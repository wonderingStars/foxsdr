// test_catalogue_error_report.cpp - a catalogue fetch that FAILED is reported
// as failed to whoever is told the result (the 3b-pre-end review, LOW).
//
// Since OPEN 3 the window reads the engine's status lines through the copy the
// engine hands over (Engine::statusText, published by publishStatusText), not
// the engine's own members. Engine::pollPluginAsync set catalogError_ and then
// told the host the result (onCatalogueResult) WITHOUT publishing first - so
// the host read the copy from before the fetch, with no error in it. Under
// CASCADE_PLUGIN_TEST the report (the window's, then; since window port group 14 part e Engine::reportPluginTestResult, which
// the window's onCatalogueResult hook calls with the frame - this file's case C is unchanged by the move) then printed
// "plugin catalogue: ... entries=0" for a fetch that had failed, instead of
// "plugin catalogue: FAILED ... <reason>" - a bounded run that proves the store
// works, fooled by a store that does not.
//   A  a failed fetch: the host, when told, reads the fetch's error
//   B  a fetch that loaded but could not remember its policy: the host reads
//      that error too (the other place catalogError_ is set)
//   C  the window's bounded-run report prints FAILED with the reason
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <sstream>
#include <string>

#if defined(_WIN32)
#include <io.h>
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "engine/engine.hpp"
#include "engine/engine_host.hpp"
#include "gui/app_window.hpp"
#include "test_host.hpp"
#include "imgui.h"
#include "test_check.hpp"

using cascade::engine::Engine;

namespace {

void setEnv(const char* n, const std::string& v) {
#if defined(_WIN32)
    ::SetEnvironmentVariableA(n, v.c_str());
    _putenv_s(n, v.c_str());
#else
    ::setenv(n, v.c_str(), 1);
#endif
}

// The host a headless front end would be, remembering what it read when told.
struct Host : cascade::engine::EngineHost {
    Engine* engine = nullptr;
    int told = 0;
    std::string errorSeen;
    void onCatalogueResult() override {
        ++told;
        errorSeen = engine->statusText().catalogError;
    }
};

}  // namespace

namespace cascade::gui {
struct AppWindowTestAccess {
    // A fetch that has finished, as the worker would have left it.
    static void finishFetch(Engine& e, bool ok, const std::string& error, const std::string& policyError) {
        std::promise<Engine::CatalogFetchResult> p;
        Engine::CatalogFetchResult r;
        r.ok = ok;
        r.error = error;
        r.policyError = policyError;
        p.set_value(std::move(r));
        e.catalogFuture_ = p.get_future();
        e.catalogPending_ = true;
    }
    static void poll(Engine& e) { e.pollPluginAsync(); }
    static Engine& engine(AppWindow& a) { return th::engine(a); }
    // The end of the window's frame as a bounded run's host hears it (shell retirement R9: the hook the host installed, core/bounded_hooks.hpp): here, the
    // frame the window says is about to begin - which the engine's report names. (Before R9 the window handed the engine a switch and a frame number in
    // its front-end facts, every frame; the switch is the host's now - the harness arms it from CASCADE_PLUGIN_TEST - and the frame is this hook's.)
    static bool endOfFrame(AppWindow& a, int nextFrame) { return a.bounded_.onFrame(nextFrame, a.boundedClient_); }
};
}  // namespace cascade::gui

using A = cascade::gui::AppWindowTestAccess;

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_catalogue_error_report\n");
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    const auto scratch = std::filesystem::temp_directory_path() / ("foxsdr_catalogue_error_" + std::to_string(pid));
    std::filesystem::create_directories(scratch);
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, scratch.string());
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");

    // A and B: the engine, with a host of its own.
    {
        Host host;
        Engine e(host);
        host.engine = &e;
        e.initialise();

        A::finishFetch(e, false, "the store could not be reached", "");
        A::poll(e);
        std::printf("A: failed fetch - host told %d time(s), read \"%s\"\n", host.told, host.errorSeen.c_str());
        CHECK(host.told == 1);
        CHECK(host.errorSeen == "the store could not be reached");

        A::finishFetch(e, true, "", "the policy cache could not be written");
        A::poll(e);
        std::printf("B: loaded, policy not remembered - host read \"%s\"\n", host.errorSeen.c_str());
        CHECK(host.told == 2);
        CHECK(host.errorSeen == "the policy cache could not be written");
        e.teardown();
    }

    // C: the bounded run's report. Since the pump move's step P2 the ENGINE prints it - when the fetch resolves, if the diagnostic is on, naming the
    // frame the window had reached - where the window's onCatalogueResult hook used to read the window's own frame counter from the engine's pass. Since
    // shell retirement R9 the diagnostic is armed by the bounded run's HOST (CASCADE_PLUGIN_TEST read when the hooks are built) and the frame is told it by
    // the hook that ends each of the window's frames; the window hands the engine neither.
    ImGuiContext* ctx = ImGui::CreateContext();
    {
        th::TestWindow app;
        const auto captured = [&](const std::function<void()>& body) {
            const std::string out = (scratch / "report.txt").string();
            std::fflush(stdout);
#if defined(_WIN32)
            const int saved = _dup(_fileno(stdout));
            FILE* f = std::fopen(out.c_str(), "wb");
            _dup2(_fileno(f), _fileno(stdout));
#else
            const int saved = dup(fileno(stdout));
            FILE* f = std::fopen(out.c_str(), "wb");
            dup2(fileno(f), fileno(stdout));
#endif
            body();
            std::fflush(stdout);
#if defined(_WIN32)
            _dup2(saved, _fileno(stdout));
            _close(saved);
#else
            dup2(saved, fileno(stdout));
            close(saved);
#endif
            std::fclose(f);
            std::ifstream in(out);
            std::stringstream ss;
            ss << in.rdbuf();
            const std::string text = ss.str();
            const std::size_t at = text.find("plugin catalogue:");
            return at == std::string::npos ? std::string() : text.substr(at, text.find('\n', at) - at);
        };
        // C0: not armed (CASCADE_PLUGIN_TEST unset when the hooks were built) - the diagnostic is off, whatever the fetch did, whatever frame the hook
        // is told: the engine prints nothing.
        CHECK(A::endOfFrame(app, 7));
        A::finishFetch(A::engine(app), false, "the store could not be reached", "");
        const std::string silent = captured([&] { A::poll(A::engine(app)); });
        std::printf("C0: not armed: \"%s\"\n", silent.c_str());
        CHECK(silent.empty());
        // C: armed by the host (the variable named an address when the hooks were built), told the frame - naming frame 7.
        setEnv("CASCADE_PLUGIN_TEST", "http://127.0.0.1:9/catalogue.json");
        th::TestWindow armed;
        setEnv("CASCADE_PLUGIN_TEST", "");
        CHECK(A::endOfFrame(armed, 7));
        A::finishFetch(A::engine(armed), false, "the store could not be reached", "");
        const std::string line = captured([&] { A::poll(A::engine(armed)); });
        std::printf("C: the bounded run's report: \"%s\"\n", line.c_str());
        CHECK(line.find("FAILED") != std::string::npos);
        CHECK(line.find("the store could not be reached") != std::string::npos);
        CHECK(line.find("frame=7") != std::string::npos);
        CHECK(line.find("entries=") == std::string::npos);
    }
    ImGui::DestroyContext(ctx);

    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
    return testSummary("test_catalogue_error_report");
}
