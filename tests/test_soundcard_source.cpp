// Tests for the SOUND CARD source (source/soundcard_source.hpp), its real-to-
// complex conversion (dsp/real_to_iq.hpp) and the Source section's rules for
// it (gui/soundcard_panel.hpp, the remembered-source rule in tune_control.hpp,
// the config fields).
//
// NO SOUND CARD IS USED. The source is handed a FAKE backend that lists
// scripted devices and pushes scripted frames through exactly the callback
// path PortAudio drives, so ctest needs no audio hardware. The REAL PortAudio
// backend is also run against a SCRIPTED PortAudio (section 12): a Windows
// machine's worth of host APIs to prove only WASAPI is offered, and an abort
// that never returns to prove no lock a close holds is one a list, an open or
// a construction takes. The two sections that talk to the real PortAudio
// only initialise it and ENUMERATE (nothing is opened) and assert nothing
// about what they find beyond PortAudio's own init count.
//
// THE HEADLINE MEASUREMENT is the tester's own case: SAQ on 17.2 kHz through a
// card at 192 kHz. A synthetic 17.2 kHz tone on the card must land at 17.2 kHz
// on the spectrum's air axis (0 - 96 kHz), and a USB receiver on 16.4 kHz must
// turn it into an 800 Hz audio tone - measured at the far end of the real
// pipeline, not asserted about the converter alone. The same receiver on
// 17.6 kHz must NOT hear it (the tone is then below the dial, the lower
// sideband), which is what proves the spectrum is not mirrored.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define TEST_GETPID _getpid
#else
#include <unistd.h>
#define TEST_GETPID getpid
#endif

#include "core/config.hpp"
#include "core/patch_devices.hpp"
#include "core/pipeline.hpp"
#include "dsp/demod.hpp"
#include "dsp/fft.hpp"
#include "dsp/real_to_iq.hpp"
#include "gui/device_scan_plan.hpp"
#include "gui/soundcard_panel.hpp"
#include "gui/tune_control.hpp"
#include "sink/pa_init.hpp"
#include "source/soundcard_portaudio.hpp"
#include "source/soundcard_source.hpp"
#include "test_check.hpp"

namespace {

using cascade::source::SoundCardBackend;
using cascade::source::SoundCardDevice;
using cascade::source::SoundCardFormat;
using cascade::source::SoundCardRate;
using cascade::source::SoundCardSettings;
using cascade::source::SoundCardSource;
using Clock = std::chrono::steady_clock;

constexpr double kTwoPi = 6.283185307179586476925286766559;

// --- the fake backend -------------------------------------------------------------

// Fills `frames` interleaved frames starting at absolute frame `first`.
using Generator = std::function<void(float* out, std::size_t frames, std::uint64_t first, int ch)>;

class FakeBackend final : public SoundCardBackend {
public:
    std::vector<SoundCardDevice> devices;
    std::atomic<bool> aliveFlag{true};
    std::atomic<int> openCalls{0};
    std::atomic<int> closeCalls{0};
    int lastIndex = -1;
    int lastChannels = 0;
    double lastRate = 0.0;
    bool lastExclusive = false;
    Generator gen;  // set -> a real-time feeding thread runs while open

    ~FakeBackend() override { close(); }

    std::vector<SoundCardDevice> listDevices() override { return devices; }

    bool open(const SoundCardDevice& dev, int channels, double rateHz, bool exclusive, PushFn push,
              void* user, std::string& error) override {
        close();
        ++openCalls;
        lastIndex = dev.index;
        lastChannels = channels;
        lastRate = rateHz;
        lastExclusive = exclusive;
        if (failOpen || std::find(refuseRatesHz.begin(), refuseRatesHz.end(), rateHz) != refuseRatesHz.end()) {
            error = "fake refused";
            return false;
        }
        push_ = push;
        user_ = user;
        isOpen_ = true;
        if (gen) {
            feeding_ = true;
            feeder_ = std::thread([this] { feedLoop(); });
        }
        return true;
    }

    void close() override {
        stopFeeding();
        if (isOpen_) { ++closeCalls; }
        isOpen_ = false;
    }

    bool alive() override {
        ++aliveCalls;
        return isOpen_ && aliveFlag.load();
    }

    std::atomic<int> aliveCalls{0};
    std::vector<double> refuseRatesHz;  // card rates open() refuses

    // The card goes quiet (unplugged): no more callbacks.
    void stopFeeding() {
        feeding_ = false;
        if (feeder_.joinable()) { feeder_.join(); }
    }

    // One scripted block through the SAME callback a stream would call.
    void push(const float* interleaved, std::size_t frames) {
        if (push_ != nullptr) { push_(user_, interleaved, frames); }
    }

    bool failOpen = false;

private:
    void feedLoop() {
        const std::size_t block = static_cast<std::size_t>(lastRate / 100.0);  // 10 ms
        std::vector<float> buf(block * static_cast<std::size_t>(lastChannels));
        std::uint64_t frame = 0;
        auto next = Clock::now();
        while (feeding_) {
            gen(buf.data(), block, frame, lastChannels);
            frame += block;
            push(buf.data(), block);
            next += std::chrono::milliseconds(10);
            std::this_thread::sleep_until(next);
        }
    }

    PushFn push_ = nullptr;
    void* user_ = nullptr;
    std::atomic<bool> isOpen_{false};
    std::atomic<bool> feeding_{false};
    std::thread feeder_;
};

SoundCardDevice stereoCard(const std::string& name, const std::string& api, int index,
                           std::vector<SoundCardRate> rates, int channels = 2) {
    SoundCardDevice d;
    d.index = index;
    d.name = name;
    d.hostApi = api;
    d.maxInputChannels = channels;
    d.defaultRateHz = 48000.0;
    d.rates = std::move(rates);
    return d;
}

std::vector<SoundCardRate> ratesUpTo192k() {
    return {{8000.0, false},  {11025.0, false}, {16000.0, false}, {44100.0, false},
            {48000.0, false}, {96000.0, false}, {192000.0, true}};
}

SoundCardSource makeSource(const std::shared_ptr<FakeBackend>& fake) {
    return SoundCardSource([fake] { return std::static_pointer_cast<SoundCardBackend>(fake); });
}

// --- measurement helpers ----------------------------------------------------------

struct Peak {
    double hz = 0.0;       // baseband, signed
    double powerDb = -300.0;
};

// Hann-windowed FFT of the LAST 16384 samples of x (complex rate `rate`);
// returns the strongest bin's baseband frequency and the power (dB) at any
// other baseband frequency asked for.
constexpr std::size_t kFft = 16384;

std::vector<double> powerSpectrum(const std::vector<std::complex<float>>& x) {
    std::vector<std::complex<float>> in(kFft), out(kFft);
    const std::size_t off = x.size() - kFft;
    for (std::size_t i = 0; i < kFft; ++i) {
        const double w = 0.5 - 0.5 * std::cos(kTwoPi * static_cast<double>(i) / kFft);
        in[i] = x[off + i] * static_cast<float>(w);
    }
    cascade::dsp::ComplexFFT fft(kFft);
    fft.forward(in.data(), out.data());
    std::vector<double> p(kFft);
    for (std::size_t k = 0; k < kFft; ++k) { p[k] = std::norm(out[k]) + 1e-30; }
    return p;
}

double binHz(std::size_t k, double rate) {
    const double kk = (k < kFft / 2) ? static_cast<double>(k) : static_cast<double>(k) - kFft;
    return kk * rate / kFft;
}

Peak strongest(const std::vector<double>& p, double rate) {
    Peak best;
    std::size_t at = 0;
    for (std::size_t k = 0; k < p.size(); ++k) {
        if (p[k] > p[at]) { at = k; }
    }
    best.hz = binHz(at, rate);
    best.powerDb = 10.0 * std::log10(p[at]);
    return best;
}

// Strongest power within +/-3 bins of a baseband frequency, dB.
double powerNearDb(const std::vector<double>& p, double hz, double rate) {
    const long c = static_cast<long>(std::lround(hz / rate * kFft));
    double best = 1e-30;
    for (long d = -3; d <= 3; ++d) {
        long k = c + d;
        k = ((k % static_cast<long>(kFft)) + static_cast<long>(kFft)) % static_cast<long>(kFft);
        best = std::max(best, p[static_cast<std::size_t>(k)]);
    }
    return 10.0 * std::log10(best);
}

// Total power within +/-4 bins of a baseband frequency (the whole Hann main
// lobe, wherever between bins the tone falls), dB.
double energyNearDb(const std::vector<double>& p, double hz, double rate) {
    const long c = static_cast<long>(std::lround(hz / rate * kFft));
    double sum = 1e-30;
    for (long d = -4; d <= 4; ++d) {
        long k = c + d;
        k = ((k % static_cast<long>(kFft)) + static_cast<long>(kFft)) % static_cast<long>(kFft);
        sum += p[static_cast<std::size_t>(k)];
    }
    return 10.0 * std::log10(sum);
}

// Read everything the source will give until it has nothing for one read.
std::vector<std::complex<float>> drain(SoundCardSource& src, std::size_t want) {
    std::vector<std::complex<float>> out;
    std::vector<std::complex<float>> buf(2048);
    while (out.size() < want) {
        const std::size_t got = src.read(buf.data(), buf.size());
        if (got == 0) { break; }
        out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(got));
    }
    return out;
}

// Pushes `frames` frames of a generator through the fake in 1024-frame
// blocks, the way a card's callback arrives.
void pushGenerated(FakeBackend& fake, int ch, std::size_t frames, const Generator& g) {
    std::vector<float> buf(1024 * static_cast<std::size_t>(ch));
    std::uint64_t at = 0;
    while (at < frames) {
        const std::size_t n = std::min<std::size_t>(1024, frames - static_cast<std::size_t>(at));
        g(buf.data(), n, at, ch);
        fake.push(buf.data(), n);
        at += n;
    }
}

// --- 1. the conversion alone -----------------------------------------------------

void testRealToIqAxis() {
    // Two tones in one real signal: SAQ's 17.2 kHz, and 60 kHz in the upper
    // half of the band, at different levels so they cannot be confused.
    const double fs = 192000.0;
    const double outRate = cascade::dsp::RealToIq::outputRateHz(fs);
    const double centre = cascade::dsp::RealToIq::centreHz(fs);
    CHECK_NEAR(outRate, 96000.0, 0.0);
    CHECK_NEAR(centre, 48000.0, 0.0);

    const std::size_t n = 2 * (kFft + 4096);
    std::vector<float> x(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / fs;
        x[i] = static_cast<float>(0.5 * std::cos(kTwoPi * 17200.0 * t + 0.3) +
                                  0.05 * std::cos(kTwoPi * 60000.0 * t + 1.1));
    }
    cascade::dsp::RealToIq conv;
    std::vector<std::complex<float>> z(cascade::dsp::RealToIq::outputCapacity(n));
    const std::size_t m = conv.process(x.data(), n, z.data());
    CHECK(m == n / 2);  // 2m real samples give exactly m outputs
    z.resize(m);

    const std::vector<double> p = powerSpectrum(z);
    const Peak pk = strongest(p, outRate);
    // THE AXIS: baseband + centre is the air frequency.
    CHECK_NEAR(centre + pk.hz, 17200.0, outRate / kFft);
    std::printf("real->iq: 17.2 kHz tone lands at %.1f Hz on the air axis\n", centre + pk.hz);
    // The two LEVELS as the energy under each tone's window lobe, so neither
    // is shortened by where it happens to fall between FFT bins (up to 1.4 dB
    // with a Hann window - which is what a peak-bin comparison measured).
    const double at60 = energyNearDb(p, 60000.0 - centre, outRate);
    const double at17 = energyNearDb(p, 17200.0 - centre, outRate);
    CHECK_NEAR(at60 - at17, -20.0, 0.1);  // 0.05 vs 0.5: -20 dB, and on the right side
    // THE MIRRORS, where a spectrum that kept the negative frequencies (or
    // shifted the wrong way) would put them: -17.2 kHz folds to air 78.8 kHz,
    // -60 kHz to air 36 kHz.
    const double mirror17 = powerNearDb(p, (fs / 2.0 - 17200.0) - centre, outRate);
    const double mirror60 = powerNearDb(p, (fs / 2.0 - 60000.0) - centre, outRate);
    std::printf("real->iq: mirror of 17.2 kHz at %.1f dB, of 60 kHz at %.1f dB (re the tone)\n",
                mirror17 - pk.powerDb, mirror60 - pk.powerDb);
    CHECK(mirror17 - pk.powerDb < -80.0);
    CHECK(mirror60 - pk.powerDb < -80.0);

    // THE LEVEL: a real tone of amplitude 0.5 is a complex tone of 0.5 (the
    // header's factor of 2). Measured as the mean |z| in the settled part of
    // a single-tone run.
    {
        cascade::dsp::RealToIq c1;
        std::vector<float> s(40000);
        for (std::size_t i = 0; i < s.size(); ++i) {
            s[i] = static_cast<float>(0.5 * std::cos(kTwoPi * 17200.0 * static_cast<double>(i) / fs));
        }
        std::vector<std::complex<float>> o(cascade::dsp::RealToIq::outputCapacity(s.size()));
        const std::size_t got = c1.process(s.data(), s.size(), o.data());
        double sum = 0.0;
        std::size_t cnt = 0;
        for (std::size_t i = 1000; i < got; ++i, ++cnt) { sum += std::abs(o[i]); }
        CHECK_NEAR(sum / static_cast<double>(cnt), 0.5, 0.01);
    }

    // BLOCKS DO NOT MATTER: any split gives bit-for-bit one call's output.
    {
        cascade::dsp::RealToIq a;
        cascade::dsp::RealToIq b;
        std::vector<std::complex<float>> whole(cascade::dsp::RealToIq::outputCapacity(n));
        const std::size_t wn = a.process(x.data(), n, whole.data());
        std::vector<std::complex<float>> parts;
        const std::size_t sizes[] = {7, 13, 1000, 1, 2, 4096};
        std::size_t at = 0;
        std::size_t si = 0;
        std::vector<std::complex<float>> tmp(8192);
        while (at < n) {
            const std::size_t len = std::min(sizes[si++ % 6], n - at);
            const std::size_t got = b.process(x.data() + at, len, tmp.data());
            parts.insert(parts.end(), tmp.begin(), tmp.begin() + static_cast<std::ptrdiff_t>(got));
            at += len;
        }
        CHECK(parts.size() == wn);
        bool same = parts.size() == wn;
        for (std::size_t i = 0; same && i < wn; ++i) { same = (parts[i] == whole[i]); }
        CHECK(same);
    }
}

// --- 2. the source: real mode, channel choice ---------------------------------------

Generator twoChannelTones(double fs, double leftHz, double rightHz) {
    return [=](float* out, std::size_t frames, std::uint64_t first, int ch) {
        for (std::size_t i = 0; i < frames; ++i) {
            const double t = static_cast<double>(first + i) / fs;
            out[i * static_cast<std::size_t>(ch)] = static_cast<float>(0.4 * std::cos(kTwoPi * leftHz * t));
            if (ch > 1) {
                out[i * static_cast<std::size_t>(ch) + 1] =
                    static_cast<float>(0.4 * std::cos(kTwoPi * rightHz * t));
            }
        }
    };
}

void testRealModeChannel() {
    for (int channel = 0; channel < 2; ++channel) {
        auto fake = std::make_shared<FakeBackend>();
        fake->devices = {stereoCard("USB Audio CODEC", "Windows WASAPI", 7, ratesUpTo192k())};
        SoundCardSource src = makeSource(fake);
        SoundCardSettings s;
        s.device = "USB Audio CODEC";
        s.hostApi = "Windows WASAPI";
        s.cardRateHz = 192000.0;
        s.format = SoundCardFormat::RealMono;
        s.channel = channel;
        CHECK(src.openWith(s));
        CHECK(src.start());
        CHECK_NEAR(src.sampleRateHz(), 96000.0, 0.0);
        CHECK_NEAR(src.centerFrequencyHz(), 48000.0, 0.0);
        CHECK(fake->lastChannels == 2);
        pushGenerated(*fake, 2, 2 * (kFft + 2048), twoChannelTones(192000.0, 17200.0, 40000.0));
        const auto z = drain(src, kFft + 2048);
        CHECK(z.size() >= kFft + 1024);
        if (z.size() < kFft) { continue; }
        const Peak pk = strongest(powerSpectrum(z), 96000.0);
        const double want = channel == 0 ? 17200.0 : 40000.0;
        std::printf("real mode, %s channel: tone at %.1f Hz\n", channel == 0 ? "left" : "right",
                    48000.0 + pk.hz);
        CHECK_NEAR(48000.0 + pk.hz, want, 96000.0 / kFft);
        src.closeDevice();
    }
}

// --- 3. I/Q mode: channel order and swap ---------------------------------------------

void testIqModeAndSwap() {
    const double fs = 48000.0;
    const double f = 6000.0;
    // A POSITIVE-frequency complex tone as an I/Q receiver delivers it:
    // I = cos, Q = sin on left and right.
    const Generator iq = [=](float* out, std::size_t frames, std::uint64_t first, int ch) {
        for (std::size_t i = 0; i < frames; ++i) {
            const double ph = kTwoPi * f * static_cast<double>(first + i) / fs;
            out[i * static_cast<std::size_t>(ch)] = static_cast<float>(0.3 * std::cos(ph));
            out[i * static_cast<std::size_t>(ch) + 1] = static_cast<float>(0.3 * std::sin(ph));
        }
    };
    for (int swap = 0; swap < 2; ++swap) {
        auto fake = std::make_shared<FakeBackend>();
        // The name in the form PortAudio's ALSA backend really gives it.
        fake->devices = {stereoCard("SoftRock: USB Audio (hw:2,0)", "ALSA", 3, ratesUpTo192k())};
        SoundCardSource src = makeSource(fake);
        SoundCardSettings s;
        s.device = "SoftRock: USB Audio (hw:2,0)";
        s.hostApi = "ALSA";
        s.cardRateHz = fs;
        s.format = SoundCardFormat::IqStereo;
        s.swapIq = (swap == 1);
        s.iqCentreHz = 7.05e6;
        CHECK(src.openWith(s));
        CHECK(src.start());
        CHECK_NEAR(src.sampleRateHz(), fs, 0.0);
        CHECK_NEAR(src.centerFrequencyHz(), 7.05e6, 0.0);
        // Nominal in I/Q mode: the external receiver's frequency is recorded.
        CHECK(src.setCenterFrequencyHz(7.1e6));
        CHECK_NEAR(src.centerFrequencyHz(), 7.1e6, 0.0);
        pushGenerated(*fake, 2, kFft + 1024, iq);
        const auto z = drain(src, kFft + 1024);
        CHECK(z.size() == kFft + 1024);
        if (z.size() < kFft) { continue; }
        const Peak pk = strongest(powerSpectrum(z), fs);
        std::printf("I/Q mode, swap %s: tone at %+.1f Hz from the centre\n", swap ? "on" : "off", pk.hz);
        CHECK_NEAR(pk.hz, swap ? -f : f, fs / kFft);
        // And the exact samples: left is I, right is Q (or the other way).
        const float i0 = 0.3f;  // cos(0)
        CHECK_NEAR(z[0].real(), swap ? 0.0f : i0, 1e-6);
        CHECK_NEAR(z[0].imag(), swap ? i0 : 0.0f, 1e-6);
    }
}

// --- 4. rates ------------------------------------------------------------------------

void testRates() {
    const SoundCardDevice card = stereoCard("Line In", "Windows WASAPI", 2, ratesUpTo192k());
    // REAL: a card rate whose half is not a whole number the pipeline takes
    // (8 kHz -> 4 kHz, 11.025 kHz -> 5512.5 Hz) is not offered.
    const auto real = cascade::source::soundCardRatesFor(card, SoundCardFormat::RealMono);
    std::vector<double> realHz;
    for (const auto& r : real) { realHz.push_back(r.hz); }
    CHECK((realHz == std::vector<double>{16000.0, 44100.0, 48000.0, 96000.0, 192000.0}));
    const auto iq = cascade::source::soundCardRatesFor(card, SoundCardFormat::IqStereo);
    CHECK(iq.size() == 7);
    // A one-channel card cannot do I/Q at all.
    const SoundCardDevice mono = stereoCard("Mic", "MME", 1, ratesUpTo192k(), 1);
    CHECK(cascade::source::soundCardRatesFor(mono, SoundCardFormat::IqStereo).empty());

    auto fake = std::make_shared<FakeBackend>();
    fake->devices = {card, mono};
    SoundCardSource src = makeSource(fake);
    SoundCardSettings s;
    s.device = "Line In";
    s.hostApi = "Windows WASAPI";
    s.cardRateHz = 190000.0;  // not offered: the nearest is 192 kHz
    CHECK(src.openWith(s));
    CHECK_NEAR(fake->lastRate, 192000.0, 0.0);
    CHECK(fake->lastExclusive);  // 192 kHz is an exclusive-only rate on this card
    CHECK(std::string(src.lastError()).find("192000") != std::string::npos);
    CHECK_NEAR(src.settings().cardRateHz, 192000.0, 0.0);
    CHECK_NEAR(src.sampleRateHz(), 96000.0, 0.0);
    CHECK((src.supportedSampleRatesHz() == std::vector<double>{8000.0, 22050.0, 24000.0, 48000.0, 96000.0}));
    double lo = 0.0, hi = 0.0;
    CHECK(src.frequencyRangeHz(lo, hi));
    CHECK_NEAR(lo, 0.0, 0.0);
    CHECK_NEAR(hi, 96000.0, 0.0);
    // The centre of a real-mode card cannot move.
    CHECK(!src.setCenterFrequencyHz(100000.0));
    CHECK(src.setCenterFrequencyHz(48000.0));
    // A new COMPLEX rate reopens the stream at the matching CARD rate, shared
    // mode this time.
    CHECK(src.setSampleRateHz(24000.0));
    CHECK_NEAR(fake->lastRate, 48000.0, 0.0);
    CHECK(!fake->lastExclusive);
    CHECK_NEAR(src.sampleRateHz(), 24000.0, 0.0);
    CHECK(fake->openCalls == 2);

    // I/Q on the one-channel card and the right channel of it are refused,
    // and nothing is opened.
    SoundCardSource m = makeSource(fake);
    SoundCardSettings ms;
    ms.device = "Mic";
    ms.hostApi = "MME";
    ms.format = SoundCardFormat::IqStereo;
    const int before = fake->openCalls;
    CHECK(!m.openWith(ms));
    ms.format = SoundCardFormat::RealMono;
    ms.channel = 1;
    CHECK(!m.openWith(ms));
    CHECK(fake->openCalls == before);
    ms.channel = 0;
    CHECK(m.openWith(ms));
    CHECK(fake->lastChannels == 1);
}

// --- 5. the device is found by name AND host API, or not at all ----------------------

void testMissingDevice() {
    auto fake = std::make_shared<FakeBackend>();
    // The same card, listed under MME only - the WASAPI entry the settings
    // name is not there (a different machine, or the card unplugged).
    fake->devices = {stereoCard("USB Audio CODEC", "MME", 0, ratesUpTo192k()),
                     stereoCard("Microphone (Realtek)", "Windows WASAPI", 1, ratesUpTo192k())};
    fake->devices[1].isDefault = true;
    SoundCardSource src = makeSource(fake);
    SoundCardSettings s;
    s.device = "USB Audio CODEC";
    s.hostApi = "Windows WASAPI";
    CHECK(!src.openWith(s));
    CHECK(fake->openCalls == 0);  // NOTHING opened in its place
    const std::string why = src.lastError();
    CHECK(why.find("not connected") != std::string::npos);
    CHECK(why.find("USB Audio CODEC") != std::string::npos);
    CHECK(!src.isOpen());
    // An empty name is "the default input" - the patch's unchosen node.
    SoundCardSettings d;
    CHECK(src.openWith(d));
    CHECK(fake->lastIndex == 1);
    // The pure lookup.
    CHECK(cascade::source::findSoundCard(fake->devices, "USB Audio CODEC", "MME") == 0);
    CHECK(cascade::source::findSoundCard(fake->devices, "USB Audio CODEC", "Windows WASAPI") == -1);
}

// --- 6. liveness ---------------------------------------------------------------------

void testLiveness() {
    // (a) the host API says the stream stopped: noticed within a poll or two.
    {
        auto fake = std::make_shared<FakeBackend>();
        fake->devices = {stereoCard("USB Audio CODEC", "Windows WASAPI", 0, ratesUpTo192k())};
        SoundCardSource src = makeSource(fake);
        SoundCardSettings s;
        s.device = "USB Audio CODEC";
        s.hostApi = "Windows WASAPI";
        CHECK(src.openWith(s));
        CHECK(src.start());
        pushGenerated(*fake, 2, 4800, twoChannelTones(48000.0, 1000.0, 1000.0));
        (void)drain(src, 100000);
        CHECK(!src.faulted());
        fake->aliveFlag = false;  // the card is pulled out
        const auto t0 = Clock::now();
        std::vector<std::complex<float>> buf(512);
        while (!src.faulted() && Clock::now() - t0 < std::chrono::seconds(3)) {
            CHECK(src.read(buf.data(), buf.size()) == 0);
        }
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        std::printf("liveness: a stopped stream was noticed after %.0f ms\n", ms);
        CHECK(src.faulted());
        CHECK(ms < 1000.0);
        CHECK(std::string(src.lastError()).find("stopped") != std::string::npos);
        CHECK(src.deviceDead());
        CHECK(!src.start());  // a dead card is not restarted by the pipeline
    }
    // (b) the stream still claims to run but no samples arrive: the stall.
    {
        auto fake = std::make_shared<FakeBackend>();
        fake->devices = {stereoCard("USB Audio CODEC", "Windows WASAPI", 0, ratesUpTo192k())};
        SoundCardSource src = makeSource(fake);
        SoundCardSettings s;
        s.device = "USB Audio CODEC";
        s.hostApi = "Windows WASAPI";
        CHECK(src.openWith(s));
        CHECK(src.start());
        const auto t0 = Clock::now();
        std::vector<std::complex<float>> buf(512);
        while (!src.faulted() && Clock::now() - t0 < std::chrono::seconds(5)) {
            (void)src.read(buf.data(), buf.size());
        }
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        std::printf("liveness: a silent stream was called dead after %.0f ms\n", ms);
        CHECK(src.faulted());
        CHECK(ms >= 1900.0 && ms < 3000.0);
        CHECK(std::string(src.lastError()).find("delivered nothing") != std::string::npos);
    }
    // (c) stop() gets an in-flight read out promptly, and is not a fault.
    {
        auto fake = std::make_shared<FakeBackend>();
        fake->devices = {stereoCard("USB Audio CODEC", "Windows WASAPI", 0, ratesUpTo192k())};
        SoundCardSource src = makeSource(fake);
        SoundCardSettings s;
        s.device = "USB Audio CODEC";
        s.hostApi = "Windows WASAPI";
        CHECK(src.openWith(s));
        CHECK(src.start());
        src.stop();
        std::vector<std::complex<float>> buf(512);
        const auto t0 = Clock::now();
        CHECK(src.read(buf.data(), buf.size()) == 0);
        CHECK(Clock::now() - t0 < std::chrono::milliseconds(50));
        CHECK(!src.faulted());
    }
}

// --- 7. the pipeline, end to end: the tester's own case -----------------------------

// SAQ at 17.2 kHz, on the left channel of a card at 192 kHz, with a little
// noise so the audio chain always has something to work on.
Generator saq(double fs) {
    auto rng = std::make_shared<std::mt19937>(12345u);
    return [=](float* out, std::size_t frames, std::uint64_t first, int ch) {
        std::normal_distribution<float> noise(0.0f, 0.001f);
        for (std::size_t i = 0; i < frames; ++i) {
            const double t = static_cast<double>(first + i) / fs;
            const float v = static_cast<float>(0.2 * std::cos(kTwoPi * 17200.0 * t)) + noise(*rng);
            for (int c = 0; c < ch; ++c) { out[i * static_cast<std::size_t>(ch) + static_cast<std::size_t>(c)] = v; }
        }
    };
}

// The audio tone's frequency by interpolated zero crossings, and the share
// of the audio's power within +/-50 Hz of it (a Goertzel scan).
struct Tone {
    double hz = 0.0;
    double purity = 0.0;
};

Tone measureTone(const std::vector<float>& a, double rate) {
    Tone t;
    // Zero crossings, upward, linearly interpolated.
    std::vector<double> ups;
    for (std::size_t i = 1; i < a.size(); ++i) {
        if (a[i - 1] < 0.0f && a[i] >= 0.0f) {
            const double frac = a[i - 1] / static_cast<double>(a[i - 1] - a[i]);
            ups.push_back(static_cast<double>(i - 1) + frac);
        }
    }
    if (ups.size() >= 3) {
        t.hz = rate * static_cast<double>(ups.size() - 1) / (ups.back() - ups.front());
    }
    const auto goertzel = [&](double f) {
        const double w = kTwoPi * f / rate;
        const double c = 2.0 * std::cos(w);
        double s1 = 0.0, s2 = 0.0;
        for (const float v : a) {
            const double s0 = v + c * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        return s1 * s1 + s2 * s2 - c * s1 * s2;
    };
    double total = 0.0, near = 0.0;
    for (double f = 50.0; f < 4000.0; f += 10.0) {
        const double p = goertzel(f);
        total += p;
        if (std::fabs(f - 800.0) <= 50.0) { near += p; }
    }
    t.purity = total > 0.0 ? near / total : 0.0;
    return t;
}

struct EndToEnd {
    double spectrumPeakAirHz = 0.0;
    Tone tone;
};

EndToEnd runPipeline(double dialHz) {
    EndToEnd r;
    auto fake = std::make_shared<FakeBackend>();
    fake->devices = {stereoCard("USB Audio CODEC", "Windows WASAPI", 4, ratesUpTo192k())};
    fake->gen = saq(192000.0);
    auto src = std::make_unique<SoundCardSource>(
        [fake] { return std::static_pointer_cast<SoundCardBackend>(fake); });
    SoundCardSettings s;
    s.device = "USB Audio CODEC";
    s.hostApi = "Windows WASAPI";
    s.cardRateHz = 192000.0;
    CHECK(src->openWith(s));

    cascade::core::Pipeline::Config cfg;
    cfg.sampleRateHz = 2000000.0;  // the generator's rate, as in the app
    cfg.fftSize = 8192;
    cfg.audioEnabled = false;
    cascade::core::Pipeline p(cfg);
    // The application's order: install, then follow the source's rate.
    p.setSource(std::move(src));
    CHECK(p.setInputRateHz(p.activeSource().sampleRateHz()));
    const double centre = p.activeSource().centerFrequencyHz();
    const double rate = p.inputRateHz();
    CHECK_NEAR(rate, 96000.0, 0.0);
    CHECK_NEAR(centre, 48000.0, 0.0);
    p.setDemodMode(cascade::dsp::DemodMode::USB);
    p.setVfoBandwidthHz(2400.0);
    p.setVfoOffsetHz(dialHz - centre);
    p.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));

    cascade::core::SpectrumFrame frame;
    CHECK(p.getLatestFrame(frame));
    if (!frame.dbBins.empty()) {
        const std::size_t n = frame.dbBins.size();
        std::size_t at = 0;
        for (std::size_t k = 0; k < n; ++k) {
            if (frame.dbBins[k] > frame.dbBins[at]) { at = k; }
        }
        // fftshifted: bin n/2 is the centre.
        r.spectrumPeakAirHz =
            centre + (static_cast<double>(at) - static_cast<double>(n) / 2.0) * rate / static_cast<double>(n);
    }
    std::vector<float> audio(4096);
    CHECK(p.audioTap(audio.data(), audio.size()) == audio.size());
    r.tone = measureTone(audio, 48000.0);
    CHECK(!p.faulted());
    p.stop();
    return r;
}

void testPipelineSaq() {
    const EndToEnd usb = runPipeline(16400.0);
    std::printf("pipeline: spectrum peak at %.1f Hz on the air axis; USB at 16.4 kHz -> %.2f Hz "
                "audio, %.1f%% of the audio power within 50 Hz of 800 Hz\n",
                usb.spectrumPeakAirHz, usb.tone.hz, 100.0 * usb.tone.purity);
    CHECK_NEAR(usb.spectrumPeakAirHz, 17200.0, 96000.0 / 8192.0);
    CHECK_NEAR(usb.tone.hz, 800.0, 2.0);
    CHECK(usb.tone.purity > 0.9);
    // The same receiver 400 Hz ABOVE the tone must not hear it: 17.2 kHz is
    // then on the lower sideband. A mirrored spectrum would get this wrong.
    const EndToEnd above = runPipeline(17600.0);
    std::printf("pipeline: USB at 17.6 kHz (tone on the lower sideband) -> %.1f%% of the audio "
                "power within 50 Hz of 800 Hz\n",
                100.0 * above.tone.purity);
    CHECK(above.tone.purity < 0.2);
}

// A card pulled out while the pipeline streams: the pipeline's own fault
// state, with the source's reason, which is what the Source section shows.
void testPipelineFault() {
    auto fake = std::make_shared<FakeBackend>();
    fake->devices = {stereoCard("USB Audio CODEC", "Windows WASAPI", 4, ratesUpTo192k())};
    fake->gen = saq(48000.0);
    auto src = std::make_unique<SoundCardSource>(
        [fake] { return std::static_pointer_cast<SoundCardBackend>(fake); });
    SoundCardSettings s;
    s.device = "USB Audio CODEC";
    s.hostApi = "Windows WASAPI";
    CHECK(src->openWith(s));
    cascade::core::Pipeline::Config cfg;
    cfg.audioEnabled = false;
    cascade::core::Pipeline p(cfg);
    p.setSource(std::move(src));
    CHECK(p.setInputRateHz(p.activeSource().sampleRateHz()));
    p.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(!p.faulted());
    fake->stopFeeding();
    fake->aliveFlag = false;
    const auto t0 = Clock::now();
    while (!p.faulted() && Clock::now() - t0 < std::chrono::seconds(4)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(p.faulted());
    std::printf("pipeline: unplugged card -> \"%s\"\n", p.faultMessage().c_str());
    CHECK(p.faultMessage().find("USB Audio CODEC") != std::string::npos);
    p.stop();
    // Swapping the dead source out closes its stream off this thread; the
    // fake's close is still counted once that thread has run.
    p.setSource(nullptr);
    const auto t1 = Clock::now();
    while (fake->closeCalls.load() == 0 && Clock::now() - t1 < std::chrono::seconds(2)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(fake->closeCalls.load() >= 1);
}

// --- 8. args, config, the Source section's rules -------------------------------------

void testArgsRoundTrip() {
    SoundCardSettings s;
    s.device = "Line In (2- USB Audio CODEC, rev=2|x%)";
    s.hostApi = "Windows WASAPI";
    s.cardRateHz = 176400.0;
    s.format = SoundCardFormat::IqStereo;
    s.channel = 1;
    s.swapIq = true;
    s.iqCentreHz = 7050000.5;
    const std::string args = cascade::source::soundCardArgs(s);
    // No bare separator of the grammar survives inside a field.
    CHECK(args.find("CODEC,") == std::string::npos);
    CHECK(args.find("|") == std::string::npos);
    SoundCardSettings back;
    CHECK(cascade::source::parseSoundCardArgs(args, back));
    CHECK(back.device == s.device);
    CHECK(back.hostApi == s.hostApi);
    CHECK_NEAR(back.cardRateHz, s.cardRateHz, 0.0);
    CHECK(back.format == s.format);
    CHECK(back.channel == 1);
    CHECK(back.swapIq);
    CHECK_NEAR(back.iqCentreHz, s.iqCentreHz, 0.001);
    SoundCardSettings bad;
    CHECK(!cascade::source::parseSoundCardArgs("format=wideband", bad));
    CHECK(!cascade::source::parseSoundCardArgs("rate=fast", bad));
    CHECK(!cascade::source::parseSoundCardArgs("device=abc%2", bad));
    CHECK(!cascade::source::parseSoundCardArgs("channel=middle", bad));
    // DeviceSource::open takes the same string.
    auto fake = std::make_shared<FakeBackend>();
    fake->devices = {stereoCard(s.device, s.hostApi, 9, {{176400.0, false}})};
    SoundCardSource src = makeSource(fake);
    CHECK(src.open(args));
    CHECK(fake->lastIndex == 9);
    CHECK(std::string(src.driverKey()) == "soundcard");
}

std::string tempPath(const char* tag) {
    const auto dir = std::filesystem::temp_directory_path();
    return (dir / ("foxsdr_soundcard_" + std::string(tag) + "_" + std::to_string(TEST_GETPID()) + ".json"))
        .string();
}

void testConfigRoundTrip() {
    using cascade::core::AppConfig;
    using cascade::core::ConfigStore;
    AppConfig cfg;
    cfg.sourceKind = "soundcard";
    cfg.soundCard.device = "Line In (2- USB Audio CODEC)";
    cfg.soundCard.hostApi = "Windows WASAPI";
    cfg.soundCard.rateHz = 192000.0;
    cfg.soundCard.format = "iq";
    cfg.soundCard.channel = 1;
    cfg.soundCard.swapIq = true;
    cfg.soundCard.centreHz = 7050000.0;
    const std::string path = tempPath("cfg");
    std::string err;
    CHECK(ConfigStore::save(path, cfg, err));
    AppConfig back;
    CHECK(ConfigStore::load(path, back, err));
    CHECK(back.sourceKind == "soundcard");
    CHECK(back.soundCard.device == cfg.soundCard.device);
    CHECK(back.soundCard.hostApi == cfg.soundCard.hostApi);
    CHECK_NEAR(back.soundCard.rateHz, 192000.0, 0.0);
    CHECK(back.soundCard.format == "iq");
    CHECK(back.soundCard.channel == 1);
    CHECK(back.soundCard.swapIq);
    CHECK_NEAR(back.soundCard.centreHz, 7050000.0, 0.0);

    // Hand-edited nonsense goes back to the defaults, field by field.
    std::string text = ConfigStore::serialize(cfg);
    const auto put = [&](const std::string& from, const std::string& to) {
        const std::size_t at = text.find(from);
        CHECK(at != std::string::npos);
        if (at != std::string::npos) { text.replace(at, from.size(), to); }
    };
    put("\"format\": \"iq\"", "\"format\": \"wideband\"");
    put("\"channel\": 1", "\"channel\": 5");
    put("\"rateHz\": 192000.0", "\"rateHz\": 5000000.0");
    put("\"centreHz\": 7050000.0", "\"centreHz\": \"seven\"");
    CHECK(ConfigStore::writeFile(path, text, err));
    AppConfig odd;
    CHECK(ConfigStore::load(path, odd, err));
    CHECK(odd.sourceKind == "soundcard");
    CHECK(odd.soundCard.format == "real");
    CHECK(odd.soundCard.channel == 0);
    CHECK_NEAR(odd.soundCard.rateHz, 48000.0, 0.0);
    CHECK_NEAR(odd.soundCard.centreHz, 0.0, 0.0);
    std::error_code ec;
    std::filesystem::remove(path, ec);

    // The Source section's mapping both ways.
    const SoundCardSettings s = cascade::gui::soundCardFromConfig(cfg.soundCard);
    CHECK(s.format == SoundCardFormat::IqStereo);
    CHECK(s.swapIq && s.channel == 1);
    const AppConfig::SoundCard c2 = cascade::gui::soundCardToConfig(s);
    CHECK(c2.device == cfg.soundCard.device && c2.hostApi == cfg.soundCard.hostApi);
    CHECK(c2.format == "iq" && c2.channel == 1 && c2.swapIq);
    CHECK_NEAR(c2.rateHz, 192000.0, 0.0);
    CHECK_NEAR(c2.centreHz, 7050000.0, 0.0);
}

void testRememberedAndTune() {
    // A card that did not open at startup: the session runs on the generator,
    // and the save still names the sound card.
    const cascade::gui::RememberedSource keep =
        cascade::gui::rememberedSourceAfterFailedOpen("soundcard", "", "", "", 96000.0);
    CHECK(keep.valid());
    CHECK(keep.kind == "soundcard");
    const cascade::gui::SavedSource saved = cascade::gui::sourceToSave("siggen", "", "", "", 2e6, keep);
    CHECK(saved.kind == "soundcard");

    // Tuning a fixed centre moves the VFO instead. Real card at 192 kHz:
    // centre 48 kHz, span 0 - 96 kHz, a 2.4 kHz USB filter.
    const auto t = cascade::gui::tuneWithFixedCentre(17200.0, 0.0, 48000.0, 96000.0, 2400.0);
    CHECK(t.inside);
    CHECK_NEAR(t.wantAbsHz, 17200.0, 0.0);
    // The offset the caller was keeping is part of where it wanted the VFO.
    const auto k = cascade::gui::tuneWithFixedCentre(48000.0, -31600.0, 48000.0, 96000.0, 2400.0);
    CHECK(k.inside);
    CHECK_NEAR(k.wantAbsHz, 16400.0, 0.0);
    const auto out = cascade::gui::tuneWithFixedCentre(120000.0, 0.0, 48000.0, 96000.0, 2400.0);
    CHECK(!out.inside);
    CHECK(!out.tooWide);
    // The sentence gives the VFO's REACH with this filter: the span less half
    // the filter at each end.
    CHECK_NEAR(out.loHz, 1200.0, 0.0);
    CHECK_NEAR(out.hiHz, 94800.0, 0.0);
}

// Item 5 of the review: "inside" and the VFO's own clamp must be ONE rule. A
// tune within half a filter of the span's edge used to be reported inside and
// then moved by setVfoToAbsoluteHz's clamp - "never clamped" said the header.
void testFixedCentreEdge() {
    using cascade::gui::tuneWithFixedCentre;
    using cascade::gui::vfoOffsetInsideSpan;
    const double c = 48000.0, rate = 96000.0, bw = 2400.0;
    // The very edge of the reach is inside, and lands exactly as asked.
    const auto edge = tuneWithFixedCentre(94800.0, 0.0, c, rate, bw);
    CHECK(edge.inside);
    CHECK_NEAR(vfoOffsetInsideSpan(edge.wantAbsHz - c, rate, bw), edge.wantAbsHz - c, 0.0);
    // Half a hertz past it is refused - and so is 95 kHz, INSIDE the span but
    // within half the filter of its edge (the reviewer's case).
    CHECK(!tuneWithFixedCentre(94800.5, 0.0, c, rate, bw).inside);
    CHECK(!tuneWithFixedCentre(95000.0, 0.0, c, rate, bw).inside);
    CHECK(!tuneWithFixedCentre(1000.0, 0.0, c, rate, bw).inside);
    CHECK(tuneWithFixedCentre(1200.0, 0.0, c, rate, bw).inside);
    // THE PROPERTY, swept across and beyond the span in 100 Hz steps for three
    // filters: a tune is inside exactly when the VFO's clamp leaves it alone.
    int disagreements = 0;
    int insideCount = 0;
    for (const double w : {2400.0, 12000.0, 50000.0}) {
        for (double want = -10000.0; want <= 110000.0; want += 100.0) {
            const auto r = tuneWithFixedCentre(want, 0.0, c, rate, w);
            const double off = want - c;
            const bool unclamped = vfoOffsetInsideSpan(off, rate, w) == off;
            if (r.inside != unclamped) { ++disagreements; }
            if (r.inside) { ++insideCount; }
        }
    }
    std::printf("fixed centre: %d tunes inside, %d disagreements with the VFO clamp\n", insideCount,
                disagreements);
    CHECK(disagreements == 0);
    CHECK(insideCount > 1000);  // the sweep really did cover the reach
    // A filter wider than the span leaves no reach: every tune is refused,
    // with that reason, and never lands half outside.
    const auto wide = tuneWithFixedCentre(48000.0, 0.0, c, rate, 150000.0);
    CHECK(wide.tooWide);
    CHECK(!wide.inside);
    CHECK(tuneWithFixedCentre(48000.0, 0.0, c, rate, 96000.0).tooWide);  // exactly the span
    CHECK(!tuneWithFixedCentre(48000.0, 0.0, c, rate, 95999.0).tooWide);
}

// Item 4 of the review: WFM's 150 kHz filter on a 96 kHz card left the VFO
// wherever it was - half outside what the card delivers. It is centred.
void testVfoInsideSpan() {
    using cascade::gui::vfoOffsetInsideSpan;
    CHECK_NEAR(vfoOffsetInsideSpan(30000.0, 96000.0, 150000.0), 0.0, 0.0);   // wider: centred
    CHECK_NEAR(vfoOffsetInsideSpan(-30000.0, 96000.0, 96000.0), 0.0, 0.0);   // exactly the span
    CHECK_NEAR(vfoOffsetInsideSpan(60000.0, 96000.0, 2400.0), 46800.0, 0.0);  // brought inside
    CHECK_NEAR(vfoOffsetInsideSpan(-60000.0, 96000.0, 2400.0), -46800.0, 0.0);
    CHECK_NEAR(vfoOffsetInsideSpan(-31600.0, 96000.0, 2400.0), -31600.0, 0.0);  // left alone
    CHECK_NEAR(vfoOffsetInsideSpan(46800.0, 96000.0, 2400.0), 46800.0, 0.0);    // on the edge
}

// The patch page: a radio's key names only the card; the Source section's
// settings apply when it is the same card; a SoapySDR scan has nothing to fear
// from a sound card.
void testPatchRules() {
    const std::string key = cascade::source::soundCardDeviceArgs("Line In (2- USB, rev|2)", "MME");
    CHECK(key.find('|') == std::string::npos);  // it sits inside a "driver|args" key
    SoundCardSettings section;
    section.device = "Line In (2- USB, rev|2)";
    section.hostApi = "MME";
    section.format = SoundCardFormat::IqStereo;
    section.swapIq = true;
    section.iqCentreHz = 7.05e6;
    section.cardRateHz = 96000.0;
    SoundCardSettings same;
    CHECK(cascade::source::parseSoundCardArgs(cascade::gui::soundCardArgsForPatch(key, section), same));
    CHECK(same.device == section.device && same.hostApi == "MME");
    CHECK(same.format == SoundCardFormat::IqStereo && same.swapIq);
    CHECK_NEAR(same.iqCentreHz, 7.05e6, 0.001);
    // Another card: plain real mono, left.
    SoundCardSettings other;
    CHECK(cascade::source::parseSoundCardArgs(
        cascade::gui::soundCardArgsForPatch(
            cascade::source::soundCardDeviceArgs("Line In (2- USB, rev|2)", "Windows WASAPI"), section),
        other));
    CHECK(other.hostApi == "Windows WASAPI");
    CHECK(other.format == SoundCardFormat::RealMono && other.channel == 0 && !other.swapIq);
    CHECK(!cascade::gui::scanMayProbe({"rtlsdr"}, "soundcard", key));
}

// --- 10. the review's items: the capture block, start, rates, the alive poll ----------

// R1/R4: the realtime callback writes WHOLE frames and counts a block that did
// not fit. A 4-frame (8-float) ring offered 5 stereo frames keeps the first 4
// exactly and counts one overrun.
void testWholeFramesAndOverruns() {
    SoundCardSource::Capture cap(8);
    cap.channels.store(2);
    const float block[10] = {1, -1, 2, -2, 3, -3, 4, -4, 5, -5};
    SoundCardSource::pushFrames(&cap, block, 5);
    CHECK(cap.ring.size() == 8);
    CHECK(cap.overruns.load() == 1);
    float got[8] = {};
    CHECK(cap.ring.read(got, 8) == 8);
    bool same = true;
    for (int i = 0; i < 8; ++i) { same = same && got[i] == block[i]; }
    CHECK(same);
    // A block that fits is not an overrun.
    SoundCardSource::pushFrames(&cap, block, 3);
    CHECK(cap.overruns.load() == 1);
    CHECK(cap.ring.size() == 6);
    // Two free floats in a stereo ring are ONE frame, not two samples of two.
    SoundCardSource::pushFrames(&cap, block, 2);
    CHECK(cap.ring.size() == 8);
    CHECK(cap.overruns.load() == 2);
}

// A stereo I/Q generator whose I sample is a constant level, so the first
// sample read says which push it came from.
Generator constantIq(float level) {
    return [=](float* out, std::size_t frames, std::uint64_t /*first*/, int ch) {
        for (std::size_t i = 0; i < frames; ++i) {
            out[i * static_cast<std::size_t>(ch)] = level;
            out[i * static_cast<std::size_t>(ch) + 1] = 0.0f;
        }
    };
}

SoundCardSettings iqSettings(const std::string& name, double rate) {
    SoundCardSettings s;
    s.device = name;
    s.hostApi = "Windows WASAPI";
    s.cardRateHz = rate;
    s.format = SoundCardFormat::IqStereo;
    return s;
}

// R3: what the card captured while the pipeline was stopped is stale, and
// start() throws it away.
void testStartDrainsStale() {
    auto fake = std::make_shared<FakeBackend>();
    fake->devices = {stereoCard("USB Audio CODEC", "Windows WASAPI", 0, ratesUpTo192k())};
    SoundCardSource src = makeSource(fake);
    CHECK(src.openWith(iqSettings("USB Audio CODEC", 48000.0)));
    pushGenerated(*fake, 2, 3000, constantIq(0.25f));  // captured before start: stale
    CHECK(src.start());
    pushGenerated(*fake, 2, 1000, constantIq(0.75f));  // after start: fresh
    std::vector<std::complex<float>> buf(4096);
    const std::size_t got = src.read(buf.data(), buf.size());
    CHECK(got == 1000);
    CHECK_NEAR(got != 0 ? buf[0].real() : 0.0f, 0.75f, 0.0);
}

// R6 and item 7: a new rate reopens the card and it is still RUNNING; a rate
// the card refuses puts it back as it was and says so; and when even that
// fails the card is reported closed, never as running.
void testRateChange() {
    // (a) accepted: running again, samples flow.
    {
        auto fake = std::make_shared<FakeBackend>();
        fake->devices = {stereoCard("USB Audio CODEC", "Windows WASAPI", 0, ratesUpTo192k())};
        SoundCardSource src = makeSource(fake);
        CHECK(src.openWith(iqSettings("USB Audio CODEC", 48000.0)));
        CHECK(src.start());
        CHECK(src.setSampleRateHz(96000.0));
        CHECK(src.running());
        CHECK_NEAR(fake->lastRate, 96000.0, 0.0);
        pushGenerated(*fake, 2, 500, constantIq(0.5f));
        std::vector<std::complex<float>> buf(1024);
        CHECK(src.read(buf.data(), buf.size()) == 500);
    }
    // (b) refused: back at 48 kHz, running, and the reason on the line.
    {
        auto fake = std::make_shared<FakeBackend>();
        fake->devices = {stereoCard("USB Audio CODEC", "Windows WASAPI", 0, ratesUpTo192k())};
        fake->refuseRatesHz = {96000.0};
        SoundCardSource src = makeSource(fake);
        CHECK(src.openWith(iqSettings("USB Audio CODEC", 48000.0)));
        CHECK(src.start());
        CHECK(!src.setSampleRateHz(96000.0));
        CHECK(src.isOpen());
        CHECK(src.running());
        CHECK_NEAR(src.sampleRateHz(), 48000.0, 0.0);
        CHECK_NEAR(fake->lastRate, 48000.0, 0.0);
        const std::string why = src.lastError();
        std::printf("rate change refused: \"%s\"\n", why.c_str());
        CHECK(why.find("fake refused") != std::string::npos);
        CHECK(why.find("still running at 48000 Hz") != std::string::npos);
        pushGenerated(*fake, 2, 500, constantIq(0.5f));
        std::vector<std::complex<float>> buf(1024);
        CHECK(src.read(buf.data(), buf.size()) == 500);
    }
    // (c) refused, and the old rate will not reopen either: CLOSED, and start()
    // refuses with both reasons.
    {
        auto fake = std::make_shared<FakeBackend>();
        fake->devices = {stereoCard("USB Audio CODEC", "Windows WASAPI", 0, ratesUpTo192k())};
        SoundCardSource src = makeSource(fake);
        CHECK(src.openWith(iqSettings("USB Audio CODEC", 48000.0)));
        CHECK(src.start());
        fake->failOpen = true;
        CHECK(!src.setSampleRateHz(96000.0));
        CHECK(!src.isOpen());
        CHECK(!src.running());
        CHECK(!src.start());
        const std::string why = src.lastError();
        std::printf("rate change and restore both refused: \"%s\"\n", why.c_str());
        CHECK(why.find("could not be reopened at 48000 Hz") != std::string::npos);
    }
}

// R8: while it waits for samples, read() asks the backend alive() every
// kAlivePollMs - not on every 2 ms turn of its wait.
void testAlivePollInterval() {
    auto fake = std::make_shared<FakeBackend>();
    fake->devices = {stereoCard("USB Audio CODEC", "Windows WASAPI", 0, ratesUpTo192k())};
    SoundCardSource src = makeSource(fake);
    CHECK(src.openWith(iqSettings("USB Audio CODEC", 48000.0)));
    CHECK(src.start());
    const int before = fake->aliveCalls.load();
    std::vector<std::complex<float>> buf(512);
    const auto t0 = Clock::now();
    while (Clock::now() - t0 < std::chrono::milliseconds(1000)) { (void)src.read(buf.data(), buf.size()); }
    const int calls = fake->aliveCalls.load() - before;
    std::printf("alive poll: %d calls in 1 s of waiting (every %lld ms)\n", calls,
                static_cast<long long>(SoundCardSource::kAlivePollMs.count()));
    CHECK(!src.faulted());
    CHECK(calls >= 2 && calls <= 5);
}

// R7: the config's rate bounds, the LOWER one too.
void testConfigRateBounds() {
    using cascade::core::AppConfig;
    using cascade::core::ConfigStore;
    const std::string path = tempPath("bounds");
    for (const double rate : {4000.0, 7999.0, 8000.0, 768000.0, 768001.0}) {
        AppConfig cfg;
        cfg.soundCard.rateHz = rate;
        std::string err;
        CHECK(ConfigStore::save(path, cfg, err));
        AppConfig back;
        CHECK(ConfigStore::load(path, back, err));
        const bool inRange = rate >= 8000.0 && rate <= 768000.0;
        CHECK_NEAR(back.soundCard.rateHz, inRange ? rate : 48000.0, 0.0);
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// Item 3: every message about a card that is missing or has stopped gives the
// same, true, advice - restart FoxSDR - and none of them "press Open".
void testRecoveryAdvice() {
    std::vector<std::string> lines;
    {
        auto fake = std::make_shared<FakeBackend>();
        fake->devices = {stereoCard("Mic", "Windows WASAPI", 0, ratesUpTo192k())};
        SoundCardSource src = makeSource(fake);
        CHECK(!src.openWith(iqSettings("USB Audio CODEC", 48000.0)));
        lines.push_back(src.lastError());
    }
    {
        auto fake = std::make_shared<FakeBackend>();
        fake->devices = {stereoCard("USB Audio CODEC", "Windows WASAPI", 0, ratesUpTo192k())};
        SoundCardSource src = makeSource(fake);
        CHECK(src.openWith(iqSettings("USB Audio CODEC", 48000.0)));
        CHECK(src.start());
        fake->aliveFlag = false;
        std::vector<std::complex<float>> buf(512);
        const auto t0 = Clock::now();
        while (!src.faulted() && Clock::now() - t0 < std::chrono::seconds(3)) { (void)src.read(buf.data(), buf.size()); }
        lines.push_back(src.lastError());
    }
    {
        auto fake = std::make_shared<FakeBackend>();
        fake->devices = {stereoCard("USB Audio CODEC", "Windows WASAPI", 0, ratesUpTo192k())};
        SoundCardSource src = makeSource(fake);
        CHECK(src.openWith(iqSettings("USB Audio CODEC", 48000.0)));
        CHECK(src.start());
        std::vector<std::complex<float>> buf(512);
        const auto t0 = Clock::now();
        while (!src.faulted() && Clock::now() - t0 < std::chrono::seconds(4)) { (void)src.read(buf.data(), buf.size()); }
        lines.push_back(src.lastError());
    }
    CHECK(lines.size() == 3);
    for (const std::string& l : lines) {
        std::printf("advice: \"%s\"\n", l.c_str());
        CHECK(l.find("restart FoxSDR") != std::string::npos);
        CHECK(l.find("press Open") == std::string::npos);
    }
}

// Items 6 and 7: the patch page takes the receiver's sound card as it takes a
// radio, under the key its device list gives that card; the centre box moves
// only a card RUNNING in I/Q mode.
void testPatchLoanAndCentreBox() {
    using cascade::gui::receiverSourceForPatch;
    SoundCardSettings card;
    card.device = "Line In (2- USB, rev|2)";
    card.hostApi = "Windows WASAPI";
    card.format = SoundCardFormat::IqStereo;
    const auto l = receiverSourceForPatch(true, "soundcard", false, false, "", false, card);
    CHECK(l.take);
    CHECK(l.kind == "soundcard");
    // The same key the patch's device list offers for that card, so a patch
    // radio already on it is on the lent card and not a second stream.
    const std::string listed = cascade::core::patch::makeDeviceKey(
        "soundcard", cascade::source::soundCardDeviceArgs(card.device, card.hostApi));
    CHECK(cascade::core::patch::makeDeviceKey(l.kind, l.args) == listed);
    // ...and it opens there as the Source section has it set up.
    SoundCardSettings opened;
    CHECK(cascade::source::parseSoundCardArgs(cascade::gui::soundCardArgsForPatch(l.args, card), opened));
    CHECK(opened.format == SoundCardFormat::IqStereo && opened.device == card.device);
    // Nothing while the patch is stopped, while an open is resolving, or from
    // a file or the generator; a radio as before.
    CHECK(!receiverSourceForPatch(false, "soundcard", false, false, "", false, card).take);
    CHECK(!receiverSourceForPatch(true, "soundcard", false, false, "", true, card).take);
    CHECK(!receiverSourceForPatch(true, "soundcard", false, true, "", false, card).take);
    CHECK(!receiverSourceForPatch(true, "siggen", false, false, "", false, card).take);
    CHECK(!receiverSourceForPatch(true, "file", false, false, "", false, card).take);
    const auto r = receiverSourceForPatch(true, "rtlsdr", true, false, "serial=1", false, card);
    CHECK(r.take && r.kind == "rtlsdr" && r.args == "serial=1");
    CHECK(!receiverSourceForPatch(true, "rtlsdr", true, true, "serial=1", false, card).take);

    using cascade::gui::soundCardCentreAppliesLive;
    CHECK(soundCardCentreAppliesLive(true, false, SoundCardFormat::IqStereo));
    CHECK(!soundCardCentreAppliesLive(true, false, SoundCardFormat::RealMono));  // the review's case
    CHECK(!soundCardCentreAppliesLive(false, false, SoundCardFormat::IqStereo));
    CHECK(!soundCardCentreAppliesLive(true, true, SoundCardFormat::IqStereo));
}

// --- 11. a close that never returns ----------------------------------------------------

// Runs `f` on a thread of its own and says whether it finished within `bound`.
// The future is kept (a std::async future's destructor waits), so a step that
// hung is joined only after the test has released whatever hung it.
struct Bounded {
    std::future<void> fut;
    bool finished = false;
    double ms = 0.0;
};

Bounded runBounded(std::function<void()> f, std::chrono::milliseconds bound) {
    Bounded b;
    const auto t0 = Clock::now();
    b.fut = std::async(std::launch::async, std::move(f));
    b.finished = b.fut.wait_for(bound) == std::future_status::ready;
    b.ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    return b;
}

// A BACKEND WHOSE CLOSE NEVER RETURNS, with the lock a real backend has:
// open, list and close all take this backend's own mutex, and the close holds
// it while it hangs - which is what 6b5aead's PortAudio backend did (with a
// lock shared by every backend). A source that closes on the caller's thread,
// or opens again on the backend its close took, waits on that lock for ever.
class HangingBackend final : public SoundCardBackend {
public:
    struct Gate {
        std::mutex m;
        std::condition_variable cv;
        bool released = false;
        std::atomic<int> hung{0};    // closes that are hanging now
        std::atomic<int> closed{0};  // closes that have returned
    };

    // `answersAlive` false: a card the host API already says has stopped - one
    // pulled out while the receiver was stopped, which no read() noticed.
    HangingBackend(std::shared_ptr<Gate> gate, bool hangOnClose, std::vector<SoundCardDevice> devices,
                   bool answersAlive = true)
        : gate_(std::move(gate)), hang_(hangOnClose), answersAlive_(answersAlive), devices_(std::move(devices)) {}

    std::vector<SoundCardDevice> listDevices() override {
        std::lock_guard<std::mutex> lk(m_);
        return devices_;
    }
    bool open(const SoundCardDevice&, int, double, bool, PushFn push, void* user, std::string& error) override {
        std::lock_guard<std::mutex> lk(m_);
        if (open_) {
            error = "already open";
            return false;
        }
        push_ = push;
        user_ = user;
        open_ = true;
        return true;
    }
    void close() override {
        std::lock_guard<std::mutex> lk(m_);
        if (!open_) { return; }
        if (hang_) {
            ++gate_->hung;
            std::unique_lock<std::mutex> g(gate_->m);
            gate_->cv.wait(g, [this] { return gate_->released; });
            --gate_->hung;
        }
        open_ = false;
        ++gate_->closed;
    }
    bool alive() override {
        std::unique_lock<std::mutex> lk(m_, std::try_to_lock);
        return !lk.owns_lock() || (open_ && answersAlive_);
    }
    void push(const float* x, std::size_t frames) {
        if (push_ != nullptr) { push_(user_, x, frames); }
    }

private:
    std::shared_ptr<Gate> gate_;
    bool hang_;
    bool answersAlive_;
    std::vector<SoundCardDevice> devices_;
    std::mutex m_;
    bool open_ = false;
    PushFn push_ = nullptr;
    void* user_ = nullptr;
};

void releaseGate(HangingBackend::Gate& g) {
    {
        std::lock_guard<std::mutex> lk(g.m);
        g.released = true;
    }
    g.cv.notify_all();
}

// Item 1, at the source: a switch, a destroy and a new open all finish within
// a bound while a close hangs, and the hung close is left on its own thread.
void testHungCloseAtTheSource() {
    const std::vector<SoundCardDevice> devices = {
        stereoCard("USB Audio CODEC", "Windows WASAPI", 0, ratesUpTo192k())};
    const auto bound = SoundCardSource::kCloseWaitMs + std::chrono::milliseconds(700);

    // (a) A HEALTHY card whose close hangs: destroying the source costs at
    // most kCloseWaitMs, the close is counted as left behind, and the SAME
    // source object opens again at once on a fresh backend.
    {
        auto gate = std::make_shared<HangingBackend::Gate>();
        std::vector<std::shared_ptr<HangingBackend>> made;
        std::mutex madeM;
        auto src = std::make_unique<SoundCardSource>([&]() -> std::shared_ptr<SoundCardBackend> {
            std::lock_guard<std::mutex> lk(madeM);
            // Only the FIRST backend hangs on close.
            made.push_back(std::make_shared<HangingBackend>(gate, made.empty(), devices));
            return made.back();
        });
        CHECK(src->openWith(iqSettings("USB Audio CODEC", 48000.0)));
        CHECK(src->start());
        const std::uint64_t abandonedBefore = SoundCardSource::abandonedCloses();
        // The source is reopened (a new rate is the same path: close, open).
        Bounded reopen = runBounded([&] { (void)src->openWith(iqSettings("USB Audio CODEC", 96000.0)); }, bound);
        std::printf("hung close: a reopen of the same source took %.0f ms (%s)\n", reopen.ms,
                    reopen.finished ? "finished" : "STILL WAITING");
        CHECK(reopen.finished);
        CHECK(gate->hung.load() == 1);
        CHECK(SoundCardSource::abandonedCloses() == abandonedBefore + 1);
        if (!reopen.finished) {
            // RED: the reopen is stuck behind the hung close. Let it go before
            // anything else touches the source.
            releaseGate(*gate);
            reopen.fut.get();
        }
        if (reopen.finished) {
            CHECK(src->isOpen());
            CHECK(src->start());
            {
                std::lock_guard<std::mutex> lk(madeM);
                CHECK(made.size() == 2);  // a fresh backend, not the one the close took
                const float frame[2] = {0.5f, 0.0f};
                if (made.size() == 2) { made[1]->push(frame, 1); }
            }
            std::vector<std::complex<float>> buf(16);
            CHECK(src->read(buf.data(), buf.size()) == 1);
        }
        // Destroying it (a source switch, the exit) is prompt too: its own
        // backend's close does not hang.
        Bounded destroy = runBounded([&] { src.reset(); }, bound);
        std::printf("hung close: destroying the reopened source took %.0f ms\n", destroy.ms);
        CHECK(destroy.finished);
        releaseGate(*gate);
        if (reopen.fut.valid()) { reopen.fut.get(); }
        if (destroy.fut.valid()) { destroy.fut.get(); }
        const auto t0 = Clock::now();
        while (gate->closed.load() < 2 && Clock::now() - t0 < std::chrono::seconds(3)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(gate->closed.load() == 2);  // the hung close finished once released
    }

    // (b) The destroy itself, with the close hanging: bounded by kCloseWaitMs.
    {
        auto gate = std::make_shared<HangingBackend::Gate>();
        auto src = std::make_unique<SoundCardSource>([&]() -> std::shared_ptr<SoundCardBackend> {
            return std::make_shared<HangingBackend>(gate, true, devices);
        });
        CHECK(src->openWith(iqSettings("USB Audio CODEC", 48000.0)));
        Bounded destroy = runBounded([&] { src.reset(); }, bound);
        std::printf("hung close: destroying a healthy source took %.0f ms (budget %lld ms)\n", destroy.ms,
                    static_cast<long long>(SoundCardSource::kCloseWaitMs.count()));
        CHECK(destroy.finished);
        CHECK(destroy.ms >= static_cast<double>(SoundCardSource::kCloseWaitMs.count()) - 20.0);
        releaseGate(*gate);
        if (destroy.fut.valid()) { destroy.fut.get(); }
    }

    // (c) A card that has FAILED is not waited for at all - through the
    // pipeline, as the application swaps it out.
    {
        auto gate = std::make_shared<HangingBackend::Gate>();
        std::shared_ptr<HangingBackend> dead;
        auto src = std::make_unique<SoundCardSource>([&]() -> std::shared_ptr<SoundCardBackend> {
            dead = std::make_shared<HangingBackend>(gate, true, devices);
            return dead;
        });
        CHECK(src->openWith(iqSettings("USB Audio CODEC", 48000.0)));
        cascade::core::Pipeline::Config cfg;
        cfg.audioEnabled = false;
        cascade::core::Pipeline p(cfg);
        p.setSource(std::move(src));
        CHECK(p.setInputRateHz(p.activeSource().sampleRateHz()));
        p.start();
        // Nothing arrives: the stall latches the card dead.
        const auto t0 = Clock::now();
        while (!p.faulted() && Clock::now() - t0 < std::chrono::seconds(4)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        CHECK(p.faulted());
        p.stop();
        Bounded swap = runBounded([&] { p.setSource(nullptr); }, bound);
        std::printf("hung close: swapping out a dead card took %.0f ms\n", swap.ms);
        CHECK(swap.finished);
        CHECK(swap.ms < 300.0);
        releaseGate(*gate);
        if (swap.fut.valid()) { swap.fut.get(); }
    }

    // (d) The teardown switch: once the application's exit has begun no
    // close is waited for, healthy or not.
    {
        auto gate = std::make_shared<HangingBackend::Gate>();
        auto src = std::make_unique<SoundCardSource>([&]() -> std::shared_ptr<SoundCardBackend> {
            return std::make_shared<HangingBackend>(gate, true, devices);
        });
        CHECK(src->openWith(iqSettings("USB Audio CODEC", 48000.0)));
        SoundCardSource::setCloseWaitEnabled(false);
        Bounded destroy = runBounded([&] { src.reset(); }, bound);
        SoundCardSource::setCloseWaitEnabled(true);
        std::printf("hung close: destroying at exit took %.0f ms\n", destroy.ms);
        CHECK(destroy.finished);
        CHECK(destroy.ms < 300.0);
        releaseGate(*gate);
        if (destroy.fut.valid()) { destroy.fut.get(); }
    }
}

// --- 12. the REAL PortAudio backend, over a scripted PortAudio ------------------------

namespace fakepa {

std::vector<PaHostApiInfo> apis;
std::vector<PaDeviceInfo> devs;
std::vector<std::string> names;  // storage behind devs[i].name
PaDeviceIndex defaultIn = paNoDevice;
std::atomic<int> inits{0};
std::atomic<int> openCalls{0};
std::atomic<int> closeCalls{0};
std::atomic<int> listCalls{0};

struct Stream {
    PaStreamCallback* cb = nullptr;
    void* user = nullptr;
    std::atomic<bool> active{false};
    std::atomic<bool> open{false};
    int device = -1;
    bool exclusive = false;
};
constexpr int kStreams = 256;
Stream streams[kStreams];
int streamIds[kStreams];
std::atomic<int> nextStream{0};

// The stream whose abort, close or activity query never returns until the
// gate is opened.
std::atomic<void*> hangOn{nullptr};
std::atomic<void*> hangCloseOn{nullptr};
std::atomic<void*> hangActiveOn{nullptr};
std::mutex gateM;
std::condition_variable gateCv;
bool released = false;
std::atomic<int> hung{0};

void armGate() {
    std::lock_guard<std::mutex> lk(gateM);
    released = false;
}
void openGate() {
    {
        std::lock_guard<std::mutex> lk(gateM);
        released = true;
    }
    gateCv.notify_all();
}
void waitAtGate() {
    ++hung;
    std::unique_lock<std::mutex> lk(gateM);
    gateCv.wait(lk, [] { return released; });
    --hung;
}

// THE WASAPI RULE, exactly as the review's probe (probe_reopen.cpp) models
// it: IAudioClient::Initialize answers AUDCLNT_E_DEVICE_IN_USE - "the device
// is being used in exclusive mode, or the device is being used in shared mode
// and the caller asked to use the device in exclusive mode" - and PortAudio's
// CreateAudioClient returns paInvalidDevice for any Initialize failure. A
// device is held from its open until its close RETURNS (the host API's close
// is what releases the audio client).
std::mutex useM;
int sharedUse[kStreams];
int exclusiveUse[kStreams];
int refuseNext = 0;               // the next N opens are refused outright
bool lastOpenExclusive = false;   // under useM
double lastOpenRateHz = 0.0;      // under useM

void resetUse() {
    std::lock_guard<std::mutex> lk(useM);
    for (int i = 0; i < kStreams; ++i) { sharedUse[i] = exclusiveUse[i] = 0; }
    refuseNext = 0;
    lastOpenExclusive = false;
    lastOpenRateHz = 0.0;
}
int streamsInUse() {
    std::lock_guard<std::mutex> lk(useM);
    int n = 0;
    for (int i = 0; i < kStreams; ++i) { n += sharedUse[i] + exclusiveUse[i]; }
    return n;
}
bool lastExclusive() {
    std::lock_guard<std::mutex> lk(useM);
    return lastOpenExclusive;
}
// Waits (bounded) for closes still finishing on their closer threads.
int streamsInUseSettled() {
    const auto t0 = Clock::now();
    while (streamsInUse() != 0 && Clock::now() - t0 < std::chrono::seconds(3)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return streamsInUse();
}

// THE LIST PROBE (item 4 of the second review). PortAudio adds a stream to
// its unlocked open-stream list as the LAST act of Pa_OpenStream and removes
// it as the FIRST act of Pa_CloseStream (pa_front.c). With the probe on, each
// of those is a short section that counts any other thread found inside it.
std::atomic<bool> listProbe{false};
std::atomic<int> inList{0};
std::atomic<int> listOverlaps{0};
void listSection() {
    if (!listProbe.load()) { return; }
    if (++inList > 1) { ++listOverlaps; }
    std::this_thread::sleep_for(std::chrono::microseconds(300));
    --inList;
}

// Whether the stream `watchTerminate` names was still open when a backend
// dropped its PortAudio initialisation (-1: no terminate seen).
std::atomic<void*> watchTerminate{nullptr};
std::atomic<int> watchedOpenAtTerminate{-1};

int indexOf(PaStream* s) { return static_cast<int>(static_cast<int*>(s) - streamIds); }

PaError initialize() {
    ++inits;
    return paNoError;
}
PaError terminate() {
    void* w = watchTerminate.load();
    if (w != nullptr) { watchedOpenAtTerminate = streams[indexOf(static_cast<PaStream*>(w))].open.load() ? 1 : 0; }
    --inits;
    return paNoError;
}
PaDeviceIndex getDeviceCount() {
    ++listCalls;
    return static_cast<PaDeviceIndex>(devs.size());
}
PaDeviceIndex getDefaultInputDevice() { return defaultIn; }
const PaDeviceInfo* getDeviceInfo(PaDeviceIndex i) {
    return (i >= 0 && i < static_cast<PaDeviceIndex>(devs.size())) ? &devs[static_cast<std::size_t>(i)] : nullptr;
}
const PaHostApiInfo* getHostApiInfo(PaHostApiIndex h) {
    return (h >= 0 && h < static_cast<PaHostApiIndex>(apis.size())) ? &apis[static_cast<std::size_t>(h)] : nullptr;
}
PaError isFormatSupported(const PaStreamParameters* in, const PaStreamParameters*, double rate) {
    // Shared mode: 48 kHz. Exclusive (a WASAPI stream-info block): 192 kHz.
    const bool excl = in != nullptr && in->hostApiSpecificStreamInfo != nullptr;
    return (excl ? rate == 192000.0 : rate == 48000.0) ? paFormatIsSupported : paInvalidSampleRate;
}
PaError openStream(PaStream** s, const PaStreamParameters* in, const PaStreamParameters*, double rate,
                   unsigned long, PaStreamFlags, PaStreamCallback* cb, void* user) {
    ++openCalls;
    // The host API's own open, before the list is touched.
    if (listProbe.load()) { std::this_thread::sleep_for(std::chrono::microseconds(300)); }
    const bool excl = in != nullptr && in->hostApiSpecificStreamInfo != nullptr;
    const int dev = in != nullptr ? static_cast<int>(in->device) : -1;
    {
        std::lock_guard<std::mutex> lk(useM);
        lastOpenExclusive = excl;
        lastOpenRateHz = rate;
        if (refuseNext > 0) {
            --refuseNext;
            return paInvalidDevice;
        }
        if (dev < 0 || dev >= kStreams) { return paInvalidDevice; }
        if (exclusiveUse[dev] > 0 || (excl && sharedUse[dev] > 0)) { return paInvalidDevice; }
        (excl ? exclusiveUse : sharedUse)[dev]++;
    }
    const int k = nextStream++ % kStreams;
    streams[k].cb = cb;
    streams[k].user = user;
    streams[k].device = dev;
    streams[k].exclusive = excl;
    streams[k].active = true;
    streams[k].open = true;
    *s = &streamIds[k];
    listSection();  // AddOpenStream, the last thing Pa_OpenStream does
    return paNoError;
}
PaError startStream(PaStream*) { return paNoError; }
PaError abortStream(PaStream* s) {
    if (s == hangOn.load()) { waitAtGate(); }
    streams[indexOf(s)].active = false;
    return paNoError;
}
PaError closeStream(PaStream* s) {
    ++closeCalls;
    listSection();  // RemoveOpenStream, the first thing Pa_CloseStream does
    if (s == hangCloseOn.load()) { waitAtGate(); }
    Stream& st = streams[indexOf(s)];
    {
        std::lock_guard<std::mutex> lk(useM);
        if (st.open.load() && st.device >= 0) { (st.exclusive ? exclusiveUse : sharedUse)[st.device]--; }
    }
    st.active = false;
    st.open = false;
    return paNoError;
}
PaError isStreamActive(PaStream* s) {
    if (s == hangActiveOn.load()) { waitAtGate(); }
    return streams[indexOf(s)].active.load() ? 1 : 0;
}
const char* getErrorText(PaError) { return "scripted PortAudio error"; }

const cascade::source::SoundCardPaApi kApi = {
    &initialize,      &terminate,  &getDeviceCount, &getDefaultInputDevice, &getDeviceInfo,
    &getHostApiInfo,  &isFormatSupported, &openStream, &startStream, &abortStream,
    &closeStream,     &isStreamActive, &getErrorText,
};

void addApi(PaHostApiTypeId type, const char* name) {
    PaHostApiInfo a{};
    a.structVersion = 1;
    a.type = type;
    a.name = name;
    a.defaultInputDevice = paNoDevice;
    a.defaultOutputDevice = paNoDevice;
    apis.push_back(a);
}

void addDevice(const char* name, PaHostApiIndex api, int inputs, bool apiDefault = false) {
    PaDeviceInfo d{};
    d.structVersion = 2;
    d.hostApi = api;
    d.maxInputChannels = inputs;
    d.maxOutputChannels = inputs == 0 ? 2 : 0;
    d.defaultHighInputLatency = 0.1;
    d.defaultSampleRate = 48000.0;
    names.emplace_back(name);
    devs.push_back(d);
    if (apiDefault) { apis[static_cast<std::size_t>(api)].defaultInputDevice = static_cast<PaDeviceIndex>(devs.size() - 1); }
}

// Names are pointed at AFTER every push_back, when neither vector moves again.
void seal() {
    for (std::size_t i = 0; i < devs.size(); ++i) { devs[i].name = names[i].c_str(); }
    for (auto& a : apis) { a.deviceCount = 0; }
    for (const auto& d : devs) { ++apis[static_cast<std::size_t>(d.hostApi)].deviceCount; }
}

// What a Windows machine with one USB codec lists, under every host API
// PortAudio has - plus ALSA's entries for the same codec, so the rule is seen
// to keep Linux's hardware input and drop its configured PCMs. The ALSA names
// are in the form PortAudio really gives them: "<card>: <pcm> (hw:N,M)" for
// hardware (pa_linux_alsa.c, BuildDeviceList) and the bare configuration id
// for everything else.
void scriptWindowsMachine() {
    apis.clear();
    devs.clear();
    names.clear();
    addApi(paMME, "MME");
    addApi(paDirectSound, "Windows DirectSound");
    addApi(paWASAPI, "Windows WASAPI");
    addApi(paWDMKS, "Windows WDM-KS");
    addApi(paALSA, "ALSA");
    addDevice("Microsoft Sound Mapper - Input", 0, 2, true);   // 0
    addDevice("USB Audio CODEC", 0, 2);                        // 1
    addDevice("Primary Sound Capture Driver", 1, 2, true);     // 2
    addDevice("Microphone (USB Audio CODEC)", 1, 2);           // 3
    addDevice("Microphone (USB Audio CODEC)", 2, 2, true);     // 4 <- WASAPI
    addDevice("Line (USB Audio CODEC)", 3, 2);                 // 5
    addDevice("USB Audio CODEC: USB Audio (hw:1,0)", 4, 2);    // 6 <- ALSA hardware
    addDevice("Speakers (USB Audio CODEC)", 2, 0);             // 7 output only
    addDevice("sysdefault", 4, 2);                             // 8 ALSA configured PCMs:
    addDevice("pulse", 4, 32);                                 // 9   the system default,
    addDevice("dsnoop", 4, 2);                                 // 10  a shared capture,
    addDevice("default", 4, 32, true);                         // 11  PortAudio's ALSA default
    seal();
    // PortAudio's own default input on Windows is MME's - the mapper.
    defaultIn = 0;
}

}  // namespace fakepa

// Item 2: only host APIs whose inputs keep their identity are offered, and a
// card saved from an MME entry is NOT FOUND - never substituted.
void testHostApiFilter() {
    fakepa::scriptWindowsMachine();
    auto b = cascade::source::makePortAudioSoundCardBackend(fakepa::kApi);
    const std::vector<SoundCardDevice> list = b->listDevices();
    for (const SoundCardDevice& d : list) {
        std::printf("scripted machine lists: [%d] %s\n", d.index, cascade::source::soundCardDeviceLabel(d).c_str());
    }
    CHECK(list.size() == 2);
    const bool wasapiOnly = list.size() == 2 && list[0].index == 4 && list[0].hostApi == "Windows WASAPI" &&
                            list[1].index == 6 && list[1].hostApi == "ALSA";
    CHECK(wasapiOnly);
    for (const SoundCardDevice& d : list) {
        CHECK(d.name.find("Mapper") == std::string::npos);
        CHECK(d.name.find("Primary Sound") == std::string::npos);
        CHECK(d.hostApi != "MME" && d.hostApi != "Windows DirectSound" && d.hostApi != "Windows WDM-KS");
        // ALSA's configured PCMs follow the system default or share another
        // device: only the hardware input is offered (second review, item 2).
        CHECK(d.name != "default" && d.name != "pulse" && d.name != "sysdefault" && d.name != "dsnoop");
    }
    // The WASAPI entry's rates: 48 kHz shared and - on Windows - 192 kHz
    // EXCLUSIVE, the rate a VLF card's own hardware runs at. (The second
    // review's X5: dropping the exclusive probe from the list went unseen.)
    if (!list.empty()) {
        CHECK(!list[0].rates.empty() && list[0].rates[0].hz == 48000.0 && !list[0].rates[0].exclusive);
#ifdef _WIN32
        CHECK(list[0].rates.size() == 2 && list[0].rates.back().hz == 192000.0 && list[0].rates.back().exclusive);
#else
        CHECK(list[0].rates.size() == 1);
#endif
    }
    // PortAudio's own default is the MME mapper, which is not offered; the
    // default row is WASAPI's own default input instead.
    CHECK(cascade::source::findSoundCard(list, "", "") == 0);

    // A card saved by an earlier build from its MME entry: not connected, and
    // nothing at all opened in its place.
    const int opensBefore = fakepa::openCalls.load();
    SoundCardSource src([] { return cascade::source::makePortAudioSoundCardBackend(fakepa::kApi); });
    SoundCardSettings mme;
    mme.device = "USB Audio CODEC";
    mme.hostApi = "MME";
    CHECK(!src.openWith(mme));
    CHECK(fakepa::openCalls.load() == opensBefore);
    CHECK(std::string(src.lastError()).find("not connected") != std::string::npos);
    SoundCardSettings mapper;
    mapper.device = "Microsoft Sound Mapper - Input";
    mapper.hostApi = "MME";
    CHECK(!src.openWith(mapper));
    CHECK(fakepa::openCalls.load() == opensBefore);
    // ...and the backend itself refuses an MME entry handed to it directly,
    // whatever list it came from.
    SoundCardDevice stale;
    stale.index = 1;
    stale.name = "USB Audio CODEC";
    stale.hostApi = "MME";
    stale.maxInputChannels = 2;
    std::string err;
    auto b2 = cascade::source::makePortAudioSoundCardBackend(fakepa::kApi);
    CHECK(!b2->open(stale, 2, 48000.0, false, &SoundCardSource::pushFrames, nullptr, err));
    CHECK(fakepa::openCalls.load() == opensBefore);
    // ...and an index that IS a WASAPI input but not the one the name says:
    // the name behind the index is checked, so a stale index can never open
    // a different card (the second review's X4 - nothing reached this).
    SoundCardDevice wrongName;
    wrongName.index = 4;  // "Microphone (USB Audio CODEC)" under WASAPI
    wrongName.name = "Line In (Another Card)";
    wrongName.hostApi = "Windows WASAPI";
    wrongName.maxInputChannels = 2;
    auto b3 = cascade::source::makePortAudioSoundCardBackend(fakepa::kApi);
    err.clear();
    CHECK(!b3->open(wrongName, 2, 48000.0, false, &SoundCardSource::pushFrames, nullptr, err));
    CHECK(fakepa::openCalls.load() == opensBefore);
    CHECK(err.find("no longer in the list") != std::string::npos);
    // The pure rule.
    CHECK(!cascade::source::soundCardHostApiListed(paMME));
    CHECK(!cascade::source::soundCardHostApiListed(paDirectSound));
    CHECK(cascade::source::soundCardHostApiListed(paWASAPI));
    CHECK(cascade::source::soundCardHostApiListed(paALSA));
}

// Item 1, in the REAL backend: while one card's abort hangs inside the host
// API, another backend enumerates, a new source opens and streams, and the
// hung source's destroy is bounded - so no lock the close holds is one that
// construction, listing or opening takes. (6b5aead's backend took ONE lock for
// all of them, and its close held it through Pa_AbortStream.)
void testRealBackendHungAbort() {
    fakepa::scriptWindowsMachine();
    const auto bound = SoundCardSource::kCloseWaitMs + std::chrono::milliseconds(700);
    const auto factory = [] { return cascade::source::makePortAudioSoundCardBackend(fakepa::kApi); };
    const int initsBefore = fakepa::inits.load();

    // A backend made on this thread calls nothing.
    {
        auto idle = cascade::source::makePortAudioSoundCardBackend(fakepa::kApi);
        CHECK(fakepa::inits.load() == initsBefore);
    }

    auto a = std::make_unique<SoundCardSource>(factory);
    CHECK(a->openWith(iqSettings("Microphone (USB Audio CODEC)", 48000.0)));
    CHECK(a->start());
    const int aStream = (fakepa::nextStream.load() - 1) % fakepa::kStreams;
    fakepa::hangOn = &fakepa::streamIds[aStream];

    // The source is destroyed (a switch, the exit) and its abort hangs.
    Bounded destroyA = runBounded([&] { a.reset(); }, bound);
    std::printf("real backend: destroying a source whose abort hangs took %.0f ms\n", destroyA.ms);
    CHECK(destroyA.finished);
    CHECK(fakepa::hung.load() == 1);

    // With that abort still hung: another backend enumerates...
    Bounded list = runBounded(
        [&] {
            auto b = cascade::source::makePortAudioSoundCardBackend(fakepa::kApi);
            CHECK(b->listDevices().size() == 2);
        },
        std::chrono::milliseconds(700));
    std::printf("real backend: an enumeration beside the hung abort took %.0f ms (%s)\n", list.ms,
                list.finished ? "finished" : "STILL WAITING");
    CHECK(list.finished);

    // ...a new source is made, opened and streams...
    std::unique_ptr<SoundCardSource> c;
    Bounded openC = runBounded(
        [&] {
            c = std::make_unique<SoundCardSource>(factory);
            CHECK(c->openWith(iqSettings("Microphone (USB Audio CODEC)", 48000.0)));
            CHECK(c->start());
        },
        std::chrono::milliseconds(700));
    std::printf("real backend: a new open beside the hung abort took %.0f ms (%s)\n", openC.ms,
                openC.finished ? "finished" : "STILL WAITING");
    CHECK(openC.finished);
    if (openC.finished && c && c->isOpen()) {
        const int k = (fakepa::nextStream.load() - 1) % fakepa::kStreams;
        const float frame[2] = {0.5f, 0.0f};
        fakepa::streams[k].cb(frame, nullptr, 1, nullptr, 0, fakepa::streams[k].user);
        std::vector<std::complex<float>> buf(16);
        CHECK(c->read(buf.data(), buf.size()) == 1);
        // ...and a switch away from it through a pipeline is prompt.
        cascade::core::Pipeline::Config cfg;
        cfg.audioEnabled = false;
        cascade::core::Pipeline p(cfg);
        p.setSource(std::move(c));
        Bounded swap = runBounded([&] { p.setSource(nullptr); }, bound);
        std::printf("real backend: a source switch beside the hung abort took %.0f ms\n", swap.ms);
        CHECK(swap.finished);
        CHECK(swap.ms < 500.0);
        {
            std::lock_guard<std::mutex> lk(fakepa::gateM);
            fakepa::released = true;
        }
        fakepa::gateCv.notify_all();
        if (swap.fut.valid()) { swap.fut.get(); }
    }
    // Release the hang, and everything that waited behind it finishes.
    {
        std::lock_guard<std::mutex> lk(fakepa::gateM);
        fakepa::released = true;
    }
    fakepa::gateCv.notify_all();
    if (destroyA.fut.valid()) { destroyA.fut.get(); }
    if (list.fut.valid()) { list.fut.get(); }
    if (openC.fut.valid()) { openC.fut.get(); }
    c.reset();
    // Every backend's initialisation is returned once its close has: the
    // hung one's too, now that it has finished.
    const auto t0 = Clock::now();
    while ((fakepa::hung.load() != 0 || fakepa::inits.load() != initsBefore) &&
           Clock::now() - t0 < std::chrono::seconds(3)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(fakepa::hung.load() == 0);
    CHECK(fakepa::inits.load() == initsBefore);
    fakepa::hangOn = nullptr;
}

// --- 13. the second review ------------------------------------------------------------

std::string readSource(const std::string& rel) {
    const std::string p = std::string(CASCADE_SOURCE_DIR) + "/" + rel;
    std::FILE* f = std::fopen(p.c_str(), "rb");
    if (f == nullptr) { return {}; }
    std::string text;
    char buf[65536];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) { text.append(buf, n); }
    std::fclose(f);
    return text;
}

std::unique_ptr<SoundCardSource> fakePaSource() {
    return std::make_unique<SoundCardSource>([] { return cascade::source::makePortAudioSoundCardBackend(fakepa::kApi); });
}

// Item 1: RE-OPENING THE SAME CARD. The Source section's Open used to open
// the new stream beside the running one and drop the old one after; Windows
// refuses that whenever exclusive mode is involved (the review's probe_reopen:
// three of four everyday changes on a 192 kHz card REFUSED). Every row of the
// review's table must now open, in the application's order - and the old
// order is run too, as the control that proves the scripted WASAPI rule is
// really being enforced.
void testSameCardReopen() {
    fakepa::scriptWindowsMachine();
    fakepa::resetUse();
    const auto factory = [] { return cascade::source::makePortAudioSoundCardBackend(fakepa::kApi); };
    const std::vector<SoundCardDevice> list = factory()->listDevices();
    CHECK(!list.empty());
    if (list.empty()) { return; }
#ifdef _WIN32
    const bool windows = true;
#else
    const bool windows = false;  // no exclusive mode: every rate is the shared 48 kHz
#endif
    SoundCardSettings base;
    base.device = list[0].name;  // "Microphone (USB Audio CODEC)", WASAPI
    base.hostApi = list[0].hostApi;
    base.format = SoundCardFormat::RealMono;
    base.pickedFromList = true;

    struct Row {
        const char* what;
        double fromHz;
        int fromCh;
        double toHz;
        int toCh;
    };
    const Row rows[] = {
        {"192k excl, left -> right", 192000.0, 0, 192000.0, 1},
        {"48k shared -> 192k excl", 48000.0, 0, 192000.0, 0},
        {"192k excl -> 48k shared", 192000.0, 0, 48000.0, 0},
        {"48k shared, left -> right", 48000.0, 0, 48000.0, 1},
    };
    for (std::size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); ++r) {
        const Row& row = rows[r];
        SoundCardSettings a = base;
        a.cardRateHz = row.fromHz;
        a.channel = row.fromCh;
        SoundCardSettings b = a;
        b.cardRateHz = row.toHz;
        b.channel = row.toCh;

        // THE CONTROL: the old order, the new card opened beside the running one.
        {
            auto running = fakePaSource();
            CHECK(running->openWith(a, list));
            const auto beside = cascade::source::openSoundCardOrRestore(b, list, nullptr, factory);
            const bool refused = !beside.src;
            std::printf("same card, OLD order | %-26s | %s %s\n", row.what, refused ? "REFUSED" : "opened",
                        refused ? beside.refused.c_str() : "");
            // On Windows exactly the three rows the review found refused are.
            CHECK(refused == (windows && r != 3));
        }
        CHECK(fakepa::streamsInUseSettled() == 0);

        // THE APPLICATION'S ORDER (AppWindow::launchSoundCardOpen): the
        // running card is the pipeline's source; the same card is released
        // first - through the pipeline, as the application does it - and the
        // new settings are opened on a worker with the old ones to fall back on.
        {
            cascade::core::Pipeline::Config cfg;
            cfg.audioEnabled = false;
            cascade::core::Pipeline p(cfg);
            auto running = fakePaSource();
            CHECK(running->openWith(a, list));
            const SoundCardSettings live = running->settings();
            p.setSource(std::move(running));
            CHECK(p.setInputRateHz(p.activeSource().sampleRateHz()));
            p.start();
            const bool release = cascade::gui::soundCardReopenReleasesFirst(true, live, b, list);
            CHECK(release);
            if (release) { p.setSource(nullptr); }
            auto out = std::async(std::launch::async, [&] {
                           return cascade::source::openSoundCardOrRestore(b, list, release ? &live : nullptr,
                                                                          factory);
                       }).get();
            const double expectHz = windows ? row.toHz : 48000.0;
            std::printf("same card, app order | %-26s | %s %s\n", row.what,
                        out.src ? (out.restoredPrevious ? "RESTORED OLD" : "opened") : "REFUSED",
                        out.src ? "" : out.refused.c_str());
            CHECK(out.src != nullptr);
            CHECK(!out.restoredPrevious);
            if (out.src) {
                CHECK(out.src->settings().cardRateHz == expectHz);
                CHECK(out.src->settings().channel == row.toCh);
                // ...in exclusive mode exactly when the rate is an exclusive one
                // (the second review's X6: open() never asking for it went unseen).
                CHECK(fakepa::lastExclusive() == (windows && row.toHz == 192000.0));
                CHECK(fakepa::streamsInUse() == 1);
                p.setSource(std::move(out.src));
            }
            p.stop();
            p.setSource(nullptr);
        }
        CHECK(fakepa::streamsInUseSettled() == 0);
    }

    // THE APPLICATION DOES IT IN THAT ORDER, read from its source (AppWindow
    // is not reachable from a unit test): the decision, then the release
    // through the pipeline, then the worker, which opens with the old
    // settings to fall back on.
    {
        const std::string text = readSource("src/gui/app_window_soundcard.cpp");
        const std::size_t fn = text.find("void AppWindow::launchSoundCardOpen(");
        const std::size_t decide =
            fn == std::string::npos ? std::string::npos : text.find("soundCardReopenReleasesFirst(", fn);
        const std::size_t releaseAt =
            decide == std::string::npos ? std::string::npos : text.find("pipeline_.setSource(nullptr);", decide);
        const std::size_t worker = fn == std::string::npos ? std::string::npos : text.find("std::async(", fn);
        const std::size_t fallback =
            worker == std::string::npos
                ? std::string::npos
                : text.find("openSoundCardOrRestore(settings, r.devices, release ? &previous : nullptr)", worker);
        std::printf("launchSoundCardOpen: decide@%zu release@%zu worker@%zu fallback@%zu\n", decide, releaseAt,
                    worker, fallback);
        CHECK(decide != std::string::npos && releaseAt != std::string::npos && worker != std::string::npos);
        CHECK(decide < releaseAt && releaseAt < worker);
        CHECK(fallback != std::string::npos && fallback - worker < 1500);
    }

    // A DIFFERENT card keeps the other order: the new one opens while the old
    // one runs, and is released only once the new one is in.
    {
        SoundCardDevice other = list[0];
        other.index = 99;
        other.name = "Line In (Realtek Audio)";
        const std::vector<SoundCardDevice> two = {list[0], other};
        SoundCardSettings live = base;
        live.cardRateHz = 48000.0;
        SoundCardSettings wantOther = base;
        wantOther.device = other.name;
        CHECK(!cascade::gui::soundCardReopenReleasesFirst(true, live, wantOther, two));
        CHECK(cascade::gui::soundCardReopenReleasesFirst(true, live, base, two));
        // Only when a sound card is what is running.
        CHECK(!cascade::gui::soundCardReopenReleasesFirst(false, live, base, two));
        // The same card named by a saved (not picked) name resolves to it too.
        SoundCardSettings saved = base;
        saved.pickedFromList = false;
        CHECK(cascade::gui::soundCardReopenReleasesFirst(true, live, saved, two));
    }

    // A REFUSED NEW OPEN RESTORES THE OLD STREAM - never nothing running,
    // silently. The running card (48 kHz, left) is released; the new
    // settings are refused (another program took the card in that instant);
    // the card comes back exactly as it was, and streams.
    {
        cascade::core::Pipeline::Config cfg;
        cfg.audioEnabled = false;
        cascade::core::Pipeline p(cfg);
        SoundCardSettings a = base;
        a.cardRateHz = 48000.0;
        SoundCardSettings b = a;
        b.channel = 1;
        auto running = fakePaSource();
        CHECK(running->openWith(a, list));
        const SoundCardSettings live = running->settings();
        p.setSource(std::move(running));
        CHECK(cascade::gui::soundCardReopenReleasesFirst(true, live, b, list));
        p.setSource(nullptr);
        {
            std::lock_guard<std::mutex> lk(fakepa::useM);
            fakepa::refuseNext = 1;
        }
        auto out = cascade::source::openSoundCardOrRestore(b, list, &live, factory);
        std::printf("same card, new settings refused: %s; refused \"%s\"\n",
                    out.src ? (out.restoredPrevious ? "the old settings are running again" : "opened?")
                            : "NOTHING RUNNING",
                    out.refused.c_str());
        CHECK(out.src != nullptr);
        CHECK(out.restoredPrevious);
        CHECK(!out.refused.empty());
        CHECK(out.previousRefused.empty());
        CHECK(fakepa::streamsInUse() == 1);
        if (out.src) {
            CHECK(out.src->settings().channel == 0);
            CHECK(out.src->settings().cardRateHz == 48000.0);
            CHECK(out.src->start());
            const int k = (fakepa::nextStream.load() - 1) % fakepa::kStreams;
            const float frame[2] = {0.5f, -0.25f};
            fakepa::streams[k].cb(frame, nullptr, 1, nullptr, 0, fakepa::streams[k].user);
            fakepa::streams[k].cb(frame, nullptr, 1, nullptr, 0, fakepa::streams[k].user);
            std::vector<std::complex<float>> buf(16);
            CHECK(out.src->read(buf.data(), buf.size()) == 1);  // two real samples -> one complex
            p.setSource(std::move(out.src));
        }
        p.setSource(nullptr);
        CHECK(fakepa::streamsInUseSettled() == 0);

        // ...and when even the old settings are refused, NOTHING is claimed
        // to be running: both reasons come back.
        {
            std::lock_guard<std::mutex> lk(fakepa::useM);
            fakepa::refuseNext = 2;
        }
        auto none = cascade::source::openSoundCardOrRestore(b, list, &live, factory);
        CHECK(none.src == nullptr);
        CHECK(!none.restoredPrevious);
        CHECK(!none.refused.empty());
        CHECK(!none.previousRefused.empty());
        CHECK(fakepa::streamsInUse() == 0);
        {
            std::lock_guard<std::mutex> lk(fakepa::useM);
            fakepa::refuseNext = 0;
        }
    }
}

// Item 2: ALSA IDENTITY. A card number that moved at boot finds the card; two
// identical cards are refused by name, never guessed between.
void testAlsaIdentity() {
    using cascade::source::AlsaHwName;
    using cascade::source::matchSoundCard;
    using cascade::source::parseAlsaHwName;
    AlsaHwName n;
    CHECK(parseAlsaHwName("USB Audio CODEC: USB Audio (hw:1,0)", n));
    CHECK(n.stripped == "USB Audio CODEC: USB Audio" && n.card == 1 && n.device == 0);
    CHECK(parseAlsaHwName("HDA Intel PCH: ALC892 Analog (plughw:0,2)", n));
    CHECK(n.stripped == "HDA Intel PCH: ALC892 Analog" && n.card == 0 && n.device == 2);
    CHECK(parseAlsaHwName("Card: dev (hw:12,3)", n) && n.card == 12 && n.device == 3);
    for (const char* no : {"default", "pulse", "sysdefault", "dsnoop", "dmix", "hw:CARD=CODEC,DEV=0",
                           "Card: dev (hw:1,0) extra", "Card: dev (hw:a,0)", "Card: dev (hw:1,)",
                           "Card: dev (hw:1)", "(hw:1,0)", "Card: dev (hw:-1,0)", "Card: dev (hdmi:1,0)",
                           // Ends in ')' but carries more after M: only the tail check sees it.
                           "Card: dev (hw:1,0 x)", "Card: dev (hw:1,0b)"}) {
        const bool parsed = parseAlsaHwName(no, n);
        if (parsed) { std::printf("      parsed as hardware, and must not be: \"%s\"\n", no); }
        CHECK(!parsed);
    }

    const auto alsa = [](const std::string& name, int index) {
        return stereoCard(name, "ALSA", index, ratesUpTo192k());
    };
    // THE BOOT THAT NUMBERED THE CARD DIFFERENTLY: saved at hw:1,0, now hw:2,0.
    {
        const std::vector<SoundCardDevice> list = {alsa("HDA Intel PCH: ALC892 Analog (hw:0,0)", 0),
                                                   alsa("USB Audio CODEC: USB Audio (hw:2,0)", 1)};
        const auto m = matchSoundCard(list, "USB Audio CODEC: USB Audio (hw:1,0)", "ALSA", false);
        CHECK(m.at == 1 && m.candidates.empty());
        auto fake = std::make_shared<FakeBackend>();
        fake->devices = list;
        SoundCardSource src = makeSource(fake);
        SoundCardSettings s;
        s.device = "USB Audio CODEC: USB Audio (hw:1,0)";
        s.hostApi = "ALSA";
        s.cardRateHz = 48000.0;
        CHECK(src.openWith(s));
        CHECK(fake->lastIndex == 1);
        // It is then known by what it is called NOW.
        CHECK(src.settings().device == "USB Audio CODEC: USB Audio (hw:2,0)");
        // A name picked from THIS list is matched exactly...
        CHECK(matchSoundCard(list, "USB Audio CODEC: USB Audio (hw:1,0)", "ALSA", true).at == -1);
        CHECK(matchSoundCard(list, "USB Audio CODEC: USB Audio (hw:2,0)", "ALSA", true).at == 1);
        // ...another PCM device on the card is another input, and so is
        // another host API.
        CHECK(matchSoundCard(list, "USB Audio CODEC: USB Audio (hw:1,1)", "ALSA", false).at == -1);
        CHECK(matchSoundCard(list, "USB Audio CODEC: USB Audio (hw:1,0)", "JACK Audio Connection Kit", false).at ==
              -1);
    }
    // TWO IDENTICAL CARDS: ALSA may have swapped their numbers, so a saved
    // name cannot say which one it was.
    {
        const std::vector<SoundCardDevice> list = {alsa("USB Audio CODEC: USB Audio (hw:1,0)", 0),
                                                   alsa("USB Audio CODEC: USB Audio (hw:2,0)", 1)};
        const auto m = matchSoundCard(list, "USB Audio CODEC: USB Audio (hw:1,0)", "ALSA", false);
        CHECK(m.at == -1);
        CHECK(m.candidates.size() == 2);
        auto fake = std::make_shared<FakeBackend>();
        fake->devices = list;
        SoundCardSource src = makeSource(fake);
        SoundCardSettings s;
        s.device = "USB Audio CODEC: USB Audio (hw:1,0)";
        s.hostApi = "ALSA";
        s.cardRateHz = 48000.0;
        CHECK(!src.openWith(s));
        CHECK(fake->openCalls.load() == 0);
        const std::string e = src.lastError();
        std::printf("two identical ALSA cards: \"%s\"\n", e.c_str());
        CHECK(e.find("(hw:1,0)") != std::string::npos && e.find("(hw:2,0)") != std::string::npos);
        CHECK(e.find("not connected") == std::string::npos);  // ambiguous, not missing
        // Chosen from this session's own list, each is exactly that entry.
        s.pickedFromList = true;
        CHECK(src.openWith(s));
        CHECK(fake->lastIndex == 0);
        s.device = "USB Audio CODEC: USB Audio (hw:2,0)";
        CHECK(src.openWith(s));
        CHECK(fake->lastIndex == 1);
        // And the Source section does not release a running card for a
        // choice that is going to be refused.
        SoundCardSettings live = s;
        live.device = "USB Audio CODEC: USB Audio (hw:1,0)";
        SoundCardSettings saved = live;
        saved.pickedFromList = false;
        CHECK(!cascade::gui::soundCardReopenReleasesFirst(true, live, saved, list));
    }
    // ONE CARD, TWO PCM DEVICES OF ONE NAME: told apart by M, which the card
    // itself numbers.
    {
        const std::vector<SoundCardDevice> list = {alsa("USB Audio CODEC: USB Audio (hw:1,0)", 0),
                                                   alsa("USB Audio CODEC: USB Audio (hw:1,1)", 1)};
        CHECK(matchSoundCard(list, "USB Audio CODEC: USB Audio (hw:3,1)", "ALSA", false).at == 1);
        CHECK(matchSoundCard(list, "USB Audio CODEC: USB Audio (hw:3,0)", "ALSA", false).at == 0);
    }
    // Not ALSA: unchanged - the exact name and host API.
    {
        const std::vector<SoundCardDevice> list = {
            stereoCard("Line (USB Audio CODEC)", "Windows WASAPI", 0, ratesUpTo192k())};
        CHECK(matchSoundCard(list, "Line (USB Audio CODEC)", "Windows WASAPI", false).at == 0);
        CHECK(matchSoundCard(list, "Line (USB Audio CODEC)", "MME", false).at == -1);
    }
    // "Picked from this list" is true of one session only: never in the args
    // a patch saves, never in the config.
    {
        SoundCardSettings s;
        s.device = "USB Audio CODEC: USB Audio (hw:1,0)";
        s.hostApi = "ALSA";
        s.pickedFromList = true;
        SoundCardSettings back;
        CHECK(cascade::source::parseSoundCardArgs(cascade::source::soundCardArgs(s), back));
        CHECK(!back.pickedFromList);
        CHECK(!cascade::gui::soundCardFromConfig(cascade::gui::soundCardToConfig(s)).pickedFromList);
    }
}

// The second review's X2: the closer thread must keep the capture block alive
// until the close has returned - a host API can deliver one last callback
// while it closes. A backend that does exactly that checks the block is there.
class LastCallbackBackend final : public SoundCardBackend {
public:
    std::vector<SoundCardDevice> devices;
    std::atomic<int> closes{0};
    std::atomic<int> captureAliveInClose{-1};
    std::vector<SoundCardDevice> listDevices() override { return devices; }
    bool open(const SoundCardDevice&, int, double, bool, PushFn push, void* user, std::string&) override {
        push_ = push;
        user_ = user;
        return true;
    }
    void close() override {
        if (user_ == nullptr) { return; }
        const bool alive = SoundCardSource::captureAlive(user_);
        captureAliveInClose = alive ? 1 : 0;
        if (alive) {
            const float frame[2] = {0.25f, 0.25f};
            push_(user_, frame, 1);  // the last callback, mid-close
        }
        user_ = nullptr;
        ++closes;
    }
    bool alive() override { return user_ != nullptr; }

private:
    PushFn push_ = nullptr;
    void* user_ = nullptr;
};

void testCloseKeepsCaptureAlive() {
    auto be = std::make_shared<LastCallbackBackend>();
    be->devices = {stereoCard("USB Audio CODEC", "Windows WASAPI", 0, ratesUpTo192k())};
    {
        SoundCardSource src([be] { return std::static_pointer_cast<SoundCardBackend>(be); });
        CHECK(src.openWith(iqSettings("USB Audio CODEC", 48000.0)));
    }  // a healthy card: its close is waited for
    std::printf("close: capture block alive during the close = %d\n", be->captureAliveInClose.load());
    CHECK(be->closes.load() == 1);
    CHECK(be->captureAliveInClose.load() == 1);
}

// Item 5: WAITS DO NOT STACK. A card the host API already says has stopped is
// not waited for, and several closes share one deadline.
void testCloseWaits() {
    const std::vector<SoundCardDevice> devices = {
        stereoCard("USB Audio CODEC", "Windows WASAPI", 0, ratesUpTo192k())};
    const auto wait = SoundCardSource::kCloseWaitMs;
    const double waitMs = static_cast<double>(wait.count());

    // (a) Pulled out while the receiver was STOPPED: nothing read(), so no
    // fault was ever latched - but the host API says the stream has stopped.
    {
        auto gate = std::make_shared<HangingBackend::Gate>();
        auto src = std::make_unique<SoundCardSource>([&]() -> std::shared_ptr<SoundCardBackend> {
            return std::make_shared<HangingBackend>(gate, true, devices, /*answersAlive=*/false);
        });
        CHECK(src->openWith(iqSettings("USB Audio CODEC", 48000.0)));
        CHECK(!src->faulted());
        Bounded d = runBounded([&] { src.reset(); }, wait * 3);
        std::printf("close waits: a card that is gone (never read) took %.0f ms\n", d.ms);
        CHECK(d.finished);
        CHECK(d.ms < 300.0);
        releaseGate(*gate);
        if (d.fut.valid()) { d.fut.get(); }
    }

    // (b) Five healthy cards whose closes all hang, closed together - a
    // patch stopping - cost ONE wait, not five.
    {
        auto gate = std::make_shared<HangingBackend::Gate>();
        std::vector<std::unique_ptr<SoundCardSource>> srcs;
        for (int i = 0; i < 5; ++i) {
            srcs.push_back(std::make_unique<SoundCardSource>([&]() -> std::shared_ptr<SoundCardBackend> {
                return std::make_shared<HangingBackend>(gate, true, devices);
            }));
            CHECK(srcs.back()->openWith(iqSettings("USB Audio CODEC", 48000.0)));
        }
        const std::uint64_t abandonedBefore = SoundCardSource::abandonedCloses();
        Bounded d = runBounded(
            [&] {
                SoundCardSource::CloseBatch batch;
                srcs.clear();
            },
            wait * 7);
        std::printf("close waits: five hung closes in one batch took %.0f ms (one wait is %.0f)\n", d.ms, waitMs);
        CHECK(d.finished);
        CHECK(d.ms >= waitMs - 20.0);
        CHECK(d.ms < waitMs + 400.0);
        CHECK(SoundCardSource::abandonedCloses() == abandonedBefore + 5);
        releaseGate(*gate);
        if (d.fut.valid()) { d.fut.get(); }
    }

    // (c) The control: two, WITHOUT a batch, wait one after the other.
    {
        auto gate = std::make_shared<HangingBackend::Gate>();
        std::vector<std::unique_ptr<SoundCardSource>> srcs;
        for (int i = 0; i < 2; ++i) {
            srcs.push_back(std::make_unique<SoundCardSource>([&]() -> std::shared_ptr<SoundCardBackend> {
                return std::make_shared<HangingBackend>(gate, true, devices);
            }));
            CHECK(srcs.back()->openWith(iqSettings("USB Audio CODEC", 48000.0)));
        }
        Bounded d = runBounded([&] { srcs.clear(); }, wait * 4);
        std::printf("close waits: two hung closes without a batch took %.0f ms\n", d.ms);
        CHECK(d.finished);
        CHECK(d.ms >= 2.0 * waitMs - 40.0);
        releaseGate(*gate);
        if (d.fut.valid()) { d.fut.get(); }
    }

    // (d) Closes that finish: the batch returns as soon as they have, and
    // leaves none behind.
    {
        auto gate = std::make_shared<HangingBackend::Gate>();
        std::vector<std::unique_ptr<SoundCardSource>> srcs;
        for (int i = 0; i < 3; ++i) {
            srcs.push_back(std::make_unique<SoundCardSource>([&]() -> std::shared_ptr<SoundCardBackend> {
                return std::make_shared<HangingBackend>(gate, false, devices);
            }));
            CHECK(srcs.back()->openWith(iqSettings("USB Audio CODEC", 48000.0)));
        }
        const std::uint64_t abandonedBefore = SoundCardSource::abandonedCloses();
        Bounded d = runBounded(
            [&] {
                SoundCardSource::CloseBatch batch;
                srcs.clear();
            },
            wait * 2);
        std::printf("close waits: three prompt closes in one batch took %.0f ms\n", d.ms);
        CHECK(d.finished);
        // Half the deadline, not a tight figure: what this separates is "came
        // back when the closes did" from "sat out the deadline anyway", and
        // three closer threads can take a few hundred ms just to be scheduled
        // on a machine that is compiling (measured once at 349 ms, 0 ms on
        // five reruns once it was idle).
        CHECK(d.ms < waitMs / 2.0);
        CHECK(gate->closed.load() == 3);  // every close had finished when the batch returned
        CHECK(SoundCardSource::abandonedCloses() == abandonedBefore);
        if (d.fut.valid()) { d.fut.get(); }
    }

    // (e) At exit (the wait switched off) a batch waits for nothing.
    {
        auto gate = std::make_shared<HangingBackend::Gate>();
        std::vector<std::unique_ptr<SoundCardSource>> srcs;
        for (int i = 0; i < 2; ++i) {
            srcs.push_back(std::make_unique<SoundCardSource>([&]() -> std::shared_ptr<SoundCardBackend> {
                return std::make_shared<HangingBackend>(gate, true, devices);
            }));
            CHECK(srcs.back()->openWith(iqSettings("USB Audio CODEC", 48000.0)));
        }
        SoundCardSource::setCloseWaitEnabled(false);
        Bounded d = runBounded(
            [&] {
                SoundCardSource::CloseBatch batch;
                srcs.clear();
            },
            wait * 3);
        SoundCardSource::setCloseWaitEnabled(true);
        std::printf("close waits: a batch at exit took %.0f ms\n", d.ms);
        CHECK(d.finished);
        CHECK(d.ms < 300.0);
        releaseGate(*gate);
        if (d.fut.valid()) { d.fut.get(); }
    }

    // (e2) ...including closes the batch collected BEFORE the teardown began:
    // the exit switches the wait off, and a batch still open then must not
    // spend the shutdown budget on them.
    {
        auto gate = std::make_shared<HangingBackend::Gate>();
        std::vector<std::unique_ptr<SoundCardSource>> srcs;
        for (int i = 0; i < 2; ++i) {
            srcs.push_back(std::make_unique<SoundCardSource>([&]() -> std::shared_ptr<SoundCardBackend> {
                return std::make_shared<HangingBackend>(gate, true, devices);
            }));
            CHECK(srcs.back()->openWith(iqSettings("USB Audio CODEC", 48000.0)));
        }
        Bounded d = runBounded(
            [&] {
                SoundCardSource::CloseBatch batch;
                srcs.clear();                                  // collected, wait still on
                SoundCardSource::setCloseWaitEnabled(false);   // the teardown begins
            },
            wait * 3);
        SoundCardSource::setCloseWaitEnabled(true);
        std::printf("close waits: a batch whose closes were collected before the exit took %.0f ms\n", d.ms);
        CHECK(d.finished);
        CHECK(d.ms < 300.0);
        releaseGate(*gate);
        if (d.fut.valid()) { d.fut.get(); }
    }

    // (f) The patch page stops its radios under one batch (read from the
    // source: AppWindow is not reachable from a unit test).
    {
        const std::string text = readSource("src/gui/app_window_patch_radios.cpp");
        const std::size_t fn = text.find("void AppWindow::patchStopAll(");
        const std::size_t batch =
            fn == std::string::npos ? std::string::npos : text.find("SoundCardSource::CloseBatch", fn);
        const std::size_t clear =
            batch == std::string::npos ? std::string::npos : text.find("patchRadios_.clear();", batch);
        std::printf("patchStopAll: CloseBatch@%zu patchRadios_.clear()@%zu\n", batch, clear);
        CHECK(batch != std::string::npos && clear != std::string::npos && clear - batch < 200);
        // ...and nothing destroys the radios before it.
        const std::size_t earlyClear =
            fn == std::string::npos ? std::string::npos : text.find("patchRadios_.clear();", fn);
        CHECK(earlyClear == clear);
    }
}

// Item 3: the patch's loan and hand-back describe the RUNNING card, not the
// Source section's controls - here edited (a new rate, the other channel)
// and never Opened.
void testPatchUsesRunningCard() {
    SoundCardSettings live;
    live.device = "Line (USB Audio CODEC)";
    live.hostApi = "Windows WASAPI";
    live.cardRateHz = 192000.0;
    live.channel = 0;
    SoundCardSettings section = live;  // edited, not Opened
    section.cardRateHz = 48000.0;
    section.channel = 1;
    const auto l = cascade::gui::receiverSourceForPatch(true, "soundcard", false, false, "", false, live);
    CHECK(l.take);
    CHECK(l.card.cardRateHz == 192000.0 && l.card.channel == 0);
    CHECK(!(l.card.cardRateHz == section.cardRateHz));

    // The application's side of it, read from the source (the same device
    // test_shutdown_budget uses for the teardown's shape): the loan is asked
    // about soundCardLive_, the keep carries what the loan took, and the
    // hand-back reopens exactly that.
    const std::string text = readSource("src/gui/app_window_patch_radios.cpp");
    CHECK(!text.empty());
    const std::size_t call = text.find("receiverSourceForPatch(");
    const std::size_t end = call == std::string::npos ? std::string::npos : text.find(");", call);
    const std::string args = (call != std::string::npos && end != std::string::npos) ? text.substr(call, end - call)
                                                                                    : std::string();
    std::printf("patch loan call: %s)\n", args.c_str());
    CHECK(args.find("soundCardLive_") != std::string::npos);
    CHECK(args.find("soundCard_") == std::string::npos);
    CHECK(text.find("keep.card = loan.card;") != std::string::npos);
    const std::size_t back = text.find("if (keep.kind == \"soundcard\")");
    CHECK(back != std::string::npos);
    const std::size_t reopen = back == std::string::npos ? std::string::npos
                                                         : text.find("launchSoundCardOpen(", back);
    const std::string handBack = "launchSoundCardOpen(false, keep.card)";
    CHECK(reopen != std::string::npos && reopen - back < 800 &&
          text.compare(reopen, handBack.size(), handBack) == 0);
}

// Item 4: PortAudio's open-stream list is changed by one thread at a time,
// and a close that hangs inside Pa_CloseStream holds nobody up for long.
void testStreamListSerialised() {
    fakepa::scriptWindowsMachine();
    fakepa::resetUse();
    const auto factory = [] { return cascade::source::makePortAudioSoundCardBackend(fakepa::kApi); };
    const std::vector<SoundCardDevice> list = factory()->listDevices();
    CHECK(!list.empty());
    if (list.empty()) { return; }
    const SoundCardDevice mic = list[0];

    // (a) Opens and closes on eight threads at once.
    fakepa::listOverlaps = 0;
    fakepa::listProbe = true;
    std::atomic<int> opened{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; ++t) {
        ts.emplace_back([&] {
            for (int i = 0; i < 25; ++i) {
                auto b = factory();
                std::string err;
                if (b->open(mic, 2, 48000.0, false, &SoundCardSource::pushFrames, nullptr, err)) {
                    ++opened;
                    b->close();
                }
            }
        });
    }
    for (auto& t : ts) { t.join(); }
    fakepa::listProbe = false;
    std::printf("stream list: %d opens and closes on 8 threads, %d found another inside the list\n",
                opened.load(), fakepa::listOverlaps.load());
    CHECK(opened.load() == 200);
    CHECK(fakepa::listOverlaps.load() == 0);
    CHECK(fakepa::streamsInUse() == 0);

    // (b) A close that hangs INSIDE Pa_CloseStream (after its list work)
    // keeps the lock; an open on a worker waits kStreamListWaitMs for it and
    // then goes ahead, and another card's close still finishes inside its
    // own caller's wait.
    fakepa::armGate();
    SoundCardSettings s;
    s.device = mic.name;
    s.hostApi = mic.hostApi;
    s.cardRateHz = 48000.0;
    s.pickedFromList = true;
    auto a = fakePaSource();
    CHECK(a->openWith(s, list));
    fakepa::hangCloseOn = &fakepa::streamIds[(fakepa::nextStream.load() - 1) % fakepa::kStreams];
    const std::uint64_t waitsBefore = cascade::sink::paStreamListWaitsAbandoned();
    Bounded destroyA = runBounded([&] { a.reset(); }, SoundCardSource::kCloseWaitMs + std::chrono::milliseconds(700));
    CHECK(destroyA.finished);
    CHECK(fakepa::hung.load() == 1);
    std::unique_ptr<SoundCardSource> c;
    Bounded openC = runBounded(
        [&] {
            c = fakePaSource();
            CHECK(c->openWith(s, list));
        },
        cascade::sink::kStreamListWaitMs + std::chrono::milliseconds(700));
    std::printf("stream list: an open beside a close hung inside Pa_CloseStream took %.0f ms (%s)\n", openC.ms,
                openC.finished ? "finished" : "STILL WAITING");
    CHECK(openC.finished);
    CHECK(openC.ms >= static_cast<double>(cascade::sink::kStreamListWaitMs.count()) - 20.0);
    CHECK(cascade::sink::paStreamListWaitsAbandoned() >= waitsBefore + 1);
    if (openC.finished && c) {
        const int inUse = fakepa::streamsInUse();
        Bounded destroyC =
            runBounded([&] { c.reset(); }, SoundCardSource::kCloseWaitMs + std::chrono::milliseconds(700));
        std::printf("stream list: closing another card meanwhile took %.0f ms\n", destroyC.ms);
        CHECK(destroyC.finished);
        CHECK(destroyC.ms < static_cast<double>(SoundCardSource::kCloseWaitMs.count()));
        CHECK(fakepa::streamsInUse() == inUse - 1);  // really closed, not left behind
        if (destroyC.fut.valid()) { destroyC.fut.get(); }
    }
    fakepa::openGate();
    if (destroyA.fut.valid()) { destroyA.fut.get(); }
    if (openC.fut.valid()) { openC.fut.get(); }
    c.reset();
    fakepa::hangCloseOn = nullptr;
    CHECK(fakepa::streamsInUseSettled() == 0);

    // (c) Every Pa_OpenStream in the product takes the guard: the audio
    // output's and the microphone's (the sound card's goes through its API
    // table and is exercised above).
    namespace fs = std::filesystem;
    int sites = 0;
    int unguarded = 0;
    for (const auto& e : fs::recursive_directory_iterator(fs::path(CASCADE_SOURCE_DIR) / "src")) {
        if (!e.is_regular_file()) { continue; }
        const std::string ext = e.path().extension().string();
        if ((ext != ".cpp" && ext != ".hpp") || e.path().filename() == "lang_assets.hpp") { continue; }
        const std::string rel = fs::relative(e.path(), fs::path(CASCADE_SOURCE_DIR)).generic_string();
        const std::string text = readSource(rel);
        for (std::size_t at = text.find("Pa_OpenStream("); at != std::string::npos;
             at = text.find("Pa_OpenStream(", at + 1)) {
            const std::size_t bol = text.rfind('\n', at);
            const std::string head = text.substr(bol == std::string::npos ? 0 : bol + 1,
                                                 at - (bol == std::string::npos ? 0 : bol + 1));
            if (head.find("//") != std::string::npos) { continue; }
            ++sites;
            const std::size_t from = at > 1500 ? at - 1500 : 0;
            if (text.substr(from, at - from).find("PaStreamListGuard") == std::string::npos) {
                ++unguarded;
                std::printf("      %s: Pa_OpenStream without the stream-list guard\n", rel.c_str());
            }
        }
    }
    std::printf("stream list: %d Pa_OpenStream call(s) in src/, %d unguarded\n", sites, unguarded);
    CHECK(sites == 2);
    CHECK(unguarded == 0);
}

// The second review's X8: alive() is asked from the source thread (and now
// from closeDevice) and must never block - a poll stuck inside the host API
// answers "busy is not dead" to anyone else who asks meanwhile.
void testAliveNeverBlocks() {
    fakepa::scriptWindowsMachine();
    fakepa::resetUse();
    fakepa::armGate();
    const std::vector<SoundCardDevice> list =
        cascade::source::makePortAudioSoundCardBackend(fakepa::kApi)->listDevices();
    CHECK(!list.empty());
    if (list.empty()) { return; }
    auto b = cascade::source::makePortAudioSoundCardBackend(fakepa::kApi);
    std::string err;
    CHECK(b->open(list[0], 2, 48000.0, false, &SoundCardSource::pushFrames, nullptr, err));
    const int hungBefore = fakepa::hung.load();
    fakepa::hangActiveOn = &fakepa::streamIds[(fakepa::nextStream.load() - 1) % fakepa::kStreams];
    auto stuck = std::async(std::launch::async, [&] { return b->alive(); });
    const auto t0 = Clock::now();
    while (fakepa::hung.load() == hungBefore && Clock::now() - t0 < std::chrono::seconds(2)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(fakepa::hung.load() == hungBefore + 1);
    std::atomic<bool> answer{false};
    Bounded second = runBounded([&] { answer = b->alive(); }, std::chrono::milliseconds(300));
    std::printf("alive: a second ask while one is stuck in the host API took %.0f ms (%s)\n", second.ms,
                second.finished ? "answered" : "BLOCKED");
    CHECK(second.finished);
    CHECK(second.ms < 100.0);
    fakepa::hangActiveOn = nullptr;
    fakepa::openGate();
    (void)stuck.get();
    if (second.fut.valid()) { second.fut.get(); }
    CHECK(answer.load());  // busy is not dead
    b->close();
    CHECK(fakepa::streamsInUse() == 0);
}

// The second review's X3: a backend destroyed with its stream still open
// closes it BEFORE it gives back its PortAudio initialisation - the last
// Pa_Terminate closes every open stream itself, and a close after that would
// close it twice.
void testBackendClosesBeforeTerminate() {
    fakepa::scriptWindowsMachine();
    fakepa::resetUse();
    auto b = cascade::source::makePortAudioSoundCardBackend(fakepa::kApi);
    const std::vector<SoundCardDevice> list = b->listDevices();
    CHECK(!list.empty());
    if (list.empty()) { return; }
    std::string err;
    CHECK(b->open(list[0], 2, 48000.0, false, &SoundCardSource::pushFrames, nullptr, err));
    fakepa::watchedOpenAtTerminate = -1;
    fakepa::watchTerminate = &fakepa::streamIds[(fakepa::nextStream.load() - 1) % fakepa::kStreams];
    b.reset();  // destroyed with its stream open
    std::printf("backend destroyed open: its stream was %s when it released PortAudio\n",
                fakepa::watchedOpenAtTerminate.load() == 0 ? "closed" : "STILL OPEN");
    CHECK(fakepa::watchedOpenAtTerminate.load() == 0);
    fakepa::watchTerminate = nullptr;
    CHECK(fakepa::streamsInUse() == 0);
}

// Item 8: every Pa_Initialize and Pa_Terminate in the product goes through the
// one locked helper (PortAudio's own count is a plain int), and the helper
// keeps the count exact under contention.
void testPortAudioInitShared() {
    // The source, read: no other call site.
    namespace fs = std::filesystem;
    int offenders = 0;
    int sites = 0;
    for (const auto& e : fs::recursive_directory_iterator(fs::path(CASCADE_SOURCE_DIR) / "src")) {
        if (!e.is_regular_file()) { continue; }
        const std::string ext = e.path().extension().string();
        if (ext != ".cpp" && ext != ".hpp" && ext != ".h") { continue; }
        if (e.path().filename() == "lang_assets.hpp") { continue; }
        std::FILE* f = std::fopen(e.path().string().c_str(), "rb");
        if (f == nullptr) { continue; }
        std::string text;
        char buf[65536];
        std::size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) { text.append(buf, n); }
        std::fclose(f);
        for (const char* call : {"Pa_Initialize()", "Pa_Terminate()"}) {
            for (std::size_t at = text.find(call); at != std::string::npos; at = text.find(call, at + 1)) {
                // A call, not a mention in a comment: the line up to it has no "//".
                const std::size_t bol = text.rfind('\n', at);
                const std::string head = text.substr(bol == std::string::npos ? 0 : bol + 1,
                                                     at - (bol == std::string::npos ? 0 : bol + 1));
                if (head.find("//") != std::string::npos) { continue; }
                ++sites;
                if (e.path().filename() != "pa_init.cpp") {
                    ++offenders;
                    std::printf("      %s calls %s outside sink/pa_init.cpp\n", e.path().string().c_str(), call);
                }
            }
        }
    }
    CHECK(sites == 2);  // the helper's own two
    CHECK(offenders == 0);

    // Under contention: with one initialisation held, eight threads pair
    // thousands of initialise/terminate calls; afterwards releasing the held
    // one must leave PortAudio uninitialised - the count came back exact.
    if (!cascade::sink::paInitializeShared()) {
        std::printf("      PortAudio would not initialise here; contention half skipped\n");
        return;
    }
    std::vector<std::thread> ts;
    std::atomic<int> failed{0};
    for (int t = 0; t < 8; ++t) {
        ts.emplace_back([&failed] {
            for (int i = 0; i < 20000; ++i) {
                if (!cascade::sink::paInitializeShared()) {
                    ++failed;
                    continue;
                }
                cascade::sink::paTerminateShared();
            }
        });
    }
    for (auto& t : ts) { t.join(); }
    CHECK(failed.load() == 0);
    CHECK(Pa_GetDeviceCount() >= 0);  // still initialised: ours is held
    cascade::sink::paTerminateShared();
    const PaDeviceIndex after = Pa_GetDeviceCount();
    std::printf("pa init: after 160000 contended pairs, released: Pa_GetDeviceCount() = %d (%s)\n",
                static_cast<int>(after), after == paNotInitialized ? "uninitialised, count exact" : "COUNT DRIFTED");
    CHECK(after == paNotInitialized);
}

// --- 9. the real PortAudio backend, enumeration only --------------------------------

void testRealBackendEnumerates() {
    // On a worker, as the application does it.
    auto fut = std::async(std::launch::async, [] {
        auto b = cascade::source::makePortAudioSoundCardBackend();
        return b->listDevices();
    });
    const std::vector<SoundCardDevice> list = fut.get();
    std::printf("real backend: %zu input device(s)\n", list.size());
    for (const SoundCardDevice& d : list) {
        std::string rates;
        for (const SoundCardRate& r : d.rates) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%s%.0f%s", rates.empty() ? "" : " ", r.hz, r.exclusive ? "x" : "");
            rates += buf;
        }
        std::printf("  [%d] %s | ch %d | default %.0f%s | %s\n", d.index,
                    cascade::source::soundCardDeviceLabel(d).c_str(), d.maxInputChannels,
                    d.defaultRateHz, d.isDefault ? " | DEFAULT" : "", rates.c_str());
        // Every listed rate is one the host API accepted; it must be a real one.
        for (const SoundCardRate& r : d.rates) { CHECK(r.hz >= 8000.0 && r.hz <= 384000.0); }
    }
}

}  // namespace

int main() {
    testRealToIqAxis();
    testRealModeChannel();
    testIqModeAndSwap();
    testRates();
    testMissingDevice();
    testLiveness();
    testPipelineSaq();
    testPipelineFault();
    testArgsRoundTrip();
    testConfigRoundTrip();
    testRememberedAndTune();
    testPatchRules();
    // The review of 6b5aead, item by item.
    testFixedCentreEdge();
    testVfoInsideSpan();
    testWholeFramesAndOverruns();
    testStartDrainsStale();
    testRateChange();
    testAlivePollInterval();
    testConfigRateBounds();
    testRecoveryAdvice();
    testPatchLoanAndCentreBox();
    testHungCloseAtTheSource();
    testHostApiFilter();
    testRealBackendHungAbort();
    // The second review, item by item.
    testSameCardReopen();
    testAlsaIdentity();
    testCloseKeepsCaptureAlive();
    testCloseWaits();
    testPatchUsesRunningCard();
    testStreamListSerialised();
    testAliveNeverBlocks();
    testBackendClosesBeforeTerminate();
    // Before anything else in this process initialises the real PortAudio.
    testPortAudioInitShared();
    testRealBackendEnumerates();
    return testSummary("test_soundcard_source");
}
