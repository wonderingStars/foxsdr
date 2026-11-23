// machine_state.cpp - see machine_state.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/machine_state.hpp"

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "core/diag_log.hpp"
#include "core/diag_report.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cascade::core {

namespace {

constexpr std::size_t kMachineLineBudget = 172;   // DiagLog::kLineBytes (192) - NUL - the 18-character stamp - one spare
std::size_t g_testBudget = 0;                      // test seam (setMachineLineBudgetForTest): 0 = kMachineLineBudget
bool g_testSampleOn = false;                       // test seam (setMachineSampleForTest): a made-up clock, memory load and commit ratio
std::string g_testClock;
int g_testLoad = 0, g_testCommitPct = 0;
std::atomic<int> g_display{-1};
std::string g_lastLogged;      // the pass thread's (logMachineStateIfChanged)
std::string g_lastClockKey;    // the clock and the memory load as last said (the second line); "" = not said yet

std::string firstWord(const std::string& outcome) {
    std::string w;
    for (const char c : outcome) {
        if (!((c >= 'a' && c <= 'z') || c == '-')) { break; }
        if (w.size() >= 12) { break; }
        w += c;
    }
    return w.empty() ? std::string("unknown") : w;
}

// "visible", "minimised", "hidden" or "unknown", from the line the frame loop's published state renders.
const char* windowWord() {
    char line[256] = {};
    renderWindowStateLine(line, sizeof(line));
    const std::string s(line);
    if (s.find("window-state: unknown") != std::string::npos) { return "unknown"; }
    if (s.find("window-state: hidden") != std::string::npos) { return "hidden"; }
    if (s.find(", minimised") != std::string::npos) { return "minimised"; }
    if (s.find("window-state: visible") != std::string::npos) { return "visible"; }
    return "unknown";
}

// THE PROCESSOR'S CLOCK NOW AND AT ITS MAXIMUM, in MHz (2026-11-23, the ringdrop measurement): the direct test of "a core held at its lowest clock
// (battery saver, a thermal or power limit)", the leading candidate for a DSP thread that runs below real time. CallNtPowerInformation
// (ProcessorInformation) from powrprof.dll, resolved at run time (no new import), read-only. "now" is the highest current clock of any logical
// processor (what the thread could be given), "max" the highest maximum. Writes "N/M" or "unknown"; Windows only.
std::string clockText() {
    if (g_testSampleOn) { return g_testClock; }
    char out[96];
    const std::size_t cap = sizeof(out);
    std::snprintf(out, cap, "unknown");
#if defined(_WIN32)
    using CallNtPowerInformationFn = LONG(WINAPI*)(int, PVOID, ULONG, PVOID, ULONG);
    struct ProcessorPowerInformation {   // PROCESSOR_POWER_INFORMATION (winnt.h), spelled out so no extra header is needed
        ULONG Number, MaxMhz, CurrentMhz, MhzLimit, MaxIdleState, CurrentIdleState;
    };
    constexpr int kProcessorInformation = 11;   // POWER_INFORMATION_LEVEL::ProcessorInformation
    // LOADED ONCE AND KEPT for the life of the process (a function-local static: thread-safe, never freed): one load, not one load and free per call.
    // From the Windows system folder ONLY (never the application's folder or the search path): a DLL planted beside the program must not be loaded.
    static const HMODULE lib = ::LoadLibraryExW(L"powrprof.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (lib == nullptr) { return out; }
    static const auto fn = reinterpret_cast<CallNtPowerInformationFn>(reinterpret_cast<void*>(::GetProcAddress(lib, "CallNtPowerInformation")));
    const DWORD active = ::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (fn != nullptr && active != 0 && active <= 1024) {
        std::vector<ProcessorPowerInformation> info(active);
        if (fn(kProcessorInformation, nullptr, 0, info.data(), static_cast<ULONG>(info.size() * sizeof(ProcessorPowerInformation))) >= 0) {
            unsigned long now = 0, max = 0;
            for (const ProcessorPowerInformation& p : info) {
                if (p.CurrentMhz > now) { now = p.CurrentMhz; }
                if (p.MaxMhz > max) { max = p.MaxMhz; }
            }
            if (now != 0 && max != 0) { std::snprintf(out, cap, "%lu/%lu", now, max); }
        }
    }
#endif
    return out;
}

// THE MEMORY (2026-11-23, a field session whose whole process was barely being run): the system's memory load in percent, the physical memory still
// available in MB, and the commit charge against the commit limit in MB - GlobalMemoryStatusEx, read-only. "unknown" for each on a failed query.
struct MemNow {
    bool ok = false;
    unsigned long load = 0;
    unsigned long long availMb = 0, usedMb = 0, limitMb = 0;
};

MemNow memoryNow() {
    MemNow m;
    if (g_testSampleOn) {
        m.ok = true;
        m.load = static_cast<unsigned long>(g_testLoad);
        m.availMb = 1000;
        m.limitMb = 1000;
        m.usedMb = static_cast<unsigned long long>(g_testCommitPct) * 10ull;
        return m;
    }
#if defined(_WIN32)
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (::GlobalMemoryStatusEx(&ms) != 0) {
        const unsigned long long mb = 1024ull * 1024ull;
        m.ok = true;
        m.load = static_cast<unsigned long>(ms.dwMemoryLoad);
        m.availMb = ms.ullAvailPhys / mb;
        m.usedMb = ms.ullTotalPageFile >= ms.ullAvailPageFile ? (ms.ullTotalPageFile - ms.ullAvailPageFile) / mb : 0ull;
        m.limitMb = ms.ullTotalPageFile / mb;
    }
#endif
    return m;
}

std::string memoryWords(const MemNow& m) {
    char out[96];
    if (!m.ok) { return "mem_load=unknown mem_avail_mb=unknown commit_mb=unknown"; }
    std::snprintf(out, sizeof(out), "mem_load=%lu mem_avail_mb=%llu commit_mb=%llu/%llu", m.load, m.availMb, m.usedMb, m.limitMb);
    return out;
}

}  // namespace

bool processMemoryNow(unsigned long long* pageFaults, unsigned long long* workingSetMb) {
#if defined(_WIN32)
    struct Counters {   // PROCESS_MEMORY_COUNTERS (psapi.h), spelled out; K32GetProcessMemoryInfo is in kernel32 (no new import)
        DWORD cb, PageFaultCount;
        SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage, QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage,
            PagefileUsage, PeakPagefileUsage;
    };
    using Fn = BOOL(WINAPI*)(HANDLE, Counters*, DWORD);
    const HMODULE k = ::GetModuleHandleW(L"kernel32.dll");
    const auto fn = k == nullptr ? nullptr : reinterpret_cast<Fn>(reinterpret_cast<void*>(::GetProcAddress(k, "K32GetProcessMemoryInfo")));
    Counters c{};
    c.cb = sizeof(c);
    if (fn != nullptr && fn(::GetCurrentProcess(), &c, sizeof(c)) != 0) {
        *pageFaults = c.PageFaultCount;
        *workingSetMb = static_cast<unsigned long long>(c.WorkingSetSize) / (1024ull * 1024ull);
        return true;
    }
#else
    (void)pageFaults;
    (void)workingSetMb;
#endif
    return false;
}

int systemMemoryLoadPercent() {
#if defined(_WIN32)
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (::GlobalMemoryStatusEx(&ms) != 0) { return static_cast<int>(ms.dwMemoryLoad); }
#endif
    return -1;
}

void noteDisplayState(int state) { g_display.store(state, std::memory_order_relaxed); }

namespace {
std::size_t lineBudget() { return g_testBudget != 0 ? g_testBudget : kMachineLineBudget; }
}  // namespace

// The text with the clock and memory words the caller already read (logMachineStateIfChanged reads them once per call and uses them three times).
static std::string machineStateTextWith(const std::string& dspPrio, const std::string& srcPrio, const std::string& clock, const std::string& memWords) {
    const char* power = "unknown";
    const char* saver = "unknown";
    char throttle[48] = "unknown";
    char timer[24] = "unknown";
    unsigned cpus = std::thread::hardware_concurrency();
    unsigned affinity = 0;   // 0 = unknown
#if defined(_WIN32)
    SYSTEM_POWER_STATUS sps{};
    if (::GetSystemPowerStatus(&sps) != 0) {
        power = sps.ACLineStatus == 1 ? "ac" : (sps.ACLineStatus == 0 ? "battery" : "unknown");
        saver = (sps.SystemStatusFlag & 1u) != 0 ? "on" : "off";
    }
    // The process's own power-throttling request (efficiency mode): the two masks, as Windows reports them. Not available on a
    // Windows that predates the query, and then it says so.
    PROCESS_POWER_THROTTLING_STATE pt{};
    pt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    if (::GetProcessInformation(::GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof(pt)) != 0) {
        std::snprintf(throttle, sizeof(throttle), "ctl:%lu,state:%lu", static_cast<unsigned long>(pt.ControlMask),
                      static_cast<unsigned long>(pt.StateMask));
    }
    // The timer's effective resolution in 100 ns units (NtQueryTimerResolution, resolved from ntdll at run time: no new import).
    using NtQueryTimerResolutionFn = LONG(NTAPI*)(PULONG, PULONG, PULONG);
    if (HMODULE nt = ::GetModuleHandleW(L"ntdll.dll")) {
        const auto fn = reinterpret_cast<NtQueryTimerResolutionFn>(
            reinterpret_cast<void*>(::GetProcAddress(nt, "NtQueryTimerResolution")));
        ULONG coarsest = 0, finest = 0, current = 0;
        if (fn != nullptr && fn(&coarsest, &finest, &current) >= 0 && current != 0) {
            std::snprintf(timer, sizeof(timer), "%lu", static_cast<unsigned long>(current));
        }
    }
    const DWORD active = ::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (active != 0) { cpus = static_cast<unsigned>(active); }
    DWORD_PTR procMask = 0, sysMask = 0;
    if (::GetProcessAffinityMask(::GetCurrentProcess(), &procMask, &sysMask) != 0) {
        for (DWORD_PTR m = procMask; m != 0; m &= m - 1) { ++affinity; }
    }
#endif
    const int d = g_display.load(std::memory_order_relaxed);
    const char* display = d == 0 ? "on" : (d == 1 ? "off" : (d == 2 ? "dimmed" : "unknown"));
    char aff[16] = "unknown";
    if (affinity != 0) { std::snprintf(aff, sizeof(aff), "%u", affinity); }
    char line[256];
    std::snprintf(line, sizeof(line),
                  "machine: power=%s saver=%s throttle=%s timer_100ns=%s cpus=%u affinity=%s window=%s display=%s prio=dsp:%s,src:%s",
                  power, saver, throttle, timer, cpus, aff, windowWord(), display, firstWord(dspPrio).c_str(),
                  firstWord(srcPrio).c_str());
    // THE CLOCK, last: "clock=<now>/<max>" (MHz) or "clock=unknown" - not "mhz=", whose letters make the upload scrub read the line as naming a
    // frequency and mask its numbers. It is left out when the line would not fit the log's line with its stamp (a cut line is masked whole by the
    // upload scrub): kMachineLineBudget characters, the same 172 the DSP minute's lines are held to; logMachineStateIfChanged then says it in a
    // line of its own.
    std::string text(line);
    const std::string clockWord = std::string(" clock=") + clock + " " + memWords;
    if (text.size() + clockWord.size() <= lineBudget()) { text += clockWord; }
    return text;
}

std::string machineStateText(const std::string& dspPrio, const std::string& srcPrio) {
    return machineStateTextWith(dspPrio, srcPrio, clockText(), memoryWords(memoryNow()));
}

// WHAT "CHANGED" MEANS FOR THE CLOCK AND MEMORY WORDS: the processor clock pair as Windows reports it (NOW/MAX), the system memory load in steps of
// ten percent, and the commit charge against the commit limit in steps of ten percent. The available-memory and commit-megabyte figures move every time
// they are read and would make every call "changed", so they are not part of it.
static std::string clockMemoryKey(const std::string& clock, const MemNow& m) {
    const int load = m.ok ? static_cast<int>(m.load) / 10 : -1;
    const int commit = (m.ok && m.limitMb != 0) ? static_cast<int>(m.usedMb * 100ull / m.limitMb) / 10 : -1;
    return clock + "|" + std::to_string(load) + "|" + std::to_string(commit);
}

bool logMachineStateIfChanged(const std::string& dspPrio, const std::string& srcPrio, bool force) {
    // The clock and the memory are read ONCE per call and used for the line, the second line and the change key.
    const std::string clock = clockText();
    const MemNow mem = memoryNow();
    const std::string memWords = memoryWords(mem);
    const std::string text = machineStateTextWith(dspPrio, srcPrio, clock, memWords);
    const bool clockInLine = text.find(" clock=") != std::string::npos;
    const std::string clockLine = std::string("machine:") + " clock=" + clock + " " + memWords;
    // CHANGE IS JUDGED ON THE STEADY WORDS ONLY for the first line: the clock and the memory figures move every time they are read.
    const std::size_t volatileAt = text.find(" clock=");
    const std::string key = volatileAt == std::string::npos ? text : text.substr(0, volatileAt);
    // THE CLOCK AND THE MEMORY are said when a value in their key CHANGED since it was last said (and at the first call, nothing said yet), not forced
    // into every loss minute: the diagnostics ring holds 256 lines, and a loss minute already writes ten.
    const std::string clockKey = clockMemoryKey(clock, mem);
    const bool steadyChanged = force || key != g_lastLogged;
    const bool clockChanged = clockKey != g_lastClockKey;
    if (clockInLine) {
        // the clock words fit in the line: one line carries both, said when either part changed
        if (!steadyChanged && !clockChanged) { return false; }
        g_lastLogged = key;
        g_lastClockKey = clockKey;
        diagLogf("%s", text.c_str());
        return true;
    }
    bool said = false;
    if (steadyChanged) {
        g_lastLogged = key;
        diagLogf("%s", text.c_str());
        said = true;
    }
    // THE SECOND LINE, the clock and the memory
    if (clockChanged) {
        g_lastClockKey = clockKey;
        diagLogf("%s", clockLine.c_str());
        said = true;
    }
    return said;
}

void resetMachineStateLogForTest() {
    g_lastLogged.clear();
    g_lastClockKey.clear();
}

void setMachineSampleForTest(const char* clock, int memLoadPercent, int commitPercent) {
    g_testSampleOn = clock != nullptr;
    g_testClock = clock != nullptr ? clock : "";
    g_testLoad = memLoadPercent;
    g_testCommitPct = commitPercent;
}

void setMachineLineBudgetForTest(std::size_t chars) { g_testBudget = chars; }

}  // namespace cascade::core
