// thread_beats.hpp - a heartbeat for every long-lived worker, and the table that says how late each may be.
//
// WHY THIS EXISTS. core/hang_watchdog.hpp watches ONE thread: the interface's frame loop (and, since the control thread
// has its own watchdog, the engine's pass). Every other thread this product runs - the DSP thread, the source thread,
// the transmitter's, the settings keeper, the servers', the image feed's, the disk workers - could wedge for the rest of
// the session and nothing would say so: the window goes on drawing, the pass goes on ticking, and what the person sees
// is a spectrum that stopped, a sound that stopped, a settings file that is never written. Those are the faults this
// product has actually shipped (docs/DIAGNOSTICS.md) and the watchdog was blind to every one of them that did not stop
// the frame loop too.
//
// WHAT IT IS. A `Registration` is a worker's name tag and a clock it stamps. The worker constructs one on its own
// thread when it starts (the registry records which OS thread that is, so the report can unwind that thread first), and
// calls `beat("phase")` at its natural cadence - at the top of its loop and between the steps that could block. The
// phase is a string LITERAL (a pointer is stored, never a copy), so the report can say not only "dsp stopped" but "dsp
// stopped in plugin-process". A worker that is parked ON PURPOSE - a server in accept(), a worker with no job - says so
// with `wait(kWaitAccept)` and leaves it with `endWait()`: an intentional wait never reports (or reports only past its
// own cap, for the waits that have one), which is the kKnownWaits discipline of tests/test_shutdown_budget.cpp carried
// down to one thread.
//
// WHO WATCHES. NOT A NEW THREAD. `HangWatchdog::threadMain` already wakes twice a second in every process that has a
// watchdog (the window's, the engine's control thread's, the headless stop's), and each wake calls
// `Registry::poll()`. Several watchdogs in one process poll the same registry; a per-registration latch means a given
// wedge is reported by exactly one of them, once. A process with no watchdog (a tool mode, most tests) has no monitor
// and the beats cost what they cost: one clock read and two stores.
//
// WHAT A MISS DOES. The watchdog writes a hang report (`hang-<pid>-N.txt`, the GUI's own file family, the same
// every-thread stack capture) whose header NAMES the thread and its last phase (`thread:`, `thread-phase:`,
// `thread-wait:`), counts `Recovered::ThreadWedge` in the health ledger, and carries on: nothing is killed, nothing is
// restarted, and when the thread beats again the recovery is logged and the registration is re-armed. For the DSP path
// the TX dead-man is untouched - it keys off the control tick and the front end's heartbeat, not off this table, and the
// monitor never calls into the transmitter (see the DSP row of docs/engine-stage4.md 6.27).
//
// THE BUDGETS ARE PER KIND, GENEROUS, AND WRITTEN DOWN WITH THEIR ARGUMENT (kKinds below). A budget is "how long may a
// thread of this kind go without beating while it is NOT in a declared wait, with nothing wrong": it has to exceed the
// longest legitimate block the kind can spend (a disk worker on an unreachable network share waits out Windows' 21 s
// timeout - B19 learned that - and a password hash is 60 s of honest work), by a margin, so that a report means a thread
// is gone and not that it is slow. tests/test_thread_beats.cpp holds each budget to at least kMargin times its argued
// legitimate block, and goes red when a registration site appears that the inventory has never heard of.
//
// THE CLOCK. Every reading is a steady-clock millisecond count plus a test skew (advanceClockForTest), so a test can
// stage twenty-one seconds of legitimate blocking in no time at all and still run the real constants.
//
// Engine-clean: standard library only. No diagnostics log, no health ledger, no lock the beat can take: `beat()` is two
// atomic stores and a clock read, and is safe on the real-time DSP thread.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_THREAD_BEATS_HPP
#define CASCADE_CORE_THREAD_BEATS_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cascade::core::beats {

// ---------------------------------------------------------------------------
// The kinds, and the budget each kind is allowed to go without a beat
// ---------------------------------------------------------------------------

enum class Kind : std::uint8_t {
    Dsp,       // the real-time thread: one block at a time, plugin process() inside it
    Source,    // the I/Q producer: a vendor read() per chunk
    Transmit,  // the transmitter's modulator thread: a 10 ms block per tick, a radio sink behind it
    Keeper,    // the settings keeper: a debounce loop that hands saves to the config writer
    Server,    // a server's own loop: the web server's idle tick, a listener's accept
    Feed,      // a worker that serves a queue: the image feed's, the server host's password worker
    Disk,      // a worker that touches the disk, possibly a network share
    Install,   // a worker that runs an installer
    Port,      // a worker that reads a device through the operating system's port driver
    Open,      // a worker that opens or scans a radio or a sound card: a vendor driver behind it, a USB bus under it
};
inline constexpr std::size_t kKindCount = 10;

struct KindSpec {
    Kind kind;
    const char* word;
    // How long a thread of this kind may go without a beat, outside a declared wait, before it is reported.
    unsigned budgetMs;
    // The longest block the kind legitimately spends between two beats, and what it is. The budget is held to
    // `kMargin` times this by tests/test_thread_beats.cpp.
    unsigned legitBlockMs;
    const char* legitWhat;
};

// A budget is at least this many times the longest legitimate block of its kind.
inline constexpr unsigned kMargin = 2;

inline constexpr KindSpec kKinds[kKindCount] = {
    {Kind::Dsp, "dsp", 10000, 1500,
     "one block of the chain, with a plugin's process() in it; the worst a healthy run shows is a plugin that pages "
     "in a table on its first block (well under a second), and 1.5 s is three of those"},
    {Kind::Source, "source", 30000, 10000,
     "a vendor read() that has nothing to return: the drivers' own bounds are 1-3 s (the native columns of "
     "tests/test_shutdown_budget.cpp), a re-tune on a slow radio can hold the stream for several seconds more"},
    {Kind::Transmit, "transmit", 10000, 3000,
     "a block handed to a sink that is a network radio: the Pluto writer's bound is 1.5 s and the radio may take its "
     "own turn at the socket"},
    {Kind::Keeper, "keeper", 60000, 21000,
     "the keeper writes the settings file through the config writer, which can sit on a cloud-synced or network "
     "profile folder: Windows' network timeout is 21 s (B19)"},
    {Kind::Server, "server", 30000, 10000,
     "a handler the loop waits for: httplib cannot interrupt a request a browser holds open, and net::kClientIoTimeout "
     "(10 s) is its bound"},
    {Kind::Feed, "feed", 120000, 60000,
     "a queued job: the server host's password hash is the long one (the engine waits for it up to kProvisionBound, "
     "60 s, and the hash is the reason the bound is that long); the host worker's reconcile stops a server, and "
     "httplib waits out a request a browser holds open (net::kClientIoTimeout, 10 s); a tile decode is milliseconds"},
    {Kind::Disk, "disk", 60000, 21000,
     "a read or a write on a network share that is offline waits out Windows' 21 s timeout once (B19: Z: on this "
     "desk); a second path behind it doubles that"},
    {Kind::Install, "install", 600000, 240000,
     "a vendor driver's installer: it asks for elevation, copies a driver package and may restart a service; the "
     "slowest seen is a few minutes"},
    {Kind::Port, "port", 30000, 10000,
     "a serial port's read: bounded by the port layer's own event wait (a second at most) and the loop checks its stop "
     "flag before each one; the OPEN is not registered - GpsReader::kOpenAbandonWait is its bound, and the worker is "
     "abandoned past it"},
    {Kind::Open, "open", 180000, 60000,
     "a radio's first open or a scan: an Ettus B200 loads its FPGA image over USB before it answers (the UHD images "
     "path, tens of seconds on a slow bus), a network radio's connect waits out its timeouts, the SDRplay service may "
     "need a restart. Judged, not measured here: 60 s is a generous reading of the slowest of those, and a hung open "
     "is exactly what the 'opening radio' state would otherwise show for ever"},
};

inline constexpr const KindSpec& kindSpec(Kind k) { return kKinds[static_cast<std::size_t>(k)]; }
inline constexpr unsigned budgetMs(Kind k) { return kindSpec(k).budgetMs; }

// ---------------------------------------------------------------------------
// The threads, by name: the inventory (docs/engine-stage4.md 6.27 prints it)
// ---------------------------------------------------------------------------

struct ThreadSpec {
    const char* name;  // one lower-case word (letters, digits, '-'): it is written into a report
    Kind kind;
};

inline constexpr ThreadSpec kThreadDsp{"dsp", Kind::Dsp};
inline constexpr ThreadSpec kThreadSource{"source", Kind::Source};
inline constexpr ThreadSpec kThreadPatchSource{"patch-source", Kind::Source};
inline constexpr ThreadSpec kThreadTransmit{"transmit", Kind::Transmit};
inline constexpr ThreadSpec kThreadKeeper{"keeper", Kind::Keeper};
inline constexpr ThreadSpec kThreadWebListen{"web-listen", Kind::Server};
inline constexpr ThreadSpec kThreadWebIdle{"web-idle", Kind::Server};
inline constexpr ThreadSpec kThreadCatAccept{"cat-accept", Kind::Server};
inline constexpr ThreadSpec kThreadCatClient{"cat-client", Kind::Server};
inline constexpr ThreadSpec kThreadImageFeed{"image-feed", Kind::Feed};
inline constexpr ThreadSpec kThreadServerHost{"server-host", Kind::Feed};
inline constexpr ThreadSpec kThreadPasswordHash{"password-hash", Kind::Feed};
inline constexpr ThreadSpec kThreadDiskJob{"disk-job", Kind::Disk};
inline constexpr ThreadSpec kThreadConfigWrite{"config-write", Kind::Disk};
inline constexpr ThreadSpec kThreadRecordOpen{"record-open", Kind::Disk};
inline constexpr ThreadSpec kThreadLinkPoll{"link-poll", Kind::Disk};
inline constexpr ThreadSpec kThreadDriverInstall{"driver-install", Kind::Install};
inline constexpr ThreadSpec kThreadGps{"gps", Kind::Port};
inline constexpr ThreadSpec kThreadDeviceScan{"device-scan", Kind::Open};
inline constexpr ThreadSpec kThreadDeviceOpen{"device-open", Kind::Open};
inline constexpr ThreadSpec kThreadPatchOpen{"patch-open", Kind::Open};
inline constexpr ThreadSpec kThreadSoundCardScan{"soundcard-scan", Kind::Open};
inline constexpr ThreadSpec kThreadSoundCardOpen{"soundcard-open", Kind::Open};

inline constexpr const ThreadSpec* kAllThreads[] = {
    &kThreadDsp,        &kThreadSource,      &kThreadPatchSource,  &kThreadTransmit,      &kThreadKeeper,
    &kThreadWebListen,  &kThreadWebIdle,     &kThreadCatAccept,    &kThreadCatClient,     &kThreadImageFeed,
    &kThreadServerHost, &kThreadPasswordHash, &kThreadDiskJob,     &kThreadConfigWrite,   &kThreadRecordOpen,
    &kThreadLinkPoll,   &kThreadDriverInstall, &kThreadGps,        &kThreadDeviceScan,    &kThreadDeviceOpen,
    &kThreadPatchOpen,  &kThreadSoundCardScan, &kThreadSoundCardOpen,
};
inline constexpr std::size_t kThreadCount = sizeof(kAllThreads) / sizeof(kAllThreads[0]);

// ---------------------------------------------------------------------------
// The declared waits: a thread parked ON PURPOSE (the kKnownWaits discipline, per thread)
// ---------------------------------------------------------------------------

struct WaitSpec {
    const char* name;
    // How long the wait may last before it is reported anyway. 0: no limit - the wait ends when the world gives the
    // thread something to do, and a thread that waits for ever with nothing to do is not wedged.
    unsigned capMs;
    const char* argued;
};

inline constexpr WaitSpec kWaitAccept{"accept", 0,
    "a listener in accept(): it ends when a client connects or the listener is closed; with nobody connecting it is "
    "idle, which is the normal state of a server nobody is using"};
inline constexpr WaitSpec kWaitJob{"job", 0,
    "a queue worker with an empty queue, parked on its condition variable until a job or the stop flag arrives"};
inline constexpr WaitSpec kWaitKey{"key-up", 0,
    "the transmitter's thread with the radio not keyed: parked until a key goes down or the stop flag arrives"};
inline constexpr WaitSpec kWaitInstaller{"installer", 480000,
    "the installer process the job started and is waiting on: it is somebody else's program and a person answers its "
    "prompts; eight minutes is twice the slowest seen"};

inline constexpr const WaitSpec* kAllWaits[] = {&kWaitAccept, &kWaitJob, &kWaitKey, &kWaitInstaller};
inline constexpr std::size_t kWaitCount = sizeof(kAllWaits) / sizeof(kAllWaits[0]);

// ---------------------------------------------------------------------------
// The registration (what a worker holds), and the registry (what the watchdog reads)
// ---------------------------------------------------------------------------

namespace detail {
struct Slot;
}

class Registration {
public:
    // Registers the CALLING thread. `live`, when given, is a flag the owner of a thread it may ABANDON clears (the
    // source thread's stop token): a registration whose flag reads false is retired in effect - the thread is a
    // zombie the owner has already counted (Recovered::SrcThread), and a second report about it would be noise.
    explicit Registration(const ThreadSpec& spec, std::shared_ptr<const std::atomic<bool>> live = nullptr);
    ~Registration();
    Registration(const Registration&) = delete;
    Registration& operator=(const Registration&) = delete;

    // Alive, and in `phase` (a string literal; nullptr keeps the last one). Ends a declared wait.
    void beat(const char* phase = nullptr) noexcept;
    // Parked on purpose. Counts as alive for as long as the wait's cap (none, for a wait with no cap).
    void wait(const WaitSpec& w) noexcept;
    void endWait(const char* phase = nullptr) noexcept { beat(phase); }

private:
    std::shared_ptr<detail::Slot> slot_;
};

// A scope for a declared wait: parks on construction, beats on destruction.
class WaitScope {
public:
    WaitScope(Registration& r, const WaitSpec& w) noexcept : r_(r) { r_.wait(w); }
    ~WaitScope() { r_.endWait(); }
    WaitScope(const WaitScope&) = delete;
    WaitScope& operator=(const WaitScope&) = delete;

private:
    Registration& r_;
};

// ---------------------------------------------------------------------------
// A CALL INTO CODE THE PRODUCT DOES NOT OWN, in flight (plugin containment P1, docs/engine-stage4.md 6.28)
// ---------------------------------------------------------------------------
//
// The registry above names a THREAD that stopped beating. A call into a plugin is a different shape: the thread (the interface's, the
// control thread, the image feed's worker) is perfectly healthy until the call, and the call is what stops - so the thing to name is the
// plugin and the entry point it was called through. That is what a CallScope declares: "this thread is inside <subject>'s <what> and
// should be out within <budget> ms". It rides the SAME monitor (Registry::poll, which HangWatchdog's thread already calls twice a
// second) and the same report path (`hang-<pid>-w<N>.txt`): there is no second watchdog and no new thread.
//
// `subject` and `what` are pointers that outlive the scope (the plugin containment passes an interned public id and a string literal);
// they are never copied, and the report only ever writes them through reportId() below. A scope costs a clock read and a handful of
// atomic stores on the calling thread and takes no lock, so it is as safe on the real-time thread as a beat.
//
// TWO STAGES, because a slow call and a call that never returns are different faults: past `budgetMs` the call has OVERRUN (one report,
// naming the subject and the entry; the call may yet return), and past `wedgeMs` it is WEDGED (counted, and the owner decides what to do
// about a plugin it can no longer call - the containment quarantines it). Each stage is reported once per call. A call that returns
// is simply gone from the table; the owner of the scope asks `overranFlags()` at its end to count a late return the monitor never
// saw (no watchdog in the process), and both sides count a stage at most once through `claimStage`.
class CallScope {
public:
    // `owner` is the caller's own handle for the subject (the containment's slot), handed back by ownerOfThread() for the crash path.
    CallScope(const void* owner, const char* subject, const char* what, unsigned budgetMs, unsigned wedgeMs) noexcept;
    ~CallScope();
    CallScope(const CallScope&) = delete;
    CallScope& operator=(const CallScope&) = delete;

    // Milliseconds since the call began, on the registry's clock (a test can advance it).
    std::int64_t elapsedMs() const noexcept;
    // Claims stage 1 (overrun) or 2 (wedge) of THIS call for the caller: true exactly once per stage, whoever asks first (this scope's
    // owner at the end of the call, or the monitor while it is in flight). False for a call whose table slot was full.
    bool claimStage(int stage) noexcept;
    // True when the table had no room and this call is not watched (the call itself is unaffected).
    bool unwatched() const noexcept { return index_ < 0; }

private:
    int index_ = -1;
    std::int64_t startMs_ = 0;
    unsigned budgetMs_ = 0;
    unsigned wedgeMs_ = 0;
};

// The call the given OS thread is in: its `owner` and the `what` it declared. False when it is in none. Lock-free and
// allocation-free: the crash path reads it.
bool callOfThread(unsigned long tid, const void** owner, const char** what) noexcept;
// How many calls are in flight now (tests, and the report).
std::size_t callsInFlight() noexcept;
// The `owner` of every call in flight now (up to `cap`; the same owner may appear twice), for the containment's unload-timeout charge.
std::size_t liveCallOwners(const void** out, std::size_t cap) noexcept;

// What the monitor found.
struct Finding {
    std::string name;   // the thread's name, from the inventory
    std::string phase;  // its last phase, or empty
    std::string wait;   // the declared wait it is in, or empty
    unsigned long tid = 0;
    std::int64_t stalledMs = 0;  // since its last beat (or since the wait began)
    unsigned budgetMs = 0;       // what it exceeded
    // FOR A CALL (CallScope) AND ONLY THEN: who and through what. `name` is "plugin-call", `phase` the entry's word, `tid` the thread
    // that is inside the call (its stack is the first one in the report), and these three are what the containment counts by.
    std::string subject;           // the interned public id the call declared
    const void* owner = nullptr;  // the owner pointer the call declared
    int stage = 0;                 // 0 for a thread; 1 overrun; 2 wedged
};

struct PollResult {
    std::vector<Finding> wedged;     // newly late: each is reported once per episode
    std::vector<Finding> recovered;  // beat again after a report: stalledMs is how long the episode lasted
    // Calls in flight past their budget (stage 1) or their wedge limit (stage 2), each reported once per call.
    std::vector<Finding> calls;
};

class Registry {
public:
    static Registry& instance();

    // One look at every registration, against the clock. Called from HangWatchdog's thread; any thread may call it.
    // Returns only what CHANGED since the last look: a wedge it has already returned is not returned again until the
    // thread has beaten (and been returned as recovered).
    PollResult poll();

    // After the process lost time (a machine asleep, a paused VM) nothing is late: stamps every registration now.
    void rearm();

    // The whole table, one line per registration: `  name age-ms=N phase=P wait=W`. For the report's `beats` section.
    std::string snapshotText();

    // WHETHER THE RADIO IS KEYED, for the report (`transmitting:`): the engine installs a probe over the transmitter's
    // own atomic state, and a report asks it. The monitor only ever READS it - it never keys or un-keys anything, the TX
    // dead-man (core/transmitter.hpp kKeyAliveWait) is the transmitter's own. The probe is called under a lock that
    // clearKeyedProbe() takes, so an engine that is being destroyed can take its probe away and know it is not running.
    // `owner` is whoever installed it (the engine passes itself): a clear from anyone else is ignored, so a second engine
    // in one process (a test) cannot take the first one's probe away.
    static void setKeyedProbe(const void* owner, std::function<bool()> probe);
    static void clearKeyedProbe(const void* owner);
    // 1 keyed, 0 not keyed, -1 no probe installed.
    static int keyedNow();

    std::size_t size() const;

    // How many wedges poll() has ever returned in this process: monotonic, and counted whether or not a report could be
    // written. The engine's pass reads it with the other recovery counters (health::RecoveryReadings::threadWedges), so a
    // wedge is counted in the ledger by the control path and never by the watchdog's thread.
    static std::uint64_t wedgesFound();

    // TESTS ONLY. A budget standing in for a kind's, and a clock that runs ahead by `ms`. Neither is reachable from the
    // product: nothing under src/ calls them (tests/test_thread_beats.cpp scans for that).
    void setBudgetOverrideForTest(Kind k, unsigned ms);
    void clearOverridesForTest();
    void advanceClockForTest(std::int64_t ms);
    // While frozen the clock does not run by itself: it reads the instant it was frozen at plus whatever advanceClockForTest has added,
    // so a test can put a thread exactly one millisecond short of its budget without the real time its own thread spends between two
    // lines (a ThreadSanitizer build spends milliseconds) moving the boundary. Nesting is not supported; unfreeze before freezing again.
    void freezeClockForTest(bool frozen);

private:
    Registry() = default;
    friend class Registration;
    void add(const std::shared_ptr<detail::Slot>& s);
    void remove(const detail::Slot* s);
};

// A thread name safe to write into a report: letters, digits and '-' only, and no longer than 32. Anything else - a
// name this build does not know - reads as "unknown".
std::string reportWord(const char* s);

// An identifier safe to write into a report: lower-case letters, digits, '-', '_' and '.', at most 40 characters. Anything else reads as
// "unknown". (A plugin's public id: the catalogue id of an official plugin, or a hash for any other.)
std::string reportId(const char* s);

}  // namespace cascade::core::beats

#endif  // CASCADE_CORE_THREAD_BEATS_HPP
