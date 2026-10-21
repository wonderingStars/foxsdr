// test_engine_api_hermetic.cpp - the engine-API tests' hermetic environment leaves the machine's scheduler alone.
//
// WHY THIS EXISTS (the 0.99.84 merge, docs/engine-merge-0.99.84.md section 10). Master's 0.99.73 asks the
// operating system to schedule every pipeline's DSP thread and source thread ahead of ordinary work
// (core/thread_priority.hpp: SCHED_RR priority 1 on Linux, MMCSS "Pro Audio" on Windows). Linux refuses an ordinary user
// (EPERM) and grants a privileged one - and the ThreadSanitizer leg runs as root under WSL. test_engine_api_merge runs
// 24 engines at once, so 48 threads that do their DSP work flat out under ThreadSanitizer became real-time threads on a
// 32-processor machine: the ordinary threads (each engine's control thread, the test's workers, the transmitter's thread)
// got what the real-time throttle left them, the sweep took 458 s instead of 106 s, and 793 of its 5,450 cases saw a
// keep-alive lapse ("the interface holding the key stopped answering") because the control thread did not run for over a
// second. Nothing was wrong with the key's rules: the test rig was starving its own control threads.
//
// WHAT THIS HOLDS. After engine_api_test::hermetic(), the pipeline's two threads are NOT raised, whoever runs the test
// (root, or an ordinary user, or Windows): their outcome is "none: ..." and the operating system agrees (on Linux the
// scheduling policy of every thread of this process is still SCHED_OTHER). The priority request itself is
// tests/test_dsp_priority.cpp's.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>

#if defined(__linux__)
#include <dirent.h>
#endif

#include "check.hpp"
#include "core/pipeline.hpp"
#include "engine_api_test_env.hpp"

namespace {

bool startsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

// Linux: how many threads of this process run under a real-time policy (SCHED_FIFO 1 or SCHED_RR 2; field 41 of
// /proc/self/task/<tid>/stat). -1 where /proc cannot say (not Linux).
int realtimeThreads() {
#if defined(__linux__)
    DIR* d = ::opendir("/proc/self/task");
    if (d == nullptr) { return -1; }
    int n = 0;
    while (const dirent* e = ::readdir(d)) {
        if (e->d_name[0] == '.') { continue; }
        std::ifstream f(std::string("/proc/self/task/") + e->d_name + "/stat");
        std::string line;
        std::getline(f, line);
        // The command is in parentheses and may hold spaces: count the fields after the last ')'. Field 3 is the first
        // after it, so the policy (field 41) is the 39th token after it.
        const std::size_t close = line.rfind(')');
        if (close == std::string::npos) { continue; }
        std::size_t pos = close + 1;
        int field = 2;
        std::string policy;
        while (pos < line.size()) {
            while (pos < line.size() && line[pos] == ' ') { ++pos; }
            const std::size_t end = line.find(' ', pos);
            const std::string tok = line.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
            ++field;
            if (field == 41) {
                policy = tok;
                break;
            }
            if (end == std::string::npos) { break; }
            pos = end;
        }
        if (policy == "1" || policy == "2") { ++n; }
    }
    ::closedir(d);
    return n;
#else
    return -1;
#endif
}

}  // namespace

int main() {
    engine_api_test::hermetic("hermetic");

    cascade::core::Pipeline::Config cfg;
    cfg.sampleRateHz = 1000000.0;
    cfg.fftSize = 1024;
    cascade::core::Pipeline p(cfg);
    p.start();
    const auto t0 = std::chrono::steady_clock::now();
    while ((p.dspThreadPriority().empty() || p.sourceThreadPriority().empty()) &&
           std::chrono::steady_clock::now() - t0 < std::chrono::seconds(20)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const std::string dsp = p.dspThreadPriority();
    const std::string src = p.sourceThreadPriority();
    const int rt = realtimeThreads();
    std::printf("hermetic: DSP thread priority \"%s\", source thread priority \"%s\", real-time threads in this process: %d\n",
                dsp.c_str(), src.c_str(), rt);
    CHECK(startsWith(dsp, "none: "));
    CHECK(startsWith(src, "none: "));
#if defined(__linux__)
    CHECK_EQ(rt, 0);
#endif
    p.stop();
    return check::finish("test_engine_api_hermetic");
}
