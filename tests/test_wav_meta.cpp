// WAV header metadata (source/wav_meta.hpp, row P16): synthetic files with the auxi chunk in every position, hostile and truncated headers, a bounded-read
// count, a fuzz run, and a survey of the real third-party recordings under D:/foxsdr-samples when they are present (counts and values are printed, not committed).
#include <cctype>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "source/wav_meta.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using namespace cascade::source::wavmeta;

namespace {

using Bytes = std::vector<unsigned char>;

void put16(Bytes& b, unsigned v) { b.push_back(static_cast<unsigned char>(v)); b.push_back(static_cast<unsigned char>(v >> 8)); }
void put32(Bytes& b, std::uint32_t v) { for (int i = 0; i < 4; ++i) { b.push_back(static_cast<unsigned char>(v >> (8 * i))); } }
void put64(Bytes& b, std::uint64_t v) { for (int i = 0; i < 8; ++i) { b.push_back(static_cast<unsigned char>(v >> (8 * i))); } }
void putId(Bytes& b, const char* id) { b.insert(b.end(), id, id + 4); }

Bytes fmtChunk(std::uint32_t rate, unsigned ch = 2, unsigned bits = 16, unsigned tag = 1) {
    Bytes b; putId(b, "fmt "); put32(b, 16); put16(b, tag); put16(b, ch); put32(b, rate); put32(b, rate * ch * bits / 8); put16(b, ch * bits / 8); put16(b, bits);
    return b;
}

void putSystemTime(Bytes& b, int y, int mo, int dow, int d, int h, int mi, int s, int ms) {
    for (int v : {y, mo, dow, d, h, mi, s, ms}) { put16(b, static_cast<unsigned>(v)); }
}

// the layout read from the real files: 2 x SYSTEMTIME, centre, AD, IF, bandwidth, IQ offset, four more words, 96 bytes of name = 164
Bytes auxiChunk(std::uint32_t center, std::uint32_t ad = 2400000, std::size_t payload = 164) {
    Bytes b; putId(b, "auxi"); put32(b, static_cast<std::uint32_t>(payload));
    Bytes p;
    putSystemTime(p, 2021, 11, 5, 26, 15, 3, 30, 574);
    putSystemTime(p, 0, 0, 0, 0, 0, 0, 0, 0);
    put32(p, center); put32(p, ad); put32(p, 0); put32(p, 0); put32(p, 0);
    while (p.size() < payload) { p.push_back(0); }
    p.resize(payload);
    b.insert(b.end(), p.begin(), p.end());
    if (payload & 1) { b.push_back(0); }
    return b;
}

Bytes dataChunk(std::size_t bytes, bool placeholder = false) {
    Bytes b; putId(b, "data"); put32(b, placeholder ? 0xFFFFFFFFu : static_cast<std::uint32_t>(bytes));
    b.resize(b.size() + bytes, 0x11);
    return b;
}

Bytes wav(const std::vector<Bytes>& chunks, const char* id = "RIFF") {
    Bytes b; putId(b, id); put32(b, 0); putId(b, "WAVE");
    for (const Bytes& c : chunks) { b.insert(b.end(), c.begin(), c.end()); }
    const std::uint32_t sz = static_cast<std::uint32_t>(b.size() - 8);
    for (int i = 0; i < 4; ++i) { b[4 + static_cast<std::size_t>(i)] = static_cast<unsigned char>(sz >> (8 * i)); }
    return b;
}

long long unixMs(int y, unsigned mo, unsigned d, int h, int mi, int s, int ms) {
    using namespace std::chrono;
    const sys_days sd = year{y} / month{mo} / day{d};
    return duration_cast<milliseconds>(sd.time_since_epoch()).count() + ((h * 60LL + mi) * 60LL + s) * 1000LL + ms;
}

void testSynthetic() {
    // fmt, auxi, data (the order of the real files)
    {
        const Bytes f = wav({fmtChunk(2400000), auxiChunk(1090000000u), dataChunk(4000)});
        const WavMeta m = readWavMeta(f.data(), f.size());
        CHECK(m.ok && m.container == "RIFF" && m.channels == 2 && m.sampleRateHz == 2400000 && m.bitsPerSample == 16 && m.formatTag == 1);
        CHECK(m.auxi.present && m.auxi.hasCenter && m.auxi.centerHz == 1090000000u && m.auxi.chunkSize == 164 && m.auxi.adFrequencyHz == 2400000);
        CHECK(m.centerKnown && m.centerHz == 1.09e9 && !m.centerSource.empty());
        CHECK(m.startKnown && m.startUnixMs == unixMs(2021, 11, 26, 15, 3, 30, 574));
        CHECK(m.auxi.start.valid && !m.auxi.stop.valid);                                    // the all-zero stop time is not a date
        CHECK(m.hasData && m.dataBytes == 4000 && m.dataOffset == f.size() - 4000);
        CHECK(m.chunks.size() == 3 && std::string(m.chunks[1].id) == "auxi" && m.chunks[1].size == 164);
    }
    // auxi after fmt but with other chunks around it, and one AFTER the data
    {
        Bytes list; putId(list, "LIST"); put32(list, 5); for (int i = 0; i < 5; ++i) { list.push_back('x'); } list.push_back(0);   // odd size, padded
        const Bytes f = wav({list, fmtChunk(48000), dataChunk(100), auxiChunk(7100000u)});
        const WavMeta m = readWavMeta(f.data(), f.size());
        CHECK(m.ok && m.centerKnown && m.centerHz == 7100000.0 && m.chunks.size() == 4);
    }
    // centre zero = unknown
    {
        const Bytes f = wav({fmtChunk(48000), auxiChunk(0), dataChunk(10)});
        const WavMeta m = readWavMeta(f.data(), f.size());
        CHECK(m.ok && m.auxi.present && !m.auxi.hasCenter && !m.centerKnown && m.centerSource.empty());
    }
    // a short auxi: 36 bytes still holds the centre, 35 does not, 16 holds a start time only
    for (std::size_t payload : {36u, 35u, 20u, 16u, 0u}) {
        const Bytes f = wav({fmtChunk(48000), auxiChunk(144800000u, 48000, payload), dataChunk(10)});
        const WavMeta m = readWavMeta(f.data(), f.size());
        CHECK(m.ok && m.auxi.present);
        CHECK(m.centerKnown == (payload >= 36));
        CHECK(m.startKnown == (payload >= 16));
    }
    // two auxi chunks: the first wins
    {
        const Bytes f = wav({fmtChunk(48000), auxiChunk(1000000u), auxiChunk(2000000u), dataChunk(10)});
        CHECK(readWavMeta(f.data(), f.size()).centerHz == 1000000.0);
    }
    // no auxi at all
    {
        const Bytes f = wav({fmtChunk(48000), dataChunk(10)});
        const WavMeta m = readWavMeta(f.data(), f.size());
        CHECK(m.ok && !m.auxi.present && !m.centerKnown && !m.startKnown);
    }
    // an impossible start date is reported but not used
    {
        Bytes a; putId(a, "auxi"); put32(a, 164);
        Bytes p; putSystemTime(p, 2021, 13, 0, 32, 25, 61, 61, 1000); putSystemTime(p, 0, 0, 0, 0, 0, 0, 0, 0);
        put32(p, 7100000); p.resize(164);
        a.insert(a.end(), p.begin(), p.end());
        const Bytes f = wav({fmtChunk(48000), a, dataChunk(10)});
        const WavMeta m = readWavMeta(f.data(), f.size());
        CHECK(m.ok && m.centerKnown && !m.startKnown && !m.auxi.start.valid);
    }
    // leap day and the end of a month are accepted, the 30th of February is not
    for (auto row : std::vector<std::vector<int>>{{2024, 2, 29, 1}, {2023, 2, 29, 0}, {2100, 2, 29, 0}, {2000, 2, 29, 1}, {2021, 4, 30, 1}, {2021, 4, 31, 0}}) {
        Bytes a; putId(a, "auxi"); put32(a, 164);
        Bytes p; putSystemTime(p, row[0], row[1], 0, row[2], 0, 0, 0, 0); putSystemTime(p, 0, 0, 0, 0, 0, 0, 0, 0); put32(p, 1); p.resize(164);
        a.insert(a.end(), p.begin(), p.end());
        const Bytes f = wav({fmtChunk(48000), a, dataChunk(2)});
        CHECK(readWavMeta(f.data(), f.size()).startKnown == (row[3] == 1));
    }
    // RF64 with ds64 and a data chunk of 0xFFFFFFFF
    {
        Bytes ds; putId(ds, "ds64"); put32(ds, 28); put64(ds, 100); put64(ds, 4000); put64(ds, 1000); put32(ds, 0);
        const Bytes f = wav({ds, fmtChunk(2048000), auxiChunk(225648000u, 2048000), dataChunk(4000, true)}, "RF64");
        const WavMeta m = readWavMeta(f.data(), f.size());
        CHECK(m.ok && m.container == "RF64" && m.hasData && m.dataBytes == 4000 && m.centerHz == 225648000.0);
    }
    // a streaming placeholder with no ds64: the data size is not trusted and the walk stops
    {
        const Bytes f = wav({fmtChunk(48000), auxiChunk(5000000u), dataChunk(500, true)});
        const WavMeta m = readWavMeta(f.data(), f.size());
        CHECK(m.ok && m.hasData && m.dataBytes == 0xFFFFFFFFull && m.centerHz == 5000000.0);
    }
    // the FREQ chunk candidate: used only when its second value equals the fmt sample rate, and never as the centre
    {
        auto freqChunk = [](std::uint64_t a, std::uint64_t b) { Bytes c; putId(c, "FREQ"); put32(c, 40); put64(c, a); put64(c, b); for (int i = 0; i < 24; ++i) { c.push_back(0); } return c; };
        const Bytes ok = wav({fmtChunk(135498, 2, 32, 3), freqChunk(131857223, 135498), dataChunk(16)});
        WavMeta m = readWavMeta(ok.data(), ok.size());
        CHECK(m.ok && m.hasFreqChunkCandidate && m.freqChunkHz == 131857223.0 && !m.centerKnown);
        const Bytes wrongRate = wav({fmtChunk(48000, 2, 32, 3), freqChunk(131857223, 135498), dataChunk(16)});
        m = readWavMeta(wrongRate.data(), wrongRate.size());
        CHECK(m.ok && !m.hasFreqChunkCandidate);
        const Bytes zero = wav({fmtChunk(135498, 2, 32, 3), freqChunk(0, 135498), dataChunk(16)});
        CHECK(!readWavMeta(zero.data(), zero.size()).hasFreqChunkCandidate);
        const Bytes huge = wav({fmtChunk(135498, 2, 32, 3), freqChunk(2000000000000ull, 135498), dataChunk(16)});
        CHECK(!readWavMeta(huge.data(), huge.size()).hasFreqChunkCandidate);
        Bytes shortChunk; putId(shortChunk, "FREQ"); put32(shortChunk, 12); for (int i = 0; i < 12; ++i) { shortChunk.push_back(1); }
        const Bytes sh = wav({fmtChunk(135498, 2, 32, 3), shortChunk, dataChunk(16)});
        CHECK(!readWavMeta(sh.data(), sh.size()).hasFreqChunkCandidate);
        // when both exist the auxi centre is the centre
        const Bytes both = wav({fmtChunk(135498, 2, 32, 3), freqChunk(131857223, 135498), auxiChunk(100000000u, 135498), dataChunk(16)});
        m = readWavMeta(both.data(), both.size());
        CHECK(m.centerKnown && m.centerHz == 1e8 && m.hasFreqChunkCandidate);
    }
    // not WAV
    { const Bytes f = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'A', 'V', 'I', ' '}; CHECK(!readWavMeta(f.data(), f.size()).ok); }
    { const Bytes f = {'O', 'g', 'g', 'S', 0, 0, 0, 0, 'W', 'A', 'V', 'E'}; CHECK(!readWavMeta(f.data(), f.size()).ok); }
    { const Bytes f(11, 0); const WavMeta m = readWavMeta(f.data(), f.size()); CHECK(!m.ok && !m.error.empty()); }
    { const WavMeta m = readWavMeta(nullptr, 0); CHECK(!m.ok); }
    { const Bytes f = wav({}); CHECK(!readWavMeta(f.data(), f.size()).ok); }
}

class CountingSource final : public ByteSource {
public:
    CountingSource(const Bytes& b) : b_(b) {}
    std::uint64_t size() const override { return b_.size(); }
    std::size_t readAt(std::uint64_t off, unsigned char* dst, std::size_t n) override {
        ++reads; bytes += n;
        if (off >= b_.size()) { return 0; }
        const std::size_t k = std::min<std::size_t>(n, b_.size() - static_cast<std::size_t>(off));
        std::memcpy(dst, b_.data() + off, k);
        return k;
    }
    const Bytes& b_;
    long reads = 0; std::size_t bytes = 0;
};

void testBounds() {
    // 100000 zero-size chunks: only 256 headers are walked
    {
        std::vector<Bytes> cs{fmtChunk(48000)};
        for (int i = 0; i < 100000; ++i) { Bytes z; putId(z, "JUNK"); put32(z, 0); cs.push_back(z); }
        cs.push_back(auxiChunk(9000000u)); cs.push_back(dataChunk(10));
        const Bytes f = wav(cs);
        CountingSource src(f);
        const WavMeta m = readWavMeta(src);
        CHECK(m.ok && m.chunks.size() == kMaxChunks);
        CHECK(!m.centerKnown);                                                     // the auxi chunk lies beyond the walk limit
        CHECK(src.reads <= static_cast<long>(kMaxChunks) * 2 + 4);
        std::printf("bounds: %zu chunks walked, %ld reads, %zu bytes read from a %zu-byte file\n", m.chunks.size(), src.reads, src.bytes, f.size());
        CHECK(src.bytes < 8000);
    }
    // a lying chunk size (4 GB) in front of the auxi chunk: nothing is read past the file, the result is a clean stop
    {
        Bytes liar; putId(liar, "LIST"); put32(liar, 0xFFFFFFF0u);
        const Bytes f = wav({fmtChunk(48000), liar, auxiChunk(1u), dataChunk(10)});
        CountingSource src(f);
        const WavMeta m = readWavMeta(src);
        CHECK(m.ok && m.chunks.size() == 2 && !m.centerKnown);
    }
    // a data chunk whose size runs past the file: still listed, walk ends
    {
        Bytes f = wav({fmtChunk(48000), auxiChunk(144000000u), dataChunk(100)});
        f.resize(f.size() - 40);
        const WavMeta m = readWavMeta(f.data(), f.size());
        CHECK(m.ok && m.centerKnown && m.hasData);
    }
    // every truncation of a valid file reads without incident and never invents a centre it did not see
    {
        const Bytes f = wav({fmtChunk(48000), auxiChunk(144800000u), dataChunk(64)});
        int okc = 0, centre = 0;
        for (std::size_t n = 0; n <= f.size(); ++n) {
            const WavMeta m = readWavMeta(f.data(), n);
            if (m.ok) { ++okc; }
            if (m.centerKnown) { ++centre; CHECK(m.centerHz == 144800000.0); }
        }
        CHECK(okc > 0 && centre > 0);
        std::printf("truncations: %d of %zu readable, %d with a centre\n", okc, f.size() + 1, centre);
    }
}

void testFuzz() {
    const Bytes good = wav({fmtChunk(2400000), auxiChunk(1090000000u), dataChunk(300)});
    std::mt19937 g(808);
    long okc = 0, centre = 0;
    bool sane = true;
    for (int i = 0; i < 100000; ++i) {
        Bytes f = good;
        const int edits = 1 + static_cast<int>(g() % 5);
        for (int e = 0; e < edits; ++e) {
            const std::size_t pos = g() % f.size();
            switch (g() % 4) {
                case 0: f[pos] = static_cast<unsigned char>(g()); break;
                case 1: f.erase(f.begin() + static_cast<std::ptrdiff_t>(pos), f.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(f.size(), pos + 1 + g() % 6))); if (f.empty()) { f.push_back(0); } break;
                case 2: f.insert(f.begin() + static_cast<std::ptrdiff_t>(pos), static_cast<unsigned char>(g())); break;
                default: f.resize(1 + pos); break;
            }
        }
        const WavMeta m = readWavMeta(f.data(), f.size());
        if (m.ok) { ++okc; sane = sane && !m.chunks.empty() && m.chunks.size() <= kMaxChunks; }
        else { sane = sane && !m.error.empty() && !m.centerKnown; }
        if (m.centerKnown) { ++centre; sane = sane && m.centerHz > 0.0 && m.centerHz <= 4294967295.0; }
        for (const ChunkInfo& c : m.chunks) { for (int k = 0; k < 4; ++k) { sane = sane && static_cast<unsigned char>(c.id[k]) >= 0x20 && static_cast<unsigned char>(c.id[k]) <= 0x7E; } }
    }
    for (int i = 0; i < 20000; ++i) {                                              // random bytes behind a valid RIFF/WAVE start
        Bytes f(12 + g() % 600);
        for (auto& b : f) { b = static_cast<unsigned char>(g()); }
        std::memcpy(f.data(), "RIFF", 4); std::memcpy(f.data() + 8, "WAVE", 4);
        const WavMeta m = readWavMeta(f.data(), f.size());
        sane = sane && (m.ok || !m.error.empty());
    }
    std::printf("fuzz: 120000 headers, %ld readable, %ld with a centre\n", okc, centre);
    CHECK(sane);
    CHECK(okc > 20000 && centre > 1000);
}

void testRealSamples() {
    const fs::path root = "D:/foxsdr-samples";
    std::error_code ec;
    if (!fs::is_directory(root, ec)) { SKIP_LINUX("D:/foxsdr-samples is not present"); return; }
    long wavs = 0, withAuxi = 0, withCentre = 0, parseFail = 0, withCandidate = 0;
    std::map<std::string, long> ids;
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { break; }
        if (!it->is_regular_file(ec)) { continue; }
        std::string ext = it->path().extension().string();
        for (char& c : ext) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
        if (ext != ".wav") { continue; }
        if (it->path().string().find("_tools") != std::string::npos) { continue; }
        const WavMeta m = readWavMeta(it->path());
        if (!m.ok) { ++parseFail; continue; }                                       // e.g. the one file that is not RIFF
        ++wavs;
        for (const ChunkInfo& c : m.chunks) { ++ids[c.id]; }
        if (m.auxi.present) { ++withAuxi; }
        if (m.hasFreqChunkCandidate) { ++withCandidate; std::printf("  FREQ-chunk candidate: %.0f Hz (rate %u Hz)\n", m.freqChunkHz, m.sampleRateHz); }
        if (m.centerKnown) {
            ++withCentre;
            std::printf("  centre from the header: %.0f Hz, rate %u Hz, start %s, auxi ADFrequency %u, folder %s\n", m.centerHz, m.sampleRateHz, m.startKnown ? "known" : "unknown",
                        m.auxi.adFrequencyHz, it->path().parent_path().filename().string().c_str());
            // the three known files: centre equals what their folders say they are
            const std::string name = it->path().filename().string();
            if (name == "adsb.2021-11-26T15_03_30_573.wav") { CHECK(m.centerHz == 1090000000.0 && m.startUnixMs == unixMs(2021, 11, 26, 15, 3, 30, 574)); }
            if (name == "apt.2021-11-27T10_42_34_142.wav") { CHECK(m.centerHz == 137912000.0); }
            if (name == "dab.2021-12-16T14_26_44_664.wav") { CHECK(m.centerHz == 225648000.0); }
        }
    }
    std::printf("real samples: %ld readable WAV files, %ld not RIFF/unreadable, %ld carry an auxi chunk, %ld give a centre, %ld have a FREQ-chunk candidate; chunk ids:", wavs, parseFail, withAuxi, withCentre, withCandidate);
    for (const auto& kv : ids) { std::printf(" %s=%ld", kv.first.c_str(), kv.second); }
    std::printf("\n");
    CHECK(wavs > 0);
    CHECK(withCentre == withAuxi);
}

}  // namespace

int main() {
    testSynthetic();
    testBounds();
    testFuzz();
    testRealSamples();
    return testSummary("test_wav_meta");
}
