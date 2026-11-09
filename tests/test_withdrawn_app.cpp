/*
 * THE KILL SWITCH IN THE REAL APPLICATION (field-early-warning). The update service can WITHDRAW a
 * release: it stops offering it, lists it in `withdrawn`, and says `current_withdrawn` when the
 * version asking is one of them. The parser's half is pinned in tests/test_updater.cpp; this runs
 * cascade itself, given the service's answer by FOXSDR_FAKE_UPDATE_MANIFEST (a file, because
 * fetchText refuses anything that is not https and so a stub on a loopback port cannot stand in for
 * foxsdr.com), and checks from the UI census what the window drew:
 *
 *   running      the version running is withdrawn and a good newer one is offered: the gentle
 *                notice AND the update banner that carries the way to update;
 *   no-fix       the version running is withdrawn and nothing newer is offered: the notice, saying
 *                no fixed version is ready yet, and NO update banner;
 *   not-offered  the version OFFERED is withdrawn though it is newer by number: no banner, no notice
 *                (nothing is wrong with the copy that is running);
 *   clean        a plain update and an empty list: the update banner and no notice;
 *   unticked     the Updates tick off: nothing asked, nothing drawn;
 *   store        a Microsoft Store copy asks the Store, not the canned answer: no notice.
 *
 * Isolated like every other test that starts the application: its own config, diagnostics tree and
 * a canned answer, nothing reaches a network.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

struct Result {
    bool ok = false;
    std::set<std::string> items;
    bool has(const char* item) const { return items.count(item) == 1; }
};

constexpr int kFrames = 60;
fs::path g_dir;

// The update service's answer, as the site writes it: a version, the three fields that make a
// download safe, the withdrawn list and (optionally) the flag.
std::string manifestText(const std::string& version, const std::string& withdrawnJson, bool currentWithdrawn) {
    return "{\"ok\":true,\"version\":\"" + version + "\",\"url\":\"https://foxsdr.com/download/"
           "foxsdr-setup-" + version + ".exe\",\"sha256\":\"393e5fd91b7b2292611c52af7e9f2db1e2c730d78f"
           "1639adefceaf0fb5cca0ea\",\"size\":3274221,\"newer\":true,\"notes\":[],\"messages\":[],"
           "\"withdrawn\":" + withdrawnJson + ",\"current_withdrawn\":" + (currentWithdrawn ? "true" : "false") + "}";
}

struct Run {
    std::string tag;
    bool ticked = true;           // updateCheckEnabled
    std::string package = "none"; // FOXSDR_FAKE_PACKAGE: "none" forces an unpackaged copy
    std::string store;            // FOXSDR_FAKE_STORE_UPDATE, for a packaged one
    std::string manifest;
};

Result once(const Run& r) {
    Result out;
    const fs::path cfg = g_dir / (r.tag + ".json");
    const fs::path census = g_dir / (r.tag + ".census");
    const fs::path diag = g_dir / (r.tag + "-diag");
    const fs::path canned = g_dir / (r.tag + ".manifest.json");
    std::error_code ec;
    fs::remove_all(diag, ec);
    fs::create_directories(diag, ec);
    {
        std::ofstream f(cfg, std::ios::binary | std::ios::trunc);
        f << "{ \"telemetryEnabled\": false, \"diagnosticsEnabled\": true, "
          << "\"updateCheckEnabled\": " << (r.ticked ? "true" : "false") << ", "
          << "\"sourceKind\": \"siggen\", \"uiTheme\": \"today\", \"interfaceScale\": \"100\" }\n";
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
    setEnv("FOXSDR_INPUT_SCRIPT", "");
    std::printf("    launching %s (%d frames)\n", r.tag.c_str(), kFrames);
    const std::string stdoutText =
        run("\"" + exePath() + "\" --frames " + std::to_string(kFrames) + " 2>&1");
    const bool rendered =
        stdoutText.find("rendered " + std::to_string(kFrames) + " frames") != std::string::npos;
    const bool written = stdoutText.find("ui census written") != std::string::npos;
    if (!rendered || !written) {
        std::printf("  %s: the run did not finish cleanly:\n%s\n", r.tag.c_str(), stdoutText.c_str());
        return out;
    }
    std::ifstream in(census);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("item ", 0) == 0) { out.items.insert(line.substr(5)); }
    }
    out.ok = true;
    std::printf("    %s found:%s%s%s\n", r.tag.c_str(), out.has("update:withdrawn-notice") ? " withdrawn notice" : "",
                out.has("update:withdrawn-no-replacement") ? ", no replacement" : "",
                out.has("update:site-banner") ? ", update banner" : "");
    return out;
}

void testRunningVersionWithdrawnAndAGoodOneOffered() {
    std::printf("  running\n");
    Run r;
    r.tag = "running";
    // 99.0.0 is newer than any build; the service says the version asking is withdrawn.
    r.manifest = manifestText("99.0.0", "[\"0.1.0\"]", true);
    const Result s = once(r);
    CHECK(s.ok);
    CHECK(s.has("update:withdrawn-notice"));
    CHECK(s.has("update:site-banner"));
    CHECK(!s.has("update:withdrawn-no-replacement"));
}

void testRunningVersionWithdrawnAndNothingToOffer() {
    std::printf("  no-fix\n");
    Run r;
    r.tag = "nofix";
    // 0.0.1 is older than any build: nothing is offered.
    r.manifest = manifestText("0.0.1", "[]", true);
    const Result s = once(r);
    CHECK(s.ok);
    CHECK(s.has("update:withdrawn-notice"));
    CHECK(s.has("update:withdrawn-no-replacement"));
    CHECK(!s.has("update:site-banner"));
}

void testAWithdrawnOfferIsNotShown() {
    std::printf("  not-offered\n");
    Run r;
    r.tag = "notoffered";
    // 99.0.0 is newer by number, and withdrawn: not offered. The copy running is fine.
    r.manifest = manifestText("99.0.0", "[\"99.0.0\"]", false);
    const Result s = once(r);
    CHECK(s.ok);
    CHECK(!s.has("update:site-banner"));
    CHECK(!s.has("update:withdrawn-notice"));
}

void testAPlainUpdateIsUnchanged() {
    std::printf("  clean\n");
    Run r;
    r.tag = "clean";
    r.manifest = manifestText("99.0.0", "[]", false);
    const Result s = once(r);
    CHECK(s.ok);
    CHECK(s.has("update:site-banner"));
    CHECK(!s.has("update:withdrawn-notice"));
}

void testUntickedAsksNobodyAndDrawsNothing() {
    std::printf("  unticked\n");
    Run r;
    r.tag = "unticked";
    r.ticked = false;
    r.manifest = manifestText("99.0.0", "[\"99.0.0\"]", true);
    const Result s = once(r);
    CHECK(s.ok);
    CHECK(!s.has("update:withdrawn-notice"));
    CHECK(!s.has("update:site-banner"));
}

void testAStoreCopyAsksTheStoreNotTheCannedAnswer() {
    std::printf("  store\n");
    Run r;
    r.tag = "store";
    r.package = "store";
    r.store = "none";
    r.manifest = manifestText("99.0.0", "[]", true);
    const Result s = once(r);
    CHECK(s.ok);
    CHECK(!s.has("update:withdrawn-notice"));
    CHECK(!s.has("update:site-banner"));
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_withdrawn_app\n");
    g_dir = fs::temp_directory_path() / ("cascade-withdrawn-" + std::to_string(pid()));
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

    testRunningVersionWithdrawnAndAGoodOneOffered();
    testRunningVersionWithdrawnAndNothingToOffer();
    testAWithdrawnOfferIsNotShown();
    testAPlainUpdateIsUnchanged();
    testUntickedAsksNobodyAndDrawsNothing();
    testAStoreCopyAsksTheStoreNotTheCannedAnswer();
    const int rc = testSummary("test_withdrawn_app");
    if (rc == 0) { fs::remove_all(g_dir, ec); }
    return rc;
}
