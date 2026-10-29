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
Outcome run(Slot* s, Entry e, void (*fn)(void*) noexcept, void* ctx) noexcept;
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
};
// Every plugin the containment has a slot for, in first-use order.
std::vector<HealthRow> health();

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
    char runId[48] = {0};      // the plugin's run id: what the crash-loop marker is keyed by (internal - never written to a report)
};
bool attributeCrash(const std::uintptr_t* frames, int nFrames, unsigned long faultingTid, Attribution& out) noexcept;

// TESTS ONLY (nothing under src/ calls these; tests/test_plugin_fault.cpp scans): forget every quarantine and counter, and the events;
// stand a budget in for an entry's (the wedge limit follows: kWedgeFactor times it), so a test can stage a call that overruns in
// milliseconds and still run the product's code path.
void resetForTest();
void overrideBudgetForTest(Entry e, unsigned budgetMs);
void dropBudgetOverridesForTest();

}  // namespace cascade::core::fault

#endif  // CASCADE_CORE_PLUGIN_FAULT_HPP
