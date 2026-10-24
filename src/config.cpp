#include "earshot.h"
#include <cstdlib>
#include <cstring>

enum class FieldType { Text, Number, Integer, Flag };

struct Field {
    const char *key;
    FieldType type;
    void *ptr;
    const char *comment;
};

#define TEXT(k, c) { #k, FieldType::Text, &cfg->k, c }
#define NUMBER(k, c) { #k, FieldType::Number, &cfg->k, c }
#define INTEGER(k, c) { #k, FieldType::Integer, &cfg->k, c }
#define FLAG(k, c) { #k, FieldType::Flag, &cfg->k, c }

static std::vector<Field> fieldsOf(Config *cfg) {
    std::vector<Field> fields = {
    TEXT(source, "what you hear: system or part of an app or device name"),
    TEXT(mic, "your voice: mic for the default input, part of a device name, or empty for off"),
    TEXT(them_label, "names shown before each channel's lines when both are on"),
    TEXT(you_label, nullptr),
    TEXT(model, "whisper model name in the data dir or a full path to a ggml file; the w key switches while running"),
    TEXT(model_paths, "more whisper models for the w key: files or folders separated by ;"),
    TEXT(language, "spoken language code or auto for multilingual models"),
    INTEGER(threads, nullptr),
    FLAG(gpu, "use the gpu when built with cuda or vulkan"),
    INTEGER(gpu_device, "which gpu to use when there are several counting from 0"),
    TEXT(vad, "speech detection: silero energy or off to transcribe everything like music"),
    NUMBER(vad_threshold, nullptr),
    NUMBER(chunk, "longest audio chunk in seconds sent to whisper"),
    FLAG(focus, "keep sound panned to the centre; helps against wide crowds, hurts when music has a centred lead vocal"),
    NUMBER(highpass, "cut rumble and bass below this many hz or 0 to disable"),
    FLAG(denoise, "clean the audio with rnnoise before deciding when someone speaks; whisper still hears the original"),
    FLAG(limiter, "flatten bangs and shouts far louder than the speech around them; mixed results in tests"),
    FLAG(normalize, "bring quiet speech up to a steady level before whisper"),
    INTEGER(beam, "beam search width where 1 is fastest"),
    FLAG(context, "give whisper the previous line so sentences carry over"),
    FLAG(live, "show a grey guess of the sentence being spoken, replaced when the speaker pauses"),
    NUMBER(live_every, "seconds between live guesses"),

    TEXT(transcript_dir, "where transcripts are saved"),
    FLAG(save_transcript, "write the transcript to a file in transcript_dir"),
    FLAG(timestamps, "show the time each paragraph started in the tui and the files"),
    TEXT(clip_dir, "where clips and replays are saved"),
    NUMBER(before, "seconds of audio a clip starts with"),
    NUMBER(replay, "length in seconds of an instant replay save"),
    NUMBER(playback, "seconds played back by the play key"),

    INTEGER(ai, "which ai slot is used, 1 to 9, or 0 for off; the a key switches while running"),
    TEXT(ai_slots[0], "ai slots: an ollama model name, a name @ any openai compatible url, or a path to a .gguf file"),
    TEXT(ai_slots[1], nullptr),
    TEXT(ai_slots[2], nullptr),
    TEXT(ai_slots[3], nullptr),
    TEXT(ai_slots[4], nullptr),
    TEXT(ai_slots[5], nullptr),
    TEXT(ai_slots[6], nullptr),
    TEXT(ai_slots[7], nullptr),
    TEXT(ai_slots[8], nullptr),
    TEXT(ai_mode, "fix corrects the transcript in place, notes writes the ai's own output in a pane under it"),
    TEXT(instruction, "what the ai does with the transcript or empty to turn it off"),
    TEXT(llm_url, "endpoint for slots that name no url"),
    NUMBER(llm_every, "seconds between ai passes"),
    TEXT(llm_start, "command started when the endpoint is down and stopped on exit"),

    FLAG(color, "terminal colours"),
    TEXT(key_clip, "keys inside the tui as a letter or space"),
    TEXT(key_replay, nullptr),
    TEXT(key_play, nullptr),
    TEXT(key_instruction, nullptr),
    TEXT(key_ai, nullptr),
    TEXT(key_model, nullptr),
    TEXT(key_settings, nullptr),
    TEXT(key_process, nullptr),
    TEXT(key_clear, nullptr),
    TEXT(key_timestamps, nullptr),
    TEXT(key_folder, nullptr),
    TEXT(key_quit, nullptr),
    TEXT(hotkey_clip, "global hotkeys on x11 and windows like ctrl+alt+c and empty to disable"),
    TEXT(hotkey_replay, nullptr),
    TEXT(hotkey_play, nullptr),
    };
    static const char *slot_keys[9] = { "ai1", "ai2", "ai3", "ai4", "ai5", "ai6", "ai7", "ai8", "ai9" };
    for (Field &f : fields)
        for (int i = 0; i < 9; i++)
            if (f.ptr == &cfg->ai_slots[i]) f.key = slot_keys[i];
    return fields;
}

static std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string configPath() {
    return configDir() + "/earshot.conf";
}

std::string expandPath(const std::string &path) {
    if (path.size() >= 1 && path[0] == '~') return homeDir() + path.substr(1);
    return path;
}

std::string shortPath(const std::string &path) {
    std::string home = homeDir();
    if (!home.empty() && path.compare(0, home.size(), home) == 0) return "~" + path.substr(home.size());
    return path;
}

std::string modelPath(const std::string &name) {
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) return expandPath(name);
    return dataDir() + "/ggml-" + name + ".bin";
}

static bool endsWith(const std::string &s, const char *tail) {
    size_t n = strlen(tail);
    return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}

// whisper models in the data dir by name and in model_paths by full path
std::vector<ModelChoice> modelChoices(const Config &cfg) {
    std::vector<ModelChoice> out;
    auto add = [&](const std::string &value, const std::string &label, uint64_t size) {
        for (const ModelChoice &c : out)
            if (modelPath(c.value) == modelPath(value)) return;
        out.push_back({ value, label, size });
    };
    for (const DirEntry &e : listDir(dataDir())) {
        if (e.dir || e.name.compare(0, 5, "ggml-") != 0 || !endsWith(e.name, ".bin") || e.name.find("silero") != std::string::npos) continue;
        std::string name = e.name.substr(5, e.name.size() - 9);
        add(name, name, e.size);
    }
    std::string list = cfg.model_paths;
    for (size_t start = 0; start <= list.size();) {
        size_t semi = list.find(';', start);
        std::string item = trim(list.substr(start, semi == std::string::npos ? std::string::npos : semi - start));
        start = semi == std::string::npos ? list.size() + 1 : semi + 1;
        if (item.empty()) continue;
        std::string path = expandPath(item);
        DirEntry info;
        if (!fileInfo(path, &info)) continue;
        if (!info.dir) {
            add(path, path.substr(path.find_last_of("/\\") + 1), info.size);
            continue;
        }
        for (const DirEntry &e : listDir(path))
            if (!e.dir && endsWith(e.name, ".bin") && e.name.find("silero") == std::string::npos) add(path + "/" + e.name, e.name, e.size);
    }
    // the configured model is always offered even when it lives somewhere else
    DirEntry info;
    if (fileInfo(modelPath(cfg.model), &info)) add(cfg.model, cfg.model.substr(cfg.model.find_last_of("/\\") + 1), info.size);
    return out;
}


static std::string fieldValue(const Field &f) {
    char buf[32];
    switch (f.type) {
    case FieldType::Text: return *(const std::string *)f.ptr;
    case FieldType::Number: snprintf(buf, sizeof buf, "%g", *(const float *)f.ptr); return buf;
    case FieldType::Integer: return std::to_string(*(const int *)f.ptr);
    case FieldType::Flag: return *(const bool *)f.ptr ? "true" : "false";
    }
    return "";
}

// writes every key with its comment and leaves out unused ai slots past the third
bool configSave(const Config &cfg, const std::string &path) {
    Config copy = cfg;
    std::string out = "# earshot config\n";
    for (const Field &field : fieldsOf(&copy)) {
        std::string value = fieldValue(field);
        bool spare_slot = field.key[0] == 'a' && field.key[1] == 'i' && field.key[2] >= '4' && field.key[2] <= '9';
        if (spare_slot && value.empty()) continue;
        if (field.comment) out += std::string("\n# ") + field.comment + "\n";
        out += std::string(field.key) + " = " + value + "\n";
    }
    std::string tmp = path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "w");
    if (!f) return false;
    bool ok = fputs(out.c_str(), f) >= 0;
    ok = fclose(f) == 0 && ok;
    return ok && replaceFile(tmp, path);
}

// keys renamed since earlier versions still load
static const char *renamed(const std::string &key) {
    if (key == "out_dir") return "clip_dir";
    if (key == "llm_model") return "ai1";
    return key.c_str();
}

static bool isRenamed(const std::string &key) {
    return key == "out_dir" || key == "llm_model";
}

bool configApply(Config *cfg, const std::string &key, const std::string &value) {
    for (const Field &field : fieldsOf(cfg)) {
        if (strcmp(renamed(key), field.key) != 0) continue;
        switch (field.type) {
        case FieldType::Text: *(std::string *)field.ptr = value; break;
        case FieldType::Number: *(float *)field.ptr = (float)atof(value.c_str()); break;
        case FieldType::Integer: *(int *)field.ptr = atoi(value.c_str()); break;
        case FieldType::Flag: *(bool *)field.ptr = value == "true" || value == "yes" || value == "1" || value == "on"; break;
        }
        return true;
    }
    return false;
}

std::string configGet(const Config &cfg, const std::string &key) {
    Config copy = cfg;
    for (const Field &field : fieldsOf(&copy))
        if (key == field.key) return fieldValue(field);
    return "";
}

bool configLoad(Config *cfg, const std::string &path, std::string *err) {
    FILE *f = fopen(path.c_str(), "r");
    if (!f) {
        makeDirs(configDir());
        configSave(*cfg, path);
        return true;
    }
    char line[4096];
    int number = 0;
    bool old_format = false;
    while (fgets(line, sizeof line, f)) {
        number++;
        std::string s = trim(line);
        if (s.empty() || s[0] == '#') continue;
        size_t eq = s.find('=');
        std::string key = trim(s.substr(0, eq));
        std::string value = eq == std::string::npos ? "" : trim(s.substr(eq + 1));
        old_format = old_format || isRenamed(key);
        if (!configApply(cfg, key, value)) *err += "line " + std::to_string(number) + ": unknown key " + key + "\n";
    }
    fclose(f);
    // files written before ai slots had an empty instruction only because that was the old default
    if (old_format) {
        if (cfg->instruction.empty()) cfg->instruction = default_instruction;
        configSave(*cfg, path);
    }
    return err->empty();
}

void configSet(const std::string &path, const char *key, const std::string &value) {
    std::vector<std::string> lines;
    bool found = false;
    if (FILE *f = fopen(path.c_str(), "r")) {
        char line[4096];
        while (fgets(line, sizeof line, f)) {
            std::string s = line;
            std::string t = trim(s);
            size_t eq = t.find('=');
            if (!t.empty() && t[0] != '#' && eq != std::string::npos && trim(t.substr(0, eq)) == key) {
                s = std::string(key) + " = " + value + "\n";
                found = true;
            }
            lines.push_back(s);
        }
        fclose(f);
    }
    if (!found) lines.push_back(std::string(key) + " = " + value + "\n");
    // a temp file and rename so a crash never leaves half a config
    std::string tmp = path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "w");
    if (!f) return;
    bool ok = true;
    for (const std::string &s : lines) ok = fputs(s.c_str(), f) >= 0 && ok;
    ok = fclose(f) == 0 && ok;
    if (!ok || !replaceFile(tmp, path)) remove(tmp.c_str());
}
