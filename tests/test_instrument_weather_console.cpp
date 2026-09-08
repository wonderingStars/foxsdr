// The weather console's arithmetic: the seven-segment table, the cell plans
// for a temperature and a humidity, the channel masks, the panel layout and
// the rail's chip word.
//
// WHAT THESE CHECKS ARE FOR. Every one of them is a place where "we have not
// heard from that sensor" could quietly become "that sensor says zero". The
// slot map hands the face three temperatures and a mask, and 0.0 C is a
// perfectly good reading, so the mask is the ONLY thing that separates the
// two claims - which makes the mask worth testing harder than the digits.
//
// The drawing is not tested here and cannot be: it needs an ImDrawList and a
// font atlas. What is tested is every decision the drawing makes before it
// puts a segment down, which is where the faults would be.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <cmath>
#include <cstring>
#include <limits>
#include <string>

// The math header is deliberately free of the plugin ABI as well as of ImGui;
// the ABI is included HERE because the overlong-text case has to be exercised
// against the real slot size, not against a number retyped in a test.
#include "core/plugin_abi.h"
#include "gui/instrument_weather_console_math.hpp"
#include "test_check.hpp"

using namespace cascade::gui::wxface;

namespace {

std::string cellsOf(const TempCells& t) {
    std::string s;
    s += t.negative ? '-' : ' ';
    s += t.hundreds;
    s += t.tens;
    s += t.units;
    s += '.';
    s += t.tenths;
    return s;
}

std::string humOf(const HumCells& h) {
    std::string s;
    s += h.hundreds;
    s += h.tens;
    s += h.units;
    return s;
}

}  // namespace

int main() {
    // --- the segment table ---------------------------------------------------
    //
    // Pinned digit by digit against the shapes a seven-segment cell makes,
    // because a transposed pair here is a 6 that looks like a 5 on every face
    // and nothing else would catch it.
    CHECK(segmentsFor('0') == (kSegA | kSegB | kSegC | kSegD | kSegE | kSegF));
    CHECK(segmentsFor('1') == (kSegB | kSegC));
    CHECK(segmentsFor('2') == (kSegA | kSegB | kSegG | kSegE | kSegD));
    CHECK(segmentsFor('3') == (kSegA | kSegB | kSegG | kSegC | kSegD));
    CHECK(segmentsFor('4') == (kSegF | kSegG | kSegB | kSegC));
    CHECK(segmentsFor('5') == (kSegA | kSegF | kSegG | kSegC | kSegD));
    CHECK(segmentsFor('6') == (kSegA | kSegF | kSegG | kSegE | kSegC | kSegD));
    CHECK(segmentsFor('7') == (kSegA | kSegB | kSegC));
    CHECK(segmentsFor('8') == kSegAll);
    CHECK(segmentsFor('9') == (kSegA | kSegB | kSegC | kSegD | kSegF | kSegG));

    // Every digit is distinct. A table where two digits light the same
    // segments would draw a display that cannot be read, and the check above
    // would not notice if both were wrong the same way.
    for (char a = '0'; a <= '9'; ++a) {
        for (char b = static_cast<char>(a + 1); b <= '9'; ++b) {
            CHECK(segmentsFor(a) != segmentsFor(b));
        }
    }
    // 8 lights everything and is therefore the union of every other digit -
    // an independent statement of the same table.
    unsigned all = 0u;
    for (char c = '0'; c <= '9'; ++c) { all |= segmentsFor(c); }
    CHECK(all == kSegAll);
    CHECK(all == segmentsFor('8'));

    // The three non-digits the equipment forms, and the rule that anything
    // else lights nothing rather than something arbitrary.
    CHECK(segmentsFor('-') == kSegG);
    CHECK(segmentsFor(' ') == 0u);
    CHECK(segmentsFor('C') == (kSegA | kSegF | kSegE | kSegD));
    CHECK(segmentsFor('H') == (kSegB | kSegC | kSegE | kSegF | kSegG));
    CHECK(segmentsFor('L') == (kSegD | kSegE | kSegF));
    CHECK(segmentsFor('Q') == 0u);
    CHECK(segmentsFor('\0') == 0u);
    CHECK(segmentsFor('\xFF') == 0u);
    // A dash and an 8 must not be confusable: the whole "no reading" rule
    // rests on the dash being nothing but the middle bar.
    CHECK((segmentsFor('-') & ~kSegG) == 0u);

    // --- the channel mask ----------------------------------------------------
    CHECK(channelReporting(0.0, 1) == false);
    CHECK(channelReporting(0.0, 2) == false);
    CHECK(channelReporting(0.0, 3) == false);
    CHECK(channelReporting(1.0, 1) == true);
    CHECK(channelReporting(1.0, 2) == false);
    CHECK(channelReporting(2.0, 2) == true);
    CHECK(channelReporting(3.0, 1) == true);
    CHECK(channelReporting(3.0, 2) == true);
    CHECK(channelReporting(3.0, 3) == false);
    CHECK(channelReporting(7.0, 3) == true);
    // Out of range channels are refused rather than indexing anything.
    CHECK(channelReporting(7.0, 0) == false);
    CHECK(channelReporting(7.0, 4) == false);
    CHECK(channelReporting(7.0, -1) == false);
    // A slot holding something that is not a small integer must not become a
    // shift by a wild amount. std::lround of these is undefined behaviour,
    // which is exactly why nothing outside this header calls it on a slot.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    CHECK(channelReporting(nan, 1) == false);
    CHECK(channelReporting(inf, 1) == false);
    CHECK(channelReporting(-inf, 1) == false);
    CHECK(channelReporting(-5.0, 1) == false);
    CHECK(channelReporting(1e300, 1) == false);
    CHECK(reportingCount(nan) == 0);
    CHECK(reportingCount(0.0) == 0);
    CHECK(reportingCount(1.0) == 1);
    CHECK(reportingCount(5.0) == 2);
    CHECK(reportingCount(7.0) == 3);

    // A low-battery bit on a channel with NO reading lights nothing: the
    // console cannot be reporting a sensor's cells when it is not reporting
    // the sensor.
    CHECK(channelLowBattery(0.0, 7.0, 1) == false);
    CHECK(channelLowBattery(1.0, 7.0, 1) == true);
    CHECK(channelLowBattery(1.0, 7.0, 2) == false);
    CHECK(channelLowBattery(3.0, 2.0, 1) == false);
    CHECK(channelLowBattery(3.0, 2.0, 2) == true);
    CHECK(channelLowBattery(7.0, nan, 3) == false);

    // --- the temperature cells ----------------------------------------------
    //
    // NO READING IS DASHES, and this is the check the whole face exists to
    // pass. A cleared channel bit must produce a blank plan whatever is
    // sitting in the temperature slot - including a perfectly plausible one.
    {
        const TempCells t = temperatureCells(false, 21.4);
        CHECK(t.blank == true);
        CHECK(t.over == false);
        CHECK(t.tens == '-');
        CHECK(t.units == '-');
        CHECK(t.tenths == '-');
        CHECK(t.hundreds == ' ');
        CHECK(t.negative == false);
        CHECK(cellsOf(t) == std::string("  --.-"));
    }
    // And zero WITH a reading is a figure, not a blank. The two cases either
    // side of this line are the ones this face is about.
    {
        const TempCells t = temperatureCells(true, 0.0);
        CHECK(t.blank == false);
        CHECK(cellsOf(t) == std::string("  " " 0.0"));
        CHECK(t.tens == ' ');
        CHECK(t.units == '0');
        CHECK(t.tenths == '0');
        CHECK(t.negative == false);
    }
    CHECK(cellsOf(temperatureCells(true, 21.4)) == std::string("  21.4"));
    CHECK(cellsOf(temperatureCells(true, -2.6)) == std::string("-  2.6"));
    CHECK(cellsOf(temperatureCells(true, 8.4)) == std::string("   8.4"));
    CHECK(cellsOf(temperatureCells(true, -8.4)) == std::string("-  8.4"));
    CHECK(cellsOf(temperatureCells(true, 99.9)) == std::string("  99.9"));
    CHECK(cellsOf(temperatureCells(true, 100.0)) == std::string(" 100.0"));
    CHECK(cellsOf(temperatureCells(true, 199.9)) == std::string(" 199.9"));
    // Rounding happens before the split, so a value a hair under a tenth
    // boundary does not come apart into two disagreeing halves.
    CHECK(cellsOf(temperatureCells(true, 21.44)) == std::string("  21.4"));
    CHECK(cellsOf(temperatureCells(true, 21.46)) == std::string("  21.5"));
    CHECK(cellsOf(temperatureCells(true, 9.96)) == std::string("  10.0"));
    CHECK(cellsOf(temperatureCells(true, -9.96)) == std::string("- 10.0"));
    CHECK(cellsOf(temperatureCells(true, 99.96)) == std::string(" 100.0"));

    // The largest the slot can hold, and everything past the field. The
    // equipment's own indication, and NOT the dashes that mean no reading.
    {
        const TempCells t = temperatureCells(true, 200.0);
        CHECK(t.blank == false);
        CHECK(t.over == true);
        CHECK(t.overHigh == true);
        CHECK(t.tens == 'H' && t.units == 'H' && t.tenths == 'H');
    }
    {
        const TempCells t = temperatureCells(true, -200.0);
        CHECK(t.over == true);
        CHECK(t.overHigh == false);
        CHECK(t.negative == true);
        CHECK(t.tens == 'L' && t.units == 'L' && t.tenths == 'L');
    }
    {
        // The largest double there is, and infinity: both are refused before
        // any cast to a long integer, which is where the undefined behaviour
        // would be.
        const TempCells t = temperatureCells(true, std::numeric_limits<double>::max());
        CHECK(t.over == true && t.blank == false && t.overHigh == true);
        const TempCells i = temperatureCells(true, inf);
        CHECK(i.over == true && i.overHigh == true);
        const TempCells ni = temperatureCells(true, -inf);
        CHECK(ni.over == true && ni.overHigh == false);
        // A NaN is not a reading and must not be drawn as one.
        const TempCells n = temperatureCells(true, nan);
        CHECK(n.blank == true);
        CHECK(n.over == false);
        CHECK(cellsOf(n) == std::string("  --.-"));
    }

    // --- the humidity cells --------------------------------------------------
    //
    // The channel gates it, and so does the slot-map convention that a zero
    // means "this sensor does not measure humidity" - a THN132N is a
    // thermometer, and 0% RH is not a thing a hygrometer reports.
    CHECK(humidityCells(false, 48.0).blank == true);
    CHECK(humidityCells(true, 0.0).blank == true);
    CHECK(humOf(humidityCells(true, 0.0)) == std::string(" --"));
    CHECK(humidityCells(true, nan).blank == true);
    CHECK(humidityCells(true, -3.0).blank == true);
    CHECK(humidityCells(true, 250.0).blank == true);
    CHECK(humOf(humidityCells(true, 48.0)) == std::string(" 48"));
    CHECK(humidityCells(true, 48.0).blank == false);
    CHECK(humOf(humidityCells(true, 5.0)) == std::string(" 05"));
    CHECK(humOf(humidityCells(true, 100.0)) == std::string("100"));
    CHECK(humOf(humidityCells(true, 99.6)) == std::string("100"));
    CHECK(humidityCells(true, 100.0).hundreds == '1');
    CHECK(humidityCells(true, 99.0).hundreds == ' ');

    // --- the printed label ---------------------------------------------------
    {
        char out[CASCADE_INSTRUMENT_TEXT_CHARS];
        fitLabel("THN132N", out, sizeof out, 20);
        CHECK(std::string(out) == "THN132N");
        fitLabel("THGR122NX/THGN123N", out, sizeof out, 10);
        CHECK(std::string(out) == "THGR122...");
        CHECK(std::strlen(out) == 10u);
        // An overlong text - the slot holds 63 characters and a compartment
        // holds a handful - must be cut, not drawn into its neighbour and not
        // written past the end of anything.
        char big[CASCADE_INSTRUMENT_TEXT_CHARS];
        std::memset(big, 'X', sizeof big);
        big[CASCADE_INSTRUMENT_TEXT_CHARS - 1] = '\0';
        char small[8];
        std::memset(small, 0x5A, sizeof small);
        fitLabel(big, small, sizeof small, 40);
        CHECK(std::strlen(small) == 7u);
        CHECK(std::string(small) == "XXXX...");
        // Degenerate widths must produce an empty string, never a read or a
        // write outside the buffer.
        fitLabel("THGR122NX", out, sizeof out, 0);
        CHECK(out[0] == '\0');
        fitLabel("THGR122NX", out, sizeof out, 2);
        CHECK(std::string(out) == "TH");
        fitLabel("THGR122NX", out, sizeof out, 3);
        CHECK(std::string(out) == "THG");
        fitLabel("THGR122NX", out, sizeof out, 4);
        CHECK(std::string(out) == "T...");
        fitLabel(nullptr, out, sizeof out, 8);
        CHECK(out[0] == '\0');
        fitLabel("anything", out, 1u, 8);
        CHECK(out[0] == '\0');
        fitLabel("anything", nullptr, 0u, 8);  // must not crash
    }

    // --- the layout ----------------------------------------------------------
    {
        // Three compartments across 320 points with 8-point divisions.
        const PanelBox a = panelBox(0.0f, 320.0f, 0, 3, 8.0f);
        const PanelBox b = panelBox(0.0f, 320.0f, 1, 3, 8.0f);
        const PanelBox c = panelBox(0.0f, 320.0f, 2, 3, 8.0f);
        CHECK(a.valid && b.valid && c.valid);
        CHECK_NEAR(a.x0, 0.0, 0.01);
        CHECK_NEAR(c.x1, 320.0, 0.01);
        CHECK_NEAR(a.x1 - a.x0, b.x1 - b.x0, 0.01);
        CHECK_NEAR(b.x1 - b.x0, c.x1 - c.x0, 0.01);
        CHECK_NEAR(b.x0 - a.x1, 8.0, 0.01);
        CHECK_NEAR(c.x0 - b.x1, 8.0, 0.01);
        // Nothing overlaps, which is what stops one compartment drawing over
        // the one beside it.
        CHECK(a.x1 <= b.x0);
        CHECK(b.x1 <= c.x0);
    }
    {
        // A ZERO-SIZE RECTANGLE. A window dragged shut must produce no boxes
        // at all rather than boxes that are inside out.
        CHECK(panelBox(0.0f, 0.0f, 0, 3, 8.0f).valid == false);
        CHECK(panelBox(100.0f, 100.0f, 1, 3, 8.0f).valid == false);
        CHECK(panelBox(100.0f, 90.0f, 0, 3, 8.0f).valid == false);
        CHECK(panelBox(0.0f, 20.0f, 0, 3, 8.0f).valid == false);
        // And out-of-range indices and counts.
        CHECK(panelBox(0.0f, 320.0f, 3, 3, 8.0f).valid == false);
        CHECK(panelBox(0.0f, 320.0f, -1, 3, 8.0f).valid == false);
        CHECK(panelBox(0.0f, 320.0f, 0, 0, 8.0f).valid == false);
    }
    {
        // The digit height must never produce a group wider than the glass it
        // was fitted to - that is the whole of rule three in one function.
        for (float w = 10.0f; w <= 400.0f; w += 7.0f) {
            const float h = fitDigitHeight(w, 62.0f, 18.0f);
            if (h > 0.0f) {
                CHECK(temperatureGroupWidth(h) <= w + 0.01f);
                CHECK(h >= 18.0f);
                CHECK(h <= 62.0f);
            }
        }
        // Too narrow for even the floor height: no digits at all, rather than
        // digits over the edge.
        CHECK(fitDigitHeight(20.0f, 62.0f, 18.0f) == 0.0f);
        CHECK(fitDigitHeight(0.0f, 62.0f, 18.0f) == 0.0f);
        CHECK(fitDigitHeight(-40.0f, 62.0f, 18.0f) == 0.0f);
        CHECK(fitDigitHeight(400.0f, 0.0f, 18.0f) == 0.0f);
        // Wider glass never gives a SMALLER digit.
        float last = 0.0f;
        for (float w = 60.0f; w <= 400.0f; w += 5.0f) {
            const float h = fitDigitHeight(w, 62.0f, 18.0f);
            CHECK(h >= last - 0.01f);
            last = h;
        }
        CHECK(digitCellWidth(40.0f) > 0.0f);
        CHECK(temperatureGroupWidth(40.0f) > digitCellWidth(40.0f) * 3.0f);
    }

    // --- the compartment's budget, at every size a window can be -------------
    //
    // RULE THREE IN A LOOP. The window is user-resizable, so the face has to
    // fit whatever rectangle it is handed - and a screenshot can only prove the
    // two or three sizes somebody thought to try. This sweeps every
    // compartment from nothing to a wall and asserts three things: the stack
    // fits, the temperature is the last thing given up, and no row is drawn at
    // a size that could not hold it.
    {
        const float tiny = 17.0f;  // fonts::kTinySize at the time of writing
        for (float w = 0.0f; w <= 400.0f; w += 7.0f) {
            for (float h = 0.0f; h <= 400.0f; h += 7.0f) {
                const PanelStyle st = panelStyle(w, h, tiny);
                const float stack = panelStackHeight(st);
                // FITS. Never taller than the compartment it was measured for.
                CHECK(stack <= h + 0.01f);
                // Never wider either: the temperature group is the widest
                // thing in a compartment and it is fitted to the width.
                if (st.digitH > 0.0f) {
                    CHECK(temperatureGroupWidth(st.digitH) <= w - 4.0f + 0.01f);
                }
                // The order things give way in. A humidity row or a printed
                // name without a temperature above it would be a caption on a
                // reading that is not being shown.
                if (st.digitH <= 0.0f) {
                    CHECK(st.humH == 0.0f);
                    CHECK(st.modelPx == 0.0f);
                }
                // Nothing is ever negative, which is what would draw a
                // rectangle inside out.
                CHECK(st.headerPx >= 0.0f);
                CHECK(st.digitH >= 0.0f);
                CHECK(st.humH >= 0.0f);
                CHECK(st.modelPx >= 0.0f);
            }
        }
        // A ZERO-SIZE COMPARTMENT DRAWS NOTHING AT ALL.
        const PanelStyle none = panelStyle(0.0f, 0.0f, tiny);
        CHECK(none.headerPx == 0.0f && none.digitH == 0.0f);
        CHECK(none.humH == 0.0f && none.modelPx == 0.0f);
        CHECK(panelStackHeight(none) == 0.0f);
        CHECK(panelStyle(-50.0f, 200.0f, tiny).digitH == 0.0f);
        CHECK(panelStyle(200.0f, -50.0f, tiny).digitH == 0.0f);

        // The window's OWN DEFAULT SIZE, which is the one a user actually
        // sees: 600 x 460 gives the face about 560 x 216 with the memory table
        // beneath it, and about 100 points of compartment. All four rows have
        // to be there at that size - that is the size the face was designed
        // at, and a regression that dropped the humidity row would show up
        // here rather than in a screenshot nobody took.
        const PanelStyle def = panelStyle(160.0f, 100.0f, tiny);
        CHECK(def.headerPx > 0.0f);
        CHECK(def.digitH >= 18.0f);
        CHECK(def.humH > 0.0f);
        CHECK(def.modelPx > 0.0f);
        CHECK(panelStackHeight(def) <= 100.0f);

        // A LARGE one: the face without a memory table takes the whole window,
        // and a compartment then gets three or four times the height. Nothing
        // may run away with it - the digits stop at their own ceiling rather
        // than growing until they are the window.
        const PanelStyle big = panelStyle(320.0f, 380.0f, tiny);
        CHECK(big.digitH <= 64.0f);
        CHECK(big.humH <= 24.0f);
        CHECK(panelStackHeight(big) <= 380.0f);
        CHECK(big.digitH > def.digitH);

        // A SMALL one: a window dragged down until only the temperature fits,
        // and then until nothing does.
        const PanelStyle small = panelStyle(120.0f, 60.0f, tiny);
        CHECK(small.digitH > 0.0f);
        CHECK(small.humH == 0.0f);
        CHECK(small.modelPx == 0.0f);
        CHECK(panelStackHeight(small) <= 60.0f);
        const PanelStyle tinyPanel = panelStyle(40.0f, 40.0f, tiny);
        CHECK(tinyPanel.digitH == 0.0f);
        CHECK(tinyPanel.humH == 0.0f);
        CHECK(tinyPanel.modelPx == 0.0f);

        // TALLER NEVER LOSES A ROW. Rows only ever appear as the compartment
        // grows; none of them is ever taken away again.
        //
        // NOTE WHAT IS NOT CLAIMED HERE. The DIGITS are not monotonic and must
        // not be asserted to be: at the height where the humidity row first
        // fits, that row takes its fourteen points from the digits and they
        // step down slightly. That is the trade this budget exists to make - a
        // reading gained is worth a point of digit height - and an earlier
        // version of this check asserted the opposite and went red at exactly
        // those thresholds, which is the test being wrong rather than the code.
        bool hadHeader = false;
        bool hadHum = false;
        bool hadModel = false;
        for (float h = 0.0f; h <= 300.0f; h += 2.0f) {
            const PanelStyle st = panelStyle(200.0f, h, tiny);
            if (st.headerPx > 0.0f) { hadHeader = true; }
            if (st.humH > 0.0f) { hadHum = true; }
            if (st.modelPx > 0.0f) { hadModel = true; }
            if (hadHeader) { CHECK(st.headerPx > 0.0f); }
            if (hadHum) { CHECK(st.humH > 0.0f); }
            if (hadModel) { CHECK(st.modelPx > 0.0f); }
            // And the digits never fall below the floor once they exist.
            if (st.digitH > 0.0f) { CHECK(st.digitH >= 18.0f); }
        }
        CHECK(hadHeader && hadHum && hadModel);
    }

    // --- the rail's chip -----------------------------------------------------
    {
        char chip[16];
        const double temps[3] = {21.4, -2.6, 0.0};
        // NOTHING REPORTING SAYS SO. It does not print the 21.4 sitting in
        // slot zero, because with no bit set that figure is not a reading.
        chipWord(0.0, temps, chip, sizeof chip);
        CHECK(std::string(chip) == "NO RX");
        chipWord(nan, temps, chip, sizeof chip);
        CHECK(std::string(chip) == "NO RX");
        chipWord(1.0, temps, chip, sizeof chip);
        CHECK(std::string(chip) == "21.4C");
        chipWord(3.0, temps, chip, sizeof chip);
        CHECK(std::string(chip) == "21.4C +1");
        chipWord(2.0, temps, chip, sizeof chip);
        CHECK(std::string(chip) == "-2.6C");
        // Channel three reporting a genuine zero is a reading and prints as
        // one.
        chipWord(4.0, temps, chip, sizeof chip);
        CHECK(std::string(chip) == "0.0C");
        chipWord(7.0, temps, chip, sizeof chip);
        CHECK(std::string(chip) == "21.4C +2");
        // Whatever it prints, it fits the sixteen bytes instrumentChip
        // promises - the chip has no room to be truncated in.
        const double wild[3] = {-199.9, 199.9, 0.0};
        chipWord(7.0, wild, chip, sizeof chip);
        CHECK(std::strlen(chip) < 16u);
        // A figure outside the field is not printed as one.
        const double over[3] = {1.0e9, 0.0, 0.0};
        chipWord(1.0, over, chip, sizeof chip);
        CHECK(std::string(chip) == "1 CH");
        chipWord(7.0, nullptr, chip, sizeof chip);
        CHECK(std::string(chip) == "NO RX");
        chipWord(7.0, temps, nullptr, 0u);  // must not crash
    }

    return testSummary("test_instrument_weather_console");
}
