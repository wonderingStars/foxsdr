// Tests for the recordings browser model (core/rec_browser.hpp).
//
// Every file is written here byte by byte (WAV, RF64, FLAC STREAMINFO, raw, SigMF JSON); expected values are worked out by hand. The 4 GB-class file is a
// sparse file (Windows). Hostile files: empty, junk, truncated, a directory with a recording name, an exclusively locked file.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/flac_writer.hpp"
#include "core/rec_browser.hpp"
#include "test_check.hpp"

#include <atomic>
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
namespace rb = cascade::core::recbrowse;
namespace cv = cascade::core::civil;
using Bytes = std::vector<unsigned char>;
fs::path g_dir;

void le16(Bytes& b, unsigned v) { b.push_back(v & 0xFF); b.push_back((v >> 8) & 0xFF); }
void le32(Bytes& b, std::uint32_t v) { for (int i = 0; i < 4; ++i) { b.push_back((v >> (8 * i)) & 0xFF); } }
void le64(Bytes& b, std::uint64_t v) { for (int i = 0; i < 8; ++i) { b.push_back(static_cast<unsigned char>((v >> (8 * i)) & 0xFF)); } }
void tag(Bytes& b, const char* s) { for (int i = 0; i < 4; ++i) { b.push_back(static_cast<unsigned char>(s[i])); } }

std::string put(const fs::path& p, const Bytes& b) {
    fs::create_directories(p.parent_path());
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    o.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    return p.string();
}
std::string putText(const fs::path& p, const std::string& t) { return put(p, Bytes(t.begin(), t.end())); }

Bytes wavBytes(unsigned tagv, unsigned ch, std::uint32_t rate, unsigned bits, std::size_t frames, std::uint32_t dataField = 0xFFFFFFFEu, std::size_t actualFrames = ~std::size_t{0}) {
    const unsigned align = ch * bits / 8;
    const std::size_t have = actualFrames == ~std::size_t{0} ? frames : actualFrames;
    Bytes f;
    tag(f, "RIFF"); le32(f, static_cast<std::uint32_t>(36 + frames * align)); tag(f, "WAVE");
    tag(f, "fmt "); le32(f, 16); le16(f, tagv); le16(f, ch); le32(f, rate); le32(f, rate * align); le16(f, align); le16(f, bits);
    tag(f, "data"); le32(f, dataField == 0xFFFFFFFEu ? static_cast<std::uint32_t>(frames * align) : dataField);
    f.resize(f.size() + have * align, 0);
    return f;
}

std::int64_t ms(int y, int mo, int d, int h, int mi, int s) { return cv::toUnixMs(y, mo, d, h, mi, s, 0); }

const rb::Entry* find(const std::vector<rb::Entry>& v, const std::string& name) {
    for (const rb::Entry& e : v) { if (e.fileName == name) { return &e; } }
    return nullptr;
}

std::vector<rb::Entry> scanBlocking(const std::string& folder, const rb::ScanOptions& opt = {}) {
    rb::RecordingsBrowser b;
    b.start(folder, opt);
    for (int i = 0; i < 6000; ++i) {
        const rb::Snapshot s = b.snapshot();
        if (s.done) { return *s.entries; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return {};
}

void testNameTimes() {
    struct Row { const char* name; bool found; std::int64_t unix; bool utc; };
    const Row rows[] = {
        {"SDRSharp_20260115_143059Z_7074000Hz_IQ.wav", true, ms(2026, 1, 15, 14, 30, 59), true},
        {"HDSDR_20260115_143059Z_7074kHz_RF.wav", true, ms(2026, 1, 15, 14, 30, 59), true},
        {"rec_20260115_143059.wav", true, ms(2026, 1, 15, 14, 30, 59), false},
        {"iq_20260115-143059.cu8", true, ms(2026, 1, 15, 14, 30, 59), false},
        {"20260115143059.wav", true, ms(2026, 1, 15, 14, 30, 59), false},
        {"2026-01-15_14-30-59.wav", true, ms(2026, 1, 15, 14, 30, 59), false},
        {"2026-01-15T14.30.59 fm.flac", true, ms(2026, 1, 15, 14, 30, 59), false},
        {"20240229_120000.wav", true, ms(2024, 2, 29, 12, 0, 0), false},         // a real leap day
        {"20230229_120000.wav", false, 0, false},                                  // not a day
        {"20261301_120000.wav", false, 0, false},                                  // month 13
        {"20260115_250000.wav", false, 0, false},                                  // hour 25
        {"20260115_146000.wav", false, 0, false},                                  // minute 60
        {"20260431_000000.wav", false, 0, false},                                  // April 31
        {"18991231_000000.wav", false, 0, false},                                  // year out of range
        {"capture_145.5MHz_2.4Msps.cu8", false, 0, false},
        {"12345678_123456.wav", false, 0, false},                                  // digits but not a date
        {"x120260115_143059.wav", false, 0, false},                                // the date does not start a digit run: "120260115..."
        {"", false, 0, false},
        {"2026", false, 0, false},
    };
    int n = 0;
    for (const Row& r : rows) {
        const rb::NameTime t = rb::parseNameTime(r.name);
        if (t.found != r.found || (t.found && (t.unixMs != r.unix || t.utc != r.utc))) { std::printf("  name '%s': found=%d unix=%lld utc=%d\n", r.name, t.found, static_cast<long long>(t.unixMs), t.utc); }
        CHECK(t.found == r.found);
        if (r.found) { CHECK(t.unixMs == r.unix && t.utc == r.utc); }
        ++n;
    }
    std::int64_t v = 0;
    CHECK(rb::parseIso8601Utc("2026-11-11T16:41:54.123Z", &v) && v == 1794415314123LL);
    CHECK(rb::parseIso8601Utc("2026-11-11T16:41:54Z", &v) && v == 1794415314000LL);
    CHECK(rb::parseIso8601Utc("2026-11-11T16:41:54.5Z", &v) && v == 1794415314500LL);
    CHECK(rb::parseIso8601Utc("2026-11-11T16:41:54.123456789Z", &v) && v == 1794415314123LL);
    CHECK(rb::parseIso8601Utc("2026-11-11T16:41:54", &v) && v == 1794415314000LL);          // no zone: SigMF is UTC
    CHECK(!rb::parseIso8601Utc("2026-11-11T16:41:54+01:00", &v));                          // a numeric offset is refused rather than guessed
    CHECK(!rb::parseIso8601Utc("2026-13-11T16:41:54Z", &v));
    CHECK(!rb::parseIso8601Utc("2026-11-11", &v));
    CHECK(!rb::parseIso8601Utc("", &v));
    CHECK(!rb::parseIso8601Utc("not a date at all, really", &v));
    std::printf("start times: %d file-name rows (compact, dashed, Z or local, leap day, impossible dates) and the SigMF ISO 8601 forms\n", n);
}

void testProbes() {
    const fs::path d = g_dir / "probe";
    fs::create_directories(d);
    // WAV rows
    put(d / "mono.wav", wavBytes(1, 1, 48000, 16, 96000));
    put(d / "music.wav", wavBytes(1, 2, 44100, 16, 44100 * 3));
    put(d / "iq_cap.wav", wavBytes(1, 2, 48000, 16, 48000));
    put(d / "SDRSharp_20260115_143059Z_7074000Hz_IQ.wav", wavBytes(3, 2, 2000000, 32, 20000));
    put(d / "wide.wav", wavBytes(1, 2, 96000, 24, 96000));
    put(d / "adpcm.wav", wavBytes(2, 1, 8000, 4, 10));
    put(d / "surround.wav", wavBytes(1, 6, 48000, 16, 100));
    put(d / "liar.wav", wavBytes(1, 1, 8000, 16, 100000, 200000, 300));        // declares 100000 frames, holds 300
    put(d / "empty.wav", Bytes{});
    put(d / "short.wav", Bytes{'R', 'I', 'F', 'F', 1, 2});
    putText(d / "junk.wav", std::string(5000, 'x'));
    { Bytes b = wavBytes(1, 2, 48000, 16, 10); b[20] = 0; b[21] = 0; /* format tag 0 */ put(d / "tag0.wav", b); }
    { Bytes b; tag(b, "RIFF"); le32(b, 100); tag(b, "WAVE"); tag(b, "fmt "); le32(b, 16); le16(b, 1); le16(b, 1); le32(b, 8000); le32(b, 16000); le16(b, 2); le16(b, 16); put(d / "nodata.wav", b); }
    { Bytes b; tag(b, "RIFF"); le32(b, 100); tag(b, "WAVE"); tag(b, "data"); le32(b, 8); b.resize(b.size() + 8, 0); put(d / "datafirst.wav", b); }
    fs::create_directories(d / "dir.wav");                                       // a directory with a recording name
    // raw
    put(d / "capture_145.5MHz_2.4Msps.cu8", Bytes(4800000, 128));
    put(d / "foo.cs16", Bytes(4000, 0));
    put(d / "bar.cf32", Bytes(8000, 0));
    // sigmf pair and an orphan data file
    put(d / "pair.sigmf-data", Bytes(8000, 0));
    putText(d / "pair.sigmf-meta", R"({"global":{"core:datatype":"ci16_le","core:sample_rate":250000,"core:version":"1.0.0"},"captures":[{"core:sample_start":0,"core:frequency":433920000,"core:datetime":"2026-11-11T16:41:54.123Z"}],"annotations":[]})");
    put(d / "orphan.sigmf-data", Bytes(100, 0));
    putText(d / "readme.txt", "not a recording");
    putText(d / "noext", "x");
    // FLAC from the writer: 1 s mono 16-bit 48 kHz; and a "crash" snapshot (total unknown)
    {
        cascade::core::flac::Options o; o.channels = 1; o.bitsPerSample = 16; o.sampleRate = 48000;
        cascade::core::flac::Writer w; std::string err;
        std::vector<std::int32_t> s(48000); for (int i = 0; i < 48000; ++i) { s[static_cast<std::size_t>(i)] = (i * 37) % 2000 - 1000; }
        w.open((d / "voice.flac").string(), o, &err); w.write(s.data(), s.size(), &err); w.finish(&err);
        cascade::core::flac::Writer w2;
        w2.open((d / "crashed.flac").string(), o, &err); w2.write(s.data(), 8192, &err);
        fs::copy_file(d / "crashed.flac", d / "crashed-snap.flac", fs::copy_options::overwrite_existing);
        w2.finish(&err);
        fs::remove(d / "crashed.flac");
        putText(d / "junk.flac", "fLaC" + std::string(60, '\0'));
    }

    rb::ScanOptions opt; opt.localUtcOffsetMinutes = 60;
    const std::vector<rb::Entry> v = scanBlocking(d.string(), opt);
    auto get = [&](const char* n) -> const rb::Entry& { static rb::Entry none; const rb::Entry* e = find(v, n); if (e == nullptr) { std::printf("  MISSING entry %s\n", n); CHECK(false); return none; } return *e; };

    const rb::Entry& mono = get("mono.wav");
    CHECK(!mono.broken && mono.kind == rb::Kind::Audio && !mono.kindGuessed && mono.sampleRateHz == 48000.0 && mono.totalFrames == 96000 && mono.durationKnown && mono.durationS == 2.0 && mono.channels == 1 && mono.bitsPerSample == 16 && mono.format == "pcm16");
    const rb::Entry& music = get("music.wav");
    CHECK(!music.broken && music.kind == rb::Kind::Audio && music.kindGuessed && music.sampleRateHz == 44100.0 && music.durationS == 3.0 && music.channels == 2);
    const rb::Entry& iqc = get("iq_cap.wav");
    CHECK(!iqc.broken && iqc.kind == rb::Kind::Iq && iqc.kindGuessed && iqc.totalFrames == 48000 && iqc.durationS == 1.0);
    const rb::Entry& sh = get("SDRSharp_20260115_143059Z_7074000Hz_IQ.wav");
    CHECK(!sh.broken && sh.kind == rb::Kind::Iq && sh.format == "float32" && sh.sampleRateHz == 2000000.0 && sh.durationS == 0.01);
    CHECK(sh.startSource == rb::StartSource::FileNameUtc && sh.startUnixMs == ms(2026, 1, 15, 14, 30, 59));
    const rb::Entry& wide = get("wide.wav");
    CHECK(!wide.broken && wide.kind == rb::Kind::Iq && wide.kindGuessed && wide.format == "pcm24");          // rate above 48 kHz: I/Q guess
    const rb::Entry& ad = get("adpcm.wav");
    CHECK(ad.broken && ad.error.find("unsupported WAVE format tag 2") != std::string::npos);
    const rb::Entry& sur = get("surround.wav");
    CHECK(!sur.broken && sur.kind == rb::Kind::Audio && sur.channels == 6);
    const rb::Entry& liar = get("liar.wav");
    CHECK(!liar.broken && liar.truncated && liar.totalFrames == 300);
    CHECK(get("empty.wav").broken && !get("empty.wav").error.empty());
    CHECK(get("short.wav").broken);
    CHECK(get("junk.wav").broken && get("junk.wav").error.find("not a RIFF") != std::string::npos);
    CHECK(get("tag0.wav").broken);
    CHECK(get("nodata.wav").broken && get("nodata.wav").error == "no data chunk");
    CHECK(get("datafirst.wav").broken);
    CHECK(find(v, "dir.wav") == nullptr);                                         // a directory is not a recording
    // raw
    const rb::Entry& cu8 = get("capture_145.5MHz_2.4Msps.cu8");
    CHECK(!cu8.broken && cu8.kind == rb::Kind::Iq && cu8.container == "raw" && cu8.sampleRateHz == 2400000.0 && cu8.centerHz == 145500000.0 && cu8.guessedFromName && cu8.totalFrames == 2400000 && cu8.durationS == 1.0);
    const rb::Entry& cs = get("foo.cs16");
    CHECK(!cs.broken && cs.kind == rb::Kind::Iq && !cs.sampleRateKnown && !cs.durationKnown && cs.totalFrames == 1000 && !cs.error.empty());
    const rb::Entry& cff = get("bar.cf32");
    CHECK(!cff.broken && cff.totalFrames == 1000 && !cff.sampleRateKnown);
    // sigmf
    const rb::Entry& sg = get("pair.sigmf-meta");
    CHECK(!sg.broken && sg.kind == rb::Kind::Iq && sg.container == "sigmf" && sg.sampleRateHz == 250000.0 && sg.centerKnown && sg.centerHz == 433920000.0 && sg.totalFrames == 2000);
    CHECK(sg.startSource == rb::StartSource::SigmfDatetime && sg.startUnixMs == 1794415314123LL);
    CHECK(sg.durationKnown && sg.durationS == 2000.0 / 250000.0);
    CHECK(find(v, "pair.sigmf-data") == nullptr);                                 // the data file is the same recording
    CHECK(get("orphan.sigmf-data").broken);
    CHECK(find(v, "readme.txt") == nullptr && find(v, "noext") == nullptr);
    // flac
    const rb::Entry& vf = get("voice.flac");
    CHECK(!vf.broken && vf.kind == rb::Kind::Audio && vf.container == "flac" && vf.sampleRateHz == 48000.0 && vf.channels == 1 && vf.bitsPerSample == 16 && vf.totalFrames == 48000 && vf.durationS == 1.0);
    const rb::Entry& cr = get("crashed-snap.flac");
    CHECK(!cr.broken && cr.totalFrames == 0 && !cr.durationKnown && cr.sampleRateHz == 48000.0);   // total unknown: shown without a length
    CHECK(get("junk.flac").broken);
    // start-time fallback: modified minus duration (the files were just written, so about "now")
    CHECK(mono.startSource == rb::StartSource::ModifiedMinusDuration);
    const std::int64_t nowMs = static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    CHECK(std::llabs(mono.startUnixMs + 2000 - nowMs) < 600000);   // mtime was within ten minutes of now
    // header sizes
    CHECK(mono.sizeBytes == 44 + 96000 * 2);
    std::printf("probes: %zu entries from one folder (WAV mono/stereo/float/24-bit/RF64-style sizes, ADPCM, junk, truncated, lying size, raw with and without a guessable name, SigMF pair and orphan, FLAC and a crash-snapshot FLAC)\n", v.size());

    // local-time names with an offset
    put(d / "local_20260115_143059.wav", wavBytes(1, 1, 8000, 16, 8000));
    const std::vector<rb::Entry> v2 = scanBlocking(d.string(), opt);
    const rb::Entry* l = find(v2, "local_20260115_143059.wav");
    CHECK(l != nullptr && l->startSource == rb::StartSource::FileNameLocal && l->startUnixMs == ms(2026, 1, 15, 13, 30, 59));   // UTC+1 -> 13:30:59Z
    // a SigMF datetime beats a name
    putText(d / "named_20200101_000000Z.sigmf-meta", R"({"global":{"core:datatype":"cf32_le","core:sample_rate":1000,"core:version":"1.0.0"},"captures":[{"core:sample_start":0,"core:datetime":"2026-11-11T16:41:54Z"}],"annotations":[]})");
    put(d / "named_20200101_000000Z.sigmf-data", Bytes(80, 0));
    const std::vector<rb::Entry> v3 = scanBlocking(d.string(), opt);
    const rb::Entry* nm = find(v3, "named_20200101_000000Z.sigmf-meta");
    CHECK(nm != nullptr && nm->startSource == rb::StartSource::SigmfDatetime && nm->startUnixMs == 1794415314000LL);

    // probeFile on a vanished and on a locked file never throws
    const rb::Entry gone = rb::probeFile(d / "vanished.wav", rb::ScanOptions{});
    CHECK(gone.broken && !gone.error.empty());
#if defined(_WIN32)
    {
        const std::string lp = put(d / "locked.wav", wavBytes(1, 1, 8000, 16, 100));
        HANDLE h = CreateFileA(lp.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);   // no sharing
        CHECK(h != INVALID_HANDLE_VALUE);
        const rb::Entry le = rb::probeFile(d / "locked.wav", rb::ScanOptions{});
        CHECK(le.broken && !le.error.empty());
        if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); }
        CHECK(!rb::probeFile(d / "locked.wav", rb::ScanOptions{}).broken);
    }
#endif
}

#if defined(_WIN32)
bool makeSparseWav(const std::string& path, const Bytes& header, std::uint64_t totalSize) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { return false; }
    DWORD ret = 0, w = 0;
    if (!DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &ret, nullptr)) { CloseHandle(h); return false; }
    WriteFile(h, header.data(), static_cast<DWORD>(header.size()), &w, nullptr);
    LARGE_INTEGER pos; pos.QuadPart = static_cast<LONGLONG>(totalSize - 4);
    SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
    const char z[4] = {0, 0, 0, 0};
    WriteFile(h, z, 4, &w, nullptr);
    CloseHandle(h);
    return true;
}
#endif

void testBigFile() {
#if defined(_WIN32)
    const fs::path d = g_dir / "big";
    fs::create_directories(d);
    const std::uint64_t frames = 1200000000ULL, bytes = frames * 4;
    Bytes ds; tag(ds, "ds64"); le32(ds, 28); le64(ds, bytes + 100); le64(ds, bytes); le64(ds, frames); le32(ds, 0);
    Bytes fmt; tag(fmt, "fmt "); le32(fmt, 16); le16(fmt, 1); le16(fmt, 2); le32(fmt, 2000000); le32(fmt, 8000000); le16(fmt, 4); le16(fmt, 16);
    Bytes head; tag(head, "RF64"); le32(head, 0xFFFFFFFFu); tag(head, "WAVE");
    head.insert(head.end(), ds.begin(), ds.end()); head.insert(head.end(), fmt.begin(), fmt.end()); tag(head, "data"); le32(head, 0xFFFFFFFFu);
    const std::string p = (d / "SDRuno_20260101_000000Z_big.wav").string();
    if (!makeSparseWav(p, head, head.size() + bytes)) { std::printf("SKIP: cannot make a sparse file here\n"); return; }
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<rb::Entry> v = scanBlocking(d.string());
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(v.size() == 1);
    if (v.size() == 1) {
        const rb::Entry& e = v[0];
        CHECK(!e.broken && e.container == "rf64" && e.totalFrames == frames && e.sampleRateHz == 2000000.0 && e.durationS == 600.0 && e.sizeBytes > 4800000000ULL && e.kind == rb::Kind::Iq);
        std::printf("big file: RF64 of %llu frames, %.2f GB: header read in %.3f s, duration %.1f s\n", static_cast<unsigned long long>(e.totalFrames), static_cast<double>(e.sizeBytes) / 1e9, s, e.durationS);
        CHECK(s < 2.0);
    }
    std::error_code ec; fs::remove(p, ec);
#else
    SKIP_LINUX("sparse > 4 GB file test is Windows-only");
#endif
}

rb::Entry mk(const std::string& name, rb::Kind k, double dur, double rate, double centre, std::int64_t start, std::uint64_t size, bool broken = false) {
    rb::Entry e; e.fileName = name; e.path = name; e.kind = k;
    if (dur > 0) { e.durationKnown = true; e.durationS = dur; }
    if (rate > 0) { e.sampleRateKnown = true; e.sampleRateHz = rate; }
    if (centre > 0) { e.centerKnown = true; e.centerHz = centre; }
    if (start != 0) { e.startSource = rb::StartSource::FileNameUtc; e.startUnixMs = start; }
    e.sizeBytes = size; e.broken = broken; e.modifiedUnixMs = static_cast<std::int64_t>(size) * 1000;
    return e;
}

void testSortFilter() {
    std::vector<rb::Entry> v = {
        mk("b.wav", rb::Kind::Audio, 10, 48000, 0, 3000, 500),
        mk("A.cu8", rb::Kind::Iq, 5, 2.4e6, 145.5e6, 1000, 900),
        mk("c.cs16", rb::Kind::Iq, 0, 0, 0, 0, 100),                   // everything unknown
        mk("d.flac", rb::Kind::Audio, 10, 44100, 0, 2000, 300),        // same duration as b.wav
        mk("E.sigmf-meta", rb::Kind::Iq, 60, 250e3, 433.92e6, 0, 700),
        mk("bad.wav", rb::Kind::Audio, 0, 0, 0, 0, 1, true),
    };
    auto names = [&](const std::vector<std::size_t>& ix) { std::string s; for (std::size_t i : ix) { s += v[i].fileName + " "; } return s; };
    rb::Filter all;
    CHECK(names(rb::sortAndFilter(v, all, rb::SortKey::Name, true)) == "A.cu8 b.wav bad.wav c.cs16 d.flac E.sigmf-meta ");     // case-insensitive
    CHECK(names(rb::sortAndFilter(v, all, rb::SortKey::Name, false)) == "E.sigmf-meta d.flac c.cs16 bad.wav b.wav A.cu8 ");
    CHECK(names(rb::sortAndFilter(v, all, rb::SortKey::Start, true)) == "A.cu8 d.flac b.wav c.cs16 E.sigmf-meta bad.wav ");   // unknown last, in original order
    CHECK(names(rb::sortAndFilter(v, all, rb::SortKey::Start, false)) == "b.wav d.flac A.cu8 c.cs16 E.sigmf-meta bad.wav ");   // unknown STILL last
    CHECK(names(rb::sortAndFilter(v, all, rb::SortKey::Duration, true)) == "A.cu8 b.wav d.flac E.sigmf-meta c.cs16 bad.wav ");   // ties keep order (stable)
    CHECK(names(rb::sortAndFilter(v, all, rb::SortKey::Duration, false)) == "E.sigmf-meta b.wav d.flac A.cu8 c.cs16 bad.wav ");   // ties keep order in descending too
    CHECK(names(rb::sortAndFilter(v, all, rb::SortKey::Size, true)) == "bad.wav c.cs16 d.flac b.wav E.sigmf-meta A.cu8 ");
    CHECK(names(rb::sortAndFilter(v, all, rb::SortKey::Rate, true)) == "d.flac b.wav E.sigmf-meta A.cu8 c.cs16 bad.wav " || names(rb::sortAndFilter(v, all, rb::SortKey::Rate, true)) == "b.wav d.flac E.sigmf-meta A.cu8 c.cs16 bad.wav ");
    CHECK(names(rb::sortAndFilter(v, all, rb::SortKey::Center, true)) == "A.cu8 E.sigmf-meta b.wav c.cs16 d.flac bad.wav ");
    CHECK(names(rb::sortAndFilter(v, all, rb::SortKey::Modified, false)) == "A.cu8 E.sigmf-meta b.wav d.flac c.cs16 bad.wav ");
    // filters
    rb::Filter f;
    f.text = "WAV"; CHECK(names(rb::sortAndFilter(v, f, rb::SortKey::Name, true)) == "b.wav bad.wav ");
    f = rb::Filter{}; f.iq = false; CHECK(names(rb::sortAndFilter(v, f, rb::SortKey::Name, true)) == "b.wav bad.wav d.flac ");
    f = rb::Filter{}; f.audio = false; CHECK(names(rb::sortAndFilter(v, f, rb::SortKey::Name, true)) == "A.cu8 c.cs16 E.sigmf-meta ");
    f = rb::Filter{}; f.iq = false; f.audio = false; CHECK(rb::sortAndFilter(v, f, rb::SortKey::Name, true).empty());
    f = rb::Filter{}; f.hideBroken = true; CHECK(rb::sortAndFilter(v, f, rb::SortKey::Name, true).size() == 5);
    f = rb::Filter{}; f.useCenter = true; f.centerMinHz = 100e6; f.centerMaxHz = 200e6; CHECK(names(rb::sortAndFilter(v, f, rb::SortKey::Name, true)) == "A.cu8 ");
    f.centerMinHz = 145.5e6; f.centerMaxHz = 145.5e6; CHECK(names(rb::sortAndFilter(v, f, rb::SortKey::Name, true)) == "A.cu8 ");   // inclusive both ends
    f.centerMinHz = 1; f.centerMaxHz = 2; CHECK(rb::sortAndFilter(v, f, rb::SortKey::Name, true).empty());
    f = rb::Filter{}; f.useDuration = true; f.durationMinS = 10; f.durationMaxS = 60; CHECK(names(rb::sortAndFilter(v, f, rb::SortKey::Name, true)) == "b.wav d.flac E.sigmf-meta ");
    f = rb::Filter{}; f.useStart = true; f.startFromUnixMs = 1500; f.startToUnixMs = 2500; CHECK(names(rb::sortAndFilter(v, f, rb::SortKey::Name, true)) == "d.flac ");
    f.startFromUnixMs = 1000; f.startToUnixMs = 3000; CHECK(names(rb::sortAndFilter(v, f, rb::SortKey::Start, true)) == "A.cu8 d.flac b.wav ");
    // combined, and an empty list
    f = rb::Filter{}; f.text = "c"; f.iq = true; f.audio = false; CHECK(names(rb::sortAndFilter(v, f, rb::SortKey::Name, true)) == "A.cu8 c.cs16 ");   // text AND kind together
    CHECK(rb::sortAndFilter({}, all, rb::SortKey::Name, true).empty());
    std::printf("sort/filter: 8 sort orders in both directions (unknown last either way, ties stable), text, kind, centre range, duration range, start range, broken, combined\n");
}

void testScanManyAndNonBlocking() {
    const fs::path d = g_dir / "many";
    const int kGood = 4000, kBroken = 400;
    std::mt19937 g(1);
    for (int i = 0; i < kGood; ++i) {
        const fs::path sub = d / ("s" + std::to_string(i % 40)) / ("t" + std::to_string(i % 3));
        const char* ext = (i % 4 == 0) ? ".wav" : (i % 4 == 1 ? ".cu8" : (i % 4 == 2 ? ".cf32" : ".wav"));
        const std::string name = "rec_" + std::to_string(i) + "_145.5MHz_2.4Msps" + ext;
        if (std::string(ext) == ".wav") { put(sub / name, wavBytes(1, (i % 8 == 3) ? 2u : 1u, 48000, 16, 100 + static_cast<std::size_t>(i % 50))); }
        else { put(sub / name, Bytes(static_cast<std::size_t>(200 + i % 100) * 2, 0)); }
    }
    for (int i = 0; i < kBroken; ++i) {
        Bytes b(static_cast<std::size_t>(g() % 200)); for (auto& x : b) { x = static_cast<unsigned char>(g()); }
        put(d / "bad" / ("junk_" + std::to_string(i) + ".wav"), b);
    }
    rb::RecordingsBrowser br;
    rb::ScanOptions opt;
    const auto t0 = std::chrono::steady_clock::now();
    br.start(d.string(), opt);
    double maxSnapMs = 0.0;
    std::size_t maxSeen = 0;
    int partial = 0, polls = 0;
    std::uint64_t id = 0;
    for (;;) {
        const auto a = std::chrono::steady_clock::now();
        const rb::Snapshot s = br.snapshot();
        const double dt = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
        maxSnapMs = std::max(maxSnapMs, dt);
        ++polls;
        id = s.scanId;
        if (!s.done && s.entries->size() > 0) { ++partial; }
        maxSeen = std::max(maxSeen, s.entries->size());
        if (s.done) { break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const rb::Snapshot f = br.snapshot();
    CHECK(f.done && !f.cancelled && f.folderError.empty());
    CHECK(f.entries->size() == static_cast<std::size_t>(kGood + kBroken));
    CHECK(f.candidates == static_cast<std::size_t>(kGood + kBroken));
    CHECK(f.probed == f.entries->size());
    CHECK(f.broken >= static_cast<std::size_t>(kBroken * 8 / 10));   // random junk is almost always refused (an empty or odd file may also be refused)
    CHECK(id == 1);
    CHECK(partial > 0);                                               // partial results were published while the scan ran
    CHECK(maxSnapMs < 50.0);                                          // the caller never waited on I/O
    std::size_t good = 0; for (const rb::Entry& e : *f.entries) { if (!e.broken) { ++good; } }
    CHECK(good >= static_cast<std::size_t>(kGood));
    std::printf("scan of %d files in 40x3 sub-folders (%d of them junk): %.2f s, %zu broken entries, %d partial snapshots seen, slowest snapshot() call %.3f ms\n", kGood + kBroken, kBroken, total, f.broken, partial, maxSnapMs);
    // sorting 4400 entries
    const auto s0 = std::chrono::steady_clock::now();
    const auto ix = rb::sortAndFilter(*f.entries, rb::Filter{}, rb::SortKey::Duration, false);
    const double sortMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s0).count();
    CHECK(ix.size() == f.entries->size());
    std::printf("  sorting those %zu entries by duration took %.2f ms\n", ix.size(), sortMs);
    CHECK(sortMs < 100.0);

    // depth: maxDepth 0 sees only the folder itself (nothing here), 1 sees s*/ only for files directly in s*
    rb::ScanOptions o0; o0.maxDepth = 0;
    CHECK(scanBlocking(d.string(), o0).empty());
    rb::ScanOptions o1; o1.maxDepth = 1;
    CHECK(scanBlocking(d.string(), o1).size() == static_cast<std::size_t>(kBroken));   // files are two levels down (s*/t*): only bad/ (depth 1) is seen
    // (bad/ is at depth 1 and holds the junk files)
    // the entry cap
    rb::ScanOptions cap; cap.maxEntries = 100;
    rb::RecordingsBrowser b2; b2.start(d.string(), cap);
    for (int i = 0; i < 3000; ++i) { if (b2.snapshot().done) { break; } std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    const rb::Snapshot s2 = b2.snapshot();
    CHECK(s2.done && s2.entries->size() == 100 && s2.listTruncated);
    // a folder that is not there
    rb::RecordingsBrowser b3; b3.start((g_dir / "no-such-folder").string());
    for (int i = 0; i < 500; ++i) { if (b3.snapshot().done) { break; } std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    const rb::Snapshot s3 = b3.snapshot();
    CHECK(s3.done && s3.entries->empty() && !s3.folderError.empty());
    // a file where a folder is expected
    putText(g_dir / "afile.txt", "x");
    rb::RecordingsBrowser b4; b4.start((g_dir / "afile.txt").string());
    for (int i = 0; i < 500; ++i) { if (b4.snapshot().done) { break; } std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    CHECK(b4.snapshot().done && !b4.snapshot().folderError.empty());
    // empty folder
    fs::create_directories(g_dir / "emptydir");
    CHECK(scanBlocking((g_dir / "emptydir").string()).empty());

    // CANCEL: a new scan of the big folder cancelled at once returns quickly and says cancelled
    rb::RecordingsBrowser b5;
    b5.start(d.string());
    const auto c0 = std::chrono::steady_clock::now();
    b5.cancel();
    const double cancelMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
    const rb::Snapshot s5 = b5.snapshot();
    CHECK(!b5.running());
    CHECK(s5.cancelled || s5.done);
    CHECK(cancelMs < 1500.0);
    std::printf("  cancel right after start returned in %.1f ms (cancelled=%d, %zu entries so far)\n", cancelMs, s5.cancelled, s5.entries->size());
    // RESTART: starting a second scan while one runs replaces it (scan id increments) and the old result does not leak into the new one
    rb::RecordingsBrowser b6;
    b6.start(d.string());
    b6.start((g_dir / "probe").string());
    for (int i = 0; i < 3000; ++i) { if (b6.snapshot().done) { break; } std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    const rb::Snapshot s6 = b6.snapshot();
    CHECK(s6.done && s6.scanId == 2);
    bool onlyProbe = true; for (const rb::Entry& e : *s6.entries) { if (e.path.find("probe") == std::string::npos) { onlyProbe = false; } }
    CHECK(onlyProbe && !s6.entries->empty());
    // destroying a running browser joins cleanly (no crash, no hang)
    { rb::RecordingsBrowser b7; b7.start(d.string()); }
    CHECK(true);
}

void testSymlinks() {
#if defined(_WIN32)
    const fs::path d = g_dir / "links";
    fs::create_directories(d / "real");
    put(d / "real" / "a.wav", wavBytes(1, 1, 8000, 16, 100));
    std::error_code ec;
    fs::create_directory_symlink(d / "real", d / "loop", ec);   // a link to a folder: needs a privilege on some Windows setups
    if (ec) {
        // a junction (mklink /J) needs no privilege and is the same kind of loop for a directory walker
        auto q = [](const fs::path& x) { return std::string(1, 34) + x.string() + std::string(1, 34); };
        const std::string cmd = "cmd /c " + std::string(1, 34) + "mklink /J " + q(d / "loop") + " " + q(d / "real") + " > NUL 2>&1 && mklink /J " + q(d / "real" / "up") + " " + q(d) + " > NUL 2>&1" + std::string(1, 34);
        const int rc = std::system(cmd.c_str());
        CHECK(rc == 0);
        const std::vector<rb::Entry> vj = scanBlocking(d.string());
        CHECK(vj.size() == 1);
        std::printf("junctions: a folder junction and a cycle are not followed (1 entry, scan finished)"); std::printf("%c", 10);
        return;
    }
    fs::create_directory_symlink(d, d / "real" / "up", ec);      // a cycle: real/up -> links
    const std::vector<rb::Entry> v = scanBlocking(d.string());
    CHECK(v.size() == 1);   // listed once: links are not followed, so no duplicate and no endless walk
    std::printf("symlinks: a folder link and a cycle are not followed (1 entry)\n");
#endif
}

}  // namespace

int main() {
    g_dir = fs::temp_directory_path() / "recm-browser";
    std::error_code ec0;
    fs::remove_all(g_dir, ec0);
    fs::create_directories(g_dir);
    testNameTimes();
    testProbes();
    testBigFile();
    testSortFilter();
    testScanManyAndNonBlocking();
    testSymlinks();
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    return testSummary("test_rec_browser");
}
