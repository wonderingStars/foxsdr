// control_ops.hpp - a validated ControlRequest (web remote, CAT) or a plugin's
// queued control (host API level 1), turned into engine commands.
//
// THE TRANSLATION, AND ONLY THE TRANSLATION (engine extraction stage 1,
// docs/engine-stage1.md). AppWindow::applyControlRequest used to hold its own
// copy of what a mode change, a bookmark tune or a record press does; then
// Engine::applyControlRequest did, applying in the engine's pass. Since stage
// 5d (docs/engine-stage4.md 6.12) nothing applies a request: the web server
// (net/web_session.hpp: handOverControl) and the CAT server translate it with
// this and SUBMIT the commands on their engine-internal session, and the pass
// applies them through the table like any other session's - the one function
// a desktop widget's command reaches too (applyCommand). Pure: no window, no
// receiver, no clock - so every field's mapping is pinned by
// tests/test_control_ops.cpp without an application.
//
// ORDER IS PART OF THE CONTRACT. A request carrying several fields is applied
// in the order applyControlRequest always applied them (the source is chosen
// before its antenna, the mode before the bandwidth, the bandwidth before the
// offset whose limit depends on it, the transmit key last), and the commands
// come out in exactly that order.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <vector>

#include "core/app_commands.hpp"
#include "core/plugin_api.hpp"
#include "net/web_control.hpp"

namespace cascade::net {

// What the translation needs from the receiver and nothing more - read by the
// caller at the moment of translation (on the web server's HTTP thread since
// stage 5d; the commands are applied a pass later, by the engine).
struct ControlOpsContext {
    // bookmarkTune / bookmarkRemove name a ROW of the list the web status
    // published; the command names the bookmark's id. Row r's id, or 0 when
    // the row no longer exists.
    std::vector<std::uint64_t> bookmarkIdByRow;
    // scannerActive=true starts a scan over the stored range, and SCANNER_RUN
    // carries its range: the stored one, with this request's own
    // scanStartHz/scanStopHz/scanStepHz (applied first) merged in.
    double scanStartHz = 0.0;
    double scanStopHz = 0.0;
    double scanStepHz = 0.0;
};

std::vector<cascade::core::cmd::QueuedCommand> controlRequestToCommands(
    const ControlRequest& r, const ControlOpsContext& ctx);

// One plugin control (the host API's set_frequency ... set_running), as the
// command a desktop control of the same kind sends. False for a kind with no
// command (none today); `out` is then untouched.
bool pluginControlToCommand(const cascade::core::PluginControl& c, FoxCommand& out);

}  // namespace cascade::net
