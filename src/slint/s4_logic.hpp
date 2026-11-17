// s4_logic.hpp - SCREEN LANE S4's pure rules (docs/multi-radio-records/screen-s4-wave8.md): what a typed field of the scanner, outputs and alerts controls means, the JSON of the
// server-configuration operation for the TCI and UDP audio switches, the keyword list as one line, and the tracker that saves a scan's hits as bookmarks when the person asked for that.
// No window, no engine: the publisher (s4_publish.cpp) sends what these produce.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>

namespace foxsdr::slints4 {

// A whole number from `text` (spaces around it allowed) inside [lo, hi]; nullopt for anything else (empty, a fraction, a sign, letters, a number too long to be one).
std::optional<int> parseWhole(const std::string& text, int lo, int hi);

// {"tci":{"<member>":<value>}} and {"udp":{...}}: the one-member JSON texts the server-configuration operation takes (the engine leaves every other member alone).
std::string tciBool(const char* member, bool value);
std::string tciPort(int port);
std::string tciText(const char* member, const std::string& value);
std::string udpBool(const char* member, bool value);
std::string udpInt(const char* member, int value);
std::string udpText(const char* member, const std::string& value);

// True for a host the audio or a broker would be sent to that is not this computer (anything but 127.x, ::1, localhost).
bool leavesThisComputer(const std::string& host);

// The keyword list as one line the person edits ("mayday, pan pan") and back to the engine's one-keyword-per-line text. Commas and semicolons separate; each keyword is trimmed.
std::string keywordsToLine(const std::string& lines);
std::string lineToKeywords(const std::string& line);

// Saves a scan's hits as bookmarks, when switched on. update() is called once a tick with the selected radio's scanner state and the frequency it is on; it returns the frequency to save
// when the scan has just stopped on a signal that was not saved yet (the engine refuses one that already is a bookmark). Nothing is returned while the switch is off.
class AutoBookmarks {
public:
    std::optional<double> update(bool enabled, std::uint32_t scannerState, double hz);
    void reset();
    std::size_t remembered() const { return saved_.size(); }

private:
    std::uint32_t last_ = 0;
    bool wasEnabled_ = false;
    std::set<long long> saved_;   // frequencies sent, in units of 100 Hz; at most kMax kept
};

}  // namespace foxsdr::slints4
