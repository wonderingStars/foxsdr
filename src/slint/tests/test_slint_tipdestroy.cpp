// A tip owner that is destroyed while its tip timers are due (docs: the tip-timer crash, 0.99.115). Slint 1.18.1 marks every expired timer of a pass as
// being activated and calls them in turn; a timer destroyed by an EARLIER callback of the same pass is still called. A tip owner that kept its own Timer
// was therefore called after its row had been erased (the host's 33 ms tick erases rows in place) and crashed on the freed component (drive s4-outputs,
// about one run in three on Linux). The tips now run off ONE clock in the TipLayer, so a destroyed owner has nothing left to be called.
//
// The test builds the pass on purpose: it hovers the last row, arms a 33 ms timer (started before the tips' own, so due earlier) that erases that row, lets
// the real time run WITHOUT pumping the timers, and pumps once: the erasing timer and the tip's are then due in the same pass.
//   1. the 600 ms wait: the erased row's tip must not open (before: shown with the dead row's owner id);
//   2. the vouching of an open tip: the erased row must not vouch (TipState.beat must not move; before: it moved, from the dead row);
//   3. control: tips of rows that are alive still open, say their text, and close.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "tipdestroy_specimen.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace foxsdr::slinttest;

namespace {
constexpr float kRowX = 60.0f;
float rowY(int i) { return 25.0f + 40.0f * static_cast<float>(i); }
}  // namespace

int main() {
    installHeadlessPlatform();
    auto ui = TipdestroySpecimen::create();
    auto rows = std::make_shared<slint::VectorModel<slint::SharedString>>(std::vector<slint::SharedString>{"a", "b", "c"});
    ui->set_rows(rows);
    render(ui, 400, 200);
    settle(std::chrono::milliseconds(50));
    slint::Window& win = ui->window();
    auto& tip = ui->global<TipState>();

    bool armed = false;
    int erased = 0;
    slint::Timer tick;   // started after the model and before any tip timer is due: it runs first in a pass in which both are due
    tick.start(slint::TimerMode::Repeated, std::chrono::milliseconds(33), [&] {
        if (armed && rows->row_count() == 3) {
            rows->erase(2);   // in place, as the host does: the row's component is destroyed here, inside the timers' pass
            ++erased;
        }
    });

    // 3 (control, first): a live row's tip opens after its wait, says its words and closes when the pointer leaves
    moveTo(win, kRowX, rowY(0));
    settle(std::chrono::milliseconds(800));
    CHECK(tip.get_shown());
    CHECK(std::string(tip.get_text()).find("tip for a") != std::string::npos);
    moveTo(win, 390.0f, 190.0f);
    settle(std::chrono::milliseconds(100));
    CHECK(!tip.get_shown());

    // 1: the row under the pointer is erased in the same pass in which its 600 ms wait ends
    moveTo(win, kRowX, rowY(2));
    settle(std::chrono::milliseconds(40));   // the wait has begun, the tip is not open
    CHECK(!tip.get_shown());
    armed = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(750));   // no pumping: the erasing timer and the tip's wait both fall due
    settle(std::chrono::milliseconds(0));                          // ONE pass
    CHECK(erased == 1);
    CHECK(rows->row_count() == 2);
    std::printf("  600 ms wait of an erased row: tip shown=%d owner=%d\n", int(tip.get_shown()), int(tip.get_owner()));
    CHECK(!tip.get_shown());
    settle(std::chrono::milliseconds(900));
    CHECK(!tip.get_shown());   // and it does not open later either
    moveTo(win, 390.0f, 190.0f);
    settle(std::chrono::milliseconds(100));

    // 2: an open tip's owner is erased in the same pass in which its vouching falls due
    armed = false;
    rows->push_back("c");
    settle(std::chrono::milliseconds(50));
    moveTo(win, kRowX, rowY(2));
    settle(std::chrono::milliseconds(900));
    CHECK(tip.get_shown());
    CHECK(std::string(tip.get_text()).find("tip for c") != std::string::npos);
    const int beat0 = tip.get_beat();
    armed = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));   // longer than the vouching interval, shorter than the layer's 400 ms look
    settle(std::chrono::milliseconds(0));                          // ONE pass
    CHECK(erased == 2);
    std::printf("  vouching of an erased owner: beat %d -> %d\n", beat0, int(tip.get_beat()));
    CHECK(tip.get_beat() == beat0);
    // the tip of an owner that went away is taken down by the layer
    settle(std::chrono::milliseconds(900));
    CHECK(!tip.get_shown());

    // 3 (control, last): the live rows still have their tips
    armed = false;
    moveTo(win, kRowX, rowY(1));
    settle(std::chrono::milliseconds(800));
    CHECK(tip.get_shown());
    CHECK(std::string(tip.get_text()).find("tip for b") != std::string::npos);

    return testSummary("test_slint_tipdestroy");
}
