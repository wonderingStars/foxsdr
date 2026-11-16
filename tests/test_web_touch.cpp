// LANE X, row G5-17 (docs/multi-radio-records/lane-x.md): the browser page on a phone-sized touch screen.
//
// The test starts the real web server on loopback, serves the real page, and runs tests/web_touch_drive.mjs: a HEADLESS browser (no window) with touch emulation at 390 x 844 that taps, swipes and
// drags the page through the DevTools protocol (real touch events, not synthesised mouse ones). The script prints what it measured in the page; this test asserts on it. It needs the node and the
// browser the machine already has: with neither it SKIPS, saying so, and never passes on nothing. A real phone is not this: UNVERIFIED in the record.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "net/web_server.hpp"
#include "test_check.hpp"

#ifdef _WIN32
#include <process.h>
#define TEST_GETPID _getpid
#else
#include <unistd.h>
#define TEST_GETPID getpid
#endif

using namespace cascade::net;
namespace fs = std::filesystem;

namespace {

RadioStatus sampleStatus() {
    RadioStatus s;
    s.running = true;
    s.centerHz = 100000000.0;
    s.sampleRateHz = 2000000.0;
    s.vfoOffsetHz = 300000.0;
    s.bandwidthHz = 150000.0;
    s.mode = "WFM";
    s.sourceName = "SigGen";
    return s;
}

std::string runCommand(const std::string& cmd) {
    std::string out;
#ifdef _WIN32
    FILE* p = _popen(cmd.c_str(), "r");
#else
    FILE* p = popen(cmd.c_str(), "r");
#endif
    if (!p) { return out; }
    char buf[4096];
    while (std::fgets(buf, sizeof buf, p)) { out += buf; }
#ifdef _WIN32
    _pclose(p);
#else
    pclose(p);
#endif
    return out;
}

}  // namespace

int main() {
#if defined(FOXSDR_TEST_NODE) && defined(FOXSDR_TEST_TOUCH_JS)
    WebServer server;
    server.setStatusProvider([]() { return sampleStatus(); });
    WebServerConfig cfg;
    cfg.enabled = true;
    cfg.bindAddress = "127.0.0.1";
    cfg.username = "admin";
    std::string error;
    int port = -1;
    for (int p = 18273; p < 18373 && port < 0; ++p) { cfg.port = p; if (server.start(cfg, error)) { port = p; } }
    CHECK(port > 0);
    if (port <= 0) { return testSummary("test_web_touch"); }

    const fs::path profile = fs::temp_directory_path() / ("foxsdr-web-touch-" + std::to_string(TEST_GETPID()));
    std::error_code ec;
    fs::create_directories(profile, ec);
    // the whole command is quoted once more for cmd.exe (_popen runs it through "cmd /c")
    const std::string cmd = std::string("\"\"") + FOXSDR_TEST_NODE + "\" \"" + FOXSDR_TEST_TOUCH_JS + "\" " + std::to_string(port) + " \"" + profile.string() + "\"\"";
    const std::string text = runCommand(cmd);
    server.stop();
    fs::remove_all(profile, ec);

    const std::size_t brace = text.rfind('{');
    nlohmann::json j = nlohmann::json::parse(brace == std::string::npos ? "{}" : text.substr(brace), nullptr, false);
    if (!j.is_object()) { std::printf("test_web_touch: no result from the script; its output was: %s\n", text.c_str()); j = nlohmann::json::object(); }
    if (j.contains("skipped")) {
        ++g_checksSkipped;
        std::printf("SKIP: %s\n", j["skipped"].get<std::string>().c_str());
        return testSummary("test_web_touch");
    }
    std::printf("test_web_touch: %s\n", j.dump().c_str());
    CHECK(!j.contains("error"));
    CHECK(j.value("ok", false));
    CHECK(j.value("width", 0) == 390);
    CHECK(j.value("coarse", false));                                              // the page was shown to a coarse pointer
    CHECK(j.value("overflowX", 999) <= 1);                                        // no sideways scrolling at phone width
    CHECK(j.contains("small") && j["small"].is_array() && j["small"].empty());     // every visible key and field is at least 44 px
    // the digits: a tap on the upper half steps up, on the lower half down, a swipe of 35 px steps twice, each by the digit's own decade
    CHECK(j.contains("digitControls") && j["digitControls"].is_array() && j["digitControls"].size() >= 4);
    if (j.contains("digitControls") && j["digitControls"].is_array() && j["digitControls"].size() >= 4 && j.contains("digitDecade")) {
        const double base = j.value("centreBefore", 0.0);
        int decade = std::atoi(j["digitDecade"].get<std::string>().c_str());
        double step = 1.0;
        for (int i = 0; i < decade; ++i) { step *= 10.0; }
        CHECK_NEAR(j["digitControls"][0].get<double>(), base + step, 0.5);          // upper-half tap
        CHECK_NEAR(j["digitControls"][1].get<double>(), base - step, 0.5);          // lower-half tap
        CHECK(j["digitControls"][2].get<double>() > base);                          // the swipe steps up
    }
    CHECK(j.value("specAfter", 0.0) < j.value("specBefore", 0.0) - 10.0);           // a finger drag on the divider moved it
    CHECK(j.value("splitHandleH", 0.0) >= 20.0);                                   // and the divider is wide enough to take a finger
    CHECK(j.value("spectrumTapClicks", 0) == 1);                                    // a tap on the spectrum is the click its tune-here listener takes
    CHECK(j.contains("gripVarBefore") && j["gripVarAfter"] != j["gripVarBefore"]);   // on a tablet-sized screen the column grip follows a finger
#else
    ++g_checksSkipped;
    std::printf("SKIP: node was not found at configure time\n");
#endif
    return testSummary("test_web_touch");
}
