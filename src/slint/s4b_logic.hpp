// s4b_logic.hpp - SCREEN LANE S4b's pure rules (docs/multi-radio-records/screen-s4b.md): what a typed field of the scanner and output controls means, and the texts the operations take.
// No window, no engine: the publisher (s4b_publish.cpp) sends what these produce.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <optional>
#include <cstdint>
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

// ---- the antenna rotator ----

// A number of degrees typed by the person (a decimal point or a decimal comma, spaces around it): nullopt for anything else or a value outside [lo, hi].
std::optional<double> parseDegrees(const std::string& text, double lo, double hi);
// The text FOXAPP_OP_ROTATOR takes: {"action":"turn","az":..,"el":..}, {"action":"stop"} or {"action":"park"}.
std::string rotatorTurnJson(double az, double el);
std::string rotatorActionJson(const char* action);

// ---- the macro recorder ----

// The name the macro model gives an op the recorder keeps, or "" for any other op. The recorder keeps ONLY the receiver's own tuning and listening settings: the frequency, the centre, the mode,
// the bandwidth, the squelch, the volume and the mute. Everything else the window sends (a scan, a bookmark, a recording, a setting of a server, anything that transmits) is never recorded.
std::string recordableName(std::uint32_t op);

struct RecordedStep {
    std::string command;
    double num[4] = {0, 0, 0, 0};
    long long ival[2] = {0, 0};
    double atMs = 0.0;
};

// Records the commands the window sends while the person has it on. Not thread-safe by itself: the caller (the table wrapper) holds a lock.
class MacroRecorder {
public:
    static constexpr std::size_t kMaxSteps = 500;
    void start();                       // forgets what was recorded and records from now
    void stop();
    void clear();
    bool active() const { return active_; }
    std::size_t size() const { return steps_.size(); }
    std::size_t skippedOtherRadio() const { return otherRadio_; }
    // One command the window sent (accepted by the engine). True when it was kept. A command aimed at another radio than the first is counted and not kept (a macro acts on the first radio).
    bool note(std::uint32_t op, std::uint32_t target, const double num[4], const long long ival[2], double nowMs);
    // The JSON of FOXAPP_OP_MACRO_EDIT for these steps: each step's delay is the time to the next one (at most 60 s), the last has none. "" when nothing was recorded.
    std::string saveJson(const std::string& name) const;

private:
    bool active_ = false;
    std::size_t otherRadio_ = 0;
    std::vector<RecordedStep> steps_;
};

// The one recorder of the window, and the lock that guards it.
MacroRecorder& macroRecorder();


}  // namespace foxsdr::slints4b
