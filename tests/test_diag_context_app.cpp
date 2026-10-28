// THE CONTEXT BLOCK OF A CRASH OR FREEZE REPORT FOLLOWS THE SESSION, through the
// REAL AppWindow.
//
// THE FIELD REPORTS. Three crash reports from 0.99.59 carried a context block
// that said `source siggen`, `deviceOpen false`, `sampleRate 2e+06` and no radio
// model, while the log tail of the SAME report said a radio had just opened:
//   (a) `source: opened miri (soapy) at 2000000 S/s`, one second before the fault;
//   (b) `source: opened ADALM-Pluto (network) (pluto) at 30720000 S/s` - the rate
//       that was the whole cause of that crash, and nowhere in the context;
//   (c) `patch: radio node 4 running SDRplay RSP1A (SoapySDR) at 2000000 S/s` - a
//       radio the PATCH page opened, which the block knows nothing about.
//
// WHY. A fault handler cannot compute anything (docs/DIAGNOSTICS.md, "What a
// fault handler is allowed to do"), so the block is rendered on the healthy path
// and the handler writes the bytes out. That render ran from three places: the
// start of run(), the Copy diagnostics bundle, and once per 60 frames in the
// frame loop - a snapshot at most a second old, and exactly as old as the open
// the fault came right after (the first read of a new stream is the most
// fault-prone moment of a radio's life). The patch page's radios were not in the
// block at all.
//
// WHAT THIS HOLDS. After every event that changes what the block says - a radio
// opened from the Source list (the asynchronous path), a failed open, a rate
// change by the browser's route, a mode change by the button's route AND the
// browser's, a swap back to the generator, a patch radio starting and stopping -
// the block that a crash handler would write out (read here with
// diagContextBlock(), with NO refresh in between) equals the window's live
// state. No frame is drawn: a window cannot be run from ctest, and a fault does
// not wait for the next frame either.
//
// And the structural half, because a mechanism that depends on every writer
// remembering a call is the one that fails the next time somebody adds a writer:
// the source kind and the mode are each assigned in exactly ONE function, and
// that function refreshes the block.
//
// Hermetic like test_ppm_app: no config file, the per-user directories in a
// scratch folder, no USB walk, no real driver.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/diag_report.hpp"
#include "core/app_commands.hpp"
#include "engine/engine.hpp"
#include "gui/app_window.hpp"
#include "test_host.hpp"
#include "pass_seam.hpp"
#include "net/web_control.hpp"
#include "source/device_source.hpp"
#include "test_check.hpp"

namespace {

// --- the fake radios ---------------------------------------------------------
//
// "rtlsdr" opens and runs; "hackrf" REFUSES to open (the failed-open case). Both
// coerce any rate they are asked for to the nearest of their own, the way a real
// dongle's clock does - so the rate a radio really runs at differs from the
// generator's 2 MS/s and from what was asked, and a context that guessed either
// one is wrong.
struct Reg {
    std::mutex m;
    std::vector<cascade::source::NativeDeviceInfo> native;
};
Reg g_reg;

double nearestRate(double want) {
    static const double kRates[] = {1.024e6, 2.048e6, 2.4e6, 3.2e6};
    double best = kRates[0];
    for (double r : kRates) {
        if (std::fabs(r - want) < std::fabs(best - want)) { best = r; }
    }
    return best;
}

class FakeRadio final : public cascade::source::DeviceSource {
public:
    explicit FakeRadio(std::string kind) : kind_(std::move(kind)) {}

    bool start() override {
        abort_ = false;
        return true;
    }
    void stop() override { abort_ = true; }
    bool running() const override { return !abort_; }
    bool selfPaced() const override { return true; }
    double sampleRateHz() const override { return rate_; }
    bool setSampleRateHz(double hz) override {
        rate_ = nearestRate(hz);
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
    bool open(const std::string&) override {
        if (kind_ == "hackrf") {
            error_ = "the fake hackrf refuses to open";
            return false;
        }
        open_ = true;
        return true;
    }
    void closeDevice() override { open_ = false; }
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
    std::vector<double> supportedSampleRatesHz() const override { return {2.4e6, 2.048e6}; }
    bool frequencyRangeHz(double& lo, double& hi) const override {
        lo = 24.0e6;
        hi = 1766.0e6;
        return true;
    }
    bool deviceDead() const override { return false; }
    std::string faultedWhile() const override { return {}; }

private:
    std::string kind_;
    std::string error_;
    std::atomic<bool> abort_{true};
    double rate_ = 2.4e6;
    double centre_ = 100.0e6;
    bool open_ = false;
};

std::unique_ptr<cascade::source::DeviceSource> makeFake(const std::string& kind) {
    return std::make_unique<FakeRadio>(kind);
}

std::vector<cascade::source::NativeDeviceInfo> fakeScan() {
    std::lock_guard<std::mutex> lk(g_reg.m);
    return g_reg.native;
}

const std::string kRtlArgs = "serial=0000000A";
const std::string kRtlKey = "rtlsdr|" + kRtlArgs;
const std::string kHackArgs = "serial=abc";
const std::string kHackKey = "hackrf|" + kHackArgs;

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    const std::filesystem::path scratch = std::filesystem::temp_directory_path() /
                                          ("foxsdr_diag_context_app_" + std::to_string(pid));
    std::filesystem::create_directories(scratch);
    const std::string s = scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, s);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
}

}  // namespace

namespace cascade::gui {

struct AppWindowTestAccess {
    // EVERY READ AND WRITE OF THE ENGINE GOES THROUGH THE SEAM (tests/pass_seam.hpp), as tests/test_converter_app_paths.cpp does.
    static void installHooks() {
        cascade::engine::Engine::testHooks_.makeDevice = &makeFake;
        cascade::engine::Engine::testHooks_.nativeScan = &fakeScan;
    }
    // What the engine does once before the first pass - and nothing after it (Engine::refreshDiagContext renders the block; the pass's
    // own check - pumpDiagContext - is gated and hashed, and is what the cost test below does not measure).
    static void firstRefresh(AppWindow& a) { passseam::on(th::engine(a), [&] { th::engine(a).refreshDiagContext(); }); }

    // --- the engine's own state, read on the pass (COPIES) ---
    static std::string source(AppWindow& a) {
        return passseam::read<std::string>(th::engine(a), [&] { return th::engine(a).sourceKind_; });
    }
    static bool deviceOpen(AppWindow& a) {
        return passseam::read<bool>(th::engine(a), [&] { return th::engine(a).device_ != nullptr; });
    }
    static std::string model(AppWindow& a) {
        return passseam::read<std::string>(th::engine(a), [&] { return th::engine(a).deviceModel_; });
    }
    static double rate(AppWindow& a) {
        return passseam::read<double>(th::engine(a), [&] { return th::engine(a).pipeline_.activeSource().sampleRateHz(); });
    }
    static int modeIndex(AppWindow& a) {
        return passseam::read<int>(th::engine(a), [&] { return th::engine(a).modeIndex_; });
    }
    static std::vector<std::string> patchKinds(AppWindow& a) {
        return passseam::read<std::vector<std::string>>(th::engine(a), [&] {
            std::vector<std::string> kinds;
            for (const auto& [id, radio] : th::engine(a).patchRadios_) {
                (void)radio;
                const cascade::core::patch::Node* n = th::engine(a).patchGraph_.find(id);
                const std::string key = n != nullptr ? n->device : std::string("?");
                const std::size_t bar = key.find('|');
                kinds.push_back(bar == std::string::npos ? key : key.substr(0, bar));
            }
            std::sort(kinds.begin(), kinds.end());
            return kinds;
        });
    }

    // --- events ---
    static bool waitOpen(AppWindow& a) {
        const auto t0 = std::chrono::steady_clock::now();
        while (passseam::read<bool>(th::engine(a), [&] { return th::engine(a).deviceOpenPending_; })) {
            passseam::custom(th::engine(a), [&] { th::engine(a).pollSourceAsync(); });
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(20)) { return false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
    }
    static bool selectNative(AppWindow& a, const std::string& args) {
        int row = -1;
        passseam::on(th::engine(a), [&] {
            th::engine(a).scanNative();
            for (std::size_t i = 0; i < th::engine(a).nativeDevices_.size(); ++i) {
                if (th::engine(a).nativeDevices_[i].args == args) {
                    row = AppWindow::kNativeRowBase + static_cast<int>(i);
                    break;
                }
            }
            if (row >= 0) { th::engine(a).selectSource(row); }
        });
        return row >= 0 && waitOpen(a);
    }
    static void selectGenerator(AppWindow& a) { passseam::on(th::engine(a), [&] { th::engine(a).selectSource(0); }); }
    // THE BROWSER'S ROUTES are commands now (FOXAPI_OP_SET_SAMPLE_RATE / FOXAPI_OP_SET_MODE), applied by the engine.
    static void setRateByRemote(AppWindow& a, double hz) {
        passseam::on(th::engine(a), [&] {
            const FoxCommand c = cascade::core::cmd::makeNum(FOXAPI_OP_SET_SAMPLE_RATE, hz);
            (void)th::engine(a).applyCommand(c, std::string());
        });
    }
    static void setModeByButton(AppWindow& a, int index) { passseam::on(th::engine(a), [&] { th::engine(a).setModeIndex(index); }); }
    static void setModeByRemote(AppWindow& a, cascade::dsp::DemodMode m) {
        // FOXAPI_DEMOD_* is the mode keys' order, 1-based (the engine's kModeMap order); AM is the only mode this test asks for.
        const std::int64_t code = m == cascade::dsp::DemodMode::AM ? FOXAPI_DEMOD_AM : FOXAPI_DEMOD_NFM;
        passseam::on(th::engine(a), [&] {
            const FoxCommand c = cascade::core::cmd::makeInt(FOXAPI_OP_SET_MODE, code);
            (void)th::engine(a).applyCommand(c, std::string());
        });
    }
    static void rescan(AppWindow& a) { passseam::on(th::engine(a), [&] { th::engine(a).rescanPlugins(); }); }
    static void tuneBookmark(AppWindow& a, const cascade::core::Bookmark& b) {
        passseam::on(th::engine(a), [&] { th::engine(a).tuneToBookmark(b); });
    }

    static cascade::core::patch::NodeId addRadioNode(AppWindow& a, const std::string& device) {
        namespace pc = cascade::core::patch;
        a.patchSeeded_ = true;
        return passseam::read<pc::NodeId>(th::engine(a), [&] {
            const pc::NodeId id = th::engine(a).patchGraph_.addNode(pc::NodeKind::Radio, "Radio");
            if (pc::Node* n = th::engine(a).patchGraph_.mutableNode(id)) {
                n->device = device;
                n->freqHz = 100.0e6;
                n->centreChosen = true;
                n->rateHz = 2.4e6;
                n->on = true;
            }
            return id;
        });
    }
    static bool runPatchUntilOpen(AppWindow& a, cascade::core::patch::NodeId id) {
        passseam::on(th::engine(a), [&] {
            th::engine(a).patchRunning_ = true;
            th::engine(a).patchWasOpen_ = true;
        });
        const auto t0 = std::chrono::steady_clock::now();
        while (passseam::read<bool>(th::engine(a), [&] { return th::engine(a).patchRadios_.count(id) == 0; })) {
            passseam::custom(th::engine(a), [&] { th::engine(a).patchReconcile(); });
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(20)) { return false; }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
    }
    static void switchNodeOff(AppWindow& a, cascade::core::patch::NodeId id) {
        passseam::on(th::engine(a), [&] {
            if (cascade::core::patch::Node* n = th::engine(a).patchGraph_.mutableNode(id)) { n->on = false; }
            th::engine(a).patchReconcile();
        });
    }
    static void stopPatch(AppWindow& a) {
        passseam::on(th::engine(a), [&] {
            th::engine(a).patchRunning_ = false;
            th::engine(a).patchStopAll(true);
        });
        waitOpen(a);
    }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;
namespace fs = std::filesystem;

namespace {

// "name: value" lines of the block, first occurrence; "(missing)" when absent.
std::string field(const std::string& block, const char* name) {
    const std::string key = std::string("\n") + name + ": ";
    const std::size_t at = ("\n" + block).find(key);
    if (at == std::string::npos) { return "(missing)"; }
    const std::string b = "\n" + block;
    const std::size_t from = at + key.size();
    const std::size_t eol = b.find('\n', from);
    return b.substr(from, eol == std::string::npos ? std::string::npos : eol - from);
}

std::string rateText(double hz) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.0f", hz);
    return buf;
}

std::string patchText(const std::vector<std::string>& kinds) {
    if (kinds.empty()) { return "none"; }
    std::string s = std::to_string(kinds.size()) + " (";
    for (std::size_t i = 0; i < kinds.size(); ++i) { s += (i ? ", " : "") + kinds[i]; }
    return s + ")";
}

const char* const kModeNames[8] = {"NFM", "WFM", "AM", "DSB", "USB", "CW", "LSB", "RAW"};

// THE ASSERTION: the block a crash handler would write out right now, with no
// refresh since the event, equals the window's live state.
void expectBlockMatchesLive(cascade::gui::AppWindow& app, const char* event) {
    const std::string block = cascade::core::diagContextBlock();
    const std::string live = Access::model(app).empty() ? "(none)" : Access::model(app);
    std::printf("  %-34s source=%s open=%s rate=%s mode=%s patch=%s\n", event,
                field(block, "source").c_str(), field(block, "device-open").c_str(),
                field(block, "sample-rate").c_str(), field(block, "mode").c_str(),
                field(block, "patch-radios").c_str());
    CHECK(field(block, "source") == Access::source(app));
    CHECK(field(block, "device-open") == (Access::deviceOpen(app) ? "yes" : "no"));
    CHECK(field(block, "sdr-model") == live);
    CHECK(field(block, "sample-rate") == rateText(Access::rate(app)));
    CHECK(field(block, "mode") == kModeNames[Access::modeIndex(app)]);
    CHECK(field(block, "patch-radios") == patchText(Access::patchKinds(app)));
}

std::string readText(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The text of the function whose definition begins with `signature` (up to the
// closing brace in column 0), or "" when it is not in `text`.
std::string functionBody(const std::string& text, const std::string& signature) {
    const std::size_t at = text.find(signature);
    if (at == std::string::npos) { return std::string(); }
    const std::size_t end = text.find("\n}\n", at);
    return text.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

std::size_t countOf(const std::string& hay, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) {
        ++n;
    }
    return n;
}

// Every `name = ` ASSIGNMENT (not `==`) in `text`, as a byte offset.
std::vector<std::size_t> assignmentsOf(const std::string& text, const std::string& name) {
    std::vector<std::size_t> out;
    for (std::size_t at = text.find(name); at != std::string::npos; at = text.find(name, at + 1)) {
        if (at > 0) {
            const char before = text[at - 1];
            if (std::isalnum(static_cast<unsigned char>(before)) || before == '_') { continue; }
        }
        std::size_t p = at + name.size();
        while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) { ++p; }
        if (p < text.size() && text[p] == '=' && (p + 1 >= text.size() || text[p + 1] != '=')) {
            out.push_back(at);
        }
    }
    return out;
}

void testEvents() {
    std::printf("the block follows every event, with no refresh and no frame in between\n");
    {
        std::lock_guard<std::mutex> lk(g_reg.m);
        g_reg.native = {{"rtlsdr", "Generic RTL2832U A", kRtlArgs},
                        {"hackrf", "HackRF One", kHackArgs}};
    }
    th::TestWindow app;
    // run() renders the block once before the first frame. That is all the
    // window has done; everything below is an event.
    Access::firstRefresh(app);
    expectBlockMatchesLive(app, "start (generator)");

    // (a)/(b) THE ASYNCHRONOUS OPEN FROM THE SOURCE LIST.
    CHECK(Access::selectNative(app, kRtlArgs));
    CHECK(Access::deviceOpen(app));
    expectBlockMatchesLive(app, "radio opened from the list");
    {
        // The two facts the reports lacked, by value.
        const std::string block = cascade::core::diagContextBlock();
        CHECK(field(block, "source") == "rtlsdr");
        CHECK(field(block, "device-open") == "yes");
        CHECK(field(block, "sample-rate") != "2000000");
    }

    // A SAMPLE-RATE CHANGE by the browser's route (the Rate combo and a plugin
    // preset reach the same followInputRate()).
    Access::setRateByRemote(app, 3.2e6);
    CHECK(Access::rate(app) == 3.2e6);
    expectBlockMatchesLive(app, "sample rate changed");

    // A MODE CHANGE, by the button/key route and by the browser's.
    Access::setModeByButton(app, 4);
    CHECK(Access::modeIndex(app) == 4);
    expectBlockMatchesLive(app, "mode button (USB)");
    CHECK(field(cascade::core::diagContextBlock(), "mode") == "USB");
    Access::setModeByRemote(app, cascade::dsp::DemodMode::AM);
    CHECK(Access::modeIndex(app) == 2);
    expectBlockMatchesLive(app, "mode by remote request (AM)");

    // A bookmark carries a mode too (the desktop's route; the browser's is the
    // same call).
    {
        cascade::core::Bookmark b;
        b.freqHz = 100.0e6;
        b.mode = "CW";
        b.bandwidthHz = 3000.0;
        Access::tuneBookmark(app, b);
        CHECK(Access::modeIndex(app) == 5);
        expectBlockMatchesLive(app, "bookmark tuned (CW)");
        CHECK(field(cascade::core::diagContextBlock(), "mode") == "CW");
    }

    // A FAILED OPEN. The radio running is closed before the attempt (the
    // close-first rule), the attempt fails, and the generator is what is left.
    CHECK(Access::selectNative(app, kHackArgs));
    CHECK(!Access::deviceOpen(app));
    CHECK(Access::source(app) == "siggen");
    expectBlockMatchesLive(app, "failed open (hackrf)");

    // BACK ONTO A RADIO, then A SWAP BACK TO THE GENERATOR by the user.
    CHECK(Access::selectNative(app, kRtlArgs));
    expectBlockMatchesLive(app, "radio opened again");
    Access::selectGenerator(app);
    CHECK(Access::source(app) == "siggen");
    expectBlockMatchesLive(app, "swapped back to the generator");

    // PATCH RADIOS STARTING AND STOPPING (report (c)): a second signal path the
    // block knew nothing about. Kinds only - never a label, a serial or a
    // frequency.
    const auto n1 = Access::addRadioNode(app, kRtlKey);
    CHECK(Access::runPatchUntilOpen(app, n1));
    expectBlockMatchesLive(app, "patch radio 1 running");
    CHECK(field(cascade::core::diagContextBlock(), "patch-radios") == "1 (rtlsdr)");
    const auto n2 = Access::addRadioNode(app, "rtlsdr|serial=0000000B");
    CHECK(Access::runPatchUntilOpen(app, n2));
    expectBlockMatchesLive(app, "patch radio 2 running");
    CHECK(field(cascade::core::diagContextBlock(), "patch-radios") == "2 (rtlsdr, rtlsdr)");
    Access::switchNodeOff(app, n2);
    expectBlockMatchesLive(app, "one patch radio switched off");
    CHECK(field(cascade::core::diagContextBlock(), "patch-radios") == "1 (rtlsdr)");
    Access::stopPatch(app);
    expectBlockMatchesLive(app, "patch stopped");
    CHECK(field(cascade::core::diagContextBlock(), "patch-radios") == "none");

    // THE PLUGIN SET: a rescan unloads and reloads every plugin. With none
    // installed the block says none before and after; that it is REFRESHED on
    // every exit of the rescan is held structurally below.
    Access::rescan(app);
    CHECK(field(cascade::core::diagContextBlock(), "plugin") == "(none)");

    // NOTHING IDENTIFYING: the block carries kinds, never a serial.
    const std::string block = cascade::core::diagContextBlock();
    CHECK(block.find("0000000A") == std::string::npos);
    CHECK(block.find("0000000B") == std::string::npos);
    CHECK(block.find("serial") == std::string::npos);
}

void testCost() {
    // The refresh runs EVERY FRAME now, so it has to be cheap: a few strings, and
    // an unchanged block writes nothing. A generous ceiling - it exists to catch a
    // refresh that starts doing real work (a directory walk, a device call), not to
    // grade the machine it runs on.
    std::printf("the refresh is cheap enough to run every frame\n");
    th::TestWindow app;
    Access::firstRefresh(app);
    constexpr int kCalls = 5000;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kCalls; ++i) { Access::firstRefresh(app); }
    const double us =
        std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() /
        kCalls;
    std::printf("  %.2f microseconds per refresh (budget at 60 Hz: 16667)\n", us);
    CHECK(us < 500.0);
}

void testStructure() {
    std::printf("the writers funnel through one function each\n");
    // (The writers are the ENGINE's since the extraction: src/engine holds what app_window*.cpp did, file for file - engine.cpp,
    // engine_converter.cpp, engine_patch_radios.cpp.)
    const fs::path src = fs::path(CASCADE_SOURCE_DIR) / "src" / "engine";
    std::string appWindow = readText(src / "engine.cpp");
    CHECK(!appWindow.empty());

    // Every install, swap and close of the source ENDS with applyConverterForSource()
    // - the step "every install site calls", by its own comment - and that call
    // renders the block, at a point where installSource() and the kind, device and
    // model assignments around it have all run. So the block is rendered from a
    // state that agrees with itself, and a new install site that forgets the
    // converter step forgets something the converter tests would catch first.
    std::size_t installSites = 0;
    for (const fs::directory_entry& e : fs::directory_iterator(src)) {
        if (e.path().extension() != ".cpp") { continue; }
        const std::string text = readText(e.path());
        const std::string call = "installSource(";
        for (std::size_t at = text.find(call); at != std::string::npos;
             at = text.find(call, at + 1)) {
            // The definition, and any mention in a comment, are not call sites.
            const std::size_t lineStart = text.rfind('\n', at) + 1;
            const std::string before = text.substr(lineStart, at - lineStart);
            if (before.find("Engine::") != std::string::npos ||
                before.find("//") != std::string::npos) {
                continue;
            }
            ++installSites;
            const std::size_t followed = text.find("applyConverterForSource();", at);
            const bool closeBy = followed != std::string::npos && followed - at < 2600;
            if (!closeBy) {
                std::printf("  installSource() not followed by applyConverterForSource(): %s offset %zu\n",
                            e.path().filename().string().c_str(), at);
            }
            CHECK(closeBy);
        }
    }
    std::printf("  %zu installSource() call sites\n", installSites);
    CHECK(installSites >= 10);
    CHECK(functionBody(readText(src / "engine_converter.cpp"),
                       "void Engine::applyConverterForSource(")
              .find("refreshDiagContext();") != std::string::npos);

    // modeIndex_ likewise, in commitModeIndex().
    const std::string commit = functionBody(appWindow, "void Engine::commitModeIndex(");
    CHECK(!commit.empty());
    CHECK(commit.find("refreshDiagContext();") != std::string::npos);
    for (const fs::directory_entry& e : fs::directory_iterator(src)) {
        if (e.path().extension() != ".cpp") { continue; }
        const std::string text = readText(e.path());
        const std::size_t commitAt = text.find(commit);
        for (const std::size_t at : assignmentsOf(text, "modeIndex_")) {
            const bool inCommit = !commit.empty() && commitAt != std::string::npos &&
                                  at >= commitAt && at < commitAt + commit.size();
            if (!inCommit) {
                std::printf("  modeIndex_ assigned outside commitModeIndex(): %s offset %zu\n",
                            e.path().filename().string().c_str(), at);
            }
            CHECK(inCommit);
        }
    }

    // The rate funnel, the plugin rescan (every exit: it returns early when the
    // quarantine fails, after the plugins have been unloaded) and the patch
    // page's two entry points.
    CHECK(functionBody(appWindow, "void Engine::followInputRate(")
              .find("DiagContextOnExit") != std::string::npos);
    CHECK(functionBody(appWindow, "void Engine::rescanPlugins(")
              .find("DiagContextOnExit") != std::string::npos);
    const std::string patch = readText(src / "engine_patch_radios.cpp");
    CHECK(functionBody(patch, "void Engine::patchReconcile(")
              .find("DiagContextOnExit") != std::string::npos);
    CHECK(functionBody(patch, "void Engine::patchStopAll(")
              .find("DiagContextOnExit") != std::string::npos);

    // THE ENGINE'S PASS checks EVERY pass - the net under anything a future writer forgets (pumpDiagContext: gated to once a second and
    // hashed, so an unchanged block writes nothing) - and not one pass in sixty.
    const std::string pass = functionBody(appWindow, "void Engine::pumpAudio(");
    CHECK(!pass.empty());
    CHECK(pass.find("pumpDiagContext();") != std::string::npos);
    const std::string pumpCtx = functionBody(appWindow, "void Engine::pumpDiagContext(");
    CHECK(!pumpCtx.empty());
    CHECK(pumpCtx.find("refreshDiagContext();") != std::string::npos);
    CHECK(pass.find("% 60") == std::string::npos && pumpCtx.find("% 60") == std::string::npos);
}

}  // namespace

int main() {
    std::printf("test_diag_context_app\n");
    isolate();
    Access::installHooks();
    testEvents();
    testCost();
    testStructure();
    return testSummary("test_diag_context_app");
}
