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

}  // namespace

HangWatchdog::~HangWatchdog() { stop(); }

void HangWatchdog::start(const std::string& reportDir, unsigned thresholdMs) {
    if (running_.load(std::memory_order_acquire)) { return; }
    {
        std::lock_guard<std::mutex> lk(dirMutex_);
        reportDir_ = reportDir;
    }
    thresholdMs_ = (thresholdMs > 0) ? thresholdMs : kDefaultThresholdMs;
#if defined(_WIN32)
    guiThreadId_.store(::GetCurrentThreadId(), std::memory_order_relaxed);
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

void HangWatchdog::stop() {
    stop_.store(true, std::memory_order_release);
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

bool HangWatchdog::suppressed() const {
    const int mode = suppression_.load(std::memory_order_relaxed);
    if (mode == static_cast<int>(SuppressionForTest::NeverSuppress)) { return false; }
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
#endif
    return false;
}

void HangWatchdog::threadMain() {
    double lastPoll = nowMs();
    while (!stop_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
        const double now = nowMs();
        const double pollInterval = now - lastPoll;
        lastPoll = now;

        // 3. THE WHOLE MACHINE STOPPED. Sleep, hibernate or a paused VM freezes
        //    this thread too. If the watchdog lost as much time as it is about
        //    to accuse the GUI thread of losing, it cannot tell the two apart -
        //    so it re-arms and says nothing.
        if (pollInterval > static_cast<double>(kPollMs) + static_cast<double>(thresholdMs_)) {
            lastBeatMs_.store(now, std::memory_order_relaxed);
            continue;
        }

        if (paused_.load(std::memory_order_relaxed) > 0) {
            lastBeatMs_.store(now, std::memory_order_relaxed);
            continue;
        }

        const double stalled = now - lastBeatMs_.load(std::memory_order_relaxed);
        if (stalled < static_cast<double>(thresholdMs_)) {
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
    // 0x48414E47 is 'HANG' - a hang and a crash at the same address are
    // different bugs and must not group together.
    const std::string sig = crashSignature(0x48414E47ul, topModule, topOffset);

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
    out << "kind: hang\n";
    out << "stalled-ms: " << static_cast<long long>(stalledMs) << "\n";
    out << "threshold-ms: " << thresholdMs_ << "\n";
    out << "signature: " << sig << "\n";
    out << "threads: " << stacks.size() << "\n";
    out << "--- context ---\n";
    out << diagContextBlock();
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
        if (ts.tid != self && ts.count > 0) {
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
#else
    (void)path;
    (void)stalledMs;
#endif
}

}  // namespace cascade::core
