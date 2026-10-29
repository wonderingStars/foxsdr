// See thread_beats.hpp for what this is, who reads it, and why the budgets are what they are.
//
// THE MEMORY ORDERS, stated because this is cross-thread state and ThreadSanitizer is the judge:
//   - a worker's beat(): `phase` is stored RELAXED and then `lastBeatMs` is stored with RELEASE;
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
    // The wait is cleared BEFORE the beat is stamped: a monitor that still sees the wait sees a wait that is ending, and
    // one that sees the stamp sees no wait.
    if (s.wait.load(std::memory_order_relaxed) != nullptr) { s.wait.store(nullptr, std::memory_order_relaxed); }
    s.lastBeatMs.store(clockMs(), std::memory_order_release);
}

void Registration::wait(const WaitSpec& w) noexcept {
    detail::Slot& s = *slot_;
    s.waitSinceMs.store(clockMs(), std::memory_order_relaxed);
    s.wait.store(&w, std::memory_order_release);
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
    return out;
}

void Registry::rearm() {
    std::vector<std::shared_ptr<detail::Slot>> copy;
    {
        std::lock_guard<std::mutex> lk(state().reg);
        copy = state().slots;
    }
    const std::int64_t now = clockMs();
    for (const auto& s : copy) {
        s->lastBeatMs.store(now, std::memory_order_release);
        s->waitSinceMs.store(now, std::memory_order_relaxed);
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

}  // namespace cascade::core::beats
