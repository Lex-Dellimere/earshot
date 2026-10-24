// talks to any openai compatible chat endpoint through the curl binary
// which ships with linux distros and windows 10 and later
#include "earshot.h"
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <thread>

struct Job {
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
    bool stop = false;
    std::thread thread;
    Process *server = nullptr;
    bool tried_start = false;
};

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
static int post(Llm *llm, const std::string &body, std::string *reply) {
    std::string base = tempDir() + "/earshot-llm";
    std::string headers = "Content-Type: application/json\n";
    if (!llm->api_key.empty()) headers += "Authorization: Bearer " + llm->api_key + "\n";
    // the key goes through a private file so it never shows in the process list
    if (!writePrivateFile(base + ".json", body) || !writePrivateFile(base + ".hdr", headers)) {
        *reply = "cannot write temp file in " + tempDir();
        return -1;
    }
    int code = runProcess({ "curl", "-sS", "-m", "180", "-H", "@" + base + ".hdr",
                            "--data-binary", "@" + base + ".json", llm->cfg.llm_url }, reply);
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

static void request(Llm *llm, const std::string &instruction, const std::string &context, const std::string &text) {
    std::string system =
        "You clean up a live audio transcript that arrives in chunks.\n"
        "Rules:\n"
        "- Use only what is actually said in the NEW chunk. Never invent, guess or add facts.\n"
        "- Do not repeat anything already in the earlier output.\n"
        "- If the NEW chunk adds nothing new, reply with an empty message.\n"
        "- Output only the result, with no preamble, labels or quotes.\n\n"
        "Instruction: " + instruction;
    std::string user;
    if (!context.empty()) user += "Earlier output (context only):\n" + context + "\n\n";
    user += "NEW chunk:\n" + text;

    std::string body = "{\"model\":";
    jsonEscape(&body, llm->cfg.llm_model);
    body += ",\"stream\":false,\"temperature\":0,\"messages\":[{\"role\":\"system\",\"content\":";
    jsonEscape(&body, system);
    body += "},{\"role\":\"user\",\"content\":";
    jsonEscape(&body, user);
    body += "}]}";

    std::string reply;
    int code = post(llm, body, &reply);
    // curl exit 7 means nothing is listening so start the local server once
    if (code == 7 && !llm->tried_start && !llm->cfg.llm_start.empty()) {
        llm->tried_start = true;
        llm->state = LlmState::Starting;
        llm->server = spawnProcess(splitWords(llm->cfg.llm_start));
        for (int i = 0; i < 30 && code == 7 && llm->server; i++) {
            sleepMs(1000);
            code = post(llm, body, &reply);
        }
        llm->state = LlmState::Working;
    }

    bool ok = false;
    std::string out;
    if (code == -1 && reply.empty()) {
        out = "curl not found";
    } else if (code != 0) {
        out = "llm request failed: " + reply.substr(0, 200);
    } else {
        size_t message = reply.find("\"message\"");
        if (message != std::string::npos && jsonString(reply, "content", message, &out)) {
            size_t a = out.find_first_not_of(" \n");
            size_t b = out.find_last_not_of(" \n");
            out = a == std::string::npos || saysNothing(out) ? "" : out.substr(a, b - a + 1);
            ok = true;
        } else if (jsonString(reply, "message", 0, &out) || jsonString(reply, "error", 0, &out)) {
            out = "llm error: " + out;
        } else {
            out = "llm unexpected reply: " + reply.substr(0, 200);
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
        request(llm, job.instruction, job.context, job.text);
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

bool llmSubmit(Llm *llm, const std::string &instruction, const std::string &context, const std::string &text) {
    if (llm->state == LlmState::Working || llm->state == LlmState::Starting) return false;
    llm->state = LlmState::Working;
    {
        std::lock_guard<std::mutex> lock(llm->mu);
        llm->jobs.push_back({ instruction, context, text });
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
