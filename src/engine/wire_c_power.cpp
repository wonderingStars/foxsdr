// wire_c_power.cpp - see wire_c_power.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/wire_c_power.hpp"

#include <chrono>
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
    char buf[48];
    if (snap.processCpuPercent.available) { std::snprintf(buf, sizeof(buf), "%.1f", snap.processCpuPercent.value); f.cpuPercent = buf; }
    if (snap.workingSetBytes.available) { f.memoryMb = std::to_string(snap.workingSetBytes.value / (1024u * 1024u)); }
    if (snap.systemMemoryPercent.available) { std::snprintf(buf, sizeof(buf), "%.0f", snap.systemMemoryPercent.value); f.systemMemoryPercent = buf; }
    if (snap.gpuPercent.available) { std::snprintf(buf, sizeof(buf), "%.0f", snap.gpuPercent.value); f.gpuPercent = buf; }
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
