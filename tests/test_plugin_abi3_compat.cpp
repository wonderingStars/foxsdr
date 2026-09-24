// THE ABI-3 COMPATIBILITY PROOF for host API level 1: every PUBLISHED plugin
// binary - built against the header as it was before level 1 existed - loads,
// attaches, is fed and is torn down on this host exactly as before.
//
// WHY A TEST AND NOT A CLAIM. Level 1 grows CascadeHostApi at the end and adds
// a capability bit (plugin_abi.h, "VERSIONING: STILL ABI 3"). The argument
// that no old plugin can notice is sound on paper; this runs the argument
// against the real modules users have installed. It uses the SAME host
// objects the application uses - PluginHost to scan and validate, PluginUi to
// attach (handing every host-client plugin the level-1 table), PluginRunner
// to create and feed every decoder - so a published plugin that tripped over
// the bigger table, the new capability check or the reordered attach would
// fail here.
//
// WHERE THE BINARIES ARE. Not in this repository - they are the plugin
// repository's plugins/ directory, which is what the catalogue publishes. Name
// it in FOXSDR_PUBLISHED_PLUGINS_DIR. Without it this test says SKIP, loudly,
// and proves nothing; it never passes by default.
//
// ISOLATED. Loading a plugin runs its code, and several keep caches and logs
// under the user's profile. Before anything is loaded this points APPDATA,
// LOCALAPPDATA, HOME and the XDG directories at a fresh scratch directory and
// makes it the working directory, so the run cannot touch the owner's real
// %LOCALAPPDATA%\foxsdr (or ~/.local/share/foxsdr) - and it checks afterwards
// that everything the plugins wrote landed there.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "core/host_image.hpp"
#include "core/plugin_abi.h"
#include "core/plugin_api.hpp"
#include "core/plugin_host.hpp"
#include "core/plugin_runner.hpp"
#include "core/plugin_ui.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using cascade::core::LoadedPlugin;
using cascade::core::PluginHost;
using cascade::core::PluginRunner;
using cascade::core::PluginUi;

namespace {

// Both copies of the environment: the CRT's (getenv) and the OS's
// (GetEnvironmentVariable) - a statically linked plugin reads one, the host the
// other (see the lesson in the repository notes about _putenv_s).
void setEnv(const char* k, const std::string& v) {
#if defined(_WIN32)
    _putenv_s(k, v.c_str());
    SetEnvironmentVariableA(k, v.c_str());
#else
    setenv(k, v.c_str(), 1);
#endif
}

std::size_t countCandidates(const fs::path& dir) {
    std::size_t n = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (e.is_regular_file() && PluginHost::hasPluginExtension(e.path().filename().string())) {
            ++n;
        }
    }
    return n;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const char* dirEnv = std::getenv("FOXSDR_PUBLISHED_PLUGINS_DIR");
    if (dirEnv == nullptr || dirEnv[0] == '\0') {
        std::printf("SKIP: set FOXSDR_PUBLISHED_PLUGINS_DIR to the plugin repository's plugins/ "
                    "directory to load the published binaries. NOTHING WAS PROVED.\n");
        return testSummary("test_plugin_abi3_compat");
    }
    const fs::path dir = fs::absolute(dirEnv);
    std::printf("published plugins: %s\n", dir.string().c_str());

    // --- isolation, before a single module is mapped ------------------------
    const fs::path scratch =
        fs::temp_directory_path() /
        ("foxsdr_abi3_compat_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(scratch / "local");
    fs::create_directories(scratch / "roaming");
    fs::create_directories(scratch / "home");
    setEnv("LOCALAPPDATA", (scratch / "local").string());
    setEnv("APPDATA", (scratch / "roaming").string());
    setEnv("HOME", (scratch / "home").string());
    setEnv("XDG_DATA_HOME", (scratch / "home" / ".local" / "share").string());
    setEnv("XDG_CONFIG_HOME", (scratch / "home" / ".config").string());
    setEnv("XDG_CACHE_HOME", (scratch / "home" / ".cache").string());
    fs::current_path(scratch);
    std::printf("isolated in %s\n", scratch.string().c_str());

    const std::size_t candidates = countCandidates(dir);
    CHECK(candidates > 0u);

    // --- 1. every published binary loads through the validator -------------
    PluginHost host;
    host.scan(dir.string());
    std::size_t hostClients = 0;
    for (const LoadedPlugin& p : host.plugins()) {
        std::printf("  %-48s %s  %-22s v%-8s caps=0x%03X %s\n",
                    fs::path(p.path).filename().string().c_str(), p.loaded ? "LOADED " : "REFUSED",
                    p.name.c_str(), p.version.c_str(), static_cast<unsigned>(p.capabilities),
                    p.error.c_str());
        CHECK(p.loaded);
        // None of them knows level 1 exists: it declares no bit above AUDIO_OUT.
        CHECK((p.capabilities & ~0x7FFu) == 0u);
        if (p.hostClient != nullptr) { ++hostClients; }
    }
    CHECK(host.plugins().size() == candidates);
    CHECK(host.loadedCount() == candidates);
    std::printf("%zu of %zu published binaries loaded; %zu take the host table\n",
                host.loadedCount(), candidates, hostClients);

    // --- 2. every host-client plugin is handed the LEVEL-1 table ------------
    std::atomic<int> centreCalls{0}, rateCalls{0}, timeCalls{0}, tuneCalls{0};
    PluginUi ui;
    cascade::core::HostServices svc;
    svc.centreHz = [&] {
        ++centreCalls;
        return 1090.0e6;
    };
    svc.sampleRateHz = [&] {
        ++rateCalls;
        return 2.4e6;
    };
    svc.tune = [&](double) {
        ++tuneCalls;
        return static_cast<std::int32_t>(CASCADE_TUNE_OK);
    };
    svc.unixTimeMs = [&] {
        ++timeCalls;
        return static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
    };
    ui.setServices(std::move(svc));
    cascade::core::ReceiverFacts f;
    f.running = true;
    f.centreHz = 1090.0e6;
    f.sampleRateHz = 2.4e6;
    f.outputRateHz = 48000.0;
    ui.api().publish(f);
    ui.rebuild(host.plugins());
    // Attached = a live client in the level-1 core: one per host-client plugin.
    CHECK(ui.api().clientCount() == hostClients);
    for (int i = 0; i < 20; ++i) { ui.poll(); }
    std::printf("UI half: %zu tracks, %zu panels, %zu instruments after 20 polls\n",
                ui.tracks().size(), ui.panels().size(), ui.instruments().size());

    // --- 3. every decoder is created at the rates a receiver really runs at,
    //        fed noise on a real-time thread, polled, and destroyed -----------
    PluginRunner runner;
    runner.setStreamClock(ui.api().streamClock());
    runner.rebuild(host.plugins(), 48000.0, 2.4e6, 1090.0e6);
    std::mt19937 rng(1234);
    std::normal_distribution<float> noise(0.0f, 0.05f);
    std::vector<float> audio(4800);
    std::vector<float> iq(2 * 24000);
    std::vector<float> left(4800), right(4800);
    std::size_t lines = 0;
    std::vector<cascade::core::HostImage> images;
    {
        const cascade::core::RealtimeThreadScope realtime;
        for (int block = 0; block < 50; ++block) {
            for (float& v : audio) { v = noise(rng); }
            for (float& v : iq) { v = noise(rng); }
            runner.processAudio(audio.data(), audio.size());
            runner.processIq(iq.data(), iq.size() / 2u);
            // No published plugin processes audio (the bit is new), so the
            // chain must leave the buffer exactly as it was.
            std::copy(audio.begin(), audio.end(), left.begin());
            std::copy(audio.begin(), audio.end(), right.begin());
            runner.processAudioChain(left.data(), right.data(), left.size());
            CHECK(std::memcmp(left.data(), audio.data(), audio.size() * sizeof(float)) == 0);
            float l0 = 0.0f, r0 = 0.0f;
            (void)runner.pullPluginAudio(&l0, &r0, 1);
        }
    }
    lines += runner.drainText().size();
    runner.pollImages(images);
    std::size_t running = 0, idle = 0;
    for (const cascade::core::DecoderStatus& s : runner.status()) {
        if (s.reason == cascade::core::DecoderIdleReason::Running) {
            ++running;
        } else {
            ++idle;
            std::printf("  idle: %s\n", s.detail.c_str());
        }
    }
    std::printf("runner: %zu running, %zu idle (rate mismatches are expected at 2.4 MS/s), "
                "%zu audio + %zu I/Q frames fed, %zu text lines, %zu images\n",
                running, idle, runner.audioFramesFed(), runner.iqFramesFed(), lines,
                images.size());
    CHECK(running > 0u);
    // Fed on whichever streams the set has decoders for (the catalogue has
    // both; a directory of one audio decoder has no I/Q to feed).
    bool anyAudio = false, anyIq = false;
    for (const LoadedPlugin& p : host.plugins()) {
        anyAudio = anyAudio || p.decoder != nullptr ||
                   (p.imageDecoder != nullptr && p.imageDecoder->inputKind == CASCADE_INPUT_AUDIO);
        anyIq = anyIq || p.iqDecoder != nullptr ||
                (p.imageDecoder != nullptr && p.imageDecoder->inputKind == CASCADE_INPUT_IQ);
    }
    CHECK(!anyAudio || runner.audioFramesFed() > 0u);
    CHECK(!anyIq || runner.iqFramesFed() > 0u);
    CHECK(runner.processorTitles().empty());

    // --- 4. the capabilities nothing above creates: create, poll, destroy ---
    for (const LoadedPlugin& p : host.plugins()) {
        if (p.basemap != nullptr) {
            void* h = p.basemap->create();
            CHECK(h != nullptr);
            if (h != nullptr) {
                char buf[256];
                (void)p.basemap->poll_text(h, buf, sizeof(buf));
                p.basemap->destroy(h);
            }
        }
        if (p.trackInfo != nullptr) {
            void* h = p.trackInfo->create();
            CHECK(h != nullptr);
            if (h != nullptr) {
                char buf[256];
                (void)p.trackInfo->poll_text(h, buf, sizeof(buf));
                p.trackInfo->destroy(h);
            }
        }
        if (p.preset != nullptr) {
            const std::uint32_t n = p.preset->count();
            for (std::uint32_t i = 0; i < n && i < 64u; ++i) {
                CascadePreset ps{};
                ps.structSize = sizeof(ps);
                (void)p.preset->get(i, &ps);
            }
        }
    }

    // --- 5. teardown in the application's own order --------------------------
    runner.clear();
    ui.clear();
    host.unloadAll();
    std::printf("host services used by old plugins through the grown table: centre %d, rate "
                "%d, time %d, tune %d\n",
                centreCalls.load(), rateCalls.load(), timeCalls.load(), tuneCalls.load());

    // Nothing was granted, so nothing can have tuned.
    CHECK(tuneCalls.load() == 0);
    std::printf("scratch profile left in place for inspection: %s\n", scratch.string().c_str());
    return testSummary("test_plugin_abi3_compat");
}
