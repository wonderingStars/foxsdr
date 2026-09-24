// test_airspy_source.cpp - the Airspy R2 / Mini driver, proven byte for byte
// against a fake that answers as the firmware does, and sample for sample
// against signal theory.
//
// WHERE THE EXPECTATIONS COME FROM. There is no Airspy on this bench, so an
// expectation invented here would only prove this file agrees with itself.
// Every number below is one of three things, and each block says which:
//
//  1. READ STRAIGHT OUT OF libairspy, naming the function and line it came
//     from - the vendor request numbers, which transfers are IN and which are
//     OUT, the payload layouts, the gain tables, the clamps.
//  2. COMPUTED HERE FROM THE FILTER'S OWN COEFFICIENTS - the half-band's
//     frequency response at the points that matter, evaluated by summing the
//     taps, so "the passband is unity and the stopband is 60 dB down" is a
//     measurement and not a claim.
//  3. DERIVED FROM SIGNAL THEORY AND CHECKED TWO WAYS - the real-to-complex
//     conversion. The driver implements it as a rotation, a polyphase
//     half-band and a matched delay; this file predicts the answer with a
//     completely different expression,
//
//         y[m] = -( h * v )[2m],   v[n] = x[n] * exp(+j*pi*n/2)
//
//     - one plain convolution of the whole 47-tap kernel with the up-shifted
//     real stream, decimated. Two different arithmetics agreeing to 1e-6 over
//     thousands of samples is evidence; one of them compared against a copy of
//     itself is not. The same block then measures the output tone's FREQUENCY,
//     AMPLITUDE and SIDE OF ZERO against the closed-form prediction
//     (amplitude A/2, frequency fs/4 - f, in a stream of rate fs/2), which is
//     what catches a conjugation or a rate that is out by two.
//
// THE FIVE DELIBERATE BREAKS. Every block that matters was watched go RED
// against a broken driver before it was trusted green:
//   - a wrong request number (SET_SAMPLERATE 12 -> 11)
//   - a wrong unpack (the third sample's low nibble taken from the wrong word)
//   - a dropped bulk buffer (the reader skipping every second write)
//   - a hung reader (the bounded join replaced by a plain one)
//   - a wrong FIR tap (kHalfBandSide[11] 0.3170... -> 0.3180...)
// The report for this change-set carries the failing line each of them
// produced. A fix whose test never went red proves nothing, and a suite that
// has never been broken on purpose is a suite nobody has read.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "airspy_fake_usb.hpp"
#include "source/airspy_protocol.hpp"
#include "source/airspy_source.hpp"
#include "test_check.hpp"

using cascade::source::AirspySource;
using cascade::source::NativeDeviceInfo;
using cascade::test::AirspyControlRecord;
using cascade::test::FakeAirspyUsb;
namespace airspy = cascade::source::airspy;

namespace {

constexpr double kPi = 3.14159265358979323846;

// --- helpers ---------------------------------------------------------------

// Bounds-safe indexing. A `CHECK(v.size() == n)` followed by `v[i]` is an
// out-of-bounds read in exactly the run that has something to report - the
// harness records a failed check and carries on, so the crash lands instead of
// the message. (mayhem-b200, 2026-08-13; the same trap cost a whole session.)
const AirspyControlRecord& at(const std::vector<AirspyControlRecord>& v, std::size_t i) {
    static const AirspyControlRecord kAbsent{};
    return i < v.size() ? v[i] : kAbsent;
}

std::string hexOf(const std::vector<std::uint8_t>& b) {
    std::string out;
    char buf[8];
    for (const std::uint8_t x : b) {
        std::snprintf(buf, sizeof(buf), "%02x ", static_cast<unsigned>(x));
        out += buf;
    }
    return out;
}

bool sameBytes(const char* label, const std::vector<std::uint8_t>& got,
               const std::vector<int>& want) {
    bool ok = got.size() == want.size();
    if (ok) {
        std::size_t i = 0;
        for (const int w : want) {
            if (got[i++] != static_cast<std::uint8_t>(w)) {
                ok = false;
                break;
            }
        }
    }
    if (!ok) {
        std::string wantHex;
        char buf[8];
        for (const int w : want) {
            std::snprintf(buf, sizeof(buf), "%02x ", static_cast<unsigned>(w) & 0xFFu);
            wantHex += buf;
        }
        std::printf("     %s payload: got [%s] want [%s]\n", label, hexOf(got).c_str(),
                    wantHex.c_str());
    }
    return ok;
}

// One control transfer, whole. Compared as a unit rather than as four separate
// CHECKs so a failure names the transfer that is wrong instead of leaving four
// lines to be reassembled by hand.
bool isControl(const char* label, const AirspyControlRecord& r, bool in, int request, int value,
               int index) {
    const std::uint8_t wantType = in ? 0xC0 : 0x40;
    const bool ok = r.in == in && r.requestType == wantType &&
                    r.request == static_cast<std::uint8_t>(request) &&
                    r.value == static_cast<std::uint16_t>(value) &&
                    r.index == static_cast<std::uint16_t>(index);
    if (!ok) {
        std::printf(
            "     %s: got %s type 0x%02x request %u value 0x%04x index 0x%04x;"
            " want %s type 0x%02x request %u value 0x%04x index 0x%04x\n",
            label, r.in ? "IN" : "OUT", static_cast<unsigned>(r.requestType),
            static_cast<unsigned>(r.request), static_cast<unsigned>(r.value),
            static_cast<unsigned>(r.index), in ? "IN" : "OUT", static_cast<unsigned>(wantType),
            static_cast<unsigned>(request), static_cast<unsigned>(value),
            static_cast<unsigned>(index));
    }
    return ok;
}

std::vector<cascade::usb::UsbDeviceInfo> oneFakeDevice(const std::string& serial,
                                                       const std::string& description = "AIRSPY") {
    cascade::usb::UsbDeviceInfo d;
    d.vid = 0x1D50;
    d.pid = 0x60A1;
    d.path = "\\\\?\\usb#vid_1d50&pid_60a1#fake#{a5dcbf10}";
    d.serial = serial;
    d.description = description;
    return {d};
}

constexpr const char* kFakeSerial = "644866c83f1a51df";

// Points `src` at a fresh fake and returns a borrowed pointer to it. The fake
// is handed to the source on the FIRST successful open only, so a second one
// fails cleanly (with a null device) instead of two owners fighting over one
// object.
FakeAirspyUsb* attachFake(AirspySource& src,
                          std::vector<cascade::usb::UsbDeviceInfo> devices = {}) {
    if (devices.empty()) { devices = oneFakeDevice(kFakeSerial); }
    auto owned = std::make_unique<FakeAirspyUsb>();
    FakeAirspyUsb* raw = owned.get();
    auto holder = std::make_shared<std::unique_ptr<cascade::usb::UsbDevice>>(std::move(owned));
    src.setTransportForTest(std::move(devices),
                            [holder](const std::string&, std::string& error) {
                                if (*holder == nullptr) { error = "fake: already handed out"; }
                                return std::move(*holder);
                            });
    return raw;
}

// Waits for a predicate, bounded, so a test that would otherwise hang fails
// instead. Polling rather than a condition variable because what is being
// waited for lives inside the driver.
template <typename Fn>
bool waitFor(Fn fn, std::chrono::milliseconds bound) {
    const auto deadline = std::chrono::steady_clock::now() + bound;
    while (std::chrono::steady_clock::now() < deadline) {
        if (fn()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return fn();
}

// --- the packer, written from the LAYOUT rather than from the unpacker ------
//
// libairspy's unpack_samples (airspy.c:323-338) is eight bit-extraction
// expressions over three uint32s. What those expressions DESCRIBE is one
// continuous 96-bit stream of eight 12-bit fields, most significant bit first,
// cut into three 32-bit words - word 0 is stream bits 0..31 with stream bit 0
// as its bit 31, and so on, each word then going down the wire little-endian.
//
// This builds that stream. It is deliberately NOT the inverse of the eight
// expressions written out backwards: a packer derived from the same eight
// lines would agree with a mis-transcribed unpacker, and agreeing with the
// mistake is what a test must not do.
std::vector<std::uint8_t> packGroup(const std::uint16_t s[8]) {
    bool bits[96] = {false};
    for (int i = 0; i < 8; ++i) {
        for (int b = 0; b < 12; ++b) {
            // Field i occupies stream bits 12i .. 12i+11, MSB of the sample
            // first.
            bits[12 * i + b] = ((s[i] >> (11 - b)) & 1u) != 0u;
        }
    }
    std::vector<std::uint8_t> out(12, 0);
    for (int w = 0; w < 3; ++w) {
        std::uint32_t word = 0;
        for (int b = 0; b < 32; ++b) {
            if (bits[32 * w + b]) { word |= (1u << (31 - b)); }
        }
        for (int b = 0; b < 4; ++b) {
            out[4 * w + b] = static_cast<std::uint8_t>((word >> (8 * b)) & 0xFFu);
        }
    }
    return out;
}

std::vector<std::uint8_t> packSamples(const std::vector<std::uint16_t>& codes) {
    std::vector<std::uint8_t> out;
    for (std::size_t g = 0; g + 8 <= codes.size(); g += 8) {
        std::uint16_t group[8];
        for (int i = 0; i < 8; ++i) { group[i] = codes[g + i]; }
        const std::vector<std::uint8_t> bytes = packGroup(group);
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    return out;
}

// --- the independent model of the conversion -------------------------------
//
// THE WHOLE POINT OF THIS FUNCTION IS THAT IT SHARES NO ARITHMETIC WITH THE
// DRIVER. airspy::IqConverter rotates by fs/4, runs the even phase through 24
// polyphase taps and the odd phase through a 12-deep delay. This convolves the
// WHOLE 47-tap kernel with the up-shifted real stream and keeps every second
// result. They are the same operator written two ways, and their agreeing is
// the evidence that the polyphase decomposition is right - which is the one
// part of an Airspy driver that cannot be checked by looking at a transcript
// of USB bytes.
//
// The DC remover is the one piece that is stated the same way in both, because
// it IS one recurrence and there is no second form of it:
// iqconverter_float.c:452-464, y[n] = x[n] - avg then avg += 0.01 * y[n], i.e.
// (1 - z^-1) / (1 - 0.99 z^-1).
std::vector<std::complex<double>> modelConvert(const std::vector<double>& real) {
    const std::array<float, 47> h = airspy::halfBandKernel();

    std::vector<double> x(real.size());
    double avg = 0.0;
    for (std::size_t n = 0; n < real.size(); ++n) {
        x[n] = real[n] - avg;
        avg += static_cast<double>(airspy::kDcRemoverScale) * x[n];
    }

    // v[n] = x[n] * exp(+j*pi*n/2), which cycles 1, j, -1, -j.
    std::vector<std::complex<double>> v(x.size());
    for (std::size_t n = 0; n < x.size(); ++n) {
        switch (n & 3u) {
            case 0: v[n] = std::complex<double>(x[n], 0.0); break;
            case 1: v[n] = std::complex<double>(0.0, x[n]); break;
            case 2: v[n] = std::complex<double>(-x[n], 0.0); break;
            default: v[n] = std::complex<double>(0.0, -x[n]); break;
        }
    }

    std::vector<std::complex<double>> y(x.size() / 2);
    for (std::size_t m = 0; m < y.size(); ++m) {
        const std::size_t n = 2 * m;
        std::complex<double> acc(0.0, 0.0);
        for (std::size_t l = 0; l < h.size(); ++l) {
            if (l > n) { break; }  // the stream starts at zero, as the filter state does
            acc += static_cast<double>(h[l]) * v[n - l];
        }
        y[m] = -acc;
    }
    return y;
}

// |H(w)| of the half-band, evaluated from the taps themselves.
double halfBandMag(double w) {
    const std::array<float, 47> h = airspy::halfBandKernel();
    std::complex<double> acc(0.0, 0.0);
    for (std::size_t l = 0; l < h.size(); ++l) {
        acc += static_cast<double>(h[l]) * std::exp(std::complex<double>(0.0, -w * static_cast<double>(l)));
    }
    return std::abs(acc);
}

// |H(w)| of the DC remover, from its closed form.
double dcRemoverMag(double w) {
    const std::complex<double> z = std::exp(std::complex<double>(0.0, -w));
    return std::abs((1.0 - z) / (1.0 - 0.99 * z));
}

// One DFT bin of a complex block, normalised so a pure tone of amplitude A
// reads A.
std::complex<double> binAt(const std::vector<std::complex<double>>& blk, int k) {
    const std::size_t L = blk.size();
    std::complex<double> acc(0.0, 0.0);
    for (std::size_t n = 0; n < L; ++n) {
        acc += blk[n] * std::exp(std::complex<double>(
                            0.0, -2.0 * kPi * static_cast<double>(k) * static_cast<double>(n) /
                                     static_cast<double>(L)));
    }
    return acc / static_cast<double>(L);
}

}  // namespace

int main() {
    // =====================================================================
    // 1. THE PROTOCOL NUMBERS, against libairspy's own enumerations.
    // =====================================================================
    {
        // airspy_commands.h:55-85, airspy_vendor_request.
        CHECK(airspy::requestByte(airspy::VendorRequest::ReceiverMode) == 1);
        CHECK(airspy::requestByte(airspy::VendorRequest::Si5351cWrite) == 2);
        CHECK(airspy::requestByte(airspy::VendorRequest::Si5351cRead) == 3);
        CHECK(airspy::requestByte(airspy::VendorRequest::R820tWrite) == 4);
        CHECK(airspy::requestByte(airspy::VendorRequest::R820tRead) == 5);
        CHECK(airspy::requestByte(airspy::VendorRequest::SpiFlashErase) == 6);
        CHECK(airspy::requestByte(airspy::VendorRequest::SpiFlashWrite) == 7);
        CHECK(airspy::requestByte(airspy::VendorRequest::SpiFlashRead) == 8);
        CHECK(airspy::requestByte(airspy::VendorRequest::BoardIdRead) == 9);
        CHECK(airspy::requestByte(airspy::VendorRequest::VersionStringRead) == 10);
        CHECK(airspy::requestByte(airspy::VendorRequest::BoardPartIdSerialNoRead) == 11);
        CHECK(airspy::requestByte(airspy::VendorRequest::SetSampleRate) == 12);
        CHECK(airspy::requestByte(airspy::VendorRequest::SetFreq) == 13);
        CHECK(airspy::requestByte(airspy::VendorRequest::SetLnaGain) == 14);
        CHECK(airspy::requestByte(airspy::VendorRequest::SetMixerGain) == 15);
        CHECK(airspy::requestByte(airspy::VendorRequest::SetVgaGain) == 16);
        CHECK(airspy::requestByte(airspy::VendorRequest::SetLnaAgc) == 17);
        CHECK(airspy::requestByte(airspy::VendorRequest::SetMixerAgc) == 18);
        CHECK(airspy::requestByte(airspy::VendorRequest::MsVendorCmd) == 19);
        CHECK(airspy::requestByte(airspy::VendorRequest::SetRfBiasCmd) == 20);
        CHECK(airspy::requestByte(airspy::VendorRequest::GpioWrite) == 21);
        CHECK(airspy::requestByte(airspy::VendorRequest::GpioRead) == 22);
        CHECK(airspy::requestByte(airspy::VendorRequest::GpioDirWrite) == 23);
        CHECK(airspy::requestByte(airspy::VendorRequest::GpioDirRead) == 24);
        CHECK(airspy::requestByte(airspy::VendorRequest::GetSampleRates) == 25);
        CHECK(airspy::requestByte(airspy::VendorRequest::SetPacking) == 26);
        CHECK(airspy::requestByte(airspy::VendorRequest::SpiFlashEraseSector) == 27);

        // airspy.c:112-113, airspy.c:559, airspy.c:73.
        CHECK(airspy::kUsbVid == 0x1D50);
        CHECK(airspy::kUsbPid == 0x60A1);
        CHECK(airspy::kRxEndpoint == 0x81);
        CHECK(airspy::kControlTimeoutMs == 500);

        // airspy_commands.h:32-36.
        CHECK(static_cast<int>(airspy::ReceiverMode::Off) == 0);
        CHECK(static_cast<int>(airspy::ReceiverMode::Rx) == 1);

        // airspy.c:882 and :1935 - transfer count, and the buffer size the
        // reference uses when packing is on. Also that it is a legal WinUSB
        // bulk read size.
        CHECK(airspy::kTransferCount == 16);
        CHECK(airspy::kPackedTransferBytes == 6144 * 24);
        CHECK(airspy::kPackedTransferBytes == 147456);
        CHECK(airspy::kPackedTransferBytes % 512 == 0);
        CHECK(airspy::kUnpackedTransferBytes == 262144);

        // airspy.c:214 / :375 - ((buffer_size / 2) * 4) / 3, said as bytes*2/3.
        CHECK(airspy::packedSampleCount(airspy::kPackedTransferBytes) ==
              ((airspy::kPackedTransferBytes / 2) * 4) / 3);
        CHECK(airspy::kRealSamplesPerTransfer == 98304);
        CHECK(airspy::kComplexSamplesPerTransfer == 49152);
        // The conversion halves the count: real in, complex out at half the
        // rate (airspy.c:394, `sample_count /= 2`).
        CHECK(airspy::kComplexSamplesPerTransfer * 2 == airspy::kRealSamplesPerTransfer);

        // airspy.c:1897-1900 -> :1357-1382. The bias tee is GPIO_WRITE with
        // (GPIO_PORT1 << 5) | GPIO_PIN13, NOT request 20.
        CHECK(airspy::kBiasTPortPin == 45);
        CHECK(airspy::kBiasTPortPin == 0x2D);

        // airspy.h:179.
        CHECK_NEAR(airspy::kMinFrequencyHz, 24.0e6, 0.5);
        CHECK_NEAR(airspy::kMaxFrequencyHz, 1.75e9, 0.5);

        // airspy.c:1691, :1721, :1751, :119.
        CHECK(airspy::kLnaMaxIndex == 14);
        CHECK(airspy::kMixerMaxIndex == 15);
        CHECK(airspy::kVgaMaxIndex == 15);
        CHECK(airspy::kCombinedGainCount == 22);

        // airspy.c:58-62: 12 bits in 16, scale 1/(1 << 11), offset 2048
        // (airspy.c:316).
        CHECK(airspy::kSampleOffset == 2048);
        CHECK_NEAR(airspy::kSampleScale, 1.0 / 2048.0, 1e-12);
        CHECK_NEAR(airspy::sampleToFloat(2048), 0.0f, 1e-9);
        CHECK_NEAR(airspy::sampleToFloat(0), -1.0f, 1e-9);        // full negative is exactly -1
        CHECK_NEAR(airspy::sampleToFloat(4095), 2047.0f / 2048.0f, 1e-9);  // ...and stays inside 1
        CHECK(airspy::sampleToFloat(4095) < 1.0f);
        CHECK_NEAR(airspy::sampleToFloat(3072), 0.5f, 1e-9);
        CHECK_NEAR(airspy::sampleToFloat(1024), -0.5f, 1e-9);

        // airspy.c:1631-1657 and :75-77: four bytes of little-endian Hz.
        std::uint8_t freq[airspy::kFreqPayloadBytes];
        airspy::encodeFreq(100000000u, freq);  // 0x05F5E100
        CHECK(sameBytes("encodeFreq(100 MHz)",
                        std::vector<std::uint8_t>(freq, freq + sizeof(freq)),
                        {0x00, 0xE1, 0xF5, 0x05}));
        airspy::encodeFreq(1090000000u, freq);  // 0x40F98A80
        CHECK(sameBytes("encodeFreq(1.09 GHz)",
                        std::vector<std::uint8_t>(freq, freq + sizeof(freq)),
                        {0x80, 0x14, 0xF8, 0x40}));
        airspy::encodeFreq(1750000000u, freq);  // 0x684EE180
        CHECK(sameBytes("encodeFreq(1.75 GHz)",
                        std::vector<std::uint8_t>(freq, freq + sizeof(freq)),
                        {0x80, 0xE1, 0x4E, 0x68}));

        // The little-endian word decode the rate list and the serial use.
        const std::uint8_t word[4] = {0x80, 0x96, 0x98, 0x00};
        CHECK(airspy::decodeWord(word) == 10000000u);
    }

    // =====================================================================
    // 2. THE PACKING, against a packer built from the LAYOUT, not the
    //    reference's eight extraction expressions (see packGroup above).
    // =====================================================================
    {
        // Every bit position exercised: the extremes, a walking-ones pattern
        // and the two values that straddle a word boundary (samples 2 and 5,
        // which airspy.c:331 and :334 assemble from two words each).
        const std::vector<std::uint16_t> codes = {
            0x000, 0xFFF, 0x001, 0x800, 0xABC, 0x123, 0xFED, 0x555,
            0x400, 0x200, 0x100, 0x080, 0x040, 0x020, 0x010, 0x008,
        };
        const std::vector<std::uint8_t> packed = packSamples(codes);
        CHECK(packed.size() == 24);  // two groups of twelve bytes

        std::vector<std::uint16_t> out(codes.size() + 8, 0xDEAD);
        const std::size_t n = airspy::unpackSamples(packed.data(), packed.size(), out.data(),
                                                    out.size());
        CHECK(n == codes.size());
        std::size_t firstBad = codes.size();
        for (std::size_t i = 0; i < codes.size() && i < n; ++i) {
            if (out[i] != codes[i]) {
                firstBad = i;
                std::printf("     unpackSamples: sample %zu is 0x%03x, expected 0x%03x\n", i,
                            static_cast<unsigned>(out[i]), static_cast<unsigned>(codes[i]));
                break;
            }
        }
        CHECK(firstBad == codes.size());

        // A SHORT TRANSFER IS TRUNCATED TO WHOLE GROUPS, never decoded past
        // the end: eleven bytes are not a group and the reference's own loop
        // would read three words out of them.
        CHECK(airspy::unpackSamples(packed.data(), 11, out.data(), out.size()) == 0);
        CHECK(airspy::unpackSamples(packed.data(), 23, out.data(), out.size()) == 8);
        // ...and a destination too small is honoured as well, so no scripted
        // buffer can walk off the driver's own vector.
        CHECK(airspy::unpackSamples(packed.data(), packed.size(), out.data(), 8) == 8);
        CHECK(airspy::unpackSamples(packed.data(), packed.size(), out.data(), 7) == 0);
    }

    // =====================================================================
    // 3. THE GAIN TABLES, against airspy.c:121-126 and the reversal at :1838.
    // =====================================================================
    {
        // The reference REVERSES the index before the lookup, so a
        // user-facing 0 reads the LAST entry of each table and 21 reads the
        // first. These four are read straight off airspy.c:121-126.
        const airspy::CombinedGain lin0 = airspy::combinedGainFor(0, true);
        CHECK(lin0.vga == 4);     // airspy_linearity_vga_gains[21]
        CHECK(lin0.mixer == 0);   // airspy_linearity_mixer_gains[21]
        CHECK(lin0.lna == 0);     // airspy_linearity_lna_gains[21]
        const airspy::CombinedGain lin21 = airspy::combinedGainFor(21, true);
        CHECK(lin21.vga == 13);   // airspy_linearity_vga_gains[0]
        CHECK(lin21.mixer == 12);
        CHECK(lin21.lna == 14);
        const airspy::CombinedGain sen0 = airspy::combinedGainFor(0, false);
        CHECK(sen0.vga == 4);     // airspy_sensitivity_vga_gains[21]
        CHECK(sen0.mixer == 0);
        CHECK(sen0.lna == 0);
        const airspy::CombinedGain sen21 = airspy::combinedGainFor(21, false);
        CHECK(sen21.vga == 13);
        CHECK(sen21.mixer == 12);
        CHECK(sen21.lna == 14);

        // A middle entry of each, where the two curves differ - which is the
        // whole reason both exist. index 10 -> table index 11.
        const airspy::CombinedGain lin10 = airspy::combinedGainFor(10, true);
        CHECK(lin10.vga == 10);    // linearity_vga[11]
        CHECK(lin10.mixer == 1);   // linearity_mixer[11]
        CHECK(lin10.lna == 6);     // linearity_lna[11]
        const airspy::CombinedGain sen10 = airspy::combinedGainFor(10, false);
        CHECK(sen10.vga == 5);     // sensitivity_vga[11]
        CHECK(sen10.mixer == 4);   // sensitivity_mixer[11]
        CHECK(sen10.lna == 12);    // sensitivity_lna[11]

        // airspy.c:1833-1836 clamps high; the low clamp is ours (the panel's
        // contract is that an out-of-range gain is clamped, not refused).
        const airspy::CombinedGain hi = airspy::combinedGainFor(99, true);
        CHECK(hi.vga == lin21.vga && hi.mixer == lin21.mixer && hi.lna == lin21.lna);
        const airspy::CombinedGain lo = airspy::combinedGainFor(-4, true);
        CHECK(lo.vga == lin0.vga && lo.mixer == lin0.mixer && lo.lna == lin0.lna);

        // Every entry of every table is within the register's own ceiling -
        // a transcription slip that put a 16 in a VGA column would otherwise
        // only show up as a silently clamped gain on real hardware.
        for (int i = 0; i < airspy::kCombinedGainCount; ++i) {
            for (const bool linear : {true, false}) {
                const airspy::CombinedGain g = airspy::combinedGainFor(i, linear);
                CHECK(g.lna <= airspy::kLnaMaxIndex);
                CHECK(g.mixer <= airspy::kMixerMaxIndex);
                CHECK(g.vga <= airspy::kVgaMaxIndex);
            }
        }
        // ...and both curves are MONOTONIC in total register steps, which is
        // what makes them usable as a single slider at all.
        int prev = -1;
        for (int i = 0; i < airspy::kCombinedGainCount; ++i) {
            const airspy::CombinedGain g = airspy::combinedGainFor(i, true);
            const int total = g.lna + g.mixer + g.vga;
            if (total < prev) {
                std::printf("     linearity curve is not monotonic at index %d (%d after %d)\n", i,
                            total, prev);
            }
            CHECK(total >= prev);
            prev = total;
        }
    }

    // =====================================================================
    // 4. THE HALF-BAND ITSELF, measured from its own coefficients.
    // =====================================================================
    {
        const std::array<float, 47> h = airspy::halfBandKernel();
        CHECK(h.size() == 47);
        // A half-band's defining shape: every odd tap zero except the centre,
        // which is one half.
        CHECK_NEAR(h[23], 0.5, 1e-12);
        bool oddZero = true;
        for (std::size_t l = 1; l < h.size(); l += 2) {
            if (l != 23 && h[l] != 0.0f) { oddZero = false; }
        }
        CHECK(oddZero);
        // Symmetric, so the filter is linear phase and the delay the Q branch
        // has to match is a whole number of real samples (23).
        bool symmetric = true;
        for (std::size_t l = 0; l < h.size(); ++l) {
            if (h[l] != h[h.size() - 1 - l]) { symmetric = false; }
        }
        CHECK(symmetric);
        CHECK(airspy::kHalfBandSideTaps == 24);

        // MEASURED, not asserted: the response at the points the conversion
        // depends on. Unity in the passband, exactly a half at the band edge
        // (that is what "half-band" means), and 60 dB down in the stopband -
        // which IS the image rejection the whole structure buys.
        CHECK_NEAR(halfBandMag(0.0), 0.999158, 1e-5);
        CHECK_NEAR(halfBandMag(kPi / 4.0), 0.999161, 1e-5);
        CHECK_NEAR(halfBandMag(kPi / 2.0), 0.5, 1e-9);
        CHECK_NEAR(halfBandMag(3.0 * kPi / 4.0), 0.000839, 1e-5);
        CHECK_NEAR(halfBandMag(kPi), 0.000842, 1e-5);
        // Said as a rejection figure, because that is the number a user of the
        // radio would care about.
        const double rejectionDb =
            20.0 * std::log10(halfBandMag(kPi / 4.0) / halfBandMag(3.0 * kPi / 4.0));
        if (!(rejectionDb > 60.0)) {
            std::printf("     half-band image rejection is only %.1f dB\n", rejectionDb);
        }
        CHECK(rejectionDb > 60.0);
    }

    // =====================================================================
    // 5. THE CONVERSION: a real tone in, a complex tone out, at the
    //    frequency and amplitude theory says and on the right side of zero.
    // =====================================================================
    {
        // A 20 MS/s REAL ADC stream, which is what a device set to its
        // "10 MS/s" rate actually produces - see airspy_protocol.hpp for the
        // three places in the reference that prove the listed rate is the
        // COMPLEX one.
        const double fsReal = 20.0e6;
        const double fsComplex = fsReal / 2.0;
        const double f = fsReal / 8.0;  // 2.5 MHz, comfortably inside the passband
        const double A = 0.5;
        const double phi = 0.3;
        const std::size_t N = 16384;

        std::vector<double> x(N);
        std::vector<float> xf(N);
        for (std::size_t n = 0; n < N; ++n) {
            x[n] = A * std::cos(2.0 * kPi * f * static_cast<double>(n) / fsReal + phi);
            xf[n] = static_cast<float>(x[n]);
        }

        airspy::IqConverter cnv;
        std::vector<std::complex<float>> got(N / 2);
        const std::size_t produced = cnv.process(xf.data(), N, got.data());
        // HALF AS MANY SAMPLES OUT AS IN. If this is ever not true the
        // pipeline's rate is wrong by a factor of two and nothing else in the
        // application would notice.
        CHECK(produced == N / 2);

        // (a) Against the independent closed form, sample by sample. The first
        // 24 outputs are the filter filling; everything after that must agree.
        const std::vector<std::complex<double>> want = modelConvert(x);
        CHECK(want.size() == produced);
        double worst = 0.0;
        std::size_t worstAt = 0;
        for (std::size_t m = 24; m < produced; ++m) {
            const double e = std::abs(std::complex<double>(got[m].real(), got[m].imag()) - want[m]);
            if (e > worst) {
                worst = e;
                worstAt = m;
            }
        }
        if (!(worst < 2.0e-6)) {
            std::printf(
                "     conversion differs from the closed form by %.3e at sample %zu: got "
                "(%.9f, %.9f), model (%.9f, %.9f)\n",
                worst, worstAt, static_cast<double>(got[worstAt].real()),
                static_cast<double>(got[worstAt].imag()), want[worstAt].real(),
                want[worstAt].imag());
        }
        CHECK(worst < 2.0e-6);

        // (b) THE TONE ITSELF, MEASURED, at three frequencies - and the third
        // is the one that matters most.
        //
        // A real tone at f becomes a complex tone at fs/4 - f in a stream of
        // rate fs/2, with amplitude A/2 shaped by the half-band at the OUTPUT
        // frequency and by the DC remover at the INPUT one. The image, which
        // lands at fs/4 + f before the filter, is what the half-band is there
        // to remove, so it reads as that filter's stopband and nothing more.
        // Every factor below is computed from the taps and from the one-pole's
        // closed form; none is read out of the driver.
        //
        // fs/8 and fs/16 both come out at a POSITIVE frequency. 3fs/8 comes
        // out at a NEGATIVE one (fs/4 - 3fs/8 = -fs/8) and its rejected image
        // aliases to the positive bin - so a conversion that had the sign of
        // the rotation backwards would put its energy in exactly the bin this
        // case expects to be empty. That is the case a conjugation error
        // cannot survive, and it is why the mapping is tested as a relation
        // across three points rather than asserted at one.
        const std::size_t L = 4096;
        const std::size_t start = 2048;  // well past both settling transients
        CHECK(start + L <= produced);
        struct ToneCase {
            double f;
            double phase;
            int bin;
            const char* label;
        };
        const ToneCase tones[] = {
            {fsReal / 8.0, phi, 1024, "fs/8 -> +fs/8"},
            {fsReal / 16.0, 0.0, 1536, "fs/16 -> +3fs/16"},
            {3.0 * fsReal / 8.0, 0.0, -1024, "3fs/8 -> -fs/8 (the sign of the rotation)"},
        };
        for (const ToneCase& t : tones) {
            std::vector<float> xt(N);
            for (std::size_t n = 0; n < N; ++n) {
                xt[n] = static_cast<float>(
                    A * std::cos(2.0 * kPi * t.f * static_cast<double>(n) / fsReal + t.phase));
            }
            airspy::IqConverter c2;
            std::vector<std::complex<float>> yt(N / 2);
            CHECK(c2.process(xt.data(), N, yt.data()) == N / 2);

            // The bin the theory names, and a check that it is an EXACT bin -
            // otherwise the reading would be a leaked amplitude and the
            // comparison meaningless.
            const double fOut = fsReal / 4.0 - t.f;
            const double exact = fOut / fsComplex * static_cast<double>(L);
            CHECK_NEAR(exact, static_cast<double>(t.bin), 1e-9);

            std::vector<std::complex<double>> blk(L);
            for (std::size_t n = 0; n < L; ++n) {
                blk[n] = std::complex<double>(yt[start + n].real(), yt[start + n].imag());
            }

            const double hbPass = halfBandMag(2.0 * kPi * (fsReal / 4.0 - t.f) / fsReal);
            const double hbImage = halfBandMag(2.0 * kPi * (fsReal / 4.0 + t.f) / fsReal);
            const double dc = dcRemoverMag(2.0 * kPi * t.f / fsReal);
            const double wantMain = A / 2.0 * hbPass * dc;
            const double wantImage = A / 2.0 * hbImage * dc;

            const double gotMain = std::abs(binAt(blk, t.bin));
            const double gotImage = std::abs(binAt(blk, -t.bin));
            if (!(std::fabs(gotMain - wantMain) < 1.0e-5)) {
                std::printf("     %s: got %.9f at bin %+d, expected %.9f\n", t.label, gotMain,
                            t.bin, wantMain);
            }
            CHECK_NEAR(gotMain, wantMain, 1.0e-5);
            // THE SIDE OF ZERO. The mirror bin must carry only the filter's
            // leakage, three orders of magnitude down.
            if (!(gotImage < gotMain / 100.0)) {
                std::printf(
                    "     %s: the mirror bin carries %.9f against a wanted %.9f - the "
                    "conversion is conjugated or the filter is not rejecting\n",
                    t.label, gotImage, gotMain);
            }
            CHECK(gotImage < gotMain / 100.0);
            CHECK_NEAR(gotImage, wantImage, 1.0e-5);
        }

        // (d) The converter's state is CARRIED ACROSS CALLS. The same input
        // fed in four chunks must give the same answer as one call, or a
        // stream would have a discontinuity at every USB transfer boundary.
        {
            airspy::IqConverter split;
            std::vector<std::complex<float>> piecewise(N / 2);
            const std::size_t chunk = N / 4;
            std::size_t outAt = 0;
            for (std::size_t c = 0; c < 4; ++c) {
                outAt += split.process(xf.data() + c * chunk, chunk, piecewise.data() + outAt);
            }
            CHECK(outAt == N / 2);
            double d = 0.0;
            for (std::size_t m = 0; m < N / 2; ++m) {
                d = std::max(d, static_cast<double>(std::abs(piecewise[m] - got[m])));
            }
            if (!(d == 0.0)) {
                std::printf("     split processing differs from whole-buffer by %.3e\n", d);
            }
            CHECK(d == 0.0);
        }

        // (e) reset() really resets: the same input twice through one
        // converter with a reset between gives the same answer both times.
        {
            airspy::IqConverter r;
            std::vector<std::complex<float>> a(N / 2), b(N / 2);
            r.process(xf.data(), N, a.data());
            r.reset();
            r.process(xf.data(), N, b.data());
            double d = 0.0;
            for (std::size_t m = 0; m < N / 2; ++m) {
                d = std::max(d, static_cast<double>(std::abs(a[m] - b[m])));
            }
            CHECK(d == 0.0);
        }
    }

    // =====================================================================
    // 6. ENUMERATION: which devices are ours, and what reopens each one.
    // =====================================================================
    {
        std::vector<cascade::usb::UsbDeviceInfo> devices;
        cascade::usb::UsbDeviceInfo r2;
        r2.vid = 0x1D50;
        r2.pid = 0x60A1;
        r2.serial = "644866c83f1a51df";
        r2.description = "AIRSPY";
        r2.path = "p1";
        devices.push_back(r2);
        cascade::usb::UsbDeviceInfo mini;
        mini.vid = 0x1D50;
        mini.pid = 0x60A1;
        mini.serial = "deadbeefcafef00d";
        mini.description = "AIRSPY MINI";
        mini.path = "p2";
        devices.push_back(mini);
        cascade::usb::UsbDeviceInfo noSerial;
        noSerial.vid = 0x1D50;
        noSerial.pid = 0x60A1;
        noSerial.serial.clear();  // a device with no serial is still openable
        noSerial.description.clear();
        noSerial.path = "p3";
        devices.push_back(noSerial);
        cascade::usb::UsbDeviceInfo hackrf;
        hackrf.vid = 0x1D50;  // SAME VENDOR, different product: not ours
        hackrf.pid = 0x6089;
        hackrf.path = "p4";
        devices.push_back(hackrf);
        cascade::usb::UsbDeviceInfo rtl;
        rtl.vid = 0x0BDA;
        rtl.pid = 0x2838;
        rtl.path = "p5";
        devices.push_back(rtl);

        const std::vector<NativeDeviceInfo> found = cascade::source::airspyDevicesFrom(devices);
        CHECK(found.size() == 3);
        const NativeDeviceInfo absent;
        const NativeDeviceInfo& f0 = found.size() > 0 ? found[0] : absent;
        const NativeDeviceInfo& f1 = found.size() > 1 ? found[1] : absent;
        const NativeDeviceInfo& f2 = found.size() > 2 ? found[2] : absent;
        CHECK(f0.driver == "airspy");
        CHECK(f0.label == "Airspy R2 (serial 644866c83f1a51df)");
        CHECK(f0.args == "serial=644866c83f1a51df");
        CHECK(f1.label == "Airspy Mini (serial deadbeefcafef00d)");
        CHECK(f1.args == "serial=deadbeefcafef00d");
        // Nothing said which board it is, so nothing is claimed.
        CHECK(f2.label == "Airspy");
        CHECK(f2.args == "index=2");

        // ONE product id for the whole family (airspy.c:112-113) - a HackRF
        // shares the vendor and must not be in the list this driver asks for.
        const std::vector<cascade::usb::UsbId> ids = cascade::source::airspyUsbIds();
        CHECK(ids.size() == 1);
        CHECK(ids.size() == 1 && ids[0].vid == 0x1D50 && ids[0].pid == 0x60A1);

        // The model test itself, including the cases that matter: a firmware
        // version string rather than a product string, mixed case, and
        // nothing at all.
        CHECK(cascade::source::airspyModelFrom("AIRSPY MINI") == "Airspy Mini");
        CHECK(cascade::source::airspyModelFrom("AirSpy MINI v1.0.0-rc10") == "Airspy Mini");
        CHECK(cascade::source::airspyModelFrom("AirSpy NOS v1.0.0-rc10-6-g4008185") ==
              "Airspy R2");
        CHECK(cascade::source::airspyModelFrom("AIRSPY") == "Airspy R2");
        CHECK(cascade::source::airspyModelFrom("") == "Airspy");
        CHECK(cascade::source::airspyModelFrom("Bulk-In, Interface") == "Airspy");
    }

    // =====================================================================
    // 7. OPEN: the whole opening sequence, byte for byte.
    // =====================================================================
    {
        AirspySource src;
        FakeAirspyUsb* fake = attachFake(src);
        const bool opened = src.open("serial=644866c83f1a51df");
        if (!opened) { std::printf("     open() said: %s\n", src.lastError()); }
        CHECK(opened);
        CHECK(src.isOpen());
        CHECK(!src.faulted());
        CHECK(std::string(src.name()) == "Airspy: Airspy R2 (serial 644866c83f1a51df)");
        CHECK(src.model() == "Airspy R2");

        // The identity the driver read back, so a problem report can say which
        // board and which firmware without asking the radio again mid-stream.
        CHECK(src.boardId() == 0);  // AIRSPY_BOARD_ID_PROTO_AIRSPY, airspy.h:80
        CHECK(src.firmwareVersion() == "AirSpy NOS v1.0.0-rc10-6-g4008185 2020-05-08");
        // Words 2..5 of airspy_read_partid_serialno_t (airspy.h:108-111).
        CHECK(src.partIdSerialNo() == "0000000000000000644866c83f1a51df");

        const std::vector<AirspyControlRecord> c = fake->controls();
        if (c.size() != 15) {
            std::printf("     open() sent %zu control transfers, expected 15\n", c.size());
            for (std::size_t i = 0; i < c.size(); ++i) {
                std::printf("       [%zu] %s req %u value 0x%04x index 0x%04x data [%s]\n", i,
                            c[i].in ? "IN " : "OUT", static_cast<unsigned>(c[i].request),
                            static_cast<unsigned>(c[i].value), static_cast<unsigned>(c[i].index),
                            hexOf(c[i].data).c_str());
            }
        }
        CHECK(c.size() == 15);

        // airspy_board_id_read (airspy.c:1534-1554), airspy_version_string_read
        // (:1556-1590), airspy_board_partid_serialno_read (:1592-1623) - all
        // IN, value and index 0.
        CHECK(isControl("open[0] board id", at(c, 0), true, 9, 0, 0));
        CHECK(isControl("open[1] version", at(c, 1), true, 10, 0, 0));
        CHECK(isControl("open[2] part/serial", at(c, 2), true, 11, 0, 0));

        // RECEIVER_MODE OFF before anything is programmed: an Airspy left
        // streaming by the last application is exactly the wrong state in
        // which to change the transfer size (airspy.c:1170-1190, an OUT with
        // the mode in the VALUE word).
        CHECK(isControl("open[3] receiver off", at(c, 3), false, 1, 0, 0));

        // GET_SAMPLERATES twice: index 0 asks the COUNT, index N asks for N
        // rates (airspy.c:812-832, :889-893).
        CHECK(isControl("open[4] rate count", at(c, 4), true, 25, 0, 0));
        CHECK(sameBytes("open[4] rate count", at(c, 4).data, {0x02, 0x00, 0x00, 0x00}));
        CHECK(isControl("open[5] rate list", at(c, 5), true, 25, 0, 2));
        CHECK(sameBytes("open[5] rate list", at(c, 5).data,
                        {0x80, 0x96, 0x98, 0x00, 0xA0, 0x25, 0x26, 0x00}));

        // SET_PACKING on: an IN with the value in the INDEX word
        // (airspy.c:1913-1921).
        CHECK(isControl("open[6] packing on", at(c, 6), true, 26, 0, 1));
        CHECK(src.packingEnabled());

        // SET_SAMPLERATE BY INDEX (airspy.c:1151-1159). The firmware listed
        // 10 MS/s first, so the widest rate is its index 0.
        CHECK(isControl("open[7] sample rate index 0", at(c, 7), true, 12, 0, 0));
        // ...and the pipe was reset first, as libairspy clears the halt at
        // airspy.c:1147.
        CHECK(fake->resetPipeCalls() == 1);
        CHECK(fake->lastResetEndpoint() == 0x81);

        // SET_FREQ (airspy.c:1631-1657): an OUT, 100 MHz little-endian.
        CHECK(isControl("open[8] set freq", at(c, 8), false, 13, 0, 0));
        CHECK(sameBytes("open[8] set freq", at(c, 8).data, {0x00, 0xE1, 0xF5, 0x05}));

        // Both AGCs off, mixer first (airspy.c:1840, :1844), then the three
        // registers at mid-scale - the last of which is ours, because
        // libairspy programs no default at all.
        CHECK(isControl("open[9] mixer AGC off", at(c, 9), true, 18, 0, 0));
        CHECK(isControl("open[10] LNA AGC off", at(c, 10), true, 17, 0, 0));
        CHECK(isControl("open[11] LNA 8", at(c, 11), true, 14, 0, 8));
        CHECK(isControl("open[12] MIXER 8", at(c, 12), true, 15, 0, 8));
        CHECK(isControl("open[13] VGA 8", at(c, 13), true, 16, 0, 8));

        // The bias tee OFF, as a GPIO write - not request 20. A bias tee
        // inherited on is 4.5 V into somebody's antenna with nothing on screen
        // to say so.
        CHECK(isControl("open[14] bias tee off", at(c, 14), false, 21, 0, 0x2D));

        // Every control transfer carried libairspy's own 500 ms bound
        // (airspy.c:73).
        bool timeouts = true;
        for (const AirspyControlRecord& r : c) {
            if (r.timeoutMs != 500) { timeouts = false; }
        }
        CHECK(timeouts);

        // THE RATE IS THE COMPLEX ONE, and it is the highest the device listed.
        CHECK_NEAR(src.sampleRateHz(), 10.0e6, 0.5);
        const std::vector<double> rates = src.supportedSampleRatesHz();
        CHECK(rates.size() == 2);
        // Ascending, whatever order the firmware used.
        CHECK(rates.size() == 2 && rates[0] < rates[1]);
        CHECK(rates.size() == 2 && std::fabs(rates[0] - 2.5e6) < 0.5);
        CHECK(rates.size() == 2 && std::fabs(rates[1] - 10.0e6) < 0.5);

        CHECK_NEAR(src.centerFrequencyHz(), 100.0e6, 0.5);
        CHECK_NEAR(src.gainDb("LNA"), 8.0, 1e-9);
        CHECK_NEAR(src.gainDb("MIXER"), 8.0, 1e-9);
        CHECK_NEAR(src.gainDb("VGA"), 8.0, 1e-9);
        CHECK(!src.biasT());
        CHECK(!src.autoGain());
        // Unlike the HackRF, this radio HAS an AGC.
        CHECK(src.autoGainSupported());
        // FIVE GAINS, AND EVERY ONE OF THEM IS STEPS, NOT DECIBELS.
        //
        // libairspy takes an index for all five and publishes no decibel
        // mapping, so "LNA 7.0 dB" - what the Source section, the RECEIVER
        // card, the scope knob and the browser all printed until 0.92.0 - is
        // a number nothing produced wearing a unit this radio does not use.
        // The driver is the only place that knows; if this list ever grows a
        // sixth gain that really is decibels, this loop has to be the thing
        // that says so rather than a panel quietly guessing.
        {
            const std::vector<cascade::source::GainInfo> g = src.gains();
            CHECK(g.size() == 5);
            bool allSteps = !g.empty();
            std::string names;
            for (const cascade::source::GainInfo& one : g) {
                if (one.unit != cascade::source::GainUnit::Steps) {
                    allSteps = false;
                    names += one.name + " ";
                }
                // The range is the register's own, and a step is one index.
                CHECK_NEAR(one.minDb, 0.0, 1e-9);
                CHECK_NEAR(one.stepDb, 1.0, 1e-9);
                CHECK(one.maxDb >= 14.0);
            }
            if (!allSteps) {
                std::printf("  gains still claiming decibels: %s\n", names.c_str());
            }
            CHECK(allSteps);
        }
        CHECK(src.antennas().size() == 1);
        CHECK(src.setAntenna("RX"));
        CHECK(!src.setAntenna("TX"));
        double lo = 0.0, hi = 0.0;
        CHECK(src.frequencyRangeHz(lo, hi));
        CHECK_NEAR(lo, 24.0e6, 0.5);
        CHECK_NEAR(hi, 1.75e9, 0.5);
        CHECK(std::string(src.driverKey()) == "airspy");
        CHECK(src.selfPaced());
        src.closeDevice();
        CHECK(!src.isOpen());
    }

    // A Mini is recognised from its bus description, and a board that says
    // nothing there is recognised from its firmware version string instead.
    {
        AirspySource src;
        auto owned = std::make_unique<FakeAirspyUsb>();
        owned->firmwareVersion = "AirSpy MINI v1.0.0-rc10-6-g4008185 2020-05-08";
        owned->sampleRates = {6000000u, 3000000u};
        auto holder = std::make_shared<std::unique_ptr<cascade::usb::UsbDevice>>(std::move(owned));
        src.setTransportForTest(oneFakeDevice("aabbccdd11223344", ""),
                                [holder](const std::string&, std::string& error) {
                                    if (*holder == nullptr) { error = "fake: already handed out"; }
                                    return std::move(*holder);
                                });
        CHECK(src.open(""));
        CHECK(src.model() == "Airspy Mini");
        CHECK(std::string(src.name()) == "Airspy: Airspy Mini (serial aabbccdd11223344)");
        // AND THE RATES ARE THE DEVICE'S, not a table compiled into this
        // driver: a board that says 6 and 3 MS/s gets 6 and 3 MS/s.
        const std::vector<double> rates = src.supportedSampleRatesHz();
        CHECK(rates.size() == 2);
        CHECK(rates.size() == 2 && std::fabs(rates[0] - 3.0e6) < 0.5);
        CHECK(rates.size() == 2 && std::fabs(rates[1] - 6.0e6) < 0.5);
        CHECK_NEAR(src.sampleRateHz(), 6.0e6, 0.5);
        src.closeDevice();
    }

    // A firmware that will not answer GET_SAMPLERATES gets libairspy's own
    // fallback list (airspy.c:902-906) rather than an empty menu - and the
    // device is NOT condemned for it.
    {
        AirspySource src;
        auto owned = std::make_unique<FakeAirspyUsb>();
        owned->forcedRateCount = 0;  // a count of zero: nonsense, not a failure
        auto holder = std::make_shared<std::unique_ptr<cascade::usb::UsbDevice>>(std::move(owned));
        src.setTransportForTest(oneFakeDevice(kFakeSerial),
                                [holder](const std::string&, std::string& error) {
                                    if (*holder == nullptr) { error = "fake: already handed out"; }
                                    return std::move(*holder);
                                });
        CHECK(src.open(""));
        CHECK(!src.faulted());
        const std::vector<double> rates = src.supportedSampleRatesHz();
        CHECK(rates.size() == 2);
        CHECK(rates.size() == 2 && std::fabs(rates[0] - 2.5e6) < 0.5);
        CHECK(rates.size() == 2 && std::fabs(rates[1] - 10.0e6) < 0.5);
        src.closeDevice();
    }

    // =====================================================================
    // 8. RATES AND TUNES, each as the exact transfer it becomes.
    // =====================================================================
    {
        AirspySource src;
        FakeAirspyUsb* fake = attachFake(src);
        CHECK(src.open(""));  // no args: the first device
        fake->clearControls();

        // 2.5 MS/s is the firmware's INDEX 1 - the list is ascending here and
        // descending on the wire, so an index taken from the wrong one would
        // set the wrong rate and nothing would say so.
        CHECK(src.setSampleRateHz(2.5e6));
        {
            const std::vector<AirspyControlRecord> c = fake->controls();
            CHECK(c.size() == 1);
            CHECK(isControl("rate 2.5 MS/s", at(c, 0), true, 12, 0, 1));
        }
        CHECK_NEAR(src.sampleRateHz(), 2.5e6, 0.5);

        // ...and back to 10 MS/s, which is index 0.
        fake->clearControls();
        CHECK(src.setSampleRateHz(10.0e6));
        {
            const std::vector<AirspyControlRecord> c = fake->controls();
            CHECK(c.size() == 1);
            CHECK(isControl("rate 10 MS/s", at(c, 0), true, 12, 0, 0));
        }
        CHECK_NEAR(src.sampleRateHz(), 10.0e6, 0.5);

        // COERCED TO THE NEAREST, never refused: the hardware has a menu, so
        // there is no such thing as an unsupported-but-close rate to fail on,
        // and the readback says what was actually programmed.
        fake->clearControls();
        CHECK(src.setSampleRateHz(4.0e6));
        CHECK_NEAR(src.sampleRateHz(), 2.5e6, 0.5);
        {
            const std::vector<AirspyControlRecord> c = fake->controls();
            CHECK(c.size() == 1);
            CHECK(isControl("4 MS/s coerced to 2.5", at(c, 0), true, 12, 0, 1));
        }
        fake->clearControls();
        CHECK(src.setSampleRateHz(40.0e6));
        CHECK_NEAR(src.sampleRateHz(), 10.0e6, 0.5);
        fake->clearControls();
        CHECK(src.setSampleRateHz(1.0e3));
        CHECK_NEAR(src.sampleRateHz(), 2.5e6, 0.5);
        // A nonsense rate is refused without a transfer.
        fake->clearControls();
        CHECK(!src.setSampleRateHz(0.0));
        CHECK(!src.setSampleRateHz(-1.0));
        CHECK(fake->controlCount() == 0);
        CHECK(!src.faulted());
        CHECK(src.setSampleRateHz(10.0e6));

        // Every rate change reset the pipe first (airspy.c:1147).
        const int resetsBefore = fake->resetPipeCalls();
        fake->clearControls();
        CHECK(src.setSampleRateHz(2.5e6));
        CHECK(fake->resetPipeCalls() == resetsBefore + 1);

        // --- tuning ------------------------------------------------------
        struct TuneCase {
            double hz;
            std::vector<int> bytes;
            const char* label;
        };
        const TuneCase tunes[] = {
            {100.0e6, {0x00, 0xE1, 0xF5, 0x05}, "tune 100 MHz"},
            {1.09e9, {0x80, 0x14, 0xF8, 0x40}, "tune 1.09 GHz"},
            {24.0e6, {0x00, 0x36, 0x6E, 0x01}, "tune 24 MHz (the bottom)"},
            {1.75e9, {0x80, 0xE1, 0x4E, 0x68}, "tune 1.75 GHz (the top)"},
            {433.92e6, {0x00, 0x18, 0xDD, 0x19}, "tune 433.92 MHz"},
        };
        for (const TuneCase& t : tunes) {
            fake->clearControls();
            const bool ok = src.setCenterFrequencyHz(t.hz);
            if (!ok) { std::printf("     %s said: %s\n", t.label, src.lastError()); }
            CHECK(ok);
            const std::vector<AirspyControlRecord> c = fake->controls();
            CHECK(c.size() == 1);
            CHECK(isControl(t.label, at(c, 0), false, 13, 0, 0));
            CHECK(sameBytes(t.label, at(c, 0).data, t.bytes));
            CHECK_NEAR(src.centerFrequencyHz(), t.hz, 0.5);
        }

        // Outside 24 MHz - 1.75 GHz is refused, and nothing is sent: a tune
        // that silently lands somewhere else is worse than one that does not
        // happen.
        fake->clearControls();
        CHECK(!src.setCenterFrequencyHz(23.9e6));
        CHECK(!src.setCenterFrequencyHz(1.8e9));
        CHECK(!src.setCenterFrequencyHz(0.0));
        CHECK(fake->controlCount() == 0);
        CHECK(!src.faulted());

        src.closeDevice();
    }

    // =====================================================================
    // 9. GAINS, AGC, BIAS TEE.
    // =====================================================================
    {
        AirspySource src;
        FakeAirspyUsb* fake = attachFake(src);
        CHECK(src.open(""));

        // The three registers, each an IN with the value in the INDEX word.
        fake->clearControls();
        CHECK(src.setGainDb("LNA", 12.0));
        CHECK(src.setGainDb("MIXER", 5.0));
        CHECK(src.setGainDb("VGA", 15.0));
        {
            const std::vector<AirspyControlRecord> c = fake->controls();
            CHECK(c.size() == 3);
            CHECK(isControl("LNA 12", at(c, 0), true, 14, 0, 12));
            CHECK(isControl("MIXER 5", at(c, 1), true, 15, 0, 5));
            CHECK(isControl("VGA 15", at(c, 2), true, 16, 0, 15));
        }
        CHECK_NEAR(src.gainDb("LNA"), 12.0, 1e-9);
        CHECK_NEAR(src.gainDb("MIXER"), 5.0, 1e-9);
        CHECK_NEAR(src.gainDb("VGA"), 15.0, 1e-9);

        // CLAMPED, not refused - and the LNA's ceiling really is one lower
        // than the other two (airspy.c:1691 against :1721 and :1751).
        fake->clearControls();
        CHECK(src.setGainDb("LNA", 99.0));
        CHECK(src.setGainDb("MIXER", 99.0));
        CHECK(src.setGainDb("VGA", -5.0));
        {
            const std::vector<AirspyControlRecord> c = fake->controls();
            CHECK(c.size() == 3);
            CHECK(isControl("LNA clamped to 14", at(c, 0), true, 14, 0, 14));
            CHECK(isControl("MIXER clamped to 15", at(c, 1), true, 15, 0, 15));
            CHECK(isControl("VGA clamped to 0", at(c, 2), true, 16, 0, 0));
        }
        CHECK_NEAR(src.gainDb("LNA"), 14.0, 1e-9);
        CHECK_NEAR(src.gainDb("MIXER"), 15.0, 1e-9);
        CHECK_NEAR(src.gainDb("VGA"), 0.0, 1e-9);

        // A fractional value is rounded to the nearest step, not truncated.
        fake->clearControls();
        CHECK(src.setGainDb("LNA", 7.6));
        {
            const std::vector<AirspyControlRecord> c = fake->controls();
            CHECK(c.size() == 1);
            CHECK(isControl("LNA 7.6 rounds to 8", at(c, 0), true, 14, 0, 8));
        }

        // THE COMBINED CURVES. airspy_set_linearity_gain (airspy.c:1829-1861):
        // mixer AGC off, LNA AGC off, then VGA, MIXER, LNA from the tables -
        // five transfers, in that order, and the order is the reference's
        // because a table programmed while an AGC is still moving a register
        // has not set the gain it says it has.
        fake->clearControls();
        CHECK(src.setGainDb("LINEARITY", 10.0));
        {
            const std::vector<AirspyControlRecord> c = fake->controls();
            CHECK(c.size() == 5);
            CHECK(isControl("linearity: mixer AGC off", at(c, 0), true, 18, 0, 0));
            CHECK(isControl("linearity: LNA AGC off", at(c, 1), true, 17, 0, 0));
            // index 10 -> table index 11: vga 10, mixer 1, lna 6.
            CHECK(isControl("linearity: VGA", at(c, 2), true, 16, 0, 10));
            CHECK(isControl("linearity: MIXER", at(c, 3), true, 15, 0, 1));
            CHECK(isControl("linearity: LNA", at(c, 4), true, 14, 0, 6));
        }
        // The three registers now report what the TABLE programmed, not what
        // was there before.
        CHECK_NEAR(src.gainDb("VGA"), 10.0, 1e-9);
        CHECK_NEAR(src.gainDb("MIXER"), 1.0, 1e-9);
        CHECK_NEAR(src.gainDb("LNA"), 6.0, 1e-9);
        CHECK_NEAR(src.gainDb("LINEARITY"), 10.0, 1e-9);
        // ...and only ONE of the two curves describes the radio at a time.
        CHECK_NEAR(src.gainDb("SENSITIVITY"), -1.0, 1e-9);

        fake->clearControls();
        CHECK(src.setGainDb("SENSITIVITY", 10.0));
        {
            const std::vector<AirspyControlRecord> c = fake->controls();
            CHECK(c.size() == 5);
            // index 10 -> table index 11: vga 5, mixer 4, lna 12.
            CHECK(isControl("sensitivity: VGA", at(c, 2), true, 16, 0, 5));
            CHECK(isControl("sensitivity: MIXER", at(c, 3), true, 15, 0, 4));
            CHECK(isControl("sensitivity: LNA", at(c, 4), true, 14, 0, 12));
        }
        CHECK_NEAR(src.gainDb("SENSITIVITY"), 10.0, 1e-9);
        CHECK_NEAR(src.gainDb("LINEARITY"), -1.0, 1e-9);

        // --- the AGCs ----------------------------------------------------
        fake->clearControls();
        CHECK(src.setAutoGain(true));
        CHECK(src.autoGain());
        {
            const std::vector<AirspyControlRecord> c = fake->controls();
            CHECK(c.size() == 2);
            // BOTH of them: half an AGC is a configuration nobody asked for.
            CHECK(isControl("AGC on: mixer", at(c, 0), true, 18, 0, 1));
            CHECK(isControl("AGC on: LNA", at(c, 1), true, 17, 0, 1));
        }
        fake->clearControls();
        CHECK(src.setAutoGain(false));
        CHECK(!src.autoGain());
        {
            const std::vector<AirspyControlRecord> c = fake->controls();
            CHECK(c.size() == 2);
            CHECK(isControl("AGC off: mixer", at(c, 0), true, 18, 0, 0));
            CHECK(isControl("AGC off: LNA", at(c, 1), true, 17, 0, 0));
        }
        // A combined curve turns the AGC back off, because it has to.
        CHECK(src.setAutoGain(true));
        CHECK(src.autoGain());
        CHECK(src.setGainDb("LINEARITY", 5.0));
        CHECK(!src.autoGain());

        // --- the bias tee ------------------------------------------------
        fake->clearControls();
        CHECK(src.setBiasT(true));
        CHECK(src.biasT());
        {
            const std::vector<AirspyControlRecord> c = fake->controls();
            CHECK(c.size() == 1);
            // GPIO_WRITE, port 1 pin 13 - NOT request 20.
            CHECK(isControl("bias tee on", at(c, 0), false, 21, 1, 0x2D));
        }
        fake->clearControls();
        CHECK(src.setBiasT(false));
        CHECK(!src.biasT());
        {
            const std::vector<AirspyControlRecord> c = fake->controls();
            CHECK(c.size() == 1);
            CHECK(isControl("bias tee off", at(c, 0), false, 21, 0, 0x2D));
        }

        // An unknown gain name is a refusal, not a transfer.
        fake->clearControls();
        CHECK(!src.setGainDb("TUNER", 20.0));
        CHECK(!src.setGainDb("IF", 20.0));
        CHECK(fake->controlCount() == 0);
        CHECK(!src.faulted());
        CHECK_NEAR(src.gainDb("TUNER"), 0.0, 1e-9);

        // Closing leaves the bias tee off, whatever it was, because the next
        // application inherits what we leave behind.
        //
        // The transcript is taken BEFORE the close and read after it: the fake
        // is owned by the source and `fake` is a dangling pointer the moment
        // closeDevice() releases it, which is why FakeAirspyUsb keeps its
        // record behind a shared_ptr.
        CHECK(src.setBiasT(true));
        const std::shared_ptr<cascade::test::AirspyTranscript> tx = fake->transcript();
        tx->clear();
        src.closeDevice();
        {
            const std::vector<AirspyControlRecord> c = tx->snapshot();
            bool sawBiasOff = false;
            for (const AirspyControlRecord& r : c) {
                if (r.request == 21 && r.index == 0x2D && r.value == 0) { sawBiasOff = true; }
            }
            if (!sawBiasOff) {
                std::printf("     closeDevice() sent %zu transfers and none of them was the "
                            "bias tee off\n",
                            c.size());
            }
            CHECK(sawBiasOff);
        }
    }

    // =====================================================================
    // 10. START AND STOP: the receiver, and the bulk ring around it.
    // =====================================================================
    {
        AirspySource src;
        FakeAirspyUsb* fake = attachFake(src);
        CHECK(src.open(""));
        fake->clearControls();

        CHECK(src.start());
        CHECK(src.running());
        // airspy_set_receiver_mode (airspy.c:1170-1190): the mode is the VALUE
        // word, OFF is 0 and RX is 1. airspy_start_rx sends both, OFF first
        // (:1202, :1210); test 17 pins where the halt-clear and the ring go.
        {
            const std::vector<AirspyControlRecord> c = fake->controls();
            CHECK(c.size() == 2);
            CHECK(isControl("start: OFF", at(c, 0), false, 1, 0, 0));
            CHECK(isControl("start: RX", at(c, 1), false, 1, 1, 0));
        }
        // The ring is queued AFTER the receiver is told to stream (test 17),
        // on the RX endpoint, with libairspy's own packed geometry.
        CHECK(fake->beginBulkCalls() == 1);
        CHECK(fake->lastBulkEndpoint() == 0x81);
        CHECK(fake->lastBulkBufferBytes() == 147456);
        CHECK(fake->lastBulkBufferCount() == 16);

        CHECK(src.start());  // idempotent
        CHECK(fake->beginBulkCalls() == 1);

        fake->clearControls();
        src.stop();
        CHECK(!src.running());
        {
            const std::vector<AirspyControlRecord> c = fake->controls();
            CHECK(c.size() == 1);
            CHECK(isControl("stop: OFF", at(c, 0), false, 1, 0, 0));
        }
        CHECK(fake->endBulkCalls() >= 1);

        // A rate change on a RUNNING stream happens with the radio quiet, and
        // running() is true either side of it.
        CHECK(src.start());
        fake->clearControls();
        const int beginsBefore = fake->beginBulkCalls();
        CHECK(src.setSampleRateHz(2.5e6));
        CHECK(src.running());
        {
            const std::vector<AirspyControlRecord> c = fake->controls();
            CHECK(c.size() == 4);
            CHECK(isControl("rate change: OFF first", at(c, 0), false, 1, 0, 0));
            CHECK(isControl("rate change: new index", at(c, 1), true, 12, 0, 1));
            CHECK(isControl("rate change: restart OFF", at(c, 2), false, 1, 0, 0));
            CHECK(isControl("rate change: RX again", at(c, 3), false, 1, 1, 0));
        }
        CHECK(!src.faulted());
        CHECK(fake->beginBulkCalls() == beginsBefore + 1);
        CHECK_NEAR(src.sampleRateHz(), 2.5e6, 0.5);
        src.closeDevice();
    }

    // =====================================================================
    // 11. STREAMING: three scripted packed buffers, every sample once and in
    //     order, converted as the closed form says.
    // =====================================================================
    {
        AirspySource src;
        FakeAirspyUsb* fake = attachFake(src);
        CHECK(src.open(""));

        // 96 bytes is eight packed groups, i.e. 64 real samples and 32 complex
        // ones. A ramp with a stride that is coprime with 4096 walks the whole
        // code space, so a lost buffer, a repeated one or a swapped pair all
        // show up as a mismatch at a named index rather than as a count that
        // happens to come out right.
        constexpr std::size_t kBuffers = 3;
        constexpr std::size_t kSamplesPerBuffer = 64;
        std::vector<std::uint16_t> allCodes;
        for (std::size_t b = 0; b < kBuffers; ++b) {
            std::vector<std::uint16_t> codes(kSamplesPerBuffer);
            for (std::size_t i = 0; i < kSamplesPerBuffer; ++i) {
                codes[i] = static_cast<std::uint16_t>(
                    ((b * kSamplesPerBuffer + i) * 317u + 1024u) & 0xFFFu);
            }
            allCodes.insert(allCodes.end(), codes.begin(), codes.end());
            fake->queueBulk(packSamples(codes));
        }
        const std::size_t wantSamples = allCodes.size() / 2;
        CHECK(wantSamples == 96);

        CHECK(src.start());
        std::vector<std::complex<float>> got;
        got.reserve(wantSamples + 16);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (got.size() < wantSamples && std::chrono::steady_clock::now() < deadline) {
            std::complex<float> chunk[32];
            const std::size_t n = src.read(chunk, 32);
            for (std::size_t i = 0; i < n; ++i) { got.push_back(chunk[i]); }
        }
        if (got.size() != wantSamples) {
            std::printf("     streaming: got %zu samples, expected %zu\n", got.size(),
                        wantSamples);
        }
        CHECK(got.size() == wantSamples);

        // THE EXPECTATION IS THE CLOSED FORM over the whole 192-sample real
        // stream, run CONTINUOUSLY across the three buffers - which is what
        // makes this a test of "nothing lost and nothing reordered" as well as
        // of the conversion. A driver that restarted its filter at each
        // transfer would fail here even though every sample arrived.
        std::vector<double> reals(allCodes.size());
        for (std::size_t i = 0; i < allCodes.size(); ++i) {
            reals[i] = static_cast<double>(airspy::sampleToFloat(allCodes[i]));
        }
        const std::vector<std::complex<double>> want = modelConvert(reals);
        CHECK(want.size() == wantSamples);
        std::size_t firstBad = wantSamples;
        double worst = 0.0;
        for (std::size_t i = 0; i < wantSamples && i < got.size(); ++i) {
            const double e =
                std::abs(std::complex<double>(got[i].real(), got[i].imag()) - want[i]);
            if (e > worst) { worst = e; }
            if (e > 2.0e-6 && firstBad == wantSamples) {
                firstBad = i;
                std::printf(
                    "     streaming: sample %zu is (%.9f, %.9f), expected (%.9f, %.9f)\n", i,
                    static_cast<double>(got[i].real()), static_cast<double>(got[i].imag()),
                    want[i].real(), want[i].imag());
            }
        }
        CHECK(firstBad == wantSamples);
        CHECK(worst < 2.0e-6);

        // Nothing duplicated behind it: with the queue empty the reader has
        // nothing more to deliver, so a further read is the contract's zero.
        std::complex<float> tail[8];
        CHECK(src.read(tail, 8) == 0);
        CHECK(src.droppedTransfers() == 0);

        // The health line, in the same words the Soapy path writes (the log a
        // crash report carries must not need to know which driver was open).
        const std::string line = src.streamHealthLine();
        CHECK(line.find("source: stream health - reads ") == 0);
        CHECK(line.find(", with samples ") != std::string::npos);
        CHECK(line.find(", timeouts ") != std::string::npos);
        CHECK(line.find(", overflows ") != std::string::npos);
        CHECK(line.find(", errors ") != std::string::npos);
        CHECK(line.find(", longest gap ") != std::string::npos);
        CHECK(line.find(" samples in ") != std::string::npos);

        src.closeDevice();
    }

    // =====================================================================
    // 12. STOP WHILE STREAMING comes back promptly.
    // =====================================================================
    {
        AirspySource src;
        FakeAirspyUsb* fake = attachFake(src);
        CHECK(src.open(""));
        CHECK(src.start());
        // Let the reader get into its loop with nothing to deliver - the state
        // a radio between transfers is actually in.
        CHECK(waitFor([fake] { return fake->readBulkInFlight(); },
                      std::chrono::milliseconds(500)));
        const auto t0 = std::chrono::steady_clock::now();
        src.stop();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t0).count();
        if (elapsed >= 500) { std::printf("     stop() took %lld ms\n", (long long)elapsed); }
        CHECK(elapsed < 500);
        CHECK(!src.running());
        src.closeDevice();
    }

    // =====================================================================
    // 13. THE DEVICE GOES: the reader leaves, and says why.
    // =====================================================================
    {
        const unsigned long long abandonedBefore = AirspySource::readersAbandoned();
        AirspySource src;
        FakeAirspyUsb* fake = attachFake(src);
        CHECK(src.open(""));
        fake->onExhausted.store(FakeAirspyUsb::Exhausted::DeviceGone);
        CHECK(src.start());

        CHECK(waitFor([&src] { return src.faulted(); }, std::chrono::milliseconds(2000)));
        CHECK(src.faulted());
        CHECK(src.deviceDead());
        CHECK(src.faultedWhile() == "reading samples");
        CHECK(std::string(src.lastError()).find("stopped answering") != std::string::npos);
        // read() on a faulted source is the contract's 0, not a hang and not a
        // crash - the pipeline's source loop polls faulted() and leaves.
        std::complex<float> buf[8];
        CHECK(src.read(buf, 8) == 0);

        // The reader EXITED rather than being abandoned: stop() comes straight
        // back and the process-wide abandonment count has not moved. Elapsed
        // time alone would still pass with the bound deleted; this number is
        // the difference.
        const auto t0 = std::chrono::steady_clock::now();
        src.stop();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t0).count();
        CHECK(elapsed < 500);
        CHECK(AirspySource::readersAbandoned() == abandonedBefore);
        src.closeDevice();
    }

    // =====================================================================
    // 14. A READER THAT WILL NOT COME BACK is abandoned, not waited for.
    // =====================================================================
    {
        const unsigned long long abandonedBefore = AirspySource::readersAbandoned();
        AirspySource src;
        FakeAirspyUsb* fake = attachFake(src);
        CHECK(src.open(""));
        fake->onExhausted.store(FakeAirspyUsb::Exhausted::Block);
        CHECK(src.start());
        CHECK(waitFor([fake] { return fake->readBulkInFlight(); },
                      std::chrono::milliseconds(500)));

        const auto t0 = std::chrono::steady_clock::now();
        src.stop();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t0).count();
        // It waited the bound, and it did NOT wait longer. The lower edge
        // matters as much as the upper one: a stop that returned instantly
        // would mean the join was never attempted.
        if (!(elapsed >= 900 && elapsed < 2500)) {
            std::printf("     stop() on a wedged reader took %lld ms; expected about %lld\n",
                        (long long)elapsed,
                        (long long)AirspySource::kReaderJoinWait.count());
        }
        CHECK(elapsed >= 900);
        CHECK(elapsed < 2500);
        CHECK(AirspySource::readersAbandoned() == abandonedBefore + 1);
        CHECK(src.deviceDead());
        CHECK(!src.running());
        CHECK(src.faultedWhile() == "waiting for the sample reader to stop");

        // Let the stranded reader finish. The device it is inside was leaked
        // deliberately (see stopStreamingLocked), so it has somewhere valid to
        // land; without this the suite would leave a thread sleeping in it.
        fake->releaseBlock.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        src.closeDevice();
    }

    // =====================================================================
    // 15. OPENING WHAT IS NOT THERE fails cleanly.
    // =====================================================================
    {
        AirspySource src;
        // No devices at all.
        src.setTransportForTest({}, [](const std::string&, std::string& error) {
            error = "should never be reached";
            return std::unique_ptr<cascade::usb::UsbDevice>();
        });
        CHECK(!src.open(""));
        CHECK(!src.isOpen());
        CHECK(std::string(src.lastError()).find("no Airspy found") != std::string::npos);
        // Nothing to stop, nothing to read, nothing to crash on.
        src.stop();
        std::complex<float> buf[4];
        CHECK(src.read(buf, 4) == 0);
        CHECK(!src.start());
        CHECK(src.supportedSampleRatesHz().empty());
        CHECK(!src.setSampleRateHz(10.0e6));
        CHECK(!src.setCenterFrequencyHz(100.0e6));
        CHECK(!src.setGainDb("LNA", 8.0));
        CHECK(!src.setAutoGain(true));
        CHECK(!src.setBiasT(true));
        src.closeDevice();
    }
    {
        AirspySource src;
        attachFake(src);
        // The right family, the wrong serial.
        CHECK(!src.open("serial=ffffffffffffffff"));
        CHECK(!src.isOpen());
        CHECK(std::string(src.lastError()).find("ffffffffffffffff") != std::string::npos);
        // An index past the end.
        CHECK(!src.open("index=4"));
        CHECK(std::string(src.lastError()).find("index 4") != std::string::npos);
        // The short form of a serial still finds it: what a user reads off
        // another tool's listing is the tail of the full sixteen digits.
        CHECK(src.open("serial=3f1a51df"));
        CHECK(src.isOpen());
        src.closeDevice();
    }

    // =====================================================================
    // 16. A CONTROL TRANSFER THAT FAILS condemns the device.
    // =====================================================================
    {
        AirspySource src;
        FakeAirspyUsb* fake = attachFake(src);
        CHECK(src.open(""));
        fake->failingRequests.push_back(13);  // SET_FREQ
        CHECK(!src.setCenterFrequencyHz(144.0e6));
        CHECK(src.faulted());
        CHECK(src.deviceDead());
        CHECK(src.faultedWhile() == "setting the centre frequency");
        // Nothing further is sent to a dead device, whatever is asked of it.
        fake->clearControls();
        CHECK(!src.setCenterFrequencyHz(145.0e6));
        CHECK(!src.setGainDb("LNA", 8.0));
        CHECK(!src.setGainDb("LINEARITY", 8.0));
        CHECK(!src.setSampleRateHz(2.5e6));
        CHECK(!src.setAutoGain(true));
        CHECK(!src.setBiasT(true));
        CHECK(!src.start());
        CHECK(fake->controlCount() == 0);
        src.closeDevice();
    }

    // An open that fails half way through leaves nothing behind - once for
    // each of the four reads open() depends on.
    {
        struct FailCase {
            std::uint8_t request;
            const char* what;
        };
        const FailCase cases[] = {
            {9, "reading the board id"},
            {10, "reading the firmware version"},
            {11, "reading the part id and serial number"},
            {25, "reading the sample-rate count"},
            {26, "switching 12-bit sample packing on"},
            {12, "setting the sample rate"},
        };
        for (const FailCase& fc : cases) {
            AirspySource src;
            FakeAirspyUsb* fake = attachFake(src);
            fake->failingRequests.push_back(fc.request);
            const bool opened = src.open("");
            if (opened) {
                std::printf("     open() succeeded with request %u failing\n",
                            static_cast<unsigned>(fc.request));
            }
            CHECK(!opened);
            CHECK(!src.isOpen());
            CHECK(src.faulted());
            if (src.faultedWhile() != fc.what) {
                std::printf("     request %u: faultedWhile() is \"%s\", expected \"%s\"\n",
                            static_cast<unsigned>(fc.request), src.faultedWhile().c_str(),
                            fc.what);
            }
            CHECK(src.faultedWhile() == fc.what);
            src.closeDevice();
        }
    }

    // =====================================================================
    // 17. THE START SEQUENCE IS libairspy's, step for step.
    //
    // airspy_start_rx (airspy.c:1192-1218 at airspyone_host fc61ab6):
    // RECEIVER_MODE OFF, libusb_clear_halt on 0x81, RECEIVER_MODE RX, and only
    // THEN create_io_threads -> prepare_transfers (:549-559), which submits the
    // bulk transfers. The firmware disables endpoint 0x81 on every mode change
    // and re-enables it only on RX (airspy_rx.c set_receiver_mode), so reads
    // queued before RX are reads against a disabled endpoint. The field
    // reports (0.99.27/0.99.28, Airspy R2, first read "Windows error 31")
    // are what queueing them first looks like.
    // =====================================================================
    {
        AirspySource src;
        FakeAirspyUsb* fake = attachFake(src);
        CHECK(src.open(""));
        fake->clearEvents();
        CHECK(src.start());
        const std::vector<std::string> e = fake->events();
        const std::vector<std::string> want = {"OUT 1 val 0", "CLEAR_HALT 0x81", "OUT 1 val 1",
                                               "BEGIN_BULK"};
        if (e != want) {
            std::printf("     start sequence was:");
            for (const std::string& s : e) { std::printf(" [%s]", s.c_str()); }
            std::printf("\n     libairspy's is:     ");
            for (const std::string& s : want) { std::printf(" [%s]", s.c_str()); }
            std::printf("\n");
        }
        CHECK(e == want);
        src.closeDevice();
    }

    // =====================================================================
    // 18. THE FIELD REPORT, replayed: an R2 on firmware rc10 that a crashed
    //     session left streaming, restored at 2.5 MS/s, then started. It must
    //     stream, and no bulk read may fail on the way.
    // =====================================================================
    {
        AirspySource src;
        FakeAirspyUsb* fake = attachFake(src);
        fake->leaveStreamingFromCrashedSession();
        CHECK(src.open(""));
        // app_window.cpp's restore path: openDeviceSync (open, then the saved
        // rate), then the saved centre, then the pipeline starts the source.
        CHECK(src.setSampleRateHz(2.5e6));
        CHECK(src.setCenterFrequencyHz(100.0e6));
        for (std::size_t b = 0; b < 3; ++b) {
            std::vector<std::uint16_t> codes(64);
            for (std::size_t i = 0; i < codes.size(); ++i) {
                codes[i] = static_cast<std::uint16_t>(((b * 64 + i) * 211u + 7u) & 0xFFFu);
            }
            fake->queueBulk(packSamples(codes));
        }
        CHECK(src.start());
        std::size_t got = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (got < 96 && !src.faulted() && std::chrono::steady_clock::now() < deadline) {
            std::complex<float> chunk[32];
            got += src.read(chunk, 32);
        }
        if (got != 96 || src.faulted()) {
            std::printf("     field replay: %zu of 96 samples, faulted=%d, failed reads %d: %s\n",
                        got, src.faulted() ? 1 : 0, fake->failedBulkReads(), src.lastError());
        }
        CHECK(got == 96);
        CHECK(!src.faulted());
        CHECK(fake->failedBulkReads() == 0);
        src.closeDevice();
    }

    // =====================================================================
    // 19. ONE FAILED READ IS NOT THE END OF THE SESSION.
    //
    // libairspy itself gives up on the first transfer that does not complete
    // (airspy.c airspy_libusb_transfer_callback: streaming = false), and so did
    // this driver - with "unplug it and plug it back in" on screen for a radio
    // that was fine. A halted pipe is cleared by exactly the steps
    // airspy_start_rx takes, so the reader takes them again: receiver off,
    // clear the halt, receiver on, queue the ring. Bounded (test 20).
    // =====================================================================
    {
        AirspySource src;
        FakeAirspyUsb* fake = attachFake(src);
        CHECK(src.open(""));
        for (std::size_t b = 0; b < 3; ++b) {
            std::vector<std::uint16_t> codes(64);
            for (std::size_t i = 0; i < codes.size(); ++i) {
                codes[i] = static_cast<std::uint16_t>(((b * 64 + i) * 173u + 99u) & 0xFFFu);
            }
            fake->queueBulk(packSamples(codes));
        }
        // The first buffer arrives, then the pipe halts under the second.
        fake->haltAfterReads.store(1);
        CHECK(src.start());
        fake->clearEvents();
        std::size_t got = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (got < 96 && !src.faulted() && std::chrono::steady_clock::now() < deadline) {
            std::complex<float> chunk[32];
            got += src.read(chunk, 32);
        }
        if (got != 96 || src.faulted()) {
            std::printf("     one halt: %zu of 96 samples, faulted=%d: %s\n", got,
                        src.faulted() ? 1 : 0, src.lastError());
        }
        CHECK(fake->failedBulkReads() >= 1);  // the halt really happened
        CHECK(got == 96);                     // and nothing queued behind it was lost
        CHECK(!src.faulted());
        CHECK(!src.deviceDead());
        CHECK(src.running());
        // The re-arm is the reference's start sequence, in its order, with
        // the old ring torn down first.
        {
            const std::vector<std::string> e = fake->events();
            const std::vector<std::string> want = {"END_BULK", "OUT 1 val 0", "CLEAR_HALT 0x81",
                                                   "OUT 1 val 1", "BEGIN_BULK"};
            bool found = false;
            for (std::size_t s = 0; s + want.size() <= e.size() && !found; ++s) {
                found = std::equal(want.begin(), want.end(), e.begin() + static_cast<long>(s));
            }
            if (!found) {
                std::printf("     events after the halt:");
                for (const std::string& s : e) { std::printf(" [%s]", s.c_str()); }
                std::printf("\n");
            }
            CHECK(found);
        }
        src.closeDevice();
    }

    // =====================================================================
    // 20. ...BUT A PIPE THAT WILL NOT CLEAR still faults, after a bounded
    //     number of re-arms, with the Windows error in the message.
    // =====================================================================
    {
        const unsigned long long abandonedBefore = AirspySource::readersAbandoned();
        AirspySource src;
        FakeAirspyUsb* fake = attachFake(src);
        CHECK(src.open(""));
        fake->queueBulk(packSamples(std::vector<std::uint16_t>(64, 2048)));
        fake->haltIsPermanent.store(true);
        fake->haltAfterReads.store(1);
        CHECK(src.start());
        CHECK(waitFor([&src] { return src.faulted(); }, std::chrono::milliseconds(3000)));
        CHECK(src.deviceDead());
        CHECK(src.faultedWhile() == "reading samples");
        CHECK(std::string(src.lastError()).find("Windows error 31") != std::string::npos);
        // One ring from start(), then kMaxStreamRearms (3) re-armed ones.
        CHECK(AirspySource::kMaxStreamRearms == 3);
        int begins = 0;
        for (const std::string& s : fake->events()) { begins += (s == "BEGIN_BULK") ? 1 : 0; }
        if (begins != 4) { std::printf("     permanent halt: %d rings queued, want 4\n", begins); }
        CHECK(begins == 4);
        const auto t0 = std::chrono::steady_clock::now();
        src.stop();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t0).count();
        CHECK(elapsed < 500);
        CHECK(AirspySource::readersAbandoned() == abandonedBefore);
        src.closeDevice();
    }

    // =====================================================================
    // 21. A RADIO THAT IS REALLY GONE is not re-armed: the first control
    //     transfer of the re-arm fails, and the fault names the READ, which
    //     is the first cause.
    // =====================================================================
    {
        AirspySource src;
        FakeAirspyUsb* fake = attachFake(src);
        CHECK(src.open(""));
        CHECK(src.start());
        CHECK(waitFor([fake] { return fake->readBulkInFlight(); }, std::chrono::milliseconds(500)));
        // Published to the reader through the bulk queue's mutex below: the
        // reader only calls a control transfer after it has taken the buffer.
        fake->failingRequests.push_back(1);  // RECEIVER_MODE
        fake->haltAfterReads.store(1);
        fake->queueBulk(packSamples(std::vector<std::uint16_t>(64, 2048)));
        CHECK(waitFor([&src] { return src.faulted(); }, std::chrono::milliseconds(2000)));
        CHECK(src.deviceDead());
        CHECK(src.faultedWhile() == "reading samples");
        int begins = 0;
        for (const std::string& s : fake->events()) { begins += (s == "BEGIN_BULK") ? 1 : 0; }
        CHECK(begins == 1);
        src.stop();
        src.closeDevice();
    }

    // =====================================================================
    // 22. STOPPING DOES NOT CONDEMN THE RADIO.
    //
    // RECEIVER_MODE OFF disables the bulk endpoint while reads are still
    // queued on it, so those reads FAIL - on the real firmware and in this
    // fake. libairspy's airspy_stop_rx sets stop_requested BEFORE it sends OFF
    // (airspy.c:1220-1235) and its callback ignores everything after that.
    // This driver lowered its run flag only after OFF had been acknowledged,
    // so a read failing in between was recorded as a dead device - and the
    // rate change that stopped the stream then refused to run.
    // =====================================================================
    {
        AirspySource src;
        FakeAirspyUsb* fake = attachFake(src);
        CHECK(src.open(""));
        CHECK(src.start());
        CHECK(waitFor([fake] { return fake->readBulkInFlight(); }, std::chrono::milliseconds(500)));
        fake->clearEvents();
        src.stop();
        if (src.faulted()) { std::printf("     stop() left: %s\n", src.lastError()); }
        CHECK(!src.faulted());
        CHECK(!src.deviceDead());
        CHECK(!fake->receiverInRx());
        // A stop is ONE receiver-off and the ring torn down - and nothing
        // else. The read that OFF fails must not be mistaken for a halt and
        // "recovered" from: that would be the reader re-arming (and switching
        // the receiver back on) behind the back of the stop.
        {
            const std::vector<std::string> e = fake->events();
            const std::vector<std::string> want = {"OUT 1 val 0", "END_BULK"};
            if (e != want) {
                std::printf("     stop sequence was:");
                for (const std::string& s : e) { std::printf(" [%s]", s.c_str()); }
                std::printf("\n");
            }
            CHECK(e == want);
        }
        // The session goes on: a new rate, and streaming again.
        CHECK(src.setSampleRateHz(2.5e6));
        CHECK(src.start());
        CHECK(src.running());
        // And the live rate change, which stops and starts inside one call.
        CHECK(waitFor([fake] { return fake->readBulkInFlight(); }, std::chrono::milliseconds(500)));
        CHECK(src.setSampleRateHz(10.0e6));
        CHECK(!src.faulted());
        CHECK(src.running());
        CHECK_NEAR(src.sampleRateHz(), 10.0e6, 0.5);
        src.closeDevice();
    }

    // =====================================================================
    // 23. A STOP THAT LANDS IN THE MIDDLE OF A RE-ARM leaves the receiver OFF.
    //
    // The reader re-arms without devMutex_ (the GUI thread may hold it while
    // waiting for the reader), so a stop can arrive between the re-arm's
    // receiver-off and its receiver-on. If the reader then went ahead, the
    // radio would be streaming again after stop() had returned - into a host
    // that has stopped listening. The receiver-mode switches are slowed down
    // here so the stop lands inside the re-arm's OFF every time.
    // =====================================================================
    {
        AirspySource src;
        FakeAirspyUsb* fake = attachFake(src);
        CHECK(src.open(""));
        fake->modeSwitchDelayMs.store(60);
        CHECK(src.start());
        CHECK(waitFor([fake] { return fake->readBulkInFlight(); }, std::chrono::milliseconds(500)));
        fake->clearEvents();
        fake->haltAfterReads.store(1);
        fake->queueBulk(packSamples(std::vector<std::uint16_t>(64, 2048)));
        // The re-arm has begun: its first act is tearing the old ring down,
        // and its second (the OFF) is now sleeping in the fake for 60 ms.
        const bool inRearm = waitFor(
            [fake] {
                for (const std::string& s : fake->events()) {
                    if (s == "END_BULK") { return true; }
                }
                return false;
            },
            std::chrono::milliseconds(1000));
        CHECK(inRearm);
        src.stop();
        CHECK(!src.running());
        CHECK(!src.faulted());
        if (fake->receiverInRx()) {
            std::printf("     the re-arm switched the receiver back on after stop():");
            for (const std::string& s : fake->events()) { std::printf(" [%s]", s.c_str()); }
            std::printf("\n");
        }
        CHECK(!fake->receiverInRx());
        int begins = 0;
        for (const std::string& s : fake->events()) { begins += (s == "BEGIN_BULK") ? 1 : 0; }
        CHECK(begins == 0);
        fake->modeSwitchDelayMs.store(10);
        src.closeDevice();
    }

    return testSummary("test_airspy_source");
}
