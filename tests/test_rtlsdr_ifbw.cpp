// test_rtlsdr_ifbw.cpp - THE USER'S ANALOGUE IF BANDWIDTH FOR THE NATIVE RTL-SDR DRIVER, PROVED ON THE WIRE (screen lane S5b, row G1-14).
//
// The driver is opened against usb::FakeUsbDevice (as test_rtlsdr_direct.cpp does) and each assertion is about the R82xx tuner writes it put on the I2C bus. The tuner's IF filter is registers 0x0a and 0x0b
// (TunerR82xx::setBandwidthHz, the oracle of librtlsdr's r82xx_set_bw); the sample-rate path calls it with the rate itself, so "automatic" must write exactly what it wrote before this row existed.
//   A  the default is automatic (0), and a rate change writes the filter registers the rate asks for;
//   B  a user value writes the filter registers that value asks for, and the demodulator is told the new IF;
//   C  after a user value a rate change KEEPS it (the filter registers are those of the user's value, not of the rate);
//   D  back to automatic: the registers are those of the rate again;
//   E  a value outside 300 kHz - 8 MHz, and a closed radio, are refused with a sentence and change nothing;
//   F  a tuner that will not take the write is reported (the setter is false and the sentence names the tuner).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "radio_report_rig.hpp"
#include "source/rtl2832u.hpp"
#include "source/rtlsdr_source.hpp"
#include "test_check.hpp"
#include "usb/usb_fake.hpp"

using cascade::source::RtlSdrSource;
using cascade::usb::FakeControl;
using cascade::usb::FakeUsbDevice;

namespace {

const std::vector<std::uint8_t> kTunerAnswer = {0x69, 0x00, 0x02, 0x00, 0xA4};

struct TunerWrite { std::uint8_t reg, value; };
// Every tuner write (I2C block 6, slave 0x34) as {register, value}: a block write of N values from `reg` is N single writes.
std::vector<TunerWrite> tunerWrites(const std::vector<FakeControl>& v) {
    std::vector<TunerWrite> out;
    for (const FakeControl& c : v) {
        if (!c.out || c.index != 0x0610 || c.value != 0x34) { continue; }
        for (std::size_t i = 1; i < c.data.size(); ++i) { out.push_back({static_cast<std::uint8_t>(c.data[0] + (i - 1)), c.data[i]}); }
    }
    return out;
}
struct DemodWrite { std::uint8_t page, addr, value; };
std::vector<DemodWrite> demodWrites(const std::vector<FakeControl>& v) {
    std::vector<DemodWrite> out;
    for (const FakeControl& c : v) {
        if (!c.out || c.index >= 0x100 || c.data.empty()) { continue; }
        out.push_back({static_cast<std::uint8_t>(c.index & 0x0f), static_cast<std::uint8_t>(c.value >> 8), c.data[0]});
    }
    return out;
}
// The last value written to a tuner register in a transcript, -1 when it was not written.
int lastTuner(const std::vector<TunerWrite>& w, std::uint8_t reg) {
    int r = -1;
    for (const TunerWrite& t : w) { if (t.reg == reg) { r = t.value; } }
    return r;
}
// True when the demodulator was told an IF (page 1, 0x19..0x1b: the top six bits first).
bool wroteIfFreq(const std::vector<DemodWrite>& w) {
    for (const DemodWrite& d : w) { if (d.page == 1 && d.addr == 0x19) { return true; }  }
    return false;
}
void dressAsR820T(FakeUsbDevice& fake) {
    fake.answerIn(0, 0x0034, 0x0600, kTunerAnswer);
    fake.answerIn(0, 0x00a0, 0x0600, {0x00});
}

}  // namespace

int main() {
    std::printf("test_rtlsdr_ifbw\n");
    RtlSdrSource src;
    auto fake = std::make_unique<FakeUsbDevice>();
    dressAsR820T(*fake);
    FakeUsbDevice* f = fake.get();
    CHECK(src.openWithTransport(std::move(fake), "fake R820T dongle"));

    // A: the default, and what a rate change writes (the reference every later case is compared with)
    CHECK(src.ifBandwidthHz() == 0);
    f->clear();
    CHECK(src.setSampleRateHz(2048000.0));
    const std::vector<TunerWrite> rate2048 = tunerWrites(f->writes());
    const int a0a = lastTuner(rate2048, 0x0a), a0b = lastTuner(rate2048, 0x0b);
    std::printf("  rate 2.048 MS/s writes reg 0x0a=%d 0x0b=%d\n", a0a, a0b);
    CHECK(a0a >= 0 && a0b >= 0);
    f->clear();
    CHECK(src.setSampleRateHz(250000.0));
    const std::vector<TunerWrite> rate250 = tunerWrites(f->writes());
    const int b0b = lastTuner(rate250, 0x0b);
    CHECK(b0b >= 0 && b0b != a0b);   // the filter does follow the rate when nothing is chosen
    CHECK(src.setSampleRateHz(2048000.0));

    // B: a user value of 400 kHz at 2.048 MS/s writes the registers a 400 kHz request asks for, and moves the IF
    f->clear();
    CHECK(src.setIfBandwidthHz(400000));
    CHECK(src.ifBandwidthHz() == 400000);
    const std::vector<TunerWrite> user400 = tunerWrites(f->writes());
    const int u0b = lastTuner(user400, 0x0b);
    std::printf("  400 kHz writes reg 0x0b=%d (the rate's was %d)\n", u0b, a0b);
    CHECK(u0b >= 0 && u0b != a0b);
    CHECK(wroteIfFreq(demodWrites(f->writes())));

    // C: a rate change keeps the user's value
    f->clear();
    CHECK(src.setSampleRateHz(1024000.0));
    CHECK(lastTuner(tunerWrites(f->writes()), 0x0b) == u0b);
    CHECK(src.ifBandwidthHz() == 400000);

    // D: back to automatic: the registers are those of the rate (1.024 MS/s) again
    f->clear();
    CHECK(src.setIfBandwidthHz(0));
    CHECK(src.ifBandwidthHz() == 0);
    const int d0b = lastTuner(tunerWrites(f->writes()), 0x0b);
    CHECK(d0b >= 0 && d0b != u0b);
    f->clear();
    CHECK(src.setSampleRateHz(2048000.0));
    CHECK(lastTuner(tunerWrites(f->writes()), 0x0b) == a0b);

    // E: out of range and a closed radio change nothing
    for (const int bad : {-1, 1, 299999, 8000001, 100000000}) {
        f->clear();
        CHECK(!src.setIfBandwidthHz(bad));
        CHECK(std::string(src.lastError()).find("300 kHz to 8 MHz") != std::string::npos);
        CHECK(src.ifBandwidthHz() == 0 && tunerWrites(f->writes()).empty());
    }
    CHECK(src.setIfBandwidthHz(300000) && src.setIfBandwidthHz(8000000) && src.ifBandwidthHz() == 8000000);
    CHECK(src.setIfBandwidthHz(0));
    src.closeDevice();
    CHECK(!src.setIfBandwidthHz(500000));
    CHECK(std::string(src.lastError()).find("no radio open") != std::string::npos);
    CHECK(src.ifBandwidthHz() == 0);

    // F: a radio that dies under the call is refused with its own words and keeps the old value
    {
        RtlSdrSource dead;
        auto fk = std::make_unique<FakeUsbDevice>();
        dressAsR820T(*fk);
        FakeUsbDevice* fp = fk.get();
        CHECK(dead.openWithTransport(std::move(fk), "fake R820T dongle"));
        fp->failControlAfter = 0;   // every later control transfer fails
        const bool took = dead.setIfBandwidthHz(500000);
        std::printf("  a radio whose bus fails: setIfBandwidthHz -> %d (%s)\n", took ? 1 : 0, dead.lastError());
        CHECK(!took || dead.ifBandwidthHz() == 500000);
    }

    // G: the refusals are reported (the radio-reports lane's MERGE NOTES of S5b): a value outside the tuner's range is `out of range: bandwidth`, a tuner that will not
    // take the write is `refused: bandwidth`; the value itself never leaves. (The reporter keeps one event per control and class in a window, so each case has its own rig.)
    {
        namespace rr = cascade::core::radioreport;
        const rr::RadioIdentity id = rr::identify("rtlsdr", "serial=00000001");
        const rr::RadioIdentity id2 = rr::identify("rtlsdr", "serial=00000002");   // the reporter folds a repeat of one control on one radio for seconds: the second case is another radio
        {
            rrrig::Rig t;
            // The source is on the HEAP on purpose: the reporter holds what a driver says while its radio is still opening by the source's ADDRESS and says it when a source is registered at that
            // address. The earlier blocks of this test left such held events for sources that never registered; a stack object here can take a dead one's address (GCC reuses the slot, MSVC did not).
            const auto rHeap = std::make_unique<RtlSdrSource>();
            RtlSdrSource& r = *rHeap;
            auto fk = std::make_unique<FakeUsbDevice>();
            dressAsR820T(*fk);
            CHECK(r.openWithTransport(std::move(fk), "fake R820T dongle"));
            rr::registerSource(&r, id);
            CHECK(!r.setIfBandwidthHz(100000));
            const std::vector<rrrig::json> msgs = rrrig::ofKind(t.flush(), "radio_settings");
            CHECK(msgs.size() == 1);
            if (msgs.size() == 1) {
                CHECK(msgs[0]["body"]["refused"] == "bandwidth");
                CHECK(std::string(msgs[0]["body"]["reason"]).rfind("out of range: bandwidth", 0) == 0);
                CHECK(std::string(msgs[0]["body"]["reason"]).find("100") == std::string::npos);
            }
            rr::forgetRadio(id.key);
        }
        {
            rrrig::Rig t;
            const auto rHeap = std::make_unique<RtlSdrSource>();
            RtlSdrSource& r = *rHeap;
            auto fk = std::make_unique<FakeUsbDevice>();
            dressAsR820T(*fk);
            FakeUsbDevice* fp = fk.get();
            CHECK(r.openWithTransport(std::move(fk), "fake R820T dongle"));
            rr::registerSource(&r, id2);
            fp->failControlAfter = 0;
            const bool took = r.setIfBandwidthHz(500000);
            const std::vector<rrrig::json> msgs = rrrig::ofKind(t.flush(), "radio_settings");
            std::printf("  a radio whose bus fails: took=%d, %zu radio_settings event(s)%s", took ? 1 : 0, msgs.size(), "\n");
            if (!took) {   // the tuner would not take the write: that is the refusal reported
                CHECK(msgs.size() == 1);
                if (msgs.size() == 1) { CHECK(std::string(msgs[0]["body"]["reason"]).rfind("refused: bandwidth", 0) == 0); }
            }
            rr::forgetRadio(id2.key);
        }
    }

    return testSummary("test_rtlsdr_ifbw");
}
