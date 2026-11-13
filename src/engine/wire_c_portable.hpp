// wire_c_portable.hpp - PORTABLE MODE at the start of the process (wiring lane C, row S1; the model is core/sys_portable.hpp).
//
// A marker file `foxsdr.portable` beside the program switches it on: the program then reads and writes its settings, data and log inside the folder it
// sits in (a USB stick), by pointing APPDATA and LOCALAPPDATA at them BEFORE anything resolves a path (config.cpp, diag_log.cpp and plugin_host.cpp already
// read those two variables). Called from bootProcess, STEP 2b: after the helper modes (which touch nothing) and before the crash handlers (which read the
// log folder). No marker is no change. A marker whose folder cannot be used STOPS the start with a sentence - it never falls back to %APPDATA%, because a
// stick that was meant to be portable must say so (and a stick must not leave its settings on somebody else's PC).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <filesystem>
#include <functional>
#include <string>

namespace cascade::engine {

struct PortableBoot {
    bool portable = false;   // the roots are in force: the two variables were set
    bool refused = false;    // a marker was found and could not be honoured: stop
    std::string message;     // why (for the refusal), or ""
};

// `setEnv(name, value)` is how the two variables are set (the process's environment in the program; a recorder in a test).
PortableBoot applyPortableMode(const std::filesystem::path& exeDir, const std::function<void(const std::string&, const std::string&)>& setEnv);

// The program's own call: the real exe folder, the real environment; on a refusal it also writes the sentence to foxsdr-portable-error.txt beside the program
// (best effort, inside the folder the marker is in) and to the standard error stream. Returns the same result.
PortableBoot applyPortableModeToProcess();

// bootProcess STEP 2c (wire-c, rows S7 / S8): a staged settings import or reset (wire_c_sys.hpp) is carried out BEFORE anything reads the settings. Writes what it did to
// wirec-last-boot.txt beside the settings (the engine publishes it as fact.sys.lastboot) and to the standard error stream. Never throws; nothing staged = nothing happens.
// A run that only prints something (--version, --help, --selftest, --benchmark and the other tool modes) never carries a staged action out: it is for a real start.
void applyPendingSystemActionsToProcess(int argc, char** argv);

}  // namespace cascade::engine
