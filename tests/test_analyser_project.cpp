// Analyser project (core/analyser_project.hpp): SHA-256 against the published vectors and the Windows CNG, the versioned JSON file (round trip, refusals,
// version gate), tags, crop, linked cursors, the reference check against a file on disk, privacy (no echo of user text, no output) and a fuzz run.
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#include <process.h>
#pragma comment(lib, "bcrypt.lib")
#define APR_PID _getpid()
#else
#include <unistd.h>
#define APR_PID getpid()
#endif

#include "core/analyser_project.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using namespace cascade::core::analyser;

namespace {

std::string cngSha256(const std::string& data) {
#if defined(_WIN32)
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) { return "cng-unavailable"; }
    unsigned char digest[32] = {};
    const NTSTATUS st = BCryptHash(alg, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), static_cast<ULONG>(data.size()), digest, 32);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (st != 0) { return "cng-failed"; }
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (unsigned char b : digest) { out += hex[b >> 4]; out += hex[b & 15]; }
    return out;
#else
    (void)data;
    return std::string();
#endif
}

fs::path scratch() {
    fs::path p = fs::current_path() / ("recplay-proj-" + std::to_string(APR_PID));
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

Project sample() {
    Project p;
    p.recording.path = "D:\\Recordings\\2026-11-11\\hf_7100kHz.wav";
    p.recording.sha256 = std::string(64, 'a');
    p.recording.sizeBytes = 123456789;
    p.recording.sampleRateHz = 2400000.0;
    p.recording.centerHz = 7100000.0;
    p.recording.durationSec = 600.0;
    p.crop.hasTime = true; p.crop.startSec = 10.5; p.crop.endSec = 120.25;
    p.crop.hasFreq = true; p.crop.lowHz = 6.5e6; p.crop.highHz = 7.5e6;
    p.cursors.hasTime = true; p.cursors.timeSec = 55.0; p.cursors.hasFreq = true; p.cursors.freqHz = 7.0987e6;
    std::string err;
    Tag a; a.startSec = 12; a.endSec = 20; a.lowHz = 7.0e6; a.highHz = 7.01e6; a.label = "CW beacon"; a.note = "keyed at about 18 wpm\nsecond line, caf\xC3\xA9";
    Tag b; b.startSec = 100; b.endSec = 100; b.lowHz = 7.2e6; b.highHz = 7.2e6; b.label = "";
    addTag(&p, a, &err); addTag(&p, b, &err);
    p.notes = "Evening session, antenna was the dipole.";
    return p;
}

bool same(const Project& a, const Project& b) { return toJson(a) == toJson(b); }

void testSha() {
    CHECK(sha256Hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256Hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(sha256Hex(two.data(), two.size()) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    {
        Sha256 s;
        const std::string chunk(1000, 'a');
        for (int i = 0; i < 1000; ++i) { s.update(chunk.data(), chunk.size()); }
        CHECK(s.finishHex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");   // one million 'a'
    }
    // every length 0..300 around the padding boundaries, random bytes, against the CNG; also fed in odd pieces
    std::mt19937 g(1);
    bool ok = true;
    int cmp = 0;
    for (std::size_t n = 0; n <= 300; ++n) {
        std::string d(n, '\0');
        for (char& c : d) { c = static_cast<char>(g()); }
        const std::string mine = sha256Hex(d.data(), d.size());
        Sha256 s;
        std::size_t pos = 0;
        while (pos < n) { const std::size_t k = std::min<std::size_t>(n - pos, 1 + g() % 70); s.update(d.data() + pos, k); pos += k; }
        ok = ok && s.finishHex() == mine;
#if defined(_WIN32)
        ok = ok && mine == cngSha256(d);
        ++cmp;
#endif
    }
    for (int i = 0; i < 40; ++i) {
        std::string d(1 + g() % 200000, '\0');
        for (char& c : d) { c = static_cast<char>(g()); }
        ok = ok && sha256Hex(d.data(), d.size()) == cngSha256(d);
        ++cmp;
    }
    std::printf("sha256: compared with the Windows CNG on %d buffers\n", cmp);
    CHECK(ok);
    // the hasher is reusable after finishHex
    Sha256 s; s.update("abc", 3); s.finishHex(); s.update("abc", 3);
    CHECK(s.finishHex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

void testFileHashAndReference() {
    const fs::path dir = scratch();
    const fs::path f = dir / "rec.bin";
    std::string data(3 * 1024 * 1024 + 17, '\0');
    std::mt19937 g(5);
    for (char& c : data) { c = static_cast<char>(g()); }
    { std::ofstream o(f, std::ios::binary); o.write(data.data(), static_cast<std::streamsize>(data.size())); }
    std::string hex, err; std::uint64_t bytes = 0;
    std::uint64_t lastProgress = 0;
    CHECK(sha256File(f, &hex, &bytes, &err, nullptr, [&](std::uint64_t b) { lastProgress = b; }));
    CHECK(hex == sha256Hex(data.data(), data.size()) && bytes == data.size() && lastProgress == data.size());
    CHECK(!sha256File(dir / "missing.bin", &hex, &bytes, &err) && !err.empty());
    std::atomic<bool> cancel{true};
    CHECK(!sha256File(f, &hex, &bytes, &err, &cancel) && err == "cancelled");
    Project p = sample();
    p.recording.sha256 = hex; p.recording.sizeBytes = data.size();
    CHECK(verifyReference(p, f) == RefCheck::Match);
    CHECK(verifyReference(p, dir / "nope.bin") == RefCheck::Missing);
    CHECK(verifyReference(p, dir) == RefCheck::Missing);                          // a folder is not a recording
    CHECK(verifyReference(p, f, &cancel) == RefCheck::Cancelled);
    { std::string d2 = data; d2[1000000] ^= 1; std::ofstream o(dir / "flip.bin", std::ios::binary); o.write(d2.data(), static_cast<std::streamsize>(d2.size())); }
    CHECK(verifyReference(p, dir / "flip.bin") == RefCheck::HashDiffers);         // one flipped bit
    { std::ofstream o(dir / "short.bin", std::ios::binary); o.write(data.data(), 1000); }
    CHECK(verifyReference(p, dir / "short.bin") == RefCheck::SizeDiffers);
    p.recording.sha256.clear();
    CHECK(verifyReference(p, f) == RefCheck::NotHashed);
    std::error_code ec; fs::remove_all(dir, ec);
}

void testRoundTrip() {
    const Project p = sample();
    std::string err;
    CHECK(validate(p, &err));
    const std::string text = toJson(p);
    Project q;
    CHECK(parse(text, &q, &err));
    CHECK(same(p, q));
    CHECK(toJson(q) == text);                                                     // stable
    CHECK(q.tags.size() == 2 && q.tags[0].id == 1 && q.tags[1].id == 2 && q.nextTagId == 3);
    CHECK(q.tags[0].note == p.tags[0].note && q.tags[0].label == "CW beacon");
    CHECK(q.crop.hasTime && q.crop.startSec == 10.5 && q.crop.hasFreq && q.crop.highHz == 7.5e6);
    CHECK(q.cursors.hasTime && q.cursors.timeSec == 55.0);
    // optional parts absent
    Project m;
    m.recording.path = "x.wav";
    CHECK(validate(m, &err));
    Project mq;
    CHECK(parse(toJson(m), &mq, &err) && same(m, mq));
    CHECK(!mq.crop.hasTime && !mq.cursors.hasFreq && mq.tags.empty());
    // unknown keys inside version 1 are ignored
    nlohmann::json j = nlohmann::json::parse(text);
    j["futureThing"] = {{"a", 1}};
    j["recording"]["extra"] = "x";
    Project r;
    CHECK(parse(j.dump(), &r, &err) && same(p, r));
}

void testRefusals() {
    const std::string good = toJson(sample());
    Project keep = sample();
    keep.notes = "untouched";
    std::string err;
    // the version gate
    for (const char* v : {"0", "2", "-1", "99", "1.5", "\"1\"", "null", "true", "100000000000000"}) {
        nlohmann::json j = nlohmann::json::parse(good);
        j["version"] = nlohmann::json::parse(v);
        Project out = keep;
        CHECK(!parse(j.dump(), &out, &err));
        CHECK(!err.empty());
        CHECK(out.notes == "untouched");                                           // the output is left unchanged
        if (std::string(v) == "2") { CHECK(err.find("unknown project version 2") != std::string::npos); }
    }
    { nlohmann::json j = nlohmann::json::parse(good); j.erase("version"); CHECK(!parse(j.dump(), nullptr, &err)); }
    { nlohmann::json j = nlohmann::json::parse(good); j["format"] = "something-else"; CHECK(!parse(j.dump(), nullptr, &err)); }
    { nlohmann::json j = nlohmann::json::parse(good); j.erase("format"); CHECK(!parse(j.dump(), nullptr, &err)); }
    // field rules, one at a time
    struct Row { const char* what; std::function<void(nlohmann::json&)> edit; };
    const std::vector<Row> rows = {
        {"no recording", [](nlohmann::json& j) { j.erase("recording"); }},
        {"empty path", [](nlohmann::json& j) { j["recording"]["path"] = ""; }},
        {"path with control char", [](nlohmann::json& j) { j["recording"]["path"] = std::string("a\x01" "b"); }},
        {"path too long", [](nlohmann::json& j) { j["recording"]["path"] = std::string(1025, 'x'); }},
        {"short sha", [](nlohmann::json& j) { j["recording"]["sha256"] = "abcd"; }},
        {"upper-case sha", [](nlohmann::json& j) { j["recording"]["sha256"] = std::string(64, 'A'); }},
        {"non-hex sha", [](nlohmann::json& j) { j["recording"]["sha256"] = std::string(64, 'g'); }},
        {"negative rate", [](nlohmann::json& j) { j["recording"]["sampleRateHz"] = -1; }},
        {"huge rate", [](nlohmann::json& j) { j["recording"]["sampleRateHz"] = 1e12; }},
        {"rate as string", [](nlohmann::json& j) { j["recording"]["sampleRateHz"] = "2400000"; }},
        {"infinite centre", [](nlohmann::json& j) { j["recording"]["centerHz"] = HUGE_VAL; }},
        {"negative duration", [](nlohmann::json& j) { j["recording"]["durationSec"] = -5; }},
        {"negative size", [](nlohmann::json& j) { j["recording"]["sizeBytes"] = -1; }},
        {"crop start without end", [](nlohmann::json& j) { j["crop"].erase("endSec"); }},
        {"crop reversed", [](nlohmann::json& j) { j["crop"]["startSec"] = 200; j["crop"]["endSec"] = 100; }},
        {"crop empty", [](nlohmann::json& j) { j["crop"]["startSec"] = 100; j["crop"]["endSec"] = 100; }},
        {"crop past the end", [](nlohmann::json& j) { j["crop"]["endSec"] = 601; }},
        {"crop negative start", [](nlohmann::json& j) { j["crop"]["startSec"] = -1; }},
        {"crop freq reversed", [](nlohmann::json& j) { j["crop"]["lowHz"] = 7.5e6; j["crop"]["highHz"] = 6.5e6; }},
        {"crop freq outside the band", [](nlohmann::json& j) { j["crop"]["highHz"] = 9e6; }},
        {"cursor time outside", [](nlohmann::json& j) { j["cursors"]["timeSec"] = 1000; }},
        {"cursor freq outside", [](nlohmann::json& j) { j["cursors"]["freqHz"] = 1e6; }},
        {"tags not an array", [](nlohmann::json& j) { j["tags"] = 5; }},
        {"tag not an object", [](nlohmann::json& j) { j["tags"][0] = 5; }},
        {"tag id 0", [](nlohmann::json& j) { j["tags"][0]["id"] = 0; }},
        {"tag id repeated", [](nlohmann::json& j) { j["tags"][1]["id"] = 1; }},
        {"tag id above nextTagId", [](nlohmann::json& j) { j["tags"][1]["id"] = 50; }},
        {"nextTagId missing", [](nlohmann::json& j) { j.erase("nextTagId"); }},
        {"nextTagId 0", [](nlohmann::json& j) { j["nextTagId"] = 0; }},
        {"tag time reversed", [](nlohmann::json& j) { j["tags"][0]["startSec"] = 30; }},
        {"tag freq reversed", [](nlohmann::json& j) { j["tags"][0]["lowHz"] = 7.5e6; }},
        {"tag outside the band", [](nlohmann::json& j) { j["tags"][0]["highHz"] = 9e6; }},
        {"label too long", [](nlohmann::json& j) { j["tags"][0]["label"] = std::string(65, 'x'); }},
        {"label with newline", [](nlohmann::json& j) { j["tags"][0]["label"] = "a\nb"; }},
        {"note too long", [](nlohmann::json& j) { j["tags"][0]["note"] = std::string(2001, 'x'); }},
        {"note with NUL", [](nlohmann::json& j) { j["tags"][0]["note"] = std::string("a\0b", 3); }},
        {"notes too long", [](nlohmann::json& j) { j["notes"] = std::string(20001, 'x'); }},
        {"notes not a string", [](nlohmann::json& j) { j["notes"] = 7; }},
    };
    int n = 0;
    for (const Row& r : rows) {
        nlohmann::json j = nlohmann::json::parse(good);
        r.edit(j);
        Project out = keep;
        std::string e2;
        const bool ok = parse(j.dump(), &out, &e2);
        if (ok) { std::printf("NOT REFUSED: %s\n", r.what); }
        CHECK(!ok);
        CHECK(!e2.empty());
        CHECK(out.notes == "untouched");
        ++n;
    }
    std::printf("refusal rows: %d\n", n);
    // structural
    CHECK(!parse("", nullptr, &err) && !parse("{", nullptr, &err) && !parse("[]", nullptr, &err) && !parse("null", nullptr, &err) && !parse("\xFF\xFE", nullptr, &err));
    CHECK(!parse(std::string(kMaxFileBytes + 1, ' '), nullptr, &err) && err.find("4 MiB") != std::string::npos);
    std::string deep(100, '['); deep += std::string(100, ']');
    CHECK(!parse(deep, nullptr, &err) && err.find("nested") != std::string::npos);
    std::string deepObj; for (int i = 0; i < 70; ++i) { deepObj += "{\"a\":"; } deepObj += "1"; deepObj += std::string(70, '}');
    CHECK(!parse(deepObj, nullptr, &err));
    // NaN / Infinity literals are not JSON
    { std::string t = good; const std::size_t pos = t.find("2400000"); t.replace(pos, 7, "NaN"); CHECK(!parse(t, nullptr, &err)); }
}

void testTagsAndCrop() {
    Project p = sample();
    std::string err;
    Tag t; t.startSec = 1; t.endSec = 2; t.lowHz = 7.0e6; t.highHz = 7.1e6;
    const std::uint32_t id = addTag(&p, t, &err);
    CHECK(id == 3 && p.nextTagId == 4 && p.tags.size() == 3);
    // an invalid add changes nothing
    Tag bad = t; bad.endSec = 0.5;
    const std::string before = toJson(p);
    CHECK(addTag(&p, bad, &err) == 0 && !err.empty() && toJson(p) == before);
    // ids are not reused after a removal
    CHECK(removeTag(&p, id) && !removeTag(&p, id) && !removeTag(&p, 999));
    CHECK(addTag(&p, t, &err) == 4);
    // edit keeps the id; an invalid edit is refused and changes nothing
    Tag e = p.tags[0]; e.label = "renamed"; e.id = 77;
    CHECK(editTag(&p, 1, e, &err) && p.tags[0].id == 1 && p.tags[0].label == "renamed");
    e.startSec = 9999;
    const std::string b2 = toJson(p);
    CHECK(!editTag(&p, 1, e, &err) && toJson(p) == b2);
    CHECK(!editTag(&p, 12345, e, &err) && err == "no such tag");
    // hit testing: smallest box first
    {
        Project h = sample();
        Tag big; big.startSec = 0; big.endSec = 100; big.lowHz = 6.6e6; big.highHz = 7.4e6;
        Tag small; small.startSec = 10; small.endSec = 11; small.lowHz = 7.0e6; small.highHz = 7.0005e6;
        const std::uint32_t ib = addTag(&h, big, &err), is = addTag(&h, small, &err);
        const auto hit = tagsAt(h, 10.5, 7.0002e6);
        CHECK(hit.size() >= 2 && hit[0] == is);
        bool hasBig = false; for (auto x : hit) { hasBig = hasBig || x == ib; }
        CHECK(hasBig);
        CHECK(tagsAt(h, 500, 7.0e6).empty());
    }
    // the tag limit
    Project many;
    many.recording.path = "x.wav";
    many.recording.durationSec = 100;
    Tag m; m.startSec = 1; m.endSec = 2; m.lowHz = 0; m.highHz = 1;
    std::uint32_t last = 0;
    for (std::size_t i = 0; i < kMaxTags; ++i) { last = addTag(&many, m, &err); if (last == 0) { break; } }
    CHECK(many.tags.size() == kMaxTags && last == kMaxTags);
    CHECK(addTag(&many, m, &err) == 0);
    // crop as frames
    Project c = sample();
    std::uint64_t a = 0, b = 0;
    CHECK(cropFrames(c, 1000.0, 1000000, &a, &b) && a == 10500 && b == 120250);
    c.crop.hasTime = false;
    CHECK(cropFrames(c, 1000.0, 1000000, &a, &b) && a == 0 && b == 1000000);
    c.crop.hasTime = true; c.crop.startSec = 0; c.crop.endSec = 600;
    CHECK(cropFrames(c, 1000.0, 5000, &a, &b) && a == 0 && b == 5000);              // clamped to the real length
    CHECK(!cropFrames(c, 0.0, 5000, &a, &b) && !cropFrames(c, std::nan(""), 5000, &a, &b));
    c.crop.startSec = 599.9; c.crop.endSec = 600;
    CHECK(!cropFrames(c, 1000.0, 100, &a, &b));                                       // crop beyond the file: empty
}

void testCursors() {
    LinkedCursors c(600.0, 6.9e6, 7.3e6);
    CHECK(!c.state().hasTime && !c.state().hasFreq && c.state().generation == 0);
    CHECK(c.clickWaterfall(10.0, 7.1e6) == (LinkedCursors::kTime | LinkedCursors::kFreq));
    CHECK(c.state().timeSec == 10.0 && c.state().freqHz == 7.1e6 && c.state().generation == 2);
    CHECK(c.clickWaterfall(10.0, 7.1e6) == LinkedCursors::kNone && c.state().generation == 2);   // no change, no new generation
    CHECK(c.clickSpectrum(7.2e6) == LinkedCursors::kFreq && c.state().timeSec == 10.0 && c.state().freqHz == 7.2e6);
    CHECK(c.clickTimeline(300.0) == LinkedCursors::kTime && c.state().freqHz == 7.2e6 && c.state().timeSec == 300.0);
    // clamped to the span
    c.clickTimeline(9999.0); CHECK(c.state().timeSec == 600.0);
    c.clickTimeline(-5.0); CHECK(c.state().timeSec == 0.0);
    c.clickSpectrum(1e9); CHECK(c.state().freqHz == 7.3e6);
    c.clickSpectrum(-1e9); CHECK(c.state().freqHz == 6.9e6);
    // NaN / infinity refused, nothing moves
    const auto g0 = c.state().generation;
    CHECK(c.clickWaterfall(std::nan(""), 7.1e6) == LinkedCursors::kNone && c.clickSpectrum(HUGE_VAL) == LinkedCursors::kNone && c.clickTimeline(-HUGE_VAL) == LinkedCursors::kNone);
    CHECK(c.state().generation == g0 && c.state().freqHz == 6.9e6);
    // save and restore through the project
    const Cursors s = c.toCursors();
    LinkedCursors d(600.0, 6.9e6, 7.3e6);
    d.restore(s);
    CHECK(d.state().timeSec == c.state().timeSec && d.state().freqHz == c.state().freqHz);
    c.clear(); CHECK(!c.state().hasTime && !c.state().hasFreq);
    // coordinate conversions are inverses
    for (std::size_t n : {256u, 1024u, 4096u}) {
        for (double k : {0.0, 1.0, 100.5, static_cast<double>(n) - 1}) {
            const double f = LinkedCursors::binToFreq(k, n, 7.1e6, 2.4e6);
            CHECK_NEAR(LinkedCursors::freqToBin(f, n, 7.1e6, 2.4e6), k, 1e-6);
        }
    }
    CHECK_NEAR(LinkedCursors::binToFreq(0, 1000, 100e6, 1e6), 100e6 - 0.5e6 + 500.0, 1e-6);   // first bin centre
    CHECK_NEAR(LinkedCursors::sampleToSeconds(48000, 48000.0), 1.0, 1e-12);
    CHECK_NEAR(LinkedCursors::secondsToSample(2.5, 48000.0), 120000.0, 1e-9);
    // a view that clicks a time inside the project restores the same time in the others
    LinkedCursors w(0.0, 0.0, 0.0);                                                        // unknown span: no clamping except time >= 0
    CHECK(w.clickWaterfall(123456.0, 5e12) != LinkedCursors::kNone && w.state().timeSec == 123456.0 && w.state().freqHz == 5e12);
}

void testFiles() {
    const fs::path dir = scratch();
    const fs::path f = dir / "analysis.fxproj";
    Project p = sample();
    std::string err;
    CHECK(saveFile(f, p, &err) && fs::exists(f) && !fs::exists(f.string() + ".tmp"));
    Project q;
    CHECK(loadFile(f, &q, &err) && same(p, q));
    p.notes = "second save";
    CHECK(saveFile(f, p, &err) && loadFile(f, &q, &err) && q.notes == "second save");
    // an invalid project is refused and the old file stays
    Project bad = p; bad.crop.endSec = 1e9;
    CHECK(!saveFile(f, bad, &err) && loadFile(f, &q, &err) && q.notes == "second save");
    // a file that is not ours, an oversized file, a missing file
    { std::ofstream o(dir / "junk.fxproj", std::ios::binary); o << "not json"; }
    CHECK(!loadFile(dir / "junk.fxproj", &q, &err));
    { std::ofstream o(dir / "big.fxproj", std::ios::binary); o << std::string(kMaxFileBytes + 10, ' '); }
    CHECK(!loadFile(dir / "big.fxproj", &q, &err) && err.find("4 MiB") != std::string::npos);
    CHECK(!loadFile(dir / "none.fxproj", &q, &err));
    CHECK(!saveFile(dir / "nodir" / "x.fxproj", p, &err));
    std::error_code ec; fs::remove_all(dir, ec);
}

void testPrivacy() {
    // an invalid file carrying user text: the error names a field and never repeats the text or the path
    nlohmann::json j = nlohmann::json::parse(toJson(sample()));
    j["notes"] = "SECRET-NOTE-9d8f";
    j["tags"][0]["note"] = "SECRET-TAGNOTE-4411";
    j["tags"][0]["label"] = "SECRET-LABEL-7732";
    j["recording"]["path"] = "D:\\Users\\SECRET-PATH-5523\\x.wav";
    j["tags"][1]["startSec"] = 99999;                                                  // invalid
    std::string err;
    CHECK(!parse(j.dump(), nullptr, &err));
    CHECK(err.find("SECRET") == std::string::npos && !err.empty());
    // unknown version with a secret in an unknown field
    j["version"] = 9; j["zzz"] = "SECRET-UNKNOWN-1";
    CHECK(!parse(j.dump(), nullptr, &err) && err.find("SECRET") == std::string::npos);
    // the header contains no output calls at all (source scan)
    fs::path hdr = fs::path(__FILE__).parent_path().parent_path() / "src" / "core" / "analyser_project.hpp";
    std::ifstream in(hdr, std::ios::binary);
    if (in) {
        const std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        for (const char* banned : {"printf", "std::cout", "std::cerr", "std::clog", "puts(", "fputs", "OutputDebugString", "std::perror", "perror("}) {
            CHECK(src.find(banned) == std::string::npos);
        }
        CHECK(src.size() > 1000);
    } else {
        SKIP_LINUX("analyser_project.hpp not found from __FILE__ for the source scan");
    }
}

void testFuzz() {
    const std::string good = toJson(sample());
    std::mt19937 g(777);
    long accepted = 0, refused = 0;
    bool consistent = true;
    auto judge = [&](const std::string& t) {
        Project p;
        std::string err;
        if (parse(t, &p, &err)) {
            ++accepted;
            std::string v;
            consistent = consistent && validate(p, &v);
            Project q;
            consistent = consistent && parse(toJson(p), &q, &err) && same(p, q);     // an accepted file round-trips
        } else {
            ++refused;
            consistent = consistent && !err.empty();
        }
    };
    for (int i = 0; i < 30000; ++i) {
        std::string t = good;
        const int edits = 1 + static_cast<int>(g() % 4);
        for (int e = 0; e < edits && !t.empty(); ++e) {
            const std::size_t pos = g() % t.size();
            switch (g() % 6) {
                case 0: t[pos] = static_cast<char>(g()); break;
                case 1: t.erase(pos, 1 + g() % 8); break;
                case 2: t.insert(pos, 1, static_cast<char>(g())); break;
                case 3: t.resize(pos); break;
                case 4: { static const char* digs = "0123456789-.eE"; t[pos] = digs[g() % 14]; break; }
                default: { const std::size_t from = g() % t.size(); t.insert(pos, t.substr(from, 1 + g() % 40)); break; }
            }
        }
        judge(t);
    }
    for (int i = 0; i < 5000; ++i) {
        std::string t(g() % 400, '\0');
        for (char& c : t) { c = static_cast<char>(g()); }
        judge(t);
    }
    for (int i = 0; i < 2000; ++i) {                                                   // JSON-shaped garbage
        static const char* parts[] = {"{", "}", "[", "]", ",", ":", "\"version\"", "1", "\"tags\"", "\"format\"", "\"foxsdr-analyser-project\"", "null", "-0", "1e999", "\"x\""};
        std::string t;
        for (int k = 0; k < 40; ++k) { t += parts[g() % 15]; }
        judge(t);
    }
    std::printf("fuzz: 37000 inputs, %ld accepted (all valid, all round-trip), %ld refused\n", accepted, refused);
    CHECK(consistent);
    CHECK(refused > 20000);
}

}  // namespace

int main() {
    testSha();
    testFileHashAndReference();
    testRoundTrip();
    testRefusals();
    testTagsAndCrop();
    testCursors();
    testFiles();
    testPrivacy();
    testFuzz();
    return testSummary("test_analyser_project");
}
