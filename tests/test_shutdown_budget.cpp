// THE SHUTDOWN BUDGET: the arithmetic nobody was watching - and then the
// guard that could not see the first drift after it was written.
//
// The newest field report this product received was a lie it told about
// itself. HangWatchdog::stop() joins its own thread; that thread was inside
// captureAllThreads() writing a report which said the main thread was stuck
// in HangWatchdog::stop(). The capture had provably completed - the log ring
// is written last and the payload contains it - so the join returned and the
// application exited normally. A clean shutdown filed a freeze report against
// itself, and a report that closes with a fault nobody can reproduce is worse
// than no report at all.
//
// The cause was arithmetic, not logic. The teardown deliberately runs
// un-heartbeaten so that a shutdown which WEDGES is still reported, and it is
// also where the deliberate bounded waits live. Those waits together were
// 4.5 s of a 5 s threshold, leaving about half a second for the DSP join, the
// config write, both ImGui shutdowns and GLFW - and the field log crossed the
// threshold 478 ms after the last guard returned. Nothing in the suite
// related the cost of the shutdown path to the threshold it had to fit
// inside: the only assertion that tied the threshold to a measurement read
// worstGapMs, which is fed solely by heartbeat() and therefore cannot see the
// teardown at all. So the two numbers drifted apart across three releases,
// and 0.68.0's "every driver wait is bounded" work made the false report
// near-certain on exactly the sessions those guards exist for.
//
// WHY THE FIRST VERSION OF THIS FILE WAS ALREADY BLIND, which is the reason
// the scan below looks the way it does. It read exactly TWO constants out of
// the shipping source BY NAME - kControlLockWait and kSourceJoinWait - added
// them up, and required the sum to equal HangWatchdog::kShutdownBoundedWaitsMs.
// In the same change-set that introduced it, the escape-path work added a
// THIRD bounded wait, kVendorCallWait, spent inside SoapySource::stopLocked()
// once the lock the first constant guards has been taken. A wedged close
// therefore costs 3 s in that one function where the file still reported 1.5,
// and the sum still came out at exactly the declared 4500 - green, and wrong,
// on the very first drift it existed to catch. A guard that silently ignores
// what it has not been introduced to is not a guard.
//
// So it DISCOVERS them. Every `constexpr std::chrono::<unit>` declaration
// under src/ is found by walking the tree, and each one must appear in
// kKnownWaits with a statement of what it costs the shutdown: a positive
// multiplier if the stretch between watchdog_.beginShutdown() and
// watchdog_.stop() can wait on it, or zero WITH THE REASON IT CANNOT. A
// declaration this file has never heard of fails the test by name, file and
// line; so does a row whose constant has been renamed away, so a rename shows
// up as a matched pair rather than as silence. The escape hatch of writing
// the number inline at the call site is closed too: on the files that
// declare these waits, a `wait_for`/`try_lock_for`/timed-lock line carrying a
// literal duration instead of a named constant is a failure, because the scan
// can only see what has a name.
//
// The blocks:
//
//   1. THE ARITHMETIC, against every bounded wait DISCOVERED in the source
//      that ships - not against a list of names this file was given.
//   2. THE WIRING, in app_window.cpp: the teardown is bracketed by
//      beginShutdown(), the watchdog is still stopped LAST, and nothing
//      pauses it in between - because a shutdown that wedges must still be
//      reported, and a WatchdogPause across the teardown would be the easy
//      fix that silently deletes the feature.
//   3. THE CLASS: the budget suppresses the stall that used to be reported
//      and still reports one past the budget, and stop() no longer costs the
//      rest of a poll interval.
//   4. THE PRODUCT, both halves, against the shipped binary: a teardown
//      longer than the frame threshold must NOT report, and a teardown longer
//      than the shutdown budget MUST. A unit test of the watchdog class
//      proves the class; only that block proves the application.
//
// AND ONE THING THE PRODUCT BLOCKS SAY OUT LOUD. They launch the real
// cascade.exe three times, for up to 22 s each, on a machine where other
// agents build and test the same tree - and every one of them follows the
// standing instruction to close a running cascade before building. A child
// stopped that way exits -1 (0xFFFFFFFF, the code TerminateProcess leaves),
// which this application never produces for itself: crash_handler.cpp exits
// with the exception code and soapy_enum_proc.cpp with 0xE0454E55. That
// signature is recognised, retried once, and if it happens again reported as
// what it is - contention on this machine, with a census of what else was
// running - rather than as a bare assertion about the shutdown budget. A test
// that goes red when the machine is busy gets ignored the third time, and
// then it guards nothing.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/hang_watchdog.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <windows.h>
// After windows.h, and in that order: tlhelp32.h needs its types.
#include <tlhelp32.h>
#endif

namespace fs = std::filesystem;
using cascade::core::HangWatchdog;

namespace {

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t eol = text.find('\n', pos);
        const std::size_t end = (eol == std::string::npos) ? text.size() : eol;
        std::string line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        out.push_back(std::move(line));
        if (eol == std::string::npos) { break; }
        pos = eol + 1;
    }
    return out;
}

// True when the occurrence at `at` is real code rather than a mention in a
// comment. Checked because the obvious way to test this file - comment the
// call out and watch it go red - is also the obvious way a maintainer would
// disable it, and a plain find() cannot tell "watchdog_.beginShutdown();"
// from "// watchdog_.beginShutdown();". That is not hypothetical: it is how
// the red half of this file was verified, and the wiring block passed anyway
// until this was added.
bool isLiveCode(const std::string& text, std::size_t at) {
    if (at == std::string::npos) { return false; }
    const std::size_t bol = text.rfind('\n', at);
    const std::size_t start = (bol == std::string::npos) ? 0 : bol + 1;
    const std::string prefix = text.substr(start, at - start);
    return prefix.find("//") == std::string::npos;
}

// The same question for one line: is `at` inside this line's comment?
bool inComment(const std::string& line, std::size_t at) {
    const std::size_t slashes = line.find("//");
    return slashes != std::string::npos && slashes < at;
}

bool identChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// ---------------------------------------------------------------------------
// DISCOVERY: every bounded wait declared in the source that ships.
// ---------------------------------------------------------------------------

struct DiscoveredWait {
    std::string file;   // relative to CASCADE_SOURCE_DIR, '/' separated
    std::string name;   // the constant's identifier
    int line = 0;       // 1-based
    long ms = -1;       // duration in milliseconds; -1 = found but unreadable
};

// One line of source, answered as: is this the declaration of a chrono
// constant, and if so what is it called and how long is it?
//
// Both shapes the tree actually uses are handled, and a THIRD outcome is
// deliberate: a line that declares a chrono constant whose value this cannot
// read (an unfamiliar unit, an expression, a declaration wrapped onto the
// next line) is reported as found with ms = -1 rather than skipped. A wait
// the scan cannot read is a wait the scan does not know about, which is the
// exact failure this file exists to stop being silent.
//
//   constexpr std::chrono::milliseconds kControlLockWait{1500};
//   constexpr auto kQuitGrace = std::chrono::milliseconds(250);
bool parseWaitDeclaration(const std::string& line, std::string& name, long& ms) {
    const std::size_t cx = line.find("constexpr");
    if (cx == std::string::npos || inComment(line, cx)) { return false; }
    const std::size_t ch = line.find("std::chrono::", cx);
    if (ch == std::string::npos) { return false; }

    // The unit token immediately after the namespace.
    const std::size_t u = ch + std::strlen("std::chrono::");
    std::size_t ue = u;
    while (ue < line.size() && identChar(line[ue])) { ++ue; }
    const std::string unit = line.substr(u, ue - u);

    // "milliseconds" CONTAINS "seconds", so exact token comparison rather
    // than find() - getting that backwards would read 1500 ms as 1500 s and
    // the whole check would pass for the wrong reason, forever.
    long mul = 0;
    long div = 1;
    if (unit == "milliseconds") {
        mul = 1;
    } else if (unit == "seconds") {
        mul = 1000;
    } else if (unit == "minutes") {
        mul = 60000;
    } else if (unit == "hours") {
        mul = 3600000;
    } else if (unit == "microseconds") {
        mul = 1;
        div = 1000;
    } else if (unit == "nanoseconds") {
        mul = 1;
        div = 1000000;
    }

    // The name. `constexpr auto NAME = std::chrono::UNIT(N);` puts it before
    // the '='; `constexpr std::chrono::UNIT NAME{N};` puts it after the unit.
    const std::size_t eq = line.find('=');
    if (eq != std::string::npos && eq < ch) {
        std::size_t e = eq;
        while (e > cx && !identChar(line[e - 1])) { --e; }
        std::size_t s = e;
        while (s > cx && identChar(line[s - 1])) { --s; }
        if (s == e) { return false; }
        name = line.substr(s, e - s);
    } else {
        std::size_t s = ue;
        while (s < line.size() && (line[s] == ' ' || line[s] == '\t')) { ++s; }
        std::size_t e = s;
        while (e < line.size() && identChar(line[e])) { ++e; }
        if (s == e) { return false; }
        name = line.substr(s, e - s);
    }

    // Past this point it IS a declaration as far as this file is concerned:
    // every remaining failure sets ms = -1 and is reported, never dropped.
    ms = -1;
    if (mul == 0) { return true; }  // a unit this scan does not understand

    const std::size_t open = line.find_first_of("({", ue);
    if (open == std::string::npos) { return true; }
    const char closer = (line[open] == '(') ? ')' : '}';
    const std::size_t close = line.find(closer, open);
    if (close == std::string::npos) { return true; }
    std::string digits = line.substr(open + 1, close - open - 1);
    std::size_t b = 0;
    while (b < digits.size() && (digits[b] == ' ' || digits[b] == '\t')) { ++b; }
    std::size_t e = digits.size();
    while (e > b && (digits[e - 1] == ' ' || digits[e - 1] == '\t')) { --e; }
    digits = digits.substr(b, e - b);
    if (digits.empty()) { return true; }
    for (const char c : digits) {
        if (c < '0' || c > '9') { return true; }
    }
    const long n = std::atol(digits.c_str());
    ms = (n * mul + div - 1) / div;  // sub-millisecond units round up
    return true;
}

bool scannableSource(const fs::path& p) {
    const std::string ext = p.extension().string();
    return ext == ".cpp" || ext == ".hpp" || ext == ".h" || ext == ".cc" || ext == ".cxx" ||
           ext == ".inl";
}

// Walks the WHOLE of src/ rather than a list of files. The scan boundary has
// to be somewhere, and "the source this product is built from" is the only
// boundary that cannot quietly exclude the next bounded wait somebody adds:
// a new one in a file this test has never heard of is still found, and still
// has to be classified before the suite goes green again.
std::vector<DiscoveredWait> discoverBoundedWaits(const fs::path& srcRoot) {
    std::vector<DiscoveredWait> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(srcRoot, ec), end; it != end; it.increment(ec)) {
        if (ec) { break; }
        if (!it->is_regular_file(ec) || !scannableSource(it->path())) { continue; }
        const std::vector<std::string> lines = splitLines(readFile(it->path()));
        std::string rel = fs::relative(it->path(), srcRoot.parent_path(), ec).generic_string();
        if (rel.empty()) { rel = it->path().generic_string(); }
        for (std::size_t i = 0; i < lines.size(); ++i) {
            DiscoveredWait d;
            if (!parseWaitDeclaration(lines[i], d.name, d.ms)) { continue; }
            d.file = rel;
            d.line = static_cast<int>(i) + 1;
            out.push_back(std::move(d));
        }
    }
    return out;
}

// WHAT EACH DISCOVERED WAIT COSTS THE SHUTDOWN. Discovery answers "does this
// exist"; this table answers "where is it spent", which is the part a scan
// cannot derive and a human must state. Every discovered declaration must
// match a row here, and every row must still match a declaration.
//
// `timesOnShutdownPath` is how many times the stretch between
// watchdog_.beginShutdown() and watchdog_.stop() in AppWindow::run() can wait
// the full bound. Zero is a legitimate answer and must carry the reason.
//
// THE COMPOSITION, traced through the code rather than assumed:
//   AppWindow::run() teardown -> pipeline_.stop()
//     -> quiesceSourceThreadLocked() -> active_->stop() = SoapySource::stop()
//          waits kControlLockWait for the driver lock                (1x)
//          -> stopLocked() -> runAbandonableVendorCall(deactivateStream)
//               waits kVendorCallWait for that one vendor call       (1x)
//     -> srcExitFuture_.wait_for(kSourceJoinWait) for the source thread (1x)
// Each is spent at most once because the first abandonment condemns the
// device and every later call on the same path is skipped rather than
// attempted (see abandonWedgedDriverLocked).
//
// RESIDUAL, stated because it is invisible from the arithmetic: the same
// lock-then-vendor-call pair composes AGAIN in ~SoapySource -> closeDevice()
// (closeStream, then unmake). That runs from ~AppWindow, which main() reaches
// only after run() has returned - i.e. after watchdog_.stop() - so it is not
// inside this budget and not watched at all. It is out of scope here; it is
// not out of scope for the product.
struct KnownWait {
    const char* file;
    const char* name;
    int timesOnShutdownPath;
    const char* where;
};

const KnownWait kKnownWaits[] = {
    {"src/source/soapy_source.cpp", "kControlLockWait", 1,
     "SoapySource::stop() - the driver-lock wait Pipeline::stop() spends on the GUI thread"},
    {"src/source/soapy_source.cpp", "kVendorCallWait", 1,
     "SoapySource::stopLocked() - the abandonable deactivateStream, once that lock is held"},
    {"src/core/pipeline.cpp", "kSourceJoinWait", 1,
     "Pipeline::stop() - the wait for the source thread to exit, after both of the above"},
    {"src/gui/app_window.cpp", "kQuitGrace", 0,
     "~AppWindow's future reaps: runs after watchdog_.stop(), outside the budgeted stretch"},
    {"src/gui/app_window.cpp", "kNoWait", 0,
     "zero by construction - a ready-poll on a std::future, not a wait"},
};

const KnownWait* findKnown(const std::string& file, const std::string& name) {
    for (const KnownWait& k : kKnownWaits) {
        if (file == k.file && name == k.name) { return &k; }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// THE OTHER WAY A WAIT CAN HIDE: written as a literal at the call site.
// ---------------------------------------------------------------------------

// A duration constructed from a literal - std::chrono::seconds(3),
// std::chrono::milliseconds{1500} - rather than from a named constant.
bool inlineDurationLiteral(const std::string& line) {
    std::size_t pos = 0;
    while ((pos = line.find("std::chrono::", pos)) != std::string::npos) {
        std::size_t u = pos + std::strlen("std::chrono::");
        while (u < line.size() && identChar(line[u])) { ++u; }
        std::size_t o = u;
        while (o < line.size() && (line[o] == ' ' || line[o] == '\t')) { ++o; }
        if (o < line.size() && (line[o] == '(' || line[o] == '{')) {
            std::size_t d = o + 1;
            while (d < line.size() && (line[d] == ' ' || line[d] == '\t')) { ++d; }
            if (d < line.size() && line[d] >= '0' && line[d] <= '9') { return true; }
        }
        pos = u;
    }
    return false;
}

// A call that takes a timeout. sleep_for is deliberately NOT one: it is a
// delay, not a bounded wait for something else to happen, and the DSP loops
// are full of one-millisecond ones.
bool timeoutWaitCall(const std::string& line) {
    static const char* const kVerbs[] = {"wait_for(", "wait_until(", "try_lock_for(",
                                         "try_lock_until(", "timed_mutex>"};
    for (const char* v : kVerbs) {
        const std::size_t at = line.find(v);
        if (at != std::string::npos && !inComment(line, at)) { return true; }
    }
    return false;
}

#if defined(_WIN32)

// ---------------------------------------------------------------------------
// THE PRODUCT BLOCKS, and telling a busy machine apart from a broken product.
// ---------------------------------------------------------------------------

// How many processes are running with this image name. Used only to describe
// a failure: a test that says "contention" without saying what else was
// running is asking to be believed rather than read.
int runningProcesses(const wchar_t* image) {
    const HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) { return -1; }
    int n = 0;
    PROCESSENTRY32W e{};
    e.dwSize = sizeof(e);
    if (::Process32FirstW(snap, &e) != 0) {
        do {
            if (::_wcsicmp(e.szExeFile, image) == 0) { ++n; }
        } while (::Process32NextW(snap, &e) != 0);
    }
    ::CloseHandle(snap);
    return n;
}

std::string contentionCensus() {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "%d cascade.exe, %d ctest.exe, %d MSBuild.exe, %d cl.exe, %d link.exe",
                  runningProcesses(L"cascade.exe"), runningProcesses(L"ctest.exe"),
                  runningProcesses(L"MSBuild.exe"), runningProcesses(L"cl.exe"),
                  runningProcesses(L"link.exe"));
    return buf;
}

struct AppRun {
    std::string out;
    int exitCode = -1;
    unsigned long long wallMs = 0;
    int stallMs = 0;
    int hangReports = 0;
    double teardownMs = -1.0;
    std::string sampleReport;
    std::string census;  // taken after the child is gone, so it is other people's
};

// Runs the SHIPPED binary with its whole diagnostics tree redirected into
// `dir`, optionally wedging its teardown, and reports what it left behind.
AppRun runAppOnce(const fs::path& dir, int shutdownStallMs) {
    AppRun r;
    r.stallMs = shutdownStallMs;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    ::SetEnvironmentVariableA("FOXSDR_DIAG_DIR", dir.string().c_str());
    if (shutdownStallMs > 0) {
        ::SetEnvironmentVariableA("CASCADE_DIAG_SHUTDOWN_STALL_MS",
                                  std::to_string(shutdownStallMs).c_str());
    }

    const std::string exe = std::string(CASCADE_APP_BINDIR) + "/cascade.exe";
    const std::string cmd = "\"\"" + exe + "\" --frames 3 2>&1\"";
    const ULONGLONG t0 = ::GetTickCount64();
    FILE* p = _popen(cmd.c_str(), "r");
    char buf[512];
    while (p != nullptr && std::fgets(buf, sizeof(buf), p) != nullptr) { r.out += buf; }
    r.exitCode = (p != nullptr) ? _pclose(p) : -1;
    r.wallMs = static_cast<unsigned long long>(::GetTickCount64() - t0);
    ::SetEnvironmentVariableA("FOXSDR_DIAG_DIR", nullptr);
    ::SetEnvironmentVariableA("CASCADE_DIAG_SHUTDOWN_STALL_MS", nullptr);
    // AFTER the child is gone: anything this counts belongs to somebody else.
    r.census = contentionCensus();

    // The FULL prefix, not "teardown ": the wedge hook's own line says
    // "wedging the teardown for 7000 ms" and comes first, so a loose match
    // reads the stall back as the measurement and reports 0.0 ms.
    const char* kTeardownLine = "cascade: teardown ";
    const std::size_t at = r.out.find(kTeardownLine);
    if (at != std::string::npos) {
        r.teardownMs = std::atof(r.out.c_str() + at + std::strlen(kTeardownLine));
    }
    for (const auto& e : fs::directory_iterator(dir / "crashes", ec)) {
        const std::string text = readFile(e.path());
        if (text.find("kind: hang") != std::string::npos) {
            ++r.hangReports;
            r.sampleReport = text;
        }
    }
    std::printf("app run: exit %d after %llu ms, %zu bytes of output, %d hang reports\n",
                r.exitCode, r.wallMs, r.out.size(), r.hangReports);
    return r;
}

// THE FINGERPRINT OF A CHILD THAT WAS STOPPED FROM OUTSIDE.
//
// -1 is 0xFFFFFFFF, the exit code TerminateProcess is asked for by
// PowerShell's Stop-Process and .NET's Process.Kill(). This application never
// leaves it: crash_handler.cpp terminates with the exception code (0xC0000005
// and friends), soapy_enum_proc.cpp with 0xE0454E55, a CLI misuse exits 1 and
// a good run exits 0. Measured, not assumed: killing a `cascade --frames 3`
// child mid-run through the same `cmd /c` wrapper _popen uses reproduces
// exactly this - exit -1, no "rendered 3 frames", a wall time far short of
// the stall it was asked for.
//
// AND WHAT IT DELIBERATELY DOES NOT CATCH, measured in the same pass:
// `taskkill /F /IM cascade.exe` surfaces as exit 1, not -1. Recognising 1 as
// "somebody killed it" is not on offer, because 1 is a code the product
// itself returns - a failed glfwInit, a malformed command line - and a retry
// keyed on it would quietly re-run a genuinely broken build until it passed.
// A child stopped with taskkill therefore fails the honest way, through the
// second message below.
//
// THE WALL-TIME CORROBORATION IS CONDITIONAL, and the first version of this
// predicate got that wrong: the healthy block asks for no stall at all, so
// `wallMs < stallMs` is false by construction there and a child killed during
// it was reported as "not stopped from outside - this one is the product's".
// Found by killing one, which is the only way that half of a diagnostic ever
// gets checked.
bool stoppedFromOutside(const AppRun& r) {
    if (r.exitCode != -1) { return false; }
    if (r.out.find("rendered 3 frames") != std::string::npos) { return false; }
    return r.stallMs <= 0 || r.wallMs < static_cast<unsigned long long>(r.stallMs);
}

// One retry for the WHOLE file. Enough to survive a single collision with
// somebody else's build; too few to hide a product that is genuinely dying on
// launch, and loud either way.
int g_killRetriesLeft = 1;
int g_killRetriesUsed = 0;

AppRun runApp(const fs::path& dir, int shutdownStallMs) {
    AppRun r = runAppOnce(dir, shutdownStallMs);
    if (stoppedFromOutside(r) && g_killRetriesLeft > 0) {
        --g_killRetriesLeft;
        ++g_killRetriesUsed;
        std::printf(
            "app run: STOPPED FROM OUTSIDE after %llu ms of a run asked to stall %d ms "
            "(exit -1). Running right now: %s. Retrying once.\n",
            r.wallMs, r.stallMs, r.census.c_str());
        std::fflush(stdout);
        r = runAppOnce(dir, shutdownStallMs);
    }
    return r;
}

// "The application exited on its own." Every block below asserts this before
// it interprets an ABSENT report, because a child that was killed from
// outside - and one wedged run of this file really was, by another build on
// the same machine stopping every cascade.exe it could find - produces the
// same empty directory as a watchdog that correctly said nothing. Without
// this the two are indistinguishable and the pass means nothing.
//
// And when it DOES fail, it says which of the two it was looking at. A bare
// `CHECK(r.exitCode == 0)` on a machine where another agent was building
// reads as "the shutdown budget is broken", which is both wrong and the
// fastest way to get a real guard ignored.
void checkRanToCompletion(const AppRun& r) {
    const bool completed =
        r.exitCode == 0 && r.out.find("rendered 3 frames") != std::string::npos;
    if (!completed) {
        if (stoppedFromOutside(r)) {
            std::printf(
                "*** CONTENTION, NOT A SHUTDOWN-BUDGET FAILURE. cascade.exe exited -1\n"
                "*** after %llu ms (this run asked for a %d ms teardown stall) and never\n"
                "*** printed \"rendered 3 frames\" - so it did not exit on its own terms.\n"
                "*** -1 (0xFFFFFFFF) is the code TerminateProcess leaves,\n"
                "*** which is what PowerShell's Stop-Process and .NET's Process.Kill() use;\n"
                "*** this application never terminates itself with it (crash_handler.cpp\n"
                "*** exits with the exception code, soapy_enum_proc.cpp with 0xE0454E55).\n"
                "*** Something outside this test stopped it, and the likeliest something is\n"
                "*** the standing instruction every agent working in this tree follows -\n"
                "*** close a running cascade before building - executed while this test had\n"
                "*** one of its own children up.\n"
                "*** Running at that moment: %s.\n"
                "*** %d retry already spent. Re-run this test with nothing else building or\n"
                "*** testing in this tree before believing anything below.\n",
                r.wallMs, r.stallMs, r.census.c_str(), g_killRetriesUsed);
        } else {
            std::printf(
                "*** THE CHILD DID NOT COMPLETE, and it was not stopped from outside:\n"
                "*** exit %d (0x%08X) after %llu ms, stall asked for %d ms, \"rendered 3\n"
                "*** frames\" %s. This one is the product's, not the machine's.\n"
                "*** Its output follows; running at that moment: %s.\n",
                r.exitCode, static_cast<unsigned>(r.exitCode), r.wallMs, r.stallMs,
                r.out.find("rendered 3 frames") != std::string::npos ? "present" : "absent",
                r.census.c_str());
        }
        std::fflush(stdout);
    }
    CHECK(r.exitCode == 0);
    CHECK(r.out.find("rendered 3 frames") != std::string::npos);
}
#endif

// Beats the watchdog the way a frame loop would, for `ms`, then returns.
void beatFor(cascade::core::HangWatchdog& w, unsigned ms) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        w.heartbeat();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

fs::path scratchDir(const std::string& tag) {
#if defined(_WIN32)
    const unsigned long pid = static_cast<unsigned long>(::GetCurrentProcessId());
#else
    const unsigned long pid = 0;
#endif
    return fs::temp_directory_path() /
           ("cascade-shutdown-" + tag + "-" + std::to_string(pid));
}

}  // namespace

int main() {
    // --- 1. THE ARITHMETIC, over every bounded wait the source declares -----
    long boundedSumMs = 0;
    {
        const fs::path root(CASCADE_SOURCE_DIR);
        const std::vector<DiscoveredWait> found = discoverBoundedWaits(root / "src");

        // The scan has to have WORKED. An empty result - a moved tree, a
        // renamed directory, a broken iterator - would make every sum below
        // pass for the wrong reason, which is the same failure class as the
        // name-scan this replaces.
        std::printf("bounded waits discovered under src/: %zu declarations\n", found.size());
        CHECK(found.size() >= 3);

        // Grouped by (file, constant): kNoWait is declared in three different
        // functions of app_window.cpp, and counting it three times would be as
        // wrong as not counting kVendorCallWait at all.
        std::map<std::pair<std::string, std::string>, std::vector<DiscoveredWait>> byName;
        for (const DiscoveredWait& d : found) { byName[{d.file, d.name}].push_back(d); }

        int unclassified = 0;
        for (const auto& entry : byName) {
            const std::string& file = entry.first.first;
            const std::string& name = entry.first.second;
            const std::vector<DiscoveredWait>& decls = entry.second;
            const KnownWait* known = findKnown(file, name);
            const long ms = decls.front().ms;

            std::printf("  %-34s %-18s %6ld ms  x%d  %s\n", file.c_str(), name.c_str(), ms,
                        known != nullptr ? known->timesOnShutdownPath : -1,
                        known != nullptr ? known->where : "(UNKNOWN)");

            if (known == nullptr) {
                std::printf(
                    "*** AN UNKNOWN BOUNDED WAIT: %s:%d declares %s at %ld ms and nothing in\n"
                    "*** this test knows what it costs a shutdown. This file used to scan for\n"
                    "*** two constants BY NAME and the very next change to the shutdown path\n"
                    "*** added a third, which is why it discovers them instead. Add a row to\n"
                    "*** kKnownWaits saying where this one is spent: timesOnShutdownPath > 0\n"
                    "*** if the stretch between watchdog_.beginShutdown() and watchdog_.stop()\n"
                    "*** can wait on it - and then raise HangWatchdog::kShutdownBoundedWaitsMs\n"
                    "*** by that much - or 0 with the reason it cannot (\"runs in ~AppWindow,\n"
                    "*** after watchdog_.stop()\" is a real one).\n",
                    file.c_str(), decls.front().line, name.c_str(), ms);
                ++unclassified;
                continue;
            }
            if (ms < 0) {
                std::printf(
                    "*** A WAIT THIS SCAN COULD NOT READ: %s:%d declares %s and the duration\n"
                    "*** could not be extracted (an unfamiliar unit, an expression, or a\n"
                    "*** declaration wrapped onto the next line). A wait that cannot be read\n"
                    "*** is a wait the budget does not know, which is the failure this file\n"
                    "*** exists to stop being silent. Put it on one line as a plain integer\n"
                    "*** count of a std::chrono unit, or teach parseWaitDeclaration the form.\n",
                    file.c_str(), decls.front().line, name.c_str());
                ++unclassified;
                continue;
            }
            for (const DiscoveredWait& d : decls) {
                if (d.ms != ms) {
                    std::printf(
                        "*** TWO VALUES FOR ONE NAME: %s declares %s as %ld ms at line %d and\n"
                        "*** %ld ms at line %d. The budget cannot be right about both.\n",
                        file.c_str(), name.c_str(), ms, decls.front().line, d.ms, d.line);
                    ++unclassified;
                }
            }
            if (known->timesOnShutdownPath > 0) {
                if (decls.size() != 1) {
                    std::printf(
                        "*** %s is on the shutdown path and is declared %zu times in %s. The\n"
                        "*** table's multiplier describes ONE declaration; split the rows or\n"
                        "*** the arithmetic below is guesswork.\n",
                        name.c_str(), decls.size(), file.c_str());
                    ++unclassified;
                }
                if (ms <= 0) {
                    std::printf("*** %s is on the shutdown path but declares %ld ms.\n",
                                name.c_str(), ms);
                    ++unclassified;
                }
                boundedSumMs += ms * known->timesOnShutdownPath;
            }
        }
        CHECK(unclassified == 0);

        // ...AND THE TABLE MUST NOT KEEP GHOSTS. A constant that has been
        // renamed shows up twice - once here as a row with nothing to match,
        // once above as an unknown wait - and the pair names the rename. A
        // row nobody removes, on the other hand, is a wait the budget still
        // charges itself for and the product no longer performs.
        int stale = 0;
        for (const KnownWait& k : kKnownWaits) {
            if (byName.find({k.file, k.name}) == byName.end()) {
                std::printf(
                    "*** A ROW WITH NO CONSTANT: kKnownWaits still lists %s in %s and no such\n"
                    "*** declaration exists any more. If it was renamed, the new name is\n"
                    "*** reported above as unknown - fix both. If it was deleted, delete the\n"
                    "*** row and lower kShutdownBoundedWaitsMs by what it contributed.\n",
                    k.name, k.file);
                ++stale;
            }
        }
        CHECK(stale == 0);

        std::printf("sum on the shutdown path %ld ms; declared kShutdownBoundedWaitsMs %u ms\n",
                    boundedSumMs, HangWatchdog::kShutdownBoundedWaitsMs);
        // The watchdog's copy of the number must still be the truth. Change
        // any guard and this is the line that goes red, and it names the real
        // sum so the fix is to update the constant AND re-check the budget
        // below - which is the whole point of the file.
        CHECK(boundedSumMs == static_cast<long>(HangWatchdog::kShutdownBoundedWaitsMs));

        // THE OTHER PLACE A WAIT CAN HIDE. Discovery can only see a wait that
        // has a name, so a duration written as a literal at the call site
        // would slip past everything above. Checked on the files that declare
        // the waits the budget depends on - a set DERIVED from the table
        // rather than listed again, so it follows the shutdown path as the
        // table does.
        int inlineWaits = 0;
        for (const KnownWait& k : kKnownWaits) {
            const fs::path src = root / k.file;
            const std::vector<std::string> lines = splitLines(readFile(src));
            CHECK(!lines.empty());
            for (std::size_t i = 0; i < lines.size(); ++i) {
                const std::string& line = lines[i];
                if (line.find("constexpr") != std::string::npos) { continue; }
                if (!timeoutWaitCall(line) || !inlineDurationLiteral(line)) { continue; }
                std::printf(
                    "*** A BOUNDED WAIT WITH NO NAME: %s:%zu waits on a duration written\n"
                    "*** inline. Nothing can discover it, so nothing can hold the shutdown\n"
                    "*** budget to it. Declare it as a constexpr std::chrono constant and\n"
                    "*** give it a row in kKnownWaits.\n"
                    "***   %s\n",
                    k.file, i + 1, line.c_str());
                ++inlineWaits;
            }
        }
        CHECK(inlineWaits == 0);

        // THE HEADROOM RULE. The worst LEGITIMATE shutdown is every bounded
        // wait spent in full plus the reserve for the unbounded-but-fast rest
        // of the teardown; the budget must be at least twice that, so that a
        // report written against it means something genuinely wedged. This is
        // the assertion that goes red if the bounded waits ever again grow
        // towards the threshold that judges them. Built from the sum
        // DISCOVERED IN THE SOURCE, not from the watchdog's copy of it: the
        // copy is checked for agreement above, and the budget has to be right
        // about the waits the product actually performs.
        const long worstLegitimate =
            boundedSumMs + static_cast<long>(HangWatchdog::kShutdownReserveMs);
        std::printf("worst legitimate shutdown %ld ms; budget %u ms\n", worstLegitimate,
                    HangWatchdog::kShutdownThresholdMs);
        CHECK(worstLegitimate * 2 <= static_cast<long>(HangWatchdog::kShutdownThresholdMs));

        // ...AND WHY THE TEARDOWN NEEDS A BUDGET OF ITS OWN AT ALL, stated as
        // arithmetic rather than as a sentence in a header: the same worst
        // legitimate shutdown does not fit inside the frame threshold. This
        // is the defect, in one line. If it ever goes red the guards have got
        // small enough that the separate budget could be retired - which is a
        // decision to make deliberately, not to discover by deleting a call.
        std::printf("frame threshold %u ms\n", HangWatchdog::kDefaultThresholdMs);
        CHECK(worstLegitimate > static_cast<long>(HangWatchdog::kDefaultThresholdMs));

        // The reserve is not a decoration: the field log crossed the old
        // threshold 478 ms after the last bounded guard returned, so whatever
        // the reserve is, it has to be comfortably more than that.
        CHECK(worstLegitimate - boundedSumMs >= 1000L);
    }

    // --- 2. THE WIRING, in the source that ships ----------------------------
    //
    // The dynamic blocks below prove the behaviour; this proves the SHAPE
    // that keeps a wedged shutdown reportable, which no timing test can see.
    // (Reading a source file to hold a promise is the same device
    // tests/test_diag_hang.cpp uses for the clean-exit ordering.)
    {
        const fs::path src = fs::path(CASCADE_SOURCE_DIR) / "src" / "gui" / "app_window.cpp";
        const std::string text = readFile(src);
        CHECK(!text.empty());

        const std::size_t begin = text.find("watchdog_.beginShutdown();");
        const std::size_t heartbeat = text.find("watchdog_.heartbeat(");
        const std::size_t stop = text.find("watchdog_.stop();");
        // ...and each of them has to be a CALL, not a mention in a comment.
        CHECK(isLiveCode(text, begin));
        CHECK(isLiveCode(text, heartbeat));
        CHECK(isLiveCode(text, stop));
        const bool haveAnchors = begin != std::string::npos &&
                                 heartbeat != std::string::npos && stop != std::string::npos;

        // Exactly one call: two would mean the budget is being reset
        // somewhere in the middle of the teardown, which would hide a wedge
        // in whichever half came first.
        CHECK(haveAnchors &&
              text.find("watchdog_.beginShutdown();", begin + 1) == std::string::npos);

        // The heartbeat is inside the frame loop, so it must come first; the
        // teardown work must come after.
        CHECK(haveAnchors && heartbeat < begin);
        CHECK(haveAnchors && begin < stop);

        const std::size_t pipelineStop =
            haveAnchors ? text.find("pipeline_.stop();", begin) : std::string::npos;
        const std::size_t glfwTerm =
            haveAnchors ? text.find("glfwTerminate();", begin) : std::string::npos;
        std::printf("teardown wiring: beginShutdown@%zu pipeline_.stop@%zu glfwTerminate@%zu "
                    "watchdog_.stop@%zu\n",
                    begin, pipelineStop, glfwTerm, stop);
        // The two bounded guards and the GL teardown are inside the budgeted
        // stretch...
        CHECK(pipelineStop != std::string::npos && pipelineStop < stop);
        CHECK(glfwTerm != std::string::npos && glfwTerm < stop);
        // ...and the watchdog is still stopped LAST, which is what keeps a
        // shutdown that wedges reportable at all.
        CHECK(pipelineStop != std::string::npos && glfwTerm != std::string::npos &&
              pipelineStop < glfwTerm);

        // AND NOTHING PAUSES IT ACROSS THE TEARDOWN. WatchdogPause was the
        // other available fix and it is the wrong one: it would silence the
        // false report and the 120 s CAT shutdown freeze with the same line.
        const std::size_t pause = haveAnchors ? text.find("WatchdogPause", begin)
                                              : std::string::npos;
        CHECK(pause == std::string::npos || (stop != std::string::npos && pause > stop));
    }

#if defined(_WIN32)
    // --- 3. The class: the budget applies, and does NOT disarm anything -----
    //
    // Windows-only for the same reason the rest of this section is: the
    // capture that turns a crossed threshold into a report is a Win32 stack
    // walk, so on any other platform "no report" is the answer to every
    // question here.
    //
    // Both halves are asserted, and the second is the one that matters most:
    // a fix that silenced the false positive by pausing or disarming the
    // watchdog across the teardown would ALSO have deleted the 120 s CAT
    // shutdown freeze - the fault class this product ships most - and every
    // other assertion in this file would still have passed.
    {
        const fs::path dir = scratchDir("class");
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);

        HangWatchdog w;
        w.setSuppressionForTest(HangWatchdog::SuppressionForTest::NeverSuppress);
        w.start(dir.string(), 800);
        CHECK(w.thresholdMs() == 800u);
        beatFor(w, 900);

        // The frame loop has ended; the teardown begins.
        w.beginShutdown(4000);
        CHECK(w.thresholdMs() == 4000u);

        // Nearly three seconds with no heartbeat at all - more than three
        // times the session threshold, and the exact shape of the field
        // report. Nothing may be written.
        std::this_thread::sleep_for(std::chrono::milliseconds(2400 + HangWatchdog::kPollMs));
        CHECK(w.reportsWritten() == 0u);
        CHECK(fs::is_empty(dir));

        // ...and past the budget it still reports, because the teardown was
        // never disarmed - only re-judged.
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        CHECK(w.reportsWritten() == 1u);
        const std::string text = readFile(fs::path(w.lastReportPath()));
        CHECK(text.find("kind: hang") != std::string::npos);
        // The report says WHICH clock judged it, so a frozen close is
        // distinguishable from a frozen session.
        CHECK(text.find("threshold-ms: 4000") != std::string::npos);

        w.stop();
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // --- 4. stop() does not cost the rest of a poll --------------------------
    //
    // The second-order half of the same defect: stop() set a flag the loop
    // only read at the top, and the loop slept in a plain, uninterruptible
    // sleep_for(kPollMs) - so asking the watchdog to stop cost up to half a
    // second of every single shutdown, doing nothing. The measurement is
    // deliberately DETERMINISTIC rather than statistical: the stop is asked
    // for ~20 ms into a fresh poll wait, so the old code would have held it
    // for the remaining ~480 ms on every iteration rather than on some.
    {
        double worstMs = 0.0;
        for (int i = 0; i < 3; ++i) {
            HangWatchdog w;
            // Empty directory and suppression on: this block is about how
            // long stop() takes, and must not write a file whatever it races.
            w.setSuppressionForTest(HangWatchdog::SuppressionForTest::AlwaysSuppress);
            w.start(std::string(), HangWatchdog::kDefaultThresholdMs);
            CHECK(w.running());
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            const auto t0 = std::chrono::steady_clock::now();
            w.stop();
            const double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                    .count();
            if (ms > worstMs) { worstMs = ms; }
            CHECK(!w.running());
        }
        std::printf("worst stop() latency: %.1f ms (poll interval %u ms)\n", worstMs,
                    HangWatchdog::kPollMs);
        CHECK(worstMs < static_cast<double>(HangWatchdog::kPollMs) / 2.0);
    }

    // --- 5a. A healthy close sits far under the budget ----------------------
    //
    // The measurement the budget is justified against, from the real binary -
    // the counterpart of "worst frame gap" for the half of the session the
    // frame counter cannot see.
    {
        const fs::path dir = scratchDir("healthy");
        const AppRun r = runApp(dir, 0);
        std::printf("%s", r.out.c_str());
        checkRanToCompletion(r);
        std::printf("healthy teardown: %.1f ms (budget %u ms)\n", r.teardownMs,
                    HangWatchdog::kShutdownThresholdMs);
        CHECK(r.teardownMs >= 0.0);
        // Under a quarter of the budget. A teardown that grows into that goes
        // red here rather than as a false freeze report on a user's machine,
        // which is exactly how this defect reached the field.
        CHECK(r.teardownMs <
              static_cast<double>(HangWatchdog::kShutdownThresholdMs) / 4.0);
        CHECK(r.hangReports == 0);

        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // --- 5b. A slow close is NOT a freeze -----------------------------------
    //
    // THE FIELD REPORT, staged. The teardown is wedged for longer than the
    // frame threshold - which is what the bounded guards can legitimately
    // cost between them - and shorter than the budget. Before beginShutdown()
    // this run wrote a hang report naming the application's own shutdown;
    // now it must write nothing. Removing the beginShutdown() call from
    // app_window.cpp turns this block red, which is how it was checked.
    {
        const fs::path dir = scratchDir("slow");
        const int stallMs = static_cast<int>(HangWatchdog::kDefaultThresholdMs) + 2000;
        const AppRun r = runApp(dir, stallMs);
        std::printf("%s", r.out.c_str());
        checkRanToCompletion(r);
        // The hook really ran: without this, a wedge that silently did
        // nothing would satisfy "no report" for the wrong reason.
        CHECK(r.out.find("--diag-shutdown-stall wedging the teardown") != std::string::npos);
        std::printf("slow teardown: %.1f ms (frame threshold %u ms, budget %u ms)\n",
                    r.teardownMs, HangWatchdog::kDefaultThresholdMs,
                    HangWatchdog::kShutdownThresholdMs);
        // It really did outlast the frame threshold - so a teardown judged by
        // that threshold would have reported - and really did stay inside the
        // budget.
        CHECK(r.teardownMs > static_cast<double>(HangWatchdog::kDefaultThresholdMs));
        CHECK(r.teardownMs < static_cast<double>(HangWatchdog::kShutdownThresholdMs));
        std::printf("hang reports from a slow but healthy close: %d\n", r.hangReports);
        CHECK(r.hangReports == 0);

        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // --- 5c. ...but a shutdown that WEDGES is still reported -----------------
    //
    // The property the un-heartbeaten teardown was designed for, and the one
    // the fix had to keep. The worst freeze this product ever shipped was
    // 120 s inside a CAT client shutdown; a fix that stopped reporting that
    // class in order to silence the false positive would have been a
    // regression wearing a bug fix's clothes.
    {
        const fs::path dir = scratchDir("wedged");
        const int stallMs = static_cast<int>(HangWatchdog::kShutdownThresholdMs) + 2000;
        const AppRun r = runApp(dir, stallMs);
        std::printf("%s", r.out.c_str());
        checkRanToCompletion(r);
        CHECK(r.out.find("--diag-shutdown-stall wedging the teardown") != std::string::npos);
        std::printf("hang reports from a wedged close: %d\n", r.hangReports);
        CHECK(r.hangReports >= 1);
        // Measured against the SHUTDOWN budget, not the frame threshold - the
        // report says which clock judged it, so a reader can tell a frozen
        // session from a frozen close.
        CHECK(r.sampleReport.find("threshold-ms: " +
                                  std::to_string(HangWatchdog::kShutdownThresholdMs)) !=
              std::string::npos);
        // And it is a real report, not a stub: the stalled thread is named
        // and more than one stack is present, because a deadlock is only
        // legible as a pair.
        CHECK(r.sampleReport.find("(gui, stalled)") != std::string::npos);
        CHECK(r.sampleReport.find("--- modules ---") != std::string::npos);

        std::error_code ec;
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // Said even on a pass: a run that had to retry a child SAW contention on
    // this machine, and a green line with no mention of it is how a flake
    // becomes folklore.
    if (g_killRetriesUsed > 0) {
        std::printf("NOTE: %d app run(s) were stopped from outside and retried. Something "
                    "else was building or testing in this tree.\n",
                    g_killRetriesUsed);
    }
#endif

    return testSummary("test_shutdown_budget");
}
