// ansi terminal ui shared by linux and windows
#include "earshot.h"
#include <cstring>
#include <ctime>

static bool color_on = true;
static std::string screen;

void tuiSetColor(bool on) {
    color_on = on;
}

static const char *paint(const char *code) {
    return color_on ? code : "";
}

static const char *reset = "\x1b[0m";
static const char *bold = "\x1b[1m";
static const char *dim = "\x1b[2m";
static const char *inverse = "\x1b[7m";
static const char *red = "\x1b[31m";
static const char *green = "\x1b[32m";
static const char *yellow = "\x1b[33m";
static const char *blue = "\x1b[34m";
static const char *magenta = "\x1b[35m";
static const char *cyan = "\x1b[36m";
static const char *gray = "\x1b[90m";
static const char *white = "\x1b[97m";
static const char *fg_default = "\x1b[39m";
static const char *bar_bg = "\x1b[44m";
static const char *foot_bg = "\x1b[100m";
static const char *clip_badge = "\x1b[41;97;1m";
static const char *play_badge = "\x1b[42;30;1m";
static const char *error_bg = "\x1b[41;97m";
static const char *info_bg = "\x1b[42;30m";

// keys

int keyCode(const std::string &name) {
    if (name == "space") return ' ';
    if (name == "tab") return '\t';
    if (name.size() == 1) return (unsigned char)name[0];
    return KeyNone;
}

int tuiReadKey(std::string *text) {
    char b[32];
    int n = terminalRead(b, sizeof b - 1);
    if (n <= 0) return KeyNone;
    b[n] = 0;
    unsigned char c = b[0];
    if (c == 27) {
        if (n == 1) return KeyEsc;
        if (n >= 3 && (b[1] == '[' || b[1] == 'O')) {
            switch (b[2]) {
            case 'A': return KeyUp;
            case 'B': return KeyDown;
            case 'C': return KeyRight;
            case 'D': return KeyLeft;
            case '5': return KeyPageUp;
            case '6': return KeyPageDown;
            }
        }
        return KeyNone;
    }
    if (c == '\r' || c == '\n') return KeyEnter;
    if (c == 127 || c == 8) return KeyBackspace;
    if (text) *text = std::string(b, n);
    return c;
}

// width helpers count code points and skip escape sequences

static size_t skipEscape(const std::string &s, size_t i) {
    if (s[i] != '\x1b' || i + 1 >= s.size() || s[i + 1] != '[') return i;
    i += 2;
    while (i < s.size() && !(s[i] >= 0x40 && s[i] <= 0x7e)) i++;
    return i + 1;
}

static int textWidth(const std::string &s) {
    int w = 0;
    for (size_t i = 0; i < s.size();) {
        size_t j = skipEscape(s, i);
        if (j != i) {
            i = j;
            continue;
        }
        if (((unsigned char)s[i] & 0xC0) != 0x80) w++;
        i++;
    }
    return w;
}

static size_t fitBytes(const std::string &s, int w) {
    int used = 0;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = skipEscape(s, i);
        if (j != i) {
            i = j;
            continue;
        }
        if (((unsigned char)s[i] & 0xC0) != 0x80) {
            if (used == w) break;
            used++;
        }
        i++;
    }
    return i;
}

static void moveTo(int row, int col) {
    char b[24];
    snprintf(b, sizeof b, "\x1b[%d;%dH", row, col);
    screen += b;
}

// writes s clipped or padded to exactly w columns
static void cell(const std::string &s, int w) {
    size_t n = fitBytes(s, w);
    screen.append(s, 0, n);
    for (int c = textWidth(s.substr(0, n)); c < w; c++) screen.push_back(' ');
}

static void wrap(const std::string &text, int w, std::vector<std::string> *out) {
    if (w < 4) w = 4;
    size_t i = 0;
    while (i < text.size()) {
        size_t n = fitBytes(text.substr(i), w);
        if (i + n < text.size() && text[i + n] != ' ') {
            size_t space = text.rfind(' ', i + n);
            if (space != std::string::npos && space > i) n = space - i;
        }
        out->push_back(text.substr(i, n));
        i += n;
        while (i < text.size() && text[i] == ' ') i++;
    }
    if (text.empty()) out->push_back("");
}

bool sameSource(const Source &a, const Source &b) {
    if (a.kind != b.kind) return false;
    if (a.kind == SourceKind::System || a.kind == SourceKind::Mic || a.kind == SourceKind::App) return a.name == b.name;
    return a.id == b.id;
}

static std::string timestamp(double t) {
    time_t tt = (time_t)t;
    char b[16];
    strftime(b, sizeof b, "%H:%M:%S", localtime(&tt));
    return b;
}

static std::string minutes(double seconds) {
    char b[16];
    snprintf(b, sizeof b, "%02d:%02d", (int)seconds / 60, (int)seconds % 60);
    return b;
}

static void drawTitle(const View &v, int w) {
    std::string s = std::string(paint(bar_bg)) + paint(white) + paint(bold) + " earshot " + paint("\x1b[22m") + "│ ";
    bool capturing = v.status == "capturing";
    s += (v.active ? v.active->label : "no source") + " " + paint(capturing ? green : yellow) + v.status + paint(white) + " │ ";
    int meter = (int)(v.level * 10 + 0.5f);
    for (int i = 0; i < 10; i++) {
        const char *c = i >= meter ? gray : i < 6 ? green : i < 8 ? yellow : red;
        s += std::string(paint(c)) + (i < meter ? "▮" : "▯");
    }
    s += std::string(paint(white)) + " │ ";
    if (v.clip_seconds > 0) s += std::string(paint(clip_badge)) + " ● CLIP " + minutes(v.clip_seconds) + " " + paint(reset) + paint(bar_bg) + paint(white) + " ";
    if (v.playback_left > 0) s += std::string(paint(play_badge)) + " ▶ " + minutes(v.playback_left) + " " + paint(reset) + paint(bar_bg) + paint(white) + " ";
    s += "whisper " + v.model + (v.whisper_busy ? std::string(paint(yellow)) + " ●" + paint(white) : "  ") + " │ ai ";
    switch (v.llm) {
    case LlmState::Off: s += std::string(paint(gray)) + "off"; break;
    case LlmState::Idle: s += std::string(paint(green)) + "ready"; break;
    case LlmState::Starting: s += std::string(paint(yellow)) + "starting server"; break;
    case LlmState::Working: s += std::string(paint(yellow)) + "thinking"; break;
    case LlmState::Error: s += std::string(paint(red)) + "error"; break;
    }
    moveTo(1, 1);
    cell(s + paint(white), w);
    screen += paint(reset);
}

static void drawSources(const View &v, int width, int height) {
    moveTo(2, 1);
    screen += std::string(paint(bold)) + paint(cyan);
    cell(" Sources", width);
    screen += paint(reset);
    const std::vector<Source> &list = *v.sources;
    int rows = height - 3;
    int first = v.selected >= rows ? v.selected - rows + 1 : 0;
    int row = 3;
    for (int i = first; i < (int)list.size() && row < height; i++, row++) {
        const Source &s = list[i];
        bool on = v.active && sameSource(*v.active, s);
        std::string mark;
        if (on) mark = std::string(paint(green)) + "●";
        else if (s.kind == SourceKind::App) mark = std::string(paint(magenta)) + "♪";
        else if (s.kind == SourceKind::System || s.kind == SourceKind::Mic) mark = std::string(paint(blue)) + "◆";
        else mark = std::string(paint(gray)) + "·";
        bool device = s.kind == SourceKind::Output || s.kind == SourceKind::Input;
        std::string line = std::string(i == v.selected ? "›" : " ") + mark + paint(fg_default) + " " + (device && !on ? paint(dim) : "") + s.label;
        moveTo(row, 1);
        if (i == v.selected) screen += paint(inverse);
        cell(line, width);
        screen += paint(reset);
    }
    for (; row < height; row++) {
        moveTo(row, 1);
        cell("", width);
    }
}

static int drawText(int row, int col, int width, int rows, const std::vector<std::string> &lines, int scroll, const char *color) {
    int end = (int)lines.size() - scroll;
    if (end < 0) end = 0;
    int start = end - rows > 0 ? end - rows : 0;
    for (int i = 0; i < rows; i++, row++) {
        moveTo(row, col);
        screen += " ";
        screen += color;
        cell(start + i < end ? lines[start + i] : "", width - 1);
        screen += paint(reset);
    }
    return row;
}

void tuiDraw(const View &v) {
    int w, h;
    terminalSize(&w, &h);
    screen.clear();
    if (w < 50 || h < 12) {
        screen = "\x1b[H\x1b[2Jterminal too small";
        terminalWrite(screen);
        return;
    }
    const int left = w / 4 < 22 ? 22 : w / 4 > 32 ? 32 : w / 4;
    const int rx = left + 2, rw = w - left - 1;

    drawTitle(v, w);
    drawSources(v, left, h);
    for (int row = 2; row < h; row++) {
        moveTo(row, left + 1);
        screen += std::string(paint(gray)) + "│" + paint(reset);
    }

    int row = 2;
    moveTo(row++, rx);
    cell(std::string(paint(bold)) + paint(cyan) + " Instruction" + paint(reset) + paint(gray) + "  " + (v.editing ? "Enter save  Esc cancel  Ctrl-U clear" : "") + paint(reset), rw);
    std::vector<std::string> lines;
    const char *ins_color = paint(yellow);
    if (v.editing) wrap(v.edit + "▏", rw - 1, &lines);
    else if (!v.instruction.empty()) wrap(v.instruction, rw - 1, &lines);
    else {
        wrap("none so the transcript stays raw", rw - 1, &lines);
        ins_color = paint(gray);
    }
    if (lines.size() > 2) lines.erase(lines.begin(), lines.end() - 2);
    row = drawText(row, rx, rw, 2, lines, 0, ins_color);

    int avail = h - 1 - row;
    int transcript_rows = v.instruction.empty() ? avail : avail / 2;
    int processed_rows = avail - transcript_rows;

    std::vector<std::string> raw, processed;
    {
        std::lock_guard<std::mutex> lock(v.tr->mu);
        for (size_t i = 0; i < v.tr->raw.size(); i++) {
            const Line &l = v.tr->raw[i];
            bool newest = i + 1 == v.tr->raw.size();
            std::vector<std::string> parts;
            wrap(l.text, rw - 12, &parts);
            for (size_t k = 0; k < parts.size(); k++)
                raw.push_back(std::string(paint(gray)) + (k == 0 ? timestamp(l.time) : "        ") + paint(reset) + "  " + (newest ? paint(white) : "") + parts[k]);
        }
        for (const std::string &p : v.tr->processed) {
            wrap(p, rw - 2, &processed);
            processed.push_back("");
        }
    }

    moveTo(row++, rx);
    cell(std::string(paint(bold)) + paint(cyan) + " Transcript" + paint(reset) + paint(gray) + (v.scroll ? "  scrolled  PgDn to follow" : "") + paint(reset), rw);
    row = drawText(row, rx, rw, transcript_rows - 1, raw, v.scroll, "");
    if (processed_rows > 0) {
        moveTo(row++, rx);
        cell(std::string(paint(bold)) + paint(cyan) + " Processed" + paint(reset), rw);
        if (!processed.empty() && processed.back().empty()) processed.pop_back();
        drawText(row, rx, rw, processed_rows - 1, processed, 0, paint(green));
    }

    moveTo(h, 1);
    if (!v.message.empty()) {
        screen += paint(v.message_error ? error_bg : info_bg);
        cell(" " + v.message, w);
    } else {
        std::string s = paint(foot_bg);
        for (const auto &[key, what] : v.help)
            s += std::string(" ") + paint(yellow) + paint(bold) + key + paint("\x1b[22m") + paint(white) + " " + what + " ";
        screen += paint(foot_bg);
        cell(s, w);
    }
    screen += paint(reset);
    terminalWrite(screen);
}
