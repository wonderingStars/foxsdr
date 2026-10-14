/*
 * A MESSAGE FROM THE TEAM IS SHOWN WITHOUT AN UPDATE, UNTIL IT IS MARKED SEEN, IN THE REAL
 * APPLICATION (0.99.79). The owner's request (2026-10-14): "the broadcast system for the
 * software should send the broadcast out even if there isn't an update; it should post to
 * the user, then they click Seen and it doesn't show up again". The parser's half is
 * pinned in tests/test_updater.cpp and the config's in tests/test_config.cpp; this runs
 * cascade itself, given the update service's answer by FOXSDR_FAKE_UPDATE_MANIFEST (a
 * file, because fetchText refuses anything that is not https and so a stub on a loopback
 * port cannot stand in for foxsdr.com), and checks from the UI census, the diagnostic log
 * and the settings file it wrote:
 *
 *   shown       an answer with NO update and one message: the card and its Seen key are
 *               drawn, the update banner is not, the log says the answer was canned;
 *   seen        one press of Seen: the settings file holds the id; the next launch, on
 *               that same file and the same answer, draws no card (seen-again);
 *   pruned      an id the answer no longer carries is forgotten, the one it does is kept;
 *   important   the owner's "Important" tick reaches the card;
 *   with-update an answer with an update AND a message draws both;
 *   unticked    the Updates tick off: nothing asked, nothing drawn;
 *   store       a Microsoft Store copy asks the Store, not the canned answer: no card;
 *   two         two messages, two cards; the first one's Seen leaves exactly the second,
 *               and the second one's leaves exactly the first.
 *
 * The press is the application's own input script, aimed from the census. Isolated like
 * every other test that starts the application: its own config, APPDATA, LOCALAPPDATA and
 * diagnostics tree, and every FOXSDR_*_URL pointed at a closed local port.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "test_check.hpp"

namespace fs = std::filesystem;

namespace {

int pid() {
#if defined(_WIN32)
    return static_cast<int>(::GetCurrentProcessId());
#else
    return static_cast<int>(::getpid());
#endif
}

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    ::SetEnvironmentVariableA(name, value.empty() ? nullptr : value.c_str());
    _putenv_s(name, value.c_str());
#else
    if (value.empty()) {
        ::unsetenv(name);
    } else {
        ::setenv(name, value.c_str(), 1);
    }
#endif
}

std::string exePath() {
#if defined(_WIN32)
    return std::string(CASCADE_APP_BINDIR) + "/cascade.exe";
#else
    return std::string(CASCADE_APP_BINDIR) + "/cascade";
#endif
}

std::string run(const std::string& cmd) {
    std::string out;
#if defined(_WIN32)
    FILE* p = _popen(("\"" + cmd + "\"").c_str(), "r");
#else
    FILE* p = popen(cmd.c_str(), "r");
#endif
    char buf[512];
    while (p != nullptr && std::fgets(buf, sizeof(buf), p) != nullptr) { out += buf; }
#if defined(_WIN32)
    if (p != nullptr) { _pclose(p); }
#else
    if (p != nullptr) { pclose(p); }
#endif
    return out;
}

struct Rect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float cx() const { return (x0 + x1) * 0.5f; }
    float cy() const { return (y0 + y1) * 0.5f; }
};

struct Result {
    bool ok = false;
    std::set<std::string> items;
    std::map<std::string, Rect> rects;
    std::string log;                      // the run's diagnostic log, whole
    std::vector<std::string> messagesSeen;  // the settings file as the run left it
    bool has(const char* item) const { return items.count(item) == 1; }
    bool logs(const char* text) const { return log.find(text) != std::string::npos; }
    int cards() const {
        int n = 0;
        for (const std::string& i : items) {
            if (i.rfind("message:card:", 0) == 0) { ++n; }
        }
        return n;
    }
};

constexpr int kFrames = 70;
fs::path g_dir;

struct Msg {
    std::string id;
    std::vector<std::string> lines;
    bool important = false;
};

// The update service's answer, as the site writes it: a version, the three fields that
// make a download safe, and the messages. An old version (0.0.1) is "no update".
std::string manifestText(const std::string& version, const std::vector<Msg>& messages) {
    std::string out = "{\"ok\":true,\"version\":\"" + version + "\",\"url\":\"https://foxsdr.com/"
                      "download/foxsdr-setup-" + version + ".exe\",\"sha256\":\"393e5fd91b7b2292611c"
                      "52af7e9f2db1e2c730d78f1639adefceaf0fb5cca0ea\",\"size\":3274221,\"notes\":[],"
                      "\"messages\":[";
    for (std::size_t i = 0; i < messages.size(); ++i) {
        if (i != 0) { out += ","; }
        out += "{\"id\":\"" + messages[i].id + "\",\"date\":\"2026-10-14\",\"lines\":[";
        for (std::size_t j = 0; j < messages[i].lines.size(); ++j) {
            if (j != 0) { out += ","; }
            out += "\"" + messages[i].lines[j] + "\"";
        }
        out += std::string("],\"important\":") + (messages[i].important ? "true" : "false") + "}";
    }
    out += "]}";
    return out;
}

struct Run {
    std::string tag;
    std::string cfgName;          // the settings file's base name; the tag's when empty
    bool rewriteConfig = true;    // false: use the file the previous run left behind
    bool ticked = true;           // updateCheckEnabled
    std::string package = "none"; // FOXSDR_FAKE_PACKAGE: "none" forces an unpackaged copy
    std::string store;            // FOXSDR_FAKE_STORE_UPDATE, for a packaged one
    std::string seenJson;         // the "messagesSeen" array as JSON text; empty for none
    std::string manifest;         // the canned answer
    std::string script;
};

Result once(const Run& r) {
    Result out;
    const std::string cfgName = r.cfgName.empty() ? r.tag : r.cfgName;
    const fs::path cfg = g_dir / (cfgName + ".json");
    const fs::path census = g_dir / (r.tag + ".census");
    const fs::path diag = g_dir / (r.tag + "-diag");
    const fs::path canned = g_dir / (r.tag + ".manifest.json");
    std::error_code ec;
    fs::remove_all(diag, ec);
    fs::create_directories(diag, ec);
    if (r.rewriteConfig) {
        std::ofstream f(cfg, std::ios::binary | std::ios::trunc);
        f << "{ \"telemetryEnabled\": false, \"diagnosticsEnabled\": true, "
          << "\"updateCheckEnabled\": " << (r.ticked ? "true" : "false") << ", "
          << (r.seenJson.empty() ? std::string() : "\"messagesSeen\": " + r.seenJson + ", ")
          << "\"sourceKind\": \"siggen\", \"uiTheme\": \"today\", \"interfaceScale\": \"100\" "
             "}\n";
    }
    {
        std::ofstream f(canned, std::ios::binary | std::ios::trunc);
        f << r.manifest;
    }
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_UI_CENSUS", census.string());
    setEnv("FOXSDR_DIAG_DIR", diag.string());
    setEnv("FOXSDR_WINDOW_SIZE", "1920x1080");
    setEnv("FOXSDR_FAKE_PACKAGE", r.package);
    setEnv("FOXSDR_FAKE_STORE_UPDATE", r.store);
    setEnv("FOXSDR_FAKE_UPDATE_MANIFEST", canned.string());
    if (r.script.empty()) {
        setEnv("FOXSDR_INPUT_SCRIPT", "");
    } else {
        const fs::path sp = g_dir / (r.tag + ".script");
        std::ofstream f(sp);
        f << r.script;
        f.close();
        setEnv("FOXSDR_INPUT_SCRIPT", sp.string());
    }
    const std::string stdoutText =
        run("\"" + exePath() + "\" --frames " + std::to_string(kFrames) + " 2>&1");
    const bool rendered =
        stdoutText.find("rendered " + std::to_string(kFrames) + " frames") != std::string::npos;
    const bool written = stdoutText.find("ui census written") != std::string::npos;
    const bool scripted =
        r.script.empty() || stdoutText.find("script steps run") != std::string::npos;
    if (!rendered || !written || !scripted) {
        std::printf("  %s: the run did not finish cleanly:\n%s\n", r.tag.c_str(),
                    stdoutText.c_str());
        return out;
    }
    {
        std::ifstream in(census);
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind("item ", 0) == 0) { out.items.insert(line.substr(5)); }
            if (line.rfind("rect ", 0) != 0) { continue; }
            std::istringstream ss(line.substr(5));
            std::string name;
            Rect rc;
            ss >> name >> rc.x0 >> rc.y0 >> rc.x1 >> rc.y1;
            out.rects[name] = rc;
        }
    }
    {
        std::ifstream in(diag / "logs" / "foxsdr.log", std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        out.log = ss.str();
    }
    {
        // What the run left in the settings file: the application writes it again as it
        // ends, so this is the list as the NEXT launch would load it.
        std::ifstream in(cfg, std::ios::binary);
        const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
        if (!j.is_discarded() && j.is_object()) {
            const auto it = j.find("messagesSeen");
            if (it != j.end() && it->is_array()) {
                for (const auto& e : *it) {
                    if (e.is_string()) { out.messagesSeen.push_back(e.get<std::string>()); }
                }
            }
        }
    }
    out.ok = !out.log.empty();
    if (!out.ok) { std::printf("  %s: no diagnostic log was written\n", r.tag.c_str()); }
    std::printf("    %s found: %d card(s)%s%s%s%s, seen in the file: [", r.tag.c_str(),
                out.cards(), out.has("message:important") ? ", important" : "",
                out.has("update:site-banner") ? ", update banner" : "",
                out.has("updates:messages") ? ", section line" : "",
                out.logs("FOXSDR_FAKE_UPDATE_MANIFEST set") ? ", canned answer logged" : "");
    for (std::size_t i = 0; i < out.messagesSeen.size(); ++i) {
        std::printf("%s%s", i != 0 ? "," : "", out.messagesSeen[i].c_str());
    }
    std::printf("]\n");
    return out;
}

// A click on a key: mouse over it, down, up, then away. Later than the Store test's, so the
// canned answer has been taken and the card drawn well before the press.
std::string press(const Rect& k) {
    char buf[128];
    std::snprintf(buf, sizeof buf, "30 screen %.0f %.0f\n32 down\n34 up\n36 screen %.0f %.0f\n",
                  k.cx(), k.cy(), k.cx() + 400.0f, k.cy() + 300.0f);
    return buf;
}

constexpr const char* kCanned =
    "update: FOXSDR_FAKE_UPDATE_MANIFEST set - canned answer, foxsdr.com was not asked";

// Not the Store path: a copy that is not packaged asks foxsdr.com, or here the file.
void checkNotTheStore(const Result& r) {
    CHECK(!r.logs("asking the Microsoft Store"));
    CHECK(!r.logs("store update:"));
    CHECK(!r.has("update:store-banner"));
}

void testShown() {
    std::printf("  shown\n");
    Run r;
    r.tag = "shown";
    // 0.0.1 is older than any build: no update. The message is the whole of the answer.
    r.manifest = manifestText("0.0.1", {{"m1", {"Server maintenance tonight.", "Back by 9."}}});
    const Result s = once(r);
    CHECK(s.ok);
    CHECK(s.logs(kCanned));
    checkNotTheStore(s);
    // THE CARD, AND ITS KEY - with no update in sight.
    CHECK(s.has("message:card"));
    CHECK(s.has("message:id:m1"));
    CHECK(s.cards() == 1);
    CHECK(s.rects.count("message:seen") == 1);
    CHECK(!s.has("message:important"));
    CHECK(!s.has("update:site-banner"));
    // The Updates section says how many are waiting.
    CHECK(s.has("updates:messages"));
}

void testSeen() {
    std::printf("  seen\n");
    Run r;
    r.tag = "seen-find";
    r.manifest = manifestText("0.0.1", {{"m1", {"Server maintenance tonight."}}});
    const Result f = once(r);
    CHECK(f.ok);
    CHECK(f.rects.count("message:seen") == 1);
    if (f.rects.count("message:seen") != 1) { return; }
    const Rect key = f.rects.at("message:seen");
    std::printf("    key at %.0f,%.0f .. %.0f,%.0f\n", key.x0, key.y0, key.x1, key.y1);

    // ONE PRESS: the id is in the file the run leaves.
    r.tag = "seen";
    r.script = press(key);
    const Result s = once(r);
    CHECK(s.ok);
    const std::vector<std::string> want = {"m1"};
    CHECK(s.messagesSeen == want);

    // THE NEXT LAUNCH, on that same file and the same answer: no card, no section line.
    // The answer still carries m1, so it is not pruned - it is remembered.
    Run again;
    again.tag = "seen-again";
    again.cfgName = "seen";
    again.rewriteConfig = false;
    again.manifest = r.manifest;
    const Result a = once(again);
    CHECK(a.ok);
    CHECK(a.logs(kCanned));
    CHECK(!a.has("message:card"));
    CHECK(a.cards() == 0);
    CHECK(!a.has("updates:messages"));
    CHECK(a.messagesSeen == want);
}

void testPruned() {
    std::printf("  pruned\n");
    Run r;
    r.tag = "pruned";
    r.seenJson = "[\"m1\",\"gone\"]";
    r.manifest = manifestText("0.0.1", {{"m1", {"Server maintenance tonight."}}});
    const Result p = once(r);
    CHECK(p.ok);
    // m1 is still carried and was seen: no card. "gone" is no longer carried: forgotten, so
    // a message the owner withdraws and sends again is shown again.
    CHECK(!p.has("message:card"));
    const std::vector<std::string> want = {"m1"};
    CHECK(p.messagesSeen == want);
}

void testImportant() {
    std::printf("  important\n");
    Run r;
    r.tag = "important";
    r.manifest = manifestText("0.0.1", {{"imp", {"Do not update yet."}, true}});
    const Result i = once(r);
    CHECK(i.ok);
    CHECK(i.has("message:card"));
    CHECK(i.has("message:important"));
    // An important MESSAGE is not an update, and not a critical one.
    CHECK(!i.has("update:site-banner"));
}

void testWithUpdate() {
    std::printf("  with-update\n");
    Run r;
    r.tag = "with-update";
    r.manifest = manifestText("99.0.0", {{"m1", {"Server maintenance tonight."}}});
    const Result w = once(r);
    CHECK(w.ok);
    CHECK(w.has("message:card"));
    CHECK(w.has("update:site-banner"));
    checkNotTheStore(w);
}

void testUnticked() {
    std::printf("  unticked\n");
    Run r;
    r.tag = "unticked";
    r.ticked = false;
    r.manifest = manifestText("0.0.1", {{"m1", {"Server maintenance tonight."}}});
    const Result u = once(r);
    CHECK(u.ok);
    // Nobody was asked, so there is nothing to draw and nothing was read.
    CHECK(!u.logs(kCanned));
    CHECK(!u.has("message:card"));
    CHECK(!u.has("updates:messages"));
}

void testStoreCopyHearsNone() {
    std::printf("  store\n");
    // A copy from the Microsoft Store asks the Store, never foxsdr.com, so the canned answer
    // (which stands in for foxsdr.com) is not read and no card is drawn - the documented
    // limit of this feature, pinned so that it cannot change without a decision.
    Run r;
    r.tag = "store";
    r.package = "hedgerowlabs.FoxSDR_1.99.79.0_x64__6ex9a1hsv3jv8";  // Store-looking
    r.store = "none";
    r.manifest = manifestText("0.0.1", {{"m1", {"Server maintenance tonight."}}});
    const Result s = once(r);
    CHECK(s.ok);
    CHECK(s.logs("asking the Microsoft Store, not foxsdr.com"));
    CHECK(!s.logs(kCanned));
    CHECK(!s.has("message:card"));
    CHECK(!s.has("updates:messages"));
}

void testTwo() {
    std::printf("  two\n");
    Run r;
    r.tag = "two-find";
    r.cfgName = "two";
    r.manifest = manifestText("0.0.1", {{"m1", {"First."}}, {"m2", {"Second."}}});
    const Result f = once(r);
    CHECK(f.ok);
    // TWO CARDS, each with a Seen key of its own.
    CHECK(f.cards() == 2);
    CHECK(f.has("message:id:m1"));
    CHECK(f.has("message:id:m2"));
    CHECK(f.rects.count("message:seen:0") == 1);
    CHECK(f.rects.count("message:seen:1") == 1);
    if (f.rects.count("message:seen:0") != 1 || f.rects.count("message:seen:1") != 1) { return; }
    const Rect first = f.rects.at("message:seen:0");
    const Rect second = f.rects.at("message:seen:1");
    CHECK(second.y0 > first.y1);  // below it, not on top of it

    // PRESS THE FIRST: its id is in the file, the second's is not.
    Run p = r;
    p.tag = "two-press";
    p.script = press(first);
    const Result pressed = once(p);
    CHECK(pressed.ok);
    const std::vector<std::string> want = {"m1"};
    CHECK(pressed.messagesSeen == want);

    // THE NEXT LAUNCH draws exactly one card, and it is the second message.
    Run n = r;
    n.tag = "two-after";
    n.rewriteConfig = false;
    n.cfgName = "two";  // the file the press left behind
    const Result after = once(n);
    CHECK(after.ok);
    CHECK(after.cards() == 1);
    CHECK(after.has("message:id:m2"));
    CHECK(!after.has("message:id:m1"));
    CHECK(after.messagesSeen == want);

    // PRESS THE SECOND INSTEAD: the first card stays, and only the second's id is kept. Two
    // cards both carry a key labelled "Seen"; this is the press that lands on the wrong one if
    // the keys share an ImGui identity.
    Run s = r;
    s.tag = "two-press-second";
    s.cfgName = "two-second";
    s.script = press(second);
    const Result pressedSecond = once(s);
    CHECK(pressedSecond.ok);
    const std::vector<std::string> wantSecond = {"m2"};
    CHECK(pressedSecond.messagesSeen == wantSecond);

    Run n2 = r;
    n2.tag = "two-after-second";
    n2.rewriteConfig = false;
    n2.cfgName = "two-second";
    const Result afterSecond = once(n2);
    CHECK(afterSecond.ok);
    CHECK(afterSecond.cards() == 1);
    CHECK(afterSecond.has("message:id:m1"));
    CHECK(!afterSecond.has("message:id:m2"));
    CHECK(afterSecond.messagesSeen == wantSecond);
}

}  // namespace

int main() {
    std::printf("test_team_message_app\n");
    g_dir = fs::temp_directory_path() / ("cascade-team-message-" + std::to_string(pid()));
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir / "appdata", ec);
    fs::create_directories(g_dir / "local", ec);
    setEnv("APPDATA", (g_dir / "appdata").string());
    setEnv("LOCALAPPDATA", (g_dir / "local").string());
    setEnv("XDG_CONFIG_HOME", (g_dir / "appdata").string());
    setEnv("XDG_DATA_HOME", (g_dir / "local").string());
    setEnv("XDG_STATE_HOME", (g_dir / "local").string());
    for (const char* url : {"FOXSDR_TELEMETRY_URL", "FOXSDR_CRASH_URL", "FOXSDR_UPDATE_URL",
                            "FOXSDR_REPORTS_URL", "FOXSDR_FEATURE_URL", "FOXSDR_PROBLEM_URL",
                            "FOXSDR_BETA_API_URL", "FOXSDR_TESTER_USAGE_URL"}) {
        setEnv(url, "http://127.0.0.1:9/");
    }
    setEnv("FOXSDR_SINGLE_VIEWPORT", "1");
    setEnv("FOXSDR_PATCH_START", "");
    // The Updates section's line ("2 message(s) from the FoxSDR team") is inside the section's
    // body, which is folded on a fresh install; the application's verification hook opens it.
    setEnv("FOXSDR_OPEN_SECTIONS", "Updates");

    testShown();
    testSeen();
    testPruned();
    testImportant();
    testWithUpdate();
    testUnticked();
    testStoreCopyHearsNone();
    testTwo();

    const int rc = testSummary("test_team_message_app");
    if (rc == 0) { fs::remove_all(g_dir, ec); }
    return rc;
}
