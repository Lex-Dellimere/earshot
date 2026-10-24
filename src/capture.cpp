#include "earshot.h"
#include <atomic>
#include <inttypes.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_NODES 256

struct Capture {
    pw_thread_loop *loop;
    pw_context *ctx;
    pw_core *core;
    pw_registry *registry;
    spa_hook registry_listener;
    pw_stream *stream;
    spa_hook stream_listener;
    Ring *ring;

    // Everything below is guarded by the thread-loop lock.
    Source nodes[MAX_NODES];
    int n_nodes;
    Source current;
    bool active;
    bool target_gone;       // the app we were capturing went away
    int reattach;           // index into nodes of a matching app that came back, or -1
    char status[128];

    std::atomic<float> level;
};

static void copy_str(char *dst, size_t len, const char *src) {
    snprintf(dst, len, "%s", src ? src : "");
}

static void on_process(void *data) {
    Capture *c = (Capture *)data;
    pw_buffer *b = pw_stream_dequeue_buffer(c->stream);
    if (!b) return;
    spa_data *d = &b->buffer->datas[0];
    if (d->data && d->chunk) {
        uint32_t off = SPA_MIN(d->chunk->offset, d->maxsize);
        uint32_t size = SPA_MIN(d->chunk->size, d->maxsize - off);
        const int16_t *s = (const int16_t *)SPA_PTROFF(d->data, off, void);
        size_t frames = size / (sizeof(int16_t) * CHANNELS);
        ring_push(c->ring, s, frames);
        int peak = 0;
        for (size_t i = 0; i < frames * CHANNELS; i++) {
            int v = s[i] < 0 ? -s[i] : s[i];
            if (v > peak) peak = v;
        }
        float p = peak / 32768.0f;
        if (p > c->level.load()) c->level.store(p);
    }
    pw_stream_queue_buffer(c->stream, b);
}

static void on_state(void *data, pw_stream_state, pw_stream_state state, const char *error) {
    Capture *c = (Capture *)data;
    if (state == PW_STREAM_STATE_ERROR)
        snprintf(c->status, sizeof c->status, "error: %s", error ? error : "?");
    else if (state == PW_STREAM_STATE_STREAMING)
        copy_str(c->status, sizeof c->status, "capturing");
    else if (state == PW_STREAM_STATE_PAUSED)
        copy_str(c->status, sizeof c->status, c->target_gone ? "waiting for app" : "idle (no audio)");
    else if (state == PW_STREAM_STATE_CONNECTING)
        copy_str(c->status, sizeof c->status, "connecting");
    else if (state == PW_STREAM_STATE_UNCONNECTED)
        copy_str(c->status, sizeof c->status, c->target_gone ? "waiting for app" : "unconnected");
}

static const pw_stream_events stream_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state,
    .process = on_process,
};

static void on_global(void *data, uint32_t id, uint32_t, const char *type, uint32_t,
                      const spa_dict *props) {
    Capture *c = (Capture *)data;
    if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0 || !props) return;
    const char *cls = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    if (!cls) return;

    Source s = {};
    if (strcmp(cls, "Stream/Output/Audio") == 0) s.kind = SRC_APP;
    else if (strcmp(cls, "Audio/Sink") == 0) s.kind = SRC_OUTPUT;
    else if (strcmp(cls, "Audio/Source") == 0) s.kind = SRC_INPUT;
    else return;

    s.id = id;
    const char *serial = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL);
    s.serial = serial ? strtoull(serial, NULL, 10) : id;
    const char *app = spa_dict_lookup(props, PW_KEY_APP_NAME);
    const char *node = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    const char *desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
    if (s.kind == SRC_APP) {
        copy_str(s.name, sizeof s.name, app ? app : node);
        snprintf(s.label, sizeof s.label, "%s", s.name);
    } else {
        copy_str(s.name, sizeof s.name, node);
        snprintf(s.label, sizeof s.label, "%s %s", s.kind == SRC_OUTPUT ? "out:" : "in: ",
                 desc ? desc : node ? node : "?");
    }
    if (c->n_nodes >= MAX_NODES) return;
    c->nodes[c->n_nodes++] = s;

    if (c->active && c->target_gone && c->current.kind == SRC_APP && s.kind == SRC_APP &&
        strcmp(s.name, c->current.name) == 0)
        c->reattach = c->n_nodes - 1;
}

static void on_global_remove(void *data, uint32_t id) {
    Capture *c = (Capture *)data;
    for (int i = 0; i < c->n_nodes; i++) {
        if (c->nodes[i].id != id) continue;
        if (c->active && c->current.kind == SRC_APP && c->current.id == id) {
            c->target_gone = true;
            copy_str(c->status, sizeof c->status, "waiting for app");
        }
        memmove(&c->nodes[i], &c->nodes[i + 1], (c->n_nodes - i - 1) * sizeof(Source));
        c->n_nodes--;
        if (c->reattach == i) c->reattach = -1;
        else if (c->reattach > i) c->reattach--;
        return;
    }
}

static const pw_registry_events registry_events = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = on_global,
    .global_remove = on_global_remove,
};

Capture *capture_open(Ring *ring) {
    pw_init(NULL, NULL);
    Capture *c = new Capture();
    c->ring = ring;
    c->reattach = -1;
    copy_str(c->status, sizeof c->status, "no source");
    c->loop = pw_thread_loop_new("earshot-pw", NULL);
    c->ctx = pw_context_new(pw_thread_loop_get_loop(c->loop), NULL, 0);
    pw_thread_loop_lock(c->loop);
    pw_thread_loop_start(c->loop);
    c->core = pw_context_connect(c->ctx, NULL, 0);
    if (!c->core) {
        pw_thread_loop_unlock(c->loop);
        return NULL;
    }
    c->registry = pw_core_get_registry(c->core, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(c->registry, &c->registry_listener, &registry_events, c);
    pw_thread_loop_unlock(c->loop);
    return c;
}

int capture_list(Capture *c, Source *out, int max) {
    int n = 0;
    if (n < max) { out[n] = {}; out[n].kind = SRC_SYSTEM; copy_str(out[n].label, sizeof out[n].label, "System (all output)"); copy_str(out[n].name, sizeof out[n].name, "system"); n++; }
    if (n < max) { out[n] = {}; out[n].kind = SRC_MIC; copy_str(out[n].label, sizeof out[n].label, "Mic (default input)"); copy_str(out[n].name, sizeof out[n].name, "mic"); n++; }
    pw_thread_loop_lock(c->loop);
    // apps first (one row per app, even with several streams), then devices
    int apps_end = n;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < c->n_nodes && n < max; i++) {
            const Source *s = &c->nodes[i];
            if ((s->kind == SRC_APP) != (pass == 0)) continue;
            bool dup = false;
            for (int j = 2; pass == 0 && j < apps_end && !dup; j++)
                dup = strcmp(out[j].name, s->name) == 0;
            if (dup) continue;
            out[n++] = *s;
            if (pass == 0) apps_end = n;
        }
    pw_thread_loop_unlock(c->loop);
    return n;
}

bool capture_find(Capture *c, const char *query, Source *out) {
    static Source all[MAX_NODES + 2];
    int n = capture_list(c, all, MAX_NODES + 2);
    for (int i = 0; i < n; i++)
        if (strcasecmp(all[i].name, query) == 0) { *out = all[i]; return true; }
    for (int i = 0; i < n; i++)
        if (strcasestr(all[i].label, query) || strcasestr(all[i].name, query)) { *out = all[i]; return true; }
    return false;
}

// Caller holds the loop lock.
static void connect_locked(Capture *c, const Source *s) {
    if (c->stream) {
        pw_stream_destroy(c->stream);
        c->stream = NULL;
    }
    c->current = *s;
    c->active = true;
    c->target_gone = false;
    c->reattach = -1;

    pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Music", PW_KEY_NODE_NAME, "earshot",
        PW_KEY_APP_NAME, "earshot", NULL);
    switch (s->kind) {
    case SRC_SYSTEM:
        pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");
        break;
    case SRC_MIC:
        break;
    case SRC_OUTPUT:
        pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");
        pw_properties_setf(props, PW_KEY_TARGET_OBJECT, "%" PRIu64, s->serial);
        break;
    case SRC_INPUT:
        pw_properties_setf(props, PW_KEY_TARGET_OBJECT, "%" PRIu64, s->serial);
        break;
    case SRC_APP:
        // Never fall back to the mic when the app's stream disappears.
        pw_properties_setf(props, PW_KEY_TARGET_OBJECT, "%" PRIu64, s->serial);
        pw_properties_set(props, PW_KEY_NODE_DONT_RECONNECT, "true");
        break;
    }

    c->stream = pw_stream_new(c->core, "earshot capture", props);
    pw_stream_add_listener(c->stream, &c->stream_listener, &stream_events, c);

    uint8_t buf[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
    spa_audio_info_raw info = {};
    info.format = SPA_AUDIO_FORMAT_S16;
    info.rate = RATE;
    info.channels = CHANNELS;
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
    const spa_pod *params[1] = { spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info) };
    pw_stream_connect(c->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                      (pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
                      params, 1);
    copy_str(c->status, sizeof c->status, "connecting");
}

void capture_start(Capture *c, const Source *s) {
    pw_thread_loop_lock(c->loop);
    connect_locked(c, s);
    pw_thread_loop_unlock(c->loop);
}

void capture_tick(Capture *c) {
    pw_thread_loop_lock(c->loop);
    if (c->reattach >= 0) {
        Source s = c->nodes[c->reattach];
        connect_locked(c, &s);
    }
    pw_thread_loop_unlock(c->loop);
}

bool capture_current(Capture *c, Source *out) {
    pw_thread_loop_lock(c->loop);
    bool a = c->active;
    if (a) *out = c->current;
    pw_thread_loop_unlock(c->loop);
    return a;
}

const char *capture_status(Capture *c) {
    static char s[128];
    pw_thread_loop_lock(c->loop);
    copy_str(s, sizeof s, c->status);
    pw_thread_loop_unlock(c->loop);
    return s;
}

float capture_level(Capture *c) {
    return c->level.exchange(0.0f);
}

void capture_close(Capture *c) {
    pw_thread_loop_lock(c->loop);
    if (c->stream) pw_stream_destroy(c->stream);
    pw_proxy_destroy((pw_proxy *)c->registry);
    pw_core_disconnect(c->core);
    pw_thread_loop_unlock(c->loop);
    pw_thread_loop_stop(c->loop);
    pw_context_destroy(c->ctx);
    pw_thread_loop_destroy(c->loop);
    delete c;
    pw_deinit();
}
