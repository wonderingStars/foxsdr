// web_session.cpp - see web_session.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/web_session.hpp"

#include "net/control_ops.hpp"

namespace cascade::net {

ControlHandOver handOverControl(WebSession& session, const ControlRequest& request, std::vector<FoxCommandResult>* results) {
    // First: what the pass has answered since the last request. A session that is not read is answered BUSY past 256 outstanding commands,
    // and this is the one place that reads it for a browser that only ever posts.
    session.pollResults(results);

    // The context is taken NOW, at the post. The rows of a bookmark request name the ids of the block /api/status is serving at this moment
    // (an entry removed since answers NOT_FOUND when the pass reaches it; one inserted since moves no other row), and a scan that is started
    // carries the range the last publish stored, with this request's own range fields merged in by the translation.
    const bool wantIds = request.bookmarkTune.has_value() || request.bookmarkRemove.has_value();
    const bool wantRange = request.scannerActive.value_or(false);
    WebControlContext have;
    if ((wantIds || wantRange) && !session.context(wantIds, wantRange, have)) { return ControlHandOver::NoContext; }
    ControlOpsContext ctx;
    ctx.bookmarkIdByRow = std::move(have.bookmarkIdByRow);
    ctx.scanStartHz = have.scanStartHz;
    ctx.scanStopHz = have.scanStopHz;
    ctx.scanStepHz = have.scanStepHz;

    const std::vector<cascade::core::cmd::QueuedCommand> commands = controlRequestToCommands(request, ctx);
    if (!commands.empty()) { (void)session.submit(commands); }
    session.pollResults(results);
    return ControlHandOver::Handed;
}

}  // namespace cascade::net
