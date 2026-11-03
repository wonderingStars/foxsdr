// The instrument faces measured against the plugin kit's Workbench-Instruments artboard (tiles C1..C9) and against the rules the
// app's own faces keep (src/gui/instrument_face.hpp: no pretend controls; "no reading" is blank, never zero; nothing invented;
// instrument_*_math.hpp: the lamp and mask bits, the 2-degrees-a-dot bar, the 0.5 s feed). Every position is the specimen's own table
// (ui/specimens/instruments_specimen.slint): a tile at (x, y) frames its face with 1px, so the face's own origin is (x + 1, y + 1)
// and its columns are the artboard's numbers (12 px side padding, a 250 px pager LCD, the 270 x 122 paper, ...).
//
// What is exact and what is not. Boxes, bars, cells and lamp dots are rectangles on whole pixels, so their edges, fills and colours
// are asserted exactly (a pixel and its neighbour). Text is the embedded Inter / JetBrains Mono, asserted by INK in a region (never a
// glyph's position); the compass, its needle and the radial lamp are Path strokes and nested discs, asserted by area and by window,
// never by the exact pixel a hairline lands on. The pale LCD and paper are the theme's fg-soft ground with bg ink: "dark ink" is
// every channel under 60, which a dimmed ink (bg at 70 %) is not. An absence ("no phosphor on this face") is asserted as zero.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "render_harness.hpp"
#include "test_check.hpp"

#include "instruments_specimen.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace foxsdr::slinttest;

// counts that say what they were when they fail
#define CHECK_GT(a, b)                                                                                           \
    do {                                                                                                         \
        ++g_checksRun;                                                                                           \
        const long va_ = static_cast<long>(a), vb_ = static_cast<long>(b);                                       \
        if (!(va_ > vb_)) { ++g_checksFailed; std::printf("FAIL %s:%d  %s = %ld, wanted > %ld\n", __FILE__, __LINE__, #a, va_, vb_); } \
        else if (std::getenv("FOXSDR_INSTR_VERBOSE") != nullptr) { std::printf("ok   %s:%d  %ld > %ld   %s\n", __FILE__, __LINE__, va_, vb_, #a); } \
    } while (0)
#define CHECK_GE(a, b)                                                                                         \
    do {                                                                                                         \
        ++g_checksRun;                                                                                           \
        const long va_ = static_cast<long>(a), vb_ = static_cast<long>(b);                                       \
        if (!(va_ >= vb_)) { ++g_checksFailed; std::printf("FAIL %s:%d  %s = %ld, wanted >= %ld\n", __FILE__, __LINE__, #a, va_, vb_); } \
    } while (0)
#define CHECK_LE(a, b)                                                                                           \
    do {                                                                                                         \
        ++g_checksRun;                                                                                           \
        const long va_ = static_cast<long>(a), vb_ = static_cast<long>(b);                                       \
        if (!(va_ <= vb_)) { ++g_checksFailed; std::printf("FAIL %s:%d  %s = %ld, wanted <= %ld\n", __FILE__, __LINE__, #a, va_, vb_); } \
    } while (0)
#define CHECK_EQ(a, b)                                                                                           \
    do {                                                                                                         \
        ++g_checksRun;                                                                                           \
        const long va_ = static_cast<long>(a), vb_ = static_cast<long>(b);                                       \
        if (!(va_ == vb_)) { ++g_checksFailed; std::printf("FAIL %s:%d  %s = %ld, wanted %ld\n", __FILE__, __LINE__, #a, va_, vb_); } \
    } while (0)

namespace {

// ---- the Workbench filling's colours --------------------------------------------------------------------------------------
constexpr Rgb kBg = rgb(0x15171b);          // bg: the sheet
constexpr Rgb kPanel = rgb(0x1d2026);       // panel: the tile's ground
constexpr Rgb kField = rgb(0x0f1114);       // field: glass and cells
constexpr Rgb kLine = rgb(0x2b2f37);        // line: the tile frame, a glass border
constexpr Rgb kLineStrong = rgb(0x363b45);  // line-strong
constexpr Rgb kRing = rgb(0x434955);        // knob-ring and lamp-ring: a lamp's ring, a ghost cell's ink
constexpr Rgb kFg = rgb(0xe6e8ec);
constexpr Rgb kFgSoft = rgb(0xc9ced6);      // the pale ground of the LCD and the paper
constexpr Rgb kMuted = rgb(0x9aa3ad);
constexpr Rgb kDim = rgb(0x7d8694);
constexpr Rgb kFaint = rgb(0x5c6370);
constexpr Rgb kDigit = rgb(0xffb84d);       // the phosphor of every glass
constexpr Rgb kAccent = rgb(0xf5a524);
constexpr Rgb kAccentSoft = rgb(0xf5c66b);
constexpr Rgb kGreen = rgb(0x3ddc84);
constexpr Rgb kRed = rgb(0xff5c5c);
constexpr Rgb kLampOff = rgb(0x2b2f37);
constexpr Rgb kFocus = rgb(0x7ad7ff);
constexpr Rgb kDangerLine = rgb(0x6b2a2a);
constexpr Rgb kDangerBg = rgb(0x3a1d1d);
constexpr Rgb kCardErr = rgb(0x2a1717);
constexpr Rgb kDangerFg = rgb(0xffd4d4);
constexpr Rgb kChipWarnBg = rgb(0x3a2a0e);
constexpr Rgb kChipGoodFg = rgb(0x7fe6ad);
constexpr Rgb kKeyBg = rgb(0x262a32);
constexpr Rgb kTrack = rgb(0x2b2f37);       // slider-track: a read bar's empty part
constexpr Rgb kTip = rgb(0x262a32);         // menu: the tooltip's ground

// ---- the sheet -------------------------------------------------------------------------------------------------------------
struct Pos { int x, y; };
struct R { int x, y, w, h; };

constexpr int kSheetW = 1440, kSheetH = 1300;
// sheet 0: the live tiles and the empty tiles (the specimen's table)
constexpr Pos kGenL{8, 12}, kPagL{488, 12}, kTelL{968, 12}, kTonL{8, 200}, kVorL{488, 200}, kFaxL{968, 200}, kBcnL{8, 420}, kMetL{488, 420}, kWxL{968, 420};
constexpr Pos kGenE{8, 656}, kPagE{488, 656}, kTelE{968, 656}, kTonE{8, 844}, kVorE{488, 844}, kFaxE{968, 844}, kBcnE{8, 1064}, kMetE{488, 1064}, kWxE{968, 1064};
// a tile's outer height: the face's preferred height + 2 (the 1px frame)
constexpr int kHGenL = 161, kHGenE = 71, kHPag = 176, kHTel = 140, kHTon = 208, kHVor = 175, kHFax = 205, kHBcn = 224, kHMet = 119, kHWx = 183;

// a rectangle given in a tile's FACE coordinates, in the sheet's
R F(Pos tile, int x, int y, int w, int h) { return {tile.x + 1 + x, tile.y + 1 + y, w, h}; }
// the same inset by 4 on every side: clear of a box's border and of its rounded corners
R in4(R r) { return {r.x + 4, r.y + 4, r.w - 8, r.h - 8}; }

int cnt(const Frame& f, R r, Rgb c, int tol = 8) { return f.countNear(r.x, r.y, r.x + r.w, r.y + r.h, c, tol); }
int ink(const Frame& f, R r, Rgb background, int tol = 24) { return f.countInk(r.x, r.y, r.x + r.w, r.y + r.h, background, tol); }
// the first x in the region whose column holds a pixel near c, -1 when none
int firstX(const Frame& f, R r, Rgb c, int tol = 24) {
    for (int x = r.x; x < r.x + r.w; ++x) {
        for (int y = r.y; y < r.y + r.h; ++y) { if (f.near(x, y, c, tol)) { return x; } }
    }
    return -1;
}
// phosphor, amber, accent: warm hues (red leads, green is mid, blue is low) - what every live glass figure is drawn in
bool isWarm(Rgb c) { return c.r >= 200 && c.g >= 110 && c.g <= 215 && c.b <= 140 && c.r > c.g + 25; }
int warm(const Frame& f, R r) {
    int n = 0;
    for (int y = r.y; y < r.y + r.h; ++y) { for (int x = r.x; x < r.x + r.w; ++x) { if (isWarm(f.at(x, y))) { ++n; } } }
    return n;
}
// full-strength dark ink on a pale ground: every channel under 60 (a dimmed ink - bg at 70 % over the pale ground - is not)
int dark(const Frame& f, R r) {
    int n = 0;
    for (int y = r.y; y < r.y + r.h; ++y) {
        for (int x = r.x; x < r.x + r.w; ++x) { const Rgb c = f.at(x, y); if (c.r < 60 && c.g < 60 && c.b < 60) { ++n; } }
    }
    return n;
}
// a red lamp glass: red leads, green and blue are low and close (the radial lamp is nested discs from a dark rust to a pale rose)
int reddish(const Frame& f, R r) {
    int n = 0;
    for (int y = r.y; y < r.y + r.h; ++y) {
        for (int x = r.x; x < r.x + r.w; ++x) { const Rgb c = f.at(x, y); if (c.r >= 150 && c.g <= 140 && c.b <= 140 && c.r > c.g + 50) { ++n; } }
    }
    return n;
}
// pixels where two frames differ by more than tol in a region
int differing(const Frame& a, const Frame& b, R r, int tol = 8) {
    int n = 0;
    for (int y = r.y; y < r.y + r.h; ++y) {
        for (int x = r.x; x < r.x + r.w; ++x) {
            const Rgb p = a.at(x, y), q = b.at(x, y);
            if (std::abs(int(p.r) - int(q.r)) > tol || std::abs(int(p.g) - int(q.g)) > tol || std::abs(int(p.b) - int(q.b)) > tol) { ++n; }
        }
    }
    return n;
}
// A box with a 1px border: its four edges' mid points are the border, the pixel outside each is `outside`, the corner region is `fill`.
void box(const Frame& f, R r, Rgb border, Rgb fill, Rgb outside) {
    const int mx = r.x + r.w / 2, my = r.y + r.h / 2;
    CHECK(f.near(r.x, my, border, 6) && f.near(r.x + r.w - 1, my, border, 6));
    CHECK(f.near(mx, r.y, border, 6) && f.near(mx, r.y + r.h - 1, border, 6));
    CHECK(f.near(r.x - 1, my, outside, 6) && f.near(r.x + r.w, my, outside, 6));
    CHECK(f.near(mx, r.y - 1, outside, 6) && f.near(mx, r.y + r.h, outside, 6));
    CHECK(f.near(r.x + 3, r.y + 3, fill, 6));
}
// only the left edge, the top and bottom edges near it and the fill, for a box whose right edge depends on its text
void boxLeft(const Frame& f, R r, Rgb border, Rgb fill, Rgb outside) {
    const int my = r.y + r.h / 2;
    CHECK(f.near(r.x, my, border, 6) && f.near(r.x - 1, my, outside, 6));
    CHECK(f.near(r.x + 10, r.y, border, 6) && f.near(r.x + 10, r.y + r.h - 1, border, 6));
    CHECK(f.near(r.x + 3, r.y + 3, fill, 6));
}
bool at(const Frame& f, R p, Rgb c, int tol) { return f.near(p.x, p.y, c, tol); }
// the leftmost column of the region that holds ink, -1 for none
int leftInk(const Frame& f, R r, Rgb background, int tol = 24) {
    for (int x = r.x; x < r.x + r.w; ++x) { if (f.countInk(x, r.y, x + 1, r.y + r.h, background, tol) > 0) { return x; } }
    return -1;
}

// a tile's height measured on the sheet: down the frame's left edge, which is the frame colour all the way between the corners
int tileHeight(const Frame& f, int x, int y) {
    int yy = y + 10;
    while (yy < static_cast<int>(f.height) && f.near(x, yy, kLine, 6)) { ++yy; }
    return (yy - y) + 8;
}

// ---- states ---------------------------------------------------------------------------------------------------------------
std::shared_ptr<slint::Model<float>> vals(std::initializer_list<float> v) {
    std::vector<float> a(v);
    a.resize(8, 0.0f);
    return std::make_shared<slint::VectorModel<float>>(std::move(a));
}
std::shared_ptr<slint::Model<slint::SharedString>> texts(std::initializer_list<const char*> t) {
    std::vector<slint::SharedString> a;
    for (const char* s : t) { a.emplace_back(s); }
    a.resize(8);
    return std::make_shared<slint::VectorModel<slint::SharedString>>(std::move(a));
}

// a picture: red above, blue below
slint::Image twoHalves(int w, int h) {
    slint::SharedPixelBuffer<slint::Rgb8Pixel> buf(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            buf.begin()[static_cast<std::size_t>(y) * w + x] = y < h / 2 ? slint::Rgb8Pixel{255, 0, 0} : slint::Rgb8Pixel{0, 0, 255};
        }
    }
    return slint::Image(buf);
}

slint::Color col(unsigned r, unsigned g, unsigned b) {
    return slint::Color::from_rgb_uint8(static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g), static_cast<std::uint8_t>(b));
}

Frame shot(InstrumentsSpecimen& ui, std::uint32_t w = kSheetW, std::uint32_t h = kSheetH) { return render(ui.window(), w, h); }

// ===== the sheet: 18 tiles, none over another, every face inside its frame ======================================================
void checkSheet(const Frame& f) {
    CHECK(f.width == kSheetW && f.height == kSheetH);
    CHECK(f.near(1435, 5, kBg) && f.near(4, 1290, kBg));
    struct T { Pos at; int h; };
    const std::array<T, 18> tiles = {{{kGenL, kHGenL}, {kPagL, kHPag}, {kTelL, kHTel}, {kTonL, kHTon}, {kVorL, kHVor}, {kFaxL, kHFax}, {kBcnL, kHBcn}, {kMetL, kHMet}, {kWxL, kHWx},
                                      {kGenE, kHGenE}, {kPagE, kHPag}, {kTelE, kHTel}, {kTonE, kHTon}, {kVorE, kHVor}, {kFaxE, kHFax}, {kBcnE, kHBcn}, {kMetE, kHMet}, {kWxE, kHWx}}};
    for (const T& t : tiles) {
        const int mid = t.at.y + t.h / 2;
        // the frame: the artboard's .tile (472 wide, a 1px line border, the panel ground inside), the sheet's ground outside
        CHECK(f.near(t.at.x, mid, kLine, 6) && f.near(t.at.x + 471, mid, kLine, 6));
        CHECK(f.near(t.at.x - 1, mid, kBg, 6) && (t.at.x + 472 >= kSheetW || f.near(t.at.x + 472, mid, kBg, 6)));
        CHECK(f.near(t.at.x + 100, t.at.y, kLine, 6) && f.near(t.at.x + 100, t.at.y + t.h - 1, kLine, 6));
        CHECK(f.near(t.at.x + 100, t.at.y - 1, kBg, 6) && f.near(t.at.x + 100, t.at.y + t.h, kBg, 6));
        CHECK(f.near(t.at.x + 5, t.at.y + 5, kPanel, 6));
        CHECK_NEAR(tileHeight(f, t.at.x, t.at.y), t.h, 1.0);
        // nothing of the face is drawn below its frame: the ground under it, down to the next row, is the sheet's
        const int below = t.at.y + t.h;
        int nextTop = kSheetH;
        for (const T& o : tiles) { if (o.at.y > t.at.y && o.at.y < nextTop && o.at.x == t.at.x) { nextTop = o.at.y; } }
        CHECK_EQ(ink(f, {t.at.x, below, 472, nextTop - below}, kBg, 6), 0);
    }
    // the gutters between the columns
    CHECK_EQ(ink(f, {480, 0, 8, kSheetH}, kBg, 6), 0);
    CHECK_EQ(ink(f, {960, 0, 8, kSheetH}, kBg, 6), 0);
}

// ===== C1 GENERIC =================================================================================================================
void checkGeneric(const Frame& f) {
    const Pos t = kGenL;
    // the title (13 px / 600, fg) on the left, the four lamps on the right in the order ALERT, NEW, LOCK, BATT
    CHECK_GT(cnt(f, F(t, 12, 10, 190, 17), kFg, 40), 25);
    const R lamps = F(t, 190, 11, 268, 15);
    CHECK_GT(cnt(f, lamps, kRed, 24), 25);
    CHECK_GT(cnt(f, lamps, kAccent, 24), 25);
    CHECK_GT(cnt(f, lamps, kGreen, 24), 25);
    CHECK_GT(cnt(f, lamps, kRing, 8), 12);
    {
        const int xr = firstX(f, lamps, kRed), xa = firstX(f, lamps, kAccent), xg = firstX(f, lamps, kGreen);
        CHECK(xr > 0 && xa > xr && xg > xa);
    }
    // the glass: three texts then three figures, three to a row, 143 px wide (the last 144), 24 high, labelled by slot number
    const std::array<Pos, 6> cell = {{{12, 52}, {163, 52}, {314, 52}, {12, 101}, {163, 101}, {314, 101}}};
    for (int i = 0; i < 6; ++i) {
        const int w = (i % 3 == 2) ? 144 : 143;
        box(f, F(t, cell[i].x, cell[i].y, w, 24), kLine, kField, kPanel);
        CHECK_GT(cnt(f, F(t, cell[i].x + 4, cell[i].y + 4, w - 8, 16), kDigit, 40), 8);              // the slot's own words
        CHECK_GT(ink(f, F(t, cell[i].x, cell[i].y - 17, 40, 13), kPanel), 3);                         // its cap (T0.. V0..)
        CHECK_GT(cnt(f, F(t, cell[i].x, cell[i].y - 17, 40, 13), kMuted, 40), 3);
    }
    // figures sit on the right of their glass, words on the left
    CHECK_EQ(ink(f, F(t, 16, 105, 40, 16), kField), 0);                                                // V0 "21.4": nothing on the left ...
    CHECK_GT(ink(f, F(t, 100, 105, 50, 16), kField), 8);                                               // ... the figure on the right
    CHECK_GT(ink(f, F(t, 16, 56, 50, 16), kField), 8);                                                 // T0 "Bravo": the word on the left ...
    CHECK_EQ(ink(f, F(t, 105, 56, 45, 16), kField), 0);                                                // ... nothing on the right
    // EVENT 12 below the glass, and no fourth row of glass (the empty slots are not drawn)
    CHECK_GT(cnt(f, F(t, 12, 133, 90, 16), kAccentSoft, 40), 20);
    CHECK_EQ(cnt(f, F(t, 12, 133, 446, 20), kLine, 6), 0);
}
void checkGenericEmpty(const Frame& f) {
    const Pos t = kGenE;
    CHECK_GT(cnt(f, F(t, 12, 10, 190, 17), kFg, 40), 25);
    // one glass across the face with the sheet's words in the muted ink; no figure, no phosphor, every lamp dark
    box(f, F(t, 12, 35, 446, 24), kLine, kField, kPanel);
    CHECK_GT(cnt(f, F(t, 13, 36, 444, 22), kMuted, 40), 25);
    CHECK_EQ(warm(f, F(t, 0, 0, 470, 69)), 0);
    const R lamps = F(t, 190, 11, 268, 15);
    CHECK_EQ(cnt(f, lamps, kRed, 24) + cnt(f, lamps, kAccent, 24) + cnt(f, lamps, kGreen, 24), 0);
    CHECK_GT(cnt(f, lamps, kRing, 8), 40);
}

// ===== C2 PAGER ===================================================================================================================
void checkPager(const Frame& f) {
    const Pos t = kPagL;
    // the LCD: 250 x 78 glass, the message wrapped at twenty columns (144 px of 12 px mono): three lines, the fourth blank
    box(f, F(t, 12, 8, 250, 78), kLine, kField, kPanel);
    CHECK_GT(cnt(f, F(t, 21, 12, 144, 48), kDigit, 40), 150);
    CHECK_GT(ink(f, F(t, 21, 12, 144, 16), kField), 40);
    CHECK_GT(ink(f, F(t, 21, 28, 144, 16), kField), 40);
    CHECK_GT(ink(f, F(t, 21, 44, 144, 16), kField), 40);
    CHECK_EQ(ink(f, F(t, 168, 12, 88, 66), kField), 0);               // nothing past the twentieth column
    CHECK_EQ(ink(f, F(t, 21, 61, 144, 16), kField), 0);               // the fourth row is blank
    // the time (16 px / 600 phosphor), the envelope and the chip "2 unread" on the right of the column
    CHECK_GT(cnt(f, F(t, 12, 92, 70, 21), kDigit, 40), 40);
    CHECK_GT(cnt(f, F(t, 170, 92, 92, 21), kChipWarnBg, 4), 100);
    CHECK_GT(ink(f, F(t, 150, 92, 40, 21), kPanel), 30);
    CHECK_GT(cnt(f, F(t, 150, 92, 40, 21), kFgSoft, 40), 6);
    // the maker's plate under it: a 1px line-strong frame, the engraved words inside
    CHECK(at(f, F(t, 12, 129, 1, 1), kLineStrong, 6) && at(f, F(t, 261, 129, 1, 1), kLineStrong, 6));
    CHECK(at(f, F(t, 100, 119, 1, 1), kLineStrong, 6) && at(f, F(t, 100, 139, 1, 1), kLineStrong, 6));
    CHECK_GT(cnt(f, F(t, 16, 121, 242, 17), kMuted, 40), 40);
    // the right column: ALERT off, NEW amber on the first row; SYNC green, BATT off on the second
    const R row1 = F(t, 274, 8, 184, 13), row2 = F(t, 274, 27, 184, 13);
    CHECK_GT(cnt(f, row1, kAccent, 24), 25);
    CHECK_EQ(cnt(f, row1, kRed, 24) + cnt(f, row1, kGreen, 24), 0);
    CHECK_GT(cnt(f, row2, kGreen, 24), 25);
    CHECK_EQ(cnt(f, row2, kAccent, 24) + cnt(f, row2, kRed, 24), 0);
    CHECK_GT(cnt(f, row1, kRing, 8), 10);                              // ALERT's ring
    CHECK_GT(cnt(f, row2, kRing, 8), 10);                              // BATT's ring
    // the capcode: seven cells 20 x 28 on a 22 px pitch, each holding a figure; the unread count: two cells
    CHECK_GT(cnt(f, F(t, 274, 46, 100, 13), kMuted, 40), 20);
    for (int i = 0; i < 7; ++i) {
        box(f, F(t, 274 + 22 * i, 65, 20, 28), kLine, kField, kPanel);
        CHECK_GT(cnt(f, F(t, 275 + 22 * i, 66, 18, 26), kDigit, 40), 8);
    }
    CHECK_EQ(ink(f, F(t, 274 + 7 * 22, 65, 8, 28), kPanel), 0);       // and no eighth
    for (int i = 0; i < 2; ++i) {
        box(f, F(t, 274 + 22 * i, 116, 20, 28), kLine, kField, kPanel);
        CHECK_GT(cnt(f, F(t, 275 + 22 * i, 117, 18, 26), kDigit, 40), 8);
    }
    // TYPE and RECEIVED: glass of the words' own width, 24 high, side by side after the counter
    boxLeft(f, F(t, 328, 116, 54, 24), kLine, kField, kPanel);
    CHECK_GT(cnt(f, F(t, 330, 118, 50, 20), kDigit, 40), 12);
    CHECK_GT(cnt(f, F(t, 274, 99, 190, 13), kMuted, 40), 30);         // UNREAD TYPE RECEIVED
    CHECK_GT(cnt(f, F(t, 274, 150, 100, 16), kAccentSoft, 40), 20);   // EVENT 18
}
void checkPagerEmpty(const Frame& f) {
    const Pos t = kPagE;
    // the glass is still there but holds nothing; no phosphor anywhere on the face; every cell unlit (the ghost ground)
    box(f, F(t, 12, 8, 250, 78), kLine, kField, kPanel);
    CHECK_EQ(ink(f, F(t, 16, 12, 240, 66), kField), 0);
    CHECK_EQ(warm(f, F(t, 0, 0, 470, 174)), 0);
    CHECK_EQ(cnt(f, F(t, 150, 92, 112, 21), kChipWarnBg, 4), 0);       // no chip: "0 unread" would be a figure
    CHECK_EQ(cnt(f, F(t, 225, 92, 35, 21), kFgSoft, 8), 0);            // the envelope is dim ...
    CHECK_GT(ink(f, F(t, 225, 92, 35, 21), kPanel), 15);               // ... but it is there
    for (int i = 0; i < 7; ++i) {
        const R c = F(t, 274 + 22 * i, 65, 20, 28);
        CHECK(f.near(c.x + 3, c.y + 3, kBg, 6) && f.near(c.x, c.y + 14, kLine, 6));
        CHECK_EQ(ink(f, {c.x + 3, c.y + 3, 14, 22}, kBg, 12), 0);
    }
    for (int i = 0; i < 2; ++i) {
        const R c = F(t, 274 + 22 * i, 116, 20, 28);
        CHECK(f.near(c.x + 3, c.y + 3, kBg, 6));
        CHECK_EQ(ink(f, {c.x + 3, c.y + 3, 14, 22}, kBg, 12), 0);
    }
    // the sheet's words, engraved faint where the event number would be
    CHECK_GT(cnt(f, F(t, 274, 150, 184, 16), kFaint, 40), 20);
    CHECK_EQ(cnt(f, F(t, 274, 8, 184, 32), kAccent, 24) + cnt(f, F(t, 274, 8, 184, 32), kGreen, 24) + cnt(f, F(t, 274, 8, 184, 32), kRed, 24), 0);
    CHECK_GT(cnt(f, F(t, 274, 8, 184, 32), kRing, 8), 40);
}

// ===== C3 TELEPRINTER =============================================================================================================
void checkTeleprinter(const Frame& f) {
    const Pos t = kTelL;
    // the paper: 270 x 122 on the pale ground, three pixels of radius, dark print on it
    const R paper = F(t, 12, 8, 270, 122);
    CHECK(f.near(paper.x + 135, paper.y, kFgSoft, 10) && f.near(paper.x + 135, paper.y - 1, kPanel, 6));
    CHECK(f.near(paper.x, paper.y + 61, kFgSoft, 10) && f.near(paper.x - 1, paper.y + 61, kPanel, 6));
    CHECK(f.near(paper.x + 269, paper.y + 61, kFgSoft, 10) && f.near(paper.x + 270, paper.y + 61, kPanel, 6));
    CHECK(f.near(paper.x + 135, paper.y + 121, kFgSoft, 10) && f.near(paper.x + 135, paper.y + 122, kPanel, 6));
    CHECK_GT(dark(f, in4(paper)), 120);                              // the heading and the newest line, full ink
    CHECK_GT(ink(f, in4(paper), kFgSoft, 30), 300);                   // the older line is fainter but it is ink
    CHECK_GT(ink(f, F(t, 22, 28, 250, 16), kFgSoft, 30), 20);         // the previous print, at the top, in its fainter ink ...
    CHECK_EQ(dark(f, F(t, 22, 28, 250, 12)), 0);                      // ... which is not full strength
    // the status lamps, two to a row: PAPER READY on the first, NEW ALERT on the second
    const R row1 = F(t, 294, 29, 164, 13), row2 = F(t, 294, 48, 164, 13);
    CHECK_GT(cnt(f, row1, kGreen, 24), 40);
    CHECK_EQ(cnt(f, row1, kAccent, 24) + cnt(f, row1, kRed, 24), 0);
    CHECK_GT(cnt(f, row2, kAccent, 24), 25);
    CHECK_GT(cnt(f, row2, kRed, 24), 25);
    CHECK_EQ(cnt(f, row2, kGreen, 24), 0);
    CHECK(firstX(f, row2, kAccent) > 0 && firstX(f, row2, kRed) > firstX(f, row2, kAccent));
    CHECK_GT(cnt(f, F(t, 294, 8, 164, 13), kMuted, 40), 20);          // the caption STATUS
    CHECK_GT(cnt(f, F(t, 294, 69, 164, 13), kMuted, 40), 25);         // MESSAGES
    // the counter: four cells holding 0042
    for (int i = 0; i < 4; ++i) {
        box(f, F(t, 294 + 22 * i, 90, 20, 28), kLine, kField, kPanel);
        CHECK_GT(cnt(f, F(t, 295 + 22 * i, 91, 18, 26), kDigit, 40), 8);
    }
    CHECK_EQ(ink(f, F(t, 294 + 4 * 22, 90, 8, 28), kPanel), 0);
}
void checkTeleprinterEmpty(const Frame& f) {
    const Pos t = kTelE;
    // a blank leader and the sheet's words on it, in a dimmed ink: nothing printed, so no full-strength print
    const R paper = F(t, 12, 8, 270, 122);
    CHECK(f.near(paper.x + 135, paper.y, kFgSoft, 10) && f.near(paper.x + 135, paper.y + 121, kFgSoft, 10));
    CHECK_EQ(dark(f, in4(paper)), 0);
    CHECK_GT(ink(f, in4(paper), kFgSoft, 30), 25);
    // every lamp dark, the counter dashes in unlit cells (never 0000)
    const R lamps = F(t, 294, 29, 164, 32);
    CHECK_EQ(cnt(f, lamps, kGreen, 24) + cnt(f, lamps, kAccent, 24) + cnt(f, lamps, kRed, 24), 0);
    CHECK_GT(cnt(f, lamps, kRing, 8), 40);
    for (int i = 0; i < 4; ++i) {
        const R c = F(t, 294 + 22 * i, 90, 20, 28);
        CHECK(f.near(c.x + 3, c.y + 3, kBg, 6) && f.near(c.x, c.y + 14, kLine, 6));
        CHECK_GT(cnt(f, {c.x + 2, c.y + 2, 16, 24}, kRing, 24), 3);   // the dash, in the ghost ink
    }
    CHECK_EQ(warm(f, F(t, 0, 0, 470, 138)), 0);
}

// ===== C4 TONE ALERT ==============================================================================================================
// the read bars take the app's engraved scale: 250 to 3000 Hz (instrument_tone_alert_math.hpp kScaleLoHz, kScaleHiHz)
double toneFrac(double hz) { return std::clamp((hz - 250.0) / (3000.0 - 250.0), 0.0, 1.0); }
void checkTone(const Frame& f) {
    const Pos t = kTonL;
    // the radial lamp, 64 x 64, lit (the clock is in the first half of a 250 ms period), and its two small lamps beneath
    CHECK_GT(reddish(f, F(t, 22, 8, 64, 64)), 1000);
    CHECK_GT(reddish(f, F(t, 22, 8, 64, 18)), 100);                    // the glass reaches its top edge
    CHECK_GT(cnt(f, F(t, 12, 78, 84, 13), kRed, 24), 25);
    CHECK_GT(cnt(f, F(t, 12, 97, 84, 13), kAccent, 24), 25);
    CHECK_EQ(cnt(f, F(t, 12, 78, 84, 13), kAccent, 24), 0);
    CHECK_EQ(cnt(f, F(t, 12, 97, 84, 13), kRed, 24), 0);
    // Tone A and Tone B: the cap on the left, the reading on the right in accent-soft mono; the bar under each, 350 wide
    CHECK_GT(cnt(f, F(t, 108, 8, 90, 24), kMuted, 40), 20);
    CHECK_GT(cnt(f, F(t, 108, 50, 90, 24), kMuted, 40), 20);
    CHECK_GT(cnt(f, F(t, 360, 8, 98, 24), kAccentSoft, 40), 30);
    CHECK_GT(cnt(f, F(t, 360, 50, 98, 24), kAccentSoft, 40), 30);
    CHECK_EQ(ink(f, F(t, 215, 8, 140, 24), kPanel), 0);                // nothing between the cap and the figure
    for (int row = 0; row < 2; ++row) {
        const double hz = row == 0 ? 853.2 : 1273.9;
        const int y = row == 0 ? 36 : 78;
        const int fillEnd = 108 + static_cast<int>(std::lround(350 * toneFrac(hz)));
        CHECK(at(f, F(t, 108 + 2, y + 3, 1, 1), kAccent, 12));           // the fill from the left stop ...
        CHECK(at(f, F(t, fillEnd - 12, y + 3, 1, 1), kAccent, 12));      // ... up to the index ...
        CHECK(at(f, F(t, fillEnd + 12, y + 3, 1, 1), kTrack, 8));        // ... and the empty track after it
        CHECK(at(f, F(t, 108 + 340, y + 3, 1, 1), kTrack, 8));
        // the 16 px knob at the index (the slider's own look: a light disc)
        CHECK_GT(cnt(f, F(t, fillEnd - 9, y - 6, 18, 18), kFg, 8), 60);
        CHECK_LE(ink(f, F(t, 108, y - 6, 350, 3), kPanel), 40);          // only the knob rises above the track
    }
    // the four glass boxes (172 wide, 24 high) in two rows, and what each says
    box(f, F(t, 108, 107, 172, 24), kLine, kField, kPanel);
    box(f, F(t, 286, 107, 172, 24), kLine, kField, kPanel);
    box(f, F(t, 108, 152, 172, 24), kLine, kField, kPanel);
    box(f, F(t, 286, 152, 172, 24), kLine, kField, kPanel);
    CHECK_GT(cnt(f, F(t, 109, 108, 170, 22), kDigit, 40), 30);          // the code
    CHECK_GT(cnt(f, F(t, 287, 108, 170, 22), kDigit, 40), 12);          // the table and group
    CHECK_GT(cnt(f, F(t, 109, 153, 170, 22), kChipGoodFg, 40), 25);     // MATCHED, in the good colour
    CHECK_GT(cnt(f, F(t, 287, 153, 170, 22), kDigit, 40), 20);          // the two seconds
    CHECK_GT(cnt(f, F(t, 108, 92, 120, 13), kMuted, 40), 10);
    CHECK_GT(cnt(f, F(t, 108, 137, 120, 13), kMuted, 40), 10);
    CHECK_GT(cnt(f, F(t, 12, 184, 90, 16), kAccentSoft, 40), 15);       // EVENT 4
}
void checkToneEmpty(const Frame& f) {
    const Pos t = kTonE;
    // the lens is dark, the small lamps are dark; the bars are bare tracks with no index; "--", NO PAGE; no event row
    CHECK_EQ(reddish(f, F(t, 22, 8, 64, 64)), 0);
    CHECK(at(f, F(t, 54, 40, 1, 1), kLampOff, 8));
    CHECK_EQ(cnt(f, F(t, 12, 78, 84, 32), kRed, 24) + cnt(f, F(t, 12, 78, 84, 32), kAccent, 24), 0);
    CHECK_GT(cnt(f, F(t, 12, 78, 84, 32), kRing, 8), 20);
    for (int row = 0; row < 2; ++row) {
        const int y = row == 0 ? 36 : 78;
        CHECK(at(f, F(t, 120, y + 3, 1, 1), kTrack, 8) && at(f, F(t, 300, y + 3, 1, 1), kTrack, 8));
        CHECK_EQ(cnt(f, F(t, 100, y - 6, 366, 18), kAccent, 24) + cnt(f, F(t, 100, y - 6, 366, 18), kFg, 8), 0);
        CHECK_GT(ink(f, F(t, 360, y - 28, 98, 24), kPanel), 6);          // "--" at the right ...
        CHECK_EQ(warm(f, F(t, 360, y - 28, 98, 24)), 0);                 // ... in the dim ink, not the amber of a reading
    }
    CHECK_GT(cnt(f, F(t, 109, 108, 170, 22), kFaint, 40), 15);           // NO PAGE
    CHECK_EQ(cnt(f, F(t, 109, 108, 170, 22), kDigit, 40), 0);
    CHECK_EQ(ink(f, in4(F(t, 108, 152, 172, 24)), kField), 0);           // no result
    CHECK_GT(ink(f, F(t, 287, 153, 170, 22), kField), 6);                // seconds: --
    CHECK_EQ(warm(f, F(t, 0, 0, 470, 206)), 0);
}

// ===== C5 VOR =====================================================================================================================
// the course solution (instrument_nav_bearing_math.hpp solveCourse): where the bar's marker sits, and whether TO or FROM
struct Solved { bool to; double bar; };
Solved solve(double radial, double course) {
    const auto w180 = [](double d) { d = std::fmod(d, 360.0); if (d < 0) { d += 360.0; } return d > 180.0 ? d - 360.0 : d; };
    const double d = w180(radial - course);
    Solved s{};
    if (std::fabs(d) <= 90.0) { s.to = false; s.bar = -d / 10.0; } else { s.to = true; s.bar = w180(radial - course - 180.0) / 10.0; }
    s.bar = std::clamp(s.bar, -1.0, 1.0);
    return s;
}
// the card, the knob, the needle and the course line (course 090 on radial 274 by default)
void checkVorDial(const Frame& f, const Pos t, int course, int radial, bool needle) {
    // the compass: a 136 px field disc with a line-strong ring in the 150 px box at (10, 6); the amber index at its top
    CHECK_GT(cnt(f, F(t, 16, 50, 4, 62), kLineStrong, 30), 20);         // the ring's left edge ...
    CHECK_GT(cnt(f, F(t, 150, 50, 4, 62), kLineStrong, 30), 20);        // ... and its right
    CHECK(at(f, F(t, 85, 56, 1, 1), kField, 6) && at(f, F(t, 85, 14, 1, 1), kAccent, 30));
    CHECK_EQ(ink(f, F(t, 0, 0, 10, 160), kPanel), 0);
    CHECK_EQ(ink(f, F(t, 161, 0, 6, 160), kPanel), 0);
    CHECK_GT(ink(f, F(t, 78, 25, 14, 13), kField), 8);                   // the card's N
    // the knob in its middle: 40 px, a 2 px focus-coloured ring, the course on it in fg
    CHECK(at(f, F(t, 65, 81, 1, 1), kFocus, 24) && at(f, F(t, 104, 81, 1, 1), kFocus, 24));
    CHECK(at(f, F(t, 85, 62, 1, 1), kFocus, 24) && at(f, F(t, 85, 99, 1, 1), kFocus, 24));
    CHECK(at(f, F(t, 75, 70, 1, 1), kKeyBg, 6));
    CHECK_GT(cnt(f, F(t, 72, 72, 26, 18), kFg, 40), 12);
    // the bearing needle through the card, toward the radial and away from it, in amber
    const double rad = radial * 3.14159265358979 / 180.0;
    for (const double r : {35.0, -35.0}) {
        const int px = 85 + static_cast<int>(std::lround(r * std::sin(rad))), py = 81 - static_cast<int>(std::lround(r * std::cos(rad)));
        const int n = warm(f, F(t, px - 4, py - 4, 9, 9));
        if (needle) { CHECK_GT(n, 4); } else { CHECK_EQ(n, 0); }
    }
    // the selected course as a dashed line in the focus colour from the middle toward the course
    const double crs = course * 3.14159265358979 / 180.0;
    const int px = 85 + static_cast<int>(std::lround(32.0 * std::sin(crs))), py = 81 - static_cast<int>(std::lround(32.0 * std::cos(crs)));
    CHECK_GT(ink(f, F(t, px - 5, py - 5, 11, 11), kField, 40), 3);
}
void checkVorBar(const Frame& f, int course, int radial) {
    const Pos t = kVorL;
    const Solved s = solve(radial, course);
    const int centre = 220 + static_cast<int>(std::lround(s.bar * 48.0));     // the marker's middle, in face coordinates
    CHECK_GE(cnt(f, F(t, centre - 6, 49, 13, 14), kAccent, 12), 30);          // 3 x 12 of amber
    CHECK(at(f, F(t, centre, 56, 1, 1), kAccent, 12));
    CHECK_LE(cnt(f, F(t, 169, 48, 102, 16), kAccent, 12), 40);                // and only that one
    // TO or FROM lit (green), the other dark
    const R to = F(t, 168, 107, 32, 13), from = F(t, 202, 107, 70, 13);
    CHECK((cnt(f, to, kGreen, 24) > 25) == s.to);
    CHECK((cnt(f, from, kGreen, 24) > 25) == !s.to);
    CHECK_GT(cnt(f, s.to ? from : to, kRing, 8), 10);
}
void checkVorLive(const Frame& f) {
    const Pos t = kVorL;
    checkVorDial(f, t, 90, 274, true);
    // the gauge column (104 wide at x 168): OBS COURSE and its three digits, the 18 px bar, the note, the lamps
    CHECK_GT(cnt(f, F(t, 168, 6, 104, 13), kMuted, 40), 20);
    CHECK_GT(cnt(f, F(t, 168, 25, 104, 16), kAccentSoft, 40), 12);
    box(f, F(t, 168, 47, 104, 18), kLineStrong, kField, kPanel);
    CHECK(at(f, F(t, 220, 56, 1, 1), kFocus, 12) && at(f, F(t, 219, 56, 1, 1), kFocus, 12));   // the centre index
    CHECK_GE(cnt(f, F(t, 169, 48, 102, 16), kFocus, 12), 30);
    CHECK_LE(cnt(f, F(t, 169, 48, 102, 16), kFocus, 12), 40);
    CHECK_GT(cnt(f, F(t, 168, 71, 104, 30), kDim, 40), 20);              // "dot = 2 deg - full scale 10", over two lines
    checkVorBar(f, 90, 274);
    // LOCK and NEW lit at the foot of the column; the NO SIGNAL flag is not there
    const R foot = F(t, 168, 148, 104, 13);
    CHECK_GT(cnt(f, foot, kGreen, 24), 25);
    CHECK_GT(cnt(f, foot, kAccent, 24), 25);
    CHECK(firstX(f, foot, kGreen) < firstX(f, foot, kAccent));
    CHECK_EQ(ink(f, F(t, 168, 126, 104, 16), kPanel), 0);
    // the readout grid: label column in dim mono, the values in accent-soft (radial, ident) and fg (the rest), a 17 px pitch
    CHECK_GT(cnt(f, F(t, 280, 6, 80, 14), kDim, 40), 15);
    CHECK_GT(cnt(f, F(t, 362, 6, 100, 14), kAccentSoft, 40), 12);
    CHECK_GT(cnt(f, F(t, 362, 23, 100, 14), kAccentSoft, 40), 12);
    for (int row = 2; row < 6; ++row) { CHECK_GT(cnt(f, F(t, 362, 6 + 17 * row, 100, 14), kFg, 40), 8); }
    CHECK_EQ(ink(f, F(t, 280, 110, 180, 40), kPanel), 0);               // six rows: SIGNAL has no slot, so no row
}
void checkVorEmpty(const Frame& f) {
    const Pos t = kVorE;
    // no signal: the flag across the column (a barber pole of danger-bg and card-err bands in a danger-line frame) and its words
    const R flag = F(t, 168, 126, 104, 16);
    CHECK(f.near(flag.x + 52, flag.y, kDangerLine, 6) && f.near(flag.x + 52, flag.y + 15, kDangerLine, 6));
    CHECK(f.near(flag.x, flag.y + 8, kDangerLine, 6) && f.near(flag.x + 103, flag.y + 8, kDangerLine, 6));
    CHECK_GT(cnt(f, flag, kDangerBg, 8), 250);
    CHECK_GT(cnt(f, flag, kCardErr, 8), 250);
    CHECK_GT(cnt(f, flag, kDangerFg, 40), 20);
    // the user's setting is still on the knob and the course line; the index furniture stays; no needle
    checkVorDial(f, t, 90, 274, false);
    // no bar marker, no TO / FROM, no LOCK / NEW; the centre index is the only thing in the bar
    box(f, F(t, 168, 47, 104, 18), kLineStrong, kField, kPanel);
    CHECK_EQ(cnt(f, F(t, 169, 48, 102, 16), kAccent, 24), 0);
    CHECK(at(f, F(t, 220, 56, 1, 1), kFocus, 12));
    CHECK_EQ(cnt(f, F(t, 168, 107, 104, 13), kGreen, 24) + cnt(f, F(t, 168, 148, 104, 13), kGreen, 24) + cnt(f, F(t, 168, 148, 104, 13), kAccent, 24), 0);
    CHECK_GT(cnt(f, F(t, 168, 107, 104, 13), kRing, 8), 20);
    CHECK_GT(cnt(f, F(t, 168, 148, 104, 13), kRing, 8), 20);
    // the readouts: dashes, never zero; NO IDENT in place of the letters
    CHECK_EQ(cnt(f, F(t, 280, 0, 190, 110), kAccentSoft, 40), 0);
    CHECK_GT(ink(f, F(t, 362, 6, 100, 14), kPanel), 6);
    CHECK_GT(ink(f, F(t, 362, 23, 100, 14), kPanel), 12);
}

// ===== C6 FAX =====================================================================================================================
void checkFax(const Frame& f) {
    const Pos t = kFaxL;
    CHECK_GT(cnt(f, F(t, 12, 8, 200, 13), kMuted, 40), 30);            // RECEPTION
    // seven lamps in two columns (12 and 115) on a 19 px pitch: the phases IDLE START / PHASING PICTURE / STOP, then NEW LOCK on a row of their own
    struct L { int x, y; bool lit; Rgb c; };
    const std::array<L, 7> lamps = {{{12, 27, false, kLampOff}, {115, 27, false, kLampOff}, {12, 46, false, kLampOff}, {115, 46, true, kGreen}, {12, 65, false, kLampOff}, {12, 84, true, kAccent}, {115, 84, true, kGreen}}};
    for (const L& l : lamps) { CHECK(at(f, F(t, l.x + 4, l.y + 6, 1, 1), l.c, l.lit ? 30 : 12)); }
    CHECK_EQ(ink(f, F(t, 115, 65, 97, 13), kPanel), 0);                 // the third row holds STOP alone
    // IOC and LPM as glass (24 high), the lines counter 0312 in four cells
    boxLeft(f, F(t, 12, 118, 48, 24), kLine, kField, kPanel);
    CHECK_GT(cnt(f, F(t, 14, 120, 44, 20), kDigit, 40), 8);
    CHECK_GT(cnt(f, F(t, 12, 103, 40, 13), kMuted, 40), 4);
    CHECK_GT(cnt(f, F(t, 12, 148, 60, 13), kMuted, 40), 10);
    for (int i = 0; i < 4; ++i) {
        box(f, F(t, 12 + 22 * i, 167, 20, 28), kLine, kField, kPanel);
        CHECK_GT(cnt(f, F(t, 13 + 22 * i, 168, 18, 26), kDigit, 40), 8);
    }
    // the tuning meter: a 234 px bar (10 high at y 39) with its danger-line end zones, a dim centre index, and the amber needle
    CHECK_GT(cnt(f, F(t, 224, 8, 234, 13), kMuted, 40), 20);
    CHECK(at(f, F(t, 224, 44, 1, 1), kLineStrong, 8) && at(f, F(t, 457, 44, 1, 1), kLineStrong, 8));
    CHECK(at(f, F(t, 232, 44, 1, 1), kDangerLine, 8) && at(f, F(t, 450, 44, 1, 1), kDangerLine, 8));
    CHECK(at(f, F(t, 290, 44, 1, 1), kField, 8));
    CHECK(at(f, F(t, 341, 36, 1, 1), kDim, 24));                        // the centre index, taller than the bar
    const int needle = 341 + static_cast<int>(std::lround(40.0 / 250.0 * 116.0));                 // +40 Hz of +-250
    CHECK_GE(cnt(f, F(t, needle - 3, 29, 7, 30), kAccent, 12), 40);
    CHECK_LE(cnt(f, F(t, 224, 29, 234, 30), kAccent, 12), 70);
    CHECK_GT(cnt(f, F(t, 224, 67, 100, 16), kAccentSoft, 40), 15);     // +40 Hz
    // the paper slot: 234 x 70, a strip of paper coming out of the top, as long as the lines received (312 of 1200)
    box(f, F(t, 224, 108, 234, 70), kLine, kField, kPanel);
    CHECK(at(f, F(t, 300, 115, 1, 1), kFgSoft, 10) && at(f, F(t, 300, 125, 1, 1), kFgSoft, 10));
    CHECK(at(f, F(t, 300, 150, 1, 1), kField, 8) && at(f, F(t, 230, 115, 1, 1), kField, 8));
    CHECK_GT(cnt(f, F(t, 224, 89, 234, 13), kMuted, 40), 30);          // PAPER SLOT
}
void checkFaxEmpty(const Frame& f) {
    const Pos t = kFaxE;
    const std::array<std::pair<int, int>, 7> pos = {{{12, 27}, {115, 27}, {12, 46}, {115, 46}, {12, 65}, {12, 84}, {115, 84}}};
    for (const auto& p : pos) { CHECK(at(f, F(t, p.first + 4, p.second + 6, 1, 1), kLampOff, 12)); }
    boxLeft(f, F(t, 12, 118, 48, 24), kLine, kField, kPanel);
    CHECK_GT(ink(f, F(t, 14, 120, 44, 20), kField), 4);                 // "--" in the dim ink ...
    CHECK_EQ(warm(f, F(t, 14, 120, 44, 20)), 0);                        // ... not the phosphor of a reading
    for (int i = 0; i < 4; ++i) {
        const R c = F(t, 12 + 22 * i, 167, 20, 28);
        CHECK(f.near(c.x + 3, c.y + 3, kBg, 6));
        CHECK_GT(cnt(f, {c.x + 2, c.y + 2, 16, 24}, kRing, 24), 3);
    }
    CHECK(at(f, F(t, 341, 36, 1, 1), kDim, 24));                        // the index stays, the needle does not
    CHECK_EQ(cnt(f, F(t, 224, 29, 234, 30), kAccent, 24), 0);
    CHECK_GT(ink(f, F(t, 224, 67, 100, 16), kPanel), 4);
    CHECK_EQ(warm(f, F(t, 224, 67, 100, 16)), 0);
    box(f, F(t, 224, 108, 234, 70), kLine, kField, kPanel);
    CHECK_EQ(cnt(f, F(t, 226, 110, 230, 66), kFgSoft, 10), 0);          // no paper: nothing has been received
    CHECK_EQ(warm(f, F(t, 0, 0, 470, 203)), 0);
}

// ===== C7 BEACON ==================================================================================================================
void checkBeacon(const Frame& f) {
    const Pos t = kBcnL;
    // DISTRESS (red) and NEW (amber) on the left of the first row, the caption on the right
    const R lamps = F(t, 12, 8, 250, 13);
    CHECK_GT(cnt(f, lamps, kRed, 24), 25);
    CHECK_GT(cnt(f, lamps, kAccent, 24), 25);
    CHECK(firstX(f, lamps, kRed) < firstX(f, lamps, kAccent));
    CHECK_GT(cnt(f, F(t, 300, 8, 158, 13), kMuted, 40), 30);
    // fifteen cells (20 x 28 on a 22 px pitch), each carrying a hexadecimal character
    for (int i = 0; i < 15; ++i) {
        box(f, F(t, 12 + 22 * i, 29, 20, 28), kLine, kField, kPanel);
        CHECK_GT(cnt(f, F(t, 13 + 22 * i, 30, 18, 26), kDigit, 40), 6);
    }
    CHECK_EQ(ink(f, F(t, 12 + 15 * 22, 29, 10, 28), kPanel), 0);
    // COUNTRY / PROTOCOL / POSITION: three rows of a cap and its value
    for (int row = 0; row < 3; ++row) {
        CHECK_GT(cnt(f, F(t, 12, 65 + 18 * row, 80, 14), kMuted, 40), 10);
        CHECK_GT(cnt(f, F(t, 94, 65 + 18 * row, 300, 14), kFg, 40), 20);
    }
    // CARRIER ERROR: the cap, the signed figure on the right, the bar 446 wide (16 high at y 151) with an index and the needle
    CHECK_GT(cnt(f, F(t, 12, 123, 200, 24), kMuted, 40), 30);
    CHECK_GT(cnt(f, F(t, 360, 123, 98, 24), kAccentSoft, 40), 20);
    CHECK(at(f, F(t, 12, 159, 1, 1), kLineStrong, 8) && at(f, F(t, 457, 159, 1, 1), kLineStrong, 8));
    CHECK(at(f, F(t, 25, 159, 1, 1), kDangerLine, 8) && at(f, F(t, 444, 159, 1, 1), kDangerLine, 8));
    CHECK(at(f, F(t, 235, 152, 1, 1), kDim, 24));
    const int needle = 235 + static_cast<int>(std::lround(210.0 / 5000.0 * 222.0));              // +210 Hz of +-5000
    CHECK_GE(cnt(f, F(t, needle - 3, 151, 7, 16), kAccent, 12), 20);
    CHECK_LE(cnt(f, F(t, 12, 151, 446, 16), kAccent, 12), 40);
    // SINCE LAST BURST 00:02:14, BURSTS LOGGED 3: two bays, each as wide as the wider of its cap and its glass
    CHECK_GT(cnt(f, F(t, 12, 175, 125, 13), kMuted, 40), 25);
    boxLeft(f, F(t, 12, 190, 76, 24), kLine, kField, kPanel);
    CHECK_GT(cnt(f, F(t, 14, 192, 72, 20), kDigit, 40), 25);
    CHECK_EQ(ink(f, F(t, 122, 175, 8, 13), kPanel), 0);                // clear air between the two caps
    CHECK_GT(cnt(f, F(t, 132, 175, 120, 13), kMuted, 40), 25);
    boxLeft(f, F(t, 132, 190, 25, 24), kLine, kField, kPanel);
    CHECK_GT(cnt(f, F(t, 134, 192, 21, 20), kDigit, 40), 4);
}
void checkBeaconEmpty(const Frame& f) {
    const Pos t = kBcnE;
    // fifteen unlit cells: the dark ground, a faint eight in each; nothing amber on the face
    for (int i = 0; i < 15; ++i) {
        const R c = F(t, 12 + 22 * i, 29, 20, 28);
        CHECK(f.near(c.x, c.y + 14, kLine, 6) && f.near(c.x + 3, c.y + 3, kBg, 6));
        CHECK_GT(cnt(f, {c.x + 2, c.y + 2, 16, 24}, kRing, 24), 6);
    }
    CHECK_EQ(warm(f, F(t, 0, 0, 470, 222)), 0);
    CHECK_EQ(cnt(f, F(t, 12, 8, 250, 13), kRed, 24) + cnt(f, F(t, 12, 8, 250, 13), kAccent, 24), 0);
    CHECK_GT(cnt(f, F(t, 12, 8, 250, 13), kRing, 8), 20);
    // no figure, no needle (the index stays), an empty age glass, and no bursts bay at all (nothing was counted)
    CHECK_EQ(ink(f, F(t, 360, 123, 98, 24), kPanel), 0);
    CHECK(at(f, F(t, 235, 152, 1, 1), kDim, 24));
    CHECK_EQ(cnt(f, F(t, 12, 151, 446, 16), kAccent, 24), 0);
    boxLeft(f, F(t, 12, 190, 76, 24), kLine, kField, kPanel);
    CHECK_EQ(ink(f, F(t, 16, 194, 68, 16), kField), 0);
    CHECK_EQ(ink(f, F(t, 126, 175, 130, 40), kPanel), 0);
}

// ===== C8 METER ===================================================================================================================
void checkMeter(const Frame& f) {
    const Pos t = kMetL;
    // the pale LCD: 230 x 63, fg-soft ground, a muted border, the nameplate on top and seven pale cells under it
    box(f, F(t, 12, 8, 230, 63), kMuted, kFgSoft, kPanel);
    CHECK_GT(dark(f, F(t, 23, 17, 120, 13)), 15);                       // ELECTRICITY
    for (int i = 0; i < 7; ++i) {
        const R c = F(t, 23 + 22 * i, 34, 20, 28);
        CHECK(f.near(c.x, c.y + 14, kMuted, 8) && f.near(c.x + 19, c.y + 14, kMuted, 8) && f.near(c.x + 3, c.y + 3, kFgSoft, 8));
        CHECK_GT(dark(f, {c.x + 1, c.y + 1, 18, 26}), 8);               // every figure of 0048213, the leading zeros included
    }
    CHECK(at(f, F(t, 23 + 7 * 22 - 1, 48, 1, 1), kFgSoft, 8));          // no eighth cell for a seven-digit reading
    CHECK_GT(dark(f, F(t, 178, 36, 60, 24)), 12);                       // kWh
    // ELECTRIC lit, GAS and WATER dark; ALERT (blinking, lit now) and NEW lit, OVER dark
    const R r1 = F(t, 12, 77, 70, 13), r1b = F(t, 90, 77, 150, 13), r2 = F(t, 12, 96, 230, 13);
    CHECK_GT(cnt(f, r1, kGreen, 24), 25);
    CHECK_EQ(cnt(f, r1b, kGreen, 24), 0);
    CHECK_GT(cnt(f, r1b, kRing, 8), 20);
    CHECK_GT(cnt(f, r2, kRed, 24), 25);
    CHECK_GT(cnt(f, r2, kAccent, 24), 25);
    CHECK_EQ(cnt(f, r2, kGreen, 24), 0);
    CHECK(firstX(f, r2, kRed) < firstX(f, r2, kAccent));
    CHECK_GT(cnt(f, r2, kRing, 8), 10);
    // the right column: TAMPER lit with PHYSICAL 0 / ENCODER 1, then the meter number, the commodity and the last heard
    CHECK_GT(cnt(f, F(t, 254, 8, 60, 13), kRed, 24), 25);
    CHECK_GT(cnt(f, F(t, 322, 8, 136, 13), kDim, 40), 20);
    CHECK_GT(cnt(f, F(t, 322, 8, 136, 13), kFg, 40), 6);
    for (int row = 0; row < 3; ++row) {
        CHECK_GT(cnt(f, F(t, 254, 27 + 16 * row, 80, 13), kMuted, 40), 10);
        CHECK_GT(cnt(f, F(t, 340, 27 + 16 * row, 118, 13), kFg, 40), 10);
    }
}
void checkMeterEmpty(const Frame& f) {
    const Pos t = kMetE;
    box(f, F(t, 12, 8, 230, 63), kMuted, kFgSoft, kPanel);
    // seven unlit cells: faint segments on the pale ground, not one figure in full ink; the unit is dim too
    for (int i = 0; i < 7; ++i) {
        const R c = F(t, 23 + 22 * i, 34, 20, 28);
        CHECK(f.near(c.x, c.y + 14, kMuted, 8) && f.near(c.x + 3, c.y + 3, kFgSoft, 8));
        CHECK_EQ(dark(f, {c.x + 1, c.y + 1, 18, 26}), 0);
        CHECK_GT(ink(f, {c.x + 2, c.y + 2, 16, 24}, kFgSoft, 12), 8);
    }
    CHECK_EQ(dark(f, F(t, 178, 36, 60, 24)), 0);
    CHECK_GT(ink(f, F(t, 178, 36, 60, 24), kFgSoft, 12), 6);
    CHECK_GT(dark(f, F(t, 23, 17, 120, 13)), 15);                       // the nameplate is printed, not lit: UTILITY METER
    const R lamps = F(t, 12, 77, 230, 32);
    CHECK_EQ(cnt(f, lamps, kGreen, 24) + cnt(f, lamps, kRed, 24) + cnt(f, lamps, kAccent, 24), 0);
    CHECK_GT(cnt(f, lamps, kRing, 8), 60);
    CHECK_EQ(cnt(f, F(t, 254, 8, 70, 13), kRed, 24), 0);
    CHECK_GT(cnt(f, F(t, 254, 8, 70, 13), kRing, 8), 10);
    CHECK_EQ(ink(f, F(t, 340, 27, 118, 45), kPanel), 0);                // no values
}

// ===== C9 WEATHER =================================================================================================================
void checkWeather(const Frame& f) {
    const Pos t = kWxL;
    // three compartments 143 / 143 / 144 wide, 143 high, 8 apart; a cap, two pale readouts (34 high), a note, a lamp row
    const std::array<int, 3> cx = {12, 163, 314};
    const std::array<int, 3> cw = {143, 143, 144};
    for (int i = 0; i < 3; ++i) {
        box(f, F(t, cx[i], 8, cw[i], 143), kLine, kPanel, kPanel);
        CHECK_GT(cnt(f, F(t, cx[i] + 9, 17, 100, 13), kMuted, 40), 25);
        box(f, F(t, cx[i] + 9, 34, cw[i] - 18, 34), kMuted, kFgSoft, kPanel);
        box(f, F(t, cx[i] + 9, 72, cw[i] - 18, 34), kMuted, kFgSoft, kPanel);
    }
    // channel 1: 21.4 C and 56 % in full ink; channel 2: dashes in the dimmed ink; channel 3: HHH, and no humidity
    CHECK_GT(dark(f, in4(F(t, 21, 34, 125, 34))), 25);
    CHECK_GT(dark(f, in4(F(t, 21, 72, 125, 34))), 12);
    CHECK_EQ(dark(f, in4(F(t, 172, 34, 125, 34))), 0);
    CHECK_GT(ink(f, in4(F(t, 172, 34, 125, 34)), kFgSoft, 30), 15);
    CHECK_EQ(dark(f, in4(F(t, 172, 72, 125, 34))), 0);
    CHECK_GT(ink(f, in4(F(t, 172, 72, 125, 34)), kFgSoft, 30), 8);
    CHECK_GT(dark(f, in4(F(t, 323, 34, 126, 34))), 15);
    CHECK_EQ(dark(f, in4(F(t, 323, 72, 126, 34))), 0);
    CHECK_GT(ink(f, in4(F(t, 323, 72, 126, 34)), kFgSoft, 30), 8);
    // the notes: the sensor, "not heard", "out of range"
    CHECK_GT(cnt(f, F(t, 21, 110, 125, 15), kDim, 40), 40);
    CHECK_GT(cnt(f, F(t, 172, 110, 125, 15), kDim, 40), 15);
    CHECK_GT(cnt(f, F(t, 323, 110, 125, 15), kDim, 40), 20);
    // the BATT lamps: channel 1 dark, channel 2 has none, channel 3 lit red
    CHECK_GT(cnt(f, F(t, 21, 129, 70, 13), kRing, 8), 10);
    CHECK_EQ(cnt(f, F(t, 21, 129, 70, 13), kRed, 24), 0);
    CHECK_EQ(ink(f, F(t, 172, 129, 125, 13), kPanel), 0);
    CHECK_GT(cnt(f, F(t, 323, 129, 70, 13), kRed, 24), 25);
    // the foot: NEW lit on the right, no sentence on the left
    CHECK_GT(cnt(f, F(t, 358, 159, 100, 16), kAccent, 24), 25);
    CHECK_EQ(ink(f, F(t, 12, 159, 300, 16), kPanel), 0);
}
void checkWeatherEmpty(const Frame& f) {
    const Pos t = kWxE;
    const std::array<int, 3> cx = {12, 163, 314};
    const std::array<int, 3> cw = {143, 143, 144};
    for (int i = 0; i < 3; ++i) {
        box(f, F(t, cx[i], 8, cw[i], 143), kLine, kPanel, kPanel);
        box(f, F(t, cx[i] + 9, 34, cw[i] - 18, 34), kMuted, kFgSoft, kPanel);
        // the console's own dashes, in the dimmed ink: not a single figure at full strength
        CHECK_EQ(dark(f, in4(F(t, cx[i] + 9, 34, cw[i] - 18, 34))), 0);
        CHECK_GT(ink(f, in4(F(t, cx[i] + 9, 34, cw[i] - 18, 34)), kFgSoft, 30), 15);
        CHECK_EQ(dark(f, in4(F(t, cx[i] + 9, 72, cw[i] - 18, 34))), 0);
        CHECK_GT(ink(f, in4(F(t, cx[i] + 9, 72, cw[i] - 18, 34)), kFgSoft, 30), 8);
        CHECK_GT(cnt(f, F(t, cx[i] + 9, 110, cw[i] - 18, 15), kDim, 40), 15);      // not heard
        CHECK_EQ(ink(f, F(t, cx[i] + 9, 129, cw[i] - 18, 13), kPanel), 0);         // no lamp row for a channel nobody heard
    }
    // the sheet's sentence at the foot, and no lamp lit
    CHECK_GT(cnt(f, F(t, 12, 159, 250, 16), kDim, 40), 40);
    CHECK_EQ(cnt(f, F(t, 12, 159, 446, 16), kAccent, 24) + cnt(f, F(t, 12, 159, 446, 16), kRed, 24), 0);
    CHECK_EQ(warm(f, F(t, 0, 0, 470, 181)), 0);
}

// ===== interactions: the knob, the blink, the feed, the chart ===================================================================
// the VOR knob's centre on sheet 0 (the face's origin + (85, 81))
constexpr float kKnobX = 488 + 1 + 85, kKnobY = 200 + 1 + 81;

void checkObs(InstrumentsSpecimen& ui, slint::Window& w) {
    moveTo(w, 800, 1250);
    const Frame before = shot(ui);
    CHECK(ui.get_obs() == 90 && ui.get_obs_events() == 0);
    // a wheel notch over the knob: one degree, up for a scroll up, down for a scroll down; the host owns the number
    scroll(w, kKnobX, kKnobY, 0.0f, 60.0f);
    CHECK(ui.get_obs_events() == 1 && ui.get_obs_last() == 91 && ui.get_obs() == 91);
    scroll(w, kKnobX, kKnobY, 0.0f, 60.0f);
    CHECK(ui.get_obs_events() == 2 && ui.get_obs_last() == 92);
    scroll(w, kKnobX, kKnobY, 0.0f, -60.0f);
    scroll(w, kKnobX, kKnobY, 0.0f, -60.0f);
    scroll(w, kKnobX, kKnobY, 0.0f, -60.0f);
    CHECK(ui.get_obs_events() == 5 && ui.get_obs_last() == 89);
    // the deviation bar follows: radial 274, course 89 -> 5 degrees off, half scale
    Frame f = shot(ui);
    CHECK_GT(differing(before, f, F(kVorL, 168, 25, 104, 40)), 20);
    checkVorBar(f, 89, 274);
    CHECK_GT(cnt(f, F(kVorL, 72, 72, 26, 18), kFg, 40), 12);
    // it wraps: 000 down is 359, 359 up is 000
    ui.set_obs(0);
    scroll(w, kKnobX, kKnobY, 0.0f, -60.0f);
    CHECK(ui.get_obs_last() == 359);
    ui.set_obs(359);
    scroll(w, kKnobX, kKnobY, 0.0f, 60.0f);
    CHECK(ui.get_obs_last() == 0);
    ui.set_obs(90);
    // whatever number the host holds is a course: -10 is 350 and 725 is 005
    ui.set_obs(-10);
    scroll(w, kKnobX, kKnobY, 0.0f, 60.0f);
    CHECK(ui.get_obs_last() == 351);
    ui.set_obs(725);
    scroll(w, kKnobX, kKnobY, 0.0f, 60.0f);
    CHECK(ui.get_obs_last() == 6);
    ui.set_obs(90);
    // a wheel off the knob is not the knob's
    const int events = ui.get_obs_events();
    scroll(w, kKnobX + 60, kKnobY, 0.0f, 60.0f);
    scroll(w, kKnobX, kKnobY + 70, 0.0f, 60.0f);
    CHECK(ui.get_obs_events() == events);
    // a press-drag twists it: the angle from the middle to the pointer is tracked, so a quarter turn clockwise is +90
    press(w, kKnobX + 14.0f, kKnobY, slint::PointerEventButton::Left);
    moveTo(w, kKnobX + 10.0f, kKnobY + 10.0f);
    moveTo(w, kKnobX, kKnobY + 14.0f);
    release(w, kKnobX, kKnobY + 14.0f, slint::PointerEventButton::Left);
    CHECK(ui.get_obs_events() > events && ui.get_obs_last() == 180);
    // and anticlockwise takes it back down
    press(w, kKnobX, kKnobY + 14.0f, slint::PointerEventButton::Left);
    moveTo(w, kKnobX + 10.0f, kKnobY + 10.0f);
    moveTo(w, kKnobX + 14.0f, kKnobY);
    release(w, kKnobX + 14.0f, kKnobY, slint::PointerEventButton::Left);
    CHECK(ui.get_obs_last() == 90);
    ui.set_obs(90);
    // the keyboard: Tab brings the focus to the knob (a ring shows where it is), Right and Left turn it a degree each
    const Frame unfocused = shot(ui);
    const R ringArea = F(kVorL, 52, 48, 66, 66);
    const int before2 = ui.get_obs_events();
    keyTap(w, u8"	");
    f = shot(ui);
    CHECK_GT(differing(unfocused, f, ringArea), 100);
    keyTap(w, slint::platform::key_codes::RightArrow);
    CHECK(ui.get_obs_events() == before2 + 1 && ui.get_obs_last() == 91);
    keyTap(w, slint::platform::key_codes::RightArrow);
    keyTap(w, slint::platform::key_codes::LeftArrow);
    keyTap(w, slint::platform::key_codes::LeftArrow);
    keyTap(w, slint::platform::key_codes::LeftArrow);
    CHECK(ui.get_obs_events() == before2 + 5 && ui.get_obs_last() == 89);
    keyTap(w, u8"x");
    CHECK(ui.get_obs_events() == before2 + 5);                                      // any other key is not the knob's
    ui.set_obs(90);
    keyTap(w, slint::platform::key_codes::Backtab);                                 // the focus leaves it again
    f = shot(ui);
    CHECK_EQ(differing(unfocused, f, ringArea), 0);
    moveTo(w, 800, 1250);
}

void checkBlink(InstrumentsSpecimen& ui) {
    // ALERT blinks at 2 Hz from the host's clock: lit for the first 250 ms of every 500, dark for the next 250
    for (const int phase : {0, 250, 500, 750, 125, 1000}) {
        const bool on = (phase / 250) % 2 == 0;
        auto tone = ui.get_st_tone(); tone.now_ms = phase; ui.set_st_tone(tone);
        auto bcn = ui.get_st_beacon(); bcn.now_ms = phase; ui.set_st_beacon(bcn);
        auto tel = ui.get_st_tele(); tel.now_ms = phase; ui.set_st_tele(tel);
        auto met = ui.get_st_meter(); met.now_ms = phase; ui.set_st_meter(met);
        auto gen = ui.get_st_generic(); gen.now_ms = phase; ui.set_st_generic(gen);
        const Frame f = shot(ui);
        CHECK((reddish(f, F(kTonL, 22, 8, 64, 64)) > 1000) == on);                        // the radial lamp
        CHECK((cnt(f, F(kTonL, 12, 78, 84, 13), kRed, 24) > 25) == on);                   // ALERT beside it
        CHECK((cnt(f, F(kBcnL, 12, 8, 250, 13), kRed, 24) > 25) == on);                   // DISTRESS
        CHECK((cnt(f, F(kTelL, 294, 48, 164, 13), kRed, 24) > 25) == on);                 // the printer's ALERT
        CHECK((cnt(f, F(kMetL, 12, 96, 230, 13), kRed, 24) > 25) == on);                  // the meter's ALERT
        CHECK((cnt(f, F(kGenL, 190, 11, 268, 15), kRed, 24) > 25) == on);                 // the generic face's
        CHECK_GT(cnt(f, F(kTonL, 12, 97, 84, 13), kAccent, 24), 25);                      // NEW does not blink
        CHECK_GT(cnt(f, F(kBcnL, 12, 8, 250, 13), kAccent, 24), 25);
    }
    auto tone = ui.get_st_tone(); tone.now_ms = 0; ui.set_st_tone(tone);
    auto bcn = ui.get_st_beacon(); bcn.now_ms = 0; ui.set_st_beacon(bcn);
    auto tel = ui.get_st_tele(); tel.now_ms = 0; ui.set_st_tele(tel);
    auto met = ui.get_st_meter(); met.now_ms = 0; ui.set_st_meter(met);
    auto gen = ui.get_st_generic(); gen.now_ms = 0; ui.set_st_generic(gen);
    // an alert that is not held does not light the lamp whatever the clock says
    auto quiet = ui.get_st_tone(); quiet.alert = false; ui.set_st_tone(quiet);
    const Frame f = shot(ui);
    CHECK_EQ(reddish(f, F(kTonL, 22, 8, 64, 64)), 0);
    CHECK_EQ(cnt(f, F(kTonL, 12, 78, 84, 13), kRed, 24), 0);
    quiet.alert = true; ui.set_st_tone(quiet);
}

void checkFeed(InstrumentsSpecimen& ui) {
    const R paper = F(kTelL, 12, 8, 270, 122);
    const Frame settled = shot(ui);
    settle(std::chrono::milliseconds(50));
    CHECK_EQ(differing(settled, shot(ui), paper), 0);                                // a paper that has stopped does not move
    // a new event (seq advances): the paper starts 26 px short of its place and feeds out over half a second
    auto tel = ui.get_st_tele();
    tel.seq = 6;
    ui.set_st_tele(tel);
    const Frame started = shot(ui);
    CHECK_GT(differing(settled, started, paper), 200);
    settle(std::chrono::milliseconds(150));
    const Frame mid = shot(ui);
    CHECK_GT(differing(started, mid, paper), 100);
    CHECK_GT(differing(settled, mid, paper), 20);
    settle(std::chrono::milliseconds(700));
    const Frame done = shot(ui);
    CHECK_EQ(differing(settled, done, paper), 0);                                    // and it comes to rest where it was
    settle(std::chrono::milliseconds(100));
    CHECK_EQ(differing(done, shot(ui), paper), 0);
    // the same seq again is not a new event
    ui.set_st_tele(tel);
    CHECK_EQ(differing(done, shot(ui), paper), 0);
    // new text on the same seq does not feed either: it simply prints
    ui.set_paper_newest("DEP EGLL 1432Z RWY 27L AND A LONGER LINE");
    const Frame longer = shot(ui);
    CHECK_GT(differing(done, longer, paper), 0);
    CHECK_EQ(differing(longer, shot(ui), paper), 0);
    ui.set_paper_newest("DEP EGLL 1432Z RWY 27L");
}

void checkChart(InstrumentsSpecimen& ui) {
    // the plugin's own picture on the paper strip: the whole image mapped onto the strip as long as the lines received
    auto fax = ui.get_st_fax();
    fax.chart = twoHalves(16, 24);
    fax.chart_complete = false;
    ui.set_st_fax(fax);
    Frame f = shot(ui);
    const R strip = F(kFaxL, 238, 110, 206, 21);
    CHECK_GT(cnt(f, strip, rgb(0xff0000), 40), 1500);                                 // the top half of the picture, red ...
    CHECK_GT(cnt(f, strip, rgb(0x0000ff), 40), 1500);                                 // ... and the bottom half, blue
    CHECK_EQ(cnt(f, F(kFaxL, 224, 135, 234, 40), rgb(0xff0000), 40), 0);               // and nothing below the strip
    // more lines make a longer strip: 600 of 1200 is half the slot
    fax.values = vals({576, 120, 600, 40});
    ui.set_st_fax(fax);
    f = shot(ui);
    CHECK_GT(cnt(f, F(kFaxL, 238, 110, 206, 70), rgb(0x0000ff), 40), 3000);
    CHECK(at(f, F(kFaxL, 300, 138, 1, 1), rgb(0x0000ff), 40));
    CHECK(at(f, F(kFaxL, 300, 160, 1, 1), kField, 8));
    // without a picture the strip is blank paper again
    fax.chart = slint::Image();
    ui.set_st_fax(fax);
    f = shot(ui);
    CHECK_EQ(cnt(f, F(kFaxL, 238, 110, 206, 70), rgb(0x0000ff), 40), 0);
    CHECK_GT(cnt(f, F(kFaxL, 238, 110, 206, 40), kFgSoft, 12), 4000);
    fax.values = vals({576, 120, 312, 40});
    ui.set_st_fax(fax);
}

void checkStates(InstrumentsSpecimen& ui) {
    // the pager: a longer message than the glass holds shows its first four rows and the "more" arrow; a short one shows none
    auto pg = ui.get_st_pager();
    pg.texts = texts({"ONE TWO THREE FOUR FIVE SIX SEVEN EIGHT NINE TEN ELEVEN TWELVE THIRTEEN FOURTEEN", "1234567", "14:32", "ALPHA"});
    ui.set_st_pager(pg);
    Frame f = shot(ui);
    CHECK_GT(ink(f, F(kPagL, 21, 59, 144, 16), kField), 40);                          // the fourth row is used
    CHECK_GT(ink(f, F(kPagL, 238, 60, 20, 22), kField), 4);                           // the arrow at the foot of the glass
    pg.texts = texts({"UK CALL 0123456 AMBULANCE CALL OUT ATTEND STATION RD", "1234567", "14:32", "ALPHA"});
    ui.set_st_pager(pg);
    f = shot(ui);
    CHECK_EQ(ink(f, F(kPagL, 238, 60, 20, 22), kField), 0);                           // none for a message that fits
    // a capcode past the cells keeps the LOW end: 12345678 shows 2345678 in the seven cells and spills nothing to the left
    pg.texts = texts({"HELLO", "12345678", "14:32", "ALPHA"});
    ui.set_st_pager(pg);
    f = shot(ui);
    for (int i = 0; i < 7; ++i) { CHECK_GT(cnt(f, F(kPagL, 275 + 22 * i, 66, 18, 26), kDigit, 40), 8); }
    CHECK_EQ(ink(f, F(kPagL, 260, 65, 13, 28), kPanel), 0);
    pg.texts = texts({"UK CALL 0123456 AMBULANCE CALL OUT ATTEND STATION RD", "1234567", "14:32", "ALPHA"});
    ui.set_st_pager(pg);

    // the meter: eight figures need eight cells (a register is never cut), a ninth is OVER and lights nothing else
    auto mt = ui.get_st_meter();
    mt.values = vals({12345678, 4});
    ui.set_st_meter(mt);
    f = shot(ui);
    {
        const R c = F(kMetL, 23 + 7 * 22, 34, 20, 28);
        CHECK(f.near(c.x, c.y + 14, kMuted, 8));
        CHECK_GT(dark(f, {c.x + 1, c.y + 1, 18, 26}), 8);
        CHECK_GT(cnt(f, F(kMetL, 12, 96, 230, 13), kAccent, 24), 25);                 // NEW only: OVER is dark ...
        CHECK_GT(cnt(f, F(kMetL, 12, 96, 230, 13), kRing, 8), 10);                    // ... its ring
    }
    mt.values = vals({1000000000.0f, 4});
    ui.set_st_meter(mt);
    f = shot(ui);
    CHECK_EQ(dark(f, F(kMetL, 23, 34, 8 * 22, 28)), 0);                                // no figure: the reading does not fit
    CHECK_GT(cnt(f, F(kMetL, 12, 96, 230, 13), kAccent, 24), 50);                      // NEW and OVER, both amber
    // a gas meter: its lamp, and the unit word UNITS (the register's scale is the installation's, not the plugin's)
    mt = ui.get_st_meter();
    mt.values = vals({12994, 0});
    mt.texts = texts({"30114508", "GAS"});
    ui.set_st_meter(mt);
    f = shot(ui);
    CHECK_EQ(cnt(f, F(kMetL, 12, 77, 70, 13), kGreen, 24), 0);
    CHECK_GT(cnt(f, F(kMetL, 80, 77, 60, 13), kGreen, 24), 10);
    CHECK_EQ(cnt(f, F(kMetL, 254, 8, 60, 13), kRed, 24), 0);                           // no tamper reported
    mt.values = vals({48213, 4});
    mt.texts = texts({"47281936", "ELECTRIC"});
    ui.set_st_meter(mt);

    // the weather console: a channel outside the mask is blank even when its slot holds a temperature; 0.0 C inside it is a reading
    auto wx = ui.get_st_weather();
    wx.values = vals({0.0f, 22.2f, 99.0f, 0, 40, 0, 3, 0});                          // channels 1 and 2 heard; channel 3 outside the mask
    wx.texts = texts({"A", "B", "C"});
    ui.set_st_weather(wx);
    f = shot(ui);
    CHECK_GT(dark(f, in4(F(kWxL, 21, 34, 125, 34))), 25);                              // 0.0 C is drawn
    CHECK_GT(dark(f, in4(F(kWxL, 172, 34, 125, 34))), 25);
    CHECK_EQ(dark(f, in4(F(kWxL, 323, 34, 126, 34))), 0);                              // 99 C in a slot nobody filled: not drawn
    CHECK_GT(ink(f, in4(F(kWxL, 323, 34, 126, 34)), kFgSoft, 30), 15);
    CHECK_GT(cnt(f, F(kWxL, 323, 110, 125, 15), kDim, 40), 15);                        // "not heard" for the third
    CHECK_EQ(dark(f, in4(F(kWxL, 21, 72, 125, 34))), 0);                               // humidity 0 means "not measured": dashes
    CHECK_GT(dark(f, in4(F(kWxL, 172, 72, 125, 34))), 12);                             // 40 % on the second
    // a temperature under the field's floor reads LLL; a low-battery bit of a channel that is not heard lights nothing
    wx.values = vals({-250.0f, 21.4f, 21.4f, 56, 56, 56, 3, 4});
    ui.set_st_weather(wx);
    f = shot(ui);
    CHECK_GT(dark(f, in4(F(kWxL, 21, 34, 125, 34))), 15);
    CHECK_GT(dark(f, in4(F(kWxL, 172, 34, 125, 34))), 15);
    CHECK_EQ(cnt(f, F(kWxL, 323, 129, 70, 13), kRed, 24), 0);
    wx.values = vals({21.4f, 0, 250, 56, 0, 0, 5, 4});
    wx.texts = texts({"Oregon THGR122N", "", "Acme ST-9"});
    ui.set_st_weather(wx);

    // a VOR that is not locked has no radial however the slot reads: the flag, no needle, no marker
    auto vor = ui.get_st_vor();
    vor.lock = false;
    ui.set_st_vor(vor);
    f = shot(ui);
    CHECK_GT(cnt(f, F(kVorL, 168, 126, 104, 16), kDangerBg, 8), 250);
    CHECK_EQ(warm(f, F(kVorL, 10, 20, 150, 136)), 0);
    CHECK_EQ(cnt(f, F(kVorL, 169, 48, 102, 16), kAccent, 24), 0);
    vor.lock = true;
    // the course is a user's setting: on a bearing of 090 (due east of the station) a course of 090 is FROM, on the bar's centre
    vor.values = vals({90, 0.92f, 0.31f, 0.30f});
    ui.set_st_vor(vor);
    f = shot(ui);
    checkVorBar(f, 90, 90);
    CHECK_GT(cnt(f, F(kVorL, 202, 107, 70, 13), kGreen, 24), 25);
    CHECK_EQ(cnt(f, F(kVorL, 168, 107, 32, 13), kGreen, 24), 0);
    // a bar a long way off its course sits on its stop and stays inside the box
    ui.set_obs(150);
    f = shot(ui);
    CHECK_GE(cnt(f, F(kVorL, 262, 48, 9, 16), kAccent, 12), 30);
    CHECK_GE(cnt(f, F(kVorL, 169, 48, 102, 16), kAccent, 12), 30);
    CHECK_LE(cnt(f, F(kVorL, 169, 48, 102, 16), kAccent, 12), 40);
    vor.values = vals({274, 0.92f, 0.31f, 0.30f});
    ui.set_st_vor(vor);
    ui.set_obs(90);

    // the beacon: a carrier error of exactly zero is an untouched slot and draws no needle; -4800 Hz sits far left of the index
    auto bc = ui.get_st_beacon();
    bc.values = vals({0, 134});
    ui.set_st_beacon(bc);
    f = shot(ui);
    CHECK_EQ(cnt(f, F(kBcnL, 12, 151, 446, 16), kAccent, 24), 0);
    CHECK_EQ(ink(f, F(kBcnL, 360, 123, 98, 24), kPanel), 0);
    bc.values = vals({-4800, 3725});
    ui.set_st_beacon(bc);
    f = shot(ui);
    CHECK_GE(cnt(f, F(kBcnL, 12 + 223 - 214, 151, 8, 16), kAccent, 12), 14);
    CHECK_GT(cnt(f, F(kBcnL, 360, 123, 98, 24), kAccentSoft, 40), 20);
    // the carrier error is in hertz while it is a hertz-sized number and in kilohertz from ten thousand ("+12.3 kHz" is two characters wider than "+210 Hz")
    bc.values = vals({210, 134});
    ui.set_st_beacon(bc);
    f = shot(ui);
    const int hzLeft = leftInk(f, F(kBcnL, 300, 123, 158, 24), kPanel);
    bc.values = vals({12345, 134});
    ui.set_st_beacon(bc);
    f = shot(ui);
    CHECK(hzLeft > 0 && leftInk(f, F(kBcnL, 300, 123, 158, 24), kPanel) > 0 && hzLeft - leftInk(f, F(kBcnL, 300, 123, 158, 24), kPanel) >= 10);
    CHECK_GE(cnt(f, F(kBcnL, 12 + 223 + 215, 151, 8, 16), kAccent, 12), 14);          // 12345 Hz is past the stop: the needle is held on it
    bc.values = vals({210, 134});
    ui.set_st_beacon(bc);

    // the fax: the phase word lights its own lamp and none other; the tuning needle waits for the phasing signal (an error measured before it
    // would be the face claiming a perfectly tuned receiver on the strength of an empty slot); an unknown word lights nothing
    auto fx = ui.get_st_fax();
    struct Phase { const char* word; int lamp; bool needle; };
    const std::array<Phase, 7> phases = {{{"IDLE", 0, false}, {"START", 1, false}, {"PHASING", 2, true}, {"PICTURE", 3, true}, {"STOP", 4, true}, {"picture", -1, false}, {"", -1, false}}};
    const std::array<Pos, 5> lampAt = {{{12, 27}, {115, 27}, {12, 46}, {115, 46}, {12, 65}}};
    for (const Phase& ph : phases) {
        fx.texts = texts({ph.word});
        ui.set_st_fax(fx);
        f = shot(ui);
        for (int i = 0; i < 5; ++i) { CHECK((cnt(f, F(kFaxL, lampAt[i].x, lampAt[i].y, 10, 13), kGreen, 24) > 25) == (i == ph.lamp)); }
        CHECK((cnt(f, F(kFaxL, 224, 29, 234, 30), kAccent, 12) >= 30) == ph.needle);
        CHECK((cnt(f, F(kFaxL, 224, 67, 100, 16), kAccentSoft, 40) > 15) == ph.needle);
    }
    fx.texts = texts({"PICTURE"});
    ui.set_st_fax(fx);
    // a fax that has heard no line shows dashes in its counter (zero lines is not a reading), one that has shows the count
    fx.values = vals({576, 120, 0, 40});
    ui.set_st_fax(fx);
    f = shot(ui);
    CHECK(f.near(F(kFaxL, 12 + 3, 167 + 3, 1, 1).x, F(kFaxL, 12 + 3, 167 + 3, 1, 1).y, kBg, 6));
    CHECK_EQ(cnt(f, F(kFaxL, 238, 110, 206, 70), kFgSoft, 12), 0);                        // and no paper
    fx.values = vals({576, 120, 312, 40});
    ui.set_st_fax(fx);

    // the pager with nothing unread: the count is a real zero (two cells, 00) and there is no chip; the printer that has printed nothing says 0000
    pg = ui.get_st_pager();
    pg.values = vals({0});
    ui.set_st_pager(pg);
    auto tl = ui.get_st_tele();
    tl.values = vals({0});
    tl.texts = texts({"", "", "", "", ""});
    ui.set_st_tele(tl);
    ui.set_paper_newest("");
    ui.set_paper_previous("");
    f = shot(ui);
    CHECK_EQ(cnt(f, F(kPagL, 170, 92, 92, 21), kChipWarnBg, 4), 0);
    for (int i = 0; i < 2; ++i) { CHECK(f.near(F(kPagL, 274 + 22 * i + 3, 116 + 3, 1, 1).x, F(kPagL, 274 + 22 * i + 3, 116 + 3, 1, 1).y, kField, 6)); CHECK_GT(cnt(f, F(kPagL, 275 + 22 * i, 117, 18, 26), kDigit, 40), 8); }
    for (int i = 0; i < 4; ++i) { CHECK_GT(cnt(f, F(kTelL, 295 + 22 * i, 91, 18, 26), kDigit, 40), 8); }
    // PAPER lights only when something is printed on the strip; READY when the feed is alive; the legend says what the strip lacks
    CHECK_EQ(cnt(f, F(kTelL, 294, 29, 79, 13), kGreen, 24), 0);
    CHECK_GT(cnt(f, F(kTelL, 379, 29, 79, 13), kGreen, 24), 25);
    CHECK_EQ(dark(f, in4(F(kTelL, 12, 8, 270, 122))), 0);
    CHECK_GT(ink(f, in4(F(kTelL, 12, 8, 270, 122)), kFgSoft, 30), 25);
    pg.values = vals({2});
    ui.set_st_pager(pg);
    tl.values = vals({42});
    tl.texts = texts({"G-EZBX", "BAW123", "H1", "2", "7"});
    ui.set_st_tele(tl);
    ui.set_paper_newest("DEP EGLL 1432Z RWY 27L");
    ui.set_paper_previous("DEP EGKK 1428Z RWY 26L");

    // the generic face of a kind from the future shows EVERY filled slot, sixteen of them in six rows, and drops none
    ui.set_sheet(2);
    auto fut = ui.get_st_future();
    fut.values = vals({1, 2, 3, 4, 5, 6, 7, 8});
    fut.texts = texts({"a", "b", "c", "d", "e", "f", "g", "h"});
    ui.set_st_future(fut);
    f = shot(ui, 920, 2100);
    {
        const Pos t{8, 1671};
        CHECK_NEAR(tileHeight(f, t.x, t.y), 35 + 6 * 41 + 5 * 8 + 8 + 16 + 10 + 2, 1.0);
        for (int i = 0; i < 16; ++i) {
            const int col = i % 3, row = i / 3;
            const int x = 12 + col * 294, y = 52 + row * 49;
            CHECK_GT(ink(f, F(t, x + 4, y + 4, 280, 16), kField), 4);
        }
        CHECK_GT(cnt(f, F(t, 12, 35 + 5 * 49 + 41 + 8, 90, 16), kAccentSoft, 40), 20);
    }
    ui.set_sheet(0);
}

void checkStale(InstrumentsSpecimen& ui) {
    // the host leaves the last reading in the struct when it clears `have`: nothing of it may reach the face. Every flag stuck on, every slot
    // filled, a picture in the chart slot - and every empty tile must still show exactly what an untouched state shows.
    const auto none0 = ui.get_st_none();
    auto none = none0;
    none.alert = true; none.lock = true; none.low_batt = true; none.unread = true; none.seq = 99; none.now_ms = 0;
    none.values = vals({5, 6, 7, 8, 9, 10, 7, 7});
    none.texts = texts({"STALE", "STALE", "STALE", "STALE", "STALE", "STALE", "STALE", "STALE"});
    none.chart = twoHalves(16, 24);
    ui.set_st_none(none);
    const Frame f = shot(ui);
    checkGenericEmpty(f);
    checkPagerEmpty(f);
    checkTeleprinterEmpty(f);
    checkToneEmpty(f);
    checkVorEmpty(f);
    checkFaxEmpty(f);
    checkBeaconEmpty(f);
    checkMeterEmpty(f);
    checkWeatherEmpty(f);
    ui.set_st_none(none0);
}

void checkGarbage(InstrumentsSpecimen& ui) {
    // a plugin may leave a NaN or an infinity in a slot: the face draws no figure, no needle and no marker for it (never a number it was not told)
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const auto pager0 = ui.get_st_pager(), tele0 = ui.get_st_tele(), tone0 = ui.get_st_tone(), vor0 = ui.get_st_vor(), fax0 = ui.get_st_fax();
    const auto bcn0 = ui.get_st_beacon(), met0 = ui.get_st_meter(), wx0 = ui.get_st_weather();
    for (const float bad : {nan, inf}) {
        auto pg = pager0; pg.values = vals({bad, bad, bad, bad, bad, bad, bad, bad}); ui.set_st_pager(pg);
        auto tl = tele0; tl.values = vals({bad, bad, bad, bad, bad, bad, bad, bad}); ui.set_st_tele(tl);
        auto tn = tone0; tn.values = vals({bad, bad, bad, bad, bad, bad, bad, bad}); ui.set_st_tone(tn);
        auto vr = vor0; vr.values = vals({bad, bad, bad, bad, bad, bad, bad, bad}); ui.set_st_vor(vr);
        auto fx = fax0; fx.values = vals({bad, bad, bad, bad, bad, bad, bad, bad}); ui.set_st_fax(fx);
        auto bc = bcn0; bc.values = vals({bad, bad, bad, bad, bad, bad, bad, bad}); ui.set_st_beacon(bc);
        auto mt = met0; mt.values = vals({bad, bad, bad, bad, bad, bad, bad, bad}); ui.set_st_meter(mt);
        auto wx = wx0; wx.values = vals({bad, bad, bad, bad, bad, bad, bad, bad}); ui.set_st_weather(wx);
        const Frame f = shot(ui);
        // a NaN count is not a count: the pager has no unread figure (ghost cells, no chip), the printer dashes in its counter, the fax dashes
        // in its line counter. (An infinity is "more than the drums hold": all nines, which is what a counter that cannot say more says.)
        if (bad != bad) {
            CHECK_EQ(cnt(f, F(kPagL, 170, 92, 92, 21), kChipWarnBg, 4), 0);
            for (int i = 0; i < 2; ++i) { CHECK(f.near(F(kPagL, 274 + 22 * i + 3, 116 + 3, 1, 1).x, F(kPagL, 274 + 22 * i + 3, 116 + 3, 1, 1).y, kBg, 6)); }
            for (int i = 0; i < 4; ++i) { CHECK(f.near(F(kTelL, 294 + 22 * i + 3, 90 + 3, 1, 1).x, F(kTelL, 294 + 22 * i + 3, 90 + 3, 1, 1).y, kBg, 6)); }
            CHECK(f.near(F(kFaxL, 12 + 3, 167 + 3, 1, 1).x, F(kFaxL, 12 + 3, 167 + 3, 1, 1).y, kBg, 6));
        } else {
            CHECK_GT(cnt(f, F(kPagL, 275, 117, 40, 26), kDigit, 40), 15);
            CHECK_GT(cnt(f, F(kTelL, 295, 91, 84, 26), kDigit, 40), 30);
        }
        // the tone alert: two bare tracks and two dashes
        CHECK_EQ(cnt(f, F(kTonL, 100, 30, 366, 18), kAccent, 24) + cnt(f, F(kTonL, 100, 72, 366, 18), kAccent, 24), 0);
        CHECK_EQ(cnt(f, F(kTonL, 360, 8, 98, 24), kAccentSoft, 40) + cnt(f, F(kTonL, 360, 50, 98, 24), kAccentSoft, 40), 0);
        // the VOR: no lock on a radial that is not a number, so the flag, no needle, no marker
        CHECK_GT(cnt(f, F(kVorL, 168, 126, 104, 16), kDangerBg, 8), 250);
        CHECK_EQ(warm(f, F(kVorL, 10, 20, 150, 136)), 0);
        CHECK_EQ(cnt(f, F(kVorL, 169, 48, 102, 16), kAccent, 24), 0);
        // the fax: no tuning needle, no line count; the beacon: no carrier needle, no age; the meter: no figure
        CHECK_EQ(cnt(f, F(kFaxL, 224, 29, 234, 30), kAccent, 24), 0);
        CHECK_EQ(cnt(f, F(kBcnL, 12, 151, 446, 16), kAccent, 24), 0);
        CHECK_EQ(ink(f, F(kBcnL, 16, 194, 68, 16), kField), 0);
        CHECK_EQ(dark(f, F(kMetL, 23, 34, 8 * 22, 28)), 0);
        // the weather console: a mask that is not a number hears no channel, whatever its temperatures say
        for (int i = 0; i < 3; ++i) { CHECK_EQ(dark(f, in4(F(kWxL, 21 + 151 * i, 34, 125, 34))), 0); }
        CHECK_GT(cnt(f, F(kWxL, 12, 159, 250, 16), kDim, 40), 40);
    }
    ui.set_st_pager(pager0); ui.set_st_tele(tele0); ui.set_st_tone(tone0); ui.set_st_vor(vor0); ui.set_st_fax(fax0);
    ui.set_st_beacon(bcn0); ui.set_st_meter(met0); ui.set_st_weather(wx0);
    // a channel that IS heard (the mask says so) but whose temperature is a NaN reads dashes, not 0.0 C ...
    auto wx = wx0;
    wx.values = vals({nan, nan, nan, 56, 56, 56, 7, 0});
    ui.set_st_weather(wx);
    Frame f = shot(ui);
    for (int i = 0; i < 3; ++i) {
        CHECK_EQ(dark(f, in4(F(kWxL, 21 + 151 * i, 34, 125, 34))), 0);
        CHECK_GT(ink(f, in4(F(kWxL, 21 + 151 * i, 34, 125, 34)), kFgSoft, 30), 15);
        CHECK_GT(dark(f, in4(F(kWxL, 21 + 151 * i, 72, 125, 34))), 12);                 // while its humidity is a real 56 %
    }
    // ... and one that is an infinity is a number off the field: HHH and LLL
    wx.values = vals({inf, -inf, 21.4f, 56, 56, 56, 7, 0});
    ui.set_st_weather(wx);
    f = shot(ui);
    for (int i = 0; i < 3; ++i) { CHECK_GT(dark(f, in4(F(kWxL, 21 + 151 * i, 34, 125, 34))), 15); }
    // a tuning error that is not a number draws no needle on a fax that is in its picture phase
    auto fx = fax0;
    fx.values = vals({576, 120, 312, nan});
    ui.set_st_fax(fx);
    f = shot(ui);
    CHECK_EQ(cnt(f, F(kFaxL, 224, 29, 234, 30), kAccent, 24), 0);
    CHECK_EQ(warm(f, F(kFaxL, 224, 67, 100, 16)), 0);
    ui.set_st_fax(fax0);
    ui.set_st_weather(wx0);
}

void checkSkin(InstrumentsSpecimen& ui) {
    // one filling reaches every pixel: each token the faces read gets a colour of its own, in one assignment
    Skin probe = ui.global<Workbench>().get_value();
    probe.digit = col(0x00, 0xff, 0xff);
    probe.field = col(0x30, 0x00, 0x30);
    probe.fg_soft = col(0xc0, 0xff, 0xc0);
    probe.bg = col(0x00, 0x00, 0x80);
    probe.lamp_green = col(0xff, 0x00, 0xff);
    probe.accent = col(0xff, 0x00, 0x00);
    probe.focus = col(0xff, 0xff, 0x00);
    probe.lamp_ring = col(0x80, 0x80, 0x00);
    probe.knob_ring = col(0x00, 0x80, 0x80);
    probe.danger_line = col(0x00, 0x80, 0x00);
    ui.global<Theme>().set_skin(probe);
    const Frame f = shot(ui);
    const Rgb pDigit = rgb(0x00ffff), pField = rgb(0x300030), pSoft = rgb(0xc0ffc0), pInk = rgb(0x000080), pGreen = rgb(0xff00ff), pFocus = rgb(0xffff00);
    CHECK(at(f, F(kPagL, 15, 20, 1, 1), pField, 6));                                  // the LCD's ground
    CHECK_GT(cnt(f, F(kPagL, 21, 12, 144, 48), pDigit, 40), 150);                      // its phosphor
    CHECK_EQ(cnt(f, F(kPagL, 21, 12, 144, 48), kDigit, 40), 0);
    CHECK(at(f, F(kTelL, 135, 20, 1, 1), pSoft, 8));                                  // the paper's ground is fg-soft ...
    CHECK(at(f, F(kMetL, 16, 12, 1, 1), pSoft, 8));                                   // ... so is the pale LCD's
    {
        int n = 0;
        const R r = F(kMetL, 23, 34, 8 * 22, 28);
        for (int y = r.y; y < r.y + r.h; ++y) { for (int x = r.x; x < r.x + r.w; ++x) { if (f.near(x, y, pInk, 16)) { ++n; } } }
        CHECK_GT(n, 60);                                                               // the pale LCD's ink is bg
    }
    CHECK(at(f, F(kVorL, 65, 81, 1, 1), pFocus, 24));                                 // the knob's ring is focus
    CHECK_GT(cnt(f, F(kFaxL, 12, 27, 200, 70), pGreen, 24), 25);                       // a lit green lamp
    CHECK(at(f, F(kGenL, 14, 56, 1, 1), pField, 6));                                  // a glass is field-coloured
    CHECK_GT(cnt(f, F(kGenL, 190, 11, 268, 15), rgb(0x808000), 8), 4);                 // a lamp's ring is lamp-ring ...
    CHECK_GT(cnt(f, F(kBcnE, 13, 30, 18, 26), rgb(0x008080), 24), 6);                  // ... a ghost cell's ink is knob-ring
    f.savePng(shotDir() + "instruments-specimen-probe-filling.png");
    ui.global<Theme>().set_skin(ui.global<Workbench>().get_value());
}

// ===== the narrow and the wide sheets ==============================================================================================
struct TileAt { const char* name; Pos at; int h; };

void checkNarrow(InstrumentsSpecimen& ui, slint::Window& w) {
    ui.set_sheet(1);
    Frame f = render(ui.window(), 1000, 1200);
    f.savePng(shotDir() + "instruments-specimen-320.png");
    // each face, 322 px wide, stacks its columns instead of squeezing them: the frame holds everything, the gaps stay clean
    const std::array<TileAt, 9> tiles = {{{"generic", {8, 8}, 180}, {"pager", {338, 8}, 320}, {"tele", {668, 8}, 258}, {"tone", {8, 336}, 208}, {"vor", {338, 336}, 225}, {"fax", {668, 336}, 387},
                                          {"beacon", {8, 731}, 224}, {"meter", {338, 731}, 195}, {"weather", {668, 731}, 183}}};
    for (const TileAt& t : tiles) {
        CHECK_NEAR(tileHeight(f, t.at.x, t.at.y), t.h, 1.0);
        const int h = t.h;
        CHECK(f.near(t.at.x, t.at.y + h / 2, kLine, 6) && f.near(t.at.x + 321, t.at.y + h / 2, kLine, 6));
        CHECK(f.near(t.at.x - 1, t.at.y + h / 2, kBg, 6) && f.near(t.at.x + 322, t.at.y + h / 2, kBg, 6));
        CHECK(f.near(t.at.x + 100, t.at.y + h - 1, kLine, 6) && f.near(t.at.x + 100, t.at.y + h, kBg, 6));
        // no ink in the sheet's gutters beside the tile, none under the tile
        CHECK_EQ(ink(f, {t.at.x + 322, t.at.y, 8, h}, kBg, 6), 0);
        CHECK_EQ(ink(f, {t.at.x, t.at.y + h, 322, 6}, kBg, 6), 0);
        // the face's last ink is clear of the frame's bottom edge: nothing is cut off
        CHECK_EQ(ink(f, {t.at.x + 12, t.at.y + h - 6, 298, 5}, kPanel, 6), 0);
        // and the face has its words: ink well inside the frame
        CHECK_GT(ink(f, {t.at.x + 2, t.at.y + 2, 318, h - 8}, kPanel, 6), 400);
    }
    // the stacked faces: the right-hand column of the wide layout sits under the left-hand one
    CHECK_GT(cnt(f, {338 + 1 + 12, 8 + 1 + 152, 184, 13}, kAccent, 24), 25);           // the pager's lamps under its plate
    CHECK_GT(cnt(f, {668 + 1 + 12, 8 + 1 + 138, 300, 13}, kMuted, 40), 20);             // the printer's STATUS under its paper
    CHECK_GT(cnt(f, {668 + 1 + 12, 336 + 1 + 207, 300, 13}, kMuted, 40), 20);           // the fax's TUNING under its lamps
    CHECK_GT(cnt(f, {338 + 1 + 10, 336 + 1 + 169, 150, 14}, kDim, 40), 15);             // the VOR's readouts under its dial
    // beacon at 320: fifteen cells inside the frame, 17 px wide on a 19 px pitch
    {
        const Pos t{8, 731};
        CHECK(at(f, F(t, 12, 43, 1, 1), kLine, 6) && at(f, F(t, 12 + 14 * 19 + 16, 43, 1, 1), kLine, 6));
        for (int i = 0; i < 15; ++i) { CHECK_GT(cnt(f, F(t, 13 + 19 * i, 30, 15, 26), kDigit, 40), 6); }
        CHECK_EQ(ink(f, F(t, 12 + 14 * 19 + 18, 29, 10, 28), kPanel), 0);
    }
    // the VOR with no host binding: the knob keeps the user's own position (wheel 91, then 92), and tells the host each time
    {
        const float kx = 8 + 1 + 85, ky = 963 + 1 + 81;
        moveTo(w, kx, ky);
        scroll(w, kx, ky, 0.0f, 60.0f);
        CHECK(ui.get_free_events() == 1 && ui.get_free_last() == 91);
        scroll(w, kx, ky, 0.0f, 60.0f);
        CHECK(ui.get_free_events() == 2 && ui.get_free_last() == 92);
        // the disabled one (45 %, every ground dimmed) takes no wheel and no drag
        const float dx = 338 + 1 + 85;
        scroll(w, dx, ky, 0.0f, 60.0f);
        press(w, dx + 14.0f, ky, slint::PointerEventButton::Left);
        moveTo(w, dx, ky + 14.0f);
        release(w, dx, ky + 14.0f, slint::PointerEventButton::Left);
        CHECK(ui.get_dis_events() == 0);
        f = render(ui.window(), 1000, 1200);
        CHECK(!f.near(338 + 1 + 65, 963 + 1 + 81, kFocus, 24));              // its ring is dimmed ...
        CHECK_GT(ink(f, {338 + 1 + 62, 963 + 1 + 78, 8, 6}, kField, 12), 3);   // ... but drawn
        CHECK_EQ(cnt(f, {339, 964, 320, 225}, kFocus, 40), 0);                // no cyan at full strength anywhere on it
        CHECK_EQ(cnt(f, {339, 964, 320, 225}, kAccent, 12), 0);
        CHECK_EQ(cnt(f, {339, 964, 320, 225}, kGreen, 12), 0);
        moveTo(w, 900, 1190);
    }
    // the tooltip: the weather tile's sentence appears after the pointer rests, below the face
    {
        const Pos t{668, 963};
        const Frame before = render(ui.window(), 1000, 1200);
        CHECK_LE(cnt(before, {t.x, 963 + 185, 320, 12}, kTip, 2), 40);
        moveTo(w, t.x + 160.0f, 963 + 100.0f);
        settle(std::chrono::milliseconds(750));
        const Frame tip = render(ui.window(), 1000, 1200);
        CHECK_GT(cnt(tip, {t.x, 963 + 185, 320, 40}, kTip, 2), 800);
        moveTo(w, 900, 1190);
        settle(std::chrono::milliseconds(50));
    }
}

void checkWide(InstrumentsSpecimen& ui) {
    ui.set_sheet(2);
    const Frame f = render(ui.window(), 920, 1850);
    f.savePng(shotDir() + "instruments-specimen-900.png");
    const std::array<Pos, 9> at9 = {{{8, 8}, {8, 177}, {8, 361}, {8, 509}, {8, 725}, {8, 908}, {8, 1121}, {8, 1353}, {8, 1480}}};
    const std::array<int, 9> h = {161, 176, 140, 208, 175, 205, 224, 119, 183};
    for (int i = 0; i < 9; ++i) {
        CHECK_NEAR(tileHeight(f, at9[i].x, at9[i].y), h[i], 1.0);
        CHECK(f.near(at9[i].x + 901, at9[i].y + h[i] / 2, kLine, 6) && f.near(at9[i].x + 902, at9[i].y + h[i] / 2, kBg, 6));
        CHECK_EQ(ink(f, {at9[i].x + 902, at9[i].y, 10, h[i]}, kBg, 6), 0);
        CHECK_EQ(ink(f, {at9[i].x, at9[i].y + h[i], 902, 8}, kBg, 6), 0);
    }
    // the flexible columns grew; the fixed ones stayed: the pager's LCD is 250, the paper 270, the tone alert's glass boxes 387
    box(f, F(at9[1], 12, 8, 250, 78), kLine, kField, kPanel);
    box(f, F(at9[2], 12, 8, 270, 122), kFgSoft, kFgSoft, kPanel);
    box(f, F(at9[3], 108, 107, 387, 24), kLine, kField, kPanel);
    CHECK(at(f, F(at9[3], 108 + 5, 39, 1, 1), kAccent, 12));
    // the generic face spreads its three columns over the width (286 wide, 8 apart)
    box(f, F(at9[0], 12, 52, 286, 24), kLine, kField, kPanel);
    box(f, F(at9[0], 306, 52, 286, 24), kLine, kField, kPanel);
    // a kind from the future gets the generic face, with every filled slot on glass, under its own title
    const Pos t{8, 1671};
    CHECK_NEAR(tileHeight(f, t.x, t.y), 161, 1.0);
    CHECK_GT(cnt(f, F(t, 12, 10, 400, 17), kFg, 40), 40);
    box(f, F(t, 12, 52, 286, 24), kLine, kField, kPanel);
    CHECK_GT(cnt(f, F(t, 12, 133, 90, 16), kAccentSoft, 40), 20);
}

}  // namespace

int main() {
    installHeadlessPlatform();
    {
        auto ui = InstrumentsSpecimen::create();
        slint::Window& w = ui->window();
        const Frame f = shot(*ui);
        f.savePng(shotDir() + "instruments-specimen.png");

        checkSheet(f);
        checkGeneric(f);
        checkGenericEmpty(f);
        checkPager(f);
        checkPagerEmpty(f);
        checkTeleprinter(f);
        checkTeleprinterEmpty(f);
        checkTone(f);
        checkToneEmpty(f);
        checkVorLive(f);
        checkVorEmpty(f);
        checkFax(f);
        checkFaxEmpty(f);
        checkBeacon(f);
        checkBeaconEmpty(f);
        checkMeter(f);
        checkMeterEmpty(f);
        checkWeather(f);
        checkWeatherEmpty(f);

        checkObs(*ui, w);
        checkBlink(*ui);
        checkFeed(*ui);
        checkChart(*ui);
        checkStates(*ui);
        checkStale(*ui);
        checkGarbage(*ui);
        checkSkin(*ui);
    }
    {
        auto ui = InstrumentsSpecimen::create();
        slint::Window& w = ui->window();
        checkNarrow(*ui, w);
    }
    {
        auto ui = InstrumentsSpecimen::create();
        checkWide(*ui);
    }
    return testSummary("test_slint_instruments");
}
