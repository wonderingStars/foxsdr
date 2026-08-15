// RDS (Radio Data System) decoder: recovers the 1187.5 bit/s data channel that
// broadcast FM carries on a 57 kHz subcarrier, straight from the WFM composite
// (MPX) signal.
//
// CLEAN ROOM: every constant and every stage below was written from the
// published RDS/RBDS broadcast standard (subcarrier frequency, bit rate,
// differential + biphase line coding, the shortened cyclic (26,16) block code
// with its five offset words, and the group 0/2 semantics). No third-party RDS
// implementation of any kind was read while writing this file.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "dsp/fir.hpp"
#include "dsp/nco.hpp"

namespace cascade::dsp {

// Everything the decoder has recovered so far, as a value snapshot.
//
// Field lifetimes are deliberately conservative: a text field stays EMPTY until
// the checkword-validated data that fills it has actually arrived, so a caller
// that renders state() every frame never displays half-formed garbage from a
// station it is not really receiving.
struct RdsState {
    // Programme Identification: the 16-bit station code from block A (or from
    // block C of a version-B group, which repeats it). Valid the first time a
    // block A passes its checkword.
    bool piValid = false;
    std::uint16_t pi = 0;

    // Programme Service name: exactly 8 characters, space padded as
    // transmitted (the caller may trim). Empty, with psValid false, until all
    // four two-character segments have been received; PS is sent as four
    // fragments and showing a partially filled buffer is the classic RDS
    // display bug.
    bool psValid = false;
    std::string ps;

    // RadioText, up to 64 characters (group 2A) or 32 (group 2B). Contains the
    // contiguous run of characters received so far, cut at the standard 0x0D
    // terminator and with trailing spaces removed. Cleared whenever the text
    // A/B flag toggles, which is how the standard signals "new message".
    std::string radioText;

    // Traffic Programme (station carries traffic info at all) and Traffic
    // Announcement (one is on the air right now). TP rides in every group's
    // block B; TA only in group 0.
    bool tp = false;
    bool ta = false;

    // Programme TYpe code, 5 bits, from block B of any group. The code-to-name
    // table differs between RDS (Europe) and RBDS (North America), so the raw
    // code is what gets exposed here.
    std::uint8_t pty = 0;

    // Counters, monotonically increasing until reset(). groupsDecoded counts
    // complete four-block frames received while synchronised whose block B
    // passed its checkword — i.e. groups whose type was known and which could
    // therefore be interpreted. blockErrors counts individual blocks that
    // failed their checkword while synchronised.
    std::uint32_t groupsDecoded = 0;
    std::uint32_t blockErrors = 0;
};

// Standalone RDS receiver: composite (MPX) samples in, decoded state out.
//
// Signal chain, and why each stage is what it is:
//
//  1. Subcarrier isolation. The RDS signal is a double-sideband suppressed
//     carrier AM of a 57 kHz subcarrier, occupying roughly 57 +/- 2.4 kHz. It
//     is mixed to baseband by a -57 kHz NCO and low-pass filtered. The filter
//     has to be reasonably sharp: the stereo difference subcarrier reaches up
//     to 53 kHz, only 1.6 kHz below the bottom of the RDS band, and it is
//     typically 20 dB stronger. The filtered stream is then decimated to about
//     19 kHz (~8 samples per half-bit), which is where all the loops run.
//
//  2. Carrier recovery. The 57 kHz subcarrier is the third harmonic of the
//     19 kHz stereo pilot, so a receiver that already tracks the pilot can
//     derive it. This decoder is standalone and is handed the composite with
//     no pilot reference of its own, and a mono station has no pilot at all,
//     so it recovers the carrier from the RDS signal itself with a Costas /
//     squaring loop: for a real (BPSK-like) baseband the product
//     Re{v}*Im{v} is proportional to sin(2*phi), which is the error of a
//     second-order (PI) phase-locked loop. The error is normalised by the
//     running mean power so loop bandwidth is independent of signal level.
//     A squaring loop locks with a 180-degree ambiguity; that is precisely why
//     the standard differentially encodes the data, and stage 5 undoes it.
//
//  3. Symbol timing. Biphase (Manchester) coding guarantees a transition at
//     every half-bit boundary that carries one, and permits transitions only
//     there, so every zero crossing of the recovered baseband is a timing
//     reference for a 2375 Hz (half-bit) clock. A zero-crossing detector with
//     linear interpolation drives a second-order timing loop; the loop's phase
//     accumulator also gates an integrate-and-dump that averages each half-bit,
//     with the boundary sample split fractionally so the integration window
//     does not have to be an integer number of samples.
//
//  4. Biphase decision. A data bit spans two half-bits with opposite signs, so
//     bit = (h[first] - h[second]) > 0. Which of the two possible pairings of
//     the half-bit stream is the right one is resolved by energy: the correct
//     pairing always sees a full-swing difference, the half-bit-shifted one
//     sees zero half the time, a 2:1 ratio in mean square that a leaky
//     integrator separates in well under one group.
//
//  5. Differential decode. The transmitter sends t[k] = t[k-1] XOR d[k], so
//     d[k] = t[k] XOR t[k-1] — which is invariant to the global inversion the
//     carrier loop's 180-degree ambiguity can introduce.
//
//  6. Block synchronisation and group assembly. The data stream is a
//     continuous run of 26-bit blocks: 16 information bits followed by a
//     10-bit checkword, where the checkword is the CRC of the information
//     word (generator x^10+x^8+x^7+x^5+x^4+x^3+1) added modulo 2 to a
//     block-position-specific offset word (A, B, C, C', D). Recovering
//     "checkword XOR CRC(data)" therefore yields the offset word directly,
//     which both validates the block and identifies its position in the group.
//     Sync is declared only after three consecutive blocks whose offset words
//     follow the A-B-C/C'-D cycle, which makes a false lock on noise
//     vanishingly unlikely.
//
// Not integrated into the pipeline or the GUI: this object is a plain DSP
// block like every other one here, and nothing in it touches a thread.
class RdsDecoder {
public:
    // compositeRateHz is the sample rate of the WFM composite/MPX stream. The
    // 57 kHz subcarrier must be below Nyquist with room for its sidebands, so
    // rates below about 120 kHz cannot work; construction still succeeds (the
    // object simply never syncs) rather than throwing at a caller who tuned to
    // a narrow channel.
    explicit RdsDecoder(double compositeRateHz);

    // Consumes n real composite samples. Returns the number consumed, which is
    // always n — the decoder buffers internally and never asks the caller to
    // re-present samples. Any block split of a stream produces the same result
    // as one call: every stage keeps its history across calls.
    std::size_t process(const float* mpx, std::size_t n);

    // Returns the object to its just-constructed condition: all loops, all
    // filter history, all sync state and all decoded content are discarded.
    // The sample rate (and therefore the filter design) is retained.
    void reset();

    // True once block synchronisation has been acquired and not since lost.
    bool synced() const noexcept { return sync_ == SyncState::Locked; }

    // Snapshot of the decoded content. Cheap: the strings are maintained as
    // groups arrive, not rebuilt here.
    RdsState state() const { return state_; }

private:
    enum class SyncState { Hunt, Verify, Locked };

    static constexpr std::size_t kPsLen = 8;
    static constexpr std::size_t kRtLen = 64;

    void processBaseband(std::complex<float> z);
    void pushHalfBit(double h);
    void pushBiphaseBit(bool t);
    void pushDataBit(bool b);
    void huntStep();
    void acceptBlock(bool ok);
    void decodeGroup();
    void clearGroup();
    void clearRadioText();
    void setRtChar(std::size_t pos, std::uint8_t byte);
    void updatePs();
    void updateRadioText();

    // --- front end ----------------------------------------------------------
    double compositeRate_ = 0.0;
    double basebandRate_ = 0.0;
    Nco mixer_;
    FirDecimator lowpass_;
    std::vector<std::complex<float>> mixBuf_;  // real->complex + mixed block
    std::vector<std::complex<float>> bbBuf_;   // decimated baseband block

    // --- carrier recovery ---------------------------------------------------
    double carrierPhase_ = 0.0;  // radians, wrapped to [-pi, pi)
    double carrierFreq_ = 0.0;   // radians/sample, the loop's integrator
    double carrierKp_ = 0.0;
    double carrierKi_ = 0.0;
    double carrierFreqLimit_ = 0.0;
    double power_ = 0.0;       // running mean |z|^2, normalises both loops
    double powerCount_ = 0.0;  // exact-mean warm-up counter

    // --- symbol timing ------------------------------------------------------
    double timingPhase_ = 0.0;    // position within the half-bit, in [0, 1)
    double timingStep_ = 0.0;     // half-bit cycles per baseband sample
    double timingStepNom_ = 0.0;  // nominal value of the above
    double timingNudge_ = 0.0;    // one-shot proportional correction
    double prevSample_ = 0.0;
    bool havePrevSample_ = false;
    double intAcc_ = 0.0;     // integrate-and-dump accumulator
    double intWeight_ = 0.0;  // and its (fractional) sample count

    // --- biphase pairing ----------------------------------------------------
    double prevHalf_ = 0.0;
    bool haveHalf_ = false;
    std::uint32_t halfIndex_ = 0;
    double pairMetric_[2] = {0.0, 0.0};
    std::size_t alignment_ = 0;

    // --- differential decode ------------------------------------------------
    bool prevBiphase_ = false;
    bool havePrevBiphase_ = false;

    // --- block synchronisation ----------------------------------------------
    std::uint32_t shift_ = 0;      // last 26 data bits, newest in bit 0
    std::uint32_t bitsInReg_ = 0;  // saturates at 26
    SyncState sync_ = SyncState::Hunt;
    std::uint32_t bitsToBlock_ = 0;
    std::uint32_t verifyLeft_ = 0;
    int expectIndex_ = 0;  // block position expected next, 0..3
    int blockIndex_ = 0;   // block position currently being filled, 0..3
    std::uint32_t consecutiveBad_ = 0;

    // --- group assembly -----------------------------------------------------
    std::uint16_t blockData_[4] = {0, 0, 0, 0};
    bool blockOk_[4] = {false, false, false, false};

    // --- decoded content ----------------------------------------------------
    char psBuf_[kPsLen] = {};
    std::uint8_t psMask_ = 0;  // one bit per two-character PS segment
    char rtBuf_[kRtLen] = {};
    std::uint64_t rtMask_ = 0;  // one bit per RadioText character position
    bool rtAbValid_ = false;
    bool rtAb_ = false;

    RdsState state_;
};

}  // namespace cascade::dsp
