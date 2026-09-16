// The Linux half of crash_handler.hpp's contract. See that header for WHAT is
// covered and WHY (four Windows entry points; the equivalent four here are
// sigaction on an alternate stack for the fault signals, std::set_terminate,
// a SIGABRT net, and an override of __cxa_pure_virtual - see below), and see
// crash_handler.cpp's file header for the allocation-free discipline every
// report writer here follows for the same reason: a fault handler runs on a
// process that has already failed once, and must not need anything that
// failure could have taken with it.
//
// WHAT THIS FILE TOUCHES, so the claim above can be checked rather than
// believed: sigaction, sigaltstack, open/write/close (kernel calls, not
// buffered stdio), clock_gettime, getpid, the SYS_gettid syscall, a LOCAL
// unwind of the ucontext_t the kernel hands every SA_SIGINFO handler, and
// memcpy/hand-written integer rendering exactly as crash_handler.cpp uses on
// Windows. It allocates nothing, takes no lock the rest of the process could
// be holding, and calls no snprintf (glibc's can take a lock for
// wide-character/locale state, which is still a lock).
//
// TWO UNWINDERS BEHIND ONE INTERFACE, chosen by CASCADE_ANDROID exactly as
// every other platform split in this file is - never __ANDROID__, so the
// host-native validation configure (-DCASCADE_ANDROID=ON, no NDK) takes the
// identical branch a real device build does:
//
//   DESKTOP LINUX (glibc, libunwind installed)   unw_init_local/unw_step over
//     the ucontext_t directly, as before.
//   ANDROID (the NDK ships no libunwind LOCAL-unwind package - see
//     CMakeLists.txt's guard around pkg_check_modules(LIBUNWIND))
//     _Unwind_Backtrace from <unwind.h>, part of the compiler runtime clang
//     links into every Android binary automatically (no separate library,
//     confirmed by linking a throwaway translation unit against it and
//     reading its NEEDED entries - libclang_rt.builtins supplies it
//     statically, nothing dynamic to find at runtime). Called DIRECTLY from
//     inside the signal handler rather than fed the ucontext_t: on both
//     shipped ABIs (arm64-v8a, x86_64 - see android/app/build.gradle's
//     abiFilters) bionic's kernel-installed sigreturn trampoline carries the
//     CFI "this is a signal frame" annotation the GNU unwind ABI defines, so
//     _Unwind_Backtrace transparently continues PAST the trampoline into the
//     interrupted frame and its full caller chain - verified empirically
//     against a real SIGSEGV on the x86_64 emulator (frame[2] landed exactly
//     on the faulting instruction three levels below main()) before this was
//     relied on rather than assumed. The faulting INSTRUCTION for the
//     `address:` field is still read directly out of the ucontext_t, exactly
//     as the desktop branch does with UNW_REG_IP - just through
//     ucontext_t.uc_mcontext's platform-specific register field
//     (gregs[REG_RIP] on x86_64, .pc on aarch64) instead of libunwind's
//     UNW_REG_IP, because _Unwind_Backtrace's own first frame is inside this
//     handler, not the fault.
//
// THE ONE HONEST CAVEAT, and it has the same shape as crash_handler.cpp's and
// applies to BOTH unwinders above: neither libunwind's local unwinder nor
// _Unwind_Backtrace is on the POSIX async-signal-safe list - both can read
// /proc/self/maps or process loaded unwind tables on their first call, and
// neither operation is guaranteed safe to perform inside a signal handler.
// This is not avoidable without shipping a private DWARF/.eh_frame reader,
// which is out of scope; the mitigation is the same one crash_handler.cpp
// applies to the equivalent Windows risk (RtlLookupFunctionEntry under the
// loader lock): the report is written INCREMENTALLY, most valuable first, so
// a wedge inside the unwinder still leaves the fault kind, the address and
// the application context on disk. install() additionally performs one
// throwaway local unwind on the healthy path so either unwinder's one-time
// setup is paid for before any real fault can be blocked behind it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/crash_handler_posix.hpp"

#include "core/crash_stack_start.hpp"
#include "core/diag_log.hpp"
#include "core/diag_report.hpp"

#if defined(CASCADE_ANDROID)
#include <ucontext.h>
#include <unwind.h>
#else
#define UNW_LOCAL_ONLY
#include <libunwind.h>
#endif

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <system_error>
#include <thread>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace cascade::core::posix_detail {

namespace {

constexpr std::size_t kPathBytes = 512;
constexpr int kMaxFrames = 62;

// The software-detected conditions that have no signal of their own get an
// 0xE00000xx sentinel in the same spirit as crash_handler.cpp's Windows codes
// - not the same numbers (there is no NTSTATUS here to match), just the same
// idea that a `code:` for one of these is never a real hardware fault code
// and is spelled distinctly from one. A direct abort() and every hardware
// fault instead use the raw signal number as `code`, because both are always
// delivered as a real signal on this platform and faultSignalHandler's
// dispatch already tells them apart by `reason`.
constexpr unsigned long kCodeTerminate = 0xE1000001ul;
constexpr unsigned long kCodePureCall = 0xE1000002ul;
constexpr unsigned long kCodeInvalidParameter = 0xE1000003ul;

// unw_context_t IS ucontext_t on Linux/x86_64 (see the comment further down
// where this was walked directly), so the desktop branch never needed a name
// of its own for it. Android has no unw_context_t at all, so this file needs
// ONE name for "the type the kernel hands a SA_SIGINFO handler" that both
// branches can share in call sites and signatures - PlatformSigContext is
// that name, and it is the ONLY new type this Android branch introduces.
// Declared here, ahead of everything that names it, rather than beside
// ChildFault further down where it used to live.
#if defined(CASCADE_ANDROID)
using PlatformSigContext = ucontext_t;
#else
using PlatformSigContext = unw_context_t;
#endif

// ALL FIXED STORAGE, prepared while the process is still healthy. See
// crash_handler.cpp's file header for why: a stack overflow is exactly the
// fault this most needs to survive, and although sigaltstack (below) gives the
// handler a fresh stack to run on regardless, keeping the report machinery out
// of any stack keeps the two implementations' discipline identical rather
// than diverging on a technicality.
char g_crashDir[kPathBytes] = {};
char g_lastPath[kPathBytes] = {};
bool g_enabled = false;
bool g_exitAfterReport = false;
std::atomic<int> g_inHandler{0};
std::atomic<int> g_inAbsorbed{0};
std::atomic<int> g_inAbsorbedChild{0};
std::atomic<long> g_reportSeq{0};

char g_ringBuf[DiagLog::kRingLines * DiagLog::kLineBytes + 1] = {};
char g_reportPath[kPathBytes] = {};
unsigned long g_frames[kMaxFrames] = {};

// The main executable's own base address, read once at install() from module
// index 0 - dl_iterate_phdr (which refreshModuleTable() calls) always visits
// the main program first, before any shared object. Used for the process
// block's fault-thread-own line exactly as g_selfBase is on Windows.
std::uintptr_t g_selfBase = 0;

std::terminate_handler g_prevTerminate = nullptr;

// A dedicated 256 KiB stack for every handler registered below, so a fault
// whose cause was a stack overflow still runs the handler on real stack space
// rather than the exhausted one. Static, not heap, for the same reason as the
// buffers above: sigaltstack itself runs on the healthy path (install()), but
// the memory it hands the kernel must outlive that call for the life of the
// process. Sized well above MINSIGSTKSZ rather than using the SIGSTKSZ macro,
// which on current glibc/kernel headers can be a runtime value (AT_MINSIGSTKSZ)
// and is therefore not usable to size a static array.
constexpr std::size_t kAltStackBytes = 256 * 1024;
alignas(16) char g_altStack[kAltStackBytes] = {};

pid_t posixGetTid() { return static_cast<pid_t>(::syscall(SYS_gettid)); }

bool enterHandler() {
    int expected = 0;
    return g_inHandler.compare_exchange_strong(expected, 1, std::memory_order_acq_rel);
}
void exitHandler() { g_inHandler.store(0, std::memory_order_release); }

// ---------------------------------------------------------------------------
// The allocation-free writer
// ---------------------------------------------------------------------------
struct Emit {
    int fd = -1;

    void raw(const char* s, std::size_t n) const {
        if (fd < 0 || n == 0) { return; }
        // A short write is possible even for a small buffer (a signal landing
        // mid-write); looping costs nothing and a partial report is still
        // better than a truncated line nobody retried.
        std::size_t at = 0;
        while (at < n) {
            const ssize_t w = ::write(fd, s + at, n - at);
            if (w <= 0) { return; }
            at += static_cast<std::size_t>(w);
        }
    }
    void str(const char* s) const {
        if (s == nullptr) { return; }
        std::size_t n = 0;
        while (s[n] != '\0') { ++n; }
        raw(s, n);
    }
    void hex(unsigned long long v, int digits) const {
        char buf[24];
        static const char kHex[] = "0123456789ABCDEF";
        if (digits <= 0) {
            digits = 1;
            for (unsigned long long t = v >> 4; t != 0; t >>= 4) { ++digits; }
        }
        if (digits > 16) { digits = 16; }
        for (int i = 0; i < digits; ++i) {
            buf[i] = kHex[(v >> ((digits - 1 - i) * 4)) & 0xFull];
        }
        raw(buf, static_cast<std::size_t>(digits));
    }
    void dec(unsigned long long v) const {
        char buf[24];
        int n = 0;
        if (v == 0) {
            buf[n++] = '0';
        } else {
            char tmp[24];
            int t = 0;
            while (v != 0 && t < 24) {
                tmp[t++] = static_cast<char>('0' + (v % 10));
                v /= 10;
            }
            while (t > 0) { buf[n++] = tmp[--t]; }
        }
        raw(buf, static_cast<std::size_t>(n));
    }
    // "<module>+0x<offset>", or the bare address when it resolves to no module
    // the snapshot knows about - same contract as crash_handler.cpp's Emit::addr.
    void addr(std::uintptr_t a) const {
        DiagModule m;
        std::uintptr_t off = 0;
        if (resolveAddress(a, m, off)) {
            str(m.name);
            str("+0x");
            hex(static_cast<unsigned long long>(off), 0);
        } else {
            str("0x");
            hex(static_cast<unsigned long long>(a), 16);
        }
    }
};

void appendDec(char* dst, std::size_t& at, std::size_t cap, unsigned long long v) {
    char tmp[24];
    int t = 0;
    if (v == 0) {
        tmp[t++] = '0';
    } else {
        while (v != 0 && t < 24) {
            tmp[t++] = static_cast<char>('0' + (v % 10));
            v /= 10;
        }
    }
    while (t > 0 && at + 1 < cap) { dst[at++] = tmp[--t]; }
}

// "<dir>/<prefix><epoch-seconds>-<pid>-<seq>.txt". No calendar breakdown: that
// needs localtime/gmtime, and neither is on the async-signal-safe list (they
// use non-reentrant global state and, for a local time, the timezone
// database). clock_gettime(CLOCK_REALTIME, ...) is; the file name is less
// readable than the Windows one's YYYYMMDD-HHMMSS but the report's own
// content parses identically, and nothing downstream (crash_upload.cpp's
// isReportName, this test suite) reads anything out of the name beyond the
// "crash-"/"hang-" prefix and the ".txt" suffix.
void buildReportPath(char* out, std::size_t cap, const char* prefix, long seq) {
    std::size_t at = 0;
    for (std::size_t i = 0; g_crashDir[i] != '\0' && at + 1 < cap; ++i) { out[at++] = g_crashDir[i]; }
    if (at + 1 < cap) { out[at++] = '/'; }
    for (std::size_t i = 0; prefix[i] != '\0' && at + 1 < cap; ++i) { out[at++] = prefix[i]; }

    struct timespec ts {};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    appendDec(out, at, cap, static_cast<unsigned long long>(ts.tv_sec));
    if (at + 1 < cap) { out[at++] = '-'; }
    appendDec(out, at, cap, static_cast<unsigned long long>(::getpid()));
    if (at + 1 < cap) { out[at++] = '-'; }
    appendDec(out, at, cap, static_cast<unsigned long long>(seq));
    const char* ext = ".txt";
    for (int i = 0; ext[i] != '\0' && at + 1 < cap; ++i) { out[at++] = ext[i]; }
    out[at] = '\0';
}

void writeContextBlock(const Emit& e) {
    e.str("--- context ---\n");
    int len = 0;
    const char* block = diagContextRaw(len);
    e.raw(block, static_cast<std::size_t>(len));
}

void writeModules(const Emit& e) {
    e.str("--- modules ---\n");
    const int n = moduleCount();
    for (int i = 0; i < n; ++i) {
        DiagModule m;
        if (!moduleAt(i, m)) { continue; }
        e.str("  ");
        e.str(m.name);
        e.str(" base=0x");
        e.hex(static_cast<unsigned long long>(m.base), 16);
        e.str(" size=0x");
        e.hex(static_cast<unsigned long long>(m.size), 0);
        e.str(" pdb=");
        e.str(m.pdb[0] != '\0' ? m.pdb : "(none)");
        e.str(" build=");
        e.str(m.buildId[0] != '\0' ? m.buildId : "(none)");
        e.str("\n");
    }
}

void writeRing(const Emit& e) {
    const std::uint64_t total = DiagLog::instance().linesWritten();
    const std::uint64_t kept =
        (total < static_cast<std::uint64_t>(DiagLog::kRingLines))
            ? total
            : static_cast<std::uint64_t>(DiagLog::kRingLines);
    e.str("--- log (last ");
    e.dec(kept);
    e.str(" of ");
    e.dec(total);
    e.str(" lines) ---\n");
    const std::size_t used = DiagLog::instance().copyRingRaw(g_ringBuf, sizeof(g_ringBuf));
    e.raw(g_ringBuf, used);
}

#if defined(CASCADE_ANDROID)

// The faulting INSTRUCTION, read straight out of the register file the
// kernel wrote into the ucontext_t - the same fact UNW_REG_IP answers on the
// desktop branch, just reached through bionic's per-ABI mcontext_t instead of
// libunwind. Only the two shipped ABIs are handled (android/app/build.gradle
// abiFilters: arm64-v8a, x86_64) - a third would need its own register field
// name verified against bionic's sys/ucontext.h before being trusted here,
// so this intentionally does not guess at one.
std::uintptr_t androidContextPc(const PlatformSigContext* ctx) {
    if (ctx == nullptr) { return 0; }
#if defined(__x86_64__)
    return static_cast<std::uintptr_t>(ctx->uc_mcontext.gregs[REG_RIP]);
#elif defined(__aarch64__)
    return static_cast<std::uintptr_t>(ctx->uc_mcontext.pc);
#else
#error "crash_handler_posix.cpp: unhandled Android ABI (only arm64-v8a and x86_64 ship - see android/app/build.gradle)"
#endif
}

struct AndroidUnwindState {
    unsigned long* frames;
    int n;
    int max;
};

_Unwind_Reason_Code androidUnwindCallback(struct _Unwind_Context* uctx, void* argVoid) {
    auto* st = static_cast<AndroidUnwindState*>(argVoid);
    if (st->n >= st->max) { return _URC_END_OF_STACK; }
    st->frames[st->n++] = static_cast<unsigned long>(::_Unwind_GetIP(uctx));
    return _URC_NO_REASON;
}

// `ctx` is accepted (and ignored for the walk itself) purely so this
// function has the same signature as the desktop branch's and every call
// site below needs no #ifdef of its own. _Unwind_Backtrace always starts
// unwinding from wherever IT is called - never from an arbitrary supplied
// context - so the walk's starting point is "here", inside the signal
// handler on the alternate stack. What makes that useful rather than
// useless is the file header's CFI claim: bionic's sigreturn trampoline is
// itself a valid unwind frame, so the walk continues straight through it
// into the code that faulted and every one of its callers. The instruction
// that actually faulted is read separately, from `ctx`, by the caller (see
// faultSignalHandler) - not recovered from this walk - because the first
// entries this produces are this handler's own frames and the trampoline,
// not the fault.
int captureFramesFromContext(PlatformSigContext* ctx, unsigned long* frames, int maxFrames) {
    (void)ctx;
    AndroidUnwindState st{frames, 0, maxFrames};
    ::_Unwind_Backtrace(&androidUnwindCallback, &st);
    return st.n;
}

int captureFramesCurrentThread(unsigned long* frames, int maxFrames) {
    return captureFramesFromContext(nullptr, frames, maxFrames);
}

#else  // !CASCADE_ANDROID - desktop Linux, libunwind

// unw_context_t IS ucontext_t on Linux/x86_64 (libunwind typedefs it so), so
// the ucontext_t* a signal handler is handed by the kernel can be walked
// directly with no copy - unlike crash_handler.cpp's Windows CONTEXT, which
// RtlVirtualUnwind mutates in place and which is copied into static storage
// first so the original stays available if a second fault needs it. Nothing
// here mutates the caller's ucontext_t, so no copy is needed.
int captureFramesFromContext(unw_context_t* ctx, unsigned long* frames, int maxFrames) {
    unw_cursor_t cursor;
    if (::unw_init_local(&cursor, ctx) != 0) { return 0; }
    int n = 0;
    do {
        unw_word_t ip = 0;
        if (::unw_get_reg(&cursor, UNW_REG_IP, &ip) != 0) { break; }
        frames[n++] = static_cast<unsigned long>(ip);
    } while (n < maxFrames && ::unw_step(&cursor) > 0);
    return n;
}

int captureFramesCurrentThread(unsigned long* frames, int maxFrames) {
    unw_context_t ctx;
    ::unw_getcontext(&ctx);
    return captureFramesFromContext(&ctx, frames, maxFrames);
}

#endif  // CASCADE_ANDROID

struct ChildFault {
    unsigned long exitCode = 0;
    int attempt = 0;
};

// The whole report, written incrementally exactly as crash_handler.cpp's
// writeReport does and for the same reason: a wedge in the unwinder must still
// leave the identifying half on disk. `ctx` is the ucontext_t a real signal
// carried, or nullptr for the software-detected conditions (terminate,
// purecall, the invalid-parameter stand-in, a direct abort) which run as
// ordinary function calls with no kernel-supplied context of their own - for
// those, `mayWalkCurrentThread` says whether the CALLING thread's own stack is
// the answer, matching captureFramesGuarded's contract on Windows.
void writeReport(const char* reason, unsigned long code, std::uintptr_t faultAddr,
                 PlatformSigContext* ctx, bool mayWalkCurrentThread, const ChildFault* child) {
    if (!g_enabled || g_crashDir[0] == '\0') { return; }

    const long seq = g_reportSeq.fetch_add(1, std::memory_order_relaxed) + 1;
    buildReportPath(g_reportPath, kPathBytes, "crash-", seq);

    Emit e;
    e.fd = ::open(g_reportPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (e.fd < 0) { return; }

    DiagModule fm;
    std::uintptr_t foff = 0;
    const bool resolved = resolveAddress(faultAddr, fm, foff);
    char sig[17];
    crashSignatureRaw(code, resolved ? fm.name : "?", foff, sig);

    // THE HEADER. Same inventory crash_handler.cpp's Windows writer emits
    // (crashReportFieldNames(), asserted in both directions by
    // tests/test_crash_capture.cpp against a report from a REAL fault) - one
    // set of fields, one parser (crash_upload.cpp's parseReportText) reading
    // reports from either platform.
    e.str("kind: crash\n");
    e.str("reason: ");
    e.str(reason);
    e.str("\ncode: 0x");
    e.hex(code, 8);
    e.str("\naddress: ");
    e.addr(faultAddr);
    e.str("\nsignature: ");
    e.str(sig);
    e.str("\nthread: ");
    e.dec(static_cast<unsigned long long>(posixGetTid()));
    e.str("\n");
    writeContextBlock(e);

    e.str("--- stack (thread ");
    e.dec(static_cast<unsigned long long>(posixGetTid()));
    e.str(") ---\n");
    int nFrames = 0;
    if (child == nullptr) {
        if (ctx != nullptr) {
            nFrames = captureFramesFromContext(ctx, g_frames, kMaxFrames);
        } else if (mayWalkCurrentThread) {
            nFrames = captureFramesCurrentThread(g_frames, kMaxFrames);
        }
    }
    if (nFrames == 0) {
        if (child != nullptr) {
            e.str("  (no frames: the fault was in a child process, so this "
                  "process has no stack to show)\n");
        } else {
            e.str("  (no frames: the stack could not be walked)\n");
        }
    }
    // THIS HANDLER'S OWN FRAMES ARE NOT PART OF THE FAULT. On Android the walk
    // starts inside this file (see captureFramesFromContext's comment, and
    // core/crash_stack_start.hpp for why that matters and what it cost: every
    // Android report grouped under captureFramesFromContext on the dashboard),
    // so the written stack starts at the faulting instruction the `address:`
    // field above already names. On desktop this is frame 0 and nothing moves.
    const int firstFrame = crashStackStartIndex(g_frames, nFrames, faultAddr);
    for (int i = firstFrame; i < nFrames; ++i) {
        e.str("  ");
        e.addr(static_cast<std::uintptr_t>(g_frames[i]));
        e.str("\n");
    }

    e.str("--- process ---\nuptime-sec: ");
    e.dec(processUptimeSec());
    e.str("\nfault-thread-own: ");
    if (nFrames == 0) {
        e.str("unknown");
    } else {
        bool own = false;
        // FROM THE SAME FRAME THE STACK IS WRITTEN FROM, or this field answers
        // a different question than it claims to. Counting the trimmed frames
        // meant "did any frame come from our module" included the handler
        // itself - which is always ours - so every Android report said yes
        // whatever had faulted.
        for (int i = firstFrame; i < nFrames && !own; ++i) {
            DiagModule m;
            std::uintptr_t off = 0;
            if (resolveAddress(static_cast<std::uintptr_t>(g_frames[i]), m, off) &&
                m.base == g_selfBase) {
                own = true;
            }
        }
        e.str(own ? "yes" : "no");
    }
    e.str("\n");
    if (child != nullptr) {
        e.str("child-exit-code: 0x");
        e.hex(child->exitCode, 8);
        e.str("\nchild-attempt: ");
        e.dec(static_cast<unsigned long long>(child->attempt));
        e.str("\n");
    }

    writeModules(e);
    writeRing(e);
    ::close(e.fd);

    std::memcpy(g_lastPath, g_reportPath, kPathBytes);
    // No minidump on this platform: nothing here writes one, ever, whatever
    // CrashHandlerConfig::minidump says. There is no Linux ELF-core
    // equivalent implemented by this feature (a full core dump is the OS's
    // own facility, gated by ulimit -c and independent of this file) - see
    // install()'s handling of cfg.minidump.
}

void finish(unsigned long exitCode) {
    if (!g_exitAfterReport) { return; }
    ::_exit(static_cast<int>(exitCode == 0 ? 1u : (exitCode & 0x7Fu ? exitCode & 0x7Fu : 1u)));
}

const char* reasonForSignal(int sig) {
    switch (sig) {
        case SIGSEGV: return "access violation (SIGSEGV)";
        case SIGBUS: return "bus error - invalid memory access (SIGBUS)";
        case SIGILL: return "illegal instruction (SIGILL)";
        case SIGFPE: return "floating point exception (SIGFPE)";
        case SIGTRAP: return "trap (SIGTRAP)";
        case SIGABRT:
            return "abort (a direct abort(), a failed assert, or an uncaught exception "
                   "whose std::terminate had no registered handler)";
        default: return "structured fault";
    }
}

// THE ONE FAULT-SIGNAL HANDLER for SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGTRAP/
// SIGABRT. Every one of them, delivered with SA_SIGINFO, carries a real
// ucontext_t describing the point of delivery - for the four hardware faults
// that IS the faulting instruction; for SIGABRT it is wherever abort()'s own
// raise() left the thread, which is one frame away from the actual caller and
// is why the unwind below (not just this one address) is what a reader uses.
// `address` is read from that context's instruction pointer, not
// siginfo_t::si_addr - si_addr is the DATA address a memory fault touched,
// which is the wrong half of "where did this happen" for symbolisation
// (crash_handler.cpp's Windows report uses ExceptionAddress, the faulting
// INSTRUCTION, for the same reason).
void faultSignalHandler(int sig, siginfo_t* /*info*/, void* ucontextVoid) {
    if (!enterHandler()) {
        // A fault INSIDE the handler. Die quietly rather than recursing.
        ::_exit(0xE2);
    }
    auto* ctx = static_cast<PlatformSigContext*>(ucontextVoid);
    std::uintptr_t addr = 0;
#if defined(CASCADE_ANDROID)
    addr = androidContextPc(ctx);
#else
    {
        unw_cursor_t cursor;
        if (::unw_init_local(&cursor, ctx) == 0) {
            unw_word_t ip = 0;
            if (::unw_get_reg(&cursor, UNW_REG_IP, &ip) == 0) {
                addr = static_cast<std::uintptr_t>(ip);
            }
        }
    }
#endif
    writeReport(reasonForSignal(sig), static_cast<unsigned long>(sig), addr, ctx, true, nullptr);
    exitHandler();
    finish(static_cast<unsigned long>(sig));

    // Production default: reset to the OS's own handling and re-raise, so a
    // user's machine still gets whatever it always got (a core dump if
    // ulimit -c allows one, the correct WIFSIGNALED exit status) - the same
    // "let the original exception continue" policy sehFilter follows on
    // Windows. While this handler runs, `sig` stays blocked (no SA_NODEFER
    // was requested), so the re-raised signal is held pending until this
    // function returns and is then delivered against the disposition just
    // installed below, not this one.
    struct sigaction dfl {};
    dfl.sa_handler = SIG_DFL;
    ::sigemptyset(&dfl.sa_mask);
    ::sigaction(sig, &dfl, nullptr);
    ::raise(sig);
}

void onTerminatePosix() {
    if (!enterHandler()) { ::_exit(0xE2); }
    void* here = __builtin_return_address(0);
    writeReport("std::terminate", kCodeTerminate, reinterpret_cast<std::uintptr_t>(here), nullptr,
                true, nullptr);
    exitHandler();
    finish(kCodeTerminate);
    // The SIGABRT net (faultSignalHandler) must not write a second report for
    // the abort() below - stand it down first, exactly as
    // crash_handler.cpp's onTerminate does before its own abort().
    struct sigaction dfl {};
    dfl.sa_handler = SIG_DFL;
    ::sigemptyset(&dfl.sa_mask);
    ::sigaction(SIGABRT, &dfl, nullptr);
    if (g_prevTerminate != nullptr && g_prevTerminate != &onTerminatePosix) { g_prevTerminate(); }
    ::abort();
}

}  // namespace

// ---------------------------------------------------------------------------
// __cxa_pure_virtual - the ABI hook for a pure virtual call
// ---------------------------------------------------------------------------
//
// The Itanium C++ ABI (which libstdc++ and libc++ both follow on Linux)
// declares this with C linkage and calls it whenever a virtual call reaches a
// slot that was never overridden - the case crash_handler.cpp's raiseTestFault
// stages by calling a pure virtual method from inside a base-class
// constructor. libstdc++ provides a default definition that prints "pure
// virtual method called" to stderr and calls abort(); a definition in the main
// program takes its place through ordinary symbol interposition, which is
// exactly the same mechanism Windows uses to redirect a pure call through
// _set_purecall_handler - the difference is that here it is a link-time
// override rather than a runtime registration, so it applies from the first
// instruction rather than from whenever installCrashHandlers() runs. Since
// this cannot fire before main() reaches installCrashHandlers() in practice
// (nothing in this codebase makes a virtual call before that), the two are
// equivalent for every report this product will ever write.
extern "C" void __cxa_pure_virtual() {
    if (!enterHandler()) { ::_exit(0xE2); }
    void* here = __builtin_return_address(0);
    writeReport("purecall", kCodePureCall, reinterpret_cast<std::uintptr_t>(here), nullptr, true,
                nullptr);
    exitHandler();
    finish(kCodePureCall);
    struct sigaction dfl {};
    dfl.sa_handler = SIG_DFL;
    ::sigemptyset(&dfl.sa_mask);
    ::sigaction(SIGABRT, &dfl, nullptr);
    ::abort();
}

// ---------------------------------------------------------------------------
// The public entry points crash_handler.cpp forwards to
// ---------------------------------------------------------------------------
void install(const CrashHandlerConfig& cfg) {
    g_enabled = cfg.enabled;
    g_exitAfterReport = cfg.exitAfterReport;
    // cfg.minidump is deliberately not read: there is no Linux equivalent
    // implemented by this feature (see writeReport's comment), so nothing
    // here can silently disagree with what the caller asked for by ignoring
    // half of a bool it never inspects.

    g_crashDir[0] = '\0';
    if (!cfg.crashDir.empty()) {
        std::size_t n = cfg.crashDir.size();
        if (n > kPathBytes - 1) { n = kPathBytes - 1; }
        std::memcpy(g_crashDir, cfg.crashDir.data(), n);
        g_crashDir[n] = '\0';
    }
    if (cfg.enabled && g_crashDir[0] != '\0') {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(cfg.crashDir), ec);
        if (ec && !std::filesystem::is_directory(std::filesystem::path(cfg.crashDir))) {
            g_enabled = false;
            g_crashDir[0] = '\0';
        }
    } else {
        g_enabled = false;
    }

    if (moduleCount() == 0) { refreshModuleTable(); }
    {
        // NEITHER "index 0" NOR "/proc/self/exe" - both were tried, in that
        // order, and both were proven wrong against real faults rather than
        // assumed correct:
        //
        //   "index 0 is the main program" (dl_iterate_phdr's glibc ordering)
        //   is Android-wrong a first way - bionic visits the DYNAMIC LINKER
        //   first ("linker64" on a 64-bit ABI), so index 0 named the linker's
        //   base and every `fault-thread-own` line read "no" for a fault
        //   genuinely in this process's own code (measured on the x86_64
        //   emulator's on-device test binaries).
        //
        //   "/proc/self/exe names our own module" is Android-wrong a SECOND,
        //   different way, one the test binaries above never exercise: this
        //   code does not always run in a process whose EXECUTABLE is the
        //   module that matters. The real application is libfoxsdr.so,
        //   dlopen'd by app_process64 (Zygote/ART) as a NativeActivity
        //   library - /proc/self/exe there is app_process64, which is
        //   present in the module table (every Android app shares it) but
        //   contains none of our code, so this fix still shipped a real
        //   fault reading "fault-thread-own: no" against a report whose own
        //   stack frames were plainly "libfoxsdr.so+0x...". Caught by
        //   inspecting a REAL app crash pulled off the emulator, not by the
        //   on-device test binaries, which are plain executables and could
        //   not have shown this.
        //
        // dladdr() on our OWN CODE is correct under both shapes: it answers
        // "which loaded module contains this address", which is
        // test_crash_capture itself when this file is linked into an
        // executable and libfoxsdr.so when it is linked into a shared
        // library loaded by a host process - exactly the "own module" this
        // line exists to name, by construction rather than by inference from
        // process identity.
        Dl_info selfInfo{};
        const char* selfName = nullptr;
        if (::dladdr(reinterpret_cast<void*>(&install), &selfInfo) != 0 &&
            selfInfo.dli_fname != nullptr) {
            const char* slash = std::strrchr(selfInfo.dli_fname, '/');
            selfName = (slash != nullptr) ? (slash + 1) : selfInfo.dli_fname;
        }
        const int n = moduleCount();
        for (int i = 0; i < n; ++i) {
            DiagModule m;
            if (!moduleAt(i, m)) { continue; }
            if (selfName != nullptr && std::strcmp(selfName, m.name) == 0) {
                g_selfBase = m.base;
                break;
            }
        }
    }

    // Pays for libunwind's one-time setup (see the file header) on the
    // healthy path, before any signal handler is even installed.
    {
        unsigned long warm[4];
        captureFramesCurrentThread(warm, 4);
    }

    static stack_t altStack{};
    altStack.ss_sp = g_altStack;
    altStack.ss_size = sizeof(g_altStack);
    altStack.ss_flags = 0;
    ::sigaltstack(&altStack, nullptr);

    struct sigaction sa {};
    sa.sa_sigaction = &faultSignalHandler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    ::sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP}) {
        ::sigaction(sig, &sa, nullptr);
    }

    const std::terminate_handler prevT = std::set_terminate(&onTerminatePosix);
    if (prevT != &onTerminatePosix) { g_prevTerminate = prevT; }
    // __cxa_pure_virtual is installed by the linker resolving the symbol
    // above to this translation unit; nothing to register at runtime.
}

void setEnabled(bool enabled, bool minidump) {
    (void)minidump;
    g_enabled = enabled && g_crashDir[0] != '\0';
    if (g_enabled) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(g_crashDir), ec);
        if (ec && !std::filesystem::is_directory(std::filesystem::path(g_crashDir))) {
            g_enabled = false;
        }
    }
}

std::string lastReportPath() { return std::string(g_lastPath); }

std::string activeDir() {
    if (!g_enabled) { return std::string(); }
    return std::string(g_crashDir);
}

void reportAbsorbed(const char* reason, unsigned long code, const void* faultAddress) {
    int expected = 0;
    if (!g_inAbsorbed.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) { return; }
    writeReport(reason != nullptr ? reason : "absorbed fault", code,
                reinterpret_cast<std::uintptr_t>(faultAddress), nullptr, true, nullptr);
    g_inAbsorbed.store(0, std::memory_order_release);
}

void reportAbsorbedChild(const char* reason, unsigned long childExitCode, int attempt) {
    int expected = 0;
    if (!g_inAbsorbedChild.compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
        return;
    }
    ChildFault child;
    child.exitCode = childExitCode;
    child.attempt = attempt;
    writeReport(reason != nullptr ? reason : "child process fault (contained)", childExitCode, 0u,
                nullptr, false, &child);
    g_inAbsorbedChild.store(0, std::memory_order_release);
}

int captureFramesForTest(bool mayWalkCurrentThread) {
    if (!mayWalkCurrentThread) { return 0; }
    unsigned long frames[kMaxFrames];
    return captureFramesCurrentThread(frames, kMaxFrames);
}

[[noreturn]] void raiseInvalidParameterTestFault() {
    // glibc has no CRT invalid-parameter fail-fast to exercise (that concept
    // is entirely an MSVC CRT one), so the closest honest equivalent is
    // exercised instead: a runtime-detected condition that has no more
    // specific registration than "call abort()", reported under a reason that
    // says exactly that - see crash_handler_posix.hpp. The literal phrase
    // "invalid parameter" is what lets
    // tests/test_crash_capture.cpp's platform-neutral
    // `r.text.find("invalid parameter")` check pass on both platforms without
    // needing to know which one produced the report.
    if (enterHandler()) {
        void* here = __builtin_return_address(0);
        writeReport(
            "invalid parameter (posix equivalent: glibc has no CRT invalid-parameter "
            "fail-fast, so this test exercises a direct abort() instead)",
            kCodeInvalidParameter, reinterpret_cast<std::uintptr_t>(here), nullptr, true, nullptr);
        exitHandler();
        finish(kCodeInvalidParameter);
        struct sigaction dfl {};
        dfl.sa_handler = SIG_DFL;
        ::sigemptyset(&dfl.sa_mask);
        ::sigaction(SIGABRT, &dfl, nullptr);
    }
    ::abort();
}

}  // namespace cascade::core::posix_detail
