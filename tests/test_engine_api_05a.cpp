// test_engine_api_05a.cpp - ENGINE EXTRACTION STAGE 5a: the table's side of web and CAT becoming engine sessions
// (docs/api-0.3-additions.md 8.16; docs/engine-stage4.md section 6.7), held against the real engine through the C table and, where a
// session is the engine's own, through ApiHost::openInternalSession.
//
//   A   read_audio (FOXAPI_CAP_AUDIO_STREAM). A1 the contract's edges: the AUDIO grant (DENIED to a LOCAL and a REMOTE session without
//       it), a null cursor, a null buffer with a count, an info shorter than FOXAPI_MIN_STREAM_INFO (BAD_ARGUMENT, and the cursor does
//       not move), a count of 0; A2 cursor 0 joins LIVE (0 frames, the cursor at the stream's end, channels 1, 48 kHz); A3 the stream
//       itself: the frames the ring was given are the frames produced (a differential: the engine's pass frozen by hand, the
//       receiver stopped, and the bytes read are exactly the tap's newest frames, the same for a LOCAL and a REMOTE reader), a second
//       reader of the same cursor sees the same bytes; A4 a reader that falls behind the 2 s ring is resynced to the oldest frame and
//       told how many it lost (`dropped`, exact); A5 nothing is copied for an engine nobody listens to (the pass feeds for
//       audioDemandMs after the last read, then stops, and the first read after marks live again).
//   K   the capability word: every bit the engine serves is declared and every bit it does not is clear, each probed by calling the
//       feature it promises.
//   P   the internal sessions' CLOSED OP PROFILES. P1 the web profile is exactly the closure of net::controlRequestToCommands (every
//       op any ControlRequest can turn into, and no other - derived here by setting every field, then the ones that yield to another,
//       and compared with the profile both ways), the CAT profile exactly the four ops rigctld's F and M send (derived from
//       executeCatLine); P2 every op there is, asked of each kind of session at the door (validateForTest): off the profile is DENIED,
//       on it is not DENIED by the profile; P3 a violation arrives as a result (DENIED, REFUSED, the profile's sentence) and changes
//       nothing; P4 the public rules for sessions a client opened are UNCHANGED (a REMOTE session: no ADMIN, no application ops, no
//       store); P5 the grants an internal session reports, the REMOTE flag, and the limits (an internal session counts against the
//       16 REMOTE places and the 64).
//   C   CAT's six figures (net::CatRadio) read from a CAT session's own read_state are the figures the window's snapshot read gave.
//   S   FOXAPI_OP_SERVER_CONFIG, the list SERVERS (49) and the facts: S1 defaults; S2 a configuration applied and read back in the
//       facts, the list, and the config the engine writes; S3 the password hashed on a worker, accepted before its record exists,
//       never visible in any text the engine publishes, the record verifying it; S4 every refusal (strict parse, ranges, forms)
//       leaving the configuration as it was; S5 who may ask (LOCAL + ADMIN only) and who may read; S6 the list block's fingerprint:
//       one move for one change, none for the same value said again, none for a counter; S7 applyConfig restores it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "check.hpp"
#include "core/app_commands.hpp"
#include "core/config.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine/server_host.hpp"
#include "engine_api_test_env.hpp"
#include "foxsdr_api.h"
#include "net/cat_protocol.hpp"
#include "net/control_ops.hpp"
#include "net/status_compose.hpp"
#include "net/web_auth.hpp"
#include "net/web_audio.hpp"

namespace cascade::gui {
struct AppWindowTestAccess {
    static std::uint64_t produced(cascade::engine::Engine& e) { return e.pipeline_.audioSamplesProduced(); }
    static std::size_t tap(cascade::engine::Engine& e, float* dst, std::size_t n) { return e.pipeline_.audioTap(dst, n); }
};
}  // namespace cascade::gui

using cascade::engine::Engine;
using cascade::engine::InternalKind;
using Access = cascade::gui::AppWindowTestAccess;
namespace cmd = cascade::core::cmd;

namespace {

const FoxEngineApi* api() { return cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR); }

constexpr std::uint64_t kAllGrants = FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS | FOXAPI_GRANT_TRANSMIT |
                                     FOXAPI_GRANT_ADMIN | FOXAPI_GRANT_AUDIO | FOXAPI_GRANT_IQ;

// An engine built the way the table's create() builds one (cascade::engine::Engine behind the options), with the control thread
// running or not, and torn down the way destroy() tears it down. Direct, so a test can reach the engine's own ApiHost.
struct Rig {
    Engine e;
    bool threaded;
    explicit Rig(const std::string& options, bool thread = true, bool run = true) : threaded(thread) {
        e.configureApi(options);
        e.initialise();
        if (run) { (void)e.applyCommand(cmd::makeInt(FOXAPI_OP_RUN, 1)); }
        e.controlPass();
        if (thread) { e.startControlThread(); }
    }
    ~Rig() { e.shutdown(); }
    // Passes by hand (an engine with no control thread).
    void pump(int n = 2) {
        for (int i = 0; i < n; ++i) { e.pump(); }
    }
};

struct Sess {
    FoxSession* s = nullptr;
    int32_t openStatus = FOXAPI_FAILED;
    Sess(Engine& e, uint32_t flags, uint64_t grants, const char* token = nullptr) {
        FoxSessionParams p{};
        p.structSize = sizeof(p);
        p.flags = flags;
        p.grants = grants;
        p.clientName = "test";
        p.token = token;
        openStatus = api()->open_session(e.apiHandle(), &p, &s);
    }
    explicit Sess(FoxSession* internal) : s(internal), openStatus(internal != nullptr ? FOXAPI_OK : FOXAPI_FAILED) {}
    ~Sess() { close(); }
    Sess(const Sess&) = delete;
    Sess& operator=(const Sess&) = delete;
    void close() {
        if (s != nullptr) {
            api()->close_session(s);
            s = nullptr;
        }
    }
    FoxReceiverState state() const {
        FoxReceiverState st{};
        st.structSize = sizeof(st);
        for (int i = 0; i < 100 && api()->read_state(s, &st) != FOXAPI_OK; ++i) {}
        return st;
    }
};

struct Answer {
    int32_t submit = FOXAPI_FAILED;
    bool landed = false;
    FoxCommandResult r{};
    int32_t status() const { return submit != FOXAPI_OK ? submit : r.status; }
};

// One command (optionally with a long text), waiting for its result: by the control thread, or by passes by hand.
Answer run(Rig& rig, Sess& s, FoxCommand c, const std::string* longText = nullptr, int ms = 3000) {
    Answer a;
    c.structSize = sizeof(c);
    FoxSubmitResult sr{};
    sr.structSize = sizeof(sr);
    const int32_t n = longText != nullptr
                          ? api()->submit_text(s.s, &c, longText->data(), static_cast<uint32_t>(longText->size()), &sr)
                          : api()->submit(s.s, &c, 1, &sr);
    a.submit = n < 0 ? n : sr.status;
    if (a.submit != FOXAPI_OK) { return a; }
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (!rig.threaded) { rig.pump(1); }
        FoxCommandResult r[16];
        for (auto& x : r) { x.structSize = sizeof(x); }
        const int32_t got = api()->poll_results(s.s, r, 16);
        for (int32_t i = 0; i < got; ++i) {
            if (r[i].ticket == sr.ticket) {
                a.landed = true;
                a.r = r[i];
            }
        }
        if (a.landed) { return a; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return a;
}

std::string fact(Sess& s, const char* key, int32_t* status = nullptr) {
    char buf[4096];
    const int32_t n = api()->get_setting(s.s, key, buf, sizeof(buf));
    if (status != nullptr) { *status = n; }
    return n >= 0 ? std::string(buf) : std::string();
}

int32_t count(Sess& s, uint32_t list) { return api()->read_list(s.s, list, 0, nullptr); }

FoxListItem item(Sess& s, uint32_t list, uint32_t index) {
    FoxListItem it{};
    it.structSize = sizeof(it);
    api()->read_list(s.s, list, index, &it);
    return it;
}

bool waitUntil(int ms, const std::function<bool()>& pred) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > end) { return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

// ===================================================================================================================================
// A. read_audio
// ===================================================================================================================================

void testReadAudioEdges() {
    Rig r("testHooks=1;token=s3cret;audioDemandMs=400");
    Sess all(r.e, FOXAPI_SESSION_LOCAL, kAllGrants);
    Sess noAudio(r.e, FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_SETTINGS);
    Sess remoteNo(r.e, FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_SETTINGS, "s3cret");
    Sess remote(r.e, FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_AUDIO, "s3cret");
    CHECK(all.openStatus == FOXAPI_OK && noAudio.openStatus == FOXAPI_OK && remoteNo.openStatus == FOXAPI_OK &&
          remote.openStatus == FOXAPI_OK);

    float buf[4800];
    std::uint64_t cursor = 0;
    // The grant, for both kinds of session.
    CHECK_EQ(api()->read_audio(noAudio.s, &cursor, buf, 4800, nullptr), FOXAPI_DENIED);
    CHECK_EQ(api()->read_audio(remoteNo.s, &cursor, buf, 4800, nullptr), FOXAPI_DENIED);
    CHECK_EQ(cursor, 0ull);
    // ...and a refusal for a missing grant does NOT start the feed (nobody who may not hear has asked to).
    CHECK(fact(all, "test.audioFeed").find("feeding=0") != std::string::npos);

    // Arguments.
    CHECK_EQ(api()->read_audio(all.s, nullptr, buf, 4800, nullptr), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(api()->read_audio(all.s, &cursor, nullptr, 4800, nullptr), FOXAPI_BAD_ARGUMENT);
    FoxStreamInfo shortInfo{};
    shortInfo.structSize = static_cast<uint32_t>(FOXAPI_MIN_STREAM_INFO - 8);
    std::uint64_t probe = 12345;
    CHECK_EQ(api()->read_audio(all.s, &probe, buf, 4800, &shortInfo), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(probe, 12345ull);  // the cursor did not move: no audio was lost to the refusal
    CHECK(fact(all, "test.audioFeed").find("feeding=0") != std::string::npos);  // ...and nothing asked for the feed

    // A count of 0 with no buffer is a legal call (the cursor still joins live when it is 0).
    FoxStreamInfo info{};
    info.structSize = sizeof(info);
    cursor = 0;
    CHECK_EQ(api()->read_audio(all.s, &cursor, nullptr, 0, &info), 0);
    // A2: joined at the stream's end, 0 frames, mono at 48 kHz.
    CHECK_EQ(info.channels, 1u);
    CHECK_EQ(info.rateHz, 48000.0);
    CHECK_EQ(cursor, info.written);
    CHECK(cursor >= 1u);  // a joined cursor is never 0 (the ring starts with a frame of silence)
    CHECK(fact(all, "test.audioFeed").find("feeding=1") != std::string::npos);
    // An info of exactly the minimum is served; a roomier one is filled to its structSize and no further.
    FoxStreamInfo exact{};
    exact.structSize = static_cast<uint32_t>(FOXAPI_MIN_STREAM_INFO);
    std::uint64_t c2 = 0;
    CHECK_EQ(api()->read_audio(remote.s, &c2, buf, 4800, &exact), 0);
    CHECK_EQ(exact.channels, 1u);
    CHECK_EQ(exact.rateHz, 48000.0);
    // A reader that reads from a cursor it was given reads frames and advances it by exactly what it got.
    std::int64_t total = 0;
    const std::uint64_t start = cursor;
    CHECK(waitUntil(3000, [&] {
        const int32_t n = api()->read_audio(all.s, &cursor, buf, 4800, &info);
        if (n > 0) { total += n; }
        return total >= 2400;
    }));
    CHECK_EQ(static_cast<std::uint64_t>(total), cursor - start);
    CHECK_EQ(info.dropped, 0ull);
    // (what the LAST call put in the buffer is whole and finite)
    bool finite = true;
    const int32_t lastN = api()->read_audio(all.s, &cursor, buf, 4800, &info);
    for (int32_t i = 0; i < lastN; ++i) { finite = finite && std::isfinite(buf[i]); }
    CHECK(finite);
    // The state shows the grants (AUDIO among them) the session holds.
    CHECK((all.state().grants & FOXAPI_GRANT_AUDIO) != 0u);
    CHECK((remote.state().grants & FOXAPI_GRANT_AUDIO) != 0u);
}

// A3: THE DIFFERENTIAL. Nothing runs the engine's pass (no control thread), so nothing feeds the ring but a read: the ring's content is
// what a read copied from the pipeline's tap by its produced-counter - the body of AppWindow::publishWebAudio, which this replaced.
// The receiver is stopped before the last read, so the tap does not move under the copy, and what the reader gets of that last feed must
// be EXACTLY the tap's newest frames: the bytes the old ring would have held, the same for every reader of the same stream.
void testReadAudioIsTheTap() {
    Rig r("testHooks=1;token=s3cret", /*thread=*/false);
    Sess local(r.e, FOXAPI_SESSION_LOCAL, kAllGrants);
    Sess remote(r.e, FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_AUDIO, "s3cret");
    // Wait for audio to be produced at all (the generator on its own clock).
    CHECK(waitUntil(5000, [&] { return Access::produced(r.e) > 20000; }));
    std::uint64_t lc = 0, rc = 0;
    FoxStreamInfo li{}, ri{};
    li.structSize = ri.structSize = sizeof(li);
    float scratch[8];
    // The first read joins (and marks live); 30 ms later a read takes what was made since, so the cursor stands at the stream's end.
    CHECK_EQ(api()->read_audio(local.s, &lc, scratch, 0, &li), 0);
    CHECK(lc >= 1u);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    {
        std::vector<float> first(8192);
        CHECK(api()->read_audio(local.s, &lc, first.data(), 8192, &li) > 0);
    }
    CHECK_EQ(lc, li.written);
    CHECK_EQ(api()->read_audio(remote.s, &rc, scratch, 0, &ri), 0);  // the same stream, a second reader, joined at ITS end
    CHECK(rc >= lc);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    // Stop the receiver and wait for the count to stand still: no pass runs here, so the tap is frozen at what the receiver made.
    CHECK(r.e.applyCommand(cmd::makeInt(FOXAPI_OP_RUN, 0)).status == FOXAPI_OK);
    std::uint64_t last = ~0ull;
    CHECK(waitUntil(5000, [&] {
        const std::uint64_t now = Access::produced(r.e);
        const bool still = now == last;
        last = now;
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        return still;
    }));
    std::vector<float> a(8192), b(8192), o(8192), tap(4096);
    const int32_t na = api()->read_audio(local.s, &lc, a.data(), 8192, &li);
    const int32_t nb = api()->read_audio(remote.s, &rc, b.data(), 8192, &ri);
    std::printf("A3: the local reader got %d frames, the remote %d (the last feed)\n", na, nb);
    CHECK(nb > 0 && nb <= 4096);       // one feed: the tap holds no more
    CHECK(na >= nb);                   // local also holds the second reader's join feed
    CHECK_EQ(li.dropped, 0ull);
    CHECK_EQ(ri.dropped, 0ull);
    CHECK_EQ(li.written, ri.written);
    CHECK_EQ(lc, rc);
    const std::size_t haveTap = Access::tap(r.e, tap.data(), tap.size());
    CHECK(haveTap >= static_cast<std::size_t>(nb));
    // The old feed, to the letter, into the old ring: the newest nb frames of the tap.
    cascade::net::AudioRing old;
    old.write(tap.data() + (haveTap - static_cast<std::size_t>(nb)), static_cast<std::size_t>(nb));
    std::uint64_t oc = 0, od = 0;
    const std::size_t no = old.read(oc, o.data(), 8192, od);
    CHECK_EQ(no, static_cast<std::size_t>(nb));
    const std::size_t bytes = sizeof(float) * static_cast<std::size_t>(nb);
    CHECK(std::memcmp(b.data(), o.data(), bytes) == 0);                                   // the remote reader: the old ring's bytes
    CHECK(std::memcmp(a.data() + (na - nb), b.data(), bytes) == 0);                       // the local reader: the same bytes
    CHECK(std::memcmp(b.data(), tap.data() + (haveTap - static_cast<std::size_t>(nb)), bytes) == 0);  // ...which are the tap's newest
    // Nothing further: the receiver is stopped, the tap does not move.
    CHECK_EQ(api()->read_audio(local.s, &lc, a.data(), 8192, &li), 0);
}

// A4: a reader that fell behind. The ring holds 96 000 frames (two seconds); a reader whose cursor is older than its oldest frame is
// resynced to that frame and told EXACTLY how many it missed. Real time: the generator makes 48 000 frames a second.
void testReadAudioOverrun() {
    Rig r("testHooks=1;audioDemandMs=4000");
    Sess s(r.e, FOXAPI_SESSION_LOCAL, kAllGrants);
    std::uint64_t cursor = 0;
    FoxStreamInfo info{};
    info.structSize = sizeof(info);
    float scratch[8];
    CHECK(waitUntil(5000, [&] { return Access::produced(r.e) > 5000; }));
    CHECK(waitUntil(3000, [&] {
        (void)api()->read_audio(s.s, &cursor, scratch, 0, &info);  // (a cursor of 0 joins again until the ring holds a frame)
        return cursor != 0u;
    }));
    const std::uint64_t joined = cursor;
    // (no reads: the pass keeps the ring fed, because a reader asked within audioDemandMs)
    CHECK(waitUntil(6000, [&] { return Access::produced(r.e) > 5000 + 110000; }));
    std::vector<float> buf(4096);
    const int32_t n = api()->read_audio(s.s, &cursor, buf.data(), 4096, &info);
    const std::uint64_t held = cascade::net::kWebAudioCapacity;
    const std::uint64_t oldest = info.written - held;
    std::printf("A4: written %llu, cursor was %llu, dropped %llu, got %d\n", static_cast<unsigned long long>(info.written),
                static_cast<unsigned long long>(joined), static_cast<unsigned long long>(info.dropped), n);
    CHECK(info.written > joined + held);
    CHECK_EQ(info.dropped, oldest - joined);   // exact: every frame between the cursor and the oldest the ring still has
    CHECK_EQ(n, 4096);
    CHECK_EQ(cursor, oldest + 4096u);          // resynced to the oldest, then advanced by what was read
    // Read on from there: whole, in order, nothing dropped now.
    FoxStreamInfo again{};
    again.structSize = sizeof(again);
    CHECK(api()->read_audio(s.s, &cursor, buf.data(), 4096, &again) > 0);
    CHECK_EQ(again.dropped, 0ull);
}

// A5: nothing is copied for an engine nobody listens to.
void testReadAudioDemand() {
    Rig r("testHooks=1;audioDemandMs=150");
    Sess s(r.e, FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(fact(s, "test.audioFeed").find("feeding=0") != std::string::npos);
    CHECK(fact(s, "test.audioFeed").find("written=0") != std::string::npos);  // no ring at all yet
    std::uint64_t cursor = 0;
    FoxStreamInfo info{};
    info.structSize = sizeof(info);
    float buf[2400];
    CHECK_EQ(api()->read_audio(s.s, &cursor, buf, 2400, &info), 0);
    CHECK(fact(s, "test.audioFeed").find("feeding=1") != std::string::npos);
    CHECK(waitUntil(3000, [&] {
        const int32_t n = api()->read_audio(s.s, &cursor, buf, 2400, &info);
        return n > 0;
    }));
    // Stop asking: within a few passes after the demand has lapsed the feed stops, and the ring stops growing.
    CHECK(waitUntil(3000, [&] { return fact(s, "test.audioFeed").find("feeding=0") != std::string::npos; }));
    const std::string frozen = fact(s, "test.audioFeed");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK_EQ(fact(s, "test.audioFeed"), frozen);
    // The first read after a quiet spell marks live again: the feed starts, and it adds nothing for the gap.
    CHECK(api()->read_audio(s.s, &cursor, buf, 2400, &info) >= 0);
    const std::string resumed = fact(s, "test.audioFeed");
    CHECK(resumed.find("feeding=1") != std::string::npos);
    CHECK_EQ(resumed.substr(resumed.find("written=")), frozen.substr(frozen.find("written=")));
}

// ===================================================================================================================================
// K. the capability word
// ===================================================================================================================================

void testCapabilityWord() {
    Rig r("testHooks=1;token=s3cret", /*thread=*/false);
    Sess s(r.e, FOXAPI_SESSION_LOCAL, kAllGrants);
    const std::uint64_t caps = api()->capabilities;
    FoxListItem tmp{};
    tmp.structSize = sizeof(tmp);
    float fbuf[16];
    std::uint64_t cur = 0;
    FoxStreamInfo si{};
    si.structSize = sizeof(si);
    struct Row {
        std::uint64_t bit;
        const char* name;
        bool served;
        bool probe;  // the feature the bit promises answers (set) / is refused as not implemented (clear)
    };
    const auto known = [](std::uint32_t op) { return cmd::isKnownOp(op); };
    const Row rows[] = {
        {FOXAPI_CAP_RECEIVER, "RECEIVER", true, known(FOXAPI_OP_SET_CENTRE) && known(FOXAPI_OP_SET_MODE)},
        {FOXAPI_CAP_SPECTRUM, "SPECTRUM", true, api()->read_spectrum != nullptr},
        {FOXAPI_CAP_AUDIO_LEVELS, "AUDIO_LEVELS", true, std::isfinite(s.state().audioLevelDb)},
        {FOXAPI_CAP_AUDIO_STREAM, "AUDIO_STREAM", true, api()->read_audio(s.s, &cur, fbuf, 0, &si) != FOXAPI_UNSUPPORTED},
        {FOXAPI_CAP_IQ_STREAM, "IQ_STREAM", false, api()->read_iq(s.s, &cur, fbuf, 16, &si) == FOXAPI_UNSUPPORTED},
        {FOXAPI_CAP_SOURCES, "SOURCES", true,
         api()->read_list(s.s, FOXAPI_LIST_DEVICES, 0, &tmp) >= 1 && known(FOXAPI_OP_SELECT_SOURCE) &&
             known(FOXAPI_OP_SET_SAMPLE_RATE) && known(FOXAPI_OP_SET_ANTENNA) &&
             api()->read_list(s.s, FOXAPI_LIST_SAMPLE_RATES, 0, nullptr) >= 0 &&
             api()->read_list(s.s, FOXAPI_LIST_ANTENNAS, 0, nullptr) >= 0},
        {FOXAPI_CAP_GAINS, "GAINS", true,
         api()->read_list(s.s, FOXAPI_LIST_GAINS, 0, nullptr) >= 0 && known(FOXAPI_OP_SET_GAIN) && known(FOXAPI_OP_SET_DEVICE_AGC)},
        {FOXAPI_CAP_AUDIO_DSP, "AUDIO_DSP", true, known(FOXAPI_OP_SET_NR) && known(FOXAPI_OP_SET_NOTCH) && known(FOXAPI_OP_SET_AUTO_NOTCH)},
        {FOXAPI_CAP_RDS, "RDS", true, s.state().structSize >= FOXAPI_MIN_RECEIVER_STATE},
        {FOXAPI_CAP_DECODERS, "DECODERS", false, true /* the text EVENTS are not emitted (no FOXAPI_EVENT_DECODER_TEXT anywhere in src/engine) */},
        {FOXAPI_CAP_PATCH, "PATCH", true,
         api()->read_list(s.s, FOXAPI_LIST_PATCH_NODES, 0, nullptr) >= 0 && known(FOXAPI_OP_PATCH_RUN) && known(FOXAPP_OP_PATCH_SET_GRAPH)},
        {FOXAPI_CAP_RECORDER, "RECORDER", true,
         api()->read_list(s.s, FOXAPI_LIST_RECORDINGS, 0, nullptr) >= 0 && known(FOXAPI_OP_RECORD_IQ) && known(FOXAPI_OP_RECORD_AUDIO)},
        {FOXAPI_CAP_TRANSMIT, "TRANSMIT", true, known(FOXAPI_OP_TX_OPEN) && known(FOXAPI_OP_TX_PTT)},
        {FOXAPI_CAP_BOOKMARKS, "BOOKMARKS", true,
         api()->read_list(s.s, FOXAPI_LIST_BOOKMARKS, 0, nullptr) >= 0 && known(FOXAPI_OP_BOOKMARK_ADD) && known(FOXAPI_OP_BOOKMARK_TUNE)},
        {FOXAPI_CAP_SCANNER, "SCANNER", true, known(FOXAPI_OP_SCANNER_RUN) && known(FOXAPI_OP_SCANNER_SKIP) && known(FOXAPI_OP_SCANNER_CONFIG)},
        {FOXAPI_CAP_SETTINGS, "SETTINGS", false,
         run(r, s, cmd::makeText(FOXAPI_OP_SETTING_SET, "ui.note=hello").c).status() == FOXAPI_UNSUPPORTED},
        {FOXAPI_CAP_PLUGIN_STORE, "PLUGIN_STORE", true,
         api()->read_list(s.s, FOXAPI_LIST_CATALOGUE, 0, nullptr) >= 0 && known(FOXAPI_OP_STORE_FETCH) && known(FOXAPI_OP_STORE_INSTALL) &&
             known(FOXAPI_OP_STORE_REMOVE)},
        {FOXAPI_CAP_REPORTS, "REPORTS", false,
         !known(FOXAPI_OP_PROBLEM_REPORT) && !known(FOXAPI_OP_FEATURE_REQUEST)},
        {FOXAPI_CAP_TELEMETRY, "TELEMETRY", false,
         run(r, s, cmd::makeInt(FOXAPI_OP_TELEMETRY_ENABLE, 1)).status() == FOXAPI_DENIED},
        {FOXAPI_CAP_EVENTS, "EVENTS", true, api()->subscribe(s.s, ~0ull) == FOXAPI_OK},
        {FOXAPI_CAP_BAND_PLAN, "BAND_PLAN", true,
         api()->read_list(s.s, FOXAPI_LIST_BAND_PLAN, 0, nullptr) >= 0 && known(FOXAPI_OP_SET_BAND_PLAN)},
        {FOXAPI_CAP_TRACKS, "TRACKS", false, api()->read_list(s.s, FOXAPI_LIST_TRACKS, 0, nullptr) == FOXAPI_UNSUPPORTED},
        {FOXAPI_CAP_SCOPE, "SCOPE", true, FOXAPI_HAS(api(), read_scope)},
        {FOXAPI_CAP_POSITION, "POSITION", true, known(FOXAPI_OP_SET_POSITION) && known(FOXAPI_OP_GPS) && api()->read_list(s.s, FOXAPI_LIST_GPS, 0, nullptr) >= 0},
        {FOXAPI_CAP_IMAGES, "IMAGES", true, FOXAPI_HAS(api(), read_image)},
        {FOXAPI_CAP_FACTS, "FACTS", true, fact(s, "fact.patch.version").size() > 0u},
        {FOXAPI_CAP_LONG_TEXT, "LONG_TEXT", true, FOXAPI_HAS(api(), submit_text)},
        {FOXAPI_CAP_NODE_SPECTRUM, "NODE_SPECTRUM", true, FOXAPI_HAS(api(), read_node_spectrum)},
    };
    std::uint64_t listed = 0;
    for (const Row& row : rows) {
        listed |= row.bit;
        const bool declared = (caps & row.bit) != 0u;
        if (declared != row.served || !row.probe) {
            std::printf("K: %s declared=%d should be %d probe=%d\n", row.name, declared ? 1 : 0, row.served ? 1 : 0, row.probe ? 1 : 0);
        }
        CHECK_EQ(declared, row.served);
        CHECK(row.probe);
    }
    // Every bit the header defines is in the table (a new bit with no row fails here), and the retired one stays clear.
    for (int bit = 0; bit <= 28; ++bit) {
        if (bit == 14) {
            CHECK((caps & (1ull << bit)) == 0u);
            continue;
        }
        CHECK((listed & (1ull << bit)) != 0u);
    }
    CHECK_EQ(caps >> 29, 0ull);
}

// ===================================================================================================================================
// P. the internal sessions' closed op profiles
// ===================================================================================================================================

// Every field of a ControlRequest set, and then the pairs that yield to another when both are given.
std::set<std::uint32_t> opsOfEveryRequest() {
    using cascade::net::ControlRequest;
    std::set<std::uint32_t> ops;
    const cascade::net::ControlOpsContext ctx{};
    const auto take = [&](const ControlRequest& q) {
        for (const cmd::QueuedCommand& c : cascade::net::controlRequestToCommands(q, ctx)) { ops.insert(c.c.op); }
    };
    ControlRequest all;
    all.running = true;
    all.centerHz = 100.0e6;
    all.vfoOffsetHz = 1000.0;
    all.mode = cascade::dsp::DemodMode::NFM;
    all.bandwidthHz = 12500.0;
    all.squelchDb = -60.0;
    all.volume = 0.5;
    all.dbMin = -100.0;
    all.dbMax = -20.0;
    all.deemphasisIndex = 1;
    all.nrEnabled = true;
    all.nrStrength = 0.5;
    all.notchEnabled = true;
    all.notchFreqHz = 1000.0;
    all.notchQ = 10.0;
    all.autoNotch = true;
    all.stereoEnabled = true;
    all.sourceKind = "soapy";
    all.soapyArgs = "driver=rtlsdr";
    all.antenna = "RX";
    all.sampleRateHz = 2.4e6;
    all.gainName = "LNA";
    all.gainDb = 10.0;
    all.agc = true;
    all.scanDevices = true;
    all.recordIq = true;
    all.recordAudio = true;
    all.bookmarkAdd = "x";
    all.bookmarkTune = 0;
    all.bookmarkRemove = 0;
    all.scannerActive = true;
    all.scannerSkip = true;
    all.scanStartHz = 88.0e6;
    all.scanStopHz = 108.0e6;
    all.scanStepHz = 100.0e3;
    all.pluginFetch = true;
    all.pluginInstall = "p";
    all.acknowledgeNotice = true;
    all.pluginRemove = "p.dll";
    all.pluginTuneName = "p";
    all.pluginTuneAllowed = true;
    all.pluginPresetName = "p";
    all.pluginPresetIndex = 1;
    all.transmitPtt = true;
    take(all);
    // The members that give way when their partner is there are each sent alone, so their own ops appear.
    { ControlRequest q; q.dbMin = -100.0; take(q); }
    { ControlRequest q; q.dbMax = -20.0; take(q); }
    { ControlRequest q; q.nrStrength = 0.5; take(q); }
    { ControlRequest q; q.notchFreqHz = 1000.0; take(q); }
    { ControlRequest q; q.notchQ = 10.0; take(q); }
    { ControlRequest q; q.sourceKind = "siggen"; take(q); }
    return ops;
}

std::set<std::uint32_t> opsOfEveryCatLine() {
    std::set<std::uint32_t> ops;
    cascade::net::CatRadio radio;
    radio.running = true;
    radio.centerHz = 145.0e6;
    radio.sampleRateHz = 2.4e6;
    radio.bandwidthHz = 12500.0;
    radio.mode = "NFM";
    const cascade::net::ControlOpsContext ctx{};
    const char* lines[] = {"F 145012500", "F 433920000", "F 145000000", "M USB 2400", "M FM 0", "M WFM 150000", "M CW 500", "M AM 6000",
                           "f", "m", "v", "s", "t", "T 1", "T 0", "S 0 VFOA", "V VFOA", "\\dump_state", "\\get_powerstat", "\\chk_vfo",
                           "\\get_freq", "\\set_freq 145000000", "\\set_mode USB 2400", "q"};
    for (const char* line : lines) {
        const cascade::net::CatResult res = cascade::net::executeCatLine(line, radio);
        if (!res.hasControl) { continue; }
        for (const cmd::QueuedCommand& c : cascade::net::controlRequestToCommands(res.control, ctx)) { ops.insert(c.c.op); }
    }
    return ops;
}

void testProfileClosure() {
    std::set<std::uint32_t> web, cat;
    for (const std::uint32_t op : cascade::engine::internalProfileOps(InternalKind::Web)) { CHECK(web.insert(op).second); }  // no duplicates
    for (const std::uint32_t op : cascade::engine::internalProfileOps(InternalKind::Cat)) { CHECK(cat.insert(op).second); }
    const std::set<std::uint32_t> emitted = opsOfEveryRequest();
    std::printf("P1: the web profile has %zu ops; every ControlRequest field emits %zu\n", web.size(), emitted.size());
    for (const std::uint32_t op : emitted) {
        if (web.count(op) == 0) { std::printf("  emitted but not in the profile: %s\n", cmd::opName(op)); }
        CHECK(web.count(op) != 0);
    }
    for (const std::uint32_t op : web) {
        if (emitted.count(op) == 0) { std::printf("  in the profile but nothing emits it: %s\n", cmd::opName(op)); }
        CHECK(emitted.count(op) != 0);
    }
    // The six application ops and the three ADMIN ones among them, named (the survey's census).
    int app = 0, admin = 0;
    for (const std::uint32_t op : web) {
        app += cmd::isAppOp(op) ? 1 : 0;
        admin += (op == FOXAPI_OP_STORE_FETCH || op == FOXAPI_OP_STORE_INSTALL || op == FOXAPI_OP_STORE_REMOVE) ? 1 : 0;
    }
    CHECK_EQ(app, 6);
    CHECK_EQ(admin, 3);
    CHECK_EQ(web.size(), static_cast<std::size_t>(38));
    // CAT: exactly what F and M send.
    const std::set<std::uint32_t> catEmitted = opsOfEveryCatLine();
    CHECK(cat == catEmitted);
    CHECK_EQ(cat.size(), static_cast<std::size_t>(4));
    for (const std::uint32_t op : {FOXAPI_OP_SET_CENTRE, FOXAPI_OP_SET_VFO_OFFSET, FOXAPI_OP_SET_MODE, FOXAPI_OP_SET_BANDWIDTH}) {
        CHECK(cat.count(op) != 0);
    }
    // The grants: the browser's TUNE, SETTINGS, TRANSMIT (the PTT only: the profile has no other transmitter op) and AUDIO; CAT's TUNE and
    // SETTINGS - never TRANSMIT, never ADMIN.
    const std::uint64_t wg = cascade::engine::internalProfileGrants(InternalKind::Web);
    const std::uint64_t cg = cascade::engine::internalProfileGrants(InternalKind::Cat);
    CHECK_EQ(wg, FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS | FOXAPI_GRANT_TRANSMIT | FOXAPI_GRANT_AUDIO);
    CHECK_EQ(cg, FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS);
    CHECK((wg & FOXAPI_GRANT_ADMIN) == 0u && (cg & (FOXAPI_GRANT_ADMIN | FOXAPI_GRANT_TRANSMIT)) == 0u);
    CHECK_EQ(cascade::engine::internalProfileOps(InternalKind::None).size(), static_cast<std::size_t>(0));
}

// Every op there is, as a command whose arguments pass the range checks, so what a refusal says is about the profile and nothing else.
FoxCommand benign(std::uint32_t op) {
    FoxCommand c{};
    c.structSize = sizeof(c);
    c.op = op;
    c.num[0] = 1.0;
    c.ival[0] = 1;
    std::strcpy(c.text, "x=y");
    switch (op) {
        case FOXAPI_OP_SET_CENTRE: case FOXAPI_OP_SET_FREQUENCY: c.num[0] = 100.0e6; break;
        case FOXAPI_OP_SET_BANDWIDTH: c.num[0] = 12500.0; break;
        case FOXAPI_OP_SET_SQUELCH: c.num[0] = -60.0; break;
        case FOXAPI_OP_SET_VOLUME: c.num[0] = 0.5; break;
        case FOXAPI_OP_SET_DISPLAY_RANGE: c.num[0] = -100.0; c.num[1] = -20.0; break;
        case FOXAPI_OP_SET_SAMPLE_RATE: c.num[0] = 2.4e6; break;
        case FOXAPI_OP_SET_MODE: c.ival[0] = FOXAPI_DEMOD_NFM; break;
        case FOXAPI_OP_SET_DEEMPHASIS: c.ival[0] = 1; break;
        case FOXAPI_OP_SET_GAIN: std::strcpy(c.text, "LNA"); c.num[0] = 10.0; break;
        case FOXAPI_OP_SELECT_SOURCE: std::strcpy(c.text, "siggen"); break;
        default: break;
    }
    return c;
}

std::vector<std::uint32_t> everyOp() {
    std::set<std::uint32_t> ops;
    std::size_t n = 0;
    const std::uint32_t* known = cmd::knownOps(n);
    for (std::size_t i = 0; i < n; ++i) { ops.insert(known[i]); }
    // ...and the ops the table refuses or handles apart from the known list.
    for (const std::uint32_t op : {FOXAPI_OP_TX_LATCH, FOXAPI_OP_TX_REMOTE_ARM, FOXAPI_OP_SETTING_SET, FOXAPI_OP_SERVER_CONFIG,
                                   FOXAPI_OP_UPDATE_CHECK, FOXAPI_OP_PROBLEM_REPORT, FOXAPI_OP_FEATURE_REQUEST, FOXAPI_OP_BOOKMARK_IMPORT,
                                   FOXAPI_OP_PATCH_LOAD, FOXAPI_OP_USER_PRESET_FORGET, 0x7FFFu}) {
        ops.insert(op);
    }
    return std::vector<std::uint32_t>(ops.begin(), ops.end());
}

void testProfileAtTheDoor() {
    Rig r("testHooks=1;token=s3cret", /*thread=*/false, /*run=*/false);
    r.pump();
    std::int32_t st = 0;
    Sess web(r.e.api().openInternalSession(InternalKind::Web, "the web server", &st));
    CHECK_EQ(st, FOXAPI_OK);
    Sess cat(r.e.api().openInternalSession(InternalKind::Cat, "a CAT client", &st));
    CHECK_EQ(st, FOXAPI_OK);
    CHECK(web.s != nullptr && cat.s != nullptr);
    std::set<std::uint32_t> webOps(cascade::engine::internalProfileOps(InternalKind::Web).begin(),
                                   cascade::engine::internalProfileOps(InternalKind::Web).end());
    std::set<std::uint32_t> catOps(cascade::engine::internalProfileOps(InternalKind::Cat).begin(),
                                   cascade::engine::internalProfileOps(InternalKind::Cat).end());
    int denied = 0, allowed = 0;
    for (const std::uint32_t op : everyOp()) {
        const FoxCommand c = benign(op);
        for (int who = 0; who < 2; ++who) {
            Sess& s = who == 0 ? web : cat;
            const auto& mine = who == 0 ? webOps : catOps;
            const std::int32_t v = r.e.api().validateForTest(s.s, c);
            const bool known = op == FOXAPI_OP_TX_LATCH || op == FOXAPI_OP_TX_REMOTE_ARM || op == FOXAPI_OP_SETTING_SET || cmd::isKnownOp(op);
            if (!known) {
                CHECK_EQ(v, FOXAPI_UNSUPPORTED);  // not an op this engine has: the newer-interface answer, as for every session
                continue;
            }
            if (mine.count(op) == 0) {
                if (v != FOXAPI_DENIED) { std::printf("P2: %s from %s answered %d, wanted DENIED\n", cmd::opName(op), who == 0 ? "web" : "cat", v); }
                CHECK_EQ(v, FOXAPI_DENIED);
                ++denied;
            } else {
                // On the profile: the profile does not refuse it. (The only DENIED left is the PTT's own consent rule, which has no TX
                // available in this engine to answer first: NO_DEVICE.)
                if (v == FOXAPI_DENIED) { std::printf("P2: %s from %s was DENIED but is on the profile\n", cmd::opName(op), who == 0 ? "web" : "cat"); }
                CHECK(v != FOXAPI_DENIED);
                ++allowed;
            }
        }
    }
    std::printf("P2: %d refused at the door, %d let through, over every op\n", denied, allowed);
    CHECK_EQ(allowed, static_cast<int>(webOps.size() + catOps.size()));
    CHECK(denied > 100);
    // The ADMIN ops are on the web profile and the web session holds no ADMIN grant: the profile is what lets them through.
    CHECK_EQ(r.e.api().validateForTest(web.s, benign(FOXAPI_OP_STORE_FETCH)), FOXAPI_OK);
    CHECK_EQ(r.e.api().validateForTest(cat.s, benign(FOXAPI_OP_STORE_FETCH)), FOXAPI_DENIED);
    CHECK((web.state().grants & FOXAPI_GRANT_ADMIN) == 0u);
    // The transmitter: the web session has the PTT and nothing else of it; CAT has none. (No transmitter is open in this engine.)
    FoxCommand ptt = benign(FOXAPI_OP_TX_PTT);
    ptt.ival[0] = 1;
    CHECK_EQ(r.e.api().validateForTest(web.s, ptt), FOXAPI_NO_DEVICE);  // not the profile: there is no radio to key
    CHECK_EQ(r.e.api().validateForTest(cat.s, ptt), FOXAPI_DENIED);     // the profile: CAT never keys
    for (const std::uint32_t op : {FOXAPI_OP_TX_OPEN, FOXAPI_OP_TX_CLOSE, FOXAPI_OP_TX_SET_MODE, FOXAPI_OP_TX_LATCH, FOXAPI_OP_TX_REMOTE_ARM}) {
        CHECK_EQ(r.e.api().validateForTest(web.s, benign(op)), FOXAPI_DENIED);
    }
}

void testProfileViolationIsAResult() {
    Rig r("testHooks=1;token=s3cret", /*thread=*/false, /*run=*/false);
    r.pump();
    Sess web(r.e.api().openInternalSession(InternalKind::Web, "the web server"));
    Sess cat(r.e.api().openInternalSession(InternalKind::Cat, "a CAT client"));
    Sess desk(r.e, FOXAPI_SESSION_LOCAL, kAllGrants);
    r.pump();
    // Allowed: applied, and visible to everybody.
    FoxCommand vol = benign(FOXAPI_OP_SET_VOLUME);
    vol.num[0] = 0.25;
    const Answer v = run(r, web, vol);
    CHECK(v.landed && v.r.status == FOXAPI_OK);
    CHECK(std::fabs(desk.state().volume - 0.25) < 1e-9);
    // Not on the web profile: answered DENIED, as a RESULT (the submit itself succeeds), with the profile's sentence, REFUSED set,
    // and the engine unchanged.
    const std::uint32_t off[] = {FOXAPI_OP_SET_MUTED, FOXAPI_OP_SERVER_CONFIG, FOXAPI_OP_TX_LATCH, FOXAPI_OP_TX_OPEN, FOXAPI_OP_STORE_CANCEL,
                                 FOXAPI_OP_STORE_UPDATE_ALL, FOXAPI_OP_PATCH_RUN, FOXAPI_OP_BOOKMARK_FAVOURITE, FOXAPP_OP_SET_BANDWIDTH_STEP,
                                 FOXAPP_OP_TELEMETRY_CONSENT, FOXAPP_OP_SET_CATALOGUE_URL, FOXAPI_OP_AUDIO_DEVICE};
    for (const std::uint32_t op : off) {
        const Answer a = run(r, web, benign(op));
        if (a.status() != FOXAPI_DENIED) { std::printf("P3: %s answered %d\n", cmd::opName(op), a.status()); }
        CHECK_EQ(a.submit, FOXAPI_OK);
        CHECK(a.landed);
        CHECK_EQ(a.r.status, FOXAPI_DENIED);
        CHECK((a.r.flags & FOXAPI_RESULT_REFUSED) != 0u);
        CHECK(std::string(a.r.message).find("profile") != std::string::npos);
    }
    CHECK(std::fabs(desk.state().volume - 0.25) < 1e-9);
    // CAT: volume is not on its profile.
    const Answer c = run(r, cat, benign(FOXAPI_OP_SET_VOLUME));
    CHECK(c.landed && c.r.status == FOXAPI_DENIED);
    CHECK(std::string(c.r.message).find("profile") != std::string::npos);
    const Answer t = run(r, cat, benign(FOXAPI_OP_SET_BANDWIDTH));
    CHECK(t.landed && t.r.status == FOXAPI_OK);
    // A malformed command from a profile session is the ordinary refusal, not the profile's.
    FoxCommand nan = benign(FOXAPI_OP_SET_VOLUME);
    nan.num[0] = std::nan("");
    const Answer n = run(r, web, nan);
    CHECK(n.landed && n.r.status == FOXAPI_BAD_ARGUMENT);
}

// P4: a session a CLIENT opened is exactly as it was.
void testPublicRulesUnchanged() {
    Rig r("testHooks=1;token=s3cret", /*thread=*/false, /*run=*/false);
    r.pump();
    Sess remote(r.e, FOXAPI_SESSION_REMOTE, kAllGrants, "s3cret");
    Sess local(r.e, FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(remote.openStatus == FOXAPI_OK && local.openStatus == FOXAPI_OK);
    CHECK((remote.state().grants & FOXAPI_GRANT_ADMIN) == 0u);  // stripped, as ever
    CHECK((local.state().grants & FOXAPI_GRANT_ADMIN) != 0u);
    FoxSession* rs = remote.s;
    for (const std::uint32_t op : {FOXAPI_OP_STORE_FETCH, FOXAPI_OP_STORE_INSTALL, FOXAPI_OP_STORE_REMOVE, FOXAPI_OP_SERVER_CONFIG,
                                   FOXAPP_OP_SET_DISPLAY_MIN, FOXAPP_OP_SET_DISPLAY_MAX, FOXAPP_OP_SET_NR_STRENGTH,
                                   FOXAPP_OP_SET_NOTCH_FREQUENCY, FOXAPP_OP_SET_NOTCH_Q, FOXAPP_OP_SCANNER_RANGE, FOXAPI_OP_TX_LATCH,
                                   FOXAPI_OP_TX_REMOTE_ARM, FOXAPI_OP_TX_OPEN}) {
        const std::int32_t v = r.e.api().validateForTest(rs, benign(op));
        if (v != FOXAPI_DENIED) { std::printf("P4: %s from a REMOTE session answered %d\n", cmd::opName(op), v); }
        CHECK_EQ(v, FOXAPI_DENIED);
    }
    // ...and the same ops are not refused to a LOCAL session by anything this stage added.
    for (const std::uint32_t op : {FOXAPI_OP_STORE_FETCH, FOXAPP_OP_SET_DISPLAY_MIN, FOXAPP_OP_SCANNER_RANGE, FOXAPI_OP_SERVER_CONFIG}) {
        CHECK(r.e.api().validateForTest(local.s, benign(op)) != FOXAPI_DENIED);
    }
    // The messages are the ones they were (a REMOTE session's refusals do not name a profile).
    const Answer a = run(r, remote, benign(FOXAPP_OP_SET_DISPLAY_MIN));
    CHECK(a.landed && a.r.status == FOXAPI_DENIED);
    CHECK(std::string(a.r.message).find("profile") == std::string::npos);
}

void testInternalSessionLimitsAndGrants() {
    Rig r("testHooks=1;token=s3cret;maxRemoteSessions=3;maxSessions=8", /*thread=*/false, /*run=*/false);
    r.pump();
    std::int32_t st = 0;
    Sess web(r.e.api().openInternalSession(InternalKind::Web, "w", &st));
    CHECK_EQ(st, FOXAPI_OK);
    Sess cat1(r.e.api().openInternalSession(InternalKind::Cat, "c1", &st));
    CHECK_EQ(st, FOXAPI_OK);
    // The REMOTE flag, the grants, no token needed, the profile's grants and VIEW.
    const FoxReceiverState ws = web.state();
    const FoxReceiverState cs = cat1.state();
    CHECK_EQ(ws.grants, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS | FOXAPI_GRANT_TRANSMIT | FOXAPI_GRANT_AUDIO);
    CHECK_EQ(cs.grants, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS);
    // The third REMOTE place is taken by a token session; then the fourth, internal or not, is refused.
    Sess tokened(r.e, FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW, "s3cret");
    CHECK_EQ(tokened.openStatus, FOXAPI_OK);
    FoxSession* none = r.e.api().openInternalSession(InternalKind::Cat, "c2", &st);
    CHECK(none == nullptr);
    CHECK_EQ(st, FOXAPI_LIMIT);
    Sess refused(r.e, FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW, "s3cret");
    CHECK_EQ(refused.openStatus, FOXAPI_LIMIT);
    // A LOCAL place is still there (the limit that was hit is the REMOTE one).
    Sess desk(r.e, FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW);
    CHECK_EQ(desk.openStatus, FOXAPI_OK);
    // Closing one gives the place back.
    cat1.close();
    r.pump();
    FoxSession* again = r.e.api().openInternalSession(InternalKind::Cat, "c3", &st);
    CHECK(again != nullptr);
    CHECK_EQ(st, FOXAPI_OK);
    if (again != nullptr) { api()->close_session(again); }
    // Not a kind.
    CHECK(r.e.api().openInternalSession(InternalKind::None, "x", &st) == nullptr);
    CHECK_EQ(st, FOXAPI_BAD_ARGUMENT);
}

// ===================================================================================================================================
// S. SERVER_CONFIG, the SERVERS list and the facts
// ===================================================================================================================================

constexpr const char* kWebFacts[] = {"fact.web.enabled", "fact.web.bindAddress", "fact.web.port", "fact.web.username",
                                     "fact.web.hasPassword", "fact.web.passwordPending", "fact.web.reachable", "fact.web.authRequired",
                                     "fact.web.policy", "fact.web.addresses", "fact.cat.enabled", "fact.cat.bindAll", "fact.cat.port"};

Answer config(Rig& r, Sess& s, const std::string& json) {
    FoxCommand c{};
    c.structSize = sizeof(c);
    c.op = FOXAPI_OP_SERVER_CONFIG;
    return run(r, s, c, &json);
}

void testServerConfig() {
    Rig r("testHooks=1;token=s3cret");
    Sess op(r.e, FOXAPI_SESSION_LOCAL, kAllGrants);
    // S1: defaults, and the list of the two listeners.
    CHECK_EQ(fact(op, "fact.web.enabled"), std::string("0"));
    CHECK_EQ(fact(op, "fact.web.bindAddress"), std::string("127.0.0.1"));
    CHECK_EQ(fact(op, "fact.web.port"), std::string("8073"));
    CHECK_EQ(fact(op, "fact.web.username"), std::string("admin"));
    CHECK_EQ(fact(op, "fact.web.hasPassword"), std::string("0"));
    CHECK_EQ(fact(op, "fact.web.passwordPending"), std::string("0"));
    CHECK_EQ(fact(op, "fact.web.reachable"), std::string("0"));
    CHECK_EQ(fact(op, "fact.web.authRequired"), std::string("0"));
    CHECK_EQ(fact(op, "fact.web.policy"), std::string());
    CHECK_EQ(fact(op, "fact.cat.enabled"), std::string("0"));
    CHECK_EQ(fact(op, "fact.cat.bindAll"), std::string("0"));
    CHECK_EQ(fact(op, "fact.cat.port"), std::string("4532"));
    CHECK_EQ(count(op, FOXAPI_LIST_SERVERS), 2);
    FoxListItem w = item(op, FOXAPI_LIST_SERVERS, 0);
    FoxListItem c = item(op, FOXAPI_LIST_SERVERS, 1);
    CHECK_EQ(std::string(w.name), std::string("web"));
    CHECK_EQ(std::string(c.name), std::string("cat"));
    CHECK_EQ(w.id, 1u);
    CHECK_EQ(c.id, 2u);
    CHECK_EQ(w.value[0], 8073.0);
    CHECK_EQ(c.value[0], 4532.0);
    CHECK_EQ(w.flags & (FOXAPI_ITEM_ACTIVE | FOXAPI_ITEM_FAULT), 0u);
    CHECK_EQ(api()->read_list(op.s, FOXAPI_LIST_SERVERS, 2, nullptr), 2);

    // S2: a configuration applied and read back. 0.0.0.0 with no password is a binding the policy refuses: stored (the file keeps what
    // the operator chose), and the policy's sentence is published beside it.
    const std::uint64_t seq0 = op.state().listSeq;
    Answer a = config(r, op, R"({"web":{"enabled":true,"bindAddress":"0.0.0.0","port":9000,"username":"op"},"cat":{"enabled":true,"bindAll":true,"port":4600}})");
    CHECK(a.landed && a.r.status == FOXAPI_OK);
    CHECK(waitUntil(2000, [&] { return fact(op, "fact.web.port") == "9000"; }));
    CHECK_EQ(fact(op, "fact.web.enabled"), std::string("1"));
    CHECK_EQ(fact(op, "fact.web.bindAddress"), std::string("0.0.0.0"));
    CHECK_EQ(fact(op, "fact.web.username"), std::string("op"));
    CHECK_EQ(fact(op, "fact.cat.enabled"), std::string("1"));
    CHECK_EQ(fact(op, "fact.cat.bindAll"), std::string("1"));
    CHECK_EQ(fact(op, "fact.cat.port"), std::string("4600"));
    CHECK_EQ(fact(op, "fact.web.reachable"), std::string("0"));  // refused: off the machine and no password
    CHECK(fact(op, "fact.web.policy").size() > 10u);
    CHECK_EQ(item(op, FOXAPI_LIST_SERVERS, 0).value[0], 9000.0);
    CHECK_EQ(item(op, FOXAPI_LIST_SERVERS, 1).value[0], 4600.0);
    // The config the engine writes follows (the window's currentConfig reads exactly this).
    CHECK(waitUntil(2000, [&] { return r.e.configSnapshot().catPort == 4600; }));
    {
        const cascade::core::AppConfig cfg = r.e.configSnapshot();
        CHECK(cfg.webEnabled && cfg.webPort == 9000 && cfg.webBindAddress == "0.0.0.0" && cfg.webUsername == "op");
        CHECK(cfg.catEnabled && cfg.catBindAll);
    }
    // A change moves the lists' sequence (the panel re-reads); a listener's report is a change too.
    CHECK(op.state().listSeq != seq0);
    // S6: the fingerprint. One change moves listSeq; the same value said again moves nothing, and answers NO_CHANGE.
    const std::uint64_t seq1 = op.state().listSeq;
    a = config(r, op, R"({"cat":{"port":4600}})");
    CHECK(a.landed && a.r.status == FOXAPI_NO_CHANGE);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK_EQ(op.state().listSeq, seq1);
    // The counters are read live and move nothing; a listener's report (the port it bound, a refusal) does.
    r.e.servers().catCounters().clients.store(3);
    r.e.servers().catCounters().commands.store(77);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK_EQ(op.state().listSeq, seq1);
    CHECK_EQ(item(op, FOXAPI_LIST_SERVERS, 1).value[1], 3.0);
    CHECK_EQ(item(op, FOXAPI_LIST_SERVERS, 1).value[3], 77.0);
    r.e.servers().webCounters().audioListeners.store(2);
    CHECK_EQ(item(op, FOXAPI_LIST_SERVERS, 0).value[2], 2.0);
    cascade::engine::ServerHost::Listener up;
    up.running = true;
    up.boundPort = 9001;
    up.addresses = {"192.168.1.5", "10.0.0.7"};
    r.e.servers().setWeb(up);
    CHECK(waitUntil(2000, [&] { return op.state().listSeq != seq1; }));
    FoxListItem wl = item(op, FOXAPI_LIST_SERVERS, 0);
    CHECK((wl.flags & FOXAPI_ITEM_ACTIVE) != 0u && (wl.flags & FOXAPI_ITEM_FAULT) == 0u);
    CHECK_EQ(wl.value[0], 9001.0);
    CHECK_EQ(fact(op, "fact.web.addresses"), std::string("192.168.1.5\n10.0.0.7"));
    cascade::engine::ServerHost::Listener refusedBind;
    refusedBind.refusal = "the port is already in use";
    r.e.servers().setWeb(refusedBind);
    CHECK(waitUntil(2000, [&] { return (item(op, FOXAPI_LIST_SERVERS, 0).flags & FOXAPI_ITEM_FAULT) != 0u; }));
    wl = item(op, FOXAPI_LIST_SERVERS, 0);
    CHECK_EQ(std::string(wl.detail), std::string("the port is already in use"));
    CHECK_EQ(wl.value[0], 9000.0);  // not listening: the configured port
    r.e.servers().setWeb({});
    r.e.servers().catCounters().clients.store(0);
    r.e.servers().catCounters().commands.store(0);
    r.e.servers().webCounters().audioListeners.store(0);

    // S3: THE PASSWORD. Accepted before its record exists; the hash runs on a worker; the record verifies it; the plaintext is nowhere.
    const std::string secret = "correct horse battery";
    a = config(r, op, std::string(R"({"web":{"password":")") + secret + R"("}})");
    CHECK(a.landed && a.r.status == FOXAPI_OK);
    CHECK(std::string(a.r.message).find("hash") != std::string::npos);
    CHECK(waitUntil(15000, [&] { return fact(op, "fact.web.hasPassword") == "1"; }));
    CHECK_EQ(fact(op, "fact.web.passwordPending"), std::string("0"));
    CHECK_EQ(fact(op, "fact.web.reachable"), std::string("1"));   // 0.0.0.0 with a password: the policy allows it
    CHECK_EQ(fact(op, "fact.web.authRequired"), std::string("1"));
    CHECK_EQ(fact(op, "fact.web.policy"), std::string());
    CHECK(waitUntil(2000, [&] { return !r.e.configSnapshot().webPasswordRecord.empty(); }));
    {
        const cascade::core::AppConfig cfg = r.e.configSnapshot();
        cascade::net::PasswordRecord rec;
        std::string why;
        CHECK(cascade::net::PasswordRecord::parse(cfg.webPasswordRecord, rec, why));
        CHECK(cascade::net::verifyPassword(secret, rec, why));
        CHECK(!cascade::net::verifyPassword("not the password", rec, why));
        CHECK(cfg.webPasswordRecord.find(secret) == std::string::npos);
        // The plaintext is in no text the engine publishes: not a fact, not a list item, not the record.
        for (const char* key : kWebFacts) { CHECK(fact(op, key).find(secret) == std::string::npos); }
        for (uint32_t i = 0; i < 2; ++i) {
            const FoxListItem it = item(op, FOXAPI_LIST_SERVERS, i);
            CHECK(std::string(it.name).find(secret) == std::string::npos && std::string(it.detail).find(secret) == std::string::npos);
        }
        // ...and the record is not published either.
        for (const char* key : kWebFacts) { CHECK(fact(op, key).find("pbkdf2") == std::string::npos); }
        // A record handed in whole is stored as it is (a restore); a damaged one is refused.
        const std::string good = cfg.webPasswordRecord;
        a = config(r, op, R"({"web":{"clearPassword":true}})");
        CHECK(a.landed && a.r.status == FOXAPI_OK);
        CHECK(waitUntil(2000, [&] { return fact(op, "fact.web.hasPassword") == "0"; }));
        a = config(r, op, std::string(R"({"web":{"passwordRecord":")") + good + R"("}})");
        CHECK(a.landed && a.r.status == FOXAPI_OK);
        CHECK(waitUntil(2000, [&] { return fact(op, "fact.web.hasPassword") == "1"; }));
        a = config(r, op, R"({"web":{"passwordRecord":"pbkdf2-sha256$1$AAAA$AAAA"}})");
        CHECK(a.landed && a.r.status == FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(fact(op, "fact.web.hasPassword"), std::string("1"));
    }
    // Two passwords in a row: the newest wins (one hash at a time; the older answer is dropped).
    a = config(r, op, R"({"web":{"password":"first password one"}})");
    CHECK(a.landed && a.r.status == FOXAPI_OK);
    a = config(r, op, R"({"web":{"password":"second password two"}})");
    CHECK(a.landed && a.r.status == FOXAPI_OK);
    CHECK(waitUntil(20000, [&] { return fact(op, "fact.web.passwordPending") == "0"; }));
    CHECK(waitUntil(2000, [&] {
        cascade::net::PasswordRecord rec;
        std::string why;
        return cascade::net::PasswordRecord::parse(r.e.configSnapshot().webPasswordRecord, rec, why) &&
               cascade::net::verifyPassword("second password two", rec, why);
    }));
    {
        cascade::net::PasswordRecord rec;
        std::string why;
        CHECK(cascade::net::PasswordRecord::parse(r.e.configSnapshot().webPasswordRecord, rec, why));
        CHECK(!cascade::net::verifyPassword("first password one", rec, why));
    }

    // S4: every refusal leaves the configuration exactly as it was.
    const auto snapshot = [&] {
        std::string s;
        for (const char* key : kWebFacts) { s += fact(op, key) + "|"; }
        return s;
    };
    const std::string before = snapshot();
    const std::uint64_t seqBefore = op.state().listSeq;
    const char* bad[] = {
        "",                                                   // not JSON
        "[1]",                                                // not an object
        "{}",                                                 // says nothing
        R"({"web":{}})",                                      // says nothing either
        R"({"wep":{"enabled":true}})",                        // unknown top-level key
        R"({"web":{"enabeld":true}})",                        // unknown key
        R"({"cat":{"enabled":1}})",                           // wrong type
        R"({"web":{"port":"80"}})",                           // wrong type
        R"({"web":{"port":80}})",                             // below 1024
        R"({"web":{"port":70000}})",                          // above 65535
        R"({"cat":{"port":1.5}})",                            // not a whole number
        R"({"web":{"bindAddress":"example.com"}})",           // a hostname
        R"({"web":{"bindAddress":"256.1.1.1"}})",             // not an address
        R"({"web":{"password":"short"}})",                    // under eight characters
        R"({"web":{"password":"long enough","clearPassword":true}})",  // two forms
        R"({"web":{"clearPassword":false}})",
        R"({"web":{"username":5}})",
        R"({"web":"on"})",
    };
    for (const char* text : bad) {
        const Answer e = config(r, op, text);
        if (!(e.landed && e.r.status == FOXAPI_BAD_ARGUMENT && std::strlen(e.r.message) > 0u)) {
            std::printf("S4: %s answered %d '%s'\n", text, e.status(), e.landed ? e.r.message : "?");
        }
        CHECK(e.landed);
        CHECK_EQ(e.r.status, FOXAPI_BAD_ARGUMENT);
        CHECK((e.r.flags & FOXAPI_RESULT_REFUSED) != 0u);
        CHECK(std::strlen(e.r.message) > 0u);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_EQ(snapshot(), before);
    CHECK_EQ(op.state().listSeq, seqBefore);
    // An empty bindAddress resets to the default (a lost value must never widen the binding), as an empty account name does.
    a = config(r, op, R"({"web":{"bindAddress":"","username":""}})");
    CHECK(a.landed && a.r.status == FOXAPI_OK);
    CHECK(waitUntil(2000, [&] { return fact(op, "fact.web.bindAddress") == "127.0.0.1"; }));
    CHECK_EQ(fact(op, "fact.web.username"), std::string("admin"));

    // S5: who may ask, who may read.
    Sess remote(r.e, FOXAPI_SESSION_REMOTE, kAllGrants, "s3cret");
    CHECK_EQ(remote.openStatus, FOXAPI_OK);
    Answer d = config(r, remote, R"({"cat":{"port":4700}})");
    CHECK(d.landed && d.r.status == FOXAPI_DENIED);
    CHECK(std::string(d.r.message).find("local operator") != std::string::npos);
    Sess noAdmin(r.e, FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_SETTINGS | FOXAPI_GRANT_TUNE);
    d = config(r, noAdmin, R"({"cat":{"port":4700}})");
    CHECK(d.landed && d.r.status == FOXAPI_DENIED);
    Sess web(r.e.api().openInternalSession(InternalKind::Web, "the web server"));
    d = config(r, web, R"({"cat":{"port":4700}})");
    CHECK(d.landed && d.r.status == FOXAPI_DENIED);
    Sess cat(r.e.api().openInternalSession(InternalKind::Cat, "a CAT client"));
    d = config(r, cat, R"({"cat":{"port":4700}})");
    CHECK(d.landed && d.r.status == FOXAPI_DENIED);
    CHECK_EQ(fact(op, "fact.cat.port"), std::string("4600"));
    // Reading: the list and every fact are the local operator's.
    CHECK_EQ(count(remote, FOXAPI_LIST_SERVERS), FOXAPI_DENIED);
    CHECK_EQ(count(web, FOXAPI_LIST_SERVERS), FOXAPI_DENIED);
    for (const char* key : kWebFacts) {
        int32_t st = 0;
        (void)fact(remote, key, &st);
        if (st != FOXAPI_DENIED) { std::printf("S5: %s read by a REMOTE session answered %d\n", key, st); }
        CHECK_EQ(st, FOXAPI_DENIED);
    }
    CHECK_EQ(count(noAdmin, FOXAPI_LIST_SERVERS), 2);  // a LOCAL session reads them, ADMIN or not
}

// S7: a configuration file restores into the engine, and the first publish carries it.
void testServerConfigRestore() {
    Engine e;
    e.configureApi("testHooks=1");
    e.initialise();
    cascade::core::AppConfig cfg;
    cfg.webEnabled = true;
    cfg.webPort = 9123;
    cfg.webBindAddress = "127.0.0.1";
    cfg.webUsername = "rx";
    cfg.catEnabled = true;
    cfg.catBindAll = false;
    cfg.catPort = 4777;
    e.applyConfig(cfg);
    e.pump();
    e.pump();
    Sess op(e, FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK_EQ(fact(op, "fact.web.enabled"), std::string("1"));
    CHECK_EQ(fact(op, "fact.web.port"), std::string("9123"));
    CHECK_EQ(fact(op, "fact.web.username"), std::string("rx"));
    CHECK_EQ(fact(op, "fact.cat.port"), std::string("4777"));
    CHECK_EQ(item(op, FOXAPI_LIST_SERVERS, 0).value[0], 9123.0);
    const cascade::core::AppConfig back = e.configSnapshot();
    CHECK(back.webEnabled && back.webPort == 9123 && back.webUsername == "rx" && back.catEnabled && back.catPort == 4777);
    // Pure: ServerHost's own load/store round trip, and the request parser's accepted shapes.
    cascade::engine::ServerHost host;
    host.load(cfg);
    cascade::core::AppConfig out;
    host.store(out);
    CHECK(out.webPort == 9123 && out.catPort == 4777 && out.webUsername == "rx" && out.webEnabled && out.catEnabled);
    cascade::engine::ServerConfigRequest req;
    std::string why;
    CHECK(cascade::engine::parseServerConfigRequest(R"({"web":{"port":1024},"cat":{"port":65535,"enabled":false}})", req, why));
    CHECK(req.webPort.value_or(0) == 1024 && req.catPort.value_or(0) == 65535 && req.catEnabled.has_value() && !*req.catEnabled);
    CHECK(!cascade::engine::parseServerConfigRequest(R"({"cat":{"port":1023}})", req, why));
}

// C: CAT's six figures, from what a CAT connection's own session reads, are the figures the window's snapshot read gave.
void testCatRadioFromTheSession() {
    Rig r("testHooks=1", /*thread=*/false);
    r.pump(3);
    Sess cat(r.e.api().openInternalSession(InternalKind::Cat, "a CAT client"));
    cascade::core::PublishedState ps;
    CHECK(r.e.snapshot()->read(ps));
    const cascade::net::CatRadio fromState = cascade::net::catRadioFromState(ps);
    const cascade::net::CatRadio fromSession = cascade::net::catRadioFromRx(cat.state());
    CHECK(fromState.running == fromSession.running);
    CHECK(fromState.centerHz == fromSession.centerHz && fromState.centerHz > 0.0);
    CHECK(fromState.sampleRateHz == fromSession.sampleRateHz && fromState.sampleRateHz > 0.0);
    CHECK(fromState.vfoOffsetHz == fromSession.vfoOffsetHz);
    CHECK(fromState.bandwidthHz == fromSession.bandwidthHz && fromState.bandwidthHz > 0.0);
    CHECK(fromState.mode == fromSession.mode && !fromSession.mode.empty());
    // ...and a change shows in both (the engine keeps running: the session is a reader of the same publish).
    const Answer a = run(r, cat, benign(FOXAPI_OP_SET_VFO_OFFSET));
    CHECK(a.landed && a.r.status == FOXAPI_OK);
    CHECK(cascade::net::catRadioFromRx(cat.state()).vfoOffsetHz == 1.0);
    // The protocol layer, on that radio: the frequency it answers is centre + offset, and an F inside the band moves the offset only.
    const cascade::net::CatRadio radio = cascade::net::catRadioFromRx(cat.state());
    const cascade::net::CatResult f = cascade::net::executeCatLine("f", radio);
    CHECK(!f.reply.empty() && !f.hasControl);
}

}  // namespace

int main() {
    engine_api_test::hermetic("api05a");
    std::printf("test_engine_api_05a\n");
    // A. read_audio
    testReadAudioEdges();
    testReadAudioIsTheTap();
    testReadAudioOverrun();
    testReadAudioDemand();
    // K. the capability word
    testCapabilityWord();
    // P. the profiles
    testProfileClosure();
    testProfileAtTheDoor();
    testProfileViolationIsAResult();
    testPublicRulesUnchanged();
    testInternalSessionLimitsAndGrants();
    testCatRadioFromTheSession();
    // S. SERVER_CONFIG
    testServerConfig();
    testServerConfigRestore();
    return check::finish("test_engine_api_05a");
}
