// test_airspyhf_source.cpp - the Airspy HF+ driver, proven byte for byte
// against a fake that answers as the firmware does.
//
// WHERE THE EXPECTATIONS COME FROM. There is no Airspy HF+ on this bench, so
// an expectation invented here would only prove this file agrees with itself.
// Every number below is either read straight out of libairspyhf's source (the
// vendor request numbers, the payload layouts, the request/answer shapes -
// each one names the function it came from) or is COMPUTED HERE, a second
// time, from the reference's own formula written out again beside the line
// number it came from. The tuning arithmetic in particular is done twice: the
// driver's computeTuning() is one implementation and expectedTuning() in this
// file is another, and where the answer reaches the wire the bytes are also
// pinned as literals worked out by hand. Two implementations that agree and a
// hand-computed byte string is as close to a reference radio as this bench
// can get.
//
// THE FIVE DELIBERATE BREAKS. Every block that matters was watched go RED
// against a broken driver before it was trusted green:
//   - a wrong request number (SET_SAMPLERATE 4 -> 5)
//   - wrong delta arithmetic (the calibration ppb dropped from the tune)
//   - a dropped bulk buffer (the reader skipping every second write)
//   - the reader's join bound raised from 1 s to 10 s. NOT replaced with a
//     plain join, deliberately: the wedged-reader block only releases the
//     fake AFTER stop() returns, so a plain join would deadlock the suite
//     rather than fail it. Raising the bound past what the block allows
//     proves the same thing - that the elapsed time is produced by that
//     constant and nothing else - and comes back.
//   - the IQ balancer's estimator disabled (estimateImbalance returning at
//     its first line)
// The report for this change-set carries the failing line each of them
// produced. A fix whose test never went red proves nothing, and a suite that
// has never been broken on purpose is a suite nobody has read.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "airspyhf_fake_usb.hpp"
#include "source/airspyhf_protocol.hpp"
#include "source/airspyhf_source.hpp"
#include "test_check.hpp"

using cascade::source::AirspyHfSource;
using cascade::source::NativeDeviceInfo;
using cascade::test::AirspyHfControlRecord;
using cascade::test::FakeAirspyHfUsb;
namespace airspyhf = cascade::source::airspyhf;

namespace {

// --- helpers ---------------------------------------------------------------

// Bounds-safe indexing. A `CHECK(v.size() == n)` followed by `v[i]` is an
// out-of-bounds read in exactly the run that has something to report - the
// harness records a failed check and carries on, so the crash lands instead of
// the message. (mayhem-b200, 2026-08-13; the same trap cost a whole session.)
const AirspyHfControlRecord& at(const std::vector<AirspyHfControlRecord>& v, std::size_t i) {
    static const AirspyHfControlRecord kAbsent{};
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

// One control transfer, whole. Compared as a unit rather than as four
// separate CHECKs so a failure names the transfer that is wrong instead of
// leaving four lines to be reassembled by hand.
bool isControl(const char* label, const AirspyHfControlRecord& r, bool in, int request, int value,
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

// THE SECOND IMPLEMENTATION of airspyhf_set_freq_double (airspyhf.c:1365-1420),
// written out here from the reference's own lines rather than called out of
// the driver, so that "the driver agrees with itself" cannot pass for a check.
struct ExpectedTune {
    std::uint32_t loKhz = 0;
    double shiftHz = 0.0;
};

ExpectedTune expectedTuning(double freqHz, int ppb, bool lowIf, bool dsp, double deltaHz) {
    const double ifShift = (dsp && !lowIf) ? 5000.0 : 0.0;          // DEFAULT_IF_SHIFT
    const double adjusted = freqHz * (1.0e9 + ppb) * 1.0e-9;        // :1368
    const double floorKhz = lowIf ? 84.0 : 180.0;                   // MIN_LOW_IF_LO / MIN_ZERO_IF_LO
    double khz = std::floor((adjusted + ifShift) * 1e-3 + 0.5);     // round(), :1370
    if (khz < floorKhz) { khz = floorKhz; }                         // MAX(), :1370
    ExpectedTune t;
    t.loKhz = static_cast<std::uint32_t>(khz);
    t.shiftHz = adjusted - khz * 1e3 + deltaHz;                     // :1418
    return t;
}

std::vector<cascade::usb::UsbDeviceInfo> oneFakeDevice(const std::string& serial) {
    cascade::usb::UsbDeviceInfo d;
    d.vid = 0x03EB;
    d.pid = 0x800C;
    d.path = "\\\\?\\usb#vid_03eb&pid_800c#fake#{a5dcbf10}";
    d.serial = serial;
    d.description = "Airspy HF+ Discovery";
    return {d};
}

// The serial string as the reference documents it: a twelve-character prefix
// and sixteen hex digits, twenty-eight characters (airspyhf.c:59, :73-75).
constexpr const char* kFakeSerialString = "AIRSPYHF SN:123456789ABCDEF0";
constexpr const char* kFakeSerialHex = "123456789abcdef0";

// Points `src` at a fresh fake and returns a borrowed pointer to it. The fake
// is handed to the source on the FIRST successful open only, so a second one
// fails cleanly (with a null device) instead of two owners fighting over one
// object.
FakeAirspyHfUsb* attachFake(AirspyHfSource& src,
                            std::vector<cascade::usb::UsbDeviceInfo> devices = {}) {
    if (devices.empty()) { devices = oneFakeDevice(kFakeSerialString); }
    auto owned = std::make_unique<FakeAirspyHfUsb>();
    FakeAirspyHfUsb* raw = owned.get();
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

// The magnitude of one frequency in a block, by direct summation. Used to
// measure an image: no FFT, no windowing, no bin alignment to argue about -
// the tone is generated at exactly this frequency, so this is the amplitude
// of that tone and of its mirror.
double toneMagnitude(const std::complex<float>* x, std::size_t n, double normFreq) {
    double sr = 0.0;
    double si = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double a = -2.0 * 3.14159265358979323846 * normFreq * static_cast<double>(i);
        const double c = std::cos(a);
        const double s = std::sin(a);
        sr += static_cast<double>(x[i].real()) * c - static_cast<double>(x[i].imag()) * s;
        si += static_cast<double>(x[i].real()) * s + static_cast<double>(x[i].imag()) * c;
    }
    return std::sqrt(sr * sr + si * si) / static_cast<double>(n);
}

}  // namespace

int main() {
    // =====================================================================
    // 1. THE PROTOCOL NUMBERS, against airspyhf_commands.h:33-58.
    // =====================================================================
    {
        CHECK(airspyhf::requestByte(airspyhf::VendorRequest::ReceiverMode) == 1);
        CHECK(airspyhf::requestByte(airspyhf::VendorRequest::SetFreq) == 2);
        CHECK(airspyhf::requestByte(airspyhf::VendorRequest::GetSampleRates) == 3);
        CHECK(airspyhf::requestByte(airspyhf::VendorRequest::SetSampleRate) == 4);
        CHECK(airspyhf::requestByte(airspyhf::VendorRequest::ConfigRead) == 5);
        CHECK(airspyhf::requestByte(airspyhf::VendorRequest::ConfigWrite) == 6);
        CHECK(airspyhf::requestByte(airspyhf::VendorRequest::GetSerialNoBoardId) == 7);
        CHECK(airspyhf::requestByte(airspyhf::VendorRequest::SetUserOutput) == 8);
        CHECK(airspyhf::requestByte(airspyhf::VendorRequest::GetVersionString) == 9);
        CHECK(airspyhf::requestByte(airspyhf::VendorRequest::SetAgc) == 10);
        CHECK(airspyhf::requestByte(airspyhf::VendorRequest::SetAgcThreshold) == 11);
        CHECK(airspyhf::requestByte(airspyhf::VendorRequest::SetAtt) == 12);
        CHECK(airspyhf::requestByte(airspyhf::VendorRequest::SetLna) == 13);
        CHECK(airspyhf::requestByte(airspyhf::VendorRequest::GetSampleRateArchitectures) == 14);
        CHECK(airspyhf::requestByte(airspyhf::VendorRequest::GetFilterGain) == 15);
        CHECK(airspyhf::requestByte(airspyhf::VendorRequest::GetFreqDelta) == 16);
        CHECK(airspyhf::requestByte(airspyhf::VendorRequest::SetVctcxoCalibration) == 17);
        CHECK(airspyhf::requestByte(airspyhf::VendorRequest::SetFrontendOptions) == 18);
        CHECK(airspyhf::requestByte(airspyhf::VendorRequest::GetAttSteps) == 19);
        CHECK(airspyhf::requestByte(airspyhf::VendorRequest::GetBiasTeeCount) == 20);
        CHECK(airspyhf::requestByte(airspyhf::VendorRequest::GetBiasTeeName) == 21);
        // AIRSPYHF_SET_BIAS_TEE = AIRSPYHF_CMD_MAX = 22.
        CHECK(airspyhf::requestByte(airspyhf::VendorRequest::SetBiasTee) == 22);

        // airspyhf.c:145-146, and airspyhf.h:36 for the endpoint.
        CHECK(airspyhf::kUsbVid == 0x03EB);
        CHECK(airspyhf::kUsbPid == 0x800C);
        CHECK(airspyhf::kRxEndpoint == 0x81);
        // airspyhf.c:55 - SAMPLES_TO_TRANSFER, four bytes a sample.
        CHECK(airspyhf::kSamplesPerTransfer == 4096);
        CHECK(airspyhf::kBytesPerSample == 4);
        CHECK(airspyhf::kTransferBufferBytes == 16384);
        CHECK(airspyhf::kTransferCount == 16);
        // airspyhf.c:80 - LIBUSB_CTRL_TIMEOUT_MS.
        CHECK(airspyhf::kControlTimeoutMs == 500);
        // receiver_mode_t (airspyhf_commands.h:29-33).
        CHECK(static_cast<int>(airspyhf::ReceiverMode::Off) == 0);
        CHECK(static_cast<int>(airspyhf::ReceiverMode::On) == 1);
    }

    // =====================================================================
    // 2. THE TUNING ARITHMETIC, twice over and then as wire bytes.
    // =====================================================================
    {
        // The delta the fake answers: exponent 0x10, mantissa 0x001234 low
        // byte first (airspyhf.c:1404) = 4660 * 1000 / 65536 Hz.
        const std::uint8_t deltaBytes[4] = {0x10, 0x34, 0x12, 0x00};
        const double delta = airspyhf::decodeFreqDelta(deltaBytes);
        CHECK_NEAR(delta, 4660.0 * 1000.0 / 65536.0, 1e-9);
        CHECK_NEAR(delta, 71.10595703125, 1e-9);

        // A NEGATIVE delta: buf[3] is signed, so 0xFF is -1 in the top byte.
        const std::uint8_t negBytes[4] = {0x10, 0x00, 0x00, 0xFF};
        CHECK_NEAR(airspyhf::decodeFreqDelta(negBytes), -1000.0, 1e-9);
        // Exponent 0 is a whole kilohertz per count.
        const std::uint8_t unityBytes[4] = {0x00, 0x02, 0x00, 0x00};
        CHECK_NEAR(airspyhf::decodeFreqDelta(unityBytes), 2000.0, 1e-9);

        struct TuneCase {
            double hz;
            int ppb;
            bool lowIf;
            const char* label;
        };
        const TuneCase cases[] = {
            {7.1e6, -1200, false, "7.1 MHz, zero-IF, -1200 ppb"},
            {14.2e6, -1200, false, "14.2 MHz, zero-IF, -1200 ppb"},
            {100.0e6, -1200, false, "100 MHz, zero-IF, -1200 ppb"},
            {7.1e6, 0, false, "7.1 MHz, zero-IF, no calibration"},
            {7.1e6, -1200, true, "7.1 MHz, low-IF, -1200 ppb"},
            {14.2e6, 25000, false, "14.2 MHz, zero-IF, +25000 ppb"},
            // Below the zero-IF LO floor: the LO clamps at 180 kHz and the
            // whole difference becomes the software shift, which is how this
            // radio reaches 9 kHz at all (airspyhf.c:1370).
            {9.0e3, 0, false, "9 kHz, zero-IF: the LO floor"},
            {60.0e3, 0, true, "60 kHz, low-IF: the lower floor"},
        };
        for (const TuneCase& c : cases) {
            const ExpectedTune want = expectedTuning(c.hz, c.ppb, c.lowIf, true, delta);
            const airspyhf::Tuning got = airspyhf::computeTuning(c.hz, c.ppb, c.lowIf, true, delta);
            if (got.loKhz != want.loKhz || std::fabs(got.shiftHz - want.shiftHz) > 1e-6) {
                std::printf("     %s: got LO %u kHz shift %.6f Hz, want %u kHz shift %.6f Hz\n",
                            c.label, got.loKhz, got.shiftHz, want.loKhz, want.shiftHz);
            }
            CHECK(got.loKhz == want.loKhz);
            CHECK_NEAR(got.shiftHz, want.shiftHz, 1e-6);
        }

        // The three the driver is driven to below, worked out by hand so the
        // two implementations above cannot both be wrong in the same way.
        // -1200 ppb scales 7.1 MHz down by 8.52 Hz, the zero-IF offset adds 5
        // kHz, and the result rounds to a whole kilohertz:
        //   7 099 991.48 + 5 000 = 7 104 991.48 -> 7105 kHz
        //  14 199 982.96 + 5 000 = 14 204 982.96 -> 14205 kHz
        //  99 999 880.00 + 5 000 = 100 004 880.00 -> 100005 kHz
        CHECK(airspyhf::computeTuning(7.1e6, -1200, false, true, 0.0).loKhz == 7105);
        CHECK(airspyhf::computeTuning(14.2e6, -1200, false, true, 0.0).loKhz == 14205);
        CHECK(airspyhf::computeTuning(100.0e6, -1200, false, true, 0.0).loKhz == 100005);
        // Without the host DSP there is no IF offset at all (airspyhf.c:1367).
        CHECK(airspyhf::computeTuning(7.1e6, -1200, false, false, 0.0).loKhz == 7100);
        // ...and a low-IF rate never carries one either.
        CHECK(airspyhf::computeTuning(14.2e6, -1200, true, true, 0.0).loKhz == 14200);
        // The floors.
        CHECK(airspyhf::computeTuning(9.0e3, 0, false, true, 0.0).loKhz == 180);
        CHECK(airspyhf::computeTuning(9.0e3, 0, true, true, 0.0).loKhz == 84);

        // SET_FREQ's payload is BIG-endian, which is the one place this
        // protocol disagrees with every other radio here (airspyhf.c:1373).
        std::uint8_t payload[airspyhf::kFreqPayloadBytes];
        airspyhf::encodeFreqKhz(7105, payload);
        CHECK(sameBytes("encodeFreqKhz(7105)",
                        std::vector<std::uint8_t>(payload, payload + sizeof(payload)),
                        {0x00, 0x00, 0x1B, 0xC1}));
        airspyhf::encodeFreqKhz(100005, payload);
        CHECK(sameBytes("encodeFreqKhz(100005)",
                        std::vector<std::uint8_t>(payload, payload + sizeof(payload)),
                        {0x00, 0x01, 0x86, 0xA5}));
        airspyhf::encodeFreqKhz(airspyhf::kMinZeroIfLoKhz, payload);
        CHECK(sameBytes("encodeFreqKhz(180)",
                        std::vector<std::uint8_t>(payload, payload + sizeof(payload)),
                        {0x00, 0x00, 0x00, 0xB4}));

        // The two bands the manufacturer publishes, and the gap between them.
        CHECK(airspyhf::frequencyCovered(9.0e3));
        CHECK(airspyhf::frequencyCovered(7.1e6));
        CHECK(airspyhf::frequencyCovered(31.0e6));
        CHECK(!airspyhf::frequencyCovered(45.0e6));
        CHECK(airspyhf::frequencyCovered(64.0e6));
        CHECK(airspyhf::frequencyCovered(260.0e6));
        CHECK(!airspyhf::frequencyCovered(300.0e6));
        CHECK(!airspyhf::frequencyCovered(1.0e3));
    }

    // =====================================================================
    // 3. THE SAMPLE FORMAT, THE FILTER GAIN AND THE ATTENUATOR TABLE.
    // =====================================================================
    {
        // airspyhf.c:1273 - filter_gain = 10^(gain * -0.05).
        CHECK_NEAR(airspyhf::filterGainFromDb(0), 1.0, 1e-6);
        CHECK_NEAR(airspyhf::filterGainFromDb(6), 0.5011872336, 1e-6);
        CHECK_NEAR(airspyhf::filterGainFromDb(20), 0.1, 1e-6);

        // airspyhf.c:84-87: THE IMAGINARY PART IS FIRST in the wire struct,
        // and both halves are little-endian int16.
        //   sample 0: im = +1 (0x0001), re = -2 (0xFFFE)
        //   sample 1: im = -32768 (0x8000), re = +32767 (0x7FFF)
        const std::uint8_t raw[8] = {0x01, 0x00, 0xFE, 0xFF, 0x00, 0x80, 0xFF, 0x7F};
        std::complex<float> out[2];
        airspyhf::decodeSamples(raw, 2, 1.0f, out);
        CHECK_NEAR(out[0].real(), -2.0f / 32768.0f, 1e-9);
        CHECK_NEAR(out[0].imag(), 1.0f / 32768.0f, 1e-9);
        CHECK_NEAR(out[1].real(), 32767.0f / 32768.0f, 1e-9);
        CHECK_NEAR(out[1].imag(), -1.0f, 1e-9);
        // ...and the filter gain multiplies both parts.
        airspyhf::decodeSamples(raw, 2, 0.5f, out);
        CHECK_NEAR(out[1].real(), 0.5f * 32767.0f / 32768.0f, 1e-9);

        // airspyhf_set_att (airspyhf.c:1674-1690): the FIRST step at or above
        // the request. Our clamp replaces the reference's silent fall back to
        // index 0 for an over-range request (see airspyhf_protocol.hpp).
        const std::vector<float> steps = airspyhf::defaultAttSteps();
        CHECK(steps.size() == 9);
        CHECK_NEAR(steps[8], 48.0f, 1e-9);
        CHECK(airspyhf::attIndexFor(steps, 0.0) == 0);
        CHECK(airspyhf::attIndexFor(steps, 1.0) == 1);
        CHECK(airspyhf::attIndexFor(steps, 6.0) == 1);
        CHECK(airspyhf::attIndexFor(steps, 6.5) == 2);
        CHECK(airspyhf::attIndexFor(steps, 48.0) == 8);
        CHECK(airspyhf::attIndexFor(steps, 100.0) == 8);  // clamped, not index 0

        // Nearest-rate coercion, ties to the lower index.
        const std::vector<std::uint32_t> rates = {912000, 768000, 456000, 384000, 256000, 192000};
        CHECK(airspyhf::nearestRateIndex(rates, 912000.0) == 0);
        CHECK(airspyhf::nearestRateIndex(rates, 500000.0) == 2);
        CHECK(airspyhf::nearestRateIndex(rates, 100.0) == 5);
        CHECK(airspyhf::nearestRateIndex(rates, 5.0e6) == 0);
    }

    // =====================================================================
    // 4. THE FINE-TUNING ROTATION, including the 0.5% the reference costs.
    // =====================================================================
    {
        // A shift of a quarter of the sample rate is a rotation by -90
        // degrees a sample. The reference ROTATES BEFORE IT MULTIPLIES
        // (airspyhf.c:352-356), so sample 0 is already turned by one step -
        // an off-by-one that would be invisible on a spectrum and wrong by a
        // fixed phase on anything coherent.
        std::vector<std::complex<float>> x(4, std::complex<float>(1.0f, 0.0f));
        airspyhf::Rotator vec;
        airspyhf::rotateBlock(x.data(), x.size(), 0.25, 1.0, vec);
        // e^-j90 = -j, and the reference's first-order re-normaliser
        // (norm = 1.99 - |v|^2) leaves |v| at 0.99 after the first step.
        // Step two turns it to -1 and re-normalises the other way: |v| was
        // 0.99, so norm is 1.99 - 0.9801 = 1.0099 and |v| becomes 0.999801.
        CHECK_NEAR(x[0].real(), 0.0, 1e-5);
        CHECK_NEAR(x[0].imag(), -0.99, 1e-5);
        CHECK_NEAR(x[1].real(), -0.99 * 1.0099, 1e-5);
        CHECK_NEAR(x[1].imag(), 0.0, 1e-5);

        // THE 1.99 IS NOT A TYPO FOR 2.0 AND IT IS NOT A NORMALISER TO UNITY.
        // v * (1.99 - |v|^2) has its fixed point at |v| = sqrt(0.99) =
        // 0.994987, so the reference's rotation costs a permanent 0.0436 dB
        // of amplitude. Transcribed rather than "fixed", and pinned here so
        // that a later tidy-up to 2.0 - which would make the rotation
        // marginally unstable instead - shows up as a failing test rather
        // than as a drift nobody can attribute.
        std::vector<std::complex<float>> y(4096, std::complex<float>(1.0f, 0.0f));
        airspyhf::Rotator settle;
        airspyhf::rotateBlock(y.data(), y.size(), 0.01, 1.0, settle);
        const double settled = std::sqrt(static_cast<double>(settle.re) * settle.re +
                                         static_cast<double>(settle.im) * settle.im);
        CHECK_NEAR(settled, std::sqrt(0.99), 1e-4);

        // A zero shift leaves the block alone and does not advance the vector.
        std::vector<std::complex<float>> z(4, std::complex<float>(0.25f, -0.5f));
        airspyhf::Rotator idle;
        airspyhf::rotateBlock(z.data(), z.size(), 0.0, 768000.0, idle);
        CHECK_NEAR(z[3].real(), 0.25, 1e-9);
        CHECK_NEAR(z[3].imag(), -0.5, 1e-9);
        CHECK_NEAR(idle.re, 1.0, 1e-9);
    }

    // =====================================================================
    // 5. ENUMERATION: which devices are ours, and what reopens each one.
    // =====================================================================
    {
        std::vector<cascade::usb::UsbDeviceInfo> devices;
        cascade::usb::UsbDeviceInfo disco;
        disco.vid = 0x03EB;
        disco.pid = 0x800C;
        disco.serial = kFakeSerialString;
        disco.description = "Airspy HF+ Discovery";
        disco.path = "p1";
        devices.push_back(disco);
        cascade::usb::UsbDeviceInfo plain;
        plain.vid = 0x03EB;
        plain.pid = 0x800C;
        plain.serial.clear();  // a device with no serial is still openable
        plain.description.clear();
        plain.path = "p2";
        devices.push_back(plain);
        cascade::usb::UsbDeviceInfo stranger;
        stranger.vid = 0x0BDA;  // an RTL-SDR: not ours, and must not be opened
        stranger.pid = 0x2838;
        stranger.path = "p3";
        devices.push_back(stranger);

        const std::vector<NativeDeviceInfo> found = cascade::source::airspyHfDevicesFrom(devices);
        CHECK(found.size() == 2);
        const NativeDeviceInfo absent;
        const NativeDeviceInfo& f0 = found.size() > 0 ? found[0] : absent;
        const NativeDeviceInfo& f1 = found.size() > 1 ? found[1] : absent;
        CHECK(f0.driver == "airspyhf");
        // The model comes off the BUS, because an HF+ Dual and an HF+
        // Discovery are the same VID/PID and the only other way to ask would
        // be to open the device.
        CHECK(f0.label == std::string("Airspy HF+ Discovery (serial ") + kFakeSerialHex + ")");
        CHECK(f0.args == std::string("serial=") + kFakeSerialHex);
        // A device whose description says nothing gets the family name, and
        // no serial means an index.
        CHECK(f1.label == "Airspy HF+");
        CHECK(f1.args == "index=1");

        // One VID/PID for the whole family (airspyhf.c:145-146).
        CHECK(cascade::source::airspyHfUsbIds().size() == 1);

        // The serial normaliser: the prefix off, lower-cased, and anything
        // that does not carry it left alone but for the case.
        CHECK(cascade::source::normalisedSerial(kFakeSerialString) == kFakeSerialHex);
        CHECK(cascade::source::normalisedSerial("123456789ABCDEF0") == kFakeSerialHex);
        CHECK(cascade::source::normalisedSerial("") == "");
    }

    // =====================================================================
    // 6. OPEN: the whole opening sequence, byte for byte.
    // =====================================================================
    {
        AirspyHfSource src;
        FakeAirspyHfUsb* fake = attachFake(src);
        const bool opened = src.open(std::string("serial=") + kFakeSerialHex);
        if (!opened) { std::printf("     open() said: %s\n", src.lastError()); }
        CHECK(opened);
        CHECK(src.isOpen());
        CHECK(!src.faulted());
        CHECK(std::string(src.name()) ==
              std::string("Airspy HF+ Discovery (serial ") + kFakeSerialHex + ")");

        // The identity the driver read back. airspyhf_info.c:47-50 prints the
        // serial as "0x%08X%08X" of serial_no[0] then serial_no[1], which are
        // words 1 and 2 of the answer struct.
        CHECK(src.partId() == 0x6ba00477u);
        CHECK(src.serialNo() == "123456789abcdef0");
        CHECK(src.firmwareVersion() == "R3.0.7-CD");
        // ...and what it learned it could do.
        CHECK(src.supportedSampleRatesHz().size() == 6);
        CHECK(src.rateArchitectures().size() == 6);
        CHECK(src.attenuatorStepsDb().size() == 9);
        CHECK(src.biasTeeSupported());
        CHECK(src.calibrationPpb() == -1200);

        const std::vector<AirspyHfControlRecord> c = fake->controls();
        if (c.size() != 21) {
            std::printf("     open() sent %zu control transfers, expected 21\n", c.size());
            for (std::size_t i = 0; i < c.size(); ++i) {
                std::printf("       [%zu] %s req %u value 0x%04x index 0x%04x len %zu data [%s]\n",
                            i, c[i].in ? "IN " : "OUT", static_cast<unsigned>(c[i].request),
                            static_cast<unsigned>(c[i].value), static_cast<unsigned>(c[i].index),
                            c[i].requestedLen, hexOf(c[i].data).c_str());
            }
        }
        CHECK(c.size() == 21);

        // airspyhf_board_partid_serialno_read (airspyhf.c:1598-1620) and
        // airspyhf_version_string_read (:1622-1654), in airspyhf_info.c's own
        // order, both IN with value and index 0.
        CHECK(isControl("open[0] part id / serial", at(c, 0), true, 7, 0, 0));
        CHECK(at(c, 0).requestedLen == 20);
        CHECK(isControl("open[1] version string", at(c, 1), true, 9, 0, 0));
        CHECK(at(c, 1).requestedLen == 63);

        // airspyhf_read_samplerates_from_fw (airspyhf.c:585-604): the COUNT
        // is asked for with index 0 and four bytes, the LIST with the count
        // in the index word and four bytes per rate.
        CHECK(isControl("open[2] rate count", at(c, 2), true, 3, 0, 0));
        CHECK(at(c, 2).requestedLen == 4);
        CHECK(isControl("open[3] rate list", at(c, 3), true, 3, 0, 6));
        CHECK(at(c, 3).requestedLen == 24);
        // airspyhf_read_samplerate_architectures_from_fw (:627-648) asks for
        // count * 4 bytes even though the device answers count - the request
        // is sent exactly as the reference sends it.
        CHECK(isControl("open[4] rate architectures", at(c, 4), true, 14, 0, 6));
        CHECK(at(c, 4).requestedLen == 24);
        CHECK(at(c, 4).data.size() == 6);

        // airspyhf_read_att_steps_from_fw (:606-625), the same shape.
        CHECK(isControl("open[5] att step count", at(c, 5), true, 19, 0, 0));
        CHECK(at(c, 5).requestedLen == 4);
        CHECK(isControl("open[6] att steps", at(c, 6), true, 19, 0, 9));
        CHECK(at(c, 6).requestedLen == 36);

        // airspyhf_config_read (:1483-1506) - a fixed 256-byte page - and the
        // two values the reference pushes back into the device from it
        // (:1079-1082).
        CHECK(isControl("open[7] config read", at(c, 7), true, 5, 0, 0));
        CHECK(at(c, 7).requestedLen == 256);
        CHECK(isControl("open[8] vctcxo trim", at(c, 8), false, 17, 0x1234, 0));
        // airspyhf_set_frontend_options (:1433-1454): the low half in VALUE,
        // the high half in INDEX.
        CHECK(isControl("open[9] frontend options", at(c, 9), false, 18, 0x0002, 0x0000));
        CHECK(isControl("open[10] bias tee count", at(c, 10), true, 20, 0, 0));

        // airspyhf_set_samplerate (:1194-1281): the LO is moved to the
        // zero-IF floor first because it is below it, then the rate index,
        // then the filter gain for that rate.
        CHECK(isControl("open[11] LO to the floor", at(c, 11), false, 2, 0, 0));
        CHECK(sameBytes("open[11] LO 180 kHz", at(c, 11).data, {0x00, 0x00, 0x00, 0xB4}));
        CHECK(isControl("open[12] sample rate index 0", at(c, 12), false, 4, 0, 0));
        CHECK(isControl("open[13] filter gain", at(c, 13), true, 15, 0, 0));
        // ...and the retune that airspyhf_set_samplerate ends with (:1280).
        // 10 MHz at -1200 ppb with the 5 kHz zero-IF offset is 10005 kHz.
        CHECK(isControl("open[14] tune", at(c, 14), false, 2, 0, 0));
        CHECK(sameBytes("open[14] LO 10005 kHz", at(c, 14).data, {0x00, 0x00, 0x27, 0x15}));
        CHECK(isControl("open[15] freq delta", at(c, 15), true, 16, 0, 0));

        // A KNOWN STATE: no attenuation, preamp off, AGC off and low, bias
        // tee off.
        CHECK(isControl("open[16] attenuator 0", at(c, 16), false, 12, 0, 0));
        CHECK(isControl("open[17] preamp off", at(c, 17), false, 13, 0, 0));
        CHECK(isControl("open[18] AGC off", at(c, 18), false, 10, 0, 0));
        CHECK(isControl("open[19] AGC threshold low", at(c, 19), false, 11, 0, 0));
        CHECK(isControl("open[20] bias tee off", at(c, 20), false, 22, 0, 0));

        // The pipe is cleared across the rate change (libusb_clear_halt,
        // :1218) - not a control transfer, so it is counted separately.
        CHECK(fake->resetPipeCalls() == 1);

        CHECK_NEAR(src.sampleRateHz(), 912000.0, 0.5);
        CHECK_NEAR(src.centerFrequencyHz(), 10.0e6, 0.5);
        CHECK(!src.isLowIf());  // rate index 0 is zero-IF in the fake's answer

        // The residual the reader rotates out, computed here from the
        // reference's formula rather than read back out of the driver's own.
        {
            const std::uint8_t deltaBytes[4] = {0x10, 0x34, 0x12, 0x00};
            const double delta = airspyhf::decodeFreqDelta(deltaBytes);
            const ExpectedTune want = expectedTuning(10.0e6, -1200, false, true, delta);
            CHECK_NEAR(src.frequencyDeltaHz(), delta, 1e-9);
            CHECK_NEAR(src.frequencyShiftHz(), want.shiftHz, 1e-6);
            // ...and its actual value, so a formula that is wrong in both
            // places still fails: 9 999 988 - 10 005 000 + 71.106 Hz.
            CHECK_NEAR(src.frequencyShiftHz(), -4940.894043, 1e-4);
        }

        CHECK_NEAR(src.gainDb("ATT"), 0.0, 1e-9);
        CHECK_NEAR(src.gainDb("LNA"), 0.0, 1e-9);
        CHECK(!src.autoGain());
        CHECK(!src.agcThresholdHigh());
        CHECK(!src.biasT());
        CHECK(src.autoGainSupported());
        CHECK(src.antennas().size() == 1);
        CHECK(src.setAntenna("RX"));
        CHECK(!src.setAntenna("TX"));
        double lo = 0.0, hi = 0.0;
        CHECK(src.frequencyRangeHz(lo, hi));
        CHECK_NEAR(lo, 9.0e3, 0.5);
        CHECK_NEAR(hi, 260.0e6, 0.5);
        CHECK(std::string(src.driverKey()) == "airspyhf");
        CHECK(src.selfPaced());

        // The gains, as the panel will see them: the attenuator turned over
        // into a negative gain so that right is louder like every other
        // control, and the preamp as a two-position 6 dB step.
        const std::vector<cascade::source::GainInfo> g = src.gains();
        CHECK(g.size() == 2);
        if (g.size() == 2) {
            CHECK(g[0].name == "ATT");
            CHECK_NEAR(g[0].minDb, -48.0, 1e-9);
            CHECK_NEAR(g[0].maxDb, 0.0, 1e-9);
            CHECK_NEAR(g[0].stepDb, 6.0, 1e-9);
            CHECK(g[1].name == "LNA");
            CHECK_NEAR(g[1].maxDb, 6.0, 1e-9);
        }
        src.closeDevice();
        CHECK(!src.isOpen());
    }

    // =====================================================================
    // 7. RATES AND TUNES, each as the exact transfer it becomes.
    // =====================================================================
    {
        AirspyHfSource src;
        FakeAirspyHfUsb* fake = attachFake(src);
        CHECK(src.open(""));  // no args: the first device
        const std::uint8_t deltaBytes[4] = {0x10, 0x34, 0x12, 0x00};
        const double delta = airspyhf::decodeFreqDelta(deltaBytes);

        // --- tuning ------------------------------------------------------
        struct TuneCase {
            double hz;
            std::vector<int> bytes;
            const char* label;
        };
        const TuneCase tunes[] = {
            {7.1e6, {0x00, 0x00, 0x1B, 0xC1}, "tune 7.1 MHz"},      // 7105 kHz
            {14.2e6, {0x00, 0x00, 0x37, 0x7D}, "tune 14.2 MHz"},    // 14205 kHz
            {100.0e6, {0x00, 0x01, 0x86, 0xA5}, "tune 100 MHz"},    // 100005 kHz
        };
        for (const TuneCase& t : tunes) {
            fake->clearControls();
            const bool ok = src.setCenterFrequencyHz(t.hz);
            if (!ok) { std::printf("     %s said: %s\n", t.label, src.lastError()); }
            CHECK(ok);
            const std::vector<AirspyHfControlRecord> c = fake->controls();
            CHECK(c.size() == 2);
            CHECK(isControl(t.label, at(c, 0), false, 2, 0, 0));
            CHECK(sameBytes(t.label, at(c, 0).data, t.bytes));
            // Every SET_FREQ is followed by GET_FREQ_DELTA, because the
            // residual is half the answer (airspyhf.c:1394-1402).
            CHECK(isControl("freq delta", at(c, 1), true, 16, 0, 0));
            CHECK_NEAR(src.centerFrequencyHz(), t.hz, 0.5);
            const ExpectedTune want = expectedTuning(t.hz, -1200, false, true, delta);
            CHECK_NEAR(src.frequencyShiftHz(), want.shiftHz, 1e-6);
            // The residual is always within half a kilohertz of the IF
            // offset: the LO lands on a kHz and the rest is done in software.
            CHECK(std::fabs(src.frequencyShiftHz() + 5000.0) < 600.0);
        }

        // Tuning to where it already is sends NOTHING - the reference's own
        // `if (device->freq_khz != freq_khz)` guard (airspyhf.c:1372).
        fake->clearControls();
        CHECK(src.setCenterFrequencyHz(100.0e6));
        CHECK(fake->controlCount() == 0);

        // Outside the two bands is refused, and nothing is sent.
        fake->clearControls();
        CHECK(!src.setCenterFrequencyHz(45.0e6));
        CHECK(!src.setCenterFrequencyHz(1.0e3));
        CHECK(!src.setCenterFrequencyHz(300.0e6));
        CHECK(fake->controlCount() == 0);
        CHECK(!src.faulted());  // a refusal is not a fault
        CHECK(std::string(src.lastError()).find("neither band") != std::string::npos);
        CHECK_NEAR(src.centerFrequencyHz(), 100.0e6, 0.5);

        // --- the calibration -----------------------------------------------
        // Changing the ppb re-tunes, because every tune is scaled by it
        // (airspyhf.c:1519-1523). At +10000 ppb, 100 MHz becomes 100.001 MHz
        // before the 5 kHz offset: 100 001 000 + 5 000 = 100006 kHz.
        fake->clearControls();
        CHECK(src.setCalibrationPpb(10000));
        CHECK(src.calibrationPpb() == 10000);
        {
            const std::vector<AirspyHfControlRecord> c = fake->controls();
            CHECK(c.size() == 2);
            CHECK(sameBytes("calibrated tune", at(c, 0).data, {0x00, 0x01, 0x86, 0xA6}));
        }
        CHECK(src.setCalibrationPpb(-1200));

        // --- rates ---------------------------------------------------------
        fake->clearControls();
        CHECK(src.setCenterFrequencyHz(14.2e6));
        fake->clearControls();

        // Index 2 (456 kS/s) is LOW-IF in the fake's architecture answer, so
        // there is no LO pre-tune and no 5 kHz offset: 14 199 982.96 rounds
        // to 14200 kHz instead of 14205.
        CHECK(src.setSampleRateHz(456000.0));
        CHECK_NEAR(src.sampleRateHz(), 456000.0, 0.5);
        CHECK(src.isLowIf());
        {
            const std::vector<AirspyHfControlRecord> c = fake->controls();
            CHECK(c.size() == 4);
            CHECK(isControl("rate: index 2", at(c, 0), false, 4, 0, 2));
            CHECK(isControl("rate: filter gain", at(c, 1), true, 15, 0, 0));
            CHECK(isControl("rate: retune", at(c, 2), false, 2, 0, 0));
            CHECK(sameBytes("rate: low-IF has no 5 kHz offset", at(c, 2).data,
                            {0x00, 0x00, 0x37, 0x78}));  // 14200
            CHECK(isControl("rate: freq delta", at(c, 3), true, 16, 0, 0));
        }

        // ...and back to a zero-IF rate, where the offset comes back.
        fake->clearControls();
        CHECK(src.setSampleRateHz(912000.0));
        CHECK(!src.isLowIf());
        {
            const std::vector<AirspyHfControlRecord> c = fake->controls();
            CHECK(c.size() == 4);
            CHECK(isControl("rate: index 0", at(c, 0), false, 4, 0, 0));
            CHECK(sameBytes("rate: zero-IF offset is back", at(c, 2).data,
                            {0x00, 0x00, 0x37, 0x7D}));  // 14205
        }

        // A rate the device does not have is COERCED to the nearest it does,
        // and the caller is told which (the firmware takes an index; there is
        // nothing else to give it).
        fake->clearControls();
        CHECK(src.setSampleRateHz(500000.0));
        CHECK_NEAR(src.sampleRateHz(), 456000.0, 0.5);
        CHECK(std::string(src.lastError()).find("nearest rate") != std::string::npos);
        CHECK(!src.faulted());

        // A nonsense rate is refused before anything is sent.
        fake->clearControls();
        CHECK(!src.setSampleRateHz(0.0));
        CHECK(!src.setSampleRateHz(-1.0));
        CHECK(fake->controlCount() == 0);

        // The panel's list is ASCENDING even though the firmware's is not,
        // and the index the rate is programmed by is the firmware's.
        const std::vector<double> menu = src.supportedSampleRatesHz();
        CHECK(menu.size() == 6);
        if (menu.size() == 6) {
            CHECK_NEAR(menu[0], 192000.0, 0.5);
            CHECK_NEAR(menu[5], 912000.0, 0.5);
        }

        // --- gains -------------------------------------------------------
        fake->clearControls();
        CHECK(src.setGainDb("ATT", -18.0));
        CHECK(src.setGainDb("LNA", 6.0));
        CHECK(src.setAutoGain(true));
        CHECK(src.setAgcThresholdHigh(true));
        CHECK(src.setBiasT(true));
        {
            const std::vector<AirspyHfControlRecord> c = fake->controls();
            CHECK(c.size() == 5);
            // airspyhf_set_hf_att (:1789): the INDEX INTO THE STEP TABLE
            // rides in the VALUE word - 18 dB is step 3.
            CHECK(isControl("ATT -18 dB is step 3", at(c, 0), false, 12, 3, 0));
            CHECK(isControl("preamp on", at(c, 1), false, 13, 1, 0));
            CHECK(isControl("AGC on", at(c, 2), false, 10, 1, 0));
            CHECK(isControl("AGC threshold high", at(c, 3), false, 11, 1, 0));
            CHECK(isControl("bias tee on", at(c, 4), false, 22, 1, 0));
        }
        CHECK_NEAR(src.gainDb("ATT"), -18.0, 1e-9);
        CHECK_NEAR(src.gainDb("LNA"), 6.0, 1e-9);
        CHECK(src.autoGain());
        CHECK(src.agcThresholdHigh());
        CHECK(src.biasT());

        // Out of range is CLAMPED and rounded to a step the hardware has, and
        // the readback says what was actually programmed
        // (device_source.hpp's contract, not the reference's refusal).
        fake->clearControls();
        CHECK(src.setGainDb("ATT", -999.0));
        CHECK_NEAR(src.gainDb("ATT"), -48.0, 1e-9);
        CHECK(src.setGainDb("ATT", 50.0));  // a POSITIVE gain: there is none
        CHECK_NEAR(src.gainDb("ATT"), 0.0, 1e-9);
        // A value between steps takes the next step UP in attenuation
        // (airspyhf.c:1680 - the first step >= the request).
        CHECK(src.setGainDb("ATT", -7.0));
        CHECK_NEAR(src.gainDb("ATT"), -12.0, 1e-9);
        {
            const std::vector<AirspyHfControlRecord> c = fake->controls();
            CHECK(c.size() == 3);
            CHECK(isControl("ATT clamped to step 8", at(c, 0), false, 12, 8, 0));
            CHECK(isControl("ATT positive means none", at(c, 1), false, 12, 0, 0));
            CHECK(isControl("ATT -7 rounds to step 2", at(c, 2), false, 12, 2, 0));
        }
        // The preamp is a switch: half its step is the boundary.
        CHECK(src.setGainDb("LNA", 2.0));
        CHECK_NEAR(src.gainDb("LNA"), 0.0, 1e-9);
        CHECK(src.setGainDb("LNA", 4.0));
        CHECK_NEAR(src.gainDb("LNA"), 6.0, 1e-9);

        // An unknown gain name is a refusal, not a transfer.
        fake->clearControls();
        CHECK(!src.setGainDb("VGA", 20.0));
        CHECK(fake->controlCount() == 0);

        src.closeDevice();
    }

    // =====================================================================
    // 8. START AND STOP: the receiver, and the bulk ring around it.
    // =====================================================================
    {
        AirspyHfSource src;
        FakeAirspyHfUsb* fake = attachFake(src);
        CHECK(src.open(""));
        fake->clearControls();
        const int resetsBefore = fake->resetPipeCalls();

        CHECK(src.start());
        CHECK(src.running());
        // airspyhf_start (airspyhf.c:1308-1334): RECEIVER_MODE off, the pipe
        // cleared, then RECEIVER_MODE on. The mode is the VALUE word.
        {
            const std::vector<AirspyHfControlRecord> c = fake->controls();
            CHECK(c.size() == 2);
            CHECK(isControl("start: receiver off first", at(c, 0), false, 1, 0, 0));
            CHECK(isControl("start: receiver on", at(c, 1), false, 1, 1, 0));
        }
        CHECK(fake->resetPipeCalls() == resetsBefore + 1);
        // The ring is queued BEFORE the receiver is told to start, on the RX
        // endpoint, with libairspyhf's own transfer geometry.
        CHECK(fake->beginBulkCalls() == 1);
        CHECK(fake->lastBulkEndpoint() == 0x81);
        CHECK(fake->lastBulkBufferBytes() == 16384);
        CHECK(fake->lastBulkBufferCount() == 16);

        CHECK(src.start());  // idempotent
        CHECK(fake->beginBulkCalls() == 1);

        fake->clearControls();
        src.stop();
        CHECK(!src.running());
        {
            const std::vector<AirspyHfControlRecord> c = fake->controls();
            CHECK(c.size() == 1);
            CHECK(isControl("stop: receiver off", at(c, 0), false, 1, 0, 0));
        }
        CHECK(fake->endBulkCalls() >= 1);

        // A rate change on a RUNNING stream happens with the radio quiet, and
        // running() is true either side of it.
        CHECK(src.start());
        fake->clearControls();
        const int beginsBefore = fake->beginBulkCalls();
        CHECK(src.setSampleRateHz(768000.0));
        CHECK(src.running());
        {
            const std::vector<AirspyHfControlRecord> c = fake->controls();
            // 10 MHz is unchanged and index 1 is zero-IF like index 0, so the
            // retune at the end of the rate change computes the same 10005
            // kHz the device is already on and sends NOTHING - which is why
            // there are five transfers here and not seven.
            CHECK(c.size() == 5);
            CHECK(isControl("rate change: receiver off first", at(c, 0), false, 1, 0, 0));
            CHECK(isControl("rate change: index 1", at(c, 1), false, 4, 0, 1));
            CHECK(isControl("rate change: filter gain", at(c, 2), true, 15, 0, 0));
            CHECK(isControl("rate change: receiver off again", at(c, 3), false, 1, 0, 0));
            CHECK(isControl("rate change: receiver on", at(c, 4), false, 1, 1, 0));
        }
        CHECK(fake->beginBulkCalls() == beginsBefore + 1);
        src.closeDevice();
    }

    // =====================================================================
    // 9. STREAMING: three scripted buffers, every sample once and in order.
    // =====================================================================
    {
        AirspyHfSource src;
        FakeAirspyHfUsb* fake = attachFake(src);
        CHECK(src.open(""));
        // The host DSP off, so what comes out is the device's own samples
        // scaled by the filter gain and nothing else - airspyhf_set_lib_dsp(0)
        // (airspyhf.c:1592-1596). With it on, the balancer and the rotation
        // are both doing their job to these samples and there is no byte-exact
        // answer to compare against; block 10 measures those separately.
        CHECK(src.setHostDspEnabled(false));
        CHECK(!src.hostDspEnabled());

        // A ramp across all three buffers, so a lost buffer, a repeated one or
        // a swapped pair all show up as a mismatch at a named index rather
        // than as a count that happens to come out right.
        constexpr std::size_t kBuffers = 3;
        constexpr std::size_t kBytesPerBuffer = 64;
        std::vector<std::uint8_t> allBytes;
        for (std::size_t b = 0; b < kBuffers; ++b) {
            std::vector<std::uint8_t> buf(kBytesPerBuffer);
            for (std::size_t i = 0; i < kBytesPerBuffer; ++i) {
                buf[i] = static_cast<std::uint8_t>((b * kBytesPerBuffer + i) * 7 + 3);
            }
            allBytes.insert(allBytes.end(), buf.begin(), buf.end());
            fake->queueBulk(std::move(buf));
        }
        const std::size_t wantSamples = allBytes.size() / airspyhf::kBytesPerSample;

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

        // The expected floats, decoded here from the same bytes: imaginary
        // part first, little-endian int16, times 1/32768 times the filter
        // gain the fake reported (6 dB -> 0.5011872).
        std::vector<std::complex<float>> want(wantSamples);
        airspyhf::decodeSamples(allBytes.data(), wantSamples, airspyhf::filterGainFromDb(6),
                                want.data());
        std::size_t firstBad = wantSamples;
        for (std::size_t i = 0; i < wantSamples && i < got.size(); ++i) {
            if (got[i].real() != want[i].real() || got[i].imag() != want[i].imag()) {
                firstBad = i;
                std::printf("     streaming: sample %zu is (%.9f, %.9f), expected (%.9f, %.9f)\n",
                            i, got[i].real(), got[i].imag(), want[i].real(), want[i].imag());
                break;
            }
        }
        CHECK(firstBad == wantSamples);

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
    // 10. THE IQ BALANCER, measured against an imbalanced tone.
    // =====================================================================
    //
    // WHY THIS BLOCK EXISTS AT ALL. Everything else here proves bytes; this
    // proves the one thing on this radio that is not a byte. A zero-IF front
    // end puts a MIRROR of every signal on the other side of the centre, and
    // the reference rejects it in software. A port of that estimator can be
    // wrong in ways nothing else in this file would notice - a sign, a bin
    // index, a missing window - and the symptom on air would be a spectrum
    // full of images the manufacturer's own software does not show.
    //
    // So: a clean tone is DAMAGED by the exact inverse of the correction the
    // balancer applies (airspyhf iqbalancer.c:461-485), which puts a
    // measurable image on the mirror frequency, and the balancer is fed block
    // after block until its estimator has run. The image is then measured
    // again. The numbers are printed either way, because a threshold with no
    // measurement behind it is a threshold nobody can re-derive.
    {
        constexpr std::size_t kBlock = 4096;   // one transfer's worth
        constexpr int kBlocks = 700;           // enough for ~4 estimator rounds
        const double toneFreq = 0.15;          // cycles per sample, off-centre
        const double amp = 0.5;
        // The imbalance a poor zero-IF front end has: about 2% of amplitude
        // error and 2% of phase error, which is roughly 34 dB of image
        // rejection - bad, and entirely ordinary.
        const float damagePhase = 0.02f;
        const float damageAmplitude = 0.02f;

        // A NOISE FLOOR, because a mathematically pure tone is not what this
        // estimator was written for and a test that only works on one is
        // testing a case no antenna produces. Deterministic - a fixed linear
        // congruential sequence, seeded once - so the measurement below is
        // reproducible run to run and a failure is a real failure rather than
        // a draw. At 0.002 rms it is 48 dB below the tone and 17 dB below the
        // image, so it cannot be what either measurement is reading.
        std::uint32_t rng = 0x13572468u;
        const auto noise = [&rng]() {
            rng = rng * 1664525u + 1013904223u;
            return (static_cast<float>(rng >> 8) / static_cast<float>(1u << 24) - 0.5f) * 0.0069f;
        };

        const auto makeBlock = [&](std::size_t startSample, std::vector<std::complex<float>>& out) {
            out.resize(kBlock);
            for (std::size_t i = 0; i < kBlock; ++i) {
                const double a = 2.0 * 3.14159265358979323846 * toneFreq *
                                 static_cast<double>(startSample + i);
                const float re = static_cast<float>(amp * std::cos(a)) + noise();
                const float im = static_cast<float>(amp * std::sin(a)) + noise();
                // The inverse of the balancer's own correction, so that a
                // correctly converged estimate cancels it exactly.
                const float dre = (re - damagePhase * im) * (1.0f - damageAmplitude);
                const float dim = (im - damagePhase * re) * (1.0f + damageAmplitude);
                out[i] = std::complex<float>(dre, dim);
            }
        };

        std::vector<std::complex<float>> block;
        makeBlock(0, block);
        const double rawSignal = toneMagnitude(block.data(), kBlock, toneFreq);
        const double rawImage = toneMagnitude(block.data(), kBlock, -toneFreq);
        const double rawRejectionDb = 20.0 * std::log10(rawSignal / rawImage);
        std::printf("     balancer: image rejection before %.2f dB\n", rawRejectionDb);
        // The damage really did put an image there - otherwise the "after"
        // number would be measuring nothing.
        CHECK(rawRejectionDb > 20.0);
        CHECK(rawRejectionDb < 45.0);

        airspyhf::IqBalancer balancer;
        double lastRejectionDb = 0.0;
        for (int b = 0; b < kBlocks; ++b) {
            makeBlock(static_cast<std::size_t>(b) * kBlock, block);
            balancer.process(block.data(), static_cast<int>(kBlock), false);
        }
        {
            const double sig = toneMagnitude(block.data(), kBlock, toneFreq);
            const double img = toneMagnitude(block.data(), kBlock, -toneFreq);
            lastRejectionDb = 20.0 * std::log10(sig / img);
        }
        std::printf("     balancer: image rejection after %d blocks %.2f dB (phase %.5f, "
                    "amplitude %.5f)\n",
                    kBlocks, lastRejectionDb, balancer.phase(), balancer.amplitude());

        // THE PINNED NUMBER, measured on this bench 2026-09-14: 30.97 dB of
        // image rejection in, 76.57 dB out after 700 blocks - an improvement
        // of 45.6 dB. The threshold is set at 30 rather than at 45 because
        // the estimator is iterative and the last step's size depends on how
        // many blocks it happened to see; what is being guarded is that the
        // image is REJECTED, by tens of decibels, and a balancer that
        // silently stopped estimating (the deliberate break) cannot pass it.
        CHECK(lastRejectionDb > rawRejectionDb + 30.0);

        // ...and the estimate itself converged on the damage that was
        // applied, which is the stronger statement: a balancer that improved
        // the number by accident would not land on these two values.
        CHECK_NEAR(balancer.phase(), damagePhase, 0.01);
        CHECK_NEAR(balancer.amplitude(), damageAmplitude, 0.01);

        // The DC canceller, separately: a constant offset on both parts is
        // removed over time (iqbalancer.c:194-221, DcTimeConst 1e-4 per
        // sample, so it is deliberately slow).
        airspyhf::IqBalancer dcBalancer;
        std::vector<std::complex<float>> dc(kBlock, std::complex<float>(0.25f, -0.125f));
        for (int b = 0; b < 200; ++b) {
            std::fill(dc.begin(), dc.end(), std::complex<float>(0.25f, -0.125f));
            dcBalancer.process(dc.data(), static_cast<int>(kBlock), false);
        }
        double meanRe = 0.0;
        double meanIm = 0.0;
        for (const std::complex<float>& s : dc) {
            meanRe += s.real();
            meanIm += s.imag();
        }
        meanRe /= static_cast<double>(kBlock);
        meanIm /= static_cast<double>(kBlock);
        std::printf("     balancer: residual DC after 200 blocks (%.6f, %.6f) from (0.25, "
                    "-0.125)\n", meanRe, meanIm);
        CHECK(std::fabs(meanRe) < 0.01);
        CHECK(std::fabs(meanIm) < 0.01);

        // skipEval removes the DC it has already learned without learning any
        // more, which is what the reference's skip_eval flag is for.
        std::vector<std::complex<float>> skipped(16, std::complex<float>(0.25f, -0.125f));
        dcBalancer.process(skipped.data(), static_cast<int>(skipped.size()), true);
        CHECK(std::fabs(skipped[0].real()) < 0.01);
    }

    // =====================================================================
    // 11. STOP WHILE STREAMING comes back promptly.
    // =====================================================================
    {
        AirspyHfSource src;
        FakeAirspyHfUsb* fake = attachFake(src);
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
    // 12. THE DEVICE GOES: the reader leaves, and says why.
    // =====================================================================
    {
        const unsigned long long abandonedBefore = AirspyHfSource::readersAbandoned();
        AirspyHfSource src;
        FakeAirspyHfUsb* fake = attachFake(src);
        CHECK(src.open(""));
        fake->onExhausted.store(FakeAirspyHfUsb::Exhausted::DeviceGone);
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
        CHECK(AirspyHfSource::readersAbandoned() == abandonedBefore);
        src.closeDevice();
    }

    // =====================================================================
    // 13. A READER THAT WILL NOT COME BACK is abandoned, not waited for.
    // =====================================================================
    {
        const unsigned long long abandonedBefore = AirspyHfSource::readersAbandoned();
        AirspyHfSource src;
        FakeAirspyHfUsb* fake = attachFake(src);
        CHECK(src.open(""));
        fake->onExhausted.store(FakeAirspyHfUsb::Exhausted::Block);
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
                        (long long)airspyhf::kReaderJoinWait.count());
        }
        CHECK(elapsed >= 900);
        CHECK(elapsed < 2500);
        CHECK(AirspyHfSource::readersAbandoned() == abandonedBefore + 1);
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
    // 14. OPENING WHAT IS NOT THERE fails cleanly.
    // =====================================================================
    {
        AirspyHfSource src;
        // No devices at all.
        src.setTransportForTest({}, [](const std::string&, std::string& error) {
            error = "should never be reached";
            return std::unique_ptr<cascade::usb::UsbDevice>();
        });
        CHECK(!src.open(""));
        CHECK(!src.isOpen());
        CHECK(std::string(src.lastError()).find("no Airspy HF+ found") != std::string::npos);
        // Nothing to stop, nothing to read, nothing to crash on.
        src.stop();
        std::complex<float> buf[4];
        CHECK(src.read(buf, 4) == 0);
        CHECK(!src.start());
        src.closeDevice();
    }
    {
        AirspyHfSource src;
        attachFake(src);
        // The right family, the wrong serial.
        CHECK(!src.open("serial=ffffffffffffffff"));
        CHECK(!src.isOpen());
        CHECK(std::string(src.lastError()).find("ffffffffffffffff") != std::string::npos);
        // An index past the end.
        CHECK(!src.open("index=4"));
        CHECK(std::string(src.lastError()).find("index 4") != std::string::npos);
        // The full serial STRING as Windows reports it finds the same radio as
        // the bare hex a user reads off another tool's listing.
        CHECK(src.open(std::string("serial=") + kFakeSerialString));
        CHECK(src.isOpen());
        src.closeDevice();
    }

    // =====================================================================
    // 15. A CONTROL TRANSFER THAT FAILS condemns the device...
    // =====================================================================
    {
        AirspyHfSource src;
        FakeAirspyHfUsb* fake = attachFake(src);
        CHECK(src.open(""));
        fake->failingRequests.push_back(2);  // SET_FREQ
        CHECK(!src.setCenterFrequencyHz(14.4e6));
        CHECK(src.faulted());
        CHECK(src.deviceDead());
        CHECK(src.faultedWhile() == "setting the centre frequency");
        // Nothing further is sent to a dead device, whatever is asked of it.
        fake->clearControls();
        CHECK(!src.setCenterFrequencyHz(14.5e6));
        CHECK(!src.setGainDb("ATT", -6.0));
        CHECK(!src.setSampleRateHz(768000.0));
        CHECK(!src.setAutoGain(true));
        CHECK(!src.setBiasT(true));
        CHECK(!src.start());
        CHECK(fake->controlCount() == 0);
        src.closeDevice();
    }

    // An open that fails half way through leaves nothing behind.
    {
        AirspyHfSource src;
        FakeAirspyHfUsb* fake = attachFake(src);
        fake->failingRequests.push_back(3);  // GET_SAMPLERATES, which is NOT optional
        CHECK(!src.open(""));
        CHECK(!src.isOpen());
        CHECK(src.faulted());
        CHECK(src.faultedWhile() == "asking how many sample rates it has");
        src.closeDevice();
    }
    {
        AirspyHfSource src;
        FakeAirspyHfUsb* fake = attachFake(src);
        fake->failingRequests.push_back(7);  // GET_SERIALNO_BOARDID
        CHECK(!src.open(""));
        CHECK(!src.isOpen());
        CHECK(src.faulted());
        CHECK(src.faultedWhile() == "reading the part id and serial number");
        src.closeDevice();
    }

    // =====================================================================
    // 16. ...BUT THE FIVE OPTIONAL REQUESTS DO NOT.
    // =====================================================================
    //
    // An Airspy HF+ with firmware from before these requests existed STALLS
    // them, which at the transport is the same negative return an unplugged
    // device gives. libairspyhf assumes a default for every one and carries
    // on (airspyhf.c:1002-1008, :1042-1058, :1076-1099, :1273-1279,
    // :1728-1751), and so must this - refusing to open a working receiver
    // because it is three years out of date would be a worse failure than
    // any of these defaults.
    {
        AirspyHfSource src;
        FakeAirspyHfUsb* fake = attachFake(src);
        fake->failingRequests = {14, 19, 5, 20, 15};  // architectures, att steps,
                                                      // config, bias tee count,
                                                      // filter gain
        const bool opened = src.open("");
        if (!opened) { std::printf("     old-firmware open() said: %s\n", src.lastError()); }
        CHECK(opened);
        CHECK(src.isOpen());
        CHECK(!src.faulted());
        CHECK(!src.deviceDead());

        // Every rate is assumed zero-IF (airspyhf.c:1002-1008).
        const std::vector<std::uint8_t> arch = src.rateArchitectures();
        CHECK(arch.size() == 6);
        bool allZero = true;
        for (const std::uint8_t a : arch) {
            if (a != 0) { allZero = false; }
        }
        CHECK(allZero);
        CHECK(!src.isLowIf());
        // The Discovery's nine steps are assumed (:1049-1058).
        CHECK(src.attenuatorStepsDb().size() == 9);
        // No calibration, no bias tee.
        CHECK(src.calibrationPpb() == 0);
        CHECK(!src.biasTeeSupported());
        CHECK(!src.setBiasT(true));
        // And with no calibration the tune is the plain arithmetic: 10 MHz
        // plus the 5 kHz offset is 10005 kHz, which happens to be the same
        // kilohertz as the calibrated case and is NOT the same residual - the
        // -1200 ppb that moved the first one by 12 Hz is gone.
        const std::uint8_t deltaBytes[4] = {0x10, 0x34, 0x12, 0x00};
        const double delta = airspyhf::decodeFreqDelta(deltaBytes);
        const ExpectedTune want = expectedTuning(10.0e6, 0, false, true, delta);
        CHECK_NEAR(src.frequencyShiftHz(), want.shiftHz, 1e-6);
        CHECK_NEAR(src.frequencyShiftHz(), -5000.0 + 71.10595703125, 1e-6);
        src.closeDevice();
    }

    // A firmware that will not list its rates at all still streams, on the
    // reference's assumed default (airspyhf.c:1016-1026). This is not the
    // same as the request failing - the device answers a count of zero.
    {
        AirspyHfSource src;
        FakeAirspyHfUsb* fake = attachFake(src);
        fake->rates.clear();
        fake->architectures.clear();
        CHECK(src.open(""));
        const std::vector<double> menu = src.supportedSampleRatesHz();
        CHECK(menu.size() == 1);
        if (menu.size() == 1) { CHECK_NEAR(menu[0], 768000.0, 0.5); }
        src.closeDevice();
    }

    return testSummary("test_airspyhf_source");
}
