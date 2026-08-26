// Tests for source/vendor_guard.hpp - the structured-exception guard that
// stands between the application and a faulting third-party SDR vendor module.
//
// WHAT THIS TEST IS FOR, precisely. SoapySource's vendor walk documents that a
// broken vendor module must not take the Source menu down with it, and its
// only protection was catch (...). This build compiles /EHsc, under which a
// C++ handler never runs for a structured exception, so that promise did not
// hold for a module that FAULTS rather than throws. This file pins the guard
// that makes it hold, and the module scoping that keeps it from hiding OUR
// faults while it is at it.
//
// WHAT IS NOT HERE ANY MORE, and why the file is better for it. The first
// version of this test called SoapySource::enumerate() twice against the real
// radio to prove the call site was wired to the guard - and inherited this
// machine's libusb fault while doing it, dying with 0xC0000005 in 2 runs out of
// 40. A test that kills itself at random is not evidence of anything; it just
// adds a second coin-flip to a suite that already had one. The wiring proof now
// lives in tests/test_soapy_enum_proc.cpp, where the enumeration runs in a
// CHILD process and the child reports its own guarded-call count back as data.
// The durability property is unchanged and still fails the suite: deleting the
// guard from the vendor walk leaves that count at zero. Nothing in THIS file
// touches hardware, and nothing in it can fault outside a guard.
//
// The three properties asserted here, and all three have to hold:
//
//   1. THE GUARD ABSORBS A STRUCTURED EXCEPTION raised on its own thread.
//      Deterministically (RaiseException) and then for real (a store through a
//      null pointer performed by ntdll's own memset, so the faulting
//      instruction genuinely belongs to a foreign module). Remove the
//      __try/__except and this file does not fail politely, it dies with
//      0xC0000005, which is exactly the gap.
//
//   2. IT ABSORBS ONLY WHAT IT SHOULD. A fault in cascade's own image is a
//      cascade bug: the filter declines it and it goes to whichever unhandled
//      -exception filter the process installed. In the application that is
//      core/crash_handler.cpp's, producing an uploadable symbolised report; in
//      the enumeration child process, which is where this walk normally runs,
//      it is the one that child installs - the same report writer when the
//      user has diagnostics on, and a silent exit code when they do not (see
//      rule 1 in source/vendor_guard.hpp, and the report-writing half proved
//      end to end in tests/test_soapy_enum_proc.cpp). Asserted here through
//      vendorGuardAbsorbs(), because the alternative - faulting inside our own
//      image and proving the process dies - cannot be checked from inside the
//      process that died.
//
//   3. AN ABSORBED FAULT IS STILL FILED. Swallowing the fault costs the
//      process's death; it must not also cost the evidence. With capture armed
//      into a scratch directory, absorbing a fault must leave a real crash
//      report behind.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/vendor_guard.hpp"

#include "core/crash_handler.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

#include "test_check.hpp"

using cascade::source::callGuardingVendorFaults;
using cascade::source::vendorGuardAbsorbs;
using cascade::source::vendorGuardCallCount;
using cascade::source::vendorGuardFaultCount;
using cascade::source::vendorGuardLastFaultCode;

namespace {

// VOLATILE, and this is load-bearing rather than decoration. The "entered the
// callee but did not return from it" checks below depend on the increment
// before the fault being separable from the one after it; with a plain int,
// MSVC merged the pair into a single g_ran += 2 hoisted above the faulting
// store, and the check read 2 for a callee that never reached its second
// line. Volatile accesses may not be reordered against each other, which is
// exactly the ordering being asserted.
volatile int g_ran = 0;

void runsCleanly(void* ctx) noexcept {
    ++g_ran;
    *static_cast<int*>(ctx) = 42;
}

#ifdef _WIN32
void raisesStructuredException(void*) noexcept {
    ++g_ran;
    // RaiseException runs in KERNELBASE, so the exception address it records
    // belongs to a foreign module - which is what the module-scoped filter
    // requires before it will absorb anything.
    ::RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
    ++g_ran;  // must NOT run; asserted by the caller
}

// A GENUINE access violation whose faulting instruction is in ntdll.
//
// This is not decoration either. A null store written HERE would fault at an
// address inside this test executable, which is the guard's own image - and
// the guard deliberately refuses to absorb those, so the test process would
// die instead of reporting. Calling ntdll's exported memset with a null
// destination puts the faulting store inside ntdll.dll, which is exactly the
// shape of the real case: foreign code, foreign address, our thread.
using MemsetFn = void*(__cdecl*)(void*, int, std::size_t);
MemsetFn g_ntdllMemset = nullptr;  // NON-const: the optimiser must not fold it

void faultsInForeignModule(void*) noexcept {
    ++g_ran;
    g_ntdllMemset(nullptr, 0, 64);  // genuine 0xC0000005, raised inside ntdll
    ++g_ran;                        // must NOT run; asserted by the caller
}

// Any function of ours will do as "an address inside cascade's own image":
// vendor_guard.cpp is linked into this executable, so this file and the guard
// share a module.
void inOurOwnImage() noexcept { g_ran += 0; }
#endif

std::string readAll(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

int main() {
    // --- a callee that returns normally ------------------------------------
    {
        const std::uint64_t calls = vendorGuardCallCount();
        const std::uint64_t faults = vendorGuardFaultCount();
        int out = 0;
        g_ran = 0;
        CHECK(callGuardingVendorFaults(&runsCleanly, &out));
        CHECK(g_ran == 1);                              // the callee really ran
        CHECK(out == 42);                               // ...with our context
        CHECK(vendorGuardCallCount() == calls + 1);     // counted
        CHECK(vendorGuardFaultCount() == faults);       // and not as a fault
    }

    // --- nullptr callee: false, and NOT counted as a fault ------------------
    {
        const std::uint64_t calls = vendorGuardCallCount();
        const std::uint64_t faults = vendorGuardFaultCount();
        CHECK(!callGuardingVendorFaults(nullptr, nullptr));
        CHECK(vendorGuardCallCount() == calls);
        CHECK(vendorGuardFaultCount() == faults);
    }

#ifdef _WIN32
    // --- THE ABSORB DECISION, asserted directly -----------------------------
    //
    // This block is the defect fix. The first version of the filter looked at
    // the exception CODE and nothing else, so an access violation raised by
    // cascade's own code inside the guarded lambda was swallowed into an empty
    // device list and one local log line - blinding, on this one path, the
    // crash reporting shipped in 0.62.0 whose whole job is to turn that fault
    // into a report that reaches foxsdr.com and gets symbolised.
    {
        HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        CHECK(ntdll != nullptr);
        const void* foreign =
            reinterpret_cast<const void*>(::GetProcAddress(ntdll, "NtClose"));
        CHECK(foreign != nullptr);

        // Foreign module, absorbable code: absorbed. This is the whole point
        // of the guard, so it must still say yes.
        CHECK(vendorGuardAbsorbs(EXCEPTION_ACCESS_VIOLATION, foreign));
        CHECK(vendorGuardAbsorbs(EXCEPTION_ILLEGAL_INSTRUCTION, foreign));

        // OUR OWN IMAGE: never absorbed, whatever the code. A bug in cascade
        // dies with a report, exactly as it would with no guard installed.
        const void* ours = reinterpret_cast<const void*>(&inOurOwnImage);
        CHECK(!vendorGuardAbsorbs(EXCEPTION_ACCESS_VIOLATION, ours));
        CHECK(!vendorGuardAbsorbs(EXCEPTION_IN_PAGE_ERROR, ours));

        // NO MODULE AT ALL - a stack address here. An instruction pointer that
        // has left every loaded image is a corrupted control transfer, which
        // is the last fault that should be quietly continued through.
        int onTheStack = 0;
        CHECK(!vendorGuardAbsorbs(EXCEPTION_ACCESS_VIOLATION, &onTheStack));
        CHECK(!vendorGuardAbsorbs(EXCEPTION_ACCESS_VIOLATION, nullptr));

        // Codes outside the narrow list stay outside it even in foreign code:
        // a stack overflow cannot be continued from, and a C++ exception
        // belongs to the callee's own try/catch.
        CHECK(!vendorGuardAbsorbs(EXCEPTION_STACK_OVERFLOW, foreign));
        CHECK(!vendorGuardAbsorbs(0xE06D7363u, foreign));  // C++ throw
        CHECK(!vendorGuardAbsorbs(EXCEPTION_BREAKPOINT, foreign));
    }

    // --- a structured exception is absorbed, deterministically --------------
    {
        const std::uint64_t calls = vendorGuardCallCount();
        const std::uint64_t faults = vendorGuardFaultCount();
        g_ran = 0;
        const bool completed = callGuardingVendorFaults(&raisesStructuredException,
                                                        nullptr);
        // Reaching this line at all is half the assertion: without the
        // __try/__except the process is already gone.
        CHECK(!completed);
        CHECK(g_ran == 1);  // entered the callee, did not return from it
        CHECK(vendorGuardCallCount() == calls + 1);
        CHECK(vendorGuardFaultCount() == faults + 1);
        CHECK(vendorGuardLastFaultCode() ==
              static_cast<std::uint32_t>(EXCEPTION_ACCESS_VIOLATION));
    }

    // --- and a REAL access violation, not a synthesised one -----------------
    {
        HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        CHECK(ntdll != nullptr);
        g_ntdllMemset =
            reinterpret_cast<MemsetFn>(reinterpret_cast<void*>(::GetProcAddress(ntdll, "memset")));
        // Asserted rather than skipped: if this export ever moves, the block
        // below must fail loudly, not quietly test nothing.
        CHECK(g_ntdllMemset != nullptr);

        const std::uint64_t calls = vendorGuardCallCount();
        const std::uint64_t faults = vendorGuardFaultCount();
        g_ran = 0;
        const bool completed = callGuardingVendorFaults(&faultsInForeignModule, nullptr);
        CHECK(!completed);
        CHECK(g_ran == 1);
        CHECK(vendorGuardCallCount() == calls + 1);
        CHECK(vendorGuardFaultCount() == faults + 1);
        CHECK(vendorGuardLastFaultCode() == 0xC0000005u);
    }

    // --- the guard is REUSABLE after a fault --------------------------------
    //
    // Absorbing one fault and then refusing to run anything again would look
    // identical to the fix working, right up until the second time the user
    // opens the Source dropdown.
    {
        int out = 0;
        g_ran = 0;
        CHECK(callGuardingVendorFaults(&runsCleanly, &out));
        CHECK(g_ran == 1);
        CHECK(out == 42);
    }

    // --- AN ABSORBED FAULT IS STILL FILED -----------------------------------
    //
    // LAST, deliberately: it arms the process-wide crash handler, and from
    // this point on every absorbed fault in this process writes a report.
    {
        std::error_code ec;
        const std::filesystem::path dir =
            std::filesystem::temp_directory_path(ec) /
            ("vendor_guard_reports_" + std::to_string(::GetCurrentProcessId()));
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        CHECK(std::filesystem::is_directory(dir));

        cascade::core::CrashHandlerConfig cfg;
        cfg.crashDir = dir.string();
        cfg.enabled = true;
        cascade::core::installCrashHandlers(cfg);
        cascade::core::setCrashCaptureEnabled(true, false);

        const std::uint64_t faults = vendorGuardFaultCount();
        g_ran = 0;
        CHECK(!callGuardingVendorFaults(&faultsInForeignModule, nullptr));
        CHECK(vendorGuardFaultCount() == faults + 1);

        // A report exists, it is the one this fault produced, and it says what
        // happened rather than pretending the process died.
        const std::string path = cascade::core::lastCrashReportPath();
        CHECK(!path.empty());
        const std::string body = readAll(std::filesystem::path(path));
        CHECK(!body.empty());
        CHECK(body.find("absorbed") != std::string::npos);
        CHECK(body.find("0xC0000005") != std::string::npos);
        // Filed under the ordinary kind on purpose: core/crash_upload.cpp
        // forwards `crash` and `hang` and refuses everything else, so any
        // other spelling would be a report nobody ever receives.
        CHECK(body.find("kind: crash") != std::string::npos);

        // Leave nothing behind. Capture is switched off first so a later fault
        // cannot try to write into a directory that is being removed.
        cascade::core::setCrashCaptureEnabled(false, false);
        std::filesystem::remove_all(dir, ec);
    }
#else
    // Documented POSIX behaviour: no recoverable equivalent exists, so the
    // guard is a counted passthrough. Asserted rather than skipped, so this
    // file never silently tests nothing on a platform.
    {
        const std::uint64_t calls = vendorGuardCallCount();
        int out = 0;
        g_ran = 0;
        CHECK(callGuardingVendorFaults(&runsCleanly, &out));
        CHECK(g_ran == 1);
        CHECK(out == 42);
        CHECK(vendorGuardCallCount() == calls + 1);
        // Nothing is absorbed off Windows, and the predicate says so rather
        // than pretending a filter exists.
        CHECK(!vendorGuardAbsorbs(0xC0000005u, reinterpret_cast<const void*>(&runsCleanly)));
    }
#endif

    return testSummary("test_vendor_guard");
}
