// test_config_snapshot_app.cpp - the config's receiver half is a copy the
// engine HANDS the window (engine/stage3b-pre, docs/engine-stage3.md OPEN 2).
//
// Until this round AppWindow::currentConfig() called Engine::fillConfig itself,
// every frame, on the GUI thread - reading every receiver setting straight out
// of the engine. Once the engine runs on a control thread that is a race on
// every field. Now the engine fills the receiver half when IT pumps
// (Engine::publishConfig, from pumpAudio / pump) into a copy behind a mutex,
// and currentConfig() only reads that copy (Engine::configSnapshot).
//
//   A  a change the engine has not published yet is not in currentConfig();
//      after the engine's publish it is
//   B  one pump (Engine::pump - the frame's phases) publishes it
//   C  the save made at shutdown (saveConfigNow) carries the newest value,
//      not the last frame's
//   E  the STATUS LINES are handed over the same way (OPEN 3): every one of
//      the fourteen reaches Engine::statusText(), and only once the engine
//      publishes - with the frame's snapshot (pumpPublish) and at the end of
//      its frame (pumpAudio)
//   D  the window's first snapshot (constructor) is a real one: with a saved
//      volume of 0.3 in the config file, the config the window remembers as
//      saved says 0.3 - not the 0.5 of a snapshot nobody filled
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/app_commands.hpp"
#include "core/config.hpp"
#include "gui/app_window.hpp"
#include "imgui.h"
#include "test_check.hpp"

namespace cascade::gui {
struct AppWindowTestAccess {
    static cascade::core::AppConfig current(AppWindow& a) { return a.currentConfig(); }
    static cascade::core::AppConfig saved(AppWindow& a) { return a.savedCfg_; }
    static cascade::core::AppConfig lastRequested(AppWindow& a) { return a.lastRequestedConfig_; }
    static void saveNow(AppWindow& a) { a.saveConfigNow(); }
    static void publish(AppWindow& a) { a.engine_.publishConfig(); }
    static void pump(AppWindow& a) { a.engine_.pump(); }
    static cascade::engine::EngineStatusText status(AppWindow& a) { return a.engine_.statusText(); }
    static void pumpPublish(AppWindow& a) { (void)a.engine_.pumpPublish(); }
    static void pumpAudio(AppWindow& a) { a.engine_.pumpAudio(); }
    // Every status line, set as the engine's own code sets it.
    static void setAllStatus(AppWindow& a, const std::string& p) {
        cascade::engine::Engine& e = a.engine_;
        e.sourceError_ = p + "1";
        e.gpsRefusal_ = p + "2";
        e.catalogError_ = p + "3";
        e.bandPlanError_ = p + "4";
        e.tuneMismatchNote_ = p + "5";
        e.transmitError_ = p + "6";
        e.soundCardMissing_ = p + "7";
        e.sdrPlayApiDetail_ = p + "8";
        e.sdrPlayAdvice_ = p + "9";
        e.recordNotice_ = p + "10";
        e.recordError_ = p + "11";
        e.presetNote_ = p + "12";
        e.pluginEnforceError_ = p + "13";
        e.restoreKeepLabel_ = p + "14";
    }
    static FoxCommandResult volume(AppWindow& a, double v) {
        return a.engine_.applyCommand(cascade::core::cmd::makeNum(FOXAPI_OP_SET_VOLUME, v));
    }
};
}  // namespace cascade::gui

using A = cascade::gui::AppWindowTestAccess;
using cascade::gui::AppWindow;

namespace {
void setEnv(const char* n, const std::string& v) {
#if defined(_WIN32)
    ::SetEnvironmentVariableA(n, v.c_str());
    _putenv_s(n, v.c_str());
#else
    ::setenv(n, v.c_str(), 1);
#endif
}
}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_config_snapshot_app\n");
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    const auto scratch = std::filesystem::temp_directory_path() / ("foxsdr_config_snapshot_" + std::to_string(pid));
    std::filesystem::create_directories(scratch);
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, scratch.string());
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");

    // The engine's own frame (Engine::pump) reads ImGui's clock through the
    // host, as the window's frame does.
    ImGui::CreateContext();
    const std::string path = (scratch / "config.json").string();
    {
        cascade::core::AppConfig seed;
        seed.volume = 0.3f;
        std::string err;
        CHECK(cascade::core::ConfigStore::save(path, seed, err));
    }
    {
        AppWindow app(path);

        // D: the first snapshot is real.
        std::printf("D: the config remembered as saved says volume %.2f (the file: 0.30)\n",
                    static_cast<double>(A::saved(app).volume));
        CHECK(A::saved(app).volume == 0.3f);

        // A: not published yet -> not in currentConfig(); published -> in it.
        const float before = A::current(app).volume;
        CHECK(A::volume(app, 0.25).status == FOXAPI_OK);
        std::printf("A: volume set to 0.25, before the engine publishes: %.2f\n",
                    static_cast<double>(A::current(app).volume));
        CHECK(A::current(app).volume == before);
        A::publish(app);
        std::printf("   after it publishes: %.2f\n", static_cast<double>(A::current(app).volume));
        CHECK(A::current(app).volume == 0.25f);

        // B: one pump of the engine's frame publishes it.
        CHECK(A::volume(app, 0.5).status == FOXAPI_OK);
        CHECK(A::current(app).volume == 0.25f);
        A::pump(app);
        std::printf("B: after one pump: %.2f\n", static_cast<double>(A::current(app).volume));
        CHECK(A::current(app).volume == 0.5f);

        // C: the save at shutdown carries a change made after the last pump.
        CHECK(A::volume(app, 0.75).status == FOXAPI_OK);
        A::saveNow(app);
        std::printf("C: saved at shutdown: %.2f\n", static_cast<double>(A::lastRequested(app).volume));
        CHECK(A::lastRequested(app).volume == 0.75f);

        // E: the status lines.
        auto allAre = [](const cascade::engine::EngineStatusText& t, const std::string& p) {
            return t.sourceError == p + "1" && t.gpsRefusal == p + "2" && t.catalogError == p + "3" &&
                   t.bandPlanError == p + "4" && t.tuneMismatchNote == p + "5" && t.transmitError == p + "6" &&
                   t.soundCardMissing == p + "7" && t.sdrPlayApiDetail == p + "8" && t.sdrPlayAdvice == p + "9" &&
                   t.recordNotice == p + "10" && t.recordError == p + "11" && t.presetNote == p + "12" &&
                   t.pluginEnforceError == p + "13" && t.restoreKeepLabel == p + "14";
        };
        A::setAllStatus(app, "first");
        CHECK(A::status(app).sourceError != "first1");   // not handed over yet
        A::pumpPublish(app);
        std::printf("E: after the frame's publish, all fourteen handed over: %s\n",
                    allAre(A::status(app), "first") ? "yes" : "NO");
        CHECK(allAre(A::status(app), "first"));
        A::setAllStatus(app, "later");
        CHECK(allAre(A::status(app), "first"));
        A::pumpAudio(app);
        std::printf("   after the end of the frame: %s\n", allAre(A::status(app), "later") ? "yes" : "NO");
        CHECK(allAre(A::status(app), "later"));
    }

    ImGui::DestroyContext();
    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
    return testSummary("test_config_snapshot_app");
}
