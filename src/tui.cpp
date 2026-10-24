// Plain ANSI/termios TUI — no ncurses.
#include "earshot.h"
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static termios saved;
static std::string screen;

void tui_init(void) {
    tcgetattr(0, &saved);
    termios t = saved;
    t.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    t.c_oflag &= ~OPOST;
    t.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    t.c_cflag |= CS8;
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSAFLUSH, &t);
    const char *enter = "\x1b[?1049h\x1b[?25l\x1b[2J";
    write(1, enter, strlen(enter));
}

void tui_shutdown(void) {
    const char *leave = "\x1b[0m\x1b[?25h\x1b[?1049l";
    write(1, leave, strlen(leave));
    tcsetattr(0, TCSAFLUSH, &saved);
}

void tui_size(int *w, int *h) {
    winsize ws = {};
    ioctl(1, TIOCGWINSZ, &ws);
    *w = ws.ws_col ? ws.ws_col : 80;
    *h = ws.ws_row ? ws.ws_row : 24;
}

int tui_key(char *utf8, size_t len) {
    pollfd p = { 0, POLLIN, 0 };
    if (poll(&p, 1, 0) <= 0) return KEY_NONE;
    unsigned char b[16];
    ssize_t n = read(0, b, sizeof b);
    if (n <= 0) return KEY_NONE;
    if (b[0] == 27) {
        if (n == 1) return KEY_ESC;
        if (n >= 3 && (b[1] == '[' || b[1] == 'O')) {
            switch (b[2]) {
            case 'A': return KEY_UP;
            case 'B': return KEY_DOWN;
            case 'C': return KEY_RIGHT;
            case 'D': return KEY_LEFT;
            case '5': return KEY_PGUP;
            case '6': return KEY_PGDN;
            }
        }
        return KEY_NONE;
    }
    if (b[0] == '\r' || b[0] == '\n') return KEY_ENTER;
    if (b[0] == 127 || b[0] == 8) return KEY_BACKSPACE;
    if (utf8 && (b[0] >= 0x20 || b[0] >= 0x80)) {
        size_t m = (size_t)n < len - 1 ? (size_t)n : len - 1;
        memcpy(utf8, b, m);
        utf8[m] = 0;
    }
    return b[0];
}

// ---- drawing helpers (column counts are in code points) ----

static int cols(const char *s, size_t n) {
    int c = 0;
    for (size_t i = 0; i < n && s[i]; i++)
        if (((unsigned char)s[i] & 0xC0) != 0x80) c++;
    return c;
}

// Byte length of the longest prefix of s that fits in w columns.
static size_t fit(const char *s, int w) {
    size_t i = 0;
    int c = 0;
    while (s[i]) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) {
            if (c == w) break;
            c++;
        }
        i++;
    }
    return i;
}

static void at(int row, int col) {
    char b[24];
    snprintf(b, sizeof b, "\x1b[%d;%dH", row, col);
    screen += b;
}

// Writes s clipped/padded to exactly w columns.
static void cell(const char *s, int w) {
    size_t n = fit(s, w);
    screen.append(s, n);
    for (int c = cols(s, n); c < w; c++) screen.push_back(' ');
}

static void wrap(const std::string &text, int w, std::vector<std::string> *out) {
    if (w < 4) w = 4;
    const char *s = text.c_str();
    while (*s) {
        size_t n = fit(s, w);
        if (s[n] && s[n] != ' ') {           // break at the last space if we can
            size_t sp = n;
            while (sp > 0 && s[sp] != ' ') sp--;
            if (sp > 0) n = sp;
        }
        out->push_back(std::string(s, n));
        s += n;
        while (*s == ' ') s++;
    }
    if (text.empty()) out->push_back("");
}

static const char *kind_tag(SrcKind k) {
    switch (k) {
    case SRC_SYSTEM: return "◆";
    case SRC_MIC: return "◆";
    case SRC_APP: return "♪";
    default: return "·";
    }
}

void tui_draw(const View *v) {
    int W, H;
    tui_size(&W, &H);
    if (W < 50 || H < 12) {
        screen = "\x1b[H\x1b[2Jterminal too small";
        write(1, screen.data(), screen.size());
        return;
    }
    const int L = W / 4 < 30 ? (W / 4 > 22 ? W / 4 : 22) : 30;   // left column
    const int RX = L + 2, RW = W - L - 1;
    char buf[512];
    screen.clear();

    // ---- title bar ----
    at(1, 1);
    screen += "\x1b[7m";
    int meter = (int)(v->level * 10 + 0.5f);
    char bar[64] = "";
    for (int i = 0; i < 10; i++) strcat(bar, i < meter ? "▮" : "▯");
    char clip[48] = "";
    if (v->clipping)
        snprintf(clip, sizeof clip, " ● CLIP %02d:%02d │", (int)v->clip_secs / 60, (int)v->clip_secs % 60);
    snprintf(buf, sizeof buf, " earshot │ %s: %s │%s %s │ whisper %s%s │ llm %s",
             v->active ? v->active->label : "no source", v->capture_status, clip, bar,
             v->model_name, v->whisper_busy ? "…" : "", v->llm_state);
    cell(buf, W);
    screen += "\x1b[0m";

    // ---- left: sources ----
    int row = 2;
    at(row++, 1);
    screen += "\x1b[1m"; cell(" Sources", L); screen += "\x1b[0m";
    int list_h = H - 3;
    int first = v->selected >= list_h - 1 ? v->selected - list_h + 2 : 0;
    for (int i = first; i < v->n_sources && row < H; i++, row++) {
        const Source *s = &v->sources[i];
        bool on = v->active && v->active->kind == s->kind &&
                  (s->kind <= SRC_MIC ? true : v->active->serial == s->serial ||
                   (s->kind == SRC_APP && strcmp(v->active->name, s->name) == 0));
        snprintf(buf, sizeof buf, "%s%s %s", i == v->selected ? "›" : " ", on ? "●" : kind_tag(s->kind), s->label);
        at(row, 1);
        if (i == v->selected) screen += "\x1b[7m";
        cell(buf, L);
        screen += "\x1b[0m";
    }
    for (; row < H; row++) { at(row, 1); cell("", L); }
    for (row = 2; row < H; row++) { at(row, L + 1); screen += "\x1b[2m│\x1b[0m"; }

    // ---- right: instruction ----
    row = 2;
    at(row++, RX);
    screen += "\x1b[1m"; cell(" Instruction", RW); screen += "\x1b[0m";
    std::vector<std::string> ins;
    wrap(v->editing ? std::string(v->edit_buf) + "▏" : (*v->instruction ? v->instruction : "(none — raw transcript only; press i to set one)"), RW - 1, &ins);
    for (size_t i = 0; i < 2; i++) {
        at(row++, RX);
        screen += v->editing ? "\x1b[33m " : "\x1b[2m ";
        cell(i < ins.size() ? ins[i].c_str() : "", RW - 1);
        screen += "\x1b[0m";
    }

    // ---- right: transcript (top) and processed (bottom) ----
    int avail = H - 1 - row;                 // rows until footer
    int t_h = *v->instruction ? avail / 2 : avail;
    int p_h = avail - t_h;

    pthread_mutex_lock(&v->tr->mu);
    std::vector<std::string> tl;
    for (const Line &l : v->tr->raw) {
        time_t tt = (time_t)l.t;
        char ts[16];
        strftime(ts, sizeof ts, "%H:%M:%S", localtime(&tt));
        std::vector<std::string> w;
        wrap(l.text, RW - 12, &w);
        for (size_t i = 0; i < w.size(); i++)
            tl.push_back(std::string(i == 0 ? ts : "        ") + "  " + w[i]);
    }
    std::vector<std::string> pl;
    for (const std::string &p : v->tr->processed) wrap(p, RW - 2, &pl);
    pthread_mutex_unlock(&v->tr->mu);

    at(row++, RX);
    snprintf(buf, sizeof buf, " Transcript%s", v->scroll ? "  (scrolled — PgDn)" : "");
    screen += "\x1b[1m"; cell(buf, RW); screen += "\x1b[0m";
    int body = t_h - 1;
    int end = (int)tl.size() - v->scroll;
    if (end < 0) end = 0;
    int start = end - body > 0 ? end - body : 0;
    for (int i = 0; i < body; i++, row++) {
        at(row, RX);
        screen += " ";
        cell(start + i < end ? tl[start + i].c_str() : "", RW - 1);
    }
    if (p_h > 0) {
        at(row++, RX);
        screen += "\x1b[1m"; cell(" Processed", RW); screen += "\x1b[0m";
        body = p_h - 1;
        start = (int)pl.size() - body > 0 ? (int)pl.size() - body : 0;
        for (int i = 0; i < body; i++, row++) {
            at(row, RX);
            screen += " \x1b[36m";
            cell(start + i < (int)pl.size() ? pl[start + i].c_str() : "", RW - 1);
            screen += "\x1b[0m";
        }
    }

    // ---- footer ----
    at(H, 1);
    screen += "\x1b[7m";
    if (v->editing)
        cell(" editing instruction — Enter save · Esc cancel", W);
    else if (v->message && *v->message)
        snprintf(buf, sizeof buf, " %s", v->message), cell(buf, W);
    else
        cell(" q quit · ↑↓ Enter source · c clip · r replay · i instruction · p process · x clear · PgUp/Dn scroll", W);
    screen += "\x1b[0m";
    write(1, screen.data(), screen.size());
}
