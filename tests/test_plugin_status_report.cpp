// test_plugin_status_report.cpp - the two bounded-run plugin diagnostics are the ENGINE's (window port group 14, part e).
//
// reportPluginStatus (CASCADE_PLUGIN_STATUS) and reportPluginTestResult (CASCADE_PLUGIN_TEST) were AppWindow members that read
// thirty-six engine fields between them - the largest block of direct reads the window had left - and printed to stdout for a
// headless run that has to PROVE what the enforcement and the install gate decided. They moved into the engine
// (src/engine/engine_diagnostics.cpp) with their bodies and their words unchanged; the window keeps the two switches, calls the
// status report once after the teardown's first phase and names the frame for the catalogue report.
//
// WHAT HOLDS THE MOVE. The proof that nothing visible changed is a before/after capture of the shipped binary against fixture
// plugins (the group-14 record in docs/engine-stage4.md: thirty-nine lines, byte-identical but for the timings and a picture's
// progress, which the fixture advances on a clock). THIS test is what keeps it so afterwards, on the engine alone: every line the
// status report prints, in order, against four REAL modules (tests/fixture_*_plugin.cpp: a text decoder, a picture decoder and
// two track sources, each with a panel and an instrument) and the picture the window hands over; the catalogue report's failure
// line, and an entry's line with both answers of the install gate (the acknowledgement unticked and ticked).
//   A  the status report: the counts, the inventory notes, one line per module, the source, the plugin UI after a poll (tracks,
//      paths, panels), the picture, the tracks, what is stopped, the mute states, the audio mute, the runner
//   B  the status report with nothing fitted still prints its counts ("no line" and "nothing there" must differ)
//   C  the catalogue report: a failed fetch prints FAILED with the reason and nothing else; a catalogue prints its entry count
//      and one line per entry, the gate's two answers as ok / blocked(reason)
//   D  the SHIPPED binary (copied, with three real modules beside it): a plain --frames run prints nothing of the report; with
//      CASCADE_PLUGIN_STATUS the window calls the engine's report once at the end and hands over its plugin windows' picture
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/host_image.hpp"
#include "core/plugin_abi.h"
#include "core/plugin_repo.hpp"
#include "engine/engine.hpp"
#include "engine_api_test_env.hpp"
#include "test_check.hpp"

namespace cascade::gui {
struct AppWindowTestAccess {
    static void setPluginDir(std::string (*dir)()) { cascade::engine::Engine::testHooks_.pluginDir = dir; }
    // What the engine's pass does when a source opens or changes: rebuild the runner and the plugin UI over the loaded modules.
    static void refreshRunner(cascade::engine::Engine& e) { e.refreshPluginRunner(); }
    // The frame's one poll of the plugin UI (what the window's hostPollPlugins does).
    static void poll(cascade::engine::Engine& e) { e.pluginUi_.poll(); }
    static std::string pluginDir(cascade::engine::Engine& e) { return e.pluginDir_; }
    static void setCatalogue(cascade::engine::Engine& e, std::vector<cascade::core::PluginCatalogEntry> entries,
                             const std::string& error) {
        e.catalog_ = std::move(entries);
        e.catalogError_ = error;
        e.publishStatusText();  // (the report reads the published copy, as the host hook's contract says)
    }
    static std::string blocked(cascade::engine::Engine& e, int i, bool ack) { return e.pluginInstallBlockedReason(i, ack); }
};
}  // namespace cascade::gui
using Access = cascade::gui::AppWindowTestAccess;

namespace {

std::string g_pluginDir;
std::string pluginDirHook() { return g_pluginDir; }

// Runs `fn` with stdout redirected into a file, and returns what it printed, split into lines.
template <class Fn>
std::vector<std::string> capture(const std::filesystem::path& file, Fn&& fn) {
    std::fflush(stdout);
#if defined(_WIN32)
    const int saved = _dup(_fileno(stdout));
    FILE* f = std::fopen(file.string().c_str(), "wb");
    _dup2(_fileno(f), _fileno(stdout));
#else
    const int saved = dup(fileno(stdout));
    FILE* f = std::fopen(file.string().c_str(), "wb");
    dup2(fileno(f), fileno(stdout));
#endif
    fn();
    std::fflush(stdout);
#if defined(_WIN32)
    _dup2(saved, _fileno(stdout));
    _close(saved);
#else
    dup2(saved, fileno(stdout));
    close(saved);
#endif
    std::fclose(f);
    std::ifstream in(file, std::ios::binary);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        lines.push_back(line);
    }
    return lines;
}

bool startsWith(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }

int indexOfPrefix(const std::vector<std::string>& lines, const std::string& prefix, int from = 0) {
    for (int i = from; i < static_cast<int>(lines.size()); ++i) {
        if (startsWith(lines[static_cast<std::size_t>(i)], prefix)) { return i; }
    }
    return -1;
}

int countPrefix(const std::vector<std::string>& lines, const std::string& prefix) {
    int n = 0;
    for (const std::string& l : lines) {
        if (startsWith(l, prefix)) { ++n; }
    }
    return n;
}

std::string fileOf(const char* path) { return std::filesystem::path(path).filename().string(); }

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_plugin_status_report\n");
    engine_api_test::hermetic("pluginstatus");
    const char* base = std::getenv("LOCALAPPDATA");
    const std::filesystem::path scratch = std::filesystem::path(base != nullptr ? base : ".") / "status_report";
    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
    std::filesystem::create_directories(scratch, ec);

    // A: four real modules fitted.
    {
        const std::filesystem::path dir = scratch / "plugins";
        std::filesystem::create_directories(dir, ec);
        for (const char* src : {FIXTURE_STAGE1_PLUGIN, FIXTURE_GROUP13_PLUGIN, FIXTURE_GROUP14_PLUGIN, FIXTURE_GROUP14B_PLUGIN}) {
            std::filesystem::copy_file(src, dir / fileOf(src), std::filesystem::copy_options::overwrite_existing, ec);
            CHECK(!ec);
        }
        g_pluginDir = dir.string();
        Access::setPluginDir(&pluginDirHook);
        {
            cascade::engine::Engine e;
            e.initialise();
            Access::setPluginDir(nullptr);
            Access::refreshRunner(e);
            Access::poll(e);

            // The picture the window's plugin windows polled into the buffer it hands over.
            cascade::core::HostImage img;
            img.plugin = "Group Thirteen Fixture";
            img.width = 8;
            img.height = 6;
            img.format = 2;
            img.complete = true;
            img.sequence = 17;
            img.pixels.assign(144, 0x5a);
            const std::vector<cascade::core::HostImage> images{img};

            const std::vector<std::string> out = capture(scratch / "status.txt", [&] { e.reportPluginStatus(images); });
            for (const std::string& l : out) { std::printf("  | %s\n", l.c_str()); }
            CHECK(!out.empty());

            const std::string dirNow = Access::pluginDir(e);
            const int status = indexOfPrefix(out, "plugin status: ");
            CHECK(status == 0);  // the first line, as it always was
            if (status >= 0) {
                CHECK(out[static_cast<std::size_t>(status)] ==
                      "plugin status: dir=" + dirNow + " candidates=4 loaded=4 blocked=0 managed=0 unmanaged=4");
            }
            // One inventory note and one host line per module, the host lines in file order, each loaded with no error.
            CHECK(countPrefix(out, "plugin inventory: ") == 5);
            const std::vector<std::pair<std::string, std::string>> modules = {
                {fileOf(FIXTURE_GROUP13_PLUGIN), "Group Thirteen Fixture"},
                {fileOf(FIXTURE_GROUP14_PLUGIN), "Group Fourteen Tracks"},
                {fileOf(FIXTURE_GROUP14B_PLUGIN), "Group Fourteen Tracks B"},
                {fileOf(FIXTURE_STAGE1_PLUGIN), "Stage One Fixture"}};
            int at = indexOfPrefix(out, "plugin host: ");
            CHECK(at > status);
            for (const auto& m : modules) {
                const std::string want = "plugin host: file=" + m.first + " loaded=1 name=" + m.second + " version=1.0.0 error=-";
                CHECK(at >= 0 && at < static_cast<int>(out.size()) && out[static_cast<std::size_t>(at)] == want);
                if (at >= 0 && at < static_cast<int>(out.size()) && out[static_cast<std::size_t>(at)] != want) {
                    std::printf("  FAIL: wanted %s\n        got    %s\n", want.c_str(), out[static_cast<std::size_t>(at)].c_str());
                }
                ++at;
            }
            // The source line follows the host lines (and says the radio is not running: nothing was opened).
            const int source = indexOfPrefix(out, "source status: kind=");
            CHECK(source == at);
            CHECK(source >= 0 && out[static_cast<std::size_t>(source)].find(" running=0 error=") != std::string::npos);
            // The plugin UI after the poll: both track sources' eight aircraft, the two trails, the two panels.
            const int ui = indexOfPrefix(out, "plugin ui: ");
            CHECK(ui == source + 1);
            CHECK(ui >= 0 && out[static_cast<std::size_t>(ui)] == "plugin ui: tracks=8 paths=2 panels=2 tuneRequests=0 tuneGranted=0");
            CHECK(countPrefix(out, "plugin panel: ") == 2);
            CHECK(indexOfPrefix(out, "plugin panel: name=Group Fourteen Tracks title=\"Group Fourteen Panel\" columns=2 rows=3") == ui + 1);
            // THE PICTURE, from the window's buffer, between the panels and the tracks where it always printed.
            const int pic = indexOfPrefix(out, "plugin image: ");
            CHECK(pic == ui + 3);
            CHECK(pic >= 0 && out[static_cast<std::size_t>(pic)] ==
                                  "plugin image: from=Group Thirteen Fixture 8x6 fmt=2 complete=1 seq=17 bytes=144");
            CHECK(countPrefix(out, "plugin track: ") == 8);
            CHECK(indexOfPrefix(out, "plugin track: from=Group Fourteen Tracks id=G14A1 label=F-G14A1 pos=51.6000,-0.6000 kind=1") ==
                  pic + 1);
            CHECK(indexOfPrefix(out, "plugin track: from=Group Fourteen Tracks B id=G14BA3 label=F-G14BA3 pos=51.5000,-0.1000 kind=1") > 0);
            // Stopped (printed even when empty), the mute states, the audio mute (printed even when not muting), the runner.
            const int stopped = indexOfPrefix(out, "plugin stopped: count=0");
            CHECK(stopped == pic + 9);  // (the eight tracks stand between)
            CHECK(countPrefix(out, "plugin mute: ") == 4);
            CHECK(indexOfPrefix(out, "audio mute: muted=0 by=") == stopped + 5);
            CHECK(indexOfPrefix(out, "plugin runner: active=") == stopped + 6);
            // Nothing is blocked, nothing is to be updated, and no line for either.
            CHECK(countPrefix(out, "plugin blocked: ") == 0);
            CHECK(countPrefix(out, "plugin update: ") == 0);
            CHECK(countPrefix(out, "plugin enforce: ") == 0);
        }
    }

    // B: nothing fitted - the counts still print.
    {
        const std::filesystem::path dir = scratch / "empty";
        std::filesystem::create_directories(dir, ec);
        g_pluginDir = dir.string();
        Access::setPluginDir(&pluginDirHook);
        cascade::engine::Engine e;
        e.initialise();
        Access::setPluginDir(nullptr);
        const std::vector<std::string> out =
            capture(scratch / "empty.txt", [&] { e.reportPluginStatus(std::vector<cascade::core::HostImage>{}); });
        CHECK(!out.empty());
        CHECK(!out.empty() && out[0] == "plugin status: dir=" + Access::pluginDir(e) + " candidates=0 loaded=0 blocked=0 managed=0 unmanaged=0");
        CHECK(indexOfPrefix(out, "plugin host: ") < 0);
        CHECK(indexOfPrefix(out, "plugin ui: tracks=0 paths=0 panels=0 tuneRequests=0 tuneGranted=0") > 0);
        CHECK(indexOfPrefix(out, "plugin stopped: count=0") > 0);
        CHECK(indexOfPrefix(out, "audio mute: muted=0 by=") > 0);
        CHECK(countPrefix(out, "plugin image: ") == 0);
    }

    // C: the catalogue report.
    {
        cascade::engine::Engine e;
        e.initialise();

        // A failed fetch: FAILED with the reason, the frame it resolved on, and nothing after it.
        Access::setCatalogue(e, {}, "the store could not be reached");
        std::vector<std::string> out = capture(scratch / "cat_failed.txt", [&] { e.reportPluginTestResult(7); });
        CHECK(out.size() == 1u);
        CHECK(!out.empty() && out[0] == "plugin catalogue: FAILED frame=7 the store could not be reached");

        // A catalogue: the entry count, then one line per entry with the gate's two answers.
        cascade::core::PluginCatalogEntry good;
        good.id = "alpha";
        good.name = "Alpha";
        good.licence = "MIT";
        good.legalNotice = "Read the patent note first";
        good.abiVersion = CASCADE_PLUGIN_ABI_VERSION;
        good.compatible = true;
        cascade::core::PluginPlatform pf;
        pf.os = cascade::core::PluginRepo::hostOs();
        pf.arch = cascade::core::PluginRepo::hostArch();
        pf.file = "alpha.dll";
        pf.url = "https://example.invalid/alpha.dll";
        pf.sha256 = std::string(64, 'a');
        good.platforms.push_back(pf);
        cascade::core::PluginCatalogEntry old = good;
        old.id = "beta";
        old.legalNotice.clear();
        old.abiVersion = CASCADE_PLUGIN_ABI_VERSION + 1;
        old.compatible = false;
        Access::setCatalogue(e, {good, old}, "");
        out = capture(scratch / "cat_ok.txt", [&] { e.reportPluginTestResult(3); });
        for (const std::string& l : out) { std::printf("  | %s\n", l.c_str()); }
        CHECK(out.size() == 3u);
        if (out.size() == 3u) {
            CHECK(out[0] == "plugin catalogue: frame=3 entries=2");
            // The gate WITH the acknowledgement unticked (what the user first sees) and ticked: a notice to read is the one
            // thing between a compatible entry and the download, so unticked it is blocked and ticked it is ok.
            const std::string noAck = Access::blocked(e, 0, false);
            const std::string ack = Access::blocked(e, 0, true);
            CHECK(!noAck.empty());
            CHECK(ack.empty());
            CHECK(out[1] == "plugin entry: id=alpha abi=" + std::to_string(CASCADE_PLUGIN_ABI_VERSION) +
                                " compatible=1 installed=0 legal=1 licence=\"MIT\" install=blocked(" + noAck +
                                ") installAcked=ok");
            // An entry built for another ABI is blocked either way.
            const std::string oldReason = Access::blocked(e, 1, true);
            CHECK(!oldReason.empty());
            CHECK(out[2] == "plugin entry: id=beta abi=" + std::to_string(CASCADE_PLUGIN_ABI_VERSION + 1) +
                                " compatible=0 installed=0 legal=0 licence=\"MIT\" install=blocked(" +
                                Access::blocked(e, 1, false) + ") installAcked=blocked(" + oldReason + ")");
        }
    }

    // D: THE SHIPPED BINARY, as a bounded run drives it. The window keeps the switch (CASCADE_PLUGIN_STATUS, honoured in a --frames
    // run only), calls the engine's report once after the teardown's first phase and hands over the pictures its plugin windows
    // polled: none of that is reachable from the engine alone. The application is COPIED into a scratch folder with three real
    // modules beside it (the folder the host looks in first, when it is writable), so nothing of the build tree or the desk's own
    // plugins is touched, and run twice - with the switch and without: a plain run prints nothing of the report (the byte-identical
    // stdout contract of a --frames run), a run with the switch prints the report once, at the end, with the picture between the
    // panels and the tracks.
    {
        const std::filesystem::path app = scratch / "app";
        std::filesystem::create_directories(app / "plugins", ec);
#if defined(_WIN32)
        const std::string exeName = "cascade.exe";
#else
        const std::string exeName = "cascade";
#endif
        std::filesystem::copy_file(std::filesystem::path(CASCADE_APP_BINDIR) / exeName, app / exeName,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        CHECK(!ec);
        for (const char* dll : {"SoapySDR.dll"}) {
            if (std::filesystem::exists(std::filesystem::path(CASCADE_APP_BINDIR) / dll)) {
                std::filesystem::copy_file(std::filesystem::path(CASCADE_APP_BINDIR) / dll, app / dll,
                                           std::filesystem::copy_options::overwrite_existing, ec);
            }
        }
        for (const char* src : {FIXTURE_STAGE1_PLUGIN, FIXTURE_GROUP13_PLUGIN, FIXTURE_GROUP14_PLUGIN}) {
            std::filesystem::copy_file(src, app / "plugins" / fileOf(src), std::filesystem::copy_options::overwrite_existing, ec);
            CHECK(!ec);
        }
        const std::filesystem::path cfg = scratch / "app_config.json";
        {
            std::ofstream c(cfg, std::ios::binary | std::ios::trunc);
            c << "{}\n";
        }
        engine_api_test::setEnv("CASCADE_CONFIG_TEST", cfg.string());
        engine_api_test::setEnv("FOXSDR_DIAG_DIR", (scratch / "diag").string());
        engine_api_test::setEnv("FOXSDR_CRASH_URL", "http://127.0.0.1:9/");
        engine_api_test::setEnv("FOXSDR_UPDATE_URL", "http://127.0.0.1:9/");
        engine_api_test::setEnv("FOXSDR_REPORTS_URL", "http://127.0.0.1:9/");
        engine_api_test::setEnv("CASCADE_DECODE_TEST", "1");
        const auto run = [&](bool report) {
            engine_api_test::setEnv("CASCADE_PLUGIN_STATUS", report ? "1" : "");
            const std::string cmd = std::string("\"\"") + (app / exeName).string() + "\" --frames 120 2>&1\"";
            std::vector<std::string> lines;
#if defined(_WIN32)
            FILE* pipe = _popen(cmd.c_str(), "r");
#else
            FILE* pipe = popen(cmd.c_str(), "r");
#endif
            constexpr char kLf = 10;
            constexpr char kCr = 13;
            char buf[1024];
            std::string cur;
            while (pipe != nullptr && std::fgets(buf, sizeof(buf), pipe) != nullptr) {
                cur = buf;
                while (!cur.empty() && (cur.back() == kLf || cur.back() == kCr)) { cur.pop_back(); }
                lines.push_back(cur);
            }
#if defined(_WIN32)
            if (pipe != nullptr) { _pclose(pipe); }
#else
            if (pipe != nullptr) { pclose(pipe); }
#endif
            return lines;
        };
        const std::vector<std::string> quiet = run(false);
        std::printf("D: a plain bounded run printed %zu lines, %d of them the report\n", quiet.size(),
                    countPrefix(quiet, "plugin ") + countPrefix(quiet, "source status: ") + countPrefix(quiet, "audio mute: "));
        CHECK(!quiet.empty());
        CHECK(indexOfPrefix(quiet, "cascade: rendered 120 frames") >= 0);
        CHECK(countPrefix(quiet, "plugin ") == 0);
        CHECK(countPrefix(quiet, "source status: ") == 0);
        CHECK(countPrefix(quiet, "audio mute: ") == 0);

        const std::vector<std::string> loud = run(true);
        for (const std::string& l : loud) { std::printf("  | %s\n", l.c_str()); }
        CHECK(countPrefix(loud, "plugin status: dir=") == 1);
        const int status = indexOfPrefix(loud, "plugin status: dir=");
        CHECK(status >= 0 && loud[static_cast<std::size_t>(status)].find(" candidates=3 loaded=3 blocked=0 managed=0 unmanaged=3") != std::string::npos);
        CHECK(countPrefix(loud, "plugin host: ") == 3);
        CHECK(countPrefix(loud, "source status: kind=siggen ") == 1);
        CHECK(countPrefix(loud, "plugin ui: tracks=4 paths=1 panels=1 ") == 1);
        // The picture the window's plugin windows polled, printed by the ENGINE, between the panel and the tracks.
        const int panel = indexOfPrefix(loud, "plugin panel: ");
        const int pic = indexOfPrefix(loud, "plugin image: from=Group Thirteen Fixture ");
        const int track = indexOfPrefix(loud, "plugin track: ");
        CHECK(panel > 0);
        CHECK(pic == panel + 1);
        CHECK(track == pic + 1);
        CHECK(countPrefix(loud, "plugin track: ") == 4);
        // It is the end of the run's own output: after the report comes the teardown's own lines, and nothing of the report
        // before the frames were rendered.
        const int teardown = indexOfPrefix(loud, "cascade: teardown ");
        const int audio = indexOfPrefix(loud, "audio mute: muted=0 by=");
        CHECK(status >= 0 && audio > status && (teardown < 0 || teardown > audio));
        engine_api_test::setEnv("CASCADE_PLUGIN_STATUS", "");
        engine_api_test::setEnv("CASCADE_DECODE_TEST", "");
    }

    std::filesystem::remove_all(scratch, ec);
    return testSummary("test_plugin_status_report");
}
