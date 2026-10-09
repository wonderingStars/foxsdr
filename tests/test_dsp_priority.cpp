// THE DSP AND SOURCE THREADS ASK FOR PRIORITY (0.99.73, core/thread_priority.hpp).
//
// THE FIELD REPORT (12CF, a Store user on 0.99.64, RSP1A at 2.048 MS/s, thirty
// plugins loaded): 110 to 126 starved audio callbacks every minute. PortAudio
// raises its own callback thread; the thread that PRODUCES the audio - the DSP
// thread, which every plugin's process() also runs on - ran at normal priority
// beside the window and everything else the machine was doing.
//
// WHAT THIS HOLDS.
//   1. The scope itself, on a thread of its own: the outcome is a real one, the
//      thread's priority really is higher while the scope lives and exactly what
//      it was afterwards. On Windows the outcome must be "mmcss" or
//      "above-normal" - never "none", which would mean the request is a no-op.
//      Linux may be refused (an ordinary user cannot take SCHED_RR) and says
//      "none: ..." in that case; it must not be anything else.
//   2. The Windows fallback, forced through the scope's test seam: with MMCSS
//      refused the thread is raised to THREAD_PRIORITY_ABOVE_NORMAL and put back.
//   3. The real Pipeline: after start() the DSP thread and the source thread each
//      report how their request came out, and the log has the lines
//      (`dsp: thread priority <outcome>`, `dsp: source thread priority <outcome>`),
//      once per thread start.
//
// A priority that is only SAID is no priority, so (1) and (2) read the priority
// back from the operating system and not from the object that set it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/pipeline.hpp"

#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

#include "core/diag_log.hpp"
#include "core/thread_priority.hpp"
#include "test_check.hpp"

using cascade::core::Pipeline;
using cascade::core::ThreadPriorityScope;

namespace {

// What the operating system says this thread's priority is right now. Windows:
// the relative priority (GetThreadPriority). Elsewhere: the policy and priority
// packed into one number.
int priorityNow() {
#if defined(_WIN32)
    return ::GetThreadPriority(::GetCurrentThread());
#else
    int policy = 0;
    sched_param param{};
    pthread_getschedparam(pthread_self(), &policy, &param);
    return policy * 1000 + param.sched_priority;
#endif
}

// What one scope did on a thread of its own.
struct Seen {
    std::string outcome;
    bool raised = false;
    int before = 0;
    int during = 0;
    int after = 0;
};

Seen scopeOnAThread() {
    Seen s;
    std::thread t([&s] {
        s.before = priorityNow();
        {
            const ThreadPriorityScope scope;
            s.outcome = scope.outcome();
            s.raised = scope.raised();
            s.during = priorityNow();
        }
        s.after = priorityNow();
    });
    t.join();
    return s;
}

bool startsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

// How many lines of the log contain `text`.
int logLinesWith(const std::string& text) {
    int n = 0;
    for (const std::string& line : cascade::core::DiagLog::instance().ringSnapshot()) {
        if (line.find(text) != std::string::npos) { ++n; }
    }
    return n;
}

}  // namespace

int main() {
    // --- 1. The scope on a thread of its own -------------------------------------
    {
        const Seen s = scopeOnAThread();
        std::printf("thread priority scope: outcome \"%s\", priority %d -> %d -> %d\n",
                    s.outcome.c_str(), s.before, s.during, s.after);
        CHECK(!s.outcome.empty());
        CHECK(s.after == s.before);   // put back, whatever it did
#if defined(_WIN32)
        // The Multimedia Class Scheduler, or the fallback; never a no-op.
        CHECK(s.outcome == "mmcss" || s.outcome == "above-normal");
        CHECK(s.raised);
        // And the operating system agrees: this thread is not at the priority it
        // started at. (MMCSS moves the base priority itself, so the figure is
        // whatever the class gives; what is pinned is that it is HIGHER.)
        CHECK(s.during > s.before);
#else
        CHECK(s.outcome == "sched-rr" || startsWith(s.outcome, "none: "));
        CHECK(s.raised == (s.outcome == "sched-rr"));
        if (s.outcome == "sched-rr") { CHECK(s.during != s.before); }
        if (!s.raised) { CHECK(s.during == s.before); }
#endif
    }

    // --- 2. The Windows fallback, forced -----------------------------------------
#if defined(_WIN32)
    {
        ThreadPriorityScope::setMmcssDisabledForTest(true);
        const Seen s = scopeOnAThread();
        ThreadPriorityScope::setMmcssDisabledForTest(false);
        std::printf("thread priority scope without MMCSS: outcome \"%s\", priority %d -> %d -> %d\n",
                    s.outcome.c_str(), s.before, s.during, s.after);
        CHECK(s.outcome == "above-normal");
        CHECK(s.raised);
        CHECK(s.before == THREAD_PRIORITY_NORMAL);
        CHECK(s.during == THREAD_PRIORITY_ABOVE_NORMAL);
        CHECK(s.after == THREAD_PRIORITY_NORMAL);
        // ...and the seam is off again: the next scope asks MMCSS as it should.
        const Seen again = scopeOnAThread();
        CHECK(again.outcome == "mmcss" || again.outcome == "above-normal");
    }
#endif

    // --- 3. The real Pipeline ----------------------------------------------------
    {
        Pipeline::Config cfg;
        cfg.sampleRateHz = 1000000.0;
        cfg.fftSize = 1024;
        Pipeline p(cfg);
        CHECK(p.dspThreadPriority().empty());      // no thread has started yet
        CHECK(p.sourceThreadPriority().empty());

        p.start();
        const auto t0 = std::chrono::steady_clock::now();
        while ((p.dspThreadPriority().empty() || p.sourceThreadPriority().empty()) &&
               std::chrono::steady_clock::now() - t0 < std::chrono::seconds(20)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        const std::string dsp = p.dspThreadPriority();
        const std::string src = p.sourceThreadPriority();
        std::printf("pipeline: DSP thread priority \"%s\", source thread priority \"%s\"\n",
                    dsp.c_str(), src.c_str());
#if defined(_WIN32)
        CHECK(dsp == "mmcss" || dsp == "above-normal");
        CHECK(src == "mmcss" || src == "above-normal");
#else
        CHECK(dsp == "sched-rr" || startsWith(dsp, "none: "));
        CHECK(src == "sched-rr" || startsWith(src, "none: "));
#endif
        // The log says the same, in the words docs/DIAGNOSTICS.md gives.
        CHECK(logLinesWith("dsp: thread priority " + dsp) == 1);
        CHECK(logLinesWith("dsp: source thread priority " + src) == 1);

        // A stop and a start is a new thread, a new request and a new line.
        p.stop();
        p.start();
        const auto t1 = std::chrono::steady_clock::now();
        while (logLinesWith("dsp: thread priority " + dsp) < 2 &&
               std::chrono::steady_clock::now() - t1 < std::chrono::seconds(20)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        CHECK(logLinesWith("dsp: thread priority " + dsp) == 2);
        CHECK(p.dspThreadPriority() == dsp);
        p.stop();
    }

    return testSummary("test_dsp_priority");
}
