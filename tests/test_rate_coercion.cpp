// Tests for gui/rate_follow_status.hpp's rate-coercion rules - what the device
// panel says when a radio runs at a different sample rate from the one asked,
// on a call that SUCCEEDED.
//
// THE DEFECT (bug hunt 2026-09-24, rx888-pluto-001). Two drivers coerce a
// rate rather than refuse it and explain why through lastError() on a call
// that RETURNS TRUE: the RX888 above the ADC's Nyquist (its VHF tuner's IF
// allows at most 8 MS/s) and the Pluto above its board's maximum. Every call
// site in the GUI read lastError() only when the call returned false, so the
// explanation never reached the panel: picking 32 MS/s in VHF mode silently
// gave 8 MS/s, and the Rate combo pointed at the entry asked for. Worse, an
// HF tune at more than 8 MS/s crossing into VHF narrowed the radio inside
// setCenterFrequencyHz, and the retune path never asked the DSP chain to
// follow - the chain stayed at the old rate while the radio ran at 8 MS/s.
//
// What each leg proves:
//  [C1] A coerced rate on a call that returned true is said, in the driver's
//       own words, and the outcome carries the rate the radio actually runs
//       at (the one the Rate combo must point at).
//  [C2] A later rate that lands where asked takes that line away again - and
//       only that line: any other device error is left alone.
//  [C3] A coercion the driver does not explain (its lastError() unchanged -
//       including the same coercion asked twice) is still said, truthfully.
//  [C4] A refusal is the driver's reason, exactly as before.
//  [C5] A retune that narrows the rate is noticed (so the chain is told to
//       follow) and explained; one that leaves the rate alone is not.
//  [C6] The tolerance: a rate a driver lands on to within a hair is not a
//       coercion, and a NaN readback is never "the same rate".
//  [C7] The same two paths on the real Rx888Source, over its fake USB bus.
//
// The source here is a small fake with the RX888's coercion contract (the
// driver itself is held to that contract by test_rx888_source); these rules
// care only about what a source says through IqSource.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdio>
#include <limits>
#include <string>

#include "gui/rate_follow_status.hpp"
#include "rx888_fake_usb.hpp"
#include "source/iq_source.hpp"
#include "source/rx888_source.hpp"
#include "test_check.hpp"

namespace {

using cascade::gui::applySourceRate;
using cascade::gui::kRateCoercedPrefix;
using cascade::gui::rateAfterRetune;
using cascade::gui::rateLandedElsewhere;

bool startsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }
bool contains(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }

// The RX888's contract, reduced to what a panel can see: above 32 MHz the
// widest rate is 8 MS/s, a wider request is COERCED (true, with a reason), and
// a tune from HF into VHF at a wider rate narrows the rate as it goes.
class CoercingSource : public cascade::source::IqSource {
public:
    double rate = 16.0e6;
    double centre = 10.0e6;
    std::string err;
    // Off: coerces without a word, the way a driver with no reason to give
    // (or one that forgot) would.
    bool explains = true;

    bool start() override { return true; }
    void stop() override {}
    bool running() const override { return false; }
    bool selfPaced() const override { return true; }
    double sampleRateHz() const override { return rate; }
    bool setSampleRateHz(double hz) override {
        if (!(hz > 0.0)) {
            err = "setSampleRateHz() requires a positive rate";
            return false;
        }
        if (vhf() && hz > kMaxVhf) {
            rate = kMaxVhf;
            if (explains) {
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                              "the VHF tuner's IF allows 8 MS/s; %.3f MS/s was coerced to it",
                              hz / 1e6);
                err = buf;
            }
            return true;
        }
        rate = hz;
        return true;
    }
    double centerFrequencyHz() const override { return centre; }
    bool setCenterFrequencyHz(double hz) override {
        centre = hz;
        if (vhf() && rate > kMaxVhf) {
            rate = kMaxVhf;
            if (explains) { err = "the sample rate was narrowed to 8 MS/s for this tune"; }
        }
        return true;
    }
    std::size_t read(std::complex<float>*, std::size_t) override { return 0; }
    const char* name() const override { return "coercing fake"; }
    const char* lastError() const override { return err.c_str(); }

private:
    static constexpr double kMaxVhf = 8.0e6;
    bool vhf() const { return centre >= 32.0e6; }
};

void testACoercedRateIsSaid() {  // [C1]
    CoercingSource src;
    src.centre = 145.5e6;  // VHF
    src.rate = 8.0e6;
    const auto r = applySourceRate(src, 32.0e6, std::string());
    CHECK(r.ok);
    CHECK(r.landedHz == 8.0e6);  // the combo points HERE, not at 32
    CHECK(startsWith(r.sourceError, kRateCoercedPrefix));
    CHECK(contains(r.sourceError, "32.000 MS/s was coerced"));
}

void testALaterExactRateClearsOnlyTheCoercion() {  // [C2]
    CoercingSource src;
    src.centre = 145.5e6;
    const auto coerced = applySourceRate(src, 32.0e6, std::string());
    CHECK(startsWith(coerced.sourceError, kRateCoercedPrefix));
    // 4 MS/s is inside what VHF allows: it lands where asked.
    const auto exact = applySourceRate(src, 4.0e6, coerced.sourceError);
    CHECK(exact.ok);
    CHECK(exact.landedHz == 4.0e6);
    CHECK(exact.sourceError.empty());
    // A device error already on the line is not the rate's to take away.
    const auto other = applySourceRate(src, 2.0e6, "bias tee write failed");
    CHECK(other.sourceError == "bias tee write failed");
    // An exact rate with nothing on the line leaves it empty.
    CHECK(applySourceRate(src, 2.0e6, std::string()).sourceError.empty());
}

void testAnUnexplainedCoercionIsStillSaid() {  // [C3]
    CoercingSource src;
    src.centre = 145.5e6;
    src.explains = false;
    src.err = "an old, unrelated failure";
    const auto r = applySourceRate(src, 32.0e6, std::string());
    CHECK(r.ok);
    CHECK(startsWith(r.sourceError, kRateCoercedPrefix));
    // The stale text is NOT passed off as the reason...
    CHECK(!contains(r.sourceError, "old, unrelated"));
    // ...the plain fact is said instead: where it runs, and what was asked.
    CHECK(contains(r.sourceError, "8 MS/s"));
    CHECK(contains(r.sourceError, "32 MS/s"));

    // THE SAME COERCION TWICE: the driver writes the identical sentence, so
    // "its words changed" cannot be the test - and after an exact rate took
    // the first line away, the repeat must still be said.
    CoercingSource again;
    again.centre = 145.5e6;
    const auto first = applySourceRate(again, 32.0e6, std::string());
    const auto cleared = applySourceRate(again, 4.0e6, first.sourceError);
    CHECK(cleared.sourceError.empty());
    const auto repeat = applySourceRate(again, 32.0e6, cleared.sourceError);
    CHECK(startsWith(repeat.sourceError, kRateCoercedPrefix));
}

void testARefusalIsTheDriversReason() {  // [C4]
    CoercingSource src;
    const auto r = applySourceRate(src, -1.0, "whatever was there");
    CHECK(!r.ok);
    CHECK(r.sourceError == "setSampleRateHz() requires a positive rate");
}

void testARetuneThatNarrowsTheRateIsFollowed() {  // [C5]
    CoercingSource src;  // HF, 16 MS/s
    const double before = src.sampleRateHz();
    const std::string errBefore = src.lastError();
    CHECK(src.setCenterFrequencyHz(145.5e6));
    const auto r = rateAfterRetune(src, before, errBefore, std::string());
    CHECK(r.rateMoved);           // the caller must make the chain follow
    CHECK(r.rateHz == 8.0e6);
    CHECK(startsWith(r.sourceError, kRateCoercedPrefix));
    CHECK(contains(r.sourceError, "narrowed to 8 MS/s"));

    // A retune inside VHF leaves the rate alone: nothing to follow, and the
    // line is not the retune's to touch.
    const double before2 = src.sampleRateHz();
    const std::string errBefore2 = src.lastError();
    CHECK(src.setCenterFrequencyHz(146.0e6));
    const auto r2 = rateAfterRetune(src, before2, errBefore2, "gain write failed");
    CHECK(!r2.rateMoved);
    CHECK(r2.sourceError == "gain write failed");
}

// [C7] THE REAL DRIVER, on its fake bus: the RX888's own coercion sentences
// reach the line through both paths, and the retune path sees the rate move.
void testTheRealRx888() {
    cascade::test::FakeRx888Bus bus;
    bus.addStreamer("SDDC0008");
    cascade::source::Rx888Source src;
    src.setTransportForTest([&bus] { return bus.list(); },
                            [&bus](const std::string& path, std::string& error) {
                                return bus.open(path, error);
                            });
    CHECK(src.open(""));
    // HF at 16 MS/s, then a preset into VHF: the tuner narrows the rate.
    CHECK(applySourceRate(src, 16.0e6, std::string()).sourceError.empty());
    CHECK(src.setCenterFrequencyHz(10.0e6));
    const double before = src.sampleRateHz();
    const std::string errBefore = src.lastError();
    CHECK(src.setCenterFrequencyHz(145.5e6));
    const auto tuned = rateAfterRetune(src, before, errBefore, std::string());
    CHECK(tuned.rateMoved);
    CHECK_NEAR(tuned.rateHz, 8.0e6, 1.0);
    CHECK(startsWith(tuned.sourceError, kRateCoercedPrefix));
    CHECK(contains(tuned.sourceError, "narrowed"));

    // In VHF, 32 MS/s from the Rate combo is coerced to 8 and said.
    const auto set = applySourceRate(src, 32.0e6, std::string());
    CHECK(set.ok);
    CHECK_NEAR(set.landedHz, 8.0e6, 1.0);
    CHECK(startsWith(set.sourceError, kRateCoercedPrefix));
    CHECK(contains(set.sourceError, "coerced to it"));
    src.closeDevice();
}

void testTheTolerance() {  // [C6]
    CHECK(!rateLandedElsewhere(2.4e6, 2.4e6));
    CHECK(!rateLandedElsewhere(2.4e6, 2.4e6 + 0.5));      // a driver's rounding
    CHECK(!rateLandedElsewhere(61.44e6, 61.44e6 + 3.0));  // 0.05 ppm
    CHECK(rateLandedElsewhere(2.4e6, 2.5e6));
    CHECK(rateLandedElsewhere(32.0e6, 8.0e6));
    CHECK(rateLandedElsewhere(2.4e6, std::numeric_limits<double>::quiet_NaN()));
}

}  // namespace

int main() {
    testACoercedRateIsSaid();
    testALaterExactRateClearsOnlyTheCoercion();
    testAnUnexplainedCoercionIsStillSaid();
    testARefusalIsTheDriversReason();
    testARetuneThatNarrowsTheRateIsFollowed();
    testTheRealRx888();
    testTheTolerance();
    return testSummary("test_rate_coercion");
}
