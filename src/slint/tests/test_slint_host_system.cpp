// The SYSTEM bank's pure logic (src/slint/system_logic.hpp) against a STUB TABLE and a hand-built interface half: what Display, Language & country,
// Updates, Serial ports (with the GPS row), Key bindings, Diagnostics, Usage reporting, Beta tester and the two report pages show, and the exact
// effect each control has - the setting that is saved (and ONLY that field of the half), the window-side application, the op, the local state, the
// report that is sent - which are the ImGui window's own. No Slint, no engine, no window.
//
// It also READS the ImGui window's sources for the few tables the Slint program has to carry a copy of (the key table, the themes, a handful of
// constants), so a change there that is not followed here fails this test: gui/key_bindings.cpp includes imgui.h and the Slint program links no ImGui.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "system_logic.hpp"
#include "test_check.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "core/config_halves.hpp"
#include "core/countries.hpp"
#include "core/i18n.hpp"
#include "core/pasted_report.hpp"
#include "core/telemetry.hpp"
#include "core/utf8_text.hpp"
#include "core/tester_usage.hpp"

using namespace foxsdr::slintsystem;
namespace core = cascade::core;
using cascade::i18n::tr;

namespace {

// ---- a table of lists and facts a test fills in (the SIGNAL test's) ----
class StubTable : public TableReader {
public:
    std::map<std::uint32_t, std::vector<FoxListItem>> lists;
    std::map<std::string, std::string> facts;

    std::int32_t readList(std::uint32_t list, std::uint32_t index, FoxListItem* out) override {
        const auto it = lists.find(list);
        const std::int32_t n = it == lists.end() ? 0 : static_cast<std::int32_t>(it->second.size());
        if (out != nullptr && static_cast<std::int32_t>(index) < n) { *out = it->second[index]; }
        return n;
    }
    bool fact(const std::string& key, std::string& out) override {
        const auto it = facts.find(key);
        if (it == facts.end()) { out.clear(); return false; }
        out = it->second;
        return true;
    }
    void add(std::uint32_t list, std::uint64_t id, const char* name, const char* detail, std::uint32_t flags = 0, double v0 = 0, double v1 = 0, double v2 = 0, double v3 = 0) {
        FoxListItem it{};
        it.structSize = sizeof(it);
        it.id = id;
        std::snprintf(it.name, sizeof(it.name), "%s", name);
        std::snprintf(it.detail, sizeof(it.detail), "%s", detail);
        it.flags = flags;
        it.value[0] = v0; it.value[1] = v1; it.value[2] = v2; it.value[3] = v3;
        lists[list].push_back(it);
    }
};

bool has(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

template <typename T>
const T& at(const std::vector<T>& v, std::size_t i) {
    static const T none{};
    return i < v.size() ? v[i] : none;
}

FoxReceiverState stateWith(double dbMin, double dbMax) {
    FoxReceiverState s{};
    s.structSize = sizeof(s);
    s.dbMin = dbMin;
    s.dbMax = dbMax;
    return s;
}

// The fields of two halves that differ (config_halves.hpp's table: every AppConfig member that can be compared).
std::vector<std::string> changedFields(const core::AppConfig& a, const core::AppConfig& b) {
    std::vector<std::string> out;
    core::forEachConfigField([&](const char* name, core::ConfigOwner, bool, auto member) {
        if constexpr (requires { a.*member == b.*member; }) {
            if (!(a.*member == b.*member)) { out.push_back(name); }
        }
    });
    return out;
}
using Names = std::vector<std::string>;

std::string slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
// <repo>/src/<rel>, from this file's own path (.../src/slint/tests/test_slint_host_system.cpp).
std::string srcPath(const char* rel) {
    std::string p = __FILE__;
    for (int i = 0; i < 3; ++i) {
        const std::size_t cut = p.find_last_of("/\\");
        if (cut == std::string::npos) { break; }
        p.resize(cut);
    }
    return p + "/" + rel;
}

std::string commandText(const Command& c) { return foxsdr::slintsignal::textOfCommand(c); }
// A command by index that cannot read past the end: a missing one is an invalid, empty Command and the CHECK that wanted it fails (never a crash).
const Command& cmdOf(const Effect& e, std::size_t i) {
    static const Command none{};
    return i < e.commands.size() ? e.commands[i] : none;
}

// A token of each shape (32 and 40 lowercase hex).
const char* kPortal = "0123456789abcdef0123456789abcdef";
const char* kAppTok = "0123456789abcdef0123456789abcdef01234567";

// ---- the tables in the ImGui window's sources ----
void driftChecks() {
    // The key table: every row's id, words and group, every stock chord, every key name and every alias.
    const std::string kb = slurp(srcPath("gui/key_bindings.cpp"));
    CHECK(!kb.empty());
    {
        const std::regex row(R"re(\{KeyAction::(\w+),\s*"(\w+)",\s*FOX_TR_NOOP\("([^"]*)"\),\s*FOX_TR_NOOP\("([^"]*)"\)\})re");
        std::vector<std::vector<std::string>> rows;
        for (std::sregex_iterator it(kb.begin(), kb.end(), row), end; it != end; ++it) { rows.push_back({(*it)[1], (*it)[2], (*it)[3], (*it)[4]}); }
        CHECK(rows.size() == static_cast<std::size_t>(kKeyActionCount));
        std::map<std::string, int> indexOf;
        for (std::size_t i = 0; i < rows.size() && i < static_cast<std::size_t>(kKeyActionCount); ++i) {
            const KeyActionInfo& a = keyAction(static_cast<int>(i));
            CHECK(rows[i][1] == a.id);
            CHECK(rows[i][2] == a.name);
            CHECK(rows[i][3] == a.group);
            indexOf[rows[i][0]] = static_cast<int>(i);
        }
        const std::regex stock(R"re(set\(KeyAction::(\w+),\s*"([^"]*)"\))re");
        int found = 0;
        for (std::sregex_iterator it(kb.begin(), kb.end(), stock), end; it != end; ++it) {
            const auto at = indexOf.find((*it)[1]);
            CHECK(at != indexOf.end());
            if (at == indexOf.end()) { continue; }
            CHECK(std::string(keyAction(at->second).stock) == (*it)[2].str());
            ++found;
        }
        CHECK(found == kKeyActionCount);
    }
    {
        const std::size_t split = kb.find("constexpr KeyRow kKeyAliases");
        CHECK(split != std::string::npos);
        const std::string names = kb.substr(0, split), aliases = kb.substr(split);
        const std::regex pair(R"re(\{ImGuiKey_(\w+),\s*"(\w+)"\})re");
        std::vector<std::string> table;
        std::map<std::string, std::string> canonicalOf;
        for (std::sregex_iterator it(names.begin(), names.end(), pair), end; it != end; ++it) {
            table.push_back((*it)[2]);
            canonicalOf[(*it)[1]] = (*it)[2];
        }
        CHECK(table == chordKeyNames());
        int aliasCount = 0;
        for (std::sregex_iterator it(aliases.begin(), aliases.end(), pair), end; it != end; ++it) {
            std::string canonical;
            CHECK(chordKeyKnown((*it)[2], canonical));
            CHECK(canonical == canonicalOf[(*it)[1]]);
            ++aliasCount;
        }
        CHECK(aliasCount == 10);
    }
    // The themes: key, label, and what picking one brings (pickTheme); a face the application does not draw (vfd, lcd) is drawn plain.
    {
        const std::string th = slurp(srcPath("gui/theme.cpp"));
        const std::regex head(R"re(p\.key = "([\w-]+)";\s*p\.name = FOX_TR_NOOP\("([^"]*)"\);)re");
        const std::regex counter(R"re(p\.counter = PresetCounter\{"(\w+)",\s*([\d.]+)f,\s*(true|false))re");
        const std::regex sizes(R"re(p\.sizes = PresetSizes\{[\d.]+f,\s*([\d.]+)f\})re");
        std::vector<std::vector<std::string>> heads, counters, sz;
        for (std::sregex_iterator it(th.begin(), th.end(), head), end; it != end; ++it) { heads.push_back({(*it)[1], (*it)[2]}); }
        for (std::sregex_iterator it(th.begin(), th.end(), counter), end; it != end; ++it) { counters.push_back({(*it)[1], (*it)[2], (*it)[3]}); }
        for (std::sregex_iterator it(th.begin(), th.end(), sizes), end; it != end; ++it) { sz.push_back({(*it)[1]}); }
        CHECK(heads.size() == 6 && counters.size() == 6 && sz.size() == 6);
        for (std::size_t i = 0; i < 6 && i < heads.size() && i < counters.size() && i < sz.size(); ++i) {
            const ThemePreset& p = themePreset(static_cast<int>(i));
            CHECK(heads[i][0] == p.key);
            CHECK(heads[i][1] == p.label);
            const std::string face = counters[i][0];
            CHECK(std::string(p.face) == ((face == "nixie" || face == "neon") ? face : std::string("plain")));
            CHECK_NEAR(p.counterScale, std::atof(counters[i][1].c_str()), 1e-6);
            CHECK(p.switches == (counters[i][2] == "true"));
            CHECK_NEAR(p.readings, std::atof(sz[i][0].c_str()), 1e-6);
        }
    }
    // The constants the Slint program copies because their header includes imgui.h, and the ones in app_window.cpp.
    {
        const std::string ic = slurp(srcPath("gui/aircraft_icons.hpp"));
        CHECK(has(ic, "kAircraftIconMinPx = 16;") && has(ic, "kAircraftIconMaxPx = 96;") && has(ic, "kAircraftIconDefaultPx = 48;"));
        CHECK(has(ic, "kTrailWidthMinPx = 1;") && has(ic, "kTrailWidthDefaultPx = 4;") && has(ic, "kAircraftSpanFraction = 114.0f / 128.0f;"));
        const std::string aw = slurp(srcPath("gui/app_window.cpp"));
        CHECK(has(aw, "constexpr float kEnlargedReadings = 1.4f;"));
        CHECK(has(aw, R"({"small", "medium", "large"})") && has(aw, R"({"classic", "vivid", "mono"})"));
        CHECK(has(aw, R"(FOX_TR_NOOP("Small"), FOX_TR_NOOP("Medium"),)") && has(aw, R"(FOX_TR_NOOP("Large")})"));
        CHECK(has(aw, R"(FOX_TR_NOOP("Classic"), FOX_TR_NOOP("Vivid"),)") && has(aw, R"(FOX_TR_NOOP("Mono")})"));
        const std::string us = slurp(srcPath("gui/ui_scale.hpp"));
        CHECK(has(us, "kSteps[] = {100, 125, 150, 175, 200, 250};"));
    }
}

// ---- Display ----
core::AppConfig displayConfig() {
    core::AppConfig c;
    c.interfaceScale = "150";
    c.uiTheme = "night";
    c.tunerDisplayStyle = "neon";
    c.counterScale = 2;
    c.counterSwitches = false;
    c.readingsScale = 1.4f;
    c.aircraftIconPx = 64;
    c.mapTrailWidthPx = 9;
    c.bandPlanOverlay = true;
    c.bandPlanSize = "large";
    c.bandPlanPalette = "mono";
    c.waterfallRangeMode = "manual";
    return c;
}
StubTable plansTable() {
    StubTable t;
    t.add(FOXAPI_LIST_BAND_PLANS, 0, "World (global allocations)", "world");
    t.add(FOXAPI_LIST_BAND_PLANS, 0, "ITU Region 1", "itu-region1", FOXAPI_ITEM_ACTIVE);
    t.add(FOXAPI_LIST_BAND_PLANS, 0, "United Kingdom", "uk");
    t.add(FOXAPI_LIST_BAND_PLAN, 0, "2 m", "amateur", 0, 144e6, 146e6);
    t.add(FOXAPI_LIST_BAND_PLAN, 0, "70 cm", "amateur", 0, 430e6, 440e6);
    t.add(FOXAPI_LIST_BAND_PLAN, 0, "Airband", "aeronautical", 0, 118e6, 137e6);
    t.facts["fact.bandPlan.name"] = "ITU Region 1";
    return t;
}

void displayReads() {
    StubTable t = plansTable();
    ReadOptions o;
    o.monitor_percent = 125;
    const core::AppConfig c = displayConfig();
    const DisplayModel m = readDisplay(t, c, stateWith(-120.0, -10.0), o);
    CHECK(m.scales.size() == 7 && at(m.scales, 0).label == "Auto (follows Windows, 125% here)" && at(m.scales, 1).label == "100%" && at(m.scales, 6).label == "250%");
    CHECK(m.scale_index == 3 && at(m.scales, 3).label == "150%" && at(m.scales, 3).id == "150" && at(m.scales, 0).id == "auto");
    CHECK(m.themes.size() == 6 && at(m.themes, 0).label == "Today's bench" && at(m.themes, 1).label == "Bench Classic XL" && at(m.themes, 2).label == "Night Watch");
    CHECK(at(m.themes, 3).label == "Glass Cockpit" && at(m.themes, 4).label == "Daylight Lab" && at(m.themes, 5).label == "Field Radio");
    CHECK(m.theme_index == 2 && at(m.themes, 2).id == "night" && at(m.themes, 1).id == "classic-xl");
    CHECK(m.db_min == -120.f && m.db_min_text == "-120" && m.db_max == -10.f && m.db_max_text == "-10" && m.range_manual);
    CHECK(m.freq_displays.size() == 3 && at(m.freq_displays, 0).label == "Nixie tubes" && at(m.freq_displays, 1).label == "Neon" && at(m.freq_displays, 2).label == "Plain");
    CHECK(m.freq_display_index == 1 && at(m.freq_displays, 1).id == "neon");
    CHECK(m.figures.size() == 2 && at(m.figures, 0).label == "Normal size" && at(m.figures, 1).label == "Enlarged figures" && m.figures_index == 1);
    CHECK(!m.tuner_switches && m.enlarge_readings);
    CHECK(m.icon_px == 64.f && m.icon_text == "64 px" && m.trail_px == 9.f && m.trail_text == "9 px" && m.trail_max == 57.f && m.trail_min == 1.f);
    CHECK(m.band_plan && m.band_options_shown);
    CHECK(m.band_sizes.size() == 3 && at(m.band_sizes, 0).label == "Small" && at(m.band_sizes, 2).label == "Large" && m.band_size_index == 2);
    CHECK(m.band_colours.size() == 3 && at(m.band_colours, 0).label == "Classic" && at(m.band_colours, 1).label == "Vivid" && at(m.band_colours, 2).label == "Mono" && m.band_colour_index == 2);
    CHECK(m.regions_shown && m.regions.size() == 3 && m.region_index == 1 && at(m.regions, 1).label == "ITU Region 1" && at(m.regions, 1).id == "itu-region1" && m.region_preview == "ITU Region 1");
    CHECK(m.plan_status == "ITU Region 1 (3 bands)" && m.plan_status_tone == Tone::Neutral);
    CHECK(has(m.scale_tooltip, "Makes the whole bench bigger") && has(m.scale_tooltip, "Ctrl+0 returns to Auto."));
    CHECK(m.switches_tooltip == "The UP and DN switches under each figure. The wheel over a figure still tunes it without them.");
    CHECK(has(m.icon_tooltip, "Standard is 48 px.") && has(m.trail_tooltip, "(57 px at the") && has(m.trail_tooltip, "Standard is 4 px."));
    // the row's chip: PLAN while the overlay is drawing (on, no load error, a plan with bands)
    const SystemModels all = readSystem(t, c, stateWith(-120.0, -10.0), o);
    CHECK(all.chips[kRowDisplay].word == "PLAN" && all.chips[kRowDisplay].lit);

    // a load error is the status line, in the alarm tone, and the lamp goes out
    StubTable bad = plansTable();
    bad.facts["fact.status.bandPlanError"] = "The band plan file could not be read.";
    const DisplayModel mb = readDisplay(bad, c, stateWith(-110, 0), o);
    CHECK(mb.plan_status == "The band plan file could not be read." && mb.plan_status_tone == Tone::Err);
    const SystemModels allBad = readSystem(bad, c, stateWith(-110, 0), o);
    CHECK(allBad.chips[kRowDisplay].word == "PLAN" && !allBad.chips[kRowDisplay].lit);
    // no rows: "no band plan installed"
    StubTable none;
    const DisplayModel mn = readDisplay(none, c, stateWith(-110, 0), o);
    CHECK(mn.plan_status == "no band plan installed" && mn.regions.empty() && !mn.regions_shown && mn.region_index == -1 && mn.region_preview == "(all installed)");
    CHECK(!readSystem(none, c, stateWith(-110, 0), o).chips[kRowDisplay].lit);
    // a single plan has no decision to offer; the named plan is translated part by part
    StubTable one;
    one.add(FOXAPI_LIST_BAND_PLANS, 0, "ITU Region 1", "itu-region1", FOXAPI_ITEM_ACTIVE);
    one.add(FOXAPI_LIST_BAND_PLAN, 0, "2 m", "amateur", 0, 144e6, 146e6);
    one.facts["fact.bandPlan.name"] = "ITU Region 1 + United Kingdom";
    const DisplayModel m1 = readDisplay(one, c, stateWith(-110, 0), o);
    CHECK(!m1.regions_shown && m1.plan_status == "ITU Region 1 + United Kingdom (1 bands)");
    CHECK(displayPlanName("ITU Region 1 + United Kingdom") == "ITU Region 1 + United Kingdom" && displayPlanName("Nothing Known") == "Nothing Known");
    // the overlay off: the Size, Colour and Region rows and the status are not drawn, the chip says PLAIN
    core::AppConfig off = c;
    off.bandPlanOverlay = false;
    const DisplayModel mo = readDisplay(t, off, stateWith(-110, 0), o);
    CHECK(!mo.band_plan && !mo.band_options_shown && mo.plan_status.empty());
    CHECK(readSystem(t, off, stateWith(-110, 0), o).chips[kRowDisplay].word == "PLAIN");
    // stored values nothing recognises degrade to the first entry; sizes clamp to the sliders'
    core::AppConfig odd;
    odd.interfaceScale = "133";
    odd.uiTheme = "zzz";
    odd.tunerDisplayStyle = "zzz";
    odd.bandPlanSize = "huge";
    odd.bandPlanPalette = "zzz";
    odd.aircraftIconPx = 500;
    odd.mapTrailWidthPx = 0;
    odd.waterfallRangeMode = "auto";
    const DisplayModel md = readDisplay(none, odd, stateWith(-110, 0), o);
    CHECK(md.scale_index == 0 && md.theme_index == 0 && md.freq_display_index == 0 && md.band_size_index == 0 && md.band_colour_index == 0);
    CHECK(md.icon_px == 96.f && md.trail_px == 1.f && !md.range_manual && md.figures_index == 0 && md.tuner_switches && !md.enlarge_readings);

    // the pure helpers
    CHECK(scaleIndexOf("auto") == 0 && scaleIndexOf("100") == 1 && scaleIndexOf("250") == 6 && scaleIndexOf("") == 0 && scaleIndexOf("99") == 0);
    CHECK(scaleChoiceOf(0) == "auto" && scaleChoiceOf(1) == "100" && scaleChoiceOf(6) == "250" && scaleChoiceOf(7).empty() && scaleChoiceOf(-1).empty());
    CHECK(scaleLabel(0, 150) == "Auto (follows Windows, 150% here)" && scaleLabel(3, 150) == "150%");
    CHECK(themeIndexOf("today") == 0 && themeIndexOf("field") == 5 && themeIndexOf("Night") == 0 && themeIndexOf("") == 0);
    CHECK(std::string(themePreset(1).key) == "classic-xl" && themePreset(1).counterScale == 2.0f && !themePreset(1).switches && themePreset(1).readings == 1.4f);
    CHECK(std::string(themePreset(3).face) == "plain" && std::string(themePreset(2).face) == "neon" && std::string(themePreset(0).face) == "nixie");
    CHECK(std::string(themePreset(-5).key) == "today" && std::string(themePreset(99).key) == "today");
    CHECK(dbText(-110.f) == "-110" && dbText(-0.4f) == "-0" && dbText(5.f) == "5");
    CHECK(wingspanPx(48) == 43 && wingspanPx(96) == 86 && wingspanPx(16) == 14 && wingspanPx(500) == 86 && wingspanPx(1) == 14);
    CHECK(clampIconPx(5) == 16 && clampIconPx(1000) == 96 && clampIconPx(50) == 50 && pxText(48) == "48 px");
    CHECK(std::string(tunerStyleName(0)) == "nixie" && std::string(tunerStyleName(2)) == "plain" && std::string(tunerStyleName(9)) == "plain" && std::string(tunerStyleName(-1)) == "nixie");
    CHECK(tunerStyleIndexOf("neon") == 1 && tunerStyleIndexOf("x") == 0);
    CHECK(std::string(bandPlanSizeKey(1)) == "medium" && bandPlanSizeIndexOf("large") == 2 && bandPlanSizeIndexOf("x") == 0);
    CHECK(std::string(bandPlanPaletteKey(2)) == "mono" && bandPlanPaletteIndexOf("vivid") == 1 && bandPlanPaletteIndexOf("x") == 0);
    const PlanStatus ps = planStatus("", 0, "Whatever");
    CHECK(ps.text == "no band plan installed" && ps.tone == Tone::Neutral);
    CHECK(planStatus("boom", 5, "x").text == "boom" && planStatus("boom", 5, "x").tone == Tone::Err);
    CHECK(planStatus("", 12, "World (global allocations)").text == "World (global allocations) (12 bands)");
}

// ---- Language & country ----
void languageReads() {
    StubTable t;
    ReadOptions o;
    o.system_locale = "de-DE";
    core::AppConfig c;
    const std::vector<cascade::i18n::Language>& langs = cascade::i18n::languages();
    CHECK(langs.size() > 20 && langs[0].code == "en");
    LanguageModel m = readLanguage(t, c, o);
    CHECK(m.languages.size() == langs.size() + 1);
    const cascade::i18n::Language* de = cascade::i18n::findLanguage("de");
    CHECK(de != nullptr);
    const std::string deName = de != nullptr ? de->name : std::string();
    CHECK(!deName.empty() && at(m.languages, 0).id == "auto" && at(m.languages, 0).label == "Automatic (" + deName + ")" && m.language_index == 0);
    CHECK(at(m.languages, 1).id == "en" && at(m.languages, 1).label == langs[0].name && at(m.languages, 1).enabled);
    CHECK(m.language_text == at(m.languages, 0).label);
    CHECK(m.machine_hint.empty() && m.suggest_label.empty() && m.country_hint == "Type to narrow the list" && m.translation_label == "SUGGEST A BETTER TRANSLATION");
    // English is the one name said in the language in force
    ReadOptions oe;
    oe.system_locale = "en-GB";
    CHECK(at(readLanguage(t, c, oe).languages, 0).label == "Automatic (English)");
    // a chosen language: its row, by the code in its canonical spelling; a code no catalogue has is English
    c.language = "de";
    std::size_t deRow = 0;
    for (std::size_t i = 0; i < m.languages.size(); ++i) { if (m.languages[i].id == "de") { deRow = i; } }
    CHECK(deRow > 1 && readLanguage(t, c, o).language_index == static_cast<int>(deRow) && readLanguage(t, c, o).language_text == m.languages[deRow].label);
    c.language = "DE";
    CHECK(readLanguage(t, c, o).language_index == static_cast<int>(deRow));
    c.language = "xx";
    CHECK(readLanguage(t, c, o).language_index == 1);
    c.language = "Auto";
    CHECK(readLanguage(t, c, o).language_index == 0);
    c.language = "";
    CHECK(readLanguage(t, c, o).language_index == 0);

    // a language this machine cannot draw is listed, disabled, in English with the reason
    const cascade::i18n::Language* ja = cascade::i18n::findLanguage("ja");
    CHECK(ja != nullptr);
    cascade::i18n::setDrawablePredicate([](const std::string& code) { return code != "ja"; });
    ReadOptions od = o;
    od.language_reason.push_back({"ja", "no system face for Japanese"});
    const LanguageModel md = readLanguage(t, core::AppConfig{}, od);
    for (const SelectItem& it : md.languages) {
        if (it.id == "ja") {
            CHECK(!it.enabled && it.label == ja->englishName + " - no system face for Japanese");
        } else if (it.id != "auto") {
            CHECK(it.enabled);
        }
    }
    cascade::i18n::setDrawablePredicate(nullptr);
    CHECK(languageLabel("de", true, "") == deName || (de != nullptr && de->machine));
    CHECK(languageLabel("ja", false, "").find(" - ") == std::string::npos);   // no reason: the name stands
    // a machine translation says so, in its own row and in the hint when it is in force
    std::string machineCode;
    for (const cascade::i18n::Language& l : langs) { if (l.machine) { machineCode = l.code; break; } }
    CHECK(!machineCode.empty());
    if (!machineCode.empty()) {
        CHECK(has(languageLabel(machineCode, true, ""), "(machine translation)"));
        const std::string before = cascade::i18n::current();
        cascade::i18n::setLanguage(machineCode);
        const LanguageModel mm = readLanguage(t, core::AppConfig{}, o);
        CHECK(mm.machine_hint == tr("This language was translated by machine and may read oddly.") && !mm.machine_hint.empty());
        cascade::i18n::setLanguage(before);
        CHECK(readLanguage(t, core::AppConfig{}, o).machine_hint.empty());
    }

    // the country list: "Not set" first while the filter is empty, then every country by its shown name
    t.add(FOXAPI_LIST_BAND_PLANS, 0, "ITU Region 1", "itu-region1", FOXAPI_ITEM_ACTIVE);
    LanguageModel mc = readLanguage(t, core::AppConfig{}, o);
    CHECK(mc.countries.size() > 200 && at(mc.countries, 0).label == "Not set" && at(mc.countries, 0).id.empty() && mc.country_index == 0 && mc.country_text == "Not set");
    bool sorted = true;
    for (std::size_t i = 2; i < mc.countries.size(); ++i) { if (core::foldForSearch(at(mc.countries, i - 1).label) > core::foldForSearch(at(mc.countries, i).label)) { sorted = false; } }
    CHECK(sorted);
    c = core::AppConfig{};
    c.country = "GB";
    mc = readLanguage(t, c, o);
    int gbRow = -1;
    for (std::size_t i = 0; i < mc.countries.size(); ++i) { if (mc.countries[i].id == "GB") { gbRow = static_cast<int>(i); } }
    CHECK(gbRow > 0 && mc.country_index == gbRow && !mc.country_text.empty() && mc.country_text == at(mc.countries, static_cast<std::size_t>(gbRow)).label);
    CHECK(mc.plan_line == "Band plan: " + std::string(core::findCountry("GB")->bandPlan));
    // the plan line names the plan as the Region picker does, when the table has it
    StubTable named;
    named.add(FOXAPI_LIST_BAND_PLANS, 0, "United Kingdom", core::findCountry("GB")->bandPlan);
    CHECK(readLanguage(named, c, o).plan_line == "Band plan: United Kingdom");
    CHECK(readLanguage(named, c, o).plan_changed_hint.empty());
    o.plan_changed_note = "Display has since been set to a different band plan.";
    CHECK(readLanguage(named, c, o).plan_changed_hint == "Display has since been set to a different band plan.");
    o.plan_changed_note.clear();
    // the filter: the shown name, the English name or the code; "Not set" only while it is empty; a chosen country the filter hides has no row
    o.country_filter = "brazil";
    mc = readLanguage(t, core::AppConfig{}, o);
    CHECK(mc.countries.size() == 1 && at(mc.countries, 0).id == "BR" && mc.country_filter == "brazil" && mc.country_index == -1);
    o.country_filter = "BR";
    mc = readLanguage(t, core::AppConfig{}, o);
    bool br = false;
    for (const SelectItem& it : mc.countries) { if (it.id == "BR") { br = true; } }
    CHECK(br && mc.countries.size() > 1);
    o.country_filter = "  zzzzzzz ";
    CHECK(readLanguage(t, core::AppConfig{}, o).countries.empty());
    o.country_filter.clear();
    CHECK(countryMatches("", "Brazil", "Brazil", "BR") && countryMatches("raz", "Brazil", "Brazil", "BR") && countryMatches("br", "Zzz", "Yyy", "BR") && !countryMatches("brx", "Brazil", "Brazil", "BR"));
    CHECK(countryMatches("brasil", "Brasil", "Brazil", "BR") && countryMatches("brazil", "Brasil", "Brazil", "BR") && countryMatches("BRAZIL", "Brasil", "Brazil", "BR"));
    CHECK(countryRows("").size() == core::countries().size() && countryRows("brazil").size() == 1);
    // USE <LANGUAGE>: the country's language when the build carries it and it is not already in force; never an automatic switch
    c = core::AppConfig{};
    c.country = "DE";
    mc = readLanguage(t, c, o);
    CHECK(mc.suggest_code == "de" && mc.suggest_label == useLanguageLabel(deName) && has(mc.suggest_label, "USE ") && mc.suggest_label == "USE DEUTSCH");
    c.country = "US";
    CHECK(readLanguage(t, c, o).suggest_label.empty());
    CHECK(useLanguageLabel("Deutsch") == "USE DEUTSCH" && useLanguageLabel("Português (Brasil)") == "USE PORTUGUÊS (BRASIL)");
    CHECK(autoLanguageLabel("de-DE") == "Automatic (" + deName + ")" && autoLanguageLabel("") == "Automatic (English)");
    CHECK(languageLabel("ja", false, "no system face for Japanese") == ja->englishName + " - no system face for Japanese");
    // the chip: the language in force, upper-cased
    CHECK(readSystem(t, core::AppConfig{}, stateWith(-110, 0), o).chips[kRowLanguage].word == "EN");
}

// ---- Updates ----
StubTable noTable;

void updatesReads() {
    ReadOptions o;
    core::AppConfig c;
    o.updates.build_version = "0.99.105";
    // the website copy, ticked, nothing asked yet
    UpdatesModel m = readUpdates(c, o);
    CHECK(m.check_at_startup && m.channel_index == 0 && m.check_label == "Check for updates at startup" && !m.packaged && m.stable_label == "Stable releases" && m.beta_label == "Every release (beta)");
    CHECK(m.channel_help == "Stable is the release the FoxSDR team recommends to everyone; beta is every release the day it is published.");
    CHECK(m.privacy == "Asks foxsdr.com once per launch whether a newer version exists, and sends the version you are running and nothing else - no identifier, no cookie. Nothing is downloaded or installed unless you press the button. This is not the usage report below; the two share nothing.");
    CHECK(m.version_line == "this build: 0.99.105" && m.team_message.empty() && m.status_line.empty() && m.status_tone == Tone::Neutral && !m.show_again);
    CHECK(readSystem(noTable, c, stateWith(-110, 0), o).chips[kRowUpdates].word == "IDLE");
    // checking
    o.updates.started = true;
    o.updates.pending = true;
    m = readUpdates(c, o);
    CHECK(m.status_line == "checking...");
    CHECK(updatesChip(true, o.updates).word == "CHECK");
    // up to date
    o.updates.pending = false;
    m = readUpdates(c, o);
    CHECK(m.status_line == "up to date" && m.status_tone == Tone::Neutral && updatesChip(true, o.updates).word == "OK" && !updatesChip(true, o.updates).lit);
    // a check that did not complete says so, only here
    o.updates.error = "no route";
    m = readUpdates(c, o);
    CHECK(m.status_line == "last check did not complete: no route" && updatesChip(true, o.updates).word == "IDLE");
    o.updates.error.clear();
    // an update: amber; an important one: red; "Show it again" only once dismissed
    o.updates.newer = true;
    o.updates.new_version = "0.99.106";
    m = readUpdates(c, o);
    CHECK(m.status_line == "0.99.106 is available" && m.status_tone == Tone::Warn && !m.show_again && m.show_again_label == "Show it again");
    CHECK(updatesChip(true, o.updates).word == "NEW" && updatesChip(true, o.updates).tone == Tone::Warn && updatesChip(true, o.updates).lit);
    o.updates.critical = true;
    o.updates.dismissed = true;
    m = readUpdates(c, o);
    CHECK(m.status_tone == Tone::Err && m.show_again && updatesChip(true, o.updates).word == "IMPT" && updatesChip(true, o.updates).tone == Tone::Err);
    // the tick off: nobody is asked, and the line says so
    c.updateCheckEnabled = false;
    m = readUpdates(c, o);
    CHECK(!m.check_at_startup && m.status_line == "checking is off, so you will not be told about a new version" && !m.show_again);
    CHECK(updatesChip(false, o.updates).word == "OFF" && !updatesChip(false, o.updates).lit);
    c.updateCheckEnabled = true;
    // the beta channel
    c.updateChannel = "beta";
    CHECK(readUpdates(c, o).channel_index == 1);
    c.updateChannel = "nonsense";
    CHECK(readUpdates(c, o).channel_index == 0);
    // unseen team messages are counted under the version line, for the website copy with the check on
    o.updates = UpdatesFacts{};
    o.updates.build_version = "0.99.105";
    o.updates.unseen_messages = 2;
    CHECK(readUpdates(c, o).team_message == "2 message(s) from the FoxSDR team");
    c.updateCheckEnabled = false;
    CHECK(readUpdates(c, o).team_message.empty());
    c.updateCheckEnabled = true;
    o.updates.unseen_messages = 0;
    CHECK(readUpdates(c, o).team_message.empty());
    // a Microsoft Store copy: its own words, no channel radios, the package line, the Store's offer without a version
    o.updates.packaged = true;
    o.updates.package_name = "FoxSDR_0.99.105_x64__abc";
    o.updates.unseen_messages = 3;
    m = readUpdates(c, o);
    CHECK(m.packaged && m.check_label == "Ask the Microsoft Store for updates at startup" && m.channel_help.empty() && m.team_message.empty());
    CHECK(m.privacy == "This copy came from the Microsoft Store, so FoxSDR asks the Microsoft Store once per launch whether it has a newer version. Nothing is sent to foxsdr.com. Nothing is downloaded or installed unless you press the key and confirm the dialog Windows shows.");
    CHECK(m.version_line == "this build: 0.99.105\npackage: FoxSDR_0.99.105_x64__abc");
    o.updates.store_offered = true;
    o.updates.newer = true;   // (a packaged copy is judged by the Store's answer, not the site's)
    m = readUpdates(c, o);
    CHECK(m.status_line == "the Microsoft Store has an update for this copy" && m.status_tone == Tone::Warn && updatesChip(true, o.updates).word == "NEW");
    o.updates.store_mandatory = true;
    CHECK(readUpdates(c, o).status_tone == Tone::Err && updatesChip(true, o.updates).word == "IMPT");
    o.updates.store_installing = true;
    CHECK(updatesChip(true, o.updates).word == "STORE" && updatesChip(true, o.updates).tone == Tone::Warn);
    // a packaged copy ignores the site's offer
    UpdatesFacts f;
    f.packaged = true;
    f.newer = true;
    CHECK(updatesStatus(true, f).text.empty() && updatesChip(true, f).word == "IDLE");
}

// ---- Serial ports and the GPS row ----
void serialReads() {
    StubTable t;
    ReadOptions o;
    core::AppConfig c;
    // nothing yet: the scan has not answered, so nothing is said; once it has, the empty line is
    o.gps_ports_waiting = true;
    SerialPortsModel m = readSerialPorts(t, c, o);
    CHECK(m.ports.empty() && m.empty_line.empty());
    o.gps_ports_waiting = false;
    m = readSerialPorts(t, c, o);
    CHECK(m.empty_line == "No serial ports found. If a device is plugged in, check Device Manager under \"Ports (COM & LPT)\" - if it is not listed there either, this application cannot see it yet.");
    CHECK(m.intro == "Every serial port this machine has right now - the same ports the GPS row below offers, gathered in one place rather than left findable only from the rail's Radar section.");
    CHECK(m.gps_note == "Sets the receiver's position - what every range and bearing on the map and the radar scope is measured from. The same control also appears on the rail's Radar section (while there is no position), the scope's empty state, and every map page's bar.");
    CHECK(m.refresh_label == "Refresh" && m.column_label == "Port" && m.gps_heading == "GPS receiver" && m.gps_hint == "or read it from a GPS receiver on a serial port:");
    CHECK(m.gps.ports.empty() && m.gps.port_index == -1 && m.gps.baud_index == 1 && m.gps.bauds.size() == 6 && at(m.gps.bauds, 0).label == "4800" && at(m.gps.bauds, 5).label == "115200");
    CHECK(!m.gps.listening && m.gps.key_label == "Read position from GPS" && !m.gps.key_enabled && m.gps.key_tooltip == "Name the port the GPS receiver is on first." && m.gps.status.empty());
    CHECK(readSystem(t, c, stateWith(-110, 0), o).chips[kRowSerial].word == "NONE");
    // the ports, a saved port the machine does not list, the saved baud
    t.add(FOXAPI_LIST_SERIAL_PORTS, 0, "COM3", "");
    t.add(FOXAPI_LIST_SERIAL_PORTS, 0, "COM10", "");
    c.gpsPort = "COM10";
    c.gpsBaud = 4800;
    m = readSerialPorts(t, c, o);
    CHECK(m.ports == (std::vector<std::string>{"COM3", "COM10"}) && m.gps.ports.size() == 2 && m.gps.port_index == 1 && m.gps.baud_index == 0 && m.gps.key_enabled && m.empty_line.empty());
    CHECK(has(m.gps.key_tooltip, "Opens the port, waits up to 60 s for a fix,") && has(m.gps.key_tooltip, "never written to the diagnostic log."));
    CHECK(m.gps.port_tooltip == "The serial port the GPS receiver is on - COM3, or /dev/ttyUSB0.\nPress the arrow to pick from the ports this machine has.");
    CHECK(m.gps.baud_tooltip == "The receiver's serial rate. Nearly everything ships at 9600;\nold pucks use 4800. Wrong, and the status line will say\nnothing readable arrived.");
    CHECK(readSystem(t, c, stateWith(-110, 0), o).chips[kRowSerial].word == "2 PORTS");
    c.gpsPort = "COM7";
    m = readSerialPorts(t, c, o);
    CHECK(m.gps.ports.size() == 3 && at(m.gps.ports, 2).id == "COM7" && m.gps.port_index == 2 && m.ports.size() == 2);
    // one port is "1 PORT"
    StubTable one;
    one.add(FOXAPI_LIST_SERIAL_PORTS, 0, "COM3", "");
    CHECK(readSystem(one, core::AppConfig{}, stateWith(-110, 0), o).chips[kRowSerial].word == "1 PORT");
    CHECK(serialPortsChip(0) == "NONE" && serialPortsChip(1) == "1 PORT" && serialPortsChip(12) == "12 PORTS");
    // the reader: listening disables the fields and turns the key to Stop; the engine's sentence rides as it is; satellites in view
    t.add(FOXAPI_LIST_GPS, 0, "COM10", "Listening on COM10: 3 s, 5 satellites, no fix yet", FOXAPI_ITEM_ACTIVE, 1, 3.0, 5, 4800);
    c.gpsPort = "COM10";
    m = readSerialPorts(t, c, o);
    CHECK(m.gps.listening && m.gps.key_label == "Stop" && m.gps.key_enabled && m.gps.status == "Listening on COM10: 3 s, 5 satellites, no fix yet" && m.gps.satellites == "5" && m.gps.status_tone == Tone::Neutral && m.gps.state == 1);
    CHECK(readSystem(t, c, stateWith(-110, 0), o).chips[kRowSerial].lit);
    // a failed read is the warning ink, and a refusal the engine kept rides beside it
    t.lists[FOXAPI_LIST_GPS].clear();
    t.add(FOXAPI_LIST_GPS, 0, "COM10", "COM10 could not be opened", FOXAPI_ITEM_FAULT, 3, 1.0, -1, 9600);
    t.facts["fact.status.gpsRefusal"] = "That position is not acceptable.";
    m = readSerialPorts(t, c, o);
    CHECK(!m.gps.listening && m.gps.status_tone == Tone::Warn && m.gps.status == "COM10 could not be opened" && m.gps.satellites.empty() && m.gps.refusal == "That position is not acceptable." && m.gps.state == 3);
    CHECK(gpsKeyLabel(true) == "Stop" && gpsKeyLabel(false) == "Read position from GPS");
    CHECK(cleanPortName("  COM3  ") == "COM3" && cleanPortName("CO\x01M3") == "COM3" && cleanPortName(std::string(100, 'A')).size() == 64 && cleanPortName("").empty());
}

// ---- Key bindings ----
void keyReads() {
    // the host copy: 32 actions, the stock chords the ImGui table ships
    CHECK(kKeyActionCount == 32 && std::string(keyAction(0).id) == "startStop" && std::string(keyAction(31).id) == "openSettings" && std::string(keyAction(24).id) == "screenshot");
    CHECK(keyActionIndexOf("mute") == 1 && keyActionIndexOf("bankView") == 28 && keyActionIndexOf("bankTools") == -1 && keyActionIndexOf("") == -1);
    const KeyTable def = defaultKeyTable();
    CHECK(formatChord(def[0]) == "Ctrl+F2" && formatChord(def[1]) == "M" && formatChord(def[4]) == "Ctrl+Shift+PageUp" && formatChord(def[22]) == "Shift+R" && formatChord(def[23]) == "Space");
    CHECK(clashCount(def) == 0 && keyTableToConfig(def).empty());
    // chords: the text form is fixed (Ctrl, Shift, Alt, then the key); parsing is tolerant of case and spaces and strict about everything else
    Chord c;
    CHECK(parseChord("ctrl + shift + pageup", c) && c.ctrl && c.shift && !c.alt && c.key == "PageUp" && formatChord(c) == "Ctrl+Shift+PageUp");
    CHECK(parseChord("Alt+Ctrl+Esc", c) && formatChord(c) == "Ctrl+Alt+Escape");
    CHECK(parseChord("Equal", c) && c.key == "Plus" && parseChord("Return", c) && c.key == "Enter" && parseChord("Del", c) && c.key == "Delete" && parseChord("PgDn", c) && c.key == "PageDown");
    CHECK(parseChord("None", c) && !c.bound() && formatChord(c) == "None" && parseChord("", c) && !c.bound() && parseChord("  none ", c) && !c.bound());
    CHECK(!parseChord("Ctrl+", c) && !parseChord("Ctrl+A+B", c) && !parseChord("Ctrl+Ctrl+A", c) && !parseChord("Banana", c) && !parseChord("Ctrl+Banana", c) && !parseChord("+A", c));
    CHECK(formatChord(Chord{"", true, true, true}) == "None");   // modifiers with no key are no chord
    // every key name survives being written and read back
    for (const std::string& name : chordKeyNames()) {
        Chord a;
        a.key = name;
        a.ctrl = true;
        Chord b;
        CHECK(parseChord(formatChord(a), b) && b == a);
    }
    // the config form: only what differs from the stock table; a bad line is dropped on its own; the first line for an action wins
    KeyTable t = def;
    t[1] = Chord{"M", true, false, false};
    t[2] = Chord{};
    CHECK(keyTableToConfig(t) == (std::vector<std::string>{"mute=Ctrl+M", "volumeUp=None"}));
    const KeyTable back = keyTableFromConfig({"mute=Ctrl+M", "volumeUp=None", "mute=Ctrl+Z", "volumeDown=Banana", "nonsense=A", "noequals", " squelchUp = Ctrl + Q "});
    CHECK(formatChord(back[1]) == "Ctrl+M" && !back[2].bound() && formatChord(back[3]) == "Minus" && formatChord(back[4]) == "Ctrl+Q");
    CHECK(keyTableFromConfig({}) == def);
    // the read: seven groups (Picture comes twice, Transmit sits between), thirty-two rows, the stored chords
    core::AppConfig cfg;
    ReadOptions o;
    KeyBindingsModel m = readKeyBindings(cfg, o);
    CHECK(m.groups.size() == 7 && at(m.groups, 0).heading == "Receiver" && at(m.groups, 1).heading == "Mode" && at(m.groups, 2).heading == "Tuning" && at(m.groups, 3).heading == "Picture");
    CHECK(at(m.groups, 4).heading == "Transmit" && at(m.groups, 5).heading == "Picture" && at(m.groups, 6).heading == "Rail");
    std::size_t rows = 0;
    for (const KeyGroup& g : m.groups) { rows += g.rows.size(); }
    CHECK(rows == 32 && at(at(m.groups, 0).rows, 0).id == "startStop" && at(at(m.groups, 0).rows, 0).action == "Start / stop the receiver" && at(at(m.groups, 0).rows, 0).chord == "Ctrl+F2");
    CHECK(at(at(m.groups, 4).rows, 0).id == "transmitPtt" && at(at(m.groups, 4).rows, 0).chord == "Space" && at(at(m.groups, 6).rows, 5).id == "openSettings" && at(at(m.groups, 6).rows, 5).chord == "F7");
    CHECK(!m.changed && m.changed_count == 0 && m.action_count == 32 && m.clash_count == 0 && m.prompt == "press a key" && m.reset_label == "Reset to defaults");
    CHECK(m.help == "Every shortcut the receiver answers. Click a key to change it, then press the keys you want - Esc leaves it alone, Backspace clears it. Shortcuts never fire while you are typing in a field.");
    CHECK(m.footer == "F12 always saves a screenshot, whatever the Screenshot key is set to.\nEsc always cancels what is being typed or the full-screen scope.");
    CHECK(readSystem(noTable, cfg, stateWith(-110, 0), o).chips[kRowKeys].word == "STOCK");
    // two actions on one chord: shown, naming each other, never refused
    cfg.keyBindings = {"mute=Ctrl+M", "volumeUp=Ctrl+M"};
    o.capturing_id = "volumeDown";
    m = readKeyBindings(cfg, o);
    const KeyRow& mute = at(at(m.groups, 0).rows, 1);
    const KeyRow& up = at(at(m.groups, 0).rows, 2);
    const KeyRow& down = at(at(m.groups, 0).rows, 3);
    CHECK(mute.chord == "Ctrl+M" && mute.clash == "also Volume up" && up.chord == "Ctrl+M" && up.clash == "also Mute the sound" && down.clash.empty());
    CHECK(down.capturing && !mute.capturing && m.changed && m.changed_count == 2 && m.clash_count == 2);
    CHECK(readSystem(noTable, cfg, stateWith(-110, 0), o).chips[kRowKeys].word == "2 SET" && readSystem(noTable, cfg, stateWith(-110, 0), o).chips[kRowKeys].lit);
    KeyTable cl = defaultKeyTable();
    cl[1] = cl[2] = Chord{"M", true, false, false};
    CHECK(clashOf(cl, 1) == 2 && clashOf(cl, 2) == 1 && clashOf(cl, 3) == -1 && clashCount(cl) == 2);
    cl[1] = cl[2] = Chord{};
    CHECK(clashOf(cl, 1) == -1 && clashCount(cl) == 0);   // unbound actions never clash with each other
    // the first in enum order is the one named
    cl[5] = cl[6] = cl[7] = Chord{"X", false, false, false};
    CHECK(clashOf(cl, 7) == 5 && clashOf(cl, 5) == 6 && clashCount(cl) == 3);
    CHECK(keyBindingsChip(0) == "STOCK" && keyBindingsChip(3) == "3 SET");
}

// ---- Diagnostics, Usage reporting, Beta tester ----
void diagnosticsUsageTesterReads() {
    ReadOptions o;
    core::AppConfig c;
    c.diagnosticsEnabled = true;
    c.diagnosticsMinidump = false;
    DiagnosticsModel m = readDiagnostics(c, o);
    CHECK(m.explanation == "If FoxSDR crashes or freezes, it writes a report on THIS machine. With this switch on, that report is also sent to us the NEXT time you open FoxSDR - never from inside the crash itself, because a program that has just failed cannot safely use the network.");
    CHECK(m.record && !m.minidump && !m.what_open && m.record_label == "Record crashes and freezes, and send the report next time" && m.minidump_label == "Also write a full memory dump beside a crash report" && m.what_label == "What exactly is in a report?");
    CHECK(m.log_path == "Log: off (nothing is being written)." && m.reports_path == "Reports: (unavailable)" && m.offer.empty());
    CHECK(has(m.sent_text, "Sent: the version and build, what failed and where (as a file name and\n") && has(m.sent_text, "Off means off: no report is written and nothing is sent."));
    CHECK(has(m.dump_note, "A memory dump can contain file names, window titles and received signal\n") && has(m.dump_note, "setting - if it is ever useful you will be asked for it, and you decide."));
    CHECK(has(m.what_text, "version  commit  os  arch  mode  source  sample rate\n") && has(m.what_text, "See PRIVACY.md for the complete list and what is excluded."));
    o.log_path = "C:\\Users\\a\\log.txt";
    o.reports_path = "C:\\Users\\a\\reports";
    o.what_open = true;
    m = readDiagnostics(c, o);
    CHECK(m.log_path == "Log: C:\\Users\\a\\log.txt" && m.reports_path == "Reports: C:\\Users\\a\\reports" && m.what_open);
    // the offer after an unclean exit: only with diagnostics on
    o.last_run_unclean = true;
    CHECK(has(readDiagnostics(c, o).offer, "The previous session ended without shutting down.") && has(readDiagnostics(c, o).offer, "No memory dump is sent, ever."));
    c.diagnosticsEnabled = false;
    c.diagnosticsMinidump = true;
    m = readDiagnostics(c, o);
    CHECK(!m.record && m.minidump && m.offer.empty());
    CHECK(diagnosticsChip(false, true) == "OFF" && diagnosticsChip(true, false) == "ON" && diagnosticsChip(true, true) == "ON+DMP");
    StubTable none;
    CHECK(readSystem(none, c, stateWith(-110, 0), o).chips[kRowDiagnostics].word == "OFF" && !readSystem(none, c, stateWith(-110, 0), o).chips[kRowDiagnostics].lit);
    c.diagnosticsEnabled = true;
    CHECK(readSystem(none, c, stateWith(-110, 0), o).chips[kRowDiagnostics].word == "ON+DMP" && readSystem(none, c, stateWith(-110, 0), o).chips[kRowDiagnostics].lit);

    // usage reporting: the switch and the id are the engine's facts; what the box has just asked for shows until the fact says it
    StubTable t;
    ReadOptions ou;
    UsageModel u = readUsage(t, ou);
    CHECK(!u.send && u.install_id_line.empty() && u.state_line == "Off. Nothing is transmitted.");
    CHECK(has(u.disclosure, "Anonymous counts only: version, operating system, how long sessions run,") && has(u.disclosure, "no IP address is recorded."));
    CHECK(u.send_label == "Send anonymous usage reports" && u.what_label == "What exactly is sent?" && u.privacy_label == "Privacy policy (foxsdr.com)" && u.privacy_url == core::kPrivacyPolicyUrl);
    t.facts["fact.telemetry.enabled"] = "1";
    t.facts["fact.telemetry.installId"] = "abcd1234";
    u = readUsage(t, ou);
    CHECK(u.send && u.install_id_line == "Install id: abcd1234" && u.state_line == "One report at start-up, describing the previous session - and, while FoxSDR is open, a \"still running\" beat every five minutes. This switch stops both.");
    CHECK(u.payload.size() == 4 && at(u.payload, 0) == "Each launch: id (random), version, os, arch, launches, crashes, display stalls, failure counts, session seconds, sdr model, modes used, panels, plugins.");
    CHECK(at(u.payload, 1) == "The crash count is also split by how each unclean exit ended: crashed, ended from outside, ended by the system, or not known. Four more counts, nothing else.");
    CHECK(at(u.payload, 2) == "Every five minutes while open: id (the same one), version, a beat marker." && at(u.payload, 3) == "See PRIVACY.md for the complete list and what is excluded.");
    CHECK(u.what_text == at(u.payload, 0) + "\n" + at(u.payload, 1) + "\n" + at(u.payload, 2) + "\n" + at(u.payload, 3) && !u.what_open);
    ou.telemetry_pending = true;
    ou.telemetry_pending_on = false;
    u = readUsage(t, ou);
    CHECK(!u.send && u.install_id_line == "Install id: abcd1234");   // the box shows what was asked; the lines follow the fact
    ou.what_open = true;
    CHECK(readUsage(t, ou).what_open);
    ou = ReadOptions{};
    CHECK(readSystem(t, core::AppConfig{}, stateWith(-110, 0), ou).chips[kRowUsage].word == "ON" && readSystem(t, core::AppConfig{}, stateWith(-110, 0), ou).chips[kRowUsage].lit);
    CHECK(readSystem(none, core::AppConfig{}, stateWith(-110, 0), ou).chips[kRowUsage].word == "OFF");

    // the beta tester: shown only with a code set (or a link in progress); the code only ever masked
    core::AppConfig tc;
    ReadOptions ot;
    TesterModel m0 = readTester(tc, ot);
    CHECK(!m0.shown && m0.field_shown && m0.use_label == "Use this code" && m0.sent_preview == "Nothing - no code is set.");
    CHECK(m0.explanation == "Paste the code from your tester link (or the whole link) to report, per session, which features, plugins and radios you actually used - linked to your own entry on the tester list so it can be marked as tested. Pasting a code IS the opt-in; with none, nothing here is collected or sent.");
    CHECK(m0.sent_text == "What is sent: the app version, platform and architecture, when a session started and how long it ran, which of the features above were used, which plugins ran and for how long, and which radio kinds were opened. Never a frequency, never anything decoded, never your location, and no IP address is recorded.");
    CHECK(m0.field_label == "Tester code" && m0.unlink_label == "Unlink" && m0.remove_label == "Remove code" && m0.show_label == "Show what is sent" && m0.hide_label == "Hide what is sent");
    CHECK(testerChip(false, false) == "OFF" && testerChip(true, false) == "ON" && testerChip(true, true) == "INVALID");
    ot.tester_revealed = true;
    CHECK(readTester(tc, ot).shown);
    ot.tester_revealed = false;
    tc.testerToken = kPortal;
    TesterModel m1 = readTester(tc, ot);
    CHECK(m1.shown && m1.code_line == "Code: 0123...cdef" && m1.linked_line.empty() && m1.invalid_line.empty() && !m1.field_shown);
    CHECK(!has(m1.code_line, "456789abcdef0123") && m1.sent_preview.empty());
    tc.testerTokenInvalid = true;
    m1 = readTester(tc, ot);
    CHECK(m1.invalid_line == "This code is no longer valid, so sending has stopped. The code above is kept so you can fix it - paste a current one below." && m1.field_shown && m1.shown);
    CHECK(readSystem(none, tc, stateWith(-110, 0), ot).chips[kRowTester].word == "INVALID" && readSystem(none, tc, stateWith(-110, 0), ot).chips[kRowTester].tone == Tone::Err);
    tc.testerTokenInvalid = false;
    tc.testerAppToken = kAppTok;
    tc.testerAppTokenName = "Alex";
    TesterModel m2 = readTester(tc, ot);
    CHECK(m2.linked_line == "Linked to Alex" && m2.code_line.empty() && !m2.field_shown && m2.shown);
    tc.testerAppTokenName.clear();
    CHECK(readTester(tc, ot).linked_line == "Linked to (no name set)" && testerNameOrPlaceholder("") == "(no name set)" && testerNameOrPlaceholder("Sam") == "Sam");
    CHECK(readSystem(none, tc, stateWith(-110, 0), ot).chips[kRowTester].word == "ON" && readSystem(none, tc, stateWith(-110, 0), ot).chips[kRowTester].lit);
    // the draft, the error line, the preview (the masked JSON the publisher built) and the queue
    ot.tester_draft = "pasted";
    ot.tester_error = "that does not look like a tester code or link";
    ot.tester_sent_open = true;
    ot.tester_preview_json = "{\"token\":\"0123...cdef\"}";
    ot.tester_queued = 2;
    tc = core::AppConfig{};
    tc.testerToken = kPortal;
    tc.testerTokenInvalid = true;
    TesterModel m3 = readTester(tc, ot);
    CHECK(m3.code_draft == "pasted" && m3.error_line == "that does not look like a tester code or link" && m3.sent_open && m3.sent_preview == "{\"token\":\"0123...cdef\"}\n2 report(s) queued to retry");
    ot.tester_queued = 0;
    CHECK(readTester(tc, ot).sent_preview == "{\"token\":\"0123...cdef\"}");
    // the code box is for no app token and (no token or an invalid one)
    tc.testerTokenInvalid = false;
    CHECK(!readTester(tc, ot).field_shown);
    // classifying what was pasted
    std::string tok;
    CHECK(classifyTesterCode(std::string("  ") + kAppTok + " \n", tok) == TesterKind::AppToken && tok == kAppTok);
    CHECK(classifyTesterCode(kPortal, tok) == TesterKind::PortalToken && tok == kPortal);
    CHECK(classifyTesterCode(std::string("https://foxsdr.com/#t=") + kPortal, tok) == TesterKind::PortalToken && tok == kPortal);
    CHECK(classifyTesterCode("hello", tok) == TesterKind::None && tok.empty());
    CHECK(classifyTesterCode(std::string("https://foxsdr.com/#c=") + kPortal, tok) == TesterKind::None);
    CHECK(classifyTesterCode("", tok) == TesterKind::None);
}

// ---- the report pages ----
void reportReads() {
    // the verdicts: the page's own words, in its own order
    SenderView idle;
    CHECK(!featureVerdict("short", "", idle, 100).can_send && featureVerdict("short", "", idle, 100).reason == tr(core::validateFeatureRequestText("short").c_str()));
    CHECK(featureVerdict("a perfectly good request", "", idle, 100).can_send && featureVerdict("a perfectly good request", "", idle, 100).reason.empty());
    CHECK(featureVerdict("a perfectly good request", std::string(500, 'x'), idle, 100).reason == tr(core::validateFeatureRequestContact(std::string(500, 'x')).c_str()) && !core::validateFeatureRequestContact(std::string(500, 'x')).empty());
    SenderView sending;
    sending.state = SendState::Sending;
    CHECK(featureVerdict("a perfectly good request", "", sending, 100).reason == "sending" && !featureVerdict("a perfectly good request", "", sending, 100).can_send);
    SenderView cooling;
    cooling.state = SendState::Sent;
    cooling.blocked_until = 130;
    CHECK(featureVerdict("a perfectly good request", "", cooling, 100).reason == "wait 30s before sending another" && !featureVerdict("a perfectly good request", "", cooling, 100).can_send);
    CHECK(featureVerdict("a perfectly good request", "", cooling, 130).can_send && featureVerdict("a perfectly good request", "", cooling, 200).can_send);
    // a bug report names its kind first
    CHECK(!problemVerdict("", "something went wrong here", "", idle, 1).can_send && problemVerdict("", "something went wrong here", "", idle, 1).reason == tr(core::validateProblemReportKind("").c_str()));
    CHECK(problemVerdict("bug", "something went wrong here", "", idle, 1).can_send && problemVerdict("dislike", "something went wrong here", "", idle, 1).can_send);
    CHECK(problemVerdict("bug", "x", "", idle, 1).reason == tr(core::validateProblemReportText("x").c_str()));
    CHECK(!problemVerdict("nonsense", "something went wrong here", "", idle, 1).can_send);
    // the sentences, by contact and by log
    CHECK(featureSentence(false) == "Sends your message above, the FoxSDR version, and whether this is running on Windows, Linux or Android, x64 or arm64. Nothing else - no identifier, no log, no settings, no frequency.");
    CHECK(featureSentence(true) == "Sends your message above, the contact line below it, the FoxSDR version, and whether this is running on Windows, Linux or Android, x64 or arm64. Nothing else - no identifier, no log, no settings, no frequency.");
    CHECK(problemSentence(false, false) == "Sends whether this is a bug or a dislike, your message above, the FoxSDR version, and whether this is running on Windows, Linux or Android, x64 or arm64. Nothing else - no identifier, no log, no crash report, no settings, no frequency.");
    CHECK(problemSentence(false, true) == "Sends whether this is a bug or a dislike, your message above, the contact line below it, the FoxSDR version, and whether this is running on Windows, Linux or Android, x64 or arm64. Nothing else - no identifier, no log, no crash report, no settings, no frequency.");
    CHECK(has(problemSentence(true, false), "and the diagnostics log attached above - untick \"Attach the diagnostics log\" to leave it out") && !has(problemSentence(true, false), "the contact line"));
    CHECK(has(problemSentence(true, true), "the contact line below it") && has(problemSentence(true, true), "No identifier, no crash report."));
    // the status line
    Tone tone = Tone::Neutral;
    SenderView s;
    CHECK(senderStatusLine(s, true, tone).empty());
    s.state = SendState::Sending;
    CHECK(senderStatusLine(s, true, tone) == "Sending..." && tone == Tone::Warn);
    s.state = SendState::Sent;
    CHECK(senderStatusLine(s, true, tone) == "Thank you - your request was sent." && tone == Tone::Good && senderStatusLine(s, false, tone) == "Thank you - your report was sent.");
    s.diagnostics_dropped = true;
    CHECK(senderStatusLine(s, false, tone) == "Thank you - your report was sent.\nThe diagnostics log could not be sent with this report." && senderStatusLine(s, true, tone) == "Thank you - your request was sent.");
    s = SenderView{};
    s.state = SendState::Failed;
    s.failure = "The server said no.";
    CHECK(senderStatusLine(s, true, tone) == "The server said no." && tone == Tone::Err);
    s.state = SendState::CoolingDown;
    CHECK(senderStatusLine(s, true, tone) == "The server said no." && tone == Tone::Warn);
    s.failure.clear();
    CHECK(senderStatusLine(s, true, tone) == "The server asked us to wait before sending another request." && senderStatusLine(s, false, tone) == "The server asked us to wait before sending another report.");
    // the evidence lines of the attachment
    const std::string diag = "version 1\nrecovered: nothing\nlast-run-unclean: yes\nrecent-faults: 3 ids\nmode: AM\n";
    CHECK(evidenceLines(diag) == (std::vector<std::string>{"last-run-unclean: yes", "recent-faults: 3 ids", "recovered: nothing"}));
    CHECK(evidenceLines("nothing here").empty() && evidenceLines("recovered: first line has no newline before it").empty());

    // the feature page
    ReportInputs in;
    in.shown = true;
    in.now_epoch = 1000;
    ReportPageModel f = readFeatureRequest(in);
    CHECK(f.shown && f.kind == 0 && f.title == "FEATURE REQUEST" && f.prompt == "What would you like FoxSDR to do?" && f.counter == "0 / 100000 characters" && f.contact_label == "Email or callsign (optional)" && f.send_label == "SEND");
    CHECK(!f.send_enabled && f.refusal == std::string("(") + tr(core::validateFeatureRequestText("").c_str()) + ")" && f.no_email_note == "Without an email address we cannot reply or tell you when this is added." && f.sent_line.empty() && !f.attach && f.evidence.empty());
    CHECK(f.attach_note == featureSentence(false));
    in.text = "please add a thing";
    in.contact = "sam@example.org";
    f = readFeatureRequest(in);
    CHECK(f.send_enabled && f.refusal.empty() && f.text == "please add a thing" && f.contact == "sam@example.org" && f.no_email_note.empty() && f.attach_note == featureSentence(true) && f.counter == "18 / 100000 characters");
    in.contact = "G4XYZ";
    CHECK(readFeatureRequest(in).no_email_note != "" && readFeatureRequest(in).attach_note == featureSentence(true));
    in.contact = std::string(500, 'x');
    CHECK(readFeatureRequest(in).attach_note == featureSentence(false) && !readFeatureRequest(in).send_enabled);   // an over-long contact is not going to be sent at all
    in.contact.clear();
    in.sender.state = SendState::Sent;
    in.sender.blocked_until = 1030;
    f = readFeatureRequest(in);
    CHECK(f.sent_line == "Thank you - your request was sent." && f.sent_tone == Tone::Good && !f.send_enabled && f.refusal == "(wait 30s before sending another)");
    in.shown = false;
    CHECK(!readFeatureRequest(in).shown);

    // the bug page: neither kind chosen, no attachment decided
    ReportInputs b;
    b.shown = true;
    b.now_epoch = 1000;
    ReportPageModel p = readProblemReport(b);
    CHECK(p.shown && p.kind == 1 && p.title == "REPORT A BUG / DISLIKE" && p.kind_question == "What are you reporting?" && p.problem_kinds.size() == 2 && p.problem_kind_index == -1);
    CHECK(at(p.problem_kinds, 0).label == "Something is broken (bug)" && at(p.problem_kinds, 0).id == "bug" && at(p.problem_kinds, 1).label == "Something I dislike" && at(p.problem_kinds, 1).id == "dislike");
    CHECK(p.prompt == "What went wrong, and what were you doing when it did?" && p.no_email_note == "Without an email address we cannot reply or tell you when this is fixed or ready." && !p.send_enabled);
    CHECK(p.refusal == std::string("(") + tr(core::validateProblemReportKind("").c_str()) + ")" && p.attach_label == "Attach the diagnostics log" && p.preview_label == "Show what will be sent" && !p.attach && !p.probe_shown);
    b.problem_kind = "dislike";
    b.text = "I dislike the colour of the thing";
    p = readProblemReport(b);
    CHECK(p.problem_kind_index == 1 && p.prompt == "What do you dislike, and what would you rather it did?" && p.send_enabled && p.refusal.empty() && p.attach_note == problemSentence(false, false));
    // attached: the evidence lines, the preview, the probe, the local-only explanation; the sentence says the log is attached
    b.problem_kind = "bug";
    b.attach = true;
    b.diagnostics = "version 1\nlast-run-unclean: no\nrecovered: nothing\n";
    b.preview_open = true;
    const std::string why = "the report is local-only";   // (data: the reader's reason as the history gives it, not a catalogue key)
    b.local_only_why = why;
    b.reports_dir = "C:\\reports";
    b.probe_report = "";
    p = readProblemReport(b);
    CHECK(p.attach && p.problem_kind_index == 0 && p.evidence == (std::vector<std::string>{"last-run-unclean: no", "recovered: nothing"}) && p.preview_open && p.preview_text == b.diagnostics && !p.probe_shown);
    CHECK(p.attach_note == problemSentence(true, false) && p.local_only_line == std::string(tr(why.c_str())) + " The file is in the reports folder: C:\\reports");
    b.local_only_why.clear();
    CHECK(readProblemReport(b).local_only_line.empty());
    b.local_only_why = why;
    b.reports_dir.clear();
    CHECK(readProblemReport(b).local_only_line == std::string(tr(why.c_str())) + " The file is in the reports folder: (unavailable)");
    b.probe_report = "probe text";
    p = readProblemReport(b);
    CHECK(p.probe_shown && !p.probe_on && p.probe_label == "Attach the SDRplay diagnostic too" && p.preview_text == b.diagnostics);
    b.attach_probe = true;
    p = readProblemReport(b);
    CHECK(p.probe_on && p.preview_text == core::appendProbeToDiagnosticsForReport(b.diagnostics, "probe text") && p.preview_text != b.diagnostics);
    // unticked: no preview, no probe, no evidence
    b.attach = false;
    p = readProblemReport(b);
    CHECK(!p.attach && !p.preview_open && p.evidence.empty() && p.preview_text.empty() && !p.probe_shown);
    // a pasted sentinel report is recognised and travels as an attachment
    b.text = "Before. {not a report}";
    CHECK(readProblemReport(b).recognised_line.empty());
    // the contact
    b.contact = "a@b.c";
    b.text = "this is long enough to send";
    p = readProblemReport(b);
    CHECK(p.no_email_note.empty() && p.attach_note == problemSentence(false, true) && p.contact == "a@b.c");
    // a failure
    b.sender.state = SendState::Failed;
    b.sender.failure = "Could not reach the server.";
    p = readProblemReport(b);
    CHECK(p.sent_line == "Could not reach the server." && p.sent_tone == Tone::Err && p.send_enabled);
    b.sender.state = SendState::Sending;
    p = readProblemReport(b);
    CHECK(p.sending && !p.send_enabled && p.refusal == "(sending)" && p.sent_line == "Sending...");
}

// ---- the effects ----
void displayEffects() {
    const core::AppConfig c = displayConfig();
    const FoxReceiverState st = stateWith(-120.0, -10.0);

    Effect e = applyDisplayScale(c, 0);
    CHECK(e.save && e.half.interfaceScale == "auto" && (e.apply & kApplyScale) != 0 && e.value == "auto" && changedFields(c, e.half) == Names{"interfaceScale"} && e.commands.empty());
    e = applyDisplayScale(c, 6);
    CHECK(e.save && e.half.interfaceScale == "250" && e.value == "250" && changedFields(c, e.half) == Names{"interfaceScale"});
    CHECK(applyDisplayScale(c, 3).empty() && applyDisplayScale(c, 7).empty() && applyDisplayScale(c, -1).empty());   // the size already in force, or none: nothing

    e = applyDisplayTheme(c, 1);   // Bench Classic XL: the figures doubled and the switches put away
    CHECK(e.save && e.half.uiTheme == "classic-xl" && e.half.counterScale == 2 && !e.half.counterSwitches && e.half.readingsScale == 1.4f && e.half.tunerDisplayStyle == "nixie");
    CHECK((e.apply & kApplySkin) != 0 && e.value == "classic-xl" && changedFields(c, e.half) == (Names{"tunerDisplayStyle", "uiTheme"}));
    core::AppConfig plain = c;
    plain.counterScale = 1;
    plain.counterSwitches = true;
    plain.readingsScale = 1.0f;
    e = applyDisplayTheme(plain, 3);   // Glass Cockpit: a vfd face is drawn plain
    CHECK(e.save && e.half.uiTheme == "glass" && e.half.tunerDisplayStyle == "plain" && e.half.counterScale == 1 && e.half.counterSwitches && e.half.readingsScale == 1.0f);
    e = applyDisplayTheme(plain, 1);   // from a bench with the figures at normal size: the preset brings them doubled, the switches put away, the readings enlarged
    CHECK(e.save && e.half.counterScale == 2 && !e.half.counterSwitches && e.half.readingsScale == 1.4f);
    CHECK(changedFields(plain, e.half) == (Names{"tunerDisplayStyle", "uiTheme", "counterScale", "counterSwitches", "readingsScale"}));
    CHECK(applyDisplayTheme(c, 2).empty() && applyDisplayTheme(c, 9).empty());   // the look already in force (pickTheme is guarded by != now)

    e = applyDisplayDbMin(c, st, -100.f);
    CHECK(e.save && e.half.dbMin == -100.f && e.half.dbMax == -10.f && e.half.waterfallRangeMode == "manual" && (e.apply & kApplyDbRange) != 0);
    CHECK(e.commands.size() == 1 && cmdOf(e, 0).valid && cmdOf(e, 0).q.c.op == FOXAPI_OP_SET_DISPLAY_RANGE && cmdOf(e, 0).q.c.num[0] == -100.0 && cmdOf(e, 0).q.c.num[1] == -10.0);
    CHECK(changedFields(c, e.half) == Names{"dbMin"} || changedFields(c, e.half) == (Names{"dbMin", "dbMax"}));
    // a floor dragged up to the ceiling pushes back the end the hand is on, never the other
    e = applyDisplayDbMin(c, st, -15.f);
    CHECK(e.half.dbMin == -20.f && e.half.dbMax == -10.f && cmdOf(e, 0).q.c.num[0] == -20.0 && cmdOf(e, 0).q.c.num[1] == -10.0);
    // the ceiling dragged down onto the floor: the ceiling is held to its own slider's bounds, then the span rule moves the ceiling (the end moved)
    e = applyDisplayDbMax(c, st, -130.f);
    CHECK(e.save && e.half.dbMax == -100.f &&e.half.dbMin == -120.f && cmdOf(e, 0).q.c.num[1] == -100.0 && e.half.waterfallRangeMode == "manual");
    e = applyDisplayDbMax(c, stateWith(-30.0, 0.0), -95.f);   // -95 - (-30) is no span at all, and it is the ceiling that was moved: it gives way to floor + 10
    CHECK(e.half.dbMax == -20.f && e.half.dbMin == -30.f && cmdOf(e, 0).q.c.num[0] == -30.0 && cmdOf(e, 0).q.c.num[1] == -20.0);
    e = applyDisplayDbMax(c, st, 10.f);
    CHECK(e.half.dbMax == 10.f && e.half.dbMin == -120.f && cmdOf(e, 0).q.c.num[1] == 10.0);
    // a floor dragged into the ceiling gives way itself, the ceiling stays where it was left (the end the hand moved yields)
    e = applyDisplayDbMin(c, stateWith(-120.0, -95.0), -100.f);
    CHECK(e.half.dbMin == -105.f && e.half.dbMax == -95.f && cmdOf(e, 0).q.c.num[0] == -105.0 && cmdOf(e, 0).q.c.num[1] == -95.0);
    // a value that is not a number is the standard window
    e = applyDisplayDbMin(c, st, std::nanf(""));
    CHECK(e.save && e.half.dbMin == core::DbPair{}.floorDb && e.half.dbMax == core::DbPair{}.ceilingDb);

    e = applyDisplayFreqDisplay(c, 0);
    CHECK(e.save && e.half.tunerDisplayStyle == "nixie" && changedFields(c, e.half) == Names{"tunerDisplayStyle"});
    CHECK(applyDisplayFreqDisplay(c, 1).empty() && applyDisplayFreqDisplay(c, 2).half.tunerDisplayStyle == "plain" && applyDisplayFreqDisplay(c, 7).empty());
    e = applyDisplayFigures(c, 0);
    CHECK(e.save && e.half.counterScale == 1 && changedFields(c, e.half) == Names{"counterScale"} && applyDisplayFigures(c, 1).empty());
    e = applyDisplayTunerSwitches(c, true);
    CHECK(e.save && e.half.counterSwitches && changedFields(c, e.half) == Names{"counterSwitches"} && applyDisplayTunerSwitches(c, false).empty());
    e = applyDisplayEnlarge(c, false);
    CHECK(e.save && e.half.readingsScale == 1.0f && changedFields(c, e.half) == Names{"readingsScale"} && applyDisplayEnlarge(c, true).empty());
    core::AppConfig small = c;
    small.readingsScale = 1.0f;
    CHECK(applyDisplayEnlarge(small, true).half.readingsScale == 1.4f);

    // the icon size clamps to 16..96 and pulls a wider trail down to the wingspan; the trail is held to the wingspan of the icon in force
    core::AppConfig wide = c;
    wide.mapTrailWidthPx = 30;
    e = applyDisplayIconPx(wide, 20.f);
    CHECK(e.save && e.half.aircraftIconPx == 20 && e.half.mapTrailWidthPx == wingspanPx(20) && wingspanPx(20) == 18 && changedFields(wide, e.half) == (Names{"aircraftIconPx", "mapTrailWidthPx"}));
    e = applyDisplayIconPx(c, 70.f);
    CHECK(e.half.aircraftIconPx == 70 && e.half.mapTrailWidthPx == 9 && changedFields(c, e.half) == Names{"aircraftIconPx"});
    CHECK(applyDisplayIconPx(c, 500.f).half.aircraftIconPx == 96 && applyDisplayIconPx(c, 1.f).half.aircraftIconPx == 16);
    CHECK(applyDisplayIconPx(c, 64.f).empty() && applyDisplayIconPx(c, 63.6f).empty());   // rounded to the pixel: the size already in force
    e = applyDisplayTrailPx(c, 30.f);
    CHECK(e.save && e.half.mapTrailWidthPx == 30 && changedFields(c, e.half) == Names{"mapTrailWidthPx"});
    CHECK(applyDisplayTrailPx(c, 500.f).half.mapTrailWidthPx == 57 && applyDisplayTrailPx(c, 0.f).half.mapTrailWidthPx == 1 && applyDisplayTrailPx(c, 9.f).empty());

    e = applyDisplayBandPlan(c, false);
    CHECK(e.save && !e.half.bandPlanOverlay && changedFields(c, e.half) == Names{"bandPlanOverlay"} && applyDisplayBandPlan(c, true).empty());
    e = applyDisplayBandSize(c, 0);
    CHECK(e.save && e.half.bandPlanSize == "small" && changedFields(c, e.half) == Names{"bandPlanSize"} && applyDisplayBandSize(c, 2).empty() && applyDisplayBandSize(c, 5).empty());
    e = applyDisplayBandColour(c, 1);
    CHECK(e.save && e.half.bandPlanPalette == "vivid" && changedFields(c, e.half) == Names{"bandPlanPalette"} && applyDisplayBandColour(c, 2).empty() && applyDisplayBandColour(c, -1).empty());

    // the Region picker is an op, and nothing for the plan already in force
    StubTable t = plansTable();
    ReadOptions o;
    const DisplayModel m = readDisplay(t, c, st, o);
    e = applyDisplayRegion(m, 2);
    CHECK(!e.save && e.commands.size() == 1 && cmdOf(e, 0).valid && cmdOf(e, 0).q.c.op == FOXAPI_OP_SET_BAND_PLAN && commandText(cmdOf(e, 0)) == "uk" && cmdOf(e, 0).grant == FOXAPI_GRANT_SETTINGS);
    CHECK(applyDisplayRegion(m, 1).empty() && applyDisplayRegion(m, 3).empty() && applyDisplayRegion(m, -1).empty());
}

void languageEffects() {
    core::AppConfig c;
    const std::vector<cascade::i18n::Language>& langs = cascade::i18n::languages();
    Effect e = applyLanguageSelected(c, 0);
    CHECK(e.empty());   // Automatic is what it already is
    c.language = "de";
    e = applyLanguageSelected(c, 0);
    CHECK(e.save && e.half.language == "auto" && (e.apply & kApplyLanguage) != 0 && e.value == "auto" && changedFields(c, e.half) == Names{"language"});
    e = applyLanguageSelected(core::AppConfig{}, 1);   // English
    CHECK(e.save && e.half.language == "en" && e.value == "en");
    e = applyLanguageSelected(core::AppConfig{}, 1 + static_cast<int>(langs.size()) - 1);
    CHECK(e.save && e.half.language == langs.back().code);
    CHECK(applyLanguageSelected(core::AppConfig{}, 1 + static_cast<int>(langs.size())).empty() && applyLanguageSelected(core::AppConfig{}, -1).empty());
    c.language = "en";
    CHECK(applyLanguageSelected(c, 1).empty());   // the language in force
    // a language this machine cannot draw sends nothing
    cascade::i18n::setDrawablePredicate([](const std::string& code) { return code != "ja"; });
    std::size_t jaRow = 0;
    for (std::size_t i = 0; i < langs.size(); ++i) { if (langs[i].code == "ja") { jaRow = i + 1; } }
    CHECK(jaRow > 0 && applyLanguageSelected(core::AppConfig{}, static_cast<int>(jaRow)).empty());
    cascade::i18n::setDrawablePredicate(nullptr);
    CHECK(applyLanguageSelected(core::AppConfig{}, static_cast<int>(jaRow)).save);

    // USE <LANGUAGE>
    core::AppConfig de;
    de.country = "DE";
    e = applyLanguageSuggest(de);
    CHECK(e.save && e.half.language == "de" && (e.apply & kApplyLanguage) != 0 && e.value == "de" && changedFields(de, e.half) == Names{"language"});
    de.language = "de";
    CHECK(applyLanguageSuggest(de).empty());
    core::AppConfig us;
    us.country = "US";
    CHECK(applyLanguageSuggest(us).empty() && applyLanguageSuggest(core::AppConfig{}).empty());

    // the translation report: the bug page opens with its box started, unless the person has a draft
    e = applyLanguageTranslationReport("");
    CHECK(!e.save && e.local == Local::OpenReport && e.number == 1 && e.text == "Translation (" + cascade::i18n::current() + "): ");
    e = applyLanguageTranslationReport("my own words");
    CHECK(e.local == Local::OpenReport && e.number == 1 && e.text == "my own words");

    // the country list
    StubTable t;
    t.add(FOXAPI_LIST_BAND_PLANS, 0, "ITU Region 1", "itu-region1", FOXAPI_ITEM_ACTIVE);
    e = applyCountryFilter("bra");
    CHECK(e.local == Local::CountryFilter && e.text == "bra" && !e.save && e.commands.empty());
    const std::vector<CountryRow> all = countryRows("");
    int gbAt = -1;
    for (std::size_t i = 0; i < all.size(); ++i) { if (all[i].code == "GB") { gbAt = static_cast<int>(i); } }
    CHECK(gbAt >= 0);
    core::AppConfig base;
    e = applyCountrySelected(t, base, "", 1 + gbAt);
    CHECK(e.save && e.half.country == "GB" && changedFields(base, e.half) == Names{"country"});
    CHECK(e.commands.size() == 1 && cmdOf(e, 0).q.c.op == FOXAPI_OP_SET_BAND_PLAN && commandText(cmdOf(e, 0)) == std::string(core::findCountry("GB")->bandPlan));
    // a country whose plan is the one in force asks for nothing
    StubTable same;
    same.add(FOXAPI_LIST_BAND_PLANS, 0, "United Kingdom", core::findCountry("GB")->bandPlan, FOXAPI_ITEM_ACTIVE);
    e = applyCountrySelected(same, base, "", 1 + gbAt);
    CHECK(e.save && e.half.country == "GB" && e.commands.empty());
    // "Not set" forgets the country and leaves the band plan alone; it is not a pick when none is set
    core::AppConfig set;
    set.country = "GB";
    e = applyCountrySelected(t, set, "", 0);
    CHECK(e.save && e.half.country.empty() && e.commands.empty() && changedFields(set, e.half) == Names{"country"});
    CHECK(applyCountrySelected(t, base, "", 0).empty());
    // the same country again is no pick
    CHECK(applyCountrySelected(t, set, "", 1 + gbAt).empty());
    // with a filter the list has no "Not set" row and the index counts its own rows
    e = applyCountrySelected(t, base, "brazil", 0);
    CHECK(e.save && e.half.country == "BR" && commandText(cmdOf(e, 0)) == std::string(core::findCountry("BR")->bandPlan));
    CHECK(applyCountrySelected(t, base, "brazil", 1).empty() && applyCountrySelected(t, base, "", 100000).empty() && applyCountrySelected(t, base, "", -1).empty());
}

void updatesSerialEffects() {
    core::AppConfig c;
    Effect e = applyUpdatesCheck(c, false);
    CHECK(e.save && !e.half.updateCheckEnabled && (e.apply & kApplyUpdateCheck) != 0 && changedFields(c, e.half) == Names{"updateCheckEnabled"});
    c.updateCheckEnabled = false;
    e = applyUpdatesCheck(c, true);
    CHECK(e.save && e.half.updateCheckEnabled && (e.apply & kApplyUpdateCheck) != 0);
    CHECK(applyUpdatesCheck(c, false).empty());
    c.updateCheckEnabled = true;
    e = applyUpdatesChannel(c, 1, false);
    CHECK(e.save && e.half.updateChannel == "beta" && (e.apply & kApplyUpdateCheck) != 0 && changedFields(c, e.half) == Names{"updateChannel"});
    c.updateChannel = "beta";
    e = applyUpdatesChannel(c, 0, false);
    CHECK(e.save && e.half.updateChannel == "stable" && (e.apply & kApplyUpdateCheck) != 0);
    CHECK(applyUpdatesChannel(c, 1, false).empty() && applyUpdatesChannel(c, 5, false).empty());
    CHECK(applyUpdatesChannel(c, 0, true).empty());   // a Store copy has no channel
    c.updateCheckEnabled = false;
    e = applyUpdatesChannel(c, 0, false);
    CHECK(e.save && e.half.updateChannel == "stable" && (e.apply & kApplyUpdateCheck) == 0);   // saved; with the check off nothing is asked
    e = applyUpdatesShowAgain();
    CHECK(!e.save && (e.apply & kApplyUpdateShowAgain) != 0);
    e = applyUpdatesTeamMessage();
    CHECK(e.local == Local::OpenTeamMessages && !e.save);

    // serial ports and the GPS row
    Command r = opSerialRefresh();
    CHECK(r.valid && r.q.c.op == FOXAPI_OP_SCAN_SERIAL_PORTS && r.grant == FOXAPI_GRANT_SETTINGS);
    e = applySerialRefresh();
    CHECK(e.commands.size() == 1 && cmdOf(e, 0).q.c.op == FOXAPI_OP_SCAN_SERIAL_PORTS && !e.save);
    StubTable t;
    t.add(FOXAPI_LIST_SERIAL_PORTS, 0, "COM3", "");
    t.add(FOXAPI_LIST_SERIAL_PORTS, 0, "COM10", "");
    core::AppConfig g;
    g.gpsPort = "COM3";
    ReadOptions o;
    SerialPortsModel m = readSerialPorts(t, g, o);
    e = applyGpsPort(g, m.gps, 1);
    CHECK(e.save && e.half.gpsPort == "COM10" && changedFields(g, e.half) == Names{"gpsPort"} && e.commands.size() == 2);
    CHECK(cmdOf(e, 0).q.c.op == FOXAPP_OP_CLEAR_STATUS && cmdOf(e, 0).q.c.ival[0] == core::cmd::FOXAPP_STATUS_GPS_REFUSAL && cmdOf(e, 1).q.c.op == FOXAPP_OP_CLEAR_STATUS && cmdOf(e, 1).q.c.ival[0] == core::cmd::FOXAPP_STATUS_GPS_RESULT);
    e = applyGpsPort(g, m.gps, 0);   // the same port picked again: nothing to save, and the last result is forgotten all the same (the Selectable fires)
    CHECK(!e.save && e.commands.size() == 2);
    CHECK(applyGpsPort(g, m.gps, 7).empty() && applyGpsPort(g, m.gps, -1).empty());
    e = applyGpsBaud(g, m.gps, 5);
    CHECK(e.save && e.half.gpsBaud == 115200 && changedFields(g, e.half) == Names{"gpsBaud"} && e.commands.size() == 2);
    CHECK(applyGpsBaud(g, m.gps, 9).empty());
    // nothing changes under a listen in progress
    GpsRow listening = m.gps;
    listening.listening = true;
    CHECK(applyGpsPort(g, listening, 1).empty() && applyGpsBaud(g, listening, 0).empty());
    // the key: Stop while listening, the read otherwise, nothing with no port
    e = applyGpsToggle(g, listening);
    CHECK(!e.save && e.commands.size() == 1 && cmdOf(e, 0).q.c.op == FOXAPI_OP_GPS && cmdOf(e, 0).q.c.ival[0] == 0 && cmdOf(e, 0).grant == FOXAPI_GRANT_SETTINGS);
    g.gpsBaud = 4800;
    e = applyGpsToggle(g, m.gps);
    CHECK(e.commands.size() == 1 && cmdOf(e, 0).q.c.op == FOXAPI_OP_GPS && commandText(cmdOf(e, 0)) == "COM3" && cmdOf(e, 0).q.c.ival[0] == 1 && cmdOf(e, 0).q.c.num[0] == 4800.0);
    core::AppConfig none;
    CHECK(applyGpsToggle(none, readSerialPorts(t, none, o).gps).empty());
}

void keyEffects() {
    core::AppConfig c;
    Effect e = applyKeysCapture("", "mute");
    CHECK(e.local == Local::CaptureStart && e.text == "mute" && !e.save);
    e = applyKeysCapture("mute", "mute");   // a second press on the listening cap puts it back
    CHECK(e.local == Local::CaptureCancel && e.text == "mute");
    e = applyKeysCapture("mute", "volumeUp");
    CHECK(e.local == Local::CaptureStart && e.text == "volumeUp");
    CHECK(applyKeysCapture("", "nonsense").empty());
    e = applyKeysCancel();
    CHECK(e.local == Local::CaptureCancel && e.text.empty());

    // a captured chord: stored as "id=chord", only what differs from the stock table
    e = applyKeysCaptured(c, "mute", "Ctrl+Shift+M");
    CHECK(e.save && e.half.keyBindings == (std::vector<std::string>{"mute=Ctrl+Shift+M"}) && e.local == Local::CaptureCancel && changedFields(c, e.half) == Names{"keyBindings"});
    // a chord that is the stock chord removes the line
    c.keyBindings = {"mute=Ctrl+Shift+M"};
    e = applyKeysCaptured(c, "mute", "M");
    CHECK(e.save && e.half.keyBindings.empty());
    // a clash is shown, never refused
    e = applyKeysCaptured(c, "volumeUp", "Ctrl+Shift+M");
    CHECK(e.save && e.half.keyBindings == (std::vector<std::string>{"mute=Ctrl+Shift+M", "volumeUp=Ctrl+Shift+M"}));
    // Escape leaves it alone, whatever is held; Backspace clears; a text that is no chord is no change
    e = applyKeysCaptured(c, "mute", "Escape");
    CHECK(!e.save && e.local == Local::CaptureCancel);
    CHECK(!applyKeysCaptured(c, "mute", "Ctrl+Esc").save && !applyKeysCaptured(c, "mute", "Banana").save && applyKeysCaptured(c, "mute", "Banana").local == Local::CaptureCancel);
    e = applyKeysCaptured(c, "mute", "Backspace");
    CHECK(e.save && e.half.keyBindings == (std::vector<std::string>{"mute=None"}) && e.local == Local::CaptureCancel);
    CHECK(applyKeysCaptured(c, "mute", "Ctrl+Backspace").half.keyBindings == (std::vector<std::string>{"mute=None"}));
    CHECK(!applyKeysCaptured(c, "nonsense", "A").save && applyKeysCaptured(c, "nonsense", "A").local == Local::CaptureCancel);
    // the stored chord again is no change (but the capture ends)
    e = applyKeysCaptured(c, "mute", "Ctrl+Shift+M");
    CHECK(!e.save && e.local == Local::CaptureCancel);
    // clear one, reset all
    e = applyKeysClear(core::AppConfig{}, "volumeDown");
    CHECK(e.save && e.half.keyBindings == (std::vector<std::string>{"volumeDown=None"}) && changedFields(core::AppConfig{}, e.half) == Names{"keyBindings"});
    CHECK(applyKeysClear(core::AppConfig{}, "nonsense").empty());
    core::AppConfig cleared;
    cleared.keyBindings = {"volumeDown=None"};
    CHECK(applyKeysClear(cleared, "volumeDown").empty());
    c.keyBindings = {"mute=Ctrl+M", "volumeUp=None"};
    e = applyKeysReset(c);
    CHECK(e.save && e.half.keyBindings.empty() && e.local == Local::CaptureCancel);
    CHECK(!applyKeysReset(core::AppConfig{}).save && applyKeysReset(core::AppConfig{}).local == Local::CaptureCancel);
}

void diagnosticsUsageTesterEffects() {
    core::AppConfig c;
    c.diagnosticsEnabled = true;
    Effect e = applyDiagRecord(c, false);
    CHECK(e.save && !e.half.diagnosticsEnabled && (e.apply & kApplyDiagnostics) != 0 && changedFields(c, e.half) == Names{"diagnosticsEnabled"});
    CHECK(applyDiagRecord(c, true).empty());
    e = applyDiagMinidump(c, true);
    CHECK(e.save && e.half.diagnosticsMinidump && (e.apply & kApplyDiagnostics) != 0 && changedFields(c, e.half) == Names{"diagnosticsMinidump"});
    CHECK(applyDiagMinidump(c, false).empty());
    e = applyDiagWhat(true);
    CHECK(e.local == Local::ToggleWhat && e.flag && !e.save);
    CHECK(!applyDiagWhat(false).flag);
    e = applyDiagOpenLog("C:\\x\\log.txt");
    CHECK(e.local == Local::OpenLog && e.text == "C:\\x\\log.txt");
    CHECK(applyDiagOpenLog("").empty());   // no log is being written: nothing to show
    e = applyDiagOpenReports("C:\\x\\reports");
    CHECK(e.local == Local::OpenReports && e.text == "C:\\x\\reports");
    CHECK(applyDiagOpenReports("").empty());

    e = applyUsageSend(true);
    CHECK(!e.save && e.commands.size() == 1 && cmdOf(e, 0).q.c.op == FOXAPP_OP_TELEMETRY_CONSENT && cmdOf(e, 0).q.c.ival[0] == 1 && cmdOf(e, 0).grant == FOXAPI_GRANT_ADMIN);
    e = applyUsageSend(false);
    CHECK(e.commands.size() == 1 && cmdOf(e, 0).q.c.ival[0] == 0 && cmdOf(e, 0).grant == FOXAPI_GRANT_ADMIN);
    e = applyUsageWhat(true);
    CHECK(e.local == Local::ToggleWhat && e.flag);
    e = applyUsagePrivacy();
    CHECK(e.local == Local::OpenUrl && e.text == core::kPrivacyPolicyUrl);

    // the beta tester
    core::AppConfig t;
    t.testerAppToken = kAppTok;
    t.testerAppTokenName = "Alex";
    t.testerToken = "";
    Effect u = applyTesterUnlink(t);
    CHECK(u.save && u.half.testerAppToken.empty() && u.half.testerAppTokenName.empty() && changedFields(t, u.half) == (Names{"testerAppToken", "testerAppTokenName"}));
    CHECK(applyTesterUnlink(core::AppConfig{}).empty());
    core::AppConfig r;
    r.testerToken = kPortal;
    r.testerTokenInvalid = true;
    u = applyTesterRemoveCode(r);
    CHECK(u.save && u.half.testerToken.empty() && !u.half.testerTokenInvalid && u.local == Local::TesterDraft && u.text.empty() && u.flag);
    CHECK(changedFields(r, u.half) == (Names{"testerToken", "testerTokenInvalid"}));
    r.testerAppToken = kAppTok;
    r.testerAppTokenName = "Alex";
    u = applyTesterRemoveCode(r);
    CHECK(u.half.testerAppToken.empty() && u.half.testerAppTokenName.empty());   // a portal-token removal is a full identity reset
    e = applyTesterCodeCommitted("some text");
    CHECK(e.local == Local::TesterDraft && e.text == "some text" && !e.flag && !e.save);
    // Use this code
    core::AppConfig base;
    e = applyTesterUse(base, std::string(" ") + kAppTok + " ");
    CHECK(!e.save && e.local == Local::TesterResolve && e.text == kAppTok && e.flag);
    e = applyTesterUse(base, kPortal);
    CHECK(e.save && e.half.testerToken == kPortal && !e.half.testerTokenInvalid && e.half.testerAppToken.empty() && e.local == Local::TesterDraft && e.text.empty() && e.flag && changedFields(base, e.half) == Names{"testerToken"});
    core::AppConfig linked = base;
    linked.testerAppToken = kAppTok;
    linked.testerAppTokenName = "Alex";
    e = applyTesterUse(linked, std::string("https://foxsdr.com/#t=") + kPortal);
    CHECK(e.save && e.half.testerToken == kPortal && e.half.testerAppToken.empty() && e.half.testerAppTokenName.empty());
    core::AppConfig bad = base;
    bad.testerToken = kPortal;
    bad.testerTokenInvalid = true;
    e = applyTesterUse(bad, "0123456789abcdef0123456789abcdee");
    CHECK(e.save && e.half.testerToken == "0123456789abcdef0123456789abcdee" && !e.half.testerTokenInvalid);
    e = applyTesterUse(base, "hello");
    CHECK(!e.save && e.local == Local::TesterError && e.text == "that does not look like a tester code or link" && !e.flag);
    e = applyTesterShowSent(true);
    CHECK(e.local == Local::ToggleTesterSent && e.flag);
}

void reportEffects() {
    Effect e = applyReportOpen(0);
    CHECK(e.local == Local::OpenReport && e.number == 0 && e.text.empty() && !e.save);
    e = applyReportOpen(1);
    CHECK(e.local == Local::OpenReport && e.number == 1);
    CHECK(applyReportOpen(2).empty());
    e = applyReportText(1, "words");
    CHECK(e.local == Local::ReportText && e.number == 1 && e.text == "words");
    e = applyReportContact(0, "a@b.c");
    CHECK(e.local == Local::ReportContact && e.number == 0 && e.text == "a@b.c");
    e = applyReportKind(0);
    CHECK(e.local == Local::ReportKind && e.text == "bug" && e.flag);   // the attach box follows the kind: ticked for a bug
    e = applyReportKind(1);
    CHECK(e.local == Local::ReportKind && e.text == "dislike" && !e.flag);
    CHECK(applyReportKind(2).empty() && applyReportKind(-1).empty());
    e = applyReportAttach(1, true);
    CHECK(e.local == Local::ReportAttach && e.flag && e.number == 1);
    e = applyReportPreview(1, true);
    CHECK(e.local == Local::ReportPreview && e.flag);
    e = applyReportProbe(true);
    CHECK(e.local == Local::ReportProbe && e.flag);
    e = applyReportClosed(0);
    CHECK(e.local == Local::CloseReport && e.number == 0);
    e = applyAboutOpen();
    CHECK(e.local == Local::OpenAbout);

    // SEND: a feature request carries its words, the contact, the version and the platform - nothing else; the count is the server's
    ReportInputs in;
    in.shown = true;
    in.text = "please add a thing";
    in.contact = "sam@example.org";
    in.version = "0.99.105";
    in.now_epoch = 1000;
    e = applyReportSend(0, in);
    CHECK(e.report.send && e.report.kind == 0 && e.report.feature.text == "please add a thing" && e.report.feature.contact == "sam@example.org" && e.report.feature.version == "0.99.105");
    CHECK(e.report.feature.platform == core::featureRequestPlatform() && e.report.feature.arch == core::featureRequestArch() && e.report.endpoint == core::featureRequestEndpoint() && e.report.chars == 18 && !e.save);
    // not while the verdict says no
    in.text = "short";
    CHECK(!applyReportSend(0, in).report.send && applyReportSend(0, in).empty());
    in.text = "please add a thing";
    in.sender.state = SendState::Sending;
    CHECK(!applyReportSend(0, in).report.send);
    in.sender.state = SendState::Idle;

    // a bug report: the kind, the text, the contact, the log when it is ticked (prepared once, byte for byte what the preview shows), a recognised report
    ReportInputs b;
    b.shown = true;
    b.problem_kind = "bug";
    b.text = "it broke when I pressed the thing";
    b.contact = "G4XYZ";
    b.version = "0.99.105";
    b.attach = true;
    b.diagnostics = "version 1\nlast-run-unclean: no\n";
    e = applyReportSend(1, b);
    CHECK(e.report.send && e.report.kind == 1 && e.report.problem.kind == "bug" && e.report.problem.text == b.text && e.report.problem.contact == "G4XYZ" && e.report.problem.version == "0.99.105");
    CHECK(e.report.problem.diagnostics == b.diagnostics && e.report.problem.attachments.empty() && e.report.endpoint == core::problemReportEndpoint() && e.report.problem.platform == core::featureRequestPlatform());
    b.attach_probe = true;
    b.probe_report = "probe text";
    CHECK(applyReportSend(1, b).report.problem.diagnostics == core::appendProbeToDiagnosticsForReport(b.diagnostics, "probe text"));
    b.probe_report.clear();
    CHECK(applyReportSend(1, b).report.problem.diagnostics == b.diagnostics);
    b.attach = false;
    CHECK(applyReportSend(1, b).report.problem.diagnostics.empty());
    b.problem_kind = "dislike";
    CHECK(applyReportSend(1, b).report.problem.kind == "dislike");
    b.problem_kind.clear();
    CHECK(!applyReportSend(1, b).report.send);   // neither chosen: the kind's own refusal
    CHECK(!applyReportSend(2, b).report.send);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // a crash must not eat the lines before it
    driftChecks();
    displayReads();
    languageReads();
    updatesReads();
    serialReads();
    keyReads();
    diagnosticsUsageTesterReads();
    reportReads();
    displayEffects();
    languageEffects();
    updatesSerialEffects();
    keyEffects();
    diagnosticsUsageTesterEffects();
    reportEffects();
    return testSummary("test_slint_host_system");
}
