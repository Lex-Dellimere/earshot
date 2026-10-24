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
    return {
    TEXT(source, "what you hear: system or part of an app or device name"),
    TEXT(mic, "your voice: mic for the default input, part of a device name, or empty for off"),
    TEXT(them_label, "names shown before each channel's lines when both are on"),
    TEXT(you_label, nullptr),
    TEXT(model, "whisper model name in the data dir or a full path to a ggml file"),
    TEXT(language, "spoken language code or auto for multilingual models"),
    INTEGER(threads, nullptr),
    FLAG(gpu, "use the gpu when built with cuda or vulkan"),
    INTEGER(gpu_device, "which gpu to use when there are several counting from 0"),
    TEXT(vad, "speech detection: silero energy or off to transcribe everything like music"),
    NUMBER(vad_threshold, nullptr),
    NUMBER(chunk, "longest audio chunk in seconds sent to whisper"),
    FLAG(focus, "keep sound panned to the centre where voices usually sit"),
    NUMBER(highpass, "cut rumble and bass below this many hz or 0 to disable"),
    FLAG(normalize, "bring quiet speech up to a steady level before whisper"),
    INTEGER(beam, "beam search width where 1 is fastest"),
    FLAG(context, "give whisper the previous line so sentences carry over"),

    TEXT(out_dir, "where clips replays and transcripts are saved"),
    NUMBER(before, "seconds of audio a clip starts with"),
    NUMBER(replay, "length in seconds of an instant replay save"),
    NUMBER(playback, "seconds played back by the play key"),
    FLAG(save_transcript, "write the transcript to a file in out_dir"),

    TEXT(instruction, "what the ai does with the transcript or empty to turn it off"),
    TEXT(llm_url, "any openai compatible chat completions endpoint"),
    TEXT(llm_model, nullptr),
    NUMBER(llm_every, "seconds between ai passes"),
    TEXT(llm_start, "command started when the endpoint is down and stopped on exit"),

    FLAG(color, "terminal colours"),
    TEXT(key_clip, "keys inside the tui as a letter or space"),
    TEXT(key_replay, nullptr),
    TEXT(key_play, nullptr),
    TEXT(key_instruction, nullptr),
    TEXT(key_process, nullptr),
    TEXT(key_clear, nullptr),
    TEXT(key_quit, nullptr),
    TEXT(hotkey_clip, "global hotkeys on x11 and windows like ctrl+alt+c and empty to disable"),
    TEXT(hotkey_replay, nullptr),
    TEXT(hotkey_play, nullptr),
    };
}

std::string configPath() {
    return configDir() + "/earshot.conf";
}

std::string expandPath(const std::string &path) {
    if (path.size() >= 1 && path[0] == '~') return homeDir() + path.substr(1);
    return path;
}

std::string modelPath(const std::string &name) {
    if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) return expandPath(name);
    return dataDir() + "/ggml-" + name + ".bin";
}

static std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
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

static void writeDefaults(const std::string &path) {
    Config defaults;
    FILE *f = fopen(path.c_str(), "w");
    if (!f) return;
    fprintf(f, "# earshot config\n");
    for (const Field &field : fieldsOf(&defaults)) {
        if (field.comment) fprintf(f, "\n# %s\n", field.comment);
        fprintf(f, "%s = %s\n", field.key, fieldValue(field).c_str());
    }
    fclose(f);
}

bool configApply(Config *cfg, const std::string &key, const std::string &value) {
    for (const Field &field : fieldsOf(cfg)) {
        if (key != field.key) continue;
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

bool configLoad(Config *cfg, const std::string &path, std::string *err) {
    FILE *f = fopen(path.c_str(), "r");
    if (!f) {
        makeDirs(configDir());
        writeDefaults(path);
        return true;
    }
    char line[4096];
    int number = 0;
    while (fgets(line, sizeof line, f)) {
        number++;
        std::string s = trim(line);
        if (s.empty() || s[0] == '#') continue;
        size_t eq = s.find('=');
        std::string key = trim(s.substr(0, eq));
        std::string value = eq == std::string::npos ? "" : trim(s.substr(eq + 1));
        if (!configApply(cfg, key, value)) *err += "line " + std::to_string(number) + ": unknown key " + key + "\n";
    }
    fclose(f);
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
    FILE *f = fopen(path.c_str(), "w");
    if (!f) return;
    for (const std::string &s : lines) fputs(s.c_str(), f);
    fclose(f);
}
