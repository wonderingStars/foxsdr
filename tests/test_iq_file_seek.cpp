// test_iq_file_seek.cpp - SEEK, A-B LOOP, POSITION AND THE REAL START TIME OF AN I/Q FILE (engine serial lane A, row P15).
//
// The file carries its own frame number in the I channel (float32 holds integers exactly up to 2^24) and the number of the pass in Q, so every sample read says where it came from.
//   A  position and order: frames come in order from 0, the position follows, the end wraps to the start;  B  a seek: the first block after seek() returned starts at the new frame (also
//   made while stopped), refused at and past the end;  C  an A-B loop: frame b-1 is followed by frame a with no gap and no repeat, a seek past b continues from a, a loop end moved below the
//   play position continues from a, refusals, clearLoop() goes back to the whole file;  D  a thread seeking and looping while another reads: every sample is a real frame of the file;
//   E  the real start time: from the date and time in the name, from the last-write time without one, nothing for a name with an impossible date.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define TEST_GETPID _getpid
#else
#include <unistd.h>
#define TEST_GETPID getpid
#endif

#include "source/file_start_time.hpp"
#include "source/iq_file_source.hpp"
#include "test_check.hpp"

using cascade::source::IqFileSource;
namespace fs = std::filesystem;

namespace {

constexpr std::uint32_t kRate = 48000;
constexpr std::uint64_t kFrames = 100000;

void put32(std::vector<unsigned char>& v, std::uint32_t x) { for (int i = 0; i < 4; ++i) { v.push_back(static_cast<unsigned char>((x >> (8 * i)) & 0xFFu)); } }
void put16(std::vector<unsigned char>& v, std::uint16_t x) { for (int i = 0; i < 2; ++i) { v.push_back(static_cast<unsigned char>((x >> (8 * i)) & 0xFFu)); } }

// A float32 stereo WAV of `frames` frames: I = the frame number, Q = 0.
std::string writeWav(const std::string& name, std::uint64_t frames) {
    std::vector<unsigned char> b;
    const std::uint32_t dataBytes = static_cast<std::uint32_t>(frames * 8);
    b.insert(b.end(), {'R', 'I', 'F', 'F'});
    put32(b, 36 + dataBytes);
    b.insert(b.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put32(b, 16);
    put16(b, 3);          // IEEE float
    put16(b, 2);
    put32(b, kRate);
    put32(b, kRate * 8);
    put16(b, 8);
    put16(b, 32);
    b.insert(b.end(), {'d', 'a', 't', 'a'});
    put32(b, dataBytes);
    for (std::uint64_t i = 0; i < frames; ++i) {
        const float re = static_cast<float>(i), im = 0.0f;
        unsigned char t[4];
        std::memcpy(t, &re, 4);
        b.insert(b.end(), t, t + 4);
        std::memcpy(t, &im, 4);
        b.insert(b.end(), t, t + 4);
    }
    const std::string path = name;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    return path;
}

std::vector<std::uint64_t> framesOf(IqFileSource& s, std::size_t n) {
    std::vector<std::complex<float>> v(n);
    const std::size_t got = s.read(v.data(), n);
    std::vector<std::uint64_t> out;
    for (std::size_t i = 0; i < got; ++i) { out.push_back(static_cast<std::uint64_t>(v[i].real())); }
    return out;
}

}  // namespace

int main() {
    std::printf("test_iq_file_seek\n");
    const std::string path = writeWav("iq_seek_" + std::to_string(TEST_GETPID()) + ".wav", kFrames);
    IqFileSource s;
    CHECK(s.open(path));
    CHECK(s.totalFrames() == kFrames);
    CHECK(s.start());

    // A: order, position, the wrap of the whole file
    {
        const std::vector<std::uint64_t> a = framesOf(s, 5000);
        bool inOrder = a.size() == 5000;
        for (std::size_t i = 0; i < a.size(); ++i) { inOrder = inOrder && a[i] == i; }
        CHECK(inOrder);
        CHECK(s.positionFrames() == 5000);
        CHECK(!s.loopOn());
        CHECK(s.seekFrame(kFrames - 100));
        const std::vector<std::uint64_t> w = framesOf(s, 300);
        bool wrapped = w.size() == 300;
        for (std::size_t i = 0; i < w.size(); ++i) { wrapped = wrapped && w[i] == (kFrames - 100 + i) % kFrames; }
        CHECK(wrapped);   // ...99998, 99999, 0, 1... : no gap, no repeat at the seam
    }

    // B: seeks
    {
        CHECK(s.seekFrame(40000));
        CHECK(s.positionFrames() == 40000);                    // the bar jumps at once, before the reader has run
        const std::vector<std::uint64_t> a = framesOf(s, 10);
        CHECK(a.size() == 10 && a[0] == 40000 && a[9] == 40009);
        CHECK(s.positionFrames() == 40010);
        CHECK(!s.seekFrame(kFrames) && !s.seekFrame(kFrames + 5) && !s.seekFrame(~std::uint64_t{0}));
        CHECK(s.positionFrames() == 40010);                    // a refused seek changes nothing
        CHECK(s.seekFrame(kFrames - 1));
        const std::vector<std::uint64_t> e = framesOf(s, 2);
        CHECK(e.size() == 2 && e[0] == kFrames - 1 && e[1] == 0);
        // two seeks before one read: the last one wins
        CHECK(s.seekFrame(10));
        CHECK(s.seekFrame(77));
        const std::vector<std::uint64_t> l = framesOf(s, 1);
        CHECK(l.size() == 1 && l[0] == 77);
        // a seek made while stopped is honoured by start()
        s.stop();
        CHECK(s.seekFrame(60000));
        CHECK(s.start());
        const std::vector<std::uint64_t> st = framesOf(s, 1);
        CHECK(st.size() == 1 && st[0] == 60000);
        // a start() with nothing asked still replays from the top
        s.stop();
        CHECK(s.start());
        CHECK(framesOf(s, 1)[0] == 0);
    }

    // C: the A-B loop
    {
        CHECK(!s.setLoopFrames(500, 500) && !s.setLoopFrames(900, 100) && !s.setLoopFrames(0, kFrames + 1));
        CHECK(!s.loopOn());
        CHECK(s.seekFrame(2000));
        CHECK(s.setLoopFrames(1000, 1500));
        CHECK(s.loopOn() && s.loopStartFrame() == 1000 && s.loopEndFrame() == 1500);
        // a play position past the end of the loop continues from its start
        std::vector<std::uint64_t> a = framesOf(s, 1200);
        bool ok = a.size() == 1200;
        for (std::size_t i = 0; i < a.size(); ++i) { ok = ok && a[i] == 1000 + (i % 500); }
        CHECK(ok);   // 1000..1499 then 1000.. again: frame b-1 is followed by frame a, nothing missing, nothing doubled
        // a seek before the loop plays up to b and then wraps to a
        CHECK(s.seekFrame(1400));
        a = framesOf(s, 250);
        ok = a.size() == 250;
        for (std::size_t i = 0; i < a.size(); ++i) { ok = ok && a[i] == (i < 100 ? 1400 + i : 1000 + (i - 100) % 500); }
        CHECK(ok);
        // the loop end moved below the play position
        CHECK(s.seekFrame(1450));
        CHECK(s.setLoopFrames(1000, 1200));
        a = framesOf(s, 10);
        CHECK(a.size() == 10 && a[0] == 1000);
        // cleared: the whole file again
        s.clearLoop();
        CHECK(!s.loopOn());
        CHECK(s.seekFrame(1190));
        a = framesOf(s, 30);
        ok = a.size() == 30;
        for (std::size_t i = 0; i < a.size(); ++i) { ok = ok && a[i] == 1190 + i; }
        CHECK(ok);
        // a loop of one block and a read of many blocks
        CHECK(s.setLoopFrames(0, 1));
        a = framesOf(s, 50);
        ok = a.size() == 50;
        for (const std::uint64_t f : a) { ok = ok && f == 0; }
        CHECK(ok);
        s.clearLoop();
    }

    // D: a thread seeking and looping while another reads
    {
        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> seeks{0};
        std::thread t([&] {
            std::uint64_t k = 1;
            while (!stop.load()) {
                k = (k * 6364136223846793005ull + 1442695040888963407ull);
                (void)s.seekFrame(k % kFrames);
                if ((k >> 20) % 3 == 0) {
                    const std::uint64_t a = (k >> 10) % (kFrames - 10);
                    (void)s.setLoopFrames(a, a + 1 + (k >> 30) % 5000);
                } else if ((k >> 20) % 3 == 1) {
                    s.clearLoop();
                }
                ++seeks;
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        });
        bool real = true;
        std::uint64_t blocks = 0;
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<std::complex<float>> v(777);
        while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2)) {
            const std::size_t got = s.read(v.data(), v.size());
            real = real && got == v.size();
            for (std::size_t i = 0; i < got; ++i) { real = real && v[i].real() >= 0.0f && v[i].real() < static_cast<float>(kFrames) && v[i].real() == std::floor(v[i].real()) && v[i].imag() == 0.0f; }
            ++blocks;
        }
        stop.store(true);
        t.join();
        std::printf("  D: %llu blocks read while %llu seeks were asked\n", static_cast<unsigned long long>(blocks), static_cast<unsigned long long>(seeks.load()));
        CHECK(real);
        CHECK(blocks > 50 && seeks.load() > 50);
        s.clearLoop();
    }
    s.stop();

    // E: the real start time
    {
        using cascade::source::startFromName;
        const auto ms = startFromName("FoxSDR_100.1MHz_WFM_20261109_134502.wav");
        CHECK(ms.has_value());
        std::tm t{};
        t.tm_year = 2026 - 1900; t.tm_mon = 10; t.tm_mday = 9; t.tm_hour = 13; t.tm_min = 45; t.tm_sec = 2; t.tm_isdst = -1;
        CHECK(ms.has_value() && *ms == static_cast<std::int64_t>(std::mktime(&t)) * 1000);
        CHECK(startFromName("take 2026-11-09 13-45-02 am.wav") == ms);
        CHECK(startFromName("take_2026-11-09T13.45.02.wav") == ms);
        CHECK(startFromName("take_2026-11-09_13:45:02.wav") == ms);
        CHECK(startFromName("2026-11-09-13-45-02") == ms);
        CHECK(!startFromName("noise.wav").has_value());
        CHECK(!startFromName("").has_value());
        CHECK(!startFromName("20261309_134502.wav").has_value());   // month 13
        CHECK(!startFromName("20260230_134502.wav").has_value());   // 30 February
        CHECK(startFromName("20240229_134502.wav").has_value());    // a leap day is real
        CHECK(!startFromName("20250229_134502.wav").has_value());   // and a non-leap one is not
        CHECK(!startFromName("20261109_246502.wav").has_value());   // hour 24
        CHECK(!startFromName("20261109_136002.wav").has_value());   // minute 60
        CHECK(!startFromName("18001109_134502.wav").has_value());   // before 1990
        CHECK(!startFromName("2026110913450").has_value());         // too short
        CHECK(!startFromName("20261109x134502.wav").has_value());   // wrong separator
        // a bad date earlier in the name does not hide a good one later
        CHECK(startFromName("99999999_999999_20261109_134502.wav") == ms);
        // through a file: the name wins; without one the last-write time minus the length
        const std::string named = writeWav("iq_seek_20261109_134502_" + std::to_string(TEST_GETPID()) + ".wav", 4800);
        IqFileSource n;
        CHECK(n.open(named));
        CHECK(n.startSource() == 1 && n.startUnixMs() == *ms);
        IqFileSource p;
        CHECK(p.open(path));
        CHECK(p.startSource() == 2);
        const double lengthMs = 1000.0 * static_cast<double>(kFrames) / kRate;
        const std::int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        std::printf("  E: last-write start %lld ms, now %lld ms, length %.0f ms\n", static_cast<long long>(p.startUnixMs()), static_cast<long long>(nowMs), lengthMs);
        CHECK(std::llabs(p.startUnixMs() - (nowMs - static_cast<std::int64_t>(lengthMs))) < 60000);
        // a file that is not there has no start
        const auto gone = cascade::source::recordingStart("no_such_iq_seek_file.wav", 10.0);
        CHECK(gone.first == 0 && gone.second == 0);
        std::error_code ec;
        fs::remove(named, ec);
    }
    std::error_code ec;
    fs::remove(path, ec);
    return testSummary("test_iq_file_seek");
}
