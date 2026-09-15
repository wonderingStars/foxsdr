// test_hackrf_source.cpp - the HackRF driver, proven byte for byte against a
// fake that answers as the firmware does.
//
// WHERE THE EXPECTATIONS COME FROM. There is no HackRF on this bench, so an
// expectation invented here would only prove this file agrees with itself.
// Every number below is either read straight out of libhackrf's source (the
// vendor request numbers, the register semantics, the payload layouts - each
// one names the function it came from) or was PRODUCED by compiling
// libhackrf's own bodies and printing the answer: hackrf_set_sample_rate's
// rational loop and hackrf_compute_baseband_filter_bw were transcribed
// verbatim into a throwaway C program and run for the rates this file pins.
// Reading them off by eye would have been the same class of mistake as
// answering a datasheet question from memory.
//
// THE FOUR DELIBERATE BREAKS. Every block that matters was watched go RED
// against a broken driver before it was trusted green:
//   - a wrong request number (SAMPLE_RATE_SET 6 -> 5)
//   - a wrong divider (computeRate forced to divider 1)
//   - a dropped bulk buffer (the reader skipping every second write)
//   - a hung reader (the bounded join replaced by a plain one)
// The report for this change-set carries the failing line each of them
// produced. A fix whose test never went red proves nothing, and a suite that
// has never been broken on purpose is a suite nobody has read.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "hackrf_fake_usb.hpp"
#include "source/hackrf_protocol.hpp"
#include "source/hackrf_source.hpp"
#include "test_check.hpp"

using cascade::source::HackRfSource;
using cascade::source::NativeDeviceInfo;
using cascade::test::ControlRecord;
using cascade::test::FakeHackRfUsb;
namespace hackrf = cascade::source::hackrf;

namespace {

// --- helpers ---------------------------------------------------------------

// Bounds-safe indexing. A `CHECK(v.size() == n)` followed by `v[i]` is an
// out-of-bounds read in exactly the run that has something to report - the
// harness records a failed check and carries on, so the crash lands instead of
// the message. (mayhem-b200, 2026-08-13; the same trap cost a whole session.)
const ControlRecord& at(const std::vector<ControlRecord>& v, std::size_t i) {
    static const ControlRecord kAbsent{};
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
bool isControl(const char* label, const ControlRecord& r, bool in, int request, int value,
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
                                                       std::uint16_t pid = 0x6089) {
    cascade::usb::UsbDeviceInfo d;
    d.vid = 0x1D50;
    d.pid = pid;
    d.path = "\\\\?\\usb#vid_1d50&pid_6089#fake#{a5dcbf10}";
    d.serial = serial;
    d.description = "HackRF One";
    return {d};
}

constexpr const char* kFakeSerial = "0000000000000000457863c8";

// Points `src` at a fresh fake and returns a borrowed pointer to it. The fake
// is handed to the source on the FIRST successful open only, so a second one
// fails cleanly (with a null device) instead of two owners fighting over one
// object.
FakeHackRfUsb* attachFake(HackRfSource& src,
                          std::vector<cascade::usb::UsbDeviceInfo> devices = {}) {
    if (devices.empty()) { devices = oneFakeDevice(kFakeSerial); }
    auto owned = std::make_unique<FakeHackRfUsb>();
    FakeHackRfUsb* raw = owned.get();
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

}  // namespace

int main() {
    // =====================================================================
    // 1. THE ARITHMETIC, against values computed by libhackrf's own bodies.
    // =====================================================================
    {
        // hackrf_set_sample_rate (hackrf.c:1920-1959) followed by
        // hackrf_set_sample_rate_manual's automatic filter choice
        // (hackrf.c:1907-1910: compute_baseband_filter_bw(0.75 * fs)).
        //
        // Reference output, from compiling those two functions verbatim:
        //   10000000 -> freq_hz=10000000 divider=1 bbfilter=7000000
        //    2000000 -> freq_hz= 2000000 divider=1 bbfilter=1750000
        //    2400000 -> freq_hz= 2400000 divider=1 bbfilter=1750000
        //    8000000 -> freq_hz= 8000000 divider=1 bbfilter=6000000
        //   20000000 -> freq_hz=20000000 divider=1 bbfilter=15000000
        //   8000000/3 -> freq_hz=8000000 divider=3 bbfilter=1750000
        //   12500000 -> freq_hz=12500000 divider=1 bbfilter=9000000
        //    4500000 -> freq_hz= 4500000 divider=1 bbfilter=2500000
        struct RateCase {
            double hz;
            std::uint32_t freqHz;
            std::uint32_t divider;
            std::uint32_t filter;
        };
        const RateCase cases[] = {
            {10.0e6, 10000000, 1, 7000000},     {2.0e6, 2000000, 1, 1750000},
            {2.4e6, 2400000, 1, 1750000},       {8.0e6, 8000000, 1, 6000000},
            {20.0e6, 20000000, 1, 15000000},    {8.0e6 / 3.0, 8000000, 3, 1750000},
            {12.5e6, 12500000, 1, 9000000},     {4.5e6, 4500000, 1, 2500000},
        };
        for (const RateCase& c : cases) {
            const hackrf::RateSetting r = hackrf::computeRate(c.hz);
            const bool ok = r.freqHz == c.freqHz && r.divider == c.divider;
            if (!ok) {
                std::printf("     computeRate(%.6f): got freq_hz=%u divider=%u, want %u / %u\n",
                            c.hz, r.freqHz, r.divider, c.freqHz, c.divider);
            }
            CHECK(ok);
            const std::uint32_t bw = hackrf::basebandFilterForRate(r);
            if (bw != c.filter) {
                std::printf("     basebandFilterForRate(%.6f): got %u, want %u\n", c.hz, bw,
                            c.filter);
            }
            CHECK(bw == c.filter);
        }

        // hackrf_compute_baseband_filter_bw (hackrf.c:2666-2682) at every
        // boundary the table has, including the two the reference's own
        // `p != max2837_ft` guard makes special: a request below the narrowest
        // width keeps the narrowest, and an exact hit is NOT rounded down.
        // Reference output:
        //   bb(0)=1750000 bb(1)=1750000 bb(1750000)=1750000 bb(1750001)=1750000
        //   bb(1800000)=1750000 bb(2000000)=1750000 bb(2500000)=2500000
        //   bb(3500000)=3500000 bb(6000000)=6000000 bb(7500000)=7000000
        //   bb(9375000)=9000000 bb(15000000)=15000000 bb(28000000)=28000000
        //   bb(30000000)=0        <- the reference walks onto its terminator
        struct BwCase {
            std::uint32_t in;
            std::uint32_t out;
        };
        const BwCase bws[] = {{0, 1750000},         {1, 1750000},        {1750000, 1750000},
                              {1750001, 1750000},   {1800000, 1750000},  {2000000, 1750000},
                              {2500000, 2500000},   {3500000, 3500000},  {6000000, 6000000},
                              {7500000, 7000000},   {9375000, 9000000},  {15000000, 15000000},
                              {28000000, 28000000}, {30000000, 0}};
        for (const BwCase& c : bws) {
            const std::uint32_t got = hackrf::computeBasebandFilterBw(c.in);
            if (got != c.out) {
                std::printf("     computeBasebandFilterBw(%u): got %u, want %u\n", c.in, got,
                            c.out);
            }
            CHECK(got == c.out);
        }

        // hackrf_set_freq (hackrf.c:1775-1790): whole MHz and the remainder.
        struct FreqCase {
            std::uint64_t hz;
            std::uint32_t mhz;
            std::uint32_t rem;
        };
        const FreqCase freqs[] = {{100000000ULL, 100, 0},
                                  {1090000000ULL, 1090, 0},
                                  {5800000000ULL, 5800, 0},
                                  {433920000ULL, 433, 920000},
                                  {1000000ULL, 1, 0},
                                  {6000000000ULL, 6000, 0}};
        for (const FreqCase& c : freqs) {
            const hackrf::FreqSplit s = hackrf::splitFrequency(c.hz);
            if (s.mhz != c.mhz || s.hz != c.rem) {
                std::printf("     splitFrequency(%llu): got %u MHz + %u Hz, want %u + %u\n",
                            static_cast<unsigned long long>(c.hz), s.mhz, s.hz, c.mhz, c.rem);
            }
            CHECK(s.mhz == c.mhz);
            CHECK(s.hz == c.rem);
        }
        // ...and the wire bytes for one of them, little-endian, MHz first.
        std::uint8_t payload[hackrf::kFreqPayloadBytes];
        hackrf::encodeFreq(hackrf::splitFrequency(433920000ULL), payload);
        CHECK(sameBytes("encodeFreq(433.92 MHz)",
                        std::vector<std::uint8_t>(payload, payload + sizeof(payload)),
                        {0xB1, 0x01, 0x00, 0x00, 0xC0, 0x09, 0x0E, 0x00}));

        // hackrf_set_lna_gain (hackrf.c:2022-2031): refuse above 40, then
        // `value &= ~0x07`. Our contract clamps instead of refusing (see
        // device_source.hpp), and the mask is the reference's.
        CHECK(hackrf::roundLnaGainDb(0.0) == 0);
        CHECK(hackrf::roundLnaGainDb(-5.0) == 0);
        CHECK(hackrf::roundLnaGainDb(7.9) == 0);
        CHECK(hackrf::roundLnaGainDb(8.0) == 8);
        CHECK(hackrf::roundLnaGainDb(16.0) == 16);
        CHECK(hackrf::roundLnaGainDb(23.0) == 16);
        CHECK(hackrf::roundLnaGainDb(40.0) == 40);
        CHECK(hackrf::roundLnaGainDb(100.0) == 40);

        // hackrf_set_vga_gain (hackrf.c:2049-2058): `value &= ~0x01`, max 62.
        CHECK(hackrf::roundVgaGainDb(0.0) == 0);
        CHECK(hackrf::roundVgaGainDb(1.0) == 0);
        CHECK(hackrf::roundVgaGainDb(20.0) == 20);
        CHECK(hackrf::roundVgaGainDb(21.0) == 20);
        CHECK(hackrf::roundVgaGainDb(62.0) == 62);
        CHECK(hackrf::roundVgaGainDb(99.0) == 62);

        // The sample table: signed 8-bit into [-1, 1).
        const hackrf::SampleTable& lut = hackrf::sampleTable();
        CHECK_NEAR(lut.v[0], 0.0f, 1e-9);
        CHECK_NEAR(lut.v[1], 1.0f / 128.0f, 1e-9);
        CHECK_NEAR(lut.v[127], 127.0f / 128.0f, 1e-9);
        CHECK_NEAR(lut.v[128], -1.0f, 1e-9);
        CHECK_NEAR(lut.v[255], -1.0f / 128.0f, 1e-9);

        // The request numbers themselves, against hackrf.c:64-125.
        CHECK(hackrf::requestByte(hackrf::VendorRequest::SetTransceiverMode) == 1);
        CHECK(hackrf::requestByte(hackrf::VendorRequest::SampleRateSet) == 6);
        CHECK(hackrf::requestByte(hackrf::VendorRequest::BasebandFilterBandwidthSet) == 7);
        CHECK(hackrf::requestByte(hackrf::VendorRequest::BoardIdRead) == 14);
        CHECK(hackrf::requestByte(hackrf::VendorRequest::VersionStringRead) == 15);
        CHECK(hackrf::requestByte(hackrf::VendorRequest::SetFreq) == 16);
        CHECK(hackrf::requestByte(hackrf::VendorRequest::AmpEnable) == 17);
        CHECK(hackrf::requestByte(hackrf::VendorRequest::BoardPartIdSerialNoRead) == 18);
        CHECK(hackrf::requestByte(hackrf::VendorRequest::SetLnaGain) == 19);
        CHECK(hackrf::requestByte(hackrf::VendorRequest::SetVgaGain) == 20);
        CHECK(hackrf::requestByte(hackrf::VendorRequest::AntennaEnable) == 23);
        CHECK(hackrf::kRxEndpoint == 0x81);
        CHECK(hackrf::kTransferBufferBytes == 262144);
        CHECK(hackrf::kUsbVid == 0x1D50);
    }

    // =====================================================================
    // 2. ENUMERATION: which devices are ours, and what reopens each one.
    // =====================================================================
    {
        std::vector<cascade::usb::UsbDeviceInfo> devices;
        cascade::usb::UsbDeviceInfo one;
        one.vid = 0x1D50;
        one.pid = 0x6089;
        one.serial = "0000000000000000457863c8";
        one.path = "p1";
        devices.push_back(one);
        cascade::usb::UsbDeviceInfo jaw;
        jaw.vid = 0x1D50;
        jaw.pid = 0x604B;
        jaw.serial = "deadbeef";
        jaw.path = "p2";
        devices.push_back(jaw);
        cascade::usb::UsbDeviceInfo rad;
        rad.vid = 0x1D50;
        rad.pid = 0xCC15;
        rad.serial.clear();  // a device with no serial is still openable
        rad.path = "p3";
        devices.push_back(rad);
        cascade::usb::UsbDeviceInfo stranger;
        stranger.vid = 0x0BDA;  // an RTL-SDR: not ours, and must not be opened
        stranger.pid = 0x2838;
        stranger.path = "p4";
        devices.push_back(stranger);

        const std::vector<NativeDeviceInfo> found =
            cascade::source::hackRfDevicesFrom(devices);
        CHECK(found.size() == 3);
        // Bounds-safe reads, for the reason `at` above exists.
        const NativeDeviceInfo absent;
        const NativeDeviceInfo& f0 = found.size() > 0 ? found[0] : absent;
        const NativeDeviceInfo& f1 = found.size() > 1 ? found[1] : absent;
        const NativeDeviceInfo& f2 = found.size() > 2 ? found[2] : absent;
        CHECK(f0.driver == "hackrf");
        CHECK(f0.label == "HackRF One (serial 0000000000000000457863c8)");
        CHECK(f0.args == "serial=0000000000000000457863c8");
        CHECK(f1.label == "HackRF Jawbreaker (serial deadbeef)");
        CHECK(f1.args == "serial=deadbeef");
        CHECK(f2.label == "rad1o");
        CHECK(f2.args == "index=2");

        // The VID/PID list the transport is asked for is the reference's three
        // and nothing else (hackrf.c:202-205).
        const std::vector<cascade::usb::UsbId> ids = cascade::source::hackRfUsbIds();
        CHECK(ids.size() == 3);
    }

    // =====================================================================
    // 3. OPEN: the whole opening sequence, byte for byte.
    // =====================================================================
    {
        HackRfSource src;
        FakeHackRfUsb* fake = attachFake(src);
        const bool opened = src.open("serial=0000000000000000457863c8");
        if (!opened) { std::printf("     open() said: %s\n", src.lastError()); }
        CHECK(opened);
        CHECK(src.isOpen());
        CHECK(!src.faulted());
        CHECK(std::string(src.name()) ==
              "HackRF: HackRF One (serial 0000000000000000457863c8)");

        // The identity the driver read back, so a problem report can say which
        // board and which firmware without asking the radio again mid-stream.
        CHECK(src.boardId() == 2);
        CHECK(src.firmwareVersion() == "2024.02.1");
        // Words 2..5 of read_partid_serialno_t (hackrf.h:981-990) are the MCU
        // unique id, which is what every HackRF tool prints as the serial.
        CHECK(src.partIdSerialNo() == "0000000000000000457863c82e1a51df");

        const std::vector<ControlRecord> c = fake->controls();
        if (c.size() != 10) {
            std::printf("     open() sent %zu control transfers, expected 10\n", c.size());
            for (std::size_t i = 0; i < c.size(); ++i) {
                std::printf("       [%zu] %s req %u value 0x%04x index 0x%04x data [%s]\n", i,
                            c[i].in ? "IN " : "OUT", static_cast<unsigned>(c[i].request),
                            static_cast<unsigned>(c[i].value), static_cast<unsigned>(c[i].index),
                            hexOf(c[i].data).c_str());
            }
        }
        CHECK(c.size() == 10);

        // hackrf_board_id_read (hackrf.c:1713-1732), hackrf_version_string_read
        // (:1734-1756), hackrf_board_partid_serialno_read (:1983-2020) - the
        // three reads, in libhackrf's own order, all IN with value and index 0.
        CHECK(isControl("open[0] board id", at(c, 0), true, 14, 0, 0));
        CHECK(isControl("open[1] version", at(c, 1), true, 15, 0, 0));
        CHECK(isControl("open[2] part/serial", at(c, 2), true, 18, 0, 0));

        // hackrf_set_sample_rate_manual (hackrf.c:1879-1911): 10 MS/s is
        // freq_hz 10000000 (0x00989680) with divider 1, then the filter that
        // goes with it - 7000000 (0x006ACFC0) as value 0xCFC0, index 0x006A.
        CHECK(isControl("open[3] sample rate", at(c, 3), false, 6, 0, 0));
        CHECK(sameBytes("open[3] sample rate", at(c, 3).data,
                        {0x80, 0x96, 0x98, 0x00, 0x01, 0x00, 0x00, 0x00}));
        CHECK(isControl("open[4] baseband filter", at(c, 4), false, 7, 0xCFC0, 0x006A));

        // hackrf_set_freq (hackrf.c:1775-1805): 100 MHz is 100 + 0 Hz.
        CHECK(isControl("open[5] set freq", at(c, 5), false, 16, 0, 0));
        CHECK(sameBytes("open[5] set freq", at(c, 5).data,
                        {0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));

        // hackrf_set_lna_gain / hackrf_set_vga_gain: the value rides in the
        // INDEX word and the firmware answers one byte.
        CHECK(isControl("open[6] LNA 16 dB", at(c, 6), true, 19, 0, 16));
        CHECK(isControl("open[7] VGA 16 dB", at(c, 7), true, 20, 0, 16));

        // hackrf_set_amp_enable (:1961) and hackrf_set_antenna_enable (:2102):
        // both OFF, so the radio's state is ours from the first frame rather
        // than whatever the last application left - a bias-T inherited on is
        // 3.3 V into somebody's antenna with nothing on screen to say so.
        CHECK(isControl("open[8] amp off", at(c, 8), false, 17, 0, 0));
        CHECK(isControl("open[9] bias-T off", at(c, 9), false, 23, 0, 0));

        CHECK_NEAR(src.sampleRateHz(), 10.0e6, 0.5);
        CHECK_NEAR(src.centerFrequencyHz(), 100.0e6, 0.5);
        CHECK_NEAR(src.gainDb("LNA"), 16.0, 1e-9);
        CHECK_NEAR(src.gainDb("VGA"), 16.0, 1e-9);
        CHECK_NEAR(src.gainDb("AMP"), 0.0, 1e-9);
        CHECK(!src.biasT());
        CHECK(!src.autoGainSupported());
        CHECK(!src.setAutoGain(true));
        CHECK(src.antennas().size() == 1);
        CHECK(src.setAntenna("RX"));
        CHECK(!src.setAntenna("TX"));
        double lo = 0.0, hi = 0.0;
        CHECK(src.frequencyRangeHz(lo, hi));
        CHECK_NEAR(lo, 1.0e6, 0.5);
        CHECK_NEAR(hi, 6.0e9, 0.5);
        CHECK(std::string(src.driverKey()) == "hackrf");
        CHECK(src.selfPaced());
        src.closeDevice();
        CHECK(!src.isOpen());
    }

    // =====================================================================
    // 4. RATES AND TUNES, each as the exact transfer it becomes.
    // =====================================================================
    {
        HackRfSource src;
        FakeHackRfUsb* fake = attachFake(src);
        CHECK(src.open(""));  // no args: the first device
        fake->clearControls();

        // 10 MS/s, reprogrammed rather than short-circuited: the driver has no
        // "already there" shortcut, so what the panel asks for is always what
        // the radio is told.
        CHECK(src.setSampleRateHz(10.0e6));
        {
            const std::vector<ControlRecord> c = fake->controls();
            CHECK(c.size() == 2);
            CHECK(isControl("rate 10 MS/s", at(c, 0), false, 6, 0, 0));
            CHECK(sameBytes("rate 10 MS/s", at(c, 0).data,
                            {0x80, 0x96, 0x98, 0x00, 0x01, 0x00, 0x00, 0x00}));
            CHECK(isControl("filter for 10 MS/s", at(c, 1), false, 7, 0xCFC0, 0x006A));
        }
        CHECK_NEAR(src.sampleRateHz(), 10.0e6, 0.5);

        // 2 MS/s: freq_hz 2000000 (0x001E8480), divider 1, filter 1750000
        // (0x001AB3F0) -> value 0xB3F0, index 0x001A.
        fake->clearControls();
        CHECK(src.setSampleRateHz(2.0e6));
        {
            const std::vector<ControlRecord> c = fake->controls();
            CHECK(c.size() == 2);
            CHECK(isControl("rate 2 MS/s", at(c, 0), false, 6, 0, 0));
            CHECK(sameBytes("rate 2 MS/s", at(c, 0).data,
                            {0x80, 0x84, 0x1E, 0x00, 0x01, 0x00, 0x00, 0x00}));
            CHECK(isControl("filter for 2 MS/s", at(c, 1), false, 7, 0xB3F0, 0x001A));
        }
        CHECK_NEAR(src.sampleRateHz(), 2.0e6, 0.5);

        // The one rate in range that does NOT come out as divider 1: the
        // reference's rational loop answers 8000000 / 3 for 2.6666 MS/s.
        fake->clearControls();
        CHECK(src.setSampleRateHz(8.0e6 / 3.0));
        {
            const std::vector<ControlRecord> c = fake->controls();
            CHECK(c.size() == 2);
            CHECK(sameBytes("rate 8/3 MS/s", at(c, 0).data,
                            {0x00, 0x12, 0x7A, 0x00, 0x03, 0x00, 0x00, 0x00}));
        }
        CHECK_NEAR(src.sampleRateHz(), 8.0e6 / 3.0, 1.0);

        // Below the documented floor is REFUSED with a reason, not quietly
        // raised (hackrf.h:1790 - "should be in the range 2-20MHz").
        fake->clearControls();
        const double before = src.sampleRateHz();
        CHECK(!src.setSampleRateHz(1.0e6));
        CHECK(fake->controlCount() == 0);
        CHECK(std::string(src.lastError()).find("lowest sample rate") != std::string::npos);
        CHECK_NEAR(src.sampleRateHz(), before, 1.0);
        CHECK(!src.faulted());  // a refusal is not a fault

        // Above the ceiling IS coerced - the caller can be given everything
        // the radio has and told so.
        fake->clearControls();
        CHECK(src.setSampleRateHz(40.0e6));
        CHECK_NEAR(src.sampleRateHz(), 20.0e6, 0.5);
        {
            const std::vector<ControlRecord> c = fake->controls();
            CHECK(c.size() == 2);
            // 20000000 = 0x01312D00, divider 1; filter 15000000 = 0x00E4E1C0.
            CHECK(sameBytes("rate coerced to 20 MS/s", at(c, 0).data,
                            {0x00, 0x2D, 0x31, 0x01, 0x01, 0x00, 0x00, 0x00}));
            CHECK(isControl("filter for 20 MS/s", at(c, 1), false, 7, 0xE1C0, 0x00E4));
        }

        // --- tuning ------------------------------------------------------
        struct TuneCase {
            double hz;
            std::vector<int> bytes;
            const char* label;
        };
        const TuneCase tunes[] = {
            {100.0e6, {0x64, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, "tune 100 MHz"},
            {1.09e9, {0x42, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, "tune 1.09 GHz"},
            {5.8e9, {0xA8, 0x16, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, "tune 5.8 GHz"},
            {433.92e6, {0xB1, 0x01, 0x00, 0x00, 0xC0, 0x09, 0x0E, 0x00}, "tune 433.92 MHz"},
        };
        for (const TuneCase& t : tunes) {
            fake->clearControls();
            const bool ok = src.setCenterFrequencyHz(t.hz);
            if (!ok) { std::printf("     %s said: %s\n", t.label, src.lastError()); }
            CHECK(ok);
            const std::vector<ControlRecord> c = fake->controls();
            CHECK(c.size() == 1);
            CHECK(isControl(t.label, at(c, 0), false, 16, 0, 0));
            CHECK(sameBytes(t.label, at(c, 0).data, t.bytes));
            CHECK_NEAR(src.centerFrequencyHz(), t.hz, 0.5);
        }

        // Outside 1 MHz - 6 GHz is refused, and nothing is sent: a tune that
        // silently lands somewhere else is worse than one that does not happen.
        fake->clearControls();
        CHECK(!src.setCenterFrequencyHz(500.0e3));
        CHECK(!src.setCenterFrequencyHz(7.0e9));
        CHECK(fake->controlCount() == 0);
        CHECK(!src.faulted());

        // --- gains -------------------------------------------------------
        fake->clearControls();
        CHECK(src.setGainDb("LNA", 16.0));
        CHECK(src.setGainDb("VGA", 20.0));
        CHECK(src.setGainDb("AMP", 14.0));
        CHECK(src.setBiasT(true));
        {
            const std::vector<ControlRecord> c = fake->controls();
            CHECK(c.size() == 4);
            CHECK(isControl("LNA 16 dB", at(c, 0), true, 19, 0, 16));
            CHECK(isControl("VGA 20 dB", at(c, 1), true, 20, 0, 20));
            CHECK(isControl("AMP on", at(c, 2), false, 17, 1, 0));
            CHECK(isControl("bias-T on", at(c, 3), false, 23, 1, 0));
        }
        CHECK_NEAR(src.gainDb("LNA"), 16.0, 1e-9);
        CHECK_NEAR(src.gainDb("VGA"), 20.0, 1e-9);
        CHECK_NEAR(src.gainDb("AMP"), 14.0, 1e-9);
        CHECK(src.biasT());

        // Out of range is CLAMPED and rounded to the hardware's step, and the
        // readback says what was actually programmed (device_source.hpp).
        fake->clearControls();
        CHECK(src.setGainDb("LNA", 999.0));
        CHECK(src.setGainDb("VGA", 21.0));
        {
            const std::vector<ControlRecord> c = fake->controls();
            CHECK(c.size() == 2);
            CHECK(isControl("LNA clamped to 40 dB", at(c, 0), true, 19, 0, 40));
            CHECK(isControl("VGA 21 rounded to 20", at(c, 1), true, 20, 0, 20));
        }
        CHECK_NEAR(src.gainDb("LNA"), 40.0, 1e-9);
        CHECK_NEAR(src.gainDb("VGA"), 20.0, 1e-9);

        // An unknown gain name is a refusal, not a transfer.
        fake->clearControls();
        CHECK(!src.setGainDb("TUNER", 20.0));
        CHECK(fake->controlCount() == 0);

        // A firmware that REFUSES a gain (the one-byte answer is zero,
        // hackrf.c:2039) is a refusal, not a dead device.
        fake->clearControls();
        fake->gainReply = 0;
        CHECK(!src.setGainDb("LNA", 24.0));
        CHECK(fake->controlCount() == 1);
        CHECK(!src.faulted());
        CHECK(!src.deviceDead());
        CHECK(std::string(src.lastError()).find("refused") != std::string::npos);
        fake->gainReply = 1;

        src.closeDevice();
    }

    // =====================================================================
    // 5. START AND STOP: the transceiver, and the bulk ring around it.
    // =====================================================================
    {
        HackRfSource src;
        FakeHackRfUsb* fake = attachFake(src);
        CHECK(src.open(""));
        fake->clearControls();

        CHECK(src.start());
        CHECK(src.running());
        // hackrf_set_transceiver_mode (hackrf.c:943): the mode is the VALUE
        // word. RECEIVE is 1.
        {
            const std::vector<ControlRecord> c = fake->controls();
            CHECK(c.size() == 1);
            CHECK(isControl("start: RECEIVE", at(c, 0), false, 1, 1, 0));
        }
        // The ring is queued BEFORE the transceiver is told to receive, on the
        // RX endpoint, with libhackrf's own transfer geometry.
        CHECK(fake->beginBulkCalls() == 1);
        CHECK(fake->lastBulkEndpoint() == 0x81);
        CHECK(fake->lastBulkBufferBytes() == 262144);
        CHECK(fake->lastBulkBufferCount() == 4);

        CHECK(src.start());  // idempotent
        CHECK(fake->beginBulkCalls() == 1);

        fake->clearControls();
        src.stop();
        CHECK(!src.running());
        {
            const std::vector<ControlRecord> c = fake->controls();
            CHECK(c.size() == 1);
            CHECK(isControl("stop: OFF", at(c, 0), false, 1, 0, 0));
        }
        CHECK(fake->endBulkCalls() >= 1);

        // A rate change on a RUNNING stream happens with the radio quiet, and
        // running() is true either side of it.
        CHECK(src.start());
        fake->clearControls();
        const int beginsBefore = fake->beginBulkCalls();
        CHECK(src.setSampleRateHz(8.0e6));
        CHECK(src.running());
        {
            const std::vector<ControlRecord> c = fake->controls();
            CHECK(c.size() == 4);
            CHECK(isControl("rate change: OFF first", at(c, 0), false, 1, 0, 0));
            CHECK(isControl("rate change: new rate", at(c, 1), false, 6, 0, 0));
            // 8000000 = 0x007A1200, divider 1; filter 6000000 = 0x005B8D80.
            CHECK(sameBytes("rate change: 8 MS/s", at(c, 1).data,
                            {0x00, 0x12, 0x7A, 0x00, 0x01, 0x00, 0x00, 0x00}));
            CHECK(isControl("rate change: filter", at(c, 2), false, 7, 0x8D80, 0x005B));
            CHECK(isControl("rate change: RECEIVE again", at(c, 3), false, 1, 1, 0));
        }
        CHECK(fake->beginBulkCalls() == beginsBefore + 1);
        src.closeDevice();
    }

    // =====================================================================
    // 6. STREAMING: three scripted buffers, every sample once and in order.
    // =====================================================================
    {
        HackRfSource src;
        FakeHackRfUsb* fake = attachFake(src);
        CHECK(src.open(""));

        // A ramp across all three buffers, so a lost buffer, a repeated one or
        // a swapped pair all show up as a mismatch at a named index rather
        // than as a count that happens to come out right.
        constexpr std::size_t kBuffers = 3;
        constexpr std::size_t kBytesPerBuffer = 64;
        std::vector<std::uint8_t> allBytes;
        for (std::size_t b = 0; b < kBuffers; ++b) {
            std::vector<std::uint8_t> buf(kBytesPerBuffer);
            for (std::size_t i = 0; i < kBytesPerBuffer; ++i) {
                buf[i] = static_cast<std::uint8_t>((b * kBytesPerBuffer + i) * 3 + 1);
            }
            allBytes.insert(allBytes.end(), buf.begin(), buf.end());
            fake->queueBulk(std::move(buf));
        }
        const std::size_t wantSamples = allBytes.size() / 2;

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

        const hackrf::SampleTable& lut = hackrf::sampleTable();
        std::size_t firstBad = wantSamples;
        for (std::size_t i = 0; i < wantSamples && i < got.size(); ++i) {
            const float wantI = lut.v[allBytes[2 * i]];
            const float wantQ = lut.v[allBytes[2 * i + 1]];
            if (got[i].real() != wantI || got[i].imag() != wantQ) {
                firstBad = i;
                std::printf("     streaming: sample %zu is (%.6f, %.6f), expected (%.6f, %.6f)\n",
                            i, got[i].real(), got[i].imag(), wantI, wantQ);
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
    // 7. STOP WHILE STREAMING comes back promptly.
    // =====================================================================
    {
        HackRfSource src;
        FakeHackRfUsb* fake = attachFake(src);
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
    // 8. THE DEVICE GOES: the reader leaves, and says why.
    // =====================================================================
    {
        const unsigned long long abandonedBefore = HackRfSource::readersAbandoned();
        HackRfSource src;
        FakeHackRfUsb* fake = attachFake(src);
        CHECK(src.open(""));
        fake->onExhausted.store(FakeHackRfUsb::Exhausted::DeviceGone);
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
        CHECK(HackRfSource::readersAbandoned() == abandonedBefore);
        src.closeDevice();
    }

    // =====================================================================
    // 9. A READER THAT WILL NOT COME BACK is abandoned, not waited for.
    // =====================================================================
    {
        const unsigned long long abandonedBefore = HackRfSource::readersAbandoned();
        HackRfSource src;
        FakeHackRfUsb* fake = attachFake(src);
        CHECK(src.open(""));
        fake->onExhausted.store(FakeHackRfUsb::Exhausted::Block);
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
                        (long long)cascade::source::kReaderJoinWait.count());
        }
        CHECK(elapsed >= 900);
        CHECK(elapsed < 2500);
        CHECK(HackRfSource::readersAbandoned() == abandonedBefore + 1);
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
    // 10. OPENING WHAT IS NOT THERE fails cleanly.
    // =====================================================================
    {
        HackRfSource src;
        // No devices at all.
        src.setTransportForTest({}, [](const std::string&, std::string& error) {
            error = "should never be reached";
            return std::unique_ptr<cascade::usb::UsbDevice>();
        });
        CHECK(!src.open(""));
        CHECK(!src.isOpen());
        CHECK(std::string(src.lastError()).find("no HackRF found") != std::string::npos);
        // Nothing to stop, nothing to read, nothing to crash on.
        src.stop();
        std::complex<float> buf[4];
        CHECK(src.read(buf, 4) == 0);
        CHECK(!src.start());
        src.closeDevice();
    }
    {
        HackRfSource src;
        attachFake(src);
        // The right family, the wrong serial.
        CHECK(!src.open("serial=ffffffffffffffff"));
        CHECK(!src.isOpen());
        CHECK(std::string(src.lastError()).find("ffffffffffffffff") != std::string::npos);
        // An index past the end.
        CHECK(!src.open("index=4"));
        CHECK(std::string(src.lastError()).find("index 4") != std::string::npos);
        // The short form of a serial still finds it: a HackRF's is 32 hex
        // digits of leading zeros and the tail is what a user reads off.
        CHECK(src.open("serial=457863c8"));
        CHECK(src.isOpen());
        src.closeDevice();
    }

    // =====================================================================
    // 11. A CONTROL TRANSFER THAT FAILS condemns the device.
    // =====================================================================
    {
        HackRfSource src;
        FakeHackRfUsb* fake = attachFake(src);
        CHECK(src.open(""));
        fake->failingRequests.push_back(16);  // SET_FREQ
        CHECK(!src.setCenterFrequencyHz(144.0e6));
        CHECK(src.faulted());
        CHECK(src.deviceDead());
        CHECK(src.faultedWhile() == "setting the centre frequency");
        // Nothing further is sent to a dead device, whatever is asked of it.
        fake->clearControls();
        CHECK(!src.setCenterFrequencyHz(145.0e6));
        CHECK(!src.setGainDb("LNA", 8.0));
        CHECK(!src.setSampleRateHz(4.0e6));
        CHECK(!src.start());
        CHECK(fake->controlCount() == 0);
        src.closeDevice();
    }

    // An open that fails half way through leaves nothing behind.
    {
        HackRfSource src;
        FakeHackRfUsb* fake = attachFake(src);
        fake->failingRequests.push_back(18);  // BOARD_PARTID_SERIALNO_READ
        CHECK(!src.open(""));
        CHECK(!src.isOpen());
        CHECK(src.faulted());
        CHECK(src.faultedWhile() == "reading the part id and serial number");
        src.closeDevice();
    }

    return testSummary("test_hackrf_source");
}
