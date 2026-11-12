// decode_db - an append-only store of decode events with search and retention, in an own file format (no bundled database).
//
// STANDALONE, header-only. Nothing here is wired (docs/feature-records/core-formats.md, "WIRING HAND-OFF").
//
// WHAT IT STORES: one DecodeEvent per decode: time (Unix ms), radio, frequency (Hz), decoder name, kind, text, named fields.
//
// FILE FORMAT (all integers little-endian), a directory of SEGMENT files seg-<8 digit sequence>.fxd, each at most `segmentBytes` long:
//   header, 32 bytes:  "FXDBSEG1" | u32 version = 1 | u32 0 | u64 sequence | u64 0
//   record:            u32 magic "FXR1" | u32 payloadLength | u32 crc32(payload) | payload
//   payload:           i64 timeMs | f64 freqHz | u16 radio | u8 n + decoder | u8 n + kind | u32 n + text | u8 nFields | nFields x (u8 n + key, u16 n + value)
// A record is written with ONE write call and flushed to the operating system before it becomes visible to a search, so a crash of the
// process leaves at most one torn record at the end of the newest segment. Opening finds it (length past the end of the file, or a bad
// CRC with no valid record after it) and cuts it off. A bad record with valid ones after it (a damaged sector) is skipped: the scanner
// resynchronises on the next magic whose CRC is right, counts the damage, and leaves the file as it is. Power loss can lose what the
// operating system had not yet written (no fsync is issued); the torn-tail rule covers what is left.
// A sealed segment also has an INDEX FILE seg-<n>.fxi: "FXIX" | u32 1 | u64 dataBytes | u64 count | count x 32-byte entries | decoder table
// | u32 crc32 of everything before it. It is used at open instead of scanning the segment when its dataBytes equals the segment's size and
// the CRC is right; otherwise the segment is scanned and the index file is rewritten. An INDEX ENTRY is 32 bytes: i64 timeMs, f64 freqHz,
// u32 offset, u32 total record length, u16 radio, u16 0, u32 FNV-1a hash of the lower-cased decoder name.
//
// SEARCH: by text (case-insensitive ASCII; several words must ALL occur in the text, kind or a field value), decoder (exact, case-
// insensitive), radio, time range, frequency range; newest first (the order of appending: events are normally in time order but the order
// is not assumed, every candidate is tested). Time and frequency filters work on the in-memory index without reading a record; a text
// search reads the records of the candidates.
// RETENTION: by age and by total size, in WHOLE SEGMENTS, oldest first; the newest segment is never dropped by size, and is sealed first
// and dropped by age when ALL its events are older than the limit. So retention is exact to a segment (8 MiB by default), not to a record.
//
// THREADS: any number of readers (search / stats) beside ONE writing object (append / retention). Readers take a short shared lock to copy
// the segment list, read the append-only entry arrays without a lock (entries are published with a release store of the count), and read
// records from the files themselves; a writer's flush happens before the entry is published. A segment dropped by retention while a search
// holds it stays readable to that search where the OS allows (on Windows an open file cannot be deleted: its deletion is retried later).
// One PROCESS may write; other processes open read-only (readOnly = true: no repair, no creation, no deletion; sees the database as of open).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cascade::core::decodedb {

struct DecodeEvent {
    std::int64_t timeMs = 0;
    std::uint16_t radio = 0;
    double freqHz = 0.0;               // 0 = unknown
    std::string decoder;               // "ADS-B", "POCSAG", ...
    std::string kind;                  // "message", "position", ...
    std::string text;
    std::vector<std::pair<std::string, std::string>> fields;
};

struct DecodeRow {
    std::uint64_t id = 0;              // (segment sequence << 32) | index in the segment: stable for the life of the segment
    DecodeEvent event;
};

struct DecodeQuery {
    bool hasTime = false;
    std::int64_t fromMs = 0, toMs = 0;          // inclusive
    bool hasFreq = false;
    double freqMinHz = 0.0, freqMaxHz = 0.0;    // inclusive
    std::string decoder;                        // empty = any
    int radio = -1;                             // -1 = any
    std::string text;                           // words separated by white space, all must occur; empty = any
    std::size_t limit = 100;                    // 0 = none returned; capped at 1,000,000
    std::size_t offset = 0;
    bool newestFirst = true;
};

struct Retention {
    std::int64_t maxAgeMs = 0;                  // 0 = no age limit
    std::uint64_t maxBytes = 0;                 // 0 = no size limit
};

struct RetentionResult {
    std::size_t segmentsDropped = 0;
    std::uint64_t recordsDropped = 0;
    std::uint64_t bytesFreed = 0;
};

struct DbOptions {
    std::uint64_t segmentBytes = 8ull << 20;    // 64 KiB .. 256 MiB
    bool readOnly = false;
};

struct DbStats {
    std::uint64_t records = 0, bytes = 0;
    std::size_t segments = 0;
    std::int64_t oldestMs = 0, newestMs = 0;    // over all records; 0 when empty
    std::size_t tornTailsCut = 0;               // at open: torn tails found and cut off
    std::size_t corruptRegions = 0;             // at open: damaged records skipped
    std::size_t badFiles = 0;                   // segment files with an unreadable header, left untouched
    std::size_t indexesLoaded = 0, segmentsScanned = 0;
    std::uint64_t truncatedValues = 0;          // appended values cut to the size caps
};

namespace detail {

constexpr std::uint32_t kRecMagic = 0x31525846u;   // "FXR1"
constexpr std::size_t kHeaderBytes = 32;
constexpr std::size_t kRecHeader = 12;
constexpr std::size_t kMaxPayload = 1u << 20;
constexpr std::size_t kMaxDecoder = 64, kMaxKind = 64, kMaxText = 16384, kMaxFields = 32, kMaxKey = 64, kMaxValue = 1024;
constexpr std::size_t kChunk = 4096, kMaxChunks = 4096;     // up to 16,777,216 entries per segment

struct Entry {
    std::int64_t timeMs = 0;
    double freqHz = 0.0;
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
    std::uint16_t radio = 0;
    std::uint16_t pad = 0;
    std::uint32_t decoderHash = 0;
};
static_assert(sizeof(Entry) == 32, "the entry is a file format");

inline std::uint32_t crc32(const unsigned char* p, std::size_t n, std::uint32_t crc = 0) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) { c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1); }
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (std::size_t i = 0; i < n; ++i) { crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8); }
    return ~crc;
}

inline char lowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c; }

inline std::uint32_t decoderHash(const std::string& s) {
    std::uint32_t h = 2166136261u;
    for (char c : s) { h ^= static_cast<unsigned char>(lowerAscii(c)); h *= 16777619u; }
    return h;
}

inline bool equalsNoCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) { return false; }
    for (std::size_t i = 0; i < a.size(); ++i) { if (lowerAscii(a[i]) != lowerAscii(b[i])) { return false; } }
    return true;
}

inline bool containsNoCase(const std::string& hay, const std::string& needleLower) {
    if (needleLower.empty()) { return true; }
    if (hay.size() < needleLower.size()) { return false; }
    const char first = needleLower[0];
    const std::size_t last = hay.size() - needleLower.size();
    for (std::size_t i = 0; i <= last; ++i) {
        if (lowerAscii(hay[i]) != first) { continue; }
        std::size_t k = 1;
        while (k < needleLower.size() && lowerAscii(hay[i + k]) == needleLower[k]) { ++k; }
        if (k == needleLower.size()) { return true; }
    }
    return false;
}

inline void put16(unsigned char* p, std::uint16_t v) { p[0] = static_cast<unsigned char>(v); p[1] = static_cast<unsigned char>(v >> 8); }
inline void put32(unsigned char* p, std::uint32_t v) { for (int i = 0; i < 4; ++i) { p[i] = static_cast<unsigned char>(v >> (8 * i)); } }
inline void put64(unsigned char* p, std::uint64_t v) { for (int i = 0; i < 8; ++i) { p[i] = static_cast<unsigned char>(v >> (8 * i)); } }
inline std::uint16_t get16(const unsigned char* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
inline std::uint32_t get32(const unsigned char* p) { return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24); }
inline std::uint64_t get64(const unsigned char* p) { return static_cast<std::uint64_t>(get32(p)) | (static_cast<std::uint64_t>(get32(p + 4)) << 32); }

inline std::string cut(const std::string& s, std::size_t cap, std::uint64_t* truncated) {
    if (s.size() <= cap) { return s; }
    if (truncated != nullptr) { ++*truncated; }
    std::size_t n = cap;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) { --n; }   // not in the middle of a UTF-8 character
    return s.substr(0, n);
}

// Appends the record (header + payload) for `e` to `out`.
inline void encode(const DecodeEvent& e, std::vector<unsigned char>& out, std::uint64_t* truncated) {
    const std::string dec = cut(e.decoder, kMaxDecoder, truncated), kind = cut(e.kind, kMaxKind, truncated), text = cut(e.text, kMaxText, truncated);
    out.clear();
    out.resize(kRecHeader);
    auto add = [&](const void* p, std::size_t n) { const unsigned char* c = static_cast<const unsigned char*>(p); out.insert(out.end(), c, c + n); };
    unsigned char b[8];
    put64(b, static_cast<std::uint64_t>(e.timeMs)); add(b, 8);
    const double f = std::isfinite(e.freqHz) ? e.freqHz : 0.0;
    std::uint64_t fb;
    std::memcpy(&fb, &f, 8);
    put64(b, fb); add(b, 8);
    put16(b, e.radio); add(b, 2);
    b[0] = static_cast<unsigned char>(dec.size()); add(b, 1); add(dec.data(), dec.size());
    b[0] = static_cast<unsigned char>(kind.size()); add(b, 1); add(kind.data(), kind.size());
    put32(b, static_cast<std::uint32_t>(text.size())); add(b, 4); add(text.data(), text.size());
    const std::size_t nf = std::min(e.fields.size(), kMaxFields);
    if (e.fields.size() > kMaxFields && truncated != nullptr) { ++*truncated; }
    b[0] = static_cast<unsigned char>(nf); add(b, 1);
    for (std::size_t i = 0; i < nf; ++i) {
        const std::string k = cut(e.fields[i].first, kMaxKey, truncated), v = cut(e.fields[i].second, kMaxValue, truncated);
        b[0] = static_cast<unsigned char>(k.size()); add(b, 1); add(k.data(), k.size());
        put16(b, static_cast<std::uint16_t>(v.size())); add(b, 2); add(v.data(), v.size());
    }
    const std::size_t payload = out.size() - kRecHeader;
    put32(out.data(), kRecMagic);
    put32(out.data() + 4, static_cast<std::uint32_t>(payload));
    put32(out.data() + 8, crc32(out.data() + kRecHeader, payload));
}

// Decodes a payload (bounds-checked). `full` false reads only what an index entry needs (time, frequency, radio, decoder).
inline bool decodePayload(const unsigned char* p, std::size_t n, DecodeEvent* e, std::uint32_t* hash, bool full) {
    std::size_t o = 0;
    auto need = [&](std::size_t k) { return o + k <= n; };
    if (!need(8 + 8 + 2 + 1)) { return false; }
    e->timeMs = static_cast<std::int64_t>(get64(p)); o += 8;
    const std::uint64_t fb = get64(p + o); o += 8;
    std::memcpy(&e->freqHz, &fb, 8);
    e->radio = get16(p + o); o += 2;
    const std::size_t dl = p[o++];
    if (!need(dl)) { return false; }
    if (hash != nullptr) {
        std::uint32_t h = 2166136261u;
        for (std::size_t i = 0; i < dl; ++i) { h ^= static_cast<unsigned char>(lowerAscii(static_cast<char>(p[o + i]))); h *= 16777619u; }
        *hash = h;
    }
    if (!full) { return true; }
    e->decoder.assign(reinterpret_cast<const char*>(p + o), dl); o += dl;
    if (!need(1)) { return false; }
    const std::size_t kl = p[o++];
    if (!need(kl)) { return false; }
    e->kind.assign(reinterpret_cast<const char*>(p + o), kl); o += kl;
    if (!need(4)) { return false; }
    const std::size_t tl = get32(p + o); o += 4;
    if (tl > kMaxText || !need(tl)) { return false; }
    e->text.assign(reinterpret_cast<const char*>(p + o), tl); o += tl;
    if (!need(1)) { return false; }
    const std::size_t nf = p[o++];
    e->fields.clear();
    for (std::size_t i = 0; i < nf; ++i) {
        if (!need(1)) { return false; }
        const std::size_t kl2 = p[o++];
        if (!need(kl2 + 2)) { return false; }
        std::string k(reinterpret_cast<const char*>(p + o), kl2); o += kl2;
        const std::size_t vl = get16(p + o); o += 2;
        if (!need(vl)) { return false; }
        e->fields.emplace_back(std::move(k), std::string(reinterpret_cast<const char*>(p + o), vl)); o += vl;
    }
    return true;
}

struct Segment {
    std::uint64_t seq = 0;
    std::filesystem::path path, indexPath;
    std::atomic<Entry*> chunks[kMaxChunks];
    std::atomic<std::size_t> count{0};
    std::atomic<std::uint64_t> bytes{kHeaderBytes};
    std::atomic<std::int64_t> minTime{INT64_MAX}, maxTime{INT64_MIN};
    std::atomic<bool> sealed{false};
    Segment() { for (auto& c : chunks) { c.store(nullptr, std::memory_order_relaxed); } }
    ~Segment() { for (auto& c : chunks) { delete[] c.load(std::memory_order_relaxed); } }
    Segment(const Segment&) = delete;
    Segment& operator=(const Segment&) = delete;

    const Entry& at(std::size_t i) const { return chunks[i / kChunk].load(std::memory_order_acquire)[i % kChunk]; }
    // Writer only. False when the segment holds the most entries it can.
    bool push(const Entry& e) {
        const std::size_t n = count.load(std::memory_order_relaxed);
        const std::size_t ci = n / kChunk;
        if (ci >= kMaxChunks) { return false; }
        Entry* c = chunks[ci].load(std::memory_order_relaxed);
        if (c == nullptr) { c = new Entry[kChunk]; chunks[ci].store(c, std::memory_order_release); }
        c[n % kChunk] = e;
        if (e.timeMs < minTime.load(std::memory_order_relaxed)) { minTime.store(e.timeMs, std::memory_order_relaxed); }
        if (e.timeMs > maxTime.load(std::memory_order_relaxed)) { maxTime.store(e.timeMs, std::memory_order_relaxed); }
        count.store(n + 1, std::memory_order_release);
        return true;
    }
};

inline std::string segName(std::uint64_t seq, const char* ext) {
    char b[48];
    std::snprintf(b, sizeof b, "seg-%08llu%s", static_cast<unsigned long long>(seq), ext);
    return b;
}

inline bool readFile(const std::filesystem::path& p, std::vector<unsigned char>& out) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(p, ec);
    if (ec) { return false; }
    std::ifstream f(p, std::ios::binary);
    if (!f) { return false; }
    out.resize(static_cast<std::size_t>(size));
    f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    out.resize(static_cast<std::size_t>(f.gcount()));
    return true;
}

}  // namespace detail

class DecodeDb {
public:
    DecodeDb() = default;
    DecodeDb(const DecodeDb&) = delete;
    DecodeDb& operator=(const DecodeDb&) = delete;
    ~DecodeDb() { close(); }

    bool open(const std::string& dir, const DbOptions& o, std::string* err) {
        close();
        if (o.segmentBytes < (64u << 10) || o.segmentBytes > (256ull << 20)) { return fail(err, "segment size must be 64 KiB to 256 MiB"); }
        opt_ = o;
        dir_ = std::filesystem::path(dir);
        std::error_code ec;
        if (!opt_.readOnly) { std::filesystem::create_directories(dir_, ec); }
        if (!std::filesystem::is_directory(dir_, ec)) { return fail(err, "not a folder: " + dir); }
        stats_ = DbStats{};
        std::vector<std::pair<std::uint64_t, std::filesystem::path>> found;
        for (const auto& de : std::filesystem::directory_iterator(dir_, ec)) {
            const std::string n = de.path().filename().string();
            if (n.size() == 16 && n.rfind("seg-", 0) == 0 && n.substr(12) == ".fxd") {
                bool digits = true;
                for (std::size_t i = 4; i < 12; ++i) { if (n[i] < '0' || n[i] > '9') { digits = false; } }
                if (digits) { found.emplace_back(std::stoull(n.substr(4, 8)), de.path()); }
            }
        }
        std::sort(found.begin(), found.end());
        nextSeq_ = found.empty() ? 1 : found.back().first + 1;   // a bad-header file's number is never reused
        std::vector<std::shared_ptr<detail::Segment>> list;
        for (std::size_t i = 0; i < found.size(); ++i) {
            const std::size_t scannedBefore = stats_.segmentsScanned;
            const bool last = i + 1 == found.size();
            auto seg = std::make_shared<detail::Segment>();
            seg->seq = found[i].first;
            seg->path = found[i].second;
            seg->indexPath = dir_ / detail::segName(seg->seq, ".fxi");
            if (!loadSegment(*seg, /*repair=*/last && !opt_.readOnly, /*useIndex=*/!last)) { ++stats_.badFiles; continue; }
            if (!last && !opt_.readOnly && stats_.segmentsScanned > scannedBefore) { writeIndex(*seg); }
            seg->sealed.store(!last, std::memory_order_relaxed);
            list.push_back(std::move(seg));
        }
        if (list.empty() && !opt_.readOnly) {
            auto s = createSegment(nextSeq_, err);
            if (!s) { return false; }
            list.push_back(std::move(s));
        }
        {
            std::unique_lock lk(listMu_);
            segs_ = std::move(list);
        }
        if (!opt_.readOnly) {
            std::unique_lock lk(wmu_);
            // a last segment that failed to load (bad header) leaves no active one: start a fresh one
            if (segs_.empty() || segs_.back()->sealed.load()) {
                auto s = createSegment(nextSeq_, err);
                if (!s) { return false; }
                std::unique_lock lk2(listMu_);
                segs_.push_back(std::move(s));
            }
            if (!openActive(err)) { return false; }
        }
        opened_ = true;
        return true;
    }

    bool isOpen() const { return opened_; }

    void close() {
        if (!opened_) { return; }
        std::unique_lock wl(wmu_);
        if (!opt_.readOnly && !segs_.empty()) {
            active_.close();
            writeIndex(*segs_.back());   // valid while the segment is not appended to again: the size check at open tells
        }
        opened_ = false;
        std::unique_lock lk(listMu_);
        segs_.clear();
    }

    // Appends one event: written, flushed to the operating system, then visible to searches. False on a disk error (the database stays usable
    // for reading; a torn record, if any, is cut at the next open).
    bool append(const DecodeEvent& e, std::string* err = nullptr) {
        std::unique_lock wl(wmu_);
        return appendLocked(e, true, err);
    }

    // Many events with ONE flush at the end: much faster, and the batch becomes visible together.
    bool appendBatch(const std::vector<DecodeEvent>& evs, std::string* err = nullptr) {
        std::unique_lock wl(wmu_);
        for (std::size_t i = 0; i < evs.size(); ++i) {
            if (!appendLocked(evs[i], i + 1 == evs.size(), err)) { return false; }
        }
        return true;
    }

    std::vector<DecodeRow> search(const DecodeQuery& q, std::string* err = nullptr) const {
        std::vector<DecodeRow> out;
        if (!opened_) { if (err != nullptr) { *err = "database is not open"; } return out; }
        const std::size_t limit = std::min<std::size_t>(q.limit, 1000000);
        if (limit == 0) { return out; }
        std::vector<std::string> words;
        {
            std::string w;
            for (char c : q.text + " ") {
                if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { if (!w.empty()) { words.push_back(w); w.clear(); } }
                else { w.push_back(detail::lowerAscii(c)); }
            }
        }
        const std::uint32_t dh = detail::decoderHash(q.decoder);
        const bool verifyDecoder = !q.decoder.empty() && collision_.load();
        const bool needRecords = !words.empty() || verifyDecoder;
        std::vector<std::shared_ptr<detail::Segment>> segs;
        {
            std::shared_lock lk(listMu_);
            segs = segs_;
        }
        std::size_t matched = 0;
        const std::size_t want = q.offset + limit;
        std::vector<std::size_t> cand;
        const std::size_t nseg = segs.size();
        for (std::size_t si = 0; si < nseg && matched < want; ++si) {
            const detail::Segment& s = *segs[q.newestFirst ? nseg - 1 - si : si];
            const std::size_t n = s.count.load(std::memory_order_acquire);
            if (n == 0) { continue; }
            if (q.hasTime && (s.maxTime.load() < q.fromMs || s.minTime.load() > q.toMs)) { continue; }
            cand.clear();
            for (std::size_t k = 0; k < n; ++k) {
                const std::size_t i = q.newestFirst ? n - 1 - k : k;
                const detail::Entry& e = s.at(i);
                if (q.hasTime && (e.timeMs < q.fromMs || e.timeMs > q.toMs)) { continue; }
                if (q.hasFreq && !(e.freqHz >= q.freqMinHz && e.freqHz <= q.freqMaxHz)) { continue; }
                if (q.radio >= 0 && e.radio != static_cast<std::uint16_t>(q.radio)) { continue; }
                if (!q.decoder.empty() && e.decoderHash != dh) { continue; }
                cand.push_back(i);
            }
            if (cand.empty()) { continue; }
            if (!needRecords) {
                // only the rows that will be returned are read
                std::vector<std::size_t> take;
                for (std::size_t i : cand) {
                    if (matched >= want) { break; }
                    if (matched >= q.offset) { take.push_back(i); }
                    ++matched;
                }
                readRows(s, take, out);
                continue;
            }
            // records must be read to decide: in bulk when many candidates, one by one otherwise
            RecordReader rr(s, cand.size() * 8 >= n);
            for (std::size_t i : cand) {
                if (matched >= want) { break; }
                DecodeEvent ev;
                if (!rr.read(s.at(i), &ev)) { continue; }
                if (verifyDecoder && !detail::equalsNoCase(ev.decoder, q.decoder)) { continue; }
                if (!words.empty() && !textMatches(ev, words)) { continue; }
                if (matched >= q.offset) { out.push_back(DecodeRow{(s.seq << 32) | i, std::move(ev)}); }
                ++matched;
            }
        }
        return out;
    }

    // Retention (see the header comment). Writer-side call.
    RetentionResult enforceRetention(std::int64_t nowMs, const Retention& r) {
        RetentionResult res;
        std::unique_lock wl(wmu_);
        if (!opened_ || opt_.readOnly) { return res; }
        retryPendingDeletes();
        const std::int64_t cutoff = r.maxAgeMs > 0 ? nowMs - r.maxAgeMs : INT64_MIN;
        {   // a newest segment whose events are all too old is sealed so that it can go too
            const auto act = segs_.back();
            if (r.maxAgeMs > 0 && act->count.load() > 0 && act->maxTime.load() < cutoff) { rollLocked(nullptr); }
        }
        for (;;) {
            std::shared_ptr<detail::Segment> oldest;
            std::uint64_t total = 0;
            {
                std::shared_lock lk(listMu_);
                for (const auto& s : segs_) { total += s->bytes.load(); }
                if (segs_.size() > 1) { oldest = segs_.front(); }
            }
            if (!oldest) { break; }
            const bool tooOld = r.maxAgeMs > 0 && (oldest->count.load() == 0 || oldest->maxTime.load() < cutoff);
            const bool tooBig = r.maxBytes > 0 && total > r.maxBytes;
            if (!tooOld && !tooBig) { break; }
            {
                std::unique_lock lk(listMu_);
                segs_.erase(segs_.begin());
            }
            res.segmentsDropped += 1;
            res.recordsDropped += oldest->count.load();
            res.bytesFreed += oldest->bytes.load();
            removeFiles(*oldest);
        }
        return res;
    }

    DbStats stats() const {
        DbStats s = stats_;
        s.records = 0; s.bytes = 0; s.segments = 0; s.oldestMs = 0; s.newestMs = 0;
        std::int64_t lo = INT64_MAX, hi = INT64_MIN;
        std::shared_lock lk(listMu_);
        for (const auto& g : segs_) {
            s.records += g->count.load(std::memory_order_acquire);
            s.bytes += g->bytes.load();
            ++s.segments;
            if (g->count.load() > 0) { lo = std::min(lo, g->minTime.load()); hi = std::max(hi, g->maxTime.load()); }
        }
        if (lo != INT64_MAX) { s.oldestMs = lo; s.newestMs = hi; }
        s.truncatedValues = truncated_.load();
        return s;
    }

    std::uint64_t recordCount() const { return stats().records; }
    std::size_t segmentCount() const { std::shared_lock lk(listMu_); return segs_.size(); }

    // TEST HOOK: makes the next append fail after writing `keepBytes` of the record, as a crash in the middle of a write would, and then
    // the writer refuses everything. Nothing is published to searches.
    void tearNextAppendForTest(std::size_t keepBytes) { tearAt_ = static_cast<long long>(keepBytes); }

private:
    static bool fail(std::string* err, const std::string& m) { if (err != nullptr) { *err = m; } return false; }

    // ---- reading records --------------------------------------------------------------------------------------------------------------

    class RecordReader {
    public:
        RecordReader(const detail::Segment& s, bool bulk) : seg_(s) {
            if (bulk) { bulk_ = detail::readFile(s.path, buf_); } else { f_.open(s.path, std::ios::binary); }
        }
        bool read(const detail::Entry& e, DecodeEvent* ev) {
            const unsigned char* rec = nullptr;
            if (bulk_) {
                if (static_cast<std::uint64_t>(e.offset) + e.length > buf_.size()) { return false; }
                rec = buf_.data() + e.offset;
            } else {
                if (!f_) { return false; }
                tmp_.resize(e.length);
                f_.clear();
                f_.seekg(static_cast<std::streamoff>(e.offset));
                f_.read(reinterpret_cast<char*>(tmp_.data()), static_cast<std::streamsize>(e.length));
                if (static_cast<std::size_t>(f_.gcount()) != e.length) { return false; }
                rec = tmp_.data();
            }
            if (detail::get32(rec) != detail::kRecMagic) { return false; }
            const std::size_t len = detail::get32(rec + 4);
            if (len + detail::kRecHeader != e.length) { return false; }
            if (detail::crc32(rec + detail::kRecHeader, len) != detail::get32(rec + 8)) { return false; }
            return detail::decodePayload(rec + detail::kRecHeader, len, ev, nullptr, true);
        }
    private:
        const detail::Segment& seg_;
        std::vector<unsigned char> buf_, tmp_;
        std::ifstream f_;
        bool bulk_ = false;
    };

    void readRows(const detail::Segment& s, const std::vector<std::size_t>& idx, std::vector<DecodeRow>& out) const {
        if (idx.empty()) { return; }
        RecordReader rr(s, false);
        for (std::size_t i : idx) {
            DecodeEvent ev;
            if (rr.read(s.at(i), &ev)) { out.push_back(DecodeRow{(s.seq << 32) | i, std::move(ev)}); }
        }
    }

    static bool textMatches(const DecodeEvent& e, const std::vector<std::string>& words) {
        for (const std::string& w : words) {
            bool hit = detail::containsNoCase(e.text, w) || detail::containsNoCase(e.kind, w);
            for (std::size_t i = 0; !hit && i < e.fields.size(); ++i) { hit = detail::containsNoCase(e.fields[i].second, w); }
            if (!hit) { return false; }
        }
        return true;
    }

    // ---- opening segments ---------------------------------------------------------------------------------------------------------------

    void noteDecoder(const std::string& name, std::uint32_t h) {
        // a hash shared by two different names is recorded: the decoder filter then verifies each record
        const auto it = decoders_.find(h);
        if (it == decoders_.end()) { decoders_.emplace(h, name); }
        else if (!detail::equalsNoCase(it->second, name)) { collision_.store(true); }
    }

    bool loadSegment(detail::Segment& s, bool repair, bool useIndex) {
        std::error_code ec;
        const std::uintmax_t size = std::filesystem::file_size(s.path, ec);
        if (ec) { return false; }
        if (useIndex && loadIndex(s, size)) { ++stats_.indexesLoaded; s.bytes.store(size); return true; }
        std::vector<unsigned char> buf;
        if (!detail::readFile(s.path, buf) || buf.size() < detail::kHeaderBytes || std::memcmp(buf.data(), "FXDBSEG1", 8) != 0 || detail::get32(buf.data() + 8) != 1) { return false; }
        ++stats_.segmentsScanned;
        std::size_t pos = detail::kHeaderBytes, goodEnd = pos;
        bool damaged = false;
        while (pos + detail::kRecHeader <= buf.size()) {
            const unsigned char* r = buf.data() + pos;
            bool ok = false;
            std::size_t len = 0;
            if (detail::get32(r) == detail::kRecMagic) {
                len = detail::get32(r + 4);
                if (len <= detail::kMaxPayload && pos + detail::kRecHeader + len <= buf.size() && detail::crc32(r + detail::kRecHeader, len) == detail::get32(r + 8)) { ok = true; }
            }
            if (ok) {
                DecodeEvent ev;
                std::uint32_t h = 0;
                if (detail::decodePayload(r + detail::kRecHeader, len, &ev, &h, false)) {
                    detail::Entry e;
                    e.timeMs = ev.timeMs; e.freqHz = ev.freqHz; e.offset = static_cast<std::uint32_t>(pos); e.length = static_cast<std::uint32_t>(detail::kRecHeader + len);
                    e.radio = ev.radio; e.decoderHash = h;
                    if (s.push(e)) {
                        DecodeEvent full;
                        if (detail::decodePayload(r + detail::kRecHeader, len, &full, nullptr, true)) { noteDecoder(full.decoder, h); }
                    }
                    pos += detail::kRecHeader + len;
                    goodEnd = pos;
                    damaged = false;
                    continue;
                }
            }
            // a bad record: look for the next valid one (a damaged middle), else it is the torn tail
            std::size_t next = pos + 1;
            bool found = false;
            for (; next + detail::kRecHeader <= buf.size(); ++next) {
                if (detail::get32(buf.data() + next) != detail::kRecMagic) { continue; }
                const std::size_t l2 = detail::get32(buf.data() + next + 4);
                if (l2 <= detail::kMaxPayload && next + detail::kRecHeader + l2 <= buf.size() && detail::crc32(buf.data() + next + detail::kRecHeader, l2) == detail::get32(buf.data() + next + 8)) { found = true; break; }
            }
            if (!found) { break; }
            if (!damaged) { ++stats_.corruptRegions; damaged = true; }
            pos = next;
        }
        if (goodEnd < buf.size()) {
            if (repair) {
                std::filesystem::resize_file(s.path, goodEnd, ec);
                if (!ec) { ++stats_.tornTailsCut; s.bytes.store(goodEnd); return true; }
            } else { ++stats_.tornTailsCut; }
        }
        s.bytes.store(goodEnd < buf.size() && repair ? goodEnd : buf.size());
        return true;
    }

    bool loadIndex(detail::Segment& s, std::uintmax_t segSize) {
        std::vector<unsigned char> b;
        if (!detail::readFile(s.indexPath, b) || b.size() < 24 + 4 + 4) { return false; }
        if (std::memcmp(b.data(), "FXIX", 4) != 0 || detail::get32(b.data() + 4) != 1) { return false; }
        if (detail::get64(b.data() + 8) != segSize) { return false; }
        const std::uint64_t count = detail::get64(b.data() + 16);
        if (count > static_cast<std::uint64_t>(detail::kChunk) * detail::kMaxChunks) { return false; }
        const std::size_t entriesEnd = 24 + static_cast<std::size_t>(count) * sizeof(detail::Entry);
        if (entriesEnd + 4 + 4 > b.size()) { return false; }
        if (detail::crc32(b.data(), b.size() - 4) != detail::get32(b.data() + b.size() - 4)) { return false; }
        const std::size_t nd = detail::get32(b.data() + entriesEnd);
        std::size_t o = entriesEnd + 4;
        std::vector<std::pair<std::uint32_t, std::string>> names;
        for (std::size_t i = 0; i < nd; ++i) {
            if (o + 5 > b.size() - 4) { return false; }
            const std::uint32_t h = detail::get32(b.data() + o);
            const std::size_t l = b[o + 4];
            if (o + 5 + l > b.size() - 4) { return false; }
            names.emplace_back(h, std::string(reinterpret_cast<const char*>(b.data() + o + 5), l));
            o += 5 + l;
        }
        for (std::size_t i = 0; i < count; ++i) {
            detail::Entry e;
            std::memcpy(&e, b.data() + 24 + i * sizeof(detail::Entry), sizeof e);   // the format is little-endian; every supported host is
            if (!s.push(e)) { return false; }
        }
        for (const auto& n : names) { noteDecoder(n.second, n.first); }
        return true;
    }

    void writeIndex(const detail::Segment& s) {
        std::vector<unsigned char> b(24);
        std::memcpy(b.data(), "FXIX", 4);
        detail::put32(b.data() + 4, 1);
        const std::size_t n = s.count.load();
        detail::put64(b.data() + 8, s.bytes.load());
        detail::put64(b.data() + 16, n);
        std::vector<std::uint32_t> hashes;
        for (std::size_t i = 0; i < n; ++i) {
            const detail::Entry& e = s.at(i);
            const unsigned char* p = reinterpret_cast<const unsigned char*>(&e);
            b.insert(b.end(), p, p + sizeof e);
            if (std::find(hashes.begin(), hashes.end(), e.decoderHash) == hashes.end()) { hashes.push_back(e.decoderHash); }
        }
        unsigned char t[4];
        detail::put32(t, static_cast<std::uint32_t>(hashes.size()));
        b.insert(b.end(), t, t + 4);
        for (std::uint32_t h : hashes) {
            const auto it = decoders_.find(h);
            const std::string nm = it != decoders_.end() ? it->second : std::string();
            detail::put32(t, h);
            b.insert(b.end(), t, t + 4);
            b.push_back(static_cast<unsigned char>(nm.size()));
            b.insert(b.end(), nm.begin(), nm.end());
        }
        detail::put32(t, detail::crc32(b.data(), b.size()));
        b.insert(b.end(), t, t + 4);
        std::filesystem::path tmp = s.indexPath;
        tmp += ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) { return; }
            f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
            if (!f) { return; }
        }
        std::error_code ec;
        std::filesystem::rename(tmp, s.indexPath, ec);
        if (ec) { std::filesystem::remove(tmp, ec); }
    }

    std::shared_ptr<detail::Segment> createSegment(std::uint64_t seq, std::string* err) {
        auto s = std::make_shared<detail::Segment>();
        s->seq = seq;
        s->path = dir_ / detail::segName(seq, ".fxd");
        s->indexPath = dir_ / detail::segName(seq, ".fxi");
        std::ofstream f(s->path, std::ios::binary | std::ios::trunc);
        if (!f) { fail(err, "cannot create " + s->path.string()); return nullptr; }
        unsigned char h[detail::kHeaderBytes] = {};
        std::memcpy(h, "FXDBSEG1", 8);
        detail::put32(h + 8, 1);
        detail::put64(h + 16, seq);
        f.write(reinterpret_cast<const char*>(h), sizeof h);
        f.flush();
        if (!f) { fail(err, "cannot write " + s->path.string()); return nullptr; }
        std::error_code ec;
        std::filesystem::remove(s->indexPath, ec);
        nextSeq_ = seq + 1;
        return s;
    }

    bool openActive(std::string* err) {
        active_.close();
        active_.clear();
        active_.open(segs_.back()->path, std::ios::binary | std::ios::app);
        if (!active_) { return fail(err, "cannot open " + segs_.back()->path.string() + " for appending"); }
        failed_ = false;
        return true;
    }

    // ---- writing -----------------------------------------------------------------------------------------------------------------------

    bool rollLocked(std::string* err) {
        const auto old = segs_.back();
        active_.flush();
        publishPending();
        active_.close();
        writeIndex(*old);
        old->sealed.store(true, std::memory_order_release);
        auto s = createSegment(nextSeq_, err);
        if (!s) { failed_ = true; return false; }
        {
            std::unique_lock lk(listMu_);
            segs_.push_back(std::move(s));
        }
        return openActive(err);
    }

    bool appendLocked(const DecodeEvent& e, bool flushNow, std::string* err) {
        if (!opened_ || opt_.readOnly) { return fail(err, opened_ ? "database is read-only" : "database is not open"); }
        if (failed_) { return fail(err, "an earlier write failed: reopen the database"); }
        std::uint64_t trunc = 0;
        detail::encode(e, rec_, &trunc);
        if (trunc != 0) { truncated_.fetch_add(trunc); }
        std::shared_ptr<detail::Segment> seg = segs_.back();
        if (seg->bytes.load() + rec_.size() > opt_.segmentBytes && seg->count.load() > 0) {
            if (!rollLocked(err)) { return false; }
            seg = segs_.back();
        }
        if (tearAt_ >= 0) {
            const std::size_t keep = std::min<std::size_t>(static_cast<std::size_t>(tearAt_), rec_.size());
            active_.write(reinterpret_cast<const char*>(rec_.data()), static_cast<std::streamsize>(keep));
            active_.flush();
            failed_ = true;
            tearAt_ = -1;
            return fail(err, "simulated crash in the middle of a write");
        }
        active_.write(reinterpret_cast<const char*>(rec_.data()), static_cast<std::streamsize>(rec_.size()));
        if (flushNow) { active_.flush(); }
        if (!active_) { failed_ = true; pending_.clear(); return fail(err, "disk error while writing " + seg->path.string()); }
        detail::Entry en;
        en.timeMs = e.timeMs;
        en.freqHz = std::isfinite(e.freqHz) ? e.freqHz : 0.0;
        en.offset = static_cast<std::uint32_t>(seg->bytes.load());
        en.length = static_cast<std::uint32_t>(rec_.size());
        en.radio = e.radio;
        en.decoderHash = detail::decoderHash(detail::cut(e.decoder, detail::kMaxDecoder, nullptr));
        noteDecoder(detail::cut(e.decoder, detail::kMaxDecoder, nullptr), en.decoderHash);
        seg->bytes.fetch_add(rec_.size());
        // an entry becomes visible to searches only after its bytes were flushed: a batch's entries are published together at its end
        pending_.emplace_back(seg, en);
        if (flushNow && !publishPending()) { failed_ = true; return fail(err, "segment entry table is full"); }
        return true;
    }

    bool publishPending() {
        bool ok = true;
        for (auto& pe : pending_) { if (!pe.first->push(pe.second)) { ok = false; } }
        pending_.clear();
        return ok;
    }

    void removeFiles(const detail::Segment& s) {
        std::error_code ec;
        std::filesystem::remove(s.indexPath, ec);
        std::filesystem::remove(s.path, ec);
        if (std::filesystem::exists(s.path, ec)) { pendingDeletes_.push_back(s.path); }
    }

    void retryPendingDeletes() {
        std::vector<std::filesystem::path> keep;
        for (const auto& p : pendingDeletes_) {
            std::error_code ec;
            std::filesystem::remove(p, ec);
            if (std::filesystem::exists(p, ec)) { keep.push_back(p); }
        }
        pendingDeletes_.swap(keep);
    }

    DbOptions opt_;
    std::filesystem::path dir_;
    bool opened_ = false;
    mutable std::shared_mutex listMu_;
    std::vector<std::shared_ptr<detail::Segment>> segs_;
    std::ofstream active_;
    std::vector<unsigned char> rec_;
    std::uint64_t nextSeq_ = 1;
    bool failed_ = false;
    long long tearAt_ = -1;
    std::vector<std::pair<std::shared_ptr<detail::Segment>, detail::Entry>> pending_;   // written, not yet flushed or visible
    std::unordered_map<std::uint32_t, std::string> decoders_;
    std::atomic<bool> collision_{false};
    std::atomic<std::uint64_t> truncated_{0};
    std::vector<std::filesystem::path> pendingDeletes_;
    DbStats stats_;
    std::mutex wmu_;
};

}  // namespace cascade::core::decodedb
