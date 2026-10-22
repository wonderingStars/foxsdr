// web_request_seam.hpp - A BROWSER'S CONTROL REQUEST APPLIED DIRECTLY, for the tests that are about what the RECEIVER does with the request and
// not about how it is delivered (engine extraction stage 5d; docs/engine-stage4.md 6.12).
//
// Until stage 5d these tests called Engine::applyControlRequest, a private copy of the translation that read the engine's own fields and
// applied each command at once. The copy is gone: the web server translates on its HTTP thread (net::handOverControl) and the commands reach
// the engine through the web server's session, in a pass. The delivery is held by test_remote_control_pull, test_remote_key_release,
// test_transmit_dead_man and test_web_control_engine; THIS is the receiver's half - the product's own translator (net::controlRequestToCommands,
// unchanged), its context taken from the engine's PUBLISHED block exactly as the session takes it (the bookmark ids of the Full block, the
// scanner's stored range of the state), and every command handed to Engine::applyCommand on the calling thread. The caller is on the pass's
// thread, or has the pass out of the way (passseam), as it always was for applyControlRequest.
//
// A TX_PTT in a request is REFUSED by applyCommand since 5d (the key is a session's): the key's tests go through web_seat.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include "core/receiver_snapshot.hpp"
#include "engine/engine.hpp"
#include "net/control_ops.hpp"
#include "net/web_control.hpp"

namespace web_test {

inline void applyRequestDirect(cascade::engine::Engine& e, const cascade::net::ControlRequest& r) {
    cascade::net::ControlOpsContext ctx;
    if (r.bookmarkTune.has_value() || r.bookmarkRemove.has_value()) { ctx.bookmarkIdByRow = e.snapshot()->readFull()->bookmarkIds; }
    cascade::core::PublishedState p;
    if (e.snapshot()->read(p)) {
        ctx.scanStartHz = p.rx.scanStartHz;
        ctx.scanStopHz = p.rx.scanStopHz;
        ctx.scanStepHz = p.rx.scanStepHz;
    }
    for (const cascade::core::cmd::QueuedCommand& q : cascade::net::controlRequestToCommands(r, ctx)) { (void)e.applyCommand(q.c, q.longText); }
}

}  // namespace web_test
