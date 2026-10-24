// the settings screen: what each config key is called, how it changes and when it applies
#include "earshot.h"
#include <cmath>
#include <cstdlib>

static const std::vector<Setting> list = {
    { "", "Listening", "", Setting::Header },
    { "source", "What you hear", "System for everything you hear, or part of an app or device name. Also set by Enter in the sources list.", Setting::Text },
    { "mic", "Your mic", "mic for the default input, part of a device name, or empty to leave your voice out.", Setting::Text },
    { "them_label", "Their name", "Shown before what you hear when both channels are on.", Setting::Text },
    { "you_label", "Your name", "Shown before your mic's lines when both channels are on.", Setting::Text },

    { "", "Transcription", "", Setting::Header },
    { "model", "Whisper model", "Left and right step through the models found; Enter types any path to a ggml file.", Setting::Model },
    { "language", "Language", "Spoken language code like en or de, or auto with a multilingual model.", Setting::Text },
    { "beam", "Beam search", "1 is fastest. 5 is a little more accurate and cheap on a gpu.", Setting::Number, 1, 1, 8 },
    { "vad", "Speech detection", "silero is best, energy is simpler, off transcribes everything like song lyrics.", Setting::Choice, 0, 0, 0, true, { "silero", "energy", "off" } },
    { "vad_threshold", "Detection threshold", "Lower catches quieter speech, higher ignores more noise.", Setting::Number, 0.05f, 0.05f, 0.95f },
    { "context", "Carry context", "Give whisper the previous line so sentences continue.", Setting::Toggle },
    { "focus", "Focus on the centre", "Keeps sound panned to the middle. Helps against wide crowd noise, hurts when music has a centred lead vocal.", Setting::Toggle },
    { "highpass", "Cut bass below (Hz)", "Removes rumble and bass. 0 turns it off.", Setting::Number, 25, 0, 400 },
    { "normalize", "Level quiet speech", "Brings quiet sources up to a steady level before whisper.", Setting::Toggle },
    { "chunk", "Longest chunk (s)", "Longest piece of speech sent to whisper at once.", Setting::Number, 1, 4, 30 },
    { "threads", "CPU threads", "Threads whisper uses when it runs on the cpu.", Setting::Number, 1, 1, 64 },
    { "gpu", "Use the GPU", "Run whisper on the gpu in a cuda or vulkan build.", Setting::Toggle, 0, 0, 0, true },
    { "gpu_device", "GPU number", "Which gpu when there are several, counting from 0.", Setting::Number, 1, 0, 8, true },

    { "", "AI", "", Setting::Header },
    { "ai", "AI model", "Which slot below is used, or off. The a key does the same while listening.", Setting::Slot },
    { "instruction", "Instruction", "What the AI does with the transcript. Empty turns it off.", Setting::Text },
    { "ai1", "Slot 1", "An ollama model name, a name @ an openai compatible url, or a path to a .gguf file.", Setting::Text },
    { "ai2", "Slot 2", "An ollama model name, a name @ an openai compatible url, or a path to a .gguf file.", Setting::Text },
    { "ai3", "Slot 3", "An ollama model name, a name @ an openai compatible url, or a path to a .gguf file.", Setting::Text },
    { "ai4", "Slot 4", "An ollama model name, a name @ an openai compatible url, or a path to a .gguf file.", Setting::Text },
    { "ai5", "Slot 5", "An ollama model name, a name @ an openai compatible url, or a path to a .gguf file.", Setting::Text },
    { "llm_every", "Seconds between passes", "How often new lines go to the AI.", Setting::Number, 5, 5, 300 },
    { "llm_url", "Default endpoint", "Used by slots that name no url.", Setting::Text },
    { "llm_start", "Start command", "Run when the endpoint is down and stopped on exit.", Setting::Text, 0, 0, 0, true },

    { "", "Saving", "", Setting::Header },
    { "save_transcript", "Save transcripts", "Write each session to a text file.", Setting::Toggle },
    { "transcript_dir", "Transcript folder", "Where transcripts go. The o key opens it.", Setting::Text },
    { "timestamps", "Timestamps", "Show when each paragraph started. The t key does the same.", Setting::Toggle },
    { "clip_dir", "Clip folder", "Where clips and replays are saved.", Setting::Text },
    { "before", "Clip starts (s ago)", "Seconds of audio before the clip key that a clip keeps.", Setting::Number, 5, 0, 120 },
    { "replay", "Replay length (s)", "Seconds saved by the replay key.", Setting::Number, 5, 5, 120 },
    { "playback", "Play back (s)", "Seconds the play key plays back.", Setting::Number, 5, 5, 120 },

    { "", "Keys and look", "", Setting::Header },
    { "color", "Colours", "Terminal colours.", Setting::Toggle },
    { "key_clip", "Clip key", "A letter, space or tab.", Setting::Text },
    { "key_replay", "Replay key", "A letter, space or tab.", Setting::Text },
    { "key_play", "Play key", "A letter, space or tab.", Setting::Text },
    { "key_ai", "AI model key", "A letter, space or tab.", Setting::Text },
    { "key_model", "Whisper model key", "A letter, space or tab.", Setting::Text },
    { "key_settings", "Settings key", "A letter, space or tab.", Setting::Text },
    { "hotkey_clip", "Global clip hotkey", "Like ctrl+alt+c on x11 and windows, empty for none.", Setting::Text, 0, 0, 0, true },
    { "hotkey_replay", "Global replay hotkey", "Like ctrl+alt+r on x11 and windows, empty for none.", Setting::Text, 0, 0, 0, true },
    { "hotkey_play", "Global play hotkey", "Like ctrl+alt+p on x11 and windows, empty for none.", Setting::Text, 0, 0, 0, true },
};

const std::vector<Setting> &settingsList() {
    return list;
}

std::string settingDisplay(const Config &cfg, const Setting &s) {
    std::string v = configGet(cfg, s.key);
    switch (s.kind) {
    case Setting::Toggle: return v == "true" ? "on" : "off";
    case Setting::Slot: {
        int n = atoi(v.c_str());
        if (n < 1 || n > 9 || cfg.ai_slots[n - 1].empty()) return "off";
        return std::to_string(n) + ": " + aiParse(cfg.ai_slots[n - 1], cfg.llm_url).label;
    }
    case Setting::Text:
        if (v.empty()) return s.key == std::string("mic") || s.key == std::string("instruction") ? "off" : "empty";
        return v;
    default: return v;
    }
}

// left and right on a row; returns false when the row only changes by typing
bool settingStep(Config *cfg, const Setting &s, int dir) {
    std::string v = configGet(*cfg, s.key);
    switch (s.kind) {
    case Setting::Toggle:
        configApply(cfg, s.key, v == "true" ? "false" : "true");
        return true;
    case Setting::Choice: {
        size_t i = 0;
        while (i < s.choices.size() && s.choices[i] != v) i++;
        i = i >= s.choices.size() ? 0 : (i + s.choices.size() + dir) % s.choices.size();
        configApply(cfg, s.key, s.choices[i]);
        return true;
    }
    case Setting::Number: {
        float x = (float)atof(v.c_str()) + dir * s.step;
        x = fminf(s.max, fmaxf(s.min, roundf(x / s.step) * s.step));
        char b[32];
        snprintf(b, sizeof b, "%g", x);
        configApply(cfg, s.key, b);
        return true;
    }
    case Setting::Slot: {
        int n = cfg->ai;
        do n = (n + dir + 10) % 10;
        while (n != 0 && cfg->ai_slots[n - 1].empty());
        cfg->ai = n;
        return true;
    }
    case Setting::Model: {
        std::vector<ModelChoice> choices = modelChoices(*cfg);
        if (choices.empty()) return false;
        int at = -1;
        for (size_t i = 0; i < choices.size(); i++)
            if (modelPath(choices[i].value) == modelPath(cfg->model)) at = (int)i;
        int n = (int)choices.size();
        cfg->model = choices[((at < 0 ? 0 : at + dir) % n + n) % n].value;
        return true;
    }
    default: return false;
    }
}
