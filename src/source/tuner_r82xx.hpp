// The Rafael Micro R820T / R820T2 / R828D tuner, over the RTL2832U's I2C
// repeater.
//
// WHAT THE PART IS. A single-conversion silicon tuner: a tracking RF filter,
// an LNA, a mixer driven by a fractional-N PLL, and an IF chain with a VGA.
// It has no register map in the public domain; what everybody knows about it
// comes from the DVB drivers, and what is encoded here are those facts - the
// register addresses, the initialisation values, the PLL arithmetic and the
// band tables. They are facts about the silicon, not anybody's expression of
// them; librtlsdr was consulted as documentation and no code was taken. See
// THIRD-PARTY-LICENSES.txt.
//
// THE SHADOW REGISTERS, and why they are not an optimisation. The R82xx can
// only be READ from register 0, in order, with every byte BIT-REVERSED - you
// cannot ask it what register 0x1a currently holds. So a read-modify-write of
// one field is impossible against the hardware, and every driver for this
// part keeps a shadow copy of registers 0x05 upwards and modifies that. A
// shadow that falls out of step with the chip produces a tuner that is
// plausibly, quietly, wrong, which is why init() writes the whole block
// rather than trusting whatever the last program left behind.
//
// THE THREE GAINS. The part has three amplification stages - LNA, mixer and
// VGA - each a 16-step index rather than a dB value, and the dB each step is
// worth was MEASURED (at 928 MHz, -60 dBm, on a GSM test set) rather than
// specified. This driver exposes all three by name, plus an aggregate
// "TUNER" gain that walks the LNA and mixer up together the way every RTL-SDR
// application's single gain slider does. The measured step tables are the
// oracle's; the dB conversions here are derived from them.
//
// THE RTL-SDR BLOG V4 is an R828D with two things bolted on: a 28.8 MHz
// upconverter in front of an HF input, and switchable notch filters. Up to
// 28.8 MHz the tuner is asked for rf + 28.8 MHz and the dongle's own GPIO 5
// plus tuner input registers route the antenna through the upconverter. It is
// not an optional extra here: sixteen of this product's users have one, and
// without this path a V4 hears nothing at all below 24 MHz.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <string>

#include "source/rtl2832u.hpp"

namespace cascade::source {

// The two I2C addresses an R82xx answers on, which is also how the two chips
// are told apart: the probe reads register 0 at each and looks for 0x69.
constexpr std::uint8_t kR820tI2cAddr = 0x34;
constexpr std::uint8_t kR828dI2cAddr = 0x74;
constexpr std::uint8_t kR82xxCheckReg = 0x00;
constexpr std::uint8_t kR82xxCheckValue = 0x69;

// The intermediate frequency the part runs at in its 6 MHz mode, which is
// what this driver leaves it in: the demodulator's down-converter removes it,
// so the user sees baseband.
constexpr std::uint32_t kR82xxIfFreqHz = 3570000;

// A standard (non-Blog-V4) R828D dongle clocks its tuner from a 16 MHz
// crystal rather than the demodulator's 28.8 MHz one.
constexpr std::uint32_t kR828dXtalHz = 16000000;

// The 28.8 MHz the Blog V4's upconverter adds, and the frequency at and below
// which it is switched in - the PLL offset, the input switch and antenna()
// all take this boundary inclusively.
constexpr std::uint32_t kBlogV4UpconvertHz = 28800000;

class TunerR82xx {
public:
    enum class Chip { R820T, R828D };
    enum class Stage { Lna, Mixer, Vga };

    struct Config {
        Chip chip = Chip::R820T;
        std::uint8_t i2cAddr = kR820tI2cAddr;
        // The tuner's own reference, which is NOT always the demodulator's.
        std::uint32_t xtalHz = kDefaultXtalHz;
        // True only for a dongle whose USB strings say RTLSDRBlog / Blog V4.
        bool blogV4 = false;
    };

    TunerR82xx(Rtl2832u& rtl, Config cfg) : rtl_(rtl), cfg_(cfg) {}

    // Probes both I2C addresses and reports what answered. The caller has
    // already switched the repeater on. Returns false when neither did.
    static bool detect(Rtl2832u& rtl, Chip& chip, std::uint8_t& i2cAddr);

    static const char* chipName(Chip c) { return c == Chip::R828D ? "R828D" : "R820T"; }
    const char* name() const { return cfg_.blogV4 ? "R828D (RTL-SDR Blog V4)" : chipName(cfg_.chip); }
    Chip chip() const { return cfg_.chip; }
    bool blogV4() const { return cfg_.blogV4; }

    // Writes the whole register block, calibrates the IF filter and puts the
    // part in its digital-television configuration - which, with the
    // demodulator in SDR mode, is what an SDR wants.
    bool init();

    // Powers the analogue blocks down. Called on close so a dongle left
    // plugged in is not heating up for nothing.
    bool standby();

    // Tunes to an RF frequency. On a Blog V4 below 28.8 MHz this is where the
    // upconverter, the input switch and the notch filters are arranged; on
    // any other R828D it is where the Air-In / Cable-1 input is chosen.
    bool setFreqHz(std::uint32_t rfHz);
    std::uint32_t freqHz() const { return rfHz_; }

    // The tuner's PLL reference. Changed only by a crystal-correction (ppm)
    // change, which moves the demodulator's crystal and the tuner's together
    // on every dongle whose tuner shares it.
    void setXtalHz(std::uint32_t hz) { cfg_.xtalHz = hz; }
    std::uint32_t xtalHz() const { return cfg_.xtalHz; }

    // Chooses the IF filter for a signal this wide and reports the IF the
    // part will now run at, which the caller must feed to the demodulator's
    // down-converter or the spectrum lands in the wrong place.
    bool setBandwidthHz(int bandwidthHz);
    std::uint32_t ifFreqHz() const { return ifFreqHz_; }

    // True when the PLL reported lock after the last tune. A tune that did
    // not lock still returns true (the part is in a defined state) but
    // delivers noise, so the caller logs this.
    bool locked() const { return locked_; }

    // --- gain ---------------------------------------------------------------

    // The tuner's own AGC: the LNA and mixer follow the signal, the VGA is
    // parked at a fixed 26.5 dB. Off restores the last manual indices.
    bool setAutoGain(bool on);
    bool autoGain() const { return autoGain_; }

    // The aggregate slider: walks the LNA and mixer indices up alternately
    // until their combined measured gain reaches the request, exactly as
    // every RTL-SDR application's single gain control does. `tenthDb` is in
    // tenths of a dB because that is the resolution the measurement has.
    bool setAggregateGainTenthDb(int tenthDb);
    int aggregateGainTenthDb() const;

    // One stage by index (0..15). Out of range is clamped.
    bool setStageIndex(Stage stage, int index);
    int stageIndex(Stage stage) const;

    // The measured gain of one stage at one index, in tenths of a dB. The
    // LNA and mixer start at 0; the VGA starts at -4.7 dB.
    static int stageGainTenthDb(Stage stage, int index);
    // The index whose measured gain is closest to `tenthDb`. The mixer table
    // is not monotonic (step 15 is negative), so this is a search, not
    // arithmetic.
    static int nearestStageIndex(Stage stage, int tenthDb);
    static void stageRangeTenthDb(Stage stage, int& loOut, int& hiOut);
    // The aggregate's own range, which is the LNA and mixer walked together.
    static void aggregateRangeTenthDb(int& loOut, int& hiOut);

    const std::string& lastError() const { return lastError_; }

private:
    bool write(std::uint8_t reg, const std::uint8_t* values, int len);
    bool writeReg(std::uint8_t reg, std::uint8_t value);
    bool writeMask(std::uint8_t reg, std::uint8_t value, std::uint8_t mask);
    int cached(std::uint8_t reg) const;
    // Reads `len` bytes from register 0, bit-reversing each - the only read
    // the part offers.
    bool read(std::uint8_t* out, int len);

    bool setMux(std::uint32_t loHz);
    bool setPll(std::uint32_t loHz);
    bool setTvStandard();
    bool sysFreqSelect();
    bool applyVgaIndex();

    Rtl2832u& rtl_;
    Config cfg_;

    // Shadow of registers 0x05 .. 0x22. See the header: the part cannot be
    // read back by address, so this is the only copy of its state there is.
    static constexpr int kShadowFirst = 0x05;
    static constexpr int kShadowCount = 30;
    std::uint8_t regs_[kShadowCount] = {0};

    std::uint32_t rfHz_ = 0;
    std::uint32_t ifFreqHz_ = kR82xxIfFreqHz;
    std::uint8_t filterCalCode_ = 0;
    bool locked_ = false;
    bool initDone_ = false;

    // The input the Blog V4 / R828D switch last selected, so a retune inside
    // one band does not rewrite the switch registers on every step.
    int selectedInput_ = -1;

    bool autoGain_ = false;
    int lnaIndex_ = 0;
    int mixerIndex_ = 0;
    // 8 is the index the reference driver hard-codes on every tune (16.3 dB).
    // Kept as state here instead, so a user who moves the VGA does not have
    // it silently reset by the next retune.
    int vgaIndex_ = 8;

    std::string lastError_;
};

}  // namespace cascade::source
