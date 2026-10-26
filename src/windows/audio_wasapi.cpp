// wasapi capture for system loopback devices and single apps
// app capture uses process loopback from windows 10 build 20348 and later
#include "../earshot.h"
#include <windows.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <algorithm>
#include <functional>
#include <thread>

// these come from the activation params header which mingw does not ship
enum ActivationType { ActivationDefault = 0, ActivationProcessLoopback = 1 };
enum LoopbackMode { IncludeProcessTree = 0, ExcludeProcessTree = 1 };
struct ProcessLoopbackParams {
    DWORD target_process_id;
    LoopbackMode mode;
};
struct ActivationParams {
    ActivationType type;
    ProcessLoopbackParams loopback;
};

static const PROPERTYKEY friendly_name_key = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };
static const GUID agile_object_iid = { 0x94ea2b94, 0xe9cc, 0x49e0, { 0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90 } };

template <typename T> static void release(T *&p) {
    if (p) p->Release();
    p = nullptr;
}

static std::string toUtf8(const wchar_t *w) {
    if (!w) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring toWide(const std::string &s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

static WAVEFORMATEX captureFormat() {
    WAVEFORMATEX f = {};
    f.wFormatTag = WAVE_FORMAT_PCM;
    f.nChannels = capture_channels;
    f.nSamplesPerSec = capture_rate;
    f.wBitsPerSample = 16;
    f.nBlockAlign = capture_channels * 2;
    f.nAvgBytesPerSec = capture_rate * f.nBlockAlign;
    return f;
}

// one capture thread feeding one channel's ring
struct Capture {
    Ring *ring;
    Source current;  // guarded by the audio mutex with active and status
    bool active = false;
    std::string status = "off";
    std::thread thread;
    std::atomic<bool> stop{ false };
    std::atomic<float> level{ 0 };
};

struct Audio {
    std::mutex mu;
    std::vector<Source> sources;
    double listed_at = 0;
    double checked_at = 0;
    Capture captures[channel_count];

    std::thread player;
    std::atomic<bool> play_stop{ false }, playing{ false };
    std::vector<int16_t> play_buf;
    std::atomic<size_t> play_pos{ 0 };
};

static void setStatus(Audio *a, Capture *c, const std::string &s) {
    std::lock_guard<std::mutex> lock(a->mu);
    c->status = s;
}

static bool isMonitor(SourceKind kind) {
    return kind == SourceKind::System || kind == SourceKind::Output;
}

static std::string processName(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return "";
    wchar_t path[MAX_PATH];
    DWORD len = MAX_PATH;
    std::string name;
    if (QueryFullProcessImageNameW(h, 0, path, &len)) {
        name = toUtf8(path);
        name = name.substr(name.find_last_of("\\/") + 1);
        if (name.size() > 4 && _stricmp(name.c_str() + name.size() - 4, ".exe") == 0) name.resize(name.size() - 4);
    }
    CloseHandle(h);
    return name;
}

static bool processAlive(DWORD pid) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return false;
    bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
}

static IMMDeviceEnumerator *enumerator() {
    IMMDeviceEnumerator *en = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void **)&en);
    return en;
}

static void listApps(IMMDeviceEnumerator *en, std::vector<Source> *out) {
    IMMDevice *dev = nullptr;
    IAudioSessionManager2 *mgr = nullptr;
    IAudioSessionEnumerator *sessions = nullptr;
    int count = 0;
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) ||
        FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void **)&mgr)) ||
        FAILED(mgr->GetSessionEnumerator(&sessions)) || FAILED(sessions->GetCount(&count)))
        count = 0;
    for (int i = 0; i < count; i++) {
        IAudioSessionControl *ctl = nullptr;
        IAudioSessionControl2 *ctl2 = nullptr;
        DWORD pid = 0;
        AudioSessionState state = AudioSessionStateExpired;
        if (SUCCEEDED(sessions->GetSession(i, &ctl)) &&
            SUCCEEDED(ctl->QueryInterface(__uuidof(IAudioSessionControl2), (void **)&ctl2)) &&
            SUCCEEDED(ctl2->GetProcessId(&pid)) && SUCCEEDED(ctl->GetState(&state)) &&
            pid != 0 && pid != GetCurrentProcessId() && state != AudioSessionStateExpired) {
            std::string name = processName(pid);
            bool seen = name.empty();
            for (const Source &s : *out) seen = seen || (s.kind == SourceKind::App && s.name == name);
            if (!seen) out->push_back({ SourceKind::App, pid, name, name });
        }
        release(ctl2);
        release(ctl);
    }
    release(sessions);
    release(mgr);
    release(dev);
}

static void listDevices(IMMDeviceEnumerator *en, EDataFlow flow, std::vector<Source> *out) {
    IMMDeviceCollection *devices = nullptr;
    UINT count = 0;
    if (FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &devices)) || FAILED(devices->GetCount(&count))) count = 0;
    for (UINT i = 0; i < count; i++) {
        IMMDevice *dev = nullptr;
        IPropertyStore *props = nullptr;
        LPWSTR id = nullptr;
        PROPVARIANT name;
        PropVariantInit(&name);
        if (SUCCEEDED(devices->Item(i, &dev)) && SUCCEEDED(dev->GetId(&id)) &&
            SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && SUCCEEDED(props->GetValue(friendly_name_key, &name))) {
            Source s;
            s.kind = flow == eRender ? SourceKind::Output : SourceKind::Input;
            s.name = toUtf8(id);
            s.id = std::hash<std::string>()(s.name);
            s.label = std::string(flow == eRender ? "out: " : "in:  ") + (name.vt == VT_LPWSTR ? toUtf8(name.pwszVal) : s.name);
            out->push_back(s);
        }
        PropVariantClear(&name);
        CoTaskMemFree(id);
        release(props);
        release(dev);
    }
    release(devices);
}

static void refreshSources(Audio *a) {
    std::vector<Source> list;
    list.push_back({ SourceKind::System, 0, "system", "system (all output)" });
    list.push_back({ SourceKind::Mic, 0, "mic", "mic (default input)" });
    if (IMMDeviceEnumerator *en = enumerator()) {
        listApps(en, &list);
        listDevices(en, eRender, &list);
        listDevices(en, eCapture, &list);
        en->Release();
    }
    std::lock_guard<std::mutex> lock(a->mu);
    a->sources = list;
    a->listed_at = nowSeconds();
}

// completion handler for async activation which must be agile
struct ActivateHandler final : IActivateAudioInterfaceCompletionHandler {
    LONG refs = 1;
    HANDLE done = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    HRESULT result = E_FAIL;
    IAudioClient *client = nullptr;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler) || riid == agile_object_iid) {
            *out = this;
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG n = InterlockedDecrement(&refs);
        if (n == 0) {
            CloseHandle(done);
            delete this;
        }
        return n;
    }
    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation *op) override {
        IUnknown *unknown = nullptr;
        HRESULT hr = E_FAIL;
        if (SUCCEEDED(op->GetActivateResult(&hr, &unknown)) && SUCCEEDED(hr))
            hr = unknown->QueryInterface(__uuidof(IAudioClient), (void **)&client);
        result = hr;
        release(unknown);
        SetEvent(done);
        return S_OK;
    }
};

static IAudioClient *processLoopbackClient(DWORD pid) {
    ActivationParams params = { ActivationProcessLoopback, { pid, IncludeProcessTree } };
    PROPVARIANT pv;
    PropVariantInit(&pv);
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof params;
    pv.blob.pBlobData = (BYTE *)&params;
    ActivateHandler *handler = new ActivateHandler();
    IActivateAudioInterfaceAsyncOperation *op = nullptr;
    IAudioClient *client = nullptr;
    if (SUCCEEDED(ActivateAudioInterfaceAsync(L"VAD\\Process_Loopback", __uuidof(IAudioClient), &pv, handler, &op)) &&
        WaitForSingleObject(handler->done, 5000) == WAIT_OBJECT_0 && SUCCEEDED(handler->result)) {
        client = handler->client;
        handler->client = nullptr;
    }
    release(handler->client);
    release(op);
    handler->Release();
    return client;
}

static IAudioClient *endpointClient(const Source &s) {
    IMMDeviceEnumerator *en = enumerator();
    if (!en) return nullptr;
    IMMDevice *dev = nullptr;
    IAudioClient *client = nullptr;
    HRESULT hr;
    if (s.kind == SourceKind::System) hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    else if (s.kind == SourceKind::Mic) hr = en->GetDefaultAudioEndpoint(eCapture, eConsole, &dev);
    else hr = en->GetDevice(toWide(s.name).c_str(), &dev);
    if (SUCCEEDED(hr)) dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&client);
    release(dev);
    en->Release();
    return client;
}

static void captureLoop(Audio *a, Capture *c, Source src) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IAudioClient *client = src.kind == SourceKind::App ? processLoopbackClient((DWORD)src.id) : endpointClient(src);
    IAudioCaptureClient *cap = nullptr;
    HANDLE ready = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    WAVEFORMATEX fmt = captureFormat();
    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    if (src.kind != SourceKind::Mic && src.kind != SourceKind::Input) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;

    HRESULT hr = client ? client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 2000000, 0, &fmt, nullptr) : E_FAIL;
    if (SUCCEEDED(hr)) hr = client->SetEventHandle(ready);
    if (SUCCEEDED(hr)) hr = client->GetService(__uuidof(IAudioCaptureClient), (void **)&cap);
    if (SUCCEEDED(hr)) hr = client->Start();
    if (FAILED(hr)) {
        char msg[64];
        snprintf(msg, sizeof msg, "error 0x%08lx", (unsigned long)hr);
        setStatus(a, c, src.kind == SourceKind::App && !client ? "app capture needs windows 10 build 20348" : msg);
    } else {
        setStatus(a, c, "capturing");
    }

    std::vector<int16_t> silence;
    while (SUCCEEDED(hr) && !c->stop) {
        // the timeout also covers drivers that never signal loopback events
        WaitForSingleObject(ready, 20);
        UINT32 packet = 0;
        while (SUCCEEDED(hr = cap->GetNextPacketSize(&packet)) && packet > 0) {
            BYTE *data;
            UINT32 frames;
            DWORD buffer_flags;
            if (FAILED(hr = cap->GetBuffer(&data, &frames, &buffer_flags, nullptr, nullptr))) break;
            const int16_t *samples = (const int16_t *)data;
            if (buffer_flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                silence.assign(frames * capture_channels, 0);
                samples = silence.data();
            }
            // a loopback would hear our own playback so skip it while playing
            if (!(a->playing && isMonitor(src.kind))) {
                ringPush(c->ring, samples, frames);
                int peak = 0;
                for (UINT32 i = 0; i < frames * capture_channels; i++) peak = std::max(peak, std::abs((int)samples[i]));
                if (peak / 32768.0f > c->level) c->level = peak / 32768.0f;
            }
            cap->ReleaseBuffer(frames);
        }
        if (FAILED(hr)) setStatus(a, c, "device lost");
    }
    if (client) client->Stop();
    release(cap);
    release(client);
    CloseHandle(ready);
    CoUninitialize();
}

static void stopCapture(Capture *c) {
    c->stop = true;
    if (c->thread.joinable()) c->thread.join();
    c->stop = false;
}

Audio *audioOpen(Ring *rings[channel_count]) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Audio *a = new Audio();
    for (int ch = 0; ch < channel_count; ch++) a->captures[ch].ring = rings[ch];
    refreshSources(a);
    return a;
}

void audioClose(Audio *a) {
    for (Capture &c : a->captures) stopCapture(&c);
    playbackStop(a);
    if (a->player.joinable()) a->player.join();
    delete a;
    CoUninitialize();
}

std::vector<Source> audioSources(Audio *a) {
    if (nowSeconds() - a->listed_at > 1.0) refreshSources(a);
    std::lock_guard<std::mutex> lock(a->mu);
    return a->sources;
}

void audioStart(Audio *a, int ch, const Source &src) {
    Capture *c = &a->captures[ch];
    stopCapture(c);
    {
        std::lock_guard<std::mutex> lock(a->mu);
        c->current = src;
        c->active = true;
        c->status = "connecting";
    }
    c->thread = std::thread(captureLoop, a, c, src);
}

void audioStop(Audio *a, int ch) {
    Capture *c = &a->captures[ch];
    stopCapture(c);
    std::lock_guard<std::mutex> lock(a->mu);
    c->active = false;
    c->status = "off";
    c->level = 0;
}

void audioTick(Audio *a) {
    if (a->player.joinable() && !a->playing) a->player.join();
    double now = nowSeconds();
    if (now - a->checked_at < 1.0) return;
    a->checked_at = now;
    for (int ch = 0; ch < channel_count; ch++) {
        Source cur;
        if (!audioCurrent(a, ch, &cur) || cur.kind != SourceKind::App || processAlive((DWORD)cur.id)) continue;
        // the app restarted so follow it to its new process
        setStatus(a, &a->captures[ch], "waiting for app");
        refreshSources(a);
        for (const Source &s : audioSources(a)) {
            if (s.kind == SourceKind::App && s.name == cur.name) {
                audioStart(a, ch, s);
                break;
            }
        }
    }
}

bool audioCurrent(Audio *a, int ch, Source *out) {
    std::lock_guard<std::mutex> lock(a->mu);
    if (a->captures[ch].active) *out = a->captures[ch].current;
    return a->captures[ch].active;
}

std::string audioStatus(Audio *a, int ch) {
    std::lock_guard<std::mutex> lock(a->mu);
    return a->captures[ch].status;
}

float audioLevel(Audio *a, int ch) {
    return a->captures[ch].level.exchange(0.0f);
}

static void playLoop(Audio *a) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Source system = { SourceKind::System, 0, "system", "" };
    IAudioClient *client = endpointClient(system);
    IAudioRenderClient *render = nullptr;
    WAVEFORMATEX fmt = captureFormat();
    UINT32 size = 0;
    HRESULT hr = client ? client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                             AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                             2000000, 0, &fmt, nullptr) : E_FAIL;
    if (SUCCEEDED(hr)) hr = client->GetBufferSize(&size);
    if (SUCCEEDED(hr)) hr = client->GetService(__uuidof(IAudioRenderClient), (void **)&render);
    if (SUCCEEDED(hr)) hr = client->Start();
    size_t total = a->play_buf.size() / capture_channels;
    while (SUCCEEDED(hr) && !a->play_stop) {
        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding))) break;
        size_t left = total - a->play_pos;
        if (left == 0 && padding == 0) break;
        UINT32 n = (UINT32)std::min<size_t>(size - padding, left);
        BYTE *data;
        if (n > 0 && SUCCEEDED(render->GetBuffer(n, &data))) {
            memcpy(data, a->play_buf.data() + a->play_pos * capture_channels, n * fmt.nBlockAlign);
            render->ReleaseBuffer(n, 0);
            a->play_pos += n;
        }
        Sleep(10);
    }
    if (client) client->Stop();
    release(render);
    release(client);
    a->playing = false;
    CoUninitialize();
}

void playbackStart(Audio *a, std::vector<int16_t> frames) {
    playbackStop(a);
    if (a->player.joinable()) a->player.join();
    a->play_buf = std::move(frames);
    a->play_pos = 0;
    a->play_stop = false;
    a->playing = true;
    a->player = std::thread(playLoop, a);
}

void playbackStop(Audio *a) {
    a->play_stop = true;
}

double playbackRemaining(Audio *a) {
    if (!a->playing) return 0;
    return (double)(a->play_buf.size() / capture_channels - a->play_pos) / capture_rate;
}
