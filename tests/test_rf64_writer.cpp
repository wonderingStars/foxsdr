// Tests for the RF64 / WAV I/Q writer (source/rf64_writer.hpp), read back through the existing reader (source/iq_readers.hpp).
//
// Header bytes are checked against hand-computed offsets of the EBU Tech 3306 layout, not only through the reader, so a writer and
// reader that agree on a wrong layout would still fail. Promotion past the plain limit is proved twice: with a lowered limit (fast,
// byte-exact) and with a REAL write of more than 4 GiB when the disk has room (it is cleaned up afterwards).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/iq_readers.hpp"
#include "source/rf64_writer.hpp"

#include "test_check.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>

namespace {

namespace iq = cascade::source::iqfile;
namespace fs = std::filesystem;
using Cx = std::complex<float>;

fs::path scratch(const char* name) {
    fs::path d = fs::temp_directory_path() / "coref-rf64";
    fs::create_directories(d);
    return d / name;
}

std::vector<unsigned char> slurp(const fs::path& p, std::size_t n) {
    std::ifstream f(p, std::ios::binary);
    std::vector<unsigned char> b(n);
    f.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(n));
    b.resize(static_cast<std::size_t>(f.gcount()));
    return b;
}

std::uint32_t le32(const std::vector<unsigned char>& b, std::size_t o) { return iq::wavdetail::get32(b.data() + o); }
std::uint64_t le64(const std::vector<unsigned char>& b, std::size_t o) { return static_cast<std::uint64_t>(le32(b, o)) | (static_cast<std::uint64_t>(le32(b, o + 4)) << 32); }

std::vector<Cx> tone(std::size_t n, float amp = 0.5f) {
    std::vector<Cx> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double ph = 0.013 * static_cast<double>(i);
        v[i] = Cx(static_cast<float>(amp * std::cos(ph)), static_cast<float>(amp * std::sin(ph)));
    }
    return v;
}

double maxErr(const std::vector<Cx>& a, const std::vector<Cx>& b) {
    double m = 0;
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) { m = std::max({m, static_cast<double>(std::abs(a[i].real() - b[i].real())), static_cast<double>(std::abs(a[i].imag() - b[i].imag()))}); }
    return m;
}

std::vector<Cx> readAll(const std::string& path, iq::IqInfo* info, std::string* err) {
    auto r = iq::openWav(path, err);
    if (!r) { return {}; }
    *info = r->info();
    std::vector<Cx> out(static_cast<std::size_t>(r->info().totalSamples));
    const std::size_t got = r->read(out.data(), out.size());
    out.resize(got);
    return out;
}

void roundTrip(iq::WriteFormat fmt, const char* name, const char* fmtName, double tol, bool extensible) {
    const std::string path = scratch(name).string();
    iq::Rf64Options o;
    o.sampleRateHz = 48000;
    o.format = fmt;
    iq::Rf64Writer w;
    std::string err;
    CHECK(w.open(path, o, &err));
    const auto x = tone(50000);
    CHECK(w.write(x.data(), 30000, &err));
    CHECK(w.write(x.data() + 30000, 20000, &err));
    CHECK(w.close(&err));
    iq::IqInfo info;
    const auto y = readAll(path, &info, &err);
    CHECK(y.size() == 50000);
    CHECK(info.container == "wav");
    CHECK(info.format == fmtName);
    CHECK(info.sampleRateHz == 48000.0);
    CHECK(!info.truncated);
    CHECK(info.hasChannelMask == extensible);
    if (extensible) { CHECK(info.channelMask == 3u); }
    const double e = maxErr(x, y);
    std::printf("  %s round trip max error %.3g\n", fmtName, e);
    CHECK(e <= tol);
    fs::remove(path);
}

void testRoundTrips() {
    roundTrip(iq::WriteFormat::F32, "rt_f32.wav", "float32", 0.0, false);
    roundTrip(iq::WriteFormat::S16, "rt_s16.wav", "pcm16", 1.0 / 32767.0, false);
    roundTrip(iq::WriteFormat::S24, "rt_s24.wav", "pcm24", 1.0 / 8388607.0 * 2, true);
    roundTrip(iq::WriteFormat::S32, "rt_s32.wav", "pcm32", 1e-6, true);
}

void testPlainHeaderBytes() {
    const std::string path = scratch("plain.wav").string();
    iq::Rf64Options o;
    o.sampleRateHz = 250000;
    o.format = iq::WriteFormat::S16;
    iq::Rf64Writer w;
    std::string err;
    CHECK(w.open(path, o, &err));
    const auto x = tone(1000);
    CHECK(w.write(x.data(), x.size(), &err));
    CHECK(w.close(&err));
    // header = 12 + (8+28) JUNK + (8+16) fmt + 8 data header = 80 bytes; data = 1000 * 4
    const auto b = slurp(path, 84);
    CHECK(std::memcmp(b.data(), "RIFF", 4) == 0);
    CHECK(le32(b, 4) == 80 + 4000 - 8);
    CHECK(std::memcmp(b.data() + 8, "WAVE", 4) == 0);
    CHECK(std::memcmp(b.data() + 12, "JUNK", 4) == 0);
    CHECK(le32(b, 16) == 28);
    CHECK(std::memcmp(b.data() + 48, "fmt ", 4) == 0);
    CHECK(le32(b, 52) == 16);
    CHECK(b[56] == 1 && b[57] == 0);              // PCM
    CHECK(b[58] == 2);                             // two channels
    CHECK(le32(b, 60) == 250000);                  // rate
    CHECK(le32(b, 64) == 250000 * 4);              // byte rate
    CHECK(b[68] == 4);                             // block align
    CHECK(b[70] == 16);                            // bits
    CHECK(std::memcmp(b.data() + 72, "data", 4) == 0);
    CHECK(le32(b, 76) == 4000);
    CHECK(fs::file_size(path) == 80 + 4000);
    fs::remove(path);
}

void testPromotion() {
    const std::string path = scratch("promote.wav").string();
    iq::Rf64Options o;
    o.sampleRateHz = 1000000;
    o.format = iq::WriteFormat::F32;
    o.plainLimitBytes = 1u << 20;   // promote at 1 MiB instead of 4 GiB
    iq::Rf64Writer w;
    std::string err;
    CHECK(w.open(path, o, &err));
    const auto x = tone(40000);
    std::size_t total = 0;
    bool sawPlain = false;
    for (int i = 0; i < 8; ++i) {   // 8 x 40000 x 8 B = 2.4 MB
        CHECK(w.write(x.data(), x.size(), &err));
        total += x.size();
        if (!w.isRf64()) { sawPlain = true; }
    }
    CHECK(sawPlain);
    CHECK(w.isRf64());
    CHECK(w.close(&err));
    const auto b = slurp(path, 90);
    CHECK(std::memcmp(b.data(), "RF64", 4) == 0);
    CHECK(le32(b, 4) == 0xFFFFFFFFu);
    CHECK(std::memcmp(b.data() + 12, "ds64", 4) == 0);
    CHECK(le32(b, 16) == 28);
    const std::uint64_t dataBytes = static_cast<std::uint64_t>(total) * 8;
    CHECK(le64(b, 20) == 80 + dataBytes - 8);      // riff size = file size - 8
    CHECK(le64(b, 28) == dataBytes);
    CHECK(le64(b, 36) == total);
    CHECK(le32(b, 44) == 0);                       // table length
    CHECK(std::memcmp(b.data() + 48, "fmt ", 4) == 0);
    CHECK(std::memcmp(b.data() + 72, "data", 4) == 0);
    // data chunk size field (header ends at 12 + 36 + 8 + 16 + 8 = 80 for float; data id at 72, size at 76)
    CHECK(le32(b, 76) == 0xFFFFFFFFu);
    CHECK(fs::file_size(path) == 80 + dataBytes);
    iq::IqInfo info;
    const auto y = readAll(path, &info, &err);
    CHECK(info.container == "rf64");
    CHECK(y.size() == total);
    CHECK(!info.truncated);
    std::vector<Cx> whole;
    for (int i = 0; i < 8; ++i) { whole.insert(whole.end(), x.begin(), x.end()); }
    CHECK(maxErr(whole, y) == 0.0);   // the data around the promotion point is intact
    fs::remove(path);
}

void testForceRf64AndEmpty() {
    const std::string path = scratch("force.wav").string();
    iq::Rf64Options o;
    o.sampleRateHz = 8000;
    o.forceRf64 = true;
    iq::Rf64Writer w;
    std::string err;
    CHECK(w.open(path, o, &err));
    CHECK(w.write(nullptr, 0, &err));          // zero input is a no-op
    CHECK(w.close(&err));
    iq::IqInfo info;
    const auto y = readAll(path, &info, &err);
    CHECK(err.empty());
    CHECK(info.container == "rf64");
    CHECK(y.empty());
    CHECK(info.totalSamples == 0);
    // an empty plain file is also valid
    o.forceRf64 = false;
    CHECK(w.open(path, o, &err));
    CHECK(w.close(&err));
    const auto z = readAll(path, &info, &err);
    CHECK(info.container == "wav" && z.empty());
    CHECK(w.close(&err));                      // closing twice is harmless
    fs::remove(path);
}

void testRefusals() {
    iq::Rf64Writer w;
    std::string err;
    iq::Rf64Options o;
    const std::string path = scratch("bad.wav").string();
    o.sampleRateHz = 0;
    CHECK(!w.open(path, o, &err) && !err.empty());
    o.sampleRateHz = -5;
    CHECK(!w.open(path, o, &err));
    o.sampleRateHz = 48000.5;
    CHECK(!w.open(path, o, &err));
    o.sampleRateHz = 5e9;
    CHECK(!w.open(path, o, &err));
    o.sampleRateHz = std::nan("");
    CHECK(!w.open(path, o, &err));
    o.sampleRateHz = 48000;
    o.plainLimitBytes = 10;
    CHECK(!w.open(path, o, &err));
    o.plainLimitBytes = 0xFFFFFFFEull;
    CHECK(!w.open((scratch("nodir") / "x" / "y.wav").string(), o, &err));   // folder does not exist
    CHECK(err.find("cannot create") != std::string::npos);
    Cx s(0, 0);
    CHECK(!w.write(&s, 1, &err));              // not open
    CHECK(!w.flushHeader(&err));
}

void testSampleConditioning() {
    const std::string path = scratch("cond.wav").string();
    iq::Rf64Options o;
    o.sampleRateHz = 48000;
    o.format = iq::WriteFormat::S16;
    iq::Rf64Writer w;
    std::string err;
    CHECK(w.open(path, o, &err));
    const float inf = std::numeric_limits<float>::infinity();
    const std::vector<Cx> x = {Cx(5.0f, -5.0f), Cx(std::nanf(""), inf), Cx(-inf, 0.25f), Cx(1.0f, -1.0f)};
    CHECK(w.write(x.data(), x.size(), &err));
    CHECK(w.close(&err));
    iq::IqInfo info;
    const auto y = readAll(path, &info, &err);
    CHECK(y.size() == 4);
    CHECK(std::abs(y[0].real() - 32767.0f / 32768.0f) < 1e-6f);   // clipped
    CHECK(std::abs(y[0].imag() + 32767.0f / 32768.0f) < 1e-6f);
    CHECK(y[1] == Cx(0, 0));                                       // NaN and +inf read as silence
    CHECK(y[2].real() == 0.0f);                                    // -inf too
    CHECK(std::abs(y[2].imag() - 0.25f) < 1e-4f);
    fs::remove(path);
}

void testCrashAndRepair() {
    const std::string path = scratch("crash.wav").string();
    iq::Rf64Options o;
    o.sampleRateHz = 48000;
    o.format = iq::WriteFormat::F32;
    o.headerUpdateBytes = 8 * 10000;   // every 10000 frames
    iq::Rf64Writer w;
    std::string err;
    CHECK(w.open(path, o, &err));
    const auto x = tone(25000);
    CHECK(w.write(x.data(), x.size(), &err));
    w.abandonForTest();   // crash: 25000 frames are on disk, the header says what the last update said
    // The survivable part: the reader sees the frames up to the last periodic update.
    iq::IqInfo info;
    auto y = readAll(path, &info, &err);
    std::printf("  crash: %zu of 25000 frames visible before repair\n", y.size());
    CHECK(y.size() >= 16384 && y.size() < 25000);   // at most one update interval plus one 16384-frame block is lost
    CHECK(!y.empty() && maxErr(std::vector<Cx>(x.begin(), x.begin() + static_cast<std::ptrdiff_t>(y.size())), y) == 0.0);
    // A torn half frame at the end (a crash in the middle of a sector).
    {
        std::ofstream t(path, std::ios::binary | std::ios::app);
        t.write("abc", 3);
    }
    std::uint64_t frames = 0;
    CHECK(iq::repairWavHeader(path, &err, &frames));
    CHECK(frames == 25000);
    {   // the header's data size is whole frames only: the torn 3 bytes are not counted
        const auto hb = slurp(path, 80);
        CHECK(le32(hb, 76) == 25000u * 8u);
    }
    y = readAll(path, &info, &err);
    CHECK(y.size() == 25000);
    CHECK(maxErr(x, y) == 0.0);
    CHECK(!info.truncated);
    CHECK(iq::repairWavHeader(path, &err, &frames) && frames == 25000);   // idempotent
    fs::remove(path);
}

void testRepairRefusals() {
    std::string err;
    CHECK(!iq::repairWavHeader(scratch("missing.wav").string(), &err));
    const std::string p = scratch("alien.wav").string();
    {
        std::ofstream f(p, std::ios::binary);
        f << "RIFF....WAVEfmt ";
        for (int i = 0; i < 200; ++i) { f.put('x'); }
    }
    CHECK(!iq::repairWavHeader(p, &err));          // no reserved chunk
    {   // a layout that is right except for the id of the reserved chunk
        iq::Rf64Options o;
        o.sampleRateHz = 48000;
        iq::Rf64Writer w;
        CHECK(w.open(p, o, &err));
        const auto x = tone(100);
        CHECK(w.write(x.data(), x.size(), &err));
        CHECK(w.close(&err));
        std::fstream f(p, std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(12);
        f.write("abcd", 4);
        f.close();
        const auto before = slurp(p, 2000);
        CHECK(!iq::repairWavHeader(p, &err));
        CHECK(slurp(p, 2000) == before);
    }
    CHECK(err.find("not written by this writer") != std::string::npos);
    {
        std::ofstream f(p, std::ios::binary);
        f << "short";
    }
    CHECK(!iq::repairWavHeader(p, &err));
    {
        std::mt19937 g(5);
        std::ofstream f(p, std::ios::binary);
        for (int i = 0; i < 500; ++i) { f.put(static_cast<char>(g())); }
    }
    CHECK(!iq::repairWavHeader(p, &err));
    // A foreign file is left byte for byte as it was
    const auto before = slurp(p, 500);
    iq::repairWavHeader(p, &err);
    CHECK(slurp(p, 500) == before);
    fs::remove(p);
}

void testDiskFull() {
    // A write error mid-stream: the writer refuses further writes and close() says so; the file is recoverable.
    const std::string path = scratch("full.wav").string();
    iq::Rf64Options o;
    o.sampleRateHz = 48000;
    iq::Rf64Writer w;
    std::string err;
    CHECK(w.open(path, o, &err));
    const auto x = tone(1000);
    CHECK(w.write(x.data(), x.size(), &err));
    // Make the next write fail by closing the underlying handle through the test hook, then writing.
    w.abandonForTest();
    CHECK(!w.write(x.data(), x.size(), &err));
    CHECK(err.find("not open") != std::string::npos);
    CHECK(iq::repairWavHeader(path, &err));
    fs::remove(path);
}

// A REAL file of more than 4 GiB, written the ordinary way, promoted by the default limit, read back through the reader,
// then deleted. Skipped (printed) when no folder has 12 GiB free.
void testReal4GiB() {
    if (std::getenv("COREF_SKIP_BIG") != nullptr) { std::printf("SKIP real 4 GiB write: COREF_SKIP_BIG is set\n"); return; }
    fs::path dir = fs::temp_directory_path() / "coref-rf64";
    std::error_code ec;
    fs::create_directories(dir, ec);
    constexpr std::uintmax_t kNeed = 12ull << 30;
    if (fs::space(dir, ec).available < kNeed) {
        const fs::path alt = "D:/foxsdr-build/coref-scratch";
        fs::create_directories(alt, ec);
        if (!ec && fs::space(alt, ec).available >= kNeed) { dir = alt; }
        else { std::printf("SKIP real 4 GiB write: less than 12 GiB free on the temp drive and no alternative\n"); return; }
    }
    const std::string path = (dir / "big.wav").string();
    iq::Rf64Options o;
    o.sampleRateHz = 2000000;
    o.format = iq::WriteFormat::S16;   // 4 bytes per frame
    iq::Rf64Writer w;
    std::string err;
    CHECK(w.open(path, o, &err));
    std::vector<Cx> blk(1u << 20);     // 1 Mi frames = 4 MiB per write
    const std::uint64_t target = (4ull << 30) / 4 + 3 * (1ull << 20) + 12345;   // frames: just over 4 GiB of data
    std::uint64_t written = 0;
    std::uint64_t k = 0;
    bool sawPlain = false;
    const auto t0 = std::chrono::steady_clock::now();
    while (written < target) {
        const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(blk.size(), target - written));
        for (std::size_t i = 0; i < n; ++i, ++k) {
            // the frame index is encoded in the sample so a read at any position proves its place
            blk[i] = Cx(static_cast<float>(static_cast<std::int64_t>(k % 30011) - 15005) / 16384.0f, static_cast<float>(static_cast<std::int64_t>(k % 20011) - 10005) / 16384.0f);
        }
        CHECK(w.write(blk.data(), n, &err));
        if (!w.isRf64()) { sawPlain = true; }
        written += n;
    }
    CHECK(w.close(&err));
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(sawPlain);
    CHECK(w.isRf64());
    std::printf("  real write: %.2f GiB of data in %.1f s\n", static_cast<double>(written) * 4.0 / (1u << 30) , sec);
    const auto b = slurp(path, 90);
    CHECK(std::memcmp(b.data(), "RF64", 4) == 0);
    CHECK(le64(b, 28) == written * 4);
    CHECK(le64(b, 36) == written);
    CHECK(le64(b, 20) == fs::file_size(path) - 8);
    CHECK(fs::file_size(path) == 80 + written * 4);
    auto r = iq::openWav(path, &err);
    CHECK(r != nullptr);
    if (r) {
        CHECK(r->info().container == "rf64");
        CHECK(r->info().totalSamples == written);
        CHECK(!r->info().truncated);
        const std::uint64_t probes[] = {0, 1000, 0x3FFFFFFFull, 0x40000000ull, 0x40000000ull + 777, written - 1000};
        for (std::uint64_t p : probes) {
            CHECK(r->seek(p));
            Cx s[4];
            CHECK(r->read(s, 4) == 4);
            for (int j = 0; j < 4; ++j) {
                const std::uint64_t kk = p + static_cast<std::uint64_t>(j);
                const float ex1 = static_cast<float>(static_cast<std::int64_t>(kk % 30011) - 15005) / 16384.0f;
                const float ex2 = static_cast<float>(static_cast<std::int64_t>(kk % 20011) - 10005) / 16384.0f;
                if (std::abs(s[j].real() - ex1) >= 1.0f / 16000.0f) { std::printf("  probe %llu +%d got %.6f want %.6f\n", static_cast<unsigned long long>(p), j, s[j].real(), ex1); }
                CHECK(std::abs(s[j].real() - ex1) < 1.0f / 16000.0f);
                CHECK(std::abs(s[j].imag() - ex2) < 1.0f / 16000.0f);
            }
        }
    }
    r.reset();
    fs::remove(path);
    CHECK(!fs::exists(path));
}

}  // namespace

int main() {
    std::printf("rf64 writer\n");
    testRoundTrips();
    testPlainHeaderBytes();
    testPromotion();
    testForceRf64AndEmpty();
    testRefusals();
    testSampleConditioning();
    testCrashAndRepair();
    testRepairRefusals();
    testDiskFull();
    testReal4GiB();
    return testSummary("test_rf64_writer");
}
