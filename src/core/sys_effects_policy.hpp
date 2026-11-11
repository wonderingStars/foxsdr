// sys_effects_policy.hpp - the reduced-effects ("low-end") policy: a struct of switches derived from the resource readings, with hysteresis.
//
// WHAT IT DECIDES. Given a stream of ResourceSnapshot (sys_resource_meter.hpp), one per second or so, it says whether the program should draw LESS (fewer frames,
// no animation, no glow, the 3D views paused) so the radio keeps up on a slow machine, and what exactly "less" means as a struct the interface can apply:
//   uiFrameCapHz            60 normally, 30 reduced
//   waterfallRowsPerSecond  0 = as fast as the radio produces rows; 15 reduced
//   animations              panel slides, fades, meter ballistics: on / off
//   glowAndShadows          on / off
//   threeDViewsLive         the globe / sonde / radar 3D views keep redrawing: on / off (a paused view keeps its last frame)
//   backgroundMapRedraw     map layers redraw while the window is idle: on / off
// These are the policy's OUTPUT NAMES; which of them the Slint window can honour today is the wiring hand-off's question (each is a property the .slint side would bind;
// none is a forbidden-file edit made here).
//
// HYSTERESIS, so the picture never flaps: the policy ENTERS reduced mode only after enterSamples consecutive samples in which any available reading is at or above its high
// threshold, and LEAVES it only after exitSamples consecutive samples in which every available reading is at or below its low threshold and at least one reading is available;
// a sample with no usable reading changes nothing and breaks neither streak. The gap between the high and low thresholds is the dead band. The user's own choice wins:
// Mode::On and Mode::Off ignore the readings. startLowEnd() looks once at the static facts (logical CPUs, total memory) so a machine that is plainly small starts reduced.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_SYS_EFFECTS_POLICY_HPP
#define CASCADE_CORE_SYS_EFFECTS_POLICY_HPP

#include "core/sys_resource_meter.hpp"

namespace cascade::core::sys {

struct EffectsSwitches {
    bool reduced = false;
    int uiFrameCapHz = 60;
    int waterfallRowsPerSecond = 0;
    bool animations = true;
    bool glowAndShadows = true;
    bool threeDViewsLive = true;
    bool backgroundMapRedraw = true;
    bool operator==(const EffectsSwitches&) const = default;
};

inline EffectsSwitches switchesFor(bool reduced) {
    EffectsSwitches s;
    s.reduced = reduced;
    if (reduced) {
        s.uiFrameCapHz = 30;
        s.waterfallRowsPerSecond = 15;
        s.animations = false;
        s.glowAndShadows = false;
        s.threeDViewsLive = false;
        s.backgroundMapRedraw = false;
    }
    return s;
}

struct EffectsThresholds {
    double cpuHigh = 85, cpuLow = 60;      // percent of the whole machine, this process
    double memHigh = 92, memLow = 85;      // percent of physical memory in use, system-wide
    double gpuHigh = 90, gpuLow = 70;      // percent, busiest engine, system-wide
    int enterSamples = 5;
    int exitSamples = 30;
};

enum class EffectsMode { Auto, On, Off };

class EffectsPolicy {
public:
    explicit EffectsPolicy(EffectsThresholds t = {}) : t_(t) {}
    void setMode(EffectsMode m) { mode_ = m; highRun_ = lowRun_ = 0; }
    EffectsMode mode() const { return mode_; }
    // Starts reduced for a plainly small machine (<= 2 logical CPUs or <= 4 GiB). Only when the facts are available; never later flips back by itself without calm readings.
    void startLowEnd(const ResourceSnapshot& facts) {
        const bool fewCpus = facts.logicalCpus.available && facts.logicalCpus.value <= 2;
        const bool smallRam = facts.systemMemoryTotalBytes.available && facts.systemMemoryTotalBytes.value <= (4ull << 30);
        if (fewCpus || smallRam) { reduced_ = true; }
    }
    const EffectsSwitches& update(const ResourceSnapshot& s) {
        if (mode_ == EffectsMode::On) { current_ = switchesFor(true); return current_; }
        if (mode_ == EffectsMode::Off) { current_ = switchesFor(false); return current_; }
        bool anyAvailable = false, anyHigh = false, allLow = true;
        auto look = [&](const Reading<double>& r, double hi, double lo) {
            if (!r.available) { return; }
            anyAvailable = true;
            if (r.value >= hi) { anyHigh = true; }
            if (r.value > lo) { allLow = false; }
        };
        look(s.processCpuPercent, t_.cpuHigh, t_.cpuLow);
        look(s.systemMemoryPercent, t_.memHigh, t_.memLow);
        look(s.gpuPercent, t_.gpuHigh, t_.gpuLow);
        if (anyAvailable) {
            if (anyHigh) { ++highRun_; lowRun_ = 0; }
            else if (allLow) { ++lowRun_; highRun_ = 0; }
            else { highRun_ = lowRun_ = 0; }   // in the dead band: neither streak survives
            if (!reduced_ && highRun_ >= t_.enterSamples) { reduced_ = true; highRun_ = 0; }
            if (reduced_ && lowRun_ >= t_.exitSamples) { reduced_ = false; lowRun_ = 0; }
        }
        current_ = switchesFor(reduced_);
        return current_;
    }
    const EffectsSwitches& current() const { return current_; }

private:
    EffectsThresholds t_;
    EffectsMode mode_ = EffectsMode::Auto;
    bool reduced_ = false;
    int highRun_ = 0, lowRun_ = 0;
    EffectsSwitches current_ = switchesFor(false);
};

}  // namespace cascade::core::sys

#endif  // CASCADE_CORE_SYS_EFFECTS_POLICY_HPP
