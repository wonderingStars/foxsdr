// Opening a map must not allocate a second native GL window; explicitly
// dragging it out may. Real application/track plugin and recorded window
// rectangles, with isolated settings.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif
#include "test_check.hpp"
namespace fs = std::filesystem;
namespace {
fs::path scratch;
void env(const char* key, const std::string& value) {
#if defined(_WIN32)
    SetEnvironmentVariableA(key, value.empty() ? nullptr : value.c_str());
    _putenv_s(key, value.c_str());
#else
    if (value.empty()) {
        unsetenv(key);
    } else {
        setenv(key, value.c_str(), 1);
    }
#endif
}
int processId() {
#if defined(_WIN32)
    return static_cast<int>(GetCurrentProcessId());
#else
    return static_cast<int>(getpid());
#endif
}
void write(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << text;
}
std::string read(const fs::path& path) {
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in), {});
}
struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool found = false;
};
Rect rect(const fs::path& path, const std::string& name) {
    std::istringstream in(read(path));
    std::string line;
    while (std::getline(in, line)) {
        if (line.find(name) == std::string::npos) {
            continue;
        }
        Rect r;
        std::istringstream row(line);
        row >> r.x >> r.y >> r.w >> r.h;
        r.found = !row.fail();
        return r;
    }
    return {};
}
Rect censusRect(const fs::path& path, const std::string& name) {
    std::istringstream lines(read(path));
    std::string line;
    while (std::getline(lines, line)) {
        if (line.rfind("rect " + name + " ", 0) != 0) {
            continue;
        }
        std::istringstream values(line.substr(5 + name.size()));
        Rect r;
        float right = 0, bottom = 0;
        values >> r.x >> r.y >> right >> bottom;
        r.found = !values.fail();
        r.w = right - r.x;
        r.h = bottom - r.y;
        return r;
    }
    return {};
}
bool inside(Rect p, Rect r) {
    return p.found && r.found && p.x >= r.x - 0.5f && p.y >= r.y - 0.5f &&
           p.x + p.w <= r.x + r.w + 0.5f && p.y + p.h <= r.y + r.h + 0.5f;
}
struct Run {
    Rect root, map;
    std::string log;
    fs::path dir;
};
Run run(const char* tag, bool saved, bool small,
        const std::string& script = "") {
    Run r;
    r.dir = scratch / tag;
    fs::create_directories(r.dir / "shots");
    std::string cfg =
        "{\"railBank\":1,\"sourceKind\":\"siggen\",\"diagnosticsEnabled\":true,"
        "\"telemetryEnabled\":false,\"updateCheckEnabled\":false,"
        "\"pluginCatalogueUrl\":\"http://127.0.0.1:9/\"";
    if (saved) {
        cfg += ",\"mapPages\":[{\"plugin\":\"Track "
               "Probe\",\"x\":1800,\"y\":100,\"width\":1583,\"height\":964}]";
    }
    cfg += "}";
    write(r.dir / "config.json", cfg);
    env("CASCADE_CONFIG_TEST", (r.dir / "config.json").string());
    env("FOXSDR_DIAG_DIR", (r.dir / "diag").string());
    env("FOXSDR_SHOT_DIR", (r.dir / "shots").string());
    env("FOXSDR_UI_CENSUS", (r.dir / "census.txt").string());
    env("FOXSDR_CENSUS_HOLD_BANK", "1");
    env("FOXSDR_WINDOW_SIZE", small ? "640x480" : "1280x720");
    env("FOXSDR_TEST_VIEWPORT_REFUSE_AT",
        std::string(tag) == "refused" ? "90" : "");
    env("FOXSDR_INPUT_SCRIPT",
        script.empty() ? "" : (r.dir / "input.txt").string());
    if (!script.empty()) {
        write(r.dir / "input.txt", script);
    }
#if defined(_WIN32)
    const std::string exe = std::string(CASCADE_APP_BINDIR) + "/cascade.exe";
#else
    const std::string exe = std::string(CASCADE_APP_BINDIR) + "/cascade";
#endif
#if defined(_WIN32)
    const std::string cmd = "\"\"" + exe + "\" --frames 160 > \"" +
                            (r.dir / "stdout.txt").string() + "\" 2>&1\"";
#else
    const std::string cmd = "\"" + exe + "\" --frames 160 > \"" +
                            (r.dir / "stdout.txt").string() + "\" 2>&1";
#endif
    int status = std::system(cmd.c_str());
    CHECK(status == 0);
    r.root = rect(r.dir / "shots/shot-50.windows.txt", "##cascade_root");
    r.map = rect(r.dir / "shots/shot-50.windows.txt", "###mapPage:Track Probe");
    r.log = read(r.dir / "diag/logs/foxsdr.log");
    CHECK(r.root.found);
    CHECK(r.map.found);
    return r;
}
} // namespace
int main(int argc, char** argv) {
    if (argc != 2) {
        std::printf("fixture path required\n");
        return 2;
    }
    scratch = fs::temp_directory_path() /
              ("fox-map-open-" + std::to_string(processId()));
    fs::create_directories(scratch / "local/foxsdr/plugins");
#if defined(_WIN32)
    fs::copy_file(argv[1], scratch / "local/foxsdr/plugins/track_probe.dll");
#elif defined(__APPLE__)
    fs::copy_file(argv[1], scratch / "local/foxsdr/plugins/track_probe.dylib");
#else
    fs::copy_file(argv[1], scratch / "local/foxsdr/plugins/track_probe.so");
#endif
    env("XDG_CONFIG_HOME", (scratch / "appdata").string());
    env("XDG_DATA_HOME", (scratch / "local").string());
    env("XDG_STATE_HOME", (scratch / "local").string());
    env("APPDATA", (scratch / "appdata").string());
    env("LOCALAPPDATA", (scratch / "local").string());
    env("FOXSDR_FAKE_PACKAGE", "FoxSDR.StoreTest_1.0.0.0_x64__test");
    env("FOXSDR_OPEN_MAP", "Track Probe");
    env("FOXSDR_SHOT_AT_FRAME", "50,80,100,150");
    env("FOXSDR_WINDOW_POS", "100,100");
    env("FOXSDR_SINGLE_VIEWPORT", "0");
    for (const char* name :
         {"FOXSDR_TELEMETRY_URL", "FOXSDR_CRASH_URL", "FOXSDR_UPDATE_URL",
          "FOXSDR_REPORTS_URL", "FOXSDR_REGIONAL_CATALOGUE_URL"}) {
        env(name, "http://127.0.0.1:9/");
    }
    const Run fresh = run("fresh", false, false);
    CHECK(inside(fresh.map, fresh.root));
    CHECK(fresh.log.find("page \"Track Probe Map\" in its own window") ==
          std::string::npos);
    const Run saved = run("saved", true, false);
    CHECK(inside(saved.map, saved.root));
    CHECK(saved.log.find("page \"Track Probe Map\" in its own window") ==
          std::string::npos);
    const Run small = run("small", true, true);
    CHECK(inside(small.map, small.root));
    CHECK(small.map.w >= 240 && small.map.h >= 140);
    // Operate the real page caption through the app's own scripted ImGui input
    // (no OS mouse).
    std::ostringstream input;
    input << "60 screen " << fresh.map.x + 150 << " " << fresh.map.y + 10
          << "\n61 down\n65 screen " << fresh.root.x + fresh.root.w + 220 << " "
          << fresh.map.y + 30 << "\n70 up\n";
    const Run drag = run("drag", false, false, input.str());
    const Rect dragged =
        rect(drag.dir / "shots/shot-100.windows.txt", "###mapPage:Track Probe");
    CHECK(dragged.found);
    CHECK(!inside(dragged, drag.root));
    CHECK(drag.log.find("page \"Track Probe Map\" in its own window") !=
          std::string::npos);
    const Run refused = run("refused", false, false, input.str());
    const Rect before = rect(refused.dir / "shots/shot-80.windows.txt",
                             "###mapPage:Track Probe");
    const Rect after = rect(refused.dir / "shots/shot-100.windows.txt",
                            "###mapPage:Track Probe");
    CHECK(before.found);
    CHECK(!inside(before, refused.root));
    // Disabling platform viewports changes ImGui coordinates to main-window
    // local coordinates; compare both rectangles from the same frame.
    const Rect afterRoot =
        rect(refused.dir / "shots/shot-100.windows.txt", "##cascade_root");
    CHECK(inside(after, afterRoot));
    CHECK(refused.log.find(
              "page \"Track Probe Map\" brought inside the main window") !=
          std::string::npos);
    // After a deliberate tear-off, close and reopen through the DECODE
    // bank's actual map switch, located by the UI census. This main-window
    // control needs no native-window input or operating-system pointer.
    const Rect mapSwitch =
        censusRect(fresh.dir / "census.txt", "switchrow:Track Probe map");
    CHECK(mapSwitch.found);
    std::ostringstream reopenScript;
    reopenScript << input.str() << "80 screen "
                 << mapSwitch.x + mapSwitch.w * 0.5f << " "
                 << mapSwitch.y + mapSwitch.h * 0.5f
                 << "\n82 down\n83 up\n110 screen "
                 << mapSwitch.x + mapSwitch.w * 0.5f << " "
                 << mapSwitch.y + mapSwitch.h * 0.5f << "\n112 down\n113 up\n";
    const Run reopened = run("reopened", false, false, reopenScript.str());
    CHECK(!rect(reopened.dir / "shots/shot-100.windows.txt",
                "###mapPage:Track Probe")
               .found);
    const Rect reopenedMap = rect(reopened.dir / "shots/shot-150.windows.txt",
                                  "###mapPage:Track Probe");
    const Rect reopenedRoot =
        rect(reopened.dir / "shots/shot-150.windows.txt", "##cascade_root");
    CHECK(inside(reopenedMap, reopenedRoot));
    CHECK(reopenedMap.w == fresh.map.w);
    CHECK(reopenedMap.h == fresh.map.h);
    const std::string native = "page \"Track Probe Map\" in its own window";
    const auto firstNative = reopened.log.find(native);
    CHECK(firstNative != std::string::npos);
    CHECK(firstNative == std::string::npos ||
          reopened.log.find(native, firstNative + native.size()) ==
              std::string::npos);
    std::printf("artifacts: %s\n", scratch.string().c_str());
    return testSummary("test_map_open_inside_app");
}