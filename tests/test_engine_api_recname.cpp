// test_engine_api_recname.cpp - recording file names from a template in the engine (FOXAPP_OP_SET_RECORD_NAME; engine wiring W2; src/engine/engine_recname.cpp,
// core/rec_naming.hpp), held against the real engine through the C table and its own passes, with a scratch recordings folder and a scratch template file.
//
//   N1  the op: a good template is OK and reads back in fact.record.nameTemplate; a template the model refuses (a forbidden character, a path escape, an unknown token, a
//       reserved device name, an unclosed brace) is BAD_ARGUMENT with words and changes nothing; the same template again is NO_CHANGE; "" clears it (and the file goes)
//   N2  a new engine reads the file back: the template survives a restart; a file holding a template this build refuses is ignored and takes keep their default names
//   N3  a take is named by the template: folders below the recordings folder are made, the tokens are the take's own (the tuned frequency in MHz, the mode, the year), the
//       file is a .wav inside the recordings folder
//   N4  with no template the take is named as it always was (a .wav straight in the recordings folder, <prefix>_<date>_<time>_...)
//   N5  a REMOTE session cannot set the template
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "check.hpp"
#include "core/app_commands.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine_api_test_env.hpp"
#include "engine_client.hpp"
#include "foxsdr_api.h"

namespace fs = std::filesystem;

namespace cascade::gui {
struct AppWindowTestAccess {
    using Engine = cascade::engine::Engine;
    static void recordDir(Engine& e, const std::string& dir) { e.recordDir_ = dir; }
    static void templateFile(Engine& e, const std::string& path) { e.recordNamePathForTest_ = path; }
};
}  // namespace cascade::gui
using Access = cascade::gui::AppWindowTestAccess;

namespace {

constexpr const char* kHooks = "testHooks=1;token=s3cret";
const FoxEngineApi* api() { return cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR); }

fs::path g_dir;

struct Rig {
    cascade::engine::Engine eng;
    std::unique_ptr<testing::Session> local;
    Rig(const fs::path& templateFile, const fs::path& recDir) {
        Access::templateFile(eng, templateFile.string());
        eng.configureApi(kHooks);
        eng.initialise();
        Access::recordDir(eng, recDir.string());
        eng.controlPass();
        local = std::make_unique<testing::Session>(api(), eng.apiHandle(), FOXAPI_SESSION_LOCAL, testing::kAllGrants);
        CHECK(local->ok());
    }
    testing::Outcome send(const FoxCommand& c, testing::Session* who = nullptr) {
        testing::Session& s = who != nullptr ? *who : *local;
        testing::Outcome o;
        FoxSubmitResult sr{};
        sr.structSize = sizeof(sr);
        const int32_t n = api()->submit(s.raw(), &c, 1, &sr);
        o.submitStatus = n < 0 ? n : sr.status;
        if (o.submitStatus != FOXAPI_OK) { return o; }
        for (int i = 0; i < 40 && !o.landed; ++i) {
            eng.controlPass();
            FoxCommandResult r[16];
            for (auto& x : r) { x.structSize = sizeof(x); }
            const int32_t got = api()->poll_results(s.raw(), r, 16);
            for (int32_t k = 0; k < got; ++k) {
                if (r[k].ticket == sr.ticket) {
                    o.landed = true;
                    o.result = r[k];
                }
            }
        }
        return o;
    }
    std::string fact(const std::string& key) {
        char buf[512];
        const int32_t n = api()->get_setting(local->raw(), ("fact." + key).c_str(), buf, sizeof(buf));
        return n >= 0 ? std::string(buf) : std::string("<absent>");
    }
    template <class F>
    bool until(F done, int ms = 8000) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            eng.controlPass();
            if (done()) { return true; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }
};

FoxCommand setName(const std::string& tmpl) { return testing::cmd(FOXAPP_OP_SET_RECORD_NAME, 0.0, 0, tmpl.c_str()); }

std::vector<fs::path> wavsUnder(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::exists(dir, ec)) { return out; }
    for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
        if (e.is_regular_file() && e.path().extension() == ".wav") { out.push_back(e.path()); }
    }
    return out;
}

void testOp() {
    std::printf("N1: the op\n");
    const fs::path file = g_dir / "n1-name.txt";
    Rig r(file, g_dir / "n1-rec");
    CHECK_EQ(r.fact("record.nameTemplate"), std::string());
    CHECK_EQ(r.send(setName("{freq_mhz}/{mode}_{datetime}")).status(), FOXAPI_OK);
    CHECK_EQ(r.fact("record.nameTemplate"), std::string("{freq_mhz}/{mode}_{datetime}"));
    CHECK(fs::exists(file));
    CHECK_EQ(r.send(setName("{freq_mhz}/{mode}_{datetime}")).status(), FOXAPI_NO_CHANGE);
    for (const char* bad : {"a:b", "../escape", "{nope}", "CON", "x/{date", "{counter:0}", "a//b", "/abs", "a/b/c/d/e"}) {
        const testing::Outcome o = r.send(setName(bad));
        if (o.status() != FOXAPI_BAD_ARGUMENT) { std::printf("  \"%s\" answered %d\n", bad, static_cast<int>(o.status())); }
        CHECK_EQ(o.status(), FOXAPI_BAD_ARGUMENT);
    }
    CHECK_EQ(r.fact("record.nameTemplate"), std::string("{freq_mhz}/{mode}_{datetime}"));   // nothing changed
    CHECK_EQ(r.send(setName("")).status(), FOXAPI_OK);
    CHECK_EQ(r.fact("record.nameTemplate"), std::string());
    CHECK(!fs::exists(file));
    CHECK_EQ(r.send(setName("")).status(), FOXAPI_NO_CHANGE);
}

void testRestart() {
    std::printf("N2: the template survives a restart\n");
    const fs::path file = g_dir / "n2-name.txt";
    {
        Rig r(file, g_dir / "n2-rec");
        CHECK_EQ(r.send(setName("{mode}/{year}")).status(), FOXAPI_OK);
    }
    {
        Rig r(file, g_dir / "n2-rec");
        CHECK(r.until([&] { return r.fact("record.nameTemplate") == "{mode}/{year}"; }, 3000));   // read from the file by the first pass that builds the facts
        CHECK_EQ(r.send(setName("{mode}/{year}")).status(), FOXAPI_NO_CHANGE);   // and so in force: setting it again changes nothing
    }
    {   // a file that holds a template this build refuses is ignored
        const fs::path bad = g_dir / "n2-bad.txt";
        { std::ofstream f(bad, std::ios::binary); f << "{no-such-token}\n"; }
        Rig r(bad, g_dir / "n2-rec");
        CHECK_EQ(r.send(setName("{mode}")).status(), FOXAPI_OK);   // nothing was in force: this one is new
        CHECK_EQ(r.fact("record.nameTemplate"), std::string("{mode}"));
    }
}

void testTake() {
    std::printf("N3: a take is named by the template; N4: and by default without one\n");
    const fs::path rec = g_dir / "n3-rec";
    std::error_code ec;
    fs::remove_all(rec, ec);
    Rig r(g_dir / "n3-name.txt", rec);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_FREQUENCY, 100.1e6, 0)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
    // N4 first: no template, the default name straight in the recordings folder
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_AUDIO, 0, 1)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return !wavsUnder(rec).empty(); }));
    {
        const std::vector<fs::path> w = wavsUnder(rec);
        CHECK_EQ(w.size(), std::size_t(1));
        if (!w.empty()) { CHECK(w[0].parent_path() == rec); }   // straight in the folder
    }
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_AUDIO, 0, 0)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.fact("radio.1.state") != "<absent>"; }, 2000));
    r.until([] { return false; }, 700);   // let the finish settle (the finishing take blocks the next start until it is closed)
    // N3: with a template
    CHECK_EQ(r.send(setName("{freq_mhz}/{mode}_{year}")).status(), FOXAPI_OK);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_AUDIO, 0, 1)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return wavsUnder(rec).size() >= 2u; }));
    std::vector<fs::path> w = wavsUnder(rec);
    fs::path named;
    for (const fs::path& p : w) {
        if (p.parent_path() != rec) { named = p; }
    }
    CHECK(!named.empty());
    if (!named.empty()) {
        const std::string folder = named.parent_path().filename().string();
        const std::string leaf = named.filename().string();
        std::printf("  a take named by the template: %s/%s\n", folder.c_str(), leaf.c_str());
        CHECK_EQ(folder, std::string("100.1"));                   // {freq_mhz}: the tuned frequency in MHz, trailing zeros trimmed
        CHECK(leaf.rfind("WFM_", 0) == 0u);                       // {mode}
        CHECK(leaf.size() == std::string("WFM_2026.wav").size());   // {year}: four digits, then .wav
        CHECK(named.parent_path().parent_path() == rec);          // one folder below the recordings folder
    }
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RECORD_AUDIO, 0, 0)).status(), FOXAPI_OK);
    r.until([] { return false; }, 700);
}

void testRemote() {
    std::printf("N5: a REMOTE session cannot set the template\n");
    Rig r(g_dir / "n5-name.txt", g_dir / "n5-rec");
    testing::Session remote(api(), r.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS, "s3cret");
    CHECK(remote.ok());
    CHECK_EQ(r.send(setName("{mode}"), &remote).status(), FOXAPI_DENIED);
    CHECK_EQ(r.fact("record.nameTemplate"), std::string());
    CHECK(!fs::exists(g_dir / "n5-name.txt"));
}

}  // namespace

int main() {
    engine_api_test::hermetic("recname");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#if defined(_WIN32)
    g_dir = fs::temp_directory_path() / ("foxsdr_recname_" + std::to_string(_getpid()));
#else
    g_dir = fs::temp_directory_path() / ("foxsdr_recname_" + std::to_string(getpid()));
#endif
    std::error_code ec;
    fs::create_directories(g_dir, ec);
    testOp();
    testRestart();
    testTake();
    testRemote();
    fs::remove_all(g_dir, ec);
    return check::finish("test_engine_api_recname");
}
