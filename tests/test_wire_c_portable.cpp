// test_wire_c_portable.cpp - portable mode at the start of the process (wiring lane C row S1: src/engine/wire_c_portable.cpp, bootProcess STEP 2b).
//
//   P1  no marker: nothing changes, nothing is created, no variable is set
//   P2  an empty marker: APPDATA and LOCALAPPDATA are pointed INSIDE the program's folder (FoxSDR-data/settings and /data) and the folders are made; a marker that names a
//       folder uses it
//   P3  every marker that cannot be honoured is a REFUSAL (never a fall back): a name with a separator or "..", a reserved device name, a file where the folder belongs;
//       no variable is set and nothing outside the folder is created
//   P4  the order: bootProcess calls it after the helper modes and BEFORE the crash handlers read the log folder (from the source text)
//   P5  THE PROGRAM: a copy of foxsdr-engine.exe with a marker beside it, run with --version, writes its log and settings inside its own folder and the real per-user folders
//       are not where it looked (the log folder exists beside the program); a bad marker stops it with exit code 1 and the sentence in foxsdr-portable-error.txt
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/sys_portable.hpp"
#include "engine/wire_c_portable.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using cascade::engine::applyPortableMode;

namespace {

fs::path g_root;

fs::path fresh(const std::string& name) {
    const fs::path p = g_root / name;
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}

void writeFile(const fs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
}

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

struct Env {
    std::map<std::string, std::string> set;
    std::function<void(const std::string&, const std::string&)> fn() {
        return [this](const std::string& n, const std::string& v) { set[n] = v; };
    }
};

void testNoMarker() {
    std::printf("P1: no marker\n");
    const fs::path dir = fresh("p1");
    Env env;
    const auto r = applyPortableMode(dir, env.fn());
    CHECK(!r.portable && !r.refused && r.message.empty());
    CHECK(env.set.empty());
    CHECK(fs::is_empty(dir));
}

void testMarker() {
    std::printf("P2: a marker\n");
    {
        const fs::path dir = fresh("p2a");
        writeFile(dir / "foxsdr.portable", "");
        Env env;
        const auto r = applyPortableMode(dir, env.fn());
        CHECK(r.portable && !r.refused);
        CHECK(env.set.size() == 2u);
        CHECK(fs::path(env.set["APPDATA"]).lexically_normal() == (dir / "FoxSDR-data" / "settings").lexically_normal());
        CHECK(fs::path(env.set["LOCALAPPDATA"]).lexically_normal() == (dir / "FoxSDR-data" / "data").lexically_normal());
        CHECK(fs::is_directory(dir / "FoxSDR-data" / "settings"));
        CHECK(fs::is_directory(dir / "FoxSDR-data" / "data"));
        CHECK(fs::is_directory(dir / "FoxSDR-data" / "logs"));
    }
    {
        const fs::path dir = fresh("p2b");
        writeFile(dir / "foxsdr.portable", "MyRadio\n");
        Env env;
        const auto r = applyPortableMode(dir, env.fn());
        CHECK(r.portable);
        CHECK(fs::path(env.set["APPDATA"]).lexically_normal() == (dir / "MyRadio" / "settings").lexically_normal());
        CHECK(!fs::exists(dir / "FoxSDR-data"));
    }
}

void testRefusals() {
    std::printf("P3: a marker that cannot be honoured is a refusal\n");
    for (const char* bad : {"..\\evil", "a/b", "../x", "CON", "C:", "name.", "x\\y"}) {
        const fs::path dir = fresh("p3");
        writeFile(dir / "foxsdr.portable", bad);
        Env env;
        const auto r = applyPortableMode(dir, env.fn());
        if (!(r.refused && !r.portable)) { std::printf("      marker '%s' was not refused\n", bad); }
        CHECK(r.refused && !r.portable);
        CHECK(!r.message.empty());
        CHECK(env.set.empty());
        std::size_t entries = 0;
        for (const auto& e : fs::directory_iterator(dir)) { (void)e; ++entries; }
        CHECK(entries == 1u);   // only the marker: nothing was made, here or outside
    }
    {   // a FILE where the folder belongs
        const fs::path dir = fresh("p3b");
        writeFile(dir / "foxsdr.portable", "");
        writeFile(dir / "FoxSDR-data", "not a folder");
        Env env;
        const auto r = applyPortableMode(dir, env.fn());
        CHECK(r.refused && !r.portable);
        CHECK(env.set.empty());
        CHECK(readFile(dir / "FoxSDR-data") == "not a folder");
    }
    {   // the marker is a folder, not a file: a refusal or no marker, never portable
        const fs::path dir = fresh("p3c");
        fs::create_directories(dir / "foxsdr.portable");
        Env env;
        const auto r = applyPortableMode(dir, env.fn());
        CHECK(!r.portable);
        CHECK(env.set.empty());
    }
}

void testOrderInSource(const fs::path& repo) {
    std::printf("P4: bootProcess calls it between the helper modes and the crash handlers\n");
    const std::string text = readFile(repo / "src" / "engine" / "process_boot.cpp");
    const std::size_t seq = text.find("BootResult bootProcess(");
    CHECK(seq != std::string::npos);
    if (seq == std::string::npos) { return; }
    const std::size_t helpers = text.find("runHelperModes(argc, argv)", seq);
    const std::size_t portable = text.find("applyPortableModeToProcess()", seq);
    const std::size_t arming = text.find("armDiagnostics(kind, argc, argv, boot)", seq);
    CHECK(helpers != std::string::npos && portable != std::string::npos && arming != std::string::npos);
    CHECK(helpers < portable);
    CHECK(portable < arming);
}

#if defined(_WIN32)
// Runs `exe args` with the output and the exit code; the environment of this process is passed on, with the profile folders the test names.
int runProgram(const fs::path& exe, const std::string& args, std::string& output) {
    const fs::path outFile = exe.parent_path() / "run-output.txt";
    std::string cmd = "cmd.exe /c \"\"" + exe.string() + "\" " + args + " > \"" + outFile.string() + "\" 2>&1\"";
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::string mutableCmd = cmd;
    if (!CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, exe.parent_path().string().c_str(), &si, &pi)) { return -1000; }
    const DWORD w = WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 0;
    if (w != WAIT_OBJECT_0) { TerminateProcess(pi.hProcess, 99); code = 99; } else { GetExitCodeProcess(pi.hProcess, &code); }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    output = readFile(outFile);
    return static_cast<int>(code);
}
#endif

void testProgram() {
    std::printf("P5: the program itself\n");
#if defined(_WIN32)
    const fs::path here = cascade::core::sys::currentExeDir();   // <build>/tests/Release
    const fs::path engine = here.parent_path().parent_path() / "Release" / "foxsdr-engine.exe";
    if (!fs::exists(engine)) {
        std::printf("SKIP P5: %s is not built\n", engine.string().c_str());
        ++g_checksSkipped;
        return;
    }
    // the program looks for its own folders in the environment: point the real ones somewhere that must stay EMPTY
    const fs::path decoyRoot = fresh("p5-decoy");
    _putenv_s("APPDATA", (decoyRoot / "roaming").string().c_str());
    _putenv_s("LOCALAPPDATA", (decoyRoot / "local").string().c_str());
    {
        const fs::path dir = fresh("p5a");
        std::error_code ec;
        fs::copy_file(engine, dir / "foxsdr-engine.exe", ec);
        CHECK(!ec);
        writeFile(dir / "foxsdr.portable", "");
        std::string out;
        const int code = runProgram(dir / "foxsdr-engine.exe", "--version", out);
        CHECK(code == 0);
        CHECK(out.find("FoxSDR") != std::string::npos);
        CHECK(fs::is_directory(dir / "FoxSDR-data" / "settings"));
        CHECK(fs::is_directory(dir / "FoxSDR-data" / "data"));
        CHECK(!fs::exists(dir / "foxsdr-portable-error.txt"));
        // the decoy per-user folders were never made by the program
        CHECK(!fs::exists(decoyRoot / "roaming" / "foxsdr"));
        CHECK(!fs::exists(decoyRoot / "local" / "foxsdr"));
    }
    {
        const fs::path dir = fresh("p5b");
        std::error_code ec;
        fs::copy_file(engine, dir / "foxsdr-engine.exe", ec);
        writeFile(dir / "foxsdr.portable", "..\\escape");
        std::string out;
        const int code = runProgram(dir / "foxsdr-engine.exe", "--version", out);
        CHECK(code == 1);
        CHECK(out.find("FoxSDR ") == std::string::npos || out.find("cannot be used") != std::string::npos);
        CHECK(readFile(dir / "foxsdr-portable-error.txt").find("cannot be used") != std::string::npos);
        CHECK(!fs::exists(dir / "FoxSDR-data"));
        CHECK(!fs::exists(decoyRoot / "roaming" / "foxsdr"));
    }
    {   // and with NO marker the same copy is not portable: it uses the (decoy) per-user folders, not a folder beside itself
        const fs::path dir = fresh("p5c");
        std::error_code ec;
        fs::copy_file(engine, dir / "foxsdr-engine.exe", ec);
        std::string out;
        const int code = runProgram(dir / "foxsdr-engine.exe", "--version", out);
        CHECK(code == 0);
        CHECK(!fs::exists(dir / "FoxSDR-data"));
    }
#else
    std::printf("SKIP P5: Windows only\n");
    ++g_checksSkipped;
#endif
}

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_wire_c_portable\n");
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_root = fs::temp_directory_path() / ("foxsdr_wc_portable_" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(g_root, ec);
    fs::create_directories(g_root, ec);
    testNoMarker();
    testMarker();
    testRefusals();
#ifdef WIRE_C_SOURCE_DIR
    testOrderInSource(fs::path(WIRE_C_SOURCE_DIR));
#endif
    testProgram();
    fs::remove_all(g_root, ec);
    return testSummary("test_wire_c_portable");
}
