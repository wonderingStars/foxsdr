// test_engine_api_wireb_gpufft.cpp - THE GPU SPECTRUM IN THE PIPELINE (wire-b; FOXAPP_OP_GPU_FFT_MODE 0x8918; core/pipeline_gpufft.cpp over dsp/gpu_fft.hpp), through a real Engine on a tone source.
//
//   G1  OFF by default: every radio reports mode 0, not engaged, the CPU backend; Auto at the legacy 1024-point size stays on the CPU estimator (nothing is engaged, no device is made)
//   G2  mode GPU: the engine publishes frames from the GPU engine (engaged, counters moving); the tone is at the same bin as on the CPU path and its level within 1.5 dB of the CPU
//       path's; frames keep arriving (at least half the CPU path's rate)
//   G3  back to CPU: not engaged, frames go on, the average re-primes (the same level again)
//   G4  refusals: a mode out of range is OUT_OF_RANGE, the same mode is NO_CHANGE, a REMOTE session is DENIED; a radio added after the mode was set starts on CPU
//
// On a machine with no usable Direct3D adapter the engine falls back to its own CPU computation (engaged, backend "cpu"): the checks that name the GPU accept that and print it. UNVERIFIED on
// every GPU but the one this was run on.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "wire1_rig.hpp"

namespace {

constexpr double kPi = 3.14159265358979323846;

class Tone final : public cascade::source::IqSource {
public:
    bool start() override { running_ = true; return true; }
    void stop() override { running_ = false; }
    bool running() const override { return running_; }
    bool selfPaced() const override { return false; }
    double sampleRateHz() const override { return 240000.0; }
    bool setSampleRateHz(double) override { return false; }
    double centerFrequencyHz() const override { return 0.0; }
    bool setCenterFrequencyHz(double) override { return true; }
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
    std::uint64_t k_ = 0;
};

json gpuFact(Rig& r, int radio = 1) {
    const json j = json::parse(r.fact("radio." + std::to_string(radio) + ".gpufft"), nullptr, false);
    CHECK(!j.is_discarded());
    return j.is_discarded() ? json::object() : j;
}

// The tone bin's level and the number of distinct frames over `ms`.
struct Reading {
    float peakDb = -999.0f;
    std::size_t peakBin = 0;
    int frames = 0;
};
Reading read(Rig& r, int ms) {
    Reading out;
    std::uint64_t lastSeq = 0;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        r.eng.controlPass();
        const Rig::Spectrum sp = r.spectrum(1);
        if (sp.status == FOXAPI_OK && sp.info.seq != lastSeq) {
            lastSeq = sp.info.seq;
            ++out.frames;
            const std::size_t pk = peakBin(sp);
            out.peakBin = pk;
            out.peakDb = sp.bins[pk];
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    return out;
}

FoxCommand mode(long long m) { return testing::cmd(FOXAPP_OP_GPU_FFT_MODE, 0.0, m, nullptr); }

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_wireb_gpufft\n");
    engine_api_test::hermetic("wirebgpufft");
    Access::installHooks();
    Rig r;
    Access::useSource(r.eng, std::make_unique<Tone>());
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_RUN, 0, 1)).status(), FOXAPI_OK);
    CHECK_EQ(r.send(testing::cmd(FOXAPI_OP_SET_SQUELCH, -120.0)).status(), FOXAPI_OK);
    r.until([] { return false; }, 1500);

    std::printf("G1: off by default\n");
    {
        const json j = gpuFact(r);
        CHECK_EQ(j["mode"].get<int>(), 0);
        CHECK(!j["engaged"].get<bool>());
        CHECK_EQ(j["backend"].get<std::string>(), std::string("cpu"));
        CHECK_EQ(j["submitted"].get<std::uint64_t>(), static_cast<std::uint64_t>(0));
    }
    const Reading cpu = read(r, 2500);
    std::printf("  CPU path: %d frames, tone bin %zu at %.2f dB\n", cpu.frames, cpu.peakBin, cpu.peakDb);
    CHECK(cpu.frames >= 20);
    CHECK_EQ(r.send(mode(1)).status(), FOXAPI_OK);   // Auto
    r.until([] { return false; }, 1500);
    {
        const json j = gpuFact(r);
        CHECK_EQ(j["mode"].get<int>(), 1);
        CHECK(!j["engaged"].get<bool>());   // 1024 points is below Auto's 65 536: the CPU estimator keeps running
        CHECK_EQ(j["submitted"].get<std::uint64_t>(), static_cast<std::uint64_t>(0));
        CHECK_EQ(r.fact("gpu_fft.mode"), std::string("1"));
    }

    std::printf("G2: mode GPU\n");
    CHECK_EQ(r.send(mode(2)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return gpuFact(r)["engaged"].get<bool>(); }, 30000));
    const Reading gpu = read(r, 2500);
    {
        const json j = gpuFact(r);
        std::printf("  GPU path: %d frames, tone bin %zu at %.2f dB; backend %s, submitted %llu, completed gpu %llu cpu %llu, dropped %llu, error \"%s\"\n", gpu.frames, gpu.peakBin, gpu.peakDb,
                    j["backend"].get<std::string>().c_str(), static_cast<unsigned long long>(j["submitted"].get<std::uint64_t>()),
                    static_cast<unsigned long long>(j["completed_gpu"].get<std::uint64_t>()), static_cast<unsigned long long>(j["completed_cpu"].get<std::uint64_t>()),
                    static_cast<unsigned long long>(j["dropped_frames"].get<std::uint64_t>()), j["error"].get<std::string>().c_str());
        CHECK(j["engaged"].get<bool>());
        CHECK(j["submitted"].get<std::uint64_t>() > 10);
        CHECK(j["completed_gpu"].get<std::uint64_t>() + j["completed_cpu"].get<std::uint64_t>() > 10);
    }
    CHECK(gpu.frames * 2 >= cpu.frames);                        // frames keep arriving
    CHECK_EQ(gpu.peakBin, cpu.peakBin);                         // the tone is where it was
    CHECK(std::fabs(gpu.peakDb - cpu.peakDb) < 1.5f);           // at the same level

    std::printf("G3: back to CPU\n");
    CHECK_EQ(r.send(mode(0)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return !gpuFact(r)["engaged"].get<bool>(); }, 15000));
    const Reading again = read(r, 2500);
    std::printf("  CPU again: %d frames, bin %zu at %.2f dB\n", again.frames, again.peakBin, again.peakDb);
    CHECK(again.frames >= 20);
    CHECK_EQ(again.peakBin, cpu.peakBin);
    CHECK(std::fabs(again.peakDb - cpu.peakDb) < 1.0f);

    std::printf("G4: refusals\n");
    CHECK_EQ(r.send(mode(3)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(mode(-1)).status(), FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.send(mode(0)).status(), FOXAPI_NO_CHANGE);
    {
        testing::Session remote(api(), r.eng.apiHandle(), FOXAPI_SESSION_REMOTE, FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS, "s3cret");
        CHECK(remote.ok());
        FoxCommand c = mode(2);
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
        CHECK_EQ(gpuFact(r)["mode"].get<int>(), 0);
    }
    // a radio added after the mode was set starts on CPU (the mode applies to the radios open when it is asked)
    CHECK_EQ(r.send(mode(2)).status(), FOXAPI_OK);
    CHECK_EQ(r.add(2, "generator", false).status(), FOXAPI_OK);
    CHECK_EQ(gpuFact(r, 2)["mode"].get<int>(), 0);
    CHECK_EQ(r.send(mode(0)).status(), FOXAPI_OK);
    return check::finish("test_engine_api_wireb_gpufft");
}
