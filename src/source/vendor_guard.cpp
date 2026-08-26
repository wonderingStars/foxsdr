// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/vendor_guard.hpp"

#include "core/crash_handler.hpp"

#include <atomic>

#ifdef _WIN32
#include <windows.h>
#endif

namespace cascade::source {

namespace {

std::atomic<std::uint64_t> g_calls{0};
std::atomic<std::uint64_t> g_faults{0};
std::atomic<std::uint32_t> g_lastCode{0};

#ifdef _WIN32
// Only the codes a hardware probe realistically raises. Anything not listed
// keeps searching, so a fault this guard was never meant to hide still reaches
// the crash handler and produces a report.
bool absorbableCode(DWORD code) noexcept {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:      // the one observed, in libusb
        case EXCEPTION_IN_PAGE_ERROR:         // a module's page could not be read
        case EXCEPTION_DATATYPE_MISALIGNMENT:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_INT_OVERFLOW:
            return true;
        default:
            // Notably EXCEPTION_STACK_OVERFLOW and C++ exceptions
            // (0xE06D7363) - see the header for why neither is absorbed.
            return false;
    }
}

// WHICH IMAGE OWNS AN ADDRESS, answered with VirtualQuery and NOT with
// GetModuleHandleEx, which is the whole reason this looks the way it does.
//
// The obvious implementation asks the loader (GetModuleHandleEx with
// FROM_ADDRESS). The loader answers that under its module-table lock, which
// LoadLibrary holds - and SoapySDR::Device::enumerate() is a LoadLibrary of
// every vendor module on the machine. A filter that waits on the loader lock
// while a vendor module is halfway through being loaded does not report a
// fault, it hangs the thread inside one, permanently. This application has
// already been bitten by exactly that lock once: see the phase-1/phase-2 split
// in core/hang_watchdog.cpp, where an unwind under the same lock could wedge
// the whole process.
//
// VirtualQuery is a syscall against the VAD tree and takes no user-mode lock
// at all. MEM_IMAGE says the address is inside a mapped executable image -
// that is exactly "some module owns this" - and AllocationBase is that image's
// base, which is the same value the loader would have called its HMODULE. Both
// facts arrive without asking the loader anything.
//
// The base of THIS image is resolved the same way, from the address of one of
// this file's own functions, so it needs no module NAME and stays correct
// whatever the executable is called.
std::atomic<void*> g_selfBase{nullptr};
std::atomic<bool> g_selfResolved{false};

// The image base containing `addr`, or nullptr when no image does. Lock-free.
void* imageBaseOf(const void* addr) noexcept {
    if (addr == nullptr) { return nullptr; }
    MEMORY_BASIC_INFORMATION mbi{};
    if (::VirtualQuery(addr, &mbi, sizeof(mbi)) == 0) { return nullptr; }
    if (mbi.State != MEM_COMMIT) { return nullptr; }
    if (mbi.Type != MEM_IMAGE) { return nullptr; }
    return mbi.AllocationBase;
}

void resolveSelfModule() noexcept {
    if (g_selfResolved.load(std::memory_order_acquire)) { return; }
    g_selfBase.store(imageBaseOf(reinterpret_cast<const void*>(&absorbableCode)),
                     std::memory_order_relaxed);
    g_selfResolved.store(true, std::memory_order_release);
}

// The filter proper. Kept OUT of the guard function itself so that the guard's
// own frame holds nothing but pointers: MSVC rejects __try in a frame that
// needs C++ object unwinding (C2712), and a filter written inline is where
// that creeps in.
//
// The counters and the report are done HERE rather than in the __except body
// because EXCEPTION_POINTERS are only valid while the faulting frame is still
// live; by the time the handler body runs, the stack they point into has been
// unwound. Writing the report from the filter is what core/crash_handler.cpp's
// own SEH filter does, for the same reason.
int vendorFaultFilter(EXCEPTION_POINTERS* ep) noexcept {
    if (ep == nullptr || ep->ExceptionRecord == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    const void* addr = ep->ExceptionRecord->ExceptionAddress;
    if (!vendorGuardAbsorbs(static_cast<std::uint32_t>(code), addr)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // Relaxed and nothing else: a fault path does the minimum, and the CALLER
    // writes its diagnostics line once it is back on a frame that is allowed
    // to allocate.
    g_lastCode.store(static_cast<std::uint32_t>(code), std::memory_order_relaxed);
    g_faults.fetch_add(1, std::memory_order_relaxed);
    // ...except for this, which cannot wait: see rule 2 in the header. The
    // writer allocates nothing and takes none of this application's locks.
    // The exception code is carried in the report's own `code` field, so the
    // reason names the CLASS of fault rather than guessing at which of the
    // eight absorbable codes this was.
    core::reportAbsorbedFault(
        "fault in a third-party SDR module, absorbed by the device enumeration "
        "guard (the process continued; the device list was empty)",
        static_cast<unsigned long>(code), addr, ep);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

}  // namespace

bool vendorGuardAbsorbs(std::uint32_t code, const void* faultAddress) noexcept {
#ifdef _WIN32
    if (!absorbableCode(static_cast<DWORD>(code))) { return false; }
    if (faultAddress == nullptr) { return false; }
    resolveSelfModule();

    void* faulting = imageBaseOf(faultAddress);
    // An instruction address that belongs to no loaded image at all: a
    // corrupted control transfer, a jump into freed or data memory. Never
    // absorbed - that is the fault most in need of a report, not least.
    if (faulting == nullptr) { return false; }
    // Our own image. A bug in cascade, and it must die with a report exactly
    // as it did before this guard existed.
    void* self = g_selfBase.load(std::memory_order_relaxed);
    // A self base that could not be resolved would make every fault look
    // foreign, quietly restoring the defect this rule exists to fix. Refuse
    // instead: a guard that cannot tell our code from theirs absorbs nothing.
    if (self == nullptr) { return false; }
    if (faulting == self) { return false; }
    return true;
#else
    (void)code;
    (void)faultAddress;
    return false;
#endif
}

bool callGuardingVendorFaults(void (*fn)(void*) noexcept, void* ctx) noexcept {
    if (fn == nullptr) { return false; }
    g_calls.fetch_add(1, std::memory_order_relaxed);
#ifdef _WIN32
    // On the HEALTHY path, so the filter does not have to: one fewer thing for
    // a fault path to do, and it means the answer is already there even if the
    // fault arrives while this image's own pages are under pressure.
    resolveSelfModule();
    __try {
        fn(ctx);
        return true;
    } __except (vendorFaultFilter(GetExceptionInformation())) {
        // Everything worth recording was recorded in the filter, where the
        // exception information was still valid. Nothing to do here but say so.
        return false;
    }
#else
    // POSIX has no equivalent that can be recovered from safely - a SIGSEGV
    // handler cannot return to the faulting instruction - so the call runs
    // unguarded and the counter still reflects that it went through here.
    fn(ctx);
    return true;
#endif
}

std::uint64_t vendorGuardCallCount() noexcept {
    return g_calls.load(std::memory_order_relaxed);
}

std::uint64_t vendorGuardFaultCount() noexcept {
    return g_faults.load(std::memory_order_relaxed);
}

std::uint32_t vendorGuardLastFaultCode() noexcept {
    return g_lastCode.load(std::memory_order_relaxed);
}

}  // namespace cascade::source
