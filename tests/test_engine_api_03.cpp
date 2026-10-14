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
//   X   the transmitter as its page draws it (state, 8.1): split, input,
//       tone, monitor, the board's power range, the device, where it would
//       transmit, the power setting with no board open
//   G   the lists BEFORE THE FIRST PUBLISH: a session opened on an engine no
//       pass has run on (what gui::AppWindow does in its constructor) reads
//       every list as empty; the bookmarks list used to dereference a block
//       that has none yet (review F4)
//   Y   the status column as its page draws it (state, 8.2, and the two list
//       changes): the sink's words, the output ring and its rate, the decoder
//       output total (a measurement), who mutes and who plays (the texts move
//       audioSeq), the GAINS unit token, the recordings' elapsed seconds
//   U   the usage report's panel names (FOXAPP_OP_NOTE_PANEL, window port
//       group 3): a LOCAL session names a panel, the engine lists it once and
//       stops at 16; a REMOTE session and an empty name add nothing. Read back from the engine's own list, with the
//       passes driven from the test thread (no control thread, no race).
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
    // The panel names the usage report would list (group U).
    static std::vector<std::string> panels(cascade::engine::Engine& e) { return e.telemetryPanels_; }
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

void testTransmitterState() {
    testing::Engine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    // No transmitter: nothing open, but a power setting is still the setting
    // (never the 0 dB - full power - a closed board's readback would say),
    // and it would transmit on the receiver's dial.
    CHECK_EQ(s.run(cmd(FOXAPI_OP_TX_SET_POWER, -40.0)).result.status, FOXAPI_OK);
    FoxReceiverState st = s.state();
    CHECK((st.flags & FOXAPI_RX_TX_AVAILABLE) == 0u);
    CHECK_EQ(std::string(st.txDeviceName), std::string());
    CHECK_EQ(st.txPowerDb, -40.0);
    CHECK_EQ(st.txFrequencyHz, st.tunedHz);
    CHECK((st.txFlags & FOXAPI_TX_POWER_RANGE) == 0u);
    // The dummy load (testHooks only) as the transmitter.
    CHECK_EQ(s.run(cmd(FOXAPI_OP_TX_OPEN, 0, 0, "test:tx:dummy")).result.status, FOXAPI_OK);
    st = s.state();
    CHECK((st.flags & FOXAPI_RX_TX_AVAILABLE) != 0u);
    CHECK_EQ(std::string(st.txDeviceName), std::string("Test transmitter (dummy load, no RF)"));
    CHECK((st.txFlags & FOXAPI_TX_POWER_RANGE) != 0u);
    CHECK_EQ(st.txPowerMinDb, -89.75);
    CHECK_EQ(st.txPowerMaxDb, 0.0);
    CHECK((st.txFlags & FOXAPI_TX_FAULTED) == 0u);
    CHECK_EQ(std::string(st.txError), std::string());
    // Settings, each read back in the state and each moving txSeq.
    uint64_t seq = st.txSeq;
    // The tone (the default: choosing the microphone would open this
    // machine's input device, which no test here does).
    CHECK_EQ(s.run(cmd(FOXAPI_OP_TX_SET_INPUT, 0, 1)).result.status, FOXAPI_OK);
    st = s.state();
    CHECK_EQ(st.txInput, 1u);
    CHECK_EQ(s.run(cmd(FOXAPI_OP_TX_SET_TONE, 1750.0)).result.status, FOXAPI_OK);
    st = s.state();
    CHECK_EQ(st.txToneHz, 1750.0);
    CHECK(st.txSeq > seq);
    seq = st.txSeq;
    CHECK_EQ(s.run(cmd(FOXAPI_OP_TX_SET_MONITOR, 0, 1)).result.status, FOXAPI_OK);
    st = s.state();
    CHECK((st.txFlags & FOXAPI_TX_MONITOR) != 0u);
    CHECK(st.txSeq > seq);
    // SPLIT: the transmitter leaves the receiver's dial.
    FoxCommand split = cmd(FOXAPI_OP_TX_SET_SPLIT, 435.0e6, 1);
    CHECK_EQ(s.run(split).result.status, FOXAPI_OK);
    st = s.state();
    CHECK((st.txFlags & FOXAPI_TX_SPLIT) != 0u);
    CHECK_EQ(st.txSplitHz, 435.0e6);
    CHECK_EQ(st.txFrequencyHz, 435.0e6);
    // ...and back: it follows the receiver again, retuned in the pass.
    CHECK_EQ(s.run(cmd(FOXAPI_OP_TX_SET_SPLIT, 0.0, 0)).result.status, FOXAPI_OK);
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SET_FREQUENCY, 145.5e6)).result.status, FOXAPI_OK);
    st = s.state();
    CHECK((st.txFlags & FOXAPI_TX_SPLIT) == 0u);
    CHECK_EQ(st.txFrequencyHz, st.tunedHz);
    // The input peak is a MEASUREMENT: it never moves txSeq by itself.
    CHECK(st.txInputPeak >= 0.0 && st.txInputPeak <= 1.0);
    CHECK_EQ(s.run(cmd(FOXAPI_OP_TX_CLOSE)).result.status, FOXAPI_OK);
}

// --- Y: the status column as its page draws it (API 0.3 8.2) ----------------------

// The state once it has stopped changing: the audio open the engine starts at
// boot (on a worker) and its first passes have landed - `seq` has held still
// for 300 ms - so a test that compares counters before and after a command
// is not comparing against a sink that was still opening.
FoxReceiverState settled(testing::Session& s) {
    using Clock = std::chrono::steady_clock;
    FoxReceiverState st = s.state();
    auto since = Clock::now();
    const auto end = since + std::chrono::seconds(10);
    while (Clock::now() < end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        FoxReceiverState now = s.state();
        if (now.seq != st.seq) {
            since = Clock::now();
        } else if (Clock::now() - since >= std::chrono::milliseconds(300)) {
            return now;
        }
        st = now;
    }
    return st;
}

int32_t setting(testing::Session& s, const std::string& keyValue) {
    return s.run(cmd(FOXAPI_OP_SETTING_SET, 0, 0, keyValue.c_str())).result.status;
}

// AUDIO and SINK and DECODER OUTPUT and DECODERS: the state's new members.
void testStatusColumnState() {
    testing::Engine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    FoxReceiverState st = settled(s);
    std::printf("Y: sink flags 0x%x, sinkName '%s', ring %llu of %llu frames at %.0f Hz, running %u of %u fitted (%u matched)\n",
                st.audioFlags, st.sinkName, static_cast<unsigned long long>(st.audioRingFrames),
                static_cast<unsigned long long>(st.audioRingCapacityFrames), st.audioRateHz, st.decodersRunning,
                st.decodersFitted, st.decodersMatched);

    // THE SINK'S WORDS with a hermetic engine: whether this machine has an
    // output device or not, they tell the same story as 0.2's own members.
    CHECK((st.audioFlags & FOXAPI_AUDIO_OPENING) == 0u);  // settled: no open is still going
    CHECK((st.audioFlags & FOXAPI_AUDIO_PLUGIN_PLAYING) == 0u);
    CHECK_EQ(std::string(st.audioPlayingPlugin), std::string());
    CHECK_EQ(std::string(st.audioMutedBy), std::string());
    const bool everOpened = (st.audioFlags & FOXAPI_AUDIO_EVER_OPENED) != 0u;
    // sinkName is the device the last successful open landed on: there is one
    // exactly when an open has succeeded.
    CHECK_EQ(everOpened, std::strlen(st.sinkName) != 0u);
    // ...a stream cannot be alive that was never opened, and the 0.2 flag for
    // an open sink says the same as the watchdog's view.
    CHECK_EQ((st.audioFlags & FOXAPI_AUDIO_STREAM_ALIVE) != 0u, (st.flags & FOXAPI_RX_SINK_OPEN) != 0u);
    if ((st.audioFlags & FOXAPI_AUDIO_STREAM_ALIVE) != 0u) { CHECK(everOpened); }

    // THE RING AND ITS RATE: the finished audio is 48 kHz; the ring's size is
    // fixed by the sink (present with or without a device) and its fill never
    // exceeds it; the level the AUDIO card letters is a real number of
    // milliseconds.
    CHECK_EQ(st.audioRateHz, 48000.0);
    CHECK(st.audioRingCapacityFrames > 0u);
    // ...and a ring that is being served is never found brim full: the fill
    // is not the size (it is 0 with no device, a few thousand frames with one).
    CHECK(st.audioRingFrames < st.audioRingCapacityFrames);
    const double capMs = 1000.0 * static_cast<double>(st.audioRingCapacityFrames) / st.audioRateHz;
    CHECK(capMs >= 10.0 && capMs <= 10000.0);

    // THE DECODERS: no plugin is loaded here. Running <= matched <= fitted
    // always (matched is the count of a stopped receiver too).
    CHECK_EQ(st.decodersFitted, 0u);
    CHECK_EQ(st.decodersMatched, 0u);
    CHECK(st.decodersRunning <= st.decodersMatched && st.decodersMatched <= st.decodersFitted);

    // DECODER OUTPUT: the total climbs with the lines the decoders emit, the
    // DECODER_LINES ids are numbered from the same counter, and a line - a
    // MEASUREMENT - moves neither seq nor audioSeq (only the lists' counter:
    // the line is a list entry).
    const FoxReceiverState base = settled(s);
    const FoxCommand three[3] = {cmd(FOXAPI_OP_SETTING_SET, 0, 0, "test.decoderLine=POCSAG|one"),
                                 cmd(FOXAPI_OP_SETTING_SET, 0, 0, "test.decoderLine=POCSAG|two"),
                                 cmd(FOXAPI_OP_SETTING_SET, 0, 0, "test.decoderLine=ACARS|three")};
    CHECK_EQ(api()->submit(s.raw(), three, 3, nullptr), 3);
    CHECK(check::waitFor([&] { return s.state().decoderLinesTotal == base.decoderLinesTotal + 3u; }, 2000));
    st = s.state();
    CHECK_EQ(st.decoderLinesTotal, base.decoderLinesTotal + 3u);
    CHECK_EQ(st.seq, base.seq);
    CHECK_EQ(st.audioSeq, base.audioSeq);
    CHECK(st.listSeq != base.listSeq);
    const int32_t lines = count(s, FOXAPI_LIST_DECODER_LINES);
    CHECK(lines >= 3);
    CHECK_EQ(item(s, FOXAPI_LIST_DECODER_LINES, static_cast<uint32_t>(lines - 1)).id, st.decoderLinesTotal);
    // A RING OF 500 IS NOT A TOTAL: past the backlog's size the list stops
    // growing and the total keeps counting (a rate differenced over a window
    // must not flatten at 500).
    {
        const uint64_t before = st.decoderLinesTotal;
        const std::vector<FoxCommand> batch(100, cmd(FOXAPI_OP_SETTING_SET, 0, 0, "test.decoderLine=SPAM|x"));
        for (uint64_t round = 1; round <= 6; ++round) {
            CHECK_EQ(api()->submit(s.raw(), batch.data(), 100, nullptr), 100);
            CHECK(check::waitFor([&] { return s.state().decoderLinesTotal == before + 100u * round; }, 3000));
            FoxCommandResult drain[16];  // the answers are read, as a session must
            for (auto& x : drain) { x.structSize = sizeof(x); }
            while (api()->poll_results(s.raw(), drain, 16) > 0) {}
        }
        st = s.state();
        CHECK_EQ(st.decoderLinesTotal, before + 600u);
        CHECK_EQ(count(s, FOXAPI_LIST_DECODER_LINES), 500);
        CHECK_EQ(item(s, FOXAPI_LIST_DECODER_LINES, 499).id, st.decoderLinesTotal);
    }

    // WHO MUTES: the decoder plugins whose preset the receiver sits on. Two
    // of them, on one frequency, are named as the mute banner names them. The
    // text moves audioSeq (and so seq) once, when it appears and when it goes.
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SET_FREQUENCY, 100.0e6)).result.status, FOXAPI_OK);
    CHECK_EQ(setting(s, "test.muteBy=Alpha decoder@145500000;Beta decoder@145500000"), FOXAPI_OK);
    const FoxReceiverState off = settled(s);
    CHECK_EQ(std::string(off.audioMutedBy), std::string());  // not on their preset yet
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SET_FREQUENCY, 145.5e6)).result.status, FOXAPI_OK);
    CHECK(check::waitFor([&] { return std::string(s.state().audioMutedBy) == "Alpha decoder and Beta decoder"; }, 2000));
    const FoxReceiverState on = settled(s);
    CHECK_EQ(std::string(on.audioMutedBy), std::string("Alpha decoder and Beta decoder"));
    CHECK_EQ(on.audioSeq, off.audioSeq + 1u);
    CHECK(on.seq > off.seq);
    // Tuned away with the plugins still running, the mute's subject stays (the
    // audio is kept down until the operator answers); with them gone it ends.
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SET_FREQUENCY, 100.0e6)).result.status, FOXAPI_OK);
    CHECK_EQ(setting(s, "test.muteBy="), FOXAPI_OK);
    CHECK(check::waitFor([&] { return std::string(s.state().audioMutedBy).empty(); }, 2000));
    const FoxReceiverState gone = settled(s);
    CHECK_EQ(gone.audioSeq, on.audioSeq + 1u);
    // The USER'S mute is the 0.2 flag and not this text: it moves audioSeq all
    // the same, and leaves the subject empty.
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SET_MUTED, 0, 1)).result.status, FOXAPI_OK);
    st = s.state();
    CHECK((st.flags & FOXAPI_RX_MUTED) != 0u);
    CHECK_EQ(std::string(st.audioMutedBy), std::string());
    CHECK(st.audioSeq > gone.audioSeq);
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SET_MUTED, 0, 0)).result.status, FOXAPI_OK);

    // WHO PLAYS: a plugin holding the speakers replaces the receiver's audio.
    // The flag and the name arrive together (one audioSeq step); the gap
    // counters are MEASUREMENTS and move nothing when only they change. The
    // receiver is stopped first: a running one pulls blocks, and the first
    // block drops a stand-in that has no audio API behind it.
    CHECK_EQ(s.run(cmd(FOXAPI_OP_RUN, 0, 0)).result.status, FOXAPI_OK);
    const FoxReceiverState quiet = settled(s);
    CHECK_EQ(setting(s, "test.audioPlaying=DAB Radio|7|960"), FOXAPI_OK);
    CHECK(check::waitFor([&] { return (s.state().audioFlags & FOXAPI_AUDIO_PLUGIN_PLAYING) != 0u; }, 2000));
    const FoxReceiverState playing = settled(s);
    CHECK_EQ(std::string(playing.audioPlayingPlugin), std::string("DAB Radio"));
    CHECK_EQ(playing.audioPluginGaps, 7u);
    CHECK_EQ(playing.audioPluginGapFrames, 960u);
    CHECK_EQ(playing.audioSeq, quiet.audioSeq + 1u);
    CHECK_EQ(setting(s, "test.audioPlaying=DAB Radio|9|1200"), FOXAPI_OK);
    CHECK(check::waitFor([&] { return s.state().audioPluginGaps == 9u; }, 2000));
    st = settled(s);
    CHECK_EQ(st.audioPluginGapFrames, 1200u);
    CHECK_EQ(st.audioSeq, playing.audioSeq);  // only counters moved
    CHECK_EQ(st.seq, playing.seq);
    // ...and another plugin taking the speakers is the TEXT alone moving.
    CHECK_EQ(setting(s, "test.audioPlaying=Packet TNC|9|1200"), FOXAPI_OK);
    CHECK(check::waitFor([&] { return std::string(s.state().audioPlayingPlugin) == "Packet TNC"; }, 2000));
    const FoxReceiverState other = settled(s);
    CHECK((other.audioFlags & FOXAPI_AUDIO_PLUGIN_PLAYING) != 0u);
    CHECK_EQ(other.audioFlags, st.audioFlags);
    CHECK_EQ(other.audioSeq, st.audioSeq + 1u);
    CHECK_EQ(setting(s, "test.audioPlaying="), FOXAPI_OK);
    CHECK(check::waitFor([&] { return (s.state().audioFlags & FOXAPI_AUDIO_PLUGIN_PLAYING) == 0u; }, 2000));
    st = settled(s);
    CHECK_EQ(std::string(st.audioPlayingPlugin), std::string());
    CHECK_EQ(st.audioPluginGaps, 0u);
    CHECK_EQ(st.audioSeq, other.audioSeq + 1u);

    // THE STREAM DIES under a healthy receiver (a USB sound card
    // re-enumerating: the SINK card's STOPPED): the watchdog's view falls with
    // the 0.2 open flag while EVER_OPENED stays, and the engine's own watchdog
    // then reopens it. Only on a machine that has an output device to lose.
    if (everOpened) {
        CHECK((st.audioFlags & FOXAPI_AUDIO_STREAM_ALIVE) != 0u);
        CHECK_EQ(setting(s, "test.audioClose="), FOXAPI_OK);
        st = s.state();
        CHECK((st.audioFlags & FOXAPI_AUDIO_STREAM_ALIVE) == 0u);
        CHECK((st.audioFlags & FOXAPI_AUDIO_EVER_OPENED) != 0u);
        CHECK((st.flags & FOXAPI_RX_SINK_OPEN) == 0u);
        CHECK(check::waitFor([&] { return (s.state().audioFlags & FOXAPI_AUDIO_STREAM_ALIVE) != 0u; }, 15000));
        const FoxReceiverState back = settled(s);
        CHECK((back.audioFlags & FOXAPI_AUDIO_STREAM_ALIVE) != 0u);
        CHECK((back.audioFlags & FOXAPI_AUDIO_OPENING) == 0u);
        CHECK((back.flags & FOXAPI_RX_SINK_OPEN) != 0u);
    }
}

// RECEIVER: the GAINS entries carry the unit token.
void testStatusColumnGains() {
    testing::Engine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK_EQ(count(s, FOXAPI_LIST_GAINS), 0);  // no radio, no stages
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SCAN_DEVICES)).result.status, FOXAPI_OK);
    CHECK(check::waitFor([&] { return count(s, FOXAPI_LIST_DEVICES) == 2; }, 3000));
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SELECT_SOURCE, 0, 0, engine_api_test::kFakeRadioId)).result.status, FOXAPI_OK);
    CHECK(check::waitFor([&] { return (s.state().flags & FOXAPI_RX_DEVICE_OPEN) != 0u; }, 3000));
    CHECK(check::waitFor([&] { return count(s, FOXAPI_LIST_GAINS) == 1; }, 2000));
    // The fake radio's one stage is a gain in decibels.
    FoxListItem g = item(s, FOXAPI_LIST_GAINS, 0);
    CHECK_EQ(std::string(g.name), std::string("TUNER"));
    CHECK_EQ(std::string(g.detail), std::string("dB"));
    CHECK_EQ(g.value[1], 49.6);
    // A radio whose stages are the hardware's own steps (an Airspy's) says
    // so, and the block is rebuilt for it (listSeq).
    const uint64_t seq0 = s.state().listSeq;
    CHECK_EQ(setting(s, "test.gainUnit=step"), FOXAPI_OK);
    CHECK(check::waitFor([&] { return std::string(item(s, FOXAPI_LIST_GAINS, 0).detail) == "step"; }, 2000));
    CHECK(s.state().listSeq != seq0);
    g = item(s, FOXAPI_LIST_GAINS, 0);
    CHECK_EQ(std::string(g.name), std::string("TUNER"));  // the rest of the entry is as it was
    CHECK_EQ(g.value[1], 49.6);
    CHECK_EQ(setting(s, "test.gainUnit=dB"), FOXAPI_OK);
    CHECK(check::waitFor([&] { return std::string(item(s, FOXAPI_LIST_GAINS, 0).detail) == "dB"; }, 2000));
}

// RECORDER: RECORDINGS' value[3] is the take's seconds, read live.
void testStatusColumnRecordings() {
    testing::Engine e(api(), kHooks);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    using Clock = std::chrono::steady_clock;
    CHECK_EQ(s.run(cmd(FOXAPI_OP_RUN, 0, 1)).result.status, FOXAPI_OK);
    const auto t0 = Clock::now();
    CHECK_EQ(s.run(cmd(FOXAPI_OP_RECORD_IQ, 0, 1)).result.status, FOXAPI_OK);
    CHECK(check::waitFor([&] { return count(s, FOXAPI_LIST_RECORDINGS) == 1; }, 2000));
    const double v0 = item(s, FOXAPI_LIST_RECORDINGS, 0).value[3];
    CHECK(v0 >= 0.0);
    // It climbs while the take runs (Windows' timer tick is 15.6 ms: wait
    // well past one), without moving the list's counter.
    const uint64_t listSeq = s.state().listSeq;
    CHECK(check::waitFor([&] { return item(s, FOXAPI_LIST_RECORDINGS, 0).value[3] >= v0 + 0.15; }, 3000));
    CHECK_EQ(s.state().listSeq, listSeq);
    // ...and it is the take's own clock, not the engine's: never more than
    // the wall time since the command that started it.
    const FoxListItem live = item(s, FOXAPI_LIST_RECORDINGS, 0);
    const double wall = std::chrono::duration<double>(Clock::now() - t0).count();
    CHECK((live.flags & FOXAPI_ITEM_ACTIVE) != 0u);
    CHECK(live.value[3] > 0.0 && live.value[3] <= wall);
    // A second take, started later, has run for less: each entry keeps its own.
    CHECK_EQ(s.run(cmd(FOXAPI_OP_RECORD_AUDIO, 0, 1)).result.status, FOXAPI_OK);
    CHECK(check::waitFor([&] { return count(s, FOXAPI_LIST_RECORDINGS) == 2; }, 2000));
    CHECK(check::waitFor([&] { return item(s, FOXAPI_LIST_RECORDINGS, 1).value[3] >= 0.15; }, 3000));
    const FoxListItem iq = item(s, FOXAPI_LIST_RECORDINGS, 0);
    const FoxListItem au = item(s, FOXAPI_LIST_RECORDINGS, 1);
    CHECK_EQ(iq.value[0], 1.0);
    CHECK_EQ(au.value[0], 2.0);
    CHECK((iq.flags & FOXAPI_ITEM_ACTIVE) != 0u && (au.flags & FOXAPI_ITEM_ACTIVE) != 0u);
    CHECK(iq.value[3] > au.value[3]);
    // Stopped, a take keeps the length it had when it ended: fixed, not zero.
    CHECK_EQ(s.run(cmd(FOXAPI_OP_RECORD_IQ, 0, 0)).result.status, FOXAPI_OK);
    CHECK_EQ(s.run(cmd(FOXAPI_OP_RECORD_AUDIO, 0, 0)).result.status, FOXAPI_OK);
    CHECK(check::waitFor(
        [&] {
            return (item(s, FOXAPI_LIST_RECORDINGS, 0).flags & FOXAPI_ITEM_ACTIVE) == 0u &&
                   (item(s, FOXAPI_LIST_RECORDINGS, 1).flags & FOXAPI_ITEM_ACTIVE) == 0u;
        },
        2000));
    const FoxListItem iqEnd = item(s, FOXAPI_LIST_RECORDINGS, 0);
    const FoxListItem auEnd = item(s, FOXAPI_LIST_RECORDINGS, 1);
    CHECK(iqEnd.value[3] >= iq.value[3]);
    CHECK(auEnd.value[3] >= au.value[3]);
    CHECK(iqEnd.value[3] > auEnd.value[3]);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK_EQ(item(s, FOXAPI_LIST_RECORDINGS, 0).value[3], iqEnd.value[3]);
    CHECK_EQ(item(s, FOXAPI_LIST_RECORDINGS, 1).value[3], auEnd.value[3]);
    s.run(cmd(FOXAPI_OP_RUN, 0, 0));
}

// G: a session on an engine that has never run a pass - no list block has been
// published, so read_list answers from an EMPTY one whose bookmarks and presets
// pointers are null. gui::AppWindow opens its session exactly here
// (openUiSession, in its constructor, before the first frame), and
// configureApi drops the block again until the next publish. Nothing may be
// dereferenced: the lists the block carries are empty, and every list is
// served (a count, never an error).
void testListsBeforeFirstPublish() {
    const auto readEveryList = [](testing::Session& s, const char* when) {
        CHECK(s.ok());
        for (uint32_t l = 1; l <= FOXAPI_LIST_PATCH_RADIOS; ++l) {
            if (l == 13u || l == FOXAPI_LIST_TRACKS) { continue; }  // retired / not served
            FoxListItem it{};
            it.structSize = sizeof(it);
            const int32_t n = api()->read_list(s.raw(), l, 0, &it);
            if (n < 0) { std::printf("  G (%s): list %u answered %d\n", when, l, n); }
            CHECK(n >= 0);
        }
    };
    {
        cascade::engine::Engine eng;  // built, never initialised, never passed
        testing::Session s(api(), eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
        // The reported case, on its own and at both calls a client makes.
        CHECK_EQ(api()->read_list(s.raw(), FOXAPI_LIST_BOOKMARKS, 0, nullptr), 0);
        FoxListItem it{};
        it.structSize = sizeof(it);
        CHECK_EQ(api()->read_list(s.raw(), FOXAPI_LIST_BOOKMARKS, 0, &it), 0);
        CHECK_EQ(api()->read_list(s.raw(), FOXAPI_LIST_BOOKMARK_GROUPS, 0, nullptr), 0);
        CHECK_EQ(api()->read_list(s.raw(), FOXAPI_LIST_PRESETS, 0, nullptr), 0);
        readEveryList(s, "fresh engine");
    }
    {
        cascade::engine::Engine eng;
        eng.configureApi(kHooks);  // the host is rebuilt: no block until the next publish
        testing::Session s(api(), eng.apiHandle(), FOXAPI_SESSION_LOCAL, kAllGrants);
        CHECK_EQ(api()->read_list(s.raw(), FOXAPI_LIST_BOOKMARKS, 0, nullptr), 0);
        readEveryList(s, "after configureApi");
    }
}

// U: the usage report's panel names. The engine is driven from THIS thread -
// submit, then controlPass() until the ticket's result lands - so reading its
// list afterwards is not a race with a control thread.
testing::Outcome pumped(cascade::engine::Engine& eng, testing::Session& s, const FoxCommand& c) {
    testing::Outcome o;
    FoxSubmitResult sr{};
    sr.structSize = sizeof(sr);
    const int32_t n = api()->submit(s.raw(), &c, 1, &sr);
    o.submitStatus = n < 0 ? n : sr.status;
    if (o.submitStatus != FOXAPI_OK) { return o; }
    for (int pass = 0; pass < 20 && !o.landed; ++pass) {
        eng.controlPass();
        FoxCommandResult r[16];
        for (auto& x : r) { x.structSize = sizeof(x); }
        const int32_t got = api()->poll_results(s.raw(), r, 16);
        for (int32_t i = 0; i < got; ++i) {
            if (r[i].ticket == sr.ticket) {
                o.landed = true;
                o.result = r[i];
            }
        }
    }
    return o;
}

FoxCommand notePanel(const char* name) { return testing::cmd(FOXAPP_OP_NOTE_PANEL, 0.0, 0, name); }

void testNotePanel() {
    using Names = std::vector<std::string>;
    using Access = cascade::gui::AppWindowTestAccess;
    cascade::engine::Engine eng;
    eng.configureApi(kHooks);
    eng.initialise();
    eng.controlPass();
    // VIEW is the grant it needs, and all it needs.
    testing::Session s(api(), eng.apiHandle(), FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW);
    CHECK(s.ok());
    CHECK(Access::panels(eng).empty());

    // radar twice, cat once: listed once each, in the order first said.
    const testing::Outcome r1 = pumped(eng, s, notePanel("radar"));
    CHECK(r1.landed);
    CHECK_EQ(r1.status(), FOXAPI_OK);
    CHECK_EQ(pumped(eng, s, notePanel("radar")).status(), FOXAPI_OK);
    CHECK_EQ(pumped(eng, s, notePanel("cat")).status(), FOXAPI_OK);
    CHECK(Access::panels(eng) == (Names{"radar", "cat"}));

    // A REMOTE session holds VIEW too, and its pages are not this
    // installation's usage: refused, nothing listed.
    {
        testing::Session r(api(), eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW, "s3cret");
        CHECK(r.ok());
        CHECK_EQ(pumped(eng, r, notePanel("map")).status(), FOXAPI_DENIED);
        CHECK(Access::panels(eng) == (Names{"radar", "cat"}));
    }
    // (There is no session "without VIEW": open_session grants it to every
    // one. The grant is pinned the other way: the LOCAL session above holds
    // VIEW and nothing else, and its names were listed - an op needing
    // SETTINGS, the default, would have been DENIED to it.)
    // An empty name names no panel.
    CHECK_EQ(pumped(eng, s, notePanel("")).status(), FOXAPI_BAD_ARGUMENT);
    CHECK(Access::panels(eng) == (Names{"radar", "cat"}));

    // The cap: 16 names, then no more - and a name already there stays one.
    for (int i = 3; i <= 16; ++i) {
        CHECK_EQ(pumped(eng, s, notePanel(("panel " + std::to_string(i)).c_str())).status(), FOXAPI_OK);
    }
    CHECK_EQ(Access::panels(eng).size(), 16u);
    CHECK_EQ(pumped(eng, s, notePanel("panel 17")).status(), FOXAPI_OK);
    CHECK_EQ(pumped(eng, s, notePanel("radar")).status(), FOXAPI_OK);
    const Names full = Access::panels(eng);
    CHECK_EQ(full.size(), 16u);
    CHECK(std::find(full.begin(), full.end(), std::string("panel 17")) == full.end());
    CHECK_EQ(std::count(full.begin(), full.end(), std::string("radar")), 1);
    std::printf("U: radar x2 and cat x1 listed as {radar, cat}; the 17th name not listed (%zu names)\n",
                full.size());
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
    testTransmitterState();
    testStatusColumnState();
    testStatusColumnGains();
    testStatusColumnRecordings();
    testListsBeforeFirstPublish();
    testNotePanel();
    return check::finish("test_engine_api_03");
}
