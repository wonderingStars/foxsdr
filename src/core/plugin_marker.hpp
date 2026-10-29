// plugin_marker.hpp - the crash-loop breaker: which plugin was in play when the last run died, and what the next start does about it
// (plugin containment P1, item 1; docs/engine-stage4.md 6.28).
//
// THE PROBLEM. A plugin that crashes the application when it is created, loaded or first fed crashes it again at the next start, and the
// one after: the user's report is "the program will not start any more", and nothing in the program says why. The clean-exit marker
// (core/config.hpp telemetryCleanExit) tells the next start THAT the last run did not end in order; it does not say which plugin was
// doing what. This is the second half of that contract: a small record, kept in memory the operating system keeps for us, that names the
// plugin and the lifecycle phase it was in.
//
// THE MARKER. A fixed 8 KiB file beside the settings file (plugin-play.marker), memory-mapped, so that setting a phase is a handful of
// plain stores - nothing is written through a file API on the way into a plugin's create() - and so that the bytes are in the page cache
// the moment the process dies, however it dies. Sixty-four slots; each holds a plugin's RUN ID (the version-stripped file name), its
// version, its origin and a PHASE:
//
//   load       the module is being mapped and its descriptor queried
//   create     inside an instance's create()
//   first-pass an instance exists and has not yet completed its first block / poll
//   running    it has: the steady state of a plugin that is doing its job
//   unload     inside destroy() or the module's unmap
//
// A slot is cleared when the plugin is out of play (its instances destroyed and its module unmapped), and every slot is cleared by an
// orderly stop (cleanShutdown), the same moment the clean-exit marker is written. A file whose slots are not clear at the next start
// is a run that did not end in order.
//
// WHO IS GUILTY. This is the rule the whole feature stands on, and it is deliberately stingy (the application's OWN crash must never cost
// a person a plugin they did nothing wrong with):
//
//   - a slot in LOAD, CREATE or UNLOAD was INSIDE a call into the plugin at the moment of death: suspect;
//   - a slot in FIRST-PASS is suspect only if it is the ONLY plugin in first-pass and nothing else is suspect (two plugins that were both
//     created and not yet fed prove nothing about either);
//   - a slot the crash handler BLAMED (it found the plugin's module on the faulting stack, or a guarded call of the plugin in flight on the
//     faulting thread: core/plugin_fault.hpp attributeCrash) is suspect, whatever its phase;
//   - a slot in RUNNING that nobody blamed is innocent: the run died of something else.
//
// THE STRIKES (planStart), persisted in plugin-faults.json:
//
//   first suspicion    STRIKE ONE: the next start begins WITHOUT the plugin (it is not even mapped), with a notice and a one-click
//                      re-enable. The start after that gives it a second chance - PROBATION: it loads normally and is watched.
//   suspect again      STRIKE TWO, whether it was on probation or was re-enabled by hand in between: DISABLED until a person re-enables it.
//   a clean run        a plugin on probation that ran (reached `running`) in a session that ended in order has its strike taken off.
//
// A REMOTE session can neither quarantine nor re-enable: the only way into this state from outside the process is the engine's
// FOXAPP_OP_PLUGIN_REENABLE, an application op that the API table refuses every remote session (docs 6.28).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_PLUGIN_MARKER_HPP
#define CASCADE_CORE_PLUGIN_MARKER_HPP

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "core/plugin_fault.hpp"
#include "core/plugin_module_check.hpp"

namespace cascade::core {

namespace marker {

enum class Phase : std::uint8_t { Idle = 0, Load = 1, Create = 2, FirstPass = 3, Running = 4, Unload = 5 };
const char* phaseWord(Phase p);  // "idle", "load", "create", "first-pass", "running", "unload"
bool parsePhase(const std::string& word, Phase& out);

// What a slot held when the PREVIOUS run ended (the file's content at open()).
struct InPlay {
    std::string id;       // the run id
    std::string version;
    fault::Origin origin = fault::Origin::Unrecorded;
    Phase phase = Phase::Idle;
    bool blamed = false;  // the crash handler attributed the death to this plugin
};

inline constexpr std::size_t kSlots = 64;
inline constexpr std::size_t kFileBytes = 8192;
inline constexpr const char* kFileName = "plugin-play.marker";

// Opens the marker file at `path` (created if absent) and returns what the previous run left in it; the file is then reset for THIS run.
// An empty path (or a file that cannot be mapped) is an in-memory marker: nothing is kept for a next start, and the return is empty.
std::vector<InPlay> open(const std::string& path);
// True when open() succeeded in keeping the marker on disk.
bool persistent();

// Allocation-free and lock-light (a spin lock around the slot table, no I/O): the plugin is in `phase` from now on. The first call for an
// id allocates its slot; a full table drops the update (and counts it: dropped()).
void set(const std::string& pluginKeyOrId, Phase phase, const std::string& version = std::string(),
         fault::Origin origin = fault::Origin::Unrecorded) noexcept;
// The plugin is out of play: its slot is freed.
void clear(const std::string& pluginKeyOrId) noexcept;
// The phase a plugin is in now (Idle when it has no slot).
Phase phaseOf(const std::string& pluginKeyOrId) noexcept;

// A BRACKET around one call into a plugin (a create, a destroy): the plugin is in `during` for the life of the object and is put back to
// the phase it was in before - or its slot freed, if it had none - when the object goes. The way to say "inside create()" without
// forgetting what the plugin was doing around it.
class CallPhase {
public:
    CallPhase(const std::string& pluginKeyOrId, Phase during, const std::string& version = std::string(),
              fault::Origin origin = fault::Origin::Unrecorded) noexcept;
    ~CallPhase();
    CallPhase(const CallPhase&) = delete;
    CallPhase& operator=(const CallPhase&) = delete;

private:
    std::string key_;
    Phase before_;
};
// FROM THE CRASH HANDLER: the death belongs to the plugin whose run id this is. Async-signal-safe: plain stores into the mapped bytes, no
// lock, no allocation; a plugin with no slot is not blamed.
void blame(const char* runId) noexcept;
// An orderly stop: every slot is cleared (the same moment as the clean-exit marker).
void cleanShutdown() noexcept;

// FIRST-PASS HAS A CLOCK: an instance that was created and has not been fed is `first-pass` until it has been alive for `afterMs`, and then it
// is `running` (the engine's pass calls this once a pass with its steady clock). Without it a plugin created while the receiver is stopped
// would sit in first-pass for ever, and the application's own crash a day later would be put to its account.
void promoteFirstPass(std::uint32_t nowMs, std::uint32_t afterMs) noexcept;
inline constexpr std::uint32_t kFirstPassMs = 5000;

// The ids that reached RUNNING in this process (bounded to the table's size): what lets a clean shutdown graduate a plugin on probation.
std::set<std::string> ranThisSession();
std::size_t dropped() noexcept;

// TESTS ONLY: forget the in-process memory and unmap, as a fresh process would.
void resetForTest();

}  // namespace marker

// ---------------------------------------------------------------------------------------------------------------------------------
// The strikes
// ---------------------------------------------------------------------------------------------------------------------------------

enum class StrikeState : std::uint8_t { Suspect = 1, Probation = 2, Disabled = 3 };
const char* strikeStateWord(StrikeState s);  // "suspect", "probation", "disabled"

struct StrikeRecord {
    std::string id;       // the run id
    std::string version;  // of the build that was suspect
    unsigned strikes = 0;
    StrikeState state = StrikeState::Suspect;
    std::string phase;    // the phase word it was in when it was suspected
    std::int64_t whenUnix = 0;
    bool fromSite = false;
};

// What a start does with the records and the marker the last run left. PURE: no file, no clock but `nowUnix`.
struct StartNotice {
    std::string id;
    std::string version;
    unsigned strikes = 0;
    bool permanent = false;  // strike two: disabled until re-enabled by hand
    std::string phase;
};
struct StartPlan {
    std::vector<StrikeRecord> records;     // the store as it is after this plan (persist this)
    std::vector<std::string> skip;         // run ids not loaded this start (suspect and disabled)
    std::vector<StartNotice> notices;      // what to tell the person (new suspects and new disabled only)
    std::vector<std::string> suspects;     // the run ids the previous run's marker made suspect (for the log)
};
// The guilty set of a marker, by the rules at the top of this file.
std::vector<marker::InPlay> suspectsOf(const std::vector<marker::InPlay>& previous);
StartPlan planStart(const std::vector<StrikeRecord>& stored, const std::vector<marker::InPlay>& previous, std::int64_t nowUnix);
// The store after an orderly stop in which `ran` reached running: a plugin on probation that ran has its strike taken off.
std::vector<StrikeRecord> graduate(const std::vector<StrikeRecord>& stored, const std::set<std::string>& ran);

// The store's file (plugin-faults.json): tolerant on load (a damaged or alien file is an empty store and says so), atomic on save.
inline constexpr const char* kStoreFileName = "plugin-faults.json";
bool loadStore(const std::string& path, std::vector<StrikeRecord>& out, std::string& error);
bool saveStore(const std::string& path, const std::vector<StrikeRecord>& records, std::string& error);

// ---------------------------------------------------------------------------------------------------------------------------------
// The engine's handle on all of it
// ---------------------------------------------------------------------------------------------------------------------------------
class Containment {
public:
    // Reads the previous run's marker and the store (both beside the settings file in `dir`; empty `dir` is a hermetic run: nothing is
    // read, kept or written), plans, persists the plan, and opens the marker for THIS run. `safeMode` (--safe-plugins) starts with every
    // plugin refused whatever the store says, and records nothing: it is the escape hatch for when the breaker itself is the question.
    // Call once, BEFORE the first plugin scan.
    void begin(const std::string& dir, bool safeMode, std::int64_t nowUnix);

    // Whether the plugin at `pluginKeyOrId` is not to be loaded this start, and the verdict and the sentence that say why.
    struct Gate {
        ModuleVerdict verdict = ModuleVerdict::Ok;
        std::string sentence;
    };
    bool gate(const std::string& pluginKeyOrId, Gate& out) const;

    // What to tell the person this start (new suspects and new disabled), in the order found.
    const std::vector<StartNotice>& notices() const { return plan_.notices; }
    // The standing record of a plugin, or nullptr.
    const StrikeRecord* recordOf(const std::string& pluginKeyOrId) const;
    bool safeMode() const { return safeMode_; }
    bool started() const { return started_; }

    // The person's one click: forget the record (strikes, state) and persist. Returns whether there was one. The plugin loads at the next
    // scan (the engine rescans), and goes on probation only if it is suspected again - it is simply a plugin with no history.
    bool reenable(const std::string& pluginKeyOrId);

    // The orderly stop: graduate what ran clean, persist, clear the marker.
    void cleanShutdown();

    const std::string& dir() const { return dir_; }

private:
    void persist();
    std::string dir_;
    bool safeMode_ = false;
    bool started_ = false;
    StartPlan plan_;
    std::vector<StrikeRecord> records_;
};

}  // namespace cascade::core

#endif  // CASCADE_CORE_PLUGIN_MARKER_HPP
