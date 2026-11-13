// test_engine_api_wire_c_recorder.cpp - the engine's side of recorder_start / recorder_stop (wiring lane C row P1: engine_wire_c.cpp wireCPluginRecorder, core/plugin_api.*),
// against a real Engine with a stand-in plugin attached to its own plugin API core.
//
//   P1  a granted plugin's start makes the radio's audio take (the radio says it is recording, one file in the recordings folder named with the label first); its stop ends it
//   P2  a start while the USER's take runs is refused and changes nothing; that plugin's stop does not end the user's take; a plugin that did not start a take cannot stop it
//   P3  the plugin's grant taken away ends its take at the next pass
//   P4  without the grant nothing is recorded
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <thread>

#include "core/plugin_api.hpp"
#include "core/plugin_host.hpp"
#include "core/plugin_ui.hpp"
#include "wire_c_rig.hpp"

using namespace wirec_test;
using cascade::core::HostServices;
using cascade::core::LoadedPlugin;
using cascade::core::PluginUi;

namespace {

struct Captured {
    const CascadeHostApi* api = nullptr;
};
Captured g_cap[2];
template <int I>
void attachI(const CascadeHostApi* h) {
    g_cap[I].api = h;
}
const CascadeHostClientApi kHc[2] = {{sizeof(CascadeHostClientApi), &attachI<0>}, {sizeof(CascadeHostClientApi), &attachI<1>}};

LoadedPlugin plug(int i, const char* name) {
    LoadedPlugin p;
    p.loaded = true;
    p.name = name;
    p.version = "1.0.0";
    p.path = std::string("C:/plugins/") + name + ".dll";
    p.capabilities = CASCADE_CAP_HOST_CLIENT;
    p.hostClient = &kHc[i];
    return p;
}
const CascadeHostApi* H(int i) { return g_cap[i].api; }
void* C(int i) { return g_cap[i].api != nullptr ? g_cap[i].api->ctx : nullptr; }

HostServices services() {
    HostServices s;
    s.centreHz = [] { return 145.0e6; };
    s.sampleRateHz = [] { return 2.4e6; };
    s.tune = [](double) { return static_cast<std::int32_t>(CASCADE_TUNE_OK); };
    s.unixTimeMs = [] { return static_cast<std::int64_t>(1'790'000'000'000LL); };
    return s;
}

FoxCommand op(std::uint32_t o, std::int64_t a = 0) { return testing::cmd(o, 0.0, a, nullptr); }

// every .wav under the engine-test profile whose file name starts with `prefix`
int wavsNamed(const std::string& prefix) {
    const char* root = std::getenv("USERPROFILE");
    int n = 0;
    std::error_code ec;
    if (root == nullptr || !fs::exists(root, ec)) { return 0; }
    for (const auto& e : fs::recursive_directory_iterator(root, ec)) {
        if (e.is_regular_file() && e.path().extension() == ".wav" && e.path().filename().string().rfind(prefix, 0) == 0) { ++n; }
    }
    return n;
}

bool recording(Rig& r) { return (r.local->state().flags & FOXAPI_RX_RECORDING_AUDIO) != 0u; }

template <class Pred>
bool waitFor(Rig& r, Pred p, int ms = 4000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        r.pass(1);
        if (p()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return p();
}

void testRecorder(const fs::path& dir) {
    Rig r(dir);
    CHECK(r.status(op(FOXAPI_OP_RUN, 1)) == FOXAPI_OK);
    PluginUi ui(r.eng.pluginApiShared());
    ui.setServices(services());
    ui.rebuild({plug(0, "tgrec"), plug(1, "other")});
    CHECK(H(0) != nullptr && H(1) != nullptr);
    if (H(0) == nullptr || H(1) == nullptr) { return; }
    const std::string keyA = PluginUi::tuneKey(plug(0, "tgrec"));
    const std::string keyB = PluginUi::tuneKey(plug(1, "other"));

    // P4: no grant, nothing is recorded
    CHECK(H(0)->recorder_start(C(0), "tg-1234") == CASCADE_API_DENIED);
    r.pass(10);
    CHECK(!recording(r));
    CHECK(wavsNamed("tg-1234_") == 0);

    // P1: granted: the take starts, with the label in the file name, and stops
    ui.api().setSettingsGranted(keyA, true);
    ui.api().setSettingsGranted(keyB, true);
    CHECK(H(0)->recorder_start(C(0), "tg-1234") == CASCADE_API_OK);
    CHECK(waitFor(r, [&] { return recording(r); }));
    CHECK(wavsNamed("tg-1234_") == 1);
    CHECK(H(1)->recorder_stop(C(1)) == CASCADE_API_OK);   // P2: another plugin cannot stop it
    r.pass(10);
    CHECK(recording(r));
    CHECK(H(0)->recorder_stop(C(0)) == CASCADE_API_OK);
    CHECK(waitFor(r, [&] { return !recording(r); }));
    r.pass(20);
    CHECK(wavsNamed("tg-1234_") == 1);   // the file is kept

    // P2: the user's own take is never taken over
    CHECK(r.status(op(FOXAPI_OP_RECORD_AUDIO, 1)) == FOXAPI_OK);
    CHECK(waitFor(r, [&] { return recording(r); }));
    CHECK(H(0)->recorder_start(C(0), "tg-5") == CASCADE_API_OK);   // accepted (queued); the host refuses it when it is applied
    r.pass(10);
    CHECK(wavsNamed("tg-5_") == 0);
    CHECK(H(0)->recorder_stop(C(0)) == CASCADE_API_OK);          // and the plugin cannot end the user's take
    r.pass(10);
    CHECK(recording(r));
    CHECK(r.status(op(FOXAPI_OP_RECORD_AUDIO, 0)) == FOXAPI_OK);
    CHECK(waitFor(r, [&] { return !recording(r); }));
    r.pass(20);

    // P3: the grant taken away ends the plugin's take
    CHECK(H(0)->recorder_start(C(0), "tg-77") == CASCADE_API_OK);
    CHECK(waitFor(r, [&] { return recording(r); }));
    ui.api().setSettingsGranted(keyA, false);
    CHECK(waitFor(r, [&] { return !recording(r); }));
    if (wavsNamed("tg-77_") != 1) { std::printf("      tg-77 files: %d, recording %d, tg-1234 files %d\n", wavsNamed("tg-77_"), recording(r) ? 1 : 0, wavsNamed("tg-1234_")); }
    CHECK(wavsNamed("tg-77_") == 1);
}

}  // namespace

int main() {
    engine_api_test::hermetic("wire_c_recorder");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    {   // a process id can come round again: start from an empty profile, or an earlier run's files are counted
        std::error_code rm;
        if (const char* profile = std::getenv("USERPROFILE")) { fs::remove_all(profile, rm); }
    }
    const fs::path root = scratch("recorder");
    const fs::path d = root / "0";
    std::error_code ec;
    fs::create_directories(d, ec);
    testRecorder(d);
    fs::remove_all(root, ec);
    return check::finish("test_engine_api_wire_c_recorder");
}
