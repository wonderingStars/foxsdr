// thread_priority.hpp - the DSP and source threads ask the operating system to
// schedule them ahead of ordinary work (0.99.73).
//
// WHY. The 12CF report: a Store user on 0.99.64, an RSP1A at 2.048 MS/s, thirty
// plugins loaded, 110 to 126 starved audio callbacks every minute and frames of
// 260 to 320 ms. PortAudio raises its OWN callback thread (the consumer of the
// audio ring); the producer - Pipeline::dspThreadMain, which every plugin's
// process() also runs on - and the thread that fills the I/Q ring ran at normal
// priority, in the same queue as the window, the plugins' own threads and
// whatever else the machine was doing. A producer that is scheduled late is
// exactly what starves the consumer.
//
// WHAT IT DOES.
//   Windows  the Multimedia Class Scheduler, class "Pro Audio"
//            (AvSetMmThreadCharacteristicsW). avrt.dll is loaded AT RUN TIME and
//            never linked: the same sources build for Linux and Android, and a
//            Windows without the MMCSS service must still start. When that fails
//            (no avrt, the service stopped, a policy), the thread is raised to
//            THREAD_PRIORITY_ABOVE_NORMAL instead. Both are put back when the
//            scope ends, on the same thread.
//   Linux    pthread_setschedparam(SCHED_RR, priority 1). An ordinary user is
//            refused (EPERM) unless RLIMIT_RTPRIO allows it; that is tolerated
//            and the thread stays as it was. Nice is NOT raised: it needs
//            privileges and would be a different promise.
//   Other    nothing, and it says so.
//
// THE OUTCOME is one of "mmcss", "above-normal", "sched-rr" or "none: <reason>",
// and is what the application logs (`dsp: thread priority <outcome>`,
// docs/DIAGNOSTICS.md). It never carries a device name, a path or an operating
// system sentence: the reason is a fixed word or an error NUMBER.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <string>

namespace cascade::core {

// Raises the CALLING thread for as long as the object lives. Construct it at the
// top of the thread's function; the destructor must run on the same thread.
class ThreadPriorityScope {
public:
    ThreadPriorityScope();
    ~ThreadPriorityScope();
    ThreadPriorityScope(const ThreadPriorityScope&) = delete;
    ThreadPriorityScope& operator=(const ThreadPriorityScope&) = delete;

    // "mmcss", "above-normal", "sched-rr" or "none: <reason>".
    const std::string& outcome() const { return outcome_; }
    // True when the thread was actually raised (anything but "none: ...").
    bool raised() const { return kind_ != Kind::None; }

    // TEST SEAM. True makes the next scopes skip the Multimedia Class Scheduler,
    // so the fallback ("above-normal") can be exercised on a machine where MMCSS
    // works. Process-wide; never set by the application.
    static void setMmcssDisabledForTest(bool disabled);

private:
    enum class Kind { None, Mmcss, AboveNormal, RoundRobin };
    Kind kind_ = Kind::None;
    std::string outcome_;
    // What to put back. Windows: the MMCSS task handle, or the thread priority
    // the thread had. Linux: the scheduling policy and priority it had. Kept as
    // plain integers so no system header is needed here.
    void* handle_ = nullptr;
    int previousA_ = 0;
    int previousB_ = 0;
};

}  // namespace cascade::core
