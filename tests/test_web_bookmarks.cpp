// Tests for the browser's personal bookmark sets (net/web_bookmarks.hpp).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "net/web_bookmarks.hpp"
#include "test_check.hpp"

#ifndef CASCADE_SOURCE_DIR
#define CASCADE_SOURCE_DIR "."
#endif

using namespace cascade::net;

namespace {

const std::string kA = "viewerAAAAAAAA";
const std::string kB = "viewer-BBBB_bbbb";

WebBookmark bm(const std::string& name, double hz, const std::string& mode = "NFM", const std::string& group = "") {
    WebBookmark b;
    b.name = name;
    b.freqHz = hz;
    b.mode = mode;
    b.group = group;
    return b;
}

void testAddListHappyPath() {
    WebBookmarkBook book;
    std::uint64_t id1 = 0, id2 = 0;
    CHECK(book.add(kA, bm("BBC R4", 198000.0, "am", "Broadcast"), &id1) == BookmarkResult::Ok);
    CHECK(book.add(kA, bm("Airband", 118.1e6, "AM", "Air"), &id2) == BookmarkResult::Ok);
    CHECK(id1 != 0 && id2 != 0 && id1 != id2);
    const auto l = book.list(kA);
    CHECK(l.size() == 2);
    // ordered by group (case-insensitive), then frequency
    CHECK(l.size() == 2 && l[0].name == "Airband" && l[1].name == "BBC R4");
    CHECK(l.size() == 2 && l[1].mode == "AM");  // normalised to upper case
    CHECK(book.size(kA) == 2);
    CHECK(book.viewerCount() == 1);
    // another viewer does not see them
    CHECK(book.list(kB).empty());
    CHECK(book.viewerCount() == 1);  // a read never creates a viewer
    CHECK(book.remove(kA, id1) == BookmarkResult::Ok);
    CHECK(book.size(kA) == 1);
    CHECK(book.remove(kA, id1) == BookmarkResult::NotFound);
    CHECK(book.remove(kB, id2) == BookmarkResult::NotFound);  // someone else's id is not found for B
    CHECK(book.size(kA) == 1);
}

void testPersonalSetsAreIsolated() {
    WebBookmarkBook book;
    std::uint64_t a = 0, b = 0;
    CHECK(book.add(kA, bm("Same", 100e6), &a) == BookmarkResult::Ok);
    CHECK(book.add(kB, bm("Same", 100e6), &b) == BookmarkResult::Ok);  // not a duplicate across viewers
    CHECK(a != b);
    CHECK(book.remove(kB, a) == BookmarkResult::NotFound);  // B cannot delete A's bookmark by id
    CHECK(book.size(kA) == 1 && book.size(kB) == 1);
    CHECK(book.update(kB, a, bm("Hijack", 1e6)) == BookmarkResult::NotFound);
    CHECK(book.list(kA)[0].name == "Same");
    book.forgetViewer(kA);
    CHECK(book.list(kA).empty() && book.size(kB) == 1);
    CHECK(book.viewerCount() == 1);
}

void testValidationRefusals() {
    WebBookmarkBook book;
    CHECK(book.add("short", bm("x", 1e6)) == BookmarkResult::BadToken);
    CHECK(book.add("has space token", bm("x", 1e6)) == BookmarkResult::BadToken);
    CHECK(book.add(std::string(129, 'a'), bm("x", 1e6)) == BookmarkResult::BadToken);
    CHECK(book.add("", bm("x", 1e6)) == BookmarkResult::BadToken);
    CHECK(book.add(kA, bm("", 1e6)) == BookmarkResult::BadName);
    CHECK(book.add(kA, bm("   ", 1e6)) == BookmarkResult::BadName);
    CHECK(book.add(kA, bm(std::string(kMaxBookmarkName + 1, 'n'), 1e6)) == BookmarkResult::BadName);
    CHECK(book.add(kA, bm(std::string(kMaxBookmarkName, 'n'), 1e6)) == BookmarkResult::Ok);   // the boundary itself
    CHECK(book.add(kA, bm("tab\there", 1e6)) == BookmarkResult::BadName);
    CHECK(book.add(kA, bm(std::string("nul\0x", 5), 1e6)) == BookmarkResult::BadName);
    CHECK(book.add(kA, bm("bad\xC0\xAF utf8", 1e6)) == BookmarkResult::BadName);              // overlong
    CHECK(book.add(kA, bm("surrogate\xED\xA0\x80", 1e6)) == BookmarkResult::BadName);
    CHECK(book.add(kA, bm("caf\xC3\xA9 ok", 2e6)) == BookmarkResult::Ok);                     // real UTF-8 is fine
    CHECK(book.add(kA, bm("f", 0.0)) == BookmarkResult::BadFrequency);
    CHECK(book.add(kA, bm("f", -5.0)) == BookmarkResult::BadFrequency);
    CHECK(book.add(kA, bm("f", std::numeric_limits<double>::quiet_NaN())) == BookmarkResult::BadFrequency);
    CHECK(book.add(kA, bm("f", std::numeric_limits<double>::infinity())) == BookmarkResult::BadFrequency);
    CHECK(book.add(kA, bm("f", kMaxBookmarkHz * 2)) == BookmarkResult::BadFrequency);
    CHECK(book.add(kA, bm("f", 5e6, "FM")) == BookmarkResult::BadMode);
    CHECK(book.add(kA, bm("f", 5e6, "")) == BookmarkResult::BadMode);
    WebBookmark b = bm("bw", 5e6);
    b.bandwidthHz = 1.0;
    CHECK(book.add(kA, b) == BookmarkResult::BadBandwidth);
    b.bandwidthHz = 1e9;
    CHECK(book.add(kA, b) == BookmarkResult::BadBandwidth);
    b.bandwidthHz = std::numeric_limits<double>::quiet_NaN();
    CHECK(book.add(kA, b) == BookmarkResult::BadBandwidth);
    WebBookmark g = bm("g", 6e6);
    g.group = std::string(kMaxBookmarkGroup + 1, 'g');
    CHECK(book.add(kA, g) == BookmarkResult::BadGroup);
    g.group = std::string(kMaxBookmarkGroup, 'g');
    CHECK(book.add(kA, g) == BookmarkResult::Ok);
    WebBookmark t = bm("t", 7e6);
    t.tags = {"ok", "Bad Tag"};
    CHECK(book.add(kA, t) == BookmarkResult::BadTag);
    t.tags = {"", "x"};
    CHECK(book.add(kA, t) == BookmarkResult::BadTag);
    t.tags = {std::string(kMaxBookmarkTag + 1, 't')};
    CHECK(book.add(kA, t) == BookmarkResult::BadTag);
    t.tags = {std::string(kMaxBookmarkTag, 't')};
    CHECK(book.add(kA, t) == BookmarkResult::Ok);
    t = bm("t2", 7.1e6);
    for (std::size_t i = 0; i <= kMaxBookmarkTags; ++i) { t.tags.push_back("tag" + std::to_string(i)); }
    CHECK(book.add(kA, t) == BookmarkResult::TooManyTags);
    t.tags.pop_back();
    CHECK(book.add(kA, t) == BookmarkResult::Ok);
    // tags are lower-cased and de-duplicated, in the order given
    WebBookmark d = bm("d", 8e6);
    d.tags = {"Net", "net", "AIR", "air", "net"};
    std::uint64_t id = 0;
    CHECK(book.add(kA, d, &id) == BookmarkResult::Ok);
    for (const WebBookmark& e : book.list(kA)) {
        if (e.id == id) { CHECK((e.tags == std::vector<std::string>{"net", "air"})); }
    }
    // a refused add changed nothing
    const std::size_t before = book.size(kA);
    CHECK(book.add(kA, bm("", 1e6)) == BookmarkResult::BadName);
    CHECK(book.size(kA) == before);
    // refused text never made a viewer for a viewer that has only refusals
    CHECK(book.add(kB, bm("", 1e6)) == BookmarkResult::BadName);
    CHECK(book.viewerCount() == 1);
}

void testDuplicateAndUpdate() {
    WebBookmarkBook book;
    std::uint64_t id = 0, id2 = 0;
    CHECK(book.add(kA, bm("X", 1e6, "AM"), &id) == BookmarkResult::Ok);
    CHECK(book.add(kA, bm("X", 1e6, "am")) == BookmarkResult::Duplicate);
    CHECK(book.add(kA, bm("X", 1e6, "USB"), &id2) == BookmarkResult::Ok);   // another mode is another bookmark
    CHECK(book.update(kA, id, bm("X", 1e6, "USB")) == BookmarkResult::Duplicate);
    CHECK(book.update(kA, id, bm("X2", 1.5e6, "CW", "g")) == BookmarkResult::Ok);
    bool found = false;
    for (const WebBookmark& e : book.list(kA)) {
        if (e.id == id) { found = true; CHECK(e.name == "X2" && e.freqHz == 1.5e6 && e.mode == "CW" && e.group == "g"); }
    }
    CHECK(found);
    CHECK(book.update(kA, 99999, bm("n", 1e6)) == BookmarkResult::NotFound);
    CHECK(book.update(kA, id, bm("", 1e6)) == BookmarkResult::BadName);
    // updating to itself is not a duplicate
    CHECK(book.update(kA, id2, bm("X", 1e6, "USB")) == BookmarkResult::Ok);
}

void testCaps() {
    WebBookmarkBook book;
    for (std::size_t i = 0; i < kMaxBookmarksPerViewer; ++i) {
        CHECK(book.add(kA, bm("n" + std::to_string(i), 1e6 + static_cast<double>(i))) == BookmarkResult::Ok);
    }
    CHECK(book.size(kA) == kMaxBookmarksPerViewer);
    CHECK(book.add(kA, bm("one more", 9e9)) == BookmarkResult::TooManyBookmarks);
    CHECK(book.size(kA) == kMaxBookmarksPerViewer);
    // another viewer is unaffected
    CHECK(book.add(kB, bm("fine", 1e6)) == BookmarkResult::Ok);
    // the viewer cap
    WebBookmarkBook many;
    for (std::size_t i = 0; i < kMaxBookmarkViewers; ++i) {
        CHECK(many.add("viewer" + std::to_string(1000 + i), bm("a", 1e6)) == BookmarkResult::Ok);
    }
    CHECK(many.add("viewer-one-too-many", bm("a", 1e6)) == BookmarkResult::TooManyViewers);
    CHECK(many.viewerCount() == kMaxBookmarkViewers);
    many.forgetViewer("viewer1000");
    CHECK(many.add("viewer-one-too-many", bm("a", 1e6)) == BookmarkResult::Ok);   // a freed slot is reusable
    many.clear();
    CHECK(many.viewerCount() == 0);
}

void testSearch() {
    WebBookmarkBook book;
    WebBookmark a = bm("Heathrow Tower", 118.7e6, "AM", "Airband");
    a.tags = {"uk", "tower"};
    WebBookmark b = bm("Gatwick Approach", 126.82e6, "AM", "Airband");
    b.tags = {"uk"};
    WebBookmark c = bm("Radio Caroline", 648e3, "AM", "Broadcast");
    WebBookmark d = bm("Marine ch16", 156.8e6, "NFM", "marine");
    d.tags = {"distress", "uk"};
    for (const WebBookmark& x : {a, b, c, d}) { CHECK(book.add(kA, x) == BookmarkResult::Ok); }
    BookmarkQuery q;
    CHECK(book.search(kA, q).size() == 4);                      // empty query = everything
    q.text = "tower";
    CHECK(book.search(kA, q).size() == 1);                      // name and tag both hit the same entry
    q.text = "AIRBAND";
    CHECK(book.search(kA, q).size() == 2);                      // case-insensitive, group matches text
    q.text = "uk";
    CHECK(book.search(kA, q).size() == 3);                      // a tag matches text
    q = BookmarkQuery{};
    q.group = "AIRBAND";
    CHECK(book.search(kA, q).size() == 2);
    q.group = "air";
    CHECK(book.search(kA, q).empty());                          // group is exact, not a substring
    q = BookmarkQuery{};
    q.mode = "nfm";
    CHECK(book.search(kA, q).size() == 1);
    q.mode = "bogus";
    CHECK(book.search(kA, q).empty());
    q = BookmarkQuery{};
    q.tag = "UK";
    CHECK(book.search(kA, q).size() == 3);
    q.tag = "uk";
    q.minHz = 120e6;
    q.maxHz = 160e6;
    CHECK(book.search(kA, q).size() == 2);                      // Gatwick and marine
    q = BookmarkQuery{};
    q.text = std::string(kMaxSearchQuery + 1, 'q');
    CHECK(book.search(kA, q).empty());
    q = BookmarkQuery{};
    q.text = "no-such-thing";
    CHECK(book.search(kA, q).empty());
    CHECK(book.search(kB, BookmarkQuery{}).empty());            // someone else's set is empty
    // text is a plain substring, never a pattern
    q.text = ".*";
    CHECK(book.search(kA, q).empty());
    const auto g = book.groups(kA);
    CHECK((g == std::vector<std::string>{"Airband", "Broadcast", "marine"}));
}

void testExportImportRoundTrip() {
    WebBookmarkBook book;
    WebBookmark a = bm("Alpha", 7.1e6, "LSB", "HF");
    a.tags = {"ham", "40m"};
    a.bandwidthHz = 2400.0;
    CHECK(book.add(kA, a) == BookmarkResult::Ok);
    CHECK(book.add(kA, bm("Beta \"quoted\" \\ \xC3\xA9", 14.2e6, "USB")) == BookmarkResult::Ok);
    const std::string json = book.exportJson(kA);
    // the token is never in the file
    CHECK(json.find(kA) == std::string::npos);
    WebBookmarkBook other;
    const BookmarkImportResult r = other.importJson(kB, json, false);
    CHECK(r.result == BookmarkResult::Ok && r.imported == 2 && r.skipped == 0);
    const auto x = book.list(kA);
    const auto y = other.list(kB);
    CHECK(x.size() == y.size());
    for (std::size_t i = 0; i < std::min(x.size(), y.size()); ++i) {
        CHECK(x[i].name == y[i].name && x[i].freqHz == y[i].freqHz && x[i].mode == y[i].mode && x[i].bandwidthHz == y[i].bandwidthHz &&
              x[i].group == y[i].group && x[i].tags == y[i].tags);
    }
    // importing the same file again: all duplicates, skipped, nothing doubled
    const BookmarkImportResult again = other.importJson(kB, json, false);
    CHECK(again.imported == 0 && again.skipped == 2 && other.size(kB) == 2);
    // replace clears first
    const BookmarkImportResult rep = other.importJson(kB, json, true);
    CHECK(rep.imported == 2 && other.size(kB) == 2);
    // an empty set exports as an empty array
    CHECK(WebBookmarkBook().exportJson(kA).find("\"bookmarks\":[]") != std::string::npos);
}

void testImportRefusals() {
    WebBookmarkBook book;
    CHECK(book.add(kA, bm("keep", 1e6)) == BookmarkResult::Ok);
    // not understood: nothing changes, even with replace
    CHECK(book.importJson(kA, "not json at all", true).result == BookmarkResult::BadJson);
    CHECK(book.importJson(kA, "{}", true).result == BookmarkResult::BadJson);
    CHECK(book.importJson(kA, "{\"bookmarks\":5}", true).result == BookmarkResult::BadJson);
    CHECK(book.importJson(kA, "42", true).result == BookmarkResult::BadJson);
    CHECK(book.importJson(kA, "", true).result == BookmarkResult::BadJson);
    CHECK(book.size(kA) == 1);
    CHECK(book.importJson("bad", "[]", false).result == BookmarkResult::BadToken);
    CHECK(book.importJson(kA, std::string(kMaxBookmarkImportBytes + 1, ' '), false).result == BookmarkResult::TooLarge);
    // mixed good and bad entries: the good ones count, the bad ones are skipped
    const std::string mixed =
        "[{\"name\":\"ok1\",\"freqHz\":1000000},"
        "{\"name\":\"\",\"freqHz\":2000000},"                       // bad name
        "{\"name\":\"nofreq\"},"                                    // missing freq
        "{\"name\":\"strfreq\",\"freqHz\":\"1e6\"},"                // wrong type
        "{\"name\":\"badmode\",\"freqHz\":3000000,\"mode\":\"FM\"},"
        "{\"name\":\"badtags\",\"freqHz\":4000000,\"tags\":[1,2]},"
        "{\"name\":\"badtags2\",\"freqHz\":4000001,\"tags\":\"x\"},"
        "17,null,[],"
        "{\"name\":\"ok2\",\"freqHz\":5000000,\"mode\":\"usb\",\"group\":\"g\",\"tags\":[\"a\"],\"bandwidthHz\":2700}]";
    const BookmarkImportResult m = book.importJson(kA, mixed, false);
    CHECK(m.result == BookmarkResult::Ok && m.imported == 2 && m.skipped == 9);
    CHECK(book.size(kA) == 3);
    // a bare array and the wrapper object are both accepted
    WebBookmarkBook b2;
    CHECK(b2.importJson(kA, "{\"version\":1,\"bookmarks\":[{\"name\":\"w\",\"freqHz\":1e6}]}", false).imported == 1);
    // entries past the per-viewer cap are skipped, not an error for the file
    std::string big = "[";
    for (std::size_t i = 0; i < kMaxBookmarksPerViewer + 10; ++i) {
        if (i) { big += ","; }
        big += "{\"name\":\"n" + std::to_string(i) + "\",\"freqHz\":" + std::to_string(1000000 + i) + "}";
    }
    big += "]";
    WebBookmarkBook b3;
    const BookmarkImportResult c = b3.importJson(kA, big, false);
    CHECK(c.imported == kMaxBookmarksPerViewer && c.skipped == 10 && b3.size(kA) == kMaxBookmarksPerViewer);
    // deeply nested junk does not blow the stack or throw (nlohmann's parser is recursive: the byte cap bounds it)
    std::string deep(100000, '[');
    CHECK(book.importJson(kA, deep, false).result == BookmarkResult::BadJson);
}

// FUZZ: every parse of outside bytes ends in a result, never a throw or a crash, and the set stays inside its caps.
void testFuzzImport() {
    std::mt19937_64 rng(777);
    const std::vector<std::string> seeds = {
        "[{\"name\":\"a\",\"freqHz\":1e6,\"mode\":\"AM\",\"group\":\"g\",\"tags\":[\"x\",\"y\"],\"bandwidthHz\":5000}]",
        "{\"version\":1,\"bookmarks\":[{\"name\":\"caf\\u00e9\",\"freqHz\":7100000}]}",
        "[{\"name\":\"\\ud800\",\"freqHz\":1}]",
        "[{\"name\":\"a\",\"freqHz\":1e999}]",
        "[{\"name\":\"a\",\"freqHz\":-0}]",
    };
    WebBookmarkBook book;
    int crashes = 0;
    for (int i = 0; i < 6000; ++i) {
        std::string s = seeds[rng() % seeds.size()];
        const int ops = static_cast<int>(rng() % 6);
        for (int k = 0; k < ops; ++k) {
            if (s.empty()) { break; }
            const std::size_t pos = rng() % s.size();
            switch (rng() % 4) {
                case 0: s[pos] = static_cast<char>(rng()); break;
                case 1: s.erase(pos, 1 + rng() % 4); break;
                case 2: s.insert(pos, 1, static_cast<char>(rng())); break;
                default: s.resize(pos); break;
            }
        }
        try {
            const BookmarkImportResult r = book.importJson(kB, s, (i % 7) == 0);
            (void)r;
            const std::string out = book.exportJson(kB);
            (void)out;
        } catch (...) {
            ++crashes;
        }
        if (book.size(kB) > kMaxBookmarksPerViewer) { ++crashes; }
    }
    CHECK(crashes == 0);
    // pure random bytes too
    for (int i = 0; i < 2000; ++i) {
        std::string s(rng() % 200, '\0');
        for (char& c : s) { c = static_cast<char>(rng()); }
        try { book.importJson(kB, s, false); } catch (...) { ++crashes; }
    }
    CHECK(crashes == 0);
    // whatever survived is valid per the book's own rules
    for (const WebBookmark& b : book.list(kB)) {
        CHECK(validBookmarkText(b.name, kMaxBookmarkName) && b.freqHz >= kMinBookmarkHz && b.freqHz <= kMaxBookmarkHz);
    }
}

void testConcurrency() {
    WebBookmarkBook book;
    std::atomic<int> failures{0};
    std::vector<std::thread> ts;
    for (int t = 0; t < 8; ++t) {
        ts.emplace_back([&, t] {
            const std::string tok = "thread-viewer-" + std::to_string(t % 4);  // four threads share a viewer on purpose
            for (int i = 0; i < 300; ++i) {
                std::uint64_t id = 0;
                const BookmarkResult r = book.add(tok, bm("n" + std::to_string(t) + "_" + std::to_string(i), 1e6 + i * 10.0 + t), &id);
                if (r != BookmarkResult::Ok && r != BookmarkResult::TooManyBookmarks) { ++failures; }
                if (r == BookmarkResult::Ok && (i % 3) == 0) { book.remove(tok, id); }
                (void)book.search(tok, BookmarkQuery{});
                if ((i % 50) == 0) { (void)book.exportJson(tok); }
            }
        });
    }
    for (auto& th : ts) { th.join(); }
    CHECK(failures.load() == 0);
    for (int v = 0; v < 4; ++v) {
        const std::string tok = "thread-viewer-" + std::to_string(v);
        const auto l = book.list(tok);
        CHECK(l.size() <= kMaxBookmarksPerViewer);
        std::vector<std::uint64_t> ids;
        for (const auto& b : l) { ids.push_back(b.id); }
        std::sort(ids.begin(), ids.end());
        CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());   // ids unique
    }
}

// SOURCE SCAN: identifiers of people never reach a stream or a file from this module.
void testSourceNeverPrintsOrWrites() {
    std::ifstream in(std::string(CASCADE_SOURCE_DIR) + "/src/net/web_bookmarks.cpp", std::ios::binary);
    CHECK(static_cast<bool>(in));
    const std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(!src.empty());
    for (const char* banned : {"printf", "std::cout", "std::cerr", "fopen", "ofstream", "fstream", "fwrite", "OutputDebugString", "stderr", "stdout",
                               "settings_set", "getenv"}) {
        CHECK(src.find(banned) == std::string::npos);
    }
}

}  // namespace

int main() {
    testAddListHappyPath();
    testPersonalSetsAreIsolated();
    testValidationRefusals();
    testDuplicateAndUpdate();
    testCaps();
    testSearch();
    testExportImportRoundTrip();
    testImportRefusals();
    testFuzzImport();
    testConcurrency();
    testSourceNeverPrintsOrWrites();
    return testSummary("test_web_bookmarks");
}
