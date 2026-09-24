// test_transmitter.cpp - the rules about when a radio may be keyed, and the
// arithmetic that gets a 48 kHz modulator onto a 2.5 MS/s DAC.
//
// THE RULES ARE THE POINT OF THIS FILE. A transmitter that modulates
// correctly and keys when it should not is worse than one that does not work:
// the first is an interference report and the second is a bug. So most of
// what follows is about the key - that nothing but a hand can close it, that
// a frozen window opens it, that a latch opens itself, that a fault opens it -
// and it is all driven into a sink that records what it was told rather than
// into a radio, because there is no radio here and nothing on this bench may
// transmit.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/diag_log.hpp"
#include "core/transmitter.hpp"
#include "test_check.hpp"

using cascade::core::Transmitter;
using cascade::core::TxInput;
using cascade::core::TxInterpolator;
using cascade::dsp::TxMode;

namespace {

constexpr double kPi = 3.14159265358979323846;

// A radio that is only a ledger. Everything the transmitter can do to a sink
// is recorded, and the two things a test needs to provoke - a refusal to
// start, and a fault mid-transmission - are flags.
class RecordingSink : public cascade::source::IqSink {
public:
    explicit RecordingSink(double rateHz) : rate_(rateHz) {}

    bool start() override {
        if (refuseStart) {
            error_ = "this test radio was told to refuse";
            return false;
        }
        ++starts;
        running_ = true;
        return true;
    }
    void stop() override {
        if (running_) {
            ++stops;
            // A test that swaps this sink out of the transmitter can no longer
            // read `stops` afterwards: setSink() destroys the old sink, so the
            // ledger has to live outside the sink for that one case.
            if (stopsMirror != nullptr) { ++*stopsMirror; }
        }
        running_ = false;
    }
    bool running() const override { return running_; }

    double sampleRateHz() const override { return rate_; }
    bool setSampleRateHz(double hz) override {
        rate_ = hz;
        return true;
    }
    double centerFrequencyHz() const override { return freq_; }
    bool setCenterFrequencyHz(double hz) override {
        freq_ = hz;
        return true;
    }
    bool frequencyRangeHz(double& lo, double& hi) const override {
        lo = 70.0e6;
        hi = 6.0e9;
        return true;
    }
    bool sampleRateRangeHz(double& lo, double& hi) const override {
        lo = 2.083e6;
        hi = 61.44e6;
        return true;
    }
    bool gainRangeDb(double& lo, double& hi) const override {
        lo = -89.75;
        hi = 0.0;
        return true;
    }
    double gainDb() const override { return gain_; }
    bool setGainDb(double db) override {
        gain_ = db;
        return true;
    }

    std::size_t write(const std::complex<float>* s, std::size_t n) override {
        if (!running_ || faulted_) { return 0; }
        std::lock_guard<std::mutex> lk(mutex_);
        samples_ += n;
        if (peekFirst_.empty() && n > 0) { peekFirst_.assign(s, s + std::min<std::size_t>(n, 64)); }
        if (faultAfter > 0 && static_cast<long long>(samples_) >= faultAfter) {
            faulted_ = true;
            error_ = "this test radio was told to fault";
        }
        return n;
    }

    bool faulted() const override { return faulted_; }
    const char* name() const override { return "test radio"; }
    const char* lastError() const override { return error_.c_str(); }

    std::size_t samples() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return samples_;
    }

    std::atomic<int> starts{0};
    std::atomic<int> stops{0};
    // Optional external ledger for stops, for a test that lets the
    // transmitter destroy this sink and still needs the count afterwards.
    std::atomic<int>* stopsMirror = nullptr;
    bool refuseStart = false;
    long long faultAfter = 0;

private:
    mutable std::mutex mutex_;
    double rate_ = 480000.0;
    double freq_ = 145.0e6;
    double gain_ = 0.0;
    bool running_ = false;
    bool faulted_ = false;
    std::string error_;
    std::size_t samples_ = 0;
    std::vector<std::complex<float>> peekFirst_;
};

// One bin of a DFT at an arbitrary frequency, against an explicit rate.
double magAt(const std::vector<std::complex<float>>& x, double freqHz, double fs) {
    std::complex<double> acc(0.0, 0.0);
    for (std::size_t k = 0; k < x.size(); ++k) {
        const double ang = -2.0 * kPi * freqHz * static_cast<double>(k) / fs;
        acc += std::complex<double>(x[k].real(), x[k].imag()) *
               std::complex<double>(std::cos(ang), std::sin(ang));
    }
    return std::abs(acc) / static_cast<double>(x.size());
}

// Ticks a transmitter for a while, the way a frame loop does.
void tickFor(Transmitter& tx, int ms) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        tx.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    tx.tick();
}

template <typename Fn>
bool waitTicking(Transmitter& tx, Fn fn, std::chrono::milliseconds bound) {
    const auto deadline = std::chrono::steady_clock::now() + bound;
    while (std::chrono::steady_clock::now() < deadline) {
        tx.tick();
        if (fn()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    tx.tick();
    return fn();
}

}  // namespace

int main() {
    // =====================================================================
    // 1. THE INPUT TABLE, and which way an unknown index falls
    // =====================================================================
    {
        CHECK(std::string(cascade::core::txInputName(TxInput::Microphone)) == "MIC");
        CHECK(std::string(cascade::core::txInputName(TxInput::Tone)) == "TONE");
        TxInput back = TxInput::Microphone;
        CHECK(cascade::core::txInputFromName("TONE", back) && back == TxInput::Tone);
        CHECK(cascade::core::txInputFromName("MIC", back) && back == TxInput::Microphone);
        CHECK(!cascade::core::txInputFromName("LINE", back));
        CHECK(!cascade::core::txInputFromName(nullptr, back));
        // AN UNKNOWN INDEX LANDS ON THE TONE, not on the microphone. A
        // hand-edited config with a number this build does not know must not
        // point a transmitter at a room.
        CHECK(cascade::core::txInputFromIndex(-1) == TxInput::Tone);
        CHECK(cascade::core::txInputFromIndex(7) == TxInput::Tone);
        CHECK(cascade::core::txInputFromIndex(0) == TxInput::Microphone);
    }

    // =====================================================================
    // 2. THE RATE MATCHER: THE IMAGES IT EXISTS TO REMOVE
    //
    //    Interpolating 48 kHz up to a megahertz rate without an anti-imaging
    //    filter leaves copies of the whole modulation at every multiple of 48
    //    kHz either side of the carrier. Those are transmitted. This is the
    //    measurement that says they are not.
    // =====================================================================
    {
        constexpr double kAudio = 48000.0;
        constexpr double kSink = 2500000.0;
        TxInterpolator up;
        up.configure(kAudio, kSink);
        std::printf("interpolator: %.0f -> %.0f Hz is x%u then a %.6f fractional step\n", kAudio,
                    kSink, up.interpolation(), kAudio * up.interpolation() / kSink);
        CHECK(up.interpolation() == 52);  // round(2500000 / 48000)

        // A complex tone at 1 kHz, which is what a 1 kHz test tone on USB
        // becomes after the modulator.
        const std::size_t nIn = 4800;  // 100 ms
        std::vector<std::complex<float>> in(nIn);
        for (std::size_t k = 0; k < nIn; ++k) {
            const double a = 2.0 * kPi * 1000.0 * static_cast<double>(k) / kAudio;
            in[k] = std::complex<float>(0.5f * static_cast<float>(std::cos(a)),
                                        0.5f * static_cast<float>(std::sin(a)));
        }
        std::vector<std::complex<float>> out(up.maxOut(nIn));
        const std::size_t made = up.process(in.data(), nIn, out.data(), out.size());
        // The count follows the rate ratio, not the integer factor.
        const double expect = static_cast<double>(nIn) * kSink / kAudio;
        std::printf("interpolator made %zu samples, expected about %.0f\n", made, expect);
        CHECK(std::fabs(static_cast<double>(made) - expect) < 100.0);
        out.resize(made);

        // Skip the filter's own start-up transient before measuring.
        std::vector<std::complex<float>> steady(out.begin() + 5000, out.end());
        const double wanted = magAt(steady, 1000.0, kSink);
        // The first images: the 48 kHz sampling grid reflected about itself.
        const double imageLo = magAt(steady, -47000.0, kSink);
        const double imageHi = magAt(steady, 49000.0, kSink);
        const double worst = std::max(imageLo, imageHi);
        const double db = 20.0 * std::log10(wanted / std::max(1e-12, worst));
        std::printf("interpolator: wanted %.4f, worst first image %.3e, %.1f dB down\n", wanted,
                    worst, db);
        CHECK(db > 40.0);
        // And the amplitude survived: a resampler that quietly lost half the
        // level would still pass the image check.
        CHECK_NEAR(wanted, 0.5, 0.02);
    }

    // =====================================================================
    // 3. A BLOCK SPLIT MAKES NO DIFFERENCE
    //
    //    The TX thread calls this every 10 ms for as long as somebody holds a
    //    key. A resampler whose output depended on how the stream was cut
    //    into blocks would put a discontinuity at every block boundary, which
    //    is a buzz at 100 Hz on top of the modulation.
    // =====================================================================
    {
        TxInterpolator a;
        TxInterpolator b;
        a.configure(48000.0, 480000.0);
        b.configure(48000.0, 480000.0);
        const std::size_t n = 1920;
        std::vector<std::complex<float>> in(n);
        for (std::size_t k = 0; k < n; ++k) {
            const double t = 2.0 * kPi * 1234.0 * static_cast<double>(k) / 48000.0;
            in[k] = std::complex<float>(static_cast<float>(0.4 * std::cos(t)),
                                        static_cast<float>(0.4 * std::sin(t)));
        }
        std::vector<std::complex<float>> whole(a.maxOut(n));
        const std::size_t wn = a.process(in.data(), n, whole.data(), whole.size());

        std::vector<std::complex<float>> pieces;
        for (std::size_t off = 0; off < n; off += 480) {
            std::vector<std::complex<float>> tmp(b.maxOut(480));
            const std::size_t got = b.process(in.data() + off, 480, tmp.data(), tmp.size());
            pieces.insert(pieces.end(), tmp.begin(),
                          tmp.begin() + static_cast<std::ptrdiff_t>(got));
        }
        CHECK(pieces.size() == wn);
        double worst = 0.0;
        for (std::size_t i = 0; i < std::min(pieces.size(), wn); ++i) {
            worst = std::max(worst, static_cast<double>(std::abs(pieces[i] - whole[i])));
        }
        std::printf("block split: %zu vs %zu samples, worst difference %.3e\n", pieces.size(), wn,
                    worst);
        CHECK(worst < 1e-6);
    }

    // =====================================================================
    // 4. THE HARD RULE: NOTHING BUT A HAND CLOSES THE KEY
    //
    //    A transmitter that is constructed, given a radio, configured every
    //    way there is and ticked like a frame loop must never transmit.
    // =====================================================================
    {
        Transmitter tx;
        CHECK(!tx.transmitting());
        CHECK(!tx.haveSink());

        auto sink = std::make_unique<RecordingSink>(480000.0);
        RecordingSink* raw = sink.get();
        tx.setSink(std::move(sink));
        CHECK(tx.haveSink());

        tx.setMode(TxMode::USB);
        tx.setInput(TxInput::Tone);
        tx.setPowerDb(-10.0);
        CHECK(tx.setFrequencyHz(145.5e6));
        tx.setToneHz(700.0);
        tickFor(tx, 120);

        CHECK(!tx.transmitting());
        CHECK(raw->starts.load() == 0);
        CHECK(raw->samples() == 0);
        // Everything that was set, was set - so this is not a transmitter
        // that failed to work, it is one that worked and stayed quiet.
        CHECK(tx.mode() == TxMode::USB);
        CHECK(tx.input() == TxInput::Tone);
        CHECK_NEAR(tx.powerDb(), -10.0, 1e-9);
        CHECK_NEAR(tx.frequencyHz(), 145.5e6, 1.0);
        CHECK_NEAR(tx.toneHz(), 700.0, 1e-9);
    }

    // =====================================================================
    // 5. A HELD PTT KEYS IT, AND LETTING GO OPENS IT
    // =====================================================================
    {
        Transmitter tx;
        auto sink = std::make_unique<RecordingSink>(480000.0);
        RecordingSink* raw = sink.get();
        tx.setSink(std::move(sink));
        tx.setInput(TxInput::Tone);
        tx.setMode(TxMode::AM);
        CHECK(tx.setFrequencyHz(145.4875e6));
        cascade::core::DiagLog::instance().resetForTest();

        tx.setPttHeld(true);
        tx.tick();
        CHECK(tx.transmitting());
        CHECK(raw->starts.load() == 1);

        // THE KEY-DOWN LINE says the mode and the rate and NEVER the
        // frequency (0.99.33: it used to print "at 145.487500 MHz").
        {
            int keyed = 0;
            for (const std::string& l : cascade::core::DiagLog::instance().ringSnapshot()) {
                if (l.find("tx: keyed") == std::string::npos) { continue; }
                ++keyed;
                std::printf("key-down line: %s\n", l.c_str());
                CHECK(l.find("MHz") == std::string::npos);
                CHECK(l.find("480000 S/s") != std::string::npos);
            }
            CHECK(keyed == 1);
        }

        // It really is feeding the radio, at about the rate the radio runs
        // at - a transmitter that keyed and sent nothing is the failure this
        // check exists for.
        CHECK(waitTicking(tx, [raw] { return raw->samples() > 48000; },
                          std::chrono::seconds(2)));
        CHECK(tx.blocksSent() > 0);

        tx.setPttHeld(false);
        tx.tick();
        CHECK(!tx.transmitting());
        CHECK(raw->stops.load() >= 1);
        const std::size_t after = raw->samples();
        tickFor(tx, 80);
        // NOTHING AFTER THE KEY OPENED. A tail that kept running would be a
        // transmitter that stopped when it felt like it.
        CHECK(raw->samples() == after);
        // And the operator opened it, so there is no automatic reason to show.
        CHECK(tx.lastAutoUnkeyReason().empty());
    }

    // =====================================================================
    // 6. THE LATCH, AND THE DEADLINE IT CANNOT OUTLIVE
    // =====================================================================
    {
        Transmitter tx;
        auto sink = std::make_unique<RecordingSink>(480000.0);
        RecordingSink* raw = sink.get();
        tx.setSink(std::move(sink));
        tx.setInput(TxInput::Tone);
        // The product's own deadline is a minute; a test that took a minute
        // to check a safety property is one nobody runs.
        tx.setLatchTimeoutForTest(std::chrono::milliseconds(300));

        tx.setLatched(true);
        tx.tick();
        CHECK(tx.transmitting());
        CHECK(tx.latched());
        // A latch holds the key with NO hand on the PTT - that is what it is
        // for.
        CHECK(!tx.pttHeld());
        tickFor(tx, 100);
        CHECK(tx.transmitting());

        // ...and then it lets go on its own.
        CHECK(waitTicking(tx, [&tx] { return !tx.transmitting(); }, std::chrono::seconds(2)));
        CHECK(!tx.latched());
        CHECK(raw->stops.load() >= 1);
        const std::string why = tx.lastAutoUnkeyReason();
        std::printf("latch failsafe: \"%s\"\n", why.c_str());
        CHECK(why.find("latch") != std::string::npos);
    }

    // =====================================================================
    // 7. A FROZEN WINDOW OPENS THE KEY
    //
    //    THE FAILURE NOTHING ELSE CATCHES. Every other release path runs on
    //    the GUI thread, so a GUI thread that has stopped is a transmitter
    //    nobody can unkey. The TX thread watches for the frames drying up and
    //    lets go itself.
    // =====================================================================
    {
        Transmitter tx;
        auto sink = std::make_unique<RecordingSink>(480000.0);
        RecordingSink* raw = sink.get();
        tx.setSink(std::move(sink));
        tx.setInput(TxInput::Tone);

        tx.setLatched(true);
        tx.tick();
        CHECK(tx.transmitting());
        CHECK(raw->running());

        // The frame loop stops. Nothing calls tick() at all from here.
        const auto t0 = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(Transmitter::kKeyAliveWait +
                                    std::chrono::milliseconds(400));
        const double waited =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                .count();

        // THE RADIO IS QUIET, and it was made quiet by the TX thread without
        // anybody asking - which is the only thing that could have done it.
        CHECK(!raw->running());
        CHECK(raw->stops.load() >= 1);
        const std::size_t after = raw->samples();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        CHECK(raw->samples() == after);
        std::printf("dead-man's handle: the radio was silenced %.0f ms after the last frame "
                    "(bound %lld ms)\n",
                    waited, static_cast<long long>(Transmitter::kKeyAliveWait.count()));

        // And when the window comes back, the panel is told why.
        tx.tick();
        CHECK(!tx.transmitting());
        CHECK(!tx.latched());
        const std::string why = tx.lastAutoUnkeyReason();
        std::printf("dead-man's handle: \"%s\"\n", why.c_str());
        CHECK(why.find("stopped responding") != std::string::npos);
    }

    // =====================================================================
    // 8. A RADIO THAT FAULTS OPENS THE KEY
    // =====================================================================
    {
        Transmitter tx;
        auto sink = std::make_unique<RecordingSink>(480000.0);
        RecordingSink* raw = sink.get();
        raw->faultAfter = 20000;  // a few blocks in
        tx.setSink(std::move(sink));
        tx.setInput(TxInput::Tone);

        tx.setLatched(true);
        tx.tick();
        CHECK(tx.transmitting());
        CHECK(waitTicking(tx, [&tx] { return !tx.transmitting(); }, std::chrono::seconds(3)));
        CHECK(!tx.latched());
        CHECK(!raw->running());
        const std::string why = tx.lastAutoUnkeyReason();
        std::printf("fault: \"%s\" / \"%s\"\n", why.c_str(), tx.lastError().c_str());
        CHECK(why.find("faulted") != std::string::npos);
        CHECK(tx.lastError().find("told to fault") != std::string::npos);
    }

    // =====================================================================
    // 9. A RADIO THAT REFUSES TO START LEAVES NO LATCH BEHIND
    //
    //    A key that could not be honoured must not leave the panel's lamp lit
    //    and its latch closed - the next thing to succeed would then key
    //    without anybody touching it.
    // =====================================================================
    {
        Transmitter tx;
        auto sink = std::make_unique<RecordingSink>(480000.0);
        RecordingSink* raw = sink.get();
        raw->refuseStart = true;
        tx.setSink(std::move(sink));

        tx.setLatched(true);
        tx.tick();
        CHECK(!tx.transmitting());
        CHECK(!tx.latched());
        CHECK(!tx.pttHeld());
        CHECK(tx.lastError().find("refuse") != std::string::npos);

        // And with no radio at all it is a refusal, not a crash.
        Transmitter bare;
        bare.setPttHeld(true);
        bare.tick();
        CHECK(!bare.transmitting());
        CHECK(!bare.pttHeld());
    }

    // =====================================================================
    // 10. SWAPPING THE RADIO UNKEYS FIRST
    //
    //     A radio replaced under a live transmission would be left keyed with
    //     nothing feeding it, and the new one would inherit a key it was
    //     never given.
    // =====================================================================
    {
        Transmitter tx;
        auto first = std::make_unique<RecordingSink>(480000.0);
        // setSink() below destroys `first`, so its stop count is read through
        // a counter that outlives it. Reading first.get()->stops after the
        // swap was a use-after-free that passed or failed by allocator luck
        // (2 of 5 runs red on Linux, 2026-09-15).
        std::atomic<int> firstStops{0};
        first->stopsMirror = &firstStops;
        tx.setSink(std::move(first));
        tx.setInput(TxInput::Tone);
        tx.setLatched(true);
        tx.tick();
        CHECK(tx.transmitting());

        auto second = std::make_unique<RecordingSink>(480000.0);
        RecordingSink* b = second.get();
        tx.setSink(std::move(second));
        CHECK(!tx.transmitting());
        CHECK(!tx.latched());
        CHECK(firstStops.load() >= 1);
        tickFor(tx, 60);
        CHECK(b->starts.load() == 0);
        CHECK(b->samples() == 0);
    }

    // =====================================================================
    // 11. THE MODULATION REALLY IS THE MODE, ALL THE WAY TO THE RADIO
    //
    //     The one end-to-end check: a 1 kHz tone on USB, through the
    //     modulator and both interpolation stages, must arrive at the sink as
    //     a line at +1 kHz and nothing at -1 kHz. Every stage in between has
    //     a way of mirroring it.
    // =====================================================================
    {
        constexpr double kRate = 480000.0;
        Transmitter tx;
        auto sink = std::make_unique<RecordingSink>(kRate);
        RecordingSink* raw = sink.get();
        tx.setSink(std::move(sink));
        tx.setInput(TxInput::Tone);
        tx.setToneHz(1000.0);
        tx.setMode(TxMode::USB);

        // A sink that keeps what it is given, for this block only.
        class Keeper : public cascade::source::IqSink {
        public:
            bool start() override { return true; }
            void stop() override {}
            bool running() const override { return true; }
            double sampleRateHz() const override { return 480000.0; }
            bool setSampleRateHz(double) override { return true; }
            double centerFrequencyHz() const override { return 145.0e6; }
            bool setCenterFrequencyHz(double) override { return true; }
            bool frequencyRangeHz(double& lo, double& hi) const override {
                lo = 70.0e6;
                hi = 6.0e9;
                return true;
            }
            bool sampleRateRangeHz(double& lo, double& hi) const override {
                lo = 2.0e6;
                hi = 61.0e6;
                return true;
            }
            bool gainRangeDb(double& lo, double& hi) const override {
                lo = -89.75;
                hi = 0.0;
                return true;
            }
            double gainDb() const override { return 0.0; }
            bool setGainDb(double) override { return true; }
            std::size_t write(const std::complex<float>* s, std::size_t n) override {
                std::lock_guard<std::mutex> lk(m_);
                kept.insert(kept.end(), s, s + n);
                return n;
            }
            bool faulted() const override { return false; }
            const char* name() const override { return "keeper"; }
            const char* lastError() const override { return ""; }
            std::vector<std::complex<float>> take() {
                std::lock_guard<std::mutex> lk(m_);
                return kept;
            }
            std::vector<std::complex<float>> kept;

        private:
            std::mutex m_;
        };
        auto keeper = std::make_unique<Keeper>();
        Keeper* kp = keeper.get();
        tx.setSink(std::move(keeper));
        (void)raw;

        tx.setPttHeld(true);
        tx.tick();
        CHECK(waitTicking(tx, [kp] { return kp->take().size() > 120000; },
                          std::chrono::seconds(3)));
        tx.setPttHeld(false);
        tx.tick();

        std::vector<std::complex<float>> got = kp->take();
        CHECK(got.size() > 120000);
        // Past the envelope ramp and the Hilbert delay.
        std::vector<std::complex<float>> steady(got.begin() + 60000, got.begin() + 120000);
        const double up = magAt(steady, 1000.0, kRate);
        const double down = magAt(steady, -1000.0, kRate);
        const double db = 20.0 * std::log10(up / std::max(1e-12, down));
        std::printf("end to end on USB: +1 kHz %.4f, -1 kHz %.6f, %.1f dB\n", up, down, db);
        CHECK(db > 35.0);
        // A 0.5-level tone through a half-scale modulator: a quarter of full
        // scale, so nothing is anywhere near clipping the DAC.
        CHECK_NEAR(up, 0.25, 0.02);
        double worst = 0.0;
        for (const std::complex<float>& c : steady) {
            worst = std::max(worst, static_cast<double>(std::abs(c)));
        }
        CHECK(worst < 1.0);
    }

    // =====================================================================
    // 12. THE WEB REMOTE'S KEY: ONE ASSERTION IS WORTH TWO SECONDS
    //
    //     The difference between this key and the other two is that the hand
    //     holding it is at the far end of a network and may simply stop
    //     existing, with nothing to say so. So it is not a switch: keyRemote()
    //     buys kRemotePttHoldMs and the caller has to keep buying. These are
    //     the measurements that say the deadline is real, that re-asserting
    //     extends it, and that it is not merely the DEADLINE doing the work -
    //     an explicit release opens the key at once.
    // =====================================================================
    {
        Transmitter tx;
        auto sink = std::make_unique<RecordingSink>(480000.0);
        RecordingSink* raw = sink.get();
        tx.setSink(std::move(sink));
        tx.setInput(TxInput::Tone);

        const auto t0 = std::chrono::steady_clock::now();
        tx.keyRemote();
        tx.tick();
        CHECK(tx.transmitting());
        CHECK(tx.remoteKeyed());
        CHECK(raw->starts.load() == 1);
        // A remote key is NOT a latch and NOT a PTT: neither of the hands-on
        // controls has been touched, and the panel must not say they have.
        CHECK(!tx.latched());
        CHECK(!tx.pttHeld());
        // The hold is published as a countdown, and it starts full.
        const std::int64_t left = tx.remoteHoldRemainingMs();
        std::printf("remote key: %lld ms of hold left immediately after keying (bound %lld)\n",
                    static_cast<long long>(left),
                    static_cast<long long>(Transmitter::kRemotePttHoldMs.count()));
        CHECK(left > 0 && left <= Transmitter::kRemotePttHoldMs.count());

        // Well inside the hold, with nobody re-asserting, it is still keyed -
        // so the release below is the deadline and not an immediate drop.
        tickFor(tx, 1200);
        CHECK(tx.transmitting());
        CHECK(tx.remoteKeyed());

        // ...and then it lets go on its own, because the remote stopped
        // asking. Nothing called release; nothing called tick() differently.
        CHECK(waitTicking(tx, [&tx] { return !tx.transmitting(); }, std::chrono::seconds(3)));
        const double heldMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                .count();
        std::printf("remote key: released itself %.0f ms after one assertion (bound %lld ms)\n",
                    heldMs, static_cast<long long>(Transmitter::kRemotePttHoldMs.count()));
        CHECK(heldMs >= static_cast<double>(Transmitter::kRemotePttHoldMs.count()) - 50.0);
        CHECK(heldMs < static_cast<double>(Transmitter::kRemotePttHoldMs.count()) + 600.0);
        CHECK(!tx.remoteKeyed());
        CHECK(tx.remoteHoldRemainingMs() == 0);
        CHECK(!raw->running());
        const std::string why = tx.lastAutoUnkeyReason();
        std::printf("remote key: \"%s\"\n", why.c_str());
        CHECK(why.find("remote") != std::string::npos);

        // AND THE RADIO REALLY IS QUIET AFTERWARDS. An expiry that unkeyed the
        // panel while the thread kept feeding the board would pass every
        // check above.
        const std::size_t after = raw->samples();
        tickFor(tx, 100);
        CHECK(raw->samples() == after);
    }

    // =====================================================================
    // 13. RE-ASSERTING EXTENDS THE HOLD
    //
    //     The other half of the same rule: a browser that keeps asking keeps
    //     the key closed past the deadline, or the feature is a two-second
    //     transmitter and nothing else.
    // =====================================================================
    {
        Transmitter tx;
        auto sink = std::make_unique<RecordingSink>(480000.0);
        tx.setSink(std::move(sink));
        tx.setInput(TxInput::Tone);

        // Re-assert at the cadence the page uses (500 ms), for comfortably
        // longer than one hold.
        const auto deadline = std::chrono::steady_clock::now() +
                              Transmitter::kRemotePttHoldMs * 2 + std::chrono::milliseconds(400);
        auto nextAssert = std::chrono::steady_clock::now();
        bool stayedUp = true;
        tx.keyRemote();
        tx.tick();
        CHECK(tx.transmitting());
        while (std::chrono::steady_clock::now() < deadline) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= nextAssert) {
                tx.keyRemote();
                nextAssert = now + std::chrono::milliseconds(500);
            }
            tx.tick();
            if (!tx.transmitting()) { stayedUp = false; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        std::printf("remote key: still up after %lld ms of re-assertion: %s\n",
                    static_cast<long long>(Transmitter::kRemotePttHoldMs.count() * 2 + 400),
                    stayedUp ? "yes" : "NO");
        CHECK(stayedUp);
        CHECK(tx.transmitting());

        // Stop asking, and it goes - which is what proves the hold was being
        // EXTENDED rather than simply never enforced after the first key.
        CHECK(waitTicking(tx, [&tx] { return !tx.transmitting(); }, std::chrono::seconds(3)));
        CHECK(!tx.remoteKeyed());
    }

    // =====================================================================
    // 14. RELEASING OPENS IT AT ONCE, AND NO RADIO MEANS NO PENDING KEY
    // =====================================================================
    {
        Transmitter tx;
        auto sink = std::make_unique<RecordingSink>(480000.0);
        RecordingSink* raw = sink.get();
        tx.setSink(std::move(sink));
        tx.setInput(TxInput::Tone);

        tx.keyRemote();
        tx.tick();
        CHECK(tx.transmitting());
        // ONE TICK. Not "eventually", not "when the hold expires" - the page
        // said the finger came up and the key opens on the next frame, the
        // same as letting go of the local PTT.
        tx.releaseRemote("the remote let go");
        tx.tick();
        CHECK(!tx.transmitting());
        CHECK(!tx.remoteKeyed());
        CHECK(raw->stops.load() >= 1);
        // Nothing automatic did this, so there is no automatic reason to show.
        CHECK(tx.lastAutoUnkeyReason().empty());

        // A remote key with no radio behind it is a refusal that leaves
        // NOTHING PENDING - if the assertion survived, the refusal would be
        // retried every frame for two seconds and would key whatever radio
        // happened to be opened inside that window.
        Transmitter bare;
        bare.keyRemote();
        bare.tick();
        CHECK(!bare.transmitting());
        CHECK(!bare.remoteKeyed());
        CHECK(bare.lastError().find("no transmitter") != std::string::npos);

        // And the radio going away takes the key with it, exactly as it takes
        // a latch: setSink() stops first.
        Transmitter swap;
        auto first = std::make_unique<RecordingSink>(480000.0);
        swap.setSink(std::move(first));
        swap.setInput(TxInput::Tone);
        swap.keyRemote();
        swap.tick();
        CHECK(swap.transmitting());
        swap.setSink(nullptr);
        CHECK(!swap.transmitting());
        CHECK(!swap.remoteKeyed());
    }

    return testSummary("test_transmitter");
}
