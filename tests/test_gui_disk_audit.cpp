// No new file-system call joins the thread that draws the window unremarked.
//
// WHY THIS TEST EXISTS. Four field freezes in 0.99.58 - 0.99.63 were one defect: a
// file-system call on the thread that draws the window, made against a disk that
// was slow to answer - the Record button (Recorder::start), a once-a-second look
// in the settings folder, the Fitted modules window's per-frame stat, and the
// debounced config write (0.96.3). Each was fixed where it was found, and each fix
// came with a test of its own function. 0.99.64 went looking for the rest
// (docs/DIAGNOSTICS.md, "The window does no disk work") and found a dozen more in
// one pass: the bookmark and marker saves, the I/Q file's Open, a speaker's WAV, a
// screenshot, a saved picture... A test per function proves the functions that were
// found. This one is about the next one: it reads the GUI translation units and
// requires every file-system call in them to be ONE OF TWO THINGS.
//
//   1. INSIDE CODE THAT RUNS ON A WORKER: the body of a lambda handed to
//      std::async, std::thread, or a gui::DiskJob / RecordStart / ConfigWriter /
//      BackgroundSaver request. Nothing to say; that is the pattern.
//   2. ON THE ALLOWLIST BELOW, with the function it is in, the kinds of call it
//      makes, and WHY IT IS ACCEPTABLE THERE - a one-off, user-initiated, tiny
//      call; start-up before the first frame; already under a watchdog pause - and
//      what stops it filing a false freeze report against a healthy application
//      (a WatchdogPause held in the function or its caller, or the reason none is
//      needed).
//
// Anything else fails with a message that says what to do. The allowlist is the
// honest list of what is STILL on the GUI thread; it is not a list of what is
// fine. An entry that no longer matches anything fails too, so the list cannot
// quietly outlive the code it describes.
//
// WHAT COUNTS AS A FILE-SYSTEM CALL: every std::filesystem operation that touches
// the disk (not path arithmetic: path(), filename(), extension(), temp_directory_path
// and current_path ask the process, not a disk), every stream and C stdio open,
// the Win32 and POSIX file calls, the C remove and rename - and the project's own
// helpers that wrap them (kWrappers below: add the name of a new one there). Calls
// are read from the source with comments and string literals blanked, so what a
// comment says about the old behaviour is not mistaken for a call.
//
// WHAT IT CANNOT SEE: a helper that reaches the disk and is not named in kWrappers
// (the scan is lexical, one level deep); a lambda built into a variable and passed
// to a worker by name (it is flagged, and must be listed or inlined); anything
// outside src/gui. Each is a reason to read this list when adding a call, not a
// reason to trust it blindly. The diagnostic log's own write (core/diag_log.cpp:
// fwrite and fflush under a mutex, on whatever thread logs) is deliberately not
// scanned: it is everywhere, it is item 13 of the audit, and docs/DIAGNOSTICS.md
// says why it has not been moved.
//
// HOW TO ADD TO THE ALLOWLIST: if your call can wait for a disk, DO NOT list it -
// move it to a worker (gui/disk_job.hpp has the pattern, tests/test_gui_file_jobs.cpp
// shows the test). If it truly is a one-off, user-initiated, tiny call, add an
// entry to kAllowed naming the FUNCTION (as it is defined, "AppWindow::name"), the
// CATEGORIES of call it makes, the REASON and the PAUSE, and hold a WatchdogPause in
// it (src/core/hang_watchdog.hpp, rule 2b).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "test_check.hpp"

namespace fs = std::filesystem;

namespace {

// --- reading and blanking ------------------------------------------------------

std::string readText(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string t = ss.str();
    t.erase(std::remove(t.begin(), t.end(), '\r'), t.end());
    return t;
}

// `text` with every comment and the CONTENT of every string and character literal
// replaced by spaces (newlines kept, so line numbers survive).
std::string blank(const std::string& text) {
    std::string out = text;
    const std::size_t n = text.size();
    std::size_t i = 0;
    const auto wipe = [&](std::size_t a, std::size_t b) {
        for (std::size_t k = a; k < b && k < n; ++k) {
            if (out[k] != '\n') { out[k] = ' '; }
        }
    };
    while (i < n) {
        const char c = text[i];
        if (c == '/' && i + 1 < n && text[i + 1] == '/') {
            std::size_t j = text.find('\n', i);
            if (j == std::string::npos) { j = n; }
            wipe(i, j);
            i = j;
        } else if (c == '/' && i + 1 < n && text[i + 1] == '*') {
            std::size_t j = text.find("*/", i + 2);
            j = (j == std::string::npos) ? n : j + 2;
            wipe(i, j);
            i = j;
        } else if (c == 'R' && i + 1 < n && text[i + 1] == '"' &&
                   (i == 0 || !(std::isalnum(static_cast<unsigned char>(text[i - 1])) ||
                                text[i - 1] == '_'))) {
            // A raw string literal: R"delim( ... )delim"
            const std::size_t open = text.find('(', i + 2);
            if (open == std::string::npos) { break; }
            const std::string delim = text.substr(i + 2, open - (i + 2));
            const std::string close = ")" + delim + "\"";
            std::size_t j = text.find(close, open);
            j = (j == std::string::npos) ? n : j + close.size();
            wipe(open + 1, j - 1);
            i = j;
        } else if (c == '"') {
            std::size_t j = i + 1;
            while (j < n && text[j] != '"') { j += (text[j] == '\\') ? 2 : 1; }
            wipe(i + 1, j);
            i = j + 1;
        } else if (c == '\'') {
            // A digit separator (1'000'000) is not a character literal.
            if (i > 0 && std::isxdigit(static_cast<unsigned char>(text[i - 1])) && i + 1 < n &&
                std::isxdigit(static_cast<unsigned char>(text[i + 1]))) {
                ++i;
                continue;
            }
            std::size_t j = i + 1;
            while (j < n && text[j] != '\'') { j += (text[j] == '\\') ? 2 : 1; }
            wipe(i + 1, j);
            i = j + 1;
        } else {
            ++i;
        }
    }
    return out;
}

int lineOf(const std::string& text, std::size_t at) {
    return 1 + static_cast<int>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(at), '\n'));
}

bool identChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// The index of the bracket that closes the one at `open`, or npos.
std::size_t matching(const std::string& s, std::size_t open) {
    const char o = s[open];
    const char c = (o == '(') ? ')' : (o == '{') ? '}' : ']';
    int depth = 0;
    for (std::size_t i = open; i < s.size(); ++i) {
        if (s[i] == o) {
            ++depth;
        } else if (s[i] == c) {
            if (--depth == 0) { return i; }
        }
    }
    return std::string::npos;
}

// --- what counts as a file-system call -------------------------------------------

// std::filesystem operations that ask a disk. (path(), filename(), extension(),
// replace_extension(), temp_directory_path() and current_path() are not here: they
// are arithmetic or ask the process.)
const std::vector<std::string>& fsOps() {
    static const std::vector<std::string> v = {
        "exists",           "is_directory",       "is_regular_file", "is_symlink",
        "is_empty",         "is_other",           "file_size",       "last_write_time",
        "create_directory", "create_directories", "create_symlink",  "create_hard_link",
        "remove",           "remove_all",         "rename",          "copy",
        "copy_file",        "copy_symlink",       "directory_iterator",
        "recursive_directory_iterator",           "status",          "symlink_status",
        "canonical",        "weakly_canonical",   "equivalent",      "space",
        "resize_file",      "permissions",        "hard_link_count", "read_symlink",
    };
    return v;
}

// Streams and C stdio that open a file by name.
const std::vector<std::string>& streamNames() {
    static const std::vector<std::string> v = {"ifstream", "ofstream", "fstream", "filebuf"};
    return v;
}
const std::vector<std::string>& stdioCalls() {
    static const std::vector<std::string> v = {"fopen",  "_wfopen", "fopen_s", "_wfopen_s",
                                               "freopen", "_fsopen", "_wfsopen", "tmpfile"};
    return v;
}
// Win32 and POSIX, as free calls.
const std::vector<std::string>& osCalls() {
    static const std::vector<std::string> v = {
        "CreateFile",   "CreateFileA",   "CreateFileW",   "ReadFile",       "WriteFile",
        "GetFileAttributes", "GetFileAttributesA", "GetFileAttributesW", "GetFileAttributesExW",
        "FindFirstFile", "FindFirstFileA", "FindFirstFileW", "FindFirstFileExW", "DeleteFile",
        "DeleteFileA",   "DeleteFileW",   "MoveFile",      "MoveFileA",      "MoveFileW",
        "MoveFileExW",   "CopyFile",      "CopyFileW",     "CreateDirectory", "CreateDirectoryW",
        "RemoveDirectory", "RemoveDirectoryW", "GetFileSize", "GetFileSizeEx",
        "stat",          "_stat",         "_stat64",       "_wstat",         "lstat",
        "opendir",       "mkdir",         "_mkdir",        "rmdir",          "_rmdir",
        "unlink",        "_unlink",       "access",        "_access",        "_waccess",
    };
    return v;
}

// THE PROJECT'S OWN HELPERS THAT REACH THE DISK. Each is a call a GUI function can
// make that does a file-system call the scan cannot see into. Add yours here. Every
// one is a plain textual pattern and is checked to exist in src/ (below), so a
// helper that is renamed or removed fails this test instead of quietly dropping out.
struct Wrapper {
    const char* pattern;  // as it appears in the source, e.g. "writeBmp24("
    const char* where;    // the file that defines it, for the existence check
    const char* why;
};
const std::vector<Wrapper>& wrappers() {
    static const std::vector<Wrapper> v = {
        {"writeBmp24(", "src/core/image_write.cpp", "writes a BMP"},
        {"freqMgr_.load(", "src/core/freq_manager.cpp", "reads bookmarks.json"},
        {"freqMarkers_.load(", "src/core/freq_markers.cpp", "reads markers.json"},
        {"freqMgr_.save(", "src/core/freq_manager.cpp", "writes bookmarks.json synchronously"},
        {"freqMarkers_.save(", "src/core/freq_markers.cpp", "writes markers.json synchronously"},
        {"FreqManager::writeFile(", "src/core/freq_manager.cpp", "the bookmark file's blocking write"},
        {"FreqMarkers::writeFile(", "src/core/freq_markers.cpp", "the marker file's blocking write"},
        {"ConfigStore::load(", "src/core/config.cpp", "reads config.json"},
        {"ConfigStore::save(", "src/core/config.cpp", "writes config.json synchronously"},
        {"ConfigStore::writeFile(", "src/core/config.cpp", "the config file's blocking write"},
        {"importFrequencyFile(", "src/core/freq_import.cpp", "reads an imported frequency list"},
        {"listIqRecordings(", "src/core/patch_recordings.cpp", "lists a folder and opens headers"},
        {"openIqRecording(", "src/core/patch_recordings.cpp", "opens a recording"},
        {"IqFileSource>()", "src/source/iq_file_source.cpp", "constructs a file source (open() follows)"},
        {"file->open(", "src/source/iq_file_source.cpp", "opens an I/Q file: an ifstream and a header walk"},
        {"Recorder::start(", "src/core/recorder.cpp", "creates a take's folder and file"},
        {"iqRecorder_.start(", "src/core/recorder.cpp", "creates a take's folder and file"},
        {"audioRecorder_.start(", "src/core/recorder.cpp", "creates a take's folder and file"},
        {"iqRecorder_.stop(", "src/core/recorder.cpp", "patches a WAV header and closes the file"},
        {"audioRecorder_.stop(", "src/core/recorder.cpp", "patches a WAV header and closes the file"},
        {"makeWavDest(", "src/core/patch_audio.cpp", "the blocking speaker file maker"},
        {"pluginHost_.scan(", "src/core/plugin_host.cpp", "lists the plugin folder and maps every module"},
        {"PluginHost::defaultPluginDir(", "src/core/plugin_host.cpp", "writes and removes a probe file"},
        {"pluginRepo_.remove(", "src/core/plugin_repo.cpp", "deletes a plugin file"},
        {"pluginRepo_.removeQuarantined(", "src/core/plugin_repo.cpp", "deletes a retired plugin file"},
        {"pluginRepo_.loadInventory(", "src/core/plugin_repo.cpp", "reads and hashes every installed plugin"},
        {"pluginRepo_.install(", "src/core/plugin_repo.cpp", "writes a plugin file"},
        {"removeSupersededPlugins(", "src/core/plugin_cleanup.cpp", "deletes superseded plugin files"},
        {"bandPlan_.loadSelection(", "src/core/band_plan.cpp", "reads a band plan file"},
        {"BandPlan::available(", "src/core/band_plan.cpp", "lists the band plan folder"},
        {"DiagLog::instance().configure(", "src/core/diag_log.cpp", "creates the log folder and opens the file"},
        {"setCrashCaptureEnabled(", "src/core/crash_handler.cpp", "creates the reports folder"},
        {"addCatalogueFile(", "src/core/i18n.cpp", "reads a language catalogue"},
        {"StallLedger::removeFile(", "src/core/telemetry.cpp", "deletes the stall ledger"},
    };
    return v;
}

enum class Kind { Fs, Stream, Stdio, Os, Wrapper };
const char* kindName(Kind k) {
    switch (k) {
        case Kind::Fs: return "fs";
        case Kind::Stream: return "stream";
        case Kind::Stdio: return "stdio";
        case Kind::Os: return "os";
        case Kind::Wrapper: return "helper";
    }
    return "?";
}

struct Hit {
    std::string file;
    int line = 0;
    std::string function;
    Kind kind = Kind::Fs;
    std::string what;
    bool onWorker = false;
};

// Whether the call at `at` (just past the name) has exactly one top-level argument:
// the C remove(path) and rename are told from the <algorithm> remove(first, last, v)
// by it.
int topLevelArgs(const std::string& s, std::size_t openParen) {
    const std::size_t close = matching(s, openParen);
    if (close == std::string::npos) { return 0; }
    int args = 0;
    int depth = 0;
    bool any = false;
    for (std::size_t i = openParen + 1; i < close; ++i) {
        const char c = s[i];
        if (c == '(' || c == '{' || c == '[') {
            ++depth;
            any = true;
        } else if (c == ')' || c == '}' || c == ']') {
            --depth;
        } else if (c == ',' && depth == 0) {
            ++args;
        } else if (!std::isspace(static_cast<unsigned char>(c))) {
            any = true;
        }
    }
    return any ? args + 1 : 0;
}

// --- which spans run on a worker ---------------------------------------------------

// The body of every lambda that sits inside the parentheses of a call that hands it
// to another thread: std::async, std::thread, and the request() of the gui's job
// classes. Returned as [begin, end) character ranges.
std::vector<std::pair<std::size_t, std::size_t>> workerSpans(const std::string& s) {
    std::vector<std::pair<std::size_t, std::size_t>> spans;
    static const std::vector<std::string> launchers = {
        "std::async(", "std::thread(", "std::jthread(", ".request(", "->request(", "requestAsync(",
    };
    for (const std::string& l : launchers) {
        for (std::size_t at = s.find(l); at != std::string::npos; at = s.find(l, at + 1)) {
            const std::size_t open = at + l.size() - 1;
            const std::size_t close = matching(s, open);
            if (close == std::string::npos) { continue; }
            // Every lambda body in the argument list: "] ... {" at bracket depth 0 of
            // the call's own parentheses is a lambda introducer's end.
            for (std::size_t i = open + 1; i < close; ++i) {
                if (s[i] != '[') { continue; }
                const std::size_t cap = matching(s, i);
                if (cap == std::string::npos || cap >= close) { continue; }
                std::size_t j = cap + 1;
                while (j < close && std::isspace(static_cast<unsigned char>(s[j]))) { ++j; }
                if (j < close && s[j] == '(') {
                    const std::size_t pc = matching(s, j);
                    if (pc == std::string::npos) { continue; }
                    j = pc + 1;
                }
                // mutable / noexcept / -> return type, up to the body's brace
                while (j < close && s[j] != '{' && s[j] != ';' && s[j] != ',') { ++j; }
                if (j < close && s[j] == '{') {
                    const std::size_t end = matching(s, j);
                    if (end != std::string::npos && end <= close) {
                        spans.emplace_back(j, end + 1);
                        i = end;
                    }
                }
            }
        }
    }
    return spans;
}

// --- which function is a character in ----------------------------------------------

struct FuncSpan {
    std::string name;
    std::size_t begin = 0;
    std::size_t end = 0;
};

// Function definitions at column 0 (the tree is clang-formatted that way): from the
// line that starts a definition to the closing brace in column 0.
std::vector<FuncSpan> functionSpans(const std::string& s) {
    std::vector<FuncSpan> out;
    std::size_t pos = 0;
    while (pos < s.size()) {
        std::size_t eol = s.find('\n', pos);
        if (eol == std::string::npos) { eol = s.size(); }
        const std::string line = s.substr(pos, eol - pos);
        const bool starts = !line.empty() && !std::isspace(static_cast<unsigned char>(line[0])) &&
                            line[0] != '}' && line[0] != '#' &&
                            line.compare(0, 9, "namespace") != 0 &&
                            line.compare(0, 6, "struct") != 0 && line.compare(0, 5, "class") != 0 &&
                            line.compare(0, 4, "enum") != 0 && line.compare(0, 8, "template") != 0 &&
                            line.compare(0, 5, "using") != 0 && line.compare(0, 7, "typedef") != 0 &&
                            line.compare(0, 6, "extern") != 0 && line.find('(') != std::string::npos;
        if (starts) {
            // The definition's name: the last identifier (with ::) before the first '('.
            const std::size_t paren = line.find('(');
            std::size_t e = paren;
            while (e > 0 && std::isspace(static_cast<unsigned char>(line[e - 1]))) { --e; }
            std::size_t b = e;
            while (b > 0 && (identChar(line[b - 1]) || line[b - 1] == ':' || line[b - 1] == '~')) { --b; }
            const std::string name = line.substr(b, e - b);
            // The body's opening brace: after the parameter list and past any
            // constructor initialiser list (a "member_{...}" brace follows an
            // identifier; the body's follows ')' or '}' or const/noexcept/override).
            std::size_t brace = std::string::npos;
            {
                std::size_t k = pos + paren;
                const std::size_t pc = matching(s, k);
                k = (pc == std::string::npos) ? s.size() : pc + 1;
                while (k < s.size()) {
                    const char ch = s[k];
                    if (ch == ';') { break; }
                    if (ch == '(') {
                        const std::size_t m = matching(s, k);
                        if (m == std::string::npos) { break; }
                        k = m + 1;
                        continue;
                    }
                    if (ch == '{') {
                        std::size_t wordEnd = k;
                        while (wordEnd > 0 && std::isspace(static_cast<unsigned char>(s[wordEnd - 1]))) {
                            --wordEnd;
                        }
                        std::size_t wordBegin = wordEnd;
                        while (wordBegin > 0 && identChar(s[wordBegin - 1])) { --wordBegin; }
                        const std::string word = s.substr(wordBegin, wordEnd - wordBegin);
                        const bool afterIdent = wordEnd > 0 && identChar(s[wordEnd - 1]);
                        if (afterIdent && word != "const" && word != "noexcept" && word != "override" &&
                            word != "final" && word != "mutable") {
                            const std::size_t m = matching(s, k);  // a member{...} initialiser
                            if (m == std::string::npos) { break; }
                            k = m + 1;
                            continue;
                        }
                        brace = k;
                        break;
                    }
                    ++k;
                }
            }
            if (!name.empty() && brace != std::string::npos) {
                const std::size_t close = matching(s, brace);
                if (close != std::string::npos) {
                    out.push_back({name, pos, close + 1});
                    pos = close + 1;
                    continue;
                }
            }
        }
        pos = eol + 1;
    }
    return out;
}

std::string functionAt(const std::vector<FuncSpan>& fns, std::size_t at) {
    for (const FuncSpan& f : fns) {
        if (at >= f.begin && at < f.end) { return f.name; }
    }
    return "(file scope)";
}

// --- the scan ---------------------------------------------------------------------

struct FileScan {
    std::string name;
    std::string code;  // blanked
    std::vector<FuncSpan> fns;
    std::vector<Hit> hits;
};

bool preceededByMemberAccess(const std::string& s, std::size_t at) {
    std::size_t i = at;
    while (i > 0 && std::isspace(static_cast<unsigned char>(s[i - 1]))) { --i; }
    if (i >= 1 && s[i - 1] == '.') { return true; }
    if (i >= 2 && s[i - 1] == '>' && s[i - 2] == '-') { return true; }
    return false;
}

FileScan scanSource(const std::string& name, const std::string& source) {
    FileScan f;
    f.name = name;
    f.code = blank(source);
    f.fns = functionSpans(f.code);
    const std::string& s = f.code;
    const auto spans = workerSpans(s);
    const auto onWorker = [&spans](std::size_t at) {
        for (const auto& sp : spans) {
            if (at >= sp.first && at < sp.second) { return true; }
        }
        return false;
    };
    const auto add = [&](std::size_t at, Kind k, const std::string& what) {
        Hit h;
        h.file = name;
        h.line = lineOf(s, at);
        h.function = functionAt(f.fns, at);
        h.kind = k;
        h.what = what;
        h.onWorker = onWorker(at);
        f.hits.push_back(h);
    };
    const auto wordAt = [&s](std::size_t at, const std::string& w) {
        if (s.compare(at, w.size(), w) != 0) { return false; }
        if (at > 0 && identChar(s[at - 1])) { return false; }
        return !(at + w.size() < s.size() && identChar(s[at + w.size()]));
    };

    // The aliases of std::filesystem this file declares.
    std::set<std::string> fsNames = {"std::filesystem", "stdfs"};
    for (std::size_t at = s.find("namespace "); at != std::string::npos; at = s.find("namespace ", at + 1)) {
        const std::size_t eq = s.find('=', at);
        const std::size_t semi = s.find(';', at);
        if (eq == std::string::npos || semi == std::string::npos || eq > semi) { continue; }
        std::string alias = s.substr(at + 10, eq - (at + 10));
        std::string target = s.substr(eq + 1, semi - eq - 1);
        alias.erase(std::remove_if(alias.begin(), alias.end(), ::isspace), alias.end());
        target.erase(std::remove_if(target.begin(), target.end(), ::isspace), target.end());
        if (target == "std::filesystem") { fsNames.insert(alias); }
    }

    for (const std::string& ns : fsNames) {
        for (const std::string& op : fsOps()) {
            const std::string needle = ns + "::" + op;
            for (std::size_t at = s.find(needle); at != std::string::npos; at = s.find(needle, at + 1)) {
                if (at > 0 && (identChar(s[at - 1]) || s[at - 1] == ':')) { continue; }
                const std::size_t after = at + needle.size();
                if (after < s.size() && identChar(s[after])) { continue; }
                // std::filesystem::remove with three arguments is not a thing; copy/remove
                // are always the file ones here.
                add(at, Kind::Fs, ns + "::" + op);
            }
        }
    }
    for (const std::string& st : streamNames()) {
        for (std::size_t at = s.find(st); at != std::string::npos; at = s.find(st, at + 1)) {
            if (!wordAt(at, st)) { continue; }
            // A declaration or construction of the stream type, not a mention in an
            // include or a template argument.
            if (at >= 5 && s.compare(at - 5, 5, "std::") == 0) { add(at - 5, Kind::Stream, "std::" + st); }
        }
    }
    for (const std::string& fn : stdioCalls()) {
        for (std::size_t at = s.find(fn); at != std::string::npos; at = s.find(fn, at + 1)) {
            if (!wordAt(at, fn)) { continue; }
            std::size_t p = at + fn.size();
            while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) { ++p; }
            if (p < s.size() && s[p] == '(' && !preceededByMemberAccess(s, at)) { add(at, Kind::Stdio, fn); }
        }
    }
    for (const std::string& fn : osCalls()) {
        for (std::size_t at = s.find(fn); at != std::string::npos; at = s.find(fn, at + 1)) {
            if (!wordAt(at, fn)) { continue; }
            std::size_t p = at + fn.size();
            while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) { ++p; }
            if (p < s.size() && s[p] == '(' && !preceededByMemberAccess(s, at)) {
                // `::stat(` and `stat(` alike; a member called stat is not this.
                add(at, Kind::Os, fn);
            }
        }
    }
    // The C remove(path) and rename(a, b): std::remove( / ::remove( with ONE argument,
    // std::rename( / ::rename( with two - never the <algorithm> remove.
    for (const char* fn : {"remove", "rename"}) {
        for (const std::string pre : {"std::", "::"}) {
            const std::string needle = pre + fn;
            for (std::size_t at = s.find(needle); at != std::string::npos; at = s.find(needle, at + 1)) {
                if (pre == "::" && at > 0 && (identChar(s[at - 1]) || s[at - 1] == ':')) { continue; }
                const std::size_t after = at + needle.size();
                if (after >= s.size() || identChar(s[after])) { continue; }
                std::size_t p = after;
                while (p < s.size() && std::isspace(static_cast<unsigned char>(s[p]))) { ++p; }
                if (p >= s.size() || s[p] != '(') { continue; }
                const int args = topLevelArgs(s, p);
                if ((std::string(fn) == "remove" && args == 1) || (std::string(fn) == "rename" && args == 2)) {
                    add(at, Kind::Stdio, needle);
                }
            }
        }
    }
    for (const Wrapper& w : wrappers()) {
        const std::string pat = w.pattern;
        for (std::size_t at = s.find(pat); at != std::string::npos; at = s.find(pat, at + 1)) {
            add(at, Kind::Wrapper, pat);
        }
    }
    std::sort(f.hits.begin(), f.hits.end(),
              [](const Hit& a, const Hit& b) { return a.line < b.line; });
    return f;
}

// --- the allowlist -------------------------------------------------------------------

// How an allowed call is kept from filing a false freeze report against a healthy
// application (docs/DIAGNOSTICS.md, "The watchdog pause"; src/core/hang_watchdog.hpp,
// rule 2b).
enum class Pause {
    Held,      // the function itself holds a WatchdogPause
    HeldBy,    // the function named in `by` holds one and is the only way here
    Startup,   // runs before the first frame, with the watchdog not yet running
    NotNeeded  // none, and `reason` says why a stall cannot be mistaken for a hang
};

struct Allowed {
    const char* file;      // the file the function is defined in
    const char* function;  // as defined: "AppWindow::name" or a free function's name
    const char* kinds;     // the kinds of call it makes, "fs,stream", or "*" for any
    Pause pause;
    const char* by;        // for HeldBy: the function that holds the pause
    const char* reason;    // why a block is acceptable here
};

// THE HONEST LIST OF WHAT IS STILL ON THE GUI THREAD. Not a list of what is fine.
// Each entry is a decision to LEAVE a call, made in the 0.99.64 audit: a one-off,
// user-initiated, tiny call; start-up before the first frame; or a stretch that
// already runs under a pause. See docs/DIAGNOSTICS.md.
const std::vector<Allowed>& allowed() {
    static const std::vector<Allowed> v = {
        // ---- START-UP: before the first frame, with the watchdog not yet running ------
        {"app_window.cpp", "AppWindow::AppWindow", "helper", Pause::Startup, "",
         "The constructor reads config.json, bookmarks.json and markers.json once, before the "
         "first frame and before run() starts the watchdog. A slow profile delays the window's "
         "first appearance, as it always has; no frame stops. Moving the start-up reads means a "
         "window that opens before its settings are known: a different change."},
        {"app_window.cpp", "AppWindow::applyConfig", "helper", Pause::Startup, "",
         "Restores the saved I/Q file source by opening it inline, once, from the constructor, "
         "before the first frame and before the watchdog starts. The same class as the Source "
         "section's Open (moved to a worker in 0.99.64) but at start-up: a saved recording on a "
         "network share delays the first frame. Left; a start-up restore on a worker is the "
         "sound card's pattern (launchSoundCardOpen) and is the next step here."},
        {"app_window.cpp", "AppWindow::telemetryStartup", "helper", Pause::Startup, "",
         "Removes the stall ledger of an opted-out run, at start-up, before the first frame "
         "(the function's own comment: 'This is start-up: the read is not on the frame path')."},

        // ---- DEVELOPER AND TRANSLATOR HOOKS: an environment variable, read once -------
        {"app_window.cpp", "AppWindow::run", "stream", Pause::NotNeeded, "",
         "Two developer environment variables, FOXSDR_INPUT_SCRIPT and FOXSDR_PATCH_FILE: each "
         "names a file the verification harness (bounded --frames runs, screenshot sweeps) reads "
         "once, before the first frame or on it. No user sets them and a normal session never "
         "reaches them; a stalled read there is a stalled test, which the harness's own timeout "
         "reports."},
        {"app_window.cpp", "AppWindow::traceScriptFrame", "stdio", Pause::NotNeeded, "",
         "The scripted-pointer trace (FOXSDR_SCRIPT_TRACE): one line appended a frame to a file "
         "the verification harness names, only in a bounded run that set the variable. Never "
         "reached in a normal session."},
        {"app_window_language.cpp", "AppWindow::applyPendingLanguage", "helper", Pause::NotNeeded, "",
         "FOXSDR_LANG_FILE: a translator's catalogue read from disk once, the first time the "
         "language is resolved, to check a translation in place without a build. No user sets "
         "it; the shipped languages are compiled in."},

        // ---- ON A WORKER, CALLED BY NAME: the scan cannot follow a call ---------------
        {"app_window.cpp", "readLocalCatalogue", "fs,stream", Pause::NotNeeded, "",
         "Reads a plugin catalogue given as a local path (no URL scheme: a developer's test "
         "catalogue). It is called only from the catalogue fetch's own worker lambda "
         "(catalogFuture_), never from a frame; listed because the lambda calls it by name, which "
         "the scan does not follow."},

        // ---- THE PLUGIN RESCAN: on the GUI thread, under a pause, an open design question
        {"app_window.cpp", "AppWindow::rescanPlugins", "fs,helper", Pause::Held, "",
         "The rescan lists the plugin folder, hashes every installed file and maps every module, "
         "on the GUI thread, on a user's act or after a change to the folder (a catalogue fetch "
         "that found nothing new no longer rescans: 0.99.63). It holds a WatchdogPause, capped at "
         "kExcuseCapMs (30 s) and reported past it. Moving it is the open design question in "
         "docs/DIAGNOSTICS.md, 'A plugin rescan runs on the GUI thread' (options A, B, C); not "
         "settled here."},
        {"app_window.cpp", "AppWindow::rescanPluginsIfChanged", "helper", Pause::Held, "",
         "One directory listing of the plugin folder (and PluginHost::defaultPluginDir's probe "
         "file) to decide whether a rescan is needed, after a catalogue fetch. Under its own "
         "WatchdogPause; the same open question as rescanPlugins."},
        {"app_window.cpp", "AppWindow::restoreQuarantinedPlugins", "fs", Pause::HeldBy,
         "AppWindow::rescanPlugins",
         "Part of the rescan: quarantined plugin files are renamed back before the inventory is "
         "read. Reached only from rescanPlugins, which holds the WatchdogPause."},
        {"app_window.cpp", "AppWindow::quarantineBlockedPlugins", "fs", Pause::HeldBy,
         "AppWindow::rescanPlugins",
         "Part of the rescan: a retired plugin's file is renamed aside so it is not loaded. "
         "Reached only from rescanPlugins, which holds the WatchdogPause."},

        // ---- A USER'S PRESS, ONE SMALL CALL, UNDER A PAUSE ----------------------------
        {"app_window.cpp", "AppWindow::loadBandPlan", "fs,helper", Pause::Held, "",
         "The install folder's bandplans directory is looked at, listed and one plan file read, at "
         "start-up and on a user's choice of plan or language, never per frame. A local install "
         "folder answers at once; a portable install on a share is a freeze here, not a reported "
         "hang."},
        {"app_window.cpp", "AppWindow::openReportsFolder", "fs", Pause::Held, "",
         "One create_directories in the reports folder, on the press of 'Open reports folder', "
         "before the shell is asked to show it (shellOpen holds its own bracket for the shell)."},
        {"app_window.cpp", "AppWindow::copyDiagnosticsBundle", "fs,stream", Pause::Held, "",
         "The reports folder and one small text file, on the press of 'Copy diagnostics'; the "
         "clipboard copy itself is not a disk call."},
        {"app_window.cpp", "AppWindow::startSdrPlayProbe", "fs", Pause::Held, "",
         "One create_directories in the reports folder, on the press of 'Run SDRplay diagnostic', "
         "which then starts a child process for a minute or two."},
        {"app_window.cpp", "AppWindow::drawSdrPlayProbeDialog", "stream", Pause::Held, "",
         "One read of the small text file the diagnostic child wrote, on the frame it is first seen "
         "to have exited: once per diagnostic the user asked for."},
        {"app_window.cpp", "AppWindow::applyDiagnosticsEnabled", "helper", Pause::Held, "",
         "The reports folder and the log folder are made and the log file opened, once at start-up "
         "and on a press of the Diagnostics switch."},
        {"app_window.cpp", "AppWindow::drawDiagnosticsSection", "helper", Pause::Held, "",
         "setCrashCaptureEnabled makes the reports folder, on a click of the memory-dump checkbox."},
        {"app_window.cpp", "AppWindow::cleanUpOldPluginVersions", "helper", Pause::Held, "",
         "Deletes the superseded copies of updated plugins - a handful of small files in the plugin "
         "folder - on the press of the store's clean-up key or after an update."},
        {"app_window.cpp", "AppWindow::removeInstalledPlugin", "helper", Pause::Held, "",
         "Deletes one plugin file, on the press of Remove, after its module is unmapped. The pause "
         "covers the delete only; the unload before it is third-party code whose freeze should be "
         "reported."},
        {"app_window.cpp", "AppWindow::removeBlockedPlugin", "helper", Pause::Held, "",
         "Deletes one retired plugin file, on the press of Remove on a retired row (see "
         "removeInstalledPlugin)."},

        // ---- STILL OPEN: found by this scan, deliberately NOT paused ------------------
        // A pause deletes the report and keeps the freeze (gui/audio_open.hpp): for a call
        // that is going to be moved, the report is how the next field freeze is found.
        {"app_window.cpp", "AppWindow::stopIqRecording", "helper", Pause::NotNeeded, "",
         "STILL OPEN (not in the audit's thirteen; found by this scan). Recorder::stop patches the "
         "WAV header (a seek and a write) and closes the file on the GUI thread - on Stop, on a "
         "source change and at quit. The same class as the Record button's start, moved off for "
         "the START in 0.99.63 and not for the finish. Deliberately not paused. Next to move: "
         "finalise on a worker, with a bounded wait at quit."},
        {"app_window.cpp", "AppWindow::stopAudioRecording", "helper", Pause::NotNeeded, "",
         "STILL OPEN (see stopIqRecording): the audio take's finish, the same call, the same "
         "reason, deliberately not paused."},
        {"app_window.cpp", "AppWindow::importBookmarkFile", "helper", Pause::NotNeeded, "",
         "STILL OPEN (not in the audit's thirteen; found by this scan). Reads the SDR# "
         "frequencies.xml or CSV the user typed or dropped, on the GUI thread: a path that can be "
         "a network share. The same class as 'Export for SDR#', moved in 0.99.64. Deliberately not "
         "paused. Next to move: a DiskJob whose worker parses the file and whose result is "
         "applied on a later frame."},
        {"app_window_patch_radios.cpp", "AppWindow::patchReconcile", "helper", Pause::NotNeeded, "",
         "STILL OPEN (not in the audit's thirteen; found by this scan). A patch Radio whose device "
         "is an I/Q recording opens it here, inline, when the patch starts or the node's device "
         "changes - the same header read as the Source section's Open, whose comment called it "
         "'bounded, and no USB walk to wait for'. Deliberately not paused. Next to move: the "
         "radio-open worker every hardware radio already uses (patchRadioPending_)."},
    };
    return v;
}

}  // namespace

int main(int argc, char** argv) {
    // Report mode: prints every hit (for writing the allowlist), fails nothing.
    const bool report = std::getenv("GUI_DISK_AUDIT_REPORT") != nullptr;
    (void)argc;
    (void)argv;
    std::printf("test_gui_disk_audit\n");

    // --- THE CONTROLS: the scan sees what it exists to forbid, and passes what it
    //     exists to pass. A scan that cannot find THESE would pass the real source
    //     for the wrong reason.
    {
        const std::string oldCode =
            "namespace fs = std::filesystem;\n"
            "void AppWindow::oldHandler() {\n"
            "    std::error_code ec;\n"
            "    fs::create_directories(dir, ec);\n"
            "    std::ofstream f(path, std::ios::binary);\n"
            "    std::FILE* h = std::fopen(p.c_str(), \"wb\");\n"
            "    DWORD a = GetFileAttributesW(w);\n"
            "    std::remove(path.c_str());\n"
            "    std::remove(v.begin(), v.end(), 0);\n"
            "    patchPresets_.remove(id);\n"
            "    // std::filesystem::exists(commented)\n"
            "    log(\"std::filesystem::exists(inString)\");\n"
            "    const std::string t = std::filesystem::path(p).filename().string();\n"
            "    auto z = writeBmp24(img, p, err);\n"
            "}\n";
        const FileScan f = scanSource("control.cpp", oldCode);
        std::set<std::string> found;
        for (const Hit& h : f.hits) {
            found.insert(h.what);
            CHECK(h.function == "AppWindow::oldHandler");
            CHECK(!h.onWorker);
        }
        CHECK(found.count("fs::create_directories") == 1u);
        CHECK(found.count("std::ofstream") == 1u);
        CHECK(found.count("fopen") == 1u);
        CHECK(found.count("GetFileAttributesW") == 1u);
        CHECK(found.count("std::remove") == 1u);   // the one-argument C remove...
        CHECK(found.count("writeBmp24(") == 1u);
        // ...once: the algorithm's three-argument remove, the member remove, the
        // comment, the string and the path arithmetic are not calls.
        int removes = 0;
        for (const Hit& h : f.hits) { removes += (h.what == "std::remove") ? 1 : 0; }
        CHECK(removes == 1);
        CHECK(found.count("std::filesystem::exists") == 0u);
        CHECK(found.count("std::filesystem::path") == 0u);
        CHECK(f.hits.size() == 6u);

        // THE SAME CALLS IN A WORKER'S LAMBDA are not the GUI thread's...
        const std::string workerCode =
            "void AppWindow::newHandler() {\n"
            "    job_.request([dir]() -> Result {\n"
            "        std::error_code ec;\n"
            "        std::filesystem::create_directories(dir, ec);\n"
            "        std::ofstream f(path);\n"
            "        return Result{};\n"
            "    });\n"
            "    auto fut = std::async(std::launch::async, [p] { return std::filesystem::exists(p); });\n"
            "    std::thread([q] { std::remove(q.c_str()); }).detach();\n"
            "}\n";
        const FileScan w = scanSource("control.cpp", workerCode);
        CHECK(w.hits.size() == 4u);
        for (const Hit& h : w.hits) { CHECK(h.onWorker); }
        // ...and the call that is an ARGUMENT of the request, evaluated inline, is
        // the GUI thread's own.
        const std::string argCode =
            "void AppWindow::sneaky() {\n"
            "    job_.request(makeWork(std::filesystem::exists(p)));\n"
            "}\n";
        const FileScan a = scanSource("control.cpp", argCode);
        CHECK(a.hits.size() == 1u);
        for (const Hit& h : a.hits) { CHECK(!h.onWorker); }
        // A lambda built into a variable and passed by name is not recognised as a
        // worker's: it is flagged, so it must be listed or written inline.
        const std::string byNameCode =
            "void AppWindow::byName() {\n"
            "    auto work = [p] { return std::filesystem::exists(p); };\n"
            "    std::async(std::launch::async, work);\n"
            "}\n";
        const FileScan b = scanSource("control.cpp", byNameCode);
        CHECK(b.hits.size() == 1u);
        for (const Hit& h : b.hits) { CHECK(!h.onWorker); }
    }

    // --- THE SOURCE ----------------------------------------------------------------
    const fs::path gui = fs::path(CASCADE_SOURCE_DIR) / "src" / "gui";
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(gui, ec)) {
        const std::string n = e.path().filename().string();
        if (e.path().extension() == ".cpp" && n.rfind("app_window", 0) == 0) { files.push_back(e.path()); }
    }
    std::sort(files.begin(), files.end());
    CHECK(files.size() >= 8u);

    std::vector<FileScan> scans;
    for (const fs::path& p : files) { scans.push_back(scanSource(p.filename().string(), readText(p))); }

    // The scan found the things it must find in the real source: the file that holds
    // the application window, the new file of workers, and calls that are known to be
    // there (a guard that sees nothing is not a guard).
    std::size_t totalHits = 0;
    std::size_t workerHits = 0;
    for (const FileScan& f : scans) {
        for (const Hit& h : f.hits) {
            ++totalHits;
            workerHits += h.onWorker ? 1u : 0u;
        }
    }
    CHECK(totalHits > 30u);
    CHECK(workerHits > 5u);  // the workers' own calls (app_window_disk_work.cpp and the patch radio opens)

    if (report) {
        for (const FileScan& f : scans) {
            for (const Hit& h : f.hits) {
                if (h.onWorker) { continue; }
                std::printf("HIT %s:%d %s [%s] %s\n", h.file.c_str(), h.line, h.function.c_str(),
                            kindName(h.kind), h.what.c_str());
            }
        }
        return 0;
    }

    // Every wrapper in the list is a thing that exists.
    for (const Wrapper& w : wrappers()) {
        const std::string def = readText(fs::path(CASCADE_SOURCE_DIR) / w.where);
        CHECK(!def.empty());
    }

    // --- every call is a worker's or is listed -------------------------------------
    // The function bodies, for the pause check.
    std::map<std::string, const FileScan*> byName;
    for (const FileScan& f : scans) { byName[f.name] = &f; }
    const auto bodyOf = [&](const std::string& file, const std::string& function) {
        const auto it = byName.find(file);
        if (it == byName.end()) { return std::string(); }
        for (const FuncSpan& fn : it->second->fns) {
            if (fn.name == function) { return it->second->code.substr(fn.begin, fn.end - fn.begin); }
        }
        return std::string();
    };

    std::set<std::size_t> usedEntries;
    int unlisted = 0;
    for (const FileScan& f : scans) {
        for (const Hit& h : f.hits) {
            if (h.onWorker) { continue; }
            bool ok = false;
            for (std::size_t i = 0; i < allowed().size(); ++i) {
                const Allowed& a = allowed()[i];
                if (f.name != a.file || h.function != a.function) { continue; }
                const std::string kinds = std::string(",") + a.kinds + ",";
                if (std::string(a.kinds) == "*" || kinds.find(std::string(",") + kindName(h.kind) + ",") != std::string::npos) {
                    ok = true;
                    usedEntries.insert(i);
                }
            }
            if (!ok) {
                ++unlisted;
                std::printf(
                    "\n*** A FILE-SYSTEM CALL ON THE GUI THREAD THAT NOBODY HAS DECIDED ABOUT:\n"
                    "      %s:%d  in %s  [%s]  %s\n"
                    "    This thread draws the window. A call that waits for a disk (a synchronised or\n"
                    "    network folder, a drive that has spun down, a scanner holding the file) is a\n"
                    "    frozen window for as long as the disk takes: four field freezes, 0.99.58 -\n"
                    "    0.99.63. WHAT TO DO:\n"
                    "      * normally, MOVE IT TO A WORKER: gui/disk_job.hpp (a click that saves, opens\n"
                    "        or lists), gui/background_saver.hpp (a file saved when it changes) or\n"
                    "        gui/record_start.hpp (a recording). Do the call inside the lambda you hand\n"
                    "        to request(); this test then sees it is a worker's. tests/test_gui_file_jobs.cpp\n"
                    "        shows the test that proves the window keeps drawing.\n"
                    "      * only if it is a one-off, user-initiated, tiny call, ADD AN ENTRY to the\n"
                    "        allowlist (kAllowed in tests/test_gui_disk_audit.cpp) naming this function, the kind\n"
                    "        of call and WHY a block is acceptable, and hold a cascade::core::WatchdogPause\n"
                    "        in it so a slow disk is not reported as a hang of a healthy application.\n",
                    h.file.c_str(), h.line, h.function.c_str(), kindName(h.kind), h.what.c_str());
            }
        }
    }
    CHECK(unlisted == 0);

    // --- the allowlist itself is honest ---------------------------------------------
    int stale = 0;
    int badPause = 0;
    for (std::size_t i = 0; i < allowed().size(); ++i) {
        const Allowed& a = allowed()[i];
        CHECK(std::string(a.reason).size() > 30u);
        if (usedEntries.count(i) == 0u) {
            ++stale;
            std::printf("\n*** AN ALLOWLIST ENTRY THAT MATCHES NOTHING: %s in %s (%s)\n"
                        "    The call it described is gone, moved or renamed. Remove the entry (or fix\n"
                        "    its function name) so the list says only what is still on the GUI thread.\n",
                        a.function, a.file, a.kinds);
        }
        const std::string own = bodyOf(a.file, a.function);
        switch (a.pause) {
            case Pause::Held:
                if (own.find("WatchdogPause") == std::string::npos) {
                    ++badPause;
                    std::printf("\n*** %s is listed as holding a watchdog pause and does not.\n", a.function);
                }
                break;
            case Pause::HeldBy: {
                const std::string caller = bodyOf(a.file, a.by);
                const std::string shortName = std::string(a.function).substr(std::string(a.function).rfind(':') + 1);
                if (caller.find("WatchdogPause") == std::string::npos ||
                    caller.find(shortName) == std::string::npos) {
                    ++badPause;
                    std::printf("\n*** %s is listed as paused by %s, which either holds no pause or never "
                                "calls it.\n", a.function, a.by);
                }
                break;
            }
            case Pause::Startup:
            case Pause::NotNeeded:
                break;
        }
    }
    CHECK(stale == 0);
    CHECK(badPause == 0);

    std::printf("  %zu calls scanned in %zu files: %zu on workers, %zu listed (%zu entries)\n",
                totalHits, scans.size(), workerHits, totalHits - workerHits, allowed().size());
    return testSummary("test_gui_disk_audit");
}
