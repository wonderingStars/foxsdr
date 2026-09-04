/*
 * THE FUNCTION SELECT RAIL FITS ITS OWN WORDS.
 *
 * WHY THIS EXISTS. Every width in that rail - the square key, the label plate,
 * the state chip, the lamp - was measured against the typefaces at the sizes
 * gui/fonts.hpp was set to when the rail was drawn. Those sizes were then
 * raised by two points, on a report that captions were hard to read. Dear ImGui
 * text does not wrap when it runs out of room; it CLIPS, silently, and a rail
 * row that has quietly lost the end of its name looks like a design decision
 * rather than a fault. Nothing in the build could tell the difference, so this
 * file is what tells it.
 *
 * THREE THINGS ARE PINNED, and the second is the one that earns the file.
 *
 *   THE ROW STILL HOLDS ITS OWN LABEL. railRowHeight() is fed the label size
 *   the rail actually letters at, and the CollapsingHeader arithmetic
 *   benchSection uses to paint over that header has to land on exactly the same
 *   height - otherwise the plate, the key and the chip are drawn against a deck
 *   the widget is not.
 *
 *   THE CHIP RESERVE AGREES WITH THE CHIP. app_window.hpp's railChipReserve
 *   mirrors scope_face.hpp's drawRailChip, because the chip is drawn by the
 *   scope's face library and the label is drawn by the window. A transcription
 *   of somebody else's constants is exactly the thing that drifts, so this does
 *   not check the transcription - it CALLS drawRailChip, reads back the leftmost
 *   pixel it actually emitted, and fails if the reserve disagrees. Change the
 *   chip's padding in scope_view.cpp and this goes red the same day.
 *
 *   EVERY SHIPPED LABEL FITS BESIDE ITS WIDEST CHIP. At the width one rail row
 *   really gets - the left column, less the plate's inset, the scrolling child's
 *   padding and the scrollbar that child always has - measured with the real
 *   typefaces at the current sizes. This is the check that would have caught a
 *   two-point raise clipping "CAT control (rigctld)".
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <cstdio>
#include <initializer_list>

#include "gui/app_window.hpp"
#include "gui/fonts.hpp"
#include "gui/scope_face.hpp"
#include "gui/theme.hpp"
#include "imgui.h"
#include "test_check.hpp"

namespace {

// Every chip the main window's rail can show, gathered from the call sites in
// app_window.cpp. The widest of them is what a label has to clear.
const char* const kChips[] = {
    "OK",      "IDLE",   "BUSY",  "2 UPD",     "0/0",       "NO DATA", "NONE",
    "12 TGT",  "REC",    "OFF",   "SCAN",      "PLAIN",     "PLAN",    "ON",
    "MUTED",   "NO DEV", "DEAD",  "RIGCTLD",   "CHECKING",  "STOPPED", "REPORTING",
    "WFM",     "NFM",    "AM",    "USB",       "LSB",       "CW",      "RAW",
    "DSB",     "B200",   "SIGGEN", "SCOPE",     "UNIT"};

// Every label the main window's rail letters, plus the two that are built at
// run time from a count and from a plugin's own display name.
const char* const kLabels[] = {"Source",
                               "Radio",
                               "Audio filters",
                               "Sinks",
                               "Plugin store",
                               "Plugins",
                               "Plugins (12 disabled)",
                               "Decoders",
                               "Radar",
                               "Target details",
                               "Satellites map",
                               "ADS-B 1090 map",
                               "Recorder",
                               "Display",
                               "Bookmarks",
                               "Scanner",
                               "Web access",
                               "CAT control (rigctld)",
                               "Updates",
                               "Usage reporting",
                               "Diagnostics"};

float chipWidth(const char* chip) {
    return cascade::gui::fonts::legend()
        ->CalcTextSizeA(cascade::gui::fonts::kTinySize, FLT_MAX, 0.0f, chip)
        .x;
}

float labelWidth(const char* label) {
    return cascade::gui::fonts::ui()
        ->CalcTextSizeA(cascade::gui::fonts::kUiSize, FLT_MAX, 0.0f, label)
        .x;
}

// --- 1. the row still holds its own label ------------------------------------
//
// benchSection paints over a CollapsingHeader whose height is the label size
// plus twice its frame padding, and that padding is derived from the row height
// this function hands out. If the two ever disagree the plate is drawn against
// a deck the widget is not, and the key floats off centre.
void testRowHeightHoldsTheLabel() {
    std::printf("  a rail row is tall enough for the type it letters\n");
    for (float px : {12.0f, 14.0f, 16.0f, cascade::gui::fonts::kUiSize, 20.0f, 24.0f,
                     28.0f}) {
        const float rowH = cascade::gui::railRowHeight(px);
        // The row can always hold the word with the minimum air above and below.
        CHECK(rowH >= px + 2.0f * cascade::gui::kRailRowPadY);
        // ...and never shrinks below the deck the reference draws.
        CHECK(rowH >= cascade::gui::kRailRowMinH);
        // The padding benchSection pushes, and the height ImGui then gives the
        // header. These must land on the row height exactly.
        const float pad = (rowH - px) * 0.5f > 2.0f ? (rowH - px) * 0.5f : 2.0f;
        CHECK_NEAR(px + 2.0f * pad, rowH, 0.001);
    }
    // At the size fonts.hpp is set to now, the row is still the reference's own
    // 28 - the geometry changed from a literal to a measurement without moving
    // a single pixel today.
    CHECK_NEAR(cascade::gui::railRowHeight(cascade::gui::fonts::kUiSize), 28.0f, 0.001);
    // The key stays the reference's 18 at that height.
    CHECK_NEAR(cascade::gui::railKeySize(28.0f), 18.0f, 0.001);
}

// --- 2. the chip reserve agrees with the chip --------------------------------
//
// Not a comparison of constants: drawRailChip is CALLED, and the leftmost thing
// it puts in the draw list is its chip's own frame. That x is what the label
// must not reach.
void testChipReserveMatchesTheDrawnChip() {
    std::printf("  the label's limit is where drawRailChip actually starts\n");
    const float rowH = cascade::gui::railRowHeight(cascade::gui::fonts::kUiSize);
    const float rowW = 211.0f;  // a real row; the exact value does not matter here

    for (const char* chip : kChips) {
        ImGui::NewFrame();
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const int before = dl->VtxBuffer.Size;
        cascade::gui::drawRailChip(dl, ImVec2(0.0f, 0.0f), ImVec2(rowW, rowH), chip,
                                   cascade::gui::theme::kPhosphor, true);
        float leftmost = FLT_MAX;
        for (int i = before; i < dl->VtxBuffer.Size; ++i) {
            const float x = dl->VtxBuffer[i].pos.x;
            if (x < leftmost) { leftmost = x; }
        }
        ImGui::Render();

        // Something has to have been drawn, or this test is checking nothing.
        CHECK(dl->VtxBuffer.Size > before);
        const float reserved = rowW - cascade::gui::railChipReserve(rowH, chipWidth(chip));
        // One pixel of tolerance: the chip's hairline frame is stroked half a
        // pixel outside the rectangle the reserve is computed from.
        if (!(leftmost >= reserved - 1.5f && leftmost <= reserved + 1.5f)) {
            std::printf("      chip \"%s\": drawn from x=%.2f, reserve says %.2f\n", chip,
                        static_cast<double>(leftmost), static_cast<double>(reserved));
        }
        CHECK(leftmost >= reserved - 1.5f);
        CHECK(leftmost <= reserved + 1.5f);
    }

    // A row with no chip keeps back only the lamp.
    const float lampOnly = cascade::gui::railChipReserve(rowH, -1.0f);
    CHECK_NEAR(lampOnly, 2.0f * cascade::gui::railLampRadius(rowH) +
                             cascade::gui::kRailLampEdgeGap,
               0.001);
    CHECK(cascade::gui::railChipReserve(rowH, 0.0f) > lampOnly);
}

// --- 3. every shipped label fits beside the widest chip ----------------------
void testEveryRailLabelFits() {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float rowW = cascade::gui::railRowWidth(cascade::gui::kMenuWidth,
                                                  cascade::gui::kRailPlatePad,
                                                  st.WindowPadding.x, st.ScrollbarSize);
    const float rowH = cascade::gui::railRowHeight(cascade::gui::fonts::kUiSize);
    const float left = cascade::gui::railLabelLeft(0.0f, rowH);

    float widestChip = 0.0f;
    const char* widestChipText = "";
    for (const char* chip : kChips) {
        const float w = chipWidth(chip);
        if (w > widestChip) {
            widestChip = w;
            widestChipText = chip;
        }
    }
    const float right = cascade::gui::railLabelRight(rowW, rowH, widestChip);
    std::printf("  a rail row is %.2f px wide; the word runs %.2f..%.2f "
                "beside the widest chip (\"%s\")\n",
                static_cast<double>(rowW), static_cast<double>(left),
                static_cast<double>(right), widestChipText);

    // The room is real, not negative-by-arithmetic.
    CHECK(right > left + 40.0f);

    for (const char* label : kLabels) {
        const float w = labelWidth(label);
        if (left + w > right) {
            std::printf("      \"%s\" needs %.2f px and has %.2f\n", label,
                        static_cast<double>(w), static_cast<double>(right - left));
        }
        CHECK(left + w <= right);
    }
}

// --- and the clip is real ----------------------------------------------------
//
// The point of railLabelRight is that a label LONGER than the room does not
// simply paint over the chip. A plugin names its own map row, so this string is
// not hypothetical - it is whatever a third party decided to call itself.
void testAnOverlongLabelIsBounded() {
    std::printf("  a label longer than the plate is bounded, not painted over the chip\n");
    const float rowH = cascade::gui::railRowHeight(cascade::gui::fonts::kUiSize);
    const float rowW = 211.0f;
    const char* huge = "Weather satellite constellation tracker map";
    const float right = cascade::gui::railLabelRight(rowW, rowH, chipWidth("12 TGT"));
    // The premise: this label genuinely does not fit, so the limit is what
    // stops it rather than luck.
    CHECK(cascade::gui::railLabelLeft(0.0f, rowH) + labelWidth(huge) > right);
    // And the limit is inboard of the chip by the stated gap.
    CHECK_NEAR(right,
               rowW - cascade::gui::railChipReserve(rowH, chipWidth("12 TGT")) -
                   cascade::gui::kRailLabelChipGap,
               0.001);
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
    cascade::gui::theme::applyTheme();

    // THE REAL TYPEFACES, at the sizes fonts.hpp is set to. Measuring against
    // ImGui's built-in bitmap font would make every figure here a fiction.
    const bool loaded = cascade::gui::fonts::load();
    CHECK(loaded);
    ImGui::NewFrame();
    ImGui::Render();

    testRowHeightHoldsTheLabel();
    testChipReserveMatchesTheDrawnChip();
    testEveryRailLabelFits();
    testAnOverlongLabelIsBounded();

    ImGui::DestroyContext();
    return testSummary("test_app_rail");
}
