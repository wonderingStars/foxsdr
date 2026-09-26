// status_compose.hpp - the RadioStatus a web or CAT reader is given, composed
// from the one published receiver snapshot (engine stage 2,
// docs/engine-stage2.md).
//
// The snapshot carries the receiver's figures as FoxReceiverState plus the
// app's extension (core/receiver_snapshot.hpp), and the /api/status text and
// lists as a RadioStatus whose scalar members are left at their defaults.
// composeRadioStatus puts the two together: every scalar member of the result
// comes from the PublishedState, every string and list from `lists`. It runs
// on the READER's thread (an HTTP worker, a CAT connection), not on the
// thread that publishes.
//
// tests/test_receiver_snapshot.cpp pins the mapping field by field (and fails
// when a RadioStatus member is added without a row); test_state_snapshot_golden
// pins the whole path against the record the code before stage 2 wrote.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_NET_STATUS_COMPOSE_HPP
#define CASCADE_NET_STATUS_COMPOSE_HPP

#include <cstdint>

#include "core/receiver_snapshot.hpp"
#include "net/web_server.hpp"

namespace cascade::net {

// The receiver's mode names, indexed by FOXAPI_DEMOD_* (1..8): the SAME words
// the desktop's mode buttons carry (app_window.cpp kModeNames, checked against
// this table where it is defined). "" outside 1..8.
inline constexpr const char* kDemodNames[9] = {"", "NFM", "WFM", "AM", "DSB",
                                               "USB", "CW", "LSB", "RAW"};
const char* demodName(std::uint32_t foxDemod);

// "idle", "scanning", "paused", "holding" for core::kScanner*; "" otherwise.
const char* scannerStateName(std::uint32_t state);

// Everything a reader of /api/status or CAT is told, from one publish.
// `lists` may be null (CAT, which reads only figures): the strings and lists
// are then empty. Before the first publish (state.app.published false) the
// answer is a default RadioStatus, which is what a reader got then.
RadioStatus composeRadioStatus(const cascade::core::PublishedState& state, const RadioStatus* lists);

}  // namespace cascade::net

#endif  // CASCADE_NET_STATUS_COMPOSE_HPP
