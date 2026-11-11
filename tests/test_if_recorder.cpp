// Tests for the IF (channel) recorder (core/if_recorder.hpp).
//
// The files are read back by the I/Q readers of the previous batch (source/iq_readers.hpp), which know nothing of this writer, and the header bytes are
// checked against the RIFF and RF64 layouts worked out by hand. Over-4 GB files are sparse (Windows).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "alloc_probe.hpp"
#include "core/if_recorder.hpp"
#include "core/rec_browser.hpp"
#include "source/iq_readers.hpp"
#include "test_check.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#endif

namespace {
namespace fs = std::filesystem;
namespace ir = cascade::core::ifrec;
namespace iq = cascade::source::iqfile;
using cf = std::complex<float>;
using Bytes = std::vector<unsigned char>;
fs::path g_dir;

Bytes readAll(const std::string& p) { std::ifstream f(p, std::ios::binary); return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); }
std::uint32_t u32(const Bytes& b, std::size_t at) { return b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (static_cast<std::uint32_t>(b[at + 3]) << 24); }
std::uint64_t u64(const Bytes& b, std::size_t at) { return u32(b, at) | (static_cast<std::uint64_t>(u32(b, at + 4)) << 32); }
bool is(const Bytes& b, std::size_t at, const char* s) { return b.size() >= at + 4 && std::memcmp(b.data() + at, s, 4) == 0; }

ir::IfMeta meta() {
    ir::IfMeta m;
    m.sampleRateHz = 48000.0; m.centerHz = 7074000.0; m.mode = "USB"; m.bandwidthHz = 2400.0;
    m.hasEdges = true; m.lowCutHz = 300.0; m.highCutHz = 2700.0;
    m.radio = "B200 #1"; m.startUnixMs = 1794415314123LL; m.hasTunerCenter = true; m.tunerCenterHz = 7100000.0;
    return m;
}

std::vector<cf> ramp(std::size_t n, std::size_t from = 0) {
    std::vector<cf> v(n);
    for (std::size_t i = 0; i < n; ++i) { v[i] = cf(static_cast<float>(from + i), -static_cast<float>(from + i) * 0.5f); }
    return v;
}

bool writeAll(const std::string& path, const ir::IfMeta& m, ir::SampleFormat f, const std::vector<cf>& x, std::size_t chunk = 1234, std::string* err = nullptr) {
    ir::IfWriter w;
    std::string e;
    if (!w.open(path, m, f, &e)) { if (err != nullptr) { *err = e; } return false; }
    for (std::size_t i = 0; i < x.size(); i += chunk) { if (!w.write(x.data() + i, std::min(chunk, x.size() - i), &e)) { if (err != nullptr) { *err = e; } return false; } }
    return w.finish(&e) || (err != nullptr && (*err = e, false));
}

void testRoundTripFloat() {
    const std::string p = (g_dir / "recm-if-f32.wav").string();
    const std::vector<cf> in = ramp(100000);
    CHECK(writeAll(p, meta(), ir::SampleFormat::Float32, in));
    // header, read by the writer's own header reader
    const ir::IfHeader h = ir::readIfHeader(p);
    CHECK(h.ok && h.hasMeta && !h.rf64 && !h.truncated);
    CHECK(h.format == ir::SampleFormat::Float32 && h.sampleRateHz == 48000.0 && h.frames == 100000);
    CHECK(h.meta.sampleRateHz == 48000.0 && h.meta.centerHz == 7074000.0 && h.meta.mode == "USB" && h.meta.bandwidthHz == 2400.0);
    CHECK(h.meta.hasEdges && h.meta.lowCutHz == 300.0 && h.meta.highCutHz == 2700.0 && h.meta.radio == "B200 #1" && h.meta.startUnixMs == 1794415314123LL);
    CHECK(h.meta.hasTunerCenter && h.meta.tunerCenterHz == 7100000.0);
    // and by the I/Q reader of the previous batch, which has never heard of the chunk
    std::string err;
    auto r = iq::openAuto(p, iq::RawParams{}, &err);
    CHECK(r != nullptr);
    if (r != nullptr) {
        CHECK(r->info().container == "wav" && r->info().sampleRateKnown && r->info().sampleRateHz == 48000.0 && r->info().totalSamples == 100000 && !r->info().truncated);
        std::vector<cf> back(100000);
        CHECK(r->read(back.data(), back.size()) == 100000);
        CHECK(back == in);   // bit exact
        CHECK(r->read(back.data(), 1) == 0);
    }
    // hand-checked layout
    const Bytes b = readAll(p);
    CHECK(is(b, 0, "RIFF") && is(b, 8, "WAVE") && is(b, 12, "JUNK") && u32(b, 16) == 28 && is(b, 48, "fmt ") && u32(b, 52) == 18);
    CHECK(b[56] == 3 && b[57] == 0 && b[58] == 2 && u32(b, 60) == 48000 && u32(b, 64) == 48000 * 8 && b[68] == 8 && b[70] == 32);   // float, 2 ch, rate, byte rate, align, bits
    CHECK(is(b, 74, "fxif"));
    const std::uint32_t jsLen = u32(b, 78);
    const std::size_t dataAt = 82 + jsLen + (jsLen & 1);
    CHECK(is(b, dataAt, "data") && u32(b, dataAt + 4) == 100000 * 8);
    CHECK(u32(b, 4) == b.size() - 8);   // RIFF size = everything after the first 8 bytes
    CHECK(b.size() == dataAt + 8 + 800000);
    float f0; std::memcpy(&f0, &b[dataAt + 8 + 8], 4); CHECK(f0 == 1.0f);   // frame 1, I
    std::printf("float32: 100000 frames written in 1234-frame calls; fxif metadata read back exactly; the I/Q reader of the previous batch reads all frames bit for bit; header bytes as laid out by hand\n");
}

void testPcm16() {
    const std::string p = (g_dir / "recm-if-p16.wav").string();
    std::vector<cf> in = {cf(0, 0), cf(0.5f, -0.5f), cf(1.0f, -1.0f), cf(-1.0f, 1.0f), cf(2.0f, -2.0f), cf(1.5f, 100.0f), cf(std::nanf(""), INFINITY), cf(-INFINITY, 0.25f),
                          cf(1.0f / 65536.0f, -1.0f / 65536.0f), cf(3.0f / 65536.0f, -3.0f / 65536.0f), cf(0.99999f, -0.99999f), cf(1e30f, -1e30f), cf(3.4e38f, -3.4e38f)};
    CHECK(writeAll(p, meta(), ir::SampleFormat::Pcm16, in));
    const ir::IfHeader h = ir::readIfHeader(p);
    CHECK(h.ok && h.format == ir::SampleFormat::Pcm16 && h.frames == in.size());
    std::string err;
    auto r = iq::openAuto(p, iq::RawParams{}, &err);
    CHECK(r != nullptr);
    if (r != nullptr) {
        std::vector<cf> back(in.size());
        CHECK(r->read(back.data(), back.size()) == in.size());
        auto q = [](float v) { return std::isfinite(v) ? static_cast<float>(std::lround(std::max(-32768.0, std::min(32767.0, static_cast<double>(v) * 32768.0)))) / 32768.0f : 0.0f; };
        bool ok = true;
        for (std::size_t i = 0; i < in.size(); ++i) { if (back[i].real() != q(in[i].real()) || back[i].imag() != q(in[i].imag())) { ok = false; std::printf("  frame %zu: got (%g, %g)\n", i, back[i].real(), back[i].imag()); } }
        CHECK(ok);
        CHECK(back[2].real() == 32767.0f / 32768.0f && back[3].real() == -1.0f);       // the rails: +1.0 saturates, -1.0 is exact
        CHECK(back[6] == cf(0.0f, 0.0f) && back[7].real() == 0.0f);                     // non-finite -> 0
        CHECK(back[11] == cf(32767.0f / 32768.0f, -1.0f) && back[12] == cf(32767.0f / 32768.0f, -1.0f));   // huge finite values go to the rails, not to 0 (the C runtime's lround gives 0 there)
        CHECK(back[8].real() == 1.0f / 32768.0f && back[9].real() == 2.0f / 32768.0f);   // 0.5 LSB and 1.5 LSB round half away from zero to 1 and 2 LSB
    }
    const Bytes b = readAll(p);
    CHECK(b[56] == 1 && b[68] == 4 && b[70] == 16);
    std::printf("pcm16: rounding, saturation at both rails and non-finite input = 0 verified against the formula; format fields in the header\n");
}

void testMetaText() {
    ir::IfMeta m = meta();
    m.radio = "quote\" back\\slash tab\t newline\n unicode caf\xC3\xA9 \xE2\x82\xAC";
    m.mode = std::string(100, 'm');
    const std::string p = (g_dir / "recm-if-text.wav").string();
    CHECK(writeAll(p, m, ir::SampleFormat::Float32, ramp(10)));
    const ir::IfHeader h = ir::readIfHeader(p);
    CHECK(h.ok && h.hasMeta);
    CHECK(h.meta.radio == m.radio);                 // JSON escaping round-trips exactly
    CHECK(h.meta.mode.size() == 32);                // cut at 32 bytes
    // a 64-byte cut never splits a UTF-8 sequence
    m.radio.clear(); for (int i = 0; i < 40; ++i) { m.radio += "\xC3\xA9"; }
    CHECK(writeAll(p, m, ir::SampleFormat::Float32, ramp(10)));
    const ir::IfHeader h2 = ir::readIfHeader(p);
    CHECK(h2.ok && h2.meta.radio.size() == 64);
    // invalid UTF-8 in a name: replaced, the file stays valid
    m.radio = std::string("bad\xFF\xFE name");
    CHECK(writeAll(p, m, ir::SampleFormat::Float32, ramp(10)));
    const ir::IfHeader h3 = ir::readIfHeader(p);
    CHECK(h3.ok && h3.hasMeta);
    // odd and even JSON lengths both leave the data chunk on an even offset (pad byte) and readable
    for (int len = 0; len < 6; ++len) {
        m.radio = std::string(static_cast<std::size_t>(len), 'x');
        CHECK(writeAll(p, m, ir::SampleFormat::Float32, ramp(5)));
        std::string err; auto r = iq::openAuto(p, iq::RawParams{}, &err);
        CHECK(r != nullptr && r->info().totalSamples == 5);
        std::vector<cf> back(5); if (r != nullptr) { r->read(back.data(), 5); CHECK(back == ramp(5)); }
    }
    std::printf("metadata text: quotes, backslash, control characters and unicode round-trip; mode cut at 32 bytes and radio at 64 without splitting UTF-8; invalid UTF-8 replaced; odd/even chunk padding\n");
}

void testRefusals() {
    const std::string p = (g_dir / "recm-if-bad.wav").string();
    std::string err;
    ir::IfWriter w;
    auto bad = [&](ir::IfMeta m) { return !w.open(p, m, ir::SampleFormat::Float32, &err) && !err.empty() && !w.isOpen(); };
    ir::IfMeta m = meta();
    m.sampleRateHz = 0; CHECK(bad(m));
    m = meta(); m.sampleRateHz = -48000; CHECK(bad(m));
    m = meta(); m.sampleRateHz = std::nan(""); CHECK(bad(m));
    m = meta(); m.sampleRateHz = 2e9; CHECK(bad(m));
    m = meta(); m.centerHz = -1; CHECK(bad(m));
    m = meta(); m.centerHz = INFINITY; CHECK(bad(m));
    m = meta(); m.bandwidthHz = -5; CHECK(bad(m));
    m = meta(); m.lowCutHz = std::nan(""); CHECK(bad(m));
    m = meta(); m.tunerCenterHz = -2; CHECK(bad(m));
    m = meta(); m.centerHz = 0; m.bandwidthHz = 0; m.hasEdges = false; m.hasTunerCenter = false; CHECK(!bad(m)); CHECK(w.isOpen()); CHECK(w.finish(&err));   // zeros are valid
    CHECK(!w.open((g_dir / "no-such-folder" / "x.wav").string(), meta(), ir::SampleFormat::Float32, &err) && err.find("cannot create") != std::string::npos);
    CHECK(!w.write(nullptr, 0, &err));    // not open
    CHECK(w.finish(&err));                // idempotent
    CHECK(w.open(p, meta(), ir::SampleFormat::Float32, &err));
    CHECK(!w.open(p, meta(), ir::SampleFormat::Float32, &err));   // second open refused
    CHECK(w.write(nullptr, 0, &err));                              // zero frames: no-op
    CHECK(w.finish(&err));
    const ir::IfHeader h = ir::readIfHeader(p);
    CHECK(h.ok && h.frames == 0);
    std::string e2; auto r = iq::openAuto(p, iq::RawParams{}, &e2);
    CHECK(r != nullptr && r->info().totalSamples == 0);
}

void testCrashContract() {
    const std::string p = (g_dir / "recm-if-crash.wav").string();
    ir::IfWriter w;
    std::string err;
    CHECK(w.open(p, meta(), ir::SampleFormat::Float32, &err));
    // (1) before any checkpoint: the header says zero data, the file is a valid zero-length recording and a reader that treats size 0 as "to the end" salvages it
    const std::vector<cf> a = ramp(3000);
    CHECK(w.write(a.data(), a.size(), &err));
    {
        fs::copy_file(p, g_dir / "recm-if-crash-snap0.wav", fs::copy_options::overwrite_existing);
        const Bytes b = readAll((g_dir / "recm-if-crash-snap0.wav").string());
        const std::size_t dataAt = 82 + u32(b, 78) + (u32(b, 78) & 1);
        CHECK(u32(b, dataAt + 4) == 0);                                       // declared size 0 (before the first checkpoint)
        std::string e; auto r = iq::openAuto((g_dir / "recm-if-crash-snap0.wav").string(), iq::RawParams{}, &e);
        CHECK(r != nullptr && r->info().totalSamples == 3000);                // salvaged
        const ir::IfHeader h = ir::readIfHeader((g_dir / "recm-if-crash-snap0.wav").string());
        CHECK(h.ok && h.hasMeta && h.frames == 3000);
    }
    // (2) after a checkpoint the sizes are explicit
    CHECK(w.checkpoint(&err));
    const std::vector<cf> a2 = ramp(2000, 3000);
    CHECK(w.write(a2.data(), a2.size(), &err));                               // written after the checkpoint, not yet declared
    fs::copy_file(p, g_dir / "recm-if-crash-snap1.wav", fs::copy_options::overwrite_existing);
    {
        const Bytes b = readAll((g_dir / "recm-if-crash-snap1.wav").string());
        const std::size_t dataAt = 82 + u32(b, 78) + (u32(b, 78) & 1);
        CHECK(u32(b, dataAt + 4) == 3000 * 8 && u32(b, 4) == dataAt + 8 + 3000 * 8 - 8);
        std::string e; auto r = iq::openAuto((g_dir / "recm-if-crash-snap1.wav").string(), iq::RawParams{}, &e);
        CHECK(r != nullptr && r->info().totalSamples == 3000);               // the declared 3000: at most the last interval is lost
        std::vector<cf> back(3000); if (r != nullptr) { r->read(back.data(), 3000); CHECK(back == a); }
    }
    CHECK(w.finish(&err));
    std::string e; auto r = iq::openAuto(p, iq::RawParams{}, &e);
    CHECK(r != nullptr && r->info().totalSamples == 5000);
    std::printf("crash contract: before a checkpoint the header declares 0 and the reader salvages all 3000 frames; after one it declares exactly what was checkpointed (3000 of 5000 written)\n");
}

void testRf64() {
#if defined(_WIN32)
    // Write a real tiny file with the writer, rewrite its header for a 5.0e9-byte data size (the writer's own RF64 code path), then extend the file sparsely
    // to that size with known bytes at the end.
    const std::string p = (g_dir / "recm-if-big.wav").string();
    CHECK(writeAll(p, meta(), ir::SampleFormat::Float32, ramp(10)));
    const std::uint64_t claimed = 5000000000ULL;   // 6.25e8 frames of 8 bytes
    CHECK(ir::IfWriter::patchSizesForTest(p, claimed));
    {
        const Bytes b = readAll(p);
        CHECK(is(b, 0, "RF64") && u32(b, 4) == 0xFFFFFFFFu && is(b, 8, "WAVE") && is(b, 12, "ds64") && u32(b, 16) == 28);
        const std::size_t jsLen = u32(b, 78), dAt = 82 + jsLen + (jsLen & 1);
        CHECK(is(b, dAt, "data") && u32(b, dAt + 4) == 0xFFFFFFFFu);
        CHECK(u64(b, 28) == claimed && u64(b, 36) == claimed / 8);                       // dataSize and sampleCount
        CHECK(u64(b, 20) == dAt + 8 - 8 + claimed);                                      // riffSize (all after the first 8 bytes)
        CHECK(!is(b, 12, "JUNK"));
    }
    // extend sparsely: the file becomes header + claimed bytes, with the last frame (1.5, -2.5) in place
    const ir::IfHeader h0 = ir::readIfHeader(p);
    const std::uint64_t dataOffset = h0.dataOffset;
    {
        HANDLE h = CreateFileA(p.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE);
        if (h == INVALID_HANDLE_VALUE) { return; }
        DWORD ret = 0, w = 0;
        DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &ret, nullptr);
        LARGE_INTEGER pos; pos.QuadPart = static_cast<LONGLONG>(dataOffset + claimed - 8);
        SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
        const float last[2] = {1.5f, -2.5f};
        WriteFile(h, last, 8, &w, nullptr);
        CloseHandle(h);
    }
    const ir::IfHeader h = ir::readIfHeader(p);
    CHECK(h.ok && h.rf64 && h.hasMeta && h.frames == claimed / 8 && !h.truncated);
    std::string err;
    auto r = iq::openAuto(p, iq::RawParams{}, &err);
    CHECK(r != nullptr);
    if (r != nullptr) {
        CHECK(r->info().container == "rf64" && r->info().totalSamples == claimed / 8 && r->info().sampleRateHz == 48000.0 && !r->info().truncated);
        CHECK(r->seek(claimed / 8 - 1));
        cf v; CHECK(r->read(&v, 1) == 1 && v == cf(1.5f, -2.5f));
        CHECK(r->read(&v, 1) == 0);
    }
    // the boundary: riffSize exactly at kRiffLimit stays a plain RIFF, two bytes more becomes RF64
    const std::string q = (g_dir / "recm-if-edge.wav").string();
    CHECK(writeAll(q, meta(), ir::SampleFormat::Float32, ramp(4)));
    const std::uint64_t hdr = ir::readIfHeader(q).dataOffset;
    const std::uint64_t atLimit = (ir::IfWriter::kRiffLimit - (hdr - 8)) & ~1ull;   // even: no pad byte
    CHECK(ir::IfWriter::patchSizesForTest(q, atLimit));
    { const Bytes b = readAll(q); CHECK(is(b, 0, "RIFF") && is(b, 12, "JUNK")); }
    CHECK(ir::IfWriter::patchSizesForTest(q, atLimit + 2));
    { const Bytes b = readAll(q); CHECK(is(b, 0, "RF64") && is(b, 12, "ds64")); }
    CHECK(ir::IfWriter::patchSizesForTest(q, 1000));    // and back: a smaller claim restores the plain header
    { const Bytes b = readAll(q); CHECK(is(b, 0, "RIFF") && is(b, 12, "JUNK")); }
    std::printf("over 4 GB: a header rewritten for 5.0e9 data bytes is RF64 with ds64 (riffSize, dataSize 5e9, sampleCount 6.25e8, data field 0xFFFFFFFF); the sparse file reads back as %llu frames and its last frame is intact\n",
                static_cast<unsigned long long>(claimed / 8));
    std::error_code ec; fs::remove(p, ec);
#else
    SKIP_LINUX("sparse > 4 GB file test is Windows-only");
#endif
}

void testHostileHeaders() {
    const std::string p = (g_dir / "recm-if-hostile.wav").string();
    CHECK(writeAll(p, meta(), ir::SampleFormat::Float32, ramp(100)));
    const Bytes good = readAll(p);
    const std::size_t jsLen = u32(good, 78), dataAt = 82 + jsLen + (jsLen & 1), headerEnd = dataAt + 8;
    // every truncation of the header: ok only when the data chunk header is complete; never a crash
    int okCount = 0, refused = 0;
    for (std::size_t n = 0; n <= headerEnd + 20 && n <= good.size(); ++n) {
        const std::string tp = (g_dir / "recm-if-cut.wav").string();
        { std::ofstream o(tp, std::ios::binary | std::ios::trunc); o.write(reinterpret_cast<const char*>(good.data()), static_cast<std::streamsize>(n)); }
        const ir::IfHeader h = ir::readIfHeader(tp);
        if (n < headerEnd) { CHECK(!h.ok); }
        if (h.ok) { ++okCount; CHECK(h.hasMeta); } else { ++refused; CHECK(!h.error.empty()); }
    }
    CHECK(okCount == 21 && refused == static_cast<int>(headerEnd));
    // corrupt JSON / wrong magic / missing fields
    auto withJson = [&](const std::string& js) {
        Bytes b(good.begin(), good.begin() + 74);
        b.push_back('f'); b.push_back('x'); b.push_back('i'); b.push_back('f');
        const std::uint32_t L = static_cast<std::uint32_t>(js.size());
        for (int i = 0; i < 4; ++i) { b.push_back((L >> (8 * i)) & 0xFF); }
        b.insert(b.end(), js.begin(), js.end());
        if (L & 1) { b.push_back(0); }
        const char d[] = {'d', 'a', 't', 'a', 0, 0, 0, 0};
        b.insert(b.end(), d, d + 8);
        const std::string tp = (g_dir / "recm-if-json.wav").string();
        { std::ofstream o(tp, std::ios::binary | std::ios::trunc); o.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size())); }
        return ir::readIfHeader(tp);
    };
    CHECK(withJson(R"({"foxsdr_if":1,"sample_rate_hz":48000,"center_hz":1000000})").ok);
    CHECK(!withJson("{not json").ok);
    CHECK(!withJson("[]").ok);
    CHECK(!withJson(R"({"sample_rate_hz":48000,"center_hz":1})").ok);                    // no magic key
    CHECK(!withJson(R"({"foxsdr_if":1,"center_hz":1})").ok);                              // no rate
    CHECK(!withJson(R"({"foxsdr_if":1,"sample_rate_hz":-5,"center_hz":1})").ok);          // invalid rate
    CHECK(!withJson(R"({"foxsdr_if":1,"sample_rate_hz":"48000","center_hz":1})").ok);     // wrong type
    CHECK(!withJson(R"({"foxsdr_if":1,"sample_rate_hz":48000,"center_hz":1e999})").ok);   // out of range
    CHECK(!withJson("").ok);
    { std::string huge(5000, ' '); huge[0] = '{'; huge[4999] = '}'; CHECK(!withJson(huge).ok); }   // over the 4096-byte cap
    // a plain WAV without the chunk: ok but hasMeta false
    const std::string plain = (g_dir / "recm-if-plain.wav").string();
    { Bytes b; auto t = [&](const char* s) { for (int i = 0; i < 4; ++i) { b.push_back(static_cast<unsigned char>(s[i])); } };
      auto l32 = [&](std::uint32_t v) { for (int i = 0; i < 4; ++i) { b.push_back((v >> (8 * i)) & 0xFF); } };
      auto l16 = [&](unsigned v) { b.push_back(v & 0xFF); b.push_back((v >> 8) & 0xFF); };
      t("RIFF"); l32(36 + 16); t("WAVE"); t("fmt "); l32(16); l16(1); l16(2); l32(48000); l32(192000); l16(4); l16(16); t("data"); l32(16); for (int i = 0; i < 16; ++i) { b.push_back(0); }
      std::ofstream o(plain, std::ios::binary | std::ios::trunc); o.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size())); }
    const ir::IfHeader hp = ir::readIfHeader(plain);
    CHECK(hp.ok && !hp.hasMeta && hp.frames == 4);
    CHECK(!ir::readIfHeader((g_dir / "does-not-exist.wav").string()).ok);
    // random mutations of the header: no crash, and a result that is either refused or consistent
    std::mt19937 g(3);
    int ok2 = 0, bad2 = 0;
    for (int i = 0; i < 4000; ++i) {
        Bytes b(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(headerEnd + 8));
        const int k = 1 + static_cast<int>(g() % 4);
        for (int j = 0; j < k; ++j) { b[g() % b.size()] = static_cast<unsigned char>(g()); }
        const std::string tp = (g_dir / "recm-if-mut.wav").string();
        { std::ofstream o(tp, std::ios::binary | std::ios::trunc); o.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size())); }
        const ir::IfHeader h = ir::readIfHeader(tp);
        if (h.ok) { ++ok2; if (h.hasMeta) { CHECK(ir::validateMeta(h.meta).empty()); } } else { ++bad2; }
    }
    CHECK(ok2 + bad2 == 4000);
    std::printf("hostile headers: every truncation of the header refused until complete (%d refused, %d ok), 10 corrupt-JSON rows, plain WAV without the chunk, 4000 random header mutations (%d still read, %d refused), no crash\n", refused, okCount, ok2, bad2);
}

void testAsync() {
    std::string err;
    ir::AsyncIfRecorder rec;
    const std::string p = (g_dir / "recm-if-async.wav").string();
    CHECK(rec.start(p, meta(), ir::SampleFormat::Float32, &err, 1u << 16, 100));
    CHECK(!rec.start(p, meta(), ir::SampleFormat::Float32, &err));
    const std::size_t total = 48000 * 3;
    const std::vector<cf> src = ramp(total);
    double maxPushUs = 0.0; long news = 0;
    std::atomic<std::uint64_t> seenAtSnap{0};
    std::thread producer([&] {
        for (std::size_t at = 0; at < total; at += 480) {
            const std::size_t n = std::min<std::size_t>(480, total - at);
            const auto t0 = std::chrono::steady_clock::now();
            { AllocProbe probe; rec.push(src.data() + at, n); news += probe.news(); }
            maxPushUs = std::max(maxPushUs, std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count());
            if ((at / 480) % 8 == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(3)); }
            if (at == 48000) {   // a snapshot of the file while it is being recorded
                std::error_code ec; fs::copy_file(p, g_dir / "recm-if-async-snap.wav", fs::copy_options::overwrite_existing, ec);
                seenAtSnap.store(rec.acceptedFrames());
            }
        }
    });
    producer.join();
    CHECK(rec.stop(&err));
    CHECK(rec.droppedFrames() == 0 && rec.acceptedFrames() == total);
    CHECK(news == 0);
    CHECK(maxPushUs < 3000.0);
    CHECK(rec.checkpointsWritten() >= 3);   // 100 ms interval over about 0.4 s of wall time
    std::string e; auto r = iq::openAuto(p, iq::RawParams{}, &e);
    CHECK(r != nullptr && r->info().totalSamples == total);
    std::vector<cf> back(total);
    if (r != nullptr) { CHECK(r->read(back.data(), total) == total); CHECK(back == src); }
    // the mid-recording snapshot is a valid prefix
    const ir::IfHeader sh = ir::readIfHeader((g_dir / "recm-if-async-snap.wav").string());
    CHECK(sh.ok && sh.hasMeta && sh.frames > 0 && sh.frames <= seenAtSnap.load());
    std::string e2; auto sr = iq::openAuto((g_dir / "recm-if-async-snap.wav").string(), iq::RawParams{}, &e2);
    CHECK(sr != nullptr);
    if (sr != nullptr) {
        std::vector<cf> pre(static_cast<std::size_t>(sr->info().totalSamples));
        sr->read(pre.data(), pre.size());
        CHECK(std::equal(pre.begin(), pre.end(), src.begin()));   // whatever the snapshot holds is a true prefix of the recording
    }
    std::printf("async: 3 s at 48 kHz from a producer thread: 0 dropped, 0 allocations in %zu pushes, slowest push %.1f us, %d checkpoints, file bit exact; a snapshot taken mid-recording holds %llu frames and is a true prefix\n",
                total / 480, maxPushUs, rec.checkpointsWritten(), static_cast<unsigned long long>(sh.frames));
    // overflow
    ir::AsyncIfRecorder small;
    const std::string p2 = (g_dir / "recm-if-async-drop.wav").string();
    CHECK(small.start(p2, meta(), ir::SampleFormat::Pcm16, &err, 512));
    const std::vector<cf> burst = ramp(100000);
    const std::size_t took = small.push(burst.data(), burst.size());
    CHECK(took <= 512 && small.droppedFrames() == burst.size() - took && small.acceptedFrames() == took);
    CHECK(small.stop(&err));
    CHECK(ir::readIfHeader(p2).frames == took);
    const std::uint64_t droppedFirst = small.droppedFrames();
    CHECK(small.push(burst.data(), 10) == 0);
    CHECK(small.stop(&err));
    ir::AsyncIfRecorder bad;
    CHECK(!bad.start((g_dir / "nope" / "x.wav").string(), meta(), ir::SampleFormat::Float32, &err) && !bad.running());
    // restart
    CHECK(small.start((g_dir / "recm-if-async-2.wav").string(), meta(), ir::SampleFormat::Float32, &err));
    CHECK(small.push(burst.data(), 100) == 100 && small.stop(&err) && ir::readIfHeader((g_dir / "recm-if-async-2.wav").string()).frames == 100);
    std::printf("async overflow: a 100000-frame burst into a 512-frame ring accepted %zu, dropped %llu\n", took, static_cast<unsigned long long>(droppedFirst));
}

void testBrowserSeesIt() {
    const fs::path d = g_dir / "browse";
    fs::create_directories(d);
    CHECK(writeAll((d / "if_7074000_usb.wav").string(), meta(), ir::SampleFormat::Float32, ramp(96000)));
    ir::IfMeta m = meta(); m.startUnixMs = 0; m.mode = "NFM"; m.centerHz = 145500000.0;
    CHECK(writeAll((d / "plain-name.wav").string(), m, ir::SampleFormat::Pcm16, ramp(48000)));
    namespace rb = cascade::core::recbrowse;
    rb::RecordingsBrowser b; b.start(d.string());
    for (int i = 0; i < 1000 && !b.snapshot().done; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    const rb::Snapshot s = b.snapshot();
    CHECK(s.entries->size() == 2);
    for (const rb::Entry& e : *s.entries) {
        CHECK(!e.broken && e.ifRecording && e.kind == rb::Kind::Iq && !e.kindGuessed && e.centerKnown && e.sampleRateHz == 48000.0 && e.channels == 2);
        if (e.fileName == "if_7074000_usb.wav") {
            CHECK(e.centerHz == 7074000.0 && e.mode == "USB" && e.bandwidthHz == 2400.0 && e.radio == "B200 #1" && e.durationS == 2.0 && e.startSource == rb::StartSource::IfHeader && e.startUnixMs == 1794415314123LL);
        } else {
            CHECK(e.centerHz == 145500000.0 && e.mode == "NFM" && e.durationS == 1.0 && e.startSource == rb::StartSource::ModifiedMinusDuration);
        }
    }
    std::printf("browser: an IF recording lists as I/Q (not guessed) with its centre, mode, bandwidth, radio and start time from the header\n");
}

}  // namespace

int main() {
    g_dir = fs::temp_directory_path() / "recm-if";
    std::error_code ec0; fs::remove_all(g_dir, ec0);
    fs::create_directories(g_dir);
    testRoundTripFloat();
    testPcm16();
    testMetaText();
    testRefusals();
    testCrashContract();
    testRf64();
    testHostileHeaders();
    testAsync();
    testBrowserSeesIt();
    std::error_code ec; fs::remove_all(g_dir, ec);
    return testSummary("test_if_recorder");
}
