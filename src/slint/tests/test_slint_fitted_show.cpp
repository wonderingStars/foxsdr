// The Fitted modules SHOW key: every loaded row has one between the state chip and START/STOP; lit when the module has a page of its own, greyed (and a click does nothing) when it has none;
// a click on a lit one asks for exactly that row (FittedModules.show-clicked(i)) and is not a START/STOP press.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "fitted_show_specimen.h"

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
FittedRowModel row(const char* name, bool hasPage, bool stopped) {
    FittedRowModel m{};
    m.file = ss(std::string(name) + ".dll"); m.name = ss(name); m.version = ss("1.0"); m.group = ss("Decoders");
    m.state = ss(stopped ? "STOPPED" : "IDLE"); m.tone = ChipTone::Neutral; m.dot = LampState::Off;
    m.can_stop = true; m.offers_start = stopped; m.can_remove = true; m.remove_label = ss("REMOVE"); m.has_page = hasPage;
    return m;
}
}  // namespace

int main() {
    installHeadlessPlatform();
    auto ui = FittedShowSpecimen::create();
    auto& win = ui->window();
    std::vector<FittedRowModel> rows{row("ADS-B", true, false), row("Feeder", false, false), row("Stopped decoder", true, true)};
    ui->set_rows(std::make_shared<slint::VectorModel<FittedRowModel>>(rows));
    render(ui, 1200, 400); settle(milliseconds(30)); render(ui, 1200, 400);
    using Role = slint::language::AccessibleRole;
    std::vector<slint::testing::ElementHandle> keys;
    for (const auto& e : slint::testing::ElementHandle::find_by_accessible_label(ui, "SHOW")) { if (e.accessible_role() != Role::Text) { keys.push_back(e); } }
    CHECKV(keys.size() == 3, "%zu SHOW keys, wanted 3", keys.size());
    if (keys.size() == 3) {
        const auto clickKey = [&](int i) { const auto p = keys[static_cast<std::size_t>(i)].absolute_position(); const auto s = keys[static_cast<std::size_t>(i)].size(); click(win, p.x + s.width / 2, p.y + s.height / 2); settle(milliseconds(20)); };
        clickKey(1);
        CHECKV(ui->get_shows() == 0, "a click on a greyed SHOW asked to show (%d)", ui->get_shows());
        clickKey(0);
        CHECKV(ui->get_shows() == 1 && ui->get_last_show() == 0 && ui->get_runs() == 0, "SHOW of row 0: %d asks, row %d, %d runs", ui->get_shows(), ui->get_last_show(), ui->get_runs());
        clickKey(2);
        CHECKV(ui->get_shows() == 2 && ui->get_last_show() == 2 && ui->get_runs() == 0, "SHOW of row 2: %d asks, row %d", ui->get_shows(), ui->get_last_show());
    }
    return testSummary("test_slint_fitted_show");
}
