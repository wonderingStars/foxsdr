// Whose console this is (0.99.69, core/console_owner.hpp): the decision, both
// branches of it. The Win32 half - AttachConsole and the CRT streams behind it - is
// proved against the real cascade.exe by test_console_owner_app.cpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstring>

#include "core/console_owner.hpp"
#include "test_check.hpp"

using cascade::core::ConsoleAction;
using cascade::core::ConsoleDecision;
using cascade::core::decideConsole;

int main() {
    // NO CONSOLE AT ALL - what every start of the windows-subsystem cascade.exe
    // sees, whoever started it, and what a failed GetConsoleProcessList reports:
    // try the parent's. A tool mode needs the terminal it was typed into; a session
    // started from the Start Menu finds there is none to attach and carries on
    // without one.
    CHECK(decideConsole(0).action == ConsoleAction::Attach);

    // A console that is already attached - alone on it, or shared with a shell, a
    // harness or a debugger - is not ours to touch, however many are on it.
    CHECK(decideConsole(1).action == ConsoleAction::Keep);
    CHECK(decideConsole(2).action == ConsoleAction::Keep);
    CHECK(decideConsole(3).action == ConsoleAction::Keep);
    CHECK(decideConsole(1000).action == ConsoleAction::Keep);

    // Every decision says why, in words a log line can carry, and the two differ.
    const ConsoleDecision attach = decideConsole(0);
    const ConsoleDecision keep = decideConsole(2);
    CHECK(attach.reason != nullptr && std::strlen(attach.reason) > 10);
    CHECK(keep.reason != nullptr && std::strlen(keep.reason) > 10);
    CHECK(std::strcmp(attach.reason, keep.reason) != 0);

    // Before applyConsoleOwnership has run there is no outcome to report: Keep, no
    // reason, nothing attached. (This test never calls it: it would touch the
    // console of whatever runs the suite.)
    const cascade::core::ConsoleOutcome none = cascade::core::consoleOutcome();
    CHECK(none.decision.action == ConsoleAction::Keep);
    CHECK(!none.attached);

    return testSummary("test_console_owner");
}
