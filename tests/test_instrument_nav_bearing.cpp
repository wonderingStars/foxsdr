// The VOR course indicator's arithmetic: the course solution, the compass
// card's lettering, the ident's Morse and the face's layout.
//
// WHY THESE AND NOT THE DRAWING. Everything here is a claim a user may act on -
// which way the bar leans, whether the flag says TO or FROM, what "094" means -
// and a claim has to be tested. The sign in particular cannot be seen to be
// wrong: feed a course indicator the wrong sense and it produces a confident,
// plausible, MIRRORED answer, which is the same failure the VOR plugin's own
// vor_signal.h opens by guarding against, one layer up.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <cstring>
#include <limits>
#include <string>

#include "core/plugin_abi.h"
#include "gui/instrument_nav_bearing_math.hpp"
#include "test_check.hpp"

namespace nb = cascade::gui::navbearing;

namespace {

std::string morse(const char* ident, std::size_t cap = 96) {
    char buf[128];
    if (cap > sizeof buf) { cap = sizeof buf; }
    std::memset(buf, '#', sizeof buf);
    nb::identMorse(ident, buf, cap);
    return std::string(buf);
}

std::string label(int deg) {
    char b[4];
    if (!nb::cardLabel(deg, b, sizeof b)) { return "<none>"; }
    return std::string(b);
}

std::string bearing(double deg) {
    char b[4];
    nb::formatBearing(deg, b, sizeof b);
    return std::string(b);
}

}  // namespace

int main() {
    // --- angles ------------------------------------------------------------
    {
        CHECK_NEAR(nb::wrap360(0.0), 0.0, 1e-9);
        CHECK_NEAR(nb::wrap360(360.0), 0.0, 1e-9);
        CHECK_NEAR(nb::wrap360(361.5), 1.5, 1e-9);
        CHECK_NEAR(nb::wrap360(-1.0), 359.0, 1e-9);
        CHECK_NEAR(nb::wrap360(-721.0), 359.0, 1e-9);
        CHECK_NEAR(nb::wrap360(1e6 + 123.0), std::fmod(1e6 + 123.0, 360.0), 1e-6);
        // A slot the plugin filled with rubbish must not become a NaN card
        // rotation, which draws nothing at all and looks like a dead face.
        CHECK_NEAR(nb::wrap360(std::nan("")), 0.0, 1e-9);
        CHECK_NEAR(nb::wrap360(std::numeric_limits<double>::infinity()), 0.0, 1e-9);

        CHECK_NEAR(nb::wrap180(0.0), 0.0, 1e-9);
        CHECK_NEAR(nb::wrap180(180.0), 180.0, 1e-9);
        CHECK_NEAR(nb::wrap180(181.0), -179.0, 1e-9);
        CHECK_NEAR(nb::wrap180(-181.0), 179.0, 1e-9);
        CHECK_NEAR(nb::wrap180(359.0), -1.0, 1e-9);

        CHECK(nb::reciprocal(0) == 180);
        CHECK(nb::reciprocal(180) == 0);
        CHECK(nb::reciprocal(94) == 274);
        CHECK(nb::reciprocal(274) == 94);
        CHECK(nb::reciprocal(359) == 179);
        CHECK(nb::reciprocal(360) == 180);

        // 359.7 is a bearing of 000, not of 360 - a card has no 360 on it.
        CHECK(nb::roundedDeg(359.7) == 0);
        CHECK(nb::roundedDeg(0.4) == 0);
        CHECK(nb::roundedDeg(93.6) == 94);
        CHECK(nb::roundedDeg(-0.4) == 0);
    }

    // --- the course solution, and THE SIGN ---------------------------------
    //
    // The case that matters most: sitting on the 094 radial with 094 selected
    // is centred and FROM, and with 274 selected is centred and TO. Everything
    // else is derived from those two by moving off course a known amount.
    {
        nb::CourseSolution s = nb::solveCourse(94.0, 94.0);
        CHECK(!s.to);
        CHECK_NEAR(s.deflectionDeg, 0.0, 1e-9);
        CHECK_NEAR(s.barFrac, 0.0, 1e-9);
        CHECK(!s.offScale);

        s = nb::solveCourse(94.0, 274.0);
        CHECK(s.to);
        CHECK_NEAR(s.deflectionDeg, 0.0, 1e-9);
        CHECK_NEAR(s.barFrac, 0.0, 1e-9);
    }
    {
        // FROM, four degrees clockwise of the selected course: the receiver is
        // RIGHT of the course, so the bar - which shows where the course is -
        // goes LEFT. Two dots of five.
        nb::CourseSolution s = nb::solveCourse(98.0, 94.0);
        CHECK(!s.to);
        CHECK_NEAR(s.deflectionDeg, -4.0, 1e-9);
        CHECK_NEAR(s.barFrac, -0.4, 1e-12);
        CHECK(!s.offScale);

        // ...and four the other way puts it right.
        s = nb::solveCourse(90.0, 94.0);
        CHECK(!s.to);
        CHECK_NEAR(s.deflectionDeg, 4.0, 1e-9);
        CHECK_NEAR(s.barFrac, 0.4, 1e-12);
    }
    {
        // TO, and the sense REVERSES - which is the whole reason a TO/FROM
        // indication exists and the single most-confused thing about a VOR.
        // Same position, same four degrees, opposite bar.
        nb::CourseSolution s = nb::solveCourse(98.0, 274.0);
        CHECK(s.to);
        CHECK_NEAR(s.deflectionDeg, 4.0, 1e-9);
        CHECK_NEAR(s.barFrac, 0.4, 1e-12);

        s = nb::solveCourse(90.0, 274.0);
        CHECK(s.to);
        CHECK_NEAR(s.deflectionDeg, -4.0, 1e-9);
    }
    {
        // The 90 degree boundary is where the flag changes over, and it is
        // exactly on the FROM side by the rule this face uses. One degree
        // either way must not straddle.
        CHECK(!nb::solveCourse(90.0, 0.0).to);
        CHECK(nb::solveCourse(91.0, 0.0).to);
        CHECK(!nb::solveCourse(89.0, 0.0).to);
        CHECK(!nb::solveCourse(270.0, 0.0).to);
        CHECK(nb::solveCourse(269.0, 0.0).to);
    }
    {
        // ACROSS THE NORTH SEAM, which is where a difference of two bearings
        // taken without wrapping goes wrong by 360 and puts the bar hard over
        // on a course that is two degrees away.
        nb::CourseSolution s = nb::solveCourse(359.0, 1.0);
        CHECK(!s.to);
        CHECK_NEAR(s.deflectionDeg, 2.0, 1e-9);
        CHECK_NEAR(s.barFrac, 0.2, 1e-12);

        s = nb::solveCourse(1.0, 359.0);
        CHECK(!s.to);
        CHECK_NEAR(s.deflectionDeg, -2.0, 1e-9);

        s = nb::solveCourse(359.0, 181.0);
        CHECK(s.to);
        CHECK_NEAR(s.deflectionDeg, -2.0, 1e-9);
    }
    {
        // FULL SCALE IS TEN DEGREES and the bar STOPS there - it does not keep
        // travelling off the dial and it does not wrap round to the other
        // stop, which is what an unclamped division would do at 190 degrees.
        nb::CourseSolution s = nb::solveCourse(84.0, 94.0);
        CHECK_NEAR(s.barFrac, 1.0, 1e-12);
        CHECK(!s.offScale);  // exactly full scale is on the scale
        s = nb::solveCourse(74.0, 94.0);
        CHECK_NEAR(s.barFrac, 1.0, 1e-12);
        CHECK(s.offScale);
        s = nb::solveCourse(154.0, 94.0);  // sixty degrees off, still FROM
        CHECK(!s.to);
        CHECK_NEAR(s.barFrac, -1.0, 1e-12);
        CHECK(s.offScale);
        // The largest a bearing slot can hold and still mean a bearing.
        s = nb::solveCourse(360.0, 0.0);
        CHECK(!s.to);
        CHECK_NEAR(s.barFrac, 0.0, 1e-12);
        CHECK(std::fabs(nb::solveCourse(1e9, 0.0).barFrac) <= 1.0);
    }

    // --- the compass card's lettering --------------------------------------
    {
        CHECK(label(0) == "N");
        CHECK(label(90) == "E");
        CHECK(label(180) == "S");
        CHECK(label(270) == "W");
        CHECK(label(30) == "3");
        CHECK(label(60) == "6");
        CHECK(label(120) == "12");
        CHECK(label(150) == "15");
        CHECK(label(210) == "21");
        CHECK(label(240) == "24");
        CHECK(label(300) == "30");
        CHECK(label(330) == "33");
        CHECK(label(360) == "N");
        CHECK(label(-30) == "33");
        // A card is lettered only every thirty degrees. Inventing "4" for 45
        // would put a figure on the dial no card carries.
        CHECK(label(45) == "<none>");
        CHECK(label(5) == "<none>");
        // A caller with no room for the answer gets nothing rather than a
        // buffer overrun.
        {
            char tiny[2] = {'#', '#'};
            CHECK(!nb::cardLabel(120, tiny, sizeof tiny));
            CHECK(tiny[0] == '#');
            CHECK(!nb::cardLabel(120, nullptr, 8));
        }
    }

    // --- where a card mark lands -------------------------------------------
    {
        // The radial sits under the top index by construction.
        CHECK_NEAR(nb::cardScreenDeg(94.0, 94.0), 0.0, 1e-9);
        // North is then 94 degrees anticlockwise of the top, i.e. at 266.
        CHECK_NEAR(nb::cardScreenDeg(0.0, 94.0), 266.0, 1e-9);
        CHECK_NEAR(nb::cardScreenDeg(184.0, 94.0), 90.0, 1e-9);
        CHECK_NEAR(nb::cardScreenDeg(30.0, 350.0), 40.0, 1e-9);
    }

    // --- the readout --------------------------------------------------------
    {
        CHECK(bearing(94.0) == "094");
        CHECK(bearing(0.0) == "000");
        CHECK(bearing(359.6) == "000");
        CHECK(bearing(273.4) == "273");
        CHECK(bearing(-1.0) == "359");
    }

    // --- the ident's Morse --------------------------------------------------
    {
        CHECK(std::strcmp(nb::morseFor('L'), ".-..") == 0);
        CHECK(std::strcmp(nb::morseFor('l'), ".-..") == 0);
        CHECK(std::strcmp(nb::morseFor('T'), "-") == 0);
        CHECK(std::strcmp(nb::morseFor('0'), "-----") == 0);
        CHECK(nb::morseFor('#') == nullptr);
        CHECK(nb::morseFor(' ') == nullptr);

        CHECK(morse("LBA") == ".-.. -... .-");
        CHECK(morse("POL") == ".--. --- .-..");
        CHECK(morse("TM") == "- --");
        CHECK(morse("") == "");
        CHECK(morse(nullptr) == "");
        // A character with no Morse is dropped, not drawn as a silent gap that
        // would read as a letter.
        CHECK(morse("L*A") == ".-.. .-");

        // AN OVERLONG IDENT IS CUT ON A LETTER BOUNDARY. Half of ".-.." is
        // "..", which is a different letter - so a truncation mid-pattern
        // would not be a shortened ident, it would be a wrong one.
        {
            char buf[8];
            nb::identMorse("LBA", buf, sizeof buf);
            CHECK(std::strcmp(buf, ".-..") == 0);
            nb::identMorse("LBA", buf, 5);
            CHECK(std::strcmp(buf, ".-..") == 0);
            nb::identMorse("LBA", buf, 4);
            CHECK(std::strcmp(buf, "") == 0);
            nb::identMorse("LBA", buf, 1);
            CHECK(std::strcmp(buf, "") == 0);
        }
        // The longest thing the slot can hold - 63 letters - must not run past
        // its buffer or leave it unterminated.
        {
            char ident[CASCADE_INSTRUMENT_TEXT_CHARS];
            for (int i = 0; i < CASCADE_INSTRUMENT_TEXT_CHARS - 1; ++i) {
                ident[i] = '0';  // "-----", the longest pattern there is
            }
            ident[CASCADE_INSTRUMENT_TEXT_CHARS - 1] = '\0';
            char buf[64];
            std::memset(buf, '#', sizeof buf);
            nb::identMorse(ident, buf, sizeof buf);
            CHECK(std::strlen(buf) < sizeof buf);
            // Whole letters only: every group is five dashes.
            std::size_t groups = 1;
            for (const char* p = buf; *p != '\0'; ++p) {
                if (*p == ' ') { ++groups; }
            }
            CHECK(std::strlen(buf) == groups * 5u + (groups - 1u));
        }
        // Zero capacity and a null destination are absorbed.
        nb::identMorse("LBA", nullptr, 8);
        {
            char buf[4] = {'#', '#', '#', '#'};
            nb::identMorse("LBA", buf, 0);
            CHECK(buf[0] == '#');
        }
    }

    // --- the layout ---------------------------------------------------------
    {
        // A ZERO-SIZE RECTANGLE MUST NOT CRASH and must not hand back a dial
        // radius something downstream divides by. The window is user-resizable
        // and dragging it to nothing is one drag away.
        nb::Layout l = nb::layout(0.0f, 0.0f);
        CHECK(!l.drawAnything);
        CHECK(l.dialR == 0.0f);
        l = nb::layout(-100.0f, -100.0f);
        CHECK(!l.drawAnything);
        l = nb::layout(std::numeric_limits<float>::quiet_NaN(), 400.0f);
        CHECK(!l.drawAnything);

        // Too small for a dial at all: the caller falls back to words.
        CHECK(!nb::layout(120.0f, 40.0f).drawAnything);
        CHECK(!nb::layout(40.0f, 400.0f).drawAnything);
    }
    {
        // The default window, 600 x 460, less the page's own chrome and the
        // plate: everything is drawn and the dial is a decent size.
        const nb::Layout l = nb::layout(584.0f, 360.0f);
        CHECK(l.drawAnything);
        CHECK(l.drawGauges);
        CHECK(l.drawReadout);
        CHECK(l.drawIdent);
        CHECK(l.dialR >= nb::kMinDialR);
        // The dial keeps clear of the gauge column it stands beside.
        CHECK(l.dialCx + l.dialR <= l.gaugeX0 + 0.5f);
        CHECK(l.gaugeX1 <= 584.0f);
        // The readout opens below the dial, not through it.
        CHECK(l.readoutY0 >= l.dialCy + l.dialR - 0.5f);
        CHECK(l.readoutY1 + nb::kIdentH <= 360.0f + 0.5f);
    }
    {
        // NARROW: the gauge column is the first thing to go, because a dial
        // too small to read is worse than no gauges.
        const nb::Layout l = nb::layout(200.0f, 360.0f);
        CHECK(l.drawAnything);
        CHECK(!l.drawGauges);
        CHECK(l.dialR >= nb::kMinDialR);
    }
    {
        // SHORT: the readout strip goes next, and the ident before it.
        const nb::Layout l = nb::layout(600.0f, 120.0f);
        CHECK(l.drawAnything);
        CHECK(!l.drawIdent);
        CHECK(l.dialR >= nb::kMinDialR);
    }
    {
        // LARGE: nothing runs off the edge and the dial grows with the window.
        const nb::Layout big = nb::layout(1600.0f, 1000.0f);
        const nb::Layout small = nb::layout(584.0f, 360.0f);
        CHECK(big.drawAnything);
        CHECK(big.dialR > small.dialR);
        CHECK(big.dialCx + big.dialR <= big.gaugeX0 + 0.5f);
        CHECK(big.gaugeX1 <= 1600.0f);
        CHECK(big.readoutY1 + nb::kIdentH <= 1000.0f + 0.5f);
    }
    {
        // Every size from tiny to large, in one pixel steps on each axis
        // through the interesting band: whatever is drawn must stay inside the
        // rectangle it was given. This is the property the three rules call
        // "fit the rectangle you are given", swept rather than sampled.
        int checked = 0;
        for (float w = 60.0f; w <= 900.0f; w += 7.0f) {
            for (float hh = 40.0f; hh <= 700.0f; hh += 7.0f) {
                const nb::Layout l = nb::layout(w, hh);
                if (!l.drawAnything) { continue; }
                ++checked;
                if (l.dialCx - l.dialR < -0.5f || l.dialCy - l.dialR < -0.5f ||
                    l.dialCy + l.dialR > hh + 0.5f || l.dialR < nb::kMinDialR) {
                    CHECK(false);
                    std::printf("      dial out of bounds at %.0f x %.0f: c=(%.1f,%.1f) r=%.1f\n",
                                w, hh, l.dialCx, l.dialCy, l.dialR);
                    hh = 1e9f;
                    w = 1e9f;
                    break;
                }
                if (l.drawGauges && (l.gaugeX1 > w + 0.5f || l.gaugeX0 < l.dialCx + l.dialR - 0.5f)) {
                    CHECK(false);
                    std::printf("      gauges out of bounds at %.0f x %.0f: %.1f..%.1f\n", w,
                                hh, l.gaugeX0, l.gaugeX1);
                    hh = 1e9f;
                    w = 1e9f;
                    break;
                }
                if (l.drawReadout && l.readoutY1 > hh + 0.5f) {
                    CHECK(false);
                    std::printf("      readout out of bounds at %.0f x %.0f: %.1f\n", w, hh,
                                l.readoutY1);
                    hh = 1e9f;
                    w = 1e9f;
                    break;
                }
                if (l.drawIdent && l.readoutY1 + nb::kIdentH > hh + 0.5f) {
                    CHECK(false);
                    std::printf("      ident out of bounds at %.0f x %.0f\n", w, hh);
                    hh = 1e9f;
                    w = 1e9f;
                    break;
                }
            }
        }
        CHECK(checked > 500);
    }

    return testSummary("test_instrument_nav_bearing");
}
