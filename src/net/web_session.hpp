// web_session.hpp - THE WEB SERVER'S LINE TO THE ENGINE (engine extraction stage 5d; docs/engine-stage4.md 6.12, docs/api-0.3-additions.md 8.16.8).
//
// The browser page's control used to go through a queue of the server's own (WebServer::takePendingControls), which the engine's pass PULLED
// and applied by a private copy of what each field does (Engine::applyControlRequest), keyed the transmitter by a fake session of its own
// (ApiHost::webRemotePtt, kWebSessionId) and judged the server's liveness by a flag read at the pull (WEB_CONTROL_STOPPED). It is now ONE
// ENGINE-INTERNAL SESSION per running server, shared by every browser: the engine implements this interface over the C table
// (engine/server_host.cpp, `TableWebSession`: every call is the table's own on the FoxSession* that ApiHost::openInternalSession(Web)
// returned), the server opens it when it starts - after the bind has succeeded - and closes it when it stops - after the last of its
// threads has ended - so a stopped server holds no session and a session that holds a key is closed by the stop (the table opens the key at
// the next pass). net/ names no engine header.
//
// THE HTTP THREAD DOES THE TRANSLATING. A POST /api/control is parsed (strict: web_control.hpp), translated into the commands a desktop
// widget would send (control_ops.hpp, UNCHANGED - the translator's output type is why its tests survive) and handed to the session in ONE
// batch; the engine applies them in its internal tier, after the local operator's and the API clients' commands of the pass the batch lands
// in, whenever the batch arrived (ApiHost::applyTail). Refusals - a command the profile does not hold, a value out of range, a key without
// the desk's consent - arrive as RESULTS and are not surfaced to the browser (the 202 has always meant "accepted, not carried out"; the page
// reads what happened from /api/status).
//
// EVERY CALL IS ANY-THREAD. The server's worker threads call all of these concurrently on the one session; an implementation keeps no state
// of its own (or locks it).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/app_commands.hpp"
#include "foxsdr_api.h"
#include "net/web_control.hpp"

namespace cascade::net {

// What the translation needs from the receiver (net/control_ops.hpp's ControlOpsContext is built from it; a separate struct so that this
// header - which the interface reaches through the web server's - names nothing of the plugin host API control_ops.hpp includes).
struct WebControlContext {
    std::vector<std::uint64_t> bookmarkIdByRow;  // row r's bookmark id (0 for a row that no longer exists)
    double scanStartHz = 0.0;                     // the scanner's stored range, Hz
    double scanStopHz = 0.0;
    double scanStepHz = 0.0;
};

class WebSession {
public:
    virtual ~WebSession() = default;  // closing the session: the engine opens a key it holds at the next pass

    // THE PAGE'S KEEP-ALIVE: the session's own heartbeat. The server calls it for every authorised GET /api/status - the page's poll, four
    // times a second - and for NOTHING ELSE: a PTT re-assertion is not a beat (API 0.2; tests/test_transmit_dead_man.cpp holds it).
    virtual void heartbeat() = 0;

    // What the translation needs from the receiver, taken when the request is POSTED (not when it is applied): the ids behind the browser's
    // bookmark rows (the block /api/status is serving), and/or the scanner's stored range (the session's own read_state). False when a
    // figure asked for could not be had - a read that kept overlapping a publish, three times running - and `out` is then not to be used.
    virtual bool context(bool bookmarkIds, bool scanRange, WebControlContext& out) = 0;

    // Hands a request's commands over, in order and in one batch (a table submit is atomic: another thread's batch can never come between
    // them). Returns how many NEW tickets the engine gave; a command answered BUSY at the door (256 outstanding: the engine is not applying)
    // has none and is not taken. A refusal is a result, collected below.
    virtual int submit(const std::vector<cascade::core::cmd::QueuedCommand>& commands) = 0;

    // Collects the results that are ready and returns how many it took (appended to `into` when given). A session that never collects them is
    // answered BUSY past 256 outstanding, so the server calls this around every request that touches the session.
    virtual int pollResults(std::vector<FoxCommandResult>* into = nullptr) = 0;
};

// Opens the session for a server that has just bound; `label` names it. Null when the engine will not give one (its session limits are
// reached): the server then refuses to start, as it would for a port it could not bind. Called from the thread that starts the server.
using WebSessionOpener = std::function<std::unique_ptr<WebSession>(const std::string& label)>;

enum class ControlHandOver {
    Handed,     // the request's commands are with the engine (a refusal, if any, comes back as a result)
    NoContext,  // a figure the translation needed could not be read: nothing was handed over; the caller asks the browser to try again
};

// THE WHOLE OF ONE POST /api/control, after the strict parse and the 409 gate: collect what the engine has answered (so stale unread results
// can never make this request BUSY), take the context the request needs, translate, submit the batch, collect again. Shared by the server's
// route and by the tests that stand a browser in front of an engine, so the tests run the product's code. The server passes no `results` (a
// refusal is not surfaced to the browser, as it never was); a test passes a vector to read them.
ControlHandOver handOverControl(WebSession& session, const ControlRequest& request, std::vector<FoxCommandResult>* results = nullptr);

}  // namespace cascade::net
