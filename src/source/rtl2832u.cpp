// See rtl2832u.hpp for the protocol and the licence position.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/rtl2832u.hpp"

#include <cmath>
#include <cstring>

namespace cascade::source {

namespace {

// 2^22, the resampler's and the DDC's fixed-point scale. Written once so the
// two arithmetics below cannot drift apart.
constexpr double kQ22 = 4194304.0;

// THE FIR THE WINDOWS DVB DRIVER LOADS, and the reason this driver loads the
// same one. The demodulator's 32-tap symmetric decimating filter runs at the
// crystal rate ahead of the resampler; only the first 16 taps are programmed
// and the chip mirrors them. The DVB driver's own coefficients are tuned for
// an 8 MHz television channel and roll off inside the passband a wideband SDR
// user wants, so every RTL-SDR application loads these instead - they are the
// DAB/FM set, flat across the widest rate the resampler can produce.
//
// The packing is the awkward part and is the chip's, not ours: the first
// eight are signed 8-bit, the last eight are signed 12-bit packed three bytes
// to two coefficients, so 16 coefficients become 20 register writes.
constexpr int kFirTaps[16] = {-54, -36, -41, -40, -32, -14, 14,  53,
                              101, 156, 215, 273, 327, 372, 404, 421};

std::uint16_t blockIndexRead(RtlBlock block) {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(block) << 8);
}

std::uint16_t blockIndexWrite(RtlBlock block) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(block) << 8) | 0x10);
}

}  // namespace

void Rtl2832u::note(const char* what) {
    failed_ = true;
    if (lastError_.empty()) {
        lastError_ = std::string("the radio stopped answering while ") + what + " (" +
                     dev_.lastError() + ")";
    }
}

// --- raw register access ----------------------------------------------------

bool Rtl2832u::writeArray(RtlBlock block, std::uint16_t addr, const std::uint8_t* data,
                          std::uint8_t len) {
    const int r = dev_.controlOut(usb::kRequestTypeVendorOut, 0, addr, blockIndexWrite(block),
                                  data, len, controlTimeoutMs_);
    if (r != static_cast<int>(len)) {
        note("writing a register block");
        return false;
    }
    return true;
}

bool Rtl2832u::readArray(RtlBlock block, std::uint16_t addr, std::uint8_t* data,
                         std::uint8_t len) {
    const int r = dev_.controlIn(usb::kRequestTypeVendorIn, 0, addr, blockIndexRead(block), data,
                                 len, controlTimeoutMs_);
    if (r != static_cast<int>(len)) {
        note("reading a register block");
        return false;
    }
    return true;
}

bool Rtl2832u::writeReg(RtlBlock block, std::uint16_t addr, std::uint16_t value,
                        std::uint8_t len) {
    // Two-byte writes are BIG-endian; see the header's note on the pair of
    // asymmetries. This one, reversed, silently mis-programmes the resampler.
    std::uint8_t data[2];
    data[0] = (len == 1) ? static_cast<std::uint8_t>(value & 0xff)
                         : static_cast<std::uint8_t>(value >> 8);
    data[1] = static_cast<std::uint8_t>(value & 0xff);
    return writeArray(block, addr, data, len);
}

int Rtl2832u::readReg(RtlBlock block, std::uint16_t addr, std::uint8_t len) {
    std::uint8_t data[2] = {0, 0};
    if (!readArray(block, addr, data, len)) { return -1; }
    // ...and two-byte reads are LITTLE-endian.
    return (static_cast<int>(data[1]) << 8) | data[0];
}

bool Rtl2832u::demodWriteReg(std::uint8_t page, std::uint16_t addr, std::uint16_t value,
                             std::uint8_t len) {
    std::uint8_t data[2];
    data[0] = (len == 1) ? static_cast<std::uint8_t>(value & 0xff)
                         : static_cast<std::uint8_t>(value >> 8);
    data[1] = static_cast<std::uint8_t>(value & 0xff);
    const std::uint16_t wValue = static_cast<std::uint16_t>((addr << 8) | 0x20);
    const std::uint16_t wIndex = static_cast<std::uint16_t>(0x10 | page);
    const int r = dev_.controlOut(usb::kRequestTypeVendorOut, 0, wValue, wIndex, data, len,
                                  controlTimeoutMs_);
    if (r != static_cast<int>(len)) {
        note("writing a demodulator register");
        return false;
    }
    // THE DUMMY READ. The demodulator needs one more bus cycle before the
    // write settles; without it writes are dropped at random, which presents
    // as a dongle that tunes to the wrong place once in a while. Its result
    // is deliberately ignored - it is a timing device, not a query.
    demodReadReg(0x0a, 0x01, 1);
    return true;
}

int Rtl2832u::demodReadReg(std::uint8_t page, std::uint16_t addr, std::uint8_t len) {
    std::uint8_t data[2] = {0, 0};
    const std::uint16_t wValue = static_cast<std::uint16_t>((addr << 8) | 0x20);
    const int r = dev_.controlIn(usb::kRequestTypeVendorIn, 0, wValue, page, data, len,
                                 controlTimeoutMs_);
    if (r != static_cast<int>(len)) { return -1; }
    return (static_cast<int>(data[1]) << 8) | data[0];
}

// --- the I2C repeater and the tuner bus -------------------------------------

bool Rtl2832u::setI2cRepeater(bool on) {
    // Demod page 1 register 0x01 doubles as the soft-reset register: bit 3 is
    // the repeater enable, and 0x10 is the quiescent value the reset sequence
    // leaves behind. A tuner write with the repeater off goes nowhere and
    // reports success, which is why every tuner operation brackets itself.
    return demodWriteReg(1, 0x01, on ? 0x18 : 0x10, 1);
}

bool Rtl2832u::i2cWrite(std::uint8_t slave, const std::uint8_t* data, std::uint8_t len) {
    return writeArray(RtlBlock::I2c, slave, data, len);
}

bool Rtl2832u::i2cRead(std::uint8_t slave, std::uint8_t* data, std::uint8_t len) {
    return readArray(RtlBlock::I2c, slave, data, len);
}

int Rtl2832u::i2cReadReg(std::uint8_t slave, std::uint8_t reg) {
    // Address then read, the ordinary I2C register-read shape, expressed as
    // two block transfers because that is all the RTL2832U's I2C block
    // offers.
    std::uint8_t value = 0;
    if (!writeArray(RtlBlock::I2c, slave, &reg, 1)) { return -1; }
    if (!readArray(RtlBlock::I2c, slave, &value, 1)) { return -1; }
    return value;
}

// --- bring-up ---------------------------------------------------------------

bool Rtl2832u::initBaseband() {
    bool ok = true;

    // The USB side first: endpoint A's FIFO depth and the transfer mode the
    // bulk stream will run in.
    ok = writeReg(RtlBlock::Usb, kRegUsbSysCtl, 0x09, 1) && ok;
    ok = writeReg(RtlBlock::Usb, kRegUsbEpaMaxPkt, 0x0002, 2) && ok;
    ok = writeReg(RtlBlock::Usb, kRegUsbEpaCtl, 0x1002, 2) && ok;

    // Power: the demodulator's own supply, then the ADC and PLL block.
    ok = writeReg(RtlBlock::Sys, kRegSysDemodCtl1, 0x22, 1) && ok;
    ok = writeReg(RtlBlock::Sys, kRegSysDemodCtl, 0xe8, 1) && ok;

    // Soft reset, asserted and released (bit 3 of page 1 register 0x01).
    ok = demodWriteReg(1, 0x01, 0x14, 1) && ok;
    ok = demodWriteReg(1, 0x01, 0x10, 1) && ok;

    // No spectrum inversion and no adjacent-channel rejection: both are
    // DVB-T features that would filter the very signal we want to keep.
    ok = demodWriteReg(1, 0x15, 0x00, 1) && ok;
    ok = demodWriteReg(1, 0x16, 0x0000, 2) && ok;

    // Clear the down-converter's shift and IF registers. Six single-byte
    // writes rather than one wide one: 0x16..0x1b span two logical registers
    // and the chip latches them a byte at a time.
    for (std::uint16_t i = 0; i < 6; ++i) {
        ok = demodWriteReg(1, static_cast<std::uint16_t>(0x16 + i), 0x00, 1) && ok;
    }

    // The decimating FIR, packed as the chip wants it (see kFirTaps).
    std::uint8_t fir[20] = {0};
    for (int i = 0; i < 8; ++i) { fir[i] = static_cast<std::uint8_t>(kFirTaps[i]); }
    for (int i = 0; i < 8; i += 2) {
        const int a = kFirTaps[8 + i];
        const int b = kFirTaps[8 + i + 1];
        const int base = 8 + i * 3 / 2;
        fir[base] = static_cast<std::uint8_t>(a >> 4);
        fir[base + 1] = static_cast<std::uint8_t>((a << 4) | ((b >> 8) & 0x0f));
        fir[base + 2] = static_cast<std::uint8_t>(b);
    }
    for (int i = 0; i < 20; ++i) {
        ok = demodWriteReg(1, static_cast<std::uint16_t>(0x1c + i), fir[i], 1) && ok;
    }

    // SDR mode with the demodulator's digital AGC off. This is the write that
    // stops the chip being a television receiver.
    ok = demodWriteReg(0, 0x19, 0x05, 1) && ok;

    // The DVB state machine's holding registers, parked.
    ok = demodWriteReg(1, 0x93, 0xf0, 1) && ok;
    ok = demodWriteReg(1, 0x94, 0x0f, 1) && ok;

    // Every AGC loop the demodulator owns, off: RF, IF and the digital one.
    // A loop left running fights whatever gain the user sets on the tuner.
    ok = demodWriteReg(1, 0x11, 0x00, 1) && ok;
    ok = demodWriteReg(1, 0x04, 0x00, 1) && ok;

    // No transport-stream PID filtering - there is no transport stream.
    ok = demodWriteReg(0, 0x61, 0x60, 1) && ok;

    // The default ADC I/Q data path (not the swapped one direct sampling
    // uses).
    ok = demodWriteReg(0, 0x06, 0x80, 1) && ok;

    // Zero-IF input, DC cancellation and I/Q imbalance estimation and
    // correction all on. The R82xx path turns the zero-IF bit off again when
    // the tuner is identified; leaving it on here keeps a tunerless dongle
    // (direct sampling) working.
    ok = demodWriteReg(1, 0xb1, 0x1b, 1) && ok;

    // The 4.096 MHz test clock output, off: it is a spur inside our own band.
    ok = demodWriteReg(0, 0x0d, 0x83, 1) && ok;

    return ok;
}

bool Rtl2832u::deinitBaseband() {
    // Demodulator and ADCs powered down. Everything else the chip forgets on
    // the next initBaseband().
    return writeReg(RtlBlock::Sys, kRegSysDemodCtl, 0x20, 1);
}

std::uint32_t Rtl2832u::correctedXtalHz() const {
    return static_cast<std::uint32_t>(static_cast<double>(xtalHz_) *
                                      (1.0 + static_cast<double>(ppm_) / 1e6));
}

bool Rtl2832u::setIfFreqHz(std::uint32_t hz) {
    // The down-converter shifts DOWN, so the register holds the negative of
    // the requested frequency in units of xtal/2^22. Twenty-two bits of it,
    // split across three registers, top six bits first.
    const std::int32_t ifFreq = -static_cast<std::int32_t>(
        (static_cast<double>(hz) * kQ22) / static_cast<double>(correctedXtalHz()));
    bool ok = true;
    ok = demodWriteReg(1, 0x19, static_cast<std::uint16_t>((ifFreq >> 16) & 0x3f), 1) && ok;
    ok = demodWriteReg(1, 0x1a, static_cast<std::uint16_t>((ifFreq >> 8) & 0xff), 1) && ok;
    ok = demodWriteReg(1, 0x1b, static_cast<std::uint16_t>(ifFreq & 0xff), 1) && ok;
    return ok;
}

bool Rtl2832u::setFreqCorrectionPpm(int ppm) {
    ppm_ = ppm;
    // The resampler's own trim, in units of 1/2^24 of the sample clock, sign
    // inverted for the same reason the IF register is: the register asks
    // "how far to pull the clock", not "how far is it out".
    const std::int16_t offs =
        static_cast<std::int16_t>(-static_cast<double>(ppm) * 16777216.0 / 1e6);
    bool ok = true;
    ok = demodWriteReg(1, 0x3f, static_cast<std::uint16_t>(offs & 0xff), 1) && ok;
    ok = demodWriteReg(1, 0x3e, static_cast<std::uint16_t>((offs >> 8) & 0x3f), 1) && ok;
    return ok;
}

bool Rtl2832u::rateSupported(std::uint32_t hz) {
    // TWO WINDOWS, NOT ONE RANGE. The resampler's ratio register cannot
    // express the gap between 300 kS/s and 900 kS/s, and above 3.2 MS/s the
    // USB endpoint drops buffers on most hosts. A rate outside these is
    // refused here rather than programmed and silently not delivered.
    if (hz > 225000 && hz <= 300000) { return true; }
    if (hz > 900000 && hz <= 3200000) { return true; }
    return false;
}

std::vector<double> Rtl2832u::supportedRatesHz() {
    // The rates a user actually picks, ascending: the Source panel offers a
    // list, not a slider, and these are the ones every RTL-SDR application
    // agrees on. Anything else inside the windows above is still accepted by
    // setSampleRate(); this is the menu, not the limit.
    return {250000.0,  1024000.0, 1200000.0, 1400000.0, 1800000.0,
            1920000.0, 2048000.0, 2160000.0, 2400000.0, 2560000.0,
            2880000.0, 3200000.0};
}

bool Rtl2832u::setSampleRate(std::uint32_t requestedHz, double& actualHz) {
    if (!rateSupported(requestedHz)) {
        lastError_ = "that sample rate is outside what the RTL2832U's resampler can produce";
        return false;
    }
    // THE RESAMPLER RATIO. 2^22 crystal periods per output sample, in a
    // register whose bottom two bits are not implemented - hence the mask.
    // The odd-looking `real` term mirrors bit 27 into bit 28, which is how
    // the chip reads ratios past 2^27; computing the achieved rate without it
    // reports a number the dongle is not running at.
    //
    // NOTE the crystal used here is the NOMINAL one, not the ppm-corrected
    // one: the ppm trim is applied separately through the resampler's own
    // correction register below, and applying it twice would double it.
    const double xtal = static_cast<double>(xtalHz_);
    std::uint32_t ratio = static_cast<std::uint32_t>((xtal * kQ22) / static_cast<double>(requestedHz));
    ratio &= 0x0ffffffc;
    const std::uint32_t real = ratio | ((ratio & 0x08000000) << 1);
    const double realRate = (xtal * kQ22) / static_cast<double>(real);

    bool ok = true;
    ok = demodWriteReg(1, 0x9f, static_cast<std::uint16_t>(ratio >> 16), 2) && ok;
    ok = demodWriteReg(1, 0xa1, static_cast<std::uint16_t>(ratio & 0xffff), 2) && ok;
    ok = setFreqCorrectionPpm(ppm_) && ok;
    // Soft reset, so the resampler restarts on the new ratio rather than
    // finishing the buffer it was part way through at the old one.
    ok = demodWriteReg(1, 0x01, 0x14, 1) && ok;
    ok = demodWriteReg(1, 0x01, 0x10, 1) && ok;

    if (ok) {
        sampleRateHz_ = realRate;
        actualHz = realRate;
    }
    return ok;
}

// --- modes ------------------------------------------------------------------

bool Rtl2832u::setDemodAgc(bool on) {
    // The same register that selected SDR mode: bit 5 is the digital AGC.
    return demodWriteReg(0, 0x19, on ? 0x25 : 0x05, 1);
}

bool Rtl2832u::setDirectSampling(int mode) {
    bool ok = true;
    if (mode > 0) {
        // Zero-IF off - there is no mixer in this path, the ADC is the
        // receiver.
        ok = demodWriteReg(1, 0xb1, 0x1a, 1) && ok;
        ok = demodWriteReg(1, 0x15, 0x00, 1) && ok;
        // One ADC only, and which of the two pins it listens to: mode 1 is
        // the I input, mode 2 the Q input, which is where the usual
        // direct-sampling modification puts the antenna.
        ok = demodWriteReg(0, 0x08, 0x4d, 1) && ok;
        ok = demodWriteReg(0, 0x06, (mode > 1) ? 0x90 : 0x80, 1) && ok;
        directSampling_ = mode;
    } else {
        ok = demodWriteReg(0, 0x06, 0x80, 1) && ok;
        directSampling_ = 0;
    }
    return ok;
}

bool Rtl2832u::setGpioOutput(std::uint8_t gpio) {
    const std::uint8_t bit = static_cast<std::uint8_t>(1u << gpio);
    const int d = readReg(RtlBlock::Sys, kRegSysGpd, 1);
    if (d < 0) { return false; }
    if (!writeReg(RtlBlock::Sys, kRegSysGpd, static_cast<std::uint16_t>(d & ~bit), 1)) {
        return false;
    }
    const int e = readReg(RtlBlock::Sys, kRegSysGpoe, 1);
    if (e < 0) { return false; }
    return writeReg(RtlBlock::Sys, kRegSysGpoe, static_cast<std::uint16_t>(e | bit), 1);
}

bool Rtl2832u::setGpioBit(std::uint8_t gpio, bool high) {
    const std::uint8_t bit = static_cast<std::uint8_t>(1u << gpio);
    const int v = readReg(RtlBlock::Sys, kRegSysGpo, 1);
    if (v < 0) { return false; }
    const int next = high ? (v | bit) : (v & ~bit);
    return writeReg(RtlBlock::Sys, kRegSysGpo, static_cast<std::uint16_t>(next), 1);
}

bool Rtl2832u::setBiasTeeGpio(std::uint8_t gpio, bool on) {
    if (!setGpioOutput(gpio)) { return false; }
    return setGpioBit(gpio, on);
}

// --- streaming --------------------------------------------------------------

bool Rtl2832u::resetBuffer() {
    bool ok = true;
    ok = writeReg(RtlBlock::Usb, kRegUsbEpaCtl, 0x1002, 2) && ok;
    ok = writeReg(RtlBlock::Usb, kRegUsbEpaCtl, 0x0000, 2) && ok;
    return ok;
}

// --- identity ---------------------------------------------------------------

std::string Rtl2832u::stringDescriptor(std::uint8_t index) {
    if (index == 0) { return std::string(); }
    // A STANDARD request, not a vendor one: bmRequestType 0x80,
    // bRequest 6 (GET_DESCRIPTOR), wValue = (STRING << 8) | index,
    // wIndex = language id. WinUSB permits standard descriptor reads, so this
    // needs nothing the transport does not already offer.
    std::uint8_t buf[256] = {0};
    const int r = dev_.controlIn(0x80, 0x06, static_cast<std::uint16_t>((0x03 << 8) | index),
                                 0x0409, buf, sizeof(buf), controlTimeoutMs_);
    if (r < 4 || buf[1] != 0x03) { return std::string(); }
    const int bytes = (buf[0] < r) ? buf[0] : r;
    // UTF-16LE, and every string this is used for is ASCII, so the low byte
    // of each unit is the character. A non-ASCII unit becomes '?' rather than
    // dragging a UTF-8 converter into a driver.
    std::string out;
    for (int i = 2; i + 1 < bytes; i += 2) {
        const unsigned unit = static_cast<unsigned>(buf[i]) | (static_cast<unsigned>(buf[i + 1]) << 8);
        out.push_back(unit < 0x80 ? static_cast<char>(unit) : '?');
    }
    return out;
}

bool Rtl2832u::readEeprom(std::uint8_t* data, std::uint8_t offset, std::uint8_t len) {
    // The EEPROM hangs off the same I2C block at slave address 0xa0. Its
    // address pointer auto-increments, so one write of the offset then `len`
    // single-byte reads.
    constexpr std::uint8_t kEepromSlave = 0xa0;
    if (!writeArray(RtlBlock::I2c, kEepromSlave, &offset, 1)) { return false; }
    for (std::uint8_t i = 0; i < len; ++i) {
        if (!readArray(RtlBlock::I2c, kEepromSlave, data + i, 1)) { return false; }
    }
    return true;
}

}  // namespace cascade::source
