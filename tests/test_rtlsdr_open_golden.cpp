// test_rtlsdr_open_golden.cpp - A HEALTHY DONGLE'S WIRE TRAFFIC IS PINNED, BYTE FOR BYTE.
//
// The re-open work (fix/rtlsdr-reopen) shares the chip bring-up and the setters with the plain open, and its review verified by reading that a healthy dongle's
// open, tune, gain, rate, IF bandwidth, direct sampling, bias tee, crystal correction and stream start send the same control transfers as before. This holds that
// reading as a test: the full list of OUT transfers of one scripted session is compared with a golden list (tests/rtlsdr_open_golden.inc), recorded from the
// driver as it was when the review verified it. Any later edit that adds, drops, reorders or alters one write of a healthy dongle is a red line naming the first
// transfer that differs.
//   `test_rtlsdr_open_golden --dump` prints the list in the form the .inc file holds (to refresh it on purpose, never to make a red test pass).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "source/rtlsdr_source.hpp"
#include "test_check.hpp"
#include "usb/usb_fake.hpp"

using cascade::source::RtlSdrSource;
using cascade::usb::FakeUsbDevice;

namespace {

const char* const kGolden[] = {
#include "rtlsdr_open_golden.inc"
};

std::vector<std::string> session() {
    auto f = std::make_unique<FakeUsbDevice>();
    f->answerIn(0, 0x0034, 0x0600, {0x69, 0x00, 0x02, 0x00, 0xA4});
    f->answerIn(0, 0x00a0, 0x0600, {0x00});
    f->onBeginBulk = [](FakeUsbDevice& d) { d.bulkQueue.push_back(std::vector<std::uint8_t>(2048, 0x80)); };
    FakeUsbDevice* fake = f.get();
    RtlSdrSource src;
    CHECK(src.openWithTransport(std::move(f), "golden R820T dongle"));
    CHECK(src.setFreqCorrectionPpm(7));
    CHECK(src.setSampleRateHz(1024000.0));
    CHECK(src.setIfBandwidthHz(1500000));
    CHECK(src.setGainDb("TUNER", 20.0));
    CHECK(src.setGainDb("LNA", 10.0));
    CHECK(src.setAutoGain(false));
    CHECK(src.setCenterFrequencyHz(433920000.0));
    CHECK(src.setDirectSamplingChoice(3));
    CHECK(src.setCenterFrequencyHz(7100000.0));
    CHECK(src.setDirectSamplingChoice(1));
    CHECK(src.setCenterFrequencyHz(98000000.0));
    CHECK(src.setBiasT(true));
    CHECK(src.setBiasT(false));
    CHECK(src.start());
    src.stop();
    std::vector<std::string> v;
    for (const cascade::usb::FakeControl& c : fake->writes()) { v.push_back(c.text()); }
    return v;
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> have = session();
    if (argc > 1 && std::strcmp(argv[1], "--dump") == 0) {
        for (const std::string& s : have) { std::printf("    \"%s\",\n", s.c_str()); }
        return 0;
    }
    std::printf("test_rtlsdr_open_golden: %zu writes, golden %zu\n", have.size(), sizeof(kGolden) / sizeof(kGolden[0]));
    const std::size_t want = sizeof(kGolden) / sizeof(kGolden[0]);
    CHECK(have.size() > 100);
    CHECK(have.size() == want);
    for (std::size_t i = 0; i < have.size() && i < want; ++i) {
        if (have[i] != kGolden[i]) {
            std::printf("FIRST DIFFERENCE at write %zu: golden %s, have %s\n", i, kGolden[i], have[i].c_str());
            CHECK(have[i] == kGolden[i]);
            break;
        }
    }
    return testSummary("test_rtlsdr_open_golden");
}
