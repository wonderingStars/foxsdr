// No excuse the watchdog grants may outlast the freeze it is excusing.
//
// TWO EXCUSES, ONE BLIND SPOT: a WatchdogPause, and false-positive rule 2c ("a
// stalled GUI thread whose instruction pointer is in win32u.dll is pumping").
// The pause is dealt with first, the rule after it (blocks 6 and 7).
//
// THE PAUSE. HangWatchdog::pause() is released by the code that took it,
// and in every case that matters that code runs on the GUI thread - the very
// thread whose stall the watchdog exists to report:
//
//   - AppWindow::rescanPlugins() holds one across detachAndUnloadPlugins(),
//     which calls every plugin's destroy(): third-party code, and for a plugin
//     with a worker thread (Radar Sweep, Survey Engine ...) a join.
//   - gui/present_grace.hpp takes one when the display changes and promises it
//     is BOUNDED (HangWatchdog::kDisplayGraceMs, "a pause that never expires is
//     the watchdog switched off") - but its expiry is evaluated by update(),
//     called once per frame by the GUI thread. A GUI thread that stops in the
//     grace window never evaluates it, so the pause the header calls bounded
//     lasts exactly as long as the stall.
//   - gui::runShellOpen holds one for as long as ShellExecute takes.
//
// A freeze inside any of them is never reported: the watchdog is paused, and
// the thread that would un-pause it is the thread that is frozen. Field report
// 0.99.59: Radar Sweep running, the application stops responding, closed from
// the taskbar, and nothing from this user reaches the crash store.
//
// WHAT IS ASSERTED, with a real HangWatchdog and (for the first block) the real
// PresentGrace:
//   1. a stalled GUI thread inside the display-change grace IS reported once
//      the pause has outlasted its cap;
//   2. the same for a plain WatchdogPause held across a stall;
//   and the three things the cap must NOT do:
//   3. a pause shorter than the cap still excuses its stall (the plugin scan);
//   4. a pause held for much longer than the cap while the GUI thread keeps
//      beating (a minimised window) is still never reported;
//   5. each continuous pause gets its own cap - one released and retaken does
//      not inherit the first one's age.
//
// RULE 2c. The watchdog suppresses a stall whose GUI thread is executing inside
// win32u.dll, on the premise that the application's own loop only PeekMessages
// and so only a loop Windows owns can park the thread in win32u's waits. The
// premise is false: win32u.dll is the user-mode end of EVERY win32k and
// graphics-kernel (D3DKMT) system call. It exports 191 NtGdiDdDDI* stubs -
// Present, WaitForSynchronizationObject, WaitForVerticalBlankEvent, the calls a
// GPU driver's present and fence waits are made through - and NtUserMessageCall
// (a SendMessage to another thread's window) and NtUserGetClipboardData (which
// waits for the clipboard's owner). A thread frozen in any of them has its
// instruction pointer in win32u.dll, is declared "pumping", and is excused for
// as long as it stays frozen; the 0.96.4 display-stall classification, which
// lists win32u as a kernel wait, never gets to see it. On a laptop's integrated
// GPU under a new display stack that is not an exotic place to wedge.
//   6. the rule, with the module injected, excuses a stall for at most the cap;
//   7. a REAL deadlock - the "GUI thread" sent a message to a window whose owner
//      never pumps, read by the watchdog's real register peek - is reported.
//
// THE ONE PAUSE THE CAP MUST NOT TOUCH. The shell-open bracket (gui::runShellOpen,
// around ShellExecute) is paced by a PERSON: an elevation or SmartScreen prompt
// stays up for as long as the user takes to read it, and the 0.96.2 field report
// "hang ntdll.dll @ cascade::gui::AppWindow::launchInstaller" (docs/DIAGNOSTICS.md,
// "Shell calls are made under a watchdog pause") was the watchdog reporting
// exactly that. A cap on it would file that report again for any prompt left open
// past the cap. So there are two kinds of pause - HangWatchdog::pause(), paced by
// the application's own code and capped; and HangWatchdog::pauseUser(), paced by
// a person and not capped - and:
//   8. a stall under a USER-paced pause held for much longer than the cap is not
//      reported, and the pause still counts in pausesTaken();
//   9. an application-paced pause nested inside a user-paced one is not reported
//      while the user-paced one is held, however long; when the user-paced one is
//      released the application-paced one is measured from THAT moment, with a
//      fresh cap, and is then reported past it as block 2 shows it always is;
//  10. once the user-paced pause is released, a stall that goes on is reported
//      under the ordinary rules;
//  11. (source scan) the uncapped kind is wired to the shell-open bracket and to
//      nothing else in src/.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <thread>

#include "core/hang_watchdog.hpp"
#include "gui/present_grace.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;
using cascade::core::HangWatchdog;
using cascade::core::WatchdogPause;

namespace {

fs::path scratchDir(const std::string& tag) {
    const char* tmp = std::getenv("TEMP");
    if (tmp == nullptr || *tmp == '\0') { tmp = std::getenv("TMPDIR"); }
    const fs::path base = (tmp != nullptr && *tmp != '\0') ? fs::path(tmp) : fs::path(".");
#if defined(_WIN32)
    const unsigned long pid = ::GetCurrentProcessId();
#else
    const unsigned long pid = 0;
#endif
    return base / (std::string("cascade-excusecap-") + tag + "-" + std::to_string(pid));
}

void beatFor(HangWatchdog& w, unsigned ms) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        w.heartbeat();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void sleepMs(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// The watchdog under test: a short threshold and cap so the suite is seconds
// long, and the debugger / modal-loop / win32u rules off so nothing but the
// pause logic can be what keeps a report from being written.
constexpr unsigned kThresholdMs = 800;
constexpr unsigned kCapMs = 1500;

void arm(HangWatchdog& w, const fs::path& dir) {
    w.setSuppressionForTest(HangWatchdog::SuppressionForTest::NeverSuppress);
    w.setExcuseCapMs(kCapMs);
    w.start(dir.string(), kThresholdMs);
}

// The product reaches pauseUser()/resumeUser() through gui::ShellPauseHooks and
// never through a scope guard (the uncapped kind is meant to be hard to reach),
// so this guard is the test's own.
class UserPause {
public:
    explicit UserPause(HangWatchdog& w) : w_(w) { w_.pauseUser(); }
    ~UserPause() { w_.resumeUser(); }
    UserPause(const UserPause&) = delete;
    UserPause& operator=(const UserPause&) = delete;

private:
    HangWatchdog& w_;
};

// Long enough for the CAPPED kind to have been reported: the cap, the threshold
// that follows it, a few polls, and a second of margin.
constexpr unsigned kPastTheCapMs = kCapMs + kThresholdMs + 3 * HangWatchdog::kPollMs + 1000;

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Comments and string literals blanked to spaces (newlines kept): a mention in
// a comment or a log line is not a use. (The same blanking
// tests/test_converter_call_sites.cpp does.)
std::string liveCode(const std::string& s) {
    std::string out = s;
    enum { Code, Line, Block, Str, Chr } st = Code;
    for (std::size_t i = 0; i < out.size(); ++i) {
        const char c = s[i];
        const char n = i + 1 < s.size() ? s[i + 1] : '\0';
        switch (st) {
            case Code:
                if (c == '/' && n == '/') { st = Line; out[i] = ' '; }
                else if (c == '/' && n == '*') { st = Block; out[i] = ' '; }
                else if (c == '"') { st = Str; }
                else if (c == '\'') { st = Chr; }
                break;
            case Line:
                if (c == '\n') { st = Code; } else { out[i] = ' '; }
                break;
            case Block:
                if (c == '*' && n == '/') { out[i] = ' '; out[i + 1] = ' '; ++i; st = Code; }
                else if (c != '\n') { out[i] = ' '; }
                break;
            case Str:
                if (c == '\\') { out[i] = ' '; if (i + 1 < out.size()) { out[++i] = ' '; } }
                else if (c == '"') { st = Code; }
                else if (c != '\n') { out[i] = ' '; }
                break;
            case Chr:
                if (c == '\\') { out[i] = ' '; if (i + 1 < out.size()) { out[++i] = ' '; } }
                else if (c == '\'') { st = Code; }
                else { out[i] = ' '; }
                break;
        }
    }
    return out;
}

std::size_t countOf(const std::string& hay, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t at = hay.find(needle); at != std::string::npos;
         at = hay.find(needle, at + needle.size())) {
        ++n;
    }
    return n;
}

}  // namespace

int main() {
    std::error_code ec;

    // --- 1. the display-change grace, with the GUI thread stalled inside it ---
    {
        const fs::path dir = scratchDir("grace");
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        HangWatchdog w;
        arm(w, dir);
        beatFor(w, 300);  // a live frame loop

        // What AppWindow::run() does the frame the display changes: one update()
        // with displayChanged, which takes the pause. graceS is the production
        // 10 s; the stall below ends long before the grace would, which is the
        // point - the GUI thread is never going to call update() again.
        cascade::gui::PresentGrace grace([&w] { w.pause(); }, [&w] { w.resume(); }, 10.0);
        grace.update(0.0, /*displayChanged=*/true, /*notPresenting=*/false);
        CHECK(grace.paused());

        // The GUI thread is now deadlocked: no heartbeat, no update(). Cap
        // 1.5 s + threshold 0.8 s + a poll, with margin.
        sleepMs(kCapMs + kThresholdMs + 3 * HangWatchdog::kPollMs + 1500);
        std::printf("reports for a stall inside the display grace: %u\n", w.reportsWritten());
        CHECK(w.reportsWritten() == 1u);
        w.stop();
        grace.release();
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // --- 2. a plain pause held across a stall (the plugin rescan's shape) ----
    {
        const fs::path dir = scratchDir("held");
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        HangWatchdog w;
        arm(w, dir);
        beatFor(w, 300);
        {
            WatchdogPause hold(w);
            sleepMs(kCapMs + kThresholdMs + 3 * HangWatchdog::kPollMs + 1500);
            std::printf("reports for a stall under a held pause: %u\n", w.reportsWritten());
            CHECK(w.reportsWritten() == 1u);
        }
        w.stop();
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // --- 3. a pause SHORTER than the cap still excuses its stall -------------
    {
        const fs::path dir = scratchDir("short");
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        HangWatchdog w;
        arm(w, dir);
        beatFor(w, 300);
        {
            WatchdogPause hold(w);
            sleepMs(1100);  // longer than the 0.8 s threshold, inside the cap
        }
        CHECK(w.reportsWritten() == 0u);
        // ...and the pause did not disarm it: the same stall unpaused reports.
        beatFor(w, 300);
        sleepMs(kThresholdMs + 3 * HangWatchdog::kPollMs + 1000);
        CHECK(w.reportsWritten() == 1u);
        w.stop();
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // --- 4. a long pause with a BEATING GUI thread is never a hang ----------
    {
        const fs::path dir = scratchDir("beating");
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        HangWatchdog w;
        arm(w, dir);
        beatFor(w, 300);
        {
            // A minimised window: the pause is held for as long as that lasts
            // and the loop keeps turning. Three times the cap.
            WatchdogPause hold(w);
            beatFor(w, 3 * kCapMs);
            CHECK(w.reportsWritten() == 0u);
        }
        CHECK(w.reportsWritten() == 0u);
        w.stop();
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // --- 5. each CONTINUOUS pause gets its own cap ----------------------------
    {
        const fs::path dir = scratchDir("fresh");
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        HangWatchdog w;
        arm(w, dir);
        beatFor(w, 300);
        for (int i = 0; i < 3; ++i) {
            {
                WatchdogPause hold(w);
                sleepMs(1000);  // inside the cap each time; 3 s of pause in all
            }
            beatFor(w, 200);
        }
        CHECK(w.reportsWritten() == 0u);
        w.stop();
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // --- 6. rule 2c, with the module injected: excused, but not for ever ------
    {
        const fs::path dir = scratchDir("pump");
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        HangWatchdog w;
        arm(w, dir);
        w.setStalledModuleForTest("WIN32U.DLL");  // the loader's own casing varies
        beatFor(w, 300);
        // Inside the cap the stall is excused, exactly as before.
        sleepMs(kThresholdMs + HangWatchdog::kPollMs + 300);
        CHECK(w.reportsWritten() == 0u);
        // Past it, the same stall - never a heartbeat in between - is a hang.
        sleepMs(kCapMs + 3 * HangWatchdog::kPollMs + 1500);
        std::printf("reports for a stall in win32u past the cap: %u\n", w.reportsWritten());
        CHECK(w.reportsWritten() == 1u);
        // A thread that comes back and is merely PUMPING again starts afresh: a
        // second, short stall inside win32u is excused, not reported.
        beatFor(w, 300);
        sleepMs(kThresholdMs + HangWatchdog::kPollMs + 300);
        CHECK(w.reportsWritten() == 1u);
        w.stop();
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

#if defined(_WIN32)
    // --- 7. a real deadlock inside win32u, read by the real register peek ------
    //
    // Thread B owns a window and never pumps. Thread A - registered as the GUI
    // thread by its first heartbeat - SendMessages to that window and blocks in
    // NtUserMessageCall, which is in win32u.dll. Nothing is injected: the
    // watchdog runs in Normal mode, so the debugger test, GetGUIThreadInfo and
    // the OpenThread/SuspendThread/GetThreadContext peek are all the shipped ones.
    if (!::IsDebuggerPresent()) {
        const fs::path dir = scratchDir("sendmsg");
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        HangWatchdog w;
        w.setExcuseCapMs(kCapMs);
        w.start(dir.string(), kThresholdMs);  // Normal suppression

        std::atomic<HWND> victim{nullptr};
        std::atomic<bool> release{false};
        std::atomic<bool> sent{false};
        std::thread owner([&] {
            HWND h = ::CreateWindowExW(0, L"STATIC", L"excuse-cap victim", 0, 0, 0, 0, 0,
                                       HWND_MESSAGE, nullptr, ::GetModuleHandleW(nullptr),
                                       nullptr);
            victim.store(h);
            // Not pumping: it owns a window and sits on a flag.
            while (!release.load()) { ::Sleep(10); }
            // Released: pump once so the blocked SendMessage returns.
            MSG m;
            for (int i = 0; i < 200; ++i) {
                while (::PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
                    ::DispatchMessageW(&m);
                }
                ::Sleep(5);
            }
            if (h != nullptr) { ::DestroyWindow(h); }
        });
        while (victim.load() == nullptr) { ::Sleep(5); }

        std::thread gui([&] {
            w.heartbeat();  // this thread is now the GUI thread
            if (victim.load() != nullptr && victim.load() != HWND(-1)) {
                sent.store(true);
                ::SendMessageW(victim.load(), WM_USER + 1, 0, 0);  // blocks in win32u
            }
        });

        sleepMs(kCapMs + kThresholdMs + 3 * HangWatchdog::kPollMs + 2000);
        std::printf("reports for a real SendMessage deadlock: %u\n", w.reportsWritten());
        CHECK(sent.load());
        CHECK(w.reportsWritten() == 1u);
        // ...and the report is the one that says where: a hang, with the GUI
        // thread's top frame in win32u.dll - which is what the register peek
        // took for "pumping".
        {
            std::string text;
            std::ifstream in(w.lastReportPath(), std::ios::binary);
            text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            CHECK(text.find("kind: hang") != std::string::npos);
            const std::size_t gui = text.find("(gui, stalled)");
            CHECK(gui != std::string::npos);
            const std::size_t top = gui == std::string::npos ? gui : text.find("  ", gui);
            CHECK(top != std::string::npos && text.compare(top, 13, "  win32u.dll+") == 0);
        }

        release.store(true);
        gui.join();
        owner.join();
        w.stop();
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }
#endif

    // --- 8. a stall under a USER-paced pause is never reported, however long ---
    //
    // The shell-open bracket: ShellExecute is showing a consent prompt and the
    // person is reading it. Past the cap an application-paced pause is reported
    // (block 2); this one must not be.
    {
        const fs::path dir = scratchDir("user");
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        HangWatchdog w;
        arm(w, dir);
        beatFor(w, 300);
        const unsigned before = w.pausesTaken();
        {
            UserPause hold(w);
            sleepMs(kPastTheCapMs);
            std::printf("reports for a stall under a held user-paced pause: %u\n",
                        w.reportsWritten());
            CHECK(w.reportsWritten() == 0u);
            // ...and it is still a pause as far as `cascade --frames N` and
            // tests/test_diag_hang.cpp are concerned.
            CHECK(w.pausesTaken() == before + 1u);
        }
        w.stop();
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // --- 9. an APPLICATION-paced pause nested inside a user-paced one -----------
    {
        const fs::path dir = scratchDir("nested");
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        HangWatchdog w;
        arm(w, dir);
        beatFor(w, 300);
        {
            // A plugin rescan that happens to run inside a long shell call (the
            // order is arbitrary; both are held across the same stall).
            WatchdogPause app(w);
            {
                UserPause user(w);
                sleepMs(kPastTheCapMs);
                std::printf("reports for an app-paced pause inside a held user-paced one: %u\n",
                            w.reportsWritten());
                CHECK(w.reportsWritten() == 0u);
            }
            // The user-paced pause has just been released and the application-
            // paced one has been held for far longer than the cap. It must NOT be
            // reported on the strength of that age: its cap starts now. Without a
            // fresh start the report lands within about a second of the release;
            // with one, nothing can land before cap + threshold has passed.
            sleepMs(1700);
            std::printf("reports 1.7 s after the release, app-paced pause still held: %u\n",
                        w.reportsWritten());
            CHECK(w.reportsWritten() == 0u);
            // ...and it is still capped from that moment: the freeze goes on, the
            // pause is still held, and past the fresh cap it is reported once.
            sleepMs(kPastTheCapMs);
            std::printf("reports once the fresh cap has run out: %u\n", w.reportsWritten());
            CHECK(w.reportsWritten() == 1u);
        }
        w.stop();
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // --- 10. after the user-paced pause is released, the ordinary rules apply ----
    {
        const fs::path dir = scratchDir("released");
        fs::remove_all(dir, ec);
        fs::create_directories(dir, ec);
        HangWatchdog w;
        arm(w, dir);
        beatFor(w, 300);
        {
            UserPause hold(w);
            sleepMs(kPastTheCapMs);
            CHECK(w.reportsWritten() == 0u);
        }
        // The GUI thread never came back (no heartbeat since the pause began):
        // the shell call returned and the window is still frozen. That is a hang
        // like any other, written one threshold after the release.
        CHECK(w.reportsWritten() == 0u);
        sleepMs(kThresholdMs + 3 * HangWatchdog::kPollMs + 1000);
        std::printf("reports for a stall that outlasts a released user-paced pause: %u\n",
                    w.reportsWritten());
        CHECK(w.reportsWritten() == 1u);
        w.stop();
        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }

    // --- 11. SOURCE SCAN: the uncapped kind belongs to the shell-open bracket ----
    //
    // Nothing in a test can press the buttons that call ShellExecute, and nothing
    // stops the next person who wants "a pause that does not expire" from reaching
    // for the uncapped one - which is the watchdog switched off for as long as the
    // thing it waits on lasts. So the wiring is held to the source that ships: the
    // only code outside the watchdog itself that names pauseUser/resumeUser is
    // AppWindow::watchdogShellHooks(), the hooks every runShellOpen() call site is
    // given.
    {
        const fs::path src = fs::path(CASCADE_SOURCE_DIR) / "src";
        std::set<std::string> users;
        int scanned = 0;
        for (fs::recursive_directory_iterator it(src, ec), end; !ec && it != end;
             it.increment(ec)) {
            if (!it->is_regular_file()) { continue; }
            const std::string ext = it->path().extension().string();
            if (ext != ".cpp" && ext != ".hpp" && ext != ".h") { continue; }
            ++scanned;
            const std::string code = liveCode(readFile(it->path()));
            if (code.find("pauseUser") != std::string::npos ||
                code.find("resumeUser") != std::string::npos ||
                code.find("WatchdogUserPause") != std::string::npos) {
                users.insert(fs::relative(it->path(), src, ec).generic_string());
            }
        }
        std::printf("source scan: %d files under src/, files naming the uncapped kind:", scanned);
        for (const std::string& u : users) { std::printf(" %s", u.c_str()); }
        std::printf("\n");
        CHECK(scanned > 100);  // a scan that found no tree checks nothing

        // The watchdog defines it; app_window.cpp wires it; nothing else may.
        const std::set<std::string> expected{"core/hang_watchdog.cpp", "core/hang_watchdog.hpp",
                                             "gui/app_window.cpp"};
        CHECK(users == expected);

        // Inside app_window.cpp, every mention is within watchdogShellHooks(),
        // once for each side of the bracket, and both shell call sites go
        // through runShellOpen() with those hooks.
        const std::string win = liveCode(readFile(src / "gui" / "app_window.cpp"));
        const std::string fn = "AppWindow::watchdogShellHooks()";
        const std::size_t begin = win.find(fn);
        const std::size_t close = begin == std::string::npos ? begin : win.find("\n}", begin);
        CHECK(begin != std::string::npos && close != std::string::npos);
        const std::string body = (begin == std::string::npos || close == std::string::npos)
                                     ? std::string()
                                     : win.substr(begin, close - begin);
        CHECK(countOf(body, "hooks.pause = [this] { watchdog_.pauseUser(); };") == 1u);
        CHECK(countOf(body, "hooks.resume = [this] { watchdog_.resumeUser(); };") == 1u);
        CHECK(countOf(win, "pauseUser") == 1u);
        CHECK(countOf(win, "resumeUser") == 1u);
        CHECK(countOf(win, "WatchdogUserPause") == 0u);
        CHECK(countOf(win, "runShellOpen(watchdogShellHooks(),") == 2u);
        // The application-paced sites are still plain pauses: the audio and
        // microphone opens and the display grace.
        CHECK(countOf(win, "watchdog_.pause()") >= 3u);
    }

    return testSummary("test_excuse_cap");
}
