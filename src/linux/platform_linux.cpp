#include "../earshot.h"
#include <X11/Xlib.h>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

// paths

static std::string env(const char *name, const std::string &fallback) {
    const char *v = getenv(name);
    return v && *v ? v : fallback;
}

std::string homeDir() { return env("HOME", "."); }
std::string configDir() { return env("XDG_CONFIG_HOME", homeDir() + "/.config") + "/earshot"; }
std::string dataDir() { return env("XDG_DATA_HOME", homeDir() + "/.local/share") + "/earshot"; }
std::string tempDir() { return env("XDG_RUNTIME_DIR", "/tmp"); }

void makeDirs(const std::string &path) {
    for (size_t i = 1; i <= path.size(); i++)
        if (i == path.size() || path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
}

bool writePrivateFile(const std::string &path, const std::string &data) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    bool ok = write(fd, data.data(), data.size()) == (ssize_t)data.size();
    close(fd);
    return ok;
}

void sleepMs(int ms) { usleep(ms * 1000); }

double nowSeconds() {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// terminal

static termios saved_termios;

void terminalInit() {
    tcgetattr(0, &saved_termios);
    termios t = saved_termios;
    t.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    t.c_oflag &= ~OPOST;
    t.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    t.c_cflag |= CS8;
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSAFLUSH, &t);
    terminalWrite("\x1b[?1049h\x1b[?25l\x1b[?7l\x1b[2J");
}

void terminalRestore() {
    terminalWrite("\x1b[0m\x1b[?7h\x1b[?25h\x1b[?1049l");
    tcsetattr(0, TCSAFLUSH, &saved_termios);
}

int terminalRead(char *buf, int len) {
    pollfd p = { 0, POLLIN, 0 };
    if (poll(&p, 1, 0) <= 0) return 0;
    ssize_t n = read(0, buf, len);
    return n > 0 ? (int)n : 0;
}

void terminalSize(int *w, int *h) {
    winsize ws = {};
    ioctl(1, TIOCGWINSZ, &ws);
    *w = ws.ws_col ? ws.ws_col : 80;
    *h = ws.ws_row ? ws.ws_row : 24;
}

void terminalWrite(const std::string &s) {
    size_t done = 0;
    while (done < s.size()) {
        ssize_t n = write(1, s.data() + done, s.size() - done);
        if (n <= 0) break;
        done += n;
    }
}

// processes

static std::vector<char *> toArgv(const std::vector<std::string> &args) {
    std::vector<char *> v;
    for (const std::string &a : args) v.push_back((char *)a.c_str());
    v.push_back(nullptr);
    return v;
}

int runProcess(const std::vector<std::string> &args, std::string *out) {
    int fd[2] = { -1, -1 };
    if (out && pipe(fd) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (out) {
            dup2(fd[1], 1);
            dup2(fd[1], 2);
            close(fd[0]);
            close(fd[1]);
        }
        std::vector<char *> v = toArgv(args);
        execvp(v[0], v.data());
        _exit(127);
    }
    if (out) {
        close(fd[1]);
        out->clear();
        char buf[4096];
        ssize_t n;
        while ((n = read(fd[0], buf, sizeof buf)) > 0) out->append(buf, n);
        close(fd[0]);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status) == 127 ? -1 : WEXITSTATUS(status);
}

struct Process {
    pid_t pid;
};

Process *spawnProcess(const std::vector<std::string> &args) {
    if (args.empty()) return nullptr;
    pid_t pid = fork();
    if (pid < 0) return nullptr;
    if (pid == 0) {
        // dies with us even if we crash
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        int null = open("/dev/null", O_RDWR);
        dup2(null, 0);
        dup2(null, 1);
        dup2(null, 2);
        std::vector<char *> v = toArgv(args);
        execvp(v[0], v.data());
        _exit(127);
    }
    return new Process{ pid };
}

void killProcess(Process *proc) {
    kill(proc->pid, SIGTERM);
    waitpid(proc->pid, nullptr, 0);
    delete proc;
}

static std::atomic<bool> *quit_flag;

static void onSignal(int) {
    if (quit_flag) *quit_flag = true;
}

void platformInit(std::atomic<bool> *quit) {
    quit_flag = quit;
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    signal(SIGHUP, onSignal);
    signal(SIGPIPE, SIG_IGN);
}

// control fifo lets wayland compositors and scripts drive a running earshot

static std::string fifoPath() { return tempDir() + "/earshot.ctl"; }
static int fifo_read = -1, fifo_keep = -1;
static std::string fifo_buf;
static std::vector<std::string> pending;

bool controlSend(const std::string &cmd) {
    int fd = open(fifoPath().c_str(), O_WRONLY | O_NONBLOCK);
    if (fd < 0) return false;
    std::string line = cmd + "\n";
    bool ok = write(fd, line.data(), line.size()) == (ssize_t)line.size();
    close(fd);
    return ok;
}

bool controlListen(std::string *err) {
    std::string path = fifoPath();
    int probe = open(path.c_str(), O_WRONLY | O_NONBLOCK);
    if (probe >= 0) {
        close(probe);
        *err = "another earshot is already running";
        return false;
    }
    unlink(path.c_str());
    if (mkfifo(path.c_str(), 0600) != 0) {
        *err = "cannot create " + path;
        return false;
    }
    fifo_read = open(path.c_str(), O_RDONLY | O_NONBLOCK);
    // holding a writer open stops reads from hitting end of file
    fifo_keep = open(path.c_str(), O_WRONLY | O_NONBLOCK);
    return fifo_read >= 0;
}

// x11 hotkeys through a runtime loaded libx11 so the binary runs without it

struct X11 {
    void *lib;
    Display *(*open_display)(const char *);
    Window (*default_root_window)(Display *);
    KeySym (*string_to_keysym)(const char *);
    KeyCode (*keysym_to_keycode)(Display *, KeySym);
    int (*grab_key)(Display *, int, unsigned, Window, Bool, int, int);
    int (*sync)(Display *, Bool);
    XErrorHandler (*set_error_handler)(XErrorHandler);
    int (*pending_events)(Display *);
    int (*next_event)(Display *, XEvent *);
    Display *display;
};

struct Hotkey {
    std::string command;
    KeyCode code;
    unsigned mods;
};

static X11 x11;
static std::vector<Hotkey> hotkeys;
static bool grab_failed;

static int onXError(Display *, XErrorEvent *) {
    grab_failed = true;
    return 0;
}

static bool loadX11() {
    x11.lib = dlopen("libX11.so.6", RTLD_LAZY);
    if (!x11.lib) return false;
    x11.open_display = (Display * (*)(const char *)) dlsym(x11.lib, "XOpenDisplay");
    x11.default_root_window = (Window(*)(Display *))dlsym(x11.lib, "XDefaultRootWindow");
    x11.string_to_keysym = (KeySym(*)(const char *))dlsym(x11.lib, "XStringToKeysym");
    x11.keysym_to_keycode = (KeyCode(*)(Display *, KeySym))dlsym(x11.lib, "XKeysymToKeycode");
    x11.grab_key = (int (*)(Display *, int, unsigned, Window, Bool, int, int))dlsym(x11.lib, "XGrabKey");
    x11.sync = (int (*)(Display *, Bool))dlsym(x11.lib, "XSync");
    x11.set_error_handler = (XErrorHandler(*)(XErrorHandler))dlsym(x11.lib, "XSetErrorHandler");
    x11.pending_events = (int (*)(Display *))dlsym(x11.lib, "XPending");
    x11.next_event = (int (*)(Display *, XEvent *))dlsym(x11.lib, "XNextEvent");
    if (!x11.open_display || !x11.next_event) return false;
    x11.display = x11.open_display(nullptr);
    return x11.display != nullptr;
}

void hotkeysStart(const std::vector<std::pair<std::string, std::string>> &binds, std::string *report) {
    if (getenv("WAYLAND_DISPLAY")) {
        *report = "wayland: bind keys in your compositor to earshot --send clip|replay|play";
        return;
    }
    if (!loadX11()) {
        *report = "no x11 display so global hotkeys are off (earshot --send still works)";
        return;
    }
    x11.set_error_handler(onXError);
    Window root = x11.default_root_window(x11.display);
    const unsigned locks[] = { 0, LockMask, Mod2Mask, LockMask | Mod2Mask };
    for (const auto &[command, spec] : binds) {
        if (spec.empty()) continue;
        unsigned mods = 0;
        std::string key;
        size_t start = 0;
        while (start <= spec.size()) {
            size_t plus = spec.find('+', start);
            std::string part = spec.substr(start, plus == std::string::npos ? std::string::npos : plus - start);
            if (part == "ctrl") mods |= ControlMask;
            else if (part == "alt") mods |= Mod1Mask;
            else if (part == "shift") mods |= ShiftMask;
            else if (part == "super") mods |= Mod4Mask;
            else key = part;
            if (plus == std::string::npos) break;
            start = plus + 1;
        }
        KeySym sym = x11.string_to_keysym(key.c_str());
        KeyCode code = sym ? x11.keysym_to_keycode(x11.display, sym) : 0;
        if (!code) {
            *report += "bad hotkey " + spec + "  ";
            continue;
        }
        grab_failed = false;
        for (unsigned lock : locks) x11.grab_key(x11.display, code, mods | lock, root, True, GrabModeAsync, GrabModeAsync);
        x11.sync(x11.display, False);
        if (grab_failed) *report += spec + " is taken by another app  ";
        else hotkeys.push_back({ command, code, mods });
    }
}

bool pollCommand(std::string *cmd) {
    while (x11.display && x11.pending_events(x11.display)) {
        XEvent ev;
        x11.next_event(x11.display, &ev);
        if (ev.type != KeyPress) continue;
        unsigned mods = ev.xkey.state & ~(LockMask | Mod2Mask);
        for (const Hotkey &h : hotkeys)
            if (h.code == ev.xkey.keycode && h.mods == mods) pending.push_back(h.command);
    }
    if (fifo_read >= 0) {
        char buf[256];
        ssize_t n;
        while ((n = read(fifo_read, buf, sizeof buf)) > 0) fifo_buf.append(buf, n);
        size_t nl;
        while ((nl = fifo_buf.find('\n')) != std::string::npos) {
            pending.push_back(fifo_buf.substr(0, nl));
            fifo_buf.erase(0, nl + 1);
        }
    }
    if (pending.empty()) return false;
    *cmd = pending.front();
    pending.erase(pending.begin());
    return true;
}
