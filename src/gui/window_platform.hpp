// window_platform.hpp - what the start-up of the window says about itself, and the one
// switch that picks the window system (0.99.78).
//
// THE FIELD REPORT. FoxSDR 0.99.75 on Linux (Ubuntu 24.04, an AppImage) ended in the phase
// "creating the window" and the report could not say which window system GLFW had chosen or
// why it gave up: GLFW's error text went to stderr, which an AppImage does not keep, and the
// two messages `glfwInit failed` / `glfwCreateWindow failed` went there too. Everything below
// is for the diagnostics LOG, one line per step, each beginning `window:` so that a report
// can ask a user to send exactly those lines. Every line is flushed to disk by diagLogf
// before it returns (core/diag_log.cpp), so a death right after one cannot lose it.
//
// WHAT IS NEVER LOGGED: the VALUE of DISPLAY or WAYLAND_DISPLAY (an SSH-forwarded DISPLAY
// names a host, a Wayland socket's name is the user's session), nor the name in GLFW's
// "X11: Failed to open display <name>". Presence is logged, and XDG_SESSION_TYPE only when
// it is one of the five values the specification gives it.
//
// FOXSDR_WINDOW_PLATFORM=x11|wayland|any picks the window system before glfwInit
// (glfwInitHint(GLFW_PLATFORM, ...)): the workaround for a machine on which GLFW's own
// choice (Wayland first when the session says so) does not work. It is an environment
// variable and not a setting on purpose: it is needed before there is a window to set it in.
// A value that names no platform, or a platform this build of GLFW does not have, is logged
// and ignored, and the start-up goes on with GLFW's choice.
//
// This header is pure (no GLFW include, no environment read) so that a test can hold every
// line to its words and its width without a window. The numbers are GLFW 3.4's own and
// tests/test_window_platform.cpp holds them to the real macros.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_WINDOW_PLATFORM_HPP
#define CASCADE_GUI_WINDOW_PLATFORM_HPP

#include <cstddef>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace cascade::gui::windowplatform {

// The log keeps 191 characters of a line, 18 of which are its own stamp and level
// ("HH:MM:SS.mmm info "): a window: line is cut to this, so it is never cut by the log.
inline constexpr std::size_t kMaxLineChars = 170;

// GLFW 3.4's platform ids (GLFW_ANY_PLATFORM, GLFW_PLATFORM_WIN32, ...).
inline constexpr int kAny = 0x00060000;
inline constexpr int kWin32 = 0x00060001;
inline constexpr int kCocoa = 0x00060002;
inline constexpr int kWayland = 0x00060003;
inline constexpr int kX11 = 0x00060004;
inline constexpr int kNull = 0x00060005;

namespace detail {

// Printable ASCII only, cut to `maxChars`: anything else (a control character, a byte of
// UTF-8, a newline that would make two lines) is a '?'. A single quote becomes a double
// one, because an upload masks anything in single quotes (core/diag_log.hpp, rule 6) and
// would eat the string it is meant to carry.
inline std::string plain(const char* s, std::size_t maxChars) {
    std::string out;
    if (s == nullptr) { return out; }
    for (; *s != '\0' && out.size() < maxChars; ++s) {
        const unsigned char c = static_cast<unsigned char>(*s);
        if (c == '\'') {
            out.push_back('"');
        } else if (c >= 0x20 && c < 0x7F) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('?');
        }
    }
    return out;
}

inline std::string cap(std::string line) {
    if (line.size() > kMaxLineChars) { line.resize(kMaxLineChars); }
    return line;
}

inline std::string trimmed(const char* s) {
    std::string v(s != nullptr ? s : "");
    const std::size_t a = v.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) { return std::string(); }
    const std::size_t b = v.find_last_not_of(" \t\r\n");
    return v.substr(a, b - a + 1);
}

inline bool present(const char* s) { return s != nullptr && *s != '\0'; }

}  // namespace detail

// ---------------------------------------------------------------------------
// FOXSDR_WINDOW_PLATFORM
// ---------------------------------------------------------------------------
struct Request {
    int platform = kAny;   // what to ask GLFW for; kAny unless the value named x11 or wayland
    bool set = false;      // the variable was there and held more than blanks
    bool valid = true;     // false: it named no platform (then `platform` is kAny)
    std::string text;      // the value as logged: the word when valid, else plain ASCII, cut
};

inline Request parseRequest(const char* value) {
    Request r;
    const std::string v = detail::trimmed(value);
    if (v.empty()) { return r; }
    r.set = true;
    std::string lower;
    for (const char c : v) { lower.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c); }
    if (lower == "x11") {
        r.platform = kX11;
        r.text = "x11";
    } else if (lower == "wayland") {
        r.platform = kWayland;
        r.text = "wayland";
    } else if (lower == "any") {
        r.text = "any";
    } else {
        r.valid = false;
        r.text = detail::plain(v.c_str(), 24);
    }
    return r;
}

// The platform as GLFW names it in its documentation; "unknown" for 0, which is what
// glfwGetPlatform answers when it could not.
inline const char* platformName(int id) {
    switch (id) {
        case kAny: return "any";
        case kWin32: return "win32";
        case kCocoa: return "cocoa";
        case kWayland: return "wayland";
        case kX11: return "x11";
        case kNull: return "null";
        default: break;
    }
    return "unknown";
}

// XDG_SESSION_TYPE from the five values the freedesktop login manager documents; anything
// else is not repeated.
inline std::string sessionTypeWord(const char* raw) {
    if (!detail::present(raw)) { return "unset"; }
    for (const char* known : {"x11", "wayland", "tty", "mir", "unspecified"}) {
        if (std::strcmp(raw, known) == 0) { return known; }
    }
    return "other";
}

// ---------------------------------------------------------------------------
// The lines
// ---------------------------------------------------------------------------

// BEFORE glfwInit: what this GLFW can do and what the session offers. The values of DISPLAY
// and WAYLAND_DISPLAY come in so that the line can say whether they are SET and no more.
inline std::string environmentLine(const char* glfwVersion, const std::vector<std::string>& supported,
                                   const char* display, const char* waylandDisplay, const char* xdgSessionType) {
    std::string s = "window: supports ";
    if (supported.empty()) {
        s += "none";
    } else {
        for (std::size_t i = 0; i < supported.size(); ++i) {
            if (i != 0) { s += ' '; }
            s += detail::plain(supported[i].c_str(), 10);
        }
    }
    s += detail::present(display) ? "; DISPLAY set" : "; DISPLAY not set";
    s += detail::present(waylandDisplay) ? "; WAYLAND_DISPLAY set" : "; WAYLAND_DISPLAY not set";
    s += "; XDG_SESSION_TYPE " + sessionTypeWord(xdgSessionType);
    // Last, so that a long version string (it lists the backends GLFW was built with) is what
    // a cut takes, not the session.
    s += "; GLFW " + (glfwVersion != nullptr ? detail::plain(glfwVersion, 64) : std::string("(unknown)"));
    return detail::cap(std::move(s));
}

// BEFORE glfwInit, when FOXSDR_WINDOW_PLATFORM was set: what was asked and what became of it.
// `supported` is glfwPlatformSupported for the platform asked for. Empty when nothing was asked.
inline std::string requestLine(const Request& r, bool supported) {
    if (!r.set) { return std::string(); }
    std::string s = "window: FOXSDR_WINDOW_PLATFORM=" + r.text;
    if (!r.valid) {
        s += " is not x11, wayland or any; ignored";
    } else if (r.platform == kAny) {
        s += "; GLFW chooses the platform";
    } else if (!supported) {
        s += " asked for, but this build of GLFW does not support it; ignored";
    } else {
        s += std::string("; asking GLFW for the ") + platformName(r.platform) + " platform";
    }
    return detail::cap(std::move(s));
}

// AFTER the context is current: the driver's own words, none when there is no context.
inline std::string glLine(const char* vendor, const char* renderer, const char* version) {
    auto field = [](const char* s, std::size_t maxChars) {
        const std::string t = detail::plain(s, maxChars);
        return t.empty() ? std::string("(none)") : t;
    };
    return detail::cap("window: GL vendor " + field(vendor, 20) + "; renderer " + field(renderer, 50) + "; version " +
                       field(version, 56));
}

// GLFW's own error text, which reaches the log as well as stderr. The one message that carries
// something of the machine - "X11: Failed to open display <DISPLAY>" - keeps its words and
// loses the name.
inline std::string glfwErrorLine(int code, const char* description) {
    std::string text = description != nullptr ? detail::plain(description, 400) : std::string("(no description)");
    static const char kOpen[] = "open display";
    const std::size_t at = text.find(kOpen);
    if (at != std::string::npos) { text = text.substr(0, at + sizeof(kOpen) - 1) + " (name not logged)"; }
    return detail::cap("window: GLFW error " + std::to_string(code) + ": " + text);
}

// ---------------------------------------------------------------------------
// WHEN THE WINDOW CANNOT BE CREATED, the person is told (field signature 85FF854AA2C772F9)
// ---------------------------------------------------------------------------
// The field cause was "WGL: The driver does not appear to support OpenGL" (GLFW error 65542) and
// then glfwCreateWindow returning NULL: the program ended with no window and no word to the person,
// who saw it vanish. On Windows a message box says why and where the log is; on Linux the same text
// goes to stderr, where the existing `cascade: ... failed` message already went. English only, like
// the sentinel's report text: it is shown before there is a window, a font or a language.

// Only a real session may put up a dialog. A bounded `--frames` run is a test or a script, and
// nothing may block it waiting for a click; the other tool modes (--version, --selftest, a bench)
// return before AppWindow::run and never get here.
inline bool errorDialogAllowed(int frames) { return frames < 0; }

// The log path is DiagLog::filePath(), as Settings > Diagnostics shows it ("Log: <path>"); empty
// when the file is off. `softwareTried`: the bundled software renderer was tried as well (below),
// so the person is not left thinking it was never attempted.
inline std::string failureMessage(const std::string& logPath, bool softwareTried = false) {
    std::string s =
        "FoxSDR could not create its window because no working OpenGL driver was found.\n\n"
        "Common causes: no graphics driver is installed on this computer, or this is a remote desktop "
        "session without GPU support. Installing or updating the graphics driver usually fixes it.\n\n";
    if (softwareTried) {
        s += "FoxSDR also tried its built-in software renderer, and that did not work either.\n\n";
    }
    if (logPath.empty()) {
        s += "Diagnostics are switched off, so no log was written. Switch them on in Settings > Diagnostics "
             "and start FoxSDR again to record one.";
    } else {
        s += "What happened was written to this log, which you can send to the developer:\n" + logPath;
    }
    return s;
}

// ---------------------------------------------------------------------------
// THE SOFTWARE RENDERER, and FOXSDR_GL (0.99.90)
// ---------------------------------------------------------------------------
// The same field log (0.99.89, "WGL: The driver does not appear to support OpenGL"): a Windows
// machine with no OpenGL driver at all - no graphics driver installed, a virtual machine without
// 3D, a remote session. The Windows package carries Mesa's llvmpipe, a software OpenGL 4.6, in
// softgl\ beside cascade.exe (gui/software_gl.hpp). It is NEVER the first choice: the graphics
// driver is asked first, and only these answers move the start-up over to the software renderer,
// once - by starting FoxSDR again with FOXSDR_GL=software, because the system's opengl32.dll, once
// a context has been asked of it, stays loaded in the process that asked (measured: it is still
// there after glfwTerminate) and the software renderer can only take its name in a process that
// never loaded it:
//
//   - glfwCreateWindow refused with one of the three GLFW errors that mean "this driver offers no
//     usable OpenGL": GLFW_API_UNAVAILABLE (the field case: only Windows' own GDI 1.1 renderer is
//     there), GLFW_VERSION_UNAVAILABLE, GLFW_FORMAT_UNAVAILABLE. Anything else (a window-class or
//     platform failure) is not the driver's, and a software renderer would not help;
//   - a context that works but reports an OpenGL older than 3.0, which the Dear ImGui backend's
//     "#version 130" shaders need. Before 0.99.90 that machine got as far as the backend, which
//     refused, and the process ended with one line on stderr and no word to anyone.
//
// FOXSDR_GL=auto|software|hardware: `auto` (or unset) is the above; `software` uses the software
// renderer from the start (a driver that crashes, a check on a machine whose driver works);
// `hardware` never falls back, so a failure ends as it did before. Anything else is logged and
// ignored. An environment variable and not a setting for the same reason as FOXSDR_WINDOW_PLATFORM:
// it is needed before there is a window to set it in.
inline constexpr int kApiUnavailable = 0x00010006;      // GLFW_API_UNAVAILABLE (65542)
inline constexpr int kVersionUnavailable = 0x00010007;  // GLFW_VERSION_UNAVAILABLE (65543)
inline constexpr int kFormatUnavailable = 0x00010009;   // GLFW_FORMAT_UNAVAILABLE (65545)

// What "#version 130" needs.
inline constexpr int kMinGlMajor = 3;
inline constexpr int kMinGlMinor = 0;

enum class GlMode { Auto, Software, Hardware };

struct GlRequest {
    GlMode mode = GlMode::Auto;
    bool set = false;    // the variable was there and held more than blanks
    bool valid = true;   // false: it named no mode (then `mode` is Auto)
    std::string text;    // the value as logged: the word when valid, else plain ASCII, cut
};

inline GlRequest parseGlRequest(const char* value) {
    GlRequest r;
    const std::string v = detail::trimmed(value);
    if (v.empty()) { return r; }
    r.set = true;
    std::string lower;
    for (const char c : v) { lower.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c); }
    if (lower == "auto") {
        r.text = "auto";
    } else if (lower == "software") {
        r.mode = GlMode::Software;
        r.text = "software";
    } else if (lower == "hardware") {
        r.mode = GlMode::Hardware;
        r.text = "hardware";
    } else {
        r.valid = false;
        r.text = detail::plain(v.c_str(), 24);
    }
    return r;
}

// BEFORE glfwInit, when FOXSDR_GL was set. `windows`: the software renderer exists only in the
// Windows package; elsewhere Mesa's own LIBGL_ALWAYS_SOFTWARE=1 is the switch. Empty when unset.
inline std::string glRequestLine(const GlRequest& r, bool windows) {
    if (!r.set) { return std::string(); }
    std::string s = "window: FOXSDR_GL=" + r.text;
    if (!r.valid) {
        s += " is not auto, software or hardware; ignored";
    } else if (!windows && r.mode == GlMode::Software) {
        s += " is for Windows; on this system Mesa's LIBGL_ALWAYS_SOFTWARE=1 does it; ignored";
    } else if (r.mode == GlMode::Software) {
        s += "; the software renderer is used from the start";
    } else if (r.mode == GlMode::Hardware) {
        s += "; the graphics driver only, with no software fallback";
    } else {
        s += "; the software renderer only if the graphics driver cannot be used";
    }
    return detail::cap(std::move(s));
}

// The GLFW errors after which a software renderer can help: the driver's OpenGL is missing or
// not good enough, as opposed to the window system refusing a window.
inline bool glfwErrorMeansNoDriver(int code) {
    return code == kApiUnavailable || code == kVersionUnavailable || code == kFormatUnavailable;
}

// GL_VERSION's leading "<major>.<minor>" ("4.6.0 NVIDIA 610.88", "2.1 Mesa 26.2.4", "1.1.0").
// `parsed` is false for anything that does not start that way - and an unreadable version is
// never called old: the start-up then goes on as it always has.
struct GlVersion {
    int major = 0;
    int minor = 0;
    bool parsed = false;
};

inline GlVersion parseGlVersion(const char* s) {
    GlVersion v;
    if (s == nullptr) { return v; }
    auto number = [&s](int& out) {
        if (*s < '0' || *s > '9') { return false; }
        long n = 0;
        while (*s >= '0' && *s <= '9') {
            if (n < 100000) { n = n * 10 + (*s - '0'); }
            ++s;
        }
        out = static_cast<int>(n);
        return true;
    };
    if (!number(v.major) || *s != '.') { return v; }
    ++s;
    if (!number(v.minor)) { return v; }
    v.parsed = true;
    return v;
}

inline bool glVersionTooOld(const GlVersion& v) {
    return v.parsed && (v.major < kMinGlMajor || (v.major == kMinGlMajor && v.minor < kMinGlMinor));
}

// What a too-old driver did, in a few words (the cause softwareRetryLine names) ...
inline std::string oldDriverCause(const GlVersion& v) {
    return "the graphics driver offers OpenGL " + std::to_string(v.major) + "." + std::to_string(v.minor) +
           "; FoxSDR needs " + std::to_string(kMinGlMajor) + "." + std::to_string(kMinGlMinor);
}

// ... and the line said right after the GL line when the version is too old.
inline std::string oldDriverLine(const GlVersion& v) { return detail::cap("window: " + oldDriverCause(v)); }

// Why there is no second attempt with the software renderer, when there is not.
enum class SoftwareSkip {
    None,            // there is one
    Hardware,        // FOXSDR_GL=hardware
    AlreadyTried,    // the software renderer was in use, or was tried, already
    Missing,         // no softgl\opengl32.dll and libgallium_wgl.dll beside the application
    NotThisPlatform  // the software renderer is part of the Windows package only
};

inline SoftwareSkip softwareSkip(GlMode mode, bool alreadyTried, bool present, bool windows) {
    if (!windows) { return SoftwareSkip::NotThisPlatform; }
    if (mode == GlMode::Hardware) { return SoftwareSkip::Hardware; }
    if (alreadyTried) { return SoftwareSkip::AlreadyTried; }
    if (!present) { return SoftwareSkip::Missing; }
    return SoftwareSkip::None;
}

// The line for a skip; empty for None and for NotThisPlatform (nothing to say off Windows).
inline std::string softwareSkipLine(SoftwareSkip why) {
    switch (why) {
        case SoftwareSkip::Hardware:
            return "window: no software fallback: FOXSDR_GL=hardware";
        case SoftwareSkip::AlreadyTried:
            return "window: no software fallback: the software renderer was already tried";
        case SoftwareSkip::Missing:
            return "window: no software fallback: softgl\\opengl32.dll and libgallium_wgl.dll are not beside "
                   "the application";
        case SoftwareSkip::None:
        case SoftwareSkip::NotThisPlatform:
            break;
    }
    return std::string();
}

// The line that announces the second attempt. `cause` is what the driver did, in a few words.
inline std::string softwareRetryLine(const std::string& cause) {
    return detail::cap("window: " + cause + "; starting again with the software renderer in softgl");
}

// The cause for a refused window, from GLFW's error code.
inline std::string noDriverCause(int code) {
    return "the graphics driver offers no usable OpenGL (GLFW error " + std::to_string(code) + ")";
}

// FOXSDR_TEST_WINDOW_FAIL, the seam a bounded run uses to reach these paths for real on a machine
// whose driver works (honoured only with --frames, like every seam in AppWindow::run):
//   1           every window is refused (an OpenGL version no driver has is asked for);
//   hardware    the window is refused while the graphics driver is in use, and not once the
//               software renderer is - the field case, with the real fallback after it;
//   old-driver  the graphics driver's context is taken to report OpenGL 2.1;
//   imgui       the Dear ImGui OpenGL 3 backend is taken to have refused.
enum class TestWindowFail { None, All, Hardware, OldDriver, ImGui };

inline TestWindowFail parseTestWindowFail(const char* value) {
    const std::string v = detail::trimmed(value);
    if (v == "1") { return TestWindowFail::All; }
    if (v == "hardware") { return TestWindowFail::Hardware; }
    if (v == "old-driver") { return TestWindowFail::OldDriver; }
    if (v == "imgui") { return TestWindowFail::ImGui; }
    return TestWindowFail::None;
}

}  // namespace cascade::gui::windowplatform

#endif  // CASCADE_GUI_WINDOW_PLATFORM_HPP
