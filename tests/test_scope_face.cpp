// Tests for gui/scope_face.hpp - the bench primitives every panel in this
// application is drawn from, rendered headless through a real Dear ImGui
// context and inspected AS DRAW DATA.
//
// WHY THIS FILE EXISTS, AND WHY THE ASSERTIONS ARE SHAPED LIKE THIS. The sizes
// in fonts.hpp went up by two points on a report that captions were hard to
// read. Every width and height in this interface was measured against the old
// numbers, and Dear ImGui does not wrap and does not shrink: a word wider than
// the plate it names is drawn straight across whatever is beside it, silently,
// and a legend sitting over its neighbour looks like a design choice rather
// than a fault. Nothing in the pure half of scope_view.hpp can see that, and
// no unit test of a return value can either - it is only visible in the
// geometry, which is what these read.
//
// TWO PROPERTIES ARE PINNED, and they are the two ways this goes wrong:
//
//   IT FITS. Every vertex a primitive emits stays inside the box it was handed.
//   That is the whole claim: a caption too wide for its furniture is drawn
//   SMALLER, never over the top of the thing next to it. Checked down to box
//   widths well below anything the application asks for today, because the
//   point is the next change to fonts.hpp and not this one.
//
//   IT DOES NOT DEPEND ON THE AMBIENT FACE. drawBenchMeter and drawScopeGauge
//   letter themselves at fonts::kTinySize in faces they push by hand, and both
//   used to reserve room for that text with ImGui::GetTextLineHeight() - the
//   size the CALLER happened to leave bound, which is the application's
//   ordinary UI face and is not what is drawn on a meter. So the same meter in
//   the same box came out a different shape depending on what was pushed
//   around it, and every point the UI face went up took two off the meter's own
//   face. Drawing each one twice under two very different ambient sizes and
//   requiring the identical geometry is the direct pin for that: it is the
//   classic way "measure your own text" goes wrong, and it is invisible until
//   somebody changes a font.
//
// AND A THIRD GROUP, ADDED LATER: WHAT THE FACE CLAIMS IN WORDS. Three
// readouts on this instrument said more than the code behind them knew - a
// heading that promised "in range" over a figure the SYS filter had also cut,
// a registry row that named an absent plugin when its predicate only proved
// nothing was instantiated, and two counters wearing the word TRACKS over two
// different numbers. None of those is a geometry fault and none of them is
// visible in a vertex buffer, so they are pinned through the pure half of
// scope_view.hpp, which is where every string on this face is decided.
//
// A NOTE ON TWO HELPERS BELOW, because the pattern is worth not repeating:
// checkTextClearsTheEdge and checkWholeWordDrawn RETURN SILENTLY when their
// precondition does not hold - no letters emitted, or a caption whose floor
// size cannot fit, where truncation is the documented answer. Both are
// legitimate skips, but a skip that says nothing is indistinguishable from a
// check that passed, and a whole loop of them would report a clean run having
// asserted nothing at all. They are counted and reported at the end of the run
// instead of being left invisible, and nothing added since is written that way:
// lineAt below answers out of range with an empty row that fails its own
// assertion rather than with a silent return.
//
// No GL and no window - ImGui builds its vertex buffers on the CPU, and that
// is what is measured.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/scope_face.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "gui/fonts.hpp"
#include "gui/scope_view.hpp"
#include "imgui.h"
#include "test_check.hpp"

namespace {

// Everything ONE call drew, and nothing else's: every vertex it emitted, and
// the extent of them.
//
// THE WHOLE VERTEX STREAM IS KEPT, NOT JUST THE EXTENT, and that is not
// thoroughness for its own sake - an extent alone cannot see this fault. Most
// of these primitives lay a background over the whole box they are given, so
// the extent is that box whatever happens to the text and the scale inside it.
// The first version of this file compared extents, and it passed cleanly
// against a drawScopeGauge deliberately put back to reserving the ambient line
// height: the bay pinned the bounding box and the segments moved inside it,
// unseen. Comparing the vertices themselves is what makes the check able to
// fail.
struct Box {
    std::vector<ImVec2> pos;
    int verts = 0;
    // JUST THE LETTERS. A glyph is textured from the font atlas and a filled
    // shape from the one white pixel in it, so the UV separates the two without
    // guessing - which is what lets "every letter is still drawn" be counted
    // without the surrounding rule, bevel or bay being counted with it. A rule
    // that shortens because its caption grew is not a missing letter.
    int textVerts = 0;
    float x0 = 0.0f;
    float y0 = 0.0f;
    float x1 = 0.0f;
    float y1 = 0.0f;
    // The letters' own extent, which is how a caption that was FITTED is told
    // from one that was merely CUT. A centred line trimmed at the edge of its
    // box still emits the part of each glyph that is inside, so the vertex
    // count cannot see it - but the trimmed text then runs exactly to the
    // edge, while a fitted one stops at its stated padding short of it.
    float tx0 = 0.0f;
    float tx1 = 0.0f;
    int textFirst = 0;
    bool bad = false;  // a NaN or an infinity anywhere in the range
};

Box extentFrom(const ImDrawList* dl, int first) {
    Box b;
    b.pos.reserve(static_cast<std::size_t>(dl->VtxBuffer.Size - first));
    const ImVec2 white = ImGui::GetFontTexUvWhitePixel();
    for (int i = first; i < dl->VtxBuffer.Size; ++i) {
        const ImDrawVert& v = dl->VtxBuffer[i];
        if (!std::isfinite(v.pos.x) || !std::isfinite(v.pos.y)) {
            b.bad = true;
            continue;
        }
        if (v.uv.x != white.x || v.uv.y != white.y) {
            if (b.textVerts == 0) {
                b.tx0 = v.pos.x;
                b.tx1 = v.pos.x;
            } else {
                b.tx0 = (v.pos.x < b.tx0) ? v.pos.x : b.tx0;
                b.tx1 = (v.pos.x > b.tx1) ? v.pos.x : b.tx1;
            }
            ++b.textVerts;
        }
        if (b.verts == 0) {
            b.x0 = v.pos.x;
            b.x1 = v.pos.x;
            b.y0 = v.pos.y;
            b.y1 = v.pos.y;
        } else {
            b.x0 = (v.pos.x < b.x0) ? v.pos.x : b.x0;
            b.x1 = (v.pos.x > b.x1) ? v.pos.x : b.x1;
            b.y0 = (v.pos.y < b.y0) ? v.pos.y : b.y0;
            b.y1 = (v.pos.y > b.y1) ? v.pos.y : b.y1;
        }
        b.pos.push_back(v.pos);
        ++b.verts;
    }
    return b;
}

// One frame, one window, one call - and the extent of exactly what that call
// drew.
//
// THE WINDOW IS DELIBERATELY MUCH BIGGER THAN ANYTHING DRAWN IN IT. ImGui culls
// glyphs against the current clip rectangle as it emits them, so a caption
// overflowing its plate inside a tight window would be trimmed by the window
// and the overflow would never reach the vertex buffer - the test would pass by
// being unable to see the fault. Everything below is drawn in the middle of a
// window with hundreds of pixels of slack on every side, so an overflowing
// glyph IS emitted and IS measured.
template <class F>
Box drawOne(F&& fn) {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(1600.0f, 1000.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("bench", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // AND THE LINES ARE TAKEN OFF THE FONT TEXTURE FIRST. ImGui draws an
    // anti-aliased line through a strip of the atlas, so a rule or a bevel
    // arrives carrying an atlas UV exactly like a glyph does - and the glyph
    // count below would then be counting hairlines. This cost an hour: four
    // "letters were cut" failures that were a group caption's RULE being
    // dropped on a narrow column, which is the primitive doing what its header
    // says. Cleared per frame, because the window's draw list is rebuilt with
    // the style's flags each time.
    dl->Flags &= ~ImDrawListFlags_AntiAliasedLinesUseTex;
    const int mark = dl->VtxBuffer.Size;
    fn(dl);
    const Box b = extentFrom(dl, mark);
    ImGui::End();
    ImGui::Render();
    return b;
}

// Half a pixel of anti-aliasing on a filled edge, and a pixel of the shadow
// pass a cut letter sits over. A caption that does not fit overflows by tens of
// pixels, so this tolerance cannot hide the fault it is looking for.
constexpr float kEdgeSlack = 2.0f;

void checkInsideX(const char* what, const Box& b, float left, float right, float w) {
    const bool ok = b.verts > 0 && !b.bad && b.x0 >= left - kEdgeSlack &&
                    b.x1 <= right + kEdgeSlack;
    CHECK(ok);
    if (!ok) {
        std::printf("      %s at width %.0f: drew x %.1f..%.1f, box %.1f..%.1f "
                    "(%d verts%s)\n",
                    what, static_cast<double>(w), static_cast<double>(b.x0),
                    static_cast<double>(b.x1), static_cast<double>(left),
                    static_cast<double>(right), b.verts, b.bad ? ", NON-FINITE" : "");
    }
}

// --- it fits ------------------------------------------------------------------
//
// TWO HALVES, AND THE SECOND IS THE ONE WITH TEETH. "Nothing was drawn outside
// the box" is satisfied by cutting the caption off at the edge, so on its own
// it cannot tell a primitive that FITS its text from one that merely TRUNCATES
// it. The rule the panel is built on is: shrink until it fits, and only cut
// what cannot be drawn at the nine-pixel floor at all. So wherever the floor
// would fit, every glyph must still be there - counted as vertices against the
// same call in a box with room to spare, since one glyph is one quad whatever
// size it is set at.

// An independent statement of the tracking rule, from the header: the glyphs'
// own advances plus the tracking between each pair.
float trackedWidthAt(ImFont* font, float px, const char* text, float trackFrac) {
    float w = 0.0f;
    int glyphs = 0;
    for (const char* p = text; *p != '\0'; ++p) {
        const char one[2] = {*p, '\0'};
        w += font->CalcTextSizeA(px, FLT_MAX, 0.0f, one).x;
        ++glyphs;
    }
    if (glyphs > 1) { w += px * trackFrac * static_cast<float>(glyphs - 1); }
    return w;
}

// The floor the fit stops at, from the header of fitTextPx, and a pixel of
// margin on top of it: a glyph's advance is rounded to the pixel at the size it
// is rasterised for, so a caption whose floor width lands exactly on its room
// is a coin toss rather than a rule, and a test may only demand the rule.
constexpr float kFloorPx = 9.0f;
constexpr float kFloorMargin = 2.0f;

// The other half of the same claim, for the lines that are CENTRED on their
// box. A cut lands exactly on the edge; a fit stops short of it by the padding
// the primitive states. Anything at or past the edge was cut.
// How many times a check below declined to check anything. See the note at the
// top of the file: both of the two helpers that follow have a legitimate reason
// to stand down, and neither may do it quietly - a run of nothing but skips
// would otherwise print the same clean summary as a run that verified every
// case.
int g_skipped = 0;

void checkTextClearsTheEdge(const char* what, const Box& b, float left, float right,
                            float clear, float w) {
    if (b.textVerts == 0) {
        ++g_skipped;
        return;
    }
    const bool ok = b.tx0 >= left + clear && b.tx1 <= right - clear;
    CHECK(ok);
    if (!ok) {
        std::printf("      %s at width %.0f: letters run %.1f..%.1f in a box of "
                    "%.1f..%.1f - fitted text stops %.1f px short of both\n",
                    what, static_cast<double>(w), static_cast<double>(b.tx0),
                    static_cast<double>(b.tx1), static_cast<double>(left),
                    static_cast<double>(right), static_cast<double>(clear));
    }
}

void checkWholeWordDrawn(const char* what, const Box& roomy, const Box& tight,
                         float w, bool floorFits) {
    if (!floorFits) {
        ++g_skipped;  // truncation is the documented answer here
        return;
    }
    const bool ok = tight.textVerts == roomy.textVerts && roomy.textVerts > 0;
    CHECK(ok);
    if (!ok) {
        std::printf("      %s at width %.0f: %d glyph vertices, %d with room to spare - "
                    "letters were cut rather than the caption fitted\n",
                    what, static_cast<double>(w), tight.textVerts, roomy.textVerts);
    }
}

// The plate's title is CENTRED on the plate, so one too wide for its ground is
// not clipped - it is drawn out over the bevel and into the panel beside it.
// Tracked capitals are the widest thing this file draws: the spacing that makes
// them read as engraving adds a fifth of the size between every pair of
// letters, so FUNCTION SELECT on a narrow rail is where a font bump lands
// first.
void testPlateTitleStaysOnItsPlate() {
    const char* titles[] = {"FUNCTION SELECT", "PLUGIN STORE", "STATUS", "SATELLITES MAP"};
    const float widths[] = {340.0f, 260.0f, 200.0f, 160.0f, 130.0f, 110.0f, 90.0f, 70.0f};
    ImFont* f = cascade::gui::fonts::legend();
    for (const char* title : titles) {
        const ImVec2 tl(300.0f, 200.0f);
        const Box roomy = drawOne([&](ImDrawList* dl) {
            cascade::gui::addBenchPlate(dl, tl, ImVec2(tl.x + 900.0f, tl.y + 140.0f),
                                        title);
        });
        for (const float w : widths) {
            const ImVec2 br(tl.x + w, tl.y + 140.0f);
            float bodyTop = 0.0f;
            const Box b = drawOne([&](ImDrawList* dl) {
                bodyTop = cascade::gui::addBenchPlate(dl, tl, br, title);
            });
            checkInsideX(title, b, tl.x, br.x, w);
            // The plate's own 8 px inset at both ends is the room a title has.
            checkWholeWordDrawn(title, roomy, b, w,
                                trackedWidthAt(f, kFloorPx, title, 0.20f) + kFloorMargin <= w - 16.0f);
            // The measurement the caller lays out from has to be a real one:
            // below the title it just drew and inside the plate it drew on.
            CHECK(bodyTop > tl.y);
            CHECK(bodyTop < br.y);
        }
    }
}

// The same for a group caption, which is fitted to the width its rule is
// carried out to.
void testGroupCaptionStaysInItsWidth() {
    const char* captions[] = {"SIGNAL PATH", "DECODE", "RECEIVER AND ANTENNA"};
    const float widths[] = {240.0f, 180.0f, 140.0f, 110.0f, 80.0f, 60.0f};
    ImFont* f = cascade::gui::fonts::legend();
    const ImVec2 at(300.0f, 200.0f);
    for (const char* caption : captions) {
        const Box roomy = drawOne([&](ImDrawList* dl) {
            cascade::gui::addBenchGroupCaption(dl, at, 900.0f, caption);
        });
        for (const float w : widths) {
            const Box b = drawOne([&](ImDrawList* dl) {
                cascade::gui::addBenchGroupCaption(dl, at, w, caption);
            });
            checkInsideX(caption, b, at.x, at.x + w, w);
            checkWholeWordDrawn(caption, roomy, b, w,
                                trackedWidthAt(f, kFloorPx, caption, 0.24f) + kFloorMargin <= w - 8.0f);
        }
    }
}

// The gauge letters its name above the bar and its reading below, both centred
// on a bay 62 px wide at full scale and narrower on a small window. NO ALT is
// the long one.
void testGaugeTextStaysInItsBay() {
    // Down to widths well under anything the deck asks for: kGaugeW is 62 px
    // at full scale and shrinks with a narrow window, and the primitive's own
    // guard lets it be called at 8.
    const float widths[] = {80.0f, 62.0f, 50.0f, 42.0f, 34.0f, 26.0f, 18.0f, 12.0f};
    const ImVec2 tl(300.0f, 200.0f);
    const Box roomy = drawOne([&](ImDrawList* dl) {
        cascade::gui::drawScopeGauge(dl, tl, ImVec2(tl.x + 400.0f, tl.y + 260.0f), "ALT",
                                     0.62, true, "NO ALT");
    });
    ImFont* lf = cascade::gui::fonts::legend();
    ImFont* rf = cascade::gui::fonts::ui();
    for (const float w : widths) {
        const ImVec2 br(tl.x + w, tl.y + 260.0f);
        const Box b = drawOne([&](ImDrawList* dl) {
            cascade::gui::drawScopeGauge(dl, tl, br, "ALT", 0.62, true, "NO ALT");
        });
        checkInsideX("gauge", b, tl.x, br.x, w);
        const float room = w - 6.0f;
        const bool floorFits =
            lf->CalcTextSizeA(kFloorPx, FLT_MAX, 0.0f, "ALT").x + kFloorMargin <= room &&
            rf->CalcTextSizeA(kFloorPx, FLT_MAX, 0.0f, "NO ALT").x + kFloorMargin <= room;
        checkWholeWordDrawn("gauge", roomy, b, w, floorFits);
        // Both lines are fitted to the bay less 3 px at each end, so where the
        // floor fits they must clear the bay's edges rather than sit on them.
        if (floorFits) { checkTextClearsTheEdge("gauge", b, tl.x, br.x, 2.0f, w); }
        // With no reading the readout is a pair of dashes, and the bay is the
        // same box: the scale is drawn and no bar, which is the whole reason
        // this primitive takes haveReading at all.
        const Box none = drawOne([&](ImDrawList* dl) {
            cascade::gui::drawScopeGauge(dl, tl, br, "ALT", 0.62, false, "NO ALT");
        });
        checkInsideX("gauge (no reading)", none, tl.x, br.x, w);
    }
}

// The counter's caption is the only thing separating SET RANGE NM from TGT
// RANGE NM - two different distances printed in the same three figures - so it
// may not be allowed to run into the counter standing beside it.
void testDrumCaptionStaysOverItsCounter() {
    const char* captions[] = {"SET RANGE NM", "TGT RANGE NM", "TRACKS"};
    const float cells[] = {30.0f, 26.0f, 20.0f, 14.0f};
    const ImVec2 tl(300.0f, 200.0f);
    for (const char* caption : captions) {
        // The same three drums with room to spare - wide cells, not more of
        // them, so the digits drawn are the same three and the caption is the
        // only thing that can differ.
        const Box roomy = drawOne([&](ImDrawList* dl) {
            cascade::gui::drawScopeDrums(dl, tl, 70.0f, 100.0f, 3, 240, caption);
        });
        for (const float cellW : cells) {
            const float groupW = 3.0f * cellW + 2.0f * 3.0f;
            Box b;
            float floorW = 0.0f;
            b = drawOne([&](ImDrawList* dl) {
                floorW = ImGui::GetFont()
                             ->CalcTextSizeA(kFloorPx, FLT_MAX, 0.0f, caption)
                             .x;
                cascade::gui::drawScopeDrums(dl, tl, cellW, cellW * 1.47f, 3, 240,
                                             caption);
            });
            checkInsideX(caption, b, tl.x, tl.x + groupW, cellW);
            checkWholeWordDrawn(caption, roomy, b, cellW, floorW + kFloorMargin <= groupW);
        }
    }
}

// The meter's two lines are centred on its own width, so the same rule applies
// to the widest value line the application prints through it.
void testMeterTextStaysOnItsFace() {
    const float widths[] = {160.0f, 126.0f, 100.0f, 80.0f, 60.0f, 44.0f};
    const ImVec2 tl(300.0f, 200.0f);
    ImFont* cf = cascade::gui::fonts::legend();
    ImFont* vf = cascade::gui::fonts::ui();
    // No unit label in the comparison shots: it is drawn only when it fits
    // beside the pivot, which is a rule of its own and would move the glyph
    // count for a reason that has nothing to do with the two centred lines.
    const Box roomy = drawOne([&](ImDrawList* dl) {
        cascade::gui::drawBenchMeter(dl, tl, 400.0f, 130.0f, "SAMPLE RATE", 0.42f, true,
                                     "22 % - 3.6 ms");
    });
    for (const float w : widths) {
        const Box b = drawOne([&](ImDrawList* dl) {
            cascade::gui::drawBenchMeter(dl, tl, w, 130.0f, "SAMPLE RATE", 0.42f, true,
                                         "22 % - 3.6 ms");
        });
        checkInsideX("meter", b, tl.x, tl.x + w, w);
        const float room = w - 4.0f;
        const bool floorFits =
            cf->CalcTextSizeA(kFloorPx, FLT_MAX, 0.0f, "SAMPLE RATE").x + kFloorMargin <= room &&
            vf->CalcTextSizeA(kFloorPx, FLT_MAX, 0.0f, "22 % - 3.6 ms").x + kFloorMargin <= room;
        checkWholeWordDrawn("meter", roomy, b, w, floorFits);
        // Fitted to the width less 2 px at each end; a line cut at the edge
        // instead would touch it.
        if (floorFits) { checkTextClearsTheEdge("meter", b, tl.x, tl.x + w, 1.0f, w); }
        // And with the unit label, which is what the bar actually draws: the
        // needle's arc and that legend must both stay on the cream.
        const Box withUnit = drawOne([&](ImDrawList* dl) {
            cascade::gui::drawBenchMeter(dl, tl, w, 130.0f, "SAMPLE RATE", 0.42f, true,
                                         "22 % - 3.6 ms", "MS/s");
        });
        checkInsideX("meter (with unit)", withUnit, tl.x, tl.x + w, w);
    }
}

// --- and it does not depend on the face the caller left bound -----------------

// THE PIN FOR THE FAULT THIS SWEEP WAS LOOKING FOR. Both of these letter
// themselves at fonts::kTinySize through faces they push by hand, so the room
// they keep for that text must come from measuring it - not from
// GetTextLineHeight(), which reports whatever the caller has bound. Drawn twice
// in the same box under two ambient sizes four times apart, they must produce
// the identical picture; before the fix the meter's face lost two lines of the
// ambient height and the gauge's bargraph lost the same, so the two runs
// disagreed by tens of pixels.
void checkSameUnderAnyFace(const char* what, const Box& small, const Box& large) {
    bool ok = small.verts == large.verts && small.verts > 0 && !small.bad && !large.bad;
    int moved = 0;
    float worst = 0.0f;
    if (ok) {
        for (std::size_t i = 0; i < small.pos.size(); ++i) {
            const float dx = std::fabs(small.pos[i].x - large.pos[i].x);
            const float dy = std::fabs(small.pos[i].y - large.pos[i].y);
            const float d = (dx > dy) ? dx : dy;
            if (d > 0.01f) {
                ++moved;
                worst = (d > worst) ? d : worst;
            }
        }
        ok = (moved == 0);
    }
    CHECK(ok);
    if (!ok) {
        std::printf("      %s moved with the ambient face: %d of %d vertices, worst "
                    "%.1f px (%d verts vs %d)\n",
                    what, moved, small.verts, static_cast<double>(worst), small.verts,
                    large.verts);
    }
}

Box meterUnderFace(float ambientPx) {
    return drawOne([&](ImDrawList* dl) {
        ImGui::PushFont(cascade::gui::fonts::ui(), ambientPx);
        cascade::gui::drawBenchMeter(dl, ImVec2(300.0f, 200.0f), 126.0f, 120.0f,
                                     "SAMPLE RATE", 0.42f, true, "2.000 MS/s", "MS/s");
        ImGui::PopFont();
    });
}

Box gaugeUnderFace(float ambientPx) {
    return drawOne([&](ImDrawList* dl) {
        ImGui::PushFont(cascade::gui::fonts::ui(), ambientPx);
        cascade::gui::drawScopeGauge(dl, ImVec2(300.0f, 200.0f), ImVec2(362.0f, 460.0f),
                                     "SIG", 0.4, true, "-42");
        ImGui::PopFont();
    });
}

void testMeterDoesNotMoveWithTheAmbientFace() {
    const Box small = meterUnderFace(10.0f);
    const Box large = meterUnderFace(40.0f);
    CHECK(small.verts > 0);
    checkSameUnderAnyFace("the meter", small, large);
}

void testGaugeDoesNotMoveWithTheAmbientFace() {
    const Box small = gaugeUnderFace(10.0f);
    const Box large = gaugeUnderFace(40.0f);
    CHECK(small.verts > 0);
    checkSameUnderAnyFace("the gauge", small, large);
}

// AND THE METER IS STILL DRAWN AT A HEIGHT A CALLER ACTUALLY ASKS FOR. The
// height the application passes is the reference's 66 px face plus room for the
// two lines; the failure mode this file is here to catch ends with the meter
// returning early and drawing nothing at all while the figures beside it carry
// on updating, so the floor is checked rather than assumed.
void testMeterDrawsAtTheHeightTheBarAsksFor() {
    for (float ambient = 10.0f; ambient <= 40.0f; ambient += 6.0f) {
        const float px = ambient;
        const Box b = drawOne([&](ImDrawList* dl) {
            ImGui::PushFont(cascade::gui::fonts::ui(), px);
            // 66 px of face plus two lines of the ambient face and 8 px, which
            // is the formula the top bar uses.
            const float h = 66.0f + ImGui::GetTextLineHeight() * 2.0f + 8.0f;
            cascade::gui::drawBenchMeter(dl, ImVec2(300.0f, 200.0f), 126.0f, h,
                                         "FRAME TIME", 0.3f, true, "18 % - 3.0 ms", "ms");
            ImGui::PopFont();
        });
        const bool ok = b.verts > 100 && !b.bad;
        CHECK(ok);
        if (!ok) {
            std::printf("      meter drew %d vertices at ambient %.0f px\n", b.verts,
                        static_cast<double>(ambient));
        }
    }
}

// --- and the face itself, at every size it can be given ----------------------
//
// WHAT THIS CAN AND CANNOT CATCH, because a test that implies more than it
// checks is worse than no test. It draws the whole scope - tube, targets,
// labels, rings, bearing ticks, corner readouts and the panel beside them - at
// sizes from the smallest the renderer accepts to a large window, and requires
// that every one of them produces real geometry and not one non-finite
// coordinate. That is a smoke test of the draw path and it is the first one
// this class has had: nothing else in the suite renders ScopeView::draw at all,
// and the label placement, the tick threshold and the corner readouts changed
// in this sweep are all inside it. It does NOT assert where any individual
// readout lands; a NaN in a clip rect or a face that stops drawing at some
// aspect ratio is what it is here to find.
cascade::core::HostTrack makeAircraft(const char* id, const char* label, double lat,
                                      double lon, double altM) {
    cascade::core::HostTrack ht{};
    std::snprintf(ht.t.id, sizeof(ht.t.id), "%s", id);
    std::snprintf(ht.t.label, sizeof(ht.t.label), "%s", label);
    ht.t.kind = CASCADE_TRACK_AIRCRAFT;
    ht.t.latDeg = lat;
    ht.t.lonDeg = lon;
    ht.t.altM = altM;
    ht.t.speedMps = 220.0;
    ht.t.courseDeg = 71.0;
    ht.t.ageMs = 1200;
    return ht;
}

void testScopeDrawsAtEverySize() {
    const float sizes[][2] = {{1600.0f, 950.0f}, {1100.0f, 700.0f}, {700.0f, 700.0f},
                              {520.0f, 300.0f}, {300.0f, 300.0f},   {180.0f, 180.0f},
                              {96.0f, 96.0f},   {64.0f, 64.0f},     {900.0f, 120.0f}};
    std::vector<cascade::core::HostTrack> tracks;
    tracks.push_back(makeAircraft("406A1B", "BAW117", 51.9, -0.4, 10600.0));
    tracks.push_back(makeAircraft("4CA2D4", "", 51.2, 0.6, 2400.0));
    tracks.push_back(makeAircraft("3C6444", "DLH8AK", 52.4, 0.9, 0.0));
    for (const auto& wh : sizes) {
        for (int range = 0; range < 2; ++range) {
            cascade::gui::ScopeView view;
            view.setReceiver(51.5, -0.12);
            view.setRangeNm(range == 0 ? 10 : 400);
            view.setSelected("406A1B");
            const Box b = drawOne([&](ImDrawList*) {
                ImGui::SetCursorScreenPos(ImVec2(0.0f, 0.0f));
                view.draw(wh[0], wh[1], tracks, nullptr, nullptr);
            });
            const bool ok = !b.bad && b.verts > 200;
            CHECK(ok);
            if (!ok) {
                std::printf("      scope at %.0fx%.0f range %d: %d vertices%s\n",
                            static_cast<double>(wh[0]), static_cast<double>(wh[1]),
                            range == 0 ? 10 : 400, b.verts,
                            b.bad ? ", NON-FINITE" : "");
            }
        }
    }
    // With no receiver position there is nothing to draw a range or a bearing
    // from, and the class says so by drawing nothing at all - which must also
    // be a clean nothing rather than a NaN.
    cascade::gui::ScopeView blind;
    const Box none = drawOne([&](ImDrawList*) {
        ImGui::SetCursorScreenPos(ImVec2(0.0f, 0.0f));
        blind.draw(900.0f, 600.0f, tracks, nullptr, nullptr);
    });
    CHECK(!none.bad);
}

// --- and what the face says in words -----------------------------------------

// Bounds-safe row access, and the reason it is not written as a size guard
// around the assertions. This harness's CHECK records and continues, so an
// `if (rows.size() == 4) { ...assertions }` skips every assertion in exactly
// the run that had something to report, while `rows[4]` after a failed CHECK
// reads off the end and takes the process with it. An out-of-range index comes
// back as a default row instead, whose empty label and empty value fail the
// assertion that asked for it - loudly, at the line that cared.
const cascade::gui::ScopeDetailLine& lineAt(
    const std::vector<cascade::gui::ScopeDetailLine>& rows, std::size_t i) {
    static const cascade::gui::ScopeDetailLine kMissing{};
    return (i < rows.size()) ? rows[i] : kMissing;
}

// Written out so the assertions below read as the claim being made about a
// readout rather than as string arithmetic.
bool says(const std::string& text, const char* word) {
    return text.find(word) != std::string::npos;
}

// THE REGISTRY ROWS MAY NOT REPORT AN ABSENT MODULE, because the flag they are
// drawn from cannot see one. `infoActive` is TrackInfoCache::active(): a module
// pointer AND a live instance handle, so it is equally false for a machine with
// no track-info module, one whose module the user stopped, and one whose module
// loaded and failed to give out an instance. The row lettered
// "NO REGISTRY PLUGIN" in all three, which was true in one of them.
//
// RED WHEN the row goes back to naming a plugin, and red when any two of the
// three states collapse into one string - which is the other way this gets
// broken, by a well-meant simplification of a ternary that looks redundant.
void testRegistryRowClaimsOnlyWhatIsKnown() {
    cascade::gui::ScopeDetailInput in;
    in.flight = "BAW123";
    in.hasRx = true;
    in.rangeNm = 42.5;
    in.bearingDeg = 273.4;

    // NOTHING IS LOOKING. All three registry rows carry the same word, and the
    // labels are asserted rather than assumed so a row inserted above them
    // fails here instead of quietly moving what the value checks read.
    in.infoActive = false;
    in.infoPending = false;
    const std::vector<cascade::gui::ScopeDetailLine> idle =
        cascade::gui::buildScopeDetailLines(in);
    CHECK(lineAt(idle, 1).label == "OPERATOR");
    CHECK(lineAt(idle, 2).label == "TYPE");
    CHECK(lineAt(idle, 3).label == "REG");
    const std::string silent = lineAt(idle, 1).value;
    CHECK(silent == "NO LOOKUP RUNNING");
    CHECK(lineAt(idle, 2).value == silent);
    CHECK(lineAt(idle, 3).value == silent);
    // None of these is a measurement, so none of them may be lettered as one.
    CHECK(!lineAt(idle, 1).known);
    CHECK(!lineAt(idle, 2).known);
    CHECK(!lineAt(idle, 3).known);

    // The claim the wording is not allowed to make, in each of the shapes it
    // has worn or could wear again.
    const char* const forbidden[] = {"PLUGIN", "INSTALL", "FITTED", "NO REGISTRY",
                                     "NO MODULE"};
    for (const char* word : forbidden) {
        const bool claimed = says(silent, word);
        CHECK(!claimed);
        if (claimed) {
            std::printf("      the registry row reads \"%s\" - it says \"%s\" about a "
                        "module it can only see the INSTANCE of\n",
                        silent.c_str(), word);
        }
    }

    // ASKED, NOTHING BACK YET.
    in.infoActive = true;
    in.infoPending = true;
    const std::vector<cascade::gui::ScopeDetailLine> pending =
        cascade::gui::buildScopeDetailLines(in);
    const std::string looking = lineAt(pending, 1).value;
    CHECK(looking == "LOOKING UP");
    CHECK(!lineAt(pending, 1).known);

    // ANSWERED, AND THE ANSWER WAS "NOT IN MY DATA".
    in.infoPending = false;
    const std::vector<cascade::gui::ScopeDetailLine> answered =
        cascade::gui::buildScopeDetailLines(in);
    const std::string empty = lineAt(answered, 1).value;
    CHECK(empty == "NO DATA");
    CHECK(!lineAt(answered, 1).known);

    // Three facts, three words. Pairwise, because a collapse of any one pair is
    // the failure and checking only that the set is non-empty would miss two of
    // the three ways it can happen.
    CHECK(silent != looking);
    CHECK(silent != empty);
    CHECK(looking != empty);

    // AND THE WORKING PATH IS UNTOUCHED: a source that answered with an entry
    // still fills the rows, bright, with what it said.
    in.operatorName = "British Airways";
    in.typeName = "Airbus A320";
    in.registration = "G-EUUU";
    const std::vector<cascade::gui::ScopeDetailLine> known =
        cascade::gui::buildScopeDetailLines(in);
    CHECK(lineAt(known, 1).value == "British Airways");
    CHECK(lineAt(known, 1).known);
    CHECK(lineAt(known, 2).value == "Airbus A320");
    CHECK(lineAt(known, 3).value == "G-EUUU");
}

// TWO COUNTERS, TWO NUMBERS, AND THEY MAY NOT SHARE A WORD. The odometer drum
// on the maker's plate is captioned TRACKS and counts every aircraft the host
// holds - no age test, no range test, no filter - while the readout in the
// corner of the tube counts the silhouettes actually drawn. Both said TRACKS,
// so the plate and the glass printed different figures under one caption and
// nothing on the instrument said which question either was answering.
//
// The drum captions are the ones this file already draws drums with, above.
// RED WHEN the corner readout is put back to the plate's word.
void testCornerCountDoesNotWearThePlateCaption() {
    const std::string corner = cascade::gui::scopeTracksReadout(4);
    CHECK(corner == "4 PLOTTED");
    const char* const drumCaptions[] = {"SET RANGE NM", "TGT RANGE NM", "TRACKS"};
    for (const char* caption : drumCaptions) {
        const bool clash = says(corner, caption);
        CHECK(!clash);
        if (clash) {
            std::printf("      the corner readout \"%s\" wears the drum caption "
                        "\"%s\" - one word over two different counts\n",
                        corner.c_str(), caption);
        }
    }
    // AND THE FIELD KEEPS ITS SHAPE. A legend on an instrument face does not
    // conjugate as the last aircraft leaves, and a negative count - which the
    // draw loop cannot produce - is floored rather than printed.
    CHECK(cascade::gui::scopeTracksReadout(0) == "0 PLOTTED");
    CHECK(cascade::gui::scopeTracksReadout(1) == "1 PLOTTED");
    CHECK(cascade::gui::scopeTracksReadout(12) == "12 PLOTTED");
    CHECK(cascade::gui::scopeTracksReadout(-4) == "0 PLOTTED");
}

// AND THE FIGURE THE TWO NEW CAPTIONS NAME IS THE GATED ONE. Both the corner
// readout and the panel's big count print plottedCount(), which is incremented
// only after the kind test and the range test, so it is the number of marks on
// the glass and not the number of aircraft the host is holding. That is the
// whole reason the captions had to change rather than the numbers, and it is
// the property that makes the new wording true.
//
// Ranges from one place, so the arithmetic is a latitude difference and needs
// no trigonometry to state: one degree of latitude is 60 nautical miles, so the
// three aircraft below sit at about 3, 120 and 600 NM from the receiver.
//
// RED WHEN the range gate leaves the draw loop, and red when the kind gate
// does - a vessel parked on the receiver's own coordinates is inside every
// range on the ladder and must still never be counted.
void testPlottedCountIsTheGatedFigure() {
    std::vector<cascade::core::HostTrack> tracks;
    tracks.push_back(makeAircraft("406A1B", "BAW117", 51.55, -0.12, 10600.0));
    tracks.push_back(makeAircraft("4CA2D4", "EZY42", 53.50, -0.12, 9500.0));
    tracks.push_back(makeAircraft("3C6444", "DLH8AK", 61.50, -0.12, 11000.0));
    cascade::core::HostTrack ship = makeAircraft("2320811", "SEAWAY", 51.50, -0.12, 0.0);
    ship.t.kind = CASCADE_TRACK_VESSEL;
    tracks.push_back(ship);
    // The guard against a vacuous pass: every count below is a count OF this
    // vector, and an empty one would satisfy the gates without exercising them.
    CHECK(tracks.size() == 4u);

    // Range on the ladder, and how many of the four belong on the face at it.
    const int expected[][2] = {{10, 1}, {200, 2}, {800, 3}};
    for (const auto& step : expected) {
        cascade::gui::ScopeView view;
        view.setReceiver(51.5, -0.12);
        view.setRangeNm(step[0]);
        CHECK(view.rangeNm() == step[0]);  // a ladder value, so nothing snapped
        const Box b = drawOne([&](ImDrawList*) {
            ImGui::SetCursorScreenPos(ImVec2(0.0f, 0.0f));
            view.draw(900.0f, 600.0f, tracks, nullptr, nullptr);
        });
        CHECK(!b.bad);
        const bool ok = view.plottedCount() == step[1];
        CHECK(ok);
        if (!ok) {
            std::printf("      at %d NM the face plotted %d of 4 tracks, expected %d - "
                        "the count the panel heads \"CONTACTS PLOTTED\" is no longer "
                        "the gated one\n",
                        step[0], view.plottedCount(), step[1]);
        }
    }
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
    // The real typefaces, at the real sizes, because the whole subject here is
    // how wide a caption comes out. A failure to load is not a failure of these
    // tests - every face falls back to the one that is bound and the fitting
    // arithmetic is the same - so it is reported and the run continues.
    if (!cascade::gui::fonts::load()) {
        std::printf("  note: font atlas refused a face; measuring the fallback\n");
    }

    testPlateTitleStaysOnItsPlate();
    testGroupCaptionStaysInItsWidth();
    testGaugeTextStaysInItsBay();
    testDrumCaptionStaysOverItsCounter();
    testMeterTextStaysOnItsFace();
    testMeterDoesNotMoveWithTheAmbientFace();
    testGaugeDoesNotMoveWithTheAmbientFace();
    testMeterDrawsAtTheHeightTheBarAsksFor();
    testScopeDrawsAtEverySize();
    testRegistryRowClaimsOnlyWhatIsKnown();
    testCornerCountDoesNotWearThePlateCaption();
    testPlottedCountIsTheGatedFigure();

    ImGui::DestroyContext();
    // The skips, out loud. A clean summary over a run that declined every check
    // it was asked to make would be the same summary as a run that made them
    // all, and this is the only line separating the two.
    std::printf("test_scope_face: %d checks stood down (nothing to check)\n", g_skipped);
    return testSummary("test_scope_face");
}
