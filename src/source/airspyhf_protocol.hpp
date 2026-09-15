// airspyhf_protocol.hpp - the Airspy HF+ family's USB vendor protocol, as
// numbers and pure functions, with nothing in it that can touch a device.
//
// WHY IT IS A SEPARATE HEADER FROM THE DRIVER, and why it is so much bigger
// than the HackRF's. Everything here is arithmetic the firmware and
// libairspyhf agree on: which request number means "set the sample rate", how
// a requested frequency in Hz becomes the whole-kHz local oscillator the
// firmware programs plus the residual the HOST has to rotate out in software,
// how the filter gain the firmware reports scales the samples, and - the part
// with no counterpart in any other driver here - the adaptive IQ balancer the
// reference applies to every zero-IF block. None of that needs a radio to be
// checked, and THERE IS NO AIRSPY HF+ ON THE BENCH THIS WAS WRITTEN ON, so it
// is separated out and tested against the reference implementation's own
// bodies (tests/test_airspyhf_source.cpp names the libairspyhf function each
// expectation came from).
//
// PORTED FROM libairspyhf, WHICH IS BSD-3-Clause. airspyhf.c, airspyhf.h,
// airspyhf_commands.h and iqbalancer.c/.h each carry the three-clause notice
// in their file banner (Copyright (c) 2013-2024 Youssef Touil, Ian Gilmour,
// Benjamin Vernoux, Michael Ossmann, Jared Boone; the balancer adds Leif
// Asbrink). It is reproduced in installer/THIRD-PARTY-LICENSES.txt under
// COMPONENT: libairspyhf. Nothing from libairspyhf is linked or shipped: what
// is taken is the wire protocol and the host-side DSP the hardware depends
// on, which is the only way to talk to this radio at all.
//
// THE ONE THING THAT IS NOT OPTIONAL. An Airspy HF+ does NOT deliver a
// finished spectrum. Three corrections live on the host, and the reference
// applies all three inside its own sample callback:
//   1. the FILTER GAIN the firmware reports for the current rate, as a
//      multiplier on every sample;
//   2. the FINE TUNING rotation, because the device's LO lands on a whole
//      kHz and (on a zero-IF rate) is deliberately placed 5 kHz off the
//      requested centre - the residual, freq_shift, is rotated out here;
//   3. the DC offset and IQ IMBALANCE correction, because a zero-IF front
//      end has no other way to reject its image.
// Skip any of them and the radio is not broken, it is subtly wrong: five
// kilohertz off, or with a mirror of every signal folded across the centre.
// That is why the balancer is transcribed in full rather than summarised.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace cascade::source::airspyhf {

// --- identity -------------------------------------------------------------

// libairspyhf airspyhf.c:145-146 (airspyhf_usb_vid / airspyhf_usb_pid). One
// VID/PID for the whole family: an HF+ Dual, an HF+ Discovery and an HF+
// Ranger are the same two numbers on the bus.
constexpr std::uint16_t kUsbVid = 0x03EB;
constexpr std::uint16_t kUsbPid = 0x800C;

// airspyhf.c:73-75, str_prefix_serial_airspyhf. The USB serial STRING is
// "AIRSPYHF SN:" followed by sixteen hex digits, 28 characters in all, and
// airspyhf_list_devices matches on exactly that shape. Windows reports the
// whole string as the device's serial, so the driver strips this prefix
// before it shows one or takes one back (see normalisedSerial).
constexpr const char* kSerialPrefix = "AIRSPYHF SN:";
constexpr std::size_t kSerialHexDigits = 16;

// --- the vendor requests --------------------------------------------------

// airspyhf_commands.h:33-58, airspyhf_vendor_request. ALL of them are named,
// unlike the HackRF header's deliberate subset: there is no transmit path on
// this radio and no request here that can do harm by existing. The ones this
// driver never sends are marked, so the numbering can be read against the
// reference without a second file open.
enum class VendorRequest : std::uint8_t {
    Invalid = 0,
    ReceiverMode = 1,
    SetFreq = 2,
    GetSampleRates = 3,
    SetSampleRate = 4,
    ConfigRead = 5,
    ConfigWrite = 6,           // not sent: it writes the device's flash
    GetSerialNoBoardId = 7,
    SetUserOutput = 8,         // not sent: the four GPIO pins on the Dual
    GetVersionString = 9,
    SetAgc = 10,
    SetAgcThreshold = 11,
    SetAtt = 12,
    SetLna = 13,
    GetSampleRateArchitectures = 14,
    GetFilterGain = 15,
    GetFreqDelta = 16,
    SetVctcxoCalibration = 17,
    SetFrontendOptions = 18,
    GetAttSteps = 19,
    GetBiasTeeCount = 20,
    GetBiasTeeName = 21,       // not sent: the name is only for a menu
    SetBiasTee = 22,
};

constexpr std::uint8_t requestByte(VendorRequest r) { return static_cast<std::uint8_t>(r); }

// airspyhf_commands.h:29-33, receiver_mode_t.
enum class ReceiverMode : std::uint16_t {
    Off = 0,
    On = 1,
};

// airspyhf.h:81-88, enum airspyhf_board_id.
//
// READ THIS BEFORE USING IT TO NAME A MODEL. The reference DECLARES this enum
// and never reads it: nothing in airspyhf.c compares anything against these
// values, and GET_SERIALNO_BOARDID's answer struct
// (airspyhf_read_partid_serialno_t, airspyhf.h:76-79) calls its first word
// `part_id`, which airspyhf_info.c prints as "Part ID" beside the serial
// rather than as a board. So there is NO verified mapping from any byte this
// driver can read to "Discovery" or "Dual", and inventing one would put a
// wrong model name in every problem report. The driver therefore takes its
// model name from the bus-reported product string, which Windows already has
// without opening anything, and logs the part id verbatim so a report can be
// matched against a real device later. These names exist so the numbers are
// readable, not so they can be guessed with.
enum class BoardId : std::uint32_t {
    Unknown = 0,
    HfRevA = 1,           // the original dual-port Airspy HF+
    DiscoveryRevA = 2,
    RangerRevA = 3,
    Invalid = 0xFF,
};

// airspyhf.h:36 - AIRSPYHF_ENDPOINT_IN is 1, and every transfer is submitted
// as LIBUSB_ENDPOINT_IN | that.
constexpr std::uint8_t kRxEndpoint = 0x81;

// airspyhf.c:55 and :939-990 - SAMPLES_TO_TRANSFER and transfer_count. 4096
// complex samples of four bytes each per transfer, sixteen transfers in
// flight. At the Discovery's top rate of 912 kS/s one buffer is 4.5 ms of
// signal, so the ring holds about 72 ms - this radio is slow enough that the
// geometry is generous rather than tight.
constexpr std::size_t kSamplesPerTransfer = 4096;
constexpr std::size_t kBytesPerSample = 4;
constexpr std::size_t kTransferBufferBytes = kSamplesPerTransfer * kBytesPerSample;
constexpr std::size_t kTransferCount = 16;

// airspyhf.c:80 - LIBUSB_CTRL_TIMEOUT_MS. Five times the HackRF's, and it is
// the reference's own number: this firmware answers GET_SAMPLERATES and the
// config read out of flash, which are not instant.
//
// A std::chrono duration rather than a bare unsigned so that
// tests/test_shutdown_budget.cpp's scan can DISCOVER it: it is a bounded wait
// a thread of ours can be inside, and that file's whole point is that a wait
// it has not been introduced to is a wait nothing holds the shutdown budget
// to. The transport takes milliseconds as an unsigned, so the call sites
// convert; the constant stays the discoverable thing.
constexpr std::chrono::milliseconds kControlTimeout{500};
constexpr unsigned kControlTimeoutMs = static_cast<unsigned>(kControlTimeout.count());

// GET_SERIALNO_BOARDID answers airspyhf_read_partid_serialno_t: one part-id
// word then four serial words (airspyhf.h:76-79).
constexpr std::size_t kSerialNoBoardIdBytes = 20;

// GET_VERSION_STRING is asked for MAX_VERSION_STRING_SIZE - 1 bytes
// (airspyhf.c:1622-1640, airspyhf.h:106-107).
constexpr std::size_t kVersionStringBytes = 63;

// CONFIG_READ / CONFIG_WRITE move a fixed 256-byte page (airspyhf.c:1456-1506)
// of which the first sixteen are flash_config_t.
constexpr std::size_t kConfigBytes = 256;

// airspyhf.c:67 - CALIBRATION_MAGIC. The word that says the flash page holds
// a real calibration rather than whatever was in the chip when it shipped.
constexpr std::uint32_t kCalibrationMagic = 0xA5CA71B0u;

// airspyhf.c:62 - DEFAULT_SAMPLERATE, the rate assumed when the firmware
// cannot be asked for its list.
constexpr std::uint32_t kDefaultSampleRateHz = 768000;

// --- what the hardware covers ---------------------------------------------
//
// NOT FROM THE REFERENCE, WHICH DOES NOT STATE A TUNING RANGE ANYWHERE. These
// are the manufacturer's published figures, read off airspy.com's own
// specification pages on 2026-09-14: the Airspy HF+ (dual port) covers
// "HF coverage between 9 kHz .. 31 MHz" and "VHF coverage between 64 .. 260
// MHz"; the HF+ Discovery reaches lower on HF ("0.5 kHz .. 31 MHz") and
// carries the same VHF band. The lower of the two HF floors is NOT used here
// because this driver cannot tell the two models apart (see BoardId): 9 kHz
// is true of both, and refusing a Discovery owner the last 8.5 kHz costs
// nothing that can be measured on an antenna anybody owns.
//
// The gap between them is real and is refused rather than clamped: there is
// no front end at 45 MHz, and a tune that silently lands somewhere else is
// worse than one that does not happen.
constexpr double kHfLowHz = 9.0e3;
constexpr double kHfHighHz = 31.0e6;
constexpr double kVhfLowHz = 64.0e6;
constexpr double kVhfHighHz = 260.0e6;

inline bool frequencyCovered(double hz) {
    return (hz >= kHfLowHz && hz <= kHfHighHz) || (hz >= kVhfLowHz && hz <= kVhfHighHz);
}

// --- the gains ------------------------------------------------------------

// airspyhf.c:64-65 - DEFAULT_ATT_STEP_COUNT and DEFAULT_ATT_STEP_INCREMENT,
// the steps assumed when the firmware is too old to list its own: nine of
// them, 0 to 48 dB in sixes. airspyhf.h:169 says the same in prose.
constexpr std::uint32_t kDefaultAttStepCount = 9;
constexpr float kDefaultAttStepDb = 6.0f;

// airspyhf.h:170 - the LNA (the front panel calls it a preamp) is "1 = +6 dB
// gain - compensated in digital", i.e. a switch, not a control.
constexpr double kLnaGainDb = 6.0;

// A ceiling on what the firmware may claim, so a device answering nonsense
// cannot make this driver allocate or loop on it. The reference has
// MAX_SAMPLERATE_INDEX (airspyhf.c:61) for the same class of protection on
// the rate side.
constexpr std::uint32_t kMaxSampleRateCount = 100;
constexpr std::uint32_t kMaxAttStepCount = 64;

// --- the tuning arithmetic ------------------------------------------------

// airspyhf.c:69-71. DEFAULT_IF_SHIFT is the offset a ZERO-IF rate is
// deliberately tuned away by, so the wanted signal never sits on the DC spike
// the front end cannot remove; the host rotates it back. MIN_ZERO_IF_LO and
// MIN_LOW_IF_LO are the lowest local oscillator each architecture will accept
// in kHz - a request below them is NOT refused, it is tuned at the floor and
// the whole difference goes into the software rotation, which is how this
// radio reaches 9 kHz at all.
constexpr double kDefaultIfShiftHz = 5000.0;
constexpr std::uint32_t kMinZeroIfLoKhz = 180;
constexpr std::uint32_t kMinLowIfLoKhz = 84;

// What one tune becomes: a whole-kHz LO for the device, and the residual the
// reader thread rotates out of every sample.
struct Tuning {
    std::uint32_t loKhz = 0;
    double shiftHz = 0.0;
};

// airspyhf_set_freq_double (airspyhf.c:1365-1420), transcribed.
//
// `calibrationPpb` is the crystal correction read out of the device's own
// flash at open; `freqDeltaHz` is what the LAST SET_FREQ's GET_FREQ_DELTA
// answered, which is the firmware telling the host how far its synthesiser
// actually landed from the kHz it was asked for. Both are part of the answer,
// which is why this takes them rather than reading device state.
//
// Two mechanical differences from the reference, neither of which changes an
// answer: the MAX() is written as a clamp on a value that has already been
// floored at zero (a negative double cast to uint32_t is undefined in C++,
// and a caller can reach this with one), and the reference's `(uint32_t)round`
// is spelled with std::llround so the intermediate is not a double sitting on
// a cast boundary.
inline Tuning computeTuning(double freqHz, std::int32_t calibrationPpb, bool lowIf,
                            bool dspEnabled, double freqDeltaHz) {
    const double ifShift = (dspEnabled && !lowIf) ? kDefaultIfShiftHz : 0.0;
    const double adjustedHz = freqHz * (1.0e9 + static_cast<double>(calibrationPpb)) * 1.0e-9;
    const std::uint32_t loLowKhz = lowIf ? kMinLowIfLoKhz : kMinZeroIfLoKhz;

    double wantKhz = (adjustedHz + ifShift) * 1e-3;
    if (!(wantKhz > 0.0)) { wantKhz = 0.0; }  // negated compare so a NaN lands here
    const long long rounded = std::llround(wantKhz);
    const std::uint32_t asked =
        rounded < 0 ? 0u
                    : static_cast<std::uint32_t>(std::min<long long>(rounded, 0xFFFFFFFFLL));

    Tuning t;
    t.loKhz = std::max(loLowKhz, asked);
    t.shiftHz = adjustedHz - static_cast<double>(t.loKhz) * 1e3 + freqDeltaHz;
    return t;
}

// SET_FREQ's payload is four bytes BIG-endian - the one place this protocol
// disagrees with every other radio in this tree, which is exactly the kind of
// detail a driver gets wrong once and then cannot find. airspyhf.c:1373-1376
// writes buf[0] = freq_khz >> 24 first.
constexpr std::size_t kFreqPayloadBytes = 4;

inline void encodeFreqKhz(std::uint32_t khz, std::uint8_t out[kFreqPayloadBytes]) {
    out[0] = static_cast<std::uint8_t>((khz >> 24) & 0xFF);
    out[1] = static_cast<std::uint8_t>((khz >> 16) & 0xFF);
    out[2] = static_cast<std::uint8_t>((khz >> 8) & 0xFF);
    out[3] = static_cast<std::uint8_t>(khz & 0xFF);
}

// airspyhf.c:1404 - the four bytes GET_FREQ_DELTA answers are NOT a number in
// any ordinary layout: buf[0] is a binary exponent, buf[1..3] are a signed
// 24-bit mantissa in millihertz-of-kilohertz, low byte FIRST, and the value
// is mantissa * 1e3 / 2^exponent.
//
// The one guard the reference does not have: a shift of 32 or more is
// undefined behaviour in C++, and this is a number the DEVICE chooses. A
// firmware answering nonsense must not be able to make this driver undefined,
// so the exponent is clamped - at every sane value the answer is unchanged.
inline double decodeFreqDelta(const std::uint8_t b[4]) {
    const std::int32_t mantissa = (static_cast<std::int32_t>(static_cast<std::int8_t>(b[3])) << 16) |
                                  (static_cast<std::int32_t>(b[2]) << 8) |
                                  static_cast<std::int32_t>(b[1]);
    const unsigned shift = b[0] > 30 ? 30u : static_cast<unsigned>(b[0]);
    return static_cast<double>(mantissa) * 1e3 / static_cast<double>(1u << shift);
}

// --- the sample rate ------------------------------------------------------

// The DeviceSource contract is that setSampleRateHz coerces to the nearest
// SUPPORTED rate, where libairspyhf's airspyhf_set_samplerate
// (airspyhf.c:1194-1226) takes either an exact rate or an index and REFUSES
// anything else. Nearest-wins is this driver's rule, not the reference's, and
// it is the one the Source panel needs: the rates come off the device, so a
// panel cannot offer a value that is not in the list, but a config saved
// against another radio can carry one.
//
// Ties go to the LOWER index, which on this firmware's descending list is the
// faster rate - deliberately, because the panel's own menu is that list and a
// tie can only happen for a value exactly between two entries.
inline std::size_t nearestRateIndex(const std::vector<std::uint32_t>& rates, double hz) {
    std::size_t best = 0;
    double bestDelta = -1.0;
    for (std::size_t i = 0; i < rates.size(); ++i) {
        const double delta = std::fabs(static_cast<double>(rates[i]) - hz);
        if (bestDelta < 0.0 || delta < bestDelta) {
            bestDelta = delta;
            best = i;
        }
    }
    return best;
}

// airspyhf.c:1273-1279 - GET_FILTER_GAIN answers one byte of decibels, and
// every sample is scaled by 10^(-dB/20) to undo it. A firmware that does not
// answer leaves the gain at 1.0 (airspyhf.c:1277).
inline float filterGainFromDb(std::uint8_t db) {
    return std::pow(10.0f, static_cast<float>(db) * -0.05f);
}

// --- the sample format ----------------------------------------------------

// airspyhf.c:84-87, airspyhf_complex_int16_t, inside a #pragma pack(1):
//
//     typedef struct { int16_t im; int16_t re; } airspyhf_complex_int16_t;
//
// THE IMAGINARY PART COMES FIRST. That is not a typo in the reference and it
// is the single easiest thing to get wrong here: swapping them mirrors the
// spectrum about the centre, which looks like a working receiver until you
// try to decode anything on one side of it.
constexpr float kSampleScale = 1.0f / 32768.0f;

inline std::int16_t readInt16Le(const std::uint8_t* p) {
    return static_cast<std::int16_t>(static_cast<std::uint16_t>(p[0]) |
                                     (static_cast<std::uint16_t>(p[1]) << 8));
}

// airspyhf.c:316-332, convert_samples' first loop: scale = 1/32768 times the
// filter gain, applied to both parts. -32768 maps to -1.0 and +32767 to just
// inside +1.0, i.e. into [-1, 1) - the asymmetry is two's complement's, not a
// choice.
inline void decodeSamples(const std::uint8_t* raw, std::size_t sampleCount, float filterGain,
                          std::complex<float>* out) {
    const float gain = kSampleScale * filterGain;
    for (std::size_t i = 0; i < sampleCount; ++i) {
        const std::int16_t im = readInt16Le(raw + i * kBytesPerSample);
        const std::int16_t re = readInt16Le(raw + i * kBytesPerSample + 2);
        out[i] = std::complex<float>(static_cast<float>(re) * gain,
                                     static_cast<float>(im) * gain);
    }
}

// --- the fine-tuning rotation ---------------------------------------------

// airspyhf.c:294-314 and :341-360, the second half of convert_samples. The
// residual from computeTuning is rotated out one sample at a time by
// repeatedly multiplying a unit vector by a fixed rotation - and the vector is
// re-normalised by the reference's own first-order trick (norm = 1.99 - |v|^2)
// rather than by a square root, because this runs per sample.
//
// `vec` is carried ACROSS blocks by the caller: restarting it at 1+0i every
// buffer would put a phase step at every transfer boundary, which is a comb of
// spurs at the buffer rate and nothing on screen to explain them.
struct Rotator {
    float re = 1.0f;
    float im = 0.0f;
};

inline void rotateBlock(std::complex<float>* iq, std::size_t count, double shiftHz,
                        double sampleRateHz, Rotator& vec) {
    if (count == 0 || shiftHz == 0.0 || !(sampleRateHz > 0.0)) { return; }
    const double angle = 2.0 * 3.14159265359 * shiftHz / sampleRateHz;
    const float rotRe = static_cast<float>(std::cos(angle));
    const float rotIm = static_cast<float>(-std::sin(angle));
    float vr = vec.re;
    float vi = vec.im;
    for (std::size_t i = 0; i < count; ++i) {
        // rotate_complex: multiply by the step, then the cheap re-normalise.
        const float nr = vr * rotRe - vi * rotIm;
        vi = vi * rotRe + vr * rotIm;
        vr = nr;
        const float norm = 1.99f - (vr * vr + vi * vi);
        vr *= norm;
        vi *= norm;
        // multiply_complex_complex(&dest[i], &vec)
        const float sr = iq[i].real();
        const float si = iq[i].imag();
        iq[i] = std::complex<float>(sr * vr - si * vi, si * vr + sr * vi);
    }
    vec.re = vr;
    vec.im = vi;
}

// --- the attenuator -------------------------------------------------------

// airspyhf_set_att (airspyhf.c:1674-1704): the FIRST step at or above the
// requested attenuation wins. The reference leaves the index at 0 when the
// request is above every step, which silently turns "as much attenuation as
// you have" into none; the DeviceSource contract (device_source.hpp) is that
// an out-of-range value is CLAMPED, so this clamps to the last step instead
// and says so. Everything inside the range is the reference's answer.
inline std::uint16_t attIndexFor(const std::vector<float>& steps, double attenuationDb) {
    if (steps.empty()) { return 0; }
    for (std::size_t i = 0; i < steps.size(); ++i) {
        if (static_cast<double>(steps[i]) >= attenuationDb) { return static_cast<std::uint16_t>(i); }
    }
    return static_cast<std::uint16_t>(steps.size() - 1);
}

// The steps assumed when the firmware cannot list its own (airspyhf.c:1049-1058).
inline std::vector<float> defaultAttSteps() {
    std::vector<float> steps(kDefaultAttStepCount);
    for (std::uint32_t i = 0; i < kDefaultAttStepCount; ++i) {
        steps[i] = static_cast<float>(i) * kDefaultAttStepDb;
    }
    return steps;
}

// ===========================================================================
// THE IQ BALANCER, ported from libairspyhf's iqbalancer.c.
// ===========================================================================
//
// WHY IT IS HERE AT ALL, when no other driver in this tree carries anything
// like it. The HF+ front end is zero-IF on most of its rates, which means
// every signal has an IMAGE folded across the centre of the span, at whatever
// rejection the analogue path happens to give - typically poor, and it drifts
// with temperature and with the tuned frequency. The reference corrects it in
// SOFTWARE, adaptively, on the host: it estimates the phase and amplitude
// error from the correlation between a block's spectrum and the mirror of
// itself, and applies the correction to every sample. A driver that streams
// this radio without it delivers a spectrum full of mirror images that the
// manufacturer's own software does not show, and the user has no way to know
// which signals are real.
//
// So it is transcribed - the window, the radix-2 FFT with its fftshift, the
// DC canceller, the correlation, the utility weighting and the lookback
// average - rather than replaced with anything of ours. The parameters are
// the reference's #defines (iqbalancer.h:28-56) and the initial phase and
// amplitude are airspyhf.c:77-78.
//
// FOUR MECHANICAL DIFFERENCES, none of which changes an answer:
//   - the file-static window tables are function-local statics built once,
//     because a mutable namespace-scope array initialised by a first caller
//     is a data race waiting for a second device;
//   - compute_corr's 32 KB stack array is a member buffer;
//   - malloc/free become std::vector, so a balancer cannot leak;
//   - `abs()` on the bin distances is std::abs on an int, which is the same
//     function the C file gets from stdlib.h.
constexpr int kFftBins = 4096;
constexpr double kBoostFactor = 100000.0;
constexpr int kBinsToOptimize = kFftBins / 25;
constexpr int kEdgeBinsToSkip = kFftBins / 22;
constexpr int kCenterBinsToSkip = 2;
constexpr int kMaxLookback = 4;
constexpr float kPhaseStep = 1e-2f;
constexpr float kAmplitudeStep = 1e-2f;
constexpr float kMaxMu = 50.0f;
constexpr float kMinDeltaMu = 0.1f;
constexpr float kDcTimeConst = 1e-4f;
constexpr float kMinimumPower = 0.01f;
constexpr float kPowerThreshold = 0.5f;
constexpr int kBuffersToSkipOnReset = 2;
constexpr float kMaxPowerDecay = 0.98f;
constexpr float kMaxPowerRatio = 0.8f;
constexpr float kBoostWindowNorm = kMaxPowerRatio / 95.0f;
constexpr float kBalancerEpsilon = 0.01f;

// iqbalancer.h:51-56, the non-ARM branch.
constexpr int kBuffersToSkip = 2;
constexpr int kFftIntegration = 4;
constexpr int kFftOverlap = 2;
constexpr int kCorrelationIntegration = 16;

// airspyhf.c:77-78 - INITIAL_PHASE and INITIAL_AMPLITUDE, the imbalance a
// cold HF+ starts at before the estimator has seen anything.
constexpr float kInitialPhase = 0.00006f;
constexpr float kInitialAmplitude = -0.0045f;

// iqbalancer.c:35 - WorkingBufferLength.
constexpr int kWorkingBufferLength = kFftBins * (1 + kFftIntegration / kFftOverlap);

// The two windows __init_library builds (iqbalancer.c:76-99): a Blackman-Harris
// analysis window, and the boost window that weights bins by their distance
// from the bin being optimised.
struct BalancerWindows {
    float fft[kFftBins];
    float boost[kFftBins];
};

inline const BalancerWindows& balancerWindows() {
    static const BalancerWindows w = [] {
        BalancerWindows t{};
        const int length = kFftBins - 1;
        for (int i = 0; i <= length; ++i) {
            const double x = static_cast<double>(i) / static_cast<double>(length);
            t.fft[i] = static_cast<float>(+0.35875 - 0.48829 * std::cos(2.0 * 3.14159265359 * x) +
                                          0.14128 * std::cos(4.0 * 3.14159265359 * x) -
                                          0.01168 * std::cos(6.0 * 3.14159265359 * x));
            t.boost[i] = static_cast<float>(
                1.0 / kBoostFactor +
                1.0 / std::exp(std::pow(i * 2.0 / static_cast<double>(kBinsToOptimize), 2.0)));
        }
        return t;
    }();
    return w;
}

class IqBalancer {
public:
    explicit IqBalancer(float initialPhase = kInitialPhase,
                        float initialAmplitude = kInitialAmplitude)
        : phase_(initialPhase), amplitude_(initialAmplitude) {
        corr_.resize(kFftBins);
        corrPlus_.resize(kFftBins);
        working_.resize(kWorkingBufferLength);
        boost_.resize(kFftBins);
        fftScratch_.resize(kFftBins);
        powerFlag_.assign(static_cast<std::size_t>(fftIntegration_), 0);
        balancerWindows();
    }

    // iq_balancer_configure (iqbalancer.c:529-542).
    void configure(int buffersToSkip, int fftIntegration, int fftOverlap,
                   int correlationIntegration) {
        buffersToSkip_ = buffersToSkip;
        fftIntegration_ = fftIntegration < 1 ? 1 : fftIntegration;
        fftOverlap_ = fftOverlap < 1 ? 1 : fftOverlap;
        correlationIntegration_ = correlationIntegration;
        powerFlag_.assign(static_cast<std::size_t>(fftIntegration_), 0);
        resetFlag_ = true;
    }

    // iq_balancer_set_optimal_point (iqbalancer.c:515-527). The driver calls
    // this after every retune, which is what the `reset_flag` it raises is
    // actually for: a new frequency is a new imbalance, and carrying the old
    // estimate into it converges more slowly than starting again.
    void setOptimalPoint(float w) {
        if (w < -0.5f) {
            w = -0.5f;
        } else if (w > 0.5f) {
            w = 0.5f;
        }
        optimalBin_ = static_cast<int>(std::floor(kFftBins * (0.5 + static_cast<double>(w))));
        resetFlag_ = true;
    }

    // iq_balancer_process (iqbalancer.c:487-513).
    void process(std::complex<float>* iq, int length, bool skipEval) {
        if (iq == nullptr || length <= 0) { return; }
        cancelDc(iq, length, skipEval);

        if (!skipEval) {
            int count = kWorkingBufferLength - workingPos_;
            if (count >= length) { count = length; }
            for (int i = 0; i < count; ++i) { working_[workingPos_ + i] = iq[i]; }
            workingPos_ += count;
            if (workingPos_ >= kWorkingBufferLength) {
                workingPos_ = 0;
                if (++skippedBuffers_ > buffersToSkip_) {
                    skippedBuffers_ = 0;
                    estimateImbalance(working_.data(), kWorkingBufferLength);
                }
            }
        }

        adjustPhaseAmplitude(iq, length);
    }

    float phase() const { return phase_; }
    float amplitude() const { return amplitude_; }

private:
    // iqbalancer.c:194-221, cancel_dc.
    void cancelDc(std::complex<float>* iq, int length, bool skipEval) {
        float iavg = iavg_;
        float qavg = qavg_;
        if (skipEval) {
            for (int i = 0; i < length; ++i) {
                iq[i] = std::complex<float>(iq[i].real() - iavg, iq[i].imag() - qavg);
            }
        } else {
            for (int i = 0; i < length; ++i) {
                iavg += kDcTimeConst * (iq[i].real() - iavg);
                qavg += kDcTimeConst * (iq[i].imag() - qavg);
                iq[i] = std::complex<float>(iq[i].real() - iavg, iq[i].imag() - qavg);
            }
            iavg_ = iavg;
            qavg_ = qavg;
        }
    }

    // iqbalancer.c:101-111, window().
    static void applyWindow(std::complex<float>* buffer, int length) {
        const BalancerWindows& w = balancerWindows();
        for (int i = 0; i < length; ++i) {
            buffer[i] = std::complex<float>(buffer[i].real() * w.fft[i],
                                            buffer[i].imag() * w.fft[i]);
        }
    }

    // iqbalancer.c:113-192, fft(): a decimation-in-time radix-2 with the
    // half-swap at the end that leaves DC in the middle. Transcribed loop for
    // loop, indices included.
    static void fft(std::complex<float>* buffer, int length) {
        const int nm1 = length - 1;
        const int nd2 = length / 2;
        int i, j, k, l, m, le, le2, ip;
        std::complex<float> u, t;

        m = 0;
        i = length;
        while (i > 1) {
            ++m;
            i = (i >> 1);
        }

        j = nd2;
        for (i = 1; i < nm1; ++i) {
            if (i < j) {
                t = buffer[j];
                buffer[j] = buffer[i];
                buffer[i] = t;
            }
            k = nd2;
            while (k <= j) {
                j = j - k;
                k = k / 2;
            }
            j += k;
        }

        for (l = 1; l <= m; ++l) {
            le = 1 << l;
            le2 = le / 2;
            float ure = 1.0f;
            float uim = 0.0f;
            const float rre = static_cast<float>(std::cos(3.14159265359 / le2));
            const float rim = static_cast<float>(-std::sin(3.14159265359 / le2));
            for (j = 1; j <= le2; ++j) {
                const int jm1 = j - 1;
                for (i = jm1; i <= nm1; i += le) {
                    ip = i + le2;
                    const float tre = ure * buffer[ip].real() - uim * buffer[ip].imag();
                    const float tim = uim * buffer[ip].real() + ure * buffer[ip].imag();
                    buffer[ip] = std::complex<float>(buffer[i].real() - tre,
                                                     buffer[i].imag() - tim);
                    buffer[i] = std::complex<float>(buffer[i].real() + tre,
                                                    buffer[i].imag() + tim);
                }
                const float nre = ure * rre - uim * rim;
                uim = uim * rre + ure * rim;
                ure = nre;
            }
        }

        for (i = 0; i < nd2; ++i) {
            j = nd2 + i;
            t = buffer[i];
            buffer[i] = buffer[j];
            buffer[j] = t;
        }
    }

    // iqbalancer.c:223-243, adjust_benchmark. Both corrections read the
    // ORIGINAL re and im - that is not an oversight in the reference, it is
    // the first-order correction it means to apply.
    static float adjustBenchmark(std::complex<float>* iq, float phase, float amplitude,
                                 bool skipPower) {
        float sum = 0.0f;
        for (int i = 0; i < kFftBins; ++i) {
            const float re = iq[i].real();
            const float im = iq[i].imag();
            float nre = re + phase * im;
            float nim = im + phase * re;
            nre *= 1.0f + amplitude;
            nim *= 1.0f - amplitude;
            iq[i] = std::complex<float>(nre, nim);
            if (!skipPower) { sum += re * re + im * im; }
        }
        return sum;
    }

    // iqbalancer.c:253-321, compute_corr.
    int computeCorr(const std::complex<float>* iq, std::complex<float>* ccorr, int length,
                    int step) {
        const BalancerWindows& w = balancerWindows();
        int count = 0;
        const float phase = phase_ + static_cast<float>(step) * kPhaseStep;
        const float amplitude = amplitude_ + static_cast<float>(step) * kAmplitudeStep;

        int n = 0;
        int m = 0;
        for (; n <= length - kFftBins && m < fftIntegration_;
             n += kFftBins / fftOverlap_, ++m) {
            std::memcpy(fftScratch_.data(), iq + n,
                        static_cast<std::size_t>(kFftBins) * sizeof(std::complex<float>));
            const float power =
                adjustBenchmark(fftScratch_.data(), phase, amplitude, step != 0);
            if (step == 0) {
                if (power > kMinimumPower) {
                    powerFlag_[static_cast<std::size_t>(m)] = 1;
                    integratedTotalPower_ += power;
                } else {
                    powerFlag_[static_cast<std::size_t>(m)] = 0;
                }
            }
            if (powerFlag_[static_cast<std::size_t>(m)] == 1) {
                ++count;
                applyWindow(fftScratch_.data(), kFftBins);
                fft(fftScratch_.data(), kFftBins);
                for (int i = kEdgeBinsToSkip, j = kFftBins - kEdgeBinsToSkip;
                     i <= kFftBins - kEdgeBinsToSkip; ++i, --j) {
                    // multiply_complex_complex(fftPtr + i, fftPtr + j) - NOT a
                    // conjugate product: the reference correlates a bin with
                    // its mirror directly.
                    const float are = fftScratch_[i].real();
                    const float aim = fftScratch_[i].imag();
                    const float bre = fftScratch_[j].real();
                    const float bim = fftScratch_[j].imag();
                    const float ccre = are * bre - aim * bim;
                    const float ccim = aim * bre + are * bim;
                    ccorr[i] = std::complex<float>(ccorr[i].real() + ccre,
                                                   ccorr[i].imag() + ccim);
                    ccorr[j] = ccorr[i];
                }
                if (step == 0) {
                    for (int i = kEdgeBinsToSkip; i <= kFftBins - kEdgeBinsToSkip; ++i) {
                        const float p = fftScratch_[i].real() * fftScratch_[i].real() +
                                        fftScratch_[i].imag() * fftScratch_[i].imag();
                        boost_[static_cast<std::size_t>(i)] += p;
                        if (optimalBin_ == kFftBins / 2) {
                            integratedImagePower_ += p;
                        } else {
                            integratedImagePower_ +=
                                p * w.boost[std::abs(kFftBins - i - optimalBin_)];
                        }
                    }
                }
            }
        }
        return count;
    }

    // iqbalancer.c:323-346, utility().
    std::complex<float> utility(const std::complex<float>* ccorr) const {
        const BalancerWindows& w = balancerWindows();
        const float invskip = 1.0f / static_cast<float>(kEdgeBinsToSkip);
        float accRe = 0.0f;
        float accIm = 0.0f;
        for (int i = kEdgeBinsToSkip, j = kFftBins - kEdgeBinsToSkip;
             i <= kFftBins - kEdgeBinsToSkip; ++i, --j) {
            const int distance = std::abs(i - kFftBins / 2);
            if (distance > kCenterBinsToSkip) {
                float weight = (distance > kEdgeBinsToSkip)
                                   ? 1.0f
                                   : (static_cast<float>(distance) * invskip);
                if (optimalBin_ != kFftBins / 2) {
                    weight *= w.boost[std::abs(optimalBin_ - i)];
                }
                weight *= boost_[static_cast<std::size_t>(j)] /
                          (boost_[static_cast<std::size_t>(i)] + kBalancerEpsilon);
                accRe += ccorr[i].real() * weight;
                accIm += ccorr[i].imag() * weight;
            }
        }
        return std::complex<float>(accRe, accIm);
    }

    // iqbalancer.c:348-459, estimate_imbalance.
    void estimateImbalance(std::complex<float>* iq, int length) {
        if (resetFlag_) {
            resetFlag_ = false;
            noOfAvg_ = -kBuffersToSkipOnReset;
            maximumImagePower_ = 0.0f;
        }

        if (noOfAvg_ < 0) {
            ++noOfAvg_;
            return;
        }
        if (noOfAvg_ == 0) {
            integratedImagePower_ = 0.0f;
            integratedTotalPower_ = 0.0f;
            std::fill(boost_.begin(), boost_.end(), 0.0f);
            std::fill(corr_.begin(), corr_.end(), std::complex<float>(0.0f, 0.0f));
            std::fill(corrPlus_.begin(), corrPlus_.end(), std::complex<float>(0.0f, 0.0f));
        }

        maximumImagePower_ *= kMaxPowerDecay;

        int i = computeCorr(iq, corr_.data(), length, 0);
        if (i == 0) { return; }

        noOfAvg_ += i;
        computeCorr(iq, corrPlus_.data(), length, 1);

        if (noOfAvg_ <= correlationIntegration_ * fftIntegration_) { return; }
        noOfAvg_ = 0;

        if (optimalBin_ == kFftBins / 2) {
            if (integratedTotalPower_ < maximumImagePower_) { return; }
            maximumImagePower_ = integratedTotalPower_;
        } else {
            if (integratedImagePower_ - integratedTotalPower_ * kBoostWindowNorm <
                maximumImagePower_ * kPowerThreshold) {
                return;
            }
            maximumImagePower_ = integratedImagePower_ - integratedTotalPower_ * kBoostWindowNorm;
        }

        const std::complex<float> a = utility(corr_.data());
        const std::complex<float> b = utility(corrPlus_.data());

        float mu = a.imag() - b.imag();
        if (std::fabs(mu) > kMinDeltaMu) {
            mu = a.imag() / mu;
            if (mu < -kMaxMu) {
                mu = -kMaxMu;
            } else if (mu > kMaxMu) {
                mu = kMaxMu;
            }
        } else {
            mu = 0.0f;
        }
        float phase = phase_ + kPhaseStep * mu;

        mu = a.real() - b.real();
        if (std::fabs(mu) > kMinDeltaMu) {
            mu = a.real() / mu;
            if (mu < -kMaxMu) {
                mu = -kMaxMu;
            } else if (mu > kMaxMu) {
                mu = kMaxMu;
            }
        } else {
            mu = 0.0f;
        }
        float amplitude = amplitude_ + kAmplitudeStep * mu;

        if (noOfRaw_ < kMaxLookback) { ++noOfRaw_; }
        rawAmplitudes_[rawPtr_] = amplitude;
        rawPhases_[rawPtr_] = phase;
        int idx = rawPtr_;
        for (int j = 0; j < noOfRaw_ - 1; ++j) {
            idx = (idx + kMaxLookback - 1) & (kMaxLookback - 1);
            phase += rawPhases_[idx];
            amplitude += rawAmplitudes_[idx];
        }
        phase /= static_cast<float>(noOfRaw_);
        amplitude /= static_cast<float>(noOfRaw_);
        rawPtr_ = (rawPtr_ + 1) & (kMaxLookback - 1);

        phase_ = phase;
        amplitude_ = amplitude;
    }

    // iqbalancer.c:461-485, adjust_phase_amplitude: the correction is ramped
    // ACROSS the block from the previous estimate to the current one, so a
    // step in the estimate is not a step in the samples.
    void adjustPhaseAmplitude(std::complex<float>* iq, int length) {
        if (length < 2) { return; }
        const float scale = 1.0f / static_cast<float>(length - 1);
        for (int i = 0; i < length; ++i) {
            const float fi = static_cast<float>(i);
            const float fl = static_cast<float>(length - 1 - i);
            const float phase = (fi * lastPhase_ + fl * phase_) * scale;
            const float amplitude = (fi * lastAmplitude_ + fl * amplitude_) * scale;
            const float re = iq[i].real();
            const float im = iq[i].imag();
            float nre = re + phase * im;
            float nim = im + phase * re;
            nre *= 1.0f + amplitude;
            nim *= 1.0f - amplitude;
            iq[i] = std::complex<float>(nre, nim);
        }
        lastPhase_ = phase_;
        lastAmplitude_ = amplitude_;
    }

    float phase_ = 0.0f;
    float lastPhase_ = 0.0f;
    float amplitude_ = 0.0f;
    float lastAmplitude_ = 0.0f;

    float iavg_ = 0.0f;
    float qavg_ = 0.0f;
    float integratedTotalPower_ = 0.0f;
    float integratedImagePower_ = 0.0f;
    float maximumImagePower_ = 0.0f;

    float rawPhases_[kMaxLookback] = {0.0f, 0.0f, 0.0f, 0.0f};
    float rawAmplitudes_[kMaxLookback] = {0.0f, 0.0f, 0.0f, 0.0f};

    int skippedBuffers_ = 0;
    int buffersToSkip_ = kBuffersToSkip;
    int workingPos_ = 0;
    int fftIntegration_ = kFftIntegration;
    int fftOverlap_ = kFftOverlap;
    int correlationIntegration_ = kCorrelationIntegration;

    int noOfAvg_ = 0;
    int noOfRaw_ = 0;
    int rawPtr_ = 0;
    int optimalBin_ = kFftBins / 2;
    bool resetFlag_ = false;

    std::vector<int> powerFlag_;
    std::vector<std::complex<float>> corr_;
    std::vector<std::complex<float>> corrPlus_;
    std::vector<std::complex<float>> working_;
    std::vector<std::complex<float>> fftScratch_;
    std::vector<float> boost_;
};

}  // namespace cascade::source::airspyhf
