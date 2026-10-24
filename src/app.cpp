// the interactive session: capture on both channels, transcription, ai, clips and the tui
#include "earshot.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>

static bool findSource(const std::vector<Source> &list, const std::string &query, Source *out) {
    auto lower = [](std::string s) {
        for (char &c : s) c = (char)tolower((unsigned char)c);
        return s;
    };
    std::string q = lower(query);
    for (const Source &s : list)
        if (lower(s.name) == q) return *out = s, true;
    for (const Source &s : list)
        if (lower(s.label).find(q) != std::string::npos || lower(s.name).find(q) != std::string::npos) return *out = s, true;
    return false;
}

// mics and input devices feed you and everything else feeds them
static int channelFor(const Source &s) {
    return s.kind == SourceKind::Mic || s.kind == SourceKind::Input ? You : Them;
}

// turns a source label into a short name for files
static std::string fileTag(const std::string &label) {
    std::string tag;
    for (char c : label) {
        if (isalnum((unsigned char)c)) tag.push_back(c);
        else if (!tag.empty() && tag.back() != '_') tag.push_back('_');
    }
    while (!tag.empty() && tag.back() == '_') tag.pop_back();
    return tag.empty() ? "clip" : tag.substr(0, 40);
}

static std::string timeText(const char *format) {
    time_t now = time(nullptr);
    char b[64];
    strftime(b, sizeof b, format, localtime(&now));
    return b;
}

// a small model often restates a recent result so drop exact repeats
static bool recentlySaid(const std::vector<std::string> &processed, const std::string &text) {
    auto plain = [](const std::string &s) {
        std::string out;
        for (unsigned char c : s)
            if (isalnum(c)) out.push_back((char)tolower(c));
        return out;
    };
    std::string p = plain(text);
    for (size_t i = processed.size() > 5 ? processed.size() - 5 : 0; i < processed.size(); i++)
        if (plain(processed[i]) == p) return true;
    return false;
}

struct App {
    Config cfg;
    std::string config_path;
    bool print = false;

    Ring rings[channel_count];
    Audio *audio = nullptr;
    Transcript tr;
    Transcriber *transcriber = nullptr;
    Llm *llm = nullptr;
    std::atomic<bool> quit{ false };

    Clip clips[channel_count];
    FILE *raw_file = nullptr, *processed_file = nullptr;
    std::string session, transcript_path;
    size_t written_raw = 0, printed_raw = 0, printed_processed = 0, llm_upto = 0;
    std::vector<uint64_t> ai_batch;  // ids of the lines the ai is correcting now
    double llm_last = 0;
    bool spoke[channel_count] = {};

    std::vector<Source> sources;
    std::vector<SourceRow> rows;      // the sources pane grouped into what you hear and your mic
    std::vector<Source> row_sources;  // the source behind each row
    int selected = 0, scroll = 0, max_scroll = 0;
    float level[channel_count] = {};
    Overlay overlay = Overlay::None;
    bool message_error = false;
    enum class Edit { None, Instruction, Setting, ModelPath } editing = Edit::None;
    enum class Pick { Whisper, Ai } pick = Pick::Whisper;
    std::vector<ListRow> pick_rows;
    std::vector<std::string> pick_values;  // what choosing each picker row does
    int setting = 1, pick_selected = 0;
    std::string edit, message, model_name;
    double message_until = 0, last_draw = 0;

    bool start(std::string *err);
    void run();
    void stop();

    void say(const std::string &text, bool error);
    bool active(int ch, Source *out = nullptr);
    bool labels();
    std::string label(int ch);
    bool aiOn();
    AiModel aiModel();
    std::string lineText(const Line &l, bool time);
    std::string paragraphText(const Paragraph &p);
    bool openTranscript();
    void buildRows();
    void activateRow(int index);
    void openPicker(Pick which);
    void applyPick();
    void dumpTranscript(double since, const std::string &wav);
    void command(const std::string &cmd);
    void key(int key, const std::string &text);
    void listKey(int key);
    void mouse(int key);
    std::string emptyHint();
    void applySetting(const Setting &s, const std::string &before);
    void runLlm(bool force);
    void takeLlmResults();
    void writeNewLines();
    void flushAll();
    std::vector<std::pair<std::string, std::string>> help(bool wide);
    void draw();
};

void App::say(const std::string &text, bool error) {
    message = text;
    message_error = error;
    message_until = nowSeconds() + 5;
    if (print) fprintf(stderr, "earshot: %s\n", text.c_str());
}

bool App::active(int ch, Source *out) {
    Source s;
    bool on = audioCurrent(audio, ch, &s);
    if (on && out) *out = s;
    return on;
}

// speakers are named once both channels are on or both have said something
bool App::labels() {
    return (spoke[Them] && spoke[You]) || (active(Them) && active(You));
}

std::string App::label(int ch) {
    return ch == You ? cfg.you_label : cfg.them_label;
}

bool App::aiOn() {
    return cfg.ai >= 1 && cfg.ai <= 9 && !cfg.ai_slots[cfg.ai - 1].empty() && !cfg.instruction.empty();
}

AiModel App::aiModel() {
    return aiParse(cfg.ai >= 1 && cfg.ai <= 9 ? cfg.ai_slots[cfg.ai - 1] : "", cfg.llm_url);
}

// a line as written to files and stdout with the speaker named when both channels are on
std::string App::lineText(const Line &l, bool time) {
    std::string s;
    if (time) s += "[" + clockText(l.time) + "] ";
    if (labels()) s += label(l.channel) + ": ";
    return s + l.text;
}

std::string App::paragraphText(const Paragraph &p) {
    std::string s;
    if (cfg.timestamps) s += "[" + clockText(p.start) + "] ";
    if (labels()) s += label(p.channel) + ": ";
    return s + p.text;
}

// the file is made on the first paragraph so runs that hear nothing leave nothing behind
bool App::openTranscript() {
    if (raw_file) return true;
    if (!cfg.save_transcript) return false;
    std::string dir = expandPath(cfg.transcript_dir);
    makeDirs(dir);
    raw_file = fopen(transcript_path.c_str(), "a");
    if (!raw_file) {
        say("cannot write " + transcript_path, true);
        cfg.save_transcript = false;
        return false;
    }
    std::string heard;
    for (int ch = 0; ch < channel_count; ch++) {
        Source s;
        if (active(ch, &s)) heard += (heard.empty() ? "" : "   ") + label(ch) + ": " + s.label;
    }
    fprintf(raw_file, "earshot transcript %s\n%s\n\n", timeText("%Y-%m-%d %H:%M").c_str(), heard.c_str());
    return true;
}

bool App::start(std::string *err) {
    for (Ring &r : rings) ringInit(&r, std::max({ cfg.before, cfg.replay, cfg.playback }) + 30);
    Ring *ptrs[channel_count] = { &rings[Them], &rings[You] };
    audio = audioOpen(ptrs);
    if (!audio) return *err = "cannot open the audio system", false;

    sources = audioSources(audio);
    Source them, you;
    if (!findSource(sources, cfg.source, &them)) return *err = "no source matching " + cfg.source + " (try --list)", false;
    std::string note;
    // capture first because loading the model can take seconds on a cold gpu
    audioStart(audio, Them, them);
    if (!cfg.mic.empty()) {
        if (findSource(sources, cfg.mic, &you)) audioStart(audio, You, you);
        else note = "no mic matching " + cfg.mic;
    }
    buildRows();
    for (size_t i = 0; i < rows.size(); i++)
        if (rows[i].active && rows[i].channel == Them) selected = (int)i;

    transcriber = transcriberStart(ptrs, &tr, cfg, err, &note);
    if (!transcriber) return false;
    llm = llmCreate(cfg);
    llm_last = nowSeconds();

    std::string control_err, hotkey_report;
    if (!controlListen(&control_err)) note = control_err;
    hotkeysStart({ { "clip", cfg.hotkey_clip }, { "replay", cfg.hotkey_replay }, { "play", cfg.hotkey_play } }, &hotkey_report);
    if (note.empty()) note = hotkey_report;
    platformInit(&quit);

    session = timeText("%Y-%m-%d_%H-%M");
    transcript_path = expandPath(cfg.transcript_dir) + "/" + session + "_" + fileTag(them.label) + ".txt";
    model_name = cfg.model;
    if (model_name.find_last_of("/\\") != std::string::npos) model_name = model_name.substr(model_name.find_last_of("/\\") + 1);
    if (!note.empty()) say(note, false);
    return true;
}

// the sources pane lists what you can hear and then what can be your mic, each ending with off
void App::buildRows() {
    std::string keep = selected < (int)rows.size() ? rows[selected].label + "\x01" + std::to_string(rows[selected].channel) : "";
    rows.clear();
    row_sources.clear();
    for (int ch = 0; ch < channel_count; ch++) {
        Source cur;
        bool on = active(ch, &cur);
        std::string status = on ? audioStatus(audio, ch) : "";
        rows.push_back({ ch, true, false, false, SourceKind::System, ch == Them ? "Hear  (" + label(ch) + ")" : "Mic  (" + label(ch) + ")", "" });
        row_sources.push_back({});
        bool listed = false;
        for (const Source &src : sources) {
            if (channelFor(src) != ch) continue;
            bool is = on && sameSource(cur, src);
            listed = listed || is;
            rows.push_back({ ch, false, false, is, src.kind, src.label, is ? status : "" });
            row_sources.push_back(src);
        }
        // an app that closed stays listed while earshot waits for it to come back
        if (on && !listed) {
            rows.push_back({ ch, false, false, true, cur.kind, cur.label, status });
            row_sources.push_back(cur);
        }
        rows.push_back({ ch, false, true, !on, SourceKind::System, "off", "" });
        row_sources.push_back({});
    }
    for (size_t i = 0; i < rows.size(); i++)
        if (rows[i].label + "\x01" + std::to_string(rows[i].channel) == keep) selected = (int)i;
    if (selected >= (int)rows.size()) selected = (int)rows.size() - 1;
    while (selected > 0 && rows[selected].header) selected++;
}

// picks the row's source for its channel; the off row turns the channel off
void App::activateRow(int index) {
    if (index < 0 || index >= (int)rows.size() || rows[index].header) return;
    const SourceRow &r = rows[index];
    int ch = r.channel;
    Source cur;
    bool on = active(ch, &cur);
    if (r.off) {
        if (!on) return;
        if (clips[Them].file || clips[You].file) command("clip");
        audioStop(audio, ch);
        return say(label(ch) + " off", false);
    }
    const Source &src = row_sources[index];
    if (on && sameSource(cur, src)) return say(label(ch) + " is already " + src.label, false);
    if (clips[Them].file || clips[You].file) command("clip");
    audioStart(audio, ch, src);
    transcriberReset(transcriber, ch);
    say(label(ch) + ": " + src.label, false);
}

// a word on what each whisper model is good for, matched on the start of its name
static std::string modelNote(const std::string &label) {
    struct Note {
        const char *prefix, *text;
    };
    static const Note notes[] = {
        { "tiny", "fastest, many mistakes" },
        { "base", "fast, makes the most mistakes" },
        { "small", "good balance, best without a dedicated gpu" },
        { "medium", "accurate, slow without a gpu" },
        { "large-v3-turbo", "most accurate, best on a dedicated gpu" },
        { "large", "very accurate, needs a strong gpu" },
    };
    for (const Note &n : notes)
        if (label.compare(0, strlen(n.prefix), n.prefix) == 0) return n.text;
    return "";
}

void App::openPicker(Pick which) {
    pick = which;
    pick_rows.clear();
    pick_values.clear();
    pick_selected = 0;
    if (which == Pick::Whisper) {
        for (const ModelChoice &c : modelChoices(cfg)) {
            char size[32];
            snprintf(size, sizeof size, "%5.0f MB  ", c.size / 1e6);
            bool current = modelPath(c.value) == modelPath(cfg.model);
            if (current) pick_selected = (int)pick_rows.size();
            pick_rows.push_back({ false, c.label, size + modelNote(c.label), current, false });
            pick_values.push_back(c.value);
        }
        pick_rows.push_back({ false, "Type the path to a model file...", "", false, false });
        pick_values.push_back("");
    } else {
        pick_rows.push_back({ false, "Off", "", cfg.ai == 0, false });
        pick_values.push_back("0");
        for (int n = 1; n <= 9; n++) {
            if (cfg.ai_slots[n - 1].empty()) continue;
            AiModel m = aiParse(cfg.ai_slots[n - 1], cfg.llm_url);
            std::string where = !m.gguf.empty() ? "gguf file" : m.url == cfg.llm_url ? "ollama" : m.url;
            if (cfg.ai == n) pick_selected = (int)pick_rows.size();
            pick_rows.push_back({ false, std::to_string(n) + "  " + m.label, where, cfg.ai == n, false });
            pick_values.push_back(std::to_string(n));
        }
        pick_rows.push_back({ false, "Change the slots or the instruction...", "", false, false });
        pick_values.push_back("settings");
    }
    overlay = Overlay::Picker;
}

void App::applyPick() {
    if (pick_selected < 0 || pick_selected >= (int)pick_values.size()) return;
    const std::string &value = pick_values[pick_selected];
    if (pick == Pick::Whisper) {
        if (value.empty()) {
            editing = Edit::ModelPath;
            edit = cfg.model.find('/') != std::string::npos ? cfg.model : "";
            return;
        }
        overlay = Overlay::None;
        if (modelPath(value) == modelPath(cfg.model)) return;
        if (!transcriberSetModel(transcriber, value)) return say("still loading the last model", true);
        cfg.model = value;
        configSet(config_path, "model", value);
        model_name = pick_rows[pick_selected].label;
        return say("loading whisper " + model_name + ", the old one keeps working until it is ready", false);
    }
    if (value == "settings") {
        overlay = Overlay::Settings;
        const std::vector<Setting> &list = settingsList();
        for (size_t i = 0; i < list.size(); i++)
            if (std::string(list[i].key) == "ai") setting = (int)i;
        return;
    }
    overlay = Overlay::None;
    cfg.ai = atoi(value.c_str());
    configSet(config_path, "ai", value);
    say(cfg.ai == 0 ? "ai off" : "ai " + aiModel().label, false);
}

void App::dumpTranscript(double since, const std::string &wav) {
    FILE *f = fopen((wav.substr(0, wav.size() - 4) + ".txt").c_str(), "w");
    if (!f) return;
    std::lock_guard<std::mutex> lock(tr.mu);
    for (const Line &l : tr.raw)
        if (!l.dropped && l.time >= since - 1) fprintf(f, "%s\n", lineText(l, true).c_str());
    fclose(f);
}

void App::command(const std::string &cmd) {
    if (cmd == "clip") {
        if (clips[Them].file || clips[You].file) {
            std::string first;
            double start = 0;
            for (Clip &c : clips) {
                if (!c.file) continue;
                clipPump(&c, &rings[&c - clips]);
                clipEnd(&c);
                if (first.empty()) first = c.path, start = c.start_time;
            }
            dumpTranscript(start, first);
            say("clip saved " + first, false);
            return;
        }
        bool any = false;
        for (int ch = 0; ch < channel_count; ch++) {
            Source s;
            if (!active(ch, &s)) continue;
            std::string tag = fileTag(s.label) + (ch == You ? "-mic" : "");
            if (!clipBegin(&clips[ch], &rings[ch], expandPath(cfg.clip_dir), cfg.before, tag)) return say("cannot write " + clips[ch].path, true);
            any = true;
        }
        if (any) say("clipping from " + std::to_string((int)cfg.before) + "s ago until you press clip again", false);
        else say("nothing to clip because no channel is on", true);
    } else if (cmd == "replay") {
        std::string first;
        double start = 0;
        for (int ch = 0; ch < channel_count; ch++) {
            Source s;
            if (!active(ch, &s)) continue;
            Clip r;
            std::string tag = fileTag(s.label) + (ch == You ? "-mic" : "") + "-replay";
            if (!clipBegin(&r, &rings[ch], expandPath(cfg.clip_dir), cfg.replay, tag)) return say("cannot write " + r.path, true);
            clipEnd(&r);
            if (first.empty()) first = r.path, start = r.start_time;
        }
        if (first.empty()) return say("nothing to save because no channel is on", true);
        dumpTranscript(start, first);
        say("replay saved " + first, false);
    } else if (cmd == "play") {
        if (playbackRemaining(audio) > 0) return playbackStop(audio);
        Ring *ring = active(Them) || !active(You) ? &rings[Them] : &rings[You];
        size_t count = (size_t)(cfg.playback * capture_rate);
        std::vector<int16_t> frames(count * capture_channels);
        uint64_t total = ringTotal(ring);
        uint64_t from = total > count ? total - count : 0;
        frames.resize(ringRead(ring, &from, frames.data(), count) * capture_channels);
        if (frames.empty()) return say("nothing captured yet", true);
        playbackStart(audio, std::move(frames));
    } else if (cmd == "process") {
        if (cfg.instruction.empty()) say("set an instruction first", true);
        else if (!aiOn()) say("the ai is off: press " + cfg.key_ai + " to pick a model", true);
        else runLlm(true);
    } else if (cmd == "model") {
        openPicker(Pick::Whisper);
    } else if (cmd == "ai") {
        openPicker(Pick::Ai);
    } else if (cmd == "timestamps") {
        cfg.timestamps = !cfg.timestamps;
        configSet(config_path, "timestamps", cfg.timestamps ? "true" : "false");
        say(cfg.timestamps ? "timestamps on" : "timestamps off", false);
    } else if (cmd == "folder") {
        std::string dir = expandPath(cfg.transcript_dir);
        makeDirs(dir);
        openPath(dir);
        say("opened " + shortPath(dir), false);
    } else if (cmd == "clear") {
        std::lock_guard<std::mutex> lock(tr.mu);
        // the file keeps everything said so far
        for (const Paragraph &p : transcriptParagraphs(&tr, written_raw, nowSeconds() + 3600)) {
            if (!openTranscript()) break;
            fprintf(raw_file, "%s\n\n", paragraphText(p).c_str());
        }
        tr.raw.clear();
        tr.processed.clear();
        ai_batch.clear();
        for (std::string &p : tr.partial) p.clear();
        llm_upto = written_raw = printed_raw = printed_processed = 0;
        scroll = 0;
    } else if (cmd == "quit") {
        quit = true;
    } else {
        say("unknown command " + cmd, true);
    }
}

void App::runLlm(bool force) {
    if (!aiOn()) return;
    std::string text, context;
    if (cfg.ai_mode != "notes") {
        // fix mode sends numbered lines once the newest has had a moment to gather neighbours
        if (!ai_batch.empty()) return;
        std::vector<uint64_t> ids;
        {
            std::lock_guard<std::mutex> lock(tr.mu);
            double now = nowSeconds(), oldest = now;
            size_t first = tr.raw.size();
            for (size_t i = 0; i < tr.raw.size(); i++) {
                const Line &l = tr.raw[i];
                if (l.dropped || l.ai != 0) continue;
                if (first == tr.raw.size()) first = i;
                oldest = std::min(oldest, l.added);
                if (ids.size() < 12) ids.push_back(l.id);
            }
            if (ids.empty() || (!force && now - oldest < 2 && ids.size() < 4)) return;
            for (size_t i = first > 6 ? first - 6 : 0; i < first; i++)
                if (!tr.raw[i].dropped) context += lineText(tr.raw[i], false) + "\n";
            int n = 1;
            for (Line &l : tr.raw) {
                if (std::find(ids.begin(), ids.end(), l.id) == ids.end()) continue;
                // no speaker names here because small models paste them into the text
                text += std::to_string(n++) + ". " + l.text + "\n";
                l.ai = 1;
            }
        }
        if (llmSubmit(llm, aiModel(), true, cfg.instruction, context, text)) {
            ai_batch = ids;
        } else {
            std::lock_guard<std::mutex> lock(tr.mu);
            for (Line &l : tr.raw)
                if (l.ai == 1) l.ai = 0;
        }
        return;
    }
    size_t upto;
    {
        std::lock_guard<std::mutex> lock(tr.mu);
        upto = tr.raw.size();
        if (upto <= llm_upto) return;
        for (size_t i = llm_upto; i < upto; i++)
            if (!tr.raw[i].dropped) text += lineText(tr.raw[i], false) + "\n";
        for (const std::string &p : tr.processed) context += p + "\n";
    }
    if (!force && nowSeconds() - llm_last < cfg.llm_every) return;
    if (context.size() > 1500) context = context.substr(context.size() - 1500);
    if (text.empty() || llmSubmit(llm, aiModel(), false, cfg.instruction, context, text)) {
        llm_upto = upto;
        llm_last = nowSeconds();
    }
}

void App::takeLlmResults() {
    std::string result;
    bool ok;
    while (llmTakeResult(llm, &result, &ok)) {
        if (!ai_batch.empty()) {
            // corrections replace whisper's words in place, and every line in the batch is done either way
            std::vector<std::pair<int, std::string>> fixed = ok ? aiNumberedLines(result) : std::vector<std::pair<int, std::string>>();
            std::lock_guard<std::mutex> lock(tr.mu);
            for (size_t k = 0; k < ai_batch.size(); k++) {
                for (Line &l : tr.raw) {
                    if (l.id != ai_batch[k]) continue;
                    for (const auto &[n, text] : fixed)
                        if (n == (int)k + 1 && aiCloseEnough(l.heard, text)) l.text = text;
                    l.ai = 2;
                }
            }
            ai_batch.clear();
            if (!ok) say(result, true);
            continue;
        }
        if (!ok) {
            say(result, true);
            continue;
        }
        std::lock_guard<std::mutex> lock(tr.mu);
        if (result.empty() || recentlySaid(tr.processed, result)) continue;
        tr.processed.push_back(result);
        if (cfg.save_transcript && !processed_file && openTranscript())
            processed_file = fopen((transcript_path.substr(0, transcript_path.size() - 4) + "_ai.txt").c_str(), "a");
        if (processed_file) {
            fprintf(processed_file, "%s\n\n", result.c_str());
            fflush(processed_file);
        }
    }
}

// files get whole paragraphs once they can no longer change and stdout gets lines once settled
void App::writeNewLines() {
    std::lock_guard<std::mutex> lock(tr.mu);
    double now = nowSeconds();
    for (const Line &l : tr.raw)
        if (!l.dropped) spoke[l.channel] = true;
    if (cfg.save_transcript) {
        bool pending[channel_count];
        for (int ch = 0; ch < channel_count; ch++) pending[ch] = transcriberPending(transcriber, ch);
        bool wait_ai = aiOn() && cfg.ai_mode != "notes";
        for (const Paragraph &p : transcriptParagraphs(&tr, written_raw, now, pending, wait_ai)) {
            if (!p.final) break;
            if (!openTranscript()) break;
            fprintf(raw_file, "%s\n\n", paragraphText(p).c_str());
            written_raw = p.last + 1;
        }
        if (raw_file) fflush(raw_file);
    }
    if (!print) return;
    for (; printed_raw < tr.raw.size(); printed_raw++) {
        const Line &l = tr.raw[printed_raw];
        // a you line may still turn out to be an echo of them
        if (l.channel == You && active(Them) && now - l.added < 6) break;
        if (!l.dropped) printf("%s\n", lineText(l, cfg.timestamps).c_str());
    }
    for (; printed_processed < tr.processed.size(); printed_processed++)
        printf("» %s\n", tr.processed[printed_processed].c_str());
    fflush(stdout);
}

// writes what is left when quitting since nothing more can arrive
void App::flushAll() {
    std::lock_guard<std::mutex> lock(tr.mu);
    for (const Line &l : tr.raw)
        if (!l.dropped) spoke[l.channel] = true;
    if (cfg.save_transcript) {
        for (const Paragraph &p : transcriptParagraphs(&tr, written_raw, nowSeconds() + 3600)) {
            if (!openTranscript()) break;
            fprintf(raw_file, "%s\n\n", paragraphText(p).c_str());
            written_raw = p.last + 1;
        }
    }
    if (print) {
        for (; printed_raw < tr.raw.size(); printed_raw++)
            if (!tr.raw[printed_raw].dropped) printf("%s\n", lineText(tr.raw[printed_raw], cfg.timestamps).c_str());
        fflush(stdout);
    }
}

// most useful first because the footer shows only what fits
std::vector<std::pair<std::string, std::string>> App::help(bool wide) {
    if (overlay == Overlay::Settings) return { { "↑↓", "choose" }, { "←→", "change" }, { "Enter", "edit" }, { "Esc", "back" } };
    if (overlay == Overlay::Picker) return { { "↑↓", "choose" }, { "Enter", "pick" }, { "Esc", "back" } };
    return {
        { cfg.key_quit, "quit" }, { wide ? "↑↓ Enter" : "Tab", wide ? "source" : "sources" }, { cfg.key_model, "whisper model" },
        { cfg.key_ai, "ai model" }, { cfg.key_settings, "settings" }, { "PgUp/Dn", "scroll" }, { cfg.key_clip, "clip" },
        { cfg.key_replay, "replay" }, { cfg.key_play, "play" }, { cfg.key_instruction, "instruction" },
        { cfg.key_process, "process" }, { cfg.key_timestamps, "times" }, { cfg.key_folder, "folder" }, { cfg.key_clear, "clear" },
        { "Home/End", "top / follow" },
    };
}

// saves a changed setting and applies it now when that is possible
void App::applySetting(const Setting &s, const std::string &before) {
    std::string key = s.key;
    std::string value = configGet(cfg, key);
    if (value == before) return;
    if (key == "model" && !transcriberSetModel(transcriber, cfg.model)) {
        configApply(&cfg, key, before);
        return say("still loading the last model", true);
    }
    configSet(config_path, s.key, value);
    if (key == "source" || key == "mic") {
        int ch = key == "mic" ? You : Them;
        Source found;
        if (value.empty()) {
            audioStop(audio, ch);
        } else if (findSource(audioSources(audio), value, &found)) {
            audioStart(audio, ch, found);
            transcriberReset(transcriber, ch);
        } else {
            return say("saved, but nothing matches " + value + " right now", true);
        }
    } else if (key == "model") {
        model_name = cfg.model.substr(cfg.model.find_last_of("/\\") + 1);
    } else if (key == "color") {
        tuiSetColor(cfg.color);
    } else if (key == "transcript_dir" && !raw_file) {
        Source them;
        transcript_path = expandPath(cfg.transcript_dir) + "/" + session + "_" + fileTag(active(Them, &them) ? them.label : "earshot") + ".txt";
    } else if (key == "language" || key == "beam" || key == "vad_threshold" || key == "context" || key == "focus" ||
               key == "highpass" || key == "normalize" || key == "threads" || key == "chunk" || key == "live" || key == "live_every") {
        transcriberConfigure(transcriber, cfg);
    }
    say(s.restart ? "saved, applies the next time earshot starts" : "saved", false);
}

// up down and enter for the settings screen and the pickers, left and right change a setting
void App::listKey(int key) {
    bool settings = overlay == Overlay::Settings;
    const std::vector<Setting> &list = settingsList();
    int &sel = settings ? setting : pick_selected;
    int count = settings ? (int)list.size() : (int)pick_rows.size();
    auto header = [&](int i) { return settings && list[i].kind == Setting::Header; };
    auto move = [&](int dir, int steps) {
        for (int moved = 0; moved < steps;) {
            int next = sel + dir;
            if (next < 0 || next >= count) break;
            sel = next;
            if (!header(sel)) moved++;
        }
        while (header(sel) && sel + 1 < count) sel++;
    };
    if (key == KeyEsc || (settings && key == keyCode(cfg.key_settings))) overlay = Overlay::None;
    else if (key == 3) quit = true;
    else if (key == KeyUp || key == KeyWheelUp) move(-1, 1);
    else if (key == KeyDown || key == KeyWheelDown) move(1, 1);
    else if (key == KeyPageUp) move(-1, 10);
    else if (key == KeyPageDown) move(1, 10);
    else if (!settings && key == KeyEnter) applyPick();
    else if (settings) {
        const Setting &s = list[sel];
        std::string before = configGet(cfg, s.key);
        if ((key == KeyLeft || key == KeyRight) && settingStep(&cfg, s, key == KeyLeft ? -1 : 1)) {
            applySetting(s, before);
        } else if (key == KeyEnter) {
            if (s.kind == Setting::Text || s.kind == Setting::Model) {
                editing = Edit::Setting;
                edit = before;
            } else if (settingStep(&cfg, s, 1)) {
                applySetting(s, before);
            }
        }
    }
}

// the wheel scrolls whatever is under the pointer and a click picks what it lands on
void App::mouse(int key) {
    int x, y, index;
    Hit what;
    tuiMouse(&x, &y);
    tuiHit(x, y, &what, &index);
    bool list = overlay == Overlay::Settings || overlay == Overlay::Picker;
    if (key == KeyClick) {
        if (what == Hit::Source) {
            selected = index;
            activateRow(index);
            if (overlay == Overlay::Sources) overlay = Overlay::None;
        } else if (what == Hit::Item && overlay == Overlay::Picker) {
            pick_selected = index;
            applyPick();
        } else if (what == Hit::Item) {
            setting = index;
        } else if (what == Hit::Whisper) {
            openPicker(Pick::Whisper);
        } else if (what == Hit::Ai) {
            openPicker(Pick::Ai);
        }
        return;
    }
    int dir = key == KeyWheelUp ? -1 : 1;
    if (list) return listKey(key);
    if (what == Hit::Source) {
        int next = selected + dir;
        while (next >= 0 && next < (int)rows.size() && rows[next].header) next += dir;
        if (next >= 0 && next < (int)rows.size()) selected = next;
        return;
    }
    scroll = std::max(0, std::min(max_scroll, scroll - dir * 3));
}

void App::key(int key, const std::string &text) {
    if (editing != Edit::None) {
        if (key == KeyEnter) {
            Edit target = editing;
            editing = Edit::None;
            if (target == Edit::Setting) {
                const Setting &s = settingsList()[setting];
                std::string before = configGet(cfg, s.key);
                configApply(&cfg, s.key, edit);
                applySetting(s, before);
            } else if (target == Edit::ModelPath) {
                std::string path = expandPath(edit);
                DirEntry info;
                if (!fileInfo(path, &info) || info.dir) return say("no model file at " + edit, true);
                if (!transcriberSetModel(transcriber, path)) return say("still loading the last model", true);
                cfg.model = path;
                configSet(config_path, "model", path);
                model_name = path.substr(path.find_last_of("/\\") + 1);
                overlay = Overlay::None;
                say("loading whisper " + model_name, false);
            } else {
                cfg.instruction = edit;
                configSet(config_path, "instruction", edit);
                std::lock_guard<std::mutex> lock(tr.mu);
                llm_upto = tr.raw.size() > 6 ? tr.raw.size() - 6 : 0;
            }
        } else if (key == KeyEsc) {
            editing = Edit::None;
        } else if (key == KeyBackspace) {
            while (!edit.empty() && ((unsigned char)edit.back() & 0xC0) == 0x80) edit.pop_back();
            if (!edit.empty()) edit.pop_back();
        } else if (key == 21) {
            edit.clear();
        } else if (key >= ' ' && key < KeyUp) {
            edit += text;
        }
        return;
    }
    if (key == KeyWheelUp || key == KeyWheelDown || key == KeyClick) return mouse(key);
    if (overlay == Overlay::Help) {
        overlay = Overlay::None;
        return;
    }
    if (overlay == Overlay::Settings || overlay == Overlay::Picker) return listKey(key);
    bool wide = tuiWide();
    int page = 10;
    if (key == 3) quit = true;
    else if (key == '?') overlay = Overlay::Help;
    else if (key == KeyTab) overlay = overlay == Overlay::Sources ? Overlay::None : Overlay::Sources;
    else if (key == KeyEsc) overlay = Overlay::None;
    else if (key == KeyUp || key == KeyDown) {
        int dir = key == KeyUp ? -1 : 1, next = selected + dir;
        while (next >= 0 && next < (int)rows.size() && rows[next].header) next += dir;
        if (next >= 0 && next < (int)rows.size()) selected = next;
        if (!wide) overlay = Overlay::Sources;
    } else if (key == KeyPageUp) scroll = std::min(max_scroll, scroll + page);
    else if (key == KeyPageDown) scroll = std::max(0, scroll - page);
    else if (key == KeyHome) scroll = max_scroll;
    else if (key == KeyEnd) scroll = 0;
    else if (key == KeyEnter && (wide || overlay == Overlay::Sources)) {
        activateRow(selected);
        overlay = Overlay::None;
    } else if (key == keyCode(cfg.key_instruction)) {
        editing = Edit::Instruction;
        edit = cfg.instruction;
    } else if (key == keyCode(cfg.key_settings)) {
        overlay = Overlay::Settings;
    } else {
        const std::pair<std::string, const char *> keys[] = {
            { cfg.key_clip, "clip" }, { cfg.key_replay, "replay" }, { cfg.key_play, "play" },
            { cfg.key_process, "process" }, { cfg.key_clear, "clear" }, { cfg.key_quit, "quit" },
            { cfg.key_timestamps, "timestamps" }, { cfg.key_folder, "folder" }, { cfg.key_ai, "ai" }, { cfg.key_model, "model" },
        };
        for (const auto &[name, cmd] : keys)
            if (key == keyCode(name)) command(cmd);
    }
}

// says why the transcript is empty so a paused app or a switched off mic is obvious
std::string App::emptyHint() {
    std::string parts;
    for (int ch = 0; ch < channel_count; ch++) {
        Source s;
        if (!active(ch, &s)) continue;
        std::string status = audioStatus(audio, ch);
        std::string what = s.label + (status == "capturing" ? "" : status == "idle" ? " (idle, nothing is playing)" : " (" + status + ")");
        parts += (parts.empty() ? "" : " and ") + what;
    }
    if (parts.empty()) return "Nothing is being listened to. Pick something under Hear or Mic on the left" +
                              std::string(tuiWide() ? "." : " (press Tab).");
    return "Listening to " + parts + ". Text appears here as soon as someone speaks.";
}

void App::draw() {
    double now = nowSeconds();
    for (int ch = 0; ch < channel_count; ch++) {
        float peak = audioLevel(audio, ch);
        level[ch] = peak > level[ch] ? peak : level[ch] * 0.85f;
    }
    if (now - last_draw < 0.1) return;
    last_draw = now;
    sources = audioSources(audio);
    buildRows();
    if (now > message_until) message.clear();
    Source current[channel_count];
    View v;
    v.rows = rows;
    v.selected = selected;
    for (int ch = 0; ch < channel_count; ch++) {
        v.active[ch] = active(ch, &current[ch]) ? &current[ch] : nullptr;
        v.label[ch] = label(ch);
        v.status[ch] = audioStatus(audio, ch);
        v.level[ch] = level[ch];
    }
    v.clip_seconds = clips[Them].file || clips[You].file
        ? (double)std::max(clips[Them].frames, clips[You].frames) / capture_rate + 0.001 : 0;
    v.playback_left = playbackRemaining(audio);
    v.whisper_busy = transcriberBusy(transcriber);
    std::string load_error;
    ModelLoad load = transcriberModelState(transcriber, &load_error);
    v.model_loading = load == ModelLoad::Loading;
    if (load == ModelLoad::Failed) say(load_error, true);
    v.model = model_name;
    v.llm = aiOn() ? llmState(llm) : LlmState::Off;
    v.ai = aiOn() ? aiModel().label : "";
    v.notes = aiOn() && cfg.ai_mode == "notes";
    v.tr = &tr;
    v.scroll = scroll;
    v.empty_hint = emptyHint();
    v.message = message;
    v.message_error = message_error;
    v.editing = editing != Edit::None;
    v.edit = edit;
    v.edit_title = editing == Edit::Instruction ? "AI instruction:" : editing == Edit::ModelPath ? "Model file:" :
                   editing == Edit::Setting ? std::string(settingsList()[setting].label) + ":" : "";
    v.help = help(tuiWide());
    v.overlay = overlay;
    v.timestamps = cfg.timestamps;
    v.labels = labels();
    v.transcript_path = !cfg.save_transcript ? "not saved" : raw_file ? shortPath(transcript_path) : "saving to " + shortPath(expandPath(cfg.transcript_dir));
    if (overlay == Overlay::Settings) {
        v.list_title = "Settings";
        for (const Setting &st : settingsList())
            v.list.push_back({ st.kind == Setting::Header, st.label, st.kind == Setting::Header ? "" : settingDisplay(cfg, st), false, st.restart });
        v.list_selected = setting;
        v.list_hint = settingsList()[setting].help;
    } else if (overlay == Overlay::Picker) {
        v.list_title = pick == Pick::Whisper ? "Whisper model" : "AI model";
        v.list = pick_rows;
        v.list_selected = pick_selected;
        v.list_hint = pick == Pick::Whisper
            ? "Bigger models make fewer mistakes but need more time per chunk. The new model loads in the background and takes over when ready."
            : "The AI corrects misheard words in the transcript. Slots are set in settings: an ollama name, a name @ url, or a .gguf file.";
    }
    max_scroll = tuiDraw(v);
    if (scroll > max_scroll) scroll = max_scroll;
}

void App::run() {
    if (print) fprintf(stderr, "earshot: listening\n");
    else terminalInit();
    while (!quit) {
        std::string cmd;
        while (pollCommand(&cmd)) command(cmd);
        audioTick(audio);
        for (int ch = 0; ch < channel_count; ch++)
            if (clips[ch].file) clipPump(&clips[ch], &rings[ch]);
        runLlm(false);
        takeLlmResults();
        writeNewLines();
        if (print) {
            sleepMs(100);
            continue;
        }
        std::string text;
        for (int k; (k = tuiReadKey(&text)) != KeyNone;) key(k, text);
        draw();
        sleepMs(20);
    }
    if (!print) terminalRestore();
}

void App::stop() {
    std::string first;
    double start = 0;
    for (int ch = 0; ch < channel_count; ch++) {
        if (!clips[ch].file) continue;
        clipPump(&clips[ch], &rings[ch]);
        clipEnd(&clips[ch]);
        if (first.empty()) first = clips[ch].path, start = clips[ch].start_time;
    }
    if (transcriber) transcriberStop(transcriber);
    if (audio) flushAll();
    if (!first.empty()) {
        dumpTranscript(start, first);
        fprintf(stderr, "earshot: clip saved %s\n", first.c_str());
    }
    if (raw_file) fclose(raw_file);
    if (processed_file) fclose(processed_file);
    if (llm) llmDestroy(llm);
    if (audio) audioClose(audio);
    platformShutdown();
}

int runApp(const Config &cfg, const std::string &config_path, bool print) {
    App *app = new App();
    app->cfg = cfg;
    app->config_path = config_path;
    app->print = print;
    std::string err;
    int code = 0;
    if (app->start(&err)) {
        app->run();
    } else {
        fprintf(stderr, "earshot: %s\n", err.c_str());
        code = 1;
    }
    app->stop();
    delete app;
    return code;
}

int listSources() {
    Ring rings[channel_count];
    Ring *ptrs[channel_count] = { &rings[Them], &rings[You] };
    for (Ring &r : rings) ringInit(&r, 1);
    Audio *audio = audioOpen(ptrs);
    if (!audio) {
        fprintf(stderr, "earshot: cannot open the audio system\n");
        return 1;
    }
    for (const Source &s : audioSources(audio))
        printf("%-4s %s\n", s.kind == SourceKind::App ? "app" : channelFor(s) == You ? "you" : "", s.label.c_str());
    audioClose(audio);
    return 0;
}
