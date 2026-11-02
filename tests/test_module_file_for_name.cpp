// test_module_file_for_name.cpp - A DISPLAY NAME STANDS FOR THE LOADED COPY OF A PLUGIN, never for a copy the host turned off (the deep pre-merge review, batch B, finding 2;
// docs/engine-stage4.md 6.32).
//
// WHAT WENT WRONG. A plugin window, a map page and a panel's rows carry the plugin's DISPLAY name, and the commands they send - a panel row's key (FOXAPI_OP_PLUGIN_COMMAND), the
// automatic preset when a window is opened (FOXAPP_OP_PLUGIN_AUTO_PRESET), the plugin's settings form - need its MODULE FILE. moduleFileForName (gui/decoder_reads.hpp) answered with
// the FIRST row of the table's PLUGINS list that had the name. The host keeps a name on the copy it turns off when two files declare one (resolveDuplicatePlugins sets loaded =
// false and leaves the name), an update leaves the older file beside the newer until CLEAN UP OLD VERSIONS, and the older file sorts first by path - so after an update the name
// stood for the file that was not running: its registered commands are empty, so the recorder's memory rows and the panel's keys were drawn as text that did nothing; it has no
// settings form, so the plugin's form vanished from its own window; and the automatic preset was sent for a module the host does not run. The comment at the call sites said an
// unloaded module yields an empty result, which is not what happened. Engine::pluginKeyForDisplayName (the engine's twin, which this function re-expresses over the table) had the
// same first-match rule, and its header says "empty when no LOADED plugin answers to the name".
//
//   A  THE RULE ON ROWS: the first LOADED row of the name; an unloaded row never answers, whatever its place; no loaded row, no file; an empty name, no file
//   B  THE ENGINE'S RECORDS: two files of one plugin fitted (1.0.0 and 1.1.0 of one probe, as an update leaves them). The table serves the older, turned-off copy FIRST - the
//      scenario - and the table-side function and the engine's own answer both name the file that runs
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/app_commands.hpp"
#include "engine/engine.hpp"
#include "engine_api_fake_radio.hpp"
#include "gui/app_window.hpp"
#include "gui/decoder_reads.hpp"
#include "imgui.h"
#include "pass_seam.hpp"
#include "test_check.hpp"
#include "test_host.hpp"

#define CHECK_EQ(a, b) CHECK((a) == (b))

namespace {
std::string g_pluginDir;
std::string pluginDir() { return g_pluginDir; }

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

#if defined(_WIN32)
constexpr const char* kExt = ".dll";
#elif defined(__APPLE__)
constexpr const char* kExt = ".dylib";
#else
constexpr const char* kExt = ".so";
#endif
}  // namespace

namespace cascade::gui {
struct AppWindowTestAccess {
    using Engine = cascade::engine::Engine;
    static void install() {
        Engine::testHooks_.makeDevice = &engine_api_test::makeFakeRadio;
        Engine::testHooks_.nativeScan = &engine_api_test::fakeNativeScan;
        Engine::testHooks_.soapyScan = &engine_api_test::noSoapyRadios;
        Engine::testHooks_.pluginDir = &pluginDir;
    }
    // One frame's engine part, as drawUi's look does it: the pass, then the window reads the state (the lists follow listSeq).
    static void look(AppWindow& a) {
        passseam::pass(th::engine(a), [&] {
            th::engine(a).pumpFrameBegin();
            th::engine(a).pumpInputs();
            th::engine(a).pumpPlugins();
            (void)th::engine(a).pumpPublish();
        });
        a.refreshUiState();
    }
    static const std::vector<PluginRow>& rows(AppWindow& a) { return a.pluginLists().plugins; }
    // The engine's own answer for a display name, asked on the pass.
    static std::string engineKeyFor(AppWindow& a, const std::string& name) {
        return passseam::read<std::string>(th::engine(a), [&] { return th::engine(a).pluginKeyForDisplayName(name); });
    }
};
}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;
using cascade::gui::AppWindow;
using cascade::gui::PluginRow;

namespace {

PluginRow row(const char* name, const char* file, bool loaded) {
    PluginRow r;
    r.name = name;
    r.file = file;
    r.loaded = loaded;
    return r;
}

// --- A ---------------------------------------------------------------------------------------------

void testTheRuleOnRows() {
    std::printf("  A: the first LOADED row of a name; an unloaded row never answers\n");
    using cascade::gui::moduleFileForName;
    // THE UPDATE: the older copy (turned off, the name kept) sorts first.
    const std::vector<PluginRow> updated = {row("WEFAX", "wefax-1.0.0.dll", false), row("WEFAX", "wefax-1.1.0.dll", true)};
    CHECK_EQ(moduleFileForName(updated, "WEFAX"), std::string("wefax-1.1.0.dll"));
    // ...and in either order.
    const std::vector<PluginRow> reversed = {row("WEFAX", "wefax-1.1.0.dll", true), row("WEFAX", "wefax-1.0.0.dll", false)};
    CHECK_EQ(moduleFileForName(reversed, "WEFAX"), std::string("wefax-1.1.0.dll"));
    // A name that only an unloaded row has answers nothing: the module is not running, so there is no file to send a command for.
    const std::vector<PluginRow> off = {row("WEFAX", "wefax-1.0.0.dll", false), row("Other", "other.dll", true)};
    CHECK_EQ(moduleFileForName(off, "WEFAX"), std::string());
    // Other plugins' rows are skipped, and the loaded row between two unloaded ones is the answer.
    const std::vector<PluginRow> between = {row("WEFAX", "a-1.0.0.dll", false), row("Other", "other.dll", true), row("WEFAX", "a-1.1.0.dll", true),
                                            row("WEFAX", "a-1.2.0.dll", false)};
    CHECK_EQ(moduleFileForName(between, "WEFAX"), std::string("a-1.1.0.dll"));
    CHECK_EQ(moduleFileForName(between, "Other"), std::string("other.dll"));
    // An empty name, an unknown name and no rows at all answer nothing.
    CHECK_EQ(moduleFileForName(updated, ""), std::string());
    CHECK_EQ(moduleFileForName(updated, "Nothing"), std::string());
    CHECK_EQ(moduleFileForName(std::vector<PluginRow>(), "WEFAX"), std::string());
}

// --- B ---------------------------------------------------------------------------------------------

void testTwoFilesOfOnePlugin() {
    std::printf("  B: two files of one plugin fitted (an update leaves the older beside the newer): the table and the engine name the file that runs\n");
    const std::string older = std::string("wefaxdup-1.0.0") + kExt;
    const std::string newer = std::string("wefaxdup-1.1.0") + kExt;
    std::error_code ec;
    std::filesystem::copy_file(FIXTURE_RESCAN_A_PLUGIN, std::filesystem::path(g_pluginDir) / older, std::filesystem::copy_options::overwrite_existing, ec);
    CHECK(!ec);
    std::filesystem::copy_file(FIXTURE_RESCAN_A_NEWER_PLUGIN, std::filesystem::path(g_pluginDir) / newer, std::filesystem::copy_options::overwrite_existing, ec);
    CHECK(!ec);
    {
        th::TestWindow app;
        Access::look(app);
        Access::look(app);
        const std::vector<PluginRow>& rows = Access::rows(app);
        // THE SCENARIO: the table serves the turned-off older copy FIRST, under the name the newer one answers to.
        std::vector<const PluginRow*> mine;
        for (const PluginRow& r : rows) {
            if (r.file == older || r.file == newer) { mine.push_back(&r); }
        }
        CHECK_EQ(mine.size(), 2u);
        if (mine.size() != 2u) { return; }
        CHECK_EQ(mine[0]->file, older);
        CHECK(!mine[0]->loaded);
        CHECK_EQ(mine[1]->file, newer);
        CHECK(mine[1]->loaded);
        CHECK_EQ(mine[0]->name, mine[1]->name);   // the host keeps the name on the copy it turns off
        CHECK(!mine[0]->name.empty());
        const std::string name = mine[1]->name;
        // THE TABLE'S FUNCTION names the file that runs...
        CHECK_EQ(cascade::gui::moduleFileForName(rows, name), newer);
        // ...and so does the engine's own, over the host's records: the table-side function is that one re-expressed.
        CHECK_EQ(Access::engineKeyFor(app, name), newer);
        CHECK_EQ(Access::engineKeyFor(app, name), cascade::gui::moduleFileForName(rows, name));
        std::printf("      \"%s\": %s is turned off and listed first; the file that runs, %s, is what both answer\n", name.c_str(), older.c_str(), newer.c_str());
    }
    std::filesystem::remove(std::filesystem::path(g_pluginDir) / older, ec);
    std::filesystem::remove(std::filesystem::path(g_pluginDir) / newer, ec);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_module_file_for_name\n");
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    const std::filesystem::path scratch = std::filesystem::temp_directory_path() / ("foxsdr_mfn_" + std::to_string(pid));
    std::filesystem::create_directories(scratch / "plugins");
    g_pluginDir = (scratch / "plugins").string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, scratch.string());
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    ImGui::CreateContext();
    Access::install();

    testTheRuleOnRows();
    testTwoFilesOfOnePlugin();

    ImGui::DestroyContext();
    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
    return testSummary("test_module_file_for_name");
}
