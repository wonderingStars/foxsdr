// rx888_protocol.hpp - the RX888 mk2's FX3 vendor protocol, and the real-to-IQ
// conversion, as numbers and pure functions with nothing in them that can
// touch a device.
//
// WHY IT IS A SEPARATE HEADER FROM THE DRIVER, the same reason
// hackrf_protocol.hpp is: everything here is arithmetic that the SDDC
// firmware and its host agree on - which request number means "start the
// ADC", how a DAT-31 attenuator code becomes decibels, how a 64-bit tuner
// frequency is laid out in the payload, how a 16-bit real ADC stream becomes
// the complex baseband the pipeline wants. None of it needs a radio to be
// checked, and THERE IS NO RX888 ON THE BENCH THIS WAS WRITTEN ON, so all of
// it is proven against a fake that answers as the firmware does and against a
// brute-force implementation of the same DSP written a second time in the
// test.
//
// PORTED FROM ExtIO_sddc by Oscar Steila (IK1XPV), WHICH IS MIT. The
// numbers below are read out of that project and out of the SDDC_FX3 firmware
// it ships, and each one names the file and line it came from. The notice is
// reproduced in installer/THIRD-PARTY-LICENSES.txt under COMPONENT:
// ExtIO_sddc. Nothing from it is linked or shipped except the FX3 firmware
// image itself (src/source/rx888_firmware.cpp), which is the same MIT image
// ExtIO embeds and without which the hardware is a Cypress bootloader and
// nothing else.
//
// ONE FILE IN THAT PACK IS DELIBERATELY NOT USED. libsddc.{h,cpp} - the small
// public API that would have been the cleanest map of what a host needs - is
// Copyright (C) 2020 Franco Venturi and carries SPDX-License-Identifier:
// GPL-3.0-or-later in its own banner, not the MIT of the rest. Neither is
// Core/arch/linux/ezusb.c, which is fxload's GPL-2+. So the FX3 boot protocol
// below is written from Cypress's own image format (AN76405) as verified
// byte-for-byte against the shipped SDDC_FX3.img, and the device-side
// semantics from the MIT firmware sources; the GPL files were read to
// corroborate and nothing was taken from them.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "dsp/fir.hpp"
#include "dsp/nco.hpp"
#include "dsp/window.hpp"

namespace cascade::source::rx888 {

// --- identity -------------------------------------------------------------

// ExtIO_sddc Core/arch/win32/FX3handler.h:22-24, and the same pair in
// Core/arch/linux/usb_device.c:67-68 with its own comment naming them.
//
// AN RX888 HAS TWO USB IDENTITIES AND BOTH MATTER. Out of a power cycle the
// FX3's on-chip bootloader enumerates as 04B4:00F3 with no firmware at all;
// the SDDC image we upload re-enumerates it as 04B4:00F1, which is the only
// identity that can stream. A user therefore has to bind WinUSB to BOTH
// (see the Zadig note in the Source section), and this driver lists a
// bootloader device as a radio that needs firmware rather than hiding it -
// "not in the list" is exactly the message that cost the RTL-SDR path several
// releases of confused reports.
constexpr std::uint16_t kUsbVid = 0x04B4;
constexpr std::uint16_t kPidBootloader = 0x00F3;
constexpr std::uint16_t kPidStreamer = 0x00F1;

// SDDC_FX3/Application.h:37 - CY_FX_EP_CONSUMER, "EP 1 IN". Its descriptor
// (SDDC_FX3/USBdescriptor.c:118-131) declares a 1024-byte SuperSpeed bulk
// endpoint with a burst of 16, which is where config.h's "transferSize must
// be a multiple of 16 (maxBurst) * 1024 (SS packet size) = 16384" comes from.
constexpr std::uint8_t kRxEndpoint = 0x81;

// --- the vendor requests --------------------------------------------------

// ExtIO_sddc Interface.h:7-68, enum FX3Command, with that file's own comments
// on what each one carries. Every one of them is a VENDOR request on endpoint
// zero; the firmware's handler is SDDC_FX3/USBhandler.c:213-540.
//
// Only what a receiver needs is named. I2CWFX3/I2CRFX3 (raw I2C to anything
// on the bus) and READINFODEBUG (the firmware's debug console) are absent on
// purpose: a request number this driver cannot name is a request number it
// cannot send by mistake, and raw I2C on a radio whose tuner we do not own is
// the one that can brick something.
enum class Command : std::uint8_t {
    StartFx3 = 0xAA,     // start the GPIF producer (USBhandler.c:466)
    StopFx3 = 0xAB,      // stop it (USBhandler.c:479)
    TestFx3 = 0xAC,      // IN 4: model, firmware hi, firmware lo, request count
    GpioFx3 = 0xAD,      // OUT 4: the whole GPIO word (USBhandler.c:219)
    ResetFx3 = 0xB1,     // reboot into the bootloader (USBhandler.c:489)
    StartAdc = 0xB2,     // OUT 4: the ADC clock in Hz (USBhandler.c:264)
    TunerInit = 0xB4,    // OUT 4: the tuner reference clock (USBhandler.c:300)
    TunerTune = 0xB5,    // OUT 8: the tuner LO in Hz (USBhandler.c:354)
    SetArgFx3 = 0xB6,    // value/index carry the argument (USBhandler.c:378)
    TunerStandby = 0xB8  // stop the tuner (USBhandler.c:330)
};

constexpr std::uint8_t requestByte(Command c) { return static_cast<std::uint8_t>(c); }

// Interface.h:117-149, enum ArgumentList: which knob a SETARGFX3 addresses.
// The value rides in wValue and the argument number in wIndex
// (Core/arch/win32/FX3handler.cpp:171-185), with a single zero byte as the
// payload - the firmware reads wLength bytes and then ignores them
// (USBhandler.c:378-382), but it reads them, so the byte has to be sent.
enum class Argument : std::uint16_t {
    R82xxAttenuator = 1,  // the VHF tuner's LNA/mixer gain index, 0-28
    R82xxVga = 2,         // the VHF tuner's VGA index, 0-15
    R82xxSideband = 3,
    R82xxHarmonic = 4,
    Dat31Att = 10,   // the HF attenuator, 0-63 in half-decibels
    Ad8340Vga = 11,  // the HF IF amplifier, a mode bit and a step
    Preselector = 12,
    VhfAttenuator = 13,
};

// Interface.h:70-104. The GPIO word is written whole, every time, so the
// driver has to keep a mirror of it - a set/clear of one bit is a read of our
// own copy and a write of all 32 (ExtIO_sddc Core/RadioHardware.cpp:3-15,
// FX3SetGPIO / FX3UnsetGPIO).
enum Gpio : std::uint32_t {
    kGpioShdwn = 1u << 5,      // SHDWN
    kGpioDither = 1u << 6,     // DITH - the ADC's dither generator
    kGpioRandom = 1u << 7,     // RAND - the ADC's output randomiser
    kGpioBiasHf = 1u << 8,     // BIAS_HF
    kGpioBiasVhf = 1u << 9,    // BIAS_VHF
    kGpioLedYellow = 1u << 10,
    kGpioLedRed = 1u << 11,
    kGpioLedBlue = 1u << 12,
    kGpioAttSel0 = 1u << 13,
    kGpioAttSel1 = 1u << 14,
    kGpioVhfEn = 1u << 15,  // RX888 mk2: switch the front end to the tuner
    kGpioPgaEn = 1u << 16,  // RX888 mk2: the HF PGA
};

// Interface.h:106-115, enum RadioModel - byte 0 of the TESTFX3 answer. This
// driver opens the mk2 and says so about anything else: the gain tables, the
// attenuator and the VHF switching below are RX888R2Radio's, and an RX999 or
// an HF103 answering the same firmware would be programmed wrongly by them.
enum class Model : std::uint8_t {
    None = 0x00,
    Bbrf103 = 0x01,
    Hf103 = 0x02,
    Rx888 = 0x03,
    Rx888mk2 = 0x04,
    Rx999 = 0x05,
    RxLucy = 0x06,
    Rx888mk3 = 0x07,
};

inline const char* modelName(Model m) {
    switch (m) {
        case Model::Bbrf103: return "BBRF103";
        case Model::Hf103: return "HF103";
        case Model::Rx888: return "RX888";
        case Model::Rx888mk2: return "RX888 mkII";
        case Model::Rx999: return "RX999";
        case Model::RxLucy: return "Lucy";
        case Model::Rx888mk3: return "RX888 mkIII";
        case Model::None:
        default: return "unknown SDDC device";
    }
}

// Interface.h:3-4, FIRMWARE_VER_MAJOR/MINOR - the version the SDDC_FX3.img we
// embed reports through TESTFX3. A device answering anything else is running
// somebody else's build of the firmware; the driver logs that and carries on,
// because 2.x has not moved any of the request numbers above.
constexpr std::uint16_t kFirmwareVersion = 0x0202;

// --- the timeouts ---------------------------------------------------------

// The ordinary per-control-transfer bound. The reference's Linux backend uses
// 5000 ms for every command (Core/arch/linux/usb_device.c:359); this driver
// splits that in two, because one command is slow and the rest are not, and
// the slow one is not on the shutdown path. Behind every request that uses
// THIS number is a GPIO write, a register write or a short I2C burst.
//
// 500 rather than 1000 BECAUSE OF THE SHUTDOWN BUDGET, and the arithmetic is
// worth writing down. An RX888 teardown can spend, at worst: one of these
// for the STOPFX3 in stopStreamingLocked, kReaderJoinWait for the bounded
// join, then TWO more of these in closeDevice (the tuner to standby when the
// radio was in VHF, and the GPIO word that unpowers both bias tees), and
// finally the transport's kAbortDrainWait. At 500 that is
// 500 + 1000 + 500 + 500 + 250 = 2750 ms, which fits inside the 3000 ms the
// Soapy column already charges and which an RX888 teardown is spent INSTEAD
// OF. At 1000 it would be 4250 and would not.
//
// A std::chrono duration rather than a bare unsigned so
// tests/test_shutdown_budget.cpp's scan can DISCOVER it; the transport takes
// milliseconds as an unsigned, so the call sites convert.
constexpr std::chrono::milliseconds kControlTimeout{500};
constexpr unsigned kControlTimeoutMs = static_cast<unsigned>(kControlTimeout.count());

// STARTADC, AND ONLY STARTADC. Its firmware handler programs the Si5351 and
// then SLEEPS FOR A FULL SECOND before it answers - SDDC_FX3/USBhandler.c:
// 264-273, `si5351aSetFrequencyA(freq); CyU3PThreadSleep(1000);` - so the
// ordinary bound above would time out on the one request that ALWAYS takes a
// second. This is the number the reference's blanket 5000 was really for.
//
// It is NOT on the teardown path: the ADC clock is set at open and at a
// sample-rate change, never at stop.
constexpr std::chrono::milliseconds kAdcStartTimeout{3000};
constexpr unsigned kAdcStartTimeoutMs = static_cast<unsigned>(kAdcStartTimeout.count());

// --- the bulk stream ------------------------------------------------------

// ExtIO_sddc config.h:80-82. transferSize is 131072 and must stay a multiple
// of 16384 (the endpoint's burst x packet size); concurrentTransfers is 16,
// with that file's own note that 96 "is too high".
//
// THE NUMBERS THIS DRIVER CHOSE, AND WHY, because they are the ones that
// decide whether a 128 MB/s stream survives a late host. At 64 MS/s of
// 16-bit real samples the radio delivers 128 MB/s: one 131072-byte transfer
// is 1.02 ms of signal, and sixteen of them queued is 16.4 ms of runway
// before a scheduling hiccup on the reader thread costs samples. The
// reference queues only FOUR on the wire (Core/arch/win32/FX3handler.cpp:332,
// USB_READ_CONCURRENT) but hands each one straight into a 32-block ring
// behind it; our transport has no second queue, so the runway has to be in
// the ring of overlapped reads itself, and 16 is what puts it back where the
// reference has it.
constexpr std::size_t kTransferBufferBytes = 131072;
constexpr std::size_t kTransferCount = 16;
constexpr std::size_t kBytesPerAdcSample = 2;
constexpr std::size_t kAdcSamplesPerTransfer = kTransferBufferBytes / kBytesPerAdcSample;

// --- the ADC clock and the rates ------------------------------------------

// ExtIO_sddc config.h:84 (DEFAULT_ADC_FREQ) and config.cpp:6-8 (MIN/MAX).
// The mk2's LTC2208 is clocked from the Si5351 and the reference will drive
// it anywhere in 50-140 MHz; 64 MHz is what it ships at and what every
// published mk2 measurement is taken at, so it is what this driver uses and
// the only clock its rate table is derived from.
constexpr std::uint32_t kDefaultAdcRateHz = 64000000;
constexpr std::uint32_t kMinAdcRateHz = 50000000;
constexpr std::uint32_t kMaxAdcRateHz = 140000000;

// RX888R2Radio.cpp:53-62, PrepareLo: below 10 kHz and above 1750 MHz the
// reference refuses to tune at all.
constexpr double kMinFrequencyHz = 10.0e3;
constexpr double kMaxFrequencyHz = 1750.0e6;

// RX888R2Radio.cpp:58-61: the ADC's own Nyquist is the boundary between
// direct sampling and the tuner. At the default clock that is 32 MHz, which
// is also config.h:59's HF_HIGH.
constexpr double kHfCeilingHz(std::uint32_t adcRateHz) {
    return static_cast<double>(adcRateHz) / 2.0;
}

// RX888R2Radio.cpp:3-4. The mk2's VHF front end is an R828D fed a 16 MHz
// reference, and its output lands in the ADC's band at a 4.57 MHz IF with the
// spectrum INVERTED (RadioHandler.cpp:266-270 turns the r2iq's sideband flag
// on for VHF, and :289-290 flips the sign of the fine-tuning offset with it).
constexpr std::uint32_t kTunerReferenceHz = 16000000;
constexpr double kTunerIfHz = 4570000.0;

// --- the HF attenuator (DAT-31) -------------------------------------------

// RX888R2Radio.cpp:24-37. The reference builds a 64-entry table by summing
// the bits of the index against 0.5/1/2/4/8/16 dB, which is the long way of
// writing -0.5 dB per code: hf_rf_steps[63 - i] = -0.5 * i, so the table runs
// from -31.5 dB at index 0 to 0 dB at index 63, and UpdateattRF (:91-104)
// sends code d = 63 - index. The code the DAT-31 actually receives is
// therefore the attenuation in half-decibels, and that identity is what this
// driver uses instead of the table.
constexpr double kHfAttMinDb = -31.5;
constexpr double kHfAttMaxDb = 0.0;
constexpr double kHfAttStepDb = 0.5;
constexpr int kHfAttCodeMax = 63;

// dB (negative, an attenuation) -> the DAT-31 code. Clamped, not refused:
// device_source.hpp's contract is that an out-of-range gain is clamped and
// gainDb() reports what was programmed.
inline std::uint16_t hfAttCode(double db) {
    if (!(db < kHfAttMaxDb)) { return 0; }  // negated compare so NaN lands here
    const double clamped = std::max(db, kHfAttMinDb);
    const int code = static_cast<int>(-clamped / kHfAttStepDb + 0.5);
    return static_cast<std::uint16_t>(std::min(std::max(code, 0), kHfAttCodeMax));
}

inline double hfAttDbForCode(std::uint16_t code) {
    return -kHfAttStepDb * static_cast<double>(std::min<std::uint16_t>(code, kHfAttCodeMax));
}

// --- the HF IF amplifier (AD8340) -----------------------------------------

// RX888R2Radio.cpp:38-45 builds the 127-entry dB table and :158-172 turns an
// index into the byte the chip gets:
//
//   index <= 18 : 20*log10(0.059 * (index + 1)),  code = index + 1
//   index >  18 : 20*log10(0.409 * (index - 15)), code = 0x80 | (index - 15)
//
// i.e. the amplifier has a low-gain and a high-gain mode with a step ratio
// each, and 0x80 selects the high one. The steps are anything but uniform -
// 6.0 dB from index 0 to 1, 0.08 dB from 125 to 126 - so the driver keeps the
// table and snaps a requested gain to the nearest entry, which is the only
// honest way to answer "what did you actually set".
constexpr int kHfVgaSteps = 127;
constexpr int kHfVgaSweetPoint = 18;  // GAIN_SWEET_POINT, RX888R2Radio.cpp:9
constexpr double kHfVgaLowRatio = 0.059;   // LOW_GAIN_RATIO, :11
constexpr double kHfVgaHighRatio = 0.409;  // HIGH_GAIN_RATIO, :10

inline double hfVgaDbForIndex(int index) {
    const int i = std::min(std::max(index, 0), kHfVgaSteps - 1);
    if (i > kHfVgaSweetPoint) {
        return 20.0 * std::log10(kHfVgaHighRatio * static_cast<double>(i - kHfVgaSweetPoint + 3));
    }
    return 20.0 * std::log10(kHfVgaLowRatio * static_cast<double>(i + 1));
}

// RX888R2Radio.cpp:163-169, the byte SETARGFX3(AD8340_VGA) carries.
inline std::uint16_t hfVgaCodeForIndex(int index) {
    const int i = std::min(std::max(index, 0), kHfVgaSteps - 1);
    if (i > kHfVgaSweetPoint) {
        return static_cast<std::uint16_t>(0x80 | (i - kHfVgaSweetPoint + 3));
    }
    return static_cast<std::uint16_t>(i + 1);
}

// The nearest table entry to a requested gain. Nearest rather than "the
// largest not above", because the table's steps range from 6 dB to 0.08 dB
// and rounding down through a 6 dB step would put a user 6 dB below what they
// asked for with nothing on screen to say why.
inline int hfVgaIndexForDb(double db) {
    int best = 0;
    double bestErr = -1.0;
    for (int i = 0; i < kHfVgaSteps; ++i) {
        const double err = std::fabs(hfVgaDbForIndex(i) - db);
        if (bestErr < 0.0 || err < bestErr) {
            bestErr = err;
            best = i;
        }
    }
    return best;
}

// --- the VHF tuner's two gains --------------------------------------------

// RX888R2Radio.cpp:15-19 (vhf_rf_steps, 29 entries) and :21-22 (vhf_if_steps,
// 16 entries) - the R828D's LNA/mixer chain and its VGA, in dB, as the
// reference publishes them. The index IS what SETARGFX3 carries
// (R82XX_ATTENUATOR and R82XX_VGA), so these tables are both the dB scale and
// the wire value.
constexpr int kVhfRfSteps = 29;
constexpr int kVhfIfSteps = 16;

// kVhfRfSteps entries, ascending. RX888R2Radio.cpp:15-19.
inline const double* vhfRfTable() {
    static const double kTable[kVhfRfSteps] = {
        0.0,  0.9,  1.4,  2.7,  3.7,  7.7,  8.7,  12.5, 14.4, 15.7,
        16.6, 19.7, 20.7, 22.9, 25.4, 28.0, 29.7, 32.8, 33.8, 36.4,
        37.2, 38.6, 40.2, 42.1, 43.4, 43.9, 44.5, 48.0, 49.6};
    return kTable;
}

// kVhfIfSteps entries, ascending. RX888R2Radio.cpp:21-22.
inline const double* vhfIfTable() {
    static const double kTable[kVhfIfSteps] = {-4.7, -2.1, 0.5,  3.5,  7.7,  11.2, 13.6, 14.9,
                                               16.3, 19.5, 23.1, 26.5, 30.0, 33.7, 37.2, 40.8};
    return kTable;
}

// Nearest entry, for the same reason as the HF VGA above.
inline int nearestIndex(const double* table, int count, double db) {
    int best = 0;
    double bestErr = -1.0;
    for (int i = 0; i < count; ++i) {
        const double err = std::fabs(table[i] - db);
        if (bestErr < 0.0 || err < bestErr) {
            bestErr = err;
            best = i;
        }
    }
    return best;
}

// --- payload encoding -----------------------------------------------------

// Every FX3Command that carries data carries it as a little-endian integer in
// the data stage, with wValue and wIndex zero
// (Core/arch/win32/FX3handler.cpp:125-168). Written out byte by byte rather
// than memcpy'd off a uint32, so the wire layout is the same on a big-endian
// host - which is what the reference's `(PUCHAR)&data` quietly is not.
constexpr std::size_t kU32PayloadBytes = 4;
constexpr std::size_t kU64PayloadBytes = 8;

inline void encodeU32(std::uint32_t v, std::uint8_t out[kU32PayloadBytes]) {
    for (std::size_t b = 0; b < 4; ++b) {
        out[b] = static_cast<std::uint8_t>((v >> (8 * b)) & 0xFF);
    }
}

inline void encodeU64(std::uint64_t v, std::uint8_t out[kU64PayloadBytes]) {
    for (std::size_t b = 0; b < 8; ++b) {
        out[b] = static_cast<std::uint8_t>((v >> (8 * b)) & 0xFF);
    }
}

// STARTFX3, STOPFX3 and TUNERSTDBY are sent through fx3class's ONE-BYTE
// overload, because that is the one carrying the default argument:
// Core/FX3Class.h:97 declares `Control(FX3Command, uint8_t data = 0)`, so
// `Fx3->Control(STARTFX3)` (RadioHandler.h:149-150) and
// `Fx3->Control(TUNERSTDBY)` (RX888R2Radio.cpp:79) each put a single zero
// byte in the data stage. The firmware reads wLength bytes and discards them,
// so the length is not load-bearing for the hardware - but it is what the
// reference sends, and "byte for byte as the reference" is the only claim
// this driver can make without an RX888 to try it on.
constexpr std::size_t kEmptyPayloadBytes = 1;

// --- the ADC randomiser ---------------------------------------------------

// fft_mt_r2iq.h:189-204, convert_float<rand>. When the ADC's output
// randomiser is on, the LTC2208 inverts bits 15..1 of every sample whose bit
// 0 is set; undoing it is the same operation. `^ (-2)` in the reference is
// `^ 0xFFFE` on a 16-bit two's complement value.
inline std::int16_t derandomise(std::int16_t v) {
    return (v & 1) != 0 ? static_cast<std::int16_t>(v ^ static_cast<std::int16_t>(0xFFFE)) : v;
}

// --- the real-to-IQ conversion --------------------------------------------
//
// WHAT THE HARDWARE GIVES US AND WHAT THE PIPELINE WANTS. The RX888 is a
// direct-sampling receiver: the bulk stream is 16-bit REAL samples of the
// whole 0 - 32 MHz band at 64 MS/s, and every stage downstream of a source in
// FoxSDR takes complex baseband at a rate it can actually run. Something has
// to translate, filter and decimate, and on this radio that something is the
// host - there is no DDC in the FX3.
//
// WHY THIS IS NOT THE REFERENCE'S ENGINE, stated plainly because it is the
// one place this port deviates. ExtIO_sddc does the job in the frequency
// domain (fft_mt_r2iq.cpp and fft_mt_r2iq_impl.hpp): an 8192-point real FFT
// with 25% overlap, a circular shift by a "tune bin" that selects the band,
// a complex multiply by a Kaiser filter's spectrum, and a shorter inverse FFT
// that decimates - overlap-save, with the negative-frequency bins zeroed so
// the same pass also does the Hilbert transform. It is a good design and it
// is much cheaper than what is below. It also quantises the centre frequency
// to four FFT bins and then needs a SECOND, separate fine mixer
// (RadioHandler.cpp:279-299 and pffft's pf_mixer) to make up the difference,
// it is written around fftw3 - which this project does not vendor - and its
// bookkeeping is welded to its own buffer sizes.
//
// So the conversion here is the textbook multirate one, built out of pieces
// this codebase already has and already tests (dsp/nco.hpp, dsp/fir.hpp):
//
//   1. an fs/4 rotation and a half-band decimate-by-two, which turns 64 MS/s
//      of real samples into 32 MS/s of complex ones covering the whole band,
//      with DC of the output sitting at 16 MHz of the input. This is the one
//      stage that runs at the full ADC rate, so it is written out by hand
//      below with the half-band's zero taps and the rotation's +-1/+-j folded
//      in, rather than as a general FIR over a rotated copy;
//   2. an NCO that moves the wanted centre to DC, exactly, with no bin
//      quantisation and no second mixer to correct it;
//   3. a cascade of half-band decimators, one per factor of two, down to the
//      requested output rate;
//   4. a conjugation for VHF, where the R828D's IF is inverted.
//
// The property that matters is measurable either way, and it is measured:
// tests/test_rx888_source.cpp puts a synthesised real tone in and pins the
// output frequency, the amplitude and the image rejection, and checks stage 1
// against a brute-force rotate-filter-decimate written a second time.
//
// AMPLITUDE. A full-scale real sine (+-32767 counts) comes out with magnitude
// 1.0. Two conventions are folded in to get there: 16-bit counts are scaled
// by 1/32768 like every other sample in this application, and the analytic
// signal is doubled, because discarding the negative-frequency half of a real
// spectrum halves what is left. A cosine of amplitude A in, a complex
// exponential of magnitude A out.

// The half-band prototype. An odd length with the cutoff exactly at a quarter
// of the sample rate makes every even-OFFSET tap (offset from the centre)
// fall on a zero of the sinc, which is the half-band's defining property and
// what stage 1's structure below relies on.
//
// Those taps are ZEROED AND THE REST RENORMALISED rather than left as the
// designer computed them. sin(pi * j) in floating point is not zero, it is
// about 1e-16, so a windowed sinc at cutoff 0.25 comes out with even-offset
// taps of order 1e-17 instead of nothing at all. They are far too small to
// change a sample - but leaving them in would mean the fast structure below
// is not EXACTLY the general filter, and the test that proves one against the
// other would have to carry a tolerance that hides a real error as easily as
// it hides this one.
//
// numTaps must be odd. A kernel handed to QuarterRotateHalfBand must also be
// congruent to 1 modulo 4, so that its centre index is EVEN: that is what
// puts the sparse half of the taps on the odd indices and leaves the even
// side with the centre tap alone, which is the structure that class is
// written around. The later stages go through the general
// cascade::dsp::FirDecimator and have no such constraint.
inline std::vector<float> halfBandTaps(std::size_t numTaps) {
    std::vector<float> h =
        cascade::dsp::windowedSincLowpass(numTaps, 0.25, cascade::dsp::WindowType::BlackmanHarris);
    const std::size_t centre = (numTaps - 1) / 2;
    double sum = 0.0;
    for (std::size_t i = 0; i < numTaps; ++i) {
        const std::size_t offset = (i > centre) ? (i - centre) : (centre - i);
        if (offset != 0 && (offset % 2) == 0) { h[i] = 0.0f; }
        sum += h[i];
    }
    if (sum != 0.0) {
        for (float& t : h) { t = static_cast<float>(static_cast<double>(t) / sum); }
    }
    return h;
}

// Stage 1 on its own, so it can be tested on its own. `n` real samples in,
// n/2 complex samples out (the stream's phase carries across calls, so any
// split of the input gives the same output as one big call).
//
// THE STRUCTURE, derived once here so the loop below can be read. With the
// rotation e^{-j*pi*n/2} folded in and the half-band's zeros taken out, the
// decimated output at input index n = 2m is
//
//   z[m] = (-1)^m * ( h[M] * x[n-M]  +  j * sum over odd k of s(k)*h[k]*x[n-k] )
//
// where M is the (even) centre index and s(k) is +1 for k = 1 mod 4 and -1
// for k = 3 mod 4. One branch is a single delayed sample, the other is the
// (numTaps-1)/2 taps that are not zero, and the rotation has collapsed into
// one sign per output. That is the whole saving, and it matters because this
// is the only filter in the chain that sees 64 MS/s.
class QuarterRotateHalfBand {
public:
    explicit QuarterRotateHalfBand(std::size_t numTaps) { setTaps(halfBandTaps(numTaps)); }

    // The taps are settable so the test can drive the same structure with a
    // kernel it has designed itself, and so the analytic doubling can be
    // folded in by the caller rather than hidden here.
    void setTaps(std::vector<float> taps) {
        taps_ = std::move(taps);
        centre_ = (taps_.size() - 1) / 2;
        hist_.assign(taps_.empty() ? 0 : taps_.size() - 1, 0.0f);
        index_ = 0;
        odd_.clear();
        for (std::size_t k = 1; k < taps_.size(); k += 2) {
            const float sign = ((k % 4) == 1) ? 1.0f : -1.0f;
            odd_.push_back(OddTap{k, sign * taps_[k]});
        }
    }

    void reset() {
        std::fill(hist_.begin(), hist_.end(), 0.0f);
        index_ = 0;
    }

    // dst must have room for outputCapacity(n).
    std::size_t process(const float* real, std::size_t n, std::complex<float>* dst) {
        if (taps_.empty()) { return 0; }
        const std::size_t tail = taps_.size() - 1;
        work_.resize(tail + n);
        if (tail > 0) { std::copy(hist_.begin(), hist_.end(), work_.begin()); }
        if (n > 0) { std::copy(real, real + n, work_.begin() + static_cast<std::ptrdiff_t>(tail)); }

        // work_[j] is absolute input index a0 + j; the first output this call
        // can produce is at absolute index index_, because anything earlier
        // would need samples that have already left the history.
        const std::uint64_t a0 = index_ - static_cast<std::uint64_t>(tail);
        std::uint64_t pos = (index_ % 2 == 0) ? index_ : index_ + 1;
        const std::uint64_t end = index_ + n;  // exclusive
        std::size_t out = 0;
        for (; pos < end; pos += 2) {
            const std::size_t j = static_cast<std::size_t>(pos - a0);
            const float sign = ((pos % 4) == 0) ? 1.0f : -1.0f;
            const float re = taps_[centre_] * work_[j - centre_];
            float im = 0.0f;
            for (const OddTap& t : odd_) { im += t.weight * work_[j - t.k]; }
            dst[out++] = std::complex<float>(sign * re, sign * im);
        }

        if (tail > 0) {
            std::copy(work_.end() - static_cast<std::ptrdiff_t>(tail), work_.end(), hist_.begin());
        }
        index_ += n;
        return out;
    }

    std::size_t outputCapacity(std::size_t n) const { return n / 2 + 1; }

    const std::vector<float>& taps() const { return taps_; }

private:
    struct OddTap {
        std::size_t k;
        float weight;
    };

    std::vector<float> taps_;
    std::vector<OddTap> odd_;
    std::size_t centre_ = 0;
    // The last numTaps-1 input samples, oldest first, zero-filled at the
    // start of a stream so x[n < 0] = 0 falls out for free.
    std::vector<float> hist_;
    std::vector<float> work_;
    // The absolute index of the NEXT input sample, so the rotation's
    // four-phase pattern and the decimation grid both survive a block
    // boundary.
    std::uint64_t index_ = 0;
};

// The whole chain.
class RealToIq {
public:
    struct Config {
        std::uint32_t adcRateHz = kDefaultAdcRateHz;
        // Must be adcRateHz / 2^k for k >= 1; configure() coerces to the
        // nearest such rate and reports what it chose through rateHz().
        double outputRateHz = 8.0e6;
        // Where in the ADC's 0 .. adcRate/2 band the output is centred.
        double centerHz = 10.0e6;
        // VHF: the R828D's IF is inverted, so the output is conjugated
        // (RadioHandler.cpp:266-270).
        bool invertSpectrum = false;
        bool randomised = false;  // the ADC's output randomiser is on
    };

    RealToIq() { configure(Config{}); }

    // Rebuilds the filter cascade and clears every filter's history. Safe to
    // call between blocks, never during one.
    void configure(const Config& cfg) {
        cfg_ = cfg;
        const double stage1Rate = static_cast<double>(cfg_.adcRateHz) / 2.0;

        // How many further halvings get closest to what was asked for, in
        // log space so 6 MS/s lands on 8 rather than on 4 by an arithmetic
        // accident. Clamped to the five rates this radio offers.
        int stages = 0;
        if (cfg_.outputRateHz > 0.0 && cfg_.outputRateHz < stage1Rate) {
            const double ratio = stage1Rate / cfg_.outputRateHz;
            stages = static_cast<int>(std::log2(ratio) + 0.5);
        }
        stages = std::min(std::max(stages, 0), kMaxExtraStages);
        extraStages_ = stages;
        rateHz_ = stage1Rate / static_cast<double>(1u << stages);

        // The analytic doubling lives in stage 1's taps: throwing away the
        // negative half of a real spectrum halves what is left, and this is
        // the stage that throws it away.
        std::vector<float> s1 = halfBandTaps(kStage1Taps);
        for (float& t : s1) { t *= 2.0f; }
        stage1_.setTaps(std::move(s1));

        stages_.clear();
        for (int i = 0; i < stages; ++i) {
            stages_.emplace_back(halfBandTaps(kStageNTaps), 2);
        }

        setCenterHz(cfg_.centerHz);
        nco_.reset();
        reserve(kAdcSamplesPerTransfer);
    }

    // Just the NCO, for a retune that does not change the rate: no filter is
    // rebuilt and no history is lost, so the stream is continuous across it.
    // The NCO's own phase is deliberately NOT reset - a phase jump at a
    // retune is a click in every demodulator downstream.
    void setCenterHz(double hz) {
        cfg_.centerHz = hz;
        const double stage1Rate = static_cast<double>(cfg_.adcRateHz) / 2.0;
        // Stage 1 puts adcRate/4 at DC, so what is left to move is the
        // distance from there, normalised to stage 1's own output rate.
        const double offset = hz - static_cast<double>(cfg_.adcRateHz) / 4.0;
        nco_.setFrequency(stage1Rate > 0.0 ? (-offset / stage1Rate) : 0.0);
    }

    // Just the de-randomiser. Separate from configure() because the ADC's
    // randomiser is a GPIO bit the user can flip while the stream runs, and
    // rebuilding the filter cascade for it would put a click in the audio
    // every time - the de-randomisation is a per-sample decision, not a
    // filter.
    void setRandomised(bool on) { cfg_.randomised = on; }

    const Config& config() const { return cfg_; }
    double rateHz() const { return rateHz_; }

    void reset() {
        stage1_.reset();
        for (cascade::dsp::FirDecimator& s : stages_) { s.reset(); }
        nco_.reset();
    }

    // `n` RAW ADC samples in (16-bit, little-endian in the transfer, already
    // unpacked to int16 by the caller), complex baseband out. Returns the
    // number written; dst must have room for outputCapacity(n).
    std::size_t process(const std::int16_t* adc, std::size_t n, std::complex<float>* dst) {
        if (n == 0 || dst == nullptr) { return 0; }
        reserve(n);

        constexpr float kAdcScale = 1.0f / 32768.0f;
        if (cfg_.randomised) {
            for (std::size_t i = 0; i < n; ++i) {
                scratchReal_[i] = static_cast<float>(derandomise(adc[i])) * kAdcScale;
            }
        } else {
            for (std::size_t i = 0; i < n; ++i) {
                scratchReal_[i] = static_cast<float>(adc[i]) * kAdcScale;
            }
        }

        std::size_t count = stage1_.process(scratchReal_.data(), n, bufA_.data());
        nco_.mix(bufA_.data(), bufA_.data(), count);

        std::complex<float>* src = bufA_.data();
        std::complex<float>* dstBuf = bufB_.data();
        for (cascade::dsp::FirDecimator& s : stages_) {
            count = s.process(src, count, dstBuf);
            std::swap(src, dstBuf);
        }

        if (cfg_.invertSpectrum) {
            for (std::size_t i = 0; i < count; ++i) { dst[i] = std::conj(src[i]); }
        } else {
            std::copy(src, src + count, dst);
        }
        return count;
    }

    std::size_t outputCapacity(std::size_t n) const {
        std::size_t c = stage1_.outputCapacity(n);
        for (int i = 0; i < extraStages_; ++i) { c = c / 2 + 1; }
        return c;
    }

    // The most halvings after stage 1: adcRate/2 down to adcRate/32, which is
    // 32 MS/s down to 2 MS/s at the default clock and the same five rates the
    // reference offers from it (RadioHandler.cpp:152-154).
    static constexpr int kMaxExtraStages = 4;

    // STAGE 1 IS THE EXPENSIVE ONE - it is the only filter that sees 64 MS/s -
    // so its length is the one number worth arguing about. A half-band's
    // transition band straddles the fold point, which for this stage is the
    // 16 MHz centre of the ADC's band mapped to the OUTPUT's band edges: 0 MHz
    // and 32 MHz. So its length decides how close to either end of the HF
    // band the conversion stays clean, and nothing else. 49 taps (which is 1
    // modulo 4, as QuarterRotateHalfBand requires) is what the test measures
    // the usable edge and the image rejection of; the numbers are pinned
    // there rather than asserted here.
    static constexpr std::size_t kStage1Taps = 49;
    // The later stages run at 32 MS/s and below, where taps are cheap, and
    // theirs is the transition that lands at the EDGES OF THE USER'S BAND -
    // which is the one somebody looking at a spectrum will notice.
    static constexpr std::size_t kStageNTaps = 95;

private:
    // Sized for the largest block that will be asked for, once, at configure
    // time - the reader thread must not allocate between transfers. A block
    // bigger than that (a test, or a future transfer size) still works; it
    // simply grows the buffers on that call.
    void reserve(std::size_t n) {
        if (scratchReal_.size() < n) { scratchReal_.resize(n); }
        const std::size_t c = stage1_.outputCapacity(n);
        if (bufA_.size() < c) { bufA_.resize(c); }
        if (bufB_.size() < c) { bufB_.resize(c); }
    }

    Config cfg_;
    double rateHz_ = 0.0;
    int extraStages_ = 0;  // half-band decimators after stage 1

    QuarterRotateHalfBand stage1_{kStage1Taps};
    cascade::dsp::Nco nco_;
    std::vector<cascade::dsp::FirDecimator> stages_;

    std::vector<float> scratchReal_;
    std::vector<std::complex<float>> bufA_;
    std::vector<std::complex<float>> bufB_;
};

// Which rates the chain can produce from a given ADC clock: adcRate/2 down to
// adcRate/32, ascending. The top one is the whole band the ADC digitises and
// the bottom is where a 64 MHz clock stops dividing by two sensibly; the
// reference offers the same five from a 64 MHz clock and a sixth from a clock
// above 80 MHz (RadioHandler.cpp:152-154, "5 IF bands" / "6 IF bands").
inline std::vector<double> supportedRatesHz(std::uint32_t adcRateHz) {
    std::vector<double> out;
    const double top = static_cast<double>(adcRateHz) / 2.0;
    for (int i = RealToIq::kMaxExtraStages; i >= 0; --i) {
        out.push_back(top / static_cast<double>(1u << i));
    }
    return out;
}

// The widest complex rate a VHF tune can use. The R828D lands its output at a
// 4.57 MHz IF inside the ADC's band (RX888R2Radio.cpp:114-126), so a complex
// band centred there is bounded by how far it can reach down before it runs
// into 0 Hz: 2 x 4.57 = 9.14 MHz, and the widest rate in the table below that
// is 8 MS/s. This is arithmetic from the reference's IF, not a number the
// reference states.
constexpr double kMaxVhfRateHz = 8.0e6;

}  // namespace cascade::source::rx888
