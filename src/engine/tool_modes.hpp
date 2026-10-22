// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
//
// The process's headless tool modes (engine extraction stage 7a: moved out of
// src/main.cpp, unchanged). See tool_modes.cpp for what each one checks.
//
// WHAT THIS HEADER IS FOR. process_boot.cpp parses the command line and, when
// the line asked for one of these checks, hands the request here AFTER the parse
// has finished and BEFORE the config is wired or a window exists. The five
// runners are also public so a test can call a check without spawning a process,
// but nothing outside process_boot.cpp does today.
//
// Engine-clean: no gui/, imgui, GLFW or OpenGL (the fence is checked at
// configure time over src/engine).
#pragma once

#include <optional>

namespace cascade::engine {

// What the command line asked for. All false / zero is "no tool mode": the
// process is a session.
struct ToolRequest {
    bool selftest = false;      // --selftest
    bool soapyCheck = false;    // --soapy-check (hidden: needs hardware)
    bool rtlsdrCheck = false;   // --rtlsdr-check (hidden: needs a WinUSB dongle)
    bool recordCheck = false;   // --record-check (hidden: ~1 s of wall time)
    bool toneCheck = false;     // --tone-check (hidden: plays sound)
    double rdsCheckMhz = 0.0;   // --rds-check <MHz> (hidden: needs an RDS station)
};

// Each returns the process exit code (0 pass, 1 fail) and prints its verdict.
int runSelftest();
int runSoapyCheck();
int runToneCheck();
int runRdsCheck(double centerHz);
int runRecordCheck();

// The one dispatch: the FIRST requested mode, in the order the checks have
// always been tried (selftest, soapy, rtlsdr, record, tone, rds), or nullopt when
// none was requested. `argc`/`argv` are the whole command line, because
// --rtlsdr-check takes the rest of it (a second argument names the dongle).
std::optional<int> runToolMode(const ToolRequest& req, int argc, char** argv);

}  // namespace cascade::engine
