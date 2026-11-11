// Tests for the setup models of lane sysmodels: core/sys_telemetry_switch.hpp, sys_reset.hpp, sys_profiles.hpp, sys_first_run.hpp.
// Scratch folders only (PID in the name); a folder that must be "named foxsdr" is made inside the scratch tree. The real settings are never named.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define TEST_GETPID _getpid
#else
#include <unistd.h>
#define TEST_GETPID getpid
#endif

#include <nlohmann/json.hpp>

#include "core/config.hpp"
#include "core/demod_names.hpp"
#include "core/freq_manager.hpp"
#include "core/sys_bundle.hpp"
#include "core/sys_first_run.hpp"
#include "core/sys_profiles.hpp"
#include "core/sys_reset.hpp"
#include "core/sys_telemetry_switch.hpp"
#include "core/telemetry.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using nlohmann::json;
using namespace cascade::core::sys;

namespace {

fs::path g_base;

std::string slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()); }
void spit(const fs::path& p, const std::string& s) { fs::create_directories(p.parent_path()); std::ofstream f(p, std::ios::binary | std::ios::trunc); f << s; }

std::set<std::string> listing(const fs::path& dir) {
    std::set<std::string> out;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        out.insert(fs::relative(it->path(), dir).generic_string());
    }
    return out;
}

struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed) {}
    std::uint32_t next() { s = s * 6364136223846793005ull + 1442695040888963407ull; return static_cast<std::uint32_t>(s >> 33); }
};

// ============================================================================ telemetry switch
const char* kId1 = "0123456789abcdef0123456789abcdef";
const char* kId2 = "fedcba9876543210fedcba9876543210";

void testTelemetrySwitch() {
    int made = 0;
    auto maker = [&] { ++made; return std::string(made == 1 ? kId1 : kId2); };
    TelemetryState off;
    auto o = setTelemetry(off, true, maker);
    CHECK(o.changed && o.next.enabled && o.next.installId == kId1 && made == 1);
    // ON while already on changes nothing and does not ask for a new id.
    auto again = setTelemetry(o.next, true, maker);
    CHECK(!again.changed && again.next == o.next && made == 1);
    // OFF deletes the id and the pending record.
    TelemetryState on = o.next;
    on.pending = "{\"launches\":3}";
    auto offOut = setTelemetry(on, false, maker);
    CHECK(offOut.changed && !offOut.next.enabled && offOut.next.installId.empty() && offOut.next.pending.empty());
    // A later opt-in is NOT the old id.
    auto reon = setTelemetry(offOut.next, true, maker);
    CHECK(reon.next.installId == kId2 && reon.next.installId != kId1);
    // OFF while off is a no-op; OFF clears leftovers even when the flag was already off.
    CHECK(!setTelemetry(off, false, maker).changed);
    TelemetryState leftovers;
    leftovers.installId = kId1;
    leftovers.pending = "x";
    const auto cleaned = setTelemetry(leftovers, false, maker);
    CHECK(cleaned.changed && cleaned.next.installId.empty() && cleaned.next.pending.empty());
    // The id maker failing (empty) or lying (wrong shape) leaves reporting off and ids empty.
    for (const char* bad : {"", "abc", "0123456789ABCDEF0123456789ABCDEF", "0123456789abcdef0123456789abcdeg", "0123456789abcdef0123456789abcdef0"}) {
        auto r = setTelemetry(off, true, [&] { return std::string(bad); });
        CHECK(!r.next.enabled && r.next.installId.empty() && !r.changed);
    }
    CHECK(!setTelemetry(off, true, nullptr).next.enabled);
    // Enabled with a malformed id is not "already on": it is repaired through a fresh mint.
    TelemetryState broken;
    broken.enabled = true;
    broken.installId = "not-an-id";
    CHECK(setTelemetry(broken, true, [] { return std::string("0123456789abcdef0123456789abcdef"); }).next.installId == kId1);
    CHECK(wellFormedInstallId(kId1) && !wellFormedInstallId("") && !wellFormedInstallId(std::string(32, 'g')));
}

// THE RULE, over every combination and then over random sequences: with reporting off nothing leaves; after off no identifier remains.
void testOffSendsNothing() {
    const std::vector<std::string> wanted = {"heartbeat", "previous session record", "crash note"};
    for (int enabled = 0; enabled < 2; ++enabled) {
        for (const char* id : {"", "0123456789abcdef0123456789abcdef", "garbage", "0123456789ABCDEF0123456789ABCDEF"}) {
            for (const char* pending : {"", "{\"x\":1}"}) {
                TelemetryState s;
                s.enabled = enabled != 0;
                s.installId = id;
                s.pending = pending;
                const bool expect = s.enabled && std::string(id) == kId1;
                CHECK(mayTransmit(s) == expect);
                CHECK(transmissions(s, wanted) == (expect ? wanted : std::vector<std::string>{}));
                // The same holds after the state has been through a load.
                const TelemetryState n = loadNormalise(s);
                CHECK(transmissions(n, wanted).empty() || (n.enabled && wellFormedInstallId(n.installId)));
            }
        }
    }
    Rng r(2026);
    TelemetryState s;
    int idCalls = 0;
    auto maker = [&] { ++idCalls; char buf[40]; std::snprintf(buf, sizeof buf, "%032x", static_cast<unsigned>(idCalls) * 2654435761u); return std::string(buf); };
    for (int i = 0; i < 20000; ++i) {
        switch (r.next() % 6) {
            case 0: s = setTelemetry(s, true, maker).next; break;
            case 1: s = setTelemetry(s, false, maker).next; CHECK(s.installId.empty() && s.pending.empty() && !s.enabled); break;
            case 2: s = loadNormalise(s); break;
            case 3: s.pending = (r.next() & 1) ? "{}" : ""; break;
            case 4: s.installId = (r.next() & 1) ? "junk" : s.installId; break;
            default: s.enabled = (r.next() & 1) != 0; break;   // a hand edit of the flag
        }
        if (!s.enabled) { CHECK(transmissions(s, wanted).empty()); }
        if (mayTransmit(s)) { CHECK(s.enabled && wellFormedInstallId(s.installId)); }
    }
}

// The model against the REAL loader, over every combination of the two fields that decide it.
void testModelMatchesRealLoad() {
    const fs::path dir = g_base / "telemetry";
    fs::create_directories(dir);
    for (int enabled = 0; enabled < 2; ++enabled) {
        for (const char* id : {"", "0123456789abcdef0123456789abcdef", "user@example.com", "0123456789ABCDEF0123456789ABCDEF", "short"}) {
            json j;
            j["telemetryEnabled"] = enabled != 0;
            j["telemetryInstallId"] = id;
            j["telemetryPending"] = "{\"launches\":1}";
            const fs::path p = dir / "config.json";
            spit(p, j.dump());
            cascade::core::AppConfig real;
            std::string err;
            CHECK(cascade::core::ConfigStore::load(p.string(), real, err));
            TelemetryState m;
            m.enabled = enabled != 0;
            m.installId = id;
            m.pending = "{\"launches\":1}";
            const TelemetryState n = loadNormalise(m);
            CHECK(n.enabled == real.telemetryEnabled);
            CHECK(n.installId == real.telemetryInstallId);
            CHECK(n.pending.empty() == real.telemetryPending.empty());
            CHECK(cascade::core::validInstallId(id) == wellFormedInstallId(id));
        }
    }
}

// ============================================================================ factory reset
struct ResetTree {
    fs::path settings, data;
};

ResetTree makeResetTree(const char* tag) {
    ResetTree t;
    const fs::path root = g_base / tag;
    fs::remove_all(root);
    t.settings = root / "AppData" / "foxsdr";
    t.data = root / "Local" / "foxsdr";
    spit(t.settings / "config.json", "{\"webPort\":1}");
    spit(t.settings / "config.json.20261101-120000.bak", "{}");
    spit(t.settings / "bookmarks.json", "{\"bookmarks\":[]}");
    spit(t.settings / "markers.json", "{\"markers\":[]}");
    spit(t.settings / "notes.txt", "mine");
    spit(t.settings / "config.json.old.bak", "mine too");
    spit(t.settings / "random.dll", "x");
    spit(t.settings / "sub" / "inner.json", "{}");
    spit(t.data / "plugins" / "a.dll", "AAAA");
    spit(t.data / "plugins" / "sub" / "b.dll", "BB");
    spit(t.data / "logs" / "x.log", "log");
    return t;
}

void testResetNames() {
    for (const char* ok : {"config.json", "bookmarks.json", "markers.json", "config.json.20261101-120000.bak", "markers.json.20000101-000000.bak"}) { CHECK(settingsFileNameIsOurs(ok)); }
    for (const char* bad : {"", "config.json.bak", "config.json.old.bak", "config.json.20261101-120000", "config.json.2026110-1200000.bak", "config.json.20261101_120000.bak", "Config.json", "config.json ",
                            "bookmarks.json.20261101-120000.bak.bak", "plugins", "foxsdr.portable", "../config.json", "x/config.json", "config.json.20261101-12000a.bak", "first-run-done"}) {
        CHECK(!settingsFileNameIsOurs(bad));
    }
    Rng r(5);
    const std::string seed = "config.json.20261101-120000.bak";
    for (int i = 0; i < 5000; ++i) {
        std::string m = seed;
        m[r.next() % m.size()] = static_cast<char>(r.next());
        if (settingsFileNameIsOurs(m)) { CHECK(m == seed || (m.size() == seed.size() && m.compare(0, 12, "config.json.") == 0 && m.compare(m.size() - 4, 4, ".bak") == 0)); }
    }
}

void testResetPlan() {
    const auto t = makeResetTree("reset-plan");
    const auto before = listing(t.settings.parent_path().parent_path());
    const ResetPlan p = planReset(t.settings, t.data, ResetOptions{});
    CHECK(p.error.empty() && p.items.size() == 4);
    std::vector<std::string> names;
    for (const auto& i : p.items) { names.push_back(i.relative); CHECK(!i.isFolder && i.kind == ResetKind::SettingsFile); }
    CHECK(names == (std::vector<std::string>{"bookmarks.json", "config.json", "config.json.20261101-120000.bak", "markers.json"}));
    CHECK(p.leftAlone == 4);   // notes.txt, config.json.old.bak, random.dll, sub
    CHECK(p.confirmToken.rfind("RESET-", 0) == 0 && p.confirmToken.size() == 14);
    CHECK(listing(t.settings.parent_path().parent_path()) == before);   // a dry run writes nothing
    CHECK(planReset(t.settings, t.data, ResetOptions{}).confirmToken == p.confirmToken);   // stable

    ResetOptions all;
    all.includePlugins = true;
    all.includeLogs = true;
    const ResetPlan q = planReset(t.settings, t.data, all);
    CHECK(q.error.empty() && q.items.size() == 6 && q.confirmToken != p.confirmToken);
    for (const auto& i : q.items) {
        if (i.kind == ResetKind::PluginsFolder) { CHECK(i.isFolder && i.files == 2 && i.bytes == 6); }
        if (i.kind == ResetKind::LogsFolder) { CHECK(i.isFolder && i.files == 1 && i.bytes == 3); }
    }
    // Guards: a folder not named foxsdr is refused outright, whatever it holds.
    const fs::path docs = g_base / "reset-plan" / "Documents";
    spit(docs / "config.json", "{}");
    CHECK(!planReset(docs, t.data, ResetOptions{}).error.empty() && planReset(docs, t.data, ResetOptions{}).items.empty());
    CHECK(!planReset("", t.data, ResetOptions{}).error.empty());
    CHECK(!planReset(fs::path("C:/"), t.data, ResetOptions{}).error.empty());
    CHECK(!planReset(t.settings, docs, all).error.empty());                     // plugins/logs asked for in a wrong data folder
    CHECK(planReset(t.settings, docs, ResetOptions{}).error.empty());           // ... but a data folder nobody asked about is never looked at
    CHECK(planReset(g_base / "reset-plan" / "FOXSDR-upper-test" / "foxsdr-none", t.data, ResetOptions{}).error.empty() == false);
    // A settings folder that does not exist yet: nothing to reset, no error.
    const ResetPlan none = planReset(g_base / "reset-plan" / "fresh" / "FoxSDR", t.data, ResetOptions{});
    CHECK(none.error.empty() && none.items.empty());
    // A path that is a file is refused.
    spit(g_base / "reset-plan" / "afile" / "foxsdr", "x");
    CHECK(!planReset(g_base / "reset-plan" / "afile" / "foxsdr", t.data, ResetOptions{}).error.empty());
}

void testResetExecuteAndUndo() {
    auto t = makeResetTree("reset-exec");
    const ResetPlan p = planReset(t.settings, t.data, ResetOptions{});
    std::string err;
    // Refusals change nothing.
    CHECK(!executeReset(t.settings, t.data, ResetOptions{}, "", "20261111-230000", err));
    CHECK(!executeReset(t.settings, t.data, ResetOptions{}, "RESET-00000000", "20261111-230000", err));
    CHECK(!executeReset(t.settings, t.data, ResetOptions{}, p.confirmToken, "bad stamp", err));
    CHECK(fs::exists(t.settings / "config.json"));
    spit(t.settings / "config.json", "{\"webPort\":2222}");   // the folder changed after the plan (size differs)
    CHECK(!executeReset(t.settings, t.data, ResetOptions{}, p.confirmToken, "20261111-230000", err));
    CHECK(slurp(t.settings / "config.json") == "{\"webPort\":2222}");
    // The right token.
    const ResetPlan p2 = planReset(t.settings, t.data, ResetOptions{});
    CHECK(executeReset(t.settings, t.data, ResetOptions{}, p2.confirmToken, "20261111-230000", err) && err.empty());
    CHECK(!fs::exists(t.settings / "config.json") && !fs::exists(t.settings / "bookmarks.json") && !fs::exists(t.settings / "markers.json") && !fs::exists(t.settings / "config.json.20261101-120000.bak"));
    CHECK(slurp(t.settings / "reset-20261111-230000" / "config.json") == "{\"webPort\":2222}");   // moved, not deleted
    // Everything that is not ours is where it was, and so are plugins and logs.
    CHECK(fs::exists(t.settings / "notes.txt") && fs::exists(t.settings / "config.json.old.bak") && fs::exists(t.settings / "random.dll") && fs::exists(t.settings / "sub" / "inner.json"));
    CHECK(fs::exists(t.data / "plugins" / "a.dll") && fs::exists(t.data / "logs" / "x.log"));
    // Planning again finds nothing to reset (the aside folder is not ours); executing that is a no-op.
    const ResetPlan p3 = planReset(t.settings, t.data, ResetOptions{});
    CHECK(p3.items.empty());
    CHECK(executeReset(t.settings, t.data, ResetOptions{}, p3.confirmToken, "20261111-230001", err));
    // Undo: back in place; a second undo has nothing; an occupied place refuses without moving anything.
    CHECK(undoReset(t.settings, t.data, "20261111-230000", err) && err.empty());
    CHECK(slurp(t.settings / "config.json") == "{\"webPort\":2222}" && fs::exists(t.settings / "markers.json") && !fs::exists(t.settings / "reset-20261111-230000"));
    CHECK(!undoReset(t.settings, t.data, "20261111-230000", err) && !err.empty());
    const ResetPlan p4 = planReset(t.settings, t.data, ResetOptions{});
    CHECK(executeReset(t.settings, t.data, ResetOptions{}, p4.confirmToken, "20261111-230100", err));
    spit(t.settings / "config.json", "{\"new\":true}");   // a fresh config appeared after the reset
    CHECK(!undoReset(t.settings, t.data, "20261111-230100", err) && !err.empty());
    CHECK(slurp(t.settings / "config.json") == "{\"new\":true}" && !fs::exists(t.settings / "markers.json") && fs::exists(t.settings / "reset-20261111-230100" / "markers.json"));
    CHECK(!undoReset(t.settings, t.data, "x y", err));
    // A reset folder with that stamp that already holds files is not mixed into.
    auto t2 = makeResetTree("reset-exec2");
    spit(t2.settings / "reset-20261111-230000" / "stray.txt", "x");
    const ResetPlan q = planReset(t2.settings, t2.data, ResetOptions{});
    CHECK(!executeReset(t2.settings, t2.data, ResetOptions{}, q.confirmToken, "20261111-230000", err));
    CHECK(fs::exists(t2.settings / "config.json"));
    // Plugins and logs, only on request, and they come back too.
    auto t3 = makeResetTree("reset-exec3");
    ResetOptions all;
    all.includePlugins = true;
    all.includeLogs = true;
    const ResetPlan r = planReset(t3.settings, t3.data, all);
    CHECK(executeReset(t3.settings, t3.data, all, r.confirmToken, "20261111-230200", err));
    CHECK(!fs::exists(t3.data / "plugins") && !fs::exists(t3.data / "logs") && fs::exists(t3.data / "reset-20261111-230200" / "plugins" / "sub" / "b.dll"));
    CHECK(undoReset(t3.settings, t3.data, "20261111-230200", err));
    CHECK(fs::exists(t3.data / "plugins" / "a.dll") && fs::exists(t3.data / "logs" / "x.log") && fs::exists(t3.settings / "config.json"));
}

void testResetRollback() {
#ifdef _WIN32
    auto t = makeResetTree("reset-rollback");
    const ResetPlan p = planReset(t.settings, t.data, ResetOptions{});
    std::string err;
    bool ok = true;
    {
        std::ifstream hold(t.settings / "markers.json", std::ios::binary);   // an open handle without delete sharing: the LAST item's move fails
        CHECK(hold.is_open());
        ok = executeReset(t.settings, t.data, ResetOptions{}, p.confirmToken, "20261111-231000", err);
        CHECK(!ok && !err.empty());
    }
    // The items that had already been moved are back; nothing is left aside.
    CHECK(fs::exists(t.settings / "config.json") && fs::exists(t.settings / "bookmarks.json") && fs::exists(t.settings / "config.json.20261101-120000.bak") && fs::exists(t.settings / "markers.json"));
    CHECK(!fs::exists(t.settings / "reset-20261111-231000"));
#else
    SKIP_LINUX("a rename of an open file succeeds on POSIX");
#endif
}

// ============================================================================ profiles
void testProfileNames() {
    for (const char* ok : {"Home", "Portable stick", "RTL-SDR 1", "a", "x_y-z 9", "Field day 2026"}) { CHECK(validProfileName(ok)); }
    for (const char* bad : {"", " lead", "trail ", "-dash", "_under", "a/b", "a\\b", "..", "a.b", "CON", "con", "COM3", "nul", "a:b", "caf\xC3\xA9", "tab\tx"}) {
        const bool v = validProfileName(bad);
        if (v) { std::printf("accepted bad profile name: %s\n", bad); }
        CHECK(!v);
    }
    CHECK(validProfileName(std::string(40, 'a')) && !validProfileName(std::string(41, 'a')));
}

std::vector<BundleFile> profileFiles(const char* channel) {
    json cfg;
    cfg["updateChannel"] = channel;
    cfg["webPort"] = 8080;
    cfg["webEnabled"] = true;
    cfg["webPasswordRecord"] = "SECRET-RECORD";
    cfg["telemetryInstallId"] = "0123456789abcdef0123456789abcdef";
    return {{"config.json", cfg.dump()}, {"bookmarks.json", "{\"schemaVersion\":1,\"bookmarks\":[]}"}};
}

void testProfileStore() {
    const fs::path dir = g_base / "profiles";
    fs::remove_all(dir);
    std::string err;
    CHECK(listProfiles(dir).empty());
    CHECK(saveProfile(dir, "Home", profileFiles("stable"), false, ExportOptions{}, err));
    const std::string disk = slurp(dir / "Home.foxsdr-profile");
    CHECK(!disk.empty() && disk.find("SECRET-RECORD") == std::string::npos && disk.find("0123456789abcdef0123456789abcdef") == std::string::npos);
    CHECK(!saveProfile(dir, "Home", profileFiles("beta"), false, ExportOptions{}, err) && !err.empty());   // no silent overwrite
    CHECK(saveProfile(dir, "Home", profileFiles("beta"), true, ExportOptions{}, err));
    CHECK(!saveProfile(dir, "bad/name", profileFiles("x"), false, ExportOptions{}, err));
    CHECK(!saveProfile(dir, "Empty", {}, false, ExportOptions{}, err) && !fs::exists(dir / "Empty.foxsdr-profile"));
    CHECK(!saveProfile(dir, "BadFile", {{"evil.dll", "{}"}}, false, ExportOptions{}, err));
    CHECK(saveProfile(dir, "Away", profileFiles("stable"), false, ExportOptions{}, err));
    spit(dir / "notes.txt", "not a profile");
    spit(dir / "bad name!.foxsdr-profile", "x");
    fs::create_directory(dir / "Dir.foxsdr-profile");
    const auto l = listProfiles(dir);
    CHECK(l.size() == 2 && l[0].name == "Away" && l[1].name == "Home" && l[0].bytes > 0 && l[0].sha256.size() == 64);
    CHECK(!deleteProfile(dir, "Nope", err) && !deleteProfile(dir, "../x", err));
    CHECK(deleteProfile(dir, "Away", err) && listProfiles(dir).size() == 1);
    // The limit.
    const fs::path many = g_base / "profiles-many";
    fs::remove_all(many);
    for (std::size_t i = 0; i < kMaxProfiles; ++i) { CHECK(saveProfile(many, "P" + std::to_string(i), profileFiles("stable"), false, ExportOptions{}, err)); }
    CHECK(!saveProfile(many, "OneTooMany", profileFiles("stable"), false, ExportOptions{}, err));
    CHECK(saveProfile(many, "P0", profileFiles("beta"), true, ExportOptions{}, err));   // overwriting is not adding
}

void testProfileSwitch() {
    const fs::path profiles = g_base / "switch-profiles";
    const fs::path settings = g_base / "switch-settings";
    fs::remove_all(profiles);
    fs::remove_all(settings);
    std::string err;
    CHECK(saveProfile(profiles, "Portable", profileFiles("beta"), false, ExportOptions{}, err));
    // This machine's own config: its password, anonymous id, network exposure, and a setting the profile will change.
    json mine;
    mine["updateChannel"] = "stable";
    mine["webEnabled"] = false;
    mine["webPort"] = 9000;
    mine["webPasswordRecord"] = "MY-OWN-RECORD";
    mine["telemetryInstallId"] = "fedcba9876543210fedcba9876543210";
    mine["telemetryEnabled"] = true;
    spit(settings / "config.json", mine.dump());
    const SwitchPlan sp = planSwitch(profiles, "Portable", settings);
    CHECK(sp.error.empty());
    CHECK(sp.plan.items.size() == 2);
    json staged;
    for (const auto& f : sp.staged.files) { if (f.name == "config.json") { staged = json::parse(f.bytes); } }
    CHECK(staged["updateChannel"] == "beta");                       // the profile decides ways of working
    CHECK(staged["webPort"] == 9000 && staged["webEnabled"] == false);   // this machine decides what it exposes
    CHECK(staged["webPasswordRecord"] == "MY-OWN-RECORD");          // and keeps its own secret ...
    CHECK(staged["telemetryInstallId"] == "fedcba9876543210fedcba9876543210");   // ... and identity
    CHECK(staged["telemetryEnabled"] == true);                      // ... and its own consent about usage reporting
    CHECK(!sp.carriedKeys.empty() && std::find(sp.carriedKeys.begin(), sp.carriedKeys.end(), "webPasswordRecord") != sp.carriedKeys.end());
    // The plan is a dry run until applied; the token gate and the kept copy are the import path's.
    CHECK(json::parse(slurp(settings / "config.json"))["updateChannel"] == "stable");
    CHECK(!applySwitch(sp, settings, "IMPORT-00000000", "20261111-232000", err));
    CHECK(applySwitch(sp, settings, sp.plan.confirmToken, "20261111-232000", err));
    const json now = json::parse(slurp(settings / "config.json"));
    CHECK(now["updateChannel"] == "beta" && now["webPasswordRecord"] == "MY-OWN-RECORD" && now["webPort"] == 9000);
    CHECK(json::parse(slurp(settings / "config.json.before-import-20261111-232000"))["updateChannel"] == "stable");
    CHECK(fs::exists(settings / "bookmarks.json"));
    // A machine with no config yet: nothing carried, the profile's keys for secrets are still dropped.
    const fs::path fresh = g_base / "switch-fresh";
    fs::remove_all(fresh);
    const SwitchPlan sf = planSwitch(profiles, "Portable", fresh);
    CHECK(sf.error.empty() && sf.carriedKeys.empty());
    for (const auto& f : sf.staged.files) { if (f.name == "config.json") { CHECK(f.bytes.find("SECRET-RECORD") == std::string::npos && !json::parse(f.bytes).contains("webEnabled")); } }
    // Errors.
    CHECK(!planSwitch(profiles, "Missing", settings).error.empty());
    CHECK(!planSwitch(profiles, "../Portable", settings).error.empty());
    spit(profiles / "Garbage.foxsdr-profile", "this is not a bundle");
    CHECK(!planSwitch(profiles, "Garbage", settings).error.empty());
    // A profile with only bookmarks leaves config.json alone.
    CHECK(saveProfile(profiles, "Marks", {{"bookmarks.json", "{\"bookmarks\":[]}"}}, false, ExportOptions{}, err));
    const SwitchPlan sm = planSwitch(profiles, "Marks", settings);
    CHECK(sm.error.empty() && sm.plan.items.size() == 1 && sm.plan.items[0].name == "bookmarks.json");
}

void testRadioMap() {
    for (const char* ok : {"rtlsdr", "airspyhf", "hackrf", "airspy-hf+", "rx888", "rtl_tcp", "Pluto.1"}) { CHECK(RadioProfileMap::validRadioKey(ok)); }
    for (const char* bad : {"", "00000001", "serial 123", "ABCDEF123456ABCDEF", "a/b", "rtl sdr", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "x1234567y"}) {
        const bool v = RadioProfileMap::validRadioKey(bad);
        if (v) { std::printf("accepted bad radio key: %s\n", bad); }
        CHECK(!v);
    }
    RadioProfileMap m;
    CHECK(m.set("rtlsdr", "Home") && m.set("airspyhf", "Portable"));
    CHECK(m.profileFor("rtlsdr") == "Home" && m.profileFor("hackrf").empty());
    CHECK(!m.set("00000001", "Home") && !m.set("rtlsdr", "bad/name") && m.size() == 2);
    CHECK(m.set("rtlsdr", "Portable") && m.profileFor("rtlsdr") == "Portable" && m.size() == 2);
    // The bound.
    RadioProfileMap full;
    for (std::size_t i = 0; i < RadioProfileMap::kMaxEntries; ++i) { CHECK(full.set("radio-" + std::string(1, static_cast<char>('a' + i % 26)) + std::string(1, static_cast<char>('a' + i / 26)), "Home")); }
    CHECK(full.size() == RadioProfileMap::kMaxEntries);
    CHECK(!full.set("one-more", "Home") && full.set("radio-aa", "Away"));
    // Prune against what exists.
    std::vector<ProfileInfo> existing = {ProfileInfo{"Portable", 1, ""}};
    CHECK(m.prune(existing) == 0);
    existing.clear();
    CHECK(m.prune(existing) == 2 && m.size() == 0);
    // JSON round trip, and the file text is just model names and profile names.
    RadioProfileMap rt;
    rt.set("rtlsdr", "Home");
    rt.set("hackrf", "Field day");
    const std::string text = rt.toJson();
    const RadioProfileMap back = RadioProfileMap::fromJson(text);
    CHECK(back.size() == 2 && back.profileFor("hackrf") == "Field day");
    // Hostile input: wrong shapes, bad entries dropped, the good ones kept, bounds held, deep nesting refused, and a mutation fuzz.
    CHECK(RadioProfileMap::fromJson("").size() == 0 && RadioProfileMap::fromJson("[1]").size() == 0 && RadioProfileMap::fromJson("{\"radios\":5}").size() == 0);
    CHECK(RadioProfileMap::fromJson("{\"radios\":{\"rtlsdr\":\"Home\",\"00000001\":\"Home\",\"hackrf\":7,\"airspy\":\"../x\"}}").size() == 1);
    CHECK(RadioProfileMap::fromJson(std::string(40, '[') + std::string(40, ']')).size() == 0);
    CHECK(RadioProfileMap::fromJson(std::string(70000, ' ')).size() == 0);
    {
        json big;
        for (int i = 0; i < 200; ++i) { big["radios"]["r" + std::to_string(i % 10) + std::string(1, static_cast<char>('a' + i % 26)) + std::to_string(i / 26)] = "Home"; }
        CHECK(RadioProfileMap::fromJson(big.dump()).size() <= RadioProfileMap::kMaxEntries);
    }
    Rng r(77);
    for (int i = 0; i < 3000; ++i) {
        std::string mtext = text;
        mtext[r.next() % mtext.size()] = static_cast<char>(r.next());
        if (r.next() % 3 == 0) { mtext.resize(1 + r.next() % mtext.size()); }
        const RadioProfileMap fm = RadioProfileMap::fromJson(mtext);
        CHECK(fm.size() <= RadioProfileMap::kMaxEntries);
        const RadioProfileMap again = RadioProfileMap::fromJson(fm.toJson());
        CHECK(again.size() == fm.size());
    }
}

void testSyncPlan() {
    auto e = [](const char* n, const char* sha) { return SyncEntry{n, sha, 0}; };
    auto action = [](const std::vector<SyncItem>& v, const char* name) { for (const auto& i : v) { if (i.name == name) { return i.action; } } return SyncAction::None; };
    // Never synced: only-one-side copies across, both-sides-different is a conflict, both-sides-same is nothing.
    {
        const auto p = planSync({e("OnlyLocal", "a"), e("Same", "s"), e("Diff", "x")}, {e("OnlyShared", "b"), e("Same", "s"), e("Diff", "y")}, {});
        CHECK(p.size() == 4);
        CHECK(action(p, "OnlyLocal") == SyncAction::PushToShared && action(p, "OnlyShared") == SyncAction::PullFromShared && action(p, "Same") == SyncAction::None && action(p, "Diff") == SyncAction::Conflict);
    }
    // With a last-synced hash: the side that moved wins; both moved is a conflict.
    {
        const std::map<std::string, std::string> base = {{"A", "1"}, {"B", "1"}, {"C", "1"}, {"D", "1"}, {"E", "1"}, {"F", "1"}};
        const auto p = planSync({e("A", "1"), e("B", "2"), e("C", "2"), e("D", "1"), e("E", "3")}, {e("A", "1"), e("B", "1"), e("C", "3"), e("D", "2"), e("E", "3"), e("F", "1")}, base);
        CHECK(action(p, "A") == SyncAction::None);
        CHECK(action(p, "B") == SyncAction::PushToShared);     // local changed
        CHECK(action(p, "C") == SyncAction::Conflict);         // both changed, differently
        CHECK(action(p, "D") == SyncAction::PullFromShared);   // shared changed
        CHECK(action(p, "E") == SyncAction::None);             // both changed to the same contents
        CHECK(action(p, "F") == SyncAction::None);             // gone from this PC, unchanged on shared: never re-pulled (and never deleted)
    }
    // A file deleted on shared by another PC is not resurrected unless it changed here since.
    {
        const auto p = planSync({e("Gone", "1"), e("Edited", "2")}, {}, {{"Gone", "1"}, {"Edited", "1"}});
        CHECK(action(p, "Gone") == SyncAction::None && action(p, "Edited") == SyncAction::PushToShared);
    }
    CHECK(planSync({}, {}, {}).empty());
    CHECK(planSync({e("X", "1")}, {e("X", "1")}, {}).front().action == SyncAction::None);
    // Sorted by name, one item per name, and no action is ever a delete (the enum has none).
    const auto p = planSync({e("b", "1"), e("a", "1")}, {e("c", "1")}, {});
    CHECK(p.size() == 3 && p[0].name == "a" && p[1].name == "b" && p[2].name == "c");
}

// ============================================================================ first run
bool modeKnown(const std::string& m) {
    for (int i = 1; i <= 7; ++i) { if (m == cascade::core::kDemodNames[i]) { return true; } }   // NFM WFM AM DSB USB CW LSB (RAW is not a listening mode)
    return false;
}

void testStarterCatalogue() {
    std::set<std::string> ids, names;
    for (const auto& p : allStarterPresets()) {
        CHECK(!p.id.empty() && !p.title.empty() && !p.bookmarks.empty() && ids.insert(p.id).second);
        for (const auto& b : p.bookmarks) {
            CHECK(!b.name.empty() && b.name.size() <= 64 && names.insert(b.name).second);
            CHECK(b.freqHz > 0 && b.freqHz < 100.0e9 && b.bandwidthHz > 0 && b.bandwidthHz <= 1.0e6);
            CHECK(modeKnown(b.mode));
            CHECK(!b.group.empty() && b.group.rfind("Starter: ", 0) == 0);
        }
    }
    CHECK(ids.size() == 8);
    // Fit: every offered preset lies inside the radio's windows; the families with no windows are offered nothing.
    for (const auto& c : radioChoices()) {
        const auto offered = startersFor(c.family);
        if (c.windowsHz.empty()) { CHECK(offered.empty()); continue; }
        CHECK(!offered.empty());
        for (const auto& p : offered) {
            for (const auto& b : p.bookmarks) {
                bool inside = false;
                for (const auto& w : c.windowsHz) { if (b.freqHz >= w.first && b.freqHz <= w.second) { inside = true; } }
                CHECK(inside);
            }
        }
    }
    auto idsFor = [](RadioFamily f) { std::set<std::string> s; for (const auto& p : startersFor(f)) { s.insert(p.id); } return s; };
    CHECK(idsFor(RadioFamily::RtlSdr) == (std::set<std::string>{"fm-broadcast", "airband", "marine", "pmr446", "ham-2m"}));       // no HF on the conservative window
    CHECK(idsFor(RadioFamily::Airspy) == idsFor(RadioFamily::RtlSdr));                                                              // 24 MHz and up: no HF either
    CHECK(idsFor(RadioFamily::AirspyHf) == (std::set<std::string>{"fm-broadcast", "airband", "marine", "ham-2m", "time-signals", "shortwave-49m", "ham-hf"}));   // HF window and 64-260 MHz, but not 446
    CHECK(idsFor(RadioFamily::HackRf).size() == 8 && idsFor(RadioFamily::Rx888).size() == 8 && idsFor(RadioFamily::MiriSdr).size() == 8);   // 5 MHz and 6 MHz are inside 1 MHz-6 GHz / 10 kHz-1.75 GHz / 150 kHz-2 GHz
    // The source kinds are the config's own words.
    const std::set<std::string> kinds = {"siggen", "rtlsdr", "airspy", "airspyhf", "sdrplay", "hackrf", "rx888", "mirisdr", "pluto", "file", "soundcard"};
    std::set<std::string> have;
    for (const auto& c : radioChoices()) { have.insert(c.sourceKind); CHECK(std::string(c.label).size() > 3); }
    CHECK(have == kinds);
    // The data goes through the program's own bookmark file and comes back the same.
    cascade::core::FreqManager fm;
    std::vector<cascade::core::Bookmark> all;
    for (const auto& p : allStarterPresets()) { for (const auto& b : p.bookmarks) { all.push_back(b); } }
    CHECK(fm.addMany(all) == all.size());
    const fs::path file = g_base / "starter-bookmarks.json";
    std::string err;
    CHECK(fm.save(file.string(), err));
    cascade::core::FreqManager back;
    CHECK(back.load(file.string(), err) && back.list().size() == all.size());
    for (const auto& b : all) {
        bool found = false;
        for (const auto& x : back.list()) { if (x.name == b.name && x.freqHz == b.freqHz && x.mode == b.mode && x.bandwidthHz == b.bandwidthHz && x.group == b.group) { found = true; } }
        CHECK(found);
    }
}

void testWizardFlow() {
    FirstRunWizard w;
    CHECK(w.step() == WizardStep::Welcome && w.canNext() && !w.back());
    CHECK(w.next() && w.step() == WizardStep::Radio);
    CHECK(!w.canNext() && !w.next() && w.step() == WizardStep::Radio);   // a radio must be chosen
    CHECK(w.offered().empty());
    CHECK(w.chooseRadio(RadioFamily::RtlSdr) && w.canNext());
    CHECK(w.offered().size() == 5);
    CHECK(w.next() && w.step() == WizardStep::Preset);
    CHECK(!w.choosePreset("time-signals"));                  // does not fit an RTL-SDR window
    CHECK(!w.choosePreset("no-such-preset"));
    CHECK(w.choosePreset("airband"));
    CHECK(w.back() && w.step() == WizardStep::Radio);         // going back loses nothing
    CHECK(w.chooseRadio(RadioFamily::RtlSdr) && w.next() && w.next() && w.step() == WizardStep::Privacy);
    CHECK(!w.canNext() && !w.next());                         // the privacy switch must have been shown and answered
    w.chooseTelemetry(false);
    CHECK(w.next() && w.step() == WizardStep::Done && w.finished());
    CHECK(!w.next() && !w.back());
    const WizardOutcome o = w.outcome();
    CHECK(o.applied && o.sourceKind == "rtlsdr" && o.presetId == "airband" && !o.telemetryEnabled);
    CHECK(o.bookmarks.size() == 1 && o.bookmarks[0].freqHz == 121.5e6 && o.bookmarks[0].mode == "AM");
    // Changing the radio drops a preset that no longer fits, keeps one that still does.
    FirstRunWizard v;
    v.next();
    v.chooseRadio(RadioFamily::HackRf);
    v.next();
    CHECK(v.choosePreset("ham-hf"));
    v.back();
    v.chooseRadio(RadioFamily::Airspy);                       // no HF
    v.next();
    v.chooseTelemetry(true);
    v.next();
    CHECK(v.next());
    CHECK(v.outcome().presetId.empty() && v.outcome().bookmarks.empty() && v.outcome().sourceKind == "airspy" && v.outcome().telemetryEnabled);
    FirstRunWizard keep;
    keep.next(); keep.chooseRadio(RadioFamily::HackRf); keep.next(); keep.choosePreset("fm-broadcast"); keep.back(); keep.chooseRadio(RadioFamily::Airspy); keep.next(); keep.chooseTelemetry(true); keep.next(); keep.next();
    CHECK(keep.outcome().presetId == "fm-broadcast" && keep.outcome().bookmarks.size() == 1);
    // "No radio yet" is a radio; "no preset" is a preset; the default for usage reporting is the app's own (ON).
    FirstRunWizard nr;
    nr.next();
    CHECK(nr.chooseRadio(RadioFamily::NoneYet) && nr.next() && nr.next());
    nr.chooseTelemetry(true);
    nr.next();
    const auto none = nr.outcome();
    CHECK(none.applied && none.sourceKind == "siggen" && none.bookmarks.empty() && none.telemetryEnabled);
    FirstRunWizard dflt;
    CHECK(dflt.outcome().telemetryEnabled && !dflt.outcome().applied);
    // Skip applies nothing, from any step; an out-of-range radio is refused.
    for (int steps = 0; steps < 4; ++steps) {
        FirstRunWizard s;
        s.next();
        if (steps >= 1) { s.chooseRadio(RadioFamily::Rx888); s.next(); }
        if (steps >= 2) { s.choosePreset("ham-hf"); s.next(); }
        if (steps >= 3) { s.chooseTelemetry(false); }
        s.skip();
        CHECK(s.finished() && !s.outcome().applied && s.outcome().bookmarks.empty() && s.outcome().sourceKind.empty());
    }
    FirstRunWizard bad;
    CHECK(!bad.chooseRadio(static_cast<RadioFamily>(99)));
    CHECK(!bad.choosePreset("airband"));                      // no radio chosen yet
}

void testFirstRunMarker() {
    const fs::path dir = g_base / "firstrun" / "foxsdr";
    fs::remove_all(g_base / "firstrun");
    CHECK(firstRunNeeded(dir));                               // nothing at all
    fs::create_directories(dir);
    CHECK(firstRunNeeded(dir));                               // an empty folder
    spit(dir / "bookmarks.json", "{}");
    CHECK(firstRunNeeded(dir));                               // bookmarks alone do not make an existing user
    CHECK(markFirstRunDone(dir) && !firstRunNeeded(dir));
    const fs::path other = g_base / "firstrun2" / "foxsdr";
    spit(other / "config.json", "{}");
    CHECK(!firstRunNeeded(other));                            // anyone who already has settings never sees it
    const fs::path fresh = g_base / "firstrun3" / "nested" / "foxsdr";
    CHECK(markFirstRunDone(fresh) && fs::exists(fresh / kFirstRunMarker) && !firstRunNeeded(fresh));   // creates the folder
    const fs::path blocked = g_base / "firstrun4";
    spit(blocked, "i am a file");
    CHECK(!markFirstRunDone(blocked / "foxsdr"));
}

}  // namespace

int main() {
    g_base = fs::temp_directory_path() / ("sysmodels-setup-" + std::to_string(TEST_GETPID()));
    fs::remove_all(g_base);
    fs::create_directories(g_base);
    testTelemetrySwitch();
    testOffSendsNothing();
    testModelMatchesRealLoad();
    testResetNames();
    testResetPlan();
    testResetExecuteAndUndo();
    testResetRollback();
    testProfileNames();
    testProfileStore();
    testProfileSwitch();
    testRadioMap();
    testSyncPlan();
    testStarterCatalogue();
    testWizardFlow();
    testFirstRunMarker();
    std::error_code ec;
    fs::remove_all(g_base, ec);
    return testSummary("test_sys_setup");
}
