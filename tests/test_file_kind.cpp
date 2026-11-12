// test_file_kind.cpp - what a dropped file IS, by content (core/file_kind.hpp).
//
//   [A] parseWavHeader: RIFF and RF64 headers, the chunk walk, damage, and a fuzz run (random bytes, mutated headers).
//   [B] looksLikeSigMfMeta: the structure the standard guarantees; a BOM; not JSON; a fuzz run.
//   [C] real files on disk: I/Q recording, mono wav, 24-bit stereo, float32 I/Q, RF64, SigMF pair (meta, data, both), a lone meta,
//       a lone data file, bookmark CSV, SDR# XML, plugin module (hand-built PE), program that is not a plugin, plain text, an empty
//       file, a folder, a file that is gone, a renamed file (extension lies), a non-ASCII name, a path over 259 characters.
//   [D] the detail sentence never carries the path or its file name.
//   [E] classifyDrop: order, a pair dropped as two files is one item, the cap and its count, repeats.
//   [F] a fuzz run of classifyFile over random files.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "core/file_kind.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;
using cascade::core::FileKind;
using cascade::core::classifyDrop;
using cascade::core::classifyFile;
using cascade::core::fileKindWord;
using cascade::core::fsPathFromUtf8;
using cascade::core::looksLikeSigMfMeta;
using cascade::core::parseWavHeader;

namespace {

fs::path g_root;

void put16(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
    if (b.size() < at + 2) { b.resize(at + 2); }
    b[at] = static_cast<std::uint8_t>(v);
    b[at + 1] = static_cast<std::uint8_t>(v >> 8);
}
void put32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
    if (b.size() < at + 4) { b.resize(at + 4); }
    for (int i = 0; i < 4; ++i) { b[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i)); }
}

// A WAV: RIFF (or RF64) header, fmt, optional LIST padding, data of `frames` frames.
std::vector<std::uint8_t> wav(std::uint32_t tag, std::uint32_t channels, std::uint32_t rate, std::uint32_t bits, std::size_t frames,
                              bool rf64 = false, std::size_t listBytes = 0, bool withData = true) {
    std::vector<std::uint8_t> b;
    const std::size_t dataBytes = frames * channels * (bits / 8);
    const char* magic = rf64 ? "RF64" : "RIFF";
    b.insert(b.end(), magic, magic + 4);
    put32(b, 4, static_cast<std::uint32_t>(36 + dataBytes));
    b.insert(b.end(), {'W', 'A', 'V', 'E'});
    if (rf64) {   // a ds64 chunk of 28 bytes, as RF64 has it
        b.insert(b.end(), {'d', 's', '6', '4'});
        put32(b, b.size(), 28);
        b.resize(b.size() + 28, 0);
    }
    b.insert(b.end(), {'f', 'm', 't', ' '});
    put32(b, b.size(), 16);
    const std::size_t at = b.size();
    put16(b, at, tag);
    put16(b, at + 2, channels);
    put32(b, at + 4, rate);
    put32(b, at + 8, rate * channels * (bits / 8));
    put16(b, at + 12, channels * (bits / 8));
    put16(b, at + 14, bits);
    if (listBytes > 0) {
        b.insert(b.end(), {'L', 'I', 'S', 'T'});
        put32(b, b.size(), static_cast<std::uint32_t>(listBytes));
        b.resize(b.size() + listBytes + (listBytes & 1u), 'x');
    }
    if (withData) {
        b.insert(b.end(), {'d', 'a', 't', 'a'});
        put32(b, b.size(), static_cast<std::uint32_t>(dataBytes));
        b.resize(b.size() + dataBytes, 0);
    }
    return b;
}

// The PE builder of tests/test_plugin_validate.cpp, cut down: a DLL exporting `names`.
std::vector<std::uint8_t> pe(std::uint32_t characteristics, const std::vector<std::string>& names) {
    std::vector<std::uint8_t> b(0x400, 0);
    b[0] = 'M';
    b[1] = 'Z';
    put32(b, 0x3C, 0x80);
    std::memcpy(&b[0x80], "PE\0\0", 4);
    put16(b, 0x84, 0x8664);
    put16(b, 0x86, 1);
    put16(b, 0x94, 240);
    put16(b, 0x96, characteristics);
    const std::size_t opt = 0x98;
    put16(b, opt, 0x20B);
    put32(b, opt + 60, 0x200);
    put32(b, opt + 108, 16);
    put32(b, opt + 112, 0x1000);
    put32(b, opt + 116, 0x100);
    const std::size_t sec = opt + 240;
    std::memcpy(&b[sec], ".edata\0\0", 8);
    put32(b, sec + 8, 0x200);
    put32(b, sec + 12, 0x1000);
    put32(b, sec + 16, 0x200);
    put32(b, sec + 20, 0x200);
    put32(b, 0x200 + 24, static_cast<std::uint32_t>(names.size()));
    put32(b, 0x200 + 32, 0x1040);
    std::uint32_t nameRva = 0x1080;
    for (std::size_t i = 0; i < names.size(); ++i) {
        put32(b, 0x240 + i * 4, nameRva);
        std::memcpy(&b[0x280 + (nameRva - 0x1080)], names[i].c_str(), names[i].size() + 1);
        nameRva += static_cast<std::uint32_t>(names[i].size() + 1);
    }
    return b;
}

std::string u8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

std::string write(const fs::path& rel, const std::vector<std::uint8_t>& bytes) {
    const fs::path p = fs::path(g_root) / rel;
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return u8(p);
}
std::string writeText(const fs::path& rel, const std::string& text) {
    return write(rel, std::vector<std::uint8_t>(text.begin(), text.end()));
}

const char* kSigMf = "{\n \"global\": {\"core:datatype\": \"cf32_le\", \"core:sample_rate\": 2400000},\n \"captures\": [], \"annotations\": []\n}\n";

void testWavHeader() {
    {
        const auto w = wav(1, 2, 48000, 16, 100);
        const auto h = parseWavHeader(w.data(), w.size());
        CHECK(h.riff && !h.rf64 && h.haveFmt && h.haveData && h.problem.empty());
        CHECK(h.formatTag == 1 && h.channels == 2 && h.sampleRateHz == 48000 && h.bitsPerSample == 16);
    }
    {
        const auto w = wav(3, 2, 2400000, 32, 10, true);
        const auto h = parseWavHeader(w.data(), w.size());
        CHECK(h.rf64 && !h.riff && h.haveFmt && h.haveData);   // the ds64 chunk is walked past
        CHECK(h.formatTag == 3 && h.sampleRateHz == 2400000);
    }
    {   // a LIST chunk before data, odd length (padded)
        const auto w = wav(1, 2, 48000, 16, 10, false, 33);
        const auto h = parseWavHeader(w.data(), w.size());
        CHECK(h.haveFmt && h.haveData);
    }
    {   // not a WAVE
        auto w = wav(1, 2, 48000, 16, 10);
        w[8] = 'X';
        const auto h = parseWavHeader(w.data(), w.size());
        CHECK(!h.riff && !h.rf64 && !h.haveFmt);
    }
    {   // too short, and null
        CHECK(!parseWavHeader(nullptr, 100).riff);
        const std::uint8_t tiny[] = {'R', 'I', 'F', 'F'};
        CHECK(!parseWavHeader(tiny, 4).riff);
        CHECK(!parseWavHeader(tiny, 0).riff);
    }
    {   // fmt chunk too small
        auto w = wav(1, 2, 48000, 16, 10);
        put32(w, 16, 4);
        const auto h = parseWavHeader(w.data(), w.size());
        CHECK(h.riff && !h.haveFmt && !h.problem.empty());
    }
    {   // data before fmt
        std::vector<std::uint8_t> b{'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'd', 'a', 't', 'a', 4, 0, 0, 0, 1, 2, 3, 4};
        const auto h = parseWavHeader(b.data(), b.size());
        CHECK(h.haveData && !h.haveFmt && !h.problem.empty());
    }
    {   // a chunk size that points far past the end: stops, no crash, no data seen
        auto w = wav(1, 2, 48000, 16, 10);
        put32(w, 40, 0xFFFFFFF0u);   // the data chunk size is huge, data tag is still seen first
        const auto h = parseWavHeader(w.data(), w.size());
        CHECK(h.haveData);
        std::vector<std::uint8_t> b = wav(1, 2, 48000, 16, 0, false, 0, false);
        b.insert(b.end(), {'J', 'U', 'N', 'K', 0xF0, 0xFF, 0xFF, 0xFF});
        const auto h2 = parseWavHeader(b.data(), b.size());
        CHECK(h2.haveFmt && !h2.haveData);
    }
    {   // more chunk headers than the cap: bounded
        std::vector<std::uint8_t> b{'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'};
        for (int i = 0; i < 2000; ++i) { b.insert(b.end(), {'J', 'U', 'N', 'K', 0, 0, 0, 0}); }
        const auto h = parseWavHeader(b.data(), b.size());
        CHECK(h.riff && !h.haveFmt && !h.haveData);
    }
    {   // FUZZ: random bytes, and valid headers with random byte flips and truncations
        std::mt19937 rng(12345);
        const auto base = wav(1, 2, 48000, 16, 10, false, 21);
        for (int i = 0; i < 40000; ++i) {
            std::vector<std::uint8_t> b = base;
            if (i % 4 == 0) {
                b.resize(rng() % 200);
                for (auto& x : b) { x = static_cast<std::uint8_t>(rng()); }
            } else {
                const int flips = 1 + static_cast<int>(rng() % 6);
                for (int f = 0; f < flips; ++f) { b[rng() % b.size()] = static_cast<std::uint8_t>(rng()); }
                b.resize(rng() % (b.size() + 1));
            }
            const auto h = parseWavHeader(b.data(), b.size());
            (void)h;
        }
        CHECK(true);   // reaching here is the check: no crash, no hang, no sanitizer report
    }
}

void testSigMfText() {
    const auto asBytes = [](const std::string& s) { return std::vector<std::uint8_t>(s.begin(), s.end()); };
    auto ok = asBytes(kSigMf);
    CHECK(looksLikeSigMfMeta(ok.data(), ok.size()));
    auto bom = asBytes(std::string("\xEF\xBB\xBF") + kSigMf);
    CHECK(looksLikeSigMfMeta(bom.data(), bom.size()));
    auto noGlobal = asBytes("{\"core:datatype\": \"cf32_le\"}");
    CHECK(!looksLikeSigMfMeta(noGlobal.data(), noGlobal.size()));
    auto noType = asBytes("{\"global\": {\"core:sample_rate\": 1}}");
    CHECK(!looksLikeSigMfMeta(noType.data(), noType.size()));
    auto notObject = asBytes("[\"global\", \"core:datatype\"]");
    CHECK(!looksLikeSigMfMeta(notObject.data(), notObject.size()));
    auto globalNoColon = asBytes("{\"global\" \"core:datatype\"}");
    CHECK(!looksLikeSigMfMeta(globalNoColon.data(), globalNoColon.size()));
    auto withNul = asBytes(kSigMf);
    withNul.push_back(0);
    CHECK(!looksLikeSigMfMeta(withNul.data(), withNul.size()));
    CHECK(!looksLikeSigMfMeta(nullptr, 5));
    CHECK(!looksLikeSigMfMeta(ok.data(), 0));
    std::mt19937 rng(777);
    for (int i = 0; i < 40000; ++i) {
        auto b = ok;
        for (int f = 0; f < 4; ++f) { b[rng() % b.size()] = static_cast<std::uint8_t>(rng()); }
        b.resize(rng() % (b.size() + 1));
        (void)looksLikeSigMfMeta(b.data(), b.size());
    }
    CHECK(true);
}

void testFiles() {
    {   // I/Q recordings: 16-bit and float32, the real reader agrees
        const auto a = classifyFile(write("a/iq16.wav", wav(1, 2, 48000, 16, 480)));
        CHECK(a.kind == FileKind::IqRecording);
        CHECK(a.channels == 2 && a.sampleRateHz == 48000 && a.bitsPerSample == 16);
        const auto f = classifyFile(write("a/iqf32.wav", wav(3, 2, 2400000, 32, 480)));
        CHECK(f.kind == FileKind::IqRecording);
        // the extension lies: an I/Q recording called .dat, and a text file called .wav
        CHECK(classifyFile(write("a/iq.dat", wav(1, 2, 48000, 16, 480))).kind == FileKind::IqRecording);
        CHECK(classifyFile(writeText("a/notwav.wav", "hello, not a wave file\n")).kind == FileKind::Unknown);
        // a LIST chunk before the data does not hide it
        CHECK(classifyFile(write("a/iqlist.wav", wav(1, 2, 48000, 16, 480, false, 101))).kind == FileKind::IqRecording);
    }
    {   // other sound files
        const auto m = classifyFile(write("a/mono.wav", wav(1, 1, 48000, 16, 480)));
        CHECK(m.kind == FileKind::Wav && m.channels == 1);
        CHECK(classifyFile(write("a/st24.wav", wav(1, 2, 48000, 24, 480))).kind == FileKind::Wav);
        CHECK(classifyFile(write("a/adpcm.wav", wav(2, 2, 48000, 4, 480))).kind == FileKind::Wav);
        // two channels, right format, but NO data chunk: the real reader refuses, so it is not an I/Q recording
        const auto nd = classifyFile(write("a/nodata.wav", wav(1, 2, 48000, 16, 0, false, 0, false)));
        CHECK(nd.kind == FileKind::Wav);
        // a header cut off inside the fmt chunk
        auto cut = wav(1, 2, 48000, 16, 10);
        cut.resize(26);
        CHECK(classifyFile(write("a/cut.wav", cut)).kind == FileKind::Wav);
        // a header cut off before any chunk
        auto cut2 = wav(1, 2, 48000, 16, 10);
        cut2.resize(14);
        CHECK(classifyFile(write("a/cut2.wav", cut2)).kind == FileKind::Wav);
    }
    {   // RF64
        const auto r = classifyFile(write("a/big.wav", wav(3, 2, 2400000, 32, 100, true)));
        CHECK(r.kind == FileKind::Rf64 && r.channels == 2 && r.sampleRateHz == 2400000);
    }
    {   // SigMF
        const std::string meta = writeText("s/rec.sigmf-meta", kSigMf);
        const std::string data = write("s/rec.sigmf-data", std::vector<std::uint8_t>(64, 1));
        const auto m = classifyFile(meta);
        CHECK(m.kind == FileKind::SigMfPair && m.path == meta && m.dataPath == data);
        const auto d = classifyFile(data);
        CHECK(d.kind == FileKind::SigMfPair && d.path == meta && d.dataPath == data);
        const std::string lone = writeText("s/lone.sigmf-meta", kSigMf);
        CHECK(classifyFile(lone).kind == FileKind::SigMfMeta);
        CHECK(classifyFile(write("s/orphan.sigmf-data", std::vector<std::uint8_t>(8, 3))).kind == FileKind::Unknown);
        // the metadata by content, whatever its name
        CHECK(classifyFile(writeText("s/renamed.json", kSigMf)).kind == FileKind::SigMfMeta);
        // a .sigmf-meta that is not SigMF content, with a data file beside it: no pair
        writeText("s/fake.sigmf-meta", "not json at all");
        write("s/fake.sigmf-data", std::vector<std::uint8_t>(8, 3));
        CHECK(classifyFile(u8(g_root / "s/fake.sigmf-meta")).kind == FileKind::Unknown);
        CHECK(classifyFile(u8(g_root / "s/fake.sigmf-data")).kind == FileKind::Unknown);
    }
    {   // frequency lists
        const auto c = classifyFile(writeText("b/list.csv", "frequency_mhz,name,group,mode\n100.1,Radio One,FM,WFM\n162.55,Weather,WX,NFM\n"));
        CHECK(c.kind == FileKind::BookmarkCsv && c.entries == 2);
        const auto x = classifyFile(writeText("b/freqs.xml", "<ArrayOfMemoryEntry><MemoryEntry><Name>A</Name><Frequency>7100000</Frequency><DetectorType>LSB</DetectorType></MemoryEntry></ArrayOfMemoryEntry>"));
        CHECK(x.kind == FileKind::BookmarkXml && x.entries == 1);
        // wire-b: the SDRuno memory bank is named as what it is, not as SDR# XML
        const auto s1b = classifyFile(writeText("b/bank.s1b", "5980000,Y,AM,Station A,,,8000,AntA\r\n7200000,N,LSB,Station B,,,2800,AntA\r\n"));
        CHECK(s1b.kind == FileKind::BookmarkXml && s1b.entries == 2);
        CHECK(s1b.detail.find("SDRuno memory bank (.s1b)") != std::string::npos);
        CHECK(x.detail == "a frequency list (SDR# XML)");
        CHECK(classifyFile(writeText("b/words.txt", "just some words, no numbers here\nand more\n")).kind == FileKind::Unknown);
        CHECK(classifyFile(writeText("b/empty.txt", "")).kind == FileKind::Unknown);
    }
    {   // plugin modules: judged by core/plugin_module_check
        const auto good = classifyFile(write("p/plug.dll", pe(0x2022, {"cascade_plugin_query"})));
#if defined(_WIN32)
        CHECK(good.kind == FileKind::PluginPackage);
#else
        CHECK(good.kind == FileKind::Unknown);   // a PE image is not loadable here
#endif
        const auto noEntry = classifyFile(write("p/other.dll", pe(0x2022, {"something_else"})));
        CHECK(noEntry.kind == FileKind::Unknown);
        CHECK(!noEntry.detail.empty());
        const auto exe = classifyFile(write("p/prog.exe", pe(0x0022, {"cascade_plugin_query"})));
        CHECK(exe.kind == FileKind::Unknown);
        auto trunc = pe(0x2022, {"cascade_plugin_query"});
        trunc.resize(100);
        CHECK(classifyFile(write("p/trunc.dll", trunc)).kind == FileKind::Unknown);
    }
    {   // not files
        const auto folder = classifyFile(u8(g_root / "a"));
        CHECK(folder.kind == FileKind::Folder);
        CHECK(classifyFile(u8(g_root / "no-such-file.wav")).kind == FileKind::Missing);
        CHECK(classifyFile("").kind == FileKind::Missing);
        // a file that vanishes between the drop and the look
        const std::string gone = write("a/vanish.wav", wav(1, 2, 48000, 16, 10));
        std::error_code ec;
        fs::remove(fsPathFromUtf8(gone), ec);
        CHECK(classifyFile(gone).kind == FileKind::Missing);
    }
    {   // a non-ASCII name (Greek, and a character outside the BMP)
        const std::string name = u8(g_root) + "/\xCE\xB1\xCE\xB2\xCE\xB3-\xF0\x9F\x93\xBB.wav";
        const auto bytes = wav(1, 2, 48000, 16, 100);
        {
            std::ofstream out(fsPathFromUtf8(name), std::ios::binary);
            out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        const auto c = classifyFile(name);
        CHECK(c.kind == FileKind::IqRecording);   // by header; the narrow-path reader is not asked for a non-ASCII name
        CHECK(c.path == name);
    }
    {   // a path over 259 characters
        fs::path deep = g_root;
        std::string seg(60, 'd');
        for (int i = 0; i < 5; ++i) { deep /= seg; }
        std::string longName = u8(deep) + "/long.wav";
        std::error_code ec;
        fs::create_directories(fsPathFromUtf8(u8(deep)), ec);
        CHECK(longName.size() > 300);
        const auto bytes = wav(1, 2, 48000, 16, 100);
        {
            std::ofstream out(fsPathFromUtf8(longName), std::ios::binary);
            CHECK(static_cast<bool>(out));
            out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        const auto c = classifyFile(longName);
        CHECK(c.kind == FileKind::IqRecording);
        CHECK(classifyFile(u8(deep)).kind == FileKind::Folder);
#if defined(_WIN32)
        fs::remove_all(fs::path(std::wstring(LR"(\\?\)") + (g_root / seg).wstring()), ec);   // the legacy limit would stop a plain remove_all
#else
        fs::remove_all(g_root / seg, ec);
#endif
    }
}

void testPrivacy() {
    const std::string secret = "my-secret-callsign-file";
    const std::vector<std::string> paths = {
        write(secret + "1.wav", wav(1, 1, 8000, 16, 10)),
        write(secret + "2.wav", wav(1, 2, 8000, 16, 0, false, 0, false)),
        write(secret + "3.rf", wav(3, 2, 8000, 32, 10, true)),
        writeText(secret + "4.sigmf-meta", kSigMf),
        write(secret + "5.bin", pe(0x2022, {"zzz"})),
        u8(g_root / (secret + "gone.wav")),
        writeText(secret + "6.txt", "plain"),
    };
    for (const auto& p : paths) {
        const auto c = classifyFile(p);
        CHECK(c.detail.find(secret) == std::string::npos);
        CHECK(c.detail.find(u8(g_root)) == std::string::npos);
        CHECK(!c.detail.empty());
    }
}

void testDrop() {
    const std::string meta = writeText("d/pair.sigmf-meta", kSigMf);
    const std::string data = write("d/pair.sigmf-data", std::vector<std::uint8_t>(16, 7));
    const std::string iq = write("d/x.wav", wav(1, 2, 48000, 16, 100));
    const std::string csv = writeText("d/l.csv", "100.1,Radio\n");
    {   // the pair dropped as two files is one item, at the first one's position
        const auto r = classifyDrop({iq, data, csv, meta});
        CHECK(r.size() == 3);
        CHECK(r.size() == 3 && r[0].kind == FileKind::IqRecording && r[1].kind == FileKind::SigMfPair && r[2].kind == FileKind::BookmarkCsv);
        const auto r2 = classifyDrop({meta, data});
        CHECK(r2.size() == 1 && r2[0].kind == FileKind::SigMfPair);
    }
    {   // a path given twice is two items (only a pair folds)
        const auto r = classifyDrop({iq, iq});
        CHECK(r.size() == 2);
    }
    {   // many files: the cap, and the count of those not looked at
        std::vector<std::string> many;
        for (int i = 0; i < 300; ++i) { many.push_back(iq); }
        std::size_t dropped = 0;
        const auto r = classifyDrop(many, 100, &dropped);
        CHECK(r.size() == 100 && dropped == 200);
        const auto r0 = classifyDrop({}, 100, &dropped);
        CHECK(r0.empty() && dropped == 0);
        const auto rz = classifyDrop(many, 0, &dropped);
        CHECK(rz.empty() && dropped == 300);
    }
}

void testFuzzFiles() {
    std::mt19937 rng(4242);
    const auto base = wav(1, 2, 48000, 16, 20);
    const auto peBase = pe(0x2022, {"cascade_plugin_query"});
    int n = 0;
    for (int i = 0; i < 300; ++i) {
        std::vector<std::uint8_t> b = (i % 3 == 0) ? base : (i % 3 == 1) ? peBase : std::vector<std::uint8_t>(rng() % 600);
        for (auto& x : b) { if (i % 3 == 2) { x = static_cast<std::uint8_t>(rng()); } }
        const int flips = static_cast<int>(rng() % 8);
        for (int f = 0; f < flips && !b.empty(); ++f) { b[rng() % b.size()] = static_cast<std::uint8_t>(rng()); }
        if (!b.empty() && rng() % 2 == 0) { b.resize(rng() % b.size()); }
        const auto c = classifyFile(write("fz/f.bin", b));
        n += static_cast<int>(c.kind) >= 0 ? 1 : 0;
    }
    CHECK(n == 300);
}

void testWords() {
    CHECK(std::string(fileKindWord(FileKind::IqRecording)) == "iq-recording");
    CHECK(std::string(fileKindWord(FileKind::SigMfPair)) == "sigmf-pair");
    CHECK(std::string(fileKindWord(FileKind::PluginPackage)) == "plugin-package");
    CHECK(std::string(fileKindWord(FileKind::Rf64)) == "rf64");
    CHECK(std::string(fileKindWord(FileKind::Missing)) == "missing");
}

}  // namespace

int main() {
    g_root = fs::temp_directory_path() / ("foxsdr-test-file-kind-" + std::to_string(static_cast<unsigned long long>(
#if defined(_WIN32)
                                                                  ::GetCurrentProcessId()
#else
                                                                  1
#endif
                                                                      )));
    std::error_code ec;
    fs::remove_all(fsPathFromUtf8(u8(g_root)), ec);
    fs::create_directories(g_root);

    testWavHeader();
    testSigMfText();
    testFiles();
    testPrivacy();
    testDrop();
    testFuzzFiles();
    testWords();

    fs::remove_all(fsPathFromUtf8(u8(g_root)), ec);
    return testSummary("test_file_kind");
}
