/*
 * OPENING A LINK OR A FOLDER ON ANDROID: THE REFUSAL, HELD TO ITS WORD.
 *
 * WHY THIS IS A TEST AND NOT JUST A COMMENT. gui/shell_open.hpp's POSIX body
 * forks twice and execs xdg-open, and on Android that would "work" in the
 * worst way available: xdg-open is not there, so the grandchild's execvp
 * fails, the pipe reports the errno, and the function answers false - the
 * right answer, reached by forking a multithreaded process on a phone and
 * then failing. The Android branch answers false immediately instead, and the
 * difference between the two is invisible from the return value alone. So
 * what is checked here is that the branch is TAKEN: with
 * FOXSDR_SHELL_OPEN_EXE pointed at a program that certainly exists and
 * certainly succeeds, the POSIX body would return TRUE, and the Android
 * branch still returns false.
 *
 * WHERE IT RUNS. In the on-device subset (tests/CMakeLists.txt's
 * CASCADE_ANDROID_TEST_NAMES), pushed to the emulator by
 * tools/run-android-tests.sh. On the host-subset build - the same list
 * compiled natively with no NDK - there is no __ANDROID__ and nothing here
 * can be observed, so it SKIPS and says so rather than passing quietly: a
 * green "0 failed" that checked nothing is the outcome this harness's
 * SKIP_LINUX exists to make impossible.
 *
 * BREAK-IT CHECK, run on the emulator while writing this: with the Android
 * branch removed from shell_open.hpp so the POSIX body is compiled instead,
 * the FOXSDR_SHELL_OPEN_EXE case returns true and this test fails at the line
 * named "even a stand-in that would succeed".
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include "gui/shell_open.hpp"
#include "test_check.hpp"

#include <cstdlib>
#include <cstring>

int main() {
#if defined(__ANDROID__)
    using cascade::gui::androidShellOpenReason;
    using cascade::gui::posixShellOpen;

    // The plain case: a URL a user might press in Settings.
    CHECK(!posixShellOpen("https://foxsdr.com/privacy"));
    // A folder, which is the other half of what shellOpen is asked for.
    CHECK(!posixShellOpen("/data/data/com.foxsdr/files/state/foxsdr/crashes"));
    // An empty target is not special-cased into a success.
    CHECK(!posixShellOpen(""));

    // EVEN A STAND-IN THAT WOULD SUCCEED. /system/bin/true exists on every
    // Android build and exits 0, so the POSIX body would exec it and answer
    // true. This is the check that proves the branch is taken rather than
    // that xdg-open happens to be missing.
    setenv("FOXSDR_SHELL_OPEN_EXE", "/system/bin/true", 1);
    CHECK(!posixShellOpen("https://foxsdr.com"));
    unsetenv("FOXSDR_SHELL_OPEN_EXE");

    // The explanation the refusal carries. Checked for substance, not for
    // exact wording: it has to say it cannot, and say what is missing.
    const char* why = androidShellOpenReason();
    CHECK(why != nullptr);
    CHECK(why != nullptr && std::strlen(why) > 20);
    CHECK(why != nullptr && std::strstr(why, "intent") != nullptr);
#else
    SKIP_LINUX(
        "posixShellOpen's Android refusal is compiled only under __ANDROID__; this build "
        "has the POSIX fork/exec body, which tests/test_shell_open_posix.cpp covers");
#endif
    return testSummary("test_shell_open_android");
}
