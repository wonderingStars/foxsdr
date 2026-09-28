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
//     reference-count increment (and, at most, the install of a handed-over
//     block). The WRITER only ever TRY-locks it. If a reader holds it at that
//     instant, the writer HANDS THE BLOCK OVER through a lock-free slot - one
//     atomic pointer exchange, which cannot fail - and frees whatever older
//     block it takes back out; the next readFull() takes the slot's block and
//     installs it, under the lock it holds anyway. So a publish is visible to
//     every readFull() that starts after publish() returns, with no further
//     pass of the writer - a frame loop that has stopped (the Windows
//     move/resize loop runs no frames) strands nothing - and the writer
//     never waits. retryInstall(), on the writer's every pass, only moves a
//     slot block into place early, so its memory is released on the writer.
//
//   A READFULL() FROM THE WRITER'S OWN THREAD IS A SHORT BLOCKING LOCK. The
//   writer's publish() and retryInstall() never wait; but the thread that
//   publishes also reads the whole block in one place - applyControlRequest,
//   resolving the web remote's bookmark rows (Engine::applyControlRequest
//   since engine stage 3; the GUI thread in 3a, the engine's control thread
//   from 3b) - and that readFull() takes fullMutex_ like any reader, so it
//   can wait for another reader's refcount copy or slot install.
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

    // --- the Airspy R2/Mini panel (0.99.41, engine/stage3b-pre Airspy round,
    // docs/engine-stage3.md OPEN 2/3) -----------------------------------------
    // Published so drawAirspyControls reads the open device's state from
    // HERE, once a frame, instead of holding the live cascade::source::
    // AirspySource* engine_.asAirspyDevice() hands out (a pointer into
    // engine-owned, mutable object state - the one query of this shape the
    // Engine ever returned - and the one the control thread will one day be
    // reopening behind, in 3b). false/default when the open device is not an
    // Airspy (or none is open); a control never needs to ask which.
    bool airspyOpen = false;
    unsigned airspyDecimation = 1;
    double airspyHardwareSampleRateHz = 0.0;
    std::uint32_t airspyGainMode = 0;  // cascade::source::AirspySource::GainMode
    bool airspyLnaAgc = false;
    bool airspyMixerAgc = false;
    static constexpr std::size_t kMaxAirspyDecimationChoices = 8;  // airspy::kDecimations has 7
    std::uint32_t airspyDecimationChoiceCount = 0;
    unsigned airspyDecimationChoices[kMaxAirspyDecimationChoices] = {};
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
        // row numbers are resolved through THIS - the ids of the block
        // current when the request is APPLIED (readFull() at that moment),
        // which is the block /api/status serves then. The browser's own rows
        // are from its last poll, so a row that moved since names what is on
        // it now (docs/engine-stage2.md section 4).
        std::vector<std::uint64_t> bookmarkIds;
        std::uint64_t generation = 0;  // publishes so far; 0 = the initial block
    };

    ReceiverSnapshot();
    ReceiverSnapshot(const ReceiverSnapshot&) = delete;
    ReceiverSnapshot& operator=(const ReceiverSnapshot&) = delete;

    // THE ONE WRITER. `facts` carries everything but the counters and
    // structSize, which this sets; app.published is set too. Never waits: the
    // block is installed, or handed over through the slot for the next
    // reader to install - either way every readFull() that starts after this
    // returns sees it (or a newer one).
    void publish(const PublishedState& facts, std::shared_ptr<const net::RadioStatus> lists,
                 std::vector<std::uint64_t> bookmarkIds = {});

    // THE WRITER'S EVERY PASS, change or no change (the frame loop now, the
    // control thread's every pass from stage 3b). Installs a handed-over block
    // if the swap lock is free, so the block it replaces is released on the
    // writer rather than by a reader; true when the slot is empty afterwards.
    // Never waits. Not needed for any reader to SEE a block (the next
    // readFull() installs it) - it keeps the slot from holding a block, and
    // the old installed one alive, through a long stretch with no readers.
    bool retryInstall();

    // Any thread, lock-free. False only when SeqlockBox::kMaxTries attempts
    // all overlapped a write (out untouched) - "ask again".
    bool read(PublishedState& out) const;

    // Any thread; never null. The newest block published and not still held
    // back by the writer - installing a handed-over one first if there is
    // one, so a reader never waits for the writer's next pass to see it.
    std::shared_ptr<const Full> readFull() const;

    // Publishes whose block could not be installed directly because a reader
    // held the swap lock at that instant, and so went through the slot. A
    // retryInstall() that finds the lock busy counts nothing: its block is
    // already in the slot. For the tests and the design note.
    std::uint64_t deferredInstalls() const { return deferred_.load(std::memory_order_relaxed); }
    // WRITER-SIDE AND TESTS ONLY - never read by the window or any reader: a
    // handed-over block is in the slot, waiting for the next readFull() or
    // retryInstall() to install it.
    bool installPending() const { return handoff_.load(std::memory_order_acquire) != nullptr; }

    // TEST SEAM: hold the lock readFull() takes, so a test can prove that
    // publish() and retryInstall() never wait for it, and what they do
    // instead. Nothing in the application calls it.
    std::unique_lock<std::mutex> holdSwapLockForTest() const {
        return std::unique_lock<std::mutex>(fullMutex_);
    }

    ~ReceiverSnapshot();

private:
    SeqlockBox<PublishedState> box_;
    PublishedState last_{};       // the writer's: the previous publish
    bool havePublished_ = false;  // the writer's
    std::uint64_t generation_ = 0;       // the writer's
    // Readers take fullMutex_ (blocking); the writer only ever TRY-locks it.
    mutable std::mutex fullMutex_;
    mutable std::shared_ptr<const Full> full_;  // under fullMutex_
    // THE HAND-OVER SLOT: a heap-held pointer to a block, or null. The
    // writer EXCHANGES its block in (never waits, never fails) and frees what
    // comes back, which is always an older block of its own; a reader
    // exchanges it OUT only while holding fullMutex_, and installs it unless
    // the installed block is newer (the generation check). Whoever takes a
    // pointer out owns it, so nothing is freed twice or read after freeing.
    mutable std::atomic<std::shared_ptr<const Full>*> handoff_{nullptr};
    std::atomic<std::uint64_t> deferred_{0};
    // Installs `block` (publish's last step): directly if the swap lock is
    // free, else through the slot.
    void install(std::shared_ptr<const Full> block);
};

}  // namespace cascade::core

#endif  // CASCADE_CORE_RECEIVER_SNAPSHOT_HPP
