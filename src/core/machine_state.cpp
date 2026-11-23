// machine_state.cpp - see machine_state.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/machine_state.hpp"

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

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

std::atomic<int> g_display{-1};
std::string g_lastLogged;   // the pass thread's (logMachineStateIfChanged)

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

}  // namespace

void noteDisplayState(int state) { g_display.store(state, std::memory_order_relaxed); }

std::string machineStateText(const std::string& dspPrio, const std::string& srcPrio) {
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
    return line;
}

bool logMachineStateIfChanged(const std::string& dspPrio, const std::string& srcPrio, bool force) {
    const std::string text = machineStateText(dspPrio, srcPrio);
    if (!force && text == g_lastLogged) { return false; }
    g_lastLogged = text;
    diagLogf("%s", text.c_str());
    return true;
}

void resetMachineStateLogForTest() { g_lastLogged.clear(); }

}  // namespace cascade::core
