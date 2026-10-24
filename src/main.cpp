#include "earshot.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <ctime>

static void usage() {
    printf(
        "usage: earshot [options]\n"
        "  -c, --config PATH    config file (default %s)\n"
        "  -s, --source NAME    start on a source: system mic or part of an app or device name\n"
        "      --list           list sources and exit\n"
        "      --print          no tui and print the transcript to stdout\n"
        "      --send CMD       tell a running earshot to clip replay play process clear or quit\n"
        "      --download NAME  fetch a whisper model like base.en small.en large-v3-turbo or silero\n",
        configPath().c_str());
}

static int download(const std::string &name) {
    std::string url = name == "silero"
        ? "https://huggingface.co/ggml-org/whisper-vad/resolve/main/ggml-silero-v5.1.2.bin"
        : "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-" + name + ".bin";
    std::string path = modelPath(name == "silero" ? "silero-v5.1.2" : name);
    makeDirs(dataDir());
    printf("%s\n  -> %s\n", url.c_str(), path.c_str());
    if (runProcess({ "curl", "-fL", "--progress-bar", "-o", path + ".part", url }, nullptr) != 0) {
        fprintf(stderr, "download failed\n");
        remove((path + ".part").c_str());
        return 1;
    }
    remove(path.c_str());
    rename((path + ".part").c_str(), path.c_str());
    return 0;
}

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

static void dumpTranscript(Transcript *tr, double since, const std::string &wav) {
    FILE *f = fopen((wav.substr(0, wav.size() - 4) + ".txt").c_str(), "w");
    if (!f) return;
    std::lock_guard<std::mutex> lock(tr->mu);
    for (const Line &l : tr->raw)
        if (l.time >= since - 1) fprintf(f, "[%s] %s\n", clockText(l.time).c_str(), l.text.c_str());
    fclose(f);
}

int main(int argc, char **argv) {
    std::string config_path = configPath(), source_arg, send_arg, download_arg;
    bool list = false, print = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        bool more = i + 1 < argc;
        if ((a == "-c" || a == "--config") && more) config_path = argv[++i];
        else if ((a == "-s" || a == "--source") && more) source_arg = argv[++i];
        else if (a == "--send" && more) send_arg = argv[++i];
        else if (a == "--download" && more) download_arg = argv[++i];
        else if (a == "--list") list = true;
        else if (a == "--print") print = true;
        else {
            usage();
            return a == "-h" || a == "--help" ? 0 : 1;
        }
    }
    if (!send_arg.empty()) {
        if (controlSend(send_arg)) return 0;
        fprintf(stderr, "earshot is not running\n");
        return 1;
    }
    if (!download_arg.empty()) return download(download_arg);

    Config cfg;
    std::string err;
    if (!configLoad(&cfg, config_path, &err)) fprintf(stderr, "%s: %s", config_path.c_str(), err.c_str());
    if (!source_arg.empty()) cfg.source = source_arg;
    cfg.out_dir = expandPath(cfg.out_dir);
    tuiSetColor(cfg.color);

    Ring ring;
    ringInit(&ring, std::max({ cfg.before, cfg.replay, cfg.playback }) + 30);
    Audio *audio = audioOpen(&ring);
    if (!audio) {
        fprintf(stderr, "earshot: cannot open the audio system\n");
        return 1;
    }
    if (list) {
        for (const Source &s : audioSources(audio)) printf("%-4s %s\n", s.kind == SourceKind::App ? "app" : "", s.label.c_str());
        audioClose(audio);
        return 0;
    }

    Source src;
    if (!findSource(audioSources(audio), cfg.source, &src)) {
        fprintf(stderr, "earshot: no source matching %s (try --list)\n", cfg.source.c_str());
        audioClose(audio);
        return 1;
    }

    // capture first because loading the model can take seconds on a cold gpu
    audioStart(audio, src);
    Transcript tr;
    std::string note;
    Transcriber *transcriber = transcriberStart(&ring, &tr, cfg, &err, &note);
    if (!transcriber) {
        fprintf(stderr, "earshot: %s\n", err.c_str());
        audioClose(audio);
        return 1;
    }
    Llm *llm = llmCreate(cfg);

    std::string control_err, hotkey_report;
    if (!controlListen(&control_err)) note = control_err;
    hotkeysStart({ { "clip", cfg.hotkey_clip }, { "replay", cfg.hotkey_replay }, { "play", cfg.hotkey_play } }, &hotkey_report);
    if (note.empty()) note = hotkey_report;
    std::atomic<bool> quit{ false };
    platformInit(&quit);

    FILE *raw_file = nullptr, *processed_file = nullptr;
    std::string session = stamp();
    if (cfg.save_transcript) {
        makeDirs(cfg.out_dir + "/transcripts");
        raw_file = fopen((cfg.out_dir + "/transcripts/" + session + ".txt").c_str(), "a");
    }

    std::string model_name = cfg.model;
    if (model_name.find_last_of("/\\") != std::string::npos) model_name = model_name.substr(model_name.find_last_of("/\\") + 1);

    const std::vector<std::pair<int, std::string>> keys = {
        { keyCode(cfg.key_clip), "clip" }, { keyCode(cfg.key_replay), "replay" }, { keyCode(cfg.key_play), "play" },
        { keyCode(cfg.key_process), "process" }, { keyCode(cfg.key_clear), "clear" }, { keyCode(cfg.key_quit), "quit" },
    };
    const std::vector<std::pair<std::string, std::string>> help = {
        { cfg.key_quit, "quit" }, { "↑↓ Enter", "source" }, { cfg.key_clip, "clip" }, { cfg.key_replay, "replay" },
        { cfg.key_play, "play" }, { cfg.key_instruction, "instruction" }, { cfg.key_process, "process" },
        { cfg.key_clear, "clear" }, { "PgUp/Dn", "scroll" },
    };

    Clip clip;
    std::string message, edit;
    bool message_error = false, editing = false;
    double message_until = 0, llm_last = nowSeconds(), last_draw = 0;
    size_t llm_upto = 0, written_raw = 0, printed_raw = 0, printed_processed = 0;
    int selected = 0, scroll = 0;
    float level = 0;
    std::vector<Source> sources = audioSources(audio);
    for (size_t i = 0; i < sources.size(); i++)
        if (sameSource(sources[i], src)) selected = (int)i;

    auto say = [&](const std::string &text, bool error) {
        message = text;
        message_error = error;
        message_until = nowSeconds() + 5;
        if (print) fprintf(stderr, "earshot: %s\n", text.c_str());
    };
    if (!note.empty()) say(note, false);

    auto runLlm = [&](bool force) {
        if (cfg.instruction.empty()) return;
        std::string text, context;
        size_t upto;
        {
            std::lock_guard<std::mutex> lock(tr.mu);
            upto = tr.raw.size();
            if (upto <= llm_upto) return;
            for (size_t i = llm_upto; i < upto; i++) text += tr.raw[i].text + "\n";
            for (const std::string &p : tr.processed) context += p + "\n";
        }
        if (!force && nowSeconds() - llm_last < cfg.llm_every) return;
        if (context.size() > 1500) context = context.substr(context.size() - 1500);
        if (llmSubmit(llm, cfg.instruction, context, text)) {
            llm_upto = upto;
            llm_last = nowSeconds();
        }
    };

    auto command = [&](const std::string &cmd) {
        if (cmd == "clip") {
            if (clip.file) {
                clipPump(&clip, &ring);
                clipEnd(&clip);
                dumpTranscript(&tr, clip.start_time, clip.path);
                say("clip saved " + clip.path, false);
            } else if (clipBegin(&clip, &ring, cfg.out_dir, cfg.before, fileTag(src.label))) {
                say("clipping from " + std::to_string((int)cfg.before) + "s ago until you press clip again", false);
            } else {
                say("cannot write " + clip.path, true);
            }
        } else if (cmd == "replay") {
            Clip r;
            if (!clipBegin(&r, &ring, cfg.out_dir, cfg.replay, fileTag(src.label) + "-replay")) return say("cannot write " + r.path, true);
            clipEnd(&r);
            dumpTranscript(&tr, r.start_time, r.path);
            say("replay saved " + r.path, false);
        } else if (cmd == "play") {
            if (playbackRemaining(audio) > 0) return playbackStop(audio);
            size_t count = (size_t)(cfg.playback * capture_rate);
            std::vector<int16_t> frames(count * capture_channels);
            uint64_t total = ringTotal(&ring);
            uint64_t from = total > count ? total - count : 0;
            frames.resize(ringRead(&ring, &from, frames.data(), count) * capture_channels);
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
    };

    auto writeNewLines = [&]() {
        std::lock_guard<std::mutex> lock(tr.mu);
        for (; raw_file && written_raw < tr.raw.size(); written_raw++)
            fprintf(raw_file, "[%s] %s\n", clockText(tr.raw[written_raw].time).c_str(), tr.raw[written_raw].text.c_str());
        if (raw_file) fflush(raw_file);
        if (!print) return;
        for (; printed_raw < tr.raw.size(); printed_raw++)
            printf("[%s] %s\n", clockText(tr.raw[printed_raw].time).c_str(), tr.raw[printed_raw].text.c_str());
        for (; printed_processed < tr.processed.size(); printed_processed++)
            printf("» %s\n", tr.processed[printed_processed].c_str());
        fflush(stdout);
    };

    if (print) fprintf(stderr, "earshot: listening to %s\n", src.label.c_str());
    else terminalInit();

    while (!quit) {
        std::string cmd;
        while (pollCommand(&cmd)) command(cmd);
        audioTick(audio);
        if (clip.file) clipPump(&clip, &ring);
        runLlm(false);

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
        writeNewLines();
        if (print) {
            sleepMs(100);
            continue;
        }

        std::string text;
        for (int key; (key = tuiReadKey(&text)) != KeyNone;) {
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
                continue;
            }
            if (key == 3) quit = true;
            else if (key == KeyUp && selected > 0) selected--;
            else if (key == KeyDown && selected + 1 < (int)sources.size()) selected++;
            else if (key == KeyPageUp) scroll += 5;
            else if (key == KeyPageDown) scroll = scroll > 5 ? scroll - 5 : 0;
            else if (key == KeyEnter && selected < (int)sources.size()) {
                if (clip.file) command("clip");
                src = sources[selected];
                audioStart(audio, src);
                transcriberReset(transcriber);
                say("listening to " + src.label, false);
            } else if (key == keyCode(cfg.key_instruction)) {
                editing = true;
                edit = cfg.instruction;
            } else {
                for (const auto &[code, name] : keys)
                    if (key == code) command(name);
            }
        }

        double now = nowSeconds();
        float peak = audioLevel(audio);
        level = peak > level ? peak : level * 0.85f;
        if (now - last_draw >= 0.1) {
            last_draw = now;
            sources = audioSources(audio);
            if (selected >= (int)sources.size()) selected = (int)sources.size() - 1;
            if (now > message_until) message.clear();
            Source current;
            bool have = audioCurrent(audio, &current);
            View v;
            v.sources = &sources;
            v.selected = selected;
            v.active = have ? &current : nullptr;
            v.status = audioStatus(audio);
            v.level = level;
            v.clip_seconds = clip.file ? (double)clip.frames / capture_rate + 0.001 : 0;
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
            v.help = help;
            tuiDraw(v);
        }
        sleepMs(20);
    }

    if (!print) terminalRestore();
    bool clip_open = clip.file != nullptr;
    if (clip_open) {
        clipPump(&clip, &ring);
        clipEnd(&clip);
    }
    transcriberStop(transcriber);
    writeNewLines();
    if (clip_open) {
        dumpTranscript(&tr, clip.start_time, clip.path);
        fprintf(stderr, "earshot: clip saved %s\n", clip.path.c_str());
    }
    if (raw_file) fclose(raw_file);
    if (processed_file) fclose(processed_file);
    llmDestroy(llm);
    audioClose(audio);
    return 0;
}
