#include "earshot.h"

static void usage() {
    printf(
        "usage: earshot [options]\n"
        "  -c, --config PATH    config file (default %s)\n"
        "  -s, --source NAME    what you hear: system or part of an app or device name\n"
        "  -m, --mic NAME       your voice: mic, part of a device name, or off\n"
        "      --list           list sources and exit\n"
        "      --print          no tui and print the transcript to stdout\n"
        "      --file WAV       transcribe a wav file and exit\n"
        "      --set KEY=VALUE  override a config value for this run\n"
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

// runs a wav through the live pipeline as fast as the model allows
static int transcribeFile(const Config &cfg, const std::string &path) {
    std::vector<int16_t> frames;
    std::string err, note;
    if (!wavRead(path, &frames, &err)) {
        fprintf(stderr, "earshot: %s\n", err.c_str());
        return 1;
    }
    size_t count = frames.size() / capture_channels;
    Ring rings[channel_count];
    ringInit(&rings[Them], (double)count / capture_rate + 1);
    ringInit(&rings[You], 0.1);
    ringPush(&rings[Them], frames.data(), count);
    Ring *ptrs[channel_count] = { &rings[Them], &rings[You] };
    Transcript tr;
    Transcriber *t = transcriberStart(ptrs, &tr, cfg, &err, &note);
    if (!t) {
        fprintf(stderr, "earshot: %s\n", err.c_str());
        return 1;
    }
    if (!note.empty()) fprintf(stderr, "earshot: %s\n", note.c_str());
    transcriberDrain(t);
    for (const Line &l : tr.raw) printf("%s\n", l.text.c_str());
    return 0;
}

int main(int argc, char **argv) {
    std::string config_path = configPath(), source_arg, mic_arg, send_arg, download_arg, file_arg;
    std::vector<std::string> overrides;
    bool list = false, print = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        bool more = i + 1 < argc;
        if ((a == "-c" || a == "--config") && more) config_path = argv[++i];
        else if ((a == "-s" || a == "--source") && more) source_arg = argv[++i];
        else if ((a == "-m" || a == "--mic") && more) mic_arg = argv[++i];
        else if (a == "--send" && more) send_arg = argv[++i];
        else if (a == "--download" && more) download_arg = argv[++i];
        else if (a == "--file" && more) file_arg = argv[++i];
        else if (a == "--set" && more) overrides.push_back(argv[++i]);
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
    for (const std::string &o : overrides) {
        size_t eq = o.find('=');
        if (eq == std::string::npos || !configApply(&cfg, o.substr(0, eq), o.substr(eq + 1))) {
            fprintf(stderr, "earshot: bad --set %s\n", o.c_str());
            return 1;
        }
    }
    if (!source_arg.empty()) cfg.source = source_arg;
    if (!mic_arg.empty()) cfg.mic = mic_arg == "off" ? "" : mic_arg;
    cfg.out_dir = expandPath(cfg.out_dir);
    tuiSetColor(cfg.color);
    if (!file_arg.empty()) return transcribeFile(cfg, file_arg);

    if (list) return listSources();
    return runApp(cfg, config_path, print);
}
