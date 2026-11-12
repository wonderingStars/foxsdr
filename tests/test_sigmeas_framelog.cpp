// Tests for dsp/sigmeas_framelog.hpp: per-symbol and per-character confidence of non-coherent MFSK
// from tone energies (calibration checked by Monte Carlo against the real error rate), and the frame
// log that keeps frames with CRC errors.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "dsp/sigmeas_framelog.hpp"

#include <cmath>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "sigmeas_gen.hpp"
#include "test_check.hpp"

using namespace cascade::sigmeas;

int main() {
    // --- the Bessel evaluation against the C++ special function ----------------------------------------------------------
    {
        double worst = 0.0;
        for (double x = 0.0; x <= 100.0; x += 0.25) {
            worst = std::max(worst, std::fabs(detail::logI0(x) - std::log(std::cyl_bessel_i(0.0, x))));
        }
        CHECK(worst < 1e-5);
    }
    // --- calibration: M = 16 tones, tone-to-noise a^2 in {6, 10, 16}; the mean posterior is the accuracy ------
    for (double a2 : {6.0, 10.0, 16.0}) {
        sg::Rng rng(91);
        const std::size_t M = 16, trials = 20000;
        std::size_t right = 0;
        double sumPost = 0.0, sumRight = 0.0, sumWrong = 0.0;
        std::size_t nWrong = 0;
        std::size_t hiN = 0, hiRight = 0, loN = 0, loRight = 0;
        std::vector<double> e(M);
        for (std::size_t t = 0; t < trials; ++t) {
            const std::size_t sent = static_cast<std::size_t>(rng.uni() * static_cast<double>(M)) % M;
            for (std::size_t j = 0; j < M; ++j) {
                const double re = rng.gauss() / std::sqrt(2.0) + (j == sent ? std::sqrt(a2) : 0.0);
                const double im = rng.gauss() / std::sqrt(2.0);
                e[j] = re * re + im * im;
            }
            const SymbolConfidence c = symbolConfidence(e.data(), M);
            CHECK(c.valid);
            if (!c.valid) { continue; }
            const bool ok = static_cast<std::size_t>(c.best) == sent;
            right += ok ? 1 : 0;
            sumPost += c.posterior;
            if (ok) { sumRight += c.posterior; } else { sumWrong += c.posterior; ++nWrong; }
            if (c.posterior >= 0.9) { ++hiN; hiRight += ok ? 1 : 0; }
            if (c.posterior < 0.5) { ++loN; loRight += ok ? 1 : 0; }
        }
        const double acc = static_cast<double>(right) / static_cast<double>(trials);
        // the mean posterior is the accuracy within a few points (a^2 is estimated per symbol)
        CHECK_NEAR(sumPost / static_cast<double>(trials), acc, 0.06);
        CHECK(nWrong == 0 || sumRight / static_cast<double>(right) > sumWrong / static_cast<double>(nWrong) + 0.15);
        CHECK(hiN > 0 && static_cast<double>(hiRight) / static_cast<double>(hiN) > 0.93);
        CHECK(loN == 0 || static_cast<double>(loRight) / static_cast<double>(loN) < 0.75);
        std::printf("  a2=%.0f accuracy %.3f mean posterior %.3f  high-confidence n=%zu right %.3f  low n=%zu right %.3f\n", a2, acc,
                    sumPost / static_cast<double>(trials), hiN, hiN ? static_cast<double>(hiRight) / hiN : 0.0, loN,
                    loN ? static_cast<double>(loRight) / loN : 0.0);
    }
    // --- the score rises with the winner's margin; ties and tiny margins score low --------------------------------------
    {
        std::vector<double> e(8, 1.0);
        double prev = 0.0;
        for (double w : {1.5, 3.0, 6.0, 12.0, 24.0, 48.0}) {
            e[3] = w;
            const SymbolConfidence c = symbolConfidence(e.data(), e.size());
            CHECK(c.valid && c.best == 3);
            CHECK(c.posterior > prev);
            CHECK(c.marginDb > 0.0);
            prev = c.posterior;
        }
        CHECK(prev > 0.99);
        std::vector<double> flat(8, 1.0);
        const SymbolConfidence f = symbolConfidence(flat.data(), flat.size());
        CHECK(f.valid && f.posterior < 0.3);
        CHECK_NEAR(f.marginDb, 0.0, 1e-9);
        // two tones at once (a collision): the margin is small, so is the confidence
        std::vector<double> two(16, 1.0);
        two[2] = 20.0;
        two[9] = 19.0;
        const SymbolConfidence t = symbolConfidence(two.data(), two.size());
        std::vector<double> clean = two;
        clean[9] = 1.0;
        const SymbolConfidence cl = symbolConfidence(clean.data(), clean.size());
        CHECK(t.valid && cl.valid && t.marginDb < 0.5);
        CHECK(t.posterior < 0.75 && cl.posterior > 0.95);   // the collision scores far under the same winner alone
        // M = 2 works
        const double m2[2] = {9.0, 1.0};
        CHECK(symbolConfidence(m2, 2).valid && symbolConfidence(m2, 2).best == 0);
    }
    // --- characters ---------------------------------------------------------------------------------------------------------
    {
        const double s[3] = {0.9, 0.8, 0.5};
        const CharConfidence c = characterConfidence(s, 3);
        CHECK(c.valid);
        CHECK_NEAR(c.product, 0.36, 1e-12);
        CHECK_NEAR(c.minimum, 0.5, 1e-12);
        CHECK(!characterConfidence(nullptr, 3).valid);
        CHECK(!characterConfidence(s, 0).valid);
        const double bad[2] = {0.9, 1.5};
        CHECK(!characterConfidence(bad, 2).valid);
        const double nanv[2] = {0.9, std::nan("")};
        CHECK(!characterConfidence(nanv, 2).valid);
        const std::vector<double> chars = {0.99, 0.4, 0.95, std::nan(""), 0.8};
        const std::vector<std::size_t> low = lowConfidence(chars, 0.9);
        CHECK(low == (std::vector<std::size_t>{1, 3, 4}));
        CHECK(lowConfidence(chars, std::nan("")).empty());
    }
    // --- failure paths of the confidence ------------------------------------------------------------------------------------------
    {
        std::vector<double> e(8, 1.0);
        CHECK(std::string(symbolConfidence(nullptr, 8).reason) == why::kEmpty);
        CHECK(std::string(symbolConfidence(e.data(), 1).reason) == why::kBadParam);
        CHECK(std::string(symbolConfidence(e.data(), 0).reason) == why::kBadParam);
        std::vector<double> big(300, 1.0);
        CHECK(std::string(symbolConfidence(big.data(), 300).reason) == why::kBadParam);
        e[2] = std::nan("");
        CHECK(std::string(symbolConfidence(e.data(), 8).reason) == why::kNonFinite);
        e[2] = std::numeric_limits<double>::infinity();
        CHECK(std::string(symbolConfidence(e.data(), 8).reason) == why::kNonFinite);
        e[2] = -1.0;
        CHECK(std::string(symbolConfidence(e.data(), 8).reason) == why::kNonFinite);
        std::vector<double> zero(8, 0.0);
        CHECK(std::string(symbolConfidence(zero.data(), 8).reason) == why::kNoSignal);
        // a huge energy does not overflow the Bessel evaluation
        std::vector<double> huge(8, 1.0);
        huge[0] = 1e12;
        const SymbolConfidence h = symbolConfidence(huge.data(), 8);
        CHECK(h.valid && h.posterior > 0.999 && std::isfinite(h.posterior));
    }

    // --- the frame log -------------------------------------------------------------------------------------------------------------------------
    {
        FrameLog log(4);
        CHECK(log.capacity() == 4 && log.keepBad());
        auto good = [](double t) { return FrameRecord::withCrc(t, 1000.0, {1, 2, 3}, 0xABCD, 0xABCD, 0.9); };
        auto bad = [](double t) { return FrameRecord::withCrc(t, 1000.0, {9, 9}, 0x1234, 0x1235, 0.4); };
        CHECK(good(0).crcOk && !bad(0).crcOk);
        CHECK(log.push(good(1.0)) == 1);
        CHECK(log.push(bad(2.0)) == 2);
        CHECK(log.push(good(3.0)) == 3);
        auto c = log.counts();
        CHECK(c.goodTotal == 2 && c.badTotal == 1 && c.held == 3 && c.heldBad == 1);
        auto all = log.snapshot();
        CHECK(all.size() == 3);
        if (all.size() == 3) {
            CHECK(all[0].seq == 1 && all[1].seq == 2 && all[2].seq == 3);
            CHECK(!all[1].crcOk && all[1].crcReceived == 0x1234 && all[1].crcComputed == 0x1235);
            CHECK(all[1].bytes == (std::vector<std::uint8_t>{9, 9}));
        }
        auto onlyGood = log.snapshot(false);
        CHECK(onlyGood.size() == 2 && onlyGood[0].crcOk && onlyGood[1].crcOk);
        // capacity: the oldest leave, the totals stay
        log.push(good(4.0));
        log.push(good(5.0));
        log.push(bad(6.0));
        c = log.counts();
        CHECK(c.held == 4 && c.goodTotal == 4 && c.badTotal == 2);
        CHECK(log.snapshot().front().seq == 3);
        // keepBad off: failed frames are dropped but counted
        log.setKeepBad(false);
        CHECK(!log.keepBad());
        const std::uint64_t seq = log.push(bad(7.0));
        CHECK(seq == 7);
        c = log.counts();
        CHECK(c.badTotal == 3 && c.held == 4);
        CHECK(log.snapshot().back().seq == 6);
        // a frame without a check value is good, with the flag saying so
        const FrameRecord nc = FrameRecord::withoutCrc(8.0, 500.0, {7});
        CHECK(nc.crcOk && !nc.hasCrc);
        // clear
        log.clear();
        c = log.counts();
        CHECK(c.held == 0 && c.goodTotal == 0 && c.badTotal == 0);
        CHECK(log.push(good(9.0)) == 1);
        // capacity 0 becomes 1
        FrameLog tiny(0);
        CHECK(tiny.capacity() == 1);
        tiny.push(good(1.0));
        tiny.push(good(2.0));
        CHECK(tiny.snapshot().size() == 1 && tiny.snapshot()[0].seq == 2);
    }
    // --- concurrency: four writers and a reader; the totals add up and nothing tears -----------------------------------------------------------------
    {
        FrameLog log(64);
        std::vector<std::thread> th;
        for (int w = 0; w < 4; ++w) {
            th.emplace_back([&log, w] {
                for (int i = 0; i < 2000; ++i) {
                    const bool ok = (i + w) % 3 != 0;
                    log.push(FrameRecord::withCrc(i, w, std::vector<std::uint8_t>(8, static_cast<std::uint8_t>(w)),
                                                  ok ? 1u : 2u, 1u));
                }
            });
        }
        bool torn = false;
        std::thread reader([&log, &torn] {
            for (int i = 0; i < 400; ++i) {
                const auto s = log.snapshot();
                for (const FrameRecord& r : s) {
                    for (std::uint8_t b : r.bytes) { if (b != static_cast<std::uint8_t>(r.freqHz)) { torn = true; } }
                }
                for (std::size_t k = 1; k < s.size(); ++k) { if (s[k].seq <= s[k - 1].seq) { torn = true; } }
            }
        });
        for (auto& t : th) { t.join(); }
        reader.join();
        const auto c = log.counts();
        CHECK(!torn);
        CHECK(c.goodTotal + c.badTotal == 8000);
        CHECK(c.held == 64);
    }
    return testSummary("test_sigmeas_framelog");
}
