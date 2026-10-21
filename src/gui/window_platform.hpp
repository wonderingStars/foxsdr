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
// when the file is off.
inline std::string failureMessage(const std::string& logPath) {
    std::string s =
        "FoxSDR could not create its window because no working OpenGL driver was found.\n\n"
        "Common causes: no graphics driver is installed on this computer, or this is a remote desktop "
        "session without GPU support. Installing or updating the graphics driver usually fixes it.\n\n";
    if (logPath.empty()) {
        s += "Diagnostics are switched off, so no log was written. Switch them on in Settings > Diagnostics "
             "and start FoxSDR again to record one.";
    } else {
        s += "What happened was written to this log, which you can send to the developer:\n" + logPath;
    }
    return s;
}

}  // namespace cascade::gui::windowplatform

#endif  // CASCADE_GUI_WINDOW_PLATFORM_HPP
