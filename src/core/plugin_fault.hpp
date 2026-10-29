// plugin_fault.hpp - a bad plugin never takes the application down: the guard on every plugin call that is not the signal path, the
// plugin's quarantine, its call budgets and its health counters (plugin containment batch P1; docs/engine-stage4.md 6.28).
//
// WHAT THIS IS FOR. A plugin is third-party native code in this process (docs/PLUGIN-API.md). Until this batch the host trusted every
// call into one: a plugin whose create() dereferenced null, whose poll_image() looped for ever or whose destroy() threw took the whole
// receiver with it, losing a recording in progress and every other plugin's state. The vendor-call guard of 0.63 (source/vendor_guard.hpp)
// had already shown the shape of the answer for the SDR drivers; this is that shape for the plugin ABI.
//
// THE CONTRACT, in five rules (each is pinned by a test named in docs/engine-stage4.md 6.28):
//
//   1. EVERY PLUGIN ENTRY POINT THAT IS NOT THE SIGNAL PATH IS CALLED THROUGH call(). create, destroy, poll_text, poll_image,
//      release_image, poll_tracks, poll_paths, poll_state, columns, poll_rows, get_tile, release_tile, get_info, release_info, attach,
//      press, the preset table's count and get - and the module's load and unload. A fault INSIDE the plugin (an access violation, an
//      illegal instruction, a divide by zero... the codes source/vendor_guard.cpp absorbs, at an address that is not this program's own
//      image) is eaten: the call reports Faulted, the plugin is QUARANTINED for the session and nothing else is affected. A C++ exception
//      thrown across the C boundary is eaten the same way. THE HOT SIGNAL PATH IS NOT GUARDED: process(), the audio-out and
//      transmit-audio pull()/active() and retune() run once per block under the runner's lock; a plugin that faults there takes the process
//      down exactly as it did, and it is the crash-loop breaker (core/plugin_marker.hpp) that turns the NEXT start into a start without it.
//   2. A FAULT THE GUARD DECLINES STILL KILLS THE PROCESS, WITH A REPORT. A fault whose address is in this program's own image (a bug in
//      the host, reached from a plugin's callback), a stack overflow, an address in no module at all: the guard's filter returns "keep
//      searching" and the crash handler writes the ordinary report. The guard hides the plugin's faults, never the host's.
//   3. A QUARANTINED PLUGIN IS NEVER CALLED AGAIN THIS SESSION. call() answers Refused without calling; the owner of the plugin's
//      instances destroys them through the ordinary directive (the engine does it on the next pass: Engine::applyPluginQuarantines), a
//      plugin that held the transmitter's key is unkeyed first, and only a LOCAL re-enable clears it.
//   4. EVERY GUARDED CALL HAS A BUDGET (kEntries below), declared to the existing monitor (core/thread_beats.hpp CallScope): a call past
//      its budget files a hang report that names the plugin and the entry point, a call past kWedgeFactor times its budget is counted as
//      wedged and the plugin is quarantined. There is no second watchdog.
//   5. EVERYTHING IS COUNTED, per plugin id and version: absorbed faults, quarantines, budget overruns, wedged calls and unload timeouts
//      (health()), and in the anonymous ledger by the four reserved words (core/health_events.hpp: pluginfault, pluginquarantine,
//      pluginwedge, pluginoveruse). A third-party site plugin's id is hashed wherever it leaves the machine (publicId).
//
// THE PLATFORM CAVEAT, stated where it cannot be missed: structured exceptions exist on Windows. On Linux and macOS a fault in a plugin
// is a signal, a signal handler cannot return into the faulting call, and call() therefore runs the plugin unguarded there (exactly as
// source::callGuardingVendorFaults does); the budgets, the counters, the C++ exception catch and the crash-loop breaker apply everywhere.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_PLUGIN_FAULT_HPP
#define CASCADE_CORE_PLUGIN_FAULT_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

namespace cascade::core::fault {

// Where a module came from, as the install records say (core::PluginOrigin, restated here so this header needs nothing of the host).
enum class Origin : std::uint8_t { Unrecorded = 0, Official = 1, Site = 2 };
const char* originWord(Origin o);  // "unrecorded", "official", "site"

// ---------------------------------------------------------------------------------------------------------------------------------
// The guarded entry points, their budgets and the argument for each
// ---------------------------------------------------------------------------------------------------------------------------------
enum class Entry : std::uint8_t {
    Load,          // LoadLibrary, the module's DllMain, and the descriptor query
    Create,        // any capability's create()
    Destroy,       // any capability's destroy()
    PollText,      // a decoder's / basemap's / track-info client's poll_text()
    PollImage,     // an image decoder's poll_image()
    ReleaseImage,  // ... and the release that must follow it
    PollTracks,    // a track source's poll_tracks()
    PollPaths,     // a track source's poll_paths()
    PollState,     // an instrument's poll_state()
    Columns,       // a panel's or an instrument's columns()
    PollRows,      // a panel's or an instrument's poll_rows()
    GetTile,       // a basemap's get_tile()
    ReleaseTile,   // ... and its release_tile()
    GetInfo,       // a track-info client's get_info()
    ReleaseInfo,   // ... and its release_info()
    Attach,        // a host client's attach()
    Press,         // an image input's press()
    PresetCount,   // the preset table's count()
    PresetGet,     // the preset table's get()
    Unload,        // FreeLibrary and the module's DllMain(PROCESS_DETACH)
};
inline constexpr std::size_t kEntryCount = 20;

struct EntrySpec {
    Entry entry;
    const char* word;      // one lower-case word (letters, digits, '-'): written into a hang report
    unsigned budgetMs;     // how long a healthy call may take; past it the call is an OVERRUN
    unsigned legitMs;      // the longest legitimate call the entry has, and what it is (budgetMs is held to kMargin times it)
    const char* argued;
};

// A call that has not returned after this many budgets is WEDGED.
inline constexpr unsigned kWedgeFactor = 4;
// A budget is at least this many times the longest legitimate call of its entry (tests/test_plugin_fault.cpp holds it).
inline constexpr unsigned kMargin = 2;

inline constexpr EntrySpec kEntries[kEntryCount] = {
    {Entry::Load, "load", 60000, 21000,
     "LoadLibrary of a module off a cold disk, a network profile or behind a scanner: Windows' network timeout is 21 s (B19), and the "
     "module's DllMain and descriptor query run inside it"},
    {Entry::Create, "create", 30000, 10000,
     "a plugin that opens its socket, its serial port or its dongle, or builds its tables, in create(); judged: the slowest seen is a "
     "few seconds"},
    {Entry::Destroy, "destroy", 30000, 10000,
     "a plugin that joins its worker or finishes the dwell in progress from destroy() (Survey Engine 0.1.0): a few seconds"},
    {Entry::PollText, "poll-text", 5000, 1000,
     "one read of a decoder's pending text, once a block: milliseconds; a second is already a plugin in trouble"},
    {Entry::PollImage, "poll-image", 10000, 2000,
     "the borrow of a finished picture, which can be a megapixel frame the plugin renders on request"},
    {Entry::ReleaseImage, "release-image", 5000, 1000, "the hand-back of that picture"},
    {Entry::PollTracks, "poll-tracks", 5000, 1000, "a snapshot of a track source's targets, once a frame"},
    {Entry::PollPaths, "poll-paths", 5000, 1000, "a snapshot of a track source's polylines, once a frame"},
    {Entry::PollState, "poll-state", 5000, 1000, "an instrument's readout, once a frame"},
    {Entry::Columns, "columns", 5000, 1000, "a panel's column description, asked once"},
    {Entry::PollRows, "poll-rows", 5000, 1000, "a panel's rows, once a frame"},
    {Entry::GetTile, "get-tile", 20000, 8000,
     "a basemap tile, which a plugin may fetch from the network on a worker thread of the host's tile cache"},
    {Entry::ReleaseTile, "release-tile", 5000, 1000, "the hand-back of that tile"},
    {Entry::GetInfo, "get-info", 20000, 8000, "a track-info lookup, which a plugin may answer from the network on the cache's worker"},
    {Entry::ReleaseInfo, "release-info", 5000, 1000, "the hand-back of that answer"},
    {Entry::Attach, "attach", 10000, 2000, "a host client's one-time hand-over of the host's API table"},
    {Entry::Press, "press", 5000, 1000, "an image input's click, delivered once"},
    {Entry::PresetCount, "preset-count", 5000, 1000, "a static table's size"},
    {Entry::PresetGet, "preset-get", 5000, 1000, "one entry of a static table"},
    {Entry::Unload, "unload", 30000, 10000,
     "FreeLibrary and the module's DllMain(PROCESS_DETACH), which a plugin may use to join a thread it started"},
};

inline constexpr const EntrySpec& specOf(Entry e) { return kEntries[static_cast<std::size_t>(e)]; }
inline constexpr const char* entryWord(Entry e) { return specOf(e).word; }
inline constexpr unsigned budgetMs(Entry e) { return specOf(e).budgetMs; }
inline constexpr unsigned wedgeMs(Entry e) { return specOf(e).budgetMs * kWedgeFactor; }

// ---------------------------------------------------------------------------------------------------------------------------------
// A plugin as the containment knows it
// ---------------------------------------------------------------------------------------------------------------------------------

// Why a plugin is quarantined THIS SESSION (the ones the host decided live). Words are written to the log and to the Fitted modules row.
enum class Why : std::uint8_t {
    None = 0,
    Fault,      // a call faulted and the guard absorbed it
    Exception,  // a call threw a C++ exception across the C boundary
    Wedge,      // a call did not return within kWedgeFactor times its budget
};
const char* whyWord(Why w);  // "none", "fault", "exception", "wedge"

// One plugin's slot: created on first use, never freed (a pointer to one stays valid for the life of the process, so an instance can keep
// it and a worker that outlives main() can still use it). Opaque: everything is read through the functions below.
struct Slot;

// The slot for a plugin, by module file name or id (core::pluginRunId reduces both; matching is ASCII case-insensitive, as NTFS names
// are). Never null for a non-empty key; an empty key returns a shared slot that is never quarantined and keeps no counters.
Slot* slotFor(const std::string& pluginKeyOrId);
const char* idOf(const Slot* s);        // the run id, lower-cased: "pocsag-decoder"
const char* publicIdOf(const Slot* s);  // what may leave the machine: the id of an official plugin, "site-<hash>" for any other

// What the load recorded about the module, so the crash path can name it and the counters carry its version. `moduleBase` / `moduleSize`
// may be null / 0 (a module that was refused, or a test).
void setIdentity(const std::string& pluginKeyOrId, const std::string& version, Origin origin, const void* moduleBase,
                 std::size_t moduleSize);
// A module was unmapped: its address range is no longer attributed to the plugin.
void clearModule(const std::string& pluginKeyOrId);

// ---------------------------------------------------------------------------------------------------------------------------------
// The guarded call
// ---------------------------------------------------------------------------------------------------------------------------------

enum class Outcome : std::uint8_t {
    Ran,      // the call ran to completion; its results are whatever the callable stored
    Faulted,  // the call faulted (or threw) and was absorbed; the plugin is now quarantined; the callable's results are not to be trusted
    Refused,  // the plugin was already quarantined: nothing was called
};

namespace detail {
// `lentMemory` is the read of memory a plugin lent (copyFromPlugin below): the filter then also absorbs an access violation at an address in
// THIS program's image, because the faulting instruction is the host's copy and the address it touched is the plugin's.
Outcome run(Slot* s, Entry e, void (*fn)(void*) noexcept, void* ctx, bool lentMemory = false) noexcept;
Outcome noteThrown(Slot* s, Entry e) noexcept;
}  // namespace detail

// Runs `f()` (a call into one plugin's code) under the guard and the budget. `f` is run on THIS thread, once; the plugin's results leave
// through whatever `f` captures by reference. Never throws. See the contract at the top of this file for what "Faulted" and "Refused"
// mean for the caller: the instance the call was made on is dead, and the caller treats the call as having failed.
template <class F>
Outcome call(Slot* s, Entry e, F&& f) noexcept {
    using Fn = std::remove_reference_t<F>;
    struct Thunk {
        Fn* f;
        bool threw;
    } thunk{&f, false};
    const Outcome o = detail::run(
        s, e,
        [](void* p) noexcept {
            Thunk* t = static_cast<Thunk*>(p);
            try {
                (*t->f)();
            } catch (...) {
                t->threw = true;
            }
        },
        &thunk);
    if (o == Outcome::Ran && thunk.threw) { return detail::noteThrown(s, e); }
    return o;
}

template <class F>
Outcome call(const std::string& pluginKeyOrId, Entry e, F&& f) noexcept {
    return call(slotFor(pluginKeyOrId), e, static_cast<F&&>(f));
}

// ---------------------------------------------------------------------------------------------------------------------------------
// Reading memory a plugin lent (plugin containment P2)
// ---------------------------------------------------------------------------------------------------------------------------------
//
// A pointer a plugin hands back in a struct - an image's or a tile's pixels, a path's vertices, a track-info client's strings - is READ BY THE
// HOST'S OWN CODE after the plugin's call has returned: outside call(), at an address in this program's image, which the guard's filter declines
// by design (rule 2: the guard hides a plugin's faults, never the host's). A plugin that lies about such a pointer - wild, readable for less than
// the struct says, or NULL where the ABI says never - would therefore end the process with an unattributed report, and the lie is the one thing no
// static check can see. These do the read under the guard, with the plugin's call (the entry named) as the thing that is in flight: a fault while
// reading is the plugin's, is counted and filed like any other, and switches the plugin off. They are the ONLY way the host reads lent memory
// (tests/test_plugin_containment.cpp scans for the sites that used to).
//
// On Windows a structured exception is caught. On Linux the read is made by process_vm_readv, which answers EFAULT where a plain read would
// fault, and the same accounting follows. Elsewhere the read is a plain copy (the platform caveat of the top of this file).
//
// All return false when nothing was read - the plugin was already switched off (nothing of it is read), or the read faulted (it now is) - and
// then the destination is not to be used.
bool copyFromPlugin(Slot* s, Entry e, void* dst, const void* src, std::size_t bytes) noexcept;
// `rows` rows of `rowBytes` bytes at `stride` intervals from `src` into `dst`, tightly packed (an image's or a tile's pixels).
bool copyRowsFromPlugin(Slot* s, Entry e, std::uint8_t* dst, std::size_t rowBytes, const std::uint8_t* src, std::size_t stride,
                        std::size_t rows) noexcept;
// The NUL-terminated string at `src`, cut at `maxBytes` bytes (never read past that), into `out`. A null `src` is the empty string.
bool copyStringFromPlugin(Slot* s, Entry e, const char* src, std::size_t maxBytes, std::string& out) noexcept;

// ---------------------------------------------------------------------------------------------------------------------------------
// The quarantine, live
// ---------------------------------------------------------------------------------------------------------------------------------

Why quarantineWhy(const Slot* s);
inline bool isQuarantined(const Slot* s) { return quarantineWhy(s) != Why::None; }
// A plugin that was ever switched off in this process is PINNED: its module is never unmapped (a thread it started, or an instance that was
// leaked, may still be running inside the image), whatever happens to its quarantine afterwards - including a re-enable. Never cleared.
bool isPinned(const Slot* s);
Entry quarantineEntry(const Slot* s);      // the entry that did it (meaningful when quarantined)
bool isQuarantinedKey(const std::string& pluginKeyOrId);
// A fault the CALLER absorbed itself (the descriptor query's own structured-exception handler, which has always turned a fault into a
// refusal): counted as an absorbed fault, and the plugin is switched off. Not the way to guard a call - use call() - only the way to tell the
// containment about one that was caught elsewhere.
void noteFault(Slot* s, Entry e, unsigned long code);
// Quarantines a plugin by hand of the host (the engine, for a reason of its own: tests). Counted as a quarantine; false if it already was.
bool quarantine(Slot* s, Why why, Entry entry);
// LOCAL re-enable: clears a live quarantine. The counters are kept. False if it was not quarantined.
bool release(Slot* s);

// What happened, for the engine's pass to act on: the instances of a quarantined plugin are destroyed, a key it held is let go, the log
// says so. Drained once a pass; bounded (the oldest are dropped past 256).
struct Event {
    enum class Kind : std::uint8_t { Fault, Quarantine, Overrun, Wedge, UnloadTimeout };
    Kind kind = Kind::Fault;
    std::string id;        // the run id (what the host keys the plugin by)
    std::string publicId;  // what may be written to a report
    Entry entry = Entry::Create;
    Why why = Why::None;
    unsigned long code = 0;  // the exception code of a fault
};
std::vector<Event> drainEvents();

// ---------------------------------------------------------------------------------------------------------------------------------
// The counters
// ---------------------------------------------------------------------------------------------------------------------------------
struct Totals {
    std::uint64_t faults = 0;       // pluginfault
    std::uint64_t quarantines = 0;  // pluginquarantine
    std::uint64_t wedges = 0;       // pluginwedge  (a call that never returned, and an unload that timed out)
    std::uint64_t overruns = 0;     // pluginoveruse
};
Totals totals();

inline constexpr std::size_t kMisuseCount = 6;   // the tripwires below (Misuse)

struct HealthRow {
    std::string id;        // the run id
    std::string publicId;
    std::string version;
    Origin origin = Origin::Unrecorded;
    std::uint64_t faults = 0;
    std::uint64_t quarantines = 0;
    std::uint64_t overruns = 0;
    std::uint64_t wedges = 0;          // calls that never returned
    std::uint64_t unloadTimeouts = 0;  // unloads that timed out, charged to the plugin that was inside a call
    Why quarantined = Why::None;
    Entry lastEntry = Entry::Create;
    std::uint64_t misuse[kMisuseCount] = {};   // the tripwires below, by Misuse
};
// Every plugin the containment has a slot for, in first-use order.
std::vector<HealthRow> health();

// ---------------------------------------------------------------------------------------------------------------------------------
// The tripwires (plugin containment batch P2, docs/engine-stage4.md 6.29)
// ---------------------------------------------------------------------------------------------------------------------------------
//
// The ABI states the rules a plugin lives by (core/plugin_abi.h: THREADING AND LIFETIME CONTRACT, HOST API LEVEL 1 - THREADING and AFTER
// UNLOAD). A plugin that breaks one is not necessarily dangerous - the host answers the call with the ABI's own refusal - but it is a plugin
// whose author should hear of it, and a host that answers a rule-breaker as if nothing had happened has hidden a defect. Every refusal below
// is therefore COUNTED per plugin id and version (health(), the same row as the faults) and the first of each kind per plugin is said once in
// the diagnostic log (drainMisuseNotices, the engine's pass). Counting is a single relaxed atomic add on the plugin's slot: it is called from
// whatever thread the plugin called from, including the real-time one, and takes no lock and allocates nothing.
//
//   WrongThread   a host call the ABI forbids on the thread it came from: settings_get / settings_set on the real-time thread (answered
//                 CASCADE_API_WRONG_THREAD).
//   AfterDetach   a host call through a table the host has shut down (the plugin was stopped, its instances destroyed, the host is exiting),
//                 made from OUTSIDE any call into the plugin: a plugin that saves its state from destroy() is inside one and is not counted;
//                 a thread the plugin started and let outlive destroy() (ABI rule 3) is not, and is counted. Answered CASCADE_API_DETACHED.
//                 Such a thread may still be running in the image when the host unmaps it, which is a crash the host cannot absorb, so the
//                 plugin's module is PINNED (never unmapped) from the first such call - exactly what a quarantine does.
//   Flood         a write refused because the bounded queue or per-plugin quota it fills was full (BUSY from the control and log rings, LIMIT
//                 from the markers, commands and settings quotas).
//   BadCall       a host call with a NULL, a malformed or too-small struct, or a bad key (CASCADE_API_BAD_ARGUMENT).
//   BadReturn     an answer to a host-to-plugin call that is outside what the call allows - a poll that returns more than it was given room
//                 for, a panel with more columns than the ABI has. The host clamps it, as it always did; now it also counts.
//   HostReentry   THE HOST'S OWN RULE, not the plugin's: a guarded call into a plugin began on a thread that was already inside a call into
//                 the same plugin. The ABI promises "no callbacks" - the host never calls plugin code from inside a host API call - so this
//                 counter is a defect in the host, and a test holds it at zero across everything the suite does.
enum class Misuse : std::uint8_t { WrongThread = 0, AfterDetach, Flood, BadCall, BadReturn, HostReentry };
const char* misuseWord(Misuse m);  // "wrong-thread", "after-detach", "flood", "bad-call", "bad-return", "host-reentry"

// Counts one tripwire for the plugin. Lock-free and allocation-free: any thread. A null or key-less slot counts nothing.
void noteMisuse(Slot* s, Misuse m) noexcept;
// AfterDetach, with its two rules: counted (and the module pinned) only when the calling thread is NOT inside a guarded call into this
// plugin. Returns whether it counted.
bool noteDetachedCall(Slot* s) noexcept;
// Whether the calling thread is inside a guarded call (call()) into this plugin.
bool insideCallOf(const Slot* s) noexcept;
// Keeps the plugin's module mapped for the life of the process (what a quarantine does, without the switching off).
void pin(Slot* s) noexcept;
std::uint64_t misuseTotal(Misuse m);

// The first occurrence of each kind, per plugin, since the last drain (the engine's pass writes them to the diagnostic log, once each).
struct MisuseNotice {
    std::string id;        // the run id
    std::string publicId;
    std::string version;
    Misuse kind = Misuse::WrongThread;
    std::uint64_t count = 0;  // how many so far
};
std::vector<MisuseNotice> drainMisuseNotices();

// The monitor's findings (core/thread_beats.hpp): the hang watchdog's thread hands a call that overran or wedged here. Counts it once
// (the call's own end counts a late return the monitor never saw), quarantines on a wedge.
void noteMonitorFinding(const void* owner, int stage, const char* entryWord);
// The engine's unload handshake gave up waiting for the interface: charge every plugin that has a call in flight (the interface was
// inside one) with an unload timeout. Returns how many were charged.
std::size_t noteUnloadTimeout();

// ---------------------------------------------------------------------------------------------------------------------------------
// The crash path
// ---------------------------------------------------------------------------------------------------------------------------------

// Which plugin a crash belongs to, if any: a plugin MODULE on the faulting stack (an address inside a registered module range), else a
// plugin whose guarded call is in flight on the faulting thread. Allocation-free, lock-free, safe from the fault path.
struct Attribution {
    bool found = false;
    char id[48] = {0};         // the PUBLIC id
    char version[24] = {0};
    char origin[12] = {0};     // "official", "site", "unrecorded"
    char how[10] = {0};        // "stack" or "in-flight"
    char entry[16] = {0};      // the guarded entry in flight on the faulting thread for THIS plugin ("create", "poll-text"...), empty when none
    char runId[48] = {0};      // the plugin's run id: what the crash-loop marker is keyed by (internal - never written to a report)
};
bool attributeCrash(const std::uintptr_t* frames, int nFrames, unsigned long faultingTid, Attribution& out) noexcept;

// TESTS ONLY (nothing under src/ calls these; tests/test_plugin_fault.cpp scans): forget every quarantine and counter, and the events;
// stand a budget in for an entry's (the wedge limit follows: kWedgeFactor times it), so a test can stage a call that overruns in
// milliseconds and still run the product's code path.
void resetForTest();   // (and the tripwires' counters)
void overrideBudgetForTest(Entry e, unsigned budgetMs);
void dropBudgetOverridesForTest();

}  // namespace cascade::core::fault

#endif  // CASCADE_CORE_PLUGIN_FAULT_HPP
