// test_engine_api_wireb_mask.cpp - THE LIMIT MASK IN THE ENGINE (wire-b; FOXAPP_OP_MASK_LOAD 0x891E, FOXAPP_OP_MASK_CONFIG 0x891F; Engine::pumpWireB over core/spectrum_mask.hpp), on a real Engine
// with a synthetic tone (+15 kHz from the radio's centre, about -19 dB in the spectrum).
//
//   M1  a mask file the model refuses is BAD_ARGUMENT with its LINE and nothing loads; enabling with no mask is FAILED; a good file loads
//   M2  an upper limit at -60 dB: after raise_frames failing frames the alarm is up, the event says upper, margin about -41 dB, at the tone's frequency; the fact says pass false
//   M3  a mask the tone stays under: pass true, no alarm; a lower limit above the tone breaches again (kind lower)
//   M4  capture on breach: the alarm starts the radio's I/Q recording (a file appears in the recordings folder), it stops post_roll_s later, the fact says captured; a radio already recording is
//       not started a second time (the note says so)
//   M5  removing the mask: the fact goes; nothing is evaluated;  a REMOTE session is DENIED
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "wire1_rig.hpp"

namespace {

namespace fs = std::filesystem;
constexpr double kPi = 3.14159265358979323846;

class Tone final : public cascade::source::IqSource {
public:
    bool start() override { running_ = true; return true; }
    void stop() override { running_ = false; }
    bool running() const override { return running_; }
    bool selfPaced() const override { return false; }
    double sampleRateHz() const override { return 240000.0; }
    bool setSampleRateHz(double) override { return false; }
    double centerFrequencyHz() const override { return center_; }
    bool setCenterFrequencyHz(double hz) override { center_ = hz; return true; }
    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        for (std::size_t i = 0; i < n; ++i, ++k_) {
            const double a = 2.0 * kPi * 15000.0 * static_cast<double>(k_) / 240000.0;
            dst[i] = {static_cast<float>(0.3 * std::cos(a)), static_cast<float>(0.3 * std::sin(a))};
        }
        return n;
    }
    const char* name() const override { return "wireb-tone"; }
    const char* lastError() const override { return ""; }

private:
    bool running_ = false;
    double center_ = 100.0e6;
    std::uint64_t k_ = 0;
};

testing::Outcome sendText(Rig& r, std::uint32_t op, const std::string& text, testing::Session* who = nullptr) {
    FoxCommand c = testing::cmd(op, 0.0, 0, nullptr);
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

json maskFact(Rig& r) {
    std::string buf(1 << 16, '\0');
    const int32_t n = api()->get_setting(r.s->raw(), "fact.radio.1.mask", &buf[0], buf.size());
    if (n < 0) { return json(); }
    const json j = json::parse(buf.c_str(), nullptr, false);
    return j.is_discarded() ? json() : j;
}

bool alarmIs(Rig& r, bool want) {
    const json j = maskFact(r);
    return j.is_object() && j["alarm"].get<bool>() == want;
}

std::size_t wavFiles(const fs::path& dir) {
    std::size_t n = 0;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
        if (e.is_regular_file(ec) && e.path().extension() == ".wav" && fs::file_size(e.path(), ec) > 44) { ++n; }
    }
    return n;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_wireb_mask\n");
    engine_api_test::hermetic("wirebmask");
    Access::installHooks();
    const fs::path rec = fs::temp_directory_path() / ("wirebmask_rec_" + std::to_string(static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000000LL)));
    std::error_code ec;
    fs::create_directories(rec, ec);
    Rig r;
    Access::recordDir(r.eng, rec.string());
    Access::useSource(r.eng, std::make_unique<Tone>());
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_SQUELCH, -120.0)).status(), FOXAPI_OK);
    r.until([] { return false; }, 1500);

    std::printf("M1: loading\n");
    {
        const auto bad = sendText(r, FOXAPP_OP_MASK_LOAD, "foxmask 1\nupper 99800000 -60 99700000 -60\n");
        CHECK_EQ(bad.status(), FOXAPI_BAD_ARGUMENT);
        CHECK(std::string(bad.result.message).find("line 2") != std::string::npos);
        CHECK(maskFact(r).is_null());
        CHECK_EQ(sendText(r, FOXAPP_OP_MASK_CONFIG, "{\"enabled\":true}").status(), FOXAPI_FAILED);
        CHECK_EQ(sendText(r, FOXAPP_OP_MASK_LOAD, "").status(), FOXAPI_NO_CHANGE);
        const auto good = sendText(r, FOXAPP_OP_MASK_LOAD, "foxmask 1\nupper 99800000 -60 100200000 -60\n");
        CHECK_EQ(good.status(), FOXAPI_OK);
        CHECK_EQ(good.result.applied[0], 1.0);
        CHECK(!maskFact(r).is_null());
        CHECK(!maskFact(r)["enabled"].get<bool>());
        CHECK_EQ(sendText(r, FOXAPP_OP_MASK_CONFIG, "{\"raise_frames\":0}").status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(sendText(r, FOXAPP_OP_MASK_CONFIG, "{\"post_roll\":1}").status(), FOXAPI_BAD_ARGUMENT);
    }

    std::printf("M2: an upper limit under the tone\n");
    CHECK_EQ(sendText(r, FOXAPP_OP_MASK_CONFIG, "{\"enabled\":true,\"raise_frames\":3,\"clear_frames\":5}").status(), FOXAPI_OK);
    CHECK_EQ(sendText(r, FOXAPP_OP_MASK_CONFIG, "{\"enabled\":true,\"raise_frames\":3,\"clear_frames\":5}").status(), FOXAPI_NO_CHANGE);
    CHECK(r.until([&] { return alarmIs(r, true); }, 15000));
    {
        const json j = maskFact(r);
        CHECK(j["enabled"].get<bool>() && !j["pass"].get<bool>() && j["limited"].get<bool>());
        CHECK_EQ(j["events"].size(), static_cast<std::size_t>(1));
        const json& e = j["events"][0];
        std::printf("  event: %s margin %.1f dB at %.0f Hz, %zu frames\n", e["kind"].get<std::string>().c_str(), e["worst_margin_db"].get<double>(), e["worst_freq_hz"].get<double>(),
                    e["frames"].get<std::size_t>());
        CHECK_EQ(e["kind"].get<std::string>(), std::string("upper"));
        CHECK(e["worst_margin_db"].get<double>() < -30.0 && e["worst_margin_db"].get<double>() > -50.0);
        CHECK(std::fabs(e["worst_freq_hz"].get<double>() - 100015000.0) < 3000.0);
        CHECK(e["frames"].get<std::size_t>() >= 3);
    }

    std::printf("M3: a mask the tone stays under, then a lower limit above it\n");
    CHECK_EQ(sendText(r, FOXAPP_OP_MASK_LOAD, "foxmask 1\nupper 99800000 0 100200000 0\n").status(), FOXAPI_OK);
    r.until([] { return false; }, 1500);
    {
        const json j = maskFact(r);
        CHECK(j["pass"].get<bool>());
        CHECK(!j["alarm"].get<bool>());
        CHECK_EQ(j["events"].size(), static_cast<std::size_t>(0));   // loading a mask starts the history again
    }
    CHECK_EQ(sendText(r, FOXAPP_OP_MASK_LOAD, "foxmask 1\nlower 99800000 -5 100200000 -5\n").status(), FOXAPI_OK);
    CHECK(r.until([&] { return alarmIs(r, true); }, 15000));
    CHECK_EQ(maskFact(r)["events"][0]["kind"].get<std::string>(), std::string("lower"));

    std::printf("M4: capture on breach\n");
    CHECK_EQ(sendText(r, FOXAPP_OP_MASK_LOAD, "foxmask 1\nupper 99800000 0 100200000 0\n").status(), FOXAPI_OK);
    r.until([] { return false; }, 800);
    CHECK_EQ(sendText(r, FOXAPP_OP_MASK_CONFIG, "{\"capture\":true,\"post_roll_s\":1.0,\"min_interval_s\":30}").status(), FOXAPI_OK);
    CHECK_EQ(wavFiles(rec), static_cast<std::size_t>(0));
    CHECK_EQ(sendText(r, FOXAPP_OP_MASK_LOAD, "foxmask 1\nupper 99800000 -60 100200000 -60\n").status(), FOXAPI_OK);
    CHECK(r.until([&] { return wavFiles(rec) >= 1 || (maskFact(r).is_object() && maskFact(r)["captures"].get<std::size_t>() >= 1); }, 20000));
    CHECK(r.until([&] { const json j = maskFact(r); return j.is_object() && j["capture_note"].get<std::string>() == "captured"; }, 20000));
    {
        const json j = maskFact(r);
        std::printf("  captures %zu, note \"%s\", %zu file(s)\n", j["captures"].get<std::size_t>(), j["capture_note"].get<std::string>().c_str(), wavFiles(rec));
        CHECK_EQ(j["captures"].get<std::size_t>(), static_cast<std::size_t>(1));
        CHECK(wavFiles(rec) >= 1);
        r.until([] { return false; }, 2500);
        CHECK_EQ(maskFact(r)["captures"].get<std::size_t>(), static_cast<std::size_t>(1));   // the minimum interval: no second capture while the breach goes on
    }

    std::printf("M5: removing the mask, REMOTE\n");
    {
        testing::Session remote(api(), r.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS, "s3cret");
        CHECK(remote.ok());
        CHECK_EQ(sendText(r, FOXAPP_OP_MASK_LOAD, "", &remote).status(), FOXAPI_DENIED);
        CHECK_EQ(sendText(r, FOXAPP_OP_MASK_CONFIG, "{}", &remote).status(), FOXAPI_DENIED);
        CHECK(!maskFact(r).is_null());
    }
    CHECK_EQ(sendText(r, FOXAPP_OP_MASK_LOAD, "").status(), FOXAPI_OK);
    r.until([] { return false; }, 500);
    CHECK(maskFact(r).is_null());
    fs::remove_all(rec, ec);
    return check::finish("test_engine_api_wireb_mask");
}
