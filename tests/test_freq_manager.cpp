// Tests for core/freq_manager.hpp / freq_manager.cpp (FreqManager).
//
// Every fixture file is synthesized in-test under one pid-suffixed directory
// in the CWD — ctest runs each test from build-<slug>/tests, which is
// gitignored — and the whole directory is removed on success, left behind on
// failure for autopsy.
//
// Reference checking: roundtrip equality is asserted field by field against
// the exact values added, never against re-serialized output. Double
// exactness through JSON is legitimate: nlohmann emits round-trippable
// shortest representations.
//
// The scrambled-adds ordering test drives frequencies from a fixed-seed LCG
// and checks the sorted invariant against an independently maintained sorted
// copy of the same values — the reference is the input data, not the
// implementation's own output.
//
// The atomicity test is Windows-specific by nature (POSIX rename happily
// replaces an open file): the target is held open WITHOUT FILE_SHARE_DELETE,
// which blocks the rename step while still permitting a plain write — so an
// implementation that "saves" by writing the target directly would succeed
// and clobber the file, turning the assertions red. (Technique from
// test_config.cpp.)
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/freq_manager.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#define TEST_GETPID _getpid
#else
#include <unistd.h>
#define TEST_GETPID getpid
#endif

#include "test_check.hpp"

using cascade::core::Bookmark;
using cascade::core::FreqManager;
namespace fs = std::filesystem;

namespace {

std::string g_root;  // per-process fixture directory, set in main()

std::string p(const char* rel) { return g_root + "/" + rel; }

bool writeText(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(f);
}

std::string readAll(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

bool sortedByFreq(const std::vector<Bookmark>& v) {
    for (std::size_t i = 1; i < v.size(); ++i) {
        if (v[i - 1].freqHz > v[i].freqHz) {
            return false;
        }
    }
    return true;
}

// The names in list order ("C,A,B"), so a wrong order is one failure line that
// says what the order was.
std::string orderOf(const FreqManager& m) {
    std::string s;
    for (const Bookmark& b : m.list()) {
        if (!s.empty()) { s += ','; }
        s += b.name;
    }
    return s;
}

void checkOrder(const FreqManager& m, const std::string& want, int line) {
    ++g_checksRun;
    const std::string got = orderOf(m);
    if (got != want) {
        ++g_checksFailed;
        std::printf("FAIL %s:%d  list order \"%s\", wanted \"%s\"\n", __FILE__, line, got.c_str(),
                    want.c_str());
    }
}
#define CHECK_ORDER(m, want) checkOrder((m), (want), __LINE__)

// Field-by-field equality with one CHECK each, so a mismatch names the field.
void checkEqual(const Bookmark& a, const Bookmark& b) {
    CHECK(a.name == b.name);
    CHECK(a.freqHz == b.freqHz);
    CHECK(a.mode == b.mode);
    CHECK(a.bandwidthHz == b.bandwidthHz);
}

// Minimal fixed-seed LCG (Numerical Recipes constants); no <random>.
struct Lcg {
    unsigned long long s;
    explicit Lcg(unsigned long long seed) : s(seed) {}
    unsigned long long next() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return s >> 33;
    }
};

}  // namespace

int main() {
    g_root = "fm_test_" + std::to_string(TEST_GETPID());
    fs::remove_all(g_root);  // stale debris from a failed prior run
    fs::create_directory(g_root);

    // --- defaultPath: documented shape, no filesystem side effects ----------
    {
        const std::string dp = FreqManager::defaultPath();
        CHECK(!dp.empty());
        CHECK(dp.find("foxsdr") != std::string::npos);
        CHECK(dp.ends_with("bookmarks.json"));
        CHECK(!fs::exists(dp) || fs::is_regular_file(dp));  // never a dir
    }

    // --- missing file: empty + true, and it CLEARS a pre-populated list -----
    {
        FreqManager m;
        Bookmark b;
        b.name = "stale";
        b.freqHz = 1.0e6;
        m.add(b);
        CHECK(m.list().size() == 1u);

        std::string err = "stale";
        CHECK(m.load(p("never_written.json"), err));
        CHECK(err.empty());
        CHECK(m.list().empty());
    }

    // --- add: sorted index returned; scrambled adds keep the invariant ------
    {
        FreqManager m;
        Bookmark b;

        b.name = "c";
        b.freqHz = 100.0;
        CHECK(m.add(b) == 0);
        b.name = "a";
        b.freqHz = 50.0;
        CHECK(m.add(b) == 0);  // smallest goes first
        b.name = "b";
        b.freqHz = 75.0;
        CHECK(m.add(b) == 1);  // between the two
        b.name = "d";
        b.freqHz = 200.0;
        CHECK(m.add(b) == 3);  // largest goes last
        CHECK(m.list().size() == 4u);
        CHECK(m.list()[0].name == "a");
        CHECK(m.list()[1].name == "b");
        CHECK(m.list()[2].name == "c");
        CHECK(m.list()[3].name == "d");

        // Equal frequency: the newcomer lands AFTER its peer (stable).
        b.name = "b2";
        b.freqHz = 75.0;
        CHECK(m.add(b) == 2);
        CHECK(m.list()[1].name == "b");
        CHECK(m.list()[2].name == "b2");
    }

    // --- scrambled bulk adds (fixed-seed LCG) vs an independent reference ---
    {
        FreqManager m;
        Lcg rng(0xC0FFEEULL);
        std::vector<double> ref;
        for (int i = 0; i < 64; ++i) {
            const double f = 1.0e5 + static_cast<double>(rng.next() % 1000000ULL);
            Bookmark b;
            b.name = "bm" + std::to_string(i);
            b.freqHz = f;
            const int idx = m.add(b);
            // The returned index must point at the bookmark just added.
            CHECK(idx >= 0 && static_cast<std::size_t>(idx) < m.list().size());
            CHECK(m.list()[static_cast<std::size_t>(idx)].name == b.name);
            ref.push_back(f);
        }
        // Reference order comes from the input values, not from the class.
        std::sort(ref.begin(), ref.end());
        CHECK(m.list().size() == ref.size());
        CHECK(sortedByFreq(m.list()));
        for (std::size_t i = 0; i < ref.size(); ++i) {
            CHECK(m.list()[i].freqHz == ref[i]);
        }
    }

    // --- dedup naming chain --------------------------------------------------
    {
        FreqManager m;
        Bookmark b;
        b.name = "Repeater";
        b.freqHz = 3.0;
        CHECK(m.add(b) == 0);
        b.freqHz = 1.0;
        CHECK(m.add(b) == 0);
        b.freqHz = 2.0;
        CHECK(m.add(b) == 1);
        // Sorted by freq: 1.0, 2.0, 3.0 — added 2nd, 3rd, 1st respectively.
        CHECK(m.list()[0].name == "Repeater (2)");
        CHECK(m.list()[1].name == "Repeater (3)");
        CHECK(m.list()[2].name == "Repeater");

        // A hand-claimed suffix is skipped over, not stolen.
        FreqManager m2;
        Bookmark c;
        c.name = "Alpha (2)";
        c.freqHz = 10.0;
        CHECK(m2.add(c) == 0);
        c.name = "Alpha";
        c.freqHz = 20.0;
        CHECK(m2.add(c) == 1);  // "Alpha" itself is free
        c.name = "Alpha";
        c.freqHz = 30.0;
        CHECK(m2.add(c) == 2);  // "Alpha (2)" taken -> "Alpha (3)"
        CHECK(m2.list()[2].name == "Alpha (3)");
    }

    // --- roundtrip through a path needing new directories --------------------
    {
        FreqManager m;
        Bookmark b1;
        b1.name = "PMR ch1";
        b1.freqHz = 446006250.0;
        b1.mode = "NFM";
        b1.bandwidthHz = 12500.0;
        Bookmark b2;
        b2.name = "";  // empty name is legal
        b2.freqHz = 198000.0;
        b2.mode = "AM";
        b2.bandwidthHz = 9000.0;
        Bookmark b3;
        b3.name = "40m FT8";
        b3.freqHz = 7074000.0;
        b3.mode = "FT8";  // unknown mode string must survive verbatim
        b3.bandwidthHz = 3000.0;
        m.add(b1);
        m.add(b2);
        m.add(b3);

        const std::string path = p("nested/deeper/bookmarks.json");
        std::string err = "stale";
        CHECK(m.save(path, err));
        CHECK(err.empty());
        CHECK(fs::is_regular_file(path));  // dirs were created on demand

        FreqManager m2;
        CHECK(m2.load(path, err));
        CHECK(err.empty());
        CHECK(m2.list().size() == 3u);
        CHECK(sortedByFreq(m2.list()));
        // Sorted: 198 kHz, 7.074 MHz, 446.00625 MHz.
        checkEqual(m2.list()[0], b2);
        checkEqual(m2.list()[1], b3);
        checkEqual(m2.list()[2], b1);
    }

    // --- per-entry corruption: bad entries skipped, rest load, true ----------
    {
        const std::string path = p("per_entry.json");
        CHECK(writeText(path,
                        "{\"schemaVersion\":1,\"bookmarks\":["
                        "\"just a bare string\","                       // not an object
                        "{\"name\":\"NoFreq\",\"mode\":\"AM\"},"        // freqHz missing
                        "{\"name\":\"Neg\",\"freqHz\":-5.0},"           // negative freq
                        "{\"name\":\"BadFreqType\",\"freqHz\":\"hi\"}," // freq not a number
                        "17,"                                            // not an object
                        "{\"name\":\"Good\",\"freqHz\":145500000.0,"
                        "\"mode\":\"NFM\",\"bandwidthHz\":12500.0},"
                        "{\"name\":\"Weird\",\"freqHz\":7100000.0,"
                        "\"mode\":\"FT8-super\",\"bandwidthHz\":-3.0},"  // bw repaired
                        "{\"freqHz\":0.0,\"mode\":9}"                    // 0 Hz legal; bad mode type
                        "]}\n"));
        FreqManager m;
        std::string err = "stale";
        CHECK(m.load(path, err));  // per-entry damage still returns true
        CHECK(err.empty());
        CHECK(m.list().size() == 3u);
        CHECK(sortedByFreq(m.list()));

        // 0 Hz entry: name defaulted to "", mode wrong-typed -> default WFM.
        CHECK(m.list()[0].freqHz == 0.0);
        CHECK(m.list()[0].name.empty());
        CHECK(m.list()[0].mode == "WFM");
        CHECK(m.list()[0].bandwidthHz == 150000.0);

        // Unknown mode kept verbatim; non-positive bandwidth repaired.
        CHECK(m.list()[1].name == "Weird");
        CHECK(m.list()[1].freqHz == 7100000.0);
        CHECK(m.list()[1].mode == "FT8-super");
        CHECK(m.list()[1].bandwidthHz == 150000.0);

        CHECK(m.list()[2].name == "Good");
        CHECK(m.list()[2].bandwidthHz == 12500.0);
    }

    // --- whole-file corruption: empty + false + error text -------------------
    {
        FreqManager m;
        Bookmark b;
        b.name = "survivor?";
        b.freqHz = 5.0e6;
        m.add(b);  // pre-populate: failure must CLEAR, not preserve

        const std::string path = p("corrupt.json");
        CHECK(writeText(path, "{ this is not json at all"));
        std::string err;
        CHECK(!m.load(path, err));
        CHECK(!err.empty());
        CHECK(m.list().empty());

        // Valid JSON, non-object root: also corrupt.
        const std::string path2 = p("array_root.json");
        CHECK(writeText(path2, "[1,2,3]\n"));
        CHECK(!m.load(path2, err));
        CHECK(!err.empty());
        CHECK(m.list().empty());

        // Object root but "bookmarks" is not an array: structural damage.
        const std::string path3 = p("bad_collection.json");
        CHECK(writeText(path3, "{\"schemaVersion\":1,\"bookmarks\":\"nope\"}\n"));
        CHECK(!m.load(path3, err));
        CHECK(!err.empty());
        CHECK(m.list().empty());

        // schemaVersion mismatch: entries untrusted, empty + false.
        const std::string path4 = p("schema_999.json");
        CHECK(writeText(path4,
                        "{\"schemaVersion\":999,\"bookmarks\":"
                        "[{\"name\":\"x\",\"freqHz\":1.0}]}\n"));
        CHECK(!m.load(path4, err));
        CHECK(!err.empty());
        CHECK(m.list().empty());

        // Object with no "bookmarks" key at all: valid empty collection.
        const std::string path5 = p("no_key.json");
        CHECK(writeText(path5, "{\"schemaVersion\":1}\n"));
        CHECK(m.load(path5, err));
        CHECK(err.empty());
        CHECK(m.list().empty());
    }

    // --- removeAt / updateAt bounds and behavior -----------------------------
    {
        FreqManager m;
        std::string err;

        // Empty list: every index is out of range.
        CHECK(!m.removeAt(0));
        Bookmark u;
        u.name = "u";
        u.freqHz = 1.0;
        CHECK(!m.updateAt(0, u));

        Bookmark b;
        b.name = "low";
        b.freqHz = 10.0;
        m.add(b);
        b.name = "mid";
        b.freqHz = 20.0;
        m.add(b);
        b.name = "high";
        b.freqHz = 30.0;
        m.add(b);

        // One-past-the-end is rejected and changes nothing.
        CHECK(!m.removeAt(3));
        CHECK(!m.updateAt(3, u));
        CHECK(m.list().size() == 3u);
        CHECK(m.list()[0].name == "low");
        CHECK(m.list()[1].name == "mid");
        CHECK(m.list()[2].name == "high");

        // updateAt re-sorts: move "low" (index 0) to the top frequency.
        Bookmark moved;
        moved.name = "low-moved";
        moved.freqHz = 99.0;
        moved.mode = "USB";
        moved.bandwidthHz = 2700.0;
        CHECK(m.updateAt(0, moved));
        CHECK(m.list().size() == 3u);
        CHECK(sortedByFreq(m.list()));
        CHECK(m.list()[0].name == "mid");
        CHECK(m.list()[1].name == "high");
        checkEqual(m.list()[2], moved);

        // removeAt drops exactly the indexed entry.
        CHECK(m.removeAt(1));  // "high"
        CHECK(m.list().size() == 2u);
        CHECK(m.list()[0].name == "mid");
        CHECK(m.list()[1].name == "low-moved");
    }

    // --- updateAt: an edit that leaves the frequency alone keeps the row's place
    // THE BUG (CI's arm64 run 37462575079): the airband monitor credits heard
    // time every 20 s through updateAt, which erased the row and re-inserted it
    // AFTER its peers on the same frequency. Two ticked rows on one frequency
    // ("High" AM, "HighNfm" NFM) swapped places at the first flush, and the
    // monitor - which plays the FIRST of them - changed channel from the AM row
    // to the NFM one in the middle of a session. The order among equal
    // frequencies is the file's order and only an edit that moves the
    // frequency may change it.
    {
        FreqManager m;
        Bookmark a;
        a.name = "A";
        a.freqHz = 20.0;
        a.mode = "AM";
        a.scan = true;
        Bookmark b;
        b.name = "B";
        b.freqHz = 20.0;
        b.mode = "NFM";
        b.scan = true;
        Bookmark c;
        c.name = "C";
        c.freqHz = 10.0;
        CHECK(m.add(a) == 0);
        CHECK(m.add(b) == 1);   // after its peer: insertion order among equals
        CHECK(m.add(c) == 0);
        CHECK_ORDER(m, "C,A,B");

        // Only heardSeconds changed (the monitor's flush): "A" stays before "B".
        unsigned v = m.version();
        Bookmark edit = m.list()[1];
        CHECK(edit.name == "A");
        edit.heardSeconds += 3.5;
        CHECK(m.updateAt(1, edit));
        CHECK_ORDER(m, "C,A,B");
        CHECK(m.list()[1].heardSeconds == 3.5);
        CHECK(m.list()[2].heardSeconds == 0.0);
        CHECK(m.version() > v);   // a row changed in place is still a change to a version reader

        // Only scan changed (a tick toggled): same.
        v = m.version();
        edit = m.list()[1];
        edit.scan = false;
        CHECK(m.updateAt(1, edit));
        CHECK_ORDER(m, "C,A,B");
        CHECK(!m.list()[1].scan);
        CHECK(m.version() > v);

        // The flush repeated: still the same order after ten of them, and the
        // other row of the pair edited too (the last of a tie stays last).
        for (int i = 0; i < 10; ++i) {
            edit = m.list()[1];
            edit.heardSeconds += 1.0;
            CHECK(m.updateAt(1, edit));
            edit = m.list()[2];
            edit.heardSeconds += 1.0;
            CHECK(m.updateAt(2, edit));
        }
        CHECK_ORDER(m, "C,A,B");
        CHECK(m.list()[1].heardSeconds == 13.5);
        CHECK(m.list()[2].heardSeconds == 10.0);

        // An edit of the first of a tie on a different field entirely, or of a
        // row with no peer, changes nothing about the order either.
        edit = m.list()[0];
        edit.favourite = true;
        CHECK(m.updateAt(0, edit));
        CHECK_ORDER(m, "C,A,B");

        // A CHANGED frequency still re-sorts. "A" to 30: behind everything.
        v = m.version();
        edit = m.list()[1];
        edit.freqHz = 30.0;
        CHECK(m.updateAt(1, edit));
        CHECK_ORDER(m, "C,B,A");
        CHECK(sortedByFreq(m.list()));
        CHECK(m.version() > v);

        // "B" to 10, onto "C"'s frequency: it lands after its new peer (the
        // insertion rule for a frequency that is new to the row), ahead of "A".
        v = m.version();
        edit = m.list()[1];
        CHECK(edit.name == "B");
        edit.freqHz = 10.0;
        CHECK(m.updateAt(1, edit));
        CHECK_ORDER(m, "C,B,A");
        CHECK(m.list()[1].freqHz == 10.0);
        CHECK(sortedByFreq(m.list()));
        CHECK(m.version() > v);

        // And back below it: "A" to 5 leads the list.
        edit = m.list()[2];
        edit.freqHz = 5.0;
        CHECK(m.updateAt(2, edit));
        CHECK_ORDER(m, "A,C,B");
        CHECK(sortedByFreq(m.list()));

        // Bounds unchanged: out of range is refused, changes nothing and does
        // not touch the version.
        v = m.version();
        CHECK(!m.updateAt(3, edit));
        CHECK(!m.updateAt(static_cast<std::size_t>(-1), edit));
        CHECK(m.version() == v);
        CHECK_ORDER(m, "A,C,B");
    }

#ifdef _WIN32
    // --- ATOMICITY: locked target => save fails, original byte-intact --------
    {
        fs::create_directory(p("atomic"));
        const std::string path = p("atomic/bookmarks.json");
        FreqManager m;
        Bookmark b;
        b.name = "orig";
        b.freqHz = 1.0e6;
        m.add(b);
        std::string err;
        CHECK(m.save(path, err));
        const std::string origBytes = readAll(path);
        CHECK(!origBytes.empty());

        // Share READ+WRITE but NOT DELETE: rename-over is blocked, yet a
        // naive direct rewrite of the target would still open fine and
        // clobber it. Atomic save must fail and leave every byte in place.
        HANDLE h = CreateFileA(path.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE);
        b.name = "update";
        b.freqHz = 2.0e6;
        m.add(b);
        err.clear();
        CHECK(!m.save(path, err));
        CHECK(!err.empty());
        CloseHandle(h);
        CHECK(readAll(path) == origBytes);

        // No temp-file debris may survive a failed save.
        std::size_t entries = 0;
        for (const auto& e : fs::directory_iterator(p("atomic"))) {
            (void)e;
            ++entries;
        }
        CHECK(entries == 1u);

        // The file still loads as the ORIGINAL single bookmark.
        FreqManager check;
        CHECK(check.load(path, err));
        CHECK(check.list().size() == 1u);
        CHECK(check.list()[0].name == "orig");

        // Lock released: the same save now succeeds and the content flips.
        CHECK(m.save(path, err));
        CHECK(check.load(path, err));
        CHECK(check.list().size() == 2u);
        CHECK(check.list()[1].name == "update");
    }
#endif

    // --- "scan" and "heardSeconds" (the airband request): round trip, written
    //     only when set, damaged values repaired ------------------------------
    {
        FreqManager m;
        Bookmark a;
        a.name = "ORD GND inbound";
        a.freqHz = 121.9e6;
        a.mode = "AM";
        a.bandwidthHz = 10000.0;
        a.scan = true;
        a.heardSeconds = 1234.5;
        m.add(a);
        Bookmark b;
        b.name = "plain";
        b.freqHz = 100.0e6;
        m.add(b);
        std::string err;
        const std::string path = p("scan_heard.json");
        CHECK(m.save(path, err));
        const std::string text = readAll(path);
        // Present once each - for the entry that has them, and only that one.
        CHECK(text.find("\"scan\"") != std::string::npos);
        CHECK(text.find("\"scan\"") == text.rfind("\"scan\""));
        CHECK(text.find("\"heardSeconds\"") != std::string::npos);
        CHECK(text.find("\"heardSeconds\"") == text.rfind("\"heardSeconds\""));

        FreqManager back;
        CHECK(back.load(path, err));
        CHECK(back.list().size() == 2u);
        if (back.list().size() == 2u) {
            CHECK(!back.list()[0].scan);
            CHECK(back.list()[0].heardSeconds == 0.0);
            CHECK(back.list()[1].scan);
            CHECK(back.list()[1].heardSeconds == 1234.5);
            checkEqual(back.list()[1], a);
        }

        // A list without either field saves exactly as before they existed.
        FreqManager old;
        old.add(b);
        CHECK(old.save(p("no_scan.json"), err));
        const std::string oldText = readAll(p("no_scan.json"));
        CHECK(oldText.find("scan") == std::string::npos);
        CHECK(oldText.find("heard") == std::string::npos);

        // Hand-edited damage: a non-boolean scan is ignored, a negative or
        // non-numeric heardSeconds reads as 0; the entries still load.
        CHECK(writeText(p("scan_bad.json"),
                        "{\"schemaVersion\":1,\"bookmarks\":["
                        "{\"name\":\"x\",\"freqHz\":118.05e6,\"scan\":\"yes\",\"heardSeconds\":-4},"
                        "{\"name\":\"y\",\"freqHz\":118.1e6,\"scan\":1,\"heardSeconds\":\"lots\"},"
                        "{\"name\":\"z\",\"freqHz\":118.2e6,\"scan\":false,\"heardSeconds\":7}]}"));
        FreqManager bad;
        CHECK(bad.load(p("scan_bad.json"), err));
        CHECK(bad.list().size() == 3u);
        if (bad.list().size() == 3u) {
            CHECK(!bad.list()[0].scan && bad.list()[0].heardSeconds == 0.0);
            CHECK(!bad.list()[1].scan && bad.list()[1].heardSeconds == 0.0);
            CHECK(!bad.list()[2].scan && bad.list()[2].heardSeconds == 7.0);
        }
    }

    // --- addMany skips a repeat only in the SAME group: two airports' EMERG
    //     121.5 are two entries; the same airport added twice adds nothing ---
    {
        FreqManager m;
        Bookmark e;
        e.name = "EMERG";
        e.freqHz = 121.5e6;
        e.group = "KORD Chicago O'Hare International Airport";
        CHECK(m.addMany({e}) == 1u);
        CHECK(m.addMany({e}) == 0u);
        e.group = "KMDW Chicago Midway International Airport";
        CHECK(m.addMany({e}) == 1u);
        CHECK(m.list().size() == 2u);
    }

    const int rc = testSummary("test_freq_manager");
    if (rc == 0) {
        std::error_code ec;
        fs::remove_all(g_root, ec);  // success: leave nothing behind
    }
    return rc;
}
