// test_iq_file_s3.cpp - PLAYING EVERY KIND OF I/Q FILE (screen lane S3, G3-13 and G3-12b; docs/multi-radio-records/screen-s3-recplay.md).
//
//   A  raw cu8, cs16 and cf32 files: the person states the format and the rate, the samples come out as worked out by hand from the bytes
//   B  24-bit and 32-bit integer WAV and WAVE_FORMAT_EXTENSIBLE (open() itself now plays them)
//   C  a SigMF recording, opened by its .sigmf-meta and by its .sigmf-data: the rate and the centre come from the metadata
//   D  hostile and odd files: zero length, one byte, an odd byte count (a trailing partial frame is dropped), truncated, a wrong statement of the format
//      (plays noise, never crashes or hangs), a raw file opened without a statement is refused in words, a rate of 0
//   E  seek and loop work on a file opened through the readers; the engine's raw id parses and refuses bad shapes
//
// Every file is written here byte by byte from the format definitions.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/raw_file_id.hpp"
#include "source/iq_file_source.hpp"
#include "test_check.hpp"

using cascade::source::IqFileSource;
namespace fs = std::filesystem;

namespace {

fs::path dir() {
    static const fs::path d = fs::temp_directory_path() / "foxsdr_test_iq_file_s3";
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
}

std::string write(const std::string& name, const std::vector<unsigned char>& b) {
    const fs::path p = dir() / name;
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    return p.string();
}
std::string writeText(const std::string& name, const std::string& t) { return write(name, std::vector<unsigned char>(t.begin(), t.end())); }

void put32(std::vector<unsigned char>& v, std::uint32_t x) { for (int i = 0; i < 4; ++i) { v.push_back(static_cast<unsigned char>((x >> (8 * i)) & 0xFF)); } }
void put16(std::vector<unsigned char>& v, std::uint16_t x) { for (int i = 0; i < 2; ++i) { v.push_back(static_cast<unsigned char>((x >> (8 * i)) & 0xFF)); } }
void tag(std::vector<unsigned char>& v, const char* t) { for (int i = 0; i < 4; ++i) { v.push_back(static_cast<unsigned char>(t[i])); } }

// A stereo WAV with a plain 16-byte fmt (tag 1 PCM), `bits` per sample, or the 40-byte extensible fmt when `extensible` (sub-format PCM).
std::vector<unsigned char> wav(int bits, bool extensible, const std::vector<unsigned char>& data, std::uint32_t rate = 48000) {
    std::vector<unsigned char> b;
    tag(b, "RIFF");
    put32(b, 0);
    tag(b, "WAVE");
    tag(b, "fmt ");
    put32(b, extensible ? 40 : 16);
    put16(b, extensible ? 0xFFFE : 1);
    put16(b, 2);
    put32(b, rate);
    put32(b, rate * 2 * (bits / 8));
    put16(b, static_cast<std::uint16_t>(2 * (bits / 8)));
    put16(b, static_cast<std::uint16_t>(bits));
    if (extensible) {
        put16(b, 22);
        put16(b, static_cast<std::uint16_t>(bits));
        put32(b, 3);   // front left and right
        put32(b, 1); put16(b, 0); put16(b, 0x10); b.insert(b.end(), {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71});   // the PCM sub-format GUID
    }
    tag(b, "data");
    put32(b, static_cast<std::uint32_t>(data.size()));
    b.insert(b.end(), data.begin(), data.end());
    return b;
}

std::vector<std::complex<float>> readAll(IqFileSource& s, std::size_t n) {
    std::vector<std::complex<float>> out(n);
    CHECK(s.start());
    const std::size_t got = s.read(out.data(), n);
    CHECK(got == n);
    s.stop();
    return out;
}

bool near(float a, float b) { return std::fabs(a - b) < 1e-6f; }

}  // namespace

int main() {
    // ---- A: raw files, the person states format and rate
    {
        const std::string p = write("a.cu8", {0, 255, 255, 0, 128, 127});   // 3 frames
        IqFileSource s;
        CHECK(s.openRaw(p, {"cu8", 2048000.0, 100.5e6, true}));
        CHECK(s.sampleRateHz() == 2048000.0 && s.totalFrames() == 3 && s.centerFrequencyHz() == 100.5e6 && s.centerFromHeader());
        const auto v = readAll(s, 3);
        CHECK(near(v[0].real(), -1.0f) && near(v[0].imag(), 1.0f));          // (u - 127.5) / 127.5
        CHECK(near(v[1].real(), 1.0f) && near(v[1].imag(), -1.0f));
        CHECK(std::fabs(v[2].real() - 0.5f / 127.5f) < 1e-6f && std::fabs(v[2].imag() + 0.5f / 127.5f) < 1e-6f);
    }
    {
        const std::string p = write("a.cs16", {0x00, 0x80, 0xFF, 0x7F, 0x00, 0x00, 0x00, 0x40});   // (-32768, 32767), (0, 16384)
        IqFileSource s;
        CHECK(s.openRaw(p, {"cs16", 250000.0, 0.0, false}));
        CHECK(s.totalFrames() == 2 && !s.centerFromHeader());
        const auto v = readAll(s, 2);
        CHECK(near(v[0].real(), -1.0f) && near(v[0].imag(), 32767.0f / 32768.0f) && near(v[1].real(), 0.0f) && near(v[1].imag(), 0.5f));
    }
    {
        std::vector<unsigned char> b;
        put32(b, 0x3F000000); put32(b, 0xBF800000);   // 0.5, -1.0
        put32(b, 0x7FC00000); put32(b, 0x7F800000);   // NaN, +inf: silence, never passed on
        const std::string p = write("a.cf32", b);
        IqFileSource s;
        CHECK(s.openRaw(p, {"cf32", 1.0e6, 0.0, false}));
        const auto v = readAll(s, 2);
        CHECK(near(v[0].real(), 0.5f) && near(v[0].imag(), -1.0f) && v[1].real() == 0.0f && v[1].imag() == 0.0f);
    }

    // ---- B: 24-bit, 32-bit integer, extensible
    {
        const std::vector<unsigned char> d = {0x00, 0x00, 0x40, 0x00, 0x00, 0xC0,    // I = +0x400000 (0.5), Q = -0x400000 (-0.5)
                                              0xFF, 0xFF, 0x7F, 0x00, 0x00, 0x80};   // I = +0x7FFFFF, Q = -0x800000 (-1.0)
        IqFileSource s;
        CHECK(s.open(write("b24.wav", wav(24, false, d))));
        CHECK(s.totalFrames() == 2 && s.sampleRateHz() == 48000.0);
        const auto v = readAll(s, 2);
        CHECK(near(v[0].real(), 0.5f) && near(v[0].imag(), -0.5f) && near(v[1].real(), 8388607.0f / 8388608.0f) && near(v[1].imag(), -1.0f));
        IqFileSource e;
        CHECK(e.open(write("b24x.wav", wav(24, true, d))));
        const auto w = readAll(e, 2);
        CHECK(near(w[0].real(), 0.5f) && near(w[1].imag(), -1.0f));
    }
    {
        std::vector<unsigned char> d;
        put32(d, 0x40000000u); put32(d, 0xC0000000u);   // 0.5, -0.5
        IqFileSource s;
        CHECK(s.open(write("b32.wav", wav(32, false, d))));
        const auto v = readAll(s, 1);
        CHECK(near(v[0].real(), 0.5f) && near(v[0].imag(), -0.5f));
    }
    {
        IqFileSource s;   // the 16-bit WAV the original reader plays is untouched
        std::vector<unsigned char> d; put16(d, 16384); put16(d, 0);
        CHECK(s.open(write("b16.wav", wav(16, false, d))));
        CHECK(near(readAll(s, 1)[0].real(), 0.5f));
    }

    // ---- C: SigMF
    {
        std::vector<unsigned char> d = {0, 0, 0, 128, 255, 127, 0, 0};   // ci16_le: (0, -32768), (32767, 0)
        write("c.sigmf-data", d);
        const std::string meta = writeText("c.sigmf-meta",
            "{\"global\":{\"core:datatype\":\"ci16_le\",\"core:sample_rate\":2400000,\"core:version\":\"1.0.0\"},"
            "\"captures\":[{\"core:sample_start\":0,\"core:frequency\":433920000}],\"annotations\":[]}");
        IqFileSource s;
        CHECK(s.open(meta));
        CHECK(s.sampleRateHz() == 2400000.0 && s.totalFrames() == 2 && s.centerFrequencyHz() == 433920000.0 && s.centerFromHeader());
        const auto v = readAll(s, 2);
        CHECK(near(v[0].real(), 0.0f) && near(v[0].imag(), -1.0f) && near(v[1].real(), 32767.0f / 32768.0f));
        IqFileSource t;
        CHECK(t.open((dir() / "c.sigmf-data").string()));   // by the data file too
        CHECK(t.sampleRateHz() == 2400000.0);
        const std::string bad = writeText("d.sigmf-meta", "{\"global\":{\"core:datatype\":\"rf32_le\",\"core:sample_rate\":1000}}");
        IqFileSource u;
        CHECK(!u.open(bad) && std::string(u.lastError()).size() > 5 && !u.start());
        const std::string nodata = writeText("e.sigmf-meta", "{\"global\":{\"core:datatype\":\"cf32_le\",\"core:sample_rate\":1000}}");
        IqFileSource w;
        CHECK(!w.open(nodata) && !w.start());   // the data file is missing
    }

    // ---- D: odd and hostile files
    {
        IqFileSource s;
        CHECK(!s.openRaw(write("z.cu8", {}), {"cu8", 1e6, 0, false}) && !s.start());                 // zero length
        CHECK(!s.openRaw(write("one.cu8", {7}), {"cu8", 1e6, 0, false}));                           // one byte: no complete frame
        CHECK(!s.openRaw(write("x.cs16", {1, 2, 3}), {"cs16", 1e6, 0, false}));                      // fewer bytes than one frame
        CHECK(s.openRaw(write("odd.cs16", {1, 0, 2, 0, 3, 0, 4, 0, 9, 9, 9}), {"cs16", 1e6, 0, false}) && s.totalFrames() == 2);   // 11 bytes: the partial frame is dropped
        CHECK(!s.openRaw(write("r0.cu8", {1, 2}), {"cu8", 0.0, 0, false}));                         // no rate
        CHECK(!s.openRaw(write("r1.cu8", {1, 2}), {"zz9", 1e6, 0, false}) && std::string(s.lastError()).find("zz9") != std::string::npos);
        CHECK(!s.openRaw((dir() / "missing.cu8").string(), {"cu8", 1e6, 0, false}));
        CHECK(!s.open(write("noheader.cu8", {1, 2, 3, 4})) && std::string(s.lastError()).find("no header") != std::string::npos);   // open() of a raw file says what is needed
        CHECK(!s.open(write("t24.wav", wav(24, false, {}))));                                         // a 24-bit WAV with no frames
        // a 24-bit file cut short: the data chunk claims 10 bytes, the file holds 5: plays what is there (one frame), never past it
        auto cut = wav(24, false, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10});
        cut.resize(cut.size() - 4);
        CHECK(s.open(write("cut24.wav", cut)) && s.totalFrames() == 1);
        // a wrong statement: a cf32 file read as cu8 is noise, but it opens, plays and loops without a fault
        std::vector<unsigned char> noise;
        for (int i = 0; i < 4000; ++i) { noise.push_back(static_cast<unsigned char>((i * 37) & 0xFF)); }
        CHECK(s.openRaw(write("wrong.cf32", noise), {"cu8", 1e6, 0, false}));
        std::vector<std::complex<float>> buf(10000);
        CHECK(s.start());
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(s.read(buf.data(), buf.size()) == buf.size());   // loops past the end
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5) && !s.faulted());
        for (const auto& c : buf) { CHECK(std::isfinite(c.real()) && std::isfinite(c.imag())); }
    }

    // ---- E: seek and loop on a file opened through the readers; the id of a raw file
    {
        std::vector<unsigned char> b;
        for (int i = 0; i < 1000; ++i) { put16(b, static_cast<std::uint16_t>(i)); put16(b, 0); }
        IqFileSource s;
        CHECK(s.openRaw(write("seek.cs16", b), {"cs16", 1000.0, 0, false}));
        CHECK(s.start() && s.seekFrame(500) && s.positionFrames() == 500);
        std::complex<float> v[2];
        CHECK(s.read(v, 2) == 2);
        CHECK(near(v[0].real(), 500.0f / 32768.0f) && near(v[1].real(), 501.0f / 32768.0f));
        CHECK(s.setLoopFrames(10, 12) && s.loopOn());
        std::complex<float> w[6];
        CHECK(s.read(w, 6) == 6);
        CHECK(near(w[0].real(), 10.0f / 32768.0f) && near(w[2].real(), 10.0f / 32768.0f) && near(w[1].real(), 11.0f / 32768.0f));
    }
    {
        cascade::core::RawFileId r;
        CHECK(cascade::core::parseRawFileId("cu8,2048000,100500000|C:/x y/z|w.cu8", r) && r.path == "C:/x y/z|w.cu8" && r.hasCentre);
        CHECK(!cascade::core::parseRawFileId("cu8,2048000,100500000", r) && !cascade::core::parseRawFileId("cs24,1,1|p", r) && !cascade::core::parseRawFileId("cu8,nan,1|p", r) &&
              !cascade::core::parseRawFileId("cu8,2e9,1|p", r) && !cascade::core::parseRawFileId("cu8,2048000,-1|p", r) && !cascade::core::parseRawFileId("", r));
    }

    std::error_code ec;
    fs::remove_all(dir(), ec);
    return testSummary("test_iq_file_s3");
}
