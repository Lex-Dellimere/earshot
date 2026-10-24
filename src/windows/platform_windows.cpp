#include "../earshot.h"
#include <windows.h>
#include <shellapi.h>
#include <direct.h>
#include <cctype>
#include <cstdlib>
#include <thread>

// the manifest sets the process code page to utf8 so narrow apis take utf8

static std::string env(const char *name, const std::string &fallback) {
    const char *v = getenv(name);
    return v && *v ? v : fallback;
}

std::string homeDir() { return env("USERPROFILE", "."); }
std::string configDir() { return env("APPDATA", homeDir()) + "\\earshot"; }
std::string dataDir() { return env("LOCALAPPDATA", homeDir()) + "\\earshot"; }
std::string tempDir() { return env("TEMP", homeDir()); }

void makeDirs(const std::string &path) {
    for (size_t i = 1; i <= path.size(); i++)
        if (i == path.size() || path[i] == '/' || path[i] == '\\') _mkdir(path.substr(0, i).c_str());
}

bool writePrivateFile(const std::string &path, const std::string &data) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    return ok;
}

void sleepMs(int ms) { Sleep(ms); }

double nowSeconds() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000ULL) / 1e7;
}

// terminal

static DWORD saved_in_mode, saved_out_mode;

void terminalInit() {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE), out = GetStdHandle(STD_OUTPUT_HANDLE);
    GetConsoleMode(in, &saved_in_mode);
    GetConsoleMode(out, &saved_out_mode);
    SetConsoleMode(in, ENABLE_VIRTUAL_TERMINAL_INPUT);
    SetConsoleMode(out, saved_out_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN);
    terminalWrite("\x1b[?1049h\x1b[?25l\x1b[?7l\x1b[2J");
}

void terminalRestore() {
    terminalWrite("\x1b[0m\x1b[?7h\x1b[?25h\x1b[?1049l");
    SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), saved_in_mode);
    SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), saved_out_mode);
}

int terminalRead(char *buf, int len) {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD count = 0;
    if (!GetNumberOfConsoleInputEvents(in, &count) || count == 0) return 0;
    INPUT_RECORD records[32];
    if (!ReadConsoleInputW(in, records, 32, &count)) return 0;
    std::wstring chars;
    for (DWORD i = 0; i < count; i++) {
        const KEY_EVENT_RECORD &k = records[i].Event.KeyEvent;
        if (records[i].EventType == KEY_EVENT && k.bKeyDown && k.uChar.UnicodeChar) chars.push_back(k.uChar.UnicodeChar);
    }
    if (chars.empty()) return 0;
    return WideCharToMultiByte(CP_UTF8, 0, chars.data(), (int)chars.size(), buf, len, nullptr, nullptr);
}

void terminalSize(int *w, int *h) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info)) {
        *w = 80;
        *h = 24;
        return;
    }
    *w = info.srWindow.Right - info.srWindow.Left + 1;
    *h = info.srWindow.Bottom - info.srWindow.Top + 1;
}

void terminalWrite(const std::string &s) {
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s.data(), (DWORD)s.size(), &written, nullptr);
}

// processes

static std::string commandLine(const std::vector<std::string> &args) {
    std::string line;
    for (const std::string &a : args) {
        if (!line.empty()) line.push_back(' ');
        if (!a.empty() && a.find_first_of(" \t\"") == std::string::npos) {
            line += a;
            continue;
        }
        // backslashes are literal unless they precede a quote where they double
        line.push_back('"');
        size_t slashes = 0;
        for (char c : a) {
            if (c == '\\') {
                slashes++;
                line.push_back(c);
                continue;
            }
            if (c == '"') line.append(slashes + 1, '\\');
            slashes = 0;
            line.push_back(c);
        }
        line.append(slashes, '\\');
        line.push_back('"');
    }
    return line;
}

int runProcess(const std::vector<std::string> &args, std::string *out) {
    SECURITY_ATTRIBUTES sa = { sizeof sa, nullptr, TRUE };
    HANDLE read_end = nullptr, write_end = nullptr;
    if (out && !CreatePipe(&read_end, &write_end, &sa, 0)) return -1;
    if (out) SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si = { sizeof si };
    if (out) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = write_end;
        si.hStdError = write_end;
    }
    PROCESS_INFORMATION pi = {};
    std::string line = commandLine(args);
    BOOL ok = CreateProcessA(nullptr, line.data(), nullptr, nullptr, TRUE, out ? CREATE_NO_WINDOW : 0, nullptr, nullptr, &si, &pi);
    if (write_end) CloseHandle(write_end);
    if (!ok) {
        if (read_end) CloseHandle(read_end);
        return -1;
    }
    if (out) {
        out->clear();
        char buf[4096];
        DWORD n;
        while (ReadFile(read_end, buf, sizeof buf, &n, nullptr) && n > 0) out->append(buf, n);
        CloseHandle(read_end);
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

struct Process {
    HANDLE process;
    HANDLE job;
};

Process *spawnProcess(const std::vector<std::string> &args) {
    if (args.empty()) return nullptr;
    // a kill on close job ends the child with us even if we crash
    HANDLE job = CreateJobObjectA(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi = {};
    std::string line = commandLine(args);
    if (!CreateProcessA(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
        CloseHandle(job);
        return nullptr;
    }
    AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    return new Process{ pi.hProcess, job };
}

void killProcess(Process *proc) {
    TerminateJobObject(proc->job, 0);
    WaitForSingleObject(proc->process, 2000);
    CloseHandle(proc->process);
    CloseHandle(proc->job);
    delete proc;
}

// commands from the control pipe and hotkeys arrive on other threads

static std::mutex command_mu;
static std::vector<std::string> commands;

static void pushCommand(const std::string &cmd) {
    std::lock_guard<std::mutex> lock(command_mu);
    commands.push_back(cmd);
}

bool pollCommand(std::string *cmd) {
    std::lock_guard<std::mutex> lock(command_mu);
    if (commands.empty()) return false;
    *cmd = commands.front();
    commands.erase(commands.begin());
    return true;
}

void openPath(const std::string &path) {
    int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n : 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), n);
    ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static std::atomic<bool> *quit_flag;

static BOOL WINAPI onConsoleEvent(DWORD) {
    if (quit_flag) *quit_flag = true;
    return TRUE;
}

void platformInit(std::atomic<bool> *quit) {
    quit_flag = quit;
    SetConsoleCtrlHandler(onConsoleEvent, TRUE);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}

// control pipe

static const char *pipe_name = "\\\\.\\pipe\\earshot";

bool controlSend(const std::string &cmd) {
    HANDLE h = CreateFileA(pipe_name, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE && WaitNamedPipeA(pipe_name, 1000))
        h = CreateFileA(pipe_name, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::string line = cmd + "\n";
    DWORD written;
    bool ok = WriteFile(h, line.data(), (DWORD)line.size(), &written, nullptr);
    CloseHandle(h);
    return ok;
}

static HANDLE createPipe() {
    return CreateNamedPipeA(pipe_name, PIPE_ACCESS_INBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
                            PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 0, 512, 0, nullptr);
}

static void pipeServer(HANDLE h) {
    for (;;) {
        if (ConnectNamedPipe(h, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
            std::string data;
            char buf[256];
            DWORD n;
            while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n > 0) data.append(buf, n);
            size_t start = 0, nl;
            while ((nl = data.find('\n', start)) != std::string::npos) {
                pushCommand(data.substr(start, nl - start));
                start = nl + 1;
            }
        }
        DisconnectNamedPipe(h);
    }
}

bool controlListen(std::string *err) {
    HANDLE h = createPipe();
    if (h == INVALID_HANDLE_VALUE) {
        *err = "another earshot is already running";
        return false;
    }
    std::thread(pipeServer, h).detach();
    return true;
}

// global hotkeys need a thread with its own message loop

static UINT virtualKey(const std::string &name) {
    if (name.size() == 1 && isalnum((unsigned char)name[0])) return (UINT)toupper((unsigned char)name[0]);
    if (name == "space") return VK_SPACE;
    if (name == "pause") return VK_PAUSE;
    if (name == "insert") return VK_INSERT;
    if (name == "home") return VK_HOME;
    if (name == "end") return VK_END;
    if (name.size() >= 2 && (name[0] == 'f' || name[0] == 'F')) {
        int n = atoi(name.c_str() + 1);
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    return 0;
}

static void hotkeyLoop(std::vector<std::pair<std::string, std::string>> binds, std::string *report, HANDLE ready) {
    std::vector<std::string> names;
    for (const auto &[command, spec] : binds) {
        if (spec.empty()) continue;
        UINT mods = MOD_NOREPEAT, key = 0;
        size_t start = 0;
        for (;;) {
            size_t plus = spec.find('+', start);
            std::string part = spec.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
            if (part == "ctrl") mods |= MOD_CONTROL;
            else if (part == "alt") mods |= MOD_ALT;
            else if (part == "shift") mods |= MOD_SHIFT;
            else if (part == "super" || part == "win") mods |= MOD_WIN;
            else key = virtualKey(part);
            if (plus == std::string::npos) break;
            start = plus + 1;
        }
        if (!key) *report += "bad hotkey " + spec + "  ";
        else if (!RegisterHotKey(nullptr, (int)names.size() + 1, mods, key)) *report += spec + " is taken by another app  ";
        else names.push_back(command);
    }
    SetEvent(ready);
    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0) > 0)
        if (msg.message == WM_HOTKEY && msg.wParam >= 1 && msg.wParam <= names.size()) pushCommand(names[msg.wParam - 1]);
}

void hotkeysStart(const std::vector<std::pair<std::string, std::string>> &binds, std::string *report) {
    HANDLE ready = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    std::thread(hotkeyLoop, binds, report, ready).detach();
    WaitForSingleObject(ready, 2000);
    CloseHandle(ready);
}
