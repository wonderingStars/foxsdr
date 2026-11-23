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
