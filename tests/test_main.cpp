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

static void testText() {
    CHECK_EQ(textWidth("abc"), 3);
    CHECK_EQ(textWidth("\x1b[1;31mab\x1b[0m"), 2);
    CHECK_EQ(textWidth("↑↓ é"), 4);
    CHECK_EQ(textWidth("日本語"), 6);
    CHECK_EQ(textWidth("e\xcc\x81"), 1);  // e plus a combining accent
    std::vector<std::string> lines;
    wrapText("the quick brown fox jumps", 10, &lines);
    CHECK_EQ(lines.size(), (size_t)3);
    CHECK_EQ(lines[0], std::string("the quick"));
    CHECK_EQ(lines[2], std::string("jumps"));
    lines.clear();
    wrapText("日本語のテキスト", 6, &lines);
    CHECK_EQ(lines.size(), (size_t)3);
    for (const std::string &l : lines) CHECK(textWidth(l) <= 6);
    lines.clear();
    wrapText("", 10, &lines);
    CHECK_EQ(lines.size(), (size_t)1);
}

static void testMeter() {
    CHECK_EQ(meterCells(0), 0);
    CHECK_EQ(meterCells(1.0f), 10);
    CHECK_EQ(meterCells(0.35f), 8);   // a loud voice
    CHECK_EQ(meterCells(0.03f), 5);   // quiet speech still shows
    CHECK_EQ(meterCells(2.0f), 10);
}

static void testTranscript() {
    Transcript tr;
    double t0 = 1000;
    transcriptAdd(&tr, { t0, "hello there how are you", Them, t0 + 2 });
    transcriptAdd(&tr, { t0 + 2.5, "I am fine thanks", Them, t0 + 4 });  // short pause joins
    transcriptAdd(&tr, { t0 + 4.5, "good to hear", You, t0 + 5.5 });
    transcriptAdd(&tr, { t0 + 20, "much later now", You, t0 + 21 });  // long pause splits
    double now = nowSeconds();
    std::vector<Paragraph> p = transcriptParagraphs(&tr, 0, now);
    CHECK_EQ(p.size(), (size_t)3);
    CHECK_EQ(p[0].text, std::string("hello there how are you I am fine thanks"));
    CHECK_EQ(p[0].channel, (int)Them);
    CHECK(p[0].final);
    CHECK_EQ(p[1].channel, (int)You);
    CHECK(!p[1].final);  // a you line waits in case it turns out to be an echo
    CHECK(transcriptParagraphs(&tr, 0, now + 10)[1].final);
    CHECK(!transcriptParagraphs(&tr, 0, now + 1)[2].final);
    bool speaking[channel_count] = { true, false };
    transcriptAdd(&tr, { t0 + 30, "them again", Them, t0 + 31 });
    CHECK(!transcriptParagraphs(&tr, 0, now + 10, speaking).back().final);  // more may join while them speaks
    speaking[Them] = false;
    CHECK(transcriptParagraphs(&tr, 0, now + 10, speaking).back().final);
    // a you line repeating them at the same moment is speaker bleed whichever arrives first
    Transcript echo;
    transcriptAdd(&echo, { t0, "ask what you can do for your country", You, t0 + 3 });
    transcriptAdd(&echo, { t0 + 0.5, "ask what you can do for your country!", Them, t0 + 3 });
    transcriptAdd(&echo, { t0 + 1, "What your country can do for you", Them, t0 + 3 });
    transcriptAdd(&echo, { t0 + 1.2, "what your country can do for you", You, t0 + 3 });
    transcriptAdd(&echo, { t0 + 2, "no that is not what I said", You, t0 + 3 });
    CHECK(echo.raw[0].dropped);
    CHECK(echo.raw[3].dropped);
    CHECK(!echo.raw[4].dropped);
    std::vector<Paragraph> e = transcriptParagraphs(&echo, 0, now + 10);
    CHECK_EQ(e.size(), (size_t)2);
    CHECK_EQ(e[1].text, std::string("no that is not what I said"));
}

int main() {
    testTranscript();
    testText();
    testMeter();
    testRing();
    testConfig();
    testWav();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
