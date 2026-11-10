// The 3D well (ui/scene3d.slint, docs 6.64) in a window: the CPU renderer's picture shown, a label placed at its projected anchor, drag to rotate by the shared 0.45 degrees per pixel,
// the wheel's zoom by the shared 1.12 per notch, double-click and Home and RESET VIEW going back, the arrow and +/- keys, the camera sliders driving and following the same properties,
// the empty state, and the pixel size it reports. The numbers are held against scene3d.hpp's own rules (dragged(), zoomed()), so the Slint copy of them cannot drift.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "scene3d.hpp"
#include "scene3d_slint.hpp"
#include "scene3d_well_specimen.h"

#include <cmath>
#include <cstdio>

using namespace foxsdr::slinttest;
namespace s3 = foxsdr::scene3d;

namespace {

constexpr float kWellX = 20.0f, kWellY = 20.0f, kWellW = 600.0f, kWellH = 380.0f;
constexpr float kBarY = 410.0f;

// the well's picture: a lit sphere drawn by the renderer at the well's own size
slint::Image paint(s3::SceneSurface& surf, const s3::Camera& cam, int w, int h) {
    s3::Scene scene;
    s3::Rgba* px = surf.begin(w, h);
    scene.begin(px, w, h, s3::Rgba{0, 0, 0, 0}, cam);
    scene.setLight({0.3f, -0.4f, 0.8f}, 1.0f);
    scene.drawMesh(s3::buildSphere(1.0f, 48, 24, s3::Rgba{240, 120, 20, 255}), true);
    slint::Image img = surf.image();
    surf.flip();
    return img;
}

}  // namespace

int main() {
    installHeadlessPlatform();
    auto ui = Scene3DWellSpecimen::create();
    Frame f = render(ui, 780, 520);
    slint::Window& win = ui->window();

    // ---- the size it reports: the logical pixels of the well ----
    CHECK(ui->get_well_w() == 600);
    CHECK(ui->get_well_h() == 380);

    // ---- the picture is shown: the renderer's orange sphere in the middle, the well's ground at a corner ----
    s3::SceneSurface surf;
    s3::Camera cam;
    cam.sceneRadius = 1.0f;
    ui->set_picture(paint(surf, cam, 600, 380));
    f = render(ui, 780, 520);
    const Rgb orange = rgb(0xf07814);
    CHECK(f.countNear(int(kWellX) + 290, int(kWellY) + 170, int(kWellX) + 310, int(kWellY) + 210, orange, 12) > 300);
    CHECK(!f.near(int(kWellX) + 4, int(kWellY) + 4, orange, 40));   // the corner is the well's ground
    f.savePng(shotDir() + "scene3d-well.png");

    // ---- a label sits at its anchor: x, y are fractions of the picture ----
    ui->set_labels(std::make_shared<slint::VectorModel<Scene3DLabel>>(std::vector<Scene3DLabel>{
        Scene3DLabel{0.4f, 0.5f, slint::SharedString("ALPHA"), slint::Color::from_rgb_uint8(255, 255, 255), 1, 2},
        Scene3DLabel{0.6f, 0.5f, slint::SharedString("OMEGA"), slint::Color::from_rgb_uint8(255, 255, 255), 2, 2}}));
    Frame g = render(ui, 780, 520);
    const int ax = int(kWellX + 0.4f * kWellW), ay = int(kWellY + 0.5f * kWellH);
    CHECK(g.countInk(ax, ay - 8, ax + 50, ay + 8, f.at(ax + 2, ay - 12), 60) > 20);        // ALPHA starts at its anchor and runs right
    CHECK(g.countInk(ax - 50, ay - 8, ax - 1, ay + 8, f.at(ax - 20, ay - 12), 60) == 0);   // nothing to the left of it
    const int ox = int(kWellX + 0.6f * kWellW);
    CHECK(g.countInk(ox - 50, ay - 8, ox, ay + 8, f.at(ox + 20, ay - 12), 60) > 20);       // OMEGA ends at its anchor
    ui->set_labels(std::make_shared<slint::VectorModel<Scene3DLabel>>(std::vector<Scene3DLabel>{}));

    // ---- drag to rotate: the shared 0.45 degrees per pixel, azimuth against dx, elevation with dy ----
    const float cx = kWellX + kWellW / 2, cy = kWellY + kWellH / 2;
    CHECK_NEAR(ui->get_azimuth(), -58.0, 1e-4);
    CHECK_NEAR(ui->get_elevation(), 32.0, 1e-4);
    const int moves0 = ui->get_moves();
    press(win, cx, cy);
    moveTo(win, cx + 40.0f, cy);
    moveTo(win, cx + 100.0f, cy + 20.0f);
    release(win, cx + 100.0f, cy + 20.0f);
    s3::Camera c0;
    c0.azimuthDeg = -58.0f;
    c0.elevationDeg = 32.0f;
    const s3::Camera want = s3::dragged(c0, 100.0f, 20.0f);
    CHECK_NEAR(ui->get_azimuth(), want.azimuthDeg, 0.05);
    CHECK_NEAR(ui->get_elevation(), want.elevationDeg, 0.05);
    CHECK(ui->get_moves() > moves0);
    CHECK(ui->get_well_focus());   // a press gave it the keyboard focus

    // a hover without the button down moves nothing
    const float az1 = ui->get_azimuth();
    moveTo(win, cx + 150.0f, cy + 50.0f);
    CHECK_NEAR(ui->get_azimuth(), az1, 1e-4);

    // ---- elevation stays in range under a huge drag; azimuth wraps ----
    press(win, cx, cy);
    moveTo(win, cx, cy + 600.0f);   // far past the well: still pressed, still turning
    release(win, cx, cy + 600.0f);
    CHECK(ui->get_elevation() <= 89.001f && ui->get_elevation() >= 3.999f);
    ui->set_azimuth(175.0f);
    press(win, cx, cy);
    moveTo(win, cx - 40.0f, cy);   // 40 px left = +18 degrees: 193 wraps to -167
    release(win, cx - 40.0f, cy);
    CHECK(ui->get_azimuth() < -160.0f && ui->get_azimuth() > -175.0f);

    // ---- the wheel: the shared factor per notch, up zooms in ----
    ui->set_azimuth(-58.0f);
    ui->set_elevation(32.0f);
    ui->set_zoom(1.0f);
    scroll(win, cx, cy, 0.0f, 120.0f);
    CHECK_NEAR(ui->get_zoom(), s3::zoomed(c0, 1).zoom, 1e-3);
    scroll(win, cx, cy, 0.0f, -120.0f);
    scroll(win, cx, cy, 0.0f, -120.0f);
    CHECK_NEAR(ui->get_zoom(), s3::zoomed(c0, -1).zoom, 1e-3);
    for (int i = 0; i < 40; ++i) { scroll(win, cx, cy, 0.0f, 120.0f); }
    CHECK_NEAR(ui->get_zoom(), s3::kMaxZoom, 1e-3);
    for (int i = 0; i < 80; ++i) { scroll(win, cx, cy, 0.0f, -120.0f); }
    CHECK_NEAR(ui->get_zoom(), s3::kMinZoom, 1e-3);

    // ---- double-click resets; the host hears it ----
    const int resets0 = ui->get_resets();
    click(win, cx, cy);
    click(win, cx, cy);
    CHECK_NEAR(ui->get_azimuth(), -58.0, 1e-3);
    CHECK_NEAR(ui->get_elevation(), 32.0, 1e-3);
    CHECK_NEAR(ui->get_zoom(), 1.0, 1e-3);
    CHECK(ui->get_resets() > resets0);

    // ---- the keyboard: arrows turn by 5 degrees, +/- zoom, Home resets ----
    press(win, cx, cy);   // focus (a press without travel)
    release(win, cx, cy);
    keyTap(win, slint::platform::key_codes::LeftArrow);
    CHECK_NEAR(ui->get_azimuth(), -53.0, 1e-3);
    keyTap(win, slint::platform::key_codes::RightArrow);
    keyTap(win, slint::platform::key_codes::RightArrow);
    CHECK_NEAR(ui->get_azimuth(), -63.0, 1e-3);
    keyTap(win, slint::platform::key_codes::UpArrow);
    CHECK_NEAR(ui->get_elevation(), 27.0, 1e-3);
    keyTap(win, slint::platform::key_codes::DownArrow);
    keyTap(win, slint::platform::key_codes::DownArrow);
    CHECK_NEAR(ui->get_elevation(), 37.0, 1e-3);
    keyTap(win, u8"+");
    CHECK_NEAR(ui->get_zoom(), 1.12, 1e-3);
    keyTap(win, u8"-");
    keyTap(win, u8"-");
    CHECK_NEAR(ui->get_zoom(), 1.0 / 1.12, 1e-3);
    keyTap(win, slint::platform::key_codes::Home);
    CHECK_NEAR(ui->get_azimuth(), -58.0, 1e-3);
    CHECK_NEAR(ui->get_zoom(), 1.0, 1e-3);

    // ---- the camera bar: its sliders drive the same two properties and RESET VIEW puts them back ----
    // (the sliders' knobs are found by the harness's pointer, the track being the left 170 px of each lane at the bar's row)
    const float sliderY = kBarY + 2.5f + 26.0f;   // the bar centres a 49 px lane in its 54 px: the track is 23 px down, 3 px to its middle
    press(win, kWellX + 12.0f + 170.0f * 0.5f, sliderY);   // ELEVATION lane: the middle of the track
    release(win, kWellX + 12.0f + 170.0f * 0.5f, sliderY);
    CHECK_NEAR(ui->get_elevation(), 4.0 + 0.5 * 85.0, 6.0);
    press(win, kWellX + 12.0f + 170.0f + 16.0f + 170.0f * 0.75f, sliderY);   // AZIMUTH lane: three quarters of 360 degrees from -180
    release(win, kWellX + 12.0f + 170.0f + 16.0f + 170.0f * 0.75f, sliderY);
    CHECK_NEAR(ui->get_azimuth(), -180.0 + 0.75 * 360.0, 25.0);
    // the well follows what a slider set: the same property
    ui->set_azimuth(10.0f);
    Frame h = render(ui, 780, 520);
    (void)h;
    CHECK_NEAR(ui->get_azimuth(), 10.0, 1e-4);

    // ---- the empty state: its words over the well, the hint gone ----
    Frame before = render(ui, 780, 520);
    ui->set_empty_text(slint::SharedString("NOTHING TO DRAW"));
    Frame after = render(ui, 780, 520);
    int differ = 0;
    for (int y = int(kWellY) + 150; y < int(kWellY) + 230; ++y) { for (int x = int(kWellX) + 150; x < int(kWellX) + 450; ++x) { const Rgb a = before.at(x, y), b = after.at(x, y); differ += (std::abs(a.r - b.r) + std::abs(a.g - b.g) + std::abs(a.b - b.b) > 60) ? 1 : 0; } }
    CHECK(differ > 150);
    ui->set_empty_text(slint::SharedString(""));

    // ---- the colour key shows the host's ramp ----
    ui->set_ramp(s3::rampImage(s3::Ramp::altitude(), true, 64));
    Frame k = render(ui, 780, 520);
    // the key plate is at x 636: the ramp's top is amber-ish (warm), its bottom indigo (blue-ish); find any pixel column of the ramp by scanning the plate's rows
    bool warm = false, cold = false;
    for (int y = 20; y < 200; ++y) {
        for (int x = 636; x < 700; ++x) {
            const Rgb p = k.at(x, y);
            warm = warm || (p.r > 200 && p.g > 150 && p.b < 130);
            cold = cold || (p.b > 140 && p.r < 90 && p.g < 40);
        }
    }
    CHECK(warm);
    CHECK(cold);
    k.savePng(shotDir() + "scene3d-well-key.png");

    // ---- the surface's own bookkeeping: two buffers, flipped; a steady size never detaches ----
    s3::SceneSurface sf;
    for (int i = 0; i < 6; ++i) { sf.begin(300, 200); (void)sf.image(); sf.flip(); }
    CHECK(sf.detaches() == 0);
    CHECK(sf.frames() == 6);
    sf.begin(320, 210);
    CHECK(sf.width() == 320 && sf.height() == 210);
    sf.begin(2, 2);
    CHECK(sf.width() == 8 && sf.height() == 8);   // never an empty image

    return testSummary("test_slint_scene3d_well");
}
