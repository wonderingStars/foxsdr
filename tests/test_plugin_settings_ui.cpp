// CASCADE_CAP_SETTINGS_UI (0.99.43) and the receiver locator: the host half.
//
//   [M] maidenheadGrid6 against reference points worked out by hand (the
//       arithmetic is written beside each one), the poles and the
//       antimeridian, and the out-of-range / non-finite refusals.
//   [V] the loader's check of the settings table (validatePluginDesc).
//   [F] settingFieldsFrom: the form as the host will draw it - kinds this
//       host does not know, entries of a later host's size, repeated and
//       unstorable keys are SKIPPED, never refused; plugin text is bounded.
//   [E] prepareSettingEdit: maxLength, numbers, checkboxes.
//   [R] A REAL MODULE (tests/fixtures/settings_ui_plugin.cpp, built as a
//       .dll/.so) loaded by PluginHost::scan and attached by PluginUi: the
//       table is cached on the record, a default reads through
//       CASCADE_API_NOT_FOUND, and an edit through the host's write path is
//       what the plugin reads with settings_get - with settings_seq moved.
//       The plugin's OWN settings_set does not move it.
//   [W] The widget itself, driven headlessly through ImGui's input queue:
//       typing into the text box, Enter, the cap at maxLength, a refused
//       number snapping back to the stored value, a checkbox click.
//   [O] THE OLD-PLUGIN REGRESSION: a plugin compiled against the smaller,
//       pre-0.99.43 CascadeReceiverState still gets CASCADE_API_OK from
//       get_state, a correct copy, and not one byte written past its own
//       struct - the bug fixed in 473ae9e (the size check refused it).
//   [P] PATH FIELDS (0.99.74, CASCADE_SETTING_PATH): the parser keeps kind 3 and
//       stores it as text ([F] [E] [R] above), and the drawn form gives a PATH
//       field - and only a PATH field - a BROWSE key ([P3] below): found in the
//       UI census as "settings:browse:<key>", pressed with a real click, the
//       chosen path committed through the call a typed edit takes (the store
//       holds it, settings_seq moved, the plugin reads it back), a cancel
//       changing nothing, a path the field cannot hold refused with the field's
//       own message, and no key at all when the host has no file dialog.
//
// Run with the fixture module's directory as argv[1] (tests/CMakeLists.txt).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "core/file_pick.hpp"
#include "core/maidenhead.hpp"
#include "core/plugin_abi.h"
#include "core/plugin_api.hpp"
#include "core/plugin_host.hpp"
#include "core/plugin_settings_ui.hpp"
#include "core/plugin_ui.hpp"
#include "gui/fonts.hpp"
#include "gui/plugin_settings_form.hpp"
#include "gui/ui_census.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "test_check.hpp"

using cascade::core::LoadedPlugin;
using cascade::core::PluginApiCore;
using cascade::core::PluginHost;
using cascade::core::PluginRejection;
using cascade::core::PluginUi;
using cascade::core::ReceiverFacts;
using cascade::core::SettingEditResult;
using cascade::core::SettingField;

namespace {

// ---------------------------------------------------------------------------
// [M] Maidenhead
// ---------------------------------------------------------------------------

std::string grid(double lat, double lon, bool* ok = nullptr) {
    char out[7] = {'x', 'x', 'x', 'x', 'x', 'x', 'x'};
    const bool r = cascade::core::maidenheadGrid6(lat, lon, out);
    if (ok != nullptr) { *ok = r; }
    return std::string(out);
}

void testMaidenhead() {
    std::printf("[M] maidenhead\n");
    bool ok = false;
    // London 51.5074, -0.1278. lon+180 = 179.8722: field 8 (I), square
    // (19.8722/2) = 9, sub (1.8722*12) = 22.47 -> w. lat+90 = 141.5074: field
    // 14 (O), square 1, sub (0.5074*24) = 12.18 -> m.
    CHECK(grid(51.5074, -0.1278, &ok) == "IO91wm");
    CHECK(ok);
    // Sydney -33.8688, 151.2093. lon+180 = 331.2093: field 16 (Q), square
    // (11.2093/2) = 5, sub (1.2093*12) = 14.51 -> o. lat+90 = 56.1312: field 5
    // (F), square 6, sub (0.1312*24) = 3.15 -> d.
    CHECK(grid(-33.8688, 151.2093) == "QF56od");
    // New York 40.7128, -74.0060. lon+180 = 105.994: field 5 (F), square
    // (5.994/2) = 2, sub (1.994*12) = 23.93 -> x. lat+90 = 130.7128: field 13
    // (N), square 0, sub (0.7128*24) = 17.11 -> r.
    CHECK(grid(40.7128, -74.0060) == "FN20xr");
    // The corners of the world: the far edges clamp into the LAST cell
    // (R, 9, x) instead of overflowing to S / ':' / 'y'.
    CHECK(grid(90.0, 180.0) == "RR99xx");
    CHECK(grid(-90.0, -180.0) == "AA00aa");
    // Just west of the antimeridian and just east of it are opposite ends of
    // the alphabet. (0, 179.99): lon+180 = 359.99 -> field 17 (R), square 9,
    // sub (1.99*12) = 23.88 -> x; lat+90 = 90 -> field 9 (J), square 0, sub a.
    CHECK(grid(0.0, 179.99) == "RJ90xa");
    // (0, -179.99): lon+180 = 0.01 -> A, 0, sub (0.01*12) = 0.12 -> a.
    CHECK(grid(0.0, -179.99) == "AJ00aa");
    // Out of range and non-finite: false, and the output EMPTY (never the
    // 'x' filler the helper put there, never a guessed grid).
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    CHECK(grid(90.0001, 0.0, &ok).empty() && !ok);
    CHECK(grid(-90.0001, 0.0, &ok).empty() && !ok);
    CHECK(grid(0.0, 180.5, &ok).empty() && !ok);
    CHECK(grid(0.0, -180.5, &ok).empty() && !ok);
    CHECK(grid(nan, 0.0, &ok).empty() && !ok);
    CHECK(grid(0.0, nan, &ok).empty() && !ok);
    CHECK(grid(inf, 0.0, &ok).empty() && !ok);
}

// ---------------------------------------------------------------------------
// Tables used by [V] and [F]
// ---------------------------------------------------------------------------

CascadeSettingSpec spec(const char* key, const char* label, std::uint32_t kind,
                        std::uint32_t maxLen, const char* hint, const char* def) {
    CascadeSettingSpec s{};
    s.structSize = static_cast<std::uint32_t>(sizeof(CascadeSettingSpec));
    std::snprintf(s.key, sizeof(s.key), "%s", key);
    std::snprintf(s.label, sizeof(s.label), "%s", label);
    s.kind = kind;
    s.maxLength = maxLen;
    std::snprintf(s.placeholder, sizeof(s.placeholder), "%s", hint);
    std::snprintf(s.defaultValue, sizeof(s.defaultValue), "%s", def);
    return s;
}

// A decoder, so a descriptor carrying only it plus the form is loadable.
void* dCreate(std::uint32_t) { return nullptr; }
void dProcess(void*, const float*, std::size_t) {}
std::int32_t dPoll(void*, char*, std::size_t) { return 0; }
void dDestroy(void*) {}
const CascadeDecoderApi kDec = {static_cast<std::uint32_t>(sizeof(CascadeDecoderApi)), 0u,
                                &dCreate, &dProcess, &dPoll, &dDestroy};

PluginRejection validateWith(const CascadeSettingsUiApi* ui, std::uint32_t tableSize) {
    CascadeCapabilityEntry caps[2] = {
        {CASCADE_CAP_DECODER, static_cast<std::uint32_t>(sizeof(CascadeDecoderApi)), &kDec},
        {CASCADE_CAP_SETTINGS_UI, tableSize, ui},
    };
    CascadePluginDesc d{};
    d.structSize = static_cast<std::uint32_t>(sizeof(CascadePluginDesc));
    d.abiVersion = CASCADE_PLUGIN_ABI_VERSION;
    d.name = "Form";
    d.version = "1.0.0";
    d.author = "";
    d.licence = "MIT";
    d.capabilities = CASCADE_CAP_DECODER | CASCADE_CAP_SETTINGS_UI;
    d.capabilityCount = 2;
    d.capabilityTables = caps;
    return cascade::core::validatePluginDesc(&d);
}

void testValidation() {
    std::printf("[V] loader check of the settings table\n");
    const CascadeSettingSpec one[1] = {spec("callsign", "Callsign", CASCADE_SETTING_TEXT, 12, "",
                                            "")};
    CascadeSettingsUiApi ui{static_cast<std::uint32_t>(sizeof(CascadeSettingsUiApi)), 1u, one};
    const auto sz = static_cast<std::uint32_t>(sizeof(CascadeSettingsUiApi));
    CHECK(validateWith(&ui, sz) == PluginRejection::None);
    // Bit declared, no table: refused, named.
    CHECK(validateWith(nullptr, sz) == PluginRejection::MissingSettingsUiApi);
    CascadeSettingsUiApi bad = ui;
    bad.structSize = sz + 4u;
    CHECK(validateWith(&bad, sz) == PluginRejection::SettingsUiStructSizeMismatch);
    bad = ui;
    bad.specs = nullptr;
    CHECK(validateWith(&bad, sz) == PluginRejection::SettingsUiBadTable);
    bad = ui;
    bad.count = 0u;
    CHECK(validateWith(&bad, sz) == PluginRejection::SettingsUiBadTable);
    bad = ui;
    bad.count = CASCADE_MAX_SETTING_SPECS + 1u;
    CHECK(validateWith(&bad, sz) == PluginRejection::SettingsUiBadTable);
    // Each has its own words, not the catch-all.
    for (PluginRejection r : {PluginRejection::MissingSettingsUiApi,
                              PluginRejection::SettingsUiStructSizeMismatch,
                              PluginRejection::SettingsUiBadTable}) {
        CHECK(std::string(cascade::core::pluginRejectionMessage(r)) != "unknown rejection");
    }
    // Two copies of one plugin: the one turned off loses its form pointer
    // with every other borrowed table (it points into an image about to be
    // unmapped), and the kept one keeps it.
    {
        std::vector<LoadedPlugin> recs(2);
        for (int i = 0; i < 2; ++i) {
            recs[i].loaded = true;
            recs[i].name = "Twin";
            recs[i].version = i == 0 ? "1.0.0" : "1.1.0";
            recs[i].path = i == 0 ? "C:/plugins/twin-1.0.0.dll" : "C:/plugins/twin-1.1.0.dll";
            recs[i].capabilities = CASCADE_CAP_DECODER | CASCADE_CAP_SETTINGS_UI;
            recs[i].decoder = &kDec;
            recs[i].settingsUi = &ui;
        }
        CHECK(cascade::core::resolveDuplicatePlugins(recs) == 1u);
        CHECK(!recs[0].loaded && recs[0].settingsUi == nullptr);
        CHECK(recs[1].loaded && recs[1].settingsUi == &ui);
    }
    // A form alone is not a plugin: nothing usable -> refused as such.
    CascadeCapabilityEntry only[1] = {{CASCADE_CAP_SETTINGS_UI, sz, &ui}};
    CascadePluginDesc d{};
    d.structSize = static_cast<std::uint32_t>(sizeof(CascadePluginDesc));
    d.abiVersion = CASCADE_PLUGIN_ABI_VERSION;
    d.name = "OnlyForm";
    d.version = "1.0.0";
    d.author = "";
    d.licence = "MIT";
    d.capabilities = CASCADE_CAP_SETTINGS_UI;
    d.capabilityCount = 1;
    d.capabilityTables = only;
    CHECK(cascade::core::validatePluginDesc(&d) == PluginRejection::NoUsableCapability);
}

// ---------------------------------------------------------------------------
// [F] the form as the host reads it
// ---------------------------------------------------------------------------

void testFields() {
    std::printf("[F] settingFieldsFrom\n");
    CascadeSettingSpec s[9] = {
        spec("callsign", "Callsign", CASCADE_SETTING_TEXT, 12, "e.g. G4ABC", ""),
        spec("power", "Power", CASCADE_SETTING_NUMBER, 0, "watts", "5"),
        spec("report", "Report", CASCADE_SETTING_BOOL, 0, "ignored", "1"),
        spec("future", "Later kind", 99u, 0, "", ""),            // unknown kind
        spec("bad key!", "Bad key", CASCADE_SETTING_TEXT, 4, "", ""),  // unstorable key
        spec("callsign", "Again", CASCADE_SETTING_TEXT, 4, "", ""),    // repeat
        spec("big", "Huge", CASCADE_SETTING_TEXT, 99999u, "", ""),     // clamped limit
        spec("tiny", "a##b\nc", CASCADE_SETTING_TEXT, 0u, "", "toolong"),  // 0 -> 1
        spec("num", "", CASCADE_SETTING_NUMBER, 0, "", "abc"),  // unusable default, no label
    };
    // An entry of a LATER host's size, placed between two good ones.
    CascadeSettingSpec later[3] = {
        spec("a", "A", CASCADE_SETTING_TEXT, 4, "", ""),
        spec("b", "B", CASCADE_SETTING_TEXT, 4, "", ""),
        spec("c", "C", CASCADE_SETTING_TEXT, 4, "", ""),
    };
    later[1].structSize += 8u;

    CascadeSettingsUiApi ui{static_cast<std::uint32_t>(sizeof(CascadeSettingsUiApi)), 9u, s};
    const std::vector<SettingField> f = cascade::core::settingFieldsFrom(&ui);
    CHECK(f.size() == 6u);
    if (f.size() == 6u) {
        CHECK(f[0].key == "callsign" && f[0].kind == CASCADE_SETTING_TEXT && f[0].maxLength == 12u);
        CHECK(f[0].label == "Callsign" && f[0].placeholder == "e.g. G4ABC" &&
              f[0].defaultValue.empty());
        CHECK(f[1].key == "power" && f[1].kind == CASCADE_SETTING_NUMBER &&
              f[1].defaultValue == "5" && f[1].maxLength == cascade::core::kMaxNumberChars);
        CHECK(f[2].key == "report" && f[2].kind == CASCADE_SETTING_BOOL &&
              f[2].defaultValue == "1" && f[2].placeholder.empty());
        CHECK(f[3].key == "big" && f[3].maxLength == CASCADE_SETTING_VALUE_BYTES - 1u);
        // "##" collapsed (it would cut an ImGui id), a newline made a space;
        // limit 0 raised to 1, and a default the field could not hold dropped.
        CHECK(f[4].key == "tiny" && f[4].label == "a#b c" && f[4].maxLength == 1u &&
              f[4].defaultValue.empty());
        // An empty label shows the key rather than an unlabelled box.
        CHECK(f[5].key == "num" && f[5].label == "num" && f[5].defaultValue.empty());
    }

    CascadeSettingsUiApi ui2{static_cast<std::uint32_t>(sizeof(CascadeSettingsUiApi)), 3u, later};
    const std::vector<SettingField> g = cascade::core::settingFieldsFrom(&ui2);
    CHECK(g.size() == 2u && g[0].key == "a" && g[1].key == "c");

    // [Minor] A label ending in EXACTLY ONE "#" keeps it. cleanPluginText
    // only collapses a CONSECUTIVE pair ("##" -> "#"); a lone trailing "#"
    // is not a pair and must reach the field untouched - the BOOL widget
    // (gui/plugin_settings_form.cpp) draws this label with TextUnformatted
    // rather than concatenating it with an id suffix precisely so a trailing
    // "#" here is never later swallowed by an accidental "##" at the join.
    CascadeSettingSpec hashEnd = spec("hashend", "Report#", CASCADE_SETTING_BOOL, 0, "", "");
    CascadeSettingsUiApi ui4{static_cast<std::uint32_t>(sizeof(CascadeSettingsUiApi)), 1u,
                             &hashEnd};
    const std::vector<SettingField> he = cascade::core::settingFieldsFrom(&ui4);
    CHECK(he.size() == 1u && he[0].label == "Report#");

    // A label with no terminator is read to its array's end and no further.
    CascadeSettingSpec open = spec("k", "", CASCADE_SETTING_TEXT, 4, "", "");
    std::memset(open.label, 'L', sizeof(open.label));
    CascadeSettingsUiApi ui3{static_cast<std::uint32_t>(sizeof(CascadeSettingsUiApi)), 1u, &open};
    const std::vector<SettingField> h = cascade::core::settingFieldsFrom(&ui3);
    CHECK(h.size() == 1u && h[0].label == std::string(CASCADE_SETTING_LABEL_CHARS, 'L'));

    // [P1] A PATH field (0.99.74) is a field, NOT a later host's kind: it is kept, in the
    // plugin's order, and holds what a TEXT field holds - the limit clamped the same way
    // (0 -> 1, past the store's value size -> the store's), the placeholder kept (the
    // BROWSE key reads ".exe" off it), and a default only if the field could hold it.
    // Before 0.99.74 this host skipped kind 3, which is what a host that does not know
    // the kind must keep doing (the "future" entries above).
    CascadeSettingSpec pathSpecs[5] = {
        spec("tool_path", "Tool path", CASCADE_SETTING_PATH, 260, "C:\\tools\\tool.exe", ""),
        spec("huge", "Huge", CASCADE_SETTING_PATH, 99999u, "", ""),
        spec("tiny", "Tiny", CASCADE_SETTING_PATH, 0u, "", ""),
        spec("shown", "Shown", CASCADE_SETTING_PATH, 20, "", "C:\\def.exe"),
        spec("cut", "Cut", CASCADE_SETTING_PATH, 4, "", "C:\\def.exe"),  // default too long
    };
    CascadeSettingsUiApi uiP{static_cast<std::uint32_t>(sizeof(CascadeSettingsUiApi)), 5u,
                             pathSpecs};
    const std::vector<SettingField> pf = cascade::core::settingFieldsFrom(&uiP);
    CHECK(pf.size() == 5u);
    if (pf.size() == 5u) {
        CHECK(pf[0].key == "tool_path" && pf[0].kind == CASCADE_SETTING_PATH &&
              pf[0].maxLength == 260u && pf[0].placeholder == "C:\\tools\\tool.exe" &&
              pf[0].label == "Tool path" && pf[0].defaultValue.empty());
        CHECK(pf[1].maxLength == CASCADE_SETTING_VALUE_BYTES - 1u);
        CHECK(pf[2].maxLength == 1u);
        CHECK(pf[3].defaultValue == "C:\\def.exe");
        CHECK(pf[4].defaultValue.empty());
    }

    // Whole-table faults: nothing, not a crash.
    CHECK(cascade::core::settingFieldsFrom(nullptr).empty());
    CascadeSettingsUiApi z = ui;
    z.structSize += 1u;
    CHECK(cascade::core::settingFieldsFrom(&z).empty());
    z = ui;
    z.specs = nullptr;
    CHECK(cascade::core::settingFieldsFrom(&z).empty());
    z = ui;
    z.count = 0u;
    CHECK(cascade::core::settingFieldsFrom(&z).empty());
}

// ---------------------------------------------------------------------------
// [E] what an edit may store
// ---------------------------------------------------------------------------

// The number a plugin built for 0.99.74 writes into CascadeSettingSpec::kind. It is part of the
// ABI: a plugin compiled with 3 meaning "path" must still mean it in every later host.
static_assert(CASCADE_SETTING_PATH == 3u, "CASCADE_SETTING_PATH is ABI: it is 3");

SettingEditResult prep(const SettingField& f, const std::string& in, std::string& out) {
    return cascade::core::prepareSettingEdit(f, in, out);
}

void testEdits() {
    std::printf("[E] prepareSettingEdit\n");
    SettingField text;
    text.key = "callsign";
    text.kind = CASCADE_SETTING_TEXT;
    text.maxLength = 6;
    std::string out;
    CHECK(prep(text, "G4ABC", out) == SettingEditResult::Accepted && out == "G4ABC");
    CHECK(prep(text, "G4ABCD", out) == SettingEditResult::Accepted && out == "G4ABCD");  // == limit
    CHECK(prep(text, "G4ABCDE", out) == SettingEditResult::TooLong && out.empty());      // limit + 1
    CHECK(prep(text, "", out) == SettingEditResult::Accepted && out.empty());  // cleared, not deleted
    CHECK(prep(text, "a\tb", out) == SettingEditResult::BadText);
    CHECK(prep(text, "a\nb", out) == SettingEditResult::BadText);
    CHECK(prep(text, "\xC3", out) == SettingEditResult::BadText);        // cut sequence
    CHECK(prep(text, "\xED\xA0\x80", out) == SettingEditResult::BadText);  // surrogate
    CHECK(prep(text, "\xC3\xA9t\xC3\xA9", out) == SettingEditResult::Accepted);  // "été", 6 bytes

    // A PATH is stored as a TEXT is (0.99.74): exactly as typed or chosen - spaces and
    // backslashes kept, no trimming - at most maxLength bytes of clean UTF-8.
    SettingField path;
    path.key = "tool_path";
    path.kind = CASCADE_SETTING_PATH;
    path.maxLength = 33;
    const std::string spaced = "C:\\Program Files\\nrsc5\\nrsc5.exe";  // 32 bytes
    CHECK(spaced.size() == 32u);
    CHECK(prep(path, spaced, out) == SettingEditResult::Accepted && out == spaced);
    CHECK(prep(path, spaced + "x", out) == SettingEditResult::Accepted && out == spaced + "x");  // == limit
    CHECK(prep(path, spaced + "xy", out) == SettingEditResult::TooLong && out.empty());          // limit + 1
    CHECK(prep(path, "", out) == SettingEditResult::Accepted && out.empty());  // cleared, not deleted
    CHECK(prep(path, "C:\\a\tb.exe", out) == SettingEditResult::BadText);
    CHECK(prep(path, "C:\\a\nb.exe", out) == SettingEditResult::BadText);
    CHECK(prep(path, "C:\\Users\\Zo\xC3\xAB\\a.exe", out) == SettingEditResult::Accepted);  // "Zoë"
    CHECK(prep(path, "C:\\Users\\Zo\xC3", out) == SettingEditResult::BadText);  // cut sequence
    CHECK(prep(path, "/usr/bin/nrsc5", out) == SettingEditResult::Accepted && out == "/usr/bin/nrsc5");

    SettingField num;
    num.key = "power";
    num.kind = CASCADE_SETTING_NUMBER;
    num.maxLength = cascade::core::kMaxNumberChars;
    for (const char* good : {"5", "-3", "+2", "12.5", ".5", "5.", "1e3", "-2.5E-3", "0"}) {
        CHECK(prep(num, good, out) == SettingEditResult::Accepted && out == good);
    }
    CHECK(prep(num, "  42 ", out) == SettingEditResult::Accepted && out == "42");  // trimmed
    CHECK(prep(num, "", out) == SettingEditResult::Accepted && out.empty());
    for (const char* bad : {"abc", "1.2.3", "0x10", "inf", "nan", "1e999", "-", ".", "e5",
                            "1e", "1,5", "12abc", "--1"}) {
        CHECK(prep(num, bad, out) == SettingEditResult::NotANumber);
    }
    CHECK(prep(num, std::string(40, '1'), out) == SettingEditResult::TooLong);

    SettingField box;
    box.key = "report";
    box.kind = CASCADE_SETTING_BOOL;
    box.maxLength = 1;
    CHECK(prep(box, "1", out) == SettingEditResult::Accepted && out == "1");
    CHECK(prep(box, "", out) == SettingEditResult::Accepted && out.empty());
    CHECK(prep(box, "0", out) == SettingEditResult::Accepted && out.empty());
    CHECK(prep(box, "true", out) == SettingEditResult::Accepted && out.empty());
    CHECK(cascade::core::settingBoolChecked("1"));
    CHECK(!cascade::core::settingBoolChecked(""));
    CHECK(!cascade::core::settingBoolChecked("0"));

    SettingField odd;
    odd.key = "x";
    odd.kind = 42u;
    CHECK(prep(odd, "v", out) == SettingEditResult::Refused);

    // Every refusal has words to show; an accepted edit shows none. And every
    // one of those words is DIFFERENT - "the plugin's settings are full" must
    // not be the sentence a NUMBER typo or an internal fault gets too, or the
    // one useful, actionable message drowns in false positives.
    CHECK(std::string(cascade::gui::settingEditMessage(SettingEditResult::Accepted)).empty());
    std::set<std::string> messages;
    for (SettingEditResult r :
        {SettingEditResult::TooLong, SettingEditResult::NotANumber, SettingEditResult::BadText,
         SettingEditResult::StoreFull, SettingEditResult::StoreError, SettingEditResult::Refused}) {
        const std::string m = cascade::gui::settingEditMessage(r);
        CHECK(!m.empty());
        messages.insert(m);
    }
    CHECK(messages.size() == 6u);  // six results, six distinct sentences
    // Two surfaces drawing one plugin keep two boxes.
    CHECK(cascade::gui::settingFieldStateKey("rail", "P", "k") !=
          cascade::gui::settingFieldStateKey("patch", "P", "k"));
}

// ---------------------------------------------------------------------------
// [R] the real module
// ---------------------------------------------------------------------------

// Everything the decoder instance has to say, drained.
std::string drain(const CascadeDecoderApi* d, void* inst) {
    std::string all;
    char buf[64];
    for (int i = 0; i < 64; ++i) {
        const std::int32_t n = d->poll_text(inst, buf, sizeof(buf));
        if (n <= 0) { break; }
        all.append(buf, static_cast<std::size_t>(n));
    }
    return all;
}

const SettingField* fieldByKey(const std::vector<SettingField>& f, const char* key) {
    for (const SettingField& x : f) {
        if (x.key == key) { return &x; }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// [W] headless ImGui
// ---------------------------------------------------------------------------

struct Form {
    PluginApiCore& api;
    std::string plugin;
    std::vector<SettingField> fields;
    cascade::gui::PluginSettingsFormState state;
    int focus = -1;        // SetKeyboardFocusHere(focus) on the next frame
    ImVec2 lastMin{}, lastMax{};
    int stored = 0;

    void frame() {
        // The production per-frame reconciliation (app_window.cpp's drawUi):
        // clear touched marks before drawing, exactly as every real frame
        // does - so a test that never calls flushUntouchedSettingsEdits sees
        // no behaviour change, and one that does (below) sees the real thing.
        cascade::gui::beginSettingsFormFrame(state);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(500.0f, 600.0f), ImGuiCond_Always);
        ImGui::Begin("form", nullptr, ImGuiWindowFlags_NoTitleBar);
        if (focus >= 0) {
            ImGui::SetKeyboardFocusHere(focus);
            focus = -1;
        }
        stored += cascade::gui::drawPluginSettingsForm(api, plugin, fields, state, "test");
        lastMin = ImGui::GetItemRectMin();
        lastMax = ImGui::GetItemRectMax();
        ImGui::End();
        ImGui::Render();
    }
    void frames(int n) {
        for (int i = 0; i < n; ++i) { frame(); }
    }
    std::string box(const char* key) {
        const auto it = state.fields.find(cascade::gui::settingFieldStateKey("test", plugin, key));
        if (it == state.fields.end() || it->second.buf.empty()) { return "<none>"; }
        return std::string(it->second.buf.data());
    }
    SettingEditResult last(const char* key) {
        return state.fields[cascade::gui::settingFieldStateKey("test", plugin, key)].last;
    }
    void type(const char* text) {
        ImGui::GetIO().AddInputCharactersUTF8(text);
        frame();
    }
    void enter() {
        ImGuiIO& io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiKey_Enter, true);
        frame();
        io.AddKeyEvent(ImGuiKey_Enter, false);
        frames(2);
    }
    void selectAll() {
        ImGuiIO& io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiMod_Ctrl, true);
        io.AddKeyEvent(ImGuiKey_A, true);
        frame();
        io.AddKeyEvent(ImGuiKey_A, false);
        io.AddKeyEvent(ImGuiMod_Ctrl, false);
        frame();
    }
};

void testWidget(PluginApiCore& api, const std::string& plugin,
                const std::vector<SettingField>& fields) {
    std::printf("[W] the drawn form, driven headlessly\n");
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 700.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    cascade::gui::fonts::load();

    Form f{api, plugin, fields};
    f.frames(2);
    // What the boxes show before any typing: the store, else the default.
    CHECK(f.box("callsign") == "M0XYZ");  // left there by [R]
    CHECK(f.box("power") == "5");

    const std::uint64_t seq0 = api.settingsUiSeq(plugin);
    // Type a callsign LONGER than the field allows: the box stops at 12.
    f.focus = 0;
    f.frames(2);
    f.selectAll();
    f.type("2E0ABCDEFGHIJKLMNOP");
    // Nothing is stored while the box is being typed in.
    CHECK(api.settingsUiSeq(plugin) == seq0);
    f.enter();
    std::string v;
    CHECK(api.settingsUiGet(plugin, "callsign", v) == CASCADE_API_OK);
    CHECK(v == "2E0ABCDEFGHI");  // 12 bytes: maxLength enforced by the widget
    CHECK(api.settingsUiSeq(plugin) == seq0 + 1u);
    CHECK(f.box("callsign") == "2E0ABCDEFGHI");
    CHECK(f.last("callsign") == SettingEditResult::Accepted);

    // A number that is not one: refused, SHOWN refused, and the box goes back
    // to what the store holds - it does not keep the rejected text.
    f.focus = 1;
    f.frames(2);
    f.selectAll();
    f.type("12x");
    CHECK(f.box("power") == "12x");  // while typing, the user's text stands
    f.enter();
    CHECK(f.last("power") == SettingEditResult::NotANumber);
    CHECK(f.box("power") == "5");
    CHECK(api.settingsUiGet(plugin, "power", v) == CASCADE_API_NOT_FOUND);
    CHECK(api.settingsUiSeq(plugin) == seq0 + 1u);  // a refusal moves nothing

    // And one that is.
    f.focus = 1;
    f.frames(2);
    f.selectAll();
    f.type("25");
    f.enter();
    CHECK(f.last("power") == SettingEditResult::Accepted);
    CHECK(api.settingsUiGet(plugin, "power", v) == CASCADE_API_OK && v == "25");
    CHECK(api.settingsUiSeq(plugin) == seq0 + 2u);

    // The checkbox is the last item drawn (no refusal line follows it now).
    // Its state is the store's "1" from [R]; a click turns it off - stored as
    // "" (never a deletion, which would bring a "1" default back).
    const ImVec2 c(f.lastMin.x + 6.0f, (f.lastMin.y + f.lastMax.y) * 0.5f);
    io.AddMousePosEvent(c.x, c.y);
    f.frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    f.frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    f.frames(2);
    CHECK(api.settingsUiGet(plugin, "report", v) == CASCADE_API_OK && v.empty());
    CHECK(api.settingsUiSeq(plugin) == seq0 + 3u);

    // A value the PLUGIN changes while nobody is typing shows at once.
    api.settingsUiSet(plugin, "callsign", "G0AAA");
    f.frames(1);
    CHECK(f.box("callsign") == "G0AAA");
    CHECK(f.stored == 3);

    ImGui::DestroyContext();
}

// ---------------------------------------------------------------------------
// [Minor] no silent loss: a surface that stops drawing a mid-edit field still
// commits it (a window closing, a plugin row disappearing, the patch page
// selecting a different node - none of which fire IsItemDeactivatedAfterEdit,
// because the box is simply never drawn again).
// ---------------------------------------------------------------------------

void testFlushOnDisappear(PluginApiCore& api, const std::string& plugin,
                          const std::vector<SettingField>& fields) {
    std::printf("[Minor] flush on window/section close\n");
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 700.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    cascade::gui::fonts::load();

    Form f{api, plugin, fields};
    f.frames(2);
    const std::uint64_t seq0 = api.settingsUiSeq(plugin);

    // Type into the callsign box WITHOUT committing it the ordinary way - no
    // Enter, no Tab, no click elsewhere.
    f.focus = 0;
    f.frames(2);
    f.selectAll();
    f.type("G7XYZ");
    CHECK(api.settingsUiSeq(plugin) == seq0);  // nothing stored yet
    CHECK(f.box("callsign") == "G7XYZ");

    // THE SURFACE DISAPPEARS: no further frame calls drawPluginSettingsForm
    // for this field at all - exactly what a closed window, a removed plugin
    // row, or the patch page deselecting this node looks like from the
    // field's own state. The per-frame reconciliation still runs, exactly as
    // it does every frame in the real application (drawUi's own begin/end
    // calls), and it is what must catch this.
    cascade::gui::beginSettingsFormFrame(f.state);
    const int flushed = cascade::gui::flushUntouchedSettingsEdits(api, f.state);
    CHECK(flushed == 1);
    std::string v;
    CHECK(api.settingsUiGet(plugin, "callsign", v) == CASCADE_API_OK && v == "G7XYZ");
    CHECK(api.settingsUiSeq(plugin) == seq0 + 1u);

    // A second flush of the same, now-clean state does nothing more - the
    // field is no longer `editing`, so there is nothing left to lose.
    CHECK(cascade::gui::flushUntouchedSettingsEdits(api, f.state) == 0);
    CHECK(api.settingsUiSeq(plugin) == seq0 + 1u);

    // A REFUSED flush still clears `editing` without storing anything - it
    // just has nowhere left to draw the refusal, since the surface is gone.
    // Field 1 (power, NUMBER) with non-numeric text: TEXT's own maxLength
    // cannot be exercised through the widget itself, because the box's
    // buffer is sized to maxLength+1 and ImGui refuses to hold more than that
    // in the first place - "too long to type" is not a state this path can
    // reach, so NotANumber is what proves a refusal is dropped rather than
    // silently accepted.
    std::string before;
    api.settingsUiGet(plugin, "power", before);  // whatever it was; may be unset
    f.focus = 1;
    f.frames(2);
    f.selectAll();
    f.type("abc");
    cascade::gui::beginSettingsFormFrame(f.state);
    CHECK(cascade::gui::flushUntouchedSettingsEdits(api, f.state) == 0);  // refused, not accepted
    std::string pv;
    api.settingsUiGet(plugin, "power", pv);
    CHECK(pv == before);  // unchanged either way
    CHECK(api.settingsUiSeq(plugin) == seq0 + 1u);  // a refusal moves nothing further

    ImGui::DestroyContext();
}

// ---------------------------------------------------------------------------
// [P3] the BROWSE key of a PATH field (0.99.74)
// ---------------------------------------------------------------------------

// What the UI census file holds right now (the file FOXSDR_UI_CENSUS names, set
// in main() before anything asks whether the census is on).
struct CensusRead {
    std::set<std::string> items;
    std::map<std::string, std::array<float, 4>> rects;
    bool hasItemStartingWith(const char* prefix) const {
        for (const std::string& s : items) {
            if (s.rfind(prefix, 0) == 0) { return true; }
        }
        return false;
    }
};

std::string g_censusPath;

CensusRead readCensus() {
    CensusRead c;
    CHECK(cascade::gui::census::write());
    std::ifstream in(g_censusPath);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("item ", 0) == 0) { c.items.insert(line.substr(5)); }
        if (line.rfind("rect ", 0) != 0) { continue; }
        std::istringstream ss(line.substr(5));
        std::string name;
        std::array<float, 4> r{};
        ss >> name >> r[0] >> r[1] >> r[2] >> r[3];
        c.rects[name] = r;
    }
    return c;
}

// A real left click at `at`, the way the existing checkbox test does it: the
// pointer arrives, goes down a frame later, up a frame after that (the press is
// the release), then two frames to settle.
void clickAt(Form& f, ImVec2 at) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(at.x, at.y);
    f.frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    f.frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    f.frames(2);
}

ImVec2 browseKeyCentre(const char* key) {
    const CensusRead c = readCensus();
    const auto it = c.rects.find(std::string("settings:browse:") + key);
    CHECK(it != c.rects.end());
    if (it == c.rects.end()) { return ImVec2(-1.0f, -1.0f); }
    return ImVec2(0.5f * (it->second[0] + it->second[2]), 0.5f * (it->second[1] + it->second[3]));
}

// What the stand-in file dialog was asked, and what it answers.
struct Picker {
    int calls = 0;
    cascade::core::FilePickRequest last;
    bool answer = false;
    std::string path;
    std::function<bool(const cascade::core::FilePickRequest&, std::string&)> fn() {
        return [this](const cascade::core::FilePickRequest& rq, std::string& out) {
            ++calls;
            last = rq;
            out = answer ? path : std::string();
            return answer;
        };
    }
};

void testBrowseKey(PluginApiCore& api, const std::string& plugin,
                   const std::vector<SettingField>& fields) {
    std::printf("[P3] the BROWSE key of a PATH field\n");
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(800.0f, 700.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    cascade::gui::fonts::load();

    const std::string before = "C:\\tools\\nrsc5.exe";  // left in the store by [P2]
    std::string v;
    CHECK(api.settingsUiGet(plugin, "tool_path", v) == CASCADE_API_OK && v == before);

    // A host with NO file dialog (state.pick empty - Linux with neither zenity
    // nor kdialog): the PATH field is its text box alone. Run FIRST, because the
    // census only ever gains items - nothing below may have drawn a key yet.
    {
        Form f{api, plugin, fields, {}};
        f.frames(3);
        CHECK(f.box("tool_path") == before);  // the box is there and shows the stored path
        CHECK(!readCensus().hasItemStartingWith("settings:browse:"));
    }

    // A host with one: the key is drawn for the PATH field - and for no other.
    Picker pick;
    Form f{api, plugin, fields, {}};
    f.state.pick = pick.fn();
    f.frames(3);
    {
        const CensusRead c = readCensus();
        CHECK(c.items.count("settings:browse:tool_path") == 1u);
        CHECK(c.rects.count("settings:browse:tool_path") == 1u);
        // Not for TEXT, NUMBER or BOOL: exactly one key was noted in all.
        std::size_t keys = 0;
        for (const std::string& s : c.items) {
            if (s.rfind("settings:browse:", 0) == 0) { ++keys; }
        }
        CHECK(keys == 1u);
        CHECK(c.rects.count("settings:browse:callsign") == 0u);
        CHECK(c.rects.count("settings:browse:power") == 0u);
        CHECK(c.rects.count("settings:browse:report") == 0u);
        // It sits beside the box, to its right, on the box's own line: the box
        // is shorter by the key's width, not pushed down a line.
        const auto it = c.rects.find("settings:browse:tool_path");
        if (it != c.rects.end()) { CHECK(it->second[2] > it->second[0] + 20.0f); }
    }

    // PRESSED, with a file chosen: the path is stored, settings_seq moves once,
    // and the box shows it on the next frame. Asked for the right things.
    const std::uint64_t seq0 = api.settingsUiSeq(plugin);
    const int stored0 = f.stored;
    pick.answer = true;
    pick.path = "C:\\Tools\\New Folder\\tool.exe";
    clickAt(f, browseKeyCentre("tool_path"));
    CHECK(pick.calls == 1);
    CHECK(pick.last.title == "Choose a file");
    CHECK(pick.last.programs);                       // the placeholder ends ".exe"
    CHECK(pick.last.startFrom == before);            // opens where the current value is
    CHECK(!pick.last.programsName.empty() && !pick.last.allFilesName.empty());
    CHECK(api.settingsUiGet(plugin, "tool_path", v) == CASCADE_API_OK && v == pick.path);
    CHECK(api.settingsUiSeq(plugin) == seq0 + 1u);   // the plugin is told, as for a typed edit
    CHECK(f.stored == stored0 + 1);
    CHECK(f.last("tool_path") == SettingEditResult::Accepted);
    f.frames(1);
    CHECK(f.box("tool_path") == pick.path);

    // CANCELLED: nothing changes - not the value, not the counter.
    pick.answer = false;
    clickAt(f, browseKeyCentre("tool_path"));
    CHECK(pick.calls == 2);
    CHECK(api.settingsUiGet(plugin, "tool_path", v) == CASCADE_API_OK && v == pick.path);
    CHECK(api.settingsUiSeq(plugin) == seq0 + 1u);
    CHECK(f.stored == stored0 + 1);
    CHECK(f.box("tool_path") == pick.path);

    // A path the field cannot hold (260 is its limit): refused with the field's
    // own sentence, the store untouched, the box back on what the store holds.
    pick.answer = true;
    pick.path = std::string(261, 'p');
    clickAt(f, browseKeyCentre("tool_path"));
    CHECK(pick.calls == 3);
    CHECK(f.last("tool_path") == SettingEditResult::TooLong);
    CHECK(api.settingsUiGet(plugin, "tool_path", v) == CASCADE_API_OK &&
          v == "C:\\Tools\\New Folder\\tool.exe");
    CHECK(api.settingsUiSeq(plugin) == seq0 + 1u);
    CHECK(f.box("tool_path") == "C:\\Tools\\New Folder\\tool.exe");

    // And one a setting cannot store at all (a control character).
    pick.path = "C:\\a\tb.exe";
    clickAt(f, browseKeyCentre("tool_path"));
    CHECK(pick.calls == 4);
    CHECK(f.last("tool_path") == SettingEditResult::BadText);
    CHECK(api.settingsUiSeq(plugin) == seq0 + 1u);

    // A placeholder that does not end in ".exe" asks for all files, not programs
    // (a Linux plugin's "/usr/bin/nrsc5").
    {
        std::vector<SettingField> other = fields;
        for (SettingField& x : other) {
            if (x.key == "tool_path") { x.placeholder = "/usr/bin/nrsc5"; }
        }
        Picker p2;
        Form g{api, plugin, other, {}};
        g.state.pick = p2.fn();
        g.frames(3);
        p2.answer = false;
        clickAt(g, browseKeyCentre("tool_path"));
        CHECK(p2.calls == 1);
        CHECK(!p2.last.programs);
        CHECK(p2.last.startFrom == "C:\\Tools\\New Folder\\tool.exe");
    }

    ImGui::DestroyContext();
}

void testRealModule(const char* dir) {
    std::printf("[R] the fixture module, through the real loader\n");
    PluginHost host;
    host.scan(dir);
    const LoadedPlugin* rec = nullptr;
    for (const LoadedPlugin& p : host.plugins()) {
        std::printf("  %s loaded=%d err='%s'\n", p.path.c_str(), p.loaded ? 1 : 0,
                    p.error.c_str());
        if (p.loaded && p.name == "Settings Form Fixture") { rec = &p; }
    }
    CHECK(rec != nullptr);
    if (rec == nullptr) { return; }
    // Cached on the record exactly like the other tables.
    CHECK((rec->capabilities & CASCADE_CAP_SETTINGS_UI) != 0u);
    CHECK(rec->settingsUi != nullptr);
    CHECK(rec->decoder != nullptr && rec->hostClient != nullptr);
    const std::vector<SettingField> fields = cascade::core::settingFieldsFrom(rec->settingsUi);
    // Seven entries declared; the later-kind, later-size and repeated ones are
    // skipped - the form still has its four (a PATH, from 0.99.74, is one of them:
    // before it, this host skipped kind 3 and the form had three).
    CHECK(fields.size() == 4u);
    const SettingField* call = fieldByKey(fields, "callsign");
    const SettingField* power = fieldByKey(fields, "power");
    const SettingField* report = fieldByKey(fields, "report");
    const SettingField* tool = fieldByKey(fields, "tool_path");
    CHECK(call != nullptr && power != nullptr && report != nullptr && tool != nullptr);
    if (call == nullptr || power == nullptr || report == nullptr || tool == nullptr) { return; }
    CHECK(call->maxLength == 12u && call->placeholder == "e.g. G4ABC");
    CHECK(tool->kind == CASCADE_SETTING_PATH && tool->maxLength == 260u &&
          tool->placeholder == "C:\\tools\\tool.exe");

    {
        PluginUi ui;
        ui.rebuild(host.plugins());  // attach(): the module keeps the table
        PluginApiCore& api = ui.api();
        const std::string name = rec->name;

        void* inst = rec->decoder->create(48000u);
        CHECK(inst != nullptr);
        if (inst == nullptr) { return; }

        // Before anything: the counter's documented start, and defaults via
        // NOT_FOUND on both sides.
        CHECK(api.settingsUiSeq(name) == 1u);
        CHECK(cascade::core::settingFieldValue(api, name, *power) == "5");
        CHECK(cascade::core::settingFieldValue(api, name, *call).empty());
        CHECK(drain(rec->decoder, inst) == "SETTINGS seq=1 callsign= power=5 report=\n");
        CHECK(drain(rec->decoder, inst).empty());  // nothing moved, nothing said

        // THE WRITE PATH: the host's field -> store -> the plugin's settings_get.
        CHECK(cascade::core::commitSettingEdit(api, name, *call, "G4ABC") ==
              SettingEditResult::Accepted);
        CHECK(api.settingsUiSeq(name) == 2u);
        CHECK(drain(rec->decoder, inst) == "SETTINGS seq=2 callsign=G4ABC power=5 report=\n");

        // Refusals move nothing and reach nothing.
        CHECK(cascade::core::commitSettingEdit(api, name, *power, "lots") ==
              SettingEditResult::NotANumber);
        CHECK(cascade::core::commitSettingEdit(api, name, *call, "G4ABCDEFGHIJK") ==
              SettingEditResult::TooLong);  // 13 > 12
        CHECK(api.settingsUiSeq(name) == 2u);
        CHECK(drain(rec->decoder, inst).empty());

        CHECK(cascade::core::commitSettingEdit(api, name, *report, "1") ==
              SettingEditResult::Accepted);
        CHECK(api.settingsUiSeq(name) == 3u);
        CHECK(drain(rec->decoder, inst) == "SETTINGS seq=3 callsign=G4ABC power=5 report=1\n");
        // The same value again is no change and no bump.
        CHECK(cascade::core::commitSettingEdit(api, name, *report, "1") ==
              SettingEditResult::Accepted);
        CHECK(api.settingsUiSeq(name) == 3u);

        // THE PLUGIN'S OWN WRITE (settingsSet, its client) does NOT move the
        // counter - but the host's field shows it.
        cascade::core::PluginApiClient& c =
            api.client(PluginUi::tuneKey(*rec), rec->name, rec->capabilities);
        CHECK(api.settingsSet(c, "callsign", "M0XYZ") == CASCADE_API_OK);
        CHECK(api.settingsUiSeq(name) == 3u);
        CHECK(cascade::core::settingFieldValue(api, name, *call) == "M0XYZ");
        CHECK(drain(rec->decoder, inst).empty());

        // Everything the host stored is in the snapshot the config saves.
        const cascade::core::PluginSettingsMap snap = api.settingsSnapshot();
        const auto it = snap.find(name);
        CHECK(it != snap.end() && it->second.count("report") == 1u &&
              it->second.at("report") == "1");

        // [B1] THE FIXTURE ALSO MODELS THE POSITIVE PATH, through the REAL
        // loaded module rather than a synthetic LoadedPlugin: it declares
        // CASCADE_CAP_RECEIVER_LOCATOR (models FT8/PSK Reporter), so its
        // client's get_state must see the receiver's grid square.
        CHECK((rec->capabilities & CASCADE_CAP_RECEIVER_LOCATOR) != 0u);
        ReceiverFacts rf;
        rf.rxPositionSet = true;
        rf.rxLatDeg = 51.5074;
        rf.rxLonDeg = -0.1278;
        api.publish(rf);
        CascadeReceiverState rs{};
        rs.structSize = static_cast<std::uint32_t>(sizeof(rs));
        CHECK(api.getState(c, &rs) == CASCADE_API_OK);
        CHECK(std::strcmp(rs.receiverLocator, "IO91wm") == 0);
        CHECK((rs.flags & CASCADE_STATE_LOCATOR_KNOWN) != 0u);

        // [P2] A PATH (0.99.74) goes down the same write path as any field: the plugin reads
        // back exactly the path text, settings_seq moves once, and a path over the field's
        // limit moves nothing.
        CHECK(cascade::core::settingFieldValue(api, name, *tool).empty());
        CHECK(cascade::core::commitSettingEdit(api, name, *tool, "C:\\tools\\nrsc5.exe") ==
              SettingEditResult::Accepted);
        CHECK(api.settingsUiSeq(name) == 4u);
        CHECK(drain(rec->decoder, inst) ==
              "SETTINGS seq=4 callsign=M0XYZ power=5 report=1 tool=C:\\tools\\nrsc5.exe\n");
        CHECK(cascade::core::commitSettingEdit(api, name, *tool, std::string(261, 'p')) ==
              SettingEditResult::TooLong);
        CHECK(api.settingsUiSeq(name) == 4u);
        CHECK(drain(rec->decoder, inst).empty());

        rec->decoder->destroy(inst);

        testWidget(api, name, fields);
        testFlushOnDisappear(api, name, fields);
        testBrowseKey(api, name, fields);

        // [Minor] StoreFull is told apart from a generic StoreError: fill
        // this plugin's store to its key limit (CASCADE_MAX_SETTINGS_PER_
        // PLUGIN, 64) with keys the form never reads - only the COUNT
        // matters - then one more edit is refused specifically as "full".
        // AFTER testWidget, which reads its own settings_seq baseline
        // itself: these writes bump it, but nothing here depends on the
        // absolute value the way the sections above do.
        {
            const std::size_t before = api.settingsSnapshot().count(name)
                                           ? api.settingsSnapshot().at(name).size()
                                           : 0u;
            for (std::size_t i = before; i < CASCADE_MAX_SETTINGS_PER_PLUGIN; ++i) {
                const std::string key = "filler" + std::to_string(i);
                CHECK(api.settingsUiSet(name, key.c_str(), "x") == CASCADE_API_OK);
            }
            CHECK(api.settingsSnapshot().at(name).size() == CASCADE_MAX_SETTINGS_PER_PLUGIN);
            SettingField extra;
            extra.key = "onemore";
            extra.kind = CASCADE_SETTING_TEXT;
            extra.maxLength = 8;
            CHECK(cascade::core::commitSettingEdit(api, name, extra, "y") ==
                  SettingEditResult::StoreFull);
        }
    }
    host.unloadAll();
}

// ---------------------------------------------------------------------------
// [O] the old plugin: a CascadeReceiverState from before receiverLocator
// ---------------------------------------------------------------------------

// The layout EVERY plugin published before 0.99.43 was compiled with, written
// out by hand - not taken from the header, so this proves what the HOST does
// with a smaller struct at run time, whatever header the test is built with.
struct OldReceiverState {
    std::uint32_t structSize;
    std::uint32_t flags;
    std::uint64_t seq;
    std::uint64_t tuneSeq;
    std::uint64_t modeSeq;
    std::uint64_t deviceSeq;
    std::uint64_t audioSeq;
    double centreHz;
    double vfoOffsetHz;
    double tunedHz;
    double sampleRateHz;
    double bandwidthHz;
    double squelchDb;
    double volume;
    double signalDb;
    double sMeter;
    std::uint32_t demodMode;
    std::uint32_t gainCount;
    char deviceName[64];
};
// The mirror must end exactly where the appended field begins; if it does
// not, this test is not testing the old layout at all.
static_assert(sizeof(OldReceiverState) == offsetof(CascadeReceiverState, receiverLocator),
              "OldReceiverState must mirror the pre-0.99.43 layout");
static_assert(sizeof(OldReceiverState) < sizeof(CascadeReceiverState),
              "the host's struct must be the larger one for this test to mean anything");

const CascadeHostApi* g_oldHost = nullptr;
void oldAttach(const CascadeHostApi* h) { g_oldHost = h; }
const CascadeHostClientApi kOldHc = {static_cast<std::uint32_t>(sizeof(CascadeHostClientApi)),
                                     &oldAttach};

// A SECOND client, distinct from the one above, that DOES declare
// CASCADE_CAP_RECEIVER_LOCATOR - the positive half of the B1 fix. Two
// separate attach callbacks and globals so ui.rebuild(), which keys bridges
// by file name, gives each its own CascadeHostApi table to call get_state
// through.
const CascadeHostApi* g_locatorHost = nullptr;
void locatorAttach(const CascadeHostApi* h) { g_locatorHost = h; }
const CascadeHostClientApi kLocatorHc = {static_cast<std::uint32_t>(sizeof(CascadeHostClientApi)),
                                         &locatorAttach};

void testOldPlugin() {
    std::printf("[O] a pre-0.99.43 plugin's smaller CascadeReceiverState, and "
               "[B] CASCADE_CAP_RECEIVER_LOCATOR gating\n");
    // [B1] NOT DECLARED: a HOST_CLIENT-only plugin, exactly the shape every
    // pre-0.99.43 plugin has. Must get NOTHING from receiverLocator, however
    // good the receiver's position is - the blocker this section exists to
    // pin (get_state used to fill it for every host-client plugin whether it
    // asked for the bit or not).
    LoadedPlugin p;
    p.loaded = true;
    p.name = "Old";
    p.version = "1.0.0";
    p.path = "C:/plugins/old.dll";
    p.capabilities = CASCADE_CAP_HOST_CLIENT;
    p.hostClient = &kOldHc;
    // [B1] DECLARED: otherwise identical, so the ONLY difference between the
    // two plugins' get_state answers is this one bit.
    LoadedPlugin p2;
    p2.loaded = true;
    p2.name = "WithLocator";
    p2.version = "1.0.0";
    p2.path = "C:/plugins/withlocator.dll";
    p2.capabilities = CASCADE_CAP_HOST_CLIENT | CASCADE_CAP_RECEIVER_LOCATOR;
    p2.hostClient = &kLocatorHc;
    PluginUi ui;
    ui.rebuild({p, p2});
    CHECK(g_oldHost != nullptr);
    CHECK(g_locatorHost != nullptr);
    if (g_oldHost == nullptr || g_locatorHost == nullptr) { return; }

    ReceiverFacts f;
    f.running = true;
    f.deviceOpen = true;
    f.centreHz = 14.074e6;
    f.vfoOffsetHz = 1500.0;
    f.sampleRateHz = 2.4e6;
    f.bandwidthHz = 3000.0;
    f.squelchDb = -100.0;
    f.signalDb = -40.0;
    f.volume = 0.25;
    f.demodMode = CASCADE_DEMOD_USB;
    std::snprintf(f.deviceName, sizeof(f.deviceName), "%s", "Airspy HF+");
    f.rxPositionSet = true;
    f.rxLatDeg = 51.5074;
    f.rxLonDeg = -0.1278;
    ui.api().publish(f);

    // The old struct inside a larger buffer painted with a sentinel: a host
    // that wrote its own sizeof would overwrite the bytes after it.
    unsigned char buf[sizeof(OldReceiverState) + 64];
    std::memset(buf, 0xAB, sizeof(buf));
    OldReceiverState old{};
    old.structSize = static_cast<std::uint32_t>(sizeof(OldReceiverState));
    std::memcpy(buf, &old, sizeof(old));
    const std::int32_t rc =
        g_oldHost->get_state(g_oldHost->ctx, reinterpret_cast<CascadeReceiverState*>(buf));
    CHECK(rc == CASCADE_API_OK);  // the regression: this was BAD_ARGUMENT
    std::memcpy(&old, buf, sizeof(old));
    CHECK(old.structSize == sizeof(OldReceiverState));  // told how much it got
    CHECK(old.centreHz == 14.074e6);
    CHECK(old.vfoOffsetHz == 1500.0);
    CHECK(old.tunedHz == 14.0755e6);
    CHECK(old.bandwidthHz == 3000.0);
    CHECK(old.volume == 0.25);
    CHECK(old.demodMode == CASCADE_DEMOD_USB);
    CHECK(std::strcmp(old.deviceName, "Airspy HF+") == 0);
    CHECK((old.flags & CASCADE_STATE_RUNNING) != 0u);
    CHECK(old.seq >= 1u);
    bool untouched = true;
    for (std::size_t i = sizeof(OldReceiverState); i < sizeof(buf); ++i) {
        untouched = untouched && buf[i] == 0xAB;
    }
    CHECK(untouched);  // not one byte past the plugin's own struct

    // [B1] A NEW plugin that did NOT declare CASCADE_CAP_RECEIVER_LOCATOR
    // (g_oldHost/p, CASCADE_CAP_HOST_CLIENT only) gets NOTHING from the
    // field, even though a position is set and even though its struct is the
    // full current size - the exact bug B1 named: this used to read
    // "IO91wm" here.
    CascadeReceiverState s{};
    s.structSize = static_cast<std::uint32_t>(sizeof(s));
    CHECK(g_oldHost->get_state(g_oldHost->ctx, &s) == CASCADE_API_OK);
    CHECK(s.structSize == sizeof(CascadeReceiverState));
    CHECK(s.receiverLocator[0] == '\0');
    CHECK((s.flags & CASCADE_STATE_LOCATOR_KNOWN) == 0u);

    // [B1] The OTHER plugin (g_locatorHost/p2), which DID declare the bit,
    // gets the locator for the exact same published position.
    CascadeReceiverState ls{};
    ls.structSize = static_cast<std::uint32_t>(sizeof(ls));
    CHECK(g_locatorHost->get_state(g_locatorHost->ctx, &ls) == CASCADE_API_OK);
    CHECK(std::strcmp(ls.receiverLocator, "IO91wm") == 0);
    CHECK((ls.flags & CASCADE_STATE_LOCATOR_KNOWN) != 0u);

    // [0.99.75] EVERY state this host fills says it LISTS TARGETS HEARD WITHOUT A
    // POSITION (CASCADE_STATE_LISTS_UNPLACED_TRACKS): a plugin may publish a
    // CASCADE_TRACK_FLAG_NO_POSITION track only when it sees this bit, so the bit
    // must be on for the plugin that declared nothing, for the one that declared the
    // locator, and for a plugin whose struct stops before the locator field. The value is the
    // contract (the plugins repository carries the same line), and it is a new bit.
    static_assert(CASCADE_STATE_LISTS_UNPLACED_TRACKS == 0x00008000u,
                  "the state flag's value is part of the contract");
    static_assert((CASCADE_STATE_LISTS_UNPLACED_TRACKS &
                   (CASCADE_STATE_RUNNING | CASCADE_STATE_DEVICE_OPEN | CASCADE_STATE_MUTED |
                    CASCADE_STATE_DEVICE_AGC | CASCADE_STATE_AGC_SUPPORTED |
                    CASCADE_STATE_SQUELCH_OPEN | CASCADE_STATE_STEREO | CASCADE_STATE_TUNE_GRANTED |
                    CASCADE_STATE_SETTINGS_GRANTED | CASCADE_STATE_STOPPED |
                    CASCADE_STATE_LOCATOR_KNOWN | CASCADE_STATE_TX_GRANTED |
                    CASCADE_STATE_TX_ARMED | CASCADE_STATE_TRANSMITTING)) == 0u,
                  "a new bit, not an old one reused");
    static_assert(CASCADE_TRACK_FLAG_NO_POSITION == 0x00000004u,
                  "the track flag's value is part of the contract");
    CHECK((s.flags & CASCADE_STATE_LISTS_UNPLACED_TRACKS) != 0u);
    CHECK((ls.flags & CASCADE_STATE_LISTS_UNPLACED_TRACKS) != 0u);
    CHECK((old.flags & CASCADE_STATE_LISTS_UNPLACED_TRACKS) != 0u);

    // [0.99.77] EVERY state this host fills says it DRAWS A PANEL ROW THAT CARRIES
    // CASCADE_ROW_FLAG_KEY AS A KEY and delivers its press (CASCADE_STATE_ROW_KEYS), on the same
    // terms as the bit above: a plugin shows "press to choose" wording only when it sees this one,
    // so it must be on for the plugin that declared nothing, for the one that declared the
    // locator, and for a plugin whose struct stops before the locator field - and it is a new bit.
    static_assert(CASCADE_STATE_ROW_KEYS == 0x00010000u,
                  "the state flag's value is part of the contract");
    static_assert((CASCADE_STATE_ROW_KEYS &
                   (CASCADE_STATE_RUNNING | CASCADE_STATE_DEVICE_OPEN | CASCADE_STATE_MUTED |
                    CASCADE_STATE_DEVICE_AGC | CASCADE_STATE_AGC_SUPPORTED |
                    CASCADE_STATE_SQUELCH_OPEN | CASCADE_STATE_STEREO | CASCADE_STATE_TUNE_GRANTED |
                    CASCADE_STATE_SETTINGS_GRANTED | CASCADE_STATE_STOPPED |
                    CASCADE_STATE_LOCATOR_KNOWN | CASCADE_STATE_TX_GRANTED |
                    CASCADE_STATE_TX_ARMED | CASCADE_STATE_TRANSMITTING |
                    CASCADE_STATE_LISTS_UNPLACED_TRACKS)) == 0u,
                  "a new bit, not an old one reused");
    CHECK((s.flags & CASCADE_STATE_ROW_KEYS) != 0u);
    CHECK((ls.flags & CASCADE_STATE_ROW_KEYS) != 0u);
    CHECK((old.flags & CASCADE_STATE_ROW_KEYS) != 0u);

    // [M2] A position change must advance `seq`, or a plugin that re-reads
    // only when seq moves would never notice "Set RX here" or a GPS fix -
    // there is no dedicated locator counter, so this is the ONLY signal.
    // Moves to Manchester (53.4808, -2.2426) - far enough from London to land
    // in a different square, not just a different subsquare - to prove it is
    // the POSITION that is compared, not just rxPositionSet's boolean.
    const std::uint64_t seqBefore = ls.seq;
    f.rxLatDeg = 53.4808;
    f.rxLonDeg = -2.2426;
    ui.api().publish(f);
    CascadeReceiverState moved{};
    moved.structSize = static_cast<std::uint32_t>(sizeof(moved));
    CHECK(g_locatorHost->get_state(g_locatorHost->ctx, &moved) == CASCADE_API_OK);
    CHECK(moved.seq == seqBefore + 1u);
    CHECK(std::strcmp(moved.receiverLocator, "IO91wm") != 0);  // a different grid

    // No position: empty and unflagged for the DECLARING plugin too, never
    // 0,0's "JJ00aa" - isolates "no position" from "no bit" by testing it
    // against the plugin that actually has the bit. Also advances seq again,
    // for the same M2 reason: a position being CLEARED is a change too.
    const std::uint64_t seqBeforeClear = moved.seq;
    f.rxPositionSet = false;
    ui.api().publish(f);
    CascadeReceiverState u{};
    u.structSize = static_cast<std::uint32_t>(sizeof(u));
    CHECK(g_locatorHost->get_state(g_locatorHost->ctx, &u) == CASCADE_API_OK);
    CHECK(u.seq == seqBeforeClear + 1u);
    CHECK(u.receiverLocator[0] == '\0');
    CHECK((u.flags & CASCADE_STATE_LOCATOR_KNOWN) == 0u);

    // The only floor left: a size that cannot even hold structSize.
    std::uint32_t tiny[1] = {2u};
    CHECK(g_oldHost->get_state(g_oldHost->ctx, reinterpret_cast<CascadeReceiverState*>(tiny)) ==
          CASCADE_API_BAD_ARGUMENT);
    CHECK(g_oldHost->get_state(g_oldHost->ctx, nullptr) == CASCADE_API_BAD_ARGUMENT);
}

}  // namespace

int main(int argc, char** argv) {
    // THE UI CENSUS ON, for [P3]: the form notes the BROWSE key it drew
    // ("settings:browse:<key>" and its rectangle) only when FOXSDR_UI_CENSUS names
    // a file, and the census reads the variable ONCE, at the first question - so it
    // is set before anything else runs. A file of its own per process.
    g_censusPath = (std::filesystem::temp_directory_path() /
                    ("cascade-settings-ui-census-" + std::to_string(
#if defined(_WIN32)
                         _getpid()
#else
                         getpid()
#endif
                             ) + ".txt")).string();
#if defined(_WIN32)
    _putenv_s("FOXSDR_UI_CENSUS", g_censusPath.c_str());
#else
    setenv("FOXSDR_UI_CENSUS", g_censusPath.c_str(), 1);
#endif
    testMaidenhead();
    testValidation();
    testFields();
    testEdits();
    testOldPlugin();
    if (argc > 1) {
        testRealModule(argv[1]);
    } else {
        // Not a silent pass: without the module the most important section
        // did not run, and the run says so by failing.
        std::printf("FAIL: no fixture module directory on the command line\n");
        CHECK(false);
    }
    std::error_code rmEc;
    std::filesystem::remove(g_censusPath, rmEc);
    return testSummary("test_plugin_settings_ui");
}
