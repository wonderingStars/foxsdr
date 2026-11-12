// gaps_logic.hpp - THE CLASSIC-WINDOW GAPS (docs/multi-radio-records/slint-gaps.md): the pure rules and the one process-wide lock, no Slint, no Win32.
//
//   item 2  dottedFrequencyText / parseTypedFrequencyHz: how the typed frequency is written and read with the counter's two digit groupings
//   item 3  FrequencyLock + lockedApi: the per-radio frequency lock. EVERY command the window sends to the engine goes through the API table the host hands to its publishers; that table
//           is a copy whose submit and submit_text first ask the lock (lockedApi), so a wheel, a drag, a click on the plot, a counter digit, a key, a band key, a bookmark or a scanner start of a LOCKED radio is
//           refused in one place, whichever publisher sent it. Plugins, the web page, CAT and a scanner already running do not come through this table and are NOT stopped (see the record: ENGINE HAND-OFF).
//   item 8  classifyDrop: what a file dropped on the window is
//   item 10 manualUrl
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "foxsdr_api.h"

namespace foxsdr::slintgaps {

// ---- item 2: digit grouping ----
// style 0 = thousands from the right (1.090.000.000), style 1 = MHz.kHz.Hz (1090.000.000). The text always has at least two dots (so a value under 1 MHz reads "0.145.500"), which is what lets
// parseTypedFrequencyHz tell it from a decimal MHz ("145.5").
std::string dottedFrequencyText(double hz, int style);
// Two or more dots: they are grouping marks, the digits are a whole number of the unit (Hz when there is none: "1090.000.000" = 1 090 000 000 Hz; "7.074 MHz" has one dot and stays a decimal).
// Anything else is host_logic's parseFrequencyHz, unchanged. Either style is accepted whatever the option says.
bool parseTypedFrequencyHz(const std::string& text, double& hz);

// ---- item 3: the lock ----
// True for the commands that move a radio's frequency: SET_CENTRE, SET_FREQUENCY, SET_VFO_OFFSET, STEP_TUNE, BOOKMARK_TUNE, PLUGIN_PRESET and a SCANNER_RUN that starts (ival[0] != 0). Everything else
// (mode, bandwidth, volume, squelch, stop, scanner stop...) is not tuning. DECODER_START applies a preset on the engine's side and is NOT in the set (record: what can still move a locked radio).
bool isTuningCommand(const FoxCommand& c);

class FrequencyLock {
public:
    static FrequencyLock& instance();
    void setMask(std::uint32_t mask) { mask_.store(mask & 0xFFu, std::memory_order_relaxed); }   // bit i = radio i + 1
    std::uint32_t mask() const { return mask_.load(std::memory_order_relaxed); }
    bool locked(int radio) const { return radio >= 1 && radio <= 8 && ((mask() >> (radio - 1)) & 1u) != 0u; }
    void setSelected(int radio) { selected_.store(radio < 1 ? 1 : radio, std::memory_order_relaxed); }   // the radio a command that names none (target 0) goes to: the session's focus
    int selected() const { return selected_.load(std::memory_order_relaxed); }
    // The radio a command is aimed at: its target, else `radioOr0` (the strip's own aim), else the selected radio.
    int radioOf(const FoxCommand& c, std::uint32_t radioOr0 = 0) const;
    // True when the command is a tuning command of a locked radio. Counts the refusal (refusals(), lastRefusedRadio()).
    bool refuses(const FoxCommand& c, std::uint32_t radioOr0 = 0);
    long refusals() const { return refusals_.load(std::memory_order_relaxed); }
    int lastRefusedRadio() const { return lastRefused_.load(std::memory_order_relaxed); }
    void resetRefusals() { refusals_.store(0, std::memory_order_relaxed); }

private:
    std::atomic<std::uint32_t> mask_{0};
    std::atomic<int> selected_{1};
    std::atomic<long> refusals_{0};
    std::atomic<int> lastRefused_{0};
};

// A copy of `real` whose submit and submit_text refuse a locked radio's tuning commands (result status FOXAPI_DENIED, ticket 0, the rest of a batch goes on). `real` must outlive the copy.
const FoxEngineApi* lockedApi(const FoxEngineApi* real);

// ---- item 8: a dropped file ----
enum class DropKind { IqRecording, FrequencyList, Unknown };
// .wav = a recording to play; .xml and .csv = a frequency list to import (what the classic window does with them); anything else is refused. The path may carry one pair of quotes.
DropKind classifyDrop(const std::string& path);

// ---- item 10 ----
// The manual: docs/MANUAL.md of the project's public repository (the classic window has no help entry; this is the page the README points at).
const char* manualUrl();

}  // namespace foxsdr::slintgaps
