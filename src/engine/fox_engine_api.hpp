// fox_engine_api.hpp - THE ENGINE BEHIND THE C TABLE (engine extraction stage
// 4, docs/engine-stage4.md): cascade::engine::Engine served through
// FoxEngineApi (third_party/foxsdr_api/foxsdr_api.h), exactly as the reference
// engine serves API 0.2 (third_party/foxsdr_api/reference/mock_engine) -
// sessions with grants and a keep-alive, tickets, BUSY, the merge of the five
// safer commands, results in ticket order and never before the snapshot that
// shows them, events, logins - with the real receiver behind it; and API
// 0.3's additions (docs/api-0.3-additions.md: the lists and facts the window
// draws from, scope taps, patch radios' spectra, the long-text submit).
//
// HOW IT SITS IN THE ENGINE. The Engine owns one ApiHost (Engine::api()).
// Interfaces reach it only through the table: foxsdr_engine_query /
// engineApi() for an engine the table CREATES (headless, on its own control
// thread), or Engine::apiHandle() for the host process's own Engine (the
// standard window holds one and opens a LOCAL session on it). The host's
// side is two calls the Engine makes on its pumping thread, once a pass:
//   beginPass()  in Engine::pumpFrameBegin, after the local queue's drain:
//                sessions closed or detached are dealt with first, then every
//                queued command is applied in queue order through the one
//                control path (Engine::applyCommand) or the key rules;
//   endPass()    in Engine::pumpPublish, AFTER the pass's snapshot has been
//                published: the pass's results are handed to their sessions,
//                in ticket order, and the pass's events broadcast.
// Everything an interface calls - heartbeat, reads, submit, polls - may be
// called from any thread and never waits on that pass (rule 3): the queue and
// each session's results are behind their own short locks, the snapshot is a
// sequence lock, and the spectrum a lock-free ring.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_ENGINE_FOX_ENGINE_API_HPP
#define CASCADE_ENGINE_FOX_ENGINE_API_HPP

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/freq_manager.hpp"
#include "core/plugin_runner.hpp"
#include "foxsdr_api.h"

namespace cascade::engine {

class Engine;
class ApiHostImpl;
struct ApiListBlock;

// The engine's side of the table. One per Engine; built with it.
class ApiHost {
public:
    // Options as FoxEngineParams::options carries them ("k=v;k=v", the
    // reference engine's names): the test timers (pttHoldMs, latchTimeoutMs,
    // keepaliveMs, remoteKeepaliveMs - which may only SHORTEN), token, user,
    // password, maxSessions, maxRemoteSessions, loginLockoutMs, tokenTtlMs,
    // and testHooks=1 (the SETTING_SET "test.*" keys and the dummy
    // transmitter "test:tx:dummy" - never set by the product).
    ApiHost(Engine& engine, const std::string& options);
    ~ApiHost();
    ApiHost(const ApiHost&) = delete;
    ApiHost& operator=(const ApiHost&) = delete;

    // The C handle for this host (never null).
    FoxEngine* handle();

    // The pumping thread's two calls, once a pass (see the file header).
    void beginPass();
    void endPass();
    // The pumping thread hands over a new list block (before the publish
    // whose listSeq says it changed).
    void setLists(std::shared_ptr<const ApiListBlock> lists);
    // API 0.3 8.7: the receiver tuned off the presets of `names` plugins that are still running and muting the
    // audio, and the engine has armed the question (Engine::updateAudioMute, on the pumping thread, AFTER the
    // pass's publish). The event FOXAPI_EVENT_MUTE_TUNE_AWAY is broadcast by the NEXT endPass - after the
    // publish that carries the held mute in MUTE_CAUSES - so an interface that is told finds the plugins
    // already named. Once per call; a call before that endPass replaces the one waiting.
    void muteTuneAway(std::uint32_t names);
    // TESTS ONLY - nothing in the product calls it: put one spectrum frame in
    // the ring the way endPass puts the pipeline's newest there (the next
    // sequence number, `count` bins of dB, the centre and span the frame
    // names). A hermetic engine that runs no pipeline (a window test must not
    // open a sound device) can then hand an interface a spectrum whose every
    // bin is known. The pumping thread's call.
    void publishSpectrumForTest(const float* bins, std::size_t count, double centreHz, double spanHz);

    // --- THE KEY (core/tx_key_rules.hpp), the pumping thread ---------------------
    // An end the ENGINE makes, whoever asked for it (Engine::applyCommand's
    // RUN 0, TX_OPEN and TX_CLOSE): opens the key and marks a latch.
    void endKeyFromEngine(const char* why);
    // The web remote's PTT (Engine::applyCommand's TX_PTT): a REMOTE key the
    // engine holds for the browser page, under the same rules as any remote
    // session - a 2 s hold, and only with a local session's consent.
    FoxCommandResult webRemotePtt(bool on);
    // ANY THREAD: the browser page polled its status - the web remote's
    // keep-alive (its PTT re-assertions are not one).
    void webRemoteBeat();
    // The pumping thread hands over the decoder-output backlog (API 0.3's
    // FOXAPI_LIST_DECODER_LINES): the lines since the last pass.
    void takeDecoderLines(const std::deque<cascade::core::DecodedLine>& log, std::uint64_t total);
    // The web server is not running (FOXAPP_OP_WEB_CONTROL_STOPPED): a key the
    // web remote holds ends in the same step.
    void webRemoteStopped(const char* why);
    // The control loop's last act, before it stops the Transmitter.
    void engineStopping();
    // What the publish carries of the key (Engine::fillPublishedState).
    struct TxFacts {
        std::uint64_t owner = 0;
        bool latched = false;
        bool remoteArmed = false;
        std::int64_t holdRemainingMs = 0;
        std::int64_t latchRemainingMs = 0;
        const char* unkeyReason = "";
        std::uint64_t changes = 0;
    };
    TxFacts txFacts() const;

    // What the pass's publish needs from here (Engine::fillPublishedState),
    // and the per-session fields read_state patches in.
    ApiHostImpl& impl() { return *impl_; }

private:
    std::unique_ptr<ApiHostImpl> impl_;
};

// THE LISTS read_list serves (API.md 7.4), as ONE immutable block the pumping
// thread builds - only when something in it changed (its fingerprint, which
// also moves FoxReceiverState::listSeq in the same publish) - and hands to
// the host before it publishes the state that says so. A reader takes the
// block's pointer under a short lock and reads it at leisure; the bookmarks,
// which can number tens of thousands, are copied only when FreqManager's
// version moves.
struct ApiListBlock {
    std::uint64_t fingerprint = 0;
    std::uint32_t mode = 0;          // FOXAPI_DEMOD_* in force (MODES' ACTIVE)
    double bandwidthHz = 0.0;        // in force (BANDWIDTHS' ACTIVE)
    std::shared_ptr<const std::vector<cascade::core::Bookmark>> bookmarks;
    struct Device {
        std::string label;
        std::string id;              // what SELECT_SOURCE takes, verbatim
        bool active = false;
    };
    std::vector<Device> devices;
    std::vector<double> rates;
    double rateHz = 0.0;             // in force
    struct Gain {
        std::string name;
        double minDb = 0.0, maxDb = 0.0, stepDb = 0.0, currentDb = 0.0;
        // THE UNIT TOKEN (API 0.3, GAINS' detail): "dB" or "step" - one of
        // gui::gainUnitWire's two static strings, so a block never allocates
        // for it.
        const char* unit = "dB";
        // THE STAGE'S OWN AGC IS DRIVING IT (API 0.3 8.3, FOXAPI_ITEM_AGC): an
        // Airspy's LNA or MIXER with its automatic gain control switched on.
        bool agc = false;
    };
    std::vector<Gain> gains;
    std::vector<std::string> antennas;
    std::string antenna;             // in force
    struct AudioDevice {
        std::string name;
        long long index = -1;        // what AUDIO_DEVICE takes (ival[0])
        bool active = false;
    };
    std::vector<AudioDevice> audioDevices;
    struct Plugin {
        std::string name;
        std::string fileName;
        std::string key;
        bool loaded = false;
        bool decoder = false;
        bool running = false;
        std::string status;
        // API 0.3 8.7: the capability word (CASCADE_CAP_* of the plugin ABI), whether the user has stopped the
        // plugin (any loaded plugin, not only a decoder), the two grants the config holds for its module file,
        // and - for a decoder - the effective "mute audio while running" setting.
        std::uint32_t capabilities = 0;
        bool stopped = false;
        bool tuneAllowed = false;
        bool settingsAllowed = false;
        bool mutes = false;
        // API 0.3 8.8, what the fitted-modules window draws a module from: the file's size in bytes (stat-ed
        // when the block is built, 0 when it could not be), the newest WARN/ERROR the module logged (0 none,
        // else CASCADE_LOG_WARN / _ERROR), whether the runner is feeding it or would with the receiver running
        // (any kind of plugin), whether it has asked to change a receiver setting, and whether its file is not
        // the one installed.
        std::uint64_t sizeBytes = 0;
        std::uint32_t noticeLevel = 0;
        bool matched = false;
        bool askedSettings = false;
        bool digestMismatch = false;
    };
    std::vector<Plugin> plugins;
    // API 0.3 8.8: the command keys the loaded modules have added (PLUGIN_COMMANDS), module by module in host
    // order and each module's in the order it added them.
    struct PluginCommand {
        std::uint32_t id = 0;
        std::string label, file;
    };
    std::vector<PluginCommand> pluginCommands;

    // ---- API 0.2's lists step 1 left out, and API 0.3's (docs/api-0.3-additions.md)
    struct Decoder {
        std::string name;
        std::string key;
        std::string status;          // why it is idle, or "" running
        bool running = false;
        bool fed = false;
        bool mutes = false;          // API 0.3 8.7: "mute audio while running" is on
    };
    std::vector<Decoder> decoders;
    struct StoreEntry {
        std::string id, name;
        bool installed = false, compatible = false;
    };
    std::vector<StoreEntry> store;
    std::vector<std::pair<std::string, std::string>> storeBlocked;  // file, why
    std::vector<std::pair<std::string, std::string>> storeUpdates;  // id, "from -> to"
    std::string txArgs;              // the address TX_OPEN is given
    bool txOpen = false;
    struct Marker {
        std::uint64_t id = 0;
        std::string label, plugin;
        double freqHz = 0.0, widthHz = 0.0;
        std::uint32_t colourRgba = 0;
        // API 0.3 8.7: what the plugin's mark is, beyond where it is.
        bool span = false, dashed = false, spectrumOnly = false;
    };
    std::vector<Marker> markers;
    struct Band {
        std::string name, service;
        double startHz = 0.0, endHz = 0.0;
        std::uint32_t colourRgba = 0;
    };
    std::vector<Band> bandPlan;
    struct PlanChoice {
        std::string id, name;
        bool active = false;
    };
    std::vector<PlanChoice> bandPlans;
    struct Preset {
        std::string label, plugin;
        double freqHz = 0.0, bandwidthHz = 0.0;
        std::uint32_t demodMode = 0, index = 0;
        bool deviceCentre = false;   // API 0.3 8.7: moves the radio's centre, not the VFO
    };
    std::shared_ptr<const std::vector<Preset>> presets;  // plugin presets: rebuilt only when the plugins change
    std::uint64_t presetsFingerprint = 0;
    std::vector<Preset> userPresets;
    std::vector<std::pair<std::string, bool>> deviceOptions;  // option, on
    std::vector<std::string> serialPorts;
    struct StatusLine {
        const char* key = "";
        std::string text;
        int level = 1;               // 1 info, 2 warning, 3 error
    };
    std::vector<StatusLine> statusLines;  // every line, the empty ones too (the list shows the others)
    struct Converter {
        std::string radioKey;
        int mode = 0;                // 0 off, 1 up, 2 down
        double loHz = 0.0;
        bool inverted = false, active = false;
        // The ACTIVE entry's value[3] (API 0.3 8.3): the radio's own centre
        // readback, read LIVE from the atomic the pass writes
        // (Engine::converterRadioCentre_), like a recording's byte count, so a
        // retune never rebuilds the block. Null on every other entry.
        const std::atomic<double>* radioCentre = nullptr;
    };
    std::vector<Converter> converters;
    struct Choice {
        std::string name;
        double value = 0.0, value2 = 0.0;
        bool active = false;
    };
    std::vector<Choice> gainModes;
    std::vector<Choice> decimations;
    // API 0.3 8.7: the plugins muting the audio now, in the order Engine::mutedBy_ names them; `key` is the
    // module file name; `kept` is the mute held after a tune-away (Engine::muteKeptRunning_).
    struct MuteCause {
        std::string name, key;
        bool kept = false;
    };
    std::vector<MuteCause> muteCauses;
    // API 0.3 8.7: the tune grants no LOADED module holds, in the config's order; `refused` when the host holds
    // a record of that file and it did not load.
    struct StaleGrant {
        std::string file;
        bool refused = false;
    };
    std::vector<StaleGrant> staleGrants;
    struct PatchNode {
        std::uint64_t id = 0;
        std::string name;
        const char* kind = "";
        double x = 0.0, y = 0.0, freqHz = 0.0, rateHz = 0.0;
        bool on = false, refused = false, fault = false;
    };
    std::vector<PatchNode> patchNodes;
    struct PatchWire {
        std::uint64_t from = 0, fromPort = 0, to = 0, toPort = 0;
    };
    std::vector<PatchWire> patchWires;
    struct PatchProblem {
        std::uint64_t node = 0;
        std::string problem, text;
    };
    std::vector<PatchProblem> patchProblems;
    struct PatchPart {
        std::string name, key;
        double feed = 0.0, rateHz = 0.0;
    };
    std::vector<PatchPart> patchParts;
    struct PatchRadio {
        std::uint64_t node = 0;
        std::string device, error;
        double rateHz = 0.0;
        bool open = false, fault = false;
    };
    std::vector<PatchRadio> patchRadios;
    // THE RECORDINGS. What a take IS is fixed when it starts and is in the
    // block; its byte count and its STALLED/FULL state are read LIVE from
    // the recorder (atomics, any thread) so a growing file never rebuilds the
    // block. `recorder` is null for a patch recording (listed, not live).
    struct Recording {
        std::string name, path;
        int kind = 0;                // 1 I/Q, 2 audio, 3 patch
        double rateHz = 0.0;
        bool active = false;
        const void* recorder = nullptr;  // const cascade::core::Recorder*
    };
    std::vector<Recording> recordings;
    // THE FACTS get_setting serves under "fact." - whole texts, and the
    // things a list item's detail[] would cut. `localOnly` ones name paths
    // or radios on this machine and are DENIED to a REMOTE session.
    struct Fact {
        std::string value;
        bool localOnly = false;
    };
    std::map<std::string, Fact> facts;
    // FOXAPI_EVENT_PATCH: what the graph's publish says.
    std::uint64_t patchEpoch = 0;
    std::uint64_t patchGraphHash = 0;
    bool patchRunning = false;
};

// The table, for an interface built for (apiMajor, apiMinor): NULL for a
// different major, and during 0.x for any minor but this engine's own.
const FoxEngineApi* engineApi(std::uint32_t apiMajor, std::uint32_t apiMinor);

}  // namespace cascade::engine

#endif  // CASCADE_ENGINE_FOX_ENGINE_API_HPP
