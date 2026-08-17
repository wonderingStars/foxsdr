// The web server itself (P11): an HTTP listener that serves a browser client
// for the receiver, gated by net/web_policy.hpp's bind decision and
// net/web_auth.hpp's credentials.
//
// WHAT IT IS NOT COUPLED TO. This class knows nothing about Pipeline, the DSP
// chain, or the GUI. It obtains everything it serves through the two provider
// callbacks below, which the application fills in from the pipeline. That keeps
// the DSP headers out of every translation unit that touches the server, lets
// the tests drive it with stubs instead of a running radio, and means the
// server can never reach into the pipeline for something it was not explicitly
// given.
//
// THREADING. start() spawns one thread that owns the listener; the HTTP library
// serves each request on its own worker. The provider callbacks are therefore
// called from arbitrary threads and MUST be safe to call concurrently — the
// pipeline's own getters already are, which is why they can be wired straight
// in. stop() joins the listener thread before returning, so no handler can be
// running once it has.
//
// TRANSPORT SECURITY. There is none: this speaks plain HTTP, deliberately (see
// third_party/THIRD_PARTY.md for why OpenSSL is not linked). That single fact
// drives the whole design here — the bind policy refuses an off-machine bind
// without a password, the session cookie is SameSite=Strict, and reaching this
// from beyond the LAN is documented as "terminate TLS in front of it", never
// "forward the port".
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "net/web_audio.hpp"
#include "net/web_auth.hpp"
#include "net/web_control.hpp"
#include "net/web_policy.hpp"

// Forward-declared rather than included: httplib.h is 747 KB and belongs in
// exactly one translation unit.
namespace httplib {
class Server;
}

namespace cascade::net {

// Fixed display range for the spectrum sent to the browser. Fixed rather than
// per-frame min/max because a range recomputed each frame makes the waterfall
// breathe: the same signal changes colour as the noise floor wanders.
inline constexpr float kSpectrumDbMin = -140.0f;
inline constexpr float kSpectrumDbMax = 0.0f;

// One consistent snapshot of what the receiver is doing. Assembled by the
// application under whatever locking the pipeline needs, so the browser can
// never show two fields from different instants.
struct RadioStatus {
    bool running = false;
    bool faulted = false;
    std::string faultMessage;
    double centerHz = 0.0;
    double sampleRateHz = 0.0;
    double vfoOffsetHz = 0.0;
    double bandwidthHz = 0.0;
    std::string mode;
    std::string sourceName;
    float signalDb = -200.0f;
    bool stereoActive = false;
    // Included so the browser's own controls can show where they currently
    // sit. Without them the page would have to remember what it last sent,
    // which goes wrong the moment the desktop window changes something.
    float squelchDb = 0.0f;
    float volume = 0.0f;
};

struct SpectrumSnapshot {
    std::vector<float> dbBins;   // fftshifted dB power, oldest-to-newest bin order
    std::uint64_t seq = 0;       // strictly increasing; 0 means nothing yet
    double centerHz = 0.0;
    double spanHz = 0.0;
};

// This machine's own IPv4 addresses, so the settings panel can say "open
// http://192.168.1.20:8073 on your phone" instead of leaving the user to find
// it. Loopback is excluded; an empty result means enumeration failed and the
// caller should simply omit the hint.
//
// It lives here rather than in the GUI because every line of socket code in
// this product belongs in the one translation unit that already owns the
// socket headers and their initialisation.
std::vector<std::string> localInterfaceAddresses();

class WebServer {
public:
    using StatusProvider = std::function<RadioStatus()>;
    // Returns false when no frame newer than `inOut.seq` exists, exactly like
    // Pipeline::getLatestFrame, so the browser can long-poll without copying
    // the same frame twice.
    using SpectrumProvider = std::function<bool(SpectrumSnapshot& inOut)>;
    using Clock = std::function<std::int64_t()>;  // seconds since the epoch

    WebServer();
    ~WebServer();  // stops if running

    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    // Must be set before start(); calling them while running is a no-op that
    // returns false, because a handler could be reading the old one.
    bool setStatusProvider(StatusProvider fn);
    bool setSpectrumProvider(SpectrumProvider fn);

    // Overrides the wall clock, for tests that need session expiry and login
    // throttling to be deterministic. Must be set before start().
    bool setClock(Clock fn);

    // Evaluates the bind policy and, if it allows, binds and starts serving.
    //
    // Returns false with `error` set when the policy refuses (error is the
    // policy's own reason text, suitable for showing verbatim) or when the
    // socket cannot be bound (address already in use, permission denied). The
    // two are distinguishable through decision(): a policy refusal leaves a
    // non-Allowed verdict, a bind failure leaves Allowed.
    //
    // Idempotent in the sense that starting an already-running server stops it
    // first, so applying new settings is one call rather than a stop/start
    // dance the caller has to get right.
    bool start(const WebServerConfig& cfg, std::string& error);

    void stop();
    bool running() const;

    // The port actually bound, which is the configured one unless the config
    // asked for 0. -1 when not running.
    int boundPort() const;

    // The most recent policy verdict, including reachableOffMachine for the
    // "remote access is live" indicator.
    BindDecision decision() const;

    // Live session count, for the settings panel ("2 browsers connected").
    std::size_t sessionCount() const;

    // Ends every session without stopping the server. The settings UI calls
    // this on a password change.
    void revokeAllSessions();

    // --- Control -------------------------------------------------------------
    // POST /api/control validates a request (net/web_control.hpp) and queues
    // it HERE; it never touches the radio, because several of the things a
    // browser can ask for are GUI-thread-only. The application drains this
    // once per frame and applies what it finds.
    //
    // Returns everything accepted since the last call, oldest first, and
    // empties the queue. Safe to call from the application's own thread while
    // requests keep arriving.
    std::vector<ControlRequest> takePendingControls();

    // Bound on the queue. An application that stops draining (a stalled GUI
    // thread) must not let a client grow this without limit; past the cap the
    // OLDEST request is dropped, because for a control surface the most recent
    // instruction is the one that matters.
    static constexpr std::size_t kMaxQueuedControls = 64;

    // --- Audio ---------------------------------------------------------------
    // Publishes newly produced receiver audio (mono, kWebAudioRateHz) for any
    // browser currently listening. The application calls this once per frame
    // with exactly the samples produced since the previous call; the ring
    // (net/web_audio.hpp) gives each listener its own cursor.
    //
    // Safe to call whether or not anyone is listening, and whether or not the
    // server is running — it is a buffer write, not a send.
    void pushAudio(const float* samples, std::size_t n);

    // Browsers currently streaming audio, for the settings panel.
    std::size_t audioListeners() const;

    // Each listener holds one HTTP worker thread for as long as it listens, so
    // this is capped well below the library's pool. Past the cap the request is
    // refused with 503 rather than being accepted and starving the rest of the
    // API — a page that cannot fetch its own status is a worse failure than one
    // that cannot play audio.
    static constexpr std::size_t kMaxAudioListeners = 4;

private:
    class Impl;  // holds the httplib::Server and the route handlers

    std::unique_ptr<Impl> impl_;
};

}  // namespace cascade::net
