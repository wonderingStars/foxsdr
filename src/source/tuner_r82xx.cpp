// See tuner_r82xx.hpp for what the part is and the licence position.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/tuner_r82xx.hpp"

#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace cascade::source {

namespace {

// THE POWER-ON REGISTER BLOCK, 0x05 upwards. Written whole by init() rather
// than field by field: the part cannot be read back, so the only way to know
// what is in it is to have put it there.
const std::uint8_t kInitRegs[30] = {
    0x83, 0x30, 0x75,              // 0x05 0x06 0x07
    0xc0, 0x40, 0xd6, 0x6c,        // 0x08 .. 0x0b
    0xf5, 0x63, 0x75, 0x68,        // 0x0c .. 0x0f
    0x6c, 0x83, 0x80, 0x00,        // 0x10 .. 0x13
    0x0f, 0x00, 0xc0, 0x30,        // 0x14 .. 0x17
    0x48, 0xcc, 0x60, 0x00,        // 0x18 .. 0x1b
    0x54, 0xae, 0x4a, 0xc0,        // 0x1c .. 0x1f
    0x00, 0x00, 0x00               // 0x20 .. 0x22 (past the documented map;
                                   // written because the part expects the
                                   // block write to be this long)
};

// THE TRACKING-FILTER BAND TABLE. Below about 300 MHz the part runs its RF
// input through an on-chip tracking filter, and which band that filter is set
// to has to be chosen by frequency; above it the filter is bypassed. Each row
// is "from this many MHz upwards, use these". open_d is the open-drain buffer,
// rf_mux_ploy selects filter/bypass and the poly-phase filter's corner, tf_c
// is the two four-bit band codes, and the three caps are the crystal loading
// options this driver does not use (it runs the high-cap 0 pF setting).
struct BandRow {
    std::uint32_t fromMHz;
    std::uint8_t openD;
    std::uint8_t rfMuxPoly;
    std::uint8_t tfC;
    std::uint8_t cap20p;
    std::uint8_t cap10p;
    std::uint8_t cap0p;
};

const BandRow kBands[] = {
    {0, 0x08, 0x02, 0xdf, 0x02, 0x01, 0x00},   {50, 0x08, 0x02, 0xbe, 0x02, 0x01, 0x00},
    {55, 0x08, 0x02, 0x8b, 0x02, 0x01, 0x00},  {60, 0x08, 0x02, 0x7b, 0x02, 0x01, 0x00},
    {65, 0x08, 0x02, 0x69, 0x02, 0x01, 0x00},  {70, 0x08, 0x02, 0x58, 0x02, 0x01, 0x00},
    {75, 0x00, 0x02, 0x44, 0x02, 0x01, 0x00},  {80, 0x00, 0x02, 0x44, 0x02, 0x01, 0x00},
    {90, 0x00, 0x02, 0x34, 0x01, 0x01, 0x00},  {100, 0x00, 0x02, 0x34, 0x01, 0x01, 0x00},
    {110, 0x00, 0x02, 0x24, 0x01, 0x01, 0x00}, {120, 0x00, 0x02, 0x24, 0x01, 0x01, 0x00},
    {140, 0x00, 0x02, 0x14, 0x01, 0x01, 0x00}, {180, 0x00, 0x02, 0x13, 0x00, 0x00, 0x00},
    {220, 0x00, 0x02, 0x13, 0x00, 0x00, 0x00}, {250, 0x00, 0x02, 0x11, 0x00, 0x00, 0x00},
    {280, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00}, {310, 0x00, 0x41, 0x00, 0x00, 0x00, 0x00},
    {450, 0x00, 0x41, 0x00, 0x00, 0x00, 0x00}, {588, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00},
    {650, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00},
};

// THE MEASURED GAIN STEPS, in tenths of a dB. Each entry is what moving from
// the previous index to this one is worth, so a stage's gain is the running
// sum. Measured on a GSM test set at 928 MHz with -60 dBm in; the mixer's
// last step is NEGATIVE, which is why index 15 is not the loudest mixer
// setting and why choosing an index by dB has to be a search.
const int kLnaSteps[16] = {0, 9, 13, 40, 38, 13, 31, 22, 26, 31, 26, 14, 19, 5, 35, 13};
const int kMixerSteps[16] = {0, 5, 10, 10, 19, 9, 10, 25, 17, 10, 8, 16, 13, 6, 3, -8};
const int kVgaSteps[16] = {0, 26, 26, 30, 42, 35, 24, 13, 14, 32, 36, 34, 35, 37, 35, 36};
// The VGA's index 0 is not silence: it is 4.7 dB of attenuation.
constexpr int kVgaBaseTenthDb = -47;

// The part's own firmware version field, which its initialisation expects to
// be written even though nothing reads it back.
constexpr std::uint8_t kVersionNumber = 49;

// The three input paths a Blog V4 (and, in its Air/Cable form, a plain R828D)
// switches between.
constexpr int kInputHf = 1;
constexpr int kInputVhf = 2;
constexpr int kInputUhf = 3;

std::uint8_t bitReverse(std::uint8_t b) {
    // The part clocks its read data out most-significant bit first into a bus
    // that is least-significant first, so every byte arrives mirrored.
    static const std::uint8_t kNibble[16] = {0x0, 0x8, 0x4, 0xc, 0x2, 0xa, 0x6, 0xe,
                                             0x1, 0x9, 0x5, 0xd, 0x3, 0xb, 0x7, 0xf};
    return static_cast<std::uint8_t>((kNibble[b & 0x0f] << 4) | kNibble[b >> 4]);
}

const int* stepsFor(TunerR82xx::Stage s) {
    switch (s) {
        case TunerR82xx::Stage::Lna: return kLnaSteps;
        case TunerR82xx::Stage::Mixer: return kMixerSteps;
        default: return kVgaSteps;
    }
}

}  // namespace

// --- register plumbing ------------------------------------------------------

int TunerR82xx::cached(std::uint8_t reg) const {
    const int i = static_cast<int>(reg) - kShadowFirst;
    if (i < 0 || i >= kShadowCount) { return -1; }
    return regs_[i];
}

bool TunerR82xx::write(std::uint8_t reg, const std::uint8_t* values, int len) {
    // Shadow first, so a transfer that fails still leaves the shadow and the
    // chip disagreeing in the safe direction (we believe we wrote it, the
    // next full init corrects it) rather than leaving the shadow behind the
    // chip, which would make every later masked write wrong.
    for (int i = 0; i < len; ++i) {
        const int at = static_cast<int>(reg) + i - kShadowFirst;
        if (at >= 0 && at < kShadowCount) { regs_[at] = values[i]; }
    }
    // The RTL2832U's I2C block takes at most 8 bytes per transfer, and the
    // first is the register address, so the block is written 7 values at a
    // time with the address stepping along.
    constexpr int kMaxPayload = 7;
    int pos = 0;
    std::uint8_t at = reg;
    while (pos < len) {
        const int size = (len - pos > kMaxPayload) ? kMaxPayload : (len - pos);
        std::uint8_t buf[kMaxPayload + 1];
        buf[0] = at;
        std::memcpy(&buf[1], values + pos, static_cast<std::size_t>(size));
        if (!rtl_.i2cWrite(cfg_.i2cAddr, buf, static_cast<std::uint8_t>(size + 1))) {
            lastError_ = "the tuner stopped answering on the I2C bus";
            return false;
        }
        at = static_cast<std::uint8_t>(at + size);
        pos += size;
    }
    return true;
}

bool TunerR82xx::writeReg(std::uint8_t reg, std::uint8_t value) { return write(reg, &value, 1); }

bool TunerR82xx::writeMask(std::uint8_t reg, std::uint8_t value, std::uint8_t mask) {
    const int cur = cached(reg);
    if (cur < 0) {
        lastError_ = "a tuner register outside the shadow was masked";
        return false;
    }
    const std::uint8_t next =
        static_cast<std::uint8_t>((static_cast<std::uint8_t>(cur) & ~mask) | (value & mask));
    return write(reg, &next, 1);
}

bool TunerR82xx::read(std::uint8_t* out, int len) {
    std::uint8_t reg = 0x00;
    if (!rtl_.i2cWrite(cfg_.i2cAddr, &reg, 1)) { return false; }
    std::uint8_t raw[8] = {0};
    const int n = (len > 8) ? 8 : len;
    if (!rtl_.i2cRead(cfg_.i2cAddr, raw, static_cast<std::uint8_t>(n))) { return false; }
    for (int i = 0; i < n; ++i) { out[i] = bitReverse(raw[i]); }
    return true;
}

// --- detection --------------------------------------------------------------

bool TunerR82xx::detect(Rtl2832u& rtl, Chip& chip, std::uint8_t& i2cAddr) {
    // Register 0 answers 0x69 on both parts; only the address differs. NOTE
    // this compares the RAW bus byte, not the bit-reversed form read() uses:
    // the part mirrors its read data, so 0x69 on the wire is 0x96 in the
    // register, and reversing here would look for a value that never arrives.
    // Both conventions are the part's and both are load-bearing.
    const int a = rtl.i2cReadReg(kR820tI2cAddr, kR82xxCheckReg);
    if (a >= 0 && static_cast<std::uint8_t>(a) == kR82xxCheckValue) {
        chip = Chip::R820T;
        i2cAddr = kR820tI2cAddr;
        return true;
    }
    const int b = rtl.i2cReadReg(kR828dI2cAddr, kR82xxCheckReg);
    if (b >= 0 && static_cast<std::uint8_t>(b) == kR82xxCheckValue) {
        chip = Chip::R828D;
        i2cAddr = kR828dI2cAddr;
        return true;
    }
    return false;
}

// --- bring-up ---------------------------------------------------------------

bool TunerR82xx::init() {
    if (!write(kShadowFirst, kInitRegs, kShadowCount)) { return false; }
    if (!setTvStandard()) { return false; }
    if (!sysFreqSelect()) { return false; }
    initDone_ = true;
    return true;
}

bool TunerR82xx::setTvStandard() {
    // The 6 MHz digital-television configuration, which is the one whose IF
    // (3.57 MHz) and filter shape suit a wideband SDR: wider settings move
    // the IF to 4.57 MHz and put the passband edge inside the span.
    constexpr std::uint8_t kHpCor = 0x6b;       // high-pass corner, 1.0 MHz
    constexpr std::uint8_t kFiltQ = 0x10;       // low-Q IF filter
    constexpr std::uint8_t kFiltGain = 0x30;    // +3 dB, 6 MHz section on
    constexpr std::uint8_t kImgR = 0x00;        // image rejection, negative
    constexpr std::uint8_t kExtEnable = 0x60;   // channel-filter extension
    constexpr std::uint8_t kLoopThrough = 0x80; // loop-through output off
    constexpr std::uint8_t kLtAtt = 0x00;
    constexpr std::uint8_t kFltExtWidest = 0x00;
    constexpr std::uint8_t kPolyFilCur = 0x60;
    // The filter calibration tunes the IF filter against a known LO. 56 MHz
    // is inside the part's lowest band and far from anything it will be used
    // at, so the calibration cannot be pulled by a strong signal.
    constexpr std::uint32_t kFilterCalLoHz = 56000000;

    std::memcpy(regs_, kInitRegs, sizeof(regs_));
    ifFreqHz_ = kR82xxIfFreqHz;

    if (!writeMask(0x0c, 0x00, 0x0f)) { return false; }
    if (!writeMask(0x13, kVersionNumber, 0x3f)) { return false; }
    if (!writeMask(0x1d, 0x00, 0x38)) { return false; }

    // TWO ATTEMPTS, because the first calibration after power-on can land on
    // a code of 0 or 0x0f - both of which mean "did not converge" rather than
    // a filter setting.
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!writeMask(0x0b, kHpCor, 0x60)) { return false; }
        if (!writeMask(0x0f, 0x04, 0x04)) { return false; }  // calibration clock on
        if (!writeMask(0x10, 0x00, 0x03)) { return false; }  // 0 pF crystal cap for the PLL
        if (!setPll(kFilterCalLoHz)) { return false; }
        if (!locked_) {
            lastError_ = "the tuner's PLL would not lock for its filter calibration";
            return false;
        }
        if (!writeMask(0x0b, 0x10, 0x10)) { return false; }  // trigger
        if (!writeMask(0x0b, 0x00, 0x10)) { return false; }  // release
        if (!writeMask(0x0f, 0x00, 0x04)) { return false; }  // calibration clock off
        std::uint8_t data[5] = {0};
        if (!read(data, 5)) { return false; }
        filterCalCode_ = static_cast<std::uint8_t>(data[4] & 0x0f);
        if (filterCalCode_ != 0 && filterCalCode_ != 0x0f) { break; }
    }
    // Both failure codes fall back to the narrowest filter, which is audible
    // but not broken - better than leaving 0x0f, which is not a setting.
    if (filterCalCode_ == 0x0f) { filterCalCode_ = 0; }

    if (!writeMask(0x0a, static_cast<std::uint8_t>(kFiltQ | filterCalCode_), 0x1f)) {
        return false;
    }
    if (!writeMask(0x0b, kHpCor, 0xef)) { return false; }
    if (!writeMask(0x07, kImgR, 0x80)) { return false; }
    if (!writeMask(0x06, kFiltGain, 0x30)) { return false; }
    if (!writeMask(0x1e, kExtEnable, 0x60)) { return false; }
    if (!writeMask(0x05, kLoopThrough, 0x80)) { return false; }
    if (!writeMask(0x1f, kLtAtt, 0x80)) { return false; }
    if (!writeMask(0x0f, kFltExtWidest, 0x80)) { return false; }
    if (!writeMask(0x19, kPolyFilCur, 0x60)) { return false; }
    return true;
}

bool TunerR82xx::sysFreqSelect() {
    // The detector thresholds and bias currents the part runs its automatic
    // loops against. These are the digital-television values; the numbers are
    // the part's, and the comments say what each field is for rather than
    // what it contains.
    constexpr std::uint8_t kMixerTop = 0x24;    // mixer detector top, low discharge
    constexpr std::uint8_t kLnaTop = 0xe5;      // LNA detector top, pre-detect top 2
    constexpr std::uint8_t kLnaVthL = 0x53;     // LNA detector thresholds
    constexpr std::uint8_t kMixerVthL = 0x75;   // mixer detector thresholds
    constexpr std::uint8_t kCpCur = 0x38;       // charge-pump current, auto
    constexpr std::uint8_t kFilterCur = 0x40;   // IF filter current, low
    constexpr std::uint8_t kLnaDischarge = 14;
    // The PLL's divider-buffer current. Raised from the television value so
    // the PLL's drop-out stays above 2 V, which is what keeps L band (1.2 -
    // 1.7 GHz) usable rather than intermittently unlocked.
    constexpr std::uint8_t kDivBufCur = 0xa0;

    if (!writeMask(0x1d, kLnaTop, 0xc7)) { return false; }
    if (!writeMask(0x1c, kMixerTop, 0xf8)) { return false; }
    if (!writeReg(0x0d, kLnaVthL)) { return false; }
    if (!writeReg(0x0e, kMixerVthL)) { return false; }
    selectedInput_ = 0;
    if (!writeMask(0x05, 0x00, 0x60)) { return false; }  // Air-In / Cable-1 select
    if (!writeMask(0x06, 0x00, 0x08)) { return false; }  // Cable-2 off
    if (!writeMask(0x11, kCpCur, 0x38)) { return false; }
    if (!writeMask(0x17, kDivBufCur, 0x30)) { return false; }
    if (!writeMask(0x0a, kFilterCur, 0x60)) { return false; }

    // The LNA detector's start-up dance. Lowering the top, letting the
    // detector settle at a fast AGC clock, then raising it and slowing the
    // clock, is what stops the loop latching at an extreme on power-on.
    if (!writeMask(0x1d, 0x00, 0x38)) { return false; }
    if (!writeMask(0x1c, 0x00, 0x04)) { return false; }
    if (!writeMask(0x06, 0x00, 0x40)) { return false; }  // pre-detect off
    if (!writeMask(0x1a, 0x30, 0x30)) { return false; }  // AGC clock 250 Hz
    if (!writeMask(0x1d, 0x18, 0x38)) { return false; }  // LNA top 3
    if (!writeMask(0x1c, kMixerTop, 0x04)) { return false; }
    if (!writeMask(0x1e, kLnaDischarge, 0x1f)) { return false; }
    if (!writeMask(0x1a, 0x20, 0x30)) { return false; }  // AGC clock 60 Hz
    return true;
}

bool TunerR82xx::standby() {
    if (!initDone_) { return true; }
    // The documented power-down values, register by register. Not a mask
    // write anywhere: the point is to leave the analogue blocks off whatever
    // the shadow currently says.
    static const std::uint8_t kStandby[][2] = {
        {0x06, 0xb1}, {0x05, 0xa0}, {0x07, 0x3a}, {0x08, 0x40}, {0x09, 0xc0},
        {0x0a, 0x36}, {0x0c, 0x35}, {0x0f, 0x68}, {0x11, 0x03}, {0x17, 0xf4},
        {0x19, 0x0c},
    };
    bool ok = true;
    for (const auto& pair : kStandby) { ok = writeReg(pair[0], pair[1]) && ok; }
    return ok;
}

// --- tuning -----------------------------------------------------------------

bool TunerR82xx::setMux(std::uint32_t loHz) {
    const std::uint32_t mhz = loHz / 1000000u;
    std::size_t i = 0;
    const std::size_t n = sizeof(kBands) / sizeof(kBands[0]);
    for (; i + 1 < n; ++i) {
        if (mhz < kBands[i + 1].fromMHz) { break; }
    }
    const BandRow& row = kBands[i];

    if (!writeMask(0x17, row.openD, 0x08)) { return false; }
    if (!writeMask(0x1a, row.rfMuxPoly, 0xc3)) { return false; }
    if (!writeReg(0x1b, row.tfC)) { return false; }
    // This driver always runs the high-capacitance 0 pF crystal option, so
    // the loading field is the row's cap0p with the drive bit clear.
    if (!writeMask(0x10, row.cap0p, 0x0b)) { return false; }
    // The two image-rejection calibration registers, left at zero: the
    // factory calibration this part can do needs a signal generator.
    if (!writeMask(0x08, 0x00, 0x3f)) { return false; }
    if (!writeMask(0x09, 0x00, 0x3f)) { return false; }
    return true;
}

bool TunerR82xx::setPll(std::uint32_t loHz) {
    // THE FRACTIONAL-N PLL. The VCO runs between 1770 and 3540 MHz whatever
    // the output frequency is, so the first job is to find the power-of-two
    // mixer divider that lands it in that window. The integer part of the
    // division is then split into the part's odd (ni, si) encoding, and the
    // remainder is pushed into a 16-bit sigma-delta.
    constexpr std::uint32_t kVcoMinKHz = 1770000;
    const std::uint32_t vcoMaxKHz = kVcoMinKHz * 2;

    const std::uint32_t loKHz = (loHz + 500) / 1000;
    const std::uint32_t refHz = cfg_.xtalHz;
    const std::uint32_t refKHz = (cfg_.xtalHz + 500) / 1000;

    locked_ = false;
    if (!writeMask(0x10, 0x00, 0x10)) { return false; }  // reference divider /1
    if (!writeMask(0x1a, 0x00, 0x0c)) { return false; }  // autotune window 128 kHz
    // VCO current at maximum. The stock setting drops out at the top of the
    // range on some parts; the cost of the maximum is a little more current.
    if (!writeMask(0x12, 0x06, 0xff)) { return false; }

    std::uint32_t mixDiv = 2;
    std::uint8_t divNum = 0;
    while (mixDiv <= 64) {
        if (static_cast<std::uint64_t>(loKHz) * mixDiv >= kVcoMinKHz &&
            static_cast<std::uint64_t>(loKHz) * mixDiv < vcoMaxKHz) {
            std::uint32_t d = mixDiv;
            while (d > 2) {
                d >>= 1;
                ++divNum;
            }
            break;
        }
        mixDiv <<= 1;
    }

    std::uint8_t data[5] = {0};
    if (!read(data, 5)) { return false; }
    // The part reports how far its VCO autotune had to move; a divider one
    // step either way lands the VCO nearer the middle of its range.
    // The R828D's VCO sits one power step below the R820T's, whatever board
    // it is on - a Blog V4 included, since a V4 IS an R828D. Getting this
    // wrong moves the divider by one step and tunes to half or double.
    const std::uint8_t vcoPowerRef = (cfg_.chip == Chip::R828D) ? 1 : 2;
    const std::uint8_t vcoFineTune = static_cast<std::uint8_t>((data[4] & 0x30) >> 4);
    if (vcoFineTune > vcoPowerRef) {
        divNum = static_cast<std::uint8_t>(divNum - 1);
    } else if (vcoFineTune < vcoPowerRef) {
        divNum = static_cast<std::uint8_t>(divNum + 1);
    }
    if (!writeMask(0x10, static_cast<std::uint8_t>(divNum << 5), 0xe0)) { return false; }

    const std::uint64_t vcoHz = static_cast<std::uint64_t>(loHz) * mixDiv;
    const std::uint32_t nint = static_cast<std::uint32_t>(vcoHz / (2ull * refHz));
    std::uint32_t vcoFraKHz =
        static_cast<std::uint32_t>((vcoHz - 2ull * refHz * nint) / 1000ull);
    if (nint > (128u / vcoPowerRef) - 1u) {
        lastError_ = "that frequency is outside the tuner's PLL range";
        return false;
    }
    // The integer divider is stored as a base of 13 plus four times ni plus
    // si - the part's own encoding, not a byte split.
    const std::uint8_t ni = static_cast<std::uint8_t>((nint - 13) / 4);
    const std::uint8_t si = static_cast<std::uint8_t>(nint - 4u * ni - 13u);
    if (!writeReg(0x14, static_cast<std::uint8_t>(ni + (si << 6)))) { return false; }
    // The sigma-delta is powered DOWN when the division is exact, which takes
    // its noise out of the loop entirely.
    if (!writeMask(0x12, vcoFraKHz ? 0x00 : 0x08, 0x08)) { return false; }

    // The fraction, accumulated by successive halving rather than by a
    // divide: the part's sigma-delta word is a binary fraction of twice the
    // reference and this is how its bits fall out.
    std::uint16_t sdm = 0;
    std::uint32_t nSdm = 2;
    while (vcoFraKHz > 1) {
        if (vcoFraKHz > (2 * refKHz / nSdm)) {
            sdm = static_cast<std::uint16_t>(sdm + 32768 / (nSdm / 2));
            vcoFraKHz -= 2 * refKHz / nSdm;
            if (nSdm >= 0x8000) { break; }
        }
        nSdm <<= 1;
    }
    if (!writeReg(0x16, static_cast<std::uint8_t>(sdm >> 8))) { return false; }
    if (!writeReg(0x15, static_cast<std::uint8_t>(sdm & 0xff))) { return false; }

    // Two lock attempts: if the first says no, raise the VCO current and ask
    // again before giving up.
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::uint8_t status[3] = {0};
        if (!read(status, 3)) { return false; }
        if ((status[2] & 0x40) != 0) {
            locked_ = true;
            break;
        }
        if (attempt == 0) {
            if (!writeMask(0x12, 0x06, 0xff)) { return false; }
        }
    }
    if (!locked_) {
        lastError_ = "the tuner's PLL did not report lock";
        return true;  // a defined state, just a deaf one - the caller logs it
    }
    // Narrow the autotune window now that it is locked, so it does not chase
    // a strong nearby signal.
    return writeMask(0x1a, 0x08, 0x08);
}

bool TunerR82xx::applyVgaIndex() {
    // Bit 4 of this register is the VGA's own auto/manual bit; the mask
    // clears it, which is what keeps the index we just wrote in force.
    return writeMask(0x0c, static_cast<std::uint8_t>(vgaIndex_ & 0x0f), 0x9f);
}

bool TunerR82xx::setFreqHz(std::uint32_t rfHz) {
    // THE BLOG V4's UPCONVERTER. Below 28.8 MHz the antenna goes through a
    // mixer that adds exactly the crystal frequency, so the tuner is asked
    // for rf + 28.8 MHz and the user still sees the frequency they typed.
    const bool useUpconverter = cfg_.blogV4 && rfHz < kBlogV4UpconvertHz;
    const std::uint32_t tunerRfHz = useUpconverter ? (rfHz + kBlogV4UpconvertHz) : rfHz;
    const std::uint32_t loHz = tunerRfHz + ifFreqHz_;

    if (!setMux(loHz)) { return false; }
    if (!applyVgaIndex()) { return false; }
    if (!setPll(loHz)) { return false; }
    rfHz_ = rfHz;

    if (cfg_.blogV4) {
        // THE NOTCH FILTERS, which are ON everywhere EXCEPT inside the bands
        // they notch - broadcast AM below 2.2 MHz, FM 85-112 MHz and DAB/VHF
        // 172-242 MHz. Tuning into a notched band has to open the notch or
        // the user hears nothing where the strongest signals are.
        const bool insideNotch = rfHz <= 2200000u || (rfHz >= 85000000u && rfHz <= 112000000u) ||
                                 (rfHz >= 172000000u && rfHz <= 242000000u);
        if (!writeMask(0x17, insideNotch ? 0x00 : 0x08, 0x08)) { return false; }

        const int band = (rfHz <= kBlogV4UpconvertHz)
                             ? kInputHf
                             : ((rfHz < 250000000u) ? kInputVhf : kInputUhf);

        // The tracking filter is bypassed on HF: setMux has just re-applied
        // it for the UPCONVERTED frequency, which is not where the signal is,
        // and its insertion loss buys nothing on that path. Outside the
        // band-change guard on purpose, because setMux runs on every tune.
        if (band == kInputHf) {
            if (!writeMask(0x1a, 0x40, 0xc3)) { return false; }
            if (!writeReg(0x1b, 0x00)) { return false; }
        }

        if (band != selectedInput_) {
            selectedInput_ = band;
            // Cable 2 is the HF input; the dongle's GPIO 5 throws the
            // upconverter switch on the boards that have one, and it is the
            // INVERSE of the tuner-side selection.
            const std::uint8_t cable2 = (band == kInputHf) ? 0x08 : 0x00;
            if (!writeMask(0x06, cable2, 0x08)) { return false; }
            if (!rtl_.setBiasTeeGpio(5, cable2 == 0)) { return false; }
            const std::uint8_t cable1 = (band == kInputVhf) ? 0x40 : 0x00;
            if (!writeMask(0x05, cable1, 0x40)) { return false; }
            const std::uint8_t airIn = (band == kInputUhf) ? 0x00 : 0x20;
            if (!writeMask(0x05, airIn, 0x20)) { return false; }
        }
    } else if (cfg_.chip == Chip::R828D) {
        // A plain R828D dongle has two antenna inputs wired to one tuner.
        // 345 MHz is where their noise floors cross with the same LNA
        // setting, so that is where the switch belongs.
        const std::uint8_t airCable1 = (rfHz > 345000000u) ? 0x00 : 0x60;
        if (airCable1 != selectedInput_) {
            selectedInput_ = airCable1;
            if (!writeMask(0x05, airCable1, 0x60)) { return false; }
        }
    }
    return true;
}

bool TunerR82xx::setBandwidthHz(int bandwidthHz) {
    // THE IF FILTER. Above 6 MHz the part has three fixed television
    // settings; below it the filter is assembled from a low-pass corner and
    // two high-pass sections, and the IF has to MOVE to keep the assembled
    // passband centred - which is why this returns an IF the caller must feed
    // to the demodulator.
    constexpr int kLowPassTable[10] = {1700000, 1600000, 1550000, 1450000, 1200000,
                                       900000,  700000,  550000,  450000,  350000};
    constexpr int kHighPass1 = 350000;
    constexpr int kHighPass2 = 380000;

    std::uint8_t reg0a = 0;
    std::uint8_t reg0b = 0;
    int bw = bandwidthHz;
    if (bw > 7000000) {
        reg0a = 0x10;
        reg0b = 0x0b;
        ifFreqHz_ = 4570000;
    } else if (bw > 6000000) {
        reg0a = 0x10;
        reg0b = 0x2a;
        ifFreqHz_ = 4570000;
    } else if (bw > kLowPassTable[0] + kHighPass1 + kHighPass2) {
        reg0a = 0x10;
        reg0b = 0x6b;
        ifFreqHz_ = kR82xxIfFreqHz;
    } else {
        reg0a = 0x00;
        reg0b = 0x80;
        int realBw = 0;
        ifFreqHz_ = 2300000;
        if (bw > kLowPassTable[0] + kHighPass1) {
            bw -= kHighPass2;
            ifFreqHz_ += kHighPass2;
            realBw += kHighPass2;
        } else {
            reg0b |= 0x20;
        }
        if (bw > kLowPassTable[0]) {
            bw -= kHighPass1;
            ifFreqHz_ += kHighPass1;
            realBw += kHighPass1;
        } else {
            reg0b |= 0x40;
        }
        int i = 0;
        for (; i < 10; ++i) {
            if (bw > kLowPassTable[i]) { break; }
        }
        if (i > 0) { --i; }
        reg0b = static_cast<std::uint8_t>(reg0b | (15 - i));
        realBw += kLowPassTable[i];
        ifFreqHz_ -= static_cast<std::uint32_t>(realBw / 2);
    }
    if (!writeMask(0x0a, reg0a, 0x10)) { return false; }
    return writeMask(0x0b, reg0b, 0xef);
}

// --- gain -------------------------------------------------------------------

int TunerR82xx::stageGainTenthDb(Stage stage, int index) {
    if (index < 0) { index = 0; }
    if (index > 15) { index = 15; }
    const int* steps = stepsFor(stage);
    int total = (stage == Stage::Vga) ? kVgaBaseTenthDb : 0;
    for (int i = 0; i <= index; ++i) { total += steps[i]; }
    return total;
}

int TunerR82xx::nearestStageIndex(Stage stage, int tenthDb) {
    int best = 0;
    int bestDelta = -1;
    for (int i = 0; i < 16; ++i) {
        const int d = std::abs(stageGainTenthDb(stage, i) - tenthDb);
        if (bestDelta < 0 || d < bestDelta) {
            bestDelta = d;
            best = i;
        }
    }
    return best;
}

void TunerR82xx::stageRangeTenthDb(Stage stage, int& loOut, int& hiOut) {
    loOut = stageGainTenthDb(stage, 0);
    hiOut = loOut;
    for (int i = 1; i < 16; ++i) {
        const int g = stageGainTenthDb(stage, i);
        if (g < loOut) { loOut = g; }
        if (g > hiOut) { hiOut = g; }
    }
}

void TunerR82xx::aggregateRangeTenthDb(int& loOut, int& hiOut) {
    // THE TOP OF THE LADDER IS NOT THE END OF IT. The aggregate walks the two
    // stages up alternately, and the mixer's last step is NEGATIVE, so the
    // loudest setting the walk reaches is one step before the end - 49.6 dB
    // at LNA 15 and mixer 14, not 48.8 at 15 and 15. Taking the final total
    // instead of the peak would advertise a maximum the control can exceed.
    loOut = 0;
    hiOut = 0;
    int total = 0;
    int lna = 0;
    int mix = 0;
    for (int i = 0; i < 15; ++i) {
        total += kLnaSteps[++lna];
        if (total > hiOut) { hiOut = total; }
        total += kMixerSteps[++mix];
        if (total > hiOut) { hiOut = total; }
    }
}

bool TunerR82xx::setStageIndex(Stage stage, int index) {
    if (index < 0) { index = 0; }
    if (index > 15) { index = 15; }
    switch (stage) {
        case Stage::Lna:
            lnaIndex_ = index;
            return writeMask(0x05, static_cast<std::uint8_t>(index), 0x0f);
        case Stage::Mixer:
            mixerIndex_ = index;
            return writeMask(0x07, static_cast<std::uint8_t>(index), 0x0f);
        case Stage::Vga:
            vgaIndex_ = index;
            return applyVgaIndex();
    }
    return false;
}

int TunerR82xx::stageIndex(Stage stage) const {
    switch (stage) {
        case Stage::Lna: return lnaIndex_;
        case Stage::Mixer: return mixerIndex_;
        default: return vgaIndex_;
    }
}

int TunerR82xx::aggregateGainTenthDb() const {
    return stageGainTenthDb(Stage::Lna, lnaIndex_) + stageGainTenthDb(Stage::Mixer, mixerIndex_);
}

bool TunerR82xx::setAggregateGainTenthDb(int tenthDb) {
    // THE WALK. Step the LNA, then the mixer, then the LNA again, stopping as
    // soon as the running total reaches the request. It is not the closest
    // pair of indices to the target and it is not meant to be: this is the
    // ladder every RTL-SDR application's gain list is built from, so a user
    // moving between programs sees the same settings.
    int total = 0;
    int lna = 0;
    int mix = 0;
    for (int i = 0; i < 15; ++i) {
        if (total >= tenthDb) { break; }
        total += kLnaSteps[++lna];
        if (total >= tenthDb) { break; }
        total += kMixerSteps[++mix];
    }

    autoGain_ = false;
    // Manual means: take the LNA and the mixer off their automatic loops
    // first, or the indices we are about to write are overwritten by the part
    // itself within a few milliseconds.
    if (!writeMask(0x05, 0x10, 0x10)) { return false; }
    if (!writeMask(0x07, 0x00, 0x10)) { return false; }
    // The part wants a read between leaving automatic mode and being given
    // manual indices; its detectors latch on the bus cycle.
    std::uint8_t data[4] = {0};
    if (!read(data, 4)) { return false; }
    if (!applyVgaIndex()) { return false; }
    lnaIndex_ = lna;
    mixerIndex_ = mix;
    if (!writeMask(0x05, static_cast<std::uint8_t>(lna), 0x0f)) { return false; }
    return writeMask(0x07, static_cast<std::uint8_t>(mix), 0x0f);
}

bool TunerR82xx::setAutoGain(bool on) {
    autoGain_ = on;
    if (on) {
        if (!writeMask(0x05, 0x00, 0x10)) { return false; }  // LNA loop on
        if (!writeMask(0x07, 0x10, 0x10)) { return false; }  // mixer loop on
        // The VGA has no loop of its own, so it is parked at a fixed 26.5 dB
        // (index 11) while the other two move.
        return writeMask(0x0c, 0x0b, 0x9f);
    }
    if (!writeMask(0x05, 0x10, 0x10)) { return false; }
    if (!writeMask(0x07, 0x00, 0x10)) { return false; }
    if (!applyVgaIndex()) { return false; }
    if (!writeMask(0x05, static_cast<std::uint8_t>(lnaIndex_), 0x0f)) { return false; }
    return writeMask(0x07, static_cast<std::uint8_t>(mixerIndex_), 0x0f);
}

}  // namespace cascade::source
