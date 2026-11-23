// machine_state.hpp - the facts about THIS COMPUTER that can explain a starved thread, as one log line (2026-11-23).
//
// WHY. The ringdrop and audiolead reports say a thread did not get the processor; they cannot say why. The facts that most often
// explain it are cheap, read-only operating-system queries: the machine is on battery with the power saver on, the process was
// throttled (efficiency mode), the timer runs at its coarse default, the window is minimised or the display is off, the process is
// confined to a few processors, a thread's multimedia-scheduler registration was refused. Nothing here CHANGES a setting.
//
//   machine: power=ac|battery|unknown saver=on|off|unknown throttle=ctl:N,state:N|unknown timer_100ns=N|unknown cpus=N affinity=N
//            window=visible|minimised|hidden|unknown display=on|off|dimmed|unknown prio=dsp:WORD,src:WORD
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

// Logs it (diagLogf) when it differs from the last one logged by this function, or `force` is true; returns whether it logged.
// For the pass thread only (it keeps the last text without a lock).
bool logMachineStateIfChanged(const std::string& dspPrio, const std::string& srcPrio, bool force);

// TEST SEAM: forget the last logged text.
void resetMachineStateLogForTest();

}  // namespace cascade::core
