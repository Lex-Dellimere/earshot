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

// them is what you hear from an app or the system and you is your mic
enum Channel { Them = 0, You = 1 };
constexpr int channel_count = 2;

// config

constexpr const char *default_instruction =
    "Fix words that were clearly misheard and add punctuation. Keep the speaker's own wording and the speaker names.";

struct Config {
    std::string source = "system";
    std::string mic = "mic";
    std::string them_label = "Them";
    std::string you_label = "You";
    std::string model = "base.en";
    std::string model_paths;
    std::string language = "en";
    int threads = 8;
    bool gpu = true;
    int gpu_device = 0;
    std::string vad = "silero";
    float vad_threshold = 0.35f;
    float chunk = 8;
    bool focus = false;
    float highpass = 100;
    bool denoise = true;
    bool limiter = false;
    bool normalize = true;
    int beam = 1;
    bool context = true;
    bool live = true;
    float live_every = 1;

    std::string transcript_dir = "~/Documents/earshot";
    bool save_transcript = true;
    bool timestamps = false;
    std::string clip_dir = "~/Music/earshot";
    float before = 10;
    float replay = 30;
    float playback = 10;

    int ai = 1;
    std::string ai_slots[9] = { "gemma3:4b", "qwen2.5:3b", "llama3.2" };
    std::string instruction = default_instruction;
    std::string llm_url = "http://127.0.0.1:11434/v1/chat/completions";
    std::string ai_mode = "fix";
    float llm_every = 20;
    std::string llm_start = "ollama serve";

    bool color = true;
    std::string key_clip = "c";
    std::string key_replay = "r";
    std::string key_play = "space";
    std::string key_instruction = "i";
    std::string key_ai = "a";
    std::string key_model = "w";
    std::string key_settings = "s";
    std::string key_process = "p";
    std::string key_clear = "x";
    std::string key_timestamps = "t";
    std::string key_folder = "o";
    std::string key_quit = "q";
    std::string hotkey_clip = "ctrl+alt+c";
    std::string hotkey_replay = "ctrl+alt+r";
    std::string hotkey_play = "ctrl+alt+p";
};

std::string configPath();
bool configLoad(Config *cfg, const std::string &path, std::string *err);
bool configApply(Config *cfg, const std::string &key, const std::string &value);
std::string configGet(const Config &cfg, const std::string &key);
void configSet(const std::string &path, const char *key, const std::string &value);
bool configSave(const Config &cfg, const std::string &path);
std::string expandPath(const std::string &path);
std::string shortPath(const std::string &path);
std::string modelPath(const std::string &name);

struct ModelChoice {
    std::string value;  // what goes in the model setting: a name from the data dir or a full path
    std::string label;
    uint64_t size;
};

std::vector<ModelChoice> modelChoices(const Config &cfg);

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
bool wavRead(const std::string &path, std::vector<int16_t> *frames, std::string *err);

// dsp turns 48k stereo into filtered 16k mono

struct Frontend;
Frontend *frontendCreate(const Config &cfg);
void frontendDestroy(Frontend *fe);
void frontendProcess(Frontend *fe, const int16_t *frames, size_t count, std::vector<float> *out);
void normalizeChunk(std::vector<float> *pcm);

// transcription

struct Line {
    double time;           // wall time the speech started
    std::string text;
    int channel = Them;
    double end = 0;        // wall time the speech ended
    double added = 0;      // wall time the text arrived
    bool dropped = false;  // an echo of the other channel
    uint64_t id = 0;
    std::string heard;     // whisper's text before any ai correction
    int ai = 0;            // 0 waiting for the ai, 1 sent, 2 done
};

struct Paragraph {
    int channel;
    double start, end;
    std::string text;
    size_t first, last;  // range of lines in the transcript including dropped ones
    bool final;          // nothing later can change it
};

struct Transcript {
    std::mutex mu;
    std::vector<Line> raw;
    std::vector<std::string> processed;
    uint64_t next_id = 1;
    std::string partial[channel_count];  // the sentence being spoken right now, replaced by its line
    double partial_time[channel_count] = {};
};

void transcriptAdd(Transcript *tr, Line line);
void transcriptPartial(Transcript *tr, int channel, double time, const std::string &text);
std::vector<std::string> plainWords(const std::string &s);
std::vector<Paragraph> transcriptParagraphs(const Transcript *tr, size_t from, double now, const bool *pending = nullptr,
                                            bool wait_ai = false);

struct Transcriber;
Transcriber *transcriberStart(Ring *rings[channel_count], Transcript *tr, const Config &cfg, std::string *err, std::string *note);
void transcriberReset(Transcriber *t, int channel);
bool transcriberBusy(Transcriber *t);
bool transcriberPending(Transcriber *t, int channel);
bool transcriberSetModel(Transcriber *t, const std::string &model);
void transcriberConfigure(Transcriber *t, const Config &cfg);
enum class ModelLoad { Ready, Loading, Failed };
ModelLoad transcriberModelState(Transcriber *t, std::string *error);
void transcriberStop(Transcriber *t);
void transcriberDrain(Transcriber *t);

// llm

enum class LlmState { Off, Idle, Starting, Importing, Working, Error };

// an ai slot is an ollama model name, a name @ an openai compatible url, or a path to a gguf file
struct AiModel {
    std::string name;  // what the endpoint calls it
    std::string url;
    std::string gguf;  // a local file imported into ollama on first use
    std::string label; // short name for the tui
};

AiModel aiParse(const std::string &slot, const std::string &default_url);

struct Llm;
Llm *llmCreate(const Config &cfg);
void llmDestroy(Llm *llm);
bool llmSubmit(Llm *llm, const AiModel &model, bool fix, const std::string &instruction, const std::string &context, const std::string &text);
std::vector<std::pair<int, std::string>> aiNumberedLines(const std::string &reply);
bool aiCloseEnough(const std::string &heard, const std::string &fixed);
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
Audio *audioOpen(Ring *rings[channel_count]);
void audioClose(Audio *audio);
std::vector<Source> audioSources(Audio *audio);
void audioStart(Audio *audio, int channel, const Source &src);
void audioStop(Audio *audio, int channel);
void audioTick(Audio *audio);
bool audioCurrent(Audio *audio, int channel, Source *out);
std::string audioStatus(Audio *audio, int channel);
float audioLevel(Audio *audio, int channel);
void playbackStart(Audio *audio, std::vector<int16_t> frames);
void playbackStop(Audio *audio);
double playbackRemaining(Audio *audio);

// platform

std::string homeDir();
std::string configDir();
std::string dataDir();
std::string tempDir();
void makeDirs(const std::string &path);

struct DirEntry {
    std::string name;
    uint64_t size;
    bool dir;
};

std::vector<DirEntry> listDir(const std::string &path);
bool fileInfo(const std::string &path, DirEntry *out);
bool writePrivateFile(const std::string &path, const std::string &data);
bool replaceFile(const std::string &from, const std::string &to);
void sleepMs(int ms);
int processId();
double nowSeconds();

void terminalInit();
void terminalRestore();
int terminalRead(char *buf, int len);
void terminalSize(int *w, int *h);
void terminalWrite(const std::string &s);

int runProcess(const std::vector<std::string> &args, std::string *out, const std::atomic<bool> *cancel = nullptr);
struct Process;
Process *spawnProcess(const std::vector<std::string> &args);
void killProcess(Process *proc);
void openPath(const std::string &path);

void platformInit(std::atomic<bool> *quit);
void platformShutdown();
bool controlListen(std::string *err);
bool controlSend(const std::string &cmd);
void hotkeysStart(const std::vector<std::pair<std::string, std::string>> &binds, std::string *report);
bool pollCommand(std::string *cmd);

// settings screen

struct Setting {
    enum Kind { Header, Toggle, Choice, Number, Text, Model, Slot };
    const char *key;
    const char *label;
    const char *help;
    Kind kind;
    float step = 0, min = 0, max = 0;
    bool restart = false;  // only takes effect when earshot starts
    std::vector<std::string> choices;
};

const std::vector<Setting> &settingsList();
std::string settingDisplay(const Config &cfg, const Setting &s);
bool settingStep(Config *cfg, const Setting &s, int dir);

// app

int runApp(const Config &cfg, const std::string &config_path, bool print);
int listSources();

// tui

enum Key {
    KeyNone = -1, KeyTab = 9, KeyEnter = 13, KeyEsc = 27, KeyBackspace = 127,
    KeyUp = 1000, KeyDown, KeyLeft, KeyRight, KeyPageUp, KeyPageDown, KeyHome, KeyEnd, KeyDelete, KeyBackTab,
    KeyWheelUp, KeyWheelDown, KeyClick,
};

enum class Overlay { None, Sources, Help, Settings, Picker };

// one row of the sources pane: a group title, a source, or the row that turns a channel off
struct SourceRow {
    int channel;
    bool header;
    bool off;
    bool active;
    SourceKind kind;
    std::string label;
    std::string status;
};

// one row of the settings screen or a picker
struct ListRow {
    bool header;
    std::string label;
    std::string value;
    bool current;  // marked as the one in use
    bool restart;  // only applies when earshot starts
};

// what a mouse click landed on
enum class Hit { None, Source, Item, Whisper, Ai };

struct View {
    std::vector<SourceRow> rows;
    int selected;
    const Source *active[channel_count];
    std::string label[channel_count];
    std::string status[channel_count];
    float level[channel_count];
    double clip_seconds;
    double playback_left;
    bool whisper_busy;
    bool model_loading;
    std::string model;
    std::string ai;
    LlmState llm;
    bool notes;  // the ai writes notes in a pane under the transcript instead of fixing it
    Transcript *tr;
    int scroll;
    std::string empty_hint;
    std::string message;
    bool message_error;
    bool editing;  // the bottom line becomes an edit line
    std::string edit_title;
    std::string edit;
    std::vector<std::pair<std::string, std::string>> help;
    Overlay overlay;
    bool timestamps;
    bool labels;
    std::string transcript_path;
    std::string list_title;
    std::string list_hint;
    std::vector<ListRow> list;
    int list_selected;
};

void tuiSetColor(bool on);
std::string clockText(double t);
int tuiReadKey(std::string *text);
int keyCode(const std::string &name);
int tuiDraw(const View &v);  // returns how far the transcript can scroll
bool tuiHit(int x, int y, Hit *what, int *index);
void tuiMouse(int *x, int *y);
bool tuiWide();
int textWidth(const std::string &s);
void wrapText(const std::string &text, int w, std::vector<std::string> *out);
int meterCells(float peak);
bool sameSource(const Source &a, const Source &b);
