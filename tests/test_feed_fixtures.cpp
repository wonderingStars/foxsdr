// test_feed_fixtures.cpp - the generator tool AND its self-test (lane feedplan; docs/feature-records/feedplan.md).
//
//   test_feed_fixtures --write <dir>     writes every generated fixture into <dir> and a SHA256SUMS list (what tests/feed/run_feed_checks.ps1 does first)
//   test_feed_fixtures                   (no arguments, what ctest runs) builds every fixture twice, and checks for each:
//                                        a well-formed 2-channel PCM16 RIFF at the stated rate; the length; the peak level (not clipped, not silent); the SAME sha256 on both builds (deterministic);
//                                        the carrier the builder promises is really in the spectrum (a Goertzel bin at its offset stands clear of the noise floor);
//                                        and that the noise floor is present (a generated signal with none would be one no receiver ever sees).
// The generators and the plugin decoders share an author: see feedgen.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "feed/feedsignals.hpp"

namespace fs = std::filesystem;

namespace {
int g_checks = 0, g_failed = 0;
}
#define CHECK(c)                                                              \
    do {                                                                      \
        ++g_checks;                                                           \
        if (!(c)) {                                                           \
            ++g_failed;                                                       \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);          \
        }                                                                     \
    } while (0)
#define CHECK_EQ(a, b) CHECK((a) == (b))
using namespace feedgen;

namespace {

struct Entry {
    const char* name;                 // file name without .wav
    std::function<Iq()> build;
    double rate;                      // expected sample rate
    double minSeconds;
    double carrierHz;                 // a frequency (from the file centre) where energy must stand out; NAN = not checked
};

std::vector<Entry> registry() {
    std::vector<Entry> r;
    r.push_back({"dtmf-decoder", [] { return dtmf(); }, kRate48, 7.0, kUsbOffset + 1000.0});
    r.push_back({"ils-meter", [] { return ilsLoc(); }, kRate48, 10.0, kUsbOffset});
    r.push_back({"ccir-selcall", [] { return ccirSelcall(); }, kRate48, 3.0, kUsbOffset + 1300.0});
    r.push_back({"psk31-decoder", [] { return psk31(); }, kRate48, 10.0, kUsbOffset + 1500.0});
    r.push_back({"per-tester", [] { return perPn9(40); }, 96000.0, 5.0, 20000.0});
    return r;
}

// Energy of the I/Q signal at `hz` from the centre (a Goertzel bin on the complex signal), against the mean energy of bins 30 kHz apart-ish elsewhere.
double binPower(const Iq& iq, double hz) {
    const std::size_t n = std::min<std::size_t>(iq.size(), 1u << 16);
    cd acc(0.0, 0.0);
    const double w = -2.0 * kPi * hz / iq.rate;
    for (std::size_t k = 0; k < n; ++k) { acc += cd(iq.i[k], iq.q[k]) * std::polar(1.0, w * static_cast<double>(k)); }
    return std::norm(acc) / static_cast<double>(n);
}

bool parseWav(const std::vector<std::uint8_t>& b, double& rate, std::size_t& frames) {
    if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) != 0 || std::memcmp(b.data() + 8, "WAVEfmt ", 8) != 0) { return false; }
    const auto u16 = [&](std::size_t o) { return static_cast<unsigned>(b[o] | (b[o + 1] << 8)); };
    const auto u32 = [&](std::size_t o) { return static_cast<std::uint32_t>(u16(o)) | (static_cast<std::uint32_t>(u16(o + 2)) << 16); };
    if (u16(20) != 1 || u16(22) != 2 || u16(34) != 16 || std::memcmp(b.data() + 36, "data", 4) != 0) { return false; }
    rate = u32(24);
    frames = u32(40) / 4;
    return b.size() == 44 + static_cast<std::size_t>(frames) * 4 && u32(4) == b.size() - 8;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::vector<Entry> reg = registry();
    if (argc >= 3 && std::string(argv[1]) == "--write") {
        const fs::path dir = argv[2];
        std::error_code ec;
        fs::create_directories(dir, ec);
        std::string sums;
        for (const Entry& e : reg) {
            const std::vector<std::uint8_t> bytes = wavBytes(e.build());
            if (!writeFile((dir / (std::string(e.name) + ".wav")).string(), bytes)) {
                std::printf("cannot write %s\n", e.name);
                return 1;
            }
            sums += sha256(bytes) + "  " + e.name + ".wav\n";
            std::printf("wrote %s.wav (%zu bytes)\n", e.name, bytes.size());
        }
        std::FILE* f = std::fopen((dir / "SHA256SUMS").string().c_str(), "wb");
        if (f == nullptr) { return 1; }
        std::fwrite(sums.data(), 1, sums.size(), f);
        std::fclose(f);
        return 0;
    }
    std::printf("test_feed_fixtures\n");
    // the SHA-256 against its published test vectors (FIPS 180-4 examples)
    CHECK_EQ(sha256(std::vector<std::uint8_t>()), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(sha256(std::vector<std::uint8_t>{'a', 'b', 'c'}), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    for (const Entry& e : reg) {
        const Iq one = e.build();
        const std::vector<std::uint8_t> a = wavBytes(one), b = wavBytes(e.build());
        double rate = 0.0;
        std::size_t frames = 0;
        const bool wavOk = parseWav(a, rate, frames);
        CHECK(wavOk);
        CHECK_EQ(rate, e.rate);
        CHECK(static_cast<double>(frames) / (rate > 0 ? rate : 1) >= e.minSeconds);
        CHECK_EQ(sha256(a), sha256(b));   // deterministic
        // level: peak between 0.6 and 0.8 of full scale
        int peak = 0;
        for (std::size_t k = 44; k + 1 < a.size(); k += 2) { peak = std::max(peak, std::abs(static_cast<int>(static_cast<std::int16_t>(a[k] | (a[k + 1] << 8))))); }
        CHECK(peak > 0.6 * 32767 && peak < 0.8 * 32767);
        // the carrier stands out of the spectrum, and a noise floor exists (bins away from the signal are neither empty nor close to it)
        const double on = binPower(one, e.carrierHz);
        double off = 0.0;
        int nOff = 0;
        for (double hz : {-17000.0, -13000.0, -9000.0, -5000.0}) {
            off += binPower(one, hz);
            ++nOff;
        }
        off /= nOff;
        std::printf("  %-16s %8.1f s  peak %5d  carrier bin %.3g  floor bin %.3g  sha256 %.12s\n", e.name, static_cast<double>(frames) / rate, peak, on, off, sha256(a).c_str());
        CHECK(on > 20.0 * off);
        CHECK(off > 0.0);
    }
    std::printf("test_feed_fixtures: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
