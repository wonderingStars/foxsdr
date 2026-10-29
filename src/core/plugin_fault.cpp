// See plugin_fault.hpp for the contract. This file is the guard, the slot table, the counters and the crash-path attribution.
//
// THE MEMORY MODEL, because the crash path and the hang watchdog's thread read what ordinary threads write:
//   - a Slot is allocated once under g_create (never freed) and published into g_slots with a RELEASE store of g_count; a reader
//     takes g_count with ACQUIRE and then reads only slots below it, so everything written before the publish is visible;
//   - a slot's id and hashed id are immutable after the publish; its public-id pointer is an atomic that selects between them;
//   - quarantine, counters, origin and the module range are atomics; the version string is a fixed array written under g_identity (rare:
//     once a load) and read, racily and only ever for a report, by the crash path - a torn version in a crash report is the cost, and
//     the field is bounded and NUL-terminated either way;
//   - the event queue is a vector behind g_events; nothing holds it across a plugin call.
// The state is leaked on purpose (core/leak_on_purpose.hpp): a worker that finishes a plugin call after main() has returned must find
// its slot still there.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/plugin_fault.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "core/crash_handler.hpp"
#include "core/diag_log.hpp"
#include "core/leak_on_purpose.hpp"
#include "core/plugin_run.hpp"
#include "core/thread_beats.hpp"
#include "source/vendor_guard.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace cascade::core::fault {

namespace {

constexpr std::size_t kMaxSlots = 256;
constexpr std::size_t kMaxEvents = 256;
constexpr std::size_t kIdBytes = 48;
constexpr std::size_t kVersionBytes = 24;

}  // namespace

struct Slot {
    char id[kIdBytes] = {0};
    char hashed[24] = {0};
    std::atomic<const char*> pub{nullptr};
    std::atomic<std::uint8_t> why{0};
    std::atomic<std::uint8_t> entry{0};
    std::atomic<std::uint8_t> lastEntry{0};
    std::atomic<std::uint8_t> origin{0};
    std::atomic<bool> pinned{false};
    char version[kVersionBytes] = {0};
    std::atomic<std::uintptr_t> modBase{0};
    std::atomic<std::uintptr_t> modSize{0};
    std::atomic<std::uint64_t> faults{0};
    std::atomic<std::uint64_t> quarantines{0};
    std::atomic<std::uint64_t> overruns{0};
    std::atomic<std::uint64_t> wedges{0};
    std::atomic<std::uint64_t> unloadTimeouts{0};
};

namespace {

struct State {
    std::mutex create;    // slot creation
    std::mutex identity;  // setIdentity's writes of a slot's version
    std::mutex events;
    std::vector<Event> queue;
    std::atomic<Slot*> slots[kMaxSlots] = {};
    std::atomic<std::size_t> count{0};
    Slot shared;  // the slot for an empty key: never quarantined, never counted
    std::atomic<std::uint64_t> faults{0};
    std::atomic<std::uint64_t> quarantines{0};
    std::atomic<std::uint64_t> overruns{0};
    std::atomic<std::uint64_t> wedges{0};
    std::atomic<unsigned> budgetOverride[kEntryCount] = {};
};

// Published once the state exists, so the CRASH path can ask "is there anything to attribute?" without constructing it (a first-use
// allocation inside a fault handler is exactly what that path must never do).
std::atomic<State*> g_published{nullptr};

State& state() {
    static State* const s = [] {
        State* made = cascade::core::leakOnPurpose(new State());
        g_published.store(made, std::memory_order_release);
        return made;
    }();
    return *s;
}

// FNV-1a, 64 bit, shown as the low 32 bits: "site-1a2b3c4d". Stable across machines, so a fleet's rows for one third-party plugin join,
// and not reversible by anyone who does not already know the id.
std::uint64_t fnv1a(const char* s) {
    std::uint64_t h = 1469598103934665603ull;
    for (; *s != '\0'; ++s) { h = (h ^ static_cast<unsigned char>(*s)) * 1099511628211ull; }
    return h;
}

std::string normaliseId(const std::string& key) {
    std::string id = pluginRunId(key);
    for (char& c : id) {
        if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }
    }
    if (id.size() >= kIdBytes) { id.resize(kIdBytes - 1); }
    return id;
}

Slot* find(const char* id) {
    State& st = state();
    const std::size_t n = st.count.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < n; ++i) {
        Slot* s = st.slots[i].load(std::memory_order_relaxed);
        if (std::strcmp(s->id, id) == 0) { return s; }
    }
    return nullptr;
}

bool isSlotPointer(const void* p) {
    if (p == nullptr) { return false; }
    State& st = state();
    if (p == &st.shared) { return true; }
    const std::size_t n = st.count.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < n; ++i) {
        if (st.slots[i].load(std::memory_order_relaxed) == p) { return true; }
    }
    return false;
}

void push(Event&& e) {
    State& st = state();
    std::lock_guard<std::mutex> lk(st.events);
    if (st.queue.size() >= kMaxEvents) { st.queue.erase(st.queue.begin()); }
    st.queue.push_back(std::move(e));
}

unsigned effectiveBudget(Entry e) {
    const unsigned o = state().budgetOverride[static_cast<std::size_t>(e)].load(std::memory_order_relaxed);
    return o != 0 ? o : budgetMs(e);
}

Event makeEvent(const Slot* s, Event::Kind kind, Entry entry, Why why, unsigned long code) {
    Event e;
    e.kind = kind;
    e.id = s->id;
    e.publicId = s->pub.load(std::memory_order_relaxed);
    e.entry = entry;
    e.why = why;
    e.code = code;
    return e;
}

bool quarantineSlot(Slot* s, Why why, Entry entry) {
    State& st = state();
    if (s == &st.shared) { return false; }  // the key-less slot belongs to no plugin
    std::uint8_t none = 0;
    if (!s->why.compare_exchange_strong(none, static_cast<std::uint8_t>(why), std::memory_order_acq_rel)) { return false; }
    s->entry.store(static_cast<std::uint8_t>(entry), std::memory_order_relaxed);
    s->pinned.store(true, std::memory_order_release);
    s->quarantines.fetch_add(1, std::memory_order_relaxed);
    st.quarantines.fetch_add(1, std::memory_order_relaxed);
    push(makeEvent(s, Event::Kind::Quarantine, entry, why, 0));
    diagWarnf("plugin: %s is switched off for this session (%s in %s)", s->id, whyWord(why), entryWord(entry));
    return true;
}

void countFault(Slot* s, Entry entry, unsigned long code) {
    State& st = state();
    if (s != &st.shared) {
        s->faults.fetch_add(1, std::memory_order_relaxed);
        st.faults.fetch_add(1, std::memory_order_relaxed);
    }
    push(makeEvent(s, Event::Kind::Fault, entry, Why::Fault, code));
    diagWarnf("plugin: %s faulted in %s (code 0x%08lX); the fault was absorbed and the session carries on", s->id, entryWord(entry),
              code);
}

void countOverrun(Slot* s, Entry entry) {
    State& st = state();
    if (s != &st.shared) {
        s->overruns.fetch_add(1, std::memory_order_relaxed);
        st.overruns.fetch_add(1, std::memory_order_relaxed);
    }
    push(makeEvent(s, Event::Kind::Overrun, entry, Why::None, 0));
    diagWarnf("plugin: %s overran its budget in %s (%u ms)", s->id, entryWord(entry), effectiveBudget(entry));
}

void countWedge(Slot* s, Entry entry) {
    State& st = state();
    if (s != &st.shared) {
        s->wedges.fetch_add(1, std::memory_order_relaxed);
        st.wedges.fetch_add(1, std::memory_order_relaxed);
    }
    push(makeEvent(s, Event::Kind::Wedge, entry, Why::Wedge, 0));
    diagWarnf("plugin: %s did not return from %s within %u ms", s->id, entryWord(entry), effectiveBudget(entry) * kWedgeFactor);
    (void)quarantineSlot(s, Why::Wedge, entry);
}

#if defined(_WIN32)

// THE REASON OF AN ABSORBED PLUGIN FAULT'S REPORT: a literal in static storage, because the filter that passes it runs on a fault path.
constexpr char kAbsorbedPluginReason[] =
    "fault in a plugin, absorbed by the plugin-call guard (the process continued; the plugin is switched off for this session)";
static_assert(reasonStartsWith(kAbsorbedPluginReason, kAbsorbedPluginFaultReasonPrefix),
              "the absorbed-plugin-fault reason must begin with core::kAbsorbedPluginFaultReasonPrefix");

// The filter. It absorbs what source/vendor_guard.cpp absorbs (the same codes, at an address in an image that is not this program's own
// - one predicate, so the two guards cannot disagree about what is "theirs") and files the fault before it unwinds, where the exception
// information is still valid. A fault it declines goes to the crash handler, and kills the process with the ordinary report.
int pluginFaultFilter(EXCEPTION_POINTERS* ep, unsigned long* codeOut) noexcept {
    if (ep == nullptr || ep->ExceptionRecord == nullptr) { return EXCEPTION_CONTINUE_SEARCH; }
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    const void* addr = ep->ExceptionRecord->ExceptionAddress;
    if (!cascade::source::vendorGuardAbsorbs(static_cast<std::uint32_t>(code), addr)) { return EXCEPTION_CONTINUE_SEARCH; }
    *codeOut = static_cast<unsigned long>(code);
    cascade::core::reportAbsorbedFault(kAbsorbedPluginReason, static_cast<unsigned long>(code), addr, ep);
    return EXCEPTION_EXECUTE_HANDLER;
}

// MSVC rejects __try in a frame that needs C++ object unwinding (C2712): nothing but pointers lives here.
bool protectedCall(void (*fn)(void*) noexcept, void* ctx, unsigned long* code) noexcept {
    __try {
        fn(ctx);
        return true;
    } __except (pluginFaultFilter(GetExceptionInformation(), code)) {
        return false;
    }
}

#else

bool protectedCall(void (*fn)(void*) noexcept, void* ctx, unsigned long* code) noexcept {
    // POSIX cannot return from a SIGSEGV handler into the faulting call: the plugin runs unguarded (plugin_fault.hpp, the platform caveat).
    (void)code;
    fn(ctx);
    return true;
}

#endif

}  // namespace

const char* originWord(Origin o) {
    switch (o) {
        case Origin::Official: return "official";
        case Origin::Site: return "site";
        case Origin::Unrecorded: break;
    }
    return "unrecorded";
}

const char* whyWord(Why w) {
    switch (w) {
        case Why::Fault: return "fault";
        case Why::Exception: return "exception";
        case Why::Wedge: return "wedge";
        case Why::None: break;
    }
    return "none";
}

Slot* slotFor(const std::string& pluginKeyOrId) {
    State& st = state();
    const std::string id = normaliseId(pluginKeyOrId);
    if (id.empty()) { return &st.shared; }
    if (Slot* s = find(id.c_str())) { return s; }
    std::lock_guard<std::mutex> lk(st.create);
    if (Slot* s = find(id.c_str())) { return s; }
    const std::size_t n = st.count.load(std::memory_order_relaxed);
    if (n >= kMaxSlots) { return &st.shared; }  // 256 distinct plugins: the table is full; the rest share the key-less slot
    Slot* s = new Slot();
    std::memcpy(s->id, id.c_str(), id.size());
    std::snprintf(s->hashed, sizeof(s->hashed), "site-%08x", static_cast<unsigned>(fnv1a(s->id) & 0xFFFFFFFFu));
    s->pub.store(s->hashed, std::memory_order_relaxed);  // until the origin is known, nothing leaves the machine under its real name
    st.slots[n].store(cascade::core::leakOnPurpose(s), std::memory_order_relaxed);
    st.count.store(n + 1, std::memory_order_release);
    return s;
}

const char* idOf(const Slot* s) { return s != nullptr ? s->id : ""; }
const char* publicIdOf(const Slot* s) { return s != nullptr ? s->pub.load(std::memory_order_relaxed) : ""; }

void setIdentity(const std::string& pluginKeyOrId, const std::string& version, Origin origin, const void* moduleBase,
                 std::size_t moduleSize) {
    Slot* s = slotFor(pluginKeyOrId);
    if (s == &state().shared) { return; }
    {
        std::lock_guard<std::mutex> lk(state().identity);
        std::memset(s->version, 0, sizeof(s->version));
        std::memcpy(s->version, version.data(), std::min(version.size(), sizeof(s->version) - 1));
    }
    s->origin.store(static_cast<std::uint8_t>(origin), std::memory_order_relaxed);
    // An OFFICIAL plugin's id is the catalogue's and public; any other is the user's own (a hand-copied file) or a stranger's (a site) and
    // is hashed wherever it leaves the machine.
    s->pub.store(origin == Origin::Official ? s->id : s->hashed, std::memory_order_relaxed);
    s->modSize.store(moduleSize, std::memory_order_relaxed);
    s->modBase.store(reinterpret_cast<std::uintptr_t>(moduleBase), std::memory_order_release);
}

void clearModule(const std::string& pluginKeyOrId) {
    Slot* s = slotFor(pluginKeyOrId);
    if (s == &state().shared) { return; }
    s->modBase.store(0, std::memory_order_release);
    s->modSize.store(0, std::memory_order_relaxed);
}

namespace detail {

Outcome run(Slot* s, Entry e, void (*fn)(void*) noexcept, void* ctx) noexcept {
    if (s == nullptr) { s = &state().shared; }
    if (s->why.load(std::memory_order_acquire) != 0) { return Outcome::Refused; }
    s->lastEntry.store(static_cast<std::uint8_t>(e), std::memory_order_relaxed);

    const unsigned budget = effectiveBudget(e);
    const unsigned wedge = budget * kWedgeFactor;
    unsigned long code = 0;
    bool ok = false;
    {
        // The existing monitor names it: the declared call is in the table the hang watchdog's poll reads.
        cascade::core::beats::CallScope scope(s, s->pub.load(std::memory_order_relaxed), entryWord(e), budget, wedge);
        ok = protectedCall(fn, ctx, &code);
        // A LATE RETURN the monitor never saw (no watchdog in this process, or it had not looked yet) is counted here; a stage the
        // monitor claimed while the call was in flight is not counted twice.
        const std::int64_t took = scope.elapsedMs();
        if (took >= static_cast<std::int64_t>(budget) && scope.claimStage(1)) { countOverrun(s, e); }
        if (took >= static_cast<std::int64_t>(wedge) && scope.claimStage(2)) { countWedge(s, e); }
    }
    if (!ok) {
        countFault(s, e, code);
        (void)quarantineSlot(s, Why::Fault, e);
        return Outcome::Faulted;
    }
    return Outcome::Ran;
}

Outcome noteThrown(Slot* s, Entry e) noexcept {
    if (s == nullptr) { s = &state().shared; }
    countFault(s, e, 0xE06D7363ul);
    (void)quarantineSlot(s, Why::Exception, e);
    return Outcome::Faulted;
}

}  // namespace detail

Why quarantineWhy(const Slot* s) {
    return s != nullptr ? static_cast<Why>(s->why.load(std::memory_order_acquire)) : Why::None;
}

Entry quarantineEntry(const Slot* s) {
    return s != nullptr ? static_cast<Entry>(s->entry.load(std::memory_order_relaxed)) : Entry::Create;
}

bool isPinned(const Slot* s) { return s != nullptr && s->pinned.load(std::memory_order_acquire); }

bool isQuarantinedKey(const std::string& pluginKeyOrId) {
    const std::string id = normaliseId(pluginKeyOrId);
    if (id.empty()) { return false; }
    const Slot* s = find(id.c_str());
    return s != nullptr && quarantineWhy(s) != Why::None;
}

void noteFault(Slot* s, Entry e, unsigned long code) {
    if (s == nullptr) { return; }
    countFault(s, e, code);
    (void)quarantineSlot(s, Why::Fault, e);
}

bool quarantine(Slot* s, Why why, Entry entry) {
    if (s == nullptr || why == Why::None) { return false; }
    return quarantineSlot(s, why, entry);
}

bool release(Slot* s) {
    if (s == nullptr || s == &state().shared) { return false; }
    std::uint8_t was = s->why.exchange(0, std::memory_order_acq_rel);
    if (was != 0) { diagLogf("plugin: %s is switched on again (re-enabled by the user)", s->id); }
    return was != 0;
}

std::vector<Event> drainEvents() {
    State& st = state();
    std::lock_guard<std::mutex> lk(st.events);
    std::vector<Event> out;
    out.swap(st.queue);
    return out;
}

Totals totals() {
    const State& st = state();
    Totals t;
    t.faults = st.faults.load(std::memory_order_relaxed);
    t.quarantines = st.quarantines.load(std::memory_order_relaxed);
    t.wedges = st.wedges.load(std::memory_order_relaxed);
    t.overruns = st.overruns.load(std::memory_order_relaxed);
    return t;
}

std::vector<HealthRow> health() {
    State& st = state();
    std::vector<HealthRow> out;
    const std::size_t n = st.count.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < n; ++i) {
        const Slot* s = st.slots[i].load(std::memory_order_relaxed);
        HealthRow r;
        r.id = s->id;
        r.publicId = s->pub.load(std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lk(st.identity);
            r.version = s->version;
        }
        r.origin = static_cast<Origin>(s->origin.load(std::memory_order_relaxed));
        r.faults = s->faults.load(std::memory_order_relaxed);
        r.quarantines = s->quarantines.load(std::memory_order_relaxed);
        r.overruns = s->overruns.load(std::memory_order_relaxed);
        r.wedges = s->wedges.load(std::memory_order_relaxed);
        r.unloadTimeouts = s->unloadTimeouts.load(std::memory_order_relaxed);
        r.quarantined = static_cast<Why>(s->why.load(std::memory_order_relaxed));
        r.lastEntry = static_cast<Entry>(s->lastEntry.load(std::memory_order_relaxed));
        out.push_back(std::move(r));
    }
    return out;
}

void noteMonitorFinding(const void* owner, int stage, const char* entryWordText) {
    if (!isSlotPointer(owner)) { return; }
    Slot* s = const_cast<Slot*>(static_cast<const Slot*>(owner));
    Entry e = static_cast<Entry>(s->lastEntry.load(std::memory_order_relaxed));
    // The finding names the entry by its report word; the slot's last entry is the same call unless two calls of one plugin overlap, in
    // which case the word wins.
    if (entryWordText != nullptr) {
        for (const EntrySpec& sp : kEntries) {
            if (std::strcmp(sp.word, entryWordText) == 0) { e = sp.entry; }
        }
    }
    if (stage == 1) {
        countOverrun(s, e);
    } else if (stage == 2) {
        countWedge(s, e);
    }
}

std::size_t noteUnloadTimeout() {
    const void* owners[64];
    const std::size_t n = cascade::core::beats::liveCallOwners(owners, 64);
    std::size_t charged = 0;
    for (std::size_t i = 0; i < n; ++i) {
        // Once per plugin, however many of its calls are in flight.
        bool seen = false;
        for (std::size_t j = 0; j < i; ++j) { seen = seen || owners[j] == owners[i]; }
        if (seen || !isSlotPointer(owners[i])) { continue; }
        Slot* s = const_cast<Slot*>(static_cast<const Slot*>(owners[i]));
        if (s == &state().shared) { continue; }
        s->unloadTimeouts.fetch_add(1, std::memory_order_relaxed);
        s->wedges.fetch_add(1, std::memory_order_relaxed);
        state().wedges.fetch_add(1, std::memory_order_relaxed);
        push(makeEvent(s, Event::Kind::UnloadTimeout, static_cast<Entry>(s->lastEntry.load(std::memory_order_relaxed)), Why::None, 0));
        diagWarnf("plugin: the unload handshake timed out with %s inside a call", s->id);
        ++charged;
    }
    return charged;
}

bool attributeCrash(const std::uintptr_t* frames, int nFrames, unsigned long faultingTid, Attribution& out) noexcept {
    State* const published = g_published.load(std::memory_order_acquire);
    if (published == nullptr) {
        out.found = false;
        return false;
    }
    State& st = *published;
    const std::size_t n = st.count.load(std::memory_order_acquire);
    const Slot* found = nullptr;
    const char* how = "stack";
    for (int f = 0; f < nFrames && found == nullptr; ++f) {
        const std::uintptr_t a = frames[f];
        for (std::size_t i = 0; i < n; ++i) {
            const Slot* s = st.slots[i].load(std::memory_order_relaxed);
            const std::uintptr_t b = s->modBase.load(std::memory_order_acquire);
            if (b != 0 && a >= b && a < b + s->modSize.load(std::memory_order_relaxed)) {
                found = s;
                break;
            }
        }
    }
    if (found == nullptr) {
        const void* owner = nullptr;
        if (cascade::core::beats::callOfThread(faultingTid, &owner, nullptr) && isSlotPointer(owner) && owner != &st.shared) {
            found = static_cast<const Slot*>(owner);
            how = "in-flight";
        }
    }
    if (found == nullptr) {
        out.found = false;
        return false;
    }
    out.found = true;
    const char* pub = found->pub.load(std::memory_order_relaxed);
    std::size_t k = 0;
    for (; pub[k] != '\0' && k + 1 < sizeof(out.id); ++k) { out.id[k] = pub[k]; }
    out.id[k] = '\0';
    for (k = 0; found->version[k] != '\0' && k + 1 < sizeof(out.version); ++k) { out.version[k] = found->version[k]; }
    out.version[k] = '\0';
    const char* o = originWord(static_cast<Origin>(found->origin.load(std::memory_order_relaxed)));
    for (k = 0; o[k] != '\0' && k + 1 < sizeof(out.origin); ++k) { out.origin[k] = o[k]; }
    out.origin[k] = '\0';
    for (k = 0; how[k] != '\0' && k + 1 < sizeof(out.how); ++k) { out.how[k] = how[k]; }
    out.how[k] = '\0';
    for (k = 0; found->id[k] != '\0' && k + 1 < sizeof(out.runId); ++k) { out.runId[k] = found->id[k]; }
    out.runId[k] = '\0';
    return true;
}

void overrideBudgetForTest(Entry e, unsigned budgetMsValue) {
    state().budgetOverride[static_cast<std::size_t>(e)].store(budgetMsValue, std::memory_order_relaxed);
}

void dropBudgetOverridesForTest() {
    for (auto& o : state().budgetOverride) { o.store(0, std::memory_order_relaxed); }
}

void resetForTest() {
    State& st = state();
    const std::size_t n = st.count.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < n; ++i) {
        Slot* s = st.slots[i].load(std::memory_order_relaxed);
        s->why.store(0);
        s->pinned.store(false);
        s->faults.store(0);
        s->quarantines.store(0);
        s->overruns.store(0);
        s->wedges.store(0);
        s->unloadTimeouts.store(0);
    }
    st.faults.store(0);
    st.quarantines.store(0);
    st.overruns.store(0);
    st.wedges.store(0);
    std::lock_guard<std::mutex> lk(st.events);
    st.queue.clear();
}

}  // namespace cascade::core::fault
