// test_rx888_source.cpp - the RX888 mk2 driver, proven byte for byte against
// a fake that answers as the SDDC firmware does, and sample for sample
// against a second implementation of its own DSP.
//
// WHERE THE EXPECTATIONS COME FROM. There is no RX888 on this bench, so an
// expectation invented here would only prove this file agrees with itself.
// Every protocol number below is read straight out of ExtIO_sddc or out of
// the SDDC_FX3 firmware it ships, and each block names the file and line it
// came from:
//
//   Interface.h                  the command and argument numbers, the GPIO bits
//   Core/RadioHandler.{h,cpp}    the open order, the dither/random/bias controls
//   Core/RX888R2Radio.cpp        the mk2's gain tables and its HF/VHF switching
//   Core/arch/win32/FX3handler.cpp   how a command's payload is laid out
//   Core/arch/linux/usb_device.c     the two USB identities
//   SDDC_FX3/USBhandler.c        what the firmware does with each request
//   SDDC_FX3/Application.h       the bulk endpoint
//   Cypress AN76405              the FX3 boot image format, verified against
//                                the actual bytes of the shipped image
//
// The DSP is a different kind of claim and needs a different kind of proof.
// It is NOT a port of the reference's FFT engine (rx888_protocol.hpp says why
// at length), so "the same bytes as the reference" is not available for it.
// What is available is better: the fast structure is checked against a
// brute-force rotate-filter-decimate written a second time in this file, and
// the whole chain is measured - tone frequency, amplitude, image rejection,
// spectrum inversion - with the numbers pinned.
//
// THE FIVE DELIBERATE BREAKS. Every block that matters was watched go RED
// against a broken driver before it was trusted green:
//   - a wrong command number (STARTADC 0xB2 -> 0xB3)
//   - a wrong attenuator code (the DAT-31 code sent uninverted)
//   - a firmware chunk size of 8192 instead of 4096
//   - the fs/4 rotation's per-output sign deleted
//   - the bounded join replaced by a plain one
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
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "rx888_fake_usb.hpp"
#include "source/rx888_firmware.hpp"
#include "source/rx888_protocol.hpp"
#include "source/rx888_source.hpp"
#include "test_check.hpp"

using cascade::source::NativeDeviceInfo;
using cascade::source::Rx888Source;
using cascade::test::FakeRx888Bus;
using cascade::test::FakeRx888Usb;
using cascade::test::Rx888ControlRecord;
namespace rx888 = cascade::source::rx888;

namespace {

// --- helpers ---------------------------------------------------------------

// Bounds-safe indexing. A `CHECK(v.size() == n)` followed by `v[i]` is an
// out-of-bounds read in exactly the run that has something to report - the
// harness records a failed check and carries on, so the crash lands instead of
// the message. (mayhem-b200, 2026-08-13; the same trap cost a whole session.)
const Rx888ControlRecord& at(const std::vector<Rx888ControlRecord>& v, std::size_t i) {
    static const Rx888ControlRecord kAbsent{};
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
bool isControl(const char* label, const Rx888ControlRecord& r, bool in, int request, int value,
               int index) {
    const std::uint8_t wantType = in ? 0xC0 : 0x40;
    const bool ok = r.in == in && r.requestType == wantType &&
                    r.request == static_cast<std::uint8_t>(request) &&
                    r.value == static_cast<std::uint16_t>(value) &&
                    r.index == static_cast<std::uint16_t>(index);
    if (!ok) {
        std::printf(
            "     %s: got %s type 0x%02x request 0x%02x value 0x%04x index 0x%04x;"
            " want %s type 0x%02x request 0x%02x value 0x%04x index 0x%04x\n",
            label, r.in ? "IN" : "OUT", static_cast<unsigned>(r.requestType),
            static_cast<unsigned>(r.request), static_cast<unsigned>(r.value),
            static_cast<unsigned>(r.index), in ? "IN" : "OUT", static_cast<unsigned>(wantType),
            static_cast<unsigned>(request), static_cast<unsigned>(value),
            static_cast<unsigned>(index));
    }
    return ok;
}

// The four bytes a little-endian uint32 payload should be.
std::vector<int> le32(std::uint32_t v) {
    return {static_cast<int>(v & 0xFF), static_cast<int>((v >> 8) & 0xFF),
            static_cast<int>((v >> 16) & 0xFF), static_cast<int>((v >> 24) & 0xFF)};
}

std::vector<int> le64(std::uint64_t v) {
    std::vector<int> out;
    for (int b = 0; b < 8; ++b) { out.push_back(static_cast<int>((v >> (8 * b)) & 0xFF)); }
    return out;
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

// Attaches a bus to a source. The bus outlives the source in every test here,
// so the transcript can still be read after closeDevice().
void attach(Rx888Source& src, FakeRx888Bus& bus) {
    src.setTransportForTest([&bus] { return bus.list(); },
                            [&bus](const std::string& path, std::string& error) {
                                return bus.open(path, error);
                            });
}

// --- the DSP, written a second time ---------------------------------------

// The brute force the fast structure in QuarterRotateHalfBand is checked
// against: rotate every sample by exp(-j*pi*n/2), convolve with the whole
// kernel including all the zeros, then keep every second output. Slow,
// obviously correct, and derived from the definition rather than from the
// code under test.
std::vector<std::complex<float>> bruteRotateHalfBand(const std::vector<float>& x,
                                                     const std::vector<float>& h) {
    std::vector<std::complex<float>> y(x.size());
    for (std::size_t n = 0; n < x.size(); ++n) {
        const float v = x[n];
        switch (n % 4) {
            case 0: y[n] = std::complex<float>(v, 0.0f); break;
            case 1: y[n] = std::complex<float>(0.0f, -v); break;
            case 2: y[n] = std::complex<float>(-v, 0.0f); break;
            default: y[n] = std::complex<float>(0.0f, v); break;
        }
    }
    std::vector<std::complex<float>> out;
    for (std::size_t n = 0; n < x.size(); n += 2) {
        std::complex<float> acc(0.0f, 0.0f);
        for (std::size_t k = 0; k < h.size(); ++k) {
            if (k > n) { break; }  // y[n < 0] = 0
            acc += h[k] * y[n - k];
        }
        out.push_back(acc);
    }
    return out;
}

// A deterministic pseudo-random real signal. Its own generator rather than
// std::mt19937 so the sequence cannot move under a standard-library update.
struct Lcg {
    std::uint64_t s = 0x2545F4914F6CDD1DULL;
    float next() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<float>(static_cast<std::int32_t>(s >> 33)) / 2147483648.0f;
    }
};

// The amplitude of one exact frequency in a complex stream: correlate with
// exp(-j*2*pi*f*n). For a signal that IS A e^{j*2*pi*f*n} this returns exactly
// A, and for any OTHER exact bin of the same window length it returns exactly
// zero - which is why the tone frequencies below are chosen to be whole
// numbers of cycles across the analysis window.
std::complex<double> binOf(const std::complex<float>* z, std::size_t n, double cyclesPerSample) {
    constexpr double kTwoPi = 6.283185307179586476925286766559;
    double re = 0.0;
    double im = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double p = -kTwoPi * cyclesPerSample * static_cast<double>(i);
        const double c = std::cos(p);
        const double s = std::sin(p);
        re += static_cast<double>(z[i].real()) * c - static_cast<double>(z[i].imag()) * s;
        im += static_cast<double>(z[i].real()) * s + static_cast<double>(z[i].imag()) * c;
    }
    return {re / static_cast<double>(n), im / static_cast<double>(n)};
}

// A real tone as the ADC would deliver it: 16-bit counts.
std::vector<std::int16_t> realTone(std::size_t n, double freqHz, double rateHz, double amplitude) {
    constexpr double kTwoPi = 6.283185307179586476925286766559;
    std::vector<std::int16_t> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double v =
            amplitude * 32767.0 * std::cos(kTwoPi * freqHz * static_cast<double>(i) / rateHz);
        out[i] = static_cast<std::int16_t>(std::lround(v));
    }
    return out;
}

double dbOf(double ratio) { return 20.0 * std::log10(ratio > 0.0 ? ratio : 1e-30); }

}  // namespace

int main() {
    // =====================================================================
    // 1. ENUMERATION. Both identities are listed, a stranger is not, and a
    //    bootloader says what it is rather than pretending to be a radio.
    // =====================================================================
    {
        std::vector<cascade::usb::UsbDeviceInfo> devices;
        cascade::usb::UsbDeviceInfo run;
        run.vid = 0x04B4;
        run.pid = 0x00F1;
        run.serial = "SDDC0001";
        run.description = "RX888mk2";
        run.path = "\\\\?\\a";
        devices.push_back(run);

        cascade::usb::UsbDeviceInfo boot;
        boot.vid = 0x04B4;
        boot.pid = 0x00F3;
        boot.serial = "";
        boot.description = "WestBridge";
        boot.path = "\\\\?\\b";
        devices.push_back(boot);

        cascade::usb::UsbDeviceInfo other;
        other.vid = 0x0BDA;  // an RTL-SDR's vendor: not ours
        other.pid = 0x2838;
        other.path = "\\\\?\\c";
        devices.push_back(other);

        const std::vector<NativeDeviceInfo> found = cascade::source::rx888DevicesFrom(devices);
        CHECK(found.size() == 2);
        if (found.size() == 2) {
            CHECK(found[0].driver == "rx888");
            // The product string the SDDC firmware publishes
            // (SDDC_FX3/USBdescriptor.c:238-243) is what SetupAPI reports, so
            // it is what the row shows.
            CHECK(found[0].label == "RX888mk2 (serial SDDC0001)");
            CHECK(found[0].args == "serial=SDDC0001");
            CHECK(found[1].label == "RX888 (needs firmware, will load on open)");
            // No serial on the bootloader, so it is addressed by position.
            CHECK(found[1].args == "index=1");
        }

        // Both VID/PID pairs are asked of the transport: a driver that
        // enumerated only the running identity would never see a radio that
        // has just been plugged in, which is all of them.
        const std::vector<cascade::usb::UsbId> ids = cascade::source::rx888UsbIds();
        CHECK(ids.size() == 2);
        bool sawBoot = false;
        bool sawRun = false;
        for (const cascade::usb::UsbId& id : ids) {
            CHECK(id.vid == 0x04B4);
            if (id.pid == 0x00F3) { sawBoot = true; }
            if (id.pid == 0x00F1) { sawRun = true; }
        }
        CHECK(sawBoot);
        CHECK(sawRun);
    }

    // =====================================================================
    // 2. THE FX3 IMAGE FORMAT, against a synthesised image.
    //
    //    Cypress AN76405: 'C' 'Y', imageCTL, imageType 0xB0, then repeated
    //    {uint32 words, uint32 address, words*4 bytes}, terminated by a
    //    zero-length record whose address field is the entry point, then the
    //    checksum: the sum of every data word.
    // =====================================================================
    {
        const auto appendU32 = [](std::vector<std::uint8_t>& v, std::uint32_t x) {
            for (int b = 0; b < 4; ++b) { v.push_back(static_cast<std::uint8_t>(x >> (8 * b))); }
        };
        std::vector<std::uint8_t> img = {'C', 'Y', 0x1C, 0xB0};
        std::uint32_t sum = 0;
        // Section one: two words at 0x40000000.
        appendU32(img, 2);
        appendU32(img, 0x40000000);
        appendU32(img, 0x11223344);
        sum += 0x11223344;
        appendU32(img, 0xAABBCCDD);
        sum += 0xAABBCCDD;
        // Section two: one word at 0x40001000.
        appendU32(img, 1);
        appendU32(img, 0x40001000);
        appendU32(img, 0x00000001);
        sum += 0x00000001;
        // The terminator and the checksum.
        appendU32(img, 0);
        appendU32(img, 0x400012FC);
        appendU32(img, sum);

        const rx888::Fx3Image parsed = rx888::parseFx3Image(img.data(), img.size());
        CHECK(parsed.valid);
        if (!parsed.valid) { std::printf("     parse error: %s\n", parsed.error.c_str()); }
        CHECK(parsed.sections.size() == 2);
        if (parsed.sections.size() == 2) {
            CHECK(parsed.sections[0].address == 0x40000000);
            CHECK(parsed.sections[0].bytes == 8);
            CHECK(parsed.sections[1].address == 0x40001000);
            CHECK(parsed.sections[1].bytes == 4);
        }
        CHECK(parsed.entry == 0x400012FC);
        CHECK(parsed.totalBytes == 12);
        CHECK(parsed.storedChecksum == sum);
        CHECK(parsed.computedChecksum == sum);

        // Each way it can be wrong is wrong by NAME, because "the firmware
        // did not load" with no reason is a support conversation nobody can
        // finish.
        std::vector<std::uint8_t> bad = img;
        bad[1] = 'X';
        CHECK(!rx888::parseFx3Image(bad.data(), bad.size()).valid);
        CHECK(rx888::parseFx3Image(bad.data(), bad.size()).error.find("CY signature") !=
              std::string::npos);

        bad = img;
        bad[3] = 0xB2;  // a VID:PID image, which the bootloader treats differently
        CHECK(rx888::parseFx3Image(bad.data(), bad.size()).error.find("normal FW binary") !=
              std::string::npos);

        bad = img;
        bad[bad.size() - 1] ^= 0x01;  // the stored checksum
        CHECK(rx888::parseFx3Image(bad.data(), bad.size()).error.find("checksum") !=
              std::string::npos);

        // A length field that points past the end must not be believed: it is
        // the one field in the format that can turn a truncated file into an
        // out-of-bounds read of 4 GB.
        bad = img;
        bad[4] = 0xFF;
        bad[5] = 0xFF;
        bad[6] = 0xFF;
        bad[7] = 0x0F;
        const rx888::Fx3Image over = rx888::parseFx3Image(bad.data(), bad.size());
        CHECK(!over.valid);
        CHECK(over.error.find("past the end") != std::string::npos);

        CHECK(!rx888::parseFx3Image(img.data(), 6).valid);
        CHECK(!rx888::parseFx3Image(nullptr, 0).valid);
    }

    // =====================================================================
    // 3. THE IMAGE FoxSDR ACTUALLY SHIPS. Its identity is pinned: a
    //    regenerated array that is not SDDC_FX3.img fails here rather than
    //    reaching a radio.
    // =====================================================================
    {
        CHECK(rx888::sddcFirmwareImageSize() == 146268);
        const rx888::Fx3Image img =
            rx888::parseFx3Image(rx888::sddcFirmwareImage(), rx888::sddcFirmwareImageSize());
        CHECK(img.valid);
        if (!img.valid) { std::printf("     shipped image: %s\n", img.error.c_str()); }
        // Read out of the file itself. Four records: the first is 0x8E0
        // words at 0x00000100 - the FX3's I-TCM, where the vectors and the
        // early startup live - and begins with an ARM `push {r4, lr}`; the
        // next two are the bulk of the code in SYSMEM at 0x40003000 and
        // 0x40013000.
        CHECK(img.sections.size() == 4);
        if (img.sections.size() == 4) {
            CHECK(img.sections[0].address == 0x00000100);
            CHECK(img.sections[0].bytes == 0x8E0 * 4);
            CHECK(img.sections[0].data[0] == 0x10);
            CHECK(img.sections[0].data[1] == 0x40);
            CHECK(img.sections[0].data[2] == 0x2D);
            CHECK(img.sections[0].data[3] == 0xE9);
            CHECK(img.sections[1].address == 0x40003000);
            CHECK(img.sections[1].bytes == 0x4000 * 4);
            CHECK(img.sections[2].address == 0x40013000);
        }
        CHECK(img.totalBytes == 146220);
        CHECK(img.entry == 0x40012CFC);
        CHECK(img.storedChecksum == 0xE5B16830);
        CHECK(img.computedChecksum == 0xE5B16830);
    }

    // =====================================================================
    // 4. OPENING A RADIO THAT IS ALREADY RUNNING ITS FIRMWARE, byte for
    //    byte. The order is RadioHandler.cpp's: read the hardware
    //    information first (:97-100), then the ADC clock
    //    (RX888R2Radio.cpp:47-51), then the front end (:64-90).
    // =====================================================================
    {
        FakeRx888Bus bus;
        bus.addStreamer("SDDC0001");
        std::shared_ptr<FakeRx888Usb> fake = bus.deviceFor(0x00F1);
        Rx888Source src;
        attach(src, bus);

        CHECK(src.open(""));
        CHECK(src.isOpen());
        CHECK(!src.firmwareWasUploaded());
        CHECK(src.model() == rx888::Model::Rx888mk2);
        CHECK(src.firmwareVersion() == 0x0202);
        CHECK(std::string(src.name()) == "RX888: RX888 mkII (serial SDDC0001)");

        const std::vector<Rx888ControlRecord> c = fake->controls();
        CHECK(c.size() == 7);
        // TESTFX3: IN, four bytes, value 0 (value 1 is the firmware's debug
        // mode - FX3handler.cpp:190-196 and USBhandler.c:504).
        CHECK(isControl("TESTFX3", at(c, 0), true, 0xAC, 0, 0));
        CHECK(sameBytes("TESTFX3", at(c, 0).data, {0x04, 0x02, 0x02, 0x00}));
        // The GPIO word, written whole and written FIRST: an SDDC device
        // keeps whatever the last application left it at.
        CHECK(isControl("GPIOFX3", at(c, 1), false, 0xAD, 0, 0));
        CHECK(sameBytes("GPIOFX3", at(c, 1).data, le32(0)));
        // STARTADC: the ADC clock in Hz, and the ONLY request given the long
        // timeout, because its handler sleeps a second
        // (SDDC_FX3/USBhandler.c:264-273).
        CHECK(isControl("STARTADC", at(c, 2), false, 0xB2, 0, 0));
        CHECK(sameBytes("STARTADC", at(c, 2).data, le32(64000000)));
        CHECK(at(c, 2).timeoutMs == 3000);
        // ... and every other request gets the ordinary one, which is short
        // enough that a whole RX888 teardown fits inside the shutdown budget.
        CHECK(at(c, 1).timeoutMs == 500);
        CHECK(at(c, 0).timeoutMs == 500);
        // HF mode: the tuner to standby, then the antenna switch, then the
        // two HF gains (RX888R2Radio.cpp:81-90).
        CHECK(isControl("TUNERSTDBY", at(c, 3), false, 0xB8, 0, 0));
        CHECK(sameBytes("TUNERSTDBY", at(c, 3).data, {0x00}));
        CHECK(isControl("GPIOFX3 (HF)", at(c, 4), false, 0xAD, 0, 0));
        CHECK(sameBytes("GPIOFX3 (HF)", at(c, 4).data, le32(0)));
        // SETARGFX3: the value in wValue, the argument number in wIndex, one
        // zero byte of payload (FX3handler.cpp:171-185). DAT31_ATT is 10.
        CHECK(isControl("DAT31_ATT", at(c, 5), false, 0xB6, 0, 10));
        CHECK(sameBytes("DAT31_ATT", at(c, 5).data, {0x00}));
        // AD8340_VGA is 11, and 0 dB of IF gain is the nearest entry of the
        // reference's own low-gain curve, 20*log10(0.059*(i+1)): i = 16 gives
        // 0.026 dB, which is the closest of the 127. The code is i+1 with no
        // high-gain bit (RX888R2Radio.cpp:163-169).
        {
            int want = 0;
            double bestErr = -1.0;
            for (int i = 0; i < 127; ++i) {
                const double db = (i > 18) ? 20.0 * std::log10(0.409 * (i - 18 + 3))
                                           : 20.0 * std::log10(0.059 * (i + 1));
                const double err = std::fabs(db);
                if (bestErr < 0.0 || err < bestErr) {
                    bestErr = err;
                    want = i;
                }
            }
            CHECK(want == 16);
            CHECK(isControl("AD8340_VGA", at(c, 6), false, 0xB6, want + 1, 11));
            CHECK(sameBytes("AD8340_VGA", at(c, 6).data, {0x00}));
        }

        // The opened state, as the panel will read it.
        CHECK(src.driverKey() == std::string("rx888"));
        CHECK(src.adcRateHz() == 64000000);
        CHECK_NEAR(src.sampleRateHz(), 8.0e6, 1.0);
        CHECK_NEAR(src.centerFrequencyHz(), 10.0e6, 1.0);
        CHECK(!src.vhfMode());
        CHECK(src.antenna() == "HF");
        CHECK(!src.autoGainSupported());
        CHECK(!src.setAutoGain(true));
        double lo = 0.0;
        double hi = 0.0;
        CHECK(src.frequencyRangeHz(lo, hi));
        CHECK_NEAR(lo, 10.0e3, 1.0);
        CHECK_NEAR(hi, 1750.0e6, 1.0);
        const std::vector<double> rates = src.supportedSampleRatesHz();
        CHECK(rates.size() == 5);
        if (rates.size() == 5) {
            CHECK_NEAR(rates[0], 2.0e6, 1.0);
            CHECK_NEAR(rates[4], 32.0e6, 1.0);
        }
        src.closeDevice();
    }

    // =====================================================================
    // 5. AN SDDC DEVICE THAT IS NOT A mk2 IS REFUSED, not driven anyway.
    // =====================================================================
    {
        FakeRx888Bus bus;
        bus.addStreamer("SDDC0002");
        std::shared_ptr<FakeRx888Usb> fake = bus.deviceFor(0x00F1);
        fake->model = 0x05;  // RX999
        Rx888Source src;
        attach(src, bus);
        CHECK(!src.open(""));
        CHECK(!src.isOpen());
        CHECK(std::string(src.lastError()).find("RX999") != std::string::npos);
        // Nothing was programmed: the refusal happened before the first write.
        const std::vector<Rx888ControlRecord> c = fake->controls();
        CHECK(c.size() == 1);
        CHECK(isControl("TESTFX3 only", at(c, 0), true, 0xAC, 0, 0));
        src.closeDevice();
    }

    // =====================================================================
    // 6. THE FIRMWARE PATH: a bootloader, the image, the jump, and the radio
    //    coming back as a different USB device.
    // =====================================================================
    {
        FakeRx888Bus bus;
        bus.addBootloader("0000000000000000", "SDDC0003");
        std::shared_ptr<FakeRx888Usb> boot = bus.deviceFor(0x00F3);
        std::shared_ptr<FakeRx888Usb> run = bus.deviceFor(0x00F1);
        Rx888Source src;
        attach(src, bus);

        // Before the upload the bus shows only the bootloader.
        CHECK(bus.list().size() == 1);
        CHECK(bus.list()[0].pid == 0x00F3);

        CHECK(src.open(""));
        CHECK(src.isOpen());
        CHECK(src.firmwareWasUploaded());
        CHECK(boot->jumped.load());
        // ... and afterwards only the radio.
        CHECK(bus.list().size() == 1);
        CHECK(bus.list()[0].pid == 0x00F1);

        const rx888::Fx3Image img =
            rx888::parseFx3Image(rx888::sddcFirmwareImage(), rx888::sddcFirmwareImageSize());
        CHECK(img.valid);
        CHECK(boot->jumpAddress.load() == img.entry);

        // EVERY BYTE OF THE IMAGE, AT THE RIGHT ADDRESS, IN CHUNKS THE
        // BOOTLOADER ACCEPTS. This is the block that would let a wrong chunk
        // size or a non-advancing address through if it only counted
        // transfers.
        const std::vector<FakeRx888Usb::BootWrite> writes = boot->bootWrites();
        CHECK(!writes.empty());
        std::size_t w = 0;
        bool layoutOk = true;
        std::size_t totalWritten = 0;
        for (const rx888::Fx3Section& s : img.sections) {
            std::uint32_t address = s.address;
            std::size_t left = s.bytes;
            while (left > 0 && w < writes.size()) {
                const FakeRx888Usb::BootWrite& bw = writes[w];
                const std::size_t expect = std::min<std::size_t>(left, 4096);
                if (bw.address != address || bw.data.size() != expect) {
                    if (layoutOk) {
                        std::printf(
                            "     boot write %zu: got address 0x%08x length %zu; want 0x%08x "
                            "length %zu\n",
                            w, static_cast<unsigned>(bw.address), bw.data.size(),
                            static_cast<unsigned>(address), expect);
                    }
                    layoutOk = false;
                    break;
                }
                if (std::memcmp(bw.data.data(), s.data + (s.bytes - left), expect) != 0) {
                    std::printf("     boot write %zu: the bytes are not the image's\n", w);
                    layoutOk = false;
                    break;
                }
                totalWritten += expect;
                address += static_cast<std::uint32_t>(expect);
                left -= expect;
                ++w;
            }
            if (!layoutOk) { break; }
        }
        CHECK(layoutOk);
        CHECK(w == writes.size());
        CHECK(totalWritten == img.totalBytes);
        for (const FakeRx888Usb::BootWrite& bw : writes) { CHECK(bw.data.size() <= 4096); }

        // The radio that came back was then opened and configured exactly as
        // one that had never needed the firmware.
        const std::vector<Rx888ControlRecord> c = run->controls();
        CHECK(c.size() == 7);
        CHECK(isControl("TESTFX3", at(c, 0), true, 0xAC, 0, 0));
        CHECK(isControl("STARTADC", at(c, 2), false, 0xB2, 0, 0));
        src.closeDevice();
    }

    // =====================================================================
    // 7. A BOOTLOADER THAT REFUSES THE IMAGE is a failure the user can act
    //    on, and NOT a device this driver has given up on: the usual cause
    //    is WinUSB bound to one identity and not the other, and the remedy
    //    is to run Zadig again and open it again.
    // =====================================================================
    {
        FakeRx888Bus bus;
        bus.addBootloader("0000000000000000", "SDDC0004");
        std::shared_ptr<FakeRx888Usb> boot = bus.deviceFor(0x00F3);
        boot->refuseWritesAtOrAbove.store(0x40000000);
        Rx888Source src;
        attach(src, bus);
        CHECK(!src.open(""));
        CHECK(!src.isOpen());
        CHECK(!boot->jumped.load());
        CHECK(std::string(src.lastError()).find("Zadig") != std::string::npos);
        CHECK(!src.deviceDead());
        src.closeDevice();
    }

    // =====================================================================
    // 8. A RADIO THAT TAKES THE FIRMWARE AND NEVER COMES BACK. The wait is
    //    BOUNDED, and what it says names the thing the user has to fix.
    //
    //    An FX3 that has been jumped is gone from the bus whatever happens
    //    next, so "it never came back" is a bootloader with nothing behind
    //    it - which is exactly what an RX888 looks like when WinUSB is bound
    //    to 04B4:00F3 and not to 04B4:00F1.
    // =====================================================================
    {
        FakeRx888Bus bus;
        bus.addBootloaderWithNoRadioBehindIt("0000000000000000");
        std::shared_ptr<FakeRx888Usb> boot = bus.deviceFor(0x00F3);
        Rx888Source src;
        attach(src, bus);

        const auto began = std::chrono::steady_clock::now();
        CHECK(!src.open(""));
        const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - began)
                              .count();
        std::printf("     the re-enumeration wait gave up after %lld ms\n",
                    static_cast<long long>(took));
        CHECK(boot->jumped.load());
        CHECK(!src.isOpen());
        // It waited the budget out, and not longer.
        CHECK(took >= 4500);
        CHECK(took < 8000);
        CHECK(Rx888Source::kFirmwareReenumerateBudget == std::chrono::milliseconds(5000));
        CHECK(std::string(src.lastError()).find("04B4:00F1") != std::string::npos);
        CHECK(std::string(src.lastError()).find("Zadig") != std::string::npos);
        src.closeDevice();
    }

    // =====================================================================
    // 9. THE GAINS. Every code is the reference's own arithmetic, recomputed
    //    here from its formulas rather than read off the implementation.
    // =====================================================================
    {
        FakeRx888Bus bus;
        bus.addStreamer("SDDC0006");
        std::shared_ptr<FakeRx888Usb> fake = bus.deviceFor(0x00F1);
        Rx888Source src;
        attach(src, bus);
        CHECK(src.open(""));
        fake->clearControls();

        // RX888R2Radio.cpp:24-37 builds hf_rf_steps[63 - i] = -0.5 * i and
        // :91-104 sends d = 63 - index, so the DAT-31 code IS the attenuation
        // in half-decibels. -10 dB is code 20.
        CHECK(src.setGainDb("HF ATT", -10.0));
        CHECK_NEAR(src.gainDb("HF ATT"), -10.0, 1e-9);
        CHECK(isControl("HF ATT -10", at(fake->controls(), 0), false, 0xB6, 20, 10));

        // Clamped, not refused (device_source.hpp's contract), at both ends.
        fake->clearControls();
        CHECK(src.setGainDb("HF ATT", -99.0));
        CHECK_NEAR(src.gainDb("HF ATT"), -31.5, 1e-9);
        CHECK(isControl("HF ATT clamp low", at(fake->controls(), 0), false, 0xB6, 63, 10));
        fake->clearControls();
        CHECK(src.setGainDb("HF ATT", +99.0));
        CHECK_NEAR(src.gainDb("HF ATT"), 0.0, 1e-9);
        CHECK(isControl("HF ATT clamp high", at(fake->controls(), 0), false, 0xB6, 0, 10));

        // The AD8340: the high-gain curve is 20*log10(0.409*(i-15)) with the
        // 0x80 bit set (RX888R2Radio.cpp:40-45 and :163-169). Ask for 20 dB
        // and check the driver picked the nearest entry AND sent that entry's
        // code.
        fake->clearControls();
        CHECK(src.setGainDb("HF IF", 20.0));
        {
            int want = 0;
            double bestErr = -1.0;
            for (int i = 0; i < 127; ++i) {
                const double db = (i > 18) ? 20.0 * std::log10(0.409 * (i - 18 + 3))
                                           : 20.0 * std::log10(0.059 * (i + 1));
                const double err = std::fabs(db - 20.0);
                if (bestErr < 0.0 || err < bestErr) {
                    bestErr = err;
                    want = i;
                }
            }
            const double wantDb = 20.0 * std::log10(0.409 * (want - 18 + 3));
            const int wantCode = 0x80 | (want - 18 + 3);
            CHECK(want > 18);
            CHECK_NEAR(src.gainDb("HF IF"), wantDb, 1e-6);
            CHECK(isControl("AD8340 20 dB", at(fake->controls(), 0), false, 0xB6, wantCode, 11));
        }

        // The VHF pair belongs to a front end that is not in circuit: the
        // value is REMEMBERED and nothing is sent to a tuner that is in
        // standby.
        fake->clearControls();
        CHECK(src.setGainDb("VHF RF", 20.7));
        CHECK_NEAR(src.gainDb("VHF RF"), 20.7, 1e-6);
        CHECK(src.setGainDb("VHF IF", 16.3));
        CHECK_NEAR(src.gainDb("VHF IF"), 16.3, 1e-6);
        CHECK(fake->controlCount() == 0);

        // An unknown gain is refused by name.
        CHECK(!src.setGainDb("LNA", 10.0));
        CHECK(std::string(src.lastError()).find("LNA") != std::string::npos);

        const std::vector<cascade::source::GainInfo> g = src.gains();
        CHECK(g.size() == 4);
        if (g.size() == 4) {
            CHECK(g[0].name == "HF ATT");
            CHECK_NEAR(g[0].minDb, -31.5, 1e-9);
            CHECK_NEAR(g[0].maxDb, 0.0, 1e-9);
            CHECK_NEAR(g[0].stepDb, 0.5, 1e-9);
            CHECK(g[1].name == "HF IF");
            CHECK(g[2].name == "VHF RF");
            CHECK_NEAR(g[2].minDb, 0.0, 1e-9);
            CHECK_NEAR(g[2].maxDb, 49.6, 1e-6);
            CHECK(g[3].name == "VHF IF");
            CHECK_NEAR(g[3].minDb, -4.7, 1e-6);
            CHECK_NEAR(g[3].maxDb, 40.8, 1e-6);
            // Every one of them is decibels: this radio publishes a dB value
            // for every step of every gain it has, unlike the Airspy R2.
            for (const cascade::source::GainInfo& gi : g) {
                CHECK(gi.unit == cascade::source::GainUnit::Decibels);
            }
        }
        src.closeDevice();
    }

    // =====================================================================
    // 10. THE GPIO CONTROLS. One word, written whole, every time.
    // =====================================================================
    {
        FakeRx888Bus bus;
        bus.addStreamer("SDDC0007");
        std::shared_ptr<FakeRx888Usb> fake = bus.deviceFor(0x00F1);
        Rx888Source src;
        attach(src, bus);
        CHECK(src.open(""));
        fake->clearControls();

        // Interface.h:70-104: DITH is bit 6, RAND bit 7, BIAS_HF bit 8,
        // BIAS_VHF bit 9, PGA_EN bit 16.
        CHECK(src.setDither(true));
        CHECK(src.dither());
        CHECK(sameBytes("DITH on", at(fake->controls(), 0).data, le32(1u << 6)));
        CHECK(src.setRandomiser(true));
        CHECK(src.randomiser());
        CHECK(sameBytes("RAND on", at(fake->controls(), 1).data, le32((1u << 6) | (1u << 7))));
        CHECK(src.setBiasT(true));
        CHECK(src.biasT());
        CHECK(sameBytes("BIAS_HF on", at(fake->controls(), 2).data,
                        le32((1u << 6) | (1u << 7) | (1u << 8))));
        CHECK(src.setVhfBiasT(true));
        CHECK(src.vhfBiasT());
        CHECK(src.setPga(true));
        CHECK(src.pga());
        CHECK(sameBytes("all on", at(fake->controls(), 4).data,
                        le32((1u << 6) | (1u << 7) | (1u << 8) | (1u << 9) | (1u << 16))));
        // And each one clears only its own bit.
        CHECK(src.setDither(false));
        CHECK(!src.dither());
        CHECK(sameBytes("DITH off", at(fake->controls(), 5).data,
                        le32((1u << 7) | (1u << 8) | (1u << 9) | (1u << 16))));

        // Closing leaves both antenna ports unpowered and the front end shut
        // down (Core/RadioHardware.cpp:17-22), which is what the next
        // application inherits.
        fake->clearControls();
        src.closeDevice();
        const std::vector<Rx888ControlRecord> c = fake->controls();
        CHECK(!c.empty());
        if (!c.empty()) {
            CHECK(sameBytes("SHDWN", c.back().data, le32(1u << 5)));
        }
    }

    // =====================================================================
    // 11. THE VHF FRONT END. RX888R2Radio.cpp:64-80 in its own order, and
    //     the tuner's 64-bit payload.
    // =====================================================================
    {
        FakeRx888Bus bus;
        bus.addStreamer("SDDC0008");
        std::shared_ptr<FakeRx888Usb> fake = bus.deviceFor(0x00F1);
        Rx888Source src;
        attach(src, bus);
        CHECK(src.open(""));
        CHECK(src.setGainDb("VHF RF", 25.4));
        CHECK(src.setGainDb("VHF IF", 23.1));
        fake->clearControls();

        // 145.5 MHz is above the ADC's 32 MHz Nyquist, so the tuner takes over.
        CHECK(src.setCenterFrequencyHz(145.5e6));
        CHECK(src.vhfMode());
        CHECK(src.antenna() == "VHF");
        const std::vector<Rx888ControlRecord> c = fake->controls();
        CHECK(c.size() == 7);
        // The HF path is muted FIRST (code 63 = -31.5 dB), then the antenna
        // switch, then the AD8340 to the reference's "high gain, 0 dB", then
        // the tuner's reference clock.
        CHECK(isControl("mute HF", at(c, 0), false, 0xB6, 63, 10));
        CHECK(isControl("VHF_EN", at(c, 1), false, 0xAD, 0, 0));
        CHECK(sameBytes("VHF_EN", at(c, 1).data, le32(1u << 15)));
        CHECK(isControl("AD8340 0x83", at(c, 2), false, 0xB6, 0x83, 11));
        CHECK(isControl("TUNERINIT", at(c, 3), false, 0xB4, 0, 0));
        CHECK(sameBytes("TUNERINIT", at(c, 3).data, le32(16000000)));
        // Then the tuner's own two gains, from what the user asked for
        // earlier: R82XX_ATTENUATOR is 1 and R82XX_VGA is 2, and the index IS
        // the wire value (RX888R2Radio.cpp:15-22).
        CHECK(isControl("R82XX_ATT", at(c, 4), false, 0xB6, 14, 1));   // 25.4 dB is entry 14
        CHECK(isControl("R82XX_VGA", at(c, 5), false, 0xB6, 10, 2));   // 23.1 dB is entry 10
        // And finally the LO. TUNERTUNE carries a uint64
        // (SDDC_FX3/USBhandler.c:354-362).
        CHECK(isControl("TUNERTUNE", at(c, 6), false, 0xB5, 0, 0));
        CHECK(sameBytes("TUNERTUNE", at(c, 6).data, le64(145500000)));

        // A retune INSIDE the VHF band is one transfer and nothing else.
        fake->clearControls();
        CHECK(src.setCenterFrequencyHz(146.0e6));
        CHECK(fake->controlCount() == 1);
        CHECK(isControl("retune", at(fake->controls(), 0), false, 0xB5, 0, 0));
        CHECK(sameBytes("retune", at(fake->controls(), 0).data, le64(146000000)));

        // A rate wider than twice the 4.57 MHz IF is COERCED rather than
        // refused, because the widest band the tuner's IF allows is a real
        // answer to "give me as much as you have" - and the reason is said.
        CHECK_NEAR(src.sampleRateHz(), 8.0e6, 1.0);
        CHECK(src.setSampleRateHz(32.0e6));
        CHECK_NEAR(src.sampleRateHz(), 8.0e6, 1.0);
        CHECK(std::string(src.lastError()).find("IF") != std::string::npos);

        // Coming back down switches the front end back and restores the two
        // HF gains the VHF switch overwrote.
        fake->clearControls();
        CHECK(src.setCenterFrequencyHz(10.0e6));
        CHECK(!src.vhfMode());
        const std::vector<Rx888ControlRecord> back = fake->controls();
        CHECK(back.size() == 4);
        CHECK(isControl("TUNERSTDBY", at(back, 0), false, 0xB8, 0, 0));
        CHECK(isControl("VHF_EN off", at(back, 1), false, 0xAD, 0, 0));
        CHECK(sameBytes("VHF_EN off", at(back, 1).data, le32(0)));
        CHECK(isControl("HF ATT restored", at(back, 2), false, 0xB6, 0, 10));
        CHECK(isControl("HF IF restored", at(back, 3), false, 0xB6, 17, 11));

        // The antenna follows the tuning rather than the other way round.
        CHECK(src.setAntenna("HF"));
        CHECK(!src.setAntenna("VHF"));
        CHECK(std::string(src.lastError()).find("follows the centre frequency") !=
              std::string::npos);
        CHECK(!src.setAntenna("LOOP"));
        src.closeDevice();
    }

    // =====================================================================
    // 12. TUNING LIMITS. An HF band that would not fit inside what the ADC
    //     digitises is refused with the arithmetic in the message, not
    //     clamped somewhere the user did not ask for.
    // =====================================================================
    {
        FakeRx888Bus bus;
        bus.addStreamer("SDDC0009");
        Rx888Source src;
        attach(src, bus);
        CHECK(src.open(""));
        CHECK(!src.setCenterFrequencyHz(1.0e3));
        CHECK(std::string(src.lastError()).find("outside") != std::string::npos);
        CHECK(!src.setCenterFrequencyHz(2.0e9));
        // At 8 MS/s a centre of 2 MHz would reach below zero.
        CHECK(!src.setCenterFrequencyHz(2.0e6));
        CHECK(std::string(src.lastError()).find("would not fit") != std::string::npos);
        // Narrow the rate and the same tune is fine.
        CHECK(src.setSampleRateHz(2.0e6));
        CHECK_NEAR(src.sampleRateHz(), 2.0e6, 1.0);
        CHECK(src.setCenterFrequencyHz(2.0e6));
        CHECK_NEAR(src.centerFrequencyHz(), 2.0e6, 1.0);
        // The widest rate has exactly one legal centre, and widening to it
        // moves the centre there rather than failing.
        CHECK(src.setSampleRateHz(32.0e6));
        CHECK_NEAR(src.sampleRateHz(), 32.0e6, 1.0);
        CHECK_NEAR(src.centerFrequencyHz(), 16.0e6, 1.0);
        // A rate between two of the five snaps to the nearer.
        CHECK(src.setSampleRateHz(5.0e6));
        CHECK_NEAR(src.sampleRateHz(), 4.0e6, 1.0);
        CHECK(!src.setSampleRateHz(-1.0));
        src.closeDevice();
    }

    // =====================================================================
    // 13. THE CONVERSION, STAGE 1: the fast structure against a brute-force
    //     rotate-filter-decimate written from the definition.
    // =====================================================================
    {
        std::vector<float> taps = rx888::halfBandTaps(49);
        // The half-band property, exactly: every even OFFSET from the centre
        // is zero, and the centre is a half.
        const std::size_t centre = 24;
        for (std::size_t i = 0; i < taps.size(); ++i) {
            const std::size_t off = (i > centre) ? i - centre : centre - i;
            if (off != 0 && off % 2 == 0) { CHECK(taps[i] == 0.0f); }
        }
        CHECK_NEAR(taps[centre], 0.5, 1e-6);
        double sum = 0.0;
        for (const float t : taps) { sum += t; }
        CHECK_NEAR(sum, 1.0, 1e-6);

        Lcg rng;
        std::vector<float> x(4096);
        for (float& v : x) { v = rng.next(); }

        rx888::QuarterRotateHalfBand fast(49);
        std::vector<std::complex<float>> got(fast.outputCapacity(x.size()));
        const std::size_t n = fast.process(x.data(), x.size(), got.data());
        const std::vector<std::complex<float>> want = bruteRotateHalfBand(x, taps);
        CHECK(n == want.size());
        double worst = 0.0;
        for (std::size_t i = 0; i < std::min(n, want.size()); ++i) {
            worst = std::max(worst, static_cast<double>(std::abs(got[i] - want[i])));
        }
        std::printf("     stage 1 vs brute force: worst |difference| %.3e\n", worst);
        CHECK(worst < 1e-6);

        // AND THE SAME ANSWER WHATEVER THE BLOCK BOUNDARIES ARE, which is the
        // property the reader thread depends on: a transfer is not a signal
        // boundary, and the rotation's four-phase pattern and the decimation
        // grid both have to survive one.
        rx888::QuarterRotateHalfBand split(49);
        std::vector<std::complex<float>> pieces;
        std::size_t pos = 0;
        const std::size_t sizes[] = {1, 2, 3, 7, 64, 511, 1000};
        int si = 0;
        while (pos < x.size()) {
            const std::size_t take = std::min(sizes[si++ % 7], x.size() - pos);
            std::vector<std::complex<float>> tmp(split.outputCapacity(take));
            const std::size_t m = split.process(x.data() + pos, take, tmp.data());
            pieces.insert(pieces.end(), tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(m));
            pos += take;
        }
        CHECK(pieces.size() == n);
        bool identical = pieces.size() == n;
        for (std::size_t i = 0; identical && i < pieces.size(); ++i) {
            identical = pieces[i] == got[i];
        }
        CHECK(identical);
    }

    // =====================================================================
    // 14. THE CONVERSION, END TO END: frequency, amplitude, image rejection.
    //
    //     The tone frequencies are chosen so that the wanted output and its
    //     image are both WHOLE NUMBERS OF CYCLES across the analysis window.
    //     A rectangular correlation then reads each one with no leakage from
    //     the other at all, which is the only way to measure a rejection
    //     deeper than the window's own sidelobes.
    // =====================================================================
    {
        constexpr double kAdc = 64.0e6;
        constexpr std::size_t kAnalyse = 65536;
        constexpr std::size_t kSkip = 4096;

        // --- the whole band, no decimation past stage 1 -------------------
        // 10.5 MHz of a 0-32 MHz band; stage 1 puts 16 MHz at DC, so the
        // wanted lands at -5.5 MHz of a 32 MS/s stream and its image - the
        // negative half of the real spectrum, which an ideal analytic
        // conversion would delete entirely - at +5.5 MHz.
        const double wantedCycles = -11264.0 / 65536.0;  // -5.5 MHz at 32 MS/s
        const double imageCycles = 11264.0 / 65536.0;
        const std::vector<std::int16_t> tone =
            realTone(2 * (kAnalyse + kSkip) + 4096, 10.5e6, kAdc, 0.5);

        rx888::RealToIq conv;
        rx888::RealToIq::Config cfg;
        cfg.adcRateHz = 64000000;
        cfg.outputRateHz = 32.0e6;
        cfg.centerHz = 16.0e6;
        conv.configure(cfg);
        CHECK_NEAR(conv.rateHz(), 32.0e6, 1.0);
        std::vector<std::complex<float>> out(conv.outputCapacity(tone.size()));
        const std::size_t n = conv.process(tone.data(), tone.size(), out.data());
        CHECK(n >= kSkip + kAnalyse);

        const std::complex<double> wanted = binOf(out.data() + kSkip, kAnalyse, wantedCycles);
        const std::complex<double> image = binOf(out.data() + kSkip, kAnalyse, imageCycles);
        const double rejection = dbOf(std::abs(image) / std::abs(wanted));
        std::printf("     32 MS/s: |wanted| %.6f, |image| %.3e, rejection %.1f dB\n",
                    std::abs(wanted), std::abs(image), rejection);
        // A full-scale real sine comes out with magnitude 1.0, so half scale
        // comes out at 0.5: the 1/32768 and the analytic doubling together.
        CHECK_NEAR(std::abs(wanted), 0.5, 0.005);
        // PINNED. This is what the 49-tap stage-1 half-band gives at this
        // offset; it is the number to watch if that length ever changes.
        CHECK(rejection < -80.0);
        // Nothing where nothing should be: the DC bin of a single off-centre
        // tone is the ADC's own offset, which a synthesised cosine does not
        // have.
        CHECK(std::abs(binOf(out.data() + kSkip, kAnalyse, 0.0)) < 0.002);

        // --- a decimated band, tuned off centre --------------------------
        // The same radio at 8 MS/s centred on 10 MHz: a 10.5 MHz tone must
        // land at +0.5 MHz, which is +4096/65536 cycles at 8 MS/s.
        rx888::RealToIq conv2;
        rx888::RealToIq::Config cfg2;
        cfg2.adcRateHz = 64000000;
        cfg2.outputRateHz = 8.0e6;
        cfg2.centerHz = 10.0e6;
        conv2.configure(cfg2);
        CHECK_NEAR(conv2.rateHz(), 8.0e6, 1.0);
        const std::vector<std::int16_t> tone2 =
            realTone(8 * (kAnalyse + kSkip) + 4096, 10.5e6, kAdc, 0.5);
        std::vector<std::complex<float>> out2(conv2.outputCapacity(tone2.size()));
        const std::size_t n2 = conv2.process(tone2.data(), tone2.size(), out2.data());
        CHECK(n2 >= kSkip + kAnalyse);
        const std::complex<double> w2 =
            binOf(out2.data() + kSkip, kAnalyse, 4096.0 / 65536.0);
        std::printf("     8 MS/s at 10 MHz: tone at +0.5 MHz reads %.6f\n", std::abs(w2));
        CHECK_NEAR(std::abs(w2), 0.5, 0.01);
        // ... and nowhere else. The mirror image of the tune offset is where
        // a quadrature error would put a ghost.
        CHECK(std::abs(binOf(out2.data() + kSkip, kAnalyse, -4096.0 / 65536.0)) < 5e-4);

        // --- the VHF spectrum inversion ----------------------------------
        // The R828D's IF is inverted (RadioHandler.cpp:266-270), so the same
        // input with invertSpectrum set must put the tone at MINUS the offset.
        rx888::RealToIq conv3;
        rx888::RealToIq::Config cfg3 = cfg2;
        cfg3.invertSpectrum = true;
        conv3.configure(cfg3);
        std::vector<std::complex<float>> out3(conv3.outputCapacity(tone2.size()));
        const std::size_t n3 = conv3.process(tone2.data(), tone2.size(), out3.data());
        CHECK(n3 == n2);
        CHECK_NEAR(std::abs(binOf(out3.data() + kSkip, kAnalyse, -4096.0 / 65536.0)), 0.5, 0.01);
        CHECK(std::abs(binOf(out3.data() + kSkip, kAnalyse, 4096.0 / 65536.0)) < 5e-4);

        // --- every rate the radio offers ---------------------------------
        // A tone 1/8 of the way up from the centre must read as a tone 1/8 of
        // the way up from the centre, at all five.
        for (const double rate : rx888::supportedRatesHz(64000000)) {
            const double centre = 16.0e6;
            const double offset = rate / 8.0;
            rx888::RealToIq c;
            rx888::RealToIq::Config cc;
            cc.adcRateHz = 64000000;
            cc.outputRateHz = rate;
            cc.centerHz = centre;
            c.configure(cc);
            const std::size_t need =
                static_cast<std::size_t>(64.0e6 / rate) * (8192 + 1024) + 4096;
            const std::vector<std::int16_t> t = realTone(need, centre + offset, kAdc, 0.25);
            std::vector<std::complex<float>> o(c.outputCapacity(t.size()));
            const std::size_t m = c.process(t.data(), t.size(), o.data());
            CHECK(m >= 1024 + 8192);
            if (m >= 1024 + 8192) {
                const double mag = std::abs(binOf(o.data() + 1024, 8192, 1024.0 / 8192.0));
                std::printf("     %.0f MS/s: tone at +fs/8 reads %.4f\n", rate / 1e6, mag);
                CHECK_NEAR(mag, 0.25, 0.01);
            }
        }

        // --- the ADC randomiser ------------------------------------------
        // fft_mt_r2iq.h:189-204: the LTC2208 inverts bits 15..1 of any sample
        // whose bit 0 is set. Randomise the input, tell the conversion, and
        // the output must be what the plain input gave.
        std::vector<std::int16_t> randomised = tone2;
        for (std::int16_t& v : randomised) {
            if ((v & 1) != 0) { v = static_cast<std::int16_t>(v ^ static_cast<std::int16_t>(0xFFFE)); }
        }
        rx888::RealToIq conv4;
        rx888::RealToIq::Config cfg4 = cfg2;
        cfg4.randomised = true;
        conv4.configure(cfg4);
        std::vector<std::complex<float>> out4(conv4.outputCapacity(randomised.size()));
        const std::size_t n4 = conv4.process(randomised.data(), randomised.size(), out4.data());
        CHECK(n4 == n2);
        bool same = n4 == n2;
        for (std::size_t i = 0; same && i < n4; ++i) { same = out4[i] == out2[i]; }
        CHECK(same);
    }

    // =====================================================================
    // 15. STREAMING: every sample, once, in order, and converted the same
    //     way a second RealToIq converts the same bytes.
    // =====================================================================
    {
        FakeRx888Bus bus;
        bus.addStreamer("SDDC0010");
        std::shared_ptr<FakeRx888Usb> fake = bus.deviceFor(0x00F1);
        Rx888Source src;
        attach(src, bus);
        CHECK(src.open(""));

        // Eight transfers of 4096 ADC samples each, filled with a ramp whose
        // every sample is distinguishable from every other.
        constexpr std::size_t kTransfers = 8;
        constexpr std::size_t kSamplesPer = 4096;
        std::vector<std::vector<std::int16_t>> blocks;
        for (std::size_t t = 0; t < kTransfers; ++t) {
            std::vector<std::int16_t> block(kSamplesPer);
            for (std::size_t i = 0; i < kSamplesPer; ++i) {
                const std::size_t k = t * kSamplesPer + i;
                block[i] = static_cast<std::int16_t>((k * 2654435761u) >> 17);
            }
            std::vector<std::uint8_t> raw(block.size() * 2);
            for (std::size_t i = 0; i < block.size(); ++i) {
                const std::uint16_t u = static_cast<std::uint16_t>(block[i]);
                raw[2 * i] = static_cast<std::uint8_t>(u & 0xFF);
                raw[2 * i + 1] = static_cast<std::uint8_t>(u >> 8);
            }
            fake->queueBulk(std::move(raw));
            blocks.push_back(std::move(block));
        }

        CHECK(src.start());
        CHECK(src.running());
        CHECK(src.selfPaced());
        // The transport was asked for exactly the ring this driver documents:
        // sixteen transfers of 131072 bytes on endpoint 0x81.
        CHECK(fake->lastBulkEndpoint() == 0x81);
        CHECK(fake->lastBulkBufferBytes() == 131072);
        CHECK(fake->lastBulkBufferCount() == 16);
        CHECK(fake->beginBulkCalls() == 1);

        // Drain everything the reader produced.
        std::vector<std::complex<float>> got;
        std::vector<std::complex<float>> chunk(4096);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        std::size_t idle = 0;
        while (std::chrono::steady_clock::now() < deadline && idle < 20) {
            const std::size_t m = src.read(chunk.data(), chunk.size());
            if (m == 0) {
                ++idle;
                continue;
            }
            idle = 0;
            got.insert(got.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(m));
        }

        // The same bytes through a second conversion configured the same way,
        // one call per transfer exactly as the reader does it.
        rx888::RealToIq ref;
        rx888::RealToIq::Config cfg;
        cfg.adcRateHz = 64000000;
        cfg.outputRateHz = 8.0e6;
        cfg.centerHz = 10.0e6;
        ref.configure(cfg);
        std::vector<std::complex<float>> want;
        std::vector<std::complex<float>> tmp(ref.outputCapacity(kSamplesPer));
        for (const std::vector<std::int16_t>& b : blocks) {
            const std::size_t m = ref.process(b.data(), b.size(), tmp.data());
            want.insert(want.end(), tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(m));
        }

        std::printf("     streamed %zu complex samples, expected %zu\n", got.size(), want.size());
        CHECK(got.size() == want.size());
        bool ordered = got.size() == want.size();
        std::size_t firstBad = 0;
        for (std::size_t i = 0; ordered && i < got.size(); ++i) {
            if (got[i] != want[i]) {
                ordered = false;
                firstBad = i;
            }
        }
        if (!ordered && got.size() == want.size()) {
            std::printf("     first difference at %zu: got (%g,%g) want (%g,%g)\n", firstBad,
                        got[firstBad].real(), got[firstBad].imag(), want[firstBad].real(),
                        want[firstBad].imag());
        }
        CHECK(ordered);
        CHECK(src.droppedTransfers() == 0);

        // STARTFX3 went out after the ring was queued, with the reference's
        // one-byte payload (Core/FX3Class.h:97, RadioHandler.h:149).
        bool sawStart = false;
        for (const Rx888ControlRecord& r : fake->controls()) {
            if (r.request == 0xAA) {
                sawStart = true;
                CHECK(sameBytes("STARTFX3", r.data, {0x00}));
            }
        }
        CHECK(sawStart);

        // A HEALTHY STOP IS PROMPT. The reader is inside a bounded readBulk,
        // so it notices within one of those; anything approaching
        // kReaderJoinWait here would mean the signal is not reaching it.
        const auto began = std::chrono::steady_clock::now();
        src.stop();
        const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - began)
                              .count();
        std::printf("     stop() took %lld ms\n", static_cast<long long>(took));
        CHECK(took < 500);
        CHECK(!src.running());
        CHECK(fake->endBulkCalls() >= 1);
        CHECK(!fake->readBulkInFlight());
        bool sawStop = false;
        for (const Rx888ControlRecord& r : fake->controls()) {
            if (r.request == 0xAB) {
                sawStop = true;
                CHECK(sameBytes("STOPFX3", r.data, {0x00}));
            }
        }
        CHECK(sawStop);
        src.closeDevice();
    }

    // =====================================================================
    // 16. A DEVICE THAT VANISHES MID-STREAM. The reader faults and LEAVES;
    //     it does not sit retrying a pipe that has failed while the source
    //     loop waits for samples that cannot come.
    // =====================================================================
    {
        FakeRx888Bus bus;
        bus.addStreamer("SDDC0011");
        std::shared_ptr<FakeRx888Usb> fake = bus.deviceFor(0x00F1);
        Rx888Source src;
        attach(src, bus);
        CHECK(src.open(""));
        fake->onExhausted.store(FakeRx888Usb::Exhausted::DeviceGone);
        CHECK(src.start());
        CHECK(waitFor([&src] { return src.faulted(); }, std::chrono::milliseconds(2000)));
        CHECK(src.deviceDead());
        CHECK(src.faultedWhile() == "reading samples");
        CHECK(std::string(src.lastError()).find("unplug it") != std::string::npos);
        std::complex<float> buf[16];
        CHECK(src.read(buf, 16) == 0);
        // Nothing further is sent to a dead device, whatever is asked of it.
        fake->clearControls();
        CHECK(!src.setCenterFrequencyHz(12.0e6));
        CHECK(!src.setGainDb("HF ATT", -6.0));
        CHECK(!src.setSampleRateHz(4.0e6));
        CHECK(!src.setDither(true));
        CHECK(!src.start());
        CHECK(fake->controlCount() == 0);
        src.stop();
        src.closeDevice();
    }

    // =====================================================================
    // 17. A READER THAT WILL NOT COME BACK. stop() must return on its own
    //     bound and ABANDON the thread - the whole reason the join is
    //     bounded. Elapsed time alone would still pass if the bound were
    //     deleted, so what is asserted is the abandonment COUNTER.
    // =====================================================================
    {
        FakeRx888Bus bus;
        bus.addStreamer("SDDC0012");
        std::shared_ptr<FakeRx888Usb> fake = bus.deviceFor(0x00F1);
        const unsigned long long before = Rx888Source::readersAbandoned();
        {
            Rx888Source src;
            attach(src, bus);
            CHECK(src.open(""));
            fake->onExhausted.store(FakeRx888Usb::Exhausted::Block);
            CHECK(src.start());
            // Let the reader get inside the blocking read.
            CHECK(waitFor([&fake] { return fake->readBulkInFlight(); },
                          std::chrono::milliseconds(1000)));
            const auto began = std::chrono::steady_clock::now();
            src.stop();
            const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - began)
                                  .count();
            std::printf("     abandoning stop() took %lld ms\n", static_cast<long long>(took));
            CHECK(took >= 900);   // it really did wait for the bound
            CHECK(took < 2500);   // ... and not for the thread
            CHECK(Rx888Source::readersAbandoned() == before + 1);
            CHECK(src.faulted());
            CHECK(std::string(src.lastError()).find("did not return") != std::string::npos);
            src.closeDevice();
        }
        // Let the stranded thread out. It is holding a leaked handle onto a
        // device the bus still owns, which is exactly the shape the
        // abandonment path is built for.
        fake->releaseBlock.store(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // =====================================================================
    // 18. NO DEVICE, AND THE WRONG DEVICE.
    // =====================================================================
    {
        FakeRx888Bus bus;
        bus.addStranger(0x0BDA, 0x2838);
        Rx888Source src;
        attach(src, bus);
        CHECK(!src.open(""));
        CHECK(!src.isOpen());
        CHECK(std::string(src.lastError()).find("no RX888 found") != std::string::npos);
        // Everything is safe on a source that never opened.
        src.stop();
        std::complex<float> buf[4];
        CHECK(src.read(buf, 4) == 0);
        CHECK(!src.start());
        CHECK(!src.setSampleRateHz(8.0e6));
        CHECK(!src.setCenterFrequencyHz(10.0e6));
        CHECK(!src.setGainDb("HF ATT", 0.0));
        CHECK(!src.setBiasT(true));
        src.closeDevice();
    }
    {
        FakeRx888Bus bus;
        bus.addStreamer("SDDC0013");
        Rx888Source src;
        attach(src, bus);
        CHECK(!src.open("serial=ffffffff"));
        CHECK(std::string(src.lastError()).find("ffffffff") != std::string::npos);
        CHECK(!src.open("index=4"));
        CHECK(std::string(src.lastError()).find("index 4") != std::string::npos);
        // The tail of a serial is what a user reads off a listing.
        CHECK(src.open("serial=0013"));
        CHECK(src.isOpen());
        // A second open on the same object is refused rather than leaking the
        // first handle.
        CHECK(!src.open(""));
        CHECK(std::string(src.lastError()).find("already has a device open") != std::string::npos);
        src.closeDevice();
    }

    // =====================================================================
    // 19. A CONTROL TRANSFER THAT FAILS condemns the device.
    // =====================================================================
    {
        FakeRx888Bus bus;
        bus.addStreamer("SDDC0014");
        std::shared_ptr<FakeRx888Usb> fake = bus.deviceFor(0x00F1);
        Rx888Source src;
        attach(src, bus);
        CHECK(src.open(""));
        fake->failingRequests.push_back(0xB6);  // SETARGFX3
        CHECK(!src.setGainDb("HF ATT", -6.0));
        CHECK(src.faulted());
        CHECK(src.deviceDead());
        CHECK(src.faultedWhile() == "setting the HF attenuator");
        src.closeDevice();
    }
    {
        // An open that fails half way through leaves nothing behind.
        FakeRx888Bus bus;
        bus.addStreamer("SDDC0015");
        std::shared_ptr<FakeRx888Usb> fake = bus.deviceFor(0x00F1);
        fake->failingRequests.push_back(0xB2);  // STARTADC
        Rx888Source src;
        attach(src, bus);
        CHECK(!src.open(""));
        CHECK(!src.isOpen());
        CHECK(src.faulted());
        CHECK(src.faultedWhile() == "setting the ADC clock");
        src.closeDevice();
    }

    // =====================================================================
    // 20. THE STREAM-HEALTH LINE, in the shared format every source writes.
    // =====================================================================
    {
        FakeRx888Bus bus;
        bus.addStreamer("SDDC0016");
        std::shared_ptr<FakeRx888Usb> fake = bus.deviceFor(0x00F1);
        Rx888Source src;
        attach(src, bus);
        CHECK(src.open(""));
        CHECK(src.streamHealthLine().empty());  // nothing read yet
        // The window is left at its full minute ON PURPOSE: the reader's own
        // reporting would otherwise flush the tally and reset it, and the
        // line this block is reading is the one the tally produces. (The
        // shortened window exists for a test that wants to watch the reader
        // report; this one wants the numbers.)
        std::vector<std::uint8_t> raw(8192, 0);
        for (int i = 0; i < 4; ++i) { fake->queueBulk(raw); }
        CHECK(src.start());
        std::vector<std::complex<float>> chunk(4096);
        CHECK(waitFor([&] { return src.read(chunk.data(), chunk.size()) > 0; },
                      std::chrono::milliseconds(2000)));
        src.stop();
        const std::string line = src.streamHealthLine();
        std::printf("     %s\n", line.c_str());
        CHECK(line.find("source: stream health - reads ") == 0);
        CHECK(line.find("with samples ") != std::string::npos);
        CHECK(line.find("overflows ") != std::string::npos);
        CHECK(line.find("longest gap ") != std::string::npos);
        src.closeDevice();
    }

    return testSummary("test_rx888_source");
}
