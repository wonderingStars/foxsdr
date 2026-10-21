// A file is never opened over one that is already there (core/unique_file.hpp, 0.99.65).
//
// The unit under test is the name search and the exclusive create. What matters, each asserted:
//   * the numbering: "name.ext", "name-2.ext" ... with the number before the LAST extension;
//   * an existing file - a stranger's, a read-only one, a folder - is stepped past, never opened over;
//   * the search is capped at 99, and fails without touching anything past it;
//   * any failure that is not "exists" (the folder is not there) ends it at once;
//   * the check and the create are one operation: eight threads asking for the same name all get
//     different files, none twice and none missing.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <cstdio>
#include <exception>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "core/unique_file.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using cascade::core::createExclusive;
using cascade::core::createUnique;
using cascade::core::CreateResult;
using cascade::core::numberedPath;
using cascade::core::reserveUnique;

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

void numbering() {
    CHECK(numberedPath("a/b.wav", 1) == "a/b.wav");
    CHECK(numberedPath("a/b.wav", 0) == "a/b.wav");
    CHECK(numberedPath("a/b.wav", 2) == "a/b-2.wav");
    CHECK(numberedPath("a\\b.c\\name", 3) == "a\\b.c\\name-3");        // a dot in the folder is not an extension
    CHECK(numberedPath("noext", 2) == "noext-2");
    CHECK(numberedPath(".hidden", 2) == ".hidden-2");                  // a leading dot is not an extension
    CHECK(numberedPath("x.tar.gz", 2) == "x.tar-2.gz");                // before the LAST extension
    CHECK(numberedPath("dir/audio_20261005_101010_48000Hz.wav", 10) ==
          "dir/audio_20261005_101010_48000Hz-10.wav");
    CHECK(numberedPath("dir/foxsdr-frequencies-20261005-101010.xml", 2) ==
          "dir/foxsdr-frequencies-20261005-101010-2.xml");
}

void stepsPastWhatIsThere() {
    const fs::path dir = g_scratch / "steps";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string wanted = (dir / "take.wav").string();

    // A stranger's file on the first name: not opened over, and the second name is used.
    putText(dir / "take.wav", "STRANGER");
    std::string used;
    bool exhausted = true;
    int index = 0;
    std::FILE* f = createUnique(wanted, used, exhausted, &index);
    CHECK(f != nullptr);
    CHECK(!exhausted);
    CHECK(index == 2);
    CHECK(used == (dir / "take-2.wav").string());
    if (f != nullptr) {
        std::fputs("second", f);
        std::fclose(f);
    }
    CHECK(readText(dir / "take.wav") == "STRANGER");
    CHECK(readText(dir / "take-2.wav") == "second");

    // The next ask goes to the third.
    f = createUnique(wanted, used, exhausted, &index);
    CHECK(f != nullptr);
    CHECK(index == 3);
    if (f != nullptr) { std::fclose(f); }

    // A FOLDER on a name is stepped past as well (Windows answers "access denied", not "exists").
    fs::create_directories(dir / "take-4.wav", ec);
    f = createUnique(wanted, used, exhausted, &index);
    CHECK(f != nullptr);
    CHECK(index == 5);
    if (f != nullptr) { std::fclose(f); }
    CHECK(fs::is_directory(dir / "take-4.wav", ec));

    // A READ-ONLY file too.
    putText(dir / "take-6.wav", "READ-ONLY");
    fs::permissions(dir / "take-6.wav", fs::perms::owner_read, fs::perm_options::replace, ec);
    f = createUnique(wanted, used, exhausted, &index);
    CHECK(f != nullptr);
    CHECK(index == 7);
    if (f != nullptr) { std::fclose(f); }
    fs::permissions(dir / "take-6.wav", fs::perms::owner_all, fs::perm_options::replace, ec);
    CHECK(readText(dir / "take-6.wav") == "READ-ONLY");

    // createExclusive on its own: Created once, then Exists.
    CreateResult r = CreateResult::Failed;
    std::FILE* g = createExclusive((dir / "once.bin").string(), r);
    CHECK(g != nullptr);
    CHECK(r == CreateResult::Created);
    if (g != nullptr) { std::fclose(g); }
    CHECK(createExclusive((dir / "once.bin").string(), r) == nullptr);
    CHECK(r == CreateResult::Exists);

    // reserveUnique makes the file, empty.
    CHECK(reserveUnique((dir / "reserved.mp3").string(), used, exhausted, &index));
    CHECK(index == 1);
    CHECK(fs::exists(dir / "reserved.mp3", ec));
    CHECK(fs::file_size(dir / "reserved.mp3", ec) == 0u);
    CHECK(reserveUnique((dir / "reserved.mp3").string(), used, exhausted, &index));
    CHECK(index == 2);
    std::printf("  a stranger's file, a folder and a read-only file on the names: stepped past, untouched\n");
}

void theSearchIsCapped() {
    const fs::path dir = g_scratch / "capped";
    std::error_code ec;
    fs::create_directories(dir, ec);
    for (int n = 1; n <= cascade::core::kMaxUniqueNames; ++n) {
        putText(dir / (n == 1 ? std::string("t.wav") : "t-" + std::to_string(n) + ".wav"),
                "TAKEN-" + std::to_string(n));
    }
    std::string used = "unset";
    bool exhausted = false;
    CHECK(createUnique((dir / "t.wav").string(), used, exhausted) == nullptr);
    CHECK(exhausted);
    CHECK(!reserveUnique((dir / "t.wav").string(), used, exhausted));
    CHECK(exhausted);
    std::size_t count = 0;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        (void)e;
        ++count;
    }
    CHECK(count == 99u);
    CHECK(readText(dir / "t.wav") == "TAKEN-1");
    CHECK(readText(dir / "t-99.wav") == "TAKEN-99");
    std::printf("  99 names taken: the search fails, exhausted, and no file is added or touched\n");
}

void anyOtherFailureEndsItAtOnce() {
    std::string used;
    bool exhausted = true;
    // The folder is not there: not "exists", so no stepping, and not "exhausted".
    CHECK(createUnique((g_scratch / "no-such-folder" / "t.wav").string(), used, exhausted) == nullptr);
    CHECK(!exhausted);
    CreateResult r = CreateResult::Exists;
    CHECK(createExclusive((g_scratch / "no-such-folder" / "t.wav").string(), r) == nullptr);
    CHECK(r == CreateResult::Failed);
}

// ONE OPERATION, NOT A CHECK THEN AN OPEN: eight threads ask for the same name at once.
void raceForOneName() {
    const fs::path dir = g_scratch / "race";
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string wanted = (dir / "same-second.wav").string();
    constexpr int kThreads = 8;
    constexpr int kEach = 12;  // 96 files in all: inside the 99-name cap
    std::mutex m;
    std::vector<std::string> names;
    std::atomic<int> failed{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            while (!go.load()) { std::this_thread::yield(); }
            for (int i = 0; i < kEach; ++i) {
                std::string used;
                bool exhausted = false;
                std::FILE* f = createUnique(wanted, used, exhausted);
                if (f == nullptr) {
                    failed.fetch_add(1);
                    continue;
                }
                std::fputs(used.c_str(), f);  // each file holds its own name
                std::fclose(f);
                std::lock_guard<std::mutex> lock(m);
                names.push_back(used);
            }
        });
    }
    go.store(true);
    for (std::thread& th : threads) { th.join(); }
    CHECK(failed.load() == 0);
    CHECK(names.size() == static_cast<std::size_t>(kThreads * kEach));
    const std::set<std::string> distinct(names.begin(), names.end());
    CHECK(distinct.size() == names.size());  // no name twice
    std::size_t files = 0;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        ++files;
        CHECK(readText(e.path()) == e.path().string());  // and nothing was opened over
    }
    CHECK(files == names.size());
    std::printf("  %d threads, one name, %d asks each: %zu files, every name different, none overwritten\n",
                kThreads, kEach, files);
}

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
    g_scratch = fs::temp_directory_path() / ("foxsdr_unique_file_" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    fs::create_directories(g_scratch, ec);
    std::printf("test_unique_file\n");
    GUARDED(numbering());
    GUARDED(stepsPastWhatIsThere());
    GUARDED(theSearchIsCapped());
    GUARDED(anyOtherFailureEndsItAtOnce());
    GUARDED(raceForOneName());
    if (g_checksFailed == 0) { fs::remove_all(g_scratch, ec); }
    return testSummary("test_unique_file");
}
