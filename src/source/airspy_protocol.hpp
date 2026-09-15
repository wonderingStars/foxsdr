// airspy_protocol.hpp - the Airspy R2 / Mini USB vendor protocol, and the
// arithmetic that turns what comes off the wire into complex samples. Numbers
// and pure functions, with nothing in it that can touch a device.
//
// WHY IT IS A SEPARATE HEADER FROM THE DRIVER, the same reason
// hackrf_protocol.hpp is: everything here can be checked without a radio, and
// there is no Airspy on the bench this was written on. The request numbers,
// the payload layouts, the 12-bit packing, the gain tables and the whole
// real-to-complex conversion are arithmetic the firmware and libairspy agree
// on; tests/test_airspy_source.cpp names the reference function each
// expectation came from, and the one thing that could not be read off by eye -
// the IQ conversion - is pinned against an expectation derived from signal
// theory rather than from a second copy of this code.
//
// PORTED FROM libairspy, WHICH IS BSD-3-Clause. airspy.c, airspy.h and
// airspy_commands.h all carry the three-clause notice in their file banners
// (Jared Boone, Michael Ossmann, Benjamin Vernoux, Youssef Touil, Ian
// Gilmour). The half-band kernel below is libairspy's filters.h, which carries
// an MIT notice (Youssef Touil, 2014). Both are reproduced in
// installer/THIRD-PARTY-LICENSES.txt. Nothing from libairspy is linked or
// shipped: what is taken is the wire protocol and the filter's coefficients,
// which are the only way to talk to the hardware and the only way to agree
// with every other Airspy application about what the samples mean.
//
// ONE FILE IS DELIBERATELY NOT PORTED. libairspy's iqconverter_float.c, which
// is where the reference performs the conversion, was relicensed in 2025 to
// permit use only "as part of the Airspy ecosystem". FoxSDR is not that, so
// its BODY is not transcribed here. What IS used is (a) the MIT-licensed
// coefficients from filters.h, which the 2025 change did not touch, and (b)
// the structure the reference's own airspy.c documents in the open - an fs/4
// translation, a half-band, a matched delay on the other branch and a
// decimation by two - written here from first principles. The v1.0.10 tag's
// iqconverter_float.c, which IS MIT and is contemporaneous with the airspy.c
// this was ported from, was read to confirm the structure; the arithmetic in
// this file was then derived and CHECKED against the closed-form answer
// (see IqConverter below), not copied.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>

namespace cascade::source::airspy {

// --- identity -------------------------------------------------------------

// libairspy airspy.c:112-113 (airspy_usb_vid / airspy_usb_pid). ONE product
// id for the whole family: an R2 and a Mini are 0x1D50:0x60A1 alike, and no
// NUMBER anywhere tells them apart - not the product id and not the board id
// either, which is 0 on both (airspy.h:78-82). What does is a string, and
// airspyModelFrom() in airspy_source.hpp is that test.
constexpr std::uint16_t kUsbVid = 0x1D50;
constexpr std::uint16_t kUsbPid = 0x60A1;

// --- the vendor requests --------------------------------------------------

// libairspy airspy_commands.h:55-85, airspy_vendor_request. Named in full
// because the enumeration is the firmware's and a gap in it would be a
// mis-numbering waiting to happen; the ones this driver never sends are
// marked, and a request it cannot name is a request it cannot send by
// mistake.
enum class VendorRequest : std::uint8_t {
    Invalid = 0,
    ReceiverMode = 1,
    Si5351cWrite = 2,               // not sent: clock generator, factory business
    Si5351cRead = 3,                // not sent
    R820tWrite = 4,                 // not sent: the tuner is the firmware's to drive
    R820tRead = 5,                  // not sent
    SpiFlashErase = 6,              // not sent: this driver never writes flash
    SpiFlashWrite = 7,              // not sent
    SpiFlashRead = 8,               // not sent
    BoardIdRead = 9,
    VersionStringRead = 10,
    BoardPartIdSerialNoRead = 11,
    SetSampleRate = 12,
    SetFreq = 13,
    SetLnaGain = 14,
    SetMixerGain = 15,
    SetVgaGain = 16,
    SetLnaAgc = 17,
    SetMixerAgc = 18,
    MsVendorCmd = 19,               // not sent: the Microsoft OS descriptor request
    SetRfBiasCmd = 20,              // NOT SENT, and not a mistake - see kBiasTPortPin
    GpioWrite = 21,
    GpioRead = 22,                  // not sent
    GpioDirWrite = 23,              // not sent
    GpioDirRead = 24,               // not sent
    GetSampleRates = 25,
    SetPacking = 26,
    SpiFlashEraseSector = 27,       // not sent
};

constexpr std::uint8_t requestByte(VendorRequest r) { return static_cast<std::uint8_t>(r); }

// libairspy airspy_commands.h:32-36, receiver_mode_t.
enum class ReceiverMode : std::uint16_t {
    Off = 0,
    Rx = 1,
};

// libairspy airspy.c:559 - prepare_transfers is given LIBUSB_ENDPOINT_IN | 1.
constexpr std::uint8_t kRxEndpoint = 0x81;

// libairspy airspy.c:73, LIBUSB_CTRL_TIMEOUT_MS. Every control transfer in
// this driver uses it. Five times the HackRF's, and that is the reference's
// number, not a hedge: the Airspy answers SET_SAMPLERATE after it has
// reprogrammed its clock generator, which is not a register write.
//
// A std::chrono duration rather than a bare unsigned so that
// tests/test_shutdown_budget.cpp's scan can DISCOVER it - a wait that file has
// not been introduced to is a wait nothing holds the shutdown budget to. The
// transport takes milliseconds as an unsigned, so the call sites convert.
constexpr std::chrono::milliseconds kControlTimeout{500};
constexpr unsigned kControlTimeoutMs = static_cast<unsigned>(kControlTimeout.count());

// --- the bulk stream ------------------------------------------------------

// libairspy airspy.c:882 - transfer_count 16. Deeper than the HackRF's four
// because an Airspy at 10 MS/s complex is 20 MS/s of 12-bit samples and a
// host that is momentarily late has to have somewhere for them to land.
constexpr std::size_t kTransferCount = 16;

// libairspy airspy.c:1935 - the buffer size the reference switches to when
// packing is enabled, 6144 * 24. Its unpacked size is 262144 (airspy.c:883),
// which this driver never uses: PACKING IS ALWAYS ON here (see setPacking in
// the driver), because 12 bits in 12 bits rather than 12 bits in 16 is a third
// less USB bandwidth for exactly the same samples, and the bus is the scarce
// thing at 20 MS/s.
//
// It is also a multiple of 512, which WinUSB requires of a bulk read size:
// 147456 / 512 = 288.
constexpr std::size_t kPackedTransferBytes = 6144 * 24;
constexpr std::size_t kUnpackedTransferBytes = 262144;

// libairspy airspy.c:48-49, PACKET_SIZE 12 and UNPACKED_SIZE 16: twelve bytes
// on the wire are eight 12-bit samples. The reference computes the count as
// ((buffer_size / 2) * 4) / 3 (airspy.c:214, :375), which is the same
// arithmetic said less plainly - bytes * 2 / 3.
constexpr std::size_t kPackedBytesPerGroup = 12;
constexpr std::size_t kSamplesPerGroup = 8;

constexpr std::size_t packedSampleCount(std::size_t bytes) {
    return (bytes / kPackedBytesPerGroup) * kSamplesPerGroup;
}

// Real ADC samples in one whole packed transfer, and the complex samples they
// become after the conversion halves them.
constexpr std::size_t kRealSamplesPerTransfer = packedSampleCount(kPackedTransferBytes);
constexpr std::size_t kComplexSamplesPerTransfer = kRealSamplesPerTransfer / 2;

// --- the sample format ----------------------------------------------------

// libairspy airspy.c:58-62. The ADC is 12 bits encapsulated in 16, so
// SAMPLE_SHIFT is 4 and SAMPLE_SCALE is 1 / (1 << 11) = 1/2048; airspy.c:316
// is `(src[i] - 2048) * SAMPLE_SCALE`. An unsigned 12-bit code 0..4095 lands
// in [-1, +4095/4096], i.e. in [-1, 1) - the same interval every other stage
// in this application assumes, and the asymmetry is the offset-binary range's
// rather than a choice.
constexpr int kSampleOffset = 2048;
constexpr float kSampleScale = 1.0f / 2048.0f;

inline float sampleToFloat(std::uint16_t code) {
    return static_cast<float>(static_cast<int>(code) - kSampleOffset) * kSampleScale;
}

// libairspy airspy.c:323-338, unpack_samples. Three little-endian 32-bit words
// carry eight 12-bit samples, most significant field first. The reference
// reads uint32s straight out of the transfer buffer, which is correct only on
// a little-endian host; the bytes are assembled explicitly here so the answer
// is the same everywhere.
//
// `bytes` is truncated to whole 12-byte groups: a short transfer is a partial
// group we cannot decode, not a reason to read past the end.
inline std::size_t unpackSamples(const std::uint8_t* src, std::size_t bytes,
                                 std::uint16_t* dst, std::size_t dstCap) {
    const std::size_t groups = std::min(bytes / kPackedBytesPerGroup, dstCap / kSamplesPerGroup);
    std::size_t out = 0;
    for (std::size_t g = 0; g < groups; ++g) {
        const std::uint8_t* p = src + g * kPackedBytesPerGroup;
        std::uint32_t w[3];
        for (int k = 0; k < 3; ++k) {
            w[k] = static_cast<std::uint32_t>(p[4 * k + 0]) |
                   (static_cast<std::uint32_t>(p[4 * k + 1]) << 8) |
                   (static_cast<std::uint32_t>(p[4 * k + 2]) << 16) |
                   (static_cast<std::uint32_t>(p[4 * k + 3]) << 24);
        }
        dst[out + 0] = static_cast<std::uint16_t>((w[0] >> 20) & 0xFFFu);
        dst[out + 1] = static_cast<std::uint16_t>((w[0] >> 8) & 0xFFFu);
        dst[out + 2] = static_cast<std::uint16_t>(((w[0] & 0xFFu) << 4) | ((w[1] >> 28) & 0xFu));
        dst[out + 3] = static_cast<std::uint16_t>((w[1] & 0x0FFF0000u) >> 16);
        dst[out + 4] = static_cast<std::uint16_t>((w[1] & 0x0000FFF0u) >> 4);
        dst[out + 5] =
            static_cast<std::uint16_t>(((w[1] & 0xFu) << 8) | ((w[2] & 0xFF000000u) >> 24));
        dst[out + 6] = static_cast<std::uint16_t>((w[2] >> 12) & 0xFFFu);
        dst[out + 7] = static_cast<std::uint16_t>(w[2] & 0xFFFu);
        out += kSamplesPerGroup;
    }
    return out;
}

// --- the limits the reference documents -----------------------------------

// airspy.h:179 - "Parameter freq_hz shall be between 24000000(24MHz) and
// 1750000000(1.75GHz)".
constexpr double kMinFrequencyHz = 24.0e6;
constexpr double kMaxFrequencyHz = 1.75e9;

// airspy.h:182-189: LNA, MIXER and VGA are register indices, not decibels.
// libairspy clamps them at airspy.c:1691 (`value > 14`), :1721 and :1751
// (`value > 15`) - and the LNA's ceiling really is one lower than the other
// two.
constexpr int kLnaMaxIndex = 14;
constexpr int kMixerMaxIndex = 15;
constexpr int kVgaMaxIndex = 15;

// libairspy airspy.c:119, GAIN_COUNT - the length of the combined tables, so
// the linearity and sensitivity controls run 0..21.
constexpr int kCombinedGainCount = 22;
constexpr int kCombinedMaxIndex = kCombinedGainCount - 1;

// --- the combined gain tables ---------------------------------------------

// libairspy airspy.c:121-126, verbatim. Two curated walks up the three
// registers: "linearity" trades sensitivity for headroom against a strong
// neighbour, "sensitivity" the other way. They are the only published mapping
// from one number to a sensible LNA/MIXER/VGA triple, so they are transcribed
// rather than invented.
//
// THE INDEX IS REVERSED ON THE WAY IN. airspy.c:1838 and :1872 both do
// `value = GAIN_COUNT - 1 - value` before the lookup, so the tables are stored
// highest-gain-first and a user-facing 0 is the QUIETEST setting, 21 the
// loudest. combinedGainFor() below does the same flip, once, so no call site
// can forget it.
struct CombinedGain {
    std::uint8_t lna;
    std::uint8_t mixer;
    std::uint8_t vga;
};

// airspy.c:121-123.
constexpr std::uint8_t kLinearityVgaGains[kCombinedGainCount] = {
    13, 12, 11, 11, 11, 11, 11, 10, 10, 10, 10, 10, 10, 10, 10, 10, 9, 8, 7, 6, 5, 4};
constexpr std::uint8_t kLinearityMixerGains[kCombinedGainCount] = {
    12, 12, 11, 9, 8, 7, 6, 6, 5, 0, 0, 1, 0, 0, 2, 2, 1, 1, 1, 1, 0, 0};
constexpr std::uint8_t kLinearityLnaGains[kCombinedGainCount] = {
    14, 14, 14, 13, 12, 10, 9, 9, 8, 9, 8, 6, 5, 3, 1, 0, 0, 0, 0, 0, 0, 0};

// airspy.c:124-126.
constexpr std::uint8_t kSensitivityVgaGains[kCombinedGainCount] = {
    13, 12, 11, 10, 9, 8, 7, 6, 5, 5, 5, 5, 5, 4, 4, 4, 4, 4, 4, 4, 4, 4};
constexpr std::uint8_t kSensitivityMixerGains[kCombinedGainCount] = {
    12, 12, 12, 12, 11, 10, 10, 9, 9, 8, 7, 4, 4, 4, 3, 2, 2, 1, 0, 0, 0, 0};
constexpr std::uint8_t kSensitivityLnaGains[kCombinedGainCount] = {
    14, 14, 14, 14, 14, 14, 14, 14, 14, 13, 12, 12, 9, 9, 8, 7, 6, 5, 3, 2, 1, 0};

// airspy.c:1829-1861 (linearity) and :1863-1895 (sensitivity), the clamp and
// the reversal. `linearity` picks which pair of tables.
constexpr CombinedGain combinedGainFor(int index, bool linearity) {
    int v = index;
    if (v < 0) { v = 0; }                                   // ours: the panel clamps low too
    if (v >= kCombinedGainCount) { v = kCombinedMaxIndex; }  // airspy.c:1833-1836
    v = kCombinedMaxIndex - v;                               // airspy.c:1838
    return linearity ? CombinedGain{kLinearityLnaGains[v], kLinearityMixerGains[v],
                                    kLinearityVgaGains[v]}
                     : CombinedGain{kSensitivityLnaGains[v], kSensitivityMixerGains[v],
                                    kSensitivityVgaGains[v]};
}

// --- the bias tee ---------------------------------------------------------

// libairspy airspy.c:1897-1900: airspy_set_rf_bias is NOT AIRSPY_SET_RF_BIAS_CMD.
// It is airspy_gpio_write(GPIO_PORT1, GPIO_PIN13, value), and gpio_write
// (airspy.c:1357-1382) packs the port and pin into the INDEX word as
// `(port << 5) | pin` with the on/off in the VALUE word. So 0x2D, and request
// 21 rather than 20.
//
// Request 20 exists in the firmware's enumeration and the reference never
// sends it; this driver does not either. Sending the request whose NAME reads
// right instead of the one the reference actually uses is exactly the class of
// mistake a port makes, so the number is written down here with its
// derivation beside it.
constexpr std::uint16_t kBiasTPortPin = (1u << 5) | 13u;  // GPIO_PORT1, GPIO_PIN13 = 0x2D

// --- the half-band kernel -------------------------------------------------

// libairspy filters.h:28-79, HB_KERNEL_FLOAT - 47 taps, MIT licensed. Every
// odd-index tap is zero except the centre, which is the half-band's defining
// shape; only the 24 non-zero side taps and the centre are stored here,
// because a table of interleaved zeros is a table half of whose entries are an
// invitation to loop over them.
//
// MEASURED, so the description above is not taken on trust (the numbers are
// computed in tests/test_airspy_source.cpp from the taps themselves):
//   |H(0)|      = 0.999158      the passband, unity to within 0.09%
//   |H(pi/4)|   = 0.999161
//   |H(pi/2)|   = 0.500000      exactly half at the band edge, as a half-band
//   |H(3pi/4)|  = 0.000839      the stopband, -61 dB
//   |H(pi)|     = 0.000842
constexpr std::size_t kHalfBandSideTaps = 24;

// filters.h indices 0, 2, 4 ... 46.
constexpr float kHalfBandSide[kHalfBandSideTaps] = {
    -0.000998606272947510f, 0.001695637278417295f,  -0.003054430179754289f,
    0.005055504379767936f,  -0.007901319195893647f, 0.011873357051047719f,
    -0.017411159379930066f, 0.025304817427568772f,  -0.037225225204559217f,
    0.057533286997004301f,  -0.102327462004259350f, 0.317034472508947400f,
    0.317034472508947400f,  -0.102327462004259350f, 0.057533286997004301f,
    -0.037225225204559217f, 0.025304817427568772f,  -0.017411159379930066f,
    0.011873357051047719f,  -0.007901319195893647f, 0.005055504379767936f,
    -0.003054430179754289f, 0.001695637278417295f,  -0.000998606272947510f};

// filters.h index 23. The reference calls it `hbc` (iqconverter_float.c:87)
// and folds it into the rotation rather than the filter, which is what makes
// the Q branch a bare delay.
constexpr float kHalfBandCentre = 0.5f;

// The whole 47-tap kernel, rebuilt from the two above. Only the tests use it -
// they evaluate its frequency response to derive the amplitude a tone must
// come out at - but it lives here so there is exactly one statement of what
// the filter is.
inline std::array<float, 47> halfBandKernel() {
    std::array<float, 47> h{};
    for (std::size_t k = 0; k < kHalfBandSideTaps; ++k) { h[2 * k] = kHalfBandSide[k]; }
    h[23] = kHalfBandCentre;
    return h;
}

// libairspy iqconverter_float.c:450, SCALE - the leak of the DC remover's
// one-pole integrator.
constexpr float kDcRemoverScale = 0.01f;

// --- the real-to-complex conversion ---------------------------------------
//
// WHAT THE AIRSPY ACTUALLY SENDS, because getting this wrong makes every
// frequency on screen wrong by half a span and nothing else complains.
//
// The ADC is REAL and the R820T puts the wanted signal at a quarter of the ADC
// rate. The host translates by fs/4, filters with the half-band above and
// decimates by two, and the complex stream that comes out runs at HALF the
// real ADC rate. So a device running its ADC at 20 MS/s yields 10 MS/s
// complex.
//
// THE RATE THE FIRMWARE REPORTS IS THE COMPLEX ONE. This is the part that is
// easy to get backwards. libairspy airspy.c:1091-1117, airspy_get_samplerates:
// the stored list is handed back unchanged for an IQ sample type and DOUBLED
// (`buffer[i] *= 2`, :1107) for a real one. airspy.c:1119-1145,
// airspy_set_samplerate: a rate given as a VALUE rather than an index is
// multiplied by two for an IQ type (:1141) before being sent as kHz. And
// airspy.c:391-396, the FLOAT32_IQ case of the consumer, ends with
// `sample_count /= 2`. All three say the same thing: the 10000000 in the
// reference's own fallback list (airspy.c:904) is 10 MS/s COMPLEX, off a
// 20 MS/s real ADC. supportedSampleRatesHz() therefore returns the list
// verbatim and the pipeline's rate is that number.
//
// WHAT THE STRUCTURE IS. Per pair of real samples (one complex output):
//   - a leaky DC remover runs first, over the real samples in order;
//   - the fs/4 rotation multiplies real sample 2m by -(-1)^m and real sample
//     2m+1 by -(-1)^m * 0.5 (the centre tap, folded in here);
//   - the even phase goes through the 24 symmetric side taps;
//   - the odd phase goes through a 12-deep delay, which puts both branches at
//     the same instant (real index 2m - 23 for both).
// The composite is exactly
//
//     y[m] = -( h * v )[2m],   v[n] = x[n] * exp(+j*pi*n/2)
//
// and THAT closed form is what tests/test_airspy_source.cpp checks against,
// which is why this can be trusted without an Airspy: a real tone of
// amplitude A at frequency f in a real stream of rate fs comes out as a
// complex tone of amplitude A/2 * |H(2pi(fs/4-f)/fs)| at frequency fs/4 - f in
// a complex stream of rate fs/2. The test computes those two numbers from the
// taps and from the tone, not from this code.
class IqConverter {
public:
    IqConverter() { reset(); }

    // libairspy airspy.c:1196 resets the converter at every start_rx, and so
    // does this driver: the filter history and the DC average belong to one
    // stream and carrying them across a stop would put a burst of the previous
    // session's samples at the front of the new one.
    void reset() {
        hist_.fill(0.0f);
        delay_.fill(0.0f);
        histPos_ = 0;
        delayIndex_ = 0;
        dcAvg_ = 0.0f;
        pairParity_ = 0;
    }

    // `n` real samples in, floor(n/2) complex samples out. `dst` must have
    // room for floor(n/2). Returns what was written.
    //
    // THE ROTATION'S PHASE IS CARRIED ACROSS CALLS rather than restarted at
    // each buffer, which is the one place this differs from the reference
    // (iqconverter_float.c:488 restarts its `i` at every process()). They
    // agree whenever the buffer length is a multiple of four, which it always
    // is in the stream - kRealSamplesPerTransfer is 98304 - and where they
    // would differ, on a short transfer, a carried phase is the correct answer
    // and a restarted one puts a half-sample step in the middle of the signal.
    std::size_t process(const float* real, std::size_t n, std::complex<float>* dst) {
        const std::size_t pairs = n / 2;
        for (std::size_t m = 0; m < pairs; ++m) {
            // The DC remover runs over the real samples IN ORDER, both phases,
            // before anything else touches them (iqconverter_float.c:505,
            // remove_dc ahead of translate_fs_4). It is a one-pole high pass:
            //   y[n] = x[n] - avg;  avg += 0.01 * y[n]
            // i.e. (1 - z^-1) / (1 - 0.99 z^-1). It is not cosmetic - the
            // Airspy's ADC offset would otherwise land as a spur at the
            // rotation frequency, which is the middle of the display.
            const float e = removeDc(real[2 * m]);
            const float o = removeDc(real[2 * m + 1]);

            // exp(-j*pi*n/2) folded into a per-pair sign, with the half-band's
            // centre tap folded into the odd phase (iqconverter_float.c:491-494:
            // `-samples[j]`, `-samples[j+1]*hbc`, `samples[j+2]`,
            // `samples[j+3]*hbc`).
            const float sign = (pairParity_ == 0) ? -1.0f : 1.0f;
            pairParity_ ^= 1u;

            const float rotEven = sign * e;
            const float rotOdd = sign * kHalfBandCentre * o;

            // The even phase through the 24 symmetric taps. THE HISTORY IS
            // WRITTEN TWICE, at p and at p + 24, and p walks DOWNWARDS - so
            // the 24 most recent values, newest first, are always the 24
            // contiguous floats at &hist_[p] and the inner loop needs no
            // wrapping arithmetic at all. At 10 MS/s complex this loop runs
            // ten million times a second; a modulo per tap is not an option,
            // and the reference reaches for the same trick with a bigger
            // buffer and a periodic memcpy (iqconverter_float.c:257-261).
            histPos_ = (histPos_ == 0) ? (kHalfBandSideTaps - 1) : (histPos_ - 1);
            hist_[histPos_] = rotEven;
            hist_[histPos_ + kHalfBandSideTaps] = rotEven;
            const float* q0 = &hist_[histPos_];
            float acc = 0.0f;
            for (std::size_t k = 0; k < kHalfBandSideTaps / 2; ++k) {
                acc += kHalfBandSide[k] * (q0[k] + q0[kHalfBandSideTaps - 1 - k]);
            }

            // The odd phase through a 12-deep delay, which is where the two
            // branches meet: both then describe real index 2m - 23.
            const float qv = delay_[delayIndex_];
            delay_[delayIndex_] = rotOdd;
            ++delayIndex_;
            if (delayIndex_ >= kHalfBandSideTaps / 2) { delayIndex_ = 0; }

            dst[m] = std::complex<float>(acc, qv);
        }
        return pairs;
    }

private:
    float removeDc(float x) {
        const float y = x - dcAvg_;
        dcAvg_ += kDcRemoverScale * y;
        return y;
    }

    // Twice the tap count, for the double write described in process().
    std::array<float, kHalfBandSideTaps * 2> hist_{};
    std::array<float, kHalfBandSideTaps / 2> delay_{};
    std::size_t histPos_ = 0;
    std::size_t delayIndex_ = 0;
    float dcAvg_ = 0.0f;
    unsigned pairParity_ = 0;
};

// --- SET_FREQ -------------------------------------------------------------

// libairspy airspy.c:1631-1657, airspy_set_freq: value and index both zero and
// a four-byte little-endian Hz payload (set_freq_params_t, airspy.c:75-77).
// The reference's TO_LE is a no-op on a little-endian host and a byte swap on
// a big-endian one, which is what writing the bytes out by hand gives on
// every host.
constexpr std::size_t kFreqPayloadBytes = 4;

inline void encodeFreq(std::uint32_t freqHz, std::uint8_t out[kFreqPayloadBytes]) {
    for (std::size_t b = 0; b < kFreqPayloadBytes; ++b) {
        out[b] = static_cast<std::uint8_t>((freqHz >> (8 * b)) & 0xFFu);
    }
}

// --- GET_SAMPLERATES ------------------------------------------------------

// libairspy airspy.c:812-832, airspy_read_samplerates_from_fw: one IN request
// whose INDEX word is the number of rates wanted, answering that many
// little-endian uint32s. Index 0 asks for the COUNT and gets one word back
// (airspy.c:823: `(len > 0 ? len : 1) * sizeof(uint32_t)`).
constexpr std::size_t kSampleRateWordBytes = 4;

// A ceiling on what a firmware's answer is allowed to claim, so a wild count
// off a confused device cannot become a multi-gigabyte read. The reference
// mallocs whatever it is told (airspy.c:892); this does not.
constexpr std::uint32_t kMaxSampleRateCount = 32;

inline std::uint32_t decodeWord(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

}  // namespace cascade::source::airspy
