// The map's picture, composed on the host's side (src/slint/map_compositor.*): no Slint, no window, no engine - the compositor paints into a buffer
// and the test reads the pixels. What is held, section by section:
//   A. the coastline: land under the centre of a 512 x 384 view on 51.5N 0.1W at 20 degrees, sea at 45N 20W, and the world's rings as the
//      data really is (14 of its 134 runs are fragments cut at the antimeridian: the Americas, Eurasia-and-Africa, Antarctica...), in
//      both projections, with the Caspian a hole and the antimeridian seamless
//   B. the graticule: lon 0 is ONE pixel column of #1e2229 where no land covers it; a parallel is one row
//   C. the range rings: a ring of 50 km around the home has the radius metresPerPixel() says (and every pixel of it is 50 km from the home on
//      the ground, in either projection); a nice step so three to six rings show
//   D. the tiles: a solid red tile paints red where it lands and is asked once per tile per compose; PENDING is asked again, MISSING is not
//      asked for 600 composes; READY is cached (256 at most, least recently used out); bilinear off scale 1; the wrap; a short buffer is MISSING
//   E. project / unproject round-trip to 1e-6 degrees in both projections, across the antimeridian too
//   F. compose() returns false when nothing changed and true after any setter, and leaves the buffer alone
//   G. the coverage wedges, the trails (width, dash 6 on 4 off, closed, translucent, across the antimeridian)
//   H. the figures: metresPerPixel, the clamps, a place that is not a place, and the time of a 1024 x 768 compose
//   I. the display settings the map applies (SYSTEM > Display: Aircraft icons, Trail width, the trail-style list): the width a trail paints at, the two
//      styles told apart, the classic window's bounds (icon 16..96, trail 1..wingspan, 0 paints as 1), the dash that scales with the width, the
//      icon size handed to a target, and the copied constants read against gui/aircraft_icons.hpp
// It also writes a few pictures (slint-shots/map-compositor-*.png, or $FOXSDR_SLINT_SHOTS) for a person to look at.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "map_compositor.hpp"
#include "test_check.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "../../../third_party/glfw/deps/stb_image_write.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

using namespace foxsdr::slintmap;

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Rgb {
    int r = 0, g = 0, b = 0;
};
constexpr Rgb kSea{0x0d, 0x0f, 0x12};
constexpr Rgb kLand{0x1d, 0x20, 0x26};
constexpr Rgb kCoast{0x43, 0x49, 0x55};
constexpr Rgb kGraticule{0x1e, 0x22, 0x29};
constexpr Rgb kRingColour{0x43, 0x49, 0x55};
constexpr Rgb kWedgeBlue{0x2e, 0x86, 0xc1};
constexpr Rgb kOrange{0xf5, 0xa5, 0x24};

bool near(const Rgb& a, const Rgb& b, int tol = 2) { return std::abs(a.r - b.r) <= tol && std::abs(a.g - b.g) <= tol && std::abs(a.b - b.b) <= tol; }
Rgb over(const Rgb& fg, double alpha, const Rgb& bg) {
    const auto mix = [alpha](int f, int b) { return static_cast<int>(std::lround(f * alpha + b * (1.0 - alpha))); };
    return {mix(fg.r, bg.r), mix(fg.g, bg.g), mix(fg.b, bg.b)};
}

// A picture the compositor paints into, kept alive (the compositor compares buffers).
struct Pic {
    int w = 0, h = 0;
    std::vector<std::uint8_t> px;
    Pic(int width, int height) : w(width), h(height), px(static_cast<std::size_t>(width) * height * 4u, 0u) {}
    Rgb at(int x, int y) const {
        if (x < 0 || y < 0 || x >= w || y >= h) { return {-1000, -1000, -1000}; }
        const std::uint8_t* p = &px[(static_cast<std::size_t>(y) * w + x) * 4u];
        return {p[0], p[1], p[2]};
    }
    int alphaAt(int x, int y) const { return px[(static_cast<std::size_t>(y) * w + x) * 4u + 3u]; }
    std::uint8_t* data() { return px.data(); }
    bool compose(TileCompositor& c) {
        c.setSize(w, h);
        return c.compose(px.data(), w, h);
    }
    int count(const Rgb& want, int x0, int y0, int x1, int y1, int tol = 2) const {
        int n = 0;
        for (int y = y0; y < y1; ++y) { for (int x = x0; x < x1; ++x) { if (near(at(x, y), want, tol)) { ++n; } } }
        return n;
    }
};

// The pixel a place lands on, by the compositor's own project().
bool pixelOf(const TileCompositor& c, const Pic& p, double lat, double lon, int& x, int& y) {
    double fx = 0.0, fy = 0.0;
    const bool on = c.project(lat, lon, fx, fy);
    x = static_cast<int>(std::floor(fx * p.w));
    y = static_cast<int>(std::floor(fy * p.h));
    return on;
}

double lonDelta(double a, double b) {
    double d = std::fmod(b - a + 180.0, 360.0);
    if (d < 0.0) { d += 360.0; }
    return d - 180.0;
}

double haversineKm(double lat1, double lon1, double lat2, double lon2) {
    const double r = kPi / 180.0;
    const double dp = (lat2 - lat1) * r, dl = (lon2 - lon1) * r;
    const double a = std::sin(dp / 2) * std::sin(dp / 2) + std::cos(lat1 * r) * std::cos(lat2 * r) * std::sin(dl / 2) * std::sin(dl / 2);
    return 2.0 * 6371.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
}

// Where the shots go: $FOXSDR_SLINT_SHOTS, else slint-shots under the working directory.
std::string shotDir() {
    std::string dir = "slint-shots";
    if (const char* e = std::getenv("FOXSDR_SLINT_SHOTS"); e != nullptr && *e != '\0') { dir = e; }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (dir.back() != '/' && dir.back() != '\\') { dir += '/'; }
    return dir;
}
void savePng(const Pic& p, const std::string& name) {
    std::vector<unsigned char> out;
    const auto sink = [](void* ctx, void* data, int size) {
        auto* v = static_cast<std::vector<unsigned char>*>(ctx);
        const auto* b = static_cast<const unsigned char*>(data);
        v->insert(v->end(), b, b + size);
    };
    if (stbi_write_png_to_func(sink, &out, p.w, p.h, 4, p.px.data(), p.w * 4) == 0) { return; }
    const std::string path = shotDir() + name;
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    std::printf("  picture: %s\n", path.c_str());
}

// A tile source that counts every question and answers by a function of the tile.
struct Source {
    std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>, int> asks;
    int total = 0;
    std::function<int(std::uint32_t, std::uint32_t, std::uint32_t, std::vector<std::uint8_t>&)> answer;
    TileSource make() {
        return [this](std::uint32_t z, std::uint32_t x, std::uint32_t y, std::vector<std::uint8_t>& rgb) {
            ++asks[{z, x, y}];
            ++total;
            return answer(z, x, y, rgb);
        };
    }
    int askedOf(std::uint32_t z, std::uint32_t x, std::uint32_t y) const {
        const auto it = asks.find({z, x, y});
        return it == asks.end() ? 0 : it->second;
    }
    int maxAsks() const {
        int m = 0;
        for (const auto& kv : asks) { m = std::max(m, kv.second); }
        return m;
    }
    void forget() {
        asks.clear();
        total = 0;
    }
};
void fillTile(std::vector<std::uint8_t>& rgb, std::uint32_t size, int r, int g, int b) {
    rgb.resize(static_cast<std::size_t>(size) * size * 3u);
    for (std::size_t i = 0; i < rgb.size(); i += 3) {
        rgb[i] = static_cast<std::uint8_t>(r);
        rgb[i + 1] = static_cast<std::uint8_t>(g);
        rgb[i + 2] = static_cast<std::uint8_t>(b);
    }
}

// ----------------------------------------------------------------------------------------------------------------------------
// A. the coastline
// ----------------------------------------------------------------------------------------------------------------------------
struct Place {
    const char* name;
    double lat, lon;
    bool land;
};
// Open sea and the middle of continents: nothing near enough a coast for a 1:110m outline to matter at 1024 px across the world.
const Place kPlaces[] = {
    {"London", 51.5, -0.1, true},         {"Paris", 48.9, 2.35, true},         {"Berlin", 52.5, 13.4, true},
    {"Moscow", 55.75, 37.6, true},        {"Cairo", 30.0, 31.2, true},         {"Johannesburg", -26.2, 28.0, true},
    {"Nairobi", -1.3, 36.8, true},        {"Delhi", 28.6, 77.2, true},         {"Beijing", 39.9, 116.4, true},
    {"Alice Springs", -23.7, 133.9, true}, {"Denver", 39.7, -105.0, true},     {"Mexico City", 19.4, -99.1, true},
    {"Brasilia", -15.8, -47.9, true},     {"Cordoba", -31.4, -64.2, true},     {"Fairbanks", 64.8, -147.7, true},
    {"Greenland", 72.0, -40.0, true},     {"Antarctica", -80.0, 0.0, true},    {"Antarctica E", -75.0, 120.0, true},
    {"Siberia", 66.0, 100.0, true},       {"Chukotka W", 66.0, 175.0, true},   {"Chukotka E", 66.0, -172.0, true},
    {"Iceland", 64.9, -18.5, true},       {"Madagascar", -19.0, 46.5, true},   {"Borneo", 0.5, 114.0, true},
    {"Central Asia", 47.0, 70.0, true},
    {"Pacific", 0.0, -150.0, false},      {"N Atlantic", 30.0, -40.0, false},  {"Indian Ocean", -30.0, 80.0, false},
    {"Gulf of Guinea", 0.0, 0.0, false},  {"45N 20W", 45.0, -20.0, false},     {"S Pacific", -50.0, -120.0, false},
    {"Caspian Sea", 42.0, 50.5, false},   {"Black Sea", 43.2, 34.0, false},    {"Mediterranean", 35.0, 18.0, false},
    {"Hudson Bay", 60.0, -86.0, false},   {"Bering Sea", 58.0, -175.0, false}, {"Chukchi Sea", 71.0, -170.0, false},
    {"Bay of Bengal", 15.0, 88.0, false}, {"Baltic", 57.5, 19.5, false},       {"Sea of Japan", 40.0, 135.0, false},
};

void sectionCoast() {
    std::printf("A. the coastline\n");
    // ---- the contract's two checks: 512 x 384, 51.5N 0.1W, 20 degrees: land at the centre ----
    {
        TileCompositor c;
        Pic p(512, 384);
        c.setView(51.5, -0.1, 20.0);
        c.setLayers(true, false);
        CHECK(p.compose(c));
        CHECK(near(p.at(256, 192), kLand));                                   // London
        CHECK(p.alphaAt(256, 192) == 255 && p.alphaAt(0, 0) == 255 && p.alphaAt(511, 383) == 255);   // the picture is opaque
        int x = 0, y = 0;
        CHECK(pixelOf(c, p, 51.5, -0.1, x, y) && x == 256 && y == 192);
        // and around it, as the map is: Great Britain and Ireland land, the Celtic Sea and the North Sea sea
        CHECK(pixelOf(c, p, 53.0, -1.5, x, y) && near(p.at(x, y), kLand));    // the Midlands
        CHECK(pixelOf(c, p, 53.3, -8.0, x, y) && near(p.at(x, y), kLand));    // Ireland
        CHECK(pixelOf(c, p, 48.5, -8.0, x, y) && near(p.at(x, y), kSea));     // the Western Approaches
        CHECK(pixelOf(c, p, 56.5, 3.0, x, y) && near(p.at(x, y), kSea));      // the North Sea
        CHECK(pixelOf(c, p, 47.0, 2.0, x, y) && near(p.at(x, y), kLand));     // France
        CHECK(pixelOf(c, p, 45.5, -5.0, x, y) && near(p.at(x, y), kSea));     // the Bay of Biscay
        CHECK(p.count(kCoast, 0, 0, 512, 384, 6) > 400);                      // the coast is stroked in #434955
        savePng(p, "map-compositor-london.png");
    }
    // ---- ... and sea at 45N 20W: that view's centre (a graticule line would cross it, so the layer is off) ----
    {
        TileCompositor c;
        Pic p(512, 384);
        c.setView(45.0, -20.0, 20.0);
        c.setLayers(true, false);
        CHECK(p.compose(c));
        CHECK(near(p.at(256, 192), kSea));
        CHECK(p.count(kSea, 0, 0, 512, 384) == 512 * 384);                    // the mid-Atlantic at 20 degrees across: no land, no stroke
        CHECK(p.count(kLand, 0, 0, 512, 384, 3) == 0);
    }
    // ---- the world in both projections: every probe is where the geography says ----
    for (int merc = 0; merc < 2; ++merc) {
        TileCompositor c;
        Pic p(1024, merc ? 1024 : 512);
        c.setMercator(merc != 0);
        c.setView(0.0, 0.0, 360.0);
        c.setLayers(true, false);
        CHECK(p.compose(c));
        int wrong = 0;
        for (const Place& pl : kPlaces) {
            int x = 0, y = 0;
            if (!pixelOf(c, p, pl.lat, pl.lon, x, y)) {
                std::printf("  probe %s is off the picture (%s)\n", pl.name, merc ? "Mercator" : "equirectangular");
                ++wrong;
                continue;
            }
            const Rgb got = p.at(x, y);
            const bool ok = pl.land ? near(got, kLand, 3) : near(got, kSea, 3);
            if (!ok) {
                std::printf("  %s (%s): %s expected, got #%02x%02x%02x at (%d,%d)\n", pl.name, merc ? "Mercator" : "equirectangular", pl.land ? "land" : "sea", got.r, got.g, got.b, x, y);
                ++wrong;
            }
        }
        CHECK(wrong == 0);
        savePng(p, merc ? "map-compositor-world-mercator.png" : "map-compositor-world.png");
    }
}

// the Bering Strait: the antimeridian runs through Chukotka, and the land is one piece across it (no seam stroked down it)
void sectionAntimeridian() {
    TileCompositor c;
    Pic p(768, 512);
    c.setView(66.0, 180.0, 40.0);
    c.setLayers(true, false);
    CHECK(p.compose(c));
    int x = 0, y = 0;
    CHECK(pixelOf(c, p, 66.0, 175.0, x, y) && near(p.at(x, y), kLand));       // Chukotka, west of the meridian
    CHECK(pixelOf(c, p, 66.0, 180.0, x, y) && near(p.at(x, y), kLand));       // on it
    CHECK(pixelOf(c, p, 66.0, -175.0, x, y) && near(p.at(x, y), kLand));      // ... and east of it
    CHECK(pixelOf(c, p, 58.0, 180.0, x, y) && near(p.at(x, y), kSea));        // the Bering Sea below
    // The seam is not stroked: in every row where the land is on both sides of the meridian the column it falls in is land, never the
    // coast's colour (the Eurasian ring is cut at +-180 in the data and has to be joined, not closed along the cut).
    int x180 = 0, y180 = 0, interior = 0, seam = 0;
    pixelOf(c, p, 66.0, 180.0, x180, y180);
    for (int yy = 0; yy < 512; ++yy) {
        if (!near(p.at(x180 - 8, yy), kLand) || !near(p.at(x180 + 8, yy), kLand)) { continue; }
        ++interior;
        for (int xx = x180 - 2; xx <= x180 + 2; ++xx) { if (near(p.at(xx, yy), kCoast, 8)) { ++seam; } }
    }
    CHECK(interior > 60);
    CHECK(seam == 0);
    savePng(p, "map-compositor-bering.png");
}

// ----------------------------------------------------------------------------------------------------------------------------
// B. the graticule
// ----------------------------------------------------------------------------------------------------------------------------
std::vector<int> lineColumns(const Pic& p, int y) {
    std::vector<int> cols;
    for (int x = 0; x < p.w; ++x) { if (near(p.at(x, y), kGraticule, 1)) { cols.push_back(x); } }
    return cols;
}

void sectionGraticule() {
    std::printf("B. the graticule\n");
    {
        TileCompositor c;
        Pic p(512, 384);
        c.setView(51.5, -0.1, 20.0);
        c.setLayers(true, true);
        CHECK(p.compose(c));
        int x0 = 0, y0 = 0;
        CHECK(pixelOf(c, p, 55.0, 0.0, x0, y0) && x0 == 258);                // 256 + 0.1 * 25.6 = 258.56: the column the line takes
        // Where nothing but sea lies beside the line (no land, no coast, no parallel) the column is #1e2229 and its neighbours are not.
        // (A line two pixels wide would leave no such row at all.)
        int clean = 0, matching = 0;
        for (int y = 0; y < 384; ++y) {
            bool sea = true;
            for (int x = 250; x <= 266 && sea; ++x) { if (x != 258 && !near(p.at(x, y), kSea, 1)) { sea = false; } }
            if (!sea) { continue; }
            ++clean;
            if (near(p.at(258, y), kGraticule, 1)) { ++matching; }
        }
        CHECK(clean >= 60);
        CHECK(matching == clean);
        // the other meridians: lon -10, -5, 5 (lon 10 is off the right edge); 5 degrees = 128 px apart
        int a = 0, b = 0, d = 0, e = 0, yy = 0;
        pixelOf(c, p, 59.0, -10.0, a, yy);
        pixelOf(c, p, 59.0, -5.0, b, yy);
        pixelOf(c, p, 59.0, 5.0, d, yy);
        pixelOf(c, p, 59.0, 10.0, e, yy);
        CHECK(b - a >= 127 && b - a <= 129);
        CHECK(d - x0 >= 127 && d - x0 <= 129);
        CHECK(e >= 512);
        int seen = 0;
        for (int col : {a, b, d}) {
            int rows = 0;
            for (int y = 0; y < 384; ++y) { if (near(p.at(col, y), kGraticule, 1)) { ++rows; } }
            if (rows > 80) { ++seen; }
        }
        CHECK(seen == 3);
        // a parallel is one row: 50N at y = 192 + 1.5 * 25.6 = 230.4, over the Celtic Sea
        int px = 0, py = 0;
        CHECK(pixelOf(c, p, 50.0, -9.0, px, py) && py == 230);
        CHECK(p.count(kGraticule, 10, 230, 100, 231, 1) == 90);
        CHECK(p.count(kSea, 10, 229, 100, 230, 1) == 90 && p.count(kSea, 10, 231, 100, 232, 1) == 90);
    }
    // the step follows the zoom: lines 60..150 px apart, at multiples of 1/2/5/10/15/30/45 degrees (and below a degree when zoomed in)
    struct Case {
        double span;
        int w, h;
        double step;
    };
    for (const Case& cs : {Case{20.0, 512, 384, 5.0}, Case{2.0, 512, 384, 0.5}, Case{360.0, 1024, 512, 30.0}, Case{0.05, 512, 384, 0.01},
                           Case{90.0, 800, 600, 10.0}, Case{9.0, 600, 400, 1.0}, Case{100.0, 512, 384, 15.0}, Case{300.0, 512, 384, 45.0},
                           Case{0.5, 700, 300, 0.05}}) {
        TileCompositor c;
        Pic p(cs.w, cs.h);
        c.setView(0.0, 0.0, cs.span);
        c.setLayers(false, true);
        CHECK(p.compose(c));
        // a row with no parallel on it, near the middle (a view wider than the world has no graticule past the poles, up and down)
        int row = -1;
        for (int y = cs.h / 2 + 3; y < cs.h && row < 0; ++y) {
            const std::size_t n = lineColumns(p, y).size();
            if (n >= 3 && n < static_cast<std::size_t>(cs.w / 2)) { row = y; }
        }
        CHECK(row >= 0);
        const std::vector<int> cols = row >= 0 ? lineColumns(p, row) : std::vector<int>();
        if (cols.size() < 3) { std::printf("  span %g at %d px: %zu lines on row %d\n", cs.span, cs.w, cols.size(), row); }
        CHECK(cols.size() >= 3);
        bool spacingOk = cols.size() >= 3;
        for (std::size_t i = 1; i < cols.size(); ++i) {
            const int gap = cols[i] - cols[i - 1];
            if (gap < 60 || gap > 150) { spacingOk = false; }
        }
        CHECK(spacingOk);
        // each line is where a multiple of the step falls
        bool onSteps = !cols.empty();
        for (int col : cols) {
            double lat = 0.0, lon = 0.0;
            c.unproject((col + 0.5) / cs.w, 0.5, lat, lon);
            const double k = lon / cs.step;
            if (std::fabs(k - std::round(k)) * cs.step * (cs.w / cs.span) > 1.0) { onSteps = false; }   // within a pixel of a multiple
        }
        CHECK(onSteps);
        if (cs.span == 20.0 || cs.span == 360.0) {
            // a meridian is vertical in a Mercator view as well
            c.setMercator(true);
            CHECK(p.compose(c));
            CHECK(lineColumns(p, cs.h / 2).size() >= 3 || lineColumns(p, cs.h / 2 + 1).size() >= 3);
        }
    }
}

// ----------------------------------------------------------------------------------------------------------------------------
// C. the range rings
// ----------------------------------------------------------------------------------------------------------------------------
double ringCoverage(const Rgb& c) {
    const double v = static_cast<double>(c.r - kSea.r) / (kRingColour.r - kSea.r);
    return std::clamp(v, 0.0, 1.0);
}

void sectionRings() {
    std::printf("C. the range rings\n");
    const double lat0 = 51.5, lon0 = -0.1;
    for (int merc = 0; merc < 2; ++merc) {
        TileCompositor c;
        Pic p(512, 384);
        c.setMercator(merc != 0);
        c.setView(lat0, lon0, 6.0);
        c.setLayers(false, false);
        c.setHome(true, lat0, lon0);
        CHECK(p.compose(c));
        const double mpp = c.metresPerPixel();
        CHECK_NEAR(mpp, 6371000.0 * kPi / 180.0 * std::cos(lat0 * kPi / 180.0) * 6.0 / 512.0, 0.01);
        const double rx = 50000.0 / mpp;                                   // the 50 km ring's radius east and west, in pixels
        // ... and north and south: Mercator is conformal, so the same; equirectangular spends the same pixels on a degree of latitude
        // as on a degree of longitude, and a degree of longitude is only cos(lat) as long on the ground, so the ring is that much flatter
        const double ry = merc ? rx : rx * std::cos(lat0 * kPi / 180.0);
        const double hx = 256.0, hy = 192.0;
        std::map<int, double> sumRho, sumCov;
        int stray = 0, groundBad = 0, strong = 0;
        for (int y = 0; y < 384; ++y) {
            for (int x = 0; x < 512; ++x) {
                const double cov = ringCoverage(p.at(x, y));
                if (cov < 0.05) { continue; }
                const double dx = x + 0.5 - hx, dy = y + 0.5 - hy;
                const double rho = std::hypot(dx / rx, dy / ry);
                const int k = static_cast<int>(std::lround(rho));
                if (k < 1 || std::fabs(rho - k) > 0.25) {
                    ++stray;
                    continue;
                }
                sumRho[k] += cov * rho;
                sumCov[k] += cov;
                if (cov >= 0.5) {
                    // every pixel of ring k is 50 * k km from the home ON THE GROUND (to a pixel and a half of the coarser scale)
                    double lat = 0.0, lon = 0.0;
                    c.unproject((x + 0.5) / 512.0, (y + 0.5) / 384.0, lat, lon);
                    const double km = haversineKm(lat0, lon0, lat, lon);
                    ++strong;
                    if (std::fabs(km - 50.0 * k) > 1.6 * mpp / 1000.0 / (merc ? 1.0 : std::cos(lat0 * kPi / 180.0))) { ++groundBad; }
                }
            }
        }
        CHECK(stray == 0);                                                  // nothing but rings
        CHECK(strong > 400);
        CHECK(groundBad == 0);
        int rings = 0;
        for (const auto& kv : sumCov) {
            if (kv.second < 30.0) { continue; }
            ++rings;
            CHECK_NEAR(sumRho[kv.first] / kv.second, kv.first, 0.05);       // ring k sits at k * 50 km: radius k * rx
        }
        CHECK(rings >= 3 && rings <= 6);
        CHECK(sumCov[1] > 100.0 && sumCov[2] > 100.0 && sumCov[3] > 100.0);
        // the 50 km ring's extremes: east and west are rx from the home, to a pixel
        int right = -1, left = 99999;
        for (int y = 190; y < 194; ++y) {
            for (int x = 0; x < 512; ++x) {
                const double dx = x + 0.5 - hx;
                if (ringCoverage(p.at(x, y)) >= 0.5 && std::fabs(std::fabs(dx) - rx) < 6.0) {
                    right = std::max(right, x);
                    left = std::min(left, x);
                }
            }
        }
        CHECK(right > 0 && std::fabs((right + 0.5 - hx) - rx) <= 1.2);
        CHECK(left < 512 && std::fabs((hx - (left + 0.5)) - rx) <= 1.2);
        // north and south: the same ring is ry from the home
        int top = 99999, bottom = -1;
        for (int x = 254; x < 258; ++x) {
            for (int y = 0; y < 384; ++y) {
                const double dy = y + 0.5 - hy;
                if (ringCoverage(p.at(x, y)) >= 0.5 && std::fabs(std::fabs(dy) - ry) < 6.0) {
                    top = std::min(top, y);
                    bottom = std::max(bottom, y);
                }
            }
        }
        CHECK(top < 384 && std::fabs((hy - (top + 0.5)) - ry) <= 1.2 + (merc ? 0.4 : 0.0));
        CHECK(bottom > 0 && std::fabs((bottom + 0.5 - hy) - ry) <= 1.2 + (merc ? 0.4 : 0.0));
        savePng(p, merc ? "map-compositor-rings-mercator.png" : "map-compositor-rings.png");
    }
    // the step is a nice number and three to six rings show, at every zoom (on the equator the ring is a circle in either projection)
    for (double span : {0.3, 1.5, 6.0, 25.0}) {
        TileCompositor c;
        Pic p(512, 384);
        c.setView(0.0, 0.0, span);
        c.setLayers(false, false);
        c.setHome(true, 0.0, 0.0);
        CHECK(p.compose(c));
        const double mpp = c.metresPerPixel();
        std::vector<double> radii;                                          // the distances of ring pixels, clustered
        {
            std::vector<double> all;
            for (int y = 0; y < 384; ++y) {
                for (int x = 0; x < 512; ++x) { if (ringCoverage(p.at(x, y)) >= 0.5) { all.push_back(std::hypot(x + 0.5 - 256.0, y + 0.5 - 192.0)); } }
            }
            std::sort(all.begin(), all.end());
            double sum = 0.0;
            int n = 0;
            for (std::size_t i = 0; i < all.size(); ++i) {
                sum += all[i];
                ++n;
                if (i + 1 == all.size() || all[i + 1] - all[i] > 3.0) {
                    radii.push_back(sum / n);
                    sum = 0.0;
                    n = 0;
                }
            }
        }
        CHECK(radii.size() >= 3 && radii.size() <= 6);
        if (radii.size() >= 3) {
            const double r1 = radii[0];
            bool equal = true;
            for (std::size_t k = 0; k < radii.size(); ++k) { if (std::fabs(radii[k] - r1 * static_cast<double>(k + 1)) > 2.5) { equal = false; } }
            CHECK(equal);                                                   // k * step
            const double stepKm = r1 * mpp / 1000.0;
            const double decade = std::pow(10.0, std::floor(std::log10(stepKm)));
            const double m = stepKm / decade;                               // 1, 2, 2.5 or 5 (or 10, when the rounding falls the other way)
            CHECK(std::fabs(m - 1.0) < 0.03 || std::fabs(m - 2.0) < 0.06 || std::fabs(m - 2.5) < 0.07 || std::fabs(m - 5.0) < 0.12 || std::fabs(m - 10.0) < 0.25);
        }
    }
    // no home, no rings; a home that is not a place is no home
    {
        TileCompositor c;
        Pic p(256, 192);
        c.setView(0.0, 0.0, 20.0);
        c.setLayers(false, false);
        CHECK(p.compose(c));
        CHECK(p.count(kSea, 0, 0, 256, 192, 0) == 256 * 192);
        c.setHome(true, std::nan(""), 3.0);
        CHECK(!p.compose(c));                                               // nothing changed: it was no home before either
        c.setHome(true, 0.0, 0.0);
        CHECK(p.compose(c));
        CHECK(p.count(kSea, 0, 0, 256, 192, 0) < 256 * 192);
        c.setHome(false, 0.0, 0.0);
        CHECK(p.compose(c));
        CHECK(p.count(kSea, 0, 0, 256, 192, 0) == 256 * 192);
    }
}

// ----------------------------------------------------------------------------------------------------------------------------
// D. the tiles
// ----------------------------------------------------------------------------------------------------------------------------
using Bytes = std::vector<std::uint8_t>;
int missingAnswer(std::uint32_t, std::uint32_t, std::uint32_t, Bytes&) { return -1; }

void sectionTiles() {
    std::printf("D. the tiles\n");
    const Rgb red{255, 0, 0};
    // ---- a solid red 256 px tile paints red where it lands, and is asked once per tile per compose ----
    {
        Source src;
        src.answer = [](std::uint32_t z, std::uint32_t x, std::uint32_t y, Bytes& rgb) {
            if (z == 1 && x == 1 && y == 1) {
                fillTile(rgb, 256, 255, 0, 0);
                return 1;
            }
            return -1;
        };
        TileCompositor c;
        Pic p(512, 384);
        c.setMercator(true);
        c.setView(0.0, 0.0, 360.0);
        c.setLayers(true, false);
        c.setTileSource(src.make(), 256, 0, 5);
        CHECK(c.tileZoom() == -1);                                           // no size yet
        CHECK(p.compose(c));
        CHECK(c.tileZoom() == 1);                                            // z = round(log2(512 / 256))
        CHECK(src.total == 4 && src.askedOf(1, 0, 0) == 1 && src.askedOf(1, 1, 0) == 1 && src.askedOf(1, 0, 1) == 1 && src.askedOf(1, 1, 1) == 1);
        CHECK(src.maxAsks() == 1);
        CHECK(p.count(red, 256, 192, 512, 384, 0) == 256 * 192);             // the bottom right quadrant: exactly the tile
        CHECK(p.count(kSea, 0, 0, 512, 192, 0) == 512 * 192);                // the rest is the sea: the other tiles are MISSING, and ...
        CHECK(p.count(kSea, 0, 192, 256, 384, 0) == 256 * 192);
        CHECK(p.count(kLand, 0, 0, 512, 384, 3) == 0 && p.count(kCoast, 0, 0, 512, 384, 3) == 0);   // ... the coastline is not drawn under tiles
        CHECK(near(p.at(256, 192), red, 0) && near(p.at(255, 192), kSea, 0) && near(p.at(256, 191), kSea, 0));
        CHECK(c.residentTiles() == 1);
        // a second compose: the READY tile is cached, the MISSING ones are believed - nothing is asked again
        c.setLayers(true, true);
        CHECK(p.compose(c));
        CHECK(src.total == 4 && src.maxAsks() == 1 && c.residentTiles() == 1);
        // the graticule is drawn over the tiles (45 degrees apart here, 64 px: the meridian at 0 is column 256)
        CHECK(near(p.at(256, 250), kGraticule, 1) && near(p.at(257, 250), red, 0));
        // clearing the tiles asks again
        c.clearTiles();
        CHECK(c.residentTiles() == 0);
        CHECK(p.compose(c));
        CHECK(src.total == 8 && src.maxAsks() == 2 && c.residentTiles() == 1);
    }
    // ---- PENDING is asked again on the next compose (the compositor stays dirty for it); READY then paints ----
    {
        Source src;
        int pendingAnswer = 0;
        src.answer = [&pendingAnswer](std::uint32_t z, std::uint32_t x, std::uint32_t y, Bytes& rgb) {
            if (z == 1 && x == 0 && y == 0) {
                if (pendingAnswer == 0) { return 0; }
                fillTile(rgb, 256, 0, 0, 255);
                return 1;
            }
            if (z == 1 && x == 1 && y == 0) {
                fillTile(rgb, 256, 0, 255, 0);
                return 1;
            }
            return -1;
        };
        TileCompositor c;
        Pic p(512, 384);
        c.setMercator(true);
        c.setView(0.0, 0.0, 360.0);
        c.setLayers(false, false);
        c.setTileSource(src.make(), 256, 0, 5);
        CHECK(p.compose(c));
        CHECK(src.askedOf(1, 0, 0) == 1 && src.askedOf(1, 1, 0) == 1);
        CHECK(near(p.at(300, 100), {0, 255, 0}, 0) && near(p.at(100, 100), kSea, 0));
        CHECK(p.compose(c));                                                 // no setter in between: still dirty, because a tile is PENDING
        CHECK(src.askedOf(1, 0, 0) == 2);                                    // asked again: that is what makes the plugin fetch it
        CHECK(src.askedOf(1, 1, 0) == 1 && src.askedOf(1, 1, 1) == 1);      // READY is cached, MISSING believed
        CHECK(p.compose(c));
        CHECK(src.askedOf(1, 0, 0) == 3);
        pendingAnswer = 1;                                                   // it arrives
        CHECK(p.compose(c));
        CHECK(src.askedOf(1, 0, 0) == 4);
        CHECK(near(p.at(100, 100), {0, 0, 255}, 0) && near(p.at(300, 100), {0, 255, 0}, 0));
        CHECK(!p.compose(c));                                                // and now nothing is left to ask: clean
        CHECK(src.askedOf(1, 0, 0) == 4);
    }
    // ---- PENDING in two places at once is still asked once per compose (the world's one tile, twice in view) ----
    {
        Source src;
        src.answer = [](std::uint32_t, std::uint32_t, std::uint32_t, Bytes&) { return 0; };
        TileCompositor c;
        Pic p(256, 192);
        c.setMercator(true);
        c.setView(0.0, 180.0, 360.0);                                        // z = 0: one tile, and the view shows it either side of the meridian
        c.setLayers(false, false);
        c.setTileSource(src.make(), 256, 0, 5);
        CHECK(p.compose(c));
        CHECK(c.tileZoom() == 0);
        CHECK(src.total == 1 && src.askedOf(0, 0, 0) == 1);
    }
    // ---- MISSING is not asked again for 600 composes ----
    {
        Source src;
        src.answer = missingAnswer;
        TileCompositor c;
        Pic p(512, 384);
        c.setMercator(true);
        c.setView(0.0, 0.0, 360.0);
        c.setLayers(true, false);
        c.setTileSource(src.make(), 256, 0, 5);
        CHECK(p.compose(c));
        CHECK(src.total == 4);
        CHECK(p.count(kLand, 0, 0, 512, 384, 3) > 1000);                     // no tile drawn: the coastline is the picture, as in the ImGui view
        bool quiet = true;
        for (int i = 2; i <= 600; ++i) {                                     // the 599 composes after the one that was told MISSING ...
            c.setLayers(true, (i % 2) == 0);
            if (!p.compose(c)) { quiet = false; }
            if (src.total != 4) { quiet = false; }
        }
        CHECK(quiet);
        c.setLayers(true, false);
        CHECK(p.compose(c));                                                 // ... and the 600th after it asks again
        CHECK(src.total == 8 && src.maxAsks() == 2);
        // it asks again when nothing else moved, too: 599 clean calls say "nothing to do", the 600th is a compose that asks
        bool allClean = true;
        for (int i = 0; i < 599; ++i) { if (p.compose(c)) { allClean = false; } }
        CHECK(allClean && src.total == 8);
        CHECK(p.compose(c));
        CHECK(src.total == 12 && src.maxAsks() == 3);
        // the retry clock is the tile path's: once the view no longer asks for tiles, clean calls stay clean however long they go on
        c.setMercator(false);
        CHECK(p.compose(c));
        bool stillClean = true;
        for (int i = 0; i < 700; ++i) { if (p.compose(c)) { stillClean = false; } }
        CHECK(stillClean && src.total == 12);
    }
    // ---- the zoom: z = round(log2(worldPx / tileSize)) clamped to the source's range ----
    {
        struct Case {
            int w;
            double span;
            std::uint32_t tileSize, minZoom, maxZoom;
            int want;
        };
        for (const Case& cs : {Case{1024, 360.0, 256, 0, 19, 2}, Case{1024, 90.0, 256, 0, 19, 4}, Case{1024, 100.0, 256, 0, 19, 4},
                               Case{1024, 130.0, 256, 0, 19, 3}, Case{1024, 0.02, 256, 0, 19, 16}, Case{1024, 90.0, 256, 0, 3, 3},
                               Case{1024, 360.0, 256, 6, 19, 6}, Case{1024, 360.0, 512, 0, 19, 1}, Case{1024, 360.0, 64, 0, 19, 4}}) {
            Source src;
            src.answer = missingAnswer;
            TileCompositor c;
            c.setSize(cs.w, 768);
            c.setMercator(true);
            c.setView(0.0, 0.0, cs.span);
            c.setTileSource(src.make(), cs.tileSize, cs.minZoom, cs.maxZoom);
            CHECK(c.tileZoom() == cs.want);
        }
        TileCompositor c;
        c.setSize(1024, 768);
        c.setView(0.0, 0.0, 90.0);
        CHECK(c.tileZoom() == -1);                                           // no source
        Source src;
        src.answer = missingAnswer;
        c.setTileSource(src.make(), 256, 0, 19);
        CHECK(c.tileZoom() == -1);                                           // a source but equirectangular: tiles are Mercator
        c.setMercator(true);
        CHECK(c.tileZoom() == 4);
        c.setTileSource(TileSource(), 256, 0, 19);
        CHECK(c.tileZoom() == -1 && c.residentTiles() == 0);                 // detached
    }
    // ---- bilinear when the scale is not 1: a texel pattern drawn four times larger is smooth, not blocky ----
    {
        Source src;
        src.answer = [](std::uint32_t z, std::uint32_t x, std::uint32_t y, Bytes& rgb) {
            if (z != 1 || x != 0 || y != 1) { return -1; }
            rgb.resize(256u * 256u * 3u);
            for (std::size_t j = 0; j < 256; ++j) {
                for (std::size_t i = 0; i < 256; ++i) {
                    const std::uint8_t v = (i % 2 == 0) ? 0 : 255;           // alternate texels: the finest pattern there is
                    rgb[(j * 256 + i) * 3 + 0] = v;
                    rgb[(j * 256 + i) * 3 + 1] = static_cast<std::uint8_t>(j);
                    rgb[(j * 256 + i) * 3 + 2] = 7;
                }
            }
            return 1;
        };
        TileCompositor c;
        Pic p(512, 384);
        c.setMercator(true);
        c.setView(-45.0, -90.0, 90.0);                                       // 512 px for 90 degrees: the world is 2048 px, a z = 1 tile 1024 px: scale 4
        c.setLayers(false, false);
        c.setTileSource(src.make(), 256, 0, 1);                              // (zoom held at 1 by maxZoom)
        CHECK(p.compose(c));
        CHECK(c.tileZoom() == 1);
        CHECK(src.total == 1 && src.askedOf(1, 0, 1) == 1);
        // the tile's west edge is lon -180, x = -256: dest column px takes texel u = (px + 0.5 + 256) / 4 - 0.5
        int exact = 0, intermediate = 0;
        for (int px = 0; px < 64; ++px) {
            const double u = (px + 0.5 + 256.0) / 4.0 - 0.5;
            const int i0 = static_cast<int>(std::floor(u));
            const double f = u - i0;
            const double a = (i0 % 2 == 0) ? 0.0 : 255.0, b = ((i0 + 1) % 2 == 0) ? 0.0 : 255.0;
            const double want = a * (1.0 - f) + b * f;
            if (std::fabs(p.at(px, 100).r - want) <= 2.5) { ++exact; }
            if (p.at(px, 100).r > 20 && p.at(px, 100).r < 235) { ++intermediate; }
        }
        CHECK(exact == 64);
        CHECK(intermediate >= 40);                                           // nearest-texel would have only 0 and 255
        CHECK(p.at(10, 100).b == 7);
    }
    // ---- a tile is placed by its own edges, and x wraps across the antimeridian ----
    {
        Source src;
        src.answer = [](std::uint32_t z, std::uint32_t x, std::uint32_t y, Bytes& rgb) {
            fillTile(rgb, 256, 40 + static_cast<int>(x) * 50, 40 + static_cast<int>(y) * 50, 200 + static_cast<int>(z));
            return 1;
        };
        TileCompositor c;
        Pic p(512, 384);
        c.setMercator(true);
        c.setView(0.0, 180.0, 180.0);                                        // lon 90..270: tile 3 and (wrapped) tile 0 at z = 2
        c.setLayers(false, false);
        c.setTileSource(src.make(), 256, 0, 5);
        CHECK(p.compose(c));
        CHECK(c.tileZoom() == 2);
        CHECK(src.total == 4 && src.maxAsks() == 1);
        CHECK(src.askedOf(2, 3, 1) == 1 && src.askedOf(2, 3, 2) == 1 && src.askedOf(2, 0, 1) == 1 && src.askedOf(2, 0, 2) == 1);
        CHECK(near(p.at(100, 100), {190, 90, 202}, 0) && near(p.at(400, 100), {40, 90, 202}, 0));
        CHECK(near(p.at(100, 300), {190, 140, 202}, 0) && near(p.at(400, 300), {40, 140, 202}, 0));
        CHECK(near(p.at(255, 100), {190, 90, 202}, 0) && near(p.at(256, 100), {40, 90, 202}, 0));   // the seam is at the centre, without a gap
        CHECK(p.count(kSea, 0, 0, 512, 384, 0) == 0);                        // every pixel is a tile's
        savePng(p, "map-compositor-tiles.png");
    }
    // ---- a buffer shorter than tileSize^2 * 3 is MISSING; a source that throws is MISSING; neither stops the compose ----
    for (int mode = 0; mode < 2; ++mode) {
        Source src;
        src.answer = [mode](std::uint32_t, std::uint32_t, std::uint32_t, Bytes& rgb) -> int {
            if (mode == 1) { throw std::runtime_error("the plugin fell over"); }
            fillTile(rgb, 256, 255, 0, 0);
            rgb.pop_back();
            return 1;
        };
        TileCompositor c;
        Pic p(512, 384);
        c.setMercator(true);
        c.setView(51.5, -0.1, 20.0);
        c.setLayers(true, false);
        c.setTileSource(src.make(), 256, 0, 12);
        CHECK(p.compose(c));
        const int first = src.total;
        CHECK(first >= 1 && src.maxAsks() == 1);
        CHECK(p.count({255, 0, 0}, 0, 0, 512, 384, 0) == 0);                 // nothing red: no tile was drawn
        CHECK(near(p.at(256, 192), kLand));                                  // the coastline stands in (Mercator)
        CHECK(c.residentTiles() == 0);
        c.setLayers(true, true);
        CHECK(p.compose(c));
        CHECK(src.total == first);                                           // believed missing
    }
    // ---- an equirectangular view with a source never asks; Mercator without one draws the coastline in Mercator ----
    {
        Source src;
        src.answer = [](std::uint32_t, std::uint32_t, std::uint32_t, Bytes& rgb) {
            fillTile(rgb, 256, 255, 0, 0);
            return 1;
        };
        TileCompositor c;
        Pic p(512, 384);
        c.setView(51.5, -0.1, 20.0);
        c.setLayers(true, false);
        c.setTileSource(src.make(), 256, 0, 12);
        CHECK(p.compose(c));
        CHECK(src.total == 0 && near(p.at(256, 192), kLand) && p.count({255, 0, 0}, 0, 0, 512, 384, 0) == 0);
        c.setTileSource(TileSource(), 256, 0, 12);
        c.setMercator(true);
        CHECK(p.compose(c));
        CHECK(near(p.at(256, 192), kLand) && c.tileZoom() == -1);
    }
    // ---- at most 64 tiles an axis are asked for however wide the view (91 columns would fit here) ----
    {
        Source src;
        src.answer = [](std::uint32_t, std::uint32_t, std::uint32_t, Bytes& rgb) {
            fillTile(rgb, 64, 90, 120, 150);
            return 1;
        };
        TileCompositor c;
        Pic p(4096, 64);
        c.setMercator(true);
        c.setView(0.0, 0.0, 4096.0 * 360.0 / 5800.0);                        // 5800 px round the world: 90.6 of a 64 px tile at zoom 7 (scale 0.71)
        c.setLayers(false, false);
        c.setTileSource(src.make(), 64, 0, 12);
        CHECK(p.compose(c));
        CHECK(c.tileZoom() == 7);
        CHECK(src.total == 64 * 2 && src.maxAsks() == 1);                    // 64 columns, 2 rows: the cap, not the 91 the view would take
        CHECK(near(p.at(100, 30), {90, 120, 150}, 0) && near(p.at(2800, 30), {90, 120, 150}, 0));
        CHECK(p.count(kSea, 3000, 0, 4096, 64, 0) == (4096 - 3000) * 64);   // the rest of the picture is bare (64 columns end at x = 64 * 45.3 = 2900)
    }
    // ---- the cache holds at most kMaxTiles, least recently used out, and the tiles of the picture on screen are in it ----
    {
        Source src;
        src.answer = [](std::uint32_t, std::uint32_t, std::uint32_t, Bytes& rgb) {
            fillTile(rgb, 256, 90, 90, 90);
            return 1;
        };
        TileCompositor c;
        Pic p(512, 384);
        c.setMercator(true);
        c.setLayers(false, false);
        c.setTileSource(src.make(), 256, 0, 12);
        std::size_t most = 0;
        for (double lat : {0.0, 30.0, -30.0, 55.0, -55.0, 15.0, -15.0, 70.0, -70.0}) {   // nine bands of two rows of 32: 576 tiles
            for (int i = 0; i < 16; ++i) {
                c.setView(lat, -180.0 + 22.5 * i, 22.5);                     // z = 5 at 512 px: 1024 tiles round the world
                CHECK(p.compose(c));
                most = std::max(most, c.residentTiles());
            }
        }
        CHECK(src.total > 256);                                              // far more than 256 distinct tiles were asked for
        CHECK(most <= TileCompositor::kMaxTiles && c.residentTiles() == TileCompositor::kMaxTiles);
        const int before = src.total;
        c.setLayers(false, true);
        CHECK(p.compose(c));
        CHECK(src.total == before);                                          // the tiles of the last view stayed
        c.setView(0.0, -180.0, 22.5);                                        // the very first view: long since evicted
        CHECK(p.compose(c));
        CHECK(src.total > before);
    }
    // ---- ... and fewer when a tile is big: 96 MiB of RGB, so 32 of 1024 px (3 MiB each), however many the views touched ----
    {
        Source src;
        src.answer = [](std::uint32_t, std::uint32_t, std::uint32_t, Bytes& rgb) {
            fillTile(rgb, 1024, 60, 70, 80);
            return 1;
        };
        TileCompositor c;
        Pic p(1024, 768);
        c.setMercator(true);
        c.setLayers(false, false);
        c.setTileSource(src.make(), 1024, 0, 12);
        std::size_t most = 0;
        for (int row = 0; row < 8; ++row) {
            const double y = (row + 0.5) / 8.0;                              // the middle of a row of the zoom 3 world
            const double lat = std::atan(std::sinh(kPi * (1.0 - 2.0 * y))) * 180.0 / kPi;
            for (int col = 0; col < 8; ++col) {
                c.setView(lat, -157.5 + 45.0 * col, 45.0);                   // 1024 px for 45 degrees: scale 1 on the 1024 px tiles of zoom 3
                CHECK(p.compose(c));
                most = std::max(most, c.residentTiles());
            }
        }
        CHECK(c.tileZoom() == 3);
        CHECK(src.total > 32 && most == 32 && c.residentTiles() == 32);
    }
}

// ----------------------------------------------------------------------------------------------------------------------------
// E. project / unproject
// ----------------------------------------------------------------------------------------------------------------------------
void sectionProjection() {
    std::printf("E. project / unproject\n");
    struct V {
        double lat, lon, span;
        int w, h;
    };
    const V views[] = {{51.5, -0.1, 20.0, 512, 384}, {-33.9, 151.2, 5.0, 800, 600}, {0.0, 179.9, 10.0, 1024, 300}, {10.0, -179.5, 30.0, 512, 384},
                       {60.0, 30.0, 90.0, 640, 480},  {0.0, 0.0, 360.0, 1024, 512},   {-45.0, 100.0, 0.5, 300, 300}, {20.0, 180.0, 120.0, 900, 500}};
    const double lats[] = {-60, -30, 0, 15, 45, 60, 80};
    const double lons[] = {-179.9, -170, -90, 0.1, 90, 170, 179.9, 180.0};
    for (int merc = 0; merc < 2; ++merc) {
        for (const V& v : views) {
            TileCompositor c;
            c.setSize(v.w, v.h);
            c.setMercator(merc != 0);
            c.setView(v.lat, v.lon, v.span);
            int bad = 0, badFlag = 0, points = 0;
            for (double lat : lats) {
                for (double lon : lons) {
                    double fx = 0, fy = 0, lat2 = 0, lon2 = 0;
                    const bool on = c.project(lat, lon, fx, fy);
                    c.unproject(fx, fy, lat2, lon2);
                    ++points;
                    if (std::fabs(lat2 - lat) > 1e-6 || std::fabs(lonDelta(lon, lon2)) > 1e-6) { ++bad; }
                    if (on != (fx >= 0.0 && fx <= 1.0 && fy >= 0.0 && fy <= 1.0)) { ++badFlag; }
                    if (!std::isfinite(fx) || !std::isfinite(fy)) { ++bad; }
                }
            }
            CHECK(points == 56 && bad == 0 && badFlag == 0);
            // and the other way: a fraction of the picture to a place and back
            int back = 0;
            for (int i = 0; i <= 10; ++i) {
                for (int j = 0; j <= 10; ++j) {
                    double lat = 0, lon = 0, fx = 0, fy = 0;
                    c.unproject(i / 10.0, j / 10.0, lat, lon);
                    c.project(lat, lon, fx, fy);
                    double dfx = std::fabs(fx - i / 10.0);
                    if (v.span == 360.0) { dfx = std::min(dfx, std::fabs(dfx - 1.0)); }   // the two edges of a whole turn are one meridian
                    if (dfx > 1e-9 || std::fabs(fy - j / 10.0) > 1e-9) { ++back; }
                }
            }
            CHECK(back == 0);
            double fx = 0, fy = 0;
            CHECK(c.project(v.lat, v.lon, fx, fy) && std::fabs(fx - 0.5) < 1e-12 && std::fabs(fy - 0.5) < 1e-12);   // the centre is the middle
        }
    }
    // the short way round the antimeridian: +179.9 seen from -179.9 is 0.2 degrees east of it, not 359.8 west
    {
        TileCompositor c;
        c.setSize(512, 384);
        c.setView(0.0, 179.9, 10.0);
        double fx = 0, fy = 0;
        CHECK(c.project(0.0, -179.9, fx, fy) && std::fabs(fx - (0.5 + 0.2 / 10.0)) < 1e-9);
        c.setView(0.0, -179.9, 10.0);
        CHECK(c.project(0.0, 179.9, fx, fy) && std::fabs(fx - (0.5 - 0.2 / 10.0)) < 1e-9);
        // off the picture: false, with the fractions still filled
        c.setView(0.0, 0.0, 10.0);
        CHECK(!c.project(0.0, 8.0, fx, fy) && std::fabs(fx - 1.3) < 1e-9 && std::fabs(fy - 0.5) < 1e-9);
        CHECK(!c.project(30.0, 0.0, fx, fy) && fy < 0.0 && std::isfinite(fy));
        CHECK(c.project(0.0, 5.0, fx, fy) && std::fabs(fx - 1.0) < 1e-12);     // exactly on the edge counts as on
        CHECK(!c.project(std::nan(""), 0.0, fx, fy) && std::isnan(fx) && std::isnan(fy));
        CHECK(!c.project(0.0, std::numeric_limits<double>::infinity(), fx, fy));
        // equirectangular: a degree of latitude is as many pixels as a degree of longitude (the latitude span follows the aspect):
        // 3 degrees east and 2 north of the centre of a 512 x 256 picture 10 degrees across are 153.6 px right and 102.4 px up
        c.setSize(512, 256);
        CHECK(c.project(2.0, 3.0, fx, fy) && std::fabs(fx - 0.8) < 1e-12 && std::fabs(fy - 0.1) < 1e-12);
        // Web Mercator: the same pixels per degree at the equator, and a parallel is further from it than its latitude says
        c.setMercator(true);
        c.setView(0.0, 0.0, 10.0);
        double fy60 = 0, fy30 = 0, fy1 = 0;
        c.project(1.0, 0.0, fx, fy1);
        c.project(30.0, 0.0, fx, fy30);
        c.project(60.0, 0.0, fx, fy60);
        CHECK(std::fabs((0.5 - fy1) * 256 - 51.2) < 0.01);                   // a degree is 51.2 px near the equator
        CHECK((0.5 - fy30) > 30.0 * (0.5 - fy1) * 1.04 && (0.5 - fy60) > 60.0 * (0.5 - fy1) * 1.2);
    }
}

// ----------------------------------------------------------------------------------------------------------------------------
// F. compose() and the dirty flag
// ----------------------------------------------------------------------------------------------------------------------------
void sectionDirty() {
    std::printf("F. compose and the dirty flag\n");
    TileCompositor c;
    Pic p(256, 192);
    CHECK(!c.compose(p.data(), 256, 192));                                   // no size yet
    CHECK(!c.compose(nullptr, 256, 192));
    c.setSize(256, 192);
    CHECK(!c.compose(p.data(), 255, 192) && !c.compose(p.data(), 256, 191));   // a buffer of another size is refused
    CHECK(c.compose(p.data(), 256, 192));
    std::fill(p.px.begin(), p.px.end(), static_cast<std::uint8_t>(0x5a));
    CHECK(!c.compose(p.data(), 256, 192));
    CHECK(std::all_of(p.px.begin(), p.px.end(), [](std::uint8_t b) { return b == 0x5a; }));   // returned false and touched nothing
    // every setter, with a new value: true; the same value again: false
    const auto once = [&](const char* what, const std::function<void()>& set) {
        set();
        const bool first = c.compose(p.data(), 256, 192);
        const bool second = c.compose(p.data(), 256, 192);
        if (!first || second) { std::printf("  %s: first %d, then %d\n", what, first ? 1 : 0, second ? 1 : 0); }
        CHECK(first);
        CHECK(!second);
    };
    once("setView", [&] { c.setView(10.0, 20.0, 30.0); });
    once("setMercator", [&] { c.setMercator(true); });
    once("setLayers", [&] { c.setLayers(false, true); });
    once("setHome", [&] { c.setHome(true, 10.0, 20.0); });
    once("setCoverage", [&] { c.setCoverage({100.0, 200.0, 300.0}); });
    Trail t;
    t.points = {{10.0, 20.0}, {11.0, 22.0}};
    t.rgba = 0xff0000ffu;
    once("setTrails", [&] { c.setTrails({t}); });
    Source src;
    src.answer = missingAnswer;
    once("setTileSource", [&] { c.setTileSource(src.make(), 256, 0, 5); });
    once("clearTiles", [&] { c.clearTiles(); });
    // the same values again change nothing
    c.setView(10.0, 20.0, 30.0);
    c.setMercator(true);
    c.setLayers(false, true);
    c.setHome(true, 10.0, 20.0);
    c.setCoverage({100.0, 200.0, 300.0});
    c.setTrails({t});
    CHECK(!c.compose(p.data(), 256, 192));
    // a trail that moved, a coverage that grew
    t.points.push_back({12.0, 23.0});
    once("setTrails (a point more)", [&] { c.setTrails({t}); });
    once("setCoverage (a bucket more)", [&] { c.setCoverage({100.0, 200.0, 300.0, 50.0}); });
    once("setHome (moved)", [&] { c.setHome(true, 10.5, 20.0); });
    once("setHome (gone)", [&] { c.setHome(false, 0.0, 0.0); });
    // a picture of another size: setSize raises it, and the new buffer is the one that is painted
    Pic q(300, 200);
    c.setSize(300, 200);
    CHECK(!c.compose(p.data(), 256, 192));                                   // the old size is refused now
    CHECK(c.compose(q.data(), 300, 200) && !c.compose(q.data(), 300, 200));
    // the same size in ANOTHER buffer (a host that alternates two, as the spectrum's does): nothing changed, so it is told false and the buffer is
    // left alone - the picture the host is showing is still the right one - and after a change the compose paints whichever buffer it is
    // asked into, whole
    Pic r(300, 200);
    std::fill(r.px.begin(), r.px.end(), static_cast<std::uint8_t>(0x77));
    CHECK(!c.compose(r.data(), 300, 200));
    CHECK(std::all_of(r.px.begin(), r.px.end(), [](std::uint8_t b) { return b == 0x77; }));
    c.setView(11.0, 21.0, 31.0);
    CHECK(c.compose(r.data(), 300, 200));
    CHECK(!c.compose(q.data(), 300, 200) && !c.compose(r.data(), 300, 200));
    c.setView(10.0, 20.0, 30.0);                                             // and back to the view q was painted at
    CHECK(c.compose(r.data(), 300, 200));
    Pic again(300, 200);
    c.setView(12.0, 22.0, 32.0);
    CHECK(c.compose(again.data(), 300, 200));
    c.setView(10.0, 20.0, 30.0);
    CHECK(c.compose(again.data(), 300, 200));
    CHECK(again.px == q.px && r.px == q.px);                                 // the same view painted afresh, into any buffer, is the same picture bit for bit
    // a clamp that changes nothing is not a change
    c.setView(10.0, 20.0, 1000.0);
    CHECK(c.compose(r.data(), 300, 200));
    c.setView(10.0, 20.0, 360.0);
    CHECK(!c.compose(r.data(), 300, 200));
    // sizes: negative and absurd are refused, not crashed on
    c.setSize(-5, 100);
    CHECK(!c.compose(r.data(), -5, 100) && !c.compose(r.data(), 0, 100));
    c.setSize(20000, 20000);
    CHECK(!c.compose(r.data(), 16384, 16384));
    c.setSize(300, 200);
    CHECK(c.compose(r.data(), 300, 200));
}

// ----------------------------------------------------------------------------------------------------------------------------
// G. the coverage wedges and the trails
// ----------------------------------------------------------------------------------------------------------------------------
struct Spot {
    double lat, lon;
};
Spot destinationOf(double lat, double lon, double bearingDeg, double km) {
    const double r = kPi / 180.0, d = km / 6371.0, b = bearingDeg * r, p1 = lat * r;
    const double p2 = std::asin(std::sin(p1) * std::cos(d) + std::cos(p1) * std::sin(d) * std::cos(b));
    const double l2 = lon * r + std::atan2(std::sin(b) * std::sin(d) * std::cos(p1), std::cos(d) - std::sin(p1) * std::sin(p2));
    return {p2 / r, l2 / r};
}

// A wedge picture and the same picture without the wedges (the rings are drawn either way): a pixel the wedges changed is a wedge pixel, and
// there it must be exactly 25 % of #2e86c1 over what was there.
struct WedgePair {
    TileCompositor c;
    Pic without{512, 384}, with{512, 384};
    bool tintedAt(int x, int y) const { return !near(with.at(x, y), without.at(x, y), 0); }
    bool tintIsOnePass(int x, int y) const { return near(with.at(x, y), over(kWedgeBlue, 64.0 / 255.0, without.at(x, y)), 2); }
    void build(const std::vector<double>& km) {
        c.setView(0.0, 0.0, 51.2);                                           // 10 px per degree, the home (on the equator) at the centre
        c.setLayers(false, false);
        c.setHome(true, 0.0, 0.0);
        c.setCoverage({});
        without.compose(c);
        c.setCoverage(km);
        if (!with.compose(c)) { with.px = without.px; }                      // nothing changed (km is no wedge at all): the picture on show is the one it has
    }
    // the pixel of a place `dist` km from the home on `bearing`
    void pixel(double bearing, double dist, int& x, int& y) {
        const Spot s = destinationOf(0.0, 0.0, bearing, dist);
        pixelOf(c, with, s.lat, s.lon, x, y);
    }
    bool tintedOn(double bearing, double dist) {
        int x = 0, y = 0;
        pixel(bearing, dist, x, y);
        return tintedAt(x, y) && tintIsOnePass(x, y);
    }
    bool cleanOn(double bearing, double dist) {
        int x = 0, y = 0;
        pixel(bearing, dist, x, y);
        return !tintedAt(x, y);
    }
};

void sectionWedges() {
    std::printf("G. the coverage wedges\n");
    {
        WedgePair w;
        std::vector<double> km(72, 0.0);
        km[18] = 2000.0;                                                     // bearings 90..95: east, a little south; 2000 km is 180 px here
        w.build(km);
        CHECK(w.tintedOn(92.5, 800.0));                                      // inside: #2e86c1 at 25 % over what was there
        CHECK(w.tintedOn(91.0, 1700.0) && w.tintedOn(94.0, 400.0));
        CHECK(w.cleanOn(80.0, 800.0) && w.cleanOn(97.0, 800.0));             // the neighbouring buckets are empty
        CHECK(w.cleanOn(92.5, 2300.0));                                      // past its km
        CHECK(w.cleanOn(272.5, 800.0));                                      // the other side of the home
        // ... and the tint is a one-pass 25 % wherever it is, and covers the area of a 5 degree wedge of 180 px: 1414 px
        int tinted = 0, wrong = 0;
        for (int y = 0; y < 384; ++y) {
            for (int x = 0; x < 512; ++x) {
                if (!w.tintedAt(x, y)) { continue; }
                ++tinted;
                if (!w.tintIsOnePass(x, y)) { ++wrong; }
            }
        }
        CHECK(tinted > 1300 && tinted < 1550 && wrong == 0);
    }
    {   // a full ring of buckets is one disc: no seam, no doubled colour
        WedgePair w;
        w.build(std::vector<double>(72, 2000.0));
        const double rPx = 2000.0 / 111.19493 * 10.0;                        // 17.99 degrees at 10 px a degree
        int inside = 0, off = 0, doubled = 0;
        for (int y = 0; y < 384; ++y) {
            for (int x = 0; x < 512; ++x) {
                const double d = std::hypot(x + 0.5 - 256.0, y + 0.5 - 192.0);
                if (d < rPx - 3.0) {
                    ++inside;
                    if (!w.tintedAt(x, y)) { ++off; }
                } else if (d > rPx + 3.0 && w.tintedAt(x, y)) {
                    ++off;
                }
                if (w.tintedAt(x, y) && !w.tintIsOnePass(x, y)) { ++doubled; }
            }
        }
        CHECK(inside > 90000 && off == 0 && doubled == 0);
        // wedges either side of an empty stretch meet at the home, and are not joined across it
        std::vector<double> v(72, 0.0);
        v[3] = 2000.0;
        v[40] = 2000.0;
        v[41] = 1500.0;
        w.build(v);
        CHECK(w.tintedOn(17.0, 800.0));                                      // bucket 3
        CHECK(w.cleanOn(100.0, 800.0) && w.cleanOn(300.0, 800.0));           // the empty buckets between
        CHECK(w.tintedOn(207.0, 1400.0) && w.cleanOn(207.0, 1700.0));        // the step from 2000 km down to 1500 between buckets 40 and 41
        CHECK(w.tintedOn(202.0, 1700.0));
    }
    {   // the hostile cases: more than 72 buckets, a bucket that is negative or not a number, an empty vector, no home
        WedgePair w;
        std::vector<double> v(100, std::nan(""));
        v[0] = -5.0;
        v[1] = std::numeric_limits<double>::infinity();
        v[80] = 400.0;                                                       // beyond bucket 71: ignored
        w.build(v);
        int tinted = 0;
        for (int y = 0; y < 384; ++y) { for (int x = 0; x < 512; ++x) { if (w.tintedAt(x, y)) { ++tinted; } } }
        CHECK(tinted == 0);
        CHECK(!w.with.compose(w.c));                                         // nothing in any wedge is the same as no coverage
        w.c.setCoverage({});
        CHECK(!w.with.compose(w.c));                                         // ... and so is an empty list
        w.c.setCoverage(std::vector<double>(72, 1000.0));
        CHECK(w.with.compose(w.c));
        tinted = 0;
        for (int y = 0; y < 384; ++y) { for (int x = 0; x < 512; ++x) { if (w.tintedAt(x, y)) { ++tinted; } } }
        CHECK(tinted > 20000);
        w.c.setHome(false, 0.0, 0.0);
        CHECK(w.with.compose(w.c));
        CHECK(w.with.count(kSea, 0, 0, 512, 384, 0) == 512 * 384);          // no home: no wedges (and no rings)
    }
}

void sectionTrails() {
    std::printf("G. the trails\n");
    // 10 px per degree, equirectangular: x = 256 + 10 lon, y = 192 - 10 lat
    const auto lonAt = [](double x) { return (x - 256.0) / 10.0; };
    const auto latAt = [](double y) { return (192.0 - y) / 10.0; };
    TileCompositor c;
    Pic p(512, 384);
    c.setView(0.0, 0.0, 51.2);
    c.setLayers(false, false);
    Trail line;
    line.points = {{latAt(100.0), lonAt(50.0)}, {latAt(100.0), lonAt(150.0)}};
    line.rgba = 0xf5a524ffu;
    c.setTrails({line});
    CHECK(p.compose(c));
    // 2 px wide: the rows either side of y = 100.0 are the line, the next ones are not
    CHECK(near(p.at(100, 99), kOrange, 2) && near(p.at(100, 100), kOrange, 2));
    CHECK(near(p.at(100, 98), kSea, 0) && near(p.at(100, 101), kSea, 0));
    CHECK(p.count(kOrange, 53, 99, 147, 101, 2) == 94 * 2);
    CHECK(near(p.at(30, 100), kSea, 0) && near(p.at(170, 100), kSea, 0));
    // the ends are round caps one pixel long
    CHECK(p.at(49, 99).r > kSea.r + 40 && p.at(48, 99).r == kSea.r && p.at(150, 100).r > kSea.r + 40 && p.at(152, 100).r == kSea.r);
    // dashed: 6 px on, 4 px off along the path, from the first vertex
    line.dashed = true;
    c.setTrails({line});
    CHECK(p.compose(c));
    bool dashesOk = true;
    for (int k = 0; k < 9; ++k) {
        for (int x = 51 + 10 * k; x <= 54 + 10 * k; ++x) { if (!near(p.at(x, 99), kOrange, 3) || !near(p.at(x, 100), kOrange, 3)) { dashesOk = false; } }
        for (int x = 57 + 10 * k; x <= 58 + 10 * k; ++x) { if (!near(p.at(x, 99), kSea, 0) || !near(p.at(x, 100), kSea, 0)) { dashesOk = false; } }
    }
    CHECK(dashesOk);
    int covered = 0;                                                         // pixels more than half orange along the path
    for (int x = 50; x < 150; ++x) { if (p.at(x, 100).r > (kSea.r + kOrange.r) / 2) { ++covered; } }
    CHECK(covered >= 56 && covered <= 64);                                   // ten dashes of six
    // translucent: alpha 0x80 is half the colour, and a joint is not darker than the rest (it is composited once)
    Trail vee;
    vee.points = {{latAt(300.0), lonAt(50.0)}, {latAt(250.0), lonAt(80.0)}, {latAt(300.0), lonAt(110.0)}};
    vee.rgba = 0xf5a52480u;
    c.setTrails({vee});
    CHECK(p.compose(c));
    const Rgb half = over(kOrange, 128.0 / 255.0, kSea);
    int brightest = 0, onTrail = 0;
    for (int y = 240; y < 310; ++y) {
        for (int x = 40; x < 120; ++x) {
            const Rgb g = p.at(x, y);
            if (g.r > kSea.r) {
                ++onTrail;
                brightest = std::max(brightest, g.r);
            }
        }
    }
    CHECK(onTrail > 100);
    CHECK(brightest <= half.r + 2 && brightest >= half.r - 2);               // one layer of half the colour, at the apex and the ends as well
    // closed: the last vertex joins the first
    Trail box;
    box.points = {{latAt(60.0), lonAt(300.0)}, {latAt(60.0), lonAt(380.0)}, {latAt(120.0), lonAt(380.0)}, {latAt(120.0), lonAt(300.0)}};
    box.rgba = 0x7fe6adffu;
    const Rgb green{0x7f, 0xe6, 0xad};
    c.setTrails({box});
    CHECK(p.compose(c));
    CHECK(near(p.at(340, 59), green, 3) && near(p.at(380, 90), green, 3) && near(p.at(340, 119), green, 3));
    CHECK(near(p.at(300, 90), kSea, 0) && near(p.at(299, 90), kSea, 0));     // open: no side from the last vertex back to the first
    box.closed = true;
    c.setTrails({box});
    CHECK(p.compose(c));
    CHECK(near(p.at(300, 89), green, 3) && near(p.at(299, 89), green, 3));
    // a vertex that is not a place is not drawn, a trail of one point draws nothing, a transparent one draws nothing
    Trail junk;
    junk.points = {{std::nan(""), 0.0}, {latAt(200.0), lonAt(200.0)}};
    junk.rgba = 0xff0000ffu;
    Trail transparent = line;
    transparent.rgba = 0xff000000u;
    c.setTrails({junk, transparent});
    CHECK(p.compose(c));
    CHECK(p.count(kSea, 0, 0, 512, 384, 0) == 512 * 384);
    // across the antimeridian: +179 to -179 is a two-degree line through the centre of a view on 180, not a streak across the world
    c.setView(0.0, 180.0, 51.2);
    Trail cross;
    cross.points = {{0.0, 179.0}, {0.0, -179.0}};
    cross.rgba = 0xffffffffu;
    c.setTrails({cross});
    CHECK(p.compose(c));
    const Rgb white{255, 255, 255};
    CHECK(near(p.at(256, 191), white, 2) && near(p.at(256, 192), white, 2));   // x(179) = 246, x(181) = 266
    CHECK(near(p.at(250, 191), white, 2) && near(p.at(262, 192), white, 2));
    CHECK(near(p.at(100, 191), kSea, 0) && near(p.at(400, 192), kSea, 0) && near(p.at(10, 192), kSea, 0));
    // ... and a world-wide view shows a line that crosses the edge of the map at both edges
    TileCompositor w;
    Pic pw(1024, 512);
    w.setView(0.0, 0.0, 360.0);
    w.setLayers(false, false);
    Trail edge;
    edge.points = {{0.0, 170.0}, {0.0, -170.0}};
    edge.rgba = 0xffffffffu;
    w.setTrails({edge});
    CHECK(pw.compose(w));
    int xr = 0, yr = 0, xl = 0, yl = 0;
    pixelOf(w, pw, 0.0, 175.0, xr, yr);
    pixelOf(w, pw, 0.0, -175.0, xl, yl);
    CHECK(near(pw.at(xr, yr), white, 2) || near(pw.at(xr, yr - 1), white, 2));
    CHECK(near(pw.at(xl, yl), white, 2) || near(pw.at(xl, yl - 1), white, 2));
    CHECK(near(pw.at(512, 255), kSea, 0) && near(pw.at(512, 256), kSea, 0) && near(pw.at(300, 256), kSea, 0));
    // many long trails do not make a mess of the buffer: 200 trails of 400 points
    std::vector<Trail> many;
    for (int k = 0; k < 200; ++k) {
        Trail t;
        for (int i = 0; i < 400; ++i) { t.points.push_back({-20.0 + 0.1 * k + 0.01 * i, -20.0 + 0.1 * i}); }
        t.rgba = 0x3a8fc0ffu;
        t.dashed = (k % 3) == 0;
        many.push_back(t);
    }
    c.setView(0.0, 0.0, 51.2);
    c.setTrails(many);
    CHECK(p.compose(c));
    CHECK(p.count(kSea, 0, 0, 512, 384, 0) < 512 * 384);
}

// ----------------------------------------------------------------------------------------------------------------------------
// I. the display settings the map applies
// ----------------------------------------------------------------------------------------------------------------------------
std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
// <repo>/src/<rel>, from this file's own path (.../src/slint/tests/test_slint_host_map.cpp).
std::string srcPath(const char* rel) {
    std::string p = __FILE__;
    for (int i = 0; i < 3; ++i) {
        const std::size_t cut = p.find_last_of("/\\");
        if (cut == std::string::npos) { break; }
        p.resize(cut);
    }
    return p + "/" + rel;
}
bool has(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

// How many pixels thick the painted trail is across column x between rows y0 and y1: the sum of each pixel's coverage of `ink` over the sea.
double thicknessAt(const Pic& p, int x, int y0, int y1, const Rgb& ink) {
    double sum = 0.0;
    for (int y = y0; y < y1; ++y) { sum += static_cast<double>(p.at(x, y).r - kSea.r) / static_cast<double>(ink.r - kSea.r); }
    return sum;
}

void sectionTrailLook() {
    std::printf("I. the display settings the map applies\n");
    const auto lonAt = [](double x) { return (x - 256.0) / 10.0; };
    const auto latAt = [](double y) { return (192.0 - y) / 10.0; };
    Trail line;                                                              // y = 100 from x = 50 to 150, as G has it
    line.points = {{latAt(100.0), lonAt(50.0)}, {latAt(100.0), lonAt(150.0)}};
    line.rgba = 0xf5a524ffu;
    TileCompositor c;
    Pic p(512, 384);
    c.setView(0.0, 0.0, 51.2);
    c.setLayers(false, false);
    c.setTrails({line});
    CHECK(p.compose(c));
    // before anything is set, a trail is the 2 px line it always was
    CHECK(c.trailWidthPx() == 2 && c.trailStyle() == TrailStyle::Line && c.trailStrokePx() == 2.0);
    CHECK(std::fabs(thicknessAt(p, 100, 30, 170, kOrange) - 2.0) < 0.1);

    // the width is what paints: 4 (the saved default), 9 (odd: the edges are half covered), 86 (the largest at a 96 px icon) - across a straight segment
    for (int w : {1, 2, 4, 9, 30, 86}) {
        c.setTrailLook(w, 96, TrailStyle::Line);
        CHECK(c.compose(p.data(), p.w, p.h));
        const double got = thicknessAt(p, 100, 30, 170, kOrange);
        if (std::fabs(got - w) >= 0.15) { std::printf("  width %d painted %.2f px thick\n", w, got); }
        CHECK(std::fabs(got - w) < 0.15);
        CHECK(c.trailWidthPx() == w);
    }
    // 86 px is measurably wider than the 2 px default, and the painted pixels grow with it
    c.setTrailLook(2, 48, TrailStyle::Line);
    CHECK(c.compose(p.data(), p.w, p.h));
    const int inkDefault = p.count(kOrange, 0, 0, 512, 384, 40);
    c.setTrailLook(86, 96, TrailStyle::Line);
    CHECK(c.compose(p.data(), p.w, p.h));
    const int inkWide = p.count(kOrange, 0, 0, 512, 384, 40);
    CHECK(inkWide > inkDefault * 20);
    CHECK(near(p.at(100, 60), kOrange, 2) && near(p.at(100, 140), kOrange, 2) && near(p.at(100, 50), kSea, 0) && near(p.at(100, 150), kSea, 0));

    // the bounds are the classic window's: a trail wider than the wingspan of the icon in force paints at the wingspan, 0 and below paint at 1
    struct Bound { int saved, icon, painted; };
    for (const Bound& b : {Bound{200, 96, 86}, Bound{86, 96, 86}, Bound{87, 96, 86}, Bound{40, 16, 14}, Bound{20, 48, 20}, Bound{50, 48, 43},
                           Bound{0, 48, 1}, Bound{-7, 48, 1}, Bound{100, 200, 86}, Bound{100, 3, 14}}) {
        c.setTrailLook(b.saved, b.icon, TrailStyle::Line);
        c.compose(p.data(), p.w, p.h);                                       // (false when the bounded look is the one before: the picture stands)
        const double got = thicknessAt(p, 100, 20, 180, kOrange);
        if (c.trailWidthPx() != b.painted || std::fabs(got - b.painted) >= 0.15) { std::printf("  saved %d at icon %d: width %d, painted %.2f, wanted %d\n", b.saved, b.icon, c.trailWidthPx(), got, b.painted); }
        CHECK(c.trailWidthPx() == b.painted);
        CHECK(std::fabs(got - b.painted) < 0.15);
    }

    // the two styles are not one picture: a ribbon is the line's width + 3 px, the same path in the same colour
    c.setTrailLook(8, 96, TrailStyle::Line);
    Pic lineShot(512, 384), ribbonShot(512, 384);
    CHECK(lineShot.compose(c));
    c.setTrailLook(8, 96, TrailStyle::Ribbon);
    CHECK(c.trailStyle() == TrailStyle::Ribbon && c.trailWidthPx() == 8 && c.trailStrokePx() == 11.0);
    CHECK(ribbonShot.compose(c));
    CHECK(std::fabs(thicknessAt(lineShot, 100, 30, 170, kOrange) - 8.0) < 0.15);
    CHECK(std::fabs(thicknessAt(ribbonShot, 100, 30, 170, kOrange) - 11.0) < 0.15);
    int differ = 0;
    for (std::size_t i = 0; i < lineShot.px.size(); i += 4) { if (lineShot.px[i] != ribbonShot.px[i]) { ++differ; } }
    CHECK(differ > 250);                                                     // 3 rows of 100 and the longer ends
    savePng(lineShot, "map-compositor-trail-line-8px.png");
    savePng(ribbonShot, "map-compositor-trail-ribbon-8px.png");
    // ... and the picture is the line's again when the style goes back; setting the same look is no change (compose says so)
    c.setTrailLook(8, 96, TrailStyle::Line);
    CHECK(c.compose(p.data(), p.w, p.h));
    CHECK(p.px == lineShot.px);
    c.setTrailLook(8, 96, TrailStyle::Line);
    CHECK(!c.compose(p.data(), p.w, p.h));
    // a ribbon at the wingspan is 3 px over it (the scope's rule: the width bounded first, the ribbon added after)
    c.setTrailLook(500, 96, TrailStyle::Ribbon);
    CHECK(c.trailWidthPx() == 86 && c.trailStrokePx() == 89.0);
    // a style that is not 1 is a line (map_view.cpp: trailStyle_ != 1)
    CHECK(mapLookOf(48, 4, 0).trailStyle == TrailStyle::Line && mapLookOf(48, 4, 1).trailStyle == TrailStyle::Ribbon);
    CHECK(mapLookOf(48, 4, 2).trailStyle == TrailStyle::Line && mapLookOf(48, 4, -1).trailStyle == TrailStyle::Line);

    // a dash scales with the width, or a 6 px dash with caps of 10 px would be a solid bar: 20 px wide, the dash is 60 on and 40 off
    Trail dashed = line;
    dashed.dashed = true;
    c.setTrails({dashed});
    c.setTrailLook(20, 96, TrailStyle::Line);
    CHECK(c.compose(p.data(), p.w, p.h));
    CHECK(near(p.at(80, 100), kOrange, 3) && near(p.at(130, 100), kSea, 0) && near(p.at(130, 95), kSea, 0));
    // while a 2 px dashed trail is the 6 on, 4 off it was (G holds the pixels)
    c.setTrailLook(2, 96, TrailStyle::Line);
    CHECK(c.compose(p.data(), p.w, p.h));
    CHECK(near(p.at(52, 100), kOrange, 3) && near(p.at(57, 100), kSea, 0));
    // a wide translucent trail is composited once: it is not darker at its joint
    Trail vee;
    vee.points = {{latAt(300.0), lonAt(50.0)}, {latAt(250.0), lonAt(80.0)}, {latAt(300.0), lonAt(110.0)}};
    vee.rgba = 0xf5a52480u;
    c.setTrails({vee});
    c.setTrailLook(20, 96, TrailStyle::Ribbon);
    CHECK(c.compose(p.data(), p.w, p.h));
    const Rgb half = over(kOrange, 128.0 / 255.0, kSea);
    int brightest = 0;
    for (int y = 200; y < 340; ++y) { for (int x = 20; x < 140; ++x) { brightest = std::max(brightest, p.at(x, y).r); } }
    CHECK(brightest <= half.r + 2 && brightest >= half.r - 2);

    // what a target is drawn at: the icon size for an aircraft, nothing (the pane's own glyph) for any other kind
    const MapLook look = mapLookOf(96, 86, 1);
    CHECK(targetIconPx(1, look) == 96 && targetIconPx(2, look) == 0 && targetIconPx(3, look) == 0 && targetIconPx(4, look) == 0 && targetIconPx(0, look) == 0);
    CHECK(targetIconPx(1, MapLook{}) == 48);
    // the icon is bounded 16..96, and the trail by the BOUNDED icon: a saved 200 px icon is 96, a saved 5 is 16
    CHECK(mapLookOf(200, 86, 0).iconPx == 96 && mapLookOf(5, 1, 0).iconPx == 16 && mapLookOf(48, 4, 0).iconPx == 48 && mapLookOf(-3, 4, 0).iconPx == 16);
    CHECK(mapLookOf(200, 200, 0).trailWidthPx == 86 && mapLookOf(200, 90, 0).trailWidthPx == 86 && mapLookOf(5, 90, 0).trailWidthPx == 14);
    CHECK(mapLookOf(48, 0, 0).trailWidthPx == 1 && mapLookOf(48, -4, 0).trailWidthPx == 1 && mapLookOf(48, 4, 0).trailWidthPx == 4);
    const MapLook want96{96, 86, TrailStyle::Ribbon};
    CHECK(mapLookOf(96, 86, 1) == want96 && mapLookOf(96, 86, 1) != MapLook{});
    CHECK(clampAircraftIconPx(200) == 96 && clampAircraftIconPx(16) == 16 && clampAircraftIconPx(15) == 16 && aircraftWingspanPx(96) == 86 && aircraftWingspanPx(48) == 43 && aircraftWingspanPx(16) == 14);
    CHECK(clampTrailWidthPx(86, 96) == 86 && clampTrailWidthPx(87, 96) == 86 && clampTrailWidthPx(0, 96) == 1);
    // the copied constants are the classic window's (gui/aircraft_icons.hpp includes imgui.h, so they are copies and this reads the source)
    const std::string ic = slurp(srcPath("gui/aircraft_icons.hpp"));
    CHECK(has(ic, "kAircraftIconMinPx = 16;") && has(ic, "kAircraftIconMaxPx = 96;") && has(ic, "kAircraftIconDefaultPx = 48;"));
    CHECK(has(ic, "kTrailWidthMinPx = 1;") && has(ic, "kTrailWidthDefaultPx = 4;") && has(ic, "kAircraftSpanFraction = 114.0f / 128.0f;"));
    CHECK(has(ic, "static_cast<float>(clampAircraftIconPx(iconPx)) *") && has(ic, "kAircraftSpanFraction +") && has(ic, "0.5f);"));
    CHECK(kAircraftIconMinPx == 16 && kAircraftIconMaxPx == 96 && kAircraftIconDefaultPx == 48 && kTrailWidthMinPx == 1 && kTrailWidthDefaultPx == 4);
    const std::string mv = slurp(srcPath("gui/map_view.cpp"));
    CHECK(has(mv, "if (trailStyle_ != 1) {"));                               // a line unless it is 1
    const std::string sv = slurp(srcPath("gui/scope_view.cpp"));
    CHECK(has(sv, "(optTrail_ == 2 ? w + 3.0f : w)"));                       // the ribbon's extra 3 px
}

// ----------------------------------------------------------------------------------------------------------------------------
// H. the figures, the clamps, and the time
// ----------------------------------------------------------------------------------------------------------------------------
void sectionFigures() {
    std::printf("H. the figures and the time\n");
    {
        TileCompositor c;
        c.setSize(1024, 768);
        c.setView(0.0, 0.0, 360.0);
        const double equator = 6371000.0 * kPi / 180.0 * 360.0 / 1024.0;
        CHECK_NEAR(c.metresPerPixel(), equator, 1e-6);                       // 39 092 m a pixel
        c.setView(60.0, 0.0, 360.0);
        CHECK_NEAR(c.metresPerPixel(), equator * 0.5, 1e-3);                 // cos 60
        c.setView(0.0, 0.0, 0.02);
        CHECK_NEAR(c.metresPerPixel(), 6371000.0 * kPi / 180.0 * 0.02 / 1024.0, 1e-9);
        c.setMercator(true);
        c.setView(51.5, -0.1, 6.0);
        CHECK_NEAR(c.metresPerPixel(), 6371000.0 * kPi / 180.0 * std::cos(51.5 * kPi / 180.0) * 6.0 / 1024.0, 1e-6);
        // the clamps and the folds
        c.setView(95.0, 190.0, 1000.0);
        CHECK(c.centreLatDeg() == 89.0 && c.centreLonDeg() == -170.0 && c.spanDeg() == 360.0);
        c.setView(-100.0, -190.0, 0.001);
        CHECK(c.centreLatDeg() == -89.0 && c.centreLonDeg() == 170.0 && c.spanDeg() == TileCompositor::kMinSpanDeg);
        c.setView(10.0, 540.0, 10.0);
        CHECK(c.centreLonDeg() == -180.0);
        // a place that is not a place is not gone to
        c.setView(10.0, 20.0, 30.0);
        c.setView(std::nan(""), 0.0, 5.0);
        c.setView(0.0, std::numeric_limits<double>::infinity(), 5.0);
        c.setView(0.0, 0.0, std::nan(""));
        CHECK(c.centreLatDeg() == 10.0 && c.centreLonDeg() == 20.0 && c.spanDeg() == 30.0);
        CHECK(c.mercator());
        c.setMercator(false);
        CHECK(!c.mercator());
        CHECK(TileCompositor::kMaxTiles == 256 && TileCompositor::kMissingRetryComposes == 600);
    }
    // ---- the picture, once, for a person: rings, wedges and trails over the coast ----
    {
        TileCompositor c;
        Pic p(768, 512);
        c.setView(51.5, -0.1, 6.0);
        c.setHome(true, 51.5, -0.1);
        std::vector<double> cov(72, 0.0);
        for (int i = 0; i < 72; ++i) { cov[i] = 80.0 + 120.0 * std::fabs(std::sin(i * 0.17)); }
        c.setCoverage(cov);
        Trail solid, dashed, loop;
        for (int i = 0; i < 40; ++i) { solid.points.push_back({50.0 + i * 0.07, -3.0 + i * 0.12 + 0.2 * std::sin(i * 0.4)}); }
        solid.rgba = 0xf5a524ffu;
        dashed = solid;
        for (auto& pt : dashed.points) { pt.first += 0.6; }
        dashed.rgba = 0x7ad7ffb0u;
        dashed.dashed = true;
        for (int i = 0; i < 60; ++i) { loop.points.push_back({51.5 + 0.8 * std::sin(i * 0.1047), -0.1 + 1.4 * std::cos(i * 0.1047)}); }
        loop.rgba = 0x7fe6adffu;
        loop.closed = true;
        loop.dashed = true;
        c.setTrails({solid, dashed, loop});
        CHECK(p.compose(c));
        savePng(p, "map-compositor-layers.png");
    }
    // ---- the time: 1024 x 768, no tiles ----
    {
        TileCompositor c;
        Pic p(1024, 768);
        c.setView(51.5, -0.1, 12.0);
        c.setHome(true, 51.5, -0.1);
        c.setCoverage(std::vector<double>(72, 250.0));
        std::vector<Trail> trails;
        for (int k = 0; k < 60; ++k) {
            Trail t;
            for (int i = 0; i < 30; ++i) { t.points.push_back({48.0 + k * 0.1 + i * 0.05, -6.0 + i * 0.4 + 0.3 * std::sin(i * 0.5 + k)}); }
            t.rgba = 0xf5a524c0u;
            t.dashed = (k % 4) == 0;
            trails.push_back(t);
        }
        c.setTrails(trails);
        c.setSize(1024, 768);
        const auto timed = [&](int runs, const std::function<void(int)>& dirty) {
            std::vector<double> ms;
            for (int i = 0; i < runs; ++i) {
                dirty(i);
                const auto t0 = std::chrono::steady_clock::now();
                const bool ok = c.compose(p.data(), 1024, 768);
                const auto t1 = std::chrono::steady_clock::now();
                CHECK(ok);
                ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
            }
            std::sort(ms.begin(), ms.end());
            return ms;
        };
        const std::vector<double> all = timed(15, [&](int i) { c.setView(51.5 + 0.01 * i, -0.1, 12.0); });
        std::printf("  1024x768 compose, coast + graticule + rings + wedges + 60 trails: best %.2f ms, median %.2f ms, worst %.2f ms\n", all.front(), all[all.size() / 2], all.back());
        CHECK(all[all.size() / 2] < 50.0);
        c.setTrails({});
        c.setHome(false, 0.0, 0.0);
        const std::vector<double> bare = timed(15, [&](int i) { c.setView(51.5 + 0.01 * i, -0.1, 12.0); });
        std::printf("  1024x768 compose, coastline + graticule only: best %.2f ms, median %.2f ms, worst %.2f ms\n", bare.front(), bare[bare.size() / 2], bare.back());
        CHECK(bare[bare.size() / 2] < 50.0);
        c.setMercator(true);
        c.setView(30.0, 20.0, 200.0);
        const std::vector<double> merc = timed(9, [&](int i) { c.setView(30.0 + 0.1 * i, 20.0, 200.0); });
        std::printf("  1024x768 compose, Mercator coastline: best %.2f ms, median %.2f ms, worst %.2f ms\n", merc.front(), merc[merc.size() / 2], merc.back());
        CHECK(merc[merc.size() / 2] < 50.0);
        c.setMercator(false);
        c.setView(0.0, 0.0, 360.0);
        const std::vector<double> world = timed(9, [&](int i) { c.setView(0.0, 0.1 * i, 360.0); });
        std::printf("  1024x768 compose, the whole world: best %.2f ms, median %.2f ms, worst %.2f ms\n", world.front(), world[world.size() / 2], world.back());
        CHECK(world[world.size() / 2] < 50.0);
        // the tiles, for the record (not part of the 50 ms contract): bilinear, cached
        Source src;
        src.answer = [](std::uint32_t, std::uint32_t, std::uint32_t, Bytes& rgb) {
            fillTile(rgb, 256, 120, 130, 140);
            return 1;
        };
        c.setMercator(true);
        c.setTileSource(src.make(), 256, 0, 19);
        c.setView(45.0, 10.0, 21.0);
        const std::vector<double> tiled = timed(9, [&](int i) { c.setView(45.0 + 0.01 * i, 10.0, 21.0 + 0.05 * i); });
        std::printf("  1024x768 compose, tiles (bilinear, cached): best %.2f ms, median %.2f ms, worst %.2f ms\n", tiled.front(), tiled[tiled.size() / 2], tiled.back());
        CHECK(tiled[tiled.size() / 2] < 100.0);
    }
}

}  // namespace

int main() {
    sectionCoast();
    sectionAntimeridian();
    sectionGraticule();
    sectionRings();
    sectionTiles();
    sectionProjection();
    sectionDirty();
    sectionWedges();
    sectionTrails();
    sectionTrailLook();
    sectionFigures();
    return testSummary("test_slint_host_map");
}
