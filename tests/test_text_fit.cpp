// Tests for gui/text_fit.hpp - a word fitted to its box, and letter-spaced
// captions stepped a character at a time.
//
// TWO HALVES.
//
//   1. THE ARITHMETIC (fitSize), against widths this file makes up: a word
//      that fits comes back at EXACTLY its own size - the promise that keeps
//      English where it always was - a word that does not fit comes back at a
//      size where it does, the floor holds, and a width that is not linear in
//      the size (glyph advances land on whole pixels) still converges.
//
//   2. THE REAL FACES. A tracked caption must measure and draw a UTF-8
//      character as ONE glyph: "É" is two bytes, and a byte-wise loop measured
//      and drew two missing glyphs for it - "F??????T??????" for FUNCTION
//      SELECT in the pseudo language. Measured in the typefaces fonts.hpp
//      loads, not ImGui's fallback, as test_bench_text_fits does.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/text_fit.hpp"

#include <cfloat>
#include <cmath>
#include <cstdio>

#include <cstring>

#include "gui/fonts.hpp"
#include "imgui.h"
#include "imgui_internal.h"  // ImHashStr: the hash ImGui keys every widget on
#include "test_check.hpp"

namespace {

using cascade::gui::fitSize;

bool g_flag = false;

bool same(float a, float b) { return std::fabs(a - b) < 0.01f; }

void testFitsIsUntouched() {
    std::printf("  fitSize: a word that fits is returned at its own size, bit for bit\n");
    auto linear = [](float s) { return 6.0f * s; };  // "a six-em word"
    CHECK(fitSize(14.0f, 84.0f, linear) == 14.0f);    // exactly full
    CHECK(fitSize(14.0f, 200.0f, linear) == 14.0f);
    CHECK(fitSize(17.0f, 102.5f, linear, 11.9f) == 17.0f);
    // Nothing to fit into, or nothing to fit: the size is left alone rather
    // than divided by zero.
    CHECK(fitSize(14.0f, 0.0f, linear) == 14.0f);
    CHECK(fitSize(14.0f, -5.0f, linear) == 14.0f);
    CHECK(fitSize(0.0f, 50.0f, linear) == 0.0f);
    CHECK(fitSize(14.0f, 50.0f, [](float) { return 0.0f; }) == 14.0f);
}

void testShrinksToFit() {
    std::printf("  fitSize: too wide shrinks until it fits, never below the floor\n");
    auto linear = [](float s) { return 6.0f * s; };
    const float s = fitSize(14.0f, 60.0f, linear);  // wants 10
    CHECK(linear(s) <= 60.0f);
    CHECK(s > 9.8f && s < 14.0f);
    // A room nothing above the floor fits: the floor, and the caller cuts.
    CHECK(fitSize(14.0f, 30.0f, linear) == cascade::gui::kFitFloorPx);
    CHECK(fitSize(14.0f, 30.0f, linear, 11.0f) == 11.0f);
    // Whole-pixel advances: 7 glyphs each round(0.55 * size) wide.
    auto stepped = [](float sz) { return 7.0f * std::round(0.55f * sz); };
    const float r = fitSize(17.0f, 50.0f, stepped);
    CHECK(stepped(r) <= 50.0f);
    CHECK(r < 17.0f && r >= cascade::gui::kFitFloorPx);
    // The relative floor: seven tenths, never under nine.
    CHECK(same(cascade::gui::fitFloorFor(17.0f), 11.9f));
    CHECK(cascade::gui::fitFloorFor(10.0f) == cascade::gui::kFitFloorPx);
}

void testTrackedIsPerCharacter() {
    std::printf("  trackedWidth: a UTF-8 character is one glyph, one tracking gap\n");
    ImFont* f = cascade::gui::fonts::legend();
    const float px = cascade::gui::fonts::kTinySize;
    // With no tracking, a tracked caption is exactly the run ImGui measures.
    const char* accented = "\xC3\x89TAT \xC5\x81\xC3\x93" "D\xC5\xBB";  // ETAT LODZ, accented
    const float whole = f->CalcTextSizeA(px, FLT_MAX, 0.0f, accented).x;
    const float tracked = cascade::gui::trackedWidth(f, px, accented, 0.0f);
    if (!same(whole, tracked)) {
        std::printf("      untracked %.2f px vs ImGui's own %.2f px\n", tracked, whole);
    }
    CHECK(same(whole, tracked));
    // One gap per pair of CHARACTERS: nine characters, eight gaps.
    const float gaps = cascade::gui::trackedWidth(f, px, accented, 3.0f) - tracked;
    if (!same(gaps, 24.0f)) { std::printf("      tracking added %.2f px, not 24\n", gaps); }
    CHECK(same(gaps, 24.0f));
    // ASCII is what it always was.
    CHECK(same(cascade::gui::trackedWidth(f, px, "SIGNAL PATH", 0.0f),
               f->CalcTextSizeA(px, FLT_MAX, 0.0f, "SIGNAL PATH").x));
    // And the fitted tracked size makes the ACCENTED caption fit - in a room
    // four fifths of its tracked width, which is reachable above the floor.
    const float room = cascade::gui::trackedWidth(f, px, accented, px * 0.2f) * 0.8f;
    const float fitted = cascade::gui::fitTrackedPx(f, px, accented, 0.2f, room);
    CHECK(cascade::gui::trackedWidth(f, fitted, accented, fitted * 0.2f) <= room + 0.5f);
}

void testDrawnPerCharacter() {
    std::printf("  addTrackedText: draws one glyph per character, stops at maxX\n");
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* f = cascade::gui::fonts::legend();
    const float px = cascade::gui::fonts::kTinySize;
    // Six vertices per drawn glyph (ImGui's quads): the byte loop drew each
    // half of a two-byte letter as a fallback glyph of its own.
    const int before = dl->VtxBuffer.Size;
    cascade::gui::addTrackedText(dl, f, px, ImVec2(10.0f, 10.0f), IM_COL32_WHITE,
                                 "\xC3\x89\xC5\x81", 1.0f);  // two letters
    const int quads = (dl->VtxBuffer.Size - before) / 4;
    if (quads != 2) { std::printf("      drew %d glyph quads for two letters\n", quads); }
    CHECK(quads == 2);
    // A limit that leaves room for one letter draws one.
    const float oneW = f->CalcTextSizeA(px, FLT_MAX, 0.0f, "\xC3\x89").x;
    const int mid = dl->VtxBuffer.Size;
    cascade::gui::addTrackedText(dl, f, px, ImVec2(10.0f, 40.0f), IM_COL32_WHITE,
                                 "\xC3\x89\xC5\x81", 1.0f, 10.0f + oneW + 0.5f);
    CHECK((dl->VtxBuffer.Size - mid) / 4 == 1);
}

void testFitsBeside() {
    std::printf("  fitsBeside: the second thing moves only when the row cannot hold both\n");
    using cascade::gui::fitsBeside;
    CHECK(fitsBeside(200.0f, 4.0f, 96.0f, 300.0f));   // exactly full
    CHECK(fitsBeside(200.0f, 4.0f, 96.4f, 300.0f));   // float dust is not a move
    CHECK(!fitsBeside(200.0f, 4.0f, 97.0f, 300.0f));  // one pixel over
    CHECK(!fitsBeside(200.0f, 4.0f, 150.0f, 300.0f)); // a translated label
    CHECK(fitsBeside(0.0f, 0.0f, 0.0f, 0.0f));        // nothing beside nothing
    CHECK(!fitsBeside(10.0f, 4.0f, 10.0f, 0.0f));     // a collapsed rail holds nothing
    CHECK(!fitsBeside(10.0f, 4.0f, 10.0f, -5.0f));
}

void testBoxGivingWay() {
    std::printf("  boxGivingWay: the box keeps its height while the page fits, gives way when not\n");
    using cascade::gui::boxGivingWay;
    // Not measured yet: the box it always was.
    CHECK(boxGivingWay(160.0f, 80.0f, 330.0f, 0.0f) == 160.0f);
    // The English page: room to spare, or exactly full - unchanged.
    CHECK(boxGivingWay(160.0f, 80.0f, 330.0f, 150.0f) == 160.0f);
    CHECK(boxGivingWay(160.0f, 80.0f, 330.0f, 170.0f) == 160.0f);
    // A translation 21 px taller underneath: the box is 21 px shorter, and the
    // page's content ends exactly at its foot.
    CHECK(boxGivingWay(160.0f, 80.0f, 330.0f, 191.0f) == 139.0f);
    CHECK(boxGivingWay(160.0f, 80.0f, 330.0f, 191.0f) + 191.0f == 330.0f);
    // Never below the floor, however much is underneath or however small the page.
    CHECK(boxGivingWay(160.0f, 80.0f, 330.0f, 300.0f) == 80.0f);
    CHECK(boxGivingWay(160.0f, 80.0f, -40.0f, 100.0f) == 80.0f);
    // A floor above the height asked for is not a reason to grow.
    CHECK(boxGivingWay(60.0f, 80.0f, 330.0f, 300.0f) == 60.0f);
}

// The helpers inside a real window of known width, so the decision is made
// against ImGui's own measurements.
void testLabelPlacement() {
    std::printf("  labelAboveIfNeeded / sameLineIfFits in a 300 px window\n");
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(300.0f, 400.0f));
    ImGui::Begin("fit-test", nullptr, ImGuiWindowFlags_NoDecoration);
    ImGui::PushItemWidth(180.0f);

    // Fits: the very same pointer comes back, and nothing was drawn above.
    const char* shortLabel = "Size";
    const float y0 = ImGui::GetCursorPosY();
    CHECK(cascade::gui::labelAboveIfNeeded(shortLabel) == shortLabel);
    CHECK(ImGui::GetCursorPosY() == y0);

    // Does not fit: the label is drawn on a line of its own, and what comes
    // back hashes exactly as the original label did - the widget keeps its id.
    // Long enough to overflow the 120 px beside the widget in ANY face: on
    // Windows the UI is lettered in the system's Georgia, on Linux in the
    // embedded Saira Condensed, and a label sized for the wide face fitted the
    // narrow one (the 0.99.27 Linux run failed here, the Windows run did not).
    const char* longLabel = "[Fr\xC3\xA9qu\xC3\xA9\xC3\xB1\xC3\xA7y d\xC3\xAD\xC5\x9Bpl\xC3\xA1y "
                            "\xC3\xA1nd \xC3\xA1 v\xC3\xA9ry l\xC3\xB6ng tr\xC3\xA1il ~~~~~~~~]";
    // The premise, in this face: wider than all the room left beside the
    // 180 px widget in a 300 px window.
    CHECK(ImGui::CalcTextSize(longLabel).x > 300.0f - 180.0f);
    const char* moved = cascade::gui::labelAboveIfNeeded(longLabel);
    CHECK(moved != longLabel);
    CHECK(ImGui::GetCursorPosY() > y0);
    CHECK(ImHashStr(moved, 0, 0) == ImHashStr(longLabel, 0, 0));
    CHECK(cascade::gui::visibleEnd(moved) == moved);  // shows nothing itself

    // A trId-style label keeps ITS id, the part after "###".
    const char* translated = "[D\xC3\xA9ma\xC3\xB1\xC3\xA7h\xC3\xA9 tr\xC3\xA8s longue, "
                             "vraiment tr\xC3\xA8s longue ~~~~~~~~]###Frequency display";
    const char* movedT = cascade::gui::labelAboveIfNeeded(translated);
    CHECK(std::strcmp(movedT, "###Frequency display") == 0);
    CHECK(ImHashStr(movedT, 0, 0) == ImHashStr("Frequency display", 0, 0));

    // Two check boxes: side by side when both fit, the second below when not.
    ImGui::Checkbox("A", &g_flag);
    CHECK(cascade::gui::sameLineIfFits(cascade::gui::checkWidth("B")));
    ImGui::Checkbox("B", &g_flag);
    ImGui::Checkbox("First box", &g_flag);
    CHECK(!cascade::gui::sameLineIfFits(260.0f));
    ImGui::PopItemWidth();
    ImGui::End();
}

}  // namespace

int main() {
    testFitsIsUntouched();
    testShrinksToFit();
    testFitsBeside();
    testBoxGivingWay();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1600.0f, 1000.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    const bool loaded = cascade::gui::fonts::load();
    CHECK(loaded);
    if (loaded) {
        ImGui::NewFrame();
        testTrackedIsPerCharacter();
        testDrawnPerCharacter();
        testLabelPlacement();
        ImGui::Render();
    } else {
        std::printf("fonts::load() failed - the typeface half was not run\n");
    }
    ImGui::DestroyContext();
    return testSummary("test_text_fit");
}
