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
#include "core/crash_upload.hpp"
#include "core/diag_log.hpp"
#include "core/diag_report.hpp"
#include "core/telemetry.hpp"
#include "core/version.hpp"

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
#else
#include <csignal>
#include <cstdint>
#include <unistd.h>
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

// The driver this child was restricted to, or empty for the whole bus.
std::string driverArg(int argc, char** argv) {
    const char* flag = "--driver=";
    const std::size_t n = std::strlen(flag);
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], flag, n) == 0) { return std::string(argv[i] + n); }
    }
    return std::string();
}

// True when this child was asked for driver NAMES rather than devices.
bool askedToListDrivers(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--list-drivers") == 0) { return true; }
    }
    return false;
}

// The comma-separated --skip= list this child was handed, or empty.
std::string skipArg(int argc, char** argv) {
    const char* flag = "--skip=";
    const std::size_t n = std::strlen(flag);
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], flag, n) == 0) { return std::string(argv[i] + n); }
    }
    return std::string();
}

// The --attempt=N this child was told (0 when it was not told).
int attemptArg(int argc, char** argv) {
    const char* flag = "--attempt=";
    const std::size_t n = std::strlen(flag);
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], flag, n) == 0) { return std::atoi(argv[i] + n); }
    }
    return 0;
}

bool listNames(const std::string& csv, const std::string& name) {
    std::size_t start = 0;
    while (start <= csv.size()) {
        const std::size_t comma = csv.find(',', start);
        const std::size_t end = (comma == std::string::npos) ? csv.size() : comma;
        if (csv.compare(start, end - start, name) == 0) { return true; }
        if (comma == std::string::npos) { break; }
        start = comma + 1;
    }
    return false;
}

// The probe log the REAL child writes, in the real format, straight to the
// pipe - flushed, because the next thing this process may do is die.
void probeLine(bool begin, const char* driver) {
    const std::string line = cascade::source::probeMarkerLine(begin, driver);
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
}

// THE DEATH IN FIELD REPORT 91965660116CF497: STATUS_HEAP_CORRUPTION, which
// Windows raises from inside the heap manager and which no user-mode filter
// sees - so the exit code is all the parent ever gets. Linux has no such
// status: glibc's heap checks abort(), which the parent reads as 128 + SIGABRT.
#ifdef _WIN32
constexpr unsigned long kHeapCorruptionExit = 0xC0000374ul;
#else
constexpr unsigned long kHeapCorruptionExit = 128ul + SIGABRT;
#endif

[[noreturn]] void dieOfHeapCorruption() {
    std::fflush(stdout);
#ifdef _WIN32
    ::TerminateProcess(::GetCurrentProcess(), static_cast<UINT>(kHeapCorruptionExit));
#else
    std::signal(SIGABRT, SIG_DFL);
    std::raise(SIGABRT);
#endif
    std::_Exit(99);  // unreachable
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
    if (mode == "onebaddriver") {
        // THE MACHINE IN FIELD REPORT 650B88A1, faked: asked for the whole
        // bus it dies EVERY time (so the retry cannot help), it can still
        // list its drivers, and of the two, one faults and one answers.
        const std::string driver = driverArg(argc, argv);
        if (askedToListDrivers(argc, argv)) {
            std::printf("{\"schema\":1,\"runtime\":true,\"guardedCalls\":1,\"capture\":%s,"
                        "\"drivers\":[\"good\",\"bad\"],\"devices\":[]}\n",
                        gotCrashDir ? "true" : "false");
            return 0;
        }
        if (driver == "good") {
            std::printf("{\"schema\":1,\"runtime\":true,\"guardedCalls\":1,\"capture\":%s,"
                        "\"devices\":[{\"label\":\"good radio\",\"args\":\"driver=good,serial=9\"}]}\n",
                        gotCrashDir ? "true" : "false");
            return 0;
        }
        // "bad", and the whole-bus probe, die the same way the field does.
        return 7;
    }
    if (mode == "heapdriver") {
        // THE MACHINE IN FIELD REPORT 91965660116CF497, faked. Two drivers:
        // "good" answers, "sdrplay" dies with 0xC0000374 whenever it is asked.
        // The whole-bus walk logs its probes the way the real child does -
        // both begin, "good" finishes, "sdrplay" is still running when the
        // process dies - and, when told to --skip sdrplay, never starts it and
        // answers with good's radio.
        const std::string driver = driverArg(argc, argv);
        const char* cap = gotCrashDir ? "true" : "false";
        if (askedToListDrivers(argc, argv)) {
            std::printf("{\"schema\":1,\"runtime\":true,\"guardedCalls\":1,\"capture\":%s,"
                        "\"drivers\":[\"good\",\"sdrplay\"],\"devices\":[]}\n", cap);
            return 0;
        }
        if (driver == "sdrplay") { dieOfHeapCorruption(); }
        const bool skipBad = listNames(skipArg(argc, argv), "sdrplay");
        if (driver.empty()) {
            probeLine(true, "good");
            if (!skipBad) { probeLine(true, "sdrplay"); }
            probeLine(false, "good");
            if (!skipBad) { dieOfHeapCorruption(); }
        }
        std::printf("{\"schema\":1,\"runtime\":true,\"guardedCalls\":2,\"capture\":%s,"
                    "\"devices\":[{\"label\":\"good radio\",\"args\":\"driver=good,serial=9\"}]}\n",
                    cap);
        return 0;
    }
    if (mode == "tworadios") {
        // A STREAMING RTL-SDR BESIDE A B200 (2026-09-23). The whole bus lists
        // both; each driver lists its own; and the rtlsdr driver, asked on its
        // own, DIES with 42 - so a scan that was told to leave it out and asks
        // it anyway is a named failure, not a quiet extra row.
        const std::string driver = driverArg(argc, argv);
        const char* cap = gotCrashDir ? "true" : "false";
        if (askedToListDrivers(argc, argv)) {
            std::printf("{\"schema\":1,\"runtime\":true,\"guardedCalls\":1,\"capture\":%s,"
                        "\"drivers\":[\"rtlsdr\",\"uhd\"],\"devices\":[]}\n", cap);
            return 0;
        }
        if (driver == "rtlsdr") { return 42; }
        if (driver == "uhd") {
            std::printf("{\"schema\":1,\"runtime\":true,\"guardedCalls\":2,\"capture\":%s,"
                        "\"devices\":[{\"label\":\"B200\",\"args\":\"driver=uhd,serial=31\"}]}\n",
                        cap);
            return 0;
        }
        std::printf("{\"schema\":1,\"runtime\":true,\"guardedCalls\":2,\"capture\":%s,"
                    "\"devices\":[{\"label\":\"RTL-SDR\",\"args\":\"driver=rtlsdr,serial=1\"},"
                    "{\"label\":\"B200\",\"args\":\"driver=uhd,serial=31\"}]}\n",
                    cap);
        return 0;
    }
    if (mode == "slowdriver" || mode == "manywedged") {
        // A DRIVER THAT NEVER ANSWERS, beside a radio that is open (bug hunt
        // 2026-09-24, soapy-enum-2). "slowdriver": good, wedged, late - the
        // middle one sleeps a minute when asked on its own. "manywedged":
        // three that sleep, then one good one. The whole bus answers at once
        // with the healthy rows, which is what "the bus is fine now" looks
        // like to the parent.
        const std::string driver = driverArg(argc, argv);
        const char* cap = gotCrashDir ? "true" : "false";
        const bool many = mode == "manywedged";
        if (askedToListDrivers(argc, argv)) {
            std::printf("{\"schema\":1,\"runtime\":true,\"guardedCalls\":1,\"capture\":%s,"
                        "\"drivers\":%s,\"devices\":[]}\n",
                        cap,
                        many ? "[\"w1\",\"w2\",\"w3\",\"good\"]"
                             : "[\"good\",\"wedged\",\"late\"]");
            return 0;
        }
        const bool wedged = many ? (driver == "w1" || driver == "w2" || driver == "w3")
                                 : driver == "wedged";
        if (wedged) {
            probeLine(true, driver.c_str());
#ifdef _WIN32
            ::Sleep(60000);
#else
            ::sleep(60);
#endif
            return 0;
        }
        if (driver.empty()) {
            std::printf("{\"schema\":1,\"runtime\":true,\"guardedCalls\":2,\"capture\":%s,"
                        "\"devices\":[{\"label\":\"good radio\",\"args\":\"driver=good,serial=1\"}]}\n",
                        cap);
            return 0;
        }
        std::printf("{\"schema\":1,\"runtime\":true,\"guardedCalls\":1,\"capture\":%s,"
                    "\"devices\":[{\"label\":\"%s radio\",\"args\":\"driver=%s,serial=1\"}]}\n",
                    cap, driver.c_str(), driver.c_str());
        return 0;
    }
    if (mode == "uhdtrap") {
        // FIELD REPORT F204602B5329B268's MACHINE, faked (2026-09-25): an
        // SDRplay and no USRP, where UHD's probe is the one that dies. The
        // listing has "good" and "uhd"; asking uhd - on its own, or as part
        // of a whole bus that was not told to --skip it - dies with 43, so a
        // scan that asks it when it was told not to is a named failure.
        const std::string driver = driverArg(argc, argv);
        const char* cap = gotCrashDir ? "true" : "false";
        if (askedToListDrivers(argc, argv)) {
            std::printf("{\"schema\":1,\"runtime\":true,\"guardedCalls\":1,\"capture\":%s,"
                        "\"drivers\":[\"good\",\"uhd\"],\"devices\":[]}\n", cap);
            return 0;
        }
        if (driver == "uhd") { return 43; }
        if (driver.empty() && !listNames(skipArg(argc, argv), "uhd")) { return 43; }
        std::printf("{\"schema\":1,\"runtime\":true,\"guardedCalls\":2,\"capture\":%s,"
                    "\"devices\":[{\"label\":\"good radio\",\"args\":\"driver=good,serial=9\"}]}\n",
                    cap);
        return 0;
    }
    if (mode == "asiotrap") {
        // THE 2026-10-01 MACHINE, faked: a Native Instruments ASIO driver
        // behind SoapyAudio. The listing has "good" and "audio"; asking audio
        // - on its own, or as part of a whole bus that was not told to --skip
        // it - dies with 0xC0000005, as the field child did every time.
        const std::string driver = driverArg(argc, argv);
        const char* cap = gotCrashDir ? "true" : "false";
        if (askedToListDrivers(argc, argv)) {
            std::printf("{\"schema\":1,\"runtime\":true,\"guardedCalls\":1,\"capture\":%s,"
                        "\"drivers\":[\"good\",\"audio\"],\"devices\":[]}\n", cap);
            return 0;
        }
        const bool asksAudio =
            driver == "audio" || (driver.empty() && !listNames(skipArg(argc, argv), "audio"));
        if (asksAudio) {
            std::fflush(stdout);
#ifdef _WIN32
            ::TerminateProcess(::GetCurrentProcess(), 0xC0000005u);
#endif
            std::_Exit(139);
        }
        std::printf("{\"schema\":1,\"runtime\":true,\"guardedCalls\":2,\"capture\":%s,"
                    "\"devices\":[{\"label\":\"good radio\",\"args\":\"driver=good,serial=9\"}]}\n",
                    cap);
        return 0;
    }
    if (mode == "asiosweep") {
        // THE SAME MACHINE, WITH A WHOLE BUS THAT DIES FOR ANOTHER REASON (a
        // stand-in for the libusb fault): every attempt of the whole bus dies,
        // so the parent lists the driver names - "good" and "audio" - and asks
        // each on its own. Asking audio, as the 0.99.57 sweep did, dies again.
        const std::string driver = driverArg(argc, argv);
        const char* cap = gotCrashDir ? "true" : "false";
        if (askedToListDrivers(argc, argv)) {
            std::printf("{\"schema\":1,\"runtime\":true,\"guardedCalls\":1,\"capture\":%s,"
                        "\"drivers\":[\"good\",\"audio\"],\"devices\":[]}\n", cap);
            return 0;
        }
        if (driver.empty() || driver == "audio") {
            std::fflush(stdout);
#ifdef _WIN32
            ::TerminateProcess(::GetCurrentProcess(), 0xC0000005u);
#endif
            std::_Exit(139);
        }
        std::printf("{\"schema\":1,\"runtime\":true,\"guardedCalls\":2,\"capture\":%s,"
                    "\"devices\":[{\"label\":\"good radio\",\"args\":\"driver=good,serial=9\"}]}\n",
                    cap);
        return 0;
    }
    if (mode == "answeredthendied" || mode == "probesdonethendied") {
        // A CHILD THAT DIES AFTER EVERY PROBE HAS ENDED - the field child of
        // 2026-10-01, which died in a vendor module's DLL detach as it exited.
        //   answeredthendied    its whole answer is already on the pipe;
        //   probesdonethendied  it dies between the walk and the answer.
        // Either way no probe was running, so "no driver's probe had begun"
        // (what the field report said) is not the truth.
        probeLine(true, "good");
        probeLine(true, "audio");
        probeLine(false, "audio");
        probeLine(false, "good");
        if (mode == "answeredthendied") { printOkJson(gotCrashDir); }
        std::fflush(stdout);
#ifdef _WIN32
        ::TerminateProcess(::GetCurrentProcess(), 0xC0000005u);
#endif
        std::_Exit(139);
    }
    if (mode == "armeduhd") {
        // THE REAL CHILD'S HANDLER, dying in the uhd probe: the listing names
        // only "uhd", and asked for it the child arms exactly as the real
        // helper does (armEnumerateHelperProcess, with the directory the
        // parent handed down) and then faults. What reaches the parent is
        // whatever that production handler writes to the pipe.
        const std::string driver = driverArg(argc, argv);
        if (askedToListDrivers(argc, argv)) {
            std::printf("{\"schema\":1,\"runtime\":true,\"guardedCalls\":1,\"capture\":%s,"
                        "\"drivers\":[\"uhd\"],\"devices\":[]}\n",
                        gotCrashDir ? "true" : "false");
            return 0;
        }
        std::string crashDir;
        for (int i = 1; i < argc; ++i) {
            const char* flag = "--crash-dir=";
            if (std::strncmp(argv[i], flag, std::strlen(flag)) == 0) {
                crashDir = argv[i] + std::strlen(flag);
            }
        }
        // The production tail of the reason line, from the arguments this
        // child was really given (--driver, --attempt) - the wiring from the
        // parent's command line to the child's report is what is under test.
        const std::string suffix = cascade::source::enumerateChildReasonSuffix(
            false, driver.c_str(), attemptArg(argc, argv));
        cascade::source::armEnumerateHelperProcess(crashDir.c_str(), suffix.c_str());
        probeLine(true, driver.empty() ? "uhd" : driver.c_str());
        cascade::core::raiseTestFault(cascade::core::TestFaultKind::AccessViolation);
        return 0;  // unreachable
    }
    if (mode == "fieldlineuhd") {
        // "armeduhd" WITHOUT THE HANDLER: the field's own line, verbatim, on
        // the pipe, and then a death that leaves no report of its own - so the
        // parent's per-driver report is the only one, and carries the line.
        if (askedToListDrivers(argc, argv)) {
            std::printf("{\"schema\":1,\"runtime\":true,\"guardedCalls\":1,\"capture\":%s,"
                        "\"drivers\":[\"uhd\"],\"devices\":[]}\n",
                        gotCrashDir ? "true" : "false");
            return 0;
        }
        const std::string line = std::string(cascade::core::kFaultLinePrefix) +
                                 "access violation 0xC0000005 at libusb-1.0.dll+0x10490\n";
        std::fwrite(line.data(), 1, line.size(), stdout);
        std::fflush(stdout);
#ifdef _WIN32
        ::TerminateProcess(::GetCurrentProcess(), 0xC0000005u);
#endif
        std::_Exit(139);
    }
    if (mode == "armedwholebus" || mode == "fieldline") {
        // THE WHOLE-BUS DEATH'S REASON, AT FULL LENGTH (review of 5e7b968).
        // The listing fails (exit 5), so no sweep follows and the only
        // reports are the whole-bus ones. Asked for the whole bus, the child
        // logs the field machine's probes and leaves two still running, then
        // dies:
        //   armedwholebus  through the PRODUCTION handler
        //                  (armEnumerateHelperProcess + a real fault), whose
        //                  line names this test binary - longer than libusb's;
        //   fieldline      writing the field's own longest line verbatim,
        //                  then dying with its code.
        if (askedToListDrivers(argc, argv)) { return 5; }
        probeLine(true, "sdrplay");
        probeLine(true, "uhd");
        probeLine(true, "rtlsdr");
        probeLine(false, "rtlsdr");
        if (mode == "fieldline") {
            const std::string line = std::string(cascade::core::kFaultLinePrefix) +
                                     "access violation 0xC0000005 at libusb-1.0.dll+0x10490\n";
            std::fwrite(line.data(), 1, line.size(), stdout);
            std::fflush(stdout);
#ifdef _WIN32
            ::TerminateProcess(::GetCurrentProcess(), 0xC0000005u);
#endif
            std::_Exit(139);
        }
        std::string crashDir;
        for (int i = 1; i < argc; ++i) {
            const char* flag = "--crash-dir=";
            if (std::strncmp(argv[i], flag, std::strlen(flag)) == 0) {
                crashDir = argv[i] + std::strlen(flag);
            }
        }
        const std::string suffix = cascade::source::enumerateChildReasonSuffix(
            false, nullptr, attemptArg(argc, argv));
        cascade::source::armEnumerateHelperProcess(crashDir.c_str(), suffix.c_str());
        cascade::core::raiseTestFault(cascade::core::TestFaultKind::AccessViolation);
        return 0;  // unreachable
    }
#ifdef _WIN32
    if (mode == "unresolved") {
        // TWO DRIVERS THAT FAULT IN NO MODULE AT ALL (2026-10-04): the listing
        // has "adrv" and "bdrv", and every walk - the whole bus and each driver
        // alone - arms the production handler (the production tail and tag, from
        // the arguments it was really given) and executes a page of private
        // memory with UD2 in it, so the faulting address resolves to nothing.
        // Such faults used to share one signature whatever driver they were in.
        const std::string driver = driverArg(argc, argv);
        if (askedToListDrivers(argc, argv)) {
            std::printf("{\"schema\":1,\"runtime\":true,\"guardedCalls\":1,\"capture\":%s,"
                        "\"drivers\":[\"adrv\",\"bdrv\"],\"devices\":[]}\n",
                        gotCrashDir ? "true" : "false");
            return 0;
        }
        std::string crashDir;
        for (int i = 1; i < argc; ++i) {
            const char* flag = "--crash-dir=";
            if (std::strncmp(argv[i], flag, std::strlen(flag)) == 0) {
                crashDir = argv[i] + std::strlen(flag);
            }
        }
        const std::string suffix = cascade::source::enumerateChildReasonSuffix(
            false, driver.c_str(), attemptArg(argc, argv));
        const std::string tag = cascade::source::childFaultSignatureTag(driver);
        cascade::source::armEnumerateHelperProcess(crashDir.c_str(), suffix.c_str(), tag.c_str());
        void* page = ::VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (page == nullptr) { return 10; }
        const unsigned char ud2[] = {0x0F, 0x0B};
        std::memcpy(page, ud2, sizeof(ud2));
        using Code = void (*)();
        reinterpret_cast<Code>(page)();
        return 7;  // never reached
    }
#endif
    if (mode == "armedterminate" || mode == "armedabsorbed") {
        // A CHILD WITH THE PRODUCTION HANDLER ARMED THAT DIES WITHOUT IT EVER
        // RUNNING (2026-10-04, one report per death): a heap corruption, a
        // vendor TerminateProcess - the deaths no user-mode filter sees, so the
        // child leaves NO report of its own for them and the parent's stackless
        // one is the only record there will ever be.
        //   armedterminate  armed, then dies unseen;
        //   armedabsorbed   armed, files an ABSORBED vendor fault first (a
        //                   report the child's own guard wrote and then
        //                   carried on from), and only then dies unseen. That
        //                   file carries this child's process id too, and must
        //                   not be mistaken for the report of the death.
        // The listing fails (exit 5), so no sweep follows.
        if (askedToListDrivers(argc, argv)) { return 5; }
        std::string crashDir;
        for (int i = 1; i < argc; ++i) {
            const char* flag = "--crash-dir=";
            if (std::strncmp(argv[i], flag, std::strlen(flag)) == 0) {
                crashDir = argv[i] + std::strlen(flag);
            }
        }
        cascade::source::armEnumerateHelperProcess(crashDir.c_str());
        if (mode == "armedabsorbed") {
            cascade::core::reportAbsorbedFault(
                "fault in a third-party SDR module, absorbed by the vendor-call guard "
                "(the process continued; the call reported failure to its caller)",
                0xC0000005ul, nullptr, nullptr);
        }
        dieOfHeapCorruption();
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
        return 0;  // unreachable on Windows
#else
        // THE LINUX EQUIVALENT: a real SIGSEGV, not a return code. No dump or
        // dialog exists to suppress on this platform - a signal death simply
        // exits the process - so there is nothing here to disable first. The
        // parent side reads this back as WIFSIGNALED(SIGSEGV), reported as
        // exitCode 128+11=139; see the "0xC0000005 on Windows, 139 on Linux"
        // comment below at the count of injected faults.
        volatile int* p = nullptr;
        *p = 1;
        return 0;  // unreachable
#endif
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
        // A probe that began and never ended is the one that wedged - the
        // parent names it from this line when the budget runs out.
        probeLine(true, "wedged");
#ifdef _WIN32
        ::Sleep(60000);
#else
        ::sleep(60);
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
    return static_cast<unsigned long>(::getpid());
#endif
}

// THE BUDGET AND THE STOPWATCH ARE NOT THE SAME CLOCK, and this is the number
// that reconciles them.
//
// runOneChild bounds a wedged probe with WaitForSingleObject(child, timeoutMs).
// That is a KERNEL wait: its due time is computed on the interrupt-time clock,
// which advances in whole ticks of lpTimeIncrement - 15.625 ms on a default
// Windows box, and what GetSystemTimeAdjustment reports below. EnumResult's
// elapsedMs, by contrast, is measured with steady_clock, i.e. QPC, which is
// sub-microsecond. So a wait that honoured the whole budget can legitimately be
// MEASURED as short of it, by up to one tick.
//
// Measured on this bench, 2026-09-15: WaitForSingleObject(event, 1500) returned
// with QPC reading 1484.6 ms at worst over fifteen waits, and the timeout case
// below produced elapsedMs of 1488..1515 over twenty runs of one md5-pinned
// binary - failing a bare ">= 1500" on nine of them while the other 127 checks
// never moved. Nothing was waiting early; the stopwatch simply has finer
// resolution than the alarm.
//
// One tick of slack is therefore the honest floor, and it is deliberately no
// more than that: the property the check exists for is "a wedged probe is not
// ABANDONED before its budget", and abandoning it early misses by hundreds of
// milliseconds, not by a tick.
unsigned long waitClockTickMs() {
#ifdef _WIN32
    DWORD adjustment = 0;
    DWORD increment = 0;
    BOOL disabled = FALSE;
    if (::GetSystemTimeAdjustment(&adjustment, &increment, &disabled) != 0 && increment > 0) {
        // 100 ns units, rounded UP: a tolerance that undershoots the real tick
        // is the flake we are here to remove.
        const unsigned long ms = static_cast<unsigned long>((increment + 9999ul) / 10000ul);
        // A machine whose timer has been pushed to 0.5 ms still gets a whole
        // millisecond, and one reporting something absurd does not get to
        // excuse a genuinely early return.
        return (ms < 1ul) ? 1ul : ((ms > 20ul) ? 20ul : ms);
    }
    return 16ul;  // the default Windows tick, rounded up
#else
    // POSIX has no child-wait path in runOneChild yet (SpawnFailed), so this is
    // only ever the conservative default.
    return 16ul;
#endif
}

#ifdef _WIN32
std::string selfExePath() { return std::filesystem::path(selfExePathW()).string(); }
#else
// /proc/self/exe is always this running binary, argv[0] notwithstanding -
// the same reasoning as the product's own enumerateHelperPath() in
// soapy_enum_proc.cpp, which this test's helper resolution below is checked
// against.
std::string selfExePath() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) { return std::string(); }
    return std::string(buf, static_cast<std::size_t>(n));
}
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
#ifdef _WIN32
        dir / "cascade.exe",
        dir.parent_path().parent_path() / "Release" / "cascade.exe",
#else
        // Single-config Ninja layout: tests build into build/tests/ and the
        // app into build/ - one level up from this binary's directory,
        // rather than the multi-config MSVC layout's two.
        dir.parent_path() / "cascade",
#endif
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

// Only the "reason:" lines of `reports` - for a check that something is NOT
// said, which must not trip over this process's own earlier log lines that
// every report's log ring also carries.
std::string reasonLines(const std::string& reports) {
    std::string out;
    std::size_t pos = 0;
    while ((pos = reports.find("reason: ", pos)) != std::string::npos) {
        if (pos == 0 || reports[pos - 1] == '\n') {
            const std::size_t eol = reports.find('\n', pos);
            out += reports.substr(pos, (eol == std::string::npos ? reports.size() : eol) - pos);
            out += '\n';
        }
        pos += 8;
    }
    return out;
}

bool rowsWellFormed(const EnumResult& r) {
    for (const auto& d : r.devices) {
        if (d.label.empty() || d.args.empty()) { return false; }
    }
    return true;
}

// --- one report per child death (2026-10-04) --------------------------------
//
// The uploader's own reading of a report, so "has a stack" and "would be sent"
// are asked of the same parser and the same decision the product uses rather
// than of a substring.

// True when the report parses and carries at least one frame on some thread.
// The stackless child-death report parses too - it just has nothing under its
// stack heading - which is exactly the difference.
bool reportHasStack(const std::string& text) {
    cascade::core::ParsedReport p;
    if (!cascade::core::parseReportText(text, p)) { return false; }
    for (const auto& t : p.threads) {
        if (!t.frames.empty()) { return true; }
    }
    return false;
}

std::string signatureOf(const std::string& text) {
    cascade::core::ParsedReport p;
    if (!cascade::core::parseReportText(text, p)) { return std::string(); }
    return p.signature;
}

// How many of `texts` the uploader would SEND inside ONE 24-hour window: each
// is put through the real decideUpload against the real policy state, and
// noteSent is called for the ones it lets through, exactly as the sweep does.
// Never more than kMaxPerWindow, so this is a count of distinct faults up to
// the daily budget, which is the budget the report is spending.
int wouldSend(const std::vector<std::string>& texts) {
    cascade::core::UploadPolicyState state;
    constexpr std::uint64_t kNow = 1800000000ull;
    int sent = 0;
    for (const std::string& t : texts) {
        cascade::core::ParsedReport p;
        if (!cascade::core::parseReportText(t, p)) { continue; }
        if (cascade::core::decideUpload(state, true, p.signature, kNow) ==
            cascade::core::UploadDecision::Send) {
            cascade::core::noteSent(state, p.signature, kNow);
            ++sent;
        }
    }
    return sent;
}

std::vector<std::string> reportTexts(const std::filesystem::path& dir) {
    std::vector<std::string> out;
    for (const auto& p : crashReports(dir)) { out.push_back(readAll(p)); }
    return out;
}

// Every file in `dir` of any name: "no file from either process" is a claim
// about the directory, not about the crash-*.txt ones.
std::size_t filesIn(const std::filesystem::path& dir) {
    std::size_t n = 0;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        (void)e;
        ++n;
    }
    return n;
}

// THE DRIVERS THIS MACHINE'S SOAPYSDR INSTALL BRINGS, left out of the scans
// that stage a fault in a fixture driver. The sweep asks every driver of the
// listing in a child of its own, and this install lists a dozen real ones
// (vcpkg's default module directory) whose probes would open real radios -
// minutes of wall time, and a real libusb fault to confuse a count of
// reports. "sdrplay" is here because the sdrplay fixture shares its name with
// the real module. The fixtures' own drivers are the only ones left to ask.
std::vector<std::string> realDriversToLeaveOut() {
    return {"airspy",   "airspyhf", "bladerf",   "hackrf", "lime",
            "netsdr",   "null",     "plutosdr",  "redpitaya", "remote",
            "rtlsdr",   "sdrplay",  "uhd"};
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
    // The fault fixture's directory (tests/CMakeLists.txt), this test's one
    // argument; the blocks that need it fail without it rather than skip.
    // Made absolute: the child loads modules with the default-directories
    // search order (soapy_modules.cpp), where a relative directory finds
    // nothing - measured, a relative argument listed no fixture at all.
    std::string fixtureDir;
    if (argc >= 2 && argv[1][0] != '-') {
        std::error_code aec;
        fixtureDir = std::filesystem::absolute(std::filesystem::path(argv[1]), aec).string();
    }
    // The DLL the fault fixture maps by itself in its "late" stage (this
    // test's second argument, tests/CMakeLists.txt) - never in fixtureDir.
    std::string lateDll;
    if (argc >= 3 && argv[2][0] != '-') {
        std::error_code aec;
        lateDll = std::filesystem::absolute(std::filesystem::path(argv[2]), aec).string();
    }
    // The SECOND faulting driver's directory (this test's third argument): a
    // module of another name, so its faults hash to another signature.
    std::string fixtureDirB;
    if (argc >= 4 && argv[3][0] != '-') {
        std::error_code aec;
        fixtureDirB = std::filesystem::absolute(std::filesystem::path(argv[3]), aec).string();
    }

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
        // whatever this test binary happens to be called. The name itself is
        // platform-specific: CMakeLists.txt's add_executable(cascade ...)
        // gets a ".exe" suffix from the linker on Windows and none at all on
        // Linux, so the literal string this checks against has to follow it.
#ifdef _WIN32
        CHECK(std::filesystem::path(resolved).filename().string() == "cascade.exe");
#else
        CHECK(std::filesystem::path(resolved).filename().string() == "cascade");
#endif
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

    // --- ONE BAD DRIVER MUST NOT HIDE THE OTHERS --------------------------
    //
    // Field report 650B88A1 (0.99.6, Windows 10.0.26200, and 0.96.3 before
    // it): a libusb-based module faulted on EVERY probe of one machine, so
    // both children died and the Source menu told a user with a radio
    // plugged in that there were no radios at all. A retry cannot help a
    // fault that happens every time; asking each driver separately can.
    {
        setMode("onebaddriver");
        EnumOptions o;
        o.helperPath = self;
        o.allowInProcessFallback = false;
        const EnumResult r = enumerateIsolated(o);
        // The working driver's radio is listed, which is the whole point.
        const Rows wantRows{{"good radio", "driver=good,serial=9"}};
        const std::vector<std::string> wantAsked{"good", "bad"};
        const std::vector<std::string> wantFaulted{"bad"};
        CHECK(r.outcome == EnumOutcome::Ok);
        CHECK(rowsOf(r) == wantRows);
        CHECK(r.sweptPerDriver);
        CHECK(r.sweptDrivers == wantAsked);
        // The faulting one is NAMED - the whole-bus death never could be.
        CHECK(r.faultedDrivers == wantFaulted);
        // The whole-bus probe still ran exactly twice; the sweep counts its
        // own children (the listing, then one per driver).
        CHECK(r.attempts == 2);
        CHECK(r.sweepChildren == 3);
        // Every death is still counted: two whole-bus, one driver.
        CHECK(r.childDeaths == 3);
        // ...and the driver that died in its own child is remembered for the
        // session (see the 91965660116CF497 block for what that buys).
        const auto session = cascade::source::sessionFaultedDrivers();
        CHECK(session.size() == 1u);
        for (const auto& f : session) {
            CHECK(f.driver == "bad");
            CHECK(f.exitCode == 7ul);
        }
        cascade::source::clearSessionFaultedDriversForTest();
        CHECK(cascade::source::sessionFaultedDrivers().empty());
    }
    {
        // A MACHINE WHERE EVEN THE DRIVER LIST DIES is left exactly where it
        // was: no devices, ChildDied, and no pretence that a sweep happened.
        setMode("die");
        EnumOptions o;
        o.helperPath = self;
        o.allowInProcessFallback = false;
        const EnumResult r = enumerateIsolated(o);
        CHECK(r.outcome == EnumOutcome::ChildDied);
        CHECK(r.devices.empty());
        CHECK(!r.sweptPerDriver);
        CHECK(r.faultedDrivers.empty());
    }
    {
        // A HEALTHY MACHINE NEVER PAYS FOR ANY OF THIS: one child, no sweep.
        setMode("ok");
        EnumOptions o;
        o.helperPath = self;
        o.allowInProcessFallback = false;
        const EnumResult r = enumerateIsolated(o);
        CHECK(r.outcome == EnumOutcome::Ok);
        CHECK(r.attempts == 1);
        CHECK(r.sweepChildren == 0);
        CHECK(!r.sweptPerDriver);
    }
    {
        // SCANNING WHILE A RADIO IS OPEN (2026-09-23). The owner's B200 never
        // appeared on the patch page while the receiver had an RTL-SDR open,
        // because the whole scan was deferred: SoapyRTLSDR's probe opens and
        // resets every RTL dongle on the bus, the streaming one included (the
        // 0.90.0 field fault). Only THAT driver has to stay out. skipDrivers
        // names it, the whole bus is never probed, and every other driver is
        // asked on its own - so the B200 is found and the dongle is untouched.
        setMode("tworadios");
        EnumOptions o;
        o.helperPath = self;
        o.allowInProcessFallback = false;
        o.skipDrivers = {"RTLSDR"};  // case does not matter: registries vary
        const EnumResult r = enumerateIsolated(o);
        const Rows wantRows{{"B200", "driver=uhd,serial=31"}};
        const std::vector<std::string> wantSkipped{"rtlsdr"};
        const std::vector<std::string> wantAsked{"uhd"};
        CHECK(r.outcome == EnumOutcome::Ok);
        CHECK(rowsOf(r) == wantRows);
        CHECK(r.attempts == 0);         // no whole-bus child at all
        CHECK(r.sweepChildren == 2);    // the listing, then uhd on its own
        CHECK(r.childDeaths == 0);      // rtlsdr was never asked (it dies with 42)
        CHECK(r.faultedDrivers.empty());
        CHECK(r.sweptDrivers == wantAsked);
        CHECK(r.skippedDrivers == wantSkipped);
    }
    {
        // ...and with nothing to leave out, the same machine gets the ordinary
        // single whole-bus child, both radios listed.
        setMode("tworadios");
        EnumOptions o;
        o.helperPath = self;
        o.allowInProcessFallback = false;
        const EnumResult r = enumerateIsolated(o);
        const Rows wantRows{{"RTL-SDR", "driver=rtlsdr,serial=1"}, {"B200", "driver=uhd,serial=31"}};
        CHECK(r.outcome == EnumOutcome::Ok);
        CHECK(rowsOf(r) == wantRows);
        CHECK(r.attempts == 1);
        CHECK(r.sweepChildren == 0);
        CHECK(r.skippedDrivers.empty());
    }
    // --- FIELD REPORT F204602B5329B268: UHD IS NOT ASKED WITH NOTHING TO FIND
    //
    // 0.99.35: the child probing driver=uhd died on a machine whose only radio
    // was an SDRplay. EnumOptions::absentDrivers leaves a driver out of every
    // walk - the whole bus (through the child's --skip), the sweep after a
    // whole-bus death, and the walk beside an open radio - WITHOUT turning the
    // scan into a beside-a-radio walk. The fake dies with 43 whenever uhd is
    // asked, so asking it anyway is a named failure.
    {
        // THE CONTROL: told nothing, the fake machine's whole bus dies twice
        // and the sweep's uhd child dies too. Without this the checks below
        // could pass against a fake that never dies at all.
        cascade::source::clearSessionFaultedDriversForTest();
        setMode("uhdtrap");
        EnumOptions o;
        o.helperPath = self;
        o.allowInProcessFallback = false;
        const EnumResult r = enumerateIsolated(o);
        const std::vector<std::string> wantUhd{"uhd"};
        CHECK(r.attempts == 2);
        CHECK(r.childDeaths == 3);
        CHECK(r.faultedDrivers == wantUhd);
        CHECK(r.absentDrivers.empty());
        cascade::source::clearSessionFaultedDriversForTest();
    }
    {
        // THE FIX: one whole-bus child, told to skip uhd, and nothing dies.
        setMode("uhdtrap");
        EnumOptions o;
        o.helperPath = self;
        o.allowInProcessFallback = false;
        o.absentDrivers = {"UHD"};  // case does not matter
        const EnumResult r = enumerateIsolated(o);
        const Rows wantRows{{"good radio", "driver=good,serial=9"}};
        const std::vector<std::string> wantUhd{"uhd"};
        CHECK(r.outcome == EnumOutcome::Ok);
        CHECK(rowsOf(r) == wantRows);
        CHECK(r.attempts == 1);       // the ordinary single whole-bus child...
        CHECK(r.sweepChildren == 0);  // ...not a per-driver walk
        CHECK(r.childDeaths == 0);
        CHECK(r.absentDrivers == wantUhd);
        CHECK(r.skippedDrivers.empty());
        CHECK(r.sessionSkippedDrivers.empty());
        CHECK(cascade::source::sessionFaultedDrivers().empty());
    }
    {
        // ...AND BESIDE AN OPEN RADIO, where every driver is asked on its own:
        // uhd is not among them.
        setMode("uhdtrap");
        EnumOptions o;
        o.helperPath = self;
        o.allowInProcessFallback = false;
        o.skipDrivers = {"sdrplay"};
        o.absentDrivers = {"uhd"};
        const EnumResult r = enumerateIsolated(o);
        const std::vector<std::string> wantGood{"good"};
        const std::vector<std::string> wantUhd{"uhd"};
        CHECK(r.outcome == EnumOutcome::Ok);
        CHECK(r.sweptDrivers == wantGood);
        CHECK(r.childDeaths == 0);
        CHECK(r.absentDrivers == wantUhd);
        CHECK(r.faultedDrivers.empty());
        cascade::source::clearSessionFaultedDriversForTest();
    }
    // --- 2026-10-01: THE SOAPYSDR AUDIO DRIVER IS NEVER ASKED ---------------
    //
    // 0.99.57 on Windows 10 19045: SoapyAudio's probe initialised a Native
    // Instruments ASIO driver ("Audio Kontrol 1"), and every child that asked
    // it died - three reports and a per-driver sweep for one scan. Nothing it
    // could list was ever offered: every driver=audio row is dropped from the
    // Source list (gui/app_window.cpp isAudioDriver), refused on open, and
    // skipped by --soapy-check and --rds-check; sound cards are FoxSDR's own
    // sound-card source since 0.99.38. So it is not asked by any walk - the
    // whole bus (through --skip), the sweep, or the walk beside an open radio.
    // The fake dies with 0xC0000005 whenever audio is asked.
    {
        cascade::source::clearSessionFaultedDriversForTest();
        setMode("asiotrap");
        EnumOptions o;
        o.helperPath = self;
        o.allowInProcessFallback = false;
        const EnumResult r = enumerateIsolated(o);
        const Rows wantRows{{"good radio", "driver=good,serial=9"}};
        std::printf("asio trap: outcome=%s attempts=%d sweep=%d deaths=%d\n",
                    enumOutcomeName(r.outcome), r.attempts, r.sweepChildren, r.childDeaths);
        CHECK(r.outcome == EnumOutcome::Ok);
        CHECK(rowsOf(r) == wantRows);
        CHECK(r.attempts == 1);       // one whole-bus child...
        CHECK(r.sweepChildren == 0);  // ...and no sweep after it
        CHECK(r.childDeaths == 0);
        CHECK(r.faultedDrivers.empty());
        CHECK(cascade::source::sessionFaultedDrivers().empty());

        // BESIDE AN OPEN RADIO every driver is asked on its own: audio is not
        // among them.
        EnumOptions beside = o;
        beside.skipDrivers = {"rtlsdr"};
        const EnumResult b = enumerateIsolated(beside);
        const std::vector<std::string> wantGood{"good"};
        CHECK(b.outcome == EnumOutcome::Ok);
        CHECK(b.sweptDrivers == wantGood);
        CHECK(b.childDeaths == 0);
        CHECK(rowsOf(b) == wantRows);
        cascade::source::clearSessionFaultedDriversForTest();
    }
    // --- ...AND THE LOG SAYS SO, ONCE A SCAN (2026-10-04) ---------------------
    //
    // Leaving a driver out silently is a choice nobody reading a log can see:
    // the uhd skip says "not asking uhd - no hardware of theirs is on this
    // machine" and the audio one said nothing at all, so a report's log tail
    // from a machine with SoapyAudio installed carried no trace of why a
    // sound card never appeared, nor that the 0.99.57 fault had been shut out.
    // One line per scan, whichever walk ran it.
    {
        cascade::source::clearSessionFaultedDriversForTest();
        setMode("asiotrap");
        EnumOptions o;
        o.helperPath = self;
        o.allowInProcessFallback = false;
        const auto notAskingAudio = []() {
            int n = 0;
            for (const std::string& l : cascade::core::DiagLog::instance().ringSnapshot()) {
                if (l.find("soapy: not asking audio - ") != std::string::npos) { ++n; }
            }
            return n;
        };
        cascade::core::DiagLog::instance().resetForTest();
        (void)enumerateIsolated(o);
        const int whole = notAskingAudio();
        cascade::core::DiagLog::instance().resetForTest();
        EnumOptions beside = o;
        beside.skipDrivers = {"rtlsdr"};
        (void)enumerateIsolated(beside);
        const int sweep = notAskingAudio();
        std::printf("audio skip log lines: whole bus=%d beside an open radio=%d\n", whole, sweep);
        CHECK(whole == 1);
        CHECK(sweep == 1);
        cascade::core::DiagLog::instance().resetForTest();
        cascade::source::clearSessionFaultedDriversForTest();
    }
    // --- ...AND AFTER A WHOLE-BUS DEATH, THE PER-DRIVER SWEEP LEAVES IT OUT TOO
    //
    // The fourth walk, and the one the asio trap above cannot reach: its fake
    // answers the whole bus, so the sweep never runs. Here the whole bus dies
    // for a reason that is not audio's (a stand-in for the libusb fault), the
    // child is asked for its driver names - "good" and "audio" - and then
    // asked each; audio would die again if asked.
    {
        cascade::source::clearSessionFaultedDriversForTest();
        setMode("asiosweep");
        EnumOptions o;
        o.helperPath = self;
        o.allowInProcessFallback = false;
        const EnumResult r = enumerateIsolated(o);
        const Rows wantRows{{"good radio", "driver=good,serial=9"}};
        const std::vector<std::string> wantGood{"good"};
        std::printf("asio sweep: outcome=%s attempts=%d sweep=%d swept=%zu faulted=%zu\n",
                    enumOutcomeName(r.outcome), r.attempts, r.sweepChildren,
                    r.sweptDrivers.size(), r.faultedDrivers.size());
        CHECK(r.outcome == EnumOutcome::Ok);
        CHECK(r.sweptPerDriver);
        CHECK(r.sweptDrivers == wantGood);  // audio is not among the drivers asked
        CHECK(r.faultedDrivers.empty());
        CHECK(rowsOf(r) == wantRows);
        CHECK(cascade::source::sessionFaultedDrivers().empty());
        cascade::source::clearSessionFaultedDriversForTest();
    }

    // WHAT A CHILD'S HANDLER SAID, read off its stdout (childFaultLineFrom):
    // the exact bytes core::faultLine writes, found even glued to a vendor's
    // unterminated printf, two lines at most, printable ASCII only - a
    // newline in there would forge a report field - and capped.
    {
        using cascade::source::childFaultLineFrom;
        const std::string p = cascade::core::kFaultLinePrefix;
        CHECK(childFaultLineFrom("").empty());
        CHECK(childFaultLineFrom("cascade-probe: begin uhd\n").empty());
        CHECK(childFaultLineFrom(p + "access violation 0xC0000005 at libusb-1.0.dll+0x10490\n") ==
              "access violation 0xC0000005 at libusb-1.0.dll+0x10490");
        CHECK(childFaultLineFrom("vendor chatter" + p + "abort 0xE0000006 at x.dll+0x1\r\n") ==
              "abort 0xE0000006 at x.dll+0x1");
        CHECK(childFaultLineFrom(p + "a\n" + p + "b\n" + p + "c\n") == "a; b");
        CHECK(childFaultLineFrom(p + "bad\tbyte\x01here\n") == "bad?byte?here");
        CHECK(childFaultLineFrom(p + std::string(400, 'x') + "\n").size() == 160u);
    }
    {
        // NO IN-PROCESS FALLBACK BESIDE AN OPEN RADIO. With no helper, the
        // ordinary scan walks the bus in this process; a scan told to leave a
        // driver out must not, because the in-process walk asks every driver -
        // the one whose radio is open included - and refuses only for a
        // SoapySDR device, not a native one.
        EnumOptions o;
        o.helperPath = "X:\\nowhere\\definitely-not-here.exe";
        o.allowInProcessFallback = true;
        o.skipDrivers = {"rtlsdr"};
        const EnumResult r = enumerateIsolated(o);
        CHECK(r.outcome == EnumOutcome::SpawnFailed);
        CHECK(!r.fellBackInProcess);
        CHECK(r.devices.empty());
    }
    {
        // A WEDGED DRIVER BESIDE AN OPEN RADIO IS PAID FOR ONCE (bug hunt
        // 2026-09-24, soapy-enum-2). The sweep asks each driver in a child of
        // its own, one after another; a driver whose child ran out its whole
        // budget used to be asked again - and waited out again - on every
        // Refresh and every patch parts-bin open for as long as the radio
        // stayed open. It is now left out of later scans BESIDE AN OPEN RADIO
        // for the session, until a whole-bus scan answers in time.
        cascade::source::clearSessionFaultedDriversForTest();
        setMode("slowdriver");
        EnumOptions o;
        o.helperPath = self;
        o.allowInProcessFallback = false;
        o.timeoutMs = 1500;
        o.skipDrivers = {"rtlsdr"};
        const EnumResult first = enumerateIsolated(o);
        const std::vector<std::string> askedAll{"good", "wedged", "late"};
        const std::vector<std::string> onlyWedged{"wedged"};
        const Rows healthy{{"good radio", "driver=good,serial=1"},
                           {"late radio", "driver=late,serial=1"}};
        CHECK(first.outcome == EnumOutcome::Ok);
        CHECK(first.sweptDrivers == askedAll);
        CHECK(first.faultedDrivers == onlyWedged);  // named, as a timeout
        CHECK(rowsOf(first) == healthy);            // the drivers after it still asked
        CHECK(first.sessionSlowDrivers.empty());

        // THE SECOND SCAN does not ask it, lists the same radios, and does not
        // wait out the budget again.
        const EnumResult second = enumerateIsolated(o);
        const std::vector<std::string> askedHealthy{"good", "late"};
        std::printf("slow driver: first scan %lu ms, second %lu ms (budget 1500)\n",
                    first.elapsedMs, second.elapsedMs);
        CHECK(second.outcome == EnumOutcome::Ok);
        CHECK(second.sweptDrivers == askedHealthy);
        CHECK(second.sessionSlowDrivers == onlyWedged);
        CHECK(rowsOf(second) == healthy);
        CHECK(second.elapsedMs < 1500u);
        // A timeout is not a crash: the panel's "it crashed the device scan"
        // list stays empty, and the WHOLE-BUS scan still asks it.
        CHECK(cascade::source::sessionFaultedDrivers().empty());

        // A WHOLE-BUS SCAN THAT ANSWERS IN TIME clears it - every driver
        // answered that one - so the next scan beside a radio asks it again.
        EnumOptions whole = o;
        whole.skipDrivers.clear();
        const EnumResult bus = enumerateIsolated(whole);
        CHECK(bus.outcome == EnumOutcome::Ok);
        const EnumResult third = enumerateIsolated(o);
        CHECK(third.sweptDrivers == askedAll);
        CHECK(third.sessionSlowDrivers.empty());
        cascade::source::clearSessionFaultedDriversForTest();
    }
    {
        // THE SWEEP HAS ONE BUDGET, not one per driver. Three wedged drivers
        // used to cost three full budgets back to back (plus the listing):
        // here 1000 ms each, over three seconds of "Scanning...". The sweep is
        // now bounded by the ordinary scan's own worst case - attempts x
        // timeoutMs - and a driver it ran out of time for is named, not asked.
        cascade::source::clearSessionFaultedDriversForTest();
        setMode("manywedged");
        EnumOptions o;
        o.helperPath = self;
        o.allowInProcessFallback = false;
        o.timeoutMs = 1000;
        o.attempts = 2;  // the budget: 2 x 1000 ms
        o.skipDrivers = {"rtlsdr"};
        const EnumResult r = enumerateIsolated(o);
        std::printf("many wedged: %lu ms, budget 2000\n", r.elapsedMs);
        const std::vector<std::string> asked{"w1", "w2"};
        const std::vector<std::string> outOfTime{"w3", "good"};
        CHECK(r.sweptDrivers == asked);
        CHECK(r.outOfTimeDrivers == outOfTime);
        CHECK(r.faultedDrivers == asked);
        // 2000 ms of budget plus the kill and spawn overheads; three full
        // budgets would be over 3000.
        CHECK(r.elapsedMs < 2800u);
        // Only a driver that had its WHOLE budget and still did not answer is
        // held back next time; w2 was cut short by the sweep's deadline.
        const EnumResult next = enumerateIsolated(o);
        const std::vector<std::string> slow{"w1"};
        CHECK(next.sessionSlowDrivers == slow);
        CHECK(!next.sweptDrivers.empty() && next.sweptDrivers.front() == "w2");
        cascade::source::clearSessionFaultedDriversForTest();
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

        // THE EXPECTED CODE, and it is genuinely platform-specific rather than
        // a Windows literal left unported: 0xC0000005 is the NTSTATUS for an
        // access violation, which has no meaning outside Windows. The POSIX
        // side of runOneChild (soapy_enum_proc.cpp) reads a signal death off
        // WIFSIGNALED/WTERMSIG and reports it the way every POSIX shell
        // already reports one - 128 + the signal number - so a SIGSEGV here
        // is 139, not an invented substitute for the Windows number.
#ifdef _WIN32
        constexpr unsigned long kExpectedFaultCode = 0xC0000005ul;
#else
        constexpr unsigned long kExpectedFaultCode = 128ul + SIGSEGV;
#endif
        constexpr int kFaults = 200;
        int died = 0;
        int rightCode = 0;
        int leakedDevices = 0;
        for (int i = 0; i < kFaults; ++i) {
            const EnumResult r = enumerateIsolated(o);
            if (r.outcome == EnumOutcome::ChildDied) { ++died; }
            if (r.exitCode == kExpectedFaultCode) { ++rightCode; }
            if (!r.devices.empty()) { ++leakedDevices; }
        }
        std::printf("injected child faults: %d/%d reported as child-died, %d/%d with 0x%08lX\n",
                    died, kFaults, rightCode, kFaults, kExpectedFaultCode);
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
        // It waited for the budget, less exactly one tick of the kernel wait
        // clock - see waitClockTickMs for why that slack is not a loosening.
        const unsigned long floorMs = 1500u - waitClockTickMs();
        std::printf("timed-out scan: elapsed %lu ms, floor %lu ms (budget 1500, wait tick %lu)\n",
                    r.elapsedMs, floorMs, waitClockTickMs());
        CHECK(r.elapsedMs >= floorMs);
        // ...and then acted, rather than waiting out the helper's own 60 s.
        CHECK(r.elapsedMs < 20000u);
        // Killed by us, rather than having exited on its own. Windows carries
        // a custom marker in the exit code because TerminateProcess lets the
        // caller choose one; POSIX's kill has no such parameter, so runOneChild
        // reports the actual mechanism instead - a SIGKILL death reads back as
        // 128+9 by the same WIFSIGNALED convention as the injected-fault block
        // above.
#ifdef _WIN32
        CHECK(r.exitCode == 0xE0454E55ul);
#else
        CHECK(r.exitCode == 128ul + SIGKILL);
#endif
        // NOT retried: a timeout has already cost the full budget.
        CHECK(r.attempts == 1);
        // AND THE WEDGED PROBE IS NAMED: its begin line reached the parent and
        // no end line ever did.
        const std::vector<std::string> wantWedged{"wedged"};
        CHECK(r.inFlightDrivers == wantWedged);
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
        // Armed on both platforms since the Linux engine
        // (core/crash_handler_posix.cpp) landed in 0.97.0.
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
            // Filed on both platforms: reportAbsorbedChildFault() has a Linux
            // writer since 0.97.0, and it records the child's exit code in the
            // same field.
            CHECK(crashReports(dir).size() == 1);
            CHECK(body.find("enumeration child process died") != std::string::npos);
            CHECK(body.find("code: 0x00000007") != std::string::npos);

            std::filesystem::remove(counter, ec);
            setEnvVar(kCounterVar, "");
            // A WHOLE-BUS DEATH BLAMES NOBODY. Every driver probes at once in
            // that walk, so a death there - here one the retry recovered from -
            // must not put any driver on the session's do-not-ask list.
            CHECK(cascade::source::sessionFaultedDrivers().empty());
        }

        // --- FIELD REPORT 91965660116CF497: THE DEATH NAMES ITS DRIVER -------
        //
        // 0.99.31 on Windows 10.0.28000: a whole-bus enumeration child died
        // with 0xC0000374 (heap corruption) while the SDRplay service was not
        // answering. The report named nothing, and it landed in a hex group
        // shared with every other child death of that exit code, because the
        // signature was hashed from (code, "?", 0). Worse, the per-driver
        // report that DOES name the driver hashed to the same signature and so
        // was dropped by the uploader as a 24-hour duplicate of the whole-bus
        // one filed seconds before it.
        //
        // Pinned here: each report carries its own signature (per driver, and
        // one for the whole bus), the whole-bus report lists the probes still
        // running when it died, the faulting driver is remembered for the
        // session and never asked again - not by the next whole-bus child, and
        // not by a scan beside an open radio - and the healthy driver keeps
        // being asked throughout.
        {
            const auto clearReports = [&dir]() {
                std::error_code rec;
                for (const auto& p : crashReports(dir)) { std::filesystem::remove(p, rec); }
            };
            clearReports();
            cascade::source::clearSessionFaultedDriversForTest();
            setMode("heapdriver");
            EnumOptions o;
            o.helperPath = self;
            o.allowInProcessFallback = false;
            const Rows wantRows{{"good radio", "driver=good,serial=9"}};
            const std::vector<std::string> wantSdrplay{"sdrplay"};
            const std::vector<std::string> wantGood{"good"};

#ifdef _WIN32
            // THE FIELD SIGNATURE, reproduced from its inputs: this is exactly
            // what every contained 0xC0000374 child death hashed to before.
            CHECK(cascade::core::crashSignature(0xC0000374ul, "?", 0) == "91965660116CF497");
#endif
            const std::string oldSig = cascade::core::crashSignature(kHeapCorruptionExit, "?", 0);
            const std::string wholeTag = cascade::source::childFaultSignatureTag("");
            const std::string driverTag = cascade::source::childFaultSignatureTag("sdrplay");
            const std::string wholeSig =
                cascade::core::crashSignature(kHeapCorruptionExit, wholeTag.c_str(), 0);
            const std::string driverSig =
                cascade::core::crashSignature(kHeapCorruptionExit, driverTag.c_str(), 0);
            CHECK(wholeSig != oldSig);
            CHECK(driverSig != oldSig);
            CHECK(driverSig != wholeSig);
            CHECK(driverTag.find("sdrplay") != std::string::npos);
            // Another driver, another group.
            CHECK(cascade::source::childFaultSignatureTag("uhd") != driverTag);
            // A driver name is third-party text, and a report is "name: value"
            // lines: one newline in a registry key must not split the reason
            // and forge a field. Case is folded, so one driver is one group.
            CHECK(cascade::source::childFaultSignatureTag("Evil\nkind: hang") ==
                  "enumerate-child:driver=evil?kind??hang");
            CHECK(cascade::source::childFaultSignatureTag("SDRplay") == driverTag);

            // FIRST SCAN: both whole-bus children die, the sweep asks each
            // driver alone, "good" answers and "sdrplay" dies again.
            const EnumResult r = enumerateIsolated(o);
            std::printf("heap-corruption driver: outcome=%s attempts=%d deaths=%d faulted=%zu "
                        "inflight=%zu reports=%zu\n",
                        enumOutcomeName(r.outcome), r.attempts, r.childDeaths,
                        r.faultedDrivers.size(), r.inFlightDrivers.size(),
                        crashReports(dir).size());
            CHECK(r.outcome == EnumOutcome::Ok);
            CHECK(rowsOf(r) == wantRows);
            CHECK(r.attempts == 2);
            CHECK(r.childDeaths == 3);
            CHECK(r.deathExitCode == kHeapCorruptionExit);
            CHECK(r.faultedDrivers == wantSdrplay);
            // The whole-bus child's probe log: good finished, sdrplay did not.
            CHECK(r.inFlightDrivers == wantSdrplay);

            const std::string body = allReportText(dir);
            CHECK(crashReports(dir).size() == 3u);  // two whole-bus, one per driver
            CHECK(body.find("died probing driver=sdrplay") != std::string::npos);
            CHECK(body.find("still probing when it died: sdrplay") != std::string::npos);
            CHECK(body.find("signature: " + driverSig) != std::string::npos);
            CHECK(body.find("signature: " + wholeSig) != std::string::npos);
            CHECK(body.find("signature: " + oldSig) == std::string::npos);

            const auto session = cascade::source::sessionFaultedDrivers();
            CHECK(session.size() == 1u);
            for (const auto& f : session) {
                CHECK(f.driver == "sdrplay");
                CHECK(f.exitCode == kHeapCorruptionExit);
            }

            // RESCAN: one whole-bus child, told to leave sdrplay out, and no
            // death at all - the Refresh button no longer costs a crash.
            clearReports();
            const EnumResult again = enumerateIsolated(o);
            CHECK(again.outcome == EnumOutcome::Ok);
            CHECK(rowsOf(again) == wantRows);
            CHECK(again.attempts == 1);
            CHECK(again.childDeaths == 0);
            CHECK(!again.sweptPerDriver);
            CHECK(again.sessionSkippedDrivers == wantSdrplay);
            CHECK(crashReports(dir).empty());

            // BESIDE AN OPEN RADIO the per-driver walk leaves it out too.
            EnumOptions beside = o;
            beside.skipDrivers = {"someotherfamily"};
            const EnumResult b = enumerateIsolated(beside);
            CHECK(b.outcome == EnumOutcome::Ok);
            CHECK(rowsOf(b) == wantRows);
            CHECK(b.childDeaths == 0);
            CHECK(b.sweptDrivers == wantGood);
            CHECK(b.sessionSkippedDrivers == wantSdrplay);
            CHECK(crashReports(dir).empty());

            // A DEATH IN THE SCAN BESIDE AN OPEN RADIO IS REMEMBERED TOO: a
            // fresh session, "good" left out as the open radio's family, so
            // sdrplay is asked alone - and dies, and is then not asked again.
            cascade::source::clearSessionFaultedDriversForTest();
            clearReports();
            EnumOptions besideGood = o;
            besideGood.skipDrivers = {"good"};
            const EnumResult c = enumerateIsolated(besideGood);
            CHECK(c.faultedDrivers == wantSdrplay);
            CHECK(c.childDeaths == 1);
            CHECK(allReportText(dir).find("died probing driver=sdrplay") != std::string::npos);
            const EnumResult c2 = enumerateIsolated(besideGood);
            CHECK(c2.childDeaths == 0);
            CHECK(c2.sessionSkippedDrivers == wantSdrplay);

            cascade::source::clearSessionFaultedDriversForTest();
            clearReports();
        }

        // --- F204602B5329B268: THE PARENT SAYS WHAT THE CHILD DIED OF --------
        //
        // The child probing uhd died with 0xE0000002 - its own handler could
        // not finish - and the parent's report could name nothing but that
        // code. The child's handler now writes one line to the pipe BEFORE its
        // report (armEnumerateHelperProcess sets faultLineToStdout), and the
        // parent's per-driver report carries it.
        //
        // Two children, beside an "open radio" so the per-driver report is the
        // one at issue:
        //   armeduhd      the production arming and a real fault: the child's
        //                 own handler runs and writes the report, the parent
        //                 files NOTHING for the same death (2026-10-04), and
        //                 the report says which driver and which attempt in
        //                 its reason line;
        //   fieldlineuhd  the field's line on the pipe and a death the handler
        //                 never saw: the parent's per-driver report is the
        //                 only one, and carries the child's words.
        {
            const auto clearReports = [&dir]() {
                std::error_code rec;
                for (const auto& p : crashReports(dir)) { std::filesystem::remove(p, rec); }
            };
            const std::vector<std::string> wantUhd{"uhd"};

            clearReports();
            cascade::source::clearSessionFaultedDriversForTest();
            setMode("armeduhd");
            EnumOptions o;
            o.helperPath = self;
            o.allowInProcessFallback = false;
            o.skipDrivers = {"sdrplay"};
            const EnumResult r = enumerateIsolated(o);
            std::printf("armed uhd child: outcome=%s exit=0x%08lX line=[%s] reports=%zu\n",
                        enumOutcomeName(r.outcome), r.deathExitCode, r.childFaultLine.c_str(),
                        crashReports(dir).size());
            CHECK(r.faultedDrivers == wantUhd);
            CHECK(r.childFaultLine.rfind("access violation", 0) == 0);
            CHECK(r.childFaultLine.find(" at ") != std::string::npos);
            const std::string body = allReportText(dir);
            // ONE REPORT FOR THE ONE DEATH: the child's own, with its stack.
            CHECK(crashReports(dir).size() == 1u);
            CHECK(reportHasStack(body));
            CHECK(body.find("child-exit-code:") == std::string::npos);
            // ...and it says what only the parent's report used to: which
            // driver, which attempt, and that the application survived.
            const std::string armedReason = reasonLines(body);
            CHECK(armedReason.find("reason: access violation - enumeration child, driver=uhd, "
                                   "attempt 1 (contained)") != std::string::npos);
            // THE SITE KEEPS 200 CHARACTERS OF A REASON (crash.go clip).
            const std::size_t armedLen =
                armedReason.size() > std::strlen("reason: ") + 1
                    ? armedReason.size() - 1 - std::strlen("reason: ")
                    : 0u;
            std::printf("armed per-driver reason: %zu characters\n", armedLen);
            CHECK(armedLen > 0u && armedLen <= 200u);
            cascade::source::clearSessionFaultedDriversForTest();
            clearReports();

            setMode("fieldlineuhd");
            const EnumResult f = enumerateIsolated(o);
            const std::string fbody = allReportText(dir);
            CHECK(f.faultedDrivers == wantUhd);
            CHECK(f.childFaultLine.rfind("access violation", 0) == 0);
            CHECK(crashReports(dir).size() == 1u);  // the parent's, alone
            CHECK(!reportHasStack(fbody));
            CHECK(fbody.find("died probing driver=uhd (contained: every other driver was still "
                             "probed) - child: access violation") != std::string::npos);
            // THE SITE KEEPS 200 CHARACTERS OF A REASON (crash.go clip), and
            // the driver's name is in the half that must survive.
            const std::size_t at = fbody.find("reason: SDR device enumeration child process died "
                                              "probing driver=uhd");
            CHECK(at != std::string::npos);
            if (at != std::string::npos) {
                const std::size_t eol = fbody.find('\n', at);
                const std::size_t len = (eol == std::string::npos ? fbody.size() : eol) - at -
                                        std::strlen("reason: ");
                std::printf("per-driver reason: %zu characters\n", len);
                CHECK(len <= 200u);
            }
            cascade::source::clearSessionFaultedDriversForTest();
            clearReports();
        }

        // --- THE WHOLE-BUS REASON FITS THE SITE'S 200 CHARACTERS ------------
        //
        // Review of 5e7b968: the whole-bus reason put the child's line BEFORE
        // "still probing when it died: ...", and at 237-244 characters the
        // site's clip (crash.go, clip(in.Reason, 200)) cut the driver list -
        // the one thing that reason exists to carry. Both the production
        // handler's line and the field's own libusb line, with the field
        // machine's drivers still probing: every parent reason within 200,
        // the driver list whole, and the child's words kept where they fit.
        //
        // Since 2026-10-04 only the "fieldline" child - a death its handler
        // never saw - leaves the parent a report to write. "armedwholebus" runs
        // the production handler, so its two deaths are two reports of the
        // CHILDREN'S, and what is held to the 200 characters is the reason
        // those carry, with the tail that names the walk and the attempt.
        for (const char* m : {"armedwholebus", "fieldline"}) {
            const auto clearReports = [&dir]() {
                std::error_code rec;
                for (const auto& p : crashReports(dir)) { std::filesystem::remove(p, rec); }
            };
            clearReports();
            cascade::source::clearSessionFaultedDriversForTest();
            setMode(m);
            EnumOptions o;
            o.helperPath = self;
            o.allowInProcessFallback = false;
            const EnumResult r = enumerateIsolated(o);
            CHECK(r.outcome == EnumOutcome::ChildDied);
            CHECK(r.childDeaths == 2);
            CHECK(r.childFaultLine.rfind("access violation", 0) == 0);
            const std::string body = allReportText(dir);
            if (std::strcmp(m, "armedwholebus") == 0) {
                // THE PRODUCTION HANDLER RAN IN BOTH CHILDREN (2026-10-04): two
                // deaths, two reports - the children's own, each with its
                // stack - and none from the parent, whose reasons below are
                // therefore only for the child that never wrote one. Each says
                // which walk and which attempt in a reason that fits the
                // site's 200 characters.
                const std::vector<std::string> texts = reportTexts(dir);
                int withStack = 0;
                int attempt1 = 0;
                int attempt2 = 0;
                for (const std::string& t : texts) {
                    if (reportHasStack(t)) { ++withStack; }
                    const std::string rl = reasonLines(t);
                    CHECK(rl.size() > std::strlen("reason: ") + 1 &&
                          rl.size() - 1 - std::strlen("reason: ") <= 200u);
                    CHECK(rl.find("access violation - enumeration child, whole bus, attempt ") !=
                          std::string::npos);
                    CHECK(rl.find(" (contained)") != std::string::npos);
                    if (rl.find("attempt 1 ") != std::string::npos) { ++attempt1; }
                    if (rl.find("attempt 2 ") != std::string::npos) { ++attempt2; }
                }
                std::printf("armedwholebus: reports=%zu with-stack=%d attempt1=%d attempt2=%d\n",
                            texts.size(), withStack, attempt1, attempt2);
                CHECK(texts.size() == 2u);
                CHECK(withStack == 2);
                CHECK(attempt1 == 1);
                CHECK(attempt2 == 1);
                CHECK(body.find("child-exit-code:") == std::string::npos);
                cascade::source::clearSessionFaultedDriversForTest();
                clearReports();
                continue;
            }
            std::size_t pos = 0;
            int wholeBus = 0;
            while ((pos = body.find("reason: SDR device enumeration child process died", pos)) !=
                   std::string::npos) {
                const std::size_t eol = body.find('\n', pos);
                const std::string reason =
                    body.substr(pos + std::strlen("reason: "),
                                (eol == std::string::npos ? body.size() : eol) - pos -
                                    std::strlen("reason: "));
                pos = (eol == std::string::npos) ? body.size() : eol;
                ++wholeBus;
                std::printf("%s whole-bus reason (%zu chars): %s\n", m, reason.size(),
                            reason.c_str());
                CHECK(reason.size() <= 200u);
                CHECK(reason.find("still probing when it died: sdrplay, uhd") !=
                      std::string::npos);
                CHECK(reason.find(" - child: access violation") != std::string::npos);
                // The drivers first: only the child's words are ever cut.
                CHECK(reason.find("still probing when it died") < reason.find(" - child: "));
                if (std::strcmp(m, "fieldline") == 0) {
                    CHECK(reason.find("libusb-1.0.dll+0x10490") != std::string::npos);
                }
            }
            CHECK(wholeBus == 2);
            cascade::source::clearSessionFaultedDriversForTest();
            clearReports();
        }

        // --- 2026-10-01: A DEATH AFTER EVERY PROBE HAD ENDED ----------------
        //
        // The field child (crash 3C2F1A0F27A8FD35) died in a vendor module's
        // DLL detach as it EXITED, its answer already written - and the parent
        // threw the answer away, re-probed, swept, and filed a reason saying
        // "no driver's probe had begun (it died while the driver modules were
        // loading)", the opposite of what happened. A complete answer is now
        // used, the death is one report that says when it happened, and a
        // death after the walk but before the answer says that instead.
        {
            const auto clearReports = [&dir]() {
                std::error_code rec;
                for (const auto& p : crashReports(dir)) { std::filesystem::remove(p, rec); }
            };
#ifdef _WIN32
            constexpr unsigned long kAvExit = 0xC0000005ul;
#else
            constexpr unsigned long kAvExit = 139ul;
#endif
            clearReports();
            cascade::source::clearSessionFaultedDriversForTest();
            setMode("answeredthendied");
            EnumOptions o;
            o.helperPath = self;
            o.allowInProcessFallback = false;
            const EnumResult r = enumerateIsolated(o);
            const std::string body = allReportText(dir);
            std::printf("answered then died: outcome=%s attempts=%d sweep=%d deaths=%d reports=%zu\n",
                        enumOutcomeName(r.outcome), r.attempts, r.sweepChildren, r.childDeaths,
                        crashReports(dir).size());
            CHECK(r.outcome == EnumOutcome::Ok);
            CHECK(rowsOf(r) == expectedOkRows());  // the answer it wrote is the answer
            CHECK(r.attempts == 1);                // not re-probed...
            CHECK(r.sweepChildren == 0);           // ...and not swept
            CHECK(r.childDeaths == 1);             // but the death is not hidden
            CHECK(r.deathExitCode == kAvExit);
            CHECK(crashReports(dir).size() == 1u);  // one event, one report
            CHECK(reasonLines(body).find("after its answer was complete") != std::string::npos);
            CHECK(reasonLines(body).find("no driver's probe had begun") == std::string::npos);
            CHECK(cascade::source::sessionFaultedDrivers().empty());

            clearReports();
            setMode("probesdonethendied");
            EnumOptions once = o;
            once.attempts = 1;
            once.perDriverSweep = false;
            const EnumResult d = enumerateIsolated(once);
            const std::string body2 = allReportText(dir);
            CHECK(d.outcome == EnumOutcome::ChildDied);
            CHECK(d.inFlightDrivers.empty());
            CHECK(crashReports(dir).size() == 1u);
            CHECK(reasonLines(body2).find("every driver's probe had finished") != std::string::npos);
            CHECK(reasonLines(body2).find("no driver's probe had begun") == std::string::npos);
            cascade::source::clearSessionFaultedDriversForTest();
            clearReports();
        }

        // --- THE REAL CHILD AND A REAL VENDOR MODULE (2026-10-01) -----------
        //
        // The fault fixture (tests/fixtures/soapy_fault_module.cpp) on
        // SOAPY_SDR_PLUGIN_PATH, loaded by the REAL cascade.exe child through
        // SoapySDR's own loader. UHD is left out, as on a machine with no
        // USRP, so the bench's own UHD fault cannot stand in for the one
        // staged here.
#ifdef _WIN32
        {
            const auto clearReports = [&dir]() {
                std::error_code rec;
                for (const auto& p : crashReports(dir)) { std::filesystem::remove(p, rec); }
            };
            const std::string real = findRealCascade();
            CHECK(!real.empty());
            CHECK(!fixtureDir.empty());  // tests/CMakeLists.txt hands it over
            const auto fixtureRow = [](const EnumResult& e) {
                for (const auto& d : e.devices) {
                    if (d.label == "Fault fixture") { return true; }
                }
                return false;
            };
            // Whatever this environment already had, restored afterwards.
            const std::string pluginPathBefore = envOr("SOAPY_SDR_PLUGIN_PATH", "");
            setEnvVar("SOAPY_SDR_PLUGIN_PATH", fixtureDir.c_str());

            // A MODULE THAT FAULTS AS IT IS DETACHED AT EXIT costs nothing: the
            // child leaves without detaching anything once its answer is
            // written, so it exits 0, once, with the fixture's row, and files
            // nothing.
            clearReports();
            cascade::source::clearSessionFaultedDriversForTest();
            setEnvVar("FOXSDR_TEST_SOAPY_FIXTURE", "exit");
            EnumOptions o;
            o.helperPath = real;
            o.allowInProcessFallback = false;
            o.timeoutMs = 25000;
            o.absentDrivers = {"uhd"};
            const EnumResult r = enumerateIsolated(o);
            std::printf("exit-fault module: outcome=%s attempts=%d sweep=%d deaths=%d "
                        "deathExit=0x%08lX fixture=%d reports=%zu\n",
                        enumOutcomeName(r.outcome), r.attempts, r.sweepChildren, r.childDeaths,
                        r.deathExitCode, fixtureRow(r) ? 1 : 0, crashReports(dir).size());
            CHECK(r.outcome == EnumOutcome::Ok);
            CHECK(fixtureRow(r));
            CHECK(r.attempts == 1);
            CHECK(r.childDeaths == 0);
            CHECK(crashReports(dir).empty());

            // A MODULE THAT FAULTS IN ITS PROBE: the child's OWN report carries
            // the build's identity and names the module - before, it had no
            // version, os or arch (a separate, versionless crash group on the
            // site) and the fault resolved to "?", hashing to 650B88A1735695DB,
            // the very signature every contained child death had before 0.99.33.
            CHECK(cascade::core::crashSignature(0xC0000005ul, "?", 0) == "650B88A1735695DB");
            clearReports();
            setEnvVar("FOXSDR_TEST_SOAPY_FIXTURE", "find");
            EnumOptions once = o;
            once.attempts = 1;
            once.perDriverSweep = false;
            const EnumResult f = enumerateIsolated(once);
            std::string own;
            int ownCount = 0;
            for (const auto& p : crashReports(dir)) {
                const std::string text = readAll(p);
                if (text.find("child-exit-code:") == std::string::npos) {
                    own += text;
                    ++ownCount;
                }
            }
            std::printf("find-fault module: outcome=%s exit=0x%08lX inflight=%zu own reports=%d\n",
                        enumOutcomeName(f.outcome), f.exitCode, f.inFlightDrivers.size(),
                        ownCount);
            CHECK(f.outcome == EnumOutcome::ChildDied);
            CHECK(f.exitCode == 0xC0000005ul);
            // Among the probes still running: every driver probes at once.
            CHECK(std::find(f.inFlightDrivers.begin(), f.inFlightDrivers.end(), "faultfixture") !=
                  f.inFlightDrivers.end());
            CHECK(ownCount == 1);
            CHECK(own.find(std::string("version: ") + cascade::versionString() + "\n") !=
                  std::string::npos);
            CHECK(own.find(std::string("commit: ") + cascade::gitCommit() + "\n") !=
                  std::string::npos);
            CHECK(own.find("os: " + cascade::core::osDescription() + "\n") != std::string::npos);
            CHECK(own.find("arch: " + cascade::core::archDescription() + "\n") !=
                  std::string::npos);
            CHECK(own.find("address: soapy_fault_fixture.dll+0x") != std::string::npos);
            CHECK(own.find("signature: 650B88A1735695DB") == std::string::npos);
            // ...and which child it was, for a reader of that report alone.
            CHECK(own.find("enumeration helper: whole bus") != std::string::npos);

            // A MODULE THAT FAULTS IN A DLL IT MAPS DURING ITS PROBE (2026-10-04):
            // the field child's actual shape - SoapyAudio's RtAudio maps an ASIO
            // driver in the middle of its find function, long after the child
            // refreshed its crash handler's module table for the modules it
            // loaded up front (setModulesLoadedHook), so the 0.99.57 report
            // listed the faulting frames as bare addresses and named nothing.
            // The DLL is in no table the child ever built; the report must name
            // it anyway - in its address, its module list and the line the
            // child writes to the parent's pipe.
            CHECK(!lateDll.empty());  // tests/CMakeLists.txt hands it over
            clearReports();
            cascade::source::clearSessionFaultedDriversForTest();
            setEnvVar("FOXSDR_TEST_SOAPY_FIXTURE", "late");
            setEnvVar("FOXSDR_TEST_SOAPY_LATE_DLL", lateDll.c_str());
            const EnumResult l = enumerateIsolated(once);
            std::string lateOwn;
            int lateOwnCount = 0;
            for (const auto& p : crashReports(dir)) {
                const std::string text = readAll(p);
                if (text.find("child-exit-code:") == std::string::npos) {
                    lateOwn += text;
                    ++lateOwnCount;
                }
            }
            std::printf("late-dll fault: outcome=%s exit=0x%08lX own reports=%d childSaid=\"%s\"\n",
                        enumOutcomeName(l.outcome), l.exitCode, lateOwnCount,
                        l.childFaultLine.c_str());
            CHECK(l.outcome == EnumOutcome::ChildDied);
            CHECK(l.exitCode == 0xC0000005ul);
            CHECK(lateOwnCount == 1);
            CHECK(lateOwn.find(std::string("version: ") + cascade::versionString() + "\n") !=
                  std::string::npos);
            CHECK(lateOwn.find(std::string("commit: ") + cascade::gitCommit() + "\n") !=
                  std::string::npos);
            CHECK(lateOwn.find("address: late_fault_fixture.dll+0x") != std::string::npos);
            CHECK(lateOwn.find("  late_fault_fixture.dll base=0x") != std::string::npos);
            CHECK(lateOwn.find("signature: 650B88A1735695DB") == std::string::npos);
            CHECK(l.childFaultLine.find("late_fault_fixture.dll+0x") != std::string::npos);
            setEnvVar("FOXSDR_TEST_SOAPY_LATE_DLL", "");

            // THE SDRPLAY API IS CLOSED BEFORE THE CHILD ENDS, and nothing else
            // is detached. SoapySDRPlay3 opens the SDRplay API (sdrplay_api_Open)
            // in a function-local singleton the first time its find function
            // runs, and sdrplay_api_Close runs only in that singleton's
            // destructor - at module unload. A child that ends by
            // TerminateProcess (endEnumerateHelperProcess, so no other vendor
            // module's detach can kill it after it answered) would leave the
            // SDRplay API service with a client that vanished without the Close
            // the API's specification requires to be the last call. The
            // "sdrplay" fixture build records its session's destruction and its
            // detach; the "faultfixture" build records the same, and must
            // record nothing: it is not unloaded.
            {
                std::error_code uec;
                const std::filesystem::path unloads =
                    std::filesystem::temp_directory_path(uec) /
                    ("foxsdr_fixture_unloads_" + std::to_string(::GetCurrentProcessId()) + ".txt");
                std::filesystem::remove(unloads, uec);
                setEnvVar("FOXSDR_TEST_SOAPY_FIXTURE", "");
                setEnvVar("FOXSDR_TEST_SOAPY_FIXTURE_UNLOADS", unloads.string().c_str());
                clearReports();
                cascade::source::clearSessionFaultedDriversForTest();
                const EnumResult c = enumerateIsolated(o);
                const std::string said = readAll(unloads);
                bool sdrplayRow = false;
                for (const auto& d : c.devices) {
                    if (d.label == "SDRplay fixture") { sdrplayRow = true; }
                }
                std::printf("sdrplay close at exit: outcome=%s deaths=%d sdrplay row=%d "
                            "recorded=\"%s\"\n",
                            enumOutcomeName(c.outcome), c.childDeaths, sdrplayRow ? 1 : 0,
                            said.c_str());
                CHECK(c.outcome == EnumOutcome::Ok);
                CHECK(c.childDeaths == 0);
                CHECK(fixtureRow(c));
                CHECK(sdrplayRow);  // its find ran, so its API session was opened
                CHECK(said.find("sdrplay closed") != std::string::npos);
                CHECK(said.find("sdrplay detached") != std::string::npos);
                CHECK(said.find("faultfixture") == std::string::npos);
                CHECK(crashReports(dir).empty());
                setEnvVar("FOXSDR_TEST_SOAPY_FIXTURE_UNLOADS", "");
                std::filesystem::remove(unloads, uec);
            }

            setEnvVar("FOXSDR_TEST_SOAPY_FIXTURE", "");
            setEnvVar("SOAPY_SDR_PLUGIN_PATH", pluginPathBefore.c_str());
            cascade::source::clearSessionFaultedDriversForTest();
            clearReports();
        }
#endif  // _WIN32

        // --- 2026-10-04: ONE REPORT PER CHILD DEATH -------------------------
        //
        // A child that dies leaves up to three reports for the one death: its
        // own, written by the handler it armed (it has the stack and the
        // module list), the parent's whole-bus report of it, and - after a
        // second whole-bus death - the parent's per-driver one from the
        // sweep, which has no stack at all. One user's deterministic ASIO
        // fault produced six files in one scan and, after the client's
        // 24-hour de-duplication, three uploads: three of the five a day a
        // machine is allowed, for a fault the application survived. The
        // parent now files its own report only for a death whose child left
        // none: heap corruption, a vendor TerminateProcess, a child that died
        // before it armed.
        //
        // (c) FIRST, because it is the half that must NOT change: the deaths
        // the child's handler never sees keep the parent's stackless report,
        // once - and a report the child wrote for something else (an
        // absorbed vendor fault, which it carried on from) is not the report
        // of the death. Four fake children, on every platform.
        {
            const auto clearReports = [&dir]() {
                std::error_code rec;
                for (const auto& p : crashReports(dir)) { std::filesystem::remove(p, rec); }
            };
            cascade::source::clearSessionFaultedDriversForTest();
            EnumOptions once;
            once.helperPath = self;
            once.allowInProcessFallback = false;
            once.attempts = 1;
            once.perDriverSweep = false;

            // ARMED, THEN DIES UNSEEN: the child's handler never runs, so the
            // child has no report and the parent's is the only record.
            clearReports();
            setMode("armedterminate");
            const EnumResult u = enumerateIsolated(once);
            const std::string ut = allReportText(dir);
            std::printf("armed child dies unseen: outcome=%s exit=0x%08lX reports=%zu\n",
                        enumOutcomeName(u.outcome), u.deathExitCode, crashReports(dir).size());
            CHECK(u.outcome == EnumOutcome::ChildDied);
            CHECK(u.deathExitCode == kHeapCorruptionExit);
            CHECK(crashReports(dir).size() == 1u);
            CHECK(!reportHasStack(ut));
            CHECK(ut.find("the fault was in a child process") != std::string::npos);
            CHECK(ut.find("child-exit-code: 0x") != std::string::npos);
            CHECK(reasonLines(ut).find("enumeration child process died") != std::string::npos);

            // ARMED, FILES AN ABSORBED FAULT, THEN DIES UNSEEN: two files, and
            // the second is the parent's. The absorbed one carries this
            // child's process id as well, and is not the death's report.
            clearReports();
            setMode("armedabsorbed");
            const EnumResult ab = enumerateIsolated(once);
            const std::vector<std::string> abTexts = reportTexts(dir);
            int absorbedFiles = 0;
            int parentFiles = 0;
            for (const std::string& t : abTexts) {
                if (t.find("absorbed by the vendor-call guard") != std::string::npos) {
                    ++absorbedFiles;
                }
                if (t.find("child-exit-code: 0x") != std::string::npos) { ++parentFiles; }
            }
            std::printf("armed child absorbs then dies unseen: outcome=%s reports=%zu "
                        "absorbed=%d parent=%d\n",
                        enumOutcomeName(ab.outcome), abTexts.size(), absorbedFiles, parentFiles);
            CHECK(ab.outcome == EnumOutcome::ChildDied);
            CHECK(abTexts.size() == 2u);
            CHECK(absorbedFiles == 1);
            CHECK(parentFiles == 1);

            // DEAD BEFORE IT ARMED (a bare exit code): the parent's report,
            // once.
            clearReports();
            setMode("die");
            const EnumResult dd = enumerateIsolated(once);
            const std::string ddText = allReportText(dir);
            CHECK(dd.outcome == EnumOutcome::ChildDied);
            CHECK(crashReports(dir).size() == 1u);
            CHECK(!reportHasStack(ddText));
            CHECK(ddText.find("code: 0x00000007") != std::string::npos);

            // THE TIMEOUT KILL files nothing, from either process, and did not
            // before: only a ChildDied outcome is ever reported, a wedged
            // probe is logged and counted. Pinned so the guard's arrival is
            // not read as a change to it.
            clearReports();
            setMode("hang");
            EnumOptions slow = once;
            slow.timeoutMs = 1500;
            const EnumResult hh = enumerateIsolated(slow);
            CHECK(hh.outcome == EnumOutcome::ChildTimedOut);
            CHECK(crashReports(dir).empty());

            setMode("");
            cascade::source::clearSessionFaultedDriversForTest();
            clearReports();
        }

#ifdef _WIN32
        // TWO DRIVERS THAT FAULT IN NO MODULE: the signature of a fault in
        // private memory has no module to hash, and every such fault shared
        // the one "?" signature - so two drivers' faults were one upload a day.
        // The parent's own per-driver report used to keep them apart (its tag);
        // now that the child's report covers the death, the child hashes the
        // same tag when it has no module to hash. Four deaths (two whole-bus,
        // then each driver alone), four reports, and three signatures: the
        // whole bus's, "adrv"'s and "bdrv"'s.
        {
            const auto clearReports = [&dir]() {
                std::error_code rec;
                for (const auto& p : crashReports(dir)) { std::filesystem::remove(p, rec); }
            };
            clearReports();
            cascade::source::clearSessionFaultedDriversForTest();
            setMode("unresolved");
            EnumOptions o;
            o.helperPath = self;
            o.allowInProcessFallback = false;
            const EnumResult r = enumerateIsolated(o);
            const std::vector<std::string> texts = reportTexts(dir);
            const auto sigWhere = [&texts](const char* needle) {
                for (const std::string& t : texts) {
                    if (reasonLines(t).find(needle) != std::string::npos) { return signatureOf(t); }
                }
                return std::string();
            };
            const std::string sigWhole = sigWhere("whole bus");
            const std::string sigA = sigWhere("driver=adrv");
            const std::string sigB = sigWhere("driver=bdrv");
            const std::string shared =
                cascade::core::crashSignature(0xC000001Dul, "?", 0);  // illegal instruction
            int withStack = 0;
            for (const std::string& t : texts) {
                if (reportHasStack(t)) { ++withStack; }
            }
            std::printf("unresolved faults: deaths=%d reports=%zu with-stack=%d uploads=%d "
                        "whole=%s a=%s b=%s\n",
                        r.childDeaths, texts.size(), withStack, wouldSend(texts),
                        sigWhole.c_str(), sigA.c_str(), sigB.c_str());
            CHECK(r.childDeaths == 4);
            CHECK(texts.size() == 4u);
            CHECK(withStack == 4);
            CHECK(!sigWhole.empty() && !sigA.empty() && !sigB.empty());
            CHECK(sigA != sigB);       // two drivers, two groups
            CHECK(sigA != sigWhole);
            CHECK(sigB != sigWhole);
            CHECK(sigWhole != shared);  // and not the "?" every one of them shared
            CHECK(sigA != shared && sigB != shared);
            CHECK(wouldSend(texts) == 3);
            cascade::source::clearSessionFaultedDriversForTest();
            setMode("");
            clearReports();
        }
#endif

        // --- THE QUESTION THE PARENT ASKS, ON ITS OWN -----------------------
        //
        // core::crashReportWrittenByProcess: "did the handler of process P
        // write a report of its own death into this directory". Every way the
        // answer could be wrong is a file in a directory below, because a wrong
        // yes silently loses a death's only record and a wrong no is the second
        // upload this change removes.
        {
            namespace fs = std::filesystem;
            using std::chrono::hours;
            using std::chrono::seconds;
            using std::chrono::system_clock;
            std::error_code pec;
            const fs::path pd = std::filesystem::temp_directory_path(pec) /
                                ("enum_pidreports_" + std::to_string(currentPid()));
            fs::remove_all(pd, pec);
            fs::create_directories(pd, pec);
            const auto put = [&pd](const std::string& name, const std::string& text) {
                std::ofstream(pd / name, std::ios::binary) << text;
            };
            const std::string whole =
                "kind: crash\nreason: access violation\ncode: 0xC0000005\n"
                "address: x.dll+0x10\nsignature: 0123456789ABCDEF\nthread: 1\n--- context ---\n";
            const std::string absorbed =
                "kind: crash\nreason: fault in a third-party SDR module, absorbed by the "
                "vendor-call guard (the process continued; the call reported failure to its "
                "caller)\ncode: 0xC0000005\naddress: x.dll+0x10\nsignature: 0123456789ABCDEF\n"
                "thread: 1\n--- context ---\n";
            const auto d = pd.string();
            const auto since = system_clock::now() - seconds(60);

            // A whole report, Windows-style and Linux-style names.
            put("crash-20261004-120000-4242-1.txt", whole);
            put("crash-1759593600-7777-3.txt", whole);
            CHECK(cascade::core::crashReportWrittenByProcess(d, 4242, since));
            CHECK(cascade::core::crashReportWrittenByProcess(d, 7777, since));
            // The process id is matched whole: not a prefix, not a suffix.
            CHECK(!cascade::core::crashReportWrittenByProcess(d, 424, since));
            CHECK(!cascade::core::crashReportWrittenByProcess(d, 42420, since));
            CHECK(!cascade::core::crashReportWrittenByProcess(d, 242, since));
            CHECK(!cascade::core::crashReportWrittenByProcess(d, 1, since));
            CHECK(!cascade::core::crashReportWrittenByProcess(d, 0, since));
            CHECK(!cascade::core::crashReportWrittenByProcess("", 4242, since));
            CHECK(!cascade::core::crashReportWrittenByProcess((pd / "nope").string(), 4242, since));

            // A STALE report of an unrelated process that held this number
            // once: older than the spawn, so it is not the death's.
            put("crash-20250101-120000-9000-1.txt", whole);
            fs::last_write_time(pd / "crash-20250101-120000-9000-1.txt",
                                fs::file_time_type::clock::now() - hours(2), pec);
            CHECK(!cascade::core::crashReportWrittenByProcess(d, 9000, since));
            CHECK(cascade::core::crashReportWrittenByProcess(d, 9000,
                                                              system_clock::now() - hours(3)));
            // ...and a fresh report is not "stale" merely because the spawn
            // time asked about is in the future.
            CHECK(!cascade::core::crashReportWrittenByProcess(d, 4242,
                                                               system_clock::now() + hours(1)));

            // NOT A REPORT: created and never written to; killed after the
            // kind line; a signature that is not sixteen hex digits.
            put("crash-20261004-120000-6000-1.txt", "");
            put("crash-20261004-120000-6001-1.txt", "kind: crash\nreason: access violation\n");
            put("crash-20261004-120000-6002-1.txt",
                "kind: crash\nreason: access violation\ncode: 0x1\naddress: x\nsignature: 0123\n");
            put("crash-20261004-120000-6003-1.txt",
                "kind: crash\nreason: access violation\ncode: 0x1\naddress: x\n"
                "signature: 0123456789ABCDEG\n");
            put("crash-20261004-120000-6004-1.txt", "kind: hang\nsignature: 0123456789ABCDEF\n");
            for (const unsigned long p : {6000ul, 6001ul, 6002ul, 6003ul, 6004ul}) {
                CHECK(!cascade::core::crashReportWrittenByProcess(d, p, since));
            }

            // THE SAME PROCESS ABSORBED A FAULT EARLIER: that file is not the
            // report of a death, so a child that then died unseen still needs
            // the parent's - but a child that ALSO wrote a fatal report has one.
            put("crash-20261004-120000-5000-1.txt", absorbed);
            CHECK(!cascade::core::crashReportWrittenByProcess(d, 5000, since));
            put("crash-20261004-120001-5000-2.txt", whole);
            CHECK(cascade::core::crashReportWrittenByProcess(d, 5000, since));

            // NOT A CRASH REPORT'S NAME: a freeze report, a dump, a sidecar.
            put("hang-20261004-120000-8000-1.txt", whole);
            put("crash-20261004-120000-8100-1.dmp", whole);
            put("crash-20261004-120000-8200-1.txt.upload", whole);
            put("crash-20261004-120000-x-1.txt", whole);
            for (const unsigned long p : {8000ul, 8100ul, 8200ul}) {
                CHECK(!cascade::core::crashReportWrittenByProcess(d, p, since));
            }
            fs::remove_all(pd, pec);
        }

        // --- WHAT THE CHILD'S OWN REPORT SAYS ABOUT THE CHILD ----------------
        {
            using cascade::source::enumerateChildReasonSuffix;
            CHECK(enumerateChildReasonSuffix(false, nullptr, 1) ==
                  " - enumeration child, whole bus, attempt 1 (contained)");
            CHECK(enumerateChildReasonSuffix(false, "", 2) ==
                  " - enumeration child, whole bus, attempt 2 (contained)");
            CHECK(enumerateChildReasonSuffix(false, "uhd", 1) ==
                  " - enumeration child, driver=uhd, attempt 1 (contained)");
            CHECK(enumerateChildReasonSuffix(true, "uhd", 1) ==
                  " - enumeration child, driver list, attempt 1 (contained)");
            // Not told which attempt (a hand-started child): said nowhere.
            CHECK(enumerateChildReasonSuffix(false, "uhd", 0) ==
                  " - enumeration child, driver=uhd (contained)");
            // A driver name is third-party text in a "name: value" line: one
            // newline would split the reason and forge a field.
            const std::string evil = enumerateChildReasonSuffix(false, "Evil\nkind: hang", 1);
            CHECK(evil.find('\n') == std::string::npos);
            CHECK(evil.find("driver=evil?kind??hang") != std::string::npos);
            // THE BUDGET: the site keeps 200 characters of a reason. The
            // longest this product writes for a fatal fault is about ninety
            // (the abort one) and for an absorbed vendor fault 134; the tail
            // behind a driver of any length has to fit behind the first
            // always and behind the second for every real driver name.
            const std::string longest =
                enumerateChildReasonSuffix(false, std::string(80, 'x').c_str(), 99);
            CHECK(90u + longest.size() <= 200u);
            CHECK(134u + enumerateChildReasonSuffix(false, "faultfixture", 2).size() <= 200u);
        }

#ifdef _WIN32
        // (a), (b) and (d) against the REAL child and REAL vendor modules: the
        // fault fixtures (tests/fixtures/soapy_fault_module.cpp) fault inside
        // a driver's find function on the probe thread SoapySDR runs it on -
        // the shape of the libusb fault - so the child's own handler writes
        // the report with the stack, and what the parent then does about the
        // same death is what is under test. Only the fixtures' drivers are
        // asked: every real driver of this install is left out
        // (realDriversToLeaveOut), whole bus and sweep alike.
        {
            const auto clearReports = [&dir]() {
                std::error_code rec;
                for (const auto& p : crashReports(dir)) { std::filesystem::remove(p, rec); }
            };
            const auto wipe = [&dir]() {
                std::error_code rec;
                for (const auto& e : std::filesystem::directory_iterator(dir, rec)) {
                    std::error_code rm;
                    std::filesystem::remove_all(e.path(), rm);
                }
            };
            const std::string real = findRealCascade();
            CHECK(!real.empty());
            CHECK(!fixtureDir.empty());
            CHECK(!fixtureDirB.empty());  // tests/CMakeLists.txt hands it over
            const std::string pluginPathBefore = envOr("SOAPY_SDR_PLUGIN_PATH", "");
            setEnvVar("SOAPY_SDR_PLUGIN_PATH", fixtureDir.c_str());
            // "thread", not "find": the fault is raised on a thread the find
            // function spawns, which is the libusb fault's shape and the one
            // fault the vendor guard cannot absorb. A single-driver walk runs
            // the find function on its CALLING thread, so a "find" fault there
            // is absorbed (exit 0, an empty list) and the sweep's child would
            // never die - the deterministic case needs a driver whose own
            // child dies.
            setEnvVar("FOXSDR_TEST_SOAPY_FIXTURE", "thread");

            EnumOptions full;  // the whole product path: two whole-bus tries, then the sweep
            full.helperPath = real;
            full.allowInProcessFallback = false;
            full.timeoutMs = 25000;
            full.absentDrivers = realDriversToLeaveOut();

            // (a) A CHILD THAT FAULTS INSIDE A DRIVER'S FIND FUNCTION: ONE
            // report, with the stack, naming the driver and the attempt.
            //
            // THE WHOLE BUS first, one try: no driver is asked alone here
            // (every driver probes at once), so the report says "whole bus",
            // which attempt it was, and - because the child writes its probe
            // log into its own log ring as well as to the parent - which
            // probes had begun and not ended, which is the "still probing
            // when it died" shortlist the parent's own report used to carry.
            clearReports();
            cascade::source::clearSessionFaultedDriversForTest();
            EnumOptions once = full;
            once.attempts = 1;
            once.perDriverSweep = false;
            const EnumResult w = enumerateIsolated(once);
            const std::string wText = allReportText(dir);
            std::printf("find-fault whole bus: outcome=%s exit=0x%08lX reports=%zu stack=%d\n",
                        enumOutcomeName(w.outcome), w.exitCode, crashReports(dir).size(),
                        reportHasStack(wText) ? 1 : 0);
            CHECK(w.outcome == EnumOutcome::ChildDied);
            CHECK(w.exitCode == 0xC0000005ul);
            CHECK(crashReports(dir).size() == 1u);
            CHECK(reportHasStack(wText));
            CHECK(wText.find("address: soapy_fault_fixture.dll+0x") != std::string::npos);
            CHECK(reasonLines(wText).find("whole bus") != std::string::npos);
            CHECK(reasonLines(wText).find("attempt 1") != std::string::npos);
            CHECK(wText.find("cascade-probe: begin faultfixture") != std::string::npos);
            CHECK(wText.find("cascade-probe: end faultfixture") == std::string::npos);

            // ONE DRIVER ALONE, in the walk beside an open radio (nothing is
            // actually open: the skip list only has to be non-empty): the
            // listing child, then each driver left in its own child. The
            // fixture's child dies, and its one report names the driver.
            clearReports();
            cascade::source::clearSessionFaultedDriversForTest();
            EnumOptions beside = full;
            beside.skipDrivers = {"nothing-open"};
            const EnumResult one = enumerateIsolated(beside);
            const std::string oneText = allReportText(dir);
            const std::vector<std::string> wantFixture{"faultfixture"};
            std::printf("find-fault one driver: faulted=%zu reports=%zu stack=%d\n",
                        one.faultedDrivers.size(), crashReports(dir).size(),
                        reportHasStack(oneText) ? 1 : 0);
            CHECK(one.faultedDrivers == wantFixture);
            CHECK(crashReports(dir).size() == 1u);
            CHECK(reportHasStack(oneText));
            CHECK(reasonLines(oneText).find("driver=faultfixture") != std::string::npos);
            CHECK(reasonLines(oneText).find("attempt 1") != std::string::npos);
            cascade::source::clearSessionFaultedDriversForTest();

            // THE LONGEST REASON THE TAIL HAS TO FIT BEHIND: a fault the vendor
            // guard ABSORBS in the child - "find" runs on the calling thread
            // of a single-driver walk - keeps the child alive (exit 0, no
            // death, nothing for the parent to report) and writes its own
            // report, whose reason is the guard's 134 characters. The tail
            // goes behind that too, and the whole still fits the site's 200.
            clearReports();
            setEnvVar("FOXSDR_TEST_SOAPY_FIXTURE", "find");
            const EnumResult absorbedWalk = enumerateIsolated(beside);
            const std::string absorbedText = allReportText(dir);
            const std::string absorbedReason = reasonLines(absorbedText);
            const std::size_t absorbedLen =
                absorbedReason.size() > std::strlen("reason: ") + 1
                    ? absorbedReason.size() - 1 - std::strlen("reason: ")
                    : 0u;
            std::printf("absorbed in the child: deaths=%d reports=%zu reason=%zu chars\n",
                        absorbedWalk.childDeaths, crashReports(dir).size(), absorbedLen);
            CHECK(absorbedWalk.childDeaths == 0);
            CHECK(crashReports(dir).size() == 1u);
            CHECK(absorbedReason.find(std::string("reason: ") +
                                      cascade::core::kAbsorbedFaultReasonPrefix) == 0u);
            CHECK(absorbedReason.find("driver=faultfixture, attempt 1 (contained)") !=
                  std::string::npos);
            CHECK(absorbedLen > 0u && absorbedLen <= 200u);
            setEnvVar("FOXSDR_TEST_SOAPY_FIXTURE", "thread");

            // (b) THE DETERMINISTIC CASE, end to end: two whole-bus deaths,
            // then the sweep finds the culprit and it dies again. Three
            // deaths, so three files - one per death - each with its stack,
            // all one fault, and so ONE upload within a day. Before this
            // change: six files (three of the child's own, two whole-bus and
            // one per-driver from the parent), three uploads.
            clearReports();
            const EnumResult det = enumerateIsolated(full);
            const std::vector<std::string> detTexts = reportTexts(dir);
            int detStacks = 0;
            std::vector<std::string> detSigs;
            for (const std::string& t : detTexts) {
                if (reportHasStack(t)) { ++detStacks; }
                const std::string sig = signatureOf(t);
                if (std::find(detSigs.begin(), detSigs.end(), sig) == detSigs.end()) {
                    detSigs.push_back(sig);
                }
            }
            const int detSends = wouldSend(detTexts);
            std::printf("deterministic fault: deaths=%d reports=%zu with-stack=%d "
                        "signatures=%zu uploads=%d\n",
                        det.childDeaths, detTexts.size(), detStacks, detSigs.size(), detSends);
            CHECK(det.outcome == EnumOutcome::ChildDied);
            CHECK(det.attempts == 2);
            CHECK(det.sweptPerDriver);
            CHECK(det.faultedDrivers == wantFixture);
            CHECK(det.childDeaths == 3);
            CHECK(detTexts.size() == static_cast<std::size_t>(det.childDeaths));
            CHECK(detStacks == static_cast<int>(detTexts.size()));
            CHECK(detSigs.size() == 1u);
            CHECK(detSends == 1);
            cascade::source::clearSessionFaultedDriversForTest();

            // TWO DIFFERENT FAULTING DRIVERS on one machine are two faults:
            // four deaths (two whole-bus, whichever fixture's probe thread
            // faulted first, then each fixture alone), each with its stack,
            // two signatures, two uploads - and the same fault twice in a day
            // is still one upload (above).
            clearReports();
            const std::string both = fixtureDir + ";" + fixtureDirB;
            setEnvVar("SOAPY_SDR_PLUGIN_PATH", both.c_str());
            const EnumResult two = enumerateIsolated(full);
            const std::vector<std::string> twoTexts = reportTexts(dir);
            int twoStacks = 0;
            std::vector<std::string> twoSigs;
            for (const std::string& t : twoTexts) {
                if (reportHasStack(t)) { ++twoStacks; }
                const std::string sig = signatureOf(t);
                if (std::find(twoSigs.begin(), twoSigs.end(), sig) == twoSigs.end()) {
                    twoSigs.push_back(sig);
                }
            }
            const int twoSends = wouldSend(twoTexts);
            std::printf("two faulting drivers: deaths=%d reports=%zu with-stack=%d "
                        "signatures=%zu uploads=%d faulted=%zu\n",
                        two.childDeaths, twoTexts.size(), twoStacks, twoSigs.size(), twoSends,
                        two.faultedDrivers.size());
            CHECK(two.faultedDrivers.size() == 2u);
            CHECK(two.childDeaths == 4);
            CHECK(twoTexts.size() == static_cast<std::size_t>(two.childDeaths));
            CHECK(twoStacks == static_cast<int>(twoTexts.size()));
            CHECK(twoSigs.size() == 2u);
            CHECK(twoSends == 2);
            cascade::source::clearSessionFaultedDriversForTest();
            setEnvVar("SOAPY_SDR_PLUGIN_PATH", fixtureDir.c_str());

            // (d) DIAGNOSTICS OFF: no file from either process. The deaths
            // still happen and are still contained (that is what makes this a
            // test of the parent and the child and not of a scan that never
            // faulted), and the crash directory stays exactly as empty as it
            // was - the child is handed no directory, so it dies quietly, and
            // the parent's own writer is disabled.
            cascade::core::setCrashCaptureEnabled(false, false);
            CHECK(cascade::core::activeCrashDir().empty());
            wipe();
            CHECK(filesIn(dir) == 0u);
            const EnumResult off = enumerateIsolated(full);
            std::printf("diagnostics off: deaths=%d files=%zu\n", off.childDeaths, filesIn(dir));
            CHECK(off.childDeaths == 3);
            CHECK(off.faultedDrivers == wantFixture);
            CHECK(filesIn(dir) == 0u);
            cascade::core::setCrashCaptureEnabled(true, false);
            CHECK(cascade::core::activeCrashDir() == dir.string());
            cascade::source::clearSessionFaultedDriversForTest();

            setEnvVar("FOXSDR_TEST_SOAPY_FIXTURE", "");
            setEnvVar("SOAPY_SDR_PLUGIN_PATH", pluginPathBefore.c_str());
            clearReports();
        }
#endif  // _WIN32

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
