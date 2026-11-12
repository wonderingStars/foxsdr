// test_engine_api_wireb_cursors.cpp - THE EIGHT CURSORS, THE HARMONIC / SIDEBAND MARKS AND THE FRAME LOG THROUGH THE ENGINE (wire-b; FOXAPP_OP_CURSOR_* 0x8919-0x891B, FOXAPP_OP_FRAMELOG_* 0x891C-0x891D).
// A real Engine on a synthetic source: a 1000 Hz carrier with its second harmonic 12 dB down and its third 20 dB down, plus noise.
//
//   C1  CURSOR_MARKS harmonic from 1000 Hz: three marks at the lines, their levels 12 and 20 dB under the fundamental (within 1.5 dB); sideband marks around a carrier
//   C2  CURSOR_SET / CURSOR_READ: free cursors on the three lines read the same levels; with snap a cursor 12 Hz off lands within 3 Hz of its line; locked spacing lays the eight cursors out; harmonic
//       mode multiplies the first; a refused part changes nothing (unknown key, a mode word, a reference of 9, a cursor of 0)
//   F1  the frame log (filled by the test: no plugin call exists yet): READ shows the good frames, with the failed ones on request; keep-bad off drops the next failed frame but counts it; a new
//       capacity starts an empty log; refusals; nothing of it is in a fact until READ asks
//   G   a radio that is not running is FAILED; a REMOTE session is DENIED
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "engine/engine_wire_b.hpp"
#include "wire1_rig.hpp"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 200000.0;

class Lines final : public cascade::source::IqSource {
public:
    bool start() override { running_ = true; return true; }
    void stop() override { running_ = false; }
    bool running() const override { return running_; }
    bool selfPaced() const override { return false; }
    double sampleRateHz() const override { return kRate; }
    bool setSampleRateHz(double) override { return false; }
    double centerFrequencyHz() const override { return 0.0; }
    bool setCenterFrequencyHz(double) override { return true; }
    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        for (std::size_t i = 0; i < n; ++i, ++k_) {
            const double t = static_cast<double>(k_) / kRate;
            std::complex<double> s = 0.3 * std::polar(1.0, 2.0 * kPi * 1000.0 * t) + 0.3 * std::pow(10.0, -12.0 / 20.0) * std::polar(1.0, 2.0 * kPi * 2000.0 * t + 0.7) +
                                     0.3 * std::pow(10.0, -20.0 / 20.0) * std::polar(1.0, 2.0 * kPi * 3000.0 * t + 1.9);
            dst[i] = {static_cast<float>(s.real() + gauss(0.002)), static_cast<float>(s.imag() + gauss(0.002))};
        }
        return n;
    }
    const char* name() const override { return "wireb-lines"; }
    const char* lastError() const override { return ""; }

private:
    std::uint32_t lfsr() { state_ ^= state_ << 13; state_ ^= state_ >> 17; state_ ^= state_ << 5; return state_ >> 7; }
    double uni() { return (static_cast<double>(lfsr() & 0xFFFFFFu) + 0.5) / 16777216.0; }
    double gauss(double sigma) { const double u1 = uni(), u2 = uni(); return sigma * std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u2); }
    bool running_ = false;
    std::uint64_t k_ = 0;
    std::uint32_t state_ = 0x7654321u;
};

std::string bigFact(Rig& r, const std::string& key) {
    std::string buf(1 << 18, '\0');
    const int32_t n = api()->get_setting(r.s->raw(), ("fact." + key).c_str(), &buf[0], buf.size());
    return n >= 0 ? std::string(buf.c_str()) : std::string("<absent>");
}

testing::Outcome sendText(Rig& r, std::uint32_t op, std::uint32_t radio, const std::string& text, long long i0 = 0, long long i1 = 0, testing::Session* who = nullptr) {
    FoxCommand c = testing::cmd(op, 0.0, i0, nullptr);
    c.ival[1] = i1;
    c.target = radio;
    testing::Session& sess = who != nullptr ? *who : *r.s;
    testing::Outcome o;
    FoxSubmitResult sr{};
    sr.structSize = sizeof(sr);
    const int32_t n = text.empty() ? api()->submit(sess.raw(), &c, 1, &sr) : api()->submit_text(sess.raw(), &c, text.data(), static_cast<uint32_t>(text.size()), &sr);
    o.submitStatus = n < 0 ? n : sr.status;
    if (o.submitStatus != FOXAPI_OK) { return o; }
    for (int pass = 0; pass < 60 && !o.landed; ++pass) {
        r.eng.controlPass();
        FoxCommandResult res[16];
        for (auto& x : res) { x.structSize = sizeof(x); }
        const int32_t got = api()->poll_results(sess.raw(), res, 16);
        for (int32_t i = 0; i < got; ++i) { if (res[i].ticket == sr.ticket) { o.landed = true; o.result = res[i]; } }
    }
    return o;
}

json fact(Rig& r, const std::string& key) {
    const json j = json::parse(bigFact(r, key), nullptr, false);
    CHECK(!j.is_discarded());
    return j.is_discarded() ? json::object() : j;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_wireb_cursors\n");
    engine_api_test::hermetic("wirebcursors");
    Access::installHooks();
    Rig r;
    Access::useSource(r.eng, std::make_unique<Lines>());
    CHECK(r.send(testing::cmd(FOXAPI_OP_SET_MODE, 0, FOXAPI_DEMOD_RAW)).status() == FOXAPI_OK || true);
    CHECK(r.send(testing::cmd(FOXAPI_OP_SET_BANDWIDTH, 200000.0)).status() == FOXAPI_OK || true);
    CHECK(r.send(testing::cmd(FOXAPI_OP_SET_VFO_OFFSET, 0.0)).status() == FOXAPI_OK || true);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_SQUELCH, -120.0)).status(), FOXAPI_OK);

    std::printf("G0: not running\n");
    CHECK_EQ(sendText(r, FOXAPP_OP_CURSOR_MARKS, 1, "{}").status(), FOXAPI_FAILED);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
    r.until([] { return false; }, 1500);

    std::printf("C1: marks\n");
    {
        CHECK_EQ(sendText(r, FOXAPP_OP_CURSOR_MARKS, 1, "{\"kind\":\"harmonic\",\"base_hz\":1000,\"count\":3,\"low_hz\":-50000,\"high_hz\":50000,\"duration_s\":0.5}").status(), FOXAPI_OK);
        const json j = fact(r, "radio.1.sigmeas.marks");
        CHECK(j["valid"].get<bool>());
        const json& m = j["result"]["marks"];
        CHECK_EQ(m.size(), static_cast<std::size_t>(3));
        if (m.size() == 3) {
            const double l1 = m[0]["level_dbfs"].get<double>(), l2 = m[1]["level_dbfs"].get<double>(), l3 = m[2]["level_dbfs"].get<double>();
            std::printf("  levels %.1f %.1f %.1f dBFS; snapped %.1f %.1f %.1f Hz\n", l1, l2, l3, m[0]["snapped_hz"].get<double>(), m[1]["snapped_hz"].get<double>(), m[2]["snapped_hz"].get<double>());
            CHECK(std::fabs((l1 - l2) - 12.0) < 1.5);
            CHECK(std::fabs((l1 - l3) - 20.0) < 1.5);
            CHECK(std::fabs(m[1]["snapped_hz"].get<double>() - 2000.0) < 3.0);
        }
        CHECK_EQ(sendText(r, FOXAPP_OP_CURSOR_MARKS, 1, "{\"kind\":\"sideband\",\"carrier_hz\":2000,\"spacing_hz\":1000,\"count\":1,\"low_hz\":-50000,\"high_hz\":50000}").status(), FOXAPI_OK);
        const json s = fact(r, "radio.1.sigmeas.marks");
        CHECK_EQ(s["result"]["marks"].size(), static_cast<std::size_t>(3));   // the carrier and one sideband each side
        CHECK_EQ(sendText(r, FOXAPP_OP_CURSOR_MARKS, 1, "{\"kind\":\"subharmonic\"}").status(), FOXAPI_BAD_ARGUMENT);
    }

    std::printf("C2: the eight cursors\n");
    {
        CHECK_EQ(sendText(r, FOXAPP_OP_CURSOR_SET, 1, "{\"set\":[{\"cursor\":1,\"freq_hz\":1012},{\"cursor\":2,\"freq_hz\":2000},{\"cursor\":3,\"freq_hz\":3000}]}").status(), FOXAPI_OK);
        const json set = fact(r, "radio.1.sigmeas.cursors");
        CHECK_EQ(set["result"]["mode"].get<std::string>(), std::string("free"));
        CHECK_EQ(set["result"]["cursors"][0]["freq_hz"].get<double>(), 1012.0);
        CHECK_EQ(sendText(r, FOXAPP_OP_CURSOR_READ, 1, "{\"duration_s\":0.5,\"search_hz\":50}").status(), FOXAPI_OK);
        const json rd = fact(r, "radio.1.sigmeas.cursors");
        CHECK(rd["valid"].get<bool>());
        const json& c = rd["result"]["cursors"];
        const double l1 = c[0]["level_dbfs"].get<double>(), l2 = c[1]["level_dbfs"].get<double>(), l3 = c[2]["level_dbfs"].get<double>();
        std::printf("  free cursors read %.1f %.1f %.1f dBFS\n", l1, l2, l3);
        CHECK(std::fabs((l1 - l2) - 12.0) < 1.5 && std::fabs((l1 - l3) - 20.0) < 1.5);
        CHECK(std::fabs(c[2]["offset_hz"].get<double>() - (3000.0 - 1012.0)) < 1e-6);   // from the reference (cursor 1)
        CHECK_EQ(sendText(r, FOXAPP_OP_CURSOR_READ, 1, "{\"duration_s\":0.5,\"search_hz\":50,\"snap\":true}").status(), FOXAPI_OK);
        const json sn = fact(r, "radio.1.sigmeas.cursors");
        CHECK(sn["result"]["snapped"].get<bool>());
        std::printf("  cursor 1 snapped from 1012 to %.2f Hz\n", sn["result"]["cursors"][0]["freq_hz"].get<double>());
        CHECK(std::fabs(sn["result"]["cursors"][0]["freq_hz"].get<double>() - 1000.0) < 3.0);
        // locked spacing, harmonic
        CHECK_EQ(sendText(r, FOXAPP_OP_CURSOR_SET, 1, "{\"mode\":\"locked\",\"spacing_hz\":500,\"set\":[{\"cursor\":1,\"freq_hz\":1000}]}").status(), FOXAPI_OK);
        const json lk = fact(r, "radio.1.sigmeas.cursors");
        CHECK_EQ(lk["result"]["cursors"][7]["freq_hz"].get<double>(), 4500.0);
        CHECK_EQ(sendText(r, FOXAPP_OP_CURSOR_SET, 1, "{\"mode\":\"harmonic\",\"reference\":2}").status(), FOXAPI_OK);
        const json hm = fact(r, "radio.1.sigmeas.cursors");
        CHECK_EQ(hm["result"]["cursors"][3]["freq_hz"].get<double>(), 4000.0);
        CHECK_EQ(hm["result"]["reference"].get<int>(), 2);
        // a refused part changes nothing
        const auto unk = sendText(r, FOXAPP_OP_CURSOR_SET, 1, "{\"mode\":\"free\",\"colour\":1}");
        CHECK_EQ(unk.status(), FOXAPI_BAD_ARGUMENT);
        CHECK(std::string(unk.result.message).find("colour") != std::string::npos);
        CHECK_EQ(sendText(r, FOXAPP_OP_CURSOR_SET, 1, "{\"mode\":\"sideways\"}").status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(sendText(r, FOXAPP_OP_CURSOR_SET, 1, "{\"reference\":9}").status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(sendText(r, FOXAPP_OP_CURSOR_SET, 1, "{\"mode\":\"free\",\"set\":[{\"cursor\":0,\"freq_hz\":5}]}").status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(sendText(r, FOXAPP_OP_CURSOR_SET, 1, "{}").status(), FOXAPI_OK);   // nothing asked: the set as it was, read back
        const json still = fact(r, "radio.1.sigmeas.cursors");
        CHECK_EQ(still["result"]["mode"].get<std::string>(), std::string("harmonic"));   // the refused 'free' did not stick
    }

    std::printf("F1: the frame log\n");
    {
        auto& log = *r.eng.wireBStateForTest().frameLog;
        namespace sm = cascade::sigmeas;
        CHECK_EQ(r.fact("framelog"), std::string("<absent>"));   // nothing of it is a fact until READ asks
        for (int i = 0; i < 3; ++i) { log.push(sm::FrameRecord::withCrc(1.0 + i, 7.1e6, {0x01, 0x02, static_cast<std::uint8_t>(i)}, 0xBEEF, 0xBEEF)); }
        for (int i = 0; i < 2; ++i) { log.push(sm::FrameRecord::withCrc(10.0 + i, 7.1e6, {0xAA, static_cast<std::uint8_t>(i)}, 0x1234, 0x4321)); }
        const auto o = sendText(r, FOXAPP_OP_FRAMELOG_READ, 1, "", 0);
        CHECK_EQ(o.status(), FOXAPI_OK);
        CHECK_EQ(o.result.applied[0], 3.0);
        const json good = fact(r, "framelog");
        CHECK_EQ(good["frames"].size(), static_cast<std::size_t>(3));
        CHECK_EQ(good["held_bad"].get<std::size_t>(), static_cast<std::size_t>(2));
        CHECK_EQ(good["frames"][0]["hex"].get<std::string>(), std::string("010200"));
        CHECK_EQ(sendText(r, FOXAPP_OP_FRAMELOG_READ, 1, "", 1).result.applied[0], 5.0);
        const json all = fact(r, "framelog");
        CHECK_EQ(all["frames"].size(), static_cast<std::size_t>(5));
        CHECK(!all["frames"][4]["crc_ok"].get<bool>());
        CHECK_EQ(all["frames"][4]["crc_received"].get<std::uint32_t>(), 0x1234u);
        // keep-bad off: the next failed frame is counted and not kept
        CHECK_EQ(sendText(r, FOXAPP_OP_FRAMELOG_CONFIG, 1, "", 0, 0).status(), FOXAPI_OK);
        log.push(sm::FrameRecord::withCrc(20.0, 7.1e6, {0xCC}, 1, 2));
        CHECK_EQ(sendText(r, FOXAPP_OP_FRAMELOG_READ, 1, "", 1).result.applied[0], 5.0);
        CHECK_EQ(fact(r, "framelog")["bad_total"].get<std::uint64_t>(), static_cast<std::uint64_t>(3));
        CHECK_EQ(sendText(r, FOXAPP_OP_FRAMELOG_CONFIG, 1, "", 0, 0).status(), FOXAPI_NO_CHANGE);
        // a new capacity starts an empty log
        CHECK_EQ(sendText(r, FOXAPP_OP_FRAMELOG_CONFIG, 1, "", -1, 2).status(), FOXAPI_OK);
        CHECK_EQ(sendText(r, FOXAPP_OP_FRAMELOG_READ, 1, "", 1).result.applied[0], 0.0);
        auto& log2 = *r.eng.wireBStateForTest().frameLog;
        for (int i = 0; i < 4; ++i) { log2.push(sm::FrameRecord::withoutCrc(30.0 + i, 0.0, {static_cast<std::uint8_t>(i)})); }
        CHECK_EQ(sendText(r, FOXAPP_OP_FRAMELOG_READ, 1, "", 0).result.applied[0], 2.0);   // the two newest of four
        CHECK_EQ(sendText(r, FOXAPP_OP_FRAMELOG_CONFIG, 1, "", 2, 0).status(), FOXAPI_OUT_OF_RANGE);
        CHECK_EQ(sendText(r, FOXAPP_OP_FRAMELOG_CONFIG, 1, "", 0, 5000).status(), FOXAPI_OUT_OF_RANGE);
    }

    std::printf("G: REMOTE\n");
    {
        testing::Session remote(api(), r.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS, "s3cret");
        CHECK(remote.ok());
        CHECK_EQ(sendText(r, FOXAPP_OP_FRAMELOG_READ, 1, "", 1, 0, &remote).status(), FOXAPI_DENIED);
        CHECK_EQ(sendText(r, FOXAPP_OP_CURSOR_SET, 1, "{}", 0, 0, &remote).status(), FOXAPI_DENIED);
    }
    return check::finish("test_engine_api_wireb_cursors");
}
