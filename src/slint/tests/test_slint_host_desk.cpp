// The new window's hands on a POSIX desktop (src/slint/desk_logic.hpp; docs/multi-radio-records/linux-local.md section 11): the address rule, the PATH search, the argument arrays
// of the openers and the clipboard tools, the choice of clipboard tool by session, the duties through a stand-in runner (no browser, file manager or clipboard tool is started by
// this test except the stand-in scripts it writes itself), the real runner started on a stand-in `xdg-open` that records what it was given, and a scan that the window's call
// sites really call the module (so a control cannot be drawn and do nothing off Windows).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "desk_logic.hpp"
#include "test_check.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

using namespace foxsdr::slintdesk;
namespace fs = std::filesystem;

namespace {

fs::path makeDir(const char* tag) {
    const fs::path d = fs::temp_directory_path() / (std::string("foxsdr-desk-") + tag + "-" + std::to_string(static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count())));
    fs::create_directories(d);
    return d;
}

void touchExecutable(const fs::path& p, const std::string& body = "#!/bin/sh\nexit 0\n") {
    std::ofstream(p, std::ios::binary) << body;
#if !defined(_WIN32)
    ::chmod(p.c_str(), 0755);
#endif
}

std::string readAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream o;
    o << f.rdbuf();
    return o.str();
}

#if defined(_WIN32)
const char kSepStr[] = ";";
#else
const char kSepStr[] = ":";
#endif

void addresses() {
    CHECK(addressOk("https://foxsdr.example/manual"));
    CHECK(addressOk("https://a.b/c?d=e&f=g#h"));
    CHECK(!addressOk("http://example.com/"));                  // https only, as the Windows side
    CHECK(!addressOk("file:///etc/passwd"));
    CHECK(!addressOk("javascript:alert(1)"));
    CHECK(!addressOk("https://"));                              // nothing after the scheme
    CHECK(!addressOk(""));
    CHECK(!addressOk("https://a b"));                           // a blank
    CHECK(!addressOk("https://a\tb"));
    CHECK(!addressOk("https://a\nb"));
    CHECK(!addressOk(std::string("https://a") + '\0' + "b"));
    CHECK(!addressOk("https://a\"b"));
    CHECK(!addressOk("https://a'b"));
    CHECK(!addressOk("https://a<b>"));
    CHECK(!addressOk("https://a\\b"));
    CHECK(!addressOk("https://a`b"));
    CHECK(addressOk("https://" + std::string(502, 'a')));       // 510 bytes: the longest
    CHECK(!addressOk("https://" + std::string(504, 'a')));      // 512 bytes: refused
    CHECK(addressOk("https://example.com/;touch%20x"));         // shell characters are only text: the command is an argument array
}

void pathSearch() {
    const fs::path a = makeDir("a"), b = makeDir("b");
    touchExecutable(b / "xdg-open");
    const std::string list = a.string() + kSepStr + b.string();
    CHECK(findOnPath("xdg-open", list) == (b / "xdg-open").string());   // the first directory that has it
    touchExecutable(a / "xdg-open");
    CHECK(findOnPath("xdg-open", list) == (a / "xdg-open").string());
    CHECK(findOnPath("nothing-here", list).empty());
    CHECK(findOnPath("xdg-open", "").empty());
    CHECK(findOnPath("", list).empty());
    CHECK(findOnPath("../xdg-open", list).empty());                      // a name, never a path
    CHECK(findOnPath("sub/xdg-open", list).empty());
    // an empty entry of the list means "here" to a shell; the working directory is never searched
    const fs::path cwd = fs::current_path();
    touchExecutable(cwd / "foxsdr-desk-probe-tool");
    CHECK(findOnPath("foxsdr-desk-probe-tool", std::string(kSepStr) + b.string()).empty());
    fs::remove(cwd / "foxsdr-desk-probe-tool");
#if !defined(_WIN32)
    // a file that is not executable is not found
    std::ofstream((a / "plain-file").string()) << "x";
    ::chmod((a / "plain-file").c_str(), 0644);
    CHECK(findOnPath("plain-file", a.string()).empty());
#endif
    fs::remove_all(a);
    fs::remove_all(b);
}

void argumentArrays() {
    CHECK(absoluteForOpener("-rf").front() != '-');                      // an option can never be read from a path
    CHECK(absoluteForOpener("--version").front() != '-');
    CHECK(absoluteForOpener("").empty());
    const std::string rel = absoluteForOpener("some/relative/dir");
    CHECK(fs::path(rel).is_absolute());

    const Command w = copyCommand(ClipboardTool::WlClipboard, "/u/bin/wl-copy", "text");
    CHECK(w.argv.size() == 3 && w.argv[0] == "/u/bin/wl-copy" && w.argv[1] == "--type" && w.argv[2] == "text/plain;charset=utf-8");
    CHECK(w.input == "text" && w.wait && !w.capture);
    const Command x = copyCommand(ClipboardTool::Xclip, "/u/bin/xclip", "t");
    CHECK(x.argv == (std::vector<std::string>{"/u/bin/xclip", "-selection", "clipboard", "-in"}));
    const Command s = copyCommand(ClipboardTool::Xsel, "/u/bin/xsel", "t");
    CHECK(s.argv == (std::vector<std::string>{"/u/bin/xsel", "--clipboard", "--input"}));
    CHECK(copyCommand(ClipboardTool::None, "", "t").argv.empty());
    const Command pw = pasteCommand(ClipboardTool::WlClipboard, (fs::path("/u/bin") / "wl-copy").string());
    CHECK(pw.argv.size() == 2 && fs::path(pw.argv[0]).filename() == "wl-paste" && pw.argv[1] == "--no-newline" && pw.capture && pw.wait);
    CHECK(pasteCommand(ClipboardTool::Xclip, "/u/bin/xclip").argv == (std::vector<std::string>{"/u/bin/xclip", "-selection", "clipboard", "-out"}));
    CHECK(pasteCommand(ClipboardTool::Xsel, "/u/bin/xsel").argv == (std::vector<std::string>{"/u/bin/xsel", "--clipboard", "--output"}));
    CHECK(pasteCommand(ClipboardTool::None, "").argv.empty());
}

void toolChoice() {
    const fs::path d = makeDir("tools");
    touchExecutable(d / "wl-copy");
    touchExecutable(d / "xclip");
    touchExecutable(d / "xsel");
    const std::string list = d.string();
    std::string tool;
    // Wayland session: wl-clipboard first
    CHECK(chooseClipboardTool("wayland-0", ":0", list, &tool) == ClipboardTool::WlClipboard && fs::path(tool).filename() == "wl-copy");
    // X11 session: xclip before xsel
    CHECK(chooseClipboardTool("", ":0", list, &tool) == ClipboardTool::Xclip && fs::path(tool).filename() == "xclip");
    fs::remove(d / "xclip");
    CHECK(chooseClipboardTool("", ":0", list, &tool) == ClipboardTool::Xsel);
    // no display, no tool: nothing to copy to
    CHECK(chooseClipboardTool("", "", list, &tool) == ClipboardTool::None);
    fs::remove(d / "wl-copy");
    CHECK(chooseClipboardTool("wayland-0", "", list, &tool) == ClipboardTool::None);          // Wayland, but only xsel installed and no X display
    CHECK(chooseClipboardTool("wayland-0", ":0", list, &tool) == ClipboardTool::Xsel);      // XWayland: the X tool serves
    fs::remove_all(d);
}

struct Seen {
    std::vector<Command> commands;
    bool answer = true;
    std::string canned;
};

void duties() {
    const fs::path d = makeDir("duties");
    touchExecutable(d / "xdg-open");
    touchExecutable(d / "xclip");
    setPathOverride(d.string());
    Seen seen;
    setRunner([&](const Command& c, std::string& out) {
        seen.commands.push_back(c);
        if (c.capture) { out = seen.canned; }
        return seen.answer;
    });
#if !defined(_WIN32)
    ::setenv("DISPLAY", ":99", 1);
    ::unsetenv("WAYLAND_DISPLAY");
#endif
    std::string why;
    // an address
    CHECK(openAddress("https://example.com/a;b", &why) && why.empty());
    CHECK(seen.commands.size() == 1 && seen.commands[0].argv == (std::vector<std::string>{(d / "xdg-open").string(), "https://example.com/a;b"}) && !seen.commands[0].wait);
    // a refused address starts nothing and says why
    seen.commands.clear();
    CHECK(!openAddress("http://example.com/", &why) && !why.empty() && seen.commands.empty());
    CHECK(!openAddress("https://a b", &why) && seen.commands.empty());
    // a folder and a file are made absolute and passed as one argument, even when odd
    CHECK(openFolder("--version", &why));
    CHECK(seen.commands.size() == 1 && seen.commands[0].argv.size() == 2 && seen.commands[0].argv[1].front() != '-' && fs::path(seen.commands[0].argv[1]).is_absolute());
    seen.commands.clear();
#if !defined(_WIN32)
    CHECK(openFile("/tmp/a b/it's; rm -rf.wav", &why) && seen.commands.size() == 1 && seen.commands[0].argv.size() == 2 && seen.commands[0].argv[1] == "/tmp/a b/it's; rm -rf.wav");
#else
    CHECK(openFile("C:\\tmp\\a b\\it's; rm -rf.wav", &why) && seen.commands.size() == 1 && seen.commands[0].argv.size() == 2 && seen.commands[0].argv[1].find("it's; rm -rf.wav") != std::string::npos);
#endif
    CHECK(!openFolder("", &why) && !openFile("", &why));
    // the runner refusing is reported, not swallowed
    seen.commands.clear();
    seen.answer = false;
    why.clear();
    CHECK(!openAddress("https://example.com/", &why) && why.find("could not be started") != std::string::npos);
    seen.answer = true;
#if !defined(_WIN32)
    // the clipboard
    seen.commands.clear();
    CHECK(copyText("14.074.000 USB\nline two \xc3\xa9", &why));
    CHECK(seen.commands.size() == 1 && seen.commands[0].input == "14.074.000 USB\nline two \xc3\xa9" && seen.commands[0].wait);
    CHECK(seen.commands[0].argv == (std::vector<std::string>{(d / "xclip").string(), "-selection", "clipboard", "-in"}));
    seen.commands.clear();
    seen.canned = "pasted text";
    CHECK(pasteText() == "pasted text" && seen.commands.size() == 1 && seen.commands[0].capture);
    seen.answer = false;
    CHECK(pasteText().empty());
    seen.answer = true;
#endif
    // no xdg-open: nothing happens and it says so
    fs::remove(d / "xdg-open");
    seen.commands.clear();
    why.clear();
    CHECK(!openAddress("https://example.com/", &why) && why.find("xdg-open") != std::string::npos && seen.commands.empty());
    // no clipboard tool: nothing copied, and it says so
    fs::remove(d / "xclip");
    why.clear();
    CHECK(!copyText("x", &why) && why.find("nothing copied") != std::string::npos && seen.commands.empty());
    CHECK(pasteText().empty());
    setRunner(nullptr);
    clearPathOverride();
    fs::remove_all(d);
}

#if !defined(_WIN32)
// The real runner, on a stand-in xdg-open that writes down what it was given: the address arrives as ONE argument and nothing in it is read by a shell.
void realRunner() {
    const fs::path d = makeDir("real");
    const fs::path log = d / "args.txt";
    touchExecutable(d / "xdg-open", "#!/bin/sh\nprintf '%s\\n' \"$#\" \"$1\" > '" + log.string() + ".tmp'\nmv '" + log.string() + ".tmp' '" + log.string() + "'\n");
    setPathOverride(d.string());
    setRunner(nullptr);
    const std::string url = "https://example.com/a;touch$IFS" + (d / "PWNED").string() + "&x=$(id)";
    CHECK(openAddress(url));
    for (int i = 0; i < 100 && !fs::exists(log); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
    CHECK(fs::exists(log));
    const std::string got = readAll(log);
    CHECK(got == "1\n" + url + "\n");
    CHECK(!fs::exists(d / "PWNED"));
    // a clipboard stand-in that stores its standard input
    const fs::path clip = d / "clip.txt";
    touchExecutable(d / "xclip", "#!/bin/sh\ncat > '" + clip.string() + "'\n");
    ::setenv("DISPLAY", ":99", 1);
    ::unsetenv("WAYLAND_DISPLAY");
    const std::string text = std::string("a\nb ") + "\xe2\x80\x94" + " $HOME `x` 'q'\n";
    std::string why;
    CHECK(copyText(text, &why));
    CHECK(readAll(clip) == text);
    // a paste stand-in that prints text, without a trailing newline
    touchExecutable(d / "xclip", "#!/bin/sh\nprintf 'from the clipboard'\n");
    CHECK(pasteText() == "from the clipboard");
    // a program that fails is reported as a failed copy
    touchExecutable(d / "xclip", "#!/bin/sh\nexit 3\n");
    CHECK(!copyText("x", &why) && !why.empty());
#if !defined(_WIN32)
    // an opener that is not an executable file is never found, so nothing is started
    touchExecutable(d / "xdg-open");
    ::chmod((d / "xdg-open").c_str(), 0644);
    why.clear();
    CHECK(!openAddress("https://example.com/", &why) && why.find("xdg-open") != std::string::npos);
#endif
    clearPathOverride();
    fs::remove_all(d);
}
#endif

// Every place the window used to do these things only on Windows now asks the module off Windows.
void callSites() {
    const fs::path src = fs::path(__FILE__).parent_path().parent_path();
    const char* const files[] = {"system_publish.cpp", "signal_publish.cpp", "tools_publish.cpp", "lanek_publish.cpp", "lanec_publish.cpp", "gaps_win.cpp"};
    for (const char* f : files) {
        const std::string text = readAll(src / f);
        CHECK(!text.empty());
        CHECK(text.find("slintdesk::") != std::string::npos);
    }
    // the stub answers that used to say "Windows-only" are gone
    CHECK(readAll(src / "tools_publish.cpp").find("Windows-only here") == std::string::npos);
}

}  // namespace

int main() {
    addresses();
    pathSearch();
    argumentArrays();
    toolChoice();
    duties();
#if !defined(_WIN32)
    realRunner();
#endif
    callSites();
    return testSummary("test_slint_host_desk");
}
