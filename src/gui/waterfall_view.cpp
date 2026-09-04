// Scrolling waterfall widget — implementation.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/waterfall_view.hpp"

#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>

#include <imgui.h>

#include "gui/fonts.hpp"
#include "gui/scope_face.hpp"
#include "gui/theme.hpp"

// glfw3.h pulls in GL/gl.h on Windows. Everything used here is OpenGL 1.1
// (glGenTextures / glTexImage2D / glTexSubImage2D / ...), which links straight
// from opengl32.lib — no loader, no GLEW, and no conflict with the ImGui
// opengl3 backend because this file never includes that backend's header.
#include <GLFW/glfw3.h>

// The GL upload below hands IM_COL32-packed pixels to glTexImage2D as
// GL_RGBA / GL_UNSIGNED_BYTE, which is only correct while ImU32 stores bytes
// in R,G,B,A memory order (ImGui's default packing). GL 1.1 has no BGRA
// format to fall back on, so fail the build loudly rather than render with
// red and blue swapped.
#ifdef IMGUI_USE_BGRA_PACKED_COLOR
#error "waterfall_view.cpp assumes ImGui's default RGBA packing (IM_COL32_R_SHIFT == 0)"
#endif

namespace cascade::gui {

namespace {

// Perceived brightness (ITU-R BT.601 luma weights) — the quantity the
// colormap keeps monotonically non-decreasing so louder always reads
// brighter, even on the yellow -> red stretch.
float lumaOf(int r, int g, int b) {
    return 0.299f * static_cast<float>(r) + 0.587f * static_cast<float>(g) +
           0.114f * static_cast<float>(b);
}

// Colormap anchor stops - PHOSPHOR, and the same tube the spectrum and the
// radar scope are drawn on.
//
// THE PROPERTY THIS TABLE EXISTS FOR IS UNCHANGED and is not a matter of
// taste: the anchor lumas strictly increase (14.7 -> 48.1 -> 98.0 -> 169.1 ->
// 230.2), so channel-wise linear interpolation gives a monotone ramp before
// rounding, and the sweep below fixes up the rounding. A waterfall is a
// MEASUREMENT: a stronger signal must never render darker than a weaker one,
// whatever colours it is made of. The blue-to-red table this replaced had the
// same property (4.6 -> 41.7 -> 105.1 -> 141.9 -> 145.2) and it was kept.
//
// THE FLOOR IS DELIBERATELY LIFTED off the reference's own value. The 1960s
// design starts its ramp at #050A06, which is within a couple of luma units of
// the panel it is painted on - and a signal a few dB above the noise floor
// would simply not be visible. This table starts at luma 14.7 instead: still
// unmistakably "nothing there", still darker than any real signal, but far
// enough off the ground that the bottom of the range keeps its resolution.
// The old table's first segment spanned 4.6 -> 41.7 and this one spans 14.7 ->
// 48.1, so the usable contrast at the quiet end is preserved rather than
// merely claimed.
//
// The top stop is a warm cream rather than white: it has to be the brightest
// entry in the table, and a pure white would leave nothing above it for the
// eye to read a peak against.
struct Rgb {
    float r, g, b;
};
constexpr float kStopPos[5] = {0.0f, 0.20f, 0.45f, 0.75f, 1.0f};
constexpr Rgb kStopRgb[5] = {
    {6.0f, 20.0f, 10.0f},      // quiet phosphor (exact at norm 0)
    {10.0f, 70.0f, 35.0f},     // green
    {30.0f, 140.0f, 60.0f},    // phosphor
    {150.0f, 200.0f, 60.0f},   // yellow-green
    {240.0f, 235.0f, 180.0f},  // cream anchor (exact at norm 1)
};

const std::array<ImU32, 256>& colorLut() {
    // Magic static: built once, thread-safe, no static-init-order hazards.
    static const std::array<ImU32, 256> lut = [] {
        std::array<int, 256> r{};
        std::array<int, 256> g{};
        std::array<int, 256> b{};
        for (int i = 0; i < 256; ++i) {
            const float t = static_cast<float>(i) / 255.0f;
            int seg = 3;
            for (int s = 0; s < 4; ++s) {
                if (t <= kStopPos[s + 1]) {
                    seg = s;
                    break;
                }
            }
            const float u = (t - kStopPos[seg]) / (kStopPos[seg + 1] - kStopPos[seg]);
            const Rgb& a = kStopRgb[seg];
            const Rgb& c = kStopRgb[seg + 1];
            r[i] = static_cast<int>(a.r + (c.r - a.r) * u + 0.5f);
            g[i] = static_cast<int>(a.g + (c.g - a.g) * u + 0.5f);
            b[i] = static_cast<int>(a.b + (c.b - a.b) * u + 0.5f);
        }
        // Rounding each channel independently can dip luma by up to ~1 unit
        // between neighbors on shallow segments. Sweep backward from the
        // fixed red anchor, darkening any entry brighter than its hotter
        // neighbor. Green goes first: it has the largest luma weight (fewest
        // steps) and lowering G near the top is exactly the natural
        // green -> yellow -> cream hue motion. Entry 255 is never touched and
        // entry 0's luma sits well below entry 1's, so both contract anchors
        // survive the sweep byte-exact.
        for (int i = 254; i >= 0; --i) {
            while (lumaOf(r[i], g[i], b[i]) > lumaOf(r[i + 1], g[i + 1], b[i + 1])) {
                if (g[i] > 0) {
                    --g[i];
                } else if (r[i] > 0) {
                    --r[i];
                } else if (b[i] > 0) {
                    --b[i];
                } else {
                    break;  // all black cannot be too bright; defensive only
                }
            }
        }
        std::array<ImU32, 256> out{};
        for (int i = 0; i < 256; ++i) {
            out[i] = IM_COL32(r[i], g[i], b[i], 255);
        }
        return out;
    }();
    return lut;
}

// --- the bench chrome --------------------------------------------------------
//
// Everything below draws the enclosure the design puts around the picture: a
// recessed frame, an elapsed-time strip, the strength key and the foot lines.
// It is all ImDrawList geometry - no ImGui items - so the widget's layout and
// hit-testing behaviour is exactly what it was before the chrome existed.
//
// NOT ONE FIGURE HERE IS TYPED IN, AND NONE OF THEM IS INFERRED EITHER. The
// time labels are ages read off the arrival stamp of the very row they sit
// beside, the scroll rate is the caller's own measurement, the key's colours
// are sampled out of waterfallColor() itself, and its dB numbers are the
// bounds the rows it claims to cover were actually mapped through. Where a
// value is missing the element is dropped; nothing is filled in with a
// plausible number.
//
// THE DIFFERENCE BETWEEN READ AND INFERRED IS THE POINT, not a nicety. Both
// annotations here were once computed from an average: the ages from a row
// count divided by the line rate, the key's floor and ceiling from the newest
// line alone. Both are exactly right while nothing changes and quietly wrong
// the moment something does - a stalled source backdated every age below the
// gap, and a nudge of the Display sliders left the key describing one row out
// of five hundred. An annotation that is right only when nothing has happened
// is worth less than none, because it is trusted in precisely the moment it
// has stopped being true.

// Padding inside a chrome plate, and the inset of a plate from the picture's
// edge. Fixed pixel sizes rather than fractions of the widget: these space
// TEXT, whose size is fixed by fonts.hpp, and a fraction would crowd the
// letters in a short window and strand them in a tall one.
constexpr float kChromePad = 6.0f;
constexpr float kChromeInset = 9.0f;

float textWidth(ImFont* font, float px, const char* text) {
    if (font == nullptr || text == nullptr || text[0] == '\0') {
        return 0.0f;
    }
    return font->CalcTextSizeA(px, FLT_MAX, 0.0f, text).x;
}

// The dark plate the annotations sit on. The picture underneath can be any
// colour in the ramp, up to a near-white cream, so text laid straight onto it
// is unreadable exactly when the waterfall is busiest - which is when a legend
// is most wanted.
//
// 0.76 WAS NOT ENOUGH OPACITY TO MAKE THAT TRUE. Over the ramp's cream anchor
// a quarter of #F0EBB4 still came through, leaving the plate at a luminance
// the dim phosphor the key and the foot lines are lettered in reads against at
// 2.5:1 - which is not "quieter", it is gone. At 0.84 the same worst case is a
// plate the same text sits on at 8:1. The plate is the same colour it always
// was; it is simply doing the job the paragraph above says it is for.
void addGlassPlate(ImDrawList* dl, const ImVec2& tl, const ImVec2& br) {
    dl->AddRectFilled(tl, br, theme::withAlpha(theme::kVoid, 0.84f),
                      theme::kPanelRounding);
    dl->AddRect(tl, br, theme::withAlpha(theme::kBrassDark, 0.85f),
                theme::kPanelRounding, 0, theme::kHairline);
}

// "45s", "2m30", "1h05". Whole seconds are taken ONCE, up front, and the
// fields split out of that single integer: computing minutes and seconds
// independently from a double is how a formatter comes to print "4h60".
void formatElapsed(double seconds, char* buf, std::size_t n) {
    if (buf == nullptr || n == 0) {
        return;
    }
    if (!(seconds > 0.0)) {
        std::snprintf(buf, n, "0s");
        return;
    }
    // Saturated before the cast: a scroll rate a whisker above zero makes the
    // span enormous, and converting a double past the integer type's range is
    // undefined behaviour rather than a silly-looking label. 99h59 is the
    // largest thing this ever needs to say.
    double whole = std::floor(seconds + 0.5);
    if (whole > 359999.0) {
        whole = 359999.0;
    }
    const long long total = static_cast<long long>(whole);
    if (total < 60) {
        std::snprintf(buf, n, "%llds", total);
        return;
    }
    if (total < 3600) {
        const long long m = total / 60;
        const long long s = total % 60;
        if (s == 0) {
            std::snprintf(buf, n, "%lldm", m);
        } else {
            std::snprintf(buf, n, "%lldm%02lld", m, s);
        }
        return;
    }
    const long long h = total / 3600;
    const long long m = (total % 3600) / 60;
    std::snprintf(buf, n, "%lldh%02lld", h, m);
}

// The ladder the strength key's dB scale is read on. Separate from
// timeLabelStep's because the ladders differ - dB counts in fives and tens,
// time in sixties.
constexpr double kDbLadder[] = {1.0, 2.0, 5.0, 10.0, 20.0, 25.0, 50.0, 100.0, 200.0};

// Tolerance on the ceiling comparison, matching the one the drawing loop uses
// so "fits" and "drawn" cannot disagree about the last tick.
constexpr double kDbTickEps = 1.0e-6;

// Whether at least two multiples of `step` land inside [dbFloor, dbCeiling].
// The first tick sits on the first multiple at or above the floor, which can
// be almost a whole step in - so a step narrower than the range still proves
// nothing about how many figures reach the scale, and this asks directly.
bool twoDbTicksFit(double dbFloor, double dbCeiling, double step) {
    if (!(step > 0.0)) {
        return false;
    }
    const double first = std::ceil(dbFloor / step) * step;
    return first + step <= dbCeiling + kDbTickEps;
}

// The next entry UP the ladder that still gets two figures onto the scale, or
// 0 if there is none. Used to back off when the labels chosen at a finer step
// would overlap: a coarser scale that is complete beats a finer one with a
// figure silently missing out of the middle of it.
double coarserDbStep(double dbFloor, double dbCeiling, double step) {
    for (const double s : kDbLadder) {
        if (s > step && twoDbTicksFit(dbFloor, dbCeiling, s)) {
            return s;
        }
    }
    return 0.0;
}

// One placed tick label: its text and its left edge, measured from the left
// end of the ramp. 64 is the cap the drawing loop has always had.
constexpr int kMaxDbTicks = 64;
struct DbTick {
    float x;
    char text[24];
};

// Lays the labels for `step` out along a ramp `barSpan` wide, applying the
// same collision rule the scale is drawn with, and reports how many labels
// that rule had to suppress. Returns the number placed.
//
// The alignment term (-f * tw) hangs the first label off the ramp's left end
// and the last off its right, so every label sits inside the plate; it also
// means the labels are packed into barSpan MINUS a label width, which is why
// a step sized off the bare span can still collide.
int layoutDbTicks(ImFont* nf, float px, float barSpan, double dbFloor, double dbCeiling,
                  double step, DbTick out[kMaxDbTicks], int& dropped) {
    dropped = 0;
    const double range = dbCeiling - dbFloor;
    if (out == nullptr || !(step > 0.0) || !(range > 0.0)) {
        return 0;
    }
    int count = 0;
    double lastRight = -1.0e9;
    const double first = std::ceil(dbFloor / step) * step;
    for (int k = 0; k < kMaxDbTicks; ++k) {
        const double v = first + static_cast<double>(k) * step;
        if (v > dbCeiling + kDbTickEps) {
            break;
        }
        const float f = static_cast<float>((v - dbFloor) / range);
        char vb[24];
        // A value too long for the buffer would be printed TRUNCATED, which
        // on a dB scale is not a shorter label but a different number. It
        // takes a nonsense range to reach (over 1e22 dB), and the honest
        // answer to a figure that cannot be written is to leave it out.
        const int need = std::snprintf(vb, sizeof(vb), "%.0f", v);
        if (need < 0 || static_cast<std::size_t>(need) >= sizeof(vb)) {
            continue;
        }
        const float tw = textWidth(nf, px, vb);
        const float x = f * barSpan - f * tw;
        if (static_cast<double>(x) < lastRight + 5.0) {
            ++dropped;
            continue;  // would collide with the label before it
        }
        out[count].x = x;
        std::snprintf(out[count].text, sizeof(out[count].text), "%s", vb);
        ++count;
        lastRight = static_cast<double>(x + tw);
    }
    return count;
}

// The ages a strip lays its labels on: the ladder step, the first label, and
// how many of them there are. Kept together so the pass that measures the
// widest label and the pass that draws them cannot iterate different sets.
constexpr int kMaxTimeTicks = 16;
struct TimeTicks {
    double step = 0.0;
    double age[kMaxTimeTicks] = {};
    int count = 0;
};

// Ages at multiples of `step` that fall inside the picture's own age window
// [newest, oldest]. The first is the first multiple STRICTLY past the newest
// row's age: a label exactly on the top row would sit under the heading, and
// with a live receiver (newest age ~0) it would be a "0s" that says nothing.
TimeTicks timeTicks(double newest, double oldest, double step) {
    TimeTicks t;
    if (!(step > 0.0) || !(oldest > newest)) {
        return t;
    }
    t.step = step;
    double age = std::ceil(newest / step) * step;
    if (!(age > newest)) {
        age += step;
    }
    while (t.count < kMaxTimeTicks && age <= oldest) {
        t.age[t.count++] = age;
        age += step;
    }
    return t;
}

// The elapsed-time strip down the left edge. Returns its width in pixels, or
// 0 when it was not drawn - the foot lines start clear of whatever it took.
//
// EVERY LABEL IS PLACED AT THE ROW THAT ACTUALLY CARRIES THAT AGE. It used to
// be placed at a fraction of the strip, the strip's whole span being the row
// count divided by the caller's average line rate - which is a true statement
// about the picture only while lines arrive at a constant rate. They do not: a
// stopped receiver, a retune, a stalled source or a device that dropped out
// all leave a gap, and dividing through the average silently backdated every
// label below it, so the axis read "40s ago" over a row ten minutes old. Now
// the strip asks the ring where each age is (WaterfallView::rowAtAge, a binary
// search over the arrival stamps addLine records) and puts the figure there.
// A gap shows as labels crowded against the band that spans it, which is the
// truth about that band and is visible at a glance.
//
// THAT ALSO MEANS THE AXIS DOES NOT START AT ZERO. Its top is the newest row's
// own age, so a picture that stopped scrolling four minutes ago is labelled
// from four minutes rather than pretending its top row is "now" - and the
// figures keep counting up over the frozen picture, because the rows really
// are getting older.
//
// IT MEASURES `rows` OF THE `totalRows` ON SCREEN, NOT THE WHOLE PICTURE.
// `rows` is how many lines this view has actually received; the rest of the
// texture is the constructor's pre-fill, drawn in the empty colour. Scaling
// the axis over the full texture height - which this did until the strip was
// caught labelling "5s" against a band of nothing on a freshly started
// receiver, and against rows recycled from an older session after a restart -
// answers "how long ago was that" about a picture nothing wrote. So the strip
// covers the top rows/totalRows of the widget and stops there.
float drawTimeStrip(ImDrawList* dl, const ImVec2& tl, float w, float h, double now,
                    const WaterfallView& wf) {
    const WaterfallView::Ages ages = wf.ages(now);
    const int totalRows = wf.texHeight();
    // Two rows at least, and a span between them: one row has an age but no
    // extent, and an axis needs something to run along.
    if (!ages.valid || ages.rows < 2 || !(ages.span > 0.0) || totalRows <= 0 || w < 90.0f) {
        return 0.0f;
    }
    // The written history's own height on screen. The 48 px floor is the same
    // one the whole widget used to be judged by, now applied to the part of it
    // the axis actually describes: a strip too short for its heading and one
    // figure is not an axis, and a fresh receiver simply has none until enough
    // lines have arrived to earn one.
    const float stripH =
        h * static_cast<float>(std::min(ages.rows, totalRows)) / static_cast<float>(totalRows);
    // FOUR LINES OF THE SMALLEST FACE - the heading, two figures and the
    // margins between them - which is the least an axis can be and still be
    // one. It was the literal 48, and 48 is exactly four lines of the 12 px
    // this strip was laid out at; written this way it travels with the type
    // instead of quietly becoming three lines the next time the type grows.
    if (stripH < fonts::kTinySize * 4.0f) {
        return 0.0f;
    }
    // How many labels the strip has ROOM for, not a fixed number: the panel
    // is split with the spectrum and can be anything from a sliver to most of
    // the window. Three line heights and a hair per label leaves a clear gap
    // (the literal was 38 px, which is that rule at 12 px lettering); a fixed
    // count either crowds the short panel or leaves the tall one with two
    // lonely figures.
    const float perLabel = fonts::kTinySize * 3.0f + 2.0f;
    const int maxLabels = std::max(2, std::min(8, static_cast<int>(stripH / perLabel)));
    const double step = WaterfallView::timeLabelStep(ages.span, maxLabels);
    const TimeTicks ticks = timeTicks(ages.newest, ages.oldest, step);
    if (ticks.count < 1) {
        return 0.0f;  // nothing would carry a label; draw no empty gutter
    }

    ImFont* lf = fonts::ui();
    ImFont* cf = fonts::legend();
    const float px = fonts::kTinySize;
    // THE FIGURES TAKE THE FULL PHOSPHOR, the heading keeps the dim one.
    // theme.hpp's rule, applied to this gutter: "AGO" is a caption at rest and
    // may be engraved into its plate, but "2m30" is a reading, and every one
    // of these was written in the dim tone. On a busy picture that was 1.6:1
    // and on a quiet one 4.9:1; it is 7.1:1 and 11.7:1 now, and the heading
    // above them is still visibly the quieter of the two.
    const ImU32 kFigureInk = theme::kPhosphor;

    // THE BREAK: the single row where the picture jumps furthest in age, which
    // is where the lines stopped arriving. The ladder cannot describe it - a
    // ten-minute silence puts every round age from "2m" to "10m" on the SAME
    // row, and all but the first are dropped as collisions, leaving one lonely
    // figure and a whole fresh region with nothing beside it. So the two ages
    // either side of the jump are drawn against the break itself, which both
    // labels those regions and shows why the ladder skipped them.
    //
    // Only a jump wider than a whole label interval qualifies. A steady
    // receiver's rows differ by a frame time and never reach it, so nothing is
    // drawn where nothing happened.
    int breakRow = -1;
    double breakAbove = 0.0;
    double breakBelow = 0.0;
    {
        double widestJump = 0.0;
        for (int i = 1; i < ages.rows; ++i) {
            const double above = wf.rowAge(now, i - 1);
            const double below = wf.rowAge(now, i);
            const double jump = below - above;
            if (jump > widestJump) {
                widestJump = jump;
                breakRow = i;
                breakAbove = above;
                breakBelow = below;
            }
        }
        if (!(widestJump > step)) {
            breakRow = -1;
        }
    }

    char buf[32];
    float widest = textWidth(cf, px, "AGO");
    for (int k = 0; k < ticks.count; ++k) {
        formatElapsed(ticks.age[k], buf, sizeof(buf));
        widest = std::max(widest, textWidth(lf, px, buf));
    }
    // The break's own two figures are measured too: they are usually the
    // widest thing the strip has to hold, and a gutter sized without them
    // would clip the very labels the break exists to show.
    if (breakRow > 0) {
        formatElapsed(breakAbove, buf, sizeof(buf));
        widest = std::max(widest, textWidth(lf, px, buf));
        formatElapsed(breakBelow, buf, sizeof(buf));
        widest = std::max(widest, textWidth(lf, px, buf));
    }
    const float stripW = widest + kChromePad * 2.0f;
    if (stripW > w * 0.25f) {
        return 0.0f;  // a strip this wide would eat the picture
    }

    const ImVec2 sBR(tl.x + stripW, tl.y + stripH);
    // THE GUTTER IS A PLATE, and it has to be as opaque as one. At 0.62 the
    // picture behind it came through hard enough that a run of strong signal
    // - the cream end of the ramp - lifted the gutter to roughly the same
    // luminance as the figures written on it: 1.6:1, which is an axis that
    // disappears precisely when the waterfall is worth reading. Same colour,
    // same recessed look, enough of it to letter on.
    dl->AddRectFilled(tl, sBR, theme::withAlpha(theme::kVoid, 0.80f));
    dl->AddLine(ImVec2(sBR.x, tl.y), ImVec2(sBR.x, sBR.y),
                theme::withAlpha(theme::kBrassTint, 0.20f), theme::kHairline);
    // Where a partly-filled history ends, the gutter is closed off with the
    // face's own deck rail rather than trailing away into the empty picture -
    // the axis has a foot, and it is visibly not the foot of the widget.
    if (stripH < h - 0.5f) {
        addBenchRail(dl, tl.x, sBR.x, sBR.y);
    }

    // The column heading. Without it the figures are three numbers down the
    // side of a picture; with it they read "16s ago".
    const float capY = tl.y + 3.0f;
    dl->AddText(cf, px, ImVec2(tl.x + kChromePad, capY),
                theme::withAlpha(theme::kPhosphorDim, 0.85f), "AGO");

    const float firstY = capY + px + 4.0f;
    const float rowH = h / static_cast<float>(totalRows);

    // The break is drawn first so the ladder can step around it: two figures
    // printed over each other say less than either alone.
    float breakY = -1.0e9f;
    if (breakRow > 0) {
        const float y = tl.y + static_cast<float>(breakRow) * rowH;
        if (y > firstY + px && y < tl.y + stripH - px * 1.6f) {
            breakY = y;
            // A deck rail, the face's own word for one deck ending and the
            // next beginning - which is exactly what a reception gap is.
            addBenchRail(dl, tl.x, sBR.x + 5.0f, y);
            formatElapsed(breakAbove, buf, sizeof(buf));
            float tw = textWidth(lf, px, buf);
            dl->AddText(lf, px, ImVec2(sBR.x - kChromePad - tw, y - px - 2.0f),
                        kFigureInk, buf);
            formatElapsed(breakBelow, buf, sizeof(buf));
            tw = textWidth(lf, px, buf);
            dl->AddText(lf, px, ImVec2(sBR.x - kChromePad - tw, y + 2.0f),
                        kFigureInk, buf);
        }
    }

    float lastY = -1.0e9f;
    for (int k = 0; k < ticks.count; ++k) {
        const int row = wf.rowAtAge(now, ticks.age[k]);
        if (row < 0) {
            continue;  // no row is that old after all; say nothing about it
        }
        const float y = tl.y + static_cast<float>(row) * rowH;
        if (y < firstY) {
            continue;  // would sit under the heading
        }
        if (y > tl.y + stripH - px * 0.6f) {
            break;  // would be cut off at the foot of the written history
        }
        // A gap in reception packs several ages onto neighbouring rows, and
        // two figures on top of each other are unreadable in exactly the case
        // the axis exists to show. The ones that fit are drawn; the rest are
        // dropped rather than overprinted - the break above has already named
        // the ages either side of the jump that swallowed them.
        if (y < lastY + px) {
            continue;
        }
        if (y > breakY - px * 1.6f && y < breakY + px * 1.6f) {
            continue;  // the break's own two figures own this space
        }
        lastY = y;
        formatElapsed(ticks.age[k], buf, sizeof(buf));
        const float tw = textWidth(lf, px, buf);
        // The tick crosses the strip's edge and overhangs the picture, the
        // way an axis on an instrument does.
        dl->AddLine(ImVec2(sBR.x - 4.0f, y), ImVec2(sBR.x + 5.0f, y),
                    theme::withAlpha(theme::kPhosphorDim, 0.55f), theme::kHairline);
        dl->AddText(lf, px, ImVec2(sBR.x - kChromePad - tw, y - px * 0.5f),
                    kFigureInk, buf);
    }
    return stripW;
}

// The strength key: the ramp itself, its dB scale, the pair the picture is
// currently mapped through, and - when they are not the same thing - how much
// of the picture that pair is true of. Returns the y of the plate's bottom
// edge, or 0 when there was no room to draw it.
//
// THE BAR IS SAMPLED OUT OF waterfallColor(). Hand-picking a few stops for a
// legend is how a key comes to describe a colour table the picture no longer
// uses - and this one is a MEASUREMENT scale, pinned by tests to be
// brightness-monotonic, so a key that disagreed with it would be lying about
// which of two signals is stronger.
//
// WHAT IT CLAIMS, AND WHY THE CLAIM IS NOW BOUNDED. A row is mapped once, when
// it is written, and it keeps those colours for as long as it is on screen.
// Move the Display sliders and the floor/ceiling change for the rows written
// AFTERWARDS and for no others - so a key captioned for the picture as a whole
// was right about the top row and wrong about every row beneath it, and said
// nothing to distinguish the two. It now states the run of newest rows that
// were actually mapped through this pair (`coveredRows` of `historyRows`) and
// the caller rules the picture at the boundary, so the reader can see where
// the key stops applying instead of being told it never does.
float drawStrengthKey(ImDrawList* dl, const ImVec2& tl, float w, float h, float dbFloor,
                      float dbCeiling, int coveredRows, int historyRows) {
    ImFont* uf = fonts::ui();
    ImFont* cf = fonts::legend();
    ImFont* nf = fonts::reading();
    const float px = fonts::kTinySize;

    const char* title = "STRENGTH KEY - dB";
    const char* pairCap = "FLOOR / CEILING";
    char pairText[48];
    std::snprintf(pairText, sizeof(pairText), "%.0f / %.0f",
                  static_cast<double>(dbFloor), static_cast<double>(dbCeiling));

    // The coverage line appears only when there is something to say: with the
    // whole history mapped through one pair the key describes the picture, and
    // a line saying so on every ordinary frame is noise that would teach the
    // reader to stop looking at it.
    const bool partial = (coveredRows > 0 && historyRows > 0 && coveredRows < historyRows);
    const char* coverCap = "APPLIES TO";
    char coverText[48];
    coverText[0] = '\0';
    if (partial) {
        std::snprintf(coverText, sizeof(coverText), "TOP %d LINES", coveredRows);
    }

    const float gap = 10.0f;
    float inner = textWidth(cf, px, title);
    inner = std::max(inner, textWidth(cf, px, pairCap) + gap + textWidth(nf, px, pairText));
    if (partial) {
        inner = std::max(inner, textWidth(cf, px, coverCap) + gap + textWidth(uf, px, coverText));
    }
    inner = std::max(inner, 110.0f);
    const float boxW = inner + kChromePad * 2.0f;

    const float lineH = px + 3.0f;
    const float barH = 9.0f;
    const float boxH =
        kChromePad * 2.0f + lineH * (partial ? 4.0f : 3.0f) + barH + 10.0f;
    if (boxW > w * 0.46f || boxH > h * 0.60f) {
        return 0.0f;  // no room for the key; the picture keeps the space
    }

    const ImVec2 bTL(tl.x + w - kChromeInset - boxW, tl.y + kChromeInset);
    const ImVec2 bBR(bTL.x + boxW, bTL.y + boxH);
    addGlassPlate(dl, bTL, bBR);

    dl->AddText(cf, px, ImVec2(bTL.x + kChromePad, bTL.y + kChromePad),
                theme::kInkMuted, title);

    // The ramp, one screen column at a time. The half-pixel overlap keeps
    // float column edges from leaving hairline gaps through the bar.
    const float barL = bTL.x + kChromePad;
    const float barR = bBR.x - kChromePad;
    const float barTop = bTL.y + kChromePad + lineH + 2.0f;
    const float barSpan = barR - barL;
    const int cols = std::max(2, std::min(320, static_cast<int>(barSpan)));
    for (int i = 0; i < cols; ++i) {
        const float x0 = barL + barSpan * static_cast<float>(i) / static_cast<float>(cols);
        const float x1 =
            barL + barSpan * static_cast<float>(i + 1) / static_cast<float>(cols);
        const float norm = static_cast<float>(i) / static_cast<float>(cols - 1);
        dl->AddRectFilled(ImVec2(x0, barTop), ImVec2(x1 + 0.5f, barTop + barH),
                          waterfallColor(norm));
    }
    dl->AddRect(ImVec2(barL, barTop), ImVec2(barR, barTop + barH),
                theme::withAlpha(theme::kBrassDark, 0.90f), 0.0f, 0, theme::kHairline);

    // The dB scale beneath the bar. Each label is positioned from its own
    // value, so a label and the colour above it cannot drift apart; the
    // alignment term (-f * tw) hangs the first label off the bar's left end
    // and the last off its right, keeping both inside the plate.
    const double lo = static_cast<double>(dbFloor);
    const double hi = static_cast<double>(dbCeiling);
    // The number of ticks comes from the width the labels actually need, not
    // from a constant: asking for five on a narrow bar puts two of them on
    // top of each other, and the collision rule then drops whichever came
    // second - which reads as an axis with a figure missing rather than as a
    // coarser scale. Sized off the floor value because it is the widest (a
    // leading minus and the most digits).
    char widestTick[24];
    std::snprintf(widestTick, sizeof(widestTick), "%.0f", lo);
    const float tickW = textWidth(nf, px, widestTick);
    const int maxIntervals =
        std::max(2, static_cast<int>(barSpan / std::max(tickW + 8.0f, 1.0f)));

    // That width estimate is an estimate: it measures every label at the
    // widest one's width and ignores the alignment squeeze, so the ladder
    // entry it points at sometimes still collides. Rather than let the
    // collision rule punch a hole in the middle of the scale, step UP the
    // ladder until nothing has to be dropped - stopping the moment a coarser
    // step would leave fewer than two figures on the scale, because a scale
    // with a gap in it is still better than a lone number. Measured over the
    // 12,966 whole-decibel floor/ceiling pairs the Display section can reach,
    // this takes the pairs that lose a figure from 742 to 24, none of them
    // losing more than one, while leaving the common ranges as fine as they
    // were (the default -110..0 keeps -100 / -50 / 0).
    double step = WaterfallView::dbLabelStep(lo, hi, maxIntervals);
    DbTick ticks[kMaxDbTicks];
    int dropped = 0;
    int count = layoutDbTicks(nf, px, barSpan, lo, hi, step, ticks, dropped);
    for (int guard = 0; guard < 12 && dropped > 0; ++guard) {
        const double coarser = coarserDbStep(lo, hi, step);
        if (!(coarser > 0.0)) {
            break;
        }
        DbTick trial[kMaxDbTicks];
        int trialDropped = 0;
        const int trialCount =
            layoutDbTicks(nf, px, barSpan, lo, hi, coarser, trial, trialDropped);
        if (trialCount < 2) {
            break;  // the coarser scale would be a lone figure; keep the gap
        }
        step = coarser;
        count = trialCount;
        dropped = trialDropped;
        std::copy(trial, trial + trialCount, ticks);
    }
    const float tickY = barTop + barH + 2.0f;
    for (int i = 0; i < count; ++i) {
        // Full phosphor, for the same reason the elapsed-time figures take it:
        // this is the scale that says which colour in the bar means what, and
        // in the dim tone on this plate it measured 2.5:1 over a bright
        // picture. The plate's captions below stay engraved-quiet.
        dl->AddText(nf, px, ImVec2(barL + ticks[i].x, tickY), theme::kPhosphor,
                    ticks[i].text);
    }

    const float ruleY = tickY + lineH + 2.0f;
    dl->AddLine(ImVec2(barL, ruleY), ImVec2(barR, ruleY),
                theme::withAlpha(theme::kBrassDark, 0.90f), theme::kHairline);

    const float pairY = ruleY + 3.0f;
    dl->AddText(cf, px, ImVec2(barL, pairY), theme::kInkFaint, pairCap);
    const float pw = textWidth(nf, px, pairText);
    dl->AddText(nf, px, ImVec2(barR - pw, pairY), theme::kPhosphor, pairText);

    if (partial) {
        // AMBER, because it is a reading about the picture rather than part of
        // the scale above it - and because it is the line that decides whether
        // the rest of the plate can be trusted about the row under the
        // reader's eye. ui() and not reading(): "TOP 128 LINES" is a phrase
        // carrying a figure, which is the case fonts.hpp hands to the prose
        // face, not three stems of Nova Mono merging into a block.
        const float coverY = pairY + lineH;
        dl->AddText(cf, px, ImVec2(barL, coverY), theme::kInkFaint, coverCap);
        const float cw = textWidth(uf, px, coverText);
        dl->AddText(uf, px, ImVec2(barR - cw, coverY), theme::kAmber, coverText);
    }
    return bBR.y;
}

// The rail across the picture at the row where the mapping changed: above it
// the strength key is true, below it the colours came from a different floor
// and ceiling. Drawn with the face's own deck rail because that is exactly
// what it is - one deck of the picture ending and another beginning - and
// captioned, because an unexplained line across a measurement is worse than no
// line at all.
//
// `x0` starts it clear of the elapsed-time gutter so it cannot be mistaken for
// part of that axis.
void drawRangeBoundary(ImDrawList* dl, float x0, float x1, float y) {
    ImFont* cf = fonts::legend();
    const float px = fonts::kTinySize;
    const char* cap = "RANGE CHANGED";
    const float tw = textWidth(cf, px, cap);
    addBenchRail(dl, x0, x1, y);
    // BELOW the rail, not above it. Above, the caption lands in the strip of
    // picture between the rail and the strength key - and the key's plate is
    // opaque and sits exactly there whenever the boundary is near the top,
    // which is the common case straight after the sliders move. The first
    // render of this put "RANGE CHANGED" underneath the plate every time.
    if (x1 - x0 > tw + kChromePad * 3.0f) {
        // ON A PLATE, like every other annotation in this file. It was the one
        // caption drawn straight onto the picture, and the picture under it is
        // whatever the ramp happens to be painting there - against the cream
        // end it was 2.5:1, which is an unexplained rail across a measurement,
        // which the comment above calls worse than no rail at all.
        const ImVec2 cTL(x1 - kChromePad * 2.0f - tw, y + 1.0f);
        const ImVec2 cBR(x1, y + 5.0f + px);
        addGlassPlate(dl, cTL, cBR);
        dl->AddText(cf, px, ImVec2(x1 - kChromePad - tw, y + 3.0f),
                    theme::withAlpha(theme::kInkMuted, 0.90f), cap);
    }
}

// The foot lines: how fast the picture scrolls and how much time it holds,
// then what the receiver is doing with it. Every clause is dropped when the
// value behind it is missing, and the plate shrinks to what is left.
//
// THE TWO HALVES COME FROM DIFFERENT MEASUREMENTS AND STAND OR FALL APART.
// The scroll rate is the caller's, taken over its own averaging window. The
// visible span is this view's: the difference between the newest and oldest
// arrival stamps in the ring, which is the history the picture is HOLDING and
// not the depth of its texture, nor a row count divided by an average rate -
// that division was reporting "8m VISIBLE" for a picture whose top and bottom
// rows were ten minutes apart across a stall. So a stalled receiver loses the
// rate clause, which is genuinely unmeasured, and keeps the span, which is
// not.
void drawFootLines(ImDrawList* dl, const ImVec2& tl, float w, float h, float leftInset,
                   float linesPerSecond, double heldSeconds, const char* decoding) {
    char rateText[48];
    rateText[0] = '\0';
    if (linesPerSecond > 0.0f) {
        // A rate of 4.2 line/s is a real reading and rounds to "4"; below ten
        // the fraction is the difference between a live pipeline and a
        // struggling one, so it is kept.
        if (linesPerSecond >= 10.0f) {
            std::snprintf(rateText, sizeof(rateText), "SCROLL %.0f line/s",
                          static_cast<double>(linesPerSecond));
        } else {
            std::snprintf(rateText, sizeof(rateText), "SCROLL %.1f line/s",
                          static_cast<double>(linesPerSecond));
        }
    }
    char spanText[48];
    spanText[0] = '\0';
    if (heldSeconds > 0.0) {
        char elapsed[32];
        formatElapsed(heldSeconds, elapsed, sizeof(elapsed));
        std::snprintf(spanText, sizeof(spanText), "%s VISIBLE", elapsed);
    }
    char scrollText[112];
    if (rateText[0] != '\0' && spanText[0] != '\0') {
        std::snprintf(scrollText, sizeof(scrollText), "%s  -  %s", rateText, spanText);
    } else {
        std::snprintf(scrollText, sizeof(scrollText), "%s%s", rateText, spanText);
    }
    const bool haveDecode = (decoding != nullptr && decoding[0] != '\0');
    if (scrollText[0] == '\0' && !haveDecode) {
        return;
    }

    ImFont* sf = fonts::ui();
    const float spx = fonts::kTinySize;
    const float dpx = fonts::kLegendSize;
    const float sw = textWidth(sf, spx, scrollText);
    const float dw = haveDecode ? textWidth(sf, dpx, decoding) : 0.0f;

    const float lineGap = 2.0f;
    float boxH = kChromePad * 2.0f;
    if (scrollText[0] != '\0') { boxH += spx + lineGap; }
    if (haveDecode) { boxH += dpx + lineGap; }
    const float boxW = std::max(sw, dw) + kChromePad * 2.0f;

    const float x0 = tl.x + leftInset + kChromeInset;
    const float y1 = tl.y + h - kChromeInset;
    if (boxW > w - leftInset - kChromeInset * 2.0f || boxH > h * 0.5f) {
        return;  // no room; better nothing than a clipped half-sentence
    }
    const ImVec2 fTL(x0, y1 - boxH);
    const ImVec2 fBR(x0 + boxW, y1);
    addGlassPlate(dl, fTL, fBR);

    float y = fTL.y + kChromePad;
    if (scrollText[0] != '\0') {
        // A shade under the decode line below it, which keeps the two apart -
        // but not the dim tone it was, which put a line carrying two figures
        // (the rate and the history the picture holds) at 2.5:1 over a bright
        // waterfall. 6.3:1 there now, and still visibly the quieter line.
        dl->AddText(sf, spx, ImVec2(fTL.x + kChromePad, y),
                    theme::withAlpha(theme::kPhosphor, 0.85f), scrollText);
        y += spx + lineGap;
    }
    if (haveDecode) {
        dl->AddText(sf, dpx, ImVec2(fTL.x + kChromePad, y), theme::kPhosphor, decoding);
    }
}

}  // namespace

ImU32 waterfallColor(float norm01) {
    // !(x > 0) instead of (x <= 0): NaN fails every comparison, so a NaN
    // input falls into this branch and reads as the coldest color instead of
    // indexing the LUT with garbage.
    if (!(norm01 > 0.0f)) {
        norm01 = 0.0f;
    }
    if (norm01 > 1.0f) {
        norm01 = 1.0f;
    }
    return colorLut()[static_cast<int>(norm01 * 255.0f + 0.5f)];
}

void mapLineToPixels(const float* dbBins, int n, float dbMin, float dbMax,
                     ImU32* dst, int texWidth) {
    if (dst == nullptr || texWidth <= 0) {
        return;
    }
    // Degenerate range check is !(dbMax > dbMin) so a NaN bound also lands
    // here instead of producing NaN norms for every pixel.
    if (dbBins == nullptr || n <= 0 || !(dbMax > dbMin)) {
        const ImU32 floorColor = waterfallColor(0.0f);
        for (int x = 0; x < texWidth; ++x) {
            dst[x] = floorColor;
        }
        return;
    }
    const float invRange = 1.0f / (dbMax - dbMin);
    for (int x = 0; x < texWidth; ++x) {
        // Nearest resampling: the bin under the pixel's center. The clamp
        // guards float round-up at the right edge for extreme sizes.
        int src = static_cast<int>((static_cast<float>(x) + 0.5f) *
                                   static_cast<float>(n) / static_cast<float>(texWidth));
        if (src >= n) {
            src = n - 1;
        }
        dst[x] = waterfallColor((dbBins[src] - dbMin) * invRange);
    }
}

WaterfallView::WaterfallView(int width, int height)
    : width_(std::max(width, 0)), height_(std::max(height, 0)) {
    // Pre-fill with the coldest color so the first frame shows an empty
    // waterfall, not uninitialized texels.
    pixels_.assign(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_),
                   waterfallColor(0.0f));
    // One arrival stamp per row. The values are never read before the row is
    // written - filled_ gates every reader - so zero here is a placeholder and
    // not a claim that these rows arrived at the epoch.
    times_.assign(static_cast<std::size_t>(height_), 0.0);
}

double WaterfallView::nowSeconds() noexcept {
    using clock = std::chrono::steady_clock;
    // Magic static: one epoch for the process, fixed at the first call, so the
    // doubles stay small and exact rather than counting from 1970.
    static const clock::time_point epoch = clock::now();
    return std::chrono::duration<double>(clock::now() - epoch).count();
}

WaterfallView::~WaterfallView() {
    if (texture_ != 0) {
        const GLuint id = texture_;
        glDeleteTextures(1, &id);
    }
}

void WaterfallView::addLine(const float* dbBins, int n, float dbMin, float dbMax) {
    addLine(dbBins, n, dbMin, dbMax, nowSeconds());
}

void WaterfallView::addLine(const float* dbBins, int n, float dbMin, float dbMax,
                            double atSeconds) {
    if (width_ <= 0 || height_ <= 0) {
        return;
    }
    // What the row ABOVE this one was mapped through and when it arrived, both
    // read before the cursor moves off it.
    const bool hadRows = (filled_ > 0);
    const float prevMin = dbMin_;
    const float prevMax = dbMax_;
    const double prevTime = hadRows ? times_[static_cast<std::size_t>(cursor_)] : 0.0;
    // The cursor walks upward through the texture (decrement, wrapped):
    // rows cursor, cursor+1, ... then read newest-to-oldest, which the
    // wrapped-v draw maps straight to top-to-bottom on screen.
    // Record what this line was mapped through, so the strength key states
    // the range the PICTURE was painted with rather than one handed to the
    // draw call separately and free to disagree with it. The !(dbMax > dbMin)
    // test is the same NaN-safe form mapLineToPixels uses: a degenerate range
    // paints a flat floor row, and there is then no range to name.
    dbMin_ = dbMin;
    dbMax_ = dbMax;
    hasRange_ = (dbMax > dbMin);
    cursor_ = (cursor_ + height_ - 1) % height_;
    mapLineToPixels(dbBins, n, dbMin, dbMax,
                    pixels_.data() + static_cast<std::size_t>(cursor_) * static_cast<std::size_t>(width_),
                    width_);
    // WHEN THIS ROW ARRIVED, which is the only thing that can date it later.
    // Forced non-decreasing toward the newest row: rowAtAge() binary-searches
    // these, and a stamp out of order would make it answer about the wrong
    // row. A non-finite stamp keeps the previous row's rather than poisoning
    // every age computed from it.
    double stamp = atSeconds;
    if (!std::isfinite(stamp)) {
        stamp = prevTime;
    }
    if (hadRows && stamp < prevTime) {
        stamp = prevTime;
    }
    times_[static_cast<std::size_t>(cursor_)] = stamp;
    // THE RUN OF NEWEST ROWS SHARING ONE dB RANGE, which is all the strength
    // key may claim. It extends while the range is unchanged and restarts at 1
    // the moment it moves, so the row it points at is exactly where the
    // mapping changed. Capped at height_: once the run has grown past the
    // ring, the row that differed has been overwritten and every row on screen
    // really was mapped through this pair.
    if (hadRows && prevMin == dbMin && prevMax == dbMax) {
        rangeRows_ = std::min(rangeRows_ + 1, height_);
    } else {
        rangeRows_ = 1;
    }
    // Cap at height_: once every row is rewritten between draws, uploading
    // the full texture is cheaper than height_ single-row uploads.
    pendingRows_ = std::min(pendingRows_ + 1, height_);
    // How much of the ring is a measurement rather than the constructor's
    // pre-fill. Saturates at height_ - past that the ring recycles a row per
    // line and the written depth stops growing. See filledRows().
    filled_ = std::min(filled_ + 1, height_);
}

WaterfallView::Ages WaterfallView::ages(double atSeconds) const noexcept {
    Ages a;
    if (filled_ <= 0 || height_ <= 0) {
        return a;
    }
    const double newestStamp = times_[static_cast<std::size_t>(cursor_)];
    const int oldestRow = (cursor_ + filled_ - 1) % height_;
    const double oldestStamp = times_[static_cast<std::size_t>(oldestRow)];
    a.valid = true;
    a.rows = filled_;
    // std::max with the literal first, so a NaN clock reading collapses to
    // zero here instead of reaching the labels: max(0.0, NaN) returns 0.0,
    // and a zero span then draws no axis at all.
    a.newest = std::max(0.0, atSeconds - newestStamp);
    a.oldest = std::max(a.newest, atSeconds - oldestStamp);
    a.span = a.oldest - a.newest;
    return a;
}

double WaterfallView::rowAge(double atSeconds, int rowsFromNewest) const noexcept {
    if (rowsFromNewest < 0 || rowsFromNewest >= filled_ || height_ <= 0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const std::size_t row =
        static_cast<std::size_t>((cursor_ + rowsFromNewest) % height_);
    const double age = atSeconds - times_[row];
    // Same clamp as ages(): a row cannot have arrived in the future, and a NaN
    // clock reading must not become a NaN label.
    return std::max(0.0, age);
}

int WaterfallView::rowAtAge(double atSeconds, double ageSeconds) const noexcept {
    if (filled_ <= 0 || height_ <= 0) {
        return -1;
    }
    // A row is at least `ageSeconds` old exactly when its stamp is at or
    // before this instant. Stamps decrease monotonically from row 0 outward
    // (addLine enforces it), so the first row satisfying it is found by
    // bisection; a NaN age makes every comparison false and returns -1, which
    // the caller reads as "no row to label".
    const double cutoff = atSeconds - ageSeconds;
    int lo = 0;
    int hi = filled_ - 1;
    int found = -1;
    while (lo <= hi) {
        const int mid = lo + (hi - lo) / 2;
        const std::size_t row = static_cast<std::size_t>((cursor_ + mid) % height_);
        if (times_[row] <= cutoff) {
            found = mid;
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    }
    return found;
}

double WaterfallView::timeLabelStep(double spanSeconds, int maxLabels) {
    // !(x > 0) so a NaN span is rejected here rather than propagating into
    // the comparison below and silently choosing the first ladder entry.
    if (!(spanSeconds > 0.0) || maxLabels < 1) {
        return 0.0;
    }
    // The ladder a clock is read on. It deliberately stops at one second: a
    // waterfall whose entire history is under a second would otherwise be
    // labelled in steps that all round to "1s", which is three identical
    // numbers down the edge and worse than no axis.
    static const double kLadder[] = {1.0,   2.0,   5.0,    10.0,   15.0,   20.0,  30.0,
                                     60.0,  120.0, 300.0,  600.0,  900.0,  1800.0, 3600.0};
    const double want = spanSeconds / static_cast<double>(maxLabels);
    for (const double s : kLadder) {
        if (s >= want) {
            return s;
        }
    }
    return std::ceil(want / 3600.0) * 3600.0;  // beyond an hour: whole hours
}

double WaterfallView::dbLabelStep(double dbFloor, double dbCeiling, int maxIntervals) {
    const double range = dbCeiling - dbFloor;
    // !(x > 0) so a NaN bound is rejected here rather than propagating into
    // the comparisons below and silently choosing the first ladder entry.
    if (!(range > 0.0) || maxIntervals < 1) {
        return 0.0;
    }
    const double want = range / static_cast<double>(maxIntervals);
    double chosen = 0.0;
    for (const double s : kDbLadder) {
        if (s >= want) {
            chosen = s;
            break;
        }
    }
    if (!(chosen > 0.0)) {
        chosen = std::ceil(want / 500.0) * 500.0;  // beyond the ladder: whole 500s
    }
    if (twoDbTicksFit(dbFloor, dbCeiling, chosen)) {
        return chosen;
    }
    // The width-driven choice would put ONE figure on the scale. Take the
    // coarsest ladder entry that puts at least two there instead - the scale
    // is then finer than asked for, which crowds it at worst, where the
    // alternative names a single colour and says nothing about its
    // neighbours. Nothing on the ladder manages it (a range under 1 dB) means
    // no scale at all: an unlabelled ramp under a plate that still states the
    // exact floor and ceiling, rather than an axis of one number.
    double best = 0.0;
    for (const double s : kDbLadder) {
        if (s < chosen && twoDbTicksFit(dbFloor, dbCeiling, s)) {
            best = s;
        }
    }
    return best;
}

int WaterfallView::uvRects(int rowCursor, int height, double u0, double u1,
                           UvRect out[2]) {
    if (out == nullptr || height <= 0) {
        return 0;
    }
    // Clamp the window into the texture. Deliberately one-sided per bound
    // (`<` / `>` rather than !(...)-style) so a NaN survives the clamps and
    // is then caught by the !(b > a) degenerate exit below — a NaN window
    // must render as "nothing", never as a silently-full window.
    double a = u0;
    double b = u1;
    if (a < 0.0) { a = 0.0; }
    if (b > 1.0) { b = 1.0; }
    // Catches inverted, equal (zero-width), NaN, and windows entirely
    // outside [0, 1] (both bounds clamp to the same edge).
    if (!(b > a)) {
        return 0;
    }
    // rowCursor() already stays in [0, height); wrap defensively anyway so a
    // caller-supplied cursor can never index texels that do not exist.
    int c = rowCursor % height;
    if (c < 0) { c += height; }

    const float fu0 = static_cast<float>(a);
    const float fu1 = static_cast<float>(b);
    if (c == 0) {
        // Ring start coincides with the texture's top row: no seam inside
        // the widget, one quad covers everything.
        out[0] = UvRect{0.0f, 1.0f, fu0, 0.0f, fu1, 1.0f};
        return 1;
    }
    // Seam split. Newest rows c..H-1 render on top, oldest rows 0..c-1
    // below. Each quad's screen share equals its v share, so history rows
    // keep a uniform on-screen thickness across the seam.
    const float vSeam = static_cast<float>(c) / static_cast<float>(height);
    out[0] = UvRect{0.0f, 1.0f - vSeam, fu0, vSeam, fu1, 1.0f};
    out[1] = UvRect{1.0f - vSeam, 1.0f, fu0, 0.0f, fu1, vSeam};
    return 2;
}

void WaterfallView::draw(float width, float height) {
    draw(width, height, 0.0, 1.0);
}

void WaterfallView::draw(float width, float height, double u0, double u1) {
    if (width_ <= 0 || height_ <= 0 || width <= 0.0f || height <= 0.0f) {
        return;
    }
    if (texture_ == 0) {
        GLuint id = 0;
        glGenTextures(1, &id);
        texture_ = id;
        glBindTexture(GL_TEXTURE_2D, texture_);
        // NEAREST: see the seam rationale in the header. Wrap modes are no
        // longer exercised (uvRects keeps every uv inside [0, 1]); REPEAT
        // stays as the value the original wrapped-v draw used, and GL 1.1
        // has no CLAMP_TO_EDGE to switch to anyway.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width_, height_, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, pixels_.data());
        pendingRows_ = 0;
    } else if (pendingRows_ > 0) {
        glBindTexture(GL_TEXTURE_2D, texture_);
        if (pendingRows_ >= height_) {
            // Every row changed since the last draw; one bulk update.
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width_, height_, GL_RGBA,
                            GL_UNSIGNED_BYTE, pixels_.data());
        } else {
            for (int k = 0; k < pendingRows_; ++k) {
                const int row = (cursor_ + k) % height_;
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, row, width_, 1, GL_RGBA,
                                GL_UNSIGNED_BYTE,
                                pixels_.data() +
                                    static_cast<std::size_t>(row) * static_cast<std::size_t>(width_));
            }
        }
        pendingRows_ = 0;
    }

    // At most two quads split at the ring seam (see uvRects). Emitted as
    // AddImage calls on the draw list rather than stacked ImGui::Image items
    // because items would insert ItemSpacing between the two quads; a single
    // Dummy then reserves the widget rectangle exactly once — even when a
    // degenerate window drew nothing, so a bad zoom state cannot shift the
    // surrounding layout.
    UvRect rects[2];
    const int rectCount = uvRects(cursor_, height_, u0, u1, rects);
    if (rectCount > 0) {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        for (int i = 0; i < rectCount; ++i) {
            const UvRect& r = rects[i];
            drawList->AddImage(static_cast<ImTextureID>(texture_),
                               ImVec2(p0.x, p0.y + r.y0Frac * height),
                               ImVec2(p0.x + width, p0.y + r.y1Frac * height),
                               ImVec2(r.u0, r.v0), ImVec2(r.u1, r.v1));
        }
    }
    ImGui::Dummy(ImVec2(width, height));
}

void WaterfallView::draw(float width, float height, double u0, double u1,
                         const Chrome& chrome) {
    // The picture first, through the unchanged path: same texture upload,
    // same seam-split quads, same single Dummy reserving the widget. The
    // cursor is read BEFORE that call because the Dummy moves it past the
    // widget, and every annotation below is placed from the widget's own
    // top-left corner.
    const ImVec2 tl = ImGui::GetCursorScreenPos();
    draw(width, height, u0, u1);
    if (width_ <= 0 || height_ <= 0 || width <= 0.0f || height <= 0.0f) {
        return;  // inert view: the plain draw reserved nothing, so neither do we
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 br(tl.x + width, tl.y + height);
    // One clock reading for the whole frame: the strip, the boundary rail and
    // the foot line all date the same picture, and three separate readings
    // could disagree about it by a frame's worth of drift.
    const double now = nowSeconds();
    const Ages age = ages(now);

    // Annotations are clipped to the widget: a legend that spilled past the
    // frame would land on the panel beside it, which on this face is brass.
    dl->PushClipRect(tl, br, true);
    // The strip measures only the rows this view has actually been handed;
    // the rest of the texture is the constructor's pre-fill. See filledRows().
    const float stripW = drawTimeStrip(dl, tl, width, height, now, *this);
    float keyBottom = 0.0f;
    if (hasRange_) {
        // Only with a range. Before the first line there is no floor and
        // ceiling to label the ramp with, and a key captioned with invented
        // bounds would misread every colour under it.
        keyBottom = drawStrengthKey(dl, tl, width, height, dbMin_, dbMax_, rangeRows(),
                                    filled_);
    }
    // The rail at the row where the mapping changed, whenever that row is on
    // screen and clear of the key's own plate. Clear of it because the plate
    // is opaque and a rail vanishing under it reads as a rendering fault; when
    // the boundary is that close to the top the plate's own APPLIES TO line is
    // already sitting beside the rows in question.
    if (hasRange_ && rangeRows() < filled_ && height_ > 0) {
        const float boundaryY =
            tl.y + static_cast<float>(rangeRows()) * (height / static_cast<float>(height_));
        if (boundaryY > keyBottom + 6.0f && boundaryY < tl.y + height - 2.0f) {
            drawRangeBoundary(dl, tl.x + stripW, br.x, boundaryY);
        }
    }
    // The span is what the ring can prove it holds, not a row count divided by
    // an average rate. Zero when fewer than two lines have arrived, which
    // drops the clause rather than reporting "0s VISIBLE" of a real picture.
    drawFootLines(dl, tl, width, height, stripW, chrome.linesPerSecond,
                  age.valid ? age.span : 0.0, chrome.decoding);
    dl->PopClipRect();

    // The frame last, over the outer edge of the picture and outside the clip
    // so its own hairlines are never cut. Recessed: this is a well cut into
    // the panel, not a plate sitting proud of it.
    addBenchBevel(dl, tl, br, theme::kPanelRounding, false);
    dl->AddRect(ImVec2(tl.x + 1.0f, tl.y + 1.0f), ImVec2(br.x - 1.0f, br.y - 1.0f),
                theme::withAlpha(theme::kEnamelDark, 0.80f), theme::kPanelRounding, 0,
                theme::kHairline);
}

const ImU32* WaterfallView::rowPixels(int row) const noexcept {
    if (row < 0 || row >= height_ || width_ <= 0) {
        return nullptr;
    }
    return pixels_.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(width_);
}

}  // namespace cascade::gui
