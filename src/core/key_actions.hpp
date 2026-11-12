// KEYBOARD ACTIONS that the key table does not have yet, as standalone handlers: bandwidth up / down, cycle the tuning step, cycle the mode, centre the signal,
// next / previous bookmark.
//
// gui/key_bindings.cpp (the action list, the chords, the dispatch) belongs to screen lane C and is not edited here. This header is what that lane needs to add
// the six rows without writing the logic: the action ids, the English labels, the PROPOSED default chords (as text in key_bindings.hpp's own form; they are
// proposals, the lane checks them against its conflict table), and one pure function per action that takes the current state and returns the new state.
// The caller applies the result with the calls it already has (set bandwidth, set step, set mode, retune); every function says what it returns when there is
// nothing to do, so a key press that cannot act is silent.
//
// bandwidthStep(): the next bandwidth up or down in a mode's own list of bandwidths (the lists below are a DEFAULT table the operator can replace: the widths
// the receiver's filters usually offer, UNVERIFIED against this program's filters; the function works on any ascending list). Up from the widest and down
// from the narrowest return the same value (no wrap): a filter that jumped from 15 kHz to 300 Hz on one key press would be a bad surprise.
// cycleStep() / cycleMode(): next or previous in a list, WRAPPING (a cycle is meant to come round), from a value not in the list to the nearest larger (next)
// or smaller (previous) entry.
// centreOnSignal(): given the tuned frequency, the peak the caller found inside the passband and the largest move allowed, the new tuned frequency that puts
// the peak at the centre of the passband (for SSB / CW the carrier offset the mode wants is the caller's: it passes the peak it wants centred); refused (nullopt)
// for a peak that is not finite, is farther than maxMoveHz, or is already within deadbandHz.
// nextBookmark() / previousBookmark(): from a list of bookmark frequencies sorted ascending, the next one above (or the first when none is above - the list
// wraps) and the previous one below (or the last).
//
// Pure functions, thread safe, no state.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace cascade::core::tuning {

enum class KeyActionId { BandwidthUp, BandwidthDown, CycleStep, CycleMode, CentreSignal, NextBookmark, PreviousBookmark };

struct KeyActionInfo {
    KeyActionId id;
    const char* idText;       // the id string a config file would store
    const char* label;        // English label for the settings list
    const char* proposedChord; // a proposal, "Ctrl+Up" style text
};

inline const std::vector<KeyActionInfo>& keyActionList() {
    static const std::vector<KeyActionInfo> list = {
        {KeyActionId::BandwidthUp, "bandwidth_up", "Bandwidth wider", "Ctrl+Right"},
        {KeyActionId::BandwidthDown, "bandwidth_down", "Bandwidth narrower", "Ctrl+Left"},
        {KeyActionId::CycleStep, "cycle_step", "Next tuning step", "Ctrl+S"},
        {KeyActionId::CycleMode, "cycle_mode", "Next mode", "Ctrl+M"},
        {KeyActionId::CentreSignal, "centre_signal", "Centre the signal", "Ctrl+Space"},
        {KeyActionId::NextBookmark, "next_bookmark", "Next bookmark", "Ctrl+Down"},
        {KeyActionId::PreviousBookmark, "previous_bookmark", "Previous bookmark", "Ctrl+Up"},
    };
    return list;
}

inline const KeyActionInfo* findKeyAction(const std::string& idText) {
    for (const KeyActionInfo& a : keyActionList()) { if (idText == a.idText) { return &a; } }
    return nullptr;
}

// A default table of bandwidths in Hz by mode name (ascending); empty for a mode that has none.
inline std::vector<double> defaultBandwidths(const std::string& mode) {
    if (mode == "AM") { return {3000, 4000, 5000, 6000, 8000, 10000, 12000}; }
    if (mode == "NFM") { return {5000, 6250, 7500, 10000, 12500, 15000, 25000}; }
    if (mode == "WFM") { return {100000, 150000, 180000, 200000, 250000}; }
    if (mode == "USB" || mode == "LSB") { return {1800, 2100, 2400, 2700, 3000, 3600}; }
    if (mode == "CW") { return {100, 200, 300, 500, 800, 1200}; }
    return {};
}

// The next bandwidth in `list` (ascending) above (dir > 0) or below (dir < 0) `current`; `current` itself when there is none; nullopt for an empty list or a
// non-finite current.
inline std::optional<double> bandwidthStep(const std::vector<double>& list, double current, int dir) {
    if (list.empty() || !std::isfinite(current) || dir == 0) { return std::nullopt; }
    if (dir > 0) {
        for (const double v : list) { if (v > current + 0.5) { return v; } }
        return list.back() > current ? list.back() : current;
    }
    for (std::size_t i = list.size(); i-- > 0;) { if (list[i] < current - 0.5) { return list[i]; } }
    return list.front() < current ? list.front() : current;
}

// Next (dir > 0) or previous (dir < 0) entry of `list`, wrapping; from a value that is not in the list, the nearest larger (next) or smaller (previous) one,
// wrapping past the ends. nullopt for an empty list or dir == 0.
inline std::optional<double> cycleStep(const std::vector<double>& list, double current, int dir) {
    if (list.empty() || dir == 0 || !std::isfinite(current)) { return std::nullopt; }
    std::vector<double> s = list;
    std::sort(s.begin(), s.end());
    if (dir > 0) {
        for (const double v : s) { if (v > current + 1e-9) { return v; } }
        return s.front();
    }
    for (std::size_t i = s.size(); i-- > 0;) { if (s[i] < current - 1e-9) { return s[i]; } }
    return s.back();
}

inline std::optional<std::string> cycleMode(const std::vector<std::string>& modes, const std::string& current, int dir) {
    if (modes.empty() || dir == 0) { return std::nullopt; }
    const auto it = std::find(modes.begin(), modes.end(), current);
    if (it == modes.end()) { return dir > 0 ? modes.front() : modes.back(); }
    const std::size_t i = static_cast<std::size_t>(it - modes.begin());
    return dir > 0 ? modes[(i + 1) % modes.size()] : modes[(i + modes.size() - 1) % modes.size()];
}

inline std::optional<double> centreOnSignal(double tunedHz, double peakHz, double maxMoveHz, double deadbandHz = 5.0) {
    if (!std::isfinite(tunedHz) || !std::isfinite(peakHz) || !std::isfinite(maxMoveHz) || maxMoveHz <= 0.0) { return std::nullopt; }
    const double d = peakHz - tunedHz;
    if (std::fabs(d) <= deadbandHz || std::fabs(d) > maxMoveHz) { return std::nullopt; }
    return peakHz;
}

inline std::optional<double> nextBookmark(const std::vector<double>& sortedHz, double currentHz) {
    if (sortedHz.empty() || !std::isfinite(currentHz)) { return std::nullopt; }
    const auto it = std::upper_bound(sortedHz.begin(), sortedHz.end(), currentHz + 0.5);
    return it == sortedHz.end() ? sortedHz.front() : *it;
}

inline std::optional<double> previousBookmark(const std::vector<double>& sortedHz, double currentHz) {
    if (sortedHz.empty() || !std::isfinite(currentHz)) { return std::nullopt; }
    auto it = std::lower_bound(sortedHz.begin(), sortedHz.end(), currentHz - 0.5);
    return it == sortedHz.begin() ? sortedHz.back() : *(it - 1);
}

}  // namespace cascade::core::tuning
