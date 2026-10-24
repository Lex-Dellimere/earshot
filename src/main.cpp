#include "earshot.h"
#include <atomic>
#include <ctype.h>
#include <getopt.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

double now_sec(void) {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

void mkdirs(const char *path) {
    char p[512];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') { *s = 0; mkdir(p, 0755); *s = '/'; }
    mkdir(p, 0755);
}

void transcript_add_raw(Transcript *tr, double t, const std::string &text) {
    pthread_mutex_lock(&tr->mu);
    tr->raw.push_back({ t, text });
    pthread_mutex_unlock(&tr->mu);
}

void transcript_add_processed(Transcript *tr, const std::string &text) {
    pthread_mutex_lock(&tr->mu);
    tr->processed.push_back(text);
    pthread_mutex_unlock(&tr->mu);
}

void transcript_dump(Transcript *tr, double since, const char *wav_path) {
    std::string path = wav_path;
    path.replace(path.size() - 4, 4, ".txt");
    FILE *f = fopen(path.c_str(), "w");
    if (!f) return;
    pthread_mutex_lock(&tr->mu);
    for (const Line &l : tr->raw) {
        if (l.t < since - 1) continue;
        time_t tt = (time_t)l.t;
        char ts[16];
        strftime(ts, sizeof ts, "%H:%M:%S", localtime(&tt));
        fprintf(f, "[%s] %s\n", ts, l.text.c_str());
    }
    pthread_mutex_unlock(&tr->mu);
    fclose(f);
}

// ---- state ----

static std::atomic<bool> g_quit, g_sig_replay, g_sig_clip;
static void on_signal(int sig) {
    if (sig == SIGUSR1) g_sig_replay = true;
    else if (sig == SIGUSR2) g_sig_clip = true;
    else g_quit = true;
}

struct LlmJob {
    LlmCfg cfg;
    std::string instruction, context, text;
    Transcript *tr;
    std::atomic<int> *state;    // 0 idle, 1 busy, 2 error
    std::string *last_error;
};

static void *llm_worker(void *arg) {
    LlmJob *j = (LlmJob *)arg;
    std::string out;
    if (llm_process(&j->cfg, j->instruction.c_str(), j->context.c_str(), j->text, &out)) {
        if (!out.empty()) transcript_add_processed(j->tr, out);
        j->state->store(0);
    } else {
        *j->last_error = out;
        j->state->store(2);
    }
    delete j;
    return NULL;
}

static std::string config_path(void) {
    const char *x = getenv("XDG_CONFIG_HOME");
    std::string d = x && *x ? x : std::string(getenv("HOME")) + "/.config";
    return d + "/earshot";
}

static std::string load_instruction(void) {
    FILE *f = fopen((config_path() + "/instruction.txt").c_str(), "r");
    if (!f) return "";
    char buf[2048];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == ' ')) buf[--n] = 0;
    return buf;
}

static void save_instruction(const std::string &s) {
    mkdirs(config_path().c_str());
    FILE *f = fopen((config_path() + "/instruction.txt").c_str(), "w");
    if (!f) return;
    fprintf(f, "%s\n", s.c_str());
    fclose(f);
}

static void usage(void) {
    printf(
        "usage: earshot [options]\n"
        "  -s, --source NAME     start on a source: system, mic, or part of an app/device name\n"
        "  -m, --model PATH      whisper ggml model (default ~/.local/share/earshot/ggml-base.en.bin)\n"
        "  -l, --lang CODE       spoken language, or 'auto' (default en)\n"
        "  -t, --threads N       whisper threads (default 8)\n"
        "  -c, --chunk SECS      longest audio chunk sent to whisper (default 8)\n"
        "  -b, --before SECS     seconds of audio a clip starts with (default 10)\n"
        "  -r, --replay SECS     length of an instant replay save (default 30)\n"
        "  -o, --out DIR         where clips go (default ~/Music/earshot)\n"
        "  -i, --instruction TXT LLM instruction, e.g. \"remove repeats and summarise\"\n"
        "  -u, --llm-url URL     OpenAI-compatible chat endpoint\n"
        "                        (default $EARSHOT_LLM_URL or http://localhost:11434/v1/chat/completions)\n"
        "  -M, --llm-model NAME  (default $EARSHOT_LLM_MODEL or llama3.2); key from $EARSHOT_API_KEY\n"
        "  -e, --every SECS      run the instruction this often (default 20)\n"
        "      --list            list sources and exit\n"
        "      --print           no TUI: print transcript to stdout\n"
        "signals: SIGUSR1 saves a replay, SIGUSR2 starts/stops a clip (bind to a hotkey:\n"
        "         pkill -USR1 earshot)\n");
}

int main(int argc, char **argv) {
    std::string home = getenv("HOME") ? getenv("HOME") : ".";
    std::string model = home + "/.local/share/earshot/ggml-base.en.bin";
    std::string out_dir = home + "/Music/earshot";
    const char *source = "system", *lang = "en";
    int threads = 8;
    double chunk = 8, before = 10, replay = 30, every = 20;
    const char *env_url = getenv("EARSHOT_LLM_URL"), *env_model = getenv("EARSHOT_LLM_MODEL");
    LlmCfg llm = { env_url ? env_url : "http://localhost:11434/v1/chat/completions",
                   env_model ? env_model : "llama3.2", getenv("EARSHOT_API_KEY") };
    std::string instruction = load_instruction();
    bool list = false, print = false;

    static option opts[] = {
        { "source", required_argument, 0, 's' }, { "model", required_argument, 0, 'm' },
        { "lang", required_argument, 0, 'l' }, { "threads", required_argument, 0, 't' },
        { "chunk", required_argument, 0, 'c' }, { "before", required_argument, 0, 'b' },
        { "replay", required_argument, 0, 'r' }, { "out", required_argument, 0, 'o' },
        { "instruction", required_argument, 0, 'i' }, { "llm-url", required_argument, 0, 'u' },
        { "llm-model", required_argument, 0, 'M' }, { "every", required_argument, 0, 'e' },
        { "list", no_argument, 0, 1 }, { "print", no_argument, 0, 2 },
        { "help", no_argument, 0, 'h' }, { 0, 0, 0, 0 },
    };
    for (int o; (o = getopt_long(argc, argv, "s:m:l:t:c:b:r:o:i:u:M:e:h", opts, NULL)) != -1;) {
        switch (o) {
        case 's': source = optarg; break;
        case 'm': model = optarg; break;
        case 'l': lang = optarg; break;
        case 't': threads = atoi(optarg); break;
        case 'c': chunk = atof(optarg); break;
        case 'b': before = atof(optarg); break;
        case 'r': replay = atof(optarg); break;
        case 'o': out_dir = optarg; break;
        case 'i': instruction = optarg; break;
        case 'u': llm.url = optarg; break;
        case 'M': llm.model = optarg; break;
        case 'e': every = atof(optarg); break;
        case 1: list = true; break;
        case 2: print = true; break;
        default: usage(); return o == 'h' ? 0 : 1;
        }
    }

    Ring ring;
    ring_init(&ring, (before > replay ? before : replay) + 30);
    Capture *cap = capture_open(&ring);
    if (!cap) { fprintf(stderr, "earshot: cannot connect to PipeWire\n"); return 1; }
    usleep(300 * 1000);                     // let the registry fill

    static Source sources[260];
    if (list) {
        int n = capture_list(cap, sources, 260);
        for (int i = 0; i < n; i++) printf("%-8s %s\n", sources[i].kind == SRC_APP ? "app" : "", sources[i].label);
        capture_close(cap);
        return 0;
    }

    Source src;
    if (!capture_find(cap, source, &src)) {
        fprintf(stderr, "earshot: no source matching '%s' (try --list)\n", source);
        capture_close(cap);
        return 1;
    }

    Transcript tr;
    pthread_mutex_init(&tr.mu, NULL);
    TranscribeCfg tc = { model.c_str(), lang, threads, chunk };
    char err[512];
    Transcriber *ts = transcriber_start(&ring, &tr, &tc, err, sizeof err);
    if (!ts) { fprintf(stderr, "earshot: %s\n", err); capture_close(cap); return 1; }
    capture_start(cap, &src);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);
    signal(SIGUSR1, on_signal);
    signal(SIGUSR2, on_signal);

    const char *model_name = strrchr(model.c_str(), '/');
    model_name = model_name ? model_name + 1 : model.c_str();
    if (!strncmp(model_name, "ggml-", 5)) model_name += 5;
    std::string model_short(model_name);
    if (model_short.size() > 4 && model_short.compare(model_short.size() - 4, 4, ".bin") == 0)
        model_short.resize(model_short.size() - 4);

    Clip clip = {};
    bool clipping = false;
    std::atomic<int> llm_state{ 0 };
    std::string llm_error, message, edit;
    size_t llm_upto = 0, printed_raw = 0, printed_proc = 0;
    double llm_last = now_sec(), message_until = 0;
    int selected = 0, scroll = 0;
    bool editing = false;

    auto say = [&](const std::string &m) {
        message = m;
        message_until = now_sec() + 4;
        if (print) fprintf(stderr, "earshot: %s\n", m.c_str());
    };
    auto tag = [&]() {
        std::string t;
        for (const char *p = src.name; *p; p++) t.push_back(isalnum((unsigned char)*p) ? *p : '_');
        return t.empty() ? std::string("clip") : t;
    };
    auto toggle_clip = [&]() {
        if (clipping) {
            clip_pump(&clip, &ring);
            clip_end(&clip);
            transcript_dump(&tr, clip.t0, clip.path);
            clipping = false;
            say(std::string("clip saved: ") + clip.path);
        } else if (clip_begin(&clip, &ring, out_dir.c_str(), before, tag().c_str())) {
            clipping = true;
            say("clipping (started " + std::to_string((int)before) + "s ago) — press c again to stop");
        } else {
            say(std::string("cannot write ") + clip.path);
        }
    };
    auto save_replay = [&]() {
        Clip r = {};
        if (!clip_begin(&r, &ring, out_dir.c_str(), replay, (tag() + "-replay").c_str())) {
            say(std::string("cannot write ") + r.path);
            return;
        }
        clip_end(&r);
        transcript_dump(&tr, r.t0, r.path);
        say(std::string("replay saved: ") + r.path);
    };
    auto run_llm = [&](bool force) {
        if (instruction.empty() || llm_state == 1) return;
        pthread_mutex_lock(&tr.mu);
        bool fresh = tr.raw.size() > llm_upto;
        std::string text, context;
        for (size_t i = llm_upto; i < tr.raw.size(); i++) text += tr.raw[i].text + "\n";
        for (const std::string &p : tr.processed) context += p + "\n";
        size_t upto = tr.raw.size();
        pthread_mutex_unlock(&tr.mu);
        if (!fresh || (!force && now_sec() - llm_last < every)) return;
        if (context.size() > 1500) context = context.substr(context.size() - 1500);
        llm_upto = upto;
        llm_last = now_sec();
        LlmJob *j = new LlmJob{ llm, instruction, context, text, &tr, &llm_state, &llm_error };
        llm_state = 1;
        pthread_t th;
        pthread_create(&th, NULL, llm_worker, j);
        pthread_detach(th);
    };

    {
        int n = capture_list(cap, sources, 260);
        for (int i = 0; i < n; i++)
            if (sources[i].kind == src.kind && !strcmp(sources[i].name, src.name)) { selected = i; break; }
    }
    if (!print) tui_init();
    else fprintf(stderr, "earshot: listening to %s (Ctrl-C to stop)\n", src.label);

    double last_draw = 0;
    float level = 0;
    while (!g_quit) {
        if (g_sig_replay.exchange(false)) save_replay();
        if (g_sig_clip.exchange(false)) toggle_clip();
        capture_tick(cap);
        if (clipping) clip_pump(&clip, &ring);
        run_llm(false);
        if (llm_state == 2) { say(llm_error); llm_state = 0; }

        if (print) {
            pthread_mutex_lock(&tr.mu);
            for (; printed_raw < tr.raw.size(); printed_raw++) {
                time_t tt = (time_t)tr.raw[printed_raw].t;
                char ts[16];
                strftime(ts, sizeof ts, "%H:%M:%S", localtime(&tt));
                printf("[%s] %s\n", ts, tr.raw[printed_raw].text.c_str());
            }
            for (; printed_proc < tr.processed.size(); printed_proc++)
                printf("» %s\n", tr.processed[printed_proc].c_str());
            pthread_mutex_unlock(&tr.mu);
            fflush(stdout);
            usleep(100 * 1000);
            continue;
        }

        char text[16];
        for (int k; (k = tui_key(text, sizeof text)) != KEY_NONE;) {
            int n = capture_list(cap, sources, 260);
            if (editing) {
                if (k == KEY_ENTER) {
                    instruction = edit;
                    save_instruction(instruction);
                    editing = false;
                    pthread_mutex_lock(&tr.mu);
                    llm_upto = tr.raw.size() > 6 ? tr.raw.size() - 6 : 0;   // re-run on recent lines
                    pthread_mutex_unlock(&tr.mu);
                    run_llm(true);
                } else if (k == KEY_ESC) {
                    editing = false;
                } else if (k == KEY_BACKSPACE) {
                    while (!edit.empty() && ((unsigned char)edit.back() & 0xC0) == 0x80) edit.pop_back();
                    if (!edit.empty()) edit.pop_back();
                } else if (k == 21) {               // Ctrl-U
                    edit.clear();
                } else if (k >= 0x20 && k < KEY_UP) {
                    edit += text;
                }
                continue;
            }
            switch (k) {
            case 'q': case 3: g_quit = true; break;
            case KEY_UP: case 'k': if (selected > 0) selected--; break;
            case KEY_DOWN: case 'j': if (selected < n - 1) selected++; break;
            case KEY_ENTER:
                if (selected < n) {
                    if (clipping) toggle_clip();
                    src = sources[selected];
                    capture_start(cap, &src);
                    transcriber_reset(ts);
                    say(std::string("listening to ") + src.label);
                }
                break;
            case 'c': toggle_clip(); break;
            case 'r': save_replay(); break;
            case 'i': editing = true; edit = instruction; break;
            case 'p': run_llm(true); break;
            case 'x':
                pthread_mutex_lock(&tr.mu);
                tr.raw.clear(); tr.processed.clear();
                pthread_mutex_unlock(&tr.mu);
                llm_upto = 0; scroll = 0;
                break;
            case KEY_PGUP: scroll += 5; break;
            case KEY_PGDN: scroll = scroll > 5 ? scroll - 5 : 0; break;
            }
        }

        double now = now_sec();
        float l = capture_level(cap);
        level = l > level ? l : level * 0.85f;
        if (now - last_draw >= 0.1) {
            last_draw = now;
            int n = capture_list(cap, sources, 260);
            if (selected >= n) selected = n - 1;
            Source cur;
            bool have = capture_current(cap, &cur);
            const char *ls = llm_state == 1 ? "working…" : instruction.empty() ? "off" : "idle";
            if (now > message_until) message.clear();
            View v = { sources, n, selected, have ? &cur : NULL, capture_status(cap), level,
                       clipping, clipping ? (double)clip.frames / RATE : 0,
                       transcriber_busy(ts), model_short.c_str(), instruction.c_str(),
                       editing, edit.c_str(), ls, &tr, scroll, message.c_str() };
            tui_draw(&v);
        }
        usleep(20 * 1000);
    }

    if (!print) tui_shutdown();
    if (clipping) {
        clip_pump(&clip, &ring);
        clip_end(&clip);
        transcript_dump(&tr, clip.t0, clip.path);
        fprintf(stderr, "earshot: clip saved: %s\n", clip.path);
    }
    transcriber_stop(ts);
    capture_close(cap);
    return 0;
}
