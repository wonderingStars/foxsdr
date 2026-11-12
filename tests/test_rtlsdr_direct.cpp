// test_rtlsdr_direct.cpp - THE USER'S DIRECT-SAMPLING CHOICE FOR THE NATIVE RTL-SDR DRIVER, PROVED ON THE WIRE (engine serial lane A, row R11).
//
// The driver is opened against usb::FakeUsbDevice (as test_rtlsdr_source.cpp does) and each assertion is about the demodulator writes it put on the wire. librtlsdr's direct-sampling sequence (the
// oracle of Rtl2832u::setDirectSampling): zero-IF off (demod page 1, 0xb1 = 0x1a and 0x15 = 0x00), one ADC (page 0, 0x08 = 0x4d), the input pin (page 0, 0x06 = 0x80 for the I branch, 0x90 for the Q branch,
// 0x80 again with the mode off).
//   A  the default: automatic, off at 100 MHz, nothing written;  B  forced Q: the range widens to 100 kHz - 14.4 MHz, a tune at 7.1 MHz writes the Q-branch sequence, and the radio reads back 7.1 MHz;
//   C  forced I: the other pin;  D  never: the radio leaves direct sampling, and 7.1 MHz is refused again with the tuner's own sentence;  E  a forced radio refuses 100 MHz and its range says so;
//   F  a Blog V4 (an upconverter): not supported, refused;  G  values outside 0..3 and a closed radio are refused.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "source/rtl2832u.hpp"
#include "source/rtlsdr_source.hpp"
#include "test_check.hpp"
#include "usb/usb_fake.hpp"

using cascade::source::RtlSdrSource;
using cascade::usb::FakeControl;
using cascade::usb::FakeUsbDevice;

namespace {

const std::vector<std::uint8_t> kTunerAnswer = {0x69, 0x00, 0x02, 0x00, 0xA4};

bool isDemodWrite(const FakeControl& c) { return c.out && c.index < 0x100; }
struct DemodWrite { std::uint8_t page, addr, value; };
std::vector<DemodWrite> demodWrites(const std::vector<FakeControl>& v) {
    std::vector<DemodWrite> out;
    for (const FakeControl& c : v) {
        if (!isDemodWrite(c) || c.data.empty()) { continue; }
        out.push_back({static_cast<std::uint8_t>(c.index & 0x0f), static_cast<std::uint8_t>(c.value >> 8), c.data[0]});
    }
    return out;
}
bool wrote(const std::vector<DemodWrite>& w, std::uint8_t page, std::uint8_t addr, int value = -1) {
    for (const DemodWrite& d : w) {
        if (d.page == page && d.addr == addr && (value < 0 || d.value == value)) { return true; }
    }
    return false;
}
void dressAsR820T(FakeUsbDevice& fake) {
    fake.answerIn(0, 0x0034, 0x0600, kTunerAnswer);
    fake.answerIn(0, 0x00a0, 0x0600, {0x00});
}

}  // namespace

int main() {
    std::printf("test_rtlsdr_direct\n");
    RtlSdrSource src;
    auto fake = std::make_unique<FakeUsbDevice>();
    dressAsR820T(*fake);
    FakeUsbDevice* f = fake.get();
    CHECK(src.openWithTransport(std::move(fake), "fake R820T dongle"));

    // A: the default
    CHECK(src.directSamplingChoice() == 0 && src.directSamplingMode() == 0 && src.directSamplingSupported());
    f->clear();
    CHECK(src.setCenterFrequencyHz(100000000.0));
    CHECK(src.directSamplingMode() == 0);
    CHECK(!wrote(demodWrites(f->writes()), 0, 0x06, 0x90) && !wrote(demodWrites(f->writes()), 1, 0xb1, 0x1a));   // nothing of the direct sequence
    double lo = 0.0, hi = 0.0;
    CHECK(src.frequencyRangeHz(lo, hi) && lo == 24000000.0);
    CHECK(!src.setCenterFrequencyHz(7100000.0));   // as before: the tuner's own range stops a tune that low

    // B: forced Q branch
    f->clear();
    CHECK(src.setDirectSamplingChoice(3));
    CHECK(src.directSamplingChoice() == 3);
    CHECK(src.frequencyRangeHz(lo, hi) && lo == 100000.0 && hi == 14400000.0);
    f->clear();
    CHECK(src.setCenterFrequencyHz(7100000.0));
    {
        const std::vector<DemodWrite> w = demodWrites(f->writes());
        std::printf("  Q branch at 7.1 MHz: %zu demod writes; direct mode %d\n", w.size(), src.directSamplingMode());
        CHECK(src.directSamplingMode() == 2);
        CHECK(wrote(w, 1, 0xb1, 0x1a) && wrote(w, 1, 0x15, 0x00) && wrote(w, 0, 0x08, 0x4d) && wrote(w, 0, 0x06, 0x90));
        CHECK(!wrote(w, 0, 0x06, 0x80));
    }
    CHECK(src.centerFrequencyHz() == 7100000.0);

    // C: forced I branch (the tuner is asleep already: only the pin changes)
    f->clear();
    CHECK(src.setDirectSamplingChoice(2));
    {
        const std::vector<DemodWrite> w = demodWrites(f->writes());
        CHECK(src.directSamplingMode() == 1);
        CHECK(wrote(w, 0, 0x06, 0x80));
        CHECK(!wrote(w, 0, 0x06, 0x90));
    }
    CHECK(src.centerFrequencyHz() == 7100000.0);

    // E: forced direct refuses a frequency the first Nyquist zone cannot hold, with the range in the sentence
    CHECK(!src.setCenterFrequencyHz(100000000.0));
    CHECK(std::string(src.lastError()).find("tunes 0.100 - 14.400 MHz") != std::string::npos);
    CHECK(src.directSamplingMode() == 1);

    // D: never: back to the tuner. The radio is at 7.1 MHz, below the tuner's range, so the choice is taken and the next tune applies it
    CHECK(src.setDirectSamplingChoice(1));
    CHECK(src.directSamplingChoice() == 1);
    CHECK(src.frequencyRangeHz(lo, hi) && lo == 24000000.0);
    f->clear();
    CHECK(src.setCenterFrequencyHz(100000000.0));
    CHECK(src.directSamplingMode() == 0);
    CHECK(wrote(demodWrites(f->writes()), 0, 0x06, 0x80));          // the pin put back
    CHECK(!src.setCenterFrequencyHz(7100000.0));
    CHECK(std::string(src.lastError()).find("tunes 24.000") != std::string::npos);
    // and automatic again behaves as it did
    CHECK(src.setDirectSamplingChoice(0));
    CHECK(src.directSamplingMode() == 0 && src.frequencyRangeHz(lo, hi) && lo == 24000000.0);

    // G: bad values, a closed radio
    for (const int bad : {-1, 4, 99}) { CHECK(!src.setDirectSamplingChoice(bad)); }
    CHECK(src.directSamplingChoice() == 0);
    src.closeDevice();
    CHECK(!src.setDirectSamplingChoice(3));

    // F: a Blog V4 has an upconverter and never leaves its tuner (dressed as test_rtlsdr_source.cpp does: the two USB strings are what tell it from any other R828D)
    {
        RtlSdrSource v4;
        auto fk = std::make_unique<FakeUsbDevice>();
        fk->answerIn(0, 0x0074, 0x0600, kTunerAnswer);
        fk->answerIn(0, 0x00a0, 0x0600, {0x00});
        fk->answerIn(0x06, 0x0100, 0x0000, {0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40, 0xDA, 0x0B, 0x38, 0x28, 0x00, 0x01, 1, 5, 0x03, 0x01});
        fk->answerIn(0x06, 0x0300, 0x0000, {0x04, 0x03, 0x09, 0x04});
        auto str = [](const std::string& t) {
            std::vector<std::uint8_t> d = {static_cast<std::uint8_t>(2 + 2 * t.size()), 0x03};
            for (const char ch : t) { d.push_back(static_cast<std::uint8_t>(ch)); d.push_back(0); }
            return d;
        };
        fk->answerIn(0x06, 0x0301, 0x0409, str("RTLSDRBlog"));
        fk->answerIn(0x06, 0x0305, 0x0409, str("Blog V4"));
        CHECK(v4.openWithTransport(std::move(fk), "fake Blog V4"));
        CHECK(v4.tunerName() == std::string("R828D (RTL-SDR Blog V4)"));
        CHECK(!v4.directSamplingSupported());
        CHECK(!v4.setDirectSamplingChoice(3) && !v4.setDirectSamplingChoice(2) && !v4.setDirectSamplingChoice(1));
        CHECK(std::string(v4.lastError()).find("upconverter") != std::string::npos);
        CHECK(v4.directSamplingChoice() == 0);
        CHECK(v4.setDirectSamplingChoice(0));     // automatic is what it already is
        v4.closeDevice();
    }
    return testSummary("test_rtlsdr_direct");
}
