// earshot shared types and module interfaces
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

constexpr int capture_rate = 48000;
constexpr int capture_channels = 2;
constexpr int whisper_rate = 16000;

// config

struct Config {
    std::string source = "system";
    std::string model = "base.en";
    std::string language = "en";
    int threads = 8;
    bool gpu = true;
    int gpu_device = 0;
    std::string vad = "silero";
    float vad_threshold = 0.35f;
    float chunk = 8;
    bool focus = true;
    float highpass = 100;
    bool normalize = true;
    int beam = 1;
    bool context = true;

    std::string out_dir = "~/Music/earshot";
    float before = 10;
    float replay = 30;
    float playback = 10;
    bool save_transcript = true;

    std::string instruction;
    std::string llm_url = "http://127.0.0.1:11434/v1/chat/completions";
    std::string llm_model = "gemma3:4b";
    float llm_every = 20;
    std::string llm_start = "ollama serve";

    bool color = true;
    std::string key_clip = "c";
    std::string key_replay = "r";
    std::string key_play = "space";
    std::string key_instruction = "i";
    std::string key_process = "p";
    std::string key_clear = "x";
    std::string key_quit = "q";
    std::string hotkey_clip = "ctrl+alt+c";
    std::string hotkey_replay = "ctrl+alt+r";
    std::string hotkey_play = "ctrl+alt+p";
};

std::string configPath();
bool configLoad(Config *cfg, const std::string &path, std::string *err);
void configSet(const std::string &path, const char *key, const std::string &value);
std::string expandPath(const std::string &path);
std::string modelPath(const std::string &name);

// ring buffer of interleaved stereo frames addressed by absolute index

struct Ring {
    std::vector<int16_t> buf;
    size_t cap = 0;
    uint64_t total = 0;
    std::mutex mu;
};

void ringInit(Ring *ring, double seconds);
void ringPush(Ring *ring, const int16_t *frames, size_t count);
uint64_t ringTotal(Ring *ring);
uint64_t ringOldest(Ring *ring);
size_t ringRead(Ring *ring, uint64_t *from, int16_t *out, size_t count);

// wav clips

struct Clip {
    FILE *file = nullptr;
    uint64_t cursor = 0;
    uint64_t frames = 0;
    double start_time = 0;
    std::string path;
};

bool clipBegin(Clip *clip, Ring *ring, const std::string &dir, double before, const std::string &tag);
void clipPump(Clip *clip, Ring *ring);
void clipEnd(Clip *clip);

// dsp turns 48k stereo into filtered 16k mono

struct Frontend;
Frontend *frontendCreate(const Config &cfg);
void frontendDestroy(Frontend *fe);
void frontendProcess(Frontend *fe, const int16_t *frames, size_t count, std::vector<float> *out);
void normalizeChunk(std::vector<float> *pcm);

// transcription

struct Line {
    double time;
    std::string text;
};

struct Transcript {
    std::mutex mu;
    std::vector<Line> raw;
    std::vector<std::string> processed;
};

struct Transcriber;
Transcriber *transcriberStart(Ring *ring, Transcript *tr, const Config &cfg, std::string *err, std::string *note);
void transcriberReset(Transcriber *t);
bool transcriberBusy(Transcriber *t);
void transcriberStop(Transcriber *t);

// llm

enum class LlmState { Off, Idle, Starting, Working, Error };

struct Llm;
Llm *llmCreate(const Config &cfg);
void llmDestroy(Llm *llm);
bool llmSubmit(Llm *llm, const std::string &instruction, const std::string &context, const std::string &text);
LlmState llmState(Llm *llm);
bool llmTakeResult(Llm *llm, std::string *text, bool *ok);

// audio is pipewire on linux and wasapi on windows

enum class SourceKind { System, Mic, App, Output, Input };

struct Source {
    SourceKind kind = SourceKind::System;
    uint64_t id = 0;
    std::string name;
    std::string label;
};

struct Audio;
Audio *audioOpen(Ring *ring);
void audioClose(Audio *audio);
std::vector<Source> audioSources(Audio *audio);
void audioStart(Audio *audio, const Source &src);
void audioTick(Audio *audio);
bool audioCurrent(Audio *audio, Source *out);
std::string audioStatus(Audio *audio);
float audioLevel(Audio *audio);
void playbackStart(Audio *audio, std::vector<int16_t> frames);
void playbackStop(Audio *audio);
double playbackRemaining(Audio *audio);

// platform

std::string homeDir();
std::string configDir();
std::string dataDir();
std::string tempDir();
void makeDirs(const std::string &path);
bool writePrivateFile(const std::string &path, const std::string &data);
void sleepMs(int ms);
double nowSeconds();

void terminalInit();
void terminalRestore();
int terminalRead(char *buf, int len);
void terminalSize(int *w, int *h);
void terminalWrite(const std::string &s);

int runProcess(const std::vector<std::string> &args, std::string *out);
struct Process;
Process *spawnProcess(const std::vector<std::string> &args);
void killProcess(Process *proc);

void platformInit(std::atomic<bool> *quit);
bool controlListen(std::string *err);
bool controlSend(const std::string &cmd);
void hotkeysStart(const std::vector<std::pair<std::string, std::string>> &binds, std::string *report);
bool pollCommand(std::string *cmd);

// tui

enum Key {
    KeyNone = -1, KeyEnter = 13, KeyEsc = 27, KeyBackspace = 127,
    KeyUp = 1000, KeyDown, KeyLeft, KeyRight, KeyPageUp, KeyPageDown,
};

struct View {
    const std::vector<Source> *sources;
    int selected;
    const Source *active;
    std::string status;
    float level;
    double clip_seconds;
    double playback_left;
    bool whisper_busy;
    std::string model;
    std::string instruction;
    bool editing;
    std::string edit;
    LlmState llm;
    Transcript *tr;
    int scroll;
    std::string message;
    bool message_error;
    std::vector<std::pair<std::string, std::string>> help;
};

void tuiSetColor(bool on);
int tuiReadKey(std::string *text);
int keyCode(const std::string &name);
void tuiDraw(const View &v);
bool sameSource(const Source &a, const Source &b);
