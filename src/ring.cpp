#include "earshot.h"
#include <stdlib.h>
#include <string.h>

void ring_init(Ring *r, double seconds) {
    r->cap = (size_t)(seconds * RATE);
    r->buf = (int16_t *)calloc(r->cap * CHANNELS, sizeof(int16_t));
    r->total = 0;
    pthread_mutex_init(&r->mu, NULL);
}

void ring_push(Ring *r, const int16_t *frames, size_t n) {
    pthread_mutex_lock(&r->mu);
    if (n > r->cap) {                        // keep only the newest cap frames
        frames += (n - r->cap) * CHANNELS;
        r->total += n - r->cap;
        n = r->cap;
    }
    size_t pos = r->total % r->cap;
    size_t first = n < r->cap - pos ? n : r->cap - pos;
    memcpy(r->buf + pos * CHANNELS, frames, first * CHANNELS * sizeof(int16_t));
    memcpy(r->buf, frames + first * CHANNELS, (n - first) * CHANNELS * sizeof(int16_t));
    r->total += n;
    pthread_mutex_unlock(&r->mu);
}

uint64_t ring_total(Ring *r) {
    pthread_mutex_lock(&r->mu);
    uint64_t t = r->total;
    pthread_mutex_unlock(&r->mu);
    return t;
}

static uint64_t oldest_locked(Ring *r) {
    return r->total > r->cap ? r->total - r->cap : 0;
}

uint64_t ring_oldest(Ring *r) {
    pthread_mutex_lock(&r->mu);
    uint64_t o = oldest_locked(r);
    pthread_mutex_unlock(&r->mu);
    return o;
}

size_t ring_read(Ring *r, uint64_t *from, int16_t *out, size_t n) {
    pthread_mutex_lock(&r->mu);
    uint64_t oldest = oldest_locked(r);
    if (*from < oldest) *from = oldest;
    if (*from > r->total) *from = r->total;
    uint64_t avail = r->total - *from;
    if (n > avail) n = (size_t)avail;
    size_t pos = *from % r->cap;
    size_t first = n < r->cap - pos ? n : r->cap - pos;
    memcpy(out, r->buf + pos * CHANNELS, first * CHANNELS * sizeof(int16_t));
    memcpy(out + first * CHANNELS, r->buf, (n - first) * CHANNELS * sizeof(int16_t));
    pthread_mutex_unlock(&r->mu);
    return n;
}
