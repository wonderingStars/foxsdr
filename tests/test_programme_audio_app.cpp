// A real module in a real AppWindow, on its own preset with the default I/Q
// mute. Measure the finished audio rather than just the plugin's claim.
//
// CONVERTED (stub campaign B9, PLUGIN-ROW-KEYS' pending list; master's test unedited except as the reasons below say). It was master's own test of
// master's AppWindow: it reached the pipeline, the plugin host, the mute decision and the preset through `a.pipeline_`, `a.pluginHost_`,
// `a.applyPluginPreset` and the rest - fields this branch's window does not have, because the engine owns them (the window's only door is the table).
// The house pattern (tests/test_audio_lead_app.cpp): rawEngine(a.engine_) through AppWindowTestAccess, and tests/pass_seam.hpp for every call that must
// run on the pass thread - so the test is registered again as _stepped, and the same assertions run against the pass on its own thread.
//   * rescanPlugins, startReceiver, stopReceiver, updateAudioMute, setPluginMutes, applyPluginPreset and pluginMutes are Engine members now;
//   * the saved overrides are read as the window reads them: the engine hands its half of the config over (publishConfig), the window's copy is
//     currentConfig();
//   * `currentDiagContext().audio.mutedByPlugin` - the window's builder of the diagnostics block - is Engine::refreshDiagContext, which renders the
//     block a crash or a hang report prints (core::diagContextBlock): the same fact, asked for the way a report reads it ("muted: ... a decoder
//     plugin"), after the engine has rendered it. Whether the preset lines (de-emphasis, an explicit rate request, keep-tuned-on-start) the test's
//     fixture rides on are in the engine's applyPluginPreset is held by the line-for-line comparison in docs/engine-stage4.md 6.22 (B9).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/config.hpp"
#include "core/diag_report.hpp"
#include "core/package_identity.hpp"
#include "core/plugin_run.hpp"
#include "gui/app_window.hpp"
#include "pass_seam.hpp"
#include "test_check.hpp"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace cc = cascade::core;
namespace cascade::gui {
struct AppWindowTestAccess {
    static cascade::engine::Engine& eng(AppWindow& a) { return rawEngine(a.engine_); }
    static void scan(AppWindow& a) { passseam::on(eng(a), [&] { eng(a).rescanPlugins(); }); }
    static void start(AppWindow& a) { passseam::on(eng(a), [&] { eng(a).startReceiver(); }); }
    static void stop(AppWindow& a) { passseam::on(eng(a), [&] { eng(a).stopReceiver("test"); }); }
    // The audio mute, which a frame decides after its look and before it draws.
    static void frame(AppWindow& a) { passseam::mute(eng(a), [&] { eng(a).updateAudioMute(); }); }
    static void userMute(AppWindow& a, bool muted) {
        passseam::on(eng(a), [&] { eng(a).userMuted_ = muted; });
        frame(a);
    }
    static const std::vector<cc::LoadedPlugin>& plugins(AppWindow& a) { return eng(a).pluginHost_.plugins(); }
    static bool autoMute(AppWindow& a, const cc::LoadedPlugin& p) {
        return passseam::read<bool>(eng(a), [&] { return eng(a).pluginMutes(p); });
    }
    static void preset(AppWindow& a, const cc::LoadedPlugin& p, const CascadePreset& ps) {
        passseam::on(eng(a), [&] { eng(a).applyPluginPreset(p, ps); });
    }
    static cc::Pipeline& pipeline(AppWindow& a) { return eng(a).pipeline_; }
    static bool hasMuteDecision(AppWindow& a) {
        return passseam::read<bool>(eng(a), [&] { return !eng(a).mutedBy_.empty(); });
    }
    static std::string muteSubject(AppWindow& a) {
        return passseam::read<std::string>(eng(a), [&] { return eng(a).muteSubjectText(); });
    }
    // What a crash or a hang report would say the audio is muted by: the engine renders the diagnostics block, and the block says who.
    static bool diagMuted(AppWindow& a) {
        passseam::on(eng(a), [&] { eng(a).refreshDiagContext(); });
        return cc::diagContextBlock().find("a decoder plugin") != std::string::npos;
    }
    static void overrideMute(AppWindow& a, const cc::LoadedPlugin& p, bool on) {
        passseam::on(eng(a), [&] { eng(a).setPluginMutes(p, on); });
    }
    static cc::AppConfig config(AppWindow& a) {
        passseam::on(eng(a), [&] { eng(a).publishConfig(); });
        return a.currentConfig();
    }
};
}
using Access = cascade::gui::AppWindowTestAccess;

namespace {
void env(const char* key, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(key, value.c_str());
    ::SetEnvironmentVariableA(key, value.c_str());
#else
    setenv(key, value.c_str(), 1);
#endif
}
bool advance(cascade::gui::AppWindow& app, std::uint64_t frames) {
    auto& p = Access::pipeline(app);
    const auto before = p.audioSamplesProduced();
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < end) {
        Access::frame(app);
        if (p.audioSamplesProduced() - before >= frames) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}
double amplitude(const std::vector<float>& pcm) {
    double re = 0.0, im = 0.0;
    for (size_t i = 0; i < pcm.size(); ++i) {
        const double phase = 6.2831853071795864769 * 1000.0 * static_cast<double>(i) / 48000.0;
        re += pcm[i] * std::cos(phase);
        im += pcm[i] * std::sin(phase);
    }
    return 2.0 * std::hypot(re, im) / static_cast<double>(pcm.size());
}
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc != 2) { return 2; }
#if defined(_WIN32)
    const int pid = static_cast<int>(::GetCurrentProcessId());
    const char* ext = ".dll";
#else
    const int pid = static_cast<int>(getpid());
#if defined(__APPLE__)
    const char* ext = ".dylib";
#else
    const char* ext = ".so";
#endif
#endif
    const fs::path scratch = fs::temp_directory_path() / ("foxsdr_programme_audio_" + std::to_string(pid) + "_" + passseam::name());
    fs::create_directories(scratch);
    for (const char* name : {"APPDATA", "LOCALAPPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                             "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        env(name, scratch.string());
    }
    for (const char* name : {"FOXSDR_TELEMETRY_URL", "FOXSDR_CRASH_URL", "FOXSDR_UPDATE_URL"}) {
        env(name, "http://127.0.0.1:9");
    }
    cc::PackageIdentity identity;
    identity.packaged = true;
    identity.fullName = "programme-audio-app-test";
    cc::setPackageIdentityForTest(identity);
    const fs::path pluginDir(cc::PluginHost::userPluginDir());
    fs::create_directories(pluginDir);
    fs::copy_file(fs::path(argv[1]), pluginDir / (std::string("programme-audio-1.0.0") + ext),
                  fs::copy_options::overwrite_existing);
    {
        cascade::gui::AppWindow app;
        Access::scan(app);
        const cc::LoadedPlugin* plugin = nullptr;
        for (const auto& p : Access::plugins(app)) {
            if (p.loaded && p.name == "Programme Audio Fixture") { plugin = &p; }
        }
        CHECK(plugin != nullptr);
        if (plugin != nullptr) {
            CHECK(Access::autoMute(app, *plugin));
            CHECK(Access::config(app).pluginMuteOverride.empty());
            CascadePreset preset{};
            preset.structSize = sizeof(preset);
            CHECK(plugin->preset != nullptr);
            if (plugin->preset != nullptr) {
                CHECK(plugin->preset->get(0u, &preset) == 1);
                Access::preset(app, *plugin, preset);
                Access::start(app);
                CHECK(advance(app, 24000u));
                auto& pipeline = Access::pipeline(app);
                std::vector<float> left(4096), right(4096);
                CHECK(pipeline.audioTapStereo(left.data(), right.data(), left.size()) == left.size());
                const double programme = amplitude(left);
                std::printf("AppWindow default I/Q preset mute: 1 kHz amplitude %.4f\n", programme);
                CHECK(Access::hasMuteDecision(app));
                CHECK(programme > 0.45 && programme < 0.55);
                CHECK(!pipeline.audioMuted());
                CHECK(Access::muteSubject(app).empty());
                CHECK(!Access::diagMuted(app));

                Access::userMute(app, true);
                CHECK(advance(app, 16000u));
                CHECK(pipeline.audioTapStereo(left.data(), right.data(), left.size()) == left.size());
                CHECK(pipeline.audioMuted());
                for (float sample : left) { CHECK(sample == 0.0f); }
                Access::userMute(app, false);
                CHECK(advance(app, 16000u));
                CHECK(pipeline.audioTapStereo(left.data(), right.data(), left.size()) == left.size());
                CHECK(amplitude(left) > 0.45);

                // An existing explicit unmute remains the same saved inverse
                // default; the audio fix does not reinterpret configurations.
                Access::overrideMute(app, *plugin, false);
                CHECK(!Access::autoMute(app, *plugin));
                CHECK(Access::config(app).pluginMuteOverride.size() == 1u);
                Access::overrideMute(app, *plugin, true);
                CHECK(Access::autoMute(app, *plugin));
                CHECK(Access::config(app).pluginMuteOverride.empty());

#if defined(_WIN32)
                auto play = reinterpret_cast<void (*)(int)>(::GetProcAddress(
                    static_cast<HMODULE>(plugin->nativeHandle), "programme_fixture_play"));
#else
                auto play = reinterpret_cast<void (*)(int)>(dlsym(plugin->nativeHandle, "programme_fixture_play"));
#endif
                CHECK(play != nullptr);
                if (play != nullptr) {
                    play(0);
                    CHECK(advance(app, 16000u));
                    CHECK(pipeline.audioMuted());
                    CHECK(!Access::muteSubject(app).empty());
                    CHECK(Access::diagMuted(app));
                }
                Access::stop(app);
                CHECK(pipeline.audioMuted());
            }
        }
    }
    return testSummary("test_programme_audio_app");
}
