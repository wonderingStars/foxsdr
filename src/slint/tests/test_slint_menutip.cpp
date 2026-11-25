// A tip inside a popup must not take the pointer from its row (problem report b277ab24, 0.99.115: "when floating the mouse over a menu button it begins to flash back and
// forth"). A dropdown row's tip used to be a PopupWindow of its own; Slint 1.18.1 makes an open popup modal, so the next pointer move after the tip opened went to the
// tip, the row lost its hover, the tip closed, the next move gave the hover back and 600 ms later the tip opened again. Tips inside a popup are now drawn by a
// PopupTipLayer inside that popup, from the TipLayer's one clock, and nothing takes the pointer.
//
// The test rests a pointer that drifts by one pixel every 130 ms (a hand) on a row of a dropdown for 3 s of real time and counts how often a tip opened and how often the row
// lost the pointer. Before the change: 4 openings and several losses. After: one opening, no loss; the tip says the row's note, and closes when the pointer leaves.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "menutip_specimen.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using namespace foxsdr::slinttest;

int main() {
    installHeadlessPlatform();
    auto ui = MenutipSpecimen::create();
    render(ui, 400, 300);
    settle(std::chrono::milliseconds(50));
    slint::Window& win = ui->window();
    auto& tip = ui->global<TipState>();

    // control: an ordinary tip (outside any popup) opens once and says its words
    moveTo(win, 60.0f, 35.0f);
    settle(std::chrono::milliseconds(800));
    CHECK(tip.get_shown());
    CHECK(std::string(tip.get_text()).find("plain tip") != std::string::npos);
    moveTo(win, 390.0f, 290.0f);
    settle(std::chrono::milliseconds(100));
    CHECK(!tip.get_shown());

    // the dropdown row under a drifting pointer, 3 s
    ui->invoke_open();
    settle(std::chrono::milliseconds(50));
    float x = 60.0f;
    const float y = 118.0f + 26.0f;   // row 1
    moveTo(win, x, y);
    const auto t0 = std::chrono::steady_clock::now();
    int step = 0;
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(3000)) {
        settle(std::chrono::milliseconds(0));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (++step % 13 == 0) {   // every ~130 ms the pointer drifts one pixel
            x += (step / 13) % 2 ? 1.0f : -1.0f;
            moveTo(win, x, y);
        }
    }
    const int openings = ui->get_openings();
    const int lost = ui->get_row1_hover_lost();
    std::printf("  row tip under a drifting pointer, 3 s: openings=%d, times the row lost the pointer=%d, on screen now=%d last text='%s'\n", openings, lost, ui->get_open_now(), std::string(ui->get_last_text()).c_str());
    CHECK(openings == 1);
    CHECK(lost == 0);
    CHECK(ui->get_open_now() == 1);
    CHECK(std::string(ui->get_last_text()) == "note 1");

    // the pointer leaves the row: the tip closes
    moveTo(win, 210.0f, 280.0f);
    settle(std::chrono::milliseconds(100));
    CHECK(ui->get_open_now() == 0);
    // and a row with a tip of its own still opens it (another row, pointer at rest)
    moveTo(win, 60.0f, 118.0f);
    settle(std::chrono::milliseconds(800));
    CHECK(ui->get_open_now() == 1);
    CHECK(std::string(ui->get_last_text()) == "note 0");

    return testSummary("test_slint_menutip");
}
