// The GPS serial-port chooser never is a dead click (item 3 of the owner's list: "the COM port selection is unresponsive"). Cause: the drop-down opened only when its list was
// not empty, and the list of ports was read only when the SYSTEM bank's Serial section was first opened, so on a map page it stayed empty and a click did nothing.
//   A. no port: a click opens the list, which holds one line "No serial port was found.", and asks the host to read the ports again (SystemEvents.serial-refresh);
//   B. ports: a click opens the list (again asking for a fresh read), choosing the second entry asks gps-port(1), and the empty line is NOT shown.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "gps_chooser_specimen.h"

#ifndef SLINT_FEATURE_EXPERIMENTAL
#define SLINT_FEATURE_EXPERIMENTAL 1
#endif
#include <slint-testing.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace foxsdr::slinttest;
using std::chrono::milliseconds;

namespace {
#define CHECKV(cond, ...)                         \
    do {                                          \
        const bool ok_ = static_cast<bool>(cond); \
        CHECK(ok_);                               \
        if (!ok_) { std::printf("      "); std::printf(__VA_ARGS__); std::printf("\n"); } \
    } while (0)
slint::SharedString ss(const std::string& s) { return slint::SharedString(s); }
SelectItem item(const char* l) { SelectItem i{}; i.label = ss(l); i.id = ss(l); i.enabled = true; return i; }
}  // namespace

int main() {
    installHeadlessPlatform();
    auto ui = GpsChooserSpecimen::create();
    auto& win = ui->window();
    int refreshes = 0, picked = -1;
    ui->global<SystemEvents>().on_serial_refresh([&] { ++refreshes; });
    ui->global<SystemEvents>().on_gps_port([&](int i) { picked = i; });
    GpsRow g{};
    g.port_index = -1; g.baud_index = -1;
    g.ports = std::make_shared<slint::VectorModel<SelectItem>>(std::vector<SelectItem>{});
    g.bauds = std::make_shared<slint::VectorModel<SelectItem>>(std::vector<SelectItem>{item("9600")});
    g.key_label = ss("Read position from GPS");
    ui->set_gps(g);
    const auto show = [&] { render(ui, 600, 300); settle(milliseconds(30)); render(ui, 600, 300); };
    using Role = slint::language::AccessibleRole;
    const auto count = [&](const std::string& label) {
        int n = 0;
        for (const auto& e : slint::testing::ElementHandle::find_by_accessible_label(ui, label)) { if (e.accessible_role() != Role::Button) { ++n; } }
        return n;
    };
    const auto openChooser = [&] {
        for (const auto& e : slint::testing::ElementHandle::find_by_accessible_label(ui, "GPS serial port")) {
            const auto p = e.absolute_position(); const auto s = e.size();
            click(win, p.x + s.width / 2, p.y + s.height / 2); settle(milliseconds(30)); render(ui, 600, 300);
            return;
        }
        CHECKV(false, "the GPS serial port chooser is not there");
    };
    show();
    openChooser();
    CHECKV(refreshes == 1, "A: %d port reads asked, wanted 1", refreshes);
    CHECKV(count("No serial port was found.") >= 1, "A: an empty chooser shows nothing (dead click)");

    // B: ports exist now
    g.ports = std::make_shared<slint::VectorModel<SelectItem>>(std::vector<SelectItem>{item("COM3"), item("COM7")});
    ui->set_gps(g);
    show();
    openChooser();
    CHECKV(refreshes == 2, "B: %d port reads asked, wanted 2", refreshes);
    CHECKV(count("No serial port was found.") == 0, "B: the empty line shows with ports present");
    bool done = false;
    for (const auto& e : slint::testing::ElementHandle::find_by_accessible_label(ui, "COM7")) {
        if (e.accessible_role() == Role::ListItem) { e.invoke_accessible_default_action(); done = true; break; }
    }
    settle(milliseconds(30));
    CHECKV(done && picked == 1, "B: choosing COM7 gave port index %d (found the entry: %d)", picked, done ? 1 : 0);
    return testSummary("test_slint_gps_chooser");
}
