// test_transcript_store.cpp - the transcript record model (core/transcript_store.hpp): timestamped segments of words with a confidence each, bounded.
//
//   T1  storing and reading back: words, confidence, times, radio, frequency, language, translated flag; plain text; export text.
//   T2  confidence classes and the clamps (NaN, below 0, above 1, the class edges, custom thresholds, odd thresholds).
//   T3  sanitising: control characters, long words, empty words, 256-word cap, bad times/radio/frequency; refusals counted.
//   T4  the bounds: segments, words and bytes each force the OLDEST segments out; a segment bigger than the store is refused; counts stay exact.
//   T5  search (case, limit, order, empty and over-long needle); clear.
//   T6  fuzz against a reference model for the counts and the order.
//   T7  source scan: no file, no network, nothing printed.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "core/transcript_store.hpp"
#include "hamops_util.hpp"
#include "test_check.hpp"

using namespace cascade::core::speech;

namespace {

Segment seg(std::int64_t t, std::vector<Word> w) {
    Segment s;
    s.startMs = t;
    s.endMs = t + 1000;
    s.radio = 2;
    s.freqHz = 7100000;
    s.language = "en";
    s.words = std::move(w);
    return s;
}

void t1Basics() {
    TranscriptStore st;
    Segment s = seg(1000, {{"CQ", 0.95f}, {"contest", 0.7f}, {"de", 0.4f}, {"test", 1.0f}});
    s.translated = true;
    CHECK(st.add(s) == AddResult::Added);
    CHECK(st.segmentCount() == 1 && st.wordCount() == 4 && st.byteCount() == 2 + 7 + 2 + 4);
    const Segment& r = st.segments().front();
    CHECK(r.startMs == 1000 && r.endMs == 2000 && r.radio == 2 && r.freqHz == 7100000 && r.language == "en" && r.translated);
    CHECK(r.words.size() == 4 && r.words[1].text == "contest" && r.words[1].confidence == 0.7f);
    CHECK(TranscriptStore::plain(r) == "CQ contest de test");
    CHECK(st.exportText() == "1000 7100000 CQ contest de test" + std::string(1, static_cast<char>(10)));
    CHECK(st.refused() == 0 && st.evicted() == 0);
    // order is arrival order
    st.add(seg(500, {{"older", 1.0f}}));
    CHECK(st.segments().back().words[0].text == "older");
}

void t2Confidence() {
    TranscriptStore st;
    CHECK(st.classify(1.0f) == ConfClass::High && st.classify(0.80f) == ConfClass::High && st.classify(0.7999f) == ConfClass::Medium);
    CHECK(st.classify(0.50f) == ConfClass::Medium && st.classify(0.4999f) == ConfClass::Low && st.classify(0.0f) == ConfClass::Low);
    CHECK(st.classify(-3.0f) == ConfClass::Low && st.classify(7.0f) == ConfClass::High);
    // stored confidence is clamped; NaN becomes 0
    Segment s = seg(1, {{"a", -0.5f}, {"b", 1.5f}, {"c", std::numeric_limits<float>::quiet_NaN()}, {"d", std::numeric_limits<float>::infinity()}, {"e", -std::numeric_limits<float>::infinity()}});
    st.add(s);
    const auto& w = st.segments().front().words;
    CHECK(w.size() == 5 && w[0].confidence == 0.0f && w[1].confidence == 1.0f && w[2].confidence == 0.0f && w[3].confidence == 1.0f && w[4].confidence == 0.0f);
    // custom thresholds, and odd ones are repaired
    StoreConfig c;
    c.highAt = 0.9f;
    c.mediumAt = 0.6f;
    TranscriptStore a(c);
    CHECK(a.classify(0.85f) == ConfClass::Medium && a.classify(0.95f) == ConfClass::High && a.classify(0.55f) == ConfClass::Low);
    c.highAt = 2.0f;                                                          // out of range: default
    TranscriptStore b(c);
    CHECK(b.classify(0.8f) == ConfClass::High);
    c = StoreConfig(); c.highAt = 0.3f; c.mediumAt = 0.5f;                    // medium above high: pulled down to high
    TranscriptStore d(c);
    CHECK(d.classify(0.3f) == ConfClass::High && d.classify(0.29f) == ConfClass::Low);
    c = StoreConfig(); c.mediumAt = std::numeric_limits<float>::quiet_NaN();
    TranscriptStore e(c);
    CHECK(e.classify(0.6f) == ConfClass::Medium && e.classify(0.4f) == ConfClass::Low);
}

void t3Sanitising() {
    TranscriptStore st;
    st.add(seg(1, {{"  spaced  ", 0.5f}, {std::string("tab\there"), 0.5f}, {std::string("nl\nx\r\n"), 0.5f}, {"", 0.5f}, {"   ", 0.5f}, {std::string(100, 'x'), 0.5f}, {std::string("\x01\x02", 2), 0.5f}}));
    const auto& w = st.segments().front().words;
    CHECK(w.size() == 4);
    CHECK(w[0].text == "spaced" && w[1].text == "tab here" && w[2].text == "nl x" && w[3].text == std::string(48, 'x'));
    for (const auto& x : w) { for (char c : x.text) { CHECK(static_cast<unsigned char>(c) >= 32 && static_cast<unsigned char>(c) != 127); } }
    // 256-word cap
    std::vector<Word> many(300, Word{"w", 0.5f});
    TranscriptStore b;
    b.add(seg(1, many));
    CHECK(b.wordCount() == 256);
    // refusals
    TranscriptStore r;
    CHECK(r.add(seg(1, {})) == AddResult::Refused);
    CHECK(r.add(seg(1, {{"", 1.0f}})) == AddResult::Refused);
    Segment bad = seg(1, {{"x", 1.0f}});
    bad.startMs = -1;
    CHECK(r.add(bad) == AddResult::Refused);
    bad = seg(1, {{"x", 1.0f}}); bad.endMs = 0;
    CHECK(r.add(bad) == AddResult::Refused);
    bad = seg(1, {{"x", 1.0f}}); bad.radio = 9;
    CHECK(r.add(bad) == AddResult::Refused);
    bad = seg(1, {{"x", 1.0f}}); bad.radio = -1;
    CHECK(r.add(bad) == AddResult::Refused);
    CHECK(r.refused() == 6 && r.segmentCount() == 0 && r.wordCount() == 0 && r.byteCount() == 0);
    // frequency: bad values become 0, the segment is kept; language is clipped and cleaned
    Segment f = seg(1, {{"x", 1.0f}});
    f.freqHz = std::nan("");
    f.language = "enGB-longer\n";
    r.add(f);
    f.freqHz = -5;
    r.add(f);
    f.freqHz = 1e13;
    r.add(f);
    CHECK(r.segmentCount() == 3);
    for (const Segment& s : r.segments()) { CHECK(s.freqHz == 0.0 && s.language == "enGB-lon"); }
    // zero-length segment (end == start) is fine
    Segment z = seg(5, {{"x", 1.0f}});
    z.endMs = 5;
    CHECK(r.add(z) == AddResult::Added);
}

void t4Bounds() {
    StoreConfig c;
    c.maxSegments = 5;
    TranscriptStore a(c);
    for (int i = 0; i < 12; ++i) { a.add(seg(i, {{"w" + std::to_string(i), 1.0f}})); CHECK(a.segmentCount() <= 5); }
    CHECK(a.segmentCount() == 5 && a.evicted() == 7 && a.segments().front().startMs == 7 && a.segments().back().startMs == 11);
    // words bound
    c = StoreConfig();
    c.maxWords = 10;
    TranscriptStore b(c);
    for (int i = 0; i < 20; ++i) { b.add(seg(i, {{"a", 1.0f}, {"b", 1.0f}, {"c", 1.0f}})); CHECK(b.wordCount() <= 10); }
    CHECK(b.wordCount() == 9 && b.segmentCount() == 3 && b.segments().back().startMs == 19);
    // bytes bound
    c = StoreConfig();
    c.maxBytes = 100;
    TranscriptStore d(c);
    for (int i = 0; i < 30; ++i) { d.add(seg(i, {{std::string(20, 'x'), 1.0f}, {std::string(10, 'y'), 1.0f}})); CHECK(d.byteCount() <= 100); }
    CHECK(d.byteCount() == 90 && d.segmentCount() == 3);
    // one segment bigger than the whole store is refused and leaves the store alone
    const std::size_t before = d.segmentCount();
    Segment big = seg(99, {{std::string(48, 'z'), 1.0f}, {std::string(48, 'z'), 1.0f}, {std::string(48, 'z'), 1.0f}});
    CHECK(d.add(big) == AddResult::Refused && d.segmentCount() == before && d.refused() == 1);
    c = StoreConfig();
    c.maxWords = 2;
    TranscriptStore e(c);
    CHECK(e.add(seg(1, {{"a", 1.0f}, {"b", 1.0f}, {"c", 1.0f}})) == AddResult::Refused);
    // limits that are out of range are repaired
    c = StoreConfig();
    c.maxSegments = 0; c.maxWords = 0; c.maxBytes = 0;
    TranscriptStore g(c);
    CHECK(g.add(seg(1, {{"x", 1.0f}})) == AddResult::Added && g.add(seg(2, {{"y", 1.0f}})) == AddResult::Added && g.segmentCount() == 1 && g.segments().front().startMs == 2);
    // memory behaviour: a very long run stays within the bounds
    c = StoreConfig();
    TranscriptStore h(c);
    for (int i = 0; i < 20000; ++i) { h.add(seg(i, {{"hello", 0.9f}, {"world", 0.8f}, {std::to_string(i), 0.7f}})); }
    CHECK(h.segmentCount() <= c.maxSegments && h.wordCount() <= c.maxWords && h.byteCount() <= c.maxBytes);
}

void t5Search() {
    TranscriptStore st;
    st.add(seg(1, {{"Alpha", 1.0f}, {"BRAVO", 1.0f}}));
    st.add(seg(2, {{"charlie", 1.0f}}));
    st.add(seg(3, {{"alpha", 1.0f}, {"delta", 1.0f}}));
    auto r = st.search("ALPHA");
    CHECK(r.size() == 2 && r[0]->startMs == 3 && r[1]->startMs == 1);          // newest first, case-insensitive
    CHECK(st.search("alpha", 1).size() == 1);
    CHECK(st.search("bravo delta").empty());                                    // words joined by one space: "bravo" and "delta" are in different segments
    CHECK(st.search("alpha delta").size() == 1);
    CHECK(st.search("").empty() && st.search(std::string(65, 'a')).empty() && st.search("zulu").empty());
    CHECK(st.search("alpha", 0).empty());
    st.clear();
    CHECK(st.segmentCount() == 0 && st.wordCount() == 0 && st.byteCount() == 0 && st.exportText().empty() && st.search("alpha").empty());
}

void t6Fuzz() {
    hamops_test::Rng rng(66);
    for (int run = 0; run < 50; ++run) {
        StoreConfig c;
        c.maxSegments = 1 + rng.below(30);
        c.maxWords = 1 + rng.below(100);
        c.maxBytes = 64 + rng.below(400);
        TranscriptStore st(c);
        // reference: a vector of (words, bytes)
        struct Ref { std::size_t words, bytes; std::int64_t t; };
        std::vector<Ref> ref;
        std::size_t rw = 0, rb = 0;
        const std::size_t maxSeg = std::min<std::size_t>(c.maxSegments, 100000), maxW = c.maxWords, maxB = std::max<std::size_t>(64, c.maxBytes);
        for (int i = 0; i < 300; ++i) {
            std::vector<Word> w;
            const int n = static_cast<int>(rng.below(12));
            std::size_t bytes = 0, words = 0;
            for (int k = 0; k < n; ++k) {
                const std::size_t len = rng.below(60);
                std::string t(len, 'a' + static_cast<char>(rng.below(26)));
                if (rng.below(8) == 0) { t = std::string(1, static_cast<char>(rng.below(32))); }
                w.push_back({t, static_cast<float>(rng.below(1200)) / 1000.0f - 0.1f});
                std::string cl;
                for (char ch : t) { cl.push_back(static_cast<unsigned char>(ch) < 32 ? ' ' : ch); }
                if (cl.size() > 48) { cl.resize(48); }
                const std::size_t a = cl.find_first_not_of(' ');
                if (a == std::string::npos) { continue; }
                const std::size_t b = cl.find_last_not_of(' ');
                if (words < 256) { ++words; bytes += b - a + 1; }
            }
            const bool refuse = words == 0 || words > maxW || bytes > maxB;
            const AddResult r = st.add(seg(i, w));
            CHECK((r == AddResult::Refused) == refuse);
            if (refuse) { continue; }
            ref.push_back({words, bytes, i});
            rw += words; rb += bytes;
            while (ref.size() > maxSeg || rw > maxW || rb > maxB) { rw -= ref.front().words; rb -= ref.front().bytes; ref.erase(ref.begin()); }
            CHECK(st.segmentCount() == ref.size() && st.wordCount() == rw && st.byteCount() == rb);
            CHECK(st.segments().front().startMs == ref.front().t && st.segments().back().startMs == ref.back().t);
        }
    }
}

void t7SourceScan() {
    const std::string src = hamops_test::stripCommentsAndStrings(hamops_test::srcFile("src/core/transcript_store.hpp"));
    CHECK(!src.empty());
    for (const char* bad : {"fopen", "ofstream", "ifstream", "fstream", "filesystem", "CreateFile", "socket", "winsock", "stderr", "stdout", "std::cerr", "std::cout", "settings_set", "std::thread",
                            "OutputDebugString"}) {
        CHECK(src.find(bad) == std::string::npos);
    }
}

}  // namespace

int main() {
    t1Basics();
    t2Confidence();
    t3Sanitising();
    t4Bounds();
    t5Search();
    t6Fuzz();
    t7SourceScan();
    return testSummary("test_transcript_store");
}
