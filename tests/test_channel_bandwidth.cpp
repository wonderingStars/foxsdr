// The BANDWIDTH control must actually narrow the channel.
//
// FIELD REPORT (three users, 0.99.59, RTL-SDR Blog V4 at 2,048,000 and
// 2,400,000 S/s): "changing the bandwidth changes nothing", "on AM I can
// listen to a station +/- 30 kHz away", "LSB, USB and CW show a double-
// sideband signal" and "all sound the same BW". Nothing in the log showed a
// bandwidth change because none was ever logged.
//
// WHAT THIS MEASURES, through the real classes the receiver is built from:
//   1. dsp::Vfo - the channel filter - at the two RTL-SDR rates, with the
//      bandwidth set through the SAME call core::Pipeline::setVfoBandwidthHz
//      makes (Vfo::setBandwidthHz), a complex tone at 0 / +5 / +15 / +30 kHz
//      from the tuned frequency, and the power that survives the filter.
//   2. dsp::Vfo -> dsp::Demodulator, exactly the chain processAudioBlock runs,
//      for the sideband selection of USB and LSB.
//   3. core::Pipeline itself, driven by a synthetic I/Q source, read at the
//      S-meter (channel power AFTER the channel filter, before the demodulator)
//      so the bandwidth reaches the DSP thread through the production call
//      sequence - including after a sample-rate change, which is what the
//      patch page's hand-back does to the receiver.
//
// Every reading is in dB relative to the SAME tone at 0 kHz through the same
// filter, so nothing here depends on the gain convention of a stage.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "core/pipeline.hpp"
#include "dsp/demod.hpp"
#include "dsp/vfo.hpp"
#include "source/iq_source.hpp"
#include "test_check.hpp"

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

// The two rates the reports came from. Decimation is what the pipeline picks
// (round(rate / 200 kHz)): 12 -> 200 kHz and 10 -> 204.8 kHz.
struct RateCase {
    double rateHz;
    unsigned decim;
};
constexpr RateCase kRates[] = {{2400000.0, 12}, {2048000.0, 10}};

// The bandwidths the receiver's combo offers that matter for AM and SSB.
constexpr double kBw10k = 10000.0;
constexpr double kBw6k = 6000.0;
constexpr double kBw3k = 3000.0;

std::vector<std::complex<float>> tone(std::size_t n, double hz, double rateHz) {
    std::vector<std::complex<float>> x(n);
    for (std::size_t i = 0; i < n; ++i) {
        // Phase from the integer sample index in double: exact at any length.
        const double a = kTwoPi * std::fmod(hz * static_cast<double>(i) / rateHz, 1.0);
        x[i] = {static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a))};
    }
    return x;
}

// Mean power (dB, unit tone = 0 dB) of the settled second half of a Vfo's output.
double vfoPowerDb(const RateCase& rc, double bwHz, double toneHz) {
    // Built the way Pipeline builds it (default 150 kHz) and then given the
    // bandwidth the way Pipeline::setVfoBandwidthHz gives it.
    cascade::dsp::Vfo vfo(rc.rateHz, rc.decim, 150000.0);
    vfo.setBandwidthHz(bwHz);
    const std::size_t nIn = static_cast<std::size_t>(rc.rateHz * 0.12);
    const auto x = tone(nIn, toneHz, rc.rateHz);
    std::vector<std::complex<float>> y(nIn / rc.decim + 1);
    const std::size_t m = vfo.process(x.data(), nIn, y.data(), y.size());
    double acc = 0.0;
    std::size_t cnt = 0;
    for (std::size_t i = m / 2; i < m; ++i, ++cnt) { acc += std::norm(y[i]); }
    return 10.0 * std::log10(std::max(acc / static_cast<double>(std::max<std::size_t>(cnt, 1)),
                                      1e-30));
}

// Level at `offHz` relative to the same tone at the tuned frequency.
double vfoRelDb(const RateCase& rc, double bwHz, double offHz) {
    return vfoPowerDb(rc, bwHz, offHz) - vfoPowerDb(rc, bwHz, 0.0);
}

// Audio level (dB, RMS of the real output; a unit complex tone through a
// unity-gain path would read about -3 dB) of a tone `toneHz` from the carrier,
// through the chain processAudioBlock runs: Vfo at the given bandwidth, then
// a Demodulator at the channel rate in `mode`.
double ssbAudioDb(const RateCase& rc, double bwHz, cascade::dsp::DemodMode mode, double toneHz) {
    cascade::dsp::Vfo vfo(rc.rateHz, rc.decim, 150000.0);
    vfo.setBandwidthHz(bwHz);
    cascade::dsp::Demodulator demod(vfo.channelRateHz());
    demod.setMode(mode);
    const std::size_t nIn = static_cast<std::size_t>(rc.rateHz * 0.15);
    const auto x = tone(nIn, toneHz, rc.rateHz);
    std::vector<std::complex<float>> ch(nIn / rc.decim + 1);
    const std::size_t m = vfo.process(x.data(), nIn, ch.data(), ch.size());
    std::vector<float> audio(m);
    demod.process(ch.data(), m, audio.data());
    double acc = 0.0;
    std::size_t cnt = 0;
    for (std::size_t i = m / 2; i < m; ++i, ++cnt) {
        acc += static_cast<double>(audio[i]) * static_cast<double>(audio[i]);
    }
    return 10.0 * std::log10(std::max(acc / static_cast<double>(std::max<std::size_t>(cnt, 1)),
                                      1e-30));
}

// --- A synthetic I/Q source: one complex tone at a settable offset ----------
class ToneSource : public cascade::source::IqSource {
public:
    explicit ToneSource(double rateHz) : rate_(rateHz) {}
    bool start() override {
        running_ = true;
        return true;
    }
    void stop() override { running_ = false; }
    bool running() const override { return running_; }
    bool selfPaced() const override { return false; }
    double sampleRateHz() const override { return rate_; }
    bool setSampleRateHz(double hz) override {
        rate_ = hz;
        return true;
    }
    double centerFrequencyHz() const override { return centerHz_; }
    bool setCenterFrequencyHz(double hz) override {
        centerHz_ = hz;
        return true;
    }
    // One tone at `hz` from the tuned frequency (the second one silent).
    void setToneHz(double hz) { setTones(hz, 1.0, 0.0, 0.0); }
    // Two tones, each with its own offset and amplitude.
    void setTones(double hzA, double ampA, double hzB, double ampB) {
        toneHz_[0].store(hzA, std::memory_order_relaxed);
        toneAmp_[0].store(ampA, std::memory_order_relaxed);
        toneHz_[1].store(hzB, std::memory_order_relaxed);
        toneAmp_[1].store(ampB, std::memory_order_relaxed);
    }
    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        double step[2];
        double amp[2];
        for (int k = 0; k < 2; ++k) {
            step[k] = toneHz_[k].load(std::memory_order_relaxed) / rate_;
            amp[k] = toneAmp_[k].load(std::memory_order_relaxed);
        }
        for (std::size_t i = 0; i < n; ++i) {
            double re = 0.0;
            double im = 0.0;
            for (int k = 0; k < 2; ++k) {
                const double a = kTwoPi * phase_[k];
                re += amp[k] * std::cos(a);
                im += amp[k] * std::sin(a);
                phase_[k] += step[k];
                phase_[k] -= std::floor(phase_[k]);
            }
            dst[i] = {static_cast<float>(re), static_cast<float>(im)};
        }
        return n;
    }
    const char* name() const override { return "channel bandwidth test tone"; }
    const char* lastError() const override { return ""; }

private:
    double rate_;
    double centerHz_ = 13720000.0;
    double phase_[2] = {0.0, 0.0};
    std::atomic<double> toneHz_[2] = {0.0, 0.0};
    std::atomic<double> toneAmp_[2] = {1.0, 0.0};
    bool running_ = false;
};

// Amplitude of the audio at `hz` in the newest 4096 samples of the audio tap
// (Goertzel / single-bin DFT with a Hann window, so a tone between bins does
// not leak into its neighbours). Two tones in the same window share the AGC's
// gain, so their RATIO is the receiver's own response and the AGC drops out.
double audioToneAmp(cascade::core::Pipeline& p, double hz) {
    constexpr std::size_t kN = 4096;
    std::vector<float> a(kN);
    if (p.audioTap(a.data(), kN) != kN) { return 0.0; }
    double re = 0.0;
    double im = 0.0;
    for (std::size_t i = 0; i < kN; ++i) {
        const double w = 0.5 - 0.5 * std::cos(kTwoPi * static_cast<double>(i) / (kN - 1));
        const double ph = kTwoPi * hz * static_cast<double>(i) / 48000.0;
        re += w * a[i] * std::cos(ph);
        im -= w * a[i] * std::sin(ph);
    }
    return std::sqrt(re * re + im * im) / static_cast<double>(kN);
}

double db(double ratio) { return 20.0 * std::log10(std::max(ratio, 1e-12)); }

template <typename Pred>
bool waitFor(Pred pred, int timeoutMs) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

// The S-meter after the channel filter has settled: wait for ~0.4 s of audio to
// pass since `mark`, then average a handful of reads.
double settledMeterDb(cascade::core::Pipeline& p) {
    const std::uint64_t mark = p.audioSamplesProduced();
    (void)waitFor([&] { return p.audioSamplesProduced() - mark >= 19200u; }, 20000);
    double acc = 0.0;
    constexpr int kReads = 6;
    for (int i = 0; i < kReads; ++i) {
        acc += static_cast<double>(p.signalPowerDb());
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    return acc / kReads;
}

}  // namespace

int main() {
    // =========================================================================
    // 1. THE CHANNEL FILTER, Vfo alone, set the way the GUI sets it.
    // =========================================================================
    std::printf("level through dsp::Vfo, dB relative to the same tone at 0 kHz\n");
    std::printf("  rate        bw      +5k      +15k      +30k\n");
    for (const RateCase& rc : kRates) {
        for (const double bw : {kBw10k, kBw6k, kBw3k}) {
            const double d5 = vfoRelDb(rc, bw, 5000.0);
            const double d15 = vfoRelDb(rc, bw, 15000.0);
            const double d30 = vfoRelDb(rc, bw, 30000.0);
            std::printf("  %-9.0f %6.0f  %8.1f  %8.1f  %8.1f\n", rc.rateHz, bw, d5, d15, d30);
            // The wanted channel must still be there at the tuned frequency
            // (the reading is relative, so check the absolute level too).
            const double d0 = vfoPowerDb(rc, bw, 0.0);
            CHECK(d0 > -1.0 && d0 < 1.0);
            // A working AM filter: +30 kHz at least 40 dB down at every width.
            CHECK(d30 <= -40.0);
            // And +15 kHz at least 30 dB down.
            CHECK(d15 <= -30.0);
        }
        // Narrowing 10 kHz -> 3 kHz must move the +5 kHz reading a long way:
        // +5 kHz is the 10 kHz filter's own edge (still passed) and 3.3 kHz
        // outside the 3 kHz filter's.
        const double at10 = vfoRelDb(rc, kBw10k, 5000.0);
        const double at3 = vfoRelDb(rc, kBw3k, 5000.0);
        std::printf("  rate %.0f: +5 kHz is %.1f dB at 10 kHz and %.1f dB at 3 kHz "
                    "(change %.1f dB)\n",
                    rc.rateHz, at10, at3, at10 - at3);
        CHECK(at10 - at3 >= 20.0);
        // The passband is the bandwidth: flat (1.5 dB at most) out to bw/2, and
        // by bw - twice the edge - at least 20 dB down.
        for (const double bw : {kBw10k, kBw6k, kBw3k}) {
            CHECK(vfoRelDb(rc, bw, 0.5 * bw) > -1.5);
            CHECK(vfoRelDb(rc, bw, -0.5 * bw) > -1.5);  // and symmetric
            CHECK(vfoRelDb(rc, bw, 1.0 * bw) <= -20.0);
        }
    }

    // Shape of the filter, for the record: level at fractions of the bandwidth.
    for (const double bw : {kBw3k, kBw10k}) {
        std::printf("  shape at 2.4 MS/s, bw %.0f: ", bw);
        for (const double frac : {0.1, 0.25, 0.4, 0.5, 0.6, 0.75, 1.0, 1.5, 2.0}) {
            std::printf(" %.2f*bw:%.1f", frac, vfoRelDb(kRates[0], bw, frac * bw));
        }
        std::printf("\n");
    }
    {
        const cascade::dsp::Vfo narrow(2400000.0, 12, kBw3k);
        const cascade::dsp::Vfo wide(2400000.0, 12, 150000.0);
        std::printf("  cost: %.1f multiply-adds per input sample at 3 kHz, %.1f at 150 kHz\n",
                    narrow.macsPerInputSample(), wide.macsPerInputSample());
        // Wall time to channelise ONE SECOND of 2.4 MS/s input, for the record.
        const auto x = tone(2400000, 5000.0, 2400000.0);
        std::vector<std::complex<float>> y(2400000 / 12 + 1);
        for (const double bw : {150000.0, 10000.0, 3000.0}) {
            cascade::dsp::Vfo v(2400000.0, 12, bw);
            const auto t0 = std::chrono::steady_clock::now();
            (void)v.process(x.data(), x.size(), y.data(), y.size());
            const auto t1 = std::chrono::steady_clock::now();
            std::printf("  time: %.0f ms per second of signal at bw %.0f\n",
                        std::chrono::duration<double, std::milli>(t1 - t0).count(), bw);
        }
    }

    // =========================================================================
    // 2. SIDEBAND SELECTION, Vfo -> Demodulator as the pipeline runs them.
    //    USB keeps the carrier's upper side, LSB its lower; the other side
    //    must be gone, at the default 3 kHz an SSB mode starts with.
    // =========================================================================
    std::printf("SSB audio level (dB) for a tone 1 kHz above / below the carrier, bw 3 kHz\n");
    for (const RateCase& rc : kRates) {
        for (const auto mode : {cascade::dsp::DemodMode::USB, cascade::dsp::DemodMode::LSB}) {
            const bool usb = (mode == cascade::dsp::DemodMode::USB);
            const double above = ssbAudioDb(rc, kBw3k, mode, +1000.0);
            const double below = ssbAudioDb(rc, kBw3k, mode, -1000.0);
            const double wanted = usb ? above : below;
            const double unwanted = usb ? below : above;
            std::printf("  %-9.0f %s  wanted side %7.1f  other side %7.1f  rejection %5.1f dB\n",
                        rc.rateHz, usb ? "USB" : "LSB", wanted, unwanted, wanted - unwanted);
            CHECK(wanted > -12.0);            // the wanted tone is really heard
            CHECK(wanted - unwanted >= 30.0);  // and the other side is not
        }
    }

    // =========================================================================
    // 3. THE WHOLE PIPELINE, through the calls the GUI makes, read at the
    //    S-meter (channel power after the channel filter, before the demod).
    //    The second half repeats the +5 kHz measurement after the receiver is
    //    stopped, handed a source at the OTHER rate and restarted - the
    //    sequence the patch page's hand-back puts it through (2,400,000 ->
    //    2,048,000 S/s in the reports) - to show the bandwidth survives it.
    // =========================================================================
    std::printf("\nS-meter through core::Pipeline in AM, dB relative to the tone at 0 kHz\n");
    std::printf("  rate        bw      +5k      +15k      +30k\n");
    for (const RateCase& rc : kRates) {
        cascade::core::Pipeline::Config cfg;
        cfg.sampleRateHz = rc.rateHz;
        cfg.fftSize = 1024;
        cfg.averagingAlpha = 0.5f;
        cfg.audioEnabled = false;  // headless: the S-meter is what gets read
        cascade::core::Pipeline pipeline(cfg);
        auto owned = std::make_unique<ToneSource>(rc.rateHz);
        ToneSource* src = owned.get();
        pipeline.setSource(std::move(owned));
        pipeline.setDemodMode(cascade::dsp::DemodMode::AM);  // the mode button
        pipeline.start();
        CHECK(waitFor([&] { return pipeline.audioSamplesProduced() > 4800u; }, 30000));

        const auto level = [&](double offHz) {
            src->setToneHz(offHz);
            return settledMeterDb(pipeline);
        };
        double at5[2] = {0.0, 0.0};
        int slot = 0;
        for (const double bw : {kBw10k, kBw3k}) {
            pipeline.setVfoBandwidthHz(bw);  // the bandwidth combo's call
            const double ref = level(0.0);
            const double d5 = level(5000.0) - ref;
            const double d15 = level(15000.0) - ref;
            const double d30 = level(30000.0) - ref;
            std::printf("  %-9.0f %6.0f  %8.1f  %8.1f  %8.1f\n", rc.rateHz, bw, d5, d15, d30);
            CHECK(d30 <= -40.0);
            CHECK(d15 <= -30.0);
            at5[slot++] = d5;
        }
        CHECK(at5[0] - at5[1] >= 20.0);

        // The patch page takes the radio and hands it back at the other rate.
        const RateCase& other = (rc.rateHz == kRates[0].rateHz) ? kRates[1] : kRates[0];
        pipeline.stop();
        auto back = std::make_unique<ToneSource>(other.rateHz);
        ToneSource* backSrc = back.get();
        pipeline.setSource(std::move(back));
        CHECK(pipeline.setInputRateHz(other.rateHz));
        pipeline.start();
        CHECK(waitFor([&] { return pipeline.audioSamplesProduced() > 4800u; }, 30000));
        backSrc->setToneHz(0.0);
        const double refBack = settledMeterDb(pipeline);
        backSrc->setToneHz(5000.0);
        const double d5Back = settledMeterDb(pipeline) - refBack;
        std::printf("  after hand-back to %.0f S/s: +5 kHz at the 3 kHz setting is %.1f dB\n",
                    other.rateHz, d5Back);
        CHECK(d5Back <= -20.0);
        pipeline.stop();
    }

    // =========================================================================
    // 4. SIDEBAND MODES THROUGH THE WHOLE PIPELINE, and what the Bandwidth
    //    control does to them. Two tones of equal strength are sent at once and
    //    read off the audio at their own frequencies, so the AGC's gain is
    //    common to both and the ratio is the receiver's response.
    //      - at the default 3 kHz the voice band 500 Hz .. 2500 Hz is intact
    //        (the narrow channel filter must not eat the upper half of the
    //        sideband it is centred on the CARRIER of),
    //      - the other sideband is gone (>= 30 dB),
    //      - and choosing 6 kHz brings in a 4.5 kHz tone that 3 kHz keeps out.
    // =========================================================================
    std::printf("\nSideband modes through core::Pipeline (audio tone ratios, dB)\n");
    for (const RateCase& rc : kRates) {
        cascade::core::Pipeline::Config cfg;
        cfg.sampleRateHz = rc.rateHz;
        cfg.fftSize = 1024;
        cfg.averagingAlpha = 0.5f;
        cfg.audioEnabled = false;
        cascade::core::Pipeline pipeline(cfg);
        auto owned = std::make_unique<ToneSource>(rc.rateHz);
        ToneSource* src = owned.get();
        pipeline.setSource(std::move(owned));
        pipeline.start();
        CHECK(waitFor([&] { return pipeline.audioSamplesProduced() > 4800u; }, 30000));

        for (const auto mode : {cascade::dsp::DemodMode::USB, cascade::dsp::DemodMode::LSB}) {
            const bool usb = (mode == cascade::dsp::DemodMode::USB);
            const double s = usb ? +1.0 : -1.0;  // which side of the carrier is wanted
            const char* name = usb ? "USB" : "LSB";
            pipeline.setDemodMode(mode);  // the mode button...
            pipeline.setVfoBandwidthHz(kBw3k);  // ...then its default bandwidth

            // Voice band, both ends, wanted side.
            src->setTones(s * 500.0, 1.0, s * 2500.0, 1.0);
            std::uint64_t mark = pipeline.audioSamplesProduced();
            (void)waitFor([&] { return pipeline.audioSamplesProduced() - mark >= 24000u; }, 20000);
            const double lowTone = audioToneAmp(pipeline, 500.0);
            const double highTone = audioToneAmp(pipeline, 2500.0);
            std::printf("  %-9.0f %s 3k: 2.5 kHz relative to 0.5 kHz  %6.1f dB\n", rc.rateHz,
                        name, db(highTone / lowTone));
            CHECK(lowTone > 1e-4);
            CHECK(std::fabs(db(highTone / lowTone)) <= 3.0);

            // The other sideband.
            src->setTones(s * 2000.0, 1.0, -s * 1200.0, 1.0);
            mark = pipeline.audioSamplesProduced();
            (void)waitFor([&] { return pipeline.audioSamplesProduced() - mark >= 24000u; }, 20000);
            const double wanted = audioToneAmp(pipeline, 2000.0);
            const double unwanted = audioToneAmp(pipeline, 1200.0);
            std::printf("  %-9.0f %s 3k: other sideband 1.2 kHz is %6.1f dB below the wanted\n",
                        rc.rateHz, name, db(wanted / unwanted));
            CHECK(db(wanted / unwanted) >= 30.0);

            // Bandwidth 3 kHz vs 6 kHz: a tone 4.5 kHz out.
            double at4500[2] = {0.0, 0.0};
            int slot = 0;
            for (const double bw : {kBw3k, kBw6k}) {
                pipeline.setVfoBandwidthHz(bw);
                src->setTones(s * 1000.0, 1.0, s * 4500.0, 1.0);
                mark = pipeline.audioSamplesProduced();
                (void)waitFor([&] { return pipeline.audioSamplesProduced() - mark >= 24000u; },
                              20000);
                at4500[slot] = db(audioToneAmp(pipeline, 4500.0) / audioToneAmp(pipeline, 1000.0));
                std::printf("  %-9.0f %s %.0f: 4.5 kHz relative to 1 kHz  %6.1f dB\n", rc.rateHz,
                            name, bw, at4500[slot]);
                ++slot;
            }
            CHECK(at4500[0] <= -30.0);  // 3 kHz keeps it out
            CHECK(at4500[1] >= -6.0);   // 6 kHz lets it in
        }
        pipeline.stop();
    }

    // =========================================================================
    // 5. THE DEMODULATOR'S SIDEBAND WIDTH, on its own.
    // =========================================================================
    {
        // Untouched, and told the width it already has, a Demodulator is the
        // one it always was - bit for bit.
        const auto x = tone(40000, 800.0, 200000.0);
        cascade::dsp::Demodulator fresh(200000.0);
        cascade::dsp::Demodulator same(200000.0);
        fresh.setMode(cascade::dsp::DemodMode::USB);
        same.setMode(cascade::dsp::DemodMode::USB);
        same.setSsbBandwidthHz(3000.0);
        std::vector<float> a(x.size());
        std::vector<float> b(x.size());
        fresh.process(x.data(), x.size(), a.data());
        same.process(x.data(), x.size(), b.data());
        CHECK(a == b);
        CHECK_NEAR(same.ssbBandwidthHz(), 3000.0, 0.0);

        // Clamped, and a non-finite request is the default.
        cascade::dsp::Demodulator d(200000.0);
        d.setSsbBandwidthHz(10.0);
        CHECK_NEAR(d.ssbBandwidthHz(), 1000.0, 0.0);
        d.setSsbBandwidthHz(1.0e9);
        CHECK_NEAR(d.ssbBandwidthHz(), 60000.0, 0.0);
        d.setSsbBandwidthHz(std::nan(""));
        CHECK_NEAR(d.ssbBandwidthHz(), 3000.0, 0.0);
    }

    // =========================================================================
    // 6. A WIDE BANDWIDTH IS THE FILTER IT ALWAYS WAS, and a narrow one has the
    //    one extra stage. (Tap for tap is pinned by test_vfo's cost checks.)
    // =========================================================================
    {
        for (const RateCase& rc : kRates) {
            const cascade::dsp::Vfo wide(rc.rateHz, rc.decim, 150000.0);
            const cascade::dsp::Vfo mid(rc.rateHz, rc.decim, 60000.0);
            const cascade::dsp::Vfo narrow(rc.rateHz, rc.decim, kBw10k);
            CHECK(wide.stageCount() == 1);
            CHECK(mid.stageCount() == 1);
            CHECK(narrow.stageCount() == 2);
        }
    }

    // =========================================================================
    // 7. THE OTHER RATES A RADIO RUNS AT, and the staged (>= 4 MS/s) design:
    //    the narrow filter must work behind every one of them, including a
    //    decimation of 1 (a 250 kS/s sound-card or file source).
    // =========================================================================
    std::printf("\nnarrow filter at other rates (dB relative to 0 kHz): +5k / +30k\n");
    for (const RateCase& rc : {RateCase{10000000.0, 50}, RateCase{6000000.0, 30},
                               RateCase{3200000.0, 16}, RateCase{2500000.0, 10},
                               RateCase{250000.0, 1}, RateCase{192000.0, 1}}) {
        for (const double bw : {kBw10k, kBw3k}) {
            const double d5 = vfoRelDb(rc, bw, 5000.0);
            const double d30 = vfoRelDb(rc, bw, 30000.0);
            std::printf("  %-10.0f decim %-3u bw %-6.0f  %8.1f  %8.1f\n", rc.rateHz, rc.decim, bw,
                        d5, d30);
            CHECK(d30 <= -40.0);
            if (bw == kBw3k) { CHECK(d5 <= -40.0); }
            if (bw == kBw10k) { CHECK(d5 > -1.5); }
        }
    }

    // A narrow filter streams like every other one: any split of the input into
    // blocks gives the same output, bit for bit (the pipeline hands the Vfo
    // 1024 samples at a time, which is not a multiple of the decimation).
    {
        std::vector<std::complex<float>> x(60000);
        std::uint32_t s = 0x1234567u;
        for (auto& v : x) {
            s = s * 1664525u + 1013904223u;
            const float re = static_cast<float>(s >> 8) / 8388608.0f - 1.0f;
            s = s * 1664525u + 1013904223u;
            const float im = static_cast<float>(s >> 8) / 8388608.0f - 1.0f;
            v = {re, im};
        }
        const auto run = [&](std::size_t block) {
            cascade::dsp::Vfo v(2400000.0, 12, kBw3k);
            v.setOffsetHz(-37000.0);
            std::vector<std::complex<float>> out;
            std::vector<std::complex<float>> buf(block / 12 + 2);
            for (std::size_t pos = 0; pos < x.size(); pos += block) {
                const std::size_t n = std::min(block, x.size() - pos);
                const std::size_t m = v.process(x.data() + pos, n, buf.data(), buf.size());
                out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(m));
            }
            return out;
        };
        const auto whole = run(x.size());
        CHECK(whole.size() == x.size() / 12);
        for (const std::size_t block : {std::size_t{7}, std::size_t{1024}, std::size_t{4999}}) {
            CHECK(run(block) == whole);
        }
    }

    return testSummary("test_channel_bandwidth");
}
