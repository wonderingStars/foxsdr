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
void checkTextClearsTheEdge(const char* what, const Box& b, float left, float right,
                            float clear, float w) {
    if (b.textVerts == 0) { return; }
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
    if (!floorFits) { return; }  // truncation is the documented answer here
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

    ImGui::DestroyContext();
    return testSummary("test_scope_face");
}
