// crash_handler.hpp - catching the process's own death, and writing something
// an engineer who was not there can act on.
//
// WHAT IS COVERED, and it is deliberately four entry points rather than one:
//
//   SetUnhandledExceptionFilter   structured exceptions - access violations,
//                                 divide by zero, stack overflow, and every
//                                 C++ exception that reaches the top of a
//                                 thread as 0xE06D7363.
//   std::set_terminate            an exception escaping a thread, a noexcept
//                                 function throwing, a failed rethrow. This
//                                 is the path a vendor SDR driver takes when
//                                 it throws out of a stream read, and it is
//                                 NOT an SEH exception the filter would see
//                                 first.
//   SIGABRT                       the net UNDER set_terminate, and it is load
//                                 bearing rather than belt-and-braces. MSVC's
//                                 set_terminate is PER THREAD, so the handler
//                                 installed on the main thread does not apply
//                                 to a thread this application never created -
//                                 which is precisely the vendor-driver case
//                                 above. Measured, not assumed: with only
//                                 set_terminate installed, an exception
//                                 escaping a std::thread killed the process
//                                 with 0xC0000409 and left no report at all
//                                 (tests/test_crash_capture.cpp, kind 1, which
//                                 fails without this registration). abort() is
//                                 where all of those paths converge on
//                                 whatever thread they happen on, and the
//                                 UCRT raises SIGABRT before it fast-fails.
//   _set_invalid_parameter_handler  the CRT's own fail-fast: a bad handle to
//                                 fclose, a printf with a null format. By
//                                 default the CRT kills the process without
//                                 ever raising an exception, so nothing else
//                                 here would see it.
//   _set_purecall_handler         a virtual call on a partially destroyed
//                                 object - a use-after-free that fails
//                                 loudly instead of quietly.
//
// WHAT IS NOT, stated plainly rather than discovered later: __fastfail (the
// /GS stack-cookie failure, 0xC0000409) transfers straight to the kernel and
// no user-mode handler runs. Those appear as an unclean exit with no report -
// which the telemetryCleanExit marker still counts, so they are visible as a
// number even when they are invisible as a report.
//
// WRITING A REPORT FROM A BROKEN PROCESS. Everything the fault path needs is
// prepared while the process is still healthy: the directory is created at
// install time, the crash-file path prefix is rendered into a fixed char
// array, the module table is already a flat array (see diag_report.hpp), and
// the log ring is preallocated storage. The handler itself:
//
//   - allocates nothing (no new, no malloc, no std::string, no iostream);
//   - takes no lock the rest of the process could be holding;
//   - calls no CRT formatting (the integers are rendered by hand, because
//     snprintf can take a locale lock and a locale lock is a lock);
//   - writes with CreateFileA/WriteFile, which are kernel calls, not
//     buffered CRT streams that would need a flush the process may not live
//     long enough to perform.
//
// A minidump is written ONLY when the user has asked for one, and only
// locally: it contains process memory, which on this application can include
// file paths and captured IQ. It is never uploaded. See PRIVACY.md.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_CRASH_HANDLER_HPP
#define CASCADE_CORE_CRASH_HANDLER_HPP

#include <string>

namespace cascade::core {

struct CrashHandlerConfig {
    // Where reports go. Created at install time on the healthy path WHEN
    // `enabled` is true; when it cannot be created, capture is disabled rather
    // than attempted from the fault path. When `enabled` is false the path is
    // remembered but the directory is NOT created - that is what lets the
    // Settings toggle arm capture mid-session in a run that started with
    // diagnostics off, without leaving a folder behind on a machine where the
    // user never turns it on.
    std::string crashDir;

    // The master switch. False means no directory, no report, no minidump -
    // nothing on disk at all. The handlers are still installed so the one-line
    // stderr attribution stays available to a terminal user, but they write
    // nothing.
    bool enabled = true;

    // A full minidump beside the text report. OFF by default and gated on
    // explicit consent: a minidump is process memory.
    bool minidump = false;

    // TEST HOOK. Normally the SEH filter returns EXCEPTION_CONTINUE_SEARCH
    // once the report is written, so the process dies with the original
    // exception code and Windows' own error reporting still runs - the
    // behaviour a user's machine has always had. A test child wants to die
    // immediately and quietly instead, with no Windows Error Reporting dialog
    // to block an unattended run. Only the child processes in
    // tests/test_crash_capture.cpp set this.
    bool exitAfterReport = false;
};

// Install as early in main() as possible - before anything that could fault
// has had a chance to. Idempotent; a second call replaces the configuration.
void installCrashHandlers(const CrashHandlerConfig& cfg);

// Update the enabled/minidump switches without re-installing, for when the
// user changes them in Settings mid-session.
void setCrashCaptureEnabled(bool enabled, bool minidump);

// The path of the most recent report this process wrote, or empty. Set from
// the fault path with a plain memcpy into fixed storage, so reading it after
// a caught fault in a child process is safe.
std::string lastCrashReportPath();

// WHERE THIS PROCESS WOULD WRITE A REPORT RIGHT NOW, or empty when it would
// write none. One call answers both halves - "is there a directory" and "did
// the user consent" - because the two are only ever useful together and
// answering them separately is how a caller ends up writing into a directory
// the user asked never to be created.
//
// It exists so that capture can be HANDED TO A CHILD PROCESS. Since 0.62.1
// the SDR device walk runs in `cascade --enumerate-json`, a process that is
// expected to die occasionally by design - and a child that installed no
// handler would turn the most crash-prone path in the product from a
// symbolised report into an exit code. source/soapy_enum_proc.cpp passes this
// string on the child's command line, and passes nothing when it is empty, so
// the child's capture is exactly the parent's consent and never more.
std::string activeCrashDir();

// A fault that was ABSORBED rather than fatal, filed so that it is still
// visible to the report reader and the uploader.
//
// WHY THIS EXISTS. source/vendor_guard.cpp can swallow an access violation
// raised inside a third-party SDR module so that the Source menu answers "no
// devices" instead of the receiver dying mid-session. Everything above still
// runs, which is the point - but it also means SetUnhandledExceptionFilter
// never sees that fault, so without this the ONLY trace is one local
// diagnostics line: no report, no upload, nothing to symbolise. A guard that
// silently deletes the evidence for the crash reporting shipped in 0.62.0 is
// not an improvement over crashing.
//
// The report is written by the SAME allocation-free writer the fatal path
// uses, into the SAME crash-<stamp>.txt naming, with the SAME field set - so
// tools/report-reader resolves it and core/crash_upload.cpp forwards it,
// neither of which needed a new code path. `reason` is what distinguishes it:
// it says the fault was absorbed and the process continued. The `kind: crash`
// line is unchanged and deliberately so - the uploader refuses any other kind
// (crash_upload.cpp), and an unforwardable report would defeat the purpose.
//
// MUST BE CALLED FROM THE __except FILTER, not the handler body:
// EXCEPTION_POINTERS are only valid while the faulting frame is still live.
// `exceptionPointers` is an EXCEPTION_POINTERS* (void* so this header stays
// free of <windows.h>); nullptr is accepted and costs only the faulting
// thread's stack instead of the faulting one. No-op when capture is disabled.
void reportAbsorbedFault(const char* reason, unsigned long code, const void* faultAddress,
                         void* exceptionPointers);

// TEST HOOK. Raises a real fault of the requested kind so a child process can
// prove the handler catches it and writes a readable report. A crash handler
// that has never caught a crash is a hypothesis; this is how it stops being
// one. Never called by the application.
//
//   0  access violation (null dereference)
//   1  std::terminate (exception escaping a thread)
//   2  pure virtual call
//   3  CRT invalid parameter
enum class TestFaultKind { AccessViolation = 0, Terminate = 1, PureCall = 2, InvalidParameter = 3 };
void raiseTestFault(TestFaultKind kind);

}  // namespace cascade::core

#endif  // CASCADE_CORE_CRASH_HANDLER_HPP
