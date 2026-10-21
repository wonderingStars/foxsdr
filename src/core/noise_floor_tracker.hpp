// noise_floor_tracker.hpp - THE NOISE FLOOR UNDER A CHANNEL, measured from the meter, and the
// squelch threshold AUTOMATIC SQUELCH derives from it. No ImGui, no pipeline: the window
// pushes the S-meter's reading, the clock and the gate's state in, and a test pushes whatever
// it likes.
//
// WHY IT EXISTS (0.99.74). The tester: "Add a Squelch knob, like the gain knob above but with
// a button for AUTO or above the noise." A squelch has to sit above the noise to keep it out
// and below the signals to let them in, and where the noise is changes with the band, the
// antenna, the gain and the weather; the person turning the knob is hunting for that level.
// AUTOMATIC finds it: the floor is the QUIETEST the channel has been lately, and the threshold
// sits a fixed margin above it.
//
// THE FLOOR IS THE MINIMUM OF THE LAST TEN SECONDS OF 100 ms BLOCK MEANS. A minimum, not an
// average, because a floor is what is there when nobody is talking: a carrier that keys up for
// a second raises the mean of its window and does not raise the minimum, so the squelch is not
// dragged up to the very signal it exists to pass.
//
// AND A CARRIER NEVER TEACHES IT (the owner's follow-up: with a three-second window every
// transmission longer than three seconds became the floor and closed the gate on itself).
// Once a floor estimate exists, readings taken while the squelch gate is OPEN are not fed at
// all - an open gate means the signal is being passed, which is not the noise. Before the first
// estimate everything is fed (the threshold at power-up is wherever the config left it, and the
// gate may be open on the noise itself). While readings are being held back the estimate
// stands: the ring of blocks drains as it ages, and the last floor it held is kept until the
// ring has five blocks of its own again.
//
// THE ONE SAFETY VALVE: noise that rises past the threshold opens the gate too, and with the
// readings held back it could never be learnt - the squelch would sit open on a noisier antenna
// for ever. So a gate that stays open continuously for kNoiseFloorGateHoldS (30 s, longer than
// any transmission worth the name and than the ten-second window) is fed again, as the floor
// having risen. What remains of the cost: that rise is followed after 30 s and not at once,
// and a signal that holds the channel for longer than that - a continuous broadcast tuned in
// from a quiet frequency - is the floor, and AUTOMATIC will close on it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_NOISE_FLOOR_TRACKER_HPP
#define CASCADE_CORE_NOISE_FLOOR_TRACKER_HPP

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>

namespace cascade::core {

// One block is a tenth of a second of the meter, however often the caller pushes (a frame
// loop pushes at 60 Hz; a dormant window at a few Hz: a block simply holds fewer readings).
inline constexpr double kNoiseFloorBlockS = 0.1;
// How much history the floor is the minimum of. Three seconds in the first cut of 0.99.74;
// ten since the follow-up above.
inline constexpr double kNoiseFloorWindowS = 10.0;
// A gate open this long without a break stops being held back (see the safety valve above).
inline constexpr double kNoiseFloorGateHoldS = 30.0;
// Fewer blocks than this is not a measurement yet: the floor is NaN until half a second is in.
inline constexpr int kNoiseFloorMinBlocks = 5;
// The pipeline reports -200 dB before its first block and while it is stopped. Anything at or
// below this is "no data", not a quiet channel, and is ignored.
inline constexpr double kNoiseFloorNoDataDb = -150.0;

// How far above the measured floor AUTOMATIC puts the threshold, and the span a threshold may
// take (the same [-120, 0] dB the Radio section's slider and the keys keep to).
inline constexpr double kSquelchAutoMarginDb = 8.0;
inline constexpr double kAutoSquelchMinDb = -120.0;
inline constexpr double kAutoSquelchMaxDb = 0.0;

// The threshold AUTOMATIC sets for a measured floor: the margin above it, kept inside the
// slider's span. NaN for a floor that is not a measurement (the caller leaves the threshold
// where it is).
inline double autoSquelchTargetDb(double floorDb) {
    if (!std::isfinite(floorDb)) { return std::numeric_limits<double>::quiet_NaN(); }
    return std::min(std::max(floorDb + kSquelchAutoMarginDb, kAutoSquelchMinDb),
                    kAutoSquelchMaxDb);
}

class NoiseFloorTracker {
public:
    // A reading of the channel power in dB, `nowS` seconds on any monotonic clock, and whether
    // the squelch gate is open as that reading is taken.
    void push(double powerDb, double nowS, bool gateOpen = false) {
        // A clock that went backwards is a different session: start again rather than keep
        // blocks from a future the new clock has not reached.
        if ((open_ && nowS < blockStartS_) || (!ring_.empty() && nowS < ring_.back().startS)) {
            reset();
        }
        // The block in hand is closed when a reading arrives after its tenth of a second.
        if (open_ && nowS - blockStartS_ >= kNoiseFloorBlockS) {
            ring_.push_back({blockStartS_, sumDb_ / static_cast<double>(count_)});
            open_ = false;
        }
        // History ages out whatever the reading is, so a run of "no data" empties the floor.
        while (!ring_.empty() && ring_.front().startS < nowS - kNoiseFloorWindowS) {
            ring_.pop_front();
        }
        const bool ringReady = static_cast<int>(ring_.size()) >= kNoiseFloorMinBlocks;
        // The ring speaks for itself again: whatever floor was held through a carrier is spent.
        if (ringReady) { heldValid_ = false; }
        // How long the gate has been open without a break.
        if (gateOpen) {
            if (!gateRun_) {
                gateRun_ = true;
                gateSinceS_ = nowS;
            }
        } else {
            gateRun_ = false;
        }
        if (!std::isfinite(powerDb) || powerDb <= kNoiseFloorNoDataDb) { return; }
        // AN OPEN GATE IS NOT NOISE: once there is an estimate, the reading is not fed - unless
        // the gate has been open for the hold, which is the floor having risen past it.
        const bool haveEstimate = ringReady || heldValid_;
        if (gateOpen && haveEstimate && nowS - gateSinceS_ < kNoiseFloorGateHoldS) {
            if (ringReady) {
                heldFloorDb_ = ringMinimum();
                heldValid_ = true;
            }
            return;
        }
        if (!open_) {
            open_ = true;
            blockStartS_ = nowS;
            sumDb_ = 0.0;
            count_ = 0;
        }
        sumDb_ += powerDb;
        ++count_;
    }

    // The quietest block mean of the window, or - while the gate has been holding readings
    // back and the ring has drained - the last floor the ring held; NaN until
    // kNoiseFloorMinBlocks are held.
    double floorDb() const {
        if (static_cast<int>(ring_.size()) >= kNoiseFloorMinBlocks) { return ringMinimum(); }
        if (heldValid_) { return heldFloorDb_; }
        return std::numeric_limits<double>::quiet_NaN();
    }

    // Blocks held (closed ones only): what floorDb() is the minimum of.
    int blocks() const { return static_cast<int>(ring_.size()); }

    // Forget everything: a retune, a new bandwidth, a stopped receiver or AUTOMATIC switched
    // off all end the measurement the next reading would have belonged to.
    void reset() {
        ring_.clear();
        open_ = false;
        sumDb_ = 0.0;
        count_ = 0;
        heldValid_ = false;
        heldFloorDb_ = 0.0;
        gateRun_ = false;
        gateSinceS_ = 0.0;
    }

private:
    struct Block {
        double startS;
        double meanDb;
    };
    double ringMinimum() const {
        double lowest = ring_.front().meanDb;
        for (const Block& b : ring_) { lowest = std::min(lowest, b.meanDb); }
        return lowest;
    }
    std::deque<Block> ring_;
    bool open_ = false;
    double blockStartS_ = 0.0;
    double sumDb_ = 0.0;
    int count_ = 0;
    // The floor kept while an open gate holds readings back and the ring drains under it.
    bool heldValid_ = false;
    double heldFloorDb_ = 0.0;
    // The gate's present unbroken run of "open", and when it began.
    bool gateRun_ = false;
    double gateSinceS_ = 0.0;
};

}  // namespace cascade::core

#endif  // CASCADE_CORE_NOISE_FLOOR_TRACKER_HPP
