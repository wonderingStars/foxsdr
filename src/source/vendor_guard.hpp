// vendor_guard.hpp - running a call into a third-party SDR vendor module
// without letting a fault INSIDE THAT CALL kill the application.
//
// WHY THIS EXISTS. Enumerating SoapySDR devices means loading every vendor
// module the machine has and calling its find function, which walks the USB
// bus through that vendor's own libusb. That is arbitrary third-party code
// touching hardware, and SoapySource::enumerate() has always claimed to
// survive it: "a probe failure in some broken vendor module must not take the
// Source menu down with it". The claim was false as written. The only
// protection was catch (...), and an access violation is a Windows
// STRUCTURED exception, not a C++ one. This build compiles /EHsc (see
// CMAKE_CXX_FLAGS), under which the compiler may assume nothing but a C++
// throw unwinds - so catch (...) never runs for a fault, and the process
// dies. Only __try/__except sees a structured exception. That gap is what
// this file closes.
//
// WHAT IT DOES NOT COVER, stated first because it is the part that matters
// and it was measured, not assumed. A structured exception is delivered on
// the thread that raised it, so this guard covers faults on the CALLING
// thread only. It does NOT cover a fault on a thread the vendor module
// spawned for itself - and that is precisely the fault this machine
// reproduces. Two minidumps of test_soapy_source.exe (0xC0000005 at
// libusb-1.0.dll+0x10490, about one run in thirty with an Ettus B200
// attached) both show the faulting stack bottoming out in
// BaseThreadInitThunk through msvcp140's thread launchpad with uhd.dll
// frames above it, and NOT ONE frame belonging to cascade or SoapySDR.dll:
// it is UHD's own discovery thread, faulting asynchronously to whatever we
// were doing, at 4.8 s and 9.3 s into runs that take 13.6 s. No guard on our
// call frame can catch that. Isolating it needs the enumeration to run in a
// CHILD PROCESS, so that the fault takes the child and the parent reads back
// an empty list. THAT is where the observed crash is actually fixed, and it
// lives in source/soapy_enum_proc.hpp - read that file for the real answer;
// this one closes a smaller, genuine hole in the contract.
//
// HOW THIS INTERACTS WITH THE CRASH HANDLER, which the first version of this
// header did not say and which made it a diagnostics regression.
//
// core/crash_handler.cpp registers a process-wide SetUnhandledExceptionFilter
// (0.62.0) whose entire job is to turn a fault into a report that reaches
// foxsdr.com and gets symbolised. Anything absorbed here NEVER REACHES IT.
// The first version of this guard filtered on the exception CODE alone, so it
// swallowed every access violation on the calling thread - including one
// raised by cascade's OWN code inside the guarded lambda, which would have
// become an empty device list and a single local log line where 0.62.0 would
// have produced an uploadable, symbolised report. Two rules now keep that
// from happening, and both are asserted in tests/test_vendor_guard.cpp:
//
//   1. THE FILTER IS SCOPED BY MODULE, not just by code. A fault is absorbed
//      only when its EXCEPTION_RECORD.ExceptionAddress lies in some module
//      OTHER than the image this guard is compiled into. A fault in our own
//      code is a cascade bug and is NOT absorbed: the filter declines it and
//      it goes to whatever unhandled-exception filter this process installed.
//      A fault whose instruction address belongs to NO loaded module is not
//      absorbed either: an instruction pointer that has left every image is a
//      corrupted control transfer, which is the last thing that should be
//      quietly continued through. The ownership question is answered with
//      VirtualQuery rather than GetModuleHandleEx, because the loader lock is
//      exactly the lock a fault raised inside a LoadLibrary of vendor modules
//      must never wait on - see the .cpp.
//
//      WHICH FILTER THAT IS DEPENDS ON THE PROCESS, and saying "the crash
//      handler, exactly as if no guard existed" was wrong from 0.62.1 on. The
//      guarded walk normally runs in `cascade --enumerate-json`, a CHILD
//      process (source/soapy_enum_proc.hpp), whose main() returns before
//      installCrashHandlers is reached. That child installs its own:
//
//        diagnostics ON   the ordinary handlers, into the directory the parent
//                         passed down, with exitAfterReport - so a declined
//                         fault writes the ordinary symbolised crash report
//                         and then dies with the exception code. This is the
//                         same evidence the in-process path produces.
//        diagnostics OFF  a bare filter that TerminateProcesses with the
//                         exception code and writes nothing - which is what
//                         "off" means, and matches a parent whose capture is
//                         disabled writing nothing either. The parent still
//                         records the death and its exit code.
//
//      In the in-process fallback (no helper could be started) the walk runs
//      under the application's own handlers and the original sentence holds
//      literally.
//
//   2. AN ABSORBED FAULT IS STILL FILED. The filter calls
//      core::reportAbsorbedFault before it handles anything, which writes the
//      ordinary crash-<stamp>.txt through the ordinary writer, so the reader
//      symbolises it and the uploader forwards it. The `reason` line says the
//      fault was absorbed and the process continued. Absorbing a fault costs
//      the process's death; it must not also cost the evidence.
//
// WHAT IS ROUTED THROUGH THIS GUARD, as of 0.62.3: EVERY CALL SOAPYSOURCE
// MAKES THAT CROSSES INTO SOAPYSDR, not just the enumeration walk it was
// written for. Device::make and the whole device interrogation in open(),
// start (activateStream), stop (deactivateStream), teardown (closeStream and
// unmake), setSampleRateHz, setCenterFrequencyHz, setGainDb, setAutoGain,
// listGainNames, listAntennas, setAntenna, antenna, and the module/search-path
// queries. See source/soapy_source.cpp; the audit is per call site.
//
// That widening was forced by measurement, not tidiness. Five crash reports
// came back from the shipped 0.62.0 in 24 hours - at least two people, two
// RTL-SDR models, two Windows 11 builds - in three signatures: the user pressed
// Play (SoapySource::start), the user changed frequency
// (SoapySource::setCenterFrequencyHz, three of the five), the user switched
// source (SoapySource::teardown). Every stack ran cascade -> SoapySDR.dll ->
// rtlsdrSupport.dll -> rtlsdr.dll -> libusb-1.0.dll -> ntdll.dll, and every one
// of them is on OUR OWN CALL FRAME - unlike the B200 enumeration fault above,
// which is why these three are catchable here and that one is not.
//
// EXACTLY ONE VENDOR CALL IS DELIBERATELY LEFT UNGUARDED: readStream, in
// SoapySource::read. See the next paragraph for why that is the line.
//
// WHY SWALLOWING AN IN-CALL FAULT IS THE RIGHT ANSWER, AND WHERE IT ENDS.
// These are all CONTROL calls made on the user's behalf - the Source dropdown
// opening, Refresh being pressed, Play, a retune, a gain change. Answering "no
// devices" or "that tune failed" plus a diagnostic line is a bad answer; taking
// the whole receiver down mid-session, losing an open recording and every
// plugin's state, is a far worse one. Be honest about the cost: a module that
// faulted may have left its own locks held and its own memory torn, so the
// process continues in a state that vendor module no longer guarantees. That
// is a deliberate trade for the control path, and it is NOT a licence to wrap
// the STREAMING path the same way - a fault in a running stream means the
// device is gone, and the pipeline has a real fault path for it. read() runs
// ten times a second forever, so absorbing there would mean calling back into a
// torn module in a hot loop, and rule 2 below would turn one faulting stream
// into a storm of uploaded reports rather than one.
//
// THE COROLLARY THE CALL SITES OWE THIS FILE. Because the process continues in
// a state the vendor no longer guarantees, SoapySource marks a device that has
// faulted DEAD and never calls it again: every setter refuses, and teardown
// drops the handle without closeStream or unmake, leaking it deliberately for
// the life of the process. That is the honest cost of the trade being taken
// here, and the argument is in soapy_source.cpp above teardown(): this guard
// can absorb a second fault, but it cannot absorb a HANG, and a call into a
// module still holding the lock its faulting thread died under is exactly how
// a hang happens.
//
// The filter is deliberately NARROW: only the codes a hardware probe
// realistically raises are handled. Stack overflow in particular is NOT
// handled, because the guard page has already been consumed by the time a
// filter runs and continuing would corrupt the next thread to use the stack.
// A C++ exception (0xE06D7363) is not handled either; it belongs to the
// callee's own try/catch, and letting it search on keeps normal error
// handling exactly as it was.
//
// SoapySDR.dll itself counts as a foreign module by rule 1 above, and that is
// intended: it is a third-party library this tree does not build, its
// enumerate() is what dispatches into every vendor find function, and a fault
// on the way in or out of it is the same class of problem as a fault deep
// inside one. It is absorbed, and - per rule 2 - it is reported.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_SOURCE_VENDOR_GUARD_HPP
#define CASCADE_SOURCE_VENDOR_GUARD_HPP

#include <cstdint>

namespace cascade::source {

// Runs fn(ctx) with structured exceptions raised ON THIS THREAD by faulting
// foreign code turned into a false return.
//
// Returns true when fn ran to completion, false when a fault was absorbed.
// Never throws, and never lets a C++ exception escape fn - the parameter is
// a noexcept function pointer so that the compiler rejects a callee that
// could throw through the guard's own frame.
//
// fn == nullptr returns false without counting a fault (nothing ran).
bool callGuardingVendorFaults(void (*fn)(void*) noexcept, void* ctx) noexcept;

// Total guarded calls ATTEMPTED since process start (fn == nullptr excluded).
//
// This is the observable that lets a test assert a given call site actually
// goes through the guard, without needing a vendor module to fault on cue.
// Deleting the guard from a call site is then a test failure rather than a
// silent return to the unguarded behaviour - see tests/test_soapy_vendor_guard.cpp,
// which counts the control path's crossings, and tests/test_soapy_enum_proc.cpp
// for the enumeration walk's.
std::uint64_t vendorGuardCallCount() noexcept;

// Faults absorbed since process start, and the exception code of the most
// recent one (0 when none). The absorbing call site logs the reason at the
// moment it happens; these accessors exist for tests and diagnostics.
std::uint64_t vendorGuardFaultCount() noexcept;
std::uint32_t vendorGuardLastFaultCode() noexcept;

// THE ABSORB DECISION, as a pure predicate, exposed so that it can be asserted
// directly rather than by faulting.
//
// True means "a fault with this code, at this instruction address, is
// swallowed by callGuardingVendorFaults"; false means it is left to the
// process-wide crash handler and kills the process with a report, exactly as
// it would with no guard installed. See rule 1 in the header notes above for
// what makes an address foreign.
//
// This exists because the alternative way to test rule 1 is to fault inside
// cascade's own image and prove the process dies - which cannot be asserted
// from inside the process that died. `faultAddress` is an instruction
// address; nullptr answers false.
bool vendorGuardAbsorbs(std::uint32_t code, const void* faultAddress) noexcept;

}  // namespace cascade::source

#endif  // CASCADE_SOURCE_VENDOR_GUARD_HPP
