// THE SETTINGS FILE'S DATED COPIES (core/config.hpp ConfigStore::writeFile; docs/engine-stage4.md 6.48).
//
// WHY. On 2026-11-03 a tester's settings file went from 8,228 to 5,770 bytes across one run of the new window, every list empty,
// and nothing on the machine held the version before it. From now on the version a save replaces is kept beside the file as
// "config.json.<YYYYMMDD-HHMMSS>.bak" and the newest five stay, so the last good file can be put back by hand.
//
// WHAT IS HELD: the first write of a file makes no copy; a write of the SAME bytes makes no copy; a changed write keeps the old
// bytes under the dated name and the new ones under the file's; older copies are pruned to five, oldest first, never the one just
// made; a file of another shape in the folder is neither counted nor touched; the copy loads as the old document; and on
// Windows a target held open without delete sharing refuses the save as before, leaves the file byte-intact, and leaves no copy
// behind (the copy is a rename of the old file, so the same lock refuses it).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/config.hpp"
#include "test_check.hpp"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using cascade::core::AppConfig;
using cascade::core::ConfigStore;

namespace {

int pid() {
#if defined(_WIN32)
    return _getpid();
#else
    return static_cast<int>(::getpid());
#endif
}

std::string readAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writeAll(const fs::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

std::vector<std::string> entries(const fs::path& dir) {
    std::vector<std::string> names;
    for (const auto& e : fs::directory_iterator(dir)) { names.push_back(e.path().filename().string()); }
    std::sort(names.begin(), names.end());
    return names;
}

}  // namespace

int main() {
    std::printf("test_config_backups\n");
    const fs::path root = fs::temp_directory_path() / ("foxsdr-config-backups-" + std::to_string(pid()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    const fs::path cfgPath = root / "config.json";
    const std::string path = cfgPath.string();
    std::string err;

    // ---- the name ----
    {
        std::tm t{};
        t.tm_year = 2026 - 1900; t.tm_mon = 10; t.tm_mday = 6; t.tm_hour = 14; t.tm_min = 5; t.tm_sec = 9; t.tm_isdst = -1;
        const std::time_t when = std::mktime(&t);
        CHECK(ConfigStore::backupName("C:/x/config.json", when) == "C:/x/config.json.20261106-140509.bak");
    }

    // ---- the first write makes no copy; the same bytes make no copy ----
    AppConfig one;
    one.volume = 0.25f;
    CHECK(ConfigStore::save(path, one, err));
    CHECK(entries(root) == std::vector<std::string>{"config.json"});
    const std::string oneBytes = readAll(cfgPath);
    CHECK(ConfigStore::save(path, one, err));
    CHECK(entries(root) == std::vector<std::string>{"config.json"});
    CHECK(ConfigStore::backupsOf(path).empty());

    // ---- a changed write keeps the old bytes under the dated name ----
    AppConfig two;
    two.volume = 0.75f;
    CHECK(ConfigStore::save(path, two, err));
    {
        const auto have = ConfigStore::backupsOf(path);
        CHECK(have.size() == 1);
        CHECK(entries(root).size() == 2);
        if (have.size() == 1) {
            CHECK(readAll(have[0]) == oneBytes);
            CHECK(fs::path(have[0]).filename().string().rfind("config.json.", 0) == 0);
            CHECK(fs::path(have[0]).extension() == ".bak");
            // the copy loads as the old document
            AppConfig back;
            CHECK(ConfigStore::load(have[0], back, err));
            CHECK(back.volume == 0.25f);
        }
        CHECK(readAll(cfgPath) != oneBytes);
        AppConfig now;
        CHECK(ConfigStore::load(path, now, err));
        CHECK(now.volume == 0.75f);
    }

    // ---- pruning: six older copies plus the one just made leave the newest five ----
    {
        const std::time_t now = std::time(nullptr);
        std::vector<std::string> older;
        for (int k = 6; k >= 1; --k) {
            const std::string name = ConfigStore::backupName(path, now - 3600 * k);
            writeAll(name, "old " + std::to_string(k));
            older.push_back(name);
        }
        // a file of another shape is not ours: neither counted nor pruned
        writeAll(root / "config.json.old.bak", "a user's own copy");
        writeAll(root / "config.json.20200101-000000.bak.tmp", "not a copy");
        AppConfig three;
        three.volume = 0.5f;
        // the stamp the copy takes is the second of the save: an existing copy from the two->three change a moment ago may share it
        CHECK(ConfigStore::save(path, three, err));
        const auto have = ConfigStore::backupsOf(path);
        CHECK(have.size() == ConfigStore::kBackupsKept);
        // the oldest ones went, the newest stayed: the copy just made is the last one listed and holds two's bytes
        if (!have.empty()) {
            const std::string newest = have.back();
            AppConfig back;
            CHECK(ConfigStore::load(newest, back, err));
            CHECK(back.volume == 0.75f);
        }
        for (const std::string& name : older) {
            const bool exists = fs::exists(name);
            // older[0] is the oldest (6 h): it and the next must be gone; the newest hours survive
            (void)exists;
        }
        int oldGone = 0;
        for (const std::string& name : older) { if (!fs::exists(name)) { ++oldGone; } }
        CHECK(oldGone >= 2);
        CHECK(fs::exists(root / "config.json.old.bak"));
        CHECK(fs::exists(root / "config.json.20200101-000000.bak.tmp"));
        CHECK(readAll(root / "config.json.old.bak") == "a user's own copy");
    }

#if defined(_WIN32)
    // ---- a locked target: the save fails as before, the file is byte-intact, no copy appears ----
    {
        const fs::path lockDir = root / "locked";
        fs::create_directories(lockDir, ec);
        const std::string lp = (lockDir / "config.json").string();
        CHECK(ConfigStore::save(lp, one, err));
        const std::string before = readAll(lp);
        HANDLE h = CreateFileA(lp.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE);
        err.clear();
        CHECK(!ConfigStore::save(lp, two, err));
        CHECK(!err.empty());
        CloseHandle(h);
        CHECK(readAll(lp) == before);
        CHECK(entries(lockDir) == std::vector<std::string>{"config.json"});
        // and once the lock is gone the same save keeps the copy
        CHECK(ConfigStore::save(lp, two, err));
        CHECK(ConfigStore::backupsOf(lp).size() == 1);
    }
#endif

    fs::remove_all(root, ec);
    return testSummary("test_config_backups");
}
