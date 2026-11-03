// How a layout's minimum width travels up through the elements around it (ui/specimens/constraints_specimen.slint): a 1200 px
// layout in a 300 px cell behind six wrappers. The one that must hold the line is C, the Flickable - the wrapper the plugin kit
// uses for a measuring row - and D (the bare layout) is the documented failure the French catalogue exposed. The other rows are
// recorded, not asserted, so a Slint upgrade that changes the rule is seen in the log before it is seen on a tester's screen.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "constraints_specimen.h"

#include <cstdio>

using namespace foxsdr::slinttest;

int main() {
    std::printf("test_slint_constraints: a 1200 px layout in a 300 px cell, behind six wrappers\n");
    installHeadlessPlatform();
    auto ui = ConstraintsSpecimen::create();
    const Frame f = render(ui, 900, 600);
    // the window has a size of its own, so it keeps it; what row D widens is its CELL, which then runs past the window - the
    // failure the French catalogue exposed in the main window (its rows ran past the right edge and cut the pages' keys off)
    CHECK(f.width == 900 && f.height == 600);
    const double a = ui->get_a_w(), b = ui->get_b_w(), c = ui->get_c_w(), d = ui->get_d_w(), e = ui->get_e_w(), fw = ui->get_f_w();
    std::printf("  A Rectangle{width:0}      cell %.0f\n", a);
    std::printf("  B ... clip: true          cell %.0f\n", b);
    std::printf("  C Flickable               cell %.0f\n", c);
    std::printf("  D bare layout             cell %.0f\n", d);
    std::printf("  E width: preferred-width  cell %.0f\n", e);
    std::printf("  F width: natural-width    cell %.0f\n", fw);
    std::printf("  the measuring row inside C reports %.0f\n", static_cast<double>(ui->get_measured()));
    CHECK(static_cast<double>(ui->get_measured()) == 1200.0);   // the Flickable does not stop the measurement
    CHECK(c == 300.0);                                           // ... and does stop the constraint
    CHECK(d > 300.0);                                            // the bare layout widens its cell (the documented failure)
    return testSummary("test_slint_constraints");
}
