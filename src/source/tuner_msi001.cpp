// The MSi001's tables and arithmetic. See tuner_msi001.hpp for where every
// number came from and for the licence posture; this file is only the bodies.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/tuner_msi001.hpp"

namespace cascade::source::msi001 {

namespace {

// Register 0's field positions. Named rather than written inline because the
// register is assembled from seven of them and a shift written twice is a
// shift that can be written twice differently.
constexpr int kReg0ModeShift = 4;
constexpr int kReg0UpconvertShift = 9;
constexpr int kReg0SynthOnShift = 10;
constexpr int kReg0AmPortShift = 11;
constexpr int kReg0IfModeShift = 12;
constexpr int kReg0BandwidthShift = 14;
constexpr int kReg0XtalShift = 17;
constexpr int kReg0IfLowPowerShift = 20;
constexpr int kReg0VcoLowPowerShift = 23;

// Register 1's, the gain register.
constexpr int kReg1BasebandShift = 4;
constexpr int kReg1MixbufferShift = 10;
constexpr int kReg1MixerShift = 12;
constexpr int kReg1LnaShift = 13;
constexpr int kReg1DcCalShift = 14;
constexpr int kReg1DcCalSpeedupShift = 17;

// The DC-offset calibration mode the reference leaves the tuner in: the third
// of its periodic settings, with the speed-up off. A zero-IF receiver has to
// track its own DC offset continuously or the carrier it is tuned to sits on
// top of its own leakage - which is exactly the fault the ADS-B decoder was
// deaf to on an RTL-SDR (see the plugin 1.6.0 note) - so this is not a detail
// to leave at whatever the chip powers up with.
constexpr std::uint32_t kDcCalPeriodic3 = 2;
constexpr std::uint32_t kDcCalSpeedupOff = 0;

// Register 6, the calibration timing: a tracking time in bits 4-7 and a rate
// in bits 8-21. Both are the reference's constants and neither depends on
// anything this driver knows, which is why the whole register is one value.
constexpr std::uint32_t kDcCalTrackTime = 0x1F;
constexpr std::uint32_t kDcCalRate = 0x800;

constexpr std::uint32_t kRfSynthesiserOn = 1;
constexpr std::uint32_t kIfLowPowerNormal = 0;
constexpr std::uint32_t kVcoLowPowerNormal = 0;

// The reserved field register 5 must carry. The reference says only that it is
// reserved and must be this, and a reserved field is exactly the kind of thing
// to copy faithfully rather than reason about.
constexpr std::uint32_t kSynthReserved = 0x28;

// The AM rows mix up by five times the 24 MHz reference before they tune, so
// the synthesiser is programmed for the sum.
constexpr std::uint64_t kUpconvertOffsetHz = 120000000ULL;

}  // namespace

const char* bandName(Band b) {
    switch (b) {
        case Band::Am1: return "AM1";
        case Band::Am2: return "AM2";
        case Band::Vhf: return "VHF";
        case Band::Band3: return "Band III";
        case Band::Band45: return "Band IV/V";
        case Band::LBand: return "L-band";
    }
    return "?";
}

const std::vector<BandPlanEntry>& planEntries(Plan p) {
    // THE ROW AT 259 MHz SETS TWO MODE BITS AT ONCE (VHF and Band III) and
    // halves the divider, which is what carries the tuner across the gap
    // between the two networks. It is kept exactly as the reference has it -
    // an entry that looks like a mistake and is not is the first thing a
    // tidying pass deletes.
    static const std::vector<BandPlanEntry> defaultPlan = {
        {0, kModeAm, true, 1, 16, 0xF780},
        {12, kModeAm, true, 1, 16, 0xFF80},
        {30, kModeAm, true, 1, 16, 0xF280},
        {50, kModeVhf, false, 0, 32, 0xF380},
        {108, kModeBand3, false, 0, 16, 0xFA80},
        {250, kModeBand3, false, 0, 16, 0xF680},
        {259, kModeVhf | kModeBand3, false, 0, 8, 0xF680},
        {330, kModeBand45, false, 0, 4, 0xF380},
        {960, kModeLBand, false, 0, 2, 0xFA80},
        {2400, -1, false, 0, 0, 0x0000},
    };
    // The same silicon behind SDRplay's input network: different edges at the
    // VHF and Band IV/V crossings, and its own switch words.
    static const std::vector<BandPlanEntry> sdrPlayPlan = {
        {0, kModeAm, true, 1, 16, 0xF580},
        {12, kModeAm, true, 1, 16, 0xF580},
        {30, kModeAm, true, 1, 16, 0xF580},
        {50, kModeVhf, false, 0, 32, 0xF180},
        {112, kModeBand3, false, 0, 16, 0xF580},
        {250, kModeBand3, false, 0, 16, 0xF480},
        {261, kModeVhf | kModeBand3, false, 0, 8, 0xF480},
        {404, kModeBand45, false, 0, 4, 0xF580},
        {1000, kModeLBand, false, 0, 2, 0xF580},
        {2400, -1, false, 0, 0, 0x0000},
    };
    return p == Plan::SdrPlay ? sdrPlayPlan : defaultPlan;
}

std::uint32_t bandwidthWord(Bandwidth b) {
    switch (b) {
        case Bandwidth::Khz200: return 0;
        case Bandwidth::Khz300: return 1;
        case Bandwidth::Khz600: return 2;
        case Bandwidth::Khz1536: return 3;
        case Bandwidth::Mhz5: return 4;
        case Bandwidth::Mhz6: return 5;
        case Bandwidth::Mhz7: return 6;
        case Bandwidth::Mhz8: return 7;
    }
    return 7;
}

double bandwidthHz(Bandwidth b) {
    switch (b) {
        case Bandwidth::Khz200: return 200.0e3;
        case Bandwidth::Khz300: return 300.0e3;
        case Bandwidth::Khz600: return 600.0e3;
        case Bandwidth::Khz1536: return 1536.0e3;
        case Bandwidth::Mhz5: return 5.0e6;
        case Bandwidth::Mhz6: return 6.0e6;
        case Bandwidth::Mhz7: return 7.0e6;
        case Bandwidth::Mhz8: return 8.0e6;
    }
    return 8.0e6;
}

Bandwidth bandwidthForRate(double sampleRateHz) {
    // The widest setting no wider than the rate itself. Not 75% of it as the
    // HackRF's MAX2837 table is walked, because these eight widths are already
    // sparse - the step from 1.536 to 5 MHz is a factor of three - and taking
    // three quarters first would leave a 2 MS/s radio on a 1.536 MHz filter
    // where it can have the whole span. Anything at or under the narrowest
    // keeps the narrowest.
    static const Bandwidth all[] = {Bandwidth::Khz200,  Bandwidth::Khz300, Bandwidth::Khz600,
                                    Bandwidth::Khz1536, Bandwidth::Mhz5,   Bandwidth::Mhz6,
                                    Bandwidth::Mhz7,    Bandwidth::Mhz8};
    Bandwidth best = Bandwidth::Khz200;
    for (const Bandwidth b : all) {
        if (bandwidthHz(b) <= sampleRateHz) { best = b; }
    }
    return best;
}

std::uint32_t ifModeWord(IfMode m) {
    // The field's values run the other way from the enumeration: 2048 kHz is
    // word 0 and zero IF is word 3.
    switch (m) {
        case IfMode::Zero: return 3;
        case IfMode::Khz450: return 2;
        case IfMode::Khz1620: return 1;
        case IfMode::Khz2048: return 0;
    }
    return 3;
}

TuneSetting computeTune(double freqHz, Plan p, Bandwidth bw, IfMode ifMode) {
    TuneSetting t;
    const std::uint64_t freq = freqHz > 0.0 ? static_cast<std::uint64_t>(freqHz + 0.5) : 0ULL;
    const std::vector<BandPlanEntry>& plan = planEntries(p);

    // WALK UP UNTIL THE ROW ABOVE IS TOO HIGH, then take the one below it -
    // which is the reference's own shape and is why the terminator row can
    // carry a negative mode: it stops the walk without ever being used.
    std::size_t i = 0;
    while (i < plan.size() &&
           freq >= 1000000ULL * static_cast<std::uint64_t>(plan[i].lowCutMHz)) {
        if (plan[i].modeBits < 0) { break; }
        ++i;
    }
    if (i == 0) { i = 1; }  // cannot happen: the first row starts at 0 Hz
    const BandPlanEntry& row = plan[i - 1];
    t.planIndex = static_cast<int>(i - 1);
    t.bandSelectWord = row.bandSelectWord;

    std::uint32_t reg0 = 0;
    std::uint64_t loDiv = 0;
    std::uint64_t offset = 0;

    if (row.modeBits == kModeAm) {
        reg0 |= static_cast<std::uint32_t>(kModeAm) << kReg0ModeShift;
        reg0 |= static_cast<std::uint32_t>(row.upconvertMixer ? 1 : 0) << kReg0UpconvertShift;
        reg0 |= static_cast<std::uint32_t>(row.amPort) << kReg0AmPortShift;
        if (row.upconvertMixer) { offset = kUpconvertOffsetHz; }
        // The AM rows all divide by 16 whatever the row says, because the AM
        // chain is one chain.
        loDiv = 16;
        t.band = row.amPort == 0 ? Band::Am1 : Band::Am2;
    } else {
        reg0 |= static_cast<std::uint32_t>(row.modeBits) << kReg0ModeShift;
        loDiv = row.loDiv;
        if (row.modeBits == kModeVhf) {
            t.band = Band::Vhf;
        } else if (row.modeBits == kModeBand45) {
            t.band = Band::Band45;
        } else if (row.modeBits == kModeLBand) {
            t.band = Band::LBand;
        } else {
            // Band III, INCLUDING the two-bit row at 259 MHz. The reference
            // leaves its own band variable untouched for that row, so whatever
            // the previous tune set survives into the gain map; classifying it
            // here costs nothing and cannot change behaviour, because the only
            // thing the gain map asks of a band is whether it is one of the
            // two AM inputs, and this row is not.
            t.band = Band::Band3;
        }
    }

    reg0 |= kRfSynthesiserOn << kReg0SynthOnShift;
    reg0 |= ifModeWord(ifMode) << kReg0IfModeShift;
    reg0 |= bandwidthWord(bw) << kReg0BandwidthShift;
    reg0 |= kXtalWord24M << kReg0XtalShift;
    reg0 |= kIfLowPowerNormal << kReg0IfLowPowerShift;
    reg0 |= kVcoLowPowerNormal << kReg0VcoLowPowerShift;

    // --- the synthesiser ---------------------------------------------------
    //
    // The VCO runs at (wanted + offset) * loDiv and is compared against a
    // 96 MHz step. What is left over is a fraction of that step, and the
    // fraction is programmed as a numerator and a DENOMINATOR rather than as a
    // fixed-point value - which is what makes the reduction below necessary
    // and what makes the trim afterwards worth having.
    // ONE GUARD THAT SHOULD BE UNREACHABLE, and is kept because of what it
    // guards against. The terminator row carries a divider of zero, and the
    // walk above can never select it - it stops ON the terminator and takes
    // the row below. But every line from here to the end of this function
    // divides by this number, so the distance between "unreachable" and an
    // integer divide by zero is one index; deliberately breaking that walk
    // while testing this file crashed the process instead of failing a check,
    // which is the difference between a defect that names itself and one that
    // does not.
    if (loDiv == 0) { loDiv = 1; }

    const std::uint64_t fvco = (freq + offset) * loDiv;
    const std::uint64_t n = fvco / 96000000ULL;
    std::uint64_t thresh = 96000000ULL / loDiv;
    std::uint64_t frac = (fvco % 96000000ULL) / loDiv;

    // Reduce by the greatest common divisor first, because a ratio that
    // reduces to small numbers can be programmed EXACTLY and needs no trim at
    // all - which is the case for every round frequency a user is likely to
    // ask for.
    std::uint64_t a = thresh;
    std::uint64_t b = frac;
    while (a != 0) {
        const std::uint64_t c = a;
        a = b % a;
        b = c;
    }
    if (b != 0) {
        thresh /= b;
        frac /= b;
    }

    // Then scale both down until the denominator fits its twelve bits,
    // rounding rather than truncating: the error this leaves is what the trim
    // below measures and corrects.
    const std::uint64_t scale = (thresh + 4094) / 4095;
    if (scale != 0) {
        thresh = (thresh + (scale / 2)) / scale;
        frac = (frac + (scale / 2)) / scale;
    }

    // WHERE THE PLL ACTUALLY LANDS with those two numbers, recomputed from
    // them rather than assumed. A rounding that went UP has put the oscillator
    // above the wanted frequency, and one step of the numerator is the only
    // correction available before the trim.
    std::uint64_t rfvco = 0;
    if (thresh != 0 && loDiv != 0) {
        rfvco = (96000000ULL * (n * thresh * 4096ULL + frac * 4096ULL)) /
                (thresh * 4096ULL * loDiv);
        if (freq + offset < rfvco && frac > 0) { --frac; }
        rfvco = (96000000ULL * (n * thresh * 4096ULL + frac * 4096ULL)) /
                (thresh * 4096ULL * loDiv);
    }
    // The remaining error, in units of the trim field. It is signed in
    // principle; in practice the step above guarantees the PLL is at or below
    // the wanted frequency, so the subtraction stays positive.
    std::uint64_t afc = 0;
    if (freq + offset > rfvco) {
        afc = ((freq + offset - rfvco) * thresh * 4096ULL * loDiv) / 96000000ULL;
    }

    t.loDiv = loDiv;
    t.offsetHz = offset;
    t.n = n;
    t.thresh = thresh;
    t.frac = frac;
    t.afc = afc;
    t.rfvcoHz = rfvco;

    // The four words. Each STARTS at its own register number - see the header.
    t.reg0 = reg0;
    t.reg3 = 3u | (static_cast<std::uint32_t>(afc & 4095ULL) << 4);
    t.reg5 = 5u | (static_cast<std::uint32_t>(thresh & 0xFFFULL) << 4) | (kSynthReserved << 16);
    t.reg2 = 2u | (static_cast<std::uint32_t>(frac & 0xFFFULL) << 4) |
             (static_cast<std::uint32_t>(n & 0x3FULL) << 16);
    return t;
}

// --- gain -----------------------------------------------------------------

GainSetting computeGain(const GainStages& g, Band band) {
    const bool am1 = band == Band::Am1;
    const bool am2 = band == Band::Am2;

    std::uint32_t reg1 = 1u;
    reg1 |= static_cast<std::uint32_t>(g.basebandReduction & 0x3F) << kReg1BasebandShift;

    // The buffer field: both bits on port 1, all-or-nothing on port 2, and
    // forced clear on every other input because the buffer is not in circuit
    // there.
    if (am1) {
        reg1 |= static_cast<std::uint32_t>(g.mixbufferReduction & 0x03) << kReg1MixbufferShift;
    } else if (am2) {
        reg1 |= static_cast<std::uint32_t>(g.mixbufferReduction == 0 ? 0 : 3)
                << kReg1MixbufferShift;
    }

    reg1 |= static_cast<std::uint32_t>(g.mixerReduction & 0x01) << kReg1MixerShift;

    // The low-noise amplifier is NOT on either AM input, so its bit is forced
    // clear there whatever the caller asked for. Silently ignoring an argument
    // would be worse if it were the only place it happened; it is not, and the
    // readback below reports what was actually programmed.
    if (!am1 && !am2) {
        reg1 |= static_cast<std::uint32_t>(g.lnaReduction & 0x01) << kReg1LnaShift;
    }

    reg1 |= kDcCalPeriodic3 << kReg1DcCalShift;
    reg1 |= kDcCalSpeedupOff << kReg1DcCalSpeedupShift;

    GainSetting s;
    s.reg1 = reg1;
    s.reg6 = 6u | (kDcCalTrackTime << 4) | (kDcCalRate << 10);
    return s;
}

double lnaGainDb(const GainStages& g) { return g.lnaReduction != 0 ? 0.0 : kLnaGainDb; }

double mixerGainDb(const GainStages& g) { return g.mixerReduction != 0 ? 0.0 : kMixerGainDb; }

double basebandGainDb(const GainStages& g) {
    return kBasebandMaxDb - static_cast<double>(g.basebandReduction);
}

}  // namespace cascade::source::msi001
