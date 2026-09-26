// plugin_api.hpp - the host side of HOST API LEVEL 1 (plugin_abi.h).
//
// WHAT THIS IS. plugin_abi.h's CascadeHostApi grew from four functions to a
// whole receiver: reads of its state, requests to change it, marks on the
// spectrum, a settings store, messages to the user, and command keys. This
// class is everything BEHIND those functions that is not ImGui and not the
// application window: the snapshot a plugin reads, the queues a plugin writes
// into, the per-plugin marks, commands and settings, and the grants that
// decide what a plugin may ask for. The C trampolines that plugins actually
// call live in plugin_ui.cpp, next to the four they join; AppWindow is the one
// place that publishes the snapshot and drains the queues, on its GUI thread.
//
// THE THREADING RULE, and it is the whole design. A plugin may call any of
// these from any thread - its own worker, the GUI thread, the real-time DSP
// thread inside process(). So:
//
//   - NOTHING HERE TOUCHES THE RECEIVER. A request is validated, checked
//     against the plugin's grant, and queued; AppWindow applies it on the GUI
//     thread at the start of its next frame, through the same code a click or
//     a web request goes through. The plugin is told at once whether it was
//     ACCEPTED, and sees it LAND through the snapshot's counters.
//   - READS TAKE NO LOCK. The snapshot is the application's one receiver
//     snapshot (core/receiver_snapshot.hpp), published through a sequence
//     lock (core/seqlock_box.hpp), so the DSP thread can read it without ever
//     waiting for the GUI thread.
//   - EVERYTHING ELSE TAKES mutex_, held only for a bounded copy of host data.
//     No plugin code is ever called with it held (this class calls no plugin
//     code at all), no I/O happens under it, and the queues are fixed-size
//     arrays so a call never allocates. A full queue answers BUSY.
//   - THE SETTINGS STORE REFUSES THE REAL-TIME THREAD. It copies up to 4 KB
//     and allocates, which the DSP thread must never do; a call from a thread
//     marked by RealtimeThreadScope gets CASCADE_API_WRONG_THREAD.
//
// LIFETIME. A PluginApiCore is held by shared_ptr: by the PluginUi that owns
// it AND by every host bridge handed to a plugin. Bridges live until process
// exit (see plugin_ui.cpp: a plugin may keep the table pointer for as long as
// it is loaded), so this object does too - a plugin calling through a stale
// table finds live memory that answers CASCADE_API_DETACHED, never freed
// memory. Clients (one per plugin module) are likewise never freed while the
// core lives.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_PLUGIN_API_HPP
#define CASCADE_CORE_PLUGIN_API_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

#include "core/plugin_abi.h"
#include "core/receiver_snapshot.hpp"
#include "core/seqlock_box.hpp"

namespace cascade::core {

// ---------------------------------------------------------------------------
// The real-time thread marker
// ---------------------------------------------------------------------------
//
// Constructed at the top of every thread that runs plugin process() calls
// under the real-time rules (the pipeline's DSP thread, each patch radio's
// reader). While one is alive on a thread, onRealtimeThread() is true there.
// Nesting is allowed. A thread_local counter, so asking costs one load and no
// thread ever sees another's answer.
class RealtimeThreadScope {
public:
    RealtimeThreadScope();
    ~RealtimeThreadScope();
    RealtimeThreadScope(const RealtimeThreadScope&) = delete;
    RealtimeThreadScope& operator=(const RealtimeThreadScope&) = delete;
};
bool onRealtimeThread();

// ---------------------------------------------------------------------------
// What plugins read about the receiver
// ---------------------------------------------------------------------------
//
// THE ONE RECEIVER SNAPSHOT (engine stage 2, core/receiver_snapshot.hpp).
// This class used to keep a snapshot of its own, published by AppWindow from
// its own copy of the facts; it now reads the snapshot every reader shares -
// the web server's /api/status and CAT read the same one - so a plugin, a
// browser and a logging program can never be told two different things. The
// level-1 ABI's own sequence counters (whose groups differ from the engine
// API's) are derived in the same publish and carried in the snapshot's
// extension (AppStateExt::abiSeq...).

// The stream clock the plugin runner keeps (see CascadeStreamInfo). Written
// only under the runner's own mutex, which is what makes it single-writer.
struct StreamFacts {
    std::uint64_t epoch = 0;
    std::int64_t epochStartUnixMs = 0;
    double iqRateHz = 0.0;
    double audioRateHz = 0.0;
    std::uint64_t iqFrames = 0;
    std::uint64_t audioFrames = 0;
};

class StreamClock {
public:
    // Control thread, under the runner's mutex. A new unbroken stream.
    void beginEpoch(double iqRateHz, double audioRateHz, std::int64_t nowUnixMs);
    // DSP thread, under the runner's mutex.
    void addIq(std::uint64_t frames);
    void addAudio(std::uint64_t frames);
    // Any thread, lock-free.
    bool read(StreamFacts& out) const { return box_.load(out); }

private:
    StreamFacts writer_{};  // the writer's own copy; guarded as the writes are
    SeqlockBox<StreamFacts> box_;
};

// ---------------------------------------------------------------------------
// One plugin module, as the API sees it
// ---------------------------------------------------------------------------
//
// Created on first attach and never freed while the core lives, so a pointer
// to one is valid for as long as the bridge that holds it.
struct PluginApiClient {
    std::string key;    // module file name: grants, stop, markers, commands
    std::string name;   // descriptor name: the settings store
    std::size_t index = 0;

    // False until the host attaches the plugin and again once it is stopped,
    // removed or the host is shutting down. Every call through a client that
    // is not live answers CASCADE_API_DETACHED.
    std::atomic<bool> live{false};
    // CASCADE_STATE_TUNE_GRANTED | _SETTINGS_GRANTED | _STOPPED, as this
    // plugin's flags in get_state. Written under the core's mutex by the GUI
    // thread, read lock-free by anyone.
    std::atomic<std::uint32_t> grants{0};
    // Set the first time the plugin asks for a SETTINGS-grant function, so the
    // host offers the grant key only where it would mean something.
    std::atomic<bool> askedSettings{false};
    std::atomic<bool> askedTune{false};
};

// One queued receiver-control request. Plain data: the queue is a fixed ring.
struct PluginControl {
    enum class Kind : std::uint8_t {
        Frequency,   // tunedHz          - TUNE grant
        VfoOffset,   // offsetHz         - TUNE grant
        Mode,        // CASCADE_DEMOD_*  - SETTINGS grant, and below
        Bandwidth,
        Squelch,
        SampleRate,
        Gain,        // gainName, value
        DeviceAgc,   // flag
        Running,     // flag
        Volume,
        Muted,       // flag
    };
    Kind kind = Kind::Frequency;
    std::size_t client = 0;     // PluginApiClient::index
    double value = 0.0;
    std::uint32_t mode = 0;
    bool flag = false;
    char gainName[CASCADE_GAIN_NAME_CHARS] = {};
};

// Whether a control kind needs the TUNE grant (true) or the SETTINGS one.
bool controlNeedsTuneGrant(PluginControl::Kind k);

// A message a plugin logged, drained by the GUI thread.
struct PluginLogLine {
    std::string key;
    std::string name;
    std::uint32_t level = CASCADE_LOG_INFO;
    std::string text;
};

// A mark, with the plugin that set it.
struct HostMarker {
    std::string key;
    std::string name;
    CascadeMarker m{};
};

struct HostCommand {
    std::uint32_t id = 0;
    std::string label;
};

// The whole settings store: plugin NAME -> key -> value.
using PluginSettingsMap = std::map<std::string, std::map<std::string, std::string>>;

// True for a key the settings store accepts: 1..63 bytes of [A-Za-z0-9._-].
bool validSettingKey(const char* key);

// The same bounds applied to a whole map loaded from a config file, which
// may have been hand-edited: bad keys, oversized values and anything past the
// per-plugin cap are dropped, and so is any plugin past kMaxSettingsPlugins
// (a plugins directory holds dozens; a file naming thousands is a hand-edit,
// and the cap keeps it from growing the config without bound). Returns the
// cleaned map.
inline constexpr std::size_t kMaxSettingsPlugins = 256;
PluginSettingsMap sanitisePluginSettings(const PluginSettingsMap& in);

class PluginApiCore {
public:
    // `snapshot` is the receiver snapshot this core's reads answer from - the
    // application's one, shared with every other reader. Null makes a private
    // one that nothing publishes (a PluginUi built on its own, as the tests
    // build it, answers the never-published state until someone does).
    explicit PluginApiCore(std::shared_ptr<ReceiverSnapshot> snapshot = nullptr);
    PluginApiCore(const PluginApiCore&) = delete;
    PluginApiCore& operator=(const PluginApiCore&) = delete;

    // ===== Host side (the GUI / control thread) ==============================

    // Find-or-create the client for a module. GUI thread.
    PluginApiClient& client(const std::string& key, const std::string& name);
    std::size_t clientCount() const;

    // The set of modules attached by the latest rebuild: those become live,
    // every other client stops being live AND loses its marks, commands,
    // pending presses and queued requests - a plugin that is gone must leave
    // nothing on the screen and nothing waiting to act. Clients staying live
    // never see a transient DETACHED.
    void setLiveSet(const std::vector<std::string>& keys);

    // False when the owner is being destroyed: every client answers DETACHED
    // from then on, whatever its own flag says.
    void setAttached(bool attached);
    bool attached() const { return attached_.load(std::memory_order_acquire); }

    // Grants and the stop set, keyed on module file name.
    void setTuneGranted(const std::string& key, bool granted);
    void setSettingsGranted(const std::string& key, bool granted);
    void setStopped(const std::vector<std::string>& keys);
    // Both grants off for every module (the stop set is kept). PluginUi's
    // clear() calls this beside clearing its tune grants; the owner re-applies
    // both from its config after the rescan.
    void clearGrants();
    bool settingsGranted(const std::string& key) const;
    // Module keys that have asked for a SETTINGS-grant function.
    std::vector<std::string> settingsRequesters() const;

    // The receiver snapshot the reads answer from. Published by its owner (the
    // application, once a frame), never by this class. Never null; the same
    // object for this core's whole life.
    ReceiverSnapshot& snapshot() const { return *snapshot_; }
    const std::shared_ptr<ReceiverSnapshot>& snapshotShared() const { return snapshot_; }

    // The stream clock the runner writes (shared with it).
    const std::shared_ptr<StreamClock>& streamClock() const { return clock_; }

    // Drains. GUI thread. Each empties its queue into `out` (appended).
    void takeControls(std::vector<PluginControl>& out);
    void takeLog(std::vector<PluginLogLine>& out);
    // How many log lines were refused BUSY since the start.
    std::uint64_t logDropped() const;

    // Whether a queued control may still be applied NOW: the client is live,
    // not stopped, and still holds the grant it needs. AppWindow asks this at
    // apply time, so a grant revoked between the request and the frame, or a
    // plugin stopped in that window, stops the request from acting.
    bool controlStillAllowed(const PluginControl& c) const;
    std::string clientKey(std::size_t index) const;
    std::string clientName(std::size_t index) const;

    // Marks, for drawing. `seq` advances on every change so the caller can
    // skip the copy on an unchanged frame.
    std::uint64_t markersSeq() const;
    void markers(std::vector<HostMarker>& out) const;

    // Commands for one module, for its plate; and a press from the user.
    std::vector<HostCommand> commands(const std::string& key) const;
    bool pressCommand(const std::string& key, std::uint32_t id);

    // The settings store. load() replaces it (from the config file);
    // generation() advances on every accepted write; snapshot() copies it.
    void loadSettings(const PluginSettingsMap& settings);
    std::uint64_t settingsGeneration() const;
    PluginSettingsMap settingsSnapshot() const;

    // ===== Plugin side (any thread; the trampolines call these) ==============
    //
    // Every one returns a CASCADE_API_* code and never throws.

    std::int32_t getState(const PluginApiClient& c, CascadeReceiverState* out) const;
    std::int32_t getGain(const PluginApiClient& c, std::uint32_t index,
                         CascadeGainInfo* out) const;
    std::int32_t getSampleRates(const PluginApiClient& c, double* out, std::uint32_t cap) const;
    std::int32_t getStreamInfo(const PluginApiClient& c, CascadeStreamInfo* out) const;

    // Validates `req` (range, NaN), checks the grant, and queues it.
    std::int32_t requestControl(PluginApiClient& c, const PluginControl& req);

    std::int32_t setMarker(const PluginApiClient& c, const CascadeMarker* m);
    std::int32_t removeMarker(const PluginApiClient& c, std::uint32_t id);
    std::int32_t clearMarkers(const PluginApiClient& c);

    std::int32_t settingsGet(const PluginApiClient& c, const char* key, char* buf,
                             std::size_t cap) const;
    std::int32_t settingsSet(const PluginApiClient& c, const char* key, const char* value);

    std::int32_t log(const PluginApiClient& c, std::uint32_t level, const char* text);

    std::int32_t addCommand(const PluginApiClient& c, std::uint32_t id, const char* label);
    std::int32_t removeCommand(const PluginApiClient& c, std::uint32_t id);
    std::int32_t pollCommand(const PluginApiClient& c, std::uint32_t* id);

    // Bounds, public so the tests and the docs quote the same numbers.
    static constexpr std::size_t kControlQueue = 256;
    static constexpr std::size_t kLogQueue = 256;
    static constexpr std::size_t kPressQueue = 16;

private:
    struct LogEntry {
        std::size_t client = 0;
        std::uint32_t level = 0;
        char text[CASCADE_LOG_TEXT_BYTES] = {};
    };
    struct CommandSlot {
        std::uint32_t id = 0;
        char label[CASCADE_COMMAND_LABEL_CHARS] = {};
    };
    // Per-client state that is not atomic. Parallel to clients_, preallocated
    // at client creation so a plugin's call never allocates.
    struct ClientState {
        std::vector<CascadeMarker> markers;  // capacity CASCADE_MAX_MARKERS_PER_PLUGIN
        std::array<CommandSlot, CASCADE_MAX_COMMANDS_PER_PLUGIN> commands{};
        std::size_t commandCount = 0;
        std::array<std::uint32_t, kPressQueue> presses{};
        std::size_t pressHead = 0;
        std::size_t pressCount = 0;
    };

    // The common gate: attached, live. Returns CASCADE_API_OK or DETACHED.
    std::int32_t gate(const PluginApiClient& c) const;
    void clearClientLocked(std::size_t index);
    void refreshGrantsLocked(PluginApiClient& c);

    std::atomic<bool> attached_{true};
    // Set once, in the constructor, and never reassigned: plugin threads read
    // through it with no lock.
    const std::shared_ptr<ReceiverSnapshot> snapshot_;
    std::shared_ptr<StreamClock> clock_;

    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<PluginApiClient>> clients_;
    std::vector<ClientState> state_;
    std::vector<std::string> tuneGranted_;
    std::vector<std::string> settingsGranted_;
    std::vector<std::string> stopped_;
    std::array<PluginControl, kControlQueue> controls_{};
    std::size_t controlHead_ = 0;
    std::size_t controlCount_ = 0;
    std::array<LogEntry, kLogQueue> log_{};
    std::size_t logHead_ = 0;
    std::size_t logCount_ = 0;
    std::uint64_t logDropped_ = 0;
    std::uint64_t markersSeq_ = 1;

    mutable std::mutex settingsMutex_;
    PluginSettingsMap settings_;
    std::uint64_t settingsGeneration_ = 0;
};

}  // namespace cascade::core

#endif  // CASCADE_CORE_PLUGIN_API_HPP
