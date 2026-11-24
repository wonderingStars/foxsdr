// test_refusal_popup_app.cpp - A REFUSED RADIO CHOICE SAYS WHY, in the CLASSIC window (0.99.113): the window's side, against the real engine on a fake radio.
//
// The saved patch has a Radio part that names the radio, the patch is stopped (the owner's case: his B200). The window's Source list click (AppWindow::submitSourceChoice - the
// Selectable's own call) is refused FOXAPI_BUSY. This holds that:
//   A  the Source error line (fact.status.sourceError, which the window letters in the Source section) carries the sentence naming the part;
//   B  the window raises its popup ONCE, for the answer to ITS OWN click, with the radio's label, the part's number and the two keys' words, and the modal draws in a frame
//      (it is the popup that is open in ImGui, not only the state);
//   C  "Open the Patch page" shows the Patch view (patchOpen_) and closes the popup; "Close" closes it and leaves the view where it was; neither touches the patch;
//   D  NOT for a command sent by another session (its answer never reaches this window), nor for a refusal whose ticket the window did not note, nor for another refusal;
//   E  no failed open is counted and the hand-off question (fact.radio.handoff) is not raised.
// The Selectable itself (an ImGui click) is not driven here: it is one line that calls submitSourceChoice.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/app_commands.hpp"
#include "core/patch_devices.hpp"
#include "core/patch_graph.hpp"
#include "core/radio_refusal_popup.hpp"
#include "engine/engine.hpp"
#include "engine_api_fake_radio.hpp"
#include "gui/app_window.hpp"
#include "test_host.hpp"
#include "gui/fonts.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "pass_seam.hpp"
#include "test_check.hpp"

#define CHECK_EQ(a, b) CHECK((a) == (b))

namespace pc = cascade::core::patch;
namespace rf = cascade::core::refusal;
using cascade::gui::AppWindow;

namespace cascade::gui {

struct AppWindowTestAccess {
    using Engine = cascade::engine::Engine;
    static void install() {
        Engine::testHooks_.makeDevice = &engine_api_test::makeFakeRadio;
        Engine::testHooks_.nativeScan = &engine_api_test::fakeNativeScan;
        Engine::testHooks_.soapyScan = &engine_api_test::noSoapyRadios;
    }
    static Engine& eng(AppWindow& a) { return th::engine(a); }
    // The frame's look, in drawUi's order: the pass, the answers taken, the state read, the answers acted on.
    static void look(AppWindow& a) {
        passseam::pass(eng(a), [&] {
            eng(a).pumpFrameBegin();
            eng(a).pumpInputs();
            eng(a).pumpPlugins();
            (void)eng(a).pumpPublish();
        });
        a.statusLines_ = eng(a).statusText();
        a.pollUiAnswers();
        a.refreshUiState();
        a.actOnUiAnswers();
        passseam::workers(eng(a), [&] {
            eng(a).pumpWorkers();
            eng(a).pumpAudio();
        });
    }
    static std::uint32_t addPart(AppWindow& a, const std::string& device) {
        std::uint32_t id = 0;
        passseam::on(eng(a), [&] {
            id = eng(a).patchGraph_.addNode(pc::NodeKind::Radio, "Aerial B200");
            pc::Node* n = eng(a).patchGraph_.mutableNode(id);
            n->device = device;
            n->on = false;
        });
        return id;
    }
    static void choose(AppWindow& a, const std::string& id, const std::string& label) { a.submitSourceChoice(id, label); }
    static void submitUnnoted(AppWindow& a, const std::string& id) { a.uiSubmit(cascade::core::cmd::makeText(FOXAPI_OP_SELECT_SOURCE, id)); }
    static rf::RefusalPopup& popup(AppWindow& a) { return a.refusalPopup(); }
    static const rf::Popup& text(AppWindow& a) { return a.refusalPopupText_; }
    static bool openRequested(AppWindow& a) { return a.refusalPopupOpenRequest_; }
    static bool patchOpen(AppWindow& a) { return a.patchOpen_; }
    static void setPatchOpen(AppWindow& a, bool on) { a.patchOpen_ = on; }
    static std::string fact(AppWindow& a, const char* key) {
        std::string s;
        a.readFactText(key, s);
        return s;
    }
    static std::uint64_t failures(AppWindow& a) { return passseam::read<std::uint64_t>(eng(a), [&] { return eng(a).radioOpenFailures_; }); }
    static std::size_t partCount(AppWindow& a) { return passseam::read<std::size_t>(eng(a), [&] { return eng(a).patchGraph_.nodes().size(); }); }
    static void drawModal(AppWindow& a) {
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        a.drawRefusalPopup();
        ImGui::Render();
    }
    // A second session's SELECT_SOURCE through the table: its answer goes to that session's queue, never to the window's.
    static std::int32_t otherSessionChoice(AppWindow& a, const std::string& id) {
        const FoxEngineApi* api = cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR);
        FoxSession* s = nullptr;
        FoxSessionParams p{};
        p.structSize = sizeof(p);
        p.flags = FOXAPI_SESSION_LOCAL;
        p.grants = FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS;
        p.clientName = "other";
        p.clientVersion = "0";
        if (api->open_session(eng(a).apiHandle(), &p, &s) != FOXAPI_OK) { return -1000; }
        FoxCommand c = cascade::core::cmd::makeText(FOXAPI_OP_SELECT_SOURCE, id).c;
        FoxSubmitResult sr{};
        sr.structSize = sizeof(sr);
        api->submit(s, &c, 1, &sr);
        std::int32_t status = -1001;
        for (int i = 0; i < 6 && status == -1001; ++i) {
            look(a);
            FoxCommandResult r[4];
            for (auto& x : r) { x.structSize = sizeof(x); }
            const int32_t got = api->poll_results(s, r, 4);
            for (int32_t k = 0; k < got; ++k) { if (r[k].ticket == sr.ticket) { status = r[k].status; } }
        }
        api->close_session(s);
        return status;
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

void looks(AppWindow& a, int n) {
    for (int i = 0; i < n; ++i) { Access::look(a); }
}

std::string partSentence(const std::string& caption) {
    return "This radio is set as the radio of the part \"" + caption +
           "\" on the Patch page. Change or remove that part, or start the patch to use the radio there.";
}

void testTheClassicWindow(AppWindow& a) {
    looks(a, 4);
    const std::string key = pc::makeDeviceKey(engine_api_test::kFakeRadioDriver, engine_api_test::kFakeRadioArgs);
    const std::uint32_t part = Access::addPart(a, key);
    looks(a, 3);
    const std::uint64_t failsBefore = Access::failures(a);
    const std::size_t partsBefore = Access::partCount(a);

    // D (first): a command another session sent is refused and answered to THAT session: nothing here.
    CHECK_EQ(Access::otherSessionChoice(a, engine_api_test::kFakeRadioId), static_cast<std::int32_t>(FOXAPI_BUSY));
    looks(a, 3);
    CHECK(!Access::popup(a).open() && Access::popup(a).shown() == 0 && !Access::openRequested(a));

    // D: a refusal of a choice the window sent but did not note (a script on the window's own session) is not the person's click.
    Access::submitUnnoted(a, engine_api_test::kFakeRadioId);
    looks(a, 4);
    CHECK(!Access::popup(a).open() && Access::popup(a).shown() == 0);
    // D: another kind of refusal of a noted click raises nothing ("no scanned device matches" is NOT_FOUND, not BUSY).
    Access::choose(a, "nosuchrow", "Some other radio");
    looks(a, 4);
    CHECK(!Access::popup(a).open() && Access::popup(a).shown() == 0);

    // A, B: the person's own click, refused because the part names the radio.
    Access::choose(a, engine_api_test::kFakeRadioId, "RTL-SDR (serial ending 0001)");
    looks(a, 4);
    CHECK_EQ(Access::fact(a, "fact.status.sourceError"), partSentence("Aerial B200"));
    CHECK(Access::popup(a).open() && Access::popup(a).shown() == 1 && Access::popup(a).part() == static_cast<int>(part));
    CHECK(Access::openRequested(a));
    const rf::Popup& p = Access::text(a);
    CHECK_EQ(p.title, std::string("Radio in use"));
    CHECK(p.text.find("RTL-SDR (serial ending 0001)") == 0);
    CHECK_EQ(p.text, std::string("RTL-SDR (serial ending 0001) is set as the radio of the part \"Aerial B200\" on the Patch page. Change or remove that part, or start the patch to use the radio there."));
    CHECK_EQ(p.goLabel, std::string("Open the Patch page"));
    CHECK_EQ(p.stayLabel, std::string("Close"));
    // the modal draws: in the frame it is opened in ImGui, not only held in the state
    Access::drawModal(a);
    CHECK(ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel));
    CHECK(!Access::openRequested(a));
    Access::drawModal(a);   // a second frame: still the same popup, not raised again
    CHECK_EQ(Access::popup(a).shown(), 1);

    // E: nothing counted, nothing asked, nothing changed
    CHECK_EQ(Access::failures(a), failsBefore);
    CHECK_EQ(Access::fact(a, "fact.radio.handoff"), std::string());
    CHECK_EQ(Access::partCount(a), partsBefore);

    // C: "Close" closes it and leaves the view where it was
    Access::setPatchOpen(a, false);
    Access::popup(a).dismiss();
    CHECK(!Access::popup(a).open() && !Access::patchOpen(a));
    Access::drawModal(a);   // the modal finds the state closed and closes with it
    Access::drawModal(a);
    CHECK(!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel));

    // a second refused click shows it again; "Open the Patch page" shows the Patch view and closes it
    Access::choose(a, engine_api_test::kFakeRadioId, "RTL-SDR (serial ending 0001)");
    looks(a, 4);
    CHECK(Access::popup(a).open() && Access::popup(a).shown() == 2);
    Access::popup(a).goToPatchPage();
    CHECK(!Access::popup(a).open() && Access::patchOpen(a));
    // the key changed neither the patch nor who holds the radio: the same choice is still refused
    CHECK_EQ(Access::partCount(a), partsBefore);
    Access::choose(a, engine_api_test::kFakeRadioId, "x");
    looks(a, 4);
    CHECK_EQ(Access::fact(a, "fact.status.sourceError"), partSentence("Aerial B200"));
    CHECK_EQ(Access::failures(a), failsBefore);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_refusal_popup_app\n");
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    const std::filesystem::path scratch = std::filesystem::temp_directory_path() / ("foxsdr_refusal_popup_" + std::to_string(pid) + "_" + passseam::name());
    std::filesystem::create_directories(scratch);
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) { setEnv(v, scratch.string()); }
    for (const char* v : {"FOXSDR_TELEMETRY_URL", "FOXSDR_CRASH_URL", "FOXSDR_UPDATE_URL"}) { setEnv(v, "http://127.0.0.1:9"); }

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1600.0f, 1000.0f);
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    CHECK(cascade::gui::fonts::load());
    Access::install();
    {
        auto app = std::make_unique<th::TestWindow>();
        testTheClassicWindow(*app);
    }
    ImGui::DestroyContext();
    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
    return testSummary("test_refusal_popup_app");
}
