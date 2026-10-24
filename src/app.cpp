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

static std::string stamp() {
    time_t now = time(nullptr);
    char b[32];
    strftime(b, sizeof b, "%Y%m%d-%H%M%S", localtime(&now));
    return b;
}

static std::string clockText(double t) {
    time_t tt = (time_t)t;
    char b[16];
    strftime(b, sizeof b, "%H:%M:%S", localtime(&tt));
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
    std::string session;
    size_t written_raw = 0, printed_raw = 0, printed_processed = 0, llm_upto = 0;
    double llm_last = 0;

    std::vector<Source> sources;
    int selected = 0, scroll = 0;
    float level[channel_count] = {};
    Overlay overlay = Overlay::None;
    bool editing = false, message_error = false;
    std::string edit, message, model_name;
    double message_until = 0, last_draw = 0;

    bool start(std::string *err);
    void run();
    void stop();

    void say(const std::string &text, bool error);
    bool active(int ch, Source *out = nullptr);
    bool bothActive();
    std::string label(int ch);
    std::string lineText(const Line &l, bool time);
    void listen(const Source &s);
    void dumpTranscript(double since, const std::string &wav);
    void command(const std::string &cmd);
    void key(int key, const std::string &text);
    void runLlm(bool force);
    void takeLlmResults();
    void writeNewLines();
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

bool App::bothActive() {
    return active(Them) && active(You);
}

std::string App::label(int ch) {
    return ch == You ? cfg.you_label : cfg.them_label;
}

// a line as written to files and stdout with the speaker named when both channels are on
std::string App::lineText(const Line &l, bool time) {
    std::string s;
    if (time) s += "[" + clockText(l.time) + "] ";
    if (bothActive()) s += label(l.channel) + ": ";
    return s + l.text;
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

    session = stamp();
    if (cfg.save_transcript) {
        makeDirs(cfg.out_dir + "/transcripts");
        raw_file = fopen((cfg.out_dir + "/transcripts/" + session + ".txt").c_str(), "a");
    }
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
            if (!clipBegin(&clips[ch], &rings[ch], cfg.out_dir, cfg.before, tag)) return say("cannot write " + clips[ch].path, true);
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
            if (!clipBegin(&r, &rings[ch], cfg.out_dir, cfg.replay, tag)) return say("cannot write " + r.path, true);
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
        else runLlm(true);
    } else if (cmd == "clear") {
        std::lock_guard<std::mutex> lock(tr.mu);
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
    if (cfg.instruction.empty()) return;
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
    if (text.empty() || llmSubmit(llm, cfg.instruction, context, text)) {
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
        if (cfg.save_transcript && !processed_file)
            processed_file = fopen((cfg.out_dir + "/transcripts/" + session + "-processed.txt").c_str(), "a");
        if (processed_file) {
            fprintf(processed_file, "%s\n\n", result.c_str());
            fflush(processed_file);
        }
    }
}

void App::writeNewLines() {
    std::lock_guard<std::mutex> lock(tr.mu);
    for (; raw_file && written_raw < tr.raw.size(); written_raw++)
        if (!tr.raw[written_raw].dropped) fprintf(raw_file, "%s\n", lineText(tr.raw[written_raw], true).c_str());
    if (raw_file) fflush(raw_file);
    if (!print) return;
    for (; printed_raw < tr.raw.size(); printed_raw++)
        if (!tr.raw[printed_raw].dropped) printf("%s\n", lineText(tr.raw[printed_raw], true).c_str());
    for (; printed_processed < tr.processed.size(); printed_processed++)
        printf("» %s\n", tr.processed[printed_processed].c_str());
    fflush(stdout);
}

// most useful first because the footer shows only what fits
std::vector<std::pair<std::string, std::string>> App::help(bool wide) {
    return {
        { cfg.key_quit, "quit" }, { wide ? "↑↓ Enter" : "Tab", wide ? "source" : "sources" }, { cfg.key_clip, "clip" },
        { cfg.key_replay, "replay" }, { cfg.key_play, "play" }, { cfg.key_instruction, "instruction" },
        { cfg.key_process, "process" }, { cfg.key_clear, "clear" }, { "PgUp/Dn", "scroll" },
    };
}

void App::key(int key, const std::string &text) {
    if (editing) {
        if (key == KeyEnter) {
            cfg.instruction = edit;
            configSet(config_path, "instruction", edit);
            editing = false;
            std::lock_guard<std::mutex> lock(tr.mu);
            llm_upto = tr.raw.size() > 6 ? tr.raw.size() - 6 : 0;
        } else if (key == KeyEsc) {
            editing = false;
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
    } else {
        const std::pair<std::string, const char *> keys[] = {
            { cfg.key_clip, "clip" }, { cfg.key_replay, "replay" }, { cfg.key_play, "play" },
            { cfg.key_process, "process" }, { cfg.key_clear, "clear" }, { cfg.key_quit, "quit" },
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
    v.model = model_name;
    v.instruction = cfg.instruction;
    v.editing = editing;
    v.edit = edit;
    v.llm = cfg.instruction.empty() ? LlmState::Off : llmState(llm);
    v.tr = &tr;
    v.scroll = scroll;
    v.message = message;
    v.message_error = message_error;
    v.help = help(tuiWide());
    v.overlay = overlay;
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
    if (audio) writeNewLines();
    if (!first.empty()) {
        dumpTranscript(start, first);
        fprintf(stderr, "earshot: clip saved %s\n", first.c_str());
    }
    if (raw_file) fclose(raw_file);
    if (processed_file) fclose(processed_file);
    if (llm) llmDestroy(llm);
    if (audio) audioClose(audio);
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
