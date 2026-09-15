// tuner_msi001.hpp - the Mirics MSi001 tuner that sits behind an MSi2500: its
// band plan, its fractional synthesiser, its filter and IF selection and its
// gain stages. Numbers and pure functions; it cannot touch a device, and every
// answer it gives is a 24-bit word for msi2500::kRegTunerWord to carry.
//
// The licence posture is msi2500.hpp's, in full: libmirisdr-4 is GPL-2.0,
// nothing was copied from it, and what it was used for is the only available
// description of how this silicon behaves. This tuner in particular has no
// published register map, so the band edges, the switch words, the PLL
// arithmetic and what each gain stage is worth in decibels are facts read off
// the behaviour of the driver that already speaks to it - facts about
// hardware, written here in this codebase's own style.
//
// HOW THE TUNER IS REACHED AT ALL, since it has no USB of its own. Every MSi001
// register is written by sending a 24-bit word to the MSi2500's register 9, and
// THE TUNER'S OWN REGISTER NUMBER IS THE LOW NIBBLE OF THAT WORD. So the
// synthesiser's registers 0, 2, 3 and 5 are words whose low nibble is 0, 2, 3
// and 5, and the gain register is a word ending in 1. That is why every value
// computed below starts life as its own register number rather than as zero,
// and why a word with the wrong low nibble does not fail - it programs a
// different block.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <vector>

namespace cascade::source::msi001 {

// --- bands ----------------------------------------------------------------

// Which input and mixer chain the tuner is using. The gain map needs it: the
// low-noise amplifier is not on the AM inputs at all, and the buffer that
// stands in for it there is worth a different number of decibels on each of
// the two AM ports.
enum class Band {
    Am1,
    Am2,
    Vhf,
    Band3,
    Band45,
    LBand,
};

const char* bandName(Band b);

// The mode bits, which are ORed into the top of the register 0 nibble at bit
// 4. They are FLAGS rather than an enumeration, and one band plan entry
// deliberately sets two of them at once (see the 259 MHz row below).
constexpr int kModeAm = 0x01;
constexpr int kModeVhf = 0x02;
constexpr int kModeBand3 = 0x04;
constexpr int kModeBand45 = 0x08;
constexpr int kModeLBand = 0x10;

// Which input network the board around the tuner has. The same pair of chips
// was sold behind two of them and they want different band edges and different
// switch words; msi2500::DeviceModel says which a device is.
enum class Plan {
    Default,
    SdrPlay,
};

// One row of the band plan: everything that changes when the tuner crosses an
// edge. `lowCutMHz` is the bottom of the row, `loDiv` is what the local
// oscillator is divided by in it, and `bandSelectWord` is what the MSi2500's
// register 8 carries to put the right filter in circuit.
struct BandPlanEntry {
    std::uint32_t lowCutMHz;
    int modeBits;  // negative marks the terminator
    bool upconvertMixer;
    int amPort;  // 0 or 1, meaningful only in the AM rows
    std::uint32_t loDiv;
    std::uint32_t bandSelectWord;
};

const std::vector<BandPlanEntry>& planEntries(Plan p);

// --- filter, IF and crystal ----------------------------------------------

// The eight baseband filter widths, in the order their register words run.
enum class Bandwidth {
    Khz200,
    Khz300,
    Khz600,
    Khz1536,
    Mhz5,
    Mhz6,
    Mhz7,
    Mhz8,
};

std::uint32_t bandwidthWord(Bandwidth b);
double bandwidthHz(Bandwidth b);

// The widest filter no wider than what the rate can carry. The tuner's filter
// is not a menu item in this product: it follows the sample rate, for the same
// reason the HackRF's does - a rate change that left the old filter behind
// would alias, or throw away half the span.
Bandwidth bandwidthForRate(double sampleRateHz);

// The IF the tuner mixes down to. Zero IF is what this driver uses; the others
// are named because the register field has four values and a field whose other
// values are unnamed is a field nobody can check.
enum class IfMode {
    Zero,
    Khz450,
    Khz1620,
    Khz2048,
};

std::uint32_t ifModeWord(IfMode m);

// The crystal field. This driver never changes it - the reference does not
// support changing it either, and every board in msi2500::deviceModels() is
// built around a 24 MHz part - so the word is a constant with a name rather
// than an enumeration nothing selects from.
constexpr std::uint32_t kXtalWord24M = 0x02;

// --- the tuning range this driver offers ----------------------------------
//
// The band plan covers 0 to 2400 MHz and its top row runs from 960 MHz to the
// terminator. This driver accepts 150 kHz to 2 GHz of that, and the two ends
// are choices rather than measurements: below 150 kHz the AM row's upconvert
// path is being asked for a frequency the input network was never built to
// pass, and above 2 GHz the plan has nothing but its last row and a
// terminator, so a tune there would land on L-band settings that were not
// chosen for it. A tune outside the range is REFUSED with a reason rather than
// clamped - a tune that silently lands somewhere else is worse than one that
// does not happen.
constexpr double kMinFrequencyHz = 150.0e3;
constexpr double kMaxFrequencyHz = 2.0e9;

// --- tuning ---------------------------------------------------------------

// Everything one frequency becomes. The four registers are what goes on the
// wire; the working values below them are exposed because they are what a test
// can pin against the reference's own printout rather than against a second
// copy of this arithmetic.
struct TuneSetting {
    Band band = Band::Vhf;
    int planIndex = 0;
    std::uint32_t bandSelectWord = 0;  // for the MSi2500's register 8

    std::uint32_t reg0 = 0;  // mode, IF, filter, crystal
    std::uint32_t reg2 = 0;  // the synthesiser's integer and fraction
    std::uint32_t reg3 = 0;  // the automatic frequency correction trim
    std::uint32_t reg5 = 0;  // the fraction's denominator

    std::uint64_t loDiv = 0;
    std::uint64_t offsetHz = 0;  // the upconvert mixer's 120 MHz, in the AM rows
    std::uint64_t n = 0;
    std::uint64_t thresh = 0;
    std::uint64_t frac = 0;
    std::uint64_t afc = 0;
    std::uint64_t rfvcoHz = 0;  // where the PLL lands before the AFC trim
};

// soft.c mirisdr_set_soft. The synthesiser is a fraction with a PROGRAMMABLE
// DENOMINATOR rather than a fixed power of two: the exact ratio is reduced by
// its greatest common divisor, then scaled down until the denominator fits the
// twelve bits it has, and whatever error that scaling leaves is measured and
// written into a separate trim field. That last step is why a frequency whose
// ratio reduces neatly (100 MHz, 1 GHz) programs a trim of zero while one an
// Ohm away (100.000001 MHz) programs a large one.
TuneSetting computeTune(double freqHz, Plan p, Bandwidth bw, IfMode ifMode);

// The word the reference writes to the tuner immediately before the four tune
// words. Its low nibble is 14, so it is a write to the tuner's register 14
// with a value of zero; what that register does is not something this driver
// can claim to know, and it is sent because the sequence it belongs to is the
// one the silicon is known to take.
constexpr std::uint32_t kTunePreambleWord = 0x00000Eu;

// --- gain -----------------------------------------------------------------

// What each stage is worth when it is NOT reduced, read off the reference's
// own readback functions. The baseband amplifier is the continuous one; the
// other two are switches.
constexpr double kLnaGainDb = 24.0;
constexpr double kMixerGainDb = 19.0;
constexpr double kBasebandMaxDb = 59.0;

// The AM buffer's four steps are worth different decibels on each AM port -
// 0/6/12/18 on port 1, and 0 or 24 on port 2, where only "none" and "all"
// exist. That is why the Source section is given it as GainUnit::Steps rather
// than as a decibel figure: a number whose unit changes with the band is a
// number no panel can letter honestly, and inventing one decibel scale for
// both ports would be a figure no instrument ever produced.
constexpr int kMixbufferSteps = 4;  // reductions 0-3

// The four reductions, as the hardware counts them: 0 is NO reduction, which
// is maximum gain. Stored this way rather than as decibels because it is what
// the register holds, and one conversion at the edge is easier to check than a
// conversion in every path.
struct GainStages {
    int lnaReduction = 0;         // 0 or 1
    int mixbufferReduction = 0;   // 0 to 3, AM inputs only
    int mixerReduction = 0;       // 0 or 1
    int basebandReduction = 0;    // 0 to 59
};

struct GainSetting {
    std::uint32_t reg1 = 0;  // the stages, plus the DC-offset calibration mode
    std::uint32_t reg6 = 0;  // the DC-offset calibration timing
};

// gain.c mirisdr_set_gain. The band matters twice: the low-noise amplifier is
// not in circuit on either AM input so its bit is forced clear there, and the
// buffer that stands in for it is a two-bit field on port 1 but only all-or-
// nothing on port 2.
GainSetting computeGain(const GainStages& g, Band band);

// The decibel readback for each stage, so the driver and its tests agree on
// what a reduction is worth without either of them holding its own copy.
double lnaGainDb(const GainStages& g);
double mixerGainDb(const GainStages& g);
double basebandGainDb(const GainStages& g);

}  // namespace cascade::source::msi001
