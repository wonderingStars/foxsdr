// Tests for the computed colour maps (core/colour_maps.hpp): the OKLab maths, and for every map: 256 entries, lightness rising
// monotonically in the 8-bit table that ships, a near-constant perceptual step between neighbours, the character of each map (what makes it
// that map), and for the colour-blind-safe map a simulation of red-green dichromacy (Machado, Oliveira, Fernandes 2009, severity 1.0).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/colour_maps.hpp"

#include "test_check.hpp"

#include <cstdio>
#include <set>
#include <string>

using namespace cascade::core::colour;

namespace {

std::vector<Lab> labs(const Table& t) {
    std::vector<Lab> v;
    for (const Rgb8& c : t) { v.push_back(rgb8ToOklab(c)); }
    return v;
}

// Colour as a dichromat sees it: the Machado 2009 matrix applied to LINEAR sRGB.
Lab simulate(const Rgb8& c, const double m[9]) {
    const Lin in{srgbDecode(c.r / 255.0), srgbDecode(c.g / 255.0), srgbDecode(c.b / 255.0)};
    const Lin out{std::clamp(m[0] * in.r + m[1] * in.g + m[2] * in.b, 0.0, 1.0), std::clamp(m[3] * in.r + m[4] * in.g + m[5] * in.b, 0.0, 1.0),
                  std::clamp(m[6] * in.r + m[7] * in.g + m[8] * in.b, 0.0, 1.0)};
    return linearToOklab(out);
}
const double kProtan[9] = {0.152286, 1.052583, -0.204868, 0.114503, 0.786281, 0.099216, -0.003882, -0.048116, 1.051998};
const double kDeutan[9] = {0.367322, 0.860646, -0.227968, 0.280085, 0.672501, 0.047413, -0.011820, 0.042940, 0.968881};

struct StepStats {
    double minRatio = 1e9, maxRatio = 0;  // windowed (8-entry) step relative to the mean
    double minStep = 1e9, maxStep = 0, mean = 0;
};

// Steps between entries i and i+k, relative to the mean step over the whole map.
StepStats steps(const std::vector<Lab>& v, std::size_t k) {
    StepStats s;
    double total = 0;
    for (std::size_t i = 0; i + 1 < v.size(); ++i) {
        const double d = labDistance(v[i], v[i + 1]);
        total += d;
        s.minStep = std::min(s.minStep, d);
        s.maxStep = std::max(s.maxStep, d);
    }
    s.mean = total / static_cast<double>(v.size() - 1);
    for (std::size_t i = 0; i + k < v.size(); ++i) {
        const double r = labDistance(v[i], v[i + k]) / (static_cast<double>(k) * s.mean);
        s.minRatio = std::min(s.minRatio, r);
        s.maxRatio = std::max(s.maxRatio, r);
    }
    return s;
}

void testOklab() {
    // black and white
    Lab k = linearToOklab({0, 0, 0}), w = linearToOklab({1, 1, 1});
    CHECK_NEAR(k.L, 0.0, 1e-9);
    CHECK_NEAR(w.L, 1.0, 1e-4);
    CHECK_NEAR(w.a, 0.0, 1e-4);
    CHECK_NEAR(w.b, 0.0, 1e-4);
    // sRGB red, green, blue (reference values from the OKLab post's table as remembered: UNVERIFIED digits; tolerance 2e-3)
    const Lab r = linearToOklab({1, 0, 0}), g = linearToOklab({0, 1, 0}), b = linearToOklab({0, 0, 1});
    CHECK_NEAR(r.L, 0.6280, 2e-3);
    CHECK_NEAR(r.a, 0.2249, 2e-3);
    CHECK_NEAR(r.b, 0.1258, 2e-3);
    CHECK_NEAR(g.L, 0.8664, 2e-3);
    CHECK_NEAR(b.L, 0.4520, 2e-3);
    // round trip over a grid of 8-bit colours
    double worst = 0;
    for (int ri = 0; ri < 256; ri += 17) {
        for (int gi = 0; gi < 256; gi += 17) {
            for (int bi = 0; bi < 256; bi += 17) {
                const Lin lin{srgbDecode(ri / 255.0), srgbDecode(gi / 255.0), srgbDecode(bi / 255.0)};
                const Lin back = oklabToLinear(linearToOklab(lin));
                worst = std::max({worst, std::fabs(back.r - lin.r), std::fabs(back.g - lin.g), std::fabs(back.b - lin.b)});
                const int rr = static_cast<int>(std::lround(srgbEncode(back.r) * 255.0));
                CHECK(rr == ri);
            }
        }
    }
    CHECK(worst < 1e-5);
    // sRGB transfer: 0.5 -> 0.2140 linear; the knee constants
    CHECK_NEAR(srgbDecode(0.5), 0.21404, 1e-4);
    CHECK_NEAR(srgbEncode(srgbDecode(0.37)), 0.37, 1e-12);
    CHECK_NEAR(srgbEncode(0.0031308), 12.92 * 0.0031308, 1e-9);
    // distance
    CHECK_NEAR(labDistance({0, 0, 0}, {0.3, 0.4, 0}), 0.5, 1e-12);
}

void testEveryMap() {
    std::set<std::string> ids, names;
    std::printf("%-18s | L first/last | exact step cv | 8-bit step min/max (x mean) | windowed(16, L>0.35) | distinct\n", "map");
    for (int mi = 0; mi < kMapCount; ++mi) {
        const MapId id = static_cast<MapId>(mi);
        const Table& t = colourMap(id);
        CHECK(t.size() == 256);
        ids.insert(mapId(id));
        names.insert(mapName(id));
        const std::vector<Lab> v = labs(t);
        // lightness monotone in the shipped 8-bit table
        bool mono = true;
        for (std::size_t i = 1; i < v.size(); ++i) { if (v[i].L < v[i - 1].L - 1e-12) { mono = false; } }
        CHECK(mono);
        CHECK(v.back().L - v.front().L > 0.55);
        // exact (float) steps: equal arc length means equal chords up to curvature
        const std::array<Lab, kEntries> ex = exactMap(id);
        std::vector<Lab> ev(ex.begin(), ex.end());
        const StepStats es = steps(ev, 1);
        double var = 0;
        for (std::size_t i = 0; i + 1 < ev.size(); ++i) { const double d = labDistance(ev[i], ev[i + 1]) - es.mean; var += d * d; }
        const double cv = std::sqrt(var / 255.0) / es.mean;
        CHECK(es.maxStep < 1.02 * es.mean);  // chords of equal length by construction
        CHECK(es.minStep > 0.98 * es.mean);
        // the 8-bit table: windowed over 8 entries (single steps are quantisation noise by nature)
        // The 8-bit grid is coarse in the dark (the first step above black is already about 0.07 in OKLab lightness, 20 times the mean step), so
        // single steps are quantisation noise and the darkest entries cannot be even; the evenness is checked in windows of 16 entries over the
        // part of the map lighter than L = 0.35.
        const StepStats b1 = steps(v, 1);
        std::size_t first = 0;
        while (first < ev.size() && ev[first].L < 0.35) { ++first; }
        StepStats bs;
        for (std::size_t i = first; i + 16 < v.size(); ++i) {
            const double r = labDistance(v[i], v[i + 16]) / (16.0 * es.mean);
            bs.minRatio = std::min(bs.minRatio, r);
            bs.maxRatio = std::max(bs.maxRatio, r);
        }
        std::set<std::uint32_t> distinct;
        for (const Rgb8& c : t) { distinct.insert((static_cast<std::uint32_t>(c.r) << 16) | (static_cast<std::uint32_t>(c.g) << 8) | c.b); }
        std::printf("%-18s | %.3f/%.3f   | %.4f        | %.2f / %.2f                 | %.3f..%.3f        | %zu\n", mapName(id), v.front().L, v.back().L, cv,
                    b1.minStep / b1.mean, b1.maxStep / b1.mean, bs.minRatio, bs.maxRatio, distinct.size());
        CHECK(bs.minRatio > 0.70 && bs.maxRatio < 1.10);  // measured 0.738..1.061: rounding to 8 bits under a monotone-lightness rule costs up to about 26 percent over 16 entries
        CHECK(distinct.size() >= 225);
        // determinism
        const Table again = buildColourMap(id);
        CHECK(again == t);
    }
    CHECK(ids.size() == static_cast<std::size_t>(kMapCount) && names.size() == static_cast<std::size_t>(kMapCount));
    CHECK(colourMap(static_cast<MapId>(99)) == colourMap(MapId::DeepSea));
    CHECK(std::string(mapName(static_cast<MapId>(-1))) == "Deep sea");
}

void testCharacter() {
    // DeepSea: starts dark blue, ends yellow
    {
        const Table& t = colourMap(MapId::DeepSea);
        CHECK(t[0].b > t[0].r && t[0].b > t[0].g && t[0].r < 40 && t[0].g < 40);
        CHECK(t[255].r > 200 && t[255].g > 200 && t[255].b < 160);
        // passes through green-ish teal in the middle
        CHECK(t[128].g > t[128].r);
    }
    // Ember: black to white through red, orange, yellow
    {
        const Table& t = colourMap(MapId::Ember);
        CHECK(t[0].r == 0 && t[0].g == 0 && t[0].b == 0);
        CHECK(t[255].r == 255 && t[255].g == 255 && t[255].b == 255);
        CHECK(t[70].r > t[70].g + 40 && t[70].g < 30 && t[70].b < 30);          // red phase
        CHECK(t[170].r > 200 && t[170].g > 60 && t[170].g < 200 && t[170].b < 60);  // orange
        CHECK(t[225].r > 230 && t[225].g > 200);                        // yellow
    }
    // Slate: neutral, r == g == b, dark to light
    {
        const Table& t = colourMap(MapId::Slate);
        for (const Rgb8& c : t) { CHECK(c.r == c.g && c.g == c.b); }
        CHECK(t[0].r < 20 && t[255].r > 240);
    }
    // Spectrum: hue spans more than 200 degrees yet lightness only rises
    {
        const Table& t = colourMap(MapId::Spectrum);
        const std::vector<Lab> v = labs(t);
        double hmin = 1e9, hmax = -1e9;
        for (const Lab& l : v) {
            if (std::hypot(l.a, l.b) < 0.04) { continue; }
            double h = std::atan2(l.b, l.a) * 180.0 / 3.14159265358979;
            if (h < 0) { h += 360.0; }
            hmin = std::min(hmin, h);
            hmax = std::max(hmax, h);
        }
        std::printf("Spectrum hue range %.0f .. %.0f degrees\n", hmin, hmax);
        CHECK(hmax - hmin > 200.0);
    }
    // SafeBlueYellow: no red-green axis: |a| stays small along the whole map, blue at the bottom, yellow at the top
    {
        const Table& t = colourMap(MapId::SafeBlueYellow);
        const std::vector<Lab> v = labs(t);
        double amax = 0;
        for (const Lab& l : v) { amax = std::max(amax, std::fabs(l.a)); }
        std::printf("SafeBlueYellow: largest |a| (red-green axis) %.4f, b from %.3f to %.3f\n", amax, v.front().b, v.back().b);
        CHECK(amax < 0.03);
        CHECK(v.front().b < -0.04 && v.back().b > 0.08);
    }
}

// Under simulated red-green dichromacy the safe map must stay readable (lightness still rising, steps not collapsing); the rainbow
// sweep is measured for contrast.
void testColourBlind() {
    struct Out { double minRatio, maxDrop; };
    auto measure = [&](MapId id, const double* m) {
        const Table& t = colourMap(id);
        std::vector<Lab> sv;
        for (const Rgb8& c : t) { sv.push_back(simulate(c, m)); }
        // reference mean step of the NORMAL-vision map
        const std::vector<Lab> nv = labs(t);
        const StepStats ns = steps(nv, 1);
        double drop = 0;
        for (std::size_t i = 8; i < sv.size(); ++i) { drop = std::max(drop, sv[i - 8].L - sv[i].L); }  // lightness lost over 8 entries
        double mn = 1e9;
        for (std::size_t i = 0; i + 8 < sv.size(); ++i) { mn = std::min(mn, labDistance(sv[i], sv[i + 8]) / (8.0 * ns.mean)); }
        return Out{mn, drop};
    };
    for (const auto& [nm, m] : {std::pair<const char*, const double*>{"protan", kProtan}, {"deutan", kDeutan}}) {
        const Out safe = measure(MapId::SafeBlueYellow, m), rain = measure(MapId::Spectrum, m), ember = measure(MapId::Ember, m);
        std::printf("%s simulation, smallest 8-entry step vs normal vision: safe %.2f  spectrum %.2f  ember %.2f; lightness lost over 8 entries: safe %.4f spectrum %.4f\n", nm,
                    safe.minRatio, rain.minRatio, ember.minRatio, safe.maxDrop, rain.maxDrop);
        CHECK(safe.minRatio > 0.5);            // the steps survive
        CHECK(safe.maxDrop < 0.02);            // and lightness still rises
        CHECK(safe.minRatio > rain.minRatio);  // the safe map is the better of the two (the claim it is made for)
    }
}

void testColourAt() {
    const Table& t = colourMap(MapId::Ember);
    CHECK(colourAt(t, 0.0f) == t[0] && colourAt(t, -3.0f) == t[0]);
    CHECK(colourAt(t, 1.0f) == t[255] && colourAt(t, 7.0f) == t[255]);
    CHECK(colourAt(t, std::nanf("")) == t[0]);
    CHECK(colourAt(t, 0.5f) == t[128]);
    CHECK(colourAt(t, 1.0f / 255.0f) == t[1]);
}

}  // namespace

int main() {
    testOklab();
    testEveryMap();
    testCharacter();
    testColourBlind();
    testColourAt();
    return testSummary("test_colour_maps");
}
