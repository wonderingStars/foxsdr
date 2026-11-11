// The user-editable waterfall gradient model (src/core/gradient_model.hpp): stops, interpolation, validation, table, text form, fuzz.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <random>

#include "core/gradient_model.hpp"
#include "test_check.hpp"

using namespace cascade::core::gradient;
namespace colour = cascade::core::colour;

namespace {

Gradient bw(Interp i) {
    Gradient g = defaultGradient();
    g.interp = i;
    return g;
}

void testInterpolationClosedForms() {
    // Black to white, halfway: straight sRGB 127.5 -> 128; linear light decode(0.5)=0.2140, i.e. encode(0.5) = 0.7354 -> 188; OKLab L = 0.5 is a
    // grey whose linear value is 0.125 and whose sRGB encoding is 0.3886 -> 99.
    const Rgb8 s = colourAtPosition(bw(Interp::Srgb), 0.5);
    const Rgb8 l = colourAtPosition(bw(Interp::LinearLight), 0.5);
    const Rgb8 o = colourAtPosition(bw(Interp::Oklab), 0.5);
    std::printf("MEAS black->white at 0.5: srgb %d, linear-light %d, oklab %d (pins 128, 188, 99)\n", s.r, l.r, o.r);
    CHECK(s.r == 128 && s.g == 128 && s.b == 128);
    CHECK(l.r == 188 && l.g == 188 && l.b == 188);
    CHECK(o.r == 99 && o.g == 99 && o.b == 99);
    // Ends are exact in every mode; NaN and out-of-range read as the ends.
    for (Interp i : {Interp::Srgb, Interp::LinearLight, Interp::Oklab}) {
        Gradient g = bw(i);
        g.stops[0].colour = {10, 20, 30};
        g.stops[1].colour = {240, 200, 100};
        CHECK(colourAtPosition(g, 0.0) == (Rgb8{10, 20, 30}));
        CHECK(colourAtPosition(g, 1.0) == (Rgb8{240, 200, 100}));
        CHECK(colourAtPosition(g, -5.0) == (Rgb8{10, 20, 30}));
        CHECK(colourAtPosition(g, std::nan("")) == (Rgb8{10, 20, 30}));
        CHECK(colourAtPosition(g, 9.0) == (Rgb8{240, 200, 100}));
    }
    // Three stops: the middle one is hit exactly and the halves are separate ramps.
    Gradient g;
    g.interp = Interp::Srgb;
    g.stops = {{0.0, {0, 0, 0}}, {0.25, {200, 0, 0}}, {1.0, {200, 0, 255}}};
    CHECK(colourAtPosition(g, 0.25) == (Rgb8{200, 0, 0}));
    CHECK(colourAtPosition(g, 0.125) == (Rgb8{100, 0, 0}));
    CHECK(colourAtPosition(g, 0.625).b == 128);  // halfway up the second ramp: 127.5 -> 128
}

void testTable() {
    const Table t = toTable(bw(Interp::Srgb));
    CHECK(t[0] == (Rgb8{0, 0, 0}) && t[255] == (Rgb8{255, 255, 255}));
    for (std::size_t i = 0; i < 256; ++i) { CHECK(t[i].r == i && t[i].g == i); }  // a straight ramp is the identity
    // The table type is the compositor's: colourAt() reads it directly.
    CHECK(colour::colourAt(t, 0.0f) == t[0]);
    CHECK(colour::colourAt(t, 1.0f) == t[255]);
    CHECK(colour::colourAt(t, 0.5f).r == 128);
    // An OKLab black-to-white ramp has monotone lightness; a gradient that goes dark again does not.
    CHECK(lightnessMonotone(toTable(bw(Interp::Oklab))));
    Gradient dip = bw(Interp::Oklab);
    dip.stops = {{0.0, {255, 255, 255}}, {0.5, {0, 0, 0}}, {1.0, {255, 255, 255}}};
    CHECK(!lightnessMonotone(toTable(dip)));
    // The stock maps sampled into stops and back stay close to the stock table (a 17-stop copy).
    for (int m = 0; m < colour::kMapCount; ++m) {
        const Gradient gm = fromMap(static_cast<colour::MapId>(m), 17);
        CHECK(validate(gm) == Problem::None);
        const Table back = toTable(gm), stock = colour::colourMap(static_cast<colour::MapId>(m));
        double worst = 0;
        for (std::size_t i = 0; i < 256; ++i) { worst = std::max(worst, colour::labDistance(colour::rgb8ToOklab(back[i]), colour::rgb8ToOklab(stock[i]))); }
        std::printf("MEAS stock map %d copied to 17 stops: worst OKLab distance %.4f\n", m, worst);
        CHECK(worst < 0.05);
    }
    // An invalid gradient gives the grey ramp, not garbage.
    Gradient bad = bw(Interp::Oklab);
    bad.stops.clear();
    const Table gt = toTable(bad);
    CHECK(gt[100] == (Rgb8{100, 100, 100}));
}

void testValidate() {
    Gradient g = bw(Interp::Oklab);
    CHECK(validate(g) == Problem::None);
    Gradient one = g;
    one.stops.pop_back();
    CHECK(validate(one) == Problem::TooFewStops);
    Gradient many = g;
    many.stops.clear();
    for (std::size_t i = 0; i <= kMaxStops; ++i) { many.stops.push_back({static_cast<double>(i) / kMaxStops, {}}); }
    CHECK(many.stops.size() == kMaxStops + 1 && validate(many) == Problem::TooManyStops);
    Gradient first = g;
    first.stops[0].pos = 0.1;
    CHECK(validate(first) == Problem::FirstNotAtZero);
    Gradient last = g;
    last.stops[1].pos = 0.9;
    CHECK(validate(last) == Problem::LastNotAtOne);
    Gradient oor = g;
    oor.stops[1].pos = 1.5;
    CHECK(validate(oor) == Problem::PositionOutOfRange);
    Gradient nf = g;
    nf.stops[0].pos = std::nan("");
    CHECK(validate(nf) == Problem::NotFinite);
    Gradient order = g;
    order.stops = {{0.0, {}}, {0.6, {}}, {0.5, {}}, {1.0, {}}};
    CHECK(validate(order) == Problem::PositionsNotIncreasing);
    Gradient gap = g;
    gap.stops = {{0.0, {}}, {0.5, {}}, {0.5005, {}}, {1.0, {}}};
    CHECK(validate(gap) == Problem::GapTooSmall);
    Gradient name = g;
    name.name = "bad|name";
    CHECK(validate(name) == Problem::BadName);
    name.name = std::string(33, 'a');
    CHECK(validate(name) == Problem::BadName);
    for (Problem p : {Problem::TooFewStops, Problem::TooManyStops, Problem::BadName, Problem::GapTooSmall}) { CHECK(problemText(p)[0] != '\0'); }
}

void testEditor() {
    GradientEditor e;
    CHECK(e.size() == 2);
    const int a = e.addStop(0.5, {255, 0, 0});
    CHECK(a == 1 && e.size() == 3);
    CHECK(e.addStop(0.25, {0, 255, 0}) == 1);  // inserted in order; the red one is now index 2
    CHECK(e.gradient().stops[2].colour == (Rgb8{255, 0, 0}));
    CHECK(e.addStop(0.5003, {}) == -1);        // too close to the stop at 0.5
    CHECK(e.addStop(0.0, {}) == -1 && e.addStop(1.0, {}) == -1);
    CHECK(e.addStop(std::nan(""), {}) == -1);
    CHECK(validate(e.gradient()) == Problem::None);
    // Moving: held between the neighbours, kMinGap clear.
    const double p = e.moveStop(1, 0.9);
    CHECK(p < 0.5 && p >= 0.5 - kMinGap - 1e-9);
    CHECK_NEAR(e.moveStop(1, 0.3), 0.3, 1e-12);
    CHECK_NEAR(e.moveStop(1, -4.0), kMinGap, 1e-12);
    CHECK(e.moveStop(0, 0.2) < 0 && e.moveStop(3, 0.2) < 0 && e.moveStop(9, 0.2) < 0);
    CHECK(e.moveStop(1, std::nan("")) < 0);
    CHECK(validate(e.gradient()) == Problem::None);
    // Removing: interior only.
    CHECK(!e.removeStop(0) && !e.removeStop(3) && !e.removeStop(77));
    CHECK(e.removeStop(1) && e.size() == 3);
    CHECK(e.removeStop(1) && e.size() == 2);
    CHECK(!e.removeStop(1));
    // Colours, interpolation, name.
    CHECK(e.setColour(0, {1, 2, 3}) && !e.setColour(5, {}));
    CHECK(e.gradient().stops[0].colour == (Rgb8{1, 2, 3}));
    e.setInterp(Interp::Srgb);
    CHECK(e.gradient().interp == Interp::Srgb);
    CHECK(e.setName("Night sky") && !e.setName("a|b") && e.gradient().name == "Night sky");
    // The editor stops at the maximum.
    GradientEditor full;
    int added = 0;
    for (std::size_t i = 1; i < 200; ++i) { if (full.addStop(static_cast<double>(i) / 200.0, {}) >= 0) { ++added; } }
    CHECK(full.size() == kMaxStops && added == static_cast<int>(kMaxStops) - 2);
    CHECK(validate(full.gradient()) == Problem::None);
    // Reverse mirrors positions and keeps colours.
    GradientEditor r;
    r.addStop(0.2, {9, 9, 9});
    r.setColour(0, {1, 1, 1});
    r.setColour(2, {7, 7, 7});
    r.reverse();
    CHECK(r.gradient().stops[0].colour == (Rgb8{7, 7, 7}) && r.gradient().stops[2].colour == (Rgb8{1, 1, 1}));
    CHECK_NEAR(r.gradient().stops[1].pos, 0.8, 1e-12);
    CHECK(validate(r.gradient()) == Problem::None);
    // Pick.
    CHECK(r.pick(0.79, 0.02) == 1 && r.pick(0.5, 0.02) == -1 && r.pick(0.001, 0.02) == 0);
    r.reset();
    CHECK(r.size() == 2);
    // A bad gradient handed to the constructor is replaced by the default.
    Gradient bad;
    GradientEditor eb(bad);
    CHECK(eb.size() == 2 && validate(eb.gradient()) == Problem::None);
}

void testTextForm() {
    Gradient g;
    g.interp = Interp::LinearLight;
    g.name = "Night sky-2";
    g.stops = {{0.0, {0, 0, 16}}, {0.3333, {0x12, 0xab, 0xef}}, {1.0, {255, 255, 224}}};
    const std::string s = serialise(g);
    CHECK(s == "FOXGRAD1|linear|Night sky-2|0@000010,3333@12abef,10000@ffffe0");
    const ParseResult r = parse(s);
    CHECK(r.ok && r.gradient == g);
    CHECK(serialise(r.gradient) == s);
    // Refusals name a reason.
    const char* bad[] = {"",
                         "FOXGRAD2|srgb||0@000000,10000@ffffff",
                         "FOXGRAD1|srgb|",
                         "FOXGRAD1|srgb|x",
                         "FOXGRAD1|rgb||0@000000,10000@ffffff",
                         "FOXGRAD1|srgb||0@000000,10000@fffff",
                         "FOXGRAD1|srgb||0@00000g,10000@ffffff",
                         "FOXGRAD1|srgb||0@000000,10000@FFFFFF",  // upper case is not the canonical spelling
                         "FOXGRAD1|srgb||0@000000,10001@ffffff",
                         "FOXGRAD1|srgb||0@000000,-1@ffffff",
                         "FOXGRAD1|srgb||@000000,10000@ffffff",
                         "FOXGRAD1|srgb||0@000000,1x@ffffff",
                         "FOXGRAD1|srgb||0@000000",                      // one stop
                         "FOXGRAD1|srgb||5@000000,10000@ffffff",        // first not at 0
                         "FOXGRAD1|srgb||0@000000,9000@ffffff",         // last not at 1
                         "FOXGRAD1|srgb||0@000000,6000@222222,5000@333333,10000@ffffff",
                         "FOXGRAD1|srgb|a:b|0@000000,10000@ffffff",
                         "FOXGRAD1|srgb||0@000000,,10000@ffffff",
                         "FOXGRAD1|srgb||0@000000,10000@ffffff,",
                         "FOXGRAD1|srgb||0@0000000,10000@ffffff"};
    for (const char* b : bad) {
        const ParseResult p = parse(b);
        CHECK(!p.ok);
        CHECK(p.syntax[0] != '\0' || p.problem != Problem::None);
        CHECK(p.gradient.stops.empty());
    }
    // 33 stops and an over-long text.
    std::string many = "FOXGRAD1|srgb||0@000000";
    for (int i = 1; i <= 31; ++i) { many += "," + std::to_string(i * 300) + "@101010"; }
    many += ",10000@ffffff";
    CHECK(parse(many).problem == Problem::TooManyStops);
    CHECK(!parse(std::string(kMaxTextLen + 1, 'x')).ok);
    // Every round trip of generated gradients is exact.
    std::mt19937_64 rng(11);
    for (int n = 0; n < 500; ++n) {
        GradientEditor e;
        const int stops = static_cast<int>(rng() % 12);
        for (int i = 0; i < stops; ++i) { e.addStop(static_cast<double>(rng() % 10001) / 10000.0, {static_cast<std::uint8_t>(rng()), static_cast<std::uint8_t>(rng()), static_cast<std::uint8_t>(rng())}); }
        e.setInterp(static_cast<Interp>(rng() % 3));
        const std::string t = serialise(e.gradient());
        const ParseResult pr = parse(t);
        CHECK(pr.ok && pr.gradient == e.gradient() && serialise(pr.gradient) == t);
    }
}

void testFuzz() {
    // Mutated valid strings and random bytes: never a crash, and whatever is accepted is valid and re-serialises to itself.
    std::mt19937_64 rng(2025);
    const std::string seed = "FOXGRAD1|oklab|Fuzz seed|0@000000,2500@3344ff,5000@aabbcc,10000@ffffe0";
    int accepted = 0;
    for (int i = 0; i < 40000; ++i) {
        std::string t = seed;
        const int edits = 1 + static_cast<int>(rng() % 4);
        for (int e = 0; e < edits; ++e) {
            const std::size_t at = rng() % (t.size() + 1);
            switch (rng() % 4) {
                case 0: if (at < t.size()) { t[at] = static_cast<char>(rng()); } break;
                case 1: t.insert(at, 1, static_cast<char>(rng())); break;
                case 2: if (at < t.size()) { t.erase(at, 1 + rng() % 4); } break;
                default: t.insert(at, "," + std::to_string(rng() % 12000) + "@0a0b0c"); break;
            }
        }
        const ParseResult p = parse(t);
        if (p.ok) {
            ++accepted;
            CHECK(validate(p.gradient) == Problem::None);
            const std::string again = serialise(p.gradient);
            CHECK(parse(again).ok && parse(again).gradient == p.gradient);
            (void)toTable(p.gradient);
        } else {
            CHECK(p.gradient.stops.empty());
        }
    }
    for (int i = 0; i < 5000; ++i) {
        std::string t(rng() % 200, '\0');
        for (char& c : t) { c = static_cast<char>(rng()); }
        CHECK(!parse(t).ok);
    }
    std::printf("MEAS fuzz: 40000 mutated gradient strings (%d accepted, all valid) and 5000 random byte strings\n", accepted);
    CHECK(accepted > 100);
}

}  // namespace

int main() {
    testInterpolationClosedForms();
    testTable();
    testValidate();
    testEditor();
    testTextForm();
    testFuzz();
    return testSummary("test_gradient_model");
}
