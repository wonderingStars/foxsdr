// thread_priority.cpp - see thread_priority.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/thread_priority.hpp"

#include <atomic>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <pthread.h>
#include <sched.h>
#endif

namespace cascade::core {

namespace {

std::atomic<bool> g_mmcssDisabledForTest{false};

#if defined(_WIN32)

// avrt.dll, resolved ONCE and at run time. It is never linked: the same sources
// build for Linux and Android, and a Windows with the Multimedia Class Scheduler
// service stopped (or no avrt.dll at all) must still start the program. Looked up
// in System32 only, so a copy beside the executable or on the PATH is never
// loaded in its place. The module is left loaded for the life of the process: a
// thread that is still inside the scheduler's calls must never find it gone.
using AvSetFn = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);
using AvRevertFn = BOOL(WINAPI*)(HANDLE);

struct Avrt {
    AvSetFn set = nullptr;
    AvRevertFn revert = nullptr;
    Avrt() {
        HMODULE m = ::LoadLibraryExW(L"avrt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (m == nullptr) { return; }
        set = reinterpret_cast<AvSetFn>(
            reinterpret_cast<void*>(::GetProcAddress(m, "AvSetMmThreadCharacteristicsW")));
        revert = reinterpret_cast<AvRevertFn>(
            reinterpret_cast<void*>(::GetProcAddress(m, "AvRevertMmThreadCharacteristics")));
        if (set == nullptr || revert == nullptr) {
            set = nullptr;
            revert = nullptr;
        }
    }
};

const Avrt& avrt() {
    static const Avrt a;   // thread-safe initialisation
    return a;
}

#endif

}  // namespace

void ThreadPriorityScope::setMmcssDisabledForTest(bool disabled) {
    g_mmcssDisabledForTest.store(disabled, std::memory_order_relaxed);
}

#if defined(_WIN32)

ThreadPriorityScope::ThreadPriorityScope() {
    // The Multimedia Class Scheduler, class "Pro Audio": what an audio program is
    // meant to ask for, and the one that survives the machine being busy.
    std::string why;   // what to say if neither works
    if (g_mmcssDisabledForTest.load(std::memory_order_relaxed)) {
        why = "MMCSS switched off";
    } else {
        const Avrt& av = avrt();
        if (av.set == nullptr) {
            why = "no MMCSS (avrt.dll unavailable)";
        } else {
            DWORD taskIndex = 0;
            HANDLE task = av.set(L"Pro Audio", &taskIndex);
            if (task != nullptr) {
                kind_ = Kind::Mmcss;
                handle_ = task;
                outcome_ = "mmcss";
                return;
            }
            why = "MMCSS refused (error " + std::to_string(static_cast<unsigned long>(::GetLastError())) + ")";
        }
    }
    // The fallback: one step above normal, put back at the end. A thread that is
    // already there (or higher) is left alone and reported the same way.
    HANDLE self = ::GetCurrentThread();
    const int before = ::GetThreadPriority(self);
    if (before == THREAD_PRIORITY_ERROR_RETURN) {
        outcome_ = "none: " + why + "; priority unreadable";
        return;
    }
    if (before >= THREAD_PRIORITY_ABOVE_NORMAL) {
        kind_ = Kind::AboveNormal;
        previousA_ = before;
        previousB_ = 0;   // nothing was changed, so nothing to put back
        outcome_ = "above-normal";
        return;
    }
    if (::SetThreadPriority(self, THREAD_PRIORITY_ABOVE_NORMAL) == 0) {
        outcome_ = "none: " + why + "; SetThreadPriority refused (error " +
                   std::to_string(static_cast<unsigned long>(::GetLastError())) + ")";
        return;
    }
    kind_ = Kind::AboveNormal;
    previousA_ = before;
    previousB_ = 1;
    outcome_ = "above-normal";
}

ThreadPriorityScope::~ThreadPriorityScope() {
    if (kind_ == Kind::Mmcss) {
        const Avrt& av = avrt();
        if (av.revert != nullptr && handle_ != nullptr) { av.revert(static_cast<HANDLE>(handle_)); }
    } else if (kind_ == Kind::AboveNormal && previousB_ != 0) {
        ::SetThreadPriority(::GetCurrentThread(), previousA_);
    }
}

#elif defined(__linux__) || defined(__ANDROID__)

ThreadPriorityScope::ThreadPriorityScope() {
    handle_ = nullptr;   // (the Windows task handle; nothing to hold here)
    // SCHED_RR at the lowest real-time priority. An ordinary user is refused
    // (EPERM) unless RLIMIT_RTPRIO allows it, and that is the normal case: the
    // thread is left exactly as it was and the outcome says so. Nice is not
    // raised - it needs privileges too, and is a different promise.
    int prevPolicy = 0;
    sched_param prev{};
    if (pthread_getschedparam(pthread_self(), &prevPolicy, &prev) != 0) {
        outcome_ = "none: scheduling policy unreadable";
        return;
    }
    sched_param want{};
    want.sched_priority = 1;
    const int rc = pthread_setschedparam(pthread_self(), SCHED_RR, &want);
    if (rc == 0) {
        kind_ = Kind::RoundRobin;
        previousA_ = prevPolicy;
        previousB_ = prev.sched_priority;
        outcome_ = "sched-rr";
        return;
    }
    outcome_ = rc == EPERM ? std::string("none: EPERM") : "none: error " + std::to_string(rc);
}

ThreadPriorityScope::~ThreadPriorityScope() {
    if (kind_ == Kind::RoundRobin) {
        sched_param back{};
        back.sched_priority = previousB_;
        pthread_setschedparam(pthread_self(), previousA_, &back);
    }
}

#else

ThreadPriorityScope::ThreadPriorityScope() {
    handle_ = nullptr;
    outcome_ = "none: not supported on this platform";
}

ThreadPriorityScope::~ThreadPriorityScope() {}

#endif

}  // namespace cascade::core
