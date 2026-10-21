/*
 * A COPY FROM THE MICROSOFT STORE ASKS THE STORE, IN THE REAL APPLICATION
 * (0.99.53). The policy is pinned in tests/test_package_identity.cpp and the
 * seam in tests/test_store_update.cpp; this runs cascade itself, made to take
 * the packaged branches by FOXSDR_FAKE_PACKAGE (a Store-looking full name)
 * and given the Store's answer by FOXSDR_FAKE_STORE_UPDATE, and checks from
 * the UI census and the diagnostic log it wrote:
 *
 *   available  the banner and its "Install from the Microsoft Store" key are
 *              drawn, the section reads "update available", and the log says
 *              the Store - not foxsdr.com - was asked;
 *   install    one press of that key runs the (fake) install request and the
 *              banner shows its outcome: installed, or cancelled;
 *   mandatory  the banner is drawn in its "important" form;
 *   none       no banner; the section reads up to date;
 *   error      no banner - a failed check is shown only in the section;
 *   unpackaged the same seam, on a copy that is NOT packaged: the Store is
 *              never asked and no Store banner is drawn;
 *   unticked   a packaged copy with the box unticked asks nobody.
 *
 * The press is the application's own input script, aimed from the census.
 * Isolated like every other test that starts the application: its own config,
 * APPDATA, LOCALAPPDATA and diagnostics tree, and every FOXSDR_*_URL pointed
 * at a closed local port.
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

// Store-looking: the identity the Store assigned (hedgerowlabs.FoxSDR), a
// package version of product major + 1, and a publisher-hash-shaped suffix.
constexpr const char* kStoreFullName = "hedgerowlabs.FoxSDR_1.99.53.0_x64__6ex9a1hsv3jv8";

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
    std::string log;  // the run's diagnostic log, whole
    bool has(const char* item) const { return items.count(item) == 1; }
    bool logs(const char* text) const { return log.find(text) != std::string::npos; }
};

constexpr int kFrames = 70;
fs::path g_dir;

struct Run {
    std::string tag;
    std::string package = kStoreFullName;  // FOXSDR_FAKE_PACKAGE
    std::string store;                     // FOXSDR_FAKE_STORE_UPDATE
    std::string install;                   // FOXSDR_FAKE_STORE_INSTALL
    bool ticked = true;                    // updateCheckEnabled
    std::string script;
};

Result once(const Run& r) {
    Result out;
    const fs::path cfg = g_dir / (r.tag + ".json");
    const fs::path census = g_dir / (r.tag + ".census");
    const fs::path diag = g_dir / (r.tag + "-diag");
    std::error_code ec;
    fs::remove_all(diag, ec);
    fs::create_directories(diag, ec);
    {
        std::ofstream f(cfg, std::ios::binary | std::ios::trunc);
        f << "{ \"telemetryEnabled\": false, \"diagnosticsEnabled\": true, "
          << "\"updateCheckEnabled\": " << (r.ticked ? "true" : "false")
          << ", \"sourceKind\": \"siggen\", \"uiTheme\": \"today\", \"interfaceScale\": \"100\" "
             "}\n";
    }
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_UI_CENSUS", census.string());
    setEnv("FOXSDR_DIAG_DIR", diag.string());
    setEnv("FOXSDR_WINDOW_SIZE", "1920x1080");
    setEnv("FOXSDR_FAKE_PACKAGE", r.package);
    setEnv("FOXSDR_FAKE_STORE_UPDATE", r.store);
    setEnv("FOXSDR_FAKE_STORE_INSTALL", r.install);
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
    out.ok = !out.log.empty();
    if (!out.ok) { std::printf("  %s: no diagnostic log was written\n", r.tag.c_str()); }
    return out;
}

std::string press(const Rect& k) {
    char buf[128];
    std::snprintf(buf, sizeof buf, "20 screen %.0f %.0f\n22 down\n24 up\n26 screen %.0f %.0f\n",
                  k.cx(), k.cy(), k.cx() + 400.0f, k.cy() + 300.0f);
    return buf;
}

constexpr const char* kAskedStore = "asking the Microsoft Store, not foxsdr.com";
constexpr const char* kAnyStoreLine = "store update:";

void testAvailableAndInstall() {
    std::printf("  available\n");
    Run r;
    r.tag = "available";
    r.store = "available";
    const Result a = once(r);
    CHECK(a.ok);
    // THE STORE WAS ASKED - and it was the canned answer, said so in the log.
    CHECK(a.logs(kAskedStore));
    CHECK(a.logs("store update: FOXSDR_FAKE_STORE_UPDATE=available"));
    CHECK(a.logs("store update: the Store lists 1 update(s)"));
    // THE BANNER, AND ITS KEY.
    CHECK(a.has("update:store-banner"));
    CHECK(!a.has("update:store-mandatory"));
    CHECK(a.has("update:store-install"));
    CHECK(a.rects.count("update:store-install") == 1);
    CHECK(a.has("updates:store-available"));
    // Nothing was requested without a press.
    CHECK(!a.logs("install requested"));
    CHECK(!a.has("update:store-outcome:installed"));
    if (a.rects.count("update:store-install") != 1) { return; }
    const Rect key = a.rects.at("update:store-install");
    std::printf("    key at %.0f,%.0f .. %.0f,%.0f\n", key.x0, key.y0, key.x1, key.y1);

    // ONE PRESS: the install request runs and its outcome is drawn.
    r.tag = "install";
    r.script = press(key);
    const Result i = once(r);
    CHECK(i.ok);
    CHECK(i.logs("store update: install requested from the banner"));
    CHECK(i.logs("canned install outcome, nothing was requested"));
    CHECK(i.logs("store update: install request ended: installed"));
    CHECK(i.has("update:store-outcome:installed"));
    CHECK(!i.has("update:store-outcome:cancelled"));

    // THE SAME PRESS, the user declining Windows' dialog: said as a cancel,
    // and the key is offered again.
    r.tag = "cancel";
    r.install = "cancelled";
    const Result c = once(r);
    CHECK(c.ok);
    CHECK(c.logs("store update: install request ended: cancelled by the user"));
    CHECK(c.has("update:store-outcome:cancelled"));
    CHECK(!c.has("update:store-outcome:installed"));
    CHECK(c.has("update:store-install"));
}

void testMandatory() {
    std::printf("  mandatory\n");
    Run r;
    r.tag = "mandatory";
    r.store = "mandatory";
    const Result m = once(r);
    CHECK(m.ok);
    CHECK(m.logs("at least one mandatory"));
    CHECK(m.has("update:store-banner"));
    CHECK(m.has("update:store-mandatory"));
    CHECK(m.has("update:store-install"));
}

void testNone() {
    std::printf("  none\n");
    Run r;
    r.tag = "none";
    r.store = "none";
    const Result n = once(r);
    CHECK(n.ok);
    CHECK(n.logs(kAskedStore));
    CHECK(n.logs("store update: the Store lists 0 update(s)"));
    CHECK(!n.has("update:store-banner"));
    CHECK(!n.has("update:store-install"));
    CHECK(n.has("updates:store-uptodate"));
}

void testError() {
    std::printf("  error\n");
    Run r;
    r.tag = "error";
    r.store = "error";
    const Result e = once(r);
    CHECK(e.ok);
    CHECK(e.logs(kAskedStore));
    CHECK(e.logs("store update: the check did not complete: 0x803F6107"));
    // A failed check is not the user's problem: no banner, only the section.
    CHECK(!e.has("update:store-banner"));
    CHECK(!e.has("update:store-install"));
    CHECK(e.has("updates:store-failed"));
}

void testUnpackagedNeverAsksTheStore() {
    std::printf("  unpackaged\n");
    Run r;
    r.tag = "unpackaged";
    r.package = "none";  // FOXSDR_FAKE_PACKAGE=none: forced unpackaged
    r.store = "available";
    const Result u = once(r);
    CHECK(u.ok);
    CHECK(!u.logs(kAnyStoreLine));
    CHECK(!u.logs(kAskedStore));
    CHECK(!u.has("update:store-banner"));
    CHECK(!u.has("update:store-install"));
    // The section is the foxsdr.com one, not the Store's.
    bool siteState = false;
    bool storeState = false;
    for (const std::string& it : u.items) {
        if (it.rfind("updates:site-", 0) == 0) { siteState = true; }
        if (it.rfind("updates:store-", 0) == 0) { storeState = true; }
    }
    CHECK(siteState);
    CHECK(!storeState);
}

void testUntickedAsksNobody() {
    std::printf("  unticked\n");
    Run r;
    r.tag = "unticked";
    r.store = "available";
    r.ticked = false;
    const Result o = once(r);
    CHECK(o.ok);
    CHECK(!o.logs(kAnyStoreLine));
    CHECK(!o.logs(kAskedStore));
    CHECK(!o.has("update:store-banner"));
    CHECK(o.has("updates:store-off"));
}

}  // namespace

int main() {
    std::printf("test_store_update_app\n");
    g_dir = fs::temp_directory_path() / ("cascade-store-update-" + std::to_string(pid()));
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

    testAvailableAndInstall();
    testMandatory();
    testNone();
    testError();
    testUnpackagedNeverAsksTheStore();
    testUntickedAsksNobody();

    const int rc = testSummary("test_store_update_app");
    if (rc == 0) { fs::remove_all(g_dir, ec); }
    return rc;
}
