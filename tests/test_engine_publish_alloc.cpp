// test_engine_publish_alloc.cpp - the engine's per-pass publish takes no heap
// allocation (engine stage 4, review F8).
//
// Engine::fillPublishedState runs once a control pass, and the tree's rule is
// that a pass allocates nothing. The transmit page's error line (API 0.3 state
// 8.1, FoxReceiverState::txError) used to be filled through
// Transmitter::lastError(), which returns a std::string by value: a copy, and a
// heap allocation whenever the text is longer than the small-string buffer
// (every real error sentence is); the audio sink's name was read the same way
// (AudioOut::openedDeviceName()). Held here by counting the calling thread's
// global operator new around the publish:
//
//   A  the publish allocates exactly as often with a 300-character error as
//      with none (the text adds nothing) and not at all; what it publishes is
//      the error, cut to the field on a character boundary, as it always was,
//      and the audio sink's name
//   A2 AudioOut::refreshOpenedDeviceName: the last good name stands when the
//      sink has none to offer
//   B  Transmitter::lastErrorInto: the whole text when it fits, a cut that
//      never splits a UTF-8 character when it does not, a NUL always, and no
//      write at all with no room
//   C  the status column's texts (API 0.3 8.2): with decoders muting the audio
//      and a plugin holding the speakers, the publish allocates nothing
//   D  the receiver rail (API 0.3 8.4): with RDS text present (a service name
//      and a 64-character RadioText, longer than any small string), the
//      auto-notch engaged and the pilot locked, the publish allocates nothing
//      and says all of it
//   E  the scanner (API 0.3 8.6): running, with its range and timings set, the
//      publish allocates nothing and says state, frequency, range and timings;
//      stepping across the range, paused on a signal and stopped again, the
//      same
//   F  the plugin-mute cluster (API 0.3 8.7): with two plugins muting the audio
//      and the mute held after a tune-away, the lists' fingerprint (computed
//      every pass) allocates nothing, a pass whose lists did not change builds
//      no block, and the fingerprint moves with each of the three things the
//      cluster adds to it (a name, a module key, the latch); and with two real
//      plugins loaded (the PLUGINS flags, the grants, the mute setting hashed
//      for each every pass) the same holds
//   F3 the fitted modules (API 0.3 8.8): with three real modules and a refused file loaded, a notice from
//      one, a request for the settings grant from another, command keys, an install record that says a file
//      is not the one installed - every pass's fingerprint, an unchanged pass and the publish allocate
//      nothing; each of those moves the fingerprint, a plugin that repeats a warning does not, and the
//      block's rebuild is counted
//   F4 the plugin store (API 0.3 8.9): with a catalogue of 24 entries (descriptions of 528 to 3 KB), a retired row,
//      a transfer in flight and an ADD ALL run, every pass's fingerprint, an unchanged pass and the publish allocate
//      nothing, and so do 1000 reads of STORE_ACTIVITY while the transfer's progress moves (it is read live, and
//      moves nothing); each input the store adds moves the fingerprint ALONE (the catalogue's counter, the index,
//      the busy name with the flag held, the run's five fields, a retired row's id and sentence, the two status
//      lines) and the block's rebuild is counted, once for the change
//   F5 the Source section and the sound card (API 0.3 8.10): with six sound card inputs of eight rates, a form, three
//      dongles that cannot be opened, two faulted drivers, the runtime's evidence, a 189-byte Pluto address and a
//      134-byte fault sentence, every pass's fingerprint, an unchanged pass and the publish (with the section's flag
//      word) allocate nothing, and so do 1000 rounds of reading each new list and fact; each input moves the
//      fingerprint ALONE and the block's rebuild is counted, once for the change
//   F6 the radar scope and the demod scope (API 0.3 8.11): the receiver's position in the state and the scope tap's
//      capacity add nothing to the pass: with a position set the publish allocates nothing, says the position (and
//      says none, 0 and 0, when it is cleared), moves no list fingerprint, and 1000 rounds of read_state and of the
//      three taps' info-only read_scope (LOCAL) allocate nothing
//   F7 the Patch page, part 1 (API 0.3 8.12): with a graph of eight nodes, a plan of two channels and two decoders, a parts
//      bin of three modules and a graph that the engine itself changes, every pass's fingerprint, an unchanged pass and the
//      publish (noticing a change to the graph is one comparison with the newest retained version) allocate nothing, and
//      so do 1000 rounds of reading the version, the epoch, the runnable fact and the plan; each input the page adds moves
//      the fingerprint ALONE (the version, runnable, every figure of every channel and decoder, every field of every
//      parts-bin entry) and the rebuild is counted, once for the change; a change to the graph is noticed as a version by
//      the publish and by nothing else, and said again moves nothing. THE PATCH STEP ITSELF ALLOCATES BY DESIGN (it
//      rebuilds the catalogue and compiles the plan once a pass): the claim here is the PUBLISH side only.
//   F9 the decoded pictures (API 0.3 8.12, part 3): a picture the runner hands over is moved in, never copied (a 1.4 MB
//      picture costs the publish under 8 KB and the take no more than its own pixels, and the allocation count is a bounded
//      handful for a small picture and a large one alike), a settled pass with pictures held allocates nothing, so do 1000
//      rounds of reading the picture row, its info and its pixels, a replaced picture costs the same again, a node's removal
//      takes its picture with one bounded rebuild, and none of it moves the list block's fingerprint.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <vector>
#include <future>
#include <cstdio>
#include <fstream>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>

#include <filesystem>
#include <string>

#include "core/plugin_repo.hpp"
#include "core/transmitter.hpp"
#include "engine/engine.hpp"
#include "engine_api_test_env.hpp"
#include "sink/audio_out.hpp"
#include "test_check.hpp"

// --- the counter: the calling thread's allocations, while armed -----------------
namespace {
thread_local bool t_counting = false;
thread_local long t_news = 0;
thread_local long long t_bytes = 0;   // ...and the bytes they asked for (F9: a picture's pixels are never copied)
}  // namespace

void* operator new(std::size_t n) {
    if (t_counting) {
        ++t_news;
        t_bytes += static_cast<long long>(n);
    }
    void* p = std::malloc(n != 0 ? n : 1);
    if (p == nullptr) { throw std::bad_alloc(); }
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {

// A transmitter that is only a name and an error: every frequency change is
// refused, with this text.
class RefusingSink : public cascade::source::IqSink {
public:
    bool start() override { return false; }
    void stop() override {}
    void finish() override {}
    bool running() const override { return false; }
    double sampleRateHz() const override { return 2.5e6; }
    bool setSampleRateHz(double) override { return true; }
    double centerFrequencyHz() const override { return 146.52e6; }
    bool setCenterFrequencyHz(double) override { return false; }
    bool frequencyRangeHz(double& lo, double& hi) const override { lo = 70.0e6; hi = 6.0e9; return true; }
    bool sampleRateRangeHz(double& lo, double& hi) const override { lo = 2.083e6; hi = 61.44e6; return true; }
    bool gainRangeDb(double& lo, double& hi) const override { lo = -89.75; hi = 0.0; return true; }
    double gainDb() const override { return 0.0; }
    bool setGainDb(double) override { return true; }
    std::size_t write(const std::complex<float>*, std::size_t) override { return 0; }
    bool faulted() const override { return false; }
    const char* name() const override { return "refusing test radio"; }
    const char* lastError() const override { return error.c_str(); }

    std::string error;
};

}  // namespace

namespace cascade::gui {
struct AppWindowTestAccess {
    static RefusingSink* installSink(cascade::engine::Engine& e) {
        auto sink = std::make_unique<RefusingSink>();
        RefusingSink* raw = sink.get();
        e.transmitter_.setSink(std::move(sink));
        return raw;
    }
    // The radio refuses a retune with the sink's current text: that text is
    // now the Transmitter's last error.
    static void provokeError(cascade::engine::Engine& e) { (void)e.transmitter_.setFrequencyHz(100.0e6); }
    static std::string sinkName(cascade::engine::Engine& e) { return e.pipeline_.audio().openedDeviceName(); }
    static void refreshSink(cascade::engine::Engine& e, std::string& lastGood) {
        e.pipeline_.audio().refreshOpenedDeviceName(lastGood);
    }
    static void errorInto(cascade::engine::Engine& e, char* out, std::size_t cap) {
        e.transmitter_.lastErrorInto(out, cap);
    }
    // The status column's mute subject and playing plugin (API 0.3 8.2).
    static void muteBy(cascade::engine::Engine& e, std::vector<std::string> names) { e.mutedBy_ = std::move(names); }
    static void playing(cascade::engine::Engine& e, const std::string& name) {
        e.pluginRunner_.setPlayingForTest(name, 3, 400);
    }
    static void publish(cascade::engine::Engine& e, cascade::core::PublishedState& ps,
                        const cascade::core::RdsSnapshot* heard = nullptr) {
        const std::string fault;
        const cascade::core::RdsSnapshot none;
        e.fillPublishedState(ps, fault, heard != nullptr ? *heard : none);
    }
    // The auto-notch's readout and the pilot lock, as the hooks stand them in
    // (test.autoNotch, test.pilot; API 0.3 8.4).
    static void autoNotch(cascade::engine::Engine& e, bool on, bool engaged, double hz) {
        e.testAutoNotchOn_ = on;
        e.testAutoNotchEngaged_ = engaged;
        e.testAutoNotchHz_ = hz;
    }
    static void pilot(cascade::engine::Engine& e, bool on, bool locked) {
        e.testPilotOn_ = on;
        e.testPilotLocked_ = locked;
    }
    // The scanner (API 0.3 8.6), driven as the engine's own commands and pass do: the timings and
    // the range set, a scan started or stopped, and one tick of its frame (the first tune, a
    // step, a pause) with the squelch at a threshold below (open) or above (closed) the -200 dB
    // a stopped pipeline reads.
    static void scanConfig(cascade::engine::Engine& e, double dwell, double hold, double resume, double listen) {
        FoxCommand c{};
        c.structSize = sizeof(c);
        c.op = FOXAPI_OP_SCANNER_CONFIG;
        c.num[0] = dwell;
        c.num[1] = hold;
        c.num[2] = resume;
        c.num[3] = listen;
        (void)e.applyCommand(c);
    }
    static void scanRun(cascade::engine::Engine& e, bool on, double startHz, double stopHz, double stepHz) {
        FoxCommand c{};
        c.structSize = sizeof(c);
        c.op = FOXAPI_OP_SCANNER_RUN;
        c.ival[0] = on ? 1 : 0;
        c.num[0] = startHz;
        c.num[1] = stopHz;
        c.num[2] = stepHz;
        (void)e.applyCommand(c);
    }
    static void scanTick(cascade::engine::Engine& e, float squelchDb) {
        e.squelchDb_ = squelchDb;
        e.scannerFrame();
    }
    // The plugin-mute cluster (API 0.3 8.7), as updateAudioMute leaves it: the plugins muting (their
    // names and their module file names, in step) and whether the mute is held after a tune-away.
    static void muteCluster(cascade::engine::Engine& e, std::vector<std::string> names, std::vector<std::string> keys,
                            bool kept) {
        e.mutedBy_ = std::move(names);
        e.mutedByKeys_ = std::move(keys);
        e.muteKeptRunning_ = kept;
    }
    static std::uint64_t fingerprint(cascade::engine::Engine& e) { return e.apiListsFingerprint(); }
    // The folder the engine looks for plugins in (null: the default), and a command applied at once.
    static void setPluginDir(std::string (*dir)()) { cascade::engine::Engine::testHooks_.pluginDir = dir; }
    static void apply(cascade::engine::Engine& e, const FoxCommand& c) { (void)e.applyCommand(c); }
    static std::uint64_t publishLists(cascade::engine::Engine& e) { return e.publishApiLists(); }
    // The fitted-modules window's inputs (API 0.3 8.8): the plugin API's host side (what a module's own calls
    // reach through the bridge), the install records, and the drain of what the modules said.
    static cascade::core::PluginApiCore& core(cascade::engine::Engine& e) { return e.pluginUi_.api(); }
    static cascade::core::PluginInventory& inventory(cascade::engine::Engine& e) { return e.pluginInventory_; }
    static void applyPluginApi(cascade::engine::Engine& e) { e.applyPluginApi(); }
    // The runner's matched keys, as the pass's publish leaves them, and a rebuild of the runner and the plugin
    // UI over the same modules (what a source change does).
    static std::vector<std::string>& feedingKeys(cascade::engine::Engine& e) { return e.feedingKeysScratch_; }
    static void refreshRunner(cascade::engine::Engine& e) { e.refreshPluginRunner(); }
    // A rescan, as the fingerprint counts it (the files' sizes and everything else a rescan changes are hashed
    // through this one value; a real rescan also rewrites the runner's rows, so only this isolates it).
    static void countScan(cascade::engine::Engine& e) { ++e.pluginScans_; }
    // The plugin store's inputs (API 0.3 8.9), each reachable alone. A catalogue as a fetch's worker leaves it, landed
    // by the engine's own poll (which also rescans the plugins, as a real landing does) ...
    static void landCatalogue(cascade::engine::Engine& e, std::vector<cascade::core::PluginCatalogEntry> entries) {
        std::promise<cascade::engine::Engine::CatalogFetchResult> p;
        cascade::engine::Engine::CatalogFetchResult r;
        r.ok = true;
        r.entries = std::move(entries);
        p.set_value(std::move(r));
        e.catalogFuture_ = p.get_future();
        e.catalogPending_ = true;
        e.pollPluginAsync();
    }
    // ... the catalogue replaced in place with only its counter bumped (no rescan, no pending flag: nothing else
    // that a landing moves), ...
    static void replaceCatalogue(cascade::engine::Engine& e, std::vector<cascade::core::PluginCatalogEntry> entries) {
        e.catalog_ = std::move(entries);
        ++e.catalogSeq_;
    }
    // ... a download in flight: the install slot holds a promise this test keeps, ...
    static void beginTransfer(cascade::engine::Engine& e, const std::string& name) {
        static std::promise<cascade::engine::Engine::PluginInstallResult> keep;
        keep = std::promise<cascade::engine::Engine::PluginInstallResult>();
        e.installFuture_ = keep.get_future();
        e.installBusyName_ = name;
        e.installPending_ = true;
    }
    static std::string& busyName(cascade::engine::Engine& e) { return e.installBusyName_; }
    static void setProgress(cascade::engine::Engine& e, float p) { e.pluginRepo_.setProgressForTest(p); }
    // ... the ADD ALL run, its summary and the catalogue's status line, ...
    static auto& addAllRun(cascade::engine::Engine& e) { return e.addAllRun_; }
    static bool& addAllFailed(cascade::engine::Engine& e) { return e.addAllFailed_; }
    static std::string& addAllSummary(cascade::engine::Engine& e) { return e.addAllSummary_; }
    static std::string& catalogStatus(cascade::engine::Engine& e) { return e.catalogStatus_; }
    // ... and the rows the policy retires.
    static std::vector<cascade::core::BlockedPlugin>& blocked(cascade::engine::Engine& e) { return e.pluginBlocked_; }
// <<F5 ACCESS BEGIN
    // The Source section's inputs (API 0.3 8.10), each reachable alone.
    static std::vector<cascade::usb::UsbDeviceInfo>& unbound(cascade::engine::Engine& e) { return e.nativeUnbound_; }
    static std::vector<std::string>& faulted(cascade::engine::Engine& e) { return e.faultedDrivers_; }
    static auto& evidence(cascade::engine::Engine& e) { return e.scanEvidence_; }
    static std::uint64_t& evidenceSeq(cascade::engine::Engine& e) { return e.scanEvidenceSeq_; }
    static char* plutoBuffer(cascade::engine::Engine& e) { return e.plutoUri_; }
    static std::vector<cascade::source::SoundCardDevice>& cards(cascade::engine::Engine& e) { return e.soundCardDevices_; }
    static cascade::source::SoundCardSettings& form(cascade::engine::Engine& e) { return e.soundCard_; }
    static bool& cardListed(cascade::engine::Engine& e) { return e.soundCardListed_; }
    static std::vector<std::string>& absent(cascade::engine::Engine& e) { return e.soapyAbsentDrivers_; }
    static std::string& keepKind(cascade::engine::Engine& e) { return e.restoreKeep_.kind; }
    static std::string& busyLabel(cascade::engine::Engine& e) { return e.deviceBusyLabel_; }
    static std::string& faultText(cascade::engine::Engine& e) { return e.apiFaultText_; }
    static int& sourceSel(cascade::engine::Engine& e) { return e.sourceSel_; }
    static bool& sdrRows(cascade::engine::Engine& e) { return e.sdrPlayRowsFound_; }
    static bool& networkUsrps(cascade::engine::Engine& e) { return e.lookForNetworkUsrps_; }
    static bool& scanned(cascade::engine::Engine& e) { return e.soapyScanned_; }
// F5 ACCESS END>>
// <<F7 ACCESS BEGIN
    // The Patch page's inputs (API 0.3 8.12), each reachable alone.
    static cascade::core::patch::Graph& patchGraph(cascade::engine::Engine& e) { return e.patchGraph_; }
    static cascade::core::patch::Plan& patchPlan(cascade::engine::Engine& e) { return e.patchPlan_; }
    static std::vector<cascade::core::patch::DecoderInfo>& patchCatalogue(cascade::engine::Engine& e) {
        return e.patchCatalogue_;
    }
    static std::uint64_t& patchVersion(cascade::engine::Engine& e) { return e.patchGraphVersion_; }
    static std::uint64_t& patchEpoch(cascade::engine::Engine& e) { return e.patchGraphEpoch_; }
// F7 ACCESS END>>
// <<F8 ACCESS BEGIN
    // The Patch page, part 2 (API 0.3 8.12): the readings PATCH_LIVE copies and the inputs of the device list.
    static cascade::core::patch::DestTable& patchDests(cascade::engine::Engine& e) { return e.patchDests_; }
    static std::map<cascade::core::patch::NodeId, std::string>& patchDestError(cascade::engine::Engine& e) {
        return e.patchDestError_;
    }
    static void setFace(cascade::engine::Engine& e, cascade::core::patch::NodeId node, std::uint64_t lines, const std::string& last) {
        cascade::engine::Engine::PatchDecoderFace& f = e.patchDecoderFaces_[node];
        f.lines = lines;
        f.last = last;
    }
    static cascade::engine::Engine::PatchMainKeep& patchMainKeep(cascade::engine::Engine& e) { return e.patchMainKeep_; }
    static std::vector<cascade::core::patch::RecordingInfo>& patchRecordings(cascade::engine::Engine& e) {
        return e.patchRecordings_;
    }
// F8 ACCESS END>>
// <<F9 ACCESS BEGIN
    // The Patch page, part 3 (API 0.3 8.12): a picture as the decoder's runner hands it to the engine's pass.
    static void takeImage(cascade::engine::Engine& e, cascade::core::patch::NodeId node, std::uint32_t w, std::uint32_t h,
                          std::uint32_t format, std::uint8_t fill) {
        cascade::core::HostImage img;
        img.plugin = "alloc fixture";
        img.width = w;
        img.height = h;
        img.format = format;
        img.complete = false;
        img.pixels.assign(static_cast<std::size_t>(w) * h * (format == CASCADE_IMAGE_RGB24 ? 3u : 1u), fill);
        e.takePatchImage(node, std::move(img));
    }
// F9 ACCESS END>>
    // The radar scope's position (API 0.3 8.11): set or cleared, as SET_POSITION and the configuration leave it.
    static void position(cascade::engine::Engine& e, bool set, double lat, double lon) {
        e.rxSet_ = set;
        e.rxLat_ = lat;
        e.rxLon_ = lon;
    }
};
}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

namespace {

// The scratch folder of the plugin case: the two fixture modules of tests/fixture_stage1_plugin.cpp and
// tests/fixture_group8_plugin.cpp.
std::string g_pluginDir;
std::string pluginDirHook() { return g_pluginDir; }

constexpr int kPublishes = 40;

// The heap allocations `kPublishes` publishes make on this thread (the RDS
// decoder's snapshot, when one is given, is the one the pass was handed: its
// own copy is made by publishReceiverState, once, and is not this function's).
long allocationsOfPublishes(cascade::engine::Engine& e, const cascade::core::RdsSnapshot* heard = nullptr) {
    cascade::core::PublishedState ps;
    Access::publish(e, ps, heard);  // warm-up: scratch the first call sizes
    t_news = 0;
    t_counting = true;
    for (int i = 0; i < kPublishes; ++i) { Access::publish(e, ps, heard); }
    t_counting = false;
    return t_news;
}

}  // namespace

int main() {
    engine_api_test::hermetic("publishalloc");
    cascade::engine::Engine e;
    e.initialise();
    RefusingSink* sink = Access::installSink(e);

    // A: the error text adds no allocation to the publish.
    sink->error.clear();
    Access::provokeError(e);
    const long quiet = allocationsOfPublishes(e);

    const std::string text = std::string("the transmitter refused the new frequency: ") + std::string(260, 'x');
    sink->error = text;
    Access::provokeError(e);
    const long loud = allocationsOfPublishes(e);
    std::printf("A: %d publishes, heap allocations with no error text: %ld, with a %zu-character one: %ld\n",
                kPublishes, quiet, text.size(), loud);
    CHECK(loud == quiet);
    // ...and the publish of a settled engine allocates nothing at all: not the
    // audio sink's name (AudioOut::openedDeviceName() is a std::string by
    // value) nor anything else the pass reads.
    CHECK(quiet == 0);
    CHECK(loud == 0);

    // ...and what it publishes is the text, cut to the field (the buffer less
    // its NUL), as before.
    cascade::core::PublishedState ps;
    Access::publish(e, ps);
    const std::size_t cap = sizeof(ps.rx.txError);
    CHECK(std::strlen(ps.rx.txError) == cap - 1);
    CHECK(std::string(ps.rx.txError) == text.substr(0, cap - 1));
    // The sink name published is the sink's own (the hermetic engine opens the
    // default output; on a machine with none both are empty).
    CHECK(std::string(ps.rx.sinkName) == Access::sinkName(e).substr(0, sizeof(ps.rx.sinkName) - 1));

    // AudioOut::refreshOpenedDeviceName keeps keepLastGoodName's rule: a sink
    // that was never opened has no name to offer, so the last good one stands;
    // an opened one's name replaces it.
    {
        cascade::sink::AudioOut fresh;
        std::string last = "Speakers";
        fresh.refreshOpenedDeviceName(last);
        CHECK(last == "Speakers");
        const std::string opened = Access::sinkName(e);
        std::string old = "previous sink";
        Access::refreshSink(e, old);
        CHECK(old == (opened.empty() ? std::string("previous sink") : opened));
    }

    // B: lastErrorInto on its own.
    const auto setError = [&](const std::string& s) {
        sink->error = s;
        Access::provokeError(e);
    };
    char buf[32];
    const auto fillBuf = [&] { std::memset(buf, 'Z', sizeof(buf)); };
    setError("boom");
    fillBuf();
    Access::errorInto(e, buf, sizeof(buf));
    CHECK(std::string(buf) == "boom");  // fits: the whole text
    fillBuf();
    Access::errorInto(e, buf, 3);
    CHECK(std::string(buf) == "bo");    // does not fit: cut, terminated...
    CHECK(buf[3] == 'Z');               // ...and nothing written past the room given
    fillBuf();
    Access::errorInto(e, buf, 0);
    CHECK(buf[0] == 'Z');               // no room: no write
    Access::errorInto(e, nullptr, 8);   // no buffer: no crash
    setError("ab\xC3\xA9");             // "ab" and a two-byte e-acute
    fillBuf();
    Access::errorInto(e, buf, 4);       // room for three bytes: the acute would be split
    CHECK(std::string(buf) == "ab");    // so it is dropped whole
    fillBuf();
    Access::errorInto(e, buf, 5);
    CHECK(std::string(buf) == "ab\xC3\xA9");
    setError("");
    fillBuf();
    Access::errorInto(e, buf, sizeof(buf));
    CHECK(buf[0] == '\0');              // no error: an empty line

    // C: the status column's two texts (API 0.3 8.2) are copied into the state,
    // not built as strings every pass: with decoders muting the audio and a
    // plugin holding the speakers (both longer than a small string), the
    // publish still allocates nothing, and says who.
    {
        const std::string longName = "A decoder plugin whose name is longer than any small-string buffer";
        Access::muteBy(e, {longName, "AIS", "ACARS"});
        Access::playing(e, longName);
        const long busy = allocationsOfPublishes(e);
        std::printf("C: %d publishes with a mute subject and a playing plugin: %ld heap allocations\n", kPublishes, busy);
        CHECK(busy == 0);
        cascade::core::PublishedState ps2;
        Access::publish(e, ps2);
        CHECK(std::string(ps2.rx.audioMutedBy) == longName + ", AIS and ACARS");
        CHECK(std::string(ps2.rx.audioPlayingPlugin) == longName);
        CHECK((ps2.rx.audioFlags & FOXAPI_AUDIO_PLUGIN_PLAYING) != 0u);
        CHECK(ps2.rx.audioPluginGaps == 3u && ps2.rx.audioPluginGapFrames == 400u);
        // ...and handed back, both go quiet.
        Access::muteBy(e, {});
        Access::playing(e, std::string());
        Access::publish(e, ps2);
        CHECK(std::string(ps2.rx.audioMutedBy).empty());
        CHECK(std::string(ps2.rx.audioPlayingPlugin).empty());
        CHECK((ps2.rx.audioFlags & FOXAPI_AUDIO_PLUGIN_PLAYING) == 0u);
    }

    // D: the receiver rail's members (API 0.3 8.4) are copied into fixed fields,
    // not built as strings every pass: RDS text longer than any small string, the
    // auto-notch engaged and the pilot locked, and the publish still allocates
    // nothing - and says all of it.
    {
        cascade::core::RdsSnapshot heard;
        heard.synced = true;
        heard.state.piValid = true;
        heard.state.pi = 0x2C41;
        heard.state.psValid = true;
        heard.state.ps = "CASCADE!";
        heard.state.radioText = std::string(64, 'r');
        heard.state.tp = true;
        heard.state.ta = true;
        heard.state.pty = 10;
        heard.state.groupsDecoded = 4321;
        heard.state.blockErrors = 12;
        Access::autoNotch(e, true, true, 1000.0);
        Access::pilot(e, true, true);
        const long rail = allocationsOfPublishes(e, &heard);
        std::printf("D: %d publishes with RDS text, an engaged auto-notch and a locked pilot: %ld heap allocations\n",
                    kPublishes, rail);
        CHECK(rail == 0);
        cascade::core::PublishedState ps3;
        Access::publish(e, ps3, &heard);
        CHECK(ps3.rx.rdsFlags == (FOXAPI_RDS_SYNCED | FOXAPI_RDS_PI_VALID | FOXAPI_RDS_PS_VALID | FOXAPI_RDS_TP |
                                  FOXAPI_RDS_TA | FOXAPI_RDS_RT_VALID));
        CHECK(ps3.rx.rdsPi == 0x2C41u && ps3.rx.rdsPty == 10u);
        CHECK(std::string(ps3.rx.rdsPs) == "CASCADE!");
        CHECK(std::string(ps3.rx.rdsRadioText) == std::string(64, 'r'));
        CHECK(ps3.rx.rdsGroups == 4321u && ps3.rx.rdsErrors == 12u);
        CHECK((ps3.rx.flags & FOXAPI_RX_AUTO_NOTCH_ENGAGED) != 0u && ps3.rx.autoNotchHz == 1000.0);
        CHECK((ps3.rx.flags & FOXAPI_RX_PILOT_LOCKED) != 0u);
        // ...and the web page's extension carries the same readings (one read, two halves).
        CHECK(ps3.app.autoNotchEngaged && ps3.app.autoNotchFreqHz == 1000.0 && ps3.app.pilotLocked);
        // A text longer than the field is cut at it, never past it.
        heard.state.radioText = std::string(90, 'q');
        Access::publish(e, ps3, &heard);
        CHECK(std::strlen(ps3.rx.rdsRadioText) == 64u);
        // Handed back: quiet again.
        Access::autoNotch(e, false, false, 0.0);
        Access::pilot(e, false, false);
        Access::publish(e, ps3);
        CHECK(ps3.rx.rdsFlags == 0u && ps3.rx.rdsPs[0] == '\0' && ps3.rx.rdsRadioText[0] == '\0');
        CHECK((ps3.rx.flags & (FOXAPI_RX_AUTO_NOTCH_ENGAGED | FOXAPI_RX_PILOT_LOCKED)) == 0u);
        CHECK(ps3.rx.autoNotchHz == 0.0);
    }

    // E: the scanner's members (API 0.3 8.6) are plain numbers copied into the state, so a scan
    // running, stepping, listening and stopping publishes without allocating - and says what it is
    // doing.
    {
        Access::scanConfig(e, 0, 700, 80, 9000);
        Access::scanRun(e, true, 144.0e6, 144.1e6, 12.5e3);
        Access::scanTick(e, 20.0f);  // the first tune, with the squelch closed
        const long scanning = allocationsOfPublishes(e);
        std::printf("E: %d publishes with a scan running: %ld heap allocations\n", kPublishes, scanning);
        CHECK(scanning == 0);
        cascade::core::PublishedState ps4;
        Access::publish(e, ps4);
        CHECK(ps4.rx.scannerState == FOXAPI_SCANNER_SCANNING);
        CHECK((ps4.rx.flags & FOXAPI_RX_SCANNER_ACTIVE) != 0u);
        CHECK(ps4.rx.scanCurrentHz == 144.0e6);
        CHECK(ps4.rx.scanStartHz == 144.0e6 && ps4.rx.scanStopHz == 144.1e6 && ps4.rx.scanStepHz == 12.5e3);
        CHECK(ps4.rx.scanDwellMs == 0u && ps4.rx.scanHoldMs == 700u && ps4.rx.scanResumeMs == 80u &&
              ps4.rx.scanListenMs == 9000u);
        // The same through a step, a pause on a signal and a hold, each published and counted.
        Access::scanTick(e, 20.0f);  // a step
        CHECK(allocationsOfPublishes(e) == 0);
        Access::publish(e, ps4);
        CHECK(ps4.rx.scanCurrentHz == 144.0125e6);
        Access::scanTick(e, -1000.0f);  // a signal: paused
        CHECK(allocationsOfPublishes(e) == 0);
        Access::publish(e, ps4);
        CHECK(ps4.rx.scannerState == FOXAPI_SCANNER_PAUSED);
        Access::scanTick(e, 20.0f);  // gone quiet: holding
        CHECK(allocationsOfPublishes(e) == 0);
        Access::publish(e, ps4);
        CHECK(ps4.rx.scannerState == FOXAPI_SCANNER_HOLDING);
        // Stopped: idle, the frequency where it was left.
        Access::scanRun(e, false, 0.0, 0.0, 0.0);
        Access::publish(e, ps4);
        CHECK(ps4.rx.scannerState == FOXAPI_SCANNER_IDLE);
        CHECK((ps4.rx.flags & FOXAPI_RX_SCANNER_ACTIVE) == 0u);
        CHECK(ps4.rx.scanCurrentHz == 144.0125e6);
    }

    // F: the plugin-mute cluster (API 0.3 8.7) is hashed in place into the lists' fingerprint, which the
    // pass computes every time: two plugins muting (names longer than any small string), the mute held after
    // a tune-away, and the fingerprint allocates nothing - and a pass whose lists did not change builds no
    // block either. The three things the cluster added are each IN it.
    {
        const std::string longName = "A decoder plugin whose name is longer than any small-string buffer";
        const std::string longKey = "a-decoder-plugin-whose-module-file-name-is-long-1.2.3-abi3-win-x64.dll";
        Access::muteCluster(e, {}, {}, false);
        const std::uint64_t quiet0 = Access::fingerprint(e);
        Access::muteCluster(e, {longName, "AIS"}, {longKey, "ais.dll"}, true);
        const std::uint64_t held = Access::fingerprint(e);  // (warm-up: anything the first call sizes)
        t_news = 0;
        t_counting = true;
        for (int i = 0; i < kPublishes; ++i) { (void)Access::fingerprint(e); }
        t_counting = false;
        std::printf("F: %d fingerprints with two plugins muting and the mute held: %ld heap allocations\n",
                    kPublishes, t_news);
        CHECK(t_news == 0);
        (void)Access::publishLists(e);  // the block is built here, once (and may allocate)
        t_news = 0;
        t_counting = true;
        for (int i = 0; i < kPublishes; ++i) { (void)Access::publishLists(e); }
        t_counting = false;
        std::printf("F: %d passes whose lists did not change: %ld heap allocations\n", kPublishes, t_news);
        CHECK(t_news == 0);
        CHECK(held != quiet0);
        // A name, a module key and the latch are each in the fingerprint: change one and it moves, put it
        // back and it is the same again.
        Access::muteCluster(e, {longName, "AIS 2"}, {longKey, "ais.dll"}, true);
        CHECK(Access::fingerprint(e) != held);
        Access::muteCluster(e, {longName, "AIS"}, {longKey, "ais2.dll"}, true);
        CHECK(Access::fingerprint(e) != held);
        Access::muteCluster(e, {longName, "AIS"}, {longKey, "ais.dll"}, false);
        CHECK(Access::fingerprint(e) != held);
        Access::muteCluster(e, {longName, "AIS"}, {longKey, "ais.dll"}, true);
        CHECK(Access::fingerprint(e) == held);
        // Handed back: the fingerprint of an engine with nothing muting.
        Access::muteCluster(e, {}, {}, false);
        CHECK(Access::fingerprint(e) == quiet0);
    }

    // F2: the same with two REAL plugins loaded - the fingerprint hashes, for each plugin and every pass, its
    // capability word, its two grants and its mute setting from the cached module keys, and builds no string to
    // do it. A grant, a mute override and a stop each move it; none allocates.
    {
        const char* base = std::getenv("LOCALAPPDATA");
        const std::filesystem::path dir = std::filesystem::path(base != nullptr ? base : ".") / "plugins_alloc";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        for (const char* src : {FIXTURE_STAGE1_PLUGIN, FIXTURE_GROUP8_PLUGIN}) {
            const std::filesystem::path p(src);
            std::filesystem::copy_file(p, dir / p.filename(), std::filesystem::copy_options::overwrite_existing, ec);
            CHECK(!ec);
        }
        g_pluginDir = dir.string();
        Access::setPluginDir(&pluginDirHook);
        {
            cascade::engine::Engine e2;
            e2.initialise();
            Access::setPluginDir(nullptr);
            const std::string key1 = std::filesystem::path(FIXTURE_STAGE1_PLUGIN).filename().string();
            const std::string key8 = std::filesystem::path(FIXTURE_GROUP8_PLUGIN).filename().string();
            const std::uint64_t plain = Access::fingerprint(e2);  // (warm-up: the cached keys are built here)
            t_news = 0;
            t_counting = true;
            for (int i = 0; i < kPublishes; ++i) { (void)Access::fingerprint(e2); }
            t_counting = false;
            std::printf("F2: %d fingerprints with two plugins loaded: %ld heap allocations\n", kPublishes, t_news);
            CHECK(t_news == 0);
            (void)Access::publishLists(e2);
            t_news = 0;
            t_counting = true;
            for (int i = 0; i < kPublishes; ++i) { (void)Access::publishLists(e2); }
            t_counting = false;
            CHECK(t_news == 0);
            // Each of the things hashed moves it.
            FoxCommand grant{};
            grant.structSize = sizeof(grant);
            grant.op = FOXAPI_OP_PLUGIN_GRANT;
            grant.ival[0] = 1;
            grant.ival[1] = 1;
            std::snprintf(grant.text, sizeof(grant.text), "%s", key8.c_str());
            Access::apply(e2, grant);
            const std::uint64_t granted = Access::fingerprint(e2);
            CHECK(granted != plain);
            grant.ival[0] = 2;
            Access::apply(e2, grant);
            const std::uint64_t both = Access::fingerprint(e2);
            CHECK(both != granted);
            FoxCommand mute{};
            mute.structSize = sizeof(mute);
            mute.op = FOXAPI_OP_PLUGIN_MUTE;
            mute.ival[0] = 1;
            std::snprintf(mute.text, sizeof(mute.text), "%s", key1.c_str());
            Access::apply(e2, mute);
            const std::uint64_t muting = Access::fingerprint(e2);
            CHECK(muting != both);
            FoxCommand stop{};
            stop.structSize = sizeof(stop);
            stop.op = FOXAPI_OP_DECODER_STOP;
            std::snprintf(stop.text, sizeof(stop.text), "%s", key8.c_str());
            Access::apply(e2, stop);
            CHECK(Access::fingerprint(e2) != muting);
            // ...and with all of it in force, still nothing allocates.
            t_news = 0;
            t_counting = true;
            for (int i = 0; i < kPublishes; ++i) { (void)Access::fingerprint(e2); }
            t_counting = false;
            std::printf("F2: %d fingerprints with grants, a mute override and a stop in force: %ld heap allocations\n",
                        kPublishes, t_news);
            CHECK(t_news == 0);
        }
        std::filesystem::remove_all(dir, ec);
    }

    // F3: the fitted modules' reads (API 0.3 8.8). Three real modules - the stage-1 decoder, the group-8 I/Q
    // decoder and the group-9 audio processor with two command keys - and a file the host refuses; one module
    // has a notice, another has asked for the settings grant, an install record disowns a third file. Every
    // pass hashes all of that in place (the cached keys, the plugin API's generations, the runner's) and builds
    // no block until something moved.
    {
        const char* base = std::getenv("LOCALAPPDATA");
        const std::filesystem::path dir = std::filesystem::path(base != nullptr ? base : ".") / "plugins_alloc9";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        for (const char* src : {FIXTURE_STAGE1_PLUGIN, FIXTURE_GROUP8_PLUGIN, FIXTURE_GROUP9_PLUGIN}) {
            const std::filesystem::path p(src);
            std::filesystem::copy_file(p, dir / p.filename(), std::filesystem::copy_options::overwrite_existing, ec);
            CHECK(!ec);
        }
        {
            std::ofstream junk(dir / "zz_refused.dll", std::ios::binary);
            junk << "this is not a portable executable";
        }
        g_pluginDir = dir.string();
        Access::setPluginDir(&pluginDirHook);
        {
            cascade::engine::Engine e3;
            e3.initialise();
            Access::setPluginDir(nullptr);
            const std::string key1 = std::filesystem::path(FIXTURE_STAGE1_PLUGIN).filename().string();
            const std::string key8 = std::filesystem::path(FIXTURE_GROUP8_PLUGIN).filename().string();
            const std::string key9 = std::filesystem::path(FIXTURE_GROUP9_PLUGIN).filename().string();
            cascade::core::PluginApiCore& core = Access::core(e3);
            cascade::core::PluginApiClient& c9 = core.client(key9, "Group Nine Fixture");
            cascade::core::PluginApiClient& c8 = core.client(key8, "Group Eight Fixture");
            CHECK(c9.live.load() && c8.live.load());

            // THE NOTICE: a warning longer than any small string, taken in by the pass that drains the plugin log.
            const std::string warning(300, 'w');
            CHECK(core.log(c9, CASCADE_LOG_WARN, warning.c_str()) == CASCADE_API_OK);
            Access::applyPluginApi(e3);
            // A request for the settings grant (answered DENIED: asking is what is recorded).
            cascade::core::PluginControl ask;
            ask.kind = cascade::core::PluginControl::Kind::Mode;
            ask.mode = CASCADE_DEMOD_NFM;
            CHECK(core.requestControl(c8, ask) == CASCADE_API_DENIED);
            // An install record that disowns the group-9 file.
            cascade::core::InstalledPlugin rec;
            rec.id = "group-nine-fixture";
            rec.file = key9;
            rec.digestMismatch = true;
            Access::inventory(e3).plugins.push_back(rec);

            const std::uint64_t settled = Access::fingerprint(e3);  // (warm-up: the cached keys, the scratch lists)
            (void)Access::publishLists(e3);                         // the block is built here, once, and may allocate
            t_news = 0;
            t_counting = true;
            for (int i = 0; i < kPublishes; ++i) { (void)Access::fingerprint(e3); }
            t_counting = false;
            std::printf("F3: %d fingerprints with three modules, a notice, a settings ask, command keys and an integrity finding: %ld heap allocations\n",
                        kPublishes, t_news);
            CHECK(t_news == 0);
            t_news = 0;
            t_counting = true;
            for (int i = 0; i < kPublishes; ++i) { (void)Access::publishLists(e3); }
            t_counting = false;
            std::printf("F3: %d passes whose lists did not change: %ld heap allocations\n", kPublishes, t_news);
            CHECK(t_news == 0);
            CHECK(Access::fingerprint(e3) == settled);
            // ...and the whole publish, which takes the runner's snapshot (MATCHED) whether or not the receiver runs.
            cascade::core::PublishedState ps;
            Access::publish(e3, ps);  // (warm-up)
            t_news = 0;
            t_counting = true;
            for (int i = 0; i < kPublishes; ++i) { Access::publish(e3, ps); }
            t_counting = false;
            std::printf("F3: %d publishes with the same: %ld heap allocations\n", kPublishes, t_news);
            CHECK(t_news == 0);
            // (The first publish took the runner's snapshot - the feeding keys, hashed whether or not the receiver
            // runs - so the settled fingerprint is the one after it.)
            const std::uint64_t quiet = Access::fingerprint(e3);
            CHECK(quiet != settled);
            (void)Access::publishLists(e3);
            CHECK(Access::fingerprint(e3) == quiet);

            // A PLUGIN THAT SAYS THE SAME WARNING AGAIN moves nothing: group 8's D3 (a list moved by what a
            // plugin prints) does not come back through the notice.
            for (int i = 0; i < 25; ++i) {
                CHECK(core.log(c9, CASCADE_LOG_WARN, warning.c_str()) == CASCADE_API_OK);
                Access::applyPluginApi(e3);
                CHECK(Access::fingerprint(e3) == quiet);
            }
            // Said again - a command key as it already is - or an INFO line: still nothing.
            CHECK(core.addCommand(c9, 1u, "RESET") == CASCADE_API_OK);
            CHECK(core.log(c9, CASCADE_LOG_INFO, "just a line") == CASCADE_API_OK);
            Access::applyPluginApi(e3);
            CHECK(Access::fingerprint(e3) == quiet);

            // THE MATCHED KEYS are hashed with the receiver STOPPED (PLUGINS' MATCHED is read then): this engine
            // has never run, and a key the runner says it would feed still moves the fingerprint.
            {
                std::vector<std::string>& fk = Access::feedingKeys(e3);
                fk.push_back("another-module.dll");
                CHECK(Access::fingerprint(e3) != quiet);
                fk.pop_back();
                CHECK(Access::fingerprint(e3) == quiet);
            }

            // EACH OF THE THINGS HASHED MOVES IT - and each rebuild of the block is counted (bounded, and it
            // happens once for the change, not once a pass).
            const auto movedBy = [&](const char* what, auto change) {
                const std::uint64_t before = Access::fingerprint(e3);
                change();
                const std::uint64_t after = Access::fingerprint(e3);
                CHECK(after != before);
                t_news = 0;
                t_counting = true;
                (void)Access::publishLists(e3);  // the rebuild
                t_counting = false;
                const long rebuild = t_news;
                t_news = 0;
                t_counting = true;
                for (int i = 0; i < kPublishes; ++i) { (void)Access::publishLists(e3); }
                t_counting = false;
                std::printf("F3: %-44s moved the fingerprint; the rebuild: %ld heap allocations, then %d settled passes: %ld\n",
                            what, rebuild, kPublishes, t_news);
                CHECK(rebuild > 0 && rebuild < 2000);  // counted, and bounded: some strings and vectors, not a flood
                CHECK(t_news == 0);
            };
            movedBy("another text at the same level", [&] {
                (void)core.log(c9, CASCADE_LOG_WARN, "another warning");
                Access::applyPluginApi(e3);
            });
            movedBy("the same text as an error", [&] {
                (void)core.log(c9, CASCADE_LOG_ERROR, "another warning");
                Access::applyPluginApi(e3);
            });
            movedBy("a command key added", [&] { (void)core.addCommand(c9, 3u, "EXTRA"); });
            movedBy("a command key relabelled", [&] { (void)core.addCommand(c9, 3u, "RELABELLED"); });
            movedBy("a command key removed", [&] { (void)core.removeCommand(c9, 3u); });
            movedBy("a first request for the settings grant", [&] { (void)core.requestControl(c9, ask); });
            movedBy("an install record cleared", [&] { Access::inventory(e3).plugins.back().digestMismatch = false; });
            // The runner's rows written again (a source change rebuilds them): its generation is hashed, so a
            // module's idle sentence that changes while the matched set does not cannot go stale.
            movedBy("the runner's status rows written again", [&] { Access::refreshRunner(e3); });
            movedBy("a rescan, counted alone", [&] { Access::countScan(e3); });
            movedBy("a rescan", [&] {
                FoxCommand rescan{};
                rescan.structSize = sizeof(rescan);
                rescan.op = FOXAPI_OP_PLUGIN_RESCAN;
                Access::apply(e3, rescan);
            });
            (void)key1;
        }
        std::filesystem::remove_all(dir, ec);
    }

    // F4: the plugin store's reads (API 0.3 8.9). A catalogue of 24 entries with descriptions of 528 to about 3000
    // bytes, a legal notice on every other one and two builds each; one retired row; a download in flight beside an
    // ADD ALL run. The catalogue's texts are never hashed one by one (a counter stands for them), the busy NAME is
    // (an ADD ALL run goes from one download to the next with the flag never clear), the progress is read live.
    {
        const auto makeCatalogue = [](char letter) {
            std::vector<cascade::core::PluginCatalogEntry> v;
            for (int i = 0; i < 24; ++i) {
                cascade::core::PluginCatalogEntry e;
                e.id = "plugin-" + std::to_string(i);
                e.name = "Plugin number " + std::to_string(i);
                e.version = "1.0." + std::to_string(i);
                e.author = std::string(30, letter);  // the only thing that differs between two catalogues
                e.licence = "MIT";
                e.summary = "A decoder.";
                e.description = std::string(static_cast<std::size_t>(528 + i * 107), 'd');
                e.homepage = "https://example.org/" + std::to_string(i);
                if (i % 2 == 0) { e.legalNotice = std::string(static_cast<std::size_t>(200 + i), 'L'); }
                e.abiVersion = CASCADE_PLUGIN_ABI_VERSION;
                e.compatible = true;
                e.minSupportedVersion = i % 3 == 0 ? "0.5.0" : "";
                cascade::core::PluginPlatform here;
                here.os = cascade::core::PluginRepo::hostOs();
                here.arch = cascade::core::PluginRepo::hostArch();
                here.file = "plugin_" + std::to_string(i) + ".dll";
                here.url = "https://example.org/x";
                here.sha256 = std::string(64, 'a');
                here.sizeBytes = 1000u + static_cast<unsigned>(i);
                e.platforms.push_back(here);
                cascade::core::PluginPlatform other;
                other.os = "plan9";
                other.arch = "mips";
                other.file = "plugin_" + std::to_string(i) + ".so";
                other.url = "https://example.org/y";
                other.sha256 = std::string(64, 'b');
                e.platforms.push_back(other);
                v.push_back(std::move(e));
            }
            return v;
        };

        cascade::engine::Engine e4;
        e4.initialise();
        const FoxEngineApi* const table = cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR);
        CHECK(table != nullptr);
        FoxSession* session = nullptr;
        {
            FoxSessionParams sp{};
            sp.structSize = sizeof(sp);
            sp.flags = FOXAPI_SESSION_LOCAL;
            sp.grants = FOXAPI_GRANT_VIEW;
            sp.clientName = "alloc";
            sp.clientVersion = "0";
            CHECK(table->open_session(e4.apiHandle(), &sp, &session) == FOXAPI_OK);
        }
        // The catalogue lands the way a fetch's result does, a retired row is on the books, a download is in flight
        // and an ADD ALL run is on its second module.
        Access::landCatalogue(e4, makeCatalogue('a'));
        {
            cascade::core::BlockedPlugin b;
            b.installed.file = "retired.dll";
            b.installed.id = "retired-id";
            b.installed.name = "Retired Plugin";
            b.installed.version = "1.0.0";
            b.message = "Retired Plugin 1.0.0 is out of date and has been disabled.";
            Access::blocked(e4).push_back(b);
        }
        Access::beginTransfer(e4, "Plugin number 1");
        Access::addAllRun(e4).active = true;
        Access::addAllRun(e4).total = 5;
        Access::addAllRun(e4).next = 2;
        Access::addAllRun(e4).currentName = "Plugin number 1";

        const std::uint64_t settled = Access::fingerprint(e4);  // (warm-up: the cached keys, the scratch lists)
        (void)Access::publishLists(e4);                         // the block is built here, once, and may allocate
        t_news = 0;
        t_counting = true;
        for (int i = 0; i < kPublishes; ++i) { (void)Access::fingerprint(e4); }
        t_counting = false;
        std::printf("F4: %d fingerprints with 24 catalogue entries, a retired row, a transfer and an ADD ALL run: %ld heap allocations\n",
                    kPublishes, t_news);
        CHECK(t_news == 0);
        t_news = 0;
        t_counting = true;
        for (int i = 0; i < kPublishes; ++i) { (void)Access::publishLists(e4); }
        t_counting = false;
        std::printf("F4: %d passes whose lists did not change: %ld heap allocations\n", kPublishes, t_news);
        CHECK(t_news == 0);
        CHECK(Access::fingerprint(e4) == settled);
        cascade::core::PublishedState ps;
        Access::publish(e4, ps);  // (warm-up)
        t_news = 0;
        t_counting = true;
        for (int i = 0; i < kPublishes; ++i) { Access::publish(e4, ps); }
        t_counting = false;
        std::printf("F4: %d publishes with the same: %ld heap allocations\n", kPublishes, t_news);
        CHECK(t_news == 0);
        const std::uint64_t quiet = Access::fingerprint(e4);
        (void)Access::publishLists(e4);
        CHECK(Access::fingerprint(e4) == quiet);

        // THE LIVE PROGRESS: 1000 reads of STORE_ACTIVITY while the figure moves allocate nothing, read what the repo
        // holds each time, and move neither the fingerprint nor the block (no rebuild: the publish after them is not one).
        {
            FoxListItem it{};
            it.structSize = sizeof(it);
            CHECK(table->read_list(session, FOXAPI_LIST_STORE_ACTIVITY, 0, &it) == 2);  // the transfer and the run
            bool exact = true;
            t_news = 0;
            t_counting = true;
            for (int i = 0; i < 1000; ++i) {
                const float p = static_cast<float>(i) / 1000.0f;
                Access::setProgress(e4, p);
                (void)table->read_list(session, FOXAPI_LIST_STORE_ACTIVITY, 0, &it);
                exact = exact && it.value[0] == static_cast<double>(p) && it.id == 1u;
                (void)table->read_list(session, FOXAPI_LIST_STORE_ACTIVITY, 1, &it);
                exact = exact && it.id == 2u && it.value[0] == 2.0 && it.value[1] == 5.0;
            }
            t_counting = false;
            std::printf("F4: 1000 reads of STORE_ACTIVITY (2 entries each) with the progress moving: %ld heap allocations\n", t_news);
            CHECK(t_news == 0);
            CHECK(exact);
            CHECK(Access::fingerprint(e4) == quiet);  // the progress is not hashed
            t_news = 0;
            t_counting = true;
            (void)Access::publishLists(e4);
            t_counting = false;
            CHECK(t_news == 0);  // ...and no block was built for it
        }

        // EACH INPUT THE STORE ADDS MOVES THE FINGERPRINT ALONE - and the rebuild of the block is counted (bounded, and
        // once for the change, not once a pass). Settled again after each.
        const auto movedBy = [&](const char* what, auto change) {
            const std::uint64_t before = Access::fingerprint(e4);
            change();
            const std::uint64_t after = Access::fingerprint(e4);
            CHECK(after != before);
            t_news = 0;
            t_counting = true;
            (void)Access::publishLists(e4);  // the rebuild
            t_counting = false;
            const long rebuild = t_news;
            t_news = 0;
            t_counting = true;
            for (int i = 0; i < kPublishes; ++i) { (void)Access::publishLists(e4); }
            t_counting = false;
            std::printf("F4: %-52s moved the fingerprint; the rebuild: %ld heap allocations, then %d settled passes: %ld\n",
                        what, rebuild, kPublishes, t_news);
            CHECK(rebuild > 0 && rebuild < 8000);  // counted, and bounded: 24 entries' facts, strings and vectors, not a flood
            CHECK(t_news == 0);
        };
        movedBy("the catalogue replaced (its counter alone)", [&] { Access::replaceCatalogue(e4, makeCatalogue('b')); });
        movedBy("the catalogue replaced again (same shape)", [&] { Access::replaceCatalogue(e4, makeCatalogue('c')); });
        movedBy("the index the store reads", [&] {
            FoxCommand url{};
            url.structSize = sizeof(url);
            url.op = FOXAPP_OP_SET_CATALOGUE_URL;
            std::snprintf(url.text, sizeof(url.text), "%s", "https://example.org/index.json");
            Access::apply(e4, url);
        });
        movedBy("the busy name (the flag held: ADD ALL's next module)", [&] { Access::busyName(e4) = "Plugin number 2"; });
        movedBy("the run's position", [&] { ++Access::addAllRun(e4).next; });
        movedBy("the run's total", [&] { ++Access::addAllRun(e4).total; });
        movedBy("the run's current name", [&] { Access::addAllRun(e4).currentName = "Plugin number 2"; });
        movedBy("the run ending (active cleared)", [&] { Access::addAllRun(e4).active = false; });
        movedBy("the run starting again (active set)", [&] { Access::addAllRun(e4).active = true; });
        movedBy("the summary's level (failed set, the text kept)", [&] { Access::addAllFailed(e4) = true; });
        movedBy("the ADD ALL summary's text", [&] { Access::addAllSummary(e4) = "2 installed, 1 failed."; });
        movedBy("the catalogue status line's text", [&] { Access::catalogStatus(e4) = "0 plugins in the catalogue"; });
        movedBy("a retired row's id", [&] { Access::blocked(e4)[0].installed.id = "retired-id-2"; });
        movedBy("a retired row's sentence", [&] {
            Access::blocked(e4)[0].message = "Retired Plugin 1.0.0 is out of date and has been disabled. Version 2.0.0 is available.";
        });
        movedBy("a second retired row", [&] {
            cascade::core::BlockedPlugin b;
            b.installed.file = "retired2.dll";
            b.installed.id = "retired-two";
            b.message = "Another.";
            Access::blocked(e4).push_back(b);
        });
        // A repeat of what is already so moves nothing: the same text, the same index, the same name.
        {
            const std::uint64_t now = Access::fingerprint(e4);
            Access::catalogStatus(e4) = "0 plugins in the catalogue";
            Access::addAllSummary(e4) = "2 installed, 1 failed.";
            Access::busyName(e4) = "Plugin number 2";
            FoxCommand url{};
            url.structSize = sizeof(url);
            url.op = FOXAPP_OP_SET_CATALOGUE_URL;
            std::snprintf(url.text, sizeof(url.text), "%s", "https://example.org/index.json");
            for (int i = 0; i < 25; ++i) { Access::apply(e4, url); }
            CHECK(Access::fingerprint(e4) == now);
            std::printf("F4: the same lines, name and index said again 25 times: the fingerprint is equal\n");
        }
        table->close_session(session);
    }

// <<F5 CASE BEGIN

    // F5: the Source section and the sound card (API 0.3 8.10). Six sound card inputs of eight rates each, a form, three
    // dongles that cannot be opened, two faulted drivers, the runtime's evidence (a list of paths, as one counter), a
    // 189-byte Pluto address, the kind kept for the config, a label and a 134-byte fault sentence: every pass's
    // fingerprint, an unchanged pass and the publish (with the section's flag word) allocate nothing, and 1000 reads of
    // each new list and fact allocate nothing; each input moves the fingerprint ALONE, the block's rebuild is counted
    // (bounded, once for the change), and a change said again moves nothing.
    {
        cascade::engine::Engine e5;
        e5.initialise();
        const FoxEngineApi* const table = cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR);
        CHECK(table != nullptr);
        FoxSession* session = nullptr;
        {
            FoxSessionParams sp{};
            sp.structSize = sizeof(sp);
            sp.flags = FOXAPI_SESSION_LOCAL;
            sp.grants = FOXAPI_GRANT_VIEW;
            sp.clientName = "alloc";
            sp.clientVersion = "0";
            CHECK(table->open_session(e5.apiHandle(), &sp, &session) == FOXAPI_OK);
        }
        for (int i = 0; i < 6; ++i) {
            cascade::source::SoundCardDevice c;
            c.index = i;
            c.name = "Fake input number " + std::to_string(i) + std::string(40, 'n');
            c.hostApi = "Fake host API";
            c.maxInputChannels = 2;
            c.defaultRateHz = 48000.0;
            for (int r = 0; r < 8; ++r) { c.rates.push_back({8000.0 * static_cast<double>(r + 2), r == 7}); }
            Access::cards(e5).push_back(c);
        }
        Access::cardListed(e5) = true;
        Access::form(e5).device = Access::cards(e5)[2].name;
        Access::form(e5).hostApi = "Fake host API";
        Access::form(e5).cardRateHz = 48000.0;
        Access::form(e5).format = cascade::source::SoundCardFormat::IqStereo;
        Access::form(e5).iqCentreHz = 7.1e6;
        Access::form(e5).pickedFromList = true;
        for (int i = 0; i < 3; ++i) {
            cascade::usb::UsbDeviceInfo u;
            u.vid = static_cast<std::uint16_t>(0x0BDA + i);
            u.pid = static_cast<std::uint16_t>(0x2838 + i);
            u.description = "A USB radio that cannot be opened, number " + std::to_string(i);
            Access::unbound(e5).push_back(u);
        }
        Access::faulted(e5) = {"uhd", "sdrplay"};
        for (int i = 0; i < 12; ++i) { Access::evidence(e5).modules.push_back("C:/Fake/modules/module" + std::to_string(i) + ".dll"); }
        Access::evidence(e5).searchPaths = {"C:/Fake/a", "C:/Fake/b"};
        ++Access::evidenceSeq(e5);
        std::snprintf(Access::plutoBuffer(e5), 192, "ip:%s", std::string(186, 'p').c_str());
        Access::keepKind(e5) = "soundcard";
        Access::busyLabel(e5) = "ADALM-Pluto at ip:192.168.2.1";
        Access::faultText(e5) =
            "source thread: the radio's driver is holding an abandoned thread and this source cannot restart; "
            "pick another source or restart FoxSDR";
        Access::absent(e5) = {"uhd"};
        Access::sdrRows(e5) = true;
        Access::scanned(e5) = true;

        const std::uint64_t settled = Access::fingerprint(e5);  // (warm-up)
        (void)Access::publishLists(e5);                         // the block is built here, once, and may allocate
        t_news = 0;
        t_counting = true;
        for (int i = 0; i < kPublishes; ++i) { (void)Access::fingerprint(e5); }
        t_counting = false;
        std::printf("F5: %d fingerprints with 6 sound card inputs, a form, 3 dongles, 2 faulted drivers, evidence and a fault sentence: %ld heap allocations\n",
                    kPublishes, t_news);
        CHECK(t_news == 0);
        t_news = 0;
        t_counting = true;
        for (int i = 0; i < kPublishes; ++i) { (void)Access::publishLists(e5); }
        t_counting = false;
        std::printf("F5: %d passes whose lists did not change: %ld heap allocations\n", kPublishes, t_news);
        CHECK(t_news == 0);
        CHECK(Access::fingerprint(e5) == settled);
        cascade::core::PublishedState ps;
        Access::publish(e5, ps);  // (warm-up)
        t_news = 0;
        t_counting = true;
        for (int i = 0; i < kPublishes; ++i) { Access::publish(e5, ps); }
        t_counting = false;
        std::printf("F5: %d publishes with the same (flag word %#x): %ld heap allocations\n", kPublishes, ps.rx.sourceFlags, t_news);
        CHECK(t_news == 0);
        // The flag word says what the engine holds: the scan started, the UHD note, the SDRplay row, the card list taken.
        CHECK((ps.rx.sourceFlags & FOXAPI_SRC_SCANNED) != 0u);
        CHECK((ps.rx.sourceFlags & FOXAPI_SRC_UHD_LEFT_OUT) != 0u);
        CHECK((ps.rx.sourceFlags & FOXAPI_SRC_SDRPLAY_ROWS) != 0u);
        CHECK((ps.rx.sourceFlags & FOXAPI_SRC_CARD_LISTED) != 0u);
        CHECK((ps.rx.sourceFlags & (FOXAPI_SRC_SCAN_PENDING | FOXAPI_SRC_CARD_SCANNING | FOXAPI_SRC_CARD_OPENING)) == 0u);
        const std::uint64_t quiet = Access::fingerprint(e5);
        (void)Access::publishLists(e5);
        CHECK(Access::fingerprint(e5) == quiet);

        // 1000 rounds of reading each new list's first entry allocate nothing: an interface reads them in its frame. (A
        // fact's read copies its text into the std::string get_setting builds it in, on the READER's thread and not in
        // a pass: that is the table's own design for every fact and is not asserted here.)
        {
            FoxListItem it{};
            it.structSize = sizeof(it);
            CHECK(table->read_list(session, FOXAPI_LIST_SOUND_CARDS, 0, &it) == 6);
            CHECK(table->read_list(session, FOXAPI_LIST_SOUND_CARD_RATES, 0, &it) == 8);
            CHECK(table->read_list(session, FOXAPI_LIST_UNBOUND_RADIOS, 0, &it) == 3);
            CHECK(table->read_list(session, FOXAPI_LIST_SCAN_EVIDENCE, 0, &it) == 14);
            CHECK(table->read_list(session, FOXAPI_LIST_SOUND_CARD_FORM, 0, &it) == 1);
            CHECK(table->read_list(session, FOXAPI_LIST_DEVICES, 0, &it) == 3);
            t_news = 0;
            t_counting = true;
            for (int i = 0; i < 1000; ++i) {
                for (const std::uint32_t list : {FOXAPI_LIST_SOUND_CARDS, FOXAPI_LIST_SOUND_CARD_RATES, FOXAPI_LIST_UNBOUND_RADIOS,
                                                 FOXAPI_LIST_SCAN_EVIDENCE, FOXAPI_LIST_SOUND_CARD_FORM, FOXAPI_LIST_DEVICES}) {
                    (void)table->read_list(session, list, 0, &it);
                }
            }
            t_counting = false;
            std::printf("F5: 1000 rounds of 6 list reads: %ld heap allocations\n", t_news);
            CHECK(t_news == 0);
            CHECK(Access::fingerprint(e5) == quiet);  // reads move nothing
        }

        // EACH INPUT MOVES THE FINGERPRINT ALONE, and the rebuild is counted (bounded: 6 cards x 8 rates and a few dozen
        // facts, not a flood), once for the change; settled again after each.
        const auto movedBy = [&](const char* what, auto change) {
            const std::uint64_t before = Access::fingerprint(e5);
            change();
            const std::uint64_t after = Access::fingerprint(e5);
            CHECK(after != before);
            t_news = 0;
            t_counting = true;
            (void)Access::publishLists(e5);  // the rebuild
            t_counting = false;
            const long rebuild = t_news;
            t_news = 0;
            t_counting = true;
            for (int i = 0; i < kPublishes; ++i) { (void)Access::publishLists(e5); }
            t_counting = false;
            std::printf("F5: %-46s moved the fingerprint; the rebuild: %ld heap allocations, then %d settled passes: %ld\n", what,
                        rebuild, kPublishes, t_news);
            CHECK(rebuild > 0 && rebuild < 4000);
            CHECK(t_news == 0);
        };
        movedBy("the combo's row", [&] { Access::sourceSel(e5) = 3; });
        movedBy("the kind kept for the config", [&] { Access::keepKind(e5) = "file"; });
        movedBy("the label of the radio being opened", [&] { Access::busyLabel(e5) = "Airspy R2"; });
        movedBy("the Pluto's address", [&] { std::snprintf(Access::plutoBuffer(e5), 192, "%s", "ip:10.0.0.9"); });
        movedBy("a dongle's product id", [&] { Access::unbound(e5)[1].pid = 0x6089; });
        movedBy("a dongle's description", [&] { Access::unbound(e5)[2].description = "Renamed"; });
        movedBy("a faulted driver", [&] { Access::faulted(e5).push_back("miri"); });
        movedBy("the runtime's evidence (its counter)", [&] { ++Access::evidenceSeq(e5); });
        movedBy("the whole fault sentence", [&] { Access::faultText(e5) += " (and then it stopped)"; });
        movedBy("a driver left out of the scan", [&] { Access::absent(e5).clear(); });
        movedBy("an SDRplay row", [&] { Access::sdrRows(e5) = false; });
        movedBy("look for network USRPs", [&] { Access::networkUsrps(e5) = true; });
        movedBy("the card list taken", [&] { Access::cardListed(e5) = false; });
        movedBy("the form's rate", [&] { Access::form(e5).cardRateHz = 96000.0; });
        movedBy("the form's centre", [&] { Access::form(e5).iqCentreHz = 7.2e6; });
        movedBy("the form's format", [&] { Access::form(e5).format = cascade::source::SoundCardFormat::RealMono; });
        movedBy("an input's rate", [&] { Access::cards(e5)[2].rates[3].hz = 33333.0; });
        movedBy("an input's exclusive flag", [&] { Access::cards(e5)[2].rates[3].exclusive = true; });
        movedBy("an input's name", [&] { Access::cards(e5)[5].name += "x"; });
        // A repeat of what is already so moves nothing.
        {
            const std::uint64_t now = Access::fingerprint(e5);
            Access::keepKind(e5) = "file";
            Access::busyLabel(e5) = "Airspy R2";
            Access::form(e5).cardRateHz = 96000.0;
            Access::sourceSel(e5) = 3;
            for (int i = 0; i < 25; ++i) { (void)Access::publishLists(e5); }
            CHECK(Access::fingerprint(e5) == now);
            std::printf("F5: the same kind, label, rate and row said again 25 times: the fingerprint is equal\n");
        }
        table->close_session(session);
    }

// F5 CASE END>>
// <<F6 CASE BEGIN

    // F6: the radar scope and the demod scope (API 0.3 8.11). The receiver's position in the state and the scope tap's
    // capacity add nothing to the pass: with a position set the publish allocates nothing (as it did with none), it says
    // the position, and 1000 rounds of read_state and of the three taps' info-only read_scope (a LOCAL
    // session) allocate nothing. The position moves no list fingerprint.
    {
        cascade::engine::Engine e6;
        e6.initialise();
        const FoxEngineApi* const table = cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR);
        CHECK(table != nullptr);
        FoxSession* local = nullptr;
        {
            FoxSessionParams sp{};
            sp.structSize = sizeof(sp);
            sp.flags = FOXAPI_SESSION_LOCAL;
            sp.grants = FOXAPI_GRANT_VIEW;
            sp.clientName = "alloc";
            sp.clientVersion = "0";
            CHECK(table->open_session(e6.apiHandle(), &sp, &local) == FOXAPI_OK);
        }
        const long none = allocationsOfPublishes(e6);
        const std::uint64_t listsBefore = Access::fingerprint(e6);
        Access::position(e6, true, 51.5074, -0.1278);
        const long placed = allocationsOfPublishes(e6);
        std::printf("F6: %d publishes with no position: %ld heap allocations, with one: %ld\n", kPublishes, none, placed);
        CHECK(none == 0);
        CHECK(placed == 0);
        CHECK(Access::fingerprint(e6) == listsBefore);  // the position is the state's, not the lists'
        cascade::core::PublishedState ps;
        Access::publish(e6, ps);
        CHECK((ps.rx.flags & FOXAPI_RX_POSITION_SET) != 0u);
        CHECK(ps.rx.rxLatDeg == 51.5074);
        CHECK(ps.rx.rxLonDeg == -0.1278);
        Access::position(e6, false, 51.5074, -0.1278);  // cleared: the pair is not published as a place
        Access::publish(e6, ps);
        CHECK((ps.rx.flags & FOXAPI_RX_POSITION_SET) == 0u);
        CHECK(ps.rx.rxLatDeg == 0.0 && ps.rx.rxLonDeg == 0.0);
        Access::position(e6, true, 51.5074, -0.1278);
        e6.controlPass();  // (the pass publishes for the readers below)

        FoxReceiverState st{};
        st.structSize = sizeof(st);
        FoxScopeInfo info{};
        info.structSize = sizeof(info);
        CHECK(table->read_state(local, &st) == FOXAPI_OK);  // (warm-up)
        CHECK((st.flags & FOXAPI_RX_POSITION_SET) != 0u && st.rxLatDeg == 51.5074 && st.rxLonDeg == -0.1278);
        t_news = 0;
        t_counting = true;
        for (int i = 0; i < 1000; ++i) {
            (void)table->read_state(local, &st);
            for (const std::uint32_t tap : {FOXAPI_SCOPE_AUDIO, FOXAPI_SCOPE_IQ, FOXAPI_SCOPE_MPX}) {
                (void)table->read_scope(local, tap, &info, nullptr, 0);
            }
        }
        t_counting = false;
        std::printf("F6: 1000 rounds of read_state and three info-only read_scope calls: %ld heap allocations (capacity %u)\n",
                    t_news, info.capacityFrames);
        CHECK(t_news == 0);
        CHECK(info.capacityFrames == 65536u);  // the last call was the multiplex tap's
        table->close_session(local);
    }

// F6 CASE END>>
// <<F7 CASE BEGIN

    // F7: the Patch page, part 1 (API 0.3 8.12). A graph of eight nodes, a plan of two channels and two decoders and a
    // parts bin of three modules, the way the pass leaves them: every pass's fingerprint, an unchanged pass and the publish
    // allocate nothing (noticing a change to the graph is one comparison with the newest retained version), and so do 1000
    // rounds of reading the version, the epoch, `runnable` and the plan; each input moves the fingerprint ALONE and the
    // rebuild is counted (bounded, once for the change); a change made to the graph behind the engine's back is a version
    // when the publish looks, and the same change said again moves nothing.
    {
        namespace pc = cascade::core::patch;
        cascade::engine::Engine e7;
        e7.initialise();
        const FoxEngineApi* const table = cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR);
        CHECK(table != nullptr);
        FoxSession* session = nullptr;
        {
            FoxSessionParams sp{};
            sp.structSize = sizeof(sp);
            sp.flags = FOXAPI_SESSION_LOCAL;
            sp.grants = FOXAPI_GRANT_VIEW;
            sp.clientName = "alloc";
            sp.clientVersion = "0";
            CHECK(table->open_session(e7.apiHandle(), &sp, &session) == FOXAPI_OK);
        }
        pc::Graph& g = Access::patchGraph(e7);
        const pc::NodeId radio = g.addNode(pc::NodeKind::Radio, "Radio with a name of some length", pc::PortType::Iq, 40.0f, 40.0f);
        const pc::NodeId chanA = g.addNode(pc::NodeKind::Channel, "Channel A", pc::PortType::Iq, 420.0f, 40.0f);
        const pc::NodeId chanB = g.addNode(pc::NodeKind::Channel, "Channel B", pc::PortType::Iq, 420.0f, 200.0f);
        const pc::NodeId demod = g.addNode(pc::NodeKind::Demod, "Demod", pc::PortType::Iq, 700.0f, 40.0f);
        const pc::NodeId decA = g.addNode(pc::NodeKind::Decoder, "Decoder A", pc::PortType::Iq, 700.0f, 200.0f);
        const pc::NodeId decB = g.addNode(pc::NodeKind::Decoder, "Decoder B", pc::PortType::Audio, 980.0f, 40.0f);
        (void)g.addNode(pc::NodeKind::Display, "Spectrum", pc::PortType::Iq, 40.0f, 400.0f);
        (void)g.addNode(pc::NodeKind::Sink, "Speaker", pc::PortType::Audio, 980.0f, 200.0f);
        if (pc::Node* n = g.mutableNode(radio)) {
            n->device = pc::kGeneratorKey;
            n->freqHz = 145.0e6;
            n->rateHz = 2.4e6;
        }
        pc::Plan& plan = Access::patchPlan(e7);
        plan.runnable = true;
        plan.channels = {{chanA, 50000.0, 50, 48000.0, radio}, {chanB, -125000.0, 50, 48000.0, radio}};
        {
            pc::DecoderPlan a;
            a.node = decA;
            a.plugin = 0;
            a.source = pc::DecoderSource::Channel;
            a.channel = chanA;
            a.inRateHz = 48000.0;
            a.rateHz = 48000.0;
            a.centreHz = 145.05e6;
            a.radio = radio;
            pc::DecoderPlan b = a;
            b.node = decB;
            b.plugin = 1;
            b.source = pc::DecoderSource::Audio;
            b.centreHz = 0.0;
            plan.decoders = {a, b};
        }
        (void)demod;
        std::vector<pc::DecoderInfo>& bin = Access::patchCatalogue(e7);
        for (int i = 0; i < 3; ++i) {
            pc::DecoderInfo d;
            d.key = "module_" + std::to_string(i) + ".dll";
            d.name = "Module number " + std::to_string(i);
            d.feed = i == 1 ? pc::PortType::Audio : pc::PortType::Iq;
            d.requiredRateHz = 48000.0 * static_cast<double>(i);
            bin.push_back(d);
        }
        (void)Access::publishLists(e7);  // the first block, and the first retained version
        const long none = [&] {
            t_news = 0;
            t_counting = true;
            for (int i = 0; i < kPublishes; ++i) { (void)Access::publishLists(e7); }
            t_counting = false;
            return t_news;
        }();
        std::printf("F7: %d settled publishes with a graph of 8 nodes, a plan of 4 entries and a parts bin of 3: %ld heap allocations\n",
                    kPublishes, none);
        CHECK(none == 0);
        CHECK(Access::patchVersion(e7) == 1u);  // nothing noticed a change: the graph was never different from itself
        // 1000 rounds of reading what the page adds allocate nothing.
        {
            char text[8192];
            FoxListItem it{};
            it.structSize = sizeof(it);
            CHECK(table->get_setting(session, "fact.patch.version", text, sizeof(text)) > 0);  // (warm-up)
            t_news = 0;
            t_counting = true;
            for (int i = 0; i < 1000; ++i) {
                (void)table->get_setting(session, "fact.patch.version", text, sizeof(text));
                (void)table->get_setting(session, "fact.patch.epoch", text, sizeof(text));
                (void)table->get_setting(session, "fact.patch.runnable", text, sizeof(text));
                (void)table->read_list(session, FOXAPI_LIST_PATCH_PLAN, 0, nullptr);
                (void)table->read_list(session, FOXAPI_LIST_PATCH_PLAN, static_cast<std::uint32_t>(i % 4), &it);
            }
            t_counting = false;
            std::printf("F7: 1000 rounds of the version, epoch, runnable and a plan entry: %ld heap allocations\n", t_news);
            CHECK(t_news == 0);
        }
        // The content: three facts and four plan entries, as the pass's block carries them.
        {
            char text[64];
            CHECK(table->get_setting(session, "fact.patch.version", text, sizeof(text)) > 0);
            CHECK(std::string(text) == "1");
            CHECK(table->get_setting(session, "fact.patch.runnable", text, sizeof(text)) > 0);
            CHECK(std::string(text) == "1");
            CHECK(table->read_list(session, FOXAPI_LIST_PATCH_PLAN, 0, nullptr) == 4);
        }

        // EACH INPUT MOVES THE FINGERPRINT ALONE, and the rebuild is counted (bounded), once for the change.
        const auto movedBy = [&](const char* what, auto change) {
            const std::uint64_t before = Access::fingerprint(e7);
            change();
            const std::uint64_t after = Access::fingerprint(e7);
            CHECK(after != before);
            t_news = 0;
            t_counting = true;
            (void)Access::publishLists(e7);  // the rebuild
            t_counting = false;
            const long rebuild = t_news;
            t_news = 0;
            t_counting = true;
            for (int i = 0; i < kPublishes; ++i) { (void)Access::publishLists(e7); }
            t_counting = false;
            std::printf("F7: %-44s moved the fingerprint; the rebuild: %ld heap allocations, then %d settled passes: %ld\n", what,
                        rebuild, kPublishes, t_news);
            CHECK(rebuild > 0 && rebuild < 3000);
            CHECK(t_news == 0);
        };
        movedBy("the version, alone", [&] { ++Access::patchVersion(e7); });
        movedBy("the epoch, alone", [&] { ++Access::patchEpoch(e7); });
        movedBy("runnable", [&] { plan.runnable = false; });
        movedBy("runnable again", [&] { plan.runnable = true; });
        movedBy("a channel's node", [&] { plan.channels[0].node = chanB + 1u; });
        movedBy("a channel's offset", [&] { plan.channels[0].offsetHz = 60000.0; });
        movedBy("a channel's decimation", [&] { plan.channels[0].decimation = 25; });
        movedBy("a channel's output rate", [&] { plan.channels[0].outRateHz = 96000.0; });
        movedBy("a channel's radio", [&] { plan.channels[1].radio = radio + 1u; });
        movedBy("a decoder's node", [&] { plan.decoders[0].node = decB + 1u; });
        movedBy("a decoder's plugin", [&] { plan.decoders[0].plugin = 2; });
        movedBy("a decoder's source", [&] { plan.decoders[0].source = pc::DecoderSource::Radio; });
        movedBy("a decoder's input rate", [&] { plan.decoders[1].inRateHz = 24000.0; });
        movedBy("a decoder's own rate", [&] { plan.decoders[1].rateHz = 24000.0; });
        movedBy("a decoder's centre", [&] { plan.decoders[0].centreHz = 145.06e6; });
        movedBy("a decoder's radio", [&] { plan.decoders[1].radio = radio + 1u; });
        movedBy("a plan entry added", [&] { plan.channels.push_back({chanB + 2u, 0.0, 50, 48000.0, radio}); });
        movedBy("a parts-bin entry's key", [&] { bin[0].key = "module_0_v2.dll"; });
        movedBy("a parts-bin entry's name", [&] { bin[1].name = "Module number one"; });
        movedBy("a parts-bin entry's input", [&] { bin[2].feed = pc::PortType::Audio; });
        movedBy("a parts-bin entry's rate", [&] { bin[2].requiredRateHz = 12000.0; });
        movedBy("a parts-bin entry's picture bit", [&] { bin[0].image = true; });
        movedBy("a parts-bin entry's track bit", [&] { bin[1].tracks = true; });
        movedBy("a parts-bin entry replaced by another", [&] {
            bin[2].key = "module_2_other.dll";  // the same count, a different module: the count alone would miss it
        });

        // A CHANGE TO THE GRAPH is a version, noticed by the publish: nothing else bumped it, and the block is built once
        // for it. The same change said again (the graph is already so) moves nothing.
        {
            (void)Access::publishLists(e7);
            const std::uint64_t v0 = Access::patchVersion(e7);
            if (pc::Node* n = g.mutableNode(chanA)) { n->freqHz = 145.123e6; }
            CHECK(Access::patchVersion(e7) == v0);  // (a plain write: nobody has looked)
            const std::uint64_t before = Access::fingerprint(e7);
            t_news = 0;
            t_counting = true;
            (void)Access::publishLists(e7);
            t_counting = false;
            const long noticed = t_news;
            CHECK(Access::patchVersion(e7) == v0 + 1u);
            CHECK(Access::fingerprint(e7) != before);
            std::printf("F7: a graph change behind the engine's back: noticed as version %llu by the publish (%ld heap allocations)\n",
                        static_cast<unsigned long long>(v0 + 1u), noticed);
            CHECK(noticed > 0 && noticed < 3000);
            for (int i = 0; i < 25; ++i) { (void)Access::publishLists(e7); }
            CHECK(Access::patchVersion(e7) == v0 + 1u);
            if (pc::Node* n = g.mutableNode(chanA)) { n->freqHz = 145.123e6; }  // said again
            for (int i = 0; i < 25; ++i) { (void)Access::publishLists(e7); }
            CHECK(Access::patchVersion(e7) == v0 + 1u);
            std::printf("F7: the same change said again 25 times: the version stays\n");
        }
        table->close_session(session);
    }

// F7 CASE END>>
// <<F8 CASE BEGIN

    // F8: the Patch page, part 2 (API 0.3 8.12). PATCH_LIVE is a block of its own, copied once a pass and fingerprinted by
    // itself: with a speaker's output, a speaker whose output could not be made and two decoders' faces, a settled pass
    // (the same readings) copies nothing, builds nothing and allocates nothing; a reading that moves rebuilds THAT block
    // (bounded, once) and never the list block - listSeq's fingerprint stays where it was, which is the point of the block.
    // And each new input of the device list moves the list fingerprint alone.
    {
        namespace pc = cascade::core::patch;
        // A speaker's output with nothing behind it: it counts what it is written and describes itself.
        struct Dest : pc::AudioDest {
            std::string error_;
            void write(const float* samples, std::size_t n) override { note(samples, n); }
            std::string describe() const override { return "TEST  speaker.wav"; }
            std::string error() const override { return error_; }
            void push(float level, std::size_t n) {
                std::vector<float> v(n, level);
                note(v.data(), n);
            }
        };
        cascade::engine::Engine e8;
        e8.initialise();
        const FoxEngineApi* const table = cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR);
        CHECK(table != nullptr);
        FoxSession* session = nullptr;
        {
            FoxSessionParams sp{};
            sp.structSize = sizeof(sp);
            sp.flags = FOXAPI_SESSION_LOCAL;
            sp.grants = FOXAPI_GRANT_VIEW;
            sp.clientName = "alloc8";
            sp.clientVersion = "0";
            CHECK(table->open_session(e8.apiHandle(), &sp, &session) == FOXAPI_OK);
        }
        pc::Graph& g = Access::patchGraph(e8);
        const pc::NodeId radio = g.addNode(pc::NodeKind::Radio, "Radio", pc::PortType::Iq, 40.0f, 40.0f);
        const pc::NodeId speaker = g.addNode(pc::NodeKind::Sink, "Speaker", pc::PortType::Audio, 700.0f, 40.0f);
        const pc::NodeId broken = g.addNode(pc::NodeKind::Sink, "Broken speaker", pc::PortType::Audio, 700.0f, 200.0f);
        const pc::NodeId decA = g.addNode(pc::NodeKind::Decoder, "Decoder A", pc::PortType::Iq, 420.0f, 40.0f);
        const pc::NodeId decB = g.addNode(pc::NodeKind::Decoder, "Decoder B", pc::PortType::Iq, 420.0f, 200.0f);
        (void)radio;
        auto dest = std::make_shared<Dest>();
        dest->push(0.25f, 480);
        Access::patchDests(e8).push_back({speaker, dest});
        Access::patchDestError(e8)[broken] = "recorder: cannot create the file";
        pc::Plan& plan = Access::patchPlan(e8);
        for (const pc::NodeId n : {decA, decB}) {
            pc::DecoderPlan d;
            d.node = n;
            d.source = pc::DecoderSource::Radio;
            d.radio = radio;
            d.inRateHz = d.rateHz = 2.4e6;
            plan.decoders.push_back(d);
        }
        Access::setFace(e8, decA, 3, "decoded line three of the first decoder, long enough not to fit a small string buffer");
        (void)Access::publishLists(e8);
        const std::uint64_t listFp = Access::fingerprint(e8);
        const int rows = table->read_list(session, FOXAPI_LIST_PATCH_LIVE, 0, nullptr);
        std::printf("F8: PATCH_LIVE with a speaker, a broken speaker and two faces: %d rows\n", rows);
        CHECK(rows == 4);  // kind 2 speaker, kind 5 broken speaker, kind 4 twice (the second has said nothing: 0 lines)
        const long none = [&] {
            t_news = 0;
            t_counting = true;
            for (int i = 0; i < kPublishes; ++i) { (void)Access::publishLists(e8); }
            t_counting = false;
            return t_news;
        }();
        std::printf("F8: %d settled publishes with those readings: %ld heap allocations\n", kPublishes, none);
        CHECK(none == 0);
        CHECK(Access::fingerprint(e8) == listFp);
        // 1000 rounds of reading the list allocate nothing either.
        {
            FoxListItem it{};
            it.structSize = sizeof(it);
            (void)table->read_list(session, FOXAPI_LIST_PATCH_LIVE, 0, &it);  // warm-up
            t_news = 0;
            t_counting = true;
            for (int i = 0; i < 1000; ++i) {
                (void)table->read_list(session, FOXAPI_LIST_PATCH_LIVE, 0, nullptr);
                (void)table->read_list(session, FOXAPI_LIST_PATCH_LIVE, static_cast<std::uint32_t>(i % 4), &it);
            }
            t_counting = false;
            std::printf("F8: 1000 rounds of reading PATCH_LIVE: %ld heap allocations\n", t_news);
            CHECK(t_news == 0);
        }

        // EACH READING MOVES THE LIVE BLOCK ALONE: its row changes, the list block's fingerprint does not, the rebuild is
        // bounded and the passes after it are settled again.
        const auto liveMoved = [&](const char* what, auto change, auto rowChanged) {
            change();
            t_news = 0;
            t_counting = true;
            (void)Access::publishLists(e8);
            t_counting = false;
            const long rebuild = t_news;
            t_news = 0;
            t_counting = true;
            for (int i = 0; i < kPublishes; ++i) { (void)Access::publishLists(e8); }
            t_counting = false;
            std::printf("F8: %-40s the live rebuild: %ld heap allocations, then %d settled passes: %ld\n", what, rebuild,
                        kPublishes, t_news);
            CHECK(rowChanged());
            CHECK(Access::fingerprint(e8) == listFp);  // (the list block did not move)
            CHECK(rebuild > 0 && rebuild < 200);
            CHECK(t_news == 0);
        };
        const auto row = [&](int kind, pc::NodeId node) {
            FoxListItem it{};
            it.structSize = sizeof(it);
            const int32_t n = table->read_list(session, FOXAPI_LIST_PATCH_LIVE, 0, nullptr);
            for (int32_t i = 0; i < n; ++i) {
                FoxListItem x{};
                x.structSize = sizeof(x);
                (void)table->read_list(session, FOXAPI_LIST_PATCH_LIVE, static_cast<std::uint32_t>(i), &x);
                if (x.id == node && static_cast<int>(x.value[2]) == kind) { return x; }
            }
            return it;
        };
        liveMoved("a speaker's peak", [&] { dest->push(0.75f, 0); }, [&] { return row(2, speaker).value[0] == 0.0; });
        liveMoved("a speaker's samples (with the peak)", [&] { dest->push(0.75f, 960); },
                  [&] { return row(2, speaker).value[1] == 480.0 + 960.0 && row(2, speaker).value[0] == 0.75; });
        liveMoved("a speaker's samples, the peak held", [&] { dest->push(0.75f, 480); },
                  [&] { return row(2, speaker).value[1] == 480.0 + 960.0 + 480.0 && row(2, speaker).value[0] == 0.75; });
        liveMoved("a speaker's error", [&] { dest->error_ = "disk full"; },
                  [&] { return std::string(row(2, speaker).detail) == "disk full"; });
        liveMoved("a speaker's output error", [&] { Access::patchDestError(e8)[broken] = "recorder: cannot open it"; },
                  [&] { return std::string(row(5, broken).detail) == "recorder: cannot open it"; });
        liveMoved("a decoder's count", [&] { Access::setFace(e8, decB, 1, "first"); },
                  [&] { return row(4, decB).value[0] == 1.0; });
        liveMoved("a decoder's newest line, the count alone", [&] { Access::setFace(e8, decB, 1, "other"); },
                  [&] { return std::string(row(4, decB).detail) == "other"; });
        liveMoved("a speaker gone", [&] { Access::patchDests(e8).clear(); }, [&] { return row(2, speaker).id == 0u; });
        // (A decoder that is refused or leaves the plan moves the PLAN, which is the list block's: its face goes with it.)

        // THE DEVICE LIST'S INPUTS move the LIST fingerprint, each alone.
        const auto movedBy = [&](const char* what, auto change) {
            const std::uint64_t before = Access::fingerprint(e8);
            change();
            const std::uint64_t after = Access::fingerprint(e8);
            CHECK(after != before);
            std::printf("F8: %-44s moved the list fingerprint\n", what);
        };
        auto& keep = Access::patchMainKeep(e8);
        movedBy("the lent radio, valid", [&] { keep.valid = true; });
        movedBy("the lent radio's kind", [&] { keep.kind = "rtlsdr"; });
        movedBy("the lent radio's args", [&] { keep.args = "serial=00000001"; });
        movedBy("the lent radio's label", [&] { keep.label = "RTL-SDR (serial 00000001)"; });
        std::vector<pc::RecordingInfo>& recs = Access::patchRecordings(e8);
        recs.push_back({"C:/rec/a.wav", "a.wav", pc::makeIqFileKey("C:/rec/a.wav"), 96000.0});
        movedBy("a recording's file (the count held)", [&] {
            recs[0] = {"C:/rec/b.wav", "b.wav", pc::makeIqFileKey("C:/rec/b.wav"), 96000.0};
        });
        movedBy("a recording's rate", [&] { recs[0].rateHz = 250000.0; });
        table->close_session(session);
    }

// F8 CASE END>>
// <<F9 CASE BEGIN

    // F9: the Patch page, part 3 (API 0.3 8.12): a decoded picture is the engine's. A picture the runner hands over is moved in
    // WHOLE behind a const pointer (the engine makes no second copy of the pixels, so what a new picture costs does not grow
    // with its size), the set read_image serves is rebuilt once for it and the PATCH_LIVE block once for it; a settled pass
    // allocates nothing, 1000 rounds of reading the picture (the info alone and the pixels) allocate nothing, and none of it
    // moves the list block's fingerprint.
    {
        namespace pc = cascade::core::patch;
        cascade::engine::Engine e9;
        e9.initialise();
        const FoxEngineApi* const table = cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR);
        CHECK(table != nullptr);
        FoxSession* session = nullptr;
        {
            FoxSessionParams sp{};
            sp.structSize = sizeof(sp);
            sp.flags = FOXAPI_SESSION_LOCAL;
            sp.grants = FOXAPI_GRANT_VIEW;
            sp.clientName = "alloc9";
            sp.clientVersion = "0";
            CHECK(table->open_session(e9.apiHandle(), &sp, &session) == FOXAPI_OK);
        }
        pc::Graph& g = Access::patchGraph(e9);
        const pc::NodeId decA = g.addNode(pc::NodeKind::Decoder, "Picture decoder A", pc::PortType::Audio, 420.0f, 40.0f);
        const pc::NodeId decB = g.addNode(pc::NodeKind::Decoder, "Picture decoder B", pc::PortType::Audio, 420.0f, 200.0f);
        (void)Access::publishLists(e9);  // (settles the graph's version, which the first publish notices)
        (void)Access::publishLists(e9);
        const std::uint64_t listFp = Access::fingerprint(e9);
        const auto liveRows = [&] { return table->read_list(session, FOXAPI_LIST_PATCH_LIVE, 0, nullptr); };
        CHECK(liveRows() == 0);
        // A picture's cost in allocations - the take and the publish that builds the set, the block and the event - for a
        // small picture and one 1000 times the size: the same, because the pixels are moved and never copied.
        // What the engine does with a picture costs, in allocations and in BYTES: the take (which includes the test's own
        // making of the picture - its pixel vector - counted apart) and the publish that builds the set and the block.
        long long bytesOfBig = 0, takeBytes = 0;
        const auto costOf = [&](pc::NodeId node, std::uint32_t w, std::uint32_t h, std::uint32_t format, std::uint8_t fill) {
            t_news = 0;
            t_bytes = 0;
            t_counting = true;
            Access::takeImage(e9, node, w, h, format, fill);
            t_counting = false;
            const long making = t_news;
            const long long madeBytes = t_bytes;
            takeBytes = madeBytes;
            t_news = 0;
            t_bytes = 0;
            t_counting = true;
            (void)Access::publishLists(e9);
            t_counting = false;
            const long publish = t_news;
            bytesOfBig = t_bytes;
            std::printf("F9: a %ux%u picture on node %u: taking it %ld allocation(s) (%lld bytes, its pixels among them), the publish "
                        "that builds the set and the block %ld (%lld bytes)\n",
                        w, h, static_cast<unsigned>(node), making, madeBytes, publish, bytesOfBig);
            return publish;
        };
        const long small = costOf(decA, 8, 6, CASCADE_IMAGE_RGB24, 0x11);
        const long big = costOf(decB, 800, 600, CASCADE_IMAGE_RGB24, 0x22);
        CHECK(small > 0 && small < 40);
        CHECK(big > 0 && big < 40);
        // A 1.44 MB picture costs the publish no more than a few KB: its pixels were moved, never copied.
        CHECK(bytesOfBig < 8192);
        // ...and the take allocated the picture's pixels (the test's own making of them) ONCE: the engine's move copied none.
        CHECK(takeBytes < 800ll * 600ll * 3ll + 4096ll);
        CHECK(Access::fingerprint(e9) == listFp);
        // SETTLED: the same pictures, publish after publish - nothing.
        const long settled = [&] {
            t_news = 0;
            t_counting = true;
            for (int i = 0; i < kPublishes; ++i) { (void)Access::publishLists(e9); }
            t_counting = false;
            return t_news;
        }();
        std::printf("F9: %d settled publishes with two pictures held: %ld heap allocations\n", kPublishes, settled);
        CHECK(settled == 0);
        // 1000 rounds of reading: the row, the picture's info alone and its pixels - nothing.
        {
            std::vector<std::uint8_t> px(800u * 600u * 3u);
            FoxListItem it{};
            it.structSize = sizeof(it);
            (void)table->read_list(session, FOXAPI_LIST_PATCH_LIVE, 0, &it);
            const std::uint64_t idA = static_cast<std::uint64_t>(it.value[0]);
            FoxImageInfo info{};
            info.structSize = sizeof(info);
            CHECK(table->read_image(session, idA, &info, px.data(), px.size()) == 8 * 6 * 3);  // (warm-up, and it is the picture)
            CHECK(info.width == 8u && info.height == 6u && info.format == CASCADE_IMAGE_RGB24);
            t_news = 0;
            t_counting = true;
            for (int i = 0; i < 1000; ++i) {
                (void)table->read_list(session, FOXAPI_LIST_PATCH_LIVE, static_cast<std::uint32_t>(i % 2), &it);
                (void)table->read_image(session, static_cast<std::uint64_t>(it.value[0]), &info, nullptr, 0);
                (void)table->read_image(session, static_cast<std::uint64_t>(it.value[0]), &info, px.data(), px.size());
            }
            t_counting = false;
            std::printf("F9: 1000 rounds of reading the row, the info and the pixels: %ld heap allocations\n", t_news);
            CHECK(t_news == 0);
        }
        // A NEW PICTURE for a node that has one replaces it: the same bounded cost, and one row still.
        const long again = costOf(decA, 8, 6, CASCADE_IMAGE_RGB24, 0x33);
        CHECK(again > 0 && again < 40);
        CHECK(liveRows() == 2);
        CHECK(Access::fingerprint(e9) == listFp);
        // ...and a node gone takes its picture: one bounded rebuild, then settled again.
        CHECK(g.removeNode(decB));
        (void)Access::publishLists(e9);  // (the graph's version moves; the pictures follow it)
        (void)Access::publishLists(e9);
        CHECK(liveRows() == 1);
        t_news = 0;
        t_counting = true;
        for (int i = 0; i < kPublishes; ++i) { (void)Access::publishLists(e9); }
        t_counting = false;
        std::printf("F9: after a node's removal, %d settled publishes: %ld heap allocations\n", kPublishes, t_news);
        CHECK(t_news == 0);
        table->close_session(session);
    }

// F9 CASE END>>
    return testSummary("test_engine_publish_alloc");
}
