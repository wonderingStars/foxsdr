// Tests for core/sys_resource_meter.hpp and core/sys_effects_policy.hpp (lane sysmodels): the live resource readout and the reduced-effects policy.
// The pure arithmetic and the meter are driven by supplied numbers; ONE test reads this machine through the real sources (process CPU, working set, system memory,
// the GPU Engine counter if present) and checks only that what it reports is in range and marked available or not.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <thread>
#include <vector>

#include "core/sys_effects_policy.hpp"
#include "core/sys_resource_meter.hpp"
#include "test_check.hpp"

using namespace cascade::core::sys;

namespace {

void testCpuPercent() {
    auto r = cpuPercent(0, 5000000, 1000, 1);             // 500 ms of CPU in 1000 ms on one logical CPU
    CHECK(r.available && std::fabs(r.value - 50.0) < 1e-9);
    r = cpuPercent(1000, 1000 + 40000000, 2000, 4);       // 4000 ms in 2000 ms on 4 CPUs = half the machine
    CHECK(r.available && std::fabs(r.value - 50.0) < 1e-9);
    r = cpuPercent(7, 7, 1000, 8);                        // idle: a real zero, available
    CHECK(r.available && r.value == 0.0);
    CHECK(!cpuPercent(0, 5000000, 10, 1).available);      // gap shorter than the minimum: no figure
    r = cpuPercent(0, 250000, 50, 1);                      // exactly the minimum gap is allowed: 25 ms of CPU in 50 ms
    CHECK(r.available && std::fabs(r.value - 50.0) < 1e-9);
    CHECK(!cpuPercent(0, 5000000, 50, 1).available);        // ... and 500 ms of CPU in 50 ms on one CPU cannot be right
    CHECK(!cpuPercent(10, 5, 1000, 1).available);         // the counter went backwards
    CHECK(!cpuPercent(0, 5000000, 1000, 0).available);    // no CPU count
    CHECK(!cpuPercent(0, 5000000, 0, 1).available);
    CHECK(!cpuPercent(0, 5000000, -5, 1).available);
    CHECK(!cpuPercent(0, 20000000, 1000, 1).available);   // 200 percent of one CPU is a counter that cannot be right
    r = cpuPercent(0, 10200000, 1000, 1);                 // 102: rounding noise, clamped
    CHECK(r.available && r.value == 100.0);
}

void testGpuNames() {
    auto n = parseGpuInstanceName("pid_1234_luid_0x00000000_0x0000A1B2_phys_0_eng_3_engtype_3D");
    CHECK(n.valid && n.pid == 1234 && n.engineType == "3d");
    n = parseGpuInstanceName("PID_7_LUID_0X0_0X1_PHYS_0_ENG_0_ENGTYPE_VideoDecode");
    CHECK(n.valid && n.pid == 7 && n.engineType == "videodecode");
    CHECK(!parseGpuInstanceName("").valid);
    CHECK(!parseGpuInstanceName("luid_0x0_phys_0_eng_0_engtype_3D").valid);
    CHECK(!parseGpuInstanceName("pid__luid_0x0_engtype_3D").valid);
    CHECK(!parseGpuInstanceName("pid_99999999999_luid_0x0_engtype_3D").valid);   // not a 32-bit pid
    CHECK(!parseGpuInstanceName("pid_5_luid_0x0_phys_0_eng_0").valid);           // no engine type
    CHECK(!parseGpuInstanceName("pid_5_luid_0x0_phys_0_eng_0_engtype_").valid);  // empty engine type
}

void testGpuBusiest() {
    std::vector<GpuInstance> v = {
        {"pid_10_luid_0x0_0x1_phys_0_eng_0_engtype_3D", 30.0},
        {"pid_11_luid_0x0_0x1_phys_0_eng_0_engtype_3D", 20.0},
        {"pid_10_luid_0x0_0x1_phys_0_eng_1_engtype_Copy", 40.0},
        {"pid_12_luid_0x0_0x1_phys_0_eng_2_engtype_VideoDecode", 5.0},
    };
    auto r = gpuBusiest(v, 0);
    CHECK(r.available && std::fabs(r.value - 50.0) < 1e-9);          // 3D: 30 + 20; Copy 40; decode 5 -> 50
    r = gpuBusiest(v, 10);
    CHECK(r.available && std::fabs(r.value - 40.0) < 1e-9);          // pid 10: 3D 30, Copy 40
    r = gpuBusiest(v, 12);
    CHECK(r.available && std::fabs(r.value - 5.0) < 1e-9);
    CHECK(!gpuBusiest(v, 999).available);                             // nobody by that pid: not available (the meter decides what that means)
    CHECK(!gpuBusiest({}, 0).available);
    std::vector<GpuInstance> bad = {{"pid_1_luid_0x0_engtype_3D", std::numeric_limits<double>::quiet_NaN()},
                                    {"pid_1_luid_0x0_engtype_3D", -4.0},
                                    {"garbage", 80.0},
                                    {"pid_1_luid_0x0_engtype_3D", 1e9}};
    CHECK(!gpuBusiest(bad, 0).available);                             // NaN, negative, unparseable and absurd values are all dropped
    std::vector<GpuInstance> over = {{"pid_1_luid_0x0_engtype_3D", 80.0}, {"pid_2_luid_0x0_engtype_3D", 70.0}};
    r = gpuBusiest(over, 0);
    CHECK(r.available && r.value == 100.0);                           // clamped
}

struct Fake {
    std::int64_t now = 0;
    std::uint64_t cpu = 0;
    unsigned cpus = 4;
    bool cpuOk = true, wsOk = true, memOk = true, gpuOk = true;
    std::uint64_t ws = 123456789;
    SystemMemory mem{40.0, 16ull << 30, 9ull << 30};
    std::vector<GpuInstance> gpu;
    ResourceSources src() {
        ResourceSources s;
        s.nowMs = [this] { return now; };
        s.processCpu = [this](std::uint64_t& t, unsigned& c) { t = cpu; c = cpus; return cpuOk; };
        s.workingSet = [this](std::uint64_t& b) { b = ws; return wsOk; };
        s.systemMemory = [this](SystemMemory& m) { m = mem; return memOk; };
        s.gpuInstances = [this](std::vector<GpuInstance>& o) { o = gpu; return gpuOk; };
        s.processId = [] { return std::uint32_t(10); };
        return s;
    }
};

void testMeter() {
    Fake f;
    f.gpu = {{"pid_10_luid_0x0_engtype_3D", 12.0}, {"pid_11_luid_0x0_engtype_3D", 8.0}, {"pid_11_luid_0x0_engtype_Copy", 30.0}};
    ResourceMeter m(f.src());
    auto s = m.sample();
    CHECK(!s.processCpuPercent.available);                     // a rate needs two points: the first has none
    CHECK(s.workingSetBytes.available && s.workingSetBytes.value == 123456789);
    CHECK(s.systemMemoryPercent.available && s.systemMemoryPercent.value == 40.0);
    CHECK(s.systemMemoryTotalBytes.value == (16ull << 30) && s.systemMemoryAvailableBytes.value == (9ull << 30));
    CHECK(s.logicalCpus.available && s.logicalCpus.value == 4);
    CHECK(s.gpuPercent.available && std::fabs(s.gpuPercent.value - 30.0) < 1e-9);      // Copy 30 beats 3D 12 + 8
    CHECK(s.gpuProcessPercent.available && std::fabs(s.gpuProcessPercent.value - 12.0) < 1e-9);
    f.now = 1000;
    f.cpu = 20000000;   // 2000 ms of CPU in 1000 ms on 4 CPUs = 50
    s = m.sample();
    CHECK(s.processCpuPercent.available && std::fabs(s.processCpuPercent.value - 50.0) < 1e-9);
    // A sample too soon after the last keeps the old reference point: 10 ms later has no figure, and 1000 ms after the FIRST reference one appears.
    f.now = 1010;
    f.cpu = 20100000;
    s = m.sample();
    CHECK(!s.processCpuPercent.available);
    f.now = 2000;
    f.cpu = 30000000;   // 1000 ms of CPU since t = 1000 (cpu 20000000), 1000 ms on 4 CPUs = 25
    s = m.sample();
    CHECK(s.processCpuPercent.available && std::fabs(s.processCpuPercent.value - 25.0) < 1e-9);
    // Each source fails on its own without disturbing the others.
    f.memOk = false;
    f.gpuOk = false;
    f.wsOk = false;
    f.now = 3000;
    f.cpu = 40000000;
    s = m.sample();
    CHECK(!s.systemMemoryPercent.available && !s.systemMemoryTotalBytes.available && !s.gpuPercent.available && !s.gpuProcessPercent.available && !s.workingSetBytes.available);
    CHECK(s.processCpuPercent.available);
    // An impossible memory answer is unavailable, not repaired.
    f.memOk = true;
    f.mem.loadPercent = 150;
    s = m.sample();
    CHECK(!s.systemMemoryPercent.available);
    f.mem = SystemMemory{50.0, 0, 0};   // no total
    CHECK(!m.sample().systemMemoryPercent.available);
    f.mem = SystemMemory{50.0, 100, 200};   // available above total
    CHECK(!m.sample().systemMemoryPercent.available);
    // The GPU counters exist but this process has no engine instance: 0 busy for it, the system figure unaffected.
    f.gpuOk = true;
    f.gpu = {{"pid_11_luid_0x0_engtype_3D", 8.0}};
    s = m.sample();
    CHECK(s.gpuPercent.available && s.gpuProcessPercent.available && s.gpuProcessPercent.value == 0.0);
    // No source at all: everything unavailable, nothing crashes.
    ResourceMeter empty{ResourceSources{}};
    s = empty.sample();
    CHECK(!s.processCpuPercent.available && !s.workingSetBytes.available && !s.systemMemoryPercent.available && !s.gpuPercent.available && !s.logicalCpus.available);
}

ResourceSnapshot snap(double cpu, double mem, double gpu) {   // negative = not available
    ResourceSnapshot s;
    if (cpu >= 0) { s.processCpuPercent = Reading<double>::of(cpu); }
    if (mem >= 0) { s.systemMemoryPercent = Reading<double>::of(mem); }
    if (gpu >= 0) { s.gpuPercent = Reading<double>::of(gpu); }
    return s;
}

void testPolicy() {
    EffectsThresholds t;   // enter 5 high samples, leave 30 calm ones
    {   // sustained load enters after exactly enterSamples, and the switches change together
        EffectsPolicy p(t);
        for (int i = 0; i < 4; ++i) { CHECK(!p.update(snap(95, 40, 10)).reduced); }
        const auto on = p.update(snap(95, 40, 10));
        CHECK(on.reduced && on.uiFrameCapHz == 30 && on.waterfallRowsPerSecond == 15 && !on.animations && !on.glowAndShadows && !on.threeDViewsLive && !on.backgroundMapRedraw);
        CHECK(on == switchesFor(true));
        CHECK(switchesFor(false).uiFrameCapHz == 60 && switchesFor(false).animations);
    }
    {   // a spike that does not last never enters; a streak broken by one calm sample starts over
        EffectsPolicy p(t);
        for (int rep = 0; rep < 20; ++rep) {
            for (int i = 0; i < 4; ++i) { p.update(snap(99, 10, 10)); }
            CHECK(!p.update(snap(50, 10, 10)).reduced);
        }
    }
    {   // leaving takes exitSamples calm samples, and a sample in the dead band restarts the count
        EffectsPolicy p(t);
        for (int i = 0; i < 5; ++i) { p.update(snap(99, 10, 10)); }
        CHECK(p.current().reduced);
        for (int i = 0; i < 29; ++i) { CHECK(p.update(snap(30, 40, 10)).reduced); }
        CHECK(p.update(snap(70, 40, 10)).reduced);                  // 70 is between low (60) and high (85): the calm streak is broken
        for (int i = 0; i < 29; ++i) { CHECK(p.update(snap(30, 40, 10)).reduced); }
        CHECK(!p.update(snap(30, 40, 10)).reduced);                 // the 30th calm sample in a row
    }
    {   // oscillating around a threshold never flips
        EffectsPolicy p(t);
        int flips = 0;
        bool last = false;
        for (int i = 0; i < 400; ++i) {
            const bool r = p.update(snap(i % 2 ? 90 : 80, 10, 10)).reduced;
            if (r != last) { ++flips; last = r; }
        }
        CHECK(flips == 0);
    }
    {   // any one high reading is enough (memory alone, GPU alone); all three must be calm to leave
        EffectsPolicy a(t), b(t);
        for (int i = 0; i < 5; ++i) { a.update(snap(10, 95, 10)); b.update(snap(10, 10, 95)); }
        CHECK(a.current().reduced && b.current().reduced);
        for (int i = 0; i < 100; ++i) { a.update(snap(10, 10, 75)); }    // GPU 75 > low 70: not calm
        CHECK(a.current().reduced);
        for (int i = 0; i < 30; ++i) { a.update(snap(10, 10, 60)); }
        CHECK(!a.current().reduced);
    }
    {   // unavailable readings neither enter nor leave, and do not break a streak
        EffectsPolicy p(t);
        for (int i = 0; i < 3; ++i) { p.update(snap(99, -1, -1)); }
        for (int i = 0; i < 50; ++i) { p.update(snap(-1, -1, -1)); }
        CHECK(!p.current().reduced);
        p.update(snap(99, -1, -1));
        CHECK(!p.current().reduced);
        CHECK(p.update(snap(99, -1, -1)).reduced);                  // the fifth high sample, the unavailable ones in between did not count or reset
        for (int i = 0; i < 200; ++i) { p.update(snap(-1, -1, -1)); }
        CHECK(p.current().reduced);                                  // nothing known: it holds
    }
    {   // the user's choice wins both ways, and returning to Auto starts counting afresh
        EffectsPolicy p(t);
        p.setMode(EffectsMode::On);
        CHECK(p.update(snap(1, 1, 1)).reduced);
        p.setMode(EffectsMode::Off);
        for (int i = 0; i < 50; ++i) { CHECK(!p.update(snap(100, 100, 100)).reduced); }
        p.setMode(EffectsMode::Auto);
        for (int i = 0; i < 4; ++i) { CHECK(!p.update(snap(100, 100, 100)).reduced); }
        CHECK(p.update(snap(100, 100, 100)).reduced);
        CHECK(p.mode() == EffectsMode::Auto);
    }
    {   // a small machine starts reduced and stays so until it is calm for long enough
        EffectsPolicy p(t);
        ResourceSnapshot facts;
        facts.logicalCpus = Reading<unsigned>::of(2);
        p.startLowEnd(facts);
        CHECK(p.update(snap(-1, -1, -1)).reduced);
        EffectsPolicy q(t);
        ResourceSnapshot ram;
        ram.systemMemoryTotalBytes = Reading<std::uint64_t>::of(4ull << 30);
        q.startLowEnd(ram);
        CHECK(q.update(snap(-1, -1, -1)).reduced);
        EffectsPolicy big(t);
        ResourceSnapshot roomy;
        roomy.logicalCpus = Reading<unsigned>::of(16);
        roomy.systemMemoryTotalBytes = Reading<std::uint64_t>::of(32ull << 30);
        big.startLowEnd(roomy);
        CHECK(!big.update(snap(-1, -1, -1)).reduced);
        EffectsPolicy none(t);
        none.startLowEnd(ResourceSnapshot{});   // facts unavailable: not assumed small
        CHECK(!none.update(snap(-1, -1, -1)).reduced);
        for (int i = 0; i < 30; ++i) { p.update(snap(10, 10, 10)); }
        CHECK(!p.current().reduced);
    }
    {   // thresholds are the configured ones
        EffectsThresholds custom;
        custom.cpuHigh = 50; custom.cpuLow = 20; custom.enterSamples = 1; custom.exitSamples = 1;
        EffectsPolicy p(custom);
        CHECK(p.update(snap(50, 0, 0)).reduced);
        CHECK(!p.update(snap(20, 0, 0)).reduced);
    }
}

// THE REAL MACHINE: whatever it reports must be in range and consistently marked; the GPU counter set may or may not exist.
void testRealSources() {
#ifdef _WIN32
    ResourceMeter m(realResourceSources());
    auto first = m.sample();
    CHECK(first.logicalCpus.available && first.logicalCpus.value >= 1);
    CHECK(first.workingSetBytes.available && first.workingSetBytes.value > (1u << 20));
    CHECK(first.systemMemoryTotalBytes.available && first.systemMemoryTotalBytes.value >= (256ull << 20));
    CHECK(first.systemMemoryPercent.available && first.systemMemoryPercent.value >= 0 && first.systemMemoryPercent.value <= 100);
    CHECK(!first.processCpuPercent.available);   // the first sample has no rate
    // Burn some CPU so the second sample has something to measure.
    volatile double sink = 0;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    while (std::chrono::steady_clock::now() < end) { for (int i = 0; i < 1000; ++i) { sink = sink + std::sqrt(static_cast<double>(i)); } }
    auto second = m.sample();
    CHECK(second.processCpuPercent.available && second.processCpuPercent.value > 0.0 && second.processCpuPercent.value <= 100.0);
    CHECK(second.workingSetBytes.available);
    // The GPU counters need their own second collection; poll a little for them, and accept either outcome.
    bool gpuSeen = second.gpuPercent.available;
    for (int i = 0; i < 6 && !gpuSeen; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const auto s = m.sample();
        gpuSeen = s.gpuPercent.available;
        if (gpuSeen) { second = s; }
    }
    if (gpuSeen) { CHECK(second.gpuPercent.value >= 0.0 && second.gpuPercent.value <= 100.0); }
    std::printf("real sources: cpus %u, working set %.0f MB, memory %.0f%% of %.1f GB, process cpu %.1f%%, gpu %s",
                second.logicalCpus.value, static_cast<double>(second.workingSetBytes.value) / 1048576.0, second.systemMemoryPercent.value,
                static_cast<double>(second.systemMemoryTotalBytes.value) / 1073741824.0, second.processCpuPercent.value, gpuSeen ? "" : "UNAVAILABLE here\n");
    if (gpuSeen) { std::printf("%.1f%% (process %s)\n", second.gpuPercent.value, second.gpuProcessPercent.available ? "available" : "unavailable"); }
#else
    SKIP_LINUX("the real sources are Windows calls");
#endif
}

}  // namespace

int main() {
    testCpuPercent();
    testGpuNames();
    testGpuBusiest();
    testMeter();
    testPolicy();
    testRealSources();
    return testSummary("test_sys_resources");
}
