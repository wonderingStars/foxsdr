// Tests for the plugin store's sort orders and release-note history (core/store_sort.hpp).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "core/plugin_repo.hpp"
#include "core/store_sort.hpp"
#include "test_check.hpp"

#ifndef CASCADE_SOURCE_DIR
#define CASCADE_SOURCE_DIR "."
#endif

using namespace cascade::core;

namespace {

PluginCatalogEntry entry(const std::string& id, const std::string& name, const std::string& version = "1.0.0", const std::string& published = "",
                         const std::string& notes = "") {
    PluginCatalogEntry e;
    e.id = id;
    e.name = name;
    e.version = version;
    e.published = published;
    e.whatsNew = notes;
    return e;
}

std::vector<std::string> ids(const std::vector<PluginCatalogEntry>& e, const std::vector<std::size_t>& order) {
    std::vector<std::string> out;
    for (const std::size_t i : order) { out.push_back(e[i].id); }
    return out;
}

void testVersionCompareMatchesTheRepo() {
    // the copy here and PluginRepo::compareVersions must never disagree
    const std::vector<std::string> v = {"", "0", "1", "1.0", "1.0.0", "1.2", "1.2.0", "1.10", "1.9", "1.0.0-rc1", "1.0.0-rc2", "1..2", "1.2.", "01.02",
                                        "2", "10", "9", "999999999999999999999999", "1000000000000000000000000", "a", "b", "1.a", "1.b", "1.0.0.1", "x.y.z",
                                        "1.0.0-beta", "1.0.0+build"};
    int disagree = 0;
    for (const auto& a : v) {
        for (const auto& b : v) {
            const int x = compareStoreVersions(a, b);
            const int y = cascade::core::PluginRepo::compareVersions(a, b);
            if ((x < 0) != (y < 0) || (x > 0) != (y > 0)) { ++disagree; std::printf("  disagree: '%s' vs '%s': %d vs %d\n", a.c_str(), b.c_str(), x, y); }
        }
    }
    CHECK(disagree == 0);
    std::mt19937_64 rng(9);
    const char alphabet[] = "0123456789.ab-";
    for (int i = 0; i < 20000; ++i) {
        std::string a(rng() % 9, ' '), b(rng() % 9, ' ');
        for (char& c : a) { c = alphabet[rng() % (sizeof(alphabet) - 1)]; }
        for (char& c : b) { c = alphabet[rng() % (sizeof(alphabet) - 1)]; }
        const int x = compareStoreVersions(a, b);
        const int y = cascade::core::PluginRepo::compareVersions(a, b);
        if ((x < 0) != (y < 0) || (x > 0) != (y > 0)) { ++disagree; }
    }
    CHECK(disagree == 0);
}

void testDates() {
    CHECK(validStoreDate("2026-11-11") && validStoreDate("2024-02-29") && validStoreDate("1970-01-01"));
    for (const char* bad : {"", "2026-13-01", "2026-00-10", "2026-11-00", "2026-11-31", "2026-02-30", "26-11-11", "2026/11/11", "2026-1-11",
                            "2026-11-1", "20261111", "2026-11-11 ", "abcd-ef-gh", "1969-12-31", "2026-11-111"}) {
        CHECK(!validStoreDate(bad));
    }
}

void testSortByName() {
    std::vector<PluginCatalogEntry> e = {entry("c", "charlie"), entry("a", "Alpha"), entry("b", "bravo"), entry("n", "")};
    e[3].id = "aaa-noname";                                    // an entry with no name sorts by its id
    const auto o = sortStoreEntries(e, {}, StoreSortKey::Name);
    CHECK((ids(e, o) == std::vector<std::string>{"aaa-noname", "a", "b", "c"}));                       // case-insensitive
    CHECK(sortStoreEntries({}, {}, StoreSortKey::Name).empty());
    // same name: by id; same id: by source
    std::vector<PluginCatalogEntry> d = {entry("z", "Same"), entry("y", "same")};
    CHECK((ids(d, sortStoreEntries(d, {}, StoreSortKey::Name)) == std::vector<std::string>{"y", "z"}));
}

void testSortByUpdated() {
    std::vector<PluginCatalogEntry> e = {entry("old", "Old", "1.0.0", "2025-01-01"), entry("new", "New", "1.0.0", "2026-11-01"),
                                         entry("none", "None", "9.0.0", ""), entry("bad", "Bad", "9.0.0", "yesterday"),
                                         entry("mid", "Mid", "1.0.0", "2026-03-15")};
    CHECK((ids(e, sortStoreEntries(e, {}, StoreSortKey::Updated)) == std::vector<std::string>{"new", "mid", "old", "bad", "none"}));   // undated last, by name
    // same date: the higher version first, then name
    std::vector<PluginCatalogEntry> s = {entry("a", "A", "1.2.0", "2026-01-01"), entry("b", "B", "1.10.0", "2026-01-01"), entry("c", "C", "1.10.0", "2026-01-01")};
    CHECK((ids(s, sortStoreEntries(s, {}, StoreSortKey::Updated)) == std::vector<std::string>{"b", "c", "a"}));
}

void testSortByPopularity() {
    std::vector<PluginCatalogEntry> e = {entry("a", "A"), entry("b", "B"), entry("c", "C"), entry("d", "D"), entry("e", "E")};
    StoreStatsMap st;
    st["b"].installs = 500;
    st["c"].installs = 500;
    st["d"].installs = 10;
    st["zzz-not-in-catalogue"].installs = 99999;               // a stat for an unknown id is ignored
    st["a"].installs = 0;                                      // a known zero ranks above an unknown figure
    const auto o = sortStoreEntries(e, st, StoreSortKey::Popular);
    CHECK((ids(e, o) == std::vector<std::string>{"b", "c", "d", "a", "e"}));
    // with no figures at all the order is by name
    CHECK((ids(e, sortStoreEntries(e, {}, StoreSortKey::Popular)) == std::vector<std::string>{"a", "b", "c", "d", "e"}));
    // the largest counts do not overflow the comparison
    st["d"].installs = std::numeric_limits<std::uint64_t>::max();
    st["b"].installs = std::numeric_limits<std::uint64_t>::max() - 1;
    CHECK((ids(e, sortStoreEntries(e, st, StoreSortKey::Popular)).front() == "d"));
}

void testRating() {
    StoreStats one;
    one.ratingAverage = 5.0;
    one.ratingCount = 1;
    StoreStats hundred;
    hundred.ratingAverage = 4.6;
    hundred.ratingCount = 100;
    CHECK(dampedRating(hundred) > dampedRating(one));            // one five-star vote does not beat a hundred votes at 4.6
    CHECK(dampedRating(StoreStats{}) == 0.0);
    StoreStats nan;
    nan.ratingAverage = std::nan("");
    nan.ratingCount = 50;
    CHECK(dampedRating(nan) == 0.0);
    StoreStats high;
    high.ratingAverage = 99.0;                                   // clamped to 5
    high.ratingCount = 1000000;
    CHECK(dampedRating(high) <= 5.0 && dampedRating(high) > 4.99);
    StoreStats neg;
    neg.ratingAverage = -3.0;
    neg.ratingCount = 1000000;
    CHECK(dampedRating(neg) >= 0.0 && dampedRating(neg) < 0.01);
    // the damping is a weighted mean: with the prior's votes and the prior's mean, nothing moves
    StoreStats neutral;
    neutral.ratingAverage = kRatingPriorMean;
    neutral.ratingCount = 10;
    CHECK_NEAR(dampedRating(neutral), kRatingPriorMean, 1e-12);

    std::vector<PluginCatalogEntry> e = {entry("one", "One"), entry("hundred", "Hundred"), entry("none", "None"), entry("nan", "Nan"), entry("zero", "Zero")};
    StoreStatsMap st;
    st["one"] = one;
    st["hundred"] = hundred;
    st["nan"] = nan;                                             // ratingCount > 0 but a non-finite average: counts as unrated
    st["zero"].installs = 50;                                    // stats with no ratings: unrated
    const auto o = sortStoreEntries(e, st, StoreSortKey::Rating);
    // nan has ratingCount > 0 and a non-finite average: it is NOT rated, so it sorts with the unrated, by name
    CHECK((ids(e, o) == std::vector<std::string>{"hundred", "one", "nan", "none", "zero"}));
}

void testSortIsAPermutationAndStable() {
    std::mt19937_64 rng(77);
    for (int round = 0; round < 200; ++round) {
        std::vector<PluginCatalogEntry> e;
        StoreStatsMap st;
        const int n = static_cast<int>(rng() % 30);
        for (int i = 0; i < n; ++i) {
            const std::string id = "p" + std::to_string(rng() % 20);        // duplicates on purpose
            char date[16];
            std::snprintf(date, sizeof(date), "20%02d-%02d-%02d", static_cast<int>(10 + rng() % 17), static_cast<int>(1 + rng() % 12), static_cast<int>(1 + rng() % 28));
            e.push_back(entry(id, (rng() % 3 == 0) ? "" : "Name" + std::to_string(rng() % 7), std::to_string(rng() % 4) + "." + std::to_string(rng() % 12),
                              (rng() % 4 == 0) ? "" : date));
            if (rng() % 2) {
                st[id].installs = rng() % 100;
                st[id].ratingCount = static_cast<std::uint32_t>(rng() % 50);
                st[id].ratingAverage = static_cast<double>(rng() % 60) / 10.0 - ((rng() % 20 == 0) ? 99.0 : 0.0);
            }
        }
        for (const StoreSortKey k : {StoreSortKey::Name, StoreSortKey::Updated, StoreSortKey::Popular, StoreSortKey::Rating}) {
            auto o = sortStoreEntries(e, st, k);
            CHECK(o.size() == e.size());
            auto copy = o;
            std::sort(copy.begin(), copy.end());
            bool perm = true;
            for (std::size_t i = 0; i < copy.size(); ++i) { perm = perm && copy[i] == i; }
            CHECK(perm);
            CHECK(sortStoreEntries(e, st, k) == o);                          // deterministic
        }
    }
}

void testHistoryObserve() {
    ReleaseNoteHistory h;
    CHECK(h.history("p").empty());
    CHECK(h.observe({entry("p", "P", "1.0.0", "2026-01-01", "first"), entry("q", "Q", "0.1.0", "", "")}) == 2);
    CHECK(h.observe({entry("p", "P", "1.1.0", "2026-02-01", "second")}) == 1);
    CHECK(h.observe({entry("p", "P", "1.10.0", "2026-03-01", "tenth"), entry("p", "P", "1.2.0", "2026-02-15", "between")}) == 2);
    const auto l = h.history("p");
    CHECK(l.size() == 4);
    // newest version first, by version order (1.10 > 1.2), not by date or insertion
    CHECK(l.size() == 4 && l[0].version == "1.10.0" && l[1].version == "1.2.0" && l[2].version == "1.1.0" && l[3].version == "1.0.0");
    CHECK(l.size() == 4 && l[3].notes == "first" && l[3].published == "2026-01-01");
    CHECK(h.history("q").size() == 1 && h.history("q")[0].notes.empty());     // no notes is recorded as no notes
    CHECK(h.pluginCount() == 2);
    // seen again: nothing new
    CHECK(h.observe({entry("p", "P", "1.1.0", "2026-02-01", "second")}) == 0);
    // FIRST SEEN WINS: a catalogue that rewrites an old version's notes changes nothing
    CHECK(h.observe({entry("p", "P", "1.0.0", "2030-01-01", "REWRITTEN HISTORY")}) == 0);
    CHECK(h.history("p")[3].notes == "first" && h.history("p")[3].published == "2026-01-01");
    // entries with no id or no version are ignored; a bad date is dropped, the entry kept
    CHECK(h.observe({entry("", "x", "1.0"), entry("z", "Z", ""), entry("w", "W", "1.0", "not-a-date", "n")}) == 1);
    CHECK(h.history("w").size() == 1 && h.history("w")[0].published.empty());
    // the same id from another source is another plugin
    PluginCatalogEntry site = entry("p", "P", "9.0.0", "", "from a site");
    site.source = "https://plugins.example";
    CHECK(h.observe({site}) == 1);
    CHECK(h.history("p").size() == 4);
    CHECK(h.history("p", "https://plugins.example").size() == 1);
    CHECK(h.history("p", "https://other.example").empty());
}

void testHistoryCaps() {
    ReleaseNoteHistory h;
    std::vector<PluginCatalogEntry> batch;
    for (int i = 1; i <= static_cast<int>(kMaxHistoryPerPlugin) + 10; ++i) { batch.push_back(entry("p", "P", "1." + std::to_string(i), "", "n" + std::to_string(i))); }
    h.observe(batch);
    const auto l = h.history("p");
    CHECK(l.size() == kMaxHistoryPerPlugin);
    CHECK(l.size() == kMaxHistoryPerPlugin && l.front().version == "1." + std::to_string(kMaxHistoryPerPlugin + 10));     // the oldest fell off
    CHECK(l.size() == kMaxHistoryPerPlugin && l.back().version == "1.11");
    // plugin cap
    ReleaseNoteHistory many;
    for (std::size_t i = 0; i < kMaxHistoryPlugins + 5; ++i) { many.observe({entry("id" + std::to_string(i), "N", "1.0")}); }
    CHECK(many.pluginCount() == kMaxHistoryPlugins);
    CHECK(many.history("id0").size() == 1 && many.history("id" + std::to_string(kMaxHistoryPlugins + 1)).empty());
    // notes are cut at a character boundary and carry no control characters; line breaks become spaces
    std::string long_note;
    for (std::size_t i = 0; i < kMaxNoteBytes; ++i) { long_note += (i % 2) ? "\xC3\xA9" : "a"; }      // multi-byte characters across the cut
    ReleaseNoteHistory c;
    c.observe({entry("p", "P", "1.0", "", long_note), entry("q", "Q", "1.0", "", "line1\nline2\r\n\tx\x01y")});
    const auto cut = c.history("p")[0].notes;
    CHECK(cut.size() <= kMaxNoteBytes);
    bool boundaryOk = true;
    for (std::size_t i = 0; i < cut.size();) {
        const unsigned char ch = static_cast<unsigned char>(cut[i]);
        const std::size_t len = ch < 0x80 ? 1 : (ch >= 0xC2 && ch <= 0xDF) ? 2 : 0;
        if (len == 0 || i + len > cut.size()) { boundaryOk = false; break; }
        i += len;
    }
    CHECK(boundaryOk);
    CHECK(c.history("q")[0].notes == "line1 line2   xy");
    // an over-long version string is cut to 64 bytes, not refused
    ReleaseNoteHistory v;
    v.observe({entry("p", "P", std::string(200, '1'))});
    CHECK(v.history("p").size() == 1 && v.history("p")[0].version.size() == 64);
}

void testHistoryJson() {
    ReleaseNoteHistory h;
    PluginCatalogEntry site = entry("p", "P", "2.0.0", "2026-05-05", "site notes \"quoted\" \\ \xC3\xA9");
    site.source = "https://plugins.example";
    h.observe({entry("p", "P", "1.0.0", "2026-01-01", "first"), entry("p", "P", "1.1.0", "2026-02-01", "second"), site, entry("q", "Q", "0.1", "", "")});
    const std::string j = h.toJson();
    ReleaseNoteHistory back;
    CHECK(ReleaseNoteHistory::fromJson(j, back));
    CHECK(back.pluginCount() == h.pluginCount());
    CHECK(back.toJson() == j);                                   // a stable round trip
    const auto p = back.history("p");
    CHECK(p.size() == 2 && p[0].version == "1.1.0" && p[1].notes == "first");
    CHECK(back.history("p", "https://plugins.example").size() == 1);
    CHECK(back.history("p", "https://plugins.example")[0].notes == "site notes \"quoted\" \\ \xC3\xA9");
    // refusals leave the target untouched
    ReleaseNoteHistory keep;
    keep.observe({entry("k", "K", "1.0", "", "kept")});
    CHECK(!ReleaseNoteHistory::fromJson("", keep));
    CHECK(!ReleaseNoteHistory::fromJson("not json", keep));
    CHECK(!ReleaseNoteHistory::fromJson("[]", keep));
    CHECK(!ReleaseNoteHistory::fromJson("{\"version\":2,\"plugins\":[]}", keep));
    CHECK(!ReleaseNoteHistory::fromJson("{\"version\":1}", keep));
    CHECK(!ReleaseNoteHistory::fromJson("{\"version\":1,\"plugins\":{}}", keep));
    CHECK(!ReleaseNoteHistory::fromJson(std::string(kMaxHistoryJsonBytes + 1, ' '), keep));
    CHECK(keep.history("k").size() == 1);
    // bad entries are skipped, good ones kept
    ReleaseNoteHistory partial;
    CHECK(ReleaseNoteHistory::fromJson(
        "{\"version\":1,\"plugins\":[5,{\"id\":\"\",\"releases\":[]},{\"id\":\"a\\nb\",\"releases\":[{\"version\":\"1\"}]},"
        "{\"id\":\"ok\",\"releases\":[7,{\"version\":\"\"},{\"notes\":\"no version\"},{\"version\":\"1.0\",\"published\":\"junk\",\"notes\":5},"
        "{\"version\":\"2.0\",\"published\":\"2026-02-02\",\"notes\":\"fine\"}]}]}", partial));
    CHECK(partial.pluginCount() == 1);
    const auto ok = partial.history("ok");
    CHECK(ok.size() == 2 && ok[0].version == "2.0" && ok[0].notes == "fine" && ok[1].version == "1.0" && ok[1].published.empty() && ok[1].notes.empty());
}

// FUZZ: fromJson on mangled documents never throws and never exceeds its caps.
void testHistoryFuzz() {
    ReleaseNoteHistory h;
    h.observe({entry("p", "P", "1.0.0", "2026-01-01", "first"), entry("p", "P", "1.1.0", "2026-02-01", "second"), entry("q", "Q", "0.1", "", "x")});
    const std::string seed = h.toJson();
    std::mt19937_64 rng(4242);
    int bad = 0;
    for (int i = 0; i < 8000; ++i) {
        std::string s = seed;
        const int ops = static_cast<int>(rng() % 8);
        for (int k = 0; k < ops; ++k) {
            if (s.empty()) { break; }
            const std::size_t pos = rng() % s.size();
            switch (rng() % 4) {
                case 0: s[pos] = static_cast<char>(rng()); break;
                case 1: s.erase(pos, 1 + rng() % 5); break;
                case 2: s.insert(pos, 1, static_cast<char>(rng())); break;
                default: s.resize(pos); break;
            }
        }
        try {
            ReleaseNoteHistory t;
            ReleaseNoteHistory::fromJson(s, t);
            if (t.pluginCount() > kMaxHistoryPlugins) { ++bad; }
            for (const char* id : {"p", "q"}) {
                if (t.history(id).size() > kMaxHistoryPerPlugin) { ++bad; }
                for (const ReleaseNote& n : t.history(id)) {
                    if (n.notes.size() > kMaxNoteBytes || n.version.size() > 64 || n.version.empty()) { ++bad; }
                }
            }
            t.toJson();
        } catch (...) { ++bad; }
    }
    CHECK(bad == 0);
    for (int i = 0; i < 3000; ++i) {
        std::string s(rng() % 200, '\0');
        for (char& c : s) { c = static_cast<char>(rng()); }
        try { ReleaseNoteHistory t; ReleaseNoteHistory::fromJson(s, t); } catch (...) { ++bad; }
    }
    CHECK(bad == 0);
}

void testSourceNeverPrintsOrWrites() {
    std::ifstream in(std::string(CASCADE_SOURCE_DIR) + "/src/core/store_sort.cpp", std::ios::binary);
    CHECK(static_cast<bool>(in));
    const std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(!src.empty());
    for (const char* banned : {"printf", "std::cout", "std::cerr", "fopen", "ofstream", "fstream", "fwrite", "stderr", "stdout", "getenv", "httplib"}) {
        CHECK(src.find(banned) == std::string::npos);
    }
}

}  // namespace

int main() {
    testVersionCompareMatchesTheRepo();
    testDates();
    testSortByName();
    testSortByUpdated();
    testSortByPopularity();
    testRating();
    testSortIsAPermutationAndStable();
    testHistoryObserve();
    testHistoryCaps();
    testHistoryJson();
    testHistoryFuzz();
    testSourceNeverPrintsOrWrites();
    return testSummary("test_store_sort");
}
