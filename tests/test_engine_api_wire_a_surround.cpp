// test_engine_api_wire_a_surround.cpp - CW_SURROUND 0x8828 THROUGH THE ENGINE (wire-a second pass, item 2): in CW mode the pitch of a signal decides where it sits between the ears.
//   A  off by default: the two ears carry the same audio, the facts say off;  B  on: a carrier above the pitch centre comes out louder in the RIGHT ear, one below it in the LEFT, the live read (pitch and pan)
//   agrees with the ears, invert swaps the sides;  C  a stereo placement of its own wins, and so does turning the surround off (the ears are equal again);  D  the settings: partial JSON keeps what is left out,
//   the same settings are NO_CHANGE, every refusal changes nothing;  E  outside CW mode nothing is placed.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "check.hpp"
#include "wire_a_rig.hpp"

using namespace wire_a_rig;

namespace {

FoxCommand sur(std::int64_t action, const char* text = nullptr) {
    FoxCommand c = testing::cmd(FOXAPP_OP_CW_SURROUND, 0.0, action, nullptr);
    (void)text;
    return c;
}

// The level of each ear over the newest 4096 samples of the audio, and the pitch of the tone by a Goertzel scan.
struct Ears {
    double left = 0.0, right = 0.0;
};
Ears ears(Rig& r) {
    r.settle(300);
    std::vector<float> l(4096), rr(4096);
    const std::size_t n = Access::audioTap(r.eng, l.data(), rr.data(), 4096);
    Ears e;
    for (std::size_t i = 0; i < n; ++i) {
        e.left += static_cast<double>(l[i]) * l[i];
        e.right += static_cast<double>(rr[i]) * rr[i];
    }
    e.left = std::sqrt(e.left / static_cast<double>(std::max<std::size_t>(n, 1)));
    e.right = std::sqrt(e.right / static_cast<double>(std::max<std::size_t>(n, 1)));
    return e;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_wire_a_surround\n");
    engine_api_test::hermetic("wire_a_surround");
    Rig r;
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_MODE, 0.0, FOXAPI_DEMOD_CW)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_SQUELCH, -200.0)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_CW_PITCH, 700.0)).status(), FOXAPI_OK);
    r.eng.controlPass();
    const double vfo = r.local->state().vfoOffsetHz;
    auto tone = [&](double aboveHz) { CHECK(Access::genTone(r.eng, 0, vfo + aboveHz, -20.0f)); };
    auto cfg = [&](const std::string& json) { return r.send(sur(0), &json); };

    std::printf("A: off by default\n");
    {
        CHECK_EQ(r.fact("cwsurround.1.on"), std::string("0"));
        CHECK_EQ(r.fact("cwsurround.1.centre"), std::string("700"));
        CHECK_EQ(r.fact("cwsurround.1.spread"), std::string("400"));
        CHECK_EQ(r.fact("cwsurround.1.invert"), std::string("0"));
        tone(300.0);
        const Ears e = ears(r);
        std::printf("  off: left %.4f right %.4f\n", e.left, e.right);
        CHECK(e.left > 0.001 && std::fabs(e.left - e.right) < 1e-3 * e.left);
    }

    std::printf("B: on, high pitch to the right, low pitch to the left, invert swaps\n");
    {
        CHECK_EQ(cfg(R"({"enabled":true})").status(), FOXAPI_OK);
        CHECK_EQ(r.fact("cwsurround.1.on"), std::string("1"));
        tone(300.0);   // a carrier above the VFO: the beat is the CW pitch plus 300 Hz
        Ears hi = ears(r);
        const FoxCommandResult rd = r.send(sur(1)).result;
        std::printf("  +300 Hz: left %.4f right %.4f; pitch %.1f pan %.2f\n", hi.left, hi.right, rd.applied[0], rd.applied[1]);
        CHECK(hi.right > hi.left * 1.5);
        CHECK(rd.applied[0] > 900.0 && rd.applied[0] < 1100.0);
        CHECK(rd.applied[1] > 0.5);
        tone(-300.0);
        Ears lo = ears(r);
        const FoxCommandResult rd2 = r.send(sur(1)).result;
        std::printf("  -300 Hz: left %.4f right %.4f; pitch %.1f pan %.2f\n", lo.left, lo.right, rd2.applied[0], rd2.applied[1]);
        CHECK(lo.left > lo.right * 1.5);
        CHECK(rd2.applied[0] > 300.0 && rd2.applied[0] < 500.0);
        CHECK(rd2.applied[1] < -0.5);
        // invert: the same low tone now sits on the right
        CHECK_EQ(cfg(R"({"invert":true})").status(), FOXAPI_OK);
        CHECK_EQ(r.fact("cwsurround.1.invert"), std::string("1"));
        Ears inv = ears(r);
        std::printf("  inverted -300 Hz: left %.4f right %.4f\n", inv.left, inv.right);
        CHECK(inv.right > inv.left * 1.5);
        CHECK_EQ(cfg(R"({"invert":false})").status(), FOXAPI_OK);
        // a centre moved to 1000 Hz puts the +300 Hz tone (beat 1000 Hz) in the middle: equal ears
        tone(300.0);
        CHECK_EQ(cfg(R"({"centreHz":1000})").status(), FOXAPI_OK);
        Ears mid = ears(r);
        std::printf("  centre 1000 Hz, +300 Hz: left %.4f right %.4f\n", mid.left, mid.right);
        CHECK(std::fabs(mid.left - mid.right) < 0.15 * (mid.left + mid.right) / 2.0);
        CHECK_EQ(cfg(R"({"centreHz":700})").status(), FOXAPI_OK);
    }

    std::printf("C: a placement of its own wins; off puts the ears level again\n");
    {
        tone(300.0);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_STEREO_PLACE, -1.0, 1)).status(), FOXAPI_OK);   // pan hard left
        const Ears p = ears(r);
        std::printf("  stereo placement left: left %.4f right %.4f\n", p.left, p.right);
        CHECK(p.left > p.right * 3.0);   // (the surround would have put this tone on the right)
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_STEREO_PLACE, 0.0, 0)).status(), FOXAPI_OK);
        CHECK_EQ(cfg(R"({"enabled":false})").status(), FOXAPI_OK);
        const Ears off = ears(r);
        CHECK(std::fabs(off.left - off.right) < 1e-3 * off.left);
    }

    std::printf("D: the settings\n");
    {
        CHECK_EQ(cfg(R"({"enabled":false})").status(), FOXAPI_NO_CHANGE);
        CHECK_EQ(cfg("{}").status(), FOXAPI_NO_CHANGE);
        CHECK_EQ(cfg(R"({"spreadHz":600,"toneRatio":50,"holdMs":1000})").status(), FOXAPI_OK);
        CHECK_EQ(r.fact("cwsurround.1.spread"), std::string("600"));
        CHECK_EQ(r.fact("cwsurround.1.ratio"), std::string("50"));
        CHECK_EQ(r.fact("cwsurround.1.hold"), std::string("1000"));
        CHECK_EQ(r.fact("cwsurround.1.centre"), std::string("700"));   // left out: kept
        CHECK_EQ(cfg("{nope").status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(cfg("[1]").status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(cfg(R"({"enabled":"yes"})").status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(cfg(R"({"centreHz":"700"})").status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(cfg(R"({"colour":1})").status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(cfg(R"({"centreHz":100})").status(), FOXAPI_OUT_OF_RANGE);
        CHECK_EQ(cfg(R"({"centreHz":1600})").status(), FOXAPI_OUT_OF_RANGE);
        CHECK_EQ(cfg(R"({"spreadHz":10})").status(), FOXAPI_OUT_OF_RANGE);
        CHECK_EQ(cfg(R"({"toneRatio":1})").status(), FOXAPI_OUT_OF_RANGE);
        CHECK_EQ(cfg(R"({"holdMs":-1})").status(), FOXAPI_OUT_OF_RANGE);
        CHECK_EQ(r.send(sur(7)).status(), FOXAPI_OUT_OF_RANGE);
        CHECK_EQ(r.fact("cwsurround.1.spread"), std::string("600"));   // none of them changed anything
        CHECK_EQ(r.fact("cwsurround.1.centre"), std::string("700"));
        FoxCommand other = sur(0);
        other.target = 7;
        const std::string j = R"({"enabled":true})";
        CHECK_EQ(r.send(other, &j).status(), FOXAPI_NO_DEVICE);
    }

    std::printf("E: outside CW mode nothing is placed\n");
    {
        CHECK_EQ(cfg(R"({"enabled":true,"spreadHz":400,"toneRatio":100,"holdMs":400})").status(), FOXAPI_OK);
        CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_MODE, 0.0, FOXAPI_DEMOD_USB)).status(), FOXAPI_OK);
        tone(1500.0);
        const Ears e = ears(r);
        CHECK(e.left > 0.001 && std::fabs(e.left - e.right) < 1e-3 * e.left);
    }
    return check::finish("test_engine_api_wire_a_surround");
}
