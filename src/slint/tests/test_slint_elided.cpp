// The elided family (docs 6.62): a longer translation is elided where the layout gives a text less room than its words ask for, and THE TIP SAYS THE WHOLE OF IT - the way the clip sweep
// (tools/slint_clip_sweep.py) accounts for an elided text - while a text that fits adds nothing to the tip. Held here for the three components that carry the rule: ElidedHint, a Key
// squeezed below its words (its own tooltip follows the label), and a RailKey whose caption is elided. The tip is read from the TipState global the TipTimer writes.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "elided_specimen.h"

#include <chrono>
#include <cstdio>
#include <string>

using namespace foxsdr::slinttest;

namespace {

// Hovers (x, y) long enough for the 600 ms timer, answers whether a tip is shown and what it says, then moves away.
struct Tip {
    bool shown = false;
    std::string text;
};

Tip hover(slint::ComponentHandle<ElidedSpecimen>& ui, float x, float y) {
    slint::Window& win = ui->window();
    moveTo(win, 396, 256);   // rest where nothing listens, so the next hover starts clean
    settle(std::chrono::milliseconds(50));
    moveTo(win, x, y);
    settle(std::chrono::milliseconds(750));
    Tip t;
    t.shown = ui->global<TipState>().get_shown();
    t.text = std::string(ui->global<TipState>().get_text());
    moveTo(win, 396, 256);
    settle(std::chrono::milliseconds(50));
    return t;
}

}  // namespace

int main() {
    installHeadlessPlatform();
    auto ui = ElidedSpecimen::create();
    render(ui, 400, 260);
    settle(std::chrono::milliseconds(50));

    // an ElidedHint far too long for its box says itself; one that fits says nothing
    Tip t = hover(ui, 40, 17);
    CHECK(t.shown);
    CHECK(t.text == "A hint far too long for sixty pixels");
    t = hover(ui, 200, 47);
    CHECK(!t.shown);

    // a Key given less than its words ask for: the label, alone when it has no tooltip and before the tooltip when it has one
    t = hover(ui, 30, 85);
    CHECK(t.shown);
    CHECK(t.text == "A key label too long");
    t = hover(ui, 120, 85);
    CHECK(t.shown);
    CHECK(t.text == "Another long label  -  What it does");
    // a Key that hugs its label keeps its own tooltip exactly
    t = hover(ui, 215, 85);
    CHECK(t.shown);
    CHECK(t.text == "Own tip");

    // a RailKey caption that does not fit the rail: the caption before the tooltip
    t = hover(ui, 42, 142);
    CHECK(t.shown);
    CHECK(t.text == "INSTRUMENTS  -  Tools");

    // a SelectKey whose value does not fit says the value; a Section header whose name does not fit says the name before its tooltip
    t = hover(ui, 240, 122);
    CHECK(t.shown);
    CHECK(t.text == "A value far too long for the key");
    t = hover(ui, 40, 204);
    CHECK(t.shown);
    CHECK(t.text == "A section name far too long to fit  -  Does things");

    // and the pictures: nothing drew past the sixty pixels of the narrow hint (the ellipsis stays inside its box)
    Frame f = render(ui, 400, 260);
    const Rgb bg = rgb(0x15171b);
    CHECK(f.countInk(80, 8, 400, 26, bg, 20) == 0);
    f.savePng(shotDir() + "elided.png");
    return testSummary("test_slint_elided");
}
