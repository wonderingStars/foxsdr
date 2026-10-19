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
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <vector>
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
}  // namespace

void* operator new(std::size_t n) {
    if (t_counting) { ++t_news; }
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

    return testSummary("test_engine_publish_alloc");
}
