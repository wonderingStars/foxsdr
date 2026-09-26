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
//   - readFull() copies a shared_ptr under fullMutex_ - the only lock here,
//     held for a reference-count increment. The WRITER only ever TRY-locks
//     it: if a reader happens to hold it, this publish's block is not
//     installed and readFull() goes on answering the previous publish (whole
//     and consistent, one frame older) until the next frame's publish. So the
//     writer's worst case is one failed try_lock, never a wait.
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
#include <type_traits>

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

class ReceiverSnapshot {
public:
    // One publish, whole: the state and the /api/status text and lists that
    // were published WITH it. Immutable once installed.
    struct Full {
        PublishedState state;
        // Strings and lists only (net/status_compose.hpp); null when the
        // publisher had none (a PluginApiCore used on its own).
        std::shared_ptr<const net::RadioStatus> lists;
    };

    ReceiverSnapshot();
    ReceiverSnapshot(const ReceiverSnapshot&) = delete;
    ReceiverSnapshot& operator=(const ReceiverSnapshot&) = delete;

    // THE ONE WRITER. `facts` carries everything but the counters and
    // structSize, which this sets; app.published is set too.
    void publish(const PublishedState& facts, std::shared_ptr<const net::RadioStatus> lists);

    // Any thread, lock-free. False only when SeqlockBox::kMaxTries attempts
    // all overlapped a write (out untouched) - "ask again".
    bool read(PublishedState& out) const;

    // Any thread; never null. The newest publish whose block was installed.
    std::shared_ptr<const Full> readFull() const;

    // Publishes whose block could not be installed because a reader held the
    // swap lock at that instant (the writer does not wait). For the tests
    // and the design note; readers never need it.
    std::uint64_t deferredInstalls() const { return deferred_.load(std::memory_order_relaxed); }

    // TEST SEAM: holds the lock readFull() takes, so a test can prove that
    // publish() does not wait for it. Nothing in the application calls it.
    std::unique_lock<std::mutex> holdSwapLockForTest() const {
        return std::unique_lock<std::mutex>(fullMutex_);
    }

private:
    SeqlockBox<PublishedState> box_;
    PublishedState last_{};       // the writer's: the previous publish
    bool havePublished_ = false;  // the writer's
    mutable std::mutex fullMutex_;
    std::shared_ptr<const Full> full_;
    std::atomic<std::uint64_t> deferred_{0};
};

}  // namespace cascade::core

#endif  // CASCADE_CORE_RECEIVER_SNAPSHOT_HPP
