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
//   G11 the Source section and the sound card (window port group 11, API 0.3 section 8.10), against the walk, the
//       SoapySDR scan, the runtime's diagnostics and a sound card backend that are g11::world()'s (no USB walk, no
//       SoapySDR call, no PortAudio), and group Z's fake radios: G11-1 DEVICES' fixed rows, whole ids from
//       fact.source.id.<n> where detail cuts (a cut id refused, the fact's opening the radio), fact.source.row;
//       G11-2 sourceFlags, ten bits both ways; G11-3 the facts and their edges (LOCAL, NOT_FOUND, known and empty),
//       and that a hermetic scan reads no runtime diagnostics; G11-4 STATUS_LINES deviceFault whole where the state's
//       faultMessage cuts the 134-byte zombie sentence; G11-5 SAMPLE_RATES' captions; G11-6 UNBOUND_RADIOS and the
//       SDRplay note; G11-7 SCAN_EVIDENCE (LOCAL, taken once per empty whole landing); G11-8 the sound card's lists,
//       facts, FOXAPP_OP_SOUND_CARD_PICK's rate rule, the form's field mask, the converter behind the span, the open
//       and the keep; G11-9 the two per-frame commands the window used to send are the engine's own pass; G11-10
//       listSeq exactness: 38 inputs, each moving it once alone.
//   G12 the radar scope and the demod scope (window port group 12, API 0.3 section 8.11): G12-1 the receiver's
//       position in the state (FOXAPI_RX_POSITION_SET and rxLatDeg/rxLonDeg: set by SET_POSITION or restored from the
//       configuration into the first publish, refused ranges, repeats, moving seq alone); G12-2 FoxScopeInfo's
//       capacityFrames per tap, for a LOCAL and a REMOTE session, the most a call returns, and the info-edges.
//   G13 the Patch page, part 1 (window port group 13, API 0.3 section 8.12): G13-1 the patch runtime in the engine's pass
//       behind FOXAPP_OP_SET_PATCH_PAGE_OPEN (a generator radio, no window: the plan, START, the radio's centre and spectrum,
//       the page closed and opened, the grants, the default); G13-2 fact.patch.version and .epoch, a merge-safe
//       FOXAPP_OP_PATCH_SET_GRAPH (two sessions racing, START and a stale rename in one pass - red with the version left
//       out, green with it - the refusals, the history, the one block); G13-3 PATCH_PLAN against the core's own compile
//       with two real modules, the problems' codes, fact.patch.runnable and its two marks, PATCH_RADIOS' note and pending
//       open; G13-4 a settled page moves nothing.
//   G13c the Patch page, part 3 (the decoded pictures, API 0.3 section 8.12.7), against a REAL picture decoder module
//       (tests/fixture_group13_plugin.cpp) run by the pass on the generator, no window anywhere: G13c-1 the PATCH_LIVE picture
//       row, FOXAPI_EVENT_DECODER_IMAGE (every event readable when taken, ids rising, the node's name), read_image's buffer
//       protocol (info alone, pixels alone, a short buffer, a cap of 0, a cap with no buffer, an info of the minimum and a
//       roomier one), the unknown, stale and removed id, a VIEW-only and a REMOTE session reading the same, a late session,
//       a stopped patch keeping its picture, nothing moving listSeq; G13c-2 a patch document takes the pictures and an edit
//       does not; G13c-3 two reader threads (LOCAL and REMOTE) reading the newest picture as fast as they can against the
//       pass that takes pictures: every read whole.
//   G14b the GPS reader as the position row draws it (window port group 14, part b, API 0.3 section 8.13): the GPS list (48), ONE
//       entry read at the call, against a receiver on a fake wire through the reader's own opener and a source handed to it with
//       a timeout of the test's choosing: the idle entry, listening (the sentence, the satellites, the baud, the seconds LIVE),
//       the entry blank to a REMOTE session in its words and not in its figures, a run of states that moves no listSeq, Stop,
//       FIXED (the sentence the core words it, no coordinate anywhere in it, the position in the state), FOXAPP_STATUS_GPS_RESULT
//       (6: forgets a finished read, a no-op while it listens, DENIED to a REMOTE session), FAILED and TIMED OUT.
//   G14c the usage switch, the session counters and the diagnostics context (part c, section 8.13): G14c-1 fact.telemetry.enabled
//       and .installId (LOCAL: DENIED to a REMOTE session), consent mints the id and moves the lists once, a repeat is NO_CHANGE,
//       OFF forgets the id and a later consent mints a NEW one, the API's own switch stays refused, fact.diag.launches and
//       .crashes; G14c-2 the context block is rendered by the engine's own pass (a mode change and a radio opening follow within
//       the second, the radio's model alone moves it, a bundle carries the block whole and replaces nothing) and by applyConfig
//       (a context stands the moment it returns, before any pass).
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
#include "core/diag_report.hpp"
#include "core/gps_reader.hpp"
#include "core/version.hpp"
#include "core/nmea.hpp"
#include "core/serial_port.hpp"
#include "core/scanner.hpp"
#include "core/i18n.hpp"
#include "core/patch_draft.hpp"
#include "core/patch_graph.hpp"
#include "core/plugin_repo.hpp"
#include "core/diag_log.hpp"
#include "core/plugin_run.hpp"
#include "core/plugin_ui.hpp"
#include "engine/bias_tee.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine/plugin_store_reasons.hpp"
#include "engine/soundcard_panel.hpp"
#include "engine_api_fake_radio.hpp"
#include "engine_api_test_env.hpp"
#include "engine_api_serial.hpp"
#include "engine_client.hpp"
#include "foxsdr_api.h"
#include "hackrf_fake_usb.hpp"
#include "source/airspy_source.hpp"
#include "source/hackrf_source.hpp"
#include "engine/airspy_panel.hpp"
#include "engine/receiver_tables.hpp"
#include "source/rtlsdr_source.hpp"
#include "source/rx888_source.hpp"

namespace radios83 {
std::unique_ptr<cascade::source::DeviceSource> make(const std::string& kind);
std::vector<cascade::source::NativeDeviceInfo> scan();
extern std::atomic<bool> g_gateOpen;
}  // namespace radios83

// <<G11 WORLD BEGIN
// --- group 11's world: what the Source section's hooks hand the engine ------------------------------------
//
// The native walk (rows, dongles that cannot be opened, the SDRplay advice), the SoapySDR scan (its rows, held on a
// gate when a test wants the scan in flight), the SoapySDR runtime's diagnostics (a counter says how often they were
// asked), and a sound card backend (its listing and its open each behind a gate, a counter for each). Nothing on the
// desk is enumerated, probed or opened: the real calls into SoapySDR are never made.
namespace g11 {

struct World {
    std::vector<cascade::source::NativeDeviceInfo> natives;
    std::vector<cascade::usb::UsbDeviceInfo> unbound;
    std::string sdrAdvice, sdrDetail;
    bool sdrRows = false;
    std::vector<cascade::source::SoapyDeviceInfo> soapy;
    std::atomic<bool> soapyGate{true};
    std::atomic<int> soapyCalls{0};
    std::vector<std::string> modules, searchPaths, faulted;
    std::vector<cascade::source::VendorRoot> vendors;
    std::atomic<int> diagCalls{0};
    std::vector<cascade::source::SoundCardDevice> cards;
    std::atomic<bool> listGate{true}, openGate{true};
    std::atomic<int> listCalls{0}, openCalls{0};
};

inline World& world() {
    static World w;
    return w;
}

// The long texts of the ids: the Pluto's address (an address box takes 191 bytes) and a SoapySDR row's kwargs.
inline std::string plutoUriLong() { return "ip:" + std::string(186, 'p'); }  // 189 bytes
inline std::string soapyLongArgs() { return "driver=uhd,addr=192.168.10.2,type=" + std::string(262, 'x'); }

inline void waitOn(const std::atomic<bool>& gate) {
    for (int i = 0; i < 10000 && !gate.load(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
}

class Card final : public cascade::source::SoundCardBackend {
public:
    std::vector<cascade::source::SoundCardDevice> listDevices() override {
        ++world().listCalls;
        waitOn(world().listGate);
        return world().cards;
    }
    bool open(const cascade::source::SoundCardDevice&, int, double, bool, PushFn, void*, std::string&) override {
        ++world().openCalls;
        waitOn(world().openGate);
        open_ = true;
        return true;
    }
    void close() override { open_ = false; }
    bool alive() override { return open_.load(); }

private:
    std::atomic<bool> open_{false};
};

inline void reset() {
    World& w = world();
    w.natives = {{engine_api_test::kFakeRadioDriver, "Mock RTL-SDR (serial 00000001)", engine_api_test::kFakeRadioArgs},
                 {"airspy", "Airspy R2", std::string("serial=") + "644866c83f1a51df"},
                 {"hackrf", "HackRF One", "serial=0000000000000000457863c8"},
                 {"pluto", "ADALM-Pluto (network)", "uri=ip:192.168.2.1"}};
    w.unbound.clear();
    w.sdrAdvice.clear();
    w.sdrDetail.clear();
    w.sdrRows = false;
    w.soapy = {{"Fake Soapy A", "driver=fake,serial=A"}, {"Long Soapy", soapyLongArgs()}};
    w.soapyGate = true;
    w.soapyCalls = 0;
    w.modules.clear();
    w.searchPaths.clear();
    w.faulted.clear();
    w.vendors.clear();
    w.diagCalls = 0;
    cascade::source::SoundCardDevice a;
    a.index = 0;
    a.name = "Fake In A";
    a.hostApi = "Fake API";
    a.maxInputChannels = 2;
    a.defaultRateHz = 48000.0;
    a.isDefault = true;
    a.rates = {{44100.0, false}, {48000.0, false}, {96000.0, true}};
    cascade::source::SoundCardDevice b;
    b.index = 1;
    b.name = "Fake In B";
    b.hostApi = "Fake API";
    b.maxInputChannels = 2;
    b.defaultRateHz = 192000.0;
    b.rates = {{96000.0, false}, {192000.0, false}};
    // A card whose default is not one of its rates: a pick that must re-choose lands on its highest.
    cascade::source::SoundCardDevice c;
    c.index = 2;
    c.name = "Fake In C";
    c.hostApi = "Fake API";
    c.maxInputChannels = 2;
    c.defaultRateHz = 50000.0;
    c.rates = {{32000.0, false}, {64000.0, false}};
    w.cards = {a, b, c};
    w.listGate = true;
    w.openGate = true;
    w.listCalls = 0;
    w.openCalls = 0;
}

}  // namespace g11

// G11 WORLD END>>
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
    // Group 14b: the engine's GPS reader - a receiver on a fake wire (the opener the op's start goes through, or a source
    // handed to it directly with a timeout of the test's choosing), and the reader's own account of itself.
    static void gpsOpener(cascade::engine::Engine& e, cascade::core::GpsReader::Opener op) { e.gpsReader_.setOpenerForTest(std::move(op)); }
    static void gpsStart(cascade::engine::Engine& e, std::unique_ptr<cascade::core::ByteSource> src, const std::string& port, int baud,
                         double timeoutS) {
        cascade::core::GpsReader::Options o;
        o.port = port;
        o.baud = baud;
        o.timeoutS = timeoutS;
        e.gpsReader_.start(std::move(src), o);
    }
    static cascade::core::GpsReader::Status gpsStatus(cascade::engine::Engine& e) { return e.gpsReader_.status(); }
    // Group 14c: the session counters a config would have set, and the engine's own account of itself.
    static void setCounters(cascade::engine::Engine& e, std::uint64_t launches, std::uint64_t crashes) {
        e.telemetryLaunches_ = launches;
        e.telemetryCrashes_ = crashes;
    }
    static bool telemetryOn(cascade::engine::Engine& e) { return e.telemetryEnabled_; }
    static std::string telemetryId(cascade::engine::Engine& e) { return e.telemetryInstallId_; }
    static void setDeviceModel(cascade::engine::Engine& e, const std::string& m) { e.deviceModel_ = m; }
    static void gpsStop(cascade::engine::Engine& e) { e.gpsReader_.stop(); }
    // Group 13: what only an attempt leaves in the patch runtime, put there behind its back - a plugin that refused to
    // start on a radio's set, the note a running radio's face prints, and an open still on its worker.
    static void injectRefused(cascade::engine::Engine& e, unsigned radio, std::vector<cascade::core::patch::NodeId> nodes) {
        e.patchRefusedBy_[radio] = std::move(nodes);
    }
    // Group 13b: the devices a Radio can be set to as the engine composes them (key, label), the folder the recordings and
    // the speakers' files are in, a module put in the parts bin behind its back (the bin is rebuilt from the loaded
    // modules every pass the page is open: put it there with the page closed), and the key the receiver's open radio has.
    static std::vector<std::pair<std::string, std::string>> choices(cascade::engine::Engine& e) {
        std::vector<std::pair<std::string, std::string>> out;
        for (const auto& c : e.patchDeviceChoices()) { out.emplace_back(c.key, c.label); }
        return out;
    }
    static void addPart(cascade::engine::Engine& e, const std::string& key, const std::string& name,
                        cascade::core::patch::PortType feed, bool tracks, bool image, bool replace = false) {
        if (replace) {
            for (auto& d : e.patchCatalogue_) {
                if (d.key == key) {
                    d.tracks = tracks;
                    d.image = image;
                    return;
                }
            }
        }
        cascade::core::patch::DecoderInfo d;
        d.key = key;
        d.name = name;
        d.feed = feed;
        d.tracks = tracks;
        d.image = image;
        e.patchCatalogue_.push_back(d);
    }
    static void setRawCentre(cascade::engine::Engine& e, double hz) { (void)e.pipeline_.rawSource().setCenterFrequencyHz(hz); }
    static std::string deviceKeyNow(cascade::engine::Engine& e) {
        return cascade::core::patch::makeDeviceKey(e.sourceKind_, e.deviceArgs_);
    }
    static void injectRadioNote(cascade::engine::Engine& e, unsigned node, const std::string& text) {
        e.patchRadioError_[node] = text;
    }
    struct PendingOpen {
        std::promise<cascade::engine::Engine::PatchRadioOpen> promise;
    };
    static void injectPending(cascade::engine::Engine& e, unsigned node, PendingOpen& held) {
        e.patchRadioPending_[node] = held.promise.get_future();
        e.patchRadioPendingAs_[node] = "test@1";
    }
    // The open answers: it failed, with no source. Always before the engine goes (its teardown waits for the future).
    static void answerPending(PendingOpen& held) { held.promise.set_value(cascade::engine::Engine::PatchRadioOpen{}); }
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
    static void setRecordDir(cascade::engine::Engine& e, const std::string& dir) { e.recordDir_ = dir; }
    // Both recorders' opener (stub campaign B18: fact.recorder.start): a disk that answers when the test lets it.
    static void bindOpener(cascade::engine::Engine& e, cascade::core::Recorder::Opener o) {
        e.iqRecorder_.bindOpener(o);
        e.audioRecorder_.bindOpener(std::move(o));
    }
    static cascade::core::Recorder::Opener opener(cascade::engine::Engine& e) { return e.audioRecorder_.opener(); }
    static void setSquelch(cascade::engine::Engine& e, float db) { e.squelchDb_ = db; }
    static cascade::core::FreqManager& store(cascade::engine::Engine& e) { return e.freqMgr_; }
    static const std::string& note(cascade::engine::Engine& e) { return e.bookmarkImportNote_; }
    // THE ENGINE'S DISK WORKERS (stub campaign B19): the I/Q file's open, the list's import and export and the recordings list are workers', and
    // their answers are counted here (each finished one moves its count). `hook` is the slow-disk seam they call before they touch the disk.
    static std::uint64_t exportSeq(cascade::engine::Engine& e) { return e.exportLast_.seq; }
    static std::uint64_t importSeq(cascade::engine::Engine& e) { return e.importLast_.seq; }
    static bool exportPending(cascade::engine::Engine& e) { return e.bookmarkExportPending(); }
    static bool importPending(cascade::engine::Engine& e) { return e.bookmarkImportPending(); }
    static bool recordingScanPending(cascade::engine::Engine& e) { return e.recordingScanPending(); }
    static bool iqOpenPending(cascade::engine::Engine& e) { return e.iqOpenPending(); }
    static bool lastExportOk(cascade::engine::Engine& e) { return e.exportLast_.ok; }
    static std::string lastExportPath(cascade::engine::Engine& e) { return e.exportLast_.shown; }
    static void diskHook(cascade::engine::Engine& e, std::function<void()> hook) { e.setDiskHookForTest(std::move(hook)); }
    // Group M: the folder the engine looks for plugins in (null: the default).
    static void setPluginDir(std::string (*dir)()) { cascade::engine::Engine::testHooks_.pluginDir = dir; }
    // Group M: a plugin asks to tune the receiver (the host bridge's request_tune), by its module file name.
    static std::int32_t askToTune(cascade::engine::Engine& e, const std::string& key) {
        return e.headlessUi_.tuneRequestFromPlugin(key, 100.0e6);  // (no window here: the engine's own plugin UI answers the bridge)
    }
    // Group N (a fitted plugin runs only while it is used, stub campaign B2): a "window" - a plugin UI the way the interface's feed holds
    // one, applying the directives the engine posts - and the engine's run state, each reachable alone.
    static void installWindow(cascade::engine::Engine& e, cascade::core::PluginUi& ui) {
        e.setPluginUiApplier([&ui](const cascade::core::PluginDirective& d) { ui.apply(d); });
    }
    static void removeWindow(cascade::engine::Engine& e) { e.setPluginUiApplier({}); }
    static cascade::core::PluginRunner& runner(cascade::engine::Engine& e) { return e.pluginRunner_; }
    static cascade::core::PluginRun runMode(cascade::engine::Engine& e, const std::string& key) { return e.pluginRunMode(key); }
    static void restoreConfig(cascade::engine::Engine& e, const cascade::core::AppConfig& c) { e.applyConfig(c); }
    static cascade::core::AppConfig readConfig(cascade::engine::Engine& e) {
        cascade::core::AppConfig c;
        e.fillConfig(c);
        return c;
    }
    // Group 9: the host's side of the plugin API, which a plugin's own calls reach through the host
    // bridge's trampolines (log, add_command, a request that needs the settings grant...). The test makes
    // the same calls on the loaded module's client, from the test thread.
    static cascade::core::PluginApiCore& core(cascade::engine::Engine& e) { return *e.pluginApi_; }
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
// <<G11 ACCESS BEGIN
    // ---- Group 11 (the Source section and the sound card): the hooks, and the inputs of the table, each reachable
    // alone. The walk, the SoapySDR scan, the runtime's diagnostics and the sound card backend are g11::world()'s.
    static void g11Walk(cascade::engine::Engine::NativeScanResult& out) {
        const g11::World& w = g11::world();
        out.devices = w.natives;
        out.unbound = w.unbound;
        out.sdrPlayAdvice = w.sdrAdvice;
        out.sdrPlayApiDetail = w.sdrDetail;
        out.sdrPlayRowsFound = w.sdrRows;
    }
    static std::vector<cascade::source::SoapyDeviceInfo> g11Soapy() {
        g11::World& w = g11::world();
        ++w.soapyCalls;
        g11::waitOn(w.soapyGate);
        return w.soapy;
    }
    static cascade::engine::Engine::ScanDiagnostics g11Diagnostics() {
        g11::World& w = g11::world();
        ++w.diagCalls;
        cascade::engine::Engine::ScanDiagnostics d;
        d.modules = w.modules;
        d.searchPaths = w.searchPaths;
        d.vendors = w.vendors;
        d.faultedDrivers = w.faulted;
        return d;
    }
    static std::shared_ptr<cascade::source::SoundCardBackend> g11Card() { return std::make_shared<g11::Card>(); }
    // The hooks of group 11: the Airspy, the HackRF and the mock radio on their fake wires (group Z's), the walk, the
    // scan, the diagnostics and the card backend above. `diagnostics` false leaves the runtime's diagnostics unhooked
    // (the engine must then read none of them: the scan is stood in for).
    static void install11(bool diagnostics = true) {
        g11::reset();
        radios83::g_gateOpen = true;
        cascade::engine::Engine::testHooks_.makeDevice = &radios83::make;
        cascade::engine::Engine::testHooks_.nativeScan = nullptr;
        cascade::engine::Engine::testHooks_.nativeScanDetail = &g11Walk;
        cascade::engine::Engine::testHooks_.soapyScan = &g11Soapy;
        cascade::engine::Engine::testHooks_.scanDiagnostics = diagnostics ? &g11Diagnostics : nullptr;
        cascade::engine::Engine::testHooks_.soundCardBackend = &g11Card;
    }
    static void uninstall11() {
        g11::reset();
        radios83::g_gateOpen = true;
        cascade::engine::Engine::testHooks_.nativeScanDetail = nullptr;
        cascade::engine::Engine::testHooks_.scanDiagnostics = nullptr;
        cascade::engine::Engine::testHooks_.soundCardBackend = nullptr;
        installFakeRadio();
    }
    static std::uint64_t fingerprint(cascade::engine::Engine& e) { return e.apiListsFingerprint(); }
    static std::uint64_t publishLists(cascade::engine::Engine& e) { return e.publishApiLists(); }
    static std::string idForRow(cascade::engine::Engine& e, int row) { return e.sourceIdForRow(row); }
    // The inputs, poked one at a time (the table must follow each: they are in the fingerprint).
    static int& sourceSel(cascade::engine::Engine& e) { return e.sourceSel_; }
    static std::string& keepKind(cascade::engine::Engine& e) { return e.restoreKeep_.kind; }
    static bool& scanned(cascade::engine::Engine& e) { return e.soapyScanned_; }
    static bool& scanPending(cascade::engine::Engine& e) { return e.soapyScanPending_; }
    static bool& scanPartial(cascade::engine::Engine& e) { return e.soapyScanPartial_; }
    static bool& deferredLogged(cascade::engine::Engine& e) { return e.soapyScanDeferredLogged_; }
    static bool& networkUsrps(cascade::engine::Engine& e) { return e.lookForNetworkUsrps_; }
    static std::vector<std::string>& absent(cascade::engine::Engine& e) { return e.soapyAbsentDrivers_; }
    static bool& sdrRowsFound(cascade::engine::Engine& e) { return e.sdrPlayRowsFound_; }
    static std::vector<cascade::usb::UsbDeviceInfo>& unbound(cascade::engine::Engine& e) { return e.nativeUnbound_; }
    static std::vector<std::string>& faulted(cascade::engine::Engine& e) { return e.faultedDrivers_; }
    static std::uint64_t& evidenceSeq(cascade::engine::Engine& e) { return e.scanEvidenceSeq_; }
    static std::string& busyLabel(cascade::engine::Engine& e) { return e.deviceBusyLabel_; }
    static bool& openPending(cascade::engine::Engine& e) { return e.deviceOpenPending_; }
    static std::string& deviceModel(cascade::engine::Engine& e) { return e.deviceModel_; }
    static std::string& faultText(cascade::engine::Engine& e) { return e.apiFaultText_; }
    static char* plutoBuffer(cascade::engine::Engine& e) { return e.plutoUri_; }
    static std::vector<std::string>& rateLabels(cascade::engine::Engine& e) { return e.deviceRateLabels_; }
    static bool& cardListed(cascade::engine::Engine& e) { return e.soundCardListed_; }
    static bool& cardScanning(cascade::engine::Engine& e) { return e.soundCardScanPending_; }
    static bool& cardOpening(cascade::engine::Engine& e) { return e.soundCardOpenPending_; }
    static cascade::source::SoundCardSettings& form(cascade::engine::Engine& e) { return e.soundCard_; }
    static std::vector<cascade::source::SoundCardDevice>& cards(cascade::engine::Engine& e) { return e.soundCardDevices_; }
    static const cascade::source::SoundCardSettings& liveCard(cascade::engine::Engine& e) { return e.soundCardLive_; }
    static const std::string& sourceKind(cascade::engine::Engine& e) { return e.sourceKind_; }
    static bool gated(cascade::engine::Engine& e) { return e.soapyScanGated(); }
    static std::string gateDevice(cascade::engine::Engine& e) { return e.soapyScanGateDevice(); }
    static std::map<std::string, cascade::core::ConverterSetting>& converters(cascade::engine::Engine& e) { return e.converters_; }
    static std::string& cardMissing(cascade::engine::Engine& e) { return e.soundCardMissing_; }
    // A patch radio's open that is still on its worker: a promise this test keeps (never satisfied, never destroyed
    // while the engine can look at it), so the future is valid and not ready, as a real open in flight is.
    static void pendPatchRadio(cascade::engine::Engine& e, bool on) {
        static std::promise<cascade::engine::Engine::PatchRadioOpen> keep;
        if (on) {
            keep = std::promise<cascade::engine::Engine::PatchRadioOpen>();
            e.patchRadioPending_[99] = keep.get_future();
        } else {
            e.patchRadioPending_.clear();
        }
    }
    static void scanNative(cascade::engine::Engine& e) { e.scanNative(); }
// G11 ACCESS END>>
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
    engine_api_test::SerialEngine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    int served = 0;
    for (uint32_t l = 1; l <= FOXAPI_LIST_FREQ_MARKERS; ++l) {  // (the newest list: 53, stub campaign B10; 52 is B9's settings forms and 51 the live Airband list, test_engine_api_airband's)
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
    engine_api_test::SerialEngine e(api(), kHooks);
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
    engine_api_test::SerialEngine e(api(), kHooks);
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
    engine_api_test::SerialEngine e(api(), kHooks);
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
    engine_api_test::SerialEngine e(api(), kHooks);
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
    engine_api_test::SerialEngine e(api(), kHooks);
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
    engine_api_test::SerialEngine e(api(), kHooks);
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
    engine_api_test::SerialEngine e(api(), kHooks);
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
    engine_api_test::SerialEngine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    Remote r(e);
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SCAN_SERIAL_PORTS)).result.status, FOXAPI_OK);
    CHECK(count(s, FOXAPI_LIST_SERIAL_PORTS) >= 0);
    CHECK_EQ(r.run(cmd(FOXAPI_OP_SCAN_SERIAL_PORTS)).status(), FOXAPI_DENIED);
}

void testDecoderLines() {
    engine_api_test::SerialEngine e(api(), kHooks);
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
    engine_api_test::SerialEngine e(api(), kHooks);
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
    engine_api_test::SerialEngine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    const FoxReceiverState st = s.state();
    CHECK((st.flags & (FOXAPI_RX_BIAS_TEE_PRESENT | FOXAPI_RX_BIAS_TEE_ON)) == 0u);
    CHECK_EQ(fact(s, "fact.biasTee.reachable"), std::string("0"));
    CHECK((st.flags & FOXAPI_RX_PATCH_RUNNING) == 0u);
}

void testTransmitterState() {
    engine_api_test::SerialEngine e(api(), kHooks);
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

// THE SINK IN ITS STEADY STATE, which is the only state a counter comparison may be taken against.
//
// A test here compares audioSeq / seq (or countersMoved(), which names audio among them) before and after ONE
// command, and the contract it holds is exact: the command moves the counter once. That is true only if nothing
// else about the sink moves in between - and the sink is a real output stream on a real device (the hermetic
// engine opens the machine's own, when it has one). A stream can die under a healthy receiver: Pa_IsStreamActive
// goes 0, STREAM_ALIVE falls (one audioSeq step), the engine's watchdog (Engine::pollAudioHealth, 1 Hz) reopens
// it - the first attempt can itself fail and be retried a second later - and STREAM_ALIVE returns (a second
// step). The engine is right to say so each time. Under a saturated machine (ctest -j 8 over the whole suite,
// with a dozen other processes opening and closing the same device) that death lands at an arbitrary moment, so
// a baseline taken while the sink is down - or a baseline/probe pair that straddles the recovery - reads one or
// two audioSeq steps more than the command made (the recorded failures: 7 vs 6, 9 vs 8, flags 14 vs 10, and
// "seq audio rds" for "seq rds"). The sampling raced; the engine did not misbehave.
//
// So a baseline waits for the sink to be steady first: no open in flight; a sink that has ever opened is alive
// (the watchdog has done its work); and the 0.2 open flag tells the same story as the watchdog's view. A
// machine with no output device is steady from the first read (nothing ever opened, nothing to lose).
bool audioSteady(const FoxReceiverState& st) {
    if ((st.audioFlags & FOXAPI_AUDIO_OPENING) != 0u) { return false; }  // a worker is still inside the open
    const bool everOpened = (st.audioFlags & FOXAPI_AUDIO_EVER_OPENED) != 0u;
    const bool alive = (st.audioFlags & FOXAPI_AUDIO_STREAM_ALIVE) != 0u;
    if (everOpened && !alive) { return false; }  // the stream died; the watchdog has not brought it back yet
    return alive == ((st.flags & FOXAPI_RX_SINK_OPEN) != 0u);
}

// How long a baseline waits for a dead stream to come back: the watchdog probes once a second, a reopen is
// bounded at 1.5 s (audio_open.hpp kOpenBound) and a refused first attempt is retried a probe later - several
// seconds on a loaded machine, and the observed worst is under four. Generous: a bound this long is only ever
// spent when the device is really gone, and then the checks that follow fail by name instead of hanging.
constexpr int kSinkSteadyMs = 30000;

// s.state() once the sink is steady: for a baseline taken straight off a read rather than after a settle. Costs
// nothing when the sink is steady (the first read answers); the state returned is that steady read.
FoxReceiverState steadyState(testing::Session& s) {
    FoxReceiverState st = s.state();
    (void)check::waitFor(
        [&] {
            st = s.state();
            return audioSteady(st);
        },
        kSinkSteadyMs);
    return st;
}

// The state once it has stopped changing: the audio open the engine starts at
// boot (on a worker) and its first passes have landed - `seq` has held still
// for 300 ms - so a test that compares counters before and after a command
// is not comparing against a sink that was still opening. AND the sink is in
// its steady state (audioSteady) for that whole window, so audioSeq and
// audioFlags have held still over several reads (about fifteen 20 ms samples)
// with the stream alive, not merely between two reads taken while it was
// down. The bound is the sink's (kSinkSteadyMs) plus the settle itself.
FoxReceiverState settled(testing::Session& s) {
    using Clock = std::chrono::steady_clock;
    FoxReceiverState st = s.state();
    auto since = Clock::now();
    const auto end = since + std::chrono::milliseconds(kSinkSteadyMs) + std::chrono::seconds(10);
    while (Clock::now() < end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        FoxReceiverState now = s.state();
        if (now.seq != st.seq || now.audioFlags != st.audioFlags || !audioSteady(now)) {
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
    engine_api_test::SerialEngine e(api(), kHooks);
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

// Y2: A BASELINE IS NEVER TAKEN OFF A SINK THAT IS DOWN. The race that failed Y (and R) under a loaded machine,
// made deterministic: the stream dies under a healthy receiver (test.audioClose is the stand-in for a USB card
// re-enumerating or a device dying under load - what the loaded suite does by chance), and the baseline is asked
// for while it is down. settled() must hand back a state taken after the engine's watchdog has reopened the
// stream, so the one-command pair that follows reads exactly one audioSeq step, as Y asserts. Before audioSteady()
// settled() only waited for `seq` to hold still for 300 ms - the watchdog's next probe can be a second away - and
// the pair then straddled STREAM_ALIVE coming back (audioSeq +2, flags 14 vs 10). Only on a machine that has an
// output device to lose (without one nothing ever opened and the sink is steady from the first read).
void testBaselineWaitsForTheSink() {
    engine_api_test::SerialEngine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    const FoxReceiverState first = settled(s);
    CHECK(audioSteady(first));  // a settled state is a steady one
    if ((first.audioFlags & FOXAPI_AUDIO_EVER_OPENED) == 0u) {
        std::printf("Y2: no output device on this machine - no stream to lose, nothing to wait for\n");
        return;
    }
    CHECK((first.audioFlags & FOXAPI_AUDIO_STREAM_ALIVE) != 0u);

    // The stream dies; the baseline is asked for at once.
    CHECK_EQ(setting(s, "test.audioClose="), FOXAPI_OK);
    const FoxReceiverState down = s.state();
    CHECK((down.audioFlags & FOXAPI_AUDIO_STREAM_ALIVE) == 0u);
    CHECK(!audioSteady(down));
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SET_FREQUENCY, 100.0e6)).result.status, FOXAPI_OK);
    CHECK_EQ(setting(s, "test.muteBy=Alpha decoder@145500000"), FOXAPI_OK);
    const FoxReceiverState off = settled(s);
    CHECK(audioSteady(off));
    CHECK((off.audioFlags & FOXAPI_AUDIO_STREAM_ALIVE) != 0u);  // the watchdog's reopen is behind the baseline
    CHECK((off.flags & FOXAPI_RX_SINK_OPEN) != 0u);
    CHECK_EQ(std::string(off.audioMutedBy), std::string());

    // The pair taken off it keeps its contract exactly: one command, one step.
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SET_FREQUENCY, 145.5e6)).result.status, FOXAPI_OK);
    CHECK(check::waitFor([&] { return std::string(s.state().audioMutedBy) == "Alpha decoder"; }, 2000));
    const FoxReceiverState on = settled(s);
    CHECK_EQ(on.audioFlags, off.audioFlags);
    CHECK_EQ(on.audioSeq, off.audioSeq + 1u);

    // steadyState() is the same wait for a baseline that is not a settle: asked while the stream is down it
    // returns after the reopen, and it costs nothing when the stream is up.
    const FoxReceiverState nowSteady = steadyState(s);
    CHECK(audioSteady(nowSteady));
    CHECK_EQ(setting(s, "test.audioClose="), FOXAPI_OK);
    CHECK((s.state().audioFlags & FOXAPI_AUDIO_STREAM_ALIVE) == 0u);
    const FoxReceiverState back = steadyState(s);
    CHECK(audioSteady(back));
    CHECK((back.audioFlags & FOXAPI_AUDIO_STREAM_ALIVE) != 0u);
    CHECK(back.audioSeq > nowSteady.audioSeq);  // the fall and the return were both seen, and both are behind it
    std::printf("Y2: baselines waited for the reopened stream (audioSeq %llu -> %llu)\n",
                static_cast<unsigned long long>(off.audioSeq), static_cast<unsigned long long>(back.audioSeq));
}

// RECEIVER: the GAINS entries carry the unit token.
void testStatusColumnGains() {
    engine_api_test::SerialEngine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK_EQ(count(s, FOXAPI_LIST_GAINS), 0);  // no radio, no stages
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SCAN_DEVICES)).result.status, FOXAPI_OK);
    CHECK(check::waitFor([&] { return count(s, FOXAPI_LIST_DEVICES) == 4; }, 3000));
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
    engine_api_test::SerialEngine e(api(), kHooks);
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
        for (uint32_t l = 1; l <= FOXAPI_LIST_FREQ_MARKERS; ++l) {  // (the newest list: 53, stub campaign B10; 52 is B9's settings forms and 51 the live Airband list, test_engine_api_airband's)
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

// RECORDER START (stub campaign B18, fact.recorder.start): where each take's start is - '0' none out, '1' the file is being opened on its worker, '2' the start was withdrawn and the worker
// has not answered - the I/Q take's character first, then the audio take's. It moves the lists' listSeq when it changes (it is hashed), and a take that is recording is '0' again.
void testRecordStartFact() {
    using Access = cascade::gui::AppWindowTestAccess;
    static std::atomic<bool> release{false};
    static cascade::core::Recorder::Opener real;
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    release = false;
    real = Access::opener(d.eng);
    Access::bindOpener(d.eng, [](const cascade::core::Recorder::OpenRequest& req, cascade::core::Recorder::OpenedFile& out, std::string& error) {
        for (int i = 0; i < 3000 && !release.load(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
        return real(req, out, error);
    });
    CHECK(passUntil(d.eng, [&] { return !fact(s, "fact.recorder.start").empty(); }));
    CHECK_EQ(fact(s, "fact.recorder.start"), std::string("00"));
    const uint64_t seq0 = s.state().listSeq;
    // An audio start is out: the audio take's character, and not the I/Q take's.
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_RECORD_AUDIO, 0, 1)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return fact(s, "fact.recorder.start") == "01"; }));
    CHECK(s.state().listSeq != seq0);  // hashed: the table moves when it does
    // The I/Q take's too, then withdrawn: it reads 2 while its worker has not answered, and the audio take is untouched.
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_RECORD_IQ, 0, 1)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return fact(s, "fact.recorder.start") == "11"; }));
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_RECORD_IQ, 0, 0)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return fact(s, "fact.recorder.start") == "21"; }));
    // The disk answers: the withdrawn start leaves no take, the audio one is armed, and neither is "out" any more.
    release = true;
    CHECK(passUntil(d.eng, [&] { return fact(s, "fact.recorder.start") == "00"; }, 8000));
    CHECK(passUntil(d.eng, [&] { return count(s, FOXAPI_LIST_RECORDINGS) == 1 && (item(s, FOXAPI_LIST_RECORDINGS, 0).flags & FOXAPI_ITEM_ACTIVE) != 0u; }));
    CHECK_EQ(item(s, FOXAPI_LIST_RECORDINGS, 0).value[0], 2.0);  // the audio take
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_RECORD_AUDIO, 0, 0)).status(), FOXAPI_OK);
}

// Scans, selects `id` and passes until the radio is open.
bool openRadio(cascade::engine::Engine& eng, testing::Session& s, const std::string& id) {
    if (pumped(eng, s, testing::cmd(FOXAPI_OP_SCAN_DEVICES)).status() != FOXAPI_OK) { return false; }
    if (!passUntil(eng, [&] { return count(s, FOXAPI_LIST_DEVICES) == 6; })) { return false; }
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
    CHECK(passUntil(d.eng, [&] { return count(s, FOXAPI_LIST_DEVICES) == 6; }));
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
    engine_api_test::SerialEngine e(api(), kHooks);
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
        const FoxReceiverState before = steadyState(s);  // countersMoved() names audio: no baseline off a dead sink
        CHECK_EQ(setting(s, o.hook), FOXAPI_OK);
        const FoxReceiverState after = s.state();
        std::printf("      %-28s -> [%s]\n", o.what, countersMoved(before, after).c_str());
        CHECK_EQ(countersMoved(before, after), std::string("seq rds"));
        CHECK_EQ(after.rdsSeq, before.rdsSeq + 1u);
    }
    // The same words again move nothing.
    {
        const FoxReceiverState before = steadyState(s);
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
    engine_api_test::SerialEngine e(api(), kHooks);
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
    engine_api_test::SerialEngine e(api(), kHooks);
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
    engine_api_test::SerialEngine e(api(), kHooks);
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
    engine_api_test::SerialEngine e(api(), kHooks);
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

// THE FILE IS A WORKER'S (stub campaign B19): the command's answer is that the write is under way, and the file and the note come a pass or more later.
// Passes (and a millisecond between them, for the worker's own thread) until `done` holds, bounded; false if it never did. Two more passes follow a
// success: the worker's answer is collected in the pass's workers' step, AFTER that pass's publish, so the facts and the lists show it from the next one.
template <class Done>
bool passesUntil(cascade::engine::Engine& eng, Done&& done, int maxMs = 10000) {
    const auto t0 = std::chrono::steady_clock::now();
    bool ok = done();
    while (!ok) {
        eng.controlPass();
        ok = done();
        if (!ok && std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(maxMs)) { return false; }
        if (!ok) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    }
    eng.controlPass();
    eng.controlPass();
    return true;
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
    const std::uint64_t seq0 = Access::exportSeq(d.eng);
    const testing::Outcome o = pumpedText(d.eng, s, testing::cmd(FOXAPP_OP_BOOKMARK_EXPORT), idsText);
    CHECK_EQ(o.status(), FOXAPI_OK);
    // (The answer is that the write is under way; the file and the note are the worker's, collected on a later pass - stub campaign B19.)
    CHECK(passesUntil(d.eng, [&] { return Access::exportSeq(d.eng) > seq0; }));
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
    {
        const std::uint64_t before = Access::exportSeq(d.eng);
        CHECK_EQ(pumpedText(d.eng, s, testing::cmd(FOXAPP_OP_BOOKMARK_EXPORT),
                            "7777777 " + std::to_string(ids["plain"]) + " 8888888").status(),
                 FOXAPI_OK);
        CHECK(passesUntil(d.eng, [&] { return Access::exportSeq(d.eng) > before; }));
    }
    CHECK_EQ(exportedFiles(dir).size(), 1u);
    CHECK(fact(s, "fact.status.bookmarkNote").rfind("Exported 1 to ", 0) == 0);

    // A folder that cannot be made: the note says so, and nothing was written. (The command is OK - the write is under way - and the failure is the worker's
    // answer: the note, and fact.bookmarks.export.ok "0". Until stub campaign B19 the command itself was FAILED.)
    const std::filesystem::path blocker = std::filesystem::path(dir).parent_path() / "export_blocker.txt";
    {
        std::ofstream(blocker) << "a file where a folder would go";
    }
    Access::setRecordDir(d.eng, (blocker / "sub").string());
    {
        const std::uint64_t before = Access::exportSeq(d.eng);
        CHECK_EQ(pumpedText(d.eng, s, testing::cmd(FOXAPP_OP_BOOKMARK_EXPORT), std::to_string(ids["plain"])).status(), FOXAPI_OK);
        CHECK(passesUntil(d.eng, [&] { return Access::exportSeq(d.eng) > before; }));
        CHECK(!Access::lastExportOk(d.eng));
        CHECK_EQ(fact(s, "fact.bookmarks.export.ok"), "0");
        CHECK_EQ(fact(s, "fact.bookmarks.exporting"), "0");
    }
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
    // (ms is the pass's own time: the file is a worker's now, and arrives after it.)
    CHECK(passesUntil(d.eng, [&] { return !Access::exportPending(d.eng); }, 60000));
    // The ceiling tells a pass (~0.1 s) from a search per id (~5 s) in a plain build. Under ThreadSanitizer - which
    // the sanitizer build announces with CASCADE_SANITIZE_THREAD, on everything that links cascade_engine - every
    // load and store is instrumented: the pass measured 0.6-0.8 s on an idle machine (about six times the plain
    // build's 0.1 s) and 2.9 s on a loaded one, over the plain ceiling. The ceiling scales by 4, not by the 10
    // cmake/sanitize.cmake gives that build's ctest TIMEOUTs: 10 s is 3.4 times the loaded pass, and a search per id
    // at the same sixfold slowdown (~30 s) is three times past it, where 25 s would barely tell the two apart. A
    // plain build's ceiling is untouched.
#if defined(CASCADE_SANITIZE_THREAD)
    constexpr double kSlowdown = 4.0;
#else
    constexpr double kSlowdown = 1.0;
#endif
    std::printf("O2: 100 000 ids (%zu bytes) exported in %.0f ms (ceiling %.0f ms)\n", idsText.size(), ms,
                2500.0 * kSlowdown);
    CHECK(ms < 2500.0 * kSlowdown);
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

// A plugin FILE name on this platform: what PluginHost::hasPluginExtension scans for and PluginRepo's
// kModuleExtension admits (".dll" on Windows, ".so" on Linux, ".dylib" on macOS), behind the prefix the build gives
// a module (CMake's shared-library prefix: none on Windows, "lib" elsewhere, as FIXTURE_*_PLUGIN's own names
// have it). The prefix keeps the host's by-file-name order the same on every platform: "group.nine.v2" sorts AFTER
// the group-9 fixture it duplicates as "fixture_..." and as "libfixture_..." alike. On Windows every name comes out
// as it always was.
#if defined(_WIN32)
constexpr const char* kModulePrefix = "";
constexpr const char* kModuleExt = ".dll";
#elif defined(__APPLE__)
constexpr const char* kModulePrefix = "lib";
constexpr const char* kModuleExt = ".dylib";
#else
constexpr const char* kModulePrefix = "lib";
constexpr const char* kModuleExt = ".so";
#endif
std::string moduleFile(const std::string& stem) { return kModulePrefix + stem + kModuleExt; }

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
            std::filesystem::copy_file(FIXTURE_GROUP9_PLUGIN, dir / moduleFile("group.nine.v2"),
                                       std::filesystem::copy_options::overwrite_existing, ec);
            CHECK(!ec);
        }
        std::ofstream junk(dir / moduleFile("zz_refused"), std::ios::binary);
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
    const std::string zz = moduleFile("zz_refused");

    // PLUGINS: the host's order (by file name), the capability word, loaded -> ACTIVE.
    CHECK_EQ(count(s, FOXAPI_LIST_PLUGINS), 3);
    const FoxListItem pg = entryNamed(s, FOXAPI_LIST_PLUGINS, "Group Eight Fixture");
    const FoxListItem ps1 = entryNamed(s, FOXAPI_LIST_PLUGINS, "Stage One Fixture");
    const FoxListItem refused = entryNamed(s, FOXAPI_LIST_PLUGINS, zz);
    CHECK_EQ(std::string(pg.detail), g8);
    CHECK_EQ(std::string(ps1.detail), stage1);
    CHECK_EQ(std::string(refused.detail), zz);
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
    const std::string removed = moduleFile("removed-module");
    CHECK_EQ(count(s, FOXAPI_LIST_STALE_GRANTS), 0);
    CHECK_EQ(pumped(d.eng, s, grantCmd(removed, 1, 1)).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, grantCmd(zz, 1, 1)).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, grantCmd(g8, 1, 1)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK_EQ(count(s, FOXAPI_LIST_STALE_GRANTS), 2);
    const FoxListItem gone = item(s, FOXAPI_LIST_STALE_GRANTS, 0);
    const FoxListItem broken = item(s, FOXAPI_LIST_STALE_GRANTS, 1);
    CHECK_EQ(std::string(gone.name), removed);
    CHECK((gone.flags & FOXAPI_ITEM_REFUSED) == 0u);  // no such file
    CHECK_EQ(std::string(broken.name), zz);
    CHECK((broken.flags & FOXAPI_ITEM_REFUSED) != 0u);  // the file is there and the host refused it
    CHECK_EQ(pumped(d.eng, s, grantCmd(removed, 1, 0)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK_EQ(count(s, FOXAPI_LIST_STALE_GRANTS), 1);
    CHECK_EQ(std::string(item(s, FOXAPI_LIST_STALE_GRANTS, 0).name), zz);

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

// M4 (D3): a decoder line moves decoderLinesTotal and NOT listSeq; the lines still reach the backlog, track-info
// status lines among them; anything else that changes a list still moves listSeq.
void testDecoderLinesLeaveListSeqAlone() {
    cascade::engine::Engine eng;
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
    // they reach the backlog an interface reads (an uncounted line never did). Since the pump move's step P2 the window polls
    // the plugin's text on its own thread and SENDS each line (FOXAPP_OP_TRACK_INFO_LINE); the engine no longer asks a host hook.
    CHECK_EQ(pumped(eng, s, testing::cmd(FOXAPP_OP_TRACK_INFO_LINE, 0, 0, "cannot reach the registry")).status(), FOXAPI_OK);
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
    std::string dotted = moduleFile("group.nine.v2");
    std::string refused = moduleFile("zz_refused");
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
    CHECK_EQ(moduleFact(s, moduleFile("no-such-module"), "version", &st), std::string());
    CHECK_EQ(st, FOXAPI_NOT_FOUND);
    CHECK_EQ(moduleFact(s, n.g9, "colour", &st), std::string());
    CHECK_EQ(st, FOXAPI_NOT_FOUND);
    CHECK_EQ(fact(s, ("fact.plugin." + n.g9).c_str(), &st), std::string());
    CHECK_EQ(st, FOXAPI_NOT_FOUND);
    CHECK_EQ(fact(s, "fact.plugin.dir.path", &st), std::string());
    CHECK_EQ(st, FOXAPI_NOT_FOUND);
    CHECK_EQ(moduleFact(remote, moduleFile("no-such-module"), "path", &st), std::string());
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
    const std::string noSuch = moduleFile("no-such-plugin");
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_STORE_REMOVE, 0, 0, noSuch.c_str())).status(), FOXAPI_OK);
    passes(d.eng, 2);
    const std::string err = fact(s, "fact.status.installError");
    CHECK(err.find("is not an installed plugin file") != std::string::npos);
    CHECK(err.find(noSuch) != std::string::npos);
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
            e.platforms = {hostBuild(moduleFile("alpha_good"), 'a', 12345), IdxPlatform{"plan9", "mips", "alpha_plan9.so", hex64('b'), 99}};
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
            e.platforms = {hostBuild(moduleFile("beta_legal"), 'c', 0)};
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
            e.platforms = {hostBuild(moduleFile("delta_abi"), 'd', 777)};
            v.push_back(e);
        }
        {
            IdxEntry e;  // no licence declared
            e.id = "epsilon-nolicence";
            e.name = "Epsilon Decoder";
            e.version = "1.1.0";
            e.platforms = {hostBuild(moduleFile("epsilon"), 'e', 4096)};
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
            e.platforms = {hostBuild(std::string("sub/") + moduleFile(word), '8', 100)};
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
    CHECK_EQ(storeFact(s, "alpha-good", "file"), moduleFile("alpha_good"));
    CHECK_EQ(storeFact(s, "alpha-good", "updateReason", &st), std::string());  // no plan
    CHECK_EQ(st, 0);
    CHECK_EQ(storeFact(s, "alpha-good", "description"), std::string(400, 'd'));
    CHECK_EQ(storeFact(s, "alpha-good", "blocked"), std::string());
    CHECK_EQ(storeFact(s, "beta.legal", "author", &st), std::string());  // an entry with no maker
    CHECK_EQ(st, 0);
    CHECK_EQ(storeFact(s, "beta.legal", "licence"), std::string("GPL-3.0-or-later"));
    CHECK_EQ(storeFact(s, "beta.legal", "floor"), std::string("0.9.0"));
    CHECK_EQ(storeFact(s, "beta.legal", "platforms"), w.hostPair);
    CHECK_EQ(storeFact(s, "beta.legal", "file"), moduleFile("beta_legal"));
    CHECK_EQ(storeFact(s, "beta.legal", "legal"), std::string(300, 'L'));
    CHECK_EQ(storeFact(s, "beta.legal", "blocked"), std::string());  // the notice taken as acknowledged
    CHECK_EQ(storeFact(s, "gamma-nobuild", "platforms", &st), std::string());  // no build at all
    CHECK_EQ(st, 0);
    CHECK_EQ(storeFact(s, "gamma-nobuild", "file", &st), std::string());
    CHECK_EQ(st, 0);
    CHECK_EQ(storeFact(s, "gamma-nobuild", "blocked"), cascade::gui::pluginNoBuildReason(w.hostPair));
    CHECK_EQ(storeFact(s, "delta-abi", "blocked"), cascade::gui::pluginAbiMismatchReason(abi + 1u, abi));
    CHECK_EQ(storeFact(s, "delta-abi", "file"), moduleFile("delta_abi"));
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
    CHECK_EQ(storeFact(remote, "alpha-good", "file"), moduleFile("alpha_good"));
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

    // A REFETCH that fails (a path that is not there) KEEPS the catalogue on screen and clears the status line, with the reason beside it
    // (0.99.72, restored by stub campaign B3). This section pinned the 0.99.42 rule - a failed refresh emptied the list - which master's
    // 0.99.72 replaced on purpose: a user offline for a moment was shown an empty store. The list that stays is only ever one this client
    // accepted once; install() re-checks every entry it is handed whatever list it came from. (tests/test_plugin_catalogue_app.cpp holds the
    // rule on the kept copy, an earlier read and the sites; tests/test_engine_api_store.cpp on the table.)
    CHECK_EQ(pumped(d.eng, s, textCmd(FOXAPP_OP_SET_CATALOGUE_URL, (w.index.string() + ".missing"))).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_STORE_FETCH)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return count(s, FOXAPI_LIST_STORE_ACTIVITY) == 0; }));
    passes(d.eng, 2);
    CHECK_EQ(count(s, FOXAPI_LIST_CATALOGUE), 7);
    CHECK_EQ(levelOf(s, "catalogStatus"), -1.0);
    CHECK_EQ(levelOf(s, "catalogError"), 3.0);
    CHECK_EQ(storeFact(s, "alpha-good", "author", &st), std::string("Alpha Labs Ltd"));
    CHECK(st > 0);  // the entry's facts stayed with it (served: the text's length, not NOT_FOUND)
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
    // A fetch: the list on screen is NOT emptied when it starts (0.99.72, stub campaign B3) and is replaced when it lands - ONE write.
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_STORE_FETCH)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return count(s, FOXAPI_LIST_CATALOGUE) == 7; }));
    CHECK_EQ(Access::catalogSeq(d.eng), c1 + 1u);
    // ...and one that fails writes nothing: the list stays (a read that did not happen changes nothing a reader sees).
    CHECK_EQ(pumped(d.eng, s, textCmd(FOXAPP_OP_SET_CATALOGUE_URL, (w.index.string() + ".missing"))).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_STORE_FETCH)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return count(s, FOXAPI_LIST_STORE_ACTIVITY) == 0; }));
    passes(d.eng, 2);
    CHECK_EQ(Access::catalogSeq(d.eng), c1 + 1u);
    CHECK_EQ(count(s, FOXAPI_LIST_CATALOGUE), 7);
    std::printf("I5: catalogSeq_ moves at the hook's entry and at a fetch landing, and with the index alone, or a fetch that fails, it does not\n");
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

// <<G11 TESTS BEGIN
// ===================================================================================================================
// GROUP 11: the Source section and the sound card as their pages draw them (window port group 11, API 0.3 section
// 8.10). Hermetic: every walk, scan, diagnostic and sound card is g11::world()'s; the radios are group Z's fakes. The
// passes are driven from the test thread (Direct): no control thread, no race with what the friend pokes.
// ===================================================================================================================

namespace g11 {

using Access = cascade::gui::AppWindowTestAccess;

uint32_t flags(testing::Session& s) { return s.state().sourceFlags; }
bool has(testing::Session& s, uint32_t bit) { return (flags(s) & bit) != 0u; }
// A bit of the state's own flag word (FOXAPI_RX_*), not the source's.
bool rxHas(testing::Session& s, uint32_t bit) { return (s.state().flags & bit) != 0u; }

// The level of a STATUS_LINES entry, -1 when the line has no text.
double lineLevel(testing::Session& s, const char* key) {
    for (int32_t i = 0; i < count(s, FOXAPI_LIST_STATUS_LINES); ++i) {
        const FoxListItem x = item(s, FOXAPI_LIST_STATUS_LINES, static_cast<uint32_t>(i));
        if (std::string(x.name) == key) { return x.value[0]; }
    }
    return -1.0;
}

// The DEVICES entry whose id is `id` (1-based), and the whole id the fact serves for it.
std::string idFact(testing::Session& s, uint32_t id, int32_t* st = nullptr) {
    return fact(s, ("fact.source.id." + std::to_string(id)).c_str(), st);
}

// Applies one command and says whether it landed OK (a command with a long text goes through submit_text).
bool cmdLandsOk(Direct& d, testing::Session& s, const FoxCommand& c) { return pumped(d.eng, s, c).status() == FOXAPI_OK; }
bool cmdLandsOk(Direct& d, testing::Session& s, const cascade::core::cmd::QueuedCommand& q) {
    if (q.longText.empty()) { return cmdLandsOk(d, s, q.c); }
    return pumpedText(d.eng, s, q.c, q.longText).status() == FOXAPI_OK;
}

// A fresh local session and a REMOTE one on `d`.
struct Sessions {
    testing::Session local;
    testing::Session remote;
    explicit Sessions(Direct& d)
        : local(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants),
          remote(api(), d.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_SETTINGS, "s3cret") {}
};

// Scans (native walk inline, SoapySDR on its worker) and passes until the worker's result is in.
bool scanAll(Direct& d, testing::Session& s) {
    if (pumped(d.eng, s, testing::cmd(FOXAPI_OP_SCAN_DEVICES)).status() != FOXAPI_OK) { return false; }
    return passUntil(d.eng, [&] { return !g11::has(s, FOXAPI_SRC_SCAN_PENDING); });
}

}  // namespace g11

// G11-1: DEVICES completed - the IQ file and the sound card are entries, an entry's index is its combo row, ids are
// whole in the fact where detail cuts them (a Pluto's with its address, a SoapySDR row's kwargs), and a cut id picks
// nothing while the fact's picks the radio. fact.source.row follows every kind of row, and ACTIVE is the source in use.
void testSourceRowsAndWholeIds() {
    using Access = cascade::gui::AppWindowTestAccess;
    Access::install11();
    {
        Direct d;
        g11::Sessions ss(d);
        testing::Session& s = ss.local;
        testing::Session& remote = ss.remote;
        CHECK(s.ok());
        CHECK(remote.ok());
        passes(d.eng, 3);

        // BEFORE ANY SCAN: the generator and the two fixed rows, in the combo's order, and nothing else.
        CHECK_EQ(count(s, FOXAPI_LIST_DEVICES), 3);
        const char* const names[3] = {"Signal generator", "IQ file", "Sound card"};
        const char* const ids[3] = {"siggen", "row:iqfile", "row:soundcard"};
        for (uint32_t i = 0; i < 3; ++i) {
            const FoxListItem e = item(s, FOXAPI_LIST_DEVICES, i);
            CHECK_EQ(std::string(e.name), std::string(names[i]));
            CHECK_EQ(std::string(e.detail), std::string(ids[i]));
            CHECK_EQ(e.id, static_cast<uint64_t>(i + 1u));
            CHECK_EQ((e.flags & FOXAPI_ITEM_ACTIVE) != 0u, i == 0u);  // the generator is the source in use
            CHECK_EQ(g11::idFact(s, i + 1u), std::string(ids[i]));
            CHECK_EQ(g11::idFact(remote, i + 1u), std::string(ids[i]));  // an id names no path: not LOCAL
        }
        CHECK_EQ(fact(s, "fact.source.row"), std::string("0"));
        // The id past the last entry, id 0 and a key that is not a number are NOT_FOUND.
        int32_t st = 0;
        g11::idFact(s, 4u, &st);
        CHECK_EQ(st, FOXAPI_NOT_FOUND);
        g11::idFact(s, 0u, &st);
        CHECK_EQ(st, FOXAPI_NOT_FOUND);
        fact(s, "fact.source.id.", &st);
        CHECK_EQ(st, FOXAPI_NOT_FOUND);
        fact(s, "fact.source.id.x", &st);
        CHECK_EQ(st, FOXAPI_NOT_FOUND);
        fact(s, "fact.source", &st);
        CHECK_EQ(st, FOXAPI_NOT_FOUND);

        // THE ADDRESS IN THE PLUTO BOX, 189 bytes: the row built at the next walk carries it, and the id is past a
        // list item's 159 bytes. (The hooked walk lists the row it is told to: the same args a real walk builds.)
        const std::string uri = g11::plutoUriLong();
        CHECK_EQ(pumped(d.eng, s, textCmd(FOXAPP_OP_SET_PLUTO_URI, uri)).status(), FOXAPI_OK);
        CHECK_EQ(fact(s, "fact.source.plutoUri"), uri);
        g11::world().natives[3].args = "uri=" + uri;
        CHECK(g11::scanAll(d, s));
        // ...so DEVICES is: generator, IQ file, sound card, four native rows (the Pluto's last), two SoapySDR rows.
        CHECK_EQ(count(s, FOXAPI_LIST_DEVICES), 9);
        for (uint32_t i = 3; i < 9; ++i) {
            const FoxListItem e = item(s, FOXAPI_LIST_DEVICES, i);
            CHECK_EQ(e.id, static_cast<uint64_t>(i + 1u));
            CHECK_EQ(g11::idFact(s, i + 1u).compare(0, std::strlen(e.detail), e.detail), 0);  // detail is the id, maybe cut
        }
        CHECK_EQ(std::string(item(s, FOXAPI_LIST_DEVICES, 3).detail), std::string(engine_api_test::kFakeRadioId));
        CHECK_EQ(std::string(item(s, FOXAPI_LIST_DEVICES, 4).name), std::string("Airspy R2"));
        CHECK_EQ(std::string(item(s, FOXAPI_LIST_DEVICES, 6).name), std::string("ADALM-Pluto (network)"));
        const std::string plutoId = "pluto:uri=" + uri;
        CHECK(plutoId.size() > 159u);  // the point: detail cannot hold it
        CHECK_EQ(g11::idFact(s, 7u, &st), plutoId);
        CHECK_EQ(st, static_cast<int32_t>(plutoId.size()));  // get_setting says the whole length
        CHECK_EQ(std::strlen(item(s, FOXAPI_LIST_DEVICES, 6).detail), 159u);  // cut
        const std::string soapyId = "soapy:" + g11::soapyLongArgs();
        CHECK(soapyId.size() > 159u);
        CHECK_EQ(g11::idFact(s, 9u), soapyId);
        CHECK_EQ(std::strlen(item(s, FOXAPI_LIST_DEVICES, 8).detail), 159u);
        CHECK_EQ(std::string(item(s, FOXAPI_LIST_DEVICES, 6).detail), plutoId.substr(0, 159));
        // The engine's own spelling is the fact's: Engine::sourceIdForRow(row) is entry row + 1's id, for every row,
        // and "" for a row there is not.
        for (int row = 0; row < 9; ++row) {
            CHECK_EQ(Access::idForRow(d.eng, row), g11::idFact(s, static_cast<uint32_t>(row + 1)));
        }
        CHECK_EQ(Access::idForRow(d.eng, 9), std::string());
        CHECK_EQ(Access::idForRow(d.eng, -1), std::string());
        CHECK_EQ(Access::idForRow(d.eng, 100), std::string());

        // A CUT ID PICKS NOTHING; THE FACT'S PICKS THE RADIO. (The old contract: "an id from DEVICES, verbatim".)
        const std::string cut = item(s, FOXAPI_LIST_DEVICES, 8).detail;
        CHECK_EQ(cut.size(), 159u);
        CHECK_EQ(pumped(d.eng, s, textCmd(FOXAPI_OP_SELECT_SOURCE, cut)).status(), FOXAPI_NOT_FOUND);
        CHECK(!g11::rxHas(s, FOXAPI_RX_DEVICE_OPEN));
        {
            // (FoxCommand::text holds 255 bytes: the whole 280-byte id goes through submit_text.)
            FoxCommand c = testing::cmd(FOXAPI_OP_SELECT_SOURCE);
            const testing::Outcome o = [&] {
                testing::Outcome out;
                FoxSubmitResult sr{};
                sr.structSize = sizeof(sr);
                const int32_t n = api()->submit_text(s.raw(), &c, soapyId.data(), static_cast<uint32_t>(soapyId.size()), &sr);
                out.submitStatus = n < 0 ? n : sr.status;
                for (int pass = 0; pass < 20 && out.submitStatus == FOXAPI_OK && !out.landed; ++pass) {
                    d.eng.controlPass();
                    FoxCommandResult r[4];
                    for (auto& x : r) { x.structSize = sizeof(x); }
                    const int32_t got = api()->poll_results(s.raw(), r, 4);
                    for (int32_t i = 0; i < got; ++i) {
                        if (r[i].ticket == sr.ticket) {
                            out.landed = true;
                            out.result = r[i];
                        }
                    }
                }
                return out;
            }();
            CHECK_EQ(o.status(), FOXAPI_OK);
        }
        CHECK(passUntil(d.eng, [&] { return g11::rxHas(s, FOXAPI_RX_DEVICE_OPEN); }));
        // ACTIVE is the source in use: the SoapySDR row, and only it. fact.source.row is that row's index.
        int at = -1;
        const FoxListItem active = activeItem(s, FOXAPI_LIST_DEVICES, &at);
        CHECK_EQ(at, 8);
        CHECK_EQ(active.id, 9u);
        CHECK_EQ(fact(s, "fact.source.row"), std::string("8"));

        // EVERY KIND OF ROW MOVES fact.source.row, the IQ file and the sound card (which open nothing) and the Pluto
        // (which selects and does not connect) included - and ACTIVE stays on the radio in use through them.
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SELECT_SOURCE, 0, 0, "row:iqfile")).status(), FOXAPI_OK);
        CHECK_EQ(fact(s, "fact.source.row"), std::string("1"));
        CHECK_EQ(fact(remote, "fact.source.row"), std::string("1"));
        CHECK_EQ(activeItem(s, FOXAPI_LIST_DEVICES, &at).id, 9u);  // the radio is still the source in use
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SELECT_SOURCE, 0, 0, "row:soundcard")).status(), FOXAPI_OK);
        CHECK_EQ(fact(s, "fact.source.row"), std::string("2"));
        // The Pluto row selects and does not connect: the row is its own, the radio in use stays.
        CHECK_EQ(pumped(d.eng, s, textCmd(FOXAPI_OP_SELECT_SOURCE, g11::idFact(s, 7u))).status(), FOXAPI_OK);
        CHECK_EQ(fact(s, "fact.source.row"), std::string("6"));  // the Pluto's entry is 7: its row is 6
        CHECK_EQ(activeItem(s, FOXAPI_LIST_DEVICES, &at).id, 9u);
        // The generator: back to row 0, the generator ACTIVE, nothing else.
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SELECT_SOURCE, 0, 0, "siggen")).status(), FOXAPI_OK);
        CHECK_EQ(fact(s, "fact.source.row"), std::string("0"));
        CHECK(!g11::rxHas(s, FOXAPI_RX_DEVICE_OPEN));
        CHECK_EQ(activeItem(s, FOXAPI_LIST_DEVICES, &at).id, 1u);
        // A row that is none (what a rescan that dropped the open radio's row, or a failed restore, leaves).
        Access::sourceSel(d.eng) = -1;
        passes(d.eng, 2);
        CHECK_EQ(fact(s, "fact.source.row"), std::string("-1"));
    }
    Access::uninstall11();
    std::printf("G11-1: DEVICES with the IQ file and the sound card, whole ids (a 199-byte Pluto, a 280-byte SoapySDR row) read from the fact, a cut id refused and the fact's id opening the radio, fact.source.row through every row\n");
}

// G11-2: sourceFlags, each of the ten bits driven both ways. The scan's three states and the gate by the engine's own
// behaviour (a scan held in flight, a deferral beside a radio that is opening, a scan beside the one that is open, the
// whole one that is owed after it), the sound card's three by a backend held on its gates; the two the engine never
// takes back by itself (the card list, UHD) are put back by the friend.
void testSourceFlags() {
    using Access = cascade::gui::AppWindowTestAccess;
    Access::install11();
    g11::World& w = g11::world();
    {
        // --- THE WHOLE SCAN, held in flight: PENDING and SCANNED up, PARTIAL and GATED never.
        Direct d;
        g11::Sessions ss(d);
        testing::Session& s = ss.local;
        passes(d.eng, 3);
        CHECK_EQ(g11::flags(s), 0u);  // nothing at rest
        const FoxReceiverState rest = s.state();
        w.soapyGate = false;
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPP_OP_SCAN_DEVICES_ON_OPEN)).status(), FOXAPI_OK);
        CHECK(g11::has(s, FOXAPI_SRC_SCAN_PENDING));
        CHECK(g11::has(s, FOXAPI_SRC_SCANNED));
        CHECK(!g11::has(s, FOXAPI_SRC_SCAN_PARTIAL));
        CHECK(!g11::has(s, FOXAPI_SRC_SCAN_GATED));
        const FoxReceiverState held = s.state();
        CHECK(held.deviceSeq > rest.deviceSeq);  // the device group moves, and seq with it
        CHECK(held.seq > rest.seq);
        w.soapyGate = true;
        CHECK(passUntil(d.eng, [&] { return !g11::has(s, FOXAPI_SRC_SCAN_PENDING); }));
        CHECK(g11::has(s, FOXAPI_SRC_SCANNED));  // a scan has been started: it stays
        CHECK_EQ(w.soapyCalls.load(), 1);

        // --- THE ENGINE'S OWN SDRPLAY ROW FLAG, both ways, through the walk.
        CHECK(!g11::has(s, FOXAPI_SRC_SDRPLAY_ROWS));
        w.sdrRows = true;
        Access::scanNative(d.eng);  // the native walk alone (the engine's own call, as the window's run() makes it)
        passes(d.eng, 2);
        CHECK(g11::has(s, FOXAPI_SRC_SDRPLAY_ROWS));
        w.sdrRows = false;
        Access::scanNative(d.eng);  // the native walk alone (the engine's own call, as the window's run() makes it)
        passes(d.eng, 2);
        CHECK(!g11::has(s, FOXAPI_SRC_SDRPLAY_ROWS));

        // --- "LOOK FOR NETWORK USRPs", both ways (ticking it rescans SoapySDR).
        CHECK(!g11::has(s, FOXAPI_SRC_NETWORK_USRPS));
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPP_OP_SET_NETWORK_USRP_SCAN, 0, 1)).status(), FOXAPI_OK);
        CHECK(g11::has(s, FOXAPI_SRC_NETWORK_USRPS));
        CHECK(passUntil(d.eng, [&] { return !g11::has(s, FOXAPI_SRC_SCAN_PENDING); }));
        CHECK_EQ(w.soapyCalls.load(), 2);  // the tick rescanned
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPP_OP_SET_NETWORK_USRP_SCAN, 0, 0)).status(), FOXAPI_OK);
        CHECK(!g11::has(s, FOXAPI_SRC_NETWORK_USRPS));
        CHECK_EQ(w.soapyCalls.load(), 2);  // unticking does not

        // --- UHD LEFT OUT, both ways: the scan's own list of drivers it did not ask (a hermetic scan has none, so the
        // friend writes it, as the worker does), and any name but uhd does not set it.
        CHECK(!g11::has(s, FOXAPI_SRC_UHD_LEFT_OUT));
        Access::absent(d.eng) = {"rtlsdr"};
        passes(d.eng, 2);
        CHECK(!g11::has(s, FOXAPI_SRC_UHD_LEFT_OUT));
        Access::absent(d.eng) = {"uhd"};
        passes(d.eng, 2);
        CHECK(g11::has(s, FOXAPI_SRC_UHD_LEFT_OUT));
        Access::absent(d.eng).clear();
        passes(d.eng, 2);
        CHECK(!g11::has(s, FOXAPI_SRC_UHD_LEFT_OUT));
    }
    {
        // --- A RADIO OPENING: GATED, and the scan it defers leaves SCANNED as it was (still owed); one log line.
        w.soapyCalls = 0;
        Direct d;
        g11::Sessions ss(d);
        testing::Session& s = ss.local;
        Access::scanNative(d.eng);  // the native walk alone (the engine's own call, as the window's run() makes it)
        passes(d.eng, 2);
        const std::string airspy = g11::idFact(s, 5u);
        CHECK_EQ(airspy.compare(0, 7, "airspy:"), 0);
        radios83::g_gateOpen = false;
        CHECK_EQ(pumped(d.eng, s, textCmd(FOXAPI_OP_SELECT_SOURCE, airspy)).status(), FOXAPI_OK);
        CHECK(g11::rxHas(s, FOXAPI_RX_DEVICE_OPENING));
        CHECK(g11::has(s, FOXAPI_SRC_SCAN_GATED));
        CHECK_EQ(fact(s, "fact.source.opening"), std::string("Airspy R2"));
        CHECK_EQ(fact(s, "fact.source.scanGate"), std::string("Airspy R2"));
        CHECK_EQ(fact(s, "fact.source.scanSkip"), std::string());  // still opening: the whole SoapySDR scan is deferred
        CHECK(!Access::deferredLogged(d.eng));
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SCAN_DEVICES)).status(), FOXAPI_OK);
        CHECK_EQ(w.soapyCalls.load(), 0);  // deferred: the worker never ran
        CHECK(!g11::has(s, FOXAPI_SRC_SCANNED));
        CHECK(!g11::has(s, FOXAPI_SRC_SCAN_PENDING));
        CHECK(!g11::has(s, FOXAPI_SRC_SCAN_PARTIAL));
        CHECK(Access::deferredLogged(d.eng));  // the one line was said, for this radio
        // ...the radio is up: still gated (a radio is open), the facts name its model, and the scan runs beside it,
        // leaving its driver out: SCANNED and PARTIAL, and the engine lets the log latch go only when the gate opens.
        radios83::g_gateOpen = true;
        CHECK(passUntil(d.eng, [&] { return g11::rxHas(s, FOXAPI_RX_DEVICE_OPEN); }));
        CHECK(!g11::rxHas(s, FOXAPI_RX_DEVICE_OPENING));
        CHECK(g11::has(s, FOXAPI_SRC_SCAN_GATED));
        CHECK_EQ(fact(s, "fact.source.opening"), std::string());
        CHECK(!fact(s, "fact.source.scanGate").empty());
        CHECK_EQ(fact(s, "fact.source.scanGate"), Access::gateDevice(d.eng));
        CHECK_EQ(fact(s, "fact.source.scanSkip"), std::string("airspy"));  // Refresh still runs, without airspy
        passes(d.eng, 5);
        CHECK(Access::deferredLogged(d.eng));  // gated all the while: the latch holds
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SCAN_DEVICES)).status(), FOXAPI_OK);
        CHECK(g11::has(s, FOXAPI_SRC_SCANNED));
        CHECK(g11::has(s, FOXAPI_SRC_SCAN_PARTIAL));
        CHECK(passUntil(d.eng, [&] { return !g11::has(s, FOXAPI_SRC_SCAN_PENDING); }));
        CHECK(g11::has(s, FOXAPI_SRC_SCAN_PARTIAL));  // it was a partial one: a whole scan is still owed
        CHECK_EQ(w.soapyCalls.load(), 1);
        // The radio closed: the gate opens, the engine's pass lets the latch go (NO command says so), and the next
        // look at the combo owes - and runs - the whole scan.
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SELECT_SOURCE, 0, 0, "siggen")).status(), FOXAPI_OK);
        passes(d.eng, 1);
        CHECK(!g11::has(s, FOXAPI_SRC_SCAN_GATED));
        CHECK(!Access::deferredLogged(d.eng));
        CHECK_EQ(fact(s, "fact.source.scanGate"), std::string());
        CHECK_EQ(fact(s, "fact.source.scanSkip"), std::string());
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPP_OP_SCAN_DEVICES_ON_OPEN)).status(), FOXAPI_OK);
        CHECK(passUntil(d.eng, [&] { return !g11::has(s, FOXAPI_SRC_SCAN_PENDING); }));
        CHECK(!g11::has(s, FOXAPI_SRC_SCAN_PARTIAL));
        CHECK(g11::has(s, FOXAPI_SRC_SCANNED));
        CHECK_EQ(w.soapyCalls.load(), 2);
    }
    {
        // --- THE SOUND CARD: SCANNING while the list is taken, LISTED when it lands, OPENING while the open runs.
        Direct d;
        g11::Sessions ss(d);
        testing::Session& s = ss.local;
        passes(d.eng, 2);
        CHECK(!g11::has(s, FOXAPI_SRC_CARD_LISTED) && !g11::has(s, FOXAPI_SRC_CARD_SCANNING) &&
              !g11::has(s, FOXAPI_SRC_CARD_OPENING));
        w.listGate = false;
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SELECT_SOURCE, 0, 0, "row:soundcard")).status(), FOXAPI_OK);
        CHECK(g11::has(s, FOXAPI_SRC_CARD_SCANNING));
        CHECK(!g11::has(s, FOXAPI_SRC_CARD_LISTED));
        w.listGate = true;
        CHECK(passUntil(d.eng, [&] { return g11::has(s, FOXAPI_SRC_CARD_LISTED); }));
        CHECK(!g11::has(s, FOXAPI_SRC_CARD_SCANNING));
        // The engine never un-lists: the friend does, and the flag follows. With the row selected an un-listed engine
        // takes the list again on its next pass (pumpSourceHousekeeping), and a worker that lands before the second
        // pass's poll would list it back under the check - it did, one run in three under ThreadSanitizer - so the
        // list's gate is held shut while the friend moves the flag, and that re-list lands only once it is let go.
        w.listGate = false;
        Access::cardListed(d.eng) = false;
        passes(d.eng, 2);
        CHECK(!g11::has(s, FOXAPI_SRC_CARD_LISTED));
        Access::cardListed(d.eng) = true;
        passes(d.eng, 2);
        CHECK(g11::has(s, FOXAPI_SRC_CARD_LISTED));
        w.listGate = true;
        CHECK(passUntil(d.eng, [&] { return !g11::has(s, FOXAPI_SRC_CARD_SCANNING); }));
        CHECK(g11::has(s, FOXAPI_SRC_CARD_LISTED));
        // OPENING: the open of the form's card held on its gate, then let go.
        w.openGate = false;
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SELECT_SOURCE, 0, 0, "soundcard:open")).status(), FOXAPI_OK);
        CHECK(g11::has(s, FOXAPI_SRC_CARD_OPENING));
        CHECK(!g11::rxHas(s, FOXAPI_RX_DEVICE_OPENING));  // not the radio's flag
        w.openGate = true;
        CHECK(passUntil(d.eng, [&] { return !g11::has(s, FOXAPI_SRC_CARD_OPENING); }));
        CHECK_EQ(Access::sourceKind(d.eng), std::string("soundcard"));
    }
    Access::uninstall11();
    std::printf("G11-2: sourceFlags, ten bits both ways; the deferral leaves SCANNED clear, a scan beside the radio is PARTIAL, the engine lets the log latch go and owes the whole scan\n");
}

// G11-3: the facts of the section, every edge: known and empty at rest, whole texts, LOCAL, NOT_FOUND.
void testSourceFacts() {
    using Access = cascade::gui::AppWindowTestAccess;
    Access::install11();
    {
        Direct d;
        g11::Sessions ss(d);
        testing::Session& s = ss.local;
        testing::Session& remote = ss.remote;
        passes(d.eng, 3);
        int32_t st = 0;
        // AT REST every text fact is known and empty (length 0, not NOT_FOUND); the row is the generator's.
        for (const char* key : {"fact.source.keep", "fact.source.opening", "fact.source.scanGate", "fact.source.scanSkip",
                                "fact.source.faultedDrivers", "fact.soundcard.device", "fact.soundcard.hostApi"}) {
            CHECK_EQ(fact(s, key, &st), std::string());
            CHECK_EQ(st, 0);
        }
        CHECK_EQ(fact(s, "fact.source.row"), std::string("0"));
        CHECK_EQ(fact(s, "fact.source.plutoUri"), std::string("ip:192.168.2.1"));  // the box's default
        // LOCAL: the Pluto's address and the sound card's device and host API are DENIED to a REMOTE session; every
        // other fact of the section is readable (it names no path).
        for (const char* key : {"fact.source.plutoUri", "fact.soundcard.device", "fact.soundcard.hostApi"}) {
            CHECK_EQ(fact(remote, key, &st), std::string());
            CHECK_EQ(st, FOXAPI_DENIED);
        }
        for (const char* key : {"fact.source.row", "fact.source.keep", "fact.source.opening", "fact.source.scanGate",
                                "fact.source.scanSkip", "fact.source.faultedDrivers", "fact.soundcard.span"}) {
            fact(remote, key, &st);
            CHECK(st >= 0);
        }
        // Facts are read-only: no SETTING_SET takes one.
        CHECK(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SETTING_SET, 0, 0, "fact.source.row=5")).status() != FOXAPI_OK);
        CHECK_EQ(fact(s, "fact.source.row"), std::string("0"));
        // Keys of the family that are not facts.
        for (const char* key : {"fact.source.nothing", "fact.source.id", "fact.soundcard", "fact.soundcard.rate",
                                "fact.source.row.x", "fact.source."}) {
            fact(s, key, &st);
            CHECK_EQ(st, FOXAPI_NOT_FOUND);
        }

        // THE KIND KEPT FOR THE CONFIG: any text the engine holds, "" again when it lets go; both sessions read it.
        for (const char* kind : {"soundcard", "file", "soapy", "airspy"}) {
            Access::keepKind(d.eng) = kind;
            passes(d.eng, 2);
            CHECK_EQ(fact(s, "fact.source.keep"), std::string(kind));
            CHECK_EQ(fact(remote, "fact.source.keep"), std::string(kind));
        }
        Access::keepKind(d.eng).clear();
        passes(d.eng, 2);
        CHECK_EQ(fact(s, "fact.source.keep"), std::string());

        // THE PLUTO'S ADDRESS: stored as typed, at most the box's 191 bytes, and a repeat of it moves no list.
        CHECK_EQ(pumped(d.eng, s, textCmd(FOXAPP_OP_SET_PLUTO_URI, "ip:10.0.0.7")).status(), FOXAPI_OK);
        CHECK_EQ(fact(s, "fact.source.plutoUri"), std::string("ip:10.0.0.7"));
        const uint64_t seq = s.state().listSeq;
        CHECK_EQ(pumped(d.eng, s, textCmd(FOXAPP_OP_SET_PLUTO_URI, "ip:10.0.0.7")).status(), FOXAPI_OK);
        passes(d.eng, 2);
        CHECK_EQ(s.state().listSeq, seq);
        CHECK_EQ(pumped(d.eng, s, textCmd(FOXAPP_OP_SET_PLUTO_URI, std::string(250, 'q'))).status(), FOXAPI_OK);
        CHECK_EQ(fact(s, "fact.source.plutoUri", &st), std::string(191, 'q'));
        CHECK_EQ(st, 191);

        // THE DRIVERS WHOSE SCAN CHILD DIED, taken when a scan lands: joined, whole, cleared by a landing without
        // them. The hook stands in for the process's session list (nothing here ever kills a child).
        g11::world().faulted = {"uhd", "sdrplay"};
        CHECK(g11::scanAll(d, s));
        CHECK_EQ(fact(s, "fact.source.faultedDrivers"), std::string("uhd, sdrplay"));
        CHECK_EQ(fact(remote, "fact.source.faultedDrivers"), std::string("uhd, sdrplay"));
        g11::world().faulted.clear();
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SCAN_DEVICES)).status(), FOXAPI_OK);
        CHECK(passUntil(d.eng, [&] { return !g11::has(s, FOXAPI_SRC_SCAN_PENDING); }));
        CHECK_EQ(fact(s, "fact.source.faultedDrivers"), std::string());
    }
    {
        // WITH THE SCAN STOOD IN FOR AND NO DIAGNOSTICS HOOK, the engine reads none of the four: no faulted drivers, no
        // evidence, and the process's environment untouched (the real calls' first act is editing SOAPY_SDR_PLUGIN_PATH).
        Access::install11(false);
        const char* before = std::getenv("SOAPY_SDR_PLUGIN_PATH");
        const std::string was = before != nullptr ? before : "";
        const bool had = before != nullptr;
        g11::world().faulted = {"uhd"};
        g11::world().modules = {"never read"};
        g11::world().soapy.clear();  // nothing found: the landing the real diagnostics would be read at
        Direct d;
        g11::Sessions ss(d);
        testing::Session& s = ss.local;
        CHECK(g11::scanAll(d, s));
        CHECK_EQ(fact(s, "fact.source.faultedDrivers"), std::string());
        CHECK_EQ(count(s, FOXAPI_LIST_SCAN_EVIDENCE), 0);
        CHECK_EQ(g11::world().diagCalls.load(), 0);
        const char* after = std::getenv("SOAPY_SDR_PLUGIN_PATH");
        CHECK_EQ(after != nullptr, had);
        CHECK_EQ(std::string(after != nullptr ? after : ""), was);
    }
    Access::uninstall11();
    std::printf("G11-3: fact.source.{row,keep,opening,scanGate,scanSkip,faultedDrivers,plutoUri}: known and empty at rest, whole, LOCAL where a path or an address, NOT_FOUND edges; a hermetic scan reads no runtime diagnostics\n");
}

// G11-4: the radio's fault sentence whole. The zombie-source sentence (pipeline.cpp, "source thread: ...") is 134 bytes;
// the state's faultMessage holds 127 of them and so loses the words that tell an interface not to add its generic advice.
void testDeviceFaultWhole() {
    using Access = cascade::gui::AppWindowTestAccess;
    Access::install11();
    {
        Direct d;
        g11::Sessions ss(d);
        testing::Session& s = ss.local;
        testing::Session& remote = ss.remote;
        passes(d.eng, 3);
        int32_t st = 0;
        CHECK_EQ(g11::lineLevel(s, "deviceFault"), -1.0);  // no text: not listed
        CHECK_EQ(fact(s, "fact.status.deviceFault", &st), std::string());
        CHECK_EQ(st, 0);  // a known line, empty
        CHECK_EQ(d.eng.statusText().deviceFault, std::string());
        const std::string zombie =
            "source thread: the radio's driver is holding an abandoned thread and this source cannot restart; "
            "pick another source or restart FoxSDR";
        CHECK_EQ(zombie.size(), 134u);
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SETTING_SET, 0, 0, ("test.fault=" + zombie).c_str())).status(), FOXAPI_OK);
        const FoxReceiverState state = s.state();
        CHECK((state.flags & FOXAPI_RX_FAULTED) != 0u);
        // THE STATE'S FIELD CUTS IT, at 127 bytes, before its last words...
        CHECK_EQ(std::strlen(state.faultMessage), 127u);
        CHECK_EQ(zombie.compare(0, 127, state.faultMessage), 0);
        CHECK(std::string(state.faultMessage).find("restart FoxSDR") == std::string::npos);
        // ...so an interface that searched it for them would add "Reconnect it and pick the source again" under a
        // sentence that says the opposite (the bug the whole line exists to close):
        CHECK(cascade::gui::reconnectAdviceApplies(state.faultMessage));
        // THE LINE DOES NOT: whole in the list item (134 < 159), in the fact and in the engine's handed-over copy.
        CHECK_EQ(g11::lineLevel(s, "deviceFault"), 3.0);
        const FoxListItem line = entryNamed(s, FOXAPI_LIST_STATUS_LINES, "deviceFault");
        CHECK_EQ(std::string(line.detail), zombie);
        CHECK_EQ(fact(s, "fact.status.deviceFault", &st), zombie);
        CHECK_EQ(st, 134);
        CHECK_EQ(fact(remote, "fact.status.deviceFault"), zombie);  // a sentence, not a path
        CHECK_EQ(d.eng.statusText().deviceFault, zombie);
        CHECK(!cascade::gui::reconnectAdviceApplies(fact(s, "fact.status.deviceFault")));
        CHECK(!cascade::gui::reconnectAdviceApplies(d.eng.statusText().deviceFault));
        // A sentence past a list item as well (a driver's own what(), of any length): detail cuts at 159 bytes, the
        // fact is whole - over submit_text, since text[] holds 255.
        const std::string longFault = "device stopped: " + std::string(400, 'z') + " - then it said: restart FoxSDR";
        CHECK_EQ(pumpedText(d.eng, s, testing::cmd(FOXAPI_OP_SETTING_SET), "test.fault=" + longFault).result.status, FOXAPI_OK);
        const FoxListItem cutLine = entryNamed(s, FOXAPI_LIST_STATUS_LINES, "deviceFault");
        CHECK_EQ(std::strlen(cutLine.detail), 159u);
        CHECK_EQ(fact(s, "fact.status.deviceFault"), longFault);
        CHECK_EQ(d.eng.statusText().deviceFault, longFault);
        CHECK_EQ(std::strlen(s.state().faultMessage), 127u);
        // The same sentence said again moves no list; it going moves the line.
        const uint64_t seq = s.state().listSeq;
        CHECK_EQ(pumpedText(d.eng, s, testing::cmd(FOXAPI_OP_SETTING_SET), "test.fault=" + longFault).result.status, FOXAPI_OK);
        passes(d.eng, 2);
        CHECK_EQ(s.state().listSeq, seq);
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SETTING_SET, 0, 0, "test.fault=")).status(), FOXAPI_OK);
        CHECK_EQ(g11::lineLevel(s, "deviceFault"), -1.0);
        CHECK_EQ(fact(s, "fact.status.deviceFault"), std::string());
        CHECK_EQ(d.eng.statusText().deviceFault, std::string());
        CHECK((s.state().flags & FOXAPI_RX_FAULTED) == 0u);
    }
    Access::uninstall11();
    std::printf("G11-4: deviceFault: the 134-byte zombie sentence whole in the line, the fact and the handed-over copy, cut at 127 in the state (and the generic advice then wrongly applies)\n");
}

// G11-5: SAMPLE_RATES' detail is the caption the Rate combo letters; the nearest row is derived from sampleRateHz.
void testSampleRateCaptions() {
    using Access = cascade::gui::AppWindowTestAccess;
    Access::install11();
    {
        Direct d;
        g11::Sessions ss(d);
        testing::Session& s = ss.local;
        CHECK(g11::scanAll(d, s));
        CHECK_EQ(count(s, FOXAPI_LIST_SAMPLE_RATES), 0);  // no radio: no rates
        CHECK_EQ(pumped(d.eng, s, textCmd(FOXAPI_OP_SELECT_SOURCE, engine_api_test::kFakeRadioId)).status(), FOXAPI_OK);
        CHECK(passUntil(d.eng, [&] { return g11::rxHas(s, FOXAPI_RX_DEVICE_OPEN); }));
        const int32_t n = count(s, FOXAPI_LIST_SAMPLE_RATES);
        CHECK_EQ(n, 6);
        const char* const captions[6] = {"0.250 MS/s", "1.024 MS/s", "1.400 MS/s", "2.048 MS/s", "2.400 MS/s", "3.200 MS/s"};
        for (int32_t i = 0; i < n; ++i) {
            const FoxListItem r = item(s, FOXAPI_LIST_SAMPLE_RATES, static_cast<uint32_t>(i));
            CHECK_EQ(std::string(r.detail), std::string(captions[i]));
            CHECK_EQ(std::string(r.detail), cascade::gui::rateLabel(r.value[0]));
        }
        // THE NEAREST ROW from the state's rate. The radio coerces a rate it does not offer, and what it lands on is
        // what the combo ticks: SET_SAMPLE_RATE 2.5 MS/s lands on 2.4 (the fake's nearest).
        const auto nearestRow = [&] {
            const double hz = s.state().sampleRateHz;
            int best = -1;
            double bestDistance = 1.0e30;
            for (int32_t i = 0; i < count(s, FOXAPI_LIST_SAMPLE_RATES); ++i) {
                const double dist = std::fabs(item(s, FOXAPI_LIST_SAMPLE_RATES, static_cast<uint32_t>(i)).value[0] - hz);
                if (dist < bestDistance) {
                    bestDistance = dist;
                    best = i;
                }
            }
            return best;
        };
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SET_SAMPLE_RATE, 2.5e6)).status(), FOXAPI_OK);
        CHECK_EQ(s.state().sampleRateHz, 2.4e6);
        CHECK_EQ(nearestRow(), 4);
        CHECK_EQ(std::string(item(s, FOXAPI_LIST_SAMPLE_RATES, static_cast<uint32_t>(nearestRow())).detail), std::string("2.400 MS/s"));
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SET_SAMPLE_RATE, 250000.0)).status(), FOXAPI_OK);
        CHECK_EQ(nearestRow(), 0);
    }
    {
        // AN AIRSPY's rates are captioned by its decimation, not as plain rates: the label the engine built.
        Direct d;
        g11::Sessions ss(d);
        testing::Session& s = ss.local;
        CHECK(g11::scanAll(d, s));
        CHECK_EQ(pumped(d.eng, s, textCmd(FOXAPI_OP_SELECT_SOURCE, g11::idFact(s, 5u))).status(), FOXAPI_OK);
        CHECK(passUntil(d.eng, [&] { return g11::rxHas(s, FOXAPI_RX_DEVICE_OPEN); }));
        const int32_t n = count(s, FOXAPI_LIST_SAMPLE_RATES);
        CHECK(n >= 2);
        for (int32_t i = 0; i < n; ++i) {
            const FoxListItem r = item(s, FOXAPI_LIST_SAMPLE_RATES, static_cast<uint32_t>(i));
            CHECK_EQ(std::string(r.detail), cascade::gui::airspyRateLabel(r.value[0]));
        }
        // The captions follow the rates: a deep decimation brings the rates under 1 MS/s, which an Airspy's captions
        // letter in kS/s and the plain ones round into MS/s - so the block is rebuilt with other texts.
        const uint64_t seq = s.state().listSeq;
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPP_OP_AIRSPY_DECIMATION, 0, 32)).status(), FOXAPI_OK);
        passes(d.eng, 2);
        CHECK(s.state().listSeq != seq);
        bool differs = false;
        for (int32_t i = 0; i < count(s, FOXAPI_LIST_SAMPLE_RATES); ++i) {
            const FoxListItem r = item(s, FOXAPI_LIST_SAMPLE_RATES, static_cast<uint32_t>(i));
            CHECK_EQ(std::string(r.detail), cascade::gui::airspyRateLabel(r.value[0]));
            differs = differs || std::string(r.detail) != cascade::gui::rateLabel(r.value[0]);
        }
        CHECK(differs);  // the Airspy's captions are not the plain ones
    }
    Access::uninstall11();
    std::printf("G11-5: SAMPLE_RATES detail is the combo's caption (plain and an Airspy's), the nearest row follows sampleRateHz\n");
}

// G11-6: UNBOUND_RADIOS and the SDRplay note, through the walk's hook (no USB walk, no SDRplay install).
void testUnboundRadiosAndSdrPlayNote() {
    using Access = cascade::gui::AppWindowTestAccess;
    Access::install11();
    {
        Direct d;
        g11::Sessions ss(d);
        testing::Session& s = ss.local;
        testing::Session& remote = ss.remote;
        passes(d.eng, 3);
        CHECK_EQ(count(s, FOXAPI_LIST_UNBOUND_RADIOS), 0);
        CHECK_EQ(count(remote, FOXAPI_LIST_UNBOUND_RADIOS), 0);
        const cascade::usb::UsbId rtl = cascade::source::rtlSdrUsbIds().front();
        const cascade::usb::UsbId rx = cascade::source::rx888UsbIds().front();
        const auto dev = [](std::uint16_t vid, std::uint16_t pid, const char* description) {
            cascade::usb::UsbDeviceInfo u;
            u.vid = vid;
            u.pid = pid;
            u.path = "\\\\?\\usb#fake";
            u.serial = "SECRETSERIAL";
            u.description = description;
            return u;
        };
        g11::world().unbound = {dev(rtl.vid, rtl.pid, "Bulk-In, Interface"), dev(rx.vid, rx.pid, ""),
                                dev(0x1D50, 0x6089, "HackRF One")};
        g11::world().sdrAdvice = "No SDRplay API found: install version 3.x from sdrplay.com.";
        g11::world().sdrDetail = "looked in C:/Fake/sdrplay_api.dll";
        const uint64_t seq0 = s.state().listSeq;
        Access::scanNative(d.eng);  // the native walk alone (the engine's own call, as the window's run() makes it)
        passes(d.eng, 2);
        CHECK(s.state().listSeq != seq0);
        CHECK_EQ(count(s, FOXAPI_LIST_UNBOUND_RADIOS), 3);
        const FoxListItem a = item(s, FOXAPI_LIST_UNBOUND_RADIOS, 0);
        const FoxListItem b = item(s, FOXAPI_LIST_UNBOUND_RADIOS, 1);
        const FoxListItem c = item(s, FOXAPI_LIST_UNBOUND_RADIOS, 2);
        CHECK_EQ(std::string(a.name), std::string("Bulk-In, Interface"));
        CHECK_EQ(std::string(a.detail), std::string("rtl"));
        CHECK_EQ(a.value[0], static_cast<double>(rtl.vid));
        CHECK_EQ(a.value[1], static_cast<double>(rtl.pid));
        CHECK_EQ(std::string(b.name), std::string());  // no description: the interface says "A USB radio"
        CHECK_EQ(std::string(b.detail), std::string("rx888"));
        CHECK_EQ(b.value[0], static_cast<double>(rx.vid));
        CHECK_EQ(b.value[1], static_cast<double>(rx.pid));
        CHECK_EQ(std::string(c.name), std::string("HackRF One"));
        CHECK_EQ(std::string(c.detail), std::string());  // any other radio
        CHECK_EQ(c.value[0], static_cast<double>(0x1D50));
        CHECK_EQ(c.value[1], static_cast<double>(0x6089));
        // Not paths: a REMOTE session reads the same, and no serial or bus path is in any field of any entry.
        CHECK_EQ(count(remote, FOXAPI_LIST_UNBOUND_RADIOS), 3);
        CHECK_EQ(std::string(item(remote, FOXAPI_LIST_UNBOUND_RADIOS, 0).name), std::string("Bulk-In, Interface"));
        for (uint32_t i = 0; i < 3; ++i) {
            const FoxListItem x = item(s, FOXAPI_LIST_UNBOUND_RADIOS, i);
            CHECK(std::string(x.name).find("SECRET") == std::string::npos && std::string(x.detail).find("usb#") == std::string::npos);
        }
        // THE SDRPLAY NOTE: two status lines (warnings) with the driver's own words, and the row flag clear.
        CHECK_EQ(g11::lineLevel(s, "sdrPlayAdvice"), 2.0);
        CHECK_EQ(g11::lineLevel(s, "sdrPlayApiDetail"), 2.0);
        CHECK_EQ(fact(s, "fact.status.sdrPlayAdvice"), g11::world().sdrAdvice);
        CHECK_EQ(fact(s, "fact.status.sdrPlayApiDetail"), g11::world().sdrDetail);
        CHECK_EQ(d.eng.statusText().sdrPlayAdvice, g11::world().sdrAdvice);
        CHECK_EQ(d.eng.statusText().sdrPlayApiDetail, g11::world().sdrDetail);
        CHECK(!g11::has(s, FOXAPI_SRC_SDRPLAY_ROWS));
        // The same walk again changes nothing at all (listSeq exact), and a walk that finds an API row sets the flag and
        // - the advice being the driver's to give or not - the lines follow the hook.
        const uint64_t seq1 = s.state().listSeq;
        Access::scanNative(d.eng);  // the native walk alone (the engine's own call, as the window's run() makes it)
        passes(d.eng, 2);
        passes(d.eng, 2);
        CHECK_EQ(s.state().listSeq, seq1);
        g11::world().sdrRows = true;
        g11::world().sdrAdvice.clear();
        g11::world().sdrDetail.clear();
        Access::scanNative(d.eng);  // the native walk alone (the engine's own call, as the window's run() makes it)
        passes(d.eng, 2);
        CHECK(g11::has(s, FOXAPI_SRC_SDRPLAY_ROWS));
        CHECK_EQ(g11::lineLevel(s, "sdrPlayAdvice"), -1.0);
        CHECK(s.state().listSeq != seq1);
        // A dongle bound at last: the next walk's list is shorter, and the entry goes.
        g11::world().unbound.erase(g11::world().unbound.begin());
        Access::scanNative(d.eng);  // the native walk alone (the engine's own call, as the window's run() makes it)
        passes(d.eng, 2);
        CHECK_EQ(count(s, FOXAPI_LIST_UNBOUND_RADIOS), 2);
        CHECK_EQ(std::string(item(s, FOXAPI_LIST_UNBOUND_RADIOS, 0).detail), std::string("rx888"));
        g11::world().unbound.clear();
        Access::scanNative(d.eng);  // the native walk alone (the engine's own call, as the window's run() makes it)
        passes(d.eng, 2);
        CHECK_EQ(count(s, FOXAPI_LIST_UNBOUND_RADIOS), 0);
    }
    Access::uninstall11();
    std::printf("G11-6: UNBOUND_RADIOS (families rtl/rx888/other, ids, no description, no path or serial) and the SDRplay note through the walk's hook; a repeated walk moves nothing\n");
}

// G11-7: SCAN_EVIDENCE - taken once when a whole scan lands empty, LOCAL, cleared by anything else, and moving nothing
// when a landing says the same thing again.
void testScanEvidence() {
    using Access = cascade::gui::AppWindowTestAccess;
    Access::install11();
    {
        Direct d;
        g11::Sessions ss(d);
        testing::Session& s = ss.local;
        testing::Session& remote = ss.remote;
        g11::World& w = g11::world();
        passes(d.eng, 3);
        CHECK_EQ(count(s, FOXAPI_LIST_SCAN_EVIDENCE), 0);
        CHECK_EQ(count(remote, FOXAPI_LIST_SCAN_EVIDENCE), FOXAPI_DENIED);
        // A scan that finds a radio takes no evidence (the diagnostics are asked of the hook at every landing for the
        // faulted drivers, and the evidence is the empty landing's alone).
        CHECK(g11::scanAll(d, s));
        CHECK_EQ(w.diagCalls.load(), 1);
        CHECK_EQ(count(s, FOXAPI_LIST_SCAN_EVIDENCE), 0);
        // NOTHING FOUND: the evidence, in the order the section's tree draws it, kinds 1, 2 and 3.
        w.soapy.clear();
        w.vendors = {{"PothosSDR", "C:/Fake/PothosSDR", "C:/Fake/PothosSDR/lib/SoapySDR/modules0.8", "C:/Fake/PothosSDR/bin"},
                     {"radioconda", "C:/Fake/radioconda", "", ""}};
        w.searchPaths = {"C:/Fake/modules-a", "C:/Fake/modules-b", "C:/Fake/" + std::string(200, 's')};
        w.modules = {"C:/Fake/modules-a/rtlsdrSupport.dll"};
        CHECK(g11::scanAll(d, s));
        CHECK_EQ(w.diagCalls.load(), 2);
        CHECK_EQ(count(s, FOXAPI_LIST_SCAN_EVIDENCE), 6);
        const FoxListItem v0 = item(s, FOXAPI_LIST_SCAN_EVIDENCE, 0);
        const FoxListItem v1 = item(s, FOXAPI_LIST_SCAN_EVIDENCE, 1);
        const FoxListItem p0 = item(s, FOXAPI_LIST_SCAN_EVIDENCE, 2);
        const FoxListItem p2 = item(s, FOXAPI_LIST_SCAN_EVIDENCE, 4);
        const FoxListItem m0 = item(s, FOXAPI_LIST_SCAN_EVIDENCE, 5);
        CHECK_EQ(v0.value[0], 1.0);
        CHECK_EQ(std::string(v0.name), std::string("PothosSDR"));
        CHECK_EQ(std::string(v0.detail), std::string("C:/Fake/PothosSDR"));
        CHECK_EQ(std::string(v1.name), std::string("radioconda"));
        CHECK_EQ(p0.value[0], 2.0);
        CHECK_EQ(std::string(p0.name), std::string());
        CHECK_EQ(std::string(p0.detail), std::string("C:/Fake/modules-a"));
        CHECK_EQ(p2.value[0], 2.0);
        CHECK_EQ(std::strlen(p2.detail), 159u);  // evidence for a person: cut at the item's size
        CHECK_EQ(m0.value[0], 3.0);
        CHECK_EQ(std::string(m0.detail), std::string("C:/Fake/modules-a/rtlsdrSupport.dll"));
        for (uint32_t i = 0; i < 6; ++i) { CHECK_EQ(item(s, FOXAPI_LIST_SCAN_EVIDENCE, i).id, static_cast<uint64_t>(i + 1u)); }
        // LOCAL: paths and installs of the engine's machine. A REMOTE session is DENIED the count and the entry.
        CHECK_EQ(count(remote, FOXAPI_LIST_SCAN_EVIDENCE), FOXAPI_DENIED);
        FoxListItem none{};
        none.structSize = sizeof(none);
        CHECK_EQ(api()->read_list(remote.raw(), FOXAPI_LIST_SCAN_EVIDENCE, 0, &none), FOXAPI_DENIED);
        CHECK_EQ(std::string(none.detail), std::string());
        // THE SAME EVIDENCE AGAIN moves nothing: the counter the fingerprint hashes stays, and a different answer moves it.
        const uint64_t evidence = Access::evidenceSeq(d.eng);
        CHECK(g11::scanAll(d, s));
        CHECK_EQ(w.diagCalls.load(), 3);  // asked again at the landing...
        CHECK_EQ(Access::evidenceSeq(d.eng), evidence);  // ...and said the same
        w.modules.push_back("C:/Fake/modules-a/hackrfSupport.dll");
        CHECK(g11::scanAll(d, s));
        CHECK_EQ(Access::evidenceSeq(d.eng), evidence + 1u);
        CHECK_EQ(count(s, FOXAPI_LIST_SCAN_EVIDENCE), 7);
        // A scan that FINDS something clears it; so does one beside an open radio (it left drivers out on purpose).
        w.soapy = {{"Fake Soapy A", "driver=fake,serial=A"}};
        CHECK(g11::scanAll(d, s));
        CHECK_EQ(count(s, FOXAPI_LIST_SCAN_EVIDENCE), 0);
        w.soapy.clear();
        CHECK(g11::scanAll(d, s));
        CHECK_EQ(count(s, FOXAPI_LIST_SCAN_EVIDENCE), 7);
        CHECK_EQ(pumped(d.eng, s, textCmd(FOXAPI_OP_SELECT_SOURCE, engine_api_test::kFakeRadioId)).status(), FOXAPI_OK);
        CHECK(passUntil(d.eng, [&] { return g11::rxHas(s, FOXAPI_RX_DEVICE_OPEN); }));
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SCAN_DEVICES)).status(), FOXAPI_OK);
        CHECK(passUntil(d.eng, [&] { return !g11::has(s, FOXAPI_SRC_SCAN_PENDING); }));
        CHECK(g11::has(s, FOXAPI_SRC_SCAN_PARTIAL));
        CHECK_EQ(count(s, FOXAPI_LIST_SCAN_EVIDENCE), 0);
    }
    Access::uninstall11();
    std::printf("G11-7: SCAN_EVIDENCE: kinds 1-3 in order, taken once per empty whole landing, LOCAL (DENIED to REMOTE), unchanged by the same answer, cleared by a find and by a partial scan\n");
}

// G11-8: the sound card as its panel draws it - the lists, the facts, FOXAPP_OP_SOUND_CARD_PICK (the rate rule), the
// form's field mask, the converter behind the span, the open and the lamp - against a backend held on its gates.
void testSoundCardPanel() {
    using Access = cascade::gui::AppWindowTestAccess;
    Access::install11();
    g11::World& w = g11::world();
    {
        Direct d;
        g11::Sessions ss(d);
        testing::Session& s = ss.local;
        testing::Session& remote = ss.remote;
        passes(d.eng, 3);
        int32_t st = 0;
        // BEFORE THE CARDS ARE LISTED: no inputs, no rates, a form of the defaults (real mono, 48 kHz, nothing chosen),
        // a span of 0 to 24 kHz, and a pick has nothing to pick from.
        CHECK_EQ(count(s, FOXAPI_LIST_SOUND_CARDS), 0);
        CHECK_EQ(count(s, FOXAPI_LIST_SOUND_CARD_RATES), 0);
        CHECK_EQ(count(s, FOXAPI_LIST_SOUND_CARD_FORM), 1);
        {
            const FoxListItem f = item(s, FOXAPI_LIST_SOUND_CARD_FORM, 0);
            CHECK_EQ(f.id, 1u);
            CHECK_EQ(std::string(f.name), std::string());
            CHECK_EQ(std::string(f.detail), std::string());
            CHECK_EQ(f.value[0], 48000.0);
            CHECK_EQ(f.value[1], 0.0);
            CHECK_EQ(f.value[2], 0.0);
            CHECK_EQ(f.flags & (FOXAPI_ITEM_IQ_STEREO | FOXAPI_ITEM_SWAP_IQ | FOXAPI_ITEM_PICKED), 0u);
        }
        CHECK_EQ(fact(s, "fact.soundcard.span", &st), std::string("0 24000"));
        CHECK_EQ(pumped(d.eng, s, appCmd(FOXAPP_OP_SOUND_CARD_PICK, 1)).status(), FOXAPI_NOT_FOUND);
        CHECK_EQ(Access::form(d.eng).device, std::string());
        // The centre op, with no card running: NO_CHANGE, so the window can send it every time (and drop its gate).
        CHECK_EQ(pumped(d.eng, s, appCmd(FOXAPP_OP_SOUNDCARD_IQ_CENTRE, 0, 7.1e6)).status(), FOXAPI_NO_CHANGE);

        // THE LISTING (the row chosen: the engine takes the list on its worker), the three inputs in the backend's order,
        // each "device (host API)" with the host API in detail. An empty device in the form means the system's default
        // input, which is the entry flagged isDefault.
        w.listGate = false;
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SELECT_SOURCE, 0, 0, "row:soundcard")).status(), FOXAPI_OK);
        CHECK_EQ(count(s, FOXAPI_LIST_SOUND_CARDS), 0);  // in flight: still nothing
        w.listGate = true;
        CHECK(passUntil(d.eng, [&] { return g11::has(s, FOXAPI_SRC_CARD_LISTED); }));
        CHECK_EQ(w.listCalls.load(), 1);
        CHECK_EQ(count(s, FOXAPI_LIST_SOUND_CARDS), 3);
        const char* const labels[3] = {"Fake In A (Fake API)", "Fake In B (Fake API)", "Fake In C (Fake API)"};
        for (uint32_t i = 0; i < 3; ++i) {
            const FoxListItem c = item(s, FOXAPI_LIST_SOUND_CARDS, i);
            CHECK_EQ(c.id, static_cast<uint64_t>(i + 1u));
            CHECK_EQ(std::string(c.name), std::string(labels[i]));
            CHECK_EQ(std::string(c.detail), std::string("Fake API"));
            CHECK_EQ((c.flags & FOXAPI_ITEM_ACTIVE) != 0u, i == 0u);  // no device named: the default input
        }
        CHECK_EQ(count(remote, FOXAPI_LIST_SOUND_CARDS), 3);  // a card's name is not a path: listed to a REMOTE session

        // A PICK: card B, whose rates do not include the form's 48 kHz, so it lands on B's own default (192 kHz).
        CHECK_EQ(g11::cmdLandsOk(d, s, appCmd(FOXAPP_OP_SOUND_CARD_PICK, 2)), true);
        CHECK_EQ(Access::form(d.eng).device, std::string("Fake In B"));
        CHECK_EQ(Access::form(d.eng).hostApi, std::string("Fake API"));
        CHECK(Access::form(d.eng).pickedFromList);
        CHECK_EQ(Access::form(d.eng).cardRateHz, 192000.0);
        for (uint32_t i = 0; i < 3; ++i) {
            CHECK_EQ((item(s, FOXAPI_LIST_SOUND_CARDS, i).flags & FOXAPI_ITEM_ACTIVE) != 0u, i == 1u);
        }
        CHECK_EQ(count(s, FOXAPI_LIST_SOUND_CARD_RATES), 2);
        CHECK_EQ(item(s, FOXAPI_LIST_SOUND_CARD_RATES, 0).value[0], 96000.0);
        CHECK_EQ(item(s, FOXAPI_LIST_SOUND_CARD_RATES, 1).value[0], 192000.0);
        CHECK((item(s, FOXAPI_LIST_SOUND_CARD_RATES, 0).flags & FOXAPI_ITEM_ACTIVE) == 0u);
        CHECK((item(s, FOXAPI_LIST_SOUND_CARD_RATES, 1).flags & FOXAPI_ITEM_ACTIVE) != 0u);
        CHECK((item(s, FOXAPI_LIST_SOUND_CARD_RATES, 0).flags & FOXAPI_ITEM_EXCLUSIVE) == 0u);
        {
            const FoxListItem f = item(s, FOXAPI_LIST_SOUND_CARD_FORM, 0);
            CHECK_EQ(std::string(f.name), std::string("Fake In B"));  // a LOCAL session reads the card whole...
            CHECK_EQ(std::string(f.detail), std::string("Fake API"));
            CHECK_EQ(f.value[0], 192000.0);
            CHECK((f.flags & FOXAPI_ITEM_PICKED) != 0u);
            const FoxListItem fr = item(remote, FOXAPI_LIST_SOUND_CARD_FORM, 0);
            CHECK_EQ(std::string(fr.name), std::string());  // ...a REMOTE one reads the figures and flags, not the card
            CHECK_EQ(std::string(fr.detail), std::string());
            CHECK_EQ(fr.value[0], 192000.0);
            CHECK((fr.flags & FOXAPI_ITEM_PICKED) != 0u);
        }
        CHECK_EQ(fact(s, "fact.soundcard.device"), std::string("Fake In B"));
        CHECK_EQ(fact(s, "fact.soundcard.hostApi"), std::string("Fake API"));
        CHECK_EQ(fact(remote, "fact.soundcard.device", &st), std::string());
        CHECK_EQ(st, FOXAPI_DENIED);
        CHECK_EQ(fact(remote, "fact.soundcard.hostApi", &st), std::string());
        CHECK_EQ(st, FOXAPI_DENIED);
        CHECK_EQ(fact(s, "fact.soundcard.span"), std::string("0 96000"));  // real mode: 0 to rate/2
        CHECK_EQ(fact(remote, "fact.soundcard.span"), std::string("0 96000"));
        // Card A: B's 192 kHz is not A's; A's default is 48 kHz. And A's 96 kHz is EXCLUSIVE.
        CHECK_EQ(g11::cmdLandsOk(d, s, appCmd(FOXAPP_OP_SOUND_CARD_PICK, 1)), true);
        CHECK_EQ(Access::form(d.eng).cardRateHz, 48000.0);
        CHECK_EQ(count(s, FOXAPI_LIST_SOUND_CARD_RATES), 3);
        CHECK((item(s, FOXAPI_LIST_SOUND_CARD_RATES, 2).flags & FOXAPI_ITEM_EXCLUSIVE) != 0u);
        CHECK((item(s, FOXAPI_LIST_SOUND_CARD_RATES, 0).flags & FOXAPI_ITEM_EXCLUSIVE) == 0u);
        CHECK((item(s, FOXAPI_LIST_SOUND_CARD_RATES, 1).flags & FOXAPI_ITEM_ACTIVE) != 0u);  // 48 kHz
        // The rate is KEPT when the new card offers it: 96 kHz set by the form's own field, then A picked again.
        CHECK_EQ(g11::cmdLandsOk(d, s, cascade::gui::soundCardFormCommand(
                                      [] { cascade::source::SoundCardSettings x; x.cardRateHz = 96000.0; return x; }(),
                                      FOXAPP_SOUND_CARD_FIELD_RATE)),
                 true);
        CHECK_EQ(g11::cmdLandsOk(d, s, appCmd(FOXAPP_OP_SOUND_CARD_PICK, 1)), true);
        CHECK_EQ(Access::form(d.eng).cardRateHz, 96000.0);
        CHECK((item(s, FOXAPI_LIST_SOUND_CARD_RATES, 2).flags & FOXAPI_ITEM_ACTIVE) != 0u);
        // ...and the highest is the fallback when neither the rate nor the card's default is on offer (card C).
        CHECK_EQ(g11::cmdLandsOk(d, s, appCmd(FOXAPP_OP_SOUND_CARD_PICK, 3)), true);
        CHECK_EQ(Access::form(d.eng).cardRateHz, 64000.0);
        // A PICK clears the "not connected" line.
        Access::cardMissing(d.eng) = "Fake In Z (Fake API) is not connected.";
        passes(d.eng, 2);
        CHECK_EQ(g11::lineLevel(s, "soundCardMissing"), 2.0);
        CHECK_EQ(g11::cmdLandsOk(d, s, appCmd(FOXAPP_OP_SOUND_CARD_PICK, 1)), true);
        CHECK_EQ(g11::lineLevel(s, "soundCardMissing"), -1.0);
        // An id with no entry changes nothing; a REMOTE session may not pick.
        const cascade::source::SoundCardSettings held = Access::form(d.eng);
        for (long long bad : {0LL, 4LL, -1LL, 1000LL}) {
            CHECK_EQ(pumped(d.eng, s, appCmd(FOXAPP_OP_SOUND_CARD_PICK, bad)).status(), FOXAPI_NOT_FOUND);
        }
        CHECK_EQ(pumped(d.eng, remote, appCmd(FOXAPP_OP_SOUND_CARD_PICK, 2)).status(), FOXAPI_DENIED);
        CHECK(Access::form(d.eng).device == held.device && Access::form(d.eng).cardRateHz == held.cardRateHz);

        // THE FORM'S FIELD MASK, through the table: a command carrying the centre alone leaves the card, the rate and
        // the format as the engine holds them; the format alone leaves the rest; the I/Q span follows the centre.
        cascade::source::SoundCardSettings stale;  // every other field wrong
        stale.device = "Stale";
        stale.cardRateHz = 8000.0;
        stale.iqCentreHz = 7.1e6;
        CHECK_EQ(g11::cmdLandsOk(d, s, cascade::gui::soundCardFormCommand(stale, FOXAPP_SOUND_CARD_FIELD_CENTRE)), true);
        CHECK_EQ(Access::form(d.eng).device, std::string("Fake In A"));
        CHECK_EQ(Access::form(d.eng).cardRateHz, 48000.0);  // (A's default, which the last pick chose over C's 64 kHz)
        CHECK_EQ(Access::form(d.eng).iqCentreHz, 7.1e6);
        CHECK_EQ(item(s, FOXAPI_LIST_SOUND_CARD_FORM, 0).value[1], 7.1e6);
        CHECK_EQ(fact(s, "fact.soundcard.span"), std::string("0 24000"));  // real mode: the centre is not the span's
        cascade::source::SoundCardSettings iq;
        iq.format = cascade::source::SoundCardFormat::IqStereo;
        CHECK_EQ(g11::cmdLandsOk(d, s, cascade::gui::soundCardFormCommand(iq, FOXAPP_SOUND_CARD_FIELD_FORMAT)), true);
        CHECK((item(s, FOXAPI_LIST_SOUND_CARD_FORM, 0).flags & FOXAPI_ITEM_IQ_STEREO) != 0u);
        CHECK_EQ(item(s, FOXAPI_LIST_SOUND_CARD_FORM, 0).value[0], 48000.0);
        CHECK_EQ(fact(s, "fact.soundcard.span"), std::string("7076000 7124000"));  // the centre +/- 24 kHz
        CHECK_EQ(count(s, FOXAPI_LIST_SOUND_CARD_RATES), 3);  // A takes the same rates in I/Q (two channels)
        cascade::source::SoundCardSettings swap;
        swap.swapIq = true;
        swap.channel = 1;
        CHECK_EQ(g11::cmdLandsOk(d, s, cascade::gui::soundCardFormCommand(swap, FOXAPP_SOUND_CARD_FIELD_SWAP_IQ)), true);
        CHECK((item(s, FOXAPI_LIST_SOUND_CARD_FORM, 0).flags & FOXAPI_ITEM_SWAP_IQ) != 0u);
        CHECK_EQ(item(s, FOXAPI_LIST_SOUND_CARD_FORM, 0).value[2], 0.0);  // the channel was not carried
        CHECK_EQ(g11::cmdLandsOk(d, s, cascade::gui::soundCardFormCommand(swap, FOXAPP_SOUND_CARD_FIELD_CHANNEL)), true);
        CHECK_EQ(item(s, FOXAPI_LIST_SOUND_CARD_FORM, 0).value[2], 1.0);

        // THE CONVERTER BEHIND THE SPAN: a card in I/Q mode takes none (the centre is already the air), a real card takes
        // a down-converter (air = radio + LO), and a stored up-converter is not used for it.
        const std::string key = cascade::gui::soundCardConverterKey("Fake In A", "Fake API");
        cascade::core::ConverterSetting down;
        down.mode = cascade::core::ConverterMode::Down;
        down.loHz = 100.0e6;
        Access::converters(d.eng)[key] = down;
        passes(d.eng, 2);
        CHECK_EQ(fact(s, "fact.soundcard.span"), std::string("7076000 7124000"));  // I/Q: unchanged
        cascade::source::SoundCardSettings real;  // format real: bit 1 clear
        CHECK_EQ(g11::cmdLandsOk(d, s, cascade::gui::soundCardFormCommand(real, FOXAPP_SOUND_CARD_FIELD_FORMAT)), true);
        CHECK_EQ(fact(s, "fact.soundcard.span"), std::string("100000000 100024000"));  // air = radio + LO, 0 to 24 kHz
        cascade::core::ConverterSetting up;
        up.mode = cascade::core::ConverterMode::Up;
        up.loHz = 100.0e6;
        Access::converters(d.eng)[key] = up;
        passes(d.eng, 2);
        CHECK_EQ(fact(s, "fact.soundcard.span"), std::string("0 24000"));  // an Up is not used by a real card
        Access::converters(d.eng).erase(key);
        passes(d.eng, 2);
    }
    {
        // THE OPEN, and the lamp's inputs while it runs: the form's card opens on its worker (CARD_OPENING, no radio's
        // flag), then it IS the source in use (DEVICES' sound card entry ACTIVE, the combo's row 2); opening it AGAIN
        // releases the running card first and keeps it for the config - fact.source.keep says "soundcard", the source in
        // use is the generator and OPENING is set, which is exactly the lamp's "do not light" state - and it comes back.
        Direct d;
        g11::Sessions ss(d);
        testing::Session& s = ss.local;
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SELECT_SOURCE, 0, 0, "row:soundcard")).status(), FOXAPI_OK);
        CHECK(passUntil(d.eng, [&] { return g11::has(s, FOXAPI_SRC_CARD_LISTED); }));
        CHECK_EQ(g11::cmdLandsOk(d, s, appCmd(FOXAPP_OP_SOUND_CARD_PICK, 1)), true);
        CHECK_EQ(fact(s, "fact.source.keep"), std::string());
        w.openGate = false;
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SELECT_SOURCE, 0, 0, "soundcard:open")).status(), FOXAPI_OK);
        CHECK(g11::has(s, FOXAPI_SRC_CARD_OPENING));
        CHECK(check::waitFor([&] { return w.openCalls.load() == 1; }, 2000));  // the worker is inside the backend's open
        w.openGate = true;
        CHECK(passUntil(d.eng, [&] { return !g11::has(s, FOXAPI_SRC_CARD_OPENING); }));
        CHECK_EQ(Access::sourceKind(d.eng), std::string("soundcard"));
        int at = -1;
        CHECK_EQ(activeItem(s, FOXAPI_LIST_DEVICES, &at).id, 3u);  // the sound card's entry
        CHECK_EQ(at, 2);
        CHECK_EQ(fact(s, "fact.source.row"), std::string("2"));
        CHECK_EQ(Access::liveCard(d.eng).device, std::string("Fake In A"));
        CHECK_EQ(fact(s, "fact.soundcard.device"), std::string("Fake In A"));
        // The centre op: this card runs in REAL mode, so a typed centre is kept for the next Open and not sent live.
        CHECK_EQ(pumped(d.eng, s, appCmd(FOXAPP_OP_SOUNDCARD_IQ_CENTRE, 0, 7.1e6)).status(), FOXAPI_NO_CHANGE);
        // OPEN AGAIN with the card running.
        w.openGate = false;
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SELECT_SOURCE, 0, 0, "soundcard:open")).status(), FOXAPI_OK);
        CHECK(g11::has(s, FOXAPI_SRC_CARD_OPENING));
        CHECK_EQ(fact(s, "fact.source.keep"), std::string("soundcard"));
        CHECK(!g11::rxHas(s, FOXAPI_RX_DEVICE_OPEN));
        CHECK_EQ(Access::sourceKind(d.eng), std::string("siggen"));
        CHECK_EQ(activeItem(s, FOXAPI_LIST_DEVICES, &at).id, 1u);  // the generator stands in
        w.openGate = true;
        CHECK(passUntil(d.eng, [&] { return !g11::has(s, FOXAPI_SRC_CARD_OPENING); }));
        CHECK_EQ(Access::sourceKind(d.eng), std::string("soundcard"));
        CHECK_EQ(fact(s, "fact.source.keep"), std::string());
        CHECK_EQ(w.openCalls.load(), 2);  // one open each time (the release is not an open)
    }
    Access::uninstall11();
    std::printf("G11-8: SOUND_CARDS, SOUND_CARD_RATES and SOUND_CARD_FORM (LOCAL name), fact.soundcard.*, the pick's rate rule (kept / default / highest), the form's mask, the converter behind the span, the open and the keep\n");
}

// G11-9: what the window used to send every frame is the engine's own pass now: no command in these tests says it.
void testPerFrameMovesAreThePasss() {
    using Access = cascade::gui::AppWindowTestAccess;
    Access::install11();
    g11::World& w = g11::world();
    {
        // THE SOUND CARDS ARE LISTED BECAUSE THE ROW IS SELECTED - nobody sent SOUND_CARDS_WANTED, and the combo's own
        // handler was not run (the row is set as a restored config sets it): the pass lists them, once.
        Direct d;
        g11::Sessions ss(d);
        testing::Session& s = ss.local;
        passes(d.eng, 5);
        CHECK_EQ(w.listCalls.load(), 0);  // the row is the generator: never asked for
        Access::sourceSel(d.eng) = 2;
        w.listGate = false;
        passes(d.eng, 3);
        CHECK(g11::has(s, FOXAPI_SRC_CARD_SCANNING));
        CHECK(check::waitFor([&] { return w.listCalls.load() == 1; }, 2000));  // the worker is inside the backend's listing
        passes(d.eng, 20);  // in flight: not asked again by every pass
        CHECK_EQ(w.listCalls.load(), 1);
        w.listGate = true;
        CHECK(passUntil(d.eng, [&] { return g11::has(s, FOXAPI_SRC_CARD_LISTED); }));
        passes(d.eng, 20);  // listed: not at all
        CHECK_EQ(w.listCalls.load(), 1);
        CHECK_EQ(count(s, FOXAPI_LIST_SOUND_CARDS), 3);
        // The same engine on another row: a listed list is not asked for either, and no row but 2 asks.
        Access::sourceSel(d.eng) = 0;
        Access::cardListed(d.eng) = false;
        Access::cards(d.eng).clear();
        passes(d.eng, 10);
        CHECK_EQ(w.listCalls.load(), 1);
    }
    {
        // ...and not while a card is opening.
        Direct d;
        g11::Sessions ss(d);
        testing::Session& s = ss.local;
        passes(d.eng, 2);
        Access::cardOpening(d.eng) = true;  // an open in flight (no worker: the pass leaves it alone)
        Access::sourceSel(d.eng) = 2;
        passes(d.eng, 10);
        CHECK_EQ(w.listCalls.load(), 1);  // unchanged from the first engine's single listing
        Access::cardOpening(d.eng) = false;
        passes(d.eng, 3);
        CHECK(check::waitFor([&] { return w.listCalls.load() == 2; }, 2000));  // the opening is over: the wish is carried out
        CHECK(passUntil(d.eng, [&] { return g11::has(s, FOXAPI_SRC_CARD_LISTED); }));
    }
    {
        // THE DEFERRAL'S LOG LATCH: set (as a deferral sets it) with nothing gating the scan, the pass lets it go.
        Direct d;
        g11::Sessions ss(d);
        testing::Session& s = ss.local;
        passes(d.eng, 2);
        CHECK(!Access::gated(d.eng));
        Access::deferredLogged(d.eng) = true;
        passes(d.eng, 1);
        CHECK(!Access::deferredLogged(d.eng));
        // With the gate shut it holds: a radio open, as many passes as you like.
        CHECK(g11::scanAll(d, s));
        CHECK_EQ(pumped(d.eng, s, textCmd(FOXAPI_OP_SELECT_SOURCE, engine_api_test::kFakeRadioId)).status(), FOXAPI_OK);
        CHECK(passUntil(d.eng, [&] { return g11::rxHas(s, FOXAPI_RX_DEVICE_OPEN); }));
        CHECK(Access::gated(d.eng));
        Access::deferredLogged(d.eng) = true;
        passes(d.eng, 10);
        CHECK(Access::deferredLogged(d.eng));
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SELECT_SOURCE, 0, 0, "siggen")).status(), FOXAPI_OK);
        passes(d.eng, 1);
        CHECK(!Access::deferredLogged(d.eng));
    }
    Access::uninstall11();
    std::printf("G11-9: the sound card list is taken by the pass while row 2 is selected (once, never in flight or opening), the deferral's log latch is let go by the pass when the gate opens\n");
}

// G11-10: listSeq EXACTNESS. Every input the section's lists, facts and flags are built from moves the fingerprint, and
// so listSeq, when it changes and nothing else does - the friend writes each member alone, on an engine with a radio
// open and the sound cards listed (so nothing else that a probe touches moves with it) - once for the change, never for
// the same value said again, and once more when it is put back. A member that is not hashed leaves its probe red.
void testSourceListSeqExactness() {
    using Access = cascade::gui::AppWindowTestAccess;
    Access::install11();
    {
        Direct d;
        g11::Sessions ss(d);
        testing::Session& s = ss.local;
        CHECK(g11::scanAll(d, s));
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SELECT_SOURCE, 0, 0, "row:soundcard")).status(), FOXAPI_OK);
        CHECK(passUntil(d.eng, [&] { return g11::has(s, FOXAPI_SRC_CARD_LISTED); }));
        CHECK_EQ(pumped(d.eng, s, textCmd(FOXAPI_OP_SELECT_SOURCE, engine_api_test::kFakeRadioId)).status(), FOXAPI_OK);
        CHECK(passUntil(d.eng, [&] { return g11::rxHas(s, FOXAPI_RX_DEVICE_OPEN); }));
        passes(d.eng, 5);
        cascade::engine::Engine& e = d.eng;
        Access::sourceSel(e) = 0;  // not the sound card's row: an un-listed card list would be asked for by the pass
        passes(e, 3);
        int probes = 0;
        const std::string model0 = Access::deviceModel(e);
        const std::string label0 = Access::rateLabels(e)[0];
        CHECK(Access::scanned(e));  // the scan above started one
        const auto probe = [&](const char* what, auto change, auto revert) {
            passes(e, 3);
            const uint64_t seq0 = s.state().listSeq;
            const uint64_t fp0 = Access::fingerprint(e);
            change();
            CHECK(Access::fingerprint(e) != fp0);  // the member is in the fingerprint
            passes(e, 1);
            const uint64_t seq1 = s.state().listSeq;
            bool exact = seq1 == seq0 + 1u;  // once for the change
            passes(e, 3);
            exact = exact && s.state().listSeq == seq1;  // ...and not once a pass
            change();                                    // the same value said again moves nothing
            passes(e, 2);
            exact = exact && s.state().listSeq == seq1;
            revert();
            passes(e, 1);
            exact = exact && s.state().listSeq == seq1 + 1u;  // and once more when it is put back
            if (!exact) {
                std::printf("  G11-10: %s: listSeq %llu, %llu after the change, %llu settled\n", what,
                            static_cast<unsigned long long>(seq0), static_cast<unsigned long long>(seq1),
                            static_cast<unsigned long long>(s.state().listSeq));
            }
            CHECK(exact);
            passes(e, 2);
            ++probes;
        };
        probe("the combo's row", [&] { Access::sourceSel(e) = 4; }, [&] { Access::sourceSel(e) = 0; });
        probe("the kind kept for the config", [&] { Access::keepKind(e) = "soundcard"; }, [&] { Access::keepKind(e).clear(); });
        probe("a radio being opened (flag, with the gate shut already)", [&] { Access::openPending(e) = true; },
              [&] { Access::openPending(e) = false; });
        probe("the label of the radio being opened", [&] { Access::busyLabel(e) = "Opening label"; },
              [&] { Access::busyLabel(e).clear(); });
        probe("the open radio's model", [&] { Access::deviceModel(e) = "Another model"; },
              [&] { Access::deviceModel(e) = model0; });
        probe("a scan in flight", [&] { Access::scanPending(e) = true; }, [&] { Access::scanPending(e) = false; });
        probe("a scan started", [&] { Access::scanned(e) = false; }, [&] { Access::scanned(e) = true; });
        probe("a partial scan", [&] { Access::scanPartial(e) = true; }, [&] { Access::scanPartial(e) = false; });
        probe("a patch radio still opening", [&] { Access::pendPatchRadio(e, true); }, [&] { Access::pendPatchRadio(e, false); });
        probe("look for network USRPs", [&] { Access::networkUsrps(e) = true; }, [&] { Access::networkUsrps(e) = false; });
        probe("a driver left out of the last scan", [&] { Access::absent(e) = {"uhd"}; }, [&] { Access::absent(e).clear(); });
        probe("a different driver left out", [&] { Access::absent(e) = {"rtlsdr"}; }, [&] { Access::absent(e).clear(); });
        probe("an SDRplay row found", [&] { Access::sdrRowsFound(e) = true; }, [&] { Access::sdrRowsFound(e) = false; });
        probe("the Pluto's address", [&] { std::snprintf(Access::plutoBuffer(e), 192, "%s", "ip:10.1.1.1"); },
              [&] { std::snprintf(Access::plutoBuffer(e), 192, "%s", "ip:192.168.2.1"); });
        const auto unboundDongle = [] {
            cascade::usb::UsbDeviceInfo u;
            u.vid = 0x0BDA;
            u.pid = 0x2838;
            u.description = "A dongle";
            return u;
        };
        probe("a dongle that cannot be opened", [&] { Access::unbound(e) = {unboundDongle()}; }, [&] { Access::unbound(e).clear(); });
        Access::unbound(e) = {unboundDongle()};
        passes(e, 3);
        probe("that dongle's vendor id", [&] { Access::unbound(e)[0].vid = 0x1D50; }, [&] { Access::unbound(e)[0].vid = 0x0BDA; });
        probe("that dongle's product id", [&] { Access::unbound(e)[0].pid = 0x6089; }, [&] { Access::unbound(e)[0].pid = 0x2838; });
        probe("that dongle's description", [&] { Access::unbound(e)[0].description = "Renamed"; },
              [&] { Access::unbound(e)[0].description = "A dongle"; });
        Access::unbound(e).clear();
        passes(e, 3);
        probe("a driver that crashed the scan", [&] { Access::faulted(e) = {"uhd"}; }, [&] { Access::faulted(e).clear(); });
        const uint64_t evidence0 = Access::evidenceSeq(e);
        probe("the runtime's evidence", [&] { Access::evidenceSeq(e) = evidence0 + 7u; }, [&] { Access::evidenceSeq(e) = evidence0; });
        // (The fault sentence is rewritten by every pass from the pipeline, so it is probed on the fingerprint itself,
        // without a pass between the write and the read; G11-4 holds the real path through test.fault.)
        {
            const uint64_t before = Access::fingerprint(e);
            Access::faultText(e) = "the radio stopped";
            const uint64_t withText = Access::fingerprint(e);
            CHECK(withText != before);
            Access::faultText(e) = "the radio stopped";
            CHECK_EQ(Access::fingerprint(e), withText);
            Access::faultText(e).clear();
            CHECK_EQ(Access::fingerprint(e), before);
            ++probes;
        }
        probe("a rate's caption", [&] { Access::rateLabels(e)[0] = "a different caption"; },
              [&] { Access::rateLabels(e)[0] = label0; });
        probe("the card list taken", [&] { Access::cardListed(e) = false; }, [&] { Access::cardListed(e) = true; });
        probe("the card list in flight", [&] { Access::cardScanning(e) = true; }, [&] { Access::cardScanning(e) = false; });
        probe("a card opening", [&] { Access::cardOpening(e) = true; }, [&] { Access::cardOpening(e) = false; });
        probe("the form's device", [&] { Access::form(e).device = "Fake In B"; }, [&] { Access::form(e).device.clear(); });
        probe("the form's host API", [&] { Access::form(e).hostApi = "Other API"; }, [&] { Access::form(e).hostApi.clear(); });
        probe("the form's format", [&] { Access::form(e).format = cascade::source::SoundCardFormat::IqStereo; },
              [&] { Access::form(e).format = cascade::source::SoundCardFormat::RealMono; });
        probe("the form's channel", [&] { Access::form(e).channel = 1; }, [&] { Access::form(e).channel = 0; });
        probe("the form's swap", [&] { Access::form(e).swapIq = true; }, [&] { Access::form(e).swapIq = false; });
        probe("the form's centre", [&] { Access::form(e).iqCentreHz = 7.1e6; }, [&] { Access::form(e).iqCentreHz = 0.0; });
        probe("the form's rate", [&] { Access::form(e).cardRateHz = 96000.0; }, [&] { Access::form(e).cardRateHz = 48000.0; });
        probe("the form's pick flag", [&] { Access::form(e).pickedFromList = true; }, [&] { Access::form(e).pickedFromList = false; });
        probe("an input's name", [&] { Access::cards(e)[0].name = "Renamed in"; }, [&] { Access::cards(e)[0].name = "Fake In A"; });
        probe("an input's host API", [&] { Access::cards(e)[0].hostApi = "Other API"; }, [&] { Access::cards(e)[0].hostApi = "Fake API"; });
        probe("an input's default rate", [&] { Access::cards(e)[0].defaultRateHz = 44100.0; },
              [&] { Access::cards(e)[0].defaultRateHz = 48000.0; });
        probe("one of an input's rates", [&] { Access::cards(e)[0].rates[0].hz = 22050.0; }, [&] { Access::cards(e)[0].rates[0].hz = 44100.0; });
        probe("a rate's exclusive flag", [&] { Access::cards(e)[0].rates[0].exclusive = true; },
              [&] { Access::cards(e)[0].rates[0].exclusive = false; });
        std::printf("G11-10: %d inputs of the Source section, each moving listSeq once alone and nothing the second time\n", probes);
    }
    Access::uninstall11();
}
// G11 TESTS END>>
// <<G12 TESTS BEGIN
// G12: the radar scope and the demod scope as they draw (window port group 12, API 0.3 section 8.11): the receiver's
// position in the state (the flag and the two coordinates) and the scope tap's capacity.

FoxCommand setPositionCmd(double lat, double lon) {
    FoxCommand c{};
    c.structSize = sizeof(c);
    c.op = FOXAPI_OP_SET_POSITION;
    c.num[0] = lat;
    c.num[1] = lon;
    return c;
}

// G12-1: the position as the state says it. Nothing set: the flag clear and both coordinates 0. SET_POSITION: the flag
// up and the pair as sent, one pass later, moving seq and no other counter; each coordinate its own (a pair swapped
// goes red); a refusal changes nothing; the same position again moves nothing; a position restored from the
// configuration is in the FIRST published state.
void testPositionState() {
    CHECK_EQ(FOXAPI_RX_POSITION_SET, 0x40000000u);
    CHECK_EQ(offsetof(FoxReceiverState, rxLonDeg), offsetof(FoxReceiverState, rxLatDeg) + sizeof(double));
    CHECK_EQ(FOXAPI_MIN_RECEIVER_STATE, offsetof(FoxReceiverState, rxLonDeg) + sizeof(double));
    {
        Direct d;
        testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
        CHECK(s.ok());
        d.eng.controlPass();
        const FoxReceiverState none = s.state();
        CHECK((none.flags & FOXAPI_RX_POSITION_SET) == 0u);
        CHECK_EQ(none.rxLatDeg, 0.0);
        CHECK_EQ(none.rxLonDeg, 0.0);
        // Passes with nothing to say move nothing.
        for (int i = 0; i < 4; ++i) { d.eng.controlPass(); }
        CHECK_EQ(countersMoved(none, s.state()), std::string());

        // A position: the flag and the pair, one pass after the command, in seq alone.
        const double lat = 51.5074, lon = -0.1278;  // not equal, not symmetrical: a swap shows
        CHECK_EQ(pumped(d.eng, s, setPositionCmd(lat, lon)).status(), FOXAPI_OK);
        const FoxReceiverState set = s.state();
        std::printf("G12-1: SET_POSITION %.4f %.4f -> flag %d, state %.4f %.4f, moved [%s]\n", lat, lon,
                    (set.flags & FOXAPI_RX_POSITION_SET) != 0u ? 1 : 0, set.rxLatDeg, set.rxLonDeg,
                    countersMoved(none, set).c_str());
        CHECK((set.flags & FOXAPI_RX_POSITION_SET) != 0u);
        CHECK_EQ(set.rxLatDeg, lat);
        CHECK_EQ(set.rxLonDeg, lon);
        CHECK_EQ(countersMoved(none, set), std::string("seq"));
        CHECK_EQ(set.seq, none.seq + 1u);
        // Only the flag moved among the flags: every other bit is as it was.
        CHECK_EQ(set.flags & ~FOXAPI_RX_POSITION_SET, none.flags & ~FOXAPI_RX_POSITION_SET);

        // The same position again: nothing moves (the answer may be OK or NO_CHANGE; the state is the test).
        (void)pumped(d.eng, s, setPositionCmd(lat, lon));
        for (int i = 0; i < 3; ++i) { d.eng.controlPass(); }
        CHECK_EQ(countersMoved(set, s.state()), std::string());

        // REFUSED, never clamped: off the globe either way, and the pair 0,0 the rule also refuses. State untouched.
        CHECK_EQ(pumped(d.eng, s, setPositionCmd(91.0, 10.0)).status(), FOXAPI_OUT_OF_RANGE);
        CHECK_EQ(pumped(d.eng, s, setPositionCmd(10.0, 181.0)).status(), FOXAPI_OUT_OF_RANGE);
        CHECK_EQ(pumped(d.eng, s, setPositionCmd(0.0, 0.0)).status(), FOXAPI_OUT_OF_RANGE);
        const FoxReceiverState refused = s.state();
        CHECK_EQ(countersMoved(set, refused), std::string());
        CHECK_EQ(refused.rxLatDeg, lat);
        CHECK_EQ(refused.rxLonDeg, lon);
        CHECK((refused.flags & FOXAPI_RX_POSITION_SET) != 0u);

        // A second position replaces the first: both coordinates, one seq step, the flag stays.
        CHECK_EQ(pumped(d.eng, s, setPositionCmd(-33.8688, 151.2093)).status(), FOXAPI_OK);
        const FoxReceiverState second = s.state();
        CHECK_EQ(second.rxLatDeg, -33.8688);
        CHECK_EQ(second.rxLonDeg, 151.2093);
        CHECK((second.flags & FOXAPI_RX_POSITION_SET) != 0u);
        CHECK_EQ(countersMoved(set, second), std::string("seq"));
        // A change of the longitude alone, and of the latitude alone, are each a move.
        CHECK_EQ(pumped(d.eng, s, setPositionCmd(-33.8688, 150.0)).status(), FOXAPI_OK);
        CHECK_EQ(countersMoved(second, s.state()), std::string("seq"));
        CHECK_EQ(s.state().rxLonDeg, 150.0);
        const FoxReceiverState lonOnly = s.state();
        CHECK_EQ(pumped(d.eng, s, setPositionCmd(-30.0, 150.0)).status(), FOXAPI_OK);
        CHECK_EQ(countersMoved(lonOnly, s.state()), std::string("seq"));
        CHECK_EQ(s.state().rxLatDeg, -30.0);
        // The facts the position rode in on are unmoved: the lists' counter is the lists', not the position's.
        CHECK_EQ(s.state().listSeq, none.listSeq);
        // A position in the western and southern hemispheres keeps its signs (not an absolute value).
        CHECK(s.state().rxLatDeg < 0.0);
    }
    {
        // A position restored from the saved configuration is in the first published state (the radar scope's
        // first frame must already see it; test_patch_map_credit holds the real application).
        cascade::engine::Engine eng;
        eng.configureApi(kHooks);
        eng.initialise();
        cascade::core::AppConfig cfg;
        cfg.rxPositionSet = true;
        cfg.rxLatDeg = 48.8566;
        cfg.rxLonDeg = 2.3522;
        eng.applyConfig(cfg);
        testing::Session s(api(), eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
        CHECK(s.ok());
        eng.controlPass();
        const FoxReceiverState st = s.state();
        CHECK((st.flags & FOXAPI_RX_POSITION_SET) != 0u);
        CHECK_EQ(st.rxLatDeg, 48.8566);
        CHECK_EQ(st.rxLonDeg, 2.3522);
        std::printf("G12-1: a position restored from the configuration, first publish: flag %d, %.4f %.4f\n",
                    (st.flags & FOXAPI_RX_POSITION_SET) != 0u ? 1 : 0, st.rxLatDeg, st.rxLonDeg);
    }
    {
        // A configuration that says no position, with coordinates left in it: the flag is clear and the pair is 0
        // (the pair means nothing without the flag, and is never published as a place).
        cascade::engine::Engine eng;
        eng.configureApi(kHooks);
        eng.initialise();
        cascade::core::AppConfig cfg;
        cfg.rxPositionSet = false;
        cfg.rxLatDeg = 12.0;
        cfg.rxLonDeg = 34.0;
        eng.applyConfig(cfg);
        testing::Session s(api(), eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
        eng.controlPass();
        const FoxReceiverState st = s.state();
        CHECK((st.flags & FOXAPI_RX_POSITION_SET) == 0u);
        CHECK_EQ(st.rxLatDeg, 0.0);
        CHECK_EQ(st.rxLonDeg, 0.0);
    }
}

// G12-2: the scope tap's capacity. Per tap, read by an info-only call (capFrames 0), for a LOCAL and a REMOTE session;
// the most a call returns is the capacity (a request past it is cut to it, the ring is full); the unknown tap, the
// short info and the unset structSize leave the info and the answer as the table says; the info's tail is not
// overwritten past the version's own end.
void testScopeCapacity() {
    // The layout: the reserved word became the capacity in place, nothing moved.
    CHECK_EQ(offsetof(FoxScopeInfo, capacityFrames), 12u);
    CHECK_EQ(offsetof(FoxScopeInfo, rateHz), 16u);
    CHECK_EQ(sizeof(FoxScopeInfo), 32u);
    CHECK(FOXAPI_MIN_SCOPE_INFO == sizeof(FoxScopeInfo));
    CHECK(offsetof(FoxScopeInfo, capacityFrames) + sizeof(uint32_t) <= FOXAPI_MIN_SCOPE_INFO);

    engine_api_test::SerialEngine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    Remote r(e);
    const FoxEngineApi* a = api();
    struct Tap {
        uint32_t tap;
        const char* name;
        uint32_t ring;
    };
    const Tap taps[] = {{FOXAPI_SCOPE_AUDIO, "audio", 65536u}, {FOXAPI_SCOPE_IQ, "I/Q", 262144u}, {FOXAPI_SCOPE_MPX, "multiplex", 65536u}};
    for (const Tap& t : taps) {
        // The receiver need not be running: the capacity is the ring's, constant for the engine's life.
        for (const bool remote : {false, true}) {
            FoxScopeInfo info{};
            info.structSize = sizeof(info);
            info.capacityFrames = 0xDEADBEEFu;  // a sentinel the call must overwrite
            const int32_t n = a->read_scope(remote ? r.raw() : s.raw(), t.tap, &info, nullptr, 0);
            const uint32_t want = remote ? std::min<uint32_t>(t.ring, FOXAPI_SCOPE_REMOTE_FRAMES) : t.ring;
            std::printf("G12-2: %-9s %-6s info-only read -> %d, capacityFrames %u (wanted %u)\n", t.name,
                        remote ? "REMOTE" : "LOCAL", n, info.capacityFrames, want);
            CHECK_EQ(n, 0);
            CHECK_EQ(info.capacityFrames, want);
            CHECK_EQ(info.tap, t.tap);
        }
        // A REMOTE session reads min(ring, 4096): the constant, whatever the ring. Held by VALUE: std::min returns a
        // reference, here to the temporary the macro's uint32_t conversion made, which CHECK_EQ's `const auto&` would
        // read after it died (GCC: -Wdangling-reference; it read 0 under ThreadSanitizer).
        const uint32_t remoteCap = std::min<uint32_t>(t.ring, FOXAPI_SCOPE_REMOTE_FRAMES);
        CHECK_EQ(remoteCap, FOXAPI_SCOPE_REMOTE_FRAMES);
    }
    // The capacity is not what ONE call returns for an empty ring: nothing has been written and the answer is 0.
    // Running the receiver fills the audio ring; a request past the capacity is cut to it (LOCAL: the ring; REMOTE:
    // 4096). The audio ring holds 1.37 s, so this waits for it to be full.
    CHECK_EQ(s.run(cmd(FOXAPI_OP_RUN, 0, 1)).result.status, FOXAPI_OK);
    std::vector<float> buf(2 * 262144 + 16);
    FoxScopeInfo info{};
    info.structSize = sizeof(info);
    CHECK(check::waitFor(
        [&] { return a->read_scope(s.raw(), FOXAPI_SCOPE_AUDIO, &info, buf.data(), 70000) == 65536; }, 20000));
    CHECK_EQ(info.capacityFrames, 65536u);
    CHECK(info.written >= 65536u);
    // The most a call can return IS capacityFrames. A read of the WHOLE ring against a running receiver is thrown away
    // by ScopeTap::snapshot whenever a push begins during the copy (any push reaches back into a full-ring span), and
    // after three laps the call answers 0 - the table's own "lapped, ask again". Under ThreadSanitizer the copy is slow
    // enough for that to happen (one run in three), so the read is asked again until it is not lapped, within the
    // bound the fill above has; every answer must be 0 or exactly the capacity, and the last one must be the capacity.
    int32_t local = 0;
    bool onlyLapsOrCapacity = true;
    CHECK(check::waitFor(
        [&] {
            local = a->read_scope(s.raw(), FOXAPI_SCOPE_AUDIO, &info, buf.data(), info.capacityFrames + 1000u);
            if (local != 0 && local != static_cast<int32_t>(info.capacityFrames)) { onlyLapsOrCapacity = false; }
            return local != 0;
        },
        20000));
    CHECK(onlyLapsOrCapacity);
    CHECK_EQ(local, static_cast<int32_t>(info.capacityFrames));
    FoxScopeInfo rinfo{};
    rinfo.structSize = sizeof(rinfo);
    const int32_t remote = a->read_scope(r.raw(), FOXAPI_SCOPE_AUDIO, &rinfo, buf.data(), 70000);
    CHECK_EQ(remote, static_cast<int32_t>(rinfo.capacityFrames));
    CHECK_EQ(rinfo.capacityFrames, FOXAPI_SCOPE_REMOTE_FRAMES);
    // The capacity is the same read to read (it does not move with the ring's fill).
    CHECK_EQ(a->read_scope(s.raw(), FOXAPI_SCOPE_IQ, &info, nullptr, 0), 0);
    CHECK_EQ(info.capacityFrames, 262144u);
    CHECK_EQ(info.channels, 2u);
    s.run(cmd(FOXAPI_OP_RUN, 0, 0));

    // THE EDGES. An unknown tap is a negative answer and the info is not written (a client that reads it anyway sees
    // its own sentinel, not a capacity).
    FoxScopeInfo unknown{};
    unknown.structSize = sizeof(unknown);
    unknown.capacityFrames = 0xDEADBEEFu;
    CHECK_EQ(a->read_scope(s.raw(), 99u, &unknown, buf.data(), 16), FOXAPI_UNSUPPORTED);
    CHECK(FOXAPI_UNSUPPORTED < 0);
    CHECK_EQ(unknown.capacityFrames, 0xDEADBEEFu);
    // A short structSize (or none) is BAD_ARGUMENT, a negative answer, and the info is not written.
    for (const uint32_t bad : {0u, 8u, static_cast<uint32_t>(FOXAPI_MIN_SCOPE_INFO) - 1u}) {
        FoxScopeInfo shortInfo{};
        shortInfo.structSize = bad;
        shortInfo.capacityFrames = 0xDEADBEEFu;
        const int32_t n = a->read_scope(s.raw(), FOXAPI_SCOPE_AUDIO, &shortInfo, buf.data(), 16);
        CHECK_EQ(n, FOXAPI_BAD_ARGUMENT);
        CHECK(n < 0);
        CHECK_EQ(shortInfo.capacityFrames, 0xDEADBEEFu);
    }
    // No buffer with a count is BAD_ARGUMENT; no buffer and no count is the info-only call.
    CHECK_EQ(a->read_scope(s.raw(), FOXAPI_SCOPE_AUDIO, nullptr, nullptr, 16), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(a->read_scope(s.raw(), FOXAPI_SCOPE_AUDIO, nullptr, nullptr, 0), 0);
    // A structSize of exactly the minimum is served in full, capacity included.
    FoxScopeInfo exact{};
    exact.structSize = static_cast<uint32_t>(FOXAPI_MIN_SCOPE_INFO);
    CHECK_EQ(a->read_scope(s.raw(), FOXAPI_SCOPE_MPX, &exact, nullptr, 0), 0);
    CHECK_EQ(exact.capacityFrames, 65536u);
    std::printf("G12-2: the capacity per tap and session, the cut at the ring, the edges: done\n");
}
// G12 TESTS END>>
// <<G13 TESTS BEGIN
// G13: the Patch page, part 1 (window port group 13, API 0.3 section 8.12): the patch runtime in the engine's pass
// behind FOXAPP_OP_SET_PATCH_PAGE_OPEN, the graph's version and epoch published with the graph, a merge-safe
// FOXAPP_OP_PATCH_SET_GRAPH, and the plan (PATCH_PLAN, fact.patch.runnable, PATCH_RADIOS' centre, pending and note).
// Every case drives the engine from THIS thread (controlPass until the thing has happened), against the built-in signal
// generator as the patch's radio (it opens synchronously and needs no device), so what the test reads is not a race
// with a control thread and no window draws anything anywhere.

namespace g13 {

namespace pc = cascade::core::patch;
using Engine = cascade::engine::Engine;

// Submits a graph command without running a pass; the ticket (0: not taken).
uint64_t submitGraph(testing::Session& s, const FoxCommand& c, const std::string& text) {
    FoxSubmitResult sr{};
    sr.structSize = sizeof(sr);
    const int32_t n = api()->submit_text(s.raw(), &c, text.data(), static_cast<uint32_t>(text.size()), &sr);
    return (n < 0 || sr.status != FOXAPI_OK) ? 0u : sr.ticket;
}

// The answer to a ticket, if it has landed.
bool answerOf(testing::Session& s, uint64_t ticket, FoxCommandResult& out) {
    FoxCommandResult r[16];
    for (auto& x : r) { x.structSize = sizeof(x); }
    const int32_t got = api()->poll_results(s.raw(), r, 16);
    for (int32_t i = 0; i < got; ++i) {
        if (r[i].ticket == ticket) {
            out = r[i];
            return true;
        }
    }
    return false;
}

// The graph `g` sent as an edit from the version now published; driven from this thread.
testing::Outcome g13PumpedGraph(Engine& eng, testing::Session& s, const pc::Graph& g);

FoxCommand setGraph(long long flags, long long base) {
    FoxCommand c = appCmd(FOXAPP_OP_PATCH_SET_GRAPH, flags);
    c.ival[1] = base;
    return c;
}

FoxCommand pageOpen(int on) { return appCmd(FOXAPP_OP_SET_PATCH_PAGE_OPEN, on); }

uint64_t number(const std::string& t) { return static_cast<uint64_t>(std::strtoull(t.c_str(), nullptr, 10)); }
uint64_t versionOf(testing::Session& s) { return number(fact(s, "fact.patch.version")); }
uint64_t epochOf(testing::Session& s) { return number(fact(s, "fact.patch.epoch")); }

// A radio on the generator, a channel 50 kHz above its centre, a demodulator under the channel and a spectrum display on
// the radio, wired: a patch that can run and that no device is needed for. 2.4 MS/s whole-decimates to 48 kHz by 50.
struct Starter {
    pc::Graph g;
    pc::NodeId radio = pc::kNoNode, chan = pc::kNoNode, demod = pc::kNoNode, display = pc::kNoNode;
};

Starter starter() {
    Starter p;
    p.radio = p.g.addNode(pc::NodeKind::Radio, "Radio", pc::PortType::Iq, 40.0f, 40.0f);
    p.chan = p.g.addNode(pc::NodeKind::Channel, "Channel", pc::PortType::Iq, 420.0f, 40.0f);
    p.demod = p.g.addNode(pc::NodeKind::Demod, "Demod", pc::PortType::Iq, 420.0f, 320.0f);
    if (pc::Node* n = p.g.mutableNode(p.radio)) {
        n->device = pc::kGeneratorKey;
        n->freqHz = 145.0e6;
        n->centreChosen = true;
        n->rateHz = 2.4e6;
    }
    if (pc::Node* n = p.g.mutableNode(p.chan)) { n->freqHz = 145.05e6; }
    CHECK(p.g.connect(p.radio, 0, p.chan, 0) == pc::Connect::Ok);
    CHECK(p.g.connect(p.chan, 0, p.demod, 0) == pc::Connect::Ok);
    // (A patch with nothing to show or play is not runnable: a spectrum display on the radio gives it something to do.)
    p.display = p.g.addNode(pc::NodeKind::Display, "Spectrum", pc::PortType::Iq, 40.0f, 520.0f);
    CHECK(p.g.connect(p.radio, 0, p.display, 0) == pc::Connect::Ok);
    return p;
}

testing::Outcome g13PumpedGraph(Engine& eng, testing::Session& s, const pc::Graph& g) {
    return pumpedText(eng, s, setGraph(0, static_cast<long long>(versionOf(s))), pc::graphCommandText(g));
}

pc::Graph graphOf(testing::Session& s) {
    pc::Graph g;
    std::string why;
    CHECK(pc::graphFromCommandText(fact(s, "fact.patch.graph"), g, why));
    return g;
}

// The entry of PATCH_PLAN for a node (a zeroed item when there is none).
// A node's fields, by value: a CHECK_EQ binds references to its operands, and a field of a temporary graph is gone by then.
float xOf(testing::Session& s, pc::NodeId id) {
    const pc::Graph g = graphOf(s);
    const pc::Node* n = g.find(id);
    return n != nullptr ? n->x : -1.0f;
}
bool onOf(testing::Session& s, pc::NodeId id) {
    const pc::Graph g = graphOf(s);
    const pc::Node* n = g.find(id);
    return n != nullptr && n->on;
}
std::string nameOf(testing::Session& s, pc::NodeId id) {
    const pc::Graph g = graphOf(s);
    const pc::Node* n = g.find(id);
    return n != nullptr ? n->name : std::string("<no such node>");
}

FoxListItem planEntry(testing::Session& s, pc::NodeId id) {
    const int32_t n = count(s, FOXAPI_LIST_PATCH_PLAN);
    for (int32_t i = 0; i < n; ++i) {
        const FoxListItem it = item(s, FOXAPI_LIST_PATCH_PLAN, static_cast<uint32_t>(i));
        if (it.id == static_cast<uint64_t>(id)) { return it; }
    }
    return FoxListItem{};
}

FoxListItem radioEntry(testing::Session& s, pc::NodeId id) {
    const int32_t n = count(s, FOXAPI_LIST_PATCH_RADIOS);
    for (int32_t i = 0; i < n; ++i) {
        const FoxListItem it = item(s, FOXAPI_LIST_PATCH_RADIOS, static_cast<uint32_t>(i));
        if (it.id == static_cast<uint64_t>(id)) { return it; }
    }
    return FoxListItem{};
}

FoxListItem problemFor(testing::Session& s, pc::NodeId id, const char* code) {
    const int32_t n = count(s, FOXAPI_LIST_PATCH_PROBLEMS);
    for (int32_t i = 0; i < n; ++i) {
        const FoxListItem it = item(s, FOXAPI_LIST_PATCH_PROBLEMS, static_cast<uint32_t>(i));
        if (it.id == static_cast<uint64_t>(id) && std::string(it.name) == code) { return it; }
    }
    return FoxListItem{};
}

bool patchRunning(testing::Session& s) { return (s.state().flags & FOXAPI_RX_PATCH_RUNNING) != 0u; }

}  // namespace g13

// G13-1: the patch runtime is the pass's. No window anywhere: a document put in the engine is compiled by the pass
// (PATCH_PLAN, fact.patch.runnable, the advisory problem), START opens the generator radio in the pass (PATCH_RADIOS, its
// centre, the node's spectrum), and the flag FOXAPP_OP_SET_PATCH_PAGE_OPEN decides whether the step runs: closed, the
// patch stops (the close rule), an edit moves the version and not the plan, and open again the plan follows. The default is
// "showing" - a fresh engine compiles with nobody having said so.
void testPatchRuntimeInThePass() {
    using namespace g13;
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    passes(d.eng, 2);
    CHECK_EQ(count(s, FOXAPI_LIST_PATCH_PLAN), 0);
    CHECK_EQ(fact(s, "fact.patch.runnable"), std::string("0"));  // the empty patch has nothing to run
    CHECK_EQ(versionOf(s), 1u);
    CHECK_EQ(epochOf(s), 0u);

    const Starter p = starter();
    const std::string doc = pc::graphCommandText(p.g);
    CHECK_EQ(pumpedText(d.eng, s, setGraph(FOXAPP_PATCH_GRAPH_DOCUMENT, 0), doc).result.status, FOXAPI_OK);
    passes(d.eng, 2);
    // THE PASS COMPILED IT: one channel, 50 kHz up, 50:1 to 48 kHz, cut from the radio.
    CHECK_EQ(count(s, FOXAPI_LIST_PATCH_PLAN), 1);
    const FoxListItem ch = planEntry(s, p.chan);
    std::printf("G13-1: the pass's plan: %s offset %.0f Hz, out %.0f Hz, decimation %.0f, radio %.0f\n", ch.detail,
                ch.value[0], ch.value[1], ch.value[2], ch.value[3]);
    CHECK_EQ(std::string(ch.detail), std::string("channel"));
    CHECK_EQ(std::string(ch.name), std::string());
    CHECK_EQ(ch.value[0], 50000.0);
    CHECK_EQ(ch.value[1], 48000.0);
    CHECK_EQ(ch.value[2], 50.0);
    CHECK_EQ(ch.value[3], static_cast<double>(p.radio));
    CHECK_EQ(fact(s, "fact.patch.runnable"), std::string("1"));
    // The advisory problem keeps its published code: the demodulator's audio goes nowhere (NothingListens, 5).
    const FoxListItem nl = problemFor(s, p.demod, "5");
    CHECK_EQ(nl.id, static_cast<uint64_t>(p.demod));
    CHECK(std::string(nl.detail).size() > 0);
    CHECK(!patchRunning(s));
    CHECK_EQ(count(s, FOXAPI_LIST_PATCH_RADIOS), 0);  // nothing starts by itself

    // START: the pass opens the generator radio. Its rate, its AIR centre and its spectrum are served, nobody drew.
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_PATCH_RUN, 0, 1)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return count(s, FOXAPI_LIST_PATCH_RADIOS) == 1; }));
    const FoxListItem r = radioEntry(s, p.radio);
    CHECK_EQ(r.id, static_cast<uint64_t>(p.radio));
    CHECK((r.flags & FOXAPI_ITEM_ACTIVE) != 0u);
    CHECK((r.flags & FOXAPI_ITEM_FAULT) == 0u);
    CHECK_EQ(r.value[0], 2.4e6);
    CHECK_EQ(r.value[1], 145.0e6);  // 8.12: the open radio's AIR centre
    CHECK_EQ(r.value[2], 0.0);      // not pending: it is open
    CHECK(patchRunning(s));
    {
        FoxSpectrumInfo info{};
        info.structSize = sizeof(info);
        std::vector<float> bins(8192);
        int32_t rc = FOXAPI_NOT_FOUND;
        const bool got = passUntil(d.eng, [&] {
            info = FoxSpectrumInfo{};
            info.structSize = sizeof(info);
            rc = api()->read_node_spectrum(s.raw(), static_cast<uint32_t>(p.radio), 0, &info, bins.data(),
                                           static_cast<uint32_t>(bins.size()));
            return rc == FOXAPI_OK;
        });
        std::printf("G13-1: the running radio's spectrum, read from the pass: %s (%u bins)\n", got ? "yes" : "NO", info.copied);
        CHECK(got);
        CHECK(info.copied > 0u);
    }

    // THE PAGE CLOSES (the receiver face chosen): the close rule, in the pass - the patch stops, the radio closes.
    const uint64_t versionBefore = versionOf(s);
    CHECK_EQ(pumped(d.eng, s, pageOpen(0)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK(!patchRunning(s));
    CHECK_EQ(count(s, FOXAPI_LIST_PATCH_RADIOS), 0);
    {
        FoxSpectrumInfo info{};
        info.structSize = sizeof(info);
        float bin[16];
        CHECK_EQ(api()->read_node_spectrum(s.raw(), static_cast<uint32_t>(p.radio), 0, &info, bin, 16), FOXAPI_NOT_FOUND);
    }
    // The close rule does not edit the graph: nothing moved (the node's switch stays as it was).
    CHECK_EQ(versionOf(s), versionBefore);
    // While it is closed the step does nothing: an edit that changes the plan moves the version and not the plan.
    pc::Graph moved = graphOf(s);
    if (pc::Node* n = moved.mutableNode(p.chan)) { n->freqHz = 145.1e6; }
    const testing::Outcome edit = pumpedText(d.eng, s, setGraph(0, static_cast<long long>(versionOf(s))), pc::graphCommandText(moved));
    CHECK_EQ(edit.result.status, FOXAPI_OK);
    passes(d.eng, 2);
    CHECK_EQ(versionOf(s), versionBefore + 1u);
    CHECK_EQ(planEntry(s, p.chan).value[0], 50000.0);  // still the plan of the last pass that ran the step
    // ...and showing again, the step runs and the plan follows. Nothing restarts the patch.
    CHECK_EQ(pumped(d.eng, s, pageOpen(1)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK_EQ(planEntry(s, p.chan).value[0], 100000.0);
    CHECK(!patchRunning(s));
    CHECK_EQ(count(s, FOXAPI_LIST_PATCH_RADIOS), 0);
    // START again opens it again (the page opened and closed does not spoil the runtime).
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_PATCH_RUN, 0, 1)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return count(s, FOXAPI_LIST_PATCH_RADIOS) == 1; }));
    CHECK(patchRunning(s));
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_PATCH_RUN, 0, 0)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK_EQ(count(s, FOXAPI_LIST_PATCH_RADIOS), 0);

    // GRANTS: VIEW is the grant (and all of it); a REMOTE session is DENIED as for every FOXAPP_OP_*, and the flag is
    // left alone.
    {
        testing::Session viewOnly(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW);
        CHECK(viewOnly.ok());
        CHECK_EQ(pumped(d.eng, viewOnly, pageOpen(0)).status(), FOXAPI_OK);
        passes(d.eng, 1);
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_PATCH_RUN, 0, 1)).status(), FOXAPI_OK);
        passes(d.eng, 3);
        CHECK_EQ(count(s, FOXAPI_LIST_PATCH_RADIOS), 0);  // closed by the VIEW session: START opens nothing
        testing::Session remote(api(), d.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW, "s3cret");
        CHECK(remote.ok());
        CHECK_EQ(pumped(d.eng, remote, pageOpen(1)).status(), FOXAPI_DENIED);
        passes(d.eng, 3);
        CHECK_EQ(count(s, FOXAPI_LIST_PATCH_RADIOS), 0);  // the refusal changed nothing
        // The START sent while the page was closed is still wanted: showing again, the pass opens the radio. (As it
        // always was: closing stops a patch that is running, it does not forget a START that came after.)
        CHECK_EQ(pumped(d.eng, viewOnly, pageOpen(1)).status(), FOXAPI_OK);
        CHECK(passUntil(d.eng, [&] { return count(s, FOXAPI_LIST_PATCH_RADIOS) == 1 && patchRunning(s); }));
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_PATCH_RUN, 0, 0)).status(), FOXAPI_OK);
        passes(d.eng, 2);
    }
    std::printf("G13-1: the patch runs in the pass: plan, START, spectrum, the page closed and opened, the grants\n");
}

// G13-2: the graph's version and epoch, and a merge-safe SET_GRAPH.
void testPatchGraphVersionAndMerge() {
    using namespace g13;
    Direct d;
    testing::Session a(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    testing::Session b(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(a.ok() && b.ok());
    passes(d.eng, 2);
    const Starter p = starter();

    // THE FACTS: version 1 and epoch 0 for the empty graph; a document is a new epoch and a new version; the three
    // facts are one block (the graph's text and its version never part, whatever moved it, below).
    CHECK_EQ(versionOf(a), 1u);
    CHECK_EQ(epochOf(a), 0u);
    int32_t st = 0;
    fact(a, "fact.patch.version", &st);
    CHECK(st > 0);
    {
        testing::Session remote(api(), d.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW, "s3cret");
        CHECK(remote.ok());
        CHECK_EQ(fact(remote, "fact.patch.version"), std::string("1"));  // a counter names nothing on this machine
        fact(remote, "fact.patch.graph", &st);
        CHECK_EQ(st, FOXAPI_DENIED);                                      // the graph does
    }
    const testing::Outcome load = pumpedText(d.eng, a, setGraph(FOXAPP_PATCH_GRAPH_DOCUMENT, 0), pc::graphCommandText(p.g));
    CHECK_EQ(load.result.status, FOXAPI_OK);
    CHECK_EQ(load.result.applied[0], 2.0);  // the answer names the version after the command
    passes(d.eng, 1);
    CHECK_EQ(versionOf(a), 2u);
    CHECK_EQ(epochOf(a), 1u);
    const uint64_t v2 = versionOf(a);

    // An EDIT with the current version applies as sent (no merge), names the new version, and does not move the epoch.
    pc::Graph g1 = graphOf(a);
    if (pc::Node* n = g1.mutableNode(p.radio)) { n->x = 100.0f; }
    const testing::Outcome e1 = pumpedText(d.eng, a, setGraph(0, static_cast<long long>(v2)), pc::graphCommandText(g1));
    CHECK_EQ(e1.result.status, FOXAPI_OK);
    CHECK_EQ(e1.result.applied[0], 3.0);
    CHECK_EQ(e1.result.applied[1], 0.0);
    CHECK((e1.result.flags & FOXAPI_RESULT_CLAMPED) == 0u);
    passes(d.eng, 1);
    CHECK_EQ(versionOf(a), 3u);
    CHECK_EQ(epochOf(a), 1u);
    CHECK_EQ(xOf(a, p.radio), 100.0f);
    // The same graph again changes nothing: no version moves, and the answer says which version it is.
    const testing::Outcome same = pumpedText(d.eng, a, setGraph(0, 3), pc::graphCommandText(g1));
    CHECK_EQ(same.result.status, FOXAPI_OK);
    CHECK_EQ(same.result.applied[0], 3.0);
    passes(d.eng, 2);
    CHECK_EQ(versionOf(a), 3u);

    // THE ENGINE'S OWN CHANGES ARE VERSIONS: START throws the radio's switch on (it was switched off here), nobody sent
    // anything.
    {
        pc::Graph off = graphOf(a);
        if (pc::Node* n = off.mutableNode(p.radio)) { n->on = false; }
        CHECK_EQ(pumpedText(d.eng, a, setGraph(0, 3), pc::graphCommandText(off)).result.status, FOXAPI_OK);
        passes(d.eng, 1);
        CHECK_EQ(versionOf(a), 4u);
        CHECK(!onOf(a, p.radio));
        CHECK_EQ(pumped(d.eng, a, testing::cmd(FOXAPI_OP_PATCH_RUN, 0, 1)).status(), FOXAPI_OK);
        passes(d.eng, 1);
        CHECK(onOf(a, p.radio));  // switchOnForStart
        CHECK_EQ(versionOf(a), 5u);
        CHECK_EQ(pumped(d.eng, a, testing::cmd(FOXAPI_OP_PATCH_RUN, 0, 0)).status(), FOXAPI_OK);
        passes(d.eng, 2);
    }
    const uint64_t v = versionOf(a);

    // TWO SESSIONS RACING, in one pass: both read version v and edit different things; a's lands first, b's base is
    // then stale and is MERGED - both edits are kept, and b's answer says it was merged.
    {
        const pc::Graph base = graphOf(a);
        pc::Graph ga = base;
        if (pc::Node* n = ga.mutableNode(p.radio)) { n->x = 222.0f; n->y = 77.0f; }
        pc::Graph gb = base;
        if (pc::Node* n = gb.mutableNode(p.demod)) { n->name = "Demod B"; }
        const uint64_t ta = submitGraph(a, setGraph(0, static_cast<long long>(v)), pc::graphCommandText(ga));
        const uint64_t tb = submitGraph(b, setGraph(0, static_cast<long long>(v)), pc::graphCommandText(gb));
        CHECK(ta != 0u && tb != 0u);
        d.eng.controlPass();
        FoxCommandResult ra{}, rb{};
        CHECK(answerOf(a, ta, ra));
        CHECK(answerOf(b, tb, rb));
        CHECK_EQ(ra.status, FOXAPI_OK);
        CHECK_EQ(rb.status, FOXAPI_OK);
        CHECK_EQ(ra.applied[1], 0.0);
        CHECK_EQ(rb.applied[1], 1.0);
        CHECK((rb.flags & FOXAPI_RESULT_CLAMPED) != 0u);
        CHECK_EQ(ra.applied[0], static_cast<double>(v + 1));
        CHECK_EQ(rb.applied[0], static_cast<double>(v + 2));
        const pc::Graph now = graphOf(b);
        std::printf("G13-2: two sessions from version %llu: radio at %.0f,%.0f, demod named \"%s\", version %llu\n",
                    static_cast<unsigned long long>(v), static_cast<double>(now.find(p.radio)->x),
                    static_cast<double>(now.find(p.radio)->y), now.find(p.demod)->name.c_str(),
                    static_cast<unsigned long long>(versionOf(b)));
        CHECK_EQ(now.find(p.radio)->x, 222.0f);        // a's edit survived b's
        CHECK_EQ(now.find(p.radio)->y, 77.0f);
        CHECK_EQ(now.find(p.demod)->name, std::string("Demod B"));  // and b's landed
        CHECK_EQ(versionOf(b), v + 2);
        // The SAME race with no version (the op as it always was): b's graph REPLACES a's, and a's edit is lost.
        const pc::Graph base2 = graphOf(a);
        pc::Graph ga2 = base2;
        if (pc::Node* n = ga2.mutableNode(p.radio)) { n->x = 300.0f; }
        pc::Graph gb2 = base2;
        if (pc::Node* n = gb2.mutableNode(p.demod)) { n->name = "Demod C"; }
        const uint64_t ta2 = submitGraph(a, setGraph(0, 0), pc::graphCommandText(ga2));
        const uint64_t tb2 = submitGraph(b, setGraph(0, 0), pc::graphCommandText(gb2));
        CHECK(ta2 != 0u && tb2 != 0u);
        d.eng.controlPass();
        const pc::Graph lost = graphOf(a);
        CHECK_EQ(lost.find(p.demod)->name, std::string("Demod C"));
        CHECK_EQ(lost.find(p.radio)->x, 222.0f);  // a's 300 is gone: unconditional means "replace"
    }

    // THE LOST UPDATE THE ASYNCHRONOUS WINDOW WOULD HAVE: a rename edited from version w, and START applied in the SAME
    // pass before it. START throws the radio's switch on; the stale rename must not put it back off.
    {
        // All radios off first, so START has a switch to throw.
        pc::Graph off = graphOf(a);
        if (pc::Node* n = off.mutableNode(p.radio)) { n->on = false; }
        CHECK_EQ(pumpedText(d.eng, a, setGraph(0, static_cast<long long>(versionOf(a))), pc::graphCommandText(off)).result.status,
                 FOXAPI_OK);
        passes(d.eng, 1);
        const uint64_t w = versionOf(a);
        pc::Graph rename = graphOf(a);
        if (pc::Node* n = rename.mutableNode(p.demod)) { n->name = "Renamed"; }
        const std::string text = pc::graphCommandText(rename);
        for (int variant = 0; variant < 2; ++variant) {
            // variant 0: the rename names its base (the version it was edited from); variant 1: unconditional.
            if (variant == 1) {
                // restore the same starting point
                pc::Graph back = graphOf(a);
                if (pc::Node* n = back.mutableNode(p.radio)) { n->on = false; }
                if (pc::Node* n = back.mutableNode(p.demod)) { n->name = "Demod B"; }
                CHECK_EQ(pumpedText(d.eng, a, setGraph(0, 0), pc::graphCommandText(back)).result.status, FOXAPI_OK);
                passes(d.eng, 1);
            }
            const uint64_t base = variant == 0 ? w : 0u;
            // (START as a command in the same queue, ahead of the rename)
            FoxSubmitResult sr{};
            sr.structSize = sizeof(sr);
            const FoxCommand start = testing::cmd(FOXAPI_OP_PATCH_RUN, 0, 1);
            CHECK_EQ(api()->submit(b.raw(), &start, 1, &sr), 1);
            const uint64_t t = submitGraph(a, setGraph(0, static_cast<long long>(base)), text);
            CHECK(t != 0u);
            d.eng.controlPass();
            FoxCommandResult r{};
            CHECK(answerOf(a, t, r));
            CHECK_EQ(r.status, FOXAPI_OK);
            const pc::Graph now = graphOf(a);
            std::printf("G13-2: START and a stale rename in one pass (%s): radio %s, demod \"%s\"\n",
                        variant == 0 ? "base named" : "unconditional", now.find(p.radio)->on ? "ON" : "OFF",
                        now.find(p.demod)->name.c_str());
            CHECK_EQ(now.find(p.demod)->name, std::string("Renamed"));
            if (variant == 0) {
                CHECK(now.find(p.radio)->on);   // START's switch survived: the engine merged
                CHECK_EQ(r.applied[1], 1.0);
            } else {
                CHECK(!now.find(p.radio)->on);  // the lost update the version exists to prevent
            }
            // back to stopped for the next round
            CHECK_EQ(pumped(d.eng, b, testing::cmd(FOXAPI_OP_PATCH_RUN, 0, 0)).status(), FOXAPI_OK);
            passes(d.eng, 2);
        }
    }

    // A BASE THE ENGINE NO LONGER HOLDS is refused, and nothing changes: more than 8 versions later, a version from the
    // future, and a version from before a document. A negative base is a bad argument. A document ignores the base.
    {
        const uint64_t old = versionOf(a);
        for (int i = 0; i < 9; ++i) {
            pc::Graph g = graphOf(a);
            if (pc::Node* n = g.mutableNode(p.radio)) { n->x = 400.0f + static_cast<float>(i); }
            CHECK_EQ(pumpedText(d.eng, a, setGraph(0, static_cast<long long>(versionOf(a))), pc::graphCommandText(g)).result.status,
                     FOXAPI_OK);
            passes(d.eng, 1);
        }
        CHECK_EQ(versionOf(a), old + 9u);
        const std::string held = fact(a, "fact.patch.graph");
        pc::Graph stale = graphOf(a);
        if (pc::Node* n = stale.mutableNode(p.demod)) { n->name = "Too late"; }
        const testing::Outcome gone = pumpedText(d.eng, a, setGraph(0, static_cast<long long>(old)), pc::graphCommandText(stale));
        std::printf("G13-2: a base 9 versions old: status %d \"%s\"\n", gone.result.status, gone.result.message);
        CHECK_EQ(gone.result.status, FOXAPI_FAILED);
        CHECK(std::string(gone.result.message).find("moved on") != std::string::npos);
        CHECK_EQ(fact(a, "fact.patch.graph"), held);
        CHECK_EQ(versionOf(a), old + 9u);
        // The oldest version still held (8 back) is merged, not refused.
        const uint64_t oldest = versionOf(a) - 7u;
        const testing::Outcome edge = pumpedText(d.eng, a, setGraph(0, static_cast<long long>(oldest)), pc::graphCommandText(stale));
        CHECK_EQ(edge.result.status, FOXAPI_OK);
        CHECK_EQ(edge.result.applied[1], 1.0);
        CHECK_EQ(nameOf(a, p.demod), std::string("Too late"));
        // The future, and a negative.
        CHECK_EQ(pumpedText(d.eng, a, setGraph(0, static_cast<long long>(versionOf(a) + 50u)), pc::graphCommandText(stale)).result.status,
                 FOXAPI_FAILED);
        CHECK_EQ(pumpedText(d.eng, a, setGraph(0, -1), pc::graphCommandText(stale)).result.status, FOXAPI_BAD_ARGUMENT);
        // A text the engine cannot honour: refused whole, the version unmoved.
        const uint64_t before = versionOf(a);
        CHECK_EQ(pumpedText(d.eng, a, setGraph(0, static_cast<long long>(before)), "not a patch\n").result.status,
                 FOXAPI_BAD_ARGUMENT);
        passes(d.eng, 1);
        CHECK_EQ(versionOf(a), before);
        // A DOCUMENT ignores the base, moves the epoch, and the history goes with it: a version of the old patch is
        // refused afterwards.
        const uint64_t e0 = epochOf(a);
        const uint64_t vOld = versionOf(a);
        CHECK_EQ(pumpedText(d.eng, a, setGraph(FOXAPP_PATCH_GRAPH_DOCUMENT, 999), pc::graphCommandText(p.g)).result.status, FOXAPI_OK);
        passes(d.eng, 1);
        CHECK_EQ(epochOf(a), e0 + 1u);
        CHECK(versionOf(a) > vOld);
        CHECK_EQ(pumpedText(d.eng, a, setGraph(0, static_cast<long long>(vOld)), pc::graphCommandText(stale)).result.status,
                 FOXAPI_FAILED);
    }

    // ONE BLOCK: whatever moved the graph, the version and the text moved together and never apart. Read after every
    // pass of a mixed run (an edit, START, ALL OFF, a merge, a refusal): the version differs from the last pass's exactly
    // when the graph's text does.
    {
        std::string lastText = fact(a, "fact.patch.graph");
        uint64_t lastVersion = versionOf(a);
        int moves = 0;
        const auto step = [&](const char* what) {
            const std::string text = fact(a, "fact.patch.graph");
            const uint64_t ver = versionOf(a);
            if ((text != lastText) != (ver != lastVersion)) {
                std::printf("G13-2: version %llu -> %llu with the text %s after %s\n", static_cast<unsigned long long>(lastVersion),
                            static_cast<unsigned long long>(ver), text != lastText ? "changed" : "the same", what);
            }
            CHECK((text != lastText) == (ver != lastVersion));
            moves += ver != lastVersion ? 1 : 0;
            lastText = text;
            lastVersion = ver;
        };
        pc::Graph g = graphOf(a);
        for (int i = 0; i < 6; ++i) {
            if (pc::Node* n = g.mutableNode(p.chan)) { n->freqHz = 145.02e6 + 10000.0 * i; }
            (void)submitGraph(a, setGraph(0, static_cast<long long>(versionOf(a))), pc::graphCommandText(g));
            d.eng.controlPass();
            step("an edit");
            const FoxCommand run = testing::cmd(FOXAPI_OP_PATCH_RUN, 0, i % 2 == 0 ? 1 : 0);
            (void)api()->submit(b.raw(), &run, 1, nullptr);
            d.eng.controlPass();
            step(i % 2 == 0 ? "START" : "STOP");
            const FoxCommand allOff = testing::cmd(FOXAPI_OP_PATCH_ALL_OFF);
            (void)api()->submit(b.raw(), &allOff, 1, nullptr);
            d.eng.controlPass();
            step("ALL OFF");
            g = graphOf(a);
        }
        CHECK(moves >= 6);
    }
    // THE EVENT follows the version: ALL OFF with the radio ON and the patch stopped changes the graph and nothing else (no
    // document, the patch does not start or stop), and FOXAPI_EVENT_PATCH is raised for it; the same again changes nothing
    // and raises nothing. (It used to follow a hash of the graph's fields; now it follows the version, whoever moved it.)
    {
        pc::Graph on = graphOf(a);
        if (pc::Node* n = on.mutableNode(p.radio)) { n->on = true; }
        CHECK_EQ(g13PumpedGraph(d.eng, a, on).result.status, FOXAPI_OK);
        passes(d.eng, 2);
        CHECK(!patchRunning(a));
        CHECK_EQ(api()->subscribe(b.raw(), FOXAPI_EVENT_MASK(FOXAPI_EVENT_PATCH)), FOXAPI_OK);
        passes(d.eng, 2);
        (void)takeEvents(b);
        const uint64_t v0 = versionOf(a);
        CHECK_EQ(pumped(d.eng, a, testing::cmd(FOXAPI_OP_PATCH_ALL_OFF)).status(), FOXAPI_OK);
        passes(d.eng, 2);
        int patchEvents = 0;
        for (const FoxEvent& e : takeEvents(b)) { patchEvents += e.kind == FOXAPI_EVENT_PATCH ? 1 : 0; }
        std::printf("G13-2: ALL OFF with the radio on: version %llu -> %llu, %d patch event(s)\n", static_cast<unsigned long long>(v0),
                    static_cast<unsigned long long>(versionOf(a)), patchEvents);
        CHECK_EQ(versionOf(a), v0 + 1u);
        CHECK(patchEvents >= 1);
        CHECK_EQ(pumped(d.eng, a, testing::cmd(FOXAPI_OP_PATCH_ALL_OFF)).status(), FOXAPI_OK);  // already off: no change
        passes(d.eng, 2);
        patchEvents = 0;
        for (const FoxEvent& e : takeEvents(b)) { patchEvents += e.kind == FOXAPI_EVENT_PATCH ? 1 : 0; }
        CHECK_EQ(patchEvents, 0);
        CHECK_EQ(versionOf(a), v0 + 1u);
    }
    std::printf("G13-2: the version, the epoch, the merge, the refusals and the one block: done\n");
}

// G13-3: the plan list (channels and decoders, against two REAL modules), fact.patch.runnable with the two marks only an
// attempt can find (a radio that would not open clears it, a plugin that refused to start does not), PATCH_RADIOS' note
// and pending entry, the problems' published codes, and the parts bin hashed by content.
void testPatchPlanList() {
    using namespace g13;
    PluginWorld world;
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    passes(d.eng, 3);
    using Access = cascade::gui::AppWindowTestAccess;

    // THE CODES, pinned: the table section 8.12 prints.
    CHECK_EQ(static_cast<int>(pc::Problem::NothingFeedsIt), 0);
    CHECK_EQ(static_cast<int>(pc::Problem::NotFedByARadio), 1);
    CHECK_EQ(static_cast<int>(pc::Problem::NoFrequency), 2);
    CHECK_EQ(static_cast<int>(pc::Problem::OutOfBand), 3);
    CHECK_EQ(static_cast<int>(pc::Problem::RateUnreachable), 4);
    CHECK_EQ(static_cast<int>(pc::Problem::NothingListens), 5);
    CHECK_EQ(static_cast<int>(pc::Problem::NoPlugin), 6);
    CHECK_EQ(static_cast<int>(pc::Problem::PluginMissing), 7);
    CHECK_EQ(static_cast<int>(pc::Problem::PluginWrongFeed), 8);
    CHECK_EQ(static_cast<int>(pc::Problem::DecoderTooFast), 9);
    CHECK_EQ(static_cast<int>(pc::Problem::PluginRefused), 10);
    CHECK_EQ(static_cast<int>(pc::Problem::NoDevice), 11);
    CHECK_EQ(static_cast<int>(pc::Problem::DeviceTwice), 12);
    CHECK_EQ(static_cast<int>(pc::Problem::RadioFailed), 13);
    CHECK_EQ(static_cast<int>(pc::Problem::NoTracks), 14);
    CHECK(pc::isAdvisory(pc::Problem::NothingListens) && pc::isAdvisory(pc::Problem::NoTracks));

    // THE PARTS BIN, from the pass: the two modules, the audio decoder and the I/Q decoder, as the catalogue lists them.
    CHECK_EQ(count(s, FOXAPI_LIST_PATCH_PARTS), 2);
    std::string keyIq, keyAudio;
    std::vector<pc::DecoderInfo> catalogue;
    for (int i = 0; i < 2; ++i) {
        const FoxListItem it = item(s, FOXAPI_LIST_PATCH_PARTS, static_cast<uint32_t>(i));
        pc::DecoderInfo info;
        info.key = it.detail;
        info.name = it.name;
        info.feed = it.value[0] == static_cast<double>(pc::PortType::Iq) ? pc::PortType::Iq : pc::PortType::Audio;
        info.requiredRateHz = it.value[1];
        catalogue.push_back(info);
        (info.feed == pc::PortType::Iq ? keyIq : keyAudio) = info.key;
    }
    CHECK(!keyIq.empty() && !keyAudio.empty());

    // A patch with a channel and its demodulator, an audio decoder behind it, an I/Q decoder on the radio and another on
    // the channel.
    Starter p = starter();
    const pc::NodeId audioDec = p.g.addNode(pc::NodeKind::Decoder, "Audio decoder", pc::PortType::Audio, 700.0f, 320.0f);
    const pc::NodeId radioDec = p.g.addNode(pc::NodeKind::Decoder, "IQ decoder on the radio", pc::PortType::Iq, 420.0f, 520.0f);
    const pc::NodeId chanDec = p.g.addNode(pc::NodeKind::Decoder, "IQ decoder on the channel", pc::PortType::Iq, 700.0f, 40.0f);
    if (pc::Node* n = p.g.mutableNode(audioDec)) { n->plugin = keyAudio; }
    if (pc::Node* n = p.g.mutableNode(radioDec)) { n->plugin = keyIq; }
    if (pc::Node* n = p.g.mutableNode(chanDec)) { n->plugin = keyIq; }
    CHECK(p.g.connect(p.demod, 0, audioDec, 0) == pc::Connect::Ok);
    CHECK(p.g.connect(p.radio, 0, radioDec, 0) == pc::Connect::Ok);
    CHECK(p.g.connect(p.chan, 0, chanDec, 0) == pc::Connect::Ok);
    CHECK_EQ(pumpedText(d.eng, s, setGraph(FOXAPP_PATCH_GRAPH_DOCUMENT, 0), pc::graphCommandText(p.g)).result.status, FOXAPI_OK);
    passes(d.eng, 2);

    // What the core's compile says of the same graph, the same radio (not yet open: its node's own settings) and the same
    // catalogue: the list is held to it, entry by entry.
    const pc::Plan want = pc::compile(p.g, {{p.radio, 2.4e6, 145.0e6}}, &catalogue);
    CHECK_EQ(want.channels.size(), 1u);
    CHECK_EQ(want.decoders.size(), 3u);
    CHECK_EQ(count(s, FOXAPI_LIST_PATCH_PLAN), static_cast<int32_t>(want.channels.size() + want.decoders.size()));
    {
        const FoxListItem c = planEntry(s, p.chan);
        CHECK_EQ(c.value[0], want.channels[0].offsetHz);
        CHECK_EQ(c.value[1], want.channels[0].outRateHz);
        CHECK_EQ(c.value[2], static_cast<double>(want.channels[0].decimation));
        CHECK_EQ(c.value[3], static_cast<double>(want.channels[0].radio));
    }
    int seenSources = 0;
    for (const pc::DecoderPlan& dp : want.decoders) {
        const FoxListItem e = planEntry(s, dp.node);
        const char* source = dp.source == pc::DecoderSource::Radio     ? "decoder:radio"
                             : dp.source == pc::DecoderSource::Channel ? "decoder:channel"
                                                                       : "decoder:audio";
        std::printf("G13-3: decoder %u %s: %s, rate %.0f from %.0f, centre %.0f, radio %.0f\n", static_cast<unsigned>(dp.node),
                    e.name, e.detail, e.value[0], e.value[1], e.value[2], e.value[3]);
        CHECK_EQ(std::string(e.detail), std::string(source));
        CHECK_EQ(std::string(e.name), catalogue[dp.plugin].key);
        CHECK_EQ(e.value[0], dp.rateHz);
        CHECK_EQ(e.value[1], dp.inRateHz);
        CHECK_EQ(e.value[2], dp.centreHz);
        CHECK_EQ(e.value[3], static_cast<double>(dp.radio));
        ++seenSources;
    }
    CHECK_EQ(seenSources, 3);
    // The three sources are three different strings (a swap of two would not be caught by the loop above alone).
    CHECK_EQ(std::string(planEntry(s, audioDec).detail), std::string("decoder:audio"));
    CHECK_EQ(std::string(planEntry(s, radioDec).detail), std::string("decoder:radio"));
    CHECK_EQ(std::string(planEntry(s, chanDec).detail), std::string("decoder:channel"));
    CHECK_EQ(fact(s, "fact.patch.runnable"), std::string(want.runnable ? "1" : "0"));
    CHECK(want.runnable);

    // THE TWO MARKS ONLY AN ATTEMPT CAN FIND. A plugin that refused to start is a problem and does NOT clear runnable.
    {
        Access::injectRefused(d.eng, p.radio, {chanDec});
        passes(d.eng, 3);
        const FoxListItem refusedProblem = problemFor(s, chanDec, "10");
        CHECK_EQ(refusedProblem.id, static_cast<uint64_t>(chanDec));
        CHECK_EQ(fact(s, "fact.patch.runnable"), std::string("1"));
    }
    // A radio that would not open is a problem and DOES: an I/Q file that is not there, opened by START in the pass.
    {
        pc::Graph g = graphOf(s);
        if (pc::Node* n = g.mutableNode(p.radio)) { n->device = pc::makeIqFileKey("Z:/no/such/recording.wav"); }
        CHECK_EQ(pumpedText(d.eng, s, setGraph(0, static_cast<long long>(versionOf(s))), pc::graphCommandText(g)).result.status, FOXAPI_OK);
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_PATCH_RUN, 0, 1)).status(), FOXAPI_OK);
        // (The recording's header is read on the open worker since stub campaign B19, so the failure lands on a later pass: wait for it by time, not by a count of
        // passes - a pass takes microseconds and a worker's thread has to start. The bound is a minute because "Z:" is a drive that is NOT THERE only on a clean
        // machine: where it is a mapped share that is offline, the first open waits out the network's own 21 s timeout - which this case used to wait out on the
        // pass itself, and which is exactly the slow disk the worker exists for.)
        CHECK(passesUntil(d.eng, [&] { return fact(s, "fact.patch.runnable") == "0"; }, 60000));
        const FoxListItem failed = problemFor(s, p.radio, "13");
        CHECK_EQ(failed.id, static_cast<uint64_t>(p.radio));
        // The radio is listed as what it is: not open, faulted, and why.
        const FoxListItem re = radioEntry(s, p.radio);
        CHECK_EQ(re.id, static_cast<uint64_t>(p.radio));
        CHECK((re.flags & FOXAPI_ITEM_ACTIVE) == 0u);
        CHECK((re.flags & FOXAPI_ITEM_FAULT) != 0u);
        CHECK(std::string(re.detail).size() > 0);
        std::printf("G13-3: a radio that would not open: runnable 0, problem 13, detail \"%s\"\n", re.detail);
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_PATCH_RUN, 0, 0)).status(), FOXAPI_OK);
        // back to the generator for the rest
        pc::Graph back = graphOf(s);
        if (pc::Node* n = back.mutableNode(p.radio)) { n->device = pc::kGeneratorKey; }
        CHECK_EQ(pumpedText(d.eng, s, setGraph(0, static_cast<long long>(versionOf(s))), pc::graphCommandText(back)).result.status, FOXAPI_OK);
        passes(d.eng, 2);
        CHECK_EQ(fact(s, "fact.patch.runnable"), std::string("1"));
    }

    // PATCH_RADIOS: the note a RUNNING radio's face prints, and a pending open.
    {
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_PATCH_RUN, 0, 1)).status(), FOXAPI_OK);
        CHECK(passUntil(d.eng, [&] { return count(s, FOXAPI_LIST_PATCH_RADIOS) == 1 && radioEntry(s, p.radio).value[0] > 0.0; }));
        FoxListItem re = radioEntry(s, p.radio);
        CHECK((re.flags & FOXAPI_ITEM_ACTIVE) != 0u && (re.flags & FOXAPI_ITEM_FAULT) == 0u);
        CHECK_EQ(std::string(re.detail), std::string());
        const std::string note = "the radio refused 2.4 MS/s and runs at its own rate";
        Access::injectRadioNote(d.eng, p.radio, note);
        passes(d.eng, 2);
        re = radioEntry(s, p.radio);
        CHECK((re.flags & FOXAPI_ITEM_ACTIVE) != 0u);
        CHECK((re.flags & FOXAPI_ITEM_FAULT) == 0u);   // a note is not a fault
        CHECK_EQ(std::string(re.detail), note);
        // A second Radio whose open is on its worker: listed, pending, no flags, its device label.
        Access::PendingOpen held;
        const pc::NodeId second = [&] {
            pc::Graph g = graphOf(s);
            const pc::NodeId id = g.addNode(pc::NodeKind::Radio, "Radio 2", pc::PortType::Iq, 40.0f, 320.0f);
            if (pc::Node* n = g.mutableNode(id)) { n->device = pc::kGeneratorKey; n->on = false; }
            CHECK_EQ(pumpedText(d.eng, s, setGraph(0, static_cast<long long>(versionOf(s))), pc::graphCommandText(g)).result.status,
                     FOXAPI_OK);
            return id;
        }();
        Access::injectPending(d.eng, second, held);
        passes(d.eng, 2);
        const FoxListItem pe = radioEntry(s, second);
        CHECK_EQ(pe.id, static_cast<uint64_t>(second));
        CHECK_EQ(pe.value[2], 1.0);
        CHECK((pe.flags & (FOXAPI_ITEM_ACTIVE | FOXAPI_ITEM_FAULT)) == 0u);
        CHECK_EQ(count(s, FOXAPI_LIST_PATCH_RADIOS), 2);
        // The open answers (it failed, with a null source): pending clears. Fulfilled BEFORE the engine goes.
        Access::answerPending(held);
        passes(d.eng, 3);
        CHECK(radioEntry(s, second).value[2] == 0.0);
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_PATCH_RUN, 0, 0)).status(), FOXAPI_OK);
        passes(d.eng, 2);
    }
    std::printf("G13-3: the plan, the codes, the two marks, the note and the pending open: done\n");
}

// G13-4: the window's looking order is held by the engine - no pass of a long mixed run publishes a graph whose text does
// not match its version, and a settled engine's passes move no version.
void testPatchSettledMovesNothing() {
    using namespace g13;
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    const Starter p = starter();
    CHECK_EQ(pumpedText(d.eng, s, setGraph(FOXAPP_PATCH_GRAPH_DOCUMENT, 0), pc::graphCommandText(p.g)).result.status, FOXAPI_OK);
    passes(d.eng, 3);
    const uint64_t listSeq = s.state().listSeq;
    const uint64_t version = versionOf(s);
    passes(d.eng, 30);
    CHECK_EQ(versionOf(s), version);
    CHECK_EQ(s.state().listSeq, listSeq);  // a settled patch page rebuilds nothing
    std::printf("G13-4: 30 settled passes with the page open: version %llu, listSeq unmoved\n", static_cast<unsigned long long>(version));
}
// G13 TESTS END>>
// <<G13B TESTS BEGIN
// G13b: the Patch page, part 2 (window port group 13b, API 0.3 section 8.12): the parts bin's track bit, PATCH_DEVICES and
// the receiver's radio, the converter in force for each Radio node, PATCH_LIVE (the running nodes' readings, taken once a
// pass, in a block of its own), DECODER_LINES' originating node, and FOXAPP_OP_PATCH_LIST_RECORDINGS. As in G13, every case
// drives the engine from THIS thread against the built-in generator or the fake radio, and no window draws anything.

namespace g13b {

namespace pc = cascade::core::patch;
using Access = cascade::gui::AppWindowTestAccess;

void put16(std::ofstream& f, uint16_t v) {
    const char b[2] = {static_cast<char>(v & 0xFF), static_cast<char>(v >> 8)};
    f.write(b, 2);
}
void put32(std::ofstream& f, uint32_t v) {
    const char b[4] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF), static_cast<char>((v >> 16) & 0xFF),
                       static_cast<char>(v >> 24)};
    f.write(b, 4);
}

// A stereo PCM16 I/Q recording of one tone, `rate` Hz, a tenth of a second: the smallest file listIqRecordings plays.
void writeIqWav(const std::filesystem::path& p, uint32_t rate) {
    const uint32_t frames = rate / 10u;
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write("RIFF", 4);
    put32(f, 36 + frames * 4u);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    put32(f, 16);
    put16(f, 1);
    put16(f, 2);
    put32(f, rate);
    put32(f, rate * 4u);
    put16(f, 4);
    put16(f, 16);
    f.write("data", 4);
    put32(f, frames * 4u);
    for (uint32_t i = 0; i < frames; ++i) {
        const double ph = 6.283185307179586 * 1000.0 * static_cast<double>(i) / rate;
        put16(f, static_cast<uint16_t>(static_cast<int16_t>(0.5 * std::cos(ph) * 32767.0)));
        put16(f, static_cast<uint16_t>(static_cast<int16_t>(0.5 * std::sin(ph) * 32767.0)));
    }
}

std::filesystem::path scratchDir(const char* name) {
    const char* base = std::getenv("LOCALAPPDATA");
    std::filesystem::path p = std::filesystem::path(base != nullptr ? base : ".") / name;
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p, ec);
    return p;
}

// The PATCH_LIVE row of `kind` for `node`: a zeroed item (id 0) when there is none.
FoxListItem liveRow(testing::Session& s, int kind, uint64_t node) {
    const int32_t n = count(s, FOXAPI_LIST_PATCH_LIVE);
    for (int32_t i = 0; i < n; ++i) {
        const FoxListItem it = item(s, FOXAPI_LIST_PATCH_LIVE, static_cast<uint32_t>(i));
        if (it.id == node && static_cast<int>(it.value[2]) == kind) { return it; }
    }
    return FoxListItem{};
}

FoxListItem deviceNamed(testing::Session& s, const std::string& key) {
    const int32_t n = count(s, FOXAPI_LIST_PATCH_DEVICES);
    for (int32_t i = 0; i < n; ++i) {
        const FoxListItem it = item(s, FOXAPI_LIST_PATCH_DEVICES, static_cast<uint32_t>(i));
        if (key == it.detail) { return it; }
    }
    return FoxListItem{};
}

}  // namespace g13b

// --- P: THE ENGINE'S DISK WORKERS (stub campaign B19; docs/api-0.3-additions.md 9, docs/engine-stage4.md 6.22) ---------------------------------
//
// The I/Q file's open, the frequency list's import and export and the patch Radio's recordings list were the pass's own calls into a disk that can be a
// network share. They are workers' (src/engine/engine_disk_work.cpp), and a command's answer is that the work is under way; what it found is the note
// and fact.bookmarks.*, fact.source.iqOpening, and the lists, a pass or more later. Each case drives the engine from THIS thread (controlPass until the thing
// has happened) and binds the slow-disk seam where it needs a worker to be out for a while.

std::string exportedCsvNamed(const std::string& dir, const std::string& prefix, std::vector<std::filesystem::path>* all = nullptr) {
    std::string text;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        const std::string n = e.path().filename().string();
        if (n.rfind(prefix, 0) == 0 && e.path().extension() == ".csv") {
            text = slurp(e.path());
            if (all != nullptr) { all->push_back(e.path()); }
        }
    }
    return text;
}

// P1: FOXAPP_OP_BOOKMARK_EXPORT ival[0] 1 - one preset's rows as a CSV, written from the store (every name whole), on a worker; the Bookmarks note is left alone and
// the result is fact.bookmarks.export.*; the refusals; a second export while one is out.
void testPresetExport() {
    using Access = cascade::gui::AppWindowTestAccess;
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    testing::Session remote(api(), d.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS, "s3cret");
    CHECK(remote.ok());
    d.eng.controlPass();
    const std::string dir = (std::filesystem::path(fact(s, "fact.recorder.dir")).parent_path() / "preset_export_dir").string();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    Access::setRecordDir(d.eng, dir);

    const std::string longName(200, 'n');      // the table's item holds 95
    const std::string longGroup(180, 'g');     // ...and 159
    const auto add = [&](const std::string& name, double hz, const char* mode, const std::string& group, bool fav, bool tick) {
        cascade::core::Bookmark b;
        b.name = name;
        b.freqHz = hz;
        b.mode = mode;
        b.bandwidthHz = cascade::core::defaultBandwidthForMode(b.mode);
        b.group = group;
        b.favourite = fav;
        b.scan = tick;
        Access::store(d.eng).add(std::move(b));
    };
    add("Ch 16, \"north\"", 156.8e6, "NFM", "Harbour", true, true);
    add(longName, 156.65e6, "AM", "Harbour", false, false);
    add("Elsewhere", 118.7e6, "AM", "Other", true, true);
    add("Long group row", 121.5e6, "AM", longGroup, true, false);
    d.eng.controlPass();

    std::vector<cascade::core::Bookmark> want;
    for (const cascade::core::Bookmark& b : Access::store(d.eng).list()) {
        if (b.group == "Harbour") { want.push_back(b); }
    }
    CHECK_EQ(want.size(), 2u);

    // THE PRESET'S ROWS, WHOLE. The note of the Bookmarks section stays as it was; the result is the facts.
    const std::string noteBefore = Access::note(d.eng);
    FoxCommand c = testing::cmd(FOXAPP_OP_BOOKMARK_EXPORT);
    c.ival[0] = 1;
    const std::uint64_t seq0 = Access::exportSeq(d.eng);
    CHECK_EQ(pumpedText(d.eng, s, c, "Harbour").status(), FOXAPI_OK);
    CHECK(passesUntil(d.eng, [&] { return Access::exportSeq(d.eng) > seq0; }));
    std::vector<std::filesystem::path> files;
    const std::string text = exportedCsvNamed(dir, "foxsdr-Harbour-", &files);
    CHECK_EQ(files.size(), 1u);
    CHECK_EQ(text, cascade::core::exportCsv(want));
    CHECK(text.find(longName) != std::string::npos);   // not cut at 95
    CHECK(text.find("Elsewhere") == std::string::npos);
    CHECK_EQ(Access::note(d.eng), noteBefore);         // a preset's export does not touch the Bookmarks note
    CHECK_EQ(fact(s, "fact.bookmarks.exporting"), "0");
    CHECK_EQ(fact(s, "fact.bookmarks.export.seq"), std::to_string(seq0 + 1u));
    CHECK_EQ(fact(s, "fact.bookmarks.export.ok"), "1");
    CHECK_EQ(fact(s, "fact.bookmarks.export.preset"), "1");
    CHECK_EQ(fact(s, "fact.bookmarks.export.count"), "2");
    if (files.size() == 1u) { CHECK_EQ(fact(s, "fact.bookmarks.export.path"), dir + "/" + files[0].filename().string()); }
    // The file reads back whole, with the tick and the favourite.
    const cascade::core::ImportResult back = cascade::core::importFrequencyList(text);
    CHECK_EQ(back.items.size(), 2u);
    for (const cascade::core::Bookmark& b : back.items) {
        const cascade::core::Bookmark* w = nullptr;
        for (const cascade::core::Bookmark& x : want) { w = x.name == b.name ? &x : w; }
        CHECK(w != nullptr);
        if (w != nullptr) { CHECK(w->favourite == b.favourite && w->scan == b.scan && w->group == b.group && w->freqHz == b.freqHz); }
    }
    // A group with a name a file name cannot carry: '_' for each such byte, and a long group cut at 40.
    {
        const std::uint64_t before = Access::exportSeq(d.eng);
        CHECK_EQ(pumpedText(d.eng, s, c, longGroup).status(), FOXAPI_OK);
        CHECK(passesUntil(d.eng, [&] { return Access::exportSeq(d.eng) > before; }));
        const std::string name = std::filesystem::path(Access::lastExportPath(d.eng)).filename().string();
        CHECK_EQ(name.rfind("foxsdr-" + std::string(40, 'g') + "-", 0), 0u);
    }

    // THE REFUSALS. An empty name, a preset with no rows, an ival[0] nobody defined, a REMOTE session: nothing is written and no worker is started.
    {
        const std::uint64_t before = Access::exportSeq(d.eng);
        CHECK_EQ(pumpedText(d.eng, s, c, "").status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(pumpedText(d.eng, s, c, "No such preset").status(), FOXAPI_NOT_FOUND);
        FoxCommand bad = testing::cmd(FOXAPP_OP_BOOKMARK_EXPORT);
        bad.ival[0] = 2;
        CHECK_EQ(pumpedText(d.eng, s, bad, "Harbour").status(), FOXAPI_OUT_OF_RANGE);
        CHECK_EQ(pumpedText(d.eng, remote, c, "Harbour").status(), FOXAPI_DENIED);
        passes(d.eng, 3);
        CHECK_EQ(Access::exportSeq(d.eng), before);
        CHECK(!Access::exportPending(d.eng));
    }

    // ONE EXPORT IS EVER OUT: a second while the first is being written is NO_CHANGE ("an export is already being written"), and the first still lands.
    {
        std::atomic<int> calls{0};
        std::atomic<bool> release{false};
        Access::diskHook(d.eng, [&] {
            calls.fetch_add(1);
            for (int i = 0; i < 5000 && !release.load(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        });
        const std::uint64_t before = Access::exportSeq(d.eng);
        CHECK_EQ(pumpedText(d.eng, s, c, "Harbour").status(), FOXAPI_OK);
        CHECK(Access::exportPending(d.eng));
        CHECK_EQ(fact(s, "fact.bookmarks.exporting"), "1");
        const testing::Outcome second = pumpedText(d.eng, s, c, "Harbour");
        CHECK_EQ(second.status(), FOXAPI_NO_CHANGE);
        CHECK(std::string(second.result.message).find("already being written") != std::string::npos);
        // The pass does not wait for the worker: passes run while it is out.
        passes(d.eng, 5);
        CHECK(Access::exportPending(d.eng));
        release.store(true);
        CHECK(passesUntil(d.eng, [&] { return Access::exportSeq(d.eng) > before; }));
        CHECK_EQ(calls.load(), 1);
        Access::diskHook(d.eng, nullptr);
    }
    // A worker that THROWS still answers, and says which kind it was: the preset's, not the Bookmarks section's.
    {
        Access::diskHook(d.eng, [] { throw std::runtime_error("the disk threw"); });
        const std::string note = Access::note(d.eng);
        const std::uint64_t before = Access::exportSeq(d.eng);
        CHECK_EQ(pumpedText(d.eng, s, c, "Harbour").status(), FOXAPI_OK);
        CHECK(passesUntil(d.eng, [&] { return Access::exportSeq(d.eng) > before; }));
        CHECK_EQ(fact(s, "fact.bookmarks.export.ok"), "0");
        CHECK_EQ(fact(s, "fact.bookmarks.export.preset"), "1");
        CHECK_EQ(Access::note(d.eng), note);   // not under the Bookmarks section
        Access::diskHook(d.eng, nullptr);
    }
    std::filesystem::remove_all(dir, ec);
    std::printf("P1: a preset's rows exported whole, on a worker; the refusals; one export out at a time\n");
}

// P2: FOXAPP_OP_BOOKMARK_IMPORT_FILE reads on a worker: the answer is OK once the read is under way; the rows, the note and fact.bookmarks.import.* come when it is
// back; a read asked for while one is out is remembered (the last wins); a file that is not there says so in the note and the fact.
void testImportOnAWorker() {
    using Access = cascade::gui::AppWindowTestAccess;
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    d.eng.controlPass();
    const std::filesystem::path dir = std::filesystem::path(fact(s, "fact.recorder.dir")).parent_path() / "import_worker_dir";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const auto csv = [&](const char* tag, int n, double firstMhz) {
        const std::filesystem::path p = dir / (std::string(tag) + ".csv");
        std::ofstream f(p, std::ios::binary);
        f << "frequency,name,mode\n";
        for (int i = 0; i < n; ++i) { f << static_cast<long long>((firstMhz + 1.1 * i) * 1.0e6) << ",Imp " << tag << " " << i << ",NFM\n"; }
        return p.string();
    };
    std::atomic<int> calls{0};
    std::atomic<bool> release{false};
    Access::diskHook(d.eng, [&] {
        calls.fetch_add(1);
        for (int i = 0; i < 5000 && !release.load(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    });
    // Three asked for at once: the first runs, the second is remembered and replaced by the third.
    const std::uint64_t seq0 = Access::importSeq(d.eng);
    CHECK_EQ(pumpedText(d.eng, s, testing::cmd(FOXAPP_OP_BOOKMARK_IMPORT_FILE), csv("a", 2, 100.0)).status(), FOXAPI_OK);
    CHECK(Access::importPending(d.eng));
    CHECK_EQ(fact(s, "fact.bookmarks.importing"), "1");
    CHECK_EQ(pumpedText(d.eng, s, testing::cmd(FOXAPP_OP_BOOKMARK_IMPORT_FILE), csv("b", 2, 120.0)).status(), FOXAPI_OK);
    CHECK_EQ(pumpedText(d.eng, s, testing::cmd(FOXAPP_OP_BOOKMARK_IMPORT_FILE), csv("c", 3, 140.0)).status(), FOXAPI_OK);
    // THE PASS DOES NOT WAIT: nothing has been read, and passes go on.
    passes(d.eng, 5);
    CHECK_EQ(Access::store(d.eng).list().size(), 0u);
    CHECK_EQ(Access::importSeq(d.eng), seq0);
    release.store(true);
    CHECK(passesUntil(d.eng, [&] { return Access::importSeq(d.eng) >= seq0 + 2u && !Access::importPending(d.eng); }));
    CHECK_EQ(calls.load(), 2);
    CHECK_EQ(Access::store(d.eng).list().size(), 5u);   // a's two and c's three
    CHECK_EQ(fact(s, "fact.bookmarks.import.ok"), "1");
    CHECK_EQ(fact(s, "fact.bookmarks.import.note"), "CSV: 3 entries read, 3 added");
    CHECK_EQ(fact(s, "fact.bookmarks.importing"), "0");
    CHECK_EQ(fact(s, "fact.bookmarks.import.path"), (dir / "c.csv").string());
    Access::diskHook(d.eng, nullptr);

    // A file that is not there: the op is OK (the read is under way), the note and the fact carry the sentence, the list is unchanged.
    {
        const std::uint64_t before = Access::importSeq(d.eng);
        const std::string missing = (dir / "no-such-list.csv").string();
        CHECK_EQ(pumpedText(d.eng, s, testing::cmd(FOXAPP_OP_BOOKMARK_IMPORT_FILE), missing).status(), FOXAPI_OK);
        CHECK(passesUntil(d.eng, [&] { return Access::importSeq(d.eng) > before; }));
        CHECK_EQ(fact(s, "fact.bookmarks.import.ok"), "0");
        CHECK_EQ(fact(s, "fact.bookmarks.import.note"), "Could not import: the file \"" + missing + "\" could not be opened");
        CHECK_EQ(Access::note(d.eng), fact(s, "fact.bookmarks.import.note"));
        CHECK_EQ(Access::store(d.eng).list().size(), 5u);
    }
    // No path at all is refused at once.
    CHECK_EQ(pumpedText(d.eng, s, testing::cmd(FOXAPP_OP_BOOKMARK_IMPORT_FILE), "").status(), FOXAPI_BAD_ARGUMENT);
    // A worker that throws names the file all the same.
    {
        Access::diskHook(d.eng, [] { throw std::runtime_error("the disk threw"); });
        const std::uint64_t before = Access::importSeq(d.eng);
        const std::string path = csv("thrown", 2, 160.0);
        CHECK_EQ(pumpedText(d.eng, s, testing::cmd(FOXAPP_OP_BOOKMARK_IMPORT_FILE), path).status(), FOXAPI_OK);
        CHECK(passesUntil(d.eng, [&] { return Access::importSeq(d.eng) > before; }));
        CHECK_EQ(fact(s, "fact.bookmarks.import.note"), "Could not import: the file \"" + path + "\" could not be read");
        CHECK_EQ(Access::store(d.eng).list().size(), 5u);
        Access::diskHook(d.eng, nullptr);
    }
    std::filesystem::remove_all(dir, ec);
    std::printf("P2: the import reads on a worker; the last asked for wins; the outcome is the facts\n");
}

// P3: FOXAPP_OP_BOOKMARK_ADD_ROWS' fourth field is a word of flags - bit 0 ticked, bit 1 favourite - and "0" and "1" mean what they always meant.
void testAddRowsFlags() {
    using Access = cascade::gui::AppWindowTestAccess;
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    d.eng.controlPass();
    FoxCommand c = testing::cmd(FOXAPP_OP_BOOKMARK_ADD_ROWS);
    c.ival[0] = 1;   // a list
    const std::string rows = "121.5\tAM\t10000\t0\tG\tplain\n"
                             "121.6\tAM\t10000\t1\tG\tticked\n"
                             "121.7\tAM\t10000\t2\tG\tstarred\n"
                             "121.8\tAM\t10000\t3\tG\tboth\n"
                             "121.9\tAM\t10000\t7\tG\tunknown flags\n"
                             "122.0\tAM\t10000\tx\tG\tnot a number\n";
    CHECK_EQ(pumpedText(d.eng, s, c, rows).status(), FOXAPI_OK);
    const auto row = [&](const char* name) -> const cascade::core::Bookmark* {
        for (const cascade::core::Bookmark& b : Access::store(d.eng).list()) {
            if (b.name == name) { return &b; }
        }
        return nullptr;
    };
    const cascade::core::Bookmark* plain = row("plain");
    const cascade::core::Bookmark* ticked = row("ticked");
    const cascade::core::Bookmark* starred = row("starred");
    const cascade::core::Bookmark* both = row("both");
    CHECK(plain != nullptr && ticked != nullptr && starred != nullptr && both != nullptr);
    if (plain != nullptr && ticked != nullptr && starred != nullptr && both != nullptr) {
        CHECK(!plain->scan && !plain->favourite);
        CHECK(ticked->scan && !ticked->favourite);
        CHECK(!starred->scan && starred->favourite);
        CHECK(both->scan && both->favourite);
    }
    // A word that is no flag set (7, "x") is no flags: neither tick nor star.
    const cascade::core::Bookmark* odd = row("unknown flags");
    const cascade::core::Bookmark* nan = row("not a number");
    CHECK(odd != nullptr && nan != nullptr);
    if (odd != nullptr && nan != nullptr) { CHECK(!odd->scan && !odd->favourite && !nan->scan && !nan->favourite); }
    // The table says it: a row's FAVOURITE and TICKED flags.
    passes(d.eng, 2);
    bool starFlag = false;
    const int32_t n = count(s, FOXAPI_LIST_BOOKMARKS);
    for (int32_t i = 0; i < n; ++i) {
        const FoxListItem it = item(s, FOXAPI_LIST_BOOKMARKS, static_cast<uint32_t>(i));
        if (std::string(it.name) == "starred") { starFlag = (it.flags & FOXAPI_ITEM_FAVOURITE) != 0u; }
    }
    CHECK(starFlag);
    std::printf("P3: ADD_ROWS' fourth field is a word of flags: 0 / 1 as before, 2 a favourite, 3 both\n");
}

// P4: the patch Radio's recordings list is a worker's: the op is answered at once, the list lands a pass or more later, and a second request while one is out is
// remembered and run when it comes back.
void testRecordingsListOnAWorker() {
    using Access = cascade::gui::AppWindowTestAccess;
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    d.eng.controlPass();
    const std::filesystem::path rec = std::filesystem::path(fact(s, "fact.recorder.dir")).parent_path() / "list_worker_dir";
    std::error_code ec;
    std::filesystem::remove_all(rec, ec);
    std::filesystem::create_directories(rec, ec);
    Access::setRecordDir(d.eng, rec.string());
    g13b::writeIqWav(rec / "one_96k.wav", 96000);
    std::atomic<int> calls{0};
    std::atomic<bool> release{false};
    Access::diskHook(d.eng, [&] {
        calls.fetch_add(1);
        for (int i = 0; i < 5000 && !release.load(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    });
    const int32_t before = count(s, FOXAPI_LIST_PATCH_DEVICES);
    CHECK_EQ(pumped(d.eng, s, appCmd(FOXAPP_OP_PATCH_LIST_RECORDINGS)).status(), FOXAPI_OK);
    CHECK(Access::recordingScanPending(d.eng));
    CHECK_EQ(pumped(d.eng, s, appCmd(FOXAPP_OP_PATCH_LIST_RECORDINGS)).status(), FOXAPI_OK);   // asked again: remembered
    passes(d.eng, 5);
    CHECK_EQ(count(s, FOXAPI_LIST_PATCH_DEVICES), before);   // the pass has not waited, and nothing is listed yet
    release.store(true);
    CHECK(passesUntil(d.eng, [&] { return calls.load() == 2 && !Access::recordingScanPending(d.eng); }));
    passes(d.eng, 2);
    CHECK_EQ(count(s, FOXAPI_LIST_PATCH_DEVICES), before + 1);
    Access::diskHook(d.eng, nullptr);
    std::filesystem::remove_all(rec, ec);
    std::printf("P4: the recordings list is a worker's; a second request is remembered\n");
}


// G13b-1: the parts bin's track bit (served, and in the fingerprint), PATCH_DEVICES in the engine's own order with every
// key whole in a LOCAL fact, a recording's rate, and FOXAPP_OP_PATCH_LIST_RECORDINGS (grants, a relisting that finds other
// files with the same count).
void testPatchPartsAndDevices() {
    using namespace g13;
    using namespace g13b;
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    testing::Session remote(api(), d.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_SETTINGS, "s3cret");
    CHECK(s.ok() && remote.ok());
    passes(d.eng, 3);

    // THE TRACK BIT. The parts bin is rebuilt from the loaded modules each pass the page is open, so a module with a track
    // source is put into the engine's catalogue behind its back with the page CLOSED (the step does not touch it then).
    CHECK_EQ(pumped(d.eng, s, pageOpen(0)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    CHECK_EQ(count(s, FOXAPI_LIST_PATCH_PARTS), 0);
    const uint64_t seqBefore = s.state().listSeq;
    Access::addPart(d.eng, "tracker.dll", "Tracker", pc::PortType::Iq, true, false);
    Access::addPart(d.eng, "plain.dll", "Plain", pc::PortType::Audio, false, false);
    passes(d.eng, 2);
    CHECK(s.state().listSeq != seqBefore);  // the bin's content is in the fingerprint
    CHECK_EQ(count(s, FOXAPI_LIST_PATCH_PARTS), 2);
    CHECK_EQ(std::string(item(s, FOXAPI_LIST_PATCH_PARTS, 0).detail), std::string("tracker.dll"));
    CHECK_EQ(item(s, FOXAPI_LIST_PATCH_PARTS, 0).value[2], 1.0);
    CHECK_EQ(item(s, FOXAPI_LIST_PATCH_PARTS, 1).value[2], 0.0);
    // ...and a change of ONLY that bit moves the block.
    {
        const uint64_t seq = s.state().listSeq;
        Access::addPart(d.eng, "plain.dll", "Plain", pc::PortType::Audio, true, false, /*replace=*/true);
        passes(d.eng, 2);
        CHECK(s.state().listSeq != seq);
        CHECK_EQ(item(s, FOXAPI_LIST_PATCH_PARTS, 1).value[2], 1.0);
    }
    CHECK_EQ(pumped(d.eng, s, pageOpen(1)).status(), FOXAPI_OK);
    passes(d.eng, 2);

    // PATCH_DEVICES, in Engine::patchDeviceChoices' order, whole keys in the facts, LOCAL.
    const auto same = [&](const char* when) {
        const std::vector<std::pair<std::string, std::string>> want = Access::choices(d.eng);
        const int32_t n = count(s, FOXAPI_LIST_PATCH_DEVICES);
        CHECK_EQ(n, static_cast<int32_t>(want.size()));
        for (int32_t i = 0; i < n && i < static_cast<int32_t>(want.size()); ++i) {
            const FoxListItem it = item(s, FOXAPI_LIST_PATCH_DEVICES, static_cast<uint32_t>(i));
            CHECK_EQ(it.id, static_cast<uint64_t>(i) + 1u);
            CHECK_EQ(std::string(it.name), want[static_cast<std::size_t>(i)].second);
            CHECK_EQ(std::string(it.detail), want[static_cast<std::size_t>(i)].first);
            CHECK_EQ(fact(s, ("fact.patch.device." + std::to_string(i + 1)).c_str()), want[static_cast<std::size_t>(i)].first);
        }
        std::printf("G13b-1: PATCH_DEVICES %s: %d entries, the engine's order\n", when, n);
    };
    CHECK_EQ(count(s, FOXAPI_LIST_PATCH_DEVICES), 1);  // the generator, nothing scanned yet
    CHECK_EQ(std::string(item(s, FOXAPI_LIST_PATCH_DEVICES, 0).name), std::string("Signal generator"));
    CHECK_EQ(std::string(item(s, FOXAPI_LIST_PATCH_DEVICES, 0).detail), std::string(pc::kGeneratorKey));
    same("before a scan");
    {
        // A REMOTE session sees the labels, never the keys (they name hardware and paths on this machine).
        CHECK_EQ(count(remote, FOXAPI_LIST_PATCH_DEVICES), 1);
        CHECK_EQ(std::string(item(remote, FOXAPI_LIST_PATCH_DEVICES, 0).name), std::string("Signal generator"));
        CHECK_EQ(std::string(item(remote, FOXAPI_LIST_PATCH_DEVICES, 0).detail), std::string());
        int32_t st = 0;
        fact(remote, "fact.patch.device.1", &st);
        CHECK_EQ(st, FOXAPI_DENIED);
    }
    // The fake radio's scan adds the native radios to the list, between the recordings and the sound cards.
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SCAN_DEVICES)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return count(s, FOXAPI_LIST_PATCH_DEVICES) > 1; }));
    same("after a scan");

    // RECORDINGS: the op lists the record folder's I/Q files on the engine's pass.
    const std::filesystem::path rec = scratchDir("patch_rec_g13b");
    Access::setRecordDir(d.eng, rec.string());
    writeIqWav(rec / "one_96k.wav", 96000);
    const std::string keyOne = pc::makeIqFileKey((rec / "one_96k.wav").string());
    const int32_t before = count(s, FOXAPI_LIST_PATCH_DEVICES);
    passes(d.eng, 3);
    CHECK_EQ(count(s, FOXAPI_LIST_PATCH_DEVICES), before);  // nothing lists them by itself
    CHECK_EQ(pumped(d.eng, s, appCmd(FOXAPP_OP_PATCH_LIST_RECORDINGS)).status(), FOXAPI_OK);
    // (The listing is a worker's since stub campaign B19: the op is answered at once and the list lands on a later pass.)
    CHECK(passesUntil(d.eng, [&] { return !Access::recordingScanPending(d.eng); }));
    passes(d.eng, 2);
    CHECK_EQ(count(s, FOXAPI_LIST_PATCH_DEVICES), before + 1);
    {
        const FoxListItem r = deviceNamed(s, keyOne);
        CHECK_EQ(std::string(r.detail), keyOne);
        CHECK(std::string(r.name).find("Recording: one_96k.wav") == 0u);
        CHECK_EQ(r.value[0], 96000.0);  // its own rate
        CHECK_EQ(item(s, FOXAPI_LIST_PATCH_DEVICES, 0).value[0], 0.0);  // a generator has none
        std::printf("G13b-1: the recording as a device: \"%s\", %.0f Hz\n", r.name, r.value[0]);
    }
    same("with a recording");
    // A relisting that finds OTHER files, the same number of them, rebuilds the list (the fingerprint has the content).
    {
        std::error_code ec;
        std::filesystem::remove(rec / "one_96k.wav", ec);
        writeIqWav(rec / "two_250k.wav", 250000);
        CHECK_EQ(pumped(d.eng, s, appCmd(FOXAPP_OP_PATCH_LIST_RECORDINGS)).status(), FOXAPI_OK);
        CHECK(passesUntil(d.eng, [&] { return !Access::recordingScanPending(d.eng); }));
        passes(d.eng, 2);
        CHECK_EQ(count(s, FOXAPI_LIST_PATCH_DEVICES), before + 1);
        CHECK_EQ(deviceNamed(s, keyOne).id, 0u);
        const FoxListItem r = deviceNamed(s, pc::makeIqFileKey((rec / "two_250k.wav").string()));
        CHECK(r.id != 0u);
        CHECK_EQ(r.value[0], 250000.0);
    }
    // GRANTS: SETTINGS; VIEW alone is refused; a REMOTE session is DENIED as for every FOXAPP_OP_*.
    {
        testing::Session viewOnly(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW);
        CHECK(viewOnly.ok());
        CHECK_EQ(pumped(d.eng, viewOnly, appCmd(FOXAPP_OP_PATCH_LIST_RECORDINGS)).status(), FOXAPI_DENIED);
        CHECK_EQ(pumped(d.eng, remote, appCmd(FOXAPP_OP_PATCH_LIST_RECORDINGS)).status(), FOXAPI_DENIED);
    }
    std::error_code ec;
    std::filesystem::remove_all(rec, ec);
    std::printf("G13b-1: the track bit, PATCH_DEVICES, the recordings op: done\n");
}

// G13b-2: the receiver's radio and the converter in force for a Radio node's device (fact.patch.receiverRadio,
// fact.patch.receiverCentre, fact.patch.conv.<node>).
void testPatchReceiverRadioAndConverter() {
    using namespace g13;
    using namespace g13b;
    Access::installRadios83();  // (the fake radio, an Airspy and a HackRF on fake wires: six rows after a scan)
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    passes(d.eng, 3);
    // The generator is not "a radio".
    CHECK_EQ(fact(s, "fact.patch.receiverRadio"), std::string());
    {
        testing::Session remote(api(), d.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW, "s3cret");
        int32_t st = 0;
        fact(remote, "fact.patch.receiverRadio", &st);
        CHECK_EQ(st, FOXAPI_DENIED);  // it names hardware
        fact(remote, "fact.patch.receiverCentre", &st);
        CHECK(st >= 0);               // a frequency does not
    }
    // Open the fake radio as the receiver's: its key is the device key a Radio node would name it by.
    CHECK(openRadio(d.eng, s, engine_api_test::kFakeRadioId));
    passes(d.eng, 2);
    const std::string key = fact(s, "fact.patch.receiverRadio");
    CHECK(!key.empty());
    CHECK_EQ(key, Access::deviceKeyNow(d.eng));
    std::printf("G13b-2: the receiver's radio: %s\n", key.c_str());
    // ...its centre is the AIR centre the radio carries, to the digit: the state's centreHz.
    const std::string centre = fact(s, "fact.patch.receiverCentre");
    CHECK(!centre.empty());
    CHECK_EQ(std::strtod(centre.c_str(), nullptr), s.state().centreHz);
    // WHETHER IT EXISTS moves the block, its VALUE does not: a radio that was never tuned has no centre to carry (the fact is
    // empty), and the first tune makes it appear. A retune after that leaves the fact as the last rebuild wrote it - the
    // live figure is the state's centreHz, which is what the standard window reads once the fact says there is one.
    {
        Direct fresh;
        testing::Session fs(api(), fresh.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
        CHECK(fs.ok());
        passes(fresh.eng, 3);
        const std::string tuned = fact(fs, "fact.patch.receiverCentre");
        std::printf("G13b-2: a fresh engine's generator carries a centre: \"%s\"\n", tuned.c_str());
        CHECK(!tuned.empty());
        Access::setRawCentre(fresh.eng, 0.0);   // never tuned
        passes(fresh.eng, 2);
        CHECK_EQ(fact(fs, "fact.patch.receiverCentre"), std::string());
        Access::setRawCentre(fresh.eng, 101.5e6);
        passes(fresh.eng, 2);
        const std::string after = fact(fs, "fact.patch.receiverCentre");
        CHECK(!after.empty());
        CHECK_EQ(std::strtod(after.c_str(), nullptr), fs.state().centreHz);
        // ...a retune moves the state and leaves the fact where it was (the value is not an input of the block).
        const uint64_t seq = fs.state().listSeq;
        Access::setRawCentre(fresh.eng, 102.5e6);
        passes(fresh.eng, 3);
        CHECK_EQ(fs.state().listSeq, seq);
        CHECK_EQ(fs.state().centreHz, 102.5e6);
        CHECK_EQ(std::strtod(fact(fs, "fact.patch.receiverCentre").c_str(), nullptr), 101.5e6);
    }

    // LENT: START takes the receiver's radio (the receiver is on the generator), and the fact still names it. A starter
    // patch is on the generator itself, so nothing else touches the radio.
    {
        const Starter p = starter();
        CHECK_EQ(pumpedText(d.eng, s, setGraph(FOXAPP_PATCH_GRAPH_DOCUMENT, 0), pc::graphCommandText(p.g)).result.status, FOXAPI_OK);
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_PATCH_RUN, 0, 1)).status(), FOXAPI_OK);
        CHECK(passUntil(d.eng, [&] { return patchRunning(s) && (s.state().flags & FOXAPI_RX_DEVICE_OPEN) == 0u; }));
        CHECK_EQ(fact(s, "fact.patch.receiverRadio"), key);
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_PATCH_RUN, 0, 0)).status(), FOXAPI_OK);
        CHECK(passUntil(d.eng, [&] { return !patchRunning(s) && (s.state().flags & FOXAPI_RX_DEVICE_OPEN) != 0u; }));
        CHECK_EQ(fact(s, "fact.patch.receiverRadio"), key);  // given back
    }

    // THE CONVERTER IN FORCE for a Radio node's device, "<mode> <LO Hz> <inverted>" as fact.converter.live spells it.
    {
        pc::Graph g;
        const pc::NodeId withDev = g.addNode(pc::NodeKind::Radio, "A", pc::PortType::Iq, 40.0f, 40.0f);
        const pc::NodeId plain = g.addNode(pc::NodeKind::Radio, "B", pc::PortType::Iq, 40.0f, 200.0f);
        const pc::NodeId noDev = g.addNode(pc::NodeKind::Radio, "C", pc::PortType::Iq, 40.0f, 360.0f);
        const pc::NodeId chan = g.addNode(pc::NodeKind::Channel, "D", pc::PortType::Iq, 400.0f, 40.0f);
        if (pc::Node* n = g.mutableNode(withDev)) { n->device = key; n->on = false; }
        if (pc::Node* n = g.mutableNode(plain)) { n->device = pc::kGeneratorKey; n->on = false; }
        Access::storeConverter(d.eng, pc::converterKeyForDevice(key),
                               cascade::core::ConverterSetting{cascade::core::ConverterMode::Up, 125.0e6, false});
        CHECK_EQ(pumpedText(d.eng, s, setGraph(FOXAPP_PATCH_GRAPH_DOCUMENT, 0), pc::graphCommandText(g)).result.status, FOXAPI_OK);
        passes(d.eng, 2);
        CHECK_EQ(fact(s, ("fact.patch.conv." + std::to_string(withDev)).c_str()), std::string("1 125000000 0"));
        CHECK_EQ(fact(s, ("fact.patch.conv." + std::to_string(plain)).c_str()), std::string("0 0 0"));
        int32_t st = 0;
        fact(s, ("fact.patch.conv." + std::to_string(noDev)).c_str(), &st);
        CHECK_EQ(st, FOXAPI_NOT_FOUND);  // a Radio with no device has none
        fact(s, ("fact.patch.conv." + std::to_string(chan)).c_str(), &st);
        CHECK_EQ(st, FOXAPI_NOT_FOUND);  // only a Radio node
        // The setting moves the block (the converter is an input of the fact, and of the fingerprint).
        Access::storeConverter(d.eng, pc::converterKeyForDevice(key),
                               cascade::core::ConverterSetting{cascade::core::ConverterMode::Down, 7.0e6, true});
        passes(d.eng, 2);
        CHECK_EQ(fact(s, ("fact.patch.conv." + std::to_string(withDev)).c_str()), std::string("2 7000000 1"));
        // ...and a node deleted takes its fact with it.
        pc::Graph less = graphOf(s);
        less.removeNode(withDev);
        CHECK_EQ(pumpedText(d.eng, s, setGraph(0, static_cast<long long>(versionOf(s))), pc::graphCommandText(less)).result.status,
                 FOXAPI_OK);
        passes(d.eng, 2);
        fact(s, ("fact.patch.conv." + std::to_string(withDev)).c_str(), &st);
        CHECK_EQ(st, FOXAPI_NOT_FOUND);
    }
    std::printf("G13b-2: the receiver's radio, lent and given back; the converter in force per Radio node: done\n");
}

// G13b-3: PATCH_LIVE. A running generator patch (a channel, a demodulator with a squelch, a spectrum display and a speaker
// to a WAV file) is read once a pass into a block of its own: a squelch gate, a channel's level, a speaker's output - and
// none of it moves listSeq. A speaker whose output could not be made says why.
void testPatchLive() {
    using namespace g13;
    using namespace g13b;
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    testing::Session remote(api(), d.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW, "s3cret");
    CHECK(s.ok() && remote.ok());
    passes(d.eng, 3);
    CHECK_EQ(count(s, FOXAPI_LIST_PATCH_LIVE), 0);  // nothing runs
    const std::filesystem::path rec = scratchDir("patch_live_g13b");
    Access::setRecordDir(d.eng, rec.string());

    Starter p = starter();
    const pc::NodeId speaker = p.g.addNode(pc::NodeKind::Sink, "Speaker", pc::PortType::Audio, 700.0f, 320.0f);
    CHECK(p.g.connect(p.demod, 0, speaker, 0) == pc::Connect::Ok);
    if (pc::Node* n = p.g.mutableNode(p.demod)) {
        n->squelch = true;
        n->squelchDb = -20.0f;  // the generator's noise floor is -90 dB: the gate is closed
    }
    CHECK_EQ(pumpedText(d.eng, s, setGraph(FOXAPP_PATCH_GRAPH_DOCUMENT, 0), pc::graphCommandText(p.g)).result.status, FOXAPI_OK);
    passes(d.eng, 2);
    CHECK_EQ(count(s, FOXAPI_LIST_PATCH_LIVE), 0);  // stopped: nothing is running to read
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_PATCH_RUN, 0, 1)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] {
        return liveRow(s, 1, p.demod).id != 0u && liveRow(s, 3, p.chan).id != 0u && liveRow(s, 2, speaker).id != 0u;
    }, 8000));

    // THE SQUELCH GATE of the demodulator: the level it judges, and whether it is open (ACTIVE).
    const FoxListItem sq = liveRow(s, 1, p.demod);
    std::printf("G13b-3: the gate: %.1f dB, %s\n", sq.value[0], (sq.flags & FOXAPI_ITEM_ACTIVE) != 0u ? "open" : "closed");
    CHECK(sq.value[0] < -20.0);
    CHECK_EQ(sq.flags & FOXAPI_ITEM_ACTIVE, 0u);
    // THE CHANNEL'S LEVEL, off its radio's spectrum: the node of the channel, a figure in dB.
    const FoxListItem lv = liveRow(s, 3, p.chan);
    CHECK(lv.value[0] < 0.0 && lv.value[0] > -200.0);
    // THE SPEAKER'S OUTPUT: what it plays to, how much it has written (rising), a peak 0..1, no error.
    const FoxListItem sp = liveRow(s, 2, speaker);
    CHECK(std::string(sp.name).find("WAV  patch-") == 0u);
    CHECK_EQ(std::string(sp.detail), std::string());
    CHECK(sp.value[0] >= 0.0 && sp.value[0] <= 1.0);
    std::printf("G13b-3: the speaker: \"%s\", peak %.3f, %.0f samples\n", sp.name, sp.value[0], sp.value[1]);
    const double samples0 = sp.value[1];
    CHECK(passUntil(d.eng, [&] { return liveRow(s, 2, speaker).value[1] > samples0 + 4800.0; }, 8000));
    CHECK_EQ(liveRow(s, 5, speaker).id, 0u);

    // NO listSeq: the readings move every pass and the block of lists does not. A settled running patch rebuilds nothing.
    {
        const uint64_t seq = s.state().listSeq;
        const double w0 = liveRow(s, 2, speaker).value[1];
        passes(d.eng, 3);
        CHECK(passUntil(d.eng, [&] { return liveRow(s, 2, speaker).value[1] > w0; }, 8000));
        CHECK_EQ(s.state().listSeq, seq);
    }
    // A REMOTE session reads the same readings (they name nothing on the machine).
    CHECK(liveRow(remote, 2, speaker).id != 0u);
    CHECK_EQ(std::string(liveRow(remote, 2, speaker).name), std::string(liveRow(s, 2, speaker).name));
    // The node's spectrum is still read, not copied: a demodulator that is wired to nothing has no row of its own kind.
    CHECK_EQ(liveRow(s, 1, speaker).id, 0u);

    // A STOPPED PATCH has no readings (the radio and the dests are gone).
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_PATCH_RUN, 0, 0)).status(), FOXAPI_OK);
    passes(d.eng, 3);
    CHECK_EQ(count(s, FOXAPI_LIST_PATCH_LIVE), 0);

    // A SPEAKER THAT COULD NOT BE MADE: the record folder is a file, so the WAV cannot be created. The reading says why.
    {
        const std::filesystem::path blocker = rec / "not_a_folder";
        { std::ofstream f(blocker); f << "x"; }
        Access::setRecordDir(d.eng, blocker.string());
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_PATCH_RUN, 0, 1)).status(), FOXAPI_OK);
        // (0.99.64: A WAV SPEAKER'S DESTINATION EXISTS AT ONCE and its file is opened by a worker, so a folder that cannot be made is a LATE
        // failure: it rides on the speaker's own reading (kind 2: the destination's error is hashed with its figures), and no destination "could
        // not be made" (kind 5 is for a speaker whose destination is refused on the spot - a sound device that will not open).)
        CHECK(passUntil(d.eng, [&] { return std::string(liveRow(s, 2, speaker).detail).size() > 0; }, 8000));
        CHECK_EQ(liveRow(s, 5, speaker).id, 0u);
        std::printf("G13b-3: a speaker whose file could not be made: \"%s\"\n", liveRow(s, 2, speaker).detail);
        CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_PATCH_RUN, 0, 0)).status(), FOXAPI_OK);
        passes(d.eng, 3);
    }
    std::error_code ec;
    std::filesystem::remove_all(rec, ec);
    std::printf("G13b-3: the gate, the level, the speaker, none of it in listSeq: done\n");
}

// G13b-4: the decoder's face (PATCH_LIVE kind 4) and DECODER_LINES' originating node, against a REAL I/Q decoder module
// that says three lines (FOXSDR_FIXTURE_G8_LINES) wired to a Text out.
void testPatchDecoderFaceAndLines() {
    using namespace g13;
    using namespace g13b;
#ifdef _WIN32
    _putenv_s("FOXSDR_FIXTURE_G8_LINES", "3");
#else
    setenv("FOXSDR_FIXTURE_G8_LINES", "3", 1);
#endif
    PluginWorld world;
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    passes(d.eng, 3);
    std::string keyIq;
    for (int32_t i = 0; i < count(s, FOXAPI_LIST_PATCH_PARTS); ++i) {
        const FoxListItem it = item(s, FOXAPI_LIST_PATCH_PARTS, static_cast<uint32_t>(i));
        if (it.value[0] == static_cast<double>(pc::PortType::Iq)) { keyIq = it.detail; }
    }
    CHECK(!keyIq.empty());
    Starter p = starter();
    const pc::NodeId dec = p.g.addNode(pc::NodeKind::Decoder, "IQ decoder", pc::PortType::Iq, 420.0f, 520.0f);
    const pc::NodeId lonely = p.g.addNode(pc::NodeKind::Decoder, "Unwired decoder", pc::PortType::Iq, 700.0f, 520.0f);
    const pc::NodeId out = p.g.addNode(pc::NodeKind::Sink, "Text out", pc::PortType::Text, 700.0f, 40.0f);
    if (pc::Node* n = p.g.mutableNode(dec)) { n->plugin = keyIq; }
    if (pc::Node* n = p.g.mutableNode(lonely)) { n->plugin = keyIq; }
    CHECK(p.g.connect(p.radio, 0, dec, 0) == pc::Connect::Ok);
    CHECK(p.g.connect(p.radio, 0, lonely, 0) == pc::Connect::Ok);
    CHECK(p.g.connect(dec, 0, out, 0) == pc::Connect::Ok);
    CHECK_EQ(pumpedText(d.eng, s, setGraph(FOXAPP_PATCH_GRAPH_DOCUMENT, 0), pc::graphCommandText(p.g)).result.status, FOXAPI_OK);
    const int32_t linesBefore = count(s, FOXAPI_LIST_DECODER_LINES);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_PATCH_RUN, 0, 1)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return count(s, FOXAPI_LIST_DECODER_LINES) >= linesBefore + 1; }, 8000));
    // THE FACE: every RUNNING decoder has one, wired or not; the count and the newest line.
    CHECK(passUntil(d.eng, [&] { return liveRow(s, 4, dec).id != 0u && liveRow(s, 4, lonely).id != 0u; }, 8000));
    const FoxListItem face = liveRow(s, 4, dec);
    std::printf("G13b-4: the face of decoder %u: %.0f line(s), newest \"%s\"\n", static_cast<unsigned>(dec), face.value[0],
                face.detail);
    CHECK(face.value[0] >= 1.0);
    CHECK(std::string(face.detail).find("G8 decoded line") == 0u);
    // THE LINES carry the decoder that said them; the unwired decoder's lines are not shown anywhere (it has a face).
    const int32_t n = count(s, FOXAPI_LIST_DECODER_LINES);
    int fromDec = 0;
    for (int32_t i = linesBefore; i < n; ++i) {
        const FoxListItem it = item(s, FOXAPI_LIST_DECODER_LINES, static_cast<uint32_t>(i));
        CHECK_EQ(it.value[0], static_cast<double>(dec));
        fromDec += it.value[0] == static_cast<double>(dec) ? 1 : 0;
    }
    CHECK(fromDec >= 1);
    CHECK_EQ(std::string(item(s, FOXAPI_LIST_DECODER_LINES, static_cast<uint32_t>(linesBefore)).detail).find("G8 decoded line"), 0u);
    // A refused decoder has no face (it is not running).
    Access::injectRefused(d.eng, p.radio, {lonely});
    passes(d.eng, 3);
    CHECK_EQ(liveRow(s, 4, lonely).id, 0u);
    CHECK(liveRow(s, 4, dec).id != 0u);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_PATCH_RUN, 0, 0)).status(), FOXAPI_OK);
    passes(d.eng, 2);
    // The receiver's own decoders say 0: a line the engine raises itself carries no node.
    std::printf("G13b-4: the decoder's face and the lines' node: done\n");
}
// G13B TESTS END>>
// <<G13C TESTS BEGIN
// G13c: the Patch page, part 3 (window port group 13, API 0.3 section 8.12.7): the decoded pictures. A REAL picture
// decoder module (tests/fixture_group13_plugin.cpp) runs in a patch on the signal generator, in the engine's pass, with no
// window anywhere; its pictures are read through read_image, announced by FOXAPI_EVENT_DECODER_IMAGE and named by the
// PATCH_LIVE picture row (kind 6). Every property of a picture is a function of its sequence number N (see the fixture): an
// odd N is RGB24 8x6, an even N is GRAY8 5x3, byte i is (N*13 + i*7) & 0xFF, complete when N % 3 == 0 - so a picture that is
// half one and half another is a picture whose bytes do not follow the formula.

namespace g13c {

namespace pc = cascade::core::patch;

// The scratch plugin folder of this group: the picture fixture and nothing else.
struct ImageWorld {
    std::filesystem::path dir;
    static std::string hook;
    ImageWorld() {
        dir = g13b::scratchDir("plugins_group13c");
        std::error_code ec;
        const std::filesystem::path p(FIXTURE_GROUP13_PLUGIN);
        std::filesystem::copy_file(p, dir / p.filename(), std::filesystem::copy_options::overwrite_existing, ec);
        CHECK(!ec);
        hook = dir.string();
        cascade::gui::AppWindowTestAccess::setPluginDir(&ImageWorld::path);
    }
    ~ImageWorld() {
        cascade::gui::AppWindowTestAccess::setPluginDir(nullptr);
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    static std::string path() { return hook; }
};
std::string ImageWorld::hook;

// What picture N must look like, byte for byte.
struct Shape {
    uint32_t w, h, format, bytes;
};
Shape shapeOf(uint64_t n) {
    const bool rgb = (n % 2u) == 1u;
    return rgb ? Shape{8u, 6u, CASCADE_IMAGE_RGB24, 144u} : Shape{5u, 3u, CASCADE_IMAGE_GRAY8, 15u};
}
bool pixelsFollow(uint64_t n, const std::vector<uint8_t>& px, size_t from = 0, size_t to = size_t(-1)) {
    to = std::min(to, px.size());
    for (size_t i = from; i < to; ++i) {
        if (px[i] != static_cast<uint8_t>((n * 13u + i * 7u) & 0xFFu)) { return false; }
    }
    return true;
}

// A picture read through the table: what read_image said, whole.
struct Got {
    int32_t status = FOXAPI_NOT_FOUND;
    FoxImageInfo info{};
    std::vector<uint8_t> px;
};
Got readPicture(const FoxEngineApi* a, FoxSession* s, uint64_t id) {
    Got g;
    g.info.structSize = sizeof(g.info);
    const int32_t size = a->read_image(s, id, &g.info, nullptr, 0);
    if (size < 0) {
        g.status = size;
        return g;
    }
    g.px.assign(static_cast<size_t>(size), 0xEEu);
    FoxImageInfo again{};
    again.structSize = sizeof(again);
    g.status = a->read_image(s, id, &again, g.px.data(), g.px.size());
    if (g.status == size) { g.info = again; }
    return g;
}

// Does the picture follow the fixture's formula, and does it agree with its own info?
bool coherent(const Got& g) {
    if (g.status < 0) { return false; }
    const Shape sh = shapeOf(g.info.sequence);
    return g.info.sequence >= 1u && g.info.width == sh.w && g.info.height == sh.h && g.info.format == sh.format &&
           g.info.bytes == sh.bytes && g.px.size() == sh.bytes && g.info.complete == ((g.info.sequence % 3u) == 0u ? 1u : 0u) &&
           pixelsFollow(g.info.sequence, g.px);
}

// The picture node of a graph on the generator: radio -> channel -> demod -> the fixture decoder (audio).
struct Pic {
    g13::Starter p;
    pc::NodeId dec = pc::kNoNode;
};
Pic picturePatch(testing::Session& s, const char* name = "Fixture picture") {
    Pic w;
    w.p = g13::starter();
    std::string key;
    for (int32_t i = 0; i < count(s, FOXAPI_LIST_PATCH_PARTS); ++i) {
        const FoxListItem it = item(s, FOXAPI_LIST_PATCH_PARTS, static_cast<uint32_t>(i));
        if (std::string(it.name) == "Group Thirteen Fixture") { key = it.detail; }
    }
    CHECK(!key.empty());
    w.dec = w.p.g.addNode(pc::NodeKind::Decoder, name, pc::PortType::Audio, 700.0f, 320.0f);
    if (pc::Node* n = w.p.g.mutableNode(w.dec)) { n->plugin = key; }
    CHECK(w.p.g.connect(w.p.demod, 0, w.dec, 0) == pc::Connect::Ok);
    return w;
}

}  // namespace g13c

// G13c-1: a picture from the pass to the reader. The row, the event, the info and the pixels, whole; the buffer protocol
// (info-only, a short buffer, a null buffer); the unknown, the stale and the removed id; a REMOTE session and a VIEW-only
// one read the same; the capability bit; nothing of it moves listSeq.
void testPatchPictures() {
    using namespace g13;
    using namespace g13b;
    using namespace g13c;
    ImageWorld world;
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    testing::Session viewer(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW);
    testing::Session remote(api(), d.eng.apiHandle(), FOXAPI_SESSION_REMOTE,
                            FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS, "s3cret");
    CHECK(s.ok() && viewer.ok() && remote.ok());
    CHECK_EQ(api()->subscribe(s.raw(), FOXAPI_EVENT_MASK(FOXAPI_EVENT_DECODER_IMAGE)), FOXAPI_OK);
    CHECK_EQ(api()->subscribe(viewer.raw(), FOXAPI_EVENT_MASK(FOXAPI_EVENT_DECODER_IMAGE)), FOXAPI_OK);
    CHECK_EQ(api()->subscribe(remote.raw(), FOXAPI_EVENT_MASK(FOXAPI_EVENT_DECODER_IMAGE)), FOXAPI_OK);
    passes(d.eng, 3);
    // The table says it serves pictures, and says so for what it implements: the decoders' text events and list are not
    // this bit (FOXAPI_CAP_DECODERS stays clear).
    CHECK((api()->capabilities & FOXAPI_CAP_IMAGES) != 0u);
    CHECK((api()->capabilities & FOXAPI_CAP_DECODERS) == 0u);
    CHECK(FOXAPI_HAS(api(), read_image));

    const Pic w = picturePatch(s);
    CHECK_EQ(pumpedText(d.eng, s, setGraph(FOXAPP_PATCH_GRAPH_DOCUMENT, 0), pc::graphCommandText(w.p.g)).result.status, FOXAPI_OK);
    passes(d.eng, 3);
    (void)takeEvents(s);
    // BEFORE ANY PICTURE: no row, no event, and every id is unknown.
    CHECK_EQ(liveRow(s, 6, w.dec).id, 0u);
    for (const uint64_t id : {uint64_t{0}, uint64_t{1}, uint64_t{12345}, ~uint64_t{0}}) {
        FoxImageInfo info{};
        info.structSize = sizeof(info);
        CHECK_EQ(api()->read_image(s.raw(), id, &info, nullptr, 0), FOXAPI_NOT_FOUND);
    }
    // A bad session is a bad argument, as everywhere else on the table.
    CHECK_EQ(api()->read_image(nullptr, 1, nullptr, nullptr, 0), FOXAPI_BAD_ARGUMENT);

    // RUN: the decoder is fed audio from the generator and offers its first picture.
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_PATCH_RUN, 0, 1)).status(), FOXAPI_OK);
    passes(d.eng, 3);
    // EVERY EVENT IS READABLE THE MOMENT IT IS TAKEN: after each pass the events are drained and each one's picture read at
    // once (a set handed over after the event would be a NOT_FOUND here).
    std::vector<FoxEvent> seen;
    int unreadable = 0;
    CHECK(passUntil(d.eng,
                    [&] {
                        for (const FoxEvent& e : takeEvents(s)) {
                            if (e.kind != FOXAPI_EVENT_DECODER_IMAGE) { continue; }
                            seen.push_back(e);
                            FoxImageInfo info{};
                            info.structSize = sizeof(info);
                            if (api()->read_image(s.raw(), static_cast<uint64_t>(e.value[0]), &info, nullptr, 0) <= 0) { ++unreadable; }
                        }
                        return seen.size() >= 3;
                    },
                    20000));
    CHECK_EQ(unreadable, 0);
    uint64_t last = 0;
    for (const FoxEvent& e : seen) {
        CHECK_EQ(e.target, static_cast<uint32_t>(w.dec));
        CHECK_EQ(e.value[1], 1.0);                                      // a patch node
        CHECK_EQ(std::string(e.text), std::string("Fixture picture"));  // the node's name
        CHECK(static_cast<uint64_t>(e.value[0]) > last);                // ids rise, never reused
        last = static_cast<uint64_t>(e.value[0]);
    }
    std::printf("G13c-1: %zu picture event(s) so far, newest image id %llu, every one readable when taken\n", seen.size(),
                static_cast<unsigned long long>(last));

    // THE ROW names the newest picture: its id, its byte count, whether it is finished; the node's name.
    const FoxListItem row = liveRow(s, 6, w.dec);
    CHECK_EQ(row.id, static_cast<uint64_t>(w.dec));
    CHECK(row.value[0] >= static_cast<double>(last));
    CHECK_EQ(std::string(row.name), std::string("Fixture picture"));
    // READ the picture the row names (a pass may move it on between the two: take the row again until they agree).
    Got g;
    FoxListItem at{};
    for (int tries = 0; tries < 50 && !coherent(g); ++tries) {
        at = liveRow(s, 6, w.dec);
        g = readPicture(api(), s.raw(), static_cast<uint64_t>(at.value[0]));
    }
    CHECK(coherent(g));
    const Shape sh = shapeOf(g.info.sequence);
    std::printf("G13c-1: picture %llu: %ux%u format %u, %llu bytes, sequence %llu, complete %u, caption \"%s\"\n",
                static_cast<unsigned long long>(at.value[0]), g.info.width, g.info.height, g.info.format,
                static_cast<unsigned long long>(g.info.bytes), static_cast<unsigned long long>(g.info.sequence), g.info.complete,
                g.info.caption);
    CHECK_EQ(g.px.size(), static_cast<size_t>(sh.bytes));
    CHECK_EQ(at.value[1], static_cast<double>(sh.bytes));                    // the row's byte count is the picture's
    CHECK_EQ((at.flags & FOXAPI_ITEM_ACTIVE) != 0u, g.info.complete == 1u);  // ...and its finished-ness
    CHECK_EQ(std::string(g.info.caption), std::string("Fixture picture"));
    const uint64_t id = static_cast<uint64_t>(at.value[0]);

    // THE BUFFER PROTOCOL, on the picture just read. The pass thread is THIS thread, so nothing moves while these run.
    {
        // Info alone: the full size, the shape, no pixel written.
        FoxImageInfo info{};
        info.structSize = sizeof(info);
        CHECK_EQ(api()->read_image(s.raw(), id, &info, nullptr, 0), static_cast<int32_t>(sh.bytes));
        CHECK_EQ(info.width, sh.w);
        // Pixels alone (no info).
        std::vector<uint8_t> px(sh.bytes, 0xEEu);
        CHECK_EQ(api()->read_image(s.raw(), id, nullptr, px.data(), px.size()), static_cast<int32_t>(sh.bytes));
        CHECK(pixelsFollow(g.info.sequence, px));
        // A SHORT buffer: the bytes that fit, the FULL count returned, nothing written past the cap.
        std::vector<uint8_t> shortBuf(sh.bytes + 8u, 0xEEu);
        CHECK_EQ(api()->read_image(s.raw(), id, &info, shortBuf.data(), sh.bytes - 1u), static_cast<int32_t>(sh.bytes));
        CHECK(pixelsFollow(g.info.sequence, shortBuf, 0, sh.bytes - 1u));
        for (size_t i = sh.bytes - 1u; i < shortBuf.size(); ++i) { CHECK_EQ(shortBuf[i], uint8_t{0xEE}); }
        CHECK_EQ(info.bytes, static_cast<uint64_t>(sh.bytes));  // the info is whole whatever the buffer
        // A buffer given and a cap of 0 writes nothing.
        std::vector<uint8_t> none(sh.bytes, 0xEEu);
        CHECK_EQ(api()->read_image(s.raw(), id, nullptr, none.data(), 0), static_cast<int32_t>(sh.bytes));
        for (const uint8_t b : none) { CHECK_EQ(b, uint8_t{0xEE}); }
        // A cap with no buffer, and an info too short for the minimum, are bad arguments - and write nothing.
        CHECK_EQ(api()->read_image(s.raw(), id, nullptr, nullptr, 7), FOXAPI_BAD_ARGUMENT);
        FoxImageInfo tiny{};
        tiny.structSize = static_cast<uint32_t>(FOXAPI_MIN_IMAGE_INFO) - 1u;
        tiny.width = 0xDEADBEEFu;
        CHECK_EQ(api()->read_image(s.raw(), id, &tiny, nullptr, 0), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(tiny.width, 0xDEADBEEFu);
        // An info of exactly the minimum gets the 0.2 fields and NOT the appended ones (rule 4).
        FoxImageInfo base{};
        base.structSize = static_cast<uint32_t>(FOXAPI_MIN_IMAGE_INFO);
        base.complete = 0xCAFEu;
        base.sequence = 0xBEEFu;
        CHECK_EQ(api()->read_image(s.raw(), id, &base, nullptr, 0), static_cast<int32_t>(sh.bytes));
        CHECK_EQ(base.width, sh.w);
        CHECK_EQ(base.format, sh.format);
        CHECK_EQ(base.complete, 0xCAFEu);
        CHECK_EQ(base.sequence, uint64_t{0xBEEF});
        CHECK_EQ(base.structSize, static_cast<uint32_t>(FOXAPI_MIN_IMAGE_INFO));
        // A roomier info (a newer client) is served what there is and told how much.
        struct Roomy {
            FoxImageInfo i;
            uint64_t more[4];
        } roomy{};
        roomy.i.structSize = sizeof(roomy);
        roomy.more[0] = 0x1234u;
        CHECK_EQ(api()->read_image(s.raw(), id, &roomy.i, nullptr, 0), static_cast<int32_t>(sh.bytes));
        CHECK_EQ(roomy.i.structSize, static_cast<uint32_t>(sizeof(FoxImageInfo)));
        CHECK_EQ(roomy.more[0], uint64_t{0x1234});
    }
    // THE OTHER SESSIONS read exactly what a LOCAL one does: a VIEW-only session (the grant every session has), and a REMOTE
    // one (decoded pictures are decoded traffic like DECODER_LINES, which a REMOTE session reads).
    for (testing::Session* other : {&viewer, &remote}) {
        const Got o = readPicture(api(), other->raw(), id);
        CHECK(coherent(o));
        CHECK_EQ(o.info.sequence, g.info.sequence);
        CHECK(o.px == g.px);
        CHECK_EQ(std::string(o.info.caption), std::string("Fixture picture"));
        // ...and its row, in its own session.
        CHECK_EQ(liveRow(*other, 6, w.dec).id, static_cast<uint64_t>(w.dec));
        CHECK(!takeEvents(*other).empty());
    }

    // A STALE ID: once a newer picture has replaced it, it is NOT_FOUND, and the newer one is not. (NONE OF THE PICTURES
    // MOVES listSeq - they are PATCH_LIVE's, a block apart - and they arrive in this wait.)
    const uint64_t listSeqRunning = s.state().listSeq;
    // (Each pass, the row's ACTIVE is held to the picture it names: finished or still being received - the fixture makes both.)
    int sampled = 0, flagsRight = 0, finishedSeen = 0, receivingSeen = 0;
    CHECK(passUntil(d.eng,
                    [&] {
                        const FoxListItem r = liveRow(s, 6, w.dec);
                        const Got x = readPicture(api(), s.raw(), static_cast<uint64_t>(r.value[0]));
                        if (coherent(x)) {
                            ++sampled;
                            flagsRight += ((r.flags & FOXAPI_ITEM_ACTIVE) != 0u) == (x.info.complete == 1u) ? 1 : 0;
                            (x.info.complete == 1u ? finishedSeen : receivingSeen) += 1;
                        }
                        return static_cast<uint64_t>(r.value[0]) > id;
                    },
                    20000));
    CHECK(sampled >= 1);
    CHECK_EQ(flagsRight, sampled);
    CHECK(finishedSeen + receivingSeen == sampled);
    FoxImageInfo stale{};
    stale.structSize = sizeof(stale);
    CHECK_EQ(api()->read_image(s.raw(), id, &stale, nullptr, 0), FOXAPI_NOT_FOUND);
    CHECK(api()->read_image(s.raw(), static_cast<uint64_t>(liveRow(s, 6, w.dec).value[0]), &stale, nullptr, 0) > 0);

    CHECK_EQ(s.state().listSeq, listSeqRunning);

    // STOPPED: the picture stays on its face - the row, the id, the pixels - and nothing more arrives.
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_PATCH_RUN, 0, 0)).status(), FOXAPI_OK);
    passes(d.eng, 4);
    (void)takeEvents(s);
    const FoxListItem kept = liveRow(s, 6, w.dec);
    CHECK_EQ(kept.id, static_cast<uint64_t>(w.dec));
    const Got keptPic = readPicture(api(), s.raw(), static_cast<uint64_t>(kept.value[0]));
    CHECK(coherent(keptPic));
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    passes(d.eng, 6);
    CHECK_EQ(liveRow(s, 6, w.dec).value[0], kept.value[0]);
    for (const FoxEvent& e : takeEvents(s)) { CHECK(e.kind != FOXAPI_EVENT_DECODER_IMAGE); }

    // A LATE SESSION has no event for the picture and reads it all the same: from the row.
    {
        testing::Session late(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW);
        CHECK(late.ok());
        const Got l = readPicture(api(), late.raw(), static_cast<uint64_t>(liveRow(late, 6, w.dec).value[0]));
        CHECK(coherent(l));
        CHECK_EQ(l.info.sequence, keptPic.info.sequence);
        CHECK(takeEvents(late).empty());
    }

    // A NODE THE GRAPH NO LONGER HOLDS takes its picture with it: the row, and the id.
    {
        pc::Graph without = graphOf(s);
        CHECK(without.removeNode(w.dec));
        CHECK_EQ(g13PumpedGraph(d.eng, s, without).result.status, FOXAPI_OK);
        passes(d.eng, 3);
        CHECK_EQ(liveRow(s, 6, w.dec).id, 0u);
        FoxImageInfo gone{};
        gone.structSize = sizeof(gone);
        CHECK_EQ(api()->read_image(s.raw(), static_cast<uint64_t>(kept.value[0]), &gone, nullptr, 0), FOXAPI_NOT_FOUND);
        for (const FoxEvent& e : takeEvents(s)) { CHECK(e.kind != FOXAPI_EVENT_DECODER_IMAGE); }
    }
    std::printf("G13c-1: the row, the event, the buffer protocol, the sessions, the stale and the removed id: done\n");
}

// G13c-2: a new patch document is a new set of nodes that may carry the old ones' ids: it takes every picture with it.
void testPatchPicturesDocument() {
    using namespace g13;
    using namespace g13b;
    using namespace g13c;
    ImageWorld world;
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    passes(d.eng, 3);
    const Pic w = picturePatch(s);
    CHECK_EQ(pumpedText(d.eng, s, setGraph(FOXAPP_PATCH_GRAPH_DOCUMENT, 0), pc::graphCommandText(w.p.g)).result.status, FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_PATCH_RUN, 0, 1)).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return liveRow(s, 6, w.dec).id != 0u; }, 20000));
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_PATCH_RUN, 0, 0)).status(), FOXAPI_OK);
    passes(d.eng, 3);
    const uint64_t id = static_cast<uint64_t>(liveRow(s, 6, w.dec).value[0]);
    CHECK(coherent(readPicture(api(), s.raw(), id)));
    // A SET_GRAPH that is not a document edits: the picture stays on its node.
    {
        pc::Graph g = graphOf(s);
        if (pc::Node* n = g.mutableNode(w.dec)) { n->x += 10.0f; }
        CHECK_EQ(g13PumpedGraph(d.eng, s, g).result.status, FOXAPI_OK);
        passes(d.eng, 3);
        CHECK_EQ(static_cast<uint64_t>(liveRow(s, 6, w.dec).value[0]), id);
    }
    // The same graph again as a DOCUMENT (the same node ids): the old picture is not shown on the new node.
    const uint64_t epoch = epochOf(s);
    CHECK_EQ(pumpedText(d.eng, s, setGraph(FOXAPP_PATCH_GRAPH_DOCUMENT, 0), pc::graphCommandText(graphOf(s))).result.status, FOXAPI_OK);
    passes(d.eng, 3);
    CHECK(epochOf(s) > epoch);
    CHECK_EQ(liveRow(s, 6, w.dec).id, 0u);
    FoxImageInfo info{};
    info.structSize = sizeof(info);
    CHECK_EQ(api()->read_image(s.raw(), id, &info, nullptr, 0), FOXAPI_NOT_FOUND);
    std::printf("G13c-2: an edit keeps the picture, a document takes it: done\n");
}

// G13c-3: a reader on its own thread never sees a torn picture. Two readers (a LOCAL session and a REMOTE one) read the
// newest picture as fast as they can while the pass thread takes pictures; every read that returns must follow the
// fixture's formula for the sequence it names, whole.
void testPatchPicturesNeverTear() {
    using namespace g13;
    using namespace g13b;
    using namespace g13c;
    ImageWorld world;
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    testing::Session rl(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW);
    testing::Session rr(api(), d.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW, "s3cret");
    CHECK(s.ok() && rl.ok() && rr.ok());
    passes(d.eng, 3);
    const Pic w = picturePatch(s);
    CHECK_EQ(pumpedText(d.eng, s, setGraph(FOXAPP_PATCH_GRAPH_DOCUMENT, 0), pc::graphCommandText(w.p.g)).result.status, FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_PATCH_RUN, 0, 1)).status(), FOXAPI_OK);
    std::atomic<bool> stop{false};
    struct Tally {
        std::atomic<long> reads{0}, torn{0}, notFound{0};
        std::mutex m;
        std::set<uint64_t> sequences;
    };
    Tally tl, tr;
    const auto reader = [&](testing::Session& session, Tally& t) {
        while (!stop.load()) {
            const FoxListItem row = liveRow(session, 6, w.dec);
            if (row.id == 0u) {
                std::this_thread::yield();
                continue;
            }
            const Got g = readPicture(api(), session.raw(), static_cast<uint64_t>(row.value[0]));
            if (g.status < 0) {
                ++t.notFound;  // moved on between the row and the read: allowed
                continue;
            }
            ++t.reads;
            if (!coherent(g)) { ++t.torn; }
            std::lock_guard<std::mutex> lk(t.m);
            t.sequences.insert(g.info.sequence);
        }
    };
    std::thread a(reader, std::ref(rl), std::ref(tl));
    std::thread b(reader, std::ref(rr), std::ref(tr));
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(4000);
    while (std::chrono::steady_clock::now() < end) {
        d.eng.controlPass();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    stop = true;
    a.join();
    b.join();
    std::printf("G13c-3: LOCAL reader %ld whole read(s), %ld torn, %ld moved on, %zu picture(s) seen; REMOTE reader %ld, %ld, %ld, %zu\n",
                tl.reads.load(), tl.torn.load(), tl.notFound.load(), tl.sequences.size(), tr.reads.load(), tr.torn.load(),
                tr.notFound.load(), tr.sequences.size());
    CHECK(tl.reads.load() > 100);
    CHECK(tr.reads.load() > 100);
    CHECK_EQ(tl.torn.load(), 0L);
    CHECK_EQ(tr.torn.load(), 0L);
    CHECK(tl.sequences.size() >= 3u);
    CHECK(tr.sequences.size() >= 3u);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_PATCH_RUN, 0, 0)).status(), FOXAPI_OK);
    passes(d.eng, 2);
}
// G13C TESTS END>>
// ======================================================================================================================
// G14b: the GPS reader as the position row draws it (window port group 14, part b, API 0.3 section 8.13): the GPS list,
// one entry read at the call, and the status code that forgets a finished read.
// ======================================================================================================================
namespace g14 {

// A receiver on a fake wire: delivers its chunks one after another (each after `delayMs`), then says nothing, every 20 ms.
class Wire final : public cascade::core::ByteSource {
public:
    struct Step {
        std::string chunk;
        int delayMs = 0;
    };
    explicit Wire(std::vector<Step> steps) : steps_(std::move(steps)) {}
    int read(char* buf, std::size_t cap) override {
        if (next_ < steps_.size()) {
            const Step& s = steps_[next_++];
            std::this_thread::sleep_for(std::chrono::milliseconds(s.delayMs));
            const std::size_t n = std::min(s.chunk.size(), cap);
            std::memcpy(buf, s.chunk.data(), n);
            return static_cast<int>(n);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return 0;
    }

private:
    std::vector<Step> steps_;
    std::size_t next_ = 0;
};

std::string framed(const char* payload) { return cascade::core::nmeaFrame(payload) + "\r\n"; }
// 8 satellites in view, no fix yet; and a fix at 51.50735 N, 0.12776 W with 8 satellites and an HDOP of 1.1.
const char* kViewPayload = "GPGGA,123519.00,,,,,0,08,1.1,,M,,M,,";
const char* kFixPayload = "GPGGA,123519.00,5130.4410,N,00007.6656,W,1,08,1.1,35.0,M,46.9,M,,";

std::unique_ptr<cascade::core::ByteSource> viewing() {
    return std::make_unique<Wire>(std::vector<Wire::Step>{{framed(kViewPayload), 0}});
}
std::unique_ptr<cascade::core::ByteSource> fixing() {
    return std::make_unique<Wire>(std::vector<Wire::Step>{{framed(kViewPayload), 0}, {framed(kFixPayload), 60}});
}

}  // namespace g14

void testGpsList() {
    using Access = cascade::gui::AppWindowTestAccess;
    using cascade::core::GpsReader;
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    testing::Session r(api(), d.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_SETTINGS, "s3cret");
    CHECK(s.ok());
    CHECK(r.ok());
    CHECK_EQ(FOXAPI_LIST_GPS, 48u);
    const auto row = [&](testing::Session& who) { return item(who, FOXAPI_LIST_GPS, 0); };
    const auto untilState = [&](double state, int ms = 4000) {
        return passUntil(d.eng, [&] { return row(s).value[0] == state; }, ms);
    };

    // IDLE: one entry, always, saying nothing - no port, no sentence, no satellites, neither flag.
    d.eng.controlPass();
    CHECK_EQ(count(s, FOXAPI_LIST_GPS), 1);
    CHECK_EQ(count(r, FOXAPI_LIST_GPS), 1);
    {
        const FoxListItem it = row(s);
        CHECK_EQ(it.list, uint32_t(FOXAPI_LIST_GPS));
        CHECK_EQ(it.id, 1ull);
        CHECK_EQ(it.value[0], 0.0);
        CHECK_EQ(it.value[2], -1.0);
        CHECK_EQ(it.flags & (FOXAPI_ITEM_ACTIVE | FOXAPI_ITEM_FAULT), 0u);
        CHECK_EQ(std::string(it.name), std::string());
        CHECK_EQ(std::string(it.detail), std::string());
        // An index past the one entry is not an entry (the count says so and nothing is filled).
        FoxListItem past{};
        past.structSize = sizeof(past);
        CHECK_EQ(api()->read_list(s.raw(), FOXAPI_LIST_GPS, 1, &past), 1);
        CHECK_EQ(past.list, 0u);
    }
    // The entry is not part of the lists' block: the count of lists the block rebuilds on is unmoved by anything below.
    const std::uint64_t listSeq0 = s.state().listSeq;

    // LISTENING: started by the op through the engine's opener (a receiver that sees 8 satellites and no fix).
    Access::gpsOpener(d.eng, [](const std::string&, int, std::string&) { return g14::viewing(); });
    FoxCommand start = testing::cmd(FOXAPI_OP_GPS, static_cast<double>(9600), 1, "FAKE1");
    CHECK_EQ(pumped(d.eng, s, start).status(), FOXAPI_OK);
    CHECK(passUntil(d.eng, [&] { return row(s).value[0] == 1.0 && row(s).value[2] == 8.0; }));
    {
        const FoxListItem it = row(s);
        std::printf("G14b-1: listening: name [%s] detail [%s] state %.0f seconds %.2f satellites %.0f baud %.0f flags %08x\n",
                    it.name, it.detail, it.value[0], it.value[1], it.value[2], it.value[3], it.flags);
        CHECK((it.flags & FOXAPI_ITEM_ACTIVE) != 0u);
        CHECK((it.flags & FOXAPI_ITEM_FAULT) == 0u);
        CHECK_EQ(std::string(it.name), std::string("FAKE1"));
        CHECK(std::string(it.detail).rfind("Listening on FAKE1 at 9600 - 8 satellites, HDOP 1.1, no fix yet (", 0) == 0);
        CHECK_EQ(it.value[3], 9600.0);
        CHECK(it.value[1] >= 0.0 && it.value[1] < 30.0);
        // The sentence is the core's own, whole - the engine composes it, so its wording is tested once.
        CHECK_EQ(std::string(it.detail), cascade::core::gpsStatusLine(Access::gpsStatus(d.eng)).substr(0, 159));
    }
    // ...the seconds are LIVE (a read later, a larger figure, no pass between) ...
    {
        const double a = row(s).value[1];
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        CHECK(row(s).value[1] > a);
    }
    // ...and nothing a listening reader does moves the lists: not the seconds, not the sentence, not the satellites.
    for (int i = 0; i < 12; ++i) {
        d.eng.controlPass();
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    CHECK_EQ(s.state().listSeq, listSeq0);

    // A REMOTE session sees the numbers and the flags, and none of the words (the port is this machine's).
    {
        const FoxListItem it = row(r);
        CHECK_EQ(it.value[0], 1.0);
        CHECK_EQ(it.value[2], 8.0);
        CHECK_EQ(it.value[3], 9600.0);
        CHECK((it.flags & FOXAPI_ITEM_ACTIVE) != 0u);
        CHECK_EQ(std::string(it.name), std::string());
        CHECK_EQ(std::string(it.detail), std::string());
    }

    // CLEARING A RUNNING READ DOES NOTHING (FOXAPP_STATUS_GPS_RESULT is the reader's own clearResult), and a REMOTE session
    // cannot send it (an app op).
    const FoxCommand clearResult = appCmd(FOXAPP_OP_CLEAR_STATUS, cascade::core::cmd::FOXAPP_STATUS_GPS_RESULT);
    CHECK(static_cast<int>(cascade::core::cmd::FOXAPP_STATUS_GPS_RESULT) == 6);
    CHECK_EQ(pumped(d.eng, s, clearResult).status(), FOXAPI_OK);
    CHECK_EQ(row(s).value[0], 1.0);
    CHECK_EQ(pumped(d.eng, r, clearResult).status(), FOXAPI_DENIED);
    CHECK_EQ(row(s).value[0], 1.0);

    // STOP: the reader returns to Idle (a listen cut short), and the entry is empty again.
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_GPS, 0.0, 0)).status(), FOXAPI_OK);
    CHECK(untilState(0.0));
    {
        const FoxListItem it = row(s);
        CHECK_EQ(it.flags & (FOXAPI_ITEM_ACTIVE | FOXAPI_ITEM_FAULT), 0u);
        CHECK_EQ(std::string(it.name), std::string());
        CHECK_EQ(std::string(it.detail), std::string());
    }

    // FIXED: the reader takes the fix, closes the port; the engine's pass applies the position (the state's, not the entry's)
    // and the entry says Fixed, not listening, with the sentence the core words it and NO coordinate anywhere.
    Access::gpsOpener(d.eng, [](const std::string&, int, std::string&) { return g14::fixing(); });
    CHECK_EQ(pumped(d.eng, s, start).status(), FOXAPI_OK);
    CHECK(untilState(2.0));
    CHECK(passUntil(d.eng, [&] { return (s.state().flags & FOXAPI_RX_POSITION_SET) != 0u; }));
    {
        const FoxListItem it = row(s);
        std::printf("G14b-1: fixed: detail [%s] satellites %.0f flags %08x; the state's position %.5f %.5f\n", it.detail, it.value[2],
                    it.flags, s.state().rxLatDeg, s.state().rxLonDeg);
        CHECK_EQ(it.flags & (FOXAPI_ITEM_ACTIVE | FOXAPI_ITEM_FAULT), 0u);
        CHECK_EQ(std::string(it.detail), std::string("Fix: 8 satellites, HDOP 1.1 - position set"));
        CHECK_EQ(it.value[2], 8.0);
        CHECK_EQ(std::string(it.name), std::string("FAKE1"));
        for (const char* digits : {"51.5", "0.127", "5130", "00007", "7.66"}) {
            CHECK(std::string(it.detail).find(digits) == std::string::npos);
            CHECK(std::string(it.name).find(digits) == std::string::npos);
        }
        CHECK(std::fabs(s.state().rxLatDeg - 51.50735) < 1e-4);
        CHECK(std::fabs(s.state().rxLonDeg + 0.12776) < 1e-4);
    }
    // ...and the entry stays Fixed until it is cleared: CLEAR_STATUS 6 forgets the finished read.
    for (int i = 0; i < 6; ++i) { d.eng.controlPass(); }
    CHECK_EQ(row(s).value[0], 2.0);
    CHECK_EQ(pumped(d.eng, s, clearResult).status(), FOXAPI_OK);
    CHECK_EQ(row(s).value[0], 0.0);
    CHECK_EQ(std::string(row(s).detail), std::string());

    // FAILED: a port that cannot be opened. FAULT, not listening, the sentence names the port and carries the OS's words.
    Access::gpsOpener(d.eng, [](const std::string&, int, std::string& error) -> std::unique_ptr<cascade::core::ByteSource> {
        error = "no such device";  // (the reader words it: "<port> could not be opened: <this>")
        return nullptr;
    });
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_GPS, 4800.0, 1, "NOPE9")).status(), FOXAPI_OK);
    CHECK(untilState(3.0));
    {
        const FoxListItem it = row(s);
        std::printf("G14b-1: failed: detail [%s] flags %08x\n", it.detail, it.flags);
        CHECK((it.flags & FOXAPI_ITEM_FAULT) != 0u);
        CHECK((it.flags & FOXAPI_ITEM_ACTIVE) == 0u);
        CHECK_EQ(std::string(it.detail), std::string("NOPE9 could not be opened: no such device"));
        CHECK_EQ(it.value[3], 4800.0);
        CHECK_EQ(std::string(row(r).detail), std::string());  // (the REMOTE one is blank: it names the port)
        CHECK((row(r).flags & FOXAPI_ITEM_FAULT) != 0u);
    }
    CHECK_EQ(pumped(d.eng, s, clearResult).status(), FOXAPI_OK);
    CHECK_EQ(row(s).value[0], 0.0);

    // TIMED OUT: a receiver that says nothing for as long as the read was given (0.25 s here, 60 s by the op).
    Access::gpsStart(d.eng, std::make_unique<g14::Wire>(std::vector<g14::Wire::Step>{}), "FAKE2", 9600, 0.25);
    CHECK(untilState(4.0));
    {
        const FoxListItem it = row(s);
        std::printf("G14b-1: timed out: detail [%s] flags %08x\n", it.detail, it.flags);
        CHECK((it.flags & FOXAPI_ITEM_FAULT) != 0u);
        CHECK((it.flags & FOXAPI_ITEM_ACTIVE) == 0u);
        CHECK(std::string(it.detail).rfind("Nothing arrived from FAKE2 in ", 0) == 0);
    }
    // The whole run of states moved no list.
    CHECK_EQ(s.state().listSeq, listSeq0);
    Access::gpsStop(d.eng);
}

// ======================================================================================================================
// G14c: the usage switch and the session counters as facts, and the diagnostics context the engine renders itself
// (window port group 14, part c, API 0.3 section 8.13).
// ======================================================================================================================
void testTelemetryFacts() {
    using Access = cascade::gui::AppWindowTestAccess;
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    testing::Session r(api(), d.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_SETTINGS, "s3cret");
    CHECK(s.ok());
    CHECK(r.ok());
    const FoxCommand consent1 = appCmd(FOXAPP_OP_TELEMETRY_CONSENT, 1);
    const FoxCommand consent0 = appCmd(FOXAPP_OP_TELEMETRY_CONSENT, 0);
    const auto enabled = [&] { return fact(s, "fact.telemetry.enabled"); };
    const auto id = [&] { return fact(s, "fact.telemetry.installId"); };
    d.eng.controlPass();
    d.eng.controlPass();

    // OFF, the engine's start: "0" and no id - known and EMPTY (a status of 0, not NOT_FOUND) - and the counters as decimals.
    int32_t st = 0;
    CHECK_EQ(enabled(), std::string("0"));
    CHECK_EQ(fact(s, "fact.telemetry.installId", &st), std::string());
    CHECK_EQ(st, 0);
    CHECK_EQ(fact(s, "fact.diag.launches"), std::string("0"));
    CHECK_EQ(fact(s, "fact.diag.crashes"), std::string("0"));
    // ...LOCAL ones: a REMOTE session is DENIED all four, whether the switch is off or on, and learns nothing of the id.
    for (const char* key : {"fact.telemetry.enabled", "fact.telemetry.installId", "fact.diag.launches", "fact.diag.crashes"}) {
        int32_t rs = 0;
        CHECK_EQ(fact(r, key, &rs), std::string());
        CHECK_EQ(rs, FOXAPI_DENIED);
    }

    // CONSENT: the switch is on and the id is minted AT THE MOMENT OF CONSENT - one block, so the two facts move together.
    const std::uint64_t seq0 = s.state().listSeq;
    CHECK_EQ(pumped(d.eng, s, consent1).status(), FOXAPI_OK);
    d.eng.controlPass();
    CHECK_EQ(enabled(), std::string("1"));
    const std::string first = id();
    std::printf("G14c-1: consent: fact.telemetry.enabled %s, an id of %zu characters, listSeq %llu -> %llu\n", enabled().c_str(),
                first.size(), static_cast<unsigned long long>(seq0), static_cast<unsigned long long>(s.state().listSeq));
    CHECK(!first.empty());
    CHECK_EQ(first, Access::telemetryId(d.eng));  // the engine's own id, whole
    CHECK(Access::telemetryOn(d.eng));
    CHECK_EQ(s.state().listSeq, seq0 + 1u);
    for (const char* key : {"fact.telemetry.enabled", "fact.telemetry.installId"}) {
        int32_t rs = 0;
        (void)fact(r, key, &rs);
        CHECK_EQ(rs, FOXAPI_DENIED);
    }
    // A REPEAT changes nothing: the id a machine reports under does not change behind its back, and nothing moves.
    CHECK_EQ(pumped(d.eng, s, consent1).status(), FOXAPI_NO_CHANGE);
    d.eng.controlPass();
    CHECK_EQ(id(), first);
    CHECK_EQ(s.state().listSeq, seq0 + 1u);

    // OFF forgets the id (the fact is "" - the id does not exist while reporting is off), once.
    CHECK_EQ(pumped(d.eng, s, consent0).status(), FOXAPI_OK);
    d.eng.controlPass();
    CHECK_EQ(enabled(), std::string("0"));
    CHECK_EQ(id(), std::string());
    CHECK_EQ(Access::telemetryId(d.eng), std::string());
    CHECK_EQ(s.state().listSeq, seq0 + 2u);
    // ...and a later opt-in cannot be tied to the old one.
    CHECK_EQ(pumped(d.eng, s, consent1).status(), FOXAPI_OK);
    d.eng.controlPass();
    const std::string second = id();
    CHECK(!second.empty());
    CHECK(second != first);
    // The API's own switch is refused from everyone, and moves nothing.
    const std::uint64_t seqNow = s.state().listSeq;
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_TELEMETRY_ENABLE, 0.0, 0)).status(), FOXAPI_DENIED);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_TELEMETRY_ENABLE, 0.0, 1)).status(), FOXAPI_DENIED);
    d.eng.controlPass();
    CHECK_EQ(enabled(), std::string("1"));
    CHECK_EQ(id(), second);
    CHECK_EQ(s.state().listSeq, seqNow);
    // A REMOTE session cannot send the desktop's op either (an app op).
    CHECK_EQ(pumped(d.eng, r, consent0).status(), FOXAPI_DENIED);
    CHECK_EQ(enabled(), std::string("1"));

    // THE COUNTERS: decimals, each one a move of the lists when it changes (they change at start-up, from the config).
    const std::uint64_t seqC = s.state().listSeq;
    Access::setCounters(d.eng, 12, 0);
    d.eng.controlPass();
    CHECK_EQ(fact(s, "fact.diag.launches"), std::string("12"));
    CHECK_EQ(fact(s, "fact.diag.crashes"), std::string("0"));
    CHECK_EQ(s.state().listSeq, seqC + 1u);
    Access::setCounters(d.eng, 12, 3);
    d.eng.controlPass();
    CHECK_EQ(fact(s, "fact.diag.crashes"), std::string("3"));
    CHECK_EQ(s.state().listSeq, seqC + 2u);
    // Settled passes move nothing.
    for (int i = 0; i < 5; ++i) { d.eng.controlPass(); }
    CHECK_EQ(s.state().listSeq, seqC + 2u);
    std::printf("G14c-1: launches %s, crashes %s; the id is gone at off, new at the next consent, and the API's own switch is refused\n",
                fact(s, "fact.diag.launches").c_str(), fact(s, "fact.diag.crashes").c_str());
}

void testDiagContextIsTheEngines() {
    using Access = cascade::gui::AppWindowTestAccess;
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    d.eng.controlPass();
    // RENDERED BY THE ENGINE'S OWN PASS: nobody called refreshDiagContext, and the crash handler's block is there.
    std::string block = cascade::core::diagContextBlock();
    std::printf("G14c-2: the context after the first pass:\n%s", block.c_str());
    CHECK(block.find(std::string("version: ") + cascade::versionString() + "\n") != std::string::npos);
    CHECK(block.find("device-open: no\n") != std::string::npos);
    CHECK(block.find("plugin: ") != std::string::npos);
    const std::string modeLine = [&] {
        const std::size_t at = block.find("mode: ");
        return at == std::string::npos ? std::string() : block.substr(at, block.find('\n', at) - at);
    }();
    CHECK(!modeLine.empty());

    // A CHANGE OF MODE shows in the block within the pass's second (the gate is one second; the pass re-renders only when
    // an input of the context moved).
    const auto activeMode = [&] {
        for (int32_t i = 0; i < count(s, FOXAPI_LIST_MODES); ++i) {
            const FoxListItem it = item(s, FOXAPI_LIST_MODES, static_cast<uint32_t>(i));
            if ((it.flags & FOXAPI_ITEM_ACTIVE) != 0u) { return std::string(it.name); }
        }
        return std::string();
    };
    const uint32_t other = (s.state().demodMode == FOXAPI_DEMOD_AM) ? FOXAPI_DEMOD_NFM : FOXAPI_DEMOD_AM;
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SET_MODE, 0.0, other)).status(), FOXAPI_OK);
    const std::string wanted = "mode: " + activeMode();
    CHECK(wanted != modeLine);
    const bool followed = passUntil(d.eng, [&] { return cascade::core::diagContextBlock().find(wanted) != std::string::npos; }, 3000);
    std::printf("G14c-2: after SET_MODE the block says [%s]: %s\n", wanted.c_str(), followed ? "yes" : "NO");
    CHECK(followed);

    // A RADIO OPENING shows too (the device flag and the model it came with).
    const bool opened = openRadio(d.eng, s, engine_api_test::kFakeRadioId);
    CHECK(opened);
    CHECK(passUntil(d.eng, [&] { return cascade::core::diagContextBlock().find("device-open: yes\n") != std::string::npos; }, 3000));
    CHECK(cascade::core::diagContextBlock().find("sdr-model: (none)\n") == std::string::npos);

    // THE RADIO'S MODEL ALONE is an input of the context (the device is open, the model is what a report names it by).
    (void)passUntil(d.eng, [] { return false; }, 1600);  // (everything else settles first: the model is the only thing that moves)
    CHECK(cascade::core::diagContextBlock().find("sdr-model: uhd b200\n") == std::string::npos);
    Access::setDeviceModel(d.eng, "uhd b200");
    CHECK(passUntil(d.eng, [&] { return cascade::core::diagContextBlock().find("sdr-model: uhd b200\n") != std::string::npos; }, 3000));

    // THE BUNDLE TAKES THE BLOCK THE ENGINE RENDERED, whole: no other context, none of its own.
    const std::string engines = cascade::core::diagContextBlock();
    cascade::core::DiagBundleInput in;
    in.contextBlock = engines;
    const std::string bundle = cascade::core::buildDiagnosticsBundle(in);
    CHECK(bundle.find(engines) != std::string::npos);
    CHECK(cascade::core::diagContextBlock() == engines);  // (building it replaced nothing)

    // IT DOES NOT FLAP: a pass that finds the inputs unmoved renders nothing (the block's bytes are the same object).
    for (int i = 0; i < 6; ++i) { d.eng.controlPass(); }
    CHECK(cascade::core::diagContextBlock() == engines + std::string());
}

// ...and the config's restore ends with it: a context stands the moment applyConfig returns, before any pass (the window used
// to render it at the top of run(); a report from a start-up that never reached a pass still describes the session).
void testDiagContextAtApplyConfig() {
    cascade::core::DiagContext sentinel;
    sentinel.version = "sentinel";
    sentinel.mode = "none";
    cascade::core::setDiagContext(sentinel);
    CHECK(cascade::core::diagContextBlock().find("version: sentinel") != std::string::npos);
    cascade::engine::Engine eng;
    eng.configureApi(kHooks);
    eng.initialise();
    cascade::core::AppConfig cfg;
    eng.applyConfig(cfg);
    const std::string block = cascade::core::diagContextBlock();
    std::printf("G14c-2: the context the moment applyConfig returned, no pass yet: %s\n",
                block.find("sentinel") == std::string::npos ? "the engine's" : "STILL THE SENTINEL");
    CHECK(block.find("sentinel") == std::string::npos);
    CHECK(block.find(std::string("version: ") + cascade::versionString() + "\n") != std::string::npos);
}

// --- N: a fitted plugin runs only while it is used (stub campaign B2, PLUGIN-RUN; 0.99.73) -------------------------------------
//
// The engine decides, once a pass, which plugins have instances: a plugin set to AUTO runs while something uses it (a window shown, a map
// page, the radar scope, a patch node, the speakers, a text sink, a browser, the Decoder output window for a text decoder), goes dormant
// FOXSDR_DORMANT_AFTER_MS after the last of those ends, and wakes again on the next; ALWAYS (pinned) runs without a signal; STOPPED never
// runs. A WINDOW-LESS host - no interface has installed its plugin UI applier - treats AUTO as pinned. The signals only a window can see
// arrive as FOXAPP_OP_PLUGIN_USE, the run state as FOXAPP_OP_PLUGIN_RUN_MODE.
//
// THE WORLD: three REAL modules in a scratch folder - the wake probe (tests/fixture_wake_probe_plugin.cpp: an audio text decoder that says
// one line per block it is handed, naming the block by its first sample), the group-14 track source with a panel and an instrument (the
// plugin UI's half: tracks, panels), and the group-9 audio processor (a standing duty). The "window" is a plugin UI that applies the
// engine's directives exactly as the interface's feed does (PluginUi::apply).
//
//   N1  AT START, with a window, nothing is running: no decoder instance, no UI instance, PLUGINS says AUTO and ASLEEP (value[3] = 4) -
//       and the same folder under an engine with NO window runs everything (value[3] = 0): the owner's decision, today's behaviour;
//   N2  THE FIRST FRAME OF A WAKE: the window's signal is applied at the head of the next pass and the plugin is created in THAT pass; the
//       first block handed to the runner after it is the first block the decoder sees - and a block handed before it was seen by nobody.
//       The same pass wakes the UI half (the track source's tracks, the panel). A signal a module cannot show (a window bit for a decoder
//       with no window of its own) wakes nothing;
//   N3  THE HYSTERESIS: the signal gone, the plugin keeps running for the interval and is put down after it (instances gone in both
//       halves, one log line), and the next signal creates a NEW instance;
//   N4  THE RUN STATES: ALWAYS runs with no signal at all, and un-pinned goes down only after the interval; STOPPED never wakes, whatever
//       is shown; the op's refusals; a REMOTE session is DENIED both ops;
//   N5  A STANDING DUTY (an in-chain audio processor) is never asleep, and says so;
//   N6  THE CONFIG: pluginRun is restored and written; the old file-name stop list is migrated into it by id and written empty; an
//       explicit AUTO wins over a stale legacy stop;
//   N7  THE WINDOW GOES: an engine whose window disconnects is window-less again, and wakes what was asleep.
namespace gn {

using cascade::core::PluginRun;
using Access = cascade::gui::AppWindowTestAccess;

struct World {
    std::filesystem::path dir;
    static std::string hook;
    World() {
        const char* base = std::getenv("LOCALAPPDATA");
        dir = std::filesystem::path(base != nullptr ? base : ".") / "plugins_groupN";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        for (const char* src : {FIXTURE_WAKE_PROBE_PLUGIN, FIXTURE_GROUP14_PLUGIN, FIXTURE_GROUP9_PLUGIN}) {
            const std::filesystem::path p(src);
            std::filesystem::copy_file(p, dir / p.filename(), std::filesystem::copy_options::overwrite_existing, ec);
            CHECK(!ec);
        }
        hook = dir.string();
        Access::setPluginDir(&World::path);
    }
    ~World() {
        Access::setPluginDir(nullptr);
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    static std::string path() { return hook; }
};
std::string World::hook;

const std::string kWake = std::filesystem::path(FIXTURE_WAKE_PROBE_PLUGIN).filename().string();
const std::string kTracks = std::filesystem::path(FIXTURE_GROUP14_PLUGIN).filename().string();
const char* const kWakeName = "Wake Probe";
const char* const kTracksName = "Group Fourteen Tracks";
const char* const kProcessorName = "Group Nine Fixture";

// An engine with a "window": the plugin UI the interface's feed would hold, installed BEFORE the first scan as the feed does.
struct Windowed {
    cascade::engine::Engine eng;
    cascade::core::PluginUi ui;
    Windowed() : ui(eng.pluginApiShared()) {
        eng.configureApi(kHooks);
        Access::installWindow(eng, ui);
        eng.initialise();
        eng.controlPass();
    }
    ~Windowed() { Access::removeWindow(eng); }
};

bool awake(cascade::engine::Engine& e, const std::string& file) { return Access::runner(e).hasInstances(file); }

// One block into the runner as the DSP thread would hand it (the receiver is not running here): `first` is its first sample, and what
// the wake probe names it by. Returns the lines the decoders said about it.
std::vector<std::string> feed(cascade::engine::Engine& e, float first, std::size_t frames = 480) {
    std::vector<float> block(frames, 0.0f);
    block[0] = first;
    Access::runner(e).processAudio(block.data(), frames);
    std::vector<std::string> out;
    for (const cascade::core::DecodedLine& l : Access::runner(e).drainText()) { out.push_back(l.text); }
    return out;
}

std::size_t logged(const std::string& needle) {
    std::size_t n = 0;
    for (const std::string& line : cascade::core::DiagLog::instance().ringSnapshot()) {
        if (line.find(needle) != std::string::npos) { ++n; }
    }
    return n;
}

// The table's word for a module: PLUGINS value[3] (bits 0-1 how it runs, bit 2 asleep).
int runWord(testing::Session& s, const char* name) { return static_cast<int>(entryNamed(s, FOXAPI_LIST_PLUGINS, name).value[3]); }

FoxCommand useCmd(const std::string& table) { return testing::cmd(FOXAPP_OP_PLUGIN_USE, 0.0, 0, table.c_str()); }
FoxCommand runCmd(const std::string& file, int state) { return testing::cmd(FOXAPP_OP_PLUGIN_RUN_MODE, 0.0, state, file.c_str()); }

// A command submitted and the NEXT pass - exactly one - run: the way the window's report meets the engine.
void submitThenOnePass(cascade::engine::Engine& e, testing::Session& s, const FoxCommand& c) {
    FoxSubmitResult sr{};
    sr.structSize = sizeof(sr);
    const int32_t n = api()->submit(s.raw(), &c, 1, &sr);
    CHECK_EQ(n, 1);
    CHECK_EQ(sr.status, FOXAPI_OK);
    e.controlPass();
}

void testAtStart() {
    World world;
    {
        Windowed w;
        testing::Session s(api(), w.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
        CHECK(s.ok());
        passes(w.eng, 3);
        std::printf("N1: with a window: wake probe word %d, tracks word %d, processor word %d\n", runWord(s, kWakeName),
                    runWord(s, kTracksName), runWord(s, kProcessorName));
        CHECK_EQ(runWord(s, kWakeName), 4);    // AUTO (0) and asleep (4)
        CHECK_EQ(runWord(s, kTracksName), 4);
        CHECK(!awake(w.eng, kWake));
        w.ui.poll();
        CHECK(w.ui.trackPluginNames().empty());   // the UI half has nothing either
        CHECK(w.ui.panels().empty());
        CHECK(feed(w.eng, 0.25f).empty());         // a block handed to nobody
        // the group-9 processor is a standing duty: never asleep
        CHECK_EQ(runWord(s, kProcessorName) & 4, 0);
    }
    {
        // THE OWNER'S DECISION (docs/engine-stage4.md 6.22): no window, no signal could ever arrive - AUTO is pinned, as it always was.
        Direct d;
        testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
        CHECK(s.ok());
        passes(d.eng, 3);
        std::printf("N1: with no window: wake probe word %d, tracks word %d\n", runWord(s, kWakeName), runWord(s, kTracksName));
        CHECK_EQ(runWord(s, kWakeName), 0);
        CHECK_EQ(runWord(s, kTracksName), 0);
        CHECK(awake(d.eng, kWake));
        CHECK_EQ(feed(d.eng, 0.25f).size(), 1u);
        // ...and it stays so however long it runs with nothing shown
        (void)passUntil(d.eng, [] { return false; }, 700);
        CHECK(awake(d.eng, kWake));
    }
}

void testFirstFrameOfAWake() {
    World world;
    Windowed w;
    testing::Session s(api(), w.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    passes(w.eng, 3);
    CHECK(!awake(w.eng, kWake));

    // A block before the signal is seen by nobody.
    CHECK(feed(w.eng, 0.125f).empty());

    // THE SIGNAL: the Decoder output window is open (a text decoder is wanted by it), and the group-14 plugin's window is shown.
    // Submitted, NOT yet applied: the window's signal is one pass late by design, and until that pass the plugin is as asleep as ever.
    FoxSubmitResult sr{};
    sr.structSize = sizeof(sr);
    const FoxCommand use = useCmd(kWake + "\t8\n" + kTracks + "\t1\n");
    CHECK_EQ(api()->submit(s.raw(), &use, 1, &sr), 1);
    CHECK(!awake(w.eng, kWake));
    CHECK(feed(w.eng, 0.25f).empty());      // (still nobody)

    // ONE pass later the plugin exists - created IN that pass, not the one after - and the very next block is its first.
    w.eng.controlPass();
    CHECK(awake(w.eng, kWake));
    const std::vector<std::string> first = feed(w.eng, 0.5f);
    std::printf("N2: the first block after the wake pass: %zu line(s)%s%s\n", first.size(), first.empty() ? "" : " - ",
                first.empty() ? "" : first[0].c_str());
    CHECK_EQ(first.size(), 1u);
    if (!first.empty()) {
        // THE FIRST FRAME IT SHOULD DECODE: block 0.5 (the first after the wake) and not a later one, whole (480 frames).
        CHECK(first[0].find("first=0.500") != std::string::npos);
        CHECK(first[0].find("frames=480") != std::string::npos);
    }
    const std::vector<std::string> second = feed(w.eng, 0.75f);
    CHECK_EQ(second.size(), 1u);   // and the next after it, in order
    if (!second.empty()) { CHECK(second[0].find("first=0.750") != std::string::npos); }
    CHECK_EQ(logged("plugin: Wake Probe woke - decoder output window open"), 1u);

    // THE UI HALF woke in the same pass, through the same directive path: the track source's targets and the panel exist.
    w.ui.poll();
    CHECK(!w.ui.trackPluginNames().empty());
    CHECK(!w.ui.panels().empty());
    CHECK_EQ(logged("plugin: Group Fourteen Tracks woke - window open"), 1u);
    CHECK_EQ(runWord(s, kWakeName), 0);    // awake: no longer ASLEEP
    CHECK_EQ(runWord(s, kTracksName), 0);

    // A SIGNAL THE MODULE CANNOT SHOW wakes nothing: the wake probe is a decoder with no window of its own, so a "window shown" bit is
    // the window's mistake. (Clear everything first: both go down after the interval.)
    submitThenOnePass(w.eng, s, useCmd(std::string()));
    CHECK(passUntil(w.eng, [&] { return !awake(w.eng, kWake); }, 4000));
    submitThenOnePass(w.eng, s, useCmd(kWake + "\t1\n"));
    passes(w.eng, 3);
    CHECK(!awake(w.eng, kWake));
    // ...nor does a bit it was not given: the map page of a decoder.
    submitThenOnePass(w.eng, s, useCmd(kWake + "\t2\n"));
    passes(w.eng, 3);
    CHECK(!awake(w.eng, kWake));
}

void testHysteresis() {
    World world;
    Windowed w;
    testing::Session s(api(), w.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    passes(w.eng, 3);
    // (The log ring is the process's, and the groups before this one woke and put down the same plugin: lines are counted from here.)
    const std::size_t wokeBefore = logged("plugin: Group Fourteen Tracks woke - window open");
    const std::size_t sleptBefore = logged("plugin: Group Fourteen Tracks dormant after 30 s without a use");
    submitThenOnePass(w.eng, s, useCmd(kTracks + "\t1\n"));
    w.ui.poll();
    CHECK(!w.ui.trackPluginNames().empty());

    // The signal goes: the plugin does NOT go with it. FOXSDR_DORMANT_AFTER_MS is 400 in this process (main).
    submitThenOnePass(w.eng, s, useCmd(std::string()));
    const auto cleared = std::chrono::steady_clock::now();
    (void)passUntil(w.eng, [] { return false; }, 150);
    w.ui.poll();
    CHECK(!w.ui.trackPluginNames().empty());   // still up 150 ms after the last use
    CHECK_EQ(runWord(s, kTracksName) & 4, 0);
    CHECK(passUntil(w.eng, [&] { w.ui.poll(); return w.ui.trackPluginNames().empty(); }, 4000));
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cleared).count();
    std::printf("N3: the track source went down %.0f ms after its last use (interval 400)\n", ms);
    CHECK(ms >= 380.0);
    CHECK(w.ui.panels().empty());
    passes(w.eng, 2);
    CHECK_EQ(runWord(s, kTracksName), 4);
    CHECK_EQ(logged("plugin: Group Fourteen Tracks dormant after 30 s without a use"), sleptBefore + 1u);

    // The next signal creates a NEW instance.
    submitThenOnePass(w.eng, s, useCmd(kTracks + "\t1\n"));
    w.ui.poll();
    CHECK(!w.ui.trackPluginNames().empty());
    CHECK_EQ(logged("plugin: Group Fourteen Tracks woke - window open"), wokeBefore + 2u);
}

void testRunStates() {
    World world;
    Windowed w;
    testing::Session s(api(), w.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    passes(w.eng, 3);

    // ALWAYS: runs with no signal at all, and goes on running.
    submitThenOnePass(w.eng, s, runCmd(kWake, 1));
    CHECK(awake(w.eng, kWake));
    CHECK_EQ(runWord(s, kWakeName), 1);
    CHECK_EQ(feed(w.eng, 0.5f).size(), 1u);
    (void)passUntil(w.eng, [] { return false; }, 900);   // twice the interval
    CHECK(awake(w.eng, kWake));
    CHECK_EQ(runWord(s, kWakeName), 1);
    // ...un-pinned it is left running and goes down only after the interval, counted from the un-pinning.
    submitThenOnePass(w.eng, s, runCmd(kWake, 0));
    CHECK(awake(w.eng, kWake));
    (void)passUntil(w.eng, [] { return false; }, 150);
    CHECK(awake(w.eng, kWake));
    CHECK(passUntil(w.eng, [&] { return !awake(w.eng, kWake); }, 4000));
    passes(w.eng, 2);
    CHECK_EQ(runWord(s, kWakeName), 4);

    // STOPPED: never wakes, whatever is shown; PLUGINS says so both ways.
    submitThenOnePass(w.eng, s, runCmd(kWake, 2));
    submitThenOnePass(w.eng, s, useCmd(kWake + "\t8\n"));
    passes(w.eng, 3);
    CHECK(!awake(w.eng, kWake));
    CHECK_EQ(runWord(s, kWakeName), 2);
    CHECK((entryNamed(s, FOXAPI_LIST_PLUGINS, kWakeName).flags & FOXAPI_ITEM_STOPPED) != 0u);
    CHECK(Access::runMode(w.eng, kWake) == PluginRun::Stopped);
    // ...and back to AUTO with the signal still there wakes it: the stop was all that held it.
    submitThenOnePass(w.eng, s, runCmd(kWake, 0));
    passes(w.eng, 2);
    CHECK(awake(w.eng, kWake));
    CHECK_EQ(runWord(s, kWakeName), 0);

    // THE OP'S REFUSALS, and a REMOTE session's.
    CHECK_EQ(pumped(w.eng, s, runCmd(kWake, 3)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(pumped(w.eng, s, runCmd("no-such-plugin.dll", 1)).status(), FOXAPI_NOT_FOUND);
    CHECK_EQ(pumped(w.eng, s, runCmd(std::string(), 1)).status(), FOXAPI_BAD_ARGUMENT);
    testing::Session remote(api(), w.eng.apiHandle(), FOXAPI_SESSION_REMOTE, kAllGrants, "s3cret");
    CHECK(remote.ok());
    CHECK_EQ(pumped(w.eng, remote, runCmd(kWake, 2)).status(), FOXAPI_DENIED);
    CHECK_EQ(pumped(w.eng, remote, useCmd(kWake + "\t8\n")).status(), FOXAPI_DENIED);
    CHECK(Access::runMode(w.eng, kWake) == PluginRun::Auto);
}

void testStandingDuty() {
    World world;
    Windowed w;
    testing::Session s(api(), w.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    passes(w.eng, 3);
    // An in-chain audio processor has no window, map or output that could ever wake it: it is in use for as long as it is fitted, and a
    // dormant one would silently stop processing the audio.
    const auto processing = [&] { return !Access::runner(w.eng).processorTitles().empty(); };
    CHECK(processing());
    (void)passUntil(w.eng, [] { return false; }, 900);   // twice the interval, nothing shown
    CHECK(processing());
    CHECK_EQ(runWord(s, kProcessorName) & 4, 0);
}

void testConfig() {
    World world;
    Windowed w;
    testing::Session s(api(), w.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    passes(w.eng, 3);
    const std::string wakeId = cascade::core::pluginRunId(kWake);
    const std::string tracksId = cascade::core::pluginRunId(kTracks);

    // The OLD stop list (module file names) is migrated, by id; pluginRun is restored; both go back out as pluginRun alone.
    cascade::core::AppConfig cfg;
    cfg.pluginsStopped = {kWake};
    cfg.pluginRun[tracksId] = "always";
    Access::restoreConfig(w.eng, cfg);
    CHECK(Access::runMode(w.eng, kWake) == PluginRun::Stopped);
    CHECK(Access::runMode(w.eng, kTracks) == PluginRun::Always);
    CHECK(!awake(w.eng, kWake));
    w.ui.poll();
    CHECK(!w.ui.trackPluginNames().empty());     // ALWAYS: up with nothing shown
    cascade::core::AppConfig out = Access::readConfig(w.eng);
    CHECK(out.pluginsStopped.empty());
    CHECK_EQ(out.pluginRun.size(), 2u);
    CHECK_EQ(out.pluginRun[wakeId], std::string("stopped"));
    CHECK_EQ(out.pluginRun[tracksId], std::string("always"));
    std::printf("N6: pluginRun written back: %s=%s %s=%s; pluginsStopped %zu entries\n", wakeId.c_str(), out.pluginRun[wakeId].c_str(),
                tracksId.c_str(), out.pluginRun[tracksId].c_str(), out.pluginsStopped.size());

    // An explicit AUTO wins over a stale legacy stop for the same id, and is never written.
    cascade::core::AppConfig cfg2;
    cfg2.pluginsStopped = {kWake};
    cfg2.pluginRun[wakeId] = "auto";
    Access::restoreConfig(w.eng, cfg2);
    CHECK(Access::runMode(w.eng, kWake) == PluginRun::Auto);
    CHECK(Access::runMode(w.eng, kTracks) == PluginRun::Auto);
    out = Access::readConfig(w.eng);
    CHECK(out.pluginRun.empty());
    CHECK(out.pluginsStopped.empty());
}

void testTheWindowGoes() {
    World world;
    Windowed w;
    testing::Session s(api(), w.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    passes(w.eng, 3);
    CHECK(!awake(w.eng, kWake));
    Access::removeWindow(w.eng);   // the interface is gone: nothing could ever report a window again
    passes(w.eng, 3);
    CHECK(awake(w.eng, kWake));
    CHECK_EQ(runWord(s, kWakeName), 0);
    CHECK_EQ(feed(w.eng, 0.5f).size(), 1u);
}

}  // namespace gn

void testPluginRunsOnlyWhileUsed() {
    gn::testAtStart();
    gn::testFirstFrameOfAWake();
    gn::testHysteresis();
    gn::testRunStates();
    gn::testStandingDuty();
    gn::testConfig();
    gn::testTheWindowGoes();
}

// SHELL RETIREMENT R2 (docs/api-0.3-additions.md 9.13, docs/engine-stage4.md 6.25): the two test ops that took the place of the door's bounded-run seams
// seamPatchStart (FOXSDR_PATCH_START) and seamBiasStandIn (FOXSDR_FORCE_BIAS_KEY), through the table as the window sends them.
void testShellRetirementR2Ops() {
    using Access = cascade::gui::AppWindowTestAccess;
    Access::installRadios83();
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(s.ok());
    testing::Session remote(api(), d.eng.apiHandle(), FOXAPI_SESSION_REMOTE,
                            FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS, "s3cret");
    CHECK(remote.ok());
    d.eng.controlPass();

    // R2.1 TEST_PATCH_RUNNING marks the graph running and nothing else: the state's flag moves (the window's FOXSDR_PATCH_RUN_AT reads it), the radio does not.
    const auto patchRunning = [&] { return (s.state().flags & FOXAPI_RX_PATCH_RUNNING) != 0u; };
    const uint32_t openBefore = s.state().flags & (FOXAPI_RX_DEVICE_OPEN | FOXAPI_RX_DEVICE_OPENING);
    CHECK(!patchRunning());
    const testing::Outcome run = pumped(d.eng, s, testing::cmd(FOXAPP_OP_TEST_PATCH_RUNNING));
    CHECK_EQ(run.status(), FOXAPI_OK);
    CHECK(patchRunning());
    CHECK_EQ(s.state().flags & (FOXAPI_RX_DEVICE_OPEN | FOXAPI_RX_DEVICE_OPENING), openBefore);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPP_OP_TEST_PATCH_RUNNING)).status(), FOXAPI_OK);   // again: the same
    CHECK(patchRunning());
    // ...and it is the application's own: a REMOTE session is refused it (every FOXAPP_OP_* is), and nothing moves.
    CHECK_EQ(pumped(d.eng, remote, testing::cmd(FOXAPP_OP_TEST_PATCH_RUNNING)).status(), FOXAPI_DENIED);

    // ...AND IT SWITCHES NO RADIO ON, which is what separates it from FOXAPI_OP_PATCH_RUN (patchPressStart: "START with every radio switched off switches
    // them all on", core/patch_graph.hpp switchOnForStart). A graph with its one Radio part switched off: the op marks the graph running and the Radio part
    // is still off, the graph's version has not moved. (The 36 real-binary tests that set FOXSDR_PATCH_START run a graph with no Radio part, where the
    // two spellings cannot be told apart; this is the check that can.)
    {
        using namespace g13;
        Direct d2;
        testing::Session a(api(), d2.eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
        CHECK(a.ok());
        passes(d2.eng, 2);
        const Starter p = starter();
        CHECK_EQ(pumpedText(d2.eng, a, setGraph(FOXAPP_PATCH_GRAPH_DOCUMENT, 0), pc::graphCommandText(p.g)).result.status, FOXAPI_OK);
        passes(d2.eng, 1);
        pc::Graph off = graphOf(a);
        if (pc::Node* n = off.mutableNode(p.radio)) { n->on = false; }
        CHECK_EQ(pumpedText(d2.eng, a, setGraph(0, static_cast<long long>(versionOf(a))), pc::graphCommandText(off)).result.status, FOXAPI_OK);
        passes(d2.eng, 1);
        CHECK(!onOf(a, p.radio));
        const uint64_t versionBefore = versionOf(a);
        CHECK((a.state().flags & FOXAPI_RX_PATCH_RUNNING) == 0u);
        CHECK_EQ(pumped(d2.eng, a, testing::cmd(FOXAPP_OP_TEST_PATCH_RUNNING)).status(), FOXAPI_OK);
        passes(d2.eng, 2);
        CHECK((a.state().flags & FOXAPI_RX_PATCH_RUNNING) != 0u);   // running ...
        CHECK(!onOf(a, p.radio));                                    // ... and the radio is still off
        CHECK_EQ(versionOf(a), versionBefore);                       // nothing in the graph moved
    }

    // R2.2 TEST_BIAS_STAND_IN: 1 accept puts the stand-in tee in front of the key, 2 refuse the one that refuses, 0 takes it away; the same stand-in
    // the "test.biasStandIn" setting makes, by the same facts.
    CHECK_EQ(fact(s, "fact.biasTee.reachable"), std::string("0"));
    CHECK_EQ(pumped(d.eng, remote, testing::cmd(FOXAPP_OP_TEST_BIAS_STAND_IN, 0.0, 1)).status(), FOXAPI_DENIED);
    d.eng.controlPass();
    CHECK_EQ(fact(s, "fact.biasTee.reachable"), std::string("0"));
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPP_OP_TEST_BIAS_STAND_IN, 0.0, 1)).status(), FOXAPI_OK);
    d.eng.controlPass();
    CHECK_EQ(fact(s, "fact.biasTee.reachable"), std::string("1"));
    CHECK_EQ(fact(s, "fact.biasTee.radioName"), std::string("stand-in radio (FOXSDR_FORCE_BIAS_KEY)"));
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SET_BIAS_TEE, 0.0, 1)).status(), FOXAPI_OK);
    CHECK((s.state().flags & FOXAPI_RX_BIAS_TEE_ON) != 0u);                                     // the accepting stand-in switched
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SET_BIAS_TEE, 0.0, 0)).status(), FOXAPI_OK);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPP_OP_TEST_BIAS_STAND_IN, 0.0, 2)).status(), FOXAPI_OK);
    d.eng.controlPass();
    CHECK_EQ(fact(s, "fact.biasTee.reachable"), std::string("1"));
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPI_OP_SET_BIAS_TEE, 0.0, 1)).status(), FOXAPI_OK);
    CHECK((s.state().flags & FOXAPI_RX_BIAS_TEE_ON) == 0u);                                     // the refusing one did not
    CHECK(fact(s, "fact.status.sourceError").find("stand-in bias tee") != std::string::npos);
    // A number that names none of the three is refused, and the stand-in stays as it was.
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPP_OP_TEST_BIAS_STAND_IN, 0.0, 3)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPP_OP_TEST_BIAS_STAND_IN, 0.0, -1)).status(), FOXAPI_OUT_OF_RANGE);
    d.eng.controlPass();
    CHECK_EQ(fact(s, "fact.biasTee.reachable"), std::string("1"));
    CHECK_EQ(pumped(d.eng, s, testing::cmd(FOXAPP_OP_TEST_BIAS_STAND_IN, 0.0, 0)).status(), FOXAPI_OK);
    d.eng.controlPass();
    CHECK_EQ(fact(s, "fact.biasTee.reachable"), std::string("0"));
}

int main() {
    engine_api_test::hermetic("api03");
    // The plugins' hysteresis, shortened for group N (the engine reads it at its first lifecycle step; no other group has a window).
#if defined(_WIN32)
    _putenv_s("FOXSDR_DORMANT_AFTER_MS", "400");
#else
    setenv("FOXSDR_DORMANT_AFTER_MS", "400", 1);
#endif
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
    testBaselineWaitsForTheSink();
    testStatusColumnGains();
    testStatusColumnRecordings();
    testListsBeforeFirstPublish();
    testNotePanel();
    // Group Z: the Airspy and HackRF drivers on fake wires beside the mock radio.
    testDeviceOpening();
    testRecordStartFact();
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
    testPresetExport();
    testImportOnAWorker();
    testAddRowsFlags();
    testRecordingsListOnAWorker();
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
// <<G11 MAIN BEGIN
    // Group 11: the Source section and the sound card.
    testSourceRowsAndWholeIds();
    testSourceFlags();
    testSourceFacts();
    testDeviceFaultWhole();
    testSampleRateCaptions();
    testUnboundRadiosAndSdrPlayNote();
    testScanEvidence();
    testSoundCardPanel();
    testPerFrameMovesAreThePasss();
    testSourceListSeqExactness();
// G11 MAIN END>>
    // Group 12: the radar scope and the demod scope.
    testPositionState();
    testScopeCapacity();
    // Group 13: the Patch page, part 1.
    testPatchRuntimeInThePass();
    testPatchGraphVersionAndMerge();
    testPatchPlanList();
    testPatchSettledMovesNothing();
    // Group 13b: the Patch page, part 2.
    testPatchPartsAndDevices();
    testPatchReceiverRadioAndConverter();
    testPatchLive();
    testPatchDecoderFaceAndLines();
    // Group 13c: the Patch page, part 3 (the decoded pictures).
    testPatchPictures();
    testPatchPicturesDocument();
    testPatchPicturesNeverTear();
    // Group 14b: the GPS reader.
    testGpsList();
    // Group 14c: the usage switch, the session counters, the diagnostics context.
    testTelemetryFacts();
    testDiagContextIsTheEngines();
    testDiagContextAtApplyConfig();
    // Group N: a fitted plugin runs only while it is used (stub campaign B2).
    testPluginRunsOnlyWhileUsed();
    // Shell retirement R2: the two test ops.
    testShellRetirementR2Ops();
    return check::finish("test_engine_api_03");
}
