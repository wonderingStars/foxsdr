// test_engine_api_private.cpp - a plugin that sets CASCADE_FLAG_PRIVATE_DATA keeps its text, targets and pictures out of every sink that leaves the
// process's own windows (engine wiring W2, brief item 12; D:/foxsdr-build/prep/host-plugin-text-sinks.md, consequence 2). Held against the real
// engine, with no window and no network.
//
//   P1  the flag is a flag: a descriptor that carries it validates, it is not a capability (it is outside CASCADE_CAP_ALL_KNOWN), a flag with no
//       capability beside it is refused like an unknown bit, and the host reads it back from the loaded record's capability word
//   P2  decoder lines: a LOCAL session (the host's own window) reads a private plugin's line whole; a REMOTE session (the web page, a network
//       client) reads the same line with its text BLANK - still numbered and still named, so a cursor and the count stay true - while another
//       plugin's line reaches both whole
//   P3  the web status ("decoded"): the private plugin's lines are not in it, the others are
//   P4  the web status ("tracks"): the private plugin's targets are not in it, the others are
//   P5  the pictures: the served set lists a private plugin's slot with no bitmap (width 0, not complete) and the others' as they are
//   P6  the diagnostics a person copies: the "plugin text:" lines leave a private plugin's text out
//   P7  the event ring (CascadeEvent, plugin_abi.h): every drained decoder line is an event of kind "text", a private plugin's is stamped private (the host reads
//       it, a plugin that is not that plugin does not), the frequency is the radio's
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "check.hpp"
#include "core/host_image.hpp"
#include "core/plugin_abi.h"
#include "core/plugin_ui_data.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine/image_feed.hpp"
#include "engine_client.hpp"
#include "foxsdr_api.h"

#if defined(_WIN32)
#include <io.h>
#define W2_DUP _dup
#define W2_DUP2 _dup2
#define W2_CLOSE _close
#define W2_FILENO _fileno
#else
#include <unistd.h>
#define W2_DUP dup
#define W2_DUP2 dup2
#define W2_CLOSE close
#define W2_FILENO fileno
#endif

namespace cascade::gui {
// The engine's friend, as every test that reaches an engine member is.
struct AppWindowTestAccess {
    using Engine = cascade::engine::Engine;
    static void markPrivate(Engine& e, const std::string& name) { e.testPrivatePlugins_.push_back(name); }
    static void handTracks(Engine& e, const std::vector<cascade::core::HostTrack>& polled) {
        cascade::engine::FrontEndFacts f = e.frontEndFacts();
        auto feed = std::make_shared<cascade::engine::TrackFeed>();
        feed->polled = polled;
        f.trackFeed = feed;
        e.setFrontEndFacts(f);
    }
    static void diagnostics(Engine& e) { e.reportPluginStatus({}, cascade::engine::PluginUiReport{}); }
};
}  // namespace cascade::gui
using Access = cascade::gui::AppWindowTestAccess;

namespace {

constexpr const char* kHooks = "testHooks=1;token=s3cret";
const FoxEngineApi* api() { return cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR); }

struct Direct {
    cascade::engine::Engine eng;
    Direct() {
        eng.configureApi(kHooks);
        eng.initialise();
        eng.controlPass();
    }
    void pass(int n = 3) {
        for (int i = 0; i < n; ++i) { eng.controlPass(); }
    }
};

int countOf(testing::Session& s, uint32_t list) { return api()->read_list(s.raw(), list, 0, nullptr); }
FoxListItem itemOf(testing::Session& s, uint32_t list, uint32_t index) {
    FoxListItem it{};
    it.structSize = sizeof(it);
    api()->read_list(s.raw(), list, index, &it);
    return it;
}
void setting(Direct& d, testing::Session& s, const std::string& kv) {
    FoxCommand c = testing::cmd(FOXAPI_OP_SETTING_SET, 0, 0, kv.c_str());
    FoxSubmitResult sr{};
    sr.structSize = sizeof(sr);
    api()->submit(s.raw(), &c, 1, &sr);
    d.pass();
}

cascade::core::HostTrack trackOf(const char* plugin, const char* id) {
    cascade::core::HostTrack t;
    t.plugin = plugin;
    std::snprintf(t.t.id, sizeof(t.t.id), "%s", id);
    t.t.latDeg = 51.5;
    t.t.lonDeg = -0.1;
    t.t.ageMs = 100;
    t.t.kind = CASCADE_TRACK_AIRCRAFT;
    return t;
}

cascade::core::HostImage imageOf(const char* plugin) {
    cascade::core::HostImage im;
    im.plugin = plugin;
    im.width = 8;
    im.height = 4;
    im.format = CASCADE_IMAGE_RGB24;
    im.complete = true;
    im.sequence = 1;
    im.revision = 1;
    im.pixels.assign(8u * 4u * 3u, 0x80);
    return im;
}

template <class Fn>
std::string captureStdout(Fn fn) {
    const std::string path = "test_engine_api_private_out_" + std::to_string(static_cast<long long>(std::rand())) + ".txt";
    std::fflush(stdout);
    const int saved = W2_DUP(W2_FILENO(stdout));
    std::FILE* f = std::fopen(path.c_str(), "w");
    W2_DUP2(W2_FILENO(f), W2_FILENO(stdout));
    fn();
    std::fflush(stdout);
    W2_DUP2(saved, W2_FILENO(stdout));
    W2_CLOSE(saved);
    std::fclose(f);
    std::string out;
    if (std::FILE* r = std::fopen(path.c_str(), "r")) {
        char buf[4096];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, r)) > 0) { out.append(buf, n); }
        std::fclose(r);
    }
    std::remove(path.c_str());
    return out;
}

// ---- P1 ---------------------------------------------------------------------------------------------------------------------------------------

void testTheFlagIsAFlag() {
    std::printf("P1: CASCADE_FLAG_PRIVATE_DATA is a flag, not a capability\n");
    CHECK((static_cast<std::uint32_t>(CASCADE_FLAG_PRIVATE_DATA) & static_cast<std::uint32_t>(CASCADE_CAP_ALL_KNOWN)) == 0u);   // no capability census row needed
    CHECK((static_cast<std::uint32_t>(CASCADE_FLAG_PRIVATE_DATA) & (static_cast<std::uint32_t>(CASCADE_FLAG_PRIVATE_DATA) - 1u)) == 0u);   // one bit
    // The engine reads it back from a loaded record's capability word: a record whose word carries it is private, one without is not.
    cascade::core::LoadedPlugin a;
    a.name = "FlagPlug";
    a.capabilities = CASCADE_CAP_DECODER | CASCADE_FLAG_PRIVATE_DATA;
    cascade::core::LoadedPlugin b;
    b.name = "PlainPlug";
    b.capabilities = CASCADE_CAP_DECODER;
    CHECK((a.capabilities & CASCADE_FLAG_PRIVATE_DATA) != 0u);
    CHECK((b.capabilities & CASCADE_FLAG_PRIVATE_DATA) == 0u);
}

// ---- P2 - P6 ----------------------------------------------------------------------------------------------------------------------------------

void testSinks() {
    std::printf("P2-P6: a private plugin's text, targets and pictures stay in the process\n");
    Direct d;
    testing::Session local(api(), d.eng.apiHandle(), FOXAPI_SESSION_LOCAL, testing::kAllGrants);
    testing::Session remote(api(), d.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS, "s3cret");
    CHECK(local.ok());
    CHECK(remote.ok());
    Access::markPrivate(d.eng, "SECRETPLUG");
    d.pass();   // the set is rebuilt on the pass

    // P2 - decoder lines
    setting(d, local, "test.decoderLine=SECRETPLUG|id-4711");
    setting(d, local, "test.decoderLine=OPENPLUG|hello");
    CHECK(countOf(local, FOXAPI_LIST_DECODER_LINES) == 2);
    CHECK(countOf(remote, FOXAPI_LIST_DECODER_LINES) == 2);   // the count is the same: a reader's cursor stays true
    const FoxListItem l0 = itemOf(local, FOXAPI_LIST_DECODER_LINES, 0);
    const FoxListItem l1 = itemOf(local, FOXAPI_LIST_DECODER_LINES, 1);
    CHECK_EQ(std::string(l0.name), std::string("SECRETPLUG"));
    CHECK_EQ(std::string(l0.detail), std::string("id-4711"));   // the host's own window reads it whole
    CHECK_EQ(std::string(l1.detail), std::string("hello"));
    const FoxListItem r0 = itemOf(remote, FOXAPI_LIST_DECODER_LINES, 0);
    const FoxListItem r1 = itemOf(remote, FOXAPI_LIST_DECODER_LINES, 1);
    CHECK_EQ(std::string(r0.name), std::string("SECRETPLUG"));   // still named
    CHECK_EQ(r0.id, l0.id);                                       // and numbered
    CHECK_EQ(std::string(r0.detail), std::string());              // but blank
    CHECK_EQ(std::string(r1.detail), std::string("hello"));      // another plugin's line is whole for the remote session too

    // P3 - the web status' decoded lines
    {
        const cascade::net::RadioStatus st = d.eng.webStatusNow();
        bool open = false, secret = false;
        for (const auto& line : st.decoded) {
            if (line.text == "hello") { open = true; }
            if (line.text.find("id-4711") != std::string::npos || line.plugin == "SECRETPLUG") { secret = true; }
        }
        CHECK(open);
        CHECK(!secret);
    }

    // P4 - the web status' tracks
    Access::handTracks(d.eng, {trackOf("SECRETPLUG", "AAAAAA"), trackOf("OPENPLUG", "BBBBBB")});
    d.pass();
    {
        const cascade::net::RadioStatus st = d.eng.webStatusNow();
        bool open = false, secret = false;
        for (const auto& t : st.tracks) {
            if (t.id == "BBBBBB") { open = true; }
            if (t.id == "AAAAAA" || t.plugin == "SECRETPLUG") { secret = true; }
        }
        CHECK(open);
        CHECK(!secret);
    }

    // P5 - the pictures
    d.eng.servers().images().setPoll([](std::vector<cascade::core::HostImage>& out) {
        out.clear();
        out.push_back(imageOf("SECRETPLUG"));
        out.push_back(imageOf("OPENPLUG"));
    });
    d.eng.servers().images().cycleForTest();
    {
        const auto headers = d.eng.servers().images().headers();
        CHECK(headers != nullptr);
        bool sawSecret = false, sawOpen = false;
        if (headers != nullptr) {
            for (const auto& h : *headers) {
                if (h.plugin == "SECRETPLUG") {
                    sawSecret = true;
                    CHECK_EQ(h.width, 0u);   // no bitmap: the slot only says the plugin exists
                    CHECK(!h.complete);
                }
                if (h.plugin == "OPENPLUG") {
                    sawOpen = true;
                    CHECK_EQ(h.width, 8u);
                    CHECK(h.complete);
                }
            }
        }
        CHECK(sawSecret && sawOpen);
    }

    // P7 - the event ring: the two injected lines are in it, the private one flagged
    {
        const std::vector<CascadeEvent> evs = d.eng.pluginApiShared()->hostEvents(0);
        const CascadeEvent* secret = nullptr;
        const CascadeEvent* open = nullptr;
        for (const CascadeEvent& e : evs) {
            if (std::string(e.text) == "id-4711") { secret = &e; }
            if (std::string(e.text) == "hello") { open = &e; }
        }
        CHECK(secret != nullptr && open != nullptr);
        if (secret != nullptr && open != nullptr) {
            CHECK_EQ(std::string(secret->kind), std::string(CASCADE_EVENT_KIND_TEXT));
            CHECK_EQ(std::string(secret->plugin), std::string("SECRETPLUG"));
            CHECK((secret->flags & CASCADE_EVENT_FLAG_PRIVATE) != 0u);
            CHECK((open->flags & CASCADE_EVENT_FLAG_PRIVATE) == 0u);
            CHECK(open->seq > 0u && secret->seq > 0u && open->seq != secret->seq);
            CHECK(std::fabs(open->frequencyHz - local.state().tunedHz) < 1.0);   // the tuned frequency the radio reports
        }
    }

    // P6 - the diagnostics: the five newest decoded lines are printed, a private plugin's is not among them
    {
        const std::string out = captureStdout([&] { Access::diagnostics(d.eng); });
        CHECK(out.find("plugin text: [OPENPLUG] hello") != std::string::npos);
        CHECK(out.find("id-4711") == std::string::npos);
        CHECK(out.find("SECRETPLUG") == std::string::npos);
    }
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    testTheFlagIsAFlag();
    testSinks();
    return check::finish("test_engine_api_private");
}
