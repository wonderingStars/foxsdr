// test_engine_api_wire_c_sys.cpp - the SYSTEM operations of wiring lane C in the real engine (src/engine/wire_c_sys.{hpp,cpp}, engine_wire_c.cpp; models core/sys_bundle,
// sys_reset, sys_first_run), over a scratch settings folder named foxsdr.
//
//   Y1  export: a bundle with config, bookmarks and markers; NO secret and no install id in it; refusals (no file named, nothing to export)
//   Y2  import: plan (what is New / Replace / Identical, a token), a wrong or stale token and a bad file refused; the commit only STAGES (the running engine would write config.json
//       over it), cancel withdraws it; the next start (applyPendingAtBoot) puts the files in place, keeps the old ones as .before-import-<stamp>, clears the stage, and is a no-op
//       the second time; a run that only prints (--version) never carries it out
//   Y3  backup: a bundle in <settings>/backups, only the newest N kept
//   Y4  reset: plan (our files, how many others are left alone), token, stage; the next start moves the files into reset-<stamp> (nothing deleted, the lane's own wirec.json included) and
//       leaves what is not ours; a folder not named foxsdr is refused
//   Y5  first-run wizard: needed until done; a step without its choice is refused; the chosen set of stations is added; usage reports changes ONLY if the person chose it; the done marker;
//       skip from the start; a choice that does not fit is refused
//   Y6  a REMOTE session cannot send any of them
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <thread>

#include "core/config.hpp"
#include "engine/wire_c_portable.hpp"
#include "engine/wire_c_sys.hpp"
#include "wire_c_rig.hpp"

using namespace wirec_test;

namespace {

void writeText(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
}

FoxCommand textCmd(std::uint32_t op, const std::string& text, std::int64_t a = 0, std::int64_t b = 0) {
    FoxCommand c = testing::cmd(op, 0.0, a, text.c_str());
    c.ival[1] = b;
    return c;
}

const char* kConfig = "{\"volume\":0.5,\"sourceKind\":\"rtl\",\"webPassword\":\"SECRET-DO-NOT-EXPORT\",\"installId\":\"abcdef0123456789\"}";
const char* kBookmarks = "{\"version\":1,\"bookmarks\":[]}";

fs::path settingsDir(const char* tag) {
    const fs::path root = scratch(tag);
    const fs::path d = root / "foxsdr";   // the settings folder must be named foxsdr (the reset's guard)
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
}

// Y1 + Y2 + Y3
void testExportImportBackup() {
    const fs::path dir = settingsDir("sysA");
    writeText(dir / "config.json", kConfig);
    writeText(dir / "bookmarks.json", kBookmarks);
    Rig r(dir);
    const fs::path bundle = dir.parent_path() / "out.foxsdr-bundle";

    CHECK(r.status(textCmd(FOXAPP_OP_SYS_EXPORT, "")) == FOXAPI_BAD_ARGUMENT);
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_EXPORT, bundle.string())) == FOXAPI_OK);
    const std::string text = readAll(bundle);
    CHECK(!text.empty());
    CHECK(text.find("SECRET-DO-NOT-EXPORT") == std::string::npos);
    CHECK(text.find("abcdef0123456789") == std::string::npos);
    CHECK(text.find("config.json") != std::string::npos && text.find("bookmarks.json") != std::string::npos);
    CHECK(r.fact("sys.export.last").find("exported") != std::string::npos);

    // plan: the same files are present, so each is Replace or Identical; a token comes with it
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_IMPORT_PLAN, (dir.parent_path() / "none.foxsdr-bundle").string())) == FOXAPI_BAD_ARGUMENT);
    writeText(dir.parent_path() / "garbage.foxsdr-bundle", "this is not a bundle");
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_IMPORT_PLAN, (dir.parent_path() / "garbage.foxsdr-bundle").string())) == FOXAPI_BAD_ARGUMENT);
    CHECK(!r.fact("sys.error").empty());
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_IMPORT_PLAN, bundle.string())) == FOXAPI_OK);
    const std::string plan = r.fact("sys.import.plan");
    const std::string token = r.fact("sys.import.token");
    CHECK(plan.find("config.json\t") != std::string::npos);
    CHECK(token.rfind("IMPORT-", 0) == 0u);
    // a wrong token, then the right one: STAGED only
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_IMPORT_COMMIT, "IMPORT-00000000")) == FOXAPI_BAD_ARGUMENT);
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_IMPORT_COMMIT, "")) == FOXAPI_BAD_ARGUMENT);
    // make the bundle differ from the folder first, so the import has something to Replace
    writeText(dir / "config.json", "{\"volume\":0.9,\"sourceKind\":\"airspy\"}");
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_IMPORT_COMMIT, token)) == FOXAPI_BAD_ARGUMENT);   // the folder changed since the plan: the token is stale
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_IMPORT_PLAN, bundle.string())) == FOXAPI_OK);
    const std::string token2 = r.fact("sys.import.token");
    CHECK(r.fact("sys.import.plan").find("config.json\tReplace") != std::string::npos);
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_IMPORT_COMMIT, token2)) == FOXAPI_OK);
    CHECK(r.fact("sys.pending") == "import");
    CHECK(readAll(dir / "config.json") == "{\"volume\":0.9,\"sourceKind\":\"airspy\"}");   // NOT replaced now: staged for the next start
    CHECK(fs::exists(dir / "wirec-pending.json") && fs::exists(dir / "wirec-pending" / "config.json"));
    // cancel withdraws it
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_PENDING_CANCEL, "")) == FOXAPI_OK);
    CHECK(r.fact("sys.pending").empty());
    CHECK(!fs::exists(dir / "wirec-pending.json") && !fs::exists(dir / "wirec-pending"));
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_PENDING_CANCEL, "")) == FOXAPI_NO_CHANGE);
    // stage again and let "the next start" carry it out
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_IMPORT_PLAN, bundle.string())) == FOXAPI_OK);
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_IMPORT_COMMIT, r.fact("sys.import.token"))) == FOXAPI_OK);
    bool ok = false;
    const std::string said = cascade::engine::wirec::applyPendingAtBoot(dir, dir.parent_path() / "data" / "foxsdr", ok);
    CHECK(ok);
    CHECK(said.find("settings imported") != std::string::npos);
    CHECK(readAll(dir / "config.json").find("SECRET") == std::string::npos);
    CHECK(readAll(dir / "config.json").find("\"volume\":0.5") != std::string::npos || readAll(dir / "config.json").find("0.5") != std::string::npos);   // the bundle's own value
    bool keptOld = false;
    for (const auto& e : fs::directory_iterator(dir)) {
        const std::string n = e.path().filename().string();
        if (n.rfind("config.json.before-import-", 0) == 0) { keptOld = readAll(e.path()) == "{\"volume\":0.9,\"sourceKind\":\"airspy\"}"; }
    }
    CHECK(keptOld);   // the file it replaced is kept, not deleted
    CHECK(!fs::exists(dir / "wirec-pending.json") && !fs::exists(dir / "wirec-pending"));
    CHECK(cascade::engine::wirec::applyPendingAtBoot(dir, dir.parent_path() / "data" / "foxsdr", ok).empty());   // a second start does nothing

    // Y3 backup: the newest N kept
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_BACKUP, "", 1)) == FOXAPI_OK);
    CHECK(!r.fact("sys.backup.last").empty());
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));   // (a backup is named by the second)
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_BACKUP, "", 1)) == FOXAPI_OK);
    int backups = 0;
    for (const auto& e : fs::directory_iterator(dir / "backups")) { (void)e; ++backups; }
    CHECK(backups == 1);
    CHECK(readAll(dir / "backups" / r.fact("sys.backup.last")).find("SECRET") == std::string::npos);
}

// Y1: nothing to export
void testExportNothing() {
    const fs::path dir = settingsDir("sysB");
    Rig r(dir);
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_EXPORT, (dir.parent_path() / "o.foxsdr-bundle").string())) == FOXAPI_BAD_ARGUMENT);
    CHECK(r.fact("sys.error").find("nothing to export") != std::string::npos);
    CHECK(!fs::exists(dir.parent_path() / "o.foxsdr-bundle"));
}

// Y4
void testReset() {
    const fs::path dir = settingsDir("sysC");
    writeText(dir / "config.json", kConfig);
    writeText(dir / "bookmarks.json", kBookmarks);
    writeText(dir / "notes-of-the-user.txt", "not ours");
    Rig r(dir);
    CHECK(r.status(setting("access.units", "metric")) == FOXAPI_OK);   // creates wirec.json
    CHECK(fs::exists(dir / "wirec.json"));
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_RESET_PLAN, "", 0, 0)) == FOXAPI_OK);
    const std::string plan = r.fact("sys.reset.plan");
    CHECK(plan.find("config.json") != std::string::npos && plan.find("bookmarks.json") != std::string::npos);
    CHECK(plan.find("notes-of-the-user") == std::string::npos);
    CHECK(plan.find("other file(s) in the folder are left alone") != std::string::npos);
    const std::string token = r.fact("sys.reset.token");
    CHECK(token.rfind("RESET-", 0) == 0u);
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_RESET_COMMIT, "RESET-00000000")) == FOXAPI_BAD_ARGUMENT);
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_RESET_COMMIT, token)) == FOXAPI_OK);
    CHECK(r.fact("sys.pending") == "reset");
    CHECK(fs::exists(dir / "config.json"));   // staged, not done: the running engine would write it again at its end
    bool ok = false;
    const std::string said = cascade::engine::wirec::applyPendingAtBoot(dir, dir.parent_path() / "wcdata" / "foxsdr", ok);
    CHECK(ok);
    CHECK(said.find("settings reset") != std::string::npos);
    CHECK(!fs::exists(dir / "config.json") && !fs::exists(dir / "bookmarks.json") && !fs::exists(dir / "wirec.json"));
    CHECK(fs::exists(dir / "notes-of-the-user.txt"));   // what is not ours stays
    bool moved = false;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.path().filename().string().rfind("reset-", 0) == 0 && fs::exists(e.path() / "config.json") && fs::exists(e.path() / "wirec.json")) { moved = readAll(e.path() / "config.json") == kConfig; }
    }
    CHECK(moved);   // moved, not deleted
    // a folder that is not named foxsdr is refused
    {
        const fs::path other = scratch("sysD") / "elsewhere";
        std::error_code ec;
        fs::create_directories(other, ec);
        writeText(other / "config.json", kConfig);
        Rig r2(other);
        CHECK(r2.status(textCmd(FOXAPP_OP_SYS_RESET_PLAN, "", 0, 0)) == FOXAPI_BAD_ARGUMENT);
        CHECK(r2.fact("sys.error").find("named foxsdr") != std::string::npos);
        CHECK(fs::exists(other / "config.json"));
    }
}

// the program's own start: STEP 2c
void testBootStep() {
    const fs::path root = scratch("sysE");
    const fs::path appdata = root / "appdata";
    const fs::path dir = appdata / "foxsdr";
    std::error_code ec;
    fs::create_directories(dir, ec);
    engine_api_test::setEnv("APPDATA", appdata.string());
    engine_api_test::setEnv("LOCALAPPDATA", (root / "local").string());
    writeText(dir / "config.json", "{\"volume\":0.9}");
    const std::string stagedBundle = "{\"volume\":0.1}";
    writeText(dir / "wirec-pending" / "config.json", stagedBundle);
    writeText(dir / "wirec-pending.json", "{\"action\":\"import\",\"stamp\":\"20261112-120000\",\"files\":[\"config.json\",\"../evil.json\"]}");
    // a run that only prints never carries it out
    char a0[] = "foxsdr-engine";
    char a1[] = "--version";
    char* argvTool[] = {a0, a1, nullptr};
    cascade::engine::applyPendingSystemActionsToProcess(2, argvTool);
    CHECK(readAll(dir / "config.json") == "{\"volume\":0.9}");
    CHECK(fs::exists(dir / "wirec-pending.json"));
    // a real start does, and only the three names a bundle may carry (the traversal name is ignored)
    char* argvReal[] = {a0, nullptr};
    cascade::engine::applyPendingSystemActionsToProcess(1, argvReal);
    CHECK(readAll(dir / "config.json") == stagedBundle);
    CHECK(!fs::exists(appdata / "evil.json") && !fs::exists(dir.parent_path().parent_path() / "evil.json"));
    CHECK(!fs::exists(dir / "wirec-pending.json"));
    CHECK(readAll(dir / "wirec-last-boot.txt").find("settings imported") != std::string::npos);
    // and the engine publishes what the start did
    Rig r(dir);
    CHECK(r.fact("sys.lastboot").find("settings imported") != std::string::npos);
}

int bookmarkCount(Rig& r) { return api()->read_list(r.local->raw(), FOXAPI_LIST_BOOKMARKS, 0, nullptr); }

// Y5
void testFirstRun() {
    const fs::path dir = settingsDir("sysF");
    Rig r(dir);
    CHECK(r.fact("sys.firstrun.needed") == "1");   // no config.json, no marker
    CHECK(r.fact("sys.firstrun.step") == "welcome");
    CHECK(r.fact("sys.firstrun.radios").find("RTL-SDR dongle") != std::string::npos);
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_FIRSTRUN, "next")) == FOXAPI_OK);
    CHECK(r.fact("sys.firstrun.step") == "radio");
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_FIRSTRUN, "next")) == FOXAPI_BAD_ARGUMENT);   // no radio chosen yet
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_FIRSTRUN, "radio:99")) == FOXAPI_BAD_ARGUMENT);
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_FIRSTRUN, "bogus")) == FOXAPI_BAD_ARGUMENT);
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_FIRSTRUN, "radio:1")) == FOXAPI_OK);   // the second choice (an RTL-SDR dongle)
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_FIRSTRUN, "next")) == FOXAPI_OK);
    CHECK(r.fact("sys.firstrun.step") == "preset");
    const std::string presets = r.fact("sys.firstrun.presets");
    CHECK(!presets.empty());
    const std::string firstId = presets.substr(0, presets.find('\t'));
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_FIRSTRUN, "preset:not-a-set")) == FOXAPI_BAD_ARGUMENT);
    const int before = bookmarkCount(r);
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_FIRSTRUN, "preset:" + firstId)) == FOXAPI_OK);
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_FIRSTRUN, "next")) == FOXAPI_OK);
    CHECK(r.fact("sys.firstrun.step") == "privacy");
    CHECK(r.fact("sys.firstrun.done") == "0");
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_FIRSTRUN, "telemetry:0")) == FOXAPI_OK);
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_FIRSTRUN, "next")) == FOXAPI_OK);
    CHECK(r.fact("sys.firstrun.step") == "done" && r.fact("sys.firstrun.done") == "1");
    CHECK(bookmarkCount(r) > before);   // the chosen set of stations was added
    CHECK(fs::exists(dir / cascade::core::sys::kFirstRunMarker));
    CHECK(r.fact("sys.firstrun.needed") == "0");
}

void testFirstRunSkip() {
    const fs::path dir = settingsDir("sysG");
    Rig r(dir);
    const int before = bookmarkCount(r);
    CHECK(r.status(textCmd(FOXAPP_OP_SYS_FIRSTRUN, "skip")) == FOXAPI_OK);
    CHECK(r.fact("sys.firstrun.done") == "1");
    CHECK(bookmarkCount(r) == before);   // a skip adds nothing
    CHECK(fs::exists(dir / cascade::core::sys::kFirstRunMarker));
    CHECK(r.fact("sys.firstrun.needed") == "0");
}

// Y6
void testRemote() {
    const fs::path dir = settingsDir("sysH");
    writeText(dir / "config.json", kConfig);
    Rig r(dir);
    testing::Session remote(api(), r.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS, "s3cret");
    CHECK(remote.ok());
    for (const std::uint32_t op : {FOXAPP_OP_SYS_FIRSTRUN, FOXAPP_OP_SYS_EXPORT, FOXAPP_OP_SYS_IMPORT_PLAN, FOXAPP_OP_SYS_IMPORT_COMMIT, FOXAPP_OP_SYS_BACKUP, FOXAPP_OP_SYS_RESET_PLAN,
                                   FOXAPP_OP_SYS_RESET_COMMIT, FOXAPP_OP_SYS_PENDING_CANCEL}) {
        const testing::Outcome o = r.sendOn(remote, textCmd(op, "skip"));
        CHECK_EQ(o.status(), FOXAPI_DENIED);
    }
    CHECK(r.fact("sys.firstrun.done") == "0");
    CHECK(!fs::exists(dir / "backups"));
}

}  // namespace

int main() {
    engine_api_test::hermetic("wire_c_sys");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    testExportImportBackup();
    testExportNothing();
    testReset();
    testBootStep();
    testFirstRun();
    testFirstRunSkip();
    testRemote();
    return check::finish("test_engine_api_wire_c_sys");
}
