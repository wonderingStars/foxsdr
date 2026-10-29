// demod_names.hpp - the receiver's mode names as data (engine extraction stage 5f, the fence's last hoist for net/).
//
// Moved out of net/status_compose.hpp, unchanged: core/receiver_tables.hpp (an allowed header of the engine's directory, so the interface
// includes it) checks at compile time that the window's mode buttons (kModeNames) name the modes as the web page and CAT do, and took
// this table from net/status_compose.hpp - the one thing the interface reached in net/. net/ is the engine's now (the servers are its own,
// docs/engine-stage4.md 6.10-6.13) and is behind the interface fence; the table is plain data, so it lives where the data headers live.
// net/status_compose.hpp includes this header and names the table back (cascade::net::kDemodNames), so every net/ user is unchanged.
//
// Nothing here takes anything but the language.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_DEMOD_NAMES_HPP
#define CASCADE_CORE_DEMOD_NAMES_HPP

namespace cascade::core {

// The receiver's mode names, indexed by FOXAPI_DEMOD_* (1..8): the SAME words the desktop's mode buttons carry (app_window.cpp kModeNames,
// checked against this table in core/receiver_tables.hpp). "" at 0 (no mode).
inline constexpr const char* kDemodNames[9] = {"", "NFM", "WFM", "AM", "DSB", "USB", "CW", "LSB", "RAW"};

}  // namespace cascade::core

#endif  // CASCADE_CORE_DEMOD_NAMES_HPP
