#include "earshot.h"
#include <cctype>
#include <cmath>
#include <thread>
#include <whisper.h>

constexpr int vad_window = 512;               // 32 ms at 16 khz
constexpr int preroll = vad_window * 10;      // audio kept from before speech starts
constexpr int hang_ms = 700;                  // silence that ends an utterance
constexpr int min_chunk = whisper_rate * 3 / 2;

enum class VadMode { Silero, Energy, Off };

struct Transcriber;

// one channel's capture to text loop
struct Worker {
    Transcriber *t;
    int channel;
    Ring *ring;
    whisper_vad_context *vad = nullptr;
    std::thread thread;
    std::atomic<bool> reset{ false };
    std::atomic<bool> pending{ false };  // inside an utterance or transcribing one
    std::string last_text;
};

struct Transcriber {
    Transcript *tr;
    Config cfg;
    VadMode mode;
    whisper_context *ctx = nullptr;
    std::mutex infer;  // one chunk at a time shares the model and the gpu
    Worker workers[channel_count];
    std::atomic<bool> stop{ false }, drain{ false };
    std::atomic<int> busy{ 0 };

    // a new model loads beside the old one so transcription never pauses
    std::thread loader;
    std::atomic<ModelLoad> load{ ModelLoad::Ready };
    std::mutex load_mu;
    std::string load_error;
};

static void quietLog(enum ggml_log_level, const char *, void *) {}

// whisper invents text on silence and music so drop the usual suspects
static bool isNoise(const std::string &text) {
    std::string s = text;
    for (size_t p; (p = s.find("\xe2\x99\xaa")) != std::string::npos;) s.erase(p, 3);
    size_t a = s.find_first_not_of(" ");
    if (a == std::string::npos) return true;
    size_t b = s.find_last_not_of(" ");
    char first = s[a], last = s[b];
    if ((first == '[' && last == ']') || (first == '(' && last == ')') || (first == '*' && last == '*')) return true;
    std::string plain;
    for (unsigned char c : s)
        if (isalnum(c) || c >= 0x80 || c == ' ') plain.push_back((char)tolower(c));
    size_t x = plain.find_first_not_of(' ');
    if (x == std::string::npos) return true;
    plain = plain.substr(x, plain.find_last_not_of(' ') - x + 1);
    static const char *phantoms[] = { "you", "thank you", "thanks for watching", "bye" };
    for (const char *p : phantoms)
        if (plain == p) return true;
    return false;
}

static void runWhisper(Worker *w, std::vector<float> pcm, double start) {
    Transcriber *t = w->t;
    if (t->cfg.normalize) normalizeChunk(&pcm);
    bool beam = t->cfg.beam > 1;
    whisper_full_params p = whisper_full_default_params(beam ? WHISPER_SAMPLING_BEAM_SEARCH : WHISPER_SAMPLING_GREEDY);
    if (beam) p.beam_search.beam_size = t->cfg.beam;
    p.n_threads = t->cfg.threads;
    p.language = t->cfg.language.c_str();
    p.no_context = true;
    p.no_timestamps = true;
    p.suppress_nst = true;
    p.print_progress = p.print_realtime = p.print_special = p.print_timestamps = false;
    if (t->cfg.context && !w->last_text.empty()) p.initial_prompt = w->last_text.c_str();

    t->busy++;
    std::string text;
    {
        std::lock_guard<std::mutex> lock(t->infer);
        if (whisper_full(t->ctx, p, pcm.data(), (int)pcm.size()) == 0) {
            for (int i = 0; i < whisper_full_n_segments(t->ctx); i++) {
                if (whisper_full_get_segment_no_speech_prob(t->ctx, i) > 0.6f) continue;
                std::string seg = whisper_full_get_segment_text(t->ctx, i);
                if (!isNoise(seg)) text += seg;
            }
        }
    }
    t->busy--;
    size_t a = text.find_first_not_of(' ');
    if (a == std::string::npos) return;
    text = text.substr(a);
    w->last_text = text;
    transcriptAdd(t->tr, { start, text, w->channel, start + (double)pcm.size() / whisper_rate });
}

static void worker(Worker *w) {
    Transcriber *t = w->t;
    std::vector<int16_t> frames(capture_rate / 10 * capture_channels);
    // a mic is mono in practice so centre extraction would only cost time
    Config fcfg = t->cfg;
    if (w->channel == You) fcfg.focus = false;
    Frontend *fe = frontendCreate(fcfg);
    // start from the oldest audio so speech captured while the model loaded is kept
    uint64_t cursor = 0;
    std::vector<float> fresh, pcm;
    size_t fresh_used = 0;
    float noise = 0.01f;
    bool speaking = false;
    int silence_ms = 0;
    double fresh_time = 0;  // wall time of the first fresh sample from its ring position
    double start = 0;       // wall time of the first pcm sample
    const size_t max_samples = (size_t)(t->cfg.chunk * whisper_rate);

    while (!t->stop) {
        if (w->reset.exchange(false)) {
            frontendDestroy(fe);
            fe = frontendCreate(fcfg);
            cursor = ringTotal(w->ring);
            fresh.clear();
            fresh_used = 0;
            pcm.clear();
            speaking = false;
            w->pending = false;
            if (w->vad) whisper_vad_reset_state(w->vad);
        }
        uint64_t total = ringTotal(w->ring), from = cursor;
        size_t n = ringRead(w->ring, &from, frames.data(), capture_rate / 10);
        if (n == 0) {
            if (t->drain) break;
            sleepMs(30);
            continue;
        }
        if (fresh.empty() || from != cursor) {
            fresh.clear();
            fresh_used = 0;
            fresh_time = nowSeconds() - (double)(total - from) / capture_rate;
        }
        cursor = from + n;
        frontendProcess(fe, frames.data(), n, &fresh);

        size_t windows = (fresh.size() - fresh_used) / vad_window;
        if (windows == 0) continue;
        const float *probs = nullptr;
        if (t->mode == VadMode::Silero &&
            whisper_vad_detect_speech_no_reset(w->vad, fresh.data() + fresh_used, (int)(windows * vad_window)) &&
            whisper_vad_n_probs(w->vad) >= (int)windows)
            probs = whisper_vad_probs(w->vad);

        for (size_t k = 0; k < windows; k++) {
            const float *win = fresh.data() + fresh_used + k * vad_window;
            if (pcm.empty()) start = fresh_time + (double)(fresh_used + k * vad_window) / whisper_rate;
            pcm.insert(pcm.end(), win, win + vad_window);

            if (t->mode == VadMode::Off) {
                if (pcm.size() >= max_samples) {
                    runWhisper(w, pcm, start);
                    pcm.clear();
                }
                continue;
            }

            bool voiced;
            if (probs) {
                voiced = probs[k] > t->cfg.vad_threshold;
            } else {
                float e = 0;
                for (int i = 0; i < vad_window; i++) e += win[i] * win[i];
                float rms = sqrtf(e / vad_window);
                noise = rms < noise ? rms : noise * 0.995f + rms * 0.005f;
                voiced = rms > fmaxf(0.004f, noise * 3.0f);
            }

            if (!speaking) {
                if (voiced) {
                    speaking = true;
                    w->pending = true;
                    silence_ms = 0;
                } else if (pcm.size() > preroll) {
                    start += (double)(pcm.size() - preroll) / whisper_rate;
                    pcm.erase(pcm.begin(), pcm.end() - preroll);
                }
                continue;
            }
            silence_ms = voiced ? 0 : silence_ms + vad_window * 1000 / whisper_rate;
            bool ended = silence_ms >= hang_ms && pcm.size() >= (size_t)min_chunk;
            if (ended || pcm.size() >= max_samples) {
                runWhisper(w, pcm, start);
                pcm.clear();
                speaking = !ended;
                w->pending = speaking;
                silence_ms = 0;
                if (ended && w->vad) whisper_vad_reset_state(w->vad);
            }
        }
        fresh_used += windows * vad_window;
        if (fresh_used > (size_t)whisper_rate) {
            fresh.erase(fresh.begin(), fresh.begin() + fresh_used);
            fresh_time += (double)fresh_used / whisper_rate;
            fresh_used = 0;
        }
    }
    // finish the utterance in progress so quitting does not drop it
    if ((speaking || t->mode == VadMode::Off) && pcm.size() >= (size_t)whisper_rate / 2) runWhisper(w, pcm, start);
    frontendDestroy(fe);
}

static whisper_context *loadModel(const Config &cfg, const std::string &model, std::string *err) {
    whisper_context_params cp = whisper_context_default_params();
    cp.use_gpu = cfg.gpu;
    cp.gpu_device = cfg.gpu_device;
    std::string path = modelPath(model);
    whisper_context *ctx = whisper_init_from_file_with_params(path.c_str(), cp);
    if (!ctx) {
        bool named = model.find('/') == std::string::npos && model.find('\\') == std::string::npos;
        *err = "cannot load whisper model " + path + (named ? "\nget one with: earshot --download " + model : "");
    }
    return ctx;
}

Transcriber *transcriberStart(Ring *rings[channel_count], Transcript *tr, const Config &cfg, std::string *err, std::string *note) {
    whisper_log_set(quietLog, nullptr);
    whisper_context *ctx = loadModel(cfg, cfg.model, err);
    if (!ctx) return nullptr;
    Transcriber *t = new Transcriber();
    t->tr = tr;
    t->cfg = cfg;
    t->ctx = ctx;
    t->mode = cfg.vad == "off" ? VadMode::Off : cfg.vad == "energy" ? VadMode::Energy : VadMode::Silero;
    for (int ch = 0; ch < channel_count; ch++) {
        Worker *w = &t->workers[ch];
        w->t = t;
        w->channel = ch;
        w->ring = rings[ch];
        if (t->mode != VadMode::Silero) continue;
        // silero keeps state between windows so each channel needs its own
        whisper_vad_context_params vp = whisper_vad_default_context_params();
        vp.n_threads = 1;
        vp.use_gpu = false;
        w->vad = whisper_vad_init_from_file_with_params(modelPath("silero-v5.1.2").c_str(), vp);
        if (!w->vad) {
            t->mode = VadMode::Energy;
            *note = "silero vad model missing so using energy vad (earshot --download silero)";
        }
    }
    for (Worker &w : t->workers) w.thread = std::thread(worker, &w);
    return t;
}

void transcriberReset(Transcriber *t, int channel) {
    t->workers[channel].reset = true;
}

bool transcriberBusy(Transcriber *t) {
    return t->busy > 0;
}

bool transcriberPending(Transcriber *t, int channel) {
    return t->workers[channel].pending;
}

// loads model on a thread and swaps it in between chunks, keeping the old one if it fails
bool transcriberSetModel(Transcriber *t, const std::string &model) {
    if (t->load == ModelLoad::Loading) return false;
    if (t->loader.joinable()) t->loader.join();
    t->load = ModelLoad::Loading;
    t->loader = std::thread([t, model] {
        std::string err;
        whisper_context *ctx = loadModel(t->cfg, model, &err);
        if (!ctx) {
            std::lock_guard<std::mutex> lock(t->load_mu);
            t->load_error = err;
            t->load = ModelLoad::Failed;
            return;
        }
        whisper_context *old;
        {
            std::lock_guard<std::mutex> lock(t->infer);
            old = t->ctx;
            t->ctx = ctx;
        }
        whisper_free(old);
        t->load = ModelLoad::Ready;
    });
    return true;
}

// a failure is reported once and then reads as ready again with the old model
ModelLoad transcriberModelState(Transcriber *t, std::string *error) {
    ModelLoad state = t->load;
    if (state == ModelLoad::Failed) {
        std::lock_guard<std::mutex> lock(t->load_mu);
        *error = t->load_error;
        t->load = ModelLoad::Ready;
    }
    return state;
}

static void finish(Transcriber *t) {
    if (t->loader.joinable()) t->loader.join();
    for (Worker &w : t->workers) {
        w.thread.join();
        if (w.vad) whisper_vad_free(w.vad);
    }
    whisper_free(t->ctx);
    delete t;
}

void transcriberStop(Transcriber *t) {
    t->stop = true;
    finish(t);
}

// transcribes everything left in the rings before stopping
void transcriberDrain(Transcriber *t) {
    t->drain = true;
    finish(t);
}
