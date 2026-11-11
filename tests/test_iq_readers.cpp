// Tests for the I/Q file readers (source/iq_readers.hpp).
//
// Every test file is written here BYTE BY BYTE from the format definitions (chunk layouts of RIFF/WAVE, WAVEFORMATEXTENSIBLE, RF64
// per EBU Tech 3306, SigMF JSON), never with the reader's own code, so a misreading in the reader cannot cancel against the same
// misreading in a writer. Expected sample values are worked out by hand from the bytes.
//
// This TU defines its own counting operator new (to bound the largest single allocation during the header fuzz), so it does not
// include alloc_probe.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/iq_readers.hpp"

#include "test_check.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <new>
#include <random>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#endif

namespace {
std::atomic<bool> g_track{false};
std::atomic<std::size_t> g_maxAlloc{0};
}  // namespace

void* operator new(std::size_t n) {
    if (g_track.load(std::memory_order_relaxed)) {
        std::size_t prev = g_maxAlloc.load(std::memory_order_relaxed);
        while (n > prev && !g_maxAlloc.compare_exchange_weak(prev, n)) {}
    }
    void* p = std::malloc(n != 0 ? n : 1);
    if (p == nullptr) { throw std::bad_alloc(); }
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {

namespace iq = cascade::source::iqfile;
namespace fs = std::filesystem;
using cf = std::complex<float>;
using Bytes = std::vector<unsigned char>;

fs::path g_dir;

Bytes& put(Bytes& b, const char* s4) { for (int i = 0; i < 4; ++i) { b.push_back(static_cast<unsigned char>(s4[i])); } return b; }
Bytes& le16(Bytes& b, unsigned v) { b.push_back(v & 0xFF); b.push_back((v >> 8) & 0xFF); return b; }
Bytes& le32(Bytes& b, std::uint32_t v) { for (int i = 0; i < 4; ++i) { b.push_back((v >> (8 * i)) & 0xFF); } return b; }
Bytes& le64(Bytes& b, std::uint64_t v) { for (int i = 0; i < 8; ++i) { b.push_back(static_cast<unsigned char>((v >> (8 * i)) & 0xFF)); } return b; }
Bytes& raw(Bytes& b, std::initializer_list<unsigned> v) { for (unsigned x : v) { b.push_back(static_cast<unsigned char>(x)); } return b; }

std::string writeFile(const std::string& name, const Bytes& b) {
    const fs::path p = g_dir / name;
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    o.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    return p.string();
}
std::string writeText(const std::string& name, const std::string& t) { return writeFile(name, Bytes(t.begin(), t.end())); }

// A fmt chunk: tag, channels, rate, bits (16 bytes of fields, WAVEFORMATEX without cbSize).
Bytes fmtChunk(unsigned tag, unsigned ch, std::uint32_t rate, unsigned bits) {
    Bytes b;
    put(b, "fmt ");
    le32(b, 16);
    le16(b, tag);
    le16(b, ch);
    le32(b, rate);
    le32(b, rate * ch * (bits / 8));
    le16(b, ch * (bits / 8));
    le16(b, bits);
    return b;
}
// WAVEFORMATEXTENSIBLE fmt chunk: 40 bytes.
Bytes fmtExt(unsigned subTag, unsigned ch, std::uint32_t rate, unsigned container, unsigned valid, std::uint32_t mask, bool badGuid = false,
             unsigned cbSize = 22) {
    Bytes b;
    put(b, "fmt ");
    le32(b, 40);
    le16(b, 0xFFFE);
    le16(b, ch);
    le32(b, rate);
    le32(b, rate * ch * (container / 8));
    le16(b, ch * (container / 8));
    le16(b, container);
    le16(b, cbSize);
    le16(b, valid);
    le32(b, mask);
    // SubFormat GUID: Data1 = tag (LE u32), Data2 = 0, Data3 = 0x0010, Data4 = 80 00 00 AA 00 38 9B 71
    le32(b, subTag);
    le16(b, 0);
    le16(b, 0x0010);
    raw(b, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, badGuid ? 0x70u : 0x71u});
    return b;
}
Bytes dataChunk(const Bytes& payload, std::uint32_t sizeField) {
    Bytes b;
    put(b, "data");
    le32(b, sizeField);
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}
Bytes riff(const Bytes& chunks, const char* id = "RIFF", std::uint32_t sizeField = 0) {
    Bytes b;
    put(b, id);
    le32(b, sizeField != 0 ? sizeField : static_cast<std::uint32_t>(chunks.size() + 4));
    put(b, "WAVE");
    b.insert(b.end(), chunks.begin(), chunks.end());
    return b;
}
Bytes cat(Bytes a, const Bytes& b) { a.insert(a.end(), b.begin(), b.end()); return a; }

std::vector<cf> readAll(iq::IqFileReader& r, std::size_t cap = 100000) {
    std::vector<cf> v(std::min<std::uint64_t>(cap, r.info().totalSamples));
    const std::size_t n = r.read(v.data(), v.size());
    v.resize(n);
    return v;
}
bool eq(cf a, double re, double im, double tol = 1e-9) { return std::fabs(a.real() - re) <= tol && std::fabs(a.imag() - im) <= tol; }

// ---- raw ---------------------------------------------------------------------------------------------------------------------

void testRaw() {
    std::string err;
    iq::RawParams prm;
    prm.sampleRateHz = 2.4e6;
    prm.centerFrequencyHz = 100.0e6;
    prm.hasCenter = true;
    // cu8: bytes (I,Q): (0,255) -> (-1,+1); (127,128) -> (-0.5/127.5, +0.5/127.5)
    {
        Bytes b;
        raw(b, {0, 255, 127, 128, 255, 0});
        const std::string p = writeFile("blk2-a.cu8", b);
        auto r = iq::openAuto(p, prm, &err);
        CHECK(r != nullptr);
        if (r == nullptr) { return; }
        CHECK(r->info().container == "raw" && r->info().format == "cu8" && r->info().totalSamples == 3 && r->info().trailingBytes == 0);
        CHECK_NEAR(r->info().sampleRateHz, 2.4e6, 0);
        CHECK(r->info().centerKnown && std::fabs(r->info().centerFrequencyHz - 100e6) < 1e-3);
        const auto v = readAll(*r);
        CHECK(v.size() == 3 && eq(v[0], -1.0, 1.0) && eq(v[1], -0.5 / 127.5, 0.5 / 127.5, 1e-7) && eq(v[2], 1.0, -1.0));
        CHECK(r->read(nullptr, 0) == 0);
    }
    // cs16 little-endian: (00 80)=-32768 -> -1.0 ; (FF 7F)=32767 -> 32767/32768 ; (01 00)=1 ; (00 FF)=-256
    {
        Bytes b;
        raw(b, {0x00, 0x80, 0xFF, 0x7F, 0x01, 0x00, 0x00, 0xFF});
        auto r = iq::openAuto(writeFile("blk2-a.cs16", b), prm, &err);
        CHECK(r != nullptr);
        if (r == nullptr) { return; }
        const auto v = readAll(*r);
        CHECK(v.size() == 2 && eq(v[0], -1.0, 32767.0 / 32768.0) && eq(v[1], 1.0 / 32768.0, -256.0 / 32768.0));
    }
    // cf32 little-endian: 1.0f = 00 00 80 3F ; -2.5f = 00 00 20 C0 ; 0.25f = 00 00 80 3E ; NaN (00 00 C0 7F) reads as 0
    {
        Bytes b;
        raw(b, {0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x20, 0xC0, 0x00, 0x00, 0x80, 0x3E, 0x00, 0x00, 0xC0, 0x7F});
        auto r = iq::openAuto(writeFile("blk2-a.cf32", b), prm, &err);
        CHECK(r != nullptr);
        if (r == nullptr) { return; }
        const auto v = readAll(*r);
        CHECK(v.size() == 2 && eq(v[0], 1.0, -2.5) && eq(v[1], 0.25, 0.0));
    }
    // odd length: 2.5 frames of cu8 -> 2 frames, 1 trailing byte; empty file -> 0 samples
    {
        Bytes b;
        raw(b, {1, 2, 3, 4, 5});
        auto r = iq::openAuto(writeFile("blk2-odd.cu8", b), prm, &err);
        CHECK(r != nullptr && r->info().totalSamples == 2 && r->info().trailingBytes == 1);
        auto e = iq::openAuto(writeFile("blk2-empty.cs16", Bytes{}), prm, &err);
        CHECK(e != nullptr && e->info().totalSamples == 0);
        std::complex<float> z;
        CHECK(e != nullptr && e->read(&z, 1) == 0);
    }
    // no rate -> clear error; missing file -> error
    {
        iq::RawParams none;
        auto r = iq::openAuto(g_dir.string() + "/blk2-a.cu8", none, &err);
        CHECK(r == nullptr && err.find("no sample rate") != std::string::npos);
        auto m = iq::openAuto((g_dir / "blk2-nothere.cu8").string(), prm, &err);
        CHECK(m == nullptr && !err.empty());
        auto x = iq::openAuto((g_dir / "blk2-a.xyz").string(), prm, &err);
        CHECK(x == nullptr && err.find("unrecognised") != std::string::npos);
    }
    // file-name guesses, marked as guesses
    {
        auto g = iq::guessFromFileName("rec_145.5MHz_2.4Msps.cu8");
        CHECK(g.hasFrequency && g.hasRate && std::fabs(g.frequencyHz - 145.5e6) < 1e-3 && std::fabs(g.rateHz - 2.4e6) < 1e-3 && !g.rule.empty());
        g = iq::guessFromFileName("SDRSharp_20201105_123456Z_100000000Hz_IQ.wav");     // date and time must not be taken for the rate
        CHECK(g.hasFrequency && std::fabs(g.frequencyHz - 100e6) < 1e-3 && !g.hasRate);
        g = iq::guessFromFileName("20201105_123456_145500000_1800000_fc.raw");        // two bare integers after a date and a time
        CHECK(g.hasFrequency && g.hasRate && std::fabs(g.frequencyHz - 145.5e6) < 1e-3 && std::fabs(g.rateHz - 1.8e6) < 1e-3);
        g = iq::guessFromFileName("capture.cu8");
        CHECK(!g.hasFrequency && !g.hasRate);
        g = iq::guessFromFileName("x_900kHz_250ksps.cs16");
        CHECK(std::fabs(g.frequencyHz - 900e3) < 1e-6 && std::fabs(g.rateHz - 250e3) < 1e-6);
        // used by the reader only on request, and flagged
        iq::RawParams guess;
        guess.useNameGuess = true;
        Bytes b;
        raw(b, {1, 2, 3, 4});
        auto r = iq::openAuto(writeFile("blk2-rec_433.92MHz_250ksps.cu8", b), guess, &err);
        CHECK(r != nullptr && r->info().guessedFromName && std::fabs(r->info().sampleRateHz - 250e3) < 1e-6 && r->info().centerKnown &&
              std::fabs(r->info().centerFrequencyHz - 433.92e6) < 1);
        guess.useNameGuess = false;
        CHECK(iq::openAuto(g_dir.string() + "/blk2-rec_433.92MHz_250ksps.cu8", guess, &err) == nullptr);
    }
}

// ---- WAV ------------------------------------------------------------------------------------------------------------------------

void testWav() {
    std::string err;
    iq::RawParams none;
    // PCM16 (tag 1): (00 80, FF 7F) and (34 12, CC ED): 0x1234 = 4660 ; 0xEDCC = -4660
    {
        Bytes pay;
        raw(pay, {0x00, 0x80, 0xFF, 0x7F, 0x34, 0x12, 0xCC, 0xED});
        auto r = iq::openAuto(writeFile("blk2-p16.wav", riff(cat(fmtChunk(1, 2, 250000, 16), dataChunk(pay, 8)))), none, &err);
        CHECK(r != nullptr);
        if (r != nullptr) {
            CHECK(r->info().container == "wav" && r->info().format == "pcm16" && r->info().totalSamples == 2 && !r->info().truncated);
            CHECK_NEAR(r->info().sampleRateHz, 250000, 0);
            const auto v = readAll(*r);
            CHECK(v.size() == 2 && eq(v[0], -1.0, 32767.0 / 32768.0) && eq(v[1], 4660.0 / 32768.0, -4660.0 / 32768.0));
        }
    }
    // PCM24: (00 00 80)=-8388608 -> -1.0 ; (FF FF 7F)=8388607 ; (01 00 00)=1 ; (FF FF FF)=-1
    {
        Bytes pay;
        raw(pay, {0x00, 0x00, 0x80, 0xFF, 0xFF, 0x7F, 0x01, 0x00, 0x00, 0xFF, 0xFF, 0xFF});
        auto r = iq::openAuto(writeFile("blk2-p24.wav", riff(cat(fmtChunk(1, 2, 1000000, 24), dataChunk(pay, 12)))), none, &err);
        CHECK(r != nullptr);
        if (r != nullptr) {
            const auto v = readAll(*r);
            CHECK(r->info().format == "pcm24" && v.size() == 2 && eq(v[0], -1.0, 8388607.0 / 8388608.0) && eq(v[1], 1.0 / 8388608.0, -1.0 / 8388608.0));
        }
    }
    // PCM32: (00 00 00 80)=-2^31 -> -1.0 ; (FF FF FF 7F) ; (00 00 00 40)=2^30 -> 0.5 ; (00 00 00 C0) = -2^30 -> -0.5
    {
        Bytes pay;
        raw(pay, {0, 0, 0, 0x80, 0xFF, 0xFF, 0xFF, 0x7F, 0, 0, 0, 0x40, 0, 0, 0, 0xC0});
        auto r = iq::openAuto(writeFile("blk2-p32.wav", riff(cat(fmtChunk(1, 2, 2000000, 32), dataChunk(pay, 16)))), none, &err);
        CHECK(r != nullptr);
        if (r != nullptr) {
            const auto v = readAll(*r);
            CHECK(r->info().format == "pcm32" && v.size() == 2 && eq(v[0], -1.0, 2147483647.0 / 2147483648.0, 1e-6) && eq(v[1], 0.5, -0.5));
        }
    }
    // float32 (tag 3): 1.0 = 00 00 80 3F, -2.5 = 00 00 20 C0, Inf (00 00 80 7F) reads as 0
    {
        Bytes pay;
        raw(pay, {0, 0, 0x80, 0x3F, 0, 0, 0x20, 0xC0, 0, 0, 0x80, 0x7F, 0, 0, 0, 0});
        auto r = iq::openAuto(writeFile("blk2-f32.wav", riff(cat(fmtChunk(3, 2, 48000, 32), dataChunk(pay, 16)))), none, &err);
        CHECK(r != nullptr);
        if (r != nullptr) {
            const auto v = readAll(*r);
            CHECK(r->info().format == "float32" && v.size() == 2 && eq(v[0], 1.0, -2.5) && eq(v[1], 0.0, 0.0));
        }
    }
    // WAVE_FORMAT_EXTENSIBLE: PCM 24-in-32 container with 20 valid bits and channel mask 3 (front left | front right); float; mask 0
    {
        Bytes pay;   // 32-bit container, left-aligned 20 valid bits: 0x80000000 = -1.0
        raw(pay, {0, 0, 0, 0x80, 0, 0, 0, 0x40});
        auto r = iq::openAuto(writeFile("blk2-ext-pcm.wav", riff(cat(fmtExt(1, 2, 900000, 32, 20, 3), dataChunk(pay, 8)))), none, &err);
        CHECK(r != nullptr);
        if (r != nullptr) {
            const auto v = readAll(*r);
            CHECK(r->info().format == "pcm32" && r->info().hasChannelMask && r->info().channelMask == 3 && r->info().validBits == 20);
            CHECK(v.size() == 1 && eq(v[0], -1.0, 0.5));
        }
        Bytes fp;
        raw(fp, {0, 0, 0x80, 0x3F, 0, 0, 0, 0xBF});
        auto f = iq::openAuto(writeFile("blk2-ext-float.wav", riff(cat(fmtExt(3, 2, 900000, 32, 32, 0), dataChunk(fp, 8)))), none, &err);
        CHECK(f != nullptr);
        if (f != nullptr) {
            const auto v = readAll(*f);
            CHECK(f->info().format == "float32" && f->info().channelMask == 0 && v.size() == 1 && eq(v[0], 1.0, -0.5));
        }
    }
    // wrong GUID: refused and the GUID is named (last byte 0x70 instead of 0x71); cbSize 0; sub-format 0x0055 (MP3) -> refused
    {
        Bytes pay;
        raw(pay, {0, 0, 0, 0});
        auto r = iq::openAuto(writeFile("blk2-badguid.wav", riff(cat(fmtExt(1, 2, 48000, 16, 16, 3, true), dataChunk(pay, 4)))), none, &err);
        CHECK(r == nullptr && err.find("SubFormat") != std::string::npos && err.find("9B70") != std::string::npos);
        auto c = iq::openAuto(writeFile("blk2-cb0.wav", riff(cat(fmtExt(1, 2, 48000, 16, 16, 3, false, 0), dataChunk(pay, 4)))), none, &err);
        CHECK(c == nullptr && err.find("cbSize") != std::string::npos);
        auto m = iq::openAuto(writeFile("blk2-mp3.wav", riff(cat(fmtExt(0x55, 2, 48000, 16, 16, 3), dataChunk(pay, 4)))), none, &err);
        CHECK(m == nullptr && err.find("SubFormat") != std::string::npos);
    }
    // refusals: mono, 8-bit PCM, float64, ADPCM tag, zero rate, bad align
    {
        Bytes pay;
        raw(pay, {0, 0, 0, 0, 0, 0, 0, 0});
        CHECK(iq::openAuto(writeFile("blk2-mono.wav", riff(cat(fmtChunk(1, 1, 48000, 16), dataChunk(pay, 8)))), none, &err) == nullptr && err.find("2 channels") != std::string::npos);
        CHECK(iq::openAuto(writeFile("blk2-p8.wav", riff(cat(fmtChunk(1, 2, 48000, 8), dataChunk(pay, 8)))), none, &err) == nullptr && err.find("bit depth 8") != std::string::npos);
        CHECK(iq::openAuto(writeFile("blk2-f64.wav", riff(cat(fmtChunk(3, 2, 48000, 64), dataChunk(pay, 8)))), none, &err) == nullptr && err.find("64") != std::string::npos);
        CHECK(iq::openAuto(writeFile("blk2-adpcm.wav", riff(cat(fmtChunk(2, 2, 48000, 16), dataChunk(pay, 8)))), none, &err) == nullptr && err.find("format tag 2") != std::string::npos);
        CHECK(iq::openAuto(writeFile("blk2-rate0.wav", riff(cat(fmtChunk(1, 2, 0, 16), dataChunk(pay, 8)))), none, &err) == nullptr && err.find("rate of 0") != std::string::npos);
        Bytes bad = fmtChunk(1, 2, 48000, 16);
        bad[8 + 12] = 3;   // block align 3
        CHECK(iq::openAuto(writeFile("blk2-align.wav", riff(cat(bad, dataChunk(pay, 8)))), none, &err) == nullptr && err.find("block align") != std::string::npos);
        CHECK(iq::openAuto(writeFile("blk2-notwav.wav", Bytes{'N', 'O', 'P', 'E', 0, 0, 0, 0, 'W', 'A', 'V', 'E'}), none, &err) == nullptr);
        CHECK(iq::openAuto(writeFile("blk2-tiny.wav", Bytes{'R', 'I'}), none, &err) == nullptr && err.find("too short") != std::string::npos);
        CHECK(iq::openAuto(writeFile("blk2-nodata.wav", riff(fmtChunk(1, 2, 48000, 16))), none, &err) == nullptr && err.find("no data") != std::string::npos);
        CHECK(iq::openAuto(writeFile("blk2-nofmt.wav", riff(dataChunk(pay, 8))), none, &err) == nullptr);
    }
    // sizes: declared data size larger than the file -> clamped + truncated flag; 0 and 0xFFFFFFFF -> to the end of the file;
    // a trailing half frame is ignored; an odd-sized chunk before 'data' is skipped with its pad byte
    {
        Bytes pay;
        raw(pay, {1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 6, 0, 7});   // 3 frames + 1 byte
        auto big = iq::openAuto(writeFile("blk2-short.wav", riff(cat(fmtChunk(1, 2, 48000, 16), dataChunk(pay, 4000)))), none, &err);
        CHECK(big != nullptr && big->info().truncated && big->info().totalSamples == 3 && big->info().trailingBytes == 1);
        for (std::uint32_t field : {0u, 0xFFFFFFFFu}) {
            auto z = iq::openAuto(writeFile("blk2-streamed.wav", riff(cat(fmtChunk(1, 2, 48000, 16), dataChunk(pay, field)), "RIFF", 0xFFFFFFFFu)), none, &err);
            CHECK(z != nullptr && !z->info().truncated && z->info().totalSamples == 3 && z->info().trailingBytes == 1);
            if (z != nullptr) {
                const auto v = readAll(*z);
                CHECK(v.size() == 3 && eq(v[2], 5.0 / 32768.0, 6.0 / 32768.0));
            }
        }
        Bytes list;
        put(list, "LIST");
        le32(list, 5);
        raw(list, {'a', 'b', 'c', 'd', 'e', 0});     // 5 bytes + 1 pad
        auto l = iq::openAuto(writeFile("blk2-list.wav", riff(cat(cat(fmtChunk(1, 2, 48000, 16), list), dataChunk(pay, 12)))), none, &err);
        CHECK(l != nullptr && l->info().totalSamples == 3);
    }
    // RF64: ds64 first, riff/data sizes 0xFFFFFFFF, the 64-bit data size in ds64 (here a small real one)
    {
        Bytes pay;
        raw(pay, {1, 0, 2, 0, 3, 0, 4, 0});
        Bytes ds;
        put(ds, "ds64");
        le32(ds, 28);
        le64(ds, 0);        // riffSize (not used by the reader)
        le64(ds, 8);        // dataSize
        le64(ds, 2);        // sampleCount
        le32(ds, 0);        // tableLength
        Bytes chunks = cat(cat(ds, fmtChunk(1, 2, 3000000, 16)), dataChunk(pay, 0xFFFFFFFFu));
        auto r = iq::openAuto(writeFile("blk2-rf64.wav", riff(chunks, "RF64", 0xFFFFFFFFu)), none, &err);
        CHECK(r != nullptr);
        if (r != nullptr) {
            CHECK(r->info().container == "rf64" && r->info().totalSamples == 2 && !r->info().truncated);
            const auto v = readAll(*r);
            CHECK(v.size() == 2 && eq(v[1], 3.0 / 32768.0, 4.0 / 32768.0));
        }
        // the same with the BW64 id
        auto b = iq::openAuto(writeFile("blk2-bw64.wav", riff(chunks, "BW64", 0xFFFFFFFFu)), none, &err);
        CHECK(b != nullptr && b->info().totalSamples == 2);
        // ds64 chunk too short -> clear error
        Bytes dsShort;
        put(dsShort, "ds64");
        le32(dsShort, 8);
        le64(dsShort, 0);
        auto s = iq::openAuto(writeFile("blk2-rf64short.wav", riff(cat(cat(dsShort, fmtChunk(1, 2, 3000000, 16)), dataChunk(pay, 0xFFFFFFFFu)), "RF64", 0xFFFFFFFFu)), none, &err);
        CHECK(s == nullptr && err.find("ds64") != std::string::npos);
        // data 0xFFFFFFFF in RF64 with NO ds64: read to the end of the file (nothing else is known)
        auto n = iq::openAuto(writeFile("blk2-rf64nods.wav", riff(cat(fmtChunk(1, 2, 3000000, 16), dataChunk(pay, 0xFFFFFFFFu)), "RF64", 0xFFFFFFFFu)), none, &err);
        CHECK(n != nullptr && n->info().totalSamples == 2);
    }
    // a run of zero-size chunks does not make the open walk the whole file: bounded
    {
        Bytes chunks = fmtChunk(1, 2, 48000, 16);
        for (int i = 0; i < 300; ++i) { put(chunks, "junk"); le32(chunks, 0); }
        Bytes pay;
        raw(pay, {1, 0, 2, 0});
        auto r = iq::openAuto(writeFile("blk2-manychunks.wav", riff(cat(chunks, dataChunk(pay, 4)))), none, &err);
        CHECK(r == nullptr && err.find("no data") != std::string::npos);
    }
}

// ---- files over 4 GB (sparse, Windows) ----------------------------------------------------------------------------------------------

#if defined(_WIN32)
bool makeSparse(const std::string& path, const Bytes& header, std::uint64_t dataOffset, std::uint64_t dataBytes, const Bytes& tailFrame) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { return false; }
    DWORD ret = 0;
    if (!DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &ret, nullptr)) { CloseHandle(h); return false; }
    DWORD w = 0;
    WriteFile(h, header.data(), static_cast<DWORD>(header.size()), &w, nullptr);
    LARGE_INTEGER pos;
    pos.QuadPart = static_cast<LONGLONG>(dataOffset + dataBytes - tailFrame.size());
    SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
    WriteFile(h, tailFrame.data(), static_cast<DWORD>(tailFrame.size()), &w, nullptr);
    CloseHandle(h);
    return w == tailFrame.size();
}
#endif

void testOver4Gb() {
#if defined(_WIN32)
    std::string err;
    iq::RawParams none;
    const std::uint64_t frames = 1200000000ULL;               // 1.2e9 PCM16 frames = 4.8e9 data bytes > 4 GiB
    const std::uint64_t bytes = frames * 4;
    const Bytes tail = [] { Bytes t; raw(t, {0x34, 0x12, 0xCC, 0xED}); return t; }();
    // (1) RF64 with ds64 carrying the 64-bit size
    {
        Bytes ds;
        put(ds, "ds64");
        le32(ds, 28);
        le64(ds, bytes + 100);
        le64(ds, bytes);
        le64(ds, frames);
        le32(ds, 0);
        Bytes head = riff(cat(cat(ds, fmtChunk(1, 2, 2000000, 16)), dataChunk(Bytes{}, 0xFFFFFFFFu)), "RF64", 0xFFFFFFFFu);
        const std::uint64_t off = head.size();
        const std::string p = (g_dir / "blk2-big-rf64.wav").string();
        if (!makeSparse(p, head, off, bytes, tail)) { std::printf("SKIP: cannot make a sparse file here\n"); return; }
        auto r = iq::openAuto(p, none, &err);
        CHECK(r != nullptr);
        if (r != nullptr) {
            std::printf("  RF64 over 4 GB: %llu samples\n", static_cast<unsigned long long>(r->info().totalSamples));
            CHECK(r->info().container == "rf64" && r->info().totalSamples == frames && !r->info().truncated);
            CHECK(r->seek(frames - 1));
            cf v;
            CHECK(r->read(&v, 1) == 1 && eq(v, 4660.0 / 32768.0, -4660.0 / 32768.0));
            CHECK(r->read(&v, 1) == 0);
            CHECK(r->seek(0) && r->read(&v, 1) == 1 && eq(v, 0.0, 0.0));
        }
    }
    // (2) plain RIFF whose data size field is 0xFFFFFFFF (and 0): the end of the file decides
    for (std::uint32_t field : {0xFFFFFFFFu, 0u}) {
        Bytes head = riff(cat(fmtChunk(1, 2, 2000000, 16), dataChunk(Bytes{}, field)), "RIFF", 0xFFFFFFFFu);
        const std::uint64_t off = head.size();
        const std::string p = (g_dir / "blk2-big-riff.wav").string();
        if (!makeSparse(p, head, off, bytes, tail)) { return; }
        auto r = iq::openAuto(p, none, &err);
        CHECK(r != nullptr && r->info().totalSamples == frames);
        if (r != nullptr) {
            CHECK(r->seek(frames - 1));
            cf v;
            CHECK(r->read(&v, 1) == 1 && eq(v, 4660.0 / 32768.0, -4660.0 / 32768.0));
        }
    }
    std::error_code ec;
    fs::remove(g_dir / "blk2-big-rf64.wav", ec);
    fs::remove(g_dir / "blk2-big-riff.wav", ec);
#else
    SKIP_LINUX("sparse > 4 GB file test is Windows-only");
#endif
}

// ---- SigMF --------------------------------------------------------------------------------------------------------------------

void testSigmf() {
    std::string err;
    iq::RawParams none;
    // cf32_le with two capture segments (frequency changes at sample 3) and two annotations (one without a count)
    {
        Bytes data;   // 5 samples: (1,0) (0,1) (-1,0) (0,-1) (0.5,-0.5)
        const float vals[10] = {1, 0, 0, 1, -1, 0, 0, -1, 0.5f, -0.5f};
        for (float f : vals) { le32(data, std::bit_cast<std::uint32_t>(f)); }
        writeFile("blk2-s1.sigmf-data", data);
        const std::string meta =
            R"({"global":{"core:datatype":"cf32_le","core:sample_rate":2400000,"core:version":"1.2.0"},)"
            R"("captures":[{"core:sample_start":0,"core:frequency":100000000,"core:datetime":"2020-01-01T00:00:00.000Z"},)"
            R"({"core:sample_start":3,"core:frequency":101000000}],)"
            R"("annotations":[{"core:sample_start":1,"core:sample_count":2,"core:freq_lower_edge":99900000,"core:freq_upper_edge":100100000,"core:label":"burst"},)"
            R"({"core:sample_start":4}]})";
        const std::string mp = writeText("blk2-s1.sigmf-meta", meta);
        auto r = iq::openAuto(mp, none, &err);
        CHECK(r != nullptr);
        if (r != nullptr) {
            const iq::IqInfo& i = r->info();
            CHECK(i.container == "sigmf" && i.format == "cf32_le" && i.totalSamples == 5 && i.sampleRateKnown);
            CHECK_NEAR(i.sampleRateHz, 2.4e6, 0);
            CHECK(i.centerKnown && std::fabs(i.centerFrequencyHz - 100e6) < 1e-3);      // from the FIRST capture segment
            CHECK(i.captures.size() == 2 && i.captures[1].sampleStart == 3 && std::fabs(i.captures[1].frequencyHz - 101e6) < 1e-3 &&
                  i.captures[0].datetime == "2020-01-01T00:00:00.000Z" && i.captures[1].datetime.empty());
            CHECK(i.annotations.size() == 2 && i.annotations[0].label == "burst" && i.annotations[0].hasCount && i.annotations[0].sampleCount == 2 &&
                  i.annotations[0].hasFreqEdges && !i.annotations[1].hasCount && !i.annotations[1].hasFreqEdges && i.annotations[1].sampleStart == 4);
            const auto v = readAll(*r);
            CHECK(v.size() == 5 && eq(v[0], 1, 0) && eq(v[1], 0, 1) && eq(v[3], 0, -1) && eq(v[4], 0.5, -0.5));
        }
        // the .sigmf-data path opens the same recording
        auto d = iq::openAuto((g_dir / "blk2-s1.sigmf-data").string(), none, &err);
        CHECK(d != nullptr && d->info().totalSamples == 5);
    }
    // big-endian types by known bytes: ci16_be (80 00 = -32768, 7F FF = 32767), cf32_be (3F 80 00 00 = 1.0), cu16_le (00 00 -> -1.0), ci8, cu8
    {
        Bytes d1;
        raw(d1, {0x80, 0x00, 0x7F, 0xFF});
        writeFile("blk2-s2.sigmf-data", d1);
        writeText("blk2-s2.sigmf-meta", R"({"global":{"core:datatype":"ci16_be","core:sample_rate":1000,"core:version":"1.0.0"},"captures":[{"core:sample_start":0}]})");
        auto r = iq::openAuto((g_dir / "blk2-s2.sigmf-meta").string(), none, &err);
        CHECK(r != nullptr);
        if (r != nullptr) {
            const auto v = readAll(*r);
            CHECK(v.size() == 1 && eq(v[0], -1.0, 32767.0 / 32768.0));
            CHECK(!r->info().centerKnown);       // a capture without core:frequency
        }
        Bytes d2;
        raw(d2, {0x3F, 0x80, 0x00, 0x00, 0xC0, 0x20, 0x00, 0x00});
        writeFile("blk2-s3.sigmf-data", d2);
        writeText("blk2-s3.sigmf-meta", R"({"global":{"core:datatype":"cf32_be","core:sample_rate":1000},"captures":[]})");
        auto b = iq::openAuto((g_dir / "blk2-s3.sigmf-meta").string(), none, &err);
        CHECK(b != nullptr);
        if (b != nullptr) { const auto v = readAll(*b); CHECK(v.size() == 1 && eq(v[0], 1.0, -2.5)); CHECK(!b->info().centerKnown); }
        Bytes d3;
        raw(d3, {0x00, 0x00, 0x00, 0x80, 0xFF, 0xFF, 0x00, 0x00});   // cu16_le: 0 -> -1, 0x8000 -> 0, 0xFFFF -> +32767/32768, 0 -> -1
        writeFile("blk2-s4.sigmf-data", d3);
        writeText("blk2-s4.sigmf-meta", R"({"global":{"core:datatype":"cu16_le","core:sample_rate":1000}})");
        auto u = iq::openAuto((g_dir / "blk2-s4.sigmf-meta").string(), none, &err);
        CHECK(u != nullptr);
        if (u != nullptr) { const auto v = readAll(*u); CHECK(v.size() == 2 && eq(v[0], -1.0, 0.0) && eq(v[1], 32767.0 / 32768.0, -1.0)); }
        Bytes d4;
        raw(d4, {0x80, 0x7F, 0x00, 0xFF});   // ci8: -128 -> -1, 127 -> 127/128, 0, -1 -> -1/128
        writeFile("blk2-s5.sigmf-data", d4);
        writeText("blk2-s5.sigmf-meta", R"({"global":{"core:datatype":"ci8","core:sample_rate":1000}})");
        auto c8 = iq::openAuto((g_dir / "blk2-s5.sigmf-meta").string(), none, &err);
        CHECK(c8 != nullptr);
        if (c8 != nullptr) { const auto v = readAll(*c8); CHECK(v.size() == 2 && eq(v[0], -1.0, 127.0 / 128.0) && eq(v[1], 0.0, -1.0 / 128.0)); }
    }
    // a missing field: no sample_rate (unknown, or the fallback), no datatype (error), no data file, bad JSON, real and unsupported types
    {
        writeText("blk2-m1.sigmf-meta", R"({"global":{"core:datatype":"cu8"}})");
        writeFile("blk2-m1.sigmf-data", Bytes{1, 2});
        auto r = iq::openAuto((g_dir / "blk2-m1.sigmf-meta").string(), none, &err);
        CHECK(r != nullptr && !r->info().sampleRateKnown);
        iq::RawParams fb;
        fb.sampleRateHz = 123456;
        auto f = iq::openAuto((g_dir / "blk2-m1.sigmf-meta").string(), fb, &err);
        CHECK(f != nullptr && f->info().sampleRateKnown && f->info().sampleRateHz == 123456);
        writeText("blk2-m2.sigmf-meta", R"({"global":{"core:sample_rate":1000}})");
        writeFile("blk2-m2.sigmf-data", Bytes{1, 2});
        auto n = iq::openAuto((g_dir / "blk2-m2.sigmf-meta").string(), none, &err);
        CHECK(n == nullptr && err.find("core:datatype") != std::string::npos);
        writeText("blk2-m3.sigmf-meta", R"({"global":{"core:datatype":"cu8"}})");
        auto nd = iq::openAuto((g_dir / "blk2-m3.sigmf-meta").string(), none, &err);
        CHECK(nd == nullptr && !err.empty());      // no .sigmf-data beside it
        writeText("blk2-m4.sigmf-meta", "{not json");
        writeFile("blk2-m4.sigmf-data", Bytes{1, 2});
        CHECK(iq::openAuto((g_dir / "blk2-m4.sigmf-meta").string(), none, &err) == nullptr && err.find("not valid JSON") != std::string::npos);
        writeText("blk2-m5.sigmf-meta", R"({"nothing":1})");
        writeFile("blk2-m5.sigmf-data", Bytes{1, 2});
        CHECK(iq::openAuto((g_dir / "blk2-m5.sigmf-meta").string(), none, &err) == nullptr && err.find("global") != std::string::npos);
        for (const char* dt : {"ri16_le", "rf32_le", "cf64_le", "ci32_le", "cu32_be", "ci16", "xyz", "cf32"}) {
            writeText("blk2-m6.sigmf-meta", std::string(R"({"global":{"core:sample_rate":1000,"core:datatype":")") + dt + R"("}})");
            writeFile("blk2-m6.sigmf-data", Bytes{1, 2, 3, 4});
            auto x = iq::openAuto((g_dir / "blk2-m6.sigmf-meta").string(), none, &err);
            CHECK(x == nullptr && err.find(dt) != std::string::npos);
        }
        // a capture without core:sample_start is an error; an annotation without one is skipped
        writeText("blk2-m7.sigmf-meta", R"({"global":{"core:datatype":"cu8"},"captures":[{"core:frequency":5}]})");
        writeFile("blk2-m7.sigmf-data", Bytes{1, 2});
        CHECK(iq::openAuto((g_dir / "blk2-m7.sigmf-meta").string(), none, &err) == nullptr && err.find("sample_start") != std::string::npos);
        writeText("blk2-m8.sigmf-meta", R"({"global":{"core:datatype":"cu8"},"annotations":[{"core:label":"x"},{"core:sample_start":0}]})");
        writeFile("blk2-m8.sigmf-data", Bytes{1, 2});
        auto an = iq::openAuto((g_dir / "blk2-m8.sigmf-meta").string(), none, &err);
        CHECK(an != nullptr && an->info().annotations.size() == 1);
        // a data file with a trailing odd byte
        writeFile("blk2-m9.sigmf-data", Bytes{1, 2, 3});
        writeText("blk2-m9.sigmf-meta", R"({"global":{"core:datatype":"cu8"}})");
        auto od = iq::openAuto((g_dir / "blk2-m9.sigmf-meta").string(), none, &err);
        CHECK(od != nullptr && od->info().totalSamples == 1 && od->info().trailingBytes == 1);
    }
}

// ---- seek, chunk boundaries --------------------------------------------------------------------------------------------------------------

void testSeek() {
    std::string err;
    iq::RawParams prm;
    prm.sampleRateHz = 1000;
    // 40000 frames of cs16: I = k mod 30011 (not a power of two), Q = -(k mod 20011): crosses the 16384-frame decode chunk twice
    const std::size_t N = 40000;
    Bytes b;
    for (std::size_t k = 0; k < N; ++k) {
        le16(b, static_cast<unsigned>(k % 30011));
        le16(b, static_cast<unsigned>(static_cast<std::uint16_t>(-static_cast<int>(k % 20011))));
    }
    auto r = iq::openAuto(writeFile("blk2-seek.cs16", b), prm, &err);
    CHECK(r != nullptr);
    if (r == nullptr) { return; }
    const auto expect = [](std::size_t k) { return cf(static_cast<float>(k % 30011) / 32768.0f, -static_cast<float>(k % 20011) / 32768.0f); };
    std::vector<cf> all(N);
    CHECK(r->read(all.data(), N) == N);
    bool ok = true;
    for (std::size_t k = 0; k < N; ++k) { ok = ok && all[k] == expect(k); }
    CHECK(ok);
    CHECK(r->position() == N && r->read(all.data(), 10) == 0);
    // random seeks and short reads agree with the sequential read
    std::mt19937 g(5);
    bool seekOk = true;
    for (int t = 0; t < 300; ++t) {
        const std::size_t at = g() % N, cnt = 1 + g() % 5000;
        seekOk = seekOk && r->seek(at) && r->position() == at;
        std::vector<cf> v(cnt);
        const std::size_t got = r->read(v.data(), cnt);
        seekOk = seekOk && got == std::min(cnt, N - at);
        for (std::size_t i = 0; i < got; ++i) { seekOk = seekOk && v[i] == all[at + i]; }
    }
    CHECK(seekOk);
    CHECK(r->seek(N) && r->read(all.data(), 1) == 0);      // the end itself is a legal position
    CHECK(!r->seek(N + 1));
    CHECK(r->seek(7) && r->position() == 7);                // a refused seek did not move it before, and a legal one does
    CHECK(!r->seek(N + 5) && r->position() == 7);
    // a file that shrinks underneath the reader: a short read, flagged, never garbage
    {
        auto s = iq::openAuto(writeFile("blk2-shrink.cs16", b), prm, &err);
        CHECK(s != nullptr);
        if (s != nullptr) {
            { std::error_code ec; fs::resize_file(g_dir / "blk2-shrink.cs16", 4 * 1000, ec); }
            std::vector<cf> v(5000);
            const std::size_t got = s->read(v.data(), 5000);
            CHECK(got == 1000 && s->info().truncated && s->info().totalSamples == 1000);
        }
    }
}

// ---- header fuzz --------------------------------------------------------------------------------------------------------------------------

void testFuzz() {
    std::string err;
    iq::RawParams none;
    std::mt19937 g(20261111);
    // seed files built from the definitions
    Bytes pay;
    for (int i = 0; i < 64; ++i) { pay.push_back(static_cast<unsigned char>(i * 7)); }
    std::vector<Bytes> seeds;
    seeds.push_back(riff(cat(fmtChunk(1, 2, 48000, 16), dataChunk(pay, 64))));
    seeds.push_back(riff(cat(fmtExt(3, 2, 48000, 32, 32, 3), dataChunk(pay, 64))));
    {
        Bytes ds;
        put(ds, "ds64");
        le32(ds, 28);
        le64(ds, 0);
        le64(ds, 64);
        le64(ds, 8);
        le32(ds, 0);
        seeds.push_back(riff(cat(cat(ds, fmtChunk(1, 2, 48000, 24)), dataChunk(pay, 0xFFFFFFFFu)), "RF64", 0xFFFFFFFFu));
    }
    const std::string meta =
        R"({"global":{"core:datatype":"ci16_le","core:sample_rate":2400000,"core:version":"1.2.0"},)"
        R"("captures":[{"core:sample_start":0,"core:frequency":100000000},{"core:sample_start":8,"core:frequency":1e8}],)"
        R"("annotations":[{"core:sample_start":1,"core:sample_count":2,"core:label":"a"}]})";
    writeFile("blk2-fz.sigmf-data", pay);
    long opened = 0, refused = 0;
    g_maxAlloc = 0;
    g_track = true;
    const int iterations = 6000;
    for (int it = 0; it < iterations; ++it) {
        const std::size_t which = g() % 4;
        std::string path;
        if (which < 3) {
            Bytes b = seeds[which];
            const int flips = 1 + static_cast<int>(g() % 4);
            for (int f = 0; f < flips; ++f) { b[g() % std::min<std::size_t>(b.size(), 80)] = static_cast<unsigned char>(g()); }
            if (g() % 4 == 0) { b.resize(g() % b.size()); }
            if (g() % 8 == 0) { le32(b, g()); }
            path = writeFile("blk2-fz.wav", b);
        } else {
            std::string m = meta;
            const int flips = 1 + static_cast<int>(g() % 4);
            for (int f = 0; f < flips; ++f) { m[g() % m.size()] = static_cast<char>(32 + g() % 95); }
            if (g() % 4 == 0) { m.resize(g() % m.size()); }
            path = writeText("blk2-fz.sigmf-meta", m);
        }
        auto r = iq::openAuto(path, none, &err);
        if (r == nullptr) {
            ++refused;
            CHECK(!err.empty());
            continue;
        }
        ++opened;
        std::vector<cf> v(100);
        r->read(v.data(), v.size());
        r->seek(r->info().totalSamples / 2);
        r->read(v.data(), v.size());
        r->seek(r->info().totalSamples);
        r->read(v.data(), 1);
    }
    g_track = false;
    std::printf("  fuzz: %d mutated headers, %ld opened, %ld refused, largest single allocation %zu bytes\n", iterations, opened, refused,
                g_maxAlloc.load());
    CHECK(opened > 100 && refused > 100);          // both outcomes occur: the loop is exercising the parser, not bouncing off one check
    CHECK(g_maxAlloc.load() < 512 * 1024);          // 128 KiB read buffer is the biggest planned allocation
}

}  // namespace

int main() {
    g_dir = fs::temp_directory_path() / "blk2-iq-readers";
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir, ec);
    testRaw();
    testWav();
    testSigmf();
    testSeek();
    testOver4Gb();
    testFuzz();
    fs::remove_all(g_dir, ec);
    return testSummary("test_iq_readers");
}
