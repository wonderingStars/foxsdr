// test_receiver_snapshot.cpp - the one receiver snapshot of engine stage 2
// (core/receiver_snapshot.hpp, net/status_compose.hpp, docs/engine-stage2.md).
//
//   1. TORN READS. A writer stores as fast as it can while readers read; every
//      value a reader gets must be ONE write. Raced on SeqlockBox itself (a
//      512-byte value whose words all carry the same number) and on the
//      snapshot (a PublishedState whose far-apart fields carry the same
//      number, and the whole block, whose lists must be the same publish's).
//      Goes red when SeqlockBox::load's sequence re-check is removed.
//   2. THE WRITER NEVER WAITS, AND NO BLOCK IS STRANDED (review M2). With the
//      swap lock held by a reader, publish() returns at once and hands the
//      block over: the next readFull() installs it with no further publish
//      or retry (a stopped frame loop). With the hand-over lock held too, the
//      writer keeps it and retryInstall() - the next pass, nothing changed -
//      lands it. Goes red when publish() locks instead of try-locking, when
//      the hand-over is removed, and when the retry does nothing.
//   3. THE COUNTERS. Each group moves on exactly its fields; the level-1 ABI's
//      counters keep their own groups; measurements move nothing.
//   4. THE COMPOSE MAPPING, field by field: every RadioStatus member a web or
//      CAT reader is given, from a state and lists in which every field is
//      distinct - and a scan of net/web_server.hpp that fails when RadioStatus
//      gains a member with no row here.
//   5. THE PLUGIN API READS THE SAME OBJECT.
//   6. THE COST: publish, read and readFull+compose, in ns (reported; a
//      generous ceiling is checked so a pathological regression fails).
//   7. WALKING ONE (review M1): each flag alone composes exactly its own
//      RadioStatus boolean (every bit, assigned or not; every extension
//      boolean; the list checked against the header), each flag SOURCE alone
//      sets exactly its own flag (core::receiverFlags), and each flag alone
//      gives exactly its level-1 plugin flag.
//   8. A busy try-locked sink name moves no counter (review L6); the bookmark
//      ids ride in the block with their rows (review L3).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <regex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/plugin_api.hpp"
#include "core/receiver_snapshot.hpp"
#include "core/seqlock_box.hpp"
#include "net/status_compose.hpp"
#include "net/web_server.hpp"
#include "test_check.hpp"

using cascade::core::PublishedState;
using cascade::core::ReceiverSnapshot;
using cascade::net::RadioStatus;

namespace {

using Clock = std::chrono::steady_clock;

// --- 1. torn reads ---------------------------------------------------------------

struct Words {
    std::uint64_t w[64];
};

void seqlockNeverTears() {
    std::printf("[1a] SeqlockBox: a writer and three readers, 1.5 s\n");
    cascade::core::SeqlockBox<Words> box;
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> writes{0};
    std::thread writer([&] {
        Words v{};
        std::uint64_t n = 1;
        while (!stop.load(std::memory_order_relaxed)) {
            for (std::uint64_t& x : v.w) { x = n; }
            box.store(v);
            ++n;
        }
        writes.store(n);
    });
    std::atomic<std::uint64_t> reads{0}, torn{0}, busy{0}, moving{0};
    std::vector<std::thread> readers;
    for (int r = 0; r < 3; ++r) {
        readers.emplace_back([&] {
            std::uint64_t last = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                Words v;
                if (!box.load(v)) {
                    ++busy;
                    continue;
                }
                ++reads;
                bool same = true;
                for (const std::uint64_t x : v.w) { same = same && x == v.w[0]; }
                if (!same) { ++torn; }
                if (v.w[0] != last) { ++moving; }
                last = v.w[0];
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    stop.store(true);
    writer.join();
    for (std::thread& t : readers) { t.join(); }
    std::printf("      writes %llu, reads %llu, torn %llu, busy %llu, saw it move %llu times\n",
                static_cast<unsigned long long>(writes.load()),
                static_cast<unsigned long long>(reads.load()),
                static_cast<unsigned long long>(torn.load()),
                static_cast<unsigned long long>(busy.load()),
                static_cast<unsigned long long>(moving.load()));
    CHECK(torn.load() == 0u);
    // The race was real: the writer wrote throughout and the readers saw it.
    CHECK(writes.load() > 10000u);
    CHECK(reads.load() > 10000u);
    CHECK(moving.load() > 1000u);
}

void stamp(PublishedState& s, std::uint64_t n) {
    const double d = static_cast<double>(n);
    // Fields at the start, the middle and the end of the struct.
    s.rx.centreHz = d;
    s.rx.volume = d;
    s.rx.txPowerDb = d;
    s.rx.audioUnderruns = n;
    std::snprintf(s.rx.txUnkeyReason, sizeof(s.rx.txUnkeyReason), "%llu",
                  static_cast<unsigned long long>(n));
    s.app.gains[0].currentDb = d;
    s.app.gains[cascade::core::kMaxPublishedGains - 1].currentDb = d;
    s.app.rates[cascade::core::kMaxPublishedRates - 1] = d;
    s.app.basemapTileSize = static_cast<std::uint32_t>(n);
}

bool stampedWhole(const PublishedState& s) {
    const double d = s.rx.centreHz;
    const std::uint64_t n = static_cast<std::uint64_t>(d);
    char want[32];
    std::snprintf(want, sizeof(want), "%llu", static_cast<unsigned long long>(n));
    return s.rx.volume == d && s.rx.txPowerDb == d && s.rx.audioUnderruns == n &&
           std::strncmp(s.rx.txUnkeyReason, want, sizeof(want)) == 0 &&
           s.app.gains[0].currentDb == d &&
           s.app.gains[cascade::core::kMaxPublishedGains - 1].currentDb == d &&
           s.app.rates[cascade::core::kMaxPublishedRates - 1] == d &&
           s.app.basemapTileSize == static_cast<std::uint32_t>(n);
}

void snapshotNeverTears() {
    std::printf("[1b] ReceiverSnapshot: read() and readFull() against a publishing writer, 1.5 s\n");
    ReceiverSnapshot snap;
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> publishes{0};
    std::thread writer([&] {
        std::uint64_t n = 1;
        PublishedState s{};
        while (!stop.load(std::memory_order_relaxed)) {
            stamp(s, n);
            auto lists = std::make_shared<RadioStatus>();
            lists->sourceName = std::to_string(n);
            snap.publish(s, std::move(lists));
            ++n;
        }
        publishes.store(n);
    });
    std::atomic<std::uint64_t> reads{0}, torn{0}, busy{0}, fulls{0}, mismatched{0};
    std::vector<std::thread> readers;
    for (int r = 0; r < 2; ++r) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                PublishedState s;
                if (!snap.read(s)) {
                    ++busy;
                    continue;
                }
                ++reads;
                if (s.app.published && !stampedWhole(s)) { ++torn; }
            }
        });
    }
    readers.emplace_back([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            const std::shared_ptr<const ReceiverSnapshot::Full> f = snap.readFull();
            ++fulls;
            if (!f->state.app.published) { continue; }
            // THE STATE AND THE LISTS OF ONE PUBLISH, never two.
            const bool same = f->lists != nullptr &&
                              f->lists->sourceName ==
                                  std::to_string(static_cast<std::uint64_t>(f->state.rx.centreHz));
            if (!stampedWhole(f->state) || !same) { ++mismatched; }
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    stop.store(true);
    writer.join();
    for (std::thread& t : readers) { t.join(); }
    std::printf("      publishes %llu, reads %llu (torn %llu, busy %llu), whole blocks %llu (mismatched %llu), "
                "deferred installs %llu\n",
                static_cast<unsigned long long>(publishes.load()),
                static_cast<unsigned long long>(reads.load()),
                static_cast<unsigned long long>(torn.load()),
                static_cast<unsigned long long>(busy.load()),
                static_cast<unsigned long long>(fulls.load()),
                static_cast<unsigned long long>(mismatched.load()),
                static_cast<unsigned long long>(snap.deferredInstalls()));
    CHECK(torn.load() == 0u);
    CHECK(mismatched.load() == 0u);
    CHECK(publishes.load() > 10000u);
    CHECK(reads.load() > 10000u);
    CHECK(fulls.load() > 10000u);
}

// --- 2. the writer never waits ---------------------------------------------------------

// Runs `f` on another thread; true when it returned within 2 s. A writer
// that waited for a held lock would not.
bool returnsWhileHeld(const std::function<void()>& f) {
    std::future<void> fut = std::async(std::launch::async, f);
    const bool returned = fut.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    return returned;  // (fut's destructor waits: if it blocked, it finishes once the caller unlocks)
}

void writerNeverWaits() {
    std::printf("[2] the writer never waits, and a block it could not install still lands\n");
    ReceiverSnapshot snap;
    PublishedState s{};
    stamp(s, 1);
    snap.publish(s, std::make_shared<RadioStatus>());
    CHECK(snap.readFull()->state.rx.centreHz == 1.0);

    // (a) A reader holds the swap lock: the block is HANDED OVER, and the
    // next readFull() installs it - with no further publish and no retry, as
    // when the frame loop stops (the Windows move/resize loop).
    {
        const std::uint64_t deferredBefore = snap.deferredInstalls();
        const std::uint64_t heldBefore = snap.heldBackInstalls();
        std::unique_lock<std::mutex> held = snap.holdSwapLockForTest();
        stamp(s, 2);
        bool returned = false;
        {
            std::future<void> fut = std::async(std::launch::async, [&] { snap.publish(s, std::make_shared<RadioStatus>()); });
            returned = fut.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
            if (!returned) { held.unlock(); }
        }
        std::printf("      (a) publish returned while the swap lock was held: %s\n", returned ? "yes" : "NO");
        CHECK(returned);
        if (held.owns_lock()) { held.unlock(); }
        CHECK(snap.deferredInstalls() == deferredBefore + 1u);
        CHECK(snap.heldBackInstalls() == heldBefore);
        CHECK(!snap.installPending());
        PublishedState r;
        CHECK(snap.read(r));
        CHECK(r.rx.centreHz == 2.0);
        const double landed = snap.readFull()->state.rx.centreHz;
        std::printf("      (a) the next readFull, no publish, no retry: centre %.0f (published 2)\n", landed);
        CHECK(landed == 2.0);
    }

    // (b) A reader is inside the hand-over too (both locks held): the writer
    // keeps the block, never waits, and retryInstall() - the writer's next
    // pass, with NOTHING changed - lands it.
    {
        std::unique_lock<std::mutex> held = snap.holdSwapLockForTest();
        std::unique_lock<std::mutex> heldHandoff = snap.holdHandoffLockForTest();
        stamp(s, 3);
        bool returned = false;
        {
            std::future<void> fut = std::async(std::launch::async, [&] { snap.publish(s, std::make_shared<RadioStatus>()); });
            returned = fut.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
            if (!returned) {
                heldHandoff.unlock();
                held.unlock();
            }
        }
        std::printf("      (b) publish returned with both locks held: %s\n", returned ? "yes" : "NO");
        CHECK(returned);
        CHECK(snap.installPending());
        bool retryResult = true;
        const bool retryReturned = returnsWhileHeld([&] { retryResult = snap.retryInstall(); });
        std::printf("      (b) retryInstall returned with both locks held: %s (landed: %s)\n",
                    retryReturned ? "yes" : "NO", retryResult ? "yes" : "no");
        CHECK(retryReturned);
        CHECK(!retryResult);
        if (heldHandoff.owns_lock()) { heldHandoff.unlock(); }
        if (held.owns_lock()) { held.unlock(); }
        // Released: the web block is still the previous publish...
        CHECK(snap.readFull()->state.rx.centreHz == 2.0);
        // ...until the writer's next pass, with no state change at all.
        CHECK(snap.retryInstall());
        CHECK(!snap.installPending());
        const double landed = snap.readFull()->state.rx.centreHz;
        std::printf("      (b) after retryInstall, no change published: centre %.0f (published 3)\n", landed);
        CHECK(landed == 3.0);
        CHECK(snap.retryInstall());  // nothing pending: a no-op
    }

    // (c) A newer publish replaces a held-back block; an older handed-over
    // block never overwrites a newer installed one.
    {
        std::unique_lock<std::mutex> held = snap.holdSwapLockForTest();
        std::unique_lock<std::mutex> heldHandoff = snap.holdHandoffLockForTest();
        stamp(s, 4);
        snap.publish(s, std::make_shared<RadioStatus>());  // held back (both locks held here)
        heldHandoff.unlock();
        stamp(s, 5);
        snap.publish(s, std::make_shared<RadioStatus>());  // swap lock still held: handed over
        held.unlock();
        CHECK(!snap.installPending());
        CHECK(snap.readFull()->state.rx.centreHz == 5.0);
        stamp(s, 6);
        snap.publish(s, std::make_shared<RadioStatus>());
        CHECK(snap.readFull()->state.rx.centreHz == 6.0);
    }
}

// --- 3. the counters -----------------------------------------------------------------

struct Seqs {
    std::uint64_t seq, tune, mode, device, audio, display, tx, list;
    std::uint64_t aSeq, aTune, aMode, aDevice, aAudio;
};

Seqs seqs(const ReceiverSnapshot& snap) {
    PublishedState s;
    const bool ok = snap.read(s);
    CHECK(ok);
    return {s.rx.seq,       s.rx.tuneSeq,      s.rx.modeSeq,      s.rx.deviceSeq,   s.rx.audioSeq,
            s.rx.displaySeq, s.rx.txSeq,       s.rx.listSeq,      s.app.abiSeq,     s.app.abiTuneSeq,
            s.app.abiModeSeq, s.app.abiDeviceSeq, s.app.abiAudioSeq};
}

// Which counters moved from `a` to `b`, as a word per counter.
std::string moved(const Seqs& a, const Seqs& b) {
    std::string o;
    const auto add = [&o](bool m, const char* w) {
        if (m) { o += o.empty() ? w : std::string(" ") + w; }
    };
    add(b.seq != a.seq, "seq");
    add(b.tune != a.tune, "tune");
    add(b.mode != a.mode, "mode");
    add(b.device != a.device, "device");
    add(b.audio != a.audio, "audio");
    add(b.display != a.display, "display");
    add(b.tx != a.tx, "tx");
    add(b.list != a.list, "list");
    add(b.aSeq != a.aSeq, "abiSeq");
    add(b.aTune != a.aTune, "abiTune");
    add(b.aMode != a.aMode, "abiMode");
    add(b.aDevice != a.aDevice, "abiDevice");
    add(b.aAudio != a.aAudio, "abiAudio");
    return o;
}

void countersMoveByGroup() {
    std::printf("[3] counters\n");
    ReceiverSnapshot snap;
    {
        PublishedState s;
        CHECK(snap.read(s));
        CHECK(!s.app.published);
        CHECK(s.rx.seq == 0u && s.app.abiSeq == 0u);
        CHECK(s.rx.signalDb == -200.0);
        CHECK(s.rx.demodMode == FOXAPI_DEMOD_NFM);
        CHECK(snap.readFull()->lists == nullptr);
    }
    PublishedState s{};
    s.rx.demodMode = FOXAPI_DEMOD_WFM;
    snap.publish(s, nullptr);
    Seqs a = seqs(snap);
    CHECK(a.seq == 1u && a.tune == 1u && a.mode == 1u && a.device == 1u && a.audio == 1u &&
          a.display == 1u && a.tx == 1u && a.list == 1u);
    CHECK(a.aSeq == 1u && a.aTune == 1u && a.aMode == 1u && a.aDevice == 1u && a.aAudio == 1u);
    {
        PublishedState r;
        CHECK(snap.read(r));
        CHECK(r.app.published);
        CHECK(r.rx.structSize == sizeof(FoxReceiverState));
    }

    struct Step {
        const char* what;
        void (*change)(PublishedState&);
        const char* expect;
    };
    const Step steps[] = {
        {"nothing", [](PublishedState&) {}, ""},
        {"signal (a measurement)", [](PublishedState& p) { p.rx.signalDb = -40.0; p.rx.sMeter = 0.6; }, ""},
        {"underruns, hold timer, pilot, frames (measurements)",
         [](PublishedState& p) {
             p.rx.audioUnderruns = 9;
             p.rx.txHoldRemainingMs = 1500;
             p.rx.pilotLevel = 0.3;
             p.rx.audioLevelDb = -12.0;
             p.app.outputFrames = 480000;
         },
         ""},
        {"centre", [](PublishedState& p) { p.rx.centreHz = 145.0e6; }, "seq tune abiSeq abiTune"},
        {"vfo offset", [](PublishedState& p) { p.rx.vfoOffsetHz = 12.5e3; }, "seq tune abiSeq abiTune"},
        {"mode", [](PublishedState& p) { p.rx.demodMode = FOXAPI_DEMOD_NFM; }, "seq mode abiSeq abiMode"},
        {"bandwidth", [](PublishedState& p) { p.rx.bandwidthHz = 9000.0; }, "seq mode abiSeq abiMode"},
        {"squelch", [](PublishedState& p) { p.rx.squelchDb = -60.0; }, "seq mode abiSeq abiMode"},
        {"noise reduction (audio DSP: not the ABI's mode group)",
         [](PublishedState& p) { p.rx.flags |= FOXAPI_RX_NR; }, "seq mode"},
        {"notch frequency", [](PublishedState& p) { p.rx.notchHz = 1000.0; }, "seq mode"},
        {"de-emphasis", [](PublishedState& p) { p.rx.deemphasis = 1; }, "seq mode"},
        {"volume", [](PublishedState& p) { p.rx.volume = 0.5; }, "seq audio abiSeq abiAudio"},
        {"mute", [](PublishedState& p) { p.rx.flags |= FOXAPI_RX_MUTED; }, "seq audio abiSeq abiAudio"},
        {"sink name", [](PublishedState& p) { std::snprintf(p.rx.sinkName, sizeof(p.rx.sinkName), "%s", "Speakers"); },
         "seq audio"},
        {"display range", [](PublishedState& p) { p.rx.dbMin = -120.0; }, "seq display"},
        {"running", [](PublishedState& p) { p.rx.flags |= FOXAPI_RX_RUNNING; }, "seq device abiSeq abiDevice"},
        {"rate", [](PublishedState& p) { p.rx.sampleRateHz = 2.048e6; }, "seq device abiSeq abiDevice"},
        {"channel rate (not in the ABI)", [](PublishedState& p) { p.rx.channelRateHz = 48.0e3; }, "seq device"},
        {"device name", [](PublishedState& p) { std::snprintf(p.rx.deviceName, sizeof(p.rx.deviceName), "%s", "RTL"); },
         "seq device abiSeq abiDevice"},
        {"a gain stage", [](PublishedState& p) { p.app.abiGainCount = 1; p.app.gains[0].currentDb = 20.0; },
         "seq device abiSeq abiDevice"},
        {"a rate in the list", [](PublishedState& p) { p.app.rateCount = 1; p.app.rates[0] = 2.4e6; },
         "seq device abiSeq abiDevice"},
        {"stereo lamp", [](PublishedState& p) { p.rx.flags |= FOXAPI_RX_STEREO_ACTIVE; }, "seq abiSeq"},
        {"recording", [](PublishedState& p) { p.rx.flags |= FOXAPI_RX_RECORDING_IQ; }, "seq"},
        {"decoders", [](PublishedState& p) { p.rx.decodersFitted = 3; }, "seq"},
        {"key", [](PublishedState& p) { p.rx.flags |= FOXAPI_RX_TX_KEYED; }, "seq tx"},
        {"tx power", [](PublishedState& p) { p.rx.txPowerDb = -10.0; }, "seq tx"},
        {"an extension figure (position)", [](PublishedState& p) { p.app.rxLatDeg = 53.8; }, ""},
    };
    for (const Step& st : steps) {
        const Seqs before = seqs(snap);
        st.change(s);
        snap.publish(s, nullptr);
        const std::string m = moved(before, seqs(snap));
        const bool ok = m == st.expect;
        std::printf("      %-52s -> [%s]%s\n", st.what, m.c_str(), ok ? "" : "   EXPECTED DIFFERENT");
        CHECK(ok);
    }
    // No counter ever goes backwards, and each moves by one per publish.
    const Seqs end = seqs(snap);
    CHECK(end.list == 1u);
}

// --- 4. the compose mapping --------------------------------------------------------

PublishedState everyFieldDistinct() {
    PublishedState s{};
    FoxReceiverState& r = s.rx;
    cascade::core::AppStateExt& e = s.app;
    e.published = true;
    r.flags = FOXAPI_RX_RUNNING | FOXAPI_RX_FAULTED | FOXAPI_RX_STEREO_ACTIVE | FOXAPI_RX_TX_KEYED |
              FOXAPI_RX_TX_REMOTE_ARMED | FOXAPI_RX_NR | FOXAPI_RX_NOTCH | FOXAPI_RX_AUTO_NOTCH |
              FOXAPI_RX_STEREO_ENABLED | FOXAPI_RX_AGC_SUPPORTED | FOXAPI_RX_DEVICE_AGC |
              FOXAPI_RX_RECORDING_IQ | FOXAPI_RX_RECORDING_AUDIO | FOXAPI_RX_SCANNER_ACTIVE;
    r.centreHz = 101.1e6;
    r.sampleRateHz = 2.4e6;
    r.vfoOffsetHz = -12.5e3;
    r.bandwidthHz = 11.0e3;
    r.demodMode = FOXAPI_DEMOD_USB;
    r.signalDb = -37.25;
    r.txHoldRemainingMs = 1234;
    r.squelchDb = -61.5;
    r.volume = 0.375;
    r.dbMin = -117.0;
    r.dbMax = -7.5;
    r.deemphasis = 2;
    r.nrStrength = 0.625;
    r.notchHz = 1111.0;
    r.notchQ = 22.0;
    r.audioUnderruns = 77;
    e.autoNotchEngaged = true;
    e.autoNotchFreqHz = 987.0;
    e.pilotLocked = true;
    e.rdsSynced = true;
    e.rdsPiValid = true;
    e.rdsPi = 0xC201;
    e.rdsPsValid = true;
    e.rdsPty = 10;
    e.rdsTp = true;
    e.rdsTa = true;
    e.rdsGroups = 4321;
    e.rdsErrors = 12;
    e.sourceBusy = true;
    e.audioPrimingCallbacks = 88;
    e.audioRingMs = 45.5;
    e.audioRingCapacityMs = 250.0;
    e.audioPluginGaps = 3;
    e.audioPluginGapFrames = 960;
    e.iqBytes = 123456789;
    e.audioBytes = 98765;
    e.scannerState = cascade::core::kScannerHolding;
    e.scanStartHz = 144.0e6;
    e.scanStopHz = 146.0e6;
    e.scanStepHz = 12.5e3;
    e.rxPositionSet = true;
    e.rxLatDeg = 53.8;
    e.rxLonDeg = -1.55;
    e.catalogueBusy = true;
    e.basemapActive = true;
    e.basemapMinZoom = 2;
    e.basemapMaxZoom = 17;
    e.basemapTileSize = 512;
    return s;
}

RadioStatus everyListDistinct() {
    RadioStatus l;
    // Scalars set to values compose must OVERWRITE: a scalar compose forgets
    // keeps one of these and fails its row below.
    l.running = false;
    l.centerHz = -1.0;
    l.mode = "WRONG";
    l.signalDb = 99.0f;
    l.scannerState = "WRONG";
    l.basemap.maxZoom = 1;
    l.transmitAvailable = false;
    l.rdsPi = 1;
    // The text and the lists.
    l.faultMessage = "fault";
    l.sourceName = "Source";
    l.tunerDisplayStyle = "plain";
    l.rdsPs = "BBC R2";
    l.rdsRadioText = "radio text";
    l.sourceKind = "rtlsdr";
    l.soapyArgs = "serial=1";
    l.antenna = "RX";
    l.antennas = {"RX", "TX/RX"};
    l.devices = {{"Dev", "serial=1", "rtlsdr"}};
    l.gains = {{"LNA", 20.0, "dB"}};
    l.sourceError = "source error";
    l.audioMutedBy = "Decoder";
    l.audioSource = "DAB";
    l.recordDir = "/rec";
    l.recordError = "record error";
    l.recordNotice = "record notice";
    l.bookmarks = {{"Mark", 145.5e6, "NFM", 12.5e3}};
    l.decoded = {{"P", "line"}};
    l.tracks.resize(1);
    l.tracks[0].id = "ABC";
    l.plugins.resize(1);
    l.plugins[0].name = "Plug";
    l.catalogue.resize(1);
    l.catalogue[0].id = "cat";
    l.catalogueStatus = "catalogue status";
    l.catalogueError = "catalogue error";
    l.installReport = "install report";
    l.installError = "install error";
    l.basemap.attribution = "(c) OSM";
    l.images = {{"Img", 10, 20, true, 5}};
    return l;
}

// Every top-level member of RadioStatus, as net/web_server.hpp declares it,
// with the row below that checks it. A member missing here fails the scan.
const char* const kRows[] = {
    "running", "faulted", "faultMessage", "centerHz", "sampleRateHz", "vfoOffsetHz", "bandwidthHz",
    "mode", "sourceName", "signalDb", "stereoActive", "transmitting", "transmitAvailable",
    "transmitRemoteHoldMs", "squelchDb", "volume", "dbMin", "dbMax", "tunerDisplayStyle",
    "deemphasisIndex", "nrEnabled", "nrStrength", "notchEnabled", "notchFreqHz", "notchQ", "autoNotch",
    "autoNotchEngaged", "autoNotchFreqHz", "stereoEnabled", "pilotLocked", "rdsSynced", "rdsPiValid",
    "rdsPi", "rdsPsValid", "rdsPs", "rdsRadioText", "rdsPty", "rdsTp", "rdsTa", "rdsGroups",
    "rdsErrors", "sourceKind", "soapyArgs", "antenna", "antennas", "devices", "gains", "agcSupported",
    "agc", "sourceBusy", "sourceError", "iqRecording", "audioMutedBy", "audioUnderruns",
    "audioPrimingCallbacks", "audioRingMs", "audioRingCapacityMs", "audioSource", "audioPluginGaps",
    "audioPluginGapFrames", "audioRecording", "iqBytes", "audioBytes", "recordDir", "recordError",
    "recordNotice", "bookmarks", "scannerActive", "scannerState", "scanStartHz", "scanStopHz",
    "scanStepHz", "decoded", "tracks", "rxPositionSet", "rxLatDeg", "rxLonDeg", "plugins",
    "catalogue", "catalogueStatus", "catalogueError", "catalogueBusy", "installReport",
    "installError", "basemap", "images",
};

std::set<std::string> radioStatusMembers() {
    const std::filesystem::path p = std::filesystem::path(CASCADE_SOURCE_DIR) / "src" / "net" / "web_server.hpp";
    std::ifstream f(p);
    std::set<std::string> names;
    std::string line;
    bool in = false;
    int depth = 0;
    const std::regex member(R"(^\s*[A-Za-z_][\w:<>, ]*[\s&*]+([A-Za-z_]\w*)\s*(=[^;]*)?;)");
    while (std::getline(f, line)) {
        if (!in) {
            if (line.rfind("struct RadioStatus {", 0) == 0) {
                in = true;
                depth = 1;
            }
            continue;
        }
        const std::string code = line.substr(0, line.find("//"));
        if (depth == 1) {
            std::smatch m;
            if (std::regex_search(code, m, member)) { names.insert(m[1].str()); }
        }
        for (const char c : code) {
            if (c == '{') { ++depth; }
            if (c == '}') { --depth; }
        }
        if (depth == 0) { break; }
    }
    return names;
}

void composeMapsEveryField() {
    std::printf("[4] compose: every RadioStatus member, from distinct fields\n");
    const PublishedState s = everyFieldDistinct();
    const RadioStatus l = everyListDistinct();
    const RadioStatus o = cascade::net::composeRadioStatus(s, &l);
    int rows = 0;
#define ROW(cond)          \
    do {                   \
        ++rows;            \
        CHECK(cond);       \
    } while (0)
    ROW(o.running);
    ROW(o.faulted);
    ROW(o.faultMessage == "fault");
    ROW(o.centerHz == 101.1e6);
    ROW(o.sampleRateHz == 2.4e6);
    ROW(o.vfoOffsetHz == -12.5e3);
    ROW(o.bandwidthHz == 11.0e3);
    ROW(o.mode == "USB");
    ROW(o.sourceName == "Source");
    ROW(o.signalDb == -37.25f);
    ROW(o.stereoActive);
    ROW(o.transmitting);
    ROW(o.transmitAvailable);
    ROW(o.transmitRemoteHoldMs == 1234);
    ROW(o.squelchDb == -61.5f);
    ROW(o.volume == 0.375f);
    ROW(o.dbMin == -117.0f);
    ROW(o.dbMax == -7.5f);
    ROW(o.tunerDisplayStyle == "plain");
    ROW(o.deemphasisIndex == 2);
    ROW(o.nrEnabled);
    ROW(o.nrStrength == 0.625f);
    ROW(o.notchEnabled);
    ROW(o.notchFreqHz == 1111.0);
    ROW(o.notchQ == 22.0);
    ROW(o.autoNotch);
    ROW(o.autoNotchEngaged);
    ROW(o.autoNotchFreqHz == 987.0);
    ROW(o.stereoEnabled);
    ROW(o.pilotLocked);
    ROW(o.rdsSynced);
    ROW(o.rdsPiValid);
    ROW(o.rdsPi == 0xC201u);
    ROW(o.rdsPsValid);
    ROW(o.rdsPs == "BBC R2");
    ROW(o.rdsRadioText == "radio text");
    ROW(o.rdsPty == 10u);
    ROW(o.rdsTp);
    ROW(o.rdsTa);
    ROW(o.rdsGroups == 4321u);
    ROW(o.rdsErrors == 12u);
    ROW(o.sourceKind == "rtlsdr");
    ROW(o.soapyArgs == "serial=1");
    ROW(o.antenna == "RX");
    ROW(o.antennas.size() == 2u && o.antennas[1] == "TX/RX");
    ROW(o.devices.size() == 1u && o.devices[0].kind == "rtlsdr");
    ROW(o.gains.size() == 1u && o.gains[0].db == 20.0);
    ROW(o.agcSupported);
    ROW(o.agc);
    ROW(o.sourceBusy);
    ROW(o.sourceError == "source error");
    ROW(o.iqRecording);
    ROW(o.audioMutedBy == "Decoder");
    ROW(o.audioUnderruns == 77u);
    ROW(o.audioPrimingCallbacks == 88u);
    ROW(o.audioRingMs == 45.5);
    ROW(o.audioRingCapacityMs == 250.0);
    ROW(o.audioSource == "DAB");
    ROW(o.audioPluginGaps == 3u);
    ROW(o.audioPluginGapFrames == 960u);
    ROW(o.audioRecording);
    ROW(o.iqBytes == 123456789u);
    ROW(o.audioBytes == 98765u);
    ROW(o.recordDir == "/rec");
    ROW(o.recordError == "record error");
    ROW(o.recordNotice == "record notice");
    ROW(o.bookmarks.size() == 1u && o.bookmarks[0].name == "Mark");
    ROW(o.scannerActive);
    ROW(o.scannerState == "holding");
    ROW(o.scanStartHz == 144.0e6);
    ROW(o.scanStopHz == 146.0e6);
    ROW(o.scanStepHz == 12.5e3);
    ROW(o.decoded.size() == 1u && o.decoded[0].text == "line");
    ROW(o.tracks.size() == 1u && o.tracks[0].id == "ABC");
    ROW(o.rxPositionSet);
    ROW(o.rxLatDeg == 53.8);
    ROW(o.rxLonDeg == -1.55);
    ROW(o.plugins.size() == 1u && o.plugins[0].name == "Plug");
    ROW(o.catalogue.size() == 1u && o.catalogue[0].id == "cat");
    ROW(o.catalogueStatus == "catalogue status");
    ROW(o.catalogueError == "catalogue error");
    ROW(o.catalogueBusy);
    ROW(o.installReport == "install report");
    ROW(o.installError == "install error");
    ROW(o.basemap.active && o.basemap.attribution == "(c) OSM" && o.basemap.minZoom == 2u &&
        o.basemap.maxZoom == 17u && o.basemap.tileSize == 512u);
    ROW(o.images.size() == 1u && o.images[0].revision == 5u);
#undef ROW

    // The same state with every flag OFF: each boolean follows its own bit.
    PublishedState off = s;
    off.rx.flags = 0;
    const RadioStatus z = cascade::net::composeRadioStatus(off, &l);
    CHECK(!z.running && !z.faulted && !z.stereoActive && !z.transmitting && !z.transmitAvailable &&
          !z.nrEnabled && !z.notchEnabled && !z.autoNotch && !z.stereoEnabled && !z.agcSupported &&
          !z.agc && !z.iqRecording && !z.audioRecording && !z.scannerActive);

    // The table above is every member: the header decides, not this file.
    const std::set<std::string> declared = radioStatusMembers();
    std::set<std::string> tabled(std::begin(kRows), std::end(kRows));
    std::printf("      %d rows; RadioStatus declares %zu members; the table names %zu\n", rows,
                declared.size(), tabled.size());
    CHECK(!declared.empty());
    for (const std::string& n : declared) {
        if (tabled.count(n) == 0u) {
            std::printf("      FAIL: RadioStatus::%s has no row here (and maybe no mapping in compose)\n",
                        n.c_str());
            CHECK(false);
        }
    }
    for (const std::string& n : tabled) {
        if (declared.count(n) == 0u) {
            std::printf("      FAIL: the table names %s, which RadioStatus does not declare\n", n.c_str());
            CHECK(false);
        }
    }
    CHECK(static_cast<std::size_t>(rows) == tabled.size());

    // Before the first publish: exactly what a reader got then.
    const RadioStatus never = cascade::net::composeRadioStatus(cascade::core::initialPublishedState(), &l);
    const RadioStatus def;
    CHECK(never.sourceName.empty() && never.mode == def.mode && never.dbMin == def.dbMin &&
          never.tunerDisplayStyle == def.tunerDisplayStyle && never.sourceKind == def.sourceKind &&
          never.basemap.maxZoom == def.basemap.maxZoom && never.stereoEnabled == def.stereoEnabled);
    // CAT's form: no lists, the figures all there.
    const RadioStatus cat = cascade::net::composeRadioStatus(s, nullptr);
    CHECK(cat.centerHz == 101.1e6 && cat.mode == "USB" && cat.running && cat.sourceName.empty() &&
          cat.devices.empty());

    // The mode and scanner words.
    const char* const want[] = {"", "NFM", "WFM", "AM", "DSB", "USB", "CW", "LSB", "RAW", ""};
    for (std::uint32_t m = 0; m <= 9; ++m) { CHECK(std::string(cascade::net::demodName(m)) == want[m]); }
    CHECK(std::string(cascade::net::scannerStateName(cascade::core::kScannerIdle)) == "idle");
    CHECK(std::string(cascade::net::scannerStateName(cascade::core::kScannerScanning)) == "scanning");
    CHECK(std::string(cascade::net::scannerStateName(cascade::core::kScannerPaused)) == "paused");
    CHECK(std::string(cascade::net::scannerStateName(99)).empty());
}

// --- 7. walking one: every flag, every boolean, exactly its own -------------------------
//
// [4] sets every flag on, then every flag off - which a mapping that reads
// the WRONG flag passes, as long as both flags are in the set (the review's
// `transmitAvailable = flag(TX_KEYED)`). Here each source, flag or boolean is
// set ALONE, and exactly one thing may answer.

// Every RadioStatus boolean compose writes, by name, and how to read it.
struct BoolOut {
    const char* name;
    bool (*get)(const RadioStatus&);
};
const BoolOut kBools[] = {
    {"running", [](const RadioStatus& o) { return o.running; }},
    {"faulted", [](const RadioStatus& o) { return o.faulted; }},
    {"stereoActive", [](const RadioStatus& o) { return o.stereoActive; }},
    {"transmitting", [](const RadioStatus& o) { return o.transmitting; }},
    {"transmitAvailable", [](const RadioStatus& o) { return o.transmitAvailable; }},
    {"nrEnabled", [](const RadioStatus& o) { return o.nrEnabled; }},
    {"notchEnabled", [](const RadioStatus& o) { return o.notchEnabled; }},
    {"autoNotch", [](const RadioStatus& o) { return o.autoNotch; }},
    {"autoNotchEngaged", [](const RadioStatus& o) { return o.autoNotchEngaged; }},
    {"stereoEnabled", [](const RadioStatus& o) { return o.stereoEnabled; }},
    {"pilotLocked", [](const RadioStatus& o) { return o.pilotLocked; }},
    {"rdsSynced", [](const RadioStatus& o) { return o.rdsSynced; }},
    {"rdsPiValid", [](const RadioStatus& o) { return o.rdsPiValid; }},
    {"rdsPsValid", [](const RadioStatus& o) { return o.rdsPsValid; }},
    {"rdsTp", [](const RadioStatus& o) { return o.rdsTp; }},
    {"rdsTa", [](const RadioStatus& o) { return o.rdsTa; }},
    {"agcSupported", [](const RadioStatus& o) { return o.agcSupported; }},
    {"agc", [](const RadioStatus& o) { return o.agc; }},
    {"sourceBusy", [](const RadioStatus& o) { return o.sourceBusy; }},
    {"iqRecording", [](const RadioStatus& o) { return o.iqRecording; }},
    {"audioRecording", [](const RadioStatus& o) { return o.audioRecording; }},
    {"scannerActive", [](const RadioStatus& o) { return o.scannerActive; }},
    {"rxPositionSet", [](const RadioStatus& o) { return o.rxPositionSet; }},
    {"catalogueBusy", [](const RadioStatus& o) { return o.catalogueBusy; }},
    {"basemap.active", [](const RadioStatus& o) { return o.basemap.active; }},
};

// Which booleans a composed status has on, as "a b c".
std::string bools(const RadioStatus& o) {
    std::string s;
    for (const BoolOut& b : kBools) {
        if (b.get(o)) { s += s.empty() ? b.name : std::string(" ") + b.name; }
    }
    return s;
}

// A state that is published and otherwise all zero/false - and whose
// defaults compose would otherwise answer true (stereoEnabled's RadioStatus
// default is true) are overwritten by compose, not kept.
PublishedState bare() {
    PublishedState s{};
    s.app.published = true;
    return s;
}

// The depth-1 `bool` members of RadioStatus, from the header.
std::set<std::string> radioStatusBools() {
    const std::filesystem::path p = std::filesystem::path(CASCADE_SOURCE_DIR) / "src" / "net" / "web_server.hpp";
    std::ifstream f(p);
    std::set<std::string> names;
    std::string line;
    bool in = false;
    int depth = 0;
    const std::regex member(R"(^\s*bool\s+([A-Za-z_]\w*)\s*(=[^;]*)?;)");
    while (std::getline(f, line)) {
        if (!in) {
            if (line.rfind("struct RadioStatus {", 0) == 0) {
                in = true;
                depth = 1;
            }
            continue;
        }
        const std::string code = line.substr(0, line.find("//"));
        std::smatch m;
        if (depth == 1 && std::regex_search(code, m, member)) { names.insert(m[1].str()); }
        // The one nested boolean that is not a list item's.
        if (depth == 2 && code.find("bool active") != std::string::npos) { names.insert("basemap.active"); }
        for (const char c : code) {
            if (c == '{') { ++depth; }
            if (c == '}') { --depth; }
        }
        if (depth == 0) { break; }
    }
    return names;
}

void walkingOneCompose() {
    std::printf("[7a] compose: each flag alone -> exactly its own boolean\n");
    struct FlagWant {
        std::uint32_t bit;
        const char* name;
        const char* want;  // the booleans that must be on, and no others
    };
    const FlagWant flags[] = {
        {FOXAPI_RX_RUNNING, "RUNNING", "running"},
        {FOXAPI_RX_DEVICE_OPEN, "DEVICE_OPEN", ""},
        {FOXAPI_RX_FAULTED, "FAULTED", "faulted"},
        {FOXAPI_RX_MUTED, "MUTED", ""},
        {FOXAPI_RX_SQUELCH_OPEN, "SQUELCH_OPEN", ""},
        {FOXAPI_RX_STEREO_ENABLED, "STEREO_ENABLED", "stereoEnabled"},
        {FOXAPI_RX_STEREO_ACTIVE, "STEREO_ACTIVE", "stereoActive"},
        {FOXAPI_RX_NR, "NR", "nrEnabled"},
        {FOXAPI_RX_NOTCH, "NOTCH", "notchEnabled"},
        {FOXAPI_RX_AUTO_NOTCH, "AUTO_NOTCH", "autoNotch"},
        {FOXAPI_RX_DEVICE_AGC, "DEVICE_AGC", "agc"},
        {FOXAPI_RX_AGC_SUPPORTED, "AGC_SUPPORTED", "agcSupported"},
        {FOXAPI_RX_RECORDING_IQ, "RECORDING_IQ", "iqRecording"},
        {FOXAPI_RX_RECORDING_AUDIO, "RECORDING_AUDIO", "audioRecording"},
        {FOXAPI_RX_SCANNER_ACTIVE, "SCANNER_ACTIVE", "scannerActive"},
        {FOXAPI_RX_DECODER_ACTIVE, "DECODER_ACTIVE", ""},
        {FOXAPI_RX_TX_AVAILABLE, "TX_AVAILABLE", ""},
        {FOXAPI_RX_TX_KEYED, "TX_KEYED", "transmitting"},
        {FOXAPI_RX_TX_LATCHED, "TX_LATCHED", ""},
        {FOXAPI_RX_TX_KEY_MINE, "TX_KEY_MINE", ""},
        {FOXAPI_RX_SINK_OPEN, "SINK_OPEN", ""},
        {FOXAPI_RX_WEB_LISTENING, "WEB_LISTENING", ""},
        {FOXAPI_RX_TX_REMOTE_ARMED, "TX_REMOTE_ARMED", "transmitAvailable"},
        {FOXAPI_RX_TX_LATCH_RELEASE_FIRST, "TX_LATCH_RELEASE_FIRST", ""},
    };
    std::uint32_t named = 0;
    for (const FlagWant& fw : flags) { named |= fw.bit; }
    // Every bit, including the ones API 0.2 has not assigned: an unnamed bit
    // must turn nothing on.
    CHECK(bools(cascade::net::composeRadioStatus(bare(), nullptr)).empty());
    for (int b = 0; b < 32; ++b) {
        const std::uint32_t bit = 1u << b;
        const char* name = "(unassigned)";
        const char* want = "";
        for (const FlagWant& fw : flags) {
            if (fw.bit == bit) {
                name = fw.name;
                want = fw.want;
            }
        }
        PublishedState s = bare();
        s.rx.flags = bit;
        const std::string got = bools(cascade::net::composeRadioStatus(s, nullptr));
        const bool okay = got == want;
        if (!okay || (named & bit) != 0u) {
            std::printf("      %-22s -> [%s]%s\n", name, got.c_str(), okay ? "" : "   EXPECTED DIFFERENT");
        }
        if (!okay) { std::printf("        wanted [%s]\n", want); }
        CHECK(okay);
    }

    std::printf("[7b] compose: each extension boolean alone -> exactly its own\n");
    struct ExtWant {
        const char* name;
        void (*set)(cascade::core::AppStateExt&);
        const char* want;
    };
    const ExtWant exts[] = {
        {"autoNotchEngaged", [](cascade::core::AppStateExt& e) { e.autoNotchEngaged = true; }, "autoNotchEngaged"},
        {"pilotLocked", [](cascade::core::AppStateExt& e) { e.pilotLocked = true; }, "pilotLocked"},
        {"rdsSynced", [](cascade::core::AppStateExt& e) { e.rdsSynced = true; }, "rdsSynced"},
        {"rdsPiValid", [](cascade::core::AppStateExt& e) { e.rdsPiValid = true; }, "rdsPiValid"},
        {"rdsPsValid", [](cascade::core::AppStateExt& e) { e.rdsPsValid = true; }, "rdsPsValid"},
        {"rdsTp", [](cascade::core::AppStateExt& e) { e.rdsTp = true; }, "rdsTp"},
        {"rdsTa", [](cascade::core::AppStateExt& e) { e.rdsTa = true; }, "rdsTa"},
        {"sourceBusy", [](cascade::core::AppStateExt& e) { e.sourceBusy = true; }, "sourceBusy"},
        {"rxPositionSet", [](cascade::core::AppStateExt& e) { e.rxPositionSet = true; }, "rxPositionSet"},
        {"catalogueBusy", [](cascade::core::AppStateExt& e) { e.catalogueBusy = true; }, "catalogueBusy"},
        {"basemapActive", [](cascade::core::AppStateExt& e) { e.basemapActive = true; }, "basemap.active"},
    };
    for (const ExtWant& ew : exts) {
        PublishedState s = bare();
        ew.set(s.app);
        const std::string got = bools(cascade::net::composeRadioStatus(s, nullptr));
        const bool okay = got == ew.want;
        std::printf("      %-22s -> [%s]%s\n", ew.name, got.c_str(), okay ? "" : "   EXPECTED DIFFERENT");
        CHECK(okay);
    }

    // The list of booleans is every boolean RadioStatus declares.
    const std::set<std::string> declared = radioStatusBools();
    std::set<std::string> listed;
    for (const BoolOut& b : kBools) { listed.insert(b.name); }
    std::printf("      RadioStatus declares %zu booleans; this test reads %zu\n", declared.size(), listed.size());
    CHECK(declared == listed);
    for (const std::string& n : declared) {
        if (listed.count(n) == 0u) { std::printf("      FAIL: RadioStatus::%s is not walked here\n", n.c_str()); }
    }
}

void walkingOneSources() {
    std::printf("[7c] receiverFlags: each source condition alone -> exactly its own flag\n");
    using S = cascade::core::RxFlagSources;
    struct SrcWant {
        const char* name;
        bool S::*member;
        std::uint32_t want;
    };
    const SrcWant srcs[] = {
        {"running", &S::running, FOXAPI_RX_RUNNING},
        {"deviceOpen", &S::deviceOpen, FOXAPI_RX_DEVICE_OPEN},
        {"faulted", &S::faulted, FOXAPI_RX_FAULTED},
        {"muted", &S::muted, FOXAPI_RX_MUTED},
        {"squelchOpen", &S::squelchOpen, FOXAPI_RX_SQUELCH_OPEN},
        {"stereoEnabled", &S::stereoEnabled, FOXAPI_RX_STEREO_ENABLED},
        {"stereoActive", &S::stereoActive, FOXAPI_RX_STEREO_ACTIVE},
        {"nr", &S::nr, FOXAPI_RX_NR},
        {"notch", &S::notch, FOXAPI_RX_NOTCH},
        {"autoNotch", &S::autoNotch, FOXAPI_RX_AUTO_NOTCH},
        {"deviceAgc", &S::deviceAgc, FOXAPI_RX_DEVICE_AGC},
        {"agcSupported", &S::agcSupported, FOXAPI_RX_AGC_SUPPORTED},
        {"recordingIq", &S::recordingIq, FOXAPI_RX_RECORDING_IQ},
        {"recordingAudio", &S::recordingAudio, FOXAPI_RX_RECORDING_AUDIO},
        {"scannerActive", &S::scannerActive, FOXAPI_RX_SCANNER_ACTIVE},
        {"decoderActive", &S::decoderActive, FOXAPI_RX_DECODER_ACTIVE},
        {"txAvailable", &S::txAvailable, FOXAPI_RX_TX_AVAILABLE},
        {"txKeyed", &S::txKeyed, FOXAPI_RX_TX_KEYED},
        {"txLatched", &S::txLatched, FOXAPI_RX_TX_LATCHED},
        {"txRemoteArmed", &S::txRemoteArmed, FOXAPI_RX_TX_REMOTE_ARMED},
        {"sinkOpen", &S::sinkOpen, FOXAPI_RX_SINK_OPEN},
        {"webListening", &S::webListening, FOXAPI_RX_WEB_LISTENING},
    };
    // Every member of RxFlagSources is a bool and is walked here: the struct
    // holds nothing else, so its size counts them.
    CHECK(sizeof(S) == sizeof(srcs) / sizeof(srcs[0]) * sizeof(bool));
    CHECK(cascade::core::receiverFlags(S{}) == 0u);
    std::uint32_t seen = 0;
    for (const SrcWant& w : srcs) {
        S s{};
        s.*(w.member) = true;
        const std::uint32_t got = cascade::core::receiverFlags(s);
        const bool okay = got == w.want;
        if (!okay) { std::printf("      %-16s -> %08x, wanted %08x\n", w.name, got, w.want); }
        CHECK(okay);
        CHECK((seen & w.want) == 0u);  // no two sources share a flag
        seen |= w.want;
    }
    // Every flag but the two per-session ones has a source.
    CHECK(seen == (0x00FFFFFFu & ~(FOXAPI_RX_TX_KEY_MINE | FOXAPI_RX_TX_LATCH_RELEASE_FIRST)));
    std::printf("      %zu sources, each to its own flag; flags covered %08x\n",
                sizeof(srcs) / sizeof(srcs[0]), seen);
}

void walkingOnePluginFlags() {
    std::printf("[7d] get_state: each flag alone -> exactly its level-1 flag\n");
    auto snap = std::make_shared<ReceiverSnapshot>();
    cascade::core::PluginApiCore api(snap);
    cascade::core::PluginApiClient& c = api.client("walker.dll", "Walker");
    api.setLiveSet({"walker.dll"});
    struct Want {
        std::uint32_t bit;
        std::uint32_t want;
    };
    const Want map[] = {
        {FOXAPI_RX_RUNNING, CASCADE_STATE_RUNNING},       {FOXAPI_RX_DEVICE_OPEN, CASCADE_STATE_DEVICE_OPEN},
        {FOXAPI_RX_MUTED, CASCADE_STATE_MUTED},           {FOXAPI_RX_DEVICE_AGC, CASCADE_STATE_DEVICE_AGC},
        {FOXAPI_RX_AGC_SUPPORTED, CASCADE_STATE_AGC_SUPPORTED},
        {FOXAPI_RX_STEREO_ACTIVE, CASCADE_STATE_STEREO},
    };
    for (int b = 0; b < 32; ++b) {
        const std::uint32_t bit = 1u << b;
        std::uint32_t want = 0;
        for (const Want& w : map) {
            if (w.bit == bit) { want = w.want; }
        }
        PublishedState s{};
        s.rx.flags = bit;
        s.rx.signalDb = -200.0;  // below the squelch: SQUELCH_OPEN stays off
        snap->publish(s, nullptr);
        CascadeReceiverState st{};
        st.structSize = sizeof(st);
        CHECK(api.getState(c, &st) == CASCADE_API_OK);
        if (st.flags != want) { std::printf("      bit %08x -> %08x, wanted %08x\n", bit, st.flags, want); }
        CHECK(st.flags == want);
    }
    std::printf("      32 bits walked\n");
}

// --- 8. a busy try-locked name moves nothing; the ids travel with their rows --------

void stickyNamesAndIds() {
    std::printf("[8] a busy sink-name read keeps the last good name; bookmark ids ride in the block\n");
    std::string last;
    CHECK(cascade::core::keepLastGoodName(last, "") == "");
    CHECK(cascade::core::keepLastGoodName(last, "Speakers") == "Speakers");
    CHECK(cascade::core::keepLastGoodName(last, "") == "Speakers");  // the lock was busy
    CHECK(cascade::core::keepLastGoodName(last, "Headphones") == "Headphones");

    ReceiverSnapshot snap;
    PublishedState s{};
    std::string lastGood;
    std::snprintf(s.rx.sinkName, sizeof(s.rx.sinkName), "%s",
                  cascade::core::keepLastGoodName(lastGood, "Speakers").c_str());
    snap.publish(s, nullptr);
    const Seqs a = seqs(snap);
    // The next frame's try-lock is busy: "" comes back.
    std::snprintf(s.rx.sinkName, sizeof(s.rx.sinkName), "%s",
                  cascade::core::keepLastGoodName(lastGood, "").c_str());
    snap.publish(s, nullptr);
    const std::string m = moved(a, seqs(snap));
    std::printf("      a busy read -> counters moved: [%s]\n", m.c_str());
    CHECK(m.empty());

    auto lists = std::make_shared<RadioStatus>();
    lists->bookmarks = {{"One", 1.0e8, "NFM", 1.0e4}, {"Two", 2.0e8, "AM", 1.0e4}};
    snap.publish(s, lists, {41u, 42u});
    const auto full = snap.readFull();
    CHECK(full->lists != nullptr && full->lists->bookmarks.size() == 2u);
    CHECK(full->bookmarkIds.size() == 2u && full->bookmarkIds[0] == 41u && full->bookmarkIds[1] == 42u);
}

// --- 5. the plugin API reads the same object ------------------------------------------

void pluginApiReadsTheSharedSnapshot() {
    std::printf("[5] PluginApiCore answers from the snapshot it was given\n");
    auto snap = std::make_shared<ReceiverSnapshot>();
    cascade::core::PluginApiCore api(snap);
    CHECK(&api.snapshot() == snap.get());
    cascade::core::PluginApiClient& c = api.client("reader.dll", "Reader");
    api.setLiveSet({"reader.dll"});
    CascadeReceiverState st{};
    st.structSize = sizeof(st);
    CHECK(api.getState(c, &st) == CASCADE_API_OK);
    CHECK(st.seq == 0u && st.signalDb == -200.0 && st.demodMode == CASCADE_DEMOD_NFM);

    PublishedState s{};
    s.rx.flags = FOXAPI_RX_RUNNING | FOXAPI_RX_DEVICE_OPEN | FOXAPI_RX_STEREO_ACTIVE;
    s.rx.centreHz = 145.0e6;
    s.rx.vfoOffsetHz = 25.0e3;
    s.rx.signalDb = -30.0;
    s.rx.squelchDb = -60.0;
    s.rx.demodMode = FOXAPI_DEMOD_AM;
    std::snprintf(s.rx.deviceName, sizeof(s.rx.deviceName), "%s", "Radio");
    s.app.abiGainCount = 1;
    std::snprintf(s.app.gains[0].name, sizeof(s.app.gains[0].name), "%s", "LNA");
    s.app.gains[0].maxDb = 40.0;
    s.app.rateCount = 2;
    s.app.rates[0] = 1.0e6;
    s.app.rates[1] = 2.0e6;
    snap->publish(s, nullptr);
    st = CascadeReceiverState{};
    st.structSize = sizeof(st);
    CHECK(api.getState(c, &st) == CASCADE_API_OK);
    CHECK(st.seq == 1u && st.tuneSeq == 1u);
    CHECK(st.centreHz == 145.0e6 && st.tunedHz == 145.025e6 && st.demodMode == CASCADE_DEMOD_AM);
    CHECK((st.flags & CASCADE_STATE_RUNNING) != 0u && (st.flags & CASCADE_STATE_DEVICE_OPEN) != 0u);
    CHECK((st.flags & CASCADE_STATE_STEREO) != 0u && (st.flags & CASCADE_STATE_SQUELCH_OPEN) != 0u);
    CHECK((st.flags & CASCADE_STATE_MUTED) == 0u);
    CHECK(std::string(st.deviceName) == "Radio" && st.gainCount == 1u);
    CascadeGainInfo gi{};
    gi.structSize = sizeof(gi);
    CHECK(api.getGain(c, 0, &gi) == CASCADE_API_OK && std::string(gi.name) == "LNA" && gi.maxDb == 40.0);
    CHECK(api.getGain(c, 1, &gi) == CASCADE_API_NOT_FOUND);
    double rates[4] = {};
    CHECK(api.getSampleRates(c, rates, 4) == 2 && rates[1] == 2.0e6);
}

// --- 6. the cost ------------------------------------------------------------------------

RadioStatus typicalLists() {
    RadioStatus l = everyListDistinct();
    for (int i = 0; i < 300; ++i) { l.bookmarks.push_back({"Bookmark " + std::to_string(i), 1.0e8 + i, "NFM", 12.5e3}); }
    for (int i = 0; i < 20; ++i) {
        RadioStatus::Plugin p;
        p.name = "Plugin " + std::to_string(i);
        p.version = "1.0.0";
        l.plugins.push_back(p);
    }
    l.tracks.resize(50);
    return l;
}

void cost() {
    std::printf("[6] cost (Release is what counts; a Debug build is slower)\n");
    ReceiverSnapshot snap;
    PublishedState s = everyFieldDistinct();
    s.app.abiGainCount = 3;
    s.app.rateCount = 12;
    const std::shared_ptr<const RadioStatus> lists = std::make_shared<RadioStatus>(typicalLists());
    constexpr int kN = 100000;

    // The publish itself: counters, the seqlock store, the block and its swap.
    // The lists are built by the caller (AppWindow::fillStatusLists) and only
    // handed over here, so the same pointer is reused: what is timed is the
    // publishing, not the building.
    auto t0 = Clock::now();
    for (int i = 0; i < kN; ++i) {
        s.rx.centreHz = 1.0e8 + (i & 1);  // a real change every publish: the counters move
        snap.publish(s, lists);
    }
    const double publishNs = std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / kN;

    t0 = Clock::now();
    std::uint64_t sink = 0;
    for (int i = 0; i < kN; ++i) {
        PublishedState r;
        sink += snap.read(r) ? r.rx.seq : 0u;
    }
    const double readNs = std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / kN;

    constexpr int kF = 2000;
    t0 = Clock::now();
    for (int i = 0; i < kF; ++i) {
        const std::shared_ptr<const ReceiverSnapshot::Full> f = snap.readFull();
        const RadioStatus o = cascade::net::composeRadioStatus(f->state, f->lists.get());
        sink += o.bookmarks.size();
    }
    const double fullNs = std::chrono::duration<double, std::nano>(Clock::now() - t0).count() / kF;

    std::printf("      sizeof(PublishedState) = %zu bytes (FoxReceiverState %zu + extension %zu)\n",
                sizeof(PublishedState), sizeof(FoxReceiverState), sizeof(cascade::core::AppStateExt));
    std::printf("      publish: %.0f ns   read (lock-free, plugins/CAT): %.0f ns   "
                "readFull + compose (web, %zu bookmarks): %.0f ns   [%llu]\n",
                publishNs, readNs, lists->bookmarks.size(), fullNs, static_cast<unsigned long long>(sink % 7));
    // Ceilings far above any healthy figure: they fail a pathological change
    // (a lock the writer waits on, a copy per field), not a slow machine.
    CHECK(publishNs < 50000.0);
    CHECK(readNs < 50000.0);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_receiver_snapshot\n");
    seqlockNeverTears();
    snapshotNeverTears();
    writerNeverWaits();
    countersMoveByGroup();
    composeMapsEveryField();
    pluginApiReadsTheSharedSnapshot();
    walkingOneCompose();
    walkingOneSources();
    walkingOnePluginFlags();
    stickyNamesAndIds();
    cost();
    return testSummary("test_receiver_snapshot");
}
