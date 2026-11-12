// test_engine_api_spectrum.cpp - THE SPECTRUM'S SIZE, WINDOW AND FRAME RATE THROUGH THE ENGINE (engine serial lane A, rows S1 S2 S3): SET_SPECTRUM 0x011C, per radio.
//   A  default (1024 / Blackman-Harris / automatic), each radio its own, refusals at the API and the engine change nothing, the pipeline follows, the radio's state names it;
//   B  saved per radio and restored in an in-process restart, junk in a file lands on a legal value;  C  the frames the ring serves have the chosen number of bins.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "wire1_rig.hpp"

namespace {

FoxCommand spec(std::uint32_t target, std::int64_t fft, std::int64_t window, double fps) {
    FoxCommand c = testing::cmd(FOXAPI_OP_SET_SPECTRUM, fps, 0, nullptr);
    c.target = target;
    c.ival[0] = fft;
    c.ival[1] = window;
    c.num[0] = fps;
    return c;
}

void testA() {
    std::printf("A: default, per radio, refusals\n");
    Rig r;
    CHECK_EQ(r.add(2, "").status(), FOXAPI_OK);
    r.pump(10);
    CHECK_EQ(r.radio(1).spectrumFftSize, 1024u);
    CHECK_EQ(r.radio(1).spectrumWindow, 3u);
    CHECK_EQ(r.radio(1).spectrumFrameRateHz, 0.0);
    CHECK_EQ(r.send(spec(2, 8192, 1, 20.0)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(2).spectrumFftSize == 8192u; }, 5000));
    CHECK_EQ(r.radio(2).spectrumWindow, 1u);
    CHECK_EQ(r.radio(2).spectrumFrameRateHz, 20.0);
    CHECK_EQ(r.radio(1).spectrumFftSize, 1024u);
    CHECK_EQ(Access::pipeline(r.eng, 1).spectrumFftSize(), static_cast<std::size_t>(8192));
    CHECK_EQ(Access::pipeline(r.eng, 1).spectrumWindow(), 1);
    CHECK_EQ(Access::pipeline(r.eng, 0).spectrumFftSize(), static_cast<std::size_t>(1024));
    CHECK_EQ(r.send(spec(1, 131072, 4, 0.0)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(1).spectrumFftSize == 131072u; }, 5000));
    for (const std::int64_t bad : {0, 512, 1000, 3072, 262144, -1024}) { CHECK_EQ(r.send(spec(1, bad, 3, 0.0)).status(), FOXAPI_OUT_OF_RANGE); }
    for (const std::int64_t bad : {-1, 5, 99}) { CHECK_EQ(r.send(spec(1, 4096, bad, 0.0)).status(), FOXAPI_OUT_OF_RANGE); }
    for (const double bad : {0.5, 121.0, -3.0}) { CHECK_EQ(r.send(spec(1, 4096, 3, bad)).status(), FOXAPI_OUT_OF_RANGE); }
    CHECK_EQ(r.send(spec(1, 4096, 3, std::nan(""))).status(), FOXAPI_BAD_ARGUMENT);
    CHECK_EQ(r.send(spec(9, 4096, 3, 0.0)).status(), FOXAPI_NO_DEVICE);
    const FoxCommandResult res = r.eng.applyCommand(spec(1, 4097, 3, 0.0), std::string());
    CHECK_EQ(res.status, FOXAPI_OUT_OF_RANGE);
    CHECK_EQ(r.radio(1).spectrumFftSize, 131072u);
    CHECK_EQ(Access::pipeline(r.eng, 0).spectrumFftSize(), static_cast<std::size_t>(131072));
    // back to the legacy choice
    CHECK_EQ(r.send(spec(1, 1024, 3, 0.0)).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(1).spectrumFftSize == 1024u && Access::pipeline(r.eng, 0).spectrumFftSize() == 1024u; }, 5000));
}

void testB() {
    std::printf("B: saved and restored\n");
    AppConfig file;
    {
        Rig a;
        CHECK_EQ(a.add(2, "").status(), FOXAPI_OK);
        CHECK_EQ(a.send(spec(1, 16384, 2, 0.0)).status(), FOXAPI_OK);
        CHECK_EQ(a.send(spec(2, 2048, 4, 45.0)).status(), FOXAPI_OK);
        a.pump(20);
        file = throughTheFile(a.eng);
        CHECK_EQ(file.spectrumFftSize, 16384);
        CHECK_EQ(file.spectrumWindow, 2);
        CHECK_EQ(file.radios.size(), static_cast<std::size_t>(1));
        CHECK_EQ(file.radios[0].spectrumFftSize, 2048);
        CHECK_EQ(file.radios[0].spectrumWindow, 4);
        CHECK_EQ(file.radios[0].spectrumFrameRateHz, 45.0);
    }
    {
        Rig b(&file);
        CHECK_EQ(b.radio(1).spectrumFftSize, 16384u);
        CHECK_EQ(b.radio(2).spectrumFftSize, 2048u);
        CHECK_EQ(b.radio(2).spectrumFrameRateHz, 45.0);
        CHECK(b.until([&] { return Access::pipeline(b.eng, 0).spectrumFftSize() == 16384u && Access::pipeline(b.eng, 1).spectrumWindow() == 4; }, 5000));
    }
    // junk in a file lands on a legal value
    AppConfig back;
    std::string error;
    CHECK(ConfigStore::parse("{\"spectrumFftSize\":3000,\"spectrumWindow\":9,\"spectrumFrameRateHz\":500}", "test", back, error));
    CHECK_EQ(back.spectrumFftSize, 1024);
    CHECK_EQ(back.spectrumWindow, 3);
    CHECK_EQ(back.spectrumFrameRateHz, 120.0);
    CHECK(ConfigStore::parse("{\"spectrumFftSize\":262144,\"spectrumFrameRateHz\":-4}", "test", back, error));
    CHECK_EQ(back.spectrumFftSize, 1024);
    CHECK_EQ(back.spectrumFrameRateHz, 0.0);
    CHECK(ConfigStore::parse("{\"spectrumFftSize\":65536,\"spectrumWindow\":0,\"spectrumFrameRateHz\":30}", "test", back, error));
    CHECK_EQ(back.spectrumFftSize, 65536);
    CHECK_EQ(back.spectrumWindow, 0);
}

struct BigFrame {
    std::int32_t status = FOXAPI_NO_CHANGE;
    FoxSpectrumInfo info{};
    std::vector<float> bins;
};
// The radio's newest frame at full width (the ring is asked for 131,072 bins; it serves the engine's own count).
BigFrame bigFrame(Rig& r, std::uint32_t radio) {
    BigFrame f;
    f.bins.assign(131072, 0.0f);
    f.info.structSize = sizeof(f.info);
    f.status = api()->read_radio_spectrum(r.s->raw(), radio, 0, &f.info, f.bins.data(), static_cast<std::uint32_t>(f.bins.size()));
    if (f.status == FOXAPI_OK) { f.bins.resize(f.info.copied); }
    return f;
}
double medianOf(std::vector<float> v) {
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
    return v[v.size() / 2];
}

void testC() {
    std::printf("C: the ring serves the chosen number of bins, the tone stays where it was and so does its level\n");
    Rig r;
    CHECK_EQ(r.add(2, "").status(), FOXAPI_OK);       // a test radio on the generator: a tone 50 kHz from its centre
    double peakLegacy = 0.0, floorLegacy = 0.0;
    for (const std::int64_t fft : {1024, 8192, 65536}) {
        CHECK_EQ(r.send(spec(2, fft, 3, 0.0)).status(), FOXAPI_OK);
        BigFrame f;
        const std::uint32_t served = static_cast<std::uint32_t>(std::min<std::int64_t>(fft, 16384));   // the ring is 16,384 wide: a wider FFT is served max-reduced
        const bool got = r.until([&] {
            f = bigFrame(r, 2);
            return f.status == FOXAPI_OK && f.info.binCount == served;
        }, 20000);
        CHECK(got);
        if (!got) { continue; }
        CHECK_EQ(f.bins.size(), static_cast<std::size_t>(served));
        const std::size_t peak = static_cast<std::size_t>(std::max_element(f.bins.begin(), f.bins.end()) - f.bins.begin());
        const double want = (50000.0 / f.info.spanHz + 0.5) * static_cast<double>(served);
        std::printf("  %6lld bins: peak at bin %zu (wanted %.1f), level %.1f dB, median %.1f dB\n", static_cast<long long>(fft), peak, want, f.bins[peak], medianOf(f.bins));
        CHECK(std::fabs(static_cast<double>(peak) - want) <= 3.0 * static_cast<double>(served) / 1024.0);
        if (fft == 1024) {
            peakLegacy = f.bins[peak];
            floorLegacy = medianOf(f.bins);
        } else {
            CHECK(std::fabs(f.bins[peak] - peakLegacy) <= 3.0);   // a tone reads the same whatever the size
            if (fft <= 16384) {   // (a reduced frame's median is the median of group maxima: not comparable)
                const double drop = floorLegacy - medianOf(f.bins);
                const double expect = 10.0 * std::log10(static_cast<double>(fft) / 1024.0);   // the noise per bin falls with the size
                CHECK(std::fabs(drop - expect) <= 3.0);
            }
        }
    }
    // a window changes the shape and not the level
    CHECK_EQ(r.send(spec(2, 8192, 1, 0.0)).status(), FOXAPI_OK);
    BigFrame h;
    CHECK(r.until([&] { h = bigFrame(r, 2); return h.status == FOXAPI_OK && h.info.binCount == 8192u; }, 20000));
    if (h.bins.size() == 8192) { CHECK(std::fabs(*std::max_element(h.bins.begin(), h.bins.end()) - peakLegacy) <= 3.0); }
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_spectrum\n");
    engine_api_test::hermetic("spectrum");
    Access::installHooks();
    testA();
    testB();
    testC();
    return check::finish("test_engine_api_spectrum");
}
