// The utility-meter instrument face's arithmetic: the seven-segment table,
// the register's decomposition, the commodity word, the tamper unpacking, the
// roster lookup and the rail chip.
//
// WHAT THESE TESTS ARE FOR. Everything a meter face can lie about is in
// instrument_meter_math.hpp, and each lie has a test named after it:
//
//   - a segment table that lights the wrong bar draws a plausible WRONG DIGIT
//     rather than a visible fault, so every figure is asserted bar by bar
//     against the shapes a seven-segment cell actually makes;
//   - a register that shows 0 when nothing has been heard is the exact
//     confusion rule two exists to stop, so "no reading" and "a reading of
//     zero" are asserted to produce different pictures;
//   - a face that ghosts the wrong number of leading zeros shows 00048213 or
//     48213 with the 4 dropped, so the significance boundary is pinned;
//   - a tamper unpacking that swaps the two counters puts an encoder fault on
//     the physical flag, so the halves are asserted separately with values
//     that cannot be mistaken for one another (1 and 2, never 1 and 1).
//
// The drawing itself needs a graphics context and is checked by eye against
// screenshots; what IS testable here without one is that the entry point
// refuses a degenerate call rather than walking off it, so the last case
// hands drawMeterFace a zero-size rectangle and no draw list.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <cstring>
#include <string>
#include <vector>

#include "gui/instrument_face.hpp"
#include "gui/instrument_meter_math.hpp"
#include "test_check.hpp"

namespace {

using namespace cascade::gui::meter;

CascadePanelRow row(const char* a, const char* b, const char* c, const char* d) {
    CascadePanelRow r{};
    r.kind = CASCADE_ROW_CELLS;
    r.flags = 0u;
    std::snprintf(r.cells[0], CASCADE_PANEL_CELL_CHARS, "%s", a);
    std::snprintf(r.cells[1], CASCADE_PANEL_CELL_CHARS, "%s", b);
    std::snprintf(r.cells[2], CASCADE_PANEL_CELL_CHARS, "%s", c);
    std::snprintf(r.cells[3], CASCADE_PANEL_CELL_CHARS, "%s", d);
    return r;
}

// --- the segment table ------------------------------------------------------

void testSegments() {
    // Every figure, bar by bar. Written out rather than compared against the
    // implementation's own table, which would assert nothing.
    CHECK(segments('0') == (kSegA | kSegB | kSegC | kSegD | kSegE | kSegF));
    CHECK(segments('1') == (kSegB | kSegC));
    CHECK(segments('2') == (kSegA | kSegB | kSegG | kSegE | kSegD));
    CHECK(segments('3') == (kSegA | kSegB | kSegG | kSegC | kSegD));
    CHECK(segments('4') == (kSegF | kSegG | kSegB | kSegC));
    CHECK(segments('5') == (kSegA | kSegF | kSegG | kSegC | kSegD));
    CHECK(segments('6') == (kSegA | kSegF | kSegG | kSegE | kSegC | kSegD));
    CHECK(segments('7') == (kSegA | kSegB | kSegC));
    CHECK(segments('8') == kSegAll);
    CHECK(segments('9') == (kSegA | kSegB | kSegC | kSegD | kSegF | kSegG));

    // The two properties that catch a transcription slip without repeating the
    // table: 8 is the only figure lighting everything, and 1 the only one
    // lighting two bars.
    int allCount = 0;
    for (char d = '0'; d <= '9'; ++d) {
        if (segments(d) == kSegAll) { ++allCount; }
    }
    CHECK(allCount == 1);
    auto bars = [](char d) {
        int n = 0;
        for (unsigned b = 1u; b <= kSegG; b <<= 1) {
            if ((segments(d) & b) != 0u) { ++n; }
        }
        return n;
    };
    CHECK(bars('1') == 2);
    CHECK(bars('7') == 3);
    CHECK(bars('8') == 7);
    // 0 is the only figure with the middle bar dark besides 1 and 7.
    CHECK((segments('0') & kSegG) == 0u);
    CHECK((segments('6') & kSegG) != 0u);
    CHECK((segments('9') & kSegE) == 0u);  // the one that separates 9 from 8

    // Anything that is not a figure lights nothing - that is how the face asks
    // for a blank cell.
    CHECK(segments(' ') == 0u);
    CHECK(segments('-') == 0u);
    CHECK(segments('A') == 0u);
    CHECK(segments('\0') == 0u);
}

// --- the register -----------------------------------------------------------

void testRegister() {
    // NO READING IS NOT A ZERO. The whole of rule two, in two assertions.
    const Register none = decompose(false, 48213.0);
    CHECK(none.status == Reading::None);
    CHECK(std::strcmp(none.digits, "        ") == 0);

    const Register zero = decompose(true, 0.0);
    CHECK(zero.status == Reading::Ok);
    CHECK(std::strcmp(zero.digits, "00000000") == 0);
    // A register that has never turned shows ONE lit figure, not eight and not
    // none: the significance boundary is the last cell.
    CHECK(zero.firstSignificant == kDigits - 1);

    const Register r = decompose(true, 48213.0);
    CHECK(r.status == Reading::Ok);
    CHECK(std::strcmp(r.digits, "00048213") == 0);
    CHECK(r.firstSignificant == 3);

    // The largest value the slot can hold. The ERT SCM consumption field is 24
    // bits, so 16777215 is the widest real reading and must fit exactly.
    const Register big = decompose(true, 16777215.0);
    CHECK(big.status == Reading::Ok);
    CHECK(std::strcmp(big.digits, "16777215") == 0);
    CHECK(big.firstSignificant == 0);

    // And the widest eight cells can hold at all.
    const Register full = decompose(true, 99999999.0);
    CHECK(full.status == Reading::Ok);
    CHECK(std::strcmp(full.digits, "99999999") == 0);

    // One more than that is a real figure that will not fit, which is its own
    // annunciator and NOT the same as no figure.
    const Register over = decompose(true, 100000000.0);
    CHECK(over.status == Reading::Over);

    // Values no meter register can produce are drawn as no reading rather than
    // as a wrapped or negative one.
    CHECK(decompose(true, -1.0).status == Reading::None);
    CHECK(decompose(true, std::nan("")).status == Reading::None);
    CHECK(decompose(true, HUGE_VAL).status == Reading::Over);

    // Rounding, not truncation: a plugin handing over a double that is one ulp
    // under an integer must not lose the count.
    CHECK(std::strcmp(decompose(true, 99.99999).digits, "00000100") == 0);
    CHECK(std::strcmp(decompose(true, 7.0).digits, "00000007") == 0);
}

// --- the commodity ----------------------------------------------------------

void testCommodity() {
    CHECK(commodity("ELECTRIC") == Commodity::Electric);
    CHECK(commodity("GAS") == Commodity::Gas);
    CHECK(commodity("WATER") == Commodity::Water);
    // Case and a trailing qualifier are both accepted, because a plugin author
    // writes what reads well and the face must still light the right lamp.
    CHECK(commodity("electric") == Commodity::Electric);
    CHECK(commodity("Gas endpoint") == Commodity::Gas);
    CHECK(commodity("  WATER") == Commodity::Water);
    // A word that merely starts the same way is NOT a match: "GASKET" is not
    // gas, and a prefix test without the word boundary would say it was.
    CHECK(commodity("GASKET") == Commodity::Unknown);
    CHECK(commodity("ELECTRICAL") == Commodity::Unknown);
    CHECK(commodity("") == Commodity::Unknown);
    CHECK(commodity(nullptr) == Commodity::Unknown);
    CHECK(commodity("repeater") == Commodity::Unknown);
    // An overlong text (the slot is 64 characters and is not promised to be
    // short) must not run off the end or match by accident.
    std::string longText(200, 'X');
    CHECK(commodity(longText.c_str()) == Commodity::Unknown);
    std::string longGas = "GAS " + std::string(300, 'Y');
    CHECK(commodity(longGas.c_str()) == Commodity::Gas);

    // Only electricity gets a unit with a scale in it; see the header for why
    // a gas index count must not be printed as cubic feet.
    CHECK(std::strcmp(unitWord(Commodity::Electric), "kWh") == 0);
    CHECK(std::strcmp(unitWord(Commodity::Gas), "UNITS") == 0);
    CHECK(std::strcmp(unitWord(Commodity::Water), "UNITS") == 0);
    CHECK(std::strcmp(unitWord(Commodity::Unknown), "UNITS") == 0);
}

// --- tamper -----------------------------------------------------------------

void testTamper() {
    // No state at all: neither counter has a value, and any() is false without
    // that meaning "no tamper".
    const Tamper none = tamper(false, 9.0);
    CHECK(!none.have);
    CHECK(!none.any());

    // A heard meter with nothing wrong: the counters ARE known, and both zero.
    const Tamper clean = tamper(true, 0.0);
    CHECK(clean.have);
    CHECK(clean.physical == 0);
    CHECK(clean.encoder == 0);
    CHECK(!clean.any());

    // The halves, with values that cannot be swapped without the test noticing.
    const Tamper t = tamper(true, 9.0);  // 1 | (2 << 2)
    CHECK(t.have);
    CHECK(t.physical == 1);
    CHECK(t.encoder == 2);
    CHECK(t.any());

    CHECK(tamper(true, 3.0).physical == 3);
    CHECK(tamper(true, 3.0).encoder == 0);
    CHECK(tamper(true, 12.0).physical == 0);
    CHECK(tamper(true, 12.0).encoder == 3);
    CHECK(tamper(true, 15.0).physical == 3);
    CHECK(tamper(true, 15.0).encoder == 3);

    // Bits above the four the slot names are ignored rather than folded in.
    CHECK(tamper(true, 255.0).physical == 3);
    CHECK(tamper(true, 255.0).encoder == 3);

    // Nonsense in the slot is "not measured", never a fabricated fault.
    CHECK(!tamper(true, -1.0).have);
    CHECK(!tamper(true, std::nan("")).have);
}

// --- the roster -------------------------------------------------------------

void testHeard() {
    const std::vector<std::string> headings = {"Meter", "Type", "Reading", "Heard"};
    std::vector<CascadePanelRow> rows;
    rows.push_back(row("28394712", "ELECTRIC", "48213", "2 s"));
    rows.push_back(row("19002231", "GAS", "3308", "41 s"));
    rows.push_back(row("40011923", "WATER", "9921", "12 min"));

    CHECK(heardText(headings, rows, "28394712") == "2 s");
    CHECK(heardText(headings, rows, "40011923") == "12 min");
    // A meter that is not on the roster gets a BLANK cell, never the first
    // row's age - which would put one meter's age against another's number.
    CHECK(heardText(headings, rows, "99999999").empty());
    CHECK(heardText(headings, rows, "").empty());
    CHECK(heardText(headings, rows, nullptr).empty());

    // A roster with no such column, and no roster at all.
    const std::vector<std::string> other = {"Meter", "Reading"};
    CHECK(heardText(other, rows, "28394712").empty());
    CHECK(heardText({}, {}, "28394712").empty());
    CHECK(heardText(headings, {}, "28394712").empty());

    // The heading is matched case-insensitively, because a plugin author
    // capitalises the way the table reads.
    const std::vector<std::string> lower = {"meter", "type", "reading", "heard"};
    CHECK(heardText(lower, rows, "19002231") == "41 s");

    // A row that is not a cells row is skipped rather than read as one.
    std::vector<CascadePanelRow> mixed;
    CascadePanelRow sep{};
    sep.kind = 99u;
    std::snprintf(sep.cells[0], CASCADE_PANEL_CELL_CHARS, "28394712");
    std::snprintf(sep.cells[3], CASCADE_PANEL_CELL_CHARS, "wrong");
    mixed.push_back(sep);
    mixed.push_back(row("28394712", "ELECTRIC", "48213", "2 s"));
    CHECK(heardText(headings, mixed, "28394712") == "2 s");

    // A blank age cell is a blank result, not the string "".size() confusion:
    // the face must draw an empty well either way.
    std::vector<CascadePanelRow> blank;
    blank.push_back(row("28394712", "ELECTRIC", "48213", ""));
    CHECK(heardText(headings, blank, "28394712").empty());

    // More headings than the ABI allows must not read off the end.
    std::vector<std::string> too(CASCADE_PANEL_MAX_COLUMNS + 4, "x");
    too[3] = "Heard";
    CHECK(heardText(too, rows, "28394712") == "2 s");
}

// --- the chip ---------------------------------------------------------------

void testChip() {
    char buf[16];

    chipWord(false, 0u, buf, sizeof buf);
    CHECK(std::strcmp(buf, "WAIT") == 0);
    chipWord(false, 5u, buf, sizeof buf);
    CHECK(std::strcmp(buf, "WAIT") == 0);

    // A heard meter with no roster says what it is, NOT "0 MTR" - a zero
    // standing in for "we have not counted" is the conflation rule two names.
    chipWord(true, 0u, buf, sizeof buf);
    CHECK(std::strcmp(buf, "MTR") == 0);

    chipWord(true, 1u, buf, sizeof buf);
    CHECK(std::strcmp(buf, "1 MTR") == 0);
    chipWord(true, 47u, buf, sizeof buf);
    CHECK(std::strcmp(buf, "47 MTR") == 0);

    // A neighbourhood at the plugin's own roster cap still fits the 16 bytes
    // instrumentChip promises.
    chipWord(true, 200u, buf, sizeof buf);
    CHECK(std::strcmp(buf, "200 MTR") == 0);

    // Degenerate outputs must not be written to.
    chipWord(true, 3u, nullptr, 0u);
    char tiny[4] = {'z', 'z', 'z', 'z'};
    chipWord(true, 3u, tiny, 0u);
    CHECK(tiny[0] == 'z');
}

// --- the degenerate call ----------------------------------------------------

void testDegenerate() {
    cascade::core::HostInstrument in;
    in.kind = CASCADE_INSTRUMENT_METER;
    in.title = "ERT meter";
    in.have = true;
    in.state.structSize = static_cast<std::uint32_t>(sizeof(CascadeInstrumentState));
    std::snprintf(in.state.text[0], CASCADE_INSTRUMENT_TEXT_CHARS, "28394712");
    std::snprintf(in.state.text[1], CASCADE_INSTRUMENT_TEXT_CHARS, "ELECTRIC");
    in.state.values[0] = 48213.0;
    cascade::gui::InstrumentCue cue;

    // A zero-size rectangle, an inverted one, and no draw list at all: each
    // must return without drawing rather than divide by a zero span or index
    // a cell that is not there. The window is user-resizable and ImGui hands
    // out exactly these while a page is being dragged.
    CHECK(cascade::gui::drawMeterFace(nullptr, ImVec2(0, 0), ImVec2(0, 0), in, cue) == 0.0f);
    CHECK(cascade::gui::drawMeterFace(nullptr, ImVec2(10, 10), ImVec2(-40, -40), in, cue) ==
          0.0f);
    CHECK(cascade::gui::drawMeterFace(nullptr, ImVec2(0, 0), ImVec2(600, 460), in, cue) ==
          0.0f);
}

}  // namespace

int main() {
    testSegments();
    testRegister();
    testCommodity();
    testTamper();
    testHeard();
    testChip();
    testDegenerate();
    return testSummary("test_instrument_meter");
}
