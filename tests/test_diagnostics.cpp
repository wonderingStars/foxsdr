// Tests for the local diagnostics capture: the log ring, the rotating file,
// the module table that makes a captured stack symbolisable, the crash
// signature that groups reports, and the "copy diagnostics" bundle.
//
// The load-bearing ones are easy to get subtly wrong and impossible to notice
// in the field:
//
//   - A ring that keeps the FIRST N lines instead of the LAST N looks
//     perfectly healthy in a report and carries the least useful part of the
//     session. It is asserted by content, not by size.
//   - A build id read from the running image that does not match the build id
//     read from the file on disk would make the whole symbol archive useless
//     while every individual piece still looked correct. Both are read and
//     compared.
//   - A bundle that quietly grows a field breaks the privacy promise in
//     PRIVACY.md. The inventory is compared in BOTH directions, so an added
//     field fails just as loudly as a removed one.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "core/crash_handler.hpp"
#include "core/diag_log.hpp"
#include "core/diag_report.hpp"
#include "core/version.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace cascade::core;

namespace {

// A private directory per run. The pid keeps two concurrent test executables
// (ctest runs them in parallel) from colliding on the same fixture, which is
// a failure mode this project has hit before.
fs::path scratchDir(const char* tag) {
    const char* tmp = std::getenv("TEMP");
    const fs::path base = (tmp != nullptr && *tmp != '\0') ? fs::path(tmp) : fs::path(".");
#if defined(_WIN32)
    const unsigned long pid = ::GetCurrentProcessId();
#else
    const unsigned long pid = 0;
#endif
    const fs::path dir =
        base / (std::string("cascade-diag-") + tag + "-" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(dir, ec);
    return dir;
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// This executable's own path, used as a PE whose build id must be readable
// both from the loaded image and from the file on disk.
std::string selfExePath() {
#if defined(_WIN32)
    char buf[MAX_PATH] = {};
    const DWORD n = ::GetModuleFileNameA(nullptr, buf, static_cast<DWORD>(sizeof(buf)));
    return std::string(buf, buf + n);
#else
    return std::string();
#endif
}

// Something with a known address inside this module, so resolveAddress has a
// target whose module identity the test already knows.
void addressAnchor() { std::printf(""); }

// BOUNDS-SAFE INDEXING, and it is not tidiness. CHECK records and continues,
// so `v.front()` guarded only by a preceding size CHECK is an out-of-bounds
// read in exactly the run that has something to report: the ring comes back
// empty, the size check goes red, and then front() reads past the end and the
// process dies at 0xC0000005 instead of naming the broken expectation. This
// project has already lost a red-phase run to that pattern twice. Returning a
// default-constructed element keeps every assertion below live and failing on
// its own terms.
std::string at(const std::vector<std::string>& v, std::size_t i) {
    return (i < v.size()) ? v[i] : std::string();
}

#if defined(_WIN32)

// A minimal config the REAL binary will load under CASCADE_CONFIG_TEST. Only
// the fields these tests turn on are written; everything else defaults, which
// is what a first-run config looks like anyway.
void writeConfig(const fs::path& p, bool diagnostics, const std::string& iqPath) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << "{\n";
    out << "  \"schemaVersion\": 1,\n";
    out << "  \"diagnosticsEnabled\": " << (diagnostics ? "true" : "false") << ",\n";
    out << "  \"diagnosticsMinidump\": false";
    if (!iqPath.empty()) {
        out << ",\n";
        out << "  \"sourceKind\": \"file\",\n";
        out << "  \"iqFilePath\": \"" << iqPath << "\"";
    }
    out << "\n}\n";
}

// Runs the shipped binary as a bounded run with BOTH the diagnostics tree and
// the config redirected into caller-owned scratch directories, and returns its
// combined output. This is the only way to see main()'s ORDERING: every unit
// test in this file calls configure()/installCrashHandlers() directly and so
// cannot tell whether the application consulted the user's switch before or
// after it created the directories.
std::string runApp(const fs::path& diagDir, const fs::path& cfgPath, int frames) {
    ::SetEnvironmentVariableA("FOXSDR_DIAG_DIR", diagDir.string().c_str());
    ::SetEnvironmentVariableA("CASCADE_CONFIG_TEST", cfgPath.string().c_str());
    const std::string exe = std::string(CASCADE_APP_BINDIR) + "/cascade.exe";
    const std::string cmd = "\"\"" + exe + "\" --frames " + std::to_string(frames) + " 2>&1\"";
    std::string out;
    FILE* p = _popen(cmd.c_str(), "r");
    char buf[512];
    while (p != nullptr && std::fgets(buf, sizeof(buf), p) != nullptr) { out += buf; }
    if (p != nullptr) { _pclose(p); }
    ::SetEnvironmentVariableA("FOXSDR_DIAG_DIR", nullptr);
    ::SetEnvironmentVariableA("CASCADE_CONFIG_TEST", nullptr);
    return out;
}

// `git rev-parse --short=12 HEAD` in a given tree, or empty.
std::string gitHeadShort(const std::string& tree) {
    const std::string cmd = "\"\"" + std::string(CASCADE_GIT_EXECUTABLE) + "\" -C \"" + tree +
                            "\" rev-parse --short=12 HEAD 2>nul\"";
    std::string out;
    FILE* p = _popen(cmd.c_str(), "r");
    char buf[256];
    while (p != nullptr && std::fgets(buf, sizeof(buf), p) != nullptr) { out += buf; }
    if (p != nullptr) { _pclose(p); }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) { out.pop_back(); }
    return out;
}

// Is this tree the commit it names? A modified OR untracked-source tree is
// not, which is the whole point of the dirty marker: an engineer told to
// "check out this commit" gets a tree that does not contain the code the
// offsets came from. Same question the generator asks, asked the same way.
std::string gitRead(const std::string& tree, const std::string& args) {
    const std::string cmd = "\"\"" + std::string(CASCADE_GIT_EXECUTABLE) + "\" -C \"" + tree +
                            "\" " + args + " 2>nul\"";
    std::string out;
    FILE* p = _popen(cmd.c_str(), "r");
    char buf[512];
    while (p != nullptr && std::fgets(buf, sizeof(buf), p) != nullptr) { out += buf; }
    if (p != nullptr) { _pclose(p); }
    return out;
}

// THE SAME TWO-HALF RULE cmake/git-commit.cmake applies, deliberately spelled
// out here rather than shelling out to the generator: a test that asked the
// implementation what it thinks would agree with it however wrong it was.
//
// Half one is any modified tracked file. Half two is untracked files, but ONLY
// under the paths a build compiles from - because this repository permanently
// carries untracked files that are nobody's mistake (docs/LICENSING.md,
// third_party/tweetnacl/), and counting those marked EVERY build from a
// spotless release commit as dirty. A marker that is always on says nothing.
bool gitTreeDirty(const std::string& tree) {
    if (!gitRead(tree, "status --porcelain --untracked-files=no").empty()) { return true; }
    return !gitRead(tree, "ls-files --others --exclude-standard -- src tests cmake CMakeLists.txt")
                .empty();
}

int runQuiet(const std::string& cmd) {
    const std::string wrapped = "\"" + cmd + " >nul 2>nul\"";
    return std::system(wrapped.c_str());
}

#endif  // _WIN32

}  // namespace

int main() {
    // --- The ring keeps the LAST N lines, not the first N -------------------
    //
    // Written with the file disabled so this block is a pure memory test: the
    // ring's contract does not depend on anything being on disk.
    {
        DiagLog& log = DiagLog::instance();
        log.resetForTest();
        log.configure(std::string(), false);

        const int total = 1000;
        for (int i = 0; i < total; ++i) {
            log.writef("info", "line %d", i);
        }

        const std::vector<std::string> ring = log.ringSnapshot();
        CHECK(ring.size() == static_cast<std::size_t>(DiagLog::kRingLines));
        CHECK(log.linesWritten() == static_cast<std::uint64_t>(total));

        // The decisive pair. A first-N ring passes the size check above and
        // fails both of these.
        const std::size_t n = ring.size();
        const int firstKept = total - DiagLog::kRingLines;
        CHECK(at(ring, 0).find("line " + std::to_string(firstKept)) != std::string::npos);
        CHECK(at(ring, n - 1).find("line " + std::to_string(total - 1)) != std::string::npos);
        CHECK(n >= 2);
        CHECK(at(ring, 0).find("line 0 ") == std::string::npos);

        // ...and the same property through the allocation-free reader the
        // crash path actually uses, because that is a separate piece of code
        // and could keep a different end of the ring.
        std::vector<char> raw(DiagLog::kRingLines * DiagLog::kLineBytes + 1);
        const std::size_t used = log.copyRingRaw(raw.data(), raw.size());
        CHECK(used > 0);
        const std::string flat(raw.data());
        CHECK(flat.find("line 999") != std::string::npos);
        CHECK(flat.find("line 743") == std::string::npos);
        CHECK(flat.find("line 744") != std::string::npos);
    }

    // --- Nothing is written when the feature is off -------------------------
    {
        const fs::path dir = scratchDir("off");
        DiagLog& log = DiagLog::instance();
        log.resetForTest();
        log.configure(dir.string(), false);

        for (int i = 0; i < 200; ++i) { log.writef("info", "should not reach disk %d", i); }

        CHECK(!log.fileEnabled());
        CHECK(log.filePath().empty());
        // Not "the file is empty" - the DIRECTORY must not even exist. A
        // feature that is off leaves no trace it was ever considered.
        CHECK(!fs::exists(dir));
        // The ring still runs: it never leaves the process, and a user who
        // turns diagnostics on mid-session should not have to reproduce the
        // fault before there is anything to look at.
        CHECK(log.ringSnapshot().size() == 200u);
    }

#if defined(_WIN32)
    // --- ...and off means off THROUGH THE REAL BINARY'S START-UP ORDERING ---
    //
    // The block above proves DiagLog obeys its switch. It cannot prove the
    // APPLICATION does, because it calls configure() itself and so says nothing
    // about WHEN main() calls it. That distinction is not academic: arming the
    // on-disk half before the config has been read means a user who opted out
    // gets a log file and an armed crash handler for the whole of start-up -
    // the config load, the GL context, and the LoadLibrary of every third-party
    // plugin, which is the most fault-prone part of the run.
    //
    // So this launches the shipped binary twice with the same poisoned config
    // and only the switch different. The ON run is the positive control:
    // without it, an OFF run that failed to start at all would pass.
    {
        const fs::path root = scratchDir("offapp");
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root, ec);

        // A file name that is unmistakably the user's own data, and that
        // contains a number that would read as a frequency in a report.
        const std::string iq = "C:/Users/steve/Recordings/marine-vhf-156800-private.wav";

        // ON: the tree appears, and the log records the session.
        const fs::path onDir = root / "on";
        const fs::path onCfg = root / "on-config.json";
        fs::create_directories(onDir, ec);
        writeConfig(onCfg, true, iq);
        const std::string onOut = runApp(onDir, onCfg, 3);
        std::printf("%s", onOut.c_str());
        CHECK(onOut.find("rendered 3 frames") != std::string::npos);
        const fs::path onLog = onDir / "logs" / "foxsdr.log";
        CHECK(fs::exists(onLog));
        const std::string onText = readFile(onLog);
        CHECK(onText.find("starting") != std::string::npos);

        // THE PRIVACY HALF, on the REAL ring rather than on log lines a test
        // supplied itself. Opening an I/Q file and then moving it is the most
        // ordinary trigger there is, and the failure branch used to log the
        // source's error string - which is "cannot open file: <full path>".
        // Everything that carries the ring carries it: every crash report,
        // every hang report, and the Copy diagnostics bundle.
        CHECK(onText.find("the saved I/Q file did not reopen") !=
              std::string::npos);  // the branch really ran
        CHECK(onText.find("marine-vhf") == std::string::npos);
        CHECK(onText.find("156800") == std::string::npos);
        CHECK(onText.find("Recordings") == std::string::npos);
        CHECK(onText.find(".wav") == std::string::npos);

        // OFF: nothing. Not a log, not an empty directory, not the crashes
        // folder the crash handler creates when it arms.
        const fs::path offDir = root / "off";
        const fs::path offCfg = root / "off-config.json";
        fs::create_directories(offDir, ec);
        writeConfig(offCfg, false, iq);
        const std::string offOut = runApp(offDir, offCfg, 3);
        std::printf("%s", offOut.c_str());
        CHECK(offOut.find("rendered 3 frames") != std::string::npos);
        CHECK(!fs::exists(offDir / "logs"));
        CHECK(!fs::exists(offDir / "crashes"));
        CHECK(fs::is_empty(offDir));

        if (g_checksFailed == 0) { fs::remove_all(root, ec); }
    }
#endif  // _WIN32

    // --- The file, and rotation --------------------------------------------
    {
        const fs::path dir = scratchDir("rotate");
        DiagLog& log = DiagLog::instance();
        log.resetForTest();
        log.configure(dir.string(), true);
        CHECK(log.fileEnabled());
        CHECK(!log.filePath().empty());

        log.write("info", "first line on disk");
        CHECK(fs::exists(fs::path(log.filePath())));
        CHECK(readFile(fs::path(log.filePath())).find("first line on disk") !=
              std::string::npos);

        // Push past the rotation bound. Each line is padded so the byte count
        // is predictable rather than a function of the loop counter's width.
        const std::string pad(150, 'x');
        const std::size_t needed = DiagLog::kRotateBytes / 150u + 64u;
        for (std::size_t i = 0; i < needed; ++i) { log.writef("info", "%s", pad.c_str()); }

        const fs::path rotated = dir / "foxsdr.1.log";
        CHECK(fs::exists(rotated));
        // The live file must have been TRUNCATED by the rotation, not merely
        // joined by a sibling.
        CHECK(fs::file_size(fs::path(log.filePath())) < DiagLog::kRotateBytes);
        // And the oldest content went to the rotated file, so the history is
        // still there to be read.
        CHECK(readFile(rotated).find("first line on disk") != std::string::npos);

        // HOW MANY FILES ARE KEPT, which is a number the header states and
        // nothing checked. kKeptFiles counts the live file, so three means
        // foxsdr.log, .1 and .2 - about 3 MiB of a user's profile. An
        // off-by-one in the shift loop keeps one more file than promised, and
        // it only appears from the THIRD rotation onwards: the first two leave
        // three files either way.
        for (int rotation = 0; rotation < 2; ++rotation) {
            for (std::size_t i = 0; i < needed; ++i) { log.writef("info", "%s", pad.c_str()); }
        }
        int logFiles = 0;
        std::error_code cec;
        for (const auto& e : fs::directory_iterator(dir, cec)) {
            const std::string leaf = e.path().filename().string();
            if (leaf.rfind("foxsdr", 0) == 0 && e.path().extension() == ".log") { ++logFiles; }
        }
        std::printf("log files kept after three rotations: %d (kKeptFiles %d)\n", logFiles,
                    DiagLog::kKeptFiles);
        CHECK(logFiles == DiagLog::kKeptFiles);
        CHECK(fs::exists(dir / "foxsdr.log"));
        // Derived from the constant rather than spelled out, so raising the
        // bound stays a one-line change instead of a red test that is wrong.
        for (int i = 1; i <= DiagLog::kKeptFiles - 1; ++i) {
            CHECK(fs::exists(dir / ("foxsdr." + std::to_string(i) + ".log")));
        }
        CHECK(!fs::exists(dir / ("foxsdr." + std::to_string(DiagLog::kKeptFiles) + ".log")));

        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    // --- The module table, and the build id that makes it symbolisable ------
    {
        const int n = refreshModuleTable();
        CHECK(n > 0);
        CHECK(moduleCount() == n);

        DiagModule m;
        std::uintptr_t offset = 0;
        const auto anchor = reinterpret_cast<std::uintptr_t>(&addressAnchor);
        CHECK(resolveAddress(anchor, m, offset));
        const std::string name(m.name);
        CHECK(name.find(".exe") != std::string::npos);
        CHECK(offset < m.size);
        CHECK(m.base != 0);

        // The whole point: the running image's key must be exactly the key
        // the archiver read from the file on disk, or the archive indexes
        // symbols by something no report will ever quote.
        std::string fileId;
        std::string filePdb;
        CHECK(peBuildId(selfExePath(), fileId, filePdb));
        CHECK(!fileId.empty());
        CHECK(fileId == std::string(m.buildId));
        CHECK(filePdb == std::string(m.pdb));
        CHECK(filePdb.size() > 4);
        CHECK(filePdb.find(".pdb") != std::string::npos);
        // 32 GUID hex digits plus an age of at least one digit.
        CHECK(fileId.size() >= 33);
        for (char c : fileId) {
            CHECK((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'));
        }

        // A nonsense address belongs to no module and must say so rather than
        // resolving to whichever record happens to be first.
        DiagModule junk;
        std::uintptr_t junkOffset = 0;
        CHECK(!resolveAddress(static_cast<std::uintptr_t>(1), junk, junkOffset));
    }

    // --- The release binary's symbols are archived under its build id -------
    //
    // This is the one that is fatal if missed: without the PDB that matches
    // this exact link, every offset in every report ever filed against this
    // build is unreadable forever. The archive layout is the symbol-server
    // one, <pdb name>/<build id>/<pdb name>, so the same store can be handed
    // straight to a debugger.
    {
        const fs::path exe = fs::path(CASCADE_APP_BINDIR) / "cascade.exe";
        CHECK(fs::exists(exe));
        std::string id;
        std::string pdb;
        CHECK(peBuildId(exe.string(), id, pdb));
        CHECK(!id.empty());
        const fs::path archived = fs::path(CASCADE_SYMBOL_ARCHIVE_DIR) / pdb / id / pdb;
        std::printf("symbol archive lookup: %s\n", archived.string().c_str());
        CHECK(fs::exists(archived));
        // The non-throwing overload, for the same reason as the plugin-style
        // block below: a missing archived PDB is exactly what a regression
        // here looks like, and the throwing overload turns it into a
        // 0xC0000409 fail-fast that names nothing instead of a red check that
        // names the property.
        std::error_code exeSizeEc;
        const std::uintmax_t exeArchivedSize = fs::file_size(archived, exeSizeEc);
        CHECK(!exeSizeEc && exeArchivedSize > 0);
    }

#if defined(_WIN32)
    // --- The archiver works for a module that is NOT cascade.exe ------------
    //
    // A report's --- modules --- block carries a build id for every loaded
    // plugin DLL, and plugins are third-party code running in-process: they
    // are the module class MOST likely to be the faulting one. The POST_BUILD
    // step in CMakeLists.txt runs for the `cascade` target only, and no plugin
    // is built in this tree, so nothing here can archive one - the plugin
    // repository has to call this script itself. That is only worth
    // documenting if it actually works, so it is run here against a PE that is
    // not cascade.exe (this test binary), into a scratch archive root.
    //
    // The index is asserted at the same time, because it is the file
    // docs/DIAGNOSTICS.md names as the way to map a build id to a release and
    // it was being written with a UTF-8 BOM: row 1 began EF BB BF instead of a
    // date, so every anchored parse silently skipped the oldest entry - the
    // one row that can never be re-derived from a later build.
    {
        const fs::path root = scratchDir("plugarchive");
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root, ec);

        const std::string self = selfExePath();
        std::string id;
        std::string pdb;
        CHECK(peBuildId(self, id, pdb));
        CHECK(!id.empty());

        const std::string script = std::string(CASCADE_SOURCE_DIR) + "/tools/archive-symbols.ps1";
        const std::string cmd = "powershell -NoProfile -ExecutionPolicy Bypass -File \"" +
                                script + "\" -Binary \"" + self + "\" -ArchiveRoot \"" +
                                root.string() + "\" -Version \"0.61.0\" -Commit \"" +
                                std::string(cascade::gitCommit()) + "\"";
        CHECK(runQuiet(cmd) == 0);

        const fs::path archived = root / pdb / id / pdb;
        std::printf("plugin-style archive lookup: %s\n", archived.string().c_str());
        CHECK(fs::exists(archived));
        // The non-throwing overload deliberately: the throwing one turns a
        // missing file - which is exactly what a regression here looks like -
        // into a 0xC0000409 fail-fast that names nothing, instead of a red
        // check that names the property. Found doing the break-it pass on this
        // very test.
        std::error_code sizeEc;
        const std::uintmax_t archivedSize = fs::file_size(archived, sizeEc);
        CHECK(!sizeEc && archivedSize > 0);

        // THE INDEX, BYTE BY BYTE AT THE FRONT. A BOM here is invisible in
        // every editor and fatal to `grep "^2026"`.
        const fs::path index = root / "index.txt";
        CHECK(fs::exists(index));
        const std::string text = readFile(index);
        CHECK(text.size() > 40);
        // Bounds-safe, for the same reason: an empty index must fail the
        // checks below rather than read past the end of the string.
        const auto byteAt = [&text](std::size_t i) -> unsigned char {
            return (i < text.size()) ? static_cast<unsigned char>(text[i]) : 0u;
        };
        std::printf("index first bytes: %02X %02X %02X\n", byteAt(0), byteAt(1), byteAt(2));
        CHECK(byteAt(0) != 0xEFu);
        CHECK(byteAt(0) >= '0' && byteAt(0) <= '9');

        // ...and the row is usable: six tab-separated columns, none of them
        // empty. Four rows in the real index carried an empty or placeholder
        // commit column, which is the difference between "this build id is
        // release X" and a dead end.
        std::vector<std::string> cols;
        std::size_t at = 0;
        const std::size_t eol = text.find('\n');
        const std::string row = text.substr(0, (eol == std::string::npos) ? text.size() : eol);
        while (at <= row.size()) {
            const std::size_t tab = row.find('\t', at);
            const std::size_t end = (tab == std::string::npos) ? row.size() : tab;
            std::string col = row.substr(at, end - at);
            while (!col.empty() && (col.back() == '\r')) { col.pop_back(); }
            cols.push_back(col);
            if (tab == std::string::npos) { break; }
            at = tab + 1;
        }
        std::printf("index row has %zu columns\n", cols.size());
        CHECK(cols.size() == 6u);
        for (const std::string& c : cols) { CHECK(!c.empty()); }
        // The module archived is this test binary, not the application: the
        // point of the exercise.
        CHECK(cols.size() == 6u && cols[1].find("test_diagnostics") != std::string::npos);
        CHECK(cols.size() == 6u && cols[3] == std::string(cascade::gitCommit()));
        CHECK(cols.size() == 6u && cols[4] == id);

        if (g_checksFailed == 0) { fs::remove_all(root, ec); }
    }

    // --- THE LIVE INDEX, not a scratch one the script just created ----------
    //
    // The block above proves the script WRITES a good index into an empty
    // archive root. That is structurally blind to the defect it was written
    // for: the file that was actually broken is the accumulated one in
    // symbols\, and the copy on nas:/volume1/foxsdr-symbols that
    // docs/DIAGNOSTICS.md calls the copy which must outlive this machine. A
    // freshly created index cannot carry a historical bad row, so both real
    // files could be BOM-ed and stuffed with empty commit columns with this
    // suite entirely green - and one of them was, for the whole time the
    // scratch-root check existed.
    //
    // So every row of the real archive index is read here. It is skipped, with
    // a printed reason rather than silently, when the archive does not exist
    // (a fresh clone that has not built yet, or a build whose POST_BUILD step
    // was not run) - a missing archive is a different fault, asserted by the
    // cascade.exe block above.
    {
        const fs::path liveIndex = fs::path(CASCADE_SYMBOL_ARCHIVE_DIR) / "index.txt";
        std::printf("live symbol index: %s\n", liveIndex.string().c_str());
        std::error_code liveEc;
        if (!fs::exists(liveIndex, liveEc)) {
            std::printf("live index absent - nothing archived on this machine yet\n");
        } else {
            const std::string text = readFile(liveIndex);
            CHECK(text.size() > 40);
            const auto byteAt = [&text](std::size_t i) -> unsigned char {
                return (i < text.size()) ? static_cast<unsigned char>(text[i]) : 0u;
            };
            std::printf("live index first bytes: %02X %02X %02X\n",
                        byteAt(0), byteAt(1), byteAt(2));
            // EF BB BF is the UTF-8 BOM. It is invisible in every editor and
            // it makes `grep "^2026"` skip row 1 - the one row that can never
            // be re-derived from a later build.
            CHECK(byteAt(0) != 0xEFu);
            CHECK(byteAt(0) >= '0' && byteAt(0) <= '9');

            // ...and every row is usable: six tab-separated columns, none of
            // them empty, each starting with a date. Four rows in this file
            // carried an empty or `test123` commit column, which is the
            // difference between "this build id is release X" and a dead end.
            std::size_t rows = 0;
            std::size_t badRows = 0;
            std::size_t pos = 0;
            while (pos < text.size()) {
                std::size_t eol = text.find('\n', pos);
                std::string row = text.substr(pos, (eol == std::string::npos)
                                                       ? std::string::npos
                                                       : (eol - pos));
                pos = (eol == std::string::npos) ? text.size() : (eol + 1);
                while (!row.empty() && (row.back() == '\r')) { row.pop_back(); }
                if (row.empty()) { continue; }
                ++rows;

                std::vector<std::string> cols;
                std::size_t at = 0;
                while (true) {
                    const std::size_t tab = row.find('\t', at);
                    const std::size_t end = (tab == std::string::npos) ? row.size() : tab;
                    cols.push_back(row.substr(at, end - at));
                    if (tab == std::string::npos) { break; }
                    at = tab + 1;
                }

                bool ok = (cols.size() == 6u);
                if (ok) {
                    for (const std::string& c : cols) {
                        if (c.empty()) { ok = false; }
                    }
                }
                // An anchored parse is how the docs tell a reader to use this
                // file, so a row that such a parse would drop is a bad row
                // whatever else is right about it.
                if (ok && !(row[0] >= '0' && row[0] <= '9')) { ok = false; }
                if (!ok) {
                    ++badRows;
                    std::printf("live index bad row %zu (%zu columns): %s\n",
                                rows, cols.size(), row.c_str());
                }
            }
            std::printf("live index: %zu rows, %zu bad\n", rows, badRows);
            CHECK(rows > 0);
            CHECK(badRows == 0);
        }
    }

    // --- The commit a report quotes is THIS build's, not an older one --------
    //
    // A version names a release; only the commit names a build, and the whole
    // point of carrying it is that an engineer can check out the exact tree the
    // offsets were produced from. A commit read once at CMake CONFIGURE time
    // does not do that: CMake re-configures when CMakeLists.txt changes, not
    // when HEAD moves, so every build after the next commit quotes the PREVIOUS
    // one. That is worse than "unknown", because it sends the reader to a tree
    // that exists and is wrong.
    {
        const std::string head = gitHeadShort(CASCADE_SOURCE_DIR);
        std::printf("git HEAD now: %s, binary says: %s\n", head.c_str(), cascade::gitCommit());
        // Only meaningful in a git checkout; a source tarball legitimately
        // reports "unknown", and that is stated rather than asserted away.
        if (!head.empty()) {
            // AND THE TREE, not just the commit. A bare SHA from a modified
            // tree is the same failure as a stale SHA: it names a tree that
            // exists and is not the one the offsets came from. Measured on
            // this repository - the binary said 5ba13f6d0c86 while `git
            // status` listed 32 entries including the entire diagnostics
            // feature. The expected value is derived from the tree's ACTUAL
            // state, so this asserts the marker appears when it should and,
            // just as importantly, does not appear when it should not.
            const bool dirty = gitTreeDirty(CASCADE_SOURCE_DIR);
            const std::string expect = dirty ? (head + "-dirty") : head;
            std::printf("tree is %s, so the commit field must read %s\n",
                        dirty ? "MODIFIED" : "clean", expect.c_str());
            CHECK(std::string(cascade::gitCommit()) == expect);
        } else {
            std::printf("not a git checkout: commit freshness not asserted\n");
            CHECK(std::string(cascade::gitCommit()) == std::string("unknown"));
        }

        // THE MECHANISM, tested where it can actually be made to fail: the
        // generator is run twice against a scratch repository whose HEAD moves
        // between the runs, with no configure step in between. A generator that
        // caches, or that is only ever run at configure time, produces the same
        // answer twice.
        const fs::path repo = scratchDir("gitrepo");
        std::error_code ec;
        fs::remove_all(repo, ec);
        fs::create_directories(repo, ec);
        const std::string q = "\"" + repo.string() + "\"";
        const std::string git = "\"" + std::string(CASCADE_GIT_EXECUTABLE) + "\"";
        const std::string id = " -c user.email=t@example.invalid -c user.name=t ";
        CHECK(runQuiet(git + " init -q " + q) == 0);
        CHECK(runQuiet(git + " -C " + q + id + "commit -q --allow-empty -m one") == 0);

        // The generated headers go OUTSIDE the repository. They used to be
        // written into it, which made the tree untracked-dirty from the first
        // run onwards - harmless while only the SHA was asserted, and fatal to
        // the dirty-marker assertions below, which need a genuinely clean tree
        // to prove the marker is ABSENT when it should be.
        const fs::path outDir = scratchDir("gitgen");
        fs::remove_all(outDir, ec);
        fs::create_directories(outDir, ec);
        const fs::path out1 = outDir / "one.h";
        const fs::path out2 = outDir / "two.h";
        const std::string script = std::string(CASCADE_SOURCE_DIR) + "/cmake/git-commit.cmake";
        const std::string gen = "\"" + std::string(CASCADE_CMAKE_COMMAND) + "\" -DGIT=" + git +
                                " -DTREE=" + q + " -DOUT=";
        CHECK(runQuiet(gen + "\"" + out1.string() + "\" -P \"" + script + "\"") == 0);
        const std::string first = gitHeadShort(repo.string());
        CHECK(!first.empty());
        CHECK(readFile(out1).find(first) != std::string::npos);

        // HEAD moves. Nothing else does.
        CHECK(runQuiet(git + " -C " + q + id + "commit -q --allow-empty -m two") == 0);
        const std::string second = gitHeadShort(repo.string());
        CHECK(!second.empty());
        CHECK(second != first);

        CHECK(runQuiet(gen + "\"" + out2.string() + "\" -P \"" + script + "\"") == 0);
        const std::string text2 = readFile(out2);
        std::printf("generator: %s then %s\n", first.c_str(), second.c_str());
        CHECK(text2.find(second) != std::string::npos);
        CHECK(text2.find(first) == std::string::npos);

        // THE DIRTY MARKER. "commit: names the exact tree to check out" is a
        // sentence in docs/DIAGNOSTICS.md and it was false: a bare SHA from a
        // modified tree sends the reader to a tree that exists and does not
        // contain the code the offsets came from - the same failure as the
        // stale SHA above, and worse than "unknown" for the same reason.
        // build-nightly.ps1 appends ".dirty" to the VERSION, but a
        // hand-compiled installer never goes near that script.
        //
        // Both arms, because a marker that is always on is no marker at all:
        // the two runs above were made against a CLEAN tree and must carry no
        // marker.
        const std::string text1 = readFile(out1);
        CHECK(text1.find("-dirty") == std::string::npos);
        CHECK(text2.find("-dirty") == std::string::npos);

        // An UNTRACKED source file. It is exactly as absent from the
        // checked-out tree as a modified one, and on this project most of the
        // diagnostics feature was untracked at the moment the binary was
        // reporting a bare SHA - so `git diff --quiet` would have missed it.
        // It must sit where a build COMPILES FROM. That is the whole rule: an
        // untracked .cpp under src/ enters the binary, so the SHA no longer
        // describes what was built.
        const fs::path out3 = outDir / "three.h";
        fs::create_directories(repo / "src" / "core");
        {
            std::ofstream extra(repo / "src" / "core" / "extra.cpp",
                                std::ios::binary | std::ios::trunc);
            extra << "// not committed\n";
        }
        CHECK(runQuiet(gen + "\"" + out3.string() + "\" -P \"" + script + "\"") == 0);
        const std::string text3 = readFile(out3);
        std::printf("untracked source present, generator says: %s", text3.c_str());
        CHECK(text3.find(second + "-dirty") != std::string::npos);

        // ...AND THE OTHER HALF OF THAT RULE, which is the one a spotless
        // release build depends on: an untracked file OUTSIDE those paths must
        // NOT mark the tree. This repository permanently carries
        // docs/LICENSING.md and third_party/tweetnacl/, and when every
        // untracked path counted, every release build recorded "-dirty" - a
        // marker that is always on, which a reader correctly learns to ignore.
        const fs::path out3b = outDir / "three_b.h";
        fs::remove(repo / "src" / "core" / "extra.cpp");
        fs::create_directories(repo / "docs");
        {
            std::ofstream note(repo / "docs" / "NOTE.md", std::ios::binary | std::ios::trunc);
            note << "not committed, and not compiled\n";
        }
        CHECK(runQuiet(gen + "\"" + out3b.string() + "\" -P \"" + script + "\"") == 0);
        const std::string text3b = readFile(out3b);
        std::printf("untracked NON-source present, generator says: %s", text3b.c_str());
        CHECK(text3b.find("-dirty") == std::string::npos);
        CHECK(text3b.find(second) != std::string::npos);
        fs::remove(repo / "docs" / "NOTE.md");
        {
            std::ofstream extra(repo / "src" / "core" / "extra.cpp",
                                std::ios::binary | std::ios::trunc);
            extra << "// not committed\n";
        }

        // ...and a TRACKED modification, once that file is committed. HEAD
        // moves again here, so the SHA is re-read as well.
        const fs::path out4 = outDir / "four.h";
        CHECK(runQuiet(git + " -C " + q + id + "add src/core/extra.cpp") == 0);
        CHECK(runQuiet(git + " -C " + q + id + "commit -q -m three") == 0);
        const std::string third = gitHeadShort(repo.string());
        CHECK(!third.empty());
        CHECK(third != second);
        {
            std::ofstream extra(repo / "src" / "core" / "extra.cpp",
                                std::ios::binary | std::ios::trunc);
            extra << "// committed, then edited\n";
        }
        CHECK(runQuiet(gen + "\"" + out4.string() + "\" -P \"" + script + "\"") == 0);
        const std::string text4 = readFile(out4);
        std::printf("tracked edit present, generator says: %s", text4.c_str());
        CHECK(text4.find(third + "-dirty") != std::string::npos);

        if (g_checksFailed == 0) {
            fs::remove_all(repo, ec);
            fs::remove_all(outDir, ec);
        }
    }
#endif  // _WIN32

    // --- The crash signature groups by fault, not by run --------------------
    {
        const std::string a = crashSignature(0xC0000005ul, "SoapyUHD.dll", 0x1a2b);
        const std::string b = crashSignature(0xC0000005ul, "SoapyUHD.dll", 0x1a2b);
        const std::string c = crashSignature(0xC0000005ul, "SoapyUHD.dll", 0x1a2c);
        const std::string d = crashSignature(0xC0000094ul, "SoapyUHD.dll", 0x1a2b);
        const std::string e = crashSignature(0xC0000005ul, "cascade.exe", 0x1a2b);
        CHECK(!a.empty());
        CHECK(a == b);   // two runs of the same fault group together
        CHECK(a != c);   // a different offset is a different bug
        CHECK(a != d);   // a different exception kind is a different bug
        CHECK(a != e);   // a different module is a different bug
    }

    // --- The bundle carries exactly the fields PRIVACY.md documents ---------
    {
        DiagContext ctx;
        ctx.version = "0.61.0";
        ctx.commit = "abcdef123456";
        ctx.os = "Windows 10.0.22631";
        ctx.arch = "x64";
        ctx.mode = "WFM";
        ctx.sourceKind = "soapy";
        ctx.sampleRateHz = 2400000.0;
        ctx.deviceOpen = true;
        ctx.sdrModel = "uhd b200";
        ctx.plugins.push_back("ADS-B 1.1.0");
        ctx.plugins.push_back("AIS 1.0.0");

        DiagBundleInput in;
        in.context = ctx;
        in.logLines.push_back("2026-08-24 10:00:00 info source opened");
        in.logLines.push_back("2026-08-24 10:00:01 warn audio stream reopened");
        in.logPath = "C:/Users/x/AppData/Local/FoxSDR/logs/foxsdr.log";
        in.crashDir = "C:/Users/x/AppData/Local/FoxSDR/crashes";
        in.lastRunUnclean = true;
        in.launches = 12;
        in.crashes = 1;
        in.logLinesTotal = 4011;

        const std::string bundle = buildDiagnosticsBundle(in);
        CHECK(!bundle.empty());

        // Everything the docs promise is present, by value not just by label.
        CHECK(bundle.find("version: 0.61.0") != std::string::npos);
        CHECK(bundle.find("commit: abcdef123456") != std::string::npos);
        CHECK(bundle.find("plugin: ADS-B 1.1.0") != std::string::npos);
        CHECK(bundle.find("plugin: AIS 1.0.0") != std::string::npos);
        CHECK(bundle.find("sdr-model: uhd b200") != std::string::npos);
        CHECK(bundle.find("last-run-unclean: yes") != std::string::npos);
        CHECK(bundle.find("audio stream reopened") != std::string::npos);
        CHECK(bundle.find("log-lines-total: 4011") != std::string::npos);

        // THE INVENTORY, both directions. Collect every "name: " label in the
        // header block and compare it with the declared list as a set: an
        // added field fails here, and so does a field the documentation
        // claims but the bundle stopped emitting.
        std::set<std::string> declared(bundleFieldNames().begin(), bundleFieldNames().end());
        CHECK(!declared.empty());
        std::set<std::string> emitted;
        std::size_t pos = 0;
        const std::size_t logStart = bundle.find("--- log ---");
        CHECK(logStart != std::string::npos);
        while (pos < logStart) {
            const std::size_t eol = bundle.find('\n', pos);
            const std::size_t end = (eol == std::string::npos) ? bundle.size() : eol;
            const std::string line = bundle.substr(pos, end - pos);
            const std::size_t colon = line.find(": ");
            const bool isField = colon != std::string::npos && colon > 0 &&
                                 line.compare(0, 3, "---") != 0;
            emitted.insert(isField ? line.substr(0, colon) : std::string("(header)"));
            pos = end + 1;
        }
        emitted.erase("(header)");
        CHECK(emitted == declared);

        // The negative half of the privacy promise: a bundle is a support
        // artefact, not a listening record.
        CHECK(bundle.find("Hz\n") == std::string::npos);
        CHECK(bundle.find("centre") == std::string::npos);
        CHECK(bundle.find("center") == std::string::npos);
        CHECK(bundle.find("bookmark") == std::string::npos);
    }

    // --- The context block is rendered on the healthy path ------------------
    {
        DiagContext ctx;
        ctx.version = "0.61.0";
        ctx.commit = "deadbeef";
        ctx.mode = "NFM";
        ctx.sourceKind = "iqfile";
        ctx.sampleRateHz = 1000000.0;
        ctx.plugins.push_back("POCSAG 1.0.0");
        setDiagContext(ctx);
        const std::string block = diagContextBlock();
        CHECK(block.find("0.61.0") != std::string::npos);
        CHECK(block.find("deadbeef") != std::string::npos);
        CHECK(block.find("NFM") != std::string::npos);
        CHECK(block.find("POCSAG 1.0.0") != std::string::npos);
    }

    return testSummary("test_diagnostics");
}
