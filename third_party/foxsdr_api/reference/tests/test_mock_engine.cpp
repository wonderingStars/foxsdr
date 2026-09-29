// test_mock_engine.cpp - the mock engine's command and snapshot behaviour,
// driven only through the C table (the conformance checklist the real engine
// will be held to).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "check.hpp"
#include "engine_client.hpp"
#include "foxsdr_api.h"
#include "mock_engine/mock_engine.hpp"

using testing::cmd;
using testing::kAllGrants;

namespace {

const FoxEngineApi* api() { return foxsdr::mock::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR); }

void testVersionGate() {
    // During 0.x only the SAME minor is served: 0.2 changed what submit
    // answers, so a 0.1 interface must not be handed a 0.2 engine.
    CHECK(foxsdr::mock::engineApi(0, 2) != nullptr);
    CHECK(foxsdr::mock::engineApi(0, 1) == nullptr);
    CHECK(foxsdr::mock::engineApi(0, 0) == nullptr);
    CHECK(foxsdr::mock::engineApi(0, 3) == nullptr);
    CHECK(foxsdr::mock::engineApi(1, 0) == nullptr);   // a different major always is
    const FoxEngineApi* a = api();
    CHECK_EQ(a->structSize, static_cast<uint32_t>(sizeof(FoxEngineApi)));
    CHECK(a->structSize >= FOXAPI_MIN_ENGINE_API);
    CHECK(FOXAPI_HAS(a, login));
    CHECK(FOXAPI_HAS(a, logout));
    CHECK((a->capabilities & FOXAPI_CAP_RECEIVER) != 0u);
    CHECK((a->capabilities & FOXAPI_CAP_TRANSMIT) != 0u);
    CHECK((a->capabilities & FOXAPI_CAP_DECODERS) == 0u);  // the mock has none, and says so
}

void testSessions() {
    testing::Engine e(api(), "token=s3cret");
    CHECK_EQ(e.createStatus, FOXAPI_OK);
    {
        testing::Session local(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
        CHECK(local.ok());
        CHECK_EQ(local.state().grants, kAllGrants);
    }
    {
        testing::Session noToken(api(), e.raw(), FOXAPI_SESSION_REMOTE, kAllGrants);
        CHECK_EQ(noToken.openStatus, FOXAPI_UNAUTHENTICATED);
        testing::Session badToken(api(), e.raw(), FOXAPI_SESSION_REMOTE, kAllGrants, "guess");
        CHECK_EQ(badToken.openStatus, FOXAPI_UNAUTHENTICATED);
    }
    {
        testing::Session remote(api(), e.raw(), FOXAPI_SESSION_REMOTE, kAllGrants, "s3cret");
        CHECK(remote.ok());
        // Administration is a local act: a remote session never holds ADMIN.
        CHECK_EQ(remote.state().grants, kAllGrants & ~FOXAPI_GRANT_ADMIN);
    }
    {
        testing::Session both(api(), e.raw(), FOXAPI_SESSION_REMOTE | FOXAPI_SESSION_LOCAL,
                              kAllGrants, "s3cret");
        CHECK_EQ(both.openStatus, FOXAPI_BAD_ARGUMENT);
    }
    {
        // An engine with no token configured refuses every remote session.
        testing::Engine e2(api());
        testing::Session remote(api(), e2.raw(), FOXAPI_SESSION_REMOTE, kAllGrants, "");
        CHECK_EQ(remote.openStatus, FOXAPI_UNAUTHENTICATED);
    }
}

void testDefaultState() {
    testing::Engine e(api());
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    const FoxReceiverState st = s.state();
    CHECK_EQ(st.structSize, static_cast<uint32_t>(sizeof(FoxReceiverState)));
    CHECK((st.flags & FOXAPI_RX_RUNNING) != 0u);
    CHECK_EQ(st.demodMode, FOXAPI_DEMOD_WFM);
    CHECK_EQ(st.tunedHz, st.centreHz + st.vfoOffsetHz);
    CHECK_EQ(std::string(st.deviceName), std::string("Signal generator"));
    CHECK(st.seq > 0 && st.tuneSeq > 0 && st.modeSeq > 0);
}

void testSpectrum() {
    testing::Engine e(api(), "bins=1024;fps=60");
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    const FoxEngineApi* a = api();
    std::vector<float> bins(1024);
    FoxSpectrumInfo info{};
    info.structSize = sizeof(info);
    uint64_t firstSeq = 0;
    CHECK(check::waitFor([&] {
        return a->read_spectrum(s.raw(), 0, &info, bins.data(), 1024) == FOXAPI_OK;
    }, 2000));
    firstSeq = info.seq;
    CHECK_EQ(info.binCount, 1024u);
    CHECK_EQ(info.copied, 1024u);
    CHECK(info.spanHz > 0.0);
    // Frames keep coming while running.
    CHECK(check::waitFor([&] {
        FoxSpectrumInfo i2{};
        i2.structSize = sizeof(i2);
        return a->read_spectrum(s.raw(), firstSeq, &i2, bins.data(), 1024) == FOXAPI_OK &&
               i2.seq > firstSeq;
    }, 2000));

    // Stop, so the latest frame stays put, then compare a reduced read of it
    // against the full read: every reduced bin is the MAXIMUM of its group.
    CHECK_EQ(s.run(cmd(FOXAPI_OP_RUN, 0, 0)).result.status, FOXAPI_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    FoxSpectrumInfo full{};
    full.structSize = sizeof(full);
    CHECK_EQ(a->read_spectrum(s.raw(), 0, &full, bins.data(), 1024), FOXAPI_OK);
    // Nothing newer than the latest: NO_CHANGE, and nothing written.
    FoxSpectrumInfo none{};
    none.structSize = sizeof(none);
    CHECK_EQ(a->read_spectrum(s.raw(), full.seq, &none, bins.data(), 1024), FOXAPI_NO_CHANGE);
    CHECK_EQ(none.seq, 0ull);
    std::vector<float> small(100);
    FoxSpectrumInfo red{};
    red.structSize = sizeof(red);
    CHECK_EQ(a->read_spectrum(s.raw(), 0, &red, small.data(), 100), FOXAPI_OK);
    CHECK_EQ(red.seq, full.seq);
    CHECK_EQ(red.copied, 100u);
    int wrong = 0;
    for (uint32_t i = 0; i < 100; ++i) {
        const uint32_t lo = i * 1024u / 100u;
        const uint32_t hi = std::max((i + 1u) * 1024u / 100u, lo + 1u);
        const float m = *std::max_element(bins.begin() + lo, bins.begin() + hi);
        if (small[i] != m) {
            ++wrong;
        }
    }
    CHECK_EQ(wrong, 0);
    // A short info struct is refused, not overrun.
    FoxSpectrumInfo tiny{};
    tiny.structSize = 8;
    CHECK_EQ(a->read_spectrum(s.raw(), 0, &tiny, bins.data(), 1024), FOXAPI_BAD_ARGUMENT);
}

void testTuningAndClamps() {
    testing::Engine e(api(), "centreHz=94.5e6");
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);

    const FoxReceiverState before = s.state();
    auto o = s.run(cmd(FOXAPI_OP_SET_FREQUENCY, 95.3e6));
    CHECK_EQ(o.submitStatus, FOXAPI_OK);
    CHECK(o.landed);
    CHECK_EQ(o.result.status, FOXAPI_OK);
    CHECK_EQ(o.result.applied[0], 95.3e6);
    FoxReceiverState st = s.state();
    CHECK_EQ(st.tunedHz, 95.3e6);
    CHECK_EQ(st.vfoOffsetHz, before.vfoOffsetHz);  // the VFO is kept; the centre moves
    CHECK(st.tuneSeq > before.tuneSeq);

    // Tune up three steps of 10 kHz, then down one.
    CHECK_EQ(s.run(cmd(FOXAPI_OP_STEP_TUNE, 10000.0, 3)).result.status, FOXAPI_OK);
    CHECK_EQ(s.run(cmd(FOXAPI_OP_STEP_TUNE, 10000.0, -1)).result.status, FOXAPI_OK);
    CHECK_EQ(s.state().tunedHz, 95.32e6);

    // A mode key moves the bandwidth to the mode's default.
    o = s.run(cmd(FOXAPI_OP_SET_MODE, 0.0, FOXAPI_DEMOD_AM));
    CHECK_EQ(o.result.status, FOXAPI_OK);
    st = s.state();
    CHECK_EQ(st.demodMode, FOXAPI_DEMOD_AM);
    CHECK_EQ(st.bandwidthHz, 10000.0);

    // An over-wide bandwidth is clamped to the channel, and says so.
    o = s.run(cmd(FOXAPI_OP_SET_BANDWIDTH, 5.0e6));
    CHECK_EQ(o.result.status, FOXAPI_OK);
    CHECK((o.result.flags & FOXAPI_RESULT_CLAMPED) != 0u);
    CHECK_EQ(o.result.applied[0], 0.9 * s.state().channelRateHz);

    // A VFO offset outside the band is pulled back in.
    o = s.run(cmd(FOXAPI_OP_SET_VFO_OFFSET, 5.0e6));
    CHECK((o.result.flags & FOXAPI_RESULT_CLAMPED) != 0u);
    st = s.state();
    CHECK(std::fabs(st.vfoOffsetHz) <= 0.5 * st.sampleRateHz);

    // An inverted display range keeps the minimum span.
    o = s.run([] {
        FoxCommand c = cmd(FOXAPI_OP_SET_DISPLAY_RANGE, -20.0);
        c.num[1] = -25.0;
        return c;
    }());
    CHECK((o.result.flags & FOXAPI_RESULT_CLAMPED) != 0u);
    st = s.state();
    CHECK_EQ(st.dbMax - st.dbMin, 10.0);
}

void testRefusals() {
    testing::Engine e(api());
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    // Refused: the answer is a RESULT (rule 2), never applied.
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SET_VOLUME, 1.5)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SET_FREQUENCY, std::nan(""))).status(), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SET_MODE, 0.0, 99)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(s.run(cmd(0x7777u)).status(), FOXAPI_UNSUPPORTED);          // a future op
    CHECK_EQ(s.run(cmd(FOXAPI_OP_DECODER_START, 0, 0, "POCSAG")).status(),
             FOXAPI_UNSUPPORTED);                                            // not in the mock
    FoxCommand unterminated = cmd(FOXAPI_OP_BOOKMARK_ADD);
    std::memset(unterminated.text, 'x', sizeof(unterminated.text));
    CHECK_EQ(s.run(unterminated).status(), FOXAPI_BAD_ARGUMENT);

    // Grants: a view-only session can read but not tune or set.
    testing::Session viewer(api(), e.raw(), FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW);
    CHECK(viewer.ok());
    CHECK_EQ(viewer.run(cmd(FOXAPI_OP_SET_FREQUENCY, 95e6)).status(), FOXAPI_DENIED);
    CHECK_EQ(viewer.run(cmd(FOXAPI_OP_SET_VOLUME, 0.2)).status(), FOXAPI_DENIED);
    CHECK_EQ(viewer.run(cmd(FOXAPI_OP_SETTING_SET, 0, 0, "a=b")).status(), FOXAPI_DENIED);
    CHECK((viewer.state().flags & FOXAPI_RX_RUNNING) != 0u);

    // A tuner-only session (a Doppler follower) may tune and nothing else.
    testing::Session tuner(api(), e.raw(), FOXAPI_SESSION_LOCAL,
                           FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE);
    CHECK_EQ(tuner.run(cmd(FOXAPI_OP_SET_FREQUENCY, 145.5e6)).result.status, FOXAPI_OK);
    CHECK_EQ(tuner.run(cmd(FOXAPI_OP_SET_MODE, 0, FOXAPI_DEMOD_NFM)).status(), FOXAPI_DENIED);
}

void testBatchAndQueueBound() {
    testing::Engine e(api());
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    const FoxEngineApi* a = api();
    // One batch, three commands: all three TAKEN (tickets 1, 2, 3 - per
    // session, from 1), and three results in ticket order, the third a
    // refusal that changed nothing.
    FoxCommand batch[3] = {cmd(FOXAPI_OP_SET_VOLUME, 0.25), cmd(FOXAPI_OP_SET_SQUELCH, -70.0),
                           cmd(FOXAPI_OP_SET_VOLUME, 2.0)};
    FoxSubmitResult r[3];
    for (auto& x : r) x.structSize = sizeof(x);
    CHECK_EQ(a->submit(s.raw(), batch, 3, r), 3);
    CHECK_EQ(r[0].status, FOXAPI_OK);
    CHECK_EQ(r[1].status, FOXAPI_OK);
    CHECK_EQ(r[2].status, FOXAPI_OK);
    CHECK_EQ(r[0].ticket, 1ull);
    CHECK_EQ(r[1].ticket, 2ull);
    CHECK_EQ(r[2].ticket, 3ull);
    std::vector<FoxCommandResult> got;
    CHECK(check::waitFor([&] {
        FoxCommandResult out[4];
        for (auto& x : out) x.structSize = sizeof(x);
        const int32_t n = a->poll_results(s.raw(), out, 4);
        got.insert(got.end(), out, out + std::max(n, 0));
        return got.size() >= 3;
    }, 2000));
    CHECK_EQ(got.size(), static_cast<std::size_t>(3));
    const auto at = [&](std::size_t i) { return i < got.size() ? got[i] : FoxCommandResult{}; };
    CHECK_EQ(at(0).ticket, 1ull);
    CHECK_EQ(at(1).ticket, 2ull);
    CHECK_EQ(at(2).ticket, 3ull);
    CHECK_EQ(at(0).status, FOXAPI_OK);
    CHECK_EQ(at(2).status, FOXAPI_OUT_OF_RANGE);
    CHECK((at(2).flags & FOXAPI_RESULT_REFUSED) != 0u);
    CHECK((at(0).flags & FOXAPI_RESULT_REFUSED) == 0u);
    CHECK(at(2).message[0] != '\0');
    const FoxReceiverState st = s.state();
    CHECK_EQ(st.volume, 0.25);  // the refused 2.0 changed nothing
    CHECK_EQ(st.squelchDb, -70.0);

    // The queue is bounded per session: past 256 pending, BUSY - never growth -
    // and a BUSY command has no ticket and will have no result.
    std::vector<FoxCommand> many(300, cmd(FOXAPI_OP_SET_VOLUME, 0.3));
    std::vector<FoxSubmitResult> mr(300);
    for (auto& x : mr) x.structSize = sizeof(x);
    const int32_t taken = a->submit(s.raw(), many.data(), 300, mr.data());
    CHECK_EQ(taken, 256);
    CHECK_EQ(mr[255].status, FOXAPI_OK);
    CHECK_EQ(mr[255].ticket, 3ull + 256ull);
    CHECK_EQ(mr[256].status, FOXAPI_BUSY);
    CHECK_EQ(mr[256].ticket, 0ull);
    CHECK_EQ(mr[299].status, FOXAPI_BUSY);

    // A batch larger than FOXAPI_MAX_BATCH is refused WHOLE, before anything
    // is read or sized by the caller's count. A count of 4 billion (with a
    // one-command array: the engine must not read past it, and must not try
    // to reserve room for 4 billion) comes FIRST, so that an engine without
    // the bound fails here by allocation - an exception its catch-all must
    // turn into FOXAPI_FAILED rather than let cross the C boundary - and not
    // by reading past the array.
    FoxCommand one = cmd(FOXAPI_OP_SET_VOLUME, 0.3);
    CHECK_EQ(a->submit(s.raw(), &one, 0xFFFFFFFFu, nullptr), FOXAPI_LIMIT);
    std::vector<FoxCommand> tooMany(FOXAPI_MAX_BATCH + 1u, cmd(FOXAPI_OP_SET_VOLUME, 0.3));
    CHECK_EQ(a->submit(s.raw(), tooMany.data(), static_cast<uint32_t>(tooMany.size()), nullptr),
             FOXAPI_LIMIT);
}

// RULE 2 IN 0.2: SUBMIT NEVER WAITS FOR A JUDGEMENT. A session with no grant
// for what it asks is still answered FOXAPI_OK by submit (the transport has
// it); the refusal arrives as a result, in order, and nothing changes. That
// is what lets a remote interface submit its frame's batch and draw on.
// FOXAPI_RESULT_REFUSED IS SET EXACTLY WHEN THE STATUS IS NOT OK (round-2
// review: refusals decided when APPLIED came back without it). Apply-time
// NOT_FOUND and NO_DEVICE, a submit-time refusal, and a plain OK.
void testRefusedFlagMeansNothingChanged() {
    testing::Engine e(api());
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    const double vol = s.state().volume;
    const auto notFound = s.run(cmd(FOXAPI_OP_BOOKMARK_TUNE, 0, 999));      // decided when applied
    const auto noDevice = s.run(cmd(FOXAPI_OP_SET_SAMPLE_RATE, 2.4e6));      // the generator's rate is fixed
    const auto atSubmit = s.run(cmd(FOXAPI_OP_SET_VOLUME, 7.0));             // out of range at submit
    const auto ok = s.run(cmd(FOXAPI_OP_SET_VOLUME, 0.33));
    CHECK_EQ(notFound.status(), FOXAPI_NOT_FOUND);
    CHECK_EQ(noDevice.status(), FOXAPI_NO_DEVICE);
    CHECK_EQ(atSubmit.status(), FOXAPI_OUT_OF_RANGE);
    CHECK((notFound.result.flags & FOXAPI_RESULT_REFUSED) != 0u);
    CHECK((noDevice.result.flags & FOXAPI_RESULT_REFUSED) != 0u);
    CHECK((atSubmit.result.flags & FOXAPI_RESULT_REFUSED) != 0u);
    CHECK_EQ(ok.status(), FOXAPI_OK);
    CHECK((ok.result.flags & FOXAPI_RESULT_REFUSED) == 0u);
    CHECK(vol != 7.0);
}

void testRefusalsAreResults() {
    testing::Engine e(api());
    testing::Session viewer(api(), e.raw(), FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW);
    const FoxEngineApi* a = api();
    const double before = viewer.state().tunedHz;
    FoxCommand c[3] = {cmd(FOXAPI_OP_SET_FREQUENCY, 101.0e6), cmd(FOXAPI_OP_SET_VOLUME, 0.9),
                       cmd(0x7777u)};
    FoxSubmitResult r[3];
    for (auto& x : r) x.structSize = sizeof(x);
    CHECK_EQ(a->submit(viewer.raw(), c, 3, r), 3);
    for (const auto& x : r) {
        CHECK_EQ(x.status, FOXAPI_OK);
        CHECK(x.ticket != 0ull);
    }
    std::vector<FoxCommandResult> got;
    CHECK(check::waitFor([&] {
        FoxCommandResult out[4];
        for (auto& x : out) x.structSize = sizeof(x);
        const int32_t n = a->poll_results(viewer.raw(), out, 4);
        got.insert(got.end(), out, out + std::max(n, 0));
        return got.size() >= 3;
    }, 2000));
    CHECK_EQ(got.size(), static_cast<std::size_t>(3));
    int refusedInOrder = 0;
    for (std::size_t i = 0; i < got.size() && i < 3; ++i) {
        refusedInOrder += (got[i].ticket == r[i].ticket &&
                           (got[i].flags & FOXAPI_RESULT_REFUSED) != 0u) ? 1 : 0;
    }
    CHECK_EQ(refusedInOrder, 3);
    if (got.size() == 3) {
        CHECK_EQ(got[0].status, FOXAPI_DENIED);
        CHECK_EQ(got[1].status, FOXAPI_DENIED);
        CHECK_EQ(got[2].status, FOXAPI_UNSUPPORTED);
        CHECK_EQ(got[0].op, FOXAPI_OP_SET_FREQUENCY);
    }
    CHECK_EQ(viewer.state().tunedHz, before);
}

// Sessions are bounded, and the login call gives a remote session its token.
// A RESULT IS NEVER LOST. Round-2 review: pending was decremented when a
// result was DELIVERED, so an interface that did not poll could keep
// submitting while results piled up and the oldest were dropped past 512
// with no mark. Now a command counts against FOXAPI_MAX_PENDING until its
// result has been POLLED, so unpolled results can never exceed the pending
// bound and none is ever dropped.
void testPendingCountsUntilPolled() {
    testing::Engine e(api());
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    const FoxEngineApi* a = api();
    std::vector<FoxCommand> batch(FOXAPI_MAX_PENDING, cmd(FOXAPI_OP_SET_VOLUME, 0.4));
    CHECK_EQ(a->submit(s.raw(), batch.data(), static_cast<uint32_t>(batch.size()), nullptr),
             static_cast<int32_t>(FOXAPI_MAX_PENDING));
    // Wait until every one has been APPLIED (its result is waiting)...
    CHECK(check::waitFor([&] { return s.state().volume == 0.4; }, 2000));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // ...and still nothing more is taken: the results have not been polled.
    FoxCommand one = cmd(FOXAPI_OP_SET_VOLUME, 0.5);
    FoxSubmitResult r{};
    r.structSize = sizeof(r);
    CHECK_EQ(a->submit(s.raw(), &one, 1, &r), 0);
    CHECK_EQ(r.status, FOXAPI_BUSY);
    // Polling frees exactly as many places as it reads.
    FoxCommandResult out[10];
    for (auto& x : out) x.structSize = sizeof(x);
    CHECK_EQ(a->poll_results(s.raw(), out, 10), 10);
    std::vector<FoxCommand> ten(10, cmd(FOXAPI_OP_SET_VOLUME, 0.5));
    std::vector<FoxSubmitResult> tr(11);
    for (auto& x : tr) x.structSize = sizeof(x);
    ten.push_back(cmd(FOXAPI_OP_SET_VOLUME, 0.5));
    CHECK_EQ(a->submit(s.raw(), ten.data(), 11, tr.data()), 10);
    CHECK_EQ(tr[10].status, FOXAPI_BUSY);
    // Every ticket issued comes back, none dropped: 256 + 10 results.
    uint64_t got = 10;
    CHECK(check::waitFor([&] {
        FoxCommandResult more[64];
        for (auto& x : more) x.structSize = sizeof(x);
        const int32_t n = a->poll_results(s.raw(), more, 64);
        got += static_cast<uint64_t>(std::max(n, 0));
        return got >= FOXAPI_MAX_PENDING + 10u;
    }, 3000));
    CHECK_EQ(got, static_cast<uint64_t>(FOXAPI_MAX_PENDING + 10u));
}

// No more than 16 REMOTE sessions (docs/TRANSPORTS.md 2.5), whatever the
// total allows; local sessions are not held back by them.
void testRemoteSessionLimit() {
    testing::Engine e(api(), "token=t");
    std::vector<std::unique_ptr<testing::Session>> remotes;
    for (int i = 0; i < 16; ++i) {
        remotes.push_back(std::make_unique<testing::Session>(api(), e.raw(), FOXAPI_SESSION_REMOTE,
                                                             kAllGrants, "t"));
        CHECK(remotes.back()->ok());
    }
    testing::Session seventeenth(api(), e.raw(), FOXAPI_SESSION_REMOTE, kAllGrants, "t");
    CHECK_EQ(seventeenth.openStatus, FOXAPI_LIMIT);
    testing::Session local(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    CHECK(local.ok());
    remotes.pop_back();  // one closes: its place is free again
    testing::Session again(api(), e.raw(), FOXAPI_SESSION_REMOTE, kAllGrants, "t");
    CHECK(again.ok());
}

// A SESSION DOES NOT OUTLIVE ITS TOKEN. Round-2 review: the token was checked
// only at open_session, so a session lived on past the 12-hour limit. Now the
// session is detached when its token expires (checked on the control
// thread's pass and on every submit), exactly as a logout detaches it.
void testSessionDiesWithItsToken() {
    testing::Engine e(api(), "user=op;password=pw;tokenTtlMs=300");
    const FoxEngineApi* a = api();
    char tok[80] = {};
    CHECK_EQ(a->login(e.raw(), "op", "pw", tok, sizeof(tok)), 64);
    testing::Session remote(api(), e.raw(), FOXAPI_SESSION_REMOTE, kAllGrants, tok);
    CHECK(remote.ok());
    CHECK_EQ(a->heartbeat(remote.raw()), FOXAPI_OK);
    FoxCommand c = cmd(FOXAPI_OP_SET_VOLUME, 0.2);
    CHECK_EQ(a->submit(remote.raw(), &c, 1, nullptr), 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(450));
    // Detached by the control thread's own pass, before this session has
    // done anything after the expiry - a session that only beats (or does
    // nothing at all) dies with its token too.
    CHECK_EQ(a->heartbeat(remote.raw()), FOXAPI_DETACHED);
    CHECK_EQ(a->submit(remote.raw(), &c, 1, nullptr), FOXAPI_DETACHED);
    testing::Session late(api(), e.raw(), FOXAPI_SESSION_REMOTE, kAllGrants, tok);
    CHECK_EQ(late.openStatus, FOXAPI_UNAUTHENTICATED);
}

// A TOKEN THAT EXPIRES DETACHES EVERY SESSION, HOWEVER THE EXPIRY IS NOTICED.
// Round-3 review: presenting an expired token to open_session made
// tokenValid() ERASE it without detaching its sessions, so the control
// thread's expiry pass never saw it: the sessions lived on past the token
// (heartbeat OK 200 ms after expiry) and logout answered NOT_FOUND for a token
// whose sessions were still attached. Here the token is presented again and
// again across its expiry, by open_session and by submit, with sessions held
// open along the way; every one of them must end DETACHED, and a later logout
// answers NOT_FOUND (the token is gone) with nothing left attached.
void testTokenExpiryDetachesEverySession() {
    const FoxEngineApi* a = api();
    for (int round = 0; round < 5; ++round) {
        testing::Engine e(api(), "user=op;password=pw;tokenTtlMs=300");
        char tok[80] = {};
        CHECK_EQ(a->login(e.raw(), "op", "pw", tok, sizeof(tok)), 64);
        std::vector<FoxSession*> held;
        FoxSession* viewer = nullptr;
        FoxSessionParams p{};
        p.structSize = sizeof(p);
        p.flags = FOXAPI_SESSION_REMOTE;
        p.grants = FOXAPI_GRANT_VIEW;
        p.token = tok;
        p.clientName = "probe";
        CHECK_EQ(a->open_session(e.raw(), &p, &viewer), FOXAPI_OK);
        int opened = 0;
        const auto t0 = std::chrono::steady_clock::now();
        // Hammer open_session with the token until it is refused.
        while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(2000)) {
            FoxSession* s = nullptr;
            const int32_t st = a->open_session(e.raw(), &p, &s);
            if (st != FOXAPI_OK) {
                CHECK_EQ(st, FOXAPI_UNAUTHENTICATED);
                break;
            }
            ++opened;
            if (opened % 97 == 0 && held.size() < 12) {
                held.push_back(s);  // kept open across the expiry
            } else {
                a->close_session(s);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const int32_t hb = a->heartbeat(viewer);
        int stillAttached = hb == FOXAPI_OK ? 1 : 0;
        for (FoxSession* s : held) {
            stillAttached += a->heartbeat(s) == FOXAPI_OK ? 1 : 0;
        }
        const int32_t lo = a->logout(e.raw(), tok);
        std::printf("token round %d: %d opens before refusal, %zu held; viewer heartbeat %d, "
                    "attached after expiry %d, logout %d\n",
                    round, opened, held.size(), hb, stillAttached, lo);
        CHECK_EQ(hb, FOXAPI_DETACHED);
        CHECK_EQ(stillAttached, 0);
        CHECK_EQ(lo, FOXAPI_NOT_FOUND);
        FoxCommand c = cmd(FOXAPI_OP_SET_VOLUME, 0.2);
        CHECK_EQ(a->submit(viewer, &c, 1, nullptr), FOXAPI_DETACHED);
        for (FoxSession* s : held) {
            a->close_session(s);
        }
        a->close_session(viewer);
    }
    // The same through submit: the first thing to notice the expiry is the
    // session's own submit, which must detach EVERY session on the token.
    {
        testing::Engine e(api(), "user=op;password=pw;tokenTtlMs=300;controlHz=20");
        char tok[80] = {};
        CHECK_EQ(a->login(e.raw(), "op", "pw", tok, sizeof(tok)), 64);
        testing::Session one(api(), e.raw(), FOXAPI_SESSION_REMOTE, kAllGrants, tok);
        testing::Session two(api(), e.raw(), FOXAPI_SESSION_REMOTE, kAllGrants, tok);
        FoxCommand c = cmd(FOXAPI_OP_SET_VOLUME, 0.2);
        int32_t st = 1;
        const auto t0 = std::chrono::steady_clock::now();
        while (st >= 0 && std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(2000)) {
            st = a->submit(one.raw(), &c, 1, nullptr);
            FoxCommandResult r[4];
            for (auto& x : r) x.structSize = sizeof(x);
            a->poll_results(one.raw(), r, 4);
        }
        CHECK_EQ(st, FOXAPI_DETACHED);
        CHECK_EQ(a->heartbeat(two.raw()), FOXAPI_DETACHED);
        CHECK_EQ(a->logout(e.raw(), tok), FOXAPI_NOT_FOUND);
    }
}

// DETACHED SESSIONS DO NOT HOLD REMOTE PLACES. Round-3 review: a session
// detached by logout still counted toward the 16-remote cap until the
// transport got round to closing it, so a logout could lock every new remote
// out. The cap counts attached remote sessions; the overall limit still
// counts every session until it is closed.
void testDetachedSessionsFreeRemotePlaces() {
    testing::Engine e(api(), "user=op;password=pw");
    const FoxEngineApi* a = api();
    char tokA[80] = {};
    CHECK_EQ(a->login(e.raw(), "op", "pw", tokA, sizeof(tokA)), 64);
    std::vector<std::unique_ptr<testing::Session>> first;
    for (int i = 0; i < 16; ++i) {
        first.push_back(std::make_unique<testing::Session>(api(), e.raw(), FOXAPI_SESSION_REMOTE,
                                                           kAllGrants, tokA));
        CHECK(first.back()->ok());
    }
    char tokB[80] = {};
    CHECK_EQ(a->login(e.raw(), "op", "pw", tokB, sizeof(tokB)), 64);
    testing::Session over(api(), e.raw(), FOXAPI_SESSION_REMOTE, kAllGrants, tokB);
    CHECK_EQ(over.openStatus, FOXAPI_LIMIT);
    CHECK_EQ(a->logout(e.raw(), tokA), FOXAPI_OK);  // 16 detached, none closed yet
    CHECK_EQ(a->heartbeat(first.back()->raw()), FOXAPI_DETACHED);
    std::vector<std::unique_ptr<testing::Session>> second;
    for (int i = 0; i < 16; ++i) {
        second.push_back(std::make_unique<testing::Session>(api(), e.raw(), FOXAPI_SESSION_REMOTE,
                                                            kAllGrants, tokB));
        CHECK(second.back()->ok());
    }
    testing::Session seventeenth(api(), e.raw(), FOXAPI_SESSION_REMOTE, kAllGrants, tokB);
    CHECK_EQ(seventeenth.openStatus, FOXAPI_LIMIT);  // still 16 attached at most
}

// Holds the engine's control thread for `ms` (mock.stall, an ADMIN setting)
// and returns once it is inside the stall.
void holdControlThread(testing::Session& admin, int ms) {
    const std::string t = "mock.stall=" + std::to_string(ms);
    FoxCommand c = cmd(FOXAPI_OP_SETTING_SET, 0, 0, t.c_str());
    api()->submit(admin.raw(), &c, 1, nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
}

// DETACHED SESSIONS ARE CLOSED BY THE ENGINE, AND THE LOCAL WINDOW ALWAYS HAS
// A PLACE. Round-4 review: detached remote sessions no longer counted toward
// the 16-remote cap but still counted toward the 64 overall, so a remote
// user with the password could log in, open 16, log out, four times over,
// and the LOCAL window's open_session answered LIMIT. Now the engine closes a
// detached session itself on its next control pass (its handle stays valid
// and answers DETACHED until close_session), and 4 of the 64 places are only
// ever given to LOCAL sessions.
void testDetachedSessionsAreClosedAndLocalHasPlaces() {
    const FoxEngineApi* a = api();
    {   // the review's probe
        testing::Engine e(api(), "user=op;password=pw");
        std::vector<std::unique_ptr<testing::Session>> held;
        int logins = 0;
        for (int round = 0; round < 8 && held.size() < 64; ++round) {
            char tok[80] = {};
            if (a->login(e.raw(), "op", "pw", tok, sizeof(tok)) != 64) break;
            ++logins;
            for (int i = 0; i < 16 && held.size() < 64; ++i) {
                auto s = std::make_unique<testing::Session>(api(), e.raw(), FOXAPI_SESSION_REMOTE,
                                                            FOXAPI_GRANT_VIEW, tok);
                if (!s->ok()) break;
                held.push_back(std::move(s));
            }
            a->logout(e.raw(), tok);  // detached; the client keeps its handles open
        }
        testing::Session local(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
        std::printf("%d logins, %zu remote sessions opened and detached; LOCAL open -> %d\n", logins,
                    held.size(), local.openStatus);
        CHECK_EQ(local.openStatus, FOXAPI_OK);
        CHECK(!held.empty());
        CHECK_EQ(a->heartbeat(held.front()->raw()), FOXAPI_DETACHED);  // handle still valid
    }
    {   // the reserve, with nothing closed meanwhile (the control thread held)
        testing::Engine e(api(), "user=op;password=pw");
        testing::Session admin(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
        holdControlThread(admin, 800);
        std::vector<std::unique_ptr<testing::Session>> held;
        int32_t refusal = FOXAPI_OK;
        for (int round = 0; round < 8 && refusal == FOXAPI_OK; ++round) {
            char tok[80] = {};
            CHECK_EQ(a->login(e.raw(), "op", "pw", tok, sizeof(tok)), 64);
            for (int i = 0; i < 16; ++i) {
                auto s = std::make_unique<testing::Session>(api(), e.raw(), FOXAPI_SESSION_REMOTE,
                                                            FOXAPI_GRANT_VIEW, tok);
                if (!s->ok()) {
                    refusal = s->openStatus;
                    break;
                }
                held.push_back(std::move(s));
            }
            a->logout(e.raw(), tok);
        }
        // 64 places: 1 admin + 59 remote, and 4 kept for LOCAL sessions.
        CHECK_EQ(refusal, FOXAPI_LIMIT);
        CHECK_EQ(held.size(), static_cast<std::size_t>(64 - 4 - 1));
        std::vector<std::unique_ptr<testing::Session>> locals;
        for (int i = 0; i < 4; ++i) {
            locals.push_back(std::make_unique<testing::Session>(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants));
            CHECK(locals.back()->ok());
        }
        testing::Session over(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
        CHECK_EQ(over.openStatus, FOXAPI_LIMIT);  // 64 in all is still the limit
        std::printf("with the control thread held: %zu remote opened, then LIMIT; 4 LOCAL opened\n", held.size());
        // When the control thread runs again it closes the detached ones, so
        // their places are free without the client closing anything.
        std::this_thread::sleep_for(std::chrono::milliseconds(900));
        char tok[80] = {};
        CHECK_EQ(a->login(e.raw(), "op", "pw", tok, sizeof(tok)), 64);
        testing::Session fresh(api(), e.raw(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW, tok);
        CHECK(fresh.ok());
        CHECK_EQ(a->heartbeat(held.back()->raw()), FOXAPI_DETACHED);
        held.clear();  // close_session on sessions the engine already closed
        testing::Session afterClose(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
        CHECK(afterClose.ok());
    }
}

// THE CONFIGURED TOKEN IS NOT A LOGIN. Round-4 review: logout() of the
// engine's configured token answered NOT_FOUND yet detached every session
// opened with it, while the token went on opening new ones. The configured
// token is configuration: logout cannot revoke it, answers DENIED and
// detaches nothing (only changing the configuration revokes it).
void testLogoutCannotRevokeTheConfiguredToken() {
    const FoxEngineApi* a = api();
    testing::Engine e(api(), "token=t;user=op;password=pw");
    testing::Session r(api(), e.raw(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW, "t");
    CHECK(r.ok());
    CHECK_EQ(a->logout(e.raw(), "t"), FOXAPI_DENIED);
    CHECK_EQ(a->heartbeat(r.raw()), FOXAPI_OK);  // nothing detached
    FoxCommand c = cmd(FOXAPI_OP_SET_VOLUME, 0.2);
    CHECK_EQ(a->submit(r.raw(), &c, 1, nullptr), 1);
    testing::Session r2(api(), e.raw(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW, "t");
    CHECK(r2.ok());
    // A login token beside it is still revoked as ever.
    char tok[80] = {};
    CHECK_EQ(a->login(e.raw(), "op", "pw", tok, sizeof(tok)), 64);
    testing::Session byLogin(api(), e.raw(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW, tok);
    CHECK_EQ(a->logout(e.raw(), tok), FOXAPI_OK);
    CHECK_EQ(a->heartbeat(byLogin.raw()), FOXAPI_DETACHED);
    CHECK_EQ(a->heartbeat(r.raw()), FOXAPI_OK);
}

// LOGOUT OF A TOKEN THAT EXPIRED BUT HAS NOT BEEN SWEPT YET answers
// NOT_FOUND (it was not live), and still detaches its sessions. The control
// thread is held so its once-a-pass sweep cannot drop the token first.
// (Round-4 review: the `live` check in logout was never exercised.)
void testLogoutOfAnExpiredUnsweptToken() {
    const FoxEngineApi* a = api();
    testing::Engine e(api(), "user=op;password=pw;tokenTtlMs=150");
    testing::Session admin(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    char tok[80] = {};
    CHECK_EQ(a->login(e.raw(), "op", "pw", tok, sizeof(tok)), 64);
    testing::Session remote(api(), e.raw(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW, tok);
    CHECK(remote.ok());
    holdControlThread(admin, 600);
    std::this_thread::sleep_for(std::chrono::milliseconds(170));  // expired at 150 ms, not swept
    CHECK_EQ(a->logout(e.raw(), tok), FOXAPI_NOT_FOUND);
    CHECK_EQ(a->heartbeat(remote.raw()), FOXAPI_DETACHED);
}

// OPEN_SESSION CHECKS THE TOKEN AGAIN ONCE THE SESSION IS LISTED. A logout
// that lands between open_session's token check and its insert detaches a
// session list that does not hold the new session yet; without the second
// check that session would live on a revoked token. The mock.openDelay seam
// holds the open inside that gap while the token is revoked.
void testOpenSessionRechecksTheToken() {
    const FoxEngineApi* a = api();
    testing::Engine e(api(), "user=op;password=pw");
    testing::Session admin(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    char tok[80] = {};
    CHECK_EQ(a->login(e.raw(), "op", "pw", tok, sizeof(tok)), 64);
    CHECK_EQ(admin.run(cmd(FOXAPI_OP_SETTING_SET, 0, 0, "mock.openDelay=300")).status(), FOXAPI_OK);
    int32_t opened = FOXAPI_FAILED;
    FoxSession* s = nullptr;
    std::thread t([&] {
        FoxSessionParams p{};
        p.structSize = sizeof(p);
        p.flags = FOXAPI_SESSION_REMOTE;
        p.grants = FOXAPI_GRANT_VIEW;
        p.token = tok;
        p.clientName = "slow open";
        opened = a->open_session(e.raw(), &p, &s);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // inside the gap
    const int32_t lo = a->logout(e.raw(), tok);
    t.join();
    std::printf("logout inside open_session's gap -> %d; the open -> %d\n", lo, opened);
    CHECK_EQ(lo, FOXAPI_OK);
    CHECK_EQ(opened, FOXAPI_UNAUTHENTICATED);
    if (opened == FOXAPI_OK && s != nullptr) {
        CHECK_EQ(a->heartbeat(s), FOXAPI_DETACHED);
        a->close_session(s);
    }
    CHECK_EQ(admin.run(cmd(FOXAPI_OP_SETTING_SET, 0, 0, "mock.openDelay=0")).status(), FOXAPI_OK);
}

void testSessionLimitAndLogin() {
    {
        testing::Engine e(api(), "maxSessions=2");
        testing::Session a1(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
        testing::Session a2(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
        testing::Session a3(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
        CHECK(a1.ok() && a2.ok());
        CHECK_EQ(a3.openStatus, FOXAPI_LIMIT);
        a1.close();
        testing::Session a4(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
        CHECK(a4.ok());  // a closed session frees its place
    }
    {   // The last 4 places are LOCAL-only, so an engine configured with 4
        // or fewer places serves LOCAL sessions only (round-5 review, r8:
        // documented in API.md 4); with 5, one remote fits.
        testing::Engine small(api(), "maxSessions=4;token=t");
        testing::Session r(api(), small.raw(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_TUNE, "t");
        CHECK_EQ(r.openStatus, FOXAPI_LIMIT);
        testing::Session l(api(), small.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
        CHECK(l.ok());
        testing::Engine five(api(), "maxSessions=5;token=t");
        testing::Session r1(api(), five.raw(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_TUNE, "t");
        testing::Session r2(api(), five.raw(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_TUNE, "t");
        CHECK(r1.ok());
        CHECK_EQ(r2.openStatus, FOXAPI_LIMIT);
    }
    testing::Engine e(api(), "user=op;password=pw;loginLockoutMs=300;keepaliveMs=150");
    const FoxEngineApi* a = api();
    char tok[80] = {};
    CHECK_EQ(a->login(e.raw(), "op", "wrong", tok, sizeof(tok)), FOXAPI_UNAUTHENTICATED);
    CHECK_EQ(a->login(e.raw(), "op", "pw", tok, 10), FOXAPI_BAD_ARGUMENT);  // too small for a token
    const int32_t n = a->login(e.raw(), "op", "pw", tok, sizeof(tok));
    CHECK_EQ(n, 64);
    CHECK_EQ(std::strlen(tok), static_cast<std::size_t>(64));
    {
        testing::Session guess(api(), e.raw(), FOXAPI_SESSION_REMOTE, kAllGrants, "not-a-token");
        CHECK_EQ(guess.openStatus, FOXAPI_UNAUTHENTICATED);
    }
    testing::Session remote(api(), e.raw(), FOXAPI_SESSION_REMOTE, kAllGrants, tok);
    CHECK(remote.ok());
    CHECK_EQ(remote.state().grants, kAllGrants & ~FOXAPI_GRANT_ADMIN);
    // Logout revokes the token: no new session, and the open one is DETACHED.
    CHECK_EQ(a->logout(e.raw(), tok), FOXAPI_OK);
    CHECK_EQ(a->logout(e.raw(), tok), FOXAPI_NOT_FOUND);
    CHECK_EQ(a->heartbeat(remote.raw()), FOXAPI_DETACHED);
    FoxCommand c = cmd(FOXAPI_OP_SET_VOLUME, 0.1);
    CHECK_EQ(a->submit(remote.raw(), &c, 1, nullptr), FOXAPI_DETACHED);
    {
        testing::Session again(api(), e.raw(), FOXAPI_SESSION_REMOTE, kAllGrants, tok);
        CHECK_EQ(again.openStatus, FOXAPI_UNAUTHENTICATED);
    }
    // Five failures lock login out - even the right password - until the
    // lockout passes.
    for (int i = 0; i < 5; ++i) {
        a->login(e.raw(), "op", "guess", tok, sizeof(tok));
    }
    CHECK_EQ(a->login(e.raw(), "op", "pw", tok, sizeof(tok)), FOXAPI_LIMIT);
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    CHECK_EQ(a->login(e.raw(), "op", "pw", tok, sizeof(tok)), 64);
    // No credential configured: login is refused.
    testing::Engine bare(api());
    CHECK_EQ(a->login(bare.raw(), "", "", tok, sizeof(tok)), FOXAPI_UNAUTHENTICATED);
}

void testEvents() {
    testing::Engine e(api());
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    const FoxEngineApi* a = api();
    // Nothing until subscribed.
    s.run(cmd(FOXAPI_OP_SET_VOLUME, 0.4));
    CHECK(s.events().empty());
    a->subscribe(s.raw(), FOXAPI_EVENT_MASK(FOXAPI_EVENT_STATE));
    s.run(cmd(FOXAPI_OP_SET_VOLUME, 0.6));
    std::vector<FoxEvent> ev;
    CHECK(check::waitFor([&] {
        auto more = s.events();
        ev.insert(ev.end(), more.begin(), more.end());
        return !ev.empty();
    }, 1000));
    bool sawAudio = false;
    for (const auto& x : ev) {
        sawAudio = sawAudio || (x.kind == FOXAPI_EVENT_STATE &&
                                (static_cast<uint32_t>(x.value[0]) & (1u << 3)) != 0u);
    }
    CHECK(sawAudio);
    // A slow reader is told it missed something rather than silently losing it.
    for (int i = 0; i < 300; ++i) {
        s.run(cmd(FOXAPI_OP_SET_VOLUME, (i % 2) ? 0.1 : 0.2));
    }
    ev = s.events();
    CHECK(!ev.empty());
    if (!ev.empty()) {
        CHECK_EQ(ev.front().kind, FOXAPI_EVENT_OVERFLOW);
        CHECK(ev.front().value[0] > 0.0);
    }
}

void testSourcesAndLists() {
    testing::Engine e(api());
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    const FoxEngineApi* a = api();
    FoxListItem it{};
    it.structSize = sizeof(it);
    CHECK_EQ(a->read_list(s.raw(), FOXAPI_LIST_MODES, 0, nullptr), 8);
    CHECK_EQ(a->read_list(s.raw(), FOXAPI_LIST_MODES, 1, &it), 8);
    CHECK_EQ(std::string(it.name), std::string("WFM"));
    CHECK_EQ(a->read_list(s.raw(), 999u, 0, &it), FOXAPI_UNSUPPORTED);
    // Before a scan only the generator is listed, and a radio cannot be picked.
    CHECK_EQ(a->read_list(s.raw(), FOXAPI_LIST_DEVICES, 0, nullptr), 1);
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SELECT_SOURCE, 0, 0, "mock:rtl:00000001")).result.status,
             FOXAPI_NOT_FOUND);
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SCAN_DEVICES)).result.status, FOXAPI_OK);
    CHECK_EQ(a->read_list(s.raw(), FOXAPI_LIST_DEVICES, 0, nullptr), 2);
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SELECT_SOURCE, 0, 0, "mock:rtl:00000001")).result.status,
             FOXAPI_OK);
    FoxReceiverState st = s.state();
    CHECK((st.flags & FOXAPI_RX_DEVICE_OPEN) != 0u);
    CHECK_EQ(st.gainCount, 1u);
    CHECK_EQ(a->read_list(s.raw(), FOXAPI_LIST_SAMPLE_RATES, 0, nullptr), 6);
    auto o = s.run(cmd(FOXAPI_OP_SET_GAIN, 30.04, 0, "TUNER"));
    CHECK_EQ(o.result.status, FOXAPI_OK);
    CHECK_EQ(o.result.applied[0], 30.0);
    CHECK((o.result.flags & FOXAPI_RESULT_CLAMPED) != 0u);
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SET_SAMPLE_RATE, 2.4e6)).result.status, FOXAPI_OK);
    CHECK_EQ(s.state().sampleRateHz, 2.4e6);
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SET_SAMPLE_RATE, 2.5e6)).result.status, FOXAPI_OUT_OF_RANGE);
}

void testBookmarks() {
    testing::Engine e(api());
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    const FoxEngineApi* a = api();
    s.run(cmd(FOXAPI_OP_SET_FREQUENCY, 145.5e6));
    s.run(cmd(FOXAPI_OP_SET_MODE, 0, FOXAPI_DEMOD_NFM));
    s.run(cmd(FOXAPI_OP_SET_BANDWIDTH, 11000.0));
    auto o = s.run(cmd(FOXAPI_OP_BOOKMARK_ADD, 0, 0, "Calling"));
    CHECK_EQ(o.result.status, FOXAPI_OK);
    const long long id = static_cast<long long>(o.result.applied[0]);
    FoxListItem it{};
    it.structSize = sizeof(it);
    CHECK_EQ(a->read_list(s.raw(), FOXAPI_LIST_BOOKMARKS, 0, &it), 1);
    CHECK_EQ(std::string(it.name), std::string("Calling"));
    CHECK_EQ(it.value[0], 145.5e6);
    CHECK_EQ(it.value[1], static_cast<double>(FOXAPI_DEMOD_NFM));
    CHECK_EQ(it.value[2], 11000.0);  // the bandwidth, as the app keeps it
    CHECK_EQ(std::string(it.detail), std::string(""));  // ungrouped
    // A name already in use is made unique, as FreqManager::add does.
    s.run(cmd(FOXAPI_OP_SET_FREQUENCY, 145.6e6));
    CHECK_EQ(s.run(cmd(FOXAPI_OP_BOOKMARK_ADD, 0, 0, "Calling")).status(), FOXAPI_OK);
    CHECK_EQ(a->read_list(s.raw(), FOXAPI_LIST_BOOKMARKS, 1, &it), 2);
    CHECK_EQ(std::string(it.name), std::string("Calling (2)"));
    // Tuning a bookmark restores its frequency, mode AND bandwidth.
    s.run(cmd(FOXAPI_OP_SET_FREQUENCY, 94.62e6));
    s.run(cmd(FOXAPI_OP_SET_MODE, 0, FOXAPI_DEMOD_WFM));
    CHECK_EQ(s.run(cmd(FOXAPI_OP_BOOKMARK_TUNE, 0, id)).status(), FOXAPI_OK);
    FoxReceiverState st = s.state();
    CHECK_EQ(st.tunedHz, 145.5e6);
    CHECK_EQ(st.demodMode, FOXAPI_DEMOD_NFM);
    CHECK_EQ(st.bandwidthHz, 11000.0);
    // Favourites.
    FoxCommand fav = cmd(FOXAPI_OP_BOOKMARK_FAVOURITE, 0, id);
    fav.ival[1] = 1;
    CHECK_EQ(s.run(fav).status(), FOXAPI_OK);
    CHECK_EQ(a->read_list(s.raw(), FOXAPI_LIST_BOOKMARKS, 0, &it), 2);
    CHECK((it.flags & FOXAPI_ITEM_FAVOURITE) != 0u);
    // An imported list is bookmarks with a GROUP (the app has no separate
    // "frequency list"): the list stays sorted by frequency, the groups are
    // listed with their counts, and a group can be removed whole.
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SETTING_SET, 0, 0, "mock.bookmark=ATIS|118.7e6|3|8000|Airband")).status(),
             FOXAPI_OK);
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SETTING_SET, 0, 0, "mock.bookmark=Approach|124.325e6|3|8000|Airband")).status(),
             FOXAPI_OK);
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SETTING_SET, 0, 0, "mock.bookmark=PMR 1|446.00625e6|1|12500|PMR446")).status(),
             FOXAPI_OK);
    CHECK_EQ(a->read_list(s.raw(), FOXAPI_LIST_BOOKMARKS, 0, nullptr), 5);
    std::vector<double> hz;
    for (uint32_t i = 0; i < 5; ++i) {
        FoxListItem b{};
        b.structSize = sizeof(b);
        a->read_list(s.raw(), FOXAPI_LIST_BOOKMARKS, i, &b);
        hz.push_back(b.value[0]);
    }
    CHECK(std::is_sorted(hz.begin(), hz.end()));
    CHECK_EQ(a->read_list(s.raw(), FOXAPI_LIST_BOOKMARKS, 0, &it), 5);
    CHECK_EQ(std::string(it.name), std::string("ATIS"));
    CHECK_EQ(std::string(it.detail), std::string("Airband"));
    FoxListItem g{};
    g.structSize = sizeof(g);
    CHECK_EQ(a->read_list(s.raw(), FOXAPI_LIST_BOOKMARK_GROUPS, 0, &g), 2);
    CHECK_EQ(std::string(g.name), std::string("Airband"));
    CHECK_EQ(g.value[0], 2.0);
    auto rg = s.run(cmd(FOXAPI_OP_BOOKMARK_REMOVE_GROUP, 0, 0, "Airband"));
    CHECK_EQ(rg.status(), FOXAPI_OK);
    CHECK_EQ(rg.result.applied[0], 2.0);
    CHECK_EQ(a->read_list(s.raw(), FOXAPI_LIST_BOOKMARKS, 0, nullptr), 3);
    CHECK_EQ(a->read_list(s.raw(), FOXAPI_LIST_BOOKMARK_GROUPS, 0, nullptr), 1);
    CHECK_EQ(s.run(cmd(FOXAPI_OP_BOOKMARK_REMOVE_GROUP, 0, 0, "Airband")).status(), FOXAPI_NOT_FOUND);
    // Remove by id.
    CHECK_EQ(s.run(cmd(FOXAPI_OP_BOOKMARK_REMOVE, 0, id)).status(), FOXAPI_OK);
    CHECK_EQ(a->read_list(s.raw(), FOXAPI_LIST_BOOKMARKS, 0, nullptr), 2);
    CHECK_EQ(s.run(cmd(FOXAPI_OP_BOOKMARK_TUNE, 0, id)).status(), FOXAPI_NOT_FOUND);
}

void testAudioAndSettings() {
    testing::Engine e(api(), "fps=60");
    const FoxEngineApi* a = api();
    testing::Session noAudio(api(), e.raw(), FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW);
    uint64_t cursor = 0;
    float buf[4800];
    CHECK_EQ(a->read_audio(noAudio.raw(), &cursor, buf, 4800, nullptr), FOXAPI_DENIED);
    testing::Session s(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    s.run(cmd(FOXAPI_OP_SET_FREQUENCY, 94.62e6));  // the strong station: squelch open
    FoxStreamInfo info{};
    info.structSize = sizeof(info);
    // A short info is refused, not silently left unfilled - and the cursor
    // does not move, so no audio is lost to the refusal.
    FoxStreamInfo shortInfo{};
    shortInfo.structSize = static_cast<uint32_t>(FOXAPI_MIN_STREAM_INFO - 8);
    uint64_t probe = 12345;
    CHECK_EQ(a->read_audio(s.raw(), &probe, buf, 4800, &shortInfo), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(probe, 12345ull);
    CHECK_EQ(a->read_audio(s.raw(), &cursor, buf, 4800, &info), 0);  // joins live
    CHECK_EQ(info.rateHz, 48000.0);
    int64_t total = 0;
    CHECK(check::waitFor([&] {
        const int32_t n = a->read_audio(s.raw(), &cursor, buf, 4800, &info);
        total += std::max(n, 0);
        return total >= 4800;
    }, 2000));
    CHECK(s.state().audioLevelDb > -60.0);

    char v[64];
    CHECK_EQ(a->get_setting(s.raw(), "spectrum.bins", v, sizeof(v)), 4);
    CHECK_EQ(std::string(v), std::string("2048"));
    CHECK_EQ(a->get_setting(s.raw(), "no.such.key", v, sizeof(v)), FOXAPI_NOT_FOUND);
    CHECK_EQ(s.run(cmd(FOXAPI_OP_SETTING_SET, 0, 0, "ui.note=hello")).result.status, FOXAPI_OK);
    CHECK_EQ(a->get_setting(s.raw(), "ui.note", v, 3), 5);  // length, even when cut
    CHECK_EQ(std::string(v), std::string("he"));
}

// RULE 1: a reader never sees a half-written snapshot, however hard the
// control thread is publishing. tunedHz is published as centre + offset in
// the same write; a torn copy would break that identity.
void testSnapshotsNeverTear() {
    testing::Engine e(api(), "controlHz=1000");
    testing::Session writer(api(), e.raw(), FOXAPI_SESSION_LOCAL, kAllGrants);
    testing::Session reader(api(), e.raw(), FOXAPI_SESSION_LOCAL, FOXAPI_GRANT_VIEW);
    const FoxEngineApi* a = api();
    std::atomic<bool> stop{false};
    std::atomic<long> reads{0};
    std::atomic<long> torn{0};
    std::atomic<long> busy{0};
    std::thread r([&] {
        FoxReceiverState st{};
        while (!stop.load()) {
            st.structSize = sizeof(st);
            const int32_t rc = a->read_state(reader.raw(), &st);
            if (rc == FOXAPI_BUSY) {
                ++busy;
                continue;
            }
            ++reads;
            if (st.tunedHz != st.centreHz + st.vfoOffsetHz) {
                ++torn;
            }
        }
    });
    for (int i = 0; i < 400; ++i) {
        FoxCommand c[2] = {cmd(FOXAPI_OP_SET_FREQUENCY, 90e6 + i * 12345.0),
                           cmd(FOXAPI_OP_SET_VFO_OFFSET, (i % 7) * 1000.0 - 3000.0)};
        a->submit(writer.raw(), c, 2, nullptr);
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        FoxCommandResult drain[8];
        for (auto& x : drain) x.structSize = sizeof(x);
        a->poll_results(writer.raw(), drain, 8);
    }
    stop.store(true);
    r.join();
    std::printf("snapshot reads=%ld busy=%ld torn=%ld\n", reads.load(), busy.load(), torn.load());
    CHECK(reads.load() > 1000);
    CHECK_EQ(torn.load(), 0L);
}

}  // namespace

int main() {
    testVersionGate();
    testSessions();
    testDefaultState();
    testSpectrum();
    testTuningAndClamps();
    testRefusals();
    testBatchAndQueueBound();
    testRefusalsAreResults();
    testRefusedFlagMeansNothingChanged();
    testSessionLimitAndLogin();
    testPendingCountsUntilPolled();
    testRemoteSessionLimit();
    testSessionDiesWithItsToken();
    testTokenExpiryDetachesEverySession();
    testDetachedSessionsFreeRemotePlaces();
    testDetachedSessionsAreClosedAndLocalHasPlaces();
    testLogoutCannotRevokeTheConfiguredToken();
    testLogoutOfAnExpiredUnsweptToken();
    testOpenSessionRechecksTheToken();
    testEvents();
    testSourcesAndLists();
    testBookmarks();
    testAudioAndSettings();
    testSnapshotsNeverTear();
    return check::finish("test_mock_engine");
}
