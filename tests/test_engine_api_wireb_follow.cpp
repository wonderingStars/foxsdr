// test_engine_api_wireb_follow.cpp - RADIOS THAT FOLLOW A MASTER THROUGH THE ENGINE (wire-b; FOXAPP_OP_FOLLOW_SET/CLEAR/LOCK/KEEP 0x8926-0x8929; Engine::pumpFollow over core/radio_matrix_model.hpp FollowSet),
// on a real Engine with three radios on the signal generator.
//
//   F1  FOLLOW_SET records the distance (applied[0]); when the master retunes, the follower is retuned to master + offset on the next pass; the matrix fact says who follows whom
//   F2  LOCK: a locked follower stays while its master moves; unlock: the next move brings it along (the distance it had)
//   F3  KEEP: a follower nudged by hand keeps its new distance when FOLLOW_KEEP is sent, and follows with it from then on
//   F4  a chain (3 follows 2 follows 1) moves all three, master first; a link that would make a circle, a radio following itself, a second master for a follower are refused
//   F5  CLEAR frees a follower (it stays where it is when the master moves); removing a radio ends every link it was in; refusals; REMOTE DENIED
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>

#include "wire1_rig.hpp"

namespace {

FoxCommand cmdI(std::uint32_t op, long long a, long long b = 0) {
    FoxCommand c = testing::cmd(op, 0.0, a, nullptr);
    c.ival[1] = b;
    return c;
}

double tuned(Rig& r, std::uint32_t radio) { return r.radio(radio).tunedHz; }

void tune(Rig& r, std::uint32_t radio, double hz) {
    CHECK_EQ(r.send(r.aimed(FOXAPI_OP_SET_FREQUENCY, radio, hz)).status(), FOXAPI_OK);
}

bool closeTo(double a, double b) { return std::fabs(a - b) < 1.0; }

json matrix(Rig& r) {
    std::string buf(1 << 16, '\0');
    const int32_t n = api()->get_setting(r.s->raw(), "fact.matrix.rows", &buf[0], buf.size());
    if (n < 0) { return json(); }
    const json j = json::parse(buf.c_str(), nullptr, false);
    return j.is_discarded() ? json() : j;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_wireb_follow\n");
    engine_api_test::hermetic("wirebfollow");
    Access::installHooks();
    Rig r;
    CHECK_EQ(r.add(2, "generator", true).status(), FOXAPI_OK);
    CHECK_EQ(r.add(3, "generator", true).status(), FOXAPI_OK);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
    tune(r, 1, 100.0e6);
    tune(r, 2, 100.5e6);
    tune(r, 3, 101.0e6);
    r.until([] { return false; }, 1500);
    std::printf("  radios at %.0f %.0f %.0f Hz\n", tuned(r, 1), tuned(r, 2), tuned(r, 3));
    CHECK(closeTo(tuned(r, 1), 100.0e6) && closeTo(tuned(r, 2), 100.5e6) && closeTo(tuned(r, 3), 101.0e6));

    std::printf("F1: follow\n");
    {
        const auto o = r.send(cmdI(FOXAPP_OP_FOLLOW_SET, 2, 1));
        CHECK_EQ(o.status(), FOXAPI_OK);
        CHECK(closeTo(o.result.applied[0], 500000.0));
        tune(r, 1, 101.0e6);
        CHECK(r.until([&] { return closeTo(tuned(r, 2), 101.5e6); }, 15000));
        CHECK(closeTo(tuned(r, 3), 101.0e6));   // radio 3 follows nobody
        const json m = matrix(r);
        CHECK(m.is_array() && m.size() == 3);
        if (m.is_array() && m.size() == 3) {
            CHECK_EQ(m[1]["follows"].get<int>(), 1);
            CHECK(closeTo(m[1]["offset_hz"].get<double>(), 500000.0));
            CHECK_EQ(m[0]["follows"].get<int>(), 0);
            CHECK(!m[1]["locked"].get<bool>());
        }
        tune(r, 1, 99.0e6);
        CHECK(r.until([&] { return closeTo(tuned(r, 2), 99.5e6); }, 15000));
    }

    std::printf("F2: lock\n");
    {
        CHECK_EQ(r.send(cmdI(FOXAPP_OP_FOLLOW_LOCK, 2, 1)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(cmdI(FOXAPP_OP_FOLLOW_LOCK, 2, 1)).status(), FOXAPI_NO_CHANGE);
        CHECK(matrix(r)[1]["locked"].get<bool>());
        tune(r, 1, 98.0e6);
        r.until([] { return false; }, 1500);
        CHECK(closeTo(tuned(r, 2), 99.5e6));   // it stayed
        CHECK_EQ(r.send(cmdI(FOXAPP_OP_FOLLOW_LOCK, 2, 0)).status(), FOXAPI_OK);
        tune(r, 1, 97.0e6);
        CHECK(r.until([] { return false; }, 100) || true);
        CHECK(r.until([&] { return closeTo(tuned(r, 2), 97.5e6); }, 15000));   // the distance it had comes back: 500 kHz over the master
    }

    std::printf("F3: keep this distance\n");
    {
        tune(r, 2, 97.8e6);   // nudged by hand: 800 kHz over its master now
        r.until([] { return false; }, 800);
        const auto o = r.send(cmdI(FOXAPP_OP_FOLLOW_KEEP, 2));
        CHECK_EQ(o.status(), FOXAPI_OK);
        CHECK(closeTo(o.result.applied[0], 800000.0));
        tune(r, 1, 96.0e6);
        CHECK(r.until([&] { return closeTo(tuned(r, 2), 96.8e6); }, 15000));
    }

    std::printf("F4: a chain, refusals\n");
    {
        CHECK_EQ(r.send(cmdI(FOXAPP_OP_FOLLOW_SET, 3, 2)).status(), FOXAPI_OK);   // 3 follows 2 follows 1
        const double d32 = tuned(r, 3) - tuned(r, 2);
        tune(r, 1, 95.0e6);
        CHECK(r.until([&] { return closeTo(tuned(r, 2), 95.8e6) && closeTo(tuned(r, 3), 95.8e6 + d32); }, 20000));
        CHECK_EQ(r.send(cmdI(FOXAPP_OP_FOLLOW_SET, 1, 3)).status(), FOXAPI_BAD_ARGUMENT);   // a circle
        CHECK_EQ(r.send(cmdI(FOXAPP_OP_FOLLOW_SET, 2, 2)).status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(r.send(cmdI(FOXAPP_OP_FOLLOW_SET, 3, 1)).status(), FOXAPI_BUSY);          // 3 follows 2 already
        CHECK_EQ(r.send(cmdI(FOXAPP_OP_FOLLOW_SET, 9, 1)).status(), FOXAPI_OUT_OF_RANGE);
        CHECK_EQ(r.send(cmdI(FOXAPP_OP_FOLLOW_SET, 4, 1)).status(), FOXAPI_NO_DEVICE);
    }

    std::printf("F5: clear, removal, REMOTE\n");
    {
        CHECK_EQ(r.send(cmdI(FOXAPP_OP_FOLLOW_CLEAR, 3)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(cmdI(FOXAPP_OP_FOLLOW_CLEAR, 3)).status(), FOXAPI_NO_CHANGE);   // it follows nothing now
        const double stays = tuned(r, 3);
        tune(r, 1, 94.0e6);
        CHECK(r.until([&] { return closeTo(tuned(r, 2), 94.8e6); }, 15000));
        CHECK(closeTo(tuned(r, 3), stays));
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RADIO_REMOVE, 0.0, 2)).status(), FOXAPI_OK);
        r.pump(5);
        CHECK_EQ(r.send(cmdI(FOXAPP_OP_FOLLOW_CLEAR, 0)).status(), FOXAPI_NO_CHANGE);   // radio 2's link went with it
        testing::Session remote(api(), r.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS, "s3cret");
        CHECK(remote.ok());
        FoxCommand c = cmdI(FOXAPP_OP_FOLLOW_SET, 3, 1);
        FoxSubmitResult sr{};
        sr.structSize = sizeof(sr);
        testing::Outcome o;
        const int32_t got = api()->submit(remote.raw(), &c, 1, &sr);
        o.submitStatus = got < 0 ? got : sr.status;
        for (int pass = 0; pass < 40 && o.submitStatus == FOXAPI_OK && !o.landed; ++pass) {
            r.eng.controlPass();
            FoxCommandResult res[8];
            for (auto& x : res) { x.structSize = sizeof(x); }
            const int32_t k = api()->poll_results(remote.raw(), res, 8);
            for (int32_t j = 0; j < k; ++j) { if (res[j].ticket == sr.ticket) { o.landed = true; o.result = res[j]; } }
        }
        CHECK_EQ(o.status(), FOXAPI_DENIED);
    }
    return check::finish("test_engine_api_wireb_follow");
}
