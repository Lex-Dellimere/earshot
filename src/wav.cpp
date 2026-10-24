#include "earshot.h"
#include <cstring>
#include <ctime>

static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

static void writeHeader(FILE *f, uint64_t frames) {
    uint32_t data = (uint32_t)(frames * capture_channels * 2);
    uint8_t h[44];
    memcpy(h, "RIFF", 4);
    put32(h + 4, 36 + data);
    memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16);
    put16(h + 20, 1);
    put16(h + 22, capture_channels);
    put32(h + 24, capture_rate);
    put32(h + 28, capture_rate * capture_channels * 2);
    put16(h + 32, capture_channels * 2);
    put16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    put32(h + 40, data);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, f);
    fseek(f, 0, SEEK_END);
}

bool clipBegin(Clip *clip, Ring *ring, const std::string &dir, double before, const std::string &tag) {
    makeDirs(dir);
    time_t now = time(nullptr);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&now));
    clip->path = dir + "/" + tag + "-" + stamp + ".wav";
    clip->file = fopen(clip->path.c_str(), "wb");
    if (!clip->file) return false;
    writeHeader(clip->file, 0);
    uint64_t total = ringTotal(ring);
    uint64_t back = (uint64_t)(before * capture_rate);
    clip->cursor = total > back ? total - back : 0;
    if (clip->cursor < ringOldest(ring)) clip->cursor = ringOldest(ring);
    clip->start_time = nowSeconds() - (double)(total - clip->cursor) / capture_rate;
    clip->frames = 0;
    clipPump(clip, ring);
    return true;
}

void clipPump(Clip *clip, Ring *ring) {
    if (!clip->file) return;
    static int16_t chunk[capture_rate * capture_channels];
    for (;;) {
        size_t n = ringRead(ring, &clip->cursor, chunk, capture_rate);
        if (n == 0) break;
        fwrite(chunk, sizeof(int16_t) * capture_channels, n, clip->file);
        clip->cursor += n;
        clip->frames += n;
    }
}

void clipEnd(Clip *clip) {
    if (!clip->file) return;
    writeHeader(clip->file, clip->frames);
    fclose(clip->file);
    clip->file = nullptr;
}
