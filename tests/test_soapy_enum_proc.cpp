// Tests for source/soapy_enum_proc.hpp - enumerating SDR hardware in a child
// process so that a vendor driver faulting mid-probe costs a device list
// rather than the session.
//
// THE FAULT THIS EXISTS FOR cannot be provoked on cue: it is an access
// violation inside libusb, on a thread UHD spawns for itself, about once in
// twenty enumerations with a B200 attached. So the parent's four answers are
// tested against a FAKE HELPER instead - this very executable, re-invoked with
// --enumerate-json and steered by an environment variable. That is not a
// weaker test than the real thing, it is a stronger one: a real child dies
// when it feels like it, while the fake dies, hangs, lies or answers exactly
// when asked, which is the only way "child died" and "child timed out" and
// "child answered no devices" can be shown to be distinguishable at all.
//
// ONE BLOCK DOES USE THE REAL BINARY, at the end, and it is the durability
// property this suite must not lose: the child reports how many times its
// vendor walk went through callGuardingVendorFaults, so deleting the guard
// from the walk - leaving a perfectly good guard that nothing calls - fails
// here. That block is also the answer to "does spawning cascade.exe actually
// work", which no fake can answer. It touches the radio, but through a child:
// if this machine's libusb fault fires, the CHILD dies and the parent retries,
// which is precisely the behaviour under test.
//
// NOTHING HERE INDEXES A RESULT VECTOR. CHECK records and continues, so an
// index guarded only by a preceding size CHECK reads past the end in exactly
// the run that has something to report - and dies with 0xC0000005 instead of
// naming the expectation it broke. This file spawns more than two hundred
// processes per run, so "a transient CreateProcess failure produced an empty
// vector" is not a hypothetical here. Device rows are compared as WHOLE
// CONTAINERS (rowsOf / expectedOkRows) and crash reports as concatenated text
// (allReportText). Measured: with the fake helper returning no devices, the
// container form prints four FAIL lines and finishes its 121 checks, while the
// indexed form segfaults.
//
// THREE PROPERTIES BEYOND THE FOUR OUTCOMES are pinned at the end, each one
// the fix for a defect an adversarial reviewer raised against this design:
//
//   - the child inherits EXACTLY its three standard handles, so two
//     overlapping scans cannot deadlock each other's pipe;
//   - a helper that faults in CASCADE'S OWN code - which vendor_guard
//     deliberately refuses to absorb - still writes a symbolised report, in
//     the directory the parent handed down, and still nothing at all when
//     diagnostics are off;
//   - a fault that was CONTAINED by the retry is filed as a report and not
//     merely logged, because the diagnostics log is never uploaded and the
//     retry succeeding is the common case rather than the rare one.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/soapy_enum_proc.hpp"

#include "core/crash_handler.hpp"

#include <SoapySDR/Device.hpp>
#include <SoapySDR/Registry.hpp>
#include <SoapySDR/Types.hpp>
#include <SoapySDR/Version.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>

#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <tlhelp32.h>
#endif

#include "test_check.hpp"

using cascade::source::EnumOptions;
using cascade::source::EnumOutcome;
using cascade::source::EnumResult;
using cascade::source::enumerateHelperPath;
using cascade::source::enumerateIsolated;
using cascade::source::enumOutcomeName;

namespace {

constexpr const char* kModeVar = "CASCADE_TEST_HELPER_MODE";
constexpr const char* kCounterVar = "CASCADE_TEST_HELPER_COUNTER";
constexpr const char* kHandleVar = "CASCADE_TEST_PROBE_HANDLE";
constexpr const char* kHandleNameVar = "CASCADE_TEST_PROBE_NAME";

constexpr const char* kCrashDirFlag = "--crash-dir=";

// A well-formed answer with THREE rows, one of which is unusable: a row with
// no reopen string cannot be turned back into a device, and the parent is
// required to drop it rather than offer the user a menu entry that fails.
//
// `capture` is what the REAL helper uses to report that it armed crash capture
// into the directory the parent handed down. The fake reports whether it was
// TOLD to - which is the parent-side half of that wiring, and the half a fake
// can honestly answer.
void printOkJson(bool capture) {
    std::printf("{\"schema\":1,\"runtime\":true,\"guardedCalls\":1,\"capture\":%s,\"devices\":["
                "{\"label\":\"fake one\",\"args\":\"driver=fake,serial=1\"},"
                "{\"label\":\"fake two\",\"args\":\"driver=fake,serial=2\"},"
                "{\"label\":\"no args here\",\"args\":\"\"}]}\n",
                capture ? "true" : "false");
}

// The device rows kOkJson's answer must survive as, in order.
using Row = std::pair<std::string, std::string>;
using Rows = std::vector<Row>;

const Rows& expectedOkRows() {
    static const Rows rows{{"fake one", "driver=fake,serial=1"},
                           {"fake two", "driver=fake,serial=2"}};
    return rows;
}

// THE WHOLE CONTAINER, never an index. Indexing a result vector after a
// separate size CHECK is the harness trap this repo has already been bitten by
// (FoxSDR test_config, 2026-08-16): CHECK records and continues, so in the one
// run that has something to report - a transient CreateProcess failure here
// turning into an empty vector - the test reads past the end and dies with
// 0xC0000005 instead of naming the expectation it broke. This file spawns more
// than two hundred processes per run, which is precisely where that transient
// lives.
Rows rowsOf(const EnumResult& r) {
    Rows out;
    for (const auto& d : r.devices) { out.emplace_back(d.label, d.args); }
    return out;
}

#ifdef _WIN32
// Does `h` name the file `wantPath`, in THIS process?
//
// Identity, not validity. A handle VALUE that means one thing in the parent
// can be some unrelated open handle in the child, and "is it valid" would
// answer yes for entirely the wrong reason. Asking the kernel which path the
// handle resolves to can only say yes for the genuinely inherited handle.
// Used by the child as the probe and by the parent as the positive control
// that the probe itself works.
bool handleNamesFile(void* h, const std::string& wantPath) {
    wchar_t buf[1024];
    const DWORD n = ::GetFinalPathNameByHandleW(static_cast<HANDLE>(h), buf, 1023,
                                                FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (n == 0 || n >= 1024) { return false; }
    const std::wstring got = std::filesystem::path(std::wstring(buf, n)).filename().wstring();
    const std::wstring want = std::filesystem::path(wantPath).filename().wstring();
    return !got.empty() && ::_wcsicmp(got.c_str(), want.c_str()) == 0;
}
#endif

// True when this child was handed a non-empty crash directory.
bool wasGivenCrashDir(int argc, char** argv) {
    const std::size_t n = std::strlen(kCrashDirFlag);
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], kCrashDirFlag, n) == 0 && argv[i][n] != '\0') { return true; }
    }
    return false;
}

#ifdef _WIN32
std::wstring selfExePathW() {
    std::wstring buf(1024, L'\0');
    const DWORD n = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) { return std::wstring(); }
    buf.resize(n);
    return buf;
}

// Dies with the exception code as the exit code, in microseconds, with no
// Windows Error Reporting round trip. The parent reads that code back.
LONG WINAPI quietDeath(EXCEPTION_POINTERS* ep) {
    const DWORD code = (ep != nullptr && ep->ExceptionRecord != nullptr)
                           ? ep->ExceptionRecord->ExceptionCode
                           : 0xE0000001ul;
    ::TerminateProcess(::GetCurrentProcess(), code);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

std::string envOr(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return (v != nullptr && *v != '\0') ? std::string(v) : std::string(fallback);
}

// --- the fake helper -------------------------------------------------------
int fakeHelper(int argc, char** argv) {
    const std::string mode = envOr(kModeVar, "empty");
    const bool gotCrashDir = wasGivenCrashDir(argc, argv);
    if (mode == "ok") {
        printOkJson(gotCrashDir);
        return 0;
    }
    if (mode == "empty") {
        std::printf("{\"schema\":1,\"runtime\":false,\"guardedCalls\":0,\"capture\":%s,"
                    "\"devices\":[]}\n",
                    gotCrashDir ? "true" : "false");
        return 0;
    }
    if (mode == "handleprobe") {
        // DOES THIS CHILD HOLD A HANDLE IT WAS NEVER MEANT TO GET?
        //
        // The parent opens a uniquely-named temp file with an INHERITABLE
        // handle and passes the handle's numeric value and the file's name in
        // the environment. Identity is checked by asking the kernel what path
        // that handle resolves to, not merely whether it is valid: a handle
        // value can coincide with some unrelated handle of this process, and
        // "valid" would then answer yes for the wrong reason. Only the
        // genuinely inherited handle names that file.
        //
        // 21 = inherited (the bug), 20 = not inherited (the fix), 29 = the
        // probe was not set up, which is a broken test rather than a pass.
#ifdef _WIN32
        const std::string hv = envOr(kHandleVar, "");
        const std::string want = envOr(kHandleNameVar, "");
        if (hv.empty() || want.empty()) { return 29; }
        HANDLE h = reinterpret_cast<HANDLE>(
            static_cast<std::uintptr_t>(std::strtoull(hv.c_str(), nullptr, 10)));
        return handleNamesFile(h, want) ? 21 : 20;
#else
        return 20;
#endif
    }
    if (mode == "garbage") {
        std::printf("this is not json at all\n");
        return 0;
    }
    if (mode == "skew") {
        // A helper from a different install: exits cleanly, answers in a
        // protocol this parent does not speak.
        std::printf("{\"schema\":999,\"devices\":[]}\n");
        return 0;
    }
    if (mode == "die") { return 7; }
    if (mode == "av") {
        // A GENUINE ACCESS VIOLATION, which is the whole point: "exit 7" only
        // proves the parent survives a child that returns a bad number. The
        // fault this design exists to contain is 0xC0000005 inside libusb, and
        // the only way to show that a fault - not a return code - stays inside
        // the child is to raise one.
        //
        // Error reporting is switched off first, exactly as the real helper
        // does it: a child that is expected to die must die in milliseconds
        // and leave no Windows Error Reporting dialog or dump behind.
#ifdef _WIN32
        ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
                       SEM_NOOPENFILEERRORBOX);
        ::SetUnhandledExceptionFilter(&quietDeath);
        ::RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, nullptr);
#endif
        return 0;  // unreachable on Windows
    }
    if (mode == "flaky") {
        // Dies the first time it is asked and answers the second, which is the
        // shape of the real fault and the reason the parent retries at all.
        const std::string counter = envOr(kCounterVar, "");
        std::error_code ec;
        if (!counter.empty() && !std::filesystem::exists(std::filesystem::path(counter), ec)) {
            std::ofstream(counter, std::ios::binary) << "1";
            return 7;
        }
        printOkJson(gotCrashDir);
        return 0;
    }
    if (mode == "hang") {
#ifdef _WIN32
        ::Sleep(60000);
#endif
        return 0;
    }
    return 0;
}

#ifdef _WIN32
// How many copies of THIS executable are running, counted by image name.
//
// Crude on purpose: the middle process and the child below are both this
// binary, so one number covers both, and ctest runs its tests serially so
// nothing else is making copies while this runs.
int selfProcessCount() {
    const std::wstring name = std::filesystem::path(selfExePathW()).filename().wstring();
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) { return -1; }
    int n = 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (::Process32FirstW(snap, &pe) != 0) {
        do {
            if (name == pe.szExeFile) { ++n; }
        } while (::Process32NextW(snap, &pe) != 0);
    }
    ::CloseHandle(snap);
    return n;
}

// Starts this executable with one argument and returns its handles.
bool spawnSelf(const wchar_t* arg, PROCESS_INFORMATION& pi) {
    std::wstring cmd = L"\"" + selfExePathW() + L"\" " + arg;
    cmd.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    return ::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                            nullptr, nullptr, &si, &pi) != 0;
}
#endif

unsigned long currentPid() {
#ifdef _WIN32
    return static_cast<unsigned long>(::GetCurrentProcessId());
#else
    return 0ul;
#endif
}

#ifdef _WIN32
std::string selfExePath() { return std::filesystem::path(selfExePathW()).string(); }
#else
std::string selfExePath() { return std::string(); }
#endif

void setMode(const char* mode) {
#ifdef _WIN32
    ::_putenv_s(kModeVar, mode);
#else
    ::setenv(kModeVar, mode, 1);
#endif
}

void setEnvVar(const char* name, const char* value) {
#ifdef _WIN32
    ::_putenv_s(name, value);
#else
    if (*value == '\0') {
        ::unsetenv(name);
    } else {
        ::setenv(name, value, 1);
    }
#endif
}

// The real application binary, which is not beside the test binaries: the
// tests build into build/tests/Release and cascade.exe into build/Release.
// Both candidates are tried and the first that exists wins; an empty answer is
// asserted against by the caller, so a moved build tree fails loudly instead
// of quietly skipping the one block that uses real hardware.
std::string findRealCascade() {
    const std::filesystem::path self(selfExePath());
    if (self.empty()) { return std::string(); }
    const std::filesystem::path dir = self.parent_path();
    const std::filesystem::path candidates[] = {
        dir / "cascade.exe",
        dir.parent_path().parent_path() / "Release" / "cascade.exe",
    };
    std::error_code ec;
    for (const auto& c : candidates) {
        if (std::filesystem::exists(c, ec)) { return c.string(); }
    }
    return std::string();
}

#ifdef _WIN32
// Starts this executable with `args`, waits, and returns its exit code.
// 0xFFFFFFFF means it could not be started at all, which is asserted against
// rather than silently read as a result.
unsigned long runSelfAndWait(const std::wstring& args) {
    std::wstring cmd = L"\"" + selfExePathW() + L"\" " + args;
    cmd.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                         nullptr, nullptr, &si, &pi) == 0) {
        return 0xFFFFFFFFul;
    }
    ::WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 0;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return static_cast<unsigned long>(code);
}
#endif

std::string readAll(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

#ifdef _WIN32
// Redirects fd 1 (stdout) to `path` for the duration of `fn`, restores it
// unconditionally - even when `fn` throws - and returns what landed in the
// file.
//
// This is the dup2-to-a-temp-file fallback the lane brief asked for: nothing
// in this file already captures an IN-PROCESS stdout write. Every other block
// that reads a child's answer does it through the CreatePipe/drain machinery
// inside soapy_enum_proc.cpp itself, which only exists for a SPAWNED child -
// runEnumerateHelper() called directly, in THIS process (see the invalid-UTF-8
// block below for why that is the real seam and not a workaround), writes
// straight to this process's own stdout with fwrite/fflush, so something has
// to intercept fd 1 before that reaches the console or gets lost.
template <typename Fn>
std::string captureStdout(const std::filesystem::path& path, Fn&& fn) {
    std::fflush(stdout);
    const int fd = ::_fileno(stdout);
    const int savedFd = ::_dup(fd);
    const int fileFd = ::_open(path.string().c_str(),
                               _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY,
                               _S_IREAD | _S_IWRITE);
    if (fileFd != -1) {
        ::_dup2(fileFd, fd);
        ::_close(fileFd);
    }
    // The exception is rethrown only AFTER fd 1 is restored, so a throwing
    // `fn` (the exact failure mode 6477BA87 was) cannot leave every later
    // std::printf in this suite writing into a temp file nobody reads.
    std::exception_ptr pending;
    try {
        fn();
    } catch (...) {
        pending = std::current_exception();
    }
    std::fflush(stdout);
    if (savedFd != -1) {
        ::_dup2(savedFd, fd);
        ::_close(savedFd);
    }
    if (pending) { std::rethrow_exception(pending); }
    return readAll(path);
}

#endif  // _WIN32

// Every crash-*.txt in `dir`, oldest first by name (the writer's sequence
// number is in the name, so lexical order is write order).
std::vector<std::filesystem::path> crashReports(const std::filesystem::path& dir) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        const std::string name = e.path().filename().string();
        if (name.rfind("crash-", 0) == 0 && e.path().extension() == ".txt") {
            out.push_back(e.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// The concatenated text of every crash report in `dir`. Asserting against the
// whole set rather than picking an index keeps this out of the same
// out-of-bounds trap the device rows above are written to avoid.
std::string allReportText(const std::filesystem::path& dir) {
    std::string all;
    for (const auto& p : crashReports(dir)) { all += readAll(p); }
    return all;
}

bool rowsWellFormed(const EnumResult& r) {
    for (const auto& d : r.devices) {
        if (d.label.empty() || d.args.empty()) { return false; }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    // THE FAKE-HELPER ENTRY POINT. This executable is its own child: the
    // parent spawns it with exactly the argument the real helper takes, so the
    // command line, the pipe, the inheritance and the wait are all the
    // production ones and only the answer is synthetic.
    // argc is NOT pinned to 2: the parent appends the crash directory when its
    // own capture is armed, and a helper that stopped recognising itself the
    // moment that argument appeared would fall through into this test body and
    // spawn itself for ever.
    if (argc >= 2 && std::strcmp(argv[1], "--enumerate-json") == 0) {
        return fakeHelper(argc, argv);
    }

    // A HELPER PROCESS THAT FAULTS IN CASCADE'S OWN CODE, which is the case
    // vendor_guard.hpp rule 1 deliberately refuses to absorb - so it is the
    // case that has to reach a crash handler. Since the walk moved out of
    // process there is no application handler in this process to reach, which
    // is exactly the defect being fixed: armEnumerateHelperProcess() installs
    // one when the parent handed down a directory, and nothing when it did
    // not. Both branches are spawned and checked below.
    if (argc >= 2 && std::strcmp(argv[1], "--armed-fault") == 0) {
        cascade::source::armEnumerateHelperProcess(argc >= 3 ? argv[2] : "");
        cascade::core::raiseTestFault(cascade::core::TestFaultKind::AccessViolation);
        return 0;  // unreachable: the fault above is fatal either way
    }

    // THE MIDDLE PROCESS for the orphan test below: it starts a child that will
    // never finish and then blocks, so that killing it leaves that child with
    // no parent. Its exit code is never read - it is killed, that is the point.
    if (argc == 2 && std::strcmp(argv[1], "--orphan-parent") == 0) {
        EnumOptions o;
        o.helperPath = selfExePath();
        o.allowInProcessFallback = false;
        o.attempts = 1;
        o.timeoutMs = 120000;  // long enough that the kill lands first
        (void)enumerateIsolated(o);
        return 0;
    }

    const std::string self = selfExePath();
    CHECK(!self.empty());

    // --- the outcomes are distinguishable, and say so ----------------------
    {
        const EnumOutcome all[] = {EnumOutcome::Ok, EnumOutcome::ChildDied,
                                   EnumOutcome::ChildTimedOut, EnumOutcome::SpawnFailed,
                                   EnumOutcome::Malformed};
        for (const EnumOutcome a : all) {
            CHECK(enumOutcomeName(a) != nullptr);
            CHECK(std::strlen(enumOutcomeName(a)) > 0);
        }
        // Distinct names, both ways round: two outcomes that log identically
        // are two outcomes nobody can tell apart in a support conversation,
        // which is the failure this whole enum exists to prevent.
        for (const EnumOutcome a : all) {
            for (const EnumOutcome b : all) {
                const bool same = std::strcmp(enumOutcomeName(a), enumOutcomeName(b)) == 0;
                CHECK(same == (a == b));
            }
        }
    }

    // --- helper resolution --------------------------------------------------
    {
        setEnvVar("CASCADE_ENUM_HELPER", "X:\\nowhere\\override-helper.exe");
        CHECK(enumerateHelperPath() == "X:\\nowhere\\override-helper.exe");

        setEnvVar("CASCADE_ENUM_HELPER", "");
        const std::string resolved = enumerateHelperPath();
        CHECK(!resolved.empty());
        // Beside the running executable, named for the application - not for
        // whatever this test binary happens to be called.
        CHECK(std::filesystem::path(resolved).filename().string() == "cascade.exe");
        CHECK(std::filesystem::path(resolved).parent_path() ==
              std::filesystem::path(self).parent_path());
    }

    // --- SpawnFailed: no helper, and the fallback held off ------------------
    {
        EnumOptions o;
        o.helperPath = "X:\\nowhere\\definitely-not-here.exe";
        o.allowInProcessFallback = false;
        const EnumResult r = enumerateIsolated(o);
        CHECK(r.outcome == EnumOutcome::SpawnFailed);
        CHECK(r.devices.empty());
        CHECK(r.attempts == 0);  // nothing was ever started
        CHECK(!r.fellBackInProcess);
    }

    // --- Ok: the child answered, and the answer was believed ----------------
    {
        setMode("ok");
        EnumOptions o;
        o.helperPath = self;
        o.allowInProcessFallback = false;
        const EnumResult r = enumerateIsolated(o);
        CHECK(r.outcome == EnumOutcome::Ok);
        CHECK(r.exitCode == 0);
        CHECK(r.attempts == 1);
        CHECK(r.childRuntimeAvailable);
        CHECK(r.guardedCalls == 1);
        // A clean run reports no deaths, so the counter below means something
        // when it is not zero.
        CHECK(r.childDeaths == 0);
        CHECK(r.deathExitCode == 0u);
        // THREE rows offered, TWO kept, in order, with their strings intact:
        // the row with no reopen string is dropped, because a menu entry that
        // cannot be opened is worse than no entry at all. Compared as a WHOLE
        // CONTAINER - see rowsOf() for why this must never be written as a
        // size check followed by indexing.
        CHECK(rowsOf(r) == expectedOkRows());
        CHECK(rowsWellFormed(r));
        // Capture off in this process, so the parent passes no crash
        // directory and the child says so.
        CHECK(!r.childCaptureArmed);
    }

    // --- INVALID UTF-8 IN A VENDOR LABEL DOES NOT KILL THE CHILD ------------
    //
    // Field crash 6477BA87: the child serialises vendor device labels/args
    // with j.dump(), and dump() VALIDATES UTF-8 by default. A vendor find()
    // function that hands back one byte that is not valid UTF-8 - which is
    // third-party text, entirely outside this program's control - made dump()
    // throw nlohmann::json::type_error.316, uncaught, and the child died with
    // 0xE06D7363 (a C++ exception surfacing as a Windows exit code). The
    // parent then misfiled that exit as "the known libusb fault, contained":
    // an ordinary encoding bug in our own serialiser, reported as somebody
    // else's memory corruption. The fix is error_handler_t::replace at the
    // dump() call in soapy_enum_proc.cpp's runEnumerateHelper().
    //
    // THIS DRIVES enumerationReportJson() - the child's real serialisation
    // step, extracted as a named seam for exactly this test (see its
    // declaration). The first version of this block called
    // runEnumerateHelper() in-process instead, reasoning that it was "the
    // narrowest reach that is still the genuine production function". It was
    // also, measured on this bench, a ~5%-per-run suite killer: the helper's
    // first act is arming the child's quiet-death exception filter, and its
    // second is a REAL vendor enumeration - the full USB walk, radioconda's
    // modules and all - so the B200's known discovery fault terminated the
    // whole test binary with no named failure about one run in twenty. The
    // seam keeps the assertion honest (it is the code the child runs, not a
    // copy) without either side effect.
    {
        std::vector<cascade::source::SoapyDeviceInfo> devices(2);
        // 0xC3 opens a two-byte UTF-8 sequence; 0x28 ('(') is not a valid
        // continuation byte (those run 0x80-0xBF) - malformed exactly the way
        // the field label was. The args string carries a lone 0xFF, invalid
        // anywhere in UTF-8, so BOTH serialised fields are exercised.
        devices[0].label = std::string("bad \xC3\x28 label");
        devices[0].args = std::string("driver=fake,serial=\xFF");
        devices[1].label = "clean device";
        devices[1].args = "driver=clean";

        // 1. A caught exception here IS the bug this test exists to catch:
        // this is exactly the type_error.316 that used to reach nobody's
        // catch block and kill the child outright.
        int threw = 0;
        std::string line;
        try {
            line = cascade::source::enumerationReportJson(true, 1, false, devices);
        } catch (const std::exception& e) {
            std::printf("bad-utf8 serialisation threw: %s\n", e.what());
            ++threw;
        } catch (...) {
            std::printf("bad-utf8 serialisation threw a non-std exception\n");
            ++threw;
        }
        CHECK(threw == 0);
        CHECK(!line.empty());

        // 2. The emitted JSON parses back - REPLACE means the byte sequence
        // was rewritten into something valid, not merely that nothing crashed.
        const nlohmann::json parsed = nlohmann::json::parse(line, nullptr, false);
        CHECK(!parsed.is_discarded());
        CHECK(parsed.is_object());

        // 3. The damage is a replacement character, not a dropped device or a
        // truncated label - and the CLEAN device is untouched, proving replace
        // is a per-byte repair rather than a blanket rewrite.
        bool sawReplacementLabel = false;
        bool sawReplacementArgs = false;
        bool cleanIntact = false;
        if (parsed.contains("devices") && parsed["devices"].is_array()) {
            for (const auto& d : parsed["devices"]) {
                if (!d.is_object() || !d.contains("label")) { continue; }
                const std::string label = d["label"].get<std::string>();
                const std::string args =
                    d.contains("args") ? d["args"].get<std::string>() : std::string();
                if (label.rfind("bad ", 0) == 0 &&
                    label.find("\xEF\xBF\xBD") != std::string::npos &&
                    label.find("label") != std::string::npos) {
                    sawReplacementLabel = true;
                    if (args.find("\xEF\xBF\xBD") != std::string::npos) {
                        sawReplacementArgs = true;
                    }
                }
                if (label == "clean device" && args == "driver=clean") {
                    cleanIntact = true;
                }
            }
        }
        std::printf("bad-utf8 label repaired: %s, args repaired: %s, clean intact: %s\n",
                    sawReplacementLabel ? "true" : "false",
                    sawReplacementArgs ? "true" : "false",
                    cleanIntact ? "true" : "false");
        CHECK(sawReplacementLabel);
        CHECK(sawReplacementArgs);
        CHECK(cleanIntact);
    }

    // --- "no devices" is an ANSWER, not a failure ---------------------------
    //
    // The whole point of keeping the outcomes apart: a machine with no vendor
    // modules must report Ok with an empty list, and must not be confusable
    // with a probe that died.
    {
        setMode("empty");
        EnumOptions o;
        o.helperPath = self;
        o.allowInProcessFallback = false;
        const EnumResult r = enumerateIsolated(o);
        CHECK(r.outcome == EnumOutcome::Ok);
        CHECK(r.devices.empty());
        CHECK(!r.childRuntimeAvailable);
        CHECK(r.guardedCalls == 0);
    }

    // --- Malformed: exited cleanly, said nothing usable ---------------------
    {
        setMode("garbage");
        EnumOptions o;
        o.helperPath = self;
        o.allowInProcessFallback = false;
        const EnumResult r = enumerateIsolated(o);
        CHECK(r.outcome == EnumOutcome::Malformed);
        CHECK(r.devices.empty());
        CHECK(r.exitCode == 0);  // it did NOT die - that is the distinction
    }
    {
        // A helper from another install: valid JSON, wrong protocol version.
        setMode("skew");
        EnumOptions o;
        o.helperPath = self;
        o.allowInProcessFallback = false;
        const EnumResult r = enumerateIsolated(o);
        CHECK(r.outcome == EnumOutcome::Malformed);
        CHECK(r.devices.empty());
    }

    // --- ChildDied: the fault this file exists for, contained ---------------
    //
    // Reaching the line after enumerateIsolated is itself the assertion. The
    // old design had no line after it: the fault landed in the application's
    // own address space and the session was over.
    {
        setMode("die");
        EnumOptions o;
        o.helperPath = self;
        o.allowInProcessFallback = false;
        o.attempts = 1;
        const EnumResult r = enumerateIsolated(o);
        CHECK(r.outcome == EnumOutcome::ChildDied);
        CHECK(r.exitCode == 7u);
        CHECK(r.devices.empty());
        CHECK(r.attempts == 1);
    }
    {
        // ...and with the default retry, a helper that always dies is asked
        // exactly twice before the parent gives up and says so.
        setMode("die");
        EnumOptions o;
        o.helperPath = self;
        o.allowInProcessFallback = false;
        const EnumResult r = enumerateIsolated(o);
        CHECK(r.outcome == EnumOutcome::ChildDied);
        CHECK(r.attempts == 2);
    }

    // --- A CHILD ACCESS VIOLATION, TWO HUNDRED TIMES ------------------------
    //
    // THE HEADLINE PROPERTY OF THIS WHOLE CHANGE, measured rather than argued.
    //
    // "exit 7" above proves the parent survives a child that returns a bad
    // number. It does not prove the parent survives a child that FAULTS, and a
    // fault is what this exists for: 0xC0000005 inside libusb, on a thread UHD
    // spawned, which killed the application outright every time it fired -
    // about one enumeration in twenty on this bench, and 2 runs in 40 of the
    // test binary that carried the in-process guard.
    //
    // So: two hundred children, each raising a real access violation, each
    // reported as ChildDied with the exception code intact, and the parent
    // still standing at the end and still able to get a good answer. Two
    // hundred faults, zero parent deaths. Reaching testSummary at all is the
    // assertion; the counters below are so a partial failure names itself.
    //
    // Retries off (attempts = 1) so the count of faults is exactly the count of
    // children, and cheap enough to keep for ever: a child that faults on entry
    // costs a process create and no bus walk at all.
    {
        setMode("av");
        EnumOptions o;
        o.helperPath = self;
        o.allowInProcessFallback = false;
        o.attempts = 1;

        constexpr int kFaults = 200;
        int died = 0;
        int rightCode = 0;
        int leakedDevices = 0;
        for (int i = 0; i < kFaults; ++i) {
            const EnumResult r = enumerateIsolated(o);
            if (r.outcome == EnumOutcome::ChildDied) { ++died; }
            if (r.exitCode == 0xC0000005ul) { ++rightCode; }
            if (!r.devices.empty()) { ++leakedDevices; }
        }
        std::printf("injected child faults: %d/%d reported as child-died, %d/%d with "
                    "0xC0000005\n",
                    died, kFaults, rightCode, kFaults);
        CHECK(died == kFaults);
        CHECK(rightCode == kFaults);
        CHECK(leakedDevices == 0);

        // AND STILL WORKING. A parent that survives 200 faults but is left
        // unable to enumerate afterwards has leaked a handle, a thread or a
        // pipe on every one of them, and would fail on the 201st in the field
        // rather than here.
        setMode("ok");
        const EnumResult after = enumerateIsolated(o);
        CHECK(after.outcome == EnumOutcome::Ok);
        CHECK(rowsOf(after) == expectedOkRows());
    }

    // --- the retry is what turns a 1-in-20 fault into a 1-in-400 one --------
    {
        std::error_code ec;
        const std::filesystem::path counter =
            std::filesystem::temp_directory_path(ec) /
            ("enum_flaky_" + std::to_string(currentPid()) + ".txt");
        std::filesystem::remove(counter, ec);
        setMode("flaky");
        setEnvVar(kCounterVar, counter.string().c_str());

        EnumOptions o;
        o.helperPath = self;
        o.allowInProcessFallback = false;
        const EnumResult r = enumerateIsolated(o);
        CHECK(r.outcome == EnumOutcome::Ok);
        CHECK(r.attempts == 2);  // the first child died and was replaced
        CHECK(rowsOf(r) == expectedOkRows());
        // THE DEATH SURVIVES THE RECOVERY. Without these two, a successful
        // retry erases its own evidence - outcome Ok, exitCode 0 from the
        // child that worked - and a machine quietly losing one scan in twenty
        // to a driver fault looks identical to one that never faults.
        CHECK(r.childDeaths == 1);
        CHECK(r.deathExitCode == 7u);
        std::filesystem::remove(counter, ec);
        setEnvVar(kCounterVar, "");
    }

    // --- ChildTimedOut: a wedged probe is bounded, and killed ---------------
    {
        setMode("hang");
        EnumOptions o;
        o.helperPath = self;
        o.allowInProcessFallback = false;
        o.timeoutMs = 1500;
        const EnumResult r = enumerateIsolated(o);
        CHECK(r.outcome == EnumOutcome::ChildTimedOut);
        CHECK(r.devices.empty());
        // It waited for the budget...
        CHECK(r.elapsedMs >= 1500u);
        // ...and then acted, rather than waiting out the helper's own 60 s.
        CHECK(r.elapsedMs < 20000u);
        // Killed by us, with our marker, rather than having exited on its own.
        CHECK(r.exitCode == 0xE0454E55ul);
        // NOT retried: a timeout has already cost the full budget.
        CHECK(r.attempts == 1);
    }

    // --- A WEDGED CHILD DOES NOT OUTLIVE ITS PARENT -------------------------
    //
    // The timeout above only works while somebody is waiting on it, and nobody
    // is if the application quits mid-scan: AppWindow's quit path waits 250 ms
    // for an in-flight scan and then detaches it. A child wedged inside a USB
    // probe would then be orphaned with no one left to kill it, sitting on a
    // bus whose device is documented to wedge - and invisible, because nothing
    // on screen would ever mention it again.
    //
    // Three processes here: this one, a middle one started with --orphan-parent
    // that spawns a child which sleeps for a minute, and that child. Killing
    // the middle one must take the child with it, which is what the job object
    // in runOneChild buys. All three are this same executable, so one count by
    // image name covers the lot.
    // Win32-only from here to the armed-fault block: job objects, handle
    // inheritance and TerminateProcess have no Linux counterpart in this
    // design (the POSIX child is process-group + SIGKILL, covered above), and
    // the helpers these blocks use are themselves defined under _WIN32.
#ifdef _WIN32
    {
        setMode("hang");
        const int before = selfProcessCount();
        CHECK(before >= 1);  // at minimum, this process

        PROCESS_INFORMATION mid{};
        CHECK(spawnSelf(L"--orphan-parent", mid));

        // Wait for BOTH descendants to exist before killing anything: killing
        // the middle process before it has spawned its child would make this
        // pass for the wrong reason.
        int peak = before;
        for (int i = 0; i < 100 && peak < before + 2; ++i) {
            ::Sleep(50);
            peak = selfProcessCount();
        }
        CHECK(peak >= before + 2);

        ::TerminateProcess(mid.hProcess, 1);
        ::WaitForSingleObject(mid.hProcess, 5000);
        ::CloseHandle(mid.hThread);
        ::CloseHandle(mid.hProcess);

        // The child sleeps for 60 s; if the job did not kill it, this count
        // never comes back down inside the window below.
        int now = selfProcessCount();
        for (int i = 0; i < 100 && now > before; ++i) {
            ::Sleep(50);
            now = selfProcessCount();
        }
        std::printf("orphan test: %d processes before, %d at peak, %d after the kill\n",
                    before, peak, now);
        CHECK(now == before);
    }
#endif  // _WIN32

    // --- THE CHILD GETS THREE HANDLES, AND NOTHING ELSE ---------------------
    //
    // CreateProcess with bInheritHandles TRUE and no handle list hands the
    // child EVERY inheritable handle the parent holds. The one that matters is
    // another enumeration's pipe write end: a child holding it keeps that pipe
    // open past its own exit, the other scan's drain thread never sees
    // end-of-file, and a good answer becomes a 20 s timeout and a kill - the
    // exact failure the drain-on-its-own-thread design exists to prevent.
    //
    // Rather than race two scans and hope, this hands the child a handle it
    // must not have and asks whether it has it. The parent's own check on the
    // same handle is the positive control: without it, a probe that could
    // never say "inherited" would pass this test by being broken.
#ifdef _WIN32
    {
        std::error_code ec;
        const std::filesystem::path probeFile =
            std::filesystem::temp_directory_path(ec) /
            ("enum_handle_probe_" + std::to_string(currentPid()) + ".txt");
        std::filesystem::remove(probeFile, ec);

        SECURITY_ATTRIBUTES psa{};
        psa.nLength = sizeof(psa);
        psa.bInheritHandle = TRUE;  // deliberately inheritable - that is the point
        HANDLE probe = ::CreateFileW(probeFile.wstring().c_str(), GENERIC_WRITE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, &psa, CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(probe != INVALID_HANDLE_VALUE);
        // POSITIVE CONTROL: in this process the handle does name that file, so
        // a "no" from the child is a real answer and not a broken probe.
        CHECK(handleNamesFile(probe, probeFile.string()));

        setEnvVar(kHandleVar,
                  std::to_string(reinterpret_cast<std::uintptr_t>(probe)).c_str());
        setEnvVar(kHandleNameVar, probeFile.string().c_str());
        setMode("handleprobe");

        EnumOptions o;
        o.helperPath = self;
        o.allowInProcessFallback = false;
        o.attempts = 1;
        const EnumResult r = enumerateIsolated(o);
        std::printf("handle scoping: child exit %lu (20 = not inherited, 21 = inherited, "
                    "29 = probe not set up)\n",
                    r.exitCode);
        // 21 means the child inherited a handle that is none of its business.
        // 29 means the environment never reached it, which is a broken test
        // and must fail as loudly as the bug would.
        CHECK(r.exitCode == 20ul);

        if (probe != INVALID_HANDLE_VALUE) { ::CloseHandle(probe); }
        setEnvVar(kHandleVar, "");
        setEnvVar(kHandleNameVar, "");
        std::filesystem::remove(probeFile, ec);
    }
#endif  // _WIN32

    // --- A HELPER THAT FAULTS IN CASCADE'S OWN CODE STILL FILES A REPORT ----
    //
    // vendor_guard.hpp rule 1 refuses to absorb a fault in our own image
    // precisely so it reaches a crash handler. Moving the walk into a child
    // process removed the handler it was supposed to reach: main() returns
    // into the helper above installCrashHandlers, so the fault became an exit
    // code and nothing else - 0.62.0's symbolised stack, gone, on the most
    // crash-prone path in the product.
    //
    // Both branches, spawned for real and read off disk:
    //   given a directory  the ordinary crash-<stamp>.txt, with the FAULT's
    //                      own address, and still the exception code as the
    //                      exit code so the parent's classification is
    //                      unchanged.
    //   given nothing      the fast quiet death, and NO report - because
    //                      diagnostics off means off in the child too.
#ifdef _WIN32
    {
        std::error_code ec;
        const std::filesystem::path dir =
            std::filesystem::temp_directory_path(ec) /
            ("enum_child_reports_" + std::to_string(currentPid()));
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        CHECK(std::filesystem::is_directory(dir));

        const unsigned long armed =
            runSelfAndWait(L"--armed-fault \"" + std::filesystem::path(dir).wstring() + L"\"");
        const std::string body = allReportText(dir);
        std::printf("armed helper fault: exit 0x%08lX, %zu report(s), %zu bytes\n", armed,
                    crashReports(dir).size(), body.size());
        // The parent's own classification must not change: it still reads the
        // exception code back as the exit code.
        CHECK(armed == 0xC0000005ul);
        CHECK(crashReports(dir).size() == 1);
        CHECK(body.find("kind: crash") != std::string::npos);
        CHECK(body.find("reason: access violation") != std::string::npos);
        CHECK(body.find("code: 0xC0000005") != std::string::npos);
        // A REAL FAULT ADDRESS. The parent-side ChildDied report can only ever
        // say 0x0000000000000000 here and carry the parent's stack; this is
        // the half that was lost and is the reason the child installs its own.
        CHECK(body.find("address: 0x0000000000000000") == std::string::npos);
        CHECK(body.find("--- stack (thread") != std::string::npos);

        // ...and with no directory: same death, same exit code, nothing on
        // disk. Written into the SAME directory so "nothing new appeared" is
        // an observation and not an absence of anywhere to look.
        const unsigned long quiet = runSelfAndWait(L"--armed-fault");
        CHECK(quiet == 0xC0000005ul);
        CHECK(crashReports(dir).size() == 1);  // still just the one from above

        std::filesystem::remove_all(dir, ec);
    }
#endif  // _WIN32

    // --- CAPTURE IS HANDED DOWN, AND A CONTAINED FAULT IS VISIBLE -----------
    //
    // From here on this process has crash capture armed into a scratch
    // directory, which is what makes the two properties below observable:
    // the child is told where to write, and a fault that was CONTAINED still
    // produces a report file rather than only a diagnostics line. The
    // diagnostics log is never uploaded on its own (core/crash_upload.cpp
    // forwards report files and nothing else), so "a line in the log" is the
    // same as invisible to anyone not sitting at the machine.
    {
        std::error_code ec;
        const std::filesystem::path dir =
            std::filesystem::temp_directory_path(ec) /
            ("enum_parent_reports_" + std::to_string(currentPid()));
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        CHECK(std::filesystem::is_directory(dir));

        cascade::core::CrashHandlerConfig cfg;
        cfg.crashDir = dir.string();
        cfg.enabled = true;
        cascade::core::installCrashHandlers(cfg);
        cascade::core::setCrashCaptureEnabled(true, false);
        CHECK(cascade::core::activeCrashDir() == dir.string());

        // THE CHILD IS TOLD. Without the command-line argument the child runs
        // with no handler at all and this is false.
        {
            setMode("ok");
            EnumOptions o;
            o.helperPath = self;
            o.allowInProcessFallback = false;
            const EnumResult r = enumerateIsolated(o);
            CHECK(r.outcome == EnumOutcome::Ok);
            CHECK(r.childCaptureArmed);
            CHECK(rowsOf(r) == expectedOkRows());
            // A healthy scan files nothing: the reports below mean something
            // only because this one produced none.
            CHECK(crashReports(dir).empty());
        }

        // A DEATH THAT WAS RECOVERED FROM IS STILL FILED. This is the common
        // shape of the real fault by a wide margin - roughly forty of every
        // forty-one occurrences end as "first child died, retry worked" - and
        // reporting only when EVERY attempt died left those forty invisible.
        {
            const std::filesystem::path counter =
                std::filesystem::temp_directory_path(ec) /
                ("enum_flaky_report_" + std::to_string(currentPid()) + ".txt");
            std::filesystem::remove(counter, ec);
            setMode("flaky");
            setEnvVar(kCounterVar, counter.string().c_str());

            EnumOptions o;
            o.helperPath = self;
            o.allowInProcessFallback = false;
            const EnumResult r = enumerateIsolated(o);
            // The enumeration SUCCEEDED - that is the whole point. The user
            // got their device list; the fault must not vanish with it.
            CHECK(r.outcome == EnumOutcome::Ok);
            CHECK(r.childDeaths == 1);
            const std::string body = allReportText(dir);
            std::printf("contained fault: %zu report(s) after a recovered death\n",
                        crashReports(dir).size());
            CHECK(crashReports(dir).size() == 1);
            CHECK(body.find("enumeration child process died") != std::string::npos);
            CHECK(body.find("code: 0x00000007") != std::string::npos);

            std::filesystem::remove(counter, ec);
            setEnvVar(kCounterVar, "");
        }

        // --- THE DURABILITY PROPERTY, against the real binary ---------------
        //
        // The real cascade.exe, walking the real bus. Three things are proved
        // here that no fake can prove: that spawning the application as its
        // own enumeration helper works at all, that the walk inside it still
        // runs under the vendor guard - the child counts its own guarded calls
        // and hands the number back - and that the real binary accepts the
        // crash directory and arms capture with it. Delete the guard from the
        // vendor walk and the count is zero; drop the argument from main()'s
        // helper dispatch and the capture flag is false.
        //
        // Four attempts, not one: this machine's libusb fault kills roughly
        // one enumeration in twenty, and four independent children all dying
        // is about one run in a million. Each of those deaths is the fix
        // working.
        {
            const std::string real = findRealCascade();
            CHECK(!real.empty());  // a moved build tree must fail, not skip

            setMode("");  // irrelevant to the real binary, but leave nothing set
            EnumOptions o;
            o.helperPath = real;
            o.allowInProcessFallback = false;
            o.attempts = 4;
            // Generous against a measured 4.9 s healthy walk, and bounded so
            // this test cannot outlive ctest's own 120 s limit: a death is
            // fast and retried, a timeout costs its budget once and is not.
            o.timeoutMs = 25000;
            const EnumResult r = enumerateIsolated(o);
            std::printf("real helper: outcome=%s attempts=%d deaths=%d deathExit=0x%08lX "
                        "devices=%zu guarded=%llu runtime=%d capture=%d elapsed=%lu ms\n",
                        enumOutcomeName(r.outcome), r.attempts, r.childDeaths,
                        r.deathExitCode, r.devices.size(),
                        static_cast<unsigned long long>(r.guardedCalls),
                        r.childRuntimeAvailable ? 1 : 0, r.childCaptureArmed ? 1 : 0,
                        r.elapsedMs);
            CHECK(r.outcome == EnumOutcome::Ok);
            CHECK(rowsWellFormed(r));
            // EXACTLY TWO guarded calls when the SoapySDR runtime is present,
            // and exactly none when it is absent and both are correctly
            // skipped. Written as one expression rather than an `if`, so
            // neither branch can vanish into a case that was never checked.
            //
            // WAS ONE until 0.62.3, and this is a correction rather than a
            // loosening: the walk used to be the only crossing into SoapySDR
            // that went through the guard. It is now the SECOND of two, because
            // runtimeAvailable() guards the one-off module search-path fix
            // (SoapySDR::getABIVersion) that runs before it. Both still have to
            // be there - delete either guard and this drops to 1 and fails.
            CHECK(r.guardedCalls == (r.childRuntimeAvailable ? 2ull : 0ull));
            // The REAL helper reports capture it actually armed, not capture
            // it was merely told about.
            CHECK(r.childCaptureArmed);
        }

        // OFF MEANS OFF, all the way down: with the parent's capture switched
        // back off the child is handed nothing and says so.
        cascade::core::setCrashCaptureEnabled(false, false);
        CHECK(cascade::core::activeCrashDir().empty());
        {
            setMode("ok");
            EnumOptions o;
            o.helperPath = self;
            o.allowInProcessFallback = false;
            const EnumResult r = enumerateIsolated(o);
            CHECK(r.outcome == EnumOutcome::Ok);
            CHECK(!r.childCaptureArmed);
        }

        std::filesystem::remove_all(dir, ec);
    }

    return testSummary("test_soapy_enum_proc");
}
