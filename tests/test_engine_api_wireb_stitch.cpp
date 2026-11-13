// test_engine_api_wireb_stitch.cpp - THE STITCHED SWEEP OF AN ORDINARY RADIO THROUGH THE ENGINE (wire-b second pass, item 10; SWEEP_OPEN kind "radio", 0x8920; engine_wire_b_stitch.cpp over
// engine/wire_b_stitch.{hpp,cpp} and core/sweep_stitch.hpp), on a real Engine with a synthetic source: four tones at ABSOLUTE frequencies (100.05, 100.20, 100.35, 100.50 MHz, amplitudes
// 0.3, 0.1, 0.3, 0.05) plus noise, seen through whatever centre the radio is tuned to, and a receiver gain that DROPS 8 dB above a 100.25 MHz centre (so the seams need level alignment).
// No radio is opened: the source is a stand-in.
//
//   W1  refusals: no radio running, a radio that is not open, stop under start, a step or trim out of range, a range needing over 512 centres, keys of another kind, REMOTE denied
//   W2  one pass over 100.0-100.6 MHz: the host is open while it runs and closed when it ends, ONE line with the four tones within a few bins of their true frequencies, their levels aligned across
//       the 8 dB step (tone 3 within 3 dB of tone 1 although it was seen 8 dB lower), the seams counted, and the radio's own centre put back
//   W3  passes 0 repeats until closed: a second open is BUSY, SWEEP_CLOSE ends it, the centre is put back, closing again is NO_CHANGE
//   W4  a radio that does not follow a retune ends the run with a sentence (and the centre is the original)
//
// UNVERIFIED on a real radio.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "engine/engine_wire_b.hpp"
#include "wire1_rig.hpp"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 240000.0;
constexpr double kHome = 100.0e6;   // the radio's own centre before and after every run

class Comb final : public cascade::source::IqSource {
public:
    std::atomic<double> center{kHome};
    std::atomic<bool> follow{true};   // false: SET_CENTRE is accepted and ignored (a radio that cannot retune)
    std::atomic<int> retunes{0};
    bool start() override { running_ = true; return true; }
    void stop() override { running_ = false; }
    bool running() const override { return running_; }
    bool selfPaced() const override { return false; }
    double sampleRateHz() const override { return kRate; }
    bool setSampleRateHz(double) override { return false; }
    double centerFrequencyHz() const override { return center.load(); }
    bool setCenterFrequencyHz(double hz) override {
        ++retunes;
        if (follow.load()) { center.store(hz); }
        return true;
    }
    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        static const double f[4] = {100.05e6, 100.20e6, 100.35e6, 100.50e6};
        static const double a[4] = {0.3, 0.1, 0.3, 0.05};
        const double c = center.load();
        const double gain = c > 100.25e6 ? 0.4 : 1.0;   // -8 dB above 100.25 MHz
        for (std::size_t i = 0; i < n; ++i, ++k_) {
            std::complex<double> s{0.0, 0.0};
            for (int t = 0; t < 4; ++t) {
                const double off = f[t] - c;
                if (std::fabs(off) < 0.45 * kRate) { s += a[t] * std::polar(1.0, 2.0 * kPi * off * static_cast<double>(k_) / kRate); }
            }
            lcg_ = lcg_ * 6364136223846793005ULL + 1442695040888963407ULL;
            const double nr = (static_cast<double>((lcg_ >> 33) & 0xFFFF) / 65535.0 - 0.5) * 0.01;
            lcg_ = lcg_ * 6364136223846793005ULL + 1442695040888963407ULL;
            const double ni = (static_cast<double>((lcg_ >> 33) & 0xFFFF) / 65535.0 - 0.5) * 0.01;
            dst[i] = {static_cast<float>(gain * (s.real() + nr)), static_cast<float>(gain * (s.imag() + ni))};
        }
        return n;
    }
    const char* name() const override { return "wireb-comb"; }
    const char* lastError() const override { return ""; }

private:
    bool running_ = false;
    std::uint64_t k_ = 0;
    std::uint64_t lcg_ = 12345;
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

std::string bigFact(Rig& r, const std::string& key) {
    std::string buf(1 << 18, '\0');
    const int32_t n = api()->get_setting(r.s->raw(), ("fact." + key).c_str(), &buf[0], buf.size());
    return n >= 0 ? std::string(buf.c_str()) : std::string("<absent>");
}

json factOf(Rig& r, const std::string& key) {
    const json j = json::parse(bigFact(r, key), nullptr, false);
    return j.is_discarded() ? json() : j;
}

bool isOpen(Rig& r) {
    const json j = factOf(r, "sweep.state");
    return j.is_object() && j["open"].get<bool>();
}

// The strongest cell within +-`halfHz` of `hz`: its frequency and level.
bool peakNear(const json& line, double hz, double halfHz, double& atHz, double& db) {
    const double a = line["start_hz"].get<double>(), b = line["stop_hz"].get<double>();
    const std::size_t n = line["db"].size();
    bool any = false;
    for (std::size_t i = 0; i < n; ++i) {
        const double f = a + (b - a) * static_cast<double>(i) / static_cast<double>(n - 1);
        if (std::fabs(f - hz) > halfHz || !line["db"][i].is_number()) { continue; }
        const double v = line["db"][i].get<double>();
        if (!any || v > db) { any = true; db = v; atHz = f; }
    }
    return any;
}

const char* kRun = "{\"kind\":\"radio\",\"start_mhz\":100.0,\"stop_mhz\":100.6,\"settle_ms\":100,\"frames\":2}";

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_wireb_stitch\n");
    engine_api_test::hermetic("wirebstitch");
    Access::installHooks();
    Rig r;
    // the engine must never reach a COM port from this test
    cascade::engine::WireBState& w = r.eng.wireBStateForTest();
    w.rfeFactory = [](const std::string&, int, std::string& why) -> std::unique_ptr<cascade::source::tinysa::SerialLink> { why = "no port in this test"; return nullptr; };

    std::printf("W1: refusals\n");
    {
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_OPEN, kRun).status(), FOXAPI_FAILED);   // the radio is not running
        const auto n = sendText(r, FOXAPP_OP_SWEEP_OPEN, kRun);
        CHECK(std::string(n.result.message).find("start it first") != std::string::npos);
    }
    auto comb = std::make_unique<Comb>();
    Comb* src = comb.get();
    Access::useSource(r.eng, std::move(comb));
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_SQUELCH, -120.0)).status(), FOXAPI_OK);
    r.until([] { return false; }, 1500);
    {
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_OPEN, "{\"kind\":\"radio\",\"radio\":2,\"start_mhz\":100,\"stop_mhz\":100.6}").status(), FOXAPI_FAILED);   // radio 2 is not open
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_OPEN, "{\"kind\":\"radio\",\"radio\":9,\"start_mhz\":100,\"stop_mhz\":100.6}").status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_OPEN, "{\"kind\":\"radio\",\"start_mhz\":100.6,\"stop_mhz\":100}").status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_OPEN, "{\"kind\":\"radio\",\"start_mhz\":100,\"stop_mhz\":100.6,\"step_fraction\":0.9}").status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_OPEN, "{\"kind\":\"radio\",\"start_mhz\":100,\"stop_mhz\":100.6,\"trim\":0.5}").status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_OPEN, "{\"kind\":\"radio\",\"start_mhz\":100,\"stop_mhz\":100.6,\"frames\":0}").status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_OPEN, "{\"kind\":\"radio\",\"start_mhz\":100,\"stop_mhz\":100.6,\"frames\":65}").status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_OPEN, "{\"kind\":\"radio\",\"start_mhz\":100,\"stop_mhz\":100.6,\"timeout_ms\":100}").status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_OPEN, "{\"kind\":\"radio\",\"start_mhz\":100,\"stop_mhz\":100.6,\"settle_ms\":6000}").status(), FOXAPI_BAD_ARGUMENT);
        const auto big = sendText(r, FOXAPP_OP_SWEEP_OPEN, "{\"kind\":\"radio\",\"start_mhz\":1,\"stop_mhz\":6000}");
        CHECK_EQ(big.status(), FOXAPI_BAD_ARGUMENT);
        CHECK(std::string(big.result.message).find("512") != std::string::npos);
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_OPEN, "{\"kind\":\"radio\",\"port\":\"COM3\",\"start_mhz\":100,\"stop_mhz\":100.6}").status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_OPEN, "{\"kind\":\"radio\",\"start_mhz\":100,\"stop_mhz\":100.6,\"points\":100}").status(), FOXAPI_BAD_ARGUMENT);
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_OPEN, "{\"kind\":\"tinysa\",\"port\":\"COM3\",\"start_mhz\":100,\"stop_mhz\":101,\"frames\":2}").status(), FOXAPI_BAD_ARGUMENT);   // a radio key on another kind
        CHECK(!isOpen(r));
        CHECK_EQ(src->retunes.load(), 0);   // nothing above touched the radio
        testing::Session remote(api(), r.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS, "s3cret");
        CHECK(remote.ok());
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_OPEN, kRun, &remote).status(), FOXAPI_DENIED);
        CHECK_EQ(src->retunes.load(), 0);
    }

    std::printf("W2: one pass over 100.0-100.6 MHz\n");
    CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_OPEN, kRun).status(), FOXAPI_OK);
    CHECK(isOpen(r));
    CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_OPEN, kRun).status(), FOXAPI_BUSY);
    CHECK(r.until([&] { return !isOpen(r); }, 90000));
    {
        const json st = factOf(r, "sweep.state");
        const json sc = factOf(r, "sweep.stitch");
        std::printf("  sweep.state %s\n  sweep.stitch %s\n", bigFact(r, "sweep.state").c_str(), bigFact(r, "sweep.stitch").c_str());
        CHECK(st.is_object() && sc.is_object());
        CHECK_EQ(st["kind"].get<std::string>(), std::string("radio"));
        CHECK_EQ(st["sweeps"].get<std::uint64_t>(), static_cast<std::uint64_t>(1));
        CHECK(st["last_error"].get<std::string>().empty());
        CHECK_EQ(sc["centres"].get<std::size_t>(), static_cast<std::size_t>(4));
        CHECK_EQ(sc["passes_done"].get<std::uint64_t>(), static_cast<std::uint64_t>(1));
        CHECK_EQ(sc["seams"].get<std::size_t>(), static_cast<std::size_t>(3));
        CHECK(sc["seams_aligned"].get<std::size_t>() >= 2);
        CHECK(!sc["active"].get<bool>());
        CHECK(st["heat_rows"].get<std::size_t>() >= 1);
        CHECK_EQ(src->retunes.load(), 4 + 1);   // four centres and the way home
        CHECK(std::fabs(src->center.load() - kHome) < 1.0);   // the radio's own centre is back
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_READ, "{\"max_points\":4096}").status(), FOXAPI_OK);
        const json line = factOf(r, "sweep.read");
        CHECK(line.is_object());
        if (line.is_object()) {
            std::printf("  line: %.4f - %.4f MHz, %zu points\n", line["start_hz"].get<double>() / 1e6, line["stop_hz"].get<double>() / 1e6, line["points"].get<std::size_t>());
            CHECK(line["start_hz"].get<double>() <= 100.01e6 && line["stop_hz"].get<double>() >= 100.59e6);
            const double want[4] = {100.05e6, 100.20e6, 100.35e6, 100.50e6};
            double hz[4] = {0, 0, 0, 0}, db[4] = {0, 0, 0, 0};
            for (int t = 0; t < 4; ++t) {
                CHECK(peakNear(line, want[t], 8000.0, hz[t], db[t]));
                std::printf("  tone %d: expected %.3f MHz, at %.4f MHz, %.1f dB\n", t + 1, want[t] / 1e6, hz[t] / 1e6, db[t]);
                CHECK(std::fabs(hz[t] - want[t]) < 1500.0);   // a few bins (a bin is about 234 Hz)
            }
            // truth: tone 1 and 3 are equal, tone 2 is 9.5 dB under them, tone 4 15.6 dB under; tones 3 and 4 were SEEN 8 dB lower
            CHECK(std::fabs(db[2] - db[0]) < 3.0);
            CHECK(std::fabs((db[0] - db[1]) - 9.54) < 3.5);
            CHECK(std::fabs((db[0] - db[3]) - 15.6) < 4.0);
        }
    }

    std::printf("W3: repeat until closed\n");
    {
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_OPEN, "{\"kind\":\"radio\",\"start_mhz\":100.0,\"stop_mhz\":100.6,\"settle_ms\":50,\"frames\":1,\"passes\":0}").status(), FOXAPI_OK);
        CHECK(r.until([&] { const json s = factOf(r, "sweep.stitch"); return s.is_object() && s["active"].get<bool>() && s["passes_done"].get<std::uint64_t>() >= 2; }, 90000));
        CHECK(isOpen(r));
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_OPEN, kRun).status(), FOXAPI_BUSY);
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_CLOSE, "").status(), FOXAPI_OK);
        CHECK(!isOpen(r));
        CHECK(!factOf(r, "sweep.stitch")["active"].get<bool>());
        CHECK(std::fabs(src->center.load() - kHome) < 1.0);
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_CLOSE, "").status(), FOXAPI_NO_CHANGE);
        const int after = src->retunes.load();
        r.until([] { return false; }, 300);
        CHECK_EQ(src->retunes.load(), after);   // nothing keeps retuning after the close
    }

    std::printf("W4: a radio that does not follow\n");
    {
        src->follow.store(false);
        CHECK_EQ(sendText(r, FOXAPP_OP_SWEEP_OPEN, "{\"kind\":\"radio\",\"start_mhz\":100.0,\"stop_mhz\":100.6,\"settle_ms\":20,\"timeout_ms\":500}").status(), FOXAPI_OK);
        CHECK(r.until([&] { return !isOpen(r); }, 30000));
        const json st = factOf(r, "sweep.state");
        std::printf("  last error: %s\n", st["last_error"].get<std::string>().c_str());
        CHECK(st["last_error"].get<std::string>().find("did not retune") != std::string::npos);
        CHECK_EQ(st["sweeps"].get<std::uint64_t>(), static_cast<std::uint64_t>(0));
        CHECK(std::fabs(src->center.load() - kHome) < 1.0);
        src->follow.store(true);
    }
    return check::finish("test_engine_api_wireb_stitch");
}
