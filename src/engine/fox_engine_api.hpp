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
#include <span>
#include <string>
#include <vector>

#include "core/freq_manager.hpp"
#include "core/host_image.hpp"
#include "core/plugin_repo.hpp"
#include "core/plugin_runner.hpp"
#include "foxsdr_api.h"

namespace cascade::engine {

class Engine;
class ApiHostImpl;
struct ApiListBlock;
struct ApiLiveBlock;
struct ApiAirbandBlock;
struct ApiImageSet;

// ENGINE-INTERNAL SESSIONS (stage 5a; docs/api-0.3-additions.md 8.16). The web server and the CAT listener are part of the engine
// (TRANSPORTS 2.6), so each holds a session of its own that no outside client opened: no token, the REMOTE flag (so it can never
// latch, hold ADMIN as a grant, name a path or be armed for transmit), and a CLOSED OP PROFILE - an allow-list fixed when the session
// is opened and enforced at submit, so the session can speak exactly the ops its server speaks today and no other. A violation is
// answered DENIED as a result, like any refusal. Not part of the C table or the vendored header: only code inside the engine opens one
// (ApiHost::openInternalSession), and every other call - submit, poll_results, read_state, read_spectrum, read_audio, heartbeat,
// close_session - is the table's own, on the FoxSession* this returns.
enum class InternalKind : int {
    None = 0,
    Web = 1,  // the browser page: what /api/control can emit, the plugin store and the PTT included
    Cat = 2,  // rigctld: tune, mode and passband, never the transmitter
};
// The ops a kind's profile allows, and the grants besides VIEW it holds. The closure tests read these.
std::span<const std::uint32_t> internalProfileOps(InternalKind kind);
std::uint64_t internalProfileGrants(InternalKind kind);
const char* internalKindName(InternalKind kind);

// The engine's side of the table. One per Engine; built with it.
class ApiHost {
public:
    // Options as FoxEngineParams::options carries them ("k=v;k=v", the
    // reference engine's names): the test timers (pttHoldMs, latchTimeoutMs,
    // keepaliveMs, remoteKeepaliveMs - which may only SHORTEN), token, user,
    // password, maxSessions, maxRemoteSessions, loginLockoutMs, tokenTtlMs,
    // and testHooks=1 (the SETTING_SET "test.*" keys and the dummy
    // transmitter "test:tx:dummy" - never set by the product), with servers=1
    // to let such an engine host its web server and CAT listener as well.
    ApiHost(Engine& engine, const std::string& options);
    ~ApiHost();
    ApiHost(const ApiHost&) = delete;
    ApiHost& operator=(const ApiHost&) = delete;

    // The C handle for this host (never null).
    FoxEngine* handle();

    // The pumping thread's two calls, once a pass (see the file header).
    void beginPass();
    void endPass();
    // STAGE 5a, the pumping thread's third call: the INTERNAL tier (Engine::pumpRemoteControls, the place the pull of the web
    // remote's and CAT's requests has always been - after the local and API queues, the inputs and before the plugins and the
    // scanner). Drains what the internal sessions submitted, the web's before CAT's and each in its own order, then runs the key drive
    // again so a PTT 0 released here releases the Transmitter in THIS pass. Why a tier of its own and not the queue beginPass drains:
    // that queue is in ARRIVAL order, and a browser that arrived first in wall time would then win the key against a local press that
    // landed in the same pass (rule 6, the operator at the machine comes first). Nothing is applied here by any other thread.
    void applyTail();
    // TESTS ONLY - nothing in the product calls it: how many times the pass has run the tier's phase (applyTail), once a pass whether or
    // not the tier held anything. What proves a kicked pass is the whole pass (tests/test_pass_control.cpp K5). Any thread.
    std::uint64_t tailPhasesForTest() const;
    // Whether the engine this host serves runs the web server and the CAT listener it is configured for (stage 5b): yes, unless it was
    // created with testHooks=1 and without servers=1 (a hermetic engine of the tests binds no socket).
    bool hostsServers() const;
    // Opens an engine-internal session (see InternalKind): the C table's handle for it, or null with `status` saying why (FOXAPI_LIMIT
    // beyond the session limits - it counts against the 64 and the 16 REMOTE places like any other - or FOXAPI_BAD_ARGUMENT). Any
    // thread. The caller closes it with the table's close_session.
    FoxSession* openInternalSession(InternalKind kind, const std::string& label, std::int32_t* status = nullptr);
    // TESTS ONLY - nothing in the product calls it: the status submit would give `c` from session `s` at the door (the refusal a result
    // would carry), with nothing queued or applied. Any thread.
    std::int32_t validateForTest(FoxSession* s, const FoxCommand& c);
    // TESTS ONLY - nothing in the product calls it: how many sessions the table holds now, of every kind (InternalKind::None), or only the
    // engine-internal ones of `kind`. Any thread.
    std::size_t sessionCountForTest(InternalKind kind = InternalKind::None);
    // The pumping thread hands over a new list block (before the publish
    // whose listSeq says it changed).
    void setLists(std::shared_ptr<const ApiListBlock> lists);
    // API 0.3 8.12: the pass's copy of what the patch's running nodes are doing (PATCH_LIVE), handed over once a pass
    // when its own fingerprint moved. It moves no listSeq.
    void setPatchLive(std::shared_ptr<const ApiLiveBlock> live);
    // API 0.3 9.6 (stub campaign B6): the pass's copy of the Airband monitor's channels (AIRBAND_CHANNELS), handed over once a pass when its own
    // fingerprint moved. It moves no listSeq, for the reason PATCH_LIVE moves none.
    void setAirbandLive(std::shared_ptr<const ApiAirbandBlock> live);
    // API 0.3 8.12 (part 3): the patch decoders' newest pictures, as ONE immutable set the pumping thread builds when a
    // picture arrived or a node's went (Engine::syncPatchImages) and hands over BEFORE the event that names the picture
    // is queued. read_image serves from it, and nothing else.
    void setPatchImages(std::shared_ptr<const ApiImageSet> images);
    // ...and the event for it: FOXAPI_EVENT_DECODER_IMAGE for the patch decoder `node`, picture `imageId`, broadcast by
    // THIS pass's endPass (after the set above is in place, so a reader that is told can read). The caption is the
    // node's name.
    void patchImageArrived(std::uint64_t node, std::uint64_t imageId, const std::string& caption);
    // API 0.3 8.7: the receiver tuned off the presets of `names` plugins that are still running and muting the
    // audio, and the engine has armed the question (Engine::updateAudioMute, on the pumping thread, AFTER the
    // pass's publish). The event FOXAPI_EVENT_MUTE_TUNE_AWAY is broadcast by the NEXT endPass - after the
    // publish that carries the held mute in MUTE_CAUSES - so an interface that is told finds the plugins
    // already named. Once per call; a call before that endPass replaces the one waiting.
    void muteTuneAway(std::uint32_t names);
    // API 0.3 8.14: a plugin's preset was applied (Engine::applyPluginPreset). The pumping thread's call; the event
    // FOXAPI_EVENT_PRESET_APPLIED (text: the module's file name, what PLUGINS' detail and every plugin op key by) is
    // broadcast by this pass's endPass, once for each application, after the publish that carries what it changed.
    // `presetFlags` is the preset's CASCADE_PRESET_* word, carried in the event's value[0] (stub campaign B5).
    void presetApplied(const std::string& moduleFile, std::uint32_t presetFlags = 0u);
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
    // (The web remote's PTT, keep-alive and "the server stopped" - webRemotePtt, webRemoteBeat, webRemoteStopped - were here until stage 5d:
    // the web server's engine session keys through the table like any client's, its heartbeat is the page's status poll, and its close is
    // the server stopping. docs/engine-stage4.md 6.12.)
    // The pumping thread hands over the decoder-output backlog (API 0.3's
    // FOXAPI_LIST_DECODER_LINES): the lines since the last pass.
    void takeDecoderLines(const std::deque<cascade::core::DecodedLine>& log, std::uint64_t total);
    // The control loop's last act, before it stops the Transmitter.
    void engineStopping();
    // How many times a thread other than the control thread entered the key rules (every entry above, and the pass itself)
    // WHILE the engine's control thread was running. The key rules are single-threaded by contract; this is the tripwire
    // that proves it (pump move step P1, F3): 0 for a correct front end, and the tests assert it.
    std::uint64_t offPassKeyTouches() const;
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
    // API 0.3 8.10: each rate's caption (SAMPLE_RATES' detail), index-aligned with `rates`.
    std::vector<std::string> rateLabels;
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
        // PLUGINS value[3] (stub campaign B2, API 0.3 8.8): bits 0-1 how it runs (core::PluginRun: 0 auto, 1 always, 2 stopped), bit 2 asleep.
        std::uint32_t runWord = 0;
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
        // API 0.3 8.9: the ABI version the entry's build declares, the size of the build for THIS machine as the
        // catalogue states it (0: not stated, or no build), and whether the catalogue publishes a build for this
        // machine's os and architecture at all.
        std::uint32_t abiVersion = 0;
        std::uint64_t sizeBytes = 0;
        bool buildHere = false;
    };
    std::vector<StoreEntry> store;
    // API 0.3 8.9, STORE_ACTIVITY: what the store is doing now, as the pass saw it. Entry 1 is a transfer in
    // flight (a catalogue fetch or a download), entry 2 an ADD ALL run. The progress of the transfer is NOT here:
    // it changes every few hundred milliseconds without anything else moving, so the list reads it from `repo`
    // when it is read (an atomic; the engine owns the repo and outlives the block's readers), as a recording's
    // bytes are read from its recorder.
    struct StoreActivity {
        bool transfer = false;
        int transferKind = 0;  // 1 fetching the catalogue, 2 downloading a module
        std::string transferName;
        const cascade::core::PluginRepo* repo = nullptr;  // for the live progress
        bool addAll = false;
        std::uint64_t addAllPosition = 0, addAllTotal = 0;
        std::string addAllName;
    };
    StoreActivity storeActivity;
    // API 0.3 8.9 (stub campaign B3), STORE_PICTURES: every picture the store was asked for and still knows, by sha256 - Pending (1: queued
    // or in flight), Ready (2: on disk at `path`, its bytes hashed to the catalogue's digest) or Failed (3: `reason`, English). `active`
    // marks the one in flight. The status moves with a transfer, not per pass, so it is a block of its own like the rest (the fingerprint
    // hashes Engine::picturesSeq_).
    struct Picture {
        std::string sha256;
        int state = 0;
        std::string path, reason;
        bool active = false;
    };
    std::vector<Picture> storePictures;
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
        // API 0.3 8.12, PATCH_PARTS value[2]: the module ALSO has a track source (a Map part can show its targets).
        bool tracks = false;
    };
    std::vector<PatchPart> patchParts;
    // PATCH_DEVICES (API 0.3 8.12): the devices a patch Radio can be set to, in Engine::patchDeviceChoices' order.
    // `key` is the Radio node's device key (core/patch_devices.hpp); `rateHz` a recording's own rate, else 0.
    struct PatchDevice {
        std::string label, key;
        double rateHz = 0.0;
    };
    std::vector<PatchDevice> patchDevices;
    struct PatchRadio {
        std::uint64_t node = 0;
        std::string device, error;
        double rateHz = 0.0;
        // API 0.3 8.12: an OPEN radio's AIR centre (its own readback, below 0 Hz through an up-converter), and a
        // radio whose open is still on its worker.
        double centreHz = 0.0;
        bool open = false, fault = false, pending = false;
    };
    std::vector<PatchRadio> patchRadios;
    // PATCH_PLAN (API 0.3 8.12): one entry per planned channel and decoder.
    struct PatchPlanEntry {
        std::uint64_t node = 0;
        bool decoder = false;
        std::string key;               // a decoder's plugin key ("" for a channel)
        const char* source = "";       // a decoder's source: "radio", "channel" or "audio"
        double v[4] = {0.0, 0.0, 0.0, 0.0};
    };
    std::vector<PatchPlanEntry> patchPlan;
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
    // ---- API 0.3 8.10: the Source section and the sound card as their pages draw them
    // UNBOUND_RADIOS: USB radios that are plugged in and cannot be opened. `family`: "rtl", "rx888" or "".
    struct UnboundRadio {
        std::string description, family;
        std::uint32_t vid = 0, pid = 0;
    };
    std::vector<UnboundRadio> unbound;
    // SCAN_EVIDENCE (LOCAL sessions only): kind 1 a vendor install (name, detail: its root), 2 a module search path,
    // 3 a module loaded (detail).
    struct EvidenceRow {
        int kind = 0;
        std::string name, detail;
    };
    std::vector<EvidenceRow> evidence;
    // SOUND_CARDS, SOUND_CARD_RATES and SOUND_CARD_FORM.
    struct SoundCard {
        std::string name, hostApi;
        bool active = false;
    };
    std::vector<SoundCard> soundCards;
    struct SoundCardRate {
        double hz = 0.0;
        bool exclusive = false, active = false;
    };
    std::vector<SoundCardRate> soundCardRates;
    struct SoundCardForm {
        std::string device, hostApi;
        double rateHz = 0.0, centreHz = 0.0;
        int channel = 0;
        bool iqStereo = false, swapIq = false, picked = false;
    };
    SoundCardForm soundCardForm;
    // THE FACTS get_setting serves under "fact." - whole texts, and the
    // things a list item's detail[] would cut. `localOnly` ones name paths
    // or radios on this machine and are DENIED to a REMOTE session.
    struct Fact {
        std::string value;
        bool localOnly = false;
    };
    std::map<std::string, Fact> facts;
    // API 0.3 8.16, SERVERS: the web server and the CAT listener as the engine's server host reported them (engine/server_host.hpp). The
    // counters are the host's own atomics, read when the list is read, so traffic rebuilds no block.
    struct Server {
        std::string name, refusal;
        int port = 0;
        bool running = false, fault = false;
        const void* live = nullptr;  // const ServerHost::Counters*
    };
    std::vector<Server> servers;
    // FOXAPI_EVENT_PATCH: what the graph's publish says. (patchVersion: the graph's version, API 0.3 8.12 - the
    // same number as fact.patch.version, in the same block.)
    std::uint64_t patchEpoch = 0;
    std::uint64_t patchVersion = 0;
    bool patchRunning = false;
};

// PATCH_LIVE (API 0.3 8.12): what the patch's running nodes are doing NOW - a squelch gate, a speaker's level, a
// channel's level, a decoder's newest line - copied once a pass by the pumping thread (Engine::refreshPatchLive) into
// ONE immutable block of its own and handed to the host. It is a block apart from ApiListBlock, and moves no listSeq,
// for the reason the recordings' bytes are read live: these figures change every pass a patch runs, and a rebuild of
// the whole block (its facts, its plugin sizes) and a re-read of every list by every interface, every pass, is the cost
// the live figures must not have. It has a fingerprint of its own: a pass that finds the same readings builds nothing.
// A reader never touches a PatchRadio, a Runner or an AudioDest - their lifetimes end with the pass that erases them.
struct ApiLiveBlock {
    std::uint64_t fingerprint = 0;
    struct Entry {
        std::uint64_t node = 0;
        int kind = 0;                // 1 squelch, 2 speaker output, 3 channel level, 4 decoder face, 5 speaker output error
        double v0 = 0.0, v1 = 0.0;
        bool open = false;           // kind 1: the gate is open
        std::string name, detail;
    };
    std::vector<Entry> entries;
};

// AIRBAND_CHANNELS (API 0.3 9.6, stub campaign B6): the channels the Airband monitor plays, copied once a pass by the pumping thread
// (Engine::refreshAirbandLive) into ONE immutable block of its own, for PATCH_LIVE's reason: a channel's level and heard time change every pass the
// monitor listens, and a rebuild of the whole list block, and a re-read of every list by every interface, every pass, is the cost live figures must
// not have. It has a fingerprint of its own (levels to half a decibel, heard time to a tenth of a second): a pass that finds the same readings builds
// nothing and hands the host nothing. A reader never touches the monitor's channel table - it belongs to the pass.
struct ApiAirbandBlock {
    std::uint64_t fingerprint = 0;
    struct Entry {
        std::uint64_t id = 0;        // the channel's number in the plan, from 1
        std::string name;
        double freqHz = 0.0;
        double bandwidthHz = 0.0;
        double levelDb = -200.0;
        double heardS = 0.0;         // heard since the monitor last wrote it to the list
        bool open = false;           // its squelch is open now
        bool onAir = false;          // it is in the block on the air
    };
    std::vector<Entry> entries;
};

// THE PATCH DECODERS' PICTURES (API 0.3 8.12, part 3): the newest picture of each picture-decoder node, as ONE immutable
// set the pumping thread builds and hands to the host (see ApiHost::setPatchImages). Each picture is itself immutable
// from the moment it is in a set (a `const HostImage` behind a shared pointer, moved in whole from the decoder's runner:
// no second copy of the pixels is ever made by the engine), and a set is never edited, only replaced. So a reader that
// takes the set's pointer and a picture's pointer under the host's short lock holds a picture the decoder will never
// write to: read_image copies it at leisure on the reader's own thread, and a picture the pass replaces meanwhile is still
// whole in the reader's hand. The id is the engine's own counter, never reused.
struct ApiImageSet {
    struct Entry {
        std::uint64_t node = 0;
        std::uint64_t id = 0;
        std::string caption;
        std::shared_ptr<const cascade::core::HostImage> image;
    };
    std::vector<Entry> entries;
};

// The table, for an interface built for (apiMajor, apiMinor): NULL for a
// different major, and during 0.x for any minor but this engine's own.
const FoxEngineApi* engineApi(std::uint32_t apiMajor, std::uint32_t apiMinor);

}  // namespace cascade::engine

#endif  // CASCADE_ENGINE_FOX_ENGINE_API_HPP
