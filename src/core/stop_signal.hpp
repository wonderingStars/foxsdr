// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
//
// THE OPERATING SYSTEM'S WAY OF SAYING "STOP", as a request the headless engine can read (engine extraction
// stage 7d). `foxsdr-engine` has no window whose close button ends it: a service manager ends it with SIGTERM
// (`systemctl stop`), a terminal with SIGINT (Ctrl+C) or SIGHUP (the terminal closing), and on Windows the console
// delivers Ctrl+C, Ctrl+Break and a closing console. Without a handler each of them ends the process on the spot,
// mid-pass, with the settings unsaved, the radio open and the clean-exit marker FALSE - which the next start
// counts as a crash. With this installed each of them only sets a flag; the engine's own wait loop reads it and runs
// the orderly stop (EngineService::stop), the same one `--seconds` ends in.
//
// WHAT IS INSTALLED
//   Windows  the console handler of core/console_close.hpp, unchanged: one handler for Ctrl+C, Ctrl+Break and a
//            closing console, the window application's own. stopSignalRequested() is its flag. A closing console is
//            given the 4.5 s the system allows before it ends the process whatever this does, so a stop that takes
//            longer than that on a console that is being closed is ended by the system; Ctrl+C and Ctrl+Break have
//            no such limit. (Logoff and shutdown never reach a console program that loads user32.dll.)
//   POSIX    SIGTERM, SIGINT and SIGHUP, with sigaction.
//
// HOW IT COMPOSES WITH THE EXIT RECORDER (core/exit_record.hpp), which is the other thing that owns these signals
// on Linux. The recorder (armed by boot when a sentinel can watch) installs a handler on SIGHUP, SIGINT, SIGQUIT,
// SIGTERM and SIGPIPE - but only on one whose disposition is still the default - that writes the signal's number
// into the shared page and re-raises it, so the process ends exactly as it would have. This module is installed
// AFTER it (process_boot.cpp, the last step), so what it finds beneath it is the recorder's handler (or the default,
// when no sentinel is armed), and it keeps that:
//
//   FIRST stop signal   handled here: one atomic store, nothing else. No record is written and the process is not
//                       ended; the engine stops in order and returns from main, and the recorder's on_exit hook then
//                       records the ordinary exit status (0, or the budget's) - which is the truth.
//   ANY LATER one       hands the signal back to whatever was there before: this handler puts the previous
//                       disposition back and raises the signal again, so it is delivered to it once the handler
//                       returns - the recorder (which records "ended by signal N" and lets the default end the
//                       process), the default (the process ends by that signal, as an operator who has asked twice
//                       expects), or any other handler. A stop that is wedged is therefore never one the operator
//                       cannot end by asking again. (The budget guard, engine/shutdown_guard.hpp, is the one that
//                       ends a wedged stop without being asked twice.)
//   A signal that was IGNORED when this was installed (nohup, a background job's SIGINT) stays ignored.
//
// The handler uses only async-signal-safe calls (sigaction, raise, a lock-free atomic). It sets SA_RESTART, so
// a system call interrupted by it is resumed; a thread of a vendor library that does not retry EINTR would be a
// library bug, not a new one, but nothing here proves there is none (UNVERIFIED on a real radio).
//
// ONE DIFFERENCE BETWEEN THE PLATFORMS, said once: on Windows a SECOND Ctrl+C does nothing more (the console handler
// answers every one the same way, as the window application's does), so there the budget guard is the only thing
// that ends a wedged stop.
#pragma once

namespace cascade::core {

// Installs the handlers, once; later calls do nothing and return true. False when the operating system refused one
// (on POSIX: a signal that could not be set; on Windows: the console handler), and the engine then says so and runs
// without a way to be stopped but a kill.
bool installStopSignals();

// True once a stop has been asked for, by any of them. Never resets. Any thread.
bool stopSignalRequested();

// The POSIX signal number that asked first (SIGTERM, SIGINT or SIGHUP), or 0: none yet, or Windows (a console event
// has no number the handler keeps).
int stopSignalNumber();

// "SIGTERM", "SIGINT", "SIGHUP"; "a console event" on Windows; "" for none. For the line the engine prints.
const char* stopSignalName();

}  // namespace cascade::core
