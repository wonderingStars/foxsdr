// engine_wire_b.hpp - the state of the wire-b lane's engine operations (operation block 0x8900-0x89FF; docs/multi-radio-records/wire-b.md).
//
// ONE struct holds what the lane's ops keep between commands, so the Engine class carries a single member for all of them (engine.hpp: `wireB_`).
// Each group of ops adds its members here, in its own commit. Everything is touched on the engine's control pass (a command is applied there, the
// facts are built there); the mutex is for the tests that read a member through the friend.
//
// THE RESULT FACTS. A read operation answers in a fact (LOCAL sessions only: a measurement of the signal is the operator's), keyed
// "radio.<n>.<block>" for a per-radio op and "<block>" for a global one, and in the command result's `message` (one short line) and `applied[]`.
// The fact is replaced by the next call of the same op; nothing is written to disk by any read op.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include "core/field_strength.hpp"

namespace cascade::engine {

struct WireBState {
    std::mutex mutex;
    // The result facts (key without the "fact." prefix -> value).
    std::map<std::string, std::string> facts;

    // --- rfmon: field strength (FIELD_TABLE / FIELD_CONFIG / FIELD_READ) ---
    cascade::core::LossTable antennaFactor;   // dB(1/m) against frequency
    cascade::core::LossTable cableLoss;       // dB against frequency
    double preampGainDb = 0.0;
    double impedanceOhm = 50.0;
    bool showField = false;
};

}  // namespace cascade::engine
