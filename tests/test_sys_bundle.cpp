// Tests for core/sys_bundle.hpp (lane sysmodels): the settings bundle - export, import (validate, plan, commit), backups. Scratch folders only (PID in the name);
// the real settings folder is never named. The config used is a REAL one written by ConfigStore::save, and the imported result is read back by ConfigStore::load.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
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
#include "core/sys_bundle.hpp"
#include "core/sys_hash.hpp"
#include "core/sys_manifest_sig.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using nlohmann::json;
using namespace cascade::core::sys;

namespace {

fs::path g_base;

std::string slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()); }
void spit(const fs::path& p, const std::string& s) { std::ofstream f(p, std::ios::binary | std::ios::trunc); f << s; }
std::string dumpS(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

const char* kSecretRecord = "pbkdf2$600000$SALTSALTSALT$HASHHASHHASH-very-secret";
const char* kToken = "0123456789abcdef0123456789abcdef";

std::string realConfigBytes(bool listeners) {
    cascade::core::AppConfig cfg;
    cfg.webPort = 4321;
    cfg.webPasswordRecord = kSecretRecord;
    cfg.testerToken = kToken;
    cfg.telemetryInstallId = "fedcba9876543210fedcba9876543210";
    cfg.telemetryLaunches = 17;
    cfg.webEnabled = listeners;
    cfg.catEnabled = listeners;
    cfg.tciEnabled = listeners;
    cfg.udpAudioEnabled = listeners;
    const fs::path p = g_base / "real-config.json";
    std::string err;
    CHECK(cascade::core::ConfigStore::save(p.string(), cfg, err));
    return slurp(p);
}

const char* kBookmarks = "{\"schemaVersion\":1,\"bookmarks\":[{\"name\":\"BBC R4\",\"freqHz\":198000,\"mode\":\"AM\",\"bandwidthHz\":9000,\"group\":\"\",\"favourite\":true}]}";
const char* kMarkers = "{\"schemaVersion\":1,\"next\":2,\"markers\":[{\"id\":1,\"freqHz\":7074000}]}";

std::vector<BundleFile> allFiles(bool listeners = false) {
    return {{"config.json", realConfigBytes(listeners)}, {"bookmarks.json", kBookmarks}, {"markers.json", kMarkers}};
}

// A bundle written by hand, so a test can make every kind of wrong one with a correct hash where it needs one.
std::string rawBundle(const std::vector<std::pair<std::string, std::string>>& files, int version = 1) {
    json b;
    b["format"] = "foxsdr-settings-bundle";
    b["version"] = version;
    b["files"] = json::array();
    for (const auto& f : files) {
        json e;
        e["name"] = f.first;
        e["size"] = f.second.size();
        e["sha256"] = sha256Hex(f.second);
        e["data"] = base64Encode(f.second);
        b["files"].push_back(e);
    }
    return dumpS(b);
}

void testExportStripsSecrets() {
    ExportOptions opt;
    opt.appVersion = "0.99.109";
    opt.createdUtc = "20261111-221530";
    ExportReport rep;
    const std::string text = exportBundle(allFiles(), opt, rep);
    CHECK(rep.error == BundleError::None && !text.empty());
    // No secret VALUE anywhere in the bundle: not in the text, not after decoding either.
    CHECK(text.find(kSecretRecord) == std::string::npos && text.find(kToken) == std::string::npos && text.find("fedcba9876543210") == std::string::npos);
    const ImportResult r = parseBundle(text);
    CHECK(r.error == BundleError::None && r.files.size() == 3 && r.appVersion == "0.99.109");
    for (const auto& f : r.files) {
        CHECK(f.bytes.find(kSecretRecord) == std::string::npos && f.bytes.find(kToken) == std::string::npos && f.bytes.find("fedcba9876543210") == std::string::npos);
    }
    bool hasPw = false, hasTok = false, hasId = false;
    for (const auto& s : rep.stripped) {
        if (s == "config.json:webPasswordRecord") { hasPw = true; }
        if (s == "config.json:testerToken") { hasTok = true; }
        if (s == "config.json:telemetryInstallId") { hasId = true; }
        CHECK(s.find(kSecretRecord) == std::string::npos);   // paths only, never values
    }
    CHECK(hasPw && hasTok && hasId);
    // The non-secret settings survive, and the result is a config the program itself can read.
    const std::string cfgBytes = r.files[0].bytes;
    CHECK(r.files[0].name == "config.json");
    const fs::path out = g_base / "imported-config.json";
    spit(out, cfgBytes);
    cascade::core::AppConfig back;
    std::string err;
    CHECK(cascade::core::ConfigStore::load(out.string(), back, err));
    CHECK(back.webPort == 4321 && back.webPasswordRecord.empty() && back.testerToken.empty() && back.telemetryInstallId.empty());
    // telemetryEnabled (a preference, not an identity) is kept.
    CHECK(json::parse(cfgBytes).contains("telemetryEnabled"));
    // stripIdentity off keeps the identity but still drops every secret.
    ExportOptions keep;
    keep.stripIdentity = false;
    ExportReport rep2;
    const std::string text2 = exportBundle(allFiles(), keep, rep2);
    CHECK(rep2.error == BundleError::None);
    CHECK(text2.find(kSecretRecord) == std::string::npos);
    const ImportResult r2 = parseBundle(text2);
    // ... but the importer drops identity again (a bundle from outside never sets this install's identity) and says so.
    CHECK(r2.error == BundleError::None && !r2.notes.empty());
    CHECK(!json::parse(r2.files[0].bytes).contains("telemetryInstallId"));
    // Nested keys, at any depth, in arrays too.
    ExportReport rep3;
    const std::string nested = exportBundle({{"config.json", "{\"a\":{\"b\":[{\"apiKey\":\"x\",\"keep\":1},{\"Passphrase\":\"y\"}]},\"BearerToken\":\"z\"}"}}, ExportOptions{}, rep3);
    CHECK(rep3.error == BundleError::None);
    CHECK(rep3.stripped.size() == 3);
    CHECK(nested.find("\"x\"") == std::string::npos);
    const ImportResult rn = parseBundle(nested);
    CHECK(rn.error == BundleError::None);
    CHECK(json::parse(rn.files[0].bytes) == json::parse("{\"a\":{\"b\":[{\"keep\":1},{}]}}"));
    // The key-name rule.
    for (const char* k : {"webPasswordRecord", "testerToken", "testerAppToken", "apiKey", "API_KEY", "clientSecret", "privateKey", "myCredentials", "passwd"}) { CHECK(secretKeyName(k)); }
    for (const char* k : {"webPort", "keyBindings", "keyboard", "bandwidthHz", "frequency", "name"}) { CHECK(!secretKeyName(k)); }
    CHECK(identityKeyName("telemetryInstallId") && identityKeyName("telemetryPending") && !identityKeyName("telemetryEnabled") && !identityKeyName("webPort"));
}

void testExportErrors() {
    ExportReport rep;
    CHECK(exportBundle({}, ExportOptions{}, rep).empty() && rep.error == BundleError::EmptyBundle);
    CHECK(exportBundle({{"../config.json", "{}"}}, ExportOptions{}, rep).empty() && rep.error == BundleError::BadName);
    CHECK(exportBundle({{"evil.txt", "{}"}}, ExportOptions{}, rep).empty() && rep.error == BundleError::BadName);
    CHECK(exportBundle({{"config.json", "{}"}, {"config.json", "{}"}}, ExportOptions{}, rep).empty() && rep.error == BundleError::DuplicateName);
    CHECK(exportBundle({{"config.json", "not json"}}, ExportOptions{}, rep).empty() && rep.error == BundleError::BadContent);
    CHECK(exportBundle({{"config.json", "[1,2]"}}, ExportOptions{}, rep).empty() && rep.error == BundleError::BadContent);
    CHECK(exportBundle({{"config.json", std::string(kBundleMaxFileBytes + 1, ' ')}}, ExportOptions{}, rep).empty() && rep.error == BundleError::TooLarge);
    CHECK(exportBundle({{"config.json", std::string(200, '[') + std::string(200, ']')}}, ExportOptions{}, rep).empty() && rep.error == BundleError::BadContent);
    // Boundary: an empty object exports and imports.
    const std::string t = exportBundle({{"config.json", "{}"}}, ExportOptions{}, rep);
    CHECK(rep.error == BundleError::None);
    const auto r = parseBundle(t);
    CHECK(r.error == BundleError::None && r.files.size() == 1 && json::parse(r.files[0].bytes) == json::object());
}

void testParseRefusals() {
    const std::string good = rawBundle({{"config.json", "{\"webPort\":1}"}});
    CHECK(parseBundle(good).error == BundleError::None);
    CHECK(parseBundle("").error == BundleError::NotJson);
    CHECK(parseBundle("{").error == BundleError::NotJson);
    CHECK(parseBundle("[1]").error == BundleError::NotABundle);
    CHECK(parseBundle("{\"format\":\"something-else\"}").error == BundleError::NotABundle);
    CHECK(parseBundle("{\"format\":\"foxsdr-settings-bundle\"}").error == BundleError::BadManifest);
    CHECK(parseBundle(rawBundle({{"config.json", "{}"}}, 2)).error == BundleError::UnknownVersion);
    CHECK(parseBundle(rawBundle({{"config.json", "{}"}}, 0)).error == BundleError::UnknownVersion);
    CHECK(parseBundle(rawBundle({{"config.json", "{}"}}, -1)).error == BundleError::UnknownVersion);
    CHECK(parseBundle(rawBundle({})).error == BundleError::EmptyBundle);
    // Path traversal and other names: every one refused as a name, never written anywhere.
    for (const char* name : {"../config.json", "..\\config.json", "..", "config.json/../x", "C:\\Windows\\x.json", "/etc/passwd", "CON", "config.JSON", "config.json ", "plugins/x.dll", "foxsdr.portable"}) {
        CHECK(parseBundle(rawBundle({{name, "{}"}})).error == BundleError::BadName);
    }
    CHECK(parseBundle(rawBundle({{"config.json", "{}"}, {"config.json", "{}"}})).error == BundleError::DuplicateName);
    {   // too many files (the list is checked before any name)
        json b = json::parse(good);
        for (int i = 0; i < 8; ++i) { b["files"].push_back(b["files"][0]); }
        CHECK(parseBundle(dumpS(b)).error == BundleError::TooManyFiles);
    }
    auto mutate = [&](auto fn) { json b = json::parse(good); fn(b["files"][0]); return dumpS(b); };
    CHECK(parseBundle(mutate([](json& e) { e["size"] = 999; })).error == BundleError::SizeMismatch);
    CHECK(parseBundle(mutate([](json& e) { e["sha256"] = std::string(64, '0'); })).error == BundleError::HashMismatch);
    CHECK(parseBundle(mutate([](json& e) { e["sha256"] = "XYZ"; })).error == BundleError::BadManifest);
    CHECK(parseBundle(mutate([](json& e) { e["sha256"] = std::string(64, 'A'); })).error == BundleError::BadManifest);   // upper case is not the format
    CHECK(parseBundle(mutate([](json& e) { e["data"] = std::string(e["data"].get<std::string>().size(), '!'); })).error == BundleError::BadBase64);
    CHECK(parseBundle(mutate([](json& e) { e["data"] = 5; })).error == BundleError::BadManifest);
    CHECK(parseBundle(mutate([](json& e) { e.erase("sha256"); })).error == BundleError::BadManifest);
    CHECK(parseBundle(mutate([](json& e) { e["size"] = -1; })).error == BundleError::BadManifest);
    CHECK(parseBundle(mutate([](json& e) { e["size"] = "13"; })).error == BundleError::BadManifest);
    // A data string far larger than its declared size is refused BEFORE any decode (the bomb shape: tiny declaration, huge payload).
    CHECK(parseBundle(mutate([](json& e) { e["data"] = std::string(4u * 1024 * 1024, 'A'); })).error == BundleError::SizeMismatch);
    // Declared sizes over the limits: refused from the declaration alone, with no data at all behind them.
    CHECK(parseBundle(mutate([](json& e) { e["size"] = 9ull * 1024 * 1024 * 1024; e["data"] = ""; })).error == BundleError::TooLarge);
    CHECK(parseBundle(mutate([](json& e) { e["size"] = kBundleMaxFileBytes + 1; e["data"] = ""; })).error == BundleError::TooLarge);
    {
        json b = json::parse(good);
        json e1 = b["files"][0], e2 = b["files"][0], e3 = b["files"][0];
        e1["name"] = "config.json"; e2["name"] = "bookmarks.json"; e3["name"] = "markers.json";
        e1["size"] = e2["size"] = e3["size"] = kBundleMaxFileBytes;   // 3 x 8 MiB = 24 MiB > 16 MiB total, declared only
        e1["data"] = e2["data"] = e3["data"] = "";
        b["files"] = json::array({e1, e2, e3});
        CHECK(parseBundle(dumpS(b)).error == BundleError::TooLarge);
    }
    // Contents: not an object, not JSON, nested too deeply (each with a CORRECT hash, so only the content check can refuse it).
    CHECK(parseBundle(rawBundle({{"config.json", "[1]"}})).error == BundleError::BadContent);
    CHECK(parseBundle(rawBundle({{"config.json", "nope"}})).error == BundleError::BadContent);
    CHECK(parseBundle(rawBundle({{"config.json", std::string(100, '[') + std::string(100, ']')}})).error == BundleError::BadContent);
    CHECK(parseBundle(rawBundle({{"config.json", ""}})).error == BundleError::BadContent);
    // Text over the limit; the envelope nested too deeply.
    CHECK(parseBundle(std::string(kBundleMaxTextBytes + 1, ' ')).error == BundleError::TooLarge);
    CHECK(parseBundle(std::string(200, '[') + std::string(200, ']')).error == BundleError::NotJson);
    // After any refusal nothing is half-returned.
    const auto bad = parseBundle(mutate([](json& e) { e["sha256"] = std::string(64, '0'); }));
    CHECK(bad.files.empty() && bad.notes.empty());
}

void testListenersImportedOff() {
    const std::string text = rawBundle({{"config.json", "{\"webEnabled\":true,\"catEnabled\":true,\"tciEnabled\":true,\"udpAudioEnabled\":true,\"webPort\":8080,\"webPasswordRecord\":\"x\"}"}});
    const ImportResult r = parseBundle(text);
    CHECK(r.error == BundleError::None);
    const json c = json::parse(r.files[0].bytes);
    CHECK(c["webEnabled"] == false && c["catEnabled"] == false && c["tciEnabled"] == false && c["udpAudioEnabled"] == false);
    CHECK(c["webPort"] == 8080 && !c.contains("webPasswordRecord"));
    CHECK(r.notes.size() == 5);   // four switches and the dropped password
    // Already-off switches produce no note.
    const ImportResult q = parseBundle(rawBundle({{"config.json", "{\"webEnabled\":false}"}}));
    CHECK(q.error == BundleError::None && q.notes.empty());
}

void testPlanAndCommit() {
    const fs::path dest = g_base / "dest";
    fs::remove_all(dest);
    fs::create_directories(dest);
    ExportReport rep;
    const std::string text = exportBundle(allFiles(), ExportOptions{}, rep);
    const ImportResult r = parseBundle(text);
    CHECK(r.error == BundleError::None);
    // Destination: config.json differs, bookmarks.json absent, markers.json identical to what the bundle carries.
    spit(dest / "config.json", "{\"webPort\":1}");
    spit(dest / "markers.json", r.files[2].bytes);
    const ImportPlan plan = planImport(r, dest);
    CHECK(plan.items.size() == 3);
    CHECK(plan.items[0].name == "config.json" && plan.items[0].action == ImportAction::Replace && !plan.items[0].presentSha256.empty());
    CHECK(plan.items[1].name == "bookmarks.json" && plan.items[1].action == ImportAction::New && plan.items[1].presentSha256.empty());
    CHECK(plan.items[2].name == "markers.json" && plan.items[2].action == ImportAction::Identical);
    CHECK(plan.confirmToken.rfind("IMPORT-", 0) == 0 && plan.confirmToken.size() == 15);
    // Nothing is written by planning.
    CHECK(slurp(dest / "config.json") == "{\"webPort\":1}" && !fs::exists(dest / "bookmarks.json"));

    // A wrong, empty or stale token refuses, and changes nothing.
    std::string err;
    CHECK(!commitImport(r, dest, "", "20261111-221530", err) && !err.empty());
    CHECK(!commitImport(r, dest, "IMPORT-00000000", "20261111-221530", err));
    CHECK(!commitImport(r, dest, plan.confirmToken, "bad stamp!", err));
    CHECK(slurp(dest / "config.json") == "{\"webPort\":1}" && !fs::exists(dest / "bookmarks.json"));
    spit(dest / "config.json", "{\"webPort\":2}");   // the destination changed AFTER the plan was shown
    CHECK(!commitImport(r, dest, plan.confirmToken, "20261111-221530", err));
    CHECK(slurp(dest / "config.json") == "{\"webPort\":2}");

    // The right token: replaced, kept, new written, identical untouched, no temp files left.
    const ImportPlan plan2 = planImport(r, dest);
    CHECK(commitImport(r, dest, plan2.confirmToken, "20261111-221530", err) && err.empty());
    CHECK(slurp(dest / "config.json") == r.files[0].bytes);
    CHECK(slurp(dest / "bookmarks.json") == r.files[1].bytes);
    CHECK(slurp(dest / "config.json.before-import-20261111-221530") == "{\"webPort\":2}");   // what was there is kept, byte for byte
    CHECK(!fs::exists(dest / "bookmarks.json.before-import-20261111-221530"));               // a new file has nothing to keep
    CHECK(!fs::exists(dest / "markers.json.before-import-20261111-221530"));
    for (const auto& e : fs::directory_iterator(dest)) { CHECK(e.path().extension() != ".import-tmp"); }
    // Planning again: everything identical, and committing that is a no-op that still needs its token.
    const ImportPlan plan3 = planImport(r, dest);
    for (const auto& it : plan3.items) { CHECK(it.action == ImportAction::Identical); }
    CHECK(commitImport(r, dest, plan3.confirmToken, "20261111-221531", err));
    CHECK(!fs::exists(dest / "config.json.before-import-20261111-221531"));
    // An invalid ImportResult is never committed.
    ImportResult broken;
    broken.error = BundleError::HashMismatch;
    CHECK(!commitImport(broken, dest, "x", "20261111-221530", err));
    CHECK(!commitImport(ImportResult{}, dest, "x", "20261111-221530", err));   // no files
    // A destination folder that does not exist yet is created (a fresh portable stick).
    const fs::path fresh = g_base / "fresh" / "settings";
    const ImportPlan pf = planImport(r, fresh);
    CHECK(pf.items.size() == 3 && pf.items[0].action == ImportAction::New);
    CHECK(commitImport(r, fresh, pf.confirmToken, "20261111-221530", err));
    CHECK(slurp(fresh / "bookmarks.json") == r.files[1].bytes);
}

void testRollback() {
    const fs::path dest = g_base / "rollback";
    fs::remove_all(dest);
    fs::create_directories(dest);
    ExportReport rep0;
    const ImportResult r = parseBundle(exportBundle(allFiles(), ExportOptions{}, rep0));
    CHECK(r.error == BundleError::None);
    spit(dest / "config.json", "{\"webPort\":1}");
    spit(dest / "bookmarks.json", "{\"bookmarks\":[]}");
    spit(dest / "markers.json", "{\"markers\":[]}");
    // The LAST target is made read-only, so its rename fails after the first two were already put in place.
    std::error_code ec;
    fs::permissions(dest / "markers.json", fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read, ec);
    const ImportPlan plan = planImport(r, dest);
    std::string err;
    const bool ok = commitImport(r, dest, plan.confirmToken, "20261111-230000", err);
#ifdef _WIN32
    CHECK(!ok && !err.empty());
    CHECK(slurp(dest / "config.json") == "{\"webPort\":1}");          // put back
    CHECK(slurp(dest / "bookmarks.json") == "{\"bookmarks\":[]}");     // put back
    CHECK(slurp(dest / "markers.json") == "{\"markers\":[]}");
    for (const auto& e : fs::directory_iterator(dest)) { CHECK(e.path().extension() != ".import-tmp"); }
#else
    (void)ok;
    SKIP_LINUX("a read-only target does not stop a rename over it on POSIX");
#endif
    for (const auto& e : fs::directory_iterator(dest)) { fs::permissions(e.path(), fs::perms::owner_all, ec); }
}

void testBackups() {
    CHECK(backupFileName("20261111-221530") == "foxsdr-settings-20261111-221530.foxsdr-bundle");
    CHECK(isBackupFileName("foxsdr-settings-20261111-221530.foxsdr-bundle"));
    for (const char* bad : {"foxsdr-settings-20261111-221530.foxsdr-bundle.bak", "foxsdr-settings-notes.foxsdr-bundle", "foxsdr-settings-2026111-2215300.foxsdr-bundle", "foxsdr-settings-2026111a-221530.foxsdr-bundle",
                            "config.json", "", "foxsdr-settings-20261111_221530.foxsdr-bundle", "xfoxsdr-settings-20261111-221530.foxsdr-bundle"}) {
        CHECK(!isBackupFileName(bad));
    }
    const fs::path dir = g_base / "backups";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::vector<std::string> ours;
    for (int d = 1; d <= 7; ++d) {
        const std::string n = backupFileName("2026110" + std::to_string(d) + "-120000");
        spit(dir / n, "{}");
        ours.push_back(n);
    }
    spit(dir / "foxsdr-settings-notes.txt", "mine");
    spit(dir / "foxsdr-settings-20261101-120000.foxsdr-bundle.bak", "mine too");
    fs::create_directory(dir / backupFileName("20250101-000000"));   // a folder with a matching name is not a backup file
    CHECK(pruneBackups(dir, 10).empty());
    const auto gone = pruneBackups(dir, 3);
    CHECK(gone == std::vector<std::string>(ours.begin(), ours.begin() + 4));   // the four OLDEST
    for (std::size_t i = 4; i < ours.size(); ++i) { CHECK(fs::exists(dir / ours[i])); }
    for (std::size_t i = 0; i < 4; ++i) { CHECK(!fs::exists(dir / ours[i])); }
    CHECK(fs::exists(dir / "foxsdr-settings-notes.txt") && fs::exists(dir / "foxsdr-settings-20261101-120000.foxsdr-bundle.bak") && fs::is_directory(dir / backupFileName("20250101-000000")));
    CHECK(pruneBackups(dir, 3).empty());   // idempotent
    CHECK(pruneBackups(dir, 0).size() == 3 && fs::exists(dir / "foxsdr-settings-notes.txt"));
    CHECK(pruneBackups(g_base / "no-such-dir", 3).empty());
}

// FUZZ: the importer takes outside bytes. Mutants of a valid bundle must never crash or hang; whatever is accepted must still be allow-listed, bounded JSON objects.
void testFuzz() {
    ExportReport rep;
    const std::string text = exportBundle(allFiles(true), ExportOptions{}, rep);
    CHECK(rep.error == BundleError::None && parseBundle(text).error == BundleError::None);
    std::uint64_t s = 20261111;
    auto next = [&] { s = s * 6364136223846793005ull + 1442695040888963407ull; return static_cast<std::uint32_t>(s >> 33); };
    int accepted = 0;
    const int kRuns = 6000;
    for (int it = 0; it < kRuns; ++it) {
        std::string m = text;
        const int rounds = 1 + static_cast<int>(next() % 3);
        for (int k = 0; k < rounds; ++k) {
            switch (next() % 6) {
                case 0: m[next() % m.size()] = static_cast<char>(next()); break;
                case 1: m.resize(1 + next() % m.size()); break;
                case 2: m.insert(next() % m.size(), 1, static_cast<char>(next())); break;
                case 3: m.erase(next() % m.size(), 1 + next() % 16); break;
                case 4: { const std::size_t a = next() % m.size(), b = next() % m.size(); std::swap(m[a], m[b]); break; }
                default: { const std::size_t a = next() % m.size(); m.insert(a, m.substr(next() % m.size(), 1 + next() % 40)); break; }
            }
            if (m.empty()) { m = "{"; }
        }
        const ImportResult r = parseBundle(m);
        if (r.error == BundleError::None) {
            ++accepted;
            CHECK(!r.files.empty() && r.files.size() <= kBundleMaxFiles);
            for (const auto& f : r.files) {
                CHECK(allowedBundleFileName(f.name));
                CHECK(f.bytes.size() <= kBundleMaxFileBytes);
                CHECK(json::parse(f.bytes, nullptr, false).is_object());
                CHECK(f.bytes.find(kSecretRecord) == std::string::npos);
                if (f.name == "config.json") {
                    const json c = json::parse(f.bytes);
                    for (const char* sw : {"webEnabled", "catEnabled", "tciEnabled", "udpAudioEnabled"}) { CHECK(!c.contains(sw) || c[sw] == false); }
                }
            }
        } else {
            CHECK(r.files.empty());
        }
    }
    std::printf("bundle fuzz: %d mutants, %d accepted (changes that leave every validated field intact), the rest refused\n", kRuns, accepted);
    CHECK(accepted < kRuns / 2);
}

void testConcurrentParse() {
    ExportReport rep;
    const std::string text = exportBundle(allFiles(), ExportOptions{}, rep);
    const ImportResult ref = parseBundle(text);
    std::atomic<int> bad{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 6; ++t) {
        ts.emplace_back([&] {
            for (int i = 0; i < 40; ++i) {
                const ImportResult r = parseBundle(text);
                if (r.error != BundleError::None || r.files.size() != ref.files.size() || r.files[0].bytes != ref.files[0].bytes) { ++bad; }
            }
        });
    }
    for (auto& t : ts) { t.join(); }
    CHECK(bad.load() == 0);
}

void testSource() {
    const fs::path root = fs::path(__FILE__).parent_path().parent_path();
    const std::string src = slurp(root / "src/core/sys_bundle.cpp");
    CHECK(!src.empty());
    for (const char* banned : {"getenv", "APPDATA", "SHGetKnownFolder", "SHGetFolderPath", "defaultPath", "ConfigStore"}) { CHECK(src.find(banned) == std::string::npos); }
}

}  // namespace

int main() {
    g_base = fs::temp_directory_path() / ("sysmodels-bundle-" + std::to_string(TEST_GETPID()));
    fs::remove_all(g_base);
    fs::create_directories(g_base);
    testExportStripsSecrets();
    testExportErrors();
    testParseRefusals();
    testListenersImportedOff();
    testPlanAndCommit();
    testRollback();
    testBackups();
    testFuzz();
    testConcurrentParse();
    testSource();
    std::error_code ec;
    fs::remove_all(g_base, ec);
    return testSummary("test_sys_bundle");
}
