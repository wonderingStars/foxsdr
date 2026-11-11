// sys_resource_meter.cpp - see sys_resource_meter.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/sys_resource_meter.hpp"

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#endif

namespace cascade::core::sys {

Reading<double> cpuPercent(std::uint64_t prev100ns, std::uint64_t now100ns, std::int64_t wallMs, unsigned logicalCpus, std::int64_t minWallMs) {
    if (wallMs < minWallMs || wallMs <= 0 || logicalCpus == 0 || now100ns < prev100ns) { return {}; }
    const double busyMs = static_cast<double>(now100ns - prev100ns) / 10000.0;   // 100 ns units -> ms
    const double pct = 100.0 * busyMs / (static_cast<double>(wallMs) * static_cast<double>(logicalCpus));
    if (pct > 105.0) { return {}; }
    return Reading<double>::of(std::min(pct, 100.0));
}

GpuInstanceName parseGpuInstanceName(const std::string& nameIn) {
    GpuInstanceName r;
    std::string n = nameIn;
    for (auto& c : n) { if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); } }
    if (n.rfind("pid_", 0) != 0) { return r; }
    std::size_t i = 4;
    std::uint64_t pid = 0;
    std::size_t digits = 0;
    while (i < n.size() && n[i] >= '0' && n[i] <= '9' && digits < 10) { pid = pid * 10 + static_cast<std::uint64_t>(n[i] - '0'); ++i; ++digits; }
    if (digits == 0 || pid > 0xFFFFFFFFull || i >= n.size() || n[i] != '_') { return r; }
    const std::string key = "_engtype_";
    const std::size_t at = n.rfind(key);
    if (at == std::string::npos || at + key.size() >= n.size()) { return r; }
    r.engineType = n.substr(at + key.size());
    r.pid = static_cast<std::uint32_t>(pid);
    r.valid = true;
    return r;
}

Reading<double> gpuBusiest(const std::vector<GpuInstance>& instances, std::uint32_t onlyPid) {
    std::map<std::string, double> perType;
    for (const auto& inst : instances) {
        const GpuInstanceName p = parseGpuInstanceName(inst.name);
        if (!p.valid) { continue; }
        if (onlyPid != 0 && p.pid != onlyPid) { continue; }
        if (!(inst.value >= 0.0) || inst.value > 1.0e6) { continue; }   // NaN, negative or absurd readings are dropped, not summed
        perType[p.engineType] += inst.value;
    }
    if (perType.empty()) { return {}; }
    double best = 0;
    for (const auto& kv : perType) { best = std::max(best, kv.second); }
    return Reading<double>::of(std::min(best, 100.0));
}

ResourceSnapshot ResourceMeter::sample() {
    ResourceSnapshot s;
    const std::int64_t now = src_.nowMs ? src_.nowMs() : 0;

    std::uint64_t cpu = 0;
    unsigned cpus = 0;
    if (src_.processCpu && src_.processCpu(cpu, cpus)) {
        if (cpus > 0) { s.logicalCpus = Reading<unsigned>::of(cpus); }
        if (havePrev_) { s.processCpuPercent = cpuPercent(prevCpu_, cpu, now - prevMs_, cpus); }
        // A sample whose gap was too short to use keeps the OLD reference point, so a caller polling quickly still gets a figure once enough time has passed.
        if (!havePrev_ || now - prevMs_ >= 50) {
            havePrev_ = true;
            prevCpu_ = cpu;
            prevMs_ = now;
        }
    }
    std::uint64_t ws = 0;
    if (src_.workingSet && src_.workingSet(ws)) { s.workingSetBytes = Reading<std::uint64_t>::of(ws); }
    SystemMemory sm;
    if (src_.systemMemory && src_.systemMemory(sm) && sm.totalBytes > 0 && sm.loadPercent >= 0.0 && sm.loadPercent <= 100.0 && sm.availableBytes <= sm.totalBytes) {
        s.systemMemoryPercent = Reading<double>::of(sm.loadPercent);
        s.systemMemoryTotalBytes = Reading<std::uint64_t>::of(sm.totalBytes);
        s.systemMemoryAvailableBytes = Reading<std::uint64_t>::of(sm.availableBytes);
    }
    std::vector<GpuInstance> gpu;
    if (src_.gpuInstances && src_.gpuInstances(gpu)) {
        s.gpuPercent = gpuBusiest(gpu, 0);
        const std::uint32_t pid = src_.processId ? src_.processId() : 0;
        if (pid != 0) {
            s.gpuProcessPercent = gpuBusiest(gpu, pid);
            // A GPU that is present but where this process has no engine instance is "0 busy" for it only when other instances exist at all.
            if (!s.gpuProcessPercent.available && s.gpuPercent.available) { s.gpuProcessPercent = Reading<double>::of(0.0); }
        }
    }
    return s;
}

#ifdef _WIN32
namespace {

std::uint64_t fileTimeTo100ns(const FILETIME& ft) { return (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime; }

using PdhOpenQueryW_t = PDH_STATUS(WINAPI*)(LPCWSTR, DWORD_PTR, PDH_HQUERY*);
using PdhAddEnglishCounterW_t = PDH_STATUS(WINAPI*)(PDH_HQUERY, LPCWSTR, DWORD_PTR, PDH_HCOUNTER*);
using PdhCollectQueryData_t = PDH_STATUS(WINAPI*)(PDH_HQUERY);
using PdhGetFormattedCounterArrayW_t = PDH_STATUS(WINAPI*)(PDH_HCOUNTER, DWORD, LPDWORD, LPDWORD, PDH_FMT_COUNTERVALUE_ITEM_W*);
using PdhCloseQuery_t = PDH_STATUS(WINAPI*)(PDH_HQUERY);

// The GPU Engine counter, owned by the sources the meter holds. pdh.dll is loaded from System32 by name; nothing is linked.
class GpuCounter {
public:
    ~GpuCounter() {
        if (close_ && query_ != nullptr) { close_(query_); }
        if (module_ != nullptr) { ::FreeLibrary(module_); }
    }
    bool read(std::vector<GpuInstance>& out) {
        std::lock_guard<std::mutex> lk(mu_);
        out.clear();
        if (failed_) { return false; }
        if (query_ == nullptr && !open()) { failed_ = true; return false; }
        if (collect_(query_) != ERROR_SUCCESS) { return false; }
        if (!primed_) { primed_ = true; return false; }   // a rate counter has no value until the second collection
        DWORD bytes = 0, count = 0;
        PDH_STATUS st = format_(counter_, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, nullptr);
        if (st != PDH_MORE_DATA || bytes == 0) { return false; }
        std::vector<char> buf(bytes);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
        st = format_(counter_, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, items);
        if (st != ERROR_SUCCESS) { return false; }
        for (DWORD i = 0; i < count; ++i) {
            if (items[i].FmtValue.CStatus != ERROR_SUCCESS && items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA && items[i].FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) { continue; }
            GpuInstance g;
            for (const wchar_t* p = items[i].szName; p != nullptr && *p != L'\0' && g.name.size() < 256; ++p) { g.name.push_back(*p < 128 ? static_cast<char>(*p) : '?'); }
            g.value = items[i].FmtValue.doubleValue;
            out.push_back(std::move(g));
        }
        return !out.empty();
    }

private:
    bool open() {
        module_ = ::LoadLibraryExW(L"pdh.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module_ == nullptr) { return false; }
        auto openQ = reinterpret_cast<PdhOpenQueryW_t>(::GetProcAddress(module_, "PdhOpenQueryW"));
        auto addC = reinterpret_cast<PdhAddEnglishCounterW_t>(::GetProcAddress(module_, "PdhAddEnglishCounterW"));
        collect_ = reinterpret_cast<PdhCollectQueryData_t>(::GetProcAddress(module_, "PdhCollectQueryData"));
        format_ = reinterpret_cast<PdhGetFormattedCounterArrayW_t>(::GetProcAddress(module_, "PdhGetFormattedCounterArrayW"));
        close_ = reinterpret_cast<PdhCloseQuery_t>(::GetProcAddress(module_, "PdhCloseQuery"));
        if (!openQ || !addC || !collect_ || !format_ || !close_) { return false; }
        if (openQ(nullptr, 0, &query_) != ERROR_SUCCESS) { query_ = nullptr; return false; }
        if (addC(query_, L"\\GPU Engine(*)\\Utilization Percentage", 0, &counter_) != ERROR_SUCCESS) {
            close_(query_);
            query_ = nullptr;
            return false;   // no GPU Engine counter set on this machine: every GPU reading stays unavailable
        }
        return true;
    }
    std::mutex mu_;
    HMODULE module_ = nullptr;
    PDH_HQUERY query_ = nullptr;
    PDH_HCOUNTER counter_ = nullptr;
    PdhCollectQueryData_t collect_ = nullptr;
    PdhGetFormattedCounterArrayW_t format_ = nullptr;
    PdhCloseQuery_t close_ = nullptr;
    bool primed_ = false;
    bool failed_ = false;
};

}  // namespace
#endif

ResourceSources realResourceSources() {
    ResourceSources s;
#ifdef _WIN32
    s.processCpu = [](std::uint64_t& t, unsigned& cpus) {
        FILETIME create, exit, kernel, user;
        if (!::GetProcessTimes(::GetCurrentProcess(), &create, &exit, &kernel, &user)) { return false; }
        t = fileTimeTo100ns(kernel) + fileTimeTo100ns(user);
        SYSTEM_INFO si;
        ::GetSystemInfo(&si);
        cpus = si.dwNumberOfProcessors;
        return cpus > 0;
    };
    s.workingSet = [](std::uint64_t& bytes) {
        PROCESS_MEMORY_COUNTERS pmc{};
        pmc.cb = sizeof(pmc);
        if (!::GetProcessMemoryInfo(::GetCurrentProcess(), &pmc, sizeof(pmc))) { return false; }
        bytes = pmc.WorkingSetSize;
        return true;
    };
    s.systemMemory = [](SystemMemory& m) {
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof(ms);
        if (!::GlobalMemoryStatusEx(&ms)) { return false; }
        m.loadPercent = static_cast<double>(ms.dwMemoryLoad);
        m.totalBytes = ms.ullTotalPhys;
        m.availableBytes = ms.ullAvailPhys;
        return true;
    };
    auto gpu = std::make_shared<GpuCounter>();
    s.gpuInstances = [gpu](std::vector<GpuInstance>& out) { return gpu->read(out); };
    s.processId = [] { return static_cast<std::uint32_t>(::GetCurrentProcessId()); };
#endif
    s.nowMs = [] {
        return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    };
    return s;
}

}  // namespace cascade::core::sys
