// test_hydrasdr_source.cpp - the HydraSDR RFOne driver, proven byte for byte
// against a fake that answers as the vendor's PUBLISHED FIRMWARE does
// (tests/hydrasdr_fake_usb.hpp), and sample for sample against signal theory.
//
// NOBODY HAS HELD A HYDRASDR. Every expectation here is therefore one of three
// things, and each block says which:
//
//  1. READ STRAIGHT OUT OF THE VENDOR'S SOURCES, naming the file and line -
//     the host library (github.com/hydrasdr/rfone_host, v1.1.3, commit
//     16942cb: libhydrasdr/src/hydrasdr_commands.h, hydrasdr_shared.c,
//     hydrasdr_rfone.c, hydrasdr.c) and the firmware (github.com/hydrasdr/
//     rfone_fw, commit 6c7c1fe: m0/usb_req.c, m0/usb_descriptor.c, m4/m4.c,
//     common/hydrasdr_rfone_conf.c). The vendor's gain tables and request
//     numbers are RETYPED here from those files, not imported from the driver,
//     so a mistake in the driver's copy is a difference and not an agreement.
//  2. COMPUTED HERE FROM THE FILTER'S OWN COEFFICIENTS - the tone check at the
//     end, which is the same arithmetic test_airspy_source.cpp proves the
//     converter against, now run THROUGH THE HYDRASDR DRIVER.
//  3. A DIFFERENCE FROM AN AIRSPY, asserted in both directions: the HydraSDR
//     driver does what the HydraSDR host does, and the Airspy driver, given the
//     same kind of device, still does what libairspy does.
//
// WHAT THIS CANNOT SHOW is that a real RFOne answers as that source says: the
// fake is the firmware's source, not a unit. The report for this change-set
// says what a tester with the hardware should try first.
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
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/diag_log.hpp"
#include "core/health_events.hpp"
#include "core/telemetry.hpp"
#include "hydrasdr_fake_usb.hpp"
#include "source/airspy_protocol.hpp"
#include "source/airspy_source.hpp"
#include "source/hydrasdr_protocol.hpp"
#include "source/hydrasdr_source.hpp"
#include "test_check.hpp"

using cascade::source::AirspySource;
using cascade::source::HydraSdrSource;
using cascade::source::NativeDeviceInfo;
using cascade::test::AirspyControlRecord;
using cascade::test::FakeAirspyUsb;
using cascade::test::FakeHydraSdrUsb;
namespace airspy = cascade::source::airspy;
namespace hs = cascade::source::hydrasdr;
namespace health = cascade::core::health;

namespace {

constexpr double kPi = 3.14159265358979323846;

// --- helpers ---------------------------------------------------------------

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

// One control transfer, whole, so a failure names the transfer that is wrong.
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

// The USB description Windows reports for a HydraSDR RFOne is its product
// string (rfone_fw m0/usb_descriptor.c:179-197).
std::vector<cascade::usb::UsbDeviceInfo> oneHydraDevice(
    const std::string& serial, const std::string& description = "HydraSDR RFOne") {
    cascade::usb::UsbDeviceInfo d;
    d.vid = hs::kUsbVid;
    d.pid = hs::kUsbPid;
    d.path = "\\\\?\\usb#vid_38af&pid_0001#fake#{a5dcbf10}";
    d.serial = serial;
    d.description = description;
    return {d};
}

// The serial as the instance id carries it: "HYDRASDR SN:" with the space an
// underscore (hydrasdr.c:78; the Airspy's "AIRSPY_SN:" is the analogy - held,
// not verified, for the RFOne).
constexpr const char* kFakeSerial = "HYDRASDR_SN:0123456789ABCDEF";
constexpr const char* kFakeTail = "0123456789ABCDEF";

// Points `src` at a fresh fake and returns a borrowed pointer to it.
template <typename Source>
FakeHydraSdrUsb* attachFake(Source& src, std::vector<cascade::usb::UsbDeviceInfo> devices = {}) {
    if (devices.empty()) { devices = oneHydraDevice(kFakeSerial); }
    auto owned = std::make_unique<FakeHydraSdrUsb>();
    FakeHydraSdrUsb* raw = owned.get();
    auto holder = std::make_shared<std::unique_ptr<cascade::usb::UsbDevice>>(std::move(owned));
    src.setTransportForTest(std::move(devices),
                            [holder](const std::string&, std::string& error) {
                                if (*holder == nullptr) { error = "fake: already handed out"; }
                                return std::move(*holder);
                            });
    return raw;
}

template <typename Fn>
bool waitFor(Fn fn, std::chrono::milliseconds bound) {
    const auto deadline = std::chrono::steady_clock::now() + bound;
    while (std::chrono::steady_clock::now() < deadline) {
        if (fn()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return fn();
}

// --- the vendor's own tables, retyped ---------------------------------------
//
// libhydrasdr/src/hydrasdr_rfone.c:89-94, verbatim and in that order, and the
// ranges at :26-30. THESE ARE NOT IMPORTED FROM THE DRIVER. If the driver's
// copy (airspy_protocol.hpp, ported from libairspy) ever differed from the
// vendor's, this is where it would show.
constexpr int kVendorTable = 22;  // RFONE_GAIN_TABLE_SIZE
constexpr std::uint8_t kVendorLinVga[kVendorTable] = {13, 12, 11, 11, 11, 11, 11, 10, 10, 10, 10,
                                                      10, 10, 10, 10, 10, 9,  8,  7,  6,  5,  4};
constexpr std::uint8_t kVendorLinMixer[kVendorTable] = {12, 12, 11, 9, 8, 7, 6, 6, 5, 0, 0,
                                                        1,  0,  0,  2, 2, 1, 1, 1, 1, 0, 0};
constexpr std::uint8_t kVendorLinLna[kVendorTable] = {14, 14, 14, 13, 12, 10, 9, 9, 8, 9, 8,
                                                      6,  5,  3,  1,  0,  0,  0, 0, 0, 0, 0};
constexpr std::uint8_t kVendorSenVga[kVendorTable] = {13, 12, 11, 10, 9, 8, 7, 6, 5, 5, 5,
                                                      5,  5,  4,  4,  4, 4, 4, 4, 4, 4, 4};
constexpr std::uint8_t kVendorSenMixer[kVendorTable] = {12, 12, 12, 12, 11, 10, 10, 9, 9, 8, 7,
                                                        4,  4,  4,  3,  2,  2,  1,  0, 0, 0, 0};
constexpr std::uint8_t kVendorSenLna[kVendorTable] = {14, 14, 14, 14, 14, 14, 14, 14, 14, 13, 12,
                                                      12, 9,  9,  8,  7,  6,  5,  3,  2,  1,  0};

// hydrasdr_rfone.c:253-268 (and :270-285): clamp, reverse, then mixer AGC off,
// LNA AGC off, VGA, MIXER, LNA.
struct VendorCombined {
    int lna;
    int mixer;
    int vga;
};
VendorCombined vendorCombined(int userIndex, bool linearity) {
    int v = userIndex;
    if (v >= kVendorTable) { v = kVendorTable - 1; }
    v = kVendorTable - 1 - v;
    return linearity ? VendorCombined{kVendorLinLna[v], kVendorLinMixer[v], kVendorLinVga[v]}
                     : VendorCombined{kVendorSenLna[v], kVendorSenMixer[v], kVendorSenVga[v]};
}

// --- the packer, written from the LAYOUT --------------------------------------

std::vector<std::uint8_t> packGroup(const std::uint16_t s[8]) {
    bool bits[96] = {false};
    for (int i = 0; i < 8; ++i) {
        for (int b = 0; b < 12; ++b) { bits[12 * i + b] = ((s[i] >> (11 - b)) & 1u) != 0u; }
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

// THE FIRMWARE'S OWN PACKER, as its source states it: rfone_fw m4/m4.c:120-137,
// the C comment above the assembler that replaced it. Eight 12-bit samples in,
// three 32-bit words out, each written little-endian on the wire.
std::vector<std::uint8_t> firmwarePack(const std::vector<std::uint16_t>& in) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 8 <= in.size(); i += 8) {
        const std::uint32_t t2 = in[i + 2];
        const std::uint32_t t5 = in[i + 5];
        const std::uint32_t w0 = (static_cast<std::uint32_t>(in[i]) << 20) |
                                 (static_cast<std::uint32_t>(in[i + 1]) << 8) | (t2 >> 4);
        const std::uint32_t w1 = ((t2 & 0xfu) << 28) | (static_cast<std::uint32_t>(in[i + 3]) << 16) |
                                 (static_cast<std::uint32_t>(in[i + 4]) << 4) | (t5 >> 8);
        const std::uint32_t w2 = ((t5 & 0xffu) << 24) | (static_cast<std::uint32_t>(in[i + 6]) << 12) |
                                 static_cast<std::uint32_t>(in[i + 7]);
        for (const std::uint32_t w : {w0, w1, w2}) {
            for (int b = 0; b < 4; ++b) { out.push_back(static_cast<std::uint8_t>((w >> (8 * b)) & 0xFFu)); }
        }
    }
    return out;
}

// --- the filter, evaluated from its own taps -----------------------------------

double halfBandMag(double w) {
    const std::array<float, 47> h = airspy::halfBandKernel();
    std::complex<double> acc(0.0, 0.0);
    for (std::size_t l = 0; l < h.size(); ++l) {
        acc += static_cast<double>(h[l]) *
               std::exp(std::complex<double>(0.0, -w * static_cast<double>(l)));
    }
    return std::abs(acc);
}

double dcRemoverMag(double w) {
    const std::complex<double> z = std::exp(std::complex<double>(0.0, -w));
    return std::abs((1.0 - z) / (1.0 - 0.99 * z));
}

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

// Every control transfer the driver sent, as a set of request numbers.
std::set<int> requestsSent(const std::vector<AirspyControlRecord>& c) {
    std::set<int> s;
    for (const AirspyControlRecord& r : c) { s.insert(r.request); }
    return s;
}

}  // namespace

int main() {
    std::printf("test_hydrasdr_source\n");

    // =====================================================================
    // 1. THE PROTOCOL NUMBERS, against the vendor's own enumeration.
    // =====================================================================
    {
        // libhydrasdr hydrasdr_commands.h:28-56, retyped. The NAMES differ from
        // libairspy's in two places (CLOCKGEN for SI5351C, RF_FRONTEND for
        // R820T); the NUMBERS are libairspy's, request for request, which is
        // the whole reason one driver serves both.
        struct Vendor {
            const char* name;
            int number;
            airspy::VendorRequest ours;
        };
        using R = airspy::VendorRequest;
        const Vendor vendor[] = {
            {"RECEIVER_MODE", 1, R::ReceiverMode},
            {"BOARD_ID_READ", 9, R::BoardIdRead},
            {"VERSION_STRING_READ", 10, R::VersionStringRead},
            {"BOARD_PARTID_SERIALNO_READ", 11, R::BoardPartIdSerialNoRead},
            {"SET_SAMPLERATE", 12, R::SetSampleRate},
            {"SET_FREQ", 13, R::SetFreq},
            {"SET_LNA_GAIN", 14, R::SetLnaGain},
            {"SET_MIXER_GAIN", 15, R::SetMixerGain},
            {"SET_VGA_GAIN", 16, R::SetVgaGain},
            {"SET_LNA_AGC", 17, R::SetLnaAgc},
            {"SET_MIXER_AGC", 18, R::SetMixerAgc},
            {"SET_RF_BIAS_CMD", 20, R::SetRfBiasCmd},
            {"GET_SAMPLERATES", 25, R::GetSampleRates},
            {"SET_PACKING", 26, R::SetPacking},
        };
        for (const Vendor& v : vendor) {
            if (airspy::requestByte(v.ours) != v.number) {
                std::printf("     %s: vendor %d, driver %d\n", v.name, v.number,
                            static_cast<int>(airspy::requestByte(v.ours)));
            }
            CHECK(airspy::requestByte(v.ours) == v.number);
        }
        // THE TWO THAT DIFFER FROM AN AIRSPY'S DRIVER: the bias tee is request
        // 20 (hydrasdr_shared.c:2175-2186) where the Airspy driver writes GPIO
        // 21, and SET_RF_PORT is 28 (hydrasdr_commands.h:56), which an Airspy has
        // no number for.
        CHECK(airspy::requestByte(hs::kBiasTeeRequest) == 20);
        CHECK(airspy::requestByte(hs::kSetRfPortRequest) == 28);
        CHECK(airspy::requestByte(airspy::VendorRequest::GpioWrite) == 21);
        // The five requests the published firmware does not implement
        // (hydrasdr_commands.h:58-62) start at 29, one past the firmware's own
        // HYDRASDR_CMD_MAX (rfone_fw common/hydrasdr_commands.h:39).
        CHECK(hs::kFirstUnpublishedRequest == 29);

        // The ids: 38AF:0001, which is NOT the Airspy's 1D50:60A1 (rfone_fw
        // m0/usb_descriptor.c:29-30; hydrasdr.c:64-65).
        CHECK(hs::kUsbVid == 0x38AF && hs::kUsbPid == 0x0001);
        CHECK(airspy::kUsbVid == 0x1D50 && airspy::kUsbPid == 0x60A1);
        CHECK(!(hs::kUsbVid == airspy::kUsbVid && hs::kUsbPid == airspy::kUsbPid));
        CHECK(hs::kLegacyUsbVid == airspy::kUsbVid && hs::kLegacyUsbPid == airspy::kUsbPid);
        CHECK(hs::kBoardId == 1);

        // SET_FREQ is eight bytes (hydrasdr_shared.c:1388-1415), an Airspy's four.
        CHECK(hs::kFreqPayloadBytes == 8);
        CHECK(airspy::kFreqPayloadBytes == 4);
        // The range (hydrasdr_rfone.c:33-34), and the Airspy's is narrower.
        CHECK_NEAR(hs::kMinFrequencyHz, 24.0e6, 0.5);
        CHECK_NEAR(hs::kMaxFrequencyHz, 1.8e9, 0.5);
        CHECK(airspy::kMaxFrequencyHz < hs::kMaxFrequencyHz);
        // Three ports named as the vendor names them (hydrasdr_rfone.c:431-456).
        CHECK(hs::kPortCount == 3);
        CHECK(std::string(hs::kPortNames[0]) == "ANT");
        CHECK(std::string(hs::kPortNames[1]) == "CABLE1");
        CHECK(std::string(hs::kPortNames[2]) == "CABLE2");
        CHECK(std::string(hs::kFirmwarePrefix) == "HydraSDR RF");
        CHECK(std::string(hs::kSerialPrefix) == "HYDRASDR SN:");

        // The register ranges are the vendor's (hydrasdr_rfone.c:26-28) and the
        // Airspy's: ONE set of numbers serves both drivers.
        CHECK(airspy::kLnaMaxIndex == 14);
        CHECK(airspy::kMixerMaxIndex == 15);
        CHECK(airspy::kVgaMaxIndex == 15);
        CHECK(airspy::kCombinedGainCount == kVendorTable);

        // 64-bit LITTLE-ENDIAN, the width a 32-bit encoder cannot carry.
        std::uint8_t b[8] = {0};
        airspy::encodeFreqWide(0x0000000123456789ULL, b, 8);
        CHECK(sameBytes("encodeFreqWide", std::vector<std::uint8_t>(b, b + 8),
                        {0x89, 0x67, 0x45, 0x23, 0x01, 0x00, 0x00, 0x00}));
        std::uint8_t b4[4] = {0};
        airspy::encodeFreqWide(0x0000000123456789ULL, b4, 4);
        CHECK(sameBytes("encodeFreqWide/4", std::vector<std::uint8_t>(b4, b4 + 4),
                        {0x89, 0x67, 0x45, 0x23}));
    }

    // =====================================================================
    // 2. THE PACKING the firmware makes, unpacked by the shared decoder.
    // =====================================================================
    {
        // The firmware's own pack() formula (m4/m4.c:120-137) and a packer
        // built from the layout agree, and both decode to the samples they
        // were given. Codes cover the ends (0, 4095), the mid-scale and a ramp
        // that walks the whole code space.
        std::vector<std::uint16_t> codes;
        codes.insert(codes.end(), {0, 4095, 2048, 2047, 1, 4094, 0x555, 0xAAA});
        for (std::size_t i = 0; i < 64; ++i) {
            codes.push_back(static_cast<std::uint16_t>((i * 317u + 1024u) & 0xFFFu));
        }
        const std::vector<std::uint8_t> fw = firmwarePack(codes);
        const std::vector<std::uint8_t> layout = packSamples(codes);
        CHECK(fw == layout);
        std::vector<std::uint16_t> back(codes.size());
        CHECK(airspy::unpackSamples(fw.data(), fw.size(), back.data(), back.size()) == codes.size());
        CHECK(back == codes);
        // ADC code 0 is -1 and 4095 is one step short of +1 (hydrasdr
        // iqconverter_lut.c:67-70: (code - 2048) / 2048).
        CHECK_NEAR(airspy::sampleToFloat(0), -1.0, 1e-9);
        CHECK_NEAR(airspy::sampleToFloat(2048), 0.0, 1e-9);
        CHECK_NEAR(airspy::sampleToFloat(4095), 2047.0 / 2048.0, 1e-9);
    }

    // =====================================================================
    // 3. THE VENDOR'S GAIN TABLES, retyped, against the driver's.
    // =====================================================================
    {
        // The tables the driver uses ARE the vendor's: the same twenty-two
        // entries in the same six rows. (One set of numbers, two drivers.)
        bool same = true;
        for (int i = 0; i < kVendorTable; ++i) {
            if (airspy::kLinearityVgaGains[i] != kVendorLinVga[i] ||
                airspy::kLinearityMixerGains[i] != kVendorLinMixer[i] ||
                airspy::kLinearityLnaGains[i] != kVendorLinLna[i] ||
                airspy::kSensitivityVgaGains[i] != kVendorSenVga[i] ||
                airspy::kSensitivityMixerGains[i] != kVendorSenMixer[i] ||
                airspy::kSensitivityLnaGains[i] != kVendorSenLna[i]) {
                std::printf("     gain table entry %d differs from the vendor's\n", i);
                same = false;
            }
        }
        CHECK(same);
        // And the mapping, at the ends and the middle: user index 0 is the
        // QUIETEST row (the vendor reverses at :258), 21 the loudest.
        for (const int idx : {0, 10, 21}) {
            const VendorCombined v = vendorCombined(idx, true);
            const airspy::CombinedGain g = airspy::combinedGainFor(idx, true);
            CHECK(g.lna == v.lna && g.mixer == v.mixer && g.vga == v.vga);
            const VendorCombined s = vendorCombined(idx, false);
            const airspy::CombinedGain gs = airspy::combinedGainFor(idx, false);
            CHECK(gs.lna == s.lna && gs.mixer == s.mixer && gs.vga == s.vga);
        }
        // A literal at each end, so a mistake in the retyped tables above is
        // not compared with itself: loudest = LNA 14, MIXER 12, VGA 13.
        const VendorCombined loud = vendorCombined(21, true);
        CHECK(loud.lna == 14 && loud.mixer == 12 && loud.vga == 13);
        const VendorCombined quiet = vendorCombined(0, true);
        CHECK(quiet.lna == 0 && quiet.mixer == 0 && quiet.vga == 4);
    }

    // =====================================================================
    // 4. ENUMERATION: the HydraSDR's ids and not the Airspy's.
    // =====================================================================
    {
        const std::vector<cascade::usb::UsbId> ids = cascade::source::hydraSdrUsbIds();
        CHECK(ids.size() == 1);
        CHECK(ids.size() == 1 && ids[0].vid == 0x38AF && ids[0].pid == 0x0001);
        // ...and the Airspy driver's list is untouched.
        const std::vector<cascade::usb::UsbId> airspyIds = cascade::source::airspyUsbIds();
        CHECK(airspyIds.size() == 1);
        CHECK(airspyIds.size() == 1 && airspyIds[0].vid == 0x1D50 && airspyIds[0].pid == 0x60A1);

        std::vector<cascade::usb::UsbDeviceInfo> devices;
        cascade::usb::UsbDeviceInfo hydra;
        hydra.vid = 0x38AF;
        hydra.pid = 0x0001;
        hydra.serial = "HYDRASDR_SN:AAAAAAAAAAAAAAAA";
        hydra.description = "HydraSDR RFOne";
        hydra.path = "p1";
        devices.push_back(hydra);
        cascade::usb::UsbDeviceInfo second = hydra;
        second.serial = "HYDRASDR_SN:BBBBBBBBBBBBBBBB";
        second.path = "p2";
        devices.push_back(second);
        cascade::usb::UsbDeviceInfo noSerial = hydra;
        noSerial.serial.clear();
        noSerial.description.clear();
        noSerial.path = "p3";
        devices.push_back(noSerial);
        cascade::usb::UsbDeviceInfo r2;
        r2.vid = 0x1D50;  // THE PROTOTYPE'S ID IS AN AIRSPY'S: not claimed here
        r2.pid = 0x60A1;
        r2.serial = "644866c83f1a51df";
        r2.description = "AIRSPY";
        r2.path = "p4";
        devices.push_back(r2);
        cascade::usb::UsbDeviceInfo hackrf;
        hackrf.vid = 0x1D50;
        hackrf.pid = 0x6089;
        hackrf.path = "p5";
        devices.push_back(hackrf);
        cascade::usb::UsbDeviceInfo rtl;
        rtl.vid = 0x0BDA;
        rtl.pid = 0x2838;
        rtl.path = "p6";
        devices.push_back(rtl);

        const std::vector<NativeDeviceInfo> found = cascade::source::hydraSdrDevicesFrom(devices);
        CHECK(found.size() == 3);
        const NativeDeviceInfo absent;
        const NativeDeviceInfo& f0 = found.size() > 0 ? found[0] : absent;
        const NativeDeviceInfo& f1 = found.size() > 1 ? found[1] : absent;
        const NativeDeviceInfo& f2 = found.size() > 2 ? found[2] : absent;
        CHECK(f0.driver == "hydrasdr");
        CHECK(f0.label == "HydraSDR RFOne (serial HYDRASDR_SN:AAAAAAAAAAAAAAAA)");
        CHECK(f0.args == "serial=HYDRASDR_SN:AAAAAAAAAAAAAAAA");
        CHECK(f1.driver == "hydrasdr");
        CHECK(f1.args == "serial=HYDRASDR_SN:BBBBBBBBBBBBBBBB");
        // A device with no serial is still openable, by its place in the walk.
        CHECK(f2.driver == "hydrasdr");
        // Nothing said which board it is, so nothing is claimed (open() names it
        // from the firmware).
        CHECK(f2.label == "HydraSDR");
        CHECK(f2.args == "index=2");

        // THE OTHER DIRECTION: the Airspy driver, given the very same list,
        // lists its R2 and none of the HydraSDRs.
        const std::vector<NativeDeviceInfo> airspyFound = cascade::source::airspyDevicesFrom(devices);
        CHECK(airspyFound.size() == 1);
        CHECK(airspyFound.size() == 1 && airspyFound[0].driver == "airspy");
        CHECK(airspyFound.size() == 1 && airspyFound[0].args == "serial=644866c83f1a51df");

        CHECK(cascade::source::hydraSdrModelFrom("HydraSDR RFOne") == "HydraSDR RFOne");
        CHECK(cascade::source::hydraSdrModelFrom("HydraSDR RFOne v1.0.0 2025-01-01") ==
              "HydraSDR RFOne");
        CHECK(cascade::source::hydraSdrModelFrom("") == "HydraSDR");
        CHECK(cascade::source::hydraSdrModelFrom("Bulk-In, Interface") == "HydraSDR");
    }

    // =====================================================================
    // 5. OPEN: the whole opening sequence, byte for byte.
    // =====================================================================
    {
        HydraSdrSource src;
        FakeHydraSdrUsb* fake = attachFake(src);
        const bool opened = src.open(std::string("serial=") + kFakeTail);
        if (!opened) { std::printf("     open() said: %s\n", src.lastError()); }
        CHECK(opened);
        CHECK(src.isOpen());
        CHECK(!src.faulted());
        CHECK(std::string(src.driverKey()) == "hydrasdr");
        CHECK(std::string(src.name()) == std::string("HydraSDR RFOne (serial ") + kFakeSerial + ")");
        CHECK(src.model() == "HydraSDR RFOne");

        // BOARD ID 1 (core.h:36-37), the firmware string without its padding,
        // the MCU's part id and unique id.
        CHECK(src.boardId() == 1);
        CHECK(src.firmwareVersion() == "HydraSDR RFOne v1.0.0 2025-01-01");
        CHECK(src.partIdSerialNo() == "0000000000000000644866c83f1a51df");

        const std::vector<AirspyControlRecord> c = fake->controls();
        if (c.size() != 16) {
            std::printf("     open() sent %zu control transfers, expected 16\n", c.size());
            for (std::size_t i = 0; i < c.size(); ++i) {
                std::printf("       [%zu] %s req %u value 0x%04x index 0x%04x data [%s]\n", i,
                            c[i].in ? "IN " : "OUT", static_cast<unsigned>(c[i].request),
                            static_cast<unsigned>(c[i].value), static_cast<unsigned>(c[i].index),
                            hexOf(c[i].data).c_str());
            }
        }
        CHECK(c.size() == 16);
        CHECK(isControl("open[0] board id", at(c, 0), true, 9, 0, 0));
        CHECK(isControl("open[1] version", at(c, 1), true, 10, 0, 0));
        CHECK(isControl("open[2] part/serial", at(c, 2), true, 11, 0, 0));
        // The version string came back padded to a multiple of four with NULs
        // (usb_req.c:359), and the driver's text stops at the first of them.
        CHECK(at(c, 1).data.size() % 4 == 0);
        CHECK(!at(c, 1).data.empty() && at(c, 1).data.back() == 0);
        CHECK(isControl("open[3] receiver off", at(c, 3), false, 1, 0, 0));
        // THREE RATES: the firmware's own configuration table, 10, 5 and 2.5
        // MS/s (hydrasdr_rfone_conf.c:161-226), read out of it by index.
        CHECK(isControl("open[4] rate count", at(c, 4), true, 25, 0, 0));
        CHECK(sameBytes("open[4] rate count", at(c, 4).data, {0x03, 0x00, 0x00, 0x00}));
        CHECK(isControl("open[5] rate list", at(c, 5), true, 25, 0, 3));
        CHECK(sameBytes("open[5] rate list", at(c, 5).data,
                        {0x80, 0x96, 0x98, 0x00, 0x40, 0x4B, 0x4C, 0x00, 0xA0, 0x25, 0x26, 0x00}));
        // 12-bit packing ON, state 1 (usb_req.c:408-444) - never 0 (the 16-bit
        // container) and never 2 (packing with a timestamp header).
        CHECK(isControl("open[6] packing on", at(c, 6), true, 26, 0, 1));
        CHECK(src.packingEnabled());
        CHECK(isControl("open[7] sample rate index 0", at(c, 7), true, 12, 0, 0));
        CHECK(fake->sampleRateConf.load() == 0);
        CHECK(fake->resetPipeCalls() == 1);
        CHECK(fake->lastResetEndpoint() == 0x81);
        // SET_FREQ: EIGHT bytes, 100 MHz little-endian.
        CHECK(isControl("open[8] set freq", at(c, 8), false, 13, 0, 0));
        CHECK(sameBytes("open[8] set freq", at(c, 8).data,
                        {0x00, 0xE1, 0xF5, 0x05, 0x00, 0x00, 0x00, 0x00}));
        CHECK(fake->lastFreqHz.load() == 100000000ULL);
        CHECK(isControl("open[9] mixer AGC off", at(c, 9), true, 18, 0, 0));
        CHECK(isControl("open[10] LNA AGC off", at(c, 10), true, 17, 0, 0));
        CHECK(isControl("open[11] LNA 8", at(c, 11), true, 14, 0, 8));
        CHECK(isControl("open[12] MIXER 8", at(c, 12), true, 15, 0, 8));
        CHECK(isControl("open[13] VGA 8", at(c, 13), true, 16, 0, 8));
        // THE BIAS TEE OFF, AS REQUEST 20: value 0, state in the index, no
        // payload (hydrasdr_shared.c:2175-2186) - and NOT the Airspy's GPIO 21.
        CHECK(isControl("open[14] bias tee off", at(c, 14), false, 20, 0, 0));
        CHECK(at(c, 14).data.empty());
        CHECK(!fake->biasTee.load());
        // ...and the receive port put to ANT, so the radio is in a KNOWN state
        // whatever the last application left (usb_req.c:897-913).
        CHECK(isControl("open[15] RF port ANT", at(c, 15), true, 28, 0, 0));
        CHECK(fake->rfPort.load() == 0);

        bool timeouts = true;
        for (const AirspyControlRecord& r : c) {
            if (r.timeoutMs != 500) { timeouts = false; }  // LIBUSB_CTRL_TIMEOUT_MS
        }
        CHECK(timeouts);

        // WHAT IT SENT AND WHAT IT DID NOT. Only requests the firmware
        // implements and this driver means: no GPIO, no flash, no tuner or
        // clock registers, no reset, nothing above 28.
        const std::set<int> sent = requestsSent(c);
        const std::set<int> allowed = {1, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 20, 25, 26, 28};
        bool onlyAllowed = true;
        for (const int r : sent) {
            if (allowed.count(r) == 0) {
                std::printf("     open() sent request %d\n", r);
                onlyAllowed = false;
            }
        }
        CHECK(onlyAllowed);
        CHECK(fake->stalled().empty());

        CHECK_NEAR(src.sampleRateHz(), 10.0e6, 0.5);
        const std::vector<double> rates = src.supportedSampleRatesHz();
        CHECK(rates.size() == 3);
        CHECK(rates.size() == 3 && std::fabs(rates[0] - 2.5e6) < 0.5);
        CHECK(rates.size() == 3 && std::fabs(rates[1] - 5.0e6) < 0.5);
        CHECK(rates.size() == 3 && std::fabs(rates[2] - 10.0e6) < 0.5);
        CHECK_NEAR(src.centerFrequencyHz(), 100.0e6, 0.5);
        CHECK_NEAR(src.gainDb("LNA"), 8.0, 1e-9);
        CHECK(!src.biasT());
        CHECK(src.autoGainSupported());
        CHECK(src.gainMode() == AirspySource::GainMode::Free);
        CHECK(src.gains().size() == 3);
        double lo = 0.0, hi = 0.0;
        CHECK(src.frequencyRangeHz(lo, hi));
        CHECK_NEAR(lo, 24.0e6, 0.5);
        CHECK_NEAR(hi, 1.8e9, 0.5);
        CHECK(src.selfPaced());
        src.closeDevice();
        CHECK(!src.isOpen());
        CHECK(std::string(src.name()) == "HydraSDR: (no device)");
    }

    // The Mini-style check: a board that says nothing in the bus description is
    // still recognised from its firmware version string.
    {
        HydraSdrSource src;
        attachFake(src, oneHydraDevice(kFakeSerial, ""));
        CHECK(src.open(""));
        CHECK(src.model() == "HydraSDR RFOne");
        src.closeDevice();
    }

    // =====================================================================
    // 6. OPENING WHAT IS NOT A HYDRASDR fails cleanly - and says what to do.
    // =====================================================================
    {
        // Only an Airspy on the bus: nothing for this driver, and the sentence
        // is the one the health classifier reads as "not bound".
        HydraSdrSource src;
        cascade::usb::UsbDeviceInfo r2;
        r2.vid = 0x1D50;
        r2.pid = 0x60A1;
        r2.serial = "644866c83f1a51df";
        r2.path = "airspy-path";
        attachFake(src, {r2});
        CHECK(!src.open(""));
        CHECK(!src.isOpen());
        const std::string said = src.lastError();
        CHECK(said.find("no HydraSDR found") != std::string::npos);
        CHECK(said.find("Airspy") == std::string::npos);
        CHECK(health::classifyRadioOpen("hydrasdr", said) == health::RadioReason::Bind);
    }
    {
        // A serial that is not on the bus.
        HydraSdrSource src;
        attachFake(src);
        CHECK(!src.open("serial=FFFFFFFFFFFFFFFF"));
        CHECK(std::string(src.lastError()).find("no HydraSDR with serial FFFFFFFFFFFFFFFF") !=
              std::string::npos);
        // Classified exactly as the Airspy driver's sentence for the same case is:
        // the words differ in the radio's name and in nothing else.
        CHECK(health::classifyRadioOpen("hydrasdr", src.lastError()) ==
              health::classifyRadioOpen("airspy", "no Airspy with serial FFFFFFFFFFFFFFFF is connected"));
    }
    {
        // A device on the HydraSDR's id whose firmware does not say HydraSDR is
        // refused, as the host library refuses it (hydrasdr.c:187).
        HydraSdrSource src;
        FakeHydraSdrUsb* fake = attachFake(src);
        fake->firmwareVersion = "AirSpy NOS v1.0.0-rc10-6-g4008185 2020-05-08";
        // The fake goes with the refused open; its transcript does not.
        const std::shared_ptr<cascade::test::AirspyTranscript> tx = fake->transcript();
        CHECK(!src.open(""));
        CHECK(!src.isOpen());
        CHECK(std::string(src.lastError()).find("HydraSDR RF") != std::string::npos);
        // Nothing was programmed on a device that is not ours.
        const std::set<int> sent = requestsSent(tx->snapshot());
        CHECK(sent == (std::set<int>{9, 10}));
    }
    {
        // No rate list: the vendor's host has no fallback list either (its
        // count is simply zero), and a radio with no rate to run at is not
        // opened. NOT condemned - nothing failed on the wire.
        HydraSdrSource src;
        FakeHydraSdrUsb* fake = attachFake(src);
        fake->forcedRateCount = 0;
        CHECK(!src.open(""));
        CHECK(!src.isOpen());
        CHECK(!src.faulted());
        CHECK(std::string(src.lastError()).find("sample rate") != std::string::npos);
        // The same fake, an AIRSPY: libairspy's own fallback list, as before.
        AirspySource air;
        auto owned = std::make_unique<FakeAirspyUsb>();
        owned->forcedRateCount = 0;
        auto holder =
            std::make_shared<std::unique_ptr<cascade::usb::UsbDevice>>(std::move(owned));
        cascade::usb::UsbDeviceInfo d;
        d.vid = 0x1D50;
        d.pid = 0x60A1;
        d.path = "p";
        d.serial = "644866c83f1a51df";
        air.setTransportForTest({d}, [holder](const std::string&, std::string&) {
            return std::move(*holder);
        });
        CHECK(air.open(""));
        CHECK(air.supportedSampleRatesHz().size() == 2);
        air.closeDevice();
    }
    {
        // A firmware that stalls the rate list is a dead device, as any
        // control transfer that fails is.
        HydraSdrSource src;
        FakeHydraSdrUsb* fake = attachFake(src);
        fake->failingRequests = {25};
        CHECK(!src.open(""));
        CHECK(src.deviceDead());
    }

    // =====================================================================
    // 7. RATES AND TUNES, each as the exact transfer it becomes.
    // =====================================================================
    {
        HydraSdrSource src;
        FakeHydraSdrUsb* fake = attachFake(src);
        CHECK(src.open(""));
        fake->clearControls();

        // The rate is programmed BY THE FIRMWARE'S INDEX: 5 MS/s is the
        // second row of its table (hydrasdr_shared.c:1709-1750, usb_req.c:460).
        CHECK(src.setSampleRateHz(5.0e6));
        {
            const std::vector<AirspyControlRecord> c = fake->controls();
            CHECK(c.size() == 1);
            CHECK(isControl("rate 5 MS/s", at(c, 0), true, 12, 0, 1));
        }
        CHECK_NEAR(src.sampleRateHz(), 5.0e6, 0.5);
        fake->clearControls();
        CHECK(src.setSampleRateHz(2.5e6));
        CHECK(isControl("rate 2.5 MS/s", at(fake->controls(), 0), true, 12, 0, 2));
        // Coerced to the nearest the DEVICE LISTED, never a rate it did not.
        CHECK(src.setSampleRateHz(9.0e6));
        CHECK_NEAR(src.sampleRateHz(), 10.0e6, 0.5);
        CHECK(src.setSampleRateHz(3.0e6));
        CHECK_NEAR(src.sampleRateHz(), 2.5e6, 0.5);
        CHECK(!src.setSampleRateHz(0.0));
        CHECK(!src.setSampleRateHz(-1.0));

        // TUNE: eight bytes, little-endian, in Hz.
        fake->clearControls();
        CHECK(src.setCenterFrequencyHz(433.92e6));
        {
            const std::vector<AirspyControlRecord> c = fake->controls();
            CHECK(c.size() == 1);
            CHECK(isControl("tune 433.92", at(c, 0), false, 13, 0, 0));
            // The payload is the Hz value little-endian, computed by shifting
            // so no byte is typed.
            const std::uint64_t want = 433920000ULL;
            std::vector<int> bytes;
            for (int b = 0; b < 8; ++b) { bytes.push_back(static_cast<int>((want >> (8 * b)) & 0xFF)); }
            CHECK(sameBytes("tune 433.92", at(c, 0).data, bytes));
        }
        CHECK(fake->lastFreqHz.load() == 433920000ULL);
        CHECK_NEAR(src.centerFrequencyHz(), 433.92e6, 0.5);

        // THE TOP OF THE RANGE IS THE HYDRASDR'S, 1.8 GHz - an Airspy's driver
        // would refuse it - and just outside is refused with the range said.
        CHECK(src.setCenterFrequencyHz(1.8e9));
        CHECK(fake->lastFreqHz.load() == 1800000000ULL);
        CHECK(src.setCenterFrequencyHz(24.0e6));
        CHECK(fake->lastFreqHz.load() == 24000000ULL);
        const int setsBefore = fake->freqSets.load();
        CHECK(!src.setCenterFrequencyHz(1.8e9 + 1.0e6));
        CHECK(!src.setCenterFrequencyHz(23.0e6));
        CHECK(fake->freqSets.load() == setsBefore);  // nothing was sent for either
        const std::string said = src.lastError();
        CHECK(said.find("HydraSDR tunes 24 MHz to 1800 MHz") != std::string::npos);
        CHECK(!src.faulted());   // a refused tune is not a fault
        src.closeDevice();
    }

    // =====================================================================
    // 8. GAINS: the ends and the middle, each as its exact transfers.
    // =====================================================================
    {
        HydraSdrSource src;
        FakeHydraSdrUsb* fake = attachFake(src);
        CHECK(src.open(""));

        // FREE MODE: three registers, clamped at both ends. LNA tops out at 14
        // (RFONE_LNA_MAX_GAIN), the other two at 15 (hydrasdr_rfone.c:26-28).
        struct Free {
            const char* name;
            int request;
            int max;
        };
        for (const Free f : {Free{"LNA", 14, 14}, Free{"MIXER", 15, 15}, Free{"VGA", 16, 15}}) {
            for (const int want : {0, f.max / 2, f.max}) {
                fake->clearControls();
                CHECK(src.setGainDb(f.name, want));
                CHECK(isControl(f.name, at(fake->controls(), 0), true, f.request, 0, want));
            }
            fake->clearControls();
            CHECK(src.setGainDb(f.name, 99.0));  // clamped, not refused
            CHECK(isControl(f.name, at(fake->controls(), 0), true, f.request, 0, f.max));
            fake->clearControls();
            CHECK(src.setGainDb(f.name, -5.0));
            CHECK(isControl(f.name, at(fake->controls(), 0), true, f.request, 0, 0));
        }
        CHECK(!src.setGainDb("NOSUCH", 1.0));

        // THE TWO COMBINED TABLES, at the quietest end, the middle and the loud
        // end, in the vendor's order (hydrasdr_rfone.c:261-265): mixer AGC off,
        // LNA AGC off, VGA, MIXER, LNA.
        for (const bool linearity : {true, false}) {
            for (const int idx : {0, 10, 21}) {
                fake->clearControls();
                CHECK(src.setGainDb(linearity ? "LINEARITY" : "SENSITIVITY", idx));
                const std::vector<AirspyControlRecord> c = fake->controls();
                const VendorCombined v = vendorCombined(idx, linearity);
                CHECK(c.size() == 5);
                CHECK(isControl("table: mixer AGC off", at(c, 0), true, 18, 0, 0));
                CHECK(isControl("table: LNA AGC off", at(c, 1), true, 17, 0, 0));
                CHECK(isControl("table: VGA", at(c, 2), true, 16, 0, v.vga));
                CHECK(isControl("table: MIXER", at(c, 3), true, 15, 0, v.mixer));
                CHECK(isControl("table: LNA", at(c, 4), true, 14, 0, v.lna));
            }
        }
        // Beyond the table's end the vendor clamps to the last row
        // (hydrasdr_rfone.c:255-257): 99 is the loud end.
        fake->clearControls();
        CHECK(src.setGainDb("LINEARITY", 99.0));
        {
            const VendorCombined v = vendorCombined(99, true);
            CHECK(isControl("table clamp: VGA", at(fake->controls(), 2), true, 16, 0, v.vga));
            CHECK(v.lna == 14 && v.mixer == 12 && v.vga == 13);
        }
        CHECK(src.gainMode() == AirspySource::GainMode::Linearity);
        CHECK(src.gains().size() == 1 && src.gains()[0].name == "LINEARITY");

        // THE AGCs, one per stage, in Free mode (hydrasdr_shared.c:2508-2546).
        CHECK(src.setGainMode(AirspySource::GainMode::Free));
        fake->clearControls();
        CHECK(src.setLnaAgc(true));
        CHECK(isControl("LNA AGC on", at(fake->controls(), 0), true, 17, 0, 1));
        fake->clearControls();
        CHECK(src.setMixerAgc(true));
        CHECK(isControl("mixer AGC on", at(fake->controls(), 0), true, 18, 0, 1));
        CHECK(src.autoGain());
        CHECK(src.setAutoGain(false));
        CHECK(!src.autoGain());
        CHECK(fake->stalled().empty());
        src.closeDevice();
    }

    // =====================================================================
    // 9. THE BIAS TEE: request 20, and not a GPIO write.
    // =====================================================================
    {
        HydraSdrSource src;
        FakeHydraSdrUsb* fake = attachFake(src);
        CHECK(src.open(""));
        fake->clearControls();
        CHECK(src.setBiasT(true));
        {
            const std::vector<AirspyControlRecord> c = fake->controls();
            CHECK(c.size() == 1);
            CHECK(isControl("bias on", at(c, 0), false, 20, 0, 1));
            CHECK(at(c, 0).data.empty());
        }
        CHECK(src.biasT());
        CHECK(fake->biasTee.load());
        fake->clearControls();
        CHECK(src.setBiasT(false));
        CHECK(isControl("bias off", at(fake->controls(), 0), false, 20, 0, 0));
        CHECK(!src.biasT());
        CHECK(!fake->biasTee.load());
        // Never GPIO_WRITE (21) from this driver, whatever the pin it reaches.
        CHECK(requestsSent(fake->controls()).count(21) == 0);

        // LEFT OFF ON THE WAY OUT: a bias tee on at close is 4.5 V into the next
        // program's antenna (hydrasdr_shared.c:3035-3043 does the same).
        CHECK(src.setBiasT(true));
        const std::shared_ptr<cascade::test::AirspyTranscript> tx = fake->transcript();
        src.closeDevice();
        const std::vector<AirspyControlRecord> after = tx->snapshot();
        bool offLast = false;
        for (std::size_t i = after.size(); i > 0; --i) {
            if (after[i - 1].request == 20) {
                offLast = after[i - 1].index == 0 && !after[i - 1].in;
                break;
            }
        }
        CHECK(offLast);
    }

    // =====================================================================
    // 10. THE THREE RECEIVE PORTS: what an Airspy does not have.
    // =====================================================================
    {
        HydraSdrSource src;
        FakeHydraSdrUsb* fake = attachFake(src);
        CHECK(src.open(""));
        CHECK(src.antennas() == std::vector<std::string>({"ANT", "CABLE1", "CABLE2"}));
        CHECK(src.antenna() == "ANT");
        fake->clearControls();
        CHECK(src.setAntenna("CABLE2"));
        {
            const std::vector<AirspyControlRecord> c = fake->controls();
            CHECK(c.size() == 1);
            CHECK(isControl("port CABLE2", at(c, 0), true, 28, 0, 2));
        }
        CHECK(src.antenna() == "CABLE2");
        CHECK(fake->rfPort.load() == 2);
        fake->clearControls();
        CHECK(src.setAntenna("CABLE1"));
        CHECK(isControl("port CABLE1", at(fake->controls(), 0), true, 28, 0, 1));
        CHECK(src.antenna() == "CABLE1");
        CHECK(src.setAntenna("ANT"));
        CHECK(src.antenna() == "ANT");
        // A port that is not one of the three is refused WITHOUT a transfer.
        fake->clearControls();
        CHECK(!src.setAntenna("RX"));
        CHECK(!src.setAntenna(""));
        CHECK(fake->controlCount() == 0);
        CHECK(!src.faulted());
        // The bias tee is not a port (it stays out of the list).
        const std::vector<std::string> ports = src.antennas();
        CHECK(std::find(ports.begin(), ports.end(), "BIAS") == ports.end());
        // A RESTART OF THE STREAM KEEPS THE PORT (nothing resets it).
        CHECK(src.setAntenna("CABLE2"));
        CHECK(src.start());
        src.stop();
        CHECK(src.antenna() == "CABLE2");
        CHECK(fake->rfPort.load() == 2);
        src.closeDevice();
    }
    {
        // A firmware that stalls a port change leaves the readback where it
        // was, and - as any failed control transfer does - condemns the device.
        HydraSdrSource src;
        FakeHydraSdrUsb* fake = attachFake(src);
        CHECK(src.open(""));
        fake->refusePorts.store(true);
        CHECK(!src.setAntenna("CABLE1"));
        CHECK(src.antenna() == "ANT");
        CHECK(src.deviceDead());
        src.closeDevice();
    }

    // =====================================================================
    // 11. START AND STOP: the bulk geometry is the packed stream's.
    // =====================================================================
    {
        HydraSdrSource src;
        FakeHydraSdrUsb* fake = attachFake(src);
        CHECK(src.open(""));
        fake->clearControls();
        CHECK(src.start());
        CHECK(isControl("start: OFF", at(fake->controls(), 0), false, 1, 0, 0));
        CHECK(isControl("start: RX", at(fake->controls(), 1), false, 1, 1, 0));
        CHECK(fake->lastBulkEndpoint() == 0x81);          // RFONE_RX_ENDPOINT 1 | IN
        CHECK(fake->lastBulkBufferBytes() == 147456);      // PACKED_BUFFER_SIZE 6144 * 24
        CHECK(fake->lastBulkBufferCount() == 16);          // RFONE_TRANSFER_COUNT
        src.stop();
        CHECK(isControl("stop: OFF", at(fake->controls(), 2), false, 1, 0, 0));
        src.closeDevice();
        // Across a whole session, packing was only ever switched ON (state 1).
        bool onlyOn = true;
        for (const AirspyControlRecord& r : fake->controls()) {
            if (r.request == 26 && r.index != 1) { onlyOn = false; }
        }
        CHECK(onlyOn);
        CHECK(fake->packingState.load() == 1);
    }

    // =====================================================================
    // 12. STREAMING: a real tone, packed as the firmware packs it, comes out
    //     at the frequency, the amplitude and the side of zero theory says.
    // =====================================================================
    {
        // A 20 MS/s REAL ADC stream is the "10 MS/s" rate the device lists.
        const double fsReal = 20.0e6;
        const double fsComplex = fsReal / 2.0;
        const double A = 0.5;
        const std::size_t L = 4096;
        const std::size_t start = 2048;
        const std::size_t N = 2 * (start + L);
        struct ToneCase {
            double f;
            double phase;
            int bin;
            const char* label;
        };
        const ToneCase tones[] = {
            {fsReal / 8.0, 0.3, 1024, "fs/8 -> +fs/8"},
            {3.0 * fsReal / 8.0, 0.0, -1024, "3fs/8 -> -fs/8 (the sign of the rotation)"},
        };
        for (const ToneCase& t : tones) {
            std::vector<std::uint16_t> codes(N);
            for (std::size_t n = 0; n < N; ++n) {
                const double x =
                    A * std::cos(2.0 * kPi * t.f * static_cast<double>(n) / fsReal + t.phase);
                codes[n] = static_cast<std::uint16_t>(std::lround(2048.0 + 2048.0 * x));
            }
            HydraSdrSource src;
            FakeHydraSdrUsb* fake = attachFake(src);
            CHECK(src.open(""));
            CHECK(src.setSampleRateHz(10.0e6));
            fake->queueBulk(firmwarePack(codes));   // THE FIRMWARE'S packer
            CHECK(src.start());
            std::vector<std::complex<float>> got;
            const std::size_t want = N / 2;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (got.size() < want && std::chrono::steady_clock::now() < deadline) {
                std::complex<float> chunk[512];
                const std::size_t n = src.read(chunk, 512);
                for (std::size_t i = 0; i < n; ++i) { got.push_back(chunk[i]); }
            }
            CHECK(got.size() == want);
            if (got.size() == want) {
                std::vector<std::complex<double>> blk(L);
                for (std::size_t n = 0; n < L; ++n) {
                    blk[n] = std::complex<double>(got[start + n].real(), got[start + n].imag());
                }
                const double fOut = fsReal / 4.0 - t.f;
                CHECK_NEAR(fOut / fsComplex * static_cast<double>(L), static_cast<double>(t.bin),
                           1e-9);
                const double hbPass = halfBandMag(2.0 * kPi * (fsReal / 4.0 - t.f) / fsReal);
                const double dc = dcRemoverMag(2.0 * kPi * t.f / fsReal);
                const double wantMain = A / 2.0 * hbPass * dc;
                const double gotMain = std::abs(binAt(blk, t.bin));
                const double gotImage = std::abs(binAt(blk, -t.bin));
                if (!(std::fabs(gotMain - wantMain) < 2.0e-4)) {
                    std::printf("     %s: got %.9f at bin %+d, expected %.9f\n", t.label, gotMain,
                                t.bin, wantMain);
                }
                CHECK_NEAR(gotMain, wantMain, 2.0e-4);
                if (!(gotImage < gotMain / 50.0)) {
                    std::printf("     %s: the mirror bin carries %.9f against %.9f\n", t.label,
                                gotImage, gotMain);
                }
                CHECK(gotImage < gotMain / 50.0);
            }
            CHECK(src.droppedTransfers() == 0);
            src.closeDevice();
        }
    }

    // =====================================================================
    // 13. THE DEVICE GOES: the reader leaves, and says why.
    // =====================================================================
    {
        const unsigned long long abandonedBefore = AirspySource::readersAbandoned();
        HydraSdrSource src;
        FakeHydraSdrUsb* fake = attachFake(src);
        CHECK(src.open(""));
        fake->onExhausted.store(FakeAirspyUsb::Exhausted::DeviceGone);
        CHECK(src.start());
        CHECK(waitFor([&src] { return src.faulted(); }, std::chrono::milliseconds(2000)));
        CHECK(src.faulted());
        CHECK(src.deviceDead());
        CHECK(src.faultedWhile() == "reading samples");
        CHECK(std::string(src.lastError()).find("stopped answering") != std::string::npos);
        std::complex<float> buf[8];
        CHECK(src.read(buf, 8) == 0);
        const auto t0 = std::chrono::steady_clock::now();
        src.stop();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t0).count();
        CHECK(elapsed < 500);
        CHECK(AirspySource::readersAbandoned() == abandonedBefore);
        // A dead device refuses further control rather than sending into it.
        fake->clearControls();
        CHECK(!src.setCenterFrequencyHz(100.0e6));
        CHECK(!src.setBiasT(true));
        CHECK(!src.setAntenna("CABLE1"));
        CHECK(fake->controlCount() == 0);
        src.closeDevice();
    }
    {
        // A control request that fails mid-session condemns it as well - the
        // frequency, here - and the stream is not restarted behind its back.
        HydraSdrSource src;
        FakeHydraSdrUsb* fake = attachFake(src);
        CHECK(src.open(""));
        fake->failingRequests = {13};
        CHECK(!src.setCenterFrequencyHz(100.5e6));
        CHECK(src.deviceDead());
        CHECK(std::string(src.lastError()).find("stopped answering") != std::string::npos);
        src.closeDevice();
    }

    // =====================================================================
    // 14. SERIAL AND IDENTITY: matched the way the Airspy's are, and kept out of
    //     what leaves the machine.
    // =====================================================================
    {
        // The sixteen hex digits a user reads off another tool find the radio
        // by suffix, case-insensitively; a different sixteen do not.
        for (const char* want : {"0123456789ABCDEF", "0123456789abcdef", "HYDRASDR_SN:0123456789ABCDEF"}) {
            HydraSdrSource src;
            attachFake(src);
            CHECK(src.open(std::string("serial=") + want));
            src.closeDevice();
        }
        {
            HydraSdrSource src;
            attachFake(src);
            CHECK(!src.open("serial=0123456789ABCDE0"));
        }
        // Two radios: index=1 is the second.
        {
            HydraSdrSource src;
            std::vector<cascade::usb::UsbDeviceInfo> two = oneHydraDevice("HYDRASDR_SN:1111111111111111");
            std::vector<cascade::usb::UsbDeviceInfo> more = oneHydraDevice("HYDRASDR_SN:2222222222222222");
            more[0].path = "second";
            two.push_back(more[0]);
            attachFake(src, two);
            CHECK(src.open("index=1"));
            CHECK(std::string(src.name()).find("2222222222222222") != std::string::npos);
            src.closeDevice();
        }
        // A serial in a line a report carries is stripped by the existing rule,
        // in the forms the driver writes it.
        CHECK(cascade::core::scrubUploadLine(
                  "opened HydraSDR RFOne (serial HYDRASDR_SN:0123456789ABCDEF)") ==
              "opened HydraSDR RFOne (serial <stripped>)");
        CHECK(cascade::core::scrubUploadLine("found HYDRASDR_SN:0123456789ABCDEF on the bus") ==
              "found HYDRASDR_SN:<stripped> on the bus");
        const std::string instanceLine = cascade::core::scrubUploadLine(
            "USB\\VID_38AF&PID_0001\\HYDRASDR_SN:0123456789ABCDEF");
        std::printf("     instance id scrubbed to: %s\n", instanceLine.c_str());
        CHECK(instanceLine.find("0123456789ABCDEF") == std::string::npos);
        CHECK(instanceLine.find("VID_38AF&PID_0001") != std::string::npos);  // the kind stays
    }

    // =====================================================================
    // 15. THE HEALTH EVENTS, through the real driver.
    // =====================================================================
    {
        // The word is in the vocabulary, and every reason makes a legal token.
        CHECK(health::driverWord("hydrasdr") == "hydrasdr");
        CHECK(health::tokenRadioOpen("hydrasdr") == "radio_open.hydrasdr");
        CHECK(health::tokenRadioData("hydrasdr") == "radio_data.hydrasdr");
        CHECK(health::validToken("radio_open.hydrasdr"));
        for (int r = 0; r <= static_cast<int>(health::RadioReason::Other); ++r) {
            const std::string t =
                health::tokenRadioFail("hydrasdr", static_cast<health::RadioReason>(r));
            CHECK(health::validToken(t));
            CHECK(t.rfind("radio_fail.hydrasdr.", 0) == 0);
        }
        // The driver's own key is the word, so the counting sites that pass the
        // source kind (which is the driver key) count under it.
        HydraSdrSource src;
        CHECK(health::driverWord(src.driverKey()) == "hydrasdr");
        CHECK(health::driverWord("hydrasdr") != "other");

        // THE REAL REFUSAL, on this machine's own USB enumeration: a serial
        // that is not there. (test_health_paths.cpp does the same for every
        // other native driver, and now for this one.)
        HydraSdrSource real;
        CHECK(!real.open("serial=HEALTHTESTNOSUCHRADIO"));
        const std::string said = real.lastError();
        CHECK(!said.empty());
        const health::RadioReason why = health::classifyRadioOpen("hydrasdr", said);
        CHECK(why == health::RadioReason::Bind || why == health::RadioReason::Absent);
        auto g = health::globalLedger();
        g->reset();
        g->arm("", cascade::core::newInstallId(), false);
        health::noteRadioFail("hydrasdr", said);
        const std::string token = g->counts().empty() ? std::string() : g->counts().begin()->first;
        CHECK(g->counts().size() == 1);
        CHECK(token == "radio_fail.hydrasdr.bind" || token == "radio_fail.hydrasdr.absent");
        CHECK(token.find("HEALTHTEST") == std::string::npos);
        g->reset();
    }

    // =====================================================================
    // 16. THE AIRSPY DRIVER, given an Airspy, is still libairspy's.
    // =====================================================================
    {
        AirspySource air;
        auto owned = std::make_unique<FakeAirspyUsb>();
        FakeAirspyUsb* fake = owned.get();
        auto holder =
            std::make_shared<std::unique_ptr<cascade::usb::UsbDevice>>(std::move(owned));
        cascade::usb::UsbDeviceInfo d;
        d.vid = 0x1D50;
        d.pid = 0x60A1;
        d.path = "p";
        d.serial = "644866c83f1a51df";
        d.description = "AIRSPY";
        air.setTransportForTest({d}, [holder](const std::string&, std::string&) {
            return std::move(*holder);
        });
        CHECK(air.open(""));
        CHECK(std::string(air.driverKey()) == "airspy");
        CHECK(std::string(air.name()) == "Airspy: Airspy R2 (serial 644866c83f1a51df)");
        const std::vector<AirspyControlRecord> c = fake->controls();
        CHECK(c.size() == 15);                       // no SET_RF_PORT, no port at all
        CHECK(isControl("airspy freq", at(c, 8), false, 13, 0, 0));
        CHECK(at(c, 8).data.size() == 4);            // four bytes, not eight
        CHECK(isControl("airspy bias off", at(c, 14), false, 21, 0, 0x2D));   // GPIO, not 20
        CHECK(air.antennas() == std::vector<std::string>({"RX"}));
        CHECK(air.setAntenna("RX"));
        CHECK(!air.setAntenna("ANT"));
        CHECK(air.setCenterFrequencyHz(1.75e9));
        CHECK(!air.setCenterFrequencyHz(1.76e9));    // the Airspy's range, not 1.8 GHz
        CHECK(air.setBiasT(true));
        CHECK(requestsSent(fake->controls()).count(20) == 0);
        CHECK(requestsSent(fake->controls()).count(28) == 0);
        air.closeDevice();
        // An Airspy driver never opens a HydraSDR, and the other way about.
        AirspySource air2;
        attachFake(air2, oneHydraDevice(kFakeSerial));
        CHECK(!air2.open(""));
        CHECK(std::string(air2.lastError()).find("no Airspy found") != std::string::npos);
    }

    return testSummary("test_hydrasdr_source");
}
