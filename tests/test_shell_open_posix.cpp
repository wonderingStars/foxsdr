// Tests for gui/shell_open.hpp's posixShellOpen() - the Linux replacement for
// ShellExecuteA, used by AppWindow::shellOpen() (src/gui/app_window.cpp) to
// open the crash-reports folder and hand the "download the update" and
// "privacy policy" links to a real browser via xdg-open.
//
// WHY A STAND-IN INSTEAD OF THE REAL xdg-open. This suite runs under Xvfb on
// a CI box with no desktop session and, per Linux porting policy, no test may
// actually launch a browser or file manager. FOXSDR_SHELL_OPEN_EXE points
// posixShellOpen() at this test's own stand-in script instead - a tiny shell
// script that appends its argv to a file and exits immediately - which lets
// this suite observe exactly what would have been exec'd without anything
// user-visible happening. This is the same shape of seam FOXSDR_GPS_PORT and
// FOXSDR_SOAPY_DEBUG use elsewhere in this codebase: an environment variable
// read at the call site, naming a substitute the test controls.
//
// WHAT WOULD ROT SILENTLY WITHOUT THIS. posixShellOpen() forks twice so that
// (a) the call never blocks waiting for xdg-open, which can legitimately run
// for as long as a freshly-launched browser stays open, and (b) nothing is
// left as a zombie regardless of how long the grandchild runs, because the
// grandchild is reparented to init/subreaper rather than reaped by this
// process. Both properties are invisible from the return value alone, so they
// are checked directly: elapsed wall time proves the call did not wait for a
// SLOW stand-in, and reading /proc's process state after the parent returns
// proves nothing was left as <defunct>.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/shell_open.hpp"

#include "test_check.hpp"

#if !defined(_WIN32)

#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

namespace {

namespace fs = std::filesystem;

fs::path scratchDir() {
    return fs::temp_directory_path() / ("foxsdr_shell_open_posix_" + std::to_string(getpid()));
}

// Writes a stand-in "xdg-open" that records every argument it was called with
// (one per line) to `recordPath`, optionally sleeping first, then exits 0.
// #!/bin/sh scripts rather than a compiled helper: every other seam script in
// this suite's spirit (tests/test_soapy_enum_proc.cpp's helper aside, which
// re-invokes the test binary itself - not applicable here, since the thing
// under test is what argv a POSIX shell tool was launched with) is simplest
// as a shell script, and /bin/sh is guaranteed present on any Linux CI image
// this project builds on.
fs::path writeRecorderScript(const fs::path& dir, const fs::path& recordPath,
                              int sleepSeconds = 0) {
    const fs::path script = dir / "record_argv.sh";
    std::ofstream out(script, std::ios::binary | std::ios::trunc);
    out << "#!/bin/sh\n";
    if (sleepSeconds > 0) { out << "sleep " << sleepSeconds << "\n"; }
    out << "for a in \"$@\"; do printf '%s\\n' \"$a\" >> \"" << recordPath.string() << "\"; done\n";
    out << "exit 0\n";
    out.close();
    fs::permissions(script, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                                 fs::perms::others_read | fs::perms::others_exec);
    return script;
}

// Polls for `path` to exist, up to a couple of seconds - posixShellOpen()
// hands the real work to a grandchild the parent does not wait for, so the
// record file appears asynchronously.
bool waitForFile(const fs::path& path, int timeoutMs = 3000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        std::error_code ec;
        if (fs::exists(path, ec) && fs::file_size(path, ec) > 0) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct ScopedEnv {
    std::string name;
    bool hadOld = false;
    std::string old;
    ScopedEnv(std::string n, const std::string& value) : name(std::move(n)) {
        if (const char* prev = std::getenv(name.c_str())) {
            hadOld = true;
            old = prev;
        }
        setenv(name.c_str(), value.c_str(), 1);
    }
    ~ScopedEnv() {
        if (hadOld) {
            setenv(name.c_str(), old.c_str(), 1);
        } else {
            unsetenv(name.c_str());
        }
    }
};

// THE PROPERTY THAT MATTERS MOST: the target string reaches the stand-in as
// argv[1], verbatim, via execvp - never through a shell, so a target with `;`
// or `&` in it (an oddly-named folder, a URL with a query string) cannot be
// reinterpreted as a second command.
void checkArgvReachesTheTarget() {
    const fs::path dir = scratchDir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path record = dir / "argv.log";
    const fs::path script = writeRecorderScript(dir, record);
    ScopedEnv env("FOXSDR_SHELL_OPEN_EXE", script.string());

    const std::string target = "/tmp/some folder; rm -rf / #not-a-command &also-not";
    const bool ok = cascade::gui::posixShellOpen(target);
    CHECK(ok);
    CHECK(waitForFile(record));
    const std::string got = readFile(record);
    CHECK(got == target + "\n");
    if (got != target + "\n") {
        std::printf("FAIL argv recorded as: [%s] want [%s]\n", got.c_str(), target.c_str());
    }

    fs::remove_all(dir, ec);
}

// A missing/non-executable FOXSDR_SHELL_OPEN_EXE must come back false, not
// crash or hang - this is the "xdg-open isn't installed" case for real.
void checkMissingExecutableFails() {
    ScopedEnv env("FOXSDR_SHELL_OPEN_EXE", "/definitely/not/a/real/executable/foxsdr-test");
    const bool ok = cascade::gui::posixShellOpen("whatever");
    CHECK(!ok);
}

// THE NO-WAIT PROPERTY. A stand-in that sleeps for several seconds must not
// make posixShellOpen() take anywhere near that long to return - the call is
// only waiting on its own fork(), never on the grandchild that runs the
// stand-in.
void checkDoesNotWaitForTheChild() {
    const fs::path dir = scratchDir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path record = dir / "argv.log";
    const fs::path script = writeRecorderScript(dir, record, /*sleepSeconds=*/3);
    ScopedEnv env("FOXSDR_SHELL_OPEN_EXE", script.string());

    const auto start = std::chrono::steady_clock::now();
    const bool ok = cascade::gui::posixShellOpen("target");
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count();
    CHECK(ok);
    CHECK(elapsedMs < 1000);
    if (elapsedMs >= 1000) {
        std::printf("FAIL posixShellOpen took %lldms against a 3s stand-in - it waited\n",
                    static_cast<long long>(elapsedMs));
    }
    // The record only appears once the (still-sleeping) grandchild finishes,
    // proving the fast return above really did happen before the stand-in was
    // done - not that the stand-in was somehow instant.
    CHECK(waitForFile(record, /*timeoutMs=*/5000));

    fs::remove_all(dir, ec);
}

// THE NO-ZOMBIE PROPERTY. Reap every child of this test process once the
// grandchild has had time to exit, then confirm nothing of ours is left in
// a zombie state - the double fork must have handed the grandchild to
// init/subreaper rather than leaving it as this process's unreaped child.
void checkNoZombieLeftBehind() {
    const fs::path dir = scratchDir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path record = dir / "argv.log";
    const fs::path script = writeRecorderScript(dir, record);
    ScopedEnv env("FOXSDR_SHELL_OPEN_EXE", script.string());

    CHECK(cascade::gui::posixShellOpen("target"));
    CHECK(waitForFile(record));

    // Reap anything a stray wait would find (there should be nothing: the
    // immediate child was already reaped inside posixShellOpen(), and the
    // grandchild was reparented away). A non-blocking loop, bounded, so a
    // real bug (a zombie left behind) fails fast instead of hanging ctest.
    int reaped = 0;
    for (int i = 0; i < 100; ++i) {
        const pid_t r = waitpid(-1, nullptr, WNOHANG);
        if (r <= 0) { break; }
        ++reaped;
    }
    CHECK(reaped == 0);
    if (reaped != 0) {
        std::printf("FAIL %d unexpected child(ren) were still ours to reap\n", reaped);
    }

    fs::remove_all(dir, ec);
}

}  // namespace

#endif  // !defined(_WIN32)

int main() {
#if !defined(_WIN32)
    checkArgvReachesTheTarget();
    checkMissingExecutableFails();
    checkDoesNotWaitForTheChild();
    checkNoZombieLeftBehind();
    return testSummary("test_shell_open_posix");
#else
    // posixShellOpen() does not exist on Windows - AppWindow::shellOpen() uses
    // ShellExecuteA there instead, unchanged, and is out of scope for this
    // file. Say so rather than print "0 checks, 0 failed", which reads the
    // same as a test that ran (tests/test_usb_usbfs.cpp does the same).
    std::printf("test_shell_open_posix: SKIPPED (xdg-open path is POSIX-only; see "
                "test_shell_open for the shared watchdog bracket)\n");
    return 0;
#endif
}
