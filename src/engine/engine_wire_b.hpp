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
#include <memory>
#include <mutex>
#include <string>

#include <array>

#include "core/field_strength.hpp"
#include "core/spectrum_mask.hpp"
#include "engine/wire_b_sweep.hpp"
#include "engine/wire_b_bits.hpp"
#include "engine/wire_b_cursors.hpp"

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

    // --- bitview: one ring of kept bursts and the last decoded frames per radio ---
    std::array<wireb::BitsRadio, 8> bits;

    // --- sigmeas: the eight cursors of each radio and the one frame log ---
    std::array<cascade::sigmeas::CursorSet8, 8> cursors;
    std::unique_ptr<cascade::sigmeas::FrameLog> frameLog = std::make_unique<cascade::sigmeas::FrameLog>(256);

    // --- rfmon: the limit mask (one mask, one evaluator and monitor per radio) ---
    struct MaskRadio {
        cascade::core::MaskEvaluator ev;
        cascade::core::MaskMonitor mon;
        double centre = 0.0, span = 0.0;
        std::size_t bins = 0;
        std::uint64_t seq = 0;
        bool pass = true;
        bool anyLimited = false;
        double margin = 0.0, marginFreq = 0.0;
        double captureStartS = -1.0;
        std::size_t captures = 0;
        std::size_t lastLogged = 0;
        std::string captureNote;
    };
    cascade::core::Mask mask;
    bool maskLoaded = false;
    bool maskEnabled = false;
    bool maskCapture = false;
    double maskPostRollS = 2.0;
    cascade::core::MaskMonitorConfig maskCfg;
    std::array<MaskRadio, 8> maskRadios;

    // --- sweepsrc: the sweep device host (one device) and the way its link is made (the tests give a stand-in: no test opens a COM port) ---
    wireb::SweepHost sweep;
    wireb::SerialLinkFactory sweepFactory = wireb::defaultSerialLinkFactory();
};

}  // namespace cascade::engine
