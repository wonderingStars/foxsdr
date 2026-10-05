// THE SOUND PATH IN THE COPY-DIAGNOSTICS BUNDLE, THROUGH THE REAL AppWindow.
//
// test_diagnostics proves the bundle PRINTS what it is handed (every state of
// the output, who muted it, the squelch) and test_audio_open_log proves the
// sink SAYS when it opened. This proves the application hands the bundle the
// right facts: that the numbers in "volume:", "audio-muted:" and "squelch:" are
// the window's own volume, mute and squelch, and that the two places that build
// a report context - the once-a-second refresh and the bundle - cannot
// disagree about the sound.
//
// THE FIELD REPORT. 0.99.58, an NESDR SMArt v5, "no audio from my speakers":
// a bundle with the source, the rate and the radio and NOTHING about the
// speakers, so the first question of any support conversation - is the output
// open, is it muted, is the squelch shut, is the volume up - could only be
// asked of the reporter.
//
// Hermetic like test_ppm_app: no config file, the per-user directories in a
// scratch folder. The audio output is whatever this machine has (a build agent
// has none), so the output line is checked for being SOME real state, never for
// a particular one.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/diag_report.hpp"
#include "gui/app_window.hpp"
#include "test_check.hpp"

namespace {

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    const std::filesystem::path scratch =
        std::filesystem::temp_directory_path() / ("foxsdr_diag_audio_app_" + std::to_string(pid));
    std::filesystem::create_directories(scratch);
    const std::string s = scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, s);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
}

}  // namespace

namespace cascade::gui {

struct AppWindowTestAccess {
    static std::string bundle(AppWindow& a) { return a.currentDiagnosticsBundle(); }
    static void setVolume(AppWindow& a, float v) { a.volume_ = v; }
    static void setUserMuted(AppWindow& a, bool m) { a.userMuted_ = m; }
    static void setSquelch(AppWindow& a, float db) {
        a.squelchDb_ = db;
        a.pipeline_.setSquelchDb(db);
    }
    static void setPluginMute(AppWindow& a, const std::vector<std::string>& names) {
        a.mutedBy_ = names;
    }
    static void setPpmOn(AppWindow& a, bool on) { a.ppmCorrectionOn_ = on; }
    // The context the ONCE-A-SECOND REFRESH renders - the one a crash report
    // would carry - read back before anything else overwrites it.
    static std::string refreshedContext(AppWindow& a) {
        a.refreshDiagContext();
        return cascade::core::diagContextBlock();
    }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

namespace {

// The value of a "name: value" header line, or "(missing)".
std::string field(const std::string& bundle, const char* name) {
    const std::string key = std::string("\n") + name + ": ";
    const std::size_t at = bundle.find(key);
    if (at == std::string::npos) { return "(missing)"; }
    const std::size_t from = at + key.size();
    const std::size_t eol = bundle.find('\n', from);
    return bundle.substr(from, eol == std::string::npos ? std::string::npos : eol - from);
}

bool startsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

}  // namespace

int main() {
    isolate();
    cascade::gui::AppWindow app;

    // --- The output: some real state, never absent --------------------------
    {
        const std::string b = Access::bundle(app);
        const std::string out = field(b, "audio-output");
        std::printf("audio-output: %s\n", out.c_str());
        CHECK(startsWith(out, "open, ") || startsWith(out, "none - ") ||
              startsWith(out, "stopped - ") || out == "opening");
        // An open stream names its driver model and layout, never the device.
        if (startsWith(out, "open, ")) {
            CHECK(out.find("channel") != std::string::npos);
        }
    }

    // --- The volume, the mute and the squelch are the window's own ----------
    {
        Access::setVolume(app, 0.25f);
        Access::setUserMuted(app, false);
        Access::setSquelch(app, -40.0f);
        const std::string b = Access::bundle(app);
        CHECK(field(b, "volume") == "25%");
        CHECK(field(b, "audio-muted") == "no");
        // The threshold is the one set; the gate and the level are whatever the
        // idle generator makes of it, so only the threshold and the shape are
        // pinned here (test_pipeline_device_rate_audio pins the gate itself).
        const std::string sq = field(b, "squelch");
        std::printf("squelch: %s\n", sq.c_str());
        CHECK(startsWith(sq, "-40 dB, open") || startsWith(sq, "-40 dB, closed"));
    }
    {
        Access::setVolume(app, 0.0f);
        Access::setUserMuted(app, true);
        const std::string b = Access::bundle(app);
        CHECK(field(b, "volume") == "0%");
        CHECK(field(b, "audio-muted") == "you");
    }

    // --- A muting plugin is DESCRIBED, never named --------------------------
    // Its name would say which band the receiver is parked on.
    {
        Access::setUserMuted(app, false);
        Access::setPluginMute(app, {"Zzyzx Decoder"});
        const std::string b = Access::bundle(app);
        CHECK(field(b, "audio-muted") == "a decoder plugin");
        CHECK(b.find("Zzyzx") == std::string::npos);
        Access::setUserMuted(app, true);
        CHECK(field(Access::bundle(app), "audio-muted") == "you + a decoder plugin");
        Access::setPluginMute(app, {});
    }

    // --- The refresh and the bundle agree ------------------------------------
    // refreshDiagContext() writes the context a crash report carries; the
    // bundle re-renders it from its own input, in a second place. Every line
    // they share has to read the same, or a crash report and a bundle filed by
    // the same session would describe two different applications.
    //
    // THE CRYSTAL CORRECTION IS HERE TOO, because it is the line that did NOT
    // agree: the bundle's input never set `ppm`, so buildDiagnosticsBundle
    // re-rendered the default - "off" - over the correct line the refresh had
    // just written, whatever the setting was. With the switch on and the
    // generator installed (no radio to correct) the true answer is "not
    // applicable".
    {
        Access::setVolume(app, 0.6f);
        Access::setUserMuted(app, true);
        Access::setSquelch(app, -33.0f);
        Access::setPpmOn(app, true);
        const std::string ctx = Access::refreshedContext(app);
        const std::string b = Access::bundle(app);
        for (const char* name :
             {"audio-output", "volume", "audio-muted", "squelch", "ppm", "mode", "source",
              "sample-rate", "device-open", "sdr-model"}) {
            const std::string fromRefresh = field("\n" + ctx, name);
            const std::string fromBundle = field(b, name);
            std::printf("  %-13s refresh=%s bundle=%s\n", name, fromRefresh.c_str(),
                        fromBundle.c_str());
            CHECK(fromRefresh != "(missing)");
            CHECK(fromRefresh == fromBundle);
        }
        CHECK(field(b, "volume") == "60%");
        CHECK(field(b, "ppm") == "not applicable");
    }

    return testSummary("test_diag_audio_app");
}
