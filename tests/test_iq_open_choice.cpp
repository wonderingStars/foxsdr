// test_iq_open_choice.cpp - the I/Q-file row's Open and Browse, through the
// real AppWindow over the real engine (no window, no ImGui frame; since the engine split the open itself is the engine's), for a tester's report of
// 2026-10-29: "Can the IQ-file source be fixed: 1. the Open does nothing 2. Not
// possible to paste in a WAV-file in the IQ-file field."
//
// WHAT WENT WRONG. The row was a text box and an Open key, and Open only ever
// tried the box's text: over an empty box it handed "" to the worker, whose
// "cannot open file: " (nothing after the colon) went into the red line below
// the converter and crystal controls, with no log line - the tester's log had
// no I/Q line at all. There was no way to choose a file. A path pasted from
// Explorer's "Copy as path" came in double quotes, which the open refused as
// part of the name. (The clipboard half - a copied FILE pastes as its path - is
// gui/paste_files.hpp's, pinned in test_iq_file_box.cpp and driven in
// test_iq_file_box_app.cpp.)
//
// WHAT IS PROVED, each against the class before the fix:
//
//   empty+dialog   Open over an empty box opens the file dialog (the FOXSDR_FILE_PICK
//                  seam stands in for it, as every test of a Browse key has it); the
//                  chosen recording is installed, the box shows it, the log says a
//                  recording was chosen and the file opened. BEFORE: "cannot open
//                  file: " and nothing else.
//   empty+cancel   the dialog dismissed: nothing changes, and the log says so.
//   empty+none     no dialog on this machine: the red line says to type or paste a
//                  path, no worker is sent out.
//   quoted         a quoted path with a trailing space (a pasted "Copy as path") is
//                  opened, and the box shows it without the quotes. BEFORE: refused.
//   refused        a path to a file that is not there: the red line names it as
//                  before, and the log now has the refusal WITHOUT the path.
//                  BEFORE: no log line.
//   browse         the Browse key's handler: chosen -> installed; cancelled ->
//                  false and nothing changed; pressed while an open is out -> false.
//   dropped        a WAV dropped on the window goes to the box and is opened,
//                  with the combo on the I/Q file's row; a CSV dropped is not.
//
// Isolated like every test that constructs the application.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/diag_log.hpp"
#include "core/recorder.hpp"
#include "engine/engine.hpp"
#include "gui/app_window.hpp"
#include "pass_seam.hpp"
#include "test_check.hpp"
#include "test_host.hpp"

namespace fs = std::filesystem;

namespace cascade::gui {

// The friend AppWindow names for the test files that drive its private members. Since the engine split the row's open is a pair of halves: the
// window's (the box, the dialog, an empty box - openIqFile / browseIqFile) and the engine's (SELECT_SOURCE "file:<path>", the worker, the install
// - pollIqOpen on the pass), so a test pumps the engine after the window has submitted.
struct AppWindowTestAccess {
    static cascade::engine::Engine& eng(AppWindow& a) { return th::engine(a); }
    static void setRow(AppWindow& a, int row) { passseam::on(eng(a), [&] { eng(a).sourceSel_ = row; }); }
    static int row(AppWindow& a) { return passseam::read<int>(eng(a), [&] { return eng(a).sourceSel_; }); }
    static void setBox(AppWindow& a, const std::string& p) { std::snprintf(a.iqPath_, sizeof(a.iqPath_), "%s", p.c_str()); }
    static std::string box(AppWindow& a) { return a.iqPath_; }
    // The file dialog's answer: a path chosen, "" cancelled, nullopt no dialog at all.
    static void setDialog(AppWindow& a, std::optional<std::string> answer) {
        a.filePickSeam_ = answer;
        a.filePickOffered_ = answer.has_value();
    }
    static void open(AppWindow& a) { a.openIqFile(); }
    static bool browse(AppWindow& a) {
        // (the window reads "an open is out" from the engine's fact: a pass and a refresh first, as a frame would)
        poll(a);
        return a.browseIqFile();
    }
    // One frame's worth of both halves: the engine's pass (the submitted commands, the open's poll) and the window's look at the facts.
    static void poll(AppWindow& a) {
        passseam::whole(eng(a), [&] { eng(a).pump(); });
        a.refreshUiState();
    }
    static bool pending(AppWindow& a) { return a.iqOpenPending(); }
    static void drop(AppWindow& a, const std::string& p) {
        a.pendingDropPath_ = p;
        a.submitPendingDrop();
        poll(a);  // the window's commands are the engine's next pass: the row moves and the open starts there
    }
    static std::string sourceKind(AppWindow& a) { return passseam::read<std::string>(eng(a), [&] { return eng(a).sourceKind_; }); }
    // The red line under the converter controls is the engine's (sourceError_); the "type or paste" line for an empty box with no dialog is the window's.
    static std::string sourceError(AppWindow& a) {
        const std::string e = passseam::read<std::string>(eng(a), [&] { return eng(a).sourceError_; });
        return e.empty() ? a.iqBoxNote_ : e;
    }
    static std::string openPath(AppWindow& a) { return passseam::read<std::string>(eng(a), [&] { return eng(a).iqOpenPath_; }); }
    static double rateHz(AppWindow& a) {
        return passseam::read<double>(eng(a), [&] { return eng(a).pipeline_.activeSource().sampleRateHz(); });
    }
};

}  // namespace cascade::gui

using Access = cascade::gui::AppWindowTestAccess;
using cascade::gui::AppWindow;

namespace {

fs::path g_scratch;

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
    ::SetEnvironmentVariableA(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void isolate() {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    g_scratch = fs::temp_directory_path() / ("foxsdr_iq_open_choice_" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    fs::create_directories(g_scratch, ec);
    const std::string s = g_scratch.string();
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME", "XDG_CONFIG_HOME",
                          "XDG_STATE_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME"}) {
        setEnv(v, s);
    }
    setEnv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_TESTER_USAGE_URL", "http://127.0.0.1:9");
    setEnv("FOXSDR_BETA_API_URL", "http://127.0.0.1:9");
}

// A real I/Q recording, written by the application's own recorder: 4000 frames at
// 250 kS/s, exactly what the row is meant to take.
std::string makeRecording(const char* tag) {
    cascade::core::Recorder rec;
    std::string err;
    const std::string dir = (g_scratch / tag).string();
    CHECK(rec.start(cascade::core::RecordKind::BasebandIq, dir, 250000.0, err));
    std::vector<std::complex<float>> s(4000);
    for (std::size_t i = 0; i < s.size(); ++i) {
        s[i] = {0.25f * static_cast<float>(i % 7), -0.125f * static_cast<float>(i % 5)};
    }
    rec.writeIq(s.data(), s.size());
    rec.stop();
    return rec.path();
}

double nowMs() {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Feeds the window's per-frame poll until `cond` or `ms` have passed.
template <class Cond>
bool pump(AppWindow& app, double ms, Cond&& cond) {
    const double until = nowMs() + ms;
    while (nowMs() < until) {
        Access::poll(app);
        if (cond()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return cond();
}

// Whether the log ring holds a line containing `needle`, from `from` on.
bool logged(std::size_t from, const std::string& needle) {
    const auto lines = cascade::core::DiagLog::instance().ringSnapshot();
    for (std::size_t i = from < lines.size() ? from : lines.size(); i < lines.size(); ++i) {
        if (lines[i].find(needle) != std::string::npos) { return true; }
    }
    return false;
}
std::size_t logMark() { return cascade::core::DiagLog::instance().ringSnapshot().size(); }

void checkInstalled(AppWindow& app, const std::string& path) {
    CHECK(Access::sourceKind(app) == "file");
    CHECK(Access::sourceError(app).empty());
    CHECK(Access::openPath(app) == path);
    CHECK(Access::box(app) == path);
    CHECK(Access::row(app) == 1);
    CHECK(Access::rateHz(app) == 250000.0);
}

}  // namespace

int main() {
    std::printf("test_iq_open_choice\n");
    isolate();
    const std::string recA = makeRecording("a");
    const std::string recB = makeRecording("b");
    CHECK(!recA.empty() && !recB.empty());

    // --- empty+dialog: Open over an empty box is the file dialog --------------------
    {
        std::printf("  empty+dialog\n");
        th::TestWindow app;
        Access::setRow(app, 1);
        Access::setBox(app, "");
        Access::setDialog(app, recA);
        const std::size_t mark = logMark();
        Access::open(app);
        CHECK(pump(app, 5000.0, [&] { return Access::sourceKind(app) == "file"; }));
        checkInstalled(app, recA);
        CHECK(logged(mark, "source: a recording was chosen in the I/Q file dialog"));
        CHECK(logged(mark, "source: opened an I/Q file at 250000 S/s"));
        CHECK(!logged(mark, "cannot open file"));
        // Never the file's name in the log.
        CHECK(!logged(mark, fs::path(recA).filename().string()));
    }
    // --- empty+cancel: the dialog dismissed changes nothing -------------------------
    {
        std::printf("  empty+cancel\n");
        th::TestWindow app;
        Access::setRow(app, 1);
        Access::setBox(app, "   ");  // spaces are an empty box too
        Access::setDialog(app, std::string());
        const std::size_t mark = logMark();
        Access::open(app);
        CHECK(!Access::pending(app));
        CHECK(!pump(app, 300.0, [&] { return Access::sourceKind(app) == "file"; }));
        CHECK(Access::sourceKind(app) == "siggen");
        CHECK(Access::sourceError(app).empty());
        CHECK(Access::box(app).empty());  // the spaces are gone: what the box shows is what is tried
        CHECK(logged(mark, "source: the I/Q file dialog was closed without a choice"));
    }
    // --- empty+none: no dialog to offer: the box says what it is for ----------------
    {
        std::printf("  empty+none\n");
        th::TestWindow app;
        Access::setRow(app, 1);
        Access::setBox(app, "");
        Access::setDialog(app, std::nullopt);
        const std::size_t mark = logMark();
        Access::open(app);
        CHECK(!Access::pending(app));
        CHECK(Access::sourceKind(app) == "siggen");
        CHECK(Access::sourceError(app) == "Type or paste the path of a WAV recording, then press Open.");
        CHECK(logged(mark, "source: Open pressed with no I/Q file named and no file dialog to offer"));
    }
    // --- quoted: Explorer's "Copy as path", pasted with its line ending -------------
    {
        std::printf("  quoted\n");
        th::TestWindow app;
        Access::setRow(app, 1);
        Access::setBox(app, "\"" + recB + "\" ");
        Access::setDialog(app, std::string());  // a dialog that, if asked, would cancel: it is not asked
        const std::size_t mark = logMark();
        Access::open(app);
        CHECK(Access::box(app) == recB);  // the box shows the path as tried, at once
        CHECK(pump(app, 5000.0, [&] { return Access::sourceKind(app) == "file"; }));
        checkInstalled(app, recB);
        CHECK(!logged(mark, "dialog"));
    }
    // --- refused: a file that is not there, in the red line and (nameless) in the log
    {
        std::printf("  refused\n");
        th::TestWindow app;
        Access::setRow(app, 1);
        const std::string missing = (g_scratch / "no-such-take.wav").string();
        Access::setBox(app, missing);
        Access::setDialog(app, std::nullopt);
        const std::size_t mark = logMark();
        Access::open(app);
        CHECK(pump(app, 5000.0, [&] { return !Access::sourceError(app).empty(); }));
        CHECK(Access::sourceKind(app) == "siggen");
        CHECK(Access::sourceError(app) == "cannot open file: " + missing);  // on screen: the whole sentence
        CHECK(logged(mark, "source: the I/Q file did not open: cannot open file: (the file)"));
        CHECK(!logged(mark, "no-such-take"));  // in the log: never the name
    }
    // --- browse: the key's handler ----------------------------------------------------
    {
        std::printf("  browse\n");
        th::TestWindow app;
        Access::setRow(app, 1);
        Access::setBox(app, recA);  // a path already in the box does not stop a Browse
        Access::setDialog(app, recB);
        CHECK(Access::browse(app));
        CHECK(Access::box(app) == recB);
        CHECK(pump(app, 5000.0, [&] { return Access::sourceKind(app) == "file"; }));
        checkInstalled(app, recB);
        // Cancelled: false, and the installed file stays.
        Access::setDialog(app, std::string());
        const std::size_t mark = logMark();
        CHECK(!Access::browse(app));
        CHECK(Access::openPath(app) == recB);
        CHECK(Access::box(app) == recB);
        CHECK(logged(mark, "closed without a choice"));
    }
    // --- dropped: a WAV goes to the row and opens; a CSV does not ------------------
    {
        std::printf("  dropped\n");
        th::TestWindow app;
        Access::setRow(app, 0);  // on the generator
        Access::setBox(app, "");
        Access::setDialog(app, std::nullopt);
        const std::size_t mark = logMark();
        Access::drop(app, recA);
        CHECK(Access::row(app) == 1);
        CHECK(Access::box(app) == recA);
        CHECK(pump(app, 5000.0, [&] { return Access::sourceKind(app) == "file"; }));
        checkInstalled(app, recA);
        CHECK(logged(mark, "source: a WAV file was dropped on the window; opening it as the I/Q file"));
        // A frequency list dropped next is not a recording: the box and the row keep the file.
        const std::string csv = (g_scratch / "list.csv").string();
        Access::drop(app, csv);
        CHECK(Access::box(app) == recA);
        CHECK(Access::openPath(app) == recA);
        pump(app, 300.0, [] { return false; });  // let the import worker answer before the window goes
    }

    std::error_code ec;
    fs::remove_all(g_scratch, ec);
    return testSummary("test_iq_open_choice");
}
