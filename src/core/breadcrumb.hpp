// breadcrumb.hpp - the small block the application keeps up to date so that a
// watcher OUTSIDE the process can say where it was when it ended (0.99.64; the
// design record is docs/DIAGNOSTICS.md, "The sentinel").
//
// WHY IT IS A BLOCK OF SHARED MEMORY AND NOT A FILE OR A LOG LINE. The ending
// this exists for is the one nothing inside the process can report: a fast-fail,
// a stack overflow, a window frozen so hard that the watchdog thread is stuck
// too, a death before the handlers are armed. Whatever the application knew at
// that moment has to already be somewhere the watcher can read it, and the only
// way to be there without writing a file sixty times a second is a page of
// memory both processes map. The watcher reads it AFTER the application has
// exited: the mapping outlives the process for as long as the watcher holds it.
//
// WHAT IT HOLDS, and the rule that keeps it small: ENUMERATED OR NUMERIC VALUES
// ONLY. A phase (a closed list, below), a few bits that say what else was going
// on, a frame count, and four clock readings. Never a string - so never a name,
// a path, a frequency, a radio or an address, and a reader cannot be made to
// print one by a corrupt page. Nothing here is personal and nothing can become
// so without changing the layout, which a test pins field by field.
//
// THE HOT PATH IS ALLOCATION-FREE AND LOCK-FREE. beat() is one steady-clock read
// and two relaxed stores into a page that already exists. Every writer is the
// application's one GUI thread; the few bits another thread could set are changed
// with an atomic read-modify-write, so no update is ever lost.
//
// NO NAME, NO COLLISION. The block is an UNNAMED mapping handed to the watcher as
// an inherited handle (Windows) or file descriptor (Linux): there is no object name
// for another user or another session to collide with or open, and the watcher
// receives exactly this application's block and no other.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_BREADCRUMB_HPP
#define CASCADE_CORE_BREADCRUMB_HPP

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace cascade::core::breadcrumb {

// WHERE THE APPLICATION IS, as a closed, ordered list taken from the code's own
// stages (AppWindow::run and main()), not from a wish list. The order matters:
// "before Running" means "before the first frame was completed", and "from
// ShutdownBegun" means "the frame loop has ended". A value outside the list reads
// as Unset.
enum class Phase : std::uint32_t {
    Unset = 0,
    // The process is up and main() is reading its arguments and configuration.
    Starting = 1,
    // The AppWindow object is being built: the band plan, and the first scan and
    // load of every plugin (rescanPlugins, which also raises kReloadingPlugins).
    BuildingApp = 2,
    // run(): GLFW, the window, the OpenGL context, ImGui and its backends.
    CreatingWindow = 3,
    // The window is shown; the receiver is started, the watchdog armed; the first
    // frame has not yet been completed. A death here is "before the first frame".
    AwaitingFirstFrame = 4,
    // At least one frame has been drawn to the end.
    Running = 5,
    // The frame loop has ended: recordings are closed, the transmitter stopped,
    // the final state saved.
    ShutdownBegun = 6,
    // pipeline_.stop(): the DSP threads, the CAT server, the USB device stack.
    ShutdownStoppingReceiver = 7,
    // patchStopAll() and detachAndUnloadPlugins(): every plugin's destroy().
    ShutdownUnloadingPlugins = 8,
    // The clean-exit marker has been requested and its save is being waited for.
    ShutdownWritingMarker = 9,
    // The marker stage is over: the GL and GLFW teardown, then the watchdog stops.
    ShutdownClosingWindow = 10,
    // run() is about to return; what is left is main() returning and the exit.
    Finished = 11,
};
constexpr std::uint32_t kLastPhase = 11;

// WHAT ELSE IS GOING ON, a set of bits beside the phase because these overlap it
// instead of following it: a radio is opened on a worker while the window is
// drawing, and plugins are rescanned at any time as well as at start-up.
enum Activity : std::uint32_t {
    kOpeningRadio = 1u << 0,      // an asynchronous device open is in flight
    kReloadingPlugins = 1u << 1,  // rescanPlugins() is running on the GUI thread
};

enum Flag : std::uint32_t {
    // Windows has told this process that the session is ending (WM_ENDSESSION,
    // wParam TRUE), which is what a log off or a shutdown looks like from inside.
    kFlagSessionEnding = 1u << 0,
    // The user has switched Diagnostics off. The watcher is also told to go away,
    // but "off means off" does not rest on that one message arriving.
    kFlagReportsOff = 1u << 1,
};

constexpr std::uint32_t kMagic = 0x43425846u;  // "FXBC", little-endian
constexpr std::uint32_t kLayout = 1;

// THE LAYOUT, FIXED AND PINNED BY A TEST (tests/test_sentinel.cpp reads every
// offset below). 64 bytes, one cache line, every field naturally aligned. Plain
// integers read and written through std::atomic_ref, so the struct is exactly
// this and nothing else.
//
//   offset  size  field
//     0      4    magic       kMagic
//     4      4    layout      kLayout
//     8      8    startedMs   steady-clock milliseconds when the block was made
//    16      8    beatMs      steady-clock ms of the last frame's heartbeat (0: none yet)
//    24      8    frames      heartbeats so far
//    32      8    phaseMs     steady-clock ms when `phase` last changed
//    40      4    phase       a Phase
//    44      4    activity    Activity bits
//    48      4    flags       Flag bits
//    52     12    reserved    zero
struct Block {
    std::uint32_t magic;
    std::uint32_t layout;
    std::uint64_t startedMs;
    std::uint64_t beatMs;
    std::uint64_t frames;
    std::uint64_t phaseMs;
    std::uint32_t phase;
    std::uint32_t activity;
    std::uint32_t flags;
    std::uint32_t reserved[3];
};
static_assert(sizeof(Block) == 64, "the breadcrumb is one cache line");
static_assert(offsetof(Block, magic) == 0 && offsetof(Block, layout) == 4 &&
                  offsetof(Block, startedMs) == 8 && offsetof(Block, beatMs) == 16 &&
                  offsetof(Block, frames) == 24 && offsetof(Block, phaseMs) == 32 &&
                  offsetof(Block, phase) == 40 && offsetof(Block, activity) == 44 &&
                  offsetof(Block, flags) == 48 && offsetof(Block, reserved) == 52,
              "the breadcrumb's layout is a contract with the watcher");
static_assert(std::atomic_ref<std::uint64_t>::is_always_lock_free &&
                  std::atomic_ref<std::uint32_t>::is_always_lock_free,
              "the hot path must be lock-free");

// A steady clock the two processes share: the same clock gives the same reading
// in every process on a machine (QueryPerformanceCounter on Windows,
// CLOCK_MONOTONIC on Linux), which is what lets the watcher say how long ago a
// heartbeat was.
inline std::uint64_t nowMs() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// ---------------------------------------------------------------------------
// The writer, in the application. Every call is a no-op until a block has been
// attached, so code that runs in a process with no watcher (a tool mode, a test)
// pays one pointer test.
// ---------------------------------------------------------------------------
inline std::atomic<Block*> g_block{nullptr};

namespace detail {
template <class T>
inline std::atomic_ref<T> at(T& v) noexcept {
    return std::atomic_ref<T>(v);
}
}  // namespace detail

// Takes a ZEROED block (the operating system hands out zeroed pages) and makes it
// this process's breadcrumb, in phase Starting. Replaces any block attached
// before; passing nullptr detaches.
inline void attach(Block* b) noexcept {
    if (b != nullptr) {
        const std::uint64_t now = nowMs();
        detail::at(b->startedMs).store(now, std::memory_order_relaxed);
        detail::at(b->phaseMs).store(now, std::memory_order_relaxed);
        detail::at(b->phase).store(static_cast<std::uint32_t>(Phase::Starting),
                                   std::memory_order_relaxed);
        detail::at(b->layout).store(kLayout, std::memory_order_relaxed);
        // The magic LAST and with release, so a reader that sees it sees the rest.
        detail::at(b->magic).store(kMagic, std::memory_order_release);
    }
    g_block.store(b, std::memory_order_release);
}

inline void setPhase(Phase p) noexcept {
    Block* b = g_block.load(std::memory_order_relaxed);
    if (b == nullptr) { return; }
    if (detail::at(b->phase).load(std::memory_order_relaxed) == static_cast<std::uint32_t>(p)) {
        return;
    }
    detail::at(b->phaseMs).store(nowMs(), std::memory_order_relaxed);
    detail::at(b->phase).store(static_cast<std::uint32_t>(p), std::memory_order_relaxed);
}

// Raises or clears one Activity bit. Cheap enough to call every frame: it reads
// first and writes only when the bit has to change.
inline void setActivity(Activity a, bool on) noexcept {
    Block* b = g_block.load(std::memory_order_relaxed);
    if (b == nullptr) { return; }
    auto word = detail::at(b->activity);
    const bool now = (word.load(std::memory_order_relaxed) & a) != 0;
    if (now == on) { return; }
    if (on) {
        word.fetch_or(a, std::memory_order_relaxed);
    } else {
        word.fetch_and(~static_cast<std::uint32_t>(a), std::memory_order_relaxed);
    }
}

inline void setFlag(Flag f, bool on) noexcept {
    Block* b = g_block.load(std::memory_order_relaxed);
    if (b == nullptr) { return; }
    auto word = detail::at(b->flags);
    if (on) {
        word.fetch_or(f, std::memory_order_relaxed);
    } else {
        word.fetch_and(~static_cast<std::uint32_t>(f), std::memory_order_relaxed);
    }
}

inline void noteSessionEnding() noexcept { setFlag(kFlagSessionEnding, true); }

// THE HEARTBEAT: called once per frame by the GUI thread, from
// HangWatchdog::heartbeat - the same call the freeze watchdog is built on, so
// "silent" means to the watcher exactly what it means to the watchdog. The second
// heartbeat is what ends AwaitingFirstFrame: by then the first frame has been
// drawn to the end, so a death during it is still "before the first frame".
inline void beat() noexcept {
    Block* b = g_block.load(std::memory_order_relaxed);
    if (b == nullptr) { return; }
    detail::at(b->beatMs).store(nowMs(), std::memory_order_relaxed);
    auto frames = detail::at(b->frames);
    const std::uint64_t n = frames.load(std::memory_order_relaxed) + 1;
    frames.store(n, std::memory_order_relaxed);
    if (n == 2 && detail::at(b->phase).load(std::memory_order_relaxed) ==
                      static_cast<std::uint32_t>(Phase::AwaitingFirstFrame)) {
        setPhase(Phase::Running);
    }
}

// ---------------------------------------------------------------------------
// The reader, in the watcher: one coherent-enough copy taken after the
// application has ended, so nothing is racing it.
// ---------------------------------------------------------------------------
struct Snapshot {
    bool valid = false;  // the page was there, had the magic and the layout we know
    std::uint64_t startedMs = 0;
    std::uint64_t beatMs = 0;
    std::uint64_t frames = 0;
    std::uint64_t phaseMs = 0;
    Phase phase = Phase::Unset;
    std::uint32_t activity = 0;
    std::uint32_t flags = 0;
};

inline Snapshot read(const Block* b) noexcept {
    Snapshot s;
    if (b == nullptr) { return s; }
    auto& m = *const_cast<Block*>(b);  // atomic_ref wants a non-const object; nothing is written
    if (detail::at(m.magic).load(std::memory_order_acquire) != kMagic ||
        detail::at(m.layout).load(std::memory_order_relaxed) != kLayout) {
        return s;
    }
    s.valid = true;
    s.startedMs = detail::at(m.startedMs).load(std::memory_order_relaxed);
    s.beatMs = detail::at(m.beatMs).load(std::memory_order_relaxed);
    s.frames = detail::at(m.frames).load(std::memory_order_relaxed);
    s.phaseMs = detail::at(m.phaseMs).load(std::memory_order_relaxed);
    const std::uint32_t p = detail::at(m.phase).load(std::memory_order_relaxed);
    s.phase = (p <= kLastPhase) ? static_cast<Phase>(p) : Phase::Unset;
    s.activity = detail::at(m.activity).load(std::memory_order_relaxed) &
                 (kOpeningRadio | kReloadingPlugins);
    s.flags = detail::at(m.flags).load(std::memory_order_relaxed) &
              (kFlagSessionEnding | kFlagReportsOff);
    return s;
}

// A scope guard for an Activity that is raised for the length of a call.
class ActivityScope {
public:
    explicit ActivityScope(Activity a) noexcept : a_(a) { setActivity(a_, true); }
    ~ActivityScope() { setActivity(a_, false); }
    ActivityScope(const ActivityScope&) = delete;
    ActivityScope& operator=(const ActivityScope&) = delete;

private:
    Activity a_;
};

}  // namespace cascade::core::breadcrumb

#endif  // CASCADE_CORE_BREADCRUMB_HPP
