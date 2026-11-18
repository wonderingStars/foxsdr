// The 3D globe views' body (ui/globe_views.slint GlobeBody) rendered headless, and the one thing a real hand does that a scripted click does not (drive fixes 4, item 4): press a control of the
// column, let the host publish again while the button is down (it publishes every 33 ms, a hand takes 50 to 150 ms), release. The Satellites globe's list of controls and chips changes on every
// publish (the satellites move), and a list handed to a `for` as a NEW model object rebuilds its rows: the press made on a row that no longer exists is never answered. reuseModel (model_reuse.hpp)
// keeps the object. The test prints both lines: a fresh list loses the click, the reused one delivers it.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "globe_body_specimen.h"
#include "../model_reuse.hpp"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace foxsdr::slinttest;

namespace {

GlobeControl checkbox(int id, const char* label, bool on) {
    GlobeControl c{};
    c.id = id;
    c.kind = 1;
    c.label = label;
    c.value = on ? 1 : 0;
    c.enabled = true;
    return c;
}

GlobeChipModel chip(const char* text, bool on) {
    GlobeChipModel c{};
    c.text = text;
    c.color = slint::Color::from_argb_uint8(255, 255, 160, 0);
    c.on = on;
    c.pressable = true;
    return c;
}

std::vector<GlobeControl> controls() { return {checkbox(1, "FOOTPRINT", true), checkbox(2, "GROUND TRACKS", true), checkbox(3, "NIGHT SIDE", true)}; }
std::vector<GlobeChipModel> chips() { return {chip("OSCAR 7 (AO-7)", true), chip("ISS (ZARYA)", false), chip("GOES 16", false)}; }

}  // namespace

int main() {
    installHeadlessPlatform();
    auto ui = GlobeBodySpecimen::create();
    slint::Window& win = ui->window();
    std::vector<std::pair<int, bool>> toggled;
    std::vector<int> chipsHit;
    auto& ev = ui->global<GlobeEvents>();
    ev.on_toggled([&](int, int id, bool on) { toggled.push_back({id, on}); });
    ev.on_chip_pressed([&](int, int idx) { chipsHit.push_back(idx); });

    GlobeModel m{};
    m.offered = true;
    m.active = true;
    m.kind = 4;
    m.reset_label = "RESET VIEW";
    m.controls = std::make_shared<slint::VectorModel<GlobeControl>>(controls());
    m.chips = std::make_shared<slint::VectorModel<GlobeChipModel>>(chips());
    m.readouts = std::make_shared<slint::VectorModel<slint::SharedString>>(std::vector<slint::SharedString>{});
    m.labels = std::make_shared<slint::VectorModel<Scene3DLabel>>(std::vector<Scene3DLabel>{});
    ui->set_globe(m);
    render(ui, 1000, 620);

    // ---- find the FOOTPRINT row and the second chip by scanning the column with real clicks ----
    float footY = -1.0f, chipY = -1.0f;
    const float x = 760.0f;
    for (float y = 8.0f; y < 560.0f; y += 2.0f) {
        const std::size_t t = toggled.size(), c = chipsHit.size();
        click(win, x, y);
        if (footY < 0.0f && toggled.size() > t && toggled.back().first == 1) { footY = y; }
        if (chipY < 0.0f && chipsHit.size() > c && chipsHit.back() == 1) { chipY = y; }
    }
    std::printf("  FOOTPRINT row at y=%.0f, second chip at y=%.0f\n", static_cast<double>(footY), static_cast<double>(chipY));
    CHECK(footY > 0.0f);
    CHECK(chipY > 0.0f);

    // ---- the real hand: press, the host publishes the lists again (same rows), release ----
    for (int which = 0; which < 2; ++which) {                 // 0 the controls' checkbox, 1 a chip
        const float y = which == 0 ? footY : chipY;
        for (int variant = 0; variant < 2; ++variant) {       // 0 a reused list (the fix), 1 a fresh list (what the host did)
            toggled.clear();
            chipsHit.clear();
            moveTo(win, x, y);
            press(win, x, y);
            if (variant == 0) {
                m.controls = foxsdr::slinthost::reuseModel<GlobeControl>(m.controls, controls());
                m.chips = foxsdr::slinthost::reuseModel<GlobeChipModel>(m.chips, chips());
            } else {
                m.controls = std::make_shared<slint::VectorModel<GlobeControl>>(controls());
                m.chips = std::make_shared<slint::VectorModel<GlobeChipModel>>(chips());
            }
            ui->set_globe(m);
            render(ui, 1000, 620);
            release(win, x, y);
            const std::size_t got = which == 0 ? toggled.size() : chipsHit.size();
            std::printf("  %s: press, publish (%s list), release: %zu clicks\n", which == 0 ? "FOOTPRINT" : "satellite chip", variant == 0 ? "reused" : "fresh", got);
            if (variant == 0) { CHECK(got == 1u); }
        }
    }

    // the model object the window holds is kept, and a changed row is written in place
    auto kept = m.controls;
    auto rows = controls();
    rows[0].value = 0;
    m.controls = foxsdr::slinthost::reuseModel<GlobeControl>(m.controls, rows);
    CHECK(m.controls == kept);
    CHECK(m.controls->row_data(0)->value == 0);
    return testSummary("test_slint_globe_body");
}
