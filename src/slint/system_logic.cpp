// system_logic.cpp - the SYSTEM bank's pure logic (system_logic.hpp): the ImGui window's rules and sentences over the interface half of the settings, the table's
// lists and facts and the engine's state, with no window and no engine. Every sentence is the drawer's own, through cascade::i18n::tr(); a format is composed with
// formatText/formatUtf8 into a std::string, never into a fixed buffer (tests/test_i18n.cpp refuses a translated format into a sizeof'd one). Each effect function
// does what the ImGui control does and returns it as data (system_logic.hpp has the table).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "system_logic.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "core/countries.hpp"
#include "core/i18n.hpp"
#include "core/pasted_report.hpp"
#include "core/serial_port.hpp"
#include "core/telemetry.hpp"
#include "core/tester_usage.hpp"
#include "core/utf8_text.hpp"

namespace foxsdr::slintsystem {

using cascade::i18n::tr;
namespace cc = cascade::core;
namespace cmd = cascade::core::cmd;

namespace {

std::string T(const char* english) { return tr(english); }

std::string factOf(TableReader& t, const std::string& key) {
    std::string s;
    if (!t.fact(key, s)) { s.clear(); }
    return s;
}

std::int32_t listSize(TableReader& t, std::uint32_t list) { return std::max<std::int32_t>(0, t.readList(list, 0, nullptr)); }

// Every entry of a list, in order; `got <= i` is the end of a list that changed under the read (what was read stands).
template <typename F>
void eachItem(TableReader& t, std::uint32_t list, F&& f) {
    const std::int32_t n = listSize(t, list);
    for (std::int32_t i = 0; i < n; ++i) {
        FoxListItem it{};
        it.structSize = sizeof(it);
        if (t.readList(list, static_cast<std::uint32_t>(i), &it) <= i) { break; }
        f(i, it);
    }
}

Command make(const cmd::QueuedCommand& q) {
    Command c;
    c.q = q;
    c.grant = foxsdr::slintsignal::grantFor(q.c.op);
    c.valid = true;
    return c;
}
Command make(const FoxCommand& f) {
    cmd::QueuedCommand q;
    q.c = f;
    return make(q);
}

std::string foldKey(const std::string& s) { return cc::foldForSearch(s); }

bool inRange(int index, std::size_t size) { return index >= 0 && static_cast<std::size_t>(index) < size; }

// An effect that saves `half` when it differs from `cur` in the one place the caller names (`changed`), else nothing.
Effect saved(bool changed, const cc::AppConfig& half) {
    Effect e;
    if (!changed) { return e; }
    e.save = true;
    e.half = half;
    return e;
}

std::string joinLines(const std::vector<std::string>& v) {
    std::string out;
    for (const std::string& s : v) {
        if (!out.empty()) { out += '\n'; }
        out += s;
    }
    return out;
}

}  // namespace

// =================================================================================================================================
// Display

namespace {
constexpr float kAircraftSpanFraction = 114.0f / 128.0f;   // gui/aircraft_icons.hpp
}

int scaleIndexOf(const std::string& stored) {
    if (stored == cascade::gui::uiscale::kAuto) { return 0; }
    for (int k = 0; k < kScaleStepCount; ++k) {
        if (stored == std::to_string(cascade::gui::uiscale::kSteps[k])) { return k + 1; }
    }
    return 0;
}
std::string scaleChoiceOf(int index) {
    if (index == 0) { return cascade::gui::uiscale::kAuto; }
    if (index >= 1 && index <= kScaleStepCount) { return std::to_string(cascade::gui::uiscale::kSteps[index - 1]); }
    return {};
}
std::string scaleLabel(int index, int monitorPercent) {
    if (index == 0) { return cc::formatText(tr("Auto (follows Windows, %d%% here)"), monitorPercent); }
    return scaleChoiceOf(index) + "%";
}

namespace {
// The six looks (gui/theme.cpp), with what picking one brings (pickTheme): a face the application does not draw (vfd, lcd) is drawn plain.
constexpr ThemePreset kThemes[kThemeCount] = {
    {"today", FOX_TR_NOOP("Today's bench"), "nixie", 1.0f, true, 1.0f},
    {"classic-xl", FOX_TR_NOOP("Bench Classic XL"), "nixie", 2.0f, false, 1.4f},
    {"night", FOX_TR_NOOP("Night Watch"), "neon", 1.0f, true, 1.0f},
    {"glass", FOX_TR_NOOP("Glass Cockpit"), "plain", 1.0f, true, 1.0f},
    {"daylight", FOX_TR_NOOP("Daylight Lab"), "plain", 1.0f, true, 1.0f},
    {"field", FOX_TR_NOOP("Field Radio"), "plain", 1.0f, true, 1.0f},
};
}  // namespace

const ThemePreset& themePreset(int index) { return kThemes[(index >= 0 && index < kThemeCount) ? index : 0]; }
int themeIndexOf(const std::string& key) {
    for (int i = 0; i < kThemeCount; ++i) {
        if (key == kThemes[i].key) { return i; }
    }
    return 0;
}

std::string dbText(float db) { return cc::formatText("%.0f", static_cast<double>(db)); }
int clampIconPx(int px) { return std::clamp(px, kAircraftIconMinPx, kAircraftIconMaxPx); }
int wingspanPx(int iconPx) {
    const int span = static_cast<int>(static_cast<float>(clampIconPx(iconPx)) * kAircraftSpanFraction + 0.5f);
    return span < kTrailWidthMinPx ? kTrailWidthMinPx : span;
}
std::string pxText(int px) { return cc::formatText("%d px", px); }

const char* tunerStyleName(int index) { return cascade::gui::kTunerStyleNames[std::clamp(index, 0, cascade::gui::kTunerStyleCount - 1)]; }
int tunerStyleIndexOf(const std::string& stored) {
    for (int i = 0; i < cascade::gui::kTunerStyleCount; ++i) {
        if (stored == cascade::gui::kTunerStyleNames[i]) { return i; }
    }
    return 0;   // tunerStyleFromName: an unknown or empty name is the Nixie plate
}

namespace {
constexpr const char* kBandPlanSizeKeys[3] = {"small", "medium", "large"};
constexpr const char* kBandPlanSizeLabels[3] = {FOX_TR_NOOP("Small"), FOX_TR_NOOP("Medium"), FOX_TR_NOOP("Large")};
constexpr const char* kBandPlanPaletteKeys[3] = {"classic", "vivid", "mono"};
constexpr const char* kBandPlanPaletteLabels[3] = {FOX_TR_NOOP("Classic"), FOX_TR_NOOP("Vivid"), FOX_TR_NOOP("Mono")};
}  // namespace

const char* bandPlanSizeKey(int index) { return kBandPlanSizeKeys[std::clamp(index, 0, 2)]; }
int bandPlanSizeIndexOf(const std::string& stored) {
    for (int i = 0; i < 3; ++i) {
        if (stored == kBandPlanSizeKeys[i]) { return i; }
    }
    return 0;
}
const char* bandPlanPaletteKey(int index) { return kBandPlanPaletteKeys[std::clamp(index, 0, 2)]; }
int bandPlanPaletteIndexOf(const std::string& stored) {
    for (int i = 0; i < 3; ++i) {
        if (stored == kBandPlanPaletteKeys[i]) { return i; }
    }
    return 0;
}

// core::displayPlanName: each " + " part of a chain through tr(), a name no catalogue carries comes back as it is.
std::string displayPlanName(const std::string& name) {
    static const std::string kJoin = " + ";
    std::string out;
    std::size_t from = 0;
    while (true) {
        const std::size_t at = name.find(kJoin, from);
        const std::string part = name.substr(from, at == std::string::npos ? std::string::npos : at - from);
        out += tr(part.c_str());
        if (at == std::string::npos) { break; }
        out += kJoin;
        from = at + kJoin.size();
    }
    return out;
}

PlanStatus planStatus(const std::string& error, int bands, const std::string& planName) {
    PlanStatus s;
    if (!error.empty()) {
        s.text = error;
        s.tone = Tone::Err;
    } else if (bands <= 0) {
        s.text = std::string(tr("no band plan installed"));
    } else {
        s.text = cc::formatText(tr("%s (%d bands)"), displayPlanName(planName).c_str(), bands);
    }
    return s;
}

DisplayModel readDisplay(TableReader& t, const cc::AppConfig& iface, const FoxReceiverState& st, const ReadOptions& o) {
    DisplayModel m;
    for (int i = 0; i <= kScaleStepCount; ++i) {
        SelectItem s;
        s.label = scaleLabel(i, o.monitor_percent);
        s.id = scaleChoiceOf(i);
        m.scales.push_back(std::move(s));
    }
    m.scale_index = scaleIndexOf(iface.interfaceScale);
    for (int i = 0; i < kThemeCount; ++i) {
        SelectItem s;
        s.label = T(kThemes[i].label);
        s.id = kThemes[i].key;
        m.themes.push_back(std::move(s));
    }
    m.theme_index = themeIndexOf(iface.uiTheme);
    m.db_min = static_cast<float>(st.dbMin);
    m.db_max = static_cast<float>(st.dbMax);
    m.db_min_text = dbText(m.db_min);
    m.db_max_text = dbText(m.db_max);
    m.db_min_lo = kFloorMinDb;
    m.db_min_hi = kFloorMaxDb;
    m.db_max_lo = kCeilingMinDb;
    m.db_max_hi = kCeilingMaxDb;
    m.range_manual = iface.waterfallRangeMode == "manual";
    for (int i = 0; i < cascade::gui::kTunerStyleCount; ++i) {
        SelectItem s;
        s.label = T(cascade::gui::kTunerStyleLabels[i]);
        s.id = cascade::gui::kTunerStyleNames[i];
        m.freq_displays.push_back(std::move(s));
    }
    m.freq_display_index = tunerStyleIndexOf(iface.tunerDisplayStyle);
    m.figures.push_back(SelectItem{std::string(tr("Normal size")), "1"});
    m.figures.push_back(SelectItem{std::string(tr("Enlarged figures")), "2"});
    m.figures_index = iface.counterScale >= 2 ? 1 : 0;
    m.tuner_switches = iface.counterSwitches;
    m.enlarge_readings = iface.readingsScale > 1.0f;
    const int icon = clampIconPx(iface.aircraftIconPx);
    const int wing = wingspanPx(icon);
    const int trail = std::clamp(iface.mapTrailWidthPx, kTrailWidthMinPx, wing);   // drawTrailWidthControl pulls a wider trail down to the wingspan
    m.icon_px = static_cast<float>(icon);
    m.icon_text = pxText(icon);
    m.trail_px = static_cast<float>(trail);
    m.trail_text = pxText(trail);
    m.icon_min = static_cast<float>(kAircraftIconMinPx);
    m.icon_max = static_cast<float>(kAircraftIconMaxPx);
    m.trail_min = static_cast<float>(kTrailWidthMinPx);
    m.trail_max = static_cast<float>(wing);
    // THE TRAIL STYLE (docs 6.52, tick row 364): the classic map page's Line / Ribbon combo, saved as mapTrailStyle (0 line, 1 ribbon; config.cpp
    // reads anything else as a line). It was applied by the Slint map since 6.44 but had no control; it sits under Trail width, where the width is.
    m.trail_styles.push_back(SelectItem{std::string(tr("Line")), "line"});
    m.trail_styles.push_back(SelectItem{std::string(tr("Ribbon")), "ribbon"});
    m.trail_style_index = iface.mapTrailStyle == 1 ? 1 : 0;
    m.scale_tooltip = std::string(tr("Makes the whole bench bigger, fonts and controls together. "
                        "Auto follows this monitor's own Windows display scaling; a "
                        "fixed percentage overrides it. Ctrl+= / Ctrl+- step through "
                        "the list, Ctrl+0 returns to Auto."));
    m.switches_tooltip = std::string(tr("The UP and DN switches under each figure. The wheel "
                           "over a figure still tunes it without them."));
    m.icon_tooltip = cc::formatText(tr("Size of the aircraft on the maps and the radar scope.\n"
                                       "Standard is %d px. Right-click to put it back."),
                                    kAircraftIconDefaultPx);
    m.trail_tooltip = cc::formatText(tr("Width of the aircraft trails on the maps and the radar scope,\n"
                                        "from a hairline up to the aircraft's wingspan (%d px at the\n"
                                        "current icon size). Standard is %d px. Right-click to put it back."),
                                     wing, kTrailWidthDefaultPx);

    m.band_plan = iface.bandPlanOverlay;
    m.band_options_shown = iface.bandPlanOverlay;
    for (int i = 0; i < 3; ++i) { m.band_sizes.push_back(SelectItem{T(kBandPlanSizeLabels[i]), kBandPlanSizeKeys[i]}); }
    for (int i = 0; i < 3; ++i) { m.band_colours.push_back(SelectItem{T(kBandPlanPaletteLabels[i]), kBandPlanPaletteKeys[i]}); }
    m.band_size_index = bandPlanSizeIndexOf(iface.bandPlanSize);
    m.band_colour_index = bandPlanPaletteIndexOf(iface.bandPlanPalette);

    // The region picker: the plans the engine holds (BAND_PLANS: name, detail = the id SET_BAND_PLAN takes, ACTIVE = in force).
    std::string activeName;
    eachItem(t, FOXAPI_LIST_BAND_PLANS, [&](int i, const FoxListItem& it) {
        SelectItem s;
        s.label = displayPlanName(it.name);
        s.id = it.detail;
        if ((it.flags & FOXAPI_ITEM_ACTIVE) != 0u) {
            m.region_index = i;
            activeName = s.label;
        }
        m.regions.push_back(std::move(s));
    });
    m.regions_shown = m.band_plan && m.regions.size() > 1;
    m.region_preview = m.region_index >= 0 ? activeName : std::string(tr("(all installed)"));
    const int bands = listSize(t, FOXAPI_LIST_BAND_PLAN);
    const std::string error = factOf(t, "fact.status.bandPlanError");
    if (m.band_plan) {
        const PlanStatus ps = planStatus(error, bands, factOf(t, "fact.bandPlan.name"));
        m.plan_status = ps.text;
        m.plan_status_tone = ps.tone;
    }
    return m;
}

// =================================================================================================================================
// Language & country

std::string languageLabel(const std::string& code, bool drawable, const std::string& reason) {
    const cascade::i18n::Language* l = cascade::i18n::findLanguage(code);
    if (l == nullptr) { return code; }
    // A language this machine cannot draw is named in ENGLISH, with the reason: its own name is written in the very characters that are missing.
    if (!drawable && !reason.empty()) { return l->englishName + " - " + tr(reason.c_str()); }
    std::string s = l->name;
    if (l->machine) {
        s += ' ';
        s += tr("(machine translation)");
    }
    return s;
}

std::string autoLanguageLabel(const std::string& systemLocale) {
    const std::string autoCode = cascade::i18n::resolveFor("auto", systemLocale);
    const cascade::i18n::Language* autoLang = cascade::i18n::findLanguage(autoCode);
    // The language "Automatic" lands on, by its own name - except English, the one name said in the language in force.
    const std::string autoName = (autoLang == nullptr || autoLang->code == "en") ? std::string(tr("English")) : autoLang->name;
    return cc::formatText(tr("Automatic (%s)"), autoName.c_str());
}

std::string useLanguageLabel(const std::string& languageName) { return cc::formatText(tr("USE %s"), cc::upperLegend(languageName).c_str()); }

bool countryMatches(const std::string& filter, const std::string& shown, const std::string& english, const std::string& code) {
    const std::string needle = foldKey(filter);
    if (needle.empty()) { return true; }
    return foldKey(shown).find(needle) != std::string::npos || foldKey(english).find(needle) != std::string::npos || foldKey(code) == needle;
}

std::vector<CountryRow> countryRows(const std::string& filter) {
    const std::span<const cc::Country> all = cc::countries();
    std::vector<std::size_t> order(all.size());
    std::vector<std::string> keys(all.size());
    std::vector<std::string> shown(all.size());
    for (std::size_t i = 0; i < all.size(); ++i) {
        order[i] = i;
        shown[i] = tr(all[i].name);
        keys[i] = foldKey(shown[i]);
    }
    // Sorted by the name as it is SHOWN, in the language in force.
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return keys[a] < keys[b]; });
    std::vector<CountryRow> rows;
    for (const std::size_t i : order) {
        if (!countryMatches(filter, shown[i], all[i].name, all[i].code)) { continue; }
        rows.push_back(CountryRow{all[i].code, shown[i]});
    }
    return rows;
}

LanguageModel readLanguage(TableReader& t, const cc::AppConfig& iface, const ReadOptions& o) {
    LanguageModel m;
    const std::vector<cascade::i18n::Language>& langs = cascade::i18n::languages();
    const std::string autoLabel = autoLanguageLabel(o.system_locale);
    m.languages.push_back(SelectItem{autoLabel, "auto"});
    const auto reasonOf = [&](const std::string& code) {
        for (const auto& r : o.language_reason) {
            if (r.first == code) { return r.second; }
        }
        return std::string();
    };
    for (const cascade::i18n::Language& l : langs) {
        SelectItem s;
        const bool drawable = cascade::i18n::drawable(l.code);
        s.label = languageLabel(l.code, drawable, reasonOf(l.code));
        s.id = l.code;
        s.enabled = drawable;
        m.languages.push_back(std::move(s));
    }
    const std::string setting = iface.language.empty() ? std::string("auto") : iface.language;
    const bool followsSystem = foldKey(setting) == "auto";
    if (followsSystem) {
        m.language_index = 0;
        m.language_text = autoLabel;
    } else {
        const std::string chosen = cascade::i18n::resolveFor(setting, std::string());
        m.language_index = 1;   // a code no catalogue has is English, which is the first language
        for (std::size_t i = 0; i < langs.size(); ++i) {
            if (langs[i].code == chosen) { m.language_index = static_cast<int>(i) + 1; }
        }
        m.language_text = m.languages[static_cast<std::size_t>(m.language_index)].label;
    }

    const std::string inForce = cascade::i18n::current();
    const cascade::i18n::Language* active = cascade::i18n::findLanguage(inForce);
    if (active != nullptr && active->machine) { m.machine_hint = std::string(tr("This language was translated by machine and may read oddly.")); }
    m.translation_label = std::string(tr("SUGGEST A BETTER TRANSLATION"));
    m.country_hint = std::string(tr("Type to narrow the list"));
    m.country_filter = o.country_filter;
    m.plan_changed_hint = o.plan_changed_note;

    // The country list: "Not set" first while the filter is empty, then the rows as filtered.
    const cc::Country* chosen = cc::findCountry(iface.country);
    const std::vector<CountryRow> rows = countryRows(o.country_filter);
    const bool unfiltered = foldKey(o.country_filter).empty();
    if (unfiltered) { m.countries.push_back(SelectItem{std::string(tr("Not set")), ""}); }
    m.country_index = (unfiltered && chosen == nullptr) ? 0 : -1;
    for (const CountryRow& r : rows) {
        if (chosen != nullptr && r.code == chosen->code) { m.country_index = static_cast<int>(m.countries.size()); }
        m.countries.push_back(SelectItem{r.shown, r.code});
    }
    m.country_text = chosen != nullptr ? T(chosen->name) : std::string(tr("Not set"));

    if (chosen != nullptr) {
        // Which plan the country chose, by the name the Region picker shows.
        std::string planName = chosen->bandPlan;
        eachItem(t, FOXAPI_LIST_BAND_PLANS, [&](int, const FoxListItem& it) {
            if (std::string(it.detail) == chosen->bandPlan) { planName = displayPlanName(it.name); }
        });
        m.plan_line = cc::formatText(tr("Band plan: %s"), planName.c_str());
        // ONE KEY, NEVER AN AUTOMATIC SWITCH: the language of the country, when this build carries it and it is not already in force.
        const std::string suggested = cascade::i18n::matchCatalogue(chosen->language);
        if (!suggested.empty() && suggested != inForce) {
            if (const cascade::i18n::Language* l = cascade::i18n::findLanguage(suggested)) {
                m.suggest_code = suggested;
                m.suggest_label = useLanguageLabel(l->name);
            }
        }
    }
    return m;
}

// =================================================================================================================================
// Updates

RowChip updatesChip(bool enabled, const UpdatesFacts& f) {
    RowChip c;
    c.word = std::string(tr("OFF"));
    if (!enabled) { return c; }
    const bool offered = f.packaged ? f.store_offered : f.newer;
    const bool important = f.packaged ? f.store_mandatory : f.critical;
    if (f.packaged && f.store_installing) {
        c.word = std::string(tr("STORE"));
        c.tone = Tone::Warn;
        c.lit = true;
    } else if (offered) {
        c.word = important ? std::string(tr("IMPT")) : std::string(tr("NEW"));
        c.tone = important ? Tone::Err : Tone::Warn;
        c.lit = true;
    } else if (f.pending) {
        c.word = std::string(tr("CHECK"));
    } else if (f.started && f.error.empty()) {
        c.word = std::string(tr("OK"));
    } else {
        c.word = std::string(tr("IDLE"));
    }
    return c;
}

UpdatesStatus updatesStatus(bool enabled, const UpdatesFacts& f) {
    UpdatesStatus s;
    if (!enabled) {
        s.text = std::string(tr("checking is off, so you will not be told about a new version"));
    } else if (f.pending) {
        s.text = std::string(tr("checking..."));
    } else if (f.packaged && f.store_offered) {
        // No version number: the Store does not give one.
        s.text = std::string(tr("the Microsoft Store has an update for this copy"));
        s.tone = f.store_mandatory ? Tone::Err : Tone::Warn;
    } else if (!f.packaged && f.newer) {
        s.text = cc::formatText(tr("%s is available"), f.new_version.c_str());
        s.tone = f.critical ? Tone::Err : Tone::Warn;
    } else if (f.started && f.error.empty()) {
        s.text = std::string(tr("up to date"));
    } else if (!f.error.empty()) {
        // Only here, never as a banner: a failed check is not the user's problem and must not interrupt them.
        s.text = cc::formatText(tr("last check did not complete: %s"), f.error.c_str());
    }
    return s;
}

UpdatesModel readUpdates(const cc::AppConfig& iface, const ReadOptions& o) {
    UpdatesModel m;
    const UpdatesFacts& f = o.updates;
    const bool enabled = iface.updateCheckEnabled;
    m.packaged = f.packaged;
    m.check_at_startup = enabled;
    m.channel_index = iface.updateChannel == "beta" ? 1 : 0;
    m.check_label = f.packaged ? std::string(tr("Ask the Microsoft Store for updates at startup")) : std::string(tr("Check for updates at startup"));
    m.stable_label = std::string(tr("Stable releases"));
    m.beta_label = std::string(tr("Every release (beta)"));
    if (!f.packaged) {
        m.channel_help = std::string(tr("Stable is the release the FoxSDR team recommends to "
                           "everyone; beta is every release the day it is "
                           "published."));
    }
    if (f.packaged) {
        // HONEST ABOUT WHO IS ASKED: Microsoft's Store service, through Windows, identifying the app by the package identity Windows holds.
        m.privacy = std::string(tr("This copy came from the Microsoft Store, so FoxSDR asks the Microsoft Store once "
                      "per launch whether it has a newer version. Nothing is sent to foxsdr.com. Nothing "
                      "is downloaded or installed unless you press the key and confirm the dialog "
                      "Windows shows."));
    } else {
        m.privacy = std::string(tr("Asks foxsdr.com once per launch whether a newer version exists, and sends the "
                      "version you are running and nothing else - no identifier, no cookie. Nothing is "
                      "downloaded or installed unless you press the button. This is not the usage report "
                      "below; the two share nothing."));
    }
    m.version_line = cc::formatText(tr("this build: %s"), f.build_version.c_str());
    if (f.packaged) { m.version_line += "\n" + cc::formatText(tr("package: %s"), f.package_name.c_str()); }
    if (!f.packaged && enabled && f.unseen_messages > 0) { m.team_message = cc::formatText(tr("%d message(s) from the FoxSDR team"), f.unseen_messages); }
    const UpdatesStatus s = updatesStatus(enabled, f);
    m.status_line = s.text;
    m.status_tone = s.tone;
    m.show_again_label = std::string(tr("Show it again"));
    const bool offered = f.packaged ? f.store_offered : f.newer;
    m.show_again = enabled && !f.pending && offered && f.dismissed;
    return m;
}

// =================================================================================================================================
// Serial ports and the GPS row

std::string serialPortsChip(std::size_t count) {
    if (count == 0) { return std::string(tr("NONE")); }
    if (count == 1) { return std::string(tr("1 PORT")); }
    return cc::formatText(tr("%zu PORTS"), count);
}

std::string cleanPortName(const std::string& typed) { return cc::sanitiseSerialPortName(typed); }

std::string gpsKeyLabel(bool listening) { return listening ? std::string(tr("Stop")) : std::string(tr("Read position from GPS")); }

SerialPortsModel readSerialPorts(TableReader& t, const cc::AppConfig& iface, const ReadOptions& o) {
    SerialPortsModel m;
    eachItem(t, FOXAPI_LIST_SERIAL_PORTS, [&](int, const FoxListItem& it) { m.ports.push_back(it.name); });
    m.refresh_label = std::string(tr("Refresh"));
    m.column_label = std::string(tr("Port"));
    m.gps_heading = std::string(tr("GPS receiver"));
    m.gps_hint = std::string(tr("or read it from a GPS receiver on a serial port:"));
    m.intro = std::string(tr("Every serial port this machine has right now - the same ports the "
                "GPS row below offers, gathered in one place rather than left "
                "findable only from the rail's Radar section."));
    // Until the scan's answer is in nothing is said, so a machine WITH ports is not told it has none.
    if (m.ports.empty() && !o.gps_ports_waiting) {
        m.empty_line = std::string(tr("No serial ports found. If a device is plugged in, check Device "
                         "Manager under \"Ports (COM & LPT)\" - if it is not listed there "
                         "either, this application cannot see it yet."));
    }
    m.gps_note = std::string(tr("Sets the receiver's position - what every range and bearing on the "
                   "map and the radar scope is measured from. The same control also "
                   "appears on the rail's Radar section (while there is no position), "
                   "the scope's empty state, and every map page's bar."));

    GpsRow& g = m.gps;
    const std::string port = o.gps_draft_port.empty() ? iface.gpsPort : o.gps_draft_port;
    for (const std::string& name : m.ports) { g.ports.push_back(SelectItem{name, name}); }
    if (!port.empty()) {
        for (std::size_t i = 0; i < g.ports.size(); ++i) {
            if (g.ports[i].id == port) { g.port_index = static_cast<int>(i); }
        }
        if (g.port_index < 0) {
            g.ports.push_back(SelectItem{port, port});   // a port the machine does not list (a typed name, a virtual port) is still the one chosen
            g.port_index = static_cast<int>(g.ports.size()) - 1;
        }
    }
    for (int i = 0; i < kSerialBaudCount; ++i) {
        const std::string text = std::to_string(cc::kSerialBaudRates[i]);
        g.bauds.push_back(SelectItem{text, text});
        if (cc::kSerialBaudRates[i] == iface.gpsBaud) { g.baud_index = i; }
    }
    if (g.baud_index == 1 && iface.gpsBaud != cc::kSerialBaudRates[1]) { g.baud_index = -1; }
    // THE READER, as the row draws it: the GPS list's one entry (state, seconds, satellites, baud; ACTIVE listening, FAULT failed or timed out).
    FoxListItem it{};
    it.structSize = sizeof(it);
    if (t.readList(FOXAPI_LIST_GPS, 0, &it) >= 1) {
        g.state = static_cast<int>(it.value[0]);
        g.listening = (it.flags & FOXAPI_ITEM_ACTIVE) != 0u;
        g.status = it.detail;
        g.status_tone = (it.flags & FOXAPI_ITEM_FAULT) != 0u ? Tone::Warn : Tone::Neutral;
        const int sats = static_cast<int>(it.value[2]);
        if (sats >= 0) { g.satellites = std::to_string(sats); }
    }
    g.key_label = gpsKeyLabel(g.listening);
    g.key_enabled = g.listening || !port.empty();
    if (!g.listening) {
        g.key_tooltip = port.empty() ? std::string(tr("Name the port the GPS receiver is on first."))
                                     : cc::formatText(tr("Opens the port, waits up to %.0f s for a fix, sets the receiver's\n"
                                                         "position from it exactly as if it had been typed, and closes the\n"
                                                         "port again. The position is never written to the diagnostic log."),
                                                      cc::kDefaultGpsTimeoutS);
    }
    g.port_tooltip = std::string(tr("The serial port the GPS receiver is on - COM3, or /dev/ttyUSB0.\n"
                       "Press the arrow to pick from the ports this machine has."));
    g.baud_tooltip = std::string(tr("The receiver's serial rate. Nearly everything ships at 9600;\n"
                       "old pucks use 4800. Wrong, and the status line will say\n"
                       "nothing readable arrived."));
    g.refusal = factOf(t, "fact.status.gpsRefusal");
    return m;
}

// =================================================================================================================================
// Key bindings (the host copy of gui/key_bindings.*; tests/test_slint_host_system.cpp reads the source and holds them equal)

namespace {

constexpr KeyActionInfo kActions[kKeyActionCount] = {
    {"startStop", FOX_TR_NOOP("Start / stop the receiver"), FOX_TR_NOOP("Receiver"), "Ctrl+F2"},
    {"mute", FOX_TR_NOOP("Mute the sound"), FOX_TR_NOOP("Receiver"), "M"},
    {"volumeUp", FOX_TR_NOOP("Volume up"), FOX_TR_NOOP("Receiver"), "Plus"},
    {"volumeDown", FOX_TR_NOOP("Volume down"), FOX_TR_NOOP("Receiver"), "Minus"},
    {"squelchUp", FOX_TR_NOOP("Squelch up"), FOX_TR_NOOP("Receiver"), "Ctrl+Shift+PageUp"},
    {"squelchDown", FOX_TR_NOOP("Squelch down"), FOX_TR_NOOP("Receiver"), "Ctrl+Shift+PageDown"},
    {"modeNFM", FOX_TR_NOOP("Mode: NFM (narrow FM)"), FOX_TR_NOOP("Mode"), "Ctrl+F"},
    {"modeWFM", FOX_TR_NOOP("Mode: WFM (broadcast FM)"), FOX_TR_NOOP("Mode"), "Ctrl+Shift+F"},
    {"modeAM", FOX_TR_NOOP("Mode: AM"), FOX_TR_NOOP("Mode"), "Ctrl+A"},
    {"modeDSB", FOX_TR_NOOP("Mode: DSB"), FOX_TR_NOOP("Mode"), "Ctrl+D"},
    {"modeUSB", FOX_TR_NOOP("Mode: USB"), FOX_TR_NOOP("Mode"), "Ctrl+U"},
    {"modeCW", FOX_TR_NOOP("Mode: CW"), FOX_TR_NOOP("Mode"), "Ctrl+C"},
    {"modeLSB", FOX_TR_NOOP("Mode: LSB"), FOX_TR_NOOP("Mode"), "Ctrl+L"},
    {"modeRAW", FOX_TR_NOOP("Mode: RAW"), FOX_TR_NOOP("Mode"), "Ctrl+R"},
    {"tuneStepUp", FOX_TR_NOOP("Tune up one step (10 kHz)"), FOX_TR_NOOP("Tuning"), "Ctrl+Up"},
    {"tuneStepDown", FOX_TR_NOOP("Tune down one step (10 kHz)"), FOX_TR_NOOP("Tuning"), "Ctrl+Down"},
    {"tuneSpanUp", FOX_TR_NOOP("Tune up one screen width"), FOX_TR_NOOP("Tuning"), "Ctrl+PageUp"},
    {"tuneSpanDown", FOX_TR_NOOP("Tune down one screen width"), FOX_TR_NOOP("Tuning"), "Ctrl+PageDown"},
    {"quickTune", FOX_TR_NOOP("Type a frequency"), FOX_TR_NOOP("Tuning"), "T"},
    {"zoomIn", FOX_TR_NOOP("Zoom the spectrum in"), FOX_TR_NOOP("Picture"), "Ctrl+Plus"},
    {"zoomOut", FOX_TR_NOOP("Zoom the spectrum out"), FOX_TR_NOOP("Picture"), "Ctrl+Minus"},
    {"zoomReset", FOX_TR_NOOP("Zoom back to the full span"), FOX_TR_NOOP("Picture"), "Ctrl+Delete"},
    {"record", FOX_TR_NOOP("Record the audio"), FOX_TR_NOOP("Picture"), "Shift+R"},
    {"transmitPtt", FOX_TR_NOOP("Transmit (hold, on the TRANSMIT page)"), FOX_TR_NOOP("Transmit"), "Space"},
    {"screenshot", FOX_TR_NOOP("Save a screenshot"), FOX_TR_NOOP("Picture"), "Ctrl+W"},
    {"fullscreen", FOX_TR_NOOP("Maximise / restore the window"), FOX_TR_NOOP("Picture"), "F11"},
    {"bankSignal", FOX_TR_NOOP("Bank: SIGNAL PATH"), FOX_TR_NOOP("Rail"), "F1"},
    {"bankDecode", FOX_TR_NOOP("Bank: DECODE"), FOX_TR_NOOP("Rail"), "F2"},
    {"bankView", FOX_TR_NOOP("Bank: TOOLS"), FOX_TR_NOOP("Rail"), "F3"},
    {"bankExtend", FOX_TR_NOOP("Bank: EXTEND"), FOX_TR_NOOP("Rail"), "F4"},
    {"bankSystem", FOX_TR_NOOP("Bank: SYSTEM"), FOX_TR_NOOP("Rail"), "F5"},
    {"openSettings", FOX_TR_NOOP("Settings and key bindings"), FOX_TR_NOOP("Rail"), "F7"},
};

// EVERY KEY A CHORD MAY USE, by its canonical name, in the ImGui table's order (the names are the file format).
constexpr const char* kKeyNames[] = {
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
    "Tab", "Left", "Right", "Up", "Down", "PageUp", "PageDown", "Home", "End", "Insert", "Delete", "Backspace", "Space", "Enter", "Escape", "Menu",
    "Apostrophe", "Comma", "Minus", "Plus", "Period", "Slash", "Semicolon", "LeftBracket", "Backslash", "RightBracket", "GraveAccent",
    "CapsLock", "ScrollLock", "NumLock", "PrintScreen", "Pause",
    "Num0", "Num1", "Num2", "Num3", "Num4", "Num5", "Num6", "Num7", "Num8", "Num9",
    "NumDecimal", "NumDivide", "NumMultiply", "NumMinus", "NumPlus", "NumEnter", "NumEqual",
};
struct KeyAlias { const char* alias; const char* canonical; };
constexpr KeyAlias kKeyAliases[] = {
    {"Equal", "Plus"}, {"Esc", "Escape"}, {"Del", "Delete"}, {"Return", "Enter"}, {"LeftArrow", "Left"},
    {"RightArrow", "Right"}, {"UpArrow", "Up"}, {"DownArrow", "Down"}, {"PgUp", "PageUp"}, {"PgDn", "PageDown"},
};

bool equalsIgnoreCase(const std::string& a, const char* b) {
    const std::size_t n = std::strlen(b);
    if (a.size() != n) { return false; }
    for (std::size_t i = 0; i < n; ++i) {
        const char x = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
        const char y = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] - 'A' + 'a') : b[i];
        if (x != y) { return false; }
    }
    return true;
}

// Both ends trimmed of the spaces a hand-edited file is entitled to contain.
std::string trimmed(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) { ++b; }
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) { --e; }
    return s.substr(b, e - b);
}

}  // namespace

const KeyActionInfo& keyAction(int index) { return kActions[std::clamp(index, 0, kKeyActionCount - 1)]; }
int keyActionIndexOf(const std::string& id) {
    for (int i = 0; i < kKeyActionCount; ++i) {
        if (id == kActions[i].id) { return i; }   // EXACT: the id is a file format key
    }
    return -1;
}

std::vector<std::string> chordKeyNames() { return std::vector<std::string>(std::begin(kKeyNames), std::end(kKeyNames)); }

bool chordKeyKnown(const std::string& name, std::string& canonical) {
    if (name.empty()) { return false; }
    for (const char* k : kKeyNames) {
        if (equalsIgnoreCase(name, k)) {
            canonical = k;
            return true;
        }
    }
    for (const KeyAlias& a : kKeyAliases) {
        if (equalsIgnoreCase(name, a.alias)) {
            canonical = a.canonical;
            return true;
        }
    }
    return false;
}

std::string formatChord(const Chord& c) {
    if (!c.bound()) { return "None"; }
    return std::string(c.ctrl ? "Ctrl+" : "") + (c.shift ? "Shift+" : "") + (c.alt ? "Alt+" : "") + c.key;
}

bool parseChord(const std::string& text, Chord& out) {
    const std::string all = trimmed(text);
    out = Chord{};
    if (all.empty() || equalsIgnoreCase(all, "None")) { return true; }
    std::size_t pos = 0;
    bool haveKey = false;
    while (pos <= all.size()) {
        const std::size_t plus = all.find('+', pos);
        const std::string token = trimmed(all.substr(pos, plus == std::string::npos ? std::string::npos : plus - pos));
        if (token.empty()) { return false; }
        if (equalsIgnoreCase(token, "Ctrl") || equalsIgnoreCase(token, "Control")) {
            if (out.ctrl) { return false; }   // stated twice: a typo, not a chord
            out.ctrl = true;
        } else if (equalsIgnoreCase(token, "Shift")) {
            if (out.shift) { return false; }
            out.shift = true;
        } else if (equalsIgnoreCase(token, "Alt")) {
            if (out.alt) { return false; }
            out.alt = true;
        } else {
            std::string canonical;
            if (!chordKeyKnown(token, canonical)) { return false; }
            if (haveKey) { return false; }   // TWO KEYS IS NOT A CHORD
            out.key = canonical;
            haveKey = true;
        }
        if (plus == std::string::npos) { break; }
        pos = plus + 1;
    }
    return haveKey;   // "Ctrl+" is an unfinished chord, not an unbound one
}

KeyTable defaultKeyTable() {
    KeyTable t;
    for (int i = 0; i < kKeyActionCount; ++i) { parseChord(kActions[i].stock, t[static_cast<std::size_t>(i)]); }
    return t;
}

KeyTable keyTableFromConfig(const std::vector<std::string>& lines) {
    KeyTable t = defaultKeyTable();
    // WHICH ACTIONS THIS FILE HAS ALREADY SPOKEN FOR: a second line for one action is a hand-edit that says two things; the first wins.
    bool seen[kKeyActionCount] = {};
    for (const std::string& raw : lines) {
        const std::size_t eq = raw.find('=');
        if (eq == std::string::npos) { continue; }
        const int a = keyActionIndexOf(trimmed(raw.substr(0, eq)));
        if (a < 0 || seen[a]) { continue; }
        Chord c;
        if (!parseChord(raw.substr(eq + 1), c)) { continue; }
        t[static_cast<std::size_t>(a)] = c;
        seen[a] = true;
    }
    return t;
}

std::vector<std::string> keyTableToConfig(const KeyTable& t) {
    const KeyTable def = defaultKeyTable();
    std::vector<std::string> lines;
    for (int i = 0; i < kKeyActionCount; ++i) {
        if (t[static_cast<std::size_t>(i)] == def[static_cast<std::size_t>(i)]) { continue; }
        lines.push_back(std::string(kActions[i].id) + "=" + formatChord(t[static_cast<std::size_t>(i)]));
    }
    return lines;
}

int clashOf(const KeyTable& t, int index) {
    if (index < 0 || index >= kKeyActionCount || !t[static_cast<std::size_t>(index)].bound()) { return -1; }
    for (int j = 0; j < kKeyActionCount; ++j) {
        if (j == index) { continue; }
        if (t[static_cast<std::size_t>(j)].bound() && t[static_cast<std::size_t>(j)] == t[static_cast<std::size_t>(index)]) { return j; }
    }
    return -1;
}

int clashCount(const KeyTable& t) {
    int n = 0;
    for (int i = 0; i < kKeyActionCount; ++i) {
        if (clashOf(t, i) >= 0) { ++n; }
    }
    return n;
}

std::string keyBindingsChip(std::size_t changed) {
    if (changed == 0) { return std::string(tr("STOCK")); }
    return cc::formatText(tr("%zu SET"), changed);
}

KeyBindingsModel readKeyBindings(const cc::AppConfig& iface, const ReadOptions& o) {
    KeyBindingsModel m;
    const KeyTable t = keyTableFromConfig(iface.keyBindings);
    m.help = std::string(tr("Every shortcut the receiver answers. Click a key to change it, then "
               "press the keys you want - Esc leaves it alone, Backspace clears it. "
               "Shortcuts never fire while you are typing in a field."));
    m.reset_label = std::string(tr("Reset to defaults"));
    m.prompt = std::string(tr("press a key"));
    m.footer = std::string(tr("F12 always saves a screenshot, whatever the Screenshot key is set to.\n"
                 "Esc always cancels what is being typed or the full-screen scope."));
    const char* group = nullptr;
    for (int i = 0; i < kKeyActionCount; ++i) {
        const KeyActionInfo& a = kActions[i];
        if (group == nullptr || std::strcmp(group, a.group) != 0) {
            group = a.group;
            KeyGroup g;
            g.heading = tr(a.group);
            m.groups.push_back(std::move(g));
        }
        KeyRow r;
        r.id = a.id;
        r.action = tr(a.name);
        r.chord = formatChord(t[static_cast<std::size_t>(i)]);
        r.capturing = !o.capturing_id.empty() && o.capturing_id == a.id;
        const int other = clashOf(t, i);
        if (other >= 0) { r.clash = cc::formatText(tr("also %s"), tr(kActions[other].name)); }
        m.groups.back().rows.push_back(std::move(r));
    }
    m.action_count = kKeyActionCount;
    m.clash_count = clashCount(t);
    m.changed_count = static_cast<int>(keyTableToConfig(t).size());
    m.changed = m.changed_count > 0;
    return m;
}

// =================================================================================================================================
// Diagnostics, Usage reporting, Beta tester

std::string diagnosticsChip(bool enabled, bool minidump) {
    if (!enabled) { return std::string(tr("OFF")); }
    return minidump ? std::string(tr("ON+DMP")) : std::string(tr("ON"));
}

DiagnosticsModel readDiagnostics(const cc::AppConfig& iface, const ReadOptions& o) {
    DiagnosticsModel m;
    m.explanation = std::string(tr("If FoxSDR crashes or freezes, it writes a report on THIS machine. With "
                      "this switch on, that report is also sent to us the NEXT time you open "
                      "FoxSDR - never from inside the crash itself, because a program that has "
                      "just failed cannot safely use the network."));
    m.record = iface.diagnosticsEnabled;
    m.minidump = iface.diagnosticsMinidump;
    m.what_open = o.what_open;
    m.record_label = std::string(tr("Record crashes and freezes, and send the report next time"));
    m.minidump_label = std::string(tr("Also write a full memory dump beside a crash report"));
    m.what_label = std::string(tr("What exactly is in a report?"));
    m.sent_text = std::string(tr("Sent: the version and build, what failed and where (as a file name and\n"
                    "an offset), every thread's stack in the same form, the recent log lines,\n"
                    "and what the receiver was doing - mode, source, sample rate, radio model\n"
                    "with the serial removed, and the plugins that were loaded.\n"
                    "NEVER sent: a frequency you tuned to, anything decoded, your position,\n"
                    "your name, your machine's name, or the name of any file you opened.\n"
                    "At most five a day, and the same fault only once a day.\n"
                    "Off means off: no report is written and nothing is sent."));
    m.dump_note = std::string(tr("A memory dump can contain file names, window titles and received signal\n"
                    "data. It is written LOCALLY ONLY and is NEVER uploaded under any\n"
                    "setting - if it is ever useful you will be asked for it, and you decide."));
    m.what_text = std::string(tr("version  commit  os  arch  mode  source  sample rate\n"
                    "device open  sdr model (serial stripped)  loaded plugins\n"
                    "faulting stack as module+offset, and every thread on a freeze\n"
                    "the loaded module list with build ids, and the last 256 log lines\n"
                    "NEVER a frequency, never anything decoded, never your position.\n"
                    "See PRIVACY.md for the complete list and what is excluded."));
    m.log_path = o.log_path.empty() ? std::string(tr("Log: off (nothing is being written).")) : cc::formatText(tr("Log: %s"), o.log_path.c_str());
    m.reports_path = cc::formatText(tr("Reports: %s"), o.reports_path.empty() ? tr("(unavailable)") : o.reports_path.c_str());
    // The dialog of a start after an unclean exit: shown when the last run did not end normally AND diagnostics were on.
    if (o.last_run_unclean && iface.diagnosticsEnabled) {
        m.offer = std::string(tr("The previous session ended without shutting down. If a report was "
                    "written, it is on this machine. On this start we TRY to send its "
                    "text - the version, what failed and where, the thread stacks and "
                    "the recent log lines. It is not sent if it repeats one already "
                    "sent, if this machine is over its send limit, or if foxsdr.com "
                    "cannot be reached. Either way the report stays in the reports "
                    "folder below. No memory dump is sent, ever. Settings > "
                    "Diagnostics lists exactly what goes and turns it off."));
    }
    return m;
}

UsageModel readUsage(TableReader& t, const ReadOptions& o) {
    UsageModel m;
    const bool enabled = factOf(t, "fact.telemetry.enabled") == "1";
    m.disclosure = std::string(tr("Anonymous counts only: version, operating system, how long sessions "
                     "run, which modes and plugins get used, which radio model, how many "
                     "times the window froze waiting for the display driver, how many "
                     "times something quietly did not work (a radio that would not open, no "
                     "sound output, a failed update or plugin install), how many times the "
                     "window was slow and which part of it was, and how many times FoxSDR "
                     "recovered from a fault on its own. Never "
                     "frequencies, never anything decoded, never your location, and no IP "
                     "address is recorded."));
    m.send = o.telemetry_pending ? o.telemetry_pending_on : enabled;
    m.send_label = std::string(tr("Send anonymous usage reports"));
    m.what_label = std::string(tr("What exactly is sent?"));
    m.privacy_label = std::string(tr("Privacy policy (foxsdr.com)"));
    m.privacy_url = cc::kPrivacyPolicyUrl;
    if (enabled) {
        m.install_id_line = cc::formatText(tr("Install id: %s"), factOf(t, "fact.telemetry.installId").c_str());
        // TWO MESSAGES, NOT ONE: the launch report and the "still running" beat every five minutes.
        m.state_line = std::string(tr("One report at start-up, describing the previous session - and, while "
                         "FoxSDR is open, a \"still running\" beat every five minutes. This switch "
                         "stops both."));
    } else {
        m.state_line = std::string(tr("Off. Nothing is transmitted."));
    }
    m.payload.push_back(std::string(tr("Each launch: id (random), version, os, arch, launches, crashes, "
                          "display stalls, failure counts, session seconds, sdr model, modes "
                          "used, panels, plugins.")));
    m.payload.push_back(std::string(tr("The crash count is also split by how each unclean exit ended: crashed, "
                          "ended from outside, ended by the system, or not known. Four more counts, "
                          "nothing else.")));
    m.payload.push_back(std::string(tr("Every five minutes while open: id (the same one), version, a beat "
                          "marker.")));
    m.payload.push_back(std::string(tr("See PRIVACY.md for the complete list and what is excluded.")));
    m.what_open = o.what_open;
    m.what_text = joinLines(m.payload);
    return m;
}

std::string testerChip(bool active, bool invalid) {
    if (!active) { return std::string(tr("OFF")); }
    return invalid ? std::string(tr("INVALID")) : std::string(tr("ON"));
}

// A tester's sign-up name is free text on the site and may be empty: an empty name in "Linked to %s" reads as a sentence missing a word.
std::string testerNameOrPlaceholder(const std::string& name) { return name.empty() ? std::string(tr("(no name set)")) : name; }

TesterKind classifyTesterCode(const std::string& pasted, std::string& token) {
    // AN APP TOKEN (40 hex) goes to confirm-by-name; a PORTAL token (32 hex, or the whole manage link) keeps the immediate opt-in this box has always had.
    token = cc::extractAppToken(pasted);
    if (!token.empty()) { return TesterKind::AppToken; }
    token = cc::extractTesterToken(pasted);
    if (!token.empty()) { return TesterKind::PortalToken; }
    token.clear();
    return TesterKind::None;
}

TesterModel readTester(const cc::AppConfig& iface, const ReadOptions& o) {
    TesterModel m;
    const bool haveApp = !iface.testerAppToken.empty();
    const bool haveCode = !iface.testerToken.empty();
    const std::string active = haveApp ? iface.testerAppToken : iface.testerToken;
    m.shown = !active.empty() || o.tester_revealed;
    m.explanation = std::string(tr("Paste the code from your tester link (or the whole link) to report, "
                      "per session, which features, plugins and radios you actually used - "
                      "linked to your own entry on the tester list so it can be marked as "
                      "tested. Pasting a code IS the opt-in; with none, nothing here is "
                      "collected or sent."));
    if (haveApp) {
        // A CONFIRMED LINK: the paste box is for the older portal-token flow, which does not apply once a link has been confirmed by name.
        m.linked_line = cc::formatText(tr("Linked to %s"), testerNameOrPlaceholder(iface.testerAppTokenName).c_str());
    } else if (haveCode) {
        m.code_line = cc::formatText(tr("Code: %s"), cc::maskTesterToken(iface.testerToken).c_str());
        if (iface.testerTokenInvalid) {
            m.invalid_line = std::string(tr("This code is no longer valid, so sending has stopped. The code "
                               "above is kept so you can fix it - paste a current one below."));
        }
    }
    m.field_shown = !haveApp && (!haveCode || iface.testerTokenInvalid);
    m.code_draft = o.tester_draft;
    m.error_line = o.tester_error;
    m.use_label = std::string(tr("Use this code"));
    m.field_label = std::string(tr("Tester code"));
    m.unlink_label = std::string(tr("Unlink"));
    m.remove_label = std::string(tr("Remove code"));
    m.show_label = std::string(tr("Show what is sent"));
    m.hide_label = std::string(tr("Hide what is sent"));
    m.sent_text = std::string(tr("What is sent: the app version, platform and architecture, when a "
                    "session started and how long it ran, which of the features above "
                    "were used, which plugins ran and for how long, and which radio "
                    "kinds were opened. Never a frequency, never anything decoded, "
                    "never your location, and no IP address is recorded."));
    m.sent_open = o.tester_sent_open;
    if (active.empty()) {
        m.sent_preview = std::string(tr("Nothing - no code is set."));
    } else {
        m.sent_preview = o.tester_preview_json;
        if (o.tester_queued > 0) { m.sent_preview += (m.sent_preview.empty() ? "" : "\n") + cc::formatText(tr("%d report(s) queued to retry"), o.tester_queued); }
    }
    return m;
}

// =================================================================================================================================
// The report pages

SendVerdict featureVerdict(const std::string& text, const std::string& contact, const SenderView& sender, std::uint64_t nowEpoch) {
    SendVerdict v;
    const std::string textError = cc::validateFeatureRequestText(text);
    const std::string contactError = cc::validateFeatureRequestContact(contact);
    const bool sending = sender.state == SendState::Sending;
    const bool cooling = !sending && sender.blocked_until > nowEpoch;
    if (sending) {
        v.reason = std::string(tr("sending"));
    } else if (!textError.empty()) {
        v.reason = tr(textError.c_str());
    } else if (!contactError.empty()) {
        v.reason = tr(contactError.c_str());
    } else if (cooling) {
        v.reason = cc::formatText(tr("wait %llus before sending another"), static_cast<unsigned long long>(sender.blocked_until - nowEpoch));
    }
    v.can_send = v.reason.empty();
    return v;
}

SendVerdict problemVerdict(const std::string& kind, const std::string& text, const std::string& contact, const SenderView& sender, std::uint64_t nowEpoch) {
    SendVerdict v;
    const std::string kindError = cc::validateProblemReportKind(kind);
    const std::string textError = cc::validateProblemReportText(text);
    const std::string contactError = cc::validateProblemReportContact(contact);
    const bool sending = sender.state == SendState::Sending;
    const bool cooling = !sending && sender.blocked_until > nowEpoch;
    if (sending) {
        v.reason = std::string(tr("sending"));
    } else if (!kindError.empty()) {
        v.reason = tr(kindError.c_str());
    } else if (!textError.empty()) {
        v.reason = tr(textError.c_str());
    } else if (!contactError.empty()) {
        v.reason = tr(contactError.c_str());
    } else if (cooling) {
        v.reason = cc::formatText(tr("wait %llus before sending another"), static_cast<unsigned long long>(sender.blocked_until - nowEpoch));
    }
    v.can_send = v.reason.empty();
    return v;
}

// TWO WHOLE SENTENCES, not one assembled from fragments, so a translation can order its clauses the way its language does.
std::string featureSentence(bool haveContact) {
    return haveContact ? std::string(tr("Sends your message above, the contact line below it, the FoxSDR version, "
                           "and whether this is running on Windows, Linux or Android, x64 or arm64. "
                           "Nothing else - no identifier, no log, no settings, no frequency."))
                       : std::string(tr("Sends your message above, the FoxSDR version, and whether this is "
                           "running on Windows, Linux or Android, x64 or arm64. Nothing else - no "
                           "identifier, no log, no settings, no frequency."));
}

std::string problemSentence(bool attach, bool haveContact) {
    if (attach && haveContact) {
        return std::string(tr("Sends whether this is a bug or a dislike, your message above, the contact "
                 "line below it, the FoxSDR version, whether this is running on Windows, "
                 "Linux or Android, x64 or arm64, and the diagnostics log attached above - "
                 "untick \"Attach the diagnostics log\" to leave it out, or use \"Show what "
                 "will be sent\" to read it first. No identifier, no crash report."));
    }
    if (attach) {
        return std::string(tr("Sends whether this is a bug or a dislike, your message above, the FoxSDR "
                 "version, whether this is running on Windows, Linux or Android, x64 or "
                 "arm64, and the diagnostics log attached above - untick \"Attach the "
                 "diagnostics log\" to leave it out, or use \"Show what will be sent\" to "
                 "read it first. No identifier, no crash report."));
    }
    if (haveContact) {
        return std::string(tr("Sends whether this is a bug or a dislike, your message above, the contact "
                 "line below it, the FoxSDR version, and whether this is running on "
                 "Windows, Linux or Android, x64 or arm64. Nothing else - no identifier, no "
                 "log, no crash report, no settings, no frequency."));
    }
    return std::string(tr("Sends whether this is a bug or a dislike, your message above, the FoxSDR "
             "version, and whether this is running on Windows, Linux or Android, x64 "
             "or arm64. Nothing else - no identifier, no log, no crash report, no "
             "settings, no frequency."));
}

std::string senderStatusLine(const SenderView& s, bool feature, Tone& tone) {
    tone = Tone::Neutral;
    switch (s.state) {
        case SendState::Idle: return {};
        case SendState::Sending:
            tone = Tone::Warn;
            return std::string(tr("Sending..."));
        case SendState::Sent: {
            tone = Tone::Good;
            std::string line = feature ? std::string(tr("Thank you - your request was sent.")) : std::string(tr("Thank you - your report was sent."));
            // THE OLDER-SITE FALLBACK, said plainly: the site did not accept the diagnostics field, so it was dropped and the report retried without it.
            if (!feature && s.diagnostics_dropped) { line += "\n" + std::string(tr("The diagnostics log could not be sent with this report.")); }
            return line;
        }
        case SendState::Failed:
            tone = Tone::Err;
            return tr(s.failure.c_str());
        case SendState::CoolingDown: {
            tone = Tone::Warn;
            std::string msg = s.failure;
            if (msg.empty()) {
                msg = feature ? FOX_TR_NOOP("The server asked us to wait before sending another request.")
                              : FOX_TR_NOOP("The server asked us to wait before sending another report.");
            }
            return tr(msg.c_str());
        }
    }
    return {};
}

// The lines of the attachment a report's reader leans on - the last run's state, the signature ids of the reports on this machine and the words of what the
// program recovered from (app_window_report.cpp censusDiagnosticsLines reads the same three).
std::vector<std::string> evidenceLines(const std::string& text) {
    std::vector<std::string> out;
    for (const char* name : {"last-run-unclean", "recent-faults", "recovered"}) {
        const std::string key = std::string("\n") + name + ": ";
        const std::size_t at = text.find(key);
        if (at == std::string::npos) { continue; }
        std::size_t eol = text.find('\n', at + 1);
        if (eol == std::string::npos) { eol = text.size(); }
        out.push_back(text.substr(at + 1, eol - at - 1));
    }
    return out;
}

namespace {

std::string countText(const std::string& text) {
    return cc::formatText(tr("%zu / %zu characters"), cc::featureRequestTextCharCount(text), cc::kFeatureRequestMaxChars);
}
std::string refusalOf(const SendVerdict& v) { return v.reason.empty() ? std::string() : "(" + v.reason + ")"; }

}  // namespace

ReportPageModel readFeatureRequest(const ReportInputs& in) {
    ReportPageModel m;
    m.shown = in.shown;
    m.kind = 0;
    m.title = std::string(tr("FEATURE REQUEST"));
    m.prompt = std::string(tr("What would you like FoxSDR to do?"));
    m.text = in.text;
    m.contact = in.contact;
    m.counter = countText(in.text);
    m.contact_label = std::string(tr("Email or callsign (optional)"));
    m.send_label = std::string(tr("SEND"));
    // A callsign alone cannot be written back to - said plainly, but never blocking SEND: the field stays entirely optional.
    if (!cc::featureRequestContactHasEmailAddress(in.contact)) { m.no_email_note = std::string(tr("Without an email address we cannot reply or tell you when this is added.")); }
    const bool haveContact = cc::validateFeatureRequestContact(in.contact).empty() && cc::featureRequestContactCharCount(in.contact) > 0;
    m.attach_note = featureSentence(haveContact);
    const SendVerdict v = featureVerdict(in.text, in.contact, in.sender, in.now_epoch);
    m.send_enabled = v.can_send;
    m.refusal = refusalOf(v);
    m.sending = in.sender.state == SendState::Sending;
    m.sent_line = senderStatusLine(in.sender, true, m.sent_tone);
    return m;
}

ReportPageModel readProblemReport(const ReportInputs& in) {
    ReportPageModel m;
    m.shown = in.shown;
    m.kind = 1;
    m.title = std::string(tr("REPORT A BUG / DISLIKE"));
    m.kind_question = std::string(tr("What are you reporting?"));
    m.problem_kinds.push_back(SelectItem{std::string(tr("Something is broken (bug)")), cc::kProblemKindBug});
    m.problem_kinds.push_back(SelectItem{std::string(tr("Something I dislike")), cc::kProblemKindDislike});
    m.problem_kind_index = in.problem_kind == cc::kProblemKindBug ? 0 : in.problem_kind == cc::kProblemKindDislike ? 1 : -1;
    m.prompt = in.problem_kind == cc::kProblemKindDislike ? std::string(tr("What do you dislike, and what would you rather it did?"))
                                                          : std::string(tr("What went wrong, and what were you doing when it did?"));
    m.text = in.text;
    m.contact = in.contact;
    m.counter = countText(in.text);
    m.contact_label = std::string(tr("Email or callsign (optional)"));
    m.send_label = std::string(tr("SEND"));
    m.attach_label = std::string(tr("Attach the diagnostics log"));
    m.preview_label = std::string(tr("Show what will be sent"));
    if (const auto pasted = cc::detectPastedReport(in.text)) { m.recognised_line = cc::formatText(tr("A report was recognised and attached as %s"), pasted->reportClass.c_str()); }
    if (!cc::problemReportContactHasEmailAddress(in.contact)) {
        m.no_email_note = std::string(tr("Without an email address we cannot reply or tell you when this is fixed or ready."));
    }
    m.attach = in.attach;
    m.probe_label = std::string(tr("Attach the SDRplay diagnostic too"));
    if (in.attach) {
        m.probe_shown = !in.probe_report.empty();
        m.probe_on = m.probe_shown && in.attach_probe;
        m.preview_text = m.probe_on ? cc::appendProbeToDiagnosticsForReport(in.diagnostics, in.probe_report) : in.diagnostics;
        m.evidence = evidenceLines(in.diagnostics);
        m.preview_open = in.preview_open;
        if (in.preview_open && !in.local_only_why.empty()) {
            // WHAT `upload local-only` IN THAT TEXT MEANS, in a sentence the person can read in their own language and with the folder the file is in.
            const std::string where = cc::formatText(tr("The file is in the reports folder: %s"), in.reports_dir.empty() ? tr("(unavailable)") : in.reports_dir.c_str());
            m.local_only_line = cc::formatText("%s %s", tr(in.local_only_why.c_str()), where.c_str());
        }
    }
    const bool haveContact = cc::validateProblemReportContact(in.contact).empty() && cc::featureRequestContactCharCount(in.contact) > 0;
    m.attach_note = problemSentence(in.attach, haveContact);
    const SendVerdict v = problemVerdict(in.problem_kind, in.text, in.contact, in.sender, in.now_epoch);
    m.send_enabled = v.can_send;
    m.refusal = refusalOf(v);
    m.sending = in.sender.state == SendState::Sending;
    m.sent_line = senderStatusLine(in.sender, false, m.sent_tone);
    return m;
}

// =================================================================================================================================
// The whole read

SystemModels readSystem(TableReader& t, const cc::AppConfig& iface, const FoxReceiverState& st, const ReadOptions& o) {
    SystemModels all;
    all.display = readDisplay(t, iface, st, o);
    all.language = readLanguage(t, iface, o);
    all.updates = readUpdates(iface, o);
    all.serial = readSerialPorts(t, iface, o);
    all.keys = readKeyBindings(iface, o);
    all.diagnostics = readDiagnostics(iface, o);
    all.usage = readUsage(t, o);
    all.tester = readTester(iface, o);
    all.feature = readFeatureRequest(o.feature);
    all.bug = readProblemReport(o.bug);

    // The rail's rows (each drawer's benchSection/railSection arguments: the chip word, the lamp).
    // Display: the band plan overlay is the one thing that is on or off; the lamp is the overlay ACTUALLY DRAWING (asked for, no load error, a plan with bands).
    {
        const bool drawing = all.display.band_plan && factOf(t, "fact.status.bandPlanError").empty() && listSize(t, FOXAPI_LIST_BAND_PLAN) > 0;
        RowChip& c = all.chips[kRowDisplay];
        c.word = iface.bandPlanOverlay ? std::string(tr("PLAN")) : std::string(tr("PLAIN"));
        c.lit = drawing;
        c.tone = drawing ? Tone::Good : Tone::Neutral;
    }
    {
        RowChip& c = all.chips[kRowLanguage];
        c.word = cc::upperLegend(cascade::i18n::current());
    }
    all.chips[kRowUpdates] = updatesChip(iface.updateCheckEnabled, o.updates);
    {
        RowChip& c = all.chips[kRowSerial];
        c.word = serialPortsChip(all.serial.ports.size());
        c.lit = all.serial.gps.listening;
        c.tone = c.lit ? Tone::Good : Tone::Neutral;
    }
    {
        RowChip& c = all.chips[kRowKeys];
        c.word = keyBindingsChip(static_cast<std::size_t>(all.keys.changed_count));
        c.lit = all.keys.changed;
    }
    {
        RowChip& c = all.chips[kRowDiagnostics];
        c.word = diagnosticsChip(iface.diagnosticsEnabled, iface.diagnosticsMinidump);
        c.lit = iface.diagnosticsEnabled;
        c.tone = c.lit ? Tone::Good : Tone::Neutral;
    }
    {
        const bool enabled = factOf(t, "fact.telemetry.enabled") == "1";
        RowChip& c = all.chips[kRowUsage];
        c.word = enabled ? std::string(tr("ON")) : std::string(tr("OFF"));
        c.lit = enabled;
        c.tone = enabled ? Tone::Good : Tone::Neutral;
    }
    {
        const bool active = !iface.testerToken.empty() || !iface.testerAppToken.empty();
        RowChip& c = all.chips[kRowTester];
        c.word = testerChip(active, iface.testerTokenInvalid);
        c.lit = active;
        c.tone = iface.testerTokenInvalid ? Tone::Err : (active ? Tone::Good : Tone::Neutral);
    }
    return all;
}

// =================================================================================================================================
// The effects

Effect applyDisplayScale(const cc::AppConfig& cur, int index) {
    const std::string choice = scaleChoiceOf(index);
    if (choice.empty()) { return {}; }
    cc::AppConfig half = cur;
    half.interfaceScale = choice;
    Effect e = saved(choice != cur.interfaceScale, half);
    if (e.save) {
        e.apply = kApplyScale;
        e.value = choice;
    }
    return e;
}

Effect applyDisplayTheme(const cc::AppConfig& cur, int index) {
    if (index < 0 || index >= kThemeCount) { return {}; }
    // Picking the look already in force is no pick (the drawer guards with `themeIndex != now`).
    if (index == themeIndexOf(cur.uiTheme) && cur.uiTheme == kThemes[index].key) { return {}; }
    const ThemePreset& p = kThemes[index];
    cc::AppConfig half = cur;
    half.uiTheme = p.key;
    // THE PRESET'S COUNTER AND READINGS COME WITH IT (pickTheme); the person can change each of them afterwards.
    half.counterScale = p.counterScale >= 1.5f ? 2 : 1;
    half.counterSwitches = p.switches;
    half.readingsScale = std::clamp(p.readings, 1.0f, 3.0f);
    half.tunerDisplayStyle = p.face;
    Effect e = saved(true, half);
    e.apply = kApplySkin;
    e.value = p.key;
    return e;
}

namespace {
Effect dbRange(const cc::AppConfig& cur, const FoxReceiverState& st, float floorEdit, float ceilingEdit, bool floorMoved) {
    // The ceiling stays at least kMinRangeSpanDb above the floor by pushing back the end the user is actually dragging (core::clampManualRange).
    const cc::DbPair p = cc::clampManualRange(floorEdit, ceilingEdit, floorMoved);
    cc::AppConfig half = cur;
    half.dbMin = p.floorDb;
    half.dbMax = p.ceilingDb;
    half.waterfallRangeMode = "manual";   // dragging either end is choosing a range
    Effect e = saved(true, half);
    e.apply = kApplyDbRange;
    e.commands.push_back(make(cmd::makeNum(FOXAPI_OP_SET_DISPLAY_RANGE, static_cast<double>(p.floorDb), static_cast<double>(p.ceilingDb))));
    (void)st;
    return e;
}
}  // namespace

Effect applyDisplayDbMin(const cc::AppConfig& cur, const FoxReceiverState& st, float value) {
    return dbRange(cur, st, value, static_cast<float>(st.dbMax), true);
}
Effect applyDisplayDbMax(const cc::AppConfig& cur, const FoxReceiverState& st, float value) {
    return dbRange(cur, st, static_cast<float>(st.dbMin), value, false);
}

Effect applyDisplayFreqDisplay(const cc::AppConfig& cur, int index) {
    if (index < 0 || index >= cascade::gui::kTunerStyleCount) { return {}; }
    cc::AppConfig half = cur;
    half.tunerDisplayStyle = cascade::gui::kTunerStyleNames[index];
    return saved(half.tunerDisplayStyle != cur.tunerDisplayStyle, half);
}
Effect applyDisplayFigures(const cc::AppConfig& cur, int index) {
    if (index < 0 || index > 1) { return {}; }
    cc::AppConfig half = cur;
    half.counterScale = index == 1 ? 2 : 1;
    return saved(half.counterScale != cur.counterScale, half);
}
Effect applyDisplayTunerSwitches(const cc::AppConfig& cur, bool on) {
    cc::AppConfig half = cur;
    half.counterSwitches = on;
    return saved(on != cur.counterSwitches, half);
}
Effect applyDisplayEnlarge(const cc::AppConfig& cur, bool on) {
    cc::AppConfig half = cur;
    half.readingsScale = on ? kEnlargedReadings : 1.0f;
    return saved(on != (cur.readingsScale > 1.0f), half);
}
Effect applyDisplayIconPx(const cc::AppConfig& cur, float px) {
    if (!std::isfinite(px)) { return {}; }
    cc::AppConfig half = cur;
    half.aircraftIconPx = clampIconPx(static_cast<int>(std::lround(px)));
    // drawTrailWidthControl: shrinking the icons pulls a wider trail down with them rather than leaving a trail broader than the aircraft drawing it.
    half.mapTrailWidthPx = std::min(half.mapTrailWidthPx, wingspanPx(half.aircraftIconPx));
    return saved(half.aircraftIconPx != cur.aircraftIconPx || half.mapTrailWidthPx != cur.mapTrailWidthPx, half);
}
Effect applyDisplayTrailPx(const cc::AppConfig& cur, float px) {
    if (!std::isfinite(px)) { return {}; }
    cc::AppConfig half = cur;
    half.mapTrailWidthPx = std::clamp(static_cast<int>(std::lround(px)), kTrailWidthMinPx, wingspanPx(cur.aircraftIconPx));
    return saved(half.mapTrailWidthPx != cur.mapTrailWidthPx, half);
}
Effect applyDisplayBandPlan(const cc::AppConfig& cur, bool on) {
    cc::AppConfig half = cur;
    half.bandPlanOverlay = on;
    return saved(on != cur.bandPlanOverlay, half);
}
Effect applyDisplayTrailStyle(const cc::AppConfig& cur, int index) {
    if (index < 0 || index > 1) { return {}; }
    cc::AppConfig half = cur;
    half.mapTrailStyle = index;
    return saved(half.mapTrailStyle != cur.mapTrailStyle, half);
}
Effect applyDisplayBandSize(const cc::AppConfig& cur, int index) {
    if (index < 0 || index > 2) { return {}; }
    cc::AppConfig half = cur;
    half.bandPlanSize = kBandPlanSizeKeys[index];
    return saved(half.bandPlanSize != cur.bandPlanSize, half);
}
Effect applyDisplayBandColour(const cc::AppConfig& cur, int index) {
    if (index < 0 || index > 2) { return {}; }
    cc::AppConfig half = cur;
    half.bandPlanPalette = kBandPlanPaletteKeys[index];
    return saved(half.bandPlanPalette != cur.bandPlanPalette, half);
}
Effect applyDisplayRegion(const DisplayModel& m, int index) {
    // The pick is the plan's id, and nothing for the plan already in force (pickFromList returns `hit && !selected`).
    if (!inRange(index, m.regions.size()) || index == m.region_index) { return {}; }
    Effect e;
    e.commands.push_back(make(cmd::makeText(FOXAPI_OP_SET_BAND_PLAN, m.regions[static_cast<std::size_t>(index)].id)));
    return e;
}

Effect applyLanguageSelected(const cc::AppConfig& cur, int index) {
    const std::vector<cascade::i18n::Language>& langs = cascade::i18n::languages();
    std::string setting;
    if (index == 0) {
        setting = "auto";
    } else if (inRange(index - 1, langs.size())) {
        const std::string& code = langs[static_cast<std::size_t>(index - 1)].code;
        // Listed, so nobody wonders where their language went, but not selectable: choosing it would only ever show English.
        if (!cascade::i18n::drawable(code)) { return {}; }
        setting = code;
    } else {
        return {};
    }
    const std::string now = cur.language.empty() ? std::string("auto") : cur.language;
    // The row in force is no pick (the drawer returns `hit && !selected && drawable`): Automatic while "auto" follows the system, else the language chosen.
    const bool followsSystem = foldKey(now) == "auto";
    const std::string chosen = followsSystem ? std::string() : cascade::i18n::resolveFor(now, std::string());
    if (index == 0 ? followsSystem : (!followsSystem && chosen == setting)) { return {}; }
    cc::AppConfig half = cur;
    half.language = setting;
    Effect e = saved(true, half);
    e.apply = kApplyLanguage;
    e.value = setting;
    return e;
}

Effect applyLanguageSuggest(const cc::AppConfig& cur) {
    const cc::Country* chosen = cc::findCountry(cur.country);
    if (chosen == nullptr) { return {}; }
    const std::string suggested = cascade::i18n::matchCatalogue(chosen->language);
    if (suggested.empty() || suggested == cascade::i18n::current() || cascade::i18n::findLanguage(suggested) == nullptr) { return {}; }
    if (cur.language == suggested) { return {}; }
    cc::AppConfig half = cur;
    half.language = suggested;
    Effect e = saved(true, half);
    e.apply = kApplyLanguage;
    e.value = suggested;
    return e;
}

Effect applyLanguageTranslationReport(const std::string& currentDraft) {
    // The prefix is English on purpose: it is read by whoever triages the site's reports, not by the user. A draft already in the box is theirs and is left alone.
    Effect e;
    e.local = Local::OpenReport;
    e.number = 1;
    e.text = currentDraft.empty() ? "Translation (" + cascade::i18n::current() + "): " : currentDraft;
    return e;
}

Effect applyCountryFilter(const std::string& text) {
    Effect e;
    e.local = Local::CountryFilter;
    e.text = text;
    return e;
}

Effect applyCountrySelected(TableReader& t, const cc::AppConfig& cur, const std::string& filter, int index) {
    const bool unfiltered = foldKey(filter).empty();
    const std::vector<CountryRow> rows = countryRows(filter);
    const cc::Country* chosen = cc::findCountry(cur.country);
    if (unfiltered && index == 0) {
        // "Not set" forgets the country and leaves the band plan alone: the plan in force was chosen for a reason.
        if (chosen == nullptr) { return {}; }
        cc::AppConfig half = cur;
        half.country.clear();
        return saved(true, half);
    }
    const int row = unfiltered ? index - 1 : index;
    if (!inRange(row, rows.size())) { return {}; }
    const cc::Country* pick = cc::findCountry(rows[static_cast<std::size_t>(row)].code);
    if (pick == nullptr || pick == chosen) { return {}; }
    cc::AppConfig half = cur;
    half.country = pick->code;
    Effect e = saved(true, half);
    // The same command the Region picker submits, when the plan the country implies is not the one in force.
    std::string activeId;
    eachItem(t, FOXAPI_LIST_BAND_PLANS, [&](int, const FoxListItem& it) {
        if ((it.flags & FOXAPI_ITEM_ACTIVE) != 0u) { activeId = it.detail; }
    });
    if (activeId != pick->bandPlan) { e.commands.push_back(make(cmd::makeText(FOXAPI_OP_SET_BAND_PLAN, pick->bandPlan))); }
    return e;
}

Effect applyUpdatesCheck(const cc::AppConfig& cur, bool on) {
    cc::AppConfig half = cur;
    half.updateCheckEnabled = on;
    Effect e = saved(on != cur.updateCheckEnabled, half);
    if (e.save) { e.apply = kApplyUpdateCheck; }
    return e;
}

Effect applyUpdatesChannel(const cc::AppConfig& cur, int index, bool packaged) {
    // Not for a Store copy: its updates are the Store's.
    if (packaged || index < 0 || index > 1) { return {}; }
    cc::AppConfig half = cur;
    half.updateChannel = index == 1 ? "beta" : "stable";
    const bool was = cur.updateChannel == "beta";
    Effect e = saved((index == 1) != was, half);
    // A CHANGE ASKS AGAIN, as a re-tick does: the answer on screen was for the other channel.
    if (e.save && cur.updateCheckEnabled) { e.apply = kApplyUpdateCheck; }
    return e;
}

Effect applyUpdatesShowAgain() {
    Effect e;
    e.apply = kApplyUpdateShowAgain;
    return e;
}

Effect applyUpdatesTeamMessage() {
    Effect e;
    e.local = Local::OpenTeamMessages;
    return e;
}

Command opSerialRefresh() { return make(cmd::make(FOXAPI_OP_SCAN_SERIAL_PORTS)); }
Effect applySerialRefresh() {
    Effect e;
    e.commands.push_back(opSerialRefresh());
    return e;
}

namespace {
// A port or baud edit forgets the last result and the refusal: two FOXAPP_OP_CLEAR_STATUS through the session (clearGpsResult).
void clearGps(Effect& e) {
    e.commands.push_back(make(cmd::makeInt(FOXAPP_OP_CLEAR_STATUS, cmd::FOXAPP_STATUS_GPS_REFUSAL)));
    e.commands.push_back(make(cmd::makeInt(FOXAPP_OP_CLEAR_STATUS, cmd::FOXAPP_STATUS_GPS_RESULT)));
}
}  // namespace

Effect applyGpsPort(const cc::AppConfig& cur, const GpsRow& row, int index) {
    // The port and baud cannot change under a listen in progress: the reader was started with a copy of both.
    if (row.listening || !inRange(index, row.ports.size())) { return {}; }
    cc::AppConfig half = cur;
    half.gpsPort = cc::sanitiseSerialPortName(row.ports[static_cast<std::size_t>(index)].id);
    Effect e = saved(half.gpsPort != cur.gpsPort, half);
    clearGps(e);
    return e;
}

Effect applyGpsBaud(const cc::AppConfig& cur, const GpsRow& row, int index) {
    if (row.listening || index < 0 || index >= kSerialBaudCount) { return {}; }
    cc::AppConfig half = cur;
    half.gpsBaud = cc::kSerialBaudRates[index];   // the sanctioned list and no other: a rate the port cannot be set to cannot be persisted either
    Effect e = saved(half.gpsBaud != cur.gpsBaud, half);
    clearGps(e);
    return e;
}

Effect applyGpsToggle(const cc::AppConfig& cur, const GpsRow& row) {
    Effect e;
    if (row.listening) {
        e.commands.push_back(make(cmd::makeInt(FOXAPI_OP_GPS, 0)));
        return e;
    }
    // Disabled with no port named rather than starting and failing.
    if (cur.gpsPort.empty()) { return e; }
    e.commands.push_back(make(cmd::makeText(FOXAPI_OP_GPS, cur.gpsPort, 1, 0, static_cast<double>(cur.gpsBaud))));
    return e;
}

namespace {
Effect keysSave(const cc::AppConfig& cur, const KeyTable& table) {
    cc::AppConfig half = cur;
    half.keyBindings = keyTableToConfig(table);
    Effect e = saved(half.keyBindings != cur.keyBindings, half);
    e.local = Local::CaptureCancel;   // the capture ends whatever came of it
    return e;
}
}  // namespace

Effect applyKeysReset(const cc::AppConfig& cur) { return keysSave(cur, defaultKeyTable()); }

Effect applyKeysCapture(const std::string& capturingNow, const std::string& id) {
    if (keyActionIndexOf(id) < 0) { return {}; }
    Effect e;
    e.text = id;
    // A second click on the row that is already listening puts it back rather than listening twice.
    e.local = capturingNow == id ? Local::CaptureCancel : Local::CaptureStart;
    return e;
}

Effect applyKeysCancel() {
    Effect e;
    e.local = Local::CaptureCancel;
    return e;
}

Effect applyKeysCaptured(const cc::AppConfig& cur, const std::string& id, const std::string& chordText) {
    const int a = keyActionIndexOf(id);
    Chord c;
    if (a < 0 || !parseChord(chordText, c) || !c.bound()) { return applyKeysCancel(); }
    KeyTable table = keyTableFromConfig(cur.keyBindings);
    // ESCAPE AND BACKSPACE ARE ANSWERED FIRST AND SO CANNOT BE BOUND (pollKeyCapture): Esc means "leave it alone", Backspace "clear the field".
    if (c.key == "Escape") { return applyKeysCancel(); }
    if (c.key == "Backspace") {
        table[static_cast<std::size_t>(a)] = Chord{};
    } else {
        table[static_cast<std::size_t>(a)] = c;   // a clash is shown, never refused
    }
    return keysSave(cur, table);
}

Effect applyKeysClear(const cc::AppConfig& cur, const std::string& id) {
    const int a = keyActionIndexOf(id);
    if (a < 0) { return {}; }
    KeyTable table = keyTableFromConfig(cur.keyBindings);
    table[static_cast<std::size_t>(a)] = Chord{};
    Effect e = keysSave(cur, table);
    e.local = Local::None;
    return e;
}

Effect applyDiagRecord(const cc::AppConfig& cur, bool on) {
    cc::AppConfig half = cur;
    half.diagnosticsEnabled = on;
    Effect e = saved(on != cur.diagnosticsEnabled, half);
    if (e.save) { e.apply = kApplyDiagnostics; }
    return e;
}
Effect applyDiagMinidump(const cc::AppConfig& cur, bool on) {
    cc::AppConfig half = cur;
    half.diagnosticsMinidump = on;
    Effect e = saved(on != cur.diagnosticsMinidump, half);
    if (e.save) { e.apply = kApplyDiagnostics; }
    return e;
}
Effect applyDiagWhat(bool on) {
    Effect e;
    e.local = Local::ToggleWhat;
    e.flag = on;
    return e;
}
Effect applyDiagOpenLog(const std::string& logPath) {
    Effect e;
    if (logPath.empty()) { return e; }   // no log is being written: nothing to show
    e.local = Local::OpenLog;
    e.text = logPath;
    return e;
}
Effect applyDiagOpenReports(const std::string& reportsPath) {
    Effect e;
    if (reportsPath.empty()) { return e; }
    e.local = Local::OpenReports;
    e.text = reportsPath;
    return e;
}

Effect applyUsageSend(bool on) {
    // A COMMAND: the id-minting, the id-forgetting and the heartbeat reconfigure all move into FOXAPP_OP_TELEMETRY_CONSENT's handler. The desktop's own op, not the
    // API's FOXAPI_OP_TELEMETRY_ENABLE, which is refused from everyone (consent is given here, never remotely).
    Effect e;
    e.commands.push_back(make(cmd::makeInt(FOXAPP_OP_TELEMETRY_CONSENT, on ? 1 : 0)));
    return e;
}
Effect applyUsageWhat(bool on) {
    Effect e;
    e.local = Local::ToggleWhat;
    e.flag = on;
    return e;
}
Effect applyUsagePrivacy() {
    Effect e;
    e.local = Local::OpenUrl;
    e.text = cc::kPrivacyPolicyUrl;
    return e;
}

Effect applyTesterUnlink(const cc::AppConfig& cur) {
    cc::AppConfig half = cur;
    half.testerAppToken.clear();
    half.testerAppTokenName.clear();
    return saved(half.testerAppToken != cur.testerAppToken || half.testerAppTokenName != cur.testerAppTokenName, half);
}

Effect applyTesterRemoveCode(const cc::AppConfig& cur) {
    // setTesterToken(""): a portal-token removal is a full identity reset - any app token linked earlier goes with it.
    cc::AppConfig half = cur;
    half.testerToken.clear();
    half.testerTokenInvalid = false;
    half.testerAppToken.clear();
    half.testerAppTokenName.clear();
    Effect e = saved(true, half);
    e.local = Local::TesterDraft;   // the draft box and its error are cleared
    e.flag = true;
    return e;
}

Effect applyTesterCodeCommitted(const std::string& text) {
    Effect e;
    e.local = Local::TesterDraft;
    e.text = text;
    return e;
}

Effect applyTesterUse(const cc::AppConfig& cur, const std::string& draft) {
    std::string token;
    const TesterKind kind = classifyTesterCode(draft, token);
    Effect e;
    if (kind == TesterKind::AppToken) {
        // An app token is never stored directly: it goes through confirm-by-name first. The draft is cleared.
        e.local = Local::TesterResolve;
        e.text = token;
        e.flag = true;
        return e;
    }
    if (kind == TesterKind::PortalToken) {
        cc::AppConfig half = cur;
        half.testerToken = token;
        half.testerTokenInvalid = false;
        half.testerAppToken.clear();      // setTesterToken: the portal-token identity supersedes any app token
        half.testerAppTokenName.clear();
        e = saved(true, half);
        e.local = Local::TesterDraft;
        e.flag = true;
        return e;
    }
    e.local = Local::TesterError;
    e.text = std::string(tr("that does not look like a tester code or link"));
    return e;
}

Effect applyTesterShowSent(bool on) {
    Effect e;
    e.local = Local::ToggleTesterSent;
    e.flag = on;
    return e;
}

Effect applyReportOpen(int kind) {
    if (kind != 0 && kind != 1) { return {}; }
    Effect e;
    e.local = Local::OpenReport;
    e.number = kind;
    return e;
}
Effect applyReportText(int kind, const std::string& text) {
    Effect e;
    e.local = Local::ReportText;
    e.number = kind;
    e.text = text;
    return e;
}
Effect applyReportContact(int kind, const std::string& contact) {
    Effect e;
    e.local = Local::ReportContact;
    e.number = kind;
    e.text = contact;
    return e;
}
Effect applyReportKind(int index) {
    if (index < 0 || index > 1) { return {}; }
    Effect e;
    e.local = Local::ReportKind;
    e.text = index == 0 ? cc::kProblemKindBug : cc::kProblemKindDislike;
    e.flag = cc::problemReportDefaultAttachDiagnostics(e.text);   // the box follows the kind's own default every time the kind is (re)chosen
    return e;
}
Effect applyReportAttach(int kind, bool on) {
    Effect e;
    e.local = Local::ReportAttach;
    e.number = kind;
    e.flag = on;
    return e;
}
Effect applyReportPreview(int kind, bool on) {
    Effect e;
    e.local = Local::ReportPreview;
    e.number = kind;
    e.flag = on;
    return e;
}
Effect applyReportProbe(bool on) {
    Effect e;
    e.local = Local::ReportProbe;
    e.flag = on;
    return e;
}
Effect applyReportClosed(int kind) {
    Effect e;
    e.local = Local::CloseReport;
    e.number = kind;
    return e;
}
Effect applyAboutOpen() {
    Effect e;
    e.local = Local::OpenAbout;
    return e;
}

Effect applyReportSend(int kind, const ReportInputs& in) {
    Effect e;
    if (kind == 0) {
        if (!featureVerdict(in.text, in.contact, in.sender, in.now_epoch).can_send) { return e; }
        ReportSend& r = e.report;
        r.send = true;
        r.kind = 0;
        r.endpoint = cc::featureRequestEndpoint();
        r.feature.text = in.text;
        r.feature.contact = in.contact;
        r.feature.version = in.version;
        r.feature.platform = cc::featureRequestPlatform();
        r.feature.arch = cc::featureRequestArch();
        // The count LOGGED is the one the key's boundary and the counter already use - the server's own - never a raw size().
        r.chars = cc::featureRequestTextCharCount(in.text);
        return e;
    }
    if (kind != 1) { return e; }
    if (!problemVerdict(in.problem_kind, in.text, in.contact, in.sender, in.now_epoch).can_send) { return e; }
    ReportSend& r = e.report;
    r.send = true;
    r.kind = 1;
    r.endpoint = cc::problemReportEndpoint();
    r.problem.kind = in.problem_kind;
    r.problem.text = in.text;
    r.problem.contact = in.contact;
    r.problem.version = in.version;
    r.problem.platform = cc::featureRequestPlatform();
    r.problem.arch = cc::featureRequestArch();
    if (in.attach) {
        // The exact text "Show what will be sent" shows, byte for byte; the probe rides in the same field when it was ticked.
        r.problem.diagnostics = (in.attach_probe && !in.probe_report.empty()) ? cc::appendProbeToDiagnosticsForReport(in.diagnostics, in.probe_report) : in.diagnostics;
    }
    // The report the page said was recognised: the same words as the message, sent once more with their class, re-read from the text as it is NOW.
    if (const auto pasted = cc::detectPastedReport(in.text)) { r.problem.attachments.push_back(*pasted); }
    r.chars = cc::featureRequestTextCharCount(in.text);
    return e;
}

}  // namespace foxsdr::slintsystem
