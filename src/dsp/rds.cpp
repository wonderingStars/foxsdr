// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/rds.hpp"

#include <algorithm>
#include <cmath>

namespace cascade::dsp {

namespace {

constexpr double kPi = 3.1415926535897932384626433832795;
constexpr double kTwoPi = 2.0 * kPi;

// --- Constants taken from the RDS/RBDS broadcast standard --------------------
//
// Subcarrier and bit rate are locked to each other by the standard: the bit
// rate is exactly the subcarrier divided by 48 (57000/48 = 1187.5), which is
// why a transmitter can derive both from the same 19 kHz pilot. Writing the
// division out rather than the literal makes that relationship checkable.
constexpr double kSubcarrierHz = 57000.0;
constexpr double kBitRateHz = kSubcarrierHz / 48.0;  // 1187.5 bit/s
constexpr double kHalfBitRateHz = 2.0 * kBitRateHz;  // 2375 half-bits/s

// Checkword generator polynomial g(x) = x^10 + x^8 + x^7 + x^5 + x^4 + x^3 + 1,
// written as its coefficient vector from x^10 down to x^0.
constexpr std::uint32_t kGenerator = 0x5B9u;  // 1 0110 1110 01

// Offset words, as the standard tabulates them (bits d9..d0). These five
// 10-bit constants are the one part of the block layer that cannot be derived
// from anything else — they are an arbitrary choice made by the standard, whose
// only job is to make the four block positions distinguishable while keeping
// the code's burst-error properties. They are reproduced here from the
// published standard; if a future reader has the document to hand, these are
// the numbers to check first, because a wrong offset word produces a decoder
// that never syncs rather than one that is subtly wrong.
constexpr std::uint16_t kOffsetA = 0x0FCu;       // 00 1111 1100
constexpr std::uint16_t kOffsetB = 0x198u;       // 01 1001 1000
constexpr std::uint16_t kOffsetC = 0x168u;       // 01 0110 1000
constexpr std::uint16_t kOffsetCPrime = 0x350u;  // 11 0101 0000
constexpr std::uint16_t kOffsetD = 0x1B4u;       // 01 1011 0100

// --- Front-end design --------------------------------------------------------
//
// Baseband rate target. Eight samples per half-bit is enough for the
// zero-crossing interpolator to place a transition to a small fraction of a
// half-bit and for the integrate-and-dump to be a decent approximation of the
// continuous integral, while keeping the decimation factor large enough that
// the (long) channel filter runs at a fraction of the composite rate.
constexpr double kBasebandTargetHz = 19000.0;

// Channel filter. RDS occupies 57 +/- 2.4 kHz, so the passband must reach
// 2.4 kHz at baseband. The nearest strong neighbour is the top of the stereo
// difference subcarrier at 53 kHz, i.e. 4 kHz away at baseband, so the
// stopband must start there. A windowed-sinc's transition is centred on its
// -6 dB point, hence cutoff 3.2 kHz with a 1.6 kHz transition: passband edge
// 2.4 kHz, stopband edge 4.0 kHz. Hamming gives ~53 dB there, which puts a
// stereo subcarrier 20 dB louder than RDS more than 30 dB below it.
constexpr double kLpCutoffHz = 3200.0;
constexpr double kLpTransitionHz = 1600.0;

// --- Loop constants ----------------------------------------------------------
//
// Carrier loop natural frequency. Small, because the only thing it has to
// track is the receiver's own tuning error (the transmitter's subcarrier is
// crystal-exact), and a narrow loop keeps the data modulation itself from
// feeding through the sin(2*phi) discriminator as phase noise.
constexpr double kCarrierLoopHz = 25.0;
constexpr double kCarrierZeta = 0.70710678118654752440;  // critically damped-ish
// Hard limit on the loop's frequency integrator, expressed in Hz of subcarrier
// error. Bounds the damage a burst of noise can do: without it the integrator
// can walk away during a fade and take a long time to walk back.
constexpr double kCarrierPullHz = 150.0;
// Averaging length for the power estimate that normalises both discriminators.
constexpr double kPowerTauSamples = 400.0;

// Timing loop. Gains are per zero crossing (roughly 1.5 per bit for random
// data). Kp is the fraction of the measured phase error corrected immediately;
// Ki the fraction applied to the clock rate, expressed relative to the nominal
// step. The rate is clamped to +/-2% because the transmitter's bit clock is
// derived from the subcarrier and the only real error is the receiver's own
// sample clock, which is a parts-per-million effect.
constexpr double kTimingKp = 0.05;
constexpr double kTimingKiFrac = 0.0015;
constexpr double kTimingRateTol = 0.02;
// Minimum crossing "strength" (sum of the two straddling magnitudes) relative
// to the RMS level, below which a sign change is treated as noise rather than
// a symbol transition.
constexpr double kTedGate = 0.2;

// Biphase pairing metric: leak per update, and the margin by which the other
// pairing must beat the current one before switching. The true ratio between
// the two pairings is 2:1, so 1.4 is comfortably inside it while still being
// far enough above 1 to stop the choice dithering during acquisition.
constexpr double kPairLeak = 0.99;  // ~100-bit memory
constexpr double kAlignHysteresis = 1.4;

// Blocks that must agree with the expected offset-word sequence, after the
// first candidate, before sync is declared. Each block is a 1-in-1024 event on
// random data, so three in a row makes a false lock a ~1e-8 affair.
constexpr std::uint32_t kVerifyBlocks = 2;
// Consecutive failed blocks that drop sync. Two groups' worth: long enough to
// ride out a multipath fade, short enough that a mistuned receiver re-hunts
// promptly.
constexpr std::uint32_t kMaxConsecutiveBad = 8;

constexpr std::uint32_t kBlockBits = 26;
constexpr std::uint32_t kBlockMask = 0x3FFFFFFu;  // 26 bits
constexpr std::uint16_t kCheckMask = 0x3FFu;      // 10 bits

// Remainder of x^10 * data(x) modulo the generator, by explicit polynomial
// long division. That product-then-divide form is exactly how the standard
// defines the checkword, so the code follows the definition rather than an
// equivalent table or shift-register trick that would need its own proof.
std::uint16_t rdsCrc(std::uint16_t data) {
    std::uint32_t reg = static_cast<std::uint32_t>(data) << 10;
    for (int bit = 25; bit >= 10; --bit) {
        if (((reg >> bit) & 1u) != 0u) {
            reg ^= (kGenerator << (bit - 10));
        }
    }
    return static_cast<std::uint16_t>(reg & kCheckMask);
}

// The offset word a received block carries, assuming it is error free:
// checkword = CRC(data) + offset (modulo 2), so offset = checkword XOR CRC.
std::uint16_t blockOffsetWord(std::uint32_t reg) {
    const std::uint16_t data = static_cast<std::uint16_t>((reg >> 10) & 0xFFFFu);
    const std::uint16_t check = static_cast<std::uint16_t>(reg & kCheckMask);
    return static_cast<std::uint16_t>(check ^ rdsCrc(data));
}

// Block position 0..3 implied by an offset word, or -1 for "not an offset
// word". Position 2 accepts both C and C': the standard uses C' in place of C
// in version-B groups, where block C carries a repeat of the PI code rather
// than group-specific data.
int indexForOffset(std::uint16_t off) {
    if (off == kOffsetA) { return 0; }
    if (off == kOffsetB) { return 1; }
    if (off == kOffsetC || off == kOffsetCPrime) { return 2; }
    if (off == kOffsetD) { return 3; }
    return -1;
}

bool offsetMatchesIndex(std::uint16_t off, int index) {
    switch (index) {
        case 0: return off == kOffsetA;
        case 1: return off == kOffsetB;
        case 2: return off == kOffsetC || off == kOffsetCPrime;
        case 3: return off == kOffsetD;
        default: return false;
    }
}

// RDS transmits its own G0 character set, which coincides with ASCII over
// 0x20..0x7E (the range every real station uses for PS and RadioText). Control
// codes have no display meaning here and would corrupt a UI string, so they
// become spaces; 0x80 and above are passed through for a caller that wants to
// apply the full G0/G1/G2 mapping itself.
char sanitizeChar(std::uint8_t b) {
    if (b < 0x20u || b == 0x7Fu) { return ' '; }
    return static_cast<char>(b);
}

std::size_t chooseDecimation(double fs) {
    const double d = std::floor(fs / kBasebandTargetHz);
    if (!(d >= 1.0)) { return 1; }
    return static_cast<std::size_t>(d);
}

std::vector<float> designChannelTaps(double fs) {
    // Hamming windowed-sinc transition width is about 3.3/N of the sample
    // rate; invert that for the tap count. Floor keeps a usable filter at low
    // composite rates, cap bounds the cost at high ones (the taps are only
    // evaluated on the decimation grid, so even the cap is cheap per input).
    double taps = std::ceil(3.3 * fs / kLpTransitionHz);
    if (!(taps >= 63.0)) { taps = 63.0; }
    if (taps > 4095.0) { taps = 4095.0; }
    std::size_t n = static_cast<std::size_t>(taps);
    if ((n & 1u) == 0u) { ++n; }
    double cutoff = kLpCutoffHz / fs;
    // Keep the design inside windowedSincLowpass's (0, 0.5) precondition even
    // for a nonsensical rate; such a decoder cannot sync anyway, but it must
    // not trip an assertion deep in the filter designer.
    cutoff = std::clamp(cutoff, 1.0e-6, 0.45);
    return windowedSincLowpass(n, cutoff, WindowType::Hamming);
}

}  // namespace

RdsDecoder::RdsDecoder(double compositeRateHz)
    : compositeRate_(compositeRateHz),
      lowpass_(designChannelTaps(compositeRateHz),
               chooseDecimation(compositeRateHz)) {
    basebandRate_ = compositeRate_ / static_cast<double>(lowpass_.decimation());

    // Mix the subcarrier down to DC. The composite is real, so this also
    // produces an image at -114 kHz; the channel filter removes it along with
    // everything else in the multiplex.
    mixer_.setFrequency(-kSubcarrierHz / compositeRate_);

    // Standard second-order digital PLL gains for a given natural frequency
    // and damping: proportional 2*zeta*wn, integral wn^2, with wn in radians
    // per sample.
    const double wn = kTwoPi * kCarrierLoopHz / basebandRate_;
    carrierKp_ = 2.0 * kCarrierZeta * wn;
    carrierKi_ = wn * wn;
    carrierFreqLimit_ = kTwoPi * kCarrierPullHz / basebandRate_;

    timingStepNom_ = kHalfBitRateHz / basebandRate_;

    reset();
}

void RdsDecoder::reset() {
    mixer_.reset();
    lowpass_.reset();
    mixBuf_.clear();
    bbBuf_.clear();

    carrierPhase_ = 0.0;
    carrierFreq_ = 0.0;
    power_ = 0.0;
    powerCount_ = 0.0;

    timingPhase_ = 0.0;
    timingStep_ = timingStepNom_;
    timingNudge_ = 0.0;
    prevSample_ = 0.0;
    havePrevSample_ = false;
    intAcc_ = 0.0;
    intWeight_ = 0.0;

    prevHalf_ = 0.0;
    haveHalf_ = false;
    halfIndex_ = 0;
    pairMetric_[0] = 0.0;
    pairMetric_[1] = 0.0;
    alignment_ = 0;

    prevBiphase_ = false;
    havePrevBiphase_ = false;

    shift_ = 0;
    bitsInReg_ = 0;
    sync_ = SyncState::Hunt;
    bitsToBlock_ = 0;
    verifyLeft_ = 0;
    expectIndex_ = 0;
    blockIndex_ = 0;
    consecutiveBad_ = 0;

    clearGroup();

    for (std::size_t i = 0; i < kPsLen; ++i) { psBuf_[i] = ' '; }
    psMask_ = 0;
    clearRadioText();
    rtAbValid_ = false;
    rtAb_ = false;

    state_ = RdsState{};
}

std::size_t RdsDecoder::process(const float* mpx, std::size_t n) {
    if (mpx == nullptr || n == 0) { return 0; }

    // Fixed internal chunking so the scratch buffers stay small and cache
    // friendly regardless of how large a block the caller hands over. Every
    // stage carries state, so the chunk boundaries are invisible in the result.
    constexpr std::size_t kChunk = 2048;
    std::size_t done = 0;
    while (done < n) {
        const std::size_t m = std::min(kChunk, n - done);
        mixBuf_.resize(m);
        for (std::size_t i = 0; i < m; ++i) {
            mixBuf_[i] = std::complex<float>(mpx[done + i], 0.0f);
        }
        mixer_.mix(mixBuf_.data(), mixBuf_.data(), m);

        bbBuf_.resize(lowpass_.outputCapacity(m));
        const std::size_t produced =
            lowpass_.process(mixBuf_.data(), m, bbBuf_.data());
        for (std::size_t j = 0; j < produced; ++j) { processBaseband(bbBuf_[j]); }
        done += m;
    }
    return n;
}

void RdsDecoder::processBaseband(std::complex<float> z) {
    const double zr = static_cast<double>(z.real());
    const double zi = static_cast<double>(z.imag());

    // Running mean power. The first kPowerTauSamples samples use an exact
    // running mean rather than an exponential one: an EMA seeded at zero would
    // divide the carrier discriminator by almost nothing and slam the loop at
    // the start of every stream.
    const double inst = zr * zr + zi * zi;
    if (powerCount_ < kPowerTauSamples) { powerCount_ += 1.0; }
    power_ += (inst - power_) / powerCount_;

    // --- carrier recovery ---------------------------------------------------
    const double cosP = std::cos(carrierPhase_);
    const double sinP = std::sin(carrierPhase_);
    const double vr = zr * cosP + zi * sinP;   // Re{z * e^{-j*phase}}
    const double vi = -zr * sinP + zi * cosP;  // Im{z * e^{-j*phase}}

    if (power_ > 1.0e-24) {
        // For z = A*e^{j*theta} with real data, 2*vr*vi/A^2 = sin(2*(theta-phase)):
        // zero at both the in-phase lock points, which is the 180-degree
        // ambiguity the differential coding exists to absorb. Clamping bounds
        // the kick a transient can deliver before the power estimate catches up.
        const double err = std::clamp(2.0 * vr * vi / power_, -1.0, 1.0);
        carrierFreq_ = std::clamp(carrierFreq_ + carrierKi_ * err,
                                  -carrierFreqLimit_, carrierFreqLimit_);
        carrierPhase_ += carrierFreq_ + carrierKp_ * err;
        while (carrierPhase_ > kPi) { carrierPhase_ -= kTwoPi; }
        while (carrierPhase_ < -kPi) { carrierPhase_ += kTwoPi; }
    }

    // After de-rotation the data lives entirely in the real part.
    const double r = vr;

    // --- symbol timing: zero-crossing discriminator -------------------------
    if (havePrevSample_) {
        const bool prevNeg = prevSample_ < 0.0;
        const bool curNeg = r < 0.0;
        if (prevNeg != curNeg) {
            const double a = std::fabs(prevSample_);
            const double b = std::fabs(r);
            const double sum = a + b;
            if (sum > kTedGate * std::sqrt(power_)) {
                // Linear interpolation places the crossing between the two
                // samples; its distance from the nearest half-bit boundary,
                // signed and wrapped to +/-half a symbol, is the phase error.
                const double frac = a / sum;
                double ph = timingPhase_ - (1.0 - frac) * timingStep_;
                ph -= std::floor(ph);
                const double err = (ph >= 0.5) ? ph - 1.0 : ph;

                // The proportional correction is applied as a one-shot change
                // to the NEXT accumulator advance rather than as a jump in the
                // phase itself. A jump can push the phase backwards past zero
                // and dump a nearly empty integration window; folding it into
                // the step cannot, as long as it stays below the step, which
                // the clamp guarantees.
                timingNudge_ = std::clamp(-kTimingKp * err, -0.4 * timingStep_,
                                          0.4 * timingStep_);
                timingStep_ -= kTimingKiFrac * timingStepNom_ * err;
                timingStep_ =
                    std::clamp(timingStep_, timingStepNom_ * (1.0 - kTimingRateTol),
                               timingStepNom_ * (1.0 + kTimingRateTol));
            }
        }
    }
    prevSample_ = r;
    havePrevSample_ = true;

    // --- integrate and dump over one half-bit -------------------------------
    const double step = timingStep_ + timingNudge_;
    timingNudge_ = 0.0;
    const double after = timingPhase_ + step;
    if (after >= 1.0) {
        // The boundary falls inside this sample's interval: split the sample
        // between the two half-bits so the window length is exact to a
        // fraction of a sample rather than rounded to the sample grid.
        const double f = (1.0 - timingPhase_) / step;
        intAcc_ += f * r;
        intWeight_ += f;
        const double h = (intWeight_ > 1.0e-12) ? intAcc_ / intWeight_ : 0.0;
        intAcc_ = (1.0 - f) * r;
        intWeight_ = 1.0 - f;
        timingPhase_ = after - 1.0;
        pushHalfBit(h);
    } else {
        intAcc_ += r;
        intWeight_ += 1.0;
        timingPhase_ = after;
    }
}

void RdsDecoder::pushHalfBit(double h) {
    const std::uint32_t idx = halfIndex_++;
    if (!haveHalf_) {
        prevHalf_ = h;
        haveHalf_ = true;
        return;
    }

    // A biphase symbol is (+e, -e) or (-e, +e), so the difference of the two
    // half-bits is a full-swing +/-2e for the correct pairing. For the pairing
    // shifted by one half-bit the difference straddles two symbols and is zero
    // whenever they happen to be equal, i.e. half the time on random data:
    // mean square 2e^2 against 4e^2. Tracking both and taking the larger, with
    // hysteresis, recovers the bit boundary without needing any preamble.
    const double d = prevHalf_ - h;
    prevHalf_ = h;

    const std::size_t par = static_cast<std::size_t>((idx - 1u) & 1u);
    pairMetric_[par] = kPairLeak * pairMetric_[par] + d * d;

    const std::size_t alt = 1u - alignment_;
    if (pairMetric_[alt] > kAlignHysteresis * pairMetric_[alignment_]) {
        alignment_ = alt;
    }
    if (par == alignment_) { pushBiphaseBit(d > 0.0); }
}

void RdsDecoder::pushBiphaseBit(bool t) {
    // Differential decode: the transmitter sent t[k] = t[k-1] XOR d[k]. The
    // XOR of two consecutive received symbols is therefore the data bit, and
    // is unchanged if every symbol is inverted — which is exactly what the
    // carrier loop's 180-degree ambiguity does.
    if (havePrevBiphase_) { pushDataBit(t != prevBiphase_); }
    prevBiphase_ = t;
    havePrevBiphase_ = true;
}

void RdsDecoder::pushDataBit(bool b) {
    shift_ = ((shift_ << 1) | (b ? 1u : 0u)) & kBlockMask;
    if (bitsInReg_ < kBlockBits) { ++bitsInReg_; }

    switch (sync_) {
        case SyncState::Hunt:
            if (bitsInReg_ >= kBlockBits) { huntStep(); }
            break;

        case SyncState::Verify:
            if (--bitsToBlock_ == 0) {
                if (offsetMatchesIndex(blockOffsetWord(shift_), expectIndex_)) {
                    if (--verifyLeft_ == 0) {
                        sync_ = SyncState::Locked;
                        consecutiveBad_ = 0;
                        clearGroup();
                        blockIndex_ = expectIndex_;
                        acceptBlock(true);
                    } else {
                        expectIndex_ = (expectIndex_ + 1) & 3;
                        bitsToBlock_ = kBlockBits;
                    }
                } else {
                    // A failed candidate must not cost the 26 bits it already
                    // spent AND the current window: re-run the hunt on this
                    // same bit so a genuine block boundary sitting here is
                    // still found.
                    sync_ = SyncState::Hunt;
                    huntStep();
                }
            }
            break;

        case SyncState::Locked:
            if (--bitsToBlock_ == 0) {
                const bool ok =
                    offsetMatchesIndex(blockOffsetWord(shift_), blockIndex_);
                if (ok) {
                    consecutiveBad_ = 0;
                } else {
                    ++state_.blockErrors;
                    if (++consecutiveBad_ >= kMaxConsecutiveBad) {
                        sync_ = SyncState::Hunt;
                        clearGroup();
                        break;
                    }
                }
                acceptBlock(ok);
            }
            break;
    }
}

void RdsDecoder::huntStep() {
    const int index = indexForOffset(blockOffsetWord(shift_));
    if (index < 0) { return; }
    // A single match is only a candidate: on random data one turns up about
    // every 200 bits. Confirmation is kVerifyBlocks further blocks whose
    // offsets continue the A-B-C/C'-D cycle.
    expectIndex_ = (index + 1) & 3;
    verifyLeft_ = kVerifyBlocks;
    bitsToBlock_ = kBlockBits;
    sync_ = SyncState::Verify;
}

void RdsDecoder::acceptBlock(bool ok) {
    blockData_[blockIndex_] = static_cast<std::uint16_t>((shift_ >> 10) & 0xFFFFu);
    blockOk_[blockIndex_] = ok;
    if (blockIndex_ == 3) {
        decodeGroup();
        clearGroup();
        blockIndex_ = 0;
    } else {
        ++blockIndex_;
    }
    bitsToBlock_ = kBlockBits;
}

void RdsDecoder::clearGroup() {
    for (std::size_t i = 0; i < 4; ++i) {
        blockData_[i] = 0;
        blockOk_[i] = false;
    }
}

void RdsDecoder::decodeGroup() {
    // Block A is the PI code in every group type.
    if (blockOk_[0]) {
        state_.pi = blockData_[0];
        state_.piValid = true;
    }

    // Without block B the group type is unknown, so nothing else in the group
    // can be interpreted — not even which fields blocks C and D hold.
    if (!blockOk_[1]) { return; }
    ++state_.groupsDecoded;

    const std::uint16_t b = blockData_[1];
    const unsigned type = (b >> 12) & 0xFu;
    const bool versionB = ((b >> 11) & 1u) != 0u;
    state_.tp = ((b >> 10) & 1u) != 0u;
    state_.pty = static_cast<std::uint8_t>((b >> 5) & 0x1Fu);

    // In a version-B group block C repeats the PI code (that is what the C'
    // offset word signals), so it is a second chance at PI on a station whose
    // block A keeps getting hit.
    if (versionB && blockOk_[2]) {
        state_.pi = blockData_[2];
        state_.piValid = true;
    }

    if (type == 0u) {
        // Group 0A/0B: Programme Service name, two characters per group,
        // plus the traffic-announcement flag.
        state_.ta = ((b >> 4) & 1u) != 0u;
        const std::size_t addr = static_cast<std::size_t>(b & 0x3u);
        if (blockOk_[3]) {
            const std::size_t pos = 2u * addr;
            // Two bits of address can only ever select 0..3, but a decoder
            // must never be one corrupt field away from an out-of-range write.
            if (pos + 1 < kPsLen) {
                psBuf_[pos] =
                    sanitizeChar(static_cast<std::uint8_t>(blockData_[3] >> 8));
                psBuf_[pos + 1] =
                    sanitizeChar(static_cast<std::uint8_t>(blockData_[3] & 0xFFu));
                psMask_ = static_cast<std::uint8_t>(psMask_ | (1u << addr));
                updatePs();
            }
        }
    } else if (type == 2u) {
        // Group 2A/2B: RadioText. 2A carries four characters (blocks C and D),
        // 2B carries two (block D only, because C is the PI repeat) and is
        // therefore limited to a 32-character message.
        const bool ab = ((b >> 4) & 1u) != 0u;
        const std::size_t addr = static_cast<std::size_t>(b & 0xFu);
        if (!rtAbValid_ || ab != rtAb_) {
            // The A/B flag toggling is the standard's "this is a new message"
            // signal. Without clearing, a short new message leaves the tail of
            // the old one visible.
            clearRadioText();
            rtAb_ = ab;
            rtAbValid_ = true;
        }
        if (!versionB) {
            const std::size_t pos = 4u * addr;
            if (blockOk_[2]) {
                setRtChar(pos, static_cast<std::uint8_t>(blockData_[2] >> 8));
                setRtChar(pos + 1,
                          static_cast<std::uint8_t>(blockData_[2] & 0xFFu));
            }
            if (blockOk_[3]) {
                setRtChar(pos + 2, static_cast<std::uint8_t>(blockData_[3] >> 8));
                setRtChar(pos + 3,
                          static_cast<std::uint8_t>(blockData_[3] & 0xFFu));
            }
        } else if (blockOk_[3]) {
            const std::size_t pos = 2u * addr;
            setRtChar(pos, static_cast<std::uint8_t>(blockData_[3] >> 8));
            setRtChar(pos + 1, static_cast<std::uint8_t>(blockData_[3] & 0xFFu));
        }
        updateRadioText();
    }
}

void RdsDecoder::setRtChar(std::size_t pos, std::uint8_t byte) {
    // Four address bits cover 0..15, so 2A reaches position 63 and 2B position
    // 31 — both in range by construction. The guard is here so that no future
    // group type, and no corrupt field that slipped past a checkword, can turn
    // a decode into an out-of-bounds write.
    if (pos >= kRtLen) { return; }
    rtBuf_[pos] = static_cast<char>(byte);
    rtMask_ |= (std::uint64_t{1} << pos);
}

void RdsDecoder::clearRadioText() {
    for (std::size_t i = 0; i < kRtLen; ++i) { rtBuf_[i] = ' '; }
    rtMask_ = 0;
    state_.radioText.clear();
}

void RdsDecoder::updatePs() {
    // All four segments, or nothing: see the RdsState comment.
    if (psMask_ != 0x0Fu) { return; }
    state_.ps.assign(psBuf_, kPsLen);
    state_.psValid = true;
}

void RdsDecoder::updateRadioText() {
    // Publish the contiguous prefix that has actually arrived. Stopping at the
    // first unreceived position rather than filling holes with spaces means a
    // caller never sees characters at the wrong index while a message streams
    // in out of order.
    std::string text;
    text.reserve(kRtLen);
    for (std::size_t i = 0; i < kRtLen; ++i) {
        if (((rtMask_ >> i) & std::uint64_t{1}) == 0u) { break; }
        const std::uint8_t raw = static_cast<std::uint8_t>(rtBuf_[i]);
        if (raw == 0x0Du) { break; }  // standard end-of-message terminator
        text.push_back(sanitizeChar(raw));
    }
    while (!text.empty() && text.back() == ' ') { text.pop_back(); }
    state_.radioText = text;
}

}  // namespace cascade::dsp
