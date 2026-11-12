// Tests for core/access_palette.hpp: the colour sets and their verifier.
//
// The verifier is checked against known numbers first (WCAG luminance and
// contrast, the Machado matrices, CIELAB), then the five sets are run through
// it, then each set is DAMAGED (two colours made equal, a colour made equal to
// its ground) to show the verifier notices.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/access_palette.hpp"

#include <cmath>
#include <cstdio>
#include <set>
#include <string>

#include "test_check.hpp"

namespace {

using namespace cascade::core::access;

void wcagArithmetic() {
    CHECK_NEAR(relativeLuminance(rgbHex(0x000000)), 0.0, 1e-12);
    CHECK_NEAR(relativeLuminance(rgbHex(0xFFFFFF)), 1.0, 1e-12);
    CHECK_NEAR(contrastRatio(rgbHex(0xFFFFFF), rgbHex(0x000000)), 21.0, 1e-9);
    CHECK_NEAR(contrastRatio(rgbHex(0x000000), rgbHex(0xFFFFFF)), 21.0, 1e-9);   // symmetric
    CHECK_NEAR(contrastRatio(rgbHex(0x123456), rgbHex(0x123456)), 1.0, 1e-12);
    // The channel weights: pure primaries are exactly the coefficients.
    CHECK_NEAR(relativeLuminance(rgbHex(0xFF0000)), 0.2126, 1e-12);
    CHECK_NEAR(relativeLuminance(rgbHex(0x00FF00)), 0.7152, 1e-12);
    CHECK_NEAR(relativeLuminance(rgbHex(0x0000FF)), 0.0722, 1e-12);
    // The two grey values every contrast checker quotes as the AA text boundary
    // on white: #767676 passes 4.5:1, #777777 does not.
    CHECK(contrastRatio(rgbHex(0x767676), rgbHex(0xFFFFFF)) >= 4.5);
    CHECK(contrastRatio(rgbHex(0x777777), rgbHex(0xFFFFFF)) < 4.5);
    CHECK_NEAR(contrastRatio(rgbHex(0x767676), rgbHex(0xFFFFFF)), 4.54, 0.01);
    // The linearisation is continuous at its knee (0.04045, i.e. 10.31 of 255).
    CHECK_NEAR(srgbToLinear(10.0), 10.0 / 255.0 / 12.92, 1e-12);
    CHECK_NEAR(srgbToLinear(11.0), std::pow((11.0 / 255.0 + 0.055) / 1.055, 2.4), 1e-12);
    CHECK(std::fabs(srgbToLinear(10.0) - srgbToLinear(11.0)) < 0.001);
    // Round trip linear <-> sRGB8.
    for (int c = 0; c <= 255; ++c) { CHECK_NEAR(linearToSrgb8(srgbToLinear(c)), c, 1e-6); }
}

void simulationArithmetic() {
    // Each matrix row sums to 1 (white stays white), published constants.
    for (Vision v : {Vision::Protanopia, Vision::Deuteranopia, Vision::Tritanopia}) {
        const double (&m)[9] = cvdMatrix(v);
        for (int r = 0; r < 3; ++r) { CHECK_NEAR(m[3 * r] + m[3 * r + 1] + m[3 * r + 2], 1.0, 2e-6); }
        const RgbD w = simulate(rgbHex(0xFFFFFF), v);
        CHECK_NEAR(w.r, 255.0, 0.1); CHECK_NEAR(w.g, 255.0, 0.1); CHECK_NEAR(w.b, 255.0, 0.1);
        const RgbD k = simulate(rgbHex(0x000000), v);
        CHECK_NEAR(k.r, 0.0, 1e-9); CHECK_NEAR(k.g, 0.0, 1e-9); CHECK_NEAR(k.b, 0.0, 1e-9);
        // a neutral grey stays neutral
        const RgbD g = simulate(rgbHex(0x808080), v);
        CHECK_NEAR(g.r, 128.0, 0.5); CHECK_NEAR(g.g, 128.0, 0.5); CHECK_NEAR(g.b, 128.0, 0.5);
    }
    // Normal vision is the identity.
    const RgbD id = simulate(rgbHex(0x3DDC84), Vision::Normal);
    CHECK(id.r == 0x3D && id.g == 0xDC && id.b == 0x84);
    // The published numbers, spot-checked verbatim.
    CHECK(cvdMatrix(Vision::Protanopia)[1] == 1.052583);
    CHECK(cvdMatrix(Vision::Deuteranopia)[0] == 0.367322);
    CHECK(cvdMatrix(Vision::Tritanopia)[7] == 0.691367);
    // The defining effect: red and green, far apart for normal vision, collapse
    // under protanopia and deuteranopia; blue and yellow collapse under tritanopia.
    const Rgb red = rgbHex(0xFF0000), green = rgbHex(0x00FF00);
    const double normalRG = distanceUnder(red, green, Vision::Normal);
    CHECK(distanceUnder(red, green, Vision::Protanopia) < 0.4 * normalRG);
    CHECK(distanceUnder(red, green, Vision::Deuteranopia) < 0.4 * normalRG);
    const Rgb blue = rgbHex(0x0000FF), cyan = rgbHex(0x00FFFF);
    CHECK(distanceUnder(blue, cyan, Vision::Tritanopia) < 0.6 * distanceUnder(blue, cyan, Vision::Normal));
}

void labArithmetic() {
    const Lab white = toLab(simulate(rgbHex(0xFFFFFF), Vision::Normal));
    CHECK_NEAR(white.l, 100.0, 0.01); CHECK_NEAR(white.a, 0.0, 0.05); CHECK_NEAR(white.b, 0.0, 0.05);
    const Lab black = toLab(simulate(rgbHex(0x000000), Vision::Normal));
    CHECK_NEAR(black.l, 0.0, 1e-6);
    CHECK_NEAR(deltaE76(white, black), 100.0, 0.01);
    CHECK_NEAR(deltaE76(white, white), 0.0, 1e-12);
    CHECK_NEAR(deltaE76(white, black), deltaE76(black, white), 1e-12);
    // sRGB red in CIELAB under D65 is L~53.2, a~80.1, b~67.2 (the well-known values).
    const Lab red = toLab(simulate(rgbHex(0xFF0000), Vision::Normal));
    CHECK_NEAR(red.l, 53.24, 0.1); CHECK_NEAR(red.a, 80.09, 0.2); CHECK_NEAR(red.b, 67.20, 0.2);
}

void tableIsConsistent() {
    std::set<std::string> keys;
    for (int i = 0; i < kPaletteCount; ++i) {
        const Palette& p = palettes()[static_cast<std::size_t>(i)];
        CHECK(static_cast<int>(p.id) == i);
        CHECK(p.key != nullptr && p.key[0] != '\0' && p.label != nullptr && p.label[0] != '\0');
        CHECK(keys.insert(p.key).second);                      // unique stored keys
        CHECK(paletteIdFromKey(p.key) == p.id);
        CHECK(&paletteFor(p.id) == &p);
        // no two colours of one palette's status group are the same hex, etc: covered by distance below
    }
    CHECK(paletteIdFromKey("") == PaletteId::Normal);
    CHECK(paletteIdFromKey("rainbow") == PaletteId::Normal);
    CHECK(paletteIdFromKey("Normal") == PaletteId::Normal);     // exact keys only, falls to the default anyway
    CHECK(paletteIdFromKey("high-contrast") == PaletteId::HighContrast);
    CHECK(&paletteFor(static_cast<PaletteId>(99)) == &palettes()[0]);
    CHECK(std::string(roleName(Role::Radio8)) == "radio-8");
}

void everySetPasses() {
    for (const Palette& p : palettes()) {
        const std::vector<Failure> f = verifyPalette(p);
        for (const Failure& x : f) { std::printf("  FAILURE in %s: %s\n", p.key, x.what.c_str()); }
        CHECK(f.empty());
        std::printf("  %-13s smallest dE76 in a group: normal %.1f  prot %.1f  deut %.1f  trit %.1f\n", p.key,
                    smallestDistance(p, Vision::Normal), smallestDistance(p, Vision::Protanopia),
                    smallestDistance(p, Vision::Deuteranopia), smallestDistance(p, Vision::Tritanopia));
    }
    // Margin: the CVD sets clear the stated distance by at least 5 under their own deficiency.
    CHECK(smallestDistance(paletteFor(PaletteId::Deuteranopia), Vision::Deuteranopia) >= kMinDeltaE + 5.0);
    CHECK(smallestDistance(paletteFor(PaletteId::Protanopia), Vision::Protanopia) >= kMinDeltaE + 5.0);
    CHECK(smallestDistance(paletteFor(PaletteId::Tritanopia), Vision::Tritanopia) >= kMinDeltaE + 5.0);
    // High contrast holds text at 7:1 against black.
    const Palette& hc = paletteFor(PaletteId::HighContrast);
    CHECK(contrastRatio(hc.role[static_cast<int>(Role::Fg)], hc.ground[0]) >= 7.0);
    CHECK(contrastRatio(hc.role[static_cast<int>(Role::Muted)], hc.ground[0]) >= 7.0);
}

void normalIsNotColourBlindSafe() {
    // The Workbench look is NOT a colour-blind-safe set, and the verifier says
    // so: its green and red status colours cannot be told apart by a person with
    // protanopia or deuteranopia. Pinned so nobody ships it as one.
    const Palette& n = paletteFor(PaletteId::Normal);
    CHECK(verifyDistinct(n, Vision::Normal).empty());
    const std::vector<Failure> prot = verifyDistinct(n, Vision::Protanopia);
    const std::vector<Failure> deut = verifyDistinct(n, Vision::Deuteranopia);
    CHECK(!prot.empty());
    CHECK(!deut.empty());
    bool okBad = false;
    for (const Failure& f : deut) { if (f.what.find("status-ok vs status-bad") != std::string::npos) { okBad = true; } }
    CHECK(okBad);
    // Radio identity colours 1 and 4 (blue and violet) merge too.
    bool radio = false;
    for (const Failure& f : prot) { if (f.what.find("radio-1 vs radio-4") != std::string::npos) { radio = true; } }
    CHECK(radio);
    for (const Failure& f : deut) { std::printf("  (normal, deuteranopia) %s\n", f.what.c_str()); }
}

void damagedSetsAreCaught() {
    for (const Palette& orig : palettes()) {
        // Two colours of one group made equal: caught under every vision.
        for (const Group& g : distinctGroups()) {
            for (std::size_t j = 1; j < g.members.size(); ++j) {
                Palette p = orig;
                p.role[static_cast<std::size_t>(g.members[j])] = p.role[static_cast<std::size_t>(g.members[0])];
                for (Vision v : requiredVisions(orig.id)) {
                    CHECK(!verifyDistinct(p, v).empty());
                }
                CHECK(!verifyPalette(p).empty());
            }
        }
        // A colour made equal to a ground: contrast failure.
        for (int r = 0; r < kRoleCount; ++r) {
            Palette p = orig;
            p.role[static_cast<std::size_t>(r)] = p.ground[2];
            CHECK(!verifyContrast(p).empty());
        }
        // One channel step on a colour that is already far: not a failure (no false alarm).
        Palette q = orig;
        q.role[static_cast<int>(Role::StatusOk)].g = static_cast<std::uint8_t>(q.role[static_cast<int>(Role::StatusOk)].g ^ 1);
        CHECK(verifyPalette(q).empty());
    }
}

}  // namespace

int main() {
    wcagArithmetic();
    simulationArithmetic();
    labArithmetic();
    tableIsConsistent();
    everySetPasses();
    normalIsNotColourBlindSafe();
    damagedSetsAreCaught();
    return testSummary("test_access_palette");
}
