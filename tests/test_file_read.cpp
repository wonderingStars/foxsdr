// A file that cannot be read never ends the program (core/file_read.hpp, 0.99.65).
//
// FOUND ON LINUX. tests/test_failure_files.cpp aborted in 0.08 s there: "terminate called after
// throwing an instance of 'std::__ios_failure' what(): basic_filebuf::underflow error reading the
// file: Is a directory". On Linux an ifstream opens a DIRECTORY and the READ then fails - by throwing,
// out of the stream's buffer; the loaders read the buffer through nlohmann's stream adapter or an
// istreambuf_iterator, neither of which catches it. On Windows the open fails and nothing throws, so
// the failure cannot be met here with a directory. This test meets it the platform-independent way:
//
//   1. THE MECHANISM. A buffer that throws from underflow - what libstdc++ does - makes
//      json::parse(stream) and istreambuf_iterator throw, and slurpStream (istream::read) does not.
//   2. readTextFile's answers: ok (small, empty, bigger than one block), a file that is not there, a
//      directory, and a read that throws.
//   3. THE LOADERS: settings, bookmarks, markers and a band plan, each with (a) a directory where the
//      file should be and (b) a file whose read throws (the seam): false, with the reason, no throw.
//   4. THE OTHER READERS that open a fixed name in a folder the user can write to: the plugin
//      manifest, the pending-removal list and an imported frequency list.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <exception>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <istream>
#include <streambuf>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#include "core/band_plan.hpp"
#include "core/config.hpp"
#include "core/file_read.hpp"
#include "core/freq_import.hpp"
#include "core/freq_manager.hpp"
#include "core/freq_markers.hpp"
#include "core/plugin_cleanup.hpp"
#include "core/plugin_repo.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using cascade::core::ReadResult;
using cascade::core::readTextFile;
using cascade::core::slurpStream;

namespace {

fs::path g_scratch;

void putText(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
}

bool contains(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

// A buffer whose read fails by throwing, as libstdc++'s filebuf does.
class ThrowingBuf final : public std::streambuf {
protected:
    int_type underflow() override { throw std::ios_base::failure("simulated read error"); }
};

// The seam: the paths whose read must throw.
std::string g_failPath;
bool failThisPath(const std::string& path) { return !g_failPath.empty() && path == g_failPath; }

struct FailRead {
    explicit FailRead(const fs::path& p) {
        g_failPath = p.string();
        cascade::core::setReadFaultHookForTest(&failThisPath);
    }
    ~FailRead() {
        cascade::core::setReadFaultHookForTest(nullptr);
        g_failPath.clear();
    }
};

void theMechanism() {
    {
        ThrowingBuf buf;
        std::istream is(&buf);
        bool threw = false;
        try {
            (void)nlohmann::json::parse(is, nullptr, /*allow_exceptions=*/false);
        } catch (...) {
            threw = true;
        }
        CHECK(threw);  // what the loaders did: the exception goes straight out, whatever allow_exceptions says
    }
    {
        ThrowingBuf buf;
        std::istream is(&buf);
        bool threw = false;
        try {
            const std::string s((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
            (void)s;
        } catch (...) {
            threw = true;
        }
        CHECK(threw);
    }
    {
        ThrowingBuf buf;
        std::istream is(&buf);
        std::string out = "stale";
        bool threw = false;
        bool ok = true;
        try {
            ok = slurpStream(is, out);
        } catch (...) {
            threw = true;
        }
        CHECK(!threw);
        CHECK(!ok);
        CHECK(out.empty());
    }
    std::printf("  a buffer that throws: json::parse and istreambuf_iterator throw, slurpStream answers false\n");
}

void readTextFileAnswers() {
    const fs::path dir = g_scratch / "answers";
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::string text = "stale";

    putText(dir / "small.txt", "hello\r\nworld");
    CHECK(readTextFile(dir / "small.txt", text) == ReadResult::Ok);
    CHECK(text == "hello\r\nworld");  // binary: no newline translation

    putText(dir / "empty.txt", "");
    CHECK(readTextFile((dir / "empty.txt").string(), text) == ReadResult::Ok);
    CHECK(text.empty());

    std::string big;
    for (int i = 0; i < 40000; ++i) { big += static_cast<char>('a' + i % 26); }  // several blocks
    putText(dir / "big.txt", big);
    CHECK(readTextFile((dir / "big.txt").string().c_str(), text) == ReadResult::Ok);
    CHECK(text == big);

    CHECK(readTextFile(dir / "missing.txt", text) == ReadResult::CannotOpen);
    CHECK(text.empty());

    fs::create_directories(dir / "folder.json", ec);
    CHECK(readTextFile(dir / "folder.json", text) == ReadResult::IsDirectory);

    {
        FailRead fail(dir / "small.txt");
        CHECK(readTextFile(dir / "small.txt", text) == ReadResult::ReadError);  // a throwing buffer: no throw
        CHECK(readTextFile(dir / "big.txt", text) == ReadResult::Ok);           // other paths are untouched
    }
    CHECK(readTextFile(dir / "small.txt", text) == ReadResult::Ok);
    std::printf("  readTextFile: ok (small, empty, 40 kB), CannotOpen, IsDirectory, ReadError - never a throw\n");
}

// One loader's two bad cases, answered false with a reason and no throw.
template <class Load>
void badCases(const char* what, const fs::path& dir, const std::string& goodText, Load&& load) {
    // (a) A DIRECTORY where the file should be.
    const fs::path folder = dir / (std::string(what) + "-folder.json");
    std::error_code ec;
    fs::create_directories(folder, ec);
    {
        std::string err;
        bool threw = false;
        bool ok = true;
        try {
            ok = load(folder, err);
        } catch (const std::exception& e) {
            threw = true;
            std::printf("  %s: a directory made it throw: %s\n", what, e.what());
        } catch (...) {
            threw = true;
        }
        CHECK(!threw);
        CHECK(!ok);
        CHECK(!err.empty());
        CHECK(contains(err, "directory"));
        std::printf("  %s, a directory: false - \"%s\"\n", what, err.substr(0, 28).c_str());
    }
    // (b) A FILE WHOSE READ THROWS (the seam): a good file, so only the read can be what fails.
    const fs::path good = dir / (std::string(what) + "-good.json");
    putText(good, goodText);
    {
        std::string err;
        bool ok = false;
        {
            std::string okErr;
            CHECK(load(good, okErr));  // unreadable by the seam only: the file itself is fine
            FailRead fail(good);
            bool threw = false;
            try {
                ok = load(good, err);
            } catch (const std::exception& e) {
                threw = true;
                std::printf("  %s: a failing read made it throw: %s\n", what, e.what());
            } catch (...) {
                threw = true;
            }
            CHECK(!threw);
        }
        CHECK(!ok);
        CHECK(contains(err, "could not be read"));
        std::printf("  %s, a read that throws: false - \"%s\"\n", what, err.substr(0, 28).c_str());
    }
}

void theLoaders() {
    const fs::path dir = g_scratch / "loaders";
    std::error_code ec;
    fs::create_directories(dir, ec);

    {
        cascade::core::AppConfig cfg;
        cfg.sourceKind = "siggen";
        badCases("settings", dir, cascade::core::ConfigStore::serialize(cfg),
                 [](const fs::path& p, std::string& err) {
                     cascade::core::AppConfig out;
                     return cascade::core::ConfigStore::load(p.string(), out, err);
                 });
    }
    {
        cascade::core::FreqManager m;
        cascade::core::Bookmark b;
        b.name = "One";
        b.freqHz = 100.0e6;
        b.mode = "NFM";
        m.add(b);
        badCases("bookmarks", dir, m.serialize(), [](const fs::path& p, std::string& err) {
            cascade::core::FreqManager out;
            return out.load(p.string(), err);
        });
    }
    {
        cascade::core::FreqMarkers m;
        m.add(90.0e6, 1700000000);
        badCases("markers", dir, m.serialize(), [](const fs::path& p, std::string& err) {
            cascade::core::FreqMarkers out;
            return out.load(p.string(), err);
        });
    }
    {
        const std::string plan =
            "{\"name\":\"Test plan\",\"bands\":[{\"startHz\":88000000,\"endHz\":108000000,"
            "\"name\":\"FM\",\"service\":\"broadcast\"}]}\n";
        badCases("band plan", dir, plan, [](const fs::path& p, std::string& err) {
            cascade::core::BandPlan out;
            return out.loadFile(p.string(), err);
        });
    }
}

// The readers that open a fixed name in a folder the user can write to.
void theOtherReaders() {
    const fs::path dir = g_scratch / "others";
    std::error_code ec;
    fs::create_directories(dir, ec);

    // The pending-removal list: a directory on its name, and a read that throws, both read as "none".
    {
        const fs::path name = dir / cascade::core::pendingRemovalFileName();
        fs::create_directories(name, ec);
        bool threw = false;
        std::vector<std::string> got;
        try {
            got = cascade::core::loadPendingRemovals(dir.string());
        } catch (...) {
            threw = true;
        }
        CHECK(!threw);
        CHECK(got.empty());
        fs::remove_all(name, ec);
        putText(name, "{\"files\":[\"a-1.0.0.dll\"]}");
        CHECK(cascade::core::loadPendingRemovals(dir.string()).size() == 1u);
        {
            FailRead fail(name);
            threw = false;
            try {
                got = cascade::core::loadPendingRemovals(dir.string());
            } catch (...) {
                threw = true;
            }
            CHECK(!threw);
            CHECK(got.empty());
        }
    }
    // The plugin manifest.
    {
        const fs::path pdir = dir / "plugins";
        fs::create_directories(pdir, ec);
        const fs::path manifest = cascade::core::PluginRepo::manifestPath(pdir.string());
        fs::create_directories(manifest, ec);  // a directory squatting on installed.json
        cascade::core::PluginInventory inv;
        std::string err;
        bool threw = false;
        try {
            (void)cascade::core::PluginRepo::loadInventory(pdir.string(), inv, err);
        } catch (...) {
            threw = true;
        }
        CHECK(!threw);
        fs::remove_all(manifest, ec);
        putText(manifest, "{\"schemaVersion\":1,\"plugins\":[]}");
        {
            FailRead fail(manifest);
            threw = false;
            try {
                (void)cascade::core::PluginRepo::loadInventory(pdir.string(), inv, err);
            } catch (...) {
                threw = true;
            }
            CHECK(!threw);
        }
        // forgetFile (reached by Remove) reads it too.
        putText(pdir / "gone-1.0.0.dll", "x");
        {
            FailRead fail(manifest);
            threw = false;
            cascade::core::PluginRepo repo;
            try {
                (void)repo.remove(pdir.string(), "gone-1.0.0.dll", err);
            } catch (...) {
                threw = true;
            }
            CHECK(!threw);
        }
    }
    // An imported frequency list: a dropped FOLDER, and a read that throws.
    {
        const fs::path folder = dir / "list-folder.csv";
        fs::create_directories(folder, ec);
        bool threw = false;
        cascade::core::ImportResult r;
        try {
            r = cascade::core::importFrequencyFile(folder.string());
        } catch (...) {
            threw = true;
        }
        CHECK(!threw);
        CHECK(!r.error.empty());
        CHECK(r.items.empty());
        const fs::path list = dir / "list.csv";
        putText(list, "frequency,name\n100100000,One\n");
        {
            FailRead fail(list);
            threw = false;
            try {
                r = cascade::core::importFrequencyFile(list.string());
            } catch (...) {
                threw = true;
            }
            CHECK(!threw);
            CHECK(!r.error.empty());
            CHECK(contains(r.error, "cannot read"));
            CHECK(r.items.empty());
        }
    }
    std::printf("  plugin manifest, pending-removal list, imported list: a folder or a failing read is "
                "answered, never thrown\n");
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
    g_scratch = fs::temp_directory_path() / ("foxsdr_file_read_" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    fs::create_directories(g_scratch, ec);
    std::printf("test_file_read\n");
    GUARDED(theMechanism());
    GUARDED(readTextFileAnswers());
    GUARDED(theLoaders());
    GUARDED(theOtherReaders());
    if (g_checksFailed == 0) { fs::remove_all(g_scratch, ec); }
    return testSummary("test_file_read");
}
