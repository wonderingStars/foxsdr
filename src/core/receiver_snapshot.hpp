// receiver_snapshot.hpp - ONE published state of the receiver, for every
// reader (engine extraction stage 2, docs/engine-stage2.md).
//
// WHAT IT IS. Once a frame the application fills a PublishedState - the engine
// API's own FoxReceiverState (third_party/foxsdr_api/foxsdr_api.h, vendored
// unmodified) plus AppStateExt, the fields the app's readers need that API 0.2
// has no field for - and publishes it here, together with the /api/status
// text and lists (a net::RadioStatus holding only strings and vectors). Every
// reader answers from what was published:
//
//   - the plugin host API (get_state, get_gain, get_sample_rates, the device
//     checks in its requests) and CAT read the PublishedState through a
//     sequence lock: no lock, no allocation, callable from a real-time thread;
//   - GET /api/status (and the /api/control key gate) take the whole block -
//     the state AND the lists of the same publish - as one immutable object.
//
// THE THREADING RULE. One writer: publish() is called from ONE thread (the
// GUI thread in stage 2, the engine's control thread from stage 3). Readers
// on any thread never block the writer and never see a torn state:
//
//   - read() is SeqlockBox::load: the writer never waits for it; a reader
//     that keeps overlapping a write gives up (false) rather than spin.
//   - readFull() copies a shared_ptr under fullMutex_, held for a
//     reference-count increment. The WRITER only ever TRY-locks it. If a
//     reader holds it at that instant, the writer hands the block over under
//     a second lock (handoffMutex_, also only try-locked by the writer) and
//     the next readFull() installs it; if that is busy too (a reader is in
//     that very install), the writer keeps the block and retryInstall() -
//     called on the writer's EVERY pass, change or not - lands it. So a
//     block is never lost behind a frame loop that has stopped (the Windows
//     move/resize loop runs no frames), and the writer never waits.
//
// THE COUNTERS ARE DERIVED, NOT SET. publish() compares the new facts with
// the previous publish and advances FoxReceiverState's seq/tuneSeq/modeSeq/
// deviceSeq/audioSeq/displaySeq/txSeq and the host API level 1's own five
// (AppStateExt::abi*, whose groups differ - see the .cpp), so no caller can
// forget to bump one. All start at 1 on the first publish.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_RECEIVER_SNAPSHOT_HPP
#define CASCADE_CORE_RECEIVER_SNAPSHOT_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

#include "core/plugin_abi.h"
#include "core/seqlock_box.hpp"
#include "foxsdr_api.h"

namespace cascade::net {
struct RadioStatus;
}

namespace cascade::core {

// The plugin ABI's gain table as the host publishes it (get_gain).
inline constexpr std::size_t kMaxPublishedGains = 16;
// Sample rates a radio offers. The RTL-SDR lists twelve and the HackRF a
// handful; 32 leaves room without making the snapshot large.
inline constexpr std::size_t kMaxPublishedRates = 32;

struct PublishedGain {
    char name[CASCADE_GAIN_NAME_CHARS] = {};
    std::uint32_t unit = CASCADE_GAIN_UNIT_DB;
    double minDb = 0.0;
    double maxDb = 0.0;
    double stepDb = 0.0;
    double currentDb = 0.0;
};

// The scanner's state, as /api/status names it (net::scannerStateName).
enum : std::uint32_t {
    kScannerIdle = 0,
    kScannerScanning = 1,
    kScannerPaused = 2,
    kScannerHolding = 3,
};

// EVERYTHING A READER NEEDS THAT FoxReceiverState HAS NO FIELD FOR. Each
// member is listed, with why API 0.2 lacks it, in docs/engine-stage2.md
// section 3. Plain data: it travels through the SeqlockBox with the rest.
struct AppStateExt {
    // False until the first publish(): a reader then answers exactly what it
    // answered before anything was published (web and CAT: a default
    // RadioStatus).
    bool published = false;

    // --- the host API level 1 (plugin_abi.h) ------------------------------
    // Its sequence counters, derived by publish() with the ABI's own groups.
    std::uint64_t abiSeq = 0;
    std::uint64_t abiTuneSeq = 0;
    std::uint64_t abiModeSeq = 0;
    std::uint64_t abiDeviceSeq = 0;
    std::uint64_t abiAudioSeq = 0;
    // The gain stages get_gain describes (at most kMaxPublishedGains; 0 with
    // no radio open) and the rates get_sample_rates lists. In the engine API
    // these are LISTS (FOXAPI_LIST_GAINS, FOXAPI_LIST_SAMPLE_RATES), not
    // fields; a plugin reads them from any thread, so they are here.
    std::uint32_t abiGainCount = 0;
    PublishedGain gains[kMaxPublishedGains];
    std::uint32_t rateCount = 0;
    double rates[kMaxPublishedRates] = {};
    // get_stream_info's output figures (the stream CLOCK stays live in the
    // runner's StreamClock - docs/engine-stage2.md section 4).
    double outputRateHz = 0.0;
    std::uint64_t outputFrames = 0;  // measurement

    // --- /api/status figures API 0.2 has no field for ------------------------
    bool rxPositionSet = false;
    double rxLatDeg = 0.0;
    double rxLonDeg = 0.0;
    bool autoNotchEngaged = false;   // measurement (API gap: auto-notch readout)
    double autoNotchFreqHz = 0.0;    // measurement
    bool pilotLocked = false;        // measurement
    bool rdsSynced = false;          // measurements (API gap: RDS), all of them
    bool rdsPiValid = false;
    bool rdsPsValid = false;
    bool rdsTp = false;
    bool rdsTa = false;
    std::uint32_t rdsPi = 0;
    std::uint32_t rdsPty = 0;
    std::uint32_t rdsGroups = 0;
    std::uint32_t rdsErrors = 0;
    bool sourceBusy = false;         // a device scan or open is in flight
    std::uint64_t audioPrimingCallbacks = 0;  // measurements: the sink's health
    double audioRingMs = 0.0;
    double audioRingCapacityMs = 0.0;
    std::uint64_t audioPluginGaps = 0;
    std::uint64_t audioPluginGapFrames = 0;
    std::uint64_t iqBytes = 0;       // the recorders' progress
    std::uint64_t audioBytes = 0;
    std::uint32_t scannerState = kScannerIdle;
    double scanStartHz = 0.0;
    double scanStopHz = 0.0;
    double scanStepHz = 0.0;
    bool catalogueBusy = false;      // a catalogue fetch or download in flight
    bool basemapActive = false;
    std::uint32_t basemapMinZoom = 0;
    std::uint32_t basemapMaxZoom = 0;
    std::uint32_t basemapTileSize = 0;
};

struct PublishedState {
    FoxReceiverState rx;
    AppStateExt app;
};

static_assert(std::is_trivially_copyable_v<PublishedState>,
              "the snapshot travels through a SeqlockBox");
static_assert(FOXAPI_NAME_CHARS == CASCADE_DEVICE_NAME_CHARS,
              "the plugin ABI's deviceName is copied from FoxReceiverState::deviceName");

// What every reader is given before the first publish(): the zero state, with
// the two defaults the plugin API always answered then (signalDb -200 and
// NFM) and app.published false.
PublishedState initialPublishedState();

// THE CONDITIONS BEHIND FoxReceiverState::flags, one per flag, named for what
// the publisher reads. receiverFlags turns them into the flag word; the
// walking-one test (test_receiver_snapshot [7]) proves each condition sets
// exactly its own bit, and test_snapshot_app proves the window hands each
// condition its own source.
struct RxFlagSources {
    bool running = false;
    bool deviceOpen = false;
    bool faulted = false;
    bool muted = false;
    bool squelchOpen = false;
    bool stereoEnabled = false;
    bool stereoActive = false;
    bool nr = false;
    bool notch = false;
    bool autoNotch = false;
    bool deviceAgc = false;
    bool agcSupported = false;
    bool recordingIq = false;
    bool recordingAudio = false;
    bool scannerActive = false;
    bool decoderActive = false;
    bool txAvailable = false;
    bool txKeyed = false;
    bool txLatched = false;
    bool txRemoteArmed = false;
    bool sinkOpen = false;
    bool webListening = false;
};
std::uint32_t receiverFlags(const RxFlagSources& s);

// A NAME READ WITH A TRY-LOCK answers "" when the lock was busy (the audio
// sink's openedDeviceName while an open runs). "" is then not news: keep the
// last good name, so a busy lock cannot move a counter. Returns the name to
// publish.
inline const std::string& keepLastGoodName(std::string& lastGood, const std::string& fresh) {
    if (!fresh.empty()) { lastGood = fresh; }
    return lastGood;
}

class ReceiverSnapshot {
public:
    // One publish, whole: the state, the /api/status text and lists, and the
    // ids of the bookmarks those lists show, all from the SAME publish.
    // Immutable once installed.
    struct Full {
        PublishedState state;
        // Strings and lists only (net/status_compose.hpp); null when the
        // publisher had none (a PluginApiCore used on its own).
        std::shared_ptr<const net::RadioStatus> lists;
        // Bookmark::id of lists->bookmarks[i], row for row: the web remote's
        // row numbers are resolved through THIS, so a row always names the
        // bookmark the block it was served from showed on it.
        std::vector<std::uint64_t> bookmarkIds;
        std::uint64_t generation = 0;  // publishes so far; 0 = the initial block
    };

    ReceiverSnapshot();
    ReceiverSnapshot(const ReceiverSnapshot&) = delete;
    ReceiverSnapshot& operator=(const ReceiverSnapshot&) = delete;

    // THE ONE WRITER. `facts` carries everything but the counters and
    // structSize, which this sets; app.published is set too. Never waits: the
    // block is installed, handed over for the next reader to install, or
    // kept for retryInstall() - see below.
    void publish(const PublishedState& facts, std::shared_ptr<const net::RadioStatus> lists,
                 std::vector<std::uint64_t> bookmarkIds = {});

    // THE WRITER'S EVERY PASS, change or no change (the frame loop now, the
    // control thread's every pass in stage 3). Lands a block publish() could
    // neither install nor hand over; true when nothing is left waiting.
    // Never waits.
    bool retryInstall();

    // Any thread, lock-free. False only when SeqlockBox::kMaxTries attempts
    // all overlapped a write (out untouched) - "ask again".
    bool read(PublishedState& out) const;

    // Any thread; never null. The newest block published and not still held
    // back by the writer - installing a handed-over one first if there is
    // one, so a reader never waits for the writer's next pass to see it.
    std::shared_ptr<const Full> readFull() const;

    // Publishes whose block could not be installed directly because a reader
    // held the swap lock at that instant; of those, the ones that could not
    // even be handed over and waited for retryInstall(). For the tests and
    // the design note; readers never need them.
    std::uint64_t deferredInstalls() const { return deferred_.load(std::memory_order_relaxed); }
    std::uint64_t heldBackInstalls() const { return heldBack_.load(std::memory_order_relaxed); }
    // The writer's: a block is waiting for retryInstall().
    bool installPending() const { return unsent_ != nullptr; }

    // TEST SEAMS: hold the locks readFull() takes, so a test can prove that
    // publish() and retryInstall() never wait for them, and what they do
    // instead. Nothing in the application calls them.
    std::unique_lock<std::mutex> holdSwapLockForTest() const {
        return std::unique_lock<std::mutex>(fullMutex_);
    }
    std::unique_lock<std::mutex> holdHandoffLockForTest() const {
        return std::unique_lock<std::mutex>(handoffMutex_);
    }

private:
    SeqlockBox<PublishedState> box_;
    PublishedState last_{};       // the writer's: the previous publish
    bool havePublished_ = false;  // the writer's
    std::uint64_t generation_ = 0;       // the writer's
    std::shared_ptr<const Full> unsent_;  // the writer's: neither installed nor handed over
    // LOCK ORDER: fullMutex_, then handoffMutex_. Readers take them in that
    // order (blocking); the writer only ever TRY-locks either.
    mutable std::mutex fullMutex_;
    mutable std::shared_ptr<const Full> full_;     // under fullMutex_
    mutable std::mutex handoffMutex_;
    mutable std::shared_ptr<const Full> handoff_;  // under handoffMutex_
    std::atomic<std::uint64_t> deferred_{0};
    std::atomic<std::uint64_t> heldBack_{0};
};

}  // namespace cascade::core

#endif  // CASCADE_CORE_RECEIVER_SNAPSHOT_HPP
