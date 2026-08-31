// soapy_enum_proc.hpp - enumerating SDR hardware in a CHILD PROCESS, so that a
// vendor driver faulting mid-probe costs a device list instead of the session.
//
// WHY A WHOLE PROCESS. This machine kills a process about once in twenty
// device enumerations: 0xC0000005 at libusb-1.0.dll+0x10490, with an Ettus
// B200 attached. It was chased twice with in-process defences and neither
// could work, which is measured rather than argued:
//
//   - catch (...) never runs. An access violation is a Windows STRUCTURED
//     exception, and this build compiles /EHsc. Proved by a null store inside
//     a catch (...) that killed the process without entering the catch.
//   - __try/__except does not see it either. A structured exception is
//     delivered on the thread that RAISED it, and two WER minidumps put this
//     fault on a discovery thread UHD spawned for itself: the stack bottoms
//     out in BaseThreadInitThunk with uhd.dll frames above it and NOT ONE
//     cascade or SoapySDR frame. Proved directly - a build of
//     test_vendor_guard.exe that contains the guard AND routes enumerate()
//     through it still died 2 times in 40 runs, at the same address.
//
// A fault on a thread we did not create, in code we did not write, at a moment
// we did not choose, cannot be contained inside our own address space. It can
// be contained by not sharing an address space with it. The child walks the
// USB bus; if it dies, it dies alone, and the parent reads an exit code
// instead of an answer.
//
// See source/vendor_guard.hpp for the smaller hole that IS in-process: a
// vendor module faulting on the CALLING thread. Both are real; only this one
// covers the fault that has actually been reproduced. The child arms the guard
// too - see below.
//
// THE FOUR ANSWERS ARE KEPT DISTINCT, because they mean opposite things to a
// user and were historically collapsed into one silent "no devices" that hid a
// total failure of the module search for several releases:
//
//   Ok             the child answered. `devices` may still be empty, and on a
//                  machine with no vendor modules that is the correct answer.
//   ChildDied      the child faulted or exited nonzero. THIS is the libusb
//                  crash, now survivable. Logged with the exit code.
//   ChildTimedOut  the child was still probing when the budget ran out and was
//                  killed. A wedged USB device, not a missing one.
//   SpawnFailed    there is no helper to run, or Windows refused to start it.
//                  A broken install, and the one case that falls back to
//                  in-process enumeration rather than telling a user with a
//                  working radio that they have no radio.
//
// CRASH CAPTURE IN THE CHILD, because moving the walk out of process moved it
// out of the crash reporting with it, and that was a regression rather than a
// trade.
//
// 0.62.0 shipped a process-wide handler whose entire job is to turn a fault
// into a symbolised report that reaches foxsdr.com. The first version of this
// file put the walk in a process that had none: main() returns into
// runEnumerateHelper() ABOVE installCrashHandlers, and the helper installed a
// bare TerminateProcess filter. Measured consequence: 5 children died with
// 0xC0000005 across a 200-run soak and the crashes directory stayed EMPTY.
// For the most crash-prone path in the product, a fault stack had become an
// exit code.
//
// So the parent hands the child its crash directory - and ONLY when the
// parent's own capture is armed, which is core::activeCrashDir() answering
// both "where" and "did the user consent" in one string. Given one, the child
// installs the ordinary handlers with exitAfterReport, so a fault writes the
// ordinary crash-<stamp>.txt with the real faulting stack and then dies
// immediately, with Windows Error Reporting still suppressed. Given nothing,
// it keeps the fast quiet death and writes no report at all: diagnostics off
// means off, in the child exactly as in the parent.
//
// WHAT IS STILL ONLY AN EXIT CODE, stated plainly: the fault this whole file
// exists for is raised on a thread UHD spawned, and a report for it is
// written by the child's own handler only if that filter runs. When the child
// dies without one - killed by the timeout, or by a fault no user-mode filter
// sees - the parent still files its own ChildDied report, which carries the
// exit code and the PARENT'S stack, not the fault's. The two are told apart
// by the reason line.
//
// THE CHILD CANNOT OUTLIVE THE PARENT. It is started inside a job object with
// KILL_ON_JOB_CLOSE, because the timeout below only protects anyone while
// somebody is still waiting on it - and AppWindow's quit path waits 250 ms for
// an in-flight scan and then DETACHES it (reapPendingSoapyScan). Quitting
// mid-scan would otherwise orphan a child that is wedged inside a USB probe,
// with no parent left to kill it, sitting on a bus whose device is documented
// to wedge, and invisible to everything on screen.
//
// NO CACHE, and it was considered rather than overlooked. A short TTL cache
// would save nothing and break something. It saves nothing because every
// caller is already an explicit act: AppWindow::scanSoapy() enumerates once,
// lazily, when the Source dropdown is first opened, keeps the list in
// soapyDevices_ for every subsequent open, and holds a soapyScanPending_ latch
// that drops a second request while one is in flight - so the dropdown does
// not respawn anything, and the only repeat callers are the Refresh button and
// the web API's scanDevices, which are a user asking for a fresh probe. It
// breaks something because serving those from a cache makes Refresh a control
// that does nothing while looking like it worked, which is precisely the class
// of silent failure this file exists to end. The spawn costs about ten
// milliseconds against an enumeration that takes four seconds; there is
// nothing here worth caching.
//
// SAFE TO CALL CONCURRENTLY, and it had to be made so rather than assumed.
// The obvious spawn - CreateProcess with bInheritHandles TRUE and no handle
// list - gives the child EVERY inheritable handle the parent holds, including
// the write end of another enumeration's pipe. Two overlapping scans would
// then deadlock the FIRST one: its pipe would still have a writer after its
// own child exited, its drain thread would never see end-of-file, and a
// perfectly good answer would turn into a 20 s timeout and a kill - exactly
// the failure the drain-on-its-own-thread design exists to prevent. The spawn
// therefore passes an explicit PROC_THREAD_ATTRIBUTE_HANDLE_LIST naming the
// three handles the child is allowed and nothing else. Today AppWindow drops
// a second scan while one is in flight, so this is not reachable through the
// GUI - but this is a public entry point with a web API caller as well, and
// "only safe because of a latch in one of its callers" is not a property a
// header can promise.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_SOURCE_SOAPY_ENUM_PROC_HPP
#define CASCADE_SOURCE_SOAPY_ENUM_PROC_HPP

#include <string>
#include <vector>

#include "source/soapy_source.hpp"

namespace cascade::source {

enum class EnumOutcome {
    Ok,             // the child answered; devices may legitimately be empty
    ChildDied,      // faulted or exited nonzero - the libusb crash, contained
    ChildTimedOut,  // still probing when the budget expired; killed
    SpawnFailed,    // no helper found, or CreateProcess refused
    Malformed,      // exited 0 but its answer did not parse
};

// For logs and test output. Never nullptr.
const char* enumOutcomeName(EnumOutcome outcome) noexcept;

struct EnumResult {
    std::vector<SoapyDeviceInfo> devices;
    EnumOutcome outcome = EnumOutcome::SpawnFailed;

    // The child's exit code, as Windows reported it. 0xC0000005 here is the
    // libusb fault; it is the single most useful number in this struct and it
    // is why ChildDied is not folded into "no devices".
    unsigned long exitCode = 0;

    // Wall time of the whole attempt, spawn included.
    unsigned long elapsedMs = 0;

    // Reported BY THE CHILD, and load-bearing rather than decorative.
    // `guardedCalls` is how many times the child crossed into SoapySDR through
    // callGuardingVendorFaults - exactly 2 for a completed enumeration on a
    // machine that has the SoapySDR runtime (the one-off module search-path fix
    // in runtimeAvailable(), then the vendor walk itself), 0 when the runtime
    // is missing and both are correctly skipped. It was 1 before 0.62.3, when
    // the walk was the only guarded crossing in the process; see
    // source/vendor_guard.hpp for what widened and why. It is what lets a test
    // assert that the REAL enumeration in the REAL binary is still guarded,
    // without the test having to touch the radio itself. Both are 0 on any
    // outcome other than Ok.
    unsigned long long guardedCalls = 0;
    bool childRuntimeAvailable = false;

    // The child said it armed crash capture: it was handed a directory and it
    // installed the ordinary handlers into it. False means the child was told
    // nothing and died quietly instead, which is the correct answer whenever
    // the user has diagnostics switched off. See CRASH CAPTURE IN THE CHILD
    // below for why this is a reported fact rather than an assumption.
    bool childCaptureArmed = false;

    // The child died or could not be started, and the walk was run in this
    // process instead. Only ever set for SpawnFailed - see below.
    bool fellBackInProcess = false;

    // How many children were started. 2 means the first one died and the
    // retry is what produced this answer.
    int attempts = 0;

    // HOW MANY CHILDREN DIED, and with what, EVEN WHEN THE ANSWER IS Ok.
    //
    // Without these, a successful retry erases the evidence: `outcome` is Ok
    // and `exitCode` is the SUCCESSFUL child's 0, so the fault that was
    // contained leaves no trace in the result at all. That is the wrong way
    // round - a machine quietly losing one enumeration in twenty to a driver
    // fault is exactly what a support conversation needs to know, and it is
    // invisible precisely because the fix worked.
    //
    // Measured on this bench: 1 death in 121 real enumerations across 120
    // separate parent processes, every parent surviving.
    int childDeaths = 0;
    unsigned long deathExitCode = 0;  // the most recent death's code
};

struct EnumOptions {
    // THE BUDGET, and why it is not "forever".
    //
    // MEASURED, on this bench, with a B200 attached and UHD installed: 120
    // consecutive real enumerations took min 4782 ms, median 4815 ms, p90
    // 4850 ms, max 5052 ms. That is a remarkably tight distribution because
    // the time is dominated by UHD's own network-discovery timeouts rather
    // than by anything that varies.
    //
    // A WEDGED USB device has no bound at all, and the caller is
    // AppWindow::scanSoapy() - so with no budget the Source menu says
    // "Scanning for devices..." until the application is killed, which is the
    // shape of the freeze this product already ships hang reports for.
    //
    // 20 s is therefore about four times the worst run ever observed here. The
    // margin is deliberately generous rather than tight, because the cost of
    // the two errors is wildly asymmetric: cutting off a slow-but-working
    // machine tells its owner they have no radio, while waiting an extra ten
    // seconds on a genuinely wedged one costs ten seconds of a menu that
    // already said it was scanning. It is bounded at all so that "wedged"
    // resolves into a stated reason instead of never resolving.
    unsigned long timeoutMs = 20000;

    // ONE RETRY on ChildDied, and none on a timeout.
    //
    // The libusb fault is intermittent at roughly one enumeration in twenty,
    // so a second child usually succeeds and the user sees their radio instead
    // of an empty list; that turns a 5% "no devices" into a 0.25% one for the
    // price of one extra probe on the rare bad run. A TIMEOUT is not retried:
    // it has already cost the full budget, and a device wedged enough to hang
    // one probe hangs the next one too - retrying it doubles the wait to reach
    // the same answer.
    int attempts = 2;

    // Path to the helper executable. Empty means resolve it (see
    // enumerateHelperPath).
    std::string helperPath;

    // Run the walk in THIS process when no helper can be started.
    //
    // The trade, stated plainly: it restores exactly the crash exposure this
    // file exists to remove. It is still the right default, because the
    // alternative is telling a user with a working radio and a slightly odd
    // install that they have no radio - and "no devices" concealing a broken
    // install is a failure this product has already shipped once. Tests turn
    // it off so that a spawn failure is visible as a spawn failure.
    bool allowInProcessFallback = true;
};

// Enumerates in a child process. Never throws.
EnumResult enumerateIsolated(const EnumOptions& options = EnumOptions{});

// The helper this process would run, or empty if it cannot find one. In order:
//
//   1. CASCADE_ENUM_HELPER, if set and non-empty. The support escape hatch,
//      and how a test binary - which is not cascade.exe and has no cascade.exe
//      beside it - points at one.
//   2. cascade.exe in the directory of the running executable, which is the
//      shipping case and where the application finds itself.
//
// The path is not checked for existence here; CreateProcess is the honest
// test of whether it can run, and its failure is reported as SpawnFailed.
std::string enumerateHelperPath();

// HOW THIS PROCESS DIES, if it turns out to be an enumeration helper. Called
// first thing by runEnumerateHelper(), and exposed only so that both of its
// branches can be proved without a radio and without waiting for a real
// driver fault.
//
//   crashDir null or empty  Windows Error Reporting off, and a filter that
//                           TerminateProcesses with the exception code. No
//                           report, no dump, no dialog, microseconds. This is
//                           the shipping behaviour whenever the user has
//                           diagnostics switched off, and it is why "off"
//                           costs a helper nothing.
//   crashDir set            the same WER suppression, plus the ordinary
//                           handlers from core/crash_handler.hpp installed
//                           into that directory with exitAfterReport - so a
//                           fault writes the ordinary crash-<stamp>.txt, with
//                           the real faulting stack, and then dies with the
//                           exception code exactly as before. The parent
//                           reads the same exit code either way.
//
// The directory is created here if it does not exist; it is only ever the one
// the parent already had armed.
void armEnumerateHelperProcess(const char* crashDir);

// THE CHILD SIDE. Runs the vendor walk in this process and writes one line of
// JSON to stdout:
//
//   {"schema":1,"runtime":true,"guardedCalls":1,"capture":false,
//    "devices":[{"label":..,"args":..}]}
//
// Returns 0 on success. Called only from main() when the hidden
// --enumerate-json flag is present, before ANY of the application's start-up
// work: no config is read, no window is opened, no telemetry marker is
// touched. `crashDir` is the one thing the parent hands down - see CRASH
// CAPTURE IN THE CHILD above - and null means "write nothing anywhere".
//
// The child arms the vendor guard too, even though its whole purpose is to be
// expendable: a fault on the child's OWN calling thread is one the guard can
// actually absorb, and absorbing it yields a clean empty list and exit 0 -
// which the parent can tell apart from a death. Free, and strictly more
// information than a corpse.
int runEnumerateHelper(const char* crashDir = nullptr);

// The child's ONE serialisation step, as a named function: everything between
// "the walk produced these devices" and "this is the line on stdout". It
// exists so the invalid-UTF-8 regression (field crash 6477BA87 - a vendor
// label that made dump() throw and killed the child) is testable WITHOUT
// running runEnumerateHelper in a test process. Calling the helper in-process
// arms the child's own quiet-death exception filter and performs a REAL
// vendor enumeration - the full USB walk, in the test - and on a bench with a
// B200 attached that combination killed the test binary about one run in
// twenty with no named failure (the known UHD discovery fault, terminated by
// the very filter the helper had just installed). This function is the
// genuine production code the helper calls, not a copy of it.
std::string enumerationReportJson(bool runtimeAvailable,
                                  unsigned long long guardedCalls,
                                  bool captureArmed,
                                  const std::vector<SoapyDeviceInfo>& devices);

}  // namespace cascade::source

#endif  // CASCADE_SOURCE_SOAPY_ENUM_PROC_HPP
