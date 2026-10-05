// frame_timing.hpp - how long the window took to draw a frame, and WHICH PART of
// the frame the time went in (0.99.64).
//
// WHY THIS EXISTS. Every freeze reported from the field this month was something
// slow on the thread that draws the window: creating a recording's file, reading
// plugin file sizes every frame, looking in a settings folder once a second, a
// plugin reload. The only detector there was is the hang watchdog, which fires at
// FIVE SECONDS and then needs a stack walk and symbol maps to say where the
// thread was. A frame that takes 300 ms is the same bug at an earlier stage, is
// far more common, and left no trace at all. The aim is to find a freeze while it
// is still a stutter, and to name the cause in fixed words without a stack.
//
// WHAT IT MEASURES. Each pass of the frame loop is timed from its top to its
// bottom with a steady clock. Inside the frame a fixed set of NAMED SCOPES
// (FrameScope below - what the frame really does, in the order it does it) is
// timed with an RAII guard, and the time that is inside no scope is the scope
// `other`, so the scopes always add up to the frame.
//
// WHAT A NESTED SCOPE'S TIME MEANS. A scope's time is EXCLUSIVE: the time spent
// inside a scope that was opened inside it belongs to the inner one and is not
// also in the outer one. That is the only definition under which the scopes still
// sum to the frame, and it is what lets the recording's start and a plugin
// reload - which can be reached from several places in the frame - be their own
// scopes wherever they were called from. FrameScopeGuard::to() moves a guard to a
// different scope WITHOUT nesting, so one guard can walk a function through its
// phases (drawUi does that) and still restore the caller's scope on every way
// out.
//
// A SLOW FRAME IS RECORDED, NOT EVERY FRAME. A frame is slow from 250 ms: fifteen
// missed frames on a 60 Hz display, a hitch anybody sees. The tiers are
//
//     250 ms to under 1 s      a stutter
//     1 s to under 5 s         a freeze the person noticed
//     5 s or more              what the hang watchdog's own threshold would have
//                              reported, had nothing excused it
//
// and a slow frame is counted ONCE, in the highest tier it reached, against the
// scope that took the most of it: table[scope][tier]. So `recorder 2/0/0` is two
// stutters and `plugins-reload 0/1/0` is one freeze, and nothing is counted
// twice. Measured on the development desktop (a scripted run of the unmodified
// window, docs/DIAGNOSTICS.md, "Slow frames") an ordinary frame is 8 ms and the
// worst steady-state one a few tens of ms, so 250 ms is not reachable by a frame
// that is merely busy; the 5 s tier is HangWatchdog::kDefaultThresholdMs, copied
// rather than included (tests/test_frame_timing.cpp holds the two equal).
//
// THE COST ON THE NORMAL PATH. No allocation, no lock, no string formatting. A
// guard is two clock reads (one on entry, one on exit; each also closes the
// interval that was open) and an add into a fixed array, plus a relaxed store of
// the current scope. A fast frame ends with one more clock read and a compare.
// Everything else - finding the dominant scope, the table, the log line - runs
// only for a frame that was already slow.
//
// WHAT MUST NOT COUNT, each held by a test:
//
//  - A FRAME THE APPLICATION IS NOT EXPECTED TO PRESENT: the window iconified or
//    hidden, or inside a display-change grace (gui/present_grace.hpp). The
//    driver's present call can stall for seconds then, and nothing in this
//    program is wrong. A slow frame is HELD for one frame before it is counted,
//    because the stall that a display change causes is in the swap that came
//    BEFORE the window procedure delivers the message that says the display
//    changed: the next frame's first look at the window discards it.
//
//  - TIME A PERSON SETS THE PACE OF. HangWatchdog separates the two kinds of
//    pause by who paces what is waited for, and this follows it exactly. The
//    USER-paced pause (the shell-open bracket, where the wait is somebody reading
//    an elevation or SmartScreen prompt) is `user-wait`: subtracted from the
//    frame and from every scope, never a candidate. The same is true of a modal
//    window loop - dragging or resizing the window, the system menu - which runs
//    inside the message pump and ends when the person lets go (the watchdog's
//    false-positive rule 2). An APPLICATION-paced pause (a plugin reload, an
//    audio or microphone open) is code that is expected to finish; it excuses the
//    WATCHDOG and does not excuse the frame - a plugin reload under its pause IS
//    the freeze this exists to find, so it is a scope of its own
//    (`plugins-reload`) and counts.
//
//  - THE MACHINE SLEEPING. steady_clock on Windows is QueryPerformanceCounter,
//    which keeps counting through a suspend; the awake clock (frameAwakeNanos)
//    does not. A frame whose two clocks disagree by more than a second was
//    spanning a suspend, not slow.
//
//  - START-UP is NOT discarded: the first kStartupFrames frames (the same number
//    as HangWatchdog::kStartupFrames) are known to be long - the GL context's
//    shaders, the font atlas, the first plugin pages - and a slow one is credited
//    to the scope `startup`, with the part that took the time named beside it.
//    What happens BEFORE the first frame (the window, the plugin scan) is not a
//    frame and is not measured here.
//
// THE USAGE RECORD (0.99.64). The process's own timer - and no timer a test builds
// - also counts every slow frame it commits into core/health_events.hpp's ledger
// as `slow.<scope>.<tier>`, the scope's own name and a tier word (`250ms`, `1s`,
// `5s`), in the record the usage report sends (PRIVACY.md, "Failures that are not
// crashes"). `user-wait` is never counted, so it is not a word of it. That is the
// slow path only, on the window's thread, and it waits on no disk.
//
// THE LOG. At most ONE line per scope per kLogGapNs (30 s), so a hitch that
// repeats every second cannot push the rest of the log out of its 256-line ring;
// the lines it did not write are counted in the next one it does. And one summary
// line at shutdown, if any were recorded. Fixed words and numbers only - never a
// plugin's name, a file, a path, a frequency or a device (the log rides inside
// bug reports, PRIVACY.md).
//
// ANOTHER THREAD CAN ASK WHERE THE FRAME IS. currentFrameScope() is one atomic
// the frame loop keeps up to date, readable from any thread without a lock. The
// freeze watchdog reads it when it files a report (the `frame-scope:` line,
// core/hang_watchdog.cpp), so a freeze names the part of the frame that held the
// window without anyone reading a stack. slowFrameCounts() is a snapshot of the
// table, for the usage record.
//
// AND SO CAN ANOTHER PROCESS. The process's one timer (frameTimer()) also writes
// the scope into the sentinel's breadcrumb (core/breadcrumb.hpp, setFrameScope),
// as a number, so an ending nothing inside the process could report - a fast-fail,
// a window frozen and then ended from the taskbar - still says where the frame
// was. A timer a test builds for itself does not: only the process's own one
// describes the window.
//
// ONE THREAD WRITES. The frame loop's thread, and only it, may open a scope or
// begin and end a frame; everything the timer writes besides the atomics is plain
// memory it owns. A guard on any other thread is a data race, so every guard in
// the application sits in code that only the GUI thread runs.
//
// TEST HOOKS (bounded --frames runs only, the same rule as every other hook in
// AppWindow::run): FOXSDR_FRAME_STALL holds the frame loop for some milliseconds
// INSIDE a named scope at a given frame, so that every scope can be made slow in
// the real window and attributed; FOXSDR_FRAME_SITUATION plays the three things
// the operating system does that a test cannot make it do - hide the window,
// change the display, run a modal loop. See parseFrameStalls and
// parseFrameSituations.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_FRAME_TIMING_HPP
#define CASCADE_CORE_FRAME_TIMING_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/breadcrumb.hpp"
#include "core/diag_log.hpp"

namespace cascade::core {

// Nanoseconds on a monotonic clock. The frame timer's own, so a test can say how
// long something took without waiting for it.
std::int64_t frameSteadyNanos();
// Nanoseconds of time the machine spent awake: the same clock without any suspend
// in it (frame_timing.cpp).
std::int64_t frameAwakeNanos();

// WHERE A FRAME'S TIME CAN GO. In the order the frame does things. Every name is
// fixed lower-case words, because the name is what a report carries.
enum class FrameScope : std::uint8_t {
    // The operating system's message pump (glfwPollEvents) - window procedures
    // run here, and so do the modal loops a person starts by dragging the window.
    Events,
    // Everything between the pump and the first widget: a pending theme, language
    // or scale (a font rebuild), both backends' NewFrame, the scripted input of a
    // bounded test run (a script's `sleep` step lands here), ImGui::NewFrame.
    FrameStart,
    // The start of drawUi before anything is drawn: key bindings, the web
    // server's and the plugins' requests, snapshots published for them, the
    // transmitter's tick.
    PreDraw,
    // The plugins' own windows: maps, pictures, instruments, decoder output.
    PluginPanels,
    // The cabinet, its rail and the top bar.
    Toolbar,
    // The menu column: the sections of the rail.
    Rail,
    // The receiver's centre: spectrum, waterfall, and the radar scope that
    // replaces them. Includes taking the spectrum frame from the DSP thread.
    Spectrum,
    // The patch page.
    Patch,
    // The status column.
    Status,
    // Dialogs and menus drawn at the top level.
    Dialogs,
    // The once-a-frame polls of workers and files: sources, sound, updates, the
    // catalogue, telemetry, the tester link, the scanner and airband drivers.
    Polls,
    // Deferred saves: bookmarks, markers and the settings file.
    Saves,
    // ImGui::Render, the GL draw and the torn-off windows' own draw and swap.
    Render,
    // glfwSwapBuffers - on a vsynced window, mostly waiting for the display.
    Present,
    // Starting, stopping and arming a recording.
    Recorder,
    // A plugin reload: unloading every module and loading them again.
    PluginsReload,
    // The first kStartupFrames frames (see above).
    Startup,
    // Time a person sets the pace of (see above). Never counted.
    UserWait,
    // Everything not inside any other scope.
    Other,
    kCount
};

inline constexpr int kFrameScopeCount = static_cast<int>(FrameScope::kCount);

inline constexpr const char* const kFrameScopeNames[kFrameScopeCount] = {
    "events",  "frame-start", "pre-draw", "plugin-panels",  "toolbar", "rail",
    "spectrum", "patch",      "status",   "dialogs",        "polls",   "saves",
    "render",  "present",     "recorder", "plugins-reload", "startup", "user-wait",
    "other"};

inline const char* frameScopeName(FrameScope s) {
    const int i = static_cast<int>(s);
    return (i >= 0 && i < kFrameScopeCount) ? kFrameScopeNames[i] : "?";
}

// The scope with this name, for the test hooks. False for anything else.
bool frameScopeFromName(const char* name, FrameScope& out);

// THE TIERS: where a slow frame starts, and the two steps above it.
inline constexpr int kSlowFrameTiers = 3;
inline constexpr std::int64_t kSlowFrameTierNs[kSlowFrameTiers] = {
    250'000'000LL, 1'000'000'000LL, 5'000'000'000LL};

// Which tier a frame of `ns` is in: -1 when it is not slow, else 0, 1 or 2.
inline int slowFrameTier(std::int64_t ns) {
    int tier = -1;
    for (int t = 0; t < kSlowFrameTiers; ++t) {
        if (ns >= kSlowFrameTierNs[t]) { tier = t; }
    }
    return tier;
}

// A snapshot of the table: [scope][tier], frames counted once each in the
// highest tier they reached.
struct SlowFrameCounts {
    std::uint32_t count[kFrameScopeCount][kSlowFrameTiers] = {};

    std::uint64_t total() const {
        std::uint64_t n = 0;
        for (const auto& row : count) {
            for (std::uint32_t c : row) { n += c; }
        }
        return n;
    }
    bool any() const { return total() != 0; }
};

// THE TABLE AS A BUNDLE VALUE: `recorder 2/0/0, plugins-reload 0/1/0` - the
// scopes that have a count, in the order of FrameScope, each as 250 ms / 1 s /
// 5 s - or `none`.
std::string slowFramesText(const SlowFrameCounts& c);

// THE BUNDLE'S FIELD, inventoried here and not in core::bundleFieldNames(), which
// lives in the report format that is being changed elsewhere (see
// docs/DIAGNOSTICS.md, "Slow frames"). tests/test_diagnostics.cpp compares the
// union of the two lists with what a bundle carries, both ways, and with
// PRIVACY.md.
const std::vector<std::string>& frameBundleFieldNames();

// The bundle with its `slow-frames:` line added at the end of the header block,
// just before the log - where the other lines that are not the context block sit.
std::string withSlowFramesField(std::string bundle, const std::string& value);

// --- test hooks -----------------------------------------------------------------

// FOXSDR_FRAME_STALL="rail=300@45,polls=200@50": hold the frame loop for the
// milliseconds, inside the named scope, at the frame. A malformed item is
// skipped; at most kMaxFrameStalls are kept.
struct FrameStall {
    FrameScope scope = FrameScope::Other;
    long frame = -1;
    int ms = 0;
};
inline constexpr int kMaxFrameStalls = 16;

std::vector<FrameStall> parseFrameStalls(const char* text);

// FOXSDR_FRAME_SITUATION="hidden@40-60,display@100,modal@80": what the operating
// system does that a test cannot make it do. `hidden` plays an iconified window
// for those frames, `display` a display change arriving at that frame, `modal` a
// modal window loop having run inside that frame's message pump.
struct FrameSituations {
    long hiddenFirst = -1;
    long hiddenLast = -1;
    long displayAt = -1;
    long modalAt = -1;

    bool hiddenAt(long frame) const {
        return hiddenFirst >= 0 && frame >= hiddenFirst && frame <= hiddenLast;
    }
    bool displayChangeAt(long frame) const { return displayAt >= 0 && frame == displayAt; }
    bool modalLoopAt(long frame) const { return modalAt >= 0 && frame == modalAt; }
};

FrameSituations parseFrameSituations(const char* text);

// --- the timer ------------------------------------------------------------------

class FrameTimer {
public:
    using ClockFn = std::int64_t (*)();
    // Where a line goes: `warn` is a freeze (a frame of a second or more).
    using SinkFn = void (*)(bool warn, const char* line);

    // One line per scope per this long.
    static constexpr std::int64_t kLogGapNs = 30'000'000'000LL;
    // The first frames are start-up. Copied from HangWatchdog::kStartupFrames.
    static constexpr long kStartupFrames = 30;
    // A frame whose steady clock ran this much further than the awake clock was
    // spanning a suspend.
    static constexpr std::int64_t kSuspendSlackNs = 1'000'000'000LL;

    constexpr FrameTimer() = default;
    FrameTimer(ClockFn steady, ClockFn awake) : steady_(steady), awake_(awake) {}
    // The process's own timer: the one whose scope the sentinel is told.
    struct ProcessTimer {};
    constexpr explicit FrameTimer(ProcessTimer) : mirror_(true) {}

    FrameTimer(const FrameTimer&) = delete;
    FrameTimer& operator=(const FrameTimer&) = delete;

    // ---- the frame ---------------------------------------------------------------

    // The top of a pass through the frame loop. `frameIndex` counts from 0.
    void beginFrame(long frameIndex) {
        frameIndex_ = frameIndex;
        for (std::int64_t& a : acc_) { a = 0; }
        cur_ = FrameScope::Other;
        publish(FrameScope::Other);
        excluded_ = false;
        awakeStart_ = awake_();
        frameStart_ = mark_ = steady_();
    }

    // This frame is not to be counted: the window is not being shown, or a
    // display change is being settled (gui/present_grace.hpp).
    void excludeFrame() { excluded_ = true; }

    // Called once a frame, after the window's state is known. The slow frame the
    // PREVIOUS pass left is counted now - or, when `discard`, dropped: the
    // display changed, or the window stopped being shown, around the swap that
    // made it slow. See the header comment.
    void settlePrevious(bool discard) {
        if (!held_.valid) { return; }
        held_.valid = false;
        if (discard) {
            ++excludedSlow_;
            return;
        }
        commit(held_);
    }

    // The bottom of the pass. A fast frame costs one clock read and a compare.
    void endFrame() {
        switchTo(FrameScope::Other);
        if (stallCount_ > 0) { runStall(FrameScope::Other); }
        const std::int64_t now = steady_();
        acc_[static_cast<int>(FrameScope::Other)] += now - mark_;
        mark_ = now;
        const std::int64_t total = now - frameStart_;
        const std::int64_t counted = total - acc_[static_cast<int>(FrameScope::UserWait)];
        ++frames_;
        sumCountedNs_ += counted;
        if (counted > maxCountedNs_) {
            maxCountedNs_ = counted;
            maxScope_ = dominant(nullptr);
        }
        if (counted < kSlowFrameTierNs[0]) { return; }
        slowFrame(total, counted);
    }

    // The loop is over: counts the frame still held and writes the summary line.
    void finish();

    // ---- scopes ------------------------------------------------------------------

    // Opens `s` inside whatever is open; returns what to restore. The guard's.
    FrameScope enter(FrameScope s) {
        const FrameScope prev = cur_;
        switchTo(s);
        return prev;
    }
    // Closes a scope: back to `prev`.
    void leave(FrameScope prev) { switchTo(prev); }

    // Charges the time since the last switch to the scope that was open and makes
    // `s` the open one. The whole of the hot path.
    void switchTo(FrameScope s) {
        const std::int64_t now = steady_();
        acc_[static_cast<int>(cur_)] += now - mark_;
        mark_ = now;
        cur_ = s;
        publish(s);
        ++switches_;
        if (stallCount_ > 0) { runStall(s); }
    }

    // The USER-paced pause's two ends (gui::ShellPauseHooks). Counted, so an
    // inner pair cannot end an outer one, and never a scope that is counted.
    void userWaitBegin() {
        if (userDepth_++ == 0) { userPrev_ = enter(FrameScope::UserWait); }
    }
    void userWaitEnd() {
        if (userDepth_ > 0 && --userDepth_ == 0) { leave(userPrev_); }
    }

    // The time this frame spent in `s` was a person's, not the program's (a modal
    // window loop inside the message pump): moved to user-wait.
    void moveToUserWait(FrameScope s) {
        if (s == FrameScope::UserWait) { return; }
        // What the open scope has run up since the last switch is not in the
        // totals yet: charge it first, so a move made from INSIDE the scope takes
        // the whole of it.
        const std::int64_t now = steady_();
        acc_[static_cast<int>(cur_)] += now - mark_;
        mark_ = now;
        acc_[static_cast<int>(FrameScope::UserWait)] += acc_[static_cast<int>(s)];
        acc_[static_cast<int>(s)] = 0;
    }

    // ---- reading it --------------------------------------------------------------

    // Which scope the frame is in right now. One relaxed load: any thread, no lock.
    FrameScope currentScope() const {
        return static_cast<FrameScope>(current_.load(std::memory_order_relaxed));
    }

    // The table so far. Any thread.
    SlowFrameCounts counts() const;

    // What the frame in progress (or the last one, until the next begins) has
    // charged to `s` so far. The scopes add up to the frame's wall time.
    std::int64_t scopeNanos(FrameScope s) const { return acc_[static_cast<int>(s)]; }

    // Frames that were slow and were not counted, by any of the exclusions.
    std::uint64_t excludedSlowFrames() const { return excludedSlow_; }
    std::uint64_t framesTimed() const { return frames_; }
    std::uint64_t scopeSwitches() const { return switches_; }
    std::int64_t longestFrameNs() const { return maxCountedNs_; }
    FrameScope longestFrameScope() const { return maxScope_; }

    // `120 frames, mean 8.3 ms, longest 34.5 ms (in startup), 21.4 scope switches a
    // frame; slow frames: none (0 slow frames not counted)` - what a bounded run
    // prints at its end, for the tests and for whoever is measuring.
    std::string summaryText() const;

    // ---- test hooks --------------------------------------------------------------

    void setClocksForTest(ClockFn steady, ClockFn awake) {
        steady_ = steady;
        awake_ = awake;
    }
    // Where lines go; null restores the diagnostic log.
    void setSinkForTest(SinkFn sink) { sink_ = sink; }
    // The stalls of FOXSDR_FRAME_STALL.
    void setStalls(const std::vector<FrameStall>& stalls);
    // Forgets everything counted and every hook; the clocks go back to the real
    // ones.
    void resetForTest();

private:
    struct Held {
        bool valid = false;
        FrameScope scope = FrameScope::Other;
        int tier = 0;
        std::int64_t countedNs = 0;
        std::int64_t scopeNs = 0;
        // For a start-up frame: the part that took the most, named.
        FrameScope part = FrameScope::Other;
        std::int64_t partNs = 0;
    };

    static int idx(FrameScope s) { return static_cast<int>(s); }

    // Makes `s` the scope other threads - and, for the process's own timer, the
    // sentinel - read. The breadcrumb takes FrameScope + 1, so that its zeroed
    // page reads as "no frame has begun".
    void publish(FrameScope s) {
        current_.store(static_cast<std::uint8_t>(s), std::memory_order_relaxed);
        if (mirror_) { breadcrumb::setFrameScope(static_cast<std::uint32_t>(s) + 1u); }
    }

    // The scope that took the most of the frame, excluding user-wait; the first of
    // equals, so the answer does not move with the order of anything. `ns`, when
    // given, receives its time.
    FrameScope dominant(std::int64_t* ns) const;

    // The slow path: runs only for a frame whose counted time reached the first
    // tier. Applies the exclusions, picks the scope, and HOLDS the result until the
    // next frame has looked at the window (settlePrevious).
    void slowFrame(std::int64_t total, std::int64_t counted);

    // Counts a held frame in the table and, within the rate limit, says so.
    void commit(const Held& h);
    void emit(bool warn, const char* line) const;

    // The stall hook: holds the thread inside the scope `s` that has just been
    // entered. Each stall runs once.
    void runStall(FrameScope s);

    static constexpr std::size_t kLineChars = 192;

    ClockFn steady_ = &frameSteadyNanos;
    ClockFn awake_ = &frameAwakeNanos;
    SinkFn sink_ = nullptr;
    // Only frameTimer() - see "AND SO CAN ANOTHER PROCESS" above.
    bool mirror_ = false;

    // The open scope, mirrored for other threads.
    std::atomic<std::uint8_t> current_{static_cast<std::uint8_t>(FrameScope::Other)};
    std::atomic<std::uint32_t> table_[kFrameScopeCount][kSlowFrameTiers] = {};

    // The frame being timed. GUI thread only from here on.
    FrameScope cur_ = FrameScope::Other;
    std::int64_t mark_ = 0;
    std::int64_t frameStart_ = 0;
    std::int64_t awakeStart_ = 0;
    std::int64_t acc_[kFrameScopeCount] = {};
    long frameIndex_ = 0;
    bool excluded_ = false;
    int userDepth_ = 0;
    FrameScope userPrev_ = FrameScope::Other;
    Held held_;

    // The log's rate limit, per scope.
    std::int64_t lastLogNs_[kFrameScopeCount] = {};
    bool logged_[kFrameScopeCount] = {};
    std::uint32_t suppressed_[kFrameScopeCount] = {};

    // What the bounded run prints.
    std::uint64_t frames_ = 0;
    std::uint64_t switches_ = 0;
    std::int64_t sumCountedNs_ = 0;
    std::int64_t maxCountedNs_ = 0;
    FrameScope maxScope_ = FrameScope::Other;
    std::uint64_t recorded_ = 0;
    std::uint64_t excludedSlow_ = 0;

    FrameStall stalls_[kMaxFrameStalls];
    int stallCount_ = 0;
};

// THE ONE TIMER OF THE PROCESS: there is one frame loop, on one thread.
inline FrameTimer g_frameTimer{FrameTimer::ProcessTimer{}};
inline FrameTimer& frameTimer() { return g_frameTimer; }

// Which scope the window's thread is in now. Any thread, no lock.
inline FrameScope currentFrameScope() { return frameTimer().currentScope(); }
// The table so far, for the usage record that will carry it.
inline SlowFrameCounts slowFrameCounts() { return frameTimer().counts(); }

// RAII: `s` is open for the guard's life, and the scope that was open comes back
// when it ends - by any way out of the function.
class FrameScopeGuard {
public:
    explicit FrameScopeGuard(FrameScope s) : t_(frameTimer()), prev_(t_.enter(s)) {}
    FrameScopeGuard(FrameTimer& t, FrameScope s) : t_(t), prev_(t.enter(s)) {}
    ~FrameScopeGuard() { t_.leave(prev_); }
    FrameScopeGuard(const FrameScopeGuard&) = delete;
    FrameScopeGuard& operator=(const FrameScopeGuard&) = delete;

    // Moves this guard to another scope - a flat switch, not a nested one - so one
    // guard can carry a function through its phases. What comes back at the end is
    // still what was open when it began.
    void to(FrameScope s) { t_.switchTo(s); }

private:
    FrameTimer& t_;
    FrameScope prev_;
};

}  // namespace cascade::core

#endif  // CASCADE_CORE_FRAME_TIMING_HPP
