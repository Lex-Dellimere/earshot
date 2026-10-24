// earshot — capture desktop/app audio, transcribe live, clip the last N seconds.
#pragma once
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <vector>

#define RATE 48000          // capture rate (clips are saved at this rate)
#define CHANNELS 2
#define WHISPER_RATE 16000  // whisper wants 16 kHz mono

double now_sec(void);       // wall clock, seconds since epoch
void mkdirs(const char *path);

// ---- ring.cpp: lock-protected ring of interleaved s16 stereo frames ----
// Readers address frames by absolute index (0 = first frame ever pushed).
struct Ring {
    int16_t *buf;
    size_t cap;             // frames
    uint64_t total;         // frames ever pushed
    pthread_mutex_t mu;
};
void ring_init(Ring *r, double seconds);
void ring_push(Ring *r, const int16_t *frames, size_t n);
uint64_t ring_total(Ring *r);
uint64_t ring_oldest(Ring *r);
// Copies up to n frames starting at *from (clamped forward to the oldest
// frame still held). Updates *from to where the copy started; returns count.
size_t ring_read(Ring *r, uint64_t *from, int16_t *out, size_t n);

// ---- capture.cpp: PipeWire sources (system, mic, apps, devices) ----
enum SrcKind { SRC_SYSTEM, SRC_MIC, SRC_APP, SRC_OUTPUT, SRC_INPUT };
struct Source {
    SrcKind kind;
    uint32_t id;
    uint64_t serial;
    char name[128];         // app name / node name, used for matching
    char label[160];        // what the UI shows
};
struct Capture;
Capture *capture_open(Ring *ring);
int capture_list(Capture *c, Source *out, int max);
bool capture_find(Capture *c, const char *query, Source *out);
void capture_start(Capture *c, const Source *s);
void capture_tick(Capture *c);              // re-attach to apps that restarted
bool capture_current(Capture *c, Source *out);
const char *capture_status(Capture *c);
float capture_level(Capture *c);            // peak 0..1 since last call
void capture_close(Capture *c);

// ---- clip.cpp: WAV writing from the ring ----
struct Clip {
    FILE *f;
    uint64_t cursor;        // next ring frame to write
    uint64_t frames;        // frames written
    double t0;              // wall time of the first frame
    char path[512];
};
bool clip_begin(Clip *c, Ring *r, const char *dir, double before_secs, const char *tag);
void clip_pump(Clip *c, Ring *r);
void clip_end(Clip *c);

// ---- transcript store shared between threads ----
struct Line {
    double t;               // wall time the chunk started
    std::string text;
};
struct Transcript {
    pthread_mutex_t mu;
    std::vector<Line> raw;
    std::vector<std::string> processed;
};
void transcript_add_raw(Transcript *tr, double t, const std::string &text);
void transcript_add_processed(Transcript *tr, const std::string &text);
// Writes raw lines with t >= since next to the WAV as .txt.
void transcript_dump(Transcript *tr, double since, const char *wav_path);

// ---- transcribe.cpp: whisper.cpp on a background thread ----
struct TranscribeCfg {
    const char *model;
    const char *lang;
    int threads;
    double max_chunk;       // seconds
};
struct Transcriber;
Transcriber *transcriber_start(Ring *ring, Transcript *tr, const TranscribeCfg *cfg, char *err, size_t errlen);
void transcriber_reset(Transcriber *t);     // drop buffered audio (source changed)
bool transcriber_busy(Transcriber *t);
void transcriber_stop(Transcriber *t);

// ---- llm.cpp: instruction-driven cleanup via an OpenAI-compatible API ----
struct LlmCfg {
    const char *url;
    const char *model;
    const char *api_key;    // may be NULL
};
// Applies `instruction` to `text`. `context` is the tail of earlier output.
bool llm_process(const LlmCfg *cfg, const char *instruction, const char *context,
                 const std::string &text, std::string *out);

// ---- tui.cpp: raw ANSI terminal ----
enum {
    KEY_NONE = -1, KEY_ENTER = 13, KEY_ESC = 27, KEY_BACKSPACE = 127,
    KEY_UP = 1000, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_PGUP, KEY_PGDN,
};
void tui_init(void);
void tui_shutdown(void);
int tui_key(char *utf8, size_t len);        // non-blocking; text bytes go to utf8
void tui_size(int *w, int *h);

struct View {
    Source *sources; int n_sources; int selected;
    const Source *active;
    const char *capture_status;
    float level;
    bool clipping; double clip_secs;
    bool whisper_busy; const char *model_name;
    const char *instruction;
    bool editing; const char *edit_buf;
    const char *llm_state;
    Transcript *tr;
    int scroll;                              // lines scrolled up from the bottom
    const char *message;
};
void tui_draw(const View *v);
