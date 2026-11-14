// test_window_handover.cpp - LEGACY IS A CHOICE INSIDE THE NEW WINDOW: the rules every hand-over follows (core/window_handover.*; engine fixes 7).
//
// Nothing here starts a program: a stand-in starter records what would be started (setStarterForTest) and the record seam is exercised as a drive script would. The two REAL hand-overs
// (one each way, on the hidden desktop, on a settings folder of their own) are src/slint/tests/drive_handover_run.cpp's.
//
//   H1  THE CHOICE: absent / garbage / too long read as "new"; legacy and new round-trip; written whole (no .tmp left); no settings file keeps nothing
//   H2  THE MARK on a started window: parsed once and removed from the environment; every malformed form is "not present"
//   H3  THE SENTENCES: one per failure, in the window the person is in now, from the note the other window left
//   H4  THE QUESTION before leaving while something runs, one plain sentence each way; none when nothing runs
//   H5  PREFLIGHT: the other program missing is a sentence and nothing started; present is none
//   H6  THE LOCK ORDER: with the settings lock still held by this process nothing is started but this window's own program (marked "locked"); with it let go the target is started
//       once, marked, and the choice is written. The same through the object main() declares first: the starter finds the lock FREE because the service in the inner scope is gone
//   H7  EVERY FAILURE: the program missing (after the window went), would not start (Windows error carried to the sentence), the lock still held; each returns the person to a window,
//       with the sentence in the mark; both programs failing is reported, not hidden; the remembered choice stays what it was after a failure
//   H8  THE RECORD SEAM: one line saying what would be started, after the lock check, and nothing started; a refusal is one line too
//   H9  WHAT THE STARTED PROGRAM IS GIVEN: --safe-plugins carried, the test's extra arguments appended
//   H10 THE OBJECT: not asked = nothing starts when it is destroyed; asked = started once, then
//   H11 THE NEW WINDOW'S SIDE (src/slint/handover_host.hpp): the Legacy entry with nothing running goes at once; with a recording or a patch it asks first (Switch / Stay here);
//       the program missing leaves the window where it is with the sentence; a sentence left by the other window is shown
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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
#endif

#include "core/config_lock.hpp"
#include "core/window_handover.hpp"
#include "slint/handover_host.hpp"   // header only: the new window's side, over three std::functions
#include "test_check.hpp"

namespace fs = std::filesystem;
namespace h = cascade::core::handover;

namespace {

void setEnv(const char* k, const std::string& v) {
#if defined(_WIN32)
    ::SetEnvironmentVariableA(k, v.empty() ? nullptr : v.c_str());
    _putenv_s(k, v.c_str());
#else
    if (v.empty()) { unsetenv(k); } else { setenv(k, v.c_str(), 1); }
#endif
}

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// The stand-in starter: records each call, and answers as told.
struct Call {
    std::string exe, args, marker;
    bool lockFree = false;   // what the lock was AT THE MOMENT of the call, asked from inside the starter
};
std::vector<Call> g_calls;
std::string g_config;
std::vector<bool> g_answers;   // per call: true = started; empty = every call starts
unsigned long g_error = 0;

bool fakeStarter(const std::string& exe, const std::string& args, const std::string& marker, unsigned long& error) {
    Call c;
    c.exe = exe;
    c.args = args;
    c.marker = marker;
    c.lockFree = h::probeLock(g_config) == h::Lock::Free;
    const std::size_t i = g_calls.size();
    g_calls.push_back(c);
    const bool ok = i < g_answers.size() ? g_answers[i] : true;
    if (!ok) { error = g_error; }
    return ok;
}

fs::path g_dir;
fs::path g_exeNew, g_exeLegacy;

void reset(bool withExes = true) {
    g_calls.clear();
    g_answers.clear();
    g_error = 0;
    setEnv(h::kRecordEnv, "");
    setEnv(h::kChildArgsEnv, "");
    setEnv(h::kFromEnv, "");
    setEnv(h::kNewExeEnv, withExes ? g_exeNew.string() : (g_dir / "no-such-new.exe").string());
    setEnv(h::kLegacyExeEnv, withExes ? g_exeLegacy.string() : (g_dir / "no-such-legacy.exe").string());
}

void testChoice() {
    std::printf("[H1] the choice\n");
    const std::string cfg = (g_dir / "h1" / "config.json").string();
    fs::create_directories(g_dir / "h1");
    CHECK(h::choicePath(cfg) == (g_dir / "h1" / "window-choice").string());
    CHECK(h::choicePath("").empty());
    CHECK(h::readChoice(cfg) == h::Window::New);   // nothing yet
    CHECK(h::writeChoice(cfg, h::Window::Legacy));
    CHECK(h::readChoice(cfg) == h::Window::Legacy);
    CHECK(readFile(h::choicePath(cfg)) == "legacy\n");
    CHECK(!fs::exists(h::choicePath(cfg) + ".tmp"));   // written whole, renamed into place
    CHECK(h::writeChoice(cfg, h::Window::New));
    CHECK(h::readChoice(cfg) == h::Window::New);
    { std::ofstream(h::choicePath(cfg), std::ios::binary | std::ios::trunc) << "  LEGACY \r\n"; }
    CHECK(h::readChoice(cfg) == h::Window::Legacy);   // case and blanks do not matter
    { std::ofstream(h::choicePath(cfg), std::ios::binary | std::ios::trunc) << "legacy but not really\n"; }
    CHECK(h::readChoice(cfg) == h::Window::New);      // anything else is the new window
    { std::ofstream(h::choicePath(cfg), std::ios::binary | std::ios::trunc) << std::string(5000, 'x'); }
    CHECK(h::readChoice(cfg) == h::Window::New);
    { std::ofstream(h::choicePath(cfg), std::ios::binary | std::ios::trunc); }
    CHECK(h::readChoice(cfg) == h::Window::New);      // empty
    CHECK(!h::writeChoice("", h::Window::Legacy));   // a run with no settings file keeps nothing
    CHECK(h::readChoice("") == h::Window::New);
    // a folder that takes no file: the "folder" is a file
    { std::ofstream(g_dir / "h1" / "afile") << "x"; }
    CHECK(!h::writeChoice((g_dir / "h1" / "afile" / "config.json").string(), h::Window::Legacy));
}

void testMarker() {
    std::printf("[H2] the mark\n");
    setEnv(h::kFromEnv, "");
    CHECK(!h::takeMarker().present);
    setEnv(h::kFromEnv, "1234:legacy:");
    h::Marker m = h::takeMarker();
    CHECK(m.present && m.parentPid == 1234 && m.from == h::Window::Legacy && m.note.empty());
    CHECK(std::getenv(h::kFromEnv) == nullptr || std::string(std::getenv(h::kFromEnv)).empty());   // removed: nothing this process starts inherits it
    CHECK(!h::takeMarker().present);
    setEnv(h::kFromEnv, "77:new:failed:193");
    m = h::takeMarker();
    CHECK(m.present && m.parentPid == 77 && m.from == h::Window::New && m.note == "failed:193");
    setEnv(h::kFromEnv, "5:new");
    m = h::takeMarker();
    CHECK(m.present && m.note.empty());
    for (const char* bad : {"x:new:", "12x:new:", ":new:", "12", "12:", "12:other:", "12:NEW:", "::", "abc"}) {
        setEnv(h::kFromEnv, bad);
        CHECK(!h::takeMarker().present);
        CHECK(std::getenv(h::kFromEnv) == nullptr || std::string(std::getenv(h::kFromEnv)).empty());
    }
}

void testSentences() {
    std::printf("[H3] the sentences\n");
    h::Marker m;
    m.present = true;
    CHECK(h::noteSentence(m, h::Window::New).empty());   // no note
    m.note = "missing";
    const std::string a = h::noteSentence(m, h::Window::New);     // the person is in the new window, the Legacy one did not open
    CHECK(a.find("Legacy window could not be opened") != std::string::npos && a.find("cascade.exe") != std::string::npos && a.find("still in the new window") != std::string::npos);
    const std::string b = h::noteSentence(m, h::Window::Legacy);
    CHECK(b.find("new window could not be opened") != std::string::npos && b.find("foxsdr-slint.exe") != std::string::npos && b.find("still in the Legacy window") != std::string::npos);
    m.note = "failed:193";
    const std::string c = h::noteSentence(m, h::Window::New);
    CHECK(c.find("Legacy window could not be started (Windows error 193)") != std::string::npos && c.find("back in the new window") != std::string::npos);
    const std::string d = h::noteSentence(m, h::Window::Legacy);
    CHECK(d.find("new window could not be started (Windows error 193)") != std::string::npos && d.find("back in the Legacy window") != std::string::npos);
    m.note = "locked";
    CHECK(h::noteSentence(m, h::Window::New).find("settings file was still in use") != std::string::npos);
    CHECK(h::noteSentence(m, h::Window::Legacy).find("new window was not started") != std::string::npos);
    m.note = "nonsense";
    CHECK(h::noteSentence(m, h::Window::New).empty());
    m.present = false;
    m.note = "missing";
    CHECK(h::noteSentence(m, h::Window::New).empty());
}

void testBusy() {
    std::printf("[H4] the question\n");
    h::Busy none;
    CHECK(!none.any());
    CHECK(h::busyQuestion(none, h::Window::Legacy).empty());
    h::Busy rec;
    rec.recording = true;
    h::Busy pat;
    pat.patch = true;
    h::Busy both;
    both.recording = both.patch = true;
    CHECK(h::busyQuestion(rec, h::Window::Legacy) == "A recording is running. Switching to the Legacy window finishes and saves it first.");
    CHECK(h::busyQuestion(pat, h::Window::Legacy) == "A patch is running. Switching to the Legacy window stops it first.");
    CHECK(h::busyQuestion(both, h::Window::Legacy).find("recording and a patch") != std::string::npos);
    CHECK(h::busyQuestion(rec, h::Window::New).find("the new window") != std::string::npos);
    CHECK(h::busyQuestion(pat, h::Window::New).find("the new window") != std::string::npos);
    CHECK(h::busyQuestion(both, h::Window::New).find("the new window") != std::string::npos);
}

void testPreflight() {
    std::printf("[H5] preflight\n");
    reset(true);
    CHECK(h::preflight(h::Window::Legacy).empty() && h::preflight(h::Window::New).empty());
    reset(false);
    CHECK(h::preflight(h::Window::Legacy).find("could not be opened") != std::string::npos);
    CHECK(h::preflight(h::Window::New).find("could not be opened") != std::string::npos);
    CHECK(g_calls.empty());
}

h::Request request(h::Window from, h::Window target, const std::string& cfg) {
    h::Request r;
    r.from = from;
    r.target = target;
    r.configPath = cfg;
    return r;
}

void testLockOrder() {
    std::printf("[H6] the lock order\n");
    reset(true);
    fs::create_directories(g_dir / "h6");
    const std::string cfg = (g_dir / "h6" / "config.json").string();
    g_config = cfg;
    h::setStarterForTest(&fakeStarter);
    // (a) THE LOCK STILL HELD BY THIS PROCESS: the target is not started; this window's own program is, marked "locked"
    {
        cascade::core::ConfigLock lock;
        CHECK(lock.acquire(cfg, "test") == cascade::core::ConfigLock::State::Held);
        CHECK(h::probeLock(cfg) == h::Lock::HeldBySelf);
        const h::Outcome o = h::launch(request(h::Window::New, h::Window::Legacy, cfg));
        CHECK(!o.started && o.returned);
        CHECK(o.sentence.find("settings file was still in use") != std::string::npos);
        CHECK(g_calls.size() == 1);
        if (g_calls.size() == 1) {
            CHECK(g_calls[0].exe == g_exeNew.string());   // the NEW window's own program again, never cascade.exe
            CHECK(g_calls[0].marker.find(":legacy:locked") != std::string::npos);
        }
        CHECK(h::readChoice(cfg) == h::Window::New);   // the remembered choice is untouched
    }
    // (b) LET GO: the target is started, once, marked from the new window, and the choice is written
    g_calls.clear();
    CHECK(h::probeLock(cfg) == h::Lock::Free);
    {
        const h::Outcome o = h::launch(request(h::Window::New, h::Window::Legacy, cfg));
        CHECK(o.started && !o.returned && o.sentence.empty());
        CHECK(g_calls.size() == 1);
        if (g_calls.size() == 1) {
            CHECK(g_calls[0].exe == g_exeLegacy.string());
            CHECK(g_calls[0].marker.find(":new:") != std::string::npos && g_calls[0].marker.back() == ':');   // "<pid>:new:" - no note
            CHECK(g_calls[0].lockFree);   // inside the starter: the lock is free at the moment the program is started
        }
        CHECK(h::readChoice(cfg) == h::Window::Legacy);
    }
    // (d) THROUGH THE OBJECT main() DECLARES FIRST: declared before the "service", destroyed after it, so the starter finds the lock free
    g_calls.clear();
    {
        h::HandoverAtExit atExit;
        {
            cascade::core::ConfigLock service;   // stands for the engine service holding the settings lock
            CHECK(service.acquire(cfg, "test") == cascade::core::ConfigLock::State::Held);
            atExit.set(request(h::Window::Legacy, h::Window::New, cfg));
            CHECK(g_calls.empty());   // asking starts nothing
        }   // the service is gone, the lock with it
        CHECK(g_calls.empty());   // and nothing yet: the object is still alive
    }
    CHECK(g_calls.size() == 1);
    if (g_calls.size() == 1) {
        CHECK(g_calls[0].exe == g_exeNew.string() && g_calls[0].lockFree);
        CHECK(g_calls[0].marker.find(":legacy:") != std::string::npos);
    }
    CHECK(h::readChoice(cfg) == h::Window::New);   // the way back is remembered too
    h::setStarterForTest(nullptr);
}

void testFailures() {
    std::printf("[H7] every failure\n");
    fs::create_directories(g_dir / "h7");
    const std::string cfg = (g_dir / "h7" / "config.json").string();
    g_config = cfg;
    h::setStarterForTest(&fakeStarter);
    CHECK(h::writeChoice(cfg, h::Window::New));
    // (a) THE PROGRAM IS NOT THERE any more: the person is given their own window back, with the sentence in its mark
    reset(false);
    {
        setEnv(h::kNewExeEnv, g_exeNew.string());   // this window's own program is there; the Legacy one is not
        const h::Outcome o = h::launch(request(h::Window::New, h::Window::Legacy, cfg));
        CHECK(!o.started && o.returned);
        CHECK(o.sentence.find("could not be opened") != std::string::npos);
        CHECK(g_calls.size() == 1 && g_calls[0].marker.find(":legacy:missing") != std::string::npos && g_calls[0].exe == g_exeNew.string());
        CHECK(h::readChoice(cfg) == h::Window::New);
    }
    // (b) THE PROGRAM WOULD NOT START: the Windows error is carried to the sentence the returned window shows
    reset(true);
    g_answers = {false, true};
    g_error = 193;
    {
        const h::Outcome o = h::launch(request(h::Window::New, h::Window::Legacy, cfg));
        CHECK(!o.started && o.returned);
        CHECK(o.sentence.find("(Windows error 193)") != std::string::npos);
        CHECK(g_calls.size() == 2);
        if (g_calls.size() == 2) {
            CHECK(g_calls[0].exe == g_exeLegacy.string());
            CHECK(g_calls[1].exe == g_exeNew.string() && g_calls[1].marker.find(":legacy:failed:193") != std::string::npos);
            // the mark the returned window reads gives the same sentence, in ITS words
            setEnv(h::kFromEnv, g_calls[1].marker);
            const h::Marker m = h::takeMarker();
            CHECK(m.present && h::noteSentence(m, h::Window::New).find("(Windows error 193)") != std::string::npos);
        }
        CHECK(h::readChoice(cfg) == h::Window::New);
    }
    // (c) THE OTHER WAY: Legacy asked for the new window and it would not start
    reset(true);
    g_answers = {false, true};
    g_error = 5;
    {
        const h::Outcome o = h::launch(request(h::Window::Legacy, h::Window::New, cfg));
        CHECK(!o.started && o.returned && o.sentence.find("new window could not be started (Windows error 5)") != std::string::npos);
        CHECK(g_calls.size() == 2 && g_calls[1].exe == g_exeLegacy.string());
    }
    // (d) NEITHER PROGRAM STARTS: said, not hidden (there is nothing more to do from here)
    reset(true);
    g_answers = {false, false};
    g_error = 1260;
    {
        const h::Outcome o = h::launch(request(h::Window::New, h::Window::Legacy, cfg));
        CHECK(!o.started && !o.returned && !o.sentence.empty());
        CHECK(g_calls.size() == 2);
    }
    h::setStarterForTest(nullptr);
}

void testRecord() {
    std::printf("[H8] the record seam\n");
    fs::create_directories(g_dir / "h8");
    const std::string cfg = (g_dir / "h8" / "config.json").string();
    const std::string rec = (g_dir / "h8" / "record.txt").string();
    g_config = cfg;
    h::setStarterForTest(&fakeStarter);
    reset(true);
    setEnv(h::kRecordEnv, rec);
    fs::remove(rec);
    const h::Outcome o = h::launch(request(h::Window::New, h::Window::Legacy, cfg));
    CHECK(o.started && o.recorded && !o.returned);
    CHECK(g_calls.empty());   // nothing was started
    const std::string text = readFile(rec);
    CHECK(text.find("launch target=legacy program=cascade.exe from=new lock=free parent=") != std::string::npos);
    CHECK(text.find("settings=" + cfg) != std::string::npos);
    CHECK(std::count(text.begin(), text.end(), '\n') == 1);
    CHECK(h::readChoice(cfg) == h::Window::Legacy);   // the saved choice
    // the lock check comes first even here: held by this process = a refusal line, no launch line
    fs::remove(rec);
    {
        cascade::core::ConfigLock lock;
        CHECK(lock.acquire(cfg, "test") == cascade::core::ConfigLock::State::Held);
        const h::Outcome r = h::launch(request(h::Window::New, h::Window::Legacy, cfg));
        CHECK(!r.started && r.recorded);
        const std::string t2 = readFile(rec);
        CHECK(t2.find("refused target=legacy lock=held-by-self") != std::string::npos && t2.find("launch ") == std::string::npos);
    }
    // the program missing: a refusal too
    fs::remove(rec);
    reset(false);
    setEnv(h::kRecordEnv, rec);
    const h::Outcome m = h::launch(request(h::Window::New, h::Window::Legacy, cfg));
    CHECK(!m.started && m.recorded && !m.sentence.empty());
    CHECK(readFile(rec).find("refused target=legacy reason=missing") != std::string::npos);
    setEnv(h::kRecordEnv, "");
    h::setStarterForTest(nullptr);
}

void testArgs() {
    std::printf("[H9] what the started program is given\n");
    fs::create_directories(g_dir / "h9");
    const std::string cfg = (g_dir / "h9" / "config.json").string();
    g_config = cfg;
    h::setStarterForTest(&fakeStarter);
    reset(true);
    h::Request r = request(h::Window::New, h::Window::Legacy, cfg);
    (void)h::launch(r);
    CHECK(g_calls.size() == 1 && g_calls[0].args.empty());
    g_calls.clear();
    r.safePlugins = true;
    (void)h::launch(r);
    CHECK(g_calls.size() == 1 && g_calls[0].args == "--safe-plugins");
    g_calls.clear();
    setEnv(h::kChildArgsEnv, "--frames 60");
    (void)h::launch(r);
    CHECK(g_calls.size() == 1 && g_calls[0].args == "--safe-plugins --frames 60");
    reset(true);
    h::setStarterForTest(nullptr);
}

void testObject() {
    std::printf("[H10] the object\n");
    fs::create_directories(g_dir / "h10");
    g_config = (g_dir / "h10" / "config.json").string();
    h::setStarterForTest(&fakeStarter);
    reset(true);
    {
        h::HandoverAtExit never;
        CHECK(!never.pending());
    }
    CHECK(g_calls.empty());   // not asked, nothing starts
    {
        h::HandoverAtExit asked;
        asked.set(request(h::Window::New, h::Window::Legacy, g_config));
        CHECK(asked.pending());
        CHECK(g_calls.empty());
    }
    CHECK(g_calls.size() == 1);
    h::setStarterForTest(nullptr);
}

void testHost() {
    std::printf("[H11] the new window's side\n");
    fs::create_directories(g_dir / "h11");
    const std::string cfg = (g_dir / "h11" / "config.json").string();
    g_config = cfg;
    h::setStarterForTest(&fakeStarter);
    reset(true);
    struct Seen {
        int quits = 0, shows = 0, hides = 0;
        foxsdr::slinthost::HandoverHost::Sheet last;
        h::Busy busy;
    } seen;
    auto make = [&](h::HandoverAtExit& ex) {
        foxsdr::slinthost::HandoverHost::Hooks hk;
        hk.busy = [&] { return seen.busy; };
        hk.quit = [&] { ++seen.quits; };
        hk.show = [&](const foxsdr::slinthost::HandoverHost::Sheet& sh) { ++seen.shows; seen.last = sh; };
        hk.hide = [&] { ++seen.hides; };
        return foxsdr::slinthost::HandoverHost(hk, ex, cfg, false);
    };
    // (a) NOTHING RUNNING: it goes at once - the loop is told to end, the launch waits for the object
    {
        h::HandoverAtExit ex;
        foxsdr::slinthost::HandoverHost host = make(ex);
        host.choose();
        CHECK(host.chosen() && seen.quits == 1 && seen.shows == 0 && ex.pending());
        CHECK(g_calls.empty());
        host.choose();   // a second press changes nothing
        CHECK(seen.quits == 1);
    }
    CHECK(g_calls.size() == 1 && g_calls[0].exe == g_exeLegacy.string());
    // (b) A RECORDING IS RUNNING: it asks, in the plain sentence; Stay here goes nowhere; Switch goes
    g_calls.clear();
    seen = Seen{};
    seen.busy.recording = true;
    {
        h::HandoverAtExit ex;
        foxsdr::slinthost::HandoverHost host = make(ex);
        host.choose();
        CHECK(!host.chosen() && seen.quits == 0 && seen.shows == 1 && !ex.pending());
        CHECK(seen.last.text == "A recording is running. Switching to the Legacy window finishes and saves it first.");
        CHECK(seen.last.goLabel == "Switch" && seen.last.stayLabel == "Stay here");
        host.cancel();
        CHECK(!host.chosen() && seen.hides == 1 && seen.quits == 0);
        host.confirm();   // a stray confirm after a cancel decides nothing
        CHECK(!host.chosen());
        host.choose();
        host.confirm();
        CHECK(host.chosen() && seen.quits == 1 && ex.pending());
    }
    CHECK(g_calls.size() == 1);
    // (c) A PATCH IS RUNNING: the same
    g_calls.clear();
    seen = Seen{};
    seen.busy.patch = true;
    {
        h::HandoverAtExit ex;
        foxsdr::slinthost::HandoverHost host = make(ex);
        host.choose();
        CHECK(seen.shows == 1 && seen.last.text.find("A patch is running") != std::string::npos && !host.chosen());
    }
    CHECK(g_calls.empty());
    // (d) THE LEGACY PROGRAM IS NOT THERE: the window stays, with the sentence and one key
    seen = Seen{};
    reset(false);
    setEnv(h::kNewExeEnv, g_exeNew.string());
    {
        h::HandoverAtExit ex;
        foxsdr::slinthost::HandoverHost host = make(ex);
        host.choose();
        CHECK(!host.chosen() && seen.quits == 0 && !ex.pending() && seen.shows == 1);
        CHECK(seen.last.text.find("could not be opened") != std::string::npos && seen.last.goLabel.empty() && seen.last.stayLabel == "OK");
    }
    CHECK(g_calls.empty());
    // (e) A SENTENCE THE OTHER WINDOW LEFT is shown; nothing at all when there is none
    seen = Seen{};
    {
        h::HandoverAtExit ex;
        foxsdr::slinthost::HandoverHost host = make(ex);
        host.showNote("");
        CHECK(seen.shows == 0);
        host.showNote("The Legacy window could not be started (Windows error 5). You are back in the new window.");
        CHECK(seen.shows == 1 && seen.last.goLabel.empty());
    }
    h::setStarterForTest(nullptr);
}

}  // namespace

int main() {
    std::printf("test_window_handover\n");
    g_dir = fs::temp_directory_path() / ("cascade-handover-" + std::to_string(static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count())));
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir, ec);
    g_exeNew = g_dir / "foxsdr-slint.exe";
    g_exeLegacy = g_dir / "cascade.exe";
    { std::ofstream(g_exeNew) << "stand-in"; }
    { std::ofstream(g_exeLegacy) << "stand-in"; }
    testChoice();
    testMarker();
    testSentences();
    testBusy();
    testPreflight();
    testLockOrder();
    testFailures();
    testRecord();
    testArgs();
    testObject();
    testHost();
    fs::remove_all(g_dir, ec);
    return testSummary("test_window_handover");
}
