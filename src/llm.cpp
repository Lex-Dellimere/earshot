// talks to any openai compatible chat endpoint through the curl binary
// which ships with linux distros and windows 10 and later
#include "earshot.h"
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <thread>

struct Job {
    AiModel model;
    std::string instruction, context, text;
};

// one long lived worker so a server it starts lives as long as earshot
struct Llm {
    Config cfg;
    std::string api_key;
    std::atomic<LlmState> state{ LlmState::Idle };
    std::mutex mu;
    std::condition_variable wake;
    std::vector<Job> jobs;
    std::vector<std::pair<bool, std::string>> results;
    std::atomic<bool> stop{ false };  // also cancels a request in flight so quitting never waits on it
    std::thread thread;
    Process *server = nullptr;
    bool tried_start = false;
    std::vector<std::string> imported;  // gguf files already turned into ollama models
};

static std::string trimmed(const std::string &s) {
    size_t a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t") - a + 1);
}

AiModel aiParse(const std::string &slot, const std::string &default_url) {
    AiModel m;
    std::string s = trimmed(slot);
    size_t at = s.find(" @ ");
    m.url = at == std::string::npos ? default_url : trimmed(s.substr(at + 3));
    s = trimmed(s.substr(0, at));
    bool path = s.find('/') != std::string::npos || s.find('\\') != std::string::npos;
    if (path || (s.size() > 5 && s.compare(s.size() - 5, 5, ".gguf") == 0)) {
        m.gguf = expandPath(s);
        std::string base = m.gguf.substr(m.gguf.find_last_of("/\\") + 1);
        if (base.size() > 5 && base.compare(base.size() - 5, 5, ".gguf") == 0) base.resize(base.size() - 5);
        m.label = base;
        // ollama names allow lower case letters digits dots dashes and underscores
        m.name = "earshot-";
        for (char c : base) m.name.push_back(isalnum((unsigned char)c) || c == '.' || c == '_' ? (char)tolower((unsigned char)c) : '-');
        return m;
    }
    m.name = s;
    m.label = s;
    return m;
}

static void jsonEscape(std::string *out, const std::string &s) {
    out->push_back('"');
    for (unsigned char c : s) {
        switch (c) {
        case '"': *out += "\\\""; break;
        case '\\': *out += "\\\\"; break;
        case '\n': *out += "\\n"; break;
        case '\r': *out += "\\r"; break;
        case '\t': *out += "\\t"; break;
        default:
            if (c < 0x20) {
                char b[8];
                snprintf(b, sizeof b, "\\u%04x", c);
                *out += b;
            } else {
                out->push_back(c);
            }
        }
    }
    out->push_back('"');
}

static void putUtf8(std::string *out, unsigned cp) {
    if (cp < 0x80) {
        out->push_back(cp);
    } else if (cp < 0x800) {
        out->push_back(0xC0 | cp >> 6);
        out->push_back(0x80 | (cp & 63));
    } else if (cp < 0x10000) {
        out->push_back(0xE0 | cp >> 12);
        out->push_back(0x80 | (cp >> 6 & 63));
        out->push_back(0x80 | (cp & 63));
    } else {
        out->push_back(0xF0 | cp >> 18);
        out->push_back(0x80 | (cp >> 12 & 63));
        out->push_back(0x80 | (cp >> 6 & 63));
        out->push_back(0x80 | (cp & 63));
    }
}

// finds the string value for key after start and decodes it
static bool jsonString(const std::string &js, const char *key, size_t start, std::string *out) {
    std::string pattern = std::string("\"") + key + "\"";
    size_t k = js.find(pattern, start);
    if (k == std::string::npos) return false;
    size_t i = js.find(':', k + pattern.size());
    if (i == std::string::npos) return false;
    i = js.find_first_not_of(" \t\r\n", i + 1);
    if (i == std::string::npos || js[i] != '"') return false;
    out->clear();
    for (i++; i < js.size() && js[i] != '"'; i++) {
        if (js[i] != '\\') {
            out->push_back(js[i]);
            continue;
        }
        if (++i >= js.size()) break;
        switch (js[i]) {
        case 'n': out->push_back('\n'); break;
        case 't': out->push_back('\t'); break;
        case 'r': case 'b': case 'f': break;
        case 'u': {
            unsigned cp = strtoul(js.substr(i + 1, 4).c_str(), nullptr, 16);
            i += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && js.compare(i + 1, 2, "\\u") == 0) {
                unsigned lo = strtoul(js.substr(i + 3, 4).c_str(), nullptr, 16);
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i += 6;
            }
            putUtf8(out, cp);
            break;
        }
        default: out->push_back(js[i]);
        }
    }
    return true;
}

static std::vector<std::string> splitWords(const std::string &s) {
    std::vector<std::string> words;
    std::string w;
    for (char c : s) {
        if (c == ' ') {
            if (!w.empty()) words.push_back(w);
            w.clear();
        } else {
            w.push_back(c);
        }
    }
    if (!w.empty()) words.push_back(w);
    return words;
}

// returns the curl exit code and fills reply with the body
static int post(Llm *llm, const std::string &url, const std::string &body, std::string *reply) {
    // the pid keeps two running copies from sharing temp files
    std::string base = tempDir() + "/earshot-llm-" + std::to_string(processId());
    std::string headers = "Content-Type: application/json\n";
    if (!llm->api_key.empty()) headers += "Authorization: Bearer " + llm->api_key + "\n";
    // the key goes through a private file so it never shows in the process list
    if (!writePrivateFile(base + ".json", body) || !writePrivateFile(base + ".hdr", headers)) {
        *reply = "cannot write temp file in " + tempDir();
        return -1;
    }
    int code = runProcess({ "curl", "-sS", "-m", "180", "-H", "@" + base + ".hdr",
                            "--data-binary", "@" + base + ".json", url }, reply, &llm->stop);
    remove((base + ".json").c_str());
    remove((base + ".hdr").c_str());
    return code;
}

// models sometimes say nothing in words instead of replying empty
static bool saysNothing(const std::string &reply) {
    std::string plain;
    for (unsigned char c : reply)
        if (isalpha(c)) plain.push_back((char)tolower(c));
    return plain == "nothing" || plain == "none" || plain == "na" || plain == "empty";
}

// makes an ollama model from a gguf file once so a slot can point at any file on disk
static bool importGguf(Llm *llm, const AiModel &m, std::string *err) {
    for (const std::string &done : llm->imported)
        if (done == m.gguf) return true;
    FILE *f = fopen(m.gguf.c_str(), "rb");
    if (!f) return *err = "cannot read " + m.gguf, false;
    fclose(f);
    std::string out;
    if (runProcess({ "ollama", "show", m.name }, &out, &llm->stop) != 0) {
        llm->state = LlmState::Importing;
        std::string modelfile = tempDir() + "/earshot-" + std::to_string(processId()) + ".Modelfile";
        if (!writePrivateFile(modelfile, "FROM " + m.gguf + "\n")) return *err = "cannot write " + modelfile, false;
        int code = runProcess({ "ollama", "create", m.name, "-f", modelfile }, &out, &llm->stop);
        remove(modelfile.c_str());
        if (code == -1) return *err = "a .gguf slot needs ollama installed (ollama.com)", false;
        if (code != 0) return *err = "ollama could not import " + m.gguf + ": " + trimmed(out).substr(0, 160), false;
    }
    llm->imported.push_back(m.gguf);
    return true;
}

// turns the usual failures into something the user can act on
static std::string explain(const AiModel &m, const std::string &message) {
    std::string lower;
    for (char c : message) lower.push_back((char)tolower((unsigned char)c));
    if (lower.find("not found") != std::string::npos && lower.find("model") != std::string::npos)
        return m.gguf.empty() ? "ai model " + m.name + " is not installed: run  ollama pull " + m.name : "ai model " + m.label + " is missing in ollama";
    return "ai error: " + message.substr(0, 200);
}

static void request(Llm *llm, const Job &job) {
    const AiModel &m = job.model;
    // tested against gemma3 qwen2.5 and llama3.2: this keeps speaker names and changes the least
    std::string system =
        "You work on a live speech-to-text transcript that arrives in chunks. Speech recognition often mishears "
        "words that sound alike, especially over music or noise.\n"
        "Rules:\n"
        "- Work only on the NEW chunk. Earlier output is context to help you understand what was meant.\n"
        "- Keep the speaker name before each colon exactly as given.\n"
        "- Replace a word only when it is clearly a mishearing that makes no sense in context.\n"
        "- Drop lines that are only filler noise like a lone I, Oh or Um.\n"
        "- Never invent or add facts and never comment. If the NEW chunk adds nothing, reply with an empty message.\n"
        "- Output only the result.\n\n"
        "Instruction: " + job.instruction;
    std::string user;
    if (!job.context.empty()) user += "Earlier output (context only):\n" + job.context + "\n\n";
    user += "NEW chunk:\n" + job.text;

    std::string body = "{\"model\":";
    jsonEscape(&body, m.name);
    body += ",\"stream\":false,\"temperature\":0,\"messages\":[{\"role\":\"system\",\"content\":";
    jsonEscape(&body, system);
    body += "},{\"role\":\"user\",\"content\":";
    jsonEscape(&body, user);
    body += "}]}";

    std::string reply, err;
    int code = post(llm, m.url, body, &reply);
    // curl exit 7 means nothing is listening so start the local server once
    if (code == 7 && !llm->tried_start && !llm->cfg.llm_start.empty()) {
        llm->tried_start = true;
        llm->state = LlmState::Starting;
        llm->server = spawnProcess(splitWords(llm->cfg.llm_start));
        for (int i = 0; i < 60 && code == 7 && llm->server && !llm->stop; i++) {
            sleepMs(500);
            code = post(llm, m.url, body, &reply);
        }
        llm->state = LlmState::Working;
    }
    if (!m.gguf.empty() && code == 0 && reply.find("not found") != std::string::npos) {
        if (importGguf(llm, m, &err)) {
            llm->state = LlmState::Working;
            code = post(llm, m.url, body, &reply);
        }
    }

    bool ok = false;
    std::string out;
    if (!err.empty()) {
        out = err;
    } else if (code == -1 && reply.empty()) {
        out = "curl not found";
    } else if (code == 7) {
        out = "cannot reach " + m.url + (llm->cfg.llm_start.rfind("ollama", 0) == 0 ? " (is ollama installed? ollama.com)" : "");
    } else if (code != 0) {
        out = "ai request failed: " + reply.substr(0, 200);
    } else {
        size_t message = reply.find("\"message\"");
        if (message != std::string::npos && jsonString(reply, "content", message, &out)) {
            size_t a = out.find_first_not_of(" \n");
            size_t b = out.find_last_not_of(" \n");
            out = a == std::string::npos || saysNothing(out) ? "" : out.substr(a, b - a + 1);
            ok = true;
        } else if (jsonString(reply, "message", 0, &out) || jsonString(reply, "error", 0, &out)) {
            out = explain(m, out);
        } else {
            out = "ai unexpected reply: " + reply.substr(0, 200);
        }
    }
    std::lock_guard<std::mutex> lock(llm->mu);
    llm->results.push_back({ ok, out });
    llm->state = ok ? LlmState::Idle : LlmState::Error;
}

static void worker(Llm *llm) {
    std::unique_lock<std::mutex> lock(llm->mu);
    for (;;) {
        llm->wake.wait(lock, [llm] { return llm->stop || !llm->jobs.empty(); });
        if (llm->stop) break;
        Job job = llm->jobs.front();
        llm->jobs.erase(llm->jobs.begin());
        lock.unlock();
        request(llm, job);
        lock.lock();
    }
}

Llm *llmCreate(const Config &cfg) {
    Llm *llm = new Llm();
    llm->cfg = cfg;
    if (const char *key = getenv("EARSHOT_API_KEY")) llm->api_key = key;
    llm->thread = std::thread(worker, llm);
    return llm;
}

void llmDestroy(Llm *llm) {
    {
        std::lock_guard<std::mutex> lock(llm->mu);
        llm->stop = true;
    }
    llm->wake.notify_one();
    llm->thread.join();
    if (llm->server) killProcess(llm->server);
    delete llm;
}

bool llmSubmit(Llm *llm, const AiModel &model, const std::string &instruction, const std::string &context, const std::string &text) {
    LlmState state = llm->state;
    if (state == LlmState::Working || state == LlmState::Starting || state == LlmState::Importing) return false;
    llm->state = LlmState::Working;
    {
        std::lock_guard<std::mutex> lock(llm->mu);
        llm->jobs.push_back({ model, instruction, context, text });
    }
    llm->wake.notify_one();
    return true;
}

LlmState llmState(Llm *llm) {
    return llm->state;
}

bool llmTakeResult(Llm *llm, std::string *text, bool *ok) {
    std::lock_guard<std::mutex> lock(llm->mu);
    if (llm->results.empty()) return false;
    *ok = llm->results.front().first;
    *text = llm->results.front().second;
    llm->results.erase(llm->results.begin());
    return true;
}
