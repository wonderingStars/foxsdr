// See thread_beats.hpp for what this is, who reads it, and why the budgets are what they are.
//
// THE MEMORY ORDERS, stated because this is cross-thread state and ThreadSanitizer is the judge:
//   - a worker's beat(): `phase` is stored RELAXED, then `lastBeatMs` is stored with RELEASE, and only THEN is a declared wait
//     cleared (RELEASE; deep review of engine/stage4, batch A: the wait used to be cleared first, and a poll between the two
//     stores read "no wait" beside the stamp from BEFORE the wait - hours old for a thread that had sat in an uncapped
//     accept - and called a thread that was waking up a wedge);
//   - the monitor loads `lastBeatMs` with ACQUIRE and `phase` RELAXED afterwards, so a phase it reads is at least as new
//     as the beat it read (it may be newer: a phase is a label, and a newer label for an older beat is still the
//     thread's own); wait() stores `waitSinceMs` RELAXED and then `wait` with RELEASE, and the monitor loads `wait` with
//     ACQUIRE before it reads `waitSinceMs`;
//   - everything else a slot holds (`spec`, `tid`, `live`) is written once, before the slot is published to the registry
//     under the registry's mutex, and never changed;
//   - `reported` and `episodeStartMs` are the monitor's own. They are only touched inside poll(), which is serialised by
//     pollMutex_, so the latch is exact however many watchdogs call it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/thread_beats.hpp"

#include "core/leak_on_purpose.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <mutex>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace cascade::core::beats {

namespace detail {

struct Slot {
    const ThreadSpec* spec = nullptr;
    unsigned long tid = 0;
    std::shared_ptr<const std::atomic<bool>> live;
    std::atomic<std::int64_t> lastBeatMs{0};
    std::atomic<const char*> phase{nullptr};
    std::atomic<const WaitSpec*> wait{nullptr};
    std::atomic<std::int64_t> waitSinceMs{0};
    // The monitor's (serialised by Registry's poll mutex).
    bool reported = false;
    std::int64_t episodeStartMs = 0;
};

}  // namespace detail

namespace {

std::atomic<std::int64_t> g_skewMs{0};
std::atomic<bool> g_frozen{false};
std::atomic<std::int64_t> g_frozenAtMs{0};
std::atomic<std::uint64_t> g_wedgesFound{0};
std::atomic<unsigned> g_budgetOverride[kKindCount] = {};
std::atomic<void (*)()> g_beatGapProbe{nullptr};   // setBeatGapProbeForTest
// When rearm() last ran, on clockMs(): the moment from which every call in flight counts its own time (CallScope::elapsedMs). The machine stopped, and a
// call that spanned the stop was not slow.
std::atomic<std::int64_t> g_lastRearmMs{0};

// THE REGISTRY'S STATE IS LEAKED ON PURPOSE (core/leak_on_purpose.hpp, and the reason 74EEC22D taught: a worker reading statics the C
// runtime has already destroyed). A detached worker - an abandoned disk job, a reader the pipeline gave up on - can finish after
// main() has returned and the static destructors have run, and its Registration's destructor takes this lock and erases from this
// vector. State that is never destroyed cannot be used after it is gone; the cost is a few hundred bytes the process keeps.
struct State {
    std::mutex reg;  // guards slots
    std::vector<std::shared_ptr<detail::Slot>> slots;
    std::mutex poll;  // serialises poll(): the latch is exact
    std::mutex probe;  // guards keyedProbe and keyedOwner
    std::function<bool()> keyedProbe;
    const void* keyedOwner = nullptr;
};

State& state() {
    static State* const s = cascade::core::leakOnPurpose(new State());
    return *s;
}

std::int64_t realMs() {
    const auto t = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(t).count());
}

std::int64_t clockMs() {
    const std::int64_t base = g_frozen.load(std::memory_order_acquire) ? g_frozenAtMs.load(std::memory_order_relaxed) : realMs();
    return base + g_skewMs.load(std::memory_order_relaxed);
}

unsigned long currentTid() {
#if defined(_WIN32)
    return static_cast<unsigned long>(::GetCurrentThreadId());
#elif defined(__linux__)
    return static_cast<unsigned long>(::syscall(SYS_gettid));
#else
    return 0;
#endif
}

// THE CALL TABLE (CallScope): a fixed array of lock-free slots, so a call costs the calling thread a few atomic stores and the
// monitor needs no lock to read them. It is plain static storage with no destructor, so a worker that finishes a call after
// main() has returned finds it still there.
constexpr int kMaxCalls = 128;
struct CallSlot {
    std::atomic<std::uint32_t> state{0};  // 0 free, 1 being filled, 2 live
    std::atomic<std::uint32_t> epoch{0};  // bumped on every entry: the monitor re-reads it to know the slot was not reused
    std::atomic<unsigned long> tid{0};
    std::atomic<const void*> owner{nullptr};
    std::atomic<const char*> subject{nullptr};
    std::atomic<const char*> what{nullptr};
    std::atomic<std::int64_t> startMs{0};
    std::atomic<unsigned> budgetMs{0};
    std::atomic<unsigned> wedgeMs{0};
    std::atomic<std::uint32_t> stages{0};  // bit 1: overrun claimed, bit 2: wedge claimed
};
CallSlot g_calls[kMaxCalls];

unsigned budgetFor(Kind k) {
    const unsigned o = g_budgetOverride[static_cast<std::size_t>(k)].load(std::memory_order_relaxed);
    return o != 0 ? o : budgetMs(k);
}

Finding makeFinding(const detail::Slot& s, std::int64_t stalledMs, unsigned budget, const WaitSpec* w) {
    Finding f;
    f.name = reportWord(s.spec->name);
    const char* ph = s.phase.load(std::memory_order_relaxed);
    f.phase = ph != nullptr ? reportWord(ph) : std::string();
    f.wait = w != nullptr ? reportWord(w->name) : std::string();
    f.tid = s.tid;
    f.stalledMs = stalledMs;
    f.budgetMs = budget;
    return f;
}

}  // namespace

std::string reportWord(const char* s) {
    if (s == nullptr) { return "unknown"; }
    std::string out;
    for (const char* p = s; *p != '\0' && out.size() < 32; ++p) {
        const char c = *p;
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) { return "unknown"; }
        out.push_back(c);
    }
    return out.empty() ? std::string("unknown") : out;
}

std::string reportId(const char* s) {
    if (s == nullptr) { return "unknown"; }
    std::string out;
    for (const char* p = s; *p != '\0'; ++p) {
        if (out.size() >= 40) { break; }
        const char c = *p;
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!ok) { return "unknown"; }
        out.push_back(c);
    }
    return out.empty() ? std::string("unknown") : out;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

Registration::Registration(const ThreadSpec& spec, std::shared_ptr<const std::atomic<bool>> live)
    : slot_(std::make_shared<detail::Slot>()) {
    slot_->spec = &spec;
    slot_->tid = currentTid();
    slot_->live = std::move(live);
    slot_->lastBeatMs.store(clockMs(), std::memory_order_relaxed);
    Registry::instance().add(slot_);
}

Registration::~Registration() { Registry::instance().remove(slot_.get()); }

void Registration::beat(const char* phase) noexcept {
    detail::Slot& s = *slot_;
    if (phase != nullptr) { s.phase.store(phase, std::memory_order_relaxed); }
    // THE BEAT IS STAMPED BEFORE THE WAIT IS CLEARED (deep review of engine/stage4, batch A; docs/engine-stage4.md 6.31). A monitor that looks between the two
    // stores then sees a thread that is still in its wait - judged against the wait's own start and cap, an uncapped one never late - and not a thread with NO
    // wait and the stamp it had BEFORE the wait: for a worker that has sat in a declared accept or job wait for hours that stamp is hours old, and it was
    // reported as a wedge (the second look re-reads the same pair if the worker has not yet run its second store - preempted, or still in its store buffer on
    // arm64). The only reading the new order can give that the old one could not is a wait past its own cap seen while its thread is waking, and that wait WAS late.
    s.lastBeatMs.store(clockMs(), std::memory_order_release);
    if (void (*const probe)() = g_beatGapProbe.load(std::memory_order_relaxed)) { probe(); }   // (tests only: the gap between the two stores)
    if (s.wait.load(std::memory_order_relaxed) != nullptr) { s.wait.store(nullptr, std::memory_order_release); }
}

void Registration::wait(const WaitSpec& w) noexcept {
    detail::Slot& s = *slot_;
    s.waitSinceMs.store(clockMs(), std::memory_order_relaxed);
    s.wait.store(&w, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// CallScope
// ---------------------------------------------------------------------------

CallScope::CallScope(const void* owner, const char* subject, const char* what, unsigned budgetMs, unsigned wedgeMs) noexcept
    : startMs_(clockMs()), budgetMs_(budgetMs), wedgeMs_(wedgeMs) {
    // Start the search at a slot this thread is likely to find free, so two threads entering at once rarely race for one.
    const unsigned long tid = currentTid();
    const int first = static_cast<int>(tid % static_cast<unsigned long>(kMaxCalls));
    for (int n = 0; n < kMaxCalls; ++n) {
        const int i = (first + n) % kMaxCalls;
        CallSlot& c = g_calls[i];
        std::uint32_t expected = 0;
        if (!c.state.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) { continue; }
        c.tid.store(tid, std::memory_order_relaxed);
        c.owner.store(owner, std::memory_order_relaxed);
        c.subject.store(subject, std::memory_order_relaxed);
        c.what.store(what, std::memory_order_relaxed);
        c.startMs.store(startMs_, std::memory_order_relaxed);
        c.budgetMs.store(budgetMs, std::memory_order_relaxed);
        c.wedgeMs.store(wedgeMs, std::memory_order_relaxed);
        c.stages.store(0, std::memory_order_relaxed);
        c.epoch.fetch_add(1, std::memory_order_relaxed);
        c.state.store(2, std::memory_order_release);  // published
        index_ = i;
        return;
    }
    // No room: the call goes ahead, unwatched.
}

CallScope::~CallScope() {
    if (index_ >= 0) { g_calls[index_].state.store(0, std::memory_order_release); }
}

std::int64_t CallScope::elapsedMs() const noexcept {
    // FROM THE LATER OF THE CALL'S START AND THE LAST REARM (deep review of engine/stage4, batch A; docs/engine-stage4.md 6.31). The machine stopped (a sleep, a
    // paused VM) while this call was in flight: rearm() restarted the monitor's copy of the call's clock - the table's slot - so that nothing was reported, but
    // this object's own start was left, and when the call returned, a few minutes later by the clock, its own end-of-call check read the whole sleep as
    // slowness: an overrun, then a wedge, and the plugin was switched off for a laptop's lid.
    return clockMs() - std::max(startMs_, g_lastRearmMs.load(std::memory_order_acquire));
}

bool CallScope::claimStage(int stage) noexcept {
    if (stage != 1 && stage != 2) { return false; }
    const std::uint32_t bit = 1u << stage;
    if (index_ < 0) { return false; }
    const std::uint32_t before = g_calls[index_].stages.fetch_or(bit, std::memory_order_acq_rel);
    return (before & bit) == 0;
}

bool callOfThread(unsigned long tid, const void** owner, const char** what) noexcept {
    for (int i = 0; i < kMaxCalls; ++i) {
        const CallSlot& c = g_calls[i];
        if (c.state.load(std::memory_order_acquire) != 2) { continue; }
        if (c.tid.load(std::memory_order_relaxed) != tid) { continue; }
        if (owner != nullptr) { *owner = c.owner.load(std::memory_order_relaxed); }
        if (what != nullptr) { *what = c.what.load(std::memory_order_relaxed); }
        return true;
    }
    return false;
}

std::size_t liveCallOwners(const void** out, std::size_t cap) noexcept {
    std::size_t n = 0;
    for (int i = 0; i < kMaxCalls && n < cap; ++i) {
        const CallSlot& c = g_calls[i];
        if (c.state.load(std::memory_order_acquire) != 2) { continue; }
        out[n++] = c.owner.load(std::memory_order_relaxed);
    }
    return n;
}

std::size_t callsInFlight() noexcept {
    std::size_t n = 0;
    for (int i = 0; i < kMaxCalls; ++i) {
        if (g_calls[i].state.load(std::memory_order_acquire) == 2) { ++n; }
    }
    return n;
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

Registry& Registry::instance() {
    static Registry r;
    return r;
}

void Registry::add(const std::shared_ptr<detail::Slot>& s) {
    std::lock_guard<std::mutex> lk(state().reg);
    state().slots.push_back(s);
}

void Registry::remove(const detail::Slot* s) {
    std::lock_guard<std::mutex> lk(state().reg);
    state().slots.erase(std::remove_if(state().slots.begin(), state().slots.end(),
                                 [s](const std::shared_ptr<detail::Slot>& p) { return p.get() == s; }),
                  state().slots.end());
}

std::size_t Registry::size() const {
    std::lock_guard<std::mutex> lk(state().reg);
    return state().slots.size();
}

PollResult Registry::poll() {
    // Serialised: the latch below is exact however many watchdogs poll. A poller that arrives while another is looking
    // waits a few microseconds; nothing a worker does ever waits on this.
    std::lock_guard<std::mutex> poller(state().poll);
    std::vector<std::shared_ptr<detail::Slot>> copy;
    {
        std::lock_guard<std::mutex> lk(state().reg);
        copy = state().slots;
    }
    PollResult out;
    const std::int64_t now = clockMs();
    for (const auto& sp : copy) {
        detail::Slot& s = *sp;
        // A generation its owner abandoned and counted is not this table's business.
        if (s.live != nullptr && !s.live->load(std::memory_order_relaxed)) { continue; }

        // What the thread is doing, read in the order the writer published it (see the top of this file).
        const WaitSpec* w = s.wait.load(std::memory_order_acquire);
        std::int64_t since = 0;
        unsigned budget = 0;
        bool unlimited = false;
        if (w != nullptr) {
            since = s.waitSinceMs.load(std::memory_order_relaxed);
            budget = w->capMs;
            unlimited = (w->capMs == 0);
        } else {
            since = s.lastBeatMs.load(std::memory_order_acquire);
            budget = budgetFor(s.spec->kind);
        }
        const std::int64_t stalled = unlimited ? 0 : now - since;
        const bool late = !unlimited && stalled >= static_cast<std::int64_t>(budget);

        if (late) {
            // Look again before accusing: a thread that beat, or left its wait, between the reads above is alive. Only a
            // reading that is unchanged is a wedge.
            const WaitSpec* w2 = s.wait.load(std::memory_order_acquire);
            const std::int64_t since2 = (w2 != nullptr) ? s.waitSinceMs.load(std::memory_order_relaxed)
                                                       : s.lastBeatMs.load(std::memory_order_acquire);
            if (w2 != w || since2 != since) { continue; }
            if (!s.reported) {
                s.reported = true;
                s.episodeStartMs = since;
                out.wedged.push_back(makeFinding(s, stalled, budget, w));
                g_wedgesFound.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (s.reported) {
            s.reported = false;
            out.recovered.push_back(makeFinding(s, now - s.episodeStartMs, budget, w));
        }
    }

    // THE CALLS IN FLIGHT. A call is accused only on a reading that is unchanged (the slot was not freed and reused between the reads),
    // and each stage of each call is claimed - and so reported - once, by whoever gets there first.
    for (int i = 0; i < kMaxCalls; ++i) {
        CallSlot& c = g_calls[i];
        if (c.state.load(std::memory_order_acquire) != 2) { continue; }
        const std::uint32_t epoch = c.epoch.load(std::memory_order_relaxed);
        const std::int64_t start = c.startMs.load(std::memory_order_relaxed);
        const unsigned budget = c.budgetMs.load(std::memory_order_relaxed);
        const unsigned wedge = c.wedgeMs.load(std::memory_order_relaxed);
        const std::int64_t age = now - start;
        const int stage = (wedge != 0 && age >= static_cast<std::int64_t>(wedge)) ? 2
                          : (budget != 0 && age >= static_cast<std::int64_t>(budget)) ? 1 : 0;
        if (stage == 0) { continue; }
        Finding f;
        f.name = "plugin-call";
        const char* what = c.what.load(std::memory_order_relaxed);
        f.phase = what != nullptr ? reportWord(what) : std::string("unknown");
        f.tid = c.tid.load(std::memory_order_relaxed);
        f.stalledMs = age;
        f.owner = c.owner.load(std::memory_order_relaxed);
        f.subject = reportId(c.subject.load(std::memory_order_relaxed));
        if (c.state.load(std::memory_order_acquire) != 2 || c.epoch.load(std::memory_order_relaxed) != epoch) { continue; }
        // Stage 2 implies stage 1 was reached: a call first seen past its wedge limit reports (and counts) both, in order.
        for (int st = 1; st <= stage; ++st) {
            const std::uint32_t bit = 1u << st;
            if ((c.stages.fetch_or(bit, std::memory_order_acq_rel) & bit) != 0) { continue; }
            Finding f2 = f;
            f2.stage = st;
            f2.budgetMs = st == 2 ? wedge : budget;
            out.calls.push_back(std::move(f2));
        }
    }
    return out;
}

void Registry::rearm() {
    std::vector<std::shared_ptr<detail::Slot>> copy;
    {
        std::lock_guard<std::mutex> lk(state().reg);
        copy = state().slots;
    }
    const std::int64_t now = clockMs();
    // BEFORE the slots are rewritten: a call that returns while this runs measures from here, as the table's copy of it will from the next line.
    g_lastRearmMs.store(now, std::memory_order_release);
    for (const auto& s : copy) {
        s->lastBeatMs.store(now, std::memory_order_release);
        s->waitSinceMs.store(now, std::memory_order_relaxed);
    }
    // A call in flight when the machine stopped was not slow: its clock starts again.
    for (int i = 0; i < kMaxCalls; ++i) {
        if (g_calls[i].state.load(std::memory_order_acquire) == 2) { g_calls[i].startMs.store(now, std::memory_order_relaxed); }
    }
}

std::string Registry::snapshotText() {
    std::vector<std::shared_ptr<detail::Slot>> copy;
    {
        std::lock_guard<std::mutex> lk(state().reg);
        copy = state().slots;
    }
    const std::int64_t now = clockMs();
    std::string out;
    for (const auto& s : copy) {
        const WaitSpec* w = s->wait.load(std::memory_order_acquire);
        const char* ph = s->phase.load(std::memory_order_relaxed);
        char line[160];
        std::snprintf(line, sizeof(line), "  %s age-ms=%lld phase=%s wait=%s\n", reportWord(s->spec->name).c_str(),
                      static_cast<long long>(now - s->lastBeatMs.load(std::memory_order_acquire)),
                      ph != nullptr ? reportWord(ph).c_str() : "none",
                      w != nullptr ? reportWord(w->name).c_str() : "none");
        out += line;
    }
    // The calls in flight, one line each: which plugin, through which entry, for how long (no thread name and no path - the
    // subject is the public id).
    for (int i = 0; i < kMaxCalls; ++i) {
        const CallSlot& c = g_calls[i];
        if (c.state.load(std::memory_order_acquire) != 2) { continue; }
        char line[160];
        const char* what = c.what.load(std::memory_order_relaxed);
        std::snprintf(line, sizeof(line), "  call %s %s age-ms=%lld\n", reportId(c.subject.load(std::memory_order_relaxed)).c_str(),
                      what != nullptr ? reportWord(what).c_str() : "unknown",
                      static_cast<long long>(now - c.startMs.load(std::memory_order_relaxed)));
        out += line;
    }
    return out;
}

namespace {
}  // namespace

void Registry::setKeyedProbe(const void* owner, std::function<bool()> probe) {
    std::lock_guard<std::mutex> lk(state().probe);
    state().keyedProbe = std::move(probe);
    state().keyedOwner = owner;
}

void Registry::clearKeyedProbe(const void* owner) {
    std::lock_guard<std::mutex> lk(state().probe);
    if (state().keyedOwner != owner) { return; }
    state().keyedProbe = nullptr;
    state().keyedOwner = nullptr;
}

int Registry::keyedNow() {
    std::lock_guard<std::mutex> lk(state().probe);
    if (!state().keyedProbe) { return -1; }
    return state().keyedProbe() ? 1 : 0;
}

std::uint64_t Registry::wedgesFound() { return g_wedgesFound.load(std::memory_order_relaxed); }

void Registry::setBudgetOverrideForTest(Kind k, unsigned ms) {
    g_budgetOverride[static_cast<std::size_t>(k)].store(ms, std::memory_order_relaxed);
}

void Registry::clearOverridesForTest() {
    for (auto& o : g_budgetOverride) { o.store(0, std::memory_order_relaxed); }
}

void Registry::freezeClockForTest(bool frozen) {
    if (frozen) {
        g_frozenAtMs.store(realMs(), std::memory_order_relaxed);
        g_frozen.store(true, std::memory_order_release);
    } else {
        // the skew absorbs the time the clock stood still, so the clock never goes backwards across an unfreeze
        if (g_frozen.exchange(false, std::memory_order_acq_rel)) {
            g_skewMs.fetch_add(g_frozenAtMs.load(std::memory_order_relaxed) - realMs(), std::memory_order_relaxed);
        }
    }
}

void Registry::advanceClockForTest(std::int64_t ms) { g_skewMs.fetch_add(ms, std::memory_order_relaxed); }

void Registry::setBeatGapProbeForTest(void (*probe)()) { g_beatGapProbe.store(probe, std::memory_order_relaxed); }

}  // namespace cascade::core::beats
