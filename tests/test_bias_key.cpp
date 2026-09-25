// THE DECK'S BIAS TEE KEY: its rules and its place, without an open frame.
//
// gui/bias_tee.hpp decides what a press of the key and each answer of its
// dialog DO (BiasKeyGate); gui/tune_control.hpp says where the key stands on
// the deck. This holds both:
//
//   * a press on a radio with no bias tee does nothing and asks nothing;
//   * a lit key (power on) is switched OFF at once, never asked about;
//   * an unlit key ASKS, and only "Turn it on" for the radio it asked about,
//     still present, switches it on - a question overtaken by a radio change
//     or a closed radio switches nothing and remembers nothing;
//   * a yes is remembered for the session for a radio named by SERIAL and for
//     no other radio; a radio named by position is asked every time;
//   * the key stands inside the MASTER compartment under the lamp row, clear
//     of the lamps' words, the transport, the counter plate and the foot
//     rail, at every width the bar is drawn at and for every counter layout.
//
// tests/test_bias_key_app.cpp drives the same rules through AppWindow and the
// shipping HackRF driver; tests/test_theme_census.cpp measures the drawn key
// in every theme.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cstdio>
#include <string>

#include "gui/bias_tee.hpp"
#include "gui/tune_control.hpp"
#include "test_check.hpp"

using cascade::gui::BiasKeyAction;
using cascade::gui::BiasKeyGate;
using cascade::gui::BiasTeePanel;
using cascade::gui::biasKeyCancel;
using cascade::gui::biasKeyConfirm;
using cascade::gui::biasKeyMayRemember;
using cascade::gui::biasKeyPress;
using cascade::gui::biasKeyQuestionStands;
using cascade::gui::biasKeyRadio;

namespace {

BiasTeePanel panel(bool present, bool shown) {
    BiasTeePanel p;
    p.present = present;
    p.shown = shown;
    return p;
}

const std::string kA = biasKeyRadio("hackrf", "serial=0000000000000000457863c8");
const std::string kB = biasKeyRadio("rtlsdr", "serial=00000002");
const std::string kIdx = biasKeyRadio("rtlsdr", "index=0");

void testGate() {
    std::printf("  the gate: nothing without a bias tee, off at once, on only after a yes\n");
    // [1] No bias tee on the open radio: the key is not drawn and a press
    // (from a stale frame, say) does nothing at all.
    {
        BiasKeyGate g;
        CHECK(biasKeyPress(g, panel(false, false), kA) == BiasKeyAction::Nothing);
        CHECK(biasKeyPress(g, panel(false, true), kA) == BiasKeyAction::Nothing);
        CHECK(!g.asking);
    }
    // [2] Lit = power on: OFF at once, never asked, confirmed or not.
    {
        BiasKeyGate g;
        CHECK(biasKeyPress(g, panel(true, true), kA) == BiasKeyAction::SwitchOff);
        CHECK(!g.asking);
    }
    // [3] Unlit: ASK. Nothing is switched by the press itself.
    {
        BiasKeyGate g;
        CHECK(biasKeyPress(g, panel(true, false), kA) == BiasKeyAction::Ask);
        CHECK(g.asking);
        CHECK(g.askingFor == kA);
        CHECK(biasKeyQuestionStands(g, panel(true, false), kA));
        // Cancel: nothing remembered, and the next press asks again.
        biasKeyCancel(g);
        CHECK(!g.asking);
        CHECK(g.confirmed.empty());
        CHECK(biasKeyPress(g, panel(true, false), kA) == BiasKeyAction::Ask);
        // Yes: switch on, and for a serial-named radio remember it.
        CHECK(biasKeyConfirm(g, panel(true, false), kA, true));
        CHECK(!g.asking);
        CHECK(g.confirmed.size() == 1);
        // The next press on that radio switches on without a question...
        CHECK(biasKeyPress(g, panel(true, false), kA) == BiasKeyAction::SwitchOn);
        CHECK(!g.asking);
        // ...and on ANOTHER radio it asks.
        CHECK(biasKeyPress(g, panel(true, false), kB) == BiasKeyAction::Ask);
        biasKeyCancel(g);
    }
    // [4] A yes to a question that no longer applies switches nothing and
    // remembers nothing: the radio changed, the radio lost its bias tee, or
    // there was no question at all.
    {
        BiasKeyGate g;
        CHECK(biasKeyPress(g, panel(true, false), kA) == BiasKeyAction::Ask);
        CHECK(!biasKeyQuestionStands(g, panel(true, false), kB));
        CHECK(!biasKeyConfirm(g, panel(true, false), kB, true));
        CHECK(g.confirmed.empty());
        CHECK(!g.asking);

        CHECK(biasKeyPress(g, panel(true, false), kA) == BiasKeyAction::Ask);
        CHECK(!biasKeyQuestionStands(g, panel(false, false), kA));
        CHECK(!biasKeyConfirm(g, panel(false, false), kA, true));
        CHECK(g.confirmed.empty());

        CHECK(!biasKeyConfirm(g, panel(true, false), kA, true));  // nothing was asked
        CHECK(g.confirmed.empty());
    }
    // [5] A radio named by POSITION: the yes switches it on and is NOT
    // remembered, so it is asked again next time.
    {
        BiasKeyGate g;
        CHECK(!biasKeyMayRemember("index=0"));
        CHECK(!biasKeyMayRemember(""));
        CHECK(biasKeyMayRemember("serial=00000001"));
        CHECK(biasKeyMayRemember("driver=x,serial=1234"));
        CHECK(biasKeyPress(g, panel(true, false), kIdx) == BiasKeyAction::Ask);
        CHECK(biasKeyConfirm(g, panel(true, false), kIdx, biasKeyMayRemember("index=0")));
        CHECK(g.confirmed.empty());
        CHECK(biasKeyPress(g, panel(true, false), kIdx) == BiasKeyAction::Ask);
    }
    // [6] The identity is "kind|args": the same args on another driver is
    // another radio.
    CHECK(biasKeyRadio("hackrf", "serial=1") != biasKeyRadio("airspy", "serial=1"));
}

void testPlace() {
    std::printf("  the key's place: inside the MASTER compartment, clear of every part, at "
                "every bar width\n");
    using cascade::gui::CounterLayout;
    const cascade::gui::FreqRect k = cascade::gui::deckBiasKeyArea();
    int checked = 0;
    for (const CounterLayout layout : {CounterLayout{1, true}, CounterLayout{1, false},
                                       CounterLayout{2, false}, CounterLayout{2, true}}) {
        for (float barW = static_cast<float>(cascade::gui::kDeckMinWindowW) - 50.0f;
             barW <= 2400.0f; barW += 1.0f) {
            const float s = cascade::gui::deckScale(barW, layout, 0.62f);
            const float barH = cascade::gui::deckBarH(layout) * s;
            // As drawn: deck units times the scale, except the pixel-sized
            // parts of the lamp row (4 px and the caption, never under 9 px).
            const float capPx = std::max(9.0f, 14.0f * s);
            const float lampWordsFoot = (86.0f + 7.0f) * s + 4.0f + capPx;
            const float kx0 = k.x0 * s, ky0 = k.y0 * s, kx1 = k.x1 * s, ky1 = k.y1 * s;
            const bool clearOfWords = ky0 >= lampWordsFoot + 2.0f;
            const bool clearOfRail = ky1 <= barH - 3.0f - 2.0f;
            const bool clearOfTransport = kx0 >= (74.0f + 46.0f) * s + 2.0f;
            const bool insideCompartment = kx1 <= cascade::gui::kDeckMasterDividerX * s - 2.0f;
            const bool readable = (k.y1 - k.y0) * s >= 12.0f;
            const bool ok =
                clearOfWords && clearOfRail && clearOfTransport && insideCompartment && readable;
            if (!ok) {
                std::printf("    layout %dx%s bar %.0f scale %.3f: key (%.1f,%.1f)-(%.1f,%.1f) "
                            "words foot %.1f bar %.1f%s%s%s%s%s\n",
                            layout.scale, layout.switches ? "+sw" : "", barW, s, kx0, ky0, kx1,
                            ky1, lampWordsFoot, barH, clearOfWords ? "" : " ON THE WORDS",
                            clearOfRail ? "" : " ON THE RAIL",
                            clearOfTransport ? "" : " ON THE TRANSPORT",
                            insideCompartment ? "" : " PAST THE DIVIDER",
                            readable ? "" : " TOO SMALL");
            }
            CHECK(ok);
            ++checked;
        }
    }
    std::printf("    %d bar widths and layouts checked\n", checked);
}

}  // namespace

int main() {
    std::printf("test_bias_key\n");
    testGate();
    testPlace();
    return testSummary("test_bias_key");
}
