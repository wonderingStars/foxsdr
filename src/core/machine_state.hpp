// machine_state.hpp - the facts about THIS COMPUTER that can explain a starved thread, as one log line (2026-11-23).
//
// WHY. The ringdrop and audiolead reports say a thread did not get the processor; they cannot say why. The facts that most often
// explain it are cheap, read-only operating-system queries: the machine is on battery with the power saver on, the process was
// throttled (efficiency mode), the timer runs at its coarse default, the window is minimised or the display is off, the process is
// confined to a few processors, a thread's multimedia-scheduler registration was refused. Nothing here CHANGES a setting.
//
//   machine: power=ac|battery|unknown saver=on|off|unknown throttle=ctl:N,state:N|unknown timer_100ns=N|unknown cpus=N affinity=N
//            window=visible|minimised|hidden|unknown display=on|off|dimmed|unknown prio=dsp:WORD,src:WORD clock_pct=N|unknown
//
// clock_pct is the processor's clock now (the highest of any logical processor) as a whole percentage of its maximum, as Windows reports them
// (CallNtPowerInformation, read-only); a core held at its lowest clock shows far below 100. NEVER MHZ (owner 2026-11-23: the privacy text sends
// the processor model without its clock speed). Left out when the line would not fit.
//
// Every value is a fixed word or a number: no device name, path, frequency or user text. WORD is the first word of the thread's
// priority outcome ("mmcss", "above-normal", "sched-rr", "none"). Where a query is not available the word is "unknown" - never a guess.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <string>

namespace cascade::core {

// What the GUI knows about the display this session is shown on (the legacy window's display-off pause): 0 on, 1 off, 2 dimmed,
// anything else unknown. One relaxed store; the machine line reads it.
void noteDisplayState(int state);

// The line above, without a trailing newline. `dspPrio` and `srcPrio` are the pipeline's outcome strings ("mmcss", "none: ...").
std::string machineStateText(const std::string& dspPrio, const std::string& srcPrio);

// THE PROCESS'S MEMORY (read-only, Windows; false elsewhere or on a failed query): its page-fault count so far and its working set in MB, and the
// system's memory load in percent (-1 = unknown). The engine's loss minute reports the fault count as a difference and the other two as they are.
bool processMemoryNow(unsigned long long* pageFaults, unsigned long long* workingSetMb);
int systemMemoryLoadPercent();

// Logs it (diagLogf) when it differs from the last one logged by this function, or `force` is true; returns whether it logged.
// For the pass thread only (it keeps the last text without a lock).
bool logMachineStateIfChanged(const std::string& dspPrio, const std::string& srcPrio, bool force);

// THE POWER WORD THE LAST machine LINE SAID ("ac", "battery" or "unknown"), kept by machineStateText() so a diagnostic elsewhere (the RTL-SDR reader's "stream
// stopped" line) can name the power state WITHOUT a query of its own on its thread. "unknown" until the first machine line has been made. One relaxed load.
const char* lastKnownPowerWord();

// TEST SEAM: forget the last logged text.
void resetMachineStateLogForTest();
// TEST SEAMS: replace what the machine says. setMachineSampleForTest(nowMhz, maxMhz, memLoadPercent, commitPercent) makes the processor clock (in MHz, which
// only the percentage is ever made from), the system memory load and the commit charge (percent of the limit) made-up values; nowMhz < 0 goes back to the
// real machine, and a zero now or max reads as unknown. setMachineLineBudgetForTest(n) sets the character budget the clock words must fit in to be said
// inline (0 = the real one).
void setMachineSampleForTest(long nowMhz, long maxMhz, int memLoadPercent, int commitPercent);
void setMachineLineBudgetForTest(std::size_t chars);

}  // namespace cascade::core
