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
// number even when they are invisible as a report. Since 0.99.64 a watcher
// OUTSIDE the process writes one for them (core/sentinel.hpp): it sees the exit
// code, and this file's own reports are how it knows not to write a second. Since
// 0.99.66 that report also says WHERE, on Windows, from the one place a fast-fail
// is recorded: the "Application Error" event Windows Error Reporting writes to the
// Application log (core/os_crash_record.hpp) - the faulting module's file name and
// the offset in it, never a stack, and never a path.
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

#include <chrono>
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

    // ONE LINE ON STDOUT AS THE HANDLER STARTS, for a process whose stdout is
    // read by its parent: the SDR enumeration child (source/soapy_enum_proc),
    // and nothing else. Written before the report, with a raw WriteFile/write
    // on the handle captured at install time, so it reaches the parent even
    // when the report cannot be finished:
    //
    //   cascade-fault: access violation 0xC0000005 at libusb-1.0.dll+0x10490
    //
    // and, when the handler could not run at all (a fault INSIDE it, or
    // another thread's report that never finished), a line that says so -
    // see kFaultLinePrefix below. Field report F204602B5329B268: a child died
    // with 0xE0000002 and the parent could say nothing about what it died of.
    bool faultLineToStdout = false;

    // WHAT THIS PROCESS IS, said on the end of the `reason:` line of every
    // report it writes (2026-10-04). Rendered by the caller on the healthy
    // path and copied here into fixed storage - the fault path only copies
    // those bytes out, it formats nothing. Empty (the default) adds nothing.
    //
    // It exists for the enumeration child (source/soapy_enum_proc.cpp): its own
    // report is the one with the stack, and the parent no longer files a second
    // report for the same death, so the facts only the parent's report used to
    // carry - which driver the child was asked about, which attempt it was,
    // that the application survived it - have to be in the child's. The
    // `reason` line is the one field already uploaded verbatim, so nothing new
    // leaves the machine. Printable ASCII only, and at most 127 characters:
    // anything else is replaced with '?' or cut, because a report is
    // "name: value" lines and the text may come from a third party's driver
    // name.
    std::string reasonSuffix;

    // WHAT THE SIGNATURE HASHES IN PLACE OF A MODULE NAME when the faulting
    // address belongs to no module at all (2026-10-04): private memory, a
    // jump through a freed pointer. Empty (the default) keeps the "?" every
    // such fault has always hashed, which is one group per exception code - so
    // two different drivers faulting that way on one machine were one
    // signature, and one upload a day.
    //
    // It exists for the enumeration child, for the reason reasonSuffix does:
    // the parent used to file a report per death under a tag of its own
    // ("enumerate-child:driver=uhd", source::childFaultSignatureTag), which
    // kept such faults apart per driver and is no longer filed when the child's
    // report covers the death. A fault that DOES resolve to a module is
    // unaffected: it groups by that module and offset, so one fault is one
    // signature whichever walk met it, and two drivers' faults - in two modules
    // - are two. Same limits as reasonSuffix.
    std::string unresolvedSignatureTag;
};

// The prefix of every line faultLineToStdout writes. Shared with the parent
// that reads them, so the writer and the reader cannot drift apart.
constexpr const char* kFaultLinePrefix = "cascade-fault: ";

// THE START OF THE `reason:` LINE OF AN ABSORBED VENDOR FAULT, which
// source/vendor_guard.cpp files from its __except filter and then carries on
// from. Shared so that crashReportWrittenByProcess below can tell such a report
// from the report of a DEATH, and so that vendor_guard.cpp can static_assert its
// own wording still starts with it.
constexpr const char* kAbsorbedFaultReasonPrefix = "fault in a third-party SDR module, absorbed";

// THE START OF THE `reason:` LINE OF THE PARENT'S REPORT OF AN ENUMERATION CHILD'S
// DEATH (source/soapy_enum_proc.cpp), and the second kind of report that is
// written by a process that SURVIVED. Shared so that crashReportWrittenByProcess
// can tell it from the report of a death: until 0.99.64 it was only ever asked
// about a CHILD's process id, whose own reports never begin this way, and the
// sentinel now asks it about the APPLICATION's - whose folder entries include
// these, for deaths of its children that it carried on from.
constexpr const char* kChildDeathReasonPrefix = "SDR device enumeration child process died";

// Compile-time prefix test, for the static_assert above's one user.
constexpr bool reasonStartsWith(const char* reason, const char* prefix) noexcept {
    for (; *prefix != '\0'; ++prefix, ++reason) {
        if (*reason != *prefix) { return false; }
    }
    return true;
}

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

// DID THE CRASH HANDLER OF PROCESS `pid` WRITE A REPORT OF ITS OWN DEATH INTO
// `crashDir`? (2026-10-04.) HEALTHY PATH ONLY: it lists a directory and reads
// files, none of which a fault path may do.
//
// WHY. A child that dies leaves up to three reports for one death - its own,
// written by the handler it armed (it has the stack and the module list), and
// the parent's stackless report of the same death. The parent now files its own
// only when the child left none, and this is how it knows. It is the PARENT
// asking about a CHILD that has already exited: the report is named with the
// child's process id (buildReportPath), and is on disk before the parent
// observes the exit, because the handler writes it and only then terminates.
//
// A report counts only if ALL of these hold, each of them a way the answer
// would otherwise be wrong:
//
//   - the file name carries `pid` as its process-id field - the second-to-last
//     "-"-separated field before ".txt", which is where both writers put it
//     ("crash-<stamp>-<pid>-<seq>.txt", the stamp being a calendar time on
//     Windows and epoch seconds on Linux). Matched whole, so 4242 is not 14242;
//   - it was last written no earlier than `notBefore` (less two seconds of file
//     system slack): process ids are reused, and a crash directory is where
//     reports accumulate for weeks, so an old report of an unrelated process
//     that happened to hold this number must not suppress a death's only record;
//   - its header is a whole crash report: `kind: crash` and a 16-digit
//     `signature:`, which is what the uploader needs to send it at all. A file
//     the handler created and never wrote to (the process was killed between
//     the two) is not a report and must not stand in for one;
//   - its `reason:` is not an ABSORBED vendor fault (kAbsorbedFaultReasonPrefix)
//     and not a parent's report of a CHILD's death (kChildDeathReasonPrefix, or
//     the writer's own default "child process fault (contained)"): a process that
//     absorbed a fault and carried on writes a report with its own process id too,
//     and if it then dies by a route the handler never sees (a heap corruption, a
//     vendor TerminateProcess) that earlier file is not the report of the death.
bool crashReportWrittenByProcess(const std::string& crashDir, unsigned long pid,
                                 std::chrono::system_clock::time_point notBefore);

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

// THE SAME THING FOR A FAULT IN A CHILD PROCESS, and it is a separate entry
// point because the one fact that matters is the one reportAbsorbedFault cannot
// express: this process did not fault, so it has no stack worth capturing.
//
// WHY IT EXISTS. `cascade --enumerate-json` is expected to die occasionally by
// design (the contained libusb fault under an old UHD), and
// source/soapy_enum_proc.cpp files a report at every death so the containment
// does not make the fault invisible - since 2026-10-04, at every death the
// child's own crash handler did not already report (crashReportWrittenByProcess
// above), so that one death is one report. It filed it through reportAbsorbedFault
// with no exception pointers, which walked the CURRENT thread's stack - the
// std::async worker of the device scan, the one caller that arrives here with a
// nearly spent stack. Twice: B9D41A8D on 0.64.0, which the __try in
// captureFramesGuarded was added for, and then "crash cascade.exe @
// captureFramesGuarded" on 0.96.3, where the fault was a STACK OVERFLOW and the
// __try had no room to run either. The parent survived the child's death and
// was then killed by the act of writing the report about it.
//
// So this path never walks at all. The report carries the reason, the child's
// exit code and which attempt it was; the stack section says in words that the
// fault was in another process. No minidump is written either - a dump of the
// process that SURVIVED cannot document the fault and is the most revealing
// artefact this product can put on a user's disk.
//
// `signatureTag` stands in for the faulting module in the report's signature
// (the fault's module is in another process, so this one has none to give).
// Null keeps the pre-0.99.33 "?" - one group per exit code, whatever died -
// which is what source::childFaultSignatureTag exists to end.
void reportAbsorbedChildFault(const char* reason, unsigned long childExitCode, int attempt,
                              const char* signatureTag = nullptr);

// TEST HOOK for the frame-capture policy above. The two properties that matter
// cannot be reached any other way - captureFramesGuarded is internal, and
// neither an exhausted stack nor a zeroed exception context can be staged
// through a real fault from ctest.
//
//   `exceptionPointers`  an EXCEPTION_POINTERS* (void* so this header stays
//                        free of <windows.h>), or nullptr.
//   `mayWalkCurrentThread`  false forbids touching this thread's own stack.
//
// Returns the number of frames captured. A supplied context with a null or
// zeroed ContextRecord returns 0 rather than quietly substituting the calling
// thread's stack for the one that was asked for.
int captureFramesForTest(void* exceptionPointers, bool mayWalkCurrentThread);

// TEST HOOK. Takes the fault path on the calling thread and never gives it
// back, exactly as a handler that is still writing its report holds it - so a
// test child can stage the two cases in which the handler cannot run: a fault
// on THIS thread afterwards (a fault inside the handler) and a fault on
// another thread (a report that never finishes). Neither can be staged
// through a real fault from ctest. Never called by the application.
void holdFaultPathForTest();

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
