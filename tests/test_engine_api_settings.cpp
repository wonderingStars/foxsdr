// test_engine_api_settings.cpp - A PLUGIN'S SETTINGS FORM through the table (stub campaign B9; API 0.3 9.7), held against the real engine and the REAL settings
// fixture module (tests/fixtures/settings_ui_plugin.cpp) - the way test_engine_api_store holds the store: through the C table and the engine's own passes, with no
// window anywhere. The window's half (the boxes, the commit-on-leave rule across the round trip) is tests/test_plugin_settings_ui.cpp [S] and the real-binary
// tests test_decoder_settings_fold_app and test_plugin_pathpick_app; what is held HERE is everything the window relies on the engine for.
//
// One group per rule; each was shown red against a mutant of the code that holds it (docs/engine-stage4.md 6.22, B9):
//   F1  THE LIST AND ITS FACTS: PLUGIN_SETTINGS has one entry per field the host can store (the fixture declares seven, three are skipped), in the plugin's order, with
//       the label, the module file, the kind and the longest value; fact.plugin.setting.<id> is the whole value shown (the default until something is stored) and
//       .spec is "<key> US <placeholder>"; a REMOTE session is DENIED the list and sees none of the facts;
//   E1  AN ACCEPTED EDIT: answered OK with applied[0] 0, written through the plugin API's one path - the plugin's settings_seq moves ONCE for it, the fact and the
//       list follow (listSeq moves), a PATH longer than a command's 255 bytes travels as long text, an empty value is a value (a cleared box is not a deletion), and the
//       same value again is accepted and moves nothing;
//   E2  A REFUSED EDIT: a number that is not one, a value too long, a character a setting cannot hold, a field the module does not declare, a module that is not loaded -
//       each answered with its verdict in applied[0] and its status, nothing stored, settings_seq unmoved, the fact as it was;
//   E3  WHO MAY SEND IT: the op is an application op, so a REMOTE session is DENIED; a LOCAL session without the SETTINGS grant is DENIED; a malformed edit is BAD_ARGUMENT;
//   E4  THE PLUGIN'S OWN WRITE (settings_set) changes what the form shows: the fact and the list follow, and settings_seq does not move (it is the HOST's edits that
//       move it);
//   E5  NOTHING MOVES WHEN NOTHING CHANGED: a settled engine's listSeq does not move pass after pass, and an edit that changes nothing does not move it either.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "check.hpp"
#include "core/app_commands.hpp"
#include "core/package_identity.hpp"
#include "core/plugin_abi.h"
#include "core/plugin_api.hpp"
#include "core/plugin_settings_ui.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine_client.hpp"
#include "foxsdr_api.h"

#ifndef FIXTURE_SETTINGS_UI_PLUGIN
#error "FIXTURE_SETTINGS_UI_PLUGIN is defined by tests/CMakeLists.txt"
#endif

namespace fs = std::filesystem;
using cascade::core::SettingEditResult;

namespace cascade::gui {
// The engine's friend, as every test that reaches an engine member is.
struct AppWindowTestAccess {
    using Engine = cascade::engine::Engine;
    static void installHook(std::string (*dir)()) { Engine::testHooks_.pluginDir = dir; }
    static cascade::core::PluginApiCore& core(Engine& e) { return *e.pluginApi_; }
};
}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

namespace {

constexpr const char* kHooks = "testHooks=1;token=s3cret";

const FoxEngineApi* api() { return cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR); }

fs::path g_scratch;
fs::path g_plugins;
std::string pluginDirHook() { return g_plugins.string(); }

#if defined(_WIN32)
const char* const kModExt = ".dll";
#elif defined(__APPLE__)
const char* const kModExt = ".dylib";
#else
const char* const kModExt = ".so";
#endif

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    ::SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

const std::string kFile = std::string("settings_ui_fixture") + kModExt;  // the module's file name: what PLUGINS lists as its detail, and the edit's module
const std::string kName = "Settings Form Fixture";                       // its descriptor name: the settings store's key

void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = fs::temp_directory_path() / ("foxsdr_engine_api_settings_" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    fs::create_directories(g_scratch, ec);
    const std::string s = g_scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) { setEnv(v, s); }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_CRASH_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_UPDATE_URL", "http://127.0.0.1:9");
    g_plugins = g_scratch / "plugins";
    fs::create_directories(g_plugins, ec);
    Access::installHook(&pluginDirHook);
    fs::copy_file(fs::path(FIXTURE_SETTINGS_UI_PLUGIN), g_plugins / kFile, fs::copy_options::overwrite_existing, ec);
    CHECK(!ec);
}

std::string fact(testing::Session& s, const std::string& key, int32_t* status = nullptr) {
    char buf[8192];
    const int32_t n = api()->get_setting(s.raw(), ("fact." + key).c_str(), buf, sizeof(buf));
    if (status != nullptr) { *status = n; }
    return n >= 0 ? std::string(buf) : std::string();
}

int32_t count(testing::Session& s, uint32_t list) { return api()->read_list(s.raw(), list, 0, nullptr); }

FoxListItem item(testing::Session& s, uint32_t list, uint32_t index) {
    FoxListItem it{};
    it.structSize = sizeof(it);
    api()->read_list(s.raw(), list, index, &it);
    return it;
}

void passes(cascade::engine::Engine& eng, int n) {
    for (int i = 0; i < n; ++i) { eng.controlPass(); }
}

// Submits ONE command on `session` - the text as a command's own 255 bytes when it fits, as long text beside it when it does not - and passes until its
// result lands.
testing::Outcome pumpedText(cascade::engine::Engine& eng, FoxSession* session, uint32_t op, const std::string& text) {
    testing::Outcome o;
    FoxCommand c = testing::cmd(op);
    FoxSubmitResult sr{};
    sr.structSize = sizeof(sr);
    int32_t n = 0;
    if (text.size() < FOXAPI_TEXT_CHARS) {
        std::snprintf(c.text, sizeof(c.text), "%s", text.c_str());
        n = api()->submit(session, &c, 1, &sr);
    } else {
        n = api()->submit_text(session, &c, text.data(), static_cast<uint32_t>(text.size()), &sr);
    }
    o.submitStatus = n < 0 ? n : sr.status;
    if (o.submitStatus != FOXAPI_OK) { return o; }
    for (int pass = 0; pass < 20 && !o.landed; ++pass) {
        eng.controlPass();
        FoxCommandResult r[16];
        for (auto& x : r) { x.structSize = sizeof(x); }
        const int32_t got = api()->poll_results(session, r, 16);
        for (int32_t i = 0; i < got; ++i) {
            if (r[i].ticket == sr.ticket) {
                o.landed = true;
                o.result = r[i];
            }
        }
    }
    return o;
}

// "<module file>\x1F<key>\x1F<value>": the edit as the window sends it.
std::string edit(const std::string& file, const std::string& key, const std::string& value) { return file + '\x1F' + key + '\x1F' + value; }

struct Direct {
    cascade::engine::Engine eng;
    Direct() {
        eng.configureApi(kHooks);
        eng.initialise();
        eng.controlPass();
    }
};

// The entry of PLUGIN_SETTINGS whose spec says `key` (0 when there is none): its index in the list.
int entryOf(testing::Session& s, const std::string& key) {
    for (int32_t i = 0; i < count(s, FOXAPI_LIST_PLUGIN_SETTINGS); ++i) {
        const FoxListItem it = item(s, FOXAPI_LIST_PLUGIN_SETTINGS, static_cast<uint32_t>(i));
        const std::string spec = fact(s, "plugin.setting." + std::to_string(it.id) + ".spec");
        if (spec.substr(0, spec.find('\x1F')) == key) { return static_cast<int>(i); }
    }
    return -1;
}

std::string valueOf(testing::Session& s, const std::string& key) {
    const int at = entryOf(s, key);
    if (at < 0) { return "<no such field>"; }
    return fact(s, "plugin.setting." + std::to_string(item(s, FOXAPI_LIST_PLUGIN_SETTINGS, static_cast<uint32_t>(at)).id));
}

// F1 ---------------------------------------------------------------------------------------------------------------------------------------

void testListAndFacts() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_SETTINGS);
    CHECK(s.ok());
    testing::Session remote(api(), d.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW, "s3cret");
    CHECK(remote.ok());
    passes(d.eng, 3);

    // Four fields the host can store (the fixture declares seven: a later host's kind, a later host's entry size and a repeated key are skipped).
    CHECK_EQ(count(s, FOXAPI_LIST_PLUGIN_SETTINGS), 4);
    struct Want {
        const char* key;
        const char* label;
        double kind, maxLength;
        const char* placeholder;
        const char* value;
    };
    // In the plugin's order. The label is the plugin's own text; callsign and tool_path say what a box shows before anything is stored (the default).
    const Want want[] = {
        {"callsign", "Callsign", CASCADE_SETTING_TEXT, 12.0, "e.g. G4ABC", ""},
        {"power", "Power (W)", CASCADE_SETTING_NUMBER, static_cast<double>(cascade::core::kMaxNumberChars), "watts", "5"},
        {"tool_path", "Tool path", CASCADE_SETTING_PATH, 260.0, "C:\\tools\\tool.exe", ""},
        {"report", "Report spots", CASCADE_SETTING_BOOL, 1.0, "", ""},
    };
    for (int32_t i = 0; i < 4; ++i) {
        const FoxListItem it = item(s, FOXAPI_LIST_PLUGIN_SETTINGS, static_cast<uint32_t>(i));
        const std::string id = std::to_string(it.id);
        CHECK_EQ(it.id, static_cast<uint64_t>(i + 1));
        CHECK_EQ(std::string(it.detail), kFile);
        CHECK_EQ(std::string(it.name), std::string(want[i].label));
        CHECK_EQ(it.value[0], want[i].kind);
        CHECK_EQ(it.value[1], want[i].maxLength);
        const std::string spec = fact(s, "plugin.setting." + id + ".spec");
        CHECK_EQ(spec, std::string(want[i].key) + '\x1F' + want[i].placeholder);
        CHECK_EQ(fact(s, "plugin.setting." + id), std::string(want[i].value));
    }
    CHECK_EQ(count(s, FOXAPI_LIST_PLUGIN_SETTINGS), 4);

    // LOCAL ONLY: what a person types into a box is the local operator's.
    CHECK_EQ(count(remote, FOXAPI_LIST_PLUGIN_SETTINGS), FOXAPI_DENIED);
    int32_t st = 0;
    (void)fact(remote, "plugin.setting.1", &st);
    CHECK(st < 0);
    (void)fact(remote, "plugin.setting.1.spec", &st);
    CHECK(st < 0);
    std::printf("F1: PLUGIN_SETTINGS lists the four fields in the plugin's order with the facts for each; a REMOTE session sees none of it\n");
}

// E1 ---------------------------------------------------------------------------------------------------------------------------------------

void testAcceptedEdit() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_SETTINGS);
    CHECK(s.ok());
    passes(d.eng, 3);
    cascade::core::PluginApiCore& core = Access::core(d.eng);
    const std::uint64_t seq0 = core.settingsUiSeq(kName);
    uint64_t listSeq = s.state().listSeq;

    // ONE ACCEPTED EDIT: answered OK, the plugin's settings_seq moves once, the fact and the list follow.
    testing::Outcome o = pumpedText(d.eng, s.raw(), FOXAPP_OP_PLUGIN_SETTING_EDIT, edit(kFile, "callsign", "G4ABC"));
    CHECK(o.landed);
    CHECK_EQ(o.result.status, FOXAPI_OK);
    CHECK_EQ(o.result.applied[0], 0.0);
    CHECK_EQ(core.settingsUiSeq(kName), seq0 + 1u);
    passes(d.eng, 2);
    CHECK_EQ(valueOf(s, "callsign"), std::string("G4ABC"));
    CHECK(s.state().listSeq != listSeq);
    std::string stored;
    CHECK_EQ(core.settingsUiGet(kName, "callsign", stored), CASCADE_API_OK);
    CHECK_EQ(stored, std::string("G4ABC"));

    // THE SAME VALUE AGAIN: accepted, and nothing moves - not the plugin's counter, not the list.
    listSeq = s.state().listSeq;
    o = pumpedText(d.eng, s.raw(), FOXAPP_OP_PLUGIN_SETTING_EDIT, edit(kFile, "callsign", "G4ABC"));
    CHECK_EQ(o.result.status, FOXAPI_OK);
    CHECK_EQ(core.settingsUiSeq(kName), seq0 + 1u);
    passes(d.eng, 3);
    CHECK_EQ(s.state().listSeq, listSeq);

    // A NUMBER is stored as the trimmed text the person typed; a BOOL as "1" or "".
    o = pumpedText(d.eng, s.raw(), FOXAPP_OP_PLUGIN_SETTING_EDIT, edit(kFile, "power", "  25 "));
    CHECK_EQ(o.result.status, FOXAPI_OK);
    passes(d.eng, 2);
    CHECK_EQ(valueOf(s, "power"), std::string("25"));
    CHECK_EQ(core.settingsUiSeq(kName), seq0 + 2u);
    o = pumpedText(d.eng, s.raw(), FOXAPP_OP_PLUGIN_SETTING_EDIT, edit(kFile, "report", "1"));
    CHECK_EQ(o.result.status, FOXAPI_OK);
    passes(d.eng, 2);
    CHECK_EQ(valueOf(s, "report"), std::string("1"));
    o = pumpedText(d.eng, s.raw(), FOXAPP_OP_PLUGIN_SETTING_EDIT, edit(kFile, "report", "yes"));  // anything that is not "1" is unchecked
    CHECK_EQ(o.result.status, FOXAPI_OK);
    passes(d.eng, 2);
    CHECK_EQ(valueOf(s, "report"), std::string());
    CHECK_EQ(core.settingsUiSeq(kName), seq0 + 4u);

    // A CLEARED BOX IS A VALUE, NOT A DELETION: the callsign's default is "" so use the number, whose default is 5 - cleared, it reads "" and not 5.
    o = pumpedText(d.eng, s.raw(), FOXAPP_OP_PLUGIN_SETTING_EDIT, edit(kFile, "power", ""));
    CHECK_EQ(o.result.status, FOXAPI_OK);
    passes(d.eng, 2);
    CHECK_EQ(valueOf(s, "power"), std::string());

    // A PATH THAT DOES NOT FIT A COMMAND'S 255 BYTES travels as long text, whole.
    const std::string longPath = "C:\\" + std::string(245, 'p') + "\\nrsc5.exe";  // 257 bytes: past a command's 255, within the field's 260
    CHECK(longPath.size() > 255u && longPath.size() <= 260u);
    o = pumpedText(d.eng, s.raw(), FOXAPP_OP_PLUGIN_SETTING_EDIT, edit(kFile, "tool_path", longPath));
    CHECK(o.landed);
    CHECK_EQ(o.result.status, FOXAPI_OK);
    passes(d.eng, 2);
    CHECK_EQ(valueOf(s, "tool_path"), longPath);
    std::printf("E1: an accepted edit is answered OK, moves settings_seq once and the fact and list follow; the same value moves nothing; a long path travels whole\n");
}

// E2 ---------------------------------------------------------------------------------------------------------------------------------------

void testRefusedEdits() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_SETTINGS);
    CHECK(s.ok());
    passes(d.eng, 3);
    cascade::core::PluginApiCore& core = Access::core(d.eng);
    CHECK_EQ(pumpedText(d.eng, s.raw(), FOXAPP_OP_PLUGIN_SETTING_EDIT, edit(kFile, "callsign", "M0XYZ")).result.status, FOXAPI_OK);
    passes(d.eng, 2);
    const std::uint64_t seq0 = core.settingsUiSeq(kName);
    const uint64_t listSeq = s.state().listSeq;

    struct Case {
        const char* what;
        std::string text;
        int32_t status;
        double verdict;
    };
    const Case cases[] = {
        {"a number that is not one", edit(kFile, "power", "12x"), FOXAPI_BAD_ARGUMENT, static_cast<double>(SettingEditResult::NotANumber)},
        {"a number too long", edit(kFile, "power", std::string(40, '1')), FOXAPI_OUT_OF_RANGE, static_cast<double>(SettingEditResult::TooLong)},
        {"a value longer than the field", edit(kFile, "callsign", "G4ABCDEFGHIJK"), FOXAPI_OUT_OF_RANGE, static_cast<double>(SettingEditResult::TooLong)},
        {"a control character", edit(kFile, "callsign", "G4\tAB"), FOXAPI_BAD_ARGUMENT, static_cast<double>(SettingEditResult::BadText)},
        {"a separator inside the value", edit(kFile, "callsign", std::string("G4") + '\x1F' + "AB"), FOXAPI_BAD_ARGUMENT, static_cast<double>(SettingEditResult::BadText)},
        {"a path past the field's limit", edit(kFile, "tool_path", std::string(261, 'p')), FOXAPI_OUT_OF_RANGE, static_cast<double>(SettingEditResult::TooLong)},
        {"a field the module does not declare", edit(kFile, "nonesuch", "x"), FOXAPI_NOT_FOUND, 0.0},
        {"a field the host skipped (a later kind)", edit(kFile, "future", "x"), FOXAPI_NOT_FOUND, 0.0},
        {"a module that is not loaded", edit("nobody.dll", "callsign", "x"), FOXAPI_NOT_FOUND, 0.0},
    };
    for (const Case& c : cases) {
        const testing::Outcome o = pumpedText(d.eng, s.raw(), FOXAPP_OP_PLUGIN_SETTING_EDIT, c.text);
        if (!o.landed || o.result.status != c.status) { std::printf("  FAILED CASE: %s (status %d, wanted %d)\n", c.what, o.result.status, c.status); }
        CHECK(o.landed);
        CHECK_EQ(o.result.status, c.status);
        CHECK((o.result.flags & FOXAPI_RESULT_REFUSED) != 0u);
        CHECK(o.result.message[0] != '\0');
        if (c.verdict != 0.0) { CHECK_EQ(o.result.applied[0], c.verdict); }
    }
    // NOTHING WAS STORED AND NOTHING MOVED: not the plugin's counter, not the facts, not the list.
    passes(d.eng, 3);
    CHECK_EQ(core.settingsUiSeq(kName), seq0);
    CHECK_EQ(valueOf(s, "callsign"), std::string("M0XYZ"));
    CHECK_EQ(valueOf(s, "power"), std::string("5"));
    CHECK_EQ(valueOf(s, "tool_path"), std::string());
    CHECK_EQ(s.state().listSeq, listSeq);
    std::printf("E2: nine refused edits are each answered with their verdict and status, and nothing is stored or moved\n");
}

// E3 ---------------------------------------------------------------------------------------------------------------------------------------

void testWhoMaySend() {
    Direct d;
    testing::Session local(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_SETTINGS);
    testing::Session noSettings(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW);
    testing::Session remote(api(), d.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_SETTINGS | FOXAPI_GRANT_ADMIN, "s3cret");
    CHECK(local.ok() && noSettings.ok() && remote.ok());
    passes(d.eng, 3);
    cascade::core::PluginApiCore& core = Access::core(d.eng);
    const std::uint64_t seq0 = core.settingsUiSeq(kName);

    // THE OP IS THE APPLICATION'S OWN: a REMOTE session is DENIED it, whatever it holds.
    testing::Outcome o = pumpedText(d.eng, remote.raw(), FOXAPP_OP_PLUGIN_SETTING_EDIT, edit(kFile, "callsign", "G4ABC"));
    CHECK(o.submitStatus == FOXAPI_DENIED || (o.landed && o.result.status == FOXAPI_DENIED));
    // ...and a LOCAL session needs the SETTINGS grant.
    o = pumpedText(d.eng, noSettings.raw(), FOXAPP_OP_PLUGIN_SETTING_EDIT, edit(kFile, "callsign", "G4ABC"));
    CHECK(o.submitStatus == FOXAPI_DENIED || (o.landed && o.result.status == FOXAPI_DENIED));
    CHECK_EQ(core.settingsUiSeq(kName), seq0);

    // A MALFORMED EDIT names no module and field.
    for (const std::string& bad : {std::string("callsign"), std::string("a\x1F" "b"), std::string("\x1F" "callsign\x1F" "x"), std::string(kFile + "\x1F\x1F" + "x")}) {
        o = pumpedText(d.eng, local.raw(), FOXAPP_OP_PLUGIN_SETTING_EDIT, bad);
        CHECK(o.landed);
        CHECK_EQ(o.result.status, FOXAPI_BAD_ARGUMENT);
    }
    CHECK_EQ(core.settingsUiSeq(kName), seq0);
    // (The op is in kOps, an application op, and not in the web profile's closure: test_apply_command and test_engine_api_03 hold the table itself.)
    CHECK(cascade::core::cmd::isKnownOp(FOXAPP_OP_PLUGIN_SETTING_EDIT));
    CHECK(cascade::core::cmd::isAppOp(FOXAPP_OP_PLUGIN_SETTING_EDIT));
    std::printf("E3: a REMOTE session and a session without the SETTINGS grant are DENIED; a malformed edit is BAD_ARGUMENT; nothing moved\n");
}

// E4 ---------------------------------------------------------------------------------------------------------------------------------------

void testPluginsOwnWrite() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_SETTINGS);
    CHECK(s.ok());
    passes(d.eng, 3);
    cascade::core::PluginApiCore& core = Access::core(d.eng);
    const std::uint64_t seq0 = core.settingsUiSeq(kName);
    const uint64_t listSeq = s.state().listSeq;

    // The plugin writes its own value (settings_set, through its client): the form shows it, and the plugin's counter - the HOST's edits' - is not moved.
    cascade::core::PluginApiClient& c = core.client(kFile, kName, CASCADE_CAP_SETTINGS_UI);
    CHECK_EQ(core.settingsSet(c, "callsign", "M0OWN"), CASCADE_API_OK);
    passes(d.eng, 3);
    CHECK_EQ(valueOf(s, "callsign"), std::string("M0OWN"));
    CHECK(s.state().listSeq != listSeq);
    CHECK_EQ(core.settingsUiSeq(kName), seq0);
    std::printf("E4: the plugin's own settings_set shows in the form (the fact and listSeq follow) and does not move settings_seq\n");
}

// E5 ---------------------------------------------------------------------------------------------------------------------------------------

void testSettled() {
    Direct d;
    testing::Session s(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_SETTINGS);
    CHECK(s.ok());
    passes(d.eng, 5);
    const uint64_t listSeq = s.state().listSeq;
    for (int i = 0; i < 25; ++i) { d.eng.controlPass(); }
    CHECK_EQ(s.state().listSeq, listSeq);
    CHECK_EQ(count(s, FOXAPI_LIST_PLUGIN_SETTINGS), 4);
    std::printf("E5: a settled engine's listSeq does not move\n");
}

}  // namespace

int main() {
    std::printf("test_engine_api_settings\n");
    isolate();
    testListAndFacts();
    testAcceptedEdit();
    testRefusedEdits();
    testWhoMaySend();
    testPluginsOwnWrite();
    testSettled();
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    return check::finish("test_engine_api_settings");
}
