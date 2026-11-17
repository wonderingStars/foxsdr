// test_engine_api_s4b_notify.cpp - screen lane S4b, G3-37b: the desktop notification's words, that building the real notifier does nothing, that the engine hands a raised alert to whatever sink it is given, and
// that NOTHING BUT THE WINDOW'S REAL RUN can install the real notifier (so no test can put a notification on the owner's desktop).
// The real notification is never shown here: it is UNVERIFIED until the mouse drive.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "engine/s4b_notify_win.hpp"
#include "wire_c_rig.hpp"

using namespace wirec_test;
using cascade::engine::s4b::balloonText;
using cascade::engine::s4b::DesktopNotifier;

namespace {

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void testWords() {
    auto t = balloonText("Mayday", "heard on 156.8");
    CHECK(t.title == u"Mayday" && t.body == u"heard on 156.8");
    t = balloonText(std::string(200, 'T'), std::string(1000, 'b'));
    CHECK(t.title.size() == 63 && t.body.size() == 255);
    // an emoji (a surrogate pair) at the cut is dropped whole, never split
    std::string text(254, 'a');
    text += "\xF0\x9F\x98\x80";
    t = balloonText("x", text);
    CHECK(t.body.size() == 254 && t.body.back() == u'a');
    t = balloonText("x", "\xF0\x9F\x98\x80");
    CHECK(t.body.size() == 2 && t.body[0] >= 0xD800 && t.body[0] <= 0xDBFF && t.body[1] >= 0xDC00 && t.body[1] <= 0xDFFF);
    // malformed UTF-8 and control characters
    t = balloonText("a\x01" "b", std::string("ok\xC3\x28 \xE2\x82 \xFF end\x07", 14));
    CHECK(t.title == u"a b");
    bool noControl = true, hasReplacement = false;
    for (const char16_t c : t.body) { if (c < 0x20 && c != u'\n') { noControl = false; } if (c == 0xFFFD) { hasReplacement = true; } }
    CHECK(noControl && hasReplacement);
    CHECK(balloonText("a\nb", "line one\nline two").body == u"line one\nline two");
    CHECK(balloonText("", "body").title == u"FoxSDR");
    CHECK(balloonText("t", "").body.empty());
    // an overlong or truncated sequence is replaced, not read
    CHECK(balloonText("t", "\xC0\x80").body.find(u'\0') == std::u16string::npos);
    CHECK(balloonText("t", "\xED\xA0\x80").body.find(char16_t(0xD800)) == std::u16string::npos);
}

void testBuildingDoesNothing() {
    DesktopNotifier n;
    CHECK(!n.threadStarted() && n.shown() == 0 && n.dropped() == 0 && std::string(n.name()) == "desktop");
}

void testEngineHandsAlertsToTheSink(const fs::path& dir) {
    Rig rig(dir);
    auto rec = std::make_shared<cascade::core::alerts::RecordingSink>();
    rig.eng.setDesktopNotifier(rec);
    CHECK(rig.status(setting("alerts.keywords", "mayday")) == FOXAPI_OK);
    CHECK(rig.status(setting("alerts.enabled", "1")) == FOXAPI_OK);
    rig.pass(4);
    rig.eng.pluginApiShared()->hostPostEvent("pocsag-decoder", false, "text", "a MAYDAY call", 148.0e6, 1000000);
    bool got = false;
    for (int i = 0; i < 80 && !got; ++i) {
        rig.pass(2);
        got = rec->items().size() == 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(got && rig.fact("alerts.notified") == "1");
    // the setting that decides whether an alert goes to the sink at all
    CHECK(rig.fact("wirec.alerts.notify") == "1");
    CHECK(rig.status(setting("alerts.notify", "0")) == FOXAPI_OK);
    rig.pass(4);
    rig.eng.pluginApiShared()->hostPostEvent("pocsag-decoder", false, "text", "another MAYDAY call", 148.0e6, 99000000);
    for (int i = 0; i < 40; ++i) { rig.pass(2); std::this_thread::sleep_for(std::chrono::milliseconds(30)); }
    CHECK(rec->items().size() == 1);
}

void testOnlyTheWindowInstallsTheRealOne() {
    namespace stdfs = std::filesystem;
    // the repository root from this file's own path
    stdfs::path here = stdfs::path(__FILE__).parent_path().parent_path();
    const stdfs::path src = here / "src";
    CHECK(stdfs::exists(src / "slint" / "main.cpp"));
    const std::string main = slurp(src / "slint" / "main.cpp");
    const std::size_t at = main.find("DesktopNotifier>()");
    CHECK(at != std::string::npos && at > 120);
    if (at != std::string::npos && at > 120) {
        const std::string before = main.substr(at - 160, 160);
        CHECK(before.find("if (!bounded.bounded())") != std::string::npos);   // a bounded (scripted) run never installs it
    }
    // no other file builds one
    int builders = 0;
    for (const auto& e : stdfs::recursive_directory_iterator(src)) {
        if (!e.is_regular_file()) { continue; }
        const std::string ext = e.path().extension().string();
        if (ext != ".cpp" && ext != ".hpp") { continue; }
        const std::string name = e.path().filename().string();
        if (name == "s4b_notify_win.cpp" || name == "s4b_notify_win.hpp") { continue; }
        if (slurp(e.path()).find("DesktopNotifier>(") != std::string::npos) { ++builders; }
    }
    CHECK(builders == 1);   // foxsdr-slint's main.cpp, and nothing else in the program
    // and no test builds one
    int testBuilders = 0;
    for (const auto& e : stdfs::directory_iterator(here / "tests")) {
        if (!e.is_regular_file() || e.path().filename() == "test_engine_api_s4b_notify.cpp" || (e.path().extension() != ".cpp" && e.path().extension() != ".hpp")) { continue; }
        if (slurp(e.path()).find("DesktopNotifier") != std::string::npos) { ++testBuilders; }
    }
    CHECK(testBuilders == 0);
}

}  // namespace

int main() {
    engine_api_test::hermetic("s4b_notify");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    testWords();
    testBuildingDoesNothing();
    testOnlyTheWindowInstallsTheRealOne();
    const fs::path d = scratch("s4bnotify");
    testEngineHandsAlertsToTheSink(d);
    std::error_code ec;
    fs::remove_all(d, ec);
    return check::finish("test_engine_api_s4b_notify");
}
