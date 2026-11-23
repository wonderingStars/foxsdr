// EQUIVALENCE OF THE DEMOD PERFORMANCE PASS (2026-11-23; D:/foxsdr-build/prep/PERF-demod-2026-11-23.md section 6).
//
// The optimisations of the demod phase must change NOTHING a listener or a decoder receives (P1 the FIR kernel, P3 the spectrum's dB conversion,
// P4 the stereo carrier while the gate is shut: bit-identical) or change it by less than a stated, measured bound (P2 the NCO). This file holds
// the proofs, against REFERENCE implementations kept in tests/ref_dsp.hpp (the loops as they were at cebfddb5), never against a stored hash (a
// stored hash of audio breaks on the next compiler or libm update; a reference in the test does not). Hashes are printed as log lines for humans.
//
//   E1k  FirDecimator against RefFirDecimator, memcmp, for every tap set the chain uses, with the input cut into blocks of 1, 3, 7, 1008, 1024 and
//        8192 so every remainder of the several-outputs-at-once grouping and every decimation-grid phase occurs, from an unaligned start;
//   E3   the spectrum's dB conversion moved to the pull (P3): the estimator's two steps against the estimator as it was, bit for bit; the Pipeline's
//        frames pulled at random block counts against a reference estimator fed the same samples; two consumers of one frame; the block path's frames.
//   E4   the stereo decoder's 38 kHz carrier skipped while the gate is shut (P4): no pilot, a pilot, a pilot switched on and off every 300 ms, and
//        forced mono, each against the reference with the carrier always computed; the audio must match in every bit.
//   E2   the receive oscillator (P2, FastNco: the maths library every 32 samples): the oscillator against RefNco over 60 s at nine frequencies, retunes,
//        block splits, phase() and 10 minutes of drift; the chain's IF, multiplex and audio feeds against the same chain on the library path; Nco itself
//        and the transmit modulator unchanged.
//   E1c  the whole receive chain (tests/demod_chain.hpp, the pipeline's classes in the pipeline's order) for each of the eight modes over 10 s:
//        the four feeds the pipeline produces (the audio, the audio decoder plugins' tap, the IF recorder's, the multiplex recorder's) are
//        hashed, printed and compared bit for bit with the same chain run with the optimised kernels switched back to their reference paths
//        (dsp/dsp_reference.hpp: one bit per kernel, so each step proves only its own).
//
// Steps are added to this file with the commit that makes each optimisation; each says in its own section what it proves and what breaking it does.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
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

#include "core/pipeline.hpp"
#include "demod_chain.hpp"
#include "dsp_signals.hpp"
#include "dsp/demod.hpp"
#include "dsp/dsp_reference.hpp"
#include "dsp/fir.hpp"
#include "dsp/modulator.hpp"
#include "dsp/nco.hpp"
#include "dsp/nco_fast.hpp"
#include "dsp/spectrum.hpp"
#include "dsp/spectrum_block.hpp"
#include "source/iq_source.hpp"
#include "ref_dsp.hpp"
#include "test_check.hpp"

using cascade::dsp::DemodMode;
using cascade::dsp::FirDecimator;
using cascade::dsp::WindowType;
using cascade::dsp::windowedSincLowpass;
using dspsig::Rng;
using dspsig::Signal;
using refdsp::RefFirDecimator;

namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

// Running difference statistics over many chunks (the 60 s and 10 minute runs are never held in memory).
struct Acc {
    // differing: components whose BITS differ. over1e9: components that differ by more than 1e-9 (visible: a float unit in the last place at any magnitude above
    // 0.016; a flip of the float rounding). The bits differ in millions of components near a zero crossing, where the values are 1e-16 and the float's own resolution
    // is finer than the double noise of 1e-15: those are not visible differences and are not what a bound should count.
    std::size_t components = 0, differing = 0, over1e12 = 0, over1e9 = 0;
    double maxAbs = 0.0, sig = 0.0, dif = 0.0;
    void add(const std::complex<float>* ref, const std::complex<float>* got, std::size_t n) {
        const float* r = reinterpret_cast<const float*>(ref);
        const float* g = reinterpret_cast<const float*>(got);
        for (std::size_t i = 0; i < 2 * n; ++i) {
            const double e = static_cast<double>(r[i]) - static_cast<double>(g[i]);
            if (std::memcmp(&r[i], &g[i], sizeof(float)) != 0) { ++differing; }
            maxAbs = std::max(maxAbs, std::fabs(e));
            if (std::fabs(e) > 1e-12) { ++over1e12; }
            if (std::fabs(e) > 1e-9) { ++over1e9; }
            sig += static_cast<double>(r[i]) * static_cast<double>(r[i]);
            dif += e * e;
        }
        components += 2 * n;
    }
    double sdrDb() const { return dif <= 0.0 ? 400.0 : 10.0 * std::log10(sig / dif); }
    double fraction() const { return components ? static_cast<double>(differing) / static_cast<double>(components) : 0.0; }
    double visibleFraction() const { return components ? static_cast<double>(over1e9) / static_cast<double>(components) : 0.0; }
};

// ---- E1k: the FIR kernel ------------------------------------------------------------------------------------------------------------------

struct TapSet {
    const char* name;
    std::size_t taps;
    std::size_t decim;
    double cutoff;       // normalised to the sample rate the filter sees
    WindowType window;
};

// The tap sets the chain actually builds (docs/PERF section 2): the channel filter at 2 MS/s, 1 MS/s and 500 kS/s, the stereo pilot and audio
// filters, the RDS filter, the narrow sideband stages, a staged 10 MS/s first stage, and the 11-tap minimum.
const TapSet kTapSets[] = {
    {"channel 335x10 (2 MS/s)", 335, 10, 87500.0 / 2.0e6, WindowType::BlackmanHarris},
    {"channel 169x5 (1 MS/s)", 169, 5, 87500.0 / 1.0e6, WindowType::BlackmanHarris},
    {"channel 43x2 (500 kS/s)", 43, 2, 87500.0 / 0.5e6, WindowType::BlackmanHarris},
    {"stereo pilot 185x1", 185, 1, 1800.0 / 200000.0, WindowType::Hamming},
    {"stereo audio 165x1", 165, 1, 17000.0 / 200000.0, WindowType::Hamming},
    {"rds 413x10", 413, 10, 3000.0 / 200000.0, WindowType::Hamming},
    {"sideband 559x1", 559, 1, 1500.0 / 200000.0, WindowType::BlackmanHarris},
    {"sideband 1101x1", 1101, 1, 1500.0 / 200000.0, WindowType::Hamming},
    {"staged 75x5 (10 MS/s)", 75, 5, 0.09, WindowType::BlackmanHarris},
    {"minimum 11x1", 11, 1, 0.2, WindowType::Hamming},
};

std::vector<std::complex<float>> randomIq(std::size_t n, std::uint64_t seed) {
    Rng rng(seed);
    std::vector<std::complex<float>> v(n);
    for (auto& s : v) { s = {rng.uni(), rng.uni()}; }
    return v;
}

// Runs the same input through the product's FirDecimator and the reference with the input cut into blocks of `block` samples (the first call starts
// `startSkew` samples into an odd-float-offset buffer, so the input pointer is only 4-byte aligned), and returns whether every output matches in bits.
bool kernelMatches(const TapSet& ts, std::size_t block, const std::vector<std::complex<float>>& input, std::size_t* outputs) {
    const std::vector<float> taps = windowedSincLowpass(ts.taps, ts.cutoff, ts.window);
    FirDecimator fast(taps, ts.decim);
    RefFirDecimator ref(taps, ts.decim);
    // an input buffer at an odd float offset: the complex samples are 4-byte aligned only
    std::vector<float> raw(2 * input.size() + 1);
    std::memcpy(raw.data() + 1, input.data(), input.size() * sizeof(std::complex<float>));
    const std::complex<float>* base = reinterpret_cast<const std::complex<float>*>(raw.data() + 1);
    std::vector<std::complex<float>> a(fast.outputCapacity(block)), b(ref.outputCapacity(block));
    std::size_t total = 0;
    bool same = true;
    for (std::size_t pos = 0; pos < input.size(); pos += block) {
        const std::size_t n = std::min(block, input.size() - pos);
        const std::size_t na = fast.process(base + pos, n, a.data());
        const std::size_t nb = ref.process(base + pos, n, b.data());
        if (na != nb || std::memcmp(a.data(), b.data(), na * sizeof(std::complex<float>)) != 0) {
            same = false;
            break;
        }
        total += na;
    }
    *outputs = total;
    return same;
}

// ---- E1c: the whole chain -----------------------------------------------------------------------------------------------------------------

struct ChainRun {
    demodchain::Feeds feeds;
    std::uint64_t hAudio = 0, hDecoder = 0, hIf = 0, hMpx = 0;
};

ChainRun runChain(DemodMode mode, Signal::Kind kind, double seconds, unsigned referenceMask, int pilotToggleMs = 0, double rate = 2.0e6, bool forceMono = false) {
    constexpr std::size_t kBlock = 1024;
    cascade::dsp::setReferenceKernels(referenceMask);
    demodchain::Chain chain(rate, mode, 300000.0, /*keep=*/true);
    chain.stereo().setForceMono(forceMono);
    Signal sig(kind, rate, pilotToggleMs);
    std::vector<std::complex<float>> in(kBlock);
    const std::size_t blocks = static_cast<std::size_t>(seconds * rate / static_cast<double>(kBlock));
    for (std::size_t b = 0; b < blocks; ++b) {
        sig.fill(in.data(), kBlock);
        chain.process(in.data(), kBlock);
    }
    cascade::dsp::setReferenceKernels(0u);
    ChainRun r;
    r.feeds = std::move(chain.feeds());
    r.hAudio = demodchain::hashBits(r.feeds.audioR.data(), r.feeds.audioR.size(), demodchain::hashBits(r.feeds.audioL.data(), r.feeds.audioL.size()));
    r.hDecoder = demodchain::hashBits(r.feeds.decoder48.data(), r.feeds.decoder48.size());
    r.hIf = demodchain::hashBits(r.feeds.ifIq.data(), r.feeds.ifIq.size());
    r.hMpx = demodchain::hashBits(r.feeds.mpx192.data(), r.feeds.mpx192.size());
    return r;
}

const char* modeWord(DemodMode m) {
    switch (m) {
        case DemodMode::NFM: return "NFM";
        case DemodMode::WFM: return "WFM";
        case DemodMode::AM: return "AM";
        case DemodMode::DSB: return "DSB";
        case DemodMode::USB: return "USB";
        case DemodMode::LSB: return "LSB";
        case DemodMode::CW: return "CW";
        case DemodMode::RAW: return "RAW";
    }
    return "?";
}

// Hands over exactly what the test grants, one deterministic xorshift block at a time (the same stream as `grantedSample`).
class GrantSource final : public cascade::source::IqSource {
public:
    bool start() override { return true; }
    void stop() override {}
    bool running() const override { return true; }
    bool selfPaced() const override { return true; }
    double sampleRateHz() const override { return 2000000.0; }
    bool setSampleRateHz(double) override { return false; }
    double centerFrequencyHz() const override { return 100.0e6; }
    bool setCenterFrequencyHz(double) override { return true; }
    const char* name() const override { return "Granted test source"; }
    const char* lastError() const override { return ""; }
    std::size_t read(std::complex<float>* dst, std::size_t n) override {
        const std::size_t give = std::min<std::size_t>(granted_.load(std::memory_order_acquire), n);
        for (std::size_t i = 0; i < give; ++i) { dst[i] = next(); }
        granted_.fetch_sub(give, std::memory_order_acq_rel);
        return give;
    }
    void grant(std::size_t n) { granted_.fetch_add(n, std::memory_order_acq_rel); }
    // the same sequence, for the reference
    static std::complex<float> sampleAt(Rng& rng) { return {0.3f * rng.uni(), 0.3f * rng.uni()}; }

private:
    std::complex<float> next() { return sampleAt(rng_); }
    std::atomic<std::size_t> granted_{0};
    Rng rng_{0xABCDEFull};
};

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // --- E1k. THE FIR KERNEL against the reference, every tap set, every block split --------------------------------------------------------
    {
        const std::vector<std::complex<float>> input = randomIq(150000, 42);
        const std::size_t blocks[] = {1, 3, 7, 1008, 1024, 8192};
        for (const TapSet& ts : kTapSets) {
            std::size_t outputs = 0;
            bool all = true;
            for (const std::size_t block : blocks) {
                // (a block of 1 sample through 1101 taps is 150000 calls of a 1100-sample copy: slow, so the 1-sample split runs on a shorter input)
                const std::vector<std::complex<float>> in = block == 1 ? std::vector<std::complex<float>>(input.begin(), input.begin() + 6000) : input;
                std::size_t n = 0;
                const bool same = kernelMatches(ts, block, in, &n);
                outputs += n;
                if (!same) { std::printf("E1k: %s block %zu DIFFERS\n", ts.name, block); }
                all = all && same;
                CHECK(same);
            }
            std::printf("E1k: %-26s %s over %zu outputs in 6 block splits\n", ts.name, all ? "bit-identical" : "DIFFERENT", outputs);
        }
    }

    // --- E3. THE SPECTRUM'S dB CONVERSION AT THE PULL (P3) -----------------------------------------------------------------------------------
    {
        // (a) the estimator: process() and powerToDb(processPower()) against the estimator as it was, 300 blocks, two averaging settings, memcmp.
        for (const float alpha : {1.0f, 0.5f}) {
            cascade::dsp::SpectrumEstimator now(1024, WindowType::BlackmanHarris), now2(1024, WindowType::BlackmanHarris);
            refdsp::RefSpectrumEstimator ref(1024, WindowType::BlackmanHarris);
            now.setAlpha(alpha);
            now2.setAlpha(alpha);
            ref.setAlpha(alpha);
            Rng rng(99);
            std::vector<std::complex<float>> in(1024);
            std::vector<float> a(1024), b(1024), r(1024), pw(1024);
            std::size_t same = 0;
            for (int blk = 0; blk < 300; ++blk) {
                for (auto& s : in) { s = {0.4f * rng.uni(), 0.4f * rng.uni()}; }
                if (blk % 50 == 7) { std::fill(in.begin(), in.end(), std::complex<float>(0.0f, 0.0f)); }   // an all-zero block: the floor
                ref.process(in.data(), r.data());
                now.process(in.data(), a.data());
                now2.processPower(in.data(), pw.data());
                cascade::dsp::SpectrumEstimator::powerToDb(pw.data(), b.data(), 1024);
                if (std::memcmp(r.data(), a.data(), sizeof(float) * 1024) == 0 && std::memcmp(r.data(), b.data(), sizeof(float) * 1024) == 0) { ++same; }
            }
            std::printf("E3a: estimator alpha %.1f: %zu of 300 frames bit-identical (process and processPower+powerToDb against the old estimator)\n", alpha, same);
            CHECK(same == 300u);
        }

        // (b) the Pipeline: frames pulled after random numbers of blocks (the window, the scanners and the rest pull when they like), each against
        // the reference estimator fed the very same samples. The averaging is the application's (alpha 0.5), so every block matters.
        {
            cascade::core::Pipeline::Config cfg;
            cfg.sampleRateHz = 2000000.0;
            cfg.fftSize = 1024;
            cfg.averagingAlpha = 0.5f;
            cfg.audioEnabled = false;
            cascade::core::Pipeline p(cfg);
            auto owned = std::make_unique<GrantSource>();
            GrantSource* src = owned.get();
            p.setSource(std::move(owned));
            p.start();
            refdsp::RefSpectrumEstimator ref(1024, WindowType::BlackmanHarris);
            ref.setAlpha(0.5f);
            Rng genRng(0xABCDEFull);   // the source's stream
            Rng pick(5);
            std::vector<std::vector<float>> refFrames;   // one per block
            std::vector<std::complex<float>> blockIn(1024);
            std::vector<float> db(1024);
            cascade::core::SpectrumFrame frame, frame2;
            std::uint64_t blocksGranted = 0;
            std::size_t matched = 0, pulls = 0, consumersAgree = 0, secondPullEmpty = 0;
            for (int pull = 0; pull < 200; ++pull) {
                const std::size_t k = 1 + static_cast<std::size_t>(pick.next() % 4);   // 1 to 4 blocks between pulls
                for (std::size_t j = 0; j < k; ++j) {
                    for (auto& s : blockIn) { s = GrantSource::sampleAt(genRng); }
                    ref.process(blockIn.data(), db.data());
                    refFrames.push_back(db);
                }
                src->grant(k * 1024);
                blocksGranted += k;
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                while (p.dspBlocks() < blocksGranted && std::chrono::steady_clock::now() < deadline) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
                CHECK(p.dspBlocks() >= blocksGranted);
                ++pulls;
                if (p.getLatestFrame(frame)) {
                    // the frame is the newest block's: its sequence number is the block count (one frame per block, counting from 1)
                    const std::size_t idx = static_cast<std::size_t>(frame.seq) - 1u;
                    if (idx < refFrames.size() && frame.dbBins.size() == 1024 && std::memcmp(frame.dbBins.data(), refFrames[idx].data(), 1024 * sizeof(float)) == 0) { ++matched; }
                    // a second consumer of the same frame gets the same dB (the first one's conversion is shared, not redone)
                    cascade::core::SpectrumFrame other;
                    if (p.getLatestFrame(other) && other.seq == frame.seq && other.dbBins == frame.dbBins) { ++consumersAgree; }
                    if (!p.getLatestFrame(frame)) { ++secondPullEmpty; }   // nothing newer: false, and frame untouched
                }
            }
            p.stop();
            std::printf("E3b: pipeline: %zu of %zu pulled frames bit-identical to the reference estimator; second consumer agreed %zu times; no-new-frame pull false %zu times\n",
                        matched, pulls, consumersAgree, secondPullEmpty);
            CHECK(matched == pulls);
        }

        // (c) the SpectrumBlock path: print a hash of the dB frames it hands out for a fixed input (compared by hand across the commit that moved its
        // logarithm to the pull: the same line before and after must read the same).
        {
            cascade::dsp::SpecConfig sc;
            sc.fftSize = 4096;
            sc.sampleRateHz = 2.0e6;
            sc.frameRateHz = 400.0;
            cascade::dsp::SpectrumBlock blk(sc);
            Rng rng(321);
            std::vector<std::complex<float>> in(1024);
            std::vector<float> out(4096);
            std::uint64_t h = 1469598103934665603ull;
            std::size_t frames = 0;
            for (int b = 0; b < 200; ++b) {
                for (auto& s : in) { s = {0.4f * rng.uni(), 0.4f * rng.uni()}; }
                blk.process(in.data(), in.size());
                std::uint64_t seq = 0;
                const std::size_t got = blk.latest(out.data(), out.size(), &seq);
                if (got > 0) {
                    h = demodchain::hashBits(out.data(), got, h);
                    ++frames;
                }
            }
            std::printf("E3c: spectrum block: %zu frames, hash %016llx\n", frames, static_cast<unsigned long long>(h));
            CHECK(frames > 20u);
        }
    }

    // --- E4. THE STEREO CARRIER NOT COMPUTED WHILE THE GATE IS SHUT (P4) ------------------------------------------------------------------------
    {
        struct Case {
            const char* name;
            Signal::Kind kind;
            int toggleMs;
            bool forceMono;
        };
        const Case cases[] = {
            {"no pilot (the generator's signal)", Signal::kGenerator, 0, false},
            {"a locked pilot (the gate opens after ~50 ms)", Signal::kStereoFm, 0, false},
            {"a pilot switched off and on every 300 ms", Signal::kStereoFm, 300, false},
            {"forced mono on a stereo multiplex", Signal::kStereoFm, 0, true},
        };
        for (const Case& cs : cases) {
            const ChainRun ref = runChain(DemodMode::WFM, cs.kind, 10.0, cascade::dsp::kRefStereoCarrier, cs.toggleMs, 2.0e6, cs.forceMono);
            const ChainRun now = runChain(DemodMode::WFM, cs.kind, 10.0, 0u, cs.toggleMs, 2.0e6, cs.forceMono);
            const refdsp::Diff dl = refdsp::compareFloats(ref.feeds.audioL, now.feeds.audioL);
            const refdsp::Diff dr = refdsp::compareFloats(ref.feeds.audioR, now.feeds.audioR);
            const refdsp::Diff dd = refdsp::compareFloats(ref.feeds.decoder48, now.feeds.decoder48);
            std::printf("E4: %-46s audio L %s R %s decoder %s (%zu frames, audio hash %016llx)\n", cs.name, dl.sameBits ? "bit-identical" : "DIFFERENT",
                        dr.sameBits ? "bit-identical" : "DIFFERENT", dd.sameBits ? "bit-identical" : "DIFFERENT", now.feeds.audioL.size(), static_cast<unsigned long long>(now.hAudio));
            CHECK(dl.sameBits && dr.sameBits && dd.sameBits);
            if (cs.forceMono || cs.kind == Signal::kGenerator) {   // the gate never opened: L == R exactly (the existing tests demand it too)
                CHECK(now.feeds.audioL == now.feeds.audioR);
            } else {
                CHECK(now.feeds.audioL != now.feeds.audioR);        // the gate opened: the stereo image is there
            }
        }
    }

    // --- E2. THE RECEIVE OSCILLATOR (P2) ------------------------------------------------------------------------------------------------------
    {
        constexpr double kRate = 2.0e6;
        constexpr std::size_t kChunk = 1u << 20;
        // (a) the oscillator alone, 60 s of signal (120 M samples) at nine normalised frequencies from zero through small to just under Nyquist, both
        // signs: FastNco against RefNco, one component at a time. Bounds: at most one float unit in the last place at 1.0 (6e-8, taken as 1.3e-7 for
        // 2 ulp), fewer than one component in a million different, signal-to-difference ratio above 130 dB.
        const double freqs[] = {0.0, 1.0e-6, 1.0e-3, 0.0123456789, 0.15, 1.0 / 3.0, 0.4999, -0.25, -0.49999};
        std::vector<std::complex<float>> a(kChunk), b(kChunk);
        for (const double f : freqs) {
            cascade::dsp::FastNco fast;
            refdsp::RefNco ref;
            fast.setFrequency(f);
            ref.setFrequency(f);
            Acc acc;
            const std::size_t total = static_cast<std::size_t>(60.0 * kRate);
            for (std::size_t done = 0; done < total; done += kChunk) {
                fast.generate(a.data(), kChunk);
                ref.generate(b.data(), kChunk);
                acc.add(b.data(), a.data(), kChunk);
            }
            std::printf("E2a-detail: components differing by more than 1e-12: %zu, by more than 1e-9 (a visible float rounding flip): %zu (%.2g)\n", acc.over1e12, acc.over1e9, acc.visibleFraction());
            std::printf("E2a: f =%+.7f cycles/sample (%+.1f kHz at 2 MS/s): 60 s, max |diff| %.3g, %zu of %zu components differ (%.2g), signal-to-difference %.1f dB, phase() %s\n",
                        f, f * kRate / 1000.0, acc.maxAbs, acc.differing, acc.components, acc.fraction(), acc.sdrDb(),
                        fast.phase() == ref.phase() ? "identical" : "DIFFERENT");
            CHECK(acc.maxAbs <= 1.3e-7);
            CHECK(acc.visibleFraction() < 1.0e-6);   // fewer than one component in a million visibly different
            CHECK(acc.sdrDb() > 130.0);
            CHECK(fast.phase() == ref.phase());   // the accumulator is Nco's, bit for bit, after 120 M steps
            if (f == 0.0) { CHECK(acc.differing == 0u); }
        }

        // (b) PHASE CONTINUITY across retunes and block boundaries: a script of retunes at awkward positions (none a multiple of 32), run in one call per
        // segment, in blocks of 1, 7, 31, 32, 33 and 1000, against RefNco. The new class against ITSELF must be bit-identical whatever the cut (the anchor
        // grid belongs to the stream, not to a call); phase() must equal RefNco's exactly after every segment; and the first sample after a retune is an
        // anchor, so it equals RefNco's bit for bit.
        {
            struct Seg { double f; std::size_t n; };
            const Seg script[] = {{0.1, 100003}, {-0.2, 77}, {0.3333333, 5000}, {0.0, 31}, {0.45, 12345}, {-0.001, 40000}, {0.123, 1}, {0.4, 99}};
            std::vector<std::complex<float>> whole;
            std::vector<std::vector<std::complex<float>>> cut(5);
            const std::size_t cuts[] = {1, 7, 31, 32, 33};
            refdsp::RefNco ref;
            cascade::dsp::FastNco oneShot;
            std::vector<cascade::dsp::FastNco> splitters(5);
            Acc acc;
            bool phaseSame = true, anchorExact = true;
            for (const Seg& s : script) {
                oneShot.setFrequency(s.f);
                ref.setFrequency(s.f);
                for (auto& sp : splitters) { sp.setFrequency(s.f); }
                std::vector<std::complex<float>> x(s.n), y(s.n);
                oneShot.generate(x.data(), s.n);
                ref.generate(y.data(), s.n);
                acc.add(y.data(), x.data(), s.n);
                if (std::memcmp(&x[0], &y[0], sizeof(std::complex<float>)) != 0) { anchorExact = false; }
                phaseSame = phaseSame && oneShot.phase() == ref.phase();
                whole.insert(whole.end(), x.begin(), x.end());
                for (std::size_t q = 0; q < 5; ++q) {
                    std::vector<std::complex<float>> z(s.n);
                    for (std::size_t pos = 0; pos < s.n; pos += cuts[q]) { splitters[q].generate(z.data() + pos, std::min(cuts[q], s.n - pos)); }
                    cut[q].insert(cut[q].end(), z.begin(), z.end());
                    phaseSame = phaseSame && splitters[q].phase() == ref.phase();
                }
            }
            bool splitsSame = true;
            for (std::size_t q = 0; q < 5; ++q) { splitsSame = splitsSame && cut[q].size() == whole.size() && std::memcmp(cut[q].data(), whole.data(), whole.size() * sizeof(std::complex<float>)) == 0; }
            std::printf("E2b: %zu retunes, 5 block cuts: cuts identical to one call %s, phase() identical to RefNco after every segment %s, first sample after each retune bit-identical to RefNco %s; max |diff| %.3g\n",
                        sizeof(script) / sizeof(script[0]), splitsSame ? "yes" : "NO", phaseSame ? "yes" : "NO", anchorExact ? "yes" : "NO", acc.maxAbs);
            CHECK(splitsSame);
            CHECK(phaseSame);
            CHECK(anchorExact);
            CHECK(acc.maxAbs <= 1.3e-7);
            // in-place mix equals out-of-place mix
            cascade::dsp::FastNco m1, m2;
            m1.setFrequency(0.17);
            m2.setFrequency(0.17);
            std::vector<std::complex<float>> in = randomIq(5000, 8), o1(5000), o2 = in;
            m1.mix(in.data(), o1.data(), 5000);
            m2.mix(o2.data(), o2.data(), 5000);
            CHECK(std::memcmp(o1.data(), o2.data(), 5000 * sizeof(std::complex<float>)) == 0);
            // an oscillator never given a frequency (the Vfo's default: no offset) is Nco's constant 1 + 0j, in generate and in mix, and again after reset()
            // (this was caught first by test_channel_bandwidth: a table that was zero past entry 0 silenced 31 samples in 32)
            cascade::dsp::FastNco idle;
            refdsp::RefNco idleRef;
            std::vector<std::complex<float>> g1(300), g2(300);
            idle.generate(g1.data(), 300);
            idleRef.generate(g2.data(), 300);
            CHECK(std::memcmp(g1.data(), g2.data(), 300 * sizeof(std::complex<float>)) == 0);
            std::vector<std::complex<float>> mx1(300), mx2(300), x300 = randomIq(300, 3);
            idle.reset();
            idleRef.reset();
            idle.mix(x300.data(), mx1.data(), 300);
            idleRef.mix(x300.data(), mx2.data(), 300);
            CHECK(std::memcmp(mx1.data(), mx2.data(), 300 * sizeof(std::complex<float>)) == 0);
        }

        // (c) DRIFT: ten minutes of samples (1.2 G) at one frequency, reported per minute against RefNco. The error must not grow: there is no
        // recurrence, every sample is one multiply from a library value at the accumulator's own phase, so the last minute reads like the first.
        {
            cascade::dsp::FastNco fast;
            refdsp::RefNco ref;
            fast.setFrequency(0.3);
            ref.setFrequency(0.3);
            double first = 0.0, last = 0.0, worstMax = 0.0, worstFrac = 0.0;
            for (int minute = 0; minute < 10; ++minute) {
                Acc acc;
                const std::size_t total = static_cast<std::size_t>(60.0 * kRate);
                for (std::size_t done = 0; done < total; done += kChunk) {
                    fast.generate(a.data(), kChunk);
                    ref.generate(b.data(), kChunk);
                    acc.add(b.data(), a.data(), kChunk);
                }
                if (minute == 0) { first = acc.sdrDb(); }
                last = acc.sdrDb();
                worstMax = std::max(worstMax, acc.maxAbs);
                worstFrac = std::max(worstFrac, acc.visibleFraction());
                std::printf("E2c: minute %2d of 10: max |diff| %.3g, visible flips (>1e-9) %zu of %zu (%.2g), bits differ in %.2g, signal-to-difference %.1f dB\n", minute + 1, acc.maxAbs, acc.over1e9, acc.components, acc.visibleFraction(), acc.fraction(), acc.sdrDb());
            }
            CHECK(worstMax <= 1.3e-7);
            CHECK(worstFrac < 1.0e-6);
            CHECK(last > 130.0 && first > 130.0);   // (the ratio itself moves with whether a rounding flip happened to land in a minute; the bounds above hold in EVERY minute)
            CHECK(fast.phase() == ref.phase());
        }

        // (d) THE CHAIN: the VFO's output (what the IF recorder gets), the multiplex feed, the decoder tap and the audio, with the receive oscillator on its library
        // path against FastNco, ten seconds of each signal: signal-to-difference above 130 dB on every feed.
        for (const Signal::Kind kind : {Signal::kGenerator, Signal::kStereoFm}) {
            const ChainRun ref = runChain(DemodMode::WFM, kind, 10.0, cascade::dsp::kRefNco);
            const ChainRun now = runChain(DemodMode::WFM, kind, 10.0, 0u);
            const refdsp::Diff di = refdsp::compareComplex(ref.feeds.ifIq, now.feeds.ifIq);
            const refdsp::Diff dm = refdsp::compareFloats(ref.feeds.mpx192, now.feeds.mpx192);
            const refdsp::Diff dd = refdsp::compareFloats(ref.feeds.decoder48, now.feeds.decoder48);
            const refdsp::Diff dl = refdsp::compareFloats(ref.feeds.audioL, now.feeds.audioL);
            std::printf("E2d: WFM %-10s: IF feed max |diff| %.3g, %.1f dB; multiplex %.3g, %.1f dB; decoder tap %.3g, %.1f dB; audio %.3g, %.1f dB\n",
                        kind == Signal::kGenerator ? "generator" : "stereo+RDS", di.maxAbs, di.sdrDb(), dm.maxAbs, dm.sdrDb(), dd.maxAbs, dd.sdrDb(), dl.maxAbs, dl.sdrDb());
            CHECK(di.sdrDb() > 130.0 && dd.sdrDb() > 130.0 && dl.sdrDb() > 130.0);
            if (kind == Signal::kStereoFm) { CHECK(dm.sdrDb() > 130.0); }
        }
    }

    // --- E5. NCO ITSELF AND THE TRANSMIT MODULATOR ARE UNTOUCHED ------------------------------------------------------------------------------
    // Nco is not edited by the pass and its transmit-side neighbour, the modulator, does not use FastNco. Proved two ways: Nco against the verbatim reference, bit for
    // bit; and a hash of the modulator's output for every mode over a fixed input - pure in-memory arithmetic, nothing is keyed or transmitted - printed as a log line,
    // which reads the same before and after the commit that added FastNco.
    {
        for (const double f : {0.0, 0.05, 0.3, -0.45}) {
            cascade::dsp::Nco real;
            refdsp::RefNco ref;
            real.setFrequency(f);
            ref.setFrequency(f);
            std::vector<std::complex<float>> x(1u << 20), y(1u << 20);
            real.generate(x.data(), x.size());
            ref.generate(y.data(), y.size());
            CHECK(std::memcmp(x.data(), y.data(), x.size() * sizeof(std::complex<float>)) == 0);
            CHECK(real.phase() == ref.phase());
        }
        std::uint64_t h = 1469598103934665603ull;
        for (int m = 0; m < cascade::dsp::kTxModeCount; ++m) {
            cascade::dsp::Modulator mod;
            mod.setSampleRateHz(48000.0);
            mod.setMode(static_cast<cascade::dsp::TxMode>(m));
            mod.setKeyed(true);   // (the modulator's own envelope state: arithmetic on a buffer, nothing is connected to a radio)
            Rng rng(77 + static_cast<std::uint64_t>(m));
            std::vector<float> audio(48000);
            for (std::size_t i = 0; i < audio.size(); ++i) { audio[i] = 0.4f * std::sin(0.13f * static_cast<float>(i)) + 0.1f * rng.uni(); }
            std::vector<std::complex<float>> out(audio.size());
            for (std::size_t pos = 0; pos < audio.size(); pos += 4800) { mod.process(audio.data() + pos, 4800, out.data() + pos); }
            h = demodchain::hashBits(out.data(), out.size(), h);
        }
        std::printf("E5: transmit modulator, all five modes, 1 s each: output hash %016llx\n", static_cast<unsigned long long>(h));
    }

    // --- E1c. THE WHOLE CHAIN, every mode, 10 s: the four feeds, optimised against reference, bit for bit ----------------------------------------
    // The same samples through the same chain twice in one process: once with the FIR kernel on its scalar reference path, once on the vector kernel.
    // Every output the pipeline produces from this phase - the audio, the audio decoder plugins' tap, the IF recorder's feed and the multiplex
    // recorder's - must match in every bit.
    {
        const DemodMode modes[] = {DemodMode::WFM, DemodMode::NFM, DemodMode::AM, DemodMode::DSB, DemodMode::USB, DemodMode::LSB, DemodMode::CW, DemodMode::RAW};
        for (const DemodMode mode : modes) {
            const Signal::Kind kind = mode == DemodMode::WFM ? Signal::kStereoFm : Signal::kGenerator;
            const ChainRun ref = runChain(mode, kind, 10.0, cascade::dsp::kRefFir);
            const ChainRun now = runChain(mode, kind, 10.0, 0u);
            const refdsp::Diff dl = refdsp::compareFloats(ref.feeds.audioL, now.feeds.audioL);
            const refdsp::Diff dr = refdsp::compareFloats(ref.feeds.audioR, now.feeds.audioR);
            const refdsp::Diff dd = refdsp::compareFloats(ref.feeds.decoder48, now.feeds.decoder48);
            const refdsp::Diff di = refdsp::compareComplex(ref.feeds.ifIq, now.feeds.ifIq);
            const refdsp::Diff dm = refdsp::compareFloats(ref.feeds.mpx192, now.feeds.mpx192);
            std::printf("E1c: %-3s audio %016llx decoder %016llx if %016llx mpx %016llx (%zu audio frames); reference-vs-new bit-identical: audio %s decoder %s if %s mpx %s\n",
                        modeName(mode), static_cast<unsigned long long>(now.hAudio), static_cast<unsigned long long>(now.hDecoder),
                        static_cast<unsigned long long>(now.hIf), static_cast<unsigned long long>(now.hMpx), now.feeds.audioL.size(),
                        dl.sameBits && dr.sameBits ? "yes" : "NO", dd.sameBits ? "yes" : "NO", di.sameBits ? "yes" : "NO", dm.sameBits ? "yes" : "NO");
            CHECK(now.feeds.audioL.size() > 400000u);
            CHECK(ref.hAudio == now.hAudio && ref.hDecoder == now.hDecoder && ref.hIf == now.hIf && ref.hMpx == now.hMpx);
            CHECK(dl.sameBits && dr.sameBits && dd.sameBits && di.sameBits && dm.sameBits);
            if (mode == DemodMode::WFM) { CHECK(!now.feeds.mpx192.empty()); }
        }
    }

    return testSummary("test_dsp_equivalence");
}
