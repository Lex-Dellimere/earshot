// the interactive session: capture on both channels, transcription, ai, clips and the tui
#include "earshot.h"
#include <algorithm>
#include <cctype>
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
    double llm_last = 0;
    bool spoke[channel_count] = {};

    std::vector<Source> sources;
    int selected = 0, scroll = 0;
    float level[channel_count] = {};
    Overlay overlay = Overlay::None;
    bool editing = false, message_error = false;
    bool editing_setting = false;  // the edit line belongs to a setting rather than the instruction
    int setting = 1;
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
    void listen(const Source &s);
    void dumpTranscript(double since, const std::string &wav);
    void command(const std::string &cmd);
    void key(int key, const std::string &text);
    void settingsKey(int key);
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
    for (size_t i = 0; i < sources.size(); i++)
        if (sameSource(sources[i], them)) selected = (int)i;

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

// enter on a source sets the channel it belongs to and on the active one turns that channel off
void App::listen(const Source &s) {
    int ch = channelFor(s);
    Source cur;
    bool clipping = clips[Them].file || clips[You].file;
    if (clipping) command("clip");
    if (active(ch, &cur) && sameSource(cur, s)) {
        audioStop(audio, ch);
        say(label(ch) + " off", false);
    } else {
        audioStart(audio, ch, s);
        transcriberReset(transcriber, ch);
        say(label(ch) + ": " + s.label, false);
    }
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
        // steps through the whisper models found in the data dir and model_paths
        std::vector<ModelChoice> choices = modelChoices(cfg);
        if (choices.empty()) return say("no whisper models found in " + shortPath(dataDir()), true);
        size_t next = 0;
        for (size_t i = 0; i < choices.size(); i++)
            if (modelPath(choices[i].value) == modelPath(cfg.model)) next = (i + 1) % choices.size();
        if (!transcriberSetModel(transcriber, choices[next].value)) return say("still loading the last model", true);
        cfg.model = choices[next].value;
        configSet(config_path, "model", cfg.model);
        model_name = choices[next].label;
        say("loading whisper " + model_name, false);
    } else if (cmd == "ai") {
        // steps through the filled slots and then off
        int next = cfg.ai;
        do next = (next + 1) % 10;
        while (next != 0 && cfg.ai_slots[next - 1].empty());
        cfg.ai = next;
        configSet(config_path, "ai", std::to_string(next));
        if (next == 0) say("ai off", false);
        else say("ai " + std::to_string(next) + ": " + aiModel().label, false);
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
    if (text.empty() || llmSubmit(llm, aiModel(), cfg.instruction, context, text)) {
        llm_upto = upto;
        llm_last = nowSeconds();
    }
}

void App::takeLlmResults() {
    std::string result;
    bool ok;
    while (llmTakeResult(llm, &result, &ok)) {
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
        for (const Paragraph &p : transcriptParagraphs(&tr, written_raw, now, pending)) {
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
    if (overlay == Overlay::Settings)
        return { { "↑↓", "choose" }, { "←→", "change" }, { "Enter", "edit" }, { "Esc", "back" } };
    return {
        { cfg.key_quit, "quit" }, { wide ? "↑↓ Enter" : "Tab", wide ? "source" : "sources" }, { cfg.key_settings, "settings" },
        { cfg.key_clip, "clip" }, { cfg.key_replay, "replay" }, { cfg.key_play, "play" }, { cfg.key_ai, "ai model" },
        { cfg.key_model, "whisper model" }, { cfg.key_instruction, "instruction" }, { cfg.key_process, "process" },
        { cfg.key_timestamps, "times" }, { cfg.key_folder, "folder" }, { cfg.key_clear, "clear" }, { "PgUp/Dn", "scroll" },
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
               key == "highpass" || key == "normalize" || key == "threads" || key == "chunk") {
        transcriberConfigure(transcriber, cfg);
    }
    say(s.restart ? "saved, applies the next time earshot starts" : "saved", false);
}

void App::settingsKey(int key) {
    const std::vector<Setting> &list = settingsList();
    auto move = [&](int dir, int count) {
        for (int moved = 0; moved < count;) {
            int next = setting + dir;
            if (next < 0 || next >= (int)list.size()) break;
            setting = next;
            if (list[setting].kind != Setting::Header) moved++;
        }
        while (list[setting].kind == Setting::Header && setting + 1 < (int)list.size()) setting++;
    };
    const Setting &s = list[setting];
    std::string before = configGet(cfg, s.key);
    if (key == KeyEsc || key == keyCode(cfg.key_settings)) overlay = Overlay::None;
    else if (key == 3) quit = true;
    else if (key == KeyUp) move(-1, 1);
    else if (key == KeyDown) move(1, 1);
    else if (key == KeyPageUp) move(-1, 10);
    else if (key == KeyPageDown) move(1, 10);
    else if ((key == KeyLeft || key == KeyRight) && settingStep(&cfg, s, key == KeyLeft ? -1 : 1)) applySetting(s, before);
    else if (key == KeyEnter) {
        if (s.kind == Setting::Text || s.kind == Setting::Model) {
            editing = editing_setting = true;
            edit = before;
        } else if (settingStep(&cfg, s, 1)) {
            applySetting(s, before);
        }
    }
}

void App::key(int key, const std::string &text) {
    if (editing) {
        if (key == KeyEnter && editing_setting) {
            const Setting &s = settingsList()[setting];
            std::string before = configGet(cfg, s.key);
            configApply(&cfg, s.key, edit);
            editing = editing_setting = false;
            applySetting(s, before);
        } else if (key == KeyEnter) {
            cfg.instruction = edit;
            configSet(config_path, "instruction", edit);
            editing = false;
            std::lock_guard<std::mutex> lock(tr.mu);
            llm_upto = tr.raw.size() > 6 ? tr.raw.size() - 6 : 0;
        } else if (key == KeyEsc) {
            editing = editing_setting = false;
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
    if (overlay == Overlay::Help) {
        overlay = Overlay::None;
        return;
    }
    if (overlay == Overlay::Settings) return settingsKey(key);
    bool wide = tuiWide();
    if (key == 3) quit = true;
    else if (key == '?') overlay = Overlay::Help;
    else if (key == KeyTab) overlay = overlay == Overlay::Sources ? Overlay::None : Overlay::Sources;
    else if (key == KeyEsc) overlay = Overlay::None;
    else if (key == KeyUp || key == KeyDown) {
        if (key == KeyUp && selected > 0) selected--;
        if (key == KeyDown && selected + 1 < (int)sources.size()) selected++;
        if (!wide) overlay = Overlay::Sources;
    } else if (key == KeyPageUp) scroll += 5;
    else if (key == KeyPageDown) scroll = scroll > 5 ? scroll - 5 : 0;
    else if (key == KeyEnter && selected < (int)sources.size() && (wide || overlay == Overlay::Sources)) {
        listen(sources[selected]);
        overlay = Overlay::None;
    } else if (key == keyCode(cfg.key_instruction)) {
        editing = true;
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

void App::draw() {
    double now = nowSeconds();
    for (int ch = 0; ch < channel_count; ch++) {
        float peak = audioLevel(audio, ch);
        level[ch] = peak > level[ch] ? peak : level[ch] * 0.85f;
    }
    if (now - last_draw < 0.1) return;
    last_draw = now;
    sources = audioSources(audio);
    if (selected >= (int)sources.size()) selected = (int)sources.size() - 1;
    if (now > message_until) message.clear();
    Source current[channel_count];
    View v;
    v.sources = &sources;
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
    v.instruction = cfg.instruction;
    v.editing = editing;
    v.edit = edit;
    v.llm = aiOn() ? llmState(llm) : LlmState::Off;
    v.ai = aiOn() ? aiModel().label : "";
    v.tr = &tr;
    v.scroll = scroll;
    v.message = message;
    v.message_error = message_error;
    v.help = help(tuiWide());
    v.overlay = overlay;
    v.timestamps = cfg.timestamps;
    if (overlay == Overlay::Settings) {
        for (const Setting &st : settingsList())
            v.settings.push_back({ st.kind == Setting::Header, st.label, st.kind == Setting::Header ? "" : settingDisplay(cfg, st), st.restart });
        v.settings_selected = setting;
        v.settings_help = settingsList()[setting].help;
        v.editing_setting = editing_setting;
    }
    v.labels = labels();
    v.transcript_path = !cfg.save_transcript ? "not saved" : raw_file ? shortPath(transcript_path) : "saving to " + shortPath(expandPath(cfg.transcript_dir));
    tuiDraw(v);
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
