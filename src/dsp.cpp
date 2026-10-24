// audio frontend for whisper
// 48k stereo goes through an anti alias fir down to 16k then an optional
// centre extraction that keeps sound common to both channels then a highpass
// and a slow gain control so quiet sources reach the vad at a usable level
#include "earshot.h"
#include <cmath>

constexpr int fir_taps = 63;
constexpr int fft_size = 512;
constexpr int fft_hop = fft_size / 2;
constexpr float pi = 3.14159265358979f;

struct Frontend {
    bool focus;
    float fir[fir_taps];
    std::vector<float> in_l, in_r;

    float window[fft_size];
    std::vector<float> st_l, st_r;
    float overlap[fft_size] = {};

    bool highpass;
    float b0, b1, b2, a1, a2;
    float x1 = 0, x2 = 0, y1 = 0, y2 = 0;

    bool agc;
    float envelope = 0.1f;
    float gain = 1;
    float release;
};

Frontend *frontendCreate(const Config &cfg) {
    Frontend *fe = new Frontend();
    fe->focus = cfg.focus;

    // windowed sinc lowpass at 7200 hz with a blackman window
    const float cutoff = 7200.0f / capture_rate;
    float sum = 0;
    for (int i = 0; i < fir_taps; i++) {
        float n = i - (fir_taps - 1) / 2.0f;
        float sinc = n == 0 ? 2 * cutoff : sinf(2 * pi * cutoff * n) / (pi * n);
        float w = 0.42f - 0.5f * cosf(2 * pi * i / (fir_taps - 1)) + 0.08f * cosf(4 * pi * i / (fir_taps - 1));
        fe->fir[i] = sinc * w;
        sum += fe->fir[i];
    }
    for (float &c : fe->fir) c /= sum;

    // sqrt hann on analysis and synthesis sums to one at half overlap
    for (int i = 0; i < fft_size; i++) fe->window[i] = sqrtf(0.5f - 0.5f * cosf(2 * pi * i / fft_size));

    // rbj biquad highpass
    fe->highpass = cfg.highpass > 0;
    float w0 = 2 * pi * (fe->highpass ? cfg.highpass : 100) / whisper_rate;
    float alpha = sinf(w0) / (2 * 0.7071f);
    float a0 = 1 + alpha;
    fe->b0 = (1 + cosf(w0)) / 2 / a0;
    fe->b1 = -(1 + cosf(w0)) / a0;
    fe->b2 = fe->b0;
    fe->a1 = -2 * cosf(w0) / a0;
    fe->a2 = (1 - alpha) / a0;

    fe->agc = cfg.normalize;
    fe->release = expf(-1.0f / (2.0f * whisper_rate));
    return fe;
}

void frontendDestroy(Frontend *fe) {
    delete fe;
}

static void fft(float *re, float *im, bool inverse) {
    for (int i = 1, j = 0; i < fft_size; i++) {
        int bit = fft_size >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (int len = 2; len <= fft_size; len <<= 1) {
        float angle = 2 * pi / len * (inverse ? 1 : -1);
        float wr = cosf(angle), wi = sinf(angle);
        for (int i = 0; i < fft_size; i += len) {
            float cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                int a = i + k, b = i + k + len / 2;
                float tr = re[b] * cr - im[b] * ci;
                float ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
                float next = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = next;
            }
        }
    }
}

// keeps each frequency bin in proportion to how alike the two channels are
// so a centred voice stays and hard panned instruments fade
static void focusFrame(Frontend *fe, std::vector<float> *out) {
    float lr[fft_size], li[fft_size], rr[fft_size], ri[fft_size];
    for (int i = 0; i < fft_size; i++) {
        lr[i] = fe->st_l[i] * fe->window[i];
        rr[i] = fe->st_r[i] * fe->window[i];
        li[i] = ri[i] = 0;
    }
    fft(lr, li, false);
    fft(rr, ri, false);
    for (int k = 0; k < fft_size; k++) {
        float cross = 2 * (lr[k] * rr[k] + li[k] * ri[k]);
        float power = lr[k] * lr[k] + li[k] * li[k] + rr[k] * rr[k] + ri[k] * ri[k] + 1e-12f;
        float mask = cross / power;
        mask = mask > 0 ? mask * mask : 0;
        if (mask < 0.05f) mask = 0.05f;
        lr[k] = (lr[k] + rr[k]) * 0.5f * mask;
        li[k] = (li[k] + ri[k]) * 0.5f * mask;
    }
    fft(lr, li, true);
    for (int i = 0; i < fft_size; i++) fe->overlap[i] += lr[i] / fft_size * fe->window[i];
    out->insert(out->end(), fe->overlap, fe->overlap + fft_hop);
    for (int i = 0; i < fft_size - fft_hop; i++) fe->overlap[i] = fe->overlap[i + fft_hop];
    for (int i = fft_size - fft_hop; i < fft_size; i++) fe->overlap[i] = 0;
    fe->st_l.erase(fe->st_l.begin(), fe->st_l.begin() + fft_hop);
    fe->st_r.erase(fe->st_r.begin(), fe->st_r.begin() + fft_hop);
}

void frontendProcess(Frontend *fe, const int16_t *frames, size_t count, std::vector<float> *out) {
    for (size_t i = 0; i < count; i++) {
        fe->in_l.push_back(frames[i * 2] / 32768.0f);
        fe->in_r.push_back(frames[i * 2 + 1] / 32768.0f);
    }
    size_t start = out->size();
    size_t pos = 0;
    for (; pos + fir_taps <= fe->in_l.size(); pos += 3) {
        float l = 0, r = 0;
        for (int k = 0; k < fir_taps; k++) {
            l += fe->fir[k] * fe->in_l[pos + k];
            r += fe->fir[k] * fe->in_r[pos + k];
        }
        if (fe->focus) {
            fe->st_l.push_back(l);
            fe->st_r.push_back(r);
            if (fe->st_l.size() == fft_size) focusFrame(fe, out);
        } else {
            out->push_back((l + r) * 0.5f);
        }
    }
    fe->in_l.erase(fe->in_l.begin(), fe->in_l.begin() + pos);
    fe->in_r.erase(fe->in_r.begin(), fe->in_r.begin() + pos);

    for (size_t i = start; i < out->size(); i++) {
        float x = (*out)[i];
        if (fe->highpass) {
            float y = fe->b0 * x + fe->b1 * fe->x1 + fe->b2 * fe->x2 - fe->a1 * fe->y1 - fe->a2 * fe->y2;
            fe->x2 = fe->x1;
            fe->x1 = x;
            fe->y2 = fe->y1;
            fe->y1 = y;
            x = y;
        }
        if (fe->agc) {
            // peak follower with a two second release and at most 26 db of boost
            fe->envelope = fmaxf(fabsf(x), fe->envelope * fe->release);
            float target = fminf(0.5f / fmaxf(fe->envelope, 1e-6f), 20.0f);
            fe->gain = target < fe->gain ? target : fe->gain + (target - fe->gain) * 0.0002f;
            x *= fe->gain;
        }
        (*out)[i] = x;
    }
}

// peak normalise with a gain cap so near silence is not blown up into noise
void normalizeChunk(std::vector<float> *pcm) {
    float peak = 0;
    for (float x : *pcm) peak = fmaxf(peak, fabsf(x));
    if (peak < 1e-4f) return;
    float gain = fminf(0.9f / peak, 10.0f);
    for (float &x : *pcm) x *= gain;
}
