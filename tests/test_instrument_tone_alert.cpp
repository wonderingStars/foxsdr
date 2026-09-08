// test_instrument_tone_alert.cpp - the arithmetic behind the two-tone
// alerting receiver's face.
//
// The drawing cannot be run without a graphics context, so everything that
// could be got wrong lives in instrument_tone_alert_math.hpp and is pinned
// here: where a tone lands on the engraved scale, what an empty slot prints,
// how the deck divides at window sizes nobody tried, and the three cases the
// brief for these faces calls out - no reading, the largest figure a slot can
// hold, an overlong text, and a rectangle of no size at all.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <cmath>
#include <cstring>
#include <limits>
#include <string>

#include "gui/instrument_tone_alert_math.hpp"
#include "test_check.hpp"

namespace ta = cascade::gui::tone_alert;

namespace {

std::string hz(double v) {
    char buf[32];
    ta::formatHz(buf, sizeof buf, v);
    return std::string(buf);
}

std::string sec(double v) {
    char buf[32];
    ta::formatSec(buf, sizeof buf, v);
    return std::string(buf);
}

}  // namespace

int main() {
    // --- no reading is no reading, never a zero -----------------------------
    //
    // The rule this whole family of faces exists to obey. An empty slot is a
    // slot the plugin did not fill, and 0 Hz is not a tone.
    CHECK(!ta::haveTone(0.0));
    CHECK(!ta::haveTone(-1.0));
    CHECK(!ta::haveTone(std::numeric_limits<double>::quiet_NaN()));
    CHECK(!ta::haveTone(std::numeric_limits<double>::infinity()));
    CHECK(ta::haveTone(1122.5));
    CHECK(!ta::haveDuration(0.0));
    CHECK(!ta::haveDuration(-0.5));
    CHECK(!ta::haveDuration(std::numeric_limits<double>::quiet_NaN()));
    CHECK(ta::haveDuration(1.02));

    // The gate every slot on the face goes through. This one is a regression
    // test with a screenshot behind it: the frequency readouts were gated on
    // `have` and the two tone bars were not, so a face with nothing to report
    // printed "--" under two bars indexed at 746.8 and 879.0 Hz.
    CHECK(ta::slotOrNone(false, 746.8) == 0.0);
    CHECK(ta::slotOrNone(false, 879.0) == 0.0);
    CHECK(ta::slotOrNone(true, 746.8) == 746.8);
    CHECK(!ta::haveTone(ta::slotOrNone(false, 746.8)));
    CHECK(!ta::haveDuration(ta::slotOrNone(false, 1.02)));
    for (double v = -5000.0; v < 5000.0; v += 37.0) {
        // Whatever the struct holds, a face with no reading has no slots.
        CHECK(!ta::haveTone(ta::slotOrNone(false, v)));
        CHECK(ta::toneFrac(ta::slotOrNone(false, v)) == 0.0);
    }

    CHECK(hz(0.0) == "--");
    CHECK(hz(-3.0) == "--");
    CHECK(hz(std::numeric_limits<double>::quiet_NaN()) == "--");
    CHECK(sec(0.0) == "--");
    CHECK(sec(std::numeric_limits<double>::quiet_NaN()) == "--");
    // Nothing that prints "--" may ever print a zero instead.
    CHECK(hz(0.0).find('0') == std::string::npos);
    CHECK(sec(0.0).find('0') == std::string::npos);

    // --- the figures the readouts print -------------------------------------
    CHECK(hz(1122.5) == "1122.5");
    CHECK(hz(330.5) == "330.5");
    CHECK(sec(1.02) == "1.02");
    CHECK(sec(2.98) == "2.98");
    // Rounded to the precision the decoder in front of this face actually
    // measures to, not to whatever the double happens to hold.
    CHECK(hz(1122.46) == "1122.5");
    CHECK(sec(3.004) == "3.00");

    // --- the largest figure a slot can hold ---------------------------------
    //
    // values[] is a double, so the face has to survive one - and the buffer it
    // is printed into is 32 bytes. Neither may be overrun and neither may
    // produce a bar off the end of the panel.
    {
        const double big = std::numeric_limits<double>::max();
        char buf[32];
        ta::formatHz(buf, sizeof buf, big);
        CHECK(std::strlen(buf) < sizeof buf);
        CHECK(ta::haveTone(big));
        CHECK(ta::toneFrac(big) <= 1.0);
        CHECK(ta::overRange(big));
        char sb[32];
        ta::formatSec(sb, sizeof sb, big);
        CHECK(std::strlen(sb) < sizeof sb);
        // A tiny buffer must still be terminated and must not be written past.
        char small[4] = {'\0', '\0', '\0', '\0'};
        ta::formatHz(small, sizeof small, 1122.5);
        CHECK(std::strlen(small) < sizeof small);
        // Null and zero-capacity destinations are simply refused.
        ta::formatHz(nullptr, 0u, 1.0);
        ta::formatSec(nullptr, 0u, 1.0);
    }

    // --- where a tone sits on the engraved scale ----------------------------
    CHECK_NEAR(ta::toneFrac(ta::kScaleLoHz), 0.0, 1e-12);
    CHECK_NEAR(ta::toneFrac(ta::kScaleHiHz), 1.0, 1e-12);
    CHECK_NEAR(ta::toneFrac((ta::kScaleLoHz + ta::kScaleHiHz) * 0.5), 0.5, 1e-12);
    // Monotonic across the whole span: a higher tone is never drawn lower.
    {
        double prev = -1.0;
        bool rising = true;
        for (double f = 100.0; f <= 4200.0; f += 25.0) {
            const double t = ta::toneFrac(f);
            if (t < prev - 1e-12) { rising = false; }
            prev = t;
            CHECK(t >= 0.0 && t <= 1.0);
        }
        CHECK(rising);
    }
    // Clamped at both stops, and the clamp announces itself so a pinned index
    // is never read as a measurement of the stop's own frequency.
    CHECK_NEAR(ta::toneFrac(50.0), 0.0, 1e-12);
    CHECK_NEAR(ta::toneFrac(9000.0), 1.0, 1e-12);
    CHECK(ta::underRange(200.0));
    CHECK(!ta::underRange(ta::kScaleLoHz));
    CHECK(ta::overRange(3824.0));
    CHECK(!ta::overRange(ta::kScaleHiHz));
    // No tone is neither over nor under range - it is nothing at all.
    CHECK(!ta::overRange(0.0));
    CHECK(!ta::underRange(0.0));

    // The published two-tone tables this face is drawn for. The scale must
    // hold the traffic it exists to show: Motorola Quick Call II's lowest reed
    // (288.5) and the demo's own pair are all on-scale, while Reach's
    // single-tone channel 01 at 3824 Hz is off the top and says so.
    CHECK(!ta::underRange(288.5));
    CHECK(!ta::overRange(288.5));
    CHECK(!ta::overRange(1122.5) && !ta::underRange(1122.5));
    CHECK(!ta::overRange(2792.4));
    CHECK(ta::overRange(3824.0));

    // Every engraved number lands strictly inside the scale it is cut into,
    // so no tick is drawn on top of a stop.
    for (int i = 0; i < ta::kMajorTickCount; ++i) {
        const double t = ta::toneFrac(ta::kMajorTickHz[i]);
        CHECK(t > 0.0 && t < 1.0);
    }
    // The minor ticks step evenly from the bottom stop to the top one.
    CHECK_NEAR(std::fmod(ta::kScaleHiHz - ta::kScaleLoHz, ta::kMinorTickStepHz), 0.0, 1e-9);

    // --- the result flag ----------------------------------------------------
    CHECK(ta::resultOf("MATCHED") == ta::Result::kMatched);
    CHECK(ta::resultOf("UNMATCHED") == ta::Result::kUnmatched);
    CHECK(ta::resultOf("matched") == ta::Result::kMatched);
    CHECK(ta::resultOf("Unmatched") == ta::Result::kUnmatched);
    CHECK(ta::resultOf("") == ta::Result::kNone);
    CHECK(ta::resultOf(nullptr) == ta::Result::kNone);
    // MATCHED must not be seen inside a longer word - "UNMATCHED" ends in it,
    // and a prefix or substring test here would colour every unmatched page
    // as a match.
    CHECK(ta::resultOf("MATCHED PARTIALLY") == ta::Result::kOther);
    CHECK(ta::resultOf("MATCH") == ta::Result::kOther);
    CHECK(ta::resultOf("SINGLE TONE") == ta::Result::kOther);

    // --- an overlong text ---------------------------------------------------
    //
    // text[] is 64 bytes and a plugin may fill every one of them. Nothing here
    // may run off the bay it is drawn in: fitPx shrinks the line until it does
    // fit, or to the floor where a caption stops being a caption.
    {
        const float px = 20.0f;
        // A line that already fits is never grown.
        CHECK_NEAR(ta::fitPx(px, 40.0f, 120.0f), px, 1e-6);
        // One that does not is shrunk in proportion.
        CHECK_NEAR(ta::fitPx(px, 200.0f, 100.0f), 10.0f, 1e-6);
        // ...but never below the floor, however long the line.
        CHECK_NEAR(ta::fitPx(px, 4000.0f, 20.0f), ta::kMinTextPx, 1e-6);
        CHECK(ta::fitPx(px, 4000.0f, 20.0f) >= ta::kMinTextPx);
        // Degenerate inputs return something drawable rather than a NaN fed
        // to a glyph.
        CHECK(ta::fitPx(px, 0.0f, 100.0f) > 0.0f);
        CHECK(ta::fitPx(px, 100.0f, 0.0f) > 0.0f);
        CHECK(ta::fitPx(0.0f, 100.0f, 100.0f) > 0.0f);
        // The result is always drawable and never larger than asked for.
        for (float wAt = 1.0f; wAt < 500.0f; wAt += 7.0f) {
            for (float maxW = 1.0f; maxW < 300.0f; maxW += 11.0f) {
                const float got = ta::fitPx(px, wAt, maxW);
                CHECK(got >= ta::kMinTextPx && got <= px);
            }
        }
    }

    // --- the deck divides, at every size ------------------------------------
    //
    // Rule 3: the drawing is scaled to the rectangle, never drawn over its
    // edge. The three bays must add up to EXACTLY the width given, at every
    // width, or the face leaves a seam or spills.
    {
        bool sums = true;
        bool positive = true;
        for (float w = ta::kMinDeckW; w < 4000.0f; w += 13.0f) {
            const ta::Columns c = ta::columnsFor(w, 200.0f);
            CHECK(c.valid);
            const float total = c.lampW + c.barsW + c.glassW + 2.0f * c.gap;
            if (std::fabs(total - w) > 1e-3f) { sums = false; }
            if (!(c.lampW > 0.0f && c.barsW > 0.0f && c.glassW > 0.0f)) {
                positive = false;
            }
        }
        CHECK(sums);
        CHECK(positive);
    }

    // --- a rectangle of no size at all --------------------------------------
    //
    // The window is user-resizable and the host hands the face whatever is
    // left. None of this may divide by zero, produce a NaN, or claim a bay it
    // has no room for.
    {
        const ta::Columns z = ta::columnsFor(0.0f, 0.0f);
        CHECK(!z.valid);
        CHECK(z.lampW == 0.0f && z.barsW == 0.0f && z.glassW == 0.0f);
        CHECK(!ta::columnsFor(-500.0f, -500.0f).valid);
        CHECK(!ta::columnsFor(ta::kMinDeckW - 1.0f, 500.0f).valid);
        CHECK(!ta::columnsFor(500.0f, ta::kMinDeckH - 1.0f).valid);
        CHECK(ta::columnsFor(ta::kMinDeckW, ta::kMinDeckH).valid);
        // And the middle bay's own split answers the same way.
        CHECK(!ta::barsFor(0.0f).valid);
        CHECK(!ta::barsFor(-10.0f).valid);
    }

    // The middle bay: two bars and the scale between them add up to the bay,
    // and the scale never grows past the width its engraved numbers need.
    {
        bool sums = true;
        bool capped = true;
        for (float w = 96.0f; w < 1200.0f; w += 7.0f) {
            const ta::BarColumns b = ta::barsFor(w);
            CHECK(b.valid);
            if (std::fabs(b.barW * 2.0f + b.scaleW + 2.0f * b.gap - w) > 1e-3f) {
                sums = false;
            }
            if (b.scaleW > 62.0f + 1e-3f) { capped = false; }
            CHECK(b.barW > 0.0f);
        }
        CHECK(sums);
        CHECK(capped);
    }

    // --- the type scale ------------------------------------------------------
    {
        // The window the host opens an instrument at is 600 x 460, whose face
        // bay is about 580 x 270 - the size this face was drawn against.
        CHECK_NEAR(ta::typeScale(580.0f, 270.0f), 1.0f, 1e-6);
        // Bounded at both ends, so a huge window does not letter itself like a
        // poster and a small one does not vanish.
        CHECK(ta::typeScale(4000.0f, 3000.0f) <= 1.30f);
        CHECK(ta::typeScale(40.0f, 30.0f) >= 0.72f);
        CHECK(ta::typeScale(0.0f, 0.0f) > 0.0f);
        CHECK(ta::typeScale(-100.0f, -100.0f) > 0.0f);
        // Never larger than the smaller dimension allows: a wide, short face
        // must take the short one's scale or its bays overflow vertically.
        CHECK(ta::typeScale(4000.0f, 270.0f) <= 1.0f + 1e-6f);
        for (float w = 60.0f; w < 3000.0f; w += 37.0f) {
            for (float h = 40.0f; h < 1200.0f; h += 41.0f) {
                const float s = ta::typeScale(w, h);
                CHECK(s >= 0.72f && s <= 1.30f);
            }
        }
    }

    // --- the alert blinks, it is not merely on -------------------------------
    //
    // A steady lamp says "a fault"; a blinking one says "you are being
    // called". The face must therefore be dark for part of every cycle.
    {
        int on = 0;
        int off = 0;
        for (int i = 0; i < 200; ++i) {
            const double t = static_cast<double>(i) * 0.01;
            if (ta::alertBlink(t)) {
                ++on;
            } else {
                ++off;
            }
        }
        CHECK(on > 0);
        CHECK(off > 0);
        // Two full cycles a second: the same 2 Hz the generic face blinks at.
        CHECK(ta::alertBlink(0.0));
        CHECK(!ta::alertBlink(0.35));
        CHECK(ta::alertBlink(0.5));
        CHECK(!ta::alertBlink(0.85));
        // A clock that has gone wrong leaves the lamp ON rather than off: a
        // missed alert is the expensive failure here, a stuck lamp is not.
        CHECK(ta::alertBlink(std::numeric_limits<double>::quiet_NaN()));
    }

    return testSummary("test_instrument_tone_alert");
}
