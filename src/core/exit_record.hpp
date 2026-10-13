// exit_record.hpp - the application writes down how it ended (0.99.78, Linux).
//
// WHY. A Linux sentinel is its application's CHILD (core/sentinel_host_posix.cpp), and the
// kernel gives a process's exit status to its parent only: waitid(2) answers ECHILD for any
// other process. Field report A0E0967441A8E6B4 (0.99.75, Ubuntu 24.04, an AppImage) ended in
// the phase "creating the window" and could say only `code: unknown`. So the application says
// how it ended itself, into the one place the watcher can read after it has gone - the shared
// page (core/breadcrumb.hpp, `noteExit`):
//
//   - exit() and return from main: an on_exit hook records the STATUS (glibc; on another C
//     library nothing is registered and the report says no record was found);
//   - a signal that would end the process with nothing in the way (SIGHUP, SIGINT, SIGQUIT,
//     SIGTERM, SIGPIPE): a handler records the number, restores the default disposition and
//     raises the signal again, so the process ends exactly as it would have, with the exit
//     status the shell and the kernel see unchanged. A signal somebody had chosen to ignore
//     or handle (a disposition other than the default) is left alone;
//   - the fault signals are recorded by the crash handler (crash_handler_posix.cpp), which
//     already catches them.
//
// WHAT NO RECORD MEANS: SIGKILL (the out-of-memory killer, `kill -9`), which cannot be caught,
// or an _exit() from a place that does not record. The report says that nothing was found and
// which sources were asked; it does not guess.
//
// The writes are one relaxed store into a page that already exists, so they are safe in a
// signal handler. The hook and the handlers are installed only where a watcher can exist
// (main.cpp, with the sentinel). A no-op on every platform but Linux.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_EXIT_RECORD_HPP
#define CASCADE_CORE_EXIT_RECORD_HPP

namespace cascade::core {

// Idempotent. Call once, from the main thread, before the sentinel is started.
void installExitRecording();

}  // namespace cascade::core

#endif  // CASCADE_CORE_EXIT_RECORD_HPP
