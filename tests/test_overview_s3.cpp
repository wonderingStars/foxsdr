// test_overview_s3.cpp - THE WHOLE-FILE WATERFALL (screen lane S3, G3-14b; core/overview_job.hpp).
//
//   A  a recording with one carrier at +6 kHz from 4 s to 8 s and another at -10 kHz from 12 s to 16 s: the picture has one row per piece of time, the carriers are in the right rows
//      and columns (the brightest column of a row), and the silent rows are dimmer
//   B  every sample format of the readers reaches the picture: raw cu8 and cs16 with the person's statement, a 24-bit WAV
//   C  hostile input: no such file, a file shorter than one block, bad rows, bad transform size, an unknown format, a file cut short under the reader
//   D  the job: progress reaches 1, a result is taken once, a second start while one runs is refused, cancel stops it between rows and the thread is joined
//   Run with  --bench <path> <cu8|cs16|cf32> <rate>  to time a big file (not a test).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <complex>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "core/overview_job.hpp"
#include "test_check.hpp"

namespace ov = cascade::core::overview;
namespace fs = std::filesystem;

namespace {

constexpr double kRate = 48000.0;

fs::path tmp(const char* name) {
    const fs::path d = fs::temp_directory_path() / "foxsdr_test_overview_s3";
    std::error_code ec;
    fs::create_directories(d, ec);
    return d / name;
}

// float I/Q with the two carriers and a little noise (a fixed generator); `seconds` long
std::vector<std::complex<float>> signal(double seconds) {
    const std::size_t n = static_cast<std::size_t>(seconds * kRate);
    std::vector<std::complex<float>> v(n);
    std::uint32_t lcg = 12345;
    for (std::size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kRate;
        lcg = lcg * 1664525u + 1013904223u;
        const float noise = (static_cast<float>(lcg >> 8) / 16777216.0f - 0.5f) * 0.02f;
        std::complex<float> s(noise, -noise);
        if (t >= 4.0 && t < 8.0) { s += std::polar(0.5f, static_cast<float>(2.0 * 3.14159265358979 * 6000.0 * t)); }
        if (t >= 12.0 && t < 16.0) { s += std::polar(0.5f, static_cast<float>(2.0 * 3.14159265358979 * -10000.0 * t)); }
        v[i] = s;
    }
    return v;
}

void writeRaw(const fs::path& p, const std::vector<std::complex<float>>& v, const char* fmt) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    for (const auto& s : v) {
        if (std::strcmp(fmt, "cf32") == 0) { const float a[2] = {s.real(), s.imag()}; f.write(reinterpret_cast<const char*>(a), 8); }
        else if (std::strcmp(fmt, "cs16") == 0) { const std::int16_t a[2] = {static_cast<std::int16_t>(s.real() * 30000), static_cast<std::int16_t>(s.imag() * 30000)}; f.write(reinterpret_cast<const char*>(a), 4); }
        else { const std::uint8_t a[2] = {static_cast<std::uint8_t>(127.5f + s.real() * 120.0f), static_cast<std::uint8_t>(127.5f + s.imag() * 120.0f)}; f.write(reinterpret_cast<const char*>(a), 2); }
    }
}

int brightestColumn(const ov::Image& im, int row, int* level = nullptr) {
    int best = 0, bestV = -1;
    for (int c = 0; c < im.width; ++c) {
        const std::uint8_t* px = im.rgba.data() + 4 * (static_cast<std::size_t>(row) * im.width + c);
        const int v = px[0] + px[1] + px[2];
        if (v > bestV) { bestV = v; best = c; }
    }
    if (level != nullptr) { *level = bestV; }
    return best;
}

bool run(const ov::Request& r, ov::Image& im, std::string& err) {
    std::atomic<bool> cancel{false};
    std::atomic<double> prog{0.0};
    bool cancelled = false;
    return ov::compute(r, cancel, prog, im, err, cancelled);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 5 && std::strcmp(argv[1], "--bench") == 0) {
        ov::Request r;
        r.path = argv[2];
        r.rawFormat = argv[3];
        r.rawRateHz = std::atof(argv[4]);
        r.rows = 1000;
        r.fft = 4096;
        ov::Image im;
        std::string err;
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = run(r, im, err);
        std::printf("overview of %s: %s, %dx%d, %.2f s\n", argv[2], ok ? "ok" : err.c_str(), im.width, im.height, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        return ok ? 0 : 1;
    }

    const std::vector<std::complex<float>> sig = signal(20.0);

    // ---- A
    {
        writeRaw(tmp("a.cf32"), sig, "cf32");
        ov::Request r;
        r.path = tmp("a.cf32").string();
        r.rawFormat = "cf32"; r.rawRateHz = kRate; r.rows = 100; r.fft = 1024;
        ov::Image im; std::string err;
        CHECK(run(r, im, err));
        CHECK(im.width == 1024 && im.height == 100 && im.rgba.size() == 1024u * 100u * 4u);
        CHECK(std::fabs(im.durationS - 20.0) < 1e-6 && im.rateHz == kRate);
        int lvA = 0, lvQuiet = 0, lvB = 0;
        const int colA = brightestColumn(im, 30, &lvA);       // t = 6 s
        const int colB = brightestColumn(im, 70, &lvB);       // t = 14 s
        brightestColumn(im, 5, &lvQuiet);                      // t = 1 s: noise only
        CHECK(std::abs(colA - 640) <= 2);                       // +6 kHz: (6/48 + 0.5) * 1024
        CHECK(std::abs(colB - 299) <= 2);                       // -10 kHz: (-10/48 + 0.5) * 1024
        CHECK(lvA > lvQuiet && lvB > lvQuiet);
        CHECK(im.peakDb > im.floorDb + 30.0);
    }
    // ---- B: every format reaches the picture
    {
        for (const char* fmt : {"cu8", "cs16"}) {
            writeRaw(tmp((std::string("b.") + fmt).c_str()), sig, fmt);
            ov::Request r; r.path = tmp((std::string("b.") + fmt).c_str()).string(); r.rawFormat = fmt; r.rawRateHz = kRate; r.rows = 100; r.fft = 1024;
            ov::Image im; std::string err;
            CHECK(run(r, im, err));
            CHECK(std::abs(brightestColumn(im, 30) - 640) <= 2 && std::abs(brightestColumn(im, 70) - 299) <= 2);
        }
        // a 24-bit WAV, written byte by byte
        std::vector<unsigned char> w;
        const auto p32 = [&](std::uint32_t x) { for (int i = 0; i < 4; ++i) { w.push_back(static_cast<unsigned char>(x >> (8 * i))); } };
        const auto p16 = [&](std::uint16_t x) { for (int i = 0; i < 2; ++i) { w.push_back(static_cast<unsigned char>(x >> (8 * i))); } };
        const auto tag = [&](const char* t) { for (int i = 0; i < 4; ++i) { w.push_back(static_cast<unsigned char>(t[i])); } };
        const std::uint32_t dataBytes = static_cast<std::uint32_t>(sig.size() * 6);
        tag("RIFF"); p32(36 + dataBytes); tag("WAVE"); tag("fmt "); p32(16); p16(1); p16(2); p32(48000); p32(48000 * 6); p16(6); p16(24); tag("data"); p32(dataBytes);
        for (const auto& s : sig) {
            for (const float c : {s.real(), s.imag()}) { const std::int32_t q = static_cast<std::int32_t>(c * 4000000.0f); w.push_back(q & 0xFF); w.push_back((q >> 8) & 0xFF); w.push_back((q >> 16) & 0xFF); }
        }
        std::ofstream f(tmp("b24.wav"), std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(w.data()), static_cast<std::streamsize>(w.size()));
        f.close();
        ov::Request r; r.path = tmp("b24.wav").string(); r.rows = 100; r.fft = 1024;
        ov::Image im; std::string err;
        CHECK(run(r, im, err));
        CHECK(std::abs(brightestColumn(im, 30) - 640) <= 2);
    }
    // ---- C: hostile input
    {
        ov::Image im; std::string err;
        ov::Request r; r.rows = 100; r.fft = 1024; r.rawFormat = "cf32"; r.rawRateHz = kRate;
        r.path = tmp("nothing-here.cf32").string();
        CHECK(!run(r, im, err) && !err.empty());
        { std::ofstream f(tmp("short.cf32"), std::ios::binary | std::ios::trunc); const float z[8] = {}; f.write(reinterpret_cast<const char*>(z), sizeof z); }
        r.path = tmp("short.cf32").string();
        err.clear(); CHECK(!run(r, im, err) && err.find("shorter") != std::string::npos);
        { std::ofstream f(tmp("empty.cf32"), std::ios::binary | std::ios::trunc); }
        r.path = tmp("empty.cf32").string();
        err.clear(); CHECK(!run(r, im, err) && !err.empty());
        r.path = tmp("a.cf32").string();
        r.rows = 10; err.clear(); CHECK(!run(r, im, err));
        r.rows = 100; r.fft = 1000; err.clear(); CHECK(!run(r, im, err));
        r.fft = 1024; r.rawFormat = "zz"; err.clear(); CHECK(!run(r, im, err));
        r.rawFormat = "cf32"; r.rawRateHz = 0.0; err.clear(); CHECK(!run(r, im, err));
        // a file with fewer samples than rows * block: rows are fewer, never an error
        { std::ofstream f(tmp("few.cf32"), std::ios::binary | std::ios::trunc); const std::vector<float> z(2 * 1024 * 60, 0.25f); f.write(reinterpret_cast<const char*>(z.data()), static_cast<std::streamsize>(z.size() * 4)); }
        r.path = tmp("few.cf32").string(); r.rawRateHz = kRate;
        CHECK(run(r, im, err) && im.height == 60);
    }
    // ---- D: the job
    {
        ov::Job job;
        ov::Request r; r.path = tmp("a.cf32").string(); r.rawFormat = "cf32"; r.rawRateHz = kRate; r.rows = 2000; r.fft = 4096;
        CHECK(job.start(r));
        CHECK(!job.start(r) || !job.running());   // never two at once
        ov::Result res;
        int guard = 0;
        while (!job.take(res) && guard++ < 2000) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
        CHECK(res.ok && res.image.height > 0 && res.elapsedS >= 0.0 && job.progress() >= 0.999 && !job.running());
        CHECK(!job.take(res));   // taken once
        // cancel: a file big enough that the run is still going when the cancel arrives
        { std::ofstream f(tmp("big.cf32"), std::ios::binary | std::ios::trunc); const std::vector<float> z(2 * 1024 * 1024, 0.1f); for (int i = 0; i < 40; ++i) { f.write(reinterpret_cast<const char*>(z.data()), static_cast<std::streamsize>(z.size() * 4)); } }
        r.path = tmp("big.cf32").string();
        CHECK(job.start(r));
        job.cancel();
        guard = 0;
        while (!job.take(res) && guard++ < 4000) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
        CHECK(!res.ok && res.cancelled);
        CHECK(job.start(r));   // and a new one can start afterwards
        job.cancel();
    }
    std::error_code ec;
    fs::remove_all(tmp("x").parent_path(), ec);
    return testSummary("test_overview_s3");
}
