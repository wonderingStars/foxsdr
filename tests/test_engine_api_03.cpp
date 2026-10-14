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
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "airspy_fake_usb.hpp"
#include "check.hpp"
#include "core/app_commands.hpp"
#include "core/freq_converter.hpp"
#include "core/i18n.hpp"
#include "core/patch_draft.hpp"
#include "core/patch_graph.hpp"
#include "engine/bias_tee.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
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
    for (uint32_t l = 1; l <= FOXAPI_LIST_PATCH_RADIOS; ++l) {
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
    CHECK(st.listSeq != base.listSeq);
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
        for (uint32_t l = 1; l <= FOXAPI_LIST_PATCH_RADIOS; ++l) {
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

    // A decoder printing a line moves listSeq - the signal the window reads
    // its facts on - and NOT the store's version: the markers are not copied
    // again for it.
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SETTING_SET, 0, 0, "test.decoderLine=TEST|a line")).status(),
             FOXAPI_OK);
    const std::uint64_t list1 = s.state().listSeq;
    std::printf("Q1: a decoder line: listSeq %llu -> %llu, version %lld\n", static_cast<unsigned long long>(list0),
                static_cast<unsigned long long>(list1), decimalFact(s, "fact.bookmarks.version"));
    CHECK(list1 != list0);
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
    return check::finish("test_engine_api_03");
}
