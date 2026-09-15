// The RTL2832U demodulator, over our own USB transport.
//
// WHAT THIS CHIP IS, because the register map only makes sense once you know.
// The RTL2832U is a DVB-T demodulator, not an SDR front end. Everything this
// class does is the sequence that persuades it to stop demodulating and hand
// the raw ADC stream to the host instead: power the demodulator, put its
// resampler in a mode where the decimation ratio is ours to choose, point its
// digital down-converter at the tuner's intermediate frequency, and read the
// result off a bulk endpoint as 8-bit unsigned interleaved I/Q.
//
// THE PROTOCOL, which is three addressing schemes wearing one control
// transfer. Request 0 on a vendor transfer reaches every block on the chip;
// which block, and whether it is a read or a write, lives in wIndex:
//
//   read  a block register : bmRequestType 0xC0, wValue = address,
//                            wIndex = block << 8
//   write a block register : bmRequestType 0x40, wValue = address,
//                            wIndex = (block << 8) | 0x10
//   read  a demod register : wValue = (address << 8) | 0x20, wIndex = page
//   write a demod register : wValue = (address << 8) | 0x20,
//                            wIndex = 0x10 | page
//
// and the I2C block (6) is a block whose "address" is the I2C slave address,
// so a tuner write is a block write whose payload is {register, value}. The
// tuner is only reachable while the demodulator's I2C REPEATER is switched on
// (demod page 1, register 0x01: 0x18 on, 0x10 off), which is why every tuner
// operation in this driver is bracketed.
//
// TWO ASYMMETRIES THAT LOOK LIKE BUGS AND ARE NOT, both taken from the
// behaviour of the chip rather than from taste:
//   - a two-byte WRITE is big-endian (high byte first) and a two-byte READ is
//     little-endian. Getting this backwards programmes a plausible-looking
//     wrong sample rate.
//   - every demod write is followed by a dummy demod READ of page 0x0a
//     register 0x01. The chip needs the extra bus cycle; without it writes
//     are intermittently dropped.
//
// INDEPENDENT IMPLEMENTATION. The register addresses, initialisation values
// and PLL/resampler arithmetic here are facts about the RTL2832U silicon.
// librtlsdr (GPL-2.0) was consulted as documentation for what those facts
// are; no code, comment, structure or name was taken from it, and none of it
// is linked into this product. See THIRD-PARTY-LICENSES.txt.
//
// THREADING. This class is not internally locked. Its owner (RtlSdrSource)
// serialises every entry with its own device mutex, exactly as SoapySource
// serialises entry into a vendor module, so only one thread is ever inside a
// transfer.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "usb/usb_device.hpp"

namespace cascade::source {

// The blocks request 0 can address. Named for what they are on the die.
enum class RtlBlock : std::uint8_t {
    Demod = 0,
    Usb = 1,
    Sys = 2,
    Tuner = 3,
    Rom = 4,
    Ir = 5,
    I2c = 6,
};

// The USB and system block registers this driver touches.
constexpr std::uint16_t kRegUsbSysCtl = 0x2000;
constexpr std::uint16_t kRegUsbEpaCtl = 0x2148;
constexpr std::uint16_t kRegUsbEpaMaxPkt = 0x2158;
constexpr std::uint16_t kRegSysDemodCtl = 0x3000;
constexpr std::uint16_t kRegSysGpo = 0x3001;
constexpr std::uint16_t kRegSysGpoe = 0x3003;
constexpr std::uint16_t kRegSysGpd = 0x3004;
constexpr std::uint16_t kRegSysDemodCtl1 = 0x300b;

// The crystal every RTL2832U dongle in the field runs from. Everything
// derived from it - the resampler ratio, the DDC frequency, the tuner's PLL
// reference - is wrong by the same proportion if this is wrong, which is
// exactly what the ppm correction exists to trim.
constexpr std::uint32_t kDefaultXtalHz = 28800000;

// The bulk endpoint the raw ADC stream arrives on, and the I/Q sample format
// it arrives in: one unsigned byte per component, I first.
constexpr std::uint8_t kBulkEndpoint = 0x81;

class Rtl2832u {
public:
    // `dev` must outlive this object. `controlTimeoutMs` bounds every control
    // transfer made through it (the caller owns the number so the whole
    // driver's waits live in one place - see rtlsdr_source.cpp).
    Rtl2832u(usb::UsbDevice& dev, unsigned controlTimeoutMs)
        : dev_(dev), controlTimeoutMs_(controlTimeoutMs) {}

    // --- raw register access ------------------------------------------------
    // All return false on a transfer failure, with lastError() set. The read
    // forms return a negative value on failure so a caller can tell 0x00 from
    // "the device did not answer".

    bool writeArray(RtlBlock block, std::uint16_t addr, const std::uint8_t* data,
                    std::uint8_t len);
    bool readArray(RtlBlock block, std::uint16_t addr, std::uint8_t* data, std::uint8_t len);
    bool writeReg(RtlBlock block, std::uint16_t addr, std::uint16_t value, std::uint8_t len);
    int readReg(RtlBlock block, std::uint16_t addr, std::uint8_t len);
    bool demodWriteReg(std::uint8_t page, std::uint16_t addr, std::uint16_t value,
                       std::uint8_t len);
    int demodReadReg(std::uint8_t page, std::uint16_t addr, std::uint8_t len);

    // --- the I2C repeater and the tuner bus ---------------------------------
    bool setI2cRepeater(bool on);
    bool i2cWrite(std::uint8_t slave, const std::uint8_t* data, std::uint8_t len);
    bool i2cRead(std::uint8_t slave, std::uint8_t* data, std::uint8_t len);
    // One register, the shape a tuner probe wants. Negative when the slave
    // did not answer.
    int i2cReadReg(std::uint8_t slave, std::uint8_t reg);

    // --- bring-up -----------------------------------------------------------

    // Powers the demodulator, configures the USB endpoint, loads the FIR
    // coefficients and puts the chip in SDR mode. This is the sequence that
    // turns a DVB-T receiver into an ADC with a USB port.
    bool initBaseband();

    // Powers the demodulator and ADCs back down. Called on close; safe to
    // call on a device that never came up.
    bool deinitBaseband();

    // The digital down-converter's frequency, in Hz. Positive `hz` shifts the
    // spectrum DOWN by that much, which is how the R82xx's 3.57 MHz IF is
    // brought to baseband without a second mixer.
    bool setIfFreqHz(std::uint32_t hz);

    // Trims the resampler's idea of the crystal, in parts per million.
    bool setFreqCorrectionPpm(int ppm);
    int freqCorrectionPpm() const { return ppm_; }

    // The rates the resampler can actually produce. Anything outside these
    // two windows is refused rather than silently coerced to something the
    // chip will not deliver.
    static bool rateSupported(std::uint32_t hz);
    static std::vector<double> supportedRatesHz();

    // Programmes the resampler. `actualHz` receives the rate the chip will
    // really run at, which differs from the request whenever the ratio does
    // not divide evenly - the same number librtlsdr prints as "Exact sample
    // rate is". False (and actualHz untouched) for an unsupported rate.
    bool setSampleRate(std::uint32_t requestedHz, double& actualHz);
    double sampleRateHz() const { return sampleRateHz_; }

    // The crystal as the resampler sees it: the nominal 28.8 MHz with the ppm
    // correction applied. The tuner's PLL reference is derived from the same
    // number, which is why a ppm change retunes as well as re-rates.
    std::uint32_t correctedXtalHz() const;
    std::uint32_t nominalXtalHz() const { return xtalHz_; }

    // --- modes --------------------------------------------------------------

    // The demodulator's own digital AGC. Distinct from the tuner's AGC: this
    // one scales the samples after the ADC.
    bool setDemodAgc(bool on);

    // Direct sampling taps the ADC input straight off a pin, bypassing the
    // tuner entirely - the only way a dongle whose tuner starts at 24 MHz
    // hears HF at all. 0 off, 1 the I branch, 2 the Q branch (which is what
    // the direct-sampling modification on most dongles wires).
    bool setDirectSampling(int mode);
    int directSampling() const { return directSampling_; }

    // One GPIO pin as an output, and its level. GPIO 0 is the bias tee on
    // every dongle that has one; GPIO 5 is the upconverter switch on an
    // RTL-SDR Blog V4.
    bool setGpioOutput(std::uint8_t gpio);
    bool setGpioBit(std::uint8_t gpio, bool high);
    bool setBiasTeeGpio(std::uint8_t gpio, bool on);
    bool setBiasTee(bool on) { return setBiasTeeGpio(0, on); }

    // --- streaming ----------------------------------------------------------

    // Empties the endpoint FIFO so the first buffer after a start is aligned
    // on an I sample rather than half way through one. Toggling EPA_CTL is
    // the documented way; skipping it is why a freshly started stream can
    // come up with I and Q swapped.
    bool resetBuffer();

    // --- identity -----------------------------------------------------------

    // A USB string descriptor by index, through a standard GET_DESCRIPTOR.
    // Empty when the device has no such string. Used only to recognise an
    // RTL-SDR Blog V4, whose manufacturer/product strings are the only thing
    // that distinguishes it from any other R828D dongle.
    std::string stringDescriptor(std::uint8_t index);
    std::string manufacturer() { return stringDescriptor(1); }
    std::string product() { return stringDescriptor(2); }

    // The first `len` bytes of the configuration EEPROM. Byte 7 bit 1 clear
    // means "this dongle's bias tee is wired to be always on".
    bool readEeprom(std::uint8_t* data, std::uint8_t offset, std::uint8_t len);

    const std::string& lastError() const { return lastError_; }
    void clearError() { lastError_.clear(); }

    // True once a transfer has failed. The owner turns this into faulted().
    bool failed() const { return failed_; }

private:
    void note(const char* what);

    usb::UsbDevice& dev_;
    unsigned controlTimeoutMs_;
    std::uint32_t xtalHz_ = kDefaultXtalHz;
    int ppm_ = 0;
    double sampleRateHz_ = 0.0;
    int directSampling_ = 0;
    std::string lastError_;
    bool failed_ = false;
};

}  // namespace cascade::source
