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
//      the fifteen reaches Engine::statusText(), and only once the engine
//      publishes - with the frame's snapshot (pumpPublish) and at the end of
//      its frame (pumpAudio)
//   D  the window's first snapshot (constructor) is a real one: with a saved
//      volume of 0.3 in the config file, the config the window remembers as
//      saved says 0.3 - not the 0.5 of a snapshot nobody filled
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
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
#include "pass_seam.hpp"
#include "imgui.h"
#include "test_check.hpp"

namespace cascade::gui {
struct AppWindowTestAccess {
    static cascade::core::AppConfig current(AppWindow& a) { return a.currentConfig(); }
    static cascade::core::AppConfig saved(AppWindow& a) { return a.savedCfg_; }
    static cascade::core::AppConfig lastRequested(AppWindow& a) { return a.lastRequestedConfig_; }
    // THE HELD FORMS: the engine's own fields and calls, made from this thread - every use is inside a PassHold (free-running
    // the pass is parked there; inline and stepped no pass runs unless the test asks for one). saveConfigNow is the window's,
    // but it hands the engine's half over itself (Engine::publishConfig), which the product does only with no pass running
    // (before the control thread starts, and after shutdownQuiesce).
    static void saveNowHeld(AppWindow& a) { a.saveConfigNow(); }
    static void publishHeld(AppWindow& a) { a.engine_.publishConfig(); }
    static void pump(AppWindow& a) { passseam::whole(a.engine_, [&] { a.engine_.pump(); }); }
    static cascade::engine::EngineStatusText status(AppWindow& a) { return a.engine_.statusText(); }
    static void pumpPublish(AppWindow& a) { passseam::custom(a.engine_, [&] { (void)a.engine_.pumpPublish(); }); }
    static void pumpAudio(AppWindow& a) { passseam::workers(a.engine_, [&] { a.engine_.pumpAudio(); }); }
    // Every status line, set as the engine's own code sets it.
    static void setAllStatusHeld(AppWindow& a, const std::string& p) {
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
        e.audioHealthNote_ = p + "15";  // API 0.3 8.4: the Sinks section's note, the fifteenth line
        e.bookmarkImportNote_ = p + "16";  // API 0.3 8.6: the Bookmarks section's note and red line,
        e.bookmarkError_ = p + "17";       // the sixteenth and the seventeenth
        e.installReport_ = p + "18";       // API 0.3 8.8: the plugin install/remove outcome, the
        e.installError_ = p + "19";        // eighteenth and the nineteenth
        e.catalogStatus_ = p + "20";       // API 0.3 8.9: the plugin store's catalogue status line and its
        e.addAllSummary_ = p + "21";       // ADD ALL summary, the twentieth and the twenty-first
    }
    static FoxCommandResult volumeHeld(AppWindow& a, double v) {
        return a.engine_.applyCommand(cascade::core::cmd::makeNum(FOXAPI_OP_SET_VOLUME, v));
    }
    // THE PASS HELD, free-running only: a piece of work posted to the pass (Engine::postToPass) that waits until the hold is
    // let go. Once it has STARTED no other pass can start until it ends, so between the constructor and the destructor the
    // only publishes are the test's own - which is what A, B, C and E are about: what a reader sees BEFORE the engine's next
    // publish. Inline and stepped no pass runs unless the test asks for one, so there is nothing to hold. Nothing may ask for
    // a pass (pump, pumpPublish, pumpAudio) while it is held: the Held forms and the thread-safe reads only.
    class PassHold {
    public:
        explicit PassHold(AppWindow& a) {
            if (passseam::mode() != passseam::Mode::Free) { return; }
            passseam::attach(a.engine_);
            s_ = std::make_shared<State>();
            std::shared_ptr<State> s = s_;
            a.engine_.postToPass([s] {
                std::unique_lock<std::mutex> lk(s->m);
                s->running = true;
                s->cv.notify_all();
                s->cv.wait(lk, [&] { return s->release; });
            });
            std::unique_lock<std::mutex> lk(s_->m);
            const bool took = s_->cv.wait_for(lk, std::chrono::seconds(10), [&] { return s_->running; });
            CHECK(took);
        }
        ~PassHold() {
            if (s_ == nullptr) { return; }
            {
                std::lock_guard<std::mutex> lk(s_->m);
                s_->release = true;
            }
            s_->cv.notify_all();
        }
        PassHold(const PassHold&) = delete;
        PassHold& operator=(const PassHold&) = delete;

    private:
        struct State {
            std::mutex m;
            std::condition_variable cv;
            bool running = false;
            bool release = false;
        };
        std::shared_ptr<State> s_;
    };
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
        // (A, B's first half, C and E's "not yet" steps run with the pass HELD: free-running, a pass of the engine's own would
        // publish within milliseconds, and each is about what a reader sees before the next publish.)
        const float before = A::current(app).volume;
        {
            A::PassHold hold(app);
            CHECK(A::volumeHeld(app, 0.25).status == FOXAPI_OK);
            std::printf("A: volume set to 0.25, before the engine publishes: %.2f\n",
                        static_cast<double>(A::current(app).volume));
            CHECK(A::current(app).volume == before);
            A::publishHeld(app);
            std::printf("   after it publishes: %.2f\n", static_cast<double>(A::current(app).volume));
            CHECK(A::current(app).volume == 0.25f);
        }

        // B: one pump of the engine's frame publishes it.
        {
            A::PassHold hold(app);
            CHECK(A::volumeHeld(app, 0.5).status == FOXAPI_OK);
            CHECK(A::current(app).volume == 0.25f);
        }
        A::pump(app);
        std::printf("B: after one pump: %.2f\n", static_cast<double>(A::current(app).volume));
        CHECK(A::current(app).volume == 0.5f);

        // C: the save at shutdown carries a change made after the last pump.
        {
            A::PassHold hold(app);
            CHECK(A::volumeHeld(app, 0.75).status == FOXAPI_OK);
            A::saveNowHeld(app);
        }
        std::printf("C: saved at shutdown: %.2f\n", static_cast<double>(A::lastRequested(app).volume));
        CHECK(A::lastRequested(app).volume == 0.75f);

        // E: the status lines.
        auto allAre = [](const cascade::engine::EngineStatusText& t, const std::string& p) {
            return t.sourceError == p + "1" && t.gpsRefusal == p + "2" && t.catalogError == p + "3" &&
                   t.bandPlanError == p + "4" && t.tuneMismatchNote == p + "5" && t.transmitError == p + "6" &&
                   t.soundCardMissing == p + "7" && t.sdrPlayApiDetail == p + "8" && t.sdrPlayAdvice == p + "9" &&
                   t.recordNotice == p + "10" && t.recordError == p + "11" && t.presetNote == p + "12" &&
                   t.pluginEnforceError == p + "13" && t.restoreKeepLabel == p + "14" &&
                   t.audioHealth == p + "15" && t.bookmarkNote == p + "16" && t.bookmarkError == p + "17" &&
                   t.installReport == p + "18" && t.installError == p + "19" && t.catalogStatus == p + "20" &&
                   t.addAllSummary == p + "21";
        };
        {
            A::PassHold hold(app);
            A::setAllStatusHeld(app, "first");
            CHECK(A::status(app).sourceError != "first1");   // not handed over yet
        }
        A::pumpPublish(app);
        std::printf("E: after the frame's publish, all twenty-one handed over: %s\n",
                    allAre(A::status(app), "first") ? "yes" : "NO");
        CHECK(allAre(A::status(app), "first"));
        {
            A::PassHold hold(app);
            A::setAllStatusHeld(app, "later");
            CHECK(allAre(A::status(app), "first"));
        }
        A::pumpAudio(app);
        std::printf("   after the end of the frame: %s\n", allAre(A::status(app), "later") ? "yes" : "NO");
        CHECK(allAre(A::status(app), "later"));
    }

    ImGui::DestroyContext();
    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
    return testSummary("test_config_snapshot_app");
}
