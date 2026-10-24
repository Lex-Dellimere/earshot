#include "earshot.h"
#include <cstring>

void ringInit(Ring *ring, double seconds) {
    ring->cap = (size_t)(seconds * capture_rate);
    ring->buf.assign(ring->cap * capture_channels, 0);
    ring->total = 0;
}

void ringPush(Ring *ring, const int16_t *frames, size_t count) {
    std::lock_guard<std::mutex> lock(ring->mu);
    if (count > ring->cap) {
        frames += (count - ring->cap) * capture_channels;
        ring->total += count - ring->cap;
        count = ring->cap;
    }
    size_t pos = ring->total % ring->cap;
    size_t first = count < ring->cap - pos ? count : ring->cap - pos;
    int16_t *buf = ring->buf.data();
    memcpy(buf + pos * capture_channels, frames, first * capture_channels * sizeof(int16_t));
    memcpy(buf, frames + first * capture_channels, (count - first) * capture_channels * sizeof(int16_t));
    ring->total += count;
}

uint64_t ringTotal(Ring *ring) {
    std::lock_guard<std::mutex> lock(ring->mu);
    return ring->total;
}

static uint64_t oldest(const Ring *ring) {
    return ring->total > ring->cap ? ring->total - ring->cap : 0;
}

uint64_t ringOldest(Ring *ring) {
    std::lock_guard<std::mutex> lock(ring->mu);
    return oldest(ring);
}

size_t ringRead(Ring *ring, uint64_t *from, int16_t *out, size_t count) {
    std::lock_guard<std::mutex> lock(ring->mu);
    if (*from < oldest(ring)) *from = oldest(ring);
    if (*from > ring->total) *from = ring->total;
    uint64_t avail = ring->total - *from;
    if (count > avail) count = (size_t)avail;
    size_t pos = *from % ring->cap;
    size_t first = count < ring->cap - pos ? count : ring->cap - pos;
    const int16_t *buf = ring->buf.data();
    memcpy(out, buf + pos * capture_channels, first * capture_channels * sizeof(int16_t));
    memcpy(out + first * capture_channels, buf, (count - first) * capture_channels * sizeof(int16_t));
    return count;
}
