// file_pick.cpp - see file_pick.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/file_pick.hpp"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <system_error>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <commdlg.h>  // GetOpenFileNameW; comdlg32 is linked by CMakeLists.txt
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace cascade::core {

namespace fs = std::filesystem;

bool placeholderNamesProgram(const std::string& placeholder) {
    std::size_t end = placeholder.size();
    while (end > 0 && std::isspace(static_cast<unsigned char>(placeholder[end - 1]))) { --end; }
    static constexpr char kExe[] = ".exe";
    if (end < 4) { return false; }
    for (std::size_t i = 0; i < 4; ++i) {
        const unsigned char c = static_cast<unsigned char>(placeholder[end - 4 + i]);
        if (std::tolower(c) != static_cast<unsigned char>(kExe[i])) { return false; }
    }
    return true;
}

std::string pickStartFolder(const std::string& value) {
    if (value.empty()) { return std::string(); }
    try {
        const fs::path dir = fs::path(std::u8string(value.begin(), value.end())).parent_path();
        if (dir.empty()) { return std::string(); }
        std::error_code ec;
        if (!fs::is_directory(dir, ec) || ec) { return std::string(); }
        const auto s = dir.u8string();
        return std::string(s.begin(), s.end());
    } catch (...) {
        // u8string() throws for a name Windows allows and UTF-8 cannot spell
        // (an unpaired surrogate). Not a reason to lose the dialog.
        return std::string();
    }
}

std::string pickStartPath(const std::string& typed, const std::string& folder,
                          const std::string& fallbackName) {
    std::string t = typed;
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"') { t = t.substr(1, t.size() - 2); }
    bool hasFolderPart = false;
    if (!t.empty()) {
        try {
            hasFolderPart = fs::path(std::u8string(t.begin(), t.end())).has_parent_path();
        } catch (...) {
            hasFolderPart = true;  // a name this cannot spell: leave it to the dialog as it stands
        }
    }
    if (hasFolderPart) { return t; }
    if (folder.empty()) { return t; }
    const char last = folder.back();
    return folder + ((last == '/' || last == '\\') ? "" : "/") + (t.empty() ? fallbackName : t);
}

std::vector<std::pair<std::string, std::string>> openFileFilters(const FilePickRequest& r) {
    std::vector<std::pair<std::string, std::string>> out;
    const auto named = [](const std::string& name, const char* pattern) {
        return std::make_pair(name.empty() ? std::string(pattern) : name, std::string(pattern));
    };
    if (r.programs) { out.push_back(named(r.programsName, "*.exe")); }
    if (r.lists) { out.push_back(named(r.listsName, "*.xml;*.csv")); }
    out.push_back(named(r.allFilesName, "*.*"));
    return out;
}

std::vector<std::string> pickerCommand(const std::string& tool, const std::string& title,
                                       const std::string& folder) {
    if (tool == "zenity") {
        std::vector<std::string> a = {"zenity", "--file-selection", "--title=" + title};
        if (!folder.empty()) {
            // A trailing slash is how zenity is told "this is a folder to open in".
            a.push_back("--filename=" + folder + (folder.back() == '/' ? "" : "/"));
        }
        return a;
    }
    if (tool == "kdialog") {
        std::vector<std::string> a = {"kdialog", "--title", title, "--getopenfilename"};
        // Only an absolute folder: a relative one starting with '-' would be read as an option.
        if (!folder.empty() && folder.front() == '/') { a.push_back(folder); }
        return a;
    }
    return {};
}

#if defined(_WIN32)

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) { return std::wstring(); }
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) { return std::wstring(); }
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const wchar_t* w) {
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) { return std::string(); }
    std::string out(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    out.resize(static_cast<std::size_t>(n) - 1u);  // the terminating NUL is not text
    return out;
}

// "name\0pattern\0name\0pattern\0\0": the filter string GetOpenFileName wants.
std::vector<wchar_t> filterBuffer(const FilePickRequest& r) {
    std::vector<wchar_t> buf;
    for (const auto& [name, pattern] : openFileFilters(r)) {
        for (const std::string* part : {&name, &pattern}) {
            const std::wstring w = widen(*part);
            buf.insert(buf.end(), w.begin(), w.end());
            buf.push_back(L'\0');
        }
    }
    buf.push_back(L'\0');
    return buf;
}

}  // namespace

bool filePickAvailable() { return true; }

bool pickFile(const FilePickRequest& request, std::string& path) {
    path.clear();
    const std::wstring title = widen(request.title);
    // The recordings folder combines USERPROFILE with forward-slash suffixes.
    // GetOpenFileName ignored that mixed-slash initial directory (0.99.80,
    // reproduced by the native import-start test). Give it native separators.
    const std::wstring folder = fs::path(widen(pickStartFolder(request.startFrom))).make_preferred().native();
    std::vector<wchar_t> filter = filterBuffer(request);
    // A value is at most CASCADE_SETTING_VALUE_BYTES (4096) bytes, so 32 K wide
    // characters hold anything the dialog can hand back; MAX_PATH would cut a
    // long path short and fail the whole call.
    std::vector<wchar_t> file(32768, L'\0');

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = static_cast<HWND>(request.owner);
    ofn.lpstrFilter = filter.data();
    ofn.nFilterIndex = 1;  // the first pair: Programs for a ".exe" placeholder, else All files
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = static_cast<DWORD>(file.size());
    ofn.lpstrInitialDir = folder.empty() ? nullptr : folder.c_str();
    ofn.lpstrTitle = title.empty() ? nullptr : title.c_str();
    // NOCHANGEDIR: the dialog must not move the process's working directory to
    // wherever the person browsed - the application resolves relative paths
    // against it. HIDEREADONLY: "Open as read-only" means nothing to a path.
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    if (!::GetOpenFileNameW(&ofn)) { return false; }  // cancelled, or the dialog failed
    path = narrow(file.data());
    return !path.empty();
}

#else  // Linux: a helper program, run without a shell

namespace {

// The full path of an executable file named `tool` on PATH, or "".
std::string findOnPath(const char* tool) {
    const char* env = std::getenv("PATH");
    if (env == nullptr) { return std::string(); }
    const std::string path = env;
    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t end = path.find(':', start);
        if (end == std::string::npos) { end = path.size(); }
        if (end > start) {
            const std::string full = path.substr(start, end - start) + "/" + tool;
            struct stat st {};
            if (::stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
                ::access(full.c_str(), X_OK) == 0) {
                return full;
            }
        }
        start = end + 1;
    }
    return std::string();
}

// "zenity", "kdialog", or "" when this machine has neither.
std::string pickerTool() {
    for (const char* tool : {"zenity", "kdialog"}) {
        if (!findOnPath(tool).empty()) { return tool; }
    }
    return std::string();
}

// Runs `args` (argv[0] looked up on PATH) with no shell, its standard input and
// error on /dev/null, and collects what it writes to standard output. True when
// it ran and exited 0. Blocks until the helper exits - which is when the person
// closes the dialog.
bool runCapture(const std::vector<std::string>& args, std::string& out) {
    // THE ARGV IS BUILT BEFORE THE FORK: this is a multithreaded process, and
    // after fork() only the forking thread exists in the child - a heap
    // allocation there can wait for ever on a lock another thread held. The
    // child makes only async-signal-safe calls (the same rule as
    // gui/shell_open.hpp's posixShellOpen).
    std::vector<std::vector<char>> storage;
    std::vector<char*> argv;
    storage.reserve(args.size());
    for (const std::string& a : args) {
        storage.emplace_back(a.begin(), a.end());
        storage.back().push_back('\0');
    }
    for (std::vector<char>& s : storage) { argv.push_back(s.data()); }
    argv.push_back(nullptr);

    int fds[2] = {-1, -1};
    if (::pipe2(fds, O_CLOEXEC) != 0) { return false; }
    const pid_t child = ::fork();
    if (child < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        return false;
    }
    if (child == 0) {
        const int devNull = ::open("/dev/null", O_RDWR);
        if (devNull >= 0) {
            ::dup2(devNull, STDIN_FILENO);
            ::dup2(devNull, STDERR_FILENO);
            if (devNull > STDERR_FILENO) { ::close(devNull); }
        }
        ::dup2(fds[1], STDOUT_FILENO);  // the copy on fd 1 is not close-on-exec
        ::execvp(argv[0], argv.data());
        _exit(127);
    }
    ::close(fds[1]);
    char buf[512];
    for (;;) {
        const ssize_t n = ::read(fds[0], buf, sizeof buf);
        if (n > 0) {
            if (out.size() < 8192u) { out.append(buf, static_cast<std::size_t>(n)); }
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            break;  // EOF: the helper closed its output (or exited)
        }
    }
    ::close(fds[0]);
    int status = 0;
    pid_t r;
    do {
        r = ::waitpid(child, &status, 0);
    } while (r < 0 && errno == EINTR);
    if (r < 0) { return !out.empty(); }  // reaped elsewhere: the output is all there is to go on
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

}  // namespace

bool filePickAvailable() { return !pickerTool().empty(); }

bool pickFile(const FilePickRequest& request, std::string& path) {
    path.clear();
    const std::string tool = pickerTool();
    if (tool.empty()) { return false; }
    const std::vector<std::string> argv =
        pickerCommand(tool, request.title, pickStartFolder(request.startFrom));
    std::string out;
    if (argv.empty() || !runCapture(argv, out)) { return false; }
    // The helpers print the path and a newline; a path may hold anything else.
    const std::size_t eol = out.find('\n');
    path = out.substr(0, eol);
    while (!path.empty() && path.back() == '\r') { path.pop_back(); }
    return !path.empty();
}

#endif

}  // namespace cascade::core
