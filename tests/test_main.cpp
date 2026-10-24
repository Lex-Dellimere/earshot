// unit tests for the parts of earshot that do not need audio or a model
#include "earshot.h"
#include <cmath>
#include <cstring>

static int failures = 0, checks = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        checks++;                                                            \
        auto va = (a);                                                       \
        auto vb = (b);                                                       \
        if (!(va == vb)) {                                                   \
            failures++;                                                      \
            fprintf(stderr, "%s:%d: %s == %s failed\n", __FILE__, __LINE__, #a, #b); \
        }                                                                    \
    } while (0)

static std::string scratch(const char *name) {
    return tempDir() + "/earshot-test-" + std::to_string((long long)(nowSeconds() * 1000)) + "-" + name;
}

static void testRing() {
    Ring ring;
    ringInit(&ring, 0.001);  // 48 frames
    CHECK_EQ(ring.cap, (size_t)48);
    int16_t in[100 * capture_channels], out[100 * capture_channels];
    for (int i = 0; i < 100 * capture_channels; i++) in[i] = (int16_t)i;
    ringPush(&ring, in, 30);
    ringPush(&ring, in + 30 * capture_channels, 30);  // wraps and drops the oldest 12
    CHECK_EQ(ringTotal(&ring), (uint64_t)60);
    CHECK_EQ(ringOldest(&ring), (uint64_t)12);
    uint64_t from = 0;
    size_t n = ringRead(&ring, &from, out, 100);
    CHECK_EQ(from, (uint64_t)12);
    CHECK_EQ(n, (size_t)48);
    CHECK(memcmp(out, in + 12 * capture_channels, 48 * capture_channels * sizeof(int16_t)) == 0);
    // a push bigger than the ring keeps only the newest frames
    ringPush(&ring, in, 100);
    from = 0;
    n = ringRead(&ring, &from, out, 100);
    CHECK_EQ(n, (size_t)48);
    CHECK(memcmp(out, in + 52 * capture_channels, 48 * capture_channels * sizeof(int16_t)) == 0);
}

static void testConfig() {
    std::string path = scratch("config.conf");
    remove(path.c_str());
    Config cfg;
    std::string err;
    CHECK(configLoad(&cfg, path, &err));  // writes defaults
    Config again;
    CHECK(configLoad(&again, path, &err));
    CHECK_EQ(again.model, cfg.model);
    configSet(path, "beam", "5");
    configSet(path, "instruction", "say # hi = there");
    Config set;
    CHECK(configLoad(&set, path, &err));
    CHECK_EQ(set.beam, 5);
    CHECK_EQ(set.instruction, std::string("say # hi = there"));
    CHECK(configApply(&set, "vad_threshold", "0.25"));
    CHECK(fabsf(set.vad_threshold - 0.25f) < 1e-6f);
    CHECK(!configApply(&set, "no_such_key", "1"));
    remove(path.c_str());
}

static void testWav() {
    // a 16 khz mono 16 bit wav comes back as 48k stereo
    std::string path = scratch("tone.wav");
    FILE *f = fopen(path.c_str(), "wb");
    const int rate = 16000, count = 1600;
    uint8_t h[44] = { 'R', 'I', 'F', 'F' };
    auto p32 = [&](int at, uint32_t v) { for (int i = 0; i < 4; i++) h[at + i] = (uint8_t)(v >> (8 * i)); };
    auto p16 = [&](int at, uint16_t v) { h[at] = (uint8_t)v; h[at + 1] = (uint8_t)(v >> 8); };
    p32(4, 36 + count * 2);
    memcpy(h + 8, "WAVEfmt ", 8);
    p32(16, 16);
    p16(20, 1);
    p16(22, 1);
    p32(24, rate);
    p32(28, rate * 2);
    p16(32, 2);
    p16(34, 16);
    memcpy(h + 36, "data", 4);
    p32(40, count * 2);
    fwrite(h, 1, 44, f);
    for (int i = 0; i < count; i++) {
        int16_t s = (int16_t)(10000 * sinf(2 * 3.14159265f * 440 * i / rate));
        fwrite(&s, 2, 1, f);
    }
    fclose(f);
    std::vector<int16_t> frames;
    std::string err;
    CHECK(wavRead(path, &frames, &err));
    CHECK_EQ(frames.size(), (size_t)(count * 3 * capture_channels));
    int peak = 0;
    for (size_t i = 0; i < frames.size(); i += 2) {
        CHECK_EQ(frames[i], frames[i + 1]);
        peak = std::max(peak, std::abs((int)frames[i]));
    }
    CHECK(peak > 9500 && peak <= 10001);
    CHECK(!wavRead(scratch("missing.wav"), &frames, &err));
    remove(path.c_str());
}

int main() {
    testRing();
    testConfig();
    testWav();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
