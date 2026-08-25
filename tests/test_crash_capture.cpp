// The crash handler catches a REAL crash, in a child process, and the report
// it leaves behind is enough to act on.
//
// A crash handler that has never caught a crash is a hypothesis. Everything
// about it - the filter registration, the allocation-free writer, the module
// lookup, the ring flush - is exercised only by an actual fault, and every
// one of those pieces is the kind that works in review and fails at 3am. So
// this test forks a copy of itself, makes it die four different ways, and
// reads what each one left on disk.
//
// Four ways, because they are four separate registrations that fail
// independently: a structured exception (the filter), an exception escaping a
// thread (std::terminate), a virtual call on a dead object (purecall), and
// the CRT's own fail-fast (invalid parameter). A test that only staged an
// access violation would prove one quarter of the feature and imply all of it.
//
// The child re-executes THIS binary with --fault, which keeps the fixture in
// one file and means the faulting process is built from the same code as the
// assertions about it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "core/crash_handler.hpp"
#include "core/diag_log.hpp"
#include "core/diag_report.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace cascade::core;

namespace {

fs::path scratchDir(const std::string& tag) {
    const char* tmp = std::getenv("TEMP");
    const fs::path base = (tmp != nullptr && *tmp != '\0') ? fs::path(tmp) : fs::path(".");
#if defined(_WIN32)
    const unsigned long pid = ::GetCurrentProcessId();
#else
    const unsigned long pid = 0;
#endif
    return base / (std::string("cascade-crash-") + tag + "-" + std::to_string(pid));
}

std::string selfExePath() {
#if defined(_WIN32)
    char buf[MAX_PATH] = {};
    const DWORD n = ::GetModuleFileNameA(nullptr, buf, static_cast<DWORD>(sizeof(buf)));
    return std::string(buf, buf + n);
#else
    return std::string();
#endif
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::vector<fs::path> filesIn(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) { out.push_back(e.path()); }
    return out;
}

// Runs this binary as `--fault <kind> <dir> <enabled>` and returns its exit
// code. A child that is still alive after the deadline is killed and reported
// as such, so a handler that hangs fails the suite instead of stalling it.
unsigned long runFaultChild(int kind, const fs::path& dir, bool enabled, bool& timedOut) {
    timedOut = false;
#if defined(_WIN32)
    std::string cmd = "\"" + selfExePath() + "\" --fault " + std::to_string(kind) + " \"" +
                      dir.string() + "\" " + (enabled ? "1" : "0");
    std::vector<char> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back('\0');

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (::CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                         CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) == 0) {
        return 0xFFFFFFFFul;
    }
    const DWORD wait = ::WaitForSingleObject(pi.hProcess, 30000);
    if (wait != WAIT_OBJECT_0) {
        timedOut = true;
        ::TerminateProcess(pi.hProcess, 1);
        ::WaitForSingleObject(pi.hProcess, 5000);
    }
    DWORD code = 0;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return code;
#else
    (void)kind;
    (void)dir;
    (void)enabled;
    return 0;
#endif
}

// The child. Sets up exactly what a real session would have set up by the
// time it faults - a context block and a few log lines - so the assertions
// about report CONTENT are assertions about the real pipeline, not about a
// synthetic string.
int faultChild(int kind, const std::string& dir, bool enabled) {
#if defined(_WIN32)
    // No Windows Error Reporting dialog: an unattended run must not sit on a
    // modal box waiting for a click.
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    DiagLog::instance().configure(std::string(), false);
    DiagLog::instance().writef("info", "child started, kind %d", kind);
    for (int i = 0; i < 400; ++i) { DiagLog::instance().writef("info", "step %d", i); }
    DiagLog::instance().write("warn", "about to fault on purpose");

    DiagContext ctx;
    ctx.version = "0.61.0-childtest";
    ctx.commit = "cafebabe1234";
    ctx.os = "test";
    ctx.arch = "x64";
    ctx.mode = "WFM";
    ctx.sourceKind = "generator";
    ctx.sampleRateHz = 2400000.0;
    ctx.deviceOpen = true;
    ctx.sdrModel = "uhd b200";
    ctx.plugins.push_back("ADS-B 1.1.0");
    setDiagContext(ctx);
    refreshModuleTable();

    CrashHandlerConfig cfg;
    cfg.crashDir = dir;
    cfg.enabled = enabled;
    cfg.minidump = false;
    cfg.exitAfterReport = true;
    installCrashHandlers(cfg);

    raiseTestFault(static_cast<TestFaultKind>(kind));
    return 7;  // never reached; a 7 exit code means the fault did not happen
}

struct CaughtReport {
    bool wroteExactlyOne = false;
    std::string text;
    unsigned long exitCode = 0;
    bool timedOut = false;
};

CaughtReport catchOne(int kind, const std::string& tag) {
    CaughtReport out;
    const fs::path dir = scratchDir(tag);
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    out.exitCode = runFaultChild(kind, dir, true, out.timedOut);
    const std::vector<fs::path> files = filesIn(dir);
    out.wroteExactlyOne = files.size() == 1u;
    out.text = files.empty() ? std::string() : readFile(files.front());
    return out;
}

// Every property a report must carry for an engineer who was not there. Kept
// as one function so all four fault kinds are held to the same bar - a
// handler that produced a rich report for an access violation and a stub for
// a purecall would otherwise pass.
void checkReportContent(const CaughtReport& r, const char* what) {
    std::printf("--- %s: %zu bytes, exit 0x%08lX ---\n", what, r.text.size(), r.exitCode);
    CHECK(!r.timedOut);
    CHECK(r.wroteExactlyOne);
    CHECK(r.exitCode != 0);   // the child must have died, not returned
    CHECK(r.exitCode != 7u);  // ...and specifically not by falling through
    CHECK(r.text.size() > 400);

    // Identity: version and commit, because "0.61.0" is not one binary.
    CHECK(r.text.find("version: 0.61.0-childtest") != std::string::npos);
    CHECK(r.text.find("commit: cafebabe1234") != std::string::npos);

    // Grouping.
    CHECK(r.text.find("signature: ") != std::string::npos);

    // The stack, as module+offset. Anything that only recorded an absolute
    // address would be unreadable the moment the process exited.
    CHECK(r.text.find("--- stack") != std::string::npos);
    CHECK(r.text.find(".exe+0x") != std::string::npos ||
          r.text.find(".dll+0x") != std::string::npos);

    // The key that finds the symbols. Without this the offsets above are hex
    // forever - this single line is the difference between a diagnosable
    // report and a souvenir.
    CHECK(r.text.find("--- modules ---") != std::string::npos);
    CHECK(r.text.find("build=") != std::string::npos);
    CHECK(r.text.find("pdb=") != std::string::npos);

    // Application context: which plugin was loaded has already been the
    // answer to real faults in this product.
    CHECK(r.text.find("plugin: ADS-B 1.1.0") != std::string::npos);
    CHECK(r.text.find("mode: WFM") != std::string::npos);
    CHECK(r.text.find("sdr-model: uhd b200") != std::string::npos);

    // The ring, flushed WITH the report, and the LAST lines of it: the state
    // leading up to a fault is usually what identifies it.
    // 402 lines were written and the ring holds 256, so "step 300" and the
    // warning must be there and "step 100" and the very first line must not.
    // Both halves matter: a ring that kept the FIRST 256 lines would satisfy
    // a size check and carry none of the run-up to the fault.
    CHECK(r.text.find("about to fault on purpose") != std::string::npos);
    CHECK(r.text.find("step 399") != std::string::npos);
    CHECK(r.text.find("step 300") != std::string::npos);
    CHECK(r.text.find("step 100") == std::string::npos);
    CHECK(r.text.find("child started") == std::string::npos);

    // THE INVENTORY, BOTH DIRECTIONS, and applied to all four fault kinds.
    //
    // Everything above asserts fields are PRESENT. None of it asserts the list
    // is EXHAUSTIVE, and PRIVACY.md claims - in those words - that a report's
    // fields are asserted "in both directions". They were not: the set
    // comparison covered the copy-diagnostics bundle only, so a line added to
    // writeReport() (a command line, a tuned frequency) would have shipped
    // undocumented with every test green. A crash report is written without
    // anyone watching and is the more revealing of the two documents this
    // product produces, so it gets the stricter check.
    //
    // Header fields only: everything after the first "--- " marker is the
    // shared context block, which tests/test_diagnostics.cpp inventories
    // through bundleFieldNames(), or free-form program addresses.
    const std::set<std::string> declared(crashReportFieldNames().begin(),
                                         crashReportFieldNames().end());
    CHECK(!declared.empty());
    std::set<std::string> emitted;
    std::size_t pos = 0;
    while (pos < r.text.size()) {
        const std::size_t eol = r.text.find('\n', pos);
        const std::size_t end = (eol == std::string::npos) ? r.text.size() : eol;
        const std::string line = r.text.substr(pos, end - pos);
        if (line.compare(0, 3, "---") == 0) { break; }
        const std::size_t colon = line.find(": ");
        if (colon != std::string::npos && colon > 0) { emitted.insert(line.substr(0, colon)); }
        pos = end + 1;
    }
    for (const std::string& f : emitted) { std::printf("crash field: %s\n", f.c_str()); }
    CHECK(emitted == declared);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 5 && std::strcmp(argv[1], "--fault") == 0) {
        return faultChild(std::atoi(argv[2]), argv[3], std::atoi(argv[4]) != 0);
    }

    // --- A real access violation, caught, reported --------------------------
    {
        const CaughtReport r = catchOne(0, "av");
        checkReportContent(r, "access violation");
        CHECK(r.text.find("kind: crash") != std::string::npos);
        CHECK(r.text.find("0xC0000005") != std::string::npos);
    }

    // --- An exception escaping a thread (std::terminate) --------------------
    {
        const CaughtReport r = catchOne(1, "terminate");
        checkReportContent(r, "terminate");
        CHECK(r.text.find("terminate") != std::string::npos);
    }

    // --- A pure virtual call ------------------------------------------------
    {
        const CaughtReport r = catchOne(2, "purecall");
        checkReportContent(r, "purecall");
        CHECK(r.text.find("purecall") != std::string::npos);
    }

    // --- The CRT's invalid-parameter fail-fast ------------------------------
    {
        const CaughtReport r = catchOne(3, "invalidparam");
        checkReportContent(r, "invalid parameter");
        CHECK(r.text.find("invalid parameter") != std::string::npos);
    }

    // --- Off means off ------------------------------------------------------
    //
    // The same fault, with capture disabled, must leave NOTHING behind. Not an
    // empty file, not a directory: a feature that is off leaves no trace.
    {
        const fs::path dir = scratchDir("disabled");
        std::error_code ec;
        fs::remove_all(dir, ec);
        bool timedOut = false;
        const unsigned long code = runFaultChild(0, dir, false, timedOut);
        CHECK(!timedOut);
        CHECK(code != 0);
        CHECK(code != 7u);
        CHECK(!fs::exists(dir));
    }

    // Fixtures are left on failure and cleaned on success, so a red run can
    // still be read.
    if (g_checksFailed == 0) {
        std::error_code ec;
        for (const char* tag : {"av", "terminate", "purecall", "invalidparam", "disabled"}) {
            fs::remove_all(scratchDir(tag), ec);
        }
    }
    return testSummary("test_crash_capture");
}
