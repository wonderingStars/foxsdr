// Tests that the torn-off windows' fixed-size furniture is big enough for the
// words in it, MEASURED IN THE REAL TYPEFACES at the sizes fonts.hpp declares.
//
// WHY THIS TEST EXISTS. Dear ImGui does not wrap or clip an AddText unless it
// is told to, and almost none of the lettering on these panels is told to. A
// key's label is CENTRED in its key; a kind tag is CENTRED in its chip; a
// rocker's plate is drawn at the label's own width. So a box that is too small
// for its word does not truncate - it hangs the word out over both machined
// edges, or prints it through the control beside it, and the result reads as a
// rendering artefact rather than as a layout fault. Nobody files a bug against
// it, which is exactly why it needs a test.
//
// It became a live problem when fonts.hpp raised all four sizes by two points
// on a report that captions were hard to read. Every hard-coded width and
// height in these windows had been fitted by eye against the old numbers.
//
// THE PROPERTY, and it is deliberately not "the number is 84". Every figure
// checked here is now DERIVED from a text measurement, so the assertion is
// that the derivation holds - the box is at least as wide as the widest string
// it can be asked to hold, plus a stated shoulder. That survives the next size
// change; a golden number would have to be edited by hand, which is the
// failure this whole sweep was cleaning up after.
//
// REAL FONTS, NOT PROGGYCLEAN. fonts::load() puts the three shipped typefaces
// in the atlas, and the first thing checked is that it succeeded: a measurement
// taken against ImGui's fallback bitmap face would pass while telling us
// nothing about what the product draws. No GL and no window - ImGui bakes and
// measures on the CPU, which is all this needs.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cfloat>
#include <cstdio>

#include "core/i18n.hpp"
#include "gui/fonts.hpp"
#include "gui/map_view.hpp"
#include "gui/plugin_store_view.hpp"
#include "gui/text_fit.hpp"
#include "gui/tune_control.hpp"
#include "imgui.h"
#include "test_check.hpp"

namespace {

float textW(ImFont* f, float px, const char* s) {
    return f->CalcTextSizeA(px, FLT_MAX, 0.0f, s).x;
}

// --- the coordinate counter's apertures ---------------------------------------
//
// The satellites window's receiver position is a row of machined apertures,
// one per character of "+51.50720" / "+002.10400", and drawFreqDrumCell centres
// its glyph in whatever cell it is handed. A cell narrower than the figure
// therefore bleeds into its neighbour, which on a counter reads as a smear.
void testCoordApertureHoldsItsFigure() {
    ImFont* rf = cascade::gui::fonts::reading();
    const float px = cascade::gui::fonts::kReadingSize;

    // Every character the two formatters can put in an aperture: the ten
    // figures, the decimal point and both signs.
    const char* shapes = "0123456789.+-";
    for (const char* p = shapes; *p != '\0'; ++p) {
        const char s[2] = {*p, '\0'};
        const float glyph = textW(rf, px, s);
        const float cell = cascade::gui::coordCellWidth(*p);
        CHECK(cell >= glyph);
        // ...and with a shoulder, because a figure touching the machined edge
        // of its window reads as a letter in a box rather than as a drum seen
        // through an aperture.
        CHECK(cell >= glyph + 2.0f);
        CHECK(cell > 0.0f);
    }

    // A SIGN IS NARROWER THAN A FIGURE, which is the only reason the width is
    // a function of the character at all. If this ever stops holding, the two
    // branches have collapsed into one and the narrow case is dead code.
    CHECK(cascade::gui::coordCellWidth('.') <= cascade::gui::coordCellWidth('0'));

    // THE COUNTER MUST NOT CHANGE SHAPE WHEN A POSITION ARRIVES. The width is
    // taken from the SHAPE the formatter produced and the glyph finally drawn
    // is coordApertureGlyph(shape, known) - a dash for a figure, a space for a
    // sign - so every substitute has to fit the cell its shape sized.
    for (const char* p = shapes; *p != '\0'; ++p) {
        const char sub[2] = {cascade::gui::coordApertureGlyph(*p, false), '\0'};
        CHECK(cascade::gui::coordCellWidth(*p) >= textW(rf, px, sub));
    }

    // The aperture is taller than the figure in it, by enough to be a window
    // rather than a crop.
    CHECK(cascade::gui::coordCellHeight() >= px + 4.0f);

    // THE FIGURES, PRINTED. A failing CHECK names the expression and not the
    // measurement behind it, and the next person to change fonts.hpp wants to
    // see how much room is actually left rather than re-derive it.
    std::printf("  counter face %.0f px: figure %.2f in a %.2f cell, "
                "point/sign %.2f in a %.2f cell\n",
                px, textW(rf, px, "0"), cascade::gui::coordCellWidth('0'),
                textW(rf, px, "."), cascade::gui::coordCellWidth('.'));
}

// --- the shared kind-tag chip --------------------------------------------------
//
// One component, drawn on the plugin store's cards and on the fitted-modules
// rows. It was two different literals in those two files - 84 px and 74 - and
// neither held "NOT DECLARED" once the engraving grew.
void testKindTagChipHoldsEveryTag() {
    ImFont* uf = cascade::gui::fonts::ui();
    const float px = cascade::gui::fonts::kTinySize;
    const float chip = cascade::gui::moduleKindTagWidth();

    // Every word moduleKindTag() can return. Kept in step with that switch by
    // hand, which is the point: a tag added there and forgotten here is a chip
    // that overflows in whichever state nobody happens to be looking at.
    const char* const tags[] = {"NOT KNOWN", "NOT DECLARED", "DECODER", "MAP",
                                "PANEL",     "CONTROL",      "MODULE"};
    for (const char* t : tags) {
        const float w = textW(uf, px, t);
        CHECK(chip >= w);
        // The chip is a plate with the word cut into it, so it needs metal
        // either side rather than just enough room for the letterforms.
        CHECK(chip >= w + 8.0f);
    }

    // And it is a chip, not a column: something is badly wrong if it has grown
    // wide enough to be mistaken for the module's name beside it.
    CHECK(chip < 400.0f);

    std::printf("  kind tag chip %.2f px, widest tag \"NOT DECLARED\" %.2f px at %.0f px\n",
                chip, textW(uf, px, "NOT DECLARED"), px);
}

// EVERY TAG THE PRODUCTION SWITCH ACTUALLY RETURNS, asked of moduleKindTag()
// itself rather than of the list above - so the list cannot silently fall out
// of step with the code it is meant to cover.
void testEveryTagTheSwitchReturnsFits() {
    ImFont* uf = cascade::gui::fonts::ui();
    const float px = cascade::gui::fonts::kTinySize;
    const float chip = cascade::gui::moduleKindTagWidth();

    // The states that produce the two "not known" tags: a catalogue row nobody
    // has fitted, and a fitted file the host refused.
    cascade::gui::ModulePlate p;
    p.haveCapabilities = false;
    p.fitted = false;
    p.loaded = false;
    CHECK(chip >= textW(uf, px, cascade::gui::moduleKindTag(p)) + 8.0f);
    p.fitted = true;
    CHECK(chip >= textW(uf, px, cascade::gui::moduleKindTag(p)) + 8.0f);

    // ...and every capability bit, one at a time, which walks the rest of the
    // switch without this test having to know which word each one produces.
    p.haveCapabilities = true;
    for (int bit = 0; bit < 32; ++bit) {
        p.capabilities = 1u << static_cast<unsigned>(bit);
        CHECK(chip >= textW(uf, px, cascade::gui::moduleKindTag(p)) + 8.0f);
    }
}

// --- the whole thing scales ----------------------------------------------------
//
// The reason every figure above is a function and not a constant: a face's
// height is the size it is asked for, so a box derived from a measurement
// tracks a size change and a box typed as a literal does not. This is the
// property the next person raising fonts.hpp is relying on.
void testMeasurementsTrackTheDeclaredSizes() {
    ImFont* rf = cascade::gui::fonts::reading();
    // A cell for a figure is wider than the figure at the size the counter is
    // actually lettered at - not at some other size that happened to be true
    // when the number was typed.
    const float atDeclared = textW(rf, cascade::gui::fonts::kReadingSize, "0");
    const float atOldSize = textW(rf, cascade::gui::fonts::kReadingSize - 2.0f, "0");
    // The face genuinely measures wider at the larger size, so this test is
    // measuring something. Without this check the two assertions above would
    // pass against a fallback bitmap face that ignores the size argument.
    CHECK(atDeclared > atOldSize);
    CHECK(cascade::gui::coordCellWidth('0') >= atDeclared + 2.0f);
}

// --- the tuning deck's UP / DN stencils, in every language ----------------------
//
// Each stencil stands in a cell one tube wide (tune_control.hpp, kFreqCellW) at
// the deck's natural scale, lettered at nine pixels - already the floor - and
// tracked out. drawToggleSwitch gives up the tracking before it cuts a word
// (text_fit.hpp, fitCellWords); a word that still does not fit is cut, and a
// cut stencil is a broken one - the Russian "ВВЕРХ" was, on every screen
// (34-language review). So every catalogue's pair must fit the cell whole.
void testUpDnStencilsFitTheirCell() {
    std::printf("  every language: the UP / DN stencils fit their tube's cell whole\n");
    int cut = 0;
    int skipped = 0;
    for (const cascade::i18n::Language& l : cascade::i18n::languages()) {
        if (!cascade::gui::fonts::canDraw(l.code)) {
            ++skipped;
            continue;
        }
        cascade::i18n::setLanguage(l.code);
        cascade::gui::fonts::applyLanguage(l.code);
        cascade::gui::fonts::applyPending();
        ImFont* f = cascade::gui::fonts::ui();
        const char* words[2] = {cascade::i18n::tr("UP"), cascade::i18n::tr("DN")};
        const float cell = cascade::gui::kFreqCellW;
        const cascade::gui::CellWordsFit fit = cascade::gui::fitCellWords(
            9.0f, 0.15f, cell, cascade::gui::kFitFloorPx, 2, [&](int i, float z, float tf) {
                return cascade::gui::trackedWidth(f, z, words[i], z * tf);
            });
        if (!fit.fits) {
            std::printf("      %s: \"%s\" %.1f px / \"%s\" %.1f px untracked at %.1f px, the cell is %.0f\n",
                        l.code.c_str(), words[0], textW(f, fit.px, words[0]), words[1],
                        textW(f, fit.px, words[1]), fit.px, cell);
            ++cut;
        }
    }
    cascade::i18n::setLanguage("en");
    cascade::gui::fonts::applyLanguage("en");
    cascade::gui::fonts::applyPending();
    std::printf("    %d languages cut a stencil, %d not drawable here\n", cut, skipped);
    CHECK(cut == 0);
}

}  // namespace

int main() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1600.0f, 1000.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    // 1.92 lets the backend own texture uploads; saying so is what makes a
    // context with no renderer behind it legal for a frame.
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

    // THE LOAD ITSELF IS THE FIRST ASSERTION. fonts::load() degrades to ImGui's
    // own bitmap face rather than crashing, which is right for the application
    // and useless here: every figure below would then be measured against a
    // typeface the product never draws, and would pass.
    const bool loaded = cascade::gui::fonts::load();
    CHECK(loaded);

    if (loaded) {
        // Measuring needs a frame in flight, because that is when the atlas
        // will bake a size it has not seen.
        ImGui::NewFrame();
        testCoordApertureHoldsItsFigure();
        testKindTagChipHoldsEveryTag();
        testEveryTagTheSwitchReturnsFits();
        testMeasurementsTrackTheDeclaredSizes();
        ImGui::Render();
        // Outside the frame: a language change swaps the atlas's faces.
        testUpDnStencilsFitTheirCell();
    } else {
        std::printf("fonts::load() failed - the measurements below were not run\n");
    }

    ImGui::DestroyContext();
    return testSummary("test_bench_text_fits");
}
