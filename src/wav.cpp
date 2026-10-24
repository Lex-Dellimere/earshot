#include "earshot.h"
#include <algorithm>
#include <cmath>
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

static uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

// reads a 16 or 24 bit pcm or 32 bit float wav as 48k stereo frames for the frontend
bool wavRead(const std::string &path, std::vector<int16_t> *frames, std::string *err) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return *err = "cannot open " + path, false;
    std::vector<uint8_t> file;
    uint8_t buf[65536];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) file.insert(file.end(), buf, buf + n);
    fclose(f);
    if (file.size() < 12 || memcmp(file.data(), "RIFF", 4) != 0 || memcmp(file.data() + 8, "WAVE", 4) != 0)
        return *err = path + " is not a wav file", false;

    int format = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t *data = nullptr;
    size_t data_size = 0;
    for (size_t pos = 12; pos + 8 <= file.size();) {
        const uint8_t *chunk = file.data() + pos;
        size_t size = std::min<size_t>(get32(chunk + 4), file.size() - pos - 8);
        if (memcmp(chunk, "fmt ", 4) == 0 && size >= 16) {
            format = get16(chunk + 8);
            channels = get16(chunk + 10);
            rate = get32(chunk + 12);
            bits = get16(chunk + 22);
            // extensible wavs keep the real format in the first two bytes of the sub format guid
            if (format == 0xFFFE && size >= 40) format = get16(chunk + 32);
        } else if (memcmp(chunk, "data", 4) == 0) {
            data = chunk + 8;
            data_size = size;
        }
        pos += 8 + size + (size & 1);
    }
    bool pcm = format == 1 && (bits == 16 || bits == 24), flt = format == 3 && bits == 32;
    if (!data || channels < 1 || rate == 0 || !(pcm || flt))
        return *err = path + " needs 16 or 24 bit pcm or 32 bit float", false;

    int bytes = bits / 8;
    size_t count = data_size / (bytes * channels);
    auto sample = [&](size_t frame, int ch) {
        const uint8_t *p = data + (frame * channels + std::min(ch, channels - 1)) * bytes;
        if (flt) {
            float x;
            memcpy(&x, p, 4);
            return x;
        }
        if (bits == 16) return (int16_t)get16(p) / 32768.0f;
        return (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) / 2147483648.0f;
    };
    // linear resampling is fine because the frontend low passes at 7.2 khz
    double step = (double)rate / capture_rate;
    size_t out = rate == (uint32_t)capture_rate ? count : (size_t)(count / step);
    frames->resize(out * capture_channels);
    for (size_t i = 0; i < out; i++) {
        double t = i * step;
        size_t a = (size_t)t;
        float frac = (float)(t - a);
        size_t b = a + 1 < count ? a + 1 : a;
        for (int ch = 0; ch < capture_channels; ch++) {
            float x = sample(a, ch) * (1 - frac) + sample(b, ch) * frac;
            (*frames)[i * capture_channels + ch] = (int16_t)lrintf(fmaxf(-1.0f, fminf(1.0f, x)) * 32767.0f);
        }
    }
    return true;
}
