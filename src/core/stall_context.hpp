// stall_context.hpp - WHAT A FREEZE REPORT CAN SAY ABOUT WHO WAS STALLED AND WHAT IT WAS WAITING FOR (freeze diagnosis, section 7).
//
// The 17 field freeze reports since 19 November could not name their cause: the control thread's watchdog is an ordinary HangWatchdog that wrote "thread: gui" and
// the WINDOW's frame scope; a lock wait showed a kernel stub and nothing about who held the lock; the pass had no phase. This header is the small, lock-free
// vocabulary the report writer reads off the stalled process:
//
//   - a table from OS thread id to ROLE (window, control, dsp, source, other), written once by each thread when it starts and read by the watchdog without a lock;
//   - the control pass's PHASE, one atomic word from a closed list, set by Engine::runPassPart;
//   - TaggedMutex: a std::mutex that records, in relaxed atomics, WHO holds it (thread id, a static site literal, since when) and who is WAITING for it, so that
//     a report of a thread stalled on it can say "waiting for plugin-runner, held by control at start for 5012 ms".
//
// EVERYTHING HERE IS FIXED WORDS AND NUMBERS. No path, no file name, no host, no address, no frequency and no text the user typed can get in: the roles, phases, lock
// names and sites are string literals chosen in this source, and the report writer writes only those and milliseconds.
//
// COST. A TaggedMutex acquisition is the mutex's own plus four relaxed stores and one load; no allocation, no lock, safe on the DSP thread (the thread beats do the
// same). Nothing here ever blocks.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>

// (No <windows.h> here: this header is included by diag_log.hpp and so by nearly everything, and a window-system header must not follow it everywhere. The one
// function it needs is declared as the system declares it.)
#if defined(_WIN32)
extern "C" __declspec(dllimport) unsigned long __stdcall GetCurrentThreadId(void);
#endif

namespace cascade::core::stall {

// The calling thread's OS id, as the watchdog and the beats registry name threads (GetCurrentThreadId on Windows).
inline unsigned long currentTid() noexcept {
#if defined(_WIN32)
    return static_cast<unsigned long>(::GetCurrentThreadId());
#else
    return 0ul;
#endif
}

inline std::int64_t nowMs() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------------------------------------------------------------
// ROLES
// ---------------------------------------------------------------------------------------------------------------------------------

enum class Role : std::uint8_t { Other = 0, Window, Control, Dsp, Source, Count };

inline const char* roleWord(Role r) noexcept {
    switch (r) {
        case Role::Window: return "window";
        case Role::Control: return "control";
        case Role::Dsp: return "dsp";
        case Role::Source: return "source";
        default: return "other";
    }
}

namespace detail {
constexpr std::size_t kRoleSlots = 48;
struct RoleSlot {
    std::atomic<unsigned long> tid{0};
    std::atomic<std::uint8_t> role{0};
};
inline std::array<RoleSlot, kRoleSlots>& roleTable() {
    static std::array<RoleSlot, kRoleSlots> table;
    return table;
}
}  // namespace detail

inline bool roleFromWord(const char* w, Role& out) noexcept {
    if (w == nullptr) { return false; }
    for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(Role::Count); ++i) {
        const char* word = roleWord(static_cast<Role>(i));
        const char* a = word;
        const char* b = w;
        while (*a != '\0' && *a == *b) { ++a; ++b; }
        if (*a == '\0' && *b == '\0') { out = static_cast<Role>(i); return true; }
    }
    return false;
}

// THE CLOSED LISTS the report writer draws its lock words from and the uploader checks them against: the tagged locks' names and the sites a lock is taken from. A
// word that is in neither list is dropped by the uploader, so nothing but these literals can leave the machine.
inline constexpr const char* kLockNames[] = {"plugin-runner", "radio-reporter", "diag-log-file"};
inline constexpr const char* kLockSites[] = {"rebuild", "start", "stop", "clear", "process-iq", "process-audio", "audio-chain", "poll-images", "press", "other"};
inline bool inWordList(const char* w, const char* const* list, std::size_t n) noexcept {
    if (w == nullptr) { return false; }
    for (std::size_t i = 0; i < n; ++i) {
        const char* a = list[i];
        const char* b = w;
        while (*a != '\0' && *a == *b) { ++a; ++b; }
        if (*a == '\0' && *b == '\0') { return true; }
    }
    return false;
}
inline bool isLockName(const char* w) noexcept { return inWordList(w, kLockNames, sizeof(kLockNames) / sizeof(kLockNames[0])); }
inline bool isLockSite(const char* w) noexcept { return inWordList(w, kLockSites, sizeof(kLockSites) / sizeof(kLockSites[0])); }

// Takes a thread's entry out of the table (the thread ended, or its id is about to be reused).
inline void forgetThreadRole(unsigned long tid) noexcept {
    if (tid == 0) { return; }
    for (auto& s : detail::roleTable()) {
        unsigned long expected = tid;
        if (s.tid.compare_exchange_strong(expected, 0, std::memory_order_relaxed)) {
            s.role.store(0, std::memory_order_relaxed);
            return;
        }
    }
}

// Called once by a thread, as it starts (or by the code that knows what a thread is). A thread already in the table has its role replaced. Full table: ignored (the
// thread then reads as `other`).
inline void registerThreadRole(Role role, unsigned long tid) noexcept {
    auto& t = detail::roleTable();
    for (auto& s : t) {
        if (s.tid.load(std::memory_order_relaxed) == tid) {
            s.role.store(static_cast<std::uint8_t>(role), std::memory_order_relaxed);
            return;
        }
    }
    for (auto& s : t) {
        unsigned long expected = 0;
        if (s.tid.compare_exchange_strong(expected, tid, std::memory_order_relaxed)) {
            s.role.store(static_cast<std::uint8_t>(role), std::memory_order_relaxed);
            return;
        }
    }
}
// THE CALLING THREAD'S ENTRY GOES WHEN THE THREAD DOES (review of fix/freezes, finding 7). Every DSP thread start used to add a thread id that nothing ever removed: a long session
// filled the 48 slots, and Windows reuses thread ids, so a thread that never registered inherited a dead thread's role (a report called some helper thread `dsp`). The
// thread_local below clears the slot when the thread ends, so no caller has to remember to.
namespace detail {
struct RoleCleanup {
    unsigned long tid = 0;
    ~RoleCleanup() { forgetThreadRole(tid); }
};
}  // namespace detail
inline void registerThreadRole(Role role) noexcept {
    const unsigned long tid = currentTid();
    registerThreadRole(role, tid);
    thread_local detail::RoleCleanup cleanup;
    cleanup.tid = tid;
}

inline Role roleOfThread(unsigned long tid) noexcept {
    if (tid == 0) { return Role::Other; }
    for (auto& s : detail::roleTable()) {
        if (s.tid.load(std::memory_order_relaxed) == tid) { return static_cast<Role>(s.role.load(std::memory_order_relaxed)); }
    }
    return Role::Other;
}

// TESTS ONLY: forget every registration.
inline void resetRolesForTest() noexcept {
    for (auto& s : detail::roleTable()) {
        s.tid.store(0, std::memory_order_relaxed);
        s.role.store(0, std::memory_order_relaxed);
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------
// THE CONTROL PASS'S PHASE
// ---------------------------------------------------------------------------------------------------------------------------------

enum class PassPhase : std::uint8_t {
    Idle = 0,   // between passes
    Gps,
    Frame,      // the pass's start: commands, the wake, the frame's housekeeping
    Inputs,
    Remote,
    Plugins,
    Patch,
    Publish,
    Mute,
    Workers,
    Audio,
    Extra,      // the added radios
    Schedule,
    Macros,
    Wire,
    Smeter,
    Count
};

inline const char* passPhaseWord(PassPhase p) noexcept {
    switch (p) {
        case PassPhase::Gps: return "gps";
        case PassPhase::Frame: return "frame";
        case PassPhase::Inputs: return "inputs";
        case PassPhase::Remote: return "remote";
        case PassPhase::Plugins: return "plugins";
        case PassPhase::Patch: return "patch";
        case PassPhase::Publish: return "publish";
        case PassPhase::Mute: return "mute";
        case PassPhase::Workers: return "workers";
        case PassPhase::Audio: return "audio";
        case PassPhase::Extra: return "extra";
        case PassPhase::Schedule: return "schedule";
        case PassPhase::Macros: return "macros";
        case PassPhase::Wire: return "wire";
        case PassPhase::Smeter: return "smeter";
        default: return "idle";
    }
}

inline bool passPhaseFromWord(const char* w, PassPhase& out) noexcept {
    if (w == nullptr) { return false; }
    for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(PassPhase::Count); ++i) {
        const char* word = passPhaseWord(static_cast<PassPhase>(i));
        const char* a = word;
        const char* b = w;
        while (*a != '\0' && *a == *b) { ++a; ++b; }
        if (*a == '\0' && *b == '\0') { out = static_cast<PassPhase>(i); return true; }
    }
    return false;
}

inline std::atomic<std::uint8_t>& passPhaseCell() noexcept {
    static std::atomic<std::uint8_t> cell{0};
    return cell;
}
inline void setPassPhase(PassPhase p) noexcept { passPhaseCell().store(static_cast<std::uint8_t>(p), std::memory_order_relaxed); }
inline PassPhase passPhase() noexcept {
    const std::uint8_t v = passPhaseCell().load(std::memory_order_relaxed);
    return v < static_cast<std::uint8_t>(PassPhase::Count) ? static_cast<PassPhase>(v) : PassPhase::Idle;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// LOCK TAGS
// ---------------------------------------------------------------------------------------------------------------------------------

// The site the calling thread is about to take tagged locks from, a static literal ("start", "process-iq"...). Set by a SiteScope; the lock records it when taken.
inline const char*& currentSiteRef() noexcept {
    thread_local const char* site = "other";
    return site;
}
class SiteScope {
public:
    explicit SiteScope(const char* site) noexcept : previous_(currentSiteRef()) { currentSiteRef() = site; }
    ~SiteScope() { currentSiteRef() = previous_; }
    SiteScope(const SiteScope&) = delete;
    SiteScope& operator=(const SiteScope&) = delete;

private:
    const char* previous_;
};

class TaggedMutex;

// What the report writer reads about one tagged lock.
struct LockReading {
    const char* name = nullptr;
    unsigned long holderTid = 0;    // 0 when free
    const char* site = nullptr;     // the holder's site literal
    std::int64_t heldMs = 0;        // how long the holder has held it (0 when free)
    bool waiterFound = false;       // the asked-about thread is waiting for it
};

namespace detail {
// ROOM FOR EVERY TAGGED LOCK THE PRODUCT HAS (review of fix/freezes, finding 7: it was 8 against up to ten, and the last two were silently unregistered, so a freeze that waited
// for them named no lock): one plugin runner per radio (the engine holds a static_assert, engine/receiver.hpp, that this is at least kMaxReceivers + 2), the radio
// reporter's lock and the diagnostics log's.
constexpr std::size_t kMaxTagged = 16;
inline std::array<std::atomic<const TaggedMutex*>, kMaxTagged>& taggedTable() {
    static std::array<std::atomic<const TaggedMutex*>, kMaxTagged> t;
    return t;
}
}  // namespace detail

// A std::mutex that remembers who has it. BasicLockable/Lockable, so std::lock_guard / std::unique_lock work unchanged. NOT recursive, as std::mutex is not.
class TaggedMutex {
public:
    explicit TaggedMutex(const char* name) noexcept : name_(name) {
        for (auto& cell : detail::taggedTable()) {
            const TaggedMutex* expected = nullptr;
            if (cell.compare_exchange_strong(expected, this, std::memory_order_acq_rel)) { break; }
        }
    }
    ~TaggedMutex() {
        for (auto& cell : detail::taggedTable()) {
            const TaggedMutex* expected = this;
            if (cell.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel)) { break; }
        }
    }
    TaggedMutex(const TaggedMutex&) = delete;
    TaggedMutex& operator=(const TaggedMutex&) = delete;

    void lock() {
        const unsigned long me = currentTid();
        std::size_t slot = kWaiters;
        if (!m_.try_lock()) {
            // WAITING: published before the thread blocks, so a report taken while it is blocked can name this lock.
            for (std::size_t i = 0; i < kWaiters; ++i) {
                unsigned long expected = 0;
                if (waiters_[i].compare_exchange_strong(expected, me, std::memory_order_relaxed)) { slot = i; break; }
            }
            m_.lock();
            if (slot < kWaiters) { waiters_[slot].store(0, std::memory_order_relaxed); }
        }
        take(me);
    }
    bool try_lock() {
        if (!m_.try_lock()) { return false; }
        take(currentTid());
        return true;
    }
    void unlock() {
        holder_.store(0, std::memory_order_relaxed);
        m_.unlock();
    }

    const char* name() const noexcept { return name_; }

    // Reads this lock for a report about `tid`: who holds it, from where, for how long, and whether `tid` is waiting for it.
    LockReading read(unsigned long tid) const noexcept {
        LockReading r;
        r.name = name_;
        r.holderTid = holder_.load(std::memory_order_relaxed);
        r.site = site_.load(std::memory_order_relaxed);
        const std::int64_t since = since_.load(std::memory_order_relaxed);
        r.heldMs = (r.holderTid != 0 && since != 0) ? nowMs() - since : 0;
        if (tid != 0) {
            for (const auto& w : waiters_) {
                if (w.load(std::memory_order_relaxed) == tid) { r.waiterFound = true; break; }
            }
        }
        return r;
    }

private:
    static constexpr std::size_t kWaiters = 4;
    void take(unsigned long me) noexcept {
        site_.store(currentSiteRef(), std::memory_order_relaxed);
        since_.store(nowMs(), std::memory_order_relaxed);
        holder_.store(me, std::memory_order_relaxed);
    }
    std::mutex m_;
    const char* name_;
    std::atomic<unsigned long> holder_{0};
    std::atomic<const char*> site_{"other"};
    std::atomic<std::int64_t> since_{0};
    std::array<std::atomic<unsigned long>, kWaiters> waiters_{};
};

// The tagged lock `tid` is waiting for right now, if any (the first found). False when it waits for none.
inline bool lockWaitedBy(unsigned long tid, LockReading& out) noexcept {
    if (tid == 0) { return false; }
    for (auto& cell : detail::taggedTable()) {
        const TaggedMutex* m = cell.load(std::memory_order_acquire);
        if (m == nullptr) { continue; }
        const LockReading r = m->read(tid);
        if (r.waiterFound) { out = r; return true; }
    }
    return false;
}

}  // namespace cascade::core::stall
