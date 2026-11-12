// sys_resource_meter.hpp - a live readout of how hard the machine is working: process CPU, process memory, system memory, GPU engine utilisation.
//
// EVERY READING CARRIES ITS OWN "AVAILABLE" FLAG, and a number that could not be read is marked unavailable, never guessed, never left at a plausible 0: the first CPU
// sample (a rate needs two points), a wall-clock gap too short to divide by, a counter that went backwards, a GPU counter set that does not exist on this machine (no WDDM
// driver, an older Windows) are each "not available". What is already in the program and what this adds: process_resources.cpp reads GDI/USER/handle counts, private bytes and
// DXGI video memory for one log line every ten minutes; engine_measure.cpp reads the process CPU and working set inside a bounded measurement run. Neither is a live meter
// and neither reads the system's memory load or any GPU engine; this one does, and takes its inputs from a table of functions so the tests supply every number.
//
// HOW EACH IS READ (Windows): process CPU = GetProcessTimes (kernel + user) between two samples, divided by the wall time and the logical processor count, so 100 means the
// whole machine; working set = GetProcessMemoryInfo; system memory = GlobalMemoryStatusEx (load percent, total and available physical bytes); GPU = the performance counter
// set "GPU Engine" (counter "Utilization Percentage", every instance, via pdh.dll loaded BY NAME with English counter names so a localised Windows works the same, and
// never linked), instances named pid_<pid>_luid_..._phys_<n>_eng_<m>_engtype_<type>: the readings of one engine type are summed (that is how Task Manager shows an engine)
// and the busiest type is the figure. Both a system-wide figure and this process's own are given. Reading a performance counter opens no device and shows no prompt.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_SYS_RESOURCE_METER_HPP
#define CASCADE_CORE_SYS_RESOURCE_METER_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cascade::core::sys {

template <class T>
struct Reading {
    bool available = false;
    T value{};
    static Reading of(T v) { Reading r; r.available = true; r.value = v; return r; }
};

struct ResourceSnapshot {
    Reading<double> processCpuPercent;            // of the whole machine, 0..100
    Reading<std::uint64_t> workingSetBytes;       // this process
    Reading<double> systemMemoryPercent;          // physical memory in use, 0..100
    Reading<std::uint64_t> systemMemoryTotalBytes;
    Reading<std::uint64_t> systemMemoryAvailableBytes;
    Reading<double> gpuPercent;                   // busiest engine type, all processes, 0..100
    Reading<double> gpuProcessPercent;            // the same, this process only
    Reading<unsigned> logicalCpus;
};

struct GpuInstance {
    std::string name;   // the counter instance name
    double value = 0;   // percent
};

struct SystemMemory {
    double loadPercent = 0;
    std::uint64_t totalBytes = 0;
    std::uint64_t availableBytes = 0;
};

// The inputs, each returning false when it cannot be read.
struct ResourceSources {
    std::function<bool(std::uint64_t& cpuTime100ns, unsigned& logicalCpus)> processCpu;   // cumulative kernel+user time of this process
    std::function<bool(std::uint64_t& bytes)> workingSet;
    std::function<bool(SystemMemory&)> systemMemory;
    std::function<bool(std::vector<GpuInstance>&)> gpuInstances;                         // false = no GPU counters here, or not yet readable
    std::function<std::int64_t()> nowMs;                                                 // steady milliseconds
    std::function<std::uint32_t()> processId;                                            // for the process-only GPU figure
};

// The real sources for this machine (Windows; elsewhere every function reports "cannot read").
ResourceSources realResourceSources();

// ---- the pure parts ----

// CPU percent of the whole machine from two cumulative-time readings; not available when the wall gap is under minWallMs, the counter went back, there is no CPU count,
// or the figure is above 105 (a counter that cannot be right); a figure between 100 and 105 is clamped to 100.
Reading<double> cpuPercent(std::uint64_t prev100ns, std::uint64_t now100ns, std::int64_t wallMs, unsigned logicalCpus, std::int64_t minWallMs = 50);

struct GpuInstanceName {
    bool valid = false;
    std::uint32_t pid = 0;
    std::string engineType;   // lower case, e.g. "3d", "videodecode", "copy"
};
GpuInstanceName parseGpuInstanceName(const std::string& name);

// Busiest engine type (sum per type), over all instances or only those of `onlyPid` (0 = all). Not available when no instance matches. Clamped to 100.
Reading<double> gpuBusiest(const std::vector<GpuInstance>& instances, std::uint32_t onlyPid = 0);

// The meter: sample() reads every source once. The first call has no CPU or GPU rate yet (unavailable) - that is the truth, not a zero.
class ResourceMeter {
public:
    explicit ResourceMeter(ResourceSources src) : src_(std::move(src)) {}
    ResourceSnapshot sample();

private:
    ResourceSources src_;
    bool havePrev_ = false;
    std::uint64_t prevCpu_ = 0;
    std::int64_t prevMs_ = 0;
};

}  // namespace cascade::core::sys

#endif  // CASCADE_CORE_SYS_RESOURCE_METER_HPP
