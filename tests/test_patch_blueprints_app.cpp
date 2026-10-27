// THE PATCH BLUEPRINTS, THROUGH THE REAL AppWindow AND A REAL PLUGIN MODULE
// (owner, 2026-10-26: "preset blue prints for all the plugins also make so
// the plugin doesn't have to be on to get the compotes").
//
// test_patch_blueprints pins what a blueprint builds. This proves the
// application wires it to real plugins:
//   - a plugin the user has STOPPED is still in the parts bin's catalogue,
//     with both of its parts (a text decoder and a picture decoder);
//   - the blueprints list reads the plugin's own preset out of the module;
//   - a pick takes over the page's starter radio, places the workflow, moves
//     the view to it and says so;
//   - the placed workflow RUNS with the plugin still stopped: START makes the
//     patch create the module's decoder (the module's own event log says so);
//   - a second pick gets a radio of its own, on a free device, not the
//     signal generator;
//   - with five radios all in use a pick places nothing and says why.
//
// The module is tests/fixtures/preset_probe_plugin.cpp with its text decoder
// switched on (PRESET_PROBE_TEXT_CAP), copied into a scratch plugin folder.
// Hermetic: the per-user directories point at a scratch folder, the native
// device list is a fake (no USB walk), and the only radio ever opened is the
// built-in signal generator. No ImGui frame is rendered.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/package_identity.hpp"
#include "core/patch_blueprints.hpp"
#include "core/patch_devices.hpp"
#include "gui/app_window.hpp"
#include "gui/patch_view_math.hpp"
#include "gui/ui_scale.hpp"
#include "source/device_source.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
namespace cc = cascade::core;
namespace pc = cascade::core::patch;

namespace {

void env(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

// One dongle on the desk, as far as the application can tell.
const char* const kDongleArgs = "driver=rtlsdr,serial=00000001";
std::vector<cascade::source::NativeDeviceInfo> fakeScan() {
    return {{"rtlsdr", "Generic RTL2832U A", kDongleArgs}};
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

namespace cascade::gui {

struct AppWindowTestAccess {
    static void installHooks() { AppWindow::testHooks_.nativeScan = &fakeScan; }
    static void scan(AppWindow& a) { a.rescanPlugins(); }
    static const std::vector<cc::LoadedPlugin>& plugins(AppWindow& a) {
        return a.pluginHost_.plugins();
    }
    static void stopPlugin(AppWindow& a, const std::string& key) {
        a.recordPluginStopped(key, true);
    }
    static bool stopped(AppWindow& a, const std::string& key) { return a.pluginIsStopped(key); }
    static const std::vector<pc::DecoderInfo>& catalogue(AppWindow& a) {
        a.rebuildPatchCatalogue();
        return a.patchCatalogue_;
    }
    static std::vector<pc::Blueprint> blueprints(AppWindow& a) {
        a.rebuildPatchCatalogue();
        return a.patchBlueprints();
    }
    static void seed(AppWindow& a) {
        a.patchSeeded_ = false;
        a.seedPatchIfNeeded();
    }
    static pc::Graph& graph(AppWindow& a) { return a.patchGraph_; }
    // A pick from the list, placed as the page places it: on a canvas of
    // kCanvasW x kCanvasH pixels.
    static constexpr float kCanvasW = 900.0f;
    static constexpr float kCanvasH = 600.0f;
    static void pick(AppWindow& a, const pc::Blueprint& bp) {
        a.patchBlueprintPending_ = bp;
        a.placePendingPatchBlueprint(kCanvasW, kCanvasH);
    }
    static pc::NodeId selected(AppWindow& a) { return a.patchUi_.selected; }
    static const std::string& note(AppWindow& a) { return a.patchBlueprintNote_; }
    static cascade::gui::patch::View view(AppWindow& a) { return a.patchUi_.view; }
    static pc::Plan plan(AppWindow& a) {
        a.rebuildPatchCatalogue();
        return pc::compile(a.patchGraph_, a.patchRadioInfos(), &a.patchCatalogue_);
    }

    // The page's own order for one frame: the catalogue, the transport, the
    // radios, the plan, and the running sets.
    static void frame(AppWindow& a) {
        a.rebuildPatchCatalogue();
        a.patchApplyRunning();
        a.patchReconcile();
        a.patchPlan_ = pc::compile(a.patchGraph_, a.patchRadioInfos(), &a.patchCatalogue_);
        a.patchPublishSets();
    }
    static void pressStart(AppWindow& a) {
        a.patchWasOpen_ = true;   // the page's first-frame scan is not under test
        a.patchPressStart();
    }
    static bool running(AppWindow& a) { return a.patchRunning_; }
    static std::size_t refused(AppWindow& a) { return a.patchRefused_.size(); }
    static void setPosition(AppWindow& a, double lat, double lon) {
        a.rxSet_ = true;
        a.rxLat_ = lat;
        a.rxLon_ = lon;
    }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc != 2) {
        std::printf("usage: test_patch_blueprints_app <preset_probe_fixture module>\n");
        return 2;
    }
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
    const fs::path scratch =
        fs::temp_directory_path() / ("foxsdr_patch_blueprints_app_" + std::to_string(pid));
    fs::create_directories(scratch);
    for (const char* name : {"APPDATA", "LOCALAPPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                             "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        env(name, scratch.string());
    }
    for (const char* name : {"FOXSDR_TELEMETRY_URL", "FOXSDR_CRASH_URL", "FOXSDR_UPDATE_URL"}) {
        env(name, "http://127.0.0.1:9");
    }
    // The module's text decoder on, and its event log in the scratch folder.
    env("PRESET_PROBE_TEXT_CAP", "1");
    const fs::path eventLog = scratch / "preset_probe_events.log";
    env("RESCAN_PROBE_LOG", eventLog.string());

    cc::PackageIdentity identity;
    identity.packaged = true;
    identity.fullName = "patch-blueprints-app-test";
    cc::setPackageIdentityForTest(identity);
    const fs::path pluginDir(cc::PluginHost::userPluginDir());
    fs::create_directories(pluginDir);
    const std::string file = std::string("preset-probe-1.0.1") + ext;
    fs::copy_file(fs::path(argv[1]), pluginDir / file, fs::copy_options::overwrite_existing);
    Access::installHooks();

    {
        cascade::gui::AppWindow app;
        Access::scan(app);
        const cc::LoadedPlugin* plugin = nullptr;
        for (const auto& p : Access::plugins(app)) {
            if (p.loaded && p.name == "Preset Probe") { plugin = &p; }
        }
        CHECK(plugin != nullptr);
        if (plugin == nullptr) { return testSummary("test_patch_blueprints_app"); }
        const std::string key = cc::pluginKey(*plugin);
        CHECK(key == file);

        // --- STOPPED, AND STILL A PART -------------------------------------------
        std::printf("  a stopped plugin is still in the parts bin\n");
        Access::stopPlugin(app, key);
        CHECK(Access::stopped(app, key));
        const std::vector<pc::DecoderInfo>& cat = Access::catalogue(app);
        bool text = false;
        bool picture = false;
        for (const pc::DecoderInfo& d : cat) {
            if (d.key == key && !d.image) { text = true; }
            if (d.key == key + pc::kImageKeySuffix && d.image) { picture = true; }
        }
        CHECK(text);
        CHECK(picture);

        // --- the list, from the module's own preset -------------------------------
        std::printf("  the blueprints list reads the module's preset\n");
        const std::vector<pc::Blueprint> list = Access::blueprints(app);
        const pc::Blueprint* textBp = nullptr;
        const pc::Blueprint* pictureBp = nullptr;
        for (const pc::Blueprint& b : list) {
            if (b.title != "Preset Probe: Probe 100.5 MHz") { continue; }
            if (b.decoder.image) {
                pictureBp = &b;
            } else {
                textBp = &b;
            }
        }
        CHECK(list.size() == 2u);
        CHECK(textBp != nullptr);
        CHECK(pictureBp != nullptr);
        if (textBp == nullptr || pictureBp == nullptr) {
            return testSummary("test_patch_blueprints_app");
        }
        CHECK(textBp->tune.freqHz == 100.5e6);
        CHECK(textBp->tune.mode == -1);   // the preset leaves the mode alone

        // --- a pick takes over the starter radio ----------------------------------
        std::printf("  a pick takes over the starter radio and moves the view to it\n");
        Access::seed(app);
        CHECK(Access::graph(app).nodes().size() == 1u);
        const pc::NodeId starter = Access::graph(app).nodes()[0].id;
        // The generator, so the run below opens nothing on the desk.
        Access::graph(app).mutableNode(starter)->device = pc::kGeneratorKey;
        Access::pick(app, *pictureBp);
        CHECK(Access::graph(app).count(pc::NodeKind::Radio) == 1u);
        CHECK(Access::graph(app).find(starter)->device == pc::kGeneratorKey);
        CHECK(Access::note(app).find("Press START") != std::string::npos);
        CHECK(Access::note(app).find("(picture)") != std::string::npos);
        {
            // The whole workflow is in view: every node's box, through the
            // transform the canvas draws with, inside the canvas. And nothing
            // is selected, so no inspector covers its right-hand end.
            const cascade::gui::patch::View v =
                cascade::gui::patch::drawView(Access::view(app), cascade::gui::uiscale::factor());
            bool allIn = true;
            for (const pc::Node& n : Access::graph(app).nodes()) {
                const cascade::gui::patch::Vec2 a =
                    cascade::gui::patch::worldToScreen(v, cascade::gui::patch::Vec2{n.x, n.y});
                const cascade::gui::patch::Vec2 b = cascade::gui::patch::worldToScreen(
                    v, cascade::gui::patch::Vec2{n.x + n.w, n.y + n.h});
                if (a.x < 0.0f || a.y < 0.0f || b.x > Access::kCanvasW || b.y > Access::kCanvasH) {
                    allIn = false;
                    std::printf("    node %u off the canvas: %.0f,%.0f - %.0f,%.0f\n",
                                static_cast<unsigned>(n.id), a.x, a.y, b.x, b.y);
                }
            }
            CHECK(allIn);
            CHECK(Access::selected(app) == pc::kNoNode);
        }
        {
            const pc::Plan p = Access::plan(app);
            CHECK(p.decoders.size() == 1u);
            CHECK(p.runnable);
            for (const pc::NodeProblem& np : p.problems) {
                CHECK(np.problem != pc::Problem::PluginMissing);
                CHECK(pc::isAdvisory(np.problem));
            }
        }

        // --- and it RUNS with the plugin still stopped -------------------------------
        std::printf("  START runs the stopped plugin's decoder in the patch\n");
        fs::remove(eventLog);
        Access::pressStart(app);
        bool created = false;
        const auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(20)) {
            Access::frame(app);
            if (slurp(eventLog).find("preset create") != std::string::npos) {
                created = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(created);
        CHECK(Access::refused(app) == 0u);
        CHECK(Access::stopped(app, key));   // running in the patch did not un-stop it
        Access::pressStart(app);            // the STOP key
        Access::frame(app);
        CHECK(!Access::running(app));

        // --- a second pick gets a radio of its own -----------------------------------
        std::printf("  a second pick gets a free device, not the generator\n");
        Access::pick(app, *textBp);
        CHECK(Access::graph(app).count(pc::NodeKind::Radio) == 2u);
        std::string secondDevice;
        for (const pc::Node& n : Access::graph(app).nodes()) {
            if (n.kind == pc::NodeKind::Radio && n.id != starter) { secondDevice = n.device; }
        }
        CHECK(!secondDevice.empty());
        CHECK(!pc::isGeneratorKey(secondDevice));
        CHECK(secondDevice.find("00000001") != std::string::npos);
        // Both of the module's decoders take audio, so each workflow has its
        // own channel and demodulator.
        CHECK(Access::graph(app).count(pc::NodeKind::Channel) == 2u);
        CHECK(Access::graph(app).count(pc::NodeKind::Demod) == 2u);

        // --- LISTEN TO THE TOWER, from the built-in airport table ---------------------
        std::printf("  with a receiver position the list offers the nearest towers\n");
        {
            // No position and no airport looked up: no tower in the list.
            std::size_t towers = 0;
            for (const pc::Blueprint& b : Access::blueprints(app)) {
                if (!b.listen.empty()) { ++towers; }
            }
            CHECK(towers == 0u);
        }
        // Beside Heathrow (51.4700 N, 0.4543 W).
        Access::setPosition(app, 51.4700, -0.4543);
        const std::vector<pc::Blueprint> withTowers = Access::blueprints(app);
        const pc::Blueprint* egll = nullptr;
        std::size_t towers = 0;
        for (const pc::Blueprint& b : withTowers) {
            if (b.listen.empty()) { continue; }
            ++towers;
            CHECK(b.title.rfind("Listen to the tower: ", 0) == 0);
            if (b.title.find("EGLL") != std::string::npos) { egll = &b; }
        }
        std::printf("    %zu tower blueprint(s) offered\n", towers);
        CHECK(towers >= 1u && towers <= 6u);
        CHECK(egll != nullptr);
        if (egll != nullptr) {
            // Heathrow's tower channels are airband: 118-137 MHz, named TWR.
            for (const pc::ListenChannel& c : egll->listen) {
                CHECK(c.hz >= 118.0e6 && c.hz <= 137.0e6);
                CHECK(c.name.rfind("TWR ", 0) == 0);
            }
            const std::size_t before = Access::graph(app).nodes().size();
            Access::pick(app, *egll);
            CHECK(Access::graph(app).count(pc::NodeKind::Radio) == 3u);   // a radio of its own
            CHECK(Access::graph(app).nodes().size() > before);
            std::size_t speakers = 0;
            for (const pc::Node& n : Access::graph(app).nodes()) {
                if (n.kind == pc::NodeKind::Sink && n.device == "speakers") { ++speakers; }
            }
            CHECK(speakers >= 1u);
            CHECK(Access::note(app).find("Listen to the tower: EGLL") != std::string::npos);
            std::printf("    Heathrow: %zu tower channel(s), %zu speaker(s) placed\n",
                        egll->listen.size(), speakers);
        }

        // --- five radios, all busy: refused, said, nothing placed ---------------------
        std::printf("  with five busy radios a pick places nothing and says why\n");
        while (Access::graph(app).count(pc::NodeKind::Radio) < pc::kMaxRadios) {
            const pc::NodeId r = Access::graph(app).addNode(pc::NodeKind::Radio, "Radio");
            const pc::NodeId s =
                Access::graph(app).addNode(pc::NodeKind::Display, "Spectrum", pc::PortType::Iq);
            Access::graph(app).connect(r, 0, s, 0);
        }
        const std::size_t before = Access::graph(app).nodes().size();
        Access::pick(app, *textBp);
        CHECK(Access::graph(app).nodes().size() == before);
        CHECK(Access::note(app).find("at most 5") != std::string::npos);
    }

    std::error_code ec;
    fs::remove_all(scratch, ec);
    return testSummary("test_patch_blueprints_app");
}
