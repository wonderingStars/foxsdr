// test_config_halves.cpp - THE OWNERSHIP OF THE SETTINGS FILE'S FIELDS (shell retirement R5; core/config_halves.hpp,
// docs/api-0.3-additions.md 9.16, docs/engine-stage4.md 6.25).
//
// THE PROPERTY. The settings file is one document with two writers, and every AppConfig field must belong to exactly one half: the engine's
// (Engine::fillConfig, telemetryJournal, ServerHost::store) or the interface's (AppWindow::buildInterfaceHalf, diagnosticsJournal,
// testerUsageJournal). Until R5 a field in neither was kept from the file as loaded - safe, silent, and the failure configsEqual's header warns
// of. Engine::publishConfig no longer starts from the loaded copy, so a field the table does not name is LOST from the snapshot (the headless
// round trip, tests/test_engine_main_config_roundtrip.cpp, goes red) - and THIS test goes red first, naming it.
//
//   1  THE TABLE IS COMPLETE: the AppConfig members, read from the text of src/core/config.hpp, and the table's names are the same set, each
//      once. (A new member belongs to nobody until it is placed: this fails and says which.)
//   2  THE TABLE IS HELD AGAINST THE WRITERS: an Engine row is assigned by the engine's functions (schemaVersion, a constant nobody assigns,
//      excepted) and never by the window's; an Interface row is assigned by the window's functions and never by the engine's; a Shared row
//      by both - and the set written by both is exactly {dbMin, dbMax}.
//   3  THE SELF-MOVING FIELDS are exactly the ten the window's configsEqual leaves out for moving by themselves (and none of them is in its
//      compare), written once.
//   4  EACH FIELD, BY ITSELF (the member pointers, every row): copyEngineHalf / copyInterfaceHalf move it iff its owner says so;
//      interfaceHalvesEqual sees it iff it is an Interface or Shared field that does not move; clearBookkeeping blanks it iff it moves.
//   5  THE DOCUMENT: a populated interface half (every Interface and Shared field off its default and a value the loader keeps) goes through
//      serializeInterfaceHalf and parseInterfaceHalf and comes out equal, field by field, the self-moving ones included; it carries no engine field;
//      the legacy map rectangle survives a document that has map pages; a document that is not JSON is refused.
//
// Break-it (docs/engine-stage4.md 6.25): a row deleted from the table -> 1 names it (and test_engine_main_config_roundtrip loses the field).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/config.hpp"
#include "core/config_halves.hpp"
#include "core/patch_io.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using json = nlohmann::json;
using cascade::core::AppConfig;
using cascade::core::ConfigOwner;
using cascade::core::ConfigStore;

namespace {

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The text of the function whose definition starts with `signature`: from the first '{' after it to its match.
std::string bodyAt(const std::string& s, const std::string& signature) {
    const std::size_t at = s.find(signature);
    if (at == std::string::npos) { return std::string(); }
    const std::size_t open = s.find('{', at);
    if (open == std::string::npos) { return std::string(); }
    int depth = 0;
    for (std::size_t i = open; i < s.size(); ++i) {
        if (s[i] == '{') { ++depth; }
        if (s[i] == '}' && --depth == 0) { return s.substr(open, i - open + 1); }
    }
    return std::string();
}

// Every `<var>.<name> =` (or `.clear()` / `[`) in `body`: the fields a function writes.
std::set<std::string> assignedFields(const std::string& body, const char* var) {
    std::set<std::string> out;
    const std::regex re(std::string("\\b") + var + "\\.([A-Za-z0-9_]+)\\s*(=[^=]|\\.clear\\(|\\[)");
    for (std::sregex_iterator it(body.begin(), body.end(), re), end; it != end; ++it) { out.insert((*it)[1].str()); }
    return out;
}

// --- 1. the members of AppConfig, from the header's text -----------------------------------------------------------------------------------

// `//` comments and the CONTENTS of string literals removed (a comma or a brace inside either is not structure).
std::string strippedCode(const std::string& s) {
    std::string out;
    bool inString = false;
    bool inLine = false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (inLine) {
            if (c == '\n') { inLine = false; out += c; }
            continue;
        }
        if (inString) {
            if (c == '\\') { ++i; continue; }
            if (c == '"') { inString = false; out += c; }
            continue;
        }
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') { inLine = true; continue; }
        if (c == '"') { inString = true; out += c; continue; }
        out += c;
    }
    return out;
}

std::string trimmed(const std::string& s) {
    std::size_t a = 0;
    std::size_t b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) { ++a; }
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) { --b; }
    return s.substr(a, b - a);
}

bool isIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

std::string lastIdentifier(const std::string& s) {
    std::size_t e = s.size();
    while (e > 0 && !isIdentChar(s[e - 1])) { --e; }
    std::size_t b = e;
    while (b > 0 && isIdentChar(s[b - 1])) { --b; }
    return s.substr(b, e - b);
}

std::string firstIdentifier(const std::string& s) {
    std::size_t b = 0;
    while (b < s.size() && !isIdentChar(s[b])) { ++b; }
    std::size_t e = b;
    while (e < s.size() && isIdentChar(s[e])) { ++e; }
    return s.substr(b, e - b);
}

// The non-static data members of `struct AppConfig`, in declaration order. A statement is read up to its ';' at the struct's own depth (a
// nested struct's body is skipped whole); its first declarator is the last identifier before any '=', and a further declarator in the same
// statement ("float dbMin = -110.0f, dbMax = 0.0f") follows a top-level comma after the first '='.
std::vector<std::string> appConfigMembers(const std::string& header) {
    std::vector<std::string> members;
    const std::string code = strippedCode(header);
    const std::size_t at = code.find("struct AppConfig {");
    if (at == std::string::npos) { return members; }
    int depth = 0;
    std::string stmt;
    for (std::size_t i = code.find('{', at); i < code.size(); ++i) {
        const char c = code[i];
        if (c == '{') {
            if (depth++ >= 1) { continue; }
            continue;
        }
        if (c == '}') {
            if (--depth == 0) { break; }
            continue;
        }
        if (depth != 1) { continue; }
        if (c != ';') {
            stmt += c;
            continue;
        }
        const std::string s = trimmed(stmt);
        stmt.clear();
        if (s.empty()) { continue; }
        const std::string first = firstIdentifier(s);
        if (first == "static" || first == "struct" || first == "using" || first == "enum" || first == "friend") { continue; }
        // the head: up to the first '=' that is not inside parentheses
        std::size_t eq = std::string::npos;
        int paren = 0;
        for (std::size_t k = 0; k < s.size(); ++k) {
            if (s[k] == '(') { ++paren; }
            if (s[k] == ')') { --paren; }
            if (s[k] == '=' && paren == 0) {
                eq = k;
                break;
            }
        }
        members.push_back(lastIdentifier(eq == std::string::npos ? s : s.substr(0, eq)));
        if (eq == std::string::npos) { continue; }
        // further declarators: a comma at depth 0 of (), <>, {} after the initialiser starts one
        int nest = 0;
        for (std::size_t k = eq; k < s.size(); ++k) {
            if (s[k] == '(' || s[k] == '<' || s[k] == '{') { ++nest; }
            if (s[k] == ')' || s[k] == '>' || s[k] == '}') { --nest; }
            if (s[k] == ',' && nest == 0) { members.push_back(firstIdentifier(s.substr(k + 1))); }
        }
    }
    return members;
}

// --- 4. a change to any field, whatever its type ------------------------------------------------------------------------------------------

template <class T>
concept MapLike = requires { typename T::mapped_type; };

template <class T>
void perturb(T& v) {
    if constexpr (std::is_same_v<T, bool>) {
        v = !v;
    } else if constexpr (std::is_integral_v<T>) {
        v = static_cast<T>(v + 3);
    } else if constexpr (std::is_floating_point_v<T>) {
        v += 0.5;
    } else if constexpr (std::is_same_v<T, std::string>) {
        v += "x";
    } else if constexpr (std::is_same_v<T, AppConfig::SoundCard>) {
        v.device += "x";
    } else if constexpr (MapLike<T>) {
        v.emplace("perturbed", typename T::mapped_type{});
    } else {
        v.emplace_back();   // a vector of anything: one more element, whatever it holds
    }
}

// The names of the fields on which two configs differ.
std::set<std::string> differing(const AppConfig& a, const AppConfig& b) {
    std::set<std::string> out;
    cascade::core::forEachConfigField([&](const char* name, ConfigOwner, bool, auto p) {
        if (!(a.*p == b.*p)) { out.insert(name); }
    });
    return out;
}

// --- 5. a populated interface half ----------------------------------------------------------------------------------------------------------

// A patch document the loader accepts (a magic line and a view line): what the patch canvas saves, in miniature.
std::string patchText(float zoom) {
    return std::string(cascade::core::patch::kPatchMagic) + " 1\nview 10 20 " + std::to_string(zoom) + "\n";
}

// EVERY Interface and Shared field set to a value that is not the default and that the loader keeps (the round trip's seed, tests/
// test_engine_main_config_roundtrip.cpp, for the window's half; the checks in main() refuse a field left at its default, so a field added to
// the table and not seeded here fails there and not silently).
AppConfig populated() {
    AppConfig c;
    c.dbMin = -100.0f;
    c.dbMax = -10.0f;
    c.waterfallRangeMode = "manual";
    c.splitRatio = 0.65f;
    c.bandPlanOverlay = false;
    c.patch = patchText(3.0f);
    c.patchPresets = {{"Airband", patchText(1.0f)}, {"Marine", patchText(2.0f)}};
    c.patchPresetPrevious = patchText(4.0f);
    c.mainView = "receiver";
    c.railCollapsed = true;
    c.statusCollapsed = true;
    c.displayOpen = true;
    c.radioSetupDontShow = true;
    c.bandPlanSize = "large";
    c.bandPlanPalette = "vivid";
    c.tunerDisplayStyle = "neon";
    c.meterStyleVolume = "led";
    c.meterStyleRate = "needle";
    c.uiTheme = "night";
    c.counterScale = 2;
    c.counterSwitches = false;
    c.readingsScale = 1.5f;
    c.interfaceScale = "150";
    c.language = "de";
    c.country = "DE";
    c.mapTrails = false;
    c.mapTrailAltitudeColours = false;
    c.bookmarkMarkers = false;
    c.bookmarkStackNames = false;
    c.spectrumTraceMode = "peak";
    c.spectrumAverageMs = 250;
    c.mapTrailStyle = 1;
    c.aircraftIconPx = 64;
    c.mapTrailWidthPx = 6;
    c.mapListOnScreenOnly = true;
    c.scopeMode = true;
    c.scopeRangeNm = 100;
    c.demodScopeOpen = true;
    c.demodScopeSignal = 2;
    c.demodScopeTimebase = 5;
    c.demodScopeGain = 4;
    c.demodScopeAutoGain = false;
    c.demodScopeDisplay = 1;
    c.railBank = 2;
    c.keyBindings = {"tune.up=Ctrl+Up", "record.toggle=Ctrl+R"};
    c.mapWindowWidth = 900;
    c.mapWindowHeight = 600;
    c.mapWindowX = 30;
    c.mapWindowY = 40;
    AppConfig::MapPage page;
    page.plugin = "Radar Sweep";
    page.x = 120;
    page.y = 80;
    page.width = 900;
    page.height = 640;
    page.open = true;
    c.mapPages = {page};
    c.gpsPort = "COM7";
    c.gpsBaud = 4800;
    c.pluginBrowserOpen = true;
    c.fittedModulesOpen = true;
    c.fittedModulesX = 40;
    c.fittedModulesY = 50;
    c.fittedModulesWidth = 700;
    c.fittedModulesHeight = 500;
    c.pluginStoreX = 11;
    c.pluginStoreY = 22;
    c.pluginStoreWidth = 800;
    c.pluginStoreHeight = 600;
    c.updateCheckEnabled = false;
    c.updateChannel = "beta";
    c.messagesSeen = {"msg-0099", "msg-0100"};
    c.testerToken = "0123456789abcdef0123456789abcdef";
    c.testerTokenInvalid = true;
    c.testerUsageCurrent = "{\"u\":1}";
    c.testerUsagePending = {"{\"token\":\"0123456789abcdef0123456789abcdef01234567\",\"u\":2}"};
    c.testerAppToken = "0123456789abcdef0123456789abcdef01234567";
    c.testerAppTokenName = "Test Tester";
    c.diagnosticsEnabled = false;
    c.diagnosticsMinidump = true;
    c.crashUploadRecent = {"deadbeef:1700000000", "cafef00d:1700000100"};
    c.crashUploadWindowStart = 1700000000;
    c.crashUploadWindowCount = 3;
    c.crashUploadBlockedUntil = 1800000000;
    return c;
}

}  // namespace

int main() {
    std::printf("test_config_halves\n");
#ifdef CASCADE_SOURCE_DIR
    const fs::path src = fs::path(CASCADE_SOURCE_DIR) / "src";
#else
    const fs::path src = fs::current_path() / "src";
#endif
    using cascade::core::kConfigFieldCount;
    using cascade::core::kConfigFields;

    // --- 1. THE TABLE IS COMPLETE ---------------------------------------------------------------------------------------------------------
    const std::string headerText = readFile(src / "core" / "config.hpp");
    CHECK(!headerText.empty());
    const std::vector<std::string> members = appConfigMembers(headerText);
    {
        CHECK(members.size() > 100);   // (the scan found the struct: 158 today)
        const std::set<std::string> memberSet(members.begin(), members.end());
        CHECK(memberSet.size() == members.size());   // no name twice in AppConfig
        std::set<std::string> tableSet;
        for (const cascade::core::ConfigFieldInfo& f : kConfigFields) {
            if (!tableSet.insert(f.name).second) { std::printf("  the table names '%s' twice\n", f.name); }
            CHECK(tableSet.count(f.name) == 1);
        }
        CHECK(tableSet.size() == kConfigFieldCount);
        for (const std::string& m : members) {
            if (tableSet.count(m) == 0) {
                std::printf("  AppConfig::%s belongs to neither half: place it in core/config_halves.hpp (Engine, Interface or Shared)\n", m.c_str());
            }
            CHECK(tableSet.count(m) == 1);
        }
        for (const std::string& t : tableSet) {
            if (memberSet.count(t) == 0) { std::printf("  the table names '%s', which AppConfig does not have\n", t.c_str()); }
            CHECK(memberSet.count(t) == 1);
        }
        CHECK(members.size() == kConfigFieldCount);
    }

    // --- 2. THE TABLE IS HELD AGAINST THE WRITERS ------------------------------------------------------------------------------------------
    {
        const std::string engineCpp = readFile(src / "engine" / "engine.cpp");
        const std::string serverCpp = readFile(src / "engine" / "server_host.cpp");
        const std::string windowCpp = readFile(src / "gui" / "app_window.cpp");
        const std::string testerCpp = readFile(src / "gui" / "app_window_tester.cpp");
        const std::string fill = bodyAt(engineCpp, "void Engine::fillConfig(");
        const std::string journal = bodyAt(engineCpp, "void Engine::telemetryJournal(");
        const std::string store = bodyAt(serverCpp, "void ServerHost::store(");
        const std::string build = bodyAt(windowCpp, "void AppWindow::buildInterfaceHalf(");
        const std::string diag = bodyAt(windowCpp, "void AppWindow::diagnosticsJournal(");
        const std::string tester = bodyAt(testerCpp, "void AppWindow::testerUsageJournal(");
        CHECK(!fill.empty() && !journal.empty() && !store.empty() && !build.empty() && !diag.empty() && !tester.empty());
        std::set<std::string> engineWrites = assignedFields(fill, "cfg");
        for (const std::string& f : assignedFields(journal, "cfg")) { engineWrites.insert(f); }
        for (const std::string& f : assignedFields(store, "c")) { engineWrites.insert(f); }
        std::set<std::string> windowWrites = assignedFields(build, "cfg");
        for (const std::string& f : assignedFields(diag, "cfg")) { windowWrites.insert(f); }
        for (const std::string& f : assignedFields(tester, "cfg")) { windowWrites.insert(f); }
        CHECK(engineWrites.size() > 60);
        CHECK(windowWrites.size() > 60);

        std::set<std::string> both;
        for (const std::string& f : engineWrites) {
            if (windowWrites.count(f) != 0) { both.insert(f); }
        }
        CHECK((both == std::set<std::string>{"dbMin", "dbMax"}));

        int engineRows = 0;
        int interfaceRows = 0;
        int sharedRows = 0;
        for (const cascade::core::ConfigFieldInfo& f : kConfigFields) {
            const std::string n = f.name;
            const bool byEngine = engineWrites.count(n) != 0;
            const bool byWindow = windowWrites.count(n) != 0;
            switch (f.owner) {
                case ConfigOwner::Engine: {
                    ++engineRows;
                    // (schemaVersion is the one constant: the loader gates it and no half assigns it.)
                    const bool ok = (byEngine && !byWindow) || n == "schemaVersion";
                    if (!ok) { std::printf("  '%s' is an Engine row but %s\n", f.name, byWindow ? "the window assigns it" : "no engine function assigns it"); }
                    CHECK(ok);
                    break;
                }
                case ConfigOwner::Interface: {
                    ++interfaceRows;
                    const bool ok = byWindow && !byEngine;
                    if (!ok) { std::printf("  '%s' is an Interface row but %s\n", f.name, byEngine ? "the engine assigns it" : "no window function assigns it"); }
                    CHECK(ok);
                    break;
                }
                case ConfigOwner::Shared: {
                    ++sharedRows;
                    const bool ok = byWindow && byEngine;
                    if (!ok) { std::printf("  '%s' is a Shared row but it is not assigned by both writers\n", f.name); }
                    CHECK(ok);
                    break;
                }
            }
        }
        std::printf("  %zu fields: %d engine, %d interface, %d shared\n", kConfigFieldCount, engineRows, interfaceRows, sharedRows);
        CHECK(sharedRows == 2);
        CHECK(engineRows + interfaceRows + sharedRows == static_cast<int>(kConfigFieldCount));
    }

    // --- 3. THE SELF-MOVING FIELDS ---------------------------------------------------------------------------------------------------------
    {
        std::set<std::string> moving;
        for (const cascade::core::ConfigFieldInfo& f : kConfigFields) {
            if (f.moves) { moving.insert(f.name); }
        }
        const std::set<std::string> expected = {"telemetryPending",       "telemetryCleanExit",     "telemetrySessionStarted", "testerUsageCurrent",
                                                "testerUsagePending",     "testerTokenInvalid",     "crashUploadRecent",       "crashUploadWindowStart",
                                                "crashUploadWindowCount", "crashUploadBlockedUntil"};
        CHECK(moving == expected);
        // ...and none of them takes part in the window's own compare (configsEqual: "none of the three would ever settle if compared here").
        const std::string windowCpp = readFile(src / "gui" / "app_window.cpp");
        const std::string equal = bodyAt(windowCpp, "bool configsEqual(");
        CHECK(!equal.empty());
        for (const std::string& m : moving) {
            const bool compared = equal.find("a." + m + " ==") != std::string::npos;
            if (compared) { std::printf("  '%s' moves by itself and configsEqual compares it\n", m.c_str()); }
            CHECK(!compared);
        }
    }

    // --- 4. EACH FIELD, BY ITSELF ----------------------------------------------------------------------------------------------------------
    {
        int visited = 0;
        cascade::core::forEachConfigField([&](const char* name, ConfigOwner owner, bool moves, auto p) {
            ++visited;
            const AppConfig base;
            AppConfig changed = base;
            perturb(changed.*p);
            // the perturbation is real, and is the only difference
            const std::set<std::string> diff = differing(base, changed);
            if (diff != std::set<std::string>{name}) { std::printf("  perturbing '%s' changed %zu field(s)\n", name, diff.size()); }
            CHECK((diff == std::set<std::string>{name}));

            AppConfig toEngine;
            cascade::core::copyEngineHalf(changed, toEngine);
            const bool engineCopied = toEngine.*p == changed.*p;
            if (engineCopied != (owner != ConfigOwner::Interface)) { std::printf("  copyEngineHalf and '%s'\n", name); }
            CHECK(engineCopied == (owner != ConfigOwner::Interface));

            AppConfig toInterface;
            cascade::core::copyInterfaceHalf(changed, toInterface);
            const bool interfaceCopied = toInterface.*p == changed.*p;
            if (interfaceCopied != (owner != ConfigOwner::Engine)) { std::printf("  copyInterfaceHalf and '%s'\n", name); }
            CHECK(interfaceCopied == (owner != ConfigOwner::Engine));

            AppConfig toStrict;
            cascade::core::copyInterfaceHalf(changed, toStrict, /*includeShared=*/false);
            const bool strictCopied = toStrict.*p == changed.*p;
            if (strictCopied != (owner == ConfigOwner::Interface)) { std::printf("  copyInterfaceHalf(no shared) and '%s'\n", name); }
            CHECK(strictCopied == (owner == ConfigOwner::Interface));

            // only the one field moves in each copy
            CHECK(differing(base, toEngine).size() == (engineCopied ? 1u : 0u));
            CHECK(differing(base, toInterface).size() == (interfaceCopied ? 1u : 0u));

            // what an interface asks before it sends
            const bool sees = !cascade::core::interfaceHalvesEqual(base, changed);
            if (sees != (owner != ConfigOwner::Engine && !moves)) { std::printf("  interfaceHalvesEqual and '%s'\n", name); }
            CHECK(sees == (owner != ConfigOwner::Engine && !moves));

            // what a keeper compares
            AppConfig cleared = changed;
            cascade::core::clearBookkeeping(cleared);
            const bool blanked = cleared.*p == base.*p;
            if (blanked != moves) { std::printf("  clearBookkeeping and '%s'\n", name); }
            CHECK(blanked == moves);
        });
        CHECK(visited == static_cast<int>(kConfigFieldCount));
    }

    // --- 5. THE DOCUMENT --------------------------------------------------------------------------------------------------------------------
    {
        // The default half: every key of the half the file writes - the legacy map rectangle, set only for a config that has one, apart.
        std::set<std::string> halfKeys;
        for (const cascade::core::ConfigFieldInfo& f : kConfigFields) {
            if (f.owner != ConfigOwner::Engine) { halfKeys.insert(f.name); }
        }
        const std::set<std::string> legacy = {"mapWindowWidth", "mapWindowHeight", "mapWindowX", "mapWindowY"};
        {
            const std::string doc = cascade::core::serializeInterfaceHalf(AppConfig{});
            const json j = json::parse(doc, nullptr, false);
            CHECK(j.is_object());
            std::set<std::string> keys;
            for (auto it = j.begin(); it != j.end(); ++it) { keys.insert(it.key()); }
            std::set<std::string> want;
            for (const std::string& k : halfKeys) {
                if (legacy.count(k) == 0) { want.insert(k); }
            }
            if (keys != want) {
                for (const std::string& k : keys) { if (want.count(k) == 0) { std::printf("  the document carries '%s', which is not the half's\n", k.c_str()); } }
                for (const std::string& k : want) { if (keys.count(k) == 0) { std::printf("  the document lacks '%s', which is the half's (is its JSON key its member name?)\n", k.c_str()); } }
            }
            CHECK(keys == want);
        }

        // The populated half: no field of it left at its default, then round-tripped.
        const AppConfig in = populated();
        {
            const AppConfig defaults;
            int atDefault = 0;
            cascade::core::forEachConfigField([&](const char* name, ConfigOwner owner, bool, auto p) {
                if (owner == ConfigOwner::Engine) { return; }
                if (in.*p == defaults.*p) {
                    ++atDefault;
                    std::printf("  populated() leaves '%s' at its default - a value lost on the way would go unseen\n", name);
                }
            });
            CHECK(atDefault == 0);
        }
        // Engine fields set on the input must not cross.
        AppConfig withEngine = in;
        withEngine.webPasswordRecord = "pbkdf2$secret";
        withEngine.sourceKind = "rtlsdr";
        withEngine.telemetryInstallId = "0123456789abcdef";
        withEngine.volume = 0.9f;
        const std::string doc = cascade::core::serializeInterfaceHalf(withEngine);
        CHECK(doc.find("pbkdf2$secret") == std::string::npos);
        CHECK(doc.find("rtlsdr") == std::string::npos);
        CHECK(doc.find("0123456789abcdef\"") == std::string::npos || doc.find("telemetryInstallId") == std::string::npos);
        CHECK(doc.find("webPasswordRecord") == std::string::npos);
        CHECK(doc.size() < 1000u * 1000u);   // (the op's text is capped at 1 MiB, FOXAPI_MAX_LONG_TEXT)

        AppConfig parsed;
        std::string error;
        CHECK(cascade::core::parseInterfaceHalf(doc, parsed, error));
        CHECK(error.empty());
        AppConfig out;
        cascade::core::copyInterfaceHalf(parsed, out);
        // every Interface and Shared field comes out equal, the self-moving ones too
        cascade::core::forEachConfigField([&](const char* name, ConfigOwner owner, bool, auto p) {
            if (owner == ConfigOwner::Engine) { return; }
            const bool same = out.*p == in.*p;
            if (!same) { std::printf("  '%s' did not survive the document\n", name); }
            CHECK(same);
        });
        // ...and nothing of the engine's came back from it
        CHECK(out.webPasswordRecord.empty());
        CHECK(out.sourceKind == AppConfig{}.sourceKind);

        // A second pass through the document changes nothing (the reader's repairs are idempotent).
        const std::string again = cascade::core::serializeInterfaceHalf(out);
        CHECK(again == cascade::core::serializeInterfaceHalf(in));

        // THE LEGACY MAP RECTANGLE survives a document that has map pages (ConfigStore::serialize drops it once a page exists).
        CHECK(out.mapWindowWidth == 900);
        CHECK(out.mapWindowHeight == 600);
        CHECK(out.mapWindowX == 30);
        CHECK(out.mapWindowY == 40);
        CHECK(ConfigStore::serialize(in).find("mapWindowWidth") == std::string::npos);   // (so the document is what carried it)

        // The start-up state of the half is the caller's to apply: the document is the half AS IT IS, open flags and all.
        CHECK(out.scopeMode && out.demodScopeOpen && out.pluginBrowserOpen && out.fittedModulesOpen);
        CHECK(!out.mapPages.empty() && out.mapPages.front().open);

        // A document that is not JSON is refused, and a JSON document that is not an object too.
        AppConfig junk;
        std::string why;
        CHECK(!cascade::core::parseInterfaceHalf("this is not json", junk, why));
        CHECK(!why.empty());
        CHECK(!cascade::core::parseInterfaceHalf("[1, 2, 3]", junk, why));
        // An empty object is a valid half: every field at its default.
        CHECK(cascade::core::parseInterfaceHalf("{}", junk, why));
        CHECK(junk.railBank == AppConfig{}.railBank);
    }

    return testSummary("test_config_halves");
}
