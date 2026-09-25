/*
 * THE MUTE BANNER'S "STOP PLUGIN" KEY IS ALWAYS WHOLE, READABLE AND CLICKABLE.
 *
 * WHY THIS EXISTS (themes repair round 2, 2026-09-25). The banner says which
 * decoders are holding the sound down and carries the one key that stops them.
 * Round 1 moved it off the counter, onto a strip it was CLIPPED to and drawn
 * smaller to fit - and on today's own default 1280 x 720 window the clip took
 * the key: with two decoders named it was lettered at 9-10 px, with three it
 * read "Stop p", with four it was gone. An ImGui item that is clipped away
 * cannot be clicked, so four muting decoders left the user no working key.
 * The sweep in test_tune_control measured the banner after clipping it, so it
 * could not see any of that; and every width it used was a made-up number.
 *
 * So this measures the REAL widths - the key's label and the banner's words in
 * every catalogue the build carries, in every theme's typeface, at the bar's
 * own font - and for every bar width from below the narrowest window to a
 * 2400-px one, every counter layout, and one to six decoders named, asks
 * gui::layoutMuteBanner (the function app_window.cpp draws from) where the key
 * and the words go. It requires:
 *
 *   - the key lies WHOLE inside the place the banner is clipped to, and inside
 *     the bar - so nothing of it is clipped away;
 *   - its lettering is at least 13 px: the floor below which a key's label
 *     stops being read at a glance (the bar's own words are 17) - and words
 *     drawn beside it are at least 13 px too, and never larger than the key;
 *   - on today's own 1280 x 720 window the key is at the bar's FULL size in
 *     every language and for any count: the words give way, never the key;
 *   - the key and the words lie on no part of the deck (transport, master
 *     cluster, counter plate, volume dial, meters), and not on each other;
 *   - the words stay inside the place too (they are shortened with an
 *     ellipsis, and the whole list is in a tooltip, when they do not fit);
 *   - where the bar's middle holds the whole banner it is drawn there, at full
 *     size, exactly where 0.99.35 put it.
 *
 * A CJK catalogue this machine has no face for is skipped and counted - the
 * application does not offer that language here either (fonts::canDraw).
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <imgui.h>

#include "core/i18n.hpp"
#include "core/utf8_text.hpp"
#include "gui/fonts.hpp"
#include "gui/theme.hpp"
#include "gui/tune_control.hpp"
#include "test_check.hpp"

namespace th = cascade::gui::theme;
namespace fonts = cascade::gui::fonts;
using cascade::gui::CounterLayout;
using cascade::gui::MuteBannerLayout;

namespace {

struct Box {
    float x0, y0, x1, y1;
};
bool hit(const Box& a, const Box& b) {
    return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}
bool inside(const Box& a, const Box& o) {
    const float e = 0.01f;
    return a.x0 >= o.x0 - e && a.y0 >= o.y0 - e && a.x1 <= o.x1 + e && a.y1 <= o.y1 + e;
}

// What one catalogue in one typeface measures at the bar's font.
struct Measured {
    std::string theme, lang;
    float lineH = 0.0f;
    float keyLabelW = 0.0f;  // at the bar's font
    float wordsW[7] = {};    // [n] = the words naming n decoders, at the bar's font
    // [n][i] = the banner naming n decoders measured at size i, as the
    // application measures it before laying the banner out.
    cascade::gui::MuteBannerSize sizes[7][cascade::gui::kMuteBannerMaxSizes] = {};
    int nSizes = 0;
    float keyPadX = 0.0f, gap = 0.0f;
};

// THE FLOOR, stated here and not read from the header: a test that takes its
// limit from the constant it is guarding moves with it (tune_control.hpp says
// kMuteKeyMinPx is 13; this says 13 is the requirement).
constexpr float kKeyFloorPx = 13.0f;

// Real plugin names, longest last so six names is the longest banner.
const std::vector<std::string> kNames = {"ADS-B Decoder", "AIS Decoder",   "APRS Decoder",
                                         "POCSAG Decoder", "Satellites", "Nearby Signal Catch"};

}  // namespace

int main() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1600.0f, 1000.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    CHECK(fonts::load());

    // --- measure: every theme's typeface, every catalogue ----------------------------------
    std::vector<Measured> all;
    int skipped = 0;
    float widestKey = 0.0f;
    std::string widestKeyWho;
    for (int t = 0; t < th::kThemeCount; ++t) {
        const th::ThemeId id = static_cast<th::ThemeId>(t);
        th::setTheme(id);
        th::applyTheme();
        fonts::setPreferredPair(th::preferredFontPair(id));
        for (const cascade::i18n::Language& lang : cascade::i18n::languages()) {
            if (!fonts::canDraw(lang.code)) {
                ++skipped;
                continue;
            }
            cascade::i18n::setLanguage(lang.code);
            fonts::applyLanguage(lang.code);
            fonts::applyPending();
            ImGui::NewFrame();
            ImGui::Begin("measure");
            Measured m;
            m.theme = th::themeKey(id);
            m.lang = lang.code;
            // The bar draws in the default face at the style's base size - the
            // font every window starts with - and pushes none of its own.
            m.lineH = ImGui::GetFontSize();
            const char* keyLabel = cascade::i18n::trId("Stop plugin##mute_banner");
            m.keyLabelW = ImGui::CalcTextSize(keyLabel, nullptr, true).x;
            float px[cascade::gui::kMuteBannerMaxSizes];
            m.nSizes = cascade::gui::muteBannerSizes(m.lineH, px, cascade::gui::kMuteBannerMaxSizes);
            for (int n = 1; n <= 6; ++n) {
                const std::vector<std::string> some(kNames.begin(), kNames.begin() + n);
                std::string words;
                cascade::core::formatUtf8(words, cascade::i18n::tr("Sound muted by %s"),
                                          cascade::gui::joinMuteNames(some).c_str());
                m.wordsW[n] = ImGui::CalcTextSize(words.c_str()).x;
                for (int i = 0; i < m.nSizes; ++i) {
                    if (i > 0) { ImGui::PushFont(nullptr, px[i]); }
                    m.sizes[n][i].px = px[i];
                    m.sizes[n][i].wordsW = ImGui::CalcTextSize(words.c_str()).x;
                    m.sizes[n][i].keyLabelW = ImGui::CalcTextSize(keyLabel, nullptr, true).x;
                    if (i > 0) { ImGui::PopFont(); }
                }
            }
            m.keyPadX = ImGui::GetStyle().FramePadding.x;
            m.gap = ImGui::GetStyle().ItemSpacing.x;
            ImGui::End();
            ImGui::EndFrame();
            if (m.keyLabelW > widestKey) {
                widestKey = m.keyLabelW;
                widestKeyWho = m.theme + "/" + m.lang;
            }
            all.push_back(m);
        }
    }
    cascade::i18n::setLanguage("en");
    std::printf("  measured %zu theme/catalogue pairs (%d skipped: no face on this machine); "
                "widest key label %.1f px (%s), bar font %.1f px\n",
                all.size(), skipped, widestKey, widestKeyWho.c_str(),
                all.empty() ? 0.0f : all[0].lineH);
    CHECK(all.size() >= static_cast<std::size_t>(th::kThemeCount) * 20u);

    // --- place: every bar width, every counter layout, one to six names ---------------------
    long placed = 0;
    long bad = 0;
    int bySlot[3] = {0, 0, 0};
    int twoLine = 0;
    int shortened = 0;
    float smallestKeyPx = 1.0e9f;
    for (const CounterLayout layout : {CounterLayout{1, true}, CounterLayout{1, false},
                                       CounterLayout{2, false}, CounterLayout{2, true}}) {
        for (float barW = static_cast<float>(cascade::gui::kDeckMinWindowW) - 50.0f;
             barW <= 2400.0f; barW += 1.0f) {
            const float s = cascade::gui::deckScale(barW, layout, 0.62f);
            const float coreW = cascade::gui::deckCoreW(layout);
            const bool meters = cascade::gui::deckMetersFit(barW, layout, s);
            const float plateW = cascade::gui::counterPlateW(layout);
            const float plateH = cascade::gui::counterPlateH(layout);
            const float volCx = cascade::gui::deckVolumeCx(layout);
            const Box bar{0.0f, 0.0f, barW, cascade::gui::deckBarH(layout) * s};
            std::vector<Box> parts = {
                {28.0f * s, 39.0f * s, 120.0f * s, 131.0f * s},                      // transport
                {150.0f * s, 50.0f * s, 378.0f * s, 140.0f * s},                     // MASTER, lamps
                {396.0f * s, 20.0f * s, (396.0f + plateW) * s, (20.0f + plateH) * s},  // plate
                {(volCx - 30.0f) * s, 38.0f * s, (volCx + 30.0f) * s, 130.0f * s},   // dial
            };
            {
                // The bias tee key under the lamp row (drawn only for a radio
                // with one - so the banner must keep off where it would be).
                const cascade::gui::FreqRect bk = cascade::gui::deckBiasKeyArea();
                parts.push_back({bk.x0 * s, bk.y0 * s, bk.x1 * s, bk.y1 * s});
            }
            if (meters) {
                const float meterH = 66.0f + 17.0f * 2.0f + 8.0f;
                parts.push_back({cascade::gui::meter1XOnBar(barW), 28.0f,
                                 cascade::gui::meter2XOnBar(barW) + cascade::gui::kMeterW,
                                 28.0f + meterH});
            }
            for (const Measured& m : all) {
                for (int n = 1; n <= 6; ++n) {
                    const MuteBannerLayout l = cascade::gui::layoutMuteBanner(
                        barW, s, coreW, layout.scale >= 2, meters, m.sizes[n], m.nSizes,
                        m.keyPadX, m.gap);
                    ++placed;
                    ++bySlot[l.slot];
                    if (l.lines == 2) { ++twoLine; }
                    if (!l.wordsWhole) { ++shortened; }
                    // THE KEY AS IMGUI WILL LAY IT OUT at the size chosen: its
                    // label measured at that size, plus the padding.
                    float realKeyW = -1.0f;
                    for (int i = 0; i < m.nSizes; ++i) {
                        if (m.sizes[n][i].px == l.px) {
                            realKeyW = m.sizes[n][i].keyLabelW + 2.0f * m.keyPadX;
                        }
                    }
                    CHECK(realKeyW > 0.0f);
                    const Box place{l.x0, l.y0, l.x1, l.y1};
                    const Box key{l.keyX, l.keyY, l.keyX + std::max(l.keyW, realKeyW),
                                  l.keyY + l.keyH};
                    const Box words{l.wordsX, l.wordsY, l.wordsX + l.wordsDrawnW,
                                    l.wordsY + l.wordsPx};
                    const float keyPx = l.px;
                    smallestKeyPx = std::min(smallestKeyPx, keyPx);
                    // Off the middle the layout keeps a pixel clear at each end
                    // of its place, and the key - at its REAL width at the size
                    // chosen - must stand inside that: a key laid out from a
                    // width scaled down from the bar's size lands past it
                    // (the round-2 census caught 0.4 px of exactly that).
                    const Box inner = l.slot == 0 ? place
                                                  : Box{place.x0 + 1.0f, place.y0, place.x1 - 1.0f,
                                                        place.y1};
                    const bool keyWhole = inside(key, inner) && inside(key, bar);
                    const bool keyReadable =
                        keyPx >= kKeyFloorPx - 1.0e-3f && l.keyH >= kKeyFloorPx - 1.0e-3f;
                    // Words drawn are read too: never under the floor, and
                    // never larger than the key they introduce.
                    const bool wordsReadable = l.wordsDrawnW <= 0.0f ||
                                               (l.wordsPx >= kKeyFloorPx - 1.0e-3f &&
                                                l.wordsPx <= l.px + 1.0e-3f);
                    // THE WORDS GIVE WAY, NEVER THE KEY: whatever they say, the
                    // key is as large as it would be with nothing to say at all.
                    // (Ranking the whole sentence above the key's size drew it at
                    // 13 px on Linux's narrower face, four names, 1280 x 720.)
                    cascade::gui::MuteBannerSize bare[cascade::gui::kMuteBannerMaxSizes];
                    for (int i = 0; i < m.nSizes; ++i) {
                        bare[i] = m.sizes[n][i];
                        bare[i].wordsW = 0.0f;
                    }
                    const MuteBannerLayout alone = cascade::gui::layoutMuteBanner(
                        barW, s, coreW, layout.scale >= 2, meters, bare, m.nSizes, m.keyPadX,
                        m.gap);
                    const bool keyFirst = l.slot == 0 || l.px >= alone.px - 1.0e-3f;
                    const bool wordsIn = l.wordsDrawnW <= 0.0f ||
                                         (inside(words, place) && inside(words, bar));
                    bool clear = !(l.wordsDrawnW > 0.0f && hit(key, words));
                    for (const Box& part : parts) {
                        clear = clear && !hit(key, part) && !(l.wordsDrawnW > 0.0f && hit(words, part));
                    }
                    const bool ok =
                        keyWhole && keyReadable && wordsReadable && keyFirst && wordsIn && clear;
                    if (!ok) {
                        if (bad < 12) {
                            std::printf("    %s/%s layout %dx%s bar %.0f, %d name(s): slot %d key "
                                        "(%.1f,%.1f)-(%.1f,%.1f) at %.1f px in place "
                                        "(%.1f,%.1f)-(%.1f,%.1f)%s%s%s%s%s%s\n",
                                        m.theme.c_str(), m.lang.c_str(), layout.scale,
                                        layout.switches ? "+sw" : "", barW, n, l.slot, key.x0,
                                        key.y0, key.x1, key.y1, keyPx, place.x0, place.y0,
                                        place.x1, place.y1, keyWhole ? "" : " KEY CLIPPED",
                                        keyReadable ? "" : " KEY TOO SMALL",
                                        wordsReadable ? "" : " WORDS TOO SMALL/LARGE",
                                        keyFirst ? "" : " KEY SHRUNK FOR THE WORDS",
                                        wordsIn ? "" : " WORDS OUTSIDE",
                                        clear ? "" : " ON A PART");
                        }
                        ++bad;
                    }
                    CHECK(ok);
                }
            }
        }
    }
    std::printf("  %ld placements: %d middle, %d over the meters, %d at the master head; "
                "%d on two lines, %d shortened; smallest key %.1f px; %ld failed\n",
                placed, bySlot[0], bySlot[1], bySlot[2], twoLine, shortened, smallestKeyPx, bad);
    // All three places are exercised, or the sweep proves less than it says.
    CHECK(bySlot[0] > 0 && bySlot[1] > 0 && bySlot[2] > 0);

    // --- today's middle, unchanged ----------------------------------------------------------
    // TODAY AT 1600 x 1000 (a 1552 bar), English, one decoder: the middle, at
    // full size, exactly where 0.99.35 drew it - the words one item spacing
    // past (cluster end + 12, 62) and the key one spacing after them.
    for (const Measured& m : all) {
        if (m.theme != "today" || m.lang != "en") { continue; }
        const CounterLayout today{1, true};
        const MuteBannerLayout l = cascade::gui::layoutMuteBanner(
            1552.0f, 1.0f, cascade::gui::deckCoreW(today), false,
            cascade::gui::metersFitOnBar(1552.0f, cascade::gui::kDeckCoreW), m.sizes[1], m.nSizes,
            m.keyPadX, m.gap);
        CHECK(l.slot == 0);
        CHECK(l.lines == 1);
        CHECK_NEAR(l.px, m.lineH, 1.0e-6f);
        CHECK(l.wordsWhole);
        const float at = cascade::gui::kDeckCoreW + cascade::gui::kMuteBannerEdgeClearance;
        CHECK_NEAR(l.wordsX, at + m.gap, 1.0e-3f);
        CHECK_NEAR(l.wordsY, 62.0f, 1.0e-3f);
        CHECK_NEAR(l.keyX, at + m.gap + m.wordsW[1] + m.gap, 1.0e-3f);
        CHECK_NEAR(l.keyY, 62.0f, 1.0e-3f);
    }

    // --- the reviewer's case, reported: today's first-launch bar, one to four names ---
    for (const Measured& m : all) {
        if (m.theme != "today") { continue; }
        const bool print = m.lang == "en" || m.lang == "pl";
        const CounterLayout today{1, true};
        for (int n = 1; n <= 4; ++n) {
            const MuteBannerLayout l = cascade::gui::layoutMuteBanner(
                cascade::gui::kFirstLaunchBarW, 1.0f, cascade::gui::deckCoreW(today), false,
                cascade::gui::metersFitOnBar(cascade::gui::kFirstLaunchBarW,
                                             cascade::gui::kDeckCoreW),
                m.sizes[n], m.nSizes, m.keyPadX, m.gap);
            if (print) std::printf("  today/%s at 1280 x 720, %d name(s): slot %d, %d line(s), key %.0f px, "
                        "words %.0f px %s, key (%.1f,%.1f) %.1f x %.1f\n",
                        m.lang.c_str(), n, l.slot, l.lines, l.px, l.wordsPx,
                        l.wordsWhole ? "whole" : "shortened", l.keyX, l.keyY, l.keyW, l.keyH);
            // Today's own default window keeps the KEY at the bar's full size
            // whatever the count and the language: the words give way, never
            // the key. (The first cut shared one size, and on Linux's narrower
            // face four English names drew the key at 13 px.)
            CHECK_NEAR(l.px, m.lineH, 1.0e-6f);
        }
    }

    ImGui::DestroyContext();
    return testSummary("test_mute_banner");
}
