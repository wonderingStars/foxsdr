// test_engine_api_03.cpp - API 0.3's additions (docs/api-0.3-additions.md;
// engine-stage4.md plan step 3, the owner's decision 4), held against the real
// engine through the C table and nothing else - the way
// test_engine_api_conformance holds 0.2.
//
// One group per addition, each shown red before the engine served it:
//   V   the version gate and the capability bits (0.3 only; a 0.2 interface
//       is refused, as 0.x requires)
//   L   every list the window draws is SERVED (a count, never UNSUPPORTED) -
//       the 0.2 lists step 1 left out and the 0.3 ones
//   T   submit_text: one command, a text longer than text[], the same answers
//       as submit
//   S   the status lines: a list, and the whole text as a fact
//   F   facts: unknown keys, LOCAL-only ones, the transmitter's address
//   P   the patch graph read back: nodes, wires, the document, the event
//   R   the recordings: the file, ACTIVE, the bytes live, the path LOCAL only
//   C   the scope taps: newest frames, the tap's rate, "reduced" for REMOTE
//   N   a patch radio's spectrum: NOT_FOUND with none running
//   E   serial ports: the scan is LOCAL only, the list is served
//   D   the decoder-output backlog
//   K   the store: an entry's long texts as facts
//   B   the bias tee: flags and facts on a radio without one
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "check.hpp"
#include "core/app_commands.hpp"
#include "core/patch_draft.hpp"
#include "core/patch_graph.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine_api_fake_radio.hpp"
#include "engine_api_test_env.hpp"
#include "engine_client.hpp"
#include "foxsdr_api.h"

namespace cascade::gui {
struct AppWindowTestAccess {
    static void installFakeRadio() {
        cascade::engine::Engine::testHooks_.makeDevice = &engine_api_test::makeFakeRadio;
        cascade::engine::Engine::testHooks_.nativeScan = &engine_api_test::fakeNativeScan;
        cascade::engine::Engine::testHooks_.soapyScan = &engine_api_test::noSoapyRadios;
    }
};
}  // namespace cascade::gui

using testing::cmd;
using testing::kAllGrants;

namespace {

constexpr const char* kHooks = "testHooks=1;token=s3cret";

const FoxEngineApi* api() { return cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR); }

std::string fact(testing::Session& s, const char* key, int32_t* status = nullptr) {
    char buf[8192];
    const int32_t n = api()->get_setting(s.raw(), key, buf, sizeof(buf));
    if (status != nullptr) { *status = n; }
    return n >= 0 ? std::string(buf) : std::string();
}

int32_t count(testing::Session& s, uint32_t list) { return api()->read_list(s.raw(), list, 0, nullptr); }

FoxListItem item(testing::Session& s, uint32_t list, uint32_t index) {
    FoxListItem it{};
    it.structSize = sizeof(it);
    api()->read_list(s.raw(), list, index, &it);
    return it;
}

// Submits ONE command with a long text and waits for its result.
testing::Outcome runText(testing::Session& s, FoxCommand c, const std::string& text, int timeoutMs = 2000) {
    testing::Outcome o;
    FoxSubmitResult sr{};
    sr.structSize = sizeof(sr);
    const int32_t n = api()->submit_text(s.raw(), &c, text.data(), static_cast<uint32_t>(text.size()), &sr);
    o.submitStatus = n < 0 ? n : sr.status;
    if (o.submitStatus != FOXAPI_OK) { return o; }
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end) {
        FoxCommandResult r[16];
        for (auto& x : r) { x.structSize = sizeof(x); }
        const int32_t got = api()->poll_results(s.raw(), r, 16);
        for (int32_t i = 0; i < got; ++i) {
            if (r[i].ticket == sr.ticket) {
                o.landed = true;
                o.result = r[i];
            }
        }
        if (o.landed) { return o; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return o;
}

FoxCommand appCmd(uint32_t op, long long i0 = 0, double n0 = 0.0) {
    FoxCommand c = cmd(op, n0, i0);
    return c;
}

// A REMOTE session on the engine's configured token.
struct Remote : testing::Session {
    explicit Remote(testing::Engine& e)
        : testing::Session(api(), e.raw(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE |
                                                                     FOXAPI_GRANT_SETTINGS,
                           "s3cret") {}
};

void testVersionGate() {
    CHECK(cascade::engine::engineApi(0, 3) != nullptr);
    CHECK(cascade::engine::engineApi(0, 2) == nullptr);  // 0.x: its own minor only
    const FoxEngineApi* a = api();
    CHECK_EQ(a->versionMinor, 3u);
    CHECK(a->structSize >= FOXAPI_MIN_ENGINE_API);
    CHECK(FOXAPI_HAS(a, read_scope));
    CHECK(FOXAPI_HAS(a, read_node_spectrum));
    CHECK(FOXAPI_HAS(a, submit_text));
    CHECK((a->capabilities & FOXAPI_CAP_FACTS) != 0u);
    CHECK((a->capabilities & FOXAPI_CAP_LONG_TEXT) != 0u);
    CHECK((a->capabilities & FOXAPI_CAP_SCOPE) != 0u);
    CHECK((a->capabilities & FOXAPI_CAP_NODE_SPECTRUM) != 0u);
}

void testEveryListIsServed() {
    testing::Engine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    int served = 0;
    for (uint32_t l = 1; l <= FOXAPI_LIST_PATCH_RADIOS; ++l) {
        if (l == 13u) { continue; }  // retired in 0.2
        if (l == FOXAPI_LIST_TRACKS) {
            // Map tracks come from track-source plugins through the map
            // page, which is not served yet (docs/api-0.3-additions.md).
            CHECK_EQ(count(s, l), FOXAPI_UNSUPPORTED);
            continue;
        }
        const int32_t n = count(s, l);
        if (n < 0) { std::printf("  list %u answered %d\n", l, n); }
        CHECK(n >= 0);
        served += n >= 0 ? 1 : 0;
    }
    std::printf("L: %d lists served\n", served);
    // The transmitter's one "device": the address TX_OPEN will be given.
    CHECK_EQ(count(s, FOXAPI_LIST_TX_DEVICES), 1);
    const FoxListItem tx = item(s, FOXAPI_LIST_TX_DEVICES, 0);
    CHECK_EQ(std::string(tx.detail), std::string("uri=ip:192.168.2.1"));
    CHECK((tx.flags & FOXAPI_ITEM_ACTIVE) == 0u);
    // No plugins, no radio options on the generator.
    CHECK_EQ(count(s, FOXAPI_LIST_DECODERS), 0);
    CHECK_EQ(count(s, FOXAPI_LIST_DEVICE_OPTIONS), 0);
    CHECK_EQ(count(s, FOXAPI_LIST_GAIN_MODES), 0);
}

void testSubmitText() {
    testing::Engine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    const FoxEngineApi* a = api();
    const std::string longText(600, 'x');
    FoxCommand c = appCmd(FOXAPP_OP_SET_SOURCE_ERROR);
    const testing::Outcome o = runText(s, c, longText);
    CHECK_EQ(o.submitStatus, FOXAPI_OK);
    CHECK(o.landed);
    CHECK_EQ(o.result.status, FOXAPI_OK);
    int32_t st = 0;
    CHECK_EQ(fact(s, "fact.status.sourceError", &st), longText);
    CHECK_EQ(st, 600);
    // The same answers as submit for what is wrong with the call.
    FoxSubmitResult sr{};
    sr.structSize = sizeof(sr);
    CHECK_EQ(a->submit_text(s.raw(), nullptr, "x", 1, &sr), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(a->submit_text(s.raw(), &c, nullptr, 5, &sr), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(a->submit_text(s.raw(), &c, longText.data(), FOXAPI_MAX_LONG_TEXT + 1u, &sr), FOXAPI_LIMIT);
    // A REMOTE session still may not send the app's own ops, long or short.
    Remote r(e);
    CHECK(r.ok());
    const testing::Outcome ro = runText(r, c, longText);
    CHECK_EQ(ro.status(), FOXAPI_DENIED);
}

void testStatusLines() {
    testing::Engine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK_EQ(count(s, FOXAPI_LIST_STATUS_LINES), 0);
    const std::string why = std::string("the radio said no: ") + std::string(300, 'y');
    CHECK_EQ(runText(s, appCmd(FOXAPP_OP_SET_SOURCE_ERROR), why).result.status, FOXAPI_OK);
    CHECK(check::waitFor([&] { return count(s, FOXAPI_LIST_STATUS_LINES) == 1; }, 2000));
    const FoxListItem it = item(s, FOXAPI_LIST_STATUS_LINES, 0);
    CHECK_EQ(std::string(it.name), std::string("sourceError"));
    CHECK_EQ(it.value[0], 3.0);  // an error
    CHECK(std::string(it.detail).size() == sizeof(it.detail) - 1);  // cut, whole in the fact
    CHECK(why.compare(0, sizeof(it.detail) - 1, it.detail) == 0);
    CHECK_EQ(fact(s, "fact.status.sourceError"), why);
    CHECK_EQ(runText(s, appCmd(FOXAPP_OP_SET_SOURCE_ERROR), std::string()).result.status, FOXAPI_OK);
    CHECK(check::waitFor([&] { return count(s, FOXAPI_LIST_STATUS_LINES) == 0; }, 2000));
    int32_t st = 0;
    CHECK_EQ(fact(s, "fact.status.sourceError", &st), std::string());
    CHECK_EQ(st, 0);  // a known line, empty now
    // A line further down the list: the list shows only the lines with text.
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SETTING_SET, 0, 0, "test.statusLine=presetNote|Tuned to the preset.")).result.status,
             FOXAPI_OK);
    CHECK(check::waitFor([&] { return count(s, FOXAPI_LIST_STATUS_LINES) == 1; }, 2000));
    const FoxListItem note = item(s, FOXAPI_LIST_STATUS_LINES, 0);
    CHECK_EQ(std::string(note.name), std::string("presetNote"));
    CHECK_EQ(std::string(note.detail), std::string("Tuned to the preset."));
    CHECK_EQ(note.value[0], 1.0);  // information
}

void testFacts() {
    testing::Engine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    Remote r(e);
    CHECK(r.ok());
    int32_t st = 0;
    fact(s, "fact.no.such.thing", &st);
    CHECK_EQ(st, FOXAPI_NOT_FOUND);
    fact(s, "fact.status.noSuchLine", &st);
    CHECK_EQ(st, FOXAPI_NOT_FOUND);
    CHECK_EQ(fact(s, "fact.transmit.args"), std::string("uri=ip:192.168.2.1"));
    // A path on this machine: LOCAL sessions only.
    CHECK(!fact(s, "fact.recorder.dir", &st).empty());
    CHECK(st > 0);
    fact(r, "fact.recorder.dir", &st);
    CHECK_EQ(st, FOXAPI_DENIED);
    // Facts are read-only: no SETTING_SET takes one.
    CHECK(s.run(cmd(FOXAPI_OP_SETTING_SET, 0, 0, "fact.transmit.args=x")).status() != FOXAPI_OK);
    CHECK_EQ(fact(s, "fact.transmit.args"), std::string("uri=ip:192.168.2.1"));
}

void testPatchGraph() {
    testing::Engine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    const FoxEngineApi* a = api();
    a->subscribe(s.raw(), FOXAPI_EVENT_MASK(FOXAPI_EVENT_PATCH));
    CHECK_EQ(count(s, FOXAPI_LIST_PATCH_NODES), 0);
    namespace patch = cascade::core::patch;
    patch::Graph g;
    const patch::NodeId radio = g.addNode(patch::NodeKind::Radio, "Radio 1", patch::PortType::Iq, 10.0f, 20.0f);
    const patch::NodeId chan = g.addNode(patch::NodeKind::Channel, "Channel", patch::PortType::Iq, 200.0f, 20.0f);
    const patch::NodeId demod = g.addNode(patch::NodeKind::Demod, "Demod", patch::PortType::Iq, 400.0f, 20.0f);
    CHECK(g.connect(radio, 0, chan, 0) == patch::Connect::Ok);
    CHECK(g.connect(chan, 0, demod, 0) == patch::Connect::Ok);
    // More parts than the list checks need: a real patch's document is
    // longer than a command's text[] - which is the reason submit_text exists.
    for (int i = 0; i < 6; ++i) {
        g.addNode(patch::NodeKind::Display, "A spectrum display with a long name " + std::to_string(i),
                  patch::PortType::Iq, 600.0f, 40.0f * static_cast<float>(i));
    }
    const std::string doc = patch::graphCommandText(g);
    CHECK(doc.size() > FOXAPI_TEXT_CHARS);  // the reason submit_text exists
    const FoxReceiverState before = s.state();
    const testing::Outcome o =
        runText(s, appCmd(FOXAPP_OP_PATCH_SET_GRAPH, FOXAPP_PATCH_GRAPH_DOCUMENT), doc);
    CHECK_EQ(o.result.status, FOXAPI_OK);
    CHECK(check::waitFor([&] { return count(s, FOXAPI_LIST_PATCH_NODES) == 9; }, 2000));
    CHECK(s.state().listSeq != before.listSeq);
    const FoxListItem n0 = item(s, FOXAPI_LIST_PATCH_NODES, 0);
    CHECK_EQ(n0.id, static_cast<uint64_t>(radio));
    CHECK_EQ(std::string(n0.name), std::string("Radio 1"));
    CHECK_EQ(std::string(n0.detail), std::string("radio"));
    CHECK_EQ(n0.value[0], 10.0);
    CHECK_EQ(n0.value[1], 20.0);
    CHECK_EQ(std::string(item(s, FOXAPI_LIST_PATCH_NODES, 2).detail), std::string("demod"));
    CHECK_EQ(count(s, FOXAPI_LIST_PATCH_WIRES), 2);
    const FoxListItem w0 = item(s, FOXAPI_LIST_PATCH_WIRES, 0);
    CHECK_EQ(w0.value[0], static_cast<double>(radio));
    CHECK_EQ(w0.value[2], static_cast<double>(chan));
    CHECK_EQ(fact(s, "fact.patch.graph"), doc);
    CHECK(check::waitFor(
        [&] {
            for (const FoxEvent& ev : s.events()) {
                if (ev.kind == FOXAPI_EVENT_PATCH && ev.value[0] >= 1.0) { return true; }
            }
            return false;
        },
        2000));
    // A document the engine cannot honour is refused whole: nothing moves.
    CHECK_EQ(runText(s, appCmd(FOXAPP_OP_PATCH_SET_GRAPH, FOXAPP_PATCH_GRAPH_DOCUMENT), "not a patch\n")
                 .result.status,
             FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(count(s, FOXAPI_LIST_PATCH_NODES), 9);
    CHECK_EQ(fact(s, "fact.patch.graph"), doc);
    // An EDIT (not a document load: the epoch stays) is read back too.
    g.addNode(patch::NodeKind::Sink, "Speaker", patch::PortType::Audio, 800.0f, 20.0f);
    const std::string edited = patch::graphCommandText(g);
    CHECK_EQ(runText(s, appCmd(FOXAPP_OP_PATCH_SET_GRAPH, 0), edited).result.status, FOXAPI_OK);
    CHECK(check::waitFor([&] { return count(s, FOXAPI_LIST_PATCH_NODES) == 10; }, 2000));
    CHECK_EQ(fact(s, "fact.patch.graph"), edited);
}

void testRecordings() {
    testing::Engine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    Remote r(e);
    CHECK_EQ(s.run(cmd(FOXAPI_OP_RUN, 0, 1)).result.status, FOXAPI_OK);
    CHECK_EQ(count(s, FOXAPI_LIST_RECORDINGS), 0);
    CHECK_EQ(s.run(cmd(FOXAPI_OP_RECORD_IQ, 0, 1)).result.status, FOXAPI_OK);
    CHECK(check::waitFor([&] { return count(s, FOXAPI_LIST_RECORDINGS) == 1; }, 2000));
    FoxListItem it = item(s, FOXAPI_LIST_RECORDINGS, 0);
    CHECK((it.flags & FOXAPI_ITEM_ACTIVE) != 0u);
    CHECK_EQ(it.value[0], 1.0);
    CHECK(std::strlen(it.name) > 0);
    CHECK(std::string(it.detail).find(it.name) != std::string::npos);  // the path ends in the file
    CHECK(std::strlen(item(r, FOXAPI_LIST_RECORDINGS, 0).detail) == 0);  // no path to a REMOTE session
    // The bytes are read live, without a list rebuild (listSeq still).
    const double b0 = it.value[1];
    const uint64_t seq0 = s.state().listSeq;
    CHECK(check::waitFor([&] { return item(s, FOXAPI_LIST_RECORDINGS, 0).value[1] > b0; }, 2000));
    CHECK_EQ(s.state().listSeq, seq0);
    CHECK_EQ(fact(s, "fact.recorder.iq.path"), std::string(it.detail));
    CHECK_EQ(s.run(cmd(FOXAPI_OP_RECORD_IQ, 0, 0)).result.status, FOXAPI_OK);
    CHECK(check::waitFor([&] { return (item(s, FOXAPI_LIST_RECORDINGS, 0).flags & FOXAPI_ITEM_ACTIVE) == 0u; },
                         2000));
    s.run(cmd(FOXAPI_OP_RUN, 0, 0));
}

void testScope() {
    testing::Engine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    Remote r(e);
    const FoxEngineApi* a = api();
    CHECK_EQ(s.run(cmd(FOXAPI_OP_RUN, 0, 1)).result.status, FOXAPI_OK);
    std::vector<float> buf(2 * 65536);
    FoxScopeInfo info{};
    info.structSize = sizeof(info);
    CHECK(check::waitFor([&] { return a->read_scope(s.raw(), FOXAPI_SCOPE_AUDIO, &info, buf.data(), 1024) == 1024; },
                         3000));
    CHECK_EQ(info.tap, FOXAPI_SCOPE_AUDIO);
    CHECK_EQ(info.channels, 1u);
    CHECK_EQ(info.rateHz, 48000.0);
    const uint64_t w0 = info.written;
    CHECK(check::waitFor(
        [&] {
            a->read_scope(s.raw(), FOXAPI_SCOPE_AUDIO, &info, buf.data(), 16);
            return info.written > w0;
        },
        2000));
    CHECK(check::waitFor([&] { return a->read_scope(s.raw(), FOXAPI_SCOPE_IQ, &info, buf.data(), 512) == 512; },
                         3000));
    CHECK_EQ(info.channels, 2u);
    CHECK(info.rateHz > 0.0);
    CHECK_EQ(a->read_scope(s.raw(), 99u, &info, buf.data(), 16), FOXAPI_UNSUPPORTED);
    CHECK_EQ(a->read_scope(s.raw(), FOXAPI_SCOPE_AUDIO, nullptr, buf.data(), 16), 16);  // info may be NULL
    FoxScopeInfo shortInfo{};
    shortInfo.structSize = 8;
    CHECK_EQ(a->read_scope(s.raw(), FOXAPI_SCOPE_AUDIO, &shortInfo, buf.data(), 16), FOXAPI_BAD_ARGUMENT);
    // REDUCED for a REMOTE session: once the tap holds more than that, a
    // LOCAL session is given what it asks and a REMOTE one no more.
    CHECK(check::waitFor([&] { return a->read_scope(s.raw(), FOXAPI_SCOPE_AUDIO, &info, buf.data(), 8192) == 8192; },
                         5000));
    const int32_t got = a->read_scope(r.raw(), FOXAPI_SCOPE_AUDIO, &info, buf.data(), 65536);
    std::printf("C: a remote session asked for 65536 audio frames and got %d\n", got);
    CHECK_EQ(got, static_cast<int32_t>(FOXAPI_SCOPE_REMOTE_FRAMES));
    s.run(cmd(FOXAPI_OP_RUN, 0, 0));
}

void testNodeSpectrum() {
    testing::Engine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    FoxSpectrumInfo info{};
    info.structSize = sizeof(info);
    float bins[64];
    CHECK_EQ(api()->read_node_spectrum(s.raw(), 1u, 0, &info, bins, 64), FOXAPI_NOT_FOUND);
    CHECK_EQ(count(s, FOXAPI_LIST_PATCH_RADIOS), 0);
    info.structSize = 4;
    CHECK_EQ(api()->read_node_spectrum(s.raw(), 1u, 0, &info, bins, 64), FOXAPI_BAD_ARGUMENT);
}

void testSerialPorts() {
    testing::Engine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    Remote r(e);
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SCAN_SERIAL_PORTS)).result.status, FOXAPI_OK);
    CHECK(count(s, FOXAPI_LIST_SERIAL_PORTS) >= 0);
    CHECK_EQ(r.run(cmd(FOXAPI_OP_SCAN_SERIAL_PORTS)).status(), FOXAPI_DENIED);
}

void testDecoderLines() {
    testing::Engine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK_EQ(count(s, FOXAPI_LIST_DECODER_LINES), 0);
    // Both in ONE batch, so both lines reach the list in the same pass.
    const FoxCommand two[2] = {cmd(FOXAPI_OP_SETTING_SET, 0, 0, "test.decoderLine=POCSAG|1234567: HELLO"),
                               cmd(FOXAPI_OP_SETTING_SET, 0, 0, "test.decoderLine=ACARS|.N123AB")};
    CHECK_EQ(api()->submit(s.raw(), two, 2, nullptr), 2);
    CHECK(check::waitFor([&] { return count(s, FOXAPI_LIST_DECODER_LINES) == 2; }, 2000));
    const FoxListItem l0 = item(s, FOXAPI_LIST_DECODER_LINES, 0);
    const FoxListItem l1 = item(s, FOXAPI_LIST_DECODER_LINES, 1);
    CHECK_EQ(std::string(l0.name), std::string("POCSAG"));
    CHECK_EQ(std::string(l0.detail), std::string("1234567: HELLO"));
    CHECK_EQ(std::string(l1.name), std::string("ACARS"));
    CHECK_EQ(l0.id, 1ull);       // line numbers, oldest first
    CHECK_EQ(l1.id, 2ull);
}

void testStore() {
    testing::Engine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK_EQ(count(s, FOXAPI_LIST_CATALOGUE), 0);
    const std::string legal = "You must hold a licence to decode this. " + std::string(400, 'L');
    FoxCommand c = cmd(FOXAPI_OP_SETTING_SET);
    CHECK_EQ(runText(s, c, "test.catalogue=demo.id|Demo decoder|" + legal).result.status, FOXAPI_OK);
    CHECK(check::waitFor([&] { return count(s, FOXAPI_LIST_CATALOGUE) == 1; }, 2000));
    const FoxListItem it = item(s, FOXAPI_LIST_CATALOGUE, 0);
    CHECK_EQ(std::string(it.name), std::string("Demo decoder"));
    CHECK_EQ(std::string(it.detail), std::string("demo.id"));
    CHECK((it.flags & FOXAPI_ITEM_INSTALLED) == 0u);
    CHECK_EQ(fact(s, "fact.store.demo.id.legal"), legal);
    int32_t st = 0;
    fact(s, "fact.store.other.id.legal", &st);
    CHECK_EQ(st, FOXAPI_NOT_FOUND);
    CHECK_EQ(count(s, FOXAPI_LIST_STORE_BLOCKED), 0);
    CHECK(count(s, FOXAPI_LIST_STORE_UPDATES) >= 0);
}

void testBiasTee() {
    testing::Engine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    const FoxReceiverState st = s.state();
    CHECK((st.flags & (FOXAPI_RX_BIAS_TEE_PRESENT | FOXAPI_RX_BIAS_TEE_ON)) == 0u);
    CHECK_EQ(fact(s, "fact.biasTee.reachable"), std::string("0"));
    CHECK((st.flags & FOXAPI_RX_PATCH_RUNNING) == 0u);
}

}  // namespace

int main() {
    engine_api_test::hermetic("api03");
    cascade::gui::AppWindowTestAccess::installFakeRadio();
    testVersionGate();
    testEveryListIsServed();
    testSubmitText();
    testStatusLines();
    testFacts();
    testPatchGraph();
    testRecordings();
    testScope();
    testNodeSpectrum();
    testSerialPorts();
    testDecoderLines();
    testStore();
    testBiasTee();
    return check::finish("test_engine_api_03");
}
