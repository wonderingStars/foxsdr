// THE PATCH PRESETS, THROUGH THE REAL AppWindow: what saving and loading a
// preset does to the patch on the canvas, to its radios and to the
// "(previous patch)" slot.
//
// test_patch_presets proves the store's rules. This proves the application
// USES them safely:
//   - save then load gives back the very graph that was saved (compared as
//     patch_io writes it);
//   - loading while the patch is running stops it through its own STOP -
//     every radio closed, none left open for a node the new patch lacks - and
//     leaves the new patch stopped until START is pressed;
//   - the "(previous patch)" slot holds what the load replaced, and loading
//     it puts that patch back;
//   - a preset naming a radio that is not plugged in loads exactly as the
//     start-up restore loads one: the load succeeds, and the radio only shows
//     as unavailable once START tries it;
//   - a text that is not a patch changes nothing;
//   - the presets reach currentConfig() and come back through applyConfig().
//
// CONVERTED FOR THE ENGINE'S TABLE (stub campaign B17; the 0.99.84 house pattern). Master's test built the patch in the window's own
// graph and ran its radios from the window; here the patch is the ENGINE's and the page edits a draft it commits through the session. So:
//   - a patch is BUILT in the page's draft and committed (commitPatchDraft) as the page does, and the engine's pass takes it - one
//     frame of the engine's part (look()) then the page's own follow (sync) - and what "the canvas" holds is the draft (serialisePatchNow);
//   - START is the command the transport sends (FOXAPI_OP_PATCH_RUN 1) and "running" is the state the window reads (the published
//     FOXAPI_RX_PATCH_RUNNING flag), after the frames that land it; the radios the patch holds are the ENGINE's (th::engine, read on the
//     pass thread through tests/pass_seam.hpp);
//   - the fake radios are installed where the ENGINE constructs a driver (Engine::testHooks_), not the window.
// What each scenario asserts is unchanged; where a master assertion read a state the window held the instant a key was pressed, it
// is made after the frames that carry the key to the engine, and says so.
//
// Hermetic: no config file is read or written (empty config path), the
// per-user directories point at a scratch folder, no USB enumeration runs
// and no real driver is ever constructed. No ImGui frame is rendered.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/app_commands.hpp"
#include "core/patch_devices.hpp"
#include "core/patch_io.hpp"
#include "core/patch_presets.hpp"
#include "engine/engine.hpp"
#include "gui/app_window.hpp"
#include "test_host.hpp"
#include "pass_seam.hpp"
#include "source/device_source.hpp"
#include "test_check.hpp"

namespace pc = cascade::core::patch;
using cascade::core::PatchPresetStatus;

namespace {

// --- the recording radios ------------------------------------------------------

struct Record {
    std::string kind;
    std::string args;
    bool opened = false;
    bool started = false;
    bool stopped = false;
    bool closed = false;
};

struct Registry {
    std::mutex m;
    std::vector<std::shared_ptr<Record>> made;
    std::vector<cascade::source::NativeDeviceInfo> native;
};
Registry g_reg;

class FakeRadio final : public cascade::source::DeviceSource {
public:
    FakeRadio(std::string kind, std::shared_ptr<Record> rec)
        : kind_(std::move(kind)), rec_(std::move(rec)) {}
    ~FakeRadio() override {
        std::lock_guard<std::mutex> lk(g_reg.m);
        rec_->closed = true;
    }

    bool start() override {
        {
            std::lock_guard<std::mutex> lk(g_reg.m);
            rec_->started = true;
        }
        abort_ = false;
        return true;
    }
    void stop() override {
        {
            std::lock_guard<std::mutex> lk(g_reg.m);
            rec_->stopped = true;
        }
        abort_ = true;
    }
    bool running() const override { return !abort_; }
    bool selfPaced() const override { return true; }
    double sampleRateHz() const override { return rate_; }
    bool setSampleRateHz(double hz) override {
        rate_ = hz;
        return true;
    }
    double centerFrequencyHz() const override { return centre_; }
    bool setCenterFrequencyHz(double hz) override {
        centre_ = hz;
        return true;
    }
    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (abort_) { return 0; }
        for (std::size_t i = 0; i < n; ++i) { dst[i] = {0.0f, 0.0f}; }
        return n;
    }
    const char* name() const override { return "Fake radio"; }
    const char* lastError() const override { return error_.c_str(); }

    const char* driverKey() const override { return kind_.c_str(); }
    bool open(const std::string& args) override {
        std::lock_guard<std::mutex> lk(g_reg.m);
        rec_->args = args;
        // An unplugged dongle: the driver finds nothing at that serial.
        if (args.find("ABSENT") != std::string::npos) {
            error_ = "no radio with that serial is connected";
            return false;
        }
        rec_->opened = true;
        open_ = true;
        return true;
    }
    void closeDevice() override {
        std::lock_guard<std::mutex> lk(g_reg.m);
        rec_->closed = true;
        open_ = false;
    }
    bool isOpen() const override { return open_; }
    std::vector<cascade::source::GainInfo> gains() const override { return {}; }
    bool setGainDb(const std::string&, double) override { return false; }
    double gainDb(const std::string&) const override { return 0.0; }
    bool autoGainSupported() const override { return false; }
    bool setAutoGain(bool) override { return false; }
    bool autoGain() const override { return false; }
    std::vector<std::string> antennas() const override { return {}; }
    bool setAntenna(const std::string&) override { return false; }
    std::string antenna() const override { return {}; }
    std::vector<double> supportedSampleRatesHz() const override { return {2.4e6}; }
    bool frequencyRangeHz(double& lo, double& hi) const override {
        lo = 24.0e6;
        hi = 1766.0e6;
        return true;
    }
    bool deviceDead() const override { return false; }
    std::string faultedWhile() const override { return {}; }

private:
    std::string kind_;
    std::shared_ptr<Record> rec_;
    std::atomic<bool> abort_{true};
    double rate_ = 2.4e6;
    double centre_ = 100.0e6;
    bool open_ = false;
    std::string error_;
};

std::unique_ptr<cascade::source::DeviceSource> makeFake(const std::string& kind) {
    auto rec = std::make_shared<Record>();
    rec->kind = kind;
    {
        std::lock_guard<std::mutex> lk(g_reg.m);
        g_reg.made.push_back(rec);
    }
    return std::make_unique<FakeRadio>(kind, rec);
}

std::vector<cascade::source::NativeDeviceInfo> fakeScan() {
    std::lock_guard<std::mutex> lk(g_reg.m);
    return g_reg.native;
}

std::size_t madeCount() {
    std::lock_guard<std::mutex> lk(g_reg.m);
    return g_reg.made.size();
}

Record made(std::size_t i) {
    std::lock_guard<std::mutex> lk(g_reg.m);
    return i < g_reg.made.size() ? *g_reg.made[i] : Record{};
}

const std::string kArgsA = "serial=0000000A";
const std::string kArgsB = "serial=0000000B";
const std::string kArgsAbsent = "serial=ABSENT01";

void resetRegistry() {
    std::lock_guard<std::mutex> lk(g_reg.m);
    g_reg.made.clear();
    // Two dongles plugged in; the ABSENT one is not.
    g_reg.native = {{"rtlsdr", "Generic RTL2832U A", kArgsA},
                    {"rtlsdr", "Generic RTL2832U B", kArgsB}};
}

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

std::filesystem::path g_scratch;

void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = std::filesystem::temp_directory_path() /
                ("foxsdr_patch_presets_app_" + std::to_string(pid));
    std::filesystem::create_directories(g_scratch);
    const std::string s = g_scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, s);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
}

}  // namespace

namespace cascade::gui {

// The friend AppWindow names for this test (see AppWindow::testHooks_ / Engine::testHooks_).
struct AppWindowTestAccess {
    static void installHooks() {
        cascade::engine::Engine::testHooks_.makeDevice = &makeFake;
        cascade::engine::Engine::testHooks_.nativeScan = &fakeScan;
    }

    // ONE FRAME'S ENGINE PART, in drawUi's order (as tests/test_patch_graph_draft_input.cpp's look): the commands the last frame queued are
    // applied by the pass, the page-open flag and the once-a-frame commit, the patch step, the publish, the window's read of it and of the
    // session's answers. No view is drawn.
    static void look(AppWindow& a) {
        passseam::pass(th::engine(a), [&] {
            th::engine(a).pumpFrameBegin();
            th::engine(a).drainLocalCommands();
            a.syncPatchPageOpen();
            a.commitPatchDraft();
            th::engine(a).pumpInputs();
            th::engine(a).pumpPlugins();
            th::engine(a).pumpPatch();
            (void)th::engine(a).pumpPublish();
        }, [&] {
            a.syncPatchPageOpen();
            a.commitPatchDraft();
        });
        a.statusLines_ = th::engine(a).statusText();
        a.refreshUiState();
        a.pumpTransmitKey();
    }
    // A frame of the page that draws nothing: the engine's part, then the page's follow of what the engine published.
    static void frame(AppWindow& a) {
        look(a);
        a.syncPatchDraft();
        a.commitPatchDraft();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    static void frames(AppWindow& a, int n) {
        for (int i = 0; i < n; ++i) { frame(a); }
    }
    // The page is up on the patch face and has read its first publish (the draft starts from the engine's graph).
    static void openPage(AppWindow& a) {
        a.setMainViewPatch(true);
        a.patchSeeded_ = true;  // no starter patch: this test builds its own
        frames(a, 3);
    }

    // A patch built on the canvas as the parts bin would build it: a radio on `device` at `hz`, a channel and a demodulator wired behind
    // it, in the page's draft, committed and taken by the engine. Node ids are the draft's (they run 1, 2, 3 in a fresh window and on
    // from there after deletes: a document is read back under ids of its own, see canon() below).
    static pc::NodeId buildPatch(AppWindow& a, const std::string& device, double hz) {
        a.patchSeeded_ = true;
        const pc::NodeId r = a.patchDraft_.addNode(pc::NodeKind::Radio, "Radio", pc::PortType::Iq, 0.0f, 0.0f);
        const pc::NodeId c = a.patchDraft_.addNode(pc::NodeKind::Channel, "Tower", pc::PortType::Iq, 260.0f, 40.0f);
        const pc::NodeId d = a.patchDraft_.addNode(pc::NodeKind::Demod, "AM", pc::PortType::Iq, 520.0f, 40.0f);
        if (pc::Node* n = a.patchDraft_.mutableNode(r)) {
            n->device = device;
            n->freqHz = hz;
            n->rateHz = 2.4e6;
            n->on = true;
        }
        if (pc::Node* n = a.patchDraft_.mutableNode(c)) { n->freqHz = hz + 25000.0; }
        CHECK(a.patchDraft_.connect(r, 0, c, 0) == pc::Connect::Ok);
        CHECK(a.patchDraft_.connect(c, 0, d, 0) == pc::Connect::Ok);
        a.patchUi_.view.pan = cascade::gui::patch::Vec2{-40.0f, 12.5f};
        a.patchUi_.view.zoom = 1.25f;
        a.patchUi_.dirty = true;
        // To the engine: the commit goes out, the pass takes it, the answer comes back and the page follows.
        frames(a, 4);
        return r;
    }
    static void clearPatch(AppWindow& a) {
        while (!a.patchDraft_.nodes().empty()) { a.patchDraft_.removeNode(a.patchDraft_.nodes().front().id); }
        frames(a, 4);
    }
    static std::string now(AppWindow& a) { return a.serialisePatchNow(); }
    static std::size_t nodeCount(AppWindow& a) { return a.patchDraft_.nodes().size(); }
    // The ENGINE's graph: a copy, made with the pass out of the way.
    static std::size_t engineNodeCount(AppWindow& a) {
        return passseam::read<std::size_t>(th::engine(a), [&] { return th::engine(a).patchGraph_.nodes().size(); });
    }
    // How many patch DOCUMENTS the engine has loaded (fact.patch.epoch): a preset's load is one - a new patch, its own epoch, so no edit of
    // the old patch is ever merged into it - and an ordinary edit of the canvas is not.
    static std::uint64_t engineEpoch(AppWindow& a) {
        return passseam::read<std::uint64_t>(th::engine(a), [&] { return th::engine(a).patchGraphEpoch_; });
    }
    static pc::NodeId firstRadio(AppWindow& a) {
        for (const pc::Node& n : a.patchDraft_.nodes()) {
            if (n.kind == pc::NodeKind::Radio) { return n.id; }
        }
        return pc::kNoNode;
    }

    static PatchPresetStatus save(AppWindow& a, const std::string& name, bool overwrite) {
        return a.savePatchPreset(name, overwrite);
    }
    // The preset's text by name, as the panel hands it over; empty if none.
    static std::string presetText(AppWindow& a, const std::string& name) {
        const int i = a.patchPresets_.find(name);
        return i < 0 ? std::string() : a.patchPresets_.list()[static_cast<std::size_t>(i)].text;
    }
    // LOAD, as the panel presses it: the keys' commands are queued, the frames after it carry them (callers run frames()).
    static bool load(AppWindow& a, const std::string& name) {
        const int i = a.patchPresets_.find(name);
        return i >= 0 && a.loadPatchPreset(a.patchPresets_.list()[static_cast<std::size_t>(i)].text);
    }
    static bool loadText(AppWindow& a, const std::string& text) { return a.loadPatchPreset(text); }
    static bool loadPrevious(AppWindow& a) { return a.loadPatchPreset(a.patchPresets_.previous()); }
    static std::string previous(AppWindow& a) { return a.patchPresets_.previous(); }
    static std::size_t presetCount(AppWindow& a) { return a.patchPresets_.list().size(); }

    // START as the transport's dome sends it (drawPatchTransport): the command, queued; the pass applies it on the next frame.
    static void pressStartOnly(AppWindow& a) { a.uiSubmit(cascade::core::cmd::makeInt(FOXAPI_OP_PATCH_RUN, 1)); }
    static void pressStart(AppWindow& a) {
        pressStartOnly(a);
        frames(a, 2);
    }
    // Frames until `id`'s radio is open (or answered why not), or 20 s.
    static bool runUntilSettled(AppWindow& a, pc::NodeId id) {
        const auto t0 = std::chrono::steady_clock::now();
        while (!radioOpen(a, id) && radioError(a, id).empty()) {
            frame(a);
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(20)) { return false; }
        }
        return true;
    }
    // Frames until `pred` holds, or 20 s (a radio closing is the engine's worker's, so it takes frames, not a fixed count).
    static bool framesUntil(AppWindow& a, const std::function<bool()>& pred) {
        const auto t0 = std::chrono::steady_clock::now();
        while (!pred()) {
            frame(a);
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(20)) { return false; }
        }
        return true;
    }
    // What the window reads: the published flag the transport's dome is lettered from.
    static bool running(AppWindow& a) { return (a.uiState_.flags & FOXAPI_RX_PATCH_RUNNING) != 0u; }
    static std::size_t radiosOpen(AppWindow& a) {
        return passseam::read<std::size_t>(th::engine(a), [&] {
            return th::engine(a).patchRadios_.size() + th::engine(a).patchRadioPending_.size();
        });
    }
    static bool radioOpen(AppWindow& a, pc::NodeId id) {
        return passseam::read<bool>(th::engine(a), [&] { return th::engine(a).patchRadios_.count(id) != 0; });
    }
    static std::string radioError(AppWindow& a, pc::NodeId id) {
        return passseam::read<std::string>(th::engine(a), [&] {
            const auto it = th::engine(a).patchRadioError_.find(id);
            return it == th::engine(a).patchRadioError_.end() ? std::string() : it->second;
        });
    }

    static cascade::core::AppConfig config(AppWindow& a) { return a.currentConfig(); }
    static void restore(AppWindow& a, const cascade::core::AppConfig& cfg) { a.applyConfig(cfg); }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

namespace {

// A document as patch_io reads it back. parse() gives the nodes ids of its
// own (1, 2, 3...) - by design, see core/patch_io.hpp - so a patch built on a
// canvas where nodes were deleted before (ids 4, 5, 6) loads as the SAME
// graph under new numbers. This is that graph written out: what a load of
// `text` must put on the canvas, byte for byte.
std::string canon(const std::string& text) {
    pc::LoadResult r = pc::parse(text);
    CHECK(r.ok);
    CHECK(r.dropped == 0);
    return pc::serialise(r.graph, r.panX, r.panY, r.zoom);
}

const std::string kDevA = pc::makeDeviceKey("rtlsdr", kArgsA);
const std::string kDevB = pc::makeDeviceKey("rtlsdr", kArgsB);
const std::string kDevAbsent = pc::makeDeviceKey("rtlsdr", kArgsAbsent);

void testSaveThenLoad() {
    std::printf("  save then load gives back the identical graph\n");
    resetRegistry();
    th::TestWindow app;
    Access::openPage(app);
    Access::buildPatch(app, kDevA, 118.0e6);
    const std::string saved = Access::now(app);
    CHECK(Access::save(app, "Airband", false) == PatchPresetStatus::Saved);
    CHECK(Access::presetText(app, "airband") == saved);

    // The canvas moves on: a different patch entirely.
    Access::clearPatch(app);
    Access::buildPatch(app, kDevB, 156.8e6);
    CHECK(Access::now(app) != saved);

    const std::uint64_t epochBefore = Access::engineEpoch(app);
    CHECK(Access::load(app, "AIRBAND"));
    // At once, in the page's own draft (what a save taken now would keep)...
    CHECK(Access::now(app) == saved);
    CHECK(Access::nodeCount(app) == 3u);
    // ...and after the frames that carry the load to the engine and back: the engine holds the same patch, and the page still shows it.
    Access::frames(app, 6);
    CHECK(Access::now(app) == saved);
    CHECK(Access::engineNodeCount(app) == 3u);
    CHECK(!Access::running(app));
    // A NEW PATCH, not an edit of the old one: the engine took it as a document (its own epoch).
    CHECK(Access::engineEpoch(app) == epochBefore + 1u);

    // Saving under the same name again asks first: refused until told.
    Access::clearPatch(app);
    Access::buildPatch(app, kDevB, 430.0e6);
    CHECK(Access::save(app, "airband", false) == PatchPresetStatus::Exists);
    CHECK(Access::presetText(app, "Airband") == saved);
    CHECK(Access::save(app, "airband", true) == PatchPresetStatus::Overwritten);
    CHECK(Access::presetText(app, "Airband") == Access::now(app));
    CHECK(Access::presetCount(app) == 1u);

    // A text that is not a patch changes nothing - not the canvas, not the slot.
    const std::string before = Access::now(app);
    const std::string prevBefore = Access::previous(app);
    CHECK(!Access::loadText(app, "this is not a patch"));
    Access::frames(app, 4);
    CHECK(Access::now(app) == before);
    CHECK(Access::previous(app) == prevBefore);
    CHECK(Access::engineNodeCount(app) == 3u);

    // The presets and the slot reach the config, and come back from it.
    const cascade::core::AppConfig cfg = Access::config(app);
    CHECK(cfg.patchPresets.size() == 1u);
    CHECK(cfg.patchPresetPrevious == Access::previous(app));
    th::TestWindow again;
    Access::restore(again, cfg);
    CHECK(Access::presetText(again, "Airband") == Access::presetText(app, "Airband"));
    CHECK(Access::previous(again) == Access::previous(app));
    CHECK(Access::load(again, "Airband"));
    // This preset was saved from a canvas whose nodes had been renumbered
    // by the deletes above: the same graph comes back under ids 1, 2, 3.
    CHECK(Access::presetText(app, "Airband") != canon(Access::presetText(app, "Airband")));
    CHECK(Access::now(again) == canon(Access::presetText(app, "Airband")));
}

void testLoadWhileRunning() {
    std::printf("  loading while running stops the patch and leaves it stopped\n");
    resetRegistry();
    th::TestWindow app;
    Access::openPage(app);
    // The preset to load: a patch on radio B.
    Access::buildPatch(app, kDevB, 162.0e6);
    CHECK(Access::save(app, "Marine", false) == PatchPresetStatus::Saved);
    Access::clearPatch(app);

    // The patch on the canvas runs radio A.
    const std::size_t first = madeCount();
    const pc::NodeId a = Access::buildPatch(app, kDevA, 118.0e6);
    Access::pressStart(app);
    CHECK(Access::runUntilSettled(app, a));
    CHECK(Access::running(app));
    CHECK(Access::radioOpen(app, a));
    CHECK(madeCount() == first + 1);
    CHECK(made(first).args == kArgsA);
    CHECK(made(first).started);

    CHECK(Access::load(app, "Marine"));
    // Stopped through the patch's own STOP: radio A stopped and closed, no
    // patch radio left open, and the transport says STOPPED. (The pass
    // applies the STOP and the new document on the next frame; the radio's
    // close is the engine's worker's, so the frames run until it has.)
    CHECK(Access::framesUntil(app, [&] { return Access::radiosOpen(app) == 0u && !Access::running(app); }));
    CHECK(!Access::running(app));
    CHECK(Access::radiosOpen(app) == 0u);
    CHECK(Access::framesUntil(app, [&] { return made(first).closed; }));
    CHECK(made(first).stopped);
    CHECK(made(first).closed);
    // ...and it STAYS stopped: frames go by and nothing opens - not radio B,
    // which the new patch names, and not radio A, which it does not.
    Access::frames(app, 10);
    CHECK(!Access::running(app));
    CHECK(Access::radiosOpen(app) == 0u);
    CHECK(madeCount() == first + 1);
    // Only START opens the new patch's radio, and it is B.
    const pc::NodeId b = Access::firstRadio(app);
    Access::pressStart(app);
    CHECK(Access::runUntilSettled(app, b));
    CHECK(Access::radioOpen(app, b));
    CHECK(madeCount() == first + 2);
    CHECK(made(first + 1).args == kArgsB);

    // A load of a running patch again, then START pressed but not yet acted on (the same frame): a load still leaves the patch stopped
    // with nothing open - the START the engine had not yet applied lands first, and the STOP the load queued follows it.
    CHECK(Access::load(app, "Marine"));
    CHECK(Access::framesUntil(app, [&] { return Access::radiosOpen(app) == 0u && !Access::running(app); }));
    CHECK(Access::framesUntil(app, [&] { return made(first + 1).closed; }));
    CHECK(!Access::running(app));
    CHECK(Access::radiosOpen(app) == 0u);
    CHECK(made(first + 1).closed);
    Access::pressStartOnly(app);
    CHECK(Access::load(app, "Marine"));
    Access::frames(app, 8);
    CHECK(!Access::running(app));
    CHECK(Access::radiosOpen(app) == 0u);
    // Not one radio was made for it: the patch was never running when the pass reached the radios.
    CHECK(madeCount() == first + 2);
}

void testPreviousSlot() {
    std::printf("  the (previous patch) slot holds what the load replaced\n");
    resetRegistry();
    th::TestWindow app;
    Access::openPage(app);
    Access::buildPatch(app, kDevB, 145.5e6);
    CHECK(Access::save(app, "Two metres", false) == PatchPresetStatus::Saved);
    const std::string preset = Access::now(app);
    Access::clearPatch(app);

    Access::buildPatch(app, kDevA, 121.5e6);
    const std::string mine = Access::now(app);
    CHECK(Access::previous(app).empty());

    CHECK(Access::load(app, "Two metres"));
    CHECK(Access::previous(app) == mine);
    CHECK(Access::now(app) == preset);
    // Not one of the presets, and not counted with them.
    CHECK(Access::presetCount(app) == 1u);

    // One mis-click undone: the slot's patch comes back, and the slot now
    // holds the preset it displaced.
    CHECK(Access::loadPrevious(app));
    CHECK(Access::now(app) == canon(mine));
    CHECK(Access::previous(app) == preset);
    Access::frames(app, 6);
    CHECK(Access::now(app) == canon(mine));
}

void testAbsentRadio() {
    std::printf("  a preset naming a radio that is not plugged in loads like the restore\n");
    resetRegistry();
    th::TestWindow app;
    Access::openPage(app);
    Access::buildPatch(app, kDevAbsent, 433.9e6);
    CHECK(Access::save(app, "Somewhere else", false) == PatchPresetStatus::Saved);
    const std::string text = Access::presetText(app, "Somewhere else");
    Access::clearPatch(app);
    Access::buildPatch(app, kDevA, 118.0e6);

    const std::size_t first = madeCount();
    CHECK(Access::load(app, "Somewhere else"));
    Access::frames(app, 6);
    CHECK(Access::now(app) == text);
    CHECK(!Access::running(app));
    CHECK(madeCount() == first);  // loading opened nothing

    // The same document through the start-up restore: the same patch.
    th::TestWindow restored;
    cascade::core::AppConfig cfg;
    cfg.patch = text;
    Access::restore(restored, cfg);
    CHECK(Access::now(restored) == Access::now(app));

    // START tries it, and the part says it is unavailable - nothing more.
    const pc::NodeId id = Access::firstRadio(app);
    Access::pressStart(app);
    CHECK(Access::runUntilSettled(app, id));
    CHECK(!Access::radioOpen(app, id));
    CHECK(!Access::radioError(app, id).empty());
    CHECK(Access::running(app));
    CHECK(Access::nodeCount(app) == 3u);
}

}  // namespace

int main() {
    isolate();
    Access::installHooks();

    testSaveThenLoad();
    testLoadWhileRunning();
    testPreviousSlot();
    testAbsentRadio();

    std::error_code ec;
    std::filesystem::remove_all(g_scratch, ec);
    return testSummary("test_patch_presets_app");
}
