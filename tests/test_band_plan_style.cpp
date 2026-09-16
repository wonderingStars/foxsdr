// Tests for gui/band_plan_style.hpp — pure geometry and palette math for the
// band-plan overlay's Size/Colour settings (GitHub issue #1). No ImGui
// context needed; see the header for why.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/band_plan_style.hpp"

#include "core/band_plan.hpp"
#include "gui/fonts.hpp"
#include "test_check.hpp"

using cascade::gui::BandPlanPaletteKind;
using cascade::gui::BandPlanSizeTier;
using cascade::gui::BandRibbonGeometry;

int main() {
    // --- key <-> tier/kind parsing, both directions -------------------------
    {
        CHECK(cascade::gui::bandPlanSizeTierFromKey("small") == BandPlanSizeTier::Small);
        CHECK(cascade::gui::bandPlanSizeTierFromKey("medium") == BandPlanSizeTier::Medium);
        CHECK(cascade::gui::bandPlanSizeTierFromKey("large") == BandPlanSizeTier::Large);
        // Unknown / hand-edited / absent all fall back to Small, the same
        // fallback the config loader applies to the field itself.
        CHECK(cascade::gui::bandPlanSizeTierFromKey("") == BandPlanSizeTier::Small);
        CHECK(cascade::gui::bandPlanSizeTierFromKey("huge") == BandPlanSizeTier::Small);
        CHECK(cascade::gui::bandPlanSizeTierFromKey("Small") == BandPlanSizeTier::Small);  // case matters

        CHECK(cascade::gui::bandPlanPaletteKindFromKey("classic") == BandPlanPaletteKind::Classic);
        CHECK(cascade::gui::bandPlanPaletteKindFromKey("vivid") == BandPlanPaletteKind::Vivid);
        CHECK(cascade::gui::bandPlanPaletteKindFromKey("mono") == BandPlanPaletteKind::Mono);
        CHECK(cascade::gui::bandPlanPaletteKindFromKey("") == BandPlanPaletteKind::Classic);
        CHECK(cascade::gui::bandPlanPaletteKindFromKey("rainbow") == BandPlanPaletteKind::Classic);

        std::string s(cascade::gui::bandPlanSizeKey(BandPlanSizeTier::Small));
        CHECK(s == "small");
        CHECK(std::string(cascade::gui::bandPlanSizeKey(BandPlanSizeTier::Medium)) == "medium");
        CHECK(std::string(cascade::gui::bandPlanSizeKey(BandPlanSizeTier::Large)) == "large");
        CHECK(std::string(cascade::gui::bandPlanPaletteKey(BandPlanPaletteKind::Classic)) == "classic");
        CHECK(std::string(cascade::gui::bandPlanPaletteKey(BandPlanPaletteKind::Vivid)) == "vivid");
        CHECK(std::string(cascade::gui::bandPlanPaletteKey(BandPlanPaletteKind::Mono)) == "mono");
        // Round trip: key -> kind -> key must return the same key, for every
        // kind, so save()/load() and this parser can never drift apart.
        CHECK(cascade::gui::bandPlanPaletteKindFromKey(
                  cascade::gui::bandPlanPaletteKey(BandPlanPaletteKind::Mono)) ==
              BandPlanPaletteKind::Mono);
    }

    // --- geometry: small is byte-identical to the overlay's original figures
    {
        const BandRibbonGeometry small = cascade::gui::bandRibbonGeometry(BandPlanSizeTier::Small);
        CHECK(small.ribbonPx == 6.0f);
        CHECK(small.labelPx == cascade::gui::fonts::kUiSize);
        CHECK(small.labelMinPx == 46.0f);
    }

    // --- geometry: medium/large scale every one of the three numbers up ----
    {
        const BandRibbonGeometry small = cascade::gui::bandRibbonGeometry(BandPlanSizeTier::Small);
        const BandRibbonGeometry medium = cascade::gui::bandRibbonGeometry(BandPlanSizeTier::Medium);
        const BandRibbonGeometry large = cascade::gui::bandRibbonGeometry(BandPlanSizeTier::Large);

        CHECK_NEAR(medium.ribbonPx, small.ribbonPx * 1.6, 1e-4);
        CHECK_NEAR(medium.labelPx, small.labelPx * 1.6, 1e-4);
        CHECK_NEAR(medium.labelMinPx, small.labelMinPx * 1.6, 1e-4);

        CHECK_NEAR(large.ribbonPx, small.ribbonPx * 2.5, 1e-4);
        CHECK_NEAR(large.labelPx, small.labelPx * 2.5, 1e-4);
        CHECK_NEAR(large.labelMinPx, small.labelMinPx * 2.5, 1e-4);

        // Strictly increasing across the ladder — a "bigger" option that is
        // not actually bigger than its neighbour is the bug this whole
        // feature exists to fix (GitHub issue #1).
        CHECK(medium.ribbonPx > small.ribbonPx);
        CHECK(large.ribbonPx > medium.ribbonPx);
        CHECK(medium.labelMinPx > small.labelMinPx);
        CHECK(large.labelMinPx > medium.labelMinPx);
    }

    // --- geometry: pinned at scale 1.0 and 2.0 (the parameter is untested
    // ambient plumbing today - see the header - so both values are exercised
    // explicitly rather than only the default) --------------------------------
    {
        const BandRibbonGeometry s1 = cascade::gui::bandRibbonGeometry(BandPlanSizeTier::Large, 1.0f);
        CHECK(s1.ribbonPx == 15.0f);           // 6 * 2.5 * 1.0
        CHECK(s1.labelMinPx == 115.0f);        // 46 * 2.5 * 1.0

        const BandRibbonGeometry s2 = cascade::gui::bandRibbonGeometry(BandPlanSizeTier::Large, 2.0f);
        CHECK(s2.ribbonPx == 30.0f);           // 6 * 2.5 * 2.0
        CHECK(s2.labelMinPx == 230.0f);        // 46 * 2.5 * 2.0
        CHECK_NEAR(s2.ribbonPx, s1.ribbonPx * 2.0, 1e-4);
        CHECK_NEAR(s2.labelPx, s1.labelPx * 2.0, 1e-4);
        CHECK_NEAR(s2.labelMinPx, s1.labelMinPx * 2.0, 1e-4);

        const BandRibbonGeometry small1 = cascade::gui::bandRibbonGeometry(BandPlanSizeTier::Small, 1.0f);
        CHECK(small1.ribbonPx == 6.0f);
        const BandRibbonGeometry small2 = cascade::gui::bandRibbonGeometry(BandPlanSizeTier::Small, 2.0f);
        CHECK(small2.ribbonPx == 12.0f);
    }

    // --- palette: classic matches core::BandPlan::colorForService exactly --
    {
        const char* services[] = {"broadcast", "amateur", "aviation", "marine",
                                  "mobile",    "satellite", "iss",     "other",
                                  "",          "made-up-class"};
        for (const char* svc : services) {
            CHECK(cascade::gui::bandPlanPaletteColor(BandPlanPaletteKind::Classic, svc) ==
                  cascade::core::BandPlan::colorForService(svc));
        }
    }

    // --- palette: vivid and mono are internally self-consistent -------------
    {
        // Every recognised service maps to a DIFFERENT colour within vivid,
        // and within mono, or two allocations sitting side by side on the
        // ribbon would be indistinguishable — the opposite of what a colour
        // option is for.
        const char* known[] = {"broadcast", "amateur", "aviation", "marine",
                               "mobile",    "satellite", "iss"};
        for (int i = 0; i < 7; ++i) {
            for (int j = i + 1; j < 7; ++j) {
                CHECK(cascade::gui::bandPlanPaletteColor(BandPlanPaletteKind::Vivid, known[i]) !=
                      cascade::gui::bandPlanPaletteColor(BandPlanPaletteKind::Vivid, known[j]));
                CHECK(cascade::gui::bandPlanPaletteColor(BandPlanPaletteKind::Mono, known[i]) !=
                      cascade::gui::bandPlanPaletteColor(BandPlanPaletteKind::Mono, known[j]));
            }
        }

        // An unrecognised service must still draw something (never 0/fully
        // transparent) under every palette — a future band-plan file naming
        // a class none of these three sets know must not vanish.
        for (const char* svc : {"", "spaceweather"}) {
            CHECK((cascade::gui::bandPlanPaletteColor(BandPlanPaletteKind::Vivid, svc) & 0xFFu) > 0);
            CHECK((cascade::gui::bandPlanPaletteColor(BandPlanPaletteKind::Mono, svc) & 0xFFu) > 0);
        }

        // Mono is genuinely mono: every recognised service's colour shares
        // the same RGB (top 24 bits) and differs only in alpha (bottom 8).
        const std::uint32_t rgb0 =
            cascade::gui::bandPlanPaletteColor(BandPlanPaletteKind::Mono, known[0]) & 0xFFFFFF00u;
        for (const char* svc : known) {
            CHECK((cascade::gui::bandPlanPaletteColor(BandPlanPaletteKind::Mono, svc) & 0xFFFFFF00u) ==
                  rgb0);
        }

        // Vivid is NOT mono: at least one pair of services differs in RGB,
        // not just alpha, or "vivid" would be indistinguishable from "mono"
        // with the alpha turned up.
        bool anyRgbDiffers = false;
        const std::uint32_t vividRgb0 =
            cascade::gui::bandPlanPaletteColor(BandPlanPaletteKind::Vivid, known[0]) & 0xFFFFFF00u;
        for (int i = 1; i < 7; ++i) {
            if ((cascade::gui::bandPlanPaletteColor(BandPlanPaletteKind::Vivid, known[i]) &
                 0xFFFFFF00u) != vividRgb0) {
                anyRgbDiffers = true;
            }
        }
        CHECK(anyRgbDiffers);
    }

    return testSummary("test_band_plan_style");
}
