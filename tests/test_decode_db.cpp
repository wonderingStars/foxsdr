// Tests for the decode database (core/decode_db.hpp).
//
// The search is checked against a brute-force reference model on randomized queries; torn and damaged files are made by hand at EVERY cut
// point of a record; a writer, readers and a retention thread run together; and a million records give the measured write and search times.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/decode_db.hpp"

#include "test_check.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <thread>

namespace {

namespace db = cascade::core::decodedb;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

fs::path dirFor(const char* name) {
    const fs::path d = fs::temp_directory_path() / (std::string("coref-decodedb-") + name);
    std::error_code ec;
    fs::remove_all(d, ec);
    return d;
}

void wipe(const fs::path& d) { std::error_code ec; fs::remove_all(d, ec); }

const char* kDecoders[] = {"ADS-B", "POCSAG", "AIS", "APRS", "RTTY", "WEFAX", "ACARS", "Sonde"};

db::DecodeEvent makeEvent(std::uint64_t i, std::int64_t baseMs = 1700000000000LL) {
    db::DecodeEvent e;
    e.timeMs = baseMs + static_cast<std::int64_t>(i) * 1000;
    e.radio = static_cast<std::uint16_t>(i % 4);
    e.freqHz = 100e6 + static_cast<double>(i % 1000) * 25e3;
    e.decoder = kDecoders[i % 8];
    e.kind = (i % 3 == 0) ? "position" : "message";
    e.text = "msg " + std::to_string(i) + " from KJ4XYZ-" + std::to_string(i % 97) + " hello world";
    if (i % 50000 == 7) { e.text += " RAREWORD"; }
    e.fields.emplace_back("callsign", "KJ4XYZ-" + std::to_string(i % 97));
    e.fields.emplace_back("alt", std::to_string(1000 + i % 40000));
    return e;
}

bool sameEvent(const db::DecodeEvent& a, const db::DecodeEvent& b) {
    return a.timeMs == b.timeMs && a.radio == b.radio && a.freqHz == b.freqHz && a.decoder == b.decoder && a.kind == b.kind && a.text == b.text && a.fields == b.fields;
}

std::string lower(std::string s) { for (char& c : s) { if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c + 32); } } return s; }

bool refMatch(const db::DecodeEvent& e, const db::DecodeQuery& q) {
    if (q.hasTime && (e.timeMs < q.fromMs || e.timeMs > q.toMs)) { return false; }
    if (q.hasFreq && !(e.freqHz >= q.freqMinHz && e.freqHz <= q.freqMaxHz)) { return false; }
    if (q.radio >= 0 && e.radio != q.radio) { return false; }
    if (!q.decoder.empty() && lower(e.decoder) != lower(q.decoder)) { return false; }
    std::size_t p = 0;
    const std::string t = lower(q.text) + " ";
    std::string w;
    for (char c : t) {
        if (c == ' ') {
            if (!w.empty()) {
                bool hit = lower(e.text).find(w) != std::string::npos || lower(e.kind).find(w) != std::string::npos;
                for (const auto& f : e.fields) { if (lower(f.second).find(w) != std::string::npos) { hit = true; } }
                if (!hit) { return false; }
                w.clear();
            }
        } else { w.push_back(c); }
    }
    (void)p;
    return true;
}

std::vector<std::size_t> refSearch(const std::vector<db::DecodeEvent>& all, const db::DecodeQuery& q) {
    std::vector<std::size_t> hits;
    if (q.newestFirst) { for (std::size_t i = all.size(); i-- > 0;) { if (refMatch(all[i], q)) { hits.push_back(i); } } }
    else { for (std::size_t i = 0; i < all.size(); ++i) { if (refMatch(all[i], q)) { hits.push_back(i); } } }
    std::vector<std::size_t> out;
    for (std::size_t k = q.offset; k < hits.size() && out.size() < q.limit; ++k) { out.push_back(hits[k]); }
    return out;
}

db::DbOptions smallSegments() {
    db::DbOptions o;
    o.segmentBytes = 64u << 10;
    return o;
}

// ---- basics and filters --------------------------------------------------------------------------------------------------------------

void testBasicsAndFilters() {
    const fs::path d = dirFor("basic");
    db::DecodeDb store;
    std::string err;
    CHECK(store.open(d.string(), smallSegments(), &err));
    CHECK(store.search(db::DecodeQuery{}).empty());   // empty database
    CHECK(store.stats().records == 0 && store.stats().segments == 1);
    std::vector<db::DecodeEvent> all;
    for (std::uint64_t i = 0; i < 3000; ++i) { all.push_back(makeEvent(i)); CHECK(store.append(all.back(), &err)); }
    CHECK(store.recordCount() == 3000);
    CHECK(store.segmentCount() > 3);                  // 64 KiB segments: the data spans several
    {
        db::DecodeQuery q;
        q.limit = 5;
        const auto r = store.search(q);
        CHECK(r.size() == 5);
        for (std::size_t k = 0; k < r.size() && k < 5; ++k) { CHECK(sameEvent(r[k].event, all[2999 - k])); }   // newest first
        q.newestFirst = false;
        const auto o = store.search(q);
        CHECK(o.size() == 5 && sameEvent(o[0].event, all[0]) && sameEvent(o[4].event, all[4]));
        q.newestFirst = true;
        q.offset = 10;
        const auto s = store.search(q);
        CHECK(s.size() == 5 && sameEvent(s[0].event, all[2989]));
        q.offset = 5000;
        CHECK(store.search(q).empty());
        q.offset = 0;
        q.limit = 0;
        CHECK(store.search(q).empty());
    }
    db::DecodeQuery q;
    q.limit = 100000;
    q.text = "RAREWORD";
    CHECK(store.search(q).size() == 1);                // event 7 carries it
    q.text = "kj4xyz-5";
    {
        const auto r = store.search(q);
        std::size_t want = 0;
        for (const auto& e : all) { if (refMatch(e, q)) { ++want; } }
        CHECK(r.size() == want && want > 0);
    }
    q.text = "HELLO position";                         // AND of a text word and the kind
    CHECK(store.search(q).size() == 1000);
    q.text = "  hello   \t world  ";                   // spacing ignored
    CHECK(store.search(q).size() == 3000);
    q.text = "";
    q.decoder = "ais";                                 // exact, case-insensitive
    CHECK(store.search(q).size() == 375);
    q.decoder = "AI";
    CHECK(store.search(q).empty());
    q.decoder = "";
    q.radio = 2;
    CHECK(store.search(q).size() == 750);
    q.radio = -1;
    q.hasTime = true;
    q.fromMs = all[100].timeMs;
    q.toMs = all[199].timeMs;                          // inclusive both ends
    CHECK(store.search(q).size() == 100);
    q.fromMs = q.toMs = all[100].timeMs;
    CHECK(store.search(q).size() == 1);
    q.fromMs = 5; q.toMs = 1;
    CHECK(store.search(q).empty());                    // an inverted range matches nothing
    q.hasTime = false;
    q.hasFreq = true;
    q.freqMinHz = 100e6 + 10 * 25e3;
    q.freqMaxHz = 100e6 + 12 * 25e3;
    CHECK(store.search(q).size() == 9);                // 3 frequencies x 3 passes of 1000
    q.freqMinHz = 1; q.freqMaxHz = 2;
    CHECK(store.search(q).empty());
    // non-ASCII text matched byte for byte
    db::DecodeEvent u = makeEvent(5000);
    u.text = "caf\xC3\xA9 \xE2\x82\xAC";
    CHECK(store.append(u, &err));
    db::DecodeQuery uq;
    uq.text = "caf\xC3\xA9";
    CHECK(store.search(uq).size() == 1);
    uq.text = "CAF\xC3\xA9";                           // ASCII case folding only: the accented byte pair is not folded, but the rest matches
    CHECK(store.search(uq).size() == 1);
    store.close();
    wipe(d);
}

void testRandomizedAgainstReference() {
    const fs::path d = dirFor("random");
    db::DecodeDb store;
    std::string err;
    CHECK(store.open(d.string(), smallSegments(), &err));
    std::vector<db::DecodeEvent> all;
    std::mt19937_64 g(77);
    for (std::uint64_t i = 0; i < 20000; ++i) {
        db::DecodeEvent e = makeEvent(i);
        if (g() % 11 == 0) { e.timeMs -= static_cast<std::int64_t>(g() % 5000) * 1000; }   // some events out of time order
        all.push_back(e);
    }
    std::vector<db::DecodeEvent> batch;
    for (std::size_t i = 0; i < all.size(); ++i) {
        batch.push_back(all[i]);
        if (batch.size() == 37 || i + 1 == all.size()) { CHECK(store.appendBatch(batch, &err)); batch.clear(); }
    }
    CHECK(store.recordCount() == all.size());
    int bad = 0;
    for (int n = 0; n < 300; ++n) {
        db::DecodeQuery q;
        q.limit = 1 + g() % 40;
        q.offset = g() % 3 == 0 ? g() % 30 : 0;
        q.newestFirst = g() % 4 != 0;
        if (g() % 2) { q.hasTime = true; const auto a = all[g() % all.size()].timeMs, b = all[g() % all.size()].timeMs; q.fromMs = std::min(a, b); q.toMs = std::max(a, b); }
        if (g() % 3 == 0) { q.hasFreq = true; q.freqMinHz = 100e6 + (g() % 500) * 25e3; q.freqMaxHz = q.freqMinHz + (g() % 300) * 25e3; }
        if (g() % 3 == 0) { q.decoder = kDecoders[g() % 8]; if (g() % 2) { q.decoder = lower(q.decoder); } }
        if (g() % 4 == 0) { q.radio = static_cast<int>(g() % 4); }
        const char* words[] = {"hello", "KJ4XYZ-3", "position", "message", "7", "RAREWORD", "nothing", "from KJ4XYZ-1"};
        if (g() % 2) { q.text = words[g() % 8]; if (g() % 5 == 0) { q.text += " "; q.text += words[g() % 8]; } }
        const auto got = store.search(q);
        const auto want = refSearch(all, q);
        bool ok = got.size() == want.size();
        for (std::size_t k = 0; ok && k < got.size(); ++k) { ok = sameEvent(got[k].event, all[want[k]]); }
        if (!ok) { ++bad; }
    }
    CHECK(bad == 0);
    std::printf("  randomized queries against the reference: 300 run, %d differ\n", bad);
    store.close();
    wipe(d);
}

// ---- persistence, index files -----------------------------------------------------------------------------------------------------------

void testPersistenceAndIndex() {
    const fs::path d = dirFor("persist");
    std::string err;
    std::vector<db::DecodeEvent> all;
    {
        db::DecodeDb store;
        CHECK(store.open(d.string(), smallSegments(), &err));
        for (std::uint64_t i = 0; i < 4000; ++i) { all.push_back(makeEvent(i)); CHECK(store.append(all.back(), &err)); }
        store.close();
    }
    std::size_t fxd = 0, fxi = 0;
    for (const auto& e : fs::directory_iterator(d)) { if (e.path().extension() == ".fxd") { ++fxd; } if (e.path().extension() == ".fxi") { ++fxi; } }
    CHECK(fxd >= 4 && fxi == fxd);                     // every sealed segment and the active one at close
    db::DecodeQuery q;
    q.text = "kj4xyz-9 ";
    q.limit = 1000000;
    std::size_t expect = 0;
    for (const auto& e : all) { if (refMatch(e, q)) { ++expect; } }
    {
        db::DecodeDb store;
        CHECK(store.open(d.string(), smallSegments(), &err));
        CHECK(store.recordCount() == 4000);
        CHECK(store.stats().indexesLoaded + 1 == store.stats().segments);   // sealed ones from their index, the newest scanned
        CHECK(store.search(q).size() == expect);
        CHECK(sameEvent(store.search(db::DecodeQuery{}).at(0).event, all[3999]));
    }
    // index files removed: scanned, identical result, and the indexes are rewritten
    for (const auto& e : fs::directory_iterator(d)) { if (e.path().extension() == ".fxi") { fs::remove(e.path()); } }
    {
        db::DecodeDb store;
        CHECK(store.open(d.string(), smallSegments(), &err));
        CHECK(store.stats().indexesLoaded == 0 && store.stats().segmentsScanned == store.stats().segments);
        CHECK(store.recordCount() == 4000 && store.search(q).size() == expect);
    }
    std::size_t again = 0;
    for (const auto& e : fs::directory_iterator(d)) { if (e.path().extension() == ".fxi") { ++again; } }
    CHECK(again >= fxd - 1);
    // a damaged index file (one flipped byte) is not trusted
    {
        fs::path first;
        for (const auto& e : fs::directory_iterator(d)) { if (e.path().extension() == ".fxi") { first = e.path(); break; } }
        std::fstream f(first, std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(100);
        char c = 0;
        f.read(&c, 1);
        f.seekp(100);
        c = static_cast<char>(c ^ 0x55);
        f.write(&c, 1);
        f.close();
        db::DecodeDb store;
        CHECK(store.open(d.string(), smallSegments(), &err));
        CHECK(store.stats().segmentsScanned >= 2);     // the damaged index's segment and the newest
        CHECK(store.recordCount() == 4000 && store.search(q).size() == expect);
    }
    wipe(d);
    // a STALE index (an older copy: the segment grew after it was written) is not trusted
    {
        db::DecodeDb a;
        CHECK(a.open(d.string(), smallSegments(), &err));
        for (std::uint64_t i = 0; i < 100; ++i) { CHECK(a.append(makeEvent(i), &err)); }
        a.close();
        const fs::path idx = d / "seg-00000001.fxi";
        CHECK(fs::exists(idx));
        fs::copy_file(idx, d / "old.bak", fs::copy_options::overwrite_existing);
        db::DecodeDb b;
        CHECK(b.open(d.string(), smallSegments(), &err));
        for (std::uint64_t i = 100; i < 3100; ++i) { CHECK(b.append(makeEvent(i), &err)); }   // segment 1 fills, is sealed, gets a new index
        b.close();
        fs::copy_file(d / "old.bak", idx, fs::copy_options::overwrite_existing);
        db::DecodeDb c;
        CHECK(c.open(d.string(), smallSegments(), &err));
        CHECK(c.recordCount() == 3100);                 // not 100 + the later segments
        CHECK(c.stats().segmentsScanned >= 2);
    }
    wipe(d);
}

// ---- crash safety -----------------------------------------------------------------------------------------------------------------------

fs::path lastSegment(const fs::path& d) {
    fs::path last;
    for (const auto& e : fs::directory_iterator(d)) { if (e.path().extension() == ".fxd" && (last.empty() || e.path() > last)) { last = e.path(); } }
    return last;
}

void testTornWrites() {
    std::string err;
    // (a) a simulated crash inside a write, at every byte of the record
    db::DecodeEvent victim = makeEvent(999);
    std::vector<unsigned char> rec;
    db::detail::encode(victim, rec, nullptr);
    int wrongCount = 0, wrongCut = 0, notWritable = 0;
    for (std::size_t keep = 0; keep <= rec.size(); keep += (keep < 40 || keep + 40 > rec.size()) ? 1 : 7) {
        const fs::path d = dirFor("torn");
        {
            db::DecodeDb store;
            CHECK(store.open(d.string(), db::DbOptions{}, &err));
            for (std::uint64_t i = 0; i < 20; ++i) { CHECK(store.append(makeEvent(i), &err)); }
            store.tearNextAppendForTest(keep);
            CHECK(!store.append(victim, &err));
            CHECK(!store.append(makeEvent(21), &err));   // the writer refuses further writes after the failure
            CHECK(store.recordCount() == 20);            // the torn record was never visible
        }
        db::DecodeDb again;
        CHECK(again.open(d.string(), db::DbOptions{}, &err));
        // a complete record that was written but not acknowledged (keep == the whole record) is a valid record and is kept
        const std::uint64_t expect = keep == rec.size() ? 21 : 20;
        if (again.recordCount() != expect) { ++wrongCount; }
        if (again.stats().tornTailsCut != (keep == 0 || keep == rec.size() ? 0u : 1u)) { ++wrongCut; }
        // the repaired database accepts new writes and they survive another reopen
        if (!again.append(makeEvent(500), &err)) { ++notWritable; }
        again.close();
        db::DecodeDb third;
        CHECK(third.open(d.string(), db::DbOptions{}, &err));
        if (third.recordCount() != expect + 1 || !sameEvent(third.search(db::DecodeQuery{}).at(0).event, makeEvent(500))) { ++wrongCount; }
        third.close();
        wipe(d);
    }
    CHECK(wrongCount == 0 && wrongCut == 0 && notWritable == 0);
    std::printf("  torn write at every cut of a %zu-byte record: %d wrong counts, %d wrong repairs, %d unwritable\n", rec.size(), wrongCount, wrongCut, notWritable);

    // (b) the file cut short by hand at every byte of its last record, and random garbage appended
    const fs::path d = dirFor("torn2");
    {
        db::DecodeDb store;
        CHECK(store.open(d.string(), db::DbOptions{}, &err));
        for (std::uint64_t i = 0; i < 10; ++i) { CHECK(store.append(makeEvent(i), &err)); }
    }
    const fs::path seg = lastSegment(d);
    const std::uintmax_t full = fs::file_size(seg);
    db::DecodeEvent lastEv = makeEvent(9);
    std::vector<unsigned char> lastRec;
    db::detail::encode(lastEv, lastRec, nullptr);
    int bad = 0;
    for (std::size_t cutBy = 1; cutBy <= lastRec.size(); ++cutBy) {
        fs::copy_file(seg, d / "keep.bak", fs::copy_options::overwrite_existing);
        fs::resize_file(seg, full - cutBy);
        for (const auto& e : fs::directory_iterator(d)) { if (e.path().extension() == ".fxi") { fs::remove(e.path()); } }
        db::DecodeDb store;
        if (!store.open(d.string(), db::DbOptions{}, &err) || store.recordCount() != 9) { ++bad; }
        store.close();
        fs::copy_file(d / "keep.bak", seg, fs::copy_options::overwrite_existing);
        for (const auto& e : fs::directory_iterator(d)) { if (e.path().extension() == ".fxi") { fs::remove(e.path()); } }
    }
    CHECK(bad == 0);
    {   // garbage after the last record
        std::ofstream f(seg, std::ios::binary | std::ios::app);
        std::mt19937 g(3);
        for (int i = 0; i < 700; ++i) { f.put(static_cast<char>(g())); }
    }
    for (const auto& e : fs::directory_iterator(d)) { if (e.path().extension() == ".fxi") { fs::remove(e.path()); } }
    {
        db::DecodeDb store;
        CHECK(store.open(d.string(), db::DbOptions{}, &err));
        CHECK(store.recordCount() == 10);
        CHECK(store.stats().tornTailsCut == 1);
        CHECK(fs::file_size(seg) == full);               // the garbage was cut off
    }
    // (c) one damaged record in the MIDDLE: it is skipped, the records around it are kept, nothing is truncated
    {
        std::vector<std::uint64_t> offs;
        std::uint64_t at = 32;
        for (std::uint64_t i = 0; i < 10; ++i) { std::vector<unsigned char> r; db::detail::encode(makeEvent(i), r, nullptr); offs.push_back(at); at += r.size(); }
        {
            std::fstream f(seg, std::ios::in | std::ios::out | std::ios::binary);
            f.seekp(static_cast<std::streamoff>(offs[4] + 20));
            char c = 0;
            f.seekg(static_cast<std::streamoff>(offs[4] + 20));
            f.read(&c, 1);
            f.seekp(static_cast<std::streamoff>(offs[4] + 20));
            c = static_cast<char>(c ^ 0xFF);
            f.write(&c, 1);
        }
        for (const auto& e : fs::directory_iterator(d)) { if (e.path().extension() == ".fxi") { fs::remove(e.path()); } }
        db::DecodeDb store;
        CHECK(store.open(d.string(), db::DbOptions{}, &err));
        CHECK(store.recordCount() == 9);
        CHECK(store.stats().corruptRegions == 1 && store.stats().tornTailsCut == 0);
        CHECK(fs::file_size(seg) == full);               // nothing cut: valid records follow the damage
        db::DecodeQuery q;
        q.limit = 100;
        const auto r = store.search(q);
        CHECK(r.size() == 9);
        bool has4 = false;
        for (const auto& row : r) { if (row.event.timeMs == makeEvent(4).timeMs) { has4 = true; } }
        CHECK(!has4);
    }
    wipe(d);
}

void testBadFilesAndOpenRefusals() {
    std::string err;
    db::DecodeDb store;
    db::DbOptions o;
    o.segmentBytes = 1000;
    CHECK(!store.open(dirFor("x").string(), o, &err) && !err.empty());
    o.segmentBytes = 1ull << 40;
    CHECK(!store.open(dirFor("x").string(), o, &err));
    {   // a path that is a file
        const fs::path f = fs::temp_directory_path() / "coref-decodedb-file";
        { std::ofstream(f) << "x"; }
        CHECK(!store.open(f.string(), db::DbOptions{}, &err));
        fs::remove(f);
    }
    {   // read-only on a missing folder
        db::DbOptions ro;
        ro.readOnly = true;
        CHECK(!store.open(dirFor("missing").string(), ro, &err));
    }
    // a segment with a damaged header is left alone and counted; other files are ignored; the sequence number is not reused
    const fs::path d = dirFor("badfile");
    fs::create_directories(d);
    { std::ofstream f(d / "seg-00000001.fxd", std::ios::binary); f << "this is not a segment file at all, but it is long enough to look like one .........."; }
    { std::ofstream f(d / "notes.txt"); f << "hello"; }
    { std::ofstream f(d / "seg-abcdefgh.fxd"); f << "x"; }
    {
        db::DecodeDb s2;
        CHECK(s2.open(d.string(), db::DbOptions{}, &err));
        CHECK(s2.stats().badFiles == 1);
        CHECK(s2.append(makeEvent(1), &err));
        CHECK(s2.recordCount() == 1);
    }
    CHECK(fs::exists(d / "seg-00000002.fxd"));
    std::ifstream f(d / "seg-00000001.fxd", std::ios::binary);
    std::string head(20, ' ');
    f.read(head.data(), 20);
    CHECK(head.substr(0, 10) == "this is no");        // untouched
    f.close();
    // read-only: sees the data, writes nothing
    {
        db::DbOptions ro;
        ro.readOnly = true;
        db::DecodeDb r;
        CHECK(r.open(d.string(), ro, &err));
        CHECK(r.recordCount() == 1 && r.search(db::DecodeQuery{}).size() == 1);
        CHECK(!r.append(makeEvent(2), &err) && err.find("read-only") != std::string::npos);
        CHECK(r.enforceRetention(0, db::Retention{1, 1}).segmentsDropped == 0);
    }
    CHECK(!store.append(makeEvent(1), &err));        // not open
    CHECK(store.search(db::DecodeQuery{}, &err).empty());
    wipe(d);
}

// A record damaged AFTER its segment was sealed and indexed: the index is trusted at open (it has its own CRC), so the damage shows only
// when the record is read, and the CRC check on read turns it into a missing row, never into wrong data.
void testDamageUnderIndex() {
    const fs::path d = dirFor("under-index");
    std::string err;
    {
        db::DecodeDb a;
        CHECK(a.open(d.string(), smallSegments(), &err));
        for (std::uint64_t i = 0; i < 4000; ++i) { CHECK(a.append(makeEvent(i), &err)); }
    }
    {
        std::fstream f(d / "seg-00000001.fxd", std::ios::in | std::ios::out | std::ios::binary);
        f.seekg(32 + 20);
        char c = 0;
        f.read(&c, 1);
        c = static_cast<char>(c ^ 0x5A);
        f.seekp(32 + 20);
        f.write(&c, 1);
    }
    db::DecodeDb b;
    CHECK(b.open(d.string(), smallSegments(), &err));
    CHECK(b.stats().indexesLoaded >= 1);
    CHECK(b.recordCount() == 4000);                    // the index still lists it
    db::DecodeQuery q;
    q.limit = 100000;
    const auto rows = b.search(q);
    CHECK(rows.size() == 3999);
    for (const auto& r : rows) { CHECK(r.event.timeMs != makeEvent(0).timeMs); }
    b.close();
    wipe(d);
}

// ---- value caps ------------------------------------------------------------------------------------------------------------------

void testCaps() {
    const fs::path d = dirFor("caps");
    std::string err;
    db::DecodeDb store;
    CHECK(store.open(d.string(), db::DbOptions{}, &err));
    db::DecodeEvent e = makeEvent(1);
    e.decoder = std::string(200, 'd');
    e.kind = std::string(100, 'k');
    e.text = std::string(1 << 20, 't') + "END";
    e.fields.clear();
    for (int i = 0; i < 50; ++i) { e.fields.emplace_back(std::string(100, 'a' + i % 26), std::string(5000, 'v')); }
    e.freqHz = std::nan("");
    CHECK(store.append(e, &err));
    const auto r = store.search(db::DecodeQuery{});
    CHECK(r.size() == 1);
    if (!r.empty()) {
        const auto& g = r[0].event;
        CHECK(g.decoder.size() == 64 && g.kind.size() == 64 && g.text.size() == 16384);
        CHECK(g.fields.size() == 32 && g.fields[0].first.size() == 64 && g.fields[0].second.size() == 1024);
        CHECK(g.freqHz == 0.0);
    }
    CHECK(store.stats().truncatedValues > 4);
    // a cut never splits a UTF-8 character
    db::DecodeEvent u = makeEvent(2);
    u.text.clear();
    for (int i = 0; i < 9000; ++i) { u.text += "\xE2\x82\xAC"; }   // 27000 bytes
    CHECK(store.append(u, &err));
    db::DecodeQuery q;
    q.limit = 1;
    const auto s = store.search(q);
    CHECK(s.size() == 1 && s[0].event.text.size() % 3 == 0 && s[0].event.text.size() <= 16384 && s[0].event.text.size() >= 16380);
    // empty strings and zero fields are fine
    db::DecodeEvent z;
    CHECK(store.append(z, &err));
    CHECK(store.search(q)[0].event.decoder.empty());
    // an event that cannot be searched by decoder 'whitespace' still stores
    store.close();
    wipe(d);
}

// ---- hash collision in the decoder index ------------------------------------------------------------------------------------------------

void testDecoderHashCollision() {
    // find two different names with the same 32-bit FNV-1a hash (birthday: about 80000 tries)
    std::unordered_map<std::uint32_t, std::string> seen;
    std::string a, b;
    for (std::uint64_t i = 0; i < 2000000 && a.empty(); ++i) {
        const std::string n = "dec" + std::to_string(i);
        const std::uint32_t h = db::detail::decoderHash(n);
        const auto it = seen.find(h);
        if (it != seen.end()) { a = it->second; b = n; } else { seen.emplace(h, n); }
    }
    CHECK(!a.empty());
    if (a.empty()) { return; }
    std::printf("  decoder hash collision found: '%s' and '%s'\n", a.c_str(), b.c_str());
    const fs::path d = dirFor("collide");
    std::string err;
    db::DecodeDb store;
    CHECK(store.open(d.string(), db::DbOptions{}, &err));
    for (int i = 0; i < 10; ++i) {
        db::DecodeEvent e = makeEvent(static_cast<std::uint64_t>(i));
        e.decoder = (i % 2 == 0) ? a : b;
        CHECK(store.append(e, &err));
    }
    db::DecodeQuery q;
    q.decoder = a;
    q.limit = 100;
    CHECK(store.search(q).size() == 5);
    q.decoder = b;
    CHECK(store.search(q).size() == 5);
    for (const auto& r : store.search(q)) { CHECK(r.event.decoder == b); }
    store.close();
    wipe(d);
}

// ---- retention --------------------------------------------------------------------------------------------------------------------------

std::size_t countFiles(const fs::path& d, const char* ext) {
    std::size_t n = 0;
    for (const auto& e : fs::directory_iterator(d)) { if (e.path().extension() == ext) { ++n; } }
    return n;
}

void testRetention() {
    std::string err;
    {   // by size
        const fs::path d = dirFor("ret-size");
        db::DecodeDb store;
        CHECK(store.open(d.string(), smallSegments(), &err));
        for (std::uint64_t i = 0; i < 6000; ++i) { CHECK(store.append(makeEvent(i), &err)); }
        const auto before = store.stats();
        CHECK(before.segments > 5);
        const auto lim = before.bytes / 2;
        const auto res = store.enforceRetention(0, db::Retention{0, lim});
        const auto after = store.stats();
        std::printf("  size retention: %zu -> %zu segments, %llu -> %llu bytes (limit %llu), %llu records dropped\n", before.segments, after.segments,
                    static_cast<unsigned long long>(before.bytes), static_cast<unsigned long long>(after.bytes), static_cast<unsigned long long>(lim),
                    static_cast<unsigned long long>(res.recordsDropped));
        CHECK(res.segmentsDropped == before.segments - after.segments && res.segmentsDropped > 0);
        CHECK(after.bytes <= lim);
        CHECK(after.bytes + res.bytesFreed == before.bytes);
        CHECK(after.records + res.recordsDropped == before.records);
        CHECK(countFiles(d, ".fxd") == after.segments);   // the files are gone, not just forgotten
        // the oldest went: what is left is a suffix of what was written
        db::DecodeQuery q;
        q.limit = 100000;
        q.newestFirst = false;
        const auto rows = store.search(q);
        CHECK(rows.size() == after.records);
        if (!rows.empty()) {
            const std::uint64_t firstIdx = static_cast<std::uint64_t>((rows[0].event.timeMs - 1700000000000LL) / 1000);
            CHECK(firstIdx == 6000 - rows.size());
            CHECK(sameEvent(rows.back().event, makeEvent(5999)));
        }
        // the database keeps working
        CHECK(store.append(makeEvent(6000), &err));
        // a limit smaller than the newest segment still keeps the newest segment
        const auto res2 = store.enforceRetention(0, db::Retention{0, 1});
        CHECK(store.stats().segments == 1 && store.recordCount() >= 1);
        (void)res2;
        // no limits: nothing happens
        const auto res3 = store.enforceRetention(0, db::Retention{});
        CHECK(res3.segmentsDropped == 0);
        const std::uint64_t kept = store.recordCount();
        store.close();
        db::DecodeDb again;
        CHECK(again.open(d.string(), smallSegments(), &err));
        CHECK(again.recordCount() == kept && kept >= 1);   // retention survives a reopen
        again.close();
        wipe(d);
    }
    {   // by age
        const fs::path d = dirFor("ret-age");
        db::DecodeDb store;
        CHECK(store.open(d.string(), smallSegments(), &err));
        for (std::uint64_t i = 0; i < 6000; ++i) { CHECK(store.append(makeEvent(i), &err)); }   // 1000 s apart in total: 6000 s
        const std::int64_t base = 1700000000000LL;
        const auto before = store.stats();
        // now = base + 6000 s, keep 3000 s: drops segments whose newest event is older than base + 3000 s
        const auto res = store.enforceRetention(base + 6000 * 1000LL, db::Retention{3000 * 1000LL, 0});
        const auto after = store.stats();
        CHECK(res.segmentsDropped > 0 && after.records + res.recordsDropped == before.records);
        db::DecodeQuery q;
        q.limit = 100000;
        q.newestFirst = false;
        const auto rows = store.search(q);
        CHECK(!rows.empty());
        // exact to a segment: everything within the age limit is still there, and the oldest survivor is less than one segment older
        CHECK(!rows.empty() && rows[0].event.timeMs <= base + 3000 * 1000LL);
        CHECK(rows.back().event.timeMs == makeEvent(5999).timeMs);
        std::size_t within = 0;
        for (const auto& r : rows) { if (r.event.timeMs >= base + 3000 * 1000LL) { ++within; } }
        CHECK(within == 3000);
        CHECK(base + 3000 * 1000LL - rows[0].event.timeMs < 1500 * 1000LL);   // one 64 KiB segment is about 600 records here
        // everything older than the limit: even the newest segment is sealed and dropped; the database stays usable
        const auto res2 = store.enforceRetention(base + 100000 * 1000LL, db::Retention{1000, 0});
        CHECK(res2.segmentsDropped > 0);
        CHECK(store.recordCount() == 0);
        CHECK(store.append(makeEvent(7000), &err));
        CHECK(store.recordCount() == 1);
        store.close();
        wipe(d);
    }
}

// ---- concurrency ------------------------------------------------------------------------------------------------------------------------

void testConcurrentWriterReadersRetention() {
    const fs::path d = dirFor("conc");
    std::string err;
    db::DecodeDb store;
    CHECK(store.open(d.string(), smallSegments(), &err));
    constexpr std::uint64_t kTotal = 60000;
    std::atomic<bool> done{false};
    std::atomic<long> badRows{0}, searches{0}, nonMonotonic{0};
    auto reader = [&](int mode) {
        std::uint64_t lastCount = 0;
        while (!done.load()) {
            db::DecodeQuery q;
            q.limit = 50;
            if (mode == 1) { q.text = "hello KJ4XYZ-3"; }
            if (mode == 2) { q.decoder = "aprs"; q.hasTime = true; q.fromMs = 1700000000000LL; q.toMs = 1700000000000LL + 40000 * 1000LL; }
            const auto rows = store.search(q);
            for (const auto& r : rows) {
                // every row that comes back is a complete, valid event that satisfies the query
                const std::uint64_t i = static_cast<std::uint64_t>((r.event.timeMs - 1700000000000LL) / 1000);
                if (!sameEvent(r.event, makeEvent(i))) { ++badRows; }
                if (mode == 1 && r.event.text.find("KJ4XYZ-3") == std::string::npos) { ++badRows; }
                if (mode == 2 && r.event.decoder != "APRS") { ++badRows; }
            }
            ++searches;
            if (mode == 0) {
                const std::uint64_t c = store.recordCount();
                (void)lastCount;
                (void)c;
            }
        }
    };
    std::vector<std::thread> readers;
    for (int m = 0; m < 3; ++m) { readers.emplace_back(reader, m); }
    std::thread retention([&] {
        while (!done.load()) {
            store.enforceRetention(0, db::Retention{0, 400u << 10});   // keep about 400 KiB: segments are dropped under the readers
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
    std::uint64_t written = 0;
    while (written < kTotal) {
        if (written % 3 == 0) {
            std::vector<db::DecodeEvent> b;
            for (int k = 0; k < 20 && written < kTotal; ++k) { b.push_back(makeEvent(written++)); }
            if (!store.appendBatch(b, &err)) { CHECK(false); break; }
        } else {
            if (!store.append(makeEvent(written++), &err)) { CHECK(false); break; }
        }
    }
    done.store(true);
    for (auto& t : readers) { t.join(); }
    retention.join();
    CHECK(badRows.load() == 0);
    CHECK(searches.load() > 20);
    store.enforceRetention(0, db::Retention{0, 400u << 10});   // the writer may have outrun the last pass
    const auto st = store.stats();
    std::printf("  concurrency: %llu records written, %ld searches by 3 readers during retention, %ld bad rows, %zu segments left\n",
                static_cast<unsigned long long>(kTotal), searches.load(), badRows.load(), st.segments);
    CHECK(st.bytes <= (400u << 10) + (64u << 10) * 2);
    // what remains is a gap-free suffix
    db::DecodeQuery q;
    q.limit = 1000000;
    q.newestFirst = false;
    const auto rows = store.search(q);
    CHECK(rows.size() == st.records);
    bool gapFree = !rows.empty();
    for (std::size_t k = 1; k < rows.size(); ++k) { if (rows[k].event.timeMs != rows[k - 1].event.timeMs + 1000) { gapFree = false; } }
    CHECK(gapFree);
    CHECK(!rows.empty() && sameEvent(rows.back().event, makeEvent(kTotal - 1)));
    store.close();
    wipe(d);
}

// ---- scale: a million records ----------------------------------------------------------------------------------------------------------

double seconds(Clock::time_point a) { return std::chrono::duration<double>(Clock::now() - a).count(); }

void testMillionRecords() {
    if (std::getenv("COREF_SKIP_BIG") != nullptr) { std::printf("SKIP million-record run: COREF_SKIP_BIG is set\n"); return; }
    constexpr std::uint64_t kN = 1000000;
    const fs::path d = dirFor("million");
    std::error_code ec;
    fs::create_directories(d, ec);
    if (fs::space(d, ec).available < (2ull << 30)) { std::printf("SKIP million-record run: under 2 GiB free\n"); return; }
    std::string err;
    db::DecodeDb store;
    db::DbOptions o;
    o.segmentBytes = 16u << 20;
    CHECK(store.open(d.string(), o, &err));
    // single appends (each flushed): the cost of making every event visible at once
    auto t0 = Clock::now();
    for (std::uint64_t i = 0; i < 100000; ++i) { if (!store.append(makeEvent(i), &err)) { CHECK(false); return; } }
    const double singleSec = seconds(t0);
    // the rest in batches of 1000
    t0 = Clock::now();
    std::vector<db::DecodeEvent> batch;
    for (std::uint64_t i = 100000; i < kN; ++i) {
        batch.push_back(makeEvent(i));
        if (batch.size() == 1000) { if (!store.appendBatch(batch, &err)) { CHECK(false); return; } batch.clear(); }
    }
    const double batchSec = seconds(t0);
    CHECK(store.recordCount() == kN);
    const auto st = store.stats();
    std::printf("  1M records: %.1f MB in %zu segments\n", static_cast<double>(st.bytes) / 1e6, st.segments);
    std::printf("  write: 100000 single flushed appends %.2f s (%.0f/s); 900000 in batches of 1000 %.2f s (%.0f/s)\n", singleSec, 100000 / singleSec, batchSec, 900000 / batchSec);
    auto timeQ = [&](const char* what, db::DecodeQuery q, std::size_t expect, bool exact) {
        t0 = Clock::now();
        const auto r = store.search(q);
        const double ms = seconds(t0) * 1000.0;
        std::printf("  search %-46s %8.1f ms, %zu rows\n", what, ms, r.size());
        if (exact) { CHECK(r.size() == expect); }
    };
    db::DecodeQuery q;
    q.limit = 100;
    timeQ("newest 100, no filter", q, 100, true);
    q.hasTime = true;
    q.fromMs = 1700000000000LL + 500000LL * 1000;
    q.toMs = q.fromMs + 99LL * 1000;
    timeQ("time range of 100 events (mid file)", q, 100, true);
    q.hasTime = false;
    q.hasFreq = true;
    q.freqMinHz = 100e6 + 500 * 25e3;
    q.freqMaxHz = q.freqMinHz;
    q.limit = 1000000;
    timeQ("one frequency (1000 events), all rows", q, 1000, true);
    q.hasFreq = false;
    q.limit = 100;
    q.decoder = "sonde";
    timeQ("decoder = sonde, newest 100", q, 100, true);
    q.decoder = "";
    q.limit = 1000000;
    q.text = "RAREWORD";
    timeQ("text RAREWORD (20 events in 1M), all rows", q, 20, true);
    q.text = "kj4xyz-33 hello";
    q.limit = 100;
    timeQ("text 'kj4xyz-33 hello', newest 100", q, 100, true);
    q.text = "kj4xyz-33";
    q.limit = 1000000;
    timeQ("text kj4xyz-33 (about 10300 events), all rows", q, 0, false);
    q.text = "no-such-text-anywhere";
    timeQ("text with no match anywhere (full scan)", q, 0, true);
    store.close();
    // reopen: sealed segments from their index files
    t0 = Clock::now();
    db::DecodeDb again;
    CHECK(again.open(d.string(), o, &err));
    const double openIdx = seconds(t0);
    CHECK(again.recordCount() == kN);
    std::printf("  reopen with index files: %.3f s (%zu indexes loaded, %zu segments scanned)\n", openIdx, again.stats().indexesLoaded, again.stats().segmentsScanned);
    again.close();
    for (const auto& e : fs::directory_iterator(d)) { if (e.path().extension() == ".fxi") { fs::remove(e.path()); } }
    t0 = Clock::now();
    db::DecodeDb scan;
    CHECK(scan.open(d.string(), o, &err));
    const double openScan = seconds(t0);
    CHECK(scan.recordCount() == kN);
    std::printf("  reopen by scanning every segment (no index files): %.3f s (%zu segments scanned)\n", openScan, scan.stats().segmentsScanned);
    scan.close();
    wipe(d);
    CHECK(!fs::exists(d));
}

}  // namespace

int main() {
    std::printf("decode database\n");
    testBasicsAndFilters();
    testRandomizedAgainstReference();
    testPersistenceAndIndex();
    testTornWrites();
    testDamageUnderIndex();
    testBadFilesAndOpenRefusals();
    testCaps();
    testDecoderHashCollision();
    testRetention();
    testConcurrentWriterReadersRetention();
    testMillionRecords();
    return testSummary("test_decode_db");
}
