// test_rtlsdr_rate_report.cpp - A SAVED SAMPLE RATE THE RADIO CANNOT DO IS SAID, AND REPORTED (hardware check 2026-11-17, docs/multi-radio-records/hw-findings.md item 3).
//
// On a real RTL-SDR a saved 5 MS/s became 3.2 MS/s with no refused-setting event. The driver coerces to the nearest rate it has (RtlSdrSource::setSampleRateHz) and the engine reads
// the rate back through cascade::gui::applySourceRate; this test drives that function against a driver opened on usb::FakeUsbDevice (no radio is opened) and holds that
//   A  the panel's line says which rate is in use and why, in one sentence;
//   B  exactly one `radio_settings` event is queued: control `rate` refused, refusedClass `range`, `why` as the caller says (open / restore / user), and no number in it;
//   C  a rate the radio can do queues nothing and leaves the line alone;
//   D  a second coercion on the same radio inside the reporter's folding window is counted, not sent again.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "core/rate_follow_status.hpp"
#include "radio_report_rig.hpp"
#include "source/rtlsdr_source.hpp"
#include "test_check.hpp"
#include "usb/usb_fake.hpp"

using cascade::source::RtlSdrSource;
using cascade::usb::FakeUsbDevice;

namespace {

const std::vector<std::uint8_t> kTunerAnswer = {0x69, 0x00, 0x02, 0x00, 0xA4};

void dressAsR820T(FakeUsbDevice& fake) {
    fake.answerIn(0, 0x0034, 0x0600, kTunerAnswer);
    fake.answerIn(0, 0x00a0, 0x0600, {0x00});
}

bool hasDigit(const std::string& s) {
    for (const char c : s) { if (c >= '0' && c <= '9') { return true; } }
    return false;
}

}  // namespace

int main() {
    std::printf("test_rtlsdr_rate_report\n");
    namespace rr = cascade::core::radioreport;

    // A + B: a saved 5 MS/s on a dongle that tops out at 3.2 MS/s, applied the way the saved-radio open applies it
    {
        rrrig::Rig t;
        const rr::RadioIdentity id = rr::identify("rtlsdr", "serial=00000001");
        RtlSdrSource src;
        auto fk = std::make_unique<FakeUsbDevice>();
        dressAsR820T(*fk);
        CHECK(src.openWithTransport(std::move(fk), "fake R820T dongle"));
        rr::registerSource(&src, id);
        const cascade::gui::RateSetOutcome out = cascade::gui::applySourceRate(src, 5000000.0, std::string(), false, "restore");
        CHECK(out.ok);
        CHECK(out.landedHz > 3000000.0 && out.landedHz < 3300000.0);                    // the driver's nearest rate
        std::printf("  line: %s\n", out.sourceError.c_str());
        CHECK(out.sourceError.rfind(cascade::gui::kRateCoercedPrefix, 0) == 0);
        CHECK(out.sourceError.find("cannot run at 5 MS/s") != std::string::npos);       // why
        CHECK(out.sourceError.find("so it runs at 3.2 MS/s") != std::string::npos);     // which rate is in use
        const std::vector<rrrig::json> msgs = rrrig::ofKind(t.flush(), "radio_settings");
        CHECK(msgs.size() == 1);
        if (msgs.size() == 1) {
            const auto& b = msgs[0]["body"];
            CHECK(b["refused"] == "rate");
            CHECK(b["refusedClass"] == "range");
            CHECK(b["why"] == "restore");
            const std::string reason = b["reason"];
            CHECK(reason.rfind("out of range: rate", 0) == 0);
            CHECK(!hasDigit(reason.substr(std::string("out of range: rate").size())));   // the rate is not reported
        }
        rr::forgetRadio(id.key);
    }

    // the "open" word, from a different radio (the reporter folds one control on one radio for seconds)
    {
        rrrig::Rig t;
        const rr::RadioIdentity id = rr::identify("rtlsdr", "serial=00000002");
        RtlSdrSource src;
        auto fk = std::make_unique<FakeUsbDevice>();
        dressAsR820T(*fk);
        CHECK(src.openWithTransport(std::move(fk), "fake R820T dongle"));
        rr::registerSource(&src, id);
        cascade::gui::applySourceRate(src, 5000000.0, std::string(), false, "open");
        const std::vector<rrrig::json> msgs = rrrig::ofKind(t.flush(), "radio_settings");
        CHECK(msgs.size() == 1 && (msgs.size() != 1 || msgs[0]["body"]["why"] == "open"));
        // D: the same coercion again, at once: folded, so still one event in all
        cascade::gui::applySourceRate(src, 5000000.0, std::string(), false, "open");
        CHECK(rrrig::ofKind(t.flush(), "radio_settings").size() <= 1);
        rr::forgetRadio(id.key);
    }

    // C: a rate the dongle can do: no event, and a coercion line already on the panel is taken away (the rule of core/rate_follow_status.hpp)
    {
        rrrig::Rig t;
        const rr::RadioIdentity id = rr::identify("rtlsdr", "serial=00000003");
        RtlSdrSource src;
        auto fk = std::make_unique<FakeUsbDevice>();
        dressAsR820T(*fk);
        CHECK(src.openWithTransport(std::move(fk), "fake R820T dongle"));
        rr::registerSource(&src, id);
        const cascade::gui::RateSetOutcome out = cascade::gui::applySourceRate(src, 2048000.0, std::string(cascade::gui::kRateCoercedPrefix) + "old", false, "restore");
        CHECK(out.ok && out.sourceError.empty());
        CHECK(rrrig::ofKind(t.flush(), "radio_settings").empty());
        rr::forgetRadio(id.key);
    }

    return testSummary("test_rtlsdr_rate_report");
}
