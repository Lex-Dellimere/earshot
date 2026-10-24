// ansi terminal ui shared by linux and windows
#include "earshot.h"
#include <cmath>
#include <cstring>
#include <ctime>

static bool color_on = true;
static std::string screen;
static int last_w = 0, last_h = 0;
static std::string keybuf;  // bytes read but not yet turned into keys

void tuiSetColor(bool on) {
    color_on = on;
}

static const char *paint(const char *code) {
    return color_on ? code : "";
}

static const char *reset = "\x1b[0m";
static const char *bold = "\x1b[1m";
static const char *normal = "\x1b[22m";
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
    if (name == "tab") return KeyTab;
    if (name.size() == 1) return (unsigned char)name[0];
    return KeyNone;
}

// returns one key per call from everything the terminal has sent so fast typing is not lost
int tuiReadKey(std::string *text) {
    char b[256];
    int n = terminalRead(b, sizeof b);
    if (n > 0) keybuf.append(b, n);
    if (keybuf.empty()) return KeyNone;
    unsigned char c = keybuf[0];
    size_t used = 1;
    int key = 0;
    if (c == 27) {
        key = KeyEsc;
        if (keybuf.size() >= 3 && (keybuf[1] == '[' || keybuf[1] == 'O')) {
            size_t end = 2;
            while (end < keybuf.size() && !(keybuf[end] >= 0x40 && keybuf[end] <= 0x7e)) end++;
            if (end == keybuf.size()) {
                keybuf.clear();
                return 0;
            }
            used = end + 1;
            std::string params = keybuf.substr(2, end - 2);
            switch (keybuf[end]) {
            case 'A': key = KeyUp; break;
            case 'B': key = KeyDown; break;
            case 'C': key = KeyRight; break;
            case 'D': key = KeyLeft; break;
            case 'H': key = KeyHome; break;
            case 'F': key = KeyEnd; break;
            case 'Z': key = KeyBackTab; break;
            case '~':
                key = params == "5" ? KeyPageUp : params == "6" ? KeyPageDown : params == "3" ? KeyDelete
                    : params == "1" || params == "7" ? KeyHome : params == "4" || params == "8" ? KeyEnd : 0;
                break;
            default: key = 0;
            }
        }
    } else if (c == '\r' || c == '\n') {
        key = KeyEnter;
    } else if (c == 127 || c == 8) {
        key = KeyBackspace;
    } else {
        used = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if (used > keybuf.size()) used = keybuf.size();
        if (text) *text = keybuf.substr(0, used);
        key = c;
    }
    keybuf.erase(0, used);
    return key;
}

// width helpers decode utf-8 and skip escape sequences

static size_t skipEscape(const std::string &s, size_t i) {
    if (s[i] != '\x1b' || i + 1 >= s.size() || s[i + 1] != '[') return i;
    i += 2;
    while (i < s.size() && !(s[i] >= 0x40 && s[i] <= 0x7e)) i++;
    return i + 1;
}

static uint32_t decode(const std::string &s, size_t *i) {
    unsigned char c = s[*i];
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    uint32_t cp = extra == 3 ? c & 7 : extra == 2 ? c & 15 : extra == 1 ? c & 31 : c;
    (*i)++;
    for (; extra > 0 && *i < s.size() && ((unsigned char)s[*i] & 0xC0) == 0x80; extra--, (*i)++)
        cp = cp << 6 | ((unsigned char)s[*i] & 63);
    return cp;
}

// east asian wide characters and emoji take two columns and combining marks none
static int charWidth(uint32_t cp) {
    if (cp < 0x300) return 1;
    if ((cp <= 0x36F) || (cp >= 0x200B && cp <= 0x200F) || (cp >= 0xFE00 && cp <= 0xFE0F)) return 0;
    static const uint32_t wide[][2] = {
        { 0x1100, 0x115F }, { 0x2E80, 0x303E }, { 0x3041, 0x33FF }, { 0x3400, 0x4DBF }, { 0x4E00, 0x9FFF },
        { 0xA000, 0xA4CF }, { 0xAC00, 0xD7A3 }, { 0xF900, 0xFAFF }, { 0xFE30, 0xFE4F }, { 0xFF00, 0xFF60 },
        { 0xFFE0, 0xFFE6 }, { 0x1F300, 0x1F64F }, { 0x1F900, 0x1F9FF }, { 0x20000, 0x3FFFD },
    };
    for (const auto &r : wide)
        if (cp >= r[0] && cp <= r[1]) return 2;
    return 1;
}

int textWidth(const std::string &s) {
    int w = 0;
    for (size_t i = 0; i < s.size();) {
        size_t j = skipEscape(s, i);
        if (j != i) {
            i = j;
            continue;
        }
        w += charWidth(decode(s, &i));
    }
    return w;
}

// bytes of s that fit in w columns
static size_t fitBytes(const std::string &s, int w) {
    int used = 0;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = skipEscape(s, i);
        if (j != i) {
            i = j;
            continue;
        }
        size_t next = i;
        int cw = charWidth(decode(s, &next));
        if (used + cw > w) break;
        used += cw;
        i = next;
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

void wrapText(const std::string &text, int w, std::vector<std::string> *out) {
    if (w < 4) w = 4;
    size_t i = 0;
    while (i < text.size()) {
        size_t n = fitBytes(text.substr(i), w);
        if (i + n < text.size() && text[i + n] != ' ') {
            size_t space = text.rfind(' ', i + n);
            if (space != std::string::npos && space > i) n = space - i;
        }
        if (n == 0) n = 1;
        out->push_back(text.substr(i, n));
        i += n;
        while (i < text.size() && text[i] == ' ') i++;
    }
    if (text.empty()) out->push_back("");
}

// peak level to lit meter cells on a -60 to 0 dbfs scale
int meterCells(float peak) {
    if (peak <= 0.001f) return 0;
    int cells = (int)lrintf((20 * log10f(peak) + 60) / 6);
    return cells < 0 ? 0 : cells > 10 ? 10 : cells;
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

static std::string meter(float level) {
    std::string s;
    int lit = meterCells(level);
    for (int i = 0; i < 10; i++) {
        const char *c = i >= lit ? gray : i < 6 ? green : i < 8 ? yellow : red;
        s += std::string(paint(c)) + (i < lit ? "▮" : "▯");
    }
    return s + paint(white);
}

// title segments in display order and dropped from the highest drop rank when space runs out
static void drawTitle(const View &v, int w) {
    struct Segment {
        std::string text;
        int drop;
    };
    std::vector<Segment> segs;
    segs.push_back({ std::string(paint(bold)) + " earshot " + paint(normal), 5 });
    for (int ch = 0; ch < channel_count; ch++) {
        const char *tint = paint(ch == You ? cyan : magenta);
        if (!v.active[ch]) {
            segs.push_back({ std::string(tint) + v.label[ch] + paint(gray) + " off" + paint(white), 6 });
            continue;
        }
        // the channel name and meter stay while the source name and state go first on narrow screens
        bool capturing = v.status[ch] == "capturing";
        std::string state = capturing ? "" : std::string(" ") + paint(yellow) + v.status[ch];
        segs.push_back({ std::string(tint) + paint(bold) + v.label[ch] + paint(normal) + " " + meter(v.level[ch]), 0 });
        segs.push_back({ v.active[ch]->label + state + paint(white), 3 });
    }
    if (v.clip_seconds > 0)
        segs.push_back({ std::string(paint(clip_badge)) + " ● CLIP " + minutes(v.clip_seconds) + " " + paint(reset) + paint(bar_bg) + paint(white), -1 });
    if (v.playback_left > 0)
        segs.push_back({ std::string(paint(play_badge)) + " ▶ " + minutes(v.playback_left) + " " + paint(reset) + paint(bar_bg) + paint(white), -1 });
    segs.push_back({ "whisper " + std::string(v.model_loading ? std::string(paint(yellow)) + "loading " + paint(white) : "") + v.model + (v.whisper_busy ? std::string(paint(yellow)) + " ●" + paint(white) : "  "), 4 });
    std::string ai = "ai " + (v.ai.empty() ? "" : v.ai + " ");
    switch (v.llm) {
    case LlmState::Off: ai += std::string(paint(gray)) + "off"; break;
    case LlmState::Idle: ai += std::string(paint(green)) + "ready"; break;
    case LlmState::Starting: ai += std::string(paint(yellow)) + "starting server"; break;
    case LlmState::Importing: ai += std::string(paint(yellow)) + "importing model"; break;
    case LlmState::Working: ai += std::string(paint(yellow)) + "thinking"; break;
    case LlmState::Error: ai += std::string(paint(red)) + "error"; break;
    }
    segs.push_back({ ai + paint(white), 3 });

    auto joined = [&]() {
        std::string s;
        for (size_t i = 0; i < segs.size(); i++) s += (i ? " │ " : "") + segs[i].text;
        return s;
    };
    std::string s = joined();
    while (textWidth(s) > w && segs.size() > 1) {
        size_t worst = 0;
        for (size_t i = 1; i < segs.size(); i++)
            if (segs[i].drop > segs[worst].drop) worst = i;
        segs.erase(segs.begin() + worst);
        s = joined();
    }
    moveTo(1, 1);
    screen += std::string(paint(bar_bg)) + paint(white);
    cell(s + paint(white), w);
    screen += paint(reset);
}

static void drawSources(const View &v, int col, int top, int width, int rows) {
    moveTo(top, col);
    screen += std::string(paint(bold)) + paint(cyan);
    std::string legend = std::string(paint(normal)) + "  " + paint(magenta) + "●" + paint(gray) + v.label[Them] + " " + paint(cyan) + "●" + paint(gray) + v.label[You];
    cell(" Sources" + legend + (v.overlay == Overlay::Sources ? "  ↑↓ Enter on/off  Esc close" : ""), width);
    screen += paint(reset);
    const std::vector<Source> &list = *v.sources;
    int room = rows - 1;
    int first = v.selected >= room ? v.selected - room + 1 : 0;
    int row = top + 1;
    for (int i = first; i < (int)list.size() && row < top + rows; i++, row++) {
        const Source &s = list[i];
        bool them = v.active[Them] && sameSource(*v.active[Them], s), you = v.active[You] && sameSource(*v.active[You], s);
        bool on = them || you;
        std::string mark;
        if (them) mark = std::string(paint(magenta)) + "●";
        else if (you) mark = std::string(paint(cyan)) + "●";
        else if (s.kind == SourceKind::App) mark = std::string(paint(magenta)) + "♪";
        else if (s.kind == SourceKind::System || s.kind == SourceKind::Mic) mark = std::string(paint(blue)) + "◆";
        else mark = std::string(paint(gray)) + "·";
        bool device = s.kind == SourceKind::Output || s.kind == SourceKind::Input;
        std::string line = std::string(i == v.selected ? "›" : " ") + mark + paint(fg_default) + " " + (device && !on ? paint(dim) : "") + s.label;
        moveTo(row, col);
        if (i == v.selected) screen += paint(inverse);
        cell(line, width);
        screen += paint(reset);
    }
    for (; row < top + rows; row++) {
        moveTo(row, col);
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

static void drawHelp(const View &v, int col, int top, int width, int rows) {
    std::vector<std::string> lines;
    for (const auto &[key, what] : v.help) {
        std::string k = key;
        while (textWidth(k) < 10) k += " ";
        lines.push_back(std::string(paint(yellow)) + paint(bold) + k + paint(normal) + paint(fg_default) + what);
    }
    moveTo(top, col);
    screen += std::string(paint(bold)) + paint(cyan);
    cell(" Keys" + std::string(paint(normal)) + paint(gray) + "  any key closes", width);
    screen += paint(reset);
    drawText(top + 1, col, width, rows - 1, lines, 0, "");
}

static void drawMain(const View &v, int rx, int rw, int top, int bottom) {
    int row = top;
    moveTo(row++, rx);
    cell(std::string(paint(bold)) + paint(cyan) + " Instruction" + paint(reset) + paint(gray) + "  " + (v.editing ? "Enter save  Esc cancel  Ctrl-U clear" : "") + paint(reset), rw);
    std::vector<std::string> lines;
    const char *ins_color = paint(yellow);
    if (v.editing) wrapText(v.edit + "▏", rw - 1, &lines);
    else if (!v.instruction.empty()) wrapText(v.instruction, rw - 1, &lines);
    else {
        wrapText("none so the ai is off", rw - 1, &lines);
        ins_color = paint(gray);
    }
    if (lines.size() > 2) lines.erase(lines.begin(), lines.end() - 2);
    row = drawText(row, rx, rw, 2, lines, 0, ins_color);

    int avail = bottom - row;
    int transcript_rows = v.ai.empty() ? avail : avail / 2;
    int processed_rows = avail - transcript_rows;

    // paragraphs with the speaker and time once at the start and a hanging indent after
    std::vector<std::string> raw, processed;
    {
        std::lock_guard<std::mutex> lock(v.tr->mu);
        std::vector<Paragraph> paras = transcriptParagraphs(v.tr, 0, nowSeconds());
        int name_w = v.labels ? std::max(textWidth(v.label[Them]), textWidth(v.label[You])) + 2 : 0;
        int indent = name_w + (v.timestamps ? 10 : 0);
        for (size_t i = 0; i < paras.size(); i++) {
            const Paragraph &p = paras[i];
            std::string head;
            if (v.timestamps) head += std::string(paint(gray)) + timestamp(p.start) + "  " + paint(reset);
            if (v.labels) {
                std::string name = v.label[p.channel];
                while (textWidth(name) < name_w) name += " ";
                head += std::string(paint(bold)) + paint(p.channel == You ? cyan : magenta) + name + paint(reset);
            }
            const char *body = i + 1 == paras.size() ? paint(white) : "";
            std::vector<std::string> parts;
            wrapText(p.text, rw - 2 - indent, &parts);
            for (size_t k = 0; k < parts.size(); k++)
                raw.push_back((k == 0 ? head : std::string(indent, ' ')) + body + parts[k]);
            if (i + 1 < paras.size()) raw.push_back("");
        }
        for (const std::string &p : v.tr->processed) {
            wrapText(p, rw - 2, &processed);
            processed.push_back("");
        }
    }

    moveTo(row++, rx);
    cell(std::string(paint(bold)) + paint(cyan) + " Transcript" + paint(reset) + paint(gray) + "  " +
         (v.scroll ? "scrolled  PgDn to follow" : v.transcript_path) + paint(reset), rw);
    row = drawText(row, rx, rw, transcript_rows - 1, raw, v.scroll, "");
    if (processed_rows > 0) {
        moveTo(row++, rx);
        cell(std::string(paint(bold)) + paint(cyan) + " AI" + paint(reset) + paint(gray) + (v.ai.empty() ? "  off" : "  " + v.ai) + paint(reset), rw);
        if (!processed.empty() && processed.back().empty()) processed.pop_back();
        drawText(row, rx, rw, processed_rows - 1, processed, 0, paint(green));
    }
}

// key hints that fit, ending with a pointer to the full list when some had to go
static void drawFooter(const View &v, int w) {
    auto hint = [](const std::string &key, const std::string &what) {
        return std::string(" ") + paint(yellow) + paint(bold) + key + paint(normal) + paint(white) + " " + what + " ";
    };
    std::string more = hint("?", "help");
    std::string s;
    bool cut = false;
    for (const auto &[key, what] : v.help) {
        std::string h = hint(key, what);
        if (textWidth(s + h + more) > w) {
            cut = true;
            break;
        }
        s += h;
    }
    if (cut) s += more;
    screen += std::string(paint(foot_bg)) + paint(white);
    cell(s, w);
}

void tuiDraw(const View &v) {
    int w, h;
    terminalSize(&w, &h);
    screen.clear();
    // a resize can leave reflowed rows behind so start from a clean screen
    if (w != last_w || h != last_h) screen = "\x1b[2J";
    last_w = w;
    last_h = h;
    if (w < 32 || h < 8) {
        screen += "\x1b[H\x1b[2Jterminal too small";
        terminalWrite(screen);
        last_w = 0;
        return;
    }
    drawTitle(v, w);
    int top = 2, bottom = h - 1, rows = bottom - top + 1;
    bool side = w >= 72;
    int rx = 1, rw = w;
    if (side) {
        const int left = w / 4 < 22 ? 22 : w / 4 > 32 ? 32 : w / 4;
        drawSources(v, 1, top, left, rows);
        for (int row = top; row <= bottom; row++) {
            moveTo(row, left + 1);
            screen += std::string(paint(gray)) + "│" + paint(reset);
        }
        rx = left + 2;
        rw = w - left - 1;
    }
    if (v.overlay == Overlay::Help) drawHelp(v, rx, top, rw, rows);
    else if (v.overlay == Overlay::Sources && !side) drawSources(v, rx, top, rw, rows);
    else drawMain(v, rx, rw, top, bottom + 1);

    moveTo(h, 1);
    if (!v.message.empty()) {
        screen += paint(v.message_error ? error_bg : info_bg);
        cell(" " + v.message, w);
    } else {
        drawFooter(v, w);
    }
    screen += paint(reset);
    terminalWrite(screen);
}

bool tuiWide() {
    int w, h;
    terminalSize(&w, &h);
    return w >= 72;
}
