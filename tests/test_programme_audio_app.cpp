// A real module in a real AppWindow, on its own preset with the default I/Q
// mute. Measure the finished audio rather than just the plugin's claim.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/config.hpp"
#include "core/package_identity.hpp"
#include "core/plugin_run.hpp"
#include "gui/app_window.hpp"
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
    static void scan(AppWindow& a) { a.rescanPlugins(); }
    static void start(AppWindow& a) { a.startReceiver(); }
    static void stop(AppWindow& a) { a.stopReceiver("test"); }
    static void frame(AppWindow& a) { a.updateAudioMute(); }
    static void userMute(AppWindow& a, bool muted) { a.userMuted_ = muted; frame(a); }
    static const std::vector<cc::LoadedPlugin>& plugins(AppWindow& a) { return a.pluginHost_.plugins(); }
    static bool autoMute(AppWindow& a, const cc::LoadedPlugin& p) { return a.pluginMutes(p); }
    static void preset(AppWindow& a, const cc::LoadedPlugin& p, const CascadePreset& ps) {
        a.applyPluginPreset(p, ps);
    }
    static cc::Pipeline& pipeline(AppWindow& a) { return a.pipeline_; }
    static bool hasMuteDecision(AppWindow& a) { return !a.mutedBy_.empty(); }
    static std::string muteSubject(AppWindow& a) { return a.muteSubjectText(); }
    static bool diagMuted(AppWindow& a) { return a.currentDiagContext().audio.mutedByPlugin; }
    static void overrideMute(AppWindow& a, const cc::LoadedPlugin& p, bool on) { a.setPluginMutes(p, on); }
    static cc::AppConfig config(AppWindow& a) { return a.currentConfig(); }
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
    const fs::path scratch = fs::temp_directory_path() / ("foxsdr_programme_audio_" + std::to_string(pid));
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
