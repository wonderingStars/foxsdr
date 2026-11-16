// s4b_logic.hpp - SCREEN LANE S4b's pure rules (docs/multi-radio-records/screen-s4b.md): what a typed field of the scanner and output controls means, and the texts the operations take.
// No window, no engine: the publisher (s4b_publish.cpp) sends what these produce.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace foxsdr::slints4b {

// ---- row 1: the scanner's locked-out frequencies ----

// The most frequencies the engine keeps in its configured locked list (SCANNER_EXTRAS 5).
constexpr std::size_t kMaxLocked = 1000;

struct HzListResult {
    std::vector<double> hz;     // the frequencies, in Hz
    std::string problem;        // "" = fine; otherwise one sentence for the person (already translated)
};
// A list of frequencies typed in MHz ("100.2, 118.1;145.5"): commas, semicolons, spaces and line ends separate. Each must be a number above 0 and at most 100000 MHz; at most kMaxLocked.
// A repeat (within 1 Hz) is dropped. An empty text is an empty list (it clears the configured list).
HzListResult parseMhzList(const std::string& text);
// The engine's "Hz;Hz;..." (fact.scan.locked) as the line the person edits ("100.2000, 118.1000"), in MHz.
std::string hzListToMhzLine(const std::string& engineText);
// The text SCANNER_EXTRAS 5 takes: Hz values separated by semicolons.
std::string hzListToWire(const std::vector<double>& hz);

struct Lockout {
    double hz = 0.0;
    long long secondsLeft = 0;   // 0 = until cleared
};
// fact.scan.lockouts ("<Hz>@<seconds left>;..." 0 = until cleared) read back. Anything malformed is skipped; at most `maxItems` are kept.
std::vector<Lockout> parseLockouts(const std::string& factText, std::size_t maxItems = 1000);
// "100.2000 MHz: until cleared" or "100.2000 MHz: 8 min 20 s left" (translated words).
std::string lockoutLine(const Lockout& l);

}  // namespace foxsdr::slints4b
