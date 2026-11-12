// Fuzz run over every sigmeas block: random lengths (including 0 and 1), random sample rates
// (including 0, negative, NaN, tiny and huge), random parameters, and samples that are noise,
// constants, zeros, huge, tiny, spiked, or carry NaN and infinity. Nothing may crash, hang or throw;
// a result that claims to be valid must have finite headline numbers. Deterministic seeds.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <limits>
#include <vector>

#include "dsp/sigmeas_audio.hpp"
#include "dsp/sigmeas_bandwidth.hpp"
#include "dsp/sigmeas_bursts.hpp"
#include "dsp/sigmeas_cursors.hpp"
#include "dsp/sigmeas_framelog.hpp"
#include "dsp/sigmeas_histogram.hpp"
#include "dsp/sigmeas_hopchirp.hpp"
#include "dsp/sigmeas_modulation.hpp"
#include "dsp/sigmeas_symbols.hpp"
#include "dsp/sigmeas_varispeed.hpp"
#include "dsp/sigmeas_vstime.hpp"
#include "sigmeas_gen.hpp"
#include "test_check.hpp"

using namespace cascade::sigmeas;

namespace {

double pickRate(sg::Rng& r) {
    switch (static_cast<int>(r.uni() * 9.0)) {
        case 0: return 0.0;
        case 1: return -48000.0;
        case 2: return std::nan("");
        case 3: return 1e-5;
        case 4: return 1e12;
        case 5: return std::numeric_limits<double>::infinity();
        default: return 8000.0 * std::pow(2.0, std::floor(r.uni() * 6.0));   // 8 .. 256 k
    }
}

// n samples of a randomly chosen kind
std::vector<Cf> pickSignal(sg::Rng& r, std::size_t n) {
    std::vector<Cf> x(n, Cf(0, 0));
    const int kind = static_cast<int>(r.uni() * 8.0);
    const double scale = std::pow(10.0, -6.0 + 12.0 * r.uni());
    const double f = r.uni() * 0.5;
    for (std::size_t i = 0; i < n; ++i) {
        switch (kind) {
            case 0: x[i] = Cf(static_cast<float>(scale * r.gauss()), static_cast<float>(scale * r.gauss())); break;
            case 1: x[i] = Cf(static_cast<float>(scale), 0.0f); break;
            case 2: break;   // zeros
            case 3: {
                const double ph = sg::kTwoPi * f * static_cast<double>(i);
                x[i] = Cf(static_cast<float>(scale * std::cos(ph)), static_cast<float>(scale * std::sin(ph)));
                break;
            }
            case 4: x[i] = (r.uni() < 0.01) ? Cf(3e30f, -3e30f) : Cf(static_cast<float>(0.1 * r.gauss()), 0.0f); break;
            case 5: x[i] = Cf((i / 64) % 2 ? 1.0f : 0.0f, 0.0f); break;   // an on/off pattern
            case 6: x[i] = Cf(static_cast<float>(1e-30 * r.gauss()), static_cast<float>(1e-30 * r.gauss())); break;
            default: {
                const double ph = sg::kTwoPi * (f + 0.1 * ((i / 200) % 2)) * static_cast<double>(i);   // FSK-like
                x[i] = Cf(static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph)));
            }
        }
        if (r.uni() < 0.0005) { x[i] = Cf(std::nanf(""), 0.0f); }
        else if (r.uni() < 0.0003) { x[i] = Cf(0.0f, std::numeric_limits<float>::infinity()); }
    }
    return x;
}

bool fin(double v) { return std::isfinite(v); }

}  // namespace

int main() {
    sg::Rng rng(20261111);
    int validCount = 0, runs = 0;
    for (int it = 0; it < 400; ++it) {
        ++runs;
        const std::size_t n = static_cast<std::size_t>(rng.uni() < 0.1 ? rng.uni() * 3.0 : rng.uni() * 6000.0);
        const double rate = pickRate(rng);
        const std::vector<Cf> x = pickSignal(rng, n);
        const Cf* p = x.empty() ? nullptr : x.data();

        // each block, default parameters and a randomised set
        {
            BandwidthParams bp;
            if (rng.uni() < 0.5) {
                bp.obwFraction = rng.uni() * 1.3;
                bp.detectMarginDb = (rng.uni() - 0.2) * 20.0;
                bp.bandLowHz = rng.uni() < 0.5 ? kNaN : (rng.uni() - 0.5) * rate;
                bp.bandHighHz = rng.uni() < 0.5 ? kNaN : (rng.uni() - 0.5) * rate;
                bp.nfft = rng.uni() < 0.5 ? 0 : static_cast<std::size_t>(rng.uni() * 5000.0);
            }
            const BandwidthResult r = measureBandwidth(p, n, rate, bp);
            if (r.valid) { ++validCount; CHECK(fin(r.obwHz) && fin(r.centreHz) && fin(r.snrDb) && r.obwHz >= 0.0); }
        }
        {
            ModParams mp;
            if (rng.uni() < 0.5) { mp.audioBandwidthHz = (rng.uni() - 0.2) * rate; mp.peakPercentile = rng.uni() * 0.4; }
            const FmResult f = measureFmDeviation(p, n, rate, mp);
            if (f.valid) { ++validCount; CHECK(fin(f.peakDeviationHz) && fin(f.carrierOffsetHz) && fin(f.rmsDeviationHz)); }
            const AmResult a = measureAmDepth(p, n, rate, mp);
            if (a.valid) { ++validCount; CHECK(fin(a.depth) && fin(a.carrierLevel)); }
        }
        {
            SymbolParams sp;
            if (rng.uni() < 0.5) {
                sp.tolerance = (rng.uni() - 0.1);
                sp.minSeparation = rng.uni() * 1.2;
                sp.maxBaudHz = (rng.uni() - 0.2) * rate;
                sp.minBaudHz = rng.uni() < 0.7 ? 0.0 : rng.uni() * 100.0;
                sp.maxBits = static_cast<std::size_t>(rng.uni() * 100.0);
            }
            const FskResult f = analyseFsk(p, n, rate, sp);
            if (f.valid) { ++validCount; CHECK(fin(f.baudHz) && fin(f.shiftHz) && f.baudHz > 0.0); }
            const OokResult o = sliceOok(p, n, rate, sp);
            if (o.valid) { ++validCount; CHECK(fin(o.baudHz) && o.baudHz > 0.0); }
        }
        {
            // runs of random length, some bad
            std::vector<double> runsv(static_cast<std::size_t>(rng.uni() * 40.0));
            for (double& v : runsv) { v = rng.uni() < 0.05 ? (rng.uni() < 0.5 ? -1.0 : std::nan("")) : 1.0 + 30.0 * rng.uni(); }
            const PeriodFit pf = fitSymbolPeriod(runsv, rng.uni() * 5.0 - 1.0, rng.uni() - 0.2);
            if (pf.valid) { ++validCount; CHECK(fin(pf.period) && pf.period > 0.0); }
            const PeriodFit pg = refineCursorPeriod(runsv, (rng.uni() - 0.2) * 40.0);
            if (pg.valid) { CHECK(fin(pg.period)); }
            const std::vector<double> track(static_cast<std::size_t>(rng.uni() * 300.0), 0.5);
            extractSymbols(track, (rng.uni() - 0.5) * 100.0, (rng.uni() - 0.2) * 40.0, 50);
        }
        {
            VsTimeParams vp;
            if (rng.uni() < 0.5) {
                vp.centreHz = (rng.uni() - 0.5) * rate * 1.2;
                vp.bandwidthHz = (rng.uni() - 0.1) * rate * 0.3;
                vp.maxPoints = static_cast<std::size_t>(rng.uni() * 200.0);
                vp.removeLinearPhase = rng.uni() < 0.5;
            }
            const VsTimeResult v = computeVsTime(p, n, rate, vp);
            if (v.valid) {
                ++validCount;
                CHECK(v.amplitude.size() == v.freqHz.size() && v.freqHz.size() == v.phaseRad.size());
                CHECK(v.amplitude.empty() || (fin(v.freqHz[v.freqHz.size() / 2]) && fin(v.phaseRad.back())));
            }
        }
        {
            BurstParams bp;
            if (rng.uni() < 0.5) {
                bp.blockSamples = static_cast<std::size_t>(rng.uni() * 200.0);
                bp.minContrastDb = (rng.uni() - 0.1) * 30.0;
                bp.minGapBlocks = static_cast<std::size_t>(rng.uni() * 5.0);
                bp.minBurstBlocks = static_cast<std::size_t>(rng.uni() * 5.0);
                bp.maxBursts = static_cast<std::size_t>(rng.uni() * 50.0);
            }
            const BurstReport b = detectBursts(p, n, rate, bp);
            if (b.valid) {
                ++validCount;
                for (const Burst& bu : b.bursts) { CHECK(fin(bu.startS) && fin(bu.lengthS) && bu.lengthS >= 0.0 && fin(bu.freqHz)); }
            }
        }
        {
            TrackParams tp;
            if (rng.uni() < 0.5) {
                tp.frameSamples = static_cast<std::size_t>(rng.uni() < 0.5 ? 64.0 * std::pow(2.0, std::floor(rng.uni() * 5.0)) : rng.uni() * 700.0);
                tp.hopSamples = static_cast<std::size_t>(rng.uni() * 100.0);
                tp.minSnrDb = (rng.uni() - 0.1) * 20.0;
            }
            const FreqTrack t = trackPeak(p, n, rate, tp);
            if (t.valid) { ++validCount; }
            const HopReport h = analyseHops(t);
            if (h.valid) { CHECK(fin(h.hopRateHz) || std::isnan(h.hopRateHz)); }
            const ChirpReport c = analyseChirps(t);
            if (c.valid) { CHECK(fin(c.slopeHzPerS) && !c.sweeps.empty()); }
        }
        {
            std::vector<float> a(n);
            for (std::size_t i = 0; i < n; ++i) { a[i] = x[i].real(); }
            AudioParams ap;
            if (rng.uni() < 0.5) {
                ap.toneHz = (rng.uni() - 0.2) * rate * 0.5;
                ap.maxHarmonic = static_cast<std::size_t>(rng.uni() * 70.0);
                ap.bandLowHz = (rng.uni() - 0.1) * 1000.0;
                ap.bandHighHz = (rng.uni() - 0.1) * rate;
                ap.nfft = rng.uni() < 0.5 ? 0 : static_cast<std::size_t>(rng.uni() * 5000.0);
            }
            const AudioResult r = measureAudio(a.empty() ? nullptr : a.data(), a.size(), rate, ap);
            if (r.valid) { ++validCount; CHECK(fin(r.sinadDb) && fin(r.thdDb) && fin(r.enob) && fin(r.toneHz)); }
            const HistResult h = amplitudeHistogram(a.empty() ? nullptr : a.data(), a.size(), HistParams{static_cast<std::size_t>(rng.uni() * 200.0), rng.uni() < 0.5 ? kNaN : -1.0, rng.uni() < 0.5 ? kNaN : 1.0});
            if (h.valid) {
                ++validCount;
                double s = 0.0;
                for (double pr : h.probability) { s += pr; }
                CHECK(fin(s) && s <= 1.0 + 1e-9);
            }
            const HistResult hm = magnitudeHistogram(p, n);
            if (hm.valid) { CHECK(fin(hm.rms)); }
            const CcdfResult cc = powerCcdf(p, n, (rng.uni() - 0.1) * 30.0, rng.uni() * 3.0);
            if (cc.valid) { CHECK(!cc.probability.empty() && fin(cc.paprDb)); }
            // varispeed over the same samples with random control
            Varispeed vs;
            if (vs.setSource(a.empty() ? nullptr : a.data(), a.size())) {
                vs.setSpeed((rng.uni() - 0.5) * 40.0);
                vs.setSlew(rng.uni() * 0.01);
                vs.seek((rng.uni() - 0.2) * static_cast<double>(n) * 1.2);
                std::vector<float> out(300);
                const std::size_t got = vs.read(out.data(), out.size());
                CHECK(got <= out.size());
            }
        }
        if (it % 20 == 0) {
            // drift search over a short random span (it is the expensive one)
            const std::size_t m = std::min<std::size_t>(n, 2048);
            const DriftResult d = searchDrift(m ? x.data() : nullptr, m, rate);
            if (d.valid) { CHECK(fin(d.driftHzPerS) && fin(d.startFreqHz)); }
        }
        {
            // tone energies for the confidence, and the frame log
            std::vector<double> e(static_cast<std::size_t>(rng.uni() * 20.0));
            for (double& v : e) { v = rng.uni() < 0.05 ? std::nan("") : std::pow(10.0, -8.0 + 16.0 * rng.uni()); }
            const SymbolConfidence c = symbolConfidence(e.empty() ? nullptr : e.data(), e.size());
            if (c.valid) { CHECK(c.posterior >= 0.0 && c.posterior <= 1.0 + 1e-12); }
            FrameLog log(static_cast<std::size_t>(rng.uni() * 5.0));
            for (int k = 0; k < 10; ++k) {
                log.push(FrameRecord::withCrc(k, 0.0, std::vector<std::uint8_t>(static_cast<std::size_t>(rng.uni() * 5.0), 1),
                                              static_cast<std::uint32_t>(rng.uni() * 3.0), 1u));
            }
            CHECK(log.counts().goodTotal + log.counts().badTotal == 10);
        }
        {
            // the cursors under random commands
            CursorSet8 cs;
            for (int k = 0; k < 20; ++k) {
                const std::size_t idx = static_cast<std::size_t>(rng.uni() * 10.0);
                const double fq = rng.uni() < 0.1 ? std::nan("") : (rng.uni() - 0.5) * 1e6;
                switch (static_cast<int>(rng.uni() * 5.0)) {
                    case 0: cs.setCursor(idx, fq); break;
                    case 1: cs.setSpacing(fq); break;
                    case 2: cs.setMode(static_cast<CursorMode>(static_cast<int>(rng.uni() * 3.0))); break;
                    case 3: cs.setReference(idx); break;
                    default: cs.setRange(fq, fq + 1000.0);
                }
            }
            const auto ro = cs.readout();
            for (const auto& c : ro) { CHECK(fin(c.freqHz)); }
        }
    }
    CHECK(runs == 400);
    CHECK(validCount > 100);   // the fuzz reaches the valid paths, not only the refusals
    return testSummary("test_sigmeas_fuzz");
}
