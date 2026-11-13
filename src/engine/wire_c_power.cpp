// wire_c_power.cpp - see wire_c_power.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/wire_c_power.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>

#include "core/diag_log.hpp"

namespace cascade::engine::wirec {

namespace sys = cascade::core::sys;

Power::Power(PowerSeams seams)
    : seams_(seams), keepAwake_(seams.keepAwake), meter_(seams.sources) {}

Power::~Power() {
    {
        std::lock_guard<std::mutex> lk(loopMu_);
        stopping_ = true;
    }
    loopCv_.notify_all();
    if (sampler_.joinable()) { sampler_.join(); }
    if (resumeWatch_) { resumeWatch_->stop(); }
    lease_.reset();
}

bool Power::keepAwakePass(bool wanted) {
    if (wanted && !lease_) {
        lease_ = keepAwake_.acquire();
        if (!keepAwake_.active() && !warned_) {
            warned_ = true;
            cascade::core::diagLogf("power: could not keep the PC awake while recording");
        }
        return true;
    }
    if (!wanted && lease_) {
        lease_.reset();
        return true;
    }
    return false;
}

void Power::setPerfMode(sys::EffectsMode m) { mode_.store(static_cast<int>(m), std::memory_order_relaxed); }

void Power::sampleOnce() {
    std::lock_guard<std::mutex> lk(sampleMu_);
    // (setMode restarts the policy's streaks, so it is called only when the user's choice CHANGED)
    const int wanted = mode_.load(std::memory_order_relaxed);
    if (wanted != modeApplied_) {
        modeApplied_ = wanted;
        policy_.setMode(static_cast<sys::EffectsMode>(wanted));
    }
    const sys::ResourceSnapshot snap = meter_.sample();
    if (!lowEndChecked_) {
        lowEndChecked_ = true;
        policy_.startLowEnd(snap);   // a plainly small machine starts reduced (until calm readings say otherwise)
    }
    const sys::EffectsSwitches& sw = policy_.update(snap);
    PerfFacts f;
    f.sampled = true;
    // THE FIGURES ARE PUBLISHED IN COARSE STEPS (CPU and GPU to 10 %, the process memory to 50 MB, the system memory to 5 %): a published fact that moves moves the engine's list
    // counter (listSeq), and a read-out that changed every second would make every client re-read every list every second for a figure nobody can read that fast.
    auto step = [](double v, double by) { return std::llround(v / by) * static_cast<long long>(by); };
    if (snap.processCpuPercent.available) { f.cpuPercent = std::to_string(step(snap.processCpuPercent.value, 10.0)); }
    if (snap.workingSetBytes.available) { f.memoryMb = std::to_string(step(static_cast<double>(snap.workingSetBytes.value) / (1024.0 * 1024.0), 50.0)); }
    if (snap.systemMemoryPercent.available) { f.systemMemoryPercent = std::to_string(step(snap.systemMemoryPercent.value, 5.0)); }
    if (snap.gpuPercent.available) { f.gpuPercent = std::to_string(step(snap.gpuPercent.value, 10.0)); }
    f.reduced = sw.reduced;
    f.switches = sw;
    {
        std::lock_guard<std::mutex> pk(perfMu_);
        const bool same = perf_.sampled && perf_.cpuPercent == f.cpuPercent && perf_.memoryMb == f.memoryMb && perf_.systemMemoryPercent == f.systemMemoryPercent &&
                          perf_.gpuPercent == f.gpuPercent && perf_.reduced == f.reduced;
        perf_ = f;
        if (!same) { perfVersion_.fetch_add(1, std::memory_order_relaxed); }
    }
}

PerfFacts Power::perf() const {
    std::lock_guard<std::mutex> lk(perfMu_);
    return perf_;
}

void Power::startSampler() {
    if (sampler_.joinable()) { return; }
    sampler_ = std::thread([this] { loop(); });
}

void Power::loop() {
    for (;;) {
        sampleOnce();
        std::unique_lock<std::mutex> lk(loopMu_);
        if (loopCv_.wait_for(lk, std::chrono::milliseconds(seams_.samplePeriodMs > 0 ? seams_.samplePeriodMs : 1000), [this] { return stopping_; })) { return; }
    }
}

void Power::startResumeWatch() {
    if (resumeWatch_) { return; }
    resumeWatch_ = std::make_unique<sys::ResumeWatcher>(seams_.resume, [this] {
        resumeFlag_.store(true, std::memory_order_release);   // (the watcher's thread: an atomic only; the control pass acts)
        resumes_.fetch_add(1, std::memory_order_relaxed);
    });
    if (!resumeWatch_->start(seams_.registerResumeWithOs)) {
        cascade::core::diagLogf("power: the wake-from-sleep notice could not be registered");   // (the thread still runs for the tests' injection)
    }
}

bool Power::takeResume() { return resumeFlag_.exchange(false, std::memory_order_acq_rel); }

void Power::injectPowerEvent(sys::PowerEvent e) {
    if (resumeWatch_) { resumeWatch_->inject(e); }
}

}  // namespace cascade::engine::wirec
