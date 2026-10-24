#include "../earshot.h"
#include <algorithm>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

struct Audio {
    pw_thread_loop *loop;
    pw_context *context;
    pw_core *core;
    pw_registry *registry;
    spa_hook registry_listener;
    Ring *ring;

    // guarded by the thread loop lock
    std::vector<Source> nodes;
    pw_stream *capture = nullptr;
    spa_hook capture_listener;
    Source current;
    bool active = false;
    bool target_gone = false;
    std::string status = "no source";

    pw_stream *player = nullptr;
    spa_hook player_listener;
    std::vector<int16_t> play_buf;
    size_t play_pos = 0;

    std::atomic<float> level{ 0 };
    std::atomic<bool> playing{ false };
};

static bool isMonitor(SourceKind kind) {
    return kind == SourceKind::System || kind == SourceKind::Output;
}

static void onCaptureProcess(void *data) {
    Audio *a = (Audio *)data;
    pw_buffer *b = pw_stream_dequeue_buffer(a->capture);
    if (!b) return;
    spa_data *d = &b->buffer->datas[0];
    // a monitor would hear our own playback so skip it while playing
    bool echo = a->playing && isMonitor(a->current.kind);
    if (d->data && d->chunk && !echo) {
        uint32_t offset = SPA_MIN(d->chunk->offset, d->maxsize);
        uint32_t size = SPA_MIN(d->chunk->size, d->maxsize - offset);
        const int16_t *s = (const int16_t *)SPA_PTROFF(d->data, offset, void);
        size_t frames = size / (sizeof(int16_t) * capture_channels);
        ringPush(a->ring, s, frames);
        int peak = 0;
        for (size_t i = 0; i < frames * capture_channels; i++) peak = std::max(peak, std::abs((int)s[i]));
        float p = peak / 32768.0f;
        if (p > a->level) a->level = p;
    }
    pw_stream_queue_buffer(a->capture, b);
}

static void onCaptureState(void *data, pw_stream_state, pw_stream_state state, const char *error) {
    Audio *a = (Audio *)data;
    switch (state) {
    case PW_STREAM_STATE_ERROR: a->status = std::string("error ") + (error ? error : ""); break;
    case PW_STREAM_STATE_STREAMING: a->status = "capturing"; break;
    case PW_STREAM_STATE_PAUSED: a->status = a->target_gone ? "waiting for app" : "idle"; break;
    case PW_STREAM_STATE_CONNECTING: a->status = "connecting"; break;
    case PW_STREAM_STATE_UNCONNECTED: a->status = a->target_gone ? "waiting for app" : "unconnected"; break;
    }
}

static const pw_stream_events capture_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = onCaptureState,
    .process = onCaptureProcess,
};

static void onPlayerProcess(void *data) {
    Audio *a = (Audio *)data;
    pw_buffer *b = pw_stream_dequeue_buffer(a->player);
    if (!b) return;
    spa_data *d = &b->buffer->datas[0];
    if (d->data) {
        size_t stride = sizeof(int16_t) * capture_channels;
        size_t want = d->maxsize / stride;
        if (b->requested && b->requested < want) want = b->requested;
        size_t left = (a->play_buf.size() / capture_channels) - a->play_pos;
        size_t n = std::min(want, left);
        memcpy(d->data, a->play_buf.data() + a->play_pos * capture_channels, n * stride);
        a->play_pos += n;
        d->chunk->offset = 0;
        d->chunk->stride = stride;
        d->chunk->size = n * stride;
        if (n == 0) a->playing = false;
    }
    pw_stream_queue_buffer(a->player, b);
}

static const pw_stream_events player_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .process = onPlayerProcess,
};

static void onGlobal(void *data, uint32_t id, uint32_t, const char *type, uint32_t, const spa_dict *props) {
    Audio *a = (Audio *)data;
    if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0 || !props) return;
    const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    if (!media_class) return;
    Source s;
    if (strcmp(media_class, "Stream/Output/Audio") == 0) s.kind = SourceKind::App;
    else if (strcmp(media_class, "Audio/Sink") == 0) s.kind = SourceKind::Output;
    else if (strcmp(media_class, "Audio/Source") == 0) s.kind = SourceKind::Input;
    else return;

    const char *serial = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL);
    const char *app = spa_dict_lookup(props, PW_KEY_APP_NAME);
    const char *node = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    const char *desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
    if (app && strcmp(app, "earshot") == 0) return;
    uint64_t object_serial = serial ? strtoull(serial, nullptr, 10) : id;
    // id packs the object serial in the high half and the registry id in the low half
    s.id = (object_serial << 32) | id;
    if (s.kind == SourceKind::App) {
        s.name = app ? app : node ? node : "?";
        s.label = s.name;
    } else {
        s.name = node ? node : "?";
        s.label = std::string(s.kind == SourceKind::Output ? "out: " : "in:  ") + (desc ? desc : s.name.c_str());
    }
    a->nodes.push_back(s);
}

static void onGlobalRemove(void *data, uint32_t id) {
    Audio *a = (Audio *)data;
    for (size_t i = 0; i < a->nodes.size(); i++) {
        if ((uint32_t)a->nodes[i].id != id) continue;
        if (a->active && a->current.kind == SourceKind::App && a->current.id == a->nodes[i].id) {
            a->target_gone = true;
            a->status = "waiting for app";
        }
        a->nodes.erase(a->nodes.begin() + i);
        return;
    }
}

static const pw_registry_events registry_events = {
    .version = PW_VERSION_REGISTRY_EVENTS,
    .global = onGlobal,
    .global_remove = onGlobalRemove,
};

Audio *audioOpen(Ring *ring) {
    pw_init(nullptr, nullptr);
    Audio *a = new Audio();
    a->ring = ring;
    a->loop = pw_thread_loop_new("earshot", nullptr);
    a->context = pw_context_new(pw_thread_loop_get_loop(a->loop), nullptr, 0);
    pw_thread_loop_lock(a->loop);
    pw_thread_loop_start(a->loop);
    a->core = pw_context_connect(a->context, nullptr, 0);
    if (!a->core) {
        pw_thread_loop_unlock(a->loop);
        return nullptr;
    }
    a->registry = pw_core_get_registry(a->core, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(a->registry, &a->registry_listener, &registry_events, a);
    pw_thread_loop_unlock(a->loop);
    sleepMs(300);
    return a;
}

void audioClose(Audio *a) {
    pw_thread_loop_lock(a->loop);
    if (a->capture) pw_stream_destroy(a->capture);
    if (a->player) pw_stream_destroy(a->player);
    pw_proxy_destroy((pw_proxy *)a->registry);
    pw_core_disconnect(a->core);
    pw_thread_loop_unlock(a->loop);
    pw_thread_loop_stop(a->loop);
    pw_context_destroy(a->context);
    pw_thread_loop_destroy(a->loop);
    delete a;
    pw_deinit();
}

std::vector<Source> audioSources(Audio *a) {
    std::vector<Source> out;
    out.push_back({ SourceKind::System, 0, "system", "System (all output)" });
    out.push_back({ SourceKind::Mic, 0, "mic", "Mic (default input)" });
    pw_thread_loop_lock(a->loop);
    for (const Source &s : a->nodes) {
        if (s.kind != SourceKind::App) continue;
        bool seen = false;
        for (const Source &o : out) seen = seen || (o.kind == SourceKind::App && o.name == s.name);
        if (!seen) out.push_back(s);
    }
    for (const Source &s : a->nodes)
        if (s.kind != SourceKind::App) out.push_back(s);
    pw_thread_loop_unlock(a->loop);
    return out;
}

static const spa_pod *audioFormat(spa_pod_builder *b) {
    spa_audio_info_raw info = {};
    info.format = SPA_AUDIO_FORMAT_S16;
    info.rate = capture_rate;
    info.channels = capture_channels;
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
    return spa_format_audio_raw_build(b, SPA_PARAM_EnumFormat, &info);
}

static void connectCapture(Audio *a, const Source &s) {
    if (a->capture) pw_stream_destroy(a->capture);
    a->current = s;
    a->active = true;
    a->target_gone = false;

    pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Music",
        PW_KEY_NODE_NAME, "earshot", PW_KEY_APP_NAME, "earshot", nullptr);
    if (isMonitor(s.kind)) pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");
    if (s.kind == SourceKind::App || s.kind == SourceKind::Output || s.kind == SourceKind::Input)
        pw_properties_setf(props, PW_KEY_TARGET_OBJECT, "%" PRIu64, s.id >> 32);
    // never fall back to the mic when the app stream goes away
    if (s.kind == SourceKind::App) pw_properties_set(props, PW_KEY_NODE_DONT_RECONNECT, "true");

    a->capture = pw_stream_new(a->core, "earshot capture", props);
    pw_stream_add_listener(a->capture, &a->capture_listener, &capture_events, a);
    uint8_t buf[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
    const spa_pod *params[1] = { audioFormat(&b) };
    pw_stream_connect(a->capture, PW_DIRECTION_INPUT, PW_ID_ANY,
                      (pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params, 1);
    a->status = "connecting";
}

void audioStart(Audio *a, const Source &s) {
    pw_thread_loop_lock(a->loop);
    connectCapture(a, s);
    pw_thread_loop_unlock(a->loop);
}

void audioTick(Audio *a) {
    pw_thread_loop_lock(a->loop);
    if (a->active && a->target_gone) {
        for (const Source &s : a->nodes) {
            if (s.kind == SourceKind::App && s.name == a->current.name) {
                Source copy = s;
                connectCapture(a, copy);
                break;
            }
        }
    }
    if (a->player && !a->playing) {
        pw_stream_destroy(a->player);
        a->player = nullptr;
    }
    pw_thread_loop_unlock(a->loop);
}

bool audioCurrent(Audio *a, Source *out) {
    pw_thread_loop_lock(a->loop);
    bool active = a->active;
    if (active) *out = a->current;
    pw_thread_loop_unlock(a->loop);
    return active;
}

std::string audioStatus(Audio *a) {
    pw_thread_loop_lock(a->loop);
    std::string s = a->status;
    pw_thread_loop_unlock(a->loop);
    return s;
}

float audioLevel(Audio *a) {
    return a->level.exchange(0.0f);
}

void playbackStart(Audio *a, std::vector<int16_t> frames) {
    pw_thread_loop_lock(a->loop);
    if (a->player) pw_stream_destroy(a->player);
    a->play_buf = std::move(frames);
    a->play_pos = 0;
    a->playing = true;
    pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Music",
        PW_KEY_APP_NAME, "earshot", PW_KEY_NODE_NAME, "earshot playback", nullptr);
    a->player = pw_stream_new(a->core, "earshot playback", props);
    pw_stream_add_listener(a->player, &a->player_listener, &player_events, a);
    uint8_t buf[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
    const spa_pod *params[1] = { audioFormat(&b) };
    pw_stream_connect(a->player, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                      (pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params, 1);
    pw_thread_loop_unlock(a->loop);
}

void playbackStop(Audio *a) {
    a->playing = false;
}

double playbackRemaining(Audio *a) {
    if (!a->playing) return 0;
    pw_thread_loop_lock(a->loop);
    double left = (double)(a->play_buf.size() / capture_channels - a->play_pos) / capture_rate;
    pw_thread_loop_unlock(a->loop);
    return left;
}
