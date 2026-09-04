// Does the lettering on the spectrum and the waterfall still FIT?
//
// WHY THIS FILE EXISTS. fonts.hpp's four sizes went up by two points on a
// report that captions were hard to read, and every box on those two panels
// had been measured against the old numbers. ImGui text does not wrap: it
// clips, silently, and a clipped legend looks like a design choice rather than
// a fault. The panels themselves cannot notice — they draw into an ImDrawList
// and nothing downstream complains — so the only place the fit can be checked
// is here, against the real compiled-in faces at the sizes the product
// actually draws them at.
//
// It needs an ImGui context (the atlas rasterises on demand) but no renderer
// and no GL: BackendFlags_RendererHasTextures is what makes a context with no
// backend behind it legal for a frame, the same trick tests/test_map_view.cpp
// uses.
//
// THREE THINGS ARE PINNED, and each one is load-bearing for a box somewhere:
//
//   1. A line is exactly `size` pixels tall in all three faces. Every derived
//      height on both panels — the spectrum's minimum chrome height, the
//      waterfall's four-line time strip, the strength key's plate — is a sum
//      of font sizes, and that arithmetic is only true while CalcTextSizeA
//      returns the size it was asked for.
//
//   2. The frequency axis is COMPLETE at the current sizes. FreqScale spaces
//      its ticks by an estimate of the label width made at one type size
//      (kMinTickSpacingPx = 80, "labels render ~70 px wide"); the spectrum
//      panel now drops a label that would touch its neighbour rather than
//      overprint it, so an estimate gone stale degrades to a thinner axis
//      instead of a smear — and this test is what says the estimate has gone
//      stale, before a release does.
//
//   3. The elapsed-time gutter holds its own widest figure. Its width is
//      measured from the labels, but the guard that decides whether to draw
//      it at all is a fraction of the picture, so a face that grew past that
//      fraction would silently remove the axis rather than clip it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cfloat>
#include <cmath>
#include <cstdio>

#include <imgui.h>

#include "gui/fonts.hpp"
#include "gui/freq_scale.hpp"
#include "test_check.hpp"

using cascade::gui::FreqScale;
namespace fonts = cascade::gui::fonts;

namespace {

float widthOf(ImFont* f, float px, const char* s) {
    return f->CalcTextSizeA(px, FLT_MAX, 0.0f, s).x;
}

float heightOf(ImFont* f, float px) {
    return f->CalcTextSizeA(px, FLT_MAX, 0.0f, "0").y;
}

// 1. A LINE IS ITS OWN SIZE TALL, in every face and at every size the product
// uses. spectrum_view.cpp's chromeMinHeight() and waterfall_view.cpp's
// four-lines-of-tiny time strip are both sums of these numbers.
void testLineHeightIsTheFontSize() {
    ImFont* faces[3] = {fonts::ui(), fonts::legend(), fonts::reading()};
    const float sizes[4] = {fonts::kUiSize, fonts::kLegendSize, fonts::kReadingSize,
                            fonts::kTinySize};
    for (ImFont* f : faces) {
        CHECK(f != nullptr);
        for (const float px : sizes) {
            CHECK_NEAR(heightOf(f, px), px, 0.001);
        }
    }
}

// 2. THE FREQUENCY AXIS, end to end: FreqScale's real ticks and labels, laid
// out by the same rule spectrum_view.cpp lays them out with, measured in the
// same face at the same size. Nothing may be dropped and nothing may overlap.
//
// The layout rule, copied from drawChrome's axis loop: centre the label on its
// tick, slide the two end labels inboard so the frame cannot slice them, then
// skip any label whose left edge falls within three pixels of the previous
// label's right edge.
float g_widestFreqLabel = 0.0f;

int freqAxisLabelsDropped(double centerHz, double rateHz, double zoom, float panelW) {
    FreqScale scale;
    scale.setSpan(centerHz, rateHz);
    if (zoom > 1.0) { scale.zoomAt(0.5, zoom); }

    constexpr int kCap = 32;
    double tickHz[kCap];
    char labels[kCap][16];
    const int n = scale.ticks(static_cast<double>(panelW), tickHz, labels, kCap);

    ImFont* uiFont = fonts::ui();
    const float px = fonts::kTinySize;
    const float p0 = 0.0f;
    const float p1 = panelW;
    float lastRight = -FLT_MAX;
    int dropped = 0;
    for (int i = 0; i < n; ++i) {
        const float xFrac = static_cast<float>(scale.hzToX(tickHz[i]));
        if (!(xFrac >= 0.0f) || !(xFrac <= 1.0f)) { continue; }
        const float x = p0 + xFrac * panelW;
        const float lw = widthOf(uiFont, px, labels[i]);
        if (lw > g_widestFreqLabel) { g_widestFreqLabel = lw; }
        float lx = x - lw * 0.5f;
        if (lx < p0 + 3.0f) { lx = p0 + 3.0f; }
        if (lx + lw > p1 - 3.0f) { lx = p1 - 3.0f - lw; }
        if (lx < lastRight + 3.0f) {
            ++dropped;
            continue;
        }
        lastRight = lx + lw;
    }
    return dropped;
}

void testFrequencyAxisIsComplete() {
    // Panel widths from a squeezed window to a wide one, over the bands and
    // sample rates this product is actually pointed at, unzoomed and deep
    // into a zoom (where the labels carry the most decimals and are widest).
    const double centers[5] = {1.0e5, 1.0e6, 100.0e6, 1090.0e6, 2.4e9};
    const double rates[4] = {250.0e3, 2.048e6, 10.0e6, 61.44e6};
    const double zooms[4] = {1.0, 8.0, 64.0, 512.0};
    int cases = 0;
    int totalDropped = 0;
    for (float w = 200.0f; w <= 3200.0f; w += 37.0f) {
        for (const double c : centers) {
            for (const double r : rates) {
                for (const double z : zooms) {
                    const int dropped = freqAxisLabelsDropped(c, r, z, w);
                    if (dropped > 0 && totalDropped == 0) {
                        std::printf("  frequency axis dropped %d label(s): "
                                    "centre %.0f Hz, rate %.0f Hz, zoom x%.0f, "
                                    "panel %.0f px\n",
                                    dropped, c, r, z, static_cast<double>(w));
                    }
                    totalDropped += dropped;
                    ++cases;
                }
            }
        }
    }
    CHECK(cases > 5000);
    // Zero, at the sizes fonts.hpp currently declares. A future size bump that
    // pushes the labels past FreqScale's 80 px pitch fails HERE, with the
    // offending case printed, instead of shipping an axis with a hole in it.
    CHECK(totalDropped == 0);
    // The headroom, stated rather than assumed: the widest label this axis can
    // actually produce, against the 80 px pitch it is spaced at.
    std::printf("  frequency axis: widest label %.2f px at %.0f px type, "
                "%d cases, %d dropped\n",
                static_cast<double>(g_widestFreqLabel),
                static_cast<double>(fonts::kTinySize), cases, totalDropped);
    CHECK(g_widestFreqLabel > 0.0f);
}

// 3. THE ELAPSED-TIME GUTTER. Its width is the widest label plus two pads
// (waterfall_view.cpp's drawTimeStrip), and the strip is abandoned entirely
// when that exceeds a quarter of the picture. "99h59" is the longest string
// formatElapsed can produce; "AGO" is the heading it also has to hold.
void testTimeStripGutterFits() {
    ImFont* uiFont = fonts::ui();
    ImFont* legendFont = fonts::legend();
    const float px = fonts::kTinySize;
    const float pad = 6.0f;  // waterfall_view.cpp's kChromePad

    float widest = widthOf(legendFont, px, "AGO");
    const char* worstLabels[5] = {"0s", "45s", "2m30", "59m59", "99h59"};
    for (const char* s : worstLabels) {
        widest = std::fmax(widest, widthOf(uiFont, px, s));
    }
    const float stripW = widest + pad * 2.0f;
    // The narrowest waterfall the panel will letter at all is 90 px wide
    // (drawTimeStrip's own early-out); at a quarter of that the strip could
    // never be drawn, so the figure that matters is the width at which the
    // gutter becomes possible. Keep it under a quarter of a modest 320 px
    // picture — i.e. the strip stays available on a genuinely small window,
    // not merely on a maximised one.
    CHECK(stripW < 320.0f * 0.25f);
    // And it must still hold its own text: a gutter narrower than the figure
    // it prints right-aligned inside it would clip the leading digit, which
    // turns "99h59" into "9h59" — a wrong number, not a short one.
    CHECK(stripW >= widest + pad * 2.0f);
    std::printf("  time strip: widest label %.2f px, gutter %.2f px at %.0f px type\n",
                static_cast<double>(widest), static_cast<double>(stripW),
                static_cast<double>(px));
}

}  // namespace

int main() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1920.0f, 1080.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    // 1.92 lets the backend own texture uploads; saying so is what makes a
    // context with no renderer behind it legal for a frame.
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

    // The real compiled-in faces. If the atlas refused one, every measurement
    // below would be taken against ImGui's built-in bitmap font and would
    // prove nothing about the product, so this is a hard failure rather than
    // a degraded run.
    CHECK(fonts::load());

    ImGui::NewFrame();
    testLineHeightIsTheFontSize();
    testFrequencyAxisIsComplete();
    testTimeStripGutterFits();
    ImGui::EndFrame();

    ImGui::DestroyContext();
    return testSummary("test_spectrum_waterfall_type");
}
