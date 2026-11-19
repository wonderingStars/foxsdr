// The map page's details card with the photo (docs/multi-radio-records/photo-new-window.md), on the real PageBody of a kind-5 page.
//   A. a selected target with details and a photo: the card shows the heading, EVERY detail line, the picture (one key labelled "Open this photo's page in your browser") and the
//      credit as text directly under the picture;
//   B. a click ANYWHERE on the picture (centre and near each corner) asks to open the photo's page (KitEvents.details-photo-clicked), once per click; a click on the credit does not;
//   C. no photo offered: no picture key and no credit; "Fetching photo..." shows while one is being fetched, and no picture key then;
//   D. nothing selected (card-shown false): no card at all.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "photo_card_specimen.h"

#ifndef SLINT_FEATURE_EXPERIMENTAL
#define SLINT_FEATURE_EXPERIMENTAL 1
#endif
#include <slint-testing.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
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
template <class T>
std::shared_ptr<slint::VectorModel<T>> modelOf(std::vector<T> v) { return std::make_shared<slint::VectorModel<T>>(std::move(v)); }
struct Box { float x0 = 0, y0 = 0, x1 = 0, y1 = 0; };

const char* kPicLabel = "Open this photo's page in your browser";
const char* kCredit = "Photo: A. Smith / Planespotters.net";

PageTab makePage() {
    PageTab t{};
    t.id = ss("adsb-map");
    t.kind = 5;
    t.title = ss("ADS-B map");
    t.map.targets = modelOf(std::vector<MapTarget>{});
    t.map.legend = modelOf(std::vector<LegendRow>{});
    t.map.list_columns = modelOf(std::vector<slint::SharedString>{ss("CALLSIGN")});
    t.map.list_rows = modelOf(std::vector<TableRow>{});
    t.map.list_widths = modelOf(std::vector<float>{100.f});
    t.map.j.layers = modelOf(std::vector<LayerRow>{});
    t.map.j.grid_labels = modelOf(std::vector<GridLabel>{});
    t.map.j.units = ss("Units: Automatic");
    t.map.j.cache_text = ss("No map tiles are kept on disk.");
    return t;
}

void card(PageTab& t, bool shown, bool photo, bool fetching) {
    DetailsModel d{};
    d.heading = ss("FOXSDR1");
    d.chip = ss("4D2025");
    d.lamp = LampState::Green;
    d.can_go = true;
    std::vector<DetailLine> lines;
    for (const char* l : {"Registration  G-ABCD", "Type  A320 (Airbus A320-214)", "Operator  Example Air", "Country  United Kingdom"}) {
        DetailLine x{}; x.text = ss(l); x.known = true; lines.push_back(x);
    }
    d.lines = modelOf(lines);
    d.photo_fetching = fetching;
    d.photo_ready = photo;
    if (photo) {
        slint::SharedPixelBuffer<slint::Rgba8Pixel> buf(200, 133);
        for (int i = 0; i < 200 * 133; ++i) { buf.begin()[i] = {static_cast<uint8_t>(i % 200), 80, 160, 255}; }
        d.photo = slint::Image(buf);
        d.photo_credit = ss(kCredit);
    }
    t.map.card = d;
    t.map.card_shown = shown;
}
}  // namespace

int main() {
    installHeadlessPlatform();
    auto ui = PhotoCardSpecimen::create();
    auto& win = ui->window();
    int opened = 0;
    ui->global<KitEvents>().on_details_photo_clicked([&] { ++opened; });
    using Role = slint::language::AccessibleRole;
    const auto show = [&](const PageTab& t) { ui->set_page(t); render(ui, 1240, 900); settle(milliseconds(30)); render(ui, 1240, 900); };
    const auto find = [&](const std::string& label, std::optional<Role> role) {
        std::vector<slint::testing::ElementHandle> out;
        for (const auto& e : slint::testing::ElementHandle::find_by_accessible_label(ui, label)) {
            if (role ? (e.accessible_role() == *role) : (e.accessible_role() != Role::Button)) { out.push_back(e); }
        }
        return out;
    };
    const auto box = [&](const std::string& label, std::optional<Role> role) -> std::optional<Box> {
        const auto v = find(label, role);
        if (v.empty()) { return std::nullopt; }
        const auto p = v[0].absolute_position(); const auto s = v[0].size();
        return Box{p.x, p.y, p.x + s.width, p.y + s.height};
    };

    // A and B
    PageTab t = makePage();
    card(t, true, true, false);
    show(t);
    for (const char* l : {"FOXSDR1", "Registration  G-ABCD", "Type  A320 (Airbus A320-214)", "Operator  Example Air", "Country  United Kingdom"}) {
        CHECKV(!find(l, std::nullopt).empty(), "the card does not show \"%s\"", l);
    }
    const auto pic = box(kPicLabel, Role::Button);
    const auto cred = box(kCredit, std::nullopt);
    CHECKV(pic.has_value(), "no picture key");
    CHECKV(cred.has_value(), "no credit text");
    if (pic && cred) {
        std::printf("  picture (%.0f,%.0f)-(%.0f,%.0f), credit (%.0f,%.0f)-(%.0f,%.0f)\n", pic->x0, pic->y0, pic->x1, pic->y1, cred->x0, cred->y0, cred->x1, cred->y1);
        CHECKV(pic->x1 - pic->x0 > 100.f && pic->y1 - pic->y0 > 60.f, "the picture is %.0f x %.0f", pic->x1 - pic->x0, pic->y1 - pic->y0);
        CHECKV(cred->y0 >= pic->y1 - 1.f && cred->y0 - pic->y1 <= 8.f, "the credit is not directly under the picture (gap %.0f)", cred->y0 - pic->y1);
        CHECKV(cred->x0 < pic->x1 && cred->x1 > pic->x0, "the credit is not under the picture's columns");
        const float xs[] = {(pic->x0 + pic->x1) / 2, pic->x0 + 3, pic->x1 - 3};
        const float ys[] = {(pic->y0 + pic->y1) / 2, pic->y0 + 3, pic->y1 - 3};
        int want = 0;
        for (const float x : xs) {
            for (const float y : ys) {
                click(win, x, y); settle(milliseconds(20)); ++want;
                CHECKV(opened == want, "a click at (%.0f,%.0f) on the picture: %d requests, wanted %d", x, y, opened, want);
            }
        }
        click(win, (cred->x0 + cred->x1) / 2, (cred->y0 + cred->y1) / 2); settle(milliseconds(20));
        CHECKV(opened == want, "a click on the credit text opened the page");
    }

    // C: no photo offered
    card(t, true, false, false);
    show(t);
    CHECKV(find(kPicLabel, Role::Button).empty() && find(kCredit, std::nullopt).empty(), "no photo offered: a picture key or a credit shows");
    CHECKV(!find("Registration  G-ABCD", std::nullopt).empty(), "no photo: the details went too");
    // fetching
    card(t, true, false, true);
    show(t);
    CHECKV(!find("Fetching photo...", std::nullopt).empty(), "while fetching, nothing says so");
    CHECKV(find(kPicLabel, Role::Button).empty(), "while fetching, a picture key shows");

    // D: nothing selected
    card(t, false, true, false);
    show(t);
    CHECKV(find("FOXSDR1", std::nullopt).empty() && find(kPicLabel, Role::Button).empty(), "nothing selected: a card shows");
    return testSummary("test_slint_photo_card");
}
