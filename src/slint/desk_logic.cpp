// desk_logic.cpp - see desk_logic.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "desk_logic.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <thread>

#if !defined(_WIN32)
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace foxsdr::slintdesk {

namespace fs = std::filesystem;

namespace {

std::mutex g_mu;
Runner g_runner;
std::string g_path;
bool g_havePath = false;

#if defined(_WIN32)
constexpr char kSep = ';';
#else
constexpr char kSep = ':';
#endif

std::string envOr(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr ? std::string(v) : std::string();
}

std::string currentPath() {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_havePath) { return g_path; }
    }
    return envOr("PATH");
}

bool isExecutableFile(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec) || ec) { return false; }
#if defined(_WIN32)
    return true;
#else
    return ::access(p.c_str(), X_OK) == 0;
#endif
}

void say(std::string* why, const std::string& text) {
    if (why != nullptr) { *why = text; }
}

#if !defined(_WIN32)
// The real runner: posix_spawn (no shell), the new program in its own session, its standard output and error discarded unless captured.
bool realRun(const Command& c, std::string& out) {
    if (c.argv.empty()) { return false; }
    int inPipe[2] = {-1, -1};
    int outPipe[2] = {-1, -1};
    if (!c.input.empty() && ::pipe2(inPipe, O_CLOEXEC) != 0) { return false; }
    if (c.capture && ::pipe2(outPipe, O_CLOEXEC) != 0) {
        if (inPipe[0] >= 0) { ::close(inPipe[0]); ::close(inPipe[1]); }
        return false;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (inPipe[0] >= 0) { posix_spawn_file_actions_adddup2(&fa, inPipe[0], 0); } else { posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0); }
    if (outPipe[1] >= 0) { posix_spawn_file_actions_adddup2(&fa, outPipe[1], 1); } else { posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0); }
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSID);
    std::vector<char*> args;
    args.reserve(c.argv.size() + 1);
    for (const std::string& a : c.argv) { args.push_back(const_cast<char*>(a.c_str())); }
    args.push_back(nullptr);
    pid_t pid = 0;
    const int rc = ::posix_spawn(&pid, c.argv[0].c_str(), &fa, &at, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    if (inPipe[0] >= 0) { ::close(inPipe[0]); }
    if (outPipe[1] >= 0) { ::close(outPipe[1]); }
    if (rc != 0) {
        if (inPipe[1] >= 0) { ::close(inPipe[1]); }
        if (outPipe[0] >= 0) { ::close(outPipe[0]); }
        return false;
    }
    // A write to a program that has already gone raises SIGPIPE on THIS thread; it is blocked here and the pending one discarded, so the process is never ended by it.
    sigset_t pipeSet, old;
    sigemptyset(&pipeSet);
    sigaddset(&pipeSet, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipeSet, &old);
    if (inPipe[1] >= 0) {
        std::size_t done = 0;
        while (done < c.input.size()) {
            const ssize_t n = ::write(inPipe[1], c.input.data() + done, c.input.size() - done);
            if (n < 0) { if (errno == EINTR) { continue; } break; }
            done += static_cast<std::size_t>(n);
        }
        ::close(inPipe[1]);
    }
    if (outPipe[0] >= 0) {
        char buf[4096];
        for (;;) {
            const ssize_t n = ::read(outPipe[0], buf, sizeof(buf));
            if (n < 0 && errno == EINTR) { continue; }
            if (n <= 0) { break; }
            out.append(buf, static_cast<std::size_t>(n));
            if (out.size() > (1u << 20)) { break; }
        }
        ::close(outPipe[0]);
    }
    {
        struct timespec zero{0, 0};
        siginfo_t si;
        while (::sigtimedwait(&pipeSet, &si, &zero) > 0) {}
    }
    pthread_sigmask(SIG_SETMASK, &old, nullptr);
    if (!c.wait) {
        std::thread([pid] { int st = 0; while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {} }).detach();   // reaped when it ends; nothing waits for it
        return true;
    }
    // a clipboard tool exits (after handing the text to a background owner) in well under a second; a hung one is left to the reaper
    for (int i = 0; i < 300; ++i) {
        int st = 0;
        const pid_t r = ::waitpid(pid, &st, WNOHANG);
        if (r == pid) { return WIFEXITED(st) && WEXITSTATUS(st) == 0; }
        if (r < 0 && errno != EINTR) { return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::thread([pid] { int st = 0; while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {} }).detach();
    return false;
}
#else
bool realRun(const Command&, std::string&) { return false; }
#endif

bool run(const Command& c, std::string& out) {
    Runner r;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        r = g_runner;
    }
    return r ? r(c, out) : realRun(c, out);
}

bool openWith(const std::string& target, std::string* why) {
    const std::string opener = findOnPath("xdg-open", currentPath());
    if (opener.empty()) {
        say(why, "no xdg-open on this system (the xdg-utils package), so nothing was opened");
        return false;
    }
    Command c;
    c.argv = {opener, target};
    std::string out;
    if (!run(c, out)) {
        say(why, "xdg-open could not be started");
        return false;
    }
    return true;
}

}  // namespace

void setRunner(Runner r) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_runner = std::move(r);
}
void setPathOverride(const std::string& path) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_path = path;
    g_havePath = true;
}
void clearPathOverride() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_path.clear();
    g_havePath = false;
}

bool addressOk(const std::string& url) {
    static const std::string kScheme = "https://";
    if (url.compare(0, kScheme.size(), kScheme) != 0) { return false; }
    if (url.size() <= kScheme.size() || url.size() >= 512) { return false; }
    for (const char ch : url) {
        const unsigned char u = static_cast<unsigned char>(ch);
        if (u <= 0x20 || u == 0x7f || ch == '"' || ch == '\'' || ch == '<' || ch == '>' || ch == '\\' || ch == '`') { return false; }
    }
    return true;
}

std::string findOnPath(const std::string& name, const std::string& pathList) {
    if (name.empty() || name.find('/') != std::string::npos || name.find('\\') != std::string::npos) { return std::string(); }
    std::size_t at = 0;
    while (at <= pathList.size()) {
        std::size_t end = pathList.find(kSep, at);
        if (end == std::string::npos) { end = pathList.size(); }
        const std::string dir = pathList.substr(at, end - at);
        at = end + 1;
        if (dir.empty()) { continue; }   // an empty entry means "here" in a shell; never run from the working directory
        const fs::path p = fs::path(dir) / name;
        if (isExecutableFile(p)) { return p.string(); }
    }
    return std::string();
}

std::string absoluteForOpener(const std::string& path) {
    if (path.empty()) { return path; }
    std::error_code ec;
    const fs::path a = fs::absolute(fs::path(std::u8string(path.begin(), path.end())), ec);
    if (ec) { return path; }
    const std::u8string u = a.lexically_normal().u8string();
    return std::string(u.begin(), u.end());
}

ClipboardTool chooseClipboardTool(const std::string& waylandDisplay, const std::string& display, const std::string& pathList, std::string* fullPath) {
    const auto pick = [&](const char* name, ClipboardTool t) {
        const std::string p = findOnPath(name, pathList);
        if (p.empty()) { return ClipboardTool::None; }
        if (fullPath != nullptr) { *fullPath = p; }
        return t;
    };
    if (!waylandDisplay.empty()) {
        const ClipboardTool w = pick("wl-copy", ClipboardTool::WlClipboard);
        if (w != ClipboardTool::None) { return w; }
    }
    if (!display.empty()) {
        const ClipboardTool x = pick("xclip", ClipboardTool::Xclip);
        if (x != ClipboardTool::None) { return x; }
        const ClipboardTool s = pick("xsel", ClipboardTool::Xsel);
        if (s != ClipboardTool::None) { return s; }
    }
    return ClipboardTool::None;
}

Command copyCommand(ClipboardTool tool, const std::string& fullPath, const std::string& text) {
    Command c;
    c.input = text;
    c.wait = true;
    switch (tool) {
        case ClipboardTool::WlClipboard: c.argv = {fullPath, "--type", "text/plain;charset=utf-8"}; break;
        case ClipboardTool::Xclip: c.argv = {fullPath, "-selection", "clipboard", "-in"}; break;
        case ClipboardTool::Xsel: c.argv = {fullPath, "--clipboard", "--input"}; break;
        case ClipboardTool::None: c.argv.clear(); break;
    }
    return c;
}

Command pasteCommand(ClipboardTool tool, const std::string& fullPath) {
    Command c;
    c.capture = true;
    c.wait = true;
    // the paste program sits beside the copy program: wl-copy -> wl-paste
    switch (tool) {
        case ClipboardTool::WlClipboard: {
            const fs::path p(fullPath);
            c.argv = {(p.parent_path() / "wl-paste").string(), "--no-newline"};
            break;
        }
        case ClipboardTool::Xclip: c.argv = {fullPath, "-selection", "clipboard", "-out"}; break;
        case ClipboardTool::Xsel: c.argv = {fullPath, "--clipboard", "--output"}; break;
        case ClipboardTool::None: c.argv.clear(); break;
    }
    return c;
}

bool openAddress(const std::string& url, std::string* why) {
    if (!addressOk(url)) {
        say(why, "that address is not an https address; not opened");
        return false;
    }
    return openWith(url, why);
}

bool openFolder(const std::string& dir, std::string* why) {
    if (dir.empty()) {
        say(why, "no folder to open");
        return false;
    }
    return openWith(absoluteForOpener(dir), why);
}

bool openFile(const std::string& path, std::string* why) {
    if (path.empty()) {
        say(why, "no file to open");
        return false;
    }
    return openWith(absoluteForOpener(path), why);
}

bool copyText(const std::string& utf8, std::string* why) {
    std::string tool;
    const ClipboardTool t = chooseClipboardTool(envOr("WAYLAND_DISPLAY"), envOr("DISPLAY"), currentPath(), &tool);
    if (t == ClipboardTool::None) {
        say(why, "no clipboard program found (wl-clipboard, xclip or xsel); nothing copied");
        return false;
    }
    std::string out;
    if (!run(copyCommand(t, tool, utf8), out)) {
        say(why, "the clipboard program did not take the text; nothing copied");
        return false;
    }
    return true;
}

std::string pasteText() {
    std::string tool;
    const ClipboardTool t = chooseClipboardTool(envOr("WAYLAND_DISPLAY"), envOr("DISPLAY"), currentPath(), &tool);
    if (t == ClipboardTool::None) { return std::string(); }
    const Command c = pasteCommand(t, tool);
    if (c.argv.empty()) { return std::string(); }
    std::string out;
    if (!run(c, out)) { return std::string(); }
    return out;
}

}  // namespace foxsdr::slintdesk
