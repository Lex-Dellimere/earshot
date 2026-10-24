// transcript lines from both channels
#include "earshot.h"
#include <cctype>

constexpr double echo_window = 6;  // seconds either side where a line may be the other channel's echo

static std::vector<std::string> plainWords(const std::string &s) {
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
