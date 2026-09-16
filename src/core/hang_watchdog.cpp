// See hang_watchdog.hpp for why this exists, why every thread is captured,
// how the 5 s threshold is derived and MEASURED, and the three separate
// causes of a false report that are suppressed.
//
// WHY THIS FILE MAY USE MACHINERY THE CRASH HANDLER MAY NOT. A hung process
// is not a corrupted one: the heap is intact, the loader is intact, nothing
// has faulted. So this allocates freely - with the exceptions spelled out at
// the suspend site below, because getting them wrong deadlocks the very
// process it is trying to describe. There are two, not one: allocation, and
// the loader lock the stack walk itself takes.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/hang_watchdog.hpp"

#include "core/diag_log.hpp"
#include "core/diag_report.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#if defined(_WIN32)
#include <windows.h>

#include <tlhelp32.h>
#elif defined(__linux__) && !defined(CASCADE_ANDROID)
#define UNW_LOCAL_ONLY
#include <libunwind.h>

#include <csignal>
#include <cstdlib>
#include <dirent.h>
#include <semaphore.h>
#include <sys/syscall.h>
#include <unistd.h>
#else
// ANDROID-TODO(crash-capture): the NDK ships no libunwind local-unwind API
// (see the "#elif defined(__linux__) && !defined(CASCADE_ANDROID)" blocks
// below, all of which this branch skips), so other-thread stack capture is
// inert on Android. <cstdlib> alone is kept: the /proc/self/status tracer
// check further down has no libunwind dependency and keeps working
// unchanged here (Android is Linux; __linux__ stays defined).
#include <cstdlib>
#endif

namespace cascade::core {

namespace {

double nowMs() {
    const auto t = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration<double, std::milli>(t).count();
}

constexpr int kMaxHangFrames = 48;

struct ThreadStack {
    unsigned long tid = 0;
    int count = 0;
    std::uintptr_t frames[kMaxHangFrames] = {};
};

#if defined(_WIN32) && defined(_M_X64)
// POD-only and wrapped in __except: MSVC forbids SEH in a function holding
// objects that need unwinding, and a suspended thread caught mid-prologue can
// present a frame pointer that is not yet valid.
int walkThreadContext(CONTEXT* ctx, std::uintptr_t* out, int maxFrames) {
    int n = 0;
    __try {
        while (n < maxFrames && ctx->Rip != 0) {
            out[n++] = static_cast<std::uintptr_t>(ctx->Rip);
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION rf = ::RtlLookupFunctionEntry(ctx->Rip, &imageBase, nullptr);
            if (rf == nullptr) {
                if (ctx->Rsp == 0) { break; }
                ctx->Rip = *reinterpret_cast<DWORD64*>(ctx->Rsp);
                ctx->Rsp += 8;
            } else {
                PVOID handlerData = nullptr;
                DWORD64 establisher = 0;
                ::RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx->Rip, rf, ctx,
                                   &handlerData, &establisher, nullptr);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}
#endif

std::string frameLine(std::uintptr_t addr) {
    DiagModule m;
    std::uintptr_t off = 0;
    char buf[160];
    if (resolveAddress(addr, m, off)) {
        std::snprintf(buf, sizeof(buf), "  %s+0x%llX\n", m.name,
                      static_cast<unsigned long long>(off));
    } else {
        std::snprintf(buf, sizeof(buf), "  0x%016llX\n",
                      static_cast<unsigned long long>(addr));
    }
    return std::string(buf);
}

#if defined(__linux__) && !defined(CASCADE_ANDROID)
// ---------------------------------------------------------------------------
// LINUX: capturing a stack that is not the caller's own.
//
// Windows suspends a thread with SuspendThread, reads its registers, resumes
// it, and only unwinds afterwards - because unwinding while a thread is
// suspended can deadlock on the loader lock a suspended LoadLibrary might
// hold (see the phase-1/phase-2 split above). There is no SuspendThread on
// Linux; the analogous primitive is to make the TARGET THREAD run a signal
// handler of our choosing, which is what a realtime signal plus tgkill does.
// The handler runs on the target thread's own stack, so the unwind happens
// WHILE that thread is "stopped" from its own point of view, in one pass, with
// no separate suspend/resume window to get wrong.
//
// THE SAME LOCK RISK APPLIES HERE, in a different shape. libunwind's local
// unwinder reads /proc/self/maps on first use and may cache DWARF unwind
// tables, and neither is on the POSIX async-signal-safe list. A thread
// signalled while it happens to hold glibc's malloc arena lock, or while it is
// the very thread that is loading a shared object under ld.so's lock, could in
// principle wedge inside the handler. This is the same trade the Windows
// implementation makes and documents at length: there is no fully
// async-signal-safe unwinder available without shipping a private DWARF/.eh_frame
// reader, which is out of scope here. Two mitigations, both cheap: (1) the
// FIRST unwind of the process happens on the healthy path, in
// ensureHangCaptureSignalInstalled() below, so libunwind's one-time setup
// (the /proc/self/maps read) is already paid for before any thread is
// ever signalled; (2) every signalled capture is bounded by a 200 ms
// sem_timedwait, so a thread that cannot answer - because it is wedged inside
// exactly the lock this paragraph worries about - costs one stack section
// that says so, not a watchdog that never reports at all.
// NOT constexpr: glibc's SIGRTMIN is a function (__libc_current_sigrtmin()),
// not a manifest constant, because the C library itself reserves the first
// few realtime signals for internal use and the count can vary. Read once.
int hangCaptureSignal() {
    static const int sig = SIGRTMIN + 5;
    return sig;
}

struct LinuxHangCapture {
    std::uintptr_t frames[kMaxHangFrames] = {};
    int count = 0;
};
LinuxHangCapture g_hangCapture;
sem_t g_hangCaptureSem;
std::atomic<bool> g_hangSignalReady{false};

// Runs ON THE SIGNALLED THREAD. unw_getcontext captures exactly this thread's
// live registers (there is no ucontext to borrow the way a fault handler
// would use the one the kernel hands a SIGSEGV handler - a realtime signal
// delivered by tgkill carries no such context), then a normal local unwind
// walks it. sem_post is on the POSIX async-signal-safe list; storing into
// g_hangCapture first is safe because the watchdog thread only ever has ONE
// capture in flight; see the mutual-exclusion note at the call site.
void hangCaptureSignalHandler(int) {
    unw_context_t ctx;
    unw_getcontext(&ctx);
    unw_cursor_t cursor;
    unw_init_local(&cursor, &ctx);
    int n = 0;
    do {
        unw_word_t ip = 0;
        if (unw_get_reg(&cursor, UNW_REG_IP, &ip) != 0) { break; }
        g_hangCapture.frames[n++] = static_cast<std::uintptr_t>(ip);
    } while (n < kMaxHangFrames && unw_step(&cursor) > 0);
    g_hangCapture.count = n;
    sem_post(&g_hangCaptureSem);
}

// Installed once, lazily, from the watchdog thread - a healthy-path call, not
// the fault path, so there is no restriction on what it may do.
void ensureHangCaptureSignalInstalled() {
    if (g_hangSignalReady.load(std::memory_order_acquire)) { return; }
    sem_init(&g_hangCaptureSem, 0, 0);
    struct sigaction sa {};
    sa.sa_handler = &hangCaptureSignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    ::sigaction(hangCaptureSignal(), &sa, nullptr);
    // Pays for libunwind's one-time setup (see the header comment above)
    // before any thread is ever signalled for a real stall.
    unw_context_t ctx;
    unw_getcontext(&ctx);
    unw_cursor_t cursor;
    unw_init_local(&cursor, &ctx);
    unw_step(&cursor);
    g_hangSignalReady.store(true, std::memory_order_release);
}

// SYS_gettid rather than glibc's gettid() wrapper (only glibc >= 2.30):
// nothing else in this file assumes a particular glibc version, and tgkill
// below is already reached the same way.
pid_t linuxGetTid() { return static_cast<pid_t>(::syscall(SYS_gettid)); }

// Every numeric entry under /proc/self/task is a live thread id of this
// process - the same enumeration ps and gdb use, and it needs no snapshot
// handle the way CreateToolhelp32Snapshot does.
std::vector<pid_t> listLinuxThreadIds() {
    std::vector<pid_t> out;
    DIR* d = ::opendir("/proc/self/task");
    if (d == nullptr) { return out; }
    while (struct dirent* e = ::readdir(d)) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') { continue; }
        out.push_back(static_cast<pid_t>(std::atoi(e->d_name)));
    }
    ::closedir(d);
    return out;
}

// One thread's stack, captured either directly (the calling thread) or by
// signalling it and waiting up to 200 ms. Never suspends anything: the target
// keeps running until the instant it takes the signal, and resumes the
// instant the handler returns.
ThreadStack captureOneLinuxThread(pid_t tid, pid_t self) {
    ThreadStack ts;
    ts.tid = static_cast<unsigned long>(tid);
    if (tid == self) {
        unw_context_t ctx;
        unw_getcontext(&ctx);
        unw_cursor_t cursor;
        unw_init_local(&cursor, &ctx);
        int n = 0;
        do {
            unw_word_t ip = 0;
            if (unw_get_reg(&cursor, UNW_REG_IP, &ip) != 0) { break; }
            ts.frames[n++] = static_cast<std::uintptr_t>(ip);
        } while (n < kMaxHangFrames && unw_step(&cursor) > 0);
        ts.count = n;
        return ts;
    }

    // Drain a stale post from an earlier, timed-out capture first: without
    // this, a thread that answered late for a PREVIOUS victim could satisfy
    // this wait immediately with yesterday's frames.
    while (::sem_trywait(&g_hangCaptureSem) == 0) {}
    g_hangCapture.count = 0;
    if (::syscall(SYS_tgkill, ::getpid(), tid, hangCaptureSignal()) != 0) { return ts; }

    struct timespec deadline{};
    ::clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += 200 * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }
    if (::sem_timedwait(&g_hangCaptureSem, &deadline) == 0) {
        ts.count = g_hangCapture.count;
        for (int i = 0; i < ts.count && i < kMaxHangFrames; ++i) { ts.frames[i] = g_hangCapture.frames[i]; }
    }
    // A timeout leaves ts.count at 0: the thread did not, or could not, answer
    // within the window - a known limit the writer states in words, not a
    // frame list invented to fill the gap.
    return ts;
}
#endif  // __linux__ && !CASCADE_ANDROID - the block above was unguarded once
        // and MSVC compiled it; the NDK equally cannot, see the ANDROID-TODO
        // near the top of this file

}  // namespace

HangWatchdog::~HangWatchdog() { stop(); }

void HangWatchdog::start(const std::string& reportDir, unsigned thresholdMs) {
    if (running_.load(std::memory_order_acquire)) { return; }
    {
        std::lock_guard<std::mutex> lk(dirMutex_);
        reportDir_ = reportDir;
    }
    thresholdMs_.store((thresholdMs > 0) ? thresholdMs : kDefaultThresholdMs,
                       std::memory_order_relaxed);
#if defined(_WIN32)
    guiThreadId_.store(::GetCurrentThreadId(), std::memory_order_relaxed);
#elif defined(__linux__) && !defined(CASCADE_ANDROID)
    guiThreadId_.store(static_cast<unsigned long>(linuxGetTid()), std::memory_order_relaxed);
#endif
    // The module snapshot the capture resolves addresses against, taken HERE -
    // start() is the healthy path, and walking the loader's module list from a
    // process that is already wedged is how a diagnostic becomes the fault.
    if (moduleCount() == 0) { refreshModuleTable(); }

    lastBeatMs_.store(nowMs(), std::memory_order_relaxed);
    worstGapMs_.store(0.0, std::memory_order_relaxed);
    paused_.store(0, std::memory_order_relaxed);
    reported_.store(false, std::memory_order_relaxed);
    reports_.store(0, std::memory_order_relaxed);
    skipGap_.store(true, std::memory_order_relaxed);
    stop_.store(false, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(pathMutex_);
        lastPath_.clear();
    }
    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&HangWatchdog::threadMain, this);
}

void HangWatchdog::beginShutdown(unsigned thresholdMs) {
    // ORDER MATTERS. The budget is raised BEFORE the clock is restarted, so a
    // poll that lands between these two lines judges the stall it measures
    // against the new threshold and not the old one. The reverse order leaves
    // a one-poll window in which the teardown is measured against the frame
    // threshold - which is the whole defect.
    thresholdMs_.store((thresholdMs > 0) ? thresholdMs : kShutdownThresholdMs,
                       std::memory_order_relaxed);
    lastBeatMs_.store(nowMs(), std::memory_order_relaxed);
    // The interval that ends here is a frame loop that has finished, not a
    // frame - so it must not become "the worst frame gap this build
    // measured", which is the number the 5 s threshold is justified against.
    skipGap_.store(true, std::memory_order_relaxed);
    diagLogf("watchdog: shutdown budget %u ms", thresholdMs_.load(std::memory_order_relaxed));
}

unsigned HangWatchdog::thresholdMs() const {
    return thresholdMs_.load(std::memory_order_relaxed);
}

void HangWatchdog::stop() {
    {
        // Set under the mutex so a watchdog thread sitting between "test the
        // flag" and "begin to wait" cannot miss the notification and sleep
        // out the rest of its poll.
        std::lock_guard<std::mutex> lk(stopMutex_);
        stop_.store(true, std::memory_order_release);
    }
    stopCv_.notify_all();
    if (thread_.joinable()) { thread_.join(); }
    running_.store(false, std::memory_order_release);
}

void HangWatchdog::setReportDir(const std::string& reportDir) {
    // Takes effect on the very next poll, running or not. See the header: the
    // Settings checkbox used to govern the crash handler and the log and leave
    // this one component armed, so "off" wrote a hang report anyway.
    std::lock_guard<std::mutex> lk(dirMutex_);
    reportDir_ = reportDir;
}

std::string HangWatchdog::reportDir() const {
    std::lock_guard<std::mutex> lk(dirMutex_);
    return reportDir_;
}

void HangWatchdog::heartbeat(bool recordGap) {
    const double now = nowMs();
    const double prev = lastBeatMs_.exchange(now, std::memory_order_relaxed);
#if defined(_WIN32)
    guiThreadId_.store(::GetCurrentThreadId(), std::memory_order_relaxed);
#elif defined(__linux__) && !defined(CASCADE_ANDROID)
    guiThreadId_.store(static_cast<unsigned long>(linuxGetTid()), std::memory_order_relaxed);
#endif
    // A gap that spans a deliberate pause is not a frame gap: it is the device
    // open, or the modal dialog, that the pause was taken out for. Folding it
    // into the worst-gap measurement would destroy the very number the
    // threshold is justified against.
    const bool skip = skipGap_.exchange(false, std::memory_order_relaxed);
    if (!recordGap || skip || prev <= 0.0) { return; }
    const double gap = now - prev;
    double worst = worstGapMs_.load(std::memory_order_relaxed);
    while (gap > worst &&
           !worstGapMs_.compare_exchange_weak(worst, gap, std::memory_order_relaxed)) {
    }
}

bool HangWatchdog::running() const { return running_.load(std::memory_order_acquire); }

void HangWatchdog::pause() {
    pauses_.fetch_add(1, std::memory_order_relaxed);
    paused_.fetch_add(1, std::memory_order_relaxed);
}

unsigned HangWatchdog::pausesTaken() const { return pauses_.load(std::memory_order_relaxed); }

void HangWatchdog::resume() {
    const int before = paused_.fetch_sub(1, std::memory_order_relaxed);
    if (before <= 1) {
        // Last pause released: the clock restarts from here, and the first
        // frame after it does not count as a gap.
        lastBeatMs_.store(nowMs(), std::memory_order_relaxed);
        skipGap_.store(true, std::memory_order_relaxed);
    }
}

unsigned HangWatchdog::reportsWritten() const { return reports_.load(std::memory_order_acquire); }

std::string HangWatchdog::lastReportPath() const {
    std::lock_guard<std::mutex> lk(pathMutex_);
    return lastPath_;
}

double HangWatchdog::worstGapMs() const { return worstGapMs_.load(std::memory_order_relaxed); }

void HangWatchdog::setSuppressionForTest(SuppressionForTest mode) {
    suppression_.store(static_cast<int>(mode), std::memory_order_relaxed);
}

void HangWatchdog::setCaptureAbortForTest(CaptureAbortForTest mode) {
    captureAbort_.store(static_cast<int>(mode), std::memory_order_relaxed);
}

void HangWatchdog::setStalledModuleForTest(const char* moduleName) {
    std::lock_guard<std::mutex> lk(stalledModuleMutex_);
    stalledModuleForTest_ = (moduleName != nullptr) ? moduleName : "";
}

namespace {

// Rule 2c's one fact: the module a stalled GUI thread is executing in when it
// is inside the window manager's own message wait. Case-insensitive because
// the loader reports the name as the file system has it, and "win32u.dll" has
// been seen capitalised both ways in module tables on the same machine.
bool isMessagePumpModule(const char* name) {
    if (name == nullptr) { return false; }
    static constexpr char kPump[] = "win32u.dll";
    std::size_t i = 0;
    for (; kPump[i] != '\0'; ++i) {
        const char a = name[i];
        if (a == '\0') { return false; }
        const char la = (a >= 'A' && a <= 'Z') ? static_cast<char>(a - 'A' + 'a') : a;
        if (la != kPump[i]) { return false; }
    }
    return name[i] == '\0';
}

// Case-insensitive helpers for the module-name rules below. The loader reports
// names as the file system has them and the same machine has produced
// "win32u.dll" and "WIN32U.DLL" in one module table, so nothing here may
// compare bytes directly.
char lowerAscii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equalsNoCase(const char* name, const char* want) {
    if (name == nullptr) { return false; }
    std::size_t i = 0;
    for (; want[i] != '\0'; ++i) {
        if (name[i] == '\0' || lowerAscii(name[i]) != want[i]) { return false; }
    }
    return name[i] == '\0';
}

bool startsWithNoCase(const char* name, const char* prefix) {
    if (name == nullptr) { return false; }
    for (std::size_t i = 0; prefix[i] != '\0'; ++i) {
        if (name[i] == '\0' || lowerAscii(name[i]) != prefix[i]) { return false; }
    }
    return true;
}

bool containsNoCase(const char* name, const char* needle) {
    if (name == nullptr) { return false; }
    for (std::size_t s = 0; name[s] != '\0'; ++s) {
        if (startsWithNoCase(name + s, needle)) { return true; }
    }
    return false;
}

// WHERE A THREAD SITS WHEN IT IS WAITING FOR SOMETHING. Every blocking wait on
// Windows bottoms out in one of these, so this alone says nothing about the
// CAUSE - it is the precondition, not the decision.
bool isKernelWaitModule(const char* name) {
    return equalsNoCase(name, "ntdll.dll") || equalsNoCase(name, "win32u.dll") ||
           equalsNoCase(name, "kernelbase.dll") || equalsNoCase(name, "kernel32.dll");
}

// THE GRAPHICS STACK, and the list is deliberately in three parts because the
// three rot at different speeds.
//
//   - The Windows-owned names are fixed and can be matched exactly.
//   - The vendor driver names are stable per vendor and matched by PREFIX, so a
//     new revision (atio6axx -> atio7axx) is still recognised.
//   - The installable client drivers all carry "icd" in the middle of a name
//     that is otherwise unpredictable (ig9icd64, igvk64, nvoglv64), so that
//     substring is the third rule.
//
// A name that is not here is simply not evidence of a display stall, and the
// report stays a hang - which is the safe direction to be wrong in.
bool isDisplayModule(const char* name) {
    static const char* const kExact[] = {
        "opengl32.dll", "glu32.dll",  "dxgi.dll",    "dxcore.dll", "gdi32.dll",
        "gdi32full.dll", "dwmapi.dll", "d3d9.dll",   "d3d11.dll",  "d3d12.dll",
        "d3d10warp.dll", "vulkan-1.dll",
    };
    for (const char* e : kExact) {
        if (equalsNoCase(name, e)) { return true; }
    }
    static const char* const kPrefixes[] = {
        // AMD (atio6axx.dll is the module the 0.96.3 report named).
        "atio", "atig", "aticfx", "amdxc", "amdihk", "amdvlk", "amdxn",
        // NVIDIA.
        "nvoglv", "nvd3dum", "nvwgf2um", "nvldumd", "nvapi",
        // Intel.
        "igd", "igvk", "igc", "ig9", "ig11", "ig12",
    };
    for (const char* p : kPrefixes) {
        if (startsWithNoCase(name, p)) { return true; }
    }
    return containsNoCase(name, "icd");
}

}  // namespace

bool HangWatchdog::isDisplayPresentationStall(const char* const* frameModules, int count) {
    if (frameModules == nullptr || count <= 0) { return false; }
    // THE TOP FRAME MUST BE A WAIT. A thread that is BUSY in a display driver -
    // spinning, or genuinely computing - is not stalled on presentation, and a
    // stall that is not a wait is this application burning a core.
    if (!isKernelWaitModule(frameModules[0])) { return false; }
    const int scan = (count < kDisplayStallScanFrames) ? count : kDisplayStallScanFrames;
    // ...AND THE GRAPHICS STACK MUST BE UNDER IT, within the handful of frames a
    // present call occupies. Frame 0 is skipped: it is the wait module by the
    // test above, and nothing else.
    for (int i = 1; i < scan; ++i) {
        if (isDisplayModule(frameModules[i])) { return true; }
    }
    return false;
}

bool HangWatchdog::guiThreadIsPumping() const {
    {
        std::lock_guard<std::mutex> lk(stalledModuleMutex_);
        if (!stalledModuleForTest_.empty()) {
            return isMessagePumpModule(stalledModuleForTest_.c_str());
        }
    }
#if defined(_WIN32) && defined(_M_X64)
    // ONE REGISTER, READ THE WAY THE CAPTURE READS IT: suspend, GetThreadContext,
    // resume, and nothing else while the thread is stopped - no allocation and
    // no loader call, for the reasons captureAllThreads spells out. The module
    // lookup afterwards walks the table start() snapshotted, which takes no
    // lock either.
    const DWORD tid = guiThreadId_.load(std::memory_order_relaxed);
    if (tid == 0) { return false; }
    HANDLE h = ::OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                            FALSE, tid);
    if (h == nullptr) { return false; }
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_CONTROL;
    bool haveCtx = false;
    if (::SuspendThread(h) != static_cast<DWORD>(-1)) {
        haveCtx = ::GetThreadContext(h, &ctx) != 0;
        ::ResumeThread(h);
    }
    ::CloseHandle(h);
    if (!haveCtx) { return false; }
    DiagModule m;
    std::uintptr_t off = 0;
    if (!resolveAddress(static_cast<std::uintptr_t>(ctx.Rip), m, off)) { return false; }
    return isMessagePumpModule(m.name);
#else
    return false;
#endif
}

bool HangWatchdog::suppressed() const {
    const int mode = suppression_.load(std::memory_order_relaxed);
    if (mode == static_cast<int>(SuppressionForTest::NeverSuppress)) {
        // The debugger and modal rules are off, as the mode promises; rule 2c
        // still answers when a test has injected a module, because that mode
        // is how the rule itself gets tested.
        std::lock_guard<std::mutex> lk(stalledModuleMutex_);
        if (!stalledModuleForTest_.empty()) {
            return isMessagePumpModule(stalledModuleForTest_.c_str());
        }
        return false;
    }
    if (mode == static_cast<int>(SuppressionForTest::AlwaysSuppress)) { return true; }
#if defined(_WIN32)
    // 1. A break is not a hang.
    if (::IsDebuggerPresent()) { return true; }
    // 2. A nested Windows modal loop - a window drag, a resize, an open system
    //    or popup menu - stops the application's own loop turning over for as
    //    long as the user holds it, legitimately, sometimes for minutes.
    GUITHREADINFO gi{};
    gi.cbSize = sizeof(gi);
    const DWORD tid = guiThreadId_.load(std::memory_order_relaxed);
    if (tid != 0 && ::GetGUIThreadInfo(tid, &gi) != 0) {
        const DWORD modal = GUI_INMOVESIZE | GUI_INMENUMODE | GUI_POPUPMENUMODE |
                            GUI_SYSTEMMENUMODE;
        if ((gi.flags & modal) != 0) { return true; }
    }
#elif defined(__linux__)
    // 1. A break is not a hang, POSIX equivalent: /proc/self/status names the
    //    tracer attached to this process, 0 when there is none. There is no
    //    Linux equivalent of rule 2 (a Windows modal message loop) - GLFW's
    //    own loop never hands control to the window manager the way DefWindowProc
    //    does, so nothing here can stall the frame loop the way a caption-button
    //    drag does on Windows.
    {
        std::ifstream status("/proc/self/status");
        std::string line;
        while (std::getline(status, line)) {
            if (line.rfind("TracerPid:", 0) == 0) {
                const std::string v = line.substr(10);
                if (std::atoi(v.c_str()) != 0) { return true; }
                break;
            }
        }
    }
#endif
    // 2c. A nested loop those flags do not cover: the thread is parked in the
    //     window manager's own wait, which the application's frame loop never
    //     does on its own (it only ever PeekMessages). See the header.
    if (guiThreadIsPumping()) { return true; }
    return false;
}

void HangWatchdog::threadMain() {
    double lastPoll = nowMs();
    while (true) {
        // THE POLL WAIT, INTERRUPTIBLE. A plain sleep_for here meant that
        // asking the watchdog to stop cost the rest of a poll - up to half a
        // second added to every shutdown, spent doing nothing. The predicate
        // is evaluated before the wait begins and again on every wake, and
        // stop() sets the flag under this same mutex, so the notification
        // cannot be missed.
        {
            std::unique_lock<std::mutex> lk(stopMutex_);
            if (stopCv_.wait_for(lk, std::chrono::milliseconds(kPollMs),
                                 [this] { return stop_.load(std::memory_order_acquire); })) {
                break;
            }
        }
        const double now = nowMs();
        const double pollInterval = now - lastPoll;
        lastPoll = now;
        const double threshold = static_cast<double>(thresholdMs_.load(std::memory_order_relaxed));

        // 3. THE WHOLE MACHINE STOPPED. Sleep, hibernate or a paused VM freezes
        //    this thread too. If the watchdog lost as much time as it is about
        //    to accuse the GUI thread of losing, it cannot tell the two apart -
        //    so it re-arms and says nothing.
        if (pollInterval > static_cast<double>(kPollMs) + threshold) {
            lastBeatMs_.store(now, std::memory_order_relaxed);
            continue;
        }

        if (paused_.load(std::memory_order_relaxed) > 0) {
            lastBeatMs_.store(now, std::memory_order_relaxed);
            continue;
        }

        const double stalled = now - lastBeatMs_.load(std::memory_order_relaxed);
        if (stalled < threshold) {
            // RECOVERY. The application came back - and the 120 s CAT shutdown
            // freeze did come back. Log it with the duration and re-arm; the
            // app is never killed.
            if (reported_.exchange(false, std::memory_order_relaxed)) {
                DiagLog::instance().writef("warn", "gui thread recovered after a stall");
            }
            continue;
        }
        if (reported_.load(std::memory_order_relaxed)) { continue; }
        if (suppressed()) { continue; }

        // ASKED TO STOP IS NOT STALLED - false-positive rule 4 in the header.
        // stop() is called at the very end of AppWindow::run(), so once the
        // flag is set the GUI thread is inside stop() joining this thread. A
        // capture begun from here would name HangWatchdog::stop() as the
        // fault, which is exactly the false report the shutdown budget above
        // exists to stop writing. Nothing reportable is lost: a shutdown that
        // really wedges never reaches stop() at all, and has already been
        // captured against the shutdown budget while it was still stuck.
        if (stop_.load(std::memory_order_acquire)) { break; }

        // Latched BEFORE the capture: a wedged application must produce one
        // report, not one per poll until the disk is full.
        reported_.store(true, std::memory_order_relaxed);
        // Read HERE, every poll, rather than captured at start(): the user can
        // untick Settings > Diagnostics at any point in the session and "off"
        // has to mean off from that moment, not from the next launch.
        const std::string dir = reportDir();
        if (dir.empty()) { continue; }

        char name[128];
#if defined(_WIN32)
        const unsigned long pid = ::GetCurrentProcessId();
#else
        const unsigned long pid = 0;
#endif
        std::snprintf(name, sizeof(name), "/hang-%lu-%u.txt", pid,
                      reports_.load(std::memory_order_relaxed) + 1u);
        const std::string path = dir + name;
        captureAllThreads(path, stalled);
    }
}

void HangWatchdog::captureAllThreads(const std::string& path, double stalledMs) {
#if defined(_WIN32)
    const DWORD self = ::GetCurrentThreadId();
    const DWORD gui = guiThreadId_.load(std::memory_order_relaxed);

    // Enumerate FIRST, with nothing suspended.
    std::vector<DWORD> tids;
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        const DWORD pid = ::GetCurrentProcessId();
        if (::Thread32First(snap, &te) != 0) {
            do {
                if (te.th32OwnerProcessID == pid) { tids.push_back(te.th32ThreadID); }
            } while (::Thread32Next(snap, &te) != 0);
        }
        ::CloseHandle(snap);
    }
    // The stalled thread first: nobody should have to guess which of five
    // stacks is the one that stopped.
    for (std::size_t i = 0; i < tids.size(); ++i) {
        if (tids[i] == gui) {
            std::swap(tids[0], tids[i]);
            break;
        }
    }

    // PHASE 1 - REGISTERS ONLY, AND NOTHING ELSE WHILE A THREAD IS SUSPENDED.
    //
    // The obvious implementation unwinds each thread while it is suspended, and
    // it can wedge the whole process. RtlLookupFunctionEntry - which every
    // frame of an x64 unwind needs - reads the loader's inverted function table
    // under an SRW lock that LoadLibrary holds EXCLUSIVELY while it inserts a
    // module. This application calls LoadLibrary from the GUI thread (a plugin
    // rescan) and from a worker (the Soapy enumerate), so a capture that lands
    // while one of them is inside the loader would block here forever with that
    // thread still suspended: the diagnostic becomes the fault, and a permanent
    // one. The earlier comment here warned only about allocation and missed the
    // lock the walk itself takes.
    //
    // So the suspend window contains OpenThread, SuspendThread,
    // GetThreadContext, ResumeThread, CloseHandle and nothing else - all
    // syscalls, no user-mode lock, no allocation - and the unwinding happens in
    // phase 2 with every thread running again. The price is that a thread which
    // is genuinely running can move under the walk and produce a garbled tail;
    // the frames are guarded by __except for exactly that, and the threads that
    // matter in a hang are the ones that are not going anywhere. Frame 0 (Rip),
    // which is what the signature is built from, is captured exactly.
    std::vector<ThreadStack> stacks;
    std::vector<CONTEXT> contexts;
    stacks.reserve(tids.size());
    contexts.reserve(tids.size());  // ALLOCATED BEFORE the first suspend
    for (const DWORD tid : tids) {
        ThreadStack ts;
        ts.tid = tid;
        if (tid == self) {
            // Never suspend the thread doing the suspending. Nothing is
            // suspended at this point, so the full walk is safe here.
            ts.count = static_cast<int>(::RtlCaptureStackBackTrace(
                0, static_cast<ULONG>(kMaxHangFrames),
                reinterpret_cast<PVOID*>(ts.frames), nullptr));
            stacks.push_back(ts);
            contexts.push_back(CONTEXT{});  // already walked; nothing to unwind
            continue;
        }
        HANDLE h = ::OpenThread(
            THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
        if (h == nullptr) { continue; }
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_FULL;
        bool haveCtx = false;
        if (::SuspendThread(h) != static_cast<DWORD>(-1)) {
            haveCtx = ::GetThreadContext(h, &ctx) != 0;
            ::ResumeThread(h);
        }
        ::CloseHandle(h);
        if (haveCtx) {
#if defined(_M_X64)
            // Frame 0 now, from the register: the identifying half must not
            // depend on the walk succeeding.
            ts.frames[0] = static_cast<std::uintptr_t>(ctx.Rip);
            ts.count = (ctx.Rip != 0) ? 1 : 0;
#else
            ts.frames[0] = static_cast<std::uintptr_t>(ctx.Eip);
            ts.count = 1;
#endif
        }
        stacks.push_back(ts);
        contexts.push_back(ctx);
    }

    const char* topModule = "?";
    std::uintptr_t topOffset = 0;
    if (!stacks.empty() && stacks[0].count > 0) {
        DiagModule m;
        std::uintptr_t off = 0;
        if (resolveAddress(stacks[0].frames[0], m, off)) {
            topModule = m.name;
            topOffset = off;
        }
    }

    // PHASE 1b - THE STALLED THREAD'S WALK, AND WHY IT COMES BEFORE THE HEADER.
    //
    // The `kind` line is the first line of the file and decides how the report
    // groups, so it has to be right before anything is written - and telling a
    // display stall from a real hang needs the frames UNDER the top one, not
    // just frame 0 (which is ntdll.dll for every wait there is). So exactly one
    // walk is moved ahead of the header: the stalled thread's.
    //
    // WHAT THAT COSTS, plainly. The header is written first because phase 2's
    // unwinder can in principle block on the loader lock, and a report that
    // stops half way still names the fault. Walking one thread first means a
    // wedge THERE costs the whole report. It is bounded and it is the least bad
    // of the three options: the thread walked here is by definition not moving,
    // it is the one walk whose result the report cannot be written without, and
    // the alternative - filing every AMD display stall as a hang in this
    // application - is the defect being fixed. Every other thread is still
    // walked after the header, exactly as before.
    //
    // ONLY WHEN IT MIGHT MATTER: a stalled thread whose frame 0 is not a kernel
    // wait cannot be waiting on presentation, so the ordinary path is untouched
    // for it and the header goes out first as it always did.
    bool displayStall = false;
    bool guiWalked = false;
#if defined(_WIN32) && defined(_M_X64)
    if (!stacks.empty() && stacks[0].tid == gui && stacks[0].tid != self &&
        stacks[0].count > 0 && isKernelWaitModule(topModule)) {
        CONTEXT ctx = contexts[0];
        const int n = walkThreadContext(&ctx, stacks[0].frames, kMaxHangFrames);
        if (n > 0) {
            stacks[0].count = n;
            guiWalked = true;
            DiagModule mods[HangWatchdog::kDisplayStallScanFrames];
            const char* names[HangWatchdog::kDisplayStallScanFrames] = {};
            const int scan =
                (n < HangWatchdog::kDisplayStallScanFrames) ? n
                                                            : HangWatchdog::kDisplayStallScanFrames;
            for (int i = 0; i < scan; ++i) {
                std::uintptr_t off = 0;
                names[i] = resolveAddress(stacks[0].frames[i], mods[i], off) ? mods[i].name
                                                                            : nullptr;
            }
            displayStall = HangWatchdog::isDisplayPresentationStall(names, scan);
        } else {
            // The walk yielded nothing: frame 0 is still the register the
            // signature is built from, so put it back rather than leaving an
            // empty stack behind.
            stacks[0].frames[0] = static_cast<std::uintptr_t>(contexts[0].Rip);
            stacks[0].count = 1;
        }
    }
#endif

    // 0x48414E47 is 'HANG' - a hang and a crash at the same address are
    // different bugs and must not group together. 0x5354414C is 'STAL', and it
    // is a THIRD group for the same reason: a stall in the display driver and a
    // deadlock in this application can present the same top frame
    // (ntdll.dll+the same wait), and letting them share a signature would bury
    // a real bug under a pile of monitors being switched off.
    const std::string sig =
        crashSignature(displayStall ? 0x5354414Cul : 0x48414E47ul, topModule, topOffset);

    // THE IDENTIFYING HALF GOES TO DISK FIRST, and is flushed, exactly as
    // crash_handler.cpp does on the fault path. Phase 1 above can no longer
    // wedge, but phase 2 still calls into the unwinder, and if that ever does
    // block, the file already on disk names the fault: kind, how long, the
    // threshold it broke, the grouping signature and the whole application
    // context. A report that exists and is short beats one that was never
    // opened. tests/test_diag_hang.cpp stops a capture here and reads it back.
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) { return; }

    // THE HEADER. Inventoried in hangReportFieldNames() and documented in
    // PRIVACY.md; tests/test_diag_hang.cpp compares the two as SETS, both
    // ways, against a report written by a real stall. A line added here
    // without being added there fails that test.
    // TWO KINDS, and a sentence saying which. "stall" is a presentation stall
    // in the graphics stack - the application was waiting for a driver that was
    // waiting for a display - and it must not be counted, grouped or triaged
    // alongside a fault in this program. core/crash_upload.cpp keeps a stall on
    // the machine rather than sending it: the receiving end accepts crash and
    // hang, and the whole point of the classification is that this is not one
    // of ours to report.
    out << "kind: " << (displayStall ? "stall" : "hang") << "\n";
    out << "note: "
        << (displayStall
                ? "presentation stalled in the display driver - the frames under the wait "
                  "are the graphics stack, not this application. A monitor switched off, a "
                  "resolution change, a GPU reset or a remote session will do this."
                : "the gui thread did not complete a frame within the threshold")
        << "\n";
    out << "stalled-ms: " << static_cast<long long>(stalledMs) << "\n";
    // The threshold IN FORCE, which is what tells a reader whether this stall
    // was measured against the frame loop's 5 s or the teardown's own budget.
    out << "threshold-ms: " << thresholdMs_.load(std::memory_order_relaxed) << "\n";
    out << "signature: " << sig << "\n";
    out << "threads: " << stacks.size() << "\n";
    out << "--- context ---\n";
    out << diagContextBlock();

    // THE MODULE TABLE, and it was missing - which made every freeze report
    // this product writes permanently unreadable.
    //
    // A stack is module+offset, and an offset is unreadable hex forever unless
    // the PDB from THAT EXACT LINK can be found again. The build id is the only
    // key that finds it. crash_handler.cpp has written this block since phase
    // one and PRIVACY.md has described freeze reports as carrying it, but the
    // writer here never did: every stack below named a module and nothing said
    // which build of it. Since every fault this product has actually shipped
    // was a hang rather than a crash, that was the whole feature missing from
    // the half that matters most.
    //
    // Written BEFORE the unwind for the same reason the header is: a report
    // that stops here is still a report, and one that names the build is worth
    // more than one that does not. The line format is byte-identical to
    // crash_handler.cpp's so ONE parser reads both - two spellings of one table
    // is how a reader silently stops resolving one of the two kinds.
    out << "--- modules ---\n";
    {
        const int mods = moduleCount();
        for (int i = 0; i < mods; ++i) {
            DiagModule m;
            if (!moduleAt(i, m)) { continue; }
            char line[256];
            std::snprintf(line, sizeof(line), "  %s base=0x%016llX size=0x%llX pdb=%s build=%s\n",
                          m.name, static_cast<unsigned long long>(m.base),
                          static_cast<unsigned long long>(m.size),
                          m.pdb[0] != '\0' ? m.pdb : "(none)",
                          m.buildId[0] != '\0' ? m.buildId : "(none)");
            out << line;
        }
    }
    // The same process block crash_handler.cpp writes after its stack, minus
    // the fault-thread line a freeze has no meaning for: how long the session
    // had run when it stopped. Before the walk, like everything else that is
    // known before the walk.
    out << "--- process ---\n";
    out << "uptime-sec: " << processUptimeSec() << "\n";
    out.flush();

    {
        // Recorded before the walk for the same reason the header is written
        // before it: a report that stops here is still a report, and the
        // application must be able to find it.
        std::lock_guard<std::mutex> lk(pathMutex_);
        lastPath_ = path;
    }
    reports_.fetch_add(1, std::memory_order_release);

    if (captureAbort_.load(std::memory_order_relaxed) ==
        static_cast<int>(CaptureAbortForTest::AfterHeader)) {
        out.flush();
        out.close();
        return;
    }

    // PHASE 2 - the unwind, with every thread running again.
    for (std::size_t i = 0; i < stacks.size(); ++i) {
        ThreadStack& ts = stacks[i];
#if defined(_WIN32) && defined(_M_X64)
        // ...except the stalled thread, if phase 1b already walked it for the
        // classification. Walking it twice would unwind an already-unwound
        // context and produce one frame.
        if (ts.tid != self && ts.count > 0 && !(i == 0 && guiWalked)) {
            CONTEXT ctx = contexts[i];
            ts.count = walkThreadContext(&ctx, ts.frames, kMaxHangFrames);
            // A walk that yielded nothing still has the register frame, which
            // is the one the signature was built from.
            if (ts.count == 0) {
                ts.frames[0] = static_cast<std::uintptr_t>(contexts[i].Rip);
                ts.count = 1;
            }
        }
#endif
        out << "--- thread " << ts.tid;
        if (ts.tid == gui) {
            out << " (gui, stalled)";
        } else if (ts.tid == self) {
            out << " (watchdog)";
        }
        out << " ---\n";
        for (int i2 = 0; i2 < ts.count; ++i2) { out << frameLine(ts.frames[i2]); }
        // Flushed per thread: a wedge in the NEXT thread's walk still leaves
        // every stack captured before it.
        out.flush();
    }

    const std::vector<std::string> ring = DiagLog::instance().ringSnapshot();
    out << "--- log (last " << ring.size() << " of " << DiagLog::instance().linesWritten()
        << " lines) ---\n";
    for (const std::string& line : ring) { out << line << "\n"; }
    out.flush();
    out.close();
#elif defined(__linux__) && !defined(CASCADE_ANDROID)
    // LINUX. No suspend/resume window to get wrong (see the header comment
    // above captureOneLinuxThread): each thread is asked, via a realtime
    // signal, to unwind itself and hand the result back, and it keeps running
    // the whole time it is not actually inside that handler. So there is no
    // Windows-shaped phase 1 (registers only, nothing suspended) / phase 2
    // (unwind, everything running again) split - every thread's stack is
    // final the moment captureOneLinuxThread returns.
    //
    // What IS kept from the Windows shape: the header, the signature and the
    // context reach disk before any thread is asked to unwind, because the
    // same worry applies here as there - libunwind is not on the
    // async-signal-safe list either (see the comment above
    // hangCaptureSignalHandler) - so a wedge in a later thread's capture must
    // still leave a report that names the fault.
    ensureHangCaptureSignalInstalled();
    const pid_t self = linuxGetTid();
    const pid_t gui = static_cast<pid_t>(guiThreadId_.load(std::memory_order_relaxed));

    std::vector<pid_t> tids = listLinuxThreadIds();
    for (std::size_t i = 0; i < tids.size(); ++i) {
        if (tids[i] == gui) {
            std::swap(tids[0], tids[i]);
            break;
        }
    }

    // THE STALLED THREAD FIRST, same reason as Windows' phase 1b: the `kind`
    // line has to be decided before anything is written, and it needs frame 0
    // of the thread that actually stalled. No display-stall classification is
    // implemented on Linux (there is no equivalent graphics-driver module list
    // to check against), so every report here is `kind: hang`.
    ThreadStack first = tids.empty() ? ThreadStack{} : captureOneLinuxThread(tids[0], self);
    const char* topModule = "?";
    std::uintptr_t topOffset = 0;
    if (first.count > 0) {
        DiagModule m;
        std::uintptr_t off = 0;
        if (resolveAddress(first.frames[0], m, off)) {
            topModule = m.name;
            topOffset = off;
        }
    }
    const std::string sig = crashSignature(0x48414E47ul, topModule, topOffset);  // 'HANG'

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) { return; }

    out << "kind: hang\n";
    out << "note: the gui thread did not complete a frame within the threshold\n";
    out << "stalled-ms: " << static_cast<long long>(stalledMs) << "\n";
    out << "threshold-ms: " << thresholdMs_.load(std::memory_order_relaxed) << "\n";
    out << "signature: " << sig << "\n";
    out << "threads: " << tids.size() << "\n";
    out << "--- context ---\n";
    out << diagContextBlock();

    // Same layout as crash_handler_posix.cpp and the Windows writer above, so
    // one parser (crash_upload.cpp's parseReportText) reads all three.
    out << "--- modules ---\n";
    {
        const int mods = moduleCount();
        for (int i = 0; i < mods; ++i) {
            DiagModule m;
            if (!moduleAt(i, m)) { continue; }
            char line[256];
            std::snprintf(line, sizeof(line), "  %s base=0x%016llX size=0x%llX pdb=%s build=%s\n",
                          m.name, static_cast<unsigned long long>(m.base),
                          static_cast<unsigned long long>(m.size),
                          m.pdb[0] != '\0' ? m.pdb : "(none)",
                          m.buildId[0] != '\0' ? m.buildId : "(none)");
            out << line;
        }
    }
    out << "--- process ---\n";
    out << "uptime-sec: " << processUptimeSec() << "\n";
    out.flush();

    {
        std::lock_guard<std::mutex> lk(pathMutex_);
        lastPath_ = path;
    }
    reports_.fetch_add(1, std::memory_order_release);

    if (captureAbort_.load(std::memory_order_relaxed) ==
        static_cast<int>(CaptureAbortForTest::AfterHeader)) {
        out.flush();
        out.close();
        return;
    }

    for (std::size_t i = 0; i < tids.size(); ++i) {
        const ThreadStack ts = (i == 0) ? first : captureOneLinuxThread(tids[i], self);
        out << "--- thread " << ts.tid;
        if (static_cast<pid_t>(ts.tid) == gui) {
            out << " (gui, stalled)";
        } else if (static_cast<pid_t>(ts.tid) == self) {
            out << " (watchdog)";
        }
        out << " ---\n";
        if (ts.count == 0) {
            out << "  (no frames: the thread did not answer the capture signal within "
                   "200 ms)\n";
        }
        for (int k = 0; k < ts.count; ++k) { out << frameLine(ts.frames[k]); }
        out.flush();
    }

    const std::vector<std::string> ring = DiagLog::instance().ringSnapshot();
    out << "--- log (last " << ring.size() << " of " << DiagLog::instance().linesWritten()
        << " lines) ---\n";
    for (const std::string& line : ring) { out << line << "\n"; }
    out.flush();
    out.close();
#else
    // ANDROID-TODO(crash-capture): no libunwind local-unwind API on the NDK,
    // so a stalled thread's other-thread stacks cannot be captured here. The
    // watchdog still detects and reports the stall's timing (threadMain()
    // above) - only the per-thread frame dump is unavailable.
    diagWarnf("watchdog: other-thread stack capture is not available on this "
              "platform (ANDROID-TODO(crash-capture)); no hang report written");
    (void)path;
    (void)stalledMs;
#endif
}

}  // namespace cascade::core
