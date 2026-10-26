#include "earshot.h"
#include <ggml-backend.h>

static void usage() {
    printf(
        "usage: earshot [options]\n"
        "  -c, --config path    config file (default %s)\n"
        "  -s, --source name    what you hear: system or part of an app or device name\n"
        "  -m, --mic name       your voice: mic, part of a device name, or off\n"
        "      --list           list sources and exit\n"
        "      --print          no tui and print the transcript to stdout\n"
        "      --file wav       transcribe a wav file and exit\n"
        "      --set key=value  override a config value for this run\n"
        "      --save           write the --set values into the config file and exit\n"
        "      --gpus           list gpus as number, kind, memory in mb and name, then exit\n"
        "      --send cmd       tell a running earshot to clip replay play process clear or quit\n"
        "      --download name  fetch a whisper model like base.en small.en large-v3-turbo or silero, unless it is already there\n",
        configPath().c_str());
}

// gpus in the order whisper counts them for gpu_device, tab separated so scripts can read it
static void quiet(enum ggml_log_level, const char *, void *) {}

static int listGpus() {
    ggml_log_set(quiet, nullptr);
    int n = 0;
    for (size_t i = 0; i < ggml_backend_dev_count(); i++) {
        ggml_backend_dev_t dev = ggml_backend_dev_get(i);
        enum ggml_backend_dev_type type = ggml_backend_dev_type(dev);
        if (type != GGML_BACKEND_DEVICE_TYPE_GPU && type != GGML_BACKEND_DEVICE_TYPE_IGPU) continue;
        size_t free_bytes = 0, total = 0;
        ggml_backend_dev_memory(dev, &free_bytes, &total);
        printf("%d\t%s\t%zu\t%s\n", n++, type == GGML_BACKEND_DEVICE_TYPE_GPU ? "gpu" : "igpu", total >> 20,
               ggml_backend_dev_description(dev));
    }
    return 0;
}

static int download(const std::string &name) {
    std::string url = name == "silero"
        ? "https://huggingface.co/ggml-org/whisper-vad/resolve/main/ggml-silero-v5.1.2.bin"
        : "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-" + name + ".bin";
    std::string path = modelPath(name == "silero" ? "silero-v5.1.2" : name);
    DirEntry have;
    if (fileInfo(path, &have) && !have.dir && have.size > 0) {
        printf("already have %s\n", path.c_str());
        return 0;
    }
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
    bool list = false, print = false, save = false, gpus = false;
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
        else if (a == "--save") save = true;
        else if (a == "--gpus") gpus = true;
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
    if (gpus) return listGpus();

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
    if (save) {
        for (const std::string &o : overrides) {
            std::string key = o.substr(0, o.find('='));
            configSet(config_path, key.c_str(), configGet(cfg, key));
        }
        printf("saved to %s\n", config_path.c_str());
        return 0;
    }
    if (!source_arg.empty()) cfg.source = source_arg;
    if (!mic_arg.empty()) cfg.mic = mic_arg == "off" ? "" : mic_arg;
    tuiSetColor(cfg.color);
    if (!file_arg.empty()) return transcribeFile(cfg, file_arg);

    if (list) return listSources();
    return runApp(cfg, config_path, print);
}
