// The converter's TRANSLATION LAYER, end to end: the pipeline's activeSource()
// and the patch page's PatchRadio, the two places a frequency crosses from the
// user's side (AIR) to the radio's.
//
// core/freq_converter.hpp's arithmetic is tested on its own in
// test_freq_converter; this is the half that proves it is WIRED: that a tune
// through activeSource() reaches the radio converted, that the radio's
// readback comes back as air, that a swap never carries one radio's converter
// onto the next, that a frequency the converter cannot deliver never reaches
// the radio, and that an inverting converter really does turn the band back
// the right way round in the samples the spectrum and every decoder see.
//
// Every AppWindow path that tunes or reads the receiver (the counter and its
// switches, click-to-tune, presets, bookmarks, the scanner, the web remote,
// CAT, the plugin API, the saved centre) goes through Pipeline::activeSource();
// test_converter_call_sites holds that to be true of the source tree.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/freq_converter.hpp"
#include "core/patch_radio.hpp"
#include "core/pipeline.hpp"
#include "source/siggen_source.hpp"
#include "test_check.hpp"

using cascade::core::ConverterMode;
using cascade::core::ConverterSetting;
using cascade::core::Pipeline;
using cascade::core::SpectrumFrame;

namespace {

ConverterSetting up(double lo, bool inv = false) { return {ConverterMode::Up, lo, inv}; }
ConverterSetting down(double lo, bool inv = false) { return {ConverterMode::Down, lo, inv}; }

Pipeline::Config testConfig() {
    Pipeline::Config cfg;
    cfg.sampleRateHz = 1000000.0;
    cfg.fftSize = 1024;
    cfg.averagingAlpha = 0.5f;
    cfg.audioEnabled = false;  // no sound device on a build machine, none needed
    return cfg;
}

// A radio stand-in: stores what it is told, like a nominal-centre source,
// and counts the calls so "the radio was never asked" is checkable.
class FakeRadio final : public cascade::source::IqSource {
public:
    bool start() override { return true; }
    void stop() override {}
    bool running() const override { return false; }
    bool selfPaced() const override { return false; }
    double sampleRateHz() const override { return 1.0e6; }
    bool setSampleRateHz(double) override { return false; }
    double centerFrequencyHz() const override { return centre; }
    bool setCenterFrequencyHz(double hz) override {
        ++sets;
        centre = hz;
        return true;
    }
    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        for (std::size_t i = 0; i < n; ++i) { dst[i] = {0.0f, 0.0f}; }
        return n;
    }
    const char* name() const override { return "Fake radio"; }
    const char* lastError() const override { return "radio's own error"; }
    double centre = 100.0e6;
    int sets = 0;
};

// A SELF-PACED radio, as every hardware driver is: read() blocks until the
// samples it hands back are due (a device's bounded read), so the pipeline
// runs its self-paced loop for it - a different loop from the generator's,
// with its own copy of the mirror. It delivers one tone `toneHz` above its
// centre and counts its reads, so the test can see that loop really ran.
class PacedToneRadio final : public cascade::source::IqSource {
public:
    explicit PacedToneRadio(double toneHz) : toneHz_(toneHz) {}
    bool start() override {
        abort_ = false;
        return true;
    }
    void stop() override { abort_ = true; }
    bool running() const override { return !abort_; }
    bool selfPaced() const override { return true; }
    double sampleRateHz() const override { return 1.0e6; }
    bool setSampleRateHz(double) override { return false; }
    double centerFrequencyHz() const override { return centre_; }
    bool setCenterFrequencyHz(double hz) override {
        centre_ = hz;
        return true;
    }
    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        // Paced like the device it stands for: n samples take n/rate seconds.
        std::this_thread::sleep_for(std::chrono::microseconds(static_cast<long long>(n)));
        if (abort_) { return 0; }
        const double step = 2.0 * 3.14159265358979323846 * toneHz_ / 1.0e6;
        for (std::size_t i = 0; i < n; ++i) {
            dst[i] = {static_cast<float>(std::cos(phase_)), static_cast<float>(std::sin(phase_))};
            phase_ += step;
            if (phase_ > 3.14159265358979323846) { phase_ -= 2.0 * 3.14159265358979323846; }
        }
        reads.fetch_add(1);
        return n;
    }
    const char* name() const override { return "Paced tone radio"; }
    const char* lastError() const override { return ""; }
    std::atomic<std::uint64_t> reads{0};

private:
    double toneHz_;
    double phase_ = 0.0;
    double centre_ = 125.0e6;
    std::atomic<bool> abort_{true};
};

std::size_t argmax(const std::vector<float>& v) {
    std::size_t best = 0;
    for (std::size_t i = 1; i < v.size(); ++i) {
        if (v[i] > v[best]) { best = i; }
    }
    return best;
}

// The peak bin of the newest spectrum after `frames` fresh frames, so the
// averaging has forgotten whatever the band looked like before.
std::size_t settledPeak(Pipeline& p, int frames) {
    SpectrumFrame f;
    int got = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (got < frames && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(30)) {
        if (p.getLatestFrame(f)) {
            ++got;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    CHECK(got == frames);
    return f.dbBins.empty() ? 0 : argmax(f.dbBins);
}

void testTuneAndReadback() {
    std::printf("  the pipeline: a tune in air reaches the radio converted, and back\n");
    Pipeline p(testConfig());
    // No converter: the view IS the source.
    CHECK(!cascade::core::converterActive(p.converter()));
    CHECK(p.activeSource().centerFrequencyHz() == p.rawSource().centerFrequencyHz());

    // THE TESTER'S EXAMPLE: a 16.4 kHz CW preset through a 125 MHz up-converter
    // must put the radio on 125.0164 MHz, and the counter must read 16.4 kHz.
    p.setConverter(up(125.0e6));
    CHECK(p.activeSource().setCenterFrequencyHz(16400.0));
    CHECK(p.rawSource().centerFrequencyHz() == 125016400.0);
    CHECK(p.activeSource().centerFrequencyHz() == 16400.0);
    // SAQ Grimeton through the 2 MHz converter.
    p.setConverter(up(2.0e6));
    CHECK(p.activeSource().setCenterFrequencyHz(17200.0));
    CHECK(p.rawSource().centerFrequencyHz() == 2017200.0);
    CHECK(p.activeSource().centerFrequencyHz() == 17200.0);

    // RADIO READBACK -> DISPLAY: whatever the radio says it is on, read as air.
    CHECK(p.rawSource().setCenterFrequencyHz(100017200.0));
    p.setConverter(up(100.0e6));
    CHECK(p.activeSource().centerFrequencyHz() == 17200.0);
    // Switching a converter on does NOT retune the radio: it relabels.
    CHECK(p.rawSource().centerFrequencyHz() == 100017200.0);

    // Inverted: radio = LO - air.
    p.setConverter(up(125.0e6, true));
    CHECK(p.activeSource().setCenterFrequencyHz(16400.0));
    CHECK(p.rawSource().centerFrequencyHz() == 124983600.0);
    CHECK(p.activeSource().centerFrequencyHz() == 16400.0);

    // The rest of the source is the radio's own, untouched by the view.
    CHECK(std::strcmp(p.activeSource().name(), p.rawSource().name()) == 0);
    CHECK(p.activeSource().sampleRateHz() == p.rawSource().sampleRateHz());
    CHECK(p.activeSource().selfPaced() == p.rawSource().selfPaced());

    // Off again: air and radio are the same number once more.
    p.setConverter(ConverterSetting{});
    CHECK(p.activeSource().centerFrequencyHz() == p.rawSource().centerFrequencyHz());
}

void testUnreachableNeverReachesTheRadio() {
    std::printf("  a frequency the converter cannot deliver is refused before the radio\n");
    Pipeline p(testConfig());
    auto fake = std::make_unique<FakeRadio>();
    FakeRadio* radio = fake.get();
    p.setSource(std::move(fake));
    p.setConverter(down(9.75e9));  // an LNB
    const int before = radio->sets;
    // 100 MHz through a 9.75 GHz down-converter is -9.65 GHz at the radio.
    CHECK(!p.activeSource().setCenterFrequencyHz(100.0e6));
    CHECK(radio->sets == before);           // never asked
    CHECK(radio->centre == 100.0e6);        // still where it was
    const std::string why = p.activeSource().lastError();
    CHECK(why.find("converter") != std::string::npos);
    // A reachable one goes through, converted, and clears the view's reason.
    CHECK(p.activeSource().setCenterFrequencyHz(10.4895e9));
    CHECK(radio->sets == before + 1);
    CHECK_NEAR(radio->centre, 739.5e6, 1e-3);
    CHECK(std::string(p.activeSource().lastError()) == "radio's own error");
    CHECK_NEAR(p.activeSource().centerFrequencyHz(), 10.4895e9, 1e-3);
}

void testSwapNeverCarriesAConverter() {
    std::printf("  a source swap puts the converter back to OFF\n");
    Pipeline p(testConfig());
    p.setConverter(up(125.0e6));
    CHECK(cascade::core::converterActive(p.converter()));
    auto fake = std::make_unique<FakeRadio>();
    fake->centre = 433.92e6;
    p.setSource(std::move(fake));
    CHECK(!cascade::core::converterActive(p.converter()));
    CHECK(p.activeSource().centerFrequencyHz() == 433.92e6);
    // ...and back to the generator: off again, whatever was set in between.
    p.setConverter(up(2.0e6, true));
    p.setSource(nullptr);
    CHECK(!cascade::core::converterActive(p.converter()));
    CHECK(p.activeSource().centerFrequencyHz() == p.rawSource().centerFrequencyHz());
}

void testMirrorReachesTheSpectrum() {
    std::printf("  an inverting converter turns the band the right way round\n");
    const Pipeline::Config cfg = testConfig();
    // A tone 125 kHz ABOVE the radio's centre: fftshifted bin 512 + 128.
    const std::size_t above = cfg.fftSize / 2 + 128;
    const std::size_t below = cfg.fftSize / 2 - 128;
    Pipeline p(cfg);
    p.sigGen().setTone(0, 125000.0, 0.0f);
    p.sigGen().setNoiseFloorDb(-300.0f);
    p.setConverter(up(125.0e6));  // not inverted: the band as the radio sees it
    p.start();
    CHECK(settledPeak(p, 40) == above);
    // Behind an inverting converter the signal that reaches the radio 125 kHz
    // ABOVE its centre is 125 kHz BELOW the air frequency, and that is where
    // the spectrum must show it.
    p.setConverter(up(125.0e6, true));
    CHECK(settledPeak(p, 80) == below);
    // And back.
    p.setConverter(ConverterSetting{});
    CHECK(settledPeak(p, 80) == above);
    p.stop();
}

// The same mirror, through the loop every HARDWARE source runs in: the
// pipeline's self-paced loop conjugates on its own (pipeline.cpp), and the
// generator test above never reaches it.
void testMirrorInTheSelfPacedLoop() {
    std::printf("  an inverting converter mirrors a self-paced (hardware) source too\n");
    const Pipeline::Config cfg = testConfig();
    const std::size_t above = cfg.fftSize / 2 + 128;  // +125 kHz at 1 MS/s
    const std::size_t below = cfg.fftSize / 2 - 128;
    Pipeline p(cfg);
    auto radio = std::make_unique<PacedToneRadio>(125000.0);
    PacedToneRadio* raw = radio.get();
    p.setSource(std::move(radio));
    CHECK(p.activeSource().selfPaced());
    p.setConverter(up(125.0e6));  // not inverted: the band as the radio sees it
    p.start();
    CHECK(settledPeak(p, 40) == above);
    p.setConverter(up(125.0e6, true));
    CHECK(settledPeak(p, 80) == below);
    p.setConverter(ConverterSetting{});
    CHECK(settledPeak(p, 80) == above);
    p.stop();
    CHECK(raw->reads.load() > 10);  // the self-paced loop is what delivered them
}

void testPatchRadio() {
    std::printf("  the patch page's radios use the same layer\n");
    auto gen = std::make_unique<cascade::source::SigGenSource>(1.0e6);
    gen->sigGen().setTone(0, 125000.0, 0.0f);
    gen->sigGen().setNoiseFloorDb(-300.0f);
    cascade::source::SigGenSource* raw = gen.get();
    cascade::core::patch::PatchRadio radio(1, std::move(gen), "Signal generator");

    // No converter: the readback is the source's.
    CHECK(radio.centreHz() == raw->centerFrequencyHz());
    radio.setConverter(up(125.0e6));
    CHECK(radio.setCentreHz(16400.0));
    CHECK(raw->centerFrequencyHz() == 125016400.0);
    CHECK(radio.centreHz() == 16400.0);
    // Unreachable: refused, the source untouched.
    radio.setConverter(down(9.75e9));
    CHECK(!radio.setCentreHz(100.0e6));
    CHECK(raw->centerFrequencyHz() == 125016400.0);

    // The mirror, in the patch radio's own spectrum.
    radio.setConverter(up(125.0e6, true));
    std::string err;
    CHECK(radio.start(err));
    std::vector<float> db;
    std::uint64_t seq = 0;
    std::size_t peak = 0;
    bool any = false;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5)) {
        // A few spectra, so the estimator's averaging has settled.
        if (radio.spectrum(db, seq) && seq >= 6) {
            any = true;
            peak = argmax(db);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    radio.stop();
    CHECK(any);
    // The patch spectrum is 2048 bins, fftshifted: +125 kHz at 1 MS/s is bin
    // 1024 + 256; mirrored it must be 1024 - 256.
    CHECK(db.size() == 2048);
    CHECK(peak + 2 >= 1024 - 256 && peak <= 1024 - 256 + 2);
}

}  // namespace

int main() {
    std::printf("test_converter_routing\n");
    testTuneAndReadback();
    testUnreachableNeverReachesTheRadio();
    testSwapNeverCarriesAConverter();
    testMirrorReachesTheSpectrum();
    testMirrorInTheSelfPacedLoop();
    testPatchRadio();
    return testSummary("test_converter_routing");
}
