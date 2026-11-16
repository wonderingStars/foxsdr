// Tests for core/secret_vault.hpp (engine lane E1, owner decision 16): the settings file's secrets live in the per-user protected store.
// Scratch folders only (PID in the name); the real settings folder is never named and nothing here can raise a window (CRYPTPROTECT_UI_FORBIDDEN).
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
#include "core/secret_vault.hpp"
#include "core/sys_bundle.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using nlohmann::json;
namespace vault = cascade::core::vault;
using cascade::core::AppConfig;
using cascade::core::ConfigStore;

namespace {

fs::path g_base;

const char* kTesterToken = "0123456789abcdef0123456789abcdef";
const char* kAppToken = "fedcba9876543210fedcba9876543210fedcba98";
const char* kRecord = "pbkdf2-sha256$600000$SALTSALTSALTSALT$HASHHASHHASHHASHHASH";

std::string slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()); }
void spit(const fs::path& p, const std::string& s) { std::ofstream f(p, std::ios::binary | std::ios::trunc); f << s; }
bool has(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

// Every file in the folder, as one string: the "is it anywhere as text" check.
std::string everythingIn(const fs::path& dir) {
    std::string all;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) { if (e.is_regular_file()) { all += slurp(e.path()); } }
    return all;
}

fs::path freshDir(const char* name) {
    const fs::path d = g_base / name;
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d);
    return d;
}

AppConfig configWithSecrets() {
    AppConfig c;
    c.testerToken = kTesterToken;
    c.testerAppToken = kAppToken;
    c.testerAppTokenName = "A tester";
    c.webPasswordRecord = kRecord;
    return c;
}

// RULE: a secret written is absent from the settings file (and from every file beside it) as text, and reads back.
void testWriteKeepsSecretsOutOfTheFile() {
    const fs::path d = freshDir("write");
    const std::string path = (d / "config.json").string();
    std::string err;
    CHECK(ConfigStore::save(path, configWithSecrets(), err));
    CHECK(err.empty());
    const std::string file = slurp(path);
    CHECK(!file.empty());
    CHECK(!has(file, kTesterToken));
    CHECK(!has(file, kAppToken));
    CHECK(!has(file, "SALTSALTSALT"));
    CHECK(has(file, "testerToken"));   // the key stays, empty
    CHECK(fs::exists(vault::sidecarPath(path)));
    const std::string side = slurp(vault::sidecarPath(path));
    CHECK(!has(side, kTesterToken));   // not as text in the protected copy either
    CHECK(!has(side, kAppToken));
    CHECK(!has(side, "SALTSALTSALT"));
    if (vault::hasProtectedStore()) { CHECK(has(side, "dpapi:")); }
    AppConfig back;
    CHECK(ConfigStore::load(path, back, err));
    CHECK(back.testerToken == kTesterToken);
    CHECK(back.testerAppToken == kAppToken);
    CHECK(back.webPasswordRecord == kRecord);
    CHECK(back.testerAppTokenName == "A tester");   // not a secret: stays in the file
}

// RULE: a second save (the dated copy of the first is kept beside the file) leaves no plain copy anywhere in the folder.
// L10 (security review 1): on Linux the protected copy is created readable by its owner only, from its first byte (no 0644 window before a chmod), and no temporary copy is left.
// A Linux-only check: it is not compiled on Windows, and was NOT RUN there (UNVERIFIED until the Linux build runs it).
#ifndef _WIN32
void testSidecarIsOwnerOnlyFromCreation() {
    const fs::path d = freshDir("perm");
    const std::string path = (d / "config.json").string();
    std::string err;
    CHECK(ConfigStore::save(path, configWithSecrets(), err));
    const fs::perms p = fs::status(vault::sidecarPath(path)).permissions();
    CHECK((p & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
    CHECK(!fs::exists(vault::sidecarPath(path) + ".vtmp"));
}
#endif

void testNoPlainCopyAfterManySaves() {
    const fs::path d = freshDir("many");
    const std::string path = (d / "config.json").string();
    std::string err;
    AppConfig c = configWithSecrets();
    for (int i = 0; i < 3; ++i) {
        c.webPort = 4000 + i;   // each save changes the text, so the writer keeps a dated copy
        CHECK(ConfigStore::save(path, c, err));
    }
    const std::string all = everythingIn(d);
    CHECK(!has(all, kTesterToken));
    CHECK(!has(all, kAppToken));
    CHECK(!has(all, "SALTSALTSALT"));
}

// RULE: a file an older build wrote with the secrets in plain text is migrated on its first load: no plain copy is left, the dated copies
// are scrubbed, and the values are still there to read.
void testMigration() {
    const fs::path d = freshDir("migrate");
    const std::string path = (d / "config.json").string();
    json old;
    old["webPort"] = 4321;
    old["testerToken"] = kTesterToken;
    old["testerAppToken"] = kAppToken;
    old["webPasswordRecord"] = kRecord;
    spit(path, old.dump(4) + "\n");
    spit(path + ".20261101-120000.bak", old.dump(4) + "\n");   // an older dated copy that also holds them
    spit(path + ".20261102-120000.bak", "not json at all " + std::string(kTesterToken));   // damaged: left alone (not parseable)
    // L11 (security review 1): EVERY copy of the settings file in the folder is scrubbed, whatever it is called - an import's kept copy, a reset's, a hand-made one
    spit(path + ".before-import-20261110-101010", old.dump(4) + "\n");
    spit(path + ".before-reset-20261110-101011", old.dump(4) + "\n");
    spit(path + ".mine-old", old.dump(4) + "\n");
    spit((d / "other.json.20261101-120000.bak").string(), old.dump(4) + "\n");               // another program's file: not a copy of THIS settings file, left alone
    spit((d / "config.jsonX").string(), old.dump(4) + "\n");                                   // a longer name that is not "<name>." + something: left alone
    CHECK(has(everythingIn(d), kTesterToken));
    AppConfig c;
    std::string err;
    CHECK(ConfigStore::load(path, c, err));
    CHECK(c.testerToken == kTesterToken);
    CHECK(c.testerAppToken == kAppToken);
    CHECK(c.webPasswordRecord == kRecord);
    CHECK(c.webPort == 4321);
    const std::string file = slurp(path);
    CHECK(!has(file, kTesterToken));
    CHECK(!has(file, kAppToken));
    CHECK(!has(file, "SALTSALTSALT"));
    CHECK(!has(slurp(path + ".20261101-120000.bak"), kTesterToken));   // scrubbed
    CHECK(!has(slurp(path + ".20261101-120000.bak"), kAppToken));
    for (const char* copy : {".before-import-20261110-101010", ".before-reset-20261110-101011", ".mine-old"}) {
        CHECK(!has(slurp(path + copy), kTesterToken));
        CHECK(!has(slurp(path + copy), kAppToken));
        CHECK(!has(slurp(path + copy), "SALTSALTSALT"));
        CHECK(has(slurp(path + copy), "4321"));   // only the secrets were taken out: the rest of the copy is as it was
    }
    CHECK(has(slurp(d / "other.json.20261101-120000.bak"), kTesterToken));   // not ours
    CHECK(has(slurp(d / "config.jsonX"), kTesterToken));
    CHECK(has(slurp(path + ".20261102-120000.bak"), kTesterToken));         // not a JSON object: nothing to take out
    CHECK(fs::exists(vault::sidecarPath(path)));
    // loading again changes nothing and still reads them
    const std::string before = slurp(path);
    AppConfig c2;
    CHECK(ConfigStore::load(path, c2, err));
    CHECK(c2.testerToken == kTesterToken);
    CHECK(slurp(path) == before);
}

// RULE: clearing a secret (saving it empty) removes the protected copy; a cleared secret does not come back.
void testClear() {
    const fs::path d = freshDir("clear");
    const std::string path = (d / "config.json").string();
    std::string err;
    AppConfig c = configWithSecrets();
    CHECK(ConfigStore::save(path, c, err));
    c.testerToken.clear();
    c.testerAppToken.clear();
    c.testerAppTokenName.clear();
    c.webPasswordRecord.clear();
    CHECK(ConfigStore::save(path, c, err));
    AppConfig back;
    CHECK(ConfigStore::load(path, back, err));
    CHECK(back.testerToken.empty());
    CHECK(back.testerAppToken.empty());
    CHECK(back.webPasswordRecord.empty());
    CHECK(!fs::exists(vault::sidecarPath(path)));   // nothing left to protect
}

// RULE: the settings export still leaves them out (and so does a file that is exported straight from the folder).
void testExportHasNone() {
    const fs::path d = freshDir("export");
    const std::string path = (d / "config.json").string();
    std::string err;
    CHECK(ConfigStore::save(path, configWithSecrets(), err));
    cascade::core::sys::BundleFile f;
    f.name = "config.json";
    f.bytes = slurp(path);
    cascade::core::sys::ExportOptions opt;
    cascade::core::sys::ExportReport rep;
    const std::string bundle = cascade::core::sys::exportBundle({f}, opt, rep);
    CHECK(rep.error == cascade::core::sys::BundleError::None);
    CHECK(!bundle.empty());
    CHECK(!has(bundle, kTesterToken));
    CHECK(!has(bundle, kAppToken));
    CHECK(!has(bundle, "SALTSALTSALT"));
    // the bundle is base64 of the file's bytes: decode check through the importer
    const auto parsed = cascade::core::sys::parseBundle(bundle);
    CHECK(parsed.error == cascade::core::sys::BundleError::None);
    for (const auto& pf : parsed.files) {
        CHECK(!has(pf.bytes, kTesterToken));
        CHECK(!has(pf.bytes, kAppToken));
        CHECK(!has(pf.bytes, "SALTSALTSALT"));
    }
}

// RULE: a blob opens only under its own key (the entropy), is refused when damaged, and a damaged protected copy reads as EMPTY and says so;
// a value the store cannot open is never deleted from anywhere.
void testRefusals() {
    std::string blob, plain, why;
    CHECK(vault::protectValue("testerToken", "secret-value", blob, why));
    CHECK(!blob.empty());
    CHECK(!has(blob, "secret-value"));
    CHECK(vault::unprotectValue("testerToken", blob, plain, why));
    CHECK(plain == "secret-value");
    if (vault::hasProtectedStore()) {
        // THE STAND-IN FOR "ANOTHER USER CAN NOT READ IT": the blob is bound to the user's own key and to the entropy, so it does not open under another
        // key name. A second Windows user is UNVERIFIED (no second account is created or signed in on this PC).
        CHECK(!vault::unprotectValue("testerAppToken", blob, plain, why));
        CHECK(plain.empty());
        CHECK(!why.empty());
        std::string damaged = blob;
        damaged[damaged.size() - 3] = damaged[damaged.size() - 3] == '0' ? '1' : '0';
        CHECK(!vault::unprotectValue("testerToken", damaged, plain, why));
    }
    CHECK(!vault::unprotectValue("testerToken", "dpapi:zz", plain, why));
    CHECK(!vault::unprotectValue("testerToken", "nonsense", plain, why));

    const fs::path d = freshDir("refuse");
    const std::string path = (d / "config.json").string();
    std::string err;
    CHECK(ConfigStore::save(path, configWithSecrets(), err));
    spit(vault::sidecarPath(path), "{ this is not json");
    AppConfig back;
    CHECK(ConfigStore::load(path, back, err));
    CHECK(back.testerToken.empty());   // unreadable = empty, the program still starts
    // the settings file itself is unharmed
    CHECK(!slurp(path).empty());
    bool migrated = false;
    std::string note;
    const std::string merged = vault::mergeAfterRead(path, "[1,2]", migrated, note);
    CHECK(merged == "[1,2]");
    CHECK(!migrated);
    bool kept = false;
    CHECK(vault::splitForWrite(path, "not json", why, kept) == "not json");
    CHECK(!kept);
    CHECK(vault::splitForWrite(path, "{}", why, kept) == "{}");
}

// M3 (security review 1): a secret the protected store cannot OPEN reads as empty, and the next ordinary save must NOT erase the stored item.
// The stand-in for "Windows cannot decrypt it" is a blob that is well formed but is not one this user's store produced; it fails to open on every
// platform. A second Windows user and a profile copied to another PC are UNVERIFIED (no second account is made).
void testUnreadableSecretIsNeverErased() {
    const fs::path d = freshDir("unreadable");
    const std::string path = (d / "config.json").string();
    std::string err;
    CHECK(ConfigStore::save(path, configWithSecrets(), err));
    // the good sidecar, kept to put back
    const std::string good = slurp(vault::sidecarPath(path));
    json side = json::parse(good);
    const std::string goodBlob = side["items"]["webPasswordRecord"].get<std::string>();
    side["items"]["webPasswordRecord"] = "dpapi:00ff00ff00ff00ff00ff";   // stored, but this user's store cannot open it
    spit(vault::sidecarPath(path), side.dump(2));

    AppConfig back;
    CHECK(ConfigStore::load(path, back, err));
    CHECK(back.webPasswordRecord.empty());                      // reads as empty ...
    CHECK(back.testerToken == kTesterToken);                    // ... and the others still open
    CHECK(vault::wasUnreadableAt(path, "webPasswordRecord"));           // ... and the program is told
    CHECK(vault::wasUnreadable("webPasswordRecord"));
    CHECK(!vault::wasUnreadableAt(path, "testerToken"));

    // two ordinary saves of the settings as the program holds them (the web password still empty)
    CHECK(ConfigStore::save(path, back, err));
    CHECK(ConfigStore::save(path, back, err));
    json after = json::parse(slurp(vault::sidecarPath(path)));
    CHECK(after["items"].contains("webPasswordRecord"));        // THE ITEM IS STILL THERE
    CHECK(after["items"]["webPasswordRecord"] == "dpapi:00ff00ff00ff00ff00ff");
    CHECK(after["items"].contains("testerToken"));
    CHECK(vault::wasUnreadableAt(path, "webPasswordRecord"));           // saving does not make it readable

    // the Windows side is later fixed (the good blob is back): the password is there again, nothing was lost
    side["items"]["webPasswordRecord"] = goodBlob;
    spit(vault::sidecarPath(path), side.dump(2));
    AppConfig fixed;
    CHECK(ConfigStore::load(path, fixed, err));
    CHECK(fixed.webPasswordRecord == kRecord);
    CHECK(!vault::wasUnreadableAt(path, "webPasswordRecord"));

    // a NEW value for an unreadable item replaces it and clears the mark
    side["items"]["webPasswordRecord"] = "dpapi:00ff00ff00ff00ff00ff";
    spit(vault::sidecarPath(path), side.dump(2));
    AppConfig again;
    CHECK(ConfigStore::load(path, again, err));
    CHECK(vault::wasUnreadableAt(path, "webPasswordRecord"));
    again.webPasswordRecord = "pbkdf2-sha256$600000$NEWSALTNEWSALT$NEWHASHNEWHASHNEW";
    CHECK(ConfigStore::save(path, again, err));
    AppConfig third;
    CHECK(ConfigStore::load(path, third, err));
    CHECK(third.webPasswordRecord == again.webPasswordRecord);
    CHECK(!vault::wasUnreadableAt(path, "webPasswordRecord"));

    // a person who really clears a READABLE secret still clears it
    third.webPasswordRecord.clear();
    CHECK(ConfigStore::save(path, third, err));
    json cleared = json::parse(slurp(vault::sidecarPath(path)));
    CHECK(!cleared["items"].contains("webPasswordRecord"));
}

// M3: a damaged sidecar may have held any secret, so every secret the settings file keeps empty is unreadable (the web password fails closed).
void testDamagedSidecarMarksTheSecretsUnreadable() {
    const fs::path d = freshDir("damaged");
    const std::string path = (d / "config.json").string();
    std::string err;
    CHECK(ConfigStore::save(path, configWithSecrets(), err));
    spit(vault::sidecarPath(path), "{ this is not json");
    AppConfig back;
    CHECK(ConfigStore::load(path, back, err));
    CHECK(back.webPasswordRecord.empty());
    CHECK(vault::wasUnreadableAt(path, "webPasswordRecord"));
    // a sidecar that is simply absent (a program that never had a secret) is not a loss
    const fs::path d2 = freshDir("absent");
    const std::string path2 = (d2 / "config.json").string();
    AppConfig plain;
    CHECK(ConfigStore::save(path2, plain, err));
    AppConfig plainBack;
    CHECK(ConfigStore::load(path2, plainBack, err));
    CHECK(!vault::wasUnreadableAt(path2, "webPasswordRecord"));
    vault::forget(path);
    CHECK(!vault::wasUnreadableAt(path, "webPasswordRecord"));
}

// RULE: when the store refuses a value, it stays in the file (nothing is lost) and the writer says so. (Windows only: the refusal is forced with a
// sidecar path that cannot be written.)
void testStoreRefusalKeepsValue() {
    const fs::path d = freshDir("keep");
    const std::string path = (d / "config.json").string();
    fs::create_directories(vault::sidecarPath(path));   // a FOLDER squats on the sidecar's name: it cannot be written
    json doc;
    doc["testerToken"] = kTesterToken;
    std::string why;
    bool kept = false;
    const std::string out = vault::splitForWrite(path, doc.dump(), why, kept);
    CHECK(kept);
    CHECK(!why.empty());
    CHECK(has(out, kTesterToken));   // still in the text that will be written
}

// RULE: a reset or an import leaves no protected copy behind to be read back into the new file.
void testResetAndImportForget() {
    const fs::path d = freshDir("forget");
    const std::string path = (d / "config.json").string();
    std::string err;
    CHECK(ConfigStore::save(path, configWithSecrets(), err));
    CHECK(fs::exists(vault::sidecarPath(path)));
    vault::forget(path);
    CHECK(!fs::exists(vault::sidecarPath(path)));
    AppConfig back;
    CHECK(ConfigStore::load(path, back, err));
    CHECK(back.testerToken.empty());
}

// RULE: two threads writing at once leave a file with no plain secret and a sidecar that opens.
void testConcurrentWrites() {
    const fs::path d = freshDir("conc");
    const std::string path = (d / "config.json").string();
    std::atomic<int> okCount{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t) {
        ts.emplace_back([&, t] {
            for (int i = 0; i < 5; ++i) {
                AppConfig c = configWithSecrets();
                c.webPort = 3000 + t * 10 + i;
                std::string e;
                // Only one writer runs at a time in the product (ConfigWriter); the temp file name is per process, so the writers here take turns.
                static std::atomic_flag gate = ATOMIC_FLAG_INIT;
                while (gate.test_and_set()) { std::this_thread::yield(); }
                if (ConfigStore::save(path, c, e)) { ++okCount; }
                gate.clear();
            }
        });
    }
    for (auto& t : ts) { t.join(); }
    CHECK(okCount == 20);
    CHECK(!has(everythingIn(d), kTesterToken));
    AppConfig back;
    std::string err;
    CHECK(ConfigStore::load(path, back, err));
    CHECK(back.testerToken == kTesterToken);
}

// RULE: a plugin's PASSWORD field (declared) and any plugin field whose key names a secret (an older build's value is found before a plugin loads)
// are kept in the protected store; an ordinary plugin field is not.
void testPluginSecrets() {
    vault::forgetDeclaredPluginSecretsForTest();
    vault::declarePluginSecret("alert-mail", "smtpLogin");   // declared a PASSWORD field by the plugin, though its name says nothing
    const fs::path d = freshDir("plugin");
    const std::string path = (d / "config.json").string();
    AppConfig c;
    c.pluginSettings["alert-mail"]["smtpLogin"] = "mail-secret-1";
    c.pluginSettings["alert-mail"]["server"] = "smtp.example.invalid";
    c.pluginSettings["influx-log"]["influxToken"] = "influx-secret-2";
    c.pluginSettings["influx-log"]["bucket"] = "radio";
    std::string err;
    CHECK(ConfigStore::save(path, c, err));
    const std::string all = everythingIn(d);
    CHECK(!has(all, "mail-secret-1"));
    CHECK(!has(all, "influx-secret-2"));
    CHECK(has(slurp(path), "smtp.example.invalid"));   // an ordinary field stays readable in the file
    CHECK(has(slurp(path), "radio"));
    AppConfig back;
    CHECK(ConfigStore::load(path, back, err));
    CHECK(back.pluginSettings["alert-mail"]["smtpLogin"] == "mail-secret-1");
    CHECK(back.pluginSettings["influx-log"]["influxToken"] == "influx-secret-2");
    CHECK(back.pluginSettings["alert-mail"]["server"] == "smtp.example.invalid");
    // the field the plugin declared is kept even in a fresh process that has not loaded the plugin yet only if its name says so: here it is declared
    CHECK(vault::isPluginSecret("alert-mail", "smtpLogin"));
    CHECK(!vault::isPluginSecret("alert-mail", "server"));
    CHECK(vault::isPluginSecret("any", "MyApiKey"));
    // removing the plugin's settings removes the protected copies
    c.pluginSettings.erase("influx-log");
    CHECK(ConfigStore::save(path, c, err));
    AppConfig again;
    CHECK(ConfigStore::load(path, again, err));
    CHECK(again.pluginSettings.count("influx-log") == 0);
    CHECK(again.pluginSettings["alert-mail"]["smtpLogin"] == "mail-secret-1");
    vault::forgetDeclaredPluginSecretsForTest();
}

}  // namespace

int main() {
    g_base = fs::temp_directory_path() / ("foxsdr-test-secret-vault-" + std::to_string(TEST_GETPID()));
    std::error_code ec;
    fs::remove_all(g_base, ec);
    fs::create_directories(g_base);
    testWriteKeepsSecretsOutOfTheFile();
#ifndef _WIN32
    testSidecarIsOwnerOnlyFromCreation();
#endif
    testNoPlainCopyAfterManySaves();
    testMigration();
    testClear();
    testExportHasNone();
    testRefusals();
    testUnreadableSecretIsNeverErased();
    testDamagedSidecarMarksTheSecretsUnreadable();
    testStoreRefusalKeepsValue();
    testResetAndImportForget();
    testConcurrentWrites();
    testPluginSecrets();
    fs::remove_all(g_base, ec);
    return testSummary("test_secret_vault");
}
