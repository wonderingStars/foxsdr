// The radiofax instrument face's arithmetic: the phase word to a lamp, the
// tuning needle on a centre-zero scale, the chart feeding out of the slot,
// the line counter's drums and how the deck divides at whatever size the
// window has been dragged to.
//
// WHY ANY OF THIS IS TESTABLE AT ALL. Everything a face decides is in
// instrument_fax_math.hpp, which has no ImGui in it, so the decisions can be
// pinned without a graphics context and the drawing file can be read as
// nothing but paint. The three rules in instrument_face.hpp are what the
// cases below are actually about, and two of them are arithmetic:
//   - NO READING IS DRAWN AS NO READING. An empty phase word lights no lamp,
//     an unmeasured tuning error draws no needle, no lines means no paper and
//     a counter of dashes. Each of those is a check here.
//   - FIT THE RECTANGLE. layout() is given every size from zero upwards and
//     must never hand back a deck taller than the room it was offered.
//
// THE EQUIPMENT AND WHERE IT WAS LOOKED AT, for the drawing in
// src/gui/instrument_fax.cpp: a marine facsimile recorder of the Furuno
// FAX-408 / FAX-410 and JRC JAX-9B kind. Consulted for proportions, colours,
// panel legends and lamp colours, and for what the panel's figures mean:
//   https://www.rcom.nl/wp-content/uploads/2014/09/Furuno_FAX410_Operators_Manual.pdf
//       Furuno FAX-410 operator's manual - outline drawing D-1 (382 x 312.5 x
//       93 mm), the paper cutter and compartment cover, IOC 576/288, 60/90/120
//       scans per minute, 257 mm thermal paper.
//   https://www.furuno.it/docs/SPECIFICATIONSSP_FAX410%20FACSIMILE%20RECEIVER.pdf
//       FAX-410 specification - coating colour Munsell 2.5GY5/1.5, and the
//       1500 Hz black / 2300 Hz white external input.
//   https://www.furuno.it/docs/OPERATOR%20MANUALOME62620C3_FAX408.pdf
//       Furuno FAX-408 operator's manual - the panel artwork, and 1.5.2, which
//       is where the TUNE indicator's colours come from: red above, GREEN in
//       the centre for on frequency, red below.
//   https://alphatronmarine.com/files/secured/docuware_documents/311-WeathFax+JRC+JAX-9B+Instruction+Manual++4-4-2012.pdf
//       JRC JAX-9B instruction manual - panel, appearance, Munsell N4, 260 mm
//       paper, and 240 spm for satellite-rate material.
//   https://www.bom.gov.au/marine/radio-sat/tech-voice-fax.shtml
//       Bureau of Meteorology - the sequence the phase lamps follow: a 300 Hz
//       start signal, 60 phasing lines, the picture, then a 450 Hz stop.
//   https://www.weather.gov/media/marine/rfax.pdf
//       NOAA/NWS "Worldwide Marine Radiofacsimile Broadcast Schedules" -
//       120 lpm at IOC 576 as the working standard, and the "for carrier
//       frequency subtract 1.9 kHz" rule the tuning meter exists to serve.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include <cmath>
#include <cstring>
#include <string>

#include "core/plugin_ui.hpp"
#include "gui/instrument_face.hpp"
#include "gui/instrument_fax_math.hpp"
#include "test_check.hpp"

namespace fx = cascade::gui::faxmath;

namespace {

cascade::core::HostInstrument makeFax(const char* phase, double ioc, double lpm,
                                      double lines, double offsetHz, bool lock) {
    cascade::core::HostInstrument h;
    h.plugin = "test";
    h.title = "Radiofax";
    h.kind = CASCADE_INSTRUMENT_FAX;
    h.have = true;
    h.state.structSize = static_cast<std::uint32_t>(sizeof(CascadeInstrumentState));
    h.state.seq = 4u;
    if (phase != nullptr) {
        std::snprintf(h.state.text[0], CASCADE_INSTRUMENT_TEXT_CHARS, "%s", phase);
    }
    h.state.values[0] = ioc;
    h.state.values[1] = lpm;
    h.state.values[2] = lines;
    h.state.values[3] = offsetHz;
    if (lock) { h.state.flags |= CASCADE_INSTRUMENT_FLAG_LOCK; }
    return h;
}

}  // namespace

int main() {
    // --- the phase ladder ----------------------------------------------------
    //
    // Exactly the five words the contract prints, in the order a transmission
    // goes through them, and NOTHING ELSE lights a lamp.
    CHECK(fx::phaseIndex("IDLE") == fx::kPhaseIdle);
    CHECK(fx::phaseIndex("START") == fx::kPhaseStart);
    CHECK(fx::phaseIndex("PHASING") == fx::kPhasePhasing);
    CHECK(fx::phaseIndex("PICTURE") == fx::kPhasePicture);
    CHECK(fx::phaseIndex("STOP") == fx::kPhaseStop);
    CHECK(std::string(fx::phaseName(fx::kPhasePicture)) == "PICTURE");
    CHECK(std::string(fx::phaseName(fx::kPhaseCount)).empty());

    // NO READING IS NO LAMP. An empty slot, a null, a prefix, a word with
    // something on the end and a word in the wrong case all light nothing -
    // a face that accepted "PIC" would be guessing which lamp the plugin
    // meant.
    CHECK(fx::phaseIndex("") == fx::kPhaseUnknown);
    CHECK(fx::phaseIndex(nullptr) == fx::kPhaseUnknown);
    CHECK(fx::phaseIndex("PICT") == fx::kPhaseUnknown);
    CHECK(fx::phaseIndex("PICTURES") == fx::kPhaseUnknown);
    CHECK(fx::phaseIndex("picture") == fx::kPhaseUnknown);
    CHECK(fx::phaseIndex("STARTED") == fx::kPhaseUnknown);
    // An overlong text - the slot holds 63 characters plus its terminator -
    // must be refused rather than matched on its first word.
    char overlong[CASCADE_INSTRUMENT_TEXT_CHARS];
    std::memset(overlong, 'A', sizeof overlong);
    overlong[sizeof overlong - 1] = '\0';
    CHECK(fx::phaseIndex(overlong) == fx::kPhaseUnknown);
    std::snprintf(overlong, sizeof overlong, "PICTURE                    ");
    CHECK(fx::phaseIndex(overlong) == fx::kPhaseUnknown);

    // --- the tuning meter ----------------------------------------------------
    //
    // CENTRE ZERO: on tune is mid-scale, and the two directions are the two
    // halves of the travel.
    CHECK_NEAR(fx::tuningFrac(0.0), 0.5, 1e-6);
    CHECK(fx::tuningFrac(-50.0) < 0.5);
    CHECK(fx::tuningFrac(50.0) > 0.5);
    CHECK_NEAR(fx::tuningFrac(fx::kTuningFullScaleHz), 1.0, 1e-6);
    CHECK_NEAR(fx::tuningFrac(-fx::kTuningFullScaleHz), 0.0, 1e-6);
    // Symmetric about zero, which is the property that makes leaning left and
    // leaning right mean equal and opposite errors.
    CHECK_NEAR(fx::tuningFrac(120.0) - 0.5, 0.5 - fx::tuningFrac(-120.0), 1e-6);
    // PEGGED, NOT SWEPT OFF THE FACE. The largest figure the slot can hold
    // must still land on the meter.
    CHECK_NEAR(fx::tuningFrac(1e308), 1.0, 1e-6);
    CHECK_NEAR(fx::tuningFrac(-1e308), 0.0, 1e-6);
    CHECK_NEAR(fx::tuningFrac(std::nan("")), 0.5, 1e-6);

    // AND WHETHER THERE IS A READING AT ALL. Zero is a real reading here - a
    // receiver exactly on tune - so it cannot double as "nothing measured".
    // The measurement comes from the phasing signal, so it exists from
    // phasing onwards and not before.
    CHECK(!fx::tuningIsMeasured(fx::kPhaseUnknown));
    CHECK(!fx::tuningIsMeasured(fx::kPhaseIdle));
    CHECK(!fx::tuningIsMeasured(fx::kPhaseStart));
    CHECK(fx::tuningIsMeasured(fx::kPhasePhasing));
    CHECK(fx::tuningIsMeasured(fx::kPhasePicture));
    CHECK(fx::tuningIsMeasured(fx::kPhaseStop));

    {
        char b[24];
        fx::formatOffset(-23.0, b, sizeof b);
        CHECK(std::string(b) == "-23 Hz");
        fx::formatOffset(7.4, b, sizeof b);
        CHECK(std::string(b) == "+7 Hz");
        fx::formatOffset(0.0, b, sizeof b);
        CHECK(std::string(b) == "0 Hz");
        fx::formatOffset(-0.2, b, sizeof b);
        CHECK(std::string(b) == "0 Hz");
        fx::formatOffset(std::nan(""), b, sizeof b);
        CHECK(std::string(b) == "--");
        // The largest value the slot can hold must not be run through the
        // buffer as three hundred characters of decimal.
        fx::formatOffset(1e308, b, sizeof b);
        CHECK(std::string(b) == ">+9999 Hz");
        fx::formatOffset(-1e308, b, sizeof b);
        CHECK(std::string(b) == "<-9999 Hz");
        // A zero-size buffer must be survivable, not written to.
        char guard[2] = {'\x7f', '\x7f'};
        fx::formatOffset(5.0, guard, 0u);
        CHECK(guard[0] == '\x7f');
        fx::formatOffset(5.0, nullptr, 8u);
    }

    // --- the selector readouts -----------------------------------------------
    {
        const fx::Selector s = fx::iocSelector(576.0, true);
        CHECK(s.count == 2);
        CHECK_NEAR(s.v[0], 576.0, 1e-9);
        CHECK_NEAR(s.v[1], 288.0, 1e-9);
        CHECK(s.lit == 0);
    }
    {
        const fx::Selector s = fx::iocSelector(288.0, true);
        CHECK(s.lit == 1);
    }
    {
        // NOTHING LIT WITH NOTHING SENT: the positions are still printed, so
        // the panel says what the format offers, and none of them claims to
        // be in use.
        const fx::Selector s = fx::iocSelector(0.0, true);
        CHECK(s.count == 2 && s.lit == -1);
        const fx::Selector t = fx::iocSelector(576.0, false);
        CHECK(t.count == 2 && t.lit == -1);
    }
    {
        // AN OFF-LIST FIGURE IS SHOWN, NOT SWALLOWED. Refusing to display it
        // would turn a reading into a "no reading".
        const fx::Selector s = fx::iocSelector(352.0, true);
        CHECK(s.count == 3 && s.lit == 2);
        CHECK_NEAR(s.v[2], 352.0, 1e-9);
    }
    {
        const fx::Selector s = fx::lpmSelector(120.0, true);
        CHECK(s.count == 4 && s.lit == 2);
        const fx::Selector t = fx::lpmSelector(60.0, true);
        CHECK(t.lit == 0);
        const fx::Selector u = fx::lpmSelector(240.0, true);
        CHECK(u.lit == 3);
        const fx::Selector v = fx::lpmSelector(90.4, true);
        CHECK(v.count == 5 && v.lit == 4);
        // The ladder is full at five, so a sixth cell cannot be appended and
        // nothing may be lit that is not the figure sent.
        const fx::Selector x = fx::lpmSelector(1e308, true);
        CHECK(x.count == 5 && x.lit == 4);
    }
    {
        char b[16];
        fx::formatSelector(576.0, b, sizeof b);
        CHECK(std::string(b) == "576");
        fx::formatSelector(119.5, b, sizeof b);
        CHECK(std::string(b) == "119.5");
        fx::formatSelector(0.0, b, sizeof b);
        CHECK(std::string(b) == "--");
        fx::formatSelector(1e308, b, sizeof b);
        CHECK(std::string(b) == "--");
        fx::formatSelector(std::nan(""), b, sizeof b);
        CHECK(std::string(b) == "--");
    }

    // --- the line counter ----------------------------------------------------
    CHECK(fx::drumValue(412.0) == 412);
    CHECK(fx::drumValue(0.0) == 0);
    CHECK(fx::drumValue(-5.0) == 0);
    CHECK(fx::drumValue(std::nan("")) == 0);
    // CLAMPED TO ALL NINES, never wrapped: the bottom four digits of 12345 is
    // 2345, which is a SMALLER number than the truth and would be a lie the
    // user could not see.
    CHECK(fx::drumValue(12345.0) == 9999);
    CHECK(fx::drumValue(1e308) == 9999);
    {
        char cells[fx::kDrumDigits + 1] = {0};
        int firstSig = -1;
        fx::drumCells(412, cells, fx::kDrumDigits, &firstSig);
        CHECK(std::string(cells) == "0412");
        CHECK(firstSig == 1);
        fx::drumCells(9999, cells, fx::kDrumDigits, &firstSig);
        CHECK(std::string(cells) == "9999");
        CHECK(firstSig == 0);
        fx::drumCells(0, cells, fx::kDrumDigits, &firstSig);
        CHECK(std::string(cells) == "0000");
        // Every cell is a leading zero, so nothing is lit bright: the drums
        // read as a counter at rest rather than as a live figure of zero.
        CHECK(firstSig == fx::kDrumDigits);
        fx::drumCells(1, cells, fx::kDrumDigits, &firstSig);
        CHECK(std::string(cells) == "0001");
        CHECK(firstSig == 3);
        // A value wider than the drums is clamped by the cells too.
        fx::drumCells(123456, cells, fx::kDrumDigits, &firstSig);
        CHECK(std::string(cells) == "9999");
        // Zero digits and a null buffer must be survivable.
        fx::drumCells(1, nullptr, fx::kDrumDigits, &firstSig);
        fx::drumCells(1, cells, 0, &firstSig);
        fx::drumCells(1, cells, fx::kDrumDigits, nullptr);
    }

    // --- the paper -----------------------------------------------------------
    CHECK_NEAR(fx::paperFrac(0.0), 0.0, 1e-6);
    CHECK_NEAR(fx::paperFrac(-3.0), 0.0, 1e-6);
    CHECK_NEAR(fx::paperFrac(std::nan("")), 0.0, 1e-6);
    CHECK_NEAR(fx::paperFrac(600.0), 0.5, 1e-6);
    CHECK_NEAR(fx::paperFrac(fx::kNominalChartLines), 1.0, 1e-6);
    // A transmission longer than a nominal chart runs the strip to the bottom
    // of the well and HOLDS it there rather than drawing past the face.
    CHECK_NEAR(fx::paperFrac(5000.0), 1.0, 1e-6);
    CHECK_NEAR(fx::paperFrac(1e308), 1.0, 1e-6);
    // Monotone, which is what makes a growing strip mean "more has arrived".
    CHECK(fx::paperFrac(100.0) < fx::paperFrac(200.0));

    // The ruling creeps down its own pitch and wraps, so the chart reads as
    // FEEDING rather than merely getting longer. Always inside [0, pitch).
    CHECK_NEAR(fx::ruleOffset(0.0, 11.0f, 50.0), 0.0, 1e-5);
    CHECK_NEAR(fx::ruleOffset(25.0, 11.0f, 50.0), 5.5, 1e-4);
    CHECK_NEAR(fx::ruleOffset(50.0, 11.0f, 50.0), 0.0, 1e-4);
    CHECK_NEAR(fx::ruleOffset(75.0, 11.0f, 50.0), 5.5, 1e-4);
    for (double n = 0.0; n < 400.0; n += 7.0) {
        const float o = fx::ruleOffset(n, 11.0f, 50.0);
        CHECK(o >= 0.0f && o < 11.0f);
    }
    CHECK_NEAR(fx::ruleOffset(std::nan(""), 11.0f, 50.0), 0.0, 1e-6);
    CHECK_NEAR(fx::ruleOffset(10.0, 0.0f, 50.0), 0.0, 1e-6);
    CHECK_NEAR(fx::ruleOffset(10.0, 11.0f, 0.0), 0.0, 1e-6);

    // --- the layout ----------------------------------------------------------
    //
    // A ZERO-SIZE RECTANGLE MUST NOT CRASH AND MUST NOT DRAW. This is the
    // case a resizable window produces on the frame it is dragged shut.
    {
        const fx::Layout L = fx::layout(0.0f, 0.0f);
        CHECK(!L.ok);
        const fx::Layout M = fx::layout(-40.0f, -40.0f);
        CHECK(!M.ok);
        const fx::Layout N = fx::layout(79.0f, 400.0f);
        CHECK(!N.ok);
        const fx::Layout O = fx::layout(400.0f, 55.0f);
        CHECK(!O.ok);
        const fx::Layout P = fx::layout(std::nanf(""), std::nanf(""));
        CHECK(!P.ok);
    }
    // FIT THE RECTANGLE YOU ARE GIVEN, at every size from the smallest that
    // draws anything up to a wall display. The deck, the gap and the paper
    // must never together exceed the height offered.
    for (float w = 80.0f; w <= 2000.0f; w += 37.0f) {
        for (float h = 56.0f; h <= 1400.0f; h += 43.0f) {
            const fx::Layout L = fx::layout(w, h);
            CHECK(L.ok);
            if (!L.ok) { break; }
            CHECK(L.deckH > 0.0f);
            CHECK(L.paperH >= 0.0f);
            CHECK(L.deckH + L.paperH <= h + 0.01f);
            CHECK(L.meterW < w);
            CHECK(L.deckH <= 210.0f);
            CHECK(L.drumDigits == 0 || L.drumDigits == fx::kDrumDigits);
        }
    }
    {
        // The order things are given up in, at the sizes the host actually
        // hands this face. 600 x 460 is the window's first-use size; the deck
        // there is wide and tall enough for everything.
        const fx::Layout big = fx::layout(560.0f, 380.0f);
        CHECK(big.ok && big.meterW > 0.0f && big.lampLadder && big.groupCaption);
        CHECK(big.statusLamps && big.paperH > 0.0f);
        CHECK(big.drumDigits == fx::kDrumDigits);
        // The face also gets the SHORT rectangle - the host gives the memory
        // rows the lower 40 % when a plugin has a log - and the lamps, the
        // counter and enough paper to read as paper must all survive that.
        const fx::Layout squat = fx::layout(560.0f, 194.0f);
        CHECK(squat.ok && squat.meterW > 0.0f && squat.lampLadder);
        CHECK(squat.statusLamps);
        CHECK(squat.paperH > 60.0f);
        // Narrow: the meter goes first, then the ladder becomes the word.
        const fx::Layout narrow = fx::layout(300.0f, 300.0f);
        CHECK(narrow.ok && narrow.meterW == 0.0f);
        const fx::Layout tiny = fx::layout(140.0f, 120.0f);
        CHECK(tiny.ok && !tiny.lampLadder && tiny.meterW == 0.0f);
        CHECK(!tiny.statusLamps);
        // Very short: the paper is the last thing to go, and it does go.
        const fx::Layout flat = fx::layout(560.0f, 60.0f);
        CHECK(flat.ok && flat.paperH == 0.0f);
    }

    // --- the rail's chip -----------------------------------------------------
    //
    // What this instrument says on the rail without being opened. NEW while
    // something has arrived unlooked-at, WAIT with no state at all, and
    // otherwise the phase word - never an invented one.
    {
        char chip[16];
        const cascade::core::HostInstrument live =
            makeFax("PICTURE", 576.0, 120.0, 412.0, -23.0, true);
        cascade::gui::instrumentChip(live, true, chip, sizeof chip);
        CHECK(std::string(chip) == "NEW");
        cascade::gui::instrumentChip(live, false, chip, sizeof chip);
        CHECK(std::string(chip) == "PICTURE");

        cascade::core::HostInstrument cold;
        cold.kind = CASCADE_INSTRUMENT_FAX;
        cold.have = false;
        cascade::gui::instrumentChip(cold, false, chip, sizeof chip);
        CHECK(std::string(chip) == "WAIT");

        // A STATE WITH NO PHASE WORD MUST NOT SAY "IDLE". Reporting a phase
        // the plugin never sent is the invented figure rule 2 forbids; the
        // chip names the instrument instead.
        cascade::core::HostInstrument mute =
            makeFax(nullptr, 0.0, 0.0, 0.0, 0.0, false);
        cascade::gui::instrumentChip(mute, false, chip, sizeof chip);
        CHECK(std::string(chip) == "FAX");
    }

    // --- the slot map, end to end --------------------------------------------
    //
    // The demonstration state the host ships for this kind, read through the
    // same functions the face uses. This is the contract check: a change to
    // which slot carries what would break here rather than in a screenshot.
    {
        const cascade::core::HostInstrument h =
            makeFax("PICTURE", 576.0, 120.0, 412.0, -23.0, true);
        CHECK(fx::phaseIndex(h.state.text[0]) == fx::kPhasePicture);
        CHECK(fx::iocSelector(h.state.values[0], h.have).lit == 0);
        CHECK(fx::lpmSelector(h.state.values[1], h.have).lit == 2);
        CHECK(fx::drumValue(h.state.values[2]) == 412);
        CHECK(fx::tuningIsMeasured(fx::phaseIndex(h.state.text[0])));
        CHECK(fx::tuningFrac(h.state.values[3]) < 0.5);
        CHECK((h.state.flags & CASCADE_INSTRUMENT_FLAG_LOCK) != 0u);
        CHECK(fx::paperFrac(h.state.values[2]) > 0.0f);
    }

    return testSummary("test_instrument_fax");
}
