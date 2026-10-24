#include "earshot.h"
#include <string.h>
#include <time.h>

static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

static void write_header(FILE *f, uint64_t frames) {
    uint32_t data = (uint32_t)(frames * CHANNELS * 2);
    uint8_t h[44];
    memcpy(h, "RIFF", 4); put32(h + 4, 36 + data);
    memcpy(h + 8, "WAVEfmt ", 8); put32(h + 16, 16);
    put16(h + 20, 1); put16(h + 22, CHANNELS);
    put32(h + 24, RATE); put32(h + 28, RATE * CHANNELS * 2);
    put16(h + 32, CHANNELS * 2); put16(h + 34, 16);
    memcpy(h + 36, "data", 4); put32(h + 40, data);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, f);
    fseek(f, 0, SEEK_END);
}

bool clip_begin(Clip *c, Ring *r, const char *dir, double before_secs, const char *tag) {
    mkdirs(dir);
    time_t now = time(NULL);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&now));
    snprintf(c->path, sizeof c->path, "%s/%s-%s.wav", dir, tag, stamp);
    c->f = fopen(c->path, "wb");
    if (!c->f) return false;
    write_header(c->f, 0);
    uint64_t total = ring_total(r);
    uint64_t back = (uint64_t)(before_secs * RATE);
    c->cursor = total > back ? total - back : 0;
    uint64_t oldest = ring_oldest(r);
    if (c->cursor < oldest) c->cursor = oldest;
    c->t0 = now_sec() - (double)(total - c->cursor) / RATE;
    c->frames = 0;
    clip_pump(c, r);
    return true;
}

void clip_pump(Clip *c, Ring *r) {
    if (!c->f) return;
    static int16_t chunk[RATE * CHANNELS];
    for (;;) {
        size_t n = ring_read(r, &c->cursor, chunk, RATE);
        if (n == 0) break;
        fwrite(chunk, sizeof(int16_t) * CHANNELS, n, c->f);
        c->cursor += n;
        c->frames += n;
    }
}

void clip_end(Clip *c) {
    if (!c->f) return;
    write_header(c->f, c->frames);
    fclose(c->f);
    c->f = NULL;
}
