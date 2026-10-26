// What the start-up of the window says about itself, and the switch that picks the window
// system (0.99.78, gui/window_platform.hpp).
//
// THE FIELD REPORT (A0E0967441A8E6B4, 0.99.75, Ubuntu 24.04 AppImage): the process ended in the
// phase "creating the window" and the log said nothing about the window system - GLFW's error
// text went to stderr, which an AppImage does not keep, and nothing logged which platform GLFW
// had chosen. This file pins the pieces that now say it, one by one, without a window:
//
//   - FOXSDR_WINDOW_PLATFORM: x11 | wayland | any, anything else ignored and said so;
//   - the environment line: whether DISPLAY and WAYLAND_DISPLAY are set - PRESENCE ONLY, never
//     the value (an SSH-forwarded DISPLAY names a host) - and the session type from a closed list;
//   - the lines for the platform GLFW chose, the GL strings, and GLFW's own error text with the
//     name of the display taken out of "X11: Failed to open display <name>";
//   - that every one of them begins `window:` (what a report asks a user to send), fits the
//     log's line width without being cut, and is on disk when diagLogf returns, so a death
//     right after it cannot lose it.
//
// The real application's lines, in order, before the first frame, and `bad` ignored with the
// run still starting, are in test_window_platform_app.cpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>

#include "core/diag_log.hpp"
#include "gui/window_platform.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace cascade::gui::windowplatform;

namespace {

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

fs::path scratchDir(const char* tag) {
    const char* tmp = std::getenv("TEMP");
    const fs::path base = (tmp != nullptr && *tmp != '\0') ? fs::path(tmp) : fs::path(".");
#if defined(_WIN32)
    const unsigned long pid = ::GetCurrentProcessId();
#else
    const unsigned long pid = 0;
#endif
    const fs::path dir = base / (std::string("cascade-windowplatform-") + tag + "-" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

bool printable(const std::string& s) {
    for (const char c : s) {
        if (c < 0x20 || c > 0x7E) { return false; }
    }
    return true;
}

// Every line this header builds: begins `window:`, is plain ASCII on one line, and is short
// enough that the log (192 bytes a line, 18 of them its own stamp and level) does not cut it.
void checkLine(const std::string& line) {
    CHECK(line.rfind("window: ", 0) == 0);
    CHECK(printable(line));
    CHECK(line.size() <= kMaxLineChars);
}

// 192 bytes a log line, of which "HH:MM:SS.mmm info " takes 18.
static_assert(kMaxLineChars + 18 < 192, "a window: line must fit the log's line width");

}  // namespace

int main() {
    // =======================================================================
    // 1. The numbers are GLFW's own (the header takes them as plain ints so that it needs no
    //    GLFW header of its own; this is where they are held to the real ones)
    // =======================================================================
    // (Read through a volatile so each is a run-time comparison, not a constant condition.)
    const volatile int numbers[] = {kAny, kWin32, kCocoa, kWayland, kX11, kNull};
    CHECK(numbers[0] == GLFW_ANY_PLATFORM);
    CHECK(numbers[1] == GLFW_PLATFORM_WIN32);
    CHECK(numbers[2] == GLFW_PLATFORM_COCOA);
    CHECK(numbers[3] == GLFW_PLATFORM_WAYLAND);
    CHECK(numbers[4] == GLFW_PLATFORM_X11);
    CHECK(numbers[5] == GLFW_PLATFORM_NULL);

    // =======================================================================
    // 2. FOXSDR_WINDOW_PLATFORM: x11 | wayland | any; anything else is ignored and said so
    // =======================================================================
    {
        const Request unset = parseRequest(nullptr);
        CHECK(!unset.set && unset.valid && unset.platform == kAny);
        const Request empty = parseRequest("");
        CHECK(!empty.set && empty.valid && empty.platform == kAny);

        const Request x11 = parseRequest("x11");
        CHECK(x11.set && x11.valid && x11.platform == kX11 && x11.text == "x11");
        const Request wl = parseRequest("wayland");
        CHECK(wl.set && wl.valid && wl.platform == kWayland && wl.text == "wayland");
        const Request any = parseRequest("any");
        CHECK(any.set && any.valid && any.platform == kAny && any.text == "any");

        // Case and surrounding blanks do not matter: it is typed by a person on a command line.
        CHECK(parseRequest("X11").valid && parseRequest("X11").platform == kX11);
        CHECK(parseRequest("Wayland").valid && parseRequest("Wayland").platform == kWayland);
        CHECK(parseRequest("  wayland \t").valid && parseRequest("  wayland \t").platform == kWayland);
        CHECK(parseRequest("ANY").valid && parseRequest("ANY").platform == kAny);

        // The error branch: a value that names no platform is set, NOT valid, still means
        // "any" (so the start-up goes on with GLFW's own choice), and keeps what was typed
        // - in plain ASCII and cut short - so the line can say it.
        for (const char* bad : {"bad", "x", "x1", "x11 wayland", "win32", "null", "cocoa", "1", "wayland!", "xorg"}) {
            const Request r = parseRequest(bad);
            CHECK(r.set && !r.valid && r.platform == kAny);
            CHECK(r.text == bad);
        }
        const Request junk = parseRequest("bad\x01\x7f\xc3\xa9 value that goes on and on and on");
        CHECK(junk.set && !junk.valid && junk.platform == kAny);
        CHECK(printable(junk.text) && junk.text.size() <= 24);
        CHECK(junk.text.rfind("bad", 0) == 0);
        const Request blank = parseRequest("   ");
        CHECK(!blank.set && blank.valid);  // only blanks is nothing
    }

    // =======================================================================
    // 3. The platform as a word, and the session type from a closed list
    // =======================================================================
    {
        CHECK(std::string(platformName(kWin32)) == "win32");
        CHECK(std::string(platformName(kCocoa)) == "cocoa");
        CHECK(std::string(platformName(kWayland)) == "wayland");
        CHECK(std::string(platformName(kX11)) == "x11");
        CHECK(std::string(platformName(kNull)) == "null");
        CHECK(std::string(platformName(kAny)) == "any");
        CHECK(std::string(platformName(0)) == "unknown");  // glfwGetPlatform answers 0 on an error
        CHECK(std::string(platformName(0x00060099)) == "unknown");

        CHECK(sessionTypeWord(nullptr) == "unset");
        CHECK(sessionTypeWord("") == "unset");
        for (const char* ok : {"x11", "wayland", "tty", "mir", "unspecified"}) { CHECK(sessionTypeWord(ok) == ok); }
        // Anything else is not repeated: a variable another program set may hold anything.
        CHECK(sessionTypeWord("X11") == "other");
        CHECK(sessionTypeWord("/home/someone/session") == "other");
        CHECK(sessionTypeWord("wayland-secret") == "other");
    }

    // =======================================================================
    // 4. The environment line: presence only, never a value
    // =======================================================================
    {
        const std::vector<std::string> both = {"wayland", "x11"};
        const std::string line = environmentLine("3.4.0 X11 Wayland GLX Null EGL OSMesa monotonic", both, ":0", nullptr,
                                                 "x11");
        checkLine(line);
        CHECK(contains(line, "supports wayland x11"));
        CHECK(contains(line, "DISPLAY set"));
        CHECK(contains(line, "WAYLAND_DISPLAY not set"));
        CHECK(contains(line, "XDG_SESSION_TYPE x11"));
        CHECK(contains(line, "GLFW 3.4.0 X11 Wayland"));

        // THE VALUE OF DISPLAY IS NEVER LOGGED: an SSH-forwarded one names a host, and a
        // Wayland socket's name is the user's session.
        const std::string secret = environmentLine("3.4.0", both, "build-host.example.invalid:10.0", "wayland-secret-7",
                                                   "wayland");
        checkLine(secret);
        CHECK(!contains(secret, "build-host") && !contains(secret, "example") && !contains(secret, "10.0") &&
              !contains(secret, "secret") && !contains(secret, "wayland-secret-7"));
        CHECK(contains(secret, "DISPLAY set") && contains(secret, "WAYLAND_DISPLAY set"));
        CHECK(!contains(secret, "WAYLAND_DISPLAY not set") && !contains(secret, "; DISPLAY not set"));

        // Set but empty is not set (GLFW treats an empty DISPLAY as missing, too).
        const std::string empty = environmentLine("3.4.0", both, "", "", nullptr);
        CHECK(contains(empty, "DISPLAY not set") && contains(empty, "WAYLAND_DISPLAY not set"));
        CHECK(contains(empty, "XDG_SESSION_TYPE unset"));

        // The session type is repeated only from the closed list.
        CHECK(contains(environmentLine("3.4.0", both, nullptr, nullptr, "/home/someone/x"), "XDG_SESSION_TYPE other"));
        CHECK(!contains(environmentLine("3.4.0", both, nullptr, nullptr, "/home/someone/x"), "someone"));

        // A build with no X11 or Wayland (Windows, macOS): it says what it does have.
        const std::string win = environmentLine("3.4.0 Win32 WGL Null EGL OSMesa", {"win32"}, nullptr, nullptr, nullptr);
        checkLine(win);
        CHECK(contains(win, "supports win32"));
        const std::string nothing = environmentLine("3.4.0", {}, nullptr, nullptr, nullptr);
        checkLine(nothing);
        CHECK(contains(nothing, "supports none"));

        // Hostile or enormous inputs do not break the line.
        const std::string longVersion(500, 'v');
        const std::string cut = environmentLine(longVersion.c_str(), both, ":0", ":1", "tty");
        checkLine(cut);
        CHECK(contains(cut, "DISPLAY set") && contains(cut, "XDG_SESSION_TYPE tty"));
        const std::string odd = environmentLine("3.4.0 \x01 \xff 'quoted'", both, ":0", nullptr, "x11");
        checkLine(odd);
        CHECK(!contains(odd, "'"));  // a single-quoted span is masked to '<name>' in an upload
        CHECK(printable(environmentLine(nullptr, both, nullptr, nullptr, nullptr)));
    }

    // =======================================================================
    // 4b. The message a person is shown when the window cannot be created
    // =======================================================================
    {
        // Only a real session may block on a dialog: --frames (0 or more) never does.
        CHECK(errorDialogAllowed(-1));
        CHECK(!errorDialogAllowed(0) && !errorDialogAllowed(3) && !errorDialogAllowed(1000000));

        const std::string logPath = "C:\\Users\\a\\AppData\\Local\\FoxSDR\\logs\\foxsdr.log";
        const std::string withLog = failureMessage(logPath);
        CHECK(contains(withLog, "FoxSDR could not create its window because no working OpenGL driver was found."));
        CHECK(contains(withLog, "no graphics driver is installed"));
        CHECK(contains(withLog, "remote desktop session without GPU support"));
        CHECK(contains(withLog, "\n" + logPath));
        const std::string off = failureMessage(std::string());
        CHECK(contains(off, "no log was written") && contains(off, "Settings > Diagnostics"));
        CHECK(!contains(off, "foxsdr.log"));
        // When the software renderer was tried too, the person is told so; when it was not, the
        // message does not claim it (the default is "not tried", as every caller before 0.99.90).
        const std::string tried = failureMessage(logPath, true);
        CHECK(contains(tried, "also tried its built-in software renderer"));
        CHECK(contains(tried, "\n" + logPath));
        CHECK(!contains(withLog, "software renderer") && !contains(off, "software renderer"));
        // plain ASCII, so it reads the same in a message box of any code page
        for (const char ch : withLog + off + tried) { CHECK(ch == '\n' || (ch >= 0x20 && ch < 0x7F)); }
    }

    // =======================================================================
    // 4c. FOXSDR_GL, the old-driver check and the software renderer's lines (0.99.90)
    // =======================================================================
    {
        // The GLFW numbers, held to the real macros.
        const volatile int codes[] = {kApiUnavailable, kVersionUnavailable, kFormatUnavailable};
        CHECK(codes[0] == GLFW_API_UNAVAILABLE);
        CHECK(codes[1] == GLFW_VERSION_UNAVAILABLE);
        CHECK(codes[2] == GLFW_FORMAT_UNAVAILABLE);
        // Only the driver's refusals move the start-up to the software renderer; a platform error
        // (a window class, a display) or no error at all does not.
        CHECK(glfwErrorMeansNoDriver(GLFW_API_UNAVAILABLE));
        CHECK(glfwErrorMeansNoDriver(GLFW_VERSION_UNAVAILABLE));
        CHECK(glfwErrorMeansNoDriver(GLFW_FORMAT_UNAVAILABLE));
        CHECK(!glfwErrorMeansNoDriver(GLFW_PLATFORM_ERROR));
        CHECK(!glfwErrorMeansNoDriver(GLFW_NOT_INITIALIZED));
        CHECK(!glfwErrorMeansNoDriver(GLFW_INVALID_VALUE));
        CHECK(!glfwErrorMeansNoDriver(0));

        // FOXSDR_GL: three words, any case, blanks trimmed; anything else ignored and said so.
        CHECK(!parseGlRequest(nullptr).set && parseGlRequest(nullptr).mode == GlMode::Auto);
        CHECK(!parseGlRequest("").set && !parseGlRequest("  \t").set);
        CHECK(parseGlRequest("software").mode == GlMode::Software && parseGlRequest("software").valid);
        CHECK(parseGlRequest(" SoftWare ").mode == GlMode::Software);
        CHECK(parseGlRequest("hardware").mode == GlMode::Hardware);
        CHECK(parseGlRequest("auto").mode == GlMode::Auto && parseGlRequest("auto").set);
        {
            const GlRequest bad = parseGlRequest("llvmpipe");
            CHECK(bad.set && !bad.valid && bad.mode == GlMode::Auto && bad.text == "llvmpipe");
        }
        {
            const GlRequest junk = parseGlRequest("soft\x01ware\xc3\xa9 and much more than twenty-four");
            CHECK(!junk.valid && printable(junk.text) && junk.text.size() <= 24);
        }
        CHECK(glRequestLine(parseGlRequest(nullptr), true).empty());
        CHECK(glRequestLine(parseGlRequest("software"), true) ==
              "window: FOXSDR_GL=software; the software renderer is used from the start");
        CHECK(glRequestLine(parseGlRequest("hardware"), true) ==
              "window: FOXSDR_GL=hardware; the graphics driver only, with no software fallback");
        CHECK(glRequestLine(parseGlRequest("auto"), true) ==
              "window: FOXSDR_GL=auto; the software renderer only if the graphics driver cannot be used");
        CHECK(glRequestLine(parseGlRequest("llvmpipe"), true) ==
              "window: FOXSDR_GL=llvmpipe is not auto, software or hardware; ignored");
        CHECK(contains(glRequestLine(parseGlRequest("software"), false), "LIBGL_ALWAYS_SOFTWARE=1"));
        CHECK(contains(glRequestLine(parseGlRequest("software"), false), "; ignored"));
        for (const char* v : {"software", "hardware", "auto", "llvmpipe", "\x01\x02 x"}) {
            checkLine(glRequestLine(parseGlRequest(v), true));
            checkLine(glRequestLine(parseGlRequest(v), false));
        }

        // GL_VERSION as drivers write it, and what is too old for "#version 130".
        {
            const GlVersion nv = parseGlVersion("4.6.0 NVIDIA 610.88");
            CHECK(nv.parsed && nv.major == 4 && nv.minor == 6 && !glVersionTooOld(nv));
            const GlVersion mesa = parseGlVersion("4.6 (Compatibility Profile) Mesa 26.2.4 (git-9a0169f2a7)");
            CHECK(mesa.parsed && mesa.major == 4 && mesa.minor == 6);
            const GlVersion gdi = parseGlVersion("1.1.0");
            CHECK(gdi.parsed && gdi.major == 1 && gdi.minor == 1 && glVersionTooOld(gdi));
            CHECK(glVersionTooOld(parseGlVersion("2.1 Mesa 7.0")));
            CHECK(glVersionTooOld(parseGlVersion("2.99")));
            // the boundary: exactly 3.0 is enough
            CHECK(!glVersionTooOld(parseGlVersion("3.0")));
            CHECK(!glVersionTooOld(parseGlVersion("3.0 - Build 9.17.10.4459")));
            CHECK(!glVersionTooOld(parseGlVersion("10.0")));
            // unreadable is never "old": the start-up goes on as before
            for (const char* v : {static_cast<const char*>(nullptr), "", "OpenGL ES 3.2", "4", "4.", ".6", "x4.6"}) {
                const GlVersion u = parseGlVersion(v);
                CHECK(!u.parsed && !glVersionTooOld(u));
            }
            // a number too long for an int is clamped, not overflowed
            CHECK(!glVersionTooOld(parseGlVersion("99999999999999999999.1")));
            CHECK(oldDriverLine(gdi) == "window: the graphics driver offers OpenGL 1.1; FoxSDR needs 3.0");
            CHECK(oldDriverCause(gdi) == "the graphics driver offers OpenGL 1.1; FoxSDR needs 3.0");
            checkLine(oldDriverLine(gdi));
        }

        // Whether there is a second attempt, and the line when there is not.
        CHECK(softwareSkip(GlMode::Auto, false, true, true) == SoftwareSkip::None);
        CHECK(softwareSkip(GlMode::Hardware, false, true, true) == SoftwareSkip::Hardware);
        CHECK(softwareSkip(GlMode::Auto, true, true, true) == SoftwareSkip::AlreadyTried);
        CHECK(softwareSkip(GlMode::Software, true, true, true) == SoftwareSkip::AlreadyTried);
        CHECK(softwareSkip(GlMode::Auto, false, false, true) == SoftwareSkip::Missing);
        CHECK(softwareSkip(GlMode::Auto, false, true, false) == SoftwareSkip::NotThisPlatform);
        // FOXSDR_GL=hardware wins over a renderer that is there, and is what the line says
        CHECK(softwareSkip(GlMode::Hardware, true, false, true) == SoftwareSkip::Hardware);
        CHECK(softwareSkipLine(SoftwareSkip::None).empty());
        CHECK(softwareSkipLine(SoftwareSkip::NotThisPlatform).empty());
        for (const SoftwareSkip s : {SoftwareSkip::Hardware, SoftwareSkip::AlreadyTried, SoftwareSkip::Missing}) {
            CHECK(contains(softwareSkipLine(s), "window: no software fallback: "));
            checkLine(softwareSkipLine(s));
        }
        CHECK(contains(softwareSkipLine(SoftwareSkip::Missing), "softgl\\opengl32.dll"));
        CHECK(softwareRetryLine(noDriverCause(GLFW_API_UNAVAILABLE)) ==
              "window: the graphics driver offers no usable OpenGL (GLFW error 65542); starting again with the "
              "software renderer in softgl");
        checkLine(softwareRetryLine(noDriverCause(GLFW_FORMAT_UNAVAILABLE)));
        checkLine(softwareRetryLine(oldDriverCause(parseGlVersion("1.1.0"))));

        // The test seam's words; anything else is no seam at all.
        CHECK(parseTestWindowFail(nullptr) == TestWindowFail::None);
        CHECK(parseTestWindowFail("") == TestWindowFail::None);
        CHECK(parseTestWindowFail("1") == TestWindowFail::All);
        CHECK(parseTestWindowFail("hardware") == TestWindowFail::Hardware);
        CHECK(parseTestWindowFail("old-driver") == TestWindowFail::OldDriver);
        CHECK(parseTestWindowFail("imgui") == TestWindowFail::ImGui);
        CHECK(parseTestWindowFail("0") == TestWindowFail::None);
        CHECK(parseTestWindowFail("yes") == TestWindowFail::None);
    }

    // =======================================================================
    // 5. The line for the request: said in every case, and ignored values said to be ignored
    // =======================================================================
    {
        CHECK(requestLine(parseRequest(nullptr), true).empty());  // nothing asked: nothing said
        CHECK(requestLine(parseRequest(""), true).empty());

        const std::string bad = requestLine(parseRequest("bad"), true);
        checkLine(bad);
        CHECK(contains(bad, "FOXSDR_WINDOW_PLATFORM=bad") && contains(bad, "ignored") && contains(bad, "x11, wayland or any"));

        const std::string any = requestLine(parseRequest("any"), true);
        checkLine(any);
        CHECK(contains(any, "FOXSDR_WINDOW_PLATFORM=any") && contains(any, "GLFW chooses"));

        const std::string x11 = requestLine(parseRequest("x11"), true);
        checkLine(x11);
        CHECK(contains(x11, "FOXSDR_WINDOW_PLATFORM=x11") && contains(x11, "asking GLFW for the x11 platform"));
        CHECK(!contains(x11, "ignored"));

        // A platform this build of GLFW does not have (x11 on Windows): ignored, and it says why.
        const std::string missing = requestLine(parseRequest("x11"), false);
        checkLine(missing);
        CHECK(contains(missing, "FOXSDR_WINDOW_PLATFORM=x11") && contains(missing, "does not support") &&
              contains(missing, "ignored"));
        const std::string missingWl = requestLine(parseRequest("wayland"), false);
        CHECK(contains(missingWl, "wayland") && contains(missingWl, "ignored"));

        // Hostile text typed into the variable stays one plain line.
        checkLine(requestLine(parseRequest("bad\n\x01 and 'quotes' and a very long tail that never ends and never ends"), true));
        CHECK(!contains(requestLine(parseRequest("a'b"), true), "'"));
    }

    // =======================================================================
    // 6. The GL strings, and GLFW's own error text
    // =======================================================================
    {
        const std::string gl = glLine("Intel", "Mesa Intel(R) UHD Graphics 620 (KBL GT2)",
                                      "4.6 (Compatibility Profile) Mesa 24.0.5-1ubuntu1");
        checkLine(gl);
        CHECK(contains(gl, "GL vendor Intel") && contains(gl, "renderer Mesa Intel(R) UHD Graphics 620 (KBL GT2)") &&
              contains(gl, "version 4.6 (Compatibility Profile) Mesa 24.0.5-1ubuntu1"));
        CHECK(!contains(gl, "'"));  // an upload masks a single-quoted span, which would eat the string

        // No context: glGetString answers null.
        const std::string none = glLine(nullptr, nullptr, nullptr);
        checkLine(none);
        CHECK(contains(none, "vendor (none)") && contains(none, "renderer (none)") && contains(none, "version (none)"));

        // Absurd strings are cut, not the line.
        const std::string huge(400, 'g');
        const std::string cut = glLine(huge.c_str(), huge.c_str(), huge.c_str());
        checkLine(cut);
        CHECK(contains(cut, "vendor g") && contains(cut, "version g"));
        checkLine(glLine("a'b\x01", "\xff", "x\ny"));

        // GLFW's error text, in the log as well as on stderr.
        const std::string plain = glfwErrorLine(65544, "Win32: Failed to query display settings");
        checkLine(plain);
        CHECK(contains(plain, "GLFW error 65544: Win32: Failed to query display settings"));
        const std::string nodesc = glfwErrorLine(65537, nullptr);
        checkLine(nodesc);
        CHECK(contains(nodesc, "GLFW error 65537: (no description)"));

        // "X11: Failed to open display <DISPLAY>" names the display it could not open. The
        // name is NOT logged, whatever it was: it can be a host.
        for (const char* name : {":0", "localhost:10.0", "build-host.example.invalid:12.0", ":0.0", "unix:0"}) {
            const std::string text = std::string("X11: Failed to open display ") + name;
            const std::string line = glfwErrorLine(65544, text.c_str());
            checkLine(line);
            CHECK(contains(line, "GLFW error 65544: X11: Failed to open display"));
            CHECK(contains(line, "(name not logged)"));
            CHECK(!contains(line, name));
        }
        // Other X11 and Wayland errors are kept whole.
        const std::string miss = glfwErrorLine(65544, "X11: The DISPLAY environment variable is missing");
        CHECK(contains(miss, "X11: The DISPLAY environment variable is missing"));
        CHECK(contains(glfwErrorLine(65544, "Wayland: Failed to connect to display"), "Wayland: Failed to connect to display"));
        CHECK(contains(glfwErrorLine(65543, "GLX: Failed to create context: BadMatch"), "GLX: Failed to create context: BadMatch"));

        const std::string longText(1000, 'e');
        checkLine(glfwErrorLine(65544, longText.c_str()));
        checkLine(glfwErrorLine(65544, "tab\there\nnewline \xc3\xa9 'q'"));
        CHECK(!contains(glfwErrorLine(65544, "it's 'quoted'"), "'"));
    }

    // =======================================================================
    // 7. ON DISK when diagLogf returns: a death right after a line must not lose it
    //    (core/diag_log.cpp flushes every line; the whole point of these lines is the
    //    run that does not reach a clean exit)
    // =======================================================================
    {
        const fs::path dir = scratchDir("flush");
        cascade::core::DiagLog& log = cascade::core::DiagLog::instance();
        log.configure(dir.string(), true);
        const std::vector<std::string> both = {"wayland", "x11"};
        const std::string first = environmentLine("3.4.0", both, ":0", nullptr, "x11");
        cascade::core::diagLogf("%s", first.c_str());
        // Read with the log still open and nothing closed or flushed by this test.
        const std::string afterFirst = readFile(dir / "foxsdr.log");
        CHECK(contains(afterFirst, first));
        cascade::core::diagLogf("%s", glfwErrorLine(65544, "X11: Failed to open display :0").c_str());
        const std::string afterSecond = readFile(dir / "foxsdr.log");
        CHECK(contains(afterSecond, "window: GLFW error 65544: X11: Failed to open display (name not logged)"));
        CHECK(afterSecond.find(first) < afterSecond.find("GLFW error 65544"));
        // Neither line was cut by the log's width.
        CHECK(contains(afterSecond, "XDG_SESSION_TYPE x11"));
        log.resetForTest();
        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    return testSummary("test_window_platform");
}
