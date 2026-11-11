// A program registered as a test: it times the existing DSP blocks on this machine (core/dsp_benchmark.hpp) and prints the table.
// It PASSES WHEN IT RUNS TO THE END with every item timed and finite; it never fails on a speed.
//
// By hand, longer and quieter on a idle machine:   test_dsp_benchmark --seconds=2
// (or the environment variable COREF_BENCH_SECONDS). The default is 0.2 s per item, about 8 s in all.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/dsp_benchmark.hpp"

#include "test_check.hpp"

#include <cstdlib>
#include <cstring>

namespace {

namespace bench = cascade::core::bench;

void testFormatting() {
    std::vector<bench::BenchResult> v(2);
    v[0].group = "FFT"; v[0].name = "forward 1024"; v[0].ok = true; v[0].samplesPerSec = 12.5e6; v[0].bestSamplesPerSec = 14e6; v[0].referenceRateHz = 2.4e6; v[0].realTimeMultiple = 12.5e6 / 2.4e6;
    v[1].group = "Demod"; v[1].name = "NFM"; v[1].ok = false; v[1].error = "boom";
    const std::string t = bench::formatTable(v);
    CHECK(t.find("forward 1024") != std::string::npos && t.find("12.50") != std::string::npos && t.find("5.2") != std::string::npos);
    CHECK(t.find("FAILED: boom") != std::string::npos);
    CHECK(t.find("median MS/s") != std::string::npos);
}

void testMedianAndFailurePath() {
    CHECK(bench::detail::median({5.0}) == 5.0);
    CHECK(bench::detail::median({3.0, 1.0, 2.0}) == 2.0);
    CHECK(bench::detail::median({1.0, 9.0, 5.0, 7.0, 3.0}) == 5.0);
    CHECK(bench::detail::median({4.0, 1.0}) == 4.0);
    // an illegal FFT size (the class refuses it) is reported, not thrown, and not counted as timed
    bench::BenchOptions o;
    o.secondsPerItem = 0.01;
    const auto bad = bench::detail::fftItem(100, o);
    CHECK(!bad.ok && !bad.error.empty() && bad.samplesPerSec == 0.0);
    const auto good = bench::detail::fftItem(1024, o);
    CHECK(good.ok && good.error.empty());
    // one slice: median and best are the same number
    o.slices = 1;
    const auto one = bench::detail::fftItem(1024, o);
    CHECK(one.ok && one.samplesPerSec == one.bestSamplesPerSec);
}

void testRun(double seconds) {
    bench::BenchOptions o;
    o.secondsPerItem = seconds;
    const auto rs = bench::runDspBenchmark(o);
    std::printf("DSP benchmark on this machine (%.2f s per item, median of %d slices):\n%s", seconds, o.slices, bench::formatTable(rs).c_str());
    CHECK(rs.size() == 21);
    int groups[4] = {0, 0, 0, 0};
    for (const auto& r : rs) {
        CHECK(r.ok);
        CHECK(r.error.empty());
        CHECK(r.samplesPerSec > 0.0 && std::isfinite(r.samplesPerSec));
        CHECK(r.bestSamplesPerSec >= r.samplesPerSec);
        CHECK(r.referenceRateHz > 0.0 && r.realTimeMultiple > 0.0);
        CHECK(std::fabs(r.realTimeMultiple * r.referenceRateHz - r.samplesPerSec) <= 1e-9 * r.samplesPerSec);   // the multiple is rate / reference
        CHECK(r.samples > 0 && r.seconds > 0.0);
        if (r.group == "FFT") { ++groups[0]; } else if (r.group == "Channel") { ++groups[1]; } else if (r.group == "Demod") { ++groups[2]; } else if (r.group == "Resample") { ++groups[3]; }
    }
    CHECK(groups[0] == 7 && groups[1] == 3 && groups[2] == 8 && groups[3] == 3);
}

}  // namespace

int main(int argc, char** argv) {
    double seconds = 0.2;
    if (const char* e = std::getenv("COREF_BENCH_SECONDS")) { seconds = std::atof(e); }
    for (int i = 1; i < argc; ++i) { if (std::strncmp(argv[i], "--seconds=", 10) == 0) { seconds = std::atof(argv[i] + 10); } }
    if (!(seconds >= 0.01 && seconds <= 60.0)) { seconds = 0.2; }
    testFormatting();
    testMedianAndFailurePath();
    testRun(seconds);
    return testSummary("test_dsp_benchmark");
}
