// fox_engine_api.hpp - THE ENGINE BEHIND THE C TABLE (engine extraction stage
// 4, docs/engine-stage4.md): cascade::engine::Engine served through
// FoxEngineApi (third_party/foxsdr_api/foxsdr_api.h, API 0.2), exactly as the
// reference engine serves it (third_party/foxsdr_api/reference/mock_engine) -
// sessions with grants and a keep-alive, tickets, BUSY, the merge of the five
// safer commands, results in ticket order and never before the snapshot that
// shows them, events, logins - with the real receiver behind it.
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

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/freq_manager.hpp"
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
    };
    std::vector<Plugin> plugins;
};

// The table, for an interface built for (apiMajor, apiMinor): NULL for a
// different major, and during 0.x for any minor but this engine's own.
const FoxEngineApi* engineApi(std::uint32_t apiMajor, std::uint32_t apiMinor);

}  // namespace cascade::engine

#endif  // CASCADE_ENGINE_FOX_ENGINE_API_HPP
