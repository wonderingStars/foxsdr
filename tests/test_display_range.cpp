// Tests for core/display_range.hpp - the dB window the spectrum and the waterfall are drawn
// through, and the choice between the program's own window and the user's (0.99.76).
//
// The dislike behind it (a French user, 0.99.75, RTL-SDR V4): "the spectrum and the waterfall are
// very milky, one cannot see the signals". The owner: "allow the users to set it themselves to
// what they prefer by right clicking on it". What is pinned here is the arithmetic under that
// menu: the floor and ceiling the sliders may reach, the ten decibels the ceiling stays above the
// floor, which of the two ends gives way, what Automatic and Manual each put in force, what a
// reset puts back, and what a saved file means by a mode.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/display_range.hpp"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

#include "core/config.hpp"
#include "test_check.hpp"

using cascade::core::clampManualRange;
using cascade::core::DbPair;
using cascade::core::DisplayRange;
using cascade::core::RangeMode;

namespace {

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kInf = std::numeric_limits<float>::infinity();

void testDefaultsAgreeWithTheConfig() {
    std::printf("  the standard window is the config's default pair\n");
    // core/config.hpp does not include display_range.hpp, so its two default numbers are
    // literals; a drift between them would make Automatic mean one thing on a fresh install and
    // another after a reset.
    const cascade::core::AppConfig c{};
    CHECK(c.dbMin == cascade::core::kDefaultFloorDb);
    CHECK(c.dbMax == cascade::core::kDefaultCeilingDb);
    CHECK(cascade::core::kDefaultCeilingDb - cascade::core::kDefaultFloorDb >=
          cascade::core::kMinRangeSpanDb);
    // The standard window is reachable with both sliders, and the bounds are in order.
    CHECK(cascade::core::kDefaultFloorDb >= cascade::core::kFloorMinDb &&
          cascade::core::kDefaultFloorDb <= cascade::core::kFloorMaxDb);
    CHECK(cascade::core::kDefaultCeilingDb >= cascade::core::kCeilingMinDb &&
          cascade::core::kDefaultCeilingDb <= cascade::core::kCeilingMaxDb);
    CHECK(cascade::core::kFloorMinDb < cascade::core::kFloorMaxDb);
    CHECK(cascade::core::kCeilingMinDb < cascade::core::kCeilingMaxDb);
}

void testClampKeepsALegalPairAsItIs() {
    std::printf("  a legal pair is left alone, whichever end moved\n");
    for (const bool floorMoved : {true, false}) {
        const DbPair p = clampManualRange(-80.0f, -20.0f, floorMoved);
        CHECK(p.floorDb == -80.0f);
        CHECK(p.ceilingDb == -20.0f);
    }
    // Exactly the minimum span is legal: "at least 10 dB".
    const DbPair tight = clampManualRange(-50.0f, -40.0f, true);
    CHECK(tight.floorDb == -50.0f);
    CHECK(tight.ceilingDb == -40.0f);
}

void testTheMovedEndGivesWay() {
    std::printf("  the ceiling stays 10 dB above the floor: the end that moved gives way\n");
    // The floor dragged up past ceiling - 10: the floor is pushed back, the ceiling does not move
    // under the user's other hand.
    const DbPair f = clampManualRange(-25.0f, -30.0f, true);
    CHECK(f.ceilingDb == -30.0f);
    CHECK(f.floorDb == -40.0f);
    // The ceiling dragged down below floor + 10: the ceiling is pushed up, the floor stays.
    const DbPair c = clampManualRange(-60.0f, -65.0f, false);
    CHECK(c.floorDb == -60.0f);
    CHECK(c.ceilingDb == -50.0f);
    // One dB short of the span is pushed to the span, not tolerated.
    const DbPair onShort = clampManualRange(-50.0f, -41.0f, true);
    CHECK(onShort.ceilingDb - onShort.floorDb >= cascade::core::kMinRangeSpanDb);
    CHECK(onShort.floorDb == -51.0f);
}

void testBoundsAreEnforced() {
    std::printf("  the sliders' own bounds are enforced\n");
    CHECK(clampManualRange(-300.0f, 0.0f, true).floorDb == cascade::core::kFloorMinDb);
    CHECK(clampManualRange(50.0f, 0.0f, true).floorDb <= cascade::core::kFloorMaxDb);
    CHECK(clampManualRange(-110.0f, -500.0f, false).ceilingDb >= cascade::core::kCeilingMinDb);
    CHECK(clampManualRange(-110.0f, 500.0f, false).ceilingDb == cascade::core::kCeilingMaxDb);
    // The two bounds overlap in the middle (-100..-20): a floor at its top with a ceiling at its
    // bottom is a conflict the span rule has to settle, on whichever end the hand moved.
    const DbPair fl = clampManualRange(cascade::core::kFloorMaxDb, cascade::core::kCeilingMinDb, true);
    CHECK(fl.ceilingDb == cascade::core::kCeilingMinDb);
    CHECK(fl.floorDb == cascade::core::kCeilingMinDb - cascade::core::kMinRangeSpanDb);
    const DbPair ce = clampManualRange(cascade::core::kFloorMaxDb, cascade::core::kCeilingMinDb, false);
    CHECK(ce.floorDb == cascade::core::kFloorMaxDb);
    CHECK(ce.ceilingDb == cascade::core::kFloorMaxDb + cascade::core::kMinRangeSpanDb);
}

void testNotANumber() {
    std::printf("  a pair that is not a number is the standard window\n");
    for (const bool floorMoved : {true, false}) {
        for (const float bad : {kNaN, kInf, -kInf}) {
            const DbPair a = clampManualRange(bad, -20.0f, floorMoved);
            const DbPair b = clampManualRange(-80.0f, bad, floorMoved);
            CHECK(a == DbPair{});
            CHECK(b == DbPair{});
        }
    }
}

void testEveryPairComesOutLegal() {
    std::printf("  a sweep of 40,000 pairs: every result is in bounds and 10 dB apart\n");
    int illegal = 0;
    int untouchedMoved = 0;
    int total = 0;
    for (float f = -220.0f; f <= 60.0f; f += 1.7f) {
        for (float c = -220.0f; c <= 80.0f; c += 1.9f) {
            for (const bool floorMoved : {true, false}) {
                ++total;
                const DbPair p = clampManualRange(f, c, floorMoved);
                const bool inBounds = p.floorDb >= cascade::core::kFloorMinDb &&
                                      p.floorDb <= cascade::core::kFloorMaxDb &&
                                      p.ceilingDb >= cascade::core::kCeilingMinDb &&
                                      p.ceilingDb <= cascade::core::kCeilingMaxDb;
                const bool apart = p.ceilingDb - p.floorDb >= cascade::core::kMinRangeSpanDb - 1e-3f;
                if (!inBounds || !apart) { ++illegal; }
                // The end that did NOT move is only ever changed by its own bounds, never by the
                // span rule: the user's other hand stays where it was.
                const float otherIn = floorMoved ? c : f;
                const float otherOut = floorMoved ? p.ceilingDb : p.floorDb;
                const float lo = floorMoved ? cascade::core::kCeilingMinDb : cascade::core::kFloorMinDb;
                const float hi = floorMoved ? cascade::core::kCeilingMaxDb : cascade::core::kFloorMaxDb;
                const float expected = otherIn < lo ? lo : (otherIn > hi ? hi : otherIn);
                if (otherOut != expected) { ++untouchedMoved; }
            }
        }
    }
    std::printf("    %d pairs, %d illegal, %d where the unmoved end was moved\n", total, illegal,
                untouchedMoved);
    CHECK(total > 40000);
    CHECK(illegal == 0);
    CHECK(untouchedMoved == 0);
}

void testAutoIgnoresTheManualPair() {
    std::printf("  Automatic puts the standard window in force, Manual the user's\n");
    const DbPair mine{-60.0f, -20.0f};
    CHECK(cascade::core::rangeInForce(RangeMode::Auto, mine) == DbPair{});
    CHECK(cascade::core::rangeInForce(RangeMode::Manual, mine) == mine);
    CHECK(cascade::core::rangeInForce(RangeMode::Auto, mine).floorDb == -110.0f);
    CHECK(cascade::core::rangeInForce(RangeMode::Auto, mine).ceilingDb == 0.0f);
}

void testTheStateObject() {
    std::printf("  the state: a choice switches to Manual, Automatic keeps it, Reset forgets it\n");
    DisplayRange r;
    CHECK(r.mode() == RangeMode::Auto);
    CHECK(!r.manual());
    CHECK(r.inForce() == DbPair{});
    CHECK(r.manualPair() == DbPair{});

    // Moving a slider is a choice of a range: it is Manual from then on.
    r.setManual(-60.0f, -20.0f, true);
    CHECK(r.mode() == RangeMode::Manual);
    CHECK(r.manual());
    CHECK(r.inForce() == (DbPair{-60.0f, -20.0f}));

    // The ten decibels is enforced through the state too.
    r.setManual(-15.0f, -20.0f, true);
    CHECK(r.inForce().ceilingDb - r.inForce().floorDb >= cascade::core::kMinRangeSpanDb);
    CHECK(r.inForce().floorDb == -30.0f);
    r.setManual(-60.0f, -20.0f, true);

    // Back to Automatic: the standard window is in force, and the pair is remembered for the
    // next time Manual is chosen.
    r.setMode(RangeMode::Auto);
    CHECK(r.mode() == RangeMode::Auto);
    CHECK(r.inForce() == DbPair{});
    CHECK(r.manualPair() == (DbPair{-60.0f, -20.0f}));
    r.setMode(RangeMode::Manual);
    CHECK(r.inForce() == (DbPair{-60.0f, -20.0f}));

    // Reset: Automatic, and the remembered pair is forgotten too.
    r.reset();
    CHECK(r.mode() == RangeMode::Auto);
    CHECK(r.inForce() == DbPair{});
    CHECK(r.manualPair() == DbPair{});
    r.setMode(RangeMode::Manual);
    CHECK(r.inForce() == DbPair{});
}

void testRestoreRefusesAnIllegalPair() {
    std::printf("  restoring from a file: a legal pair is kept, an illegal one is the standard window\n");
    DisplayRange r;
    r.restore(RangeMode::Manual, DbPair{-70.0f, -15.0f});
    CHECK(r.mode() == RangeMode::Manual);
    CHECK(r.inForce() == (DbPair{-70.0f, -15.0f}));
    // The loader has already refused these; restore() does not rely on it.
    r.restore(RangeMode::Manual, DbPair{-20.0f, -25.0f});
    CHECK(r.inForce() == DbPair{});
    r.restore(RangeMode::Manual, DbPair{kNaN, -25.0f});
    CHECK(r.inForce() == DbPair{});
    // Automatic with a remembered pair keeps the pair.
    r.restore(RangeMode::Auto, DbPair{-70.0f, -15.0f});
    CHECK(r.mode() == RangeMode::Auto);
    CHECK(r.inForce() == DbPair{});
    CHECK(r.manualPair() == (DbPair{-70.0f, -15.0f}));
}

void testWhatAFileMeans() {
    std::printf("  a saved mode: the word wins, and a file from before it keeps the user's range\n");
    const DbPair standard{};
    const DbPair mine{-95.0f, -20.0f};
    CHECK(cascade::core::resolveRangeMode("auto", standard) == RangeMode::Auto);
    CHECK(cascade::core::resolveRangeMode("manual", standard) == RangeMode::Manual);
    // The word is the user's choice even where the pair looks like the other.
    CHECK(cascade::core::resolveRangeMode("auto", mine) == RangeMode::Auto);
    CHECK(cascade::core::resolveRangeMode("manual", mine) == RangeMode::Manual);
    // A file from before the key existed: a range somebody set with the Display sliders is theirs.
    CHECK(cascade::core::resolveRangeMode("", mine) == RangeMode::Manual);
    CHECK(cascade::core::resolveRangeMode("", standard) == RangeMode::Auto);
    // A word nobody wrote is not a word.
    CHECK(cascade::core::resolveRangeMode("sideways", mine) == RangeMode::Manual);
    CHECK(cascade::core::resolveRangeMode("sideways", standard) == RangeMode::Auto);
    CHECK(cascade::core::resolveRangeMode("MANUAL", standard) == RangeMode::Auto);
    // Names, and the round trip through them.
    CHECK(std::string(cascade::core::rangeModeName(RangeMode::Auto)) == "auto");
    CHECK(std::string(cascade::core::rangeModeName(RangeMode::Manual)) == "manual");
    for (const RangeMode m : {RangeMode::Auto, RangeMode::Manual}) {
        CHECK(cascade::core::resolveRangeMode(cascade::core::rangeModeName(m), mine) == m);
    }
}

}  // namespace

int main() {
    std::printf("test_display_range\n");
    testDefaultsAgreeWithTheConfig();
    testClampKeepsALegalPairAsItIs();
    testTheMovedEndGivesWay();
    testBoundsAreEnforced();
    testNotANumber();
    testEveryPairComesOutLegal();
    testAutoIgnoresTheManualPair();
    testTheStateObject();
    testRestoreRefusesAnIllegalPair();
    testWhatAFileMeans();
    return testSummary("test_display_range");
}
