// transcript lines from both channels
#include "earshot.h"
#include <algorithm>
#include <cctype>

constexpr double echo_window = 6;  // seconds either side where a line may be the other channel's echo

std::vector<std::string> plainWords(const std::string &s) {
    std::vector<std::string> words;
    std::string w;
    for (unsigned char c : s) {
        if (isalnum(c) || c >= 0x80 || c == '\'') {
            w.push_back((char)tolower(c));
        } else if (!w.empty()) {
            words.push_back(w);
            w.clear();
        }
    }
    if (!w.empty()) words.push_back(w);
    return words;
}

// share of the words in line that also appear in others
static float overlap(const std::string &line, const std::vector<const Line *> &others) {
    std::vector<std::string> mine = plainWords(line), pool;
    if (mine.size() < 3) return 0;
    for (const Line *o : others)
        for (const std::string &w : plainWords(o->text)) pool.push_back(w);
    int found = 0;
    for (const std::string &w : mine) {
        for (std::string &p : pool) {
            if (p == w) {
                p.clear();
                found++;
                break;
            }
        }
    }
    return (float)found / mine.size();
}

static std::vector<const Line *> near(const Transcript *tr, int channel, double time) {
    std::vector<const Line *> out;
    for (const Line &l : tr->raw)
        if (l.channel == channel && !l.dropped && l.time > time - echo_window && l.time < time + echo_window) out.push_back(&l);
    return out;
}

// speakers bleeding into a mic would repeat the other side so those you lines are dropped
// whichever channel finishes its chunk first
void transcriptPartial(Transcript *tr, int channel, double time, const std::string &text) {
    std::lock_guard<std::mutex> lock(tr->mu);
    tr->partial[channel] = text;
    tr->partial_time[channel] = time;
}

void transcriptAdd(Transcript *tr, Line line) {
    std::lock_guard<std::mutex> lock(tr->mu);
    line.added = nowSeconds();
    line.id = tr->next_id++;
    line.heard = line.text;
    tr->partial[line.channel].clear();
    if (line.channel == You) {
        line.dropped = overlap(line.text, near(tr, Them, line.time)) >= 0.7f;
    } else {
        tr->raw.push_back(line);
        for (Line &l : tr->raw)
            if (l.channel == You && !l.dropped && l.time > line.time - echo_window && l.time < line.time + echo_window)
                l.dropped = overlap(l.text, near(tr, Them, l.time)) >= 0.7f;
        return;
    }
    tr->raw.push_back(line);
}

constexpr double para_gap = 2.5;    // a pause this long starts a new paragraph
constexpr double ai_wait = 30;      // longest a paragraph waits for the ai before it is written as heard
constexpr size_t para_max = 700;    // and so does a paragraph this long

// groups lines into paragraphs by speaker and pause and marks the ones that can no longer change
// a you line can still be dropped as an echo for a while so its paragraph waits for that
// pending says which channels are mid utterance and so may still add to their last paragraph
// wait_ai holds a paragraph back until the ai has corrected its lines or given up on them
std::vector<Paragraph> transcriptParagraphs(const Transcript *tr, size_t from, double now, const bool *pending, bool wait_ai) {
    std::vector<Paragraph> out;
    for (size_t i = from; i < tr->raw.size(); i++) {
        const Line &l = tr->raw[i];
        if (l.dropped) {
            if (!out.empty()) out.back().last = i;
            continue;
        }
        Paragraph *p = out.empty() ? nullptr : &out.back();
        if (!p || p->channel != l.channel || l.time - p->end > para_gap || p->text.size() > para_max) {
            out.push_back({ l.channel, l.time, l.end, l.text, i, i, false });
            continue;
        }
        p->text += " " + l.text;
        p->end = std::max(p->end, l.end);
        p->last = i;
    }
    for (size_t k = 0; k < out.size(); k++) {
        double latest = 0;
        bool corrected = true;
        for (size_t i = out[k].first; i <= out[k].last; i++) {
            const Line &l = tr->raw[i];
            latest = std::max(latest, l.added);
            if (wait_ai && !l.dropped && l.ai != 2 && now - l.added < ai_wait) corrected = false;
        }
        bool settled = (out[k].channel == Them || now - latest > echo_window) && corrected;
        bool more = pending && pending[out[k].channel];
        out[k].final = settled && (k + 1 < out.size() || (!more && now - out[k].end > para_gap));
    }
    return out;
}

// pulls "n. text" lines out of an ai reply, dropping any speaker the model repeated
std::vector<std::pair<int, std::string>> aiNumberedLines(const std::string &reply) {
    std::vector<std::pair<int, std::string>> out;
    size_t start = 0;
    while (start < reply.size()) {
        size_t nl = reply.find('\n', start);
        std::string line = reply.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        start = nl == std::string::npos ? reply.size() : nl + 1;
        size_t i = line.find_first_not_of(" *-");
        if (i == std::string::npos || !isdigit((unsigned char)line[i])) continue;
        int n = 0;
        while (i < line.size() && isdigit((unsigned char)line[i])) n = n * 10 + (line[i++] - '0');
        if (i >= line.size() || (line[i] != '.' && line[i] != ':' && line[i] != ')')) continue;
        i = line.find_first_not_of(" ", i + 1);
        if (i == std::string::npos) continue;
        std::string text = line.substr(i);
        size_t lead = text.find_first_not_of("* ");
        if (lead == std::string::npos) continue;
        text = text.substr(lead);
        if (text[0] == '(' || text[0] == '[') {
            size_t close = text.find(text[0] == '(' ? ')' : ']');
            if (close != std::string::npos && close < 24) text = text.substr(text.find_first_not_of(" ", close + 1) == std::string::npos ? text.size() : text.find_first_not_of(" ", close + 1));
        }
        size_t colon = text.find(": ");
        if (colon != std::string::npos && colon < 16 && text.substr(0, colon).find(' ') == std::string::npos) text = text.substr(colon + 2);
        while (!text.empty() && (text.back() == ' ' || text.back() == '\r')) text.pop_back();
        // models like to swap in curly quotes, which would read as a change to every contraction
        static const char *curly[][2] = { { "\xe2\x80\x99", "'" }, { "\xe2\x80\x98", "'" }, { "\xe2\x80\x9c", "\"" }, { "\xe2\x80\x9d", "\"" } };
        for (const auto &c : curly)
            for (size_t p; (p = text.find(c[0])) != std::string::npos;) text.replace(p, 3, c[1]);
        if (!text.empty()) out.push_back({ n, text });
    }
    return out;
}

// a correction is kept only when it changes a few words, so a model that rewrites or invents is ignored
bool aiCloseEnough(const std::string &heard, const std::string &fixed) {
    std::vector<std::string> a = plainWords(heard), b = plainWords(fixed);
    if (b.empty()) return false;
    std::vector<size_t> d(b.size() + 1);
    for (size_t j = 0; j <= b.size(); j++) d[j] = j;
    for (size_t i = 1; i <= a.size(); i++) {
        size_t prev = d[0];
        d[0] = i;
        for (size_t j = 1; j <= b.size(); j++) {
            size_t cur = d[j];
            d[j] = std::min({ d[j] + 1, d[j - 1] + 1, prev + (a[i - 1] == b[j - 1] ? 0 : 1) });
            prev = cur;
        }
    }
    return d[b.size()] <= std::max<size_t>(2, a.size() / 2);
}
