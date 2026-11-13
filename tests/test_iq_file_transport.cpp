// test_iq_file_transport.cpp - THE TRANSPORT OF A RECORDED I/Q FILE (wire-a second pass, item 1): speed 0.25x to 8x (varispeed and block skip), pause and step, on IqFileSource itself.
//   A  speed 1 is the unchanged path, frame for frame;  B  varispeed: 2x delivers every second frame exactly, 0.5x interpolates a ramp exactly, a tone's frequency is multiplied by the speed;
//   C  block skip: grains of 2048 frames with the jumps between them (forward at 2x, back at 0.5x), frequencies unchanged;  D  pause: read() returns 0 and wantsRead() is false, resume goes on from the same
//   frame;  E  step: n blocks then 0 again, a step back replays, refused while playing;  F  refusals: speed NaN / 0.1 / 9, step 0 / too many / no file;  G  a loop holds at every speed;  H  stop() ends a varied read.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/iq_file_source.hpp"

#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define TEST_GETPID _getpid
#else
#include <unistd.h>
#define TEST_GETPID getpid
#endif

#include "test_check.hpp"

using cascade::source::IqFileSource;
using cf = std::complex<float>;

namespace {

constexpr std::uint32_t kRate = 48000;
constexpr std::uint64_t kFrames = 200000;
constexpr double kToneHz = 1000.0;

void put32(std::vector<unsigned char>& v, std::uint32_t x) { for (int i = 0; i < 4; ++i) { v.push_back(static_cast<unsigned char>((x >> (8 * i)) & 0xFFu)); } }
void put16(std::vector<unsigned char>& v, std::uint16_t x) { for (int i = 0; i < 2; ++i) { v.push_back(static_cast<unsigned char>((x >> (8 * i)) & 0xFFu)); } }

// float32 WAV: with `ramp` frame k is (k * 1e-5, -k * 1e-5), otherwise a tone of kToneHz.
std::string writeWav(const std::string& tag, bool ramp) {
    const std::string name = "iq_transport_" + std::to_string(TEST_GETPID()) + "_" + tag + ".wav";
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
    for (std::uint64_t k = 0; k < kFrames; ++k) {
        float re, im;
        if (ramp) {
            re = static_cast<float>(static_cast<double>(k) * 1e-5);
            im = -re;
        } else {
            const double ph = 6.283185307179586 * kToneHz * static_cast<double>(k) / kRate;
            re = static_cast<float>(0.5 * std::cos(ph));
            im = static_cast<float>(0.5 * std::sin(ph));
        }
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

float rampAt(double k) { return static_cast<float>(k * 1e-5); }

std::vector<cf> readAll(IqFileSource& s, std::size_t total, std::size_t chunk = 480) {
    std::vector<cf> out;
    while (out.size() < total) {
        std::vector<cf> b(chunk);
        const std::size_t g = s.read(b.data(), std::min(chunk, total - out.size()));
        if (g == 0) { break; }
        out.insert(out.end(), b.begin(), b.begin() + static_cast<std::ptrdiff_t>(g));
    }
    return out;
}

// The frequency of a complex tone by the phase advance between frames (mean over the run), in Hz.
double toneHz(const std::vector<cf>& y) {
    double acc = 0.0;
    std::size_t n = 0;
    for (std::size_t i = 1; i < y.size(); ++i) {
        const std::complex<double> d = std::complex<double>(y[i]) * std::conj(std::complex<double>(y[i - 1]));
        acc += std::arg(d);
        ++n;
    }
    return acc / static_cast<double>(n) * kRate / 6.283185307179586;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_iq_file_transport\n");
    const std::string ramp = writeWav("ramp", true);
    const std::string tone = writeWav("tone", false);

    std::printf("A: speed 1 is the unchanged path\n");
    {
        IqFileSource s;
        CHECK(s.open(ramp));
        CHECK(s.start());
        const std::vector<cf> y = readAll(s, 3000);
        CHECK(y.size() == 3000);
        bool exact = true;
        for (std::size_t k = 0; k < y.size(); ++k) { exact = exact && y[k].real() == rampAt(static_cast<double>(k)) && y[k].imag() == -rampAt(static_cast<double>(k)); }
        CHECK(exact);
        CHECK(s.playSpeed() == 1.0);
        CHECK(!s.playbackPaused());
        CHECK(s.wantsRead());
        CHECK(!s.speedBlockSkip());
        // a speed that is set and set back is the unchanged path again
        CHECK(s.setPlaySpeed(2.0));
        CHECK(s.setPlaySpeed(1.0));
        const std::uint64_t at = s.positionFrames();
        const std::vector<cf> z = readAll(s, 100);
        CHECK(z.size() == 100 && z[0].real() == rampAt(static_cast<double>(at)));
    }

    std::printf("B: varispeed\n");
    {
        IqFileSource s;
        CHECK(s.open(ramp));
        CHECK(s.start());
        CHECK(s.setPlaySpeed(2.0));
        const std::vector<cf> y = readAll(s, 4000);
        CHECK(y.size() == 4000);
        double worst = 0.0;
        for (std::size_t j = 0; j < y.size(); ++j) { worst = std::max(worst, static_cast<double>(std::fabs(y[j].real() - rampAt(2.0 * static_cast<double>(j))))); }
        std::printf("  2x: worst error against x[2j] is %.3g\n", worst);
        CHECK(worst < 1e-6);
        CHECK(s.positionFrames() >= 8000 && s.positionFrames() <= 8004);   // the position moved two frames per frame delivered
        // 0.5x: a ramp is its own interpolation, so y[j] = x[j / 2] to float precision
        IqFileSource h;
        CHECK(h.open(ramp));
        CHECK(h.start());
        CHECK(h.setPlaySpeed(0.5));
        const std::vector<cf> v = readAll(h, 4000);
        double worstHalf = 0.0;
        for (std::size_t j = 0; j < v.size(); ++j) { worstHalf = std::max(worstHalf, static_cast<double>(std::fabs(v[j].real() - rampAt(0.5 * static_cast<double>(j))))); }
        std::printf("  0.5x: worst error against x[j/2] is %.3g\n", worstHalf);
        CHECK(v.size() == 4000 && worstHalf < 1e-6);
        CHECK(h.positionFrames() >= 2000 && h.positionFrames() <= 2004);
        // 8x and 0.25x are the ends of the range and work
        CHECK(h.setPlaySpeed(8.0) && h.playSpeed() == 8.0);
        CHECK(readAll(h, 1000).size() == 1000);
        CHECK(h.setPlaySpeed(0.25) && readAll(h, 1000).size() == 1000);
    }
    {
        // a tone's frequency is multiplied by the speed (a tape)
        for (double sp : {0.5, 1.0, 2.0, 3.0}) {
            IqFileSource s;
            CHECK(s.open(tone));
            CHECK(s.start());
            CHECK(s.setPlaySpeed(sp));
            const std::vector<cf> y = readAll(s, 6000);
            const double hz = toneHz(y);
            std::printf("  tone at %.2fx: %.1f Hz (wanted %.1f)\n", sp, hz, kToneHz * sp);
            CHECK(std::fabs(hz - kToneHz * sp) < kToneHz * sp * 0.01);
        }
    }

    std::printf("C: block skip\n");
    {
        IqFileSource s;
        CHECK(s.open(ramp));
        CHECK(s.start());
        s.setSpeedBlockSkip(true);
        CHECK(s.speedBlockSkip());
        CHECK(s.setPlaySpeed(2.0));
        const std::vector<cf> y = readAll(s, 6144);
        CHECK(y.size() == 6144);
        // grain 0 = frames 0..2047, then a jump of 2048: grain 1 = 4096..6143, then 8192..
        CHECK(y[0].real() == rampAt(0) && y[2047].real() == rampAt(2047));
        CHECK(y[2048].real() == rampAt(4096) && y[4095].real() == rampAt(6143));
        CHECK(y[4096].real() == rampAt(8192));
        // the carrier is NOT moved: inside a grain the frames are consecutive
        IqFileSource t;
        CHECK(t.open(tone));
        CHECK(t.start());
        t.setSpeedBlockSkip(true);
        CHECK(t.setPlaySpeed(2.0));
        const std::vector<cf> g = readAll(t, 2000);   // inside the first grain
        CHECK(std::fabs(toneHz(g) - kToneHz) < 5.0);
        // 0.5x: back 1024 after each grain, so the grains overlap by half
        IqFileSource h;
        CHECK(h.open(ramp));
        CHECK(h.start());
        h.setSpeedBlockSkip(true);
        CHECK(h.setPlaySpeed(0.5));
        const std::vector<cf> v = readAll(h, 4096);
        CHECK(v[0].real() == rampAt(0) && v[2047].real() == rampAt(2047));
        CHECK(v[2048].real() == rampAt(1024) && v[4095].real() == rampAt(3071));
    }

    std::printf("D: pause\n");
    {
        IqFileSource s;
        CHECK(s.open(ramp));
        CHECK(s.start());
        const std::vector<cf> a = readAll(s, 1000);
        CHECK(a.size() == 1000);
        s.pausePlayback();
        CHECK(s.playbackPaused());
        CHECK(!s.wantsRead());
        cf b[100];
        CHECK(s.read(b, 100) == 0);
        CHECK(s.positionFrames() == 1000);
        CHECK(s.running());   // paused is not stopped
        s.resumePlayback();
        CHECK(!s.playbackPaused() && s.wantsRead());
        const std::vector<cf> c = readAll(s, 100);
        CHECK(c.size() == 100 && c[0].real() == rampAt(1000));   // on from the same frame
    }

    std::printf("E: step\n");
    {
        IqFileSource s;
        CHECK(s.open(ramp));
        CHECK(s.start());
        CHECK(!s.stepBlocks(1));   // refused while playing
        CHECK(readAll(s, 10000).size() == 10000);
        s.pausePlayback();
        CHECK(s.stepBlocks(2));
        CHECK(s.stepFramesPending() == 2 * 4096);
        CHECK(s.wantsRead());
        const std::vector<cf> y = readAll(s, 100000);   // asks for far more: delivers exactly the two blocks
        CHECK(y.size() == 2 * 4096);
        CHECK(y[0].real() == rampAt(10000));
        CHECK(y.back().real() == rampAt(10000 + 8191));
        CHECK(s.stepFramesPending() == 0 && !s.wantsRead());
        cf b[10];
        CHECK(s.read(b, 10) == 0);
        // a step back: 3 blocks back from 18192, one block played from there
        CHECK(s.stepBlocks(-3));
        const std::vector<cf> w = readAll(s, 100000);
        CHECK(w.size() == 4096);
        CHECK(w[0].real() == rampAt(18192 - 3 * 4096));
        // far enough back is clamped to the first frame
        CHECK(s.stepBlocks(-4000));
        const std::vector<cf> z = readAll(s, 100000);
        CHECK(z.size() == 4096 && z[0].real() == rampAt(0));
        // a step at 2x delivers the same number of frames, from twice as far
        CHECK(s.setPlaySpeed(2.0));
        CHECK(s.stepBlocks(1));
        const std::vector<cf> d = readAll(s, 100000);
        CHECK(d.size() == 4096);
        CHECK(std::fabs(d[1].real() - d[0].real() - rampAt(2)) < 1e-6);
        // a resume drops a step nobody used
        CHECK(s.stepBlocks(5));
        s.resumePlayback();
        CHECK(s.stepFramesPending() == 0);
    }

    std::printf("F: refusals change nothing\n");
    {
        IqFileSource s;
        CHECK(!s.setPlaySpeed(std::numeric_limits<double>::quiet_NaN()));
        CHECK(!s.setPlaySpeed(std::numeric_limits<double>::infinity()));
        CHECK(!s.setPlaySpeed(0.1));
        CHECK(!s.setPlaySpeed(9.0));
        CHECK(!s.setPlaySpeed(-1.0));
        CHECK(s.playSpeed() == 1.0);
        CHECK(!s.stepBlocks(1));   // no file open, not paused
        s.pausePlayback();
        CHECK(!s.stepBlocks(1));   // paused but no file
        CHECK(s.open(ramp));
        CHECK(!s.stepBlocks(0));
        CHECK(!s.stepBlocks(4097));
        CHECK(!s.stepBlocks(-4097));
        CHECK(s.stepFramesPending() == 0);
        CHECK(s.stepBlocks(4096));
        CHECK(s.stepFramesPending() == 4096LL * 4096);
        // a stopped source reads nothing, paused or not
        cf b[10];
        CHECK(s.read(b, 10) == 0);
    }

    std::printf("G: an A-B loop holds at every speed\n");
    {
        for (double sp : {0.5, 2.0, 5.0}) {
            for (bool skip : {false, true}) {
                IqFileSource s;
                CHECK(s.open(ramp));
                CHECK(s.start());
                s.setSpeedBlockSkip(skip);
                CHECK(s.setPlaySpeed(sp));
                CHECK(s.setLoopFrames(10000, 14000));
                bool inside = true;
                std::uint64_t lo = ~0ull, hi = 0;
                for (int i = 0; i < 80; ++i) {
                    std::vector<cf> b(480);
                    CHECK(s.read(b.data(), 480) == 480);
                    if (i > 12) {   // after the seek into the loop has been applied
                        for (const cf& v : b) {
                            const double k = static_cast<double>(v.real()) / 1e-5;
                            lo = std::min<std::uint64_t>(lo, static_cast<std::uint64_t>(k));
                            hi = std::max<std::uint64_t>(hi, static_cast<std::uint64_t>(k));
                            inside = inside && (k >= 0 && k < 14000 + 3.0);
                        }
                    }
                }
                std::printf("  %.1fx %s: frames up to %llu (loop end 14000)\n", sp, skip ? "skip" : "vari", static_cast<unsigned long long>(hi));
                CHECK(inside);
            }
        }
    }

    std::printf("H: a varied read ends when the source stops\n");
    {
        IqFileSource s;
        CHECK(s.open(ramp));
        CHECK(s.start());
        CHECK(s.setPlaySpeed(3.0));
        CHECK(readAll(s, 100).size() == 100);
        s.stop();
        cf b[100];
        CHECK(s.read(b, 100) == 0);
        CHECK(s.start());   // a start rewinds: the transport starts from the top
        const std::vector<cf> y = readAll(s, 10);
        CHECK(y.size() == 10 && y[0].real() == rampAt(0));
    }

    std::remove(ramp.c_str());
    std::remove(tone.c_str());
    return testSummary("test_iq_file_transport");
}
