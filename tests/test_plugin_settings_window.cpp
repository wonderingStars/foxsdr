// test_plugin_settings_window.cpp - A PLUGIN'S SETTINGS FORM IN THE REAL WINDOW, against the REAL ENGINE and the REAL settings fixture module (stub campaign B9,
// PLUGIN-SETTINGS-UI; docs/api-0.3-additions.md 9.7). The form's own rules are tests/test_plugin_settings_ui.cpp's (with a sink of its own), the engine's half is
// tests/test_engine_api_settings.cpp's, and the real binary is test_decoder_settings_fold_app / test_plugin_pathpick_app. What only THIS test holds is the
// window's wiring in between, frame by frame in drawUi's own order (the frame's first word on the forms, the pass's front part, the answers taken, the state read,
// the answers acted on, the draw, the frame's last word) with the window's real sink: PLUGIN_SETTINGS and fact.plugin.setting.<id> in, FOXAPP_OP_PLUGIN_SETTING_EDIT
// out, the ticketed answer back to the box that sent it.
//
//   A  the table: the window reads the module's four fields (key, label, kind, longest value) and what each shows;
//   B  COMMIT ON LEAVE ACROSS THE REAL ROUND TRIP: "G4" then "A" typed - nothing is sent, nothing reaches the engine; Enter sends ONE command, whole; the box keeps
//      its text while the command is on its way; the engine applies it on the next pass, the plugin's settings_seq moves ONCE, the answer reaches the box and the box
//      never shows the old text in between; a settled form sends nothing more;
//   C  A REFUSED EDIT SNAPS BACK AFTER THE ANSWER, NOT BEFORE: "12x" into the number - sent, kept in the box while it waits, then the engine's verdict (NotANumber)
//      reaches the box, which says so and goes back to the stored "5"; the plugin's settings_seq did not move;
//   D  A SURFACE THAT STOPS DRAWING A BOX MID-EDIT IS FLUSHED BY THE FRAME'S LAST WORD, through the same command, once - and the engine's answer reaches a form that
//      is not drawn.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/app_commands.hpp"
#include "core/plugin_api.hpp"
#include "engine/engine.hpp"
#include "engine_api_fake_radio.hpp"
#include "gui/app_window.hpp"
#include "test_host.hpp"
#include "gui/fonts.hpp"
#include "gui/plugin_settings_form.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "pass_seam.hpp"
#include "test_check.hpp"

namespace cmd = cascade::core::cmd;
using cascade::core::SettingEditResult;
using cascade::gui::AppWindow;

namespace {
std::string g_pluginDir;
std::string pluginDir() { return g_pluginDir; }

#if defined(_WIN32)
const char* const kExt = ".dll";
#elif defined(__APPLE__)
const char* const kExt = ".dylib";
#else
const char* const kExt = ".so";
#endif
const std::string kFile = std::string("settings_ui_fixture") + kExt;  // the module's file name: what PLUGINS lists and the edit names
const std::string kName = "Settings Form Fixture";                    // its declared name: the settings store's key
const std::string kUs(1, '\x1F');
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
    static Engine& eng(AppWindow& a) { return th::engine(a); }
    // THE FRAME'S LOOK, in drawUi's order: the pass's front part, the session's answers taken, the state read, the answers acted on. `runPass` false is a
    // frame in which the engine's pass did not run (a pass that is busy: the command waits at the door and no answer comes) - the window still looks.
    static void look(AppWindow& a, bool runPass = true) {
        if (runPass) {
            passseam::pass(eng(a), [&] {
                eng(a).pumpFrameBegin();
                eng(a).pumpInputs();
                eng(a).pumpPlugins();
                (void)eng(a).pumpPublish();
            });
        }
        a.statusLines_ = eng(a).statusText();
        a.pollUiAnswers();
        a.refreshUiState();
        a.actOnUiAnswers();
    }
    static void after(AppWindow& a) {
        passseam::workers(eng(a), [&] {
            eng(a).pumpWorkers();
            eng(a).pumpAudio();
        });
    }
    static void beginFrame(AppWindow& a) { a.settingsFormsBeginFrame(); }
    static void flush(AppWindow& a) { a.settingsFormsFlush(); }
    static void drawForm(AppWindow& a, const std::string& file) { a.drawPluginSettingsFields(file, "test"); }
    static const PluginSettingsLists& lists(AppWindow& a) { return a.settingsLists(); }
    static bool hasSettings(AppWindow& a, const std::string& file) { return a.pluginHasSettings(file); }
    static PluginSettingsFormState& forms(AppWindow& a) { return a.pluginSettingsForms_; }
    static void tap(AppWindow& a, std::vector<cmd::QueuedCommand>* log) { a.uiSubmitTap_ = log; }
    static std::string fact(AppWindow& a, const char* key) {
        std::string s;
        a.readFactText(key, s);
        return s;
    }
    // The plugin's own view of the store: the counter its settings_seq reads, and the value it would read back.
    static std::uint64_t settingsSeq(AppWindow& a) { return eng(a).pluginApi_->settingsUiSeq(kName); }
    static std::string stored(AppWindow& a, const char* key) {
        std::string v;
        return eng(a).pluginApi_->settingsUiGet(kName, key, v) == CASCADE_API_OK ? v : std::string("<unset>");
    }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;

namespace {

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

// The edits among the commands the window sent, each as the text the engine will read.
std::vector<std::string> edits(const std::vector<cmd::QueuedCommand>& log) {
    std::vector<std::string> out;
    for (const cmd::QueuedCommand& q : log) {
        if (q.c.op == FOXAPP_OP_PLUGIN_SETTING_EDIT) { out.push_back(cmd::textOf(q.c, q.longText)); }
    }
    return out;
}

// One window the test draws the form in, a frame at a time in drawUi's order.
struct Win {
    AppWindow& a;
    int focus = -1;  // SetKeyboardFocusHere(focus) on the next frame
    std::vector<cmd::QueuedCommand> log;
    explicit Win(AppWindow& w) : a(w) { Access::tap(a, &log); }
    ~Win() { Access::tap(a, nullptr); }

    void frame(bool drawForm = true, bool runPass = true) {
        Access::beginFrame(a);        // the first word on the forms
        Access::look(a, runPass);     // the pass, the answers, the state
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(500.0f, 600.0f), ImGuiCond_Always);
        ImGui::Begin("form", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings);
        if (focus >= 0) {
            ImGui::SetKeyboardFocusHere(focus);
            focus = -1;
        }
        if (drawForm) { Access::drawForm(a, kFile); }
        ImGui::End();
        ImGui::Render();
        Access::flush(a);  // the last word
        Access::after(a);
    }
    void frames(int n, bool drawForm = true, bool runPass = true) {
        for (int i = 0; i < n; ++i) { frame(drawForm, runPass); }
    }
    cascade::gui::SettingFieldState& field(const char* key) {
        return Access::forms(a).fields[cascade::gui::settingFieldStateKey("test", kFile, key)];
    }
    std::string box(const char* key) {
        const auto& f = field(key);
        return f.buf.empty() ? std::string("<none>") : std::string(f.buf.data());
    }
    void type(const char* text) {
        ImGui::GetIO().AddInputCharactersUTF8(text);
        frame();
    }
    // Enter pressed for one frame: the box is LEFT in it, and the edit is sent in it.
    void enterDown() {
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, true);
        frame();
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Enter, false);
    }
    void selectAll() {
        ImGuiIO& io = ImGui::GetIO();
        io.AddKeyEvent(ImGuiMod_Ctrl, true);
        io.AddKeyEvent(ImGuiKey_A, true);
        frame();
        io.AddKeyEvent(ImGuiKey_A, false);
        io.AddKeyEvent(ImGuiMod_Ctrl, false);
        frame();
    }
};

// ---------------------------------------------------------------------------------------------------------------------------------------------

void testTable(AppWindow& a) {
    std::printf("[A] the window reads the module's form from the table\n");
    Win w(a);
    w.frames(4);
    const cascade::gui::PluginSettingsLists& lists = Access::lists(a);
    CHECK(lists.sets.size() == 1u);
    CHECK(Access::hasSettings(a, kFile));
    CHECK(!Access::hasSettings(a, "nobody.dll"));
    if (lists.sets.size() != 1u) { return; }
    const cascade::gui::PluginSettingsSet& set = lists.sets[0];
    CHECK(set.file == kFile);
    CHECK(set.fields.size() == 4u && set.ids.size() == 4u);
    if (set.fields.size() != 4u) { return; }
    const char* const keys[4] = {"callsign", "power", "tool_path", "report"};
    const char* const labels[4] = {"Callsign", "Power (W)", "Tool path", "Report spots"};
    const std::uint32_t kinds[4] = {CASCADE_SETTING_TEXT, CASCADE_SETTING_NUMBER, CASCADE_SETTING_PATH, CASCADE_SETTING_BOOL};
    for (std::size_t i = 0; i < 4; ++i) {
        CHECK(set.fields[i].key == keys[i]);
        CHECK(set.fields[i].label == labels[i]);
        CHECK(set.fields[i].kind == kinds[i]);
        CHECK(set.ids[i] == i + 1u);
    }
    CHECK(set.fields[0].maxLength == 12u && set.fields[0].placeholder == "e.g. G4ABC");
    CHECK(set.fields[2].maxLength == 260u);
    CHECK(w.box("callsign").empty());  // nothing stored yet: the default ""
    CHECK(w.box("power") == "5");      // ...and the number's default
}

void testCommitOnLeave(AppWindow& a) {
    std::printf("[B] commit on leave across the real round trip\n");
    Win w(a);
    w.frames(3);
    const std::uint64_t seq0 = Access::settingsSeq(a);

    // TYPING SENDS NOTHING: "G4", then "A" - two keystrokes in two frames, the box still being typed in.
    w.focus = 0;
    w.frames(2);
    w.selectAll();
    w.type("G4");
    w.type("A");
    CHECK(edits(w.log).empty());
    CHECK(w.box("callsign") == "G4A");
    CHECK(Access::settingsSeq(a) == seq0);  // the plugin saw nothing, not "G4" and not "G4A"
    w.frames(3);                            // still being typed in: still nothing
    CHECK(edits(w.log).empty());

    // LEAVING THE BOX SENDS IT: ONE command, the whole of the text, naming the module's FILE and the field's key.
    w.enterDown();
    CHECK(edits(w.log).size() == 1u);
    CHECK(!edits(w.log).empty() && edits(w.log)[0] == kFile + kUs + "callsign" + kUs + "G4A");
    CHECK(w.field("callsign").pending);       // sent, not answered
    CHECK(w.box("callsign") == "G4A");        // the box keeps what was typed
    CHECK(Access::settingsSeq(a) == seq0);    // the engine has not applied it: the pass that does is the next frame's
    // THE PASS IS BUSY FOR A WHILE (five frames in which it does not run): the command waits at the door, no answer comes, and the box - drawn every one of
    // those frames - keeps what was typed instead of snapping back to a store that has not changed; nothing is sent again.
    for (int i = 0; i < 5; ++i) {
        w.frame(true, /*runPass=*/false);
        CHECK(w.field("callsign").pending);
        CHECK(w.box("callsign") == "G4A");
        CHECK(Access::settingsSeq(a) == seq0);
    }
    CHECK(edits(w.log).size() == 1u);

    // THE NEXT FRAME: the pass applies the command, answers it and publishes; the answer and the state are read; the form takes the verdict.
    w.frame();
    CHECK(Access::settingsSeq(a) == seq0 + 1u);  // the plugin sees settings_seq move ONCE
    CHECK(Access::stored(a, "callsign") == "G4A");
    CHECK(!w.field("callsign").pending);
    CHECK(w.field("callsign").last == SettingEditResult::Accepted);
    // THE BOX NEVER SHOWS THE OLD TEXT, not in the frame the answer came in nor in any after it - and nothing is sent again.
    for (int i = 0; i < 40; ++i) {
        CHECK(w.box("callsign") == "G4A");
        w.frame();
    }
    CHECK(w.box("callsign") == "G4A");
    CHECK(Access::fact(a, "fact.plugin.setting.1") == "G4A");  // what the table now says the field shows
    CHECK(w.field("callsign").hold == 0);                       // and the box is back on the store's text, not held to ours
    CHECK(edits(w.log).size() == 1u);
    CHECK(Access::settingsSeq(a) == seq0 + 1u);
}

void testRefusedSnapsBack(AppWindow& a) {
    std::printf("[C] a refused edit snaps back after the answer, not before\n");
    Win w(a);
    w.frames(3);
    const std::uint64_t seq0 = Access::settingsSeq(a);
    w.focus = 1;
    w.frames(2);
    w.selectAll();
    w.type("12x");
    CHECK(w.box("power") == "12x");
    w.enterDown();
    CHECK(edits(w.log).size() == 1u);
    CHECK(!edits(w.log).empty() && edits(w.log)[0] == kFile + kUs + "power" + kUs + "12x");
    // SENT, NOT ANSWERED: the box keeps "12x" and says nothing is wrong yet.
    CHECK(w.field("power").pending);
    CHECK(w.box("power") == "12x");
    CHECK(w.field("power").last == SettingEditResult::Accepted);
    for (int i = 0; i < 5; ++i) {  // ...for as many frames as the pass takes to answer
        w.frame(true, /*runPass=*/false);
        CHECK(w.field("power").pending);
        CHECK(w.box("power") == "12x");
        CHECK(w.field("power").last == SettingEditResult::Accepted);
    }
    // THE ANSWER: the engine judged it - a number that is not one - and the box says so and goes back to what the store holds.
    w.frame();
    CHECK(!w.field("power").pending);
    CHECK(w.field("power").last == SettingEditResult::NotANumber);
    CHECK(w.box("power") == "5");
    CHECK(Access::stored(a, "power") == "<unset>");
    CHECK(Access::settingsSeq(a) == seq0);  // a refusal moves nothing
    w.frames(4);
    CHECK(w.box("power") == "5");
    CHECK(edits(w.log).size() == 1u);
}

void testFlushOfAVanishedSurface(AppWindow& a) {
    std::printf("[D] a surface that stops drawing a box mid-edit is flushed by the frame's last word\n");
    Win w(a);
    w.frames(3);
    const std::uint64_t seq0 = Access::settingsSeq(a);
    w.focus = 0;
    w.frames(2);
    w.selectAll();
    w.type("G7XYZ");
    CHECK(edits(w.log).empty());
    CHECK(Access::settingsSeq(a) == seq0);

    // THE SURFACE VANISHES: nothing draws the form again - a window closed, a fold shut, a node deselected. The frame's last word commits the box.
    w.frame(/*drawForm=*/false);
    CHECK(edits(w.log).size() == 1u);
    CHECK(!edits(w.log).empty() && edits(w.log)[0] == kFile + kUs + "callsign" + kUs + "G7XYZ");
    CHECK(w.field("callsign").pending);
    // THE ANSWER REACHES A FORM THAT IS NOT DRAWN: the pass applies the edit, and the verdict is taken by the box's state all the same.
    w.frame(false);
    CHECK(Access::settingsSeq(a) == seq0 + 1u);
    CHECK(Access::stored(a, "callsign") == "G7XYZ");
    CHECK(!w.field("callsign").pending);
    CHECK(w.field("callsign").last == SettingEditResult::Accepted);
    // ONCE: a second flush has nothing to send.
    w.frames(3, false);
    CHECK(edits(w.log).size() == 1u);
    // ...and the form, drawn again, shows what was stored.
    w.frames(3);
    CHECK(w.box("callsign") == "G7XYZ");
    CHECK(edits(w.log).size() == 1u);
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_plugin_settings_window\n");
    if (argc < 2) {
        std::printf("usage: test_plugin_settings_window <settings fixture module>\n");
        return 2;
    }
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    const std::filesystem::path scratch = std::filesystem::temp_directory_path() /
                                          ("foxsdr_settingswin_" + std::to_string(pid) + "_" + passseam::name());
    std::filesystem::create_directories(scratch / "plugins");
    g_pluginDir = (scratch / "plugins").string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, scratch.string());
    }
    for (const char* v : {"FOXSDR_TELEMETRY_URL", "FOXSDR_CRASH_URL", "FOXSDR_UPDATE_URL"}) { setEnv(v, "http://127.0.0.1:9"); }

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1600.0f, 1000.0f);
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    CHECK(cascade::gui::fonts::load());
    Access::install();

    // THE MODULE goes into the directory BEFORE the window is made (the host scans at construction).
    std::error_code ec;
    std::filesystem::copy_file(std::filesystem::path(argv[1]), std::filesystem::path(g_pluginDir) / kFile,
                               std::filesystem::copy_options::overwrite_existing, ec);
    CHECK(!ec);
    {
        auto app = std::make_unique<th::TestWindow>();
        AppWindow& a = *app;
        for (int i = 0; i < 4; ++i) { Access::look(a); Access::after(a); }
        testTable(a);
        testCommitOnLeave(a);
        testRefusedSnapsBack(a);
        testFlushOfAVanishedSurface(a);
    }
    ImGui::DestroyContext();
    std::filesystem::remove_all(scratch, ec);
    return testSummary("test_plugin_settings_window");
}
