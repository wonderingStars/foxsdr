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

    const int rc = testSummary("test_freq_manager");
    if (rc == 0) {
        std::error_code ec;
        fs::remove_all(g_root, ec);  // success: leave nothing behind
    }
    return rc;
}
