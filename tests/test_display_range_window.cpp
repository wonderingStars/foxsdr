// test_display_range_window.cpp - THE WINDOW'S HALF OF THE SPECTRUM'S TRACE AND THE DISPLAY RANGE (stub campaign batch B11): what leaves the real AppWindow in the config it
// saves and in the commands it sends, with no window and no ImGui frame.
//
//   trace          the mode (Normal / Peak hold / Average) and the average's length, restored from the file and written back;
//   range          the person's choice (Automatic or Manual) and the pair Manual returns to, restored, written back as waterfallRangeMode / dbMin / dbMax, and sent to the
//                  engine as ONE FOXAPI_OP_SET_DISPLAY_RANGE carrying the window IN FORCE (the standard window under Automatic, the pair under Manual);
//   the ten dB     the ceiling stays 10 dB above the floor and the end that moved gives way;
//   Reset          Automatic, the pair forgotten, the trace Normal;
//   somebody else  a range the ENGINE shows that nobody here asked for - the web remote's - is taken as the person's choice of Manual (master's onDisplayRange did); a state
//                  that has not caught up with what the window just sent is NOT somebody else's change; and a command the state never shows (clamped, or held for two seconds)
//                  leaves the window agreeing with the state, which is what the picture is drawn through.
//
// WHY ITS OWN TEST. The window keeps `loadedCfg_` and currentConfig() starts from it, writing back only the fields the window owns: a field the window forgets to write is
// silently kept at the value the file had and never changes (tests/test_window_chrome_config.cpp's argument). tests/test_display_menu_app.cpp and
// tests/test_spectrum_trace_app.cpp run the real application and read the config it saved, so they hold the round trip; this holds each rule in a line, and the part of
// the follow that a run of the application cannot reach (a second writer of the range).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/app_commands.hpp"
#include "core/config.hpp"
#include "core/display_range.hpp"
#include "core/trace_hold.hpp"
#include "gui/app_window.hpp"
#include "test_host.hpp"
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

std::filesystem::path g_scratch;

// ISOLATED FROM THE REAL CONFIG, the rule every *_app test follows.
void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = std::filesystem::temp_directory_path() / ("foxsdr_display_range_" + std::to_string(pid));
    std::filesystem::create_directories(g_scratch);
    const std::string s = g_scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) { setEnv(v, s); }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_CRASH_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_UPDATE_URL", "http://127.0.0.1:9");
}

double g_clockS = 1000.0;
double fakeClock() { return g_clockS; }

}  // namespace

namespace cascade::gui {

struct AppWindowTestAccess {
    using Sent = std::vector<cascade::core::cmd::QueuedCommand>;
    static cascade::core::AppConfig config(AppWindow& a) { return a.currentConfig(); }
    static void restore(AppWindow& a, const cascade::core::AppConfig& c) { a.applyConfig(c); }
    static void tap(AppWindow& a, Sent* log) { a.uiSubmitTap_ = log; }
    static void clock(AppWindow& a) { a.uiClockS_ = &fakeClock; }
    static cascade::core::TraceMode trace(const AppWindow& a) { return a.spectrumTraceMode_; }
    static double averageMs(const AppWindow& a) { return a.spectrumAverageMs_; }
    static const cascade::core::DisplayRange& range(const AppWindow& a) { return a.displayRange_; }
    static bool pending(const AppWindow& a) { return a.displaySent_.pending; }
    static void setState(AppWindow& a, double lo, double hi) {
        a.uiState_.dbMin = lo;
        a.uiState_.dbMax = hi;
    }
    static void follow(AppWindow& a) { a.followDisplayRange(); }
    static void setTrace(AppWindow& a, cascade::core::TraceMode m) { a.setSpectrumTraceMode(m); }
    static void setAverage(AppWindow& a, double ms) { a.setSpectrumAverageMs(ms); }
    static void setMode(AppWindow& a, cascade::core::RangeMode m) { a.setRangeMode(m); }
    static void setManual(AppWindow& a, float lo, float hi, bool floorMoved) { a.setManualRange(lo, hi, floorMoved); }
    static void reset(AppWindow& a) { a.resetDisplay(); }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;
namespace cc = cascade::core;

namespace {

// The display-range commands sent since `from`.
std::vector<cc::cmd::QueuedCommand> rangeCommands(const Access::Sent& log, std::size_t from) {
    std::vector<cc::cmd::QueuedCommand> out;
    for (std::size_t i = from; i < log.size(); ++i) {
        const std::uint32_t op = log[i].c.op;
        if (op == FOXAPI_OP_SET_DISPLAY_RANGE || op == FOXAPP_OP_SET_DISPLAY_MIN || op == FOXAPP_OP_SET_DISPLAY_MAX) { out.push_back(log[i]); }
    }
    return out;
}

}  // namespace

int main() {
    std::printf("test_display_range_window\n");
    isolate();
    namespace g = cascade::gui;

    // A FRESH WINDOW: Normal, a second of average, the standard window, Automatic.
    {
        th::TestWindow app;
        CHECK(Access::trace(app) == cc::TraceMode::Normal);
        CHECK(Access::averageMs(app) == cc::kTraceAverageDefaultMs);
        CHECK(!Access::range(app).manual());
        const cc::AppConfig c = Access::config(app);
        CHECK(c.spectrumTraceMode == "normal" && c.spectrumAverageMs == 1000);
        CHECK(c.waterfallRangeMode == "auto" && c.dbMin == -110.0f && c.dbMax == 0.0f);
    }

    // RESTORE, MANUAL AND AVERAGE: every field the file carries reaches its member, and the engine is told the window in force, as one command.
    cc::AppConfig file;
    file.spectrumTraceMode = "average";
    file.spectrumAverageMs = 500;
    file.waterfallRangeMode = "manual";
    file.dbMin = -60.0f;
    file.dbMax = -20.0f;
    {
        th::TestWindow app;
        Access::Sent log;
        Access::tap(app, &log);
        Access::restore(app, file);
        CHECK(Access::trace(app) == cc::TraceMode::Average);
        CHECK(Access::averageMs(app) == 500.0);
        CHECK(Access::range(app).manual());
        CHECK(Access::range(app).inForce() == (cc::DbPair{-60.0f, -20.0f}));
        const auto sent = rangeCommands(log, 0);
        CHECK(sent.size() == 1u);
        if (sent.size() == 1u) {
            CHECK(sent[0].c.op == FOXAPI_OP_SET_DISPLAY_RANGE);   // both ends at once: no order of two sliders' commands to get wrong
            CHECK(sent[0].c.num[0] == -60.0 && sent[0].c.num[1] == -20.0);
        }
        // Nothing was changed, so nothing differs on the way out.
        const cc::AppConfig out = Access::config(app);
        CHECK(out.spectrumTraceMode == "average" && out.spectrumAverageMs == 500);
        CHECK(out.waterfallRangeMode == "manual" && out.dbMin == -60.0f && out.dbMax == -20.0f);
    }

    // RESTORE, AUTOMATIC WITH A REMEMBERED PAIR: the standard window is in force, the pair is kept and saved.
    {
        cc::AppConfig autoFile = file;
        autoFile.waterfallRangeMode = "auto";
        th::TestWindow app;
        Access::Sent log;
        Access::tap(app, &log);
        Access::restore(app, autoFile);
        CHECK(!Access::range(app).manual());
        CHECK(Access::range(app).inForce() == (cc::DbPair{}));
        CHECK(Access::range(app).manualPair() == (cc::DbPair{-60.0f, -20.0f}));
        const auto sent = rangeCommands(log, 0);
        CHECK(sent.size() == 1u);
        if (sent.size() == 1u) { CHECK(sent[0].c.num[0] == -110.0 && sent[0].c.num[1] == 0.0); }   // the engine holds the pair; Automatic puts the standard window over it
        const cc::AppConfig out = Access::config(app);
        CHECK(out.waterfallRangeMode == "auto" && out.dbMin == -60.0f && out.dbMax == -20.0f);
    }

    // A FILE WITH NO MODE WORD (0.99.75's): the pair that is not the standard window was the user's, so it is Manual.
    {
        cc::AppConfig old;
        old.waterfallRangeMode = "";
        old.dbMin = -95.0f;
        old.dbMax = -20.0f;
        th::TestWindow app;
        Access::restore(app, old);
        CHECK(Access::range(app).manual());
        CHECK(Access::range(app).inForce() == (cc::DbPair{-95.0f, -20.0f}));
    }

    // SAVE: a change made AFTER the restore is what the config says, one at a time, so a field written from the wrong member cannot hide behind another.
    {
        th::TestWindow app;
        Access::Sent log;
        Access::tap(app, &log);
        Access::restore(app, file);
        log.clear();

        Access::setMode(app, cc::RangeMode::Auto);
        cc::AppConfig out = Access::config(app);
        CHECK(out.waterfallRangeMode == "auto" && out.dbMin == -60.0f && out.dbMax == -20.0f);   // Automatic keeps the pair
        CHECK(out.spectrumTraceMode == "average");
        auto sent = rangeCommands(log, 0);
        CHECK(sent.size() == 1u);
        if (sent.size() == 1u) { CHECK(sent[0].c.num[0] == -110.0 && sent[0].c.num[1] == 0.0); }

        // Manual again: the numbers the user had.
        log.clear();
        Access::setMode(app, cc::RangeMode::Manual);
        out = Access::config(app);
        CHECK(out.waterfallRangeMode == "manual" && out.dbMin == -60.0f && out.dbMax == -20.0f);
        sent = rangeCommands(log, 0);
        CHECK(sent.size() == 1u);
        if (sent.size() == 1u) { CHECK(sent[0].c.num[0] == -60.0 && sent[0].c.num[1] == -20.0); }

        // THE TEN DB: the ceiling dragged to -95 over a floor of -60 stops at -50 (the end that moved gives way) ...
        log.clear();
        Access::setManual(app, -60.0f, -95.0f, false);
        out = Access::config(app);
        CHECK(out.dbMin == -60.0f && out.dbMax == -50.0f && out.waterfallRangeMode == "manual");
        sent = rangeCommands(log, 0);
        CHECK(sent.size() == 1u);
        if (sent.size() == 1u) { CHECK(sent[0].c.num[0] == -60.0 && sent[0].c.num[1] == -50.0); }
        // ... and then the floor dragged up to -35 stops at -60 (the ceiling stays where the hand left it).
        Access::setManual(app, -35.0f, -50.0f, true);
        out = Access::config(app);
        CHECK(out.dbMin == -60.0f && out.dbMax == -50.0f);

        // CHOOSING A RANGE IS CHOOSING MANUAL: a slider moved while Automatic is in force.
        Access::setMode(app, cc::RangeMode::Auto);
        Access::setManual(app, -80.0f, -30.0f, true);
        out = Access::config(app);
        CHECK(out.waterfallRangeMode == "manual" && out.dbMin == -80.0f && out.dbMax == -30.0f);

        // THE TRACE: each field written from its own member.
        Access::setTrace(app, cc::TraceMode::Peak);
        out = Access::config(app);
        CHECK(out.spectrumTraceMode == "peak" && out.spectrumAverageMs == 500);
        Access::setAverage(app, 250.0);
        out = Access::config(app);
        CHECK(out.spectrumAverageMs == 250 && out.spectrumTraceMode == "peak");
        Access::setAverage(app, 5.0);                                 // under the shortest: clamped
        CHECK(Access::config(app).spectrumAverageMs == static_cast<int>(cc::kTraceAverageMinMs));
        Access::setAverage(app, 1.0e9);                               // over the longest: clamped
        CHECK(Access::config(app).spectrumAverageMs == static_cast<int>(cc::kTraceAverageMaxMs));
        CHECK(out.waterfallRangeMode == "manual" && out.dbMin == -80.0f && out.dbMax == -30.0f);   // the trace did not touch the range

        // RESET: Automatic, the remembered pair forgotten too, and the trace Normal.
        log.clear();
        Access::reset(app);
        out = Access::config(app);
        CHECK(out.waterfallRangeMode == "auto" && out.dbMin == -110.0f && out.dbMax == 0.0f);
        CHECK(out.spectrumTraceMode == "normal");
        CHECK(Access::trace(app) == cc::TraceMode::Normal);
        sent = rangeCommands(log, 0);
        CHECK(sent.size() == 1u);
        if (sent.size() == 1u) { CHECK(sent[0].c.num[0] == -110.0 && sent[0].c.num[1] == 0.0); }
    }

    // SOMEBODY ELSE'S RANGE. The window restores Automatic and asks the engine for the standard window; until the state shows it, a state that does not carry it is the engine
    // not having caught up, not somebody's change.
    {
        th::TestWindow app;
        Access::Sent log;
        Access::tap(app, &log);
        Access::clock(app);
        g_clockS = 1000.0;
        cc::AppConfig autoFile = file;
        autoFile.waterfallRangeMode = "auto";
        Access::restore(app, autoFile);
        CHECK(Access::pending(app));
        Access::setState(app, -60.0, -20.0);              // what the engine had from the file's pair, before the command landed
        Access::follow(app);
        CHECK(!Access::range(app).manual());                // not taken as somebody's range
        CHECK(Access::pending(app));
        Access::setState(app, -110.0, 0.0);               // landed
        Access::follow(app);
        CHECK(!Access::pending(app));
        CHECK(!Access::range(app).manual());

        // The web remote's range arrives in the state: the person's choice of Manual, to the exact pair it passed the engine's checks with.
        Access::setState(app, -80.0, -30.0);
        Access::follow(app);
        CHECK(Access::range(app).manual());
        CHECK(Access::range(app).inForce() == (cc::DbPair{-80.0f, -30.0f}));
        cc::AppConfig out = Access::config(app);
        CHECK(out.waterfallRangeMode == "manual" && out.dbMin == -80.0f && out.dbMax == -30.0f);
        // Following it again with the state unchanged changes nothing (and sends nothing).
        const std::size_t before = log.size();
        Access::follow(app);
        Access::follow(app);
        CHECK(rangeCommands(log, before).empty());

        // A range the window sends is pending until the state shows it, and the state still showing the old one is not somebody else's change.
        Access::setManual(app, -60.0f, -20.0f, true);
        CHECK(Access::pending(app));
        Access::follow(app);                                  // the state is still -80 / -30
        CHECK(Access::range(app).inForce() == (cc::DbPair{-60.0f, -20.0f}));
        CHECK(Access::pending(app));
        Access::setState(app, -60.0, -20.0);
        Access::follow(app);
        CHECK(!Access::pending(app));
        CHECK(Access::range(app).inForce() == (cc::DbPair{-60.0f, -20.0f}));

        // A command the state never shows (the engine clamped it, or the session held it): after two seconds the window agrees with the state, which is what the
        // picture is drawn through.
        Access::setManual(app, -50.0f, -10.0f, true);
        CHECK(Access::pending(app));
        g_clockS += 1.5;
        Access::follow(app);
        CHECK(Access::pending(app));                          // still waiting: a command lands at the engine's next pass
        CHECK(Access::range(app).inForce() == (cc::DbPair{-50.0f, -10.0f}));
        g_clockS += 1.0;
        Access::follow(app);
        CHECK(!Access::pending(app));
        CHECK(Access::range(app).inForce() == (cc::DbPair{-60.0f, -20.0f}));
    }

    std::error_code ec;
    std::filesystem::remove_all(g_scratch, ec);
    return testSummary("test_display_range_window");
}
