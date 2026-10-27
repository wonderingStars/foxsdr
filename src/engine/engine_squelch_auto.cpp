// engine_squelch_auto.cpp - AUTOMATIC SQUELCH, the top bar's AUTO key under the SQUELCH dial (stub campaign B12, RAIL-B; master 0.99.74).
//
// THE REQUEST (the tester, 2026-10-12): "Add a Squelch knob, like the gain knob above but with a button for AUTO or above the noise." A squelch has to sit above
// the noise to keep it out and below the signals to let them in, and where the noise is changes with the band, the antenna, the gain and the weather; the person
// turning the knob is hunting for that level. AUTOMATIC finds it: the floor is the QUIETEST the channel has been lately (core/noise_floor_tracker.hpp: the
// minimum of the last ten seconds of 100 ms block means, a carrier that holds the gate open never teaching it) and the threshold sits 8 dB above it.
//
// WHERE IT LIVES. In master the window did it (autoSquelchFrame, once a frame, with the tracker a window member and the switch the window's squelchAuto_). It is
// the ENGINE's now: a headless engine, a web page and a CAT client all run the same receiver, and an AUTO that only a window could keep would be a switch that
// goes dead the moment the window is not the one asking (the plan's INFERRED choice, docs/engine-stage4.md 6.22 B12). The window sends FOXAPP_OP_SET_SQUELCH_AUTO
// from the AUTO key and reads fact.squelch.auto for the lamp under it.
//
// THE RULES, each held by tests/test_engine_api_squelch.cpp and (the window's half) the real binary's tests/test_deck_knobs_app.cpp:
//   - the threshold follows the floor measured on the receiver's channel, + 8 dB, re-judged four times a second and moved only for half a decibel or more (the
//     floor's own wobble must not turn the dial and the gate every quarter second);
//   - the receiver's channel is the one tuned: a retune, another bandwidth or another mode is another floor, the one before is dropped at once and the threshold
//     it set stays until the new one has half a second of its own; a stopped receiver measures nothing;
//   - the gate is told to the tracker (pipeline_.squelchOpen()): a carrier never teaches the floor;
//   - WHILE THE AIRBAND MONITOR LISTENS the rule is the monitor's own "above the noise" (Engine::airbandAboveTheNoise, the one place it is written) applied to
//     the monitor's squelch - not the tracker, which measures the receiver's VFO channel, the one thing the monitor is not playing - and the receiver's
//     threshold is left where it was; when the monitor stops the tracker starts afresh;
//   - A HAND ON ANY THRESHOLD IS A MANUAL CHOICE: an accepted SET_SQUELCH (whoever sent it - the dial, the slider, the keys, a web page) and an accepted
//     AIRBAND squelch or above-the-noise turn it off. The rule's own writes do not go through those ops, so they never do. A threshold restored from the config
//     does not.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <climits>
#include <cmath>
#include <cstdint>
#include <string>

#include "core/diag_log.hpp"
#include "core/noise_floor_tracker.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"

namespace cascade::engine {

namespace cc = cascade::core;

namespace {
// How often the rule is judged, and the smallest move worth a call.
constexpr double kSquelchAutoPeriodS = 0.25;
constexpr double kSquelchAutoMinMoveDb = 0.5;
}  // namespace

std::int32_t Engine::applySquelchAutoOp(const FoxCommand& c, FoxCommandResult& res, std::string& why) {
    if (c.ival[0] != 0 && c.ival[0] != 1) {
        why = "ival[0] is 0 (off) or 1 (on)";
        return FOXAPI_OUT_OF_RANGE;
    }
    const bool on = c.ival[0] == 1;
    if (on != squelchAuto_) {
        squelchAuto_ = on;
        // Either way the history is another session's: the floor was measured with the threshold somewhere else, or not at all.
        noiseFloor_.reset();
        squelchAutoSetS_ = -1.0e9;
        squelchAutoOnAirband_ = false;
        cc::diagLogf("squelch: auto %s (AUTO key)", on ? "on" : "off");
    }
    res.applied[0] = squelchAuto_ ? 1.0 : 0.0;
    return FOXAPI_OK;
}

void Engine::squelchAutoOff(const char* why) {
    if (!squelchAuto_) { return; }
    squelchAuto_ = false;
    noiseFloor_.reset();
    squelchAutoSetS_ = -1.0e9;
    squelchAutoOnAirband_ = false;
    cc::diagLogf("squelch: auto off (%s)", why != nullptr ? why : "");
}

void Engine::squelchAutoRestore(bool on) {
    squelchAuto_ = on;   // quietly: a restored setting is not a decision to log
    noiseFloor_.reset();
    squelchAutoSetS_ = -1.0e9;
    squelchAutoOnAirband_ = false;
}

void Engine::squelchAutoFrame() {
    if (!squelchAuto_) { return; }
    const double now = clockS();
    // WHILE THE AIRBAND MONITOR LISTENS, AUTO IS THE MONITOR'S OWN RULE: the median channel level plus 8 dB, re-applied every quarter second to the monitor's
    // squelch. The block is rebuilt only when the value moves by half a decibel or more. The receiver's own threshold is left where it was.
    if (airbandListening_) {
        if (!squelchAutoOnAirband_) {
            squelchAutoOnAirband_ = true;
            noiseFloor_.reset();
            squelchAutoSetS_ = -1.0e9;
        }
        if (now - squelchAutoSetS_ >= kSquelchAutoPeriodS) {
            squelchAutoSetS_ = now;
            airbandAboveTheNoise(static_cast<float>(kSquelchAutoMinMoveDb));
        }
        return;
    }
    if (squelchAutoOnAirband_) {
        squelchAutoOnAirband_ = false;
        noiseFloor_.reset();
        squelchAutoSetS_ = -1.0e9;
    }
    // Nothing to measure while the receiver is stopped (the meter reads "no data" then), and the history from before it stopped is not the channel it will
    // start on.
    if (!pipeline_.running()) {
        noiseFloor_.reset();
        return;
    }
    // A retune, another bandwidth or another mode is another channel and another floor: the history belongs to the one before, and the threshold it set stays
    // until the new one has half a second of its own.
    const double absHz = currentAbsoluteHz();
    if (std::fabs(absHz - squelchAutoAbsHz_) > 1.0 || vfoBandwidthHz_ != squelchAutoBwHz_ || modeIndex_ != squelchAutoMode_) {
        noiseFloor_.reset();
        squelchAutoAbsHz_ = absHz;
        squelchAutoBwHz_ = vfoBandwidthHz_;
        squelchAutoMode_ = modeIndex_;
    }
    // THE GATE IS TOLD TO THE TRACKER: while the squelch is open the signal is being passed, which is not the noise, and once there is a floor estimate such
    // readings are not fed - a carrier never teaches the floor (core/noise_floor_tracker.hpp has the rule and its one valve).
    noiseFloor_.push(static_cast<double>(pipeline_.signalPowerDb()), now, pipeline_.squelchOpen());
    if (now - squelchAutoSetS_ < kSquelchAutoPeriodS) { return; }
    const double target = cc::autoSquelchTargetDb(noiseFloor_.floorDb());
    if (std::isnan(target)) { return; }
    squelchAutoSetS_ = now;
    // Half a decibel is the smallest move worth a call: the floor's own wobble must not turn the dial and the gate every quarter second.
    if (std::fabs(target - static_cast<double>(squelchDb_)) >= kSquelchAutoMinMoveDb) {
        squelchDb_ = static_cast<float>(target);
        pipeline_.setSquelchDb(squelchDb_);
    }
}

// The floor as published: whole decibels (a fact is rebuilt when its fingerprint moves, and a floor that wobbles by a tenth must not move it), INT_MIN when
// there is nothing to say - the switch is off, the monitor listens (its rule has no floor) or the tracker holds no measurement yet.
int Engine::squelchFloorWhole() const {
    if (!squelchAuto_ || airbandListening_) { return INT_MIN; }
    const double f = noiseFloor_.floorDb();
    if (!std::isfinite(f)) { return INT_MIN; }
    return static_cast<int>(std::lround(f));
}

void Engine::buildApiListsSquelch(ApiListBlock& b) {
    b.facts["squelch.auto"] = ApiListBlock::Fact{squelchAuto_ ? "1" : "0", false};
    const int floorDb = squelchFloorWhole();
    b.facts["squelch.floorDb"] = ApiListBlock::Fact{floorDb == INT_MIN ? std::string() : std::to_string(floorDb), false};
}

}  // namespace cascade::engine
