// companion_fault.hpp - what the host does with a plugin's report that a program it runs beside itself failed (plugin_abi.h report_fault; engine fixes 7).
//
// THE CALL. A plugin that runs a companion program (a decoder in a program of its own) says, once a minute at most per kind, that it crashed, stalled, broke or was given
// up on, with how many times since the last report. PluginApiCore::reportFault checks and rate-limits it and keeps only two numbers per kind (the sum of the counts);
// nothing else is kept - no plugin name, no identifier, no path, no text. This file is what happens to those sums.
//
// THE SWITCH. The four words (health::Recovered::CompanionCrash, CompanionStall, CompanionBreak, CompanionGaveUp = `recovered.companioncrash` ...) must be accepted by
// the telemetry Worker BEFORE an app sends them: the Worker drops a word it does not know, and the OLD Worker answers 204, which tells the app to forget the count
// (docs/DIAGNOSTICS.md, "Deployment order"). Nothing was deployed by the change that added them, so kCompanionFaultReportsOn is FALSE. While it is false the call is
// still accepted and rate-limited (the plugin is told OK, as the header says OK is not "sent"), the sums are discarded by drain(), and the log says only that reports
// are held back. THE ONE LINE TO FLIP, once the Worker is confirmed to list the four words: `kCompanionFaultReportsOn = true` below (and tests/test_plugin_radio_fault.cpp
// then needs no change: it sets the switch itself).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_COMPANION_FAULT_HPP
#define CASCADE_CORE_COMPANION_FAULT_HPP

#include <array>
#include <cstdint>

#include "core/health_events.hpp"

namespace cascade::core {
class PluginApiCore;
}

namespace cascade::core::companion {

// THE SWITCH: OFF until the telemetry Worker lists recovered.companioncrash, .companionstall, .companionbreak and .companiongaveup.
inline constexpr bool kCompanionFaultReportsOn = false;

// The word of a kind (CASCADE_FAULT_CRASH .. CASCADE_FAULT_GAVE_UP); never called with another number.
health::Recovered wordOfKind(std::uint32_t kind);

// Whether reports are filed now: kCompanionFaultReportsOn, unless a test has set it (-1 = the constant, 0 = off, 1 = on).
bool reportsOn();
void setReportsOnForTest(int state);

// The pass's step: takes what plugins reported since the last call. Switch on: each kind with a non-zero sum is counted in the health ledger (the sum, once) and filed
// as an issue report (once a session per word, as every issue word is). Switch off: the sums are dropped. Returns how many kinds were filed (0 when held back).
// Never throws. Called from the engine's pass, never from a plugin's thread.
std::size_t drain(PluginApiCore& api);

// The same, from the four sums already taken (index kind-1): the part the test drives without an API core.
std::size_t file(const std::array<std::uint64_t, 4>& sums);

}  // namespace cascade::core::companion

#endif  // CASCADE_CORE_COMPANION_FAULT_HPP
