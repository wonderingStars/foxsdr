// test_meter_style_app.cpp - the bench meter's right-click style picker in
// the REAL application (0.99.47): an Italian user on 0.99.42, "is it
// possible to customise the VU meter, choosing between 3 or 4 different VU
// meters?" - AppWindow's own fields, the debounced save noticing a pick, and
// the round trip through a saved config, all with no window, no device and
// no ImGui frame ever opened, the same way tests/test_airspy_app.cpp proves
// its own settings without a radio on the desk.
//
// WHY THE "HANDLER" IS A DIRECT FIELD ASSIGNMENT RATHER THAN A SIMULATED
// ImGui::MenuItem CLICK: AppWindow::drawMeterStyleMenu's whole effect, for
// the item the user picks, is exactly `style = st;` (app_window.cpp) - there
// is no other state to update and no ImGui frame to drive headlessly (this
// codebase has no harness for scripting a MenuItem click, by design: see
// AppWindowTestAccess in the other test_*_app.cpp files, none of which
// drives a menu either). Access::pick below performs that one assignment
// directly on the SAME field the real menu item writes, so what this test
// proves - the assignment reaches currentConfig() and the debounce notices
// it - is exactly what picking the item in the running application does.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/config.hpp"
#include "gui/app_window.hpp"
#include "gui/meter_styles_math.hpp"
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

// ISOLATED FROM THE REAL CONFIG, on the same rule every other *_app test
// follows: a headless AppWindow still reads and writes
// %APPDATA%/foxsdr/config.json unless every path it might resolve is pointed
// at a scratch directory first.
void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    const std::filesystem::path scratch =
        std::filesystem::temp_directory_path() / ("foxsdr_meter_style_app_" + std::to_string(pid));
    std::filesystem::create_directories(scratch);
    const std::string s = scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, s);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_CRASH_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_UPDATE_URL", "http://127.0.0.1:9");
}

}  // namespace

namespace cascade::gui {

struct AppWindowTestAccess {
    // THE MENU'S SELECTION HANDLER, called directly - see the file header for
    // why this is the same assignment drawMeterStyleMenu's MenuItem callback
    // performs, not a stand-in for it.
    static void pickVolume(AppWindow& a, MeterStyle st) { a.meterStyleVolume_ = st; }
    static void pickRate(AppWindow& a, MeterStyle st) { a.meterStyleRate_ = st; }
    static MeterStyle volumeStyle(AppWindow& a) { return a.meterStyleVolume_; }
    static MeterStyle rateStyle(AppWindow& a) { return a.meterStyleRate_; }
    static cascade::core::AppConfig config(AppWindow& a) { return a.currentConfig(); }
    static void restore(AppWindow& a, const cascade::core::AppConfig& c) { a.applyConfig(c); }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;
using cascade::gui::MeterStyle;

int main() {
    std::printf("test_meter_style_app\n");
    isolate();

    cascade::core::AppConfig saved;
    {
        cascade::gui::AppWindow app;

        // --- BOTH METERS OPEN ON THE FACE THEY HAVE ALWAYS HAD ------------------
        CHECK(Access::volumeStyle(app) == MeterStyle::Classic);
        CHECK(Access::rateStyle(app) == MeterStyle::Classic);
        CHECK(Access::config(app).meterStyleVolume == "classic");
        CHECK(Access::config(app).meterStyleRate == "classic");

        // --- PICKING AN ITEM SETS THE FIELD AT ONCE, AND MARKS THE CONFIG
        //     DIRTY - configsEqual is the whole of the debounced save's
        //     decision, so a pick that does not change what it reports would
        //     survive only if something else happened to save in the same
        //     session (the tunerDisplayStyle field's own reason for being in
        //     that function; see app_window.cpp's configsEqual). ---------------
        const cascade::core::AppConfig before = Access::config(app);
        Access::pickVolume(app, MeterStyle::LedLadder);
        CHECK(Access::volumeStyle(app) == MeterStyle::LedLadder);
        const cascade::core::AppConfig afterVolume = Access::config(app);
        CHECK(afterVolume.meterStyleVolume == "led");
        CHECK(afterVolume.meterStyleRate == "classic");  // untouched
        CHECK(!cascade::gui::configsEqual(before, afterVolume));

        // --- THE TWO METERS NEVER SHARE ONE: picking the RATE meter's style
        //     leaves the VOLUME meter's exactly where it was left above. ------
        Access::pickRate(app, MeterStyle::Peak);
        CHECK(Access::rateStyle(app) == MeterStyle::Peak);
        CHECK(Access::volumeStyle(app) == MeterStyle::LedLadder);  // still there
        const cascade::core::AppConfig afterBoth = Access::config(app);
        CHECK(afterBoth.meterStyleVolume == "led");
        CHECK(afterBoth.meterStyleRate == "peak");
        CHECK(!cascade::gui::configsEqual(afterVolume, afterBoth));

        // Every one of the four styles is reachable and reads back as itself.
        const MeterStyle all[] = {MeterStyle::Classic, MeterStyle::Needle,
                                  MeterStyle::LedLadder, MeterStyle::Peak};
        for (MeterStyle st : all) {
            Access::pickVolume(app, st);
            CHECK(Access::volumeStyle(app) == st);
            CHECK(Access::config(app).meterStyleVolume == cascade::gui::meterStyleName(st));
        }

        saved = Access::config(app);
    }

    // --- A LATER LAUNCH: BOTH CHOICES COME BACK, INDEPENDENTLY --------------
    {
        cascade::gui::AppWindow app;
        Access::restore(app, saved);
        CHECK(Access::volumeStyle(app) == MeterStyle::Peak);  // the loop above's last pick
        CHECK(Access::rateStyle(app) == MeterStyle::Peak);
    }

    // --- AN UNKNOWN VALUE IN A HAND-EDITED CONFIG FALLS BACK TO CLASSIC,
    //     NEVER A REFUSAL - applyConfig's own guard, independent of
    //     ConfigStore::load's (which never hands it anything else in the real
    //     application, but a hand-edited file is exactly the case this
    //     guards against reaching the painter twice removed from the file). --
    {
        cascade::core::AppConfig bogus;
        bogus.meterStyleVolume = "bogus";
        bogus.meterStyleRate = "";
        cascade::gui::AppWindow app;
        Access::restore(app, bogus);
        CHECK(Access::volumeStyle(app) == MeterStyle::Classic);
        CHECK(Access::rateStyle(app) == MeterStyle::Classic);
    }

    return testSummary("test_meter_style_app");
}
