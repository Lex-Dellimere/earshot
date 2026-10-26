#include "earshot.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <thread>
#include <ggml-backend.h>
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
    std::atomic<bool> reconfigure{ false };
    Config cfg;  // the worker's own copy so settings can change while it runs
    std::string last_text;
};

struct Transcriber {
    Transcript *tr;
    std::mutex cfg_mu;
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

// mean token probability a one or two word line needs; real short replies scored 0.5 to 0.7 in tests
constexpr float short_min_p = 0.35f;

// whisper makes up short filler like I or Oh from noise and is unsure when it does
static bool unsure(whisper_context *ctx, int seg, const std::string &text) {
    if (plainWords(text).size() > 2) return false;
    whisper_token eot = whisper_token_eot(ctx);
    float sum = 0;
    int n = 0;
    for (int j = 0; j < whisper_full_n_tokens(ctx, seg); j++) {
        whisper_token_data d = whisper_full_get_token_data(ctx, seg, j);
        if (d.id >= eot) continue;
        sum += d.p;
        n++;
    }
    return n == 0 || sum / n < short_min_p;
}

// over music after speech whisper tends to hand its prompt back instead of hearing anything new
static bool echoesPrompt(const std::string &text, const std::string &prompt) {
    std::vector<std::string> mine = plainWords(text), pool = plainWords(prompt);
    if (mine.size() < 3) return false;
    size_t found = 0;
    for (const std::string &w : mine) {
        for (std::string &p : pool) {
            if (p == w) {
                p.clear();
                found++;
                break;
            }
        }
    }
    return found * 5 >= mine.size() * 4;
}

static std::string decode(Transcriber *t, const Config &c, const std::vector<float> &pcm, const char *prompt) {
    bool beam = c.beam > 1;
    whisper_full_params p = whisper_full_default_params(beam ? WHISPER_SAMPLING_BEAM_SEARCH : WHISPER_SAMPLING_GREEDY);
    if (beam) p.beam_search.beam_size = c.beam;
    p.n_threads = c.threads;
    p.language = c.language.c_str();
    p.no_context = true;
    p.no_timestamps = true;
    p.suppress_nst = true;
    p.print_progress = p.print_realtime = p.print_special = p.print_timestamps = false;
    p.initial_prompt = prompt;
    std::string text;
    std::lock_guard<std::mutex> lock(t->infer);
    if (whisper_full(t->ctx, p, pcm.data(), (int)pcm.size()) != 0) return "";
    for (int i = 0; i < whisper_full_n_segments(t->ctx); i++) {
        if (whisper_full_get_segment_no_speech_prob(t->ctx, i) > 0.6f) continue;
        std::string seg = whisper_full_get_segment_text(t->ctx, i);
        if (!isNoise(seg) && !unsure(t->ctx, i, seg)) text += seg;
    }
    size_t a = text.find_first_not_of(' ');
    return a == std::string::npos ? "" : text.substr(a);
}

static void runWhisper(Worker *w, std::vector<float> pcm, double start) {
    Transcriber *t = w->t;
    if (w->cfg.normalize) normalizeChunk(&pcm);
    bool prompted = w->cfg.context && !w->last_text.empty();
    t->busy++;
    std::string text = decode(t, w->cfg, pcm, prompted ? w->last_text.c_str() : nullptr);
    if (prompted && echoesPrompt(text, w->last_text)) text = decode(t, w->cfg, pcm, nullptr);
    t->busy--;
    if (text.empty()) return transcriptPartial(t->tr, w->channel, 0, "");
    w->last_text = text;
    transcriptAdd(t->tr, { start, text, w->channel, start + (double)pcm.size() / whisper_rate });
}

static void worker(Worker *w) {
    Transcriber *t = w->t;
    std::vector<int16_t> frames(capture_rate / 10 * capture_channels);
    // a mic is mono in practice so centre extraction would only cost time
    auto snapshot = [w, t] {
        std::lock_guard<std::mutex> lock(t->cfg_mu);
        w->cfg = t->cfg;
        if (w->channel == You) w->cfg.focus = false;
    };
    snapshot();
    // whisper hears the audio as it is; denoising only feeds speech detection because tests showed
    // denoised audio costs whisper accuracy while a cleaner signal helps decide when someone speaks
    Frontend *fe = nullptr, *fe_vad = nullptr;
    auto makeFrontends = [&] {
        if (fe) frontendDestroy(fe);
        if (fe_vad) frontendDestroy(fe_vad);
        Config plain = w->cfg;
        plain.denoise = false;
        fe = frontendCreate(plain);
        fe_vad = w->cfg.denoise ? frontendCreate(w->cfg) : nullptr;
    };
    makeFrontends();
    std::vector<float> clean;
    size_t clean_used = 0;
    // start from the oldest audio so speech captured while the model loaded is kept
    uint64_t cursor = 0;
    std::vector<float> fresh, pcm;
    size_t fresh_used = 0, partial_at = 0;
    float noise = 0.01f;
    bool speaking = false;
    int silence_ms = 0;
    double fresh_time = 0;  // wall time of the first fresh sample from its ring position
    double start = 0;       // wall time of the first pcm sample
    while (!t->stop) {
        if (w->reconfigure.exchange(false)) {
            snapshot();
            makeFrontends();
            clean.clear();
            clean_used = 0;
        }
        const size_t max_samples = (size_t)(w->cfg.chunk * whisper_rate);
        if (w->reset.exchange(false)) {
            makeFrontends();
            cursor = ringTotal(w->ring);
            fresh.clear();
            fresh_used = 0;
            clean.clear();
            clean_used = 0;
            pcm.clear();
            speaking = false;
            w->pending = false;
            partial_at = 0;
            transcriptPartial(t->tr, w->channel, 0, "");
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
            clean.clear();
            clean_used = 0;
            fresh_time = nowSeconds() - (double)(total - from) / capture_rate;
        }
        cursor = from + n;
        frontendProcess(fe, frames.data(), n, &fresh);
        if (fe_vad) frontendProcess(fe_vad, frames.data(), n, &clean);

        size_t windows = (fresh.size() - fresh_used) / vad_window;
        if (fe_vad) windows = std::min(windows, (clean.size() - clean_used) / vad_window);
        if (windows == 0) continue;
        const float *detect = fe_vad ? clean.data() + clean_used : fresh.data() + fresh_used;
        const float *probs = nullptr;
        if (t->mode == VadMode::Silero &&
            whisper_vad_detect_speech_no_reset(w->vad, detect, (int)(windows * vad_window)) &&
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
                voiced = probs[k] > w->cfg.vad_threshold;
            } else {
                float e = 0;
                const float *heard = detect + k * vad_window;
                for (int i = 0; i < vad_window; i++) e += heard[i] * heard[i];
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
        // a guess at the sentence so far shows while it is still being spoken
        if (speaking && w->cfg.live && pcm.size() >= (size_t)whisper_rate &&
            pcm.size() >= partial_at + (size_t)(w->cfg.live_every * whisper_rate)) {
            double behind = (double)(ringTotal(w->ring) - cursor) / capture_rate;
            if (behind < 0.5) {
                partial_at = pcm.size();
                std::vector<float> copy = pcm;
                if (w->cfg.normalize) normalizeChunk(&copy);
                bool prompted = w->cfg.context && !w->last_text.empty();
                t->busy++;
                std::string guess = decode(t, w->cfg, copy, prompted ? w->last_text.c_str() : nullptr);
                t->busy--;
                transcriptPartial(t->tr, w->channel, start, guess);
            }
        }
        if (pcm.empty()) partial_at = 0;
        fresh_used += windows * vad_window;
        if (fresh_used > (size_t)whisper_rate) {
            fresh.erase(fresh.begin(), fresh.begin() + fresh_used);
            fresh_time += (double)fresh_used / whisper_rate;
            fresh_used = 0;
        }
        if (fe_vad) {
            clean_used += windows * vad_window;
            if (clean_used > (size_t)whisper_rate) {
                clean.erase(clean.begin(), clean.begin() + clean_used);
                clean_used = 0;
            }
        }
    }
    // finish the utterance in progress so quitting does not drop it
    if ((speaking || t->mode == VadMode::Off) && pcm.size() >= (size_t)whisper_rate / 2) runWhisper(w, pcm, start);
    frontendDestroy(fe);
    if (fe_vad) frontendDestroy(fe_vad);
}

// free memory on the gpu whisper would use, counted the way whisper picks gpu_device, or -1 if unknown
static int64_t gpuFreeBytes(int index) {
    int n = 0;
    for (size_t i = 0; i < ggml_backend_dev_count(); i++) {
        ggml_backend_dev_t dev = ggml_backend_dev_get(i);
        enum ggml_backend_dev_type type = ggml_backend_dev_type(dev);
        if (type != GGML_BACKEND_DEVICE_TYPE_GPU && type != GGML_BACKEND_DEVICE_TYPE_IGPU) continue;
        if (n++ != index) continue;
        size_t free_bytes = 0, total = 0;
        ggml_backend_dev_memory(dev, &free_bytes, &total);
        return (int64_t)free_bytes;
    }
    return -1;
}

static whisper_context *loadModel(const Config &cfg, const std::string &model, std::string *err, std::string *note) {
    whisper_context_params cp = whisper_context_default_params();
    cp.use_gpu = cfg.gpu;
    cp.gpu_device = cfg.gpu_device;
    std::string path = modelPath(model);
    // a full gpu makes whisper crash while allocating, so a model that will not fit runs on the cpu
    DirEntry info;
    int64_t free_bytes = cfg.gpu ? gpuFreeBytes(cfg.gpu_device) : -1;
    if (free_bytes >= 0 && fileInfo(path, &info) && (int64_t)(info.size + info.size / 2 + (400 << 20)) > free_bytes) {
        cp.use_gpu = false;
        if (note) *note = "gpu " + std::to_string(cfg.gpu_device) + " has only " + std::to_string(free_bytes >> 20) +
                          " MB free, so whisper runs on the cpu this time";
    }
    whisper_context *ctx = whisper_init_from_file_with_params(path.c_str(), cp);
    if (!ctx) {
        bool named = model.find('/') == std::string::npos && model.find('\\') == std::string::npos;
        *err = "cannot load whisper model " + path + (named ? "\nget one with: earshot --download " + model : "");
    }
    return ctx;
}

Transcriber *transcriberStart(Ring *rings[channel_count], Transcript *tr, const Config &cfg, std::string *err, std::string *note) {
    whisper_log_set(quietLog, nullptr);
    whisper_context *ctx = loadModel(cfg, cfg.model, err, note);
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
        // the lstm state starts as uninitialised memory and only the resetting detect call clears it
        if (w->vad) whisper_vad_reset_state(w->vad);
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
        Config cfg;
        {
            std::lock_guard<std::mutex> lock(t->cfg_mu);
            cfg = t->cfg;
        }
        whisper_context *ctx = loadModel(cfg, model, &err, nullptr);
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

// settings changed while running reach each worker before its next chunk
void transcriberConfigure(Transcriber *t, const Config &cfg) {
    {
        std::lock_guard<std::mutex> lock(t->cfg_mu);
        t->cfg = cfg;
    }
    for (Worker &w : t->workers) w.reconfigure = true;
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
