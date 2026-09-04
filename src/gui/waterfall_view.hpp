// Scrolling waterfall widget: CPU-side pixel ring + lazily created GL texture.
//
// The CPU ring (per-row RGBA pixels + row cursor) is deliberately separate
// from the GL upload path so every piece of logic that can be wrong — the
// colormap, the dB->pixel conversion, nearest resampling, ring wrap, the
// seam/window uv math — is testable headless. The draw() overloads are the
// only members that touch OpenGL.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <vector>

#include <imgui.h>

namespace cascade::gui {

// Colormap LUT lookup. Input clamped to [0, 1] (NaN reads as 0). Progression
// deep blue -> cyan -> yellow -> red with monotonically non-decreasing
// perceived brightness (0.299R + 0.587G + 0.114B) across the whole table —
// enforced at LUT build time, because rounding independent channels of a
// piecewise-linear ramp can otherwise dip by a fraction of a luma unit.
//
// Contract constants a display (and the tests) may rely on:
//   waterfallColor(0.0f) == IM_COL32(  6,  20,  10, 255)   // quiet phosphor
//   waterfallColor(1.0f) == IM_COL32(240, 235, 180, 255)   // cream anchor
// The top anchor is a warm cream rather than white because it must be the
// brightest entry in the table and still leave the eye somewhere to read a
// peak; the bottom anchor is lifted off true black so a signal a few dB above
// the noise floor stays visible. All entries are fully opaque.
ImU32 waterfallColor(float norm01);

// Converts one spectrum line into one texture row: clamped normalization
// norm = (db - dbMin) / (dbMax - dbMin), nearest-neighbor resampling of n
// bins onto texWidth pixels, then waterfallColor lookup.
//
// Resampling rule (documented so tests can pin it): pixel x reads source bin
//   floor((x + 0.5) * n / texWidth), clamped to [0, n-1]
// i.e. the bin under the pixel's center. n == texWidth is an exact identity.
//
// Degenerate inputs are defined, not UB: dbBins == nullptr, n <= 0, or a
// non-positive/NaN dB span (dbMax <= dbMin) fill the row with the norm-0
// floor color — "no information" renders as an empty (coldest) line.
void mapLineToPixels(const float* dbBins, int n, float dbMin, float dbMax,
                     ImU32* dst, int texWidth);

// Scrolling waterfall: newest line at the top, history scrolling down.
//
// Threading: addLine() and draw() mutate shared state without locks; call
// both from the same (GUI) thread. The pipeline hands frames to the GUI
// thread anyway, so this costs nothing and keeps draw() allocation-free.
class WaterfallView {
public:
    // Texture dimensions: width = frequency bins across, height = history
    // lines. Non-positive dimensions yield an inert view (addLine/draw no-op).
    WaterfallView(int width, int height);

    // Frees the GL texture if draw() ever created one. Requires the GL
    // context that created it to still be current (same rule as any GL
    // resource); a never-drawn view is safely destroyed without a context.
    ~WaterfallView();

    // The destructor owns a GL texture handle, so copies would double-free.
    WaterfallView(const WaterfallView&) = delete;
    WaterfallView& operator=(const WaterfallView&) = delete;

    // Pushes one spectrum line into the CPU ring (newest at top, scrolls
    // down), resampling n bins to the texture width via mapLineToPixels.
    // Never touches GL, so it is headless-testable; the rows written since
    // the last draw() are uploaded lazily there.
    //
    // THE ROW IS STAMPED WITH THE MOMENT IT ARRIVED, read from nowSeconds()
    // below. That stamp is the whole basis of the elapsed-time strip: an age
    // taken from a per-row clock survives a stopped receiver, a retune, a
    // stalled source and a device that dropped out, none of which an age
    // divided out of an average line rate survives. It costs one double per
    // history row — 4 KB for the application's 512-row ring, against the two
    // megabytes of texture those rows are painted into.
    void addLine(const float* dbBins, int n, float dbMin, float dbMax);

    // The same, with the arrival time supplied on the caller's own monotonic
    // clock rather than read from this view's. It exists so the age math can
    // be exercised deterministically headless, and for a caller that already
    // timestamps its frames; mixing the two forms in one view mixes two
    // clocks and is not supported.
    //
    // A non-finite stamp, or one earlier than the row before it, is replaced
    // by the previous row's. Arrivals cannot be un-ordered after the fact,
    // and rowAtAge() below is a binary search that needs them ordered.
    void addLine(const float* dbBins, int n, float dbMin, float dbMax,
                 double atSeconds);

    // The monotonic clock the four-argument addLine() and the chrome draw
    // both read, in seconds from an arbitrary fixed epoch. Steady rather than
    // wall-clock: an age is a difference, and a difference across a system
    // clock adjustment is not one.
    static double nowSeconds() noexcept;

    // Full-span draw: forwards to the windowed draw with the whole texture
    // width visible ([u0, u1] = [0, 1]). Kept so existing callers and the
    // unzoomed display need no window bookkeeping.
    void draw(float width, float height);

    // Windowed draw: renders only the horizontal texture window [u0, u1]
    // (0..1 across the bins) stretched to width x height. Lazily creates
    // the GL texture on first call (needs a live GL context), then uploads
    // only the rows addLine() wrote since the previous draw, as single-row
    // glTexSubImage2D updates. The window arrives as plain texture-u
    // numbers from the zoom controller — no frequency-scale dependency —
    // and addLine() always stores the full span, so zooming back out never
    // loses history.
    //
    // Seam handling: the ring seam (newest row at the widget's top edge,
    // ages increasing downward) is drawn via uvRects() below — at most two
    // quads split exactly at the seam row, each quad's screen share equal
    // to its v share. On-screen this is identical to the previous
    // wrapped-v single-Image approach, but it also works for any u-window
    // and keeps every uv inside [0, 1]. Filtering is GL_NEAREST so the
    // newest and oldest rows never blend into each other at the seam.
    // A degenerate window (u0 >= u1 after clamping to [0, 1], or NaN)
    // draws no texture but still occupies the widget rectangle, so a bad
    // zoom state cannot shift the surrounding layout.
    void draw(float width, float height, double u0, double u1);

    // --- the framed face --------------------------------------------------
    //
    // WHAT THE PICTURE CANNOT KNOW ABOUT ITSELF. The ring holds colours and
    // per-row arrival stamps: it knows when each of its lines turned up, and
    // it does not know what the receiver is doing with them. So the ages the
    // time axis is drawn from are the view's own measurement, and the decode
    // line arrives from the caller.
    //
    // EVERY FIELD MAY BE ABSENT, AND ABSENT MEANS THE ELEMENT IS NOT DRAWN.
    // Nothing here is filled in with a plausible number.
    struct Chrome {
        // Measured waterfall scroll rate, in lines per second — one line per
        // spectrum frame the pipeline actually published, NOT per GUI frame.
        // Zero, negative or NaN means "not measured" and drops the SCROLL
        // clause from the foot line. Nothing else depends on it.
        //
        // IT IS NO LONGER WHAT THE TIME AXIS IS DRAWN FROM. It was: the strip
        // divided the history depth by this rate and labelled the result,
        // which is only true while lines arrive at a constant rate. A stopped
        // receiver, a retune, a stalled source or a device that dropped out
        // silently backdated every label below the gap — the axis said "40s
        // ago" over a row that was really ten minutes old. Ages are now read
        // from the arrival stamps addLine() records, so a gap pushes the
        // labels apart instead of being averaged away.
        float linesPerSecond = 0.0f;

        // What the receiver is doing with these lines — the foot line, e.g.
        // "1090 MHz ADS-B - DECODING". nullptr or empty draws no line; this
        // widget has no way to find out for itself and must not invent it.
        const char* decoding = nullptr;
    };

    // Windowed draw plus the bench chrome the design calls for: a recessed
    // bevelled frame, the elapsed-time strip down the left edge, the strength
    // key at the top right, and the scroll/decode lines at the foot.
    //
    // The picture is drawn FULL BLEED and the frame painted over its outer
    // edge — deliberately, and not merely to save two pixels. The horizontal
    // mapping of this widget is shared with the spectrum above it (the caller
    // hands both the same visible bin window, so a signal's trace peak and
    // its waterfall stripe land on the same screen column); insetting the
    // texture would shift every column by the frame width and quietly break
    // that alignment.
    //
    // The chrome emits no ImGui items — only draw-list geometry — so the
    // layout item this call leaves behind is still the same Dummy the plain
    // draw() leaves, and a caller's ImGui::IsItemHovered() after it still
    // asks about the waterfall.
    //
    // The strength key shows the ramp by SAMPLING waterfallColor() across the
    // bar, never a hand-picked set of stops: a key drawn from its own colour
    // table can drift away from the picture it explains, and this one cannot.
    // It is drawn only once a line has been mapped (hasRange() below) —
    // before that there is no floor/ceiling pair to label it with.
    //
    // WHAT THE KEY CLAIMS, AND HOW FAR DOWN THE PICTURE THE CLAIM REACHES.
    // The floor/ceiling pair is the range the NEWEST line was mapped through,
    // and rows are mapped once, when they are written: after the user moves
    // the Display sliders, the rows already in the ring keep the colours the
    // old range gave them. A key captioned for the whole picture would then
    // be right about the top row and wrong about everything beneath it. So
    // the key names the run of newest rows it actually covers — rangeRows()
    // below — and says so on its own plate whenever that run is shorter than
    // the history, and a rail is drawn across the picture at the row where
    // the mapping changed, so the boundary can be seen rather than imagined.
    //
    // THE TIME STRIP STOPS WHERE THE HISTORY STOPS. It spans filledRows() of
    // the texture's texHeight(), not the whole texture: until the ring has
    // wrapped once, the rows below that hold the constructor's pre-fill and
    // not a line this view ever received. Labelling them put "5s" beside a
    // band of nothing — an axis measuring a picture that is not there — so the
    // strip ends at the last row that was actually written, and the foot line
    // reports the history the view is holding rather than the history it has
    // room for.
    //
    // ITS LABELS ARE READ, NOT COMPUTED. Each one is placed at the row whose
    // stamp actually carries that age (rowAtAge()), so a stretch where no
    // lines arrived shows up as labels bunched against the band that spans
    // it, and the axis keeps counting up over a frozen picture instead of
    // pretending its top row is always "now".
    void draw(float width, float height, double u0, double u1, const Chrome& chrome);

    // Spacing of the elapsed-time labels, in seconds, for a strip spanning
    // `spanSeconds` with at most `maxLabels` of them: the smallest value from
    // the 1/2/5/10/15/20/30 s, 1/2/5/10/15/30 min, whole-hour ladder that is
    // at least spanSeconds / maxLabels. Pure and static so the choice is
    // testable without a GL context.
    //
    // Returns 0 for a non-positive/NaN span or maxLabels < 1, and never
    // returns a sub-second step: a waterfall whose whole history is under a
    // second gets no labels at all rather than ones rounded to "1s".
    static double timeLabelStep(double spanSeconds, int maxLabels);

    // Tick spacing for the strength key's dB scale: the smallest step from the
    // 1/2/5/10/20/25/50/100/200 dB ladder that keeps [dbFloor, dbCeiling] to
    // `maxIntervals` or fewer — EXCEPT that a step which would put only one
    // figure on the scale is rejected in favour of the coarsest ladder entry
    // that puts at least two there.
    //
    // THAT EXCEPTION IS THE WHOLE REASON THIS IS A SEPARATE, TESTABLE
    // FUNCTION. The ladder doubles at 50 -> 100, and the first tick lands on
    // the first multiple of the step at or above the floor, so a step can eat
    // most of the range before its first label and leave the second one past
    // the ceiling: -160 dB .. -5 dB on the default panel width chose 100 and
    // printed the single figure "-100". A one-figure scale is not a scale —
    // it names one colour and says nothing about the ones either side of it.
    // Forty-five of the 12,966 whole-decibel floor/ceiling pairs the Display
    // section can produce did exactly that.
    //
    // Returns 0 for a non-positive/NaN range or maxIntervals < 1, and also
    // when even a 1 dB step cannot place two figures inside the range (a span
    // under 1 dB — unreachable through the Display section, which holds the
    // two ends at least 10 dB apart, and drawn as no scale at all rather than
    // as a lone number). Pure and static so the choice is testable without a
    // GL context.
    static double dbLabelStep(double dbFloor, double dbCeiling, int maxIntervals);

    // One screen quad of a windowed draw: the vertical slice of the widget
    // it covers and the texture uv corners it samples. Plain floats, no GL
    // types, so the seam math is testable headless.
    struct UvRect {
        float y0Frac, y1Frac;  // widget-vertical span: 0 = top edge, 1 = bottom
        float u0, v0;          // uv of the quad's top-left corner
        float u1, v1;          // uv of the quad's bottom-right corner
    };

    // The uv computation behind draw(w, h, u0, u1), exposed static so tests
    // can pin seam handling without a GL context. Clamps [u0, u1] to
    // [0, 1], then writes the quads to issue for ring cursor `rowCursor` in
    // a texture of `height` rows:
    //   rowCursor == 0 -> 1 quad: v spans [0, 1], no seam inside the widget;
    //   rowCursor == c -> 2 quads: rows c..H-1 (v in [c/H, 1]) on top, then
    //                     rows 0..c-1 (v in [0, c/H]) below — newest row at
    //                     the top edge, seam exactly between the quads.
    // Invariant the tests rely on: each quad's screen span equals its v
    // span (y1Frac - y0Frac == v1 - v0), so every history row renders at
    // the same thickness. Returns the number of quads written (0, 1 or 2);
    // a degenerate window (u0 >= u1 after clamping, or NaN), height <= 0,
    // or a null `out` returns 0. An out-of-range rowCursor is wrapped into
    // [0, height) defensively.
    static int uvRects(int rowCursor, int height, double u0, double u1, UvRect out[2]);

    // --- Read-only introspection (headless-safe, never touches GL) ---

    int texWidth() const noexcept { return width_; }
    int texHeight() const noexcept { return height_; }

    // Texture row holding the newest line. Starts at 0; each addLine()
    // decrements it (wrapping), so rows cursor, cursor+1, ... cursor+H-1
    // (mod H) read newest-to-oldest — which is what the seam-split draw
    // (uvRects) shows top-to-bottom.
    int rowCursor() const noexcept { return cursor_; }

    // How many of the texHeight() rows hold a line this view actually
    // received, counting up from 0 and saturating at texHeight() once the ring
    // has wrapped. Rows cursor(), cursor()+1, ... cursor()+filledRows()-1
    // (mod H) are exactly the written ones, newest first.
    //
    // WHY THE VIEW COUNTS THIS ITSELF rather than taking a tally from the
    // caller: the ring's contents and this number have to share a lifetime.
    // The application's own line counter is never reset when the view is torn
    // down with its GL context and rebuilt (run() does that every time it is
    // called), so after a restart it would report a full history over a ring
    // that had just been re-filled with the empty colour.
    //
    // A line whose input was degenerate — a dropout, a null/short bin array —
    // still counts. It was received, and it is painted as the floor colour on
    // purpose so the gap scrolls through the picture like any other line.
    int filledRows() const noexcept { return filled_; }

    // Pointer to texture row `row`'s texWidth() pixels, or nullptr when the
    // row is out of [0, texHeight()) or the view is empty.
    const ImU32* rowPixels(int row) const noexcept;

    // --- how old the picture is, read from the rows themselves --------------

    // The ages of the retained history at `atSeconds` on the clock the rows
    // were stamped with. `valid` is false with no rows at all, and every age
    // is clamped at zero: a row cannot have arrived in the future, and a
    // caller passing a NaN clock reading gets zeroes rather than NaN labels.
    struct Ages {
        bool valid = false;   // false when nothing has been received
        int rows = 0;         // rows carrying a stamp (== filledRows())
        double newest = 0.0;  // seconds since the newest row arrived
        double oldest = 0.0;  // seconds since the oldest retained row arrived
        // The history the picture is HOLDING: oldest - newest. Not the same
        // as `oldest` once the source has stalled — then the picture holds
        // exactly what it held when the lines stopped, and it is all older.
        double span = 0.0;
    };
    Ages ages(double atSeconds) const noexcept;

    // Index of the newest retained row that is at least `ageSeconds` old at
    // `atSeconds`, counting 0 for the newest row and filledRows() - 1 for the
    // oldest, or -1 when no retained row is that old (or the age is NaN).
    //
    // This is what puts a time label beside the right row. Stamps decrease
    // monotonically from row 0 outward — addLine() enforces that — so it is a
    // binary search, not a scan, and a 512-row ring costs nine comparisons a
    // label.
    int rowAtAge(double atSeconds, double ageSeconds) const noexcept;

    // The age of one row, counting 0 for the newest and filledRows() - 1 for
    // the oldest, or NaN when that row holds no line this view received.
    // Clamped at zero for the same reason Ages is.
    //
    // The strip walks these to find the largest single-row jump in age — the
    // moment the lines stopped arriving — because that is the one place a
    // ladder of round ages cannot describe: every figure from a few seconds to
    // several minutes lands on the SAME row, and all but the first are dropped
    // as collisions. A picture with a gap in it would otherwise carry one
    // lonely label and nothing else.
    double rowAge(double atSeconds, int rowsFromNewest) const noexcept;

    // How many of the newest rows were mapped through the CURRENT dbFloor()/
    // dbCeiling() pair, counting 1 for the newest row alone. It is a run, not
    // a tally: it stops at the first row whose range differed, which is the
    // only row count the strength key can honestly claim.
    //
    // Equal to filledRows() whenever the whole picture shares one range —
    // which is the ordinary case, and the case in which the key is captioned
    // for the picture as a whole.
    int rangeRows() const noexcept { return std::min(rangeRows_, filled_); }

    // --- the dB range the picture is actually painted with ------------------
    //
    // The bounds of the most recent addLine(), recorded so the strength key
    // can state the floor and ceiling THIS PICTURE was mapped through rather
    // than a pair passed separately to the draw call and free to disagree
    // with it. hasRange() is false before the first line, and false again
    // after a line whose range was degenerate (dbMax <= dbMin, or NaN) — such
    // a line renders as the flat floor colour, so there is no range to name.
    // dbFloor()/dbCeiling() are then meaningless and must not be printed.
    bool hasRange() const noexcept { return hasRange_; }
    float dbFloor() const noexcept { return dbMin_; }
    float dbCeiling() const noexcept { return dbMax_; }

private:
    int width_ = 0;
    int height_ = 0;
    std::vector<ImU32> pixels_;  // row-major width_ x height_ CPU ring
    // When each ring row arrived, on nowSeconds()' clock. Parallel to the
    // pixel ring and read only for rows the ring has actually been handed
    // (the first filled_ of them, from cursor_ outward). One double a row:
    // 4 KB beside the 2 MB of pixels it dates.
    std::vector<double> times_;
    int cursor_ = 0;             // ring row of the newest line
    int filled_ = 0;             // rows ever written, capped at height_
    int pendingRows_ = 0;        // rows written since last upload, capped at height_
    unsigned int texture_ = 0;   // GLuint; 0 = not created yet
    float dbMin_ = 0.0f;         // bounds of the last addLine(), for the key
    float dbMax_ = 0.0f;
    bool hasRange_ = false;      // false until a line with a usable range
    int rangeRows_ = 0;          // newest rows sharing dbMin_/dbMax_; see rangeRows()
};

}  // namespace cascade::gui
