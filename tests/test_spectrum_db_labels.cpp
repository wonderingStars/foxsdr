// The dB figures down the spectrum's left edge keep their three pixels of air
// when a band-plan ribbon reserves a strip along the top of the well.
//
// WHY THIS FILE EXISTS. SpectrumView::dbLabelStride picks how many 10 dB lines
// to step between figures so that one label never sits closer than its own
// height plus three pixels to the next. It was handed the FULL panel height,
// while the gridlines and their labels are laid out over the panel MINUS
// Chrome::reservedTopPx (the strip a Large band-plan ribbon needs). With the
// ribbon on and a short spectrum pane the stride came out one rung too fine
// and the surviving figures printed with 1.2 to 2.9 px between them instead of
// the promised 3. tests/test_spectrum_view.cpp pins dbLabelStride on its own;
// nothing checked what draw() feeds it, which is where this was wrong.
//
// So this draws the real panel, headless (the same no-renderer context
// tests/test_spectrum_waterfall_type.cpp uses), and reads the figures back out
// of the draw list: every glyph quad in the axis ink, at the axis's x, below
// the header. The glyphs of one figure are clustered; the top of each cluster
// is where that figure was printed, and consecutive tops must be at least a
// label height plus three pixels apart.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cstdio>
#include <vector>

#include <imgui.h>

#include "gui/fonts.hpp"
#include "gui/spectrum_view.hpp"
#include "gui/theme.hpp"
#include "test_check.hpp"

using cascade::gui::SpectrumView;
namespace fonts = cascade::gui::fonts;
namespace theme = cascade::gui::theme;

namespace {

// spectrum_view.cpp's axisInk() and kChromePad, restated: the figures are
// found by the ink they are printed in and the x they are printed at.
const ImU32 kAxisInk = theme::withAlpha(theme::kInkMuted, 0.95f);
constexpr float kChromePad = 8.0f;
constexpr float kGapPx = 3.0f;  // SpectrumView's promised clearance

struct Case {
    int labels = 0;         // figures that survived onto the axis
    float minPitch = 0.0f;  // smallest top-to-top distance between two of them
};

Case drawAndMeasure(float dbMin, float dbMax, float height, float reservedTopPx) {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(900.0f, 700.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("spectrum", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();

    SpectrumView view;
    view.setRange(dbMin, dbMax);
    SpectrumView::Chrome chrome;
    chrome.reservedTopPx = reservedTopPx;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const int vtxBefore = dl->VtxBuffer.Size;
    view.draw(nullptr, 0, 600.0f, height, &chrome);
    const float headerBottom = view.headerBottom();

    std::vector<float> ys;
    for (int i = vtxBefore; i < dl->VtxBuffer.Size; ++i) {
        const ImDrawVert& v = dl->VtxBuffer[i];
        if (v.col != kAxisInk) { continue; }
        if (v.pos.x < p0.x + kChromePad - 1.0f || v.pos.x > p0.x + kChromePad + 60.0f) { continue; }
        if (v.pos.y < headerBottom) { continue; }
        ys.push_back(v.pos.y);
    }
    ImGui::End();
    ImGui::EndFrame();

    std::sort(ys.begin(), ys.end());
    const float labelH = fonts::kTinySize;
    std::vector<float> tops;
    for (const float y : ys) {
        if (tops.empty() || y > tops.back() + labelH) { tops.push_back(y); }
    }
    Case c;
    c.labels = static_cast<int>(tops.size());
    c.minPitch = 1.0e9f;
    for (std::size_t k = 1; k < tops.size(); ++k) {
        c.minPitch = std::min(c.minPitch, tops[k] - tops[k - 1]);
    }
    return c;
}

void checkLabelClearance(float dbMin, float dbMax, float reservedTopPx) {
    const float need = fonts::kTinySize + kGapPx - 0.01f;
    int measured = 0;
    int tooClose = 0;
    for (float h = 60.0f; h <= 400.0f; h += 1.0f) {
        const Case c = drawAndMeasure(dbMin, dbMax, h, reservedTopPx);
        if (c.labels < 2) { continue; }
        ++measured;
        if (c.minPitch < need) {
            if (tooClose == 0) {
                std::printf("  %.0f..%.0f dB, reserved %.0f px, panel %.0f px: figures %.2f px "
                            "apart, need %.2f\n",
                            static_cast<double>(dbMin), static_cast<double>(dbMax),
                            static_cast<double>(reservedTopPx), static_cast<double>(h),
                            static_cast<double>(c.minPitch), static_cast<double>(need));
            }
            ++tooClose;
        }
    }
    std::printf("  %.0f..%.0f dB, reserved %.0f px: %d heights with 2+ figures, %d too close\n",
                static_cast<double>(dbMin), static_cast<double>(dbMax),
                static_cast<double>(reservedTopPx), measured, tooClose);
    // The sweep has to have measured something, or "none too close" is empty.
    CHECK(measured > 100);
    CHECK(tooClose == 0);
}

}  // namespace

int main() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1920.0f, 1080.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    CHECK(fonts::load());

    // The measurement itself is sound: with nothing reserved, where the stride
    // has always been computed from the right height, it finds no crowding.
    checkLabelClearance(-110.0f, 0.0f, 0.0f);
    // The Large band-plan tier (9 px) and Medium (3.6 px) over the default
    // range and the ranges either side of it.
    checkLabelClearance(-110.0f, 0.0f, 9.0f);
    checkLabelClearance(-100.0f, 0.0f, 9.0f);
    checkLabelClearance(-160.0f, 20.0f, 9.0f);
    checkLabelClearance(-110.0f, 0.0f, 3.6f);

    ImGui::DestroyContext();
    return testSummary("test_spectrum_db_labels");
}
