// the settings screen: what each config key is called, how it changes and when it applies
#include "earshot.h"
#include <cmath>
#include <cstdlib>

static const std::vector<Setting> list = {
    { "", "listening", "", Setting::Header },
    { "source", "what you hear", "system for everything you hear, or part of an app or device name. also set by enter in the sources list.", Setting::Text },
    { "mic", "your mic", "mic for the default input, part of a device name, or empty to leave your voice out.", Setting::Text },
    { "them_label", "their name", "shown before what you hear when both channels are on.", Setting::Text },
    { "you_label", "your name", "shown before your mic's lines when both channels are on.", Setting::Text },

    { "", "transcription", "", Setting::Header },
    { "model", "whisper model", "left and right step through the models found; enter types any path to a ggml file.", Setting::Model },
    { "language", "language", "spoken language code like en or de, or auto with a multilingual model.", Setting::Text },
    { "beam", "beam search", "1 is fastest. 5 is a little more accurate and cheap on a gpu.", Setting::Number, 1, 1, 8 },
    { "vad", "speech detection", "silero is best, energy is simpler, off transcribes everything like song lyrics.", Setting::Choice, 0, 0, 0, true, { "silero", "energy", "off" } },
    { "vad_threshold", "detection threshold", "lower catches quieter speech, higher ignores more noise.", Setting::Number, 0.05f, 0.05f, 0.95f },
    { "live", "live text", "show a grey guess of the sentence while it is spoken. costs extra work; turn off if whisper falls behind.", Setting::Toggle },
    { "live_every", "live update (s)", "seconds between live guesses.", Setting::Number, 0.5f, 0.5f, 5 },
    { "context", "carry context", "give whisper the previous line so sentences continue.", Setting::Toggle },
    { "denoise", "noise suppression", "cleans the audio with rnnoise to decide when someone speaks, which helps with crowds and fans. whisper still hears the original, because denoised audio made it less accurate in tests.", Setting::Toggle },
    { "limiter", "flatten loud bangs", "pulls sounds far louder than the speech around them down. helped the small models a little and the larger ones not at all in tests.", Setting::Toggle },
    { "focus", "focus on the centre", "keeps sound panned to the middle. helps against wide crowd noise, hurts when music has a centred lead vocal.", Setting::Toggle },
    { "highpass", "cut bass below (hz)", "removes rumble and bass. 0 turns it off.", Setting::Number, 25, 0, 400 },
    { "normalize", "level quiet speech", "brings quiet sources up to a steady level before whisper.", Setting::Toggle },
    { "chunk", "longest chunk (s)", "longest piece of speech sent to whisper at once.", Setting::Number, 1, 4, 30 },
    { "threads", "cpu threads", "threads whisper uses when it runs on the cpu.", Setting::Number, 1, 1, 64 },
    { "gpu", "use the gpu", "run whisper on the gpu in a cuda or vulkan build.", Setting::Toggle, 0, 0, 0, true },
    { "gpu_device", "gpu number", "which gpu when there are several, counting from 0.", Setting::Number, 1, 0, 8, true },

    { "", "ai", "", Setting::Header },
    { "ai", "ai model", "which slot below is used, or off. the a key does the same while listening.", Setting::Slot },
    { "ai_mode", "ai does", "fix corrects misheard words inside the transcript; notes writes its own output, like a summary, in a pane below.", Setting::Choice, 0, 0, 0, false, { "fix", "notes" } },
    { "instruction", "instruction", "extra guidance for fixing, or what to write in notes mode. empty turns the ai off.", Setting::Text },
    { "ai1", "slot 1", "an ollama model name, a name @ an openai compatible url, or a path to a .gguf file.", Setting::Text },
    { "ai2", "slot 2", "an ollama model name, a name @ an openai compatible url, or a path to a .gguf file.", Setting::Text },
    { "ai3", "slot 3", "an ollama model name, a name @ an openai compatible url, or a path to a .gguf file.", Setting::Text },
    { "ai4", "slot 4", "an ollama model name, a name @ an openai compatible url, or a path to a .gguf file.", Setting::Text },
    { "ai5", "slot 5", "an ollama model name, a name @ an openai compatible url, or a path to a .gguf file.", Setting::Text },
    { "llm_every", "seconds between notes", "how often new lines go to the ai in notes mode. fixing runs as lines arrive.", Setting::Number, 5, 5, 300 },
    { "llm_url", "default endpoint", "used by slots that name no url.", Setting::Text },
    { "llm_start", "start command", "run when the endpoint is down and stopped on exit.", Setting::Text, 0, 0, 0, true },

    { "", "saving", "", Setting::Header },
    { "save_transcript", "save transcripts", "write each session to a text file.", Setting::Toggle },
    { "transcript_dir", "transcript folder", "where transcripts go. the o key opens it.", Setting::Text },
    { "timestamps", "timestamps", "show when each paragraph started. the t key does the same.", Setting::Toggle },
    { "clip_dir", "clip folder", "where clips and replays are saved.", Setting::Text },
    { "before", "clip starts (s ago)", "seconds of audio before the clip key that a clip keeps.", Setting::Number, 5, 0, 120 },
    { "replay", "replay length (s)", "seconds saved by the replay key.", Setting::Number, 5, 5, 120 },
    { "playback", "play back (s)", "seconds the play key plays back.", Setting::Number, 5, 5, 120 },

    { "", "keys and look", "", Setting::Header },
    { "color", "colours", "terminal colours.", Setting::Toggle },
    { "key_clip", "clip key", "a letter, space or tab.", Setting::Text },
    { "key_replay", "replay key", "a letter, space or tab.", Setting::Text },
    { "key_play", "play key", "a letter, space or tab.", Setting::Text },
    { "key_ai", "ai model key", "a letter, space or tab.", Setting::Text },
    { "key_model", "whisper model key", "a letter, space or tab.", Setting::Text },
    { "key_settings", "settings key", "a letter, space or tab.", Setting::Text },
    { "hotkey_clip", "global clip hotkey", "like ctrl+alt+c on x11 and windows, empty for none.", Setting::Text, 0, 0, 0, true },
    { "hotkey_replay", "global replay hotkey", "like ctrl+alt+r on x11 and windows, empty for none.", Setting::Text, 0, 0, 0, true },
    { "hotkey_play", "global play hotkey", "like ctrl+alt+p on x11 and windows, empty for none.", Setting::Text, 0, 0, 0, true },
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
