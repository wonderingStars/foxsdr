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

// --- the 34-language screenshot review ------------------------------------------

void testFitOrWrap() {
    std::printf("  fitOrWrap: fits as it is, smaller when it can, wrapped only past the floor\n");
    using cascade::gui::fitOrWrap;
    auto linear = [](float s) { return 6.0f * s; };
    // Fits: untouched, never wrapped.
    const cascade::gui::LineFit a = fitOrWrap(14.0f, 84.0f, linear, 9.8f);
    CHECK(a.px == 14.0f && !a.wrap);
    // A little long: smaller, on one line.
    const cascade::gui::LineFit b = fitOrWrap(14.0f, 66.0f, linear, 9.8f);
    CHECK(!b.wrap && b.px < 14.0f && linear(b.px) <= 66.0f);
    // Too long even at the floor: WRAPPED at its own size - the line the
    // status card used to cut mid-word ("...χωρίς δεδομ").
    const cascade::gui::LineFit c = fitOrWrap(14.0f, 40.0f, linear, 9.8f);
    CHECK(c.wrap);
    CHECK(c.px == 14.0f);
}

void testLongestUnbreakable() {
    std::printf("  longestUnbreakable: words between blanks, and each CJK character alone\n");
    // One pixel per BYTE, so a word's width is its length.
    auto bytes = [](const char* a, const char* b) { return static_cast<float>(b - a); };
    CHECK(cascade::gui::longestUnbreakable("Mode: WFM (FM de radiodifusio)", bytes) == 13.0f);
    CHECK(cascade::gui::longestUnbreakable("", bytes) == 0.0f);
    CHECK(cascade::gui::longestUnbreakable("   ", bytes) == 0.0f);
    CHECK(cascade::gui::longestUnbreakable("one", bytes) == 3.0f);
    // "模式: NFM" - each ideograph (3 bytes) stands alone; "NFM" is the widest run.
    CHECK(cascade::gui::longestUnbreakable("\xE6\xA8\xA1\xE5\xBC\x8F: NFM", bytes) == 3.0f);
    CHECK(cascade::gui::longestUnbreakable("\xE6\xA8\xA1\xE5\xBC\x8F\xE6\xA8\xA1", bytes) == 3.0f);
}

void testChipPlacementRule() {
    std::printf("  chipPlacement: beside, beside with the name wrapped, or below\n");
    using cascade::gui::ChipPlace;
    using cascade::gui::chipPlacement;
    // English: name and key on one line, as always.
    CHECK(chipPlacement(150.0f, 60.0f, 8.0f, 136.0f, 355.0f) == ChipPlace::Beside);
    // A long name whose every word fits beside the key: the key stays on the
    // name's line and the name wraps beside it (ca "Mode: WFM (FM de
    // radiodifusió)" dropped its key onto a line of its own).
    CHECK(chipPlacement(260.0f, 110.0f, 8.0f, 136.0f, 355.0f) == ChipPlace::BesideWrapped);
    // A rail too narrow for even one word beside the key: below, not cut.
    CHECK(chipPlacement(260.0f, 110.0f, 8.0f, 136.0f, 200.0f) == ChipPlace::Below);
}

void testCellWordsFit() {
    std::printf("  fitCellWords: size first, then the tracking, then (only then) a cut\n");
    // Word i is (4 + i) glyphs, each 0.6 of the size wide, tracked by frac * size.
    auto width = [](int i, float s, float tf) {
        const float n = 4.0f + static_cast<float>(i);
        return n * 0.6f * s + (n - 1.0f) * tf * s;
    };
    // Roomy: exactly as asked.
    const cascade::gui::CellWordsFit a = cascade::gui::fitCellWords(9.0f, 0.15f, 60.0f, 9.0f, 2, width);
    CHECK(a.px == 9.0f && a.trackingFrac == 0.15f && a.fits);
    // At the floor with the tracking the five-glyph word is 32.4 px; untracked
    // it is 27 - a 28 px cell holds it once the tracking goes.
    const cascade::gui::CellWordsFit b = cascade::gui::fitCellWords(9.0f, 0.15f, 28.0f, 9.0f, 2, width);
    CHECK(b.fits);
    CHECK(b.trackingFrac == 0.0f);
    CHECK(width(1, b.px, 0.0f) <= 28.5f);
    // Nothing fits: reported, and the caller cuts at a whole glyph.
    const cascade::gui::CellWordsFit c = cascade::gui::fitCellWords(9.0f, 0.15f, 20.0f, 9.0f, 2, width);
    CHECK(!c.fits);
    CHECK(c.trackingFrac == 0.0f);
}

void testCentredFloor() {
    std::printf("  addFittedCentred: a caller's floor below seven tenths is honoured\n");
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* f = cascade::gui::fonts::legend();
    const float px = cascade::gui::fonts::kTinySize;  // seven tenths is 9.8
    const char* word = "J\xC3\x84RJESTELM\xC3\x84";      // JÄRJESTELMÄ
    // A key the word fits at 9.2 px and not at 9.8: the default floor cuts it,
    // the absolute floor keeps it whole.
    const float room = f->CalcTextSizeA(9.2f, FLT_MAX, 0.0f, word).x;
    CHECK(f->CalcTextSizeA(cascade::gui::fitFloorFor(px), FLT_MAX, 0.0f, word).x > room + 0.5f);
    const ImVec2 tl(10.0f, 200.0f);
    const ImVec2 br(tl.x + room + 6.0f, tl.y + 22.0f);
    const float def = cascade::gui::addFittedCentred(dl, f, px, tl, br, IM_COL32_WHITE, word, 3.0f);
    CHECK(def == cascade::gui::fitFloorFor(px));
    const float nine = cascade::gui::addFittedCentred(dl, f, px, tl, br, IM_COL32_WHITE, word, 3.0f,
                                                      0.0f, cascade::gui::kFitFloorPx);
    CHECK(nine < cascade::gui::fitFloorFor(px));
    CHECK(f->CalcTextSizeA(nine, FLT_MAX, 0.0f, word).x <= room + 0.5f);
}

// The key-binding row, measured in the real face: the Catalan action name that
// dropped its key onto a line of its own, in the rail's row width.
void testChipPlacementRealFace() {
    std::printf("  chipPlacement in the real face: the Catalan WFM row keeps its key beside it\n");
    ImFont* f = cascade::gui::fonts::ui();
    const float px = cascade::gui::fonts::kUiSize;
    const char* ca = "Mode: WFM (FM de radiodifusi\xC3\xB3)";
    const char* en = "Mode: WFM (broadcast FM)";
    const float keyW = f->CalcTextSizeA(cascade::gui::fonts::kTinySize, FLT_MAX, 0.0f, "Ctrl+Shift+PageDown").x + 16.0f;
    const float rowW = 340.0f;
    const float nameCa = f->CalcTextSizeA(px, FLT_MAX, 0.0f, ca).x;
    // The premise, in this face: the Catalan name does not fit beside the key.
    if (nameCa + 8.0f + keyW <= rowW) {
        std::printf("      (this face fits the Catalan name on one line: %.1f px)\n", nameCa);
    }
    const cascade::gui::ChipPlace pCa = cascade::gui::chipPlacement(
        nameCa, cascade::gui::longestUnbreakableWidth(f, px, ca), 8.0f, keyW, rowW);
    CHECK(pCa != cascade::gui::ChipPlace::Below);
    const cascade::gui::ChipPlace pEn = cascade::gui::chipPlacement(
        f->CalcTextSizeA(px, FLT_MAX, 0.0f, en).x, cascade::gui::longestUnbreakableWidth(f, px, en),
        8.0f, keyW, rowW);
    CHECK(pEn == cascade::gui::ChipPlace::Beside);
}

// The reason beside a dead SEND key: at its own size when it fits, smaller when
// only that fits, and not at all (the caller wraps it below) past the floor.
void testSameLineFittedText() {
    std::printf("  sameLineFittedText: beside at full size, beside smaller, or not beside\n");
    ImGui::SetNextWindowPos(ImVec2(0.0f, 500.0f));
    ImGui::SetNextWindowSize(ImVec2(400.0f, 300.0f));
    ImGui::Begin("send-test", nullptr, ImGuiWindowFlags_NoDecoration);
    const float px = ImGui::GetFontSize();
    ImGui::Button("SEND", ImVec2(120.0f, 0.0f));
    const float keyY = ImGui::GetItemRectMin().y;
    CHECK(cascade::gui::sameLineFittedText("(short)"));
    CHECK(ImGui::GetItemRectMin().y < keyY + px);  // on the key's line
    // Room beside the key: the window's content width less the key and a gap.
    const float room = ImGui::GetContentRegionAvail().x - 120.0f - ImGui::GetStyle().ItemSpacing.x;
    ImFont* f = ImGui::GetFont();
    // A reason 1.2 x the room at full size: fits at 0.8 of the size, above the floor.
    std::string reason = "(";
    while (f->CalcTextSizeA(px, FLT_MAX, 0.0f, reason.c_str()).x < room * 1.2f) { reason += "wait "; }
    reason += ")";
    ImGui::Button("SEND##2", ImVec2(120.0f, 0.0f));
    const float key2Y = ImGui::GetItemRectMin().y;
    CHECK(cascade::gui::sameLineFittedText(reason.c_str()));
    CHECK(ImGui::GetItemRectMin().y < key2Y + px);
    CHECK(ImGui::GetItemRectMax().x <= ImGui::GetWindowPos().x + 400.0f);
    // Twice the room: nothing drawn beside, the caller wraps it.
    std::string longer = reason + reason;
    ImGui::Button("SEND##3", ImVec2(120.0f, 0.0f));
    CHECK(!cascade::gui::sameLineFittedText(longer.c_str()));
    ImGui::End();
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
    testFitOrWrap();
    testLongestUnbreakable();
    testChipPlacementRule();
    testCellWordsFit();

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
        testCentredFloor();
        testChipPlacementRealFace();
        testSameLineFittedText();
        ImGui::Render();
    } else {
        std::printf("fonts::load() failed - the typeface half was not run\n");
    }
    ImGui::DestroyContext();
    return testSummary("test_text_fit");
}
