// Talks to any OpenAI-compatible /chat/completions endpoint (Ollama, llama.cpp
// server, Groq, OpenRouter...) by running the curl binary — nothing extra to link.
#include "earshot.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void json_escape(std::string *o, const char *s) {
    o->push_back('"');
    for (; *s; s++) {
        unsigned char ch = *s;
        switch (ch) {
        case '"': *o += "\\\""; break;
        case '\\': *o += "\\\\"; break;
        case '\n': *o += "\\n"; break;
        case '\r': *o += "\\r"; break;
        case '\t': *o += "\\t"; break;
        default:
            if (ch < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", ch); *o += b; }
            else o->push_back(ch);
        }
    }
    o->push_back('"');
}

static void put_utf8(std::string *o, unsigned cp) {
    if (cp < 0x80) o->push_back(cp);
    else if (cp < 0x800) { o->push_back(0xC0 | cp >> 6); o->push_back(0x80 | (cp & 63)); }
    else if (cp < 0x10000) { o->push_back(0xE0 | cp >> 12); o->push_back(0x80 | (cp >> 6 & 63)); o->push_back(0x80 | (cp & 63)); }
    else { o->push_back(0xF0 | cp >> 18); o->push_back(0x80 | (cp >> 12 & 63)); o->push_back(0x80 | (cp >> 6 & 63)); o->push_back(0x80 | (cp & 63)); }
}

// Finds `"key": "<string>"` after `start` and decodes the string.
static bool json_string_after(const std::string &js, const char *key, size_t start, std::string *out) {
    std::string pat = std::string("\"") + key + "\"";
    size_t k = js.find(pat, start);
    if (k == std::string::npos) return false;
    size_t i = js.find(':', k + pat.size());
    if (i == std::string::npos) return false;
    i = js.find_first_not_of(" \t\r\n", i + 1);
    if (i == std::string::npos || js[i] != '"') return false;
    out->clear();
    for (i++; i < js.size() && js[i] != '"'; i++) {
        if (js[i] != '\\') { out->push_back(js[i]); continue; }
        if (++i >= js.size()) break;
        switch (js[i]) {
        case 'n': out->push_back('\n'); break;
        case 't': out->push_back('\t'); break;
        case 'r': break;
        case 'b': case 'f': break;
        case 'u': {
            unsigned cp = strtoul(js.substr(i + 1, 4).c_str(), NULL, 16);
            i += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && js.compare(i + 1, 2, "\\u") == 0) {
                unsigned lo = strtoul(js.substr(i + 3, 4).c_str(), NULL, 16);
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i += 6;
            }
            put_utf8(out, cp);
            break;
        }
        default: out->push_back(js[i]);
        }
    }
    return true;
}

static bool write_private(const char *path, const std::string &data) {
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fchmod(fileno(f), 0600);
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    return true;
}

bool llm_process(const LlmCfg *cfg, const char *instruction, const char *context,
                 const std::string &text, std::string *out) {
    std::string sys =
        "You process a live audio transcript that arrives in chunks. Apply the user's "
        "instruction to the NEW chunk and output only the resulting text: no preamble, "
        "no quotes, no commentary. Do not repeat anything already in the earlier output. "
        "If nothing is worth keeping, output nothing.\n\nInstruction: ";
    sys += instruction;
    std::string user;
    if (context && *context) { user += "Earlier output (context only):\n"; user += context; user += "\n\n"; }
    user += "NEW chunk:\n";
    user += text;

    std::string body = "{\"model\":";
    json_escape(&body, cfg->model);
    body += ",\"stream\":false,\"temperature\":0.2,\"messages\":[{\"role\":\"system\",\"content\":";
    json_escape(&body, sys.c_str());
    body += "},{\"role\":\"user\",\"content\":";
    json_escape(&body, user.c_str());
    body += "}]}";

    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (!dir) dir = "/tmp";
    char body_path[256], hdr_path[256];
    snprintf(body_path, sizeof body_path, "%s/earshot-%d.json", dir, (int)getpid());
    snprintf(hdr_path, sizeof hdr_path, "%s/earshot-%d.hdr", dir, (int)getpid());
    std::string hdr = "Content-Type: application/json\n";
    if (cfg->api_key && *cfg->api_key) hdr += std::string("Authorization: Bearer ") + cfg->api_key + "\n";
    // Key goes through a 0600 file, never argv, so it doesn't show in `ps`.
    if (!write_private(body_path, body) || !write_private(hdr_path, hdr)) {
        *out = "could not write temp file";
        return false;
    }

    int fd[2];
    if (pipe(fd) != 0) { *out = "pipe failed"; return false; }
    pid_t pid = fork();
    if (pid == 0) {
        dup2(fd[1], 1);
        dup2(fd[1], 2);
        close(fd[0]); close(fd[1]);
        char hdr_arg[260], body_arg[260];
        snprintf(hdr_arg, sizeof hdr_arg, "@%s", hdr_path);
        snprintf(body_arg, sizeof body_arg, "@%s", body_path);
        execlp("curl", "curl", "-sS", "-m", "120", "-H", hdr_arg, "--data-binary", body_arg,
               cfg->url, (char *)NULL);
        _exit(127);
    }
    close(fd[1]);
    std::string resp;
    char buf[4096];
    ssize_t r;
    while ((r = read(fd[0], buf, sizeof buf)) > 0) resp.append(buf, r);
    close(fd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    unlink(body_path);
    unlink(hdr_path);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        *out = "curl failed: " + resp.substr(0, 200);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 127) *out = "curl not found";
        return false;
    }
    size_t msg = resp.find("\"message\"");
    if (msg != std::string::npos && json_string_after(resp, "content", msg, out)) {
        size_t a = out->find_first_not_of(" \n");
        size_t b = out->find_last_not_of(" \n");
        *out = a == std::string::npos ? "" : out->substr(a, b - a + 1);
        return true;
    }
    std::string err;
    if (json_string_after(resp, "message", 0, &err) || json_string_after(resp, "error", 0, &err))
        *out = "llm error: " + err;
    else
        *out = "llm: unexpected reply: " + resp.substr(0, 200);
    return false;
}
