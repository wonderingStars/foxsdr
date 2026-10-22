// A damaged settings file is KEPT, never saved over, by the headless engine (engine extraction stage 7c): the
// analogue of the window's 0.99.65 rule (src/core/damaged_file.hpp), on the real foxsdr-engine executable.
//
// THE RULE. A settings file that fails to load (cut off by a crash mid-write, edited by hand with a quote missing,
// written by a newer version) leaves the run on defaults - and the first change, or the clean exit, would then save
// a good file over it. The damaged one, which still held the whole configuration, would be gone. So at start-up,
// where the load failed and before anything can save, the engine renames it aside to "<name>.bad-<UTC stamp>" in
// the same folder (the three newest copies of a file are kept); if the rename is REFUSED (another program holds the
// file) the file is left exactly as it is and this run saves nothing at all.
//
// WHAT IS ASSERTED, on a scratch settings file named on the command line:
//   1. DAMAGED, KEPT: a truncated file -> exit 0; the run says the file was damaged and on defaults; ONE
//      "config.json.bad-*" copy exists and holds the damaged bytes exactly (a hash-free byte compare against what was
//      seeded); config.json is a NEW, whole file that loads, with the clean-exit marker true;
//   2. a second run on that whole file keeps nothing aside (no second copy);
//   3. EMPTY: a zero-byte file is not worth keeping (nothing to lose): replaced, no copy, nothing said of damage;
//   4. MISSING: a first run - defaults, the file created, no copy;
//   5. KEPT-ASIDE REFUSED (Windows: the damaged file held open by this test with no write or delete sharing, as an
//      editor or an antivirus scan holds one): exit 0, the run says nothing will be saved over it, NO copy exists
//      and the damaged file is byte for byte what it was - the run saved nothing, not even the marker.
//
// Break-it (docs/engine-stage4.md 6.17): EngineService::restore does not call setAsideDamaged() -> 1 is RED (the damaged
// bytes are gone: there is no copy and config.json is the new file) and 5 is RED (the run saved over a file it could not
// keep); setAsideDamaged does not forbid the writer when the rename is refused -> 5 is RED.
//
// HERMETIC: scratch profile, the settings file a scratch path named on the command line, the real %APPDATA% config
// never named. A damaged, empty or missing file means DEFAULTS (volume 0.5 on the demo generator) and the volume
// cannot be seeded in a file the loader refuses, so these runs are the ones of this binary's tests that can be
// heard: each is `--seconds 0` (start, report running, stop), a fraction of a second of the demo tones.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/config.hpp"
#include "engine_main_proc.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace emt = engine_main_test;
namespace fs = std::filesystem;

namespace {

void writeBytes(const fs::path& p, const std::string& bytes) {
    std::ofstream out(p, std::ios::binary);
    out << bytes;
}

// The kept copies beside `cfg`: "config.json.bad-<stamp>[-N]".
std::vector<fs::path> keptCopies(const fs::path& cfg) {
    std::vector<fs::path> out;
    std::error_code ec;
    const std::string prefix = cfg.filename().string() + ".bad-";
    for (fs::directory_iterator it(cfg.parent_path(), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.rfind(prefix, 0) == 0) { out.push_back(it->path()); }
    }
    return out;
}

bool loads(const fs::path& cfg) {
    cascade::core::AppConfig c;
    std::string err;
    return cascade::core::ConfigStore::load(cfg.string(), c, err);
}

}  // namespace

int main() {
    std::printf("test_engine_main_damaged_config\n");
    const fs::path root = emt::scratch("engine-damaged");
    const fs::path home = root / "home";
    const fs::path cwd = home / "cwd";
    fs::create_directories(home / "tmp");
    fs::create_directories(cwd);
    emt::isolate(home);
    CHECK(cascade::core::ConfigStore::defaultPath().rfind(home.string(), 0) == 0);
    const fs::path engineExe = emt::appExe("foxsdr-engine");
    CHECK(fs::exists(engineExe));
    const fs::path outDir = root / "out";
    fs::create_directories(outDir);

    // --- 1. DAMAGED: kept aside, then a whole new file ------------------------------------------------------------
    {
        const fs::path dir = home / "d1";
        fs::create_directories(dir);
        const fs::path cfg = dir / "config.json";
        // Cut off mid-write: a real crash's file, with settings in it worth keeping.
        const std::string damaged = "{\n    \"mode\": \"USB\",\n    \"volume\": 0.25,\n    \"mapPages\": [{\"plugin\": \"Radar Sweep\", \"x\": 12";
        writeBytes(cfg, damaged);
        CHECK(!loads(cfg));
        const emt::RunResult r = emt::run(engineExe, {"--config", cfg.string(), "--seconds", "0"}, cwd, outDir, "d1", 120000);
        std::printf("  1: exit %d\n%s", r.exitCode, r.err.c_str());
        CHECK(r.started && !r.timedOut && r.exitCode == 0);
        CHECK(emt::contains(r.err, ": running, settings file in use (damaged, defaults in use)\n"));
        const std::vector<fs::path> kept = keptCopies(cfg);
        CHECK(kept.size() == 1);
        if (kept.size() == 1) { CHECK(emt::readFile(kept[0]) == damaged); }  // byte for byte what was damaged
        CHECK(loads(cfg));                                               // config.json is a whole new file
        CHECK(emt::contains(emt::readFile(cfg), "\"telemetryCleanExit\": true"));
        CHECK(!emt::contains(emt::readFile(cfg), "Radar Sweep"));        // and it is the defaults', not the damaged file's

        // --- 2. the next run on the whole file keeps nothing -------------------------------------------------------
        const emt::RunResult again = emt::run(engineExe, {"--config", cfg.string(), "--seconds", "0"}, cwd, outDir, "d1b", 120000);
        CHECK(again.started && !again.timedOut && again.exitCode == 0);
        CHECK(emt::contains(again.err, "settings file in use (restored)"));
        CHECK(keptCopies(cfg).size() == 1);
    }

    // --- 3. EMPTY: nothing to lose, nothing kept -------------------------------------------------------------------
    {
        const fs::path dir = home / "d3";
        fs::create_directories(dir);
        const fs::path cfg = dir / "config.json";
        writeBytes(cfg, std::string());
        const emt::RunResult r = emt::run(engineExe, {"--config", cfg.string(), "--seconds", "0"}, cwd, outDir, "d3", 120000);
        CHECK(r.started && !r.timedOut && r.exitCode == 0);
        CHECK(keptCopies(cfg).empty());
        CHECK(loads(cfg));
        CHECK(fs::file_size(cfg) > 0);
    }

    // --- 4. MISSING: a first run ------------------------------------------------------------------------------------
    {
        const fs::path dir = home / "d4";
        const fs::path cfg = dir / "config.json";  // the folder is not there either
        CHECK(!fs::exists(dir));
        const emt::RunResult r = emt::run(engineExe, {"--config", cfg.string(), "--seconds", "0"}, cwd, outDir, "d4", 120000);
        CHECK(r.started && !r.timedOut && r.exitCode == 0);
        CHECK(emt::contains(r.err, "settings file in use (restored)"));  // a first run is a good load of the defaults
        CHECK(keptCopies(cfg).empty());
        CHECK(fs::exists(cfg) && loads(cfg));
    }

    // --- 5. the rename REFUSED: the damaged file is held by another program --------------------------------------
#if defined(_WIN32)
    {
        const fs::path dir = home / "d5";
        fs::create_directories(dir);
        const fs::path cfg = dir / "config.json";
        const std::string damaged = "{\"mode\": \"USB\", \"volume\":";
        writeBytes(cfg, damaged);
        // Read-shared only: others may read it (the loader does) but nobody may rename, delete or write it.
        HANDLE held = ::CreateFileW(cfg.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(held != INVALID_HANDLE_VALUE);
        const emt::RunResult r = emt::run(engineExe, {"--config", cfg.string(), "--seconds", "0"}, cwd, outDir, "d5", 120000);
        std::printf("  5: exit %d\n%s", r.exitCode, r.err.c_str());
        CHECK(r.started && !r.timedOut && r.exitCode == 0);
        CHECK(emt::contains(r.err, "could not be kept aside, so nothing will be saved over it in this run"));
        CHECK(emt::contains(r.err, "foxsdr-engine: config: not saved - the damaged settings file could not be kept aside"));  // the writer was FORBIDDEN, not merely blocked
        CHECK(emt::contains(r.err, "foxsdr-engine: settings saved 0 time(s)"));  // a refused request is not counted as a save
        if (held != INVALID_HANDLE_VALUE) { ::CloseHandle(held); }
        CHECK(keptCopies(cfg).empty());
        CHECK(emt::readFile(cfg) == damaged);  // untouched: the run saved nothing, not even the marker
    }
#else
    SKIP_LINUX("the refused rename is staged with a Windows share mode");
#endif

    std::error_code ec;
    if (g_checksFailed == 0) { fs::remove_all(root, ec); }
    return testSummary("test_engine_main_damaged_config");
}
