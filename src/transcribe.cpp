#include "earshot.h"
#include <atomic>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <whisper.h>

#define FRAME 480               // 30 ms at 16 kHz, the VAD frame
#define PREROLL (FRAME * 10)    // audio kept before speech starts
#define HANG_MS 700             // silence that ends an utterance

struct Transcriber {
    Ring *ring;
    Transcript *tr;
    TranscribeCfg cfg;
    whisper_context *ctx;
    pthread_t thread;
    std::atomic<bool> stop, reset, busy;
};

static void quiet_log(enum ggml_log_level, const char *, void *) {}

// Whisper hallucinates on silence/music; drop the usual suspects.
static bool is_noise(const char *s) {
    while (*s == ' ') s++;
    size_t n = strlen(s);
    if (n == 0) return true;
    if ((s[0] == '[' && s[n - 1] == ']') || (s[0] == '(' && s[n - 1] == ')') ||
        (s[0] == '*' && s[n - 1] == '*'))
        return true;
    return false;
}

static void run_whisper(Transcriber *t, const std::vector<float> &pcm, double t0) {
    whisper_full_params p = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    p.n_threads = t->cfg.threads;
    p.language = t->cfg.lang;
    p.no_context = true;
    p.no_timestamps = true;
    p.suppress_nst = true;
    p.print_progress = p.print_realtime = p.print_special = p.print_timestamps = false;
    t->busy = true;
    if (whisper_full(t->ctx, p, pcm.data(), (int)pcm.size()) == 0) {
        std::string text;
        int n = whisper_full_n_segments(t->ctx);
        for (int i = 0; i < n; i++) {
            if (whisper_full_get_segment_no_speech_prob(t->ctx, i) > 0.6f) continue;
            const char *seg = whisper_full_get_segment_text(t->ctx, i);
            if (is_noise(seg)) continue;
            text += seg;
        }
        size_t a = text.find_first_not_of(' ');
        if (a != std::string::npos) transcript_add_raw(t->tr, t0, text.substr(a));
    }
    t->busy = false;
}

static void *worker(void *arg) {
    Transcriber *t = (Transcriber *)arg;
    static int16_t chunk[RATE / 10 * CHANNELS * 3];
    uint64_t cursor = ring_total(t->ring);
    std::vector<float> pcm, frame;
    float noise = 0.01f;
    bool speaking = false;
    int silence_ms = 0;
    double t0 = 0;
    const size_t max_samples = (size_t)(t->cfg.max_chunk * WHISPER_RATE);

    while (!t->stop) {
        if (t->reset.exchange(false)) {
            cursor = ring_total(t->ring);
            pcm.clear(); frame.clear();
            speaking = false; silence_ms = 0;
        }
        // Pull whole 3-frame groups so 48k -> 16k stays aligned.
        uint64_t from = cursor;
        size_t n = ring_read(t->ring, &from, chunk, sizeof chunk / sizeof(int16_t) / CHANNELS);
        n -= n % 3;
        if (n == 0) { usleep(50 * 1000); continue; }
        cursor = from + n;

        for (size_t i = 0; i < n; i += 3) {
            const int16_t *s = chunk + i * CHANNELS;
            float v = (s[0] + s[1] + s[2] + s[3] + s[4] + s[5]) / (6.0f * 32768.0f);
            frame.push_back(v);
            if (frame.size() < FRAME) continue;

            float e = 0;
            for (float x : frame) e += x * x;
            float rms = sqrtf(e / FRAME);
            // Floor follows quiet passages fast and loud ones slowly.
            noise = rms < noise ? rms : noise * 0.995f + rms * 0.005f;
            bool voiced = rms > fmaxf(0.004f, noise * 3.0f);

            pcm.insert(pcm.end(), frame.begin(), frame.end());
            frame.clear();

            if (!speaking) {
                if (voiced) {
                    speaking = true;
                    silence_ms = 0;
                    t0 = now_sec() - (double)pcm.size() / WHISPER_RATE;
                } else if (pcm.size() > PREROLL) {
                    pcm.erase(pcm.begin(), pcm.end() - PREROLL);
                }
                continue;
            }
            silence_ms = voiced ? 0 : silence_ms + 30;
            bool ended = silence_ms >= HANG_MS && pcm.size() >= WHISPER_RATE;
            if (ended || pcm.size() >= max_samples) {
                run_whisper(t, pcm, t0);
                pcm.clear();
                speaking = !ended;   // music/continuous talk: keep going
                silence_ms = 0;
                t0 = now_sec();
            }
        }
    }
    return NULL;
}

Transcriber *transcriber_start(Ring *ring, Transcript *tr, const TranscribeCfg *cfg, char *err, size_t errlen) {
    whisper_log_set(quiet_log, NULL);
    whisper_context_params cp = whisper_context_default_params();
    whisper_context *ctx = whisper_init_from_file_with_params(cfg->model, cp);
    if (!ctx) {
        snprintf(err, errlen, "could not load whisper model %s (run ./download-model.sh)", cfg->model);
        return NULL;
    }
    Transcriber *t = new Transcriber();
    t->ring = ring;
    t->tr = tr;
    t->cfg = *cfg;
    t->ctx = ctx;
    pthread_create(&t->thread, NULL, worker, t);
    return t;
}

void transcriber_reset(Transcriber *t) { t->reset = true; }
bool transcriber_busy(Transcriber *t) { return t->busy; }

void transcriber_stop(Transcriber *t) {
    t->stop = true;
    pthread_join(t->thread, NULL);
    whisper_free(t->ctx);
    delete t;
}
