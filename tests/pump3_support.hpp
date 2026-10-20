// pump3_support.hpp - what the two tests of the plugin UI's thread ownership share (the pump move, step P3):
// tests/test_plugin_ui_threaded.cpp and tests/test_plugin_unload_handshake.cpp.
//
// Include AFTER gui/app_window.hpp and engine_api_fake_radio.hpp (the test .cpp includes the gui header itself: the build links the
// window by what the test's own includes name). It defines cascade::gui::AppWindowTestAccess - the friend the window and the
// engine name for their tests - and the helpers around the instrumented fixture module (tests/fixture_pump3_plugin.cpp).
//
// THE WORLD THESE TESTS SIMULATE is the pump move's end state, "P5": the engine's pass on a thread of its own and the window's
// frame - here, the test's own thread - polling the plugin UI beside it. The product still pumps in the frame (P5 is not built);
// what is real in these tests is every cross-thread edge that move will use: the directive the pass posts, the window applying it
// at the head of its poll, the acknowledgement, the bounded wait, the queued request_tune, the unload handshake.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

#include "core/app_commands.hpp"
#include "core/diag_log.hpp"
#include "engine/engine.hpp"

namespace pump3 {

namespace cmd = cascade::core::cmd;
using cascade::engine::Engine;

inline void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

inline std::filesystem::path g_scratch;
inline std::string g_pluginDir;
inline std::string pluginDirHook() { return g_pluginDir; }

// A scratch tree for the config, the plugins and the unmap log, with every per-user location pointed into it.
inline void isolate(const char* tag) {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = std::filesystem::temp_directory_path() / (std::string("foxsdr_") + tag + "_" + std::to_string(pid));
    std::error_code ec;
    std::filesystem::remove_all(g_scratch, ec);
    std::filesystem::create_directories(g_scratch / "plugins");
    g_pluginDir = (g_scratch / "plugins").string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME",
                          "XDG_CACHE_HOME"}) {
        setEnv(v, g_scratch.string());
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
}

// The fixture copied into the plugin directory; its path there (a module is identified by this path in the OS).
inline std::string installFixture() {
    const std::filesystem::path src(FIXTURE_PUMP3_PLUGIN);
    const std::filesystem::path dst = std::filesystem::path(g_pluginDir) / src.filename();
    std::error_code ec;
    std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);
    return dst.string();
}
inline std::string fixtureKey() { return std::filesystem::path(FIXTURE_PUMP3_PLUGIN).filename().string(); }
inline std::string fixtureName() { return "Pump3 Fixture"; }

// ---- the instrumented module, looked up in the OS's own table of loaded images, so a test can ask it things without touching the
// engine's plugin list (which the control thread owns) - and tell "unmapped" from "mapped" -----------------------------------------
inline void* loadedImage(const std::string& path) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetModuleHandleA(path.c_str()));   // (takes no reference)
#else
    // dlopen takes a REFERENCE even with RTLD_NOLOAD: the probe lets go of it at once (releaseImage), or it would keep the
    // module mapped for ever and the host's own dlclose would never unmap it.
    return dlopen(path.c_str(), RTLD_NOLOAD | RTLD_LAZY);
#endif
}
inline void releaseImage(void* image) {
#if !defined(_WIN32)
    if (image != nullptr) { dlclose(image); }
#else
    (void)image;
#endif
}
inline void* symbol(void* image, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(image), name));
#else
    return dlsym(image, name);
#endif
}

struct Fx {
    bool mapped = false;
    long (*live)() = nullptr;
    long (*creates)() = nullptr;
    long (*destroys)() = nullptr;
    long (*polls)() = nullptr;
    long (*attaches)() = nullptr;
    long (*staleCalls)() = nullptr;
    long (*destroyDuringPoll)() = nullptr;
    long (*wrongThread)() = nullptr;
    int (*inPoll)() = nullptr;
    void (*setGate)(int) = nullptr;
    void (*setChurn)(int, double, double) = nullptr;
    long (*tuneCalls)() = nullptr;
    int (*lastTuneRc)() = nullptr;
    int (*requestTune)(double) = nullptr;
    double (*centre)() = nullptr;
    long (*basemapLive)() = nullptr;
    long (*infoLive)() = nullptr;
    unsigned long long (*basemapCreateThread)() = nullptr;
    unsigned long long (*basemapDestroyThread)() = nullptr;
    unsigned long long (*infoCreateThread)() = nullptr;
    unsigned long long (*infoDestroyThread)() = nullptr;
    unsigned long long (*thread)() = nullptr;
    void (*reset)() = nullptr;
};

template <class F>
void bind(void* image, const char* name, F& slot) {
    slot = reinterpret_cast<F>(symbol(image, name));
}

// Whatever image of the fixture is mapped right now (unmapped: mapped == false and every pointer null).
inline Fx fxNow(const std::string& path) {
    Fx f;
    void* image = loadedImage(path);
    if (image == nullptr) { return f; }
    f.mapped = true;
    bind(image, "fixture_p3_live", f.live);
    bind(image, "fixture_p3_creates", f.creates);
    bind(image, "fixture_p3_destroys", f.destroys);
    bind(image, "fixture_p3_polls", f.polls);
    bind(image, "fixture_p3_attaches", f.attaches);
    bind(image, "fixture_p3_stale_calls", f.staleCalls);
    bind(image, "fixture_p3_destroy_during_poll", f.destroyDuringPoll);
    bind(image, "fixture_p3_wrong_thread", f.wrongThread);
    bind(image, "fixture_p3_in_poll", f.inPoll);
    bind(image, "fixture_p3_set_gate", f.setGate);
    bind(image, "fixture_p3_set_churn", f.setChurn);
    bind(image, "fixture_p3_tune_calls", f.tuneCalls);
    bind(image, "fixture_p3_last_tune_rc", f.lastTuneRc);
    bind(image, "fixture_p3_request_tune", f.requestTune);
    bind(image, "fixture_p3_centre_hz", f.centre);
    bind(image, "fixture_p3_basemap_live", f.basemapLive);
    bind(image, "fixture_p3_info_live", f.infoLive);
    bind(image, "fixture_p3_basemap_create_thread", f.basemapCreateThread);
    bind(image, "fixture_p3_basemap_destroy_thread", f.basemapDestroyThread);
    bind(image, "fixture_p3_info_create_thread", f.infoCreateThread);
    bind(image, "fixture_p3_info_destroy_thread", f.infoDestroyThread);
    bind(image, "fixture_p3_thread", f.thread);
    bind(image, "fixture_p3_reset", f.reset);
    releaseImage(image);   // (the pointers stay good for as long as the host keeps the module mapped, which is what a caller asks)
    return f;
}

// ---- waiting ---------------------------------------------------------------------------------------------------------------------
inline bool waitFor(const std::function<bool()>& pred, int timeoutMs, int sliceMs = 1) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(sliceMs));
    }
    return pred();
}

// How many lines in the diagnostic ring contain `needle`.
inline int diagLines(const std::string& needle) {
    int n = 0;
    for (const std::string& l : cascade::core::DiagLog::instance().ringSnapshot()) {
        if (l.find(needle) != std::string::npos) { ++n; }
    }
    return n;
}

}  // namespace pump3

namespace cascade::gui {

// The friend the window and the engine name for their tests.
struct AppWindowTestAccess {
    using Engine = cascade::engine::Engine;
    static void install() {
        Engine::testHooks_.makeDevice = &engine_api_test::makeFakeRadio;
        Engine::testHooks_.nativeScan = &engine_api_test::fakeNativeScan;
        Engine::testHooks_.soapyScan = &engine_api_test::noSoapyRadios;
        Engine::testHooks_.pluginDir = &pump3::pluginDirHook;
    }

    static void setUnmapHook(void (*hook)()) { Engine::testHooks_.pluginsAboutToUnmap = hook; }
    static Engine& engine(AppWindow& a) { return a.engine_; }
    static void submit(AppWindow& a, const cascade::core::cmd::QueuedCommand& q) { a.uiSubmit(q); }

    // ---- the window's plugin UI: the one thing the GUI thread does with it --------------------------------------------------
    static void poll(AppWindow& a) { a.hostPollPlugins(); }                     // head-of-poll apply, then the poll
    static void applyOnly(AppWindow& a) { a.applyPluginDirective(); }
    static cascade::core::PluginUi& ui(AppWindow& a) { return a.pluginUi_; }
    static const std::vector<cascade::core::HostTrack>& tracks(AppWindow& a) { return a.hostTracks(); }
    static BasemapCache& basemap(AppWindow& a) { return a.basemap_; }
    static TrackInfoCache& trackInfo(AppWindow& a) { return a.trackInfo_; }

    // ---- the engine's thread --------------------------------------------------------------------------------------------------
    static void startControl(AppWindow& a, std::chrono::milliseconds period = std::chrono::milliseconds(2),
                             const std::string& reportDir = std::string(), unsigned thresholdMs = 5000) {
        cascade::engine::ControlThreadOptions o;
        o.period = period;
        o.watchdogReportDir = reportDir;
        o.watchdogThresholdMs = thresholdMs;
        a.engine_.startControlThread(o);
    }
    static void stopControl(AppWindow& a) { a.engine_.stopControlThread(); }
    static bool controlRunning(AppWindow& a) { return a.engine_.controlThreadRunning(); }
    static void park(AppWindow& a, bool on) { a.engine_.controlParkForTest_.store(on); }
    static std::uint64_t passes(AppWindow& a) { return a.engine_.controlPasses_.load(); }
    static unsigned watchdogReports(AppWindow& a) { return a.engine_.controlWatchdog_.reportsWritten(); }
    static unsigned watchdogPauses(AppWindow& a) { return a.engine_.controlWatchdog_.pausesTaken(); }

    // ---- the link and the engine half of the plugin system ---------------------------------------------------------------------
    static cascade::core::PluginDirectiveLink& link(AppWindow& a) { return a.engine_.pluginLink_; }
    static cascade::core::PluginApiCore& core(AppWindow& a) { return *a.engine_.pluginApi_; }
    static bool unloadPending(AppWindow& a) { return a.engine_.pluginUnloadOp_ != Engine::PluginUnloadOp::None; }
    static std::size_t pluginsLoaded(AppWindow& a) { return a.engine_.pluginHost_.loadedCount(); }
    // (Only while the pass is not running: the pass owns the list.)
    static const std::vector<cascade::core::LoadedPlugin>& loaded(AppWindow& a) { return a.engine_.pluginHost_.plugins(); }

    // ---- what the receiver is doing, from the one snapshot ------------------------------------------------------------------
    static double centre(AppWindow& a) {
        cascade::core::PublishedState ps;
        for (int i = 0; i < 50; ++i) {
            if (a.engine_.receiverSnapshot_->read(ps)) { return ps.rx.centreHz; }
        }
        return -1.0;
    }
    // The SOURCE'S own centre - what the receiver is actually tuned to - read while the pass is parked (the pass owns the source).
    static double sourceCentre(AppWindow& a) { return a.engine_.pipeline_.activeSource().centerFrequencyHz(); }
    static std::uint64_t statePasses(AppWindow& a) {
        cascade::core::PublishedState ps;
        return a.engine_.receiverSnapshot_->read(ps) ? ps.rx.seq : 0u;
    }
};

}  // namespace cascade::gui
