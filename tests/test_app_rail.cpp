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
 * AND THE LISTS THOSE LAST TWO WALK ARE THE WEAK POINT. They are a hand
 * transcription of what the rail draws, and an audit found them wrong in both
 * directions at once: three chips that no call site has ever passed, two more
 * that belong to other panels entirely, one the Radar row computed and could
 * never draw, a map row spelled in a way the rail has never lettered - and
 * fifteen chips missing. They are grouped by the row that draws them now, so
 * an added row shows up as a gap rather than hiding in an alphabet.
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

// EVERY CHIP THE MAIN WINDOW'S RAIL CAN SHOW, row by row, gathered from the
// benchSection and benchSwitchRow call sites in app_window.cpp. The widest of
// them is what a label has to clear.
//
// IT WAS A HAND TRANSCRIPTION AND IT WAS WRONG IN BOTH DIRECTIONS. It carried
// "RIGCTLD", "CHECKING" and "REPORTING", which no call site anywhere has ever
// passed; "NO DATA" and "STOPPED", which are drawn by the radar scope's detail
// panel and the status column and are not rail chips at all; and "SCOPE",
// which the Radar row computed but could never draw, because scope mode
// replaces the whole layout and the rail is only reached in the else arm of
// it. Meanwhile fifteen chips the rail really does letter were missing. A
// transcription is exactly the thing that drifts, which is why the two lists
// below are grouped by the row that draws them: an added row with no entry
// here is visible as a gap rather than hidden in an alphabet.
//
// COUNTED CHIPS ARE LISTED AT THEIR WIDEST HONEST READING, not at a tidy
// single digit. "%d TGT" is bounded by PluginUi::kMaxTracksPerPlugin (4000)
// and "%d ROW" by kMaxRowsPerPanel (2000), and those two are the widest chips
// on the whole rail; a one-digit example would have let this check pass on a
// rail that clips the moment a busy decoder fills a map.
//
// THE TWO CHIPS BUILT FROM THIRD-PARTY TEXT ARE DELIBERATELY NOT HERE. The
// list used to hold "B200" and "SIGGEN" as stand-ins for the Source row's
// device name, which measured two names this machine happens to produce rather
// than the ten-character cut the code actually applies; both that row and the
// Target details row are pinned by testRuntimeChipRowsAreBounded instead.
const char* const kChips[] = {
    // Radio: the demodulator, from kModeNames.
    "NFM", "WFM", "AM", "DSB", "USB", "CW", "LSB", "RAW",
    // Audio filters: how many of the three are in the chain.
    "OFF", "1 ON", "2 ON", "3 ON",
    // Sinks: audio leaving, no device ever opened, the stream dead, a plugin
    // holding the mute.
    "ON", "NO DEV", "DEAD", "MUTED",
    // Recorder.
    "REC",
    // Plugin store: a transfer running, never asked, updates waiting, nothing
    // to do.
    "BUSY", "IDLE", "2 UPD", "999 UPD", "OK",
    // Plugins: fed, of decoders fitted.
    "0/0", "99/99",
    // Decoders: how many are being fed.
    "0 FED", "99 FED",
    // Target details with nothing chosen, and the blocked map row.
    "NONE",
    // A map page: the targets its window is showing.
    "12 TGT", "4000 TGT",
    // A plugin's picture window: no picture yet, one arriving, one complete.
    "WAIT", "RX", "IMG",
    // A plugin's own panel: how many rows it holds.
    "12 ROW", "2000 ROW",
    // Display: the band-plan overlay.
    "PLAIN", "PLAN",
    // Bookmarks: how many are saved.
    "0", "9999",
    // Scanner.
    "SCAN",
    // Web access, and CAT control: refused, and the clients on the port.
    "FAIL", "12 CLI", "999 CLI",
    // Updates: a critical one, an ordinary one, a check in flight.
    "IMPT", "NEW", "CHECK",
    // Diagnostics, with the memory dump switched on.
    "ON+DMP"};

// EVERY LABEL THE MAIN WINDOW'S RAIL LETTERS. The first group is fixed text in
// app_window.cpp; the last three are built at run time from a PLUGIN'S OWN
// DISPLAY NAME, which is third-party text with no length bound at all, and are
// the rows the shipped plugins actually produce ("ADS-B", "NOAA APT"). A name
// longer than the plate is testAnOverlongLabelIsBounded's business rather than
// this list's - and so is a plugin panel's row, whose word is the panel's own
// title and for which there is no shipped example to name honestly.
//
// "ADS-B 1090 map" USED TO BE IN HERE AND IS NOT A ROW. The map row is
// "<display name> map", so the ADS-B plugin's is "ADS-B map"; the old entry
// was measuring a string the rail has never drawn. "Satellites map" is a real
// row and stays, as the widest map row the shipped plugins make.
const char* const kLabels[] = {"Source",
                               "Radio",
                               "Audio filters",
                               "Sinks",
                               "Recorder",
                               "Plugin store",
                               "Plugins",
                               "Plugins (12 disabled)",
                               "Decoders",
                               "Target details",
                               "Target maps",
                               "Display",
                               "Radar",
                               "Bookmarks",
                               "Scanner",
                               "Web access",
                               "CAT control (rigctld)",
                               "Updates",
                               "Diagnostics",
                               "Usage reporting",
                               // Built from a plugin's own display name.
                               "Satellites map",
                               "ADS-B map",
                               "NOAA APT image"};

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
    // At whatever size fonts.hpp is set to, the row is the type plus its
    // padding, never below the floor - DERIVED here rather than pinned to a
    // number, because that number has now moved three times (28 at the
    // reference's 18 px, 31 at 0.79.0's 21 px, and down again with Georgia
    // in 0.84.0) and each move was a test edit that proved nothing. The
    // reference row is still what an 18 px label gets, which the loop above
    // checks by measurement.
    {
        const float fromType = cascade::gui::fonts::kUiSize + 2.0f * cascade::gui::kRailRowPadY;
        const float want = fromType > cascade::gui::kRailRowMinH ? fromType
                                                                 : cascade::gui::kRailRowMinH;
        CHECK_NEAR(cascade::gui::railRowHeight(cascade::gui::fonts::kUiSize), want, 0.001);
    }
    CHECK_NEAR(cascade::gui::railRowHeight(18.0f), 28.0f, 0.001);
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

// --- 4. the two rows whose chip is third-party text --------------------------
//
// The Source row's chip is the ACTIVE DEVICE'S OWN NAME and the Target details
// row's is the SELECTED TARGET'S OWN ID, each cut to ten characters by
// app_window.cpp and otherwise arbitrary. They are kept out of kChips because
// a ten-character chip is wider than every state word on the rail, and asking
// "Plugins (12 disabled)" to fit beside one would be demanding room that row
// can never need - the two never appear on the same row.
//
// WHAT IS PINNED IS THE CUT, NOT THAT EVERY WORD SURVIVES IT. The rail clips a
// label it cannot fit, deliberately (testAnOverlongLabelIsBounded), so the
// promise here is that a ten-character chip cannot eat the plate: railPlateLabel
// draws NOTHING AT ALL once the limit reaches the start of the word, and a
// Source row with no word on it reads as a fault rather than as a long name.
//
// "Target details" IS THE ONE THAT DOES NOT CLEAR IT COMFORTABLY - at the
// sizes fonts.hpp is set to now it clears ten W's by about a third of a pixel,
// and a name drawn from glyphs wider than any ASCII one (this face's widest is
// a Latin digraph, a quarter wider than its W) pushes it into the clip. That
// margin is PRINTED rather than asserted: the assertion would be a claim about
// which characters a third party puts in a device name.
void testRuntimeChipRowsAreBounded() {
    std::printf("  a ten-character chip still leaves its row a word\n");
    const ImGuiStyle& st = ImGui::GetStyle();
    const float rowW = cascade::gui::railRowWidth(cascade::gui::kMenuWidth,
                                                  cascade::gui::kRailPlatePad,
                                                  st.WindowPadding.x, st.ScrollbarSize);
    const float rowH = cascade::gui::railRowHeight(cascade::gui::fonts::kUiSize);
    const float left = cascade::gui::railLabelLeft(0.0f, rowH);
    // Ten of the widest printable ASCII glyph in the legend face, which is the
    // worst chip the ten-character cut can produce out of a device name or a
    // track id written in it.
    const char* const kWidestTen = "WWWWWWWWWW";
    const float right = cascade::gui::railLabelRight(rowW, rowH, chipWidth(kWidestTen));
    std::printf("      chip \"%s\" is %.2f px; the word runs %.2f..%.2f\n", kWidestTen,
                static_cast<double>(chipWidth(kWidestTen)), static_cast<double>(left),
                static_cast<double>(right));

    // The row still has a word on it, by the same margin the shipped labels
    // are held to above.
    CHECK(right > left + 40.0f);
    // And the shorter of the two rows keeps its whole name beside it.
    CHECK(left + labelWidth("Source") <= right);
    // The longer one, reported rather than asserted - see the note above.
    std::printf("      \"Target details\" needs %.2f px and has %.2f\n",
                static_cast<double>(labelWidth("Target details")),
                static_cast<double>(right - left));
}

// --- 5. the five bank keys carry their own words --------------------------------
//
// The keys share the plate's width five ways (drawRailBankKeys: an 8 px inset
// each side, 4 px between keys), and a key's word is lettered at kTinySize in
// the legend face. A word wider than its key would be clipped or spill onto
// the next key, and either reads as a design decision rather than a fault -
// exactly the failure the rest of this file exists to catch for the rows.
void testBankKeyLabelsFit() {
    constexpr float kPad = 8.0f;
    constexpr float kGap = 4.0f;
    const float keyW = (cascade::gui::kMenuWidth - 2.0f * kPad -
                        kGap * static_cast<float>(cascade::gui::kRailBankCount - 1)) /
                       static_cast<float>(cascade::gui::kRailBankCount);
    std::printf("  a bank key is %.2f px wide\n", static_cast<double>(keyW));
    for (int i = 0; i < cascade::gui::kRailBankCount; ++i) {
        const char* word = cascade::gui::railBankLabel(cascade::gui::railBankFromIndex(i));
        const float w = cascade::gui::fonts::legend()
                            ->CalcTextSizeA(cascade::gui::fonts::kTinySize, FLT_MAX, 0.0f, word)
                            .x;
        // Four pixels of brass either side of the word, or it is touching the
        // bevel.
        if (w + 8.0f > keyW) {
            std::printf("      \"%s\" needs %.2f px and has %.2f\n", word,
                        static_cast<double>(w + 8.0f), static_cast<double>(keyW));
        }
        CHECK(w + 8.0f <= keyW);
    }
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
    testRuntimeChipRowsAreBounded();
    testBankKeyLabelsFit();

    ImGui::DestroyContext();
    return testSummary("test_app_rail");
}
