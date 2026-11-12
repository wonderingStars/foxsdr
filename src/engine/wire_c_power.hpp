// wire_c_power.hpp - the engine's power and resource work (wiring lane C rows S3, S4, S5): keep the PC awake while a recording runs, the once-a-second
// resource read-out with the reduced-effects policy, and what happens when the PC wakes from sleep. The models are core/sys_keep_awake, sys_resource_meter,
// sys_effects_policy and sys_resume_watch; this file is only the engine's use of them.
//
// KEEP AWAKE (S4). The engine says once a pass whether anything is recording (an audio or I/Q take, the IF or multiplex take, a scheduled capture that is
// starting or recording); Power holds one lease while that is so and releases it the pass it stops, on every way a take can end, because the decision is made from
// the state, not from the start and stop sites. The DISPLAY is never held on. Opt-out setting sys.keep_awake (default on). If the OS refuses the request the
// reason is logged once ("could not keep the PC awake").
//
// RESOURCES (S5). A worker thread samples at the seam's period (1 s in the program): process CPU %, working set, system memory %, GPU engine % (each with its own
// available flag), runs the reduced-effects policy (5 high samples in, 30 calm out; a dead band between) and publishes the last reading. NOTHING OF THIS RUNS ON THE
// CONTROL PASS (the GPU counter is a Windows performance-counter query); the pass only reads the published copy. perf.mode (auto / on / off) is the user's override.
// The engine does not change the picture itself: the screen reads fact.perf.reduced and the switches.
//
// RESUME (S3). A ResumeWatcher sets a flag when the PC has been awake again for the settle time (a suspend cancels; at least 30 s between two); the control pass takes
// it once (takeResume). What the engine does with it is its own (engine_wire_c.cpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "core/sys_effects_policy.hpp"
#include "core/sys_keep_awake.hpp"
#include "core/sys_resource_meter.hpp"
#include "core/sys_resume_watch.hpp"

namespace cascade::engine::wirec {

struct PowerSeams {
    cascade::core::sys::KeepAwake::Fns keepAwake = cascade::core::sys::KeepAwake::realFns();
    cascade::core::sys::ResourceSources sources = cascade::core::sys::realResourceSources();
    std::int64_t samplePeriodMs = 1000;
    bool registerResumeWithOs = true;
    cascade::core::sys::ResumeGateConfig resume{};
};

struct PerfFacts {
    bool sampled = false;       // at least one reading has been made
    std::string cpuPercent;     // "" = not available (never 0)
    std::string memoryMb;
    std::string systemMemoryPercent;
    std::string gpuPercent;
    bool reduced = false;
    cascade::core::sys::EffectsSwitches switches;
};

class Power {
public:
    explicit Power(PowerSeams seams);
    ~Power();
    Power(const Power&) = delete;
    Power& operator=(const Power&) = delete;

    // KEEP AWAKE: called once a pass. Returns true when the holding state changed (a fact moved).
    bool keepAwakePass(bool wanted);
    bool keepAwakeHeld() const { return lease_ ? true : false; }
    bool keepAwakeActive() const { return keepAwake_.active(); }
    const cascade::core::sys::KeepAwake& keepAwake() const { return keepAwake_; }

    // RESOURCES: the user's override (auto / on / off) is applied by the worker at its next sample; the worker is started by the first call.
    void setPerfMode(cascade::core::sys::EffectsMode m);
    void startSampler();
    void sampleOnce();              // one sample + policy step on the calling thread (the worker calls it; a test may too, before startSampler)
    PerfFacts perf() const;
    std::uint64_t perfVersion() const { return perfVersion_.load(std::memory_order_relaxed); }

    // RESUME: starts the watcher once (idempotent); takeResume() is true once for each action that fired.
    void startResumeWatch();
    bool takeResume();
    void injectPowerEvent(cascade::core::sys::PowerEvent e);   // tests: the path the OS callback takes
    std::uint64_t resumeCount() const { return resumes_.load(std::memory_order_relaxed); }
    bool resumeWatchStarted() const { return resumeWatch_ != nullptr; }

private:
    void loop();

    PowerSeams seams_;
    cascade::core::sys::KeepAwake keepAwake_;
    cascade::core::sys::KeepAwake::Lease lease_;
    bool warned_ = false;

    cascade::core::sys::ResourceMeter meter_;
    cascade::core::sys::EffectsPolicy policy_;
    std::atomic<int> mode_{0};   // 0 auto, 1 on, 2 off
    int modeApplied_ = 0;        // the mode the policy was last given (the worker's own)
    bool lowEndChecked_ = false;
    mutable std::mutex perfMu_;
    PerfFacts perf_;
    std::atomic<std::uint64_t> perfVersion_{1};
    std::mutex sampleMu_;        // one sample at a time (the worker's, or a test's)
    std::mutex loopMu_;
    std::condition_variable loopCv_;
    bool stopping_ = false;
    std::thread sampler_;

    std::unique_ptr<cascade::core::sys::ResumeWatcher> resumeWatch_;
    std::atomic<bool> resumeFlag_{false};
    std::atomic<std::uint64_t> resumes_{0};
};

}  // namespace cascade::engine::wirec
