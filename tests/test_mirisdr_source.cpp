// test_mirisdr_source.cpp - the Mirics MSi2500 / MSi001 driver, proven byte
// for byte against a fake that records every transfer.
//
// WHERE THE EXPECTATIONS COME FROM. There is no Mirics device on this bench,
// so an expectation invented here would only prove this file agrees with
// itself. Every register value below was PRODUCED by transcribing the
// reference's own bodies - hard.c mirisdr_set_hard, soft.c mirisdr_set_soft,
// gain.c mirisdr_set_gain, adc.c mirisdr_adc_init, reg.c mirisdr_write_reg -
// into a throwaway C program, compiling it and printing the answer for each
// case pinned here. Reading a twenty-four-bit register off by eye would have
// been the same class of mistake as answering a datasheet question from
// memory. Each block below names the function its numbers came from and
// quotes the line the program printed.
//
// WHAT THIS SUITE CANNOT PROVE, said once here rather than hedged everywhere.
// The reference pack read for this work did not include the files that unpack
// the bit-packed sample formats, so the twelve- and ten-bit BIT ORDERS in
// msi2500.hpp are derived from its size arithmetic rather than read off. The
// unpacking blocks below therefore prove that the driver implements the layout
// the header documents - hand-packed bytes in, known samples out - and not
// that the layout is the silicon's. The sixteen- and eight-bit formats leave
// nothing to order and are not in doubt. The first Mirics device on a bench
// settles the other two.
//
// THE FIVE DELIBERATE BREAKS. Every block that matters was watched go RED
// against a broken driver before it was trusted green:
//   - the register encoding transposed (value and index swapped)
//   - the band plan's row chosen as plan[i] instead of plan[i-1]
//   - the gain write dropped from the end of a tune
//   - a dropped bulk block (the reader unpacking every second block)
//   - a hung reader (the bounded join replaced by a plain one)
// The report for this change-set carries the failing line each of them
// produced.
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

#include "mirisdr_fake_usb.hpp"
#include "source/mirisdr_source.hpp"
#include "source/msi2500.hpp"
#include "source/tuner_msi001.hpp"
#include "test_check.hpp"

using cascade::source::MiriSdrSource;
using cascade::source::NativeDeviceInfo;
using cascade::test::FakeMiriSdrUsb;
using cascade::test::MiriControlRecord;
namespace msi2500 = cascade::source::msi2500;
namespace msi001 = cascade::source::msi001;

namespace {

// --- helpers ---------------------------------------------------------------

// Bounds-safe indexing. A `CHECK(v.size() == n)` followed by `v[i]` is an
// out-of-bounds read in exactly the run that has something to report - the
// harness records a failed check and carries on, so the crash lands instead of
// the message. (mayhem-b200, 2026-08-13; the same trap cost a whole session.)
const MiriControlRecord& at(const std::vector<MiriControlRecord>& v, std::size_t i) {
    static const MiriControlRecord kAbsent{};
    return i < v.size() ? v[i] : kAbsent;
}

// One register write, whole. Compared as a unit rather than as five separate
// CHECKs so a failure names the transfer that is wrong instead of leaving five
// lines to be reassembled by hand.
bool isRegWrite(const char* label, const MiriControlRecord& r, int reg, std::uint32_t val) {
    const bool ok = !r.in && r.requestType == 0x42 && r.request == 0x41 &&
                    r.reg() == static_cast<std::uint8_t>(reg) && r.regValue() == val &&
                    r.payloadBytes == 0;
    if (!ok) {
        std::printf(
            "     %s: got %s type 0x%02x request 0x%02x reg 0x%02x value 0x%06x (wValue 0x%04x "
            "wIndex 0x%04x, %u payload bytes); want OUT type 0x42 request 0x41 reg 0x%02x value "
            "0x%06x\n",
            label, r.in ? "IN" : "OUT", static_cast<unsigned>(r.requestType),
            static_cast<unsigned>(r.request), static_cast<unsigned>(r.reg()), r.regValue(),
            static_cast<unsigned>(r.value), static_cast<unsigned>(r.index),
            static_cast<unsigned>(r.payloadBytes), static_cast<unsigned>(reg), val);
    }
    return ok;
}

bool isCommand(const char* label, const MiriControlRecord& r, int request) {
    const bool ok = !r.in && r.requestType == 0x42 &&
                    r.request == static_cast<std::uint8_t>(request) && r.value == 0 &&
                    r.index == 0 && r.payloadBytes == 0;
    if (!ok) {
        std::printf(
            "     %s: got %s type 0x%02x request 0x%02x value 0x%04x index 0x%04x; want OUT type "
            "0x42 request 0x%02x value 0 index 0\n",
            label, r.in ? "IN" : "OUT", static_cast<unsigned>(r.requestType),
            static_cast<unsigned>(r.request), static_cast<unsigned>(r.value),
            static_cast<unsigned>(r.index), static_cast<unsigned>(request));
    }
    return ok;
}

// A whole expected sequence, in order, starting at `from`. Reports the first
// transfer that does not match and stops - a sequence that has gone wrong at
// step three produces one useful line, not fifteen.
struct ExpectedWrite {
    int reg;
    std::uint32_t val;
};

bool isRegSequence(const char* label, const std::vector<MiriControlRecord>& v, std::size_t from,
                   const std::vector<ExpectedWrite>& want) {
    if (v.size() < from + want.size()) {
        std::printf("     %s: only %zu transfers recorded, wanted %zu from index %zu\n", label,
                    v.size(), want.size(), from);
        return false;
    }
    for (std::size_t i = 0; i < want.size(); ++i) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s step %zu", label, i);
        if (!isRegWrite(buf, at(v, from + i), want[i].reg, want[i].val)) { return false; }
    }
    return true;
}

std::vector<cascade::usb::UsbDeviceInfo> oneFakeDevice(const std::string& serial = std::string(),
                                                       std::uint16_t vid = 0x1DF7,
                                                       std::uint16_t pid = 0x2500) {
    cascade::usb::UsbDeviceInfo d;
    d.vid = vid;
    d.pid = pid;
    d.path = "\\\\?\\usb#vid_1df7&pid_2500#fake#{a5dcbf10}";
    d.serial = serial;
    d.description = "MSi2500";
    return {d};
}

// Points `src` at a fresh fake and returns a borrowed pointer to it. The fake
// is handed to the source on the FIRST successful open only, so a second one
// fails cleanly (with a null device) instead of two owners fighting over one
// object.
FakeMiriSdrUsb* attachFake(MiriSdrSource& src,
                           std::vector<cascade::usb::UsbDeviceInfo> devices = {}) {
    if (devices.empty()) { devices = oneFakeDevice(); }
    auto owned = std::make_unique<FakeMiriSdrUsb>();
    FakeMiriSdrUsb* raw = owned.get();
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

// --- hand-packed blocks ----------------------------------------------------
//
// Each of these builds ONE 1024-byte block by composing bytes from the bit
// layout msi2500.hpp documents, independently of the code that takes them
// apart. The sixteen header bytes are filled with a pattern rather than zeros:
// a driver that failed to skip them would decode the pattern as enormous
// samples, and zeros would let that pass as merely quiet.
std::vector<std::uint8_t> blockWithHeader() {
    std::vector<std::uint8_t> b(msi2500::kBlockBytes, 0);
    for (std::size_t i = 0; i < msi2500::kBlockHeaderBytes; ++i) {
        b[i] = static_cast<std::uint8_t>(0xA0 + i);
    }
    return b;
}

std::vector<std::uint8_t> pack16(const std::vector<int>& components) {
    std::vector<std::uint8_t> b = blockWithHeader();
    std::uint8_t* p = b.data() + msi2500::kBlockHeaderBytes;
    for (std::size_t i = 0; i < components.size(); ++i) {
        const std::uint16_t u = static_cast<std::uint16_t>(components[i] & 0xFFFF);
        p[2 * i + 0] = static_cast<std::uint8_t>(u & 0xFF);
        p[2 * i + 1] = static_cast<std::uint8_t>(u >> 8);
    }
    return b;
}

std::vector<std::uint8_t> pack8(const std::vector<int>& components) {
    std::vector<std::uint8_t> b = blockWithHeader();
    std::uint8_t* p = b.data() + msi2500::kBlockHeaderBytes;
    for (std::size_t i = 0; i < components.size(); ++i) {
        p[i] = static_cast<std::uint8_t>(components[i] & 0xFF);
    }
    return b;
}

std::vector<std::uint8_t> pack12(const std::vector<int>& components) {
    std::vector<std::uint8_t> b = blockWithHeader();
    std::uint8_t* p = b.data() + msi2500::kBlockHeaderBytes;
    for (std::size_t i = 0; i + 1 < components.size(); i += 2) {
        const std::uint32_t a = static_cast<std::uint32_t>(components[i] & 0xFFF);
        const std::uint32_t c = static_cast<std::uint32_t>(components[i + 1] & 0xFFF);
        std::uint8_t* g = p + (i / 2) * 3;
        g[0] = static_cast<std::uint8_t>(a & 0xFF);
        g[1] = static_cast<std::uint8_t>(((a >> 8) & 0x0F) | ((c & 0x0F) << 4));
        g[2] = static_cast<std::uint8_t>((c >> 4) & 0xFF);
    }
    return b;
}

std::vector<std::uint8_t> pack10(const std::vector<int>& components) {
    std::vector<std::uint8_t> b = blockWithHeader();
    std::uint8_t* p = b.data() + msi2500::kBlockHeaderBytes;
    for (std::size_t i = 0; i < components.size(); i += 4) {
        const std::uint32_t c0 = static_cast<std::uint32_t>(components[i + 0] & 0x3FF);
        const std::uint32_t c1 = static_cast<std::uint32_t>(components[i + 1] & 0x3FF);
        const std::uint32_t c2 = static_cast<std::uint32_t>(components[i + 2] & 0x3FF);
        const std::uint32_t c3 = static_cast<std::uint32_t>(components[i + 3] & 0x3FF);
        std::uint8_t* g = p + (i / 4) * 5;
        g[0] = static_cast<std::uint8_t>(c0 & 0xFF);
        g[1] = static_cast<std::uint8_t>(((c0 >> 8) & 0x03) | ((c1 & 0x3F) << 2));
        g[2] = static_cast<std::uint8_t>(((c1 >> 6) & 0x0F) | ((c2 & 0x0F) << 4));
        g[3] = static_cast<std::uint8_t>(((c2 >> 4) & 0x3F) | ((c3 & 0x03) << 6));
        g[4] = static_cast<std::uint8_t>((c3 >> 2) & 0xFF);
    }
    return b;
}

// The components a format's block holds, as a ramp that visits both signs and
// stays inside the width.
std::vector<int> rampFor(msi2500::Format f) {
    const std::size_t n = msi2500::pairsPerBlock(f) * 2;
    const int span = 1 << (msi2500::componentBits(f) - 1);
    std::vector<int> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        const int v = static_cast<int>(i % static_cast<std::size_t>(span)) - span / 2;
        out[i] = (i % 2 == 0) ? v : -v;
    }
    return out;
}

}  // namespace

int main() {
    // =====================================================================
    // 1. THE WIRE ENCODING, against reg.c mirisdr_write_reg.
    //
    //    Oracle:
    //      reg=0x09 val=0x05f420 -> bmRequestType=0x42 bRequest=0x41
    //                               wValue=0x2009 wIndex=0x05f4
    //      reg=0x08 val=0x00f380 -> wValue=0x8008 wIndex=0x00f3
    //      reg=0x04 val=0x000000 -> wValue=0x0004 wIndex=0x0000
    //      reg=0x03 val=0x011513 -> wValue=0x1303 wIndex=0x0115
    //      reg=0x07 val=0x000094 -> wValue=0x9407 wIndex=0x0000
    // =====================================================================
    {
        struct WireCase {
            std::uint8_t reg;
            std::uint32_t val;
            std::uint16_t value;
            std::uint16_t index;
        };
        const WireCase cases[] = {
            {0x09, 0x05F420, 0x2009, 0x05F4}, {0x08, 0x00F380, 0x8008, 0x00F3},
            {0x04, 0x000000, 0x0004, 0x0000}, {0x03, 0x011513, 0x1303, 0x0115},
            {0x07, 0x000094, 0x9407, 0x0000},
        };
        for (const WireCase& c : cases) {
            const msi2500::RegWrite w = msi2500::encodeRegWrite(c.reg, c.val);
            if (w.value != c.value || w.index != c.index) {
                std::printf(
                    "     encodeRegWrite(0x%02x, 0x%06x): got wValue 0x%04x wIndex 0x%04x, want "
                    "0x%04x / 0x%04x\n",
                    static_cast<unsigned>(c.reg), c.val, static_cast<unsigned>(w.value),
                    static_cast<unsigned>(w.index), static_cast<unsigned>(c.value),
                    static_cast<unsigned>(c.index));
            }
            CHECK(w.value == c.value);
            CHECK(w.index == c.index);
        }

        // The request numbers and the request TYPE, which is the one this
        // family does differently: recipient endpoint, not device.
        CHECK(msi2500::kRequestTypeVendorOutEndpoint == 0x42);
        CHECK(msi2500::requestByte(msi2500::VendorRequest::WriteRegister) == 0x41);
        CHECK(msi2500::requestByte(msi2500::VendorRequest::StartStreaming) == 0x43);
        CHECK(msi2500::requestByte(msi2500::VendorRequest::StopStreaming) == 0x45);
        CHECK(msi2500::requestByte(msi2500::VendorRequest::Reset) == 0x40);
        CHECK(msi2500::kControlTimeoutMs == 2000);
        CHECK(msi2500::kRxEndpoint == 0x81);

        // The transfer geometry: whole blocks, and a multiple of the 512
        // WinUSB requires.
        CHECK(msi2500::kBlockBytes == 1024);
        CHECK(msi2500::kBlockPayloadBytes == 1008);
        CHECK(msi2500::kTransferBytes % msi2500::kBlockBytes == 0);
        CHECK(msi2500::kTransferBytes % 512 == 0);
        CHECK(msi2500::kBlocksPerTransfer == 64);
        CHECK(msi2500::kMaxSamplesPerTransfer == 64 * 504);

        // adc.c mirisdr_adc_init, in its order.
        const std::vector<msi2500::RegSet>& init = msi2500::adcInitSequence();
        CHECK(init.size() == 5);
        const msi2500::RegSet wantInit[] = {{0x08, 0x006080}, {0x05, 0x00000C}, {0x00, 0x000200},
                                            {0x02, 0x004801}, {0x08, 0x00F380}};
        for (std::size_t i = 0; i < 5 && i < init.size(); ++i) {
            if (init[i].reg != wantInit[i].reg || init[i].val != wantInit[i].val) {
                std::printf("     adcInitSequence[%zu]: got reg 0x%02x = 0x%06x, want 0x%02x = "
                            "0x%06x\n",
                            i, static_cast<unsigned>(init[i].reg), init[i].val,
                            static_cast<unsigned>(wantInit[i].reg), wantInit[i].val);
            }
            CHECK(init[i].reg == wantInit[i].reg);
            CHECK(init[i].val == wantInit[i].val);
        }
        CHECK(msi2500::kAdcStopValue == 0x010000u);

        // The bias-T shares register 8 with the band switch and must not clear
        // it.
        CHECK(msi2500::bandSwitchValue(0x00F380, false) == 0x00F380u);
        CHECK(msi2500::bandSwitchValue(0x00F380, true) == 0x00FB80u);
    }

    // =====================================================================
    // 2. THE RATE ARITHMETIC, against hard.c mirisdr_set_hard.
    //
    //    Oracle (rate, format, format register, register 3, register 4):
    //      2000000  fmt 252 0x000094  reg3=0x011513 reg4=0x000000 n=5  vco=240000000
    //      3000000  fmt 252 0x000094  reg3=0x01148b reg4=0x000000 n=4  vco=216000000
    //      6000000  fmt 252 0x000094  reg3=0x011607 reg4=0x000000 n=6  vco=288000000
    //      8000000  fmt 336 0x000085  reg3=0x015807 reg4=0x000000 n=8  vco=384000000
    //      9000000  fmt 384 0x0000a5  reg3=0x019907 reg4=0x000000 n=9  vco=432000000
    //     12000000  fmt 504 0x000c94  reg3=0x01dc07 reg4=0x000000 n=12 vco=576000000
    //      1000000 -> clamped to 1300000: reg3=0x01149b reg4=0x019999 n=4
    //     20000000 -> clamped to 15000000: reg3=0x01df07 reg4=0x000000 n=15
    // =====================================================================
    {
        struct RateCase {
            double hz;
            std::uint32_t programmed;
            msi2500::Format format;
            std::uint32_t formatReg;
            std::uint32_t reg3;
            std::uint32_t reg4;
            std::uint64_t n;
            std::uint64_t vco;
        };
        const RateCase cases[] = {
            {2.0e6, 2000000, msi2500::Format::Bits16, 0x000094, 0x011513, 0x000000, 5, 240000000},
            {3.0e6, 3000000, msi2500::Format::Bits16, 0x000094, 0x01148B, 0x000000, 4, 216000000},
            {6.0e6, 6000000, msi2500::Format::Bits16, 0x000094, 0x011607, 0x000000, 6, 288000000},
            {8.0e6, 8000000, msi2500::Format::Bits12, 0x000085, 0x015807, 0x000000, 8, 384000000},
            {9.0e6, 9000000, msi2500::Format::Bits10, 0x0000A5, 0x019907, 0x000000, 9, 432000000},
            {12.0e6, 12000000, msi2500::Format::Bits8, 0x000C94, 0x01DC07, 0x000000, 12,
             576000000},
            {1.0e6, 1300000, msi2500::Format::Bits16, 0x000094, 0x01149B, 0x019999, 4, 218400000},
            {20.0e6, 15000000, msi2500::Format::Bits8, 0x000C94, 0x01DF07, 0x000000, 15,
             720000000},
        };
        for (const RateCase& c : cases) {
            const msi2500::RateSetting r = msi2500::computeRate(c.hz);
            const bool ok = r.rateHz == c.programmed && r.format == c.format &&
                            r.formatRegister == c.formatReg && r.clockDivider == c.reg3 &&
                            r.clockFraction == c.reg4 && r.n == c.n && r.vcoHz == c.vco;
            if (!ok) {
                std::printf(
                    "     computeRate(%.0f): got rate %u fmt %d reg7 0x%06x reg3 0x%06x reg4 "
                    "0x%06x n %llu vco %llu; want %u / %d / 0x%06x / 0x%06x / 0x%06x / %llu / "
                    "%llu\n",
                    c.hz, r.rateHz, static_cast<int>(r.format), r.formatRegister, r.clockDivider,
                    r.clockFraction, static_cast<unsigned long long>(r.n),
                    static_cast<unsigned long long>(r.vcoHz), c.programmed,
                    static_cast<int>(c.format), c.formatReg, c.reg3, c.reg4,
                    static_cast<unsigned long long>(c.n),
                    static_cast<unsigned long long>(c.vco));
            }
            CHECK(ok);
        }

        // The format boundaries themselves, which are the top rate each
        // packing can carry at the fixed 24000 blocks a second.
        CHECK(msi2500::computeRate(6048000.0).format == msi2500::Format::Bits16);
        CHECK(msi2500::computeRate(6048001.0).format == msi2500::Format::Bits12);
        CHECK(msi2500::computeRate(8064000.0).format == msi2500::Format::Bits12);
        CHECK(msi2500::computeRate(8064001.0).format == msi2500::Format::Bits10);
        CHECK(msi2500::computeRate(9216000.0).format == msi2500::Format::Bits10);
        CHECK(msi2500::computeRate(9216001.0).format == msi2500::Format::Bits8);

        // And the arithmetic that justifies the block size: every format's top
        // rate is the same 24000 blocks a second.
        CHECK(msi2500::pairsPerBlock(msi2500::Format::Bits16) * 24000 == 6048000u);
        CHECK(msi2500::pairsPerBlock(msi2500::Format::Bits12) * 24000 == 8064000u);
        CHECK(msi2500::pairsPerBlock(msi2500::Format::Bits10) * 24000 == 9216000u);
        CHECK(msi2500::pairsPerBlock(msi2500::Format::Bits8) * 24000 == 12096000u);

        // A NaN must not walk out of the arithmetic as a wild divider.
        const msi2500::RateSetting nan = msi2500::computeRate(std::nan(""));
        CHECK(nan.rateHz == 1300000u);
        CHECK(msi2500::computeRate(-5.0e6).rateHz == 1300000u);
    }

    // =====================================================================
    // 3. THE TUNER, against soft.c mirisdr_set_soft.
    //
    //    Oracle (frequency, band, plan row, registers), filter word 7 (8 MHz),
    //    zero IF, 24 MHz crystal:
    //      100000000  VHF  row 3  reg8=0x00f380 reg0=0x05f420 reg2=0x210012
    //                             reg3=0x000003 reg5=0x280035 n=33 thresh=3 frac=1 afc=0
    //     1000000000  L    row 8  reg8=0x00fa80 reg0=0x05f500 reg2=0x140052
    //                             reg3=0x000003 reg5=0x280065 n=20 thresh=6 frac=5
    //        7000000  AM2  row 0  reg8=0x00f780 reg0=0x05fe10 reg2=0x150012
    //                             reg3=0x000003 reg5=0x280065 n=21 thresh=6 frac=1
    //      433920000  B45  row 7  reg8=0x00f380 reg0=0x05f480 reg2=0x120022 reg5=0x280195
    //      100000001  VHF  row 3  reg2=0x215542 reg3=0x0055e3 reg5=0x28ffd5
    //                             n=33 thresh=4093 frac=1364 afc=1374 rfvco=99999755
    //      145500000  B3   row 4  reg8=0x00fa80 reg0=0x05f440 reg2=0x180012 reg5=0x280045
    //      300000000       row 6  reg8=0x00f680 reg0=0x05f460 reg2=0x190002 reg5=0x280015
    //         500000  AM2  row 0  reg8=0x00f780 reg0=0x05fe10 reg2=0x140012 reg5=0x2800c5
    //     2000000000  L    row 8  reg2=0x290022 reg5=0x280035 n=41 thresh=3 frac=2
    // =====================================================================
    {
        struct TuneCase {
            double hz;
            msi001::Band band;
            int row;
            std::uint32_t reg8;
            std::uint32_t reg0;
            std::uint32_t reg2;
            std::uint32_t reg3;
            std::uint32_t reg5;
            std::uint64_t n;
            std::uint64_t thresh;
            std::uint64_t frac;
            std::uint64_t afc;
        };
        const TuneCase cases[] = {
            {100.0e6, msi001::Band::Vhf, 3, 0x00F380, 0x05F420, 0x210012, 0x000003, 0x280035, 33,
             3, 1, 0},
            {1.0e9, msi001::Band::LBand, 8, 0x00FA80, 0x05F500, 0x140052, 0x000003, 0x280065, 20,
             6, 5, 0},
            {7.0e6, msi001::Band::Am2, 0, 0x00F780, 0x05FE10, 0x150012, 0x000003, 0x280065, 21, 6,
             1, 0},
            {433.92e6, msi001::Band::Band45, 7, 0x00F380, 0x05F480, 0x120022, 0x000003, 0x280195,
             18, 25, 2, 0},
            {100.000001e6, msi001::Band::Vhf, 3, 0x00F380, 0x05F420, 0x215542, 0x0055E3, 0x28FFD5,
             33, 4093, 1364, 1374},
            {145.5e6, msi001::Band::Band3, 4, 0x00FA80, 0x05F440, 0x180012, 0x000003, 0x280045, 24,
             4, 1, 0},
            {300.0e6, msi001::Band::Band3, 6, 0x00F680, 0x05F460, 0x190002, 0x000003, 0x280015, 25,
             1, 0, 0},
            {500.0e3, msi001::Band::Am2, 0, 0x00F780, 0x05FE10, 0x140012, 0x000003, 0x2800C5, 20,
             12, 1, 0},
            {2.0e9, msi001::Band::LBand, 8, 0x00FA80, 0x05F500, 0x290022, 0x000003, 0x280035, 41,
             3, 2, 0},
        };
        for (const TuneCase& c : cases) {
            const msi001::TuneSetting t = msi001::computeTune(
                c.hz, msi001::Plan::Default, msi001::Bandwidth::Mhz8, msi001::IfMode::Zero);
            const bool ok = t.band == c.band && t.planIndex == c.row &&
                            t.bandSelectWord == c.reg8 && t.reg0 == c.reg0 && t.reg2 == c.reg2 &&
                            t.reg3 == c.reg3 && t.reg5 == c.reg5 && t.n == c.n &&
                            t.thresh == c.thresh && t.frac == c.frac && t.afc == c.afc;
            if (!ok) {
                std::printf(
                    "     computeTune(%.6f MHz): got band %s row %d reg8 0x%06x reg0 0x%06x reg2 "
                    "0x%06x reg3 0x%06x reg5 0x%06x n %llu thresh %llu frac %llu afc %llu; want "
                    "band %s row %d 0x%06x / 0x%06x / 0x%06x / 0x%06x / 0x%06x / %llu / %llu / "
                    "%llu / %llu\n",
                    c.hz / 1e6, msi001::bandName(t.band), t.planIndex, t.bandSelectWord, t.reg0,
                    t.reg2, t.reg3, t.reg5, static_cast<unsigned long long>(t.n),
                    static_cast<unsigned long long>(t.thresh),
                    static_cast<unsigned long long>(t.frac),
                    static_cast<unsigned long long>(t.afc), msi001::bandName(c.band), c.row,
                    c.reg8, c.reg0, c.reg2, c.reg3, c.reg5,
                    static_cast<unsigned long long>(c.n),
                    static_cast<unsigned long long>(c.thresh),
                    static_cast<unsigned long long>(c.frac),
                    static_cast<unsigned long long>(c.afc));
            }
            CHECK(ok);
        }

        // The AM rows mix up by 120 MHz before they tune, which is why a
        // medium-wave frequency programs a VCO above 120 MHz rather than
        // below 1.
        const msi001::TuneSetting am = msi001::computeTune(
            500.0e3, msi001::Plan::Default, msi001::Bandwidth::Mhz8, msi001::IfMode::Zero);
        CHECK(am.offsetHz == 120000000ULL);
        CHECK(am.loDiv == 16);
        CHECK(am.rfvcoHz == 120500000ULL);

        // The trim is zero for a frequency whose ratio reduces neatly and
        // large for one an Ohm away from it - which is the whole reason the
        // trim field exists.
        const msi001::TuneSetting exact = msi001::computeTune(
            100.0e6, msi001::Plan::Default, msi001::Bandwidth::Mhz8, msi001::IfMode::Zero);
        CHECK(exact.afc == 0);
        CHECK(exact.rfvcoHz == 100000000ULL);
        const msi001::TuneSetting off = msi001::computeTune(
            100.000001e6, msi001::Plan::Default, msi001::Bandwidth::Mhz8, msi001::IfMode::Zero);
        CHECK(off.afc == 1374);
        CHECK(off.rfvcoHz == 99999755ULL);

        // The filter and the IF live in the same register as the band, so a
        // change to either rewrites it. Oracle: filter word 3 gives 0x04f420,
        // word 5 gives 0x057420, word 4 gives 0x053420, and the 450 kHz IF
        // gives 0x05e420, all at 100 MHz.
        struct Reg0Case {
            msi001::Bandwidth bw;
            msi001::IfMode ifMode;
            std::uint32_t reg0;
        };
        const Reg0Case reg0s[] = {
            {msi001::Bandwidth::Khz1536, msi001::IfMode::Zero, 0x04F420},
            {msi001::Bandwidth::Mhz6, msi001::IfMode::Zero, 0x057420},
            {msi001::Bandwidth::Mhz5, msi001::IfMode::Zero, 0x053420},
            {msi001::Bandwidth::Mhz8, msi001::IfMode::Khz450, 0x05E420},
        };
        for (const Reg0Case& c : reg0s) {
            const msi001::TuneSetting t =
                msi001::computeTune(100.0e6, msi001::Plan::Default, c.bw, c.ifMode);
            if (t.reg0 != c.reg0) {
                std::printf("     computeTune(100 MHz, filter %.0f kHz): reg0 0x%06x, want "
                            "0x%06x\n",
                            msi001::bandwidthHz(c.bw) / 1e3, t.reg0, c.reg0);
            }
            CHECK(t.reg0 == c.reg0);
        }

        // The SDRplay input network takes the other plan: same synthesiser,
        // different switch word and different edges. 100 MHz is VHF in both,
        // and the switch words differ.
        const msi001::TuneSetting sdr = msi001::computeTune(
            100.0e6, msi001::Plan::SdrPlay, msi001::Bandwidth::Mhz8, msi001::IfMode::Zero);
        CHECK(sdr.band == msi001::Band::Vhf);
        CHECK(sdr.bandSelectWord == 0x00F180u);
        CHECK(sdr.reg0 == exact.reg0);  // the mode and divider are the same
        CHECK(sdr.reg2 == exact.reg2);
        // 110 MHz is Band III on the default plan (its edge is 108) and still
        // VHF on the SDRplay one (its edge is 112). An edge that moved is
        // exactly what taking the wrong plan gets wrong.
        CHECK(msi001::computeTune(110.0e6, msi001::Plan::Default, msi001::Bandwidth::Mhz8,
                                  msi001::IfMode::Zero)
                  .band == msi001::Band::Band3);
        CHECK(msi001::computeTune(110.0e6, msi001::Plan::SdrPlay, msi001::Bandwidth::Mhz8,
                                  msi001::IfMode::Zero)
                  .band == msi001::Band::Vhf);

        // The filter follows the rate: the widest setting no wider than the
        // span the rate carries.
        CHECK(msi001::bandwidthForRate(2.0e6) == msi001::Bandwidth::Khz1536);
        CHECK(msi001::bandwidthForRate(6.0e6) == msi001::Bandwidth::Mhz6);
        CHECK(msi001::bandwidthForRate(8.0e6) == msi001::Bandwidth::Mhz8);
        CHECK(msi001::bandwidthForRate(12.0e6) == msi001::Bandwidth::Mhz8);
        CHECK(msi001::bandwidthForRate(1.4e6) == msi001::Bandwidth::Khz600);
        CHECK(msi001::bandwidthForRate(0.1e6) == msi001::Bandwidth::Khz200);
        CHECK(msi001::bandwidthWord(msi001::Bandwidth::Khz200) == 0);
        CHECK(msi001::bandwidthWord(msi001::Bandwidth::Mhz8) == 7);
        CHECK(msi001::ifModeWord(msi001::IfMode::Zero) == 3);
        CHECK(msi001::ifModeWord(msi001::IfMode::Khz2048) == 0);
        CHECK(msi001::kTunePreambleWord == 0x00000Eu);
    }

    // =====================================================================
    // 4. THE GAIN STAGES, against gain.c mirisdr_set_gain.
    //
    //    Oracle (lna / buffer / mixer / baseband reductions, band -> reg1):
    //      0 0 0 29 VHF -> 0x0081d1      1 0 0 29 VHF -> 0x00a1d1
    //      0 0 1 29 VHF -> 0x0091d1      0 0 0  0 VHF -> 0x008001
    //      0 0 0 59 VHF -> 0x0083b1      1 3 1 29 VHF -> 0x00b1d1
    //      0 3 0 29 AM2 -> 0x008dd1      0 0 0 29 AM2 -> 0x0081d1
    //      0 2 0 29 AM1 -> 0x0089d1      1 1 0 29 AM1 -> 0x0085d1
    //    and reg6 is 0x2001f6 in every one of them.
    // =====================================================================
    {
        struct GainCase {
            int lna;
            int buffer;
            int mixer;
            int baseband;
            msi001::Band band;
            std::uint32_t reg1;
        };
        const GainCase cases[] = {
            {0, 0, 0, 29, msi001::Band::Vhf, 0x0081D1},
            {1, 0, 0, 29, msi001::Band::Vhf, 0x00A1D1},
            {0, 0, 1, 29, msi001::Band::Vhf, 0x0091D1},
            {0, 0, 0, 0, msi001::Band::Vhf, 0x008001},
            {0, 0, 0, 59, msi001::Band::Vhf, 0x0083B1},
            {1, 3, 1, 29, msi001::Band::Vhf, 0x00B1D1},
            {0, 3, 0, 29, msi001::Band::Am2, 0x008DD1},
            {0, 0, 0, 29, msi001::Band::Am2, 0x0081D1},
            {0, 2, 0, 29, msi001::Band::Am1, 0x0089D1},
            {1, 1, 0, 29, msi001::Band::Am1, 0x0085D1},
        };
        for (const GainCase& c : cases) {
            msi001::GainStages g;
            g.lnaReduction = c.lna;
            g.mixbufferReduction = c.buffer;
            g.mixerReduction = c.mixer;
            g.basebandReduction = c.baseband;
            const msi001::GainSetting s = msi001::computeGain(g, c.band);
            if (s.reg1 != c.reg1 || s.reg6 != 0x2001F6u) {
                std::printf(
                    "     computeGain(lna %d buffer %d mixer %d baseband %d, %s): reg1 0x%06x "
                    "reg6 0x%06x, want 0x%06x / 0x2001f6\n",
                    c.lna, c.buffer, c.mixer, c.baseband, msi001::bandName(c.band), s.reg1,
                    s.reg6, c.reg1);
            }
            CHECK(s.reg1 == c.reg1);
            CHECK(s.reg6 == 0x2001F6u);
        }

        // THE AM ROWS DROP THE LOW-NOISE AMPLIFIER'S BIT whatever was asked
        // for, because the amplifier is not in circuit there. The last two
        // cases above are the proof: reduction 1 on AM1 programs the same
        // amplifier bit as reduction 0 does.
        msi001::GainStages on;
        on.basebandReduction = 29;
        msi001::GainStages off = on;
        off.lnaReduction = 1;
        CHECK(msi001::computeGain(on, msi001::Band::Am1).reg1 ==
              msi001::computeGain(off, msi001::Band::Am1).reg1);
        CHECK(msi001::computeGain(on, msi001::Band::Vhf).reg1 !=
              msi001::computeGain(off, msi001::Band::Vhf).reg1);

        // The decibel readbacks the panel letters.
        CHECK_NEAR(msi001::lnaGainDb(on), 24.0, 1e-9);
        CHECK_NEAR(msi001::lnaGainDb(off), 0.0, 1e-9);
        CHECK_NEAR(msi001::mixerGainDb(on), 19.0, 1e-9);
        CHECK_NEAR(msi001::basebandGainDb(on), 30.0, 1e-9);
    }

    // =====================================================================
    // 5. SAMPLE UNPACKING, one hand-packed block per format.
    // =====================================================================
    {
        // The literal cases first, computed by hand from the layout in
        // msi2500.hpp - these are what a ramp packed by this same file's own
        // helper cannot prove, because a helper and a reader that share a
        // mistake agree perfectly.
        {
            std::vector<std::uint8_t> b = blockWithHeader();
            std::uint8_t* p = b.data() + msi2500::kBlockHeaderBytes;
            p[0] = 0x00;
            p[1] = 0x80;  // -32768
            p[2] = 0xFF;
            p[3] = 0x7F;  // +32767
            std::vector<std::complex<float>> out(252);
            CHECK(msi2500::unpackBlock(msi2500::Format::Bits16, b.data(), out.data()) == 252);
            CHECK_NEAR(out[0].real(), -1.0f, 1e-6);
            CHECK_NEAR(out[0].imag(), 32767.0f / 32768.0f, 1e-6);
        }
        {
            std::vector<std::uint8_t> b = blockWithHeader();
            std::uint8_t* p = b.data() + msi2500::kBlockHeaderBytes;
            p[0] = 0x80;  // -128
            p[1] = 0x7F;  // +127
            std::vector<std::complex<float>> out(504);
            CHECK(msi2500::unpackBlock(msi2500::Format::Bits8, b.data(), out.data()) == 504);
            CHECK_NEAR(out[0].real(), -1.0f, 1e-6);
            CHECK_NEAR(out[0].imag(), 127.0f / 128.0f, 1e-6);
        }
        {
            // 0x34 0x12 0xF0 -> 0x234 = 564, then 0xF01 = -255.
            std::vector<std::uint8_t> b = blockWithHeader();
            std::uint8_t* p = b.data() + msi2500::kBlockHeaderBytes;
            p[0] = 0x34;
            p[1] = 0x12;
            p[2] = 0xF0;
            std::vector<std::complex<float>> out(336);
            CHECK(msi2500::unpackBlock(msi2500::Format::Bits12, b.data(), out.data()) == 336);
            CHECK_NEAR(out[0].real(), 564.0f / 2048.0f, 1e-6);
            CHECK_NEAR(out[0].imag(), -255.0f / 2048.0f, 1e-6);
        }
        {
            // 0x55 0xAA 0xFF 0x00 0x81 -> -427, -22, 15, -508.
            std::vector<std::uint8_t> b = blockWithHeader();
            std::uint8_t* p = b.data() + msi2500::kBlockHeaderBytes;
            p[0] = 0x55;
            p[1] = 0xAA;
            p[2] = 0xFF;
            p[3] = 0x00;
            p[4] = 0x81;
            std::vector<std::complex<float>> out(384);
            CHECK(msi2500::unpackBlock(msi2500::Format::Bits10, b.data(), out.data()) == 384);
            CHECK_NEAR(out[0].real(), -427.0f / 512.0f, 1e-6);
            CHECK_NEAR(out[0].imag(), -22.0f / 512.0f, 1e-6);
            CHECK_NEAR(out[1].real(), 15.0f / 512.0f, 1e-6);
            CHECK_NEAR(out[1].imag(), -508.0f / 512.0f, 1e-6);
        }

        // Then a whole block of each, every component of it, to prove nothing
        // is lost or transposed across the block and that the sixteen header
        // bytes are skipped rather than decoded.
        struct FormatCase {
            msi2500::Format f;
            std::vector<std::uint8_t> (*pack)(const std::vector<int>&);
            const char* name;
        };
        const FormatCase formats[] = {
            {msi2500::Format::Bits16, pack16, "16-bit"},
            {msi2500::Format::Bits12, pack12, "12-bit"},
            {msi2500::Format::Bits10, pack10, "10-bit"},
            {msi2500::Format::Bits8, pack8, "8-bit"},
        };
        for (const FormatCase& fc : formats) {
            const std::vector<int> ramp = rampFor(fc.f);
            const std::vector<std::uint8_t> block = fc.pack(ramp);
            const std::size_t pairs = msi2500::pairsPerBlock(fc.f);
            std::vector<std::complex<float>> out(pairs);
            const std::size_t got = msi2500::unpackBlock(fc.f, block.data(), out.data());
            CHECK(got == pairs);
            const float scale = msi2500::fullScale(fc.f);
            std::size_t wrong = 0;
            for (std::size_t i = 0; i < pairs && i < got; ++i) {
                const float wantI = static_cast<float>(ramp[2 * i]) / scale;
                const float wantQ = static_cast<float>(ramp[2 * i + 1]) / scale;
                if (std::fabs(out[i].real() - wantI) > 1e-6f ||
                    std::fabs(out[i].imag() - wantQ) > 1e-6f) {
                    if (wrong == 0) {
                        std::printf("     %s unpack: pair %zu is (%.6f, %.6f), want (%.6f, %.6f)\n",
                                    fc.name, i, out[i].real(), out[i].imag(), wantI, wantQ);
                    }
                    ++wrong;
                }
            }
            if (wrong != 0) { std::printf("     %s unpack: %zu pairs wrong\n", fc.name, wrong); }
            CHECK(wrong == 0);
        }

        // A whole transfer is whole blocks, and a partial tail is DROPPED
        // rather than half-decoded.
        {
            const std::vector<int> ramp = rampFor(msi2500::Format::Bits16);
            const std::vector<std::uint8_t> one = pack16(ramp);
            std::vector<std::uint8_t> three;
            for (int i = 0; i < 3; ++i) { three.insert(three.end(), one.begin(), one.end()); }
            three.resize(three.size() + 500, 0);  // a partial fourth block
            std::vector<std::complex<float>> out(4 * 252);
            const std::size_t got = msi2500::unpackTransfer(msi2500::Format::Bits16, three.data(),
                                                            three.size(), out.data(), out.size());
            CHECK(got == 3 * 252);
            // And a destination too small for what arrived takes whole blocks
            // only, never a partial one.
            const std::size_t capped = msi2500::unpackTransfer(
                msi2500::Format::Bits16, three.data(), three.size(), out.data(), 300);
            CHECK(capped == 252);
        }
    }

    // =====================================================================
    // 6. ENUMERATION: which devices are ours, and what reopens each one.
    // =====================================================================
    {
        CHECK(msi2500::modelFor(0x1DF7, 0x2500) != nullptr);
        CHECK(msi2500::modelFor(0x1DF7, 0x3000) != nullptr);
        CHECK(msi2500::modelFor(0x1DF7, 0x3000)->sdrPlayFlavour);
        CHECK(!msi2500::modelFor(0x1DF7, 0x2500)->sdrPlayFlavour);

        // --- WHICH RSP IS WHICH (0.99.9) ------------------------------
        //
        // The table lettered 3000 as an RSP1 for four releases. It is an
        // RSP1A: SDRplay's own udev rules map 2500 to the RSP1, 3000 to
        // the RSP1A, 3010 to the RSP2/RSP2pro, 3020 to the RSPduo, 3030
        // to the RSPdx, 3050 to the RSP1B and 3060 to the RSPdx-R2.
        CHECK(std::string(msi2500::modelFor(0x1DF7, 0x3000)->label) ==
              "SDRplay RSP1A");
        CHECK(std::string(msi2500::modelFor(0x1DF7, 0x3010)->label) ==
              "SDRplay RSP2");
        // The four with front ends this driver cannot drive stay OUT: a
        // native row that opens a radio and then hears very little reads
        // as FoxSDR failing rather than as a driver never written. They
        // are served by the SDRplay API path.
        CHECK(msi2500::modelFor(0x1DF7, 0x3020) == nullptr);  // RSPduo
        CHECK(msi2500::modelFor(0x1DF7, 0x3030) == nullptr);  // RSPdx
        CHECK(msi2500::modelFor(0x1DF7, 0x3050) == nullptr);  // RSP1B
        CHECK(msi2500::modelFor(0x1DF7, 0x3060) == nullptr);  // RSPdx-R2

        // --- THE ID THE RSP1 SHARES WITH A TELEVISION STICK -----------
        //
        // 1df7:2500 is BOTH the original RSP1 and the Mirics reference
        // design. The id cannot separate them, and the band plan is not
        // cosmetic: the wrong one tunes with the wrong filter in circuit
        // and says nothing. The bus description is the only thing left
        // that can tell them apart without opening the device.
        {
            const msi2500::DeviceModel& shared = *msi2500::modelFor(0x1DF7, 0x2500);
            const msi2500::ResolvedModel rsp = msi2500::resolveModel(shared, "SDRplay RSP1");
            CHECK(rsp.label == "SDRplay RSP1");
            CHECK(rsp.sdrPlayFlavour);
            // Case and surrounding words do not matter; the name does.
            CHECK(msi2500::resolveModel(shared, "sdrplay rsp1 (usb)").sdrPlayFlavour);
            CHECK(msi2500::resolveModel(shared, "RSP1").sdrPlayFlavour);

            // A TELEVISION STICK KEEPS TODAY'S BEHAVIOUR EXACTLY, and so
            // does a device whose description is empty - which is the
            // ordinary case on this bus and must never be read as an RSP.
            const msi2500::ResolvedModel tv = msi2500::resolveModel(shared, "MSi2500 DVB-T");
            CHECK(tv.label == "Mirics MSi2500");
            CHECK(!tv.sdrPlayFlavour);
            CHECK(!msi2500::resolveModel(shared, "").sdrPlayFlavour);
            CHECK(msi2500::resolveModel(shared, "").label == "Mirics MSi2500");

            // AND A UNIQUE ID IS NEVER SECOND-GUESSED: a description that
            // disagrees with an id naming one product is a bus string
            // somebody renamed, not a different radio.
            const msi2500::DeviceModel& tvOnly = *msi2500::modelFor(0x2040, 0xD300);
            CHECK(!msi2500::resolveModel(tvOnly, "SDRplay RSP1").sdrPlayFlavour);
            const msi2500::DeviceModel& rsp1a = *msi2500::modelFor(0x1DF7, 0x3000);
            CHECK(msi2500::resolveModel(rsp1a, "").sdrPlayFlavour);
            CHECK(msi2500::resolveModel(rsp1a, "").label == "SDRplay RSP1A");
        }
        CHECK(msi2500::modelFor(0x2040, 0xD300) != nullptr);
        CHECK(msi2500::modelFor(0x0BDA, 0x2838) == nullptr);  // an RTL-SDR is not ours
        CHECK(msi2500::usbIds().size() == msi2500::deviceModels().size());

        std::vector<cascade::usb::UsbDeviceInfo> devices;
        cascade::usb::UsbDeviceInfo a;
        a.vid = 0x1DF7;
        a.pid = 0x2500;
        a.path = "p1";
        devices.push_back(a);  // no serial: the ordinary case for this family
        cascade::usb::UsbDeviceInfo b;
        b.vid = 0x0BDA;
        b.pid = 0x2838;
        b.path = "p2";
        b.serial = "00000001";
        devices.push_back(b);  // somebody else's dongle
        cascade::usb::UsbDeviceInfo c;
        c.vid = 0x2040;
        c.pid = 0xD300;
        c.path = "p3";
        c.serial = "ABC123";
        devices.push_back(c);

        const std::vector<NativeDeviceInfo> found =
            cascade::source::miriSdrDevicesFrom(devices);
        CHECK(found.size() == 2);
        if (found.size() == 2) {
            CHECK(found[0].driver == "mirisdr");
            CHECK(found[0].label == "Mirics MSi2500");
            CHECK(found[0].args == "index=0");
            CHECK(found[1].label == "Hauppauge WinTV 133559 LF (serial ABC123)");
            CHECK(found[1].args == "serial=ABC123");
        }
        CHECK(cascade::source::miriSdrDevicesFrom({}).empty());
    }

    // =====================================================================
    // 7. OPEN: the whole opening sequence, byte for byte.
    //
    //    Quieten (stop streaming, ADC asleep), initialise, then 2 MS/s
    //    (format 252, register 3 = 0x011513), 100 MHz with the 1536 kHz
    //    filter the rate chose (register 0 = 0x04f420), then the gain.
    // =====================================================================
    {
        MiriSdrSource src;
        FakeMiriSdrUsb* fake = attachFake(src);
        CHECK(src.open(""));
        CHECK(src.isOpen());
        CHECK(!src.faulted());
        CHECK(std::string(src.name()) == "Mirics: Mirics MSi2500");
        CHECK(std::string(src.driverKey()) == "mirisdr");
        CHECK_NEAR(src.sampleRateHz(), 2.0e6, 1.0);
        CHECK_NEAR(src.centerFrequencyHz(), 100.0e6, 1.0);
        CHECK(src.band() == msi001::Band::Vhf);
        CHECK_NEAR(src.bandwidthHz(), 1536.0e3, 1.0);
        CHECK(src.sampleFormat() == msi2500::Format::Bits16);

        const std::vector<MiriControlRecord> ctl = fake->controls();
        CHECK(ctl.size() == 18);
        CHECK(isCommand("open: stop streaming", at(ctl, 0), 0x45));
        CHECK(isRegSequence("open", ctl, 1,
                            {
                                {0x03, 0x010000},  // the ADC asleep
                                {0x08, 0x006080},  // adc.c mirisdr_adc_init, in order
                                {0x05, 0x00000C},
                                {0x00, 0x000200},
                                {0x02, 0x004801},
                                {0x08, 0x00F380},
                                {0x07, 0x000094},  // format 252 for 2 MS/s
                                {0x04, 0x000000},  // the clock fraction
                                {0x03, 0x011513},  // the clock divider
                                {0x08, 0x00F380},  // the VHF switch word, bias-T off
                                {0x09, 0x00000E},  // the tune preamble
                                {0x09, 0x000003},  // the AFC trim
                                {0x09, 0x04F420},  // mode, IF, 1536 kHz filter, crystal
                                {0x09, 0x280035},  // the fraction's denominator
                                {0x09, 0x210012},  // the integer and the fraction
                                {0x09, 0x0081D1},  // the gain stages
                                {0x09, 0x2001F6},  // the DC-offset calibration timing
                            }));

        // NOT ONE INBOUND TRANSFER. This chip has nothing to read back, and a
        // driver that asked would be asking a device that cannot answer.
        std::size_t inbound = 0;
        for (const MiriControlRecord& r : ctl) {
            if (r.in) { ++inbound; }
        }
        CHECK(inbound == 0);
        // And every one of them carried the 2000 ms bound, not a literal.
        for (const MiriControlRecord& r : ctl) { CHECK(r.timeoutMs == 2000); }

        // The gains the panel is offered, and the one that is not decibels.
        const std::vector<cascade::source::GainInfo> g = src.gains();
        CHECK(g.size() == 4);
        if (g.size() == 4) {
            CHECK(g[0].name == "LNA");
            CHECK(g[0].unit == cascade::source::GainUnit::Decibels);
            CHECK_NEAR(g[0].maxDb, 24.0, 1e-9);
            CHECK(g[1].name == "MIXER");
            CHECK_NEAR(g[1].maxDb, 19.0, 1e-9);
            CHECK(g[2].name == "BASEBAND");
            CHECK_NEAR(g[2].maxDb, 59.0, 1e-9);
            CHECK_NEAR(g[2].stepDb, 1.0, 1e-9);
            CHECK(g[3].name == "AM BUFFER");
            CHECK(g[3].unit == cascade::source::GainUnit::Steps);
            CHECK_NEAR(g[3].maxDb, 3.0, 1e-9);
        }
        CHECK_NEAR(src.gainDb("LNA"), 24.0, 1e-9);
        CHECK_NEAR(src.gainDb("MIXER"), 19.0, 1e-9);
        CHECK_NEAR(src.gainDb("BASEBAND"), 30.0, 1e-9);
        CHECK_NEAR(src.gainDb("AM BUFFER"), 0.0, 1e-9);
        CHECK(!src.autoGainSupported());
        CHECK(!src.setAutoGain(true));
        CHECK(src.antennas().size() == 1);
        CHECK(src.setAntenna("RX"));
        CHECK(!src.setAntenna("TX"));

        double lo = 0.0;
        double hi = 0.0;
        CHECK(src.frequencyRangeHz(lo, hi));
        CHECK_NEAR(lo, 150.0e3, 1.0);
        CHECK_NEAR(hi, 2.0e9, 1.0);

        // The menu steps over the window whose packing could not be derived.
        for (const double r : src.supportedSampleRatesHz()) {
            CHECK(msi2500::computeRate(r).format != msi2500::Format::Bits10);
        }

        src.closeDevice();
        CHECK(!src.isOpen());
    }

    // An SDRplay-badged device takes the other band plan, and the only
    // difference at 100 MHz is the switch word - which is exactly the kind of
    // difference that would never show up as a failure, only as a filter in
    // the wrong place.
    //
    // THE NAME CHANGED IN 0.99.9 AND THE EXPECTATION WAS THE WRONG ONE, not
    // the code: 1df7:3000 is an RSP1A, per SDRplay own udev rules. This check
    // asserted the mislabel for four releases. The band plan it exists to
    // prove is unchanged - an RSP1A wants the SDRplay plan exactly as the
    // device this row was thought to be did.
    {
        MiriSdrSource src;
        FakeMiriSdrUsb* fake = attachFake(src, oneFakeDevice("", 0x1DF7, 0x3000));
        CHECK(src.open(""));
        CHECK(std::string(src.name()) == "Mirics: SDRplay RSP1A");
        const std::vector<MiriControlRecord> ctl = fake->controls();
        // Index 10 is the band switch: one command, the ADC asleep, five
        // initialisation writes, three rate writes, then the tune.
        CHECK(isRegWrite("sdrplay switch word", at(ctl, 10), 0x08, 0x00F180));
        src.closeDevice();
    }

    // =====================================================================
    // 8. RATES, each as the exact transfers it becomes - INCLUDING the retune
    //    and the gain write that must follow, because the filter lives in the
    //    tuner and the gain register's meaning follows the band.
    // =====================================================================
    {
        MiriSdrSource src;
        FakeMiriSdrUsb* fake = attachFake(src);
        CHECK(src.open(""));
        fake->clearControls();

        CHECK(src.setSampleRateHz(8.0e6));
        CHECK_NEAR(src.sampleRateHz(), 8.0e6, 1.0);
        CHECK(src.sampleFormat() == msi2500::Format::Bits12);
        CHECK_NEAR(src.bandwidthHz(), 8.0e6, 1.0);
        CHECK(isRegSequence("8 MS/s", fake->controls(), 0,
                            {
                                {0x07, 0x000085},  // format 336
                                {0x04, 0x000000},
                                {0x03, 0x015807},
                                {0x08, 0x00F380},  // the retune, with the 8 MHz filter
                                {0x09, 0x00000E},
                                {0x09, 0x000003},
                                {0x09, 0x05F420},  // filter word 7 now
                                {0x09, 0x280035},
                                {0x09, 0x210012},
                                {0x09, 0x0081D1},  // and the gain behind it
                                {0x09, 0x2001F6},
                            }));
        CHECK(fake->controls().size() == 11);

        fake->clearControls();
        CHECK(src.setSampleRateHz(2.0e6));
        CHECK(src.sampleFormat() == msi2500::Format::Bits16);
        CHECK(isRegSequence("2 MS/s", fake->controls(), 0,
                            {
                                {0x07, 0x000094},
                                {0x04, 0x000000},
                                {0x03, 0x011513},
                                {0x08, 0x00F380},
                                {0x09, 0x00000E},
                                {0x09, 0x000003},
                                {0x09, 0x04F420},  // back to the 1536 kHz filter
                                {0x09, 0x280035},
                                {0x09, 0x210012},
                                {0x09, 0x0081D1},
                                {0x09, 0x2001F6},
                            }));

        // Below the floor is REFUSED and nothing is sent; above the ceiling is
        // coerced and said.
        fake->clearControls();
        CHECK(!src.setSampleRateHz(1.0e6));
        CHECK(fake->controlCount() == 0);
        CHECK(std::string(src.lastError()).find("1.3 MS/s") != std::string::npos);
        CHECK_NEAR(src.sampleRateHz(), 2.0e6, 1.0);
        CHECK(src.setSampleRateHz(20.0e6));
        CHECK_NEAR(src.sampleRateHz(), 15.0e6, 1.0);
        CHECK(std::string(src.lastError()).find("coerced") != std::string::npos);
        CHECK(src.sampleFormat() == msi2500::Format::Bits8);

        src.closeDevice();
    }

    // =====================================================================
    // 9. TUNES, and the band switching that comes with them.
    // =====================================================================
    {
        MiriSdrSource src;
        FakeMiriSdrUsb* fake = attachFake(src);
        CHECK(src.open(""));
        fake->clearControls();

        // 1 GHz: L-band, its own switch word.
        CHECK(src.setCenterFrequencyHz(1.0e9));
        CHECK(src.band() == msi001::Band::LBand);
        CHECK(isRegSequence("1 GHz", fake->controls(), 0,
                            {
                                {0x08, 0x00FA80},
                                {0x09, 0x00000E},
                                {0x09, 0x000003},
                                {0x09, 0x04F500},
                                {0x09, 0x280065},
                                {0x09, 0x140052},
                                {0x09, 0x0081D1},
                                {0x09, 0x2001F6},
                            }));
        CHECK(fake->controls().size() == 8);

        // 7 MHz: an AM input, the upconvert mixer, and a gain register whose
        // amplifier bit is now forced clear.
        fake->clearControls();
        CHECK(src.setCenterFrequencyHz(7.0e6));
        CHECK(src.band() == msi001::Band::Am2);
        CHECK(isRegSequence("7 MHz", fake->controls(), 0,
                            {
                                {0x08, 0x00F780},
                                {0x09, 0x00000E},
                                {0x09, 0x000003},
                                {0x09, 0x04FE10},
                                {0x09, 0x280065},
                                {0x09, 0x150012},
                                {0x09, 0x0081D1},
                                {0x09, 0x2001F6},
                            }));
        // And the panel is told the truth about it: there is no low-noise
        // amplifier on this input, whatever the control was left at.
        CHECK_NEAR(src.gainDb("LNA"), 0.0, 1e-9);

        // Back to 100 MHz and the amplifier is real again.
        CHECK(src.setCenterFrequencyHz(100.0e6));
        CHECK(src.band() == msi001::Band::Vhf);
        CHECK_NEAR(src.gainDb("LNA"), 24.0, 1e-9);

        // Outside the range is refused with a reason, and nothing is sent.
        fake->clearControls();
        CHECK(!src.setCenterFrequencyHz(100.0e3));
        CHECK(!src.setCenterFrequencyHz(2.5e9));
        CHECK(fake->controlCount() == 0);
        CHECK(std::string(src.lastError()).find("2500") != std::string::npos);
        CHECK_NEAR(src.centerFrequencyHz(), 100.0e6, 1.0);

        src.closeDevice();
    }

    // =====================================================================
    // 10. EACH GAIN STAGE, as the exact transfer it becomes.
    // =====================================================================
    {
        MiriSdrSource src;
        FakeMiriSdrUsb* fake = attachFake(src);
        CHECK(src.open(""));

        struct Case {
            const char* gain;
            double db;
            std::uint32_t reg1;
            double readback;
        };
        const Case cases[] = {
            {"LNA", 0.0, 0x00A1D1, 0.0},          // the amplifier out
            {"LNA", 24.0, 0x0081D1, 24.0},        // and back in
            {"LNA", 40.0, 0x0081D1, 24.0},        // clamped, not refused
            {"MIXER", 0.0, 0x0091D1, 0.0},        //
            {"MIXER", 19.0, 0x0081D1, 19.0},      //
            {"BASEBAND", 59.0, 0x008001, 59.0},   // no reduction at all
            {"BASEBAND", 0.0, 0x0083B1, 0.0},     // the full 59 dB of reduction
            {"BASEBAND", 100.0, 0x008001, 59.0},  // clamped
            {"BASEBAND", 30.0, 0x0081D1, 30.0},   // back to where open left it
            {"AM BUFFER", 3.0, 0x0081D1, 3.0},    // no effect on a VHF band...
            {"AM BUFFER", 0.0, 0x0081D1, 0.0},
        };
        for (const Case& c : cases) {
            fake->clearControls();
            CHECK(src.setGainDb(c.gain, c.db));
            const std::vector<MiriControlRecord> ctl = fake->controls();
            char label[96];
            std::snprintf(label, sizeof(label), "%s = %.1f", c.gain, c.db);
            CHECK(ctl.size() == 2);
            CHECK(isRegWrite(label, at(ctl, 0), 0x09, c.reg1));
            CHECK(isRegWrite("calibration timing", at(ctl, 1), 0x09, 0x2001F6));
            CHECK_NEAR(src.gainDb(c.gain), c.readback, 1e-9);
        }

        // ...but it does on an AM one, which is the whole reason it is a
        // control rather than a constant.
        CHECK(src.setCenterFrequencyHz(7.0e6));
        fake->clearControls();
        CHECK(src.setGainDb("AM BUFFER", 3.0));
        CHECK(isRegWrite("AM BUFFER = 3 on AM2", at(fake->controls(), 0), 0x09, 0x008DD1));

        CHECK(!src.setGainDb("VGA", 10.0));
        CHECK(std::string(src.lastError()).find("VGA") != std::string::npos);

        // The bias-T rides in the band-switch register and must not clear it.
        CHECK(src.setCenterFrequencyHz(100.0e6));
        fake->clearControls();
        CHECK(src.setBiasT(true));
        CHECK(src.biasT());
        CHECK(fake->controls().size() == 1);
        CHECK(isRegWrite("bias-T on", at(fake->controls(), 0), 0x08, 0x00FB80));
        // ...and a tune while it is on carries it through.
        //
        // AND ONE FINDING WORTH PINNING RATHER THAN SMOOTHING OVER: the L-band
        // row's own switch word ALREADY HAS BIT 11 SET (0xFA80), which is the
        // bit the bias-T rides in. So switching the bias-T on changes nothing
        // in that band and switching it off cannot turn it off. That is the
        // reference's own arrangement, reproduced faithfully because the
        // alternative is inventing a different bit; it is recorded here so the
        // first person to measure a Mirics device knows to look at it.
        fake->clearControls();
        CHECK(src.setCenterFrequencyHz(1.0e9));
        CHECK(isRegWrite("bias-T through a tune", at(fake->controls(), 0), 0x08, 0x00FA80));
        CHECK((0x00FA80u & msi2500::kBiasTeeBit) != 0);
        CHECK(src.setBiasT(false));

        src.closeDevice();
    }

    // =====================================================================
    // 11. START AND STOP: the stream commands, and the bulk ring around them.
    // =====================================================================
    {
        MiriSdrSource src;
        FakeMiriSdrUsb* fake = attachFake(src);
        CHECK(src.open(""));
        fake->clearControls();

        CHECK(src.start());
        CHECK(src.running());
        CHECK(src.selfPaced());
        // The ring is queued BEFORE the device is told to stream.
        CHECK(fake->beginBulkCalls() == 1);
        CHECK(fake->lastBulkEndpoint() == 0x81);
        CHECK(fake->lastBulkBufferBytes() == msi2500::kTransferBytes);
        CHECK(fake->lastBulkBufferCount() == msi2500::kTransferCount);
        CHECK(fake->controls().size() == 1);
        CHECK(isCommand("start streaming", at(fake->controls(), 0), 0x43));

        CHECK(src.start());  // idempotent
        CHECK(fake->beginBulkCalls() == 1);

        fake->clearControls();
        src.stop();
        CHECK(!src.running());
        CHECK(fake->endBulkCalls() >= 1);
        CHECK(fake->controls().size() == 1);
        CHECK(isCommand("stop streaming", at(fake->controls(), 0), 0x45));
        // THE SHORT BOUND, and it is the reason the shutdown budget does not
        // have to move for this driver: 500 ms here rather than 2000 keeps the
        // Mirics teardown column at 1750 ms, under the 3000 ms SoapySDR one
        // that tests/test_shutdown_budget.cpp charges. Every other transfer
        // this driver sends takes the full 2000 (asserted at open, above).
        CHECK(at(fake->controls(), 0).timeoutMs == 500);
        CHECK(!fake->readBulkInFlight());  // nothing is inside readBulk when the ring is freed

        src.stop();  // idempotent
        src.closeDevice();
    }

    // =====================================================================
    // 12. STREAMING: scripted blocks, every sample once and in order.
    // =====================================================================
    {
        MiriSdrSource src;
        FakeMiriSdrUsb* fake = attachFake(src);
        CHECK(src.open(""));

        // Three transfers of two blocks each, one continuous ramp across all
        // six blocks - so a reader that dropped a block, repeated one or
        // reordered two cannot pass.
        constexpr std::size_t kBlocks = 6;
        constexpr std::size_t kPairs = 252;
        std::vector<int> all(kBlocks * kPairs * 2);
        for (std::size_t i = 0; i < all.size(); ++i) {
            all[i] = static_cast<int>(i % 2000) - 1000;
        }
        for (std::size_t t = 0; t < 3; ++t) {
            std::vector<std::uint8_t> transfer;
            for (std::size_t b = 0; b < 2; ++b) {
                const std::size_t block = t * 2 + b;
                const std::vector<int> slice(all.begin() + block * kPairs * 2,
                                             all.begin() + (block + 1) * kPairs * 2);
                const std::vector<std::uint8_t> packed = pack16(slice);
                transfer.insert(transfer.end(), packed.begin(), packed.end());
            }
            fake->queueBulk(std::move(transfer));
        }

        CHECK(src.start());
        std::vector<std::complex<float>> got;
        got.reserve(kBlocks * kPairs);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        std::complex<float> buf[512];
        while (got.size() < kBlocks * kPairs && std::chrono::steady_clock::now() < deadline) {
            const std::size_t n = src.read(buf, 512);
            for (std::size_t i = 0; i < n; ++i) { got.push_back(buf[i]); }
        }
        CHECK(got.size() == kBlocks * kPairs);
        std::size_t wrong = 0;
        for (std::size_t i = 0; i < got.size() && i < kBlocks * kPairs; ++i) {
            const float wantI = static_cast<float>(all[2 * i]) / 32768.0f;
            const float wantQ = static_cast<float>(all[2 * i + 1]) / 32768.0f;
            if (std::fabs(got[i].real() - wantI) > 1e-6f ||
                std::fabs(got[i].imag() - wantQ) > 1e-6f) {
                if (wrong == 0) {
                    std::printf("     stream: sample %zu is (%.6f, %.6f), want (%.6f, %.6f)\n", i,
                                got[i].real(), got[i].imag(), wantI, wantQ);
                }
                ++wrong;
            }
        }
        if (wrong != 0) { std::printf("     stream: %zu samples wrong of %zu\n", wrong, got.size()); }
        CHECK(wrong == 0);
        CHECK(src.droppedTransfers() == 0);

        // The health line the reader writes, in the shared format.
        src.setStreamHealthWindowForTest(std::chrono::milliseconds(1));
        const std::string line = src.streamHealthLine();
        CHECK(line.find("source: stream health - reads ") == 0);
        CHECK(line.find("with samples ") != std::string::npos);
        CHECK(line.find("timeouts ") != std::string::npos);
        CHECK(line.find("overflows ") != std::string::npos);
        CHECK(line.find("longest gap ") != std::string::npos);

        src.stop();
        src.closeDevice();
    }

    // =====================================================================
    // 13. STOP WHILE STREAMING comes back promptly.
    // =====================================================================
    {
        MiriSdrSource src;
        attachFake(src);
        CHECK(src.open(""));
        CHECK(src.start());
        CHECK(waitFor([&] { return src.running(); }, std::chrono::milliseconds(500)));
        const auto t0 = std::chrono::steady_clock::now();
        src.stop();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t0)
                                 .count();
        if (elapsed >= 500) { std::printf("     stop() took %lld ms\n", elapsed); }
        CHECK(elapsed < 500);
        CHECK(!src.running());
        src.closeDevice();
    }

    // =====================================================================
    // 14. THE DEVICE GOES: the reader leaves, and says why.
    // =====================================================================
    {
        MiriSdrSource src;
        FakeMiriSdrUsb* fake = attachFake(src);
        CHECK(src.open(""));
        fake->onExhausted.store(FakeMiriSdrUsb::Exhausted::DeviceGone);
        CHECK(src.start());
        CHECK(waitFor([&] { return src.faulted(); }, std::chrono::seconds(3)));
        CHECK(src.deviceDead());
        CHECK(src.faultedWhile() == "reading samples");
        CHECK(std::string(src.lastError()).find("unplug it") != std::string::npos);
        // read() on a dead device answers nothing rather than blocking.
        std::complex<float> buf[8];
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(src.read(buf, 8) == 0);
        CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0)
                  .count() < 200);
        // And nothing further is sent to it, whatever is asked.
        fake->clearControls();
        CHECK(!src.setCenterFrequencyHz(144.0e6));
        CHECK(!src.setGainDb("LNA", 0.0));
        CHECK(!src.setSampleRateHz(4.0e6));
        CHECK(fake->controlCount() == 0);
        src.stop();
        src.closeDevice();
    }

    // =====================================================================
    // 15. A READER THAT WILL NOT COME BACK is abandoned, not waited for.
    // =====================================================================
    {
        MiriSdrSource src;
        FakeMiriSdrUsb* fake = attachFake(src);
        CHECK(src.open(""));
        fake->onExhausted.store(FakeMiriSdrUsb::Exhausted::Block);
        CHECK(src.start());
        CHECK(waitFor([&] { return fake->readBulkInFlight(); }, std::chrono::seconds(2)));

        const unsigned long long before = MiriSdrSource::readersAbandoned();
        const auto t0 = std::chrono::steady_clock::now();
        src.stop();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - t0)
                                 .count();
        // THE BOUND, and the flag. Elapsed time alone still passes when the
        // bound is deleted and the reader happens to come back; the counter is
        // what proves the abandoning path ran.
        if (elapsed >= 2000) { std::printf("     stop() on a wedged reader took %lld ms\n", elapsed); }
        CHECK(elapsed < 2000);
        CHECK(MiriSdrSource::readersAbandoned() == before + 1);
        CHECK(!src.running());
        CHECK(src.faulted());
        CHECK(src.faultedWhile() == "waiting for the sample reader to stop");

        // The device was LEAKED on purpose so the stranded thread still has
        // something valid to be inside. Let it out now, and leave it alone
        // afterwards - it outlives this scope by design.
        fake->releaseBlock.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        src.closeDevice();
    }

    // =====================================================================
    // 16. OPENING WHAT IS NOT THERE fails cleanly.
    // =====================================================================
    {
        MiriSdrSource src;  // no transport at all
        CHECK(!src.open(""));
        CHECK(!src.isOpen());
        CHECK(std::string(src.lastError()).find("no Mirics") != std::string::npos);
        // Nothing to stop, nothing to read, nothing to crash on.
        src.stop();
        std::complex<float> buf[4];
        CHECK(src.read(buf, 4) == 0);
        CHECK(!src.start());
        CHECK(!src.setSampleRateHz(2.0e6));
        CHECK(!src.setCenterFrequencyHz(100.0e6));
        CHECK(!src.setBiasT(true));
        src.closeDevice();
    }
    {
        MiriSdrSource src;
        attachFake(src, oneFakeDevice("ABC123"));
        CHECK(!src.open("serial=ffffffff"));
        CHECK(!src.isOpen());
        CHECK(std::string(src.lastError()).find("ffffffff") != std::string::npos);
        CHECK(!src.open("index=4"));
        CHECK(std::string(src.lastError()).find("index 4") != std::string::npos);
        // A suffix of the serial still finds it.
        CHECK(src.open("serial=c123"));
        CHECK(src.isOpen());
        src.closeDevice();
    }
    {
        // A device with NO serial - the ordinary case for this family - must
        // not be matched by a serial request, because every such device would
        // otherwise match every request.
        MiriSdrSource src;
        attachFake(src);
        CHECK(!src.open("serial=00000001"));
        CHECK(src.open("index=0"));
        src.closeDevice();
    }

    // =====================================================================
    // 17. A CONTROL TRANSFER THAT FAILS condemns the device.
    // =====================================================================
    {
        MiriSdrSource src;
        FakeMiriSdrUsb* fake = attachFake(src);
        CHECK(src.open(""));
        fake->failingRegisters.push_back(0x09);  // every tuner word
        CHECK(!src.setCenterFrequencyHz(144.0e6));
        CHECK(src.faulted());
        CHECK(src.deviceDead());
        CHECK(src.faultedWhile() == "setting the centre frequency");
        src.closeDevice();
    }
    {
        // An open that fails half way through leaves nothing behind.
        MiriSdrSource src;
        FakeMiriSdrUsb* fake = attachFake(src);
        fake->failingRegisters.push_back(0x05);  // inside the ADC initialisation
        CHECK(!src.open(""));
        CHECK(!src.isOpen());
        CHECK(src.faulted());
        CHECK(src.faultedWhile() == "initialising the ADC");
        src.closeDevice();
    }
    {
        // ...and so does one that fails at the very first transfer.
        MiriSdrSource src;
        FakeMiriSdrUsb* fake = attachFake(src);
        fake->failingRequests.push_back(0x45);
        CHECK(!src.open(""));
        CHECK(!src.isOpen());
        CHECK(src.faultedWhile() == "stopping any stream in progress");
        src.closeDevice();
    }
    {
        // A start that cannot tell the device to stream tears the ring back
        // down rather than leaving it queued.
        MiriSdrSource src;
        FakeMiriSdrUsb* fake = attachFake(src);
        CHECK(src.open(""));
        fake->failingRequests.push_back(0x43);
        CHECK(!src.start());
        CHECK(!src.running());
        CHECK(fake->endBulkCalls() >= 1);
        src.closeDevice();
    }

    return testSummary("test_mirisdr_source");
}
