// test_engine_api_fileseek.cpp - THE SEEK BAR AND THE A-B LOOP THROUGH THE ENGINE (engine serial lane A, row P15): FILE_SEEK 0x861A and FILE_LOOP 0x861B on a radio playing an I/Q file.
//   A  the radio's state reports a file (position, length, real start, where the start came from) and a generator radio reports none;  B  a seek in seconds and as a fraction lands where it
//   was asked and playing goes on from there;  C  a loop keeps the position inside it, clearing it lets the position out;  D  the refusals (past either end, a loop backwards or past the end,
//   a radio that plays no file) change nothing.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "wire1_rig.hpp"

#ifdef _WIN32
#include <process.h>
#define TEST_GETPID _getpid
#else
#include <unistd.h>
#define TEST_GETPID getpid
#endif

namespace {

constexpr std::uint32_t kRate = 48000;
constexpr std::uint64_t kFrames = 480000;   // ten seconds

void put32(std::vector<unsigned char>& v, std::uint32_t x) { for (int i = 0; i < 4; ++i) { v.push_back(static_cast<unsigned char>((x >> (8 * i)) & 0xFFu)); } }
void put16(std::vector<unsigned char>& v, std::uint16_t x) { for (int i = 0; i < 2; ++i) { v.push_back(static_cast<unsigned char>((x >> (8 * i)) & 0xFFu)); } }

// A float32 stereo WAV of a quiet tone.
std::string writeWav(const std::string& name) {
    std::vector<unsigned char> b;
    const std::uint32_t dataBytes = static_cast<std::uint32_t>(kFrames * 8);
    b.insert(b.end(), {'R', 'I', 'F', 'F'});
    put32(b, 36 + dataBytes);
    b.insert(b.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put32(b, 16);
    put16(b, 3);
    put16(b, 2);
    put32(b, kRate);
    put32(b, kRate * 8);
    put16(b, 8);
    put16(b, 32);
    b.insert(b.end(), {'d', 'a', 't', 'a'});
    put32(b, dataBytes);
    for (std::uint64_t i = 0; i < kFrames; ++i) {
        const double ph = 6.283185307179586 * 5000.0 * static_cast<double>(i) / kRate;
        const float re = static_cast<float>(0.3 * std::cos(ph)), im = static_cast<float>(0.3 * std::sin(ph));
        unsigned char t[4];
        std::memcpy(t, &re, 4);
        b.insert(b.end(), t, t + 4);
        std::memcpy(t, &im, 4);
        b.insert(b.end(), t, t + 4);
    }
    std::ofstream f(name, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    return name;
}

FoxCommand seek(std::uint32_t target, std::int64_t mode, double v) {
    FoxCommand c = testing::cmd(FOXAPP_OP_FILE_SEEK, v, mode, nullptr);
    c.target = target;
    return c;
}
FoxCommand loop(std::uint32_t target, std::int64_t on, double a, double b) {
    FoxCommand c = testing::cmd(FOXAPP_OP_FILE_LOOP, a, on, nullptr);
    c.target = target;
    c.num[1] = b;
    return c;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_engine_api_fileseek\n");
    engine_api_test::hermetic("fileseek");
    Access::installHooks();
    const std::string path = writeWav("fileseek_20261109_134502_" + std::to_string(TEST_GETPID()) + ".wav");
    Rig r;
    CHECK_EQ(r.add(2, "file:" + path).status(), FOXAPI_OK);
    CHECK(r.until([&] { return r.radio(2).fileSource == 1u; }, 30000));

    std::printf("A: the state of a file radio and of a generator radio\n");
    {
        const FoxReceiverState f = r.radio(2), g = r.radio(1);
        CHECK_EQ(f.fileSource, 1u);
        CHECK(std::fabs(f.fileDurationSec - 10.0) < 1e-6);
        CHECK(f.filePositionSec >= 0.0 && f.filePositionSec < 10.0);
        CHECK_EQ(f.fileStartSource, 1u);   // the date and time in the name
        CHECK(f.fileStartUnixMs > 1.7e12);
        CHECK_EQ(f.fileLoopStartSec, 0.0);
        CHECK_EQ(f.fileLoopEndSec, 0.0);
        CHECK_EQ(g.fileSource, 0u);
        CHECK_EQ(g.fileDurationSec, 0.0);
        CHECK_EQ(g.fileStartUnixMs, 0.0);
    }

    std::printf("B: seeks\n");
    {
        CHECK_EQ(r.send(seek(2, 0, 6.0)).status(), FOXAPI_OK);
        CHECK(r.until([&] { const double p = r.radio(2).filePositionSec; return p >= 6.3 && p < 7.8; }, 3000));   // (past the place asked for, by playing on from it)
        CHECK_EQ(r.send(seek(2, 1, 0.25)).status(), FOXAPI_OK);
        CHECK(r.until([&] { const double p = r.radio(2).filePositionSec; return p >= 2.8 && p < 4.3; }, 3000));
        const FoxCommandResult res = r.send(seek(2, 0, 1.5)).result;
        CHECK_EQ(res.applied[0], 1.5);
        // and playing goes on from there
        const double at = r.radio(2).filePositionSec;
        r.pump(40);
        CHECK(r.until([&] { return r.radio(2).filePositionSec > at + 0.05 || r.radio(2).filePositionSec < at; }, 10000));
    }

    std::printf("C: a loop\n");
    {
        CHECK_EQ(r.send(seek(2, 0, 2.0)).status(), FOXAPI_OK);
        CHECK_EQ(r.send(loop(2, 1, 1.0, 1.4)).status(), FOXAPI_OK);
        CHECK(r.until([&] { return r.radio(2).fileLoopEndSec == 1.4; }, 10000));
        CHECK_EQ(r.radio(2).fileLoopStartSec, 1.0);
        // the reader applies the loop at its next chunk: wait until the position has come inside it, then watch it stay there
        CHECK(r.until([&] { const double p = r.radio(2).filePositionSec; return p >= 1.0 && p <= 1.4; }, 10000));
        bool inside = true;
        int samples = 0;
        double lo = 1e9, hi = -1e9;
        CHECK(r.until([&] {
            const double p = r.radio(2).filePositionSec;
            ++samples;
            lo = std::fmin(lo, p);
            hi = std::fmax(hi, p);
            inside = inside && (p >= 1.0 - 1e-6 && p <= 1.4 + 0.05);   // (a block read past b in one chunk is at most a few ms)
            return samples > 150;
        }, 20000));
        std::printf("  position stayed between %.3f and %.3f s over %d reads\n", lo, hi, samples);
        CHECK(inside);
        CHECK_EQ(r.send(loop(2, 0, 0.0, 0.0)).status(), FOXAPI_OK);
        CHECK(r.until([&] { return r.radio(2).fileLoopEndSec == 0.0; }, 10000));
        // out of the loop it plays on past 1.4 s
        CHECK_EQ(r.send(seek(2, 0, 1.3)).status(), FOXAPI_OK);
        CHECK(r.until([&] { return r.radio(2).filePositionSec > 1.6; }, 10000));
    }

    std::printf("D: refusals\n");
    {
        const double len = r.radio(2).fileDurationSec;
        for (const double bad : {-0.5, len, len + 1.0}) { CHECK_EQ(r.send(seek(2, 0, bad)).status(), FOXAPI_OUT_OF_RANGE); }
        for (const double bad : {-0.1, 1.0, 1.5}) { CHECK_EQ(r.send(seek(2, 1, bad)).status(), FOXAPI_OUT_OF_RANGE); }
        CHECK_EQ(r.send(loop(2, 1, 5.0, 4.0)).status(), FOXAPI_OUT_OF_RANGE);
        CHECK_EQ(r.send(loop(2, 1, 5.0, 5.0)).status(), FOXAPI_OUT_OF_RANGE);
        CHECK_EQ(r.send(loop(2, 1, -1.0, 4.0)).status(), FOXAPI_OUT_OF_RANGE);
        CHECK_EQ(r.send(loop(2, 1, 1.0, len + 1.0)).status(), FOXAPI_OUT_OF_RANGE);
        CHECK_EQ(r.radio(2).fileLoopEndSec, 0.0);   // none of them set a loop
        // the generator radio plays no file
        CHECK_EQ(r.send(seek(1, 0, 1.0)).status(), FOXAPI_UNSUPPORTED);
        CHECK_EQ(r.send(loop(1, 1, 1.0, 2.0)).status(), FOXAPI_UNSUPPORTED);
        CHECK_EQ(r.send(seek(9, 0, 1.0)).status(), FOXAPI_NO_DEVICE);
    }
    r.send(r.aimed(FOXAPI_OP_RADIO_REMOVE, 2));
    std::remove(path.c_str());
    return check::finish("test_engine_api_fileseek");
}
