// test_tool_benchmark.cpp - `--benchmark` (engine wiring W2): runBenchmark times the DSP blocks, prints the table and answers by its exit code.
//
//   A  a short run (10 ms per item) returns 0 and prints the header, a line for every group (FFT, Channel, Demod, Resample), the PASS line
//   B  a speed is never judged: the run is made with a time too short to be accurate and still passes (the exit code is about items that ran)
//   C  a zero or negative time falls back to the default per-item time rather than timing nothing: the printed header names a positive time
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstring>
#include <string>

#include "engine/tool_modes.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <io.h>
#define W2_DUP _dup
#define W2_DUP2 _dup2
#define W2_CLOSE _close
#define W2_FILENO _fileno
#else
#include <unistd.h>
#define W2_DUP dup
#define W2_DUP2 dup2
#define W2_CLOSE close
#define W2_FILENO fileno
#endif

namespace {

// Runs `fn` with stdout sent to a file and returns what it printed.
template <class Fn>
std::string captureStdout(Fn fn, int* result) {
    const std::string path = std::string("test_tool_benchmark_out_") + std::to_string(static_cast<long long>(std::rand())) + ".txt";
    std::fflush(stdout);
    const int saved = W2_DUP(W2_FILENO(stdout));
    std::FILE* f = std::fopen(path.c_str(), "w");
    W2_DUP2(W2_FILENO(f), W2_FILENO(stdout));
    *result = fn();
    std::fflush(stdout);
    W2_DUP2(saved, W2_FILENO(stdout));
    W2_CLOSE(saved);
    std::fclose(f);
    std::string out;
    if (std::FILE* r = std::fopen(path.c_str(), "r")) {
        char buf[4096];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, r)) > 0) { out.append(buf, n); }
        std::fclose(r);
    }
    std::remove(path.c_str());
    return out;
}

}  // namespace

int main() {
    std::printf("test_tool_benchmark\n");
    int rc = -1;
    const std::string out = captureStdout([] { return cascade::engine::runBenchmark(0.01); }, &rc);
    std::printf("%s", out.c_str());
    CHECK(rc == 0);                                           // A, B
    CHECK(out.find("DSP benchmark: 0.01 s per item") != std::string::npos);
    for (const char* group : {"FFT", "Channel", "Demod", "Resample"}) { CHECK(out.find(group) != std::string::npos); }
    CHECK(out.find("benchmark PASS:") != std::string::npos);
    CHECK(out.find("FAILED") == std::string::npos);

    int rc2 = -1;
    const std::string out2 = captureStdout([] { return cascade::engine::runBenchmark(-1.0); }, &rc2);  // C: the default time (0.2 s per item)
    CHECK(rc2 == 0);
    CHECK(out2.find("DSP benchmark: 0.20 s per item") != std::string::npos);
    return testSummary("test_tool_benchmark");
}
