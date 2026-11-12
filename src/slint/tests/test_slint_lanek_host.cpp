// Screen lane K's pure rules (src/slint/lanek_logic.hpp; docs/multi-radio-records/screen-k.md): the row builders, the saved record, and the settings keeper over the sysmodels lane's
// bundle, profile and reset models. Scratch folders only (the PID in the name, a folder NAMED foxsdr because the reset's guard demands it); the real settings folder is never named.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <cmath>
#include <map>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
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

#include "lanek_logic.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using namespace foxsdr::slintlanek;
using nlohmann::json;

namespace {

fs::path g_base;

std::string slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()); }
void spit(const fs::path& p, const std::string& s) { fs::create_directories(p.parent_path()); std::ofstream f(p, std::ios::binary | std::ios::trunc); f << s; }

const char* kConfigA = R"({"schemaVersion":1,"centerHz":98000000,"volume":0.5,"webPasswordRecord":"pbkdf2$1$SECRETSALT$SECRETHASH","telemetryInstallId":"fedcba9876543210fedcba9876543210"})";
const char* kConfigB = R"({"schemaVersion":1,"centerHz":145500000,"volume":0.25})";

// a fresh scratch tree: <base>/<tag>/foxsdr (settings), <tag>/data/foxsdr, exports, backups, profiles
KeeperDirs freshDirs(const std::string& tag) {
    KeeperDirs d;
    const fs::path root = g_base / tag;
    std::error_code ec;
    fs::remove_all(root, ec);
    d.settings = root / "foxsdr";
    d.data = root / "local" / "foxsdr";
    d.exports = root / "exports";
    d.backups = d.settings / "backups";
    d.profiles = d.settings / "profiles";
    fs::create_directories(d.settings);
    fs::create_directories(d.data);
    return d;
}

void rows() {
    CHECK(rHeading("H").kind == kHeading);
    CHECK(rNote("n", kWarn).tone == kWarn);
    const Row k = rKey("id", "Label", false, "tip", true);
    CHECK(k.kind == kKey && k.id == "id" && !k.enabled && k.on && k.note == "tip");
    const Row c = rChoice("c", "L", {"a", "b", "c"}, 2);
    CHECK(c.options.size() == 3 && c.pick == 2);
    CHECK(rSlider("s", "L", 3.0f, "x").level == 1.0f);     // clamped
    CHECK(rSlider("s", "L", -3.0f, "x").level == 0.0f);
    CHECK(rBar("L", 0.5f, "50%").kind == kBar);
    // a signature moves when anything the screen shows moves, and only then
    std::vector<Row> a = {rKey("x", "X"), rTick("t", "T", false)};
    std::vector<Row> b = a;
    CHECK(signatureOf(a) == signatureOf(b));
    b[1].on = true;
    CHECK(signatureOf(a) != signatureOf(b));
    b = a; b[0].enabled = false;
    CHECK(signatureOf(a) != signatureOf(b));
    b = a; b.push_back(rNote("more"));
    CHECK(signatureOf(a) != signatureOf(b));
    CHECK(!wizardStrings().empty());
}

void record() {
    Record r;
    CHECK(!r.loadText(""));
    CHECK(!r.loadText("not json"));
    CHECK(!r.loadText("[1,2]"));
    CHECK(r.getBool("a", true) == true);                    // missing: the default
    CHECK(r.setBool("a", false));
    CHECK(!r.setBool("a", false));                          // unchanged: not dirty again
    CHECK(r.dirty());
    CHECK(r.setInt("n", 7));
    CHECK(r.setNum("x", 1.5));
    CHECK(!r.setNum("x", std::nan("")));                    // a non-finite number is refused
    CHECK(r.setStr("s", "hello"));
    CHECK(r.getBool("a", true) == false);
    CHECK(r.getInt("n", 0, 0, 100) == 7);
    CHECK(r.getInt("n", 0, 0, 5) == 5);                     // clamped
    CHECK(r.getNum("x", 0, 0, 10) == 1.5);
    CHECK(r.getStr("s") == "hello");
    CHECK(r.getInt("s", 3, 0, 10) == 3);                    // wrong type: the default
    CHECK(r.getBool("n", false) == false);
    CHECK(!r.setStr("", "x"));                              // an empty key
    CHECK(!r.setStr(std::string(65, 'k'), "x"));            // a key over 64 characters
    r.setMap("m", {{"p", "1"}, {"q", "2"}});
    Record r2;
    CHECK(r2.loadText(r.toText()));
    CHECK(r2.getBool("a", true) == false && r2.getInt("n", 0, 0, 100) == 7 && r2.getStr("s") == "hello");
    CHECK(r2.getMap("m").size() == 2 && r2.getMap("m").at("q") == "2");
    CHECK(!r2.dirty());                                     // a loaded record is clean
    // hostile input: a huge text is refused, nested junk is bounded, a long string is cut
    Record r3;
    CHECK(!r3.loadText(std::string(300 * 1024, ' ')));
    Record r4;
    r4.setStr("long", std::string(5000, 'z'));
    CHECK(r4.getStr("long").size() == 2048);
    CHECK(r4.getStr("long", "", 10).size() == 10);
    json big = json::object();
    for (int i = 0; i < 2000; ++i) { big["k" + std::to_string(i)] = i; }
    Record r5;
    CHECK(r5.loadText(big.dump(-1, ' ', false, json::error_handler_t::replace)));
    int kept = 0;
    for (int i = 0; i < 2000; ++i) { if (r5.getInt("k" + std::to_string(i), -1, -1, 5000) >= 0) { ++kept; } }
    CHECK(kept <= 512 && kept > 0);                         // the key count is bounded
}

void exportAndBackup() {
    const KeeperDirs d = freshDirs("export");
    SettingsKeeper k(d);
    std::vector<cascade::core::sys::BundleFile> files;
    std::string err;
    CHECK(!k.readCurrent(files, err));                      // nothing there yet
    CHECK(!err.empty());
    ExportOut none = k.exportTo(d.exports, "20261112-100000", "0.99.108");
    CHECK(!none.ok && !none.error.empty());
    spit(d.settings / "config.json", kConfigA);
    spit(d.settings / "bookmarks.json", R"({"bookmarks":[]})");
    const ExportOut o = k.exportTo(d.exports, "20261112-100000", "0.99.108");
    CHECK(o.ok);
    CHECK(o.stripped >= 2);                                 // the password record and the install id
    const std::string bundle = slurp(o.path);
    CHECK(bundle.find("SECRETHASH") == std::string::npos);  // never in the file ...
    CHECK(bundle.find("fedcba9876543210") == std::string::npos);
    // (the bundle holds base64 of each file: decode by importing it)
    const ImportPreview pv = k.previewImport(o.path);
    CHECK(pv.ok);
    bool sawSecret = false;
    for (const auto& f : pv.res.files) { if (f.bytes.find("SECRETHASH") != std::string::npos) { sawSecret = true; } }
    CHECK(!sawSecret);
    CHECK(pv.replace + pv.identical + pv.added == 2 && pv.replace >= 1 && pv.added == 0);   // both files are on disk; config.json differs by the stripped keys (the bundle also re-writes each file's JSON, so a byte-for-byte match is not expected)
    const auto names = k.listBundles(d.exports);
    CHECK(names.size() == 1 && names[0] == "foxsdr-settings-20261112-100000.foxsdr-bundle");
    // backups: pruned to the newest N, by their own names only
    spit(d.backups / "not-ours.txt", "keep me");
    for (int i = 0; i < 5; ++i) {
        char stamp[32];
        std::snprintf(stamp, sizeof(stamp), "20261112-10%02d00", 10 + i);
        CHECK(k.backupNow(stamp, "0.99.108", 3).ok);
    }
    const auto kept = k.listBundles(d.backups);
    CHECK(kept.size() == 3);
    CHECK(kept[0].find("101400") != std::string::npos);      // the newest survives
    CHECK(fs::exists(d.backups / "not-ours.txt"));
    CHECK(k.backupNow("20261112-102000", "v", 0).ok);        // keep 0 is clamped to 1
    CHECK(k.listBundles(d.backups).size() == 1);
}

void importAndPending() {
    const KeeperDirs d = freshDirs("import");
    SettingsKeeper k(d);
    spit(d.settings / "config.json", kConfigA);
    const ExportOut o = k.exportTo(d.exports, "20261112-110000", "v");
    CHECK(o.ok);
    // change the live settings, then plan the import of the exported file
    spit(d.settings / "config.json", kConfigB);
    ImportPreview pv = k.previewImport(o.path);
    CHECK(pv.ok && pv.replace == 1);
    CHECK(!pv.plan.confirmToken.empty());
    Pending p;
    p.op = Op::Import; p.staged = pv.res; p.token = pv.plan.confirmToken; p.stamp = "20261112-110500"; p.what = "import";
    std::string detail;
    // the engine's last save changes the file between the plan and the apply: the apply plans again, so it still goes through
    spit(d.settings / "config.json", R"({"schemaVersion":1,"centerHz":123456789,"volume":0.75})");
    CHECK(k.apply(p, detail));
    const json now = json::parse(slurp(d.settings / "config.json"));
    CHECK(now["centerHz"] == 98000000);                      // the exported value came back
    CHECK(!now.contains("webPasswordRecord"));               // and the secret was never in it
    bool hadBefore = false;
    for (auto it = fs::directory_iterator(d.settings); it != fs::directory_iterator(); ++it) { if (it->path().filename().string().find("before-import") != std::string::npos) { hadBefore = true; } }
    CHECK(hadBefore);                                        // the old file is kept
    // files that are not bundles
    spit(g_base / "junk.foxsdr-bundle", "this is not json");
    CHECK(!k.previewImport(g_base / "junk.foxsdr-bundle").ok);
    CHECK(!k.previewImport(g_base / "missing.foxsdr-bundle").ok);
    spit(g_base / "empty.foxsdr-bundle", "");
    CHECK(!k.previewImport(g_base / "empty.foxsdr-bundle").ok);
    spit(g_base / "notabundle.foxsdr-bundle", R"({"format":"something-else"})");
    CHECK(!k.previewImport(g_base / "notabundle.foxsdr-bundle").ok);
    CHECK(k.apply(Pending{}, detail));                       // nothing pending: nothing to do
}

void profiles() {
    const KeeperDirs d = freshDirs("profiles");
    SettingsKeeper k(d);
    spit(d.settings / "config.json", kConfigA);
    std::string err;
    CHECK(k.profileNames().empty());
    CHECK(!k.saveProfile("", true, "v", err));
    CHECK(!k.saveProfile("../evil", true, "v", err));        // a name that is a path
    CHECK(k.saveProfile("Home desk", true, "v", err));
    spit(d.settings / "config.json", kConfigB);
    CHECK(k.saveProfile("Portable", true, "v", err));
    CHECK(!k.saveProfile("Portable", false, "v", err));      // no overwrite when not asked
    auto names = k.profileNames();
    CHECK(names.size() == 2);
    // switch to "Home desk": the file is staged, then applied at exit; this computer's secret stays
    spit(d.settings / "config.json", R"({"schemaVersion":1,"centerHz":1,"webPasswordRecord":"KEEPME"})");
    const auto sw = k.planSwitch("Home desk");
    CHECK(sw.error.empty());
    Pending p;
    p.op = Op::Switch; p.sw = sw; p.profile = "Home desk"; p.token = sw.plan.confirmToken; p.stamp = "20261112-120000"; p.what = "switch";
    spit(d.settings / "config.json", R"({"schemaVersion":1,"centerHz":2,"webPasswordRecord":"KEEPME-AFTER-SAVE"})");   // the engine's last save changed the file after the plan
    std::string detail;
    CHECK(k.apply(p, detail));
    const json now = json::parse(slurp(d.settings / "config.json"));
    CHECK(now["centerHz"] == 98000000);
    CHECK(now.contains("webPasswordRecord") && now["webPasswordRecord"] == "KEEPME-AFTER-SAVE");   // this computer's own value, as the last save left it
    CHECK(!k.planSwitch("No such").error.empty());
    CHECK(k.deleteProfile("Portable", err));
    CHECK(!k.deleteProfile("Portable", err));
    CHECK(k.profileNames().size() == 1);
    // sync with a shared folder: push, then pull, then a conflict is reported and left alone
    const fs::path shared = g_base / "profiles" / "shared";
    fs::create_directories(shared);
    std::map<std::string, std::string> base;
    SyncReport r = k.sync(shared, base);
    CHECK(r.ok && r.pushed == 1 && r.pulled == 0);
    r = k.sync(shared, base);
    CHECK(r.ok && r.pushed == 0 && r.pulled == 0 && r.same == 1);
    CHECK(!k.sync(g_base / "no-such-folder", base).ok);
    // both sides change: a conflict, neither is overwritten
    spit(d.settings / "config.json", kConfigB);
    CHECK(k.saveProfile("Home desk", true, "v2", err));
    spit(d.settings / "config.json", kConfigA);
    // make the shared copy differ by saving another profile there directly
    SettingsKeeper other(KeeperDirs{d.settings, d.data, d.exports, d.backups, shared});
    CHECK(other.saveProfile("Home desk", true, "v3", err));
    const std::string localBefore = slurp(d.profiles / "Home desk.foxsdr-profile");
    const std::string sharedBefore = slurp(shared / "Home desk.foxsdr-profile");
    r = k.sync(shared, base);
    CHECK(r.ok && r.conflicts == 1 && r.conflictNames.size() == 1);
    CHECK(slurp(d.profiles / "Home desk.foxsdr-profile") == localBefore && slurp(shared / "Home desk.foxsdr-profile") == sharedBefore);
}

void reset() {
    const KeeperDirs d = freshDirs("reset");
    SettingsKeeper k(d);
    spit(d.settings / "config.json", kConfigA);
    spit(d.settings / "bookmarks.json", R"({"bookmarks":[]})");
    spit(d.settings / "lanek.json", "{}");                    // ours? no: left alone
    spit(d.settings / "unrelated.txt", "x");
    spit(d.data / "plugins" / "p.dll", "x");
    spit(d.data / "logs" / "l.txt", "x");
    cascade::core::sys::ResetOptions o;
    auto plan = k.planReset(o);
    CHECK(plan.error.empty());
    CHECK(plan.items.size() == 2);                            // config.json and bookmarks.json only
    CHECK(plan.leftAlone >= 2);
    o.includePlugins = true;
    plan = k.planReset(o);
    CHECK(plan.items.size() == 3);
    Pending p;
    p.op = Op::Reset; p.reset = o; p.token = plan.confirmToken; p.stamp = "20261112-130000"; p.what = "reset";
    std::string detail;
    spit(d.settings / "config.json", R"({"schemaVersion":1,"centerHz":5})");           // the last save changed it after the plan: still planned again and done
    CHECK(k.apply(p, detail));
    CHECK(!fs::exists(d.settings / "config.json") && !fs::exists(d.settings / "bookmarks.json"));
    CHECK(fs::exists(d.settings / "unrelated.txt") && fs::exists(d.settings / "lanek.json"));
    CHECK(fs::exists(d.data / "logs" / "l.txt") && !fs::exists(d.data / "plugins"));
    const auto stamps = k.resetStamps();
    CHECK(stamps.size() == 1 && stamps[0] == "20261112-130000");
    Pending u;
    u.op = Op::UndoReset; u.undoStamp = stamps[0]; u.what = "undo";
    CHECK(k.apply(u, detail));
    CHECK(slurp(d.settings / "config.json") == R"({"schemaVersion":1,"centerHz":5})" && fs::exists(d.data / "plugins" / "p.dll"));
    CHECK(k.resetStamps().empty());
    // the folder guard: a settings folder that is not named foxsdr is refused
    KeeperDirs wrongName = d;
    wrongName.settings = g_base / "reset" / "notours";
    fs::create_directories(wrongName.settings);
    CHECK(!SettingsKeeper(wrongName).planReset({}).error.empty());
}

void stamp() {
    const std::string s = stampOf(1700000000);
    CHECK(s.size() == 15 && s[8] == '-');
    for (std::size_t i = 0; i < s.size(); ++i) { if (i != 8) { CHECK(s[i] >= '0' && s[i] <= '9'); } }
    CHECK(cascade::core::sys::isBackupFileName(cascade::core::sys::backupFileName(s)));   // the models accept it
}

}  // namespace

int main() {
    g_base = fs::temp_directory_path() / ("sk-lanek-" + std::to_string(TEST_GETPID()));
    std::error_code ec;
    fs::remove_all(g_base, ec);
    fs::create_directories(g_base);
    rows();
    record();
    exportAndBackup();
    importAndPending();
    profiles();
    reset();
    stamp();
    fs::remove_all(g_base, ec);
    return testSummary("test_slint_lanek_host");
}
