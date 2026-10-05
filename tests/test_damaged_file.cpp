// A file that failed to load is kept aside, never saved over (core/damaged_file.hpp, 0.99.65).
//
// The unit under test is setDamagedFileAside: what it keeps, what it leaves alone, the name it
// gives the copy, the three-newest rule and the refusal. The application-level reactions (the
// savers told not to write, the log lines, the three real files) are in
// tests/test_failure_files.cpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cstdio>
#include <exception>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/damaged_file.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using cascade::core::SetAsideResult;
using cascade::core::setDamagedFileAside;
using Outcome = cascade::core::SetAsideResult::Outcome;

namespace {

fs::path g_scratch;

std::string readText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

void putText(const fs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
}

std::vector<std::string> namesIn(const fs::path& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) { out.push_back(e.path().filename().string()); }
    std::sort(out.begin(), out.end());
    return out;
}

// 2023-11-14 22:13:20 UTC
constexpr std::time_t kNow = 1700000000;
const char* const kStamp = "20231114-221320";

void nothingToKeep() {
    const fs::path dir = g_scratch / "nothing";
    std::error_code ec;
    fs::create_directories(dir, ec);
    // A first run: no file.
    SetAsideResult r = setDamagedFileAside((dir / "config.json").string(), kNow);
    CHECK(r.outcome == Outcome::NothingToKeep);
    // A zero-byte file.
    putText(dir / "config.json", "");
    r = setDamagedFileAside((dir / "config.json").string(), kNow);
    CHECK(r.outcome == Outcome::NothingToKeep);
    CHECK(fs::exists(dir / "config.json", ec));
    // A folder squatting on the name: not the user's data, left where it is.
    fs::create_directories(dir / "bookmarks.json", ec);
    r = setDamagedFileAside((dir / "bookmarks.json").string(), kNow);
    CHECK(r.outcome == Outcome::NothingToKeep);
    CHECK(fs::is_directory(dir / "bookmarks.json", ec));
    CHECK(namesIn(dir).size() == 2u);  // no copy was made of any of them
    std::printf("  missing, zero-byte and a folder: nothing kept, nothing renamed\n");
}

void keptAside() {
    const fs::path dir = g_scratch / "kept";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string damaged = "{\"schemaVersion\": 1, \"sourceKi";
    putText(dir / "config.json", damaged);
    const SetAsideResult r = setDamagedFileAside((dir / "config.json").string(), kNow);
    CHECK(r.outcome == Outcome::KeptAside);
    CHECK(r.bytes == damaged.size());
    const std::string want = std::string("config.json.bad-") + kStamp;
    CHECK(fs::path(r.keptAs).filename().string() == want);
    CHECK(readText(dir / want) == damaged);               // byte for byte
    CHECK(!fs::exists(dir / "config.json", ec));          // the live name is free for a good file
    std::printf("  kept as %s, byte for byte; the live name is free\n", want.c_str());
}

void sameSecondGetsASuffix() {
    const fs::path dir = g_scratch / "same-second";
    std::error_code ec;
    fs::create_directories(dir, ec);
    for (int i = 0; i < 3; ++i) {
        putText(dir / "markers.json", "damaged-" + std::to_string(i));
        const SetAsideResult r = setDamagedFileAside((dir / "markers.json").string(), kNow);
        CHECK(r.outcome == Outcome::KeptAside);
    }
    const std::string base = std::string("markers.json.bad-") + kStamp;
    CHECK(readText(dir / base) == "damaged-0");           // never replaced by the next
    CHECK(readText(dir / (base + "-2")) == "damaged-1");
    CHECK(readText(dir / (base + "-3")) == "damaged-2");
}

// THE THREE NEWEST, BY THE STAMP and then NUMERICALLY by the suffix ("-10" is newer than "-2").
void onlyTheThreeNewestAreKept() {
    const fs::path dir = g_scratch / "newest";
    std::error_code ec;
    fs::create_directories(dir, ec);
    // Five different seconds: the oldest two go.
    for (int i = 0; i < 5; ++i) {
        putText(dir / "config.json", "second-" + std::to_string(i));
        setDamagedFileAside((dir / "config.json").string(), kNow + i);
    }
    std::vector<std::string> bad;
    for (const std::string& n : namesIn(dir)) { bad.push_back(n); }
    CHECK(bad.size() == 3u);
    if (bad.size() == 3u) {
        CHECK(readText(dir / bad[0]) == "second-2");
        CHECK(readText(dir / bad[1]) == "second-3");
        CHECK(readText(dir / bad[2]) == "second-4");
    }

    // Eleven in ONE second: "-9", "-10" and "-11" are the newest, not whatever sorts last as text.
    const fs::path dir2 = g_scratch / "numeric";
    fs::create_directories(dir2, ec);
    for (int i = 1; i <= 11; ++i) {
        putText(dir2 / "bookmarks.json", "copy-" + std::to_string(i));
        setDamagedFileAside((dir2 / "bookmarks.json").string(), kNow);
    }
    const std::string base = std::string("bookmarks.json.bad-") + kStamp;
    const std::vector<std::string> names2 = namesIn(dir2);
    const std::set<std::string> have(names2.begin(), names2.end());
    CHECK(have == (std::set<std::string>{base + "-9", base + "-10", base + "-11"}));
    CHECK(readText(dir2 / (base + "-11")) == "copy-11");

    // Other files in the folder are not ours to prune: the neighbour's copies, a file that only
    // looks alike, and unrelated names stay.
    const fs::path dir3 = g_scratch / "neighbours";
    fs::create_directories(dir3, ec);
    putText(dir3 / "markers.json.bad-20200101-000000", "a neighbour's copy");
    putText(dir3 / "config.json.bad-notastamp", "does not look like a copy");
    putText(dir3 / "config.json.bak", "a backup made by hand");
    putText(dir3 / "config.json", "x");
    for (int i = 0; i < 5; ++i) {
        putText(dir3 / "config.json", "damaged-" + std::to_string(i));
        setDamagedFileAside((dir3 / "config.json").string(), kNow + i);
    }
    CHECK(fs::exists(dir3 / "markers.json.bad-20200101-000000", ec));
    CHECK(fs::exists(dir3 / "config.json.bad-notastamp", ec));
    CHECK(fs::exists(dir3 / "config.json.bak", ec));
    std::size_t configCopies = 0;
    for (const std::string& n : namesIn(dir3)) {
        if (n.rfind("config.json.bad-2", 0) == 0) { ++configCopies; }
    }
    CHECK(configCopies == 3u);
    std::printf("  five seconds leave the three newest; eleven in one second leave -9, -10, -11; "
                "neighbours are not touched\n");
}

#if defined(_WIN32)
// THE RENAME REFUSED: another program holds the file (reads and writes allowed, rename not).
void renameRefused() {
    const fs::path dir = g_scratch / "refused";
    std::error_code ec;
    fs::create_directories(dir, ec);
    putText(dir / "config.json", "damaged but held");
    // An older copy is there: a refusal must not prune it either.
    putText(dir / (std::string("config.json.bad-") + "20200101-000000"), "older copy");
    HANDLE h = ::CreateFileW((dir / "config.json").wstring().c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    if (h == INVALID_HANDLE_VALUE) { return; }
    const SetAsideResult r = setDamagedFileAside((dir / "config.json").string(), kNow);
    ::CloseHandle(h);
    CHECK(r.outcome == Outcome::CouldNotKeep);
    CHECK(r.bytes == std::string("damaged but held").size());
    CHECK(readText(dir / "config.json") == "damaged but held");
    CHECK(namesIn(dir).size() == 2u);
    std::printf("  rename refused (file held): reported, the damaged file untouched, nothing pruned\n");
}
#else
// THE RENAME REFUSED, POSIX: a file that is open can still be renamed there, so what refuses it is the
// permission on its FOLDER (a rename needs write access to the folder). Proved by trying to write in
// it - run as root the bits stop nothing and there is nothing to test.
void renameRefused() {
    const fs::path dir = g_scratch / "refused";
    std::error_code ec;
    fs::create_directories(dir, ec);
    putText(dir / "config.json", "damaged but in a read-only folder");
    putText(dir / (std::string("config.json.bad-") + "20200101-000000"), "older copy");
    fs::permissions(dir, fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
                    fs::perm_options::remove, ec);
    bool writable = true;
    {
        std::ofstream probe(dir / "probe.tmp");
        writable = static_cast<bool>(probe);
    }
    if (writable) {
        fs::remove(dir / "probe.tmp", ec);
        fs::permissions(dir, fs::perms::owner_write, fs::perm_options::add, ec);
        std::printf("  rename refused (read-only folder): NOT REACHED (the permission bits stop nothing here)\n");
        return;
    }
    const SetAsideResult r = setDamagedFileAside((dir / "config.json").string(), kNow);
    fs::permissions(dir, fs::perms::owner_write, fs::perm_options::add, ec);
    CHECK(r.outcome == Outcome::CouldNotKeep);
    CHECK(r.bytes == std::string("damaged but in a read-only folder").size());
    CHECK(readText(dir / "config.json") == "damaged but in a read-only folder");
    CHECK(namesIn(dir).size() == 2u);  // the live file and the older copy: nothing renamed, nothing pruned
    std::printf("  rename refused (read-only folder): reported, the damaged file untouched, nothing pruned\n");
}
#endif

}  // namespace

// A case that THROWS is a failed check with its message, never a terminate: an uncaught exception
// ended test_failure_files on Linux in 0.08 s and took every later case with it (0.99.65).
#define GUARDED(call)                                                              \
    do {                                                                           \
        try {                                                                      \
            call;                                                                  \
        } catch (const std::exception& e) {                                        \
            std::printf("  EXCEPTION in %s: %s\n", #call, e.what());                \
            CHECK(false);                                                          \
        } catch (...) {                                                            \
            std::printf("  EXCEPTION in %s: (not a std::exception)\n", #call);      \
            CHECK(false);                                                          \
        }                                                                          \
    } while (0)

int main() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = fs::temp_directory_path() / ("foxsdr_damaged_file_" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    fs::create_directories(g_scratch, ec);
    std::printf("test_damaged_file\n");
    GUARDED(nothingToKeep());
    GUARDED(keptAside());
    GUARDED(sameSecondGetsASuffix());
    GUARDED(onlyTheThreeNewestAreKept());
    GUARDED(renameRefused());
    if (g_checksFailed == 0) { fs::remove_all(g_scratch, ec); }
    return testSummary("test_damaged_file");
}
