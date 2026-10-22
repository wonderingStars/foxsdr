// web_seat.hpp - THE WEB SERVER'S SEAT ON THE ENGINE, for the tests that stand a browser in front of a real Engine (engine extraction stage
// 5d; docs/engine-stage4.md 6.12).
//
// Before 5d these tests pushed ControlRequests into a queue of their own and installed it as the engine's remote-control source; the engine
// PULLED it. The web server holds an engine-internal session now (net/web_session.hpp), and a test that wants "the browser" opens the same
// session the server opens - engine::openWebSession, the production wrapper over the C table, kind Web: the closed op profile, the REMOTE
// flag, principal "web", the internal tier - and hands it requests through net::handOverControl, which is the whole of what the server's
// POST /api/control does after the parse and the 409 gate. So the tests run the product's code, not a copy of it.
//
// Every call takes a lock, so a test may open and close the seat (the server stopping and starting) while producer threads push into it,
// which is what a flapping server is: a push to a closed seat is a request nobody holds and is dropped, as a POST to a stopped server would
// be refused.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine/server_host.hpp"
#include "foxsdr_api.h"
#include "net/web_control.hpp"
#include "net/web_session.hpp"

namespace web_test {

class WebSeat {
public:
    WebSeat() = default;
    explicit WebSeat(cascade::engine::Engine& e) { open(e); }
    ~WebSeat() { close(); }
    WebSeat(const WebSeat&) = delete;
    WebSeat& operator=(const WebSeat&) = delete;

    // The server starting: a session of kind Web, as WebServer::start opens one once it has bound. False when the engine gave none.
    bool open(cascade::engine::Engine& e, const char* label = "web") {
        std::lock_guard<std::mutex> lk(m_);
        if (s_ != nullptr) { return true; }
        eng_ = &e;
        s_ = cascade::engine::openWebSession(
            e.api(), [&e] { return e.snapshot()->readFull()->bookmarkIds; }, label);
        return s_ != nullptr;
    }
    // The server stopping: the session closes (the table opens a key it held at the start of its next pass).
    void close() {
        std::unique_ptr<cascade::net::WebSession> gone;
        {
            std::lock_guard<std::mutex> lk(m_);
            gone = std::move(s_);
        }
        gone.reset();  // (outside the lock: the close wakes the engine's pass)
    }
    bool isOpen() const {
        std::lock_guard<std::mutex> lk(m_);
        return s_ != nullptr;
    }

    // The page's poll of its status: the session's heartbeat, and NOTHING a PTT re-assertion does.
    void beat() {
        std::lock_guard<std::mutex> lk(m_);
        if (s_ != nullptr) { s_->heartbeat(); }
    }
    // One POST /api/control, after the parse and the gate: false when there is no session (a stopped server) or the translation's context
    // could not be read; true when the commands are with the engine. Does NOT beat: tests that mean "the browser also polled" call beat().
    bool push(const cascade::net::ControlRequest& r) {
        std::lock_guard<std::mutex> lk(m_);
        if (s_ == nullptr) { return false; }
        return cascade::net::handOverControl(*s_, r, &results_) == cascade::net::ControlHandOver::Handed;
    }
    // Pushes as a browser does - it polled its status, then pressed: a beat first, then the request.
    bool pushPolled(const cascade::net::ControlRequest& r) {
        beat();
        return push(r);
    }
    // Every result the session has been given and not yet reported (the pass answers each command of a batch, refusals included).
    std::vector<FoxCommandResult> take() {
        std::lock_guard<std::mutex> lk(m_);
        collectLocked();
        std::vector<FoxCommandResult> out;
        out.swap(results_);
        return out;
    }
    // The status of the newest result carrying `op` among those collected so far, or `none` when there is none (it consumes the results).
    std::int32_t lastStatus(std::uint32_t op, std::int32_t none = -9999) {
        const std::vector<FoxCommandResult> r = take();
        std::int32_t st = none;
        for (const FoxCommandResult& x : r) {
            if (x.op == op) { st = x.status; }
        }
        return st;
    }

private:
    void collectLocked() {
        if (s_ != nullptr) { (void)s_->pollResults(&results_); }
    }

    mutable std::mutex m_;
    std::unique_ptr<cascade::net::WebSession> s_;
    cascade::engine::Engine* eng_ = nullptr;
    std::vector<FoxCommandResult> results_;
};

}  // namespace web_test
