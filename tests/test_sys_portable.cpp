// Test for core/sys_portable.hpp (lane sysmodels): the portable-mode path resolver. Everything happens in scratch folders under the temp directory (the PID in the name);
// the real %APPDATA%\foxsdr\config.json is never opened: the one test that goes through ConfigStore points APPDATA at a scratch folder FIRST and restores it after.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#ifdef _WIN32
#include <process.h>
#define TEST_GETPID _getpid
#else
#include <unistd.h>
#define TEST_GETPID getpid
#endif

#include "core/config.hpp"
#include "core/sys_portable.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using namespace cascade::core::sys;

namespace {

fs::path g_base;

void spit(const fs::path& p, const std::string& s) { std::ofstream f(p, std::ios::binary | std::ios::trunc); f << s; }

fs::path freshExeDir(const char* tag) {
    const fs::path d = g_base / tag / "stick";
    fs::remove_all(g_base / tag);
    fs::create_directories(d);
    return d;
}

std::set<std::string> listing(const fs::path& dir) {
    std::set<std::string> out;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        out.insert(fs::relative(it->path(), dir).generic_string());
    }
    return out;
}

void testResolve() {
    {   // no marker: not portable, and not an error
        const fs::path exe = freshExeDir("none");
        const auto p = resolvePortable(exe);
        CHECK(!p.markerFound && !p.portable && p.error.empty());
        CHECK(portableEnvOverrides(p).empty());
        std::string err;
        PortablePaths copy = p;
        CHECK(!ensurePortableRoots(exe, copy, err));
        CHECK(listing(exe).empty());   // asking for roots of a non-portable install created nothing
    }
    {   // an empty marker: the default folder
        const fs::path exe = freshExeDir("empty");
        spit(exe / kPortableMarkerName, "");
        const auto p = resolvePortable(exe);
        CHECK(p.markerFound && p.portable && p.error.empty());
        CHECK(p.root == exe / "FoxSDR-data" && p.settingsRoot == p.root / "settings" && p.dataRoot == p.root / "data" && p.logRoot == p.root / "logs");
        const auto env = portableEnvOverrides(p);
        CHECK(env.size() == 2 && env[0].first == "APPDATA" && env[0].second == p.settingsRoot.string() && env[1].first == "LOCALAPPDATA" && env[1].second == p.dataRoot.string());
        CHECK(listing(exe) == std::set<std::string>{kPortableMarkerName});   // resolving wrote nothing
    }
    {   // a named folder, tolerant of a byte-order mark, CRLF, trailing spaces and a second line
        const fs::path exe = freshExeDir("named");
        spit(exe / kPortableMarkerName, "\xEF\xBB\xBF  My Radio Stick  \r\nignored second line\r\n");
        const auto p = resolvePortable(exe);
        CHECK(p.portable && p.root == exe / "My Radio Stick");
        spit(exe / kPortableMarkerName, "   \n");   // whitespace only = default
        CHECK(resolvePortable(exe).root == exe / "FoxSDR-data");
    }
    {   // a marker that is not a usable file
        const fs::path exe = freshExeDir("badmarker");
        fs::create_directory(exe / kPortableMarkerName);
        auto p = resolvePortable(exe);
        CHECK(p.markerFound && !p.portable && !p.error.empty());
        fs::remove(exe / kPortableMarkerName);
        spit(exe / kPortableMarkerName, std::string(257, 'a'));
        p = resolvePortable(exe);
        CHECK(p.markerFound && !p.portable && !p.error.empty());
        spit(exe / kPortableMarkerName, std::string(256, 'a'));   // too long a NAME (64 limit) even though the file is at its size limit
        p = resolvePortable(exe);
        CHECK(p.markerFound && !p.portable && !p.error.empty());
    }
}

void testNames() {
    for (const char* ok : {"FoxSDR-data", "data", "a", "My Stick", "radio_1.v2", "A-b_c.d"}) { CHECK(validPortableFolderName(ok)); }
    for (const char* bad : {"", ".", "..", "../x", "a/b", "a\\b", "C:", "C:\\x", "/abs", "\\\\server\\share", "CON", "con", "Con.txt", "NUL", "aux.log", "COM1", "lpt9", "COM1.x", "name.",
                            "name ", " lead", "a..b", "a*b", "a?b", "a|b", "a<b", "tab\tname", "\xC3\xA9", "x\ny"}) {
        const bool v = validPortableFolderName(bad);
        if (v) { std::printf("accepted bad name: %s\n", bad); }
        CHECK(!v);
    }
    CHECK(validPortableFolderName(std::string(64, 'a')));
    CHECK(!validPortableFolderName(std::string(65, 'a')));
    CHECK(validPortableFolderName("COM0") && validPortableFolderName("COM10") && validPortableFolderName("CONSOLE"));   // not reserved
    // Every marker with a bad name makes portable mode invalid, with the marker file present.
    const fs::path exe = freshExeDir("badnames");
    for (const char* bad : {"..", "../x", "a/b", "C:", "CON"}) {
        spit(exe / kPortableMarkerName, bad);
        const auto p = resolvePortable(exe);
        CHECK(p.markerFound && !p.portable && !p.error.empty() && p.root.empty());
        CHECK(portableEnvOverrides(p).empty());
    }
}

void testEnsure() {
    const fs::path base = g_base / "ensure";
    fs::remove_all(base);
    const fs::path exe = base / "stick";
    fs::create_directories(exe);
    fs::create_directories(base / "canary");
    spit(base / "canary" / "keep.txt", "x");
    spit(exe / kPortableMarkerName, "");
    spit(exe / "foxsdr.exe", "not a program");
    auto p = resolvePortable(exe);
    const auto before = listing(base);
    std::string err;
    CHECK(ensurePortableRoots(exe, p, err) && err.empty());
    CHECK(fs::is_directory(p.settingsRoot) && fs::is_directory(p.dataRoot) && fs::is_directory(p.logRoot));
    // Everything new is inside the stick; nothing outside changed.
    const auto after = listing(base);
    std::set<std::string> added;
    for (const auto& a : after) { if (!before.count(a)) { added.insert(a); } }
    CHECK(added == (std::set<std::string>{"stick/FoxSDR-data", "stick/FoxSDR-data/settings", "stick/FoxSDR-data/data", "stick/FoxSDR-data/logs"}));
    for (const auto& b : before) { CHECK(after.count(b) == 1); }
    // Idempotent.
    CHECK(ensurePortableRoots(exe, p, err));
    CHECK(listing(base) == after);

    {   // a FILE where the data folder belongs: refused with a reason, nothing else made
        const fs::path e2 = base / "stick2";
        fs::create_directories(e2);
        spit(e2 / kPortableMarkerName, "");
        spit(e2 / "FoxSDR-data", "i am a file");
        auto p2 = resolvePortable(e2);
        CHECK(p2.portable);
        CHECK(!ensurePortableRoots(e2, p2, err) && !err.empty());
        CHECK(fs::is_regular_file(e2 / "FoxSDR-data"));
    }
    {   // a program folder that does not exist
        PortablePaths ghost = p;
        CHECK(!ensurePortableRoots(base / "no-such-stick", ghost, err) && !err.empty());
        CHECK(!fs::exists(base / "no-such-stick"));
    }
    {   // a link (junction) at the data folder's name that leads OUTSIDE the stick: refused, and the outside folder stays empty
        const fs::path e3 = base / "stick3";
        const fs::path outside = base / "outside";
        fs::create_directories(e3);
        fs::create_directories(outside);
        spit(e3 / kPortableMarkerName, "");
        bool linked = false;
        std::error_code ec;
        fs::create_directory_symlink(outside, e3 / "FoxSDR-data", ec);
        linked = !ec;
#ifdef _WIN32
        if (!linked) {   // no symlink privilege: a junction needs none (and raises no prompt)
            const std::string cmd = "\"mklink /J \"" + (e3 / "FoxSDR-data").string() + "\" \"" + outside.string() + "\" >NUL 2>&1\"";
            linked = (std::system(cmd.c_str()) == 0) && fs::exists(e3 / "FoxSDR-data");
        }
#endif
        if (linked) {
            auto p3 = resolvePortable(e3);
            CHECK(p3.portable);
            CHECK(!ensurePortableRoots(e3, p3, err) && !err.empty());
            CHECK(listing(outside).empty());
            // Remove the link itself, not what it points to.
            fs::remove(e3 / "FoxSDR-data", ec);
        } else {
            std::printf("link-escape case: could not make a symlink or a junction here; NOT RUN\n");
            ++g_checksSkipped;
        }
    }
}

// The wiring claim, proved: with the two overrides applied, the program's own ConfigStore resolves into the stick, and a save and a load go there.
void testThroughConfigStore() {
    const fs::path exe = freshExeDir("wiring");
    spit(exe / kPortableMarkerName, "");
    auto p = resolvePortable(exe);
    std::string err;
    CHECK(ensurePortableRoots(exe, p, err));

    const char* oldAppData = std::getenv("APPDATA");
    const std::string saved = oldAppData ? oldAppData : "";
    const bool had = oldAppData != nullptr;
#ifdef _WIN32
    for (const auto& kv : portableEnvOverrides(p)) {
        if (kv.first == "APPDATA") { _putenv_s("APPDATA", kv.second.c_str()); }   // LOCALAPPDATA is not touched by this test
    }
#else
    setenv("XDG_CONFIG_HOME", p.settingsRoot.string().c_str(), 1);
#endif
    const std::string path = cascade::core::ConfigStore::defaultPath();
    const fs::path want = p.settingsRoot / "foxsdr" / "config.json";
#ifdef _WIN32
    CHECK(fs::path(path) == want);
#endif
    cascade::core::AppConfig cfg;
    cfg.webPort = 4321;
    CHECK(cascade::core::ConfigStore::save(path, cfg, err));
    cascade::core::AppConfig back;
    CHECK(cascade::core::ConfigStore::load(path, back, err) && back.webPort == 4321);
    CHECK(fs::exists(want));
#ifdef _WIN32
    _putenv_s("APPDATA", had ? saved.c_str() : "");
#endif
    CHECK(!fs::exists(exe / "FoxSDR-data" / "data" / "foxsdr"));   // nothing else was created
}

}  // namespace

int main() {
    g_base = fs::temp_directory_path() / ("sysmodels-portable-" + std::to_string(TEST_GETPID()));
    fs::remove_all(g_base);
    fs::create_directories(g_base);
    testResolve();
    testNames();
    testEnsure();
    testThroughConfigStore();
    std::error_code ec;
    fs::remove_all(g_base, ec);
    return testSummary("test_sys_portable");
}
