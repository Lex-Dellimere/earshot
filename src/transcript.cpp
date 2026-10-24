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
void transcriptAdd(Transcript *tr, Line line) {
    std::lock_guard<std::mutex> lock(tr->mu);
    line.added = nowSeconds();
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
constexpr size_t para_max = 700;    // and so does a paragraph this long

// groups lines into paragraphs by speaker and pause and marks the ones that can no longer change
// a you line can still be dropped as an echo for a while so its paragraph waits for that
// pending says which channels are mid utterance and so may still add to their last paragraph
std::vector<Paragraph> transcriptParagraphs(const Transcript *tr, size_t from, double now, const bool *pending) {
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
        for (size_t i = out[k].first; i <= out[k].last; i++) latest = std::max(latest, tr->raw[i].added);
        bool settled = out[k].channel == Them || now - latest > echo_window;
        bool more = pending && pending[out[k].channel];
        out[k].final = settled && (k + 1 < out.size() || (!more && now - out[k].end > para_gap));
    }
    return out;
}
