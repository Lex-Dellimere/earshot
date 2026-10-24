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

struct Transcriber {
    Ring *ring;
    Transcript *tr;
    Config cfg;
    VadMode mode;
    whisper_context *ctx = nullptr;
    whisper_vad_context *vad = nullptr;
    std::thread thread;
    std::atomic<bool> stop{ false }, reset{ false }, busy{ false };
    std::string last_text;
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

static void runWhisper(Transcriber *t, std::vector<float> pcm, double start) {
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
    if (t->cfg.context && !t->last_text.empty()) p.initial_prompt = t->last_text.c_str();

    t->busy = true;
    if (whisper_full(t->ctx, p, pcm.data(), (int)pcm.size()) == 0) {
        std::string text;
        for (int i = 0; i < whisper_full_n_segments(t->ctx); i++) {
            if (whisper_full_get_segment_no_speech_prob(t->ctx, i) > 0.6f) continue;
            std::string seg = whisper_full_get_segment_text(t->ctx, i);
            if (!isNoise(seg)) text += seg;
        }
        size_t a = text.find_first_not_of(' ');
        if (a != std::string::npos) {
            text = text.substr(a);
            t->last_text = text;
            std::lock_guard<std::mutex> lock(t->tr->mu);
            t->tr->raw.push_back({ start, text });
        }
    }
    t->busy = false;
}

static void worker(Transcriber *t) {
    static int16_t frames[capture_rate / 10 * capture_channels];
    Frontend *fe = frontendCreate(t->cfg);
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
        if (t->reset.exchange(false)) {
            frontendDestroy(fe);
            fe = frontendCreate(t->cfg);
            cursor = ringTotal(t->ring);
            fresh.clear();
            fresh_used = 0;
            pcm.clear();
            speaking = false;
            if (t->vad) whisper_vad_reset_state(t->vad);
        }
        uint64_t total = ringTotal(t->ring), from = cursor;
        size_t n = ringRead(t->ring, &from, frames, capture_rate / 10);
        if (n == 0) {
            sleepMs(30);
            continue;
        }
        if (fresh.empty() || from != cursor) {
            fresh.clear();
            fresh_used = 0;
            fresh_time = nowSeconds() - (double)(total - from) / capture_rate;
        }
        cursor = from + n;
        frontendProcess(fe, frames, n, &fresh);

        size_t windows = (fresh.size() - fresh_used) / vad_window;
        if (windows == 0) continue;
        const float *probs = nullptr;
        if (t->mode == VadMode::Silero &&
            whisper_vad_detect_speech_no_reset(t->vad, fresh.data() + fresh_used, (int)(windows * vad_window)) &&
            whisper_vad_n_probs(t->vad) >= (int)windows)
            probs = whisper_vad_probs(t->vad);

        for (size_t w = 0; w < windows; w++) {
            const float *win = fresh.data() + fresh_used + w * vad_window;
            if (pcm.empty()) start = fresh_time + (double)(fresh_used + w * vad_window) / whisper_rate;
            pcm.insert(pcm.end(), win, win + vad_window);

            if (t->mode == VadMode::Off) {
                if (pcm.size() >= max_samples) {
                    runWhisper(t, pcm, start);
                    pcm.clear();
                }
                continue;
            }

            bool voiced;
            if (probs) {
                voiced = probs[w] > t->cfg.vad_threshold;
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
                runWhisper(t, pcm, start);
                pcm.clear();
                speaking = !ended;
                silence_ms = 0;
                if (ended && t->vad) whisper_vad_reset_state(t->vad);
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
    if ((speaking || t->mode == VadMode::Off) && pcm.size() >= (size_t)whisper_rate / 2) runWhisper(t, pcm, start);
    frontendDestroy(fe);
}

Transcriber *transcriberStart(Ring *ring, Transcript *tr, const Config &cfg, std::string *err, std::string *note) {
    whisper_log_set(quietLog, nullptr);
    whisper_context_params cp = whisper_context_default_params();
    cp.use_gpu = cfg.gpu;
    cp.gpu_device = cfg.gpu_device;
    std::string path = modelPath(cfg.model);
    whisper_context *ctx = whisper_init_from_file_with_params(path.c_str(), cp);
    if (!ctx) {
        *err = "cannot load whisper model " + path + "\nget one with: earshot --download " + cfg.model;
        return nullptr;
    }
    Transcriber *t = new Transcriber();
    t->ring = ring;
    t->tr = tr;
    t->cfg = cfg;
    t->ctx = ctx;
    t->mode = cfg.vad == "off" ? VadMode::Off : cfg.vad == "energy" ? VadMode::Energy : VadMode::Silero;
    if (t->mode == VadMode::Silero) {
        whisper_vad_context_params vp = whisper_vad_default_context_params();
        vp.n_threads = 1;
        vp.use_gpu = false;
        std::string vad_path = modelPath("silero-v5.1.2");
        t->vad = whisper_vad_init_from_file_with_params(vad_path.c_str(), vp);
        if (!t->vad) {
            t->mode = VadMode::Energy;
            *note = "silero vad model missing so using energy vad (earshot --download silero)";
        }
    }
    t->thread = std::thread(worker, t);
    return t;
}

void transcriberReset(Transcriber *t) {
    t->reset = true;
}

bool transcriberBusy(Transcriber *t) {
    return t->busy;
}

void transcriberStop(Transcriber *t) {
    t->stop = true;
    t->thread.join();
    if (t->vad) whisper_vad_free(t->vad);
    whisper_free(t->ctx);
    delete t;
}
