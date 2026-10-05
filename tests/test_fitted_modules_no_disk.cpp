// The Fitted modules window asks the file system for NOTHING on a frame.
//
// WHAT WENT WRONG. drawFittedModulesWindow() builds the window's model every
// frame it is open, and for every loaded module it called
// std::filesystem::file_size() to print the file's size: one synchronous stat
// per plugin, per frame, on the thread that draws the window. A stat is cheap
// until the folder is not - a network or synchronised profile, an antivirus
// scanner holding the files, a disk that spun down, a Store package's redirected
// profile - and then every frame is as slow as the slowest of twenty answers.
// It is the fault class of the settings-folder poll that froze the window on
// 0.99.59 (commit 4d96604, release notes 0.99.61), which flagged this very call
// as "the same class, still on the GUI thread", and it is worse than that poll,
// which ran once a second: this ran sixty times a second, for as long as the
// window was open - and the window is the one the plugin rescan key lives on.
// On a machine whose disk stalled (the same session's Record button blocked in
// file creation), an open Fitted modules window is a freeze waiting for the
// next slow answer. One stat of an unreachable share measured 26.7 s on the
// machine this was written on.
//
// THE FIX is that the size is the record's: PluginHost::scan() measures each
// file once, from the directory listing it is already reading
// (LoadedPlugin::fileBytes, tests/test_plugin_host.cpp), and the window carries
// it (makeFittedModule, tests/test_plugins_view.cpp). A figure that may be as
// old as the last scan is the whole of the cost.
//
// WHAT THIS TEST HOLDS, because no test can run the window against a slow disk:
// the call site lives in AppWindow, which needs a GL context and an ImGui frame,
// so a regression that put a stat back would pass every test that exercises the
// record and the adapter. The source scan is the check that the draw function
// does not go back to the disk - as the link-request poll's scan does for its
// call - and the control below proves the scan can see the call it is looking
// for: it is run against the code the window had before the fix.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "test_check.hpp"

namespace fs = std::filesystem;

namespace {

// The file's text with Windows line endings folded to \n: a checkout on Windows
// carries CRLF (autocrlf), and a function's closing brace is "\r\n}\r\n" there.
std::string readText(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c != '\r') { out += c; }
    }
    return out;
}

// The text of the function whose definition begins with `signature`, up to the
// closing brace in column 0, or "" when it is not in `text`.
std::string functionBody(const std::string& text, const std::string& signature) {
    const std::size_t at = text.find(signature);
    if (at == std::string::npos) { return std::string(); }
    const std::size_t end = text.find("\n}\n", at);
    return text.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

// `text` with every // comment removed, so what a comment SAYS about the old
// behaviour (and it says a good deal) is not mistaken for a call.
std::string withoutLineComments(const std::string& text) {
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t eol = text.find('\n', i);
        const std::size_t end = eol == std::string::npos ? text.size() : eol;
        std::string line = text.substr(i, end - i);
        const std::size_t slashes = line.find("//");
        if (slashes != std::string::npos) { line.erase(slashes); }
        out += line;
        out += '\n';
        i = end + 1;
    }
    return out;
}

// Every way the C++ standard library or the C runtime asks the disk something,
// as it would be spelled in this file. Deliberately wide: a window that opened
// a file or listed a directory per frame is the same defect as one that stats.
const std::vector<std::string>& diskCalls() {
    static const std::vector<std::string> calls = {
        "std::filesystem::", "fs::",        "file_size(",  "is_regular_file(", "exists(",
        "last_write_time(",  "directory_iterator", "std::ifstream", "std::ofstream",
        "std::fstream",      "fopen(",      "_wfopen(",    "stat(",
        "GetFileAttributes", "FindFirstFile", "CreateFile",
    };
    return calls;
}

// The disk calls `body` makes, by spelling.
std::vector<std::string> diskCallsIn(const std::string& body) {
    std::vector<std::string> found;
    const std::string code = withoutLineComments(body);
    for (const std::string& call : diskCalls()) {
        if (code.find(call) != std::string::npos) { found.push_back(call); }
    }
    return found;
}

}  // namespace

int main() {
    std::printf("test_fitted_modules_no_disk\n");

    // --- THE CONTROL: the scan sees the call it exists to forbid ------------
    //
    // The window as it was before the fix, as the code spelled it. A scan that
    // cannot find THIS would pass the real source for the wrong reason - a
    // renamed function, a signature that no longer matches - and the test would
    // be green and blind.
    const std::string before =
        "void AppWindow::drawFittedModulesWindow() {\n"
        "    for (const LoadedPlugin& p : list) {\n"
        "        // THE ONLY SIZE THERE IS.\n"
        "        std::error_code sizeEc;\n"
        "        const std::uintmax_t bytes = std::filesystem::file_size(p.path, sizeEc);\n"
        "        if (!sizeEc) { m.sizeBytes = bytes; }\n"
        "    }\n"
        "}\n";
    const std::vector<std::string> controlHits =
        diskCallsIn(functionBody(before, "void AppWindow::drawFittedModulesWindow("));
    CHECK(!controlHits.empty());
    // ...and a comment ABOUT a stat is not one: the rule is on code.
    CHECK(diskCallsIn("void f() {\n    // std::filesystem::file_size(p) was here\n    int x = 1;\n}\n")
              .empty());

    // --- THE WINDOW, as it is ----------------------------------------------
    const fs::path appWindow = fs::path(CASCADE_SOURCE_DIR) / "src" / "gui" / "app_window.cpp";
    const std::string source = readText(appWindow);
    CHECK(!source.empty());
    const std::string body = functionBody(source, "void AppWindow::drawFittedModulesWindow(");
    std::printf("  drawFittedModulesWindow: %zu bytes of source\n", body.size());
    // The function was found, and what was cut out is that function and not the
    // rest of a 600 KB file: a body that ran on past its brace would be scanned
    // for disk calls it does not make (the window's neighbours open files).
    CHECK(body.size() > 2000);
    CHECK(body.size() < 40000);

    const std::vector<std::string> hits = diskCallsIn(body);
    for (const std::string& h : hits) {
        std::printf("  drawFittedModulesWindow asks the disk: %s\n", h.c_str());
    }
    CHECK(hits.empty());

    // The window still builds its rows through makeFittedModule, which is where
    // the record's size is carried to the plate (tests/test_plugins_view.cpp): a
    // window that stopped building them would pass the scan above by deleting
    // the feature.
    CHECK(withoutLineComments(body).find("makeFittedModule(") != std::string::npos);

    return testSummary("test_fitted_modules_no_disk");
}
