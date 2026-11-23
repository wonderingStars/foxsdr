// companion_fault.cpp - see companion_fault.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/companion_fault.hpp"

#include <algorithm>
#include <atomic>

#include "core/companion_programs.hpp"
#include "core/diag_log.hpp"
#include "core/health_events.hpp"
#include "core/issue_report.hpp"
#include "core/plugin_abi.h"
#include "core/plugin_api.hpp"

namespace cascade::core::companion {

namespace {
std::atomic<int> g_override{-1};
}

health::Recovered wordOfKind(std::uint32_t kind) {
    switch (kind) {
        case CASCADE_FAULT_STALL: return health::Recovered::CompanionStall;
        case CASCADE_FAULT_BREAK: return health::Recovered::CompanionBreak;
        case CASCADE_FAULT_GAVE_UP: return health::Recovered::CompanionGaveUp;
        default: return health::Recovered::CompanionCrash;
    }
}

bool reportsOn() {
    const int o = g_override.load(std::memory_order_acquire);
    return o < 0 ? kCompanionFaultReportsOn : o != 0;
}

void setReportsOnForTest(int state) { g_override.store(state, std::memory_order_release); }

std::size_t file(const std::array<std::uint64_t, 4>& sums) {
    try {
        const bool any = std::any_of(sums.begin(), sums.end(), [](std::uint64_t n) { return n != 0; });
        if (!any) { return 0; }
        if (!reportsOn()) {
            // Held back: one fixed line, no number of any kind of fault and nothing of the plugin.
            cascade::core::diagLogf("plugins: a companion program fault was reported by a plugin; companion fault reports are held back in this build");
            return 0;
        }
        std::size_t filed = 0;
        for (std::uint32_t kind = CASCADE_FAULT_CRASH; kind <= CASCADE_FAULT_GAVE_UP; ++kind) {
            const std::uint64_t n = sums[kind - 1u];
            if (n == 0) { continue; }
            const health::Recovered word = wordOfKind(kind);
            if (auto g = health::globalLedger()) {
                g->note(health::tokenRecovered(word), static_cast<std::uint32_t>(std::min<std::uint64_t>(n, health::kMaxCount)));
            }
            issue::fileIssueReportOffThread(word);
            ++filed;
        }
        return filed;
    } catch (...) {
        return 0;
    }
}

std::size_t drain(PluginApiCore& api) {
    std::array<std::uint64_t, 4> sums{};
    api.takeFaultReports(sums);
    takeHostFaults(sums);   // companion applications shelf: a program the HOST refused to start (changed on disk, gone, no checksum) counts the same way (kind BREAK), never with a name
    return file(sums);
}

}  // namespace cascade::core::companion
