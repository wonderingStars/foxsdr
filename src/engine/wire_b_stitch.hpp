// wire_b_stitch.hpp - THE STITCHED SWEEP OF AN ORDINARY RADIO (wire-b second pass, item 10 of not-wired-after-merge-3.md; survey row X2).
//
// An ordinary SDR sees one sample rate's worth of spectrum. This is the retune-and-measure LOOP that builds a wider picture out of it: tune the radio to one centre after another, wait for the
// retune to settle, take N spectrum frames, average them (power), keep the capture, and after the last centre hand the captures to core/sweep_stitch.hpp `stitchPass`, which trims the roll-off,
// aligns the levels in the overlaps and returns ONE wide line. This file is the loop's STATE MACHINE and nothing else: it touches no radio and no clock of its own (the caller passes the time and
// what the radio shows), so every branch is tested without an engine. Engine::pumpStitch (engine_wire_b.cpp) is the thin driver that does the retunes and reads the frames.
//
// THE PLAN. The radio's sample rate R is read once at the start (the radio must be running). Captures are spaced `stepFraction` * R apart (default 0.6: with the stitcher's 12 % trim at each edge
// that leaves 16 % of R in common between neighbours, which is what the level alignment needs); the first centre puts the lower edge of the kept bins at the start frequency, the last
// covers the stop frequency. At most kMaxSteps centres. The plan is frozen: a change of the radio's sample rate in the middle of a run ends it with a sentence.
//
// WHAT THE CALLER MUST DO with each answer of advance(): Retune -> send SET_CENTRE to retuneTo() (a refusal -> fail(sentence)); LineReady -> takeLine(); Finished -> restore the original centre;
// Failed -> restore the original centre and show failure(). Receive only: the only thing the loop ever asks of the radio is a new centre.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/sweep_stitch.hpp"

namespace cascade::engine::wireb {

inline constexpr std::size_t kStitchMaxSteps = 512;

struct StitchSettings {
    double startHz = 0.0;
    double stopHz = 0.0;
    double stepFraction = 0.6;   // 0.3..0.7 of the radio's sample rate between neighbouring centres
    double trimFraction = 0.12;  // handed to the stitcher (0..0.4)
    int settleMs = 150;          // after SET_CENTRE, before the first frame counts (0..5000)
    int frames = 2;              // frames averaged per centre (1..64)
    int timeoutMs = 4000;        // a retune that does not show, or frames that do not come, end the run (500..60000)
    int passes = 1;              // 0 = repeat until closed (1..1000000)
};

// What the caller can see of the radio at this moment.
struct StitchObservation {
    bool haveFrame = false;
    std::uint64_t seq = 0;                 // the newest frame's sequence number (strictly increasing)
    const std::vector<float>* db = nullptr;  // that frame's bins (fftshifted dB, bin n/2 at the centre)
    double centreHz = 0.0;                 // the radio's centre now
    double rateHz = 0.0;                   // the radio's sample rate now
};

enum class StitchStep { Nothing, Retune, LineReady, Finished, Failed };

class StitchRun {
public:
    // Plans the centres for a radio running at `rateHz`. False with `why` for a range that cannot be done (stop under start, a range over kStitchMaxSteps centres, bad fractions...).
    bool begin(const StitchSettings& s, double rateHz, double nowS, std::string& why);
    // The next thing to do. Call once per control pass.
    StitchStep advance(double nowS, const StitchObservation& obs);
    void fail(const std::string& sentence);   // the caller could not do what advance asked (e.g. the retune was refused)
    void cancel() { if (active()) { phase_ = Phase::Done; note_ = "closed"; } }   // the user closed it

    bool collecting() const { return phase_ == Phase::Collect; }
    bool active() const { return phase_ != Phase::Idle && phase_ != Phase::Done && phase_ != Phase::Failed; }
    double retuneTo() const { return centres_.empty() ? 0.0 : centres_[k_]; }
    bool takeLine(cascade::core::SweepLine& out);   // the pass's wide line (once)
    const std::string& failure() const { return failure_; }
    const std::string& note() const { return note_; }

    std::size_t steps() const { return centres_.size(); }
    std::size_t step() const { return k_; }          // the centre being measured (0-based)
    std::uint64_t passesDone() const { return passesDone_; }
    std::size_t alignedSeams() const { return alignedSeams_; }
    std::size_t seams() const { return seams_; }
    double lastBinHz() const { return lastBinHz_; }
    const std::vector<double>& centres() const { return centres_; }
    const StitchSettings& settings() const { return s_; }

private:
    enum class Phase { Idle, NeedRetune, Settle, Collect, Finishing, Done, Failed };
    Phase phase_ = Phase::Idle;
    StitchSettings s_;
    double rateHz_ = 0.0;
    std::vector<double> centres_;
    std::size_t k_ = 0;
    std::uint64_t passesDone_ = 0;
    double settleEndS_ = 0.0, deadlineS_ = 0.0;
    std::uint64_t lastSeq_ = 0;
    bool haveSeq_ = false;
    int got_ = 0;
    std::vector<double> power_;         // linear power sum of the frames of the current centre
    std::size_t bins_ = 0;
    double capCentre_ = 0.0;
    std::vector<cascade::core::SpectrumCapture> caps_;
    cascade::core::SweepLine line_;
    bool haveLine_ = false;
    std::string failure_, note_;
    std::size_t alignedSeams_ = 0, seams_ = 0;
    double lastBinHz_ = 0.0;
};

}  // namespace cascade::engine::wireb
