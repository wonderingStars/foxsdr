// test_engine_api_03.cpp - API 0.3's additions (docs/api-0.3-additions.md;
// engine-stage4.md plan step 3, the owner's decision 4), held against the real
// engine through the C table and nothing else - the way
// test_engine_api_conformance holds 0.2.
//
// One group per addition, each shown red before the engine served it:
//   V   the version gate and the capability bits (0.3 only; a 0.2 interface
//       is refused, as 0.x requires)
//   L   every list the window draws is SERVED (a count, never UNSUPPORTED) -
//       the 0.2 lists step 1 left out and the 0.3 ones
//   T   submit_text: one command, a text longer than text[], the same answers
//       as submit
//   S   the status lines: a list, and the whole text as a fact
//   F   facts: unknown keys, LOCAL-only ones, the transmitter's address
//   P   the patch graph read back: nodes, wires, the document, the event
//   R   the recordings: the file, ACTIVE, the bytes live, the path LOCAL only
//   C   the scope taps: newest frames, the tap's rate, "reduced" for REMOTE
//   N   a patch radio's spectrum: NOT_FOUND with none running
//   E   serial ports: the scan is LOCAL only, the list is served
//   D   the decoder-output backlog
//   K   the store: an entry's long texts as facts
//   B   the bias tee: flags and facts on a radio without one
//   X   the transmitter as its page draws it (state, 8.1): split, input,
//       tone, monitor, the board's power range, the device, where it would
//       transmit, the power setting with no board open
//   G   the lists BEFORE THE FIRST PUBLISH: a session opened on an engine no
//       pass has run on (what gui::AppWindow does in its constructor) reads
//       every list as empty; the bookmarks list used to dereference a block
//       that has none yet (review F4)
//   Y   the status column as its page draws it (state, 8.2, and the two list
//       changes): the sink's words, the output ring and its rate, the decoder
//       output total (a measurement), who mutes and who plays (the texts move
//       audioSeq), the GAINS unit token, the recordings' elapsed seconds
//   U   the usage report's panel names (FOXAPP_OP_NOTE_PANEL, window port
//       group 3): a LOCAL session names a panel, the engine lists it once and
//       stops at 16; a REMOTE session and an empty name add nothing. Read back from the engine's own list, with the
//       passes driven from the test thread (no control thread, no race).
//   Z   the radio panels as their pages draw them (window port group 4, API
//       0.3 section 8.3), each driven from the test thread against a fake radio:
//       Z1 FOXAPI_RX_DEVICE_OPENING while an open is held on its worker;
//       Z2 an Airspy's gain stages carry FOXAPI_ITEM_AGC (and the gain-mode and
//       decimation lists the panel reads are pinned); Z3 the converter in force
//       (fact.converter.live) and the radio's own centre (CONVERTERS value[3],
//       live); Z4 the alias note, the modes offered and the ignored up-converter
//       (a sound card's), and the second sound card moving listSeq; Z5 the
//       bias tee's stand-in (test.biasStandIn) and the asked-about radio an
//       ON may carry (N9); Z6 the same against the HackRF driver on a fake wire,
//       and the fingerprint gaps (a radio that dies, a full memory); Z7 a
//       language switch moving the translated converter name.
//   R   the receiver rail and the display as their pages draw them (window
//       port group 5, API 0.3 section 8.4), against test.* stand-ins for what
//       the DSP thread measures: R1-R3 the RDS decoder's state (rdsFlags, the
//       codes, the texts, rdsSeq, the two measured counters), R4 the
//       auto-notch's lamp (a mode-group flag) and frequency (a measurement),
//       R5 the audio watchdog's note as a status line, R7 the pilot lock,
//       R8 the loaded band plan's name; R6 (MUTE_CAUSES) was not needed.
//   Q   the tuning surface (window port group 6, API 0.3 section 8.5): Q1
//       fact.bookmarks.version moves with the bookmark store (an add, a star
//       that keeps the list's size, a remove) and with nothing else; Q2
//       fact.input.rateHz is the DSP chain's input rate, not the source's, and
//       moves listSeq; Q3 the test seam that puts a known spectrum frame in the
//       ring (ApiHost::publishSpectrumForTest), read back through read_spectrum.
//   W   the scanner as its section draws it (window port group 7, API 0.3 section
//       8.6): W1 the idle scanner and its defaults, W2 the seven settings as a
//       group of their own (scanSeq), W3 the timings as whole milliseconds, W4 the
//       state word and the frequency through all four states, moving seq alone.
//   J   the Bookmarks section's note and red line as status lines (and facts, and
//       the window's own copy), including the real writer of the note.
//   O   FOXAPP_OP_BOOKMARK_EXPORT: the ids it takes, the file it writes whole under the
//       record folder, the note, what it refuses, and its cost on a long list.
//   M   the decoders, the presets, the marks, the plugin-mute cluster and the tune grants (window
//       port group 8, API 0.3 section 8.7): M1 MUTE_CAUSES, fact.mute.* and the tune-away event
//       (R6 of group R, which was not needed there); M2 PLUGINS' capability word and flags,
//       DECODERS' MUTES, STALE_GRANTS and fact.plugin.lastDenied, against two real modules in a
//       scratch folder; M3 PRESETS' device-centre flag and raw ids and MARKERS' kind and flags;
//       M4 (D3) a decoder line moves decoderLinesTotal and not listSeq, and a track-info line is
//       counted.
//   H   the fitted-modules window (window port group 9, API 0.3 section 8.8), against three real
//       modules and a file the host refuses in a scratch folder: H0 a plugin's log lines are counted
//       into decoderLinesTotal (the bug found beside this window); H1 the per-file facts and
//       fact.plugin.dir (whole, "" and NOT_FOUND, DENIED to a REMOTE session); H2 PLUGINS value[1]
//       (the file's size, measured at a rebuild) and value[2] (the notice level), and a repeated
//       warning moving nothing; H3 the flags MATCHED, ASKED_SETTINGS and DIGEST_MISMATCH and the idle
//       sentence; H4 PLUGIN_COMMANDS; H5 installReport and installError as status lines.
//   I   the plugin store (window port group 10, API 0.3 section 8.9), against a REAL fetch of a local index.json
//       over the group-9 plugin folder (a retired module with an update plan, an updatable fitted one, and five
//       entries for the columns), with the transfers' workers stood in for by a promise the test keeps and ADD
//       ALL run for real over entries the install refuses before the network: I1 CATALOGUE's value[0], value[1]
//       and BUILD_HERE, the six per-entry facts, fact.store.url (LOCAL), the retired row's id and whole sentence,
//       every NOT_FOUND / "" / DENIED edge and a failed refetch; I2 STORE_ACTIVITY's transfer entry and the
//       live progress (which moves no list); I3 the ADD ALL run's position, total and name, published after
//       every pass, and its red summary; I4 catalogStatus and addAllSummary as lines, and the language moving the
//       retired row's sentence; I5 the catalogue counter the fingerprint hashes, at every write of the catalogue.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "airspy_fake_usb.hpp"
#include "check.hpp"
#include "core/app_commands.hpp"
#include "core/freq_converter.hpp"
#include "core/freq_import.hpp"
#include "core/freq_manager.hpp"
#include "core/scanner.hpp"
#include "core/i18n.hpp"
#include "core/patch_draft.hpp"
#include "core/patch_graph.hpp"
#include "core/plugin_repo.hpp"
#include "engine/bias_tee.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine/plugin_store_reasons.hpp"
#include "engine/soundcard_panel.hpp"
#include "engine_api_fake_radio.hpp"
#include "engine_api_test_env.hpp"
#include "engine_client.hpp"
#include "foxsdr_api.h"
#include "hackrf_fake_usb.hpp"
#include "source/airspy_source.hpp"
#include "source/hackrf_source.hpp"

namespace radios83 {
std::unique_ptr<cascade::source::DeviceSource> make(const std::string& kind);
std::vector<cascade::source::NativeDeviceInfo> scan();
}  // namespace radios83

namespace cascade::gui {
struct AppWindowTestAccess {
    static void installFakeRadio() {
        cascade::engine::Engine::testHooks_.makeDevice = &engine_api_test::makeFakeRadio;
        cascade::engine::Engine::testHooks_.nativeScan = &engine_api_test::fakeNativeScan;
        cascade::engine::Engine::testHooks_.soapyScan = &engine_api_test::noSoapyRadios;
    }
    // Group Z: the fake mock radio, an Airspy and a HackRF on fake wires.
    static void installRadios83() {
        cascade::engine::Engine::testHooks_.makeDevice = &radios83::make;
        cascade::engine::Engine::testHooks_.nativeScan = &radios83::scan;
        cascade::engine::Engine::testHooks_.soapyScan = &engine_api_test::noSoapyRadios;
    }
    // The panel names the usage report would list (group U).
    static std::vector<std::string> panels(cascade::engine::Engine& e) { return e.telemetryPanels_; }
    // The converter's and the sound card's inputs, set directly: no sound card
    // backend runs in this test, so the engine is told it is on one (Z4).
    static void setSourceKind(cascade::engine::Engine& e, const std::string& kind) { e.sourceKind_ = kind; }
    static void setCard(cascade::engine::Engine& e, const std::string& device, const std::string& hostApi,
                        cascade::source::SoundCardFormat format) {
        e.soundCardLive_.device = device;
        e.soundCardLive_.hostApi = hostApi;
        e.soundCardLive_.format = format;
    }
    static std::string keyNow(cascade::engine::Engine& e) { return e.converterRadioKeyNow(); }
    static std::string rawKey(cascade::engine::Engine& e) { return e.converterRawKeyNow(); }
    static void storeConverter(cascade::engine::Engine& e, const std::string& key,
                               const cascade::core::ConverterSetting& s) {
        e.converters_[key] = s;
    }
    static void aliasKey(cascade::engine::Engine& e, const std::string& from, const std::string& to) {
        e.converterKeyAlias_[from] = to;
    }
    // The converter in force changed with nothing stored or sourced differently:
    // what a source install does to the pipeline (applyConverterForSource).
    static void setPipelineConverter(cascade::engine::Engine& e, const cascade::core::ConverterSetting& s) {
        e.pipeline_.setConverter(s);
    }
    static void clearAlias(cascade::engine::Engine& e) {
        e.converterKeyAlias_.clear();
        e.converterNotCarried_.clear();
    }
    static void notCarried(cascade::engine::Engine& e, const std::string& key) { e.converterNotCarried_.insert(key); }
    // The bias tee memory filled to `n` radios other than the open one (Z6).
    static void fillBiasMemory(cascade::engine::Engine& e, std::size_t n) {
        e.biasTeePanel_.remembered.clear();
        for (std::size_t i = 0; i < n; ++i) { e.biasTeePanel_.remembered["filler|" + std::to_string(i)] = false; }
    }
    // The open HackRF's request, made behind the engine's back: it fails (the
    // wire is told to) and the driver treats the radio as dead from then on.
    static bool breakHackRf(cascade::engine::Engine& e) {
        auto* h = dynamic_cast<cascade::source::HackRfSource*>(e.device_);
        if (h == nullptr) { return false; }
        (void)h->setBiasT(true);
        return h->deviceDead();
    }
    // Group Q: the DSP chain's input rate, moved behind the source's back - the
    // state a refused rate change leaves (Engine::followInputRate): the chain on
    // one rate and the source on another.
    static bool setChainRate(cascade::engine::Engine& e, double hz) { return e.pipeline_.setInputRateHz(hz); }
    static double chainRate(cascade::engine::Engine& e) { return e.pipeline_.inputRateHz(); }
    // Group W: the squelch the scanner compares the pipeline's power with; a hermetic engine
    // hears nothing (-200 dB), so a threshold below that stands in for a signal. Group O/J: the
    // bookmark store, the record folder and the note the engine keeps.
    static void setSquelch(cascade::engine::Engine& e, float db) { e.squelchDb_ = db; }
    static cascade::core::FreqManager& store(cascade::engine::Engine& e) { return e.freqMgr_; }
    static void setRecordDir(cascade::engine::Engine& e, const std::string& dir) { e.recordDir_ = dir; }
    static const std::string& note(cascade::engine::Engine& e) { return e.bookmarkImportNote_; }
    // Group M: the folder the engine looks for plugins in (null: the default).
    static void setPluginDir(std::string (*dir)()) { cascade::engine::Engine::testHooks_.pluginDir = dir; }
    // Group M: a plugin asks to tune the receiver (the host bridge's request_tune), by its module file name.
    static std::int32_t askToTune(cascade::engine::Engine& e, const std::string& key) {
        return e.pluginUi_.tuneRequestFromPlugin(key, 100.0e6);
    }
    // Group 9: the host's side of the plugin API, which a plugin's own calls reach through the host
    // bridge's trampolines (log, add_command, a request that needs the settings grant...). The test makes
    // the same calls on the loaded module's client, from the test thread.
    static cascade::core::PluginApiCore& core(cascade::engine::Engine& e) { return e.pluginUi_.api(); }
    // The install records the engine holds, as the last rescan read them (a rescan replaces them).
    static cascade::core::PluginInventory& inventory(cascade::engine::Engine& e) { return e.pluginInventory_; }
    // Group 10 (the plugin store): a transfer as its worker would leave it. The engine's install slot holds a
    // promise the test keeps, so a download is in flight (installPending_, the busy name) until the test says it
    // is over, with a result the pump applies exactly as it applies a real worker's. Nothing touches the network.
    static std::promise<cascade::engine::Engine::PluginInstallResult>& installPromise() {
        static std::promise<cascade::engine::Engine::PluginInstallResult> p;
        return p;
    }
    static void beginTransfer(cascade::engine::Engine& e, const std::string& name) {
        installPromise() = std::promise<cascade::engine::Engine::PluginInstallResult>();
        e.installFuture_ = installPromise().get_future();
        e.installBusyName_ = name;
        e.installPending_ = true;
    }
    static void endTransfer(bool ok, const std::string& name, const std::string& error) {
        cascade::engine::Engine::PluginInstallResult r;
        r.ok = ok;
        r.name = name;
        r.error = error;
        installPromise().set_value(std::move(r));
    }
    // What the download worker would have stored in the repo's progress atomic.
    static void setProgress(cascade::engine::Engine& e, float p) { e.pluginRepo_.setProgressForTest(p); }
    // The counter the list fingerprint hashes for everything a catalogue entry carries.
    static std::uint64_t catalogSeq(cascade::engine::Engine& e) { return e.catalogSeq_; }
};
}  // namespace cascade::gui

// --- the radios of group Z ---------------------------------------------------------------
//
// The mock RTL-SDR the other groups use, the SHIPPING Airspy driver and the
// SHIPPING HackRF driver, each on a fake USB wire (tests/airspy_fake_usb.hpp,
// tests/hackrf_fake_usb.hpp): nothing on the desk is enumerated or opened. The
// Airspy's wire hands its device out only while the gate is open, so a test can
// hold an open on its worker (Z1).
namespace radios83 {

constexpr const char* kAirspySerial = "644866c83f1a51df";
const std::string kAirspyId = std::string("airspy:serial=") + kAirspySerial;
constexpr const char* kHackSerial = "0000000000000000457863c8";
const std::string kHackArgs = std::string("serial=") + kHackSerial;
const std::string kHackId = std::string("hackrf:") + kHackArgs;
constexpr std::uint8_t kAntennaEnable = 23;  // libhackrf's ANTENNA_ENABLE: the bias-T request

std::atomic<bool> g_gateOpen{true};
std::mutex g_mutex;
cascade::test::FakeHackRfUsb* g_hack = nullptr;

std::unique_ptr<cascade::source::DeviceSource> make(const std::string& kind) {
    if (kind == "airspy") {
        auto src = std::make_unique<cascade::source::AirspySource>();
        cascade::usb::UsbDeviceInfo d;
        d.vid = 0x1D50;
        d.pid = 0x60A1;
        d.path = "\\\\?\\usb#vid_1d50&pid_60a1#fake#{a5dcbf10}";
        d.serial = kAirspySerial;
        d.description = "AIRSPY";
        auto holder = std::make_shared<std::unique_ptr<cascade::usb::UsbDevice>>(
            std::make_unique<cascade::test::FakeAirspyUsb>());
        src->setTransportForTest({d}, [holder](const std::string&, std::string& error) {
            // The open waits here, on its worker, for as long as the test says
            // (never more than ten seconds: a test that forgets cannot hang).
            for (int i = 0; i < 10000 && !g_gateOpen.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (*holder == nullptr) { error = "fake: already handed out"; }
            return std::move(*holder);
        });
        return src;
    }
    if (kind == "hackrf") {
        auto src = std::make_unique<cascade::source::HackRfSource>();
        cascade::usb::UsbDeviceInfo d;
        d.vid = 0x1D50;
        d.pid = 0x6089;
        d.path = std::string("\\\\?\\usb#vid_1d50&pid_6089#fake") + kHackSerial;
        d.serial = kHackSerial;
        d.description = "HackRF One";
        auto owned = std::make_unique<cascade::test::FakeHackRfUsb>();
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            g_hack = owned.get();
        }
        auto holder = std::make_shared<std::unique_ptr<cascade::usb::UsbDevice>>(std::move(owned));
        src->setTransportForTest({d}, [holder](const std::string&, std::string& error) {
            if (*holder == nullptr) { error = "fake: already handed out"; }
            return std::move(*holder);
        });
        return src;
    }
    return engine_api_test::makeFakeRadio(kind);
}

std::vector<cascade::source::NativeDeviceInfo> scan() {
    return {{engine_api_test::kFakeRadioDriver, "Mock RTL-SDR (serial 00000001)", engine_api_test::kFakeRadioArgs},
            {"airspy", "Airspy R2", std::string("serial=") + kAirspySerial},
            {"hackrf", "HackRF One", kHackArgs}};
}

cascade::test::FakeHackRfUsb* hack() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_hack;
}

// The bias-T requests the driver has sent since the transcript was cleared, as
// the values it asked for (1 on, 0 off).
std::vector<int> biasRequests() {
    std::vector<int> out;
    if (hack() == nullptr) { return out; }
    for (const cascade::test::ControlRecord& c : hack()->controls()) {
        if (!c.in && c.request == kAntennaEnable) { out.push_back(c.value); }
    }
    return out;
}

}  // namespace radios83

using testing::cmd;
using testing::kAllGrants;

namespace {

constexpr const char* kHooks = "testHooks=1;token=s3cret";

const FoxEngineApi* api() { return cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR); }

std::string fact(testing::Session& s, const char* key, int32_t* status = nullptr) {
    char buf[8192];
    const int32_t n = api()->get_setting(s.raw(), key, buf, sizeof(buf));
    if (status != nullptr) { *status = n; }
    return n >= 0 ? std::string(buf) : std::string();
}

int32_t count(testing::Session& s, uint32_t list) { return api()->read_list(s.raw(), list, 0, nullptr); }

FoxListItem item(testing::Session& s, uint32_t list, uint32_t index) {
    FoxListItem it{};
    it.structSize = sizeof(it);
    api()->read_list(s.raw(), list, index, &it);
    return it;
}

// Submits ONE command with a long text and waits for its result.
testing::Outcome runText(testing::Session& s, FoxCommand c, const std::string& text, int timeoutMs = 2000) {
    testing::Outcome o;
    FoxSubmitResult sr{};
    sr.structSize = sizeof(sr);
    const int32_t n = api()->submit_text(s.raw(), &c, text.data(), static_cast<uint32_t>(text.size()), &sr);
    o.submitStatus = n < 0 ? n : sr.status;
    if (o.submitStatus != FOXAPI_OK) { return o; }
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end) {
        FoxCommandResult r[16];
        for (auto& x : r) { x.structSize = sizeof(x); }
        const int32_t got = api()->poll_results(s.raw(), r, 16);
        for (int32_t i = 0; i < got; ++i) {
            if (r[i].ticket == sr.ticket) {
                o.landed = true;
                o.result = r[i];
            }
        }
        if (o.landed) { return o; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return o;
}

FoxCommand appCmd(uint32_t op, long long i0 = 0, double n0 = 0.0) {
    FoxCommand c = cmd(op, n0, i0);
    return c;
}

// A REMOTE session on the engine's configured token.
struct Remote : testing::Session {
    explicit Remote(testing::Engine& e)
        : testing::Session(api(), e.raw(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE |
                                                                     FOXAPI_GRANT_SETTINGS,
                           "s3cret") {}
};

void testVersionGate() {
    CHECK(cascade::engine::engineApi(0, 3) != nullptr);
    CHECK(cascade::engine::engineApi(0, 2) == nullptr);  // 0.x: its own minor only
    const FoxEngineApi* a = api();
    CHECK_EQ(a->versionMinor, 3u);
    CHECK(a->structSize >= FOXAPI_MIN_ENGINE_API);
    CHECK(FOXAPI_HAS(a, read_scope));
    CHECK(FOXAPI_HAS(a, read_node_spectrum));
    CHECK(FOXAPI_HAS(a, submit_text));
    CHECK((a->capabilities & FOXAPI_CAP_FACTS) != 0u);
    CHECK((a->capabilities & FOXAPI_CAP_LONG_TEXT) != 0u);
    CHECK((a->capabilities & FOXAPI_CAP_SCOPE) != 0u);
    CHECK((a->capabilities & FOXAPI_CAP_NODE_SPECTRUM) != 0u);
}

void testEveryListIsServed() {
    testing::Engine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    int served = 0;
    for (uint32_t l = 1; l <= FOXAPI_LIST_STORE_ACTIVITY; ++l) {
        if (l == 13u) { continue; }  // retired in 0.2
        if (l == FOXAPI_LIST_TRACKS) {
            // Map tracks come from track-source plugins through the map
            // page, which is not served yet (docs/api-0.3-additions.md).
            CHECK_EQ(count(s, l), FOXAPI_UNSUPPORTED);
            continue;
        }
        const int32_t n = count(s, l);
        if (n < 0) { std::printf("  list %u answered %d\n", l, n); }
        CHECK(n >= 0);
        served += n >= 0 ? 1 : 0;
    }
    std::printf("L: %d lists served\n", served);
    // The transmitter's one "device": the address TX_OPEN will be given.
    CHECK_EQ(count(s, FOXAPI_LIST_TX_DEVICES), 1);
    const FoxListItem tx = item(s, FOXAPI_LIST_TX_DEVICES, 0);
    CHECK_EQ(std::string(tx.detail), std::string("uri=ip:192.168.2.1"));
    CHECK((tx.flags & FOXAPI_ITEM_ACTIVE) == 0u);
    // No plugins, no radio options on the generator.
    CHECK_EQ(count(s, FOXAPI_LIST_DECODERS), 0);
    CHECK_EQ(count(s, FOXAPI_LIST_DEVICE_OPTIONS), 0);
    CHECK_EQ(count(s, FOXAPI_LIST_GAIN_MODES), 0);
}

void testSubmitText() {
    testing::Engine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    const FoxEngineApi* a = api();
    const std::string longText(600, 'x');
    FoxCommand c = appCmd(FOXAPP_OP_SET_SOURCE_ERROR);
    const testing::Outcome o = runText(s, c, longText);
    CHECK_EQ(o.submitStatus, FOXAPI_OK);
    CHECK(o.landed);
    CHECK_EQ(o.result.status, FOXAPI_OK);
    int32_t st = 0;
    CHECK_EQ(fact(s, "fact.status.sourceError", &st), longText);
    CHECK_EQ(st, 600);
    // The same answers as submit for what is wrong with the call.
    FoxSubmitResult sr{};
    sr.structSize = sizeof(sr);
    CHECK_EQ(a->submit_text(s.raw(), nullptr, "x", 1, &sr), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(a->submit_text(s.raw(), &c, nullptr, 5, &sr), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(a->submit_text(s.raw(), &c, longText.data(), FOXAPI_MAX_LONG_TEXT + 1u, &sr), FOXAPI_LIMIT);
    // A REMOTE session still may not send the app's own ops, long or short.
    Remote r(e);
    CHECK(r.ok());
    const testing::Outcome ro = runText(r, c, longText);
    CHECK_EQ(ro.status(), FOXAPI_DENIED);
}

void testStatusLines() {
    testing::Engine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK_EQ(count(s, FOXAPI_LIST_STATUS_LINES), 0);
    const std::string why = std::string("the radio said no: ") + std::string(300, 'y');
    CHECK_EQ(runText(s, appCmd(FOXAPP_OP_SET_SOURCE_ERROR), why).result.status, FOXAPI_OK);
    CHECK(check::waitFor([&] { return count(s, FOXAPI_LIST_STATUS_LINES) == 1; }, 2000));
    const FoxListItem it = item(s, FOXAPI_LIST_STATUS_LINES, 0);
    CHECK_EQ(std::string(it.name), std::string("sourceError"));
    CHECK_EQ(it.value[0], 3.0);  // an error
    CHECK(std::string(it.detail).size() == sizeof(it.detail) - 1);  // cut, whole in the fact
    CHECK(why.compare(0, sizeof(it.detail) - 1, it.detail) == 0);
    CHECK_EQ(fact(s, "fact.status.sourceError"), why);
    CHECK_EQ(runText(s, appCmd(FOXAPP_OP_SET_SOURCE_ERROR), std::string()).result.status, FOXAPI_OK);
    CHECK(check::waitFor([&] { return count(s, FOXAPI_LIST_STATUS_LINES) == 0; }, 2000));
    int32_t st = 0;
    CHECK_EQ(fact(s, "fact.status.sourceError", &st), std::string());
    CHECK_EQ(st, 0);  // a known line, empty now
    // A line further down the list: the list shows only the lines with text.
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SETTING_SET, 0, 0, "test.statusLine=presetNote|Tuned to the preset.")).result.status,
             FOXAPI_OK);
    CHECK(check::waitFor([&] { return count(s, FOXAPI_LIST_STATUS_LINES) == 1; }, 2000));
    const FoxListItem note = item(s, FOXAPI_LIST_STATUS_LINES, 0);
    CHECK_EQ(std::string(note.name), std::string("presetNote"));
    CHECK_EQ(std::string(note.detail), std::string("Tuned to the preset."));
    CHECK_EQ(note.value[0], 1.0);  // information
}

void testFacts() {
    testing::Engine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    Remote r(e);
    CHECK(r.ok());
    int32_t st = 0;
    fact(s, "fact.no.such.thing", &st);
    CHECK_EQ(st, FOXAPI_NOT_FOUND);
    fact(s, "fact.status.noSuchLine", &st);
    CHECK_EQ(st, FOXAPI_NOT_FOUND);
    CHECK_EQ(fact(s, "fact.transmit.args"), std::string("uri=ip:192.168.2.1"));
    // A path on this machine: LOCAL sessions only.
    CHECK(!fact(s, "fact.recorder.dir", &st).empty());
    CHECK(st > 0);
    fact(r, "fact.recorder.dir", &st);
    CHECK_EQ(st, FOXAPI_DENIED);
    // Facts are read-only: no SETTING_SET takes one.
    CHECK(s.run(cmd(FOXAPI_OP_SETTING_SET, 0, 0, "fact.transmit.args=x")).status() != FOXAPI_OK);
    CHECK_EQ(fact(s, "fact.transmit.args"), std::string("uri=ip:192.168.2.1"));
}

void testPatchGraph() {
    testing::Engine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    const FoxEngineApi* a = api();
    a->subscribe(s.raw(), FOXAPI_EVENT_MASK(FOXAPI_EVENT_PATCH));
    CHECK_EQ(count(s, FOXAPI_LIST_PATCH_NODES), 0);
    namespace patch = cascade::core::patch;
    patch::Graph g;
    const patch::NodeId radio = g.addNode(patch::NodeKind::Radio, "Radio 1", patch::PortType::Iq, 10.0f, 20.0f);
    const patch::NodeId chan = g.addNode(patch::NodeKind::Channel, "Channel", patch::PortType::Iq, 200.0f, 20.0f);
    const patch::NodeId demod = g.addNode(patch::NodeKind::Demod, "Demod", patch::PortType::Iq, 400.0f, 20.0f);
    CHECK(g.connect(radio, 0, chan, 0) == patch::Connect::Ok);
    CHECK(g.connect(chan, 0, demod, 0) == patch::Connect::Ok);
    // More parts than the list checks need: a real patch's document is
    // longer than a command's text[] - which is the reason submit_text exists.
    for (int i = 0; i < 6; ++i) {
        g.addNode(patch::NodeKind::Display, "A spectrum display with a long name " + std::to_string(i),
                  patch::PortType::Iq, 600.0f, 40.0f * static_cast<float>(i));
    }
    const std::string doc = patch::graphCommandText(g);
    CHECK(doc.size() > FOXAPI_TEXT_CHARS);  // the reason submit_text exists
    const FoxReceiverState before = s.state();
    const testing::Outcome o =
        runText(s, appCmd(FOXAPP_OP_PATCH_SET_GRAPH, FOXAPP_PATCH_GRAPH_DOCUMENT), doc);
    CHECK_EQ(o.result.status, FOXAPI_OK);
    CHECK(check::waitFor([&] { return count(s, FOXAPI_LIST_PATCH_NODES) == 9; }, 2000));
    CHECK(s.state().listSeq != before.listSeq);
    const FoxListItem n0 = item(s, FOXAPI_LIST_PATCH_NODES, 0);
    CHECK_EQ(n0.id, static_cast<uint64_t>(radio));
    CHECK_EQ(std::string(n0.name), std::string("Radio 1"));
    CHECK_EQ(std::string(n0.detail), std::string("radio"));
    CHECK_EQ(n0.value[0], 10.0);
    CHECK_EQ(n0.value[1], 20.0);
    CHECK_EQ(std::string(item(s, FOXAPI_LIST_PATCH_NODES, 2).detail), std::string("demod"));
    CHECK_EQ(count(s, FOXAPI_LIST_PATCH_WIRES), 2);
    const FoxListItem w0 = item(s, FOXAPI_LIST_PATCH_WIRES, 0);
    CHECK_EQ(w0.value[0], static_cast<double>(radio));
    CHECK_EQ(w0.value[2], static_cast<double>(chan));
    CHECK_EQ(fact(s, "fact.patch.graph"), doc);
    CHECK(check::waitFor(
        [&] {
            for (const FoxEvent& ev : s.events()) {
                if (ev.kind == FOXAPI_EVENT_PATCH && ev.value[0] >= 1.0) { return true; }
            }
            return false;
        },
        2000));
    // A document the engine cannot honour is refused whole: nothing moves.
    CHECK_EQ(runText(s, appCmd(FOXAPP_OP_PATCH_SET_GRAPH, FOXAPP_PATCH_GRAPH_DOCUMENT), "not a patch\n")
                 .result.status,
             FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(count(s, FOXAPI_LIST_PATCH_NODES), 9);
    CHECK_EQ(fact(s, "fact.patch.graph"), doc);
    // An EDIT (not a document load: the epoch stays) is read back too.
    g.addNode(patch::NodeKind::Sink, "Speaker", patch::PortType::Audio, 800.0f, 20.0f);
    const std::string edited = patch::graphCommandText(g);
    CHECK_EQ(runText(s, appCmd(FOXAPP_OP_PATCH_SET_GRAPH, 0), edited).result.status, FOXAPI_OK);
    CHECK(check::waitFor([&] { return count(s, FOXAPI_LIST_PATCH_NODES) == 10; }, 2000));
    CHECK_EQ(fact(s, "fact.patch.graph"), edited);
}

void testRecordings() {
    testing::Engine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    Remote r(e);
    CHECK_EQ(s.run(cmd(FOXAPI_OP_RUN, 0, 1)).result.status, FOXAPI_OK);
    CHECK_EQ(count(s, FOXAPI_LIST_RECORDINGS), 0);
    CHECK_EQ(s.run(cmd(FOXAPI_OP_RECORD_IQ, 0, 1)).result.status, FOXAPI_OK);
    CHECK(check::waitFor([&] { return count(s, FOXAPI_LIST_RECORDINGS) == 1; }, 2000));
    FoxListItem it = item(s, FOXAPI_LIST_RECORDINGS, 0);
    CHECK((it.flags & FOXAPI_ITEM_ACTIVE) != 0u);
    CHECK_EQ(it.value[0], 1.0);
    CHECK(std::strlen(it.name) > 0);
    CHECK(std::string(it.detail).find(it.name) != std::string::npos);  // the path ends in the file
    CHECK(std::strlen(item(r, FOXAPI_LIST_RECORDINGS, 0).detail) == 0);  // no path to a REMOTE session
    // The bytes are read live, without a list rebuild (listSeq still).
    const double b0 = it.value[1];
    const uint64_t seq0 = s.state().listSeq;
    CHECK(check::waitFor([&] { return item(s, FOXAPI_LIST_RECORDINGS, 0).value[1] > b0; }, 2000));
    CHECK_EQ(s.state().listSeq, seq0);
    CHECK_EQ(fact(s, "fact.recorder.iq.path"), std::string(it.detail));
    CHECK_EQ(s.run(cmd(FOXAPI_OP_RECORD_IQ, 0, 0)).result.status, FOXAPI_OK);
    CHECK(check::waitFor([&] { return (item(s, FOXAPI_LIST_RECORDINGS, 0).flags & FOXAPI_ITEM_ACTIVE) == 0u; },
                         2000));
    s.run(cmd(FOXAPI_OP_RUN, 0, 0));
}

void testScope() {
    testing::Engine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    Remote r(e);
    const FoxEngineApi* a = api();
    CHECK_EQ(s.run(cmd(FOXAPI_OP_RUN, 0, 1)).result.status, FOXAPI_OK);
    std::vector<float> buf(2 * 65536);
    FoxScopeInfo info{};
    info.structSize = sizeof(info);
    CHECK(check::waitFor([&] { return a->read_scope(s.raw(), FOXAPI_SCOPE_AUDIO, &info, buf.data(), 1024) == 1024; },
                         3000));
    CHECK_EQ(info.tap, FOXAPI_SCOPE_AUDIO);
    CHECK_EQ(info.channels, 1u);
    CHECK_EQ(info.rateHz, 48000.0);
    const uint64_t w0 = info.written;
    CHECK(check::waitFor(
        [&] {
            a->read_scope(s.raw(), FOXAPI_SCOPE_AUDIO, &info, buf.data(), 16);
            return info.written > w0;
        },
        2000));
    CHECK(check::waitFor([&] { return a->read_scope(s.raw(), FOXAPI_SCOPE_IQ, &info, buf.data(), 512) == 512; },
                         3000));
    CHECK_EQ(info.channels, 2u);
    CHECK(info.rateHz > 0.0);
    CHECK_EQ(a->read_scope(s.raw(), 99u, &info, buf.data(), 16), FOXAPI_UNSUPPORTED);
    CHECK_EQ(a->read_scope(s.raw(), FOXAPI_SCOPE_AUDIO, nullptr, buf.data(), 16), 16);  // info may be NULL
    FoxScopeInfo shortInfo{};
    shortInfo.structSize = 8;
    CHECK_EQ(a->read_scope(s.raw(), FOXAPI_SCOPE_AUDIO, &shortInfo, buf.data(), 16), FOXAPI_BAD_ARGUMENT);
    // REDUCED for a REMOTE session: once the tap holds more than that, a
    // LOCAL session is given what it asks and a REMOTE one no more.
    CHECK(check::waitFor([&] { return a->read_scope(s.raw(), FOXAPI_SCOPE_AUDIO, &info, buf.data(), 8192) == 8192; },
                         5000));
    const int32_t got = a->read_scope(r.raw(), FOXAPI_SCOPE_AUDIO, &info, buf.data(), 65536);
    std::printf("C: a remote session asked for 65536 audio frames and got %d\n", got);
    CHECK_EQ(got, static_cast<int32_t>(FOXAPI_SCOPE_REMOTE_FRAMES));
    s.run(cmd(FOXAPI_OP_RUN, 0, 0));
}

void testNodeSpectrum() {
    testing::Engine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    FoxSpectrumInfo info{};
    info.structSize = sizeof(info);
    float bins[64];
    CHECK_EQ(api()->read_node_spectrum(s.raw(), 1u, 0, &info, bins, 64), FOXAPI_NOT_FOUND);
    CHECK_EQ(count(s, FOXAPI_LIST_PATCH_RADIOS), 0);
    info.structSize = 4;
    CHECK_EQ(api()->read_node_spectrum(s.raw(), 1u, 0, &info, bins, 64), FOXAPI_BAD_ARGUMENT);
}

void testSerialPorts() {
    testing::Engine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    Remote r(e);
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SCAN_SERIAL_PORTS)).result.status, FOXAPI_OK);
    CHECK(count(s, FOXAPI_LIST_SERIAL_PORTS) >= 0);
    CHECK_EQ(r.run(cmd(FOXAPI_OP_SCAN_SERIAL_PORTS)).status(), FOXAPI_DENIED);
}

void testDecoderLines() {
    testing::Engine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK_EQ(count(s, FOXAPI_LIST_DECODER_LINES), 0);
    // Both in ONE batch, so both lines reach the list in the same pass.
    const FoxCommand two[2] = {cmd(FOXAPI_OP_SETTING_SET, 0, 0, "test.decoderLine=POCSAG|1234567: HELLO"),
                               cmd(FOXAPI_OP_SETTING_SET, 0, 0, "test.decoderLine=ACARS|.N123AB")};
    CHECK_EQ(api()->submit(s.raw(), two, 2, nullptr), 2);
    CHECK(check::waitFor([&] { return count(s, FOXAPI_LIST_DECODER_LINES) == 2; }, 2000));
    const FoxListItem l0 = item(s, FOXAPI_LIST_DECODER_LINES, 0);
    const FoxListItem l1 = item(s, FOXAPI_LIST_DECODER_LINES, 1);
    CHECK_EQ(std::string(l0.name), std::string("POCSAG"));
    CHECK_EQ(std::string(l0.detail), std::string("1234567: HELLO"));
    CHECK_EQ(std::string(l1.name), std::string("ACARS"));
    CHECK_EQ(l0.id, 1ull);       // line numbers, oldest first
    CHECK_EQ(l1.id, 2ull);
}

void testStore() {
    testing::Engine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK_EQ(count(s, FOXAPI_LIST_CATALOGUE), 0);
    const std::string legal = "You must hold a licence to decode this. " + std::string(400, 'L');
    FoxCommand c = cmd(FOXAPI_OP_SETTING_SET);
    CHECK_EQ(runText(s, c, "test.catalogue=demo.id|Demo decoder|" + legal).result.status, FOXAPI_OK);
    CHECK(check::waitFor([&] { return count(s, FOXAPI_LIST_CATALOGUE) == 1; }, 2000));
    const FoxListItem it = item(s, FOXAPI_LIST_CATALOGUE, 0);
    CHECK_EQ(std::string(it.name), std::string("Demo decoder"));
    CHECK_EQ(std::string(it.detail), std::string("demo.id"));
    CHECK((it.flags & FOXAPI_ITEM_INSTALLED) == 0u);
    CHECK_EQ(fact(s, "fact.store.demo.id.legal"), legal);
    int32_t st = 0;
    fact(s, "fact.store.other.id.legal", &st);
    CHECK_EQ(st, FOXAPI_NOT_FOUND);
    CHECK_EQ(count(s, FOXAPI_LIST_STORE_BLOCKED), 0);
    CHECK(count(s, FOXAPI_LIST_STORE_UPDATES) >= 0);
}

void testBiasTee() {
    testing::Engine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    const FoxReceiverState st = s.state();
    CHECK((st.flags & (FOXAPI_RX_BIAS_TEE_PRESENT | FOXAPI_RX_BIAS_TEE_ON)) == 0u);
    CHECK_EQ(fact(s, "fact.biasTee.reachable"), std::string("0"));
    CHECK((st.flags & FOXAPI_RX_PATCH_RUNNING) == 0u);
}

void testTransmitterState() {
    testing::Engine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    // No transmitter: nothing open, but a power setting is still the setting
    // (never the 0 dB - full power - a closed board's readback would say),
    // and it would transmit on the receiver's dial.
    CHECK_EQ(s.run(cmd(FOXAPI_OP_TX_SET_POWER, -40.0)).result.status, FOXAPI_OK);
    FoxReceiverState st = s.state();
    CHECK((st.flags & FOXAPI_RX_TX_AVAILABLE) == 0u);
    CHECK_EQ(std::string(st.txDeviceName), std::string());
    CHECK_EQ(st.txPowerDb, -40.0);
    CHECK_EQ(st.txFrequencyHz, st.tunedHz);
    CHECK((st.txFlags & FOXAPI_TX_POWER_RANGE) == 0u);
    // The dummy load (testHooks only) as the transmitter.
    CHECK_EQ(s.run(cmd(FOXAPI_OP_TX_OPEN, 0, 0, "test:tx:dummy")).result.status, FOXAPI_OK);
    st = s.state();
    CHECK((st.flags & FOXAPI_RX_TX_AVAILABLE) != 0u);
    CHECK_EQ(std::string(st.txDeviceName), std::string("Test transmitter (dummy load, no RF)"));
    CHECK((st.txFlags & FOXAPI_TX_POWER_RANGE) != 0u);
    CHECK_EQ(st.txPowerMinDb, -89.75);
    CHECK_EQ(st.txPowerMaxDb, 0.0);
    CHECK((st.txFlags & FOXAPI_TX_FAULTED) == 0u);
    CHECK_EQ(std::string(st.txError), std::string());
    // Settings, each read back in the state and each moving txSeq.
    uint64_t seq = st.txSeq;
    // The tone (the default: choosing the microphone would open this
    // machine's input device, which no test here does).
    CHECK_EQ(s.run(cmd(FOXAPI_OP_TX_SET_INPUT, 0, 1)).result.status, FOXAPI_OK);
    st = s.state();
    CHECK_EQ(st.txInput, 1u);
    CHECK_EQ(s.run(cmd(FOXAPI_OP_TX_SET_TONE, 1750.0)).result.status, FOXAPI_OK);
    st = s.state();
    CHECK_EQ(st.txToneHz, 1750.0);
    CHECK(st.txSeq > seq);
    seq = st.txSeq;
    CHECK_EQ(s.run(cmd(FOXAPI_OP_TX_SET_MONITOR, 0, 1)).result.status, FOXAPI_OK);
    st = s.state();
    CHECK((st.txFlags & FOXAPI_TX_MONITOR) != 0u);
    CHECK(st.txSeq > seq);
    // SPLIT: the transmitter leaves the receiver's dial.
    FoxCommand split = cmd(FOXAPI_OP_TX_SET_SPLIT, 435.0e6, 1);
    CHECK_EQ(s.run(split).result.status, FOXAPI_OK);
    st = s.state();
    CHECK((st.txFlags & FOXAPI_TX_SPLIT) != 0u);
    CHECK_EQ(st.txSplitHz, 435.0e6);
    CHECK_EQ(st.txFrequencyHz, 435.0e6);
    // ...and back: it follows the receiver again, retuned in the pass.
    CHECK_EQ(s.run(cmd(FOXAPI_OP_TX_SET_SPLIT, 0.0, 0)).result.status, FOXAPI_OK);
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SET_FREQUENCY, 145.5e6)).result.status, FOXAPI_OK);
    st = s.state();
    CHECK((st.txFlags & FOXAPI_TX_SPLIT) == 0u);
    CHECK_EQ(st.txFrequencyHz, st.tunedHz);
    // The input peak is a MEASUREMENT: it never moves txSeq by itself.
    CHECK(st.txInputPeak >= 0.0 && st.txInputPeak <= 1.0);
    CHECK_EQ(s.run(cmd(FOXAPI_OP_TX_CLOSE)).result.status, FOXAPI_OK);
}

// --- Y: the status column as its page draws it (API 0.3 8.2) ----------------------

// The state once it has stopped changing: the audio open the engine starts at
// boot (on a worker) and its first passes have landed - `seq` has held still
// for 300 ms - so a test that compares counters before and after a command
// is not comparing against a sink that was still opening.
FoxReceiverState settled(testing::Session& s) {
    using Clock = std::chrono::steady_clock;
    FoxReceiverState st = s.state();
    auto since = Clock::now();
    const auto end = since + std::chrono::seconds(10);
    while (Clock::now() < end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        FoxReceiverState now = s.state();
        if (now.seq != st.seq) {
            since = Clock::now();
        } else if (Clock::now() - since >= std::chrono::milliseconds(300)) {
            return now;
        }
        st = now;
    }
    return st;
}

int32_t setting(testing::Session& s, const std::string& keyValue) {
    return s.run(cmd(FOXAPI_OP_SETTING_SET, 0, 0, keyValue.c_str())).result.status;
}

// AUDIO and SINK and DECODER OUTPUT and DECODERS: the state's new members.
void testStatusColumnState() {
    testing::Engine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    FoxReceiverState st = settled(s);
    std::printf("Y: sink flags 0x%x, sinkName '%s', ring %llu of %llu frames at %.0f Hz, running %u of %u fitted (%u matched)\n",
                st.audioFlags, st.sinkName, static_cast<unsigned long long>(st.audioRingFrames),
                static_cast<unsigned long long>(st.audioRingCapacityFrames), st.audioRateHz, st.decodersRunning,
                st.decodersFitted, st.decodersMatched);

    // THE SINK'S WORDS with a hermetic engine: whether this machine has an
    // output device or not, they tell the same story as 0.2's own members.
    CHECK((st.audioFlags & FOXAPI_AUDIO_OPENING) == 0u);  // settled: no open is still going
    CHECK((st.audioFlags & FOXAPI_AUDIO_PLUGIN_PLAYING) == 0u);
    CHECK_EQ(std::string(st.audioPlayingPlugin), std::string());
    CHECK_EQ(std::string(st.audioMutedBy), std::string());
    const bool everOpened = (st.audioFlags & FOXAPI_AUDIO_EVER_OPENED) != 0u;
    // sinkName is the device the last successful open landed on: there is one
    // exactly when an open has succeeded.
    CHECK_EQ(everOpened, std::strlen(st.sinkName) != 0u);
    // ...a stream cannot be alive that was never opened, and the 0.2 flag for
    // an open sink says the same as the watchdog's view.
    CHECK_EQ((st.audioFlags & FOXAPI_AUDIO_STREAM_ALIVE) != 0u, (st.flags & FOXAPI_RX_SINK_OPEN) != 0u);
    if ((st.audioFlags & FOXAPI_AUDIO_STREAM_ALIVE) != 0u) { CHECK(everOpened); }

    // THE RING AND ITS RATE: the finished audio is 48 kHz; the ring's size is
    // fixed by the sink (present with or without a device) and its fill never
    // exceeds it; the level the AUDIO card letters is a real number of
    // milliseconds.
    CHECK_EQ(st.audioRateHz, 48000.0);
    CHECK(st.audioRingCapacityFrames > 0u);
    // ...and a ring that is being served is never found brim full: the fill
    // is not the size (it is 0 with no device, a few thousand frames with one).
    CHECK(st.audioRingFrames < st.audioRingCapacityFrames);
    const double capMs = 1000.0 * static_cast<double>(st.audioRingCapacityFrames) / st.audioRateHz;
    CHECK(capMs >= 10.0 && capMs <= 10000.0);

    // THE DECODERS: no plugin is loaded here. Running <= matched <= fitted
    // always (matched is the count of a stopped receiver too).
    CHECK_EQ(st.decodersFitted, 0u);
    CHECK_EQ(st.decodersMatched, 0u);
    CHECK(st.decodersRunning <= st.decodersMatched && st.decodersMatched <= st.decodersFitted);

    // DECODER OUTPUT: the total climbs with the lines the decoders emit, the
    // DECODER_LINES ids are numbered from the same counter, and a line - a
    // MEASUREMENT - moves neither seq nor audioSeq (only the lists' counter:
    // the line is a list entry).
    const FoxReceiverState base = settled(s);
    const FoxCommand three[3] = {cmd(FOXAPI_OP_SETTING_SET, 0, 0, "test.decoderLine=POCSAG|one"),
                                 cmd(FOXAPI_OP_SETTING_SET, 0, 0, "test.decoderLine=POCSAG|two"),
                                 cmd(FOXAPI_OP_SETTING_SET, 0, 0, "test.decoderLine=ACARS|three")};
    CHECK_EQ(api()->submit(s.raw(), three, 3, nullptr), 3);
    CHECK(check::waitFor([&] { return s.state().decoderLinesTotal == base.decoderLinesTotal + 3u; }, 2000));
    st = s.state();
    CHECK_EQ(st.decoderLinesTotal, base.decoderLinesTotal + 3u);
    CHECK_EQ(st.seq, base.seq);
    CHECK_EQ(st.audioSeq, base.audioSeq);
    // (API 0.3 8.7, D3: a line is a measurement and moves no list counter. It used to move listSeq,
    // and with it every cache an interface keeps of a list, once a line; an interface that wants the
    // backlog watches decoderLinesTotal. Group M4 holds it at 1,060 lines.)
    CHECK_EQ(st.listSeq, base.listSeq);
    const int32_t lines = count(s, FOXAPI_LIST_DECODER_LINES);
    CHECK(lines >= 3);
    CHECK_EQ(item(s, FOXAPI_LIST_DECODER_LINES, static_cast<uint32_t>(lines - 1)).id, st.decoderLinesTotal);
    // A RING OF 500 IS NOT A TOTAL: past the backlog's size the list stops
    // growing and the total keeps counting (a rate differenced over a window
    // must not flatten at 500).
    {
        const uint64_t before = st.decoderLinesTotal;
        const std::vector<FoxCommand> batch(100, cmd(FOXAPI_OP_SETTING_SET, 0, 0, "test.decoderLine=SPAM|x"));
        for (uint64_t round = 1; round <= 6; ++round) {
            CHECK_EQ(api()->submit(s.raw(), batch.data(), 100, nullptr), 100);
            CHECK(check::waitFor([&] { return s.state().decoderLinesTotal == before + 100u * round; }, 3000));
            FoxCommandResult drain[16];  // the answers are read, as a session must
            for (auto& x : drain) { x.structSize = sizeof(x); }
            while (api()->poll_results(s.raw(), drain, 16) > 0) {}
        }
        st = s.state();
        CHECK_EQ(st.decoderLinesTotal, before + 600u);
        CHECK_EQ(count(s, FOXAPI_LIST_DECODER_LINES), 500);
        CHECK_EQ(item(s, FOXAPI_LIST_DECODER_LINES, 499).id, st.decoderLinesTotal);
    }

    // WHO MUTES: the decoder plugins whose preset the receiver sits on. Two
    // of them, on one frequency, are named as the mute banner names them. The
    // text moves audioSeq (and so seq) once, when it appears and when it goes.
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SET_FREQUENCY, 100.0e6)).result.status, FOXAPI_OK);
    CHECK_EQ(setting(s, "test.muteBy=Alpha decoder@145500000;Beta decoder@145500000"), FOXAPI_OK);
    const FoxReceiverState off = settled(s);
    CHECK_EQ(std::string(off.audioMutedBy), std::string());  // not on their preset yet
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SET_FREQUENCY, 145.5e6)).result.status, FOXAPI_OK);
    CHECK(check::waitFor([&] { return std::string(s.state().audioMutedBy) == "Alpha decoder and Beta decoder"; }, 2000));
    const FoxReceiverState on = settled(s);
    CHECK_EQ(std::string(on.audioMutedBy), std::string("Alpha decoder and Beta decoder"));
    CHECK_EQ(on.audioSeq, off.audioSeq + 1u);
    CHECK(on.seq > off.seq);
    // Tuned away with the plugins still running, the mute's subject stays (the
    // audio is kept down until the operator answers); with them gone it ends.
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SET_FREQUENCY, 100.0e6)).result.status, FOXAPI_OK);
    CHECK_EQ(setting(s, "test.muteBy="), FOXAPI_OK);
    CHECK(check::waitFor([&] { return std::string(s.state().audioMutedBy).empty(); }, 2000));
    const FoxReceiverState gone = settled(s);
    CHECK_EQ(gone.audioSeq, on.audioSeq + 1u);
    // The USER'S mute is the 0.2 flag and not this text: it moves audioSeq all
    // the same, and leaves the subject empty.
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SET_MUTED, 0, 1)).result.status, FOXAPI_OK);
    st = s.state();
    CHECK((st.flags & FOXAPI_RX_MUTED) != 0u);
    CHECK_EQ(std::string(st.audioMutedBy), std::string());
    CHECK(st.audioSeq > gone.audioSeq);
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SET_MUTED, 0, 0)).result.status, FOXAPI_OK);

    // WHO PLAYS: a plugin holding the speakers replaces the receiver's audio.
    // The flag and the name arrive together (one audioSeq step); the gap
    // counters are MEASUREMENTS and move nothing when only they change. The
    // receiver is stopped first: a running one pulls blocks, and the first
    // block drops a stand-in that has no audio API behind it.
    CHECK_EQ(s.run(cmd(FOXAPI_OP_RUN, 0, 0)).result.status, FOXAPI_OK);
    const FoxReceiverState quiet = settled(s);
    CHECK_EQ(setting(s, "test.audioPlaying=DAB Radio|7|960"), FOXAPI_OK);
    CHECK(check::waitFor([&] { return (s.state().audioFlags & FOXAPI_AUDIO_PLUGIN_PLAYING) != 0u; }, 2000));
    const FoxReceiverState playing = settled(s);
    CHECK_EQ(std::string(playing.audioPlayingPlugin), std::string("DAB Radio"));
    CHECK_EQ(playing.audioPluginGaps, 7u);
    CHECK_EQ(playing.audioPluginGapFrames, 960u);
    CHECK_EQ(playing.audioSeq, quiet.audioSeq + 1u);
    CHECK_EQ(setting(s, "test.audioPlaying=DAB Radio|9|1200"), FOXAPI_OK);
    CHECK(check::waitFor([&] { return s.state().audioPluginGaps == 9u; }, 2000));
    st = settled(s);
    CHECK_EQ(st.audioPluginGapFrames, 1200u);
    CHECK_EQ(st.audioSeq, playing.audioSeq);  // only counters moved
    CHECK_EQ(st.seq, playing.seq);
    // ...and another plugin taking the speakers is the TEXT alone moving.
    CHECK_EQ(setting(s, "test.audioPlaying=Packet TNC|9|1200"), FOXAPI_OK);
    CHECK(check::waitFor([&] { return std::string(s.state().audioPlayingPlugin) == "Packet TNC"; }, 2000));
    const FoxReceiverState other = settled(s);
    CHECK((other.audioFlags & FOXAPI_AUDIO_PLUGIN_PLAYING) != 0u);
    CHECK_EQ(other.audioFlags, st.audioFlags);
    CHECK_EQ(other.audioSeq, st.audioSeq + 1u);
    CHECK_EQ(setting(s, "test.audioPlaying="), FOXAPI_OK);
    CHECK(check::waitFor([&] { return (s.state().audioFlags & FOXAPI_AUDIO_PLUGIN_PLAYING) == 0u; }, 2000));
    st = settled(s);
    CHECK_EQ(std::string(st.audioPlayingPlugin), std::string());
    CHECK_EQ(st.audioPluginGaps, 0u);
    CHECK_EQ(st.audioSeq, other.audioSeq + 1u);

    // THE STREAM DIES under a healthy receiver (a USB sound card
    // re-enumerating: the SINK card's STOPPED): the watchdog's view falls with
    // the 0.2 open flag while EVER_OPENED stays, and the engine's own watchdog
    // then reopens it. Only on a machine that has an output device to lose.
    if (everOpened) {
        CHECK((st.audioFlags & FOXAPI_AUDIO_STREAM_ALIVE) != 0u);
        CHECK_EQ(setting(s, "test.audioClose="), FOXAPI_OK);
        st = s.state();
        CHECK((st.audioFlags & FOXAPI_AUDIO_STREAM_ALIVE) == 0u);
        CHECK((st.audioFlags & FOXAPI_AUDIO_EVER_OPENED) != 0u);
        CHECK((st.flags & FOXAPI_RX_SINK_OPEN) == 0u);
        CHECK(check::waitFor([&] { return (s.state().audioFlags & FOXAPI_AUDIO_STREAM_ALIVE) != 0u; }, 15000));
        const FoxReceiverState back = settled(s);
        CHECK((back.audioFlags & FOXAPI_AUDIO_STREAM_ALIVE) != 0u);
        CHECK((back.audioFlags & FOXAPI_AUDIO_OPENING) == 0u);
        CHECK((back.flags & FOXAPI_RX_SINK_OPEN) != 0u);
    }
}

// RECEIVER: the GAINS entries carry the unit token.
void testStatusColumnGains() {
    testing::Engine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK_EQ(count(s, FOXAPI_LIST_GAINS), 0);  // no radio, no stages
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SCAN_DEVICES)).result.status, FOXAPI_OK);
    CHECK(check::waitFor([&] { return count(s, FOXAPI_LIST_DEVICES) == 2; }, 3000));
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SELECT_SOURCE, 0, 0, engine_api_test::kFakeRadioId)).result.status, FOXAPI_OK);
    CHECK(check::waitFor([&] { return (s.state().flags & FOXAPI_RX_DEVICE_OPEN) != 0u; }, 3000));
    CHECK(check::waitFor([&] { return count(s, FOXAPI_LIST_GAINS) == 1; }, 2000));
    // The fake radio's one stage is a gain in decibels.
    FoxListItem g = item(s, FOXAPI_LIST_GAINS, 0);
    CHECK_EQ(std::string(g.name), std::string("TUNER"));
    CHECK_EQ(std::string(g.detail), std::string("dB"));
    CHECK_EQ(g.value[1], 49.6);
    // A radio whose stages are the hardware's own steps (an Airspy's) says
    // so, and the block is rebuilt for it (listSeq).
    const uint64_t seq0 = s.state().listSeq;
    CHECK_EQ(setting(s, "test.gainUnit=step"), FOXAPI_OK);
    CHECK(check::waitFor([&] { return std::string(item(s, FOXAPI_LIST_GAINS, 0).detail) == "step"; }, 2000));
    CHECK(s.state().listSeq != seq0);
    g = item(s, FOXAPI_LIST_GAINS, 0);
    CHECK_EQ(std::string(g.name), std::string("TUNER"));  // the rest of the entry is as it was
    CHECK_EQ(g.value[1], 49.6);
    CHECK_EQ(setting(s, "test.gainUnit=dB"), FOXAPI_OK);
    CHECK(check::waitFor([&] { return std::string(item(s, FOXAPI_LIST_GAINS, 0).detail) == "dB"; }, 2000));
}

// RECORDER: RECORDINGS' value[3] is the take's seconds, read live.
void testStatusColumnRecordings() {
    testing::Engine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    using Clock = std::chrono::steady_clock;
    CHECK_EQ(s.run(cmd(FOXAPI_OP_RUN, 0, 1)).result.status, FOXAPI_OK);
    const auto t0 = Clock::now();
    CHECK_EQ(s.run(cmd(FOXAPI_OP_RECORD_IQ, 0, 1)).result.status, FOXAPI_OK);
    CHECK(check::waitFor([&] { return count(s, FOXAPI_LIST_RECORDINGS) == 1; }, 2000));
    const double v0 = item(s, FOXAPI_LIST_RECORDINGS, 0).value[3];
    CHECK(v0 >= 0.0);
    // It climbs while the take runs (Windows' timer tick is 15.6 ms: wait
    // well past one), without moving the list's counter.
    const uint64_t listSeq = s.state().listSeq;
    CHECK(check::waitFor([&] { return item(s, FOXAPI_LIST_RECORDINGS, 0).value[3] >= v0 + 0.15; }, 3000));
    CHECK_EQ(s.state().listSeq, listSeq);
    // ...and it is the take's own clock, not the engine's: never more than
    // the wall time since the command that started it.
    const FoxListItem live = item(s, FOXAPI_LIST_RECORDINGS, 0);
    const double wall = std::chrono::duration<double>(Clock::now() - t0).count();
    CHECK((live.flags & FOXAPI_ITEM_ACTIVE) != 0u);
    CHECK(live.value[3] > 0.0 && live.value[3] <= wall);
    // A second take, started later, has run for less: each entry keeps its own.
    CHECK_EQ(s.run(cmd(FOXAPI_OP_RECORD_AUDIO, 0, 1)).result.status, FOXAPI_OK);
    CHECK(check::waitFor([&] { return count(s, FOXAPI_LIST_RECORDINGS) == 2; }, 2000));
    CHECK(check::waitFor([&] { return item(s, FOXAPI_LIST_RECORDINGS, 1).value[3] >= 0.15; }, 3000));
    const FoxListItem iq = item(s, FOXAPI_LIST_RECORDINGS, 0);
    const FoxListItem au = item(s, FOXAPI_LIST_RECORDINGS, 1);
    CHECK_EQ(iq.value[0], 1.0);
    CHECK_EQ(au.value[0], 2.0);
    CHECK((iq.flags & FOXAPI_ITEM_ACTIVE) != 0u && (au.flags & FOXAPI_ITEM_ACTIVE) != 0u);
    CHECK(iq.value[3] > au.value[3]);
    // Stopped, a take keeps the length it had when it ended: fixed, not zero.
    CHECK_EQ(s.run(cmd(FOXAPI_OP_RECORD_IQ, 0, 0)).result.status, FOXAPI_OK);
    CHECK_EQ(s.run(cmd(FOXAPI_OP_RECORD_AUDIO, 0, 0)).result.status, FOXAPI_OK);
    CHECK(check::waitFor(
        [&] {
            return (item(s, FOXAPI_LIST_RECORDINGS, 0).flags & FOXAPI_ITEM_ACTIVE) == 0u &&
                   (item(s, FOXAPI_LIST_RECORDINGS, 1).flags & FOXAPI_ITEM_ACTIVE) == 0u;
        },
        2000));
    const FoxListItem iqEnd = item(s, FOXAPI_LIST_RECORDINGS, 0);
    const FoxListItem auEnd = item(s, FOXAPI_LIST_RECORDINGS, 1);
    CHECK(iqEnd.value[3] >= iq.value[3]);
    CHECK(auEnd.value[3] >= au.value[3]);
    CHECK(iqEnd.value[3] > auEnd.value[3]);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK_EQ(item(s, FOXAPI_LIST_RECORDINGS, 0).value[3], iqEnd.value[3]);
    CHECK_EQ(item(s, FOXAPI_LIST_RECORDINGS, 1).value[3], auEnd.value[3]);
    s.run(cmd(FOXAPI_OP_RUN, 0, 0));
}

// G: a session on an engine that has never run a pass - no list block has been
// published, so read_list answers from an EMPTY one whose bookmarks and presets
// pointers are null. gui::AppWindow opens its session exactly here
// (openUiSession, in its constructor, before the first frame), and
// configureApi drops the block again until the next publish. Nothing may be
// dereferenced: the lists the block carries are empty, and every list is
// served (a count, never an error).
void testListsBeforeFirstPublish() {
    const auto readEveryList = [](testing::Session& s, const char* when) {
        CHECK(s.ok());
        for (uint32_t l = 1; l <= FOXAPI_LIST_STORE_ACTIVITY; ++l) {
            if (l == 13u || l == FOXAPI_LIST_TRACKS) { continue; }  // retired / not served
            FoxListItem it{};
            it.structSize = sizeof(it);
            const int32_t n = api()->read_list(s.raw(), l, 0, &it);
            if (n < 0) { std::printf("  G (%s): list %u answered %d\n", when, l, n); }
            CHECK(n >= 0);
        }
    };
    {
        cascade::engine::Engine eng;  // built, never initialised, never passed
        testing::Session s(api(), eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
        // The reported case, on its own and at both calls a client makes.
        CHECK_EQ(api()->read_list(s.raw(), FOXAPI_LIST_BOOKMARKS, 0, nullptr), 0);
        FoxListItem it{};
        it.structSize = sizeof(it);
        CHECK_EQ(api()->read_list(s.raw(), FOXAPI_LIST_BOOKMARKS, 0, &it), 0);
        CHECK_EQ(api()->read_list(s.raw(), FOXAPI_LIST_BOOKMARK_GROUPS, 0, nullptr), 0);
        CHECK_EQ(api()->read_list(s.raw(), FOXAPI_LIST_PRESETS, 0, nullptr), 0);
        readEveryList(s, "fresh engine");
    }
    {
        cascade::engine::Engine eng;
        eng.configureApi(kHooks);  // the host is rebuilt: no block until the next publish
        testing::Session s(api(), eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
        CHECK_EQ(api()->read_list(s.raw(), FOXAPI_LIST_BOOKMARKS, 0, nullptr), 0);
        readEveryList(s, "after configureApi");
    }
}

// U: the usage report's panel names. The engine is driven from THIS thread -
// submit, then controlPass() until the ticket's result lands - so reading its
// list afterwards is not a race with a control thread.
testing::Outcome pumped(cascade::engine::Engine& eng, testing::Session& s, const FoxCommand& c) {
    testing::Outcome o;
    FoxSubmitResult sr{};
    sr.structSize = sizeof(sr);
    const int32_t n = api()->submit(s.raw(), &c, 1, &sr);
    o.submitStatus = n < 0 ? n : sr.status;
    if (o.submitStatus != FOXAPI_OK) { return o; }
    for (int pass = 0; pass < 20 && !o.landed; ++pass) {
        eng.controlPass();
        FoxCommandResult r[16];
        for (auto& x : r) { x.structSize = sizeof(x); }
        const int32_t got = api()->poll_results(s.raw(), r, 16);
        for (int32_t i = 0; i < got; ++i) {
            if (r[i].ticket == sr.ticket) {
                o.landed = true;
                o.result = r[i];
            }
        }
    }
    return o;
}

FoxCommand notePanel(const char* name) { return testing::cmd(FOXAPP_OP_NOTE_PANEL, 0.0, 0, name); }

void testNotePanel() {
    using Names = std::vector<std::string>;
    using Access = cascade::gui::AppWindowTestAccess;
    cascade::engine::Engine eng;
    eng.configureApi(kHooks);
    eng.initialise();
    eng.controlPass();
    // VIEW is the grant it needs, and all it needs.
    testing::Session s(api(), eng.apiHandle(), FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW);
    CHECK(s.ok());
    CHECK(Access::panels(eng).empty());

    // radar twice, cat once: listed once each, in the order first said.
    const testing::Outcome r1 = pumped(eng, s, notePanel("radar"));
    CHECK(r1.landed);
    CHECK_EQ(r1.status(), FOXAPI_OK);
    CHECK_EQ(pumped(eng, s, notePanel("radar")).status(), FOXAPI_OK);
    CHECK_EQ(pumped(eng, s, notePanel("cat")).status(), FOXAPI_OK);
    CHECK(Access::panels(eng) == (Names{"radar", "cat"}));

    // A REMOTE session holds VIEW too, and its pages are not this
    // installation's usage: refused, nothing listed.
    {
        testing::Session r(api(), eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW, "s3cret");
        CHECK(r.ok());
        CHECK_EQ(pumped(eng, r, notePanel("map")).status(), FOXAPI_DENIED);
        CHECK(Access::panels(eng) == (Names{"radar", "cat"}));
    }
    // (There is no session "without VIEW": open_session grants it to every
    // one. The grant is pinned the other way: the LOCAL session above holds
    // VIEW and nothing else, and its names were listed - an op needing
    // SETTINGS, the default, would have been DENIED to it.)
    // An empty name names no panel.
    CHECK_EQ(pumped(eng, s, notePanel("")).status(), FOXAPI_BAD_ARGUMENT);
    CHECK(Access::panels(eng) == (Names{"radar", "cat"}));

    // The cap: 16 names, then no more - and a name already there stays one.
    for (int i = 3; i <= 16; ++i) {
        CHECK_EQ(pumped(eng, s, notePanel(("panel " + std::to_string(i)).c_str())).status(), FOXAPI_OK);
    }
    CHECK_EQ(Access::panels(eng).size(), 16u);
    CHECK_EQ(pumped(eng, s, notePanel("panel 17")).status(), FOXAPI_OK);
    CHECK_EQ(pumped(eng, s, notePanel("radar")).status(), FOXAPI_OK);
    const Names full = Access::panels(eng);
    CHECK_EQ(full.size(), 16u);
    CHECK(std::find(full.begin(), full.end(), std::string("panel 17")) == full.end());
    CHECK_EQ(std::count(full.begin(), full.end(), std::string("radar")), 1);
    std::printf("U: radar x2 and cat x1 listed as {radar, cat}; the 17th name not listed (%zu names)\n",
                full.size());
}

// --- Z: the radio panels as their pages draw them (API 0.3 section 8.3) ------------------
//
// Window port group 4: the Airspy panel, the deck's bias tee key and the
// Converter rows draw from the table. Each group below drives the engine from
// THIS thread (controlPass until the thing has happened) against a fake radio,
// so what the test reads and what the friend sets is not a race with a control
// thread.

struct Direct {
    cascade::engine::Engine eng;
    Direct() {
        eng.configureApi(kHooks);
        eng.initialise();
        eng.controlPass();
    }
};

// Passes until `done` holds (at most timeoutMs).
template <class F>
bool passUntil(cascade::engine::Engine& eng, F done, int timeoutMs = 4000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end) {
        eng.controlPass();
        if (done()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

// Scans, selects `id` and passes until the radio is open.
bool openRadio(cascade::engine::Engine& eng, testing::Session& s, const std::string& id) {
    if (pumped(eng, s, testing::cmd(FOXAPI_OP_SCAN_DEVICES)).status() != FOXAPI_OK) { return false; }
    if (!passUntil(eng, [&] { return count(s, FOXAPI_LIST_DEVICES) == 4; })) { return false; }
    if (pumped(eng, s, testing::cmd(FOXAPI_OP_SELECT_SOURCE, 0, 0, id.c_str())).status() != FOXAPI_OK) {
        return false;
    }
    return passUntil(eng, [&] { return (s.state().flags & FOXAPI_RX_DEVICE_OPEN) != 0u; });
}

// The entry of `list` that is ACTIVE (a zeroed item when none is), and where.
FoxListItem activeItem(testing::Session& s, uint32_t list, int* index = nullptr) {
    const int32_t n = count(s, list);
    for (int32_t i = 0; i < n; ++i) {
        const FoxListItem it = item(s, list, static_cast<uint32_t>(i));
        if ((it.flags & FOXAPI_ITEM_ACTIVE) != 0u) {
            if (index != nullptr) { *index = i; }
            return it;
        }
    }
    if (index != nullptr) { *index = -1; }
    return FoxListItem{};
}

FoxCommand converterCmd(int mode, double loHz, int inverted) {
    FoxCommand c = testing::cmd(FOXAPP_OP_SET_CONVERTER, loHz, mode);
    c.ival[1] = inverted;
    return c;
}

// Z1 - N1: a radio is being opened. The open is held on its worker by the
// Airspy's wire; the state says so, in the device group, and the converter the
// page greys its controls for is refused meanwhile.
void testDeviceOpening() {
    using Access = cascade::gui::AppWindowTestAccess;
    Access::installRadios83();
    radios83::g_gateOpen = true;
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SCAN_DEVICES)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return count(s, FOXAPI_LIST_DEVICES) == 4; }));
    FoxReceiverState rest = s.state();
    CHECK((rest.flags & FOXAPI_RX_DEVICE_OPENING) == 0u);  // nothing opening at rest

    radios83::g_gateOpen = false;
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SELECT_SOURCE, 0, 0, radios83::kAirspyId.c_str())).status(),
             FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return (s.state().flags & FOXAPI_RX_DEVICE_OPENING) != 0u; }));
    FoxReceiverState opening = s.state();
    CHECK((opening.flags & FOXAPI_RX_DEVICE_OPEN) == 0u);  // still held
    // A device-group change: the flag alone moved deviceSeq (the radio is not open yet).
    CHECK(opening.deviceSeq > rest.deviceSeq);
    // The engine refuses a converter meanwhile (the reason the page is greyed).
    CHECK_EQ(pumped(d.eng, s, converterCmd(1, 125.0e6, 0)).status(), FOXAPI_BUSY);

    radios83::g_gateOpen = true;
    CHECK(passUntil(d.eng, [&] { return (s.state().flags & FOXAPI_RX_DEVICE_OPEN) != 0u; }));
    const FoxReceiverState open = s.state();
    CHECK((open.flags & FOXAPI_RX_DEVICE_OPENING) == 0u);  // the open has finished
    CHECK(open.deviceSeq > opening.deviceSeq);
    std::printf("Z1: OPENING while the Airspy's open was held (deviceSeq %llu -> %llu), clear once open (%llu)\n",
                static_cast<unsigned long long>(rest.deviceSeq), static_cast<unsigned long long>(opening.deviceSeq),
                static_cast<unsigned long long>(open.deviceSeq));
}

// Z2 - N7, and the contract the Airspy panel relies on: GAIN_MODES, DECIMATIONS
// and the stages. A stage its own AGC drives carries FOXAPI_ITEM_AGC, and
// switching an AGC moves listSeq.
void testAirspyLists() {
    using Access = cascade::gui::AppWindowTestAccess;
    Access::installRadios83();
    radios83::g_gateOpen = true;
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    CHECK_EQ(count(s, FOXAPI_LIST_GAIN_MODES), 0);  // not an Airspy: no panel
    CHECK(openRadio(d.eng, s, radios83::kAirspyId));
    d.eng.controlPass();

    // As it opens: Free mode, three stages in steps, no AGC.
    CHECK_EQ(count(s, FOXAPI_LIST_GAIN_MODES), 3);
    int at = -1;
    CHECK_EQ(activeItem(s, FOXAPI_LIST_GAIN_MODES, &at).value[0], 2.0);  // Free
    CHECK_EQ(count(s, FOXAPI_LIST_GAINS), 3);
    const auto stage = [&](uint32_t i) { return item(s, FOXAPI_LIST_GAINS, i); };
    CHECK_EQ(std::string(stage(0).name), std::string("LNA"));
    CHECK_EQ(std::string(stage(1).name), std::string("MIXER"));
    CHECK_EQ(std::string(stage(2).name), std::string("VGA"));
    for (uint32_t i = 0; i < 3; ++i) {
        CHECK_EQ(std::string(stage(i).detail), std::string("step"));
        CHECK((stage(i).flags & FOXAPI_ITEM_AGC) == 0u);
    }

    // The LNA's AGC: its stage (and only its stage) says so, and the block moved.
    const uint64_t seq0 = s.state().listSeq;
    FoxCommand agc = testing::cmd(FOXAPP_OP_AIRSPY_AGC, 0.0, 0);
    agc.ival[1] = 1;
    CHECK_EQ(pumped(d.eng, s, agc).status(), FOXAPI_OK);
    CHECK((stage(0).flags & FOXAPI_ITEM_AGC) != 0u);
    CHECK((stage(1).flags & FOXAPI_ITEM_AGC) == 0u);
    CHECK((stage(2).flags & FOXAPI_ITEM_AGC) == 0u);
    CHECK(s.state().listSeq != seq0);
    // The mixer's too; then the LNA's off again.
    agc.ival[0] = 1;
    CHECK_EQ(pumped(d.eng, s, agc).status(), FOXAPI_OK);
    CHECK((stage(0).flags & FOXAPI_ITEM_AGC) != 0u);
    CHECK((stage(1).flags & FOXAPI_ITEM_AGC) != 0u);
    CHECK((stage(2).flags & FOXAPI_ITEM_AGC) == 0u);
    const uint64_t seq1 = s.state().listSeq;
    agc.ival[0] = 0;
    agc.ival[1] = 0;
    CHECK_EQ(pumped(d.eng, s, agc).status(), FOXAPI_OK);
    CHECK((stage(0).flags & FOXAPI_ITEM_AGC) == 0u);
    CHECK((stage(1).flags & FOXAPI_ITEM_AGC) != 0u);
    CHECK(s.state().listSeq != seq1);
    agc.ival[0] = 1;
    CHECK_EQ(pumped(d.eng, s, agc).status(), FOXAPI_OK);
    CHECK((stage(1).flags & FOXAPI_ITEM_AGC) == 0u);

    // The decimation list: the factors in the combo's order, the active one with
    // the rate it gives; the board's own rate is that rate times the factor.
    const FoxReceiverState st0 = s.state();
    CHECK(count(s, FOXAPI_LIST_DECIMATIONS) > 1);
    int di = -1;
    const FoxListItem dec1 = activeItem(s, FOXAPI_LIST_DECIMATIONS, &di);
    CHECK_EQ(dec1.value[0], 1.0);
    CHECK_EQ(dec1.value[1], st0.sampleRateHz);
    const double hardware = dec1.value[1] * dec1.value[0];
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPP_OP_AIRSPY_DECIMATION, 0.0, 2)).status(), FOXAPI_OK);
    const FoxListItem dec2 = activeItem(s, FOXAPI_LIST_DECIMATIONS, &di);
    CHECK_EQ(dec2.value[0], 2.0);
    CHECK_EQ(dec2.value[1] * dec2.value[0], hardware);  // the same board rate
    CHECK_EQ(dec2.value[1], s.state().sampleRateHz);
    for (int32_t i = 0; i < count(s, FOXAPI_LIST_DECIMATIONS); ++i) {
        const FoxListItem e = item(s, FOXAPI_LIST_DECIMATIONS, static_cast<uint32_t>(i));
        CHECK_EQ(e.value[1] * e.value[0], hardware);
    }

    // Linearity: one stage, no AGC, and the mode list follows.
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPP_OP_AIRSPY_GAIN_MODE, 0.0, 0)).status(), FOXAPI_OK);
    CHECK_EQ(activeItem(s, FOXAPI_LIST_GAIN_MODES, &at).value[0], 0.0);
    CHECK_EQ(count(s, FOXAPI_LIST_GAINS), 1);
    CHECK_EQ(std::string(stage(0).name), std::string("LINEARITY"));
    CHECK((stage(0).flags & FOXAPI_ITEM_AGC) == 0u);
    std::printf("Z2: Airspy: LNA then MIXER AGC flagged on their stages (listSeq moved each time), decimation 2 "
                "keeps the board at %.0f S/s\n", hardware);
}

// Z3 - N2 and N6: the converter in force, and the radio's own centre, live.
void testConverterFacts() {
    using Access = cascade::gui::AppWindowTestAccess;
    Access::installRadios83();
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    CHECK(openRadio(d.eng, s, engine_api_test::kFakeRadioId));
    d.eng.controlPass();

    // Nothing set: the starting point of the controls. A radio with no AGC of its own carries no flag.
    CHECK_EQ(fact(s, "fact.converter.live"), std::string("0 0 0"));
    CHECK_EQ(count(s, FOXAPI_LIST_CONVERTERS), 0);
    CHECK_EQ(count(s, FOXAPI_LIST_GAINS), 1);
    CHECK((item(s, FOXAPI_LIST_GAINS, 0).flags & FOXAPI_ITEM_AGC) == 0u);

    // The converter in force is the PIPELINE's, not the stored setting: changed
    // alone (as a source install does), it still moves the block.
    Access::setPipelineConverter(d.eng, cascade::core::ConverterSetting{cascade::core::ConverterMode::Down, 7.0e6, false});
    d.eng.controlPass();
    CHECK_EQ(fact(s, "fact.converter.live"), std::string("2 7000000 0"));
    CHECK_EQ(count(s, FOXAPI_LIST_CONVERTERS), 0);  // nothing is stored
    Access::setPipelineConverter(d.eng, cascade::core::ConverterSetting{});
    d.eng.controlPass();
    CHECK_EQ(fact(s, "fact.converter.live"), std::string("0 0 0"));

    // The dialog's radio line follows the radio's NAME when only that changes.
    CHECK_EQ(fact(s, "fact.biasTee.radioName"), std::string("Mock RTL-SDR (serial 00000001)"));
    engine_api_test::fakeRadioNameOverride() = "Renamed radio";
    d.eng.controlPass();
    CHECK_EQ(fact(s, "fact.biasTee.radioName"), std::string("Renamed radio"));
    engine_api_test::fakeRadioNameOverride().clear();
    d.eng.controlPass();
    CHECK_EQ(fact(s, "fact.biasTee.radioName"), std::string("Mock RTL-SDR (serial 00000001)"));

    // An up-converter on 125 MHz.
    CHECK_EQ(pumped(d.eng, s, converterCmd(1, 125.0e6, 0)).status(), FOXAPI_OK);
    CHECK_EQ(fact(s, "fact.converter.live"), std::string("1 125000000 0"));
    const std::string name = fact(s, "fact.converter.name");
    CHECK(name.find("125 MHz") != std::string::npos);
    CHECK(name.find("up-converter") != std::string::npos);
    int at = -1;
    FoxListItem cv = activeItem(s, FOXAPI_LIST_CONVERTERS, &at);
    CHECK_EQ(count(s, FOXAPI_LIST_CONVERTERS), 1);
    CHECK_EQ(cv.value[0], 1.0);
    CHECK_EQ(cv.value[1], 125.0e6);
    CHECK_EQ(cv.value[2], 0.0);
    CHECK_EQ(std::string(cv.detail), fact(s, "fact.converter.radioKey"));
    FoxReceiverState st = s.state();
    // The radio's OWN readback (not the air frequency the state speaks): air + LO.
    CHECK(cv.value[3] != st.centreHz);
    CHECK_EQ(cv.value[3], st.centreHz + 125.0e6);

    // Inverting, on a higher oscillator: radio = LO - air.
    CHECK_EQ(pumped(d.eng, s, converterCmd(1, 250.0e6, 1)).status(), FOXAPI_OK);
    CHECK_EQ(fact(s, "fact.converter.live"), std::string("1 250000000 1"));
    cv = activeItem(s, FOXAPI_LIST_CONVERTERS, &at);
    CHECK_EQ(cv.value[2], 1.0);
    st = s.state();
    CHECK_EQ(cv.value[3], 250.0e6 - st.centreHz);

    // LIVE: a retune moves the radio's figure with no new block (no listSeq).
    const uint64_t listSeq = s.state().listSeq;
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SET_FREQUENCY, 110.0e6)).status(), FOXAPI_OK);
    const double radioBefore = cv.value[3];
    // (The tuned frequency is 110 MHz; the centre sits the VFO offset below it.)
    const bool followed = passUntil(d.eng, [&] {
        const FoxReceiverState now = s.state();
        return now.tunedHz == 110.0e6 && activeItem(s, FOXAPI_LIST_CONVERTERS).value[3] == 250.0e6 - now.centreHz;
    });
    const double radioAfter = activeItem(s, FOXAPI_LIST_CONVERTERS).value[3];
    std::printf("Z3: radio centre followed the retune to 110 MHz: %s (%.1f -> %.1f Hz; listSeq %llu -> %llu)\n",
                followed ? "yes" : "NO", radioBefore, radioAfter, static_cast<unsigned long long>(listSeq),
                static_cast<unsigned long long>(s.state().listSeq));
    CHECK(followed);
    CHECK(radioAfter != radioBefore);
    CHECK_EQ(s.state().listSeq, listSeq);

    // A setting remembered for another radio is not the open one's: no readback on it.
    Access::storeConverter(d.eng, "rtlsdr|serial=elsewhere", cascade::core::ConverterSetting{
                                                                   cascade::core::ConverterMode::Down, 50.0e6, false});
    d.eng.controlPass();
    CHECK_EQ(count(s, FOXAPI_LIST_CONVERTERS), 2);
    for (int32_t i = 0; i < 2; ++i) {
        const FoxListItem e = item(s, FOXAPI_LIST_CONVERTERS, static_cast<uint32_t>(i));
        if ((e.flags & FOXAPI_ITEM_ACTIVE) != 0u) {
            CHECK(e.value[3] != 0.0);
        } else {
            CHECK_EQ(std::string(e.detail), std::string("rtlsdr|serial=elsewhere"));
            CHECK_EQ(e.value[3], 0.0);
        }
    }

    // Off again: the live converter reads off, whatever is remembered.
    CHECK_EQ(pumped(d.eng, s, converterCmd(0, 250.0e6, 1)).status(), FOXAPI_OK);
    CHECK_EQ(fact(s, "fact.converter.live").substr(0, 2), std::string("0 "));
}

// Z4 - N3, N4, N5 and the sound card's fingerprint gap. No sound card backend
// runs here: the engine is told it is on one (the same members the window's
// tests set), and the facts are read back from the table.
void testConverterAliasAndCards() {
    using Access = cascade::gui::AppWindowTestAccess;
    using Fmt = cascade::source::SoundCardFormat;
    Access::installRadios83();
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    d.eng.controlPass();

    // The generator: every mode offered, nothing aliased, nothing ignored.
    CHECK_EQ(fact(s, "fact.converter.modes"), std::string("0,1,2"));
    CHECK_EQ(fact(s, "fact.converter.alias"), std::string());
    CHECK_EQ(fact(s, "fact.converter.upIgnored"), std::string("0"));

    // The alias note: said until the user sets a converter themselves...
    const std::string raw = Access::rawKey(d.eng);
    Access::notCarried(d.eng, raw);
    d.eng.controlPass();
    CHECK_EQ(fact(s, "fact.converter.alias"), std::string("notCarried"));
    CHECK_EQ(pumped(d.eng, s, converterCmd(1, 125.0e6, 0)).status(), FOXAPI_OK);
    CHECK_EQ(fact(s, "fact.converter.alias"), std::string());  // a converter set here ends the note
    // ...and "the converter still applies" while an alias is in force and one is on.
    Access::clearAlias(d.eng);
    Access::aliasKey(d.eng, raw, "native|serial=0001");
    d.eng.controlPass();
    CHECK_EQ(fact(s, "fact.converter.alias"), std::string("carried"));
    CHECK_EQ(pumped(d.eng, s, converterCmd(0, 125.0e6, 0)).status(), FOXAPI_OK);
    CHECK_EQ(fact(s, "fact.converter.alias"), std::string());  // off: nothing to carry
    Access::clearAlias(d.eng);
    d.eng.controlPass();

    // A sound card in real mode: Off and Down only; a stored Up is not used.
    Access::setSourceKind(d.eng, "soundcard");
    Access::setCard(d.eng, "Card A", "WASAPI", Fmt::RealMono);
    d.eng.controlPass();
    CHECK_EQ(fact(s, "fact.converter.modes"), std::string("0,2"));
    CHECK_EQ(fact(s, "fact.converter.upIgnored"), std::string("0"));
    const std::string keyA = fact(s, "fact.converter.radioKey");
    CHECK_EQ(keyA, Access::keyNow(d.eng));
    Access::storeConverter(d.eng, keyA, cascade::core::ConverterSetting{cascade::core::ConverterMode::Up, 125.0e6, false});
    d.eng.controlPass();
    CHECK_EQ(fact(s, "fact.converter.upIgnored"), std::string("1"));
    // The same card in I/Q mode: no converter at all, so nothing to ignore.
    Access::setCard(d.eng, "Card A", "WASAPI", Fmt::IqStereo);
    d.eng.controlPass();
    CHECK_EQ(fact(s, "fact.converter.modes"), std::string());
    CHECK_EQ(fact(s, "fact.converter.upIgnored"), std::string("0"));

    // A SECOND CARD has the same (empty) deviceArgs_ as the first: nothing else
    // moves, and the lists must still be rebuilt for it.
    Access::setCard(d.eng, "Card A", "WASAPI", Fmt::RealMono);
    d.eng.controlPass();
    const uint64_t seqA = s.state().listSeq;
    CHECK_EQ(fact(s, "fact.converter.upIgnored"), std::string("1"));
    Access::setCard(d.eng, "Card B", "WASAPI", Fmt::RealMono);
    d.eng.controlPass();
    CHECK(s.state().listSeq != seqA);
    const std::string keyB = fact(s, "fact.converter.radioKey");
    CHECK(keyB != keyA);
    CHECK_EQ(keyB, Access::keyNow(d.eng));
    CHECK_EQ(fact(s, "fact.converter.upIgnored"), std::string("0"));  // card B has none stored
    // The same name through another host API is another card too.
    const uint64_t seqB = s.state().listSeq;
    Access::setCard(d.eng, "Card B", "MME", Fmt::RealMono);
    d.eng.controlPass();
    CHECK(s.state().listSeq != seqB);
    CHECK(fact(s, "fact.converter.radioKey") != keyB);
    Access::setSourceKind(d.eng, "siggen");
    d.eng.controlPass();
    std::printf("Z4: alias \"\"/notCarried/carried, modes 0,1,2 / 0,2 / none, upIgnored, a second card moved listSeq\n");
}

// Z5 - N8 and N9: the bias tee's test stand-in, published by the engine, and
// the radio an ON may be asked about.
void testBiasStandIn() {
    using Access = cascade::gui::AppWindowTestAccess;
    Access::installRadios83();
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    d.eng.controlPass();
    const auto on = [&] { return (s.state().flags & FOXAPI_RX_BIAS_TEE_ON) != 0u; };
    const auto setOn = [&](int v, const char* radio = nullptr) {
        return pumped(d.eng, s, testing::cmd(FOXAPI_OP_SET_BIAS_TEE, 0.0, v, radio));
    };
    // No stand-in, no radio: no key.
    CHECK_EQ(fact(s, "fact.biasTee.reachable"), std::string("0"));
    CHECK_EQ(fact(s, "fact.biasTee.radioName"), std::string());
    CHECK(!on());

    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SETTING_SET, 0, 0, "test.biasStandIn=accept")).status(), FOXAPI_OK);
    d.eng.controlPass();
    const uint64_t listSeq = s.state().listSeq;
    CHECK_EQ(fact(s, "fact.biasTee.reachable"), std::string("1"));
    CHECK_EQ(fact(s, "fact.biasTee.mayRemember"), std::string("1"));  // a serial-named stand-in
    const std::string standKey = fact(s, "fact.biasTee.radioKey");
    CHECK(standKey.find("stand-in") != std::string::npos);
    CHECK_EQ(fact(s, "fact.biasTee.radioName"), std::string("stand-in radio (FOXSDR_FORCE_BIAS_KEY)"));
    CHECK(!on());  // it starts off

    // Switched through the command, read back through the state's flag.
    const uint64_t deviceSeq = s.state().deviceSeq;
    CHECK_EQ(setOn(1).status(), FOXAPI_OK);
    CHECK(on());
    CHECK(s.state().deviceSeq > deviceSeq);
    CHECK_EQ(setOn(0).status(), FOXAPI_OK);
    CHECK(!on());

    // N9: an ON that names another radio is refused, and switches nothing.
    const testing::Outcome wrong = setOn(1, "hackrf|serial=somebodyelse");
    CHECK_EQ(wrong.status(), FOXAPI_NOT_FOUND);
    CHECK_EQ(std::string(wrong.result.message), std::string("the radio changed"));
    CHECK((wrong.result.flags & FOXAPI_RESULT_REFUSED) != 0u);
    CHECK(!on());
    // The radio it was asked about: switched. (No text: not checked at all, as before.)
    CHECK_EQ(setOn(1, standKey.c_str()).status(), FOXAPI_OK);
    CHECK(on());
    CHECK_EQ(setOn(0).status(), FOXAPI_OK);
    CHECK_EQ(setOn(1).status(), FOXAPI_OK);
    CHECK(on());
    CHECK_EQ(setOn(0).status(), FOXAPI_OK);

    // The stand-in that refuses: the command is applied, the lamp stays dark and
    // the refusal is where every Source refusal is shown.
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SETTING_SET, 0, 0, "test.biasStandIn=refuse")).status(), FOXAPI_OK);
    d.eng.controlPass();
    CHECK_EQ(fact(s, "fact.biasTee.reachable"), std::string("1"));
    CHECK_EQ(setOn(1).status(), FOXAPI_OK);
    CHECK(!on());
    CHECK(fact(s, "fact.status.sourceError").find("stand-in bias tee") != std::string::npos);

    // Taken away: no key again.
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SETTING_SET, 0, 0, "test.biasStandIn=none")).status(), FOXAPI_OK);
    d.eng.controlPass();
    CHECK_EQ(fact(s, "fact.biasTee.reachable"), std::string("0"));
    CHECK_EQ(fact(s, "fact.biasTee.radioName"), std::string());
    CHECK(!on());
    std::printf("Z5: stand-in key reachable/off/on through SET_BIAS_TEE; a wrong radio refused \"%s\" (listSeq %llu -> %llu)\n",
                wrong.result.message, static_cast<unsigned long long>(listSeq),
                static_cast<unsigned long long>(s.state().listSeq));
}

// Z6 - N9 against the shipping HackRF driver on a fake wire, and the bias tee
// facts' fingerprint gaps: a radio that dies with nothing else changing, and a
// full memory of radios.
void testBiasRealDriver() {
    using Access = cascade::gui::AppWindowTestAccess;
    Access::installRadios83();
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    CHECK(openRadio(d.eng, s, radios83::kHackId));
    d.eng.controlPass();
    const auto on = [&] { return (s.state().flags & FOXAPI_RX_BIAS_TEE_ON) != 0u; };
    CHECK_EQ(fact(s, "fact.biasTee.reachable"), std::string("1"));
    CHECK_EQ(fact(s, "fact.biasTee.mayRemember"), std::string("1"));
    const std::string key = fact(s, "fact.biasTee.radioKey");
    CHECK(key.find("hackrf") != std::string::npos);
    // The radio's own name, as the state says it.
    CHECK(!fact(s, "fact.biasTee.radioName").empty());
    CHECK_EQ(fact(s, "fact.biasTee.radioName"), std::string(s.state().deviceName));
    CHECK(!on());

    // FULL MEMORY: the radio's "on" would not be kept, and the dialog must say so.
    // Nothing else changes, and the block is rebuilt for it.
    const uint64_t seq0 = s.state().listSeq;
    Access::fillBiasMemory(d.eng, cascade::core::kBiasTeeMemoryCap);
    d.eng.controlPass();
    CHECK_EQ(fact(s, "fact.biasTee.mayRemember"), std::string("0"));
    CHECK(s.state().listSeq != seq0);
    Access::fillBiasMemory(d.eng, 0);
    d.eng.controlPass();
    CHECK_EQ(fact(s, "fact.biasTee.mayRemember"), std::string("1"));

    // N9 against the real wire: an ON for another radio reaches the driver as nothing.
    radios83::hack()->clearControls();
    const testing::Outcome wrong =
        pumped(d.eng, s, testing::cmd(FOXAPI_OP_SET_BIAS_TEE, 0.0, 1, "airspy|serial=644866c83f1a51df"));
    CHECK_EQ(wrong.status(), FOXAPI_NOT_FOUND);
    CHECK(radios83::biasRequests().empty());
    CHECK(!on());
    // ...and for this one, it is switched: the driver is asked once and the lamp is its readback.
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SET_BIAS_TEE, 0.0, 1, key.c_str())).status(), FOXAPI_OK);
    CHECK(radios83::biasRequests() == std::vector<int>{1});
    CHECK(on());
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SET_BIAS_TEE, 0.0, 0)).status(), FOXAPI_OK);
    CHECK(!on());

    // A RADIO THAT DIES with no other change: the key goes (reachable "0") and
    // the lists are rebuilt for it. The failure is made behind the engine's
    // back, through the driver, so no status line or setting moves with it.
    const uint64_t seq1 = s.state().listSeq;
    radios83::hack()->failingRequests.push_back(radios83::kAntennaEnable);
    CHECK(Access::breakHackRf(d.eng));
    radios83::hack()->failingRequests.clear();
    d.eng.controlPass();
    CHECK_EQ(fact(s, "fact.biasTee.reachable"), std::string("0"));
    CHECK(s.state().listSeq != seq1);
    std::printf("Z6: HackRF: wrong radio refused with no request on the wire; a full memory and a dead radio moved listSeq\n");
}

// Z7: the converter's name is translated when the block is built, so a language
// switch has to rebuild it.
void testLanguageMovesNames() {
    using Access = cascade::gui::AppWindowTestAccess;
    Access::installRadios83();
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    CHECK_EQ(pumped(d.eng, s, converterCmd(1, 125.0e6, 0)).status(), FOXAPI_OK);
    std::string other;
    for (const cascade::i18n::Language& l : cascade::i18n::languages()) {
        if (l.code != "en") {
            other = l.code;
            break;
        }
    }
    CHECK(!other.empty());
    if (other.empty()) { return; }
    const std::string before = fact(s, "fact.converter.name");
    const uint64_t seq0 = s.state().listSeq;
    (void)cascade::i18n::setLanguage(other);
    d.eng.controlPass();
    const uint64_t seq1 = s.state().listSeq;
    const std::string translated = fact(s, "fact.converter.name");
    (void)cascade::i18n::setLanguage("en");
    d.eng.controlPass();
    std::printf("Z7: \"%s\" -> \"%s\" in %s (listSeq %llu -> %llu)\n", before.c_str(), translated.c_str(),
                other.c_str(), static_cast<unsigned long long>(seq0), static_cast<unsigned long long>(seq1));
    CHECK(seq1 != seq0);
    CHECK_EQ(fact(s, "fact.converter.name"), before);
}

// --- R: the receiver rail and the display as their pages draw them ---------------------
//
// Window port group 5, API 0.3 section 8.4. A hermetic engine on the signal
// generator hears no RDS, has no pilot to lock and no tone for the auto-notch,
// so the hooks stand in for what the DSP thread measures (test.rds,
// test.autoNotch, test.pilot); everything after the hook - the flag, the
// counter it moves, the bytes, the cut - is the engine's own.

const uint32_t kRdsAll = FOXAPI_RDS_SYNCED | FOXAPI_RDS_PI_VALID | FOXAPI_RDS_PS_VALID | FOXAPI_RDS_TP |
                         FOXAPI_RDS_TA | FOXAPI_RDS_RT_VALID;

// Which of the engine API's group counters differ between two reads, as words.
std::string countersMoved(const FoxReceiverState& a, const FoxReceiverState& b) {
    std::string o;
    const auto add = [&o](bool m, const char* w) {
        if (m) { o += o.empty() ? w : std::string(" ") + w; }
    };
    add(b.seq != a.seq, "seq");
    add(b.tuneSeq != a.tuneSeq, "tune");
    add(b.modeSeq != a.modeSeq, "mode");
    add(b.deviceSeq != a.deviceSeq, "device");
    add(b.audioSeq != a.audioSeq, "audio");
    add(b.displaySeq != a.displaySeq, "display");
    add(b.txSeq != a.txSeq, "tx");
    add(b.listSeq != a.listSeq, "list");
    add(b.rdsSeq != a.rdsSeq, "rds");
    add(b.scanSeq != a.scanSeq, "scan");
    return o;
}

// R1-R3: the RDS decoder's state.
void testRdsState() {
    testing::Engine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    FoxReceiverState st = settled(s);
    // A receiver that hears no RDS: every member empty, the counter at its start.
    CHECK_EQ(st.rdsFlags, 0u);
    CHECK_EQ(st.rdsPi, 0u);
    CHECK_EQ(st.rdsPty, 0u);
    CHECK_EQ(std::string(st.rdsPs), std::string());
    CHECK_EQ(std::string(st.rdsRadioText), std::string());
    CHECK_EQ(st.rdsGroups, 0ull);
    CHECK_EQ(st.rdsErrors, 0ull);
    CHECK_EQ(st.autoNotchHz, 0.0);
    CHECK(st.rdsSeq >= 1u);
    const FoxReceiverState base = st;

    // The decoder has everything: one pass publishes it all, and the RDS group
    // moves ONCE (not once per member), taking seq with it and nothing else.
    CHECK_EQ(setting(s, "test.rds=SPNTA;2C41;10;321;12;CASCADE!;STEREO RDS TEST "), FOXAPI_OK);
    CHECK(check::waitFor([&] { return s.state().rdsFlags != 0u; }, 2000));
    st = settled(s);
    CHECK_EQ(st.rdsFlags, kRdsAll);
    CHECK_EQ(st.rdsPi, 0x2C41u);
    CHECK_EQ(st.rdsPty, 10u);
    CHECK_EQ(std::string(st.rdsPs), std::string("CASCADE!"));
    CHECK_EQ(std::string(st.rdsRadioText), std::string("STEREO RDS TEST "));
    CHECK_EQ(st.rdsGroups, 321ull);
    CHECK_EQ(st.rdsErrors, 12ull);
    CHECK_EQ(st.rdsSeq, base.rdsSeq + 1u);
    CHECK_EQ(countersMoved(base, st), std::string("seq rds"));
    std::printf("R1-R3: RDS flags 0x%x, PI %04X, PTY %u, PS \"%s\", RT \"%s\", %llu groups, %llu errors; rdsSeq %llu -> %llu\n",
                st.rdsFlags, st.rdsPi, st.rdsPty, st.rdsPs, st.rdsRadioText,
                static_cast<unsigned long long>(st.rdsGroups), static_cast<unsigned long long>(st.rdsErrors),
                static_cast<unsigned long long>(base.rdsSeq), static_cast<unsigned long long>(st.rdsSeq));

    // The two counters are MEASUREMENTS: they climb and move no counter at all.
    CHECK_EQ(setting(s, "test.rds=SPNTA;2C41;10;400;20;CASCADE!;STEREO RDS TEST "), FOXAPI_OK);
    FoxReceiverState next = s.state();
    CHECK_EQ(next.rdsGroups, 400ull);
    CHECK_EQ(next.rdsErrors, 20ull);
    CHECK_EQ(countersMoved(st, next), std::string());

    // Each of the five that are not measurements moves the group, alone.
    const struct {
        const char* what;
        const char* hook;
    } one[] = {
        {"a flag (TA off)", "test.rds=SPNT;2C41;10;400;20;CASCADE!;STEREO RDS TEST "},
        {"the PI code", "test.rds=SPNT;2C42;10;400;20;CASCADE!;STEREO RDS TEST "},
        {"the PTY code", "test.rds=SPNT;2C42;11;400;20;CASCADE!;STEREO RDS TEST "},
        {"the service name", "test.rds=SPNT;2C42;11;400;20;CASCADE?;STEREO RDS TEST "},
        {"the RadioText growing", "test.rds=SPNT;2C42;11;400;20;CASCADE?;STEREO RDS TEST 2"},
    };
    for (const auto& o : one) {
        const FoxReceiverState before = s.state();
        CHECK_EQ(setting(s, o.hook), FOXAPI_OK);
        const FoxReceiverState after = s.state();
        std::printf("      %-28s -> [%s]\n", o.what, countersMoved(before, after).c_str());
        CHECK_EQ(countersMoved(before, after), std::string("seq rds"));
        CHECK_EQ(after.rdsSeq, before.rdsSeq + 1u);
    }
    // The same words again move nothing.
    {
        const FoxReceiverState before = s.state();
        CHECK_EQ(setting(s, one[4].hook), FOXAPI_OK);
        CHECK_EQ(countersMoved(before, s.state()), std::string());
    }
    // The bits are the decoder's own: TP/TA/sync/PI/PS each alone.
    CHECK_EQ(setting(s, "test.rds=S;0;0;0;0;;"), FOXAPI_OK);
    CHECK_EQ(s.state().rdsFlags, static_cast<uint32_t>(FOXAPI_RDS_SYNCED));
    CHECK_EQ(setting(s, "test.rds=P;1;0;0;0;;"), FOXAPI_OK);
    CHECK_EQ(s.state().rdsFlags, static_cast<uint32_t>(FOXAPI_RDS_PI_VALID));
    CHECK_EQ(setting(s, "test.rds=N;0;0;0;0;ABCDEFGH;"), FOXAPI_OK);
    CHECK_EQ(s.state().rdsFlags, static_cast<uint32_t>(FOXAPI_RDS_PS_VALID));
    CHECK_EQ(std::string(s.state().rdsPs), std::string("ABCDEFGH"));
    CHECK_EQ(setting(s, "test.rds=T;0;0;0;0;;"), FOXAPI_OK);
    CHECK_EQ(s.state().rdsFlags, static_cast<uint32_t>(FOXAPI_RDS_TP));
    CHECK_EQ(setting(s, "test.rds=A;0;0;0;0;;"), FOXAPI_OK);
    CHECK_EQ(s.state().rdsFlags, static_cast<uint32_t>(FOXAPI_RDS_TA));
    // RadioText sets its own bit (and only it) whatever else is heard; a ';' in it
    // is the text's.
    CHECK_EQ(setting(s, "test.rds=;0;0;0;0;;A;B"), FOXAPI_OK);
    CHECK_EQ(s.state().rdsFlags, static_cast<uint32_t>(FOXAPI_RDS_RT_VALID));
    CHECK_EQ(std::string(s.state().rdsRadioText), std::string("A;B"));
    // 64 characters fit; more are cut at 64 (and never overrun the field).
    const std::string text64(64, 'x');
    CHECK_EQ(setting(s, "test.rds=;0;0;0;0;;" + text64), FOXAPI_OK);
    CHECK_EQ(std::string(s.state().rdsRadioText), text64);
    CHECK_EQ(setting(s, "test.rds=;0;0;0;0;;" + text64 + "yyyyyy"), FOXAPI_OK);
    CHECK_EQ(std::string(s.state().rdsRadioText), text64);
    CHECK_EQ(std::strlen(s.state().rdsRadioText), 64u);
    // Programme service name: 8 characters and a NUL, cut there.
    CHECK_EQ(setting(s, "test.rds=N;0;0;0;0;123456789;"), FOXAPI_OK);
    CHECK_EQ(std::string(s.state().rdsPs), std::string("12345678"));
    // Handed back to the pipeline, which hears none: all empty again.
    const FoxReceiverState before = s.state();
    CHECK_EQ(setting(s, "test.rds="), FOXAPI_OK);
    st = s.state();
    CHECK_EQ(st.rdsFlags, 0u);
    CHECK_EQ(std::string(st.rdsPs), std::string());
    CHECK_EQ(std::string(st.rdsRadioText), std::string());
    CHECK_EQ(st.rdsGroups, 0ull);
    CHECK_EQ(st.rdsSeq, before.rdsSeq + 1u);
}

// R4: the auto-notch's readout.
void testAutoNotchReadout() {
    testing::Engine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    const FoxReceiverState base = settled(s);
    CHECK((base.flags & FOXAPI_RX_AUTO_NOTCH_ENGAGED) == 0u);
    CHECK_EQ(base.autoNotchHz, 0.0);

    // Enabled and searching: the filter's switch is on, nothing is engaged.
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SET_AUTO_NOTCH, 0, 1)).result.status, FOXAPI_OK);
    CHECK_EQ(setting(s, "test.autoNotch=searching"), FOXAPI_OK);
    FoxReceiverState st = settled(s);
    CHECK((st.flags & FOXAPI_RX_AUTO_NOTCH) != 0u);
    CHECK((st.flags & FOXAPI_RX_AUTO_NOTCH_ENGAGED) == 0u);
    CHECK_EQ(st.autoNotchHz, 0.0);

    // Engaged at 1000 Hz: the lamp is a MODE-group flag (modeSeq and seq, and
    // nothing else); "enabled" stays what it was.
    FoxReceiverState before = st;
    CHECK_EQ(setting(s, "test.autoNotch=1000"), FOXAPI_OK);
    st = s.state();
    CHECK((st.flags & FOXAPI_RX_AUTO_NOTCH_ENGAGED) != 0u);
    CHECK((st.flags & FOXAPI_RX_AUTO_NOTCH) != 0u);
    CHECK_EQ(st.autoNotchHz, 1000.0);
    CHECK_EQ(countersMoved(before, st), std::string("seq mode"));
    CHECK_EQ(st.modeSeq, before.modeSeq + 1u);
    std::printf("R4: engaged at %.0f Hz (flags 0x%08x), modeSeq %llu -> %llu\n", st.autoNotchHz, st.flags,
                static_cast<unsigned long long>(before.modeSeq), static_cast<unsigned long long>(st.modeSeq));

    // The frequency is a MEASUREMENT: it follows the tone and moves no counter.
    before = st;
    CHECK_EQ(setting(s, "test.autoNotch=1200"), FOXAPI_OK);
    st = s.state();
    CHECK_EQ(st.autoNotchHz, 1200.0);
    CHECK_EQ(countersMoved(before, st), std::string());

    // Not engaged: 0 whatever the pipeline last remembered (the tone it lost).
    before = st;
    CHECK_EQ(setting(s, "test.autoNotch=searching:800"), FOXAPI_OK);
    st = s.state();
    CHECK((st.flags & FOXAPI_RX_AUTO_NOTCH_ENGAGED) == 0u);
    CHECK_EQ(st.autoNotchHz, 0.0);
    CHECK_EQ(countersMoved(before, st), std::string("seq mode"));

    // Engaged does not need the switch to read as on: the flag is the pipeline's.
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SET_AUTO_NOTCH, 0, 0)).result.status, FOXAPI_OK);
    CHECK_EQ(setting(s, "test.autoNotch=900"), FOXAPI_OK);
    st = s.state();
    CHECK((st.flags & FOXAPI_RX_AUTO_NOTCH) == 0u);
    CHECK((st.flags & FOXAPI_RX_AUTO_NOTCH_ENGAGED) != 0u);
    // Handed back to the real pipeline, which has nothing engaged.
    CHECK_EQ(setting(s, "test.autoNotch="), FOXAPI_OK);
    st = s.state();
    CHECK((st.flags & FOXAPI_RX_AUTO_NOTCH_ENGAGED) == 0u);
    CHECK_EQ(st.autoNotchHz, 0.0);
}

// R7: the pilot lock, which STEREO_ACTIVE is a conjunction with.
void testPilotLock() {
    testing::Engine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    const FoxReceiverState base = settled(s);
    CHECK((base.flags & FOXAPI_RX_PILOT_LOCKED) == 0u);
    // Locked with stereo forced off: the third state the rail letters, "ST
    // (forced mono)" - the lock without the lamp.
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SET_STEREO, 0, 0)).result.status, FOXAPI_OK);
    const FoxReceiverState off = settled(s);
    CHECK((off.flags & FOXAPI_RX_STEREO_ENABLED) == 0u);
    CHECK_EQ(setting(s, "test.pilot=1"), FOXAPI_OK);
    FoxReceiverState st = s.state();
    CHECK((st.flags & FOXAPI_RX_PILOT_LOCKED) != 0u);
    CHECK((st.flags & FOXAPI_RX_STEREO_ACTIVE) == 0u);
    CHECK_EQ(countersMoved(off, st), std::string("seq"));
    std::printf("R7: pilot locked with stereo forced off: flags 0x%08x (seq %llu -> %llu, no group counter)\n", st.flags,
                static_cast<unsigned long long>(off.seq), static_cast<unsigned long long>(st.seq));
    // Unlocked again, and the real pipeline's answer (none) when handed back.
    CHECK_EQ(setting(s, "test.pilot=0"), FOXAPI_OK);
    CHECK((s.state().flags & FOXAPI_RX_PILOT_LOCKED) == 0u);
    CHECK_EQ(setting(s, "test.pilot=1"), FOXAPI_OK);
    CHECK_EQ(setting(s, "test.pilot="), FOXAPI_OK);
    CHECK((s.state().flags & FOXAPI_RX_PILOT_LOCKED) == 0u);
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SET_STEREO, 0, 1)).result.status, FOXAPI_OK);
}

// R5: the Sinks section's note is a status line.
void testAudioHealthLine() {
    testing::Engine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    FoxReceiverState st = settled(s);
    const bool everOpened = (st.audioFlags & FOXAPI_AUDIO_EVER_OPENED) != 0u;
    CHECK_EQ(count(s, FOXAPI_LIST_STATUS_LINES), 0);
    int32_t status = 0;
    CHECK_EQ(fact(s, "fact.status.audioHealth", &status), std::string());
    CHECK(status >= 0);  // a known line with nothing in it, not an unknown key

    const std::string note = "output device stopped and was restarted (1 time)";
    const uint64_t seq0 = st.listSeq;
    CHECK_EQ(setting(s, "test.statusLine=audioHealth|" + note), FOXAPI_OK);
    CHECK(check::waitFor([&] { return count(s, FOXAPI_LIST_STATUS_LINES) == 1; }, 2000));
    const FoxListItem it = item(s, FOXAPI_LIST_STATUS_LINES, 0);
    CHECK_EQ(std::string(it.name), std::string("audioHealth"));
    CHECK_EQ(std::string(it.detail), note);
    CHECK_EQ(it.value[0], 2.0);  // a warning, as the Sinks section letters it
    CHECK_EQ(fact(s, "fact.status.audioHealth"), note);
    CHECK(s.state().listSeq != seq0);
    std::printf("R5: audioHealth listed at level %.0f, \"%s\"\n", it.value[0], it.detail);

    // Beside another line, and whole when it is longer than a list item holds.
    const std::string longNote = std::string(200, 'n');
    CHECK_EQ(setting(s, "test.statusLine=presetNote|tuned"), FOXAPI_OK);
    CHECK_EQ(setting(s, "test.statusLine=audioHealth|" + longNote), FOXAPI_OK);
    CHECK(check::waitFor([&] { return count(s, FOXAPI_LIST_STATUS_LINES) == 2; }, 2000));
    CHECK_EQ(fact(s, "fact.status.audioHealth"), longNote);
    std::set<std::string> names;
    for (int i = 0; i < 2; ++i) { names.insert(item(s, FOXAPI_LIST_STATUS_LINES, static_cast<uint32_t>(i)).name); }
    CHECK(names == (std::set<std::string>{"presetNote", "audioHealth"}));
    // Taken down, the line goes.
    CHECK_EQ(setting(s, "test.statusLine=audioHealth|"), FOXAPI_OK);
    CHECK_EQ(setting(s, "test.statusLine=presetNote|"), FOXAPI_OK);
    CHECK(check::waitFor([&] { return count(s, FOXAPI_LIST_STATUS_LINES) == 0; }, 2000));
    CHECK_EQ(fact(s, "fact.status.audioHealth"), std::string());

    // The engine's own watchdog writes it: the stream dies under a healthy
    // receiver and comes back, and says so - on a machine that has an output
    // device to lose.
    if (everOpened) {
        CHECK_EQ(setting(s, "test.audioClose="), FOXAPI_OK);
        CHECK(check::waitFor([&] { return fact(s, "fact.status.audioHealth").find("restarted") != std::string::npos; },
                             20000));
        std::printf("      the watchdog's own note: \"%s\"\n", fact(s, "fact.status.audioHealth").c_str());
    }
}

// R8: the loaded band plan's name is not the active entry's.
void testBandPlanName() {
    testing::Engine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    const FoxReceiverState base = settled(s);
    int32_t status = 0;
    // No plans installed beside this executable: no plan, no name, a known fact.
    CHECK_EQ(count(s, FOXAPI_LIST_BAND_PLAN), 0);
    CHECK_EQ(fact(s, "fact.bandPlan.name", &status), std::string());
    CHECK(status >= 0);
    CHECK_EQ(setting(s, std::string("test.bandPlanDir=") + CASCADE_SOURCE_DIR + "/resources/bandplans"), FOXAPI_OK);
    CHECK(count(s, FOXAPI_LIST_BAND_PLANS) > 1);
    CHECK(s.state().listSeq != base.listSeq);
    // The default selection is the world plan, which refines nothing: the loaded
    // name and the ACTIVE entry's are the same words.
    {
        const FoxListItem act = activeItem(s, FOXAPI_LIST_BAND_PLANS);
        CHECK_EQ(std::string(act.detail), std::string("world"));
        CHECK_EQ(fact(s, "fact.bandPlan.name"), std::string(act.name));
        CHECK(count(s, FOXAPI_LIST_BAND_PLAN) > 0);
    }
    // No selection at all (the legacy setting): every installed plan merged, the
    // name every plan's joined, and none of them ACTIVE.
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SET_BAND_PLAN, 0, 0, "")).result.status, FOXAPI_OK);
    const std::string all = fact(s, "fact.bandPlan.name");
    std::printf("R8: nothing selected: \"%s\", %d bands, none ACTIVE\n", all.c_str(), count(s, FOXAPI_LIST_BAND_PLAN));
    CHECK(all.find(" + ") != std::string::npos);
    CHECK_EQ(activeItem(s, FOXAPI_LIST_BAND_PLANS).flags, 0u);
    CHECK(count(s, FOXAPI_LIST_BAND_PLAN) > 0);
    // The UK plan refines Region 1: the loaded name is the chain, the ACTIVE
    // entry is the one plan.
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SET_BAND_PLAN, 0, 0, "uk")).result.status, FOXAPI_OK);
    const std::string chain = fact(s, "fact.bandPlan.name");
    const FoxListItem act = activeItem(s, FOXAPI_LIST_BAND_PLANS);
    std::printf("R8: loaded \"%s\"; ACTIVE entry \"%s\" (%s); %d bands\n", chain.c_str(), act.name, act.detail,
                count(s, FOXAPI_LIST_BAND_PLAN));
    CHECK_EQ(chain, std::string("ITU Region 1 + United Kingdom"));
    CHECK_EQ(std::string(act.detail), std::string("uk"));
    CHECK(chain != std::string(act.name));
    // The rows are in the model's order: start ascending, the widest first on a
    // shared start - the back-to-front order a ribbon is drawn in.
    const int32_t n = count(s, FOXAPI_LIST_BAND_PLAN);
    CHECK(n > 10);
    FoxListItem prev = item(s, FOXAPI_LIST_BAND_PLAN, 0);
    for (int32_t i = 1; i < n; ++i) {
        const FoxListItem cur = item(s, FOXAPI_LIST_BAND_PLAN, static_cast<uint32_t>(i));
        const bool ordered = cur.value[0] > prev.value[0] || (cur.value[0] == prev.value[0] && cur.value[1] <= prev.value[1]);
        if (!ordered) { std::printf("      row %d (%s) is out of order\n", i, cur.name); }
        CHECK(ordered);
        prev = cur;
    }
}

// --- Q: the tuning surface (window port group 6, API 0.3 section 8.5) --------------------
//
// The toolbar, the frequency readout, the keys and the centre panels (the
// spectrum, the waterfall and the bookmark markers) read the table. It had all
// but two things: the bookmark store's version, which tells the window when to
// copy its markers again (listSeq also moves whenever a decoder prints a line),
// and the DSP chain's own input rate, which the spectrum's axis, the VFO's
// reach and the click-to-tune are measured against. Each is driven from the
// test thread (controlPass), so what is read is not a race with a control thread.

// A fact read as a decimal number: -1 when the engine does not serve it, -2
// when what it serves is not a whole number.
long long decimalFact(testing::Session& s, const char* key) {
    int32_t status = 0;
    const std::string v = fact(s, key, &status);
    if (status < 0) { return -1; }
    char* end = nullptr;
    const long long n = std::strtoll(v.c_str(), &end, 10);
    return (!v.empty() && end != nullptr && *end == '\0') ? n : -2;
}

// Q1: the version moves with the store and with nothing else.
void testBookmarksVersionFact() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    const long long v0 = decimalFact(s, "fact.bookmarks.version");
    std::printf("Q1: fact.bookmarks.version is %lld at the start\n", v0);
    CHECK(v0 >= 0);

    // Passes with nothing to say leave it, and listSeq, where they were.
    const std::uint64_t list0 = s.state().listSeq;
    for (int i = 0; i < 5; ++i) { d.eng.controlPass(); }
    CHECK_EQ(decimalFact(s, "fact.bookmarks.version"), v0);
    CHECK_EQ(s.state().listSeq, list0);

    // A decoder printing a line moves neither listSeq (API 0.3 8.7, D3: it used to, once a line, and
    // with it every cache a window keeps on listSeq - the markers' among them) nor the store's version:
    // the markers are not copied again for it, and not re-read either.
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SETTING_SET, 0, 0, "test.decoderLine=TEST|a line")).status(),
             FOXAPI_OK);
    const std::uint64_t list1 = s.state().listSeq;
    std::printf("Q1: a decoder line: listSeq %llu -> %llu, version %lld\n", static_cast<unsigned long long>(list0),
                static_cast<unsigned long long>(list1), decimalFact(s, "fact.bookmarks.version"));
    CHECK_EQ(list1, list0);
    CHECK_EQ(decimalFact(s, "fact.bookmarks.version"), v0);

    // An add moves it, and the list grows.
    const int32_t n0 = count(s, FOXAPI_LIST_BOOKMARKS);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SETTING_SET, 0, 0, "test.bookmark=Alpha|100300000|2|150000|"))
                 .status(),
             FOXAPI_OK);
    CHECK_EQ(count(s, FOXAPI_LIST_BOOKMARKS), n0 + 1);
    const long long v1 = decimalFact(s, "fact.bookmarks.version");
    CHECK(v1 > v0);
    CHECK(s.state().listSeq != list1);
    std::uint64_t id = 0;
    for (int32_t i = 0; i < count(s, FOXAPI_LIST_BOOKMARKS); ++i) {
        const FoxListItem it = item(s, FOXAPI_LIST_BOOKMARKS, static_cast<uint32_t>(i));
        if (std::string(it.name) == "Alpha") { id = it.id; }
    }
    CHECK(id != 0u);

    // A star on it: the list keeps its size and its order, the store changed.
    const std::uint64_t list2 = s.state().listSeq;
    FoxCommand star = testing::cmd(FOXAPI_OP_BOOKMARK_FAVOURITE, 0, static_cast<long long>(id));
    star.ival[1] = 1;
    CHECK_EQ(pumped(d.eng, s, star).status(), FOXAPI_OK);
    CHECK_EQ(count(s, FOXAPI_LIST_BOOKMARKS), n0 + 1);
    const long long v2 = decimalFact(s, "fact.bookmarks.version");
    std::printf("Q1: add: %lld -> %lld; a star (the list still has %d): -> %lld\n", v0, v1, n0 + 1, v2);
    CHECK(v2 > v1);
    CHECK(s.state().listSeq != list2);

    // A remove.
    const std::uint64_t list3 = s.state().listSeq;
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_BOOKMARK_REMOVE, 0, static_cast<long long>(id))).status(),
             FOXAPI_OK);
    CHECK_EQ(count(s, FOXAPI_LIST_BOOKMARKS), n0);
    CHECK(decimalFact(s, "fact.bookmarks.version") > v2);
    CHECK(s.state().listSeq != list3);
}

// Q2: the input rate is the chain's, to the digit, and moving it moves listSeq.
void testInputRateFact() {
    using Access = cascade::gui::AppWindowTestAccess;
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    int32_t status = 0;
    const std::string text0 = fact(s, "fact.input.rateHz", &status);
    CHECK(status >= 0);
    const double rate0 = std::strtod(text0.c_str(), nullptr);
    std::printf("Q2: fact.input.rateHz \"%s\"; the chain's %.17g, the source's %.17g\n", text0.c_str(),
                Access::chainRate(d.eng), s.state().sampleRateHz);
    CHECK(rate0 > 0.0);
    CHECK_EQ(rate0, Access::chainRate(d.eng));          // the digit it round-trips by
    CHECK_EQ(rate0, s.state().sampleRateHz);            // the chain follows the source at rest

    // The chain on another rate than the source (a refused rate change leaves it so): the fact
    // is the chain's - the one the display, the VFO's reach and the click-to-tune are measured
    // against - and sampleRateHz is still the source's.
    const double other = rate0 == 2.0e6 ? 1.0e6 : 2.0e6;
    const std::uint64_t list0 = s.state().listSeq;
    CHECK(Access::setChainRate(d.eng, other));
    for (int i = 0; i < 3; ++i) { d.eng.controlPass(); }
    CHECK_EQ(Access::chainRate(d.eng), other);
    const double rate1 = std::strtod(fact(s, "fact.input.rateHz").c_str(), nullptr);
    std::printf("Q2: chain moved to %.0f: fact %.17g, sampleRateHz %.17g, listSeq %llu -> %llu\n", other, rate1,
                s.state().sampleRateHz, static_cast<unsigned long long>(list0),
                static_cast<unsigned long long>(s.state().listSeq));
    CHECK_EQ(rate1, other);
    CHECK_EQ(s.state().sampleRateHz, rate0);
    CHECK(s.state().listSeq != list0);

    // Stable across passes that change nothing.
    const std::uint64_t list1 = s.state().listSeq;
    for (int i = 0; i < 5; ++i) { d.eng.controlPass(); }
    CHECK_EQ(s.state().listSeq, list1);
}

// Q3: the test seam: a frame of known bins in the ring, read through read_spectrum.
void testSpectrumSeam() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    std::vector<float> got(1024, -1.0f);
    FoxSpectrumInfo info{};
    info.structSize = sizeof(info);
    // No pipeline is running: no frame yet.
    CHECK_EQ(api()->read_spectrum(s.raw(), 0, &info, got.data(), 1024), FOXAPI_NO_CHANGE);
    std::vector<float> frame(1024);
    for (int k = 1; k <= 3; ++k) {
        for (int i = 0; i < 1024; ++i) { frame[static_cast<std::size_t>(i)] = static_cast<float>(k * 1000 + i) * 0.001f; }
        d.eng.api().publishSpectrumForTest(frame.data(), frame.size(), 100.3e6 + k, 2.4e6);
    }
    CHECK_EQ(api()->read_spectrum(s.raw(), 0, &info, got.data(), 1024), FOXAPI_OK);
    std::printf("Q3: three frames published; the newest read: seq %llu, %u bins, centre %.0f, span %.0f\n",
                static_cast<unsigned long long>(info.seq), info.copied, info.centreHz, info.spanHz);
    CHECK_EQ(info.seq, 3ull);
    CHECK_EQ(info.binCount, 1024u);
    CHECK_EQ(info.copied, 1024u);
    CHECK_EQ(info.centreHz, 100.3e6 + 3);
    CHECK_EQ(info.spanHz, 2.4e6);
    int wrong = 0;
    for (int i = 0; i < 1024; ++i) {
        if (got[static_cast<std::size_t>(i)] != static_cast<float>(3 * 1000 + i) * 0.001f) { ++wrong; }
    }
    CHECK_EQ(wrong, 0);
    // Nothing newer than a cursor at the newest; the one before it reads the newest again.
    CHECK_EQ(api()->read_spectrum(s.raw(), 3, &info, got.data(), 1024), FOXAPI_NO_CHANGE);
    CHECK_EQ(api()->read_spectrum(s.raw(), 2, &info, got.data(), 1024), FOXAPI_OK);
    CHECK_EQ(info.seq, 3ull);
}


// --- W: the scanner as its section draws it (window port group 7, API 0.3 section 8.6) ------
//
// The Scanner section letters its state and frequency and re-seeds its seven form fields from
// the engine. Driven from the test thread (controlPass), so what is read is not a race with a
// control thread. A hermetic engine hears no signal (the pipeline reads -200 dB while stopped),
// so the squelch is moved through the friend instead: the scanner stops on a signal when the
// pipeline's power is above the squelch.

FoxCommand scanConfigCmd(double dwell, double hold, double resume, double listen) {
    FoxCommand c = testing::cmd(FOXAPI_OP_SCANNER_CONFIG, dwell);
    c.num[1] = hold;
    c.num[2] = resume;
    c.num[3] = listen;
    return c;
}

FoxCommand scanRangeCmd(long long mask, double startHz, double stopHz, double stepHz) {
    FoxCommand c = testing::cmd(FOXAPP_OP_SCANNER_RANGE, startHz, mask);
    c.num[1] = stopHz;
    c.num[2] = stepHz;
    return c;
}

FoxCommand scanRunCmd(long long on, double startHz, double stopHz, double stepHz) {
    FoxCommand c = testing::cmd(FOXAPI_OP_SCANNER_RUN, startHz, on);
    c.num[1] = stopHz;
    c.num[2] = stepHz;
    return c;
}

// W1: what a scanner nobody has touched says.
void testScannerDefaults() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    d.eng.controlPass();
    const FoxReceiverState st = s.state();
    const cascade::core::Scanner::Params def;
    std::printf("W1: idle scanner: state %u, %.0f..%.0f Hz step %.0f, dwell %u hold %u resume %u listen %u, at %.0f Hz, scanSeq %llu\n",
                st.scannerState, st.scanStartHz, st.scanStopHz, st.scanStepHz, st.scanDwellMs, st.scanHoldMs,
                st.scanResumeMs, st.scanListenMs, st.scanCurrentHz, static_cast<unsigned long long>(st.scanSeq));
    CHECK_EQ(st.scannerState, FOXAPI_SCANNER_IDLE);
    CHECK((st.flags & FOXAPI_RX_SCANNER_ACTIVE) == 0u);
    CHECK_EQ(st.scanStartHz, def.startHz);
    CHECK_EQ(st.scanStopHz, def.stopHz);
    CHECK_EQ(st.scanStepHz, def.stepHz);
    CHECK_EQ(static_cast<double>(st.scanDwellMs), def.dwellMs);
    CHECK_EQ(static_cast<double>(st.scanHoldMs), def.holdMs);
    CHECK_EQ(static_cast<double>(st.scanResumeMs), def.resumeMs);
    CHECK_EQ(static_cast<double>(st.scanListenMs), def.listenMs);
    CHECK_EQ(st.scanCurrentHz, cascade::core::Scanner{}.currentHz());
    CHECK(st.scanSeq >= 1u);
    // Passes with nothing to say leave the group where it was.
    for (int i = 0; i < 5; ++i) { d.eng.controlPass(); }
    CHECK_EQ(countersMoved(st, s.state()), std::string());
}

// W2: the seven settings are their own group - one move for a command that changes any of
// them, none for one that changes nothing - and they read back as the engine stores them.
void testScannerSettings() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    d.eng.controlPass();

    FoxReceiverState before = s.state();
    CHECK_EQ(pumped(d.eng, s, scanConfigCmd(10, 20, 30, 40)).status(), FOXAPI_OK);
    FoxReceiverState after = s.state();
    CHECK_EQ(after.scanDwellMs, 10u);
    CHECK_EQ(after.scanHoldMs, 20u);
    CHECK_EQ(after.scanResumeMs, 30u);
    CHECK_EQ(after.scanListenMs, 40u);
    CHECK_EQ(countersMoved(before, after), std::string("seq scan"));
    CHECK_EQ(after.scanSeq, before.scanSeq + 1u);
    // The same command again changes nothing, so nothing moves.
    before = after;
    CHECK_EQ(pumped(d.eng, s, scanConfigCmd(10, 20, 30, 40)).status(), FOXAPI_OK);
    after = s.state();
    CHECK_EQ(countersMoved(before, after), std::string());

    // Each timing alone moves the group once.
    const double timings[4][4] = {{11, 20, 30, 40}, {11, 21, 30, 40}, {11, 21, 31, 40}, {11, 21, 31, 41}};
    for (const auto& t : timings) {
        before = s.state();
        CHECK_EQ(pumped(d.eng, s, scanConfigCmd(t[0], t[1], t[2], t[3])).status(), FOXAPI_OK);
        after = s.state();
        CHECK_EQ(countersMoved(before, after), std::string("seq scan"));
        CHECK_EQ(after.scanSeq, before.scanSeq + 1u);
    }

    // The range, by its mask: only the fields named change, as the form commits them.
    before = s.state();
    CHECK_EQ(pumped(d.eng, s, scanRangeCmd(7, 144.0e6, 146.0e6, 12.5e3)).status(), FOXAPI_OK);
    after = s.state();
    CHECK_EQ(after.scanStartHz, 144.0e6);
    CHECK_EQ(after.scanStopHz, 146.0e6);
    CHECK_EQ(after.scanStepHz, 12.5e3);
    CHECK_EQ(countersMoved(before, after), std::string("seq scan"));
    before = after;
    CHECK_EQ(pumped(d.eng, s, scanRangeCmd(1, 99.0e6, 1.0, 1.0)).status(), FOXAPI_OK);  // start only
    after = s.state();
    CHECK_EQ(after.scanStartHz, 99.0e6);
    CHECK_EQ(after.scanStopHz, 146.0e6);
    CHECK_EQ(after.scanStepHz, 12.5e3);
    CHECK_EQ(after.scanSeq, before.scanSeq + 1u);
    before = after;
    CHECK_EQ(pumped(d.eng, s, scanRangeCmd(2, 1.0, 147.0e6, 1.0)).status(), FOXAPI_OK);  // stop only
    CHECK_EQ(s.state().scanStopHz, 147.0e6);
    CHECK_EQ(s.state().scanStartHz, 99.0e6);
    CHECK_EQ(pumped(d.eng, s, scanRangeCmd(4, 1.0, 1.0, 25.0e3)).status(), FOXAPI_OK);  // step only
    CHECK_EQ(s.state().scanStepHz, 25.0e3);
    CHECK_EQ(s.state().scanStopHz, 147.0e6);
    // Not in the mask: nothing changes, nothing moves.
    before = s.state();
    CHECK_EQ(pumped(d.eng, s, scanRangeCmd(0, 1.0, 2.0, 3.0)).status(), FOXAPI_OK);
    CHECK_EQ(countersMoved(before, s.state()), std::string());

    // The range is published as the engine stores it (megahertz and kilohertz in the engine's
    // own fields, times the same factor out), so a value that is not exact in megahertz reads back
    // as the engine's product and not as the number that was sent.
    const double sent = 144.39e6;
    CHECK_EQ(pumped(d.eng, s, scanRangeCmd(1, sent, 1.0, 1.0)).status(), FOXAPI_OK);
    CHECK_EQ(s.state().scanStartHz, (sent / 1.0e6) * 1.0e6);
    // ...and a reversed range is published as stored (Scanner::configure swaps it when it runs).
    CHECK_EQ(pumped(d.eng, s, scanRangeCmd(3, 146.0e6, 144.0e6, 1.0)).status(), FOXAPI_OK);
    CHECK_EQ(s.state().scanStartHz, 146.0e6);
    CHECK_EQ(s.state().scanStopHz, 144.0e6);
}

// W3: the timings are whole milliseconds - rounded to the nearest, 0 for a negative,
// saturating at the top of a uint32.
void testScannerTimingsAreWholeMilliseconds() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    d.eng.controlPass();
    CHECK_EQ(pumped(d.eng, s, scanConfigCmd(12.4, 12.5, -5.0, 0.4)).status(), FOXAPI_OK);
    FoxReceiverState st = s.state();
    std::printf("W3: 12.4 -> %u, 12.5 -> %u, -5 -> %u, 0.4 -> %u\n", st.scanDwellMs, st.scanHoldMs, st.scanResumeMs,
                st.scanListenMs);
    CHECK_EQ(st.scanDwellMs, 12u);
    CHECK_EQ(st.scanHoldMs, 13u);
    CHECK_EQ(st.scanResumeMs, 0u);
    CHECK_EQ(st.scanListenMs, 0u);
    CHECK_EQ(pumped(d.eng, s, scanConfigCmd(4294967295.0, 4294967295.4, 1.0e12, 9.0e18)).status(), FOXAPI_OK);
    st = s.state();
    CHECK_EQ(st.scanDwellMs, 4294967295u);
    CHECK_EQ(st.scanHoldMs, 4294967295u);
    CHECK_EQ(st.scanResumeMs, 4294967295u);
    CHECK_EQ(st.scanListenMs, 4294967295u);
    // A value that is not finite never gets this far: the table refuses the command.
    FoxCommand nan = scanConfigCmd(1.0, 1.0, 1.0, 1.0);
    nan.num[2] = std::numeric_limits<double>::quiet_NaN();
    CHECK_EQ(pumped(d.eng, s, nan).status(), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(s.state().scanResumeMs, 4294967295u);
}

// W4: the scanner's state word and frequency follow the machine, through all four states, and
// move seq alone (the settings group stays where it was).
void testScannerStates() {
    using Access = cascade::gui::AppWindowTestAccess;
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    d.eng.controlPass();
    // No dwell, a long hold: every pass is a step, and a quiet spell is waited on.
    CHECK_EQ(pumped(d.eng, s, scanConfigCmd(0, 60000, 0, 0)).status(), FOXAPI_OK);

    FoxReceiverState before = s.state();
    CHECK_EQ(pumped(d.eng, s, scanRunCmd(1, 144.0e6, 144.05e6, 12.5e3)).status(), FOXAPI_OK);
    FoxReceiverState st = s.state();
    std::printf("W4: after RUN 1: state %u, at %.0f Hz, counters moved [%s]\n", st.scannerState, st.scanCurrentHz,
                countersMoved(before, st).c_str());
    CHECK_EQ(st.scannerState, FOXAPI_SCANNER_SCANNING);
    CHECK((st.flags & FOXAPI_RX_SCANNER_ACTIVE) != 0u);
    CHECK_EQ(st.scanCurrentHz, 144.0e6);  // the first tune
    CHECK_EQ(st.scanStartHz, 144.0e6);    // RUN carries the range, and it is kept
    CHECK_EQ(st.scanStopHz, 144.05e6);
    CHECK_EQ(st.scanStepHz, 12.5e3);
    CHECK(countersMoved(before, st).find("scan") != std::string::npos);

    // Stepping: the frequency walks the lattice and moves seq each time it changes, and never the
    // settings group. (The receiver's own tuning moves with it: that is the tune group's.)
    const std::uint64_t scan0 = st.scanSeq;
    std::set<double> seen;
    for (int i = 0; i < 12; ++i) {
        const FoxReceiverState prev = s.state();
        d.eng.controlPass();
        st = s.state();
        CHECK_EQ(st.scanSeq, scan0);
        CHECK_EQ(st.scannerState, FOXAPI_SCANNER_SCANNING);
        if (st.scanCurrentHz != prev.scanCurrentHz) { CHECK(st.seq != prev.seq); }
        const double k = (st.scanCurrentHz - 144.0e6) / 12.5e3;
        CHECK(k >= 0.0 && k <= 4.0 && k == static_cast<double>(static_cast<int>(k)));
        seen.insert(st.scanCurrentHz);
    }
    std::printf("W4: 12 passes stepped across %zu of the 5 frequencies of the range\n", seen.size());
    CHECK(seen.size() >= 4u);

    // A signal is found: Paused, held on that frequency.
    Access::setSquelch(d.eng, -1000.0f);
    d.eng.controlPass();
    FoxReceiverState paused = s.state();
    CHECK_EQ(paused.scannerState, FOXAPI_SCANNER_PAUSED);
    CHECK((paused.flags & FOXAPI_RX_SCANNER_ACTIVE) != 0u);
    d.eng.controlPass();
    d.eng.controlPass();
    CHECK_EQ(s.state().scannerState, FOXAPI_SCANNER_PAUSED);
    CHECK_EQ(s.state().scanCurrentHz, paused.scanCurrentHz);
    CHECK_EQ(countersMoved(paused, s.state()), std::string());  // nothing moves while it listens

    // It goes quiet: Holding, waiting for the station to come back.
    Access::setSquelch(d.eng, 20.0f);
    d.eng.controlPass();
    FoxReceiverState holding = s.state();
    CHECK_EQ(holding.scannerState, FOXAPI_SCANNER_HOLDING);
    CHECK_EQ(holding.scanCurrentHz, paused.scanCurrentHz);
    // The state word moved seq. (The squelch the test moved is published in the same pass, which is the
    // mode group's: what must NOT move is the scanner's settings, and the tuning.)
    {
        const std::string moved = countersMoved(paused, holding);
        CHECK(moved.find("seq") != std::string::npos);
        CHECK(moved.find("scan") == std::string::npos);
        CHECK(moved.find("tune") == std::string::npos);
    }
    CHECK_EQ(holding.scanSeq, scan0);

    // The operator's "next": back to Scanning, a step on.
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SCANNER_SKIP)).status(), FOXAPI_OK);
    st = s.state();
    CHECK_EQ(st.scannerState, FOXAPI_SCANNER_SCANNING);
    CHECK(st.scanCurrentHz != holding.scanCurrentHz);

    // Stopped: Idle, the flag clear, the frequency where the scan left it.
    const double last = st.scanCurrentHz;
    CHECK_EQ(pumped(d.eng, s, scanRunCmd(0, 0.0, 0.0, 0.0)).status(), FOXAPI_OK);
    st = s.state();
    CHECK_EQ(st.scannerState, FOXAPI_SCANNER_IDLE);
    CHECK((st.flags & FOXAPI_RX_SCANNER_ACTIVE) == 0u);
    CHECK_EQ(st.scanCurrentHz, last);
    // ...and the flag and the word agree in every state seen above (the flag is the word not Idle).
    d.eng.controlPass();
    CHECK_EQ(s.state().scanCurrentHz, last);
}

// --- J: the bookmark section's two lines (API 0.3 8.6, S2) ----------------------------------

void testBookmarkStatusLines() {
    using Access = cascade::gui::AppWindowTestAccess;
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    d.eng.controlPass();
    CHECK_EQ(count(s, FOXAPI_LIST_STATUS_LINES), 0);
    // The note: information; the error: red. Set as the hook stands in for whatever writes them.
    const std::string note = std::string("Removed 3 from \"") + std::string(200, 'g') + "\"";
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SETTING_SET, 0, 0, ("test.statusLine=bookmarkNote|" + note).c_str())).status(),
             FOXAPI_OK);
    CHECK_EQ(count(s, FOXAPI_LIST_STATUS_LINES), 1);
    FoxListItem it = item(s, FOXAPI_LIST_STATUS_LINES, 0);
    CHECK_EQ(std::string(it.name), std::string("bookmarkNote"));
    CHECK_EQ(it.value[0], 1.0);
    CHECK_EQ(fact(s, "fact.status.bookmarkNote"), note);  // whole in the fact (the item cuts at 159)
    CHECK(std::string(it.detail).size() == sizeof(it.detail) - 1);
    CHECK_EQ(fact(s, "fact.status.bookmarkError"), std::string());
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SETTING_SET, 0, 0, "test.statusLine=bookmarkError|bookmarks.json: disk full")).status(),
             FOXAPI_OK);
    CHECK_EQ(count(s, FOXAPI_LIST_STATUS_LINES), 2);
    std::set<std::string> names;
    double errorLevel = 0.0;
    for (int32_t i = 0; i < 2; ++i) {
        const FoxListItem x = item(s, FOXAPI_LIST_STATUS_LINES, static_cast<uint32_t>(i));
        names.insert(x.name);
        if (std::string(x.name) == "bookmarkError") { errorLevel = x.value[0]; }
    }
    CHECK(names == (std::set<std::string>{"bookmarkNote", "bookmarkError"}));
    CHECK_EQ(errorLevel, 3.0);
    CHECK_EQ(fact(s, "fact.status.bookmarkError"), std::string("bookmarks.json: disk full"));
    // The window's own copy of the lines (Engine::statusText) carries both.
    const cascade::engine::Engine::StatusText text = d.eng.statusText();
    CHECK_EQ(text.bookmarkNote, note);
    CHECK_EQ(text.bookmarkError, std::string("bookmarks.json: disk full"));
    // Cleared, they go from the list.
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SETTING_SET, 0, 0, "test.statusLine=bookmarkNote|")).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SETTING_SET, 0, 0, "test.statusLine=bookmarkError|")).status(), FOXAPI_OK);
    CHECK_EQ(count(s, FOXAPI_LIST_STATUS_LINES), 0);
    CHECK_EQ(d.eng.statusText().bookmarkNote, std::string());

    // The real writer: a group removed says how many (the note the section lettered from the
    // engine's own field), and the line follows it on the pass that wrote it.
    for (int i = 0; i < 3; ++i) {
        cascade::core::Bookmark b;
        b.name = "g" + std::to_string(i);
        b.freqHz = 100.0e6 + 1.0e6 * i;
        b.group = "Airband";
        Access::store(d.eng).add(b);
    }
    d.eng.controlPass();
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_BOOKMARK_REMOVE_GROUP, 0, 0, "Airband")).status(), FOXAPI_OK);
    CHECK_EQ(count(s, FOXAPI_LIST_STATUS_LINES), 1);
    CHECK_EQ(fact(s, "fact.status.bookmarkNote"), std::string("Removed 3 from \"Airband\""));
    CHECK_EQ(d.eng.statusText().bookmarkNote, std::string("Removed 3 from \"Airband\""));
}

// --- O: FOXAPP_OP_BOOKMARK_EXPORT (API 0.3 8.6, S3) -----------------------------------------

testing::Outcome pumpedText(cascade::engine::Engine& eng, testing::Session& s, FoxCommand c, const std::string& text) {
    testing::Outcome o;
    FoxSubmitResult sr{};
    sr.structSize = sizeof(sr);
    const int32_t n = api()->submit_text(s.raw(), &c, text.data(), static_cast<uint32_t>(text.size()), &sr);
    o.submitStatus = n < 0 ? n : sr.status;
    if (o.submitStatus != FOXAPI_OK) { return o; }
    for (int pass = 0; pass < 20 && !o.landed; ++pass) {
        eng.controlPass();
        FoxCommandResult r[16];
        for (auto& x : r) { x.structSize = sizeof(x); }
        const int32_t got = api()->poll_results(s.raw(), r, 16);
        for (int32_t i = 0; i < got; ++i) {
            if (r[i].ticket == sr.ticket) {
                o.landed = true;
                o.result = r[i];
            }
        }
    }
    return o;
}

std::vector<std::filesystem::path> exportedFiles(const std::string& dir) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        const std::string n = e.path().filename().string();
        if (n.rfind("foxsdr-frequencies-", 0) == 0 && e.path().extension() == ".xml") { out.push_back(e.path()); }
    }
    return out;
}

std::string slurp(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void testBookmarkExport() {
    using Access = cascade::gui::AppWindowTestAccess;
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    testing::Session remote(api(), d.eng.apiHandle(), FOXAPI_SESSION_REMOTE,
                            FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS, "s3cret");
    CHECK(remote.ok());
    d.eng.controlPass();
    const std::string dir = fact(s, "fact.recorder.dir");
    CHECK(!dir.empty());
    for (const auto& f : exportedFiles(dir)) { std::filesystem::remove(f); }

    // A list with what the table's BOOKMARKS item would cut or change: a name of 200 bytes (the
    // item holds 95), a group of 180 (it holds 159), a mode no table code names.
    struct Want {
        const char* tag;
        cascade::core::Bookmark b;
    };
    std::vector<Want> all;
    const auto mk = [&](const char* tag, const std::string& name, double hz, const char* mode, double bw,
                        const std::string& group, bool fav) {
        cascade::core::Bookmark b;
        b.name = name;
        b.freqHz = hz;
        b.mode = mode;
        b.bandwidthHz = bw;
        b.group = group;
        b.favourite = fav;
        all.push_back({tag, b});
    };
    mk("plain", "Radio 3", 100.3e6, "WFM", 150000.0, "", true);
    mk("longname", std::string(200, 'n'), 118.1e6, "AM", 10000.0, "Tower", false);
    mk("longgroup", "Ground", 121.9e6, "AM", 8333.0, std::string(180, 'g'), false);
    mk("futuremode", "Odd", 145.5e6, "ZZ-FUTURE", 12345.0, "Misc", true);
    mk("left out", "Not exported", 162.4e6, "NFM", 12500.0, "WX", false);
    mk("accents", "Caf\xC3\xA9 <1> & \"two\"", 433.92e6, "NFM", 12500.0, "Caf\xC3\xA9", false);
    std::map<std::string, std::uint64_t> ids;
    for (Want& w : all) {
        const int at = Access::store(d.eng).add(w.b);
        ids[w.tag] = Access::store(d.eng).list()[static_cast<std::size_t>(at)].id;
    }
    d.eng.controlPass();

    // The ids of five of them in no order but their own (the store's order is by frequency).
    const std::string idsText = std::to_string(ids["futuremode"]) + " " + std::to_string(ids["plain"]) + "  " +
                                std::to_string(ids["accents"]) + "\n" + std::to_string(ids["longgroup"]) + "," +
                                std::to_string(ids["longname"]) + " " + std::to_string(ids["plain"]);  // one twice
    const testing::Outcome o = pumpedText(d.eng, s, testing::cmd(FOXAPP_OP_BOOKMARK_EXPORT), idsText);
    CHECK_EQ(o.status(), FOXAPI_OK);
    const std::vector<std::filesystem::path> files = exportedFiles(dir);
    CHECK_EQ(files.size(), 1u);
    if (files.size() != 1u) { return; }
    const std::string xml = slurp(files[0]);
    const cascade::core::ImportResult back = cascade::core::importFrequencyList(xml);
    CHECK_EQ(back.items.size(), 5u);
    // In the store's order (by frequency), each whole.
    const char* order[] = {"plain", "longname", "longgroup", "futuremode", "accents"};
    for (std::size_t i = 0; i < 5 && i < back.items.size(); ++i) {
        const cascade::core::Bookmark* want = nullptr;
        for (const Want& w : all) {
            if (std::string(w.tag) == order[i]) { want = &w.b; }
        }
        const cascade::core::Bookmark& got = back.items[i];
        if (want == nullptr) { continue; }
        CHECK_EQ(got.name, want->name);
        CHECK_EQ(got.group, want->group);
        CHECK_EQ(got.mode, want->mode);
        CHECK_EQ(got.freqHz, want->freqHz);
        CHECK_EQ(got.bandwidthHz, want->bandwidthHz);
        CHECK_EQ(got.favourite, want->favourite);
    }
    // The file is exactly what exportSdrSharpXml writes for those five, in that order.
    std::vector<cascade::core::Bookmark> expect;
    for (const char* tag : order) {
        for (const Want& w : all) {
            if (std::string(w.tag) == tag) { expect.push_back(w.b); }
        }
    }
    CHECK_EQ(xml, cascade::core::exportSdrSharpXml(expect));
    // The name of the file, and the note that says where it went.
    const std::string fileName = files[0].filename().string();
    CHECK(fileName.size() == std::string("foxsdr-frequencies-YYYYMMDD-HHMMSS.xml").size());
    CHECK_EQ(fact(s, "fact.status.bookmarkNote"), "Exported 5 to " + dir + "/" + fileName);
    CHECK_EQ(Access::note(d.eng), "Exported 5 to " + dir + "/" + fileName);
    std::printf("O1: five of six bookmarks written whole to %s\n", fileName.c_str());

    // What it will not do. Nothing named, nothing found, a stranger: nothing is written, and each says why.
    for (const auto& f : exportedFiles(dir)) { std::filesystem::remove(f); }
    CHECK_EQ(pumpedText(d.eng, s, testing::cmd(FOXAPP_OP_BOOKMARK_EXPORT), "").status(), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(pumpedText(d.eng, s, testing::cmd(FOXAPP_OP_BOOKMARK_EXPORT), "no ids here at all").status(), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(pumpedText(d.eng, s, testing::cmd(FOXAPP_OP_BOOKMARK_EXPORT), "999999 999998 0").status(), FOXAPI_NOT_FOUND);
    CHECK_EQ(pumpedText(d.eng, s, testing::cmd(FOXAPP_OP_BOOKMARK_EXPORT), "99999999999999999999999 18446744073709551616").status(),
             FOXAPI_NOT_FOUND);
    CHECK_EQ(exportedFiles(dir).size(), 0u);
    // A REMOTE session may not (an app op: this machine's folder).
    CHECK_EQ(pumpedText(d.eng, remote, testing::cmd(FOXAPP_OP_BOOKMARK_EXPORT), idsText).status(), FOXAPI_DENIED);
    CHECK_EQ(exportedFiles(dir).size(), 0u);
    // One real id among strangers still exports that one.
    CHECK_EQ(pumpedText(d.eng, s, testing::cmd(FOXAPP_OP_BOOKMARK_EXPORT),
                        "7777777 " + std::to_string(ids["plain"]) + " 8888888").status(),
             FOXAPI_OK);
    CHECK_EQ(exportedFiles(dir).size(), 1u);
    CHECK(fact(s, "fact.status.bookmarkNote").rfind("Exported 1 to ", 0) == 0);

    // A folder that cannot be made: the note says so, the command is FAILED, and nothing was written.
    const std::filesystem::path blocker = std::filesystem::path(dir).parent_path() / "export_blocker.txt";
    {
        std::ofstream(blocker) << "a file where a folder would go";
    }
    Access::setRecordDir(d.eng, (blocker / "sub").string());
    CHECK_EQ(pumpedText(d.eng, s, testing::cmd(FOXAPP_OP_BOOKMARK_EXPORT), std::to_string(ids["plain"])).status(), FOXAPI_FAILED);
    CHECK(fact(s, "fact.status.bookmarkNote").rfind("Could not write ", 0) == 0);
    Access::setRecordDir(d.eng, dir);
    std::filesystem::remove(blocker);
}

// O2: a list of a hundred thousand exports in one pass over the store, not one search per id (a search per
// id measured 831 ms at 40 000 entries against 37 ms for the pass; at 100 000 it is some five seconds against
// about a tenth of one, which the ceiling below tells apart on a loaded machine too).
void testBookmarkExportCost() {
    using Access = cascade::gui::AppWindowTestAccess;
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    d.eng.controlPass();
    const std::string dir = fact(s, "fact.recorder.dir");
    for (const auto& f : exportedFiles(dir)) { std::filesystem::remove(f); }
    std::vector<cascade::core::Bookmark> many;
    many.reserve(100000);
    for (int i = 0; i < 100000; ++i) {
        cascade::core::Bookmark b;
        b.name = "ch" + std::to_string(i);
        b.freqHz = 30.0e6 + 1000.0 * i;
        b.group = "G" + std::to_string(i % 50);
        many.push_back(std::move(b));
    }
    CHECK_EQ(Access::store(d.eng).addMany(std::move(many)), 100000u);
    d.eng.controlPass();
    std::string idsText;
    for (const cascade::core::Bookmark& b : Access::store(d.eng).list()) {
        idsText += std::to_string(b.id);
        idsText += ' ';
    }
    CHECK(idsText.size() < FOXAPI_MAX_LONG_TEXT);
    const auto t0 = std::chrono::steady_clock::now();
    const testing::Outcome o = pumpedText(d.eng, s, testing::cmd(FOXAPP_OP_BOOKMARK_EXPORT), idsText);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK_EQ(o.status(), FOXAPI_OK);
    std::printf("O2: 100 000 ids (%zu bytes) exported in %.0f ms\n", idsText.size(), ms);
    CHECK(ms < 2500.0);
    const std::vector<std::filesystem::path> files = exportedFiles(dir);
    CHECK_EQ(files.size(), 1u);
    if (files.size() == 1u) {
        CHECK(slurp(files[0]) == cascade::core::exportSdrSharpXml(Access::store(d.eng).list()));
        std::filesystem::remove(files[0]);
    }
}

// --- M: the decoders, the presets, the marks, the plugin-mute cluster and the tune grants (8.7) ------
//
// Window port group 8. Each case drives the engine from THIS thread (controlPass until the thing has
// happened), as groups U and Z do, so what the test reads and what the friend sets is not a race with a
// control thread. The plugins are two REAL modules loaded from a scratch folder
// (tests/fixture_stage1_plugin.cpp: an audio decoder with two presets; tests/fixture_group8_plugin.cpp:
// an I/Q decoder with a device-centre preset, a preset the host drops, a preset with no label and two
// marks) and a third file that is not a module at all, which the host refuses.

std::vector<FoxEvent> takeEvents(testing::Session& s) {
    std::vector<FoxEvent> out;
    for (;;) {
        FoxEvent ev[16];
        for (auto& x : ev) { x.structSize = sizeof(x); }
        const int32_t got = api()->poll_events(s.raw(), ev, 16);
        for (int32_t i = 0; i < got; ++i) { out.push_back(ev[i]); }
        if (got < 16) { break; }
    }
    return out;
}

int tuneAways(const std::vector<FoxEvent>& v) {
    int n = 0;
    for (const FoxEvent& e : v) { n += e.kind == FOXAPI_EVENT_MUTE_TUNE_AWAY ? 1 : 0; }
    return n;
}

void passes(cascade::engine::Engine& eng, int n) {
    for (int i = 0; i < n; ++i) { eng.controlPass(); }
}

// The entry of `list` called `name` (a zeroed item, id 0 and no structSize, when there is none) and where.
FoxListItem entryNamed(testing::Session& s, uint32_t list, const std::string& name, int* index = nullptr) {
    const int32_t n = count(s, list);
    for (int32_t i = 0; i < n; ++i) {
        const FoxListItem it = item(s, list, static_cast<uint32_t>(i));
        if (name == it.name) {
            if (index != nullptr) { *index = i; }
            return it;
        }
    }
    if (index != nullptr) { *index = -1; }
    return FoxListItem{};
}

FoxCommand grantCmd(const std::string& file, int which, int on) {
    FoxCommand c = testing::cmd(FOXAPI_OP_PLUGIN_GRANT, 0, which, file.c_str());
    c.ival[1] = on;
    return c;
}

// M1: who is muting the audio (MUTE_CAUSES, fact.mute.*) and the tune-away event.
void testMuteCauses() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    testing::Session mute(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);  // never subscribes
    CHECK(mute.ok());
    CHECK_EQ(api()->subscribe(s.raw(), FOXAPI_EVENT_MASK(FOXAPI_EVENT_MUTE_TUNE_AWAY)), FOXAPI_OK);
    passes(d.eng, 3);
    int32_t st = 0;
    // Nothing is muting: an empty list, and both facts are SERVED (an engine that predates them says -1).
    CHECK_EQ(count(s, FOXAPI_LIST_MUTE_CAUSES), 0);
    CHECK_EQ(fact(s, "fact.mute.subject", &st), std::string());
    CHECK_EQ(st, 0);
    CHECK_EQ(fact(s, "fact.mute.keptRunning", &st), std::string("0"));
    CHECK_EQ(st, 1);
    CHECK_EQ(tuneAways(takeEvents(s)), 0);

    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SET_FREQUENCY, 100.0e6)).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s,
                    testing::cmd(FOXAPI_OP_SETTING_SET, 0, 0,
                                 "test.muteBy=Alpha decoder@145500000;Beta decoder@145500000"))
                 .status(),
             FOXAPI_OK);
    passes(d.eng, 3);
    CHECK_EQ(count(s, FOXAPI_LIST_MUTE_CAUSES), 0);  // not on their preset yet
    const uint64_t before = s.state().listSeq;
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SET_FREQUENCY, 145.5e6)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return count(s, FOXAPI_LIST_MUTE_CAUSES) == 2; }));
    // The list names the plugins, in the order the state's text does, with their module file names.
    const FoxListItem a = item(s, FOXAPI_LIST_MUTE_CAUSES, 0);
    const FoxListItem b = item(s, FOXAPI_LIST_MUTE_CAUSES, 1);
    CHECK_EQ(std::string(a.name), std::string("Alpha decoder"));
    CHECK_EQ(std::string(a.detail), std::string("Alpha decoder.dll"));
    CHECK_EQ(std::string(b.name), std::string("Beta decoder"));
    CHECK_EQ(std::string(b.detail), std::string("Beta decoder.dll"));
    CHECK((a.flags & FOXAPI_ITEM_ACTIVE) != 0u && (b.flags & FOXAPI_ITEM_ACTIVE) != 0u);
    CHECK_EQ(a.value[0], 0.0);  // on the preset: not a held mute
    CHECK_EQ(b.value[0], 0.0);
    CHECK(s.state().listSeq != before);  // the list moved listSeq
    CHECK_EQ(fact(s, "fact.mute.subject"), std::string("Alpha decoder and Beta decoder"));
    CHECK_EQ(std::string(s.state().audioMutedBy), fact(s, "fact.mute.subject"));  // the state's text, whole
    CHECK_EQ(fact(s, "fact.mute.keptRunning"), std::string("0"));
    CHECK_EQ(tuneAways(takeEvents(s)), 0);  // arriving on a preset is not a tune-away

    // TUNED AWAY with both still running: the mute is held, and the event is raised ONCE, by the publish
    // that shows the held mute - so the list already names them when the event arrives.
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SET_FREQUENCY, 100.0e6)).status(), FOXAPI_OK);
    {
        // The event is polled after EVERY pass, and the lists read the moment it arrives: that moment is the
        // point of raising it from the publish that shows the held mute.
        std::vector<FoxEvent> got;
        std::string keptWhenTold;
        double valueWhenTold = -1.0;
        for (int i = 0; i < 40; ++i) {
            got = takeEvents(s);
            if (!got.empty()) {
                keptWhenTold = fact(s, "fact.mute.keptRunning");
                valueWhenTold = item(s, FOXAPI_LIST_MUTE_CAUSES, 0).value[0];
                break;
            }
            d.eng.controlPass();
        }
        CHECK_EQ(tuneAways(got), 1);
        CHECK_EQ(keptWhenTold, std::string("1"));  // the lists already carry the held mute when it arrives
        CHECK_EQ(valueWhenTold, 1.0);
        for (const FoxEvent& e : got) {
            if (e.kind != FOXAPI_EVENT_MUTE_TUNE_AWAY) { continue; }
            CHECK_EQ(e.value[0], 2.0);
            CHECK_EQ(count(s, FOXAPI_LIST_MUTE_CAUSES), 2);  // named already
        }
        CHECK_EQ(takeEvents(mute).size(), 0u);  // a session that did not subscribe is told nothing
    }
    CHECK_EQ(fact(s, "fact.mute.keptRunning"), std::string("1"));
    CHECK_EQ(item(s, FOXAPI_LIST_MUTE_CAUSES, 0).value[0], 1.0);
    CHECK_EQ(item(s, FOXAPI_LIST_MUTE_CAUSES, 1).value[0], 1.0);
    CHECK_EQ(fact(s, "fact.mute.subject"), std::string("Alpha decoder and Beta decoder"));  // kept with the latch
    // ...and not again, however many passes go by.
    passes(d.eng, 30);
    CHECK_EQ(tuneAways(takeEvents(s)), 0);
    CHECK_EQ(count(s, FOXAPI_LIST_MUTE_CAUSES), 2);

    // "Keep it running": the mute stays (the banner's), nothing is raised.
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPP_OP_MUTE_KEEP_RUNNING)).status(), FOXAPI_OK);
    passes(d.eng, 5);
    CHECK_EQ(fact(s, "fact.mute.keptRunning"), std::string("1"));
    CHECK_EQ(count(s, FOXAPI_LIST_MUTE_CAUSES), 2);
    CHECK_EQ(tuneAways(takeEvents(s)), 0);

    // The banner's stop key: both plugins stopped, the mute ended, the list empty, the facts quiet.
    CHECK_EQ(pumped(d.eng, s,
                    testing::cmd(FOXAPP_OP_DECODER_STOP_LIST, 0, 1, "Alpha decoder.dll\nBeta decoder.dll"))
                 .status(),
             FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return count(s, FOXAPI_LIST_MUTE_CAUSES) == 0; }));
    CHECK_EQ(fact(s, "fact.mute.keptRunning"), std::string("0"));
    CHECK_EQ(fact(s, "fact.mute.subject", &st), std::string());
    CHECK_EQ(st, 0);

    // A SECOND tune-away is a second event (arriving on a preset re-arms), with one name this time.
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SETTING_SET, 0, 0, "test.muteBy=Alpha decoder@145500000"))
                 .status(),
             FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SET_FREQUENCY, 145.5e6)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return count(s, FOXAPI_LIST_MUTE_CAUSES) == 1; }));
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SET_FREQUENCY, 100.0e6)).status(), FOXAPI_OK);
    {
        std::vector<FoxEvent> got;
        for (int i = 0; i < 40 && got.empty(); ++i) {
            got = takeEvents(s);
            if (got.empty()) { d.eng.controlPass(); }
        }
        CHECK_EQ(tuneAways(got), 1);
        for (const FoxEvent& e : got) {
            if (e.kind == FOXAPI_EVENT_MUTE_TUNE_AWAY) { CHECK_EQ(e.value[0], 1.0); }
        }
    }
    CHECK(passUntil(d.eng, [&] { return fact(s, "fact.mute.keptRunning") == "1"; }));
    // Arriving back on the preset lets the latch go (the popup would be withdrawn): no event.
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SET_FREQUENCY, 145.5e6)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return fact(s, "fact.mute.keptRunning") == "0"; }));
    CHECK_EQ(item(s, FOXAPI_LIST_MUTE_CAUSES, 0).value[0], 0.0);
    CHECK_EQ(tuneAways(takeEvents(s)), 0);

    // A plugin that stopped muting before the receiver left its preset leaves nothing to ask about: the
    // tune-away raises no event and holds no mute.
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SETTING_SET, 0, 0, "test.muteBy=")).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SET_FREQUENCY, 100.0e6)).status(), FOXAPI_OK);
    passes(d.eng, 10);
    CHECK_EQ(tuneAways(takeEvents(s)), 0);
    CHECK_EQ(count(s, FOXAPI_LIST_MUTE_CAUSES), 0);
    CHECK_EQ(fact(s, "fact.mute.keptRunning"), std::string("0"));

    // A SUBJECT LONGER THAN THE STATE'S FIELD: the fact is whole, the state's audioMutedBy is cut at 127 bytes.
    {
        const std::string n1(70, 'a');
        const std::string n2(70, 'b');
        CHECK_EQ(pumped(d.eng, s,
                        testing::cmd(FOXAPI_OP_SETTING_SET, 0, 0,
                                     ("test.muteBy=" + n1 + "@145500000;" + n2 + "@145500000").c_str()))
                     .status(),
                 FOXAPI_OK);
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SET_FREQUENCY, 145.5e6)).status(), FOXAPI_OK);
        CHECK(passUntil(d.eng, [&] { return count(s, FOXAPI_LIST_MUTE_CAUSES) == 2; }));
        CHECK_EQ(fact(s, "fact.mute.subject"), n1 + " and " + n2);
        CHECK_EQ(std::strlen(s.state().audioMutedBy), 127u);
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SETTING_SET, 0, 0, "test.muteBy=")).status(), FOXAPI_OK);
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SET_FREQUENCY, 100.0e6)).status(), FOXAPI_OK);
        passes(d.eng, 6);
    }
    std::printf("M1: MUTE_CAUSES names the muting plugins, the latch flags them, one event a tune-away\n");
}

// The scratch plugin folder of group M: the two fixture modules, and a third file that is not one.
//
// Group 9 asks for more (`nine`): the third fixture, tests/fixture_group9_plugin.cpp (an audio processor with two
// command keys), and with `dotted` a second copy of it under a file name with dots in it, which the host turns
// off as the duplicate of a plugin it already runs.
struct PluginWorld {
    std::filesystem::path dir;
    static std::string hook;
    explicit PluginWorld(bool nine = false, bool dotted = false) {
        const char* base = std::getenv("LOCALAPPDATA");
        dir = std::filesystem::path(base != nullptr ? base : ".") / (nine ? "plugins_group9" : "plugins_group8");
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        std::vector<const char*> fixtures{FIXTURE_STAGE1_PLUGIN, FIXTURE_GROUP8_PLUGIN};
        if (nine) { fixtures.push_back(FIXTURE_GROUP9_PLUGIN); }
        for (const char* src : fixtures) {
            const std::filesystem::path p(src);
            std::filesystem::copy_file(p, dir / p.filename(), std::filesystem::copy_options::overwrite_existing, ec);
            CHECK(!ec);
        }
        if (dotted) {
            std::filesystem::copy_file(FIXTURE_GROUP9_PLUGIN, dir / "group.nine.v2.dll",
                                       std::filesystem::copy_options::overwrite_existing, ec);
            CHECK(!ec);
        }
        std::ofstream junk(dir / "zz_refused.dll", std::ios::binary);
        junk << "this is not a portable executable";
        junk.close();
        hook = dir.string();
        cascade::gui::AppWindowTestAccess::setPluginDir(&PluginWorld::path);
    }
    ~PluginWorld() {
        cascade::gui::AppWindowTestAccess::setPluginDir(nullptr);
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    static std::string path() { return hook; }
    static std::string fileOf(const char* fixture) { return std::filesystem::path(fixture).filename().string(); }
};
std::string PluginWorld::hook;

// M2: the plugins as the table lists them (PLUGINS' capability word and flags, DECODERS' MUTES), the grants
// (TUNE_ALLOWED, SETTINGS_ALLOWED, STALE_GRANTS) and what the last refused tune request was.
void testPluginLists() {
    PluginWorld world;
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    passes(d.eng, 3);
    const std::string stage1 = PluginWorld::fileOf(FIXTURE_STAGE1_PLUGIN);
    const std::string g8 = PluginWorld::fileOf(FIXTURE_GROUP8_PLUGIN);

    // PLUGINS: the host's order (by file name), the capability word, loaded -> ACTIVE.
    CHECK_EQ(count(s, FOXAPI_LIST_PLUGINS), 3);
    const FoxListItem pg = entryNamed(s, FOXAPI_LIST_PLUGINS, "Group Eight Fixture");
    const FoxListItem ps1 = entryNamed(s, FOXAPI_LIST_PLUGINS, "Stage One Fixture");
    const FoxListItem refused = entryNamed(s, FOXAPI_LIST_PLUGINS, "zz_refused.dll");
    CHECK_EQ(std::string(pg.detail), g8);
    CHECK_EQ(std::string(ps1.detail), stage1);
    CHECK_EQ(std::string(refused.detail), std::string("zz_refused.dll"));
    CHECK_EQ(static_cast<unsigned>(pg.value[0]),
             static_cast<unsigned>(CASCADE_CAP_IQ_DECODER | CASCADE_CAP_PRESET | CASCADE_CAP_HOST_CLIENT));
    CHECK_EQ(static_cast<unsigned>(ps1.value[0]),
             static_cast<unsigned>(CASCADE_CAP_DECODER | CASCADE_CAP_PRESET | CASCADE_CAP_HOST_CLIENT));
    CHECK_EQ(refused.value[0], 0.0);  // refused before a descriptor was accepted: no word to read
    CHECK((pg.flags & FOXAPI_ITEM_ACTIVE) != 0u && (ps1.flags & FOXAPI_ITEM_ACTIVE) != 0u);
    CHECK((refused.flags & FOXAPI_ITEM_ACTIVE) == 0u);
    const uint32_t grantBits = FOXAPI_ITEM_TUNE_ALLOWED | FOXAPI_ITEM_SETTINGS_ALLOWED | FOXAPI_ITEM_STOPPED;
    CHECK_EQ(pg.flags & grantBits, 0u);
    CHECK_EQ(ps1.flags & grantBits, 0u);

    // DECODERS: both modules, with MUTES at the capability default - an I/Q decoder mutes, an audio one does
    // not - and the toggle (FOXAPI_OP_PLUGIN_MUTE, named by file) moves it for one and not the other.
    CHECK_EQ(count(s, FOXAPI_LIST_DECODERS), 2);
    const auto muting = [&](const char* name) {
        return (entryNamed(s, FOXAPI_LIST_DECODERS, name).flags & FOXAPI_ITEM_MUTES) != 0u;
    };
    CHECK(muting("Group Eight Fixture"));
    CHECK(!muting("Stage One Fixture"));
    const uint64_t seq0 = s.state().listSeq;
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_PLUGIN_MUTE, 0, 1, stage1.c_str())).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK(muting("Stage One Fixture"));
    CHECK(muting("Group Eight Fixture"));
    CHECK(s.state().listSeq != seq0);  // the flag is in the fingerprint
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_PLUGIN_MUTE, 0, 0, g8.c_str())).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK(!muting("Group Eight Fixture"));  // the override of the default
    CHECK(muting("Stage One Fixture"));
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_PLUGIN_MUTE, 0, 0, stage1.c_str())).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_PLUGIN_MUTE, 0, 1, g8.c_str())).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK(!muting("Stage One Fixture") && muting("Group Eight Fixture"));

    // STOPPED follows STOP and START, on PLUGINS (any plugin) and as DECODERS' ACTIVE (a decoder running).
    const auto stopped = [&](const char* name) {
        return (entryNamed(s, FOXAPI_LIST_PLUGINS, name).flags & FOXAPI_ITEM_STOPPED) != 0u;
    };
    const auto running = [&](const char* name) {
        return (entryNamed(s, FOXAPI_LIST_DECODERS, name).flags & FOXAPI_ITEM_ACTIVE) != 0u;
    };
    CHECK(!stopped("Stage One Fixture") && running("Stage One Fixture"));
    const uint64_t seq1 = s.state().listSeq;
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_DECODER_STOP, 0, 0, stage1.c_str())).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK(stopped("Stage One Fixture") && !running("Stage One Fixture"));
    CHECK(!stopped("Group Eight Fixture") && running("Group Eight Fixture"));
    CHECK(s.state().listSeq != seq1);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_DECODER_START, 0, 0, stage1.c_str())).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK(!stopped("Stage One Fixture") && running("Stage One Fixture"));

    // THE GRANTS: tune and settings are separate item flags, set and revoked by FOXAPI_OP_PLUGIN_GRANT.
    const auto flag = [&](const char* name, uint32_t bit) {
        return (entryNamed(s, FOXAPI_LIST_PLUGINS, name).flags & bit) != 0u;
    };
    CHECK_EQ(pumped(d.eng, s, grantCmd(g8, 1, 1)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK(flag("Group Eight Fixture", FOXAPI_ITEM_TUNE_ALLOWED));
    CHECK(!flag("Group Eight Fixture", FOXAPI_ITEM_SETTINGS_ALLOWED));
    CHECK(!flag("Stage One Fixture", FOXAPI_ITEM_TUNE_ALLOWED));
    CHECK_EQ(pumped(d.eng, s, grantCmd(g8, 2, 1)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK(flag("Group Eight Fixture", FOXAPI_ITEM_SETTINGS_ALLOWED) &&
          flag("Group Eight Fixture", FOXAPI_ITEM_TUNE_ALLOWED));
    CHECK_EQ(pumped(d.eng, s, grantCmd(g8, 1, 0)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK(!flag("Group Eight Fixture", FOXAPI_ITEM_TUNE_ALLOWED));
    CHECK(flag("Group Eight Fixture", FOXAPI_ITEM_SETTINGS_ALLOWED));
    CHECK_EQ(pumped(d.eng, s, grantCmd(g8, 2, 0)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK_EQ(entryNamed(s, FOXAPI_LIST_PLUGINS, "Group Eight Fixture").flags & grantBits, 0u);

    // STALE_GRANTS: a tune grant no LOADED module holds - a module the host has no record of at all, and one
    // it refused - in the order the grants were given; a loaded module's grant is not stale.
    CHECK_EQ(count(s, FOXAPI_LIST_STALE_GRANTS), 0);
    CHECK_EQ(pumped(d.eng, s, grantCmd("removed-module.dll", 1, 1)).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, grantCmd("zz_refused.dll", 1, 1)).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, grantCmd(g8, 1, 1)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK_EQ(count(s, FOXAPI_LIST_STALE_GRANTS), 2);
    const FoxListItem gone = item(s, FOXAPI_LIST_STALE_GRANTS, 0);
    const FoxListItem broken = item(s, FOXAPI_LIST_STALE_GRANTS, 1);
    CHECK_EQ(std::string(gone.name), std::string("removed-module.dll"));
    CHECK((gone.flags & FOXAPI_ITEM_REFUSED) == 0u);  // no such file
    CHECK_EQ(std::string(broken.name), std::string("zz_refused.dll"));
    CHECK((broken.flags & FOXAPI_ITEM_REFUSED) != 0u);  // the file is there and the host refused it
    CHECK_EQ(pumped(d.eng, s, grantCmd("removed-module.dll", 1, 0)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK_EQ(count(s, FOXAPI_LIST_STALE_GRANTS), 1);
    CHECK_EQ(std::string(item(s, FOXAPI_LIST_STALE_GRANTS, 0).name), std::string("zz_refused.dll"));

    // THE LAST REFUSAL: "" until a module asks to tune without the grant, then the module file name (the key
    // the grant is held under); a rescan forgets it.
    using Access = cascade::gui::AppWindowTestAccess;
    CHECK_EQ(fact(s, "fact.plugin.lastDenied"), std::string());
    const uint64_t seq2 = s.state().listSeq;
    CHECK_EQ(Access::askToTune(d.eng, stage1), static_cast<int32_t>(CASCADE_TUNE_DENIED));
    passes(d.eng, 2);
    CHECK_EQ(fact(s, "fact.plugin.lastDenied"), stage1);
    CHECK(s.state().listSeq != seq2);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_PLUGIN_RESCAN)).status(), FOXAPI_OK);
    passes(d.eng, 3);
    CHECK_EQ(fact(s, "fact.plugin.lastDenied"), std::string());
    std::printf("M2: PLUGINS' flags and word, DECODERS' MUTES, STALE_GRANTS and the last refusal follow the engine\n");
}

// M3: PRESETS' device-centre flag and raw indexes, and MARKERS' kind and flags (what the plugin set).
void testPresetsAndMarks() {
    PluginWorld world;
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    passes(d.eng, 3);
    // PRESETS: the host drops the entry whose frequency is not one, so the raw indexes (the ids) skip it.
    int n8 = 0;
    std::vector<FoxListItem> g8;
    for (int32_t i = 0; i < count(s, FOXAPI_LIST_PRESETS); ++i) {
        const FoxListItem it = item(s, FOXAPI_LIST_PRESETS, static_cast<uint32_t>(i));
        if (std::string(it.detail) == "Group Eight Fixture") { g8.push_back(it); }
        ++n8;
    }
    CHECK_EQ(g8.size(), 2u);
    if (g8.size() == 2u) {
        CHECK_EQ(g8[0].id, 0u);
        CHECK_EQ(std::string(g8[0].name), std::string("G8 centre"));
        CHECK_EQ(g8[0].value[0], 1090.0e6);
        CHECK((g8[0].flags & FOXAPI_ITEM_DEVICE_CENTRE) != 0u);
        CHECK_EQ(g8[1].id, 2u);  // raw index 2: index 1 was dropped
        CHECK_EQ(std::string(g8[1].name), std::string());  // no label of its own: the interface supplies the plugin's
        CHECK_EQ(g8[1].value[0], 162.4e6);
        CHECK_EQ(g8[1].value[2], 25000.0);
        CHECK((g8[1].flags & FOXAPI_ITEM_DEVICE_CENTRE) == 0u);
    }
    CHECK_EQ(n8, 4);  // the other module's two as well
    for (int32_t i = 0; i < count(s, FOXAPI_LIST_PRESETS); ++i) {
        const FoxListItem it = item(s, FOXAPI_LIST_PRESETS, static_cast<uint32_t>(i));
        if (std::string(it.detail) == "Stage One Fixture") { CHECK((it.flags & FOXAPI_ITEM_DEVICE_CENTRE) == 0u); }
    }

    // MARKERS: the two marks the module set when it was attached - a plain point with the host's own colour,
    // and a dashed, spectrum-only span with a colour of its own.
    CHECK(passUntil(d.eng, [&] { return count(s, FOXAPI_LIST_MARKERS) == 2; }));
    FoxListItem point{}, span{};
    for (int32_t i = 0; i < count(s, FOXAPI_LIST_MARKERS); ++i) {
        const FoxListItem it = item(s, FOXAPI_LIST_MARKERS, static_cast<uint32_t>(i));
        (it.id == 6u ? point : span) = it;
    }
    CHECK_EQ(point.id, 6u);
    CHECK_EQ(std::string(point.name), std::string("G8 mark"));
    CHECK_EQ(std::string(point.detail), std::string("Group Eight Fixture"));
    CHECK_EQ(point.value[0], 100.1e6);
    CHECK_EQ(point.value[2], 0.0);
    CHECK_EQ(point.flags & (FOXAPI_ITEM_SPAN | FOXAPI_ITEM_DASHED | FOXAPI_ITEM_SPECTRUM_ONLY), 0u);
    CHECK_EQ(span.id, 5u);
    CHECK_EQ(std::string(span.name), std::string("G8 span"));
    CHECK_EQ(span.value[0], 162.4e6);
    CHECK_EQ(span.value[1], 25000.0);
    CHECK_EQ(span.value[2], static_cast<double>(0x11223344u));
    CHECK_EQ(span.flags & (FOXAPI_ITEM_SPAN | FOXAPI_ITEM_DASHED | FOXAPI_ITEM_SPECTRUM_ONLY),
             FOXAPI_ITEM_SPAN | FOXAPI_ITEM_DASHED | FOXAPI_ITEM_SPECTRUM_ONLY);
    std::printf("M3: PRESETS' device-centre flag and raw ids, MARKERS' kind and flags\n");
}

// A host whose track-info plugin has something to say (the window's own does, through its cache).
struct TrackInfoHost : cascade::engine::EngineHost {
    std::vector<std::string> pending;
    std::vector<std::string> drainTrackInfoText() override {
        std::vector<std::string> out;
        out.swap(pending);
        return out;
    }
};

// M4 (D3): a decoder line moves decoderLinesTotal and NOT listSeq; the lines still reach the backlog, track-info
// status lines among them; anything else that changes a list still moves listSeq.
void testDecoderLinesLeaveListSeqAlone() {
    TrackInfoHost host;
    cascade::engine::Engine eng(host);
    eng.configureApi(kHooks);
    eng.initialise();
    eng.controlPass();
    testing::Session s(api(), eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    passes(eng, 3);
    const FoxReceiverState base = s.state();
    for (int i = 0; i < 60; ++i) {
        CHECK_EQ(pumped(eng, s, testing::cmd(FOXAPI_OP_SETTING_SET, 0, 0, "test.decoderLine=POCSAG|a line")).status(),
                 FOXAPI_OK);
    }
    const FoxReceiverState after = s.state();
    CHECK_EQ(after.decoderLinesTotal, base.decoderLinesTotal + 60u);  // measured
    CHECK_EQ(after.listSeq, base.listSeq);                            // ...and no list moved
    // 1,000 lines more, in batches (the session's queue holds 256): still not.
    for (int round = 0; round < 10; ++round) {
        const std::vector<FoxCommand> batch(100, testing::cmd(FOXAPI_OP_SETTING_SET, 0, 0, "test.decoderLine=SPAM|x"));
        CHECK_EQ(api()->submit(s.raw(), batch.data(), 100, nullptr), 100);
        passes(eng, 2);
        FoxCommandResult drain[16];
        for (auto& x : drain) { x.structSize = sizeof(x); }
        while (api()->poll_results(s.raw(), drain, 16) > 0) {}
    }
    const FoxReceiverState lots = s.state();
    CHECK_EQ(lots.decoderLinesTotal, base.decoderLinesTotal + 1060u);
    CHECK_EQ(lots.listSeq, base.listSeq);
    // The backlog is there: the ring holds the newest 500, numbered from the same counter.
    CHECK_EQ(count(s, FOXAPI_LIST_DECODER_LINES), 500);
    CHECK_EQ(item(s, FOXAPI_LIST_DECODER_LINES, 499).id, lots.decoderLinesTotal);
    // A status line is a list change, and moves listSeq as it did.
    CHECK_EQ(pumped(eng, s, testing::cmd(FOXAPI_OP_SETTING_SET, 0, 0, "test.statusLine=presetNote|hello")).status(),
             FOXAPI_OK);
    passes(eng, 2);
    const uint64_t afterStatus = s.state().listSeq;
    CHECK(afterStatus != base.listSeq);

    // THE TRACK-INFO PLUGIN'S STATUS LINES go to the decoder output as they always did - and are COUNTED, so
    // they reach the backlog an interface reads (an uncounted line never did).
    host.pending = {"cannot reach the registry"};
    passes(eng, 2);
    const FoxReceiverState info = s.state();
    CHECK_EQ(info.decoderLinesTotal, lots.decoderLinesTotal + 1u);
    const int32_t n = count(s, FOXAPI_LIST_DECODER_LINES);
    CHECK_EQ(n, 500);
    const FoxListItem last = item(s, FOXAPI_LIST_DECODER_LINES, static_cast<uint32_t>(n - 1));
    CHECK_EQ(std::string(last.name), std::string("Aircraft info"));
    CHECK_EQ(std::string(last.detail), std::string("cannot reach the registry"));
    CHECK_EQ(last.id, info.decoderLinesTotal);
    CHECK_EQ(info.listSeq, afterStatus);  // a line of any kind leaves the lists alone
    std::printf("M4: 1,060 decoder lines moved decoderLinesTotal and not listSeq; track-info lines are counted\n");
}

// --- H: the fitted-modules window (window port group 9, API 0.3 section 8.8) -------------------------
//
// Each case drives the engine from THIS thread, as group M does, against the scratch plugin folder.
// What a plugin does through the host bridge (log a line, add or remove a command key, ask for the
// settings grant) the test does on the loaded module's own client, through PluginApiCore - the very
// object the bridge's trampolines call.

// H0 (F7): a plugin's log() lines are decoder output lines - they reach decoderLinesTotal (one a line
// actually pushed: a text with several lines is several, an empty part is none) and DECODER_LINES at the
// matching ids. Before this change applyPluginApi pushed them into the log without counting them, so an
// interface that reads new lines only when the total moves never saw them, and the ring - which takes
// "the last (total - seen) entries" - took the wrong slice once an uncounted line sat among counted ones.
void testPluginLogLinesAreCounted() {
    using Access = cascade::gui::AppWindowTestAccess;
    PluginWorld world;
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    passes(d.eng, 3);
    const std::string stage1 = PluginWorld::fileOf(FIXTURE_STAGE1_PLUGIN);
    cascade::core::PluginApiCore& core = Access::core(d.eng);
    cascade::core::PluginApiClient& client = core.client(stage1, "Stage One Fixture");
    CHECK(client.live.load());

    const auto lastLine = [&](int back = 0) {
        const int32_t n = count(s, FOXAPI_LIST_DECODER_LINES);
        return item(s, FOXAPI_LIST_DECODER_LINES, static_cast<uint32_t>(n - 1 - back));
    };
    const uint64_t base = s.state().decoderLinesTotal;
    const int32_t baseCount = count(s, FOXAPI_LIST_DECODER_LINES);

    // One INFO line: the total climbs by one and the list serves it at that id.
    CHECK_EQ(core.log(client, CASCADE_LOG_INFO, "hello from the plugin"), CASCADE_API_OK);
    passes(d.eng, 2);
    CHECK_EQ(s.state().decoderLinesTotal, base + 1u);
    CHECK_EQ(count(s, FOXAPI_LIST_DECODER_LINES), baseCount + 1);
    CHECK_EQ(lastLine().id, base + 1u);
    CHECK_EQ(std::string(lastLine().name), std::string("Stage One Fixture"));
    CHECK_EQ(std::string(lastLine().detail), std::string("hello from the plugin"));

    // A WARN and an ERROR are lines too (they are also the plate's notice).
    CHECK_EQ(core.log(client, CASCADE_LOG_WARN, "a warning"), CASCADE_API_OK);
    CHECK_EQ(core.log(client, CASCADE_LOG_ERROR, "an error"), CASCADE_API_OK);
    passes(d.eng, 2);
    CHECK_EQ(s.state().decoderLinesTotal, base + 3u);
    CHECK_EQ(lastLine().id, base + 3u);
    CHECK_EQ(std::string(lastLine().detail), std::string("an error"));
    CHECK_EQ(lastLine(1).id, base + 2u);
    CHECK_EQ(std::string(lastLine(1).detail), std::string("a warning"));

    // ONE log() CALL, SEVERAL LINES: split at the newlines, a trailing CR dropped, an empty part dropped.
    CHECK_EQ(core.log(client, CASCADE_LOG_INFO, "one\r\n\ntwo\n\nthree\n"), CASCADE_API_OK);
    passes(d.eng, 2);
    CHECK_EQ(s.state().decoderLinesTotal, base + 6u);  // three lines, not five
    CHECK_EQ(lastLine(2).id, base + 4u);
    CHECK_EQ(std::string(lastLine(2).detail), std::string("one"));
    CHECK_EQ(std::string(lastLine(1).detail), std::string("two"));
    CHECK_EQ(lastLine().id, base + 6u);
    CHECK_EQ(std::string(lastLine().detail), std::string("three"));

    // A text of nothing but separators pushes no line, and counts none.
    CHECK_EQ(core.log(client, CASCADE_LOG_INFO, "\r\n\n"), CASCADE_API_OK);
    passes(d.eng, 2);
    CHECK_EQ(s.state().decoderLinesTotal, base + 6u);

    // Counted and uncounted writers side by side keep the ring's numbering whole: a plugin's line between
    // two test-hook lines (the runner's own go through the same count) is at its own id, not a neighbour's.
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SETTING_SET, 0, 0, "test.decoderLine=POCSAG|before")).status(),
             FOXAPI_OK);
    CHECK_EQ(core.log(client, CASCADE_LOG_INFO, "between"), CASCADE_API_OK);
    passes(d.eng, 2);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SETTING_SET, 0, 0, "test.decoderLine=POCSAG|after")).status(),
             FOXAPI_OK);
    const uint64_t total = s.state().decoderLinesTotal;
    CHECK_EQ(total, base + 9u);
    CHECK_EQ(lastLine().id, total);
    CHECK_EQ(std::string(lastLine().detail), std::string("after"));
    CHECK_EQ(lastLine(1).id, total - 1u);
    CHECK_EQ(std::string(lastLine(1).detail), std::string("between"));
    CHECK_EQ(lastLine(2).id, total - 2u);
    CHECK_EQ(std::string(lastLine(2).detail), std::string("before"));
    std::printf("H0: a plugin's log lines are counted into decoderLinesTotal, one for each line pushed\n");
}

// The PLUGINS entry whose module file name (its detail) is `file`: a zeroed item when there is none.
FoxListItem pluginByFile(testing::Session& s, const std::string& file) {
    const int32_t n = count(s, FOXAPI_LIST_PLUGINS);
    for (int32_t i = 0; i < n; ++i) {
        const FoxListItem it = item(s, FOXAPI_LIST_PLUGINS, static_cast<uint32_t>(i));
        if (file == it.detail) { return it; }
    }
    return FoxListItem{};
}

// `fact.plugin.<file>.<leaf>`, read by `who`; the status (a length, or FOXAPI_NOT_FOUND / FOXAPI_DENIED) in *st.
std::string moduleFact(testing::Session& who, const std::string& file, const char* leaf, int32_t* st = nullptr) {
    return fact(who, ("fact.plugin." + file + "." + leaf).c_str(), st);
}

// The three fixtures' module file names (and the dotted copy of the third, when the world has one).
struct Nine {
    std::string g8 = PluginWorld::fileOf(FIXTURE_GROUP8_PLUGIN);
    std::string g9 = PluginWorld::fileOf(FIXTURE_GROUP9_PLUGIN);
    std::string s1 = PluginWorld::fileOf(FIXTURE_STAGE1_PLUGIN);
    std::string dotted = "group.nine.v2.dll";
    std::string refused = "zz_refused.dll";
};

// H1: the per-file facts - whole texts, the LOCAL ones denied to a REMOTE session, "" for a known file with
// nothing to say, NOT_FOUND for a file or a leaf that is not there - and the plugins folder.
void testFittedModuleFacts() {
    PluginWorld world(true, true);
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    testing::Session remote(api(), d.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW, "s3cret");
    CHECK(remote.ok());
    passes(d.eng, 3);
    const Nine n;
    int32_t st = 0;

    // The folder: LOCAL (it is a path on the engine's machine).
    CHECK_EQ(fact(s, "fact.plugin.dir", &st), world.dir.string());
    CHECK(st > 0);
    CHECK_EQ(fact(remote, "fact.plugin.dir", &st), std::string());
    CHECK_EQ(st, FOXAPI_DENIED);

    // Five records, in the host's order (by file name).
    CHECK_EQ(count(s, FOXAPI_LIST_PLUGINS), 5);

    // A loaded module: the descriptor's own strings, no refusal, nothing idle, no notice, no integrity note,
    // and the path it was loaded from.
    CHECK_EQ(moduleFact(s, n.g9, "version"), std::string("9.1.0"));
    CHECK_EQ(moduleFact(s, n.g9, "author"), std::string("FoxSDR tests (group nine)"));
    CHECK_EQ(moduleFact(s, n.g9, "licence"), std::string("MIT"));
    CHECK_EQ(moduleFact(s, n.g8, "version"), std::string("1.0.0"));
    CHECK_EQ(moduleFact(s, n.g8, "author"), std::string("FoxSDR tests"));
    std::error_code ec;
    CHECK(std::filesystem::equivalent(std::filesystem::path(moduleFact(s, n.g9, "path")), world.dir / n.g9, ec));
    for (const char* leaf : {"error", "idle", "notice", "integrity"}) {
        const std::string v = moduleFact(s, n.g9, leaf, &st);
        CHECK_EQ(v, std::string());  // known, and nothing to say: "" and not NOT_FOUND
        CHECK_EQ(st, 0);
    }

    // A file the host refused before it read a descriptor: no identity, a reason, a path.
    for (const char* leaf : {"version", "author", "licence", "idle", "notice", "integrity"}) {
        CHECK_EQ(moduleFact(s, n.refused, leaf, &st), std::string());
        CHECK_EQ(st, 0);
    }
    const std::string why = moduleFact(s, n.refused, "error", &st);
    CHECK(st > 0 && !why.empty());
    CHECK(std::filesystem::equivalent(std::filesystem::path(moduleFact(s, n.refused, "path")), world.dir / n.refused, ec));
    // ...and the reason is the host's own, as the PLUGINS list's own entry for it says (the entry is not ACTIVE).
    CHECK((pluginByFile(s, n.refused).flags & FOXAPI_ITEM_ACTIVE) == 0u);

    // A module whose file name has dots in it, turned off by the host as the second copy of one plugin: its
    // identity is known and it carries a reason. The key is the file name whole.
    CHECK_EQ(moduleFact(s, n.dotted, "version"), std::string("9.1.0"));
    const std::string dup = moduleFact(s, n.dotted, "error", &st);
    CHECK(st > 0 && !dup.empty());
    CHECK(dup.find(n.g9) != std::string::npos);  // it names the file that was kept instead
    CHECK((pluginByFile(s, n.dotted).flags & FOXAPI_ITEM_ACTIVE) == 0u);
    CHECK(std::filesystem::equivalent(std::filesystem::path(moduleFact(s, n.dotted, "path")), world.dir / n.dotted, ec));
    CHECK_EQ(moduleFact(s, n.g9, "error"), std::string());  // the copy that was kept has none

    // The LOCAL one answers DENIED to a REMOTE session, and the rest of the texts do not.
    CHECK_EQ(moduleFact(remote, n.g9, "path", &st), std::string());
    CHECK_EQ(st, FOXAPI_DENIED);
    CHECK_EQ(moduleFact(remote, n.refused, "path", &st), std::string());
    CHECK_EQ(st, FOXAPI_DENIED);
    CHECK_EQ(moduleFact(remote, n.g9, "version", &st), std::string("9.1.0"));
    CHECK_EQ(moduleFact(remote, n.refused, "error", &st), why);
    CHECK_EQ(moduleFact(remote, n.g9, "idle", &st), std::string());
    CHECK_EQ(st, 0);

    // NOT_FOUND: a file nobody holds, a leaf that is not one, the key with no leaf, the folder's key with a
    // leaf, and a file named for a leaf.
    CHECK_EQ(moduleFact(s, "no-such-module.dll", "version", &st), std::string());
    CHECK_EQ(st, FOXAPI_NOT_FOUND);
    CHECK_EQ(moduleFact(s, n.g9, "colour", &st), std::string());
    CHECK_EQ(st, FOXAPI_NOT_FOUND);
    CHECK_EQ(fact(s, ("fact.plugin." + n.g9).c_str(), &st), std::string());
    CHECK_EQ(st, FOXAPI_NOT_FOUND);
    CHECK_EQ(fact(s, "fact.plugin.dir.path", &st), std::string());
    CHECK_EQ(st, FOXAPI_NOT_FOUND);
    CHECK_EQ(moduleFact(remote, "no-such-module.dll", "path", &st), std::string());
    CHECK_EQ(st, FOXAPI_NOT_FOUND);  // not found is not hidden behind denied

    // A rescan that takes a file away takes its facts: the record is gone, so NOT_FOUND again.
    std::filesystem::remove(world.dir / n.dotted, ec);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_PLUGIN_RESCAN)).status(), FOXAPI_OK);
    passes(d.eng, 3);
    CHECK_EQ(count(s, FOXAPI_LIST_PLUGINS), 4);
    CHECK_EQ(moduleFact(s, n.dotted, "version", &st), std::string());
    CHECK_EQ(st, FOXAPI_NOT_FOUND);
    CHECK_EQ(moduleFact(s, n.g9, "version"), std::string("9.1.0"));
    std::printf("H1: fact.plugin.dir and the eight per-file facts: whole, \"\" known-and-empty, NOT_FOUND, DENIED to a REMOTE session\n");
}

// H2: PLUGINS value[1] (the file's size, measured when the block is built and not every pass) and value[2] (the
// newest WARN/ERROR's level, whole text in fact.plugin.<file>.notice); a plugin that says the same warning again
// moves nothing.
void testFittedSizeAndNotice() {
    using Access = cascade::gui::AppWindowTestAccess;
    PluginWorld world(true);
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    passes(d.eng, 3);
    const Nine n;
    cascade::core::PluginApiCore& core = Access::core(d.eng);
    cascade::core::PluginApiClient& c9 = core.client(n.g9, "Group Nine Fixture");
    CHECK(c9.live.load());

    // SIZE: the file's bytes, for every record (loaded or not).
    for (const std::string& f : {n.g8, n.g9, n.s1, n.refused}) {
        const FoxListItem it = pluginByFile(s, f);
        CHECK_EQ(std::string(it.detail), f);
        CHECK_EQ(it.value[1], static_cast<double>(std::filesystem::file_size(world.dir / f)));
        CHECK(it.value[1] > 0.0);
    }
    CHECK_EQ(pluginByFile(s, n.refused).value[1], 33.0);  // "this is not a portable executable"
    // Measured when the block is built, not every pass: the file grows behind the engine's back and nothing
    // says so until something rebuilds the block (a rescan does).
    const uint64_t seq0 = s.state().listSeq;
    {
        std::ofstream more(world.dir / n.refused, std::ios::binary | std::ios::app);
        more << " and now it is longer";
    }
    passes(d.eng, 10);
    CHECK_EQ(s.state().listSeq, seq0);
    CHECK_EQ(pluginByFile(s, n.refused).value[1], 33.0);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_PLUGIN_RESCAN)).status(), FOXAPI_OK);
    passes(d.eng, 3);
    CHECK_EQ(pluginByFile(s, n.refused).value[1], static_cast<double>(std::filesystem::file_size(world.dir / n.refused)));
    CHECK(pluginByFile(s, n.refused).value[1] > 33.0);
    CHECK(s.state().listSeq != seq0);  // the rescan is in the fingerprint

    // NOTICE: none, then a warning of a length no list item holds, then an error.
    passes(d.eng, 3);
    const uint64_t quiet = s.state().listSeq;
    for (const std::string& f : {n.g8, n.g9, n.s1, n.refused}) {
        CHECK_EQ(pluginByFile(s, f).value[2], 0.0);
        CHECK_EQ(moduleFact(s, f, "notice"), std::string());
    }
    const std::string warn(400, 'w');
    CHECK_EQ(core.log(c9, CASCADE_LOG_WARN, warn.c_str()), CASCADE_API_OK);
    passes(d.eng, 2);
    CHECK_EQ(pluginByFile(s, n.g9).value[2], static_cast<double>(CASCADE_LOG_WARN));
    CHECK_EQ(moduleFact(s, n.g9, "notice"), warn);  // whole: the item's detail would cut at 159
    CHECK_EQ(pluginByFile(s, n.g8).value[2], 0.0);  // another module's plate is unmoved
    const uint64_t warned = s.state().listSeq;
    CHECK(warned != quiet);

    // THE SAME WARNING AGAIN, every pass, is no change: nothing moves listSeq, so a window that re-reads
    // when it moves does not re-read for a plugin that repeats itself. (Group 8's D3 would have come back.)
    for (int i = 0; i < 50; ++i) {
        CHECK_EQ(core.log(c9, CASCADE_LOG_WARN, warn.c_str()), CASCADE_API_OK);
        d.eng.controlPass();
    }
    CHECK_EQ(s.state().listSeq, warned);
    CHECK_EQ(pluginByFile(s, n.g9).value[2], static_cast<double>(CASCADE_LOG_WARN));
    // An INFO line is a decoder line and not a notice: it changes neither the level nor the text.
    const uint64_t linesBefore = s.state().decoderLinesTotal;
    CHECK_EQ(core.log(c9, CASCADE_LOG_INFO, "just so you know"), CASCADE_API_OK);
    passes(d.eng, 2);
    CHECK_EQ(s.state().decoderLinesTotal, linesBefore + 1u);
    CHECK_EQ(s.state().listSeq, warned);
    CHECK_EQ(moduleFact(s, n.g9, "notice"), warn);
    // Other text at the same level, and then another level with the same text, each move it.
    CHECK_EQ(core.log(c9, CASCADE_LOG_WARN, "a different warning"), CASCADE_API_OK);
    passes(d.eng, 2);
    const uint64_t other = s.state().listSeq;
    CHECK(other != warned);
    CHECK_EQ(moduleFact(s, n.g9, "notice"), std::string("a different warning"));
    CHECK_EQ(core.log(c9, CASCADE_LOG_ERROR, "a different warning"), CASCADE_API_OK);
    passes(d.eng, 2);
    CHECK(s.state().listSeq != other);
    CHECK_EQ(pluginByFile(s, n.g9).value[2], static_cast<double>(CASCADE_LOG_ERROR));
    CHECK_EQ(moduleFact(s, n.g9, "notice"), std::string("a different warning"));
    // The longest text a plugin can send (512 bytes) is served whole.
    const std::string longest(512, 'e');
    CHECK_EQ(core.log(c9, CASCADE_LOG_ERROR, longest.c_str()), CASCADE_API_OK);
    passes(d.eng, 2);
    CHECK_EQ(moduleFact(s, n.g9, "notice").size(), static_cast<std::size_t>(CASCADE_LOG_TEXT_BYTES) - 1u);
    std::printf("H2: PLUGINS value[1] is the file's size (measured at a rebuild), value[2] the notice level; a repeated warning moves nothing\n");
}

// H3: the three flags. MATCHED (the runner feeds it or would, any kind of plugin, the receiver stopped),
// ASKED_SETTINGS (set when a plugin first asks, and then sticky) and DIGEST_MISMATCH (the install record says the
// file is not the one installed) - and the idle sentence the runner gives a module that is not fed.
void testFittedFlags() {
    using Access = cascade::gui::AppWindowTestAccess;
    PluginWorld world(true);
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    passes(d.eng, 3);
    const Nine n;
    const auto has = [&](const std::string& f, uint32_t bit) { return (pluginByFile(s, f).flags & bit) != 0u; };

    // MATCHED, with the receiver STOPPED: the decoders and the audio processor (which has no DECODERS entry),
    // and not the refused file. DECODERS' FED is false for all of them (it is MATCHED and the receiver running).
    CHECK((s.state().flags & FOXAPI_RX_RUNNING) == 0u);
    CHECK(has(n.g8, FOXAPI_ITEM_MATCHED) && has(n.g9, FOXAPI_ITEM_MATCHED) && has(n.s1, FOXAPI_ITEM_MATCHED));
    CHECK(!has(n.refused, FOXAPI_ITEM_MATCHED));
    CHECK_EQ(count(s, FOXAPI_LIST_DECODERS), 2);  // the audio processor is not a decoder
    CHECK_EQ(entryNamed(s, FOXAPI_LIST_DECODERS, "Group Eight Fixture").flags & FOXAPI_ITEM_FED, 0u);
    CHECK_EQ(entryNamed(s, FOXAPI_LIST_DECODERS, "Stage One Fixture").flags & FOXAPI_ITEM_FED, 0u);
    CHECK_EQ(s.state().decodersMatched, 2u);
    // None of the three flags is on a record that has not earned it.
    for (const std::string& f : {n.g8, n.g9, n.s1, n.refused}) {
        CHECK(!has(f, FOXAPI_ITEM_ASKED_SETTINGS));
        CHECK(!has(f, FOXAPI_ITEM_DIGEST_MISMATCH));
    }

    // Stopped, a plugin is fed nothing: not matched. Started again, it is.
    uint64_t seq = s.state().listSeq;
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_DECODER_STOP, 0, 0, n.g9.c_str())).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK(has(n.g9, FOXAPI_ITEM_STOPPED) && !has(n.g9, FOXAPI_ITEM_MATCHED));
    CHECK(has(n.g8, FOXAPI_ITEM_MATCHED));
    CHECK(s.state().listSeq != seq);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_DECODER_START, 0, 0, n.g9.c_str())).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK(!has(n.g9, FOXAPI_ITEM_STOPPED) && has(n.g9, FOXAPI_ITEM_MATCHED));

    // IDLE: a processor that cannot start is not matched, and the runner's own sentence is the fact, whole.
    engine_api_test::setEnv("FOXSDR_FIXTURE_G9_NO_CREATE", "1");
    seq = s.state().listSeq;
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_PLUGIN_RESCAN)).status(), FOXAPI_OK);
    passes(d.eng, 3);
    CHECK(!has(n.g9, FOXAPI_ITEM_MATCHED));
    CHECK(has(n.g9, FOXAPI_ITEM_ACTIVE));  // loaded all the same
    const std::string idle = moduleFact(s, n.g9, "idle");
    CHECK(idle.find("failed to start its audio processor") != std::string::npos);
    CHECK_EQ(idle, std::string("\"Group Nine Fixture\" failed to start its audio processor (its create() returned "
                               "nothing), so the audio is not being processed."));  // the runner's own words
    CHECK_EQ(moduleFact(s, n.g8, "idle"), std::string());
    CHECK(has(n.g8, FOXAPI_ITEM_MATCHED));
    CHECK(s.state().listSeq != seq);
    engine_api_test::setEnv("FOXSDR_FIXTURE_G9_NO_CREATE", "");
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_PLUGIN_RESCAN)).status(), FOXAPI_OK);
    passes(d.eng, 3);
    CHECK(has(n.g9, FOXAPI_ITEM_MATCHED));
    CHECK_EQ(moduleFact(s, n.g9, "idle"), std::string());

    // ASKED_SETTINGS: a plugin's first request for something only the settings grant allows (here a mode
    // change, answered DENIED) sets it - on that module only - and a repeat moves nothing.
    cascade::core::PluginApiCore& core = Access::core(d.eng);
    cascade::core::PluginApiClient& c9 = core.client(n.g9, "Group Nine Fixture");
    cascade::core::PluginControl ask;
    ask.kind = cascade::core::PluginControl::Kind::Mode;
    ask.mode = CASCADE_DEMOD_NFM;
    seq = s.state().listSeq;
    CHECK_EQ(core.requestControl(c9, ask), CASCADE_API_DENIED);
    passes(d.eng, 2);
    CHECK(has(n.g9, FOXAPI_ITEM_ASKED_SETTINGS));
    CHECK(!has(n.g8, FOXAPI_ITEM_ASKED_SETTINGS) && !has(n.s1, FOXAPI_ITEM_ASKED_SETTINGS));
    CHECK(!has(n.g9, FOXAPI_ITEM_SETTINGS_ALLOWED));  // asking is not holding the grant
    CHECK(s.state().listSeq != seq);
    seq = s.state().listSeq;
    for (int i = 0; i < 5; ++i) {
        CHECK_EQ(core.requestControl(c9, ask), CASCADE_API_DENIED);
        d.eng.controlPass();
    }
    CHECK_EQ(s.state().listSeq, seq);
    // A request that needs the TUNE grant is not a request for a setting.
    cascade::core::PluginControl tune;
    tune.kind = cascade::core::PluginControl::Kind::Frequency;
    tune.value = 100.0e6;
    cascade::core::PluginApiClient& c8 = core.client(n.g8, "Group Eight Fixture");
    CHECK_EQ(core.requestControl(c8, tune), CASCADE_API_DENIED);
    passes(d.eng, 2);
    CHECK(!has(n.g8, FOXAPI_ITEM_ASKED_SETTINGS));
    // It stays: through a stop and a start, a grant and its revocation, and a rescan (the flag is "this module
    // has asked at least once" for as long as the engine lives).
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_DECODER_STOP, 0, 0, n.g9.c_str())).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK(has(n.g9, FOXAPI_ITEM_ASKED_SETTINGS));
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_DECODER_START, 0, 0, n.g9.c_str())).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, grantCmd(n.g9, 2, 1)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK(has(n.g9, FOXAPI_ITEM_ASKED_SETTINGS) && has(n.g9, FOXAPI_ITEM_SETTINGS_ALLOWED));
    CHECK_EQ(pumped(d.eng, s, grantCmd(n.g9, 2, 0)).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_PLUGIN_RESCAN)).status(), FOXAPI_OK);
    passes(d.eng, 3);
    CHECK(has(n.g9, FOXAPI_ITEM_ASKED_SETTINGS) && !has(n.g9, FOXAPI_ITEM_SETTINGS_ALLOWED));

    // DIGEST_MISMATCH: the engine's inventory (the install records, re-hashed at every rescan) says the file on
    // disk is not the one that was installed. Doctored here, as no manifest is in a scratch folder.
    cascade::core::InstalledPlugin rec;
    rec.id = "group-nine-fixture";
    rec.name = "Group Nine Fixture";
    rec.file = n.g9;
    rec.digestMismatch = true;
    Access::inventory(d.eng).plugins.push_back(rec);
    seq = s.state().listSeq;
    passes(d.eng, 2);
    CHECK(has(n.g9, FOXAPI_ITEM_DIGEST_MISMATCH));
    CHECK(!has(n.g8, FOXAPI_ITEM_DIGEST_MISMATCH) && !has(n.s1, FOXAPI_ITEM_DIGEST_MISMATCH));
    CHECK(s.state().listSeq != seq);
    const std::string note = moduleFact(s, n.g9, "integrity");
    CHECK_EQ(note, cascade::core::PluginRepo::changedSinceInstallNote(Access::inventory(d.eng).plugins, n.g9));
    CHECK(note.find("is not the file that was installed") != std::string::npos);
    CHECK(note.find(n.g9) != std::string::npos && note.find("group-nine-fixture") != std::string::npos);
    CHECK_EQ(moduleFact(s, n.g8, "integrity"), std::string());
    // The record names the file in any case (the host's own comparison ignores it), and a record that is fine says nothing.
    std::string upper = n.g9;
    for (char& ch : upper) { ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch))); }
    Access::inventory(d.eng).plugins.back().file = upper;
    passes(d.eng, 2);
    CHECK(has(n.g9, FOXAPI_ITEM_DIGEST_MISMATCH));
    Access::inventory(d.eng).plugins.back().digestMismatch = false;
    seq = s.state().listSeq;
    passes(d.eng, 2);
    CHECK(!has(n.g9, FOXAPI_ITEM_DIGEST_MISMATCH));
    CHECK_EQ(moduleFact(s, n.g9, "integrity"), std::string());
    CHECK(s.state().listSeq != seq);
    // A rescan re-reads the install records from the folder (none, here) and the doctored finding goes with them.
    Access::inventory(d.eng).plugins.back().digestMismatch = true;
    passes(d.eng, 2);
    CHECK(has(n.g9, FOXAPI_ITEM_DIGEST_MISMATCH));
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_PLUGIN_RESCAN)).status(), FOXAPI_OK);
    passes(d.eng, 3);
    CHECK(!has(n.g9, FOXAPI_ITEM_DIGEST_MISMATCH));
    std::printf("H3: MATCHED with the receiver stopped (an audio processor too), ASKED_SETTINGS sticky, DIGEST_MISMATCH from the install records, idle sentences whole\n");
}

// H4: PLUGIN_COMMANDS - one entry for each command key of each loaded module, in host order and then in the order
// the module added them - and what moves listSeq.
void testFittedCommands() {
    using Access = cascade::gui::AppWindowTestAccess;
    PluginWorld world(true);
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    testing::Session remote(api(), d.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW, "s3cret");
    passes(d.eng, 3);
    const Nine n;
    const auto entry = [](testing::Session& who, int32_t i) { return item(who, FOXAPI_LIST_PLUGIN_COMMANDS, static_cast<uint32_t>(i)); };

    // The two fixtures that add keys: the group-9 module's two (host order puts it before the stage-1 module's one).
    CHECK_EQ(count(s, FOXAPI_LIST_PLUGIN_COMMANDS), 3);
    CHECK_EQ(entry(s, 0).id, 1u);
    CHECK_EQ(std::string(entry(s, 0).name), std::string("RESET"));
    CHECK_EQ(std::string(entry(s, 0).detail), n.g9);
    CHECK_EQ(entry(s, 1).id, 2u);
    CHECK_EQ(std::string(entry(s, 1).name), std::string("CALIBRATE"));
    CHECK_EQ(std::string(entry(s, 1).detail), n.g9);
    CHECK_EQ(entry(s, 2).id, 7u);
    CHECK_EQ(std::string(entry(s, 2).name), std::string("PING"));
    CHECK_EQ(std::string(entry(s, 2).detail), n.s1);
    CHECK_EQ(count(remote, FOXAPI_LIST_PLUGIN_COMMANDS), 3);  // a key is not a path on this machine: not LOCAL-only
    CHECK_EQ(std::string(entry(remote, 0).name), std::string("RESET"));

    // A key a module lists is one FOXAPI_OP_PLUGIN_COMMAND takes (file name as its text, the id in ival[0]).
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_PLUGIN_COMMAND, 0, 2, n.g9.c_str())).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_PLUGIN_COMMAND, 0, 99, n.g9.c_str())).status(), FOXAPI_NOT_FOUND);

    // A module adds a key, relabels it, says it again, removes it: each real change moves listSeq, the same
    // thing said again does not.
    cascade::core::PluginApiCore& core = Access::core(d.eng);
    cascade::core::PluginApiClient& c9 = core.client(n.g9, "Group Nine Fixture");
    uint64_t seq = s.state().listSeq;
    CHECK_EQ(core.addCommand(c9, 3u, "EXTRA"), CASCADE_API_OK);
    passes(d.eng, 2);
    CHECK_EQ(count(s, FOXAPI_LIST_PLUGIN_COMMANDS), 4);
    CHECK_EQ(entry(s, 2).id, 3u);  // after the module's own, before the next module's
    CHECK_EQ(std::string(entry(s, 2).name), std::string("EXTRA"));
    CHECK_EQ(std::string(entry(s, 3).detail), n.s1);
    CHECK(s.state().listSeq != seq);
    seq = s.state().listSeq;
    CHECK_EQ(core.addCommand(c9, 3u, "EXTRA"), CASCADE_API_OK);  // said again
    passes(d.eng, 2);
    CHECK_EQ(s.state().listSeq, seq);
    CHECK_EQ(core.addCommand(c9, 3u, "RELABELLED"), CASCADE_API_OK);
    passes(d.eng, 2);
    CHECK_EQ(std::string(entry(s, 2).name), std::string("RELABELLED"));
    CHECK_EQ(count(s, FOXAPI_LIST_PLUGIN_COMMANDS), 4);
    CHECK(s.state().listSeq != seq);
    seq = s.state().listSeq;
    for (int i = 0; i < 20; ++i) { d.eng.controlPass(); }  // settled: nothing moves
    CHECK_EQ(s.state().listSeq, seq);
    CHECK_EQ(core.removeCommand(c9, 3u), CASCADE_API_OK);
    passes(d.eng, 2);
    CHECK_EQ(count(s, FOXAPI_LIST_PLUGIN_COMMANDS), 3);
    CHECK(s.state().listSeq != seq);
    seq = s.state().listSeq;
    CHECK_EQ(core.removeCommand(c9, 3u), CASCADE_API_NOT_FOUND);  // nothing to remove: no change
    passes(d.eng, 2);
    CHECK_EQ(s.state().listSeq, seq);

    // A stopped module's keys leave the screen at once; started again, the module adds them again.
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_DECODER_STOP, 0, 0, n.g9.c_str())).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK_EQ(count(s, FOXAPI_LIST_PLUGIN_COMMANDS), 1);
    CHECK_EQ(std::string(entry(s, 0).detail), n.s1);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_DECODER_START, 0, 0, n.g9.c_str())).status(), FOXAPI_OK);
    passes(d.eng, 3);
    CHECK_EQ(count(s, FOXAPI_LIST_PLUGIN_COMMANDS), 3);
    CHECK_EQ(std::string(entry(s, 0).name), std::string("RESET"));
    // ...and so does a rescan that unloads them and brings them back.
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_PLUGIN_RESCAN)).status(), FOXAPI_OK);
    passes(d.eng, 3);
    CHECK_EQ(count(s, FOXAPI_LIST_PLUGIN_COMMANDS), 3);
    // A removed module (its file deleted, then a rescan) has no keys: the list is by module.
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_STORE_REMOVE, 0, 0, n.g9.c_str())).status(), FOXAPI_OK);
    passes(d.eng, 3);
    CHECK_EQ(count(s, FOXAPI_LIST_PLUGIN_COMMANDS), 1);
    std::printf("H4: PLUGIN_COMMANDS lists each loaded module's keys in order; add, relabel and remove move listSeq, a repeat does not\n");
}

// H5: the install outcome as two status lines - the hook, the engine's own writers (a failed and a successful
// remove) and the window's copy of the lines.
void testInstallStatusLines() {
    PluginWorld world(true);
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    passes(d.eng, 3);
    const Nine n;
    int32_t st = 0;
    const auto levelOf = [&](const char* key) {
        for (int32_t i = 0; i < count(s, FOXAPI_LIST_STATUS_LINES); ++i) {
            const FoxListItem x = item(s, FOXAPI_LIST_STATUS_LINES, static_cast<uint32_t>(i));
            if (std::string(x.name) == key) { return x.value[0]; }
        }
        return -1.0;
    };
    CHECK_EQ(levelOf("installReport"), -1.0);
    CHECK_EQ(levelOf("installError"), -1.0);
    CHECK_EQ(fact(s, "fact.status.installReport", &st), std::string());  // served, and empty
    CHECK_EQ(st, 0);
    CHECK_EQ(fact(s, "fact.status.installError", &st), std::string());
    CHECK_EQ(st, 0);

    // The hook stands in for whatever writes them: the report is information, the error red, both whole.
    const std::string report = "Installed " + std::string(190, 'r') + ".dll";
    const uint64_t seq = s.state().listSeq;
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SETTING_SET, 0, 0, ("test.statusLine=installReport|" + report).c_str())).status(), FOXAPI_OK);
    CHECK_EQ(levelOf("installReport"), 1.0);
    CHECK_EQ(fact(s, "fact.status.installReport"), report);
    CHECK(s.state().listSeq != seq);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SETTING_SET, 0, 0, "test.statusLine=installError|the sha256 did not match")).status(), FOXAPI_OK);
    CHECK_EQ(levelOf("installError"), 3.0);
    CHECK_EQ(fact(s, "fact.status.installError"), std::string("the sha256 did not match"));
    const cascade::engine::Engine::StatusText text = d.eng.statusText();
    CHECK_EQ(text.installReport, report);
    CHECK_EQ(text.installError, std::string("the sha256 did not match"));
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SETTING_SET, 0, 0, "test.statusLine=installReport|")).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SETTING_SET, 0, 0, "test.statusLine=installError|")).status(), FOXAPI_OK);
    CHECK_EQ(levelOf("installReport"), -1.0);
    CHECK_EQ(levelOf("installError"), -1.0);
    CHECK_EQ(d.eng.statusText().installError, std::string());

    // The engine's own writer, a remove that fails: red, verbatim, and no green line beside it.
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_STORE_REMOVE, 0, 0, "no-such-plugin.dll")).status(), FOXAPI_OK);
    passes(d.eng, 2);
    const std::string err = fact(s, "fact.status.installError");
    CHECK(err.find("is not an installed plugin file") != std::string::npos);
    CHECK(err.find("no-such-plugin.dll") != std::string::npos);
    CHECK_EQ(levelOf("installError"), 3.0);
    CHECK_EQ(levelOf("installReport"), -1.0);
    CHECK_EQ(d.eng.statusText().installError, err);
    // ...and one that works: green, and the red of the attempt before it is gone (a remove clears both first).
    CHECK_EQ(count(s, FOXAPI_LIST_PLUGINS), 4);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_STORE_REMOVE, 0, 0, n.refused.c_str())).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK_EQ(fact(s, "fact.status.installReport"), "Removed " + n.refused);
    CHECK_EQ(levelOf("installReport"), 1.0);
    CHECK_EQ(levelOf("installError"), -1.0);
    CHECK_EQ(fact(s, "fact.status.installError"), std::string());
    CHECK_EQ(count(s, FOXAPI_LIST_PLUGINS), 3);  // the record went with the file
    CHECK_EQ(d.eng.statusText().installReport, "Removed " + n.refused);
    std::printf("H5: installReport (level 1) and installError (level 3) are status lines, facts and part of the window's copy\n");
}


// --- I: the plugin store (window port group 10, API 0.3 section 8.9) ------------------------------------------
//
// The catalogue is a REAL fetch of a local index.json (an address with no "://" is read from disk, which is how the
// store is exercised offline), over the scratch plugin folder of group 9: its stage-1 module is installed under an
// old version and the index sets a floor above it (a retired module, with an update plan), its group-9 module is
// installed and updatable (a host record to join by file name), and five more entries cover the columns - a build
// with a size and a second platform, a legal notice, no build for this machine, another ABI, no licence. A transfer
// is a promise the test keeps in the engine's own install slot (nothing here touches the network); ADD ALL runs for
// real over entries whose file names the loader's rule refuses, which the install refuses before any network.

struct IdxPlatform {
    std::string os, arch, file, sha;
    unsigned long long size = 0;
};
struct IdxEntry {
    std::string id, name, version, author, licence, summary, description, homepage, legal, floor;
    unsigned abi = CASCADE_PLUGIN_ABI_VERSION;
    std::vector<IdxPlatform> platforms;
};

std::string hex64(char c) { return std::string(64, c); }

// The index document. The texts are plain ASCII with no quotes, so no escaping is needed.
std::string indexJson(const std::vector<IdxEntry>& v) {
    std::string j = "{\"schemaVersion\":1,\"plugins\":[";
    for (std::size_t i = 0; i < v.size(); ++i) {
        const IdxEntry& e = v[i];
        j += i == 0 ? "{" : ",{";
        j += "\"id\":\"" + e.id + "\",\"name\":\"" + e.name + "\",\"version\":\"" + e.version + "\"";
        j += ",\"author\":\"" + e.author + "\",\"licence\":\"" + e.licence + "\",\"summary\":\"" + e.summary + "\"";
        j += ",\"description\":\"" + e.description + "\",\"homepage\":\"" + e.homepage + "\"";
        j += ",\"legalNotice\":\"" + e.legal + "\",\"minSupportedVersion\":\"" + e.floor + "\"";
        j += ",\"abiVersion\":" + std::to_string(e.abi) + ",\"platforms\":[";
        for (std::size_t k = 0; k < e.platforms.size(); ++k) {
            const IdxPlatform& p = e.platforms[k];
            j += k == 0 ? "{" : ",{";
            j += "\"os\":\"" + p.os + "\",\"arch\":\"" + p.arch + "\",\"file\":\"" + p.file + "\"";
            j += ",\"url\":\"https://example.invalid/" + p.file + "\",\"sha256\":\"" + p.sha + "\"";
            if (p.size != 0) { j += ",\"sizeBytes\":" + std::to_string(p.size); }
            j += "}";
        }
        j += "]}";
    }
    return j + "]}";
}

IdxPlatform hostBuild(const std::string& file, char shaChar, unsigned long long size) {
    return IdxPlatform{cascade::core::PluginRepo::hostOs(), cascade::core::PluginRepo::hostArch(), file, hex64(shaChar), size};
}

void writeText(const std::filesystem::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary);
    f << text;
}

std::string lowerAscii(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

// The catalogue entry (CATALOGUE) whose id, in detail, is `id`: a zeroed item when there is none.
FoxListItem catalogueEntry(testing::Session& s, const std::string& id) {
    const int32_t n = count(s, FOXAPI_LIST_CATALOGUE);
    for (int32_t i = 0; i < n; ++i) {
        const FoxListItem it = item(s, FOXAPI_LIST_CATALOGUE, static_cast<uint32_t>(i));
        if (id == it.detail) { return it; }
    }
    return FoxListItem{};
}

std::string storeFact(testing::Session& who, const std::string& id, const char* leaf, int32_t* st = nullptr) {
    return fact(who, ("fact.store." + id + "." + leaf).c_str(), st);
}
std::string blockedFact(testing::Session& who, const std::string& file, const char* leaf, int32_t* st = nullptr) {
    return fact(who, ("fact.blocked." + file + "." + leaf).c_str(), st);
}

// The first language that is not English (Z7's rule).
std::string otherLanguage() {
    for (const cascade::i18n::Language& l : cascade::i18n::languages()) {
        if (l.code != "en") { return l.code; }
    }
    return std::string();
}

// The scratch world of group I: the group-9 plugin folder, a manifest that records two of its modules, and the
// index documents. Built BEFORE the engine, which reads the manifest when it first scans the folder.
struct StoreWorld {
    PluginWorld world{true};
    Nine n;
    std::filesystem::path index, indexRun;
    std::string stageId = "stage-one-fixture", nineId = "group-nine-fixture";
    std::string stageName =
        "Stage One Fixture Plugin With A Deliberately Long Display Name So The Retirement Sentence Passes The Cut";
    std::string hostPair = std::string(cascade::core::PluginRepo::hostOs()) + "/" + cascade::core::PluginRepo::hostArch();

    std::string digest(const std::string& file) const {
        std::string hex, err;
        CHECK(cascade::core::PluginRepo::sha256File((world.dir / file).string(), hex, err));
        return hex;
    }
    StoreWorld() {
        // The install records: the stage-1 module at 1.0.0 and the group-9 one at 9.0.0, each with the digest of
        // the file that is really there (an honest record: no integrity finding to confuse the facts).
        for (int which = 0; which < 2; ++which) {
            cascade::core::PluginCatalogEntry rec;
            rec.id = which == 0 ? stageId : nineId;
            rec.name = which == 0 ? stageName : "Group Nine Fixture";
            rec.version = which == 0 ? "1.0.0" : "9.0.0";
            rec.abiVersion = CASCADE_PLUGIN_ABI_VERSION;
            cascade::core::PluginPlatform p;
            p.os = cascade::core::PluginRepo::hostOs();
            p.arch = cascade::core::PluginRepo::hostArch();
            p.file = which == 0 ? n.s1 : n.g9;
            p.url = "https://example.invalid/x";
            p.sha256 = digest(p.file);
            rec.platforms.push_back(p);
            std::string err;
            CHECK(cascade::core::PluginRepo::recordInstall(world.dir.string(), rec, err));
        }
        std::vector<IdxEntry> v;
        {
            IdxEntry e;  // a plain entry: two builds, the one for this machine with a size
            e.id = "alpha-good";
            e.name = "Alpha Decoder";
            e.version = "1.0.0";
            e.author = "Alpha Labs Ltd";
            e.licence = "MIT";
            e.summary = "Decodes alpha.";
            e.description = std::string(400, 'd');
            e.homepage = "https://example.org/alpha";
            e.platforms = {hostBuild("alpha_good.dll", 'a', 12345), IdxPlatform{"plan9", "mips", "alpha_plan9.so", hex64('b'), 99}};
            v.push_back(e);
        }
        {
            IdxEntry e;  // a legal notice longer than a list item, a floor, a dotted id, no stated size, no author
            e.id = "beta.legal";
            e.name = "Beta Decoder";
            e.version = "0.9.1";
            e.licence = "GPL-3.0-or-later";
            e.legal = std::string(300, 'L');
            e.floor = "0.9.0";
            e.platforms = {hostBuild("beta_legal.dll", 'c', 0)};
            v.push_back(e);
        }
        {
            IdxEntry e;  // announced before its first build: no platforms at all
            e.id = "gamma-nobuild";
            e.name = "Gamma Decoder";
            e.version = "0.1.0";
            e.licence = "MIT";
            v.push_back(e);
        }
        {
            IdxEntry e;  // another ABI
            e.id = "delta-abi";
            e.name = "Delta Decoder";
            e.version = "3.0.0";
            e.licence = "MIT";
            e.abi = CASCADE_PLUGIN_ABI_VERSION + 1u;
            e.platforms = {hostBuild("delta_abi.dll", 'd', 777)};
            v.push_back(e);
        }
        {
            IdxEntry e;  // no licence declared
            e.id = "epsilon-nolicence";
            e.name = "Epsilon Decoder";
            e.version = "1.1.0";
            e.platforms = {hostBuild("epsilon.dll", 'e', 4096)};
            v.push_back(e);
        }
        {
            IdxEntry e;  // installed at 1.0.0 below its own floor: RETIRED, and an update is planned
            e.id = stageId;
            e.name = stageName;
            e.version = "2.0.0";
            e.author = "FoxSDR tests";
            e.licence = "MIT";
            e.floor = "2.0.0";
            e.platforms = {hostBuild(n.s1, 'f', 0)};
            v.push_back(e);
        }
        {
            IdxEntry e;  // installed at 9.0.0, loaded: a host record to join, and an update is planned
            e.id = nineId;
            e.name = "Group Nine Fixture";
            e.version = "9.2.0";
            e.author = "FoxSDR tests (group nine)";
            e.licence = "MIT";
            e.platforms = {hostBuild(n.g9, '7', 2048)};
            v.push_back(e);
        }
        index = world.dir.parent_path() / "store_index_group10.json";
        writeText(index, indexJson(v));
        // The ADD ALL run: three entries whose file names the loader's rule refuses (a separator), so every install
        // is refused by install() before it reaches the network, in a handful of passes.
        std::vector<IdxEntry> run;
        for (const char* word : {"One", "Two", "Three"}) {
            IdxEntry e;
            e.id = std::string("bad-") + word;
            e.name = std::string("Bad ") + word;
            e.version = "1.0.0";
            e.licence = "MIT";
            e.platforms = {hostBuild(std::string("sub/") + word + ".dll", '8', 100)};
            run.push_back(e);
        }
        indexRun = world.dir.parent_path() / "store_index_group10_run.json";
        writeText(indexRun, indexJson(run));
    }
    ~StoreWorld() {
        std::error_code ec;
        std::filesystem::remove(index, ec);
        std::filesystem::remove(indexRun, ec);
    }
};

FoxCommand textCmd(uint32_t op, const std::string& text, long long i0 = 0) { return testing::cmd(op, 0, i0, text.c_str()); }

// Points the store at `path`, fetches, and passes until `entries` entries are listed (the rescan the landing runs is
// published in the same block). Returns whether they were.
bool fetchCatalogue(cascade::engine::Engine& eng, testing::Session& s, const std::filesystem::path& path, int entries) {
    if (pumped(eng, s, textCmd(FOXAPP_OP_SET_CATALOGUE_URL, path.string())).status() != FOXAPI_OK) { return false; }
    if (pumped(eng, s, testing::cmd(FOXAPI_OP_STORE_FETCH)).status() != FOXAPI_OK) { return false; }
    return passUntil(eng, [&] { return count(s, FOXAPI_LIST_CATALOGUE) == entries && count(s, FOXAPI_LIST_STORE_ACTIVITY) == 0; });
}

// I1: before anything was fetched, and the fetch itself - what the table says when nobody has asked, while the
// fetch is in flight, and when it has landed with a retired module and an update plan in it.
void testStoreBeforeAndFetch() {
    StoreWorld w;
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    testing::Session remote(api(), d.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW, "s3cret");
    CHECK(remote.ok());
    passes(d.eng, 3);
    int32_t st = 0;
    const auto levelOf = [&](testing::Session& who, const char* key) {
        for (int32_t i = 0; i < count(who, FOXAPI_LIST_STATUS_LINES); ++i) {
            const FoxListItem x = item(who, FOXAPI_LIST_STATUS_LINES, static_cast<uint32_t>(i));
            if (std::string(x.name) == key) { return x.value[0]; }
        }
        return -1.0;
    };

    // NOBODY HAS ASKED: no entries, nothing going on, no status line, and every fact absent or empty.
    CHECK_EQ(count(s, FOXAPI_LIST_CATALOGUE), 0);
    CHECK_EQ(count(s, FOXAPI_LIST_STORE_ACTIVITY), 0);
    CHECK_EQ(count(s, FOXAPI_LIST_STORE_BLOCKED), 0);
    CHECK_EQ(levelOf(s, "catalogStatus"), -1.0);
    CHECK_EQ(levelOf(s, "addAllSummary"), -1.0);
    CHECK_EQ(fact(s, "fact.status.catalogStatus", &st), std::string());  // served, and empty
    CHECK_EQ(st, 0);
    CHECK_EQ(fact(s, "fact.status.addAllSummary", &st), std::string());
    CHECK_EQ(st, 0);
    storeFact(s, w.stageId, "author", &st);
    CHECK_EQ(st, FOXAPI_NOT_FOUND);  // an id nobody listed
    blockedFact(s, w.n.s1, "id", &st);
    CHECK_EQ(st, FOXAPI_NOT_FOUND);  // a file nobody retired
    const std::string url0 = fact(s, "fact.store.url", &st);
    CHECK(st >= 0);  // known, whatever it holds
    CHECK_EQ(fact(remote, "fact.store.url", &st), std::string());
    CHECK_EQ(st, FOXAPI_DENIED);  // a path or an address on the engine's machine: LOCAL

    // THE INDEX THE STORE READS: a command lands at the next pass and moves listSeq, and the same index again
    // moves nothing.
    const uint64_t seqA = s.state().listSeq;
    CHECK_EQ(pumped(d.eng, s, textCmd(FOXAPP_OP_SET_CATALOGUE_URL, w.index.string())).status(), FOXAPI_OK);
    CHECK_EQ(fact(s, "fact.store.url"), w.index.string());
    CHECK(s.state().listSeq != seqA);
    CHECK(w.index.string() != url0);
    const uint64_t seqB = s.state().listSeq;
    CHECK_EQ(pumped(d.eng, s, textCmd(FOXAPP_OP_SET_CATALOGUE_URL, w.index.string())).status(), FOXAPI_OK);
    passes(d.eng, 3);
    CHECK_EQ(s.state().listSeq, seqB);
    CHECK_EQ(fact(remote, "fact.store.url", &st), std::string());
    CHECK_EQ(st, FOXAPI_DENIED);

    // THE FETCH IN FLIGHT: the pass that applies it publishes entry 1 (kind 1: the catalogue, no name) and an empty
    // catalogue, and the progress is whatever the repo holds (0 at rest).
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_STORE_FETCH)).status(), FOXAPI_OK);
    CHECK_EQ(count(s, FOXAPI_LIST_STORE_ACTIVITY), 1);
    {
        const FoxListItem a = item(s, FOXAPI_LIST_STORE_ACTIVITY, 0);
        CHECK_EQ(a.id, 1u);
        CHECK_EQ(std::string(a.name), std::string());
        CHECK_EQ(a.value[1], 1.0);
        CHECK_EQ(a.value[0], 0.0);
        CHECK((a.flags & FOXAPI_ITEM_ACTIVE) != 0u);
    }
    CHECK_EQ(count(s, FOXAPI_LIST_CATALOGUE), 0);
    CHECK_EQ(count(remote, FOXAPI_LIST_STORE_ACTIVITY), 1);  // not LOCAL-only: a lamp is not a path

    // ...and its landing: the rows, the status line, the retired module and the plans in ONE block, entry 1 gone.
    const bool landed = passUntil(d.eng, [&] { return count(s, FOXAPI_LIST_CATALOGUE) == 7; });
    if (!landed) { std::printf("  I1: the fetch did not land: catalogError \"%s\"\n", fact(s, "fact.status.catalogError").c_str()); }
    CHECK(landed);
    CHECK_EQ(count(s, FOXAPI_LIST_STORE_ACTIVITY), 0);
    CHECK_EQ(levelOf(s, "catalogStatus"), 1.0);
    CHECK_EQ(fact(s, "fact.status.catalogStatus"), std::string("7 plugins in the catalogue"));
    CHECK_EQ(d.eng.statusText().catalogStatus, std::string("7 plugins in the catalogue"));
    CHECK_EQ(count(s, FOXAPI_LIST_STORE_BLOCKED), 1);
    CHECK_EQ(count(s, FOXAPI_LIST_STORE_UPDATES), 2);
    CHECK_EQ(count(s, FOXAPI_LIST_PLUGINS), 3);  // the retired module was renamed out of the scan
    CHECK_EQ(levelOf(s, "addAllSummary"), -1.0);

    // CATALOGUE's columns and the flag.
    const unsigned abi = CASCADE_PLUGIN_ABI_VERSION;
    const FoxListItem alpha = catalogueEntry(s, "alpha-good");
    const FoxListItem beta = catalogueEntry(s, "beta.legal");
    const FoxListItem gamma = catalogueEntry(s, "gamma-nobuild");
    const FoxListItem delta = catalogueEntry(s, "delta-abi");
    const FoxListItem epsilon = catalogueEntry(s, "epsilon-nolicence");
    const FoxListItem stage = catalogueEntry(s, w.stageId);
    const FoxListItem nine = catalogueEntry(s, w.nineId);
    const uint32_t cf = FOXAPI_ITEM_COMPATIBLE | FOXAPI_ITEM_BUILD_HERE | FOXAPI_ITEM_INSTALLED;
    CHECK_EQ(std::string(alpha.name), std::string("Alpha Decoder"));
    CHECK_EQ(alpha.value[0], static_cast<double>(abi));
    CHECK_EQ(alpha.value[1], 12345.0);  // the build for THIS machine, not the other platform's
    CHECK_EQ(alpha.flags & cf, FOXAPI_ITEM_COMPATIBLE | FOXAPI_ITEM_BUILD_HERE);
    CHECK_EQ(beta.value[1], 0.0);  // not stated: a plate says "not told"
    CHECK_EQ(beta.flags & cf, FOXAPI_ITEM_COMPATIBLE | FOXAPI_ITEM_BUILD_HERE);
    CHECK_EQ(gamma.value[0], static_cast<double>(abi));
    CHECK_EQ(gamma.value[1], 0.0);  // no build: no size
    CHECK_EQ(gamma.flags & cf, FOXAPI_ITEM_COMPATIBLE);  // the right ABI, and nothing to install
    CHECK_EQ(delta.value[0], static_cast<double>(abi + 1u));
    CHECK_EQ(delta.value[1], 777.0);
    CHECK_EQ(delta.flags & cf, FOXAPI_ITEM_BUILD_HERE);  // a build, and the wrong ABI
    CHECK_EQ(epsilon.value[1], 4096.0);
    CHECK_EQ(epsilon.flags & cf, FOXAPI_ITEM_COMPATIBLE | FOXAPI_ITEM_BUILD_HERE);
    CHECK_EQ(stage.flags & cf, cf);  // installed (by the manifest: the host has no record of a retired module)
    CHECK_EQ(nine.flags & cf, cf);
    CHECK_EQ(nine.value[1], 2048.0);
    CHECK(FOXAPI_ITEM_BUILD_HERE != FOXAPI_ITEM_DIGEST_MISMATCH && (FOXAPI_ITEM_BUILD_HERE & 0x000FFFFFu) == 0u);

    // THE PER-ENTRY FACTS: whole, "" when there is none (known and empty), for every entry.
    CHECK_EQ(storeFact(s, "alpha-good", "author"), std::string("Alpha Labs Ltd"));
    CHECK_EQ(storeFact(s, "alpha-good", "licence"), std::string("MIT"));
    CHECK_EQ(storeFact(s, "alpha-good", "platforms"), w.hostPair + ", plan9/mips");  // the index's order
    CHECK_EQ(storeFact(s, "alpha-good", "floor", &st), std::string());  // no floor: "" and not NOT_FOUND
    CHECK_EQ(st, 0);
    CHECK_EQ(storeFact(s, "alpha-good", "file"), std::string("alpha_good.dll"));
    CHECK_EQ(storeFact(s, "alpha-good", "updateReason", &st), std::string());  // no plan
    CHECK_EQ(st, 0);
    CHECK_EQ(storeFact(s, "alpha-good", "description"), std::string(400, 'd'));
    CHECK_EQ(storeFact(s, "alpha-good", "blocked"), std::string());
    CHECK_EQ(storeFact(s, "beta.legal", "author", &st), std::string());  // an entry with no maker
    CHECK_EQ(st, 0);
    CHECK_EQ(storeFact(s, "beta.legal", "licence"), std::string("GPL-3.0-or-later"));
    CHECK_EQ(storeFact(s, "beta.legal", "floor"), std::string("0.9.0"));
    CHECK_EQ(storeFact(s, "beta.legal", "platforms"), w.hostPair);
    CHECK_EQ(storeFact(s, "beta.legal", "file"), std::string("beta_legal.dll"));
    CHECK_EQ(storeFact(s, "beta.legal", "legal"), std::string(300, 'L'));
    CHECK_EQ(storeFact(s, "beta.legal", "blocked"), std::string());  // the notice taken as acknowledged
    CHECK_EQ(storeFact(s, "gamma-nobuild", "platforms", &st), std::string());  // no build at all
    CHECK_EQ(st, 0);
    CHECK_EQ(storeFact(s, "gamma-nobuild", "file", &st), std::string());
    CHECK_EQ(st, 0);
    CHECK_EQ(storeFact(s, "gamma-nobuild", "blocked"), cascade::gui::pluginNoBuildReason(w.hostPair));
    CHECK_EQ(storeFact(s, "delta-abi", "blocked"), cascade::gui::pluginAbiMismatchReason(abi + 1u, abi));
    CHECK_EQ(storeFact(s, "delta-abi", "file"), std::string("delta_abi.dll"));
    CHECK_EQ(storeFact(s, "epsilon-nolicence", "licence", &st), std::string());  // declares none
    CHECK_EQ(st, 0);
    CHECK_EQ(storeFact(s, "epsilon-nolicence", "blocked"), std::string("the catalogue entry declares no licence"));
    // Installed, and updatable: the reason is the plan's own sentence; and the file joins to PLUGINS.
    CHECK_EQ(storeFact(s, w.nineId, "updateReason"), std::string("version 9.2.0 replaces the installed 9.0.0"));
    CHECK_EQ(storeFact(s, w.stageId, "updateReason"), std::string("version 2.0.0 replaces the installed 1.0.0"));
    CHECK_EQ(storeFact(s, w.stageId, "floor"), std::string("2.0.0"));
    CHECK_EQ(storeFact(s, w.nineId, "blocked"), std::string("already installed"));
    CHECK_EQ(storeFact(s, w.stageId, "blocked"), std::string("already installed"));
    CHECK_EQ(storeFact(s, w.nineId, "file"), w.n.g9);
    CHECK_EQ(storeFact(s, w.stageId, "file"), w.n.s1);
    {
        // THE JOIN: the entry's file against PLUGINS' detail, ignoring ASCII case. A fitted module has a record...
        const std::string want = lowerAscii(storeFact(s, w.nineId, "file"));
        bool found = false;
        for (int32_t i = 0; i < count(s, FOXAPI_LIST_PLUGINS); ++i) {
            const FoxListItem p = item(s, FOXAPI_LIST_PLUGINS, static_cast<uint32_t>(i));
            if (lowerAscii(p.detail) == want) {
                found = true;
                CHECK((p.flags & FOXAPI_ITEM_ACTIVE) != 0u);
            }
        }
        CHECK(found);
        // ...a retired one is INSTALLED and has none.
        const std::string gone = lowerAscii(storeFact(s, w.stageId, "file"));
        for (int32_t i = 0; i < count(s, FOXAPI_LIST_PLUGINS); ++i) {
            CHECK(lowerAscii(item(s, FOXAPI_LIST_PLUGINS, static_cast<uint32_t>(i)).detail) != gone);
        }
    }
    // The entries with no update have no plan, and STORE_UPDATES agrees with the facts exactly.
    for (const char* id : {"alpha-good", "beta.legal", "gamma-nobuild", "delta-abi", "epsilon-nolicence"}) {
        CHECK_EQ(storeFact(s, id, "updateReason"), std::string());
    }
    {
        std::set<std::string> planned;
        for (int32_t i = 0; i < count(s, FOXAPI_LIST_STORE_UPDATES); ++i) {
            planned.insert(item(s, FOXAPI_LIST_STORE_UPDATES, static_cast<uint32_t>(i)).name);
        }
        CHECK(planned == (std::set<std::string>{w.stageId, w.nineId}));
    }
    // NOT_FOUND: a leaf no entry has, an id nobody listed, the key with no leaf, an id's key with a leaf of another
    // kind; a REMOTE session reads every one of these (none is a path) and gets NOT_FOUND where the engine has none.
    storeFact(s, "alpha-good", "colour", &st);
    CHECK_EQ(st, FOXAPI_NOT_FOUND);
    storeFact(s, "no-such-entry", "file", &st);
    CHECK_EQ(st, FOXAPI_NOT_FOUND);
    storeFact(remote, "no-such-entry", "licence", &st);
    CHECK_EQ(st, FOXAPI_NOT_FOUND);
    fact(s, "fact.store.alpha-good", &st);
    CHECK_EQ(st, FOXAPI_NOT_FOUND);
    CHECK_EQ(storeFact(remote, "alpha-good", "author"), std::string("Alpha Labs Ltd"));
    CHECK_EQ(storeFact(remote, "alpha-good", "file"), std::string("alpha_good.dll"));
    CHECK_EQ(storeFact(remote, w.stageId, "updateReason"), std::string("version 2.0.0 replaces the installed 1.0.0"));
    CHECK_EQ(fact(remote, "fact.store.url", &st), std::string());
    CHECK_EQ(st, FOXAPI_DENIED);
    CHECK_EQ(fact(s, "fact.store.url"), w.index.string());

    // THE RETIRED MODULE (fact.blocked.<file>.*): keyed by the file, the id the update is matched on, and the
    // sentence WHOLE (STORE_BLOCKED's detail cuts at 159 bytes).
    const FoxListItem row = item(s, FOXAPI_LIST_STORE_BLOCKED, 0);
    CHECK_EQ(std::string(row.name), w.n.s1);
    CHECK_EQ(blockedFact(s, w.n.s1, "id"), w.stageId);
    const std::string message = blockedFact(s, w.n.s1, "message", &st);
    CHECK(st > 159);  // whole: more than a list item holds
    CHECK(message.find("out of date and has been disabled") != std::string::npos);
    CHECK(message.find(w.stageName) != std::string::npos);
    CHECK(message.find("Version 2.0.0 is available - update it to use it again.") != std::string::npos);
    CHECK(std::strlen(row.detail) <= 159u);
    CHECK_EQ(message.compare(0, std::strlen(row.detail), row.detail), 0);  // the detail is the same sentence, cut
    CHECK_EQ(blockedFact(remote, w.n.s1, "message"), message);  // a sentence is not a path
    CHECK_EQ(blockedFact(remote, w.n.s1, "id"), w.stageId);
    blockedFact(s, w.n.g9, "id", &st);
    CHECK_EQ(st, FOXAPI_NOT_FOUND);  // a module that is fitted and fine is not a retired row
    blockedFact(s, w.n.s1, "colour", &st);
    CHECK_EQ(st, FOXAPI_NOT_FOUND);
    fact(s, ("fact.blocked." + w.n.s1).c_str(), &st);
    CHECK_EQ(st, FOXAPI_NOT_FOUND);
    // The row's update is the one STORE_UPDATES plans for that id.
    {
        bool found = false;
        for (int32_t i = 0; i < count(s, FOXAPI_LIST_STORE_UPDATES); ++i) {
            const FoxListItem u = item(s, FOXAPI_LIST_STORE_UPDATES, static_cast<uint32_t>(i));
            if (w.stageId == u.name) {
                found = true;
                CHECK_EQ(std::string(u.detail), std::string("1.0.0 -> 2.0.0"));
            }
        }
        CHECK(found);
    }

    // A REFETCH that fails (a path that is not there) empties the catalogue and clears the status line.
    CHECK_EQ(pumped(d.eng, s, textCmd(FOXAPP_OP_SET_CATALOGUE_URL, (w.index.string() + ".missing"))).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_STORE_FETCH)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return count(s, FOXAPI_LIST_STORE_ACTIVITY) == 0; }));
    passes(d.eng, 2);
    CHECK_EQ(count(s, FOXAPI_LIST_CATALOGUE), 0);
    CHECK_EQ(levelOf(s, "catalogStatus"), -1.0);
    CHECK_EQ(levelOf(s, "catalogError"), 3.0);
    storeFact(s, "alpha-good", "author", &st);
    CHECK_EQ(st, FOXAPI_NOT_FOUND);  // the entry's facts went with it
    std::printf("I1: CATALOGUE's value[0], value[1] and BUILD_HERE, the six per-entry facts, fact.store.url (LOCAL), the retired row's id and whole sentence, the two edges and a failed refetch\n");
}

// I2: STORE_ACTIVITY with a transfer in flight, and the live progress: the entry, its name and kind, a progress
// that moves with no list moving, and the entry leaving with the result.
void testStoreActivityAndProgress() {
    using Access = cascade::gui::AppWindowTestAccess;
    StoreWorld w;
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    testing::Session remote(api(), d.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW, "s3cret");
    CHECK(remote.ok());
    CHECK(fetchCatalogue(d.eng, s, w.index, 7));
    passes(d.eng, 3);
    CHECK_EQ(count(s, FOXAPI_LIST_STORE_ACTIVITY), 0);
    CHECK_EQ(storeFact(s, "alpha-good", "blocked"), std::string());
    const auto levelOf = [&](const char* key) {
        for (int32_t i = 0; i < count(s, FOXAPI_LIST_STATUS_LINES); ++i) {
            const FoxListItem x = item(s, FOXAPI_LIST_STATUS_LINES, static_cast<uint32_t>(i));
            if (std::string(x.name) == key) { return x.value[0]; }
        }
        return -1.0;
    };

    // A DOWNLOAD begins (the worker's slot is a promise the test keeps), at a quarter.
    const uint64_t seq0 = s.state().listSeq;
    Access::beginTransfer(d.eng, "Alpha Decoder");
    Access::setProgress(d.eng, 0.25f);
    passes(d.eng, 1);
    const uint64_t seq1 = s.state().listSeq;
    CHECK(seq1 != seq0);  // the transfer starting moves the lists
    CHECK_EQ(count(s, FOXAPI_LIST_STORE_ACTIVITY), 1);
    {
        const FoxListItem a = item(s, FOXAPI_LIST_STORE_ACTIVITY, 0);
        CHECK_EQ(a.id, 1u);
        CHECK_EQ(std::string(a.name), std::string("Alpha Decoder"));
        CHECK_EQ(a.value[1], 2.0);  // downloading
        CHECK_EQ(a.value[0], 0.25);
        CHECK((a.flags & FOXAPI_ITEM_ACTIVE) != 0u);
    }
    CHECK_EQ(count(remote, FOXAPI_LIST_STORE_ACTIVITY), 1);
    CHECK_EQ(storeFact(s, "alpha-good", "blocked"), std::string("a transfer is already in progress"));

    // THE PROGRESS IS READ LIVE: a figure the worker stores shows at the next read, with no pass between and no
    // list moving; many passes of a moving figure leave listSeq where it was.
    Access::setProgress(d.eng, 0.5f);
    CHECK_EQ(item(s, FOXAPI_LIST_STORE_ACTIVITY, 0).value[0], 0.5);
    Access::setProgress(d.eng, 1.0f);
    CHECK_EQ(item(s, FOXAPI_LIST_STORE_ACTIVITY, 0).value[0], 1.0);
    for (int i = 0; i < 20; ++i) {
        Access::setProgress(d.eng, 0.01f * static_cast<float>(i));
        passes(d.eng, 1);
        CHECK_EQ(s.state().listSeq, seq1);
        CHECK_EQ(item(s, FOXAPI_LIST_STORE_ACTIVITY, 0).value[0], static_cast<double>(0.01f * static_cast<float>(i)));
    }

    // ...and the result: the worker is done, the next pass finds it, the pass after publishes the entry gone and the
    // reason in the red line (the table's own line is in the same block).
    Access::endTransfer(false, "Alpha Decoder", "the sha256 did not match");
    passes(d.eng, 1);
    passes(d.eng, 1);
    CHECK_EQ(count(s, FOXAPI_LIST_STORE_ACTIVITY), 0);
    CHECK_EQ(fact(s, "fact.status.installError"), std::string("the sha256 did not match"));
    CHECK_EQ(levelOf("installError"), 3.0);
    CHECK_EQ(storeFact(s, "alpha-good", "blocked"), std::string());  // idle again: the gate follows
    CHECK(s.state().listSeq != seq1);

    // A CATALOGUE FETCH is kind 1 with no name; the same progress figure is read.
    Access::beginTransfer(d.eng, "Alpha Decoder");
    passes(d.eng, 1);
    Access::endTransfer(false, "Alpha Decoder", "x");
    passes(d.eng, 2);
    CHECK_EQ(count(s, FOXAPI_LIST_STORE_ACTIVITY), 0);
    std::printf("I2: STORE_ACTIVITY entry 1 (name, kind), the live progress moving nothing, and the entry leaving with the result\n");
}

// I3: ADD ALL for real over three entries the loader's name rule refuses - every install fails before the network -
// reading STORE_ACTIVITY after every pass: entry 2's position is the module in flight (1-based), the total is
// fixed, entry 1 carries the same name, and the run ends with a red summary and nothing listed.
void testStoreAddAllRun() {
    StoreWorld w;
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    CHECK(fetchCatalogue(d.eng, s, w.indexRun, 3));
    passes(d.eng, 2);
    CHECK_EQ(count(s, FOXAPI_LIST_STORE_ACTIVITY), 0);
    const auto levelOf = [&](const char* key) {
        for (int32_t i = 0; i < count(s, FOXAPI_LIST_STATUS_LINES); ++i) {
            const FoxListItem x = item(s, FOXAPI_LIST_STATUS_LINES, static_cast<uint32_t>(i));
            if (std::string(x.name) == key) { return x.value[0]; }
        }
        return -1.0;
    };
    // The plan is the catalogue's order.
    const std::vector<std::string> order = {"Bad One", "Bad Two", "Bad Three"};
    CHECK_EQ(pumped(d.eng, s, textCmd(FOXAPP_OP_STORE_ADD_ALL, std::string(), 0)).status(), FOXAPI_OK);
    // The pass that applied it: the run is going, nothing has started (the pump runs after the publish).
    std::set<std::string> seen;
    std::vector<std::string> trace;
    bool sawFirstUnnamed = false;
    for (int i = 0; i < 400; ++i) {
        const int32_t n = count(s, FOXAPI_LIST_STORE_ACTIVITY);
        if (n == 0) { break; }
        const FoxListItem run = item(s, FOXAPI_LIST_STORE_ACTIVITY, static_cast<uint32_t>(n - 1));
        CHECK_EQ(run.id, 2u);
        CHECK_EQ(run.value[1], 3.0);  // the total
        CHECK((run.flags & FOXAPI_ITEM_ACTIVE) != 0u);
        const std::string name = run.name;
        if (name.empty()) {
            CHECK_EQ(run.value[0], 1.0);  // "starting": position 1 of 3
            sawFirstUnnamed = true;
        } else {
            const auto at = std::find(order.begin(), order.end(), name);
            CHECK(at != order.end());
            if (at != order.end()) {
                // THE POSITION IS THE MODULE IN FLIGHT, 1-BASED: the first module is 1 of 3, and the last one is 3.
                CHECK_EQ(run.value[0], static_cast<double>(at - order.begin() + 1));
            }
            seen.insert(name);
        }
        if (n == 2) {  // a transfer is in flight beside it: the same module, downloading
            const FoxListItem t = item(s, FOXAPI_LIST_STORE_ACTIVITY, 0);
            CHECK_EQ(t.id, 1u);
            CHECK_EQ(std::string(t.name), name);
            CHECK_EQ(t.value[1], 2.0);
        }
        trace.push_back(name + "@" + std::to_string(static_cast<int>(run.value[0])));
        d.eng.controlPass();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::string joined;
    for (const std::string& t : trace) { joined += (joined.empty() ? "" : " ") + t; }
    std::printf("I3: the run as published: %s\n", joined.c_str());
    CHECK(sawFirstUnnamed);
    CHECK(seen.size() == 3);  // every module was seen in flight (each is published once at least)
    passes(d.eng, 2);
    CHECK_EQ(count(s, FOXAPI_LIST_STORE_ACTIVITY), 0);
    // The summary: red (level 3) because modules failed, whole, in the table and in the window's copy.
    const std::string summary = fact(s, "fact.status.addAllSummary");
    CHECK(summary.rfind("0 installed, 3 failed.", 0) == 0);
    CHECK(summary.find("Bad One") != std::string::npos && summary.find("Bad Three") != std::string::npos);
    CHECK_EQ(levelOf("addAllSummary"), 3.0);
    CHECK_EQ(d.eng.statusText().addAllSummary, summary);
    CHECK(d.eng.statusText().addAllFailed);
    // A line the hook sets is information (level 1) unless the failed flag is set, and clears with its text.
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SETTING_SET, 0, 0, "test.statusLine=addAllFailed|0")).status(), FOXAPI_OK);
    CHECK_EQ(levelOf("addAllSummary"), 1.0);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SETTING_SET, 0, 0, "test.statusLine=addAllFailed|1")).status(), FOXAPI_OK);
    CHECK_EQ(levelOf("addAllSummary"), 3.0);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SETTING_SET, 0, 0, "test.statusLine=addAllSummary|")).status(), FOXAPI_OK);
    CHECK_EQ(levelOf("addAllSummary"), -1.0);
    CHECK_EQ(fact(s, "fact.status.addAllSummary"), std::string());
    // An entry whose file name the loader's rule refuses has no installable file: "" (known and empty), though
    // it has a build for this machine.
    int32_t st = 0;
    CHECK_EQ(storeFact(s, "bad-One", "file", &st), std::string());
    CHECK_EQ(st, 0);
    CHECK_EQ(storeFact(s, "bad-One", "platforms"), w.hostPair);
    CHECK((catalogueEntry(s, "bad-One").flags & FOXAPI_ITEM_BUILD_HERE) != 0u);
}

// I5: the counter the fingerprint hashes for the catalogue's content moves at every write of it: the hook's entry, a
// fetch starting (which empties it), a fetch landing, and a fetch failing (which empties it again). Each is
// redundant with another input on the paths a real fetch takes (its busy flag, the rescan a landing runs), so
// the counter is held directly: it is what makes the fingerprint exact, and a bump that is lost is invisible
// everywhere else.
void testStoreCatalogueCounter() {
    using Access = cascade::gui::AppWindowTestAccess;
    StoreWorld w;
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    passes(d.eng, 2);
    const uint64_t c0 = Access::catalogSeq(d.eng);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SETTING_SET, 0, 0, "test.catalogue=hook.id|Hook entry|")).status(), FOXAPI_OK);
    CHECK_EQ(Access::catalogSeq(d.eng), c0 + 1u);  // the hook's entry
    CHECK_EQ(count(s, FOXAPI_LIST_CATALOGUE), 1);
    CHECK_EQ(storeFact(s, "hook.id", "version"), std::string("1.0.0"));
    // The hook's entry carries no licence, no build and no ABI: every new column and fact is served for it too.
    const FoxListItem hook = catalogueEntry(s, "hook.id");
    CHECK_EQ(hook.value[0], 0.0);
    CHECK_EQ(hook.value[1], 0.0);
    CHECK_EQ(hook.flags & FOXAPI_ITEM_BUILD_HERE, 0u);
    int32_t st = 0;
    for (const char* leaf : {"author", "licence", "platforms", "floor", "file", "updateReason"}) {
        CHECK_EQ(storeFact(s, "hook.id", leaf, &st), std::string());
        CHECK_EQ(st, 0);
    }
    const uint64_t c1 = Access::catalogSeq(d.eng);
    CHECK_EQ(pumped(d.eng, s, textCmd(FOXAPP_OP_SET_CATALOGUE_URL, w.index.string())).status(), FOXAPI_OK);
    CHECK_EQ(Access::catalogSeq(d.eng), c1);  // the index is not the catalogue
    // A fetch: it empties the catalogue when it starts and replaces it when it lands (two writes, whichever pass
    // each falls in).
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_STORE_FETCH)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return count(s, FOXAPI_LIST_CATALOGUE) == 7; }));
    CHECK_EQ(Access::catalogSeq(d.eng), c1 + 2u);
    // ...and one that fails: emptied when it starts, emptied again when it fails.
    CHECK_EQ(pumped(d.eng, s, textCmd(FOXAPP_OP_SET_CATALOGUE_URL, (w.index.string() + ".missing"))).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_STORE_FETCH)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return count(s, FOXAPI_LIST_STORE_ACTIVITY) == 0; }));
    passes(d.eng, 2);
    CHECK_EQ(Access::catalogSeq(d.eng), c1 + 4u);
    CHECK_EQ(count(s, FOXAPI_LIST_CATALOGUE), 0);
    std::printf("I5: catalogSeq_ moves at the hook's entry, a fetch starting, landing and failing, and with the index alone it does not\n");
}

// I4: the two status lines through the hook (the writers are the engine's own: I1 and I3), and the language: a
// retired module's sentence is composed when the block is built, so a switch rebuilds it - while STORE_BLOCKED's
// detail, the sentence as stored, stays English.
void testStoreLinesAndLanguage() {
    StoreWorld w;
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    CHECK(fetchCatalogue(d.eng, s, w.index, 7));
    passes(d.eng, 3);
    const auto levelOf = [&](const char* key) {
        for (int32_t i = 0; i < count(s, FOXAPI_LIST_STATUS_LINES); ++i) {
            const FoxListItem x = item(s, FOXAPI_LIST_STATUS_LINES, static_cast<uint32_t>(i));
            if (std::string(x.name) == key) { return x.value[0]; }
        }
        return -1.0;
    };
    // catalogStatus: a hook line is information, whole, in the table's lines, as a fact and in the window's copy.
    const std::string long1 = "0 plugins in the catalogue " + std::string(190, 'c');  // (the hook's text is one command: 255)
    const uint64_t seq = s.state().listSeq;
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SETTING_SET, 0, 0, ("test.statusLine=catalogStatus|" + long1).c_str())).status(), FOXAPI_OK);
    CHECK_EQ(levelOf("catalogStatus"), 1.0);
    CHECK_EQ(fact(s, "fact.status.catalogStatus"), long1);
    CHECK_EQ(d.eng.statusText().catalogStatus, long1);
    CHECK(s.state().listSeq != seq);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SETTING_SET, 0, 0, "test.statusLine=catalogStatus|")).status(), FOXAPI_OK);
    CHECK_EQ(levelOf("catalogStatus"), -1.0);
    CHECK_EQ(d.eng.statusText().catalogStatus, std::string());

    // THE LANGUAGE.
    const std::string english = blockedFact(s, w.n.s1, "message");
    const std::string detailEnglish = item(s, FOXAPI_LIST_STORE_BLOCKED, 0).detail;
    CHECK(!english.empty());
    const std::string other = otherLanguage();
    CHECK(!other.empty());
    if (other.empty()) { return; }
    const uint64_t seq0 = s.state().listSeq;
    (void)cascade::i18n::setLanguage(other);
    d.eng.controlPass();
    const uint64_t seq1 = s.state().listSeq;
    const std::string translated = blockedFact(s, w.n.s1, "message");
    const std::string detailOther = item(s, FOXAPI_LIST_STORE_BLOCKED, 0).detail;
    (void)cascade::i18n::setLanguage("en");
    d.eng.controlPass();
    const std::string back = blockedFact(s, w.n.s1, "message");
    std::printf("I4: in %s the retired row's sentence is \"%.60s...\" (listSeq %llu -> %llu)\n", other.c_str(), translated.c_str(),
                static_cast<unsigned long long>(seq0), static_cast<unsigned long long>(seq1));
    CHECK(seq1 != seq0);              // the language is hashed: the block is rebuilt
    CHECK(translated != english);     // ...and the sentence with it, composed in the language in force
    CHECK_EQ(detailOther, detailEnglish);  // while the stored sentence in the list is English as it was
    CHECK_EQ(back, english);
    CHECK(translated.find(w.stageName) != std::string::npos);  // the plugin's own name is not translated
}

}  // namespace

int main() {
    engine_api_test::hermetic("api03");
    cascade::gui::AppWindowTestAccess::installFakeRadio();
    testVersionGate();
    testEveryListIsServed();
    testSubmitText();
    testStatusLines();
    testFacts();
    testPatchGraph();
    testRecordings();
    testScope();
    testNodeSpectrum();
    testSerialPorts();
    testDecoderLines();
    testStore();
    testBiasTee();
    testTransmitterState();
    testStatusColumnState();
    testStatusColumnGains();
    testStatusColumnRecordings();
    testListsBeforeFirstPublish();
    testNotePanel();
    // Group Z: the Airspy and HackRF drivers on fake wires beside the mock radio.
    testDeviceOpening();
    testAirspyLists();
    testConverterFacts();
    testConverterAliasAndCards();
    testBiasStandIn();
    testBiasRealDriver();
    testLanguageMovesNames();
    // Group R: the receiver rail and the display (window port group 5).
    testRdsState();
    testAutoNotchReadout();
    testPilotLock();
    testAudioHealthLine();
    testBandPlanName();
    // Group Q: the tuning surface (window port group 6).
    testBookmarksVersionFact();
    testInputRateFact();
    testSpectrumSeam();
    // Group 7: the recorder, bookmarks and scanner.
    testScannerDefaults();
    testScannerSettings();
    testScannerTimingsAreWholeMilliseconds();
    testScannerStates();
    testBookmarkStatusLines();
    testBookmarkExport();
    testBookmarkExportCost();
    // Group 8: the decoders, presets, marks, the plugin-mute cluster and the tune grants.
    testMuteCauses();
    testPluginLists();
    testPresetsAndMarks();
    testDecoderLinesLeaveListSeqAlone();
    // Group 9: the fitted-modules window.
    testPluginLogLinesAreCounted();
    testFittedModuleFacts();
    testFittedSizeAndNotice();
    testFittedFlags();
    testFittedCommands();
    testInstallStatusLines();
    // Group 10: the plugin store.
    testStoreBeforeAndFetch();
    testStoreActivityAndProgress();
    testStoreAddAllRun();
    testStoreCatalogueCounter();
    testStoreLinesAndLanguage();
    return check::finish("test_engine_api_03");
}
