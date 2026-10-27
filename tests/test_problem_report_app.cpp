/*
 * THE REPORT A BUG / DISLIKE PAGE AND THE REQUEST A FEATURE PAGE, IN THE REAL APPLICATION (0.99.69, stub campaign B14).
 *
 * The pure halves are pinned elsewhere - the payload and the older-site fallback in tests/test_problem_report.cpp, the recognising of a pasted report in
 * tests/test_pasted_report.cpp, the bundle's lines in tests/test_diagnostics.cpp. Nothing held the PAGE: master had no test of it (the ledger id PROBLEM-REPORT
 * has no pending placeholder), so this branch's page lost the window half of 0.99.69 without a single test going red. This runs cascade itself, opens each page by
 * its bounded-run seam (FOXSDR_OPEN_PROBLEM_REPORT / FOXSDR_OPEN_FEATURE_REQUEST), drives it with the application's own input script aimed from the UI census, and
 * checks from the census and the diagnostic log it wrote:
 *
 *   opens         the problem page draws, with no kind chosen and SEND refused for that reason; the feature page is not drawn;
 *   bug           choosing "bug" ticks "Attach the diagnostics log" and the attachment it builds is the bundle - with the signature ids of the reports on the machine
 *                 and the recovered words (owner-approved 2026-10-27), and the line that says the last run's state;
 *   dislike       choosing "dislike" leaves the box unticked and builds nothing;
 *   no text       SEND with no text is refused, with the reason, and a press on it sends nothing (no log line, no payload);
 *   sends         with text, SEND is enabled, a press sends once (one log line, with the count of characters and never the words), and the payload carries the
 *                 diagnostics when the box is ticked;
 *   recognised    a sentinel/hang report pasted into the box is recognised (the line above the contact box, and the attachment in the payload); prose that merely
 *                 quotes a `kind:` line is not;
 *   local-only    "Show what will be sent" draws the preview, and a report that stayed on this machine is explained in words, with the folder;
 *   no email      both pages say that a callsign alone cannot be written back to, and say nothing of it when an email address is there;
 *   feature       the feature-request path is its own: its own page, its own log line, no kind, no diagnostics, and none of the problem page's rows.
 *
 * Isolated like every other test that starts the application: its own config, APPDATA, LOCALAPPDATA and diagnostics tree, and every FOXSDR_*_URL pointed at a
 * closed local port, so nothing is sent anywhere real.
 *
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "test_check.hpp"

namespace fs = std::filesystem;

namespace {

int pid() {
#if defined(_WIN32)
    return static_cast<int>(::GetCurrentProcessId());
#else
    return static_cast<int>(::getpid());
#endif
}

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    ::SetEnvironmentVariableA(name, value.empty() ? nullptr : value.c_str());
    _putenv_s(name, value.c_str());
#else
    if (value.empty()) {
        ::unsetenv(name);
    } else {
        ::setenv(name, value.c_str(), 1);
    }
#endif
}

std::string exePath() {
#if defined(_WIN32)
    return std::string(CASCADE_APP_BINDIR) + "/cascade.exe";
#else
    return std::string(CASCADE_APP_BINDIR) + "/cascade";
#endif
}

std::string runCmd(const std::string& cmd) {
    std::string out;
#if defined(_WIN32)
    FILE* p = _popen(("\"" + cmd + "\"").c_str(), "r");
#else
    FILE* p = popen(cmd.c_str(), "r");
#endif
    char buf[512];
    while (p != nullptr && std::fgets(buf, sizeof(buf), p) != nullptr) { out += buf; }
#if defined(_WIN32)
    if (p != nullptr) { _pclose(p); }
#else
    if (p != nullptr) { pclose(p); }
#endif
    return out;
}

struct Rect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float cx() const { return 0.5f * (x0 + x1); }
    float cy() const { return 0.5f * (y0 + y1); }
};

struct Result {
    bool ok = false;
    std::set<std::string> items;
    std::map<std::string, Rect> rects;
    std::string out;
    std::string log;
    bool has(const std::string& item) const { return items.count(item) != 0; }
    // The first item that starts with `prefix`, whole; empty when none does.
    std::string itemWith(const std::string& prefix) const {
        for (const std::string& it : items) {
            if (it.rfind(prefix, 0) == 0) { return it; }
        }
        return std::string();
    }
    bool anyItemWith(const std::string& prefix) const { return !itemWith(prefix).empty(); }
    // EVERY item that starts with `prefix`, one after another: a line the page rebuilt (once "still being read", then with the answer in it) leaves each version behind,
    // and the census is a set, not a history.
    std::string allItemsWith(const std::string& prefix) const {
        std::string all;
        for (const std::string& it : items) {
            if (it.rfind(prefix, 0) == 0) { all += it + "\n"; }
        }
        return all;
    }
    const Rect* rect(const std::string& name) const {
        const auto it = rects.find(name);
        return it == rects.end() ? nullptr : &it->second;
    }
    bool logHas(const std::string& needle) const { return log.find(needle) != std::string::npos; }
};

fs::path g_dir;

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::string readTree(const fs::path& dir) {
    std::string all;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)) { all += readFile(it->path()) + "\n"; }
    }
    return all;
}

void writeFile(const fs::path& p, const std::string& body) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << body;
}

// THE REPORTS ON "THIS MACHINE", for the runs that seed them: a display-driver stall that stayed here (its sidecar says local-only, which is what the page explains
// in words) and a freeze that was sent. The signatures are the ids the diagnostics must name; neither file holds anything but the head a report has.
constexpr const char* kSigStall = "0123456789ABCDEF";
constexpr const char* kSigHang = "FEDCBA9876543210";

void seedReports(const fs::path& diag) {
    const fs::path crashes = diag / "crashes";
    writeFile(crashes / "hang-20261001-100000.txt",
              std::string("kind: stall\nsignature: ") + kSigStall +
                  "\nstalled-ms: 3200\n--- context ---\nversion: 0.99.90\n--- process ---\nuptime-sec: 600\n");
    writeFile(crashes / "hang-20261001-100000.txt.upload", "status: local-only\n");
    writeFile(crashes / "hang-20261002-100000.txt",
              std::string("kind: hang\nsignature: ") + kSigHang +
                  "\nstalled-ms: 9100\n--- context ---\nversion: 0.99.90\n--- process ---\nuptime-sec: 1200\n");
    writeFile(crashes / "hang-20261002-100000.txt.upload", "status: sent\n");
}

struct Run {
    std::string tag;
    std::string page = "problem";  // "problem" or "feature"
    std::string script;
    int frames = 110;
    bool seed = false;
    bool diagnostics = true;
};

Result once(const Run& r) {
    Result out;
    const fs::path cfg = g_dir / (r.tag + ".json");
    const fs::path census = g_dir / (r.tag + ".census");
    const fs::path diag = g_dir / (r.tag + "-diag");
    std::error_code ec;
    fs::remove_all(diag, ec);
    fs::create_directories(diag, ec);
    fs::create_directories(diag / "crashes", ec);
    if (r.seed) { seedReports(diag); }
    writeFile(cfg, std::string("{ \"telemetryEnabled\": false, \"updateCheckEnabled\": false, \"diagnosticsEnabled\": ") +
                       (r.diagnostics ? "true" : "false") +
                       ", \"sourceKind\": \"siggen\", \"uiTheme\": \"today\", \"interfaceScale\": \"100\", \"mainView\": "
                       "\"receiver\", \"railBank\": 0, \"bandPlanOverlay\": false }\n");
    const std::string scriptPath = (g_dir / (r.tag + ".script")).string();
    writeFile(scriptPath, r.script);
    setEnv("CASCADE_CONFIG_TEST", cfg.string());
    setEnv("FOXSDR_UI_CENSUS", census.string());
    setEnv("FOXSDR_DIAG_DIR", diag.string());
    setEnv("FOXSDR_INPUT_SCRIPT", r.script.empty() ? std::string() : scriptPath);
    setEnv("FOXSDR_OPEN_PROBLEM_REPORT", r.page == "problem" ? "1" : "");
    setEnv("FOXSDR_OPEN_FEATURE_REQUEST", r.page == "feature" ? "1" : "");
    out.out = runCmd("\"" + exePath() + "\" --frames " + std::to_string(r.frames) + " 2>&1");
    out.log = readTree(diag / "logs");
    const bool rendered = out.out.find("rendered " + std::to_string(r.frames) + " frames") != std::string::npos;
    const bool written = out.out.find("ui census written") != std::string::npos;
    if (!rendered || !written) {
        std::printf("  %s: the run did not finish cleanly:\n%s\n", r.tag.c_str(), out.out.c_str());
        return out;
    }
    std::ifstream in(census);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("item ", 0) == 0) { out.items.insert(line.substr(5)); }
        if (line.rfind("rect ", 0) != 0) { continue; }
        std::istringstream ss(line.substr(5));
        std::vector<std::string> tok;
        std::string t;
        while (ss >> t) { tok.push_back(t); }
        if (tok.size() < 5) { continue; }
        const std::size_t n = tok.size();
        Rect rc;
        rc.x0 = std::strtof(tok[n - 4].c_str(), nullptr);
        rc.y0 = std::strtof(tok[n - 3].c_str(), nullptr);
        rc.x1 = std::strtof(tok[n - 2].c_str(), nullptr);
        rc.y1 = std::strtof(tok[n - 1].c_str(), nullptr);
        std::string name = tok[0];
        for (std::size_t i = 1; i + 4 < n; ++i) { name += " " + tok[i]; }
        out.rects[name] = rc;
    }
    out.ok = true;
    return out;
}

// A click at the centre of `at`: the pointer arrives at `frame`, goes down two frames later and up two after that.
std::string click(int frame, const Rect& at) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%d screen %.1f %.1f\n%d down\n%d up\n", frame, at.cx(), at.cy(), frame + 2, frame + 4);
    return buf;
}

std::string typeText(int frame, const std::string& text) { return std::to_string(frame) + " text " + text + "\n"; }
std::string tapEnter(int frame) { return std::to_string(frame) + " key enter\n"; }

// A SEND IS A NETWORK ATTEMPT ON A WORKER, to a port nothing listens on, and the page logs its one line on the frame it first SEES the answer. A bounded run's frames
// are not a clock, so the script holds the frame loop (the `sleep` verb) for real time after the press: long enough for a refused connection to come back.
std::string waitForTheAnswer(int frame) {
    return std::to_string(frame) + " sleep 4000\n" + std::to_string(frame + 2) + " sleep 4000\n";
}

// ONE RUN'S SCRIPT BUILT FROM THE LAYOUT THE LAST ONE LEFT. A page moves as it is filled in (a line appears under the box, the sentence changes), so a key is aimed
// from a run that did everything before it and stopped: `prefix` first, its census read, then the same prefix and the press.
const Rect* need(const Result& r, const char* name) {
    const Rect* rc = r.rect(name);
    if (rc == nullptr) { std::printf("    missing rectangle %s\n", name); }
    return rc;
}

// --- the fixtures the scenarios share --------------------------------------------------------------------------------------------------------------------
// The text a report-by-hand is made of. A freeze report in the shape the writers produce (core/pasted_report.hpp): the kind, the signature, the context block and
// the version that wrote it.
const char* const kHangReportLines[] = {"kind: hang", "signature: 0123456789ABCDEF", "--- context ---", "version: 0.99.90"};

// --- THE SCENARIOS ---------------------------------------------------------------------------------------------------------------------------------------

void testOpens(Rect& bug, Rect& dislike) {
    std::printf("  opens\n");
    Run run;
    run.tag = "opens";
    const Result r = once(run);
    CHECK(r.ok);
    if (!r.ok) { return; }
    CHECK(r.has("problem:page"));
    CHECK(!r.has("feature:page"));
    // NO KIND IS CHOSEN when it opens - a default would file every hurried report as whichever came first - and SEND says that is why it is dead.
    CHECK(r.has("problem:kind-is:none"));
    CHECK(!r.has("problem:kind-is:bug"));
    CHECK(r.has("problem:send:disabled:kind"));
    CHECK(!r.has("problem:send:enabled"));
    for (const char* name : {"problem:kind:bug", "problem:kind:dislike", "problem:text", "problem:contact", "problem:send"}) {
        CHECK(r.rect(name) != nullptr);
    }
    // THE ATTACH ROW is on the page from the start, unticked until a kind has been chosen, and nothing is built for it.
    CHECK(r.has("problem:attach"));
    CHECK(r.rect("problem:attach") != nullptr);
    CHECK(r.has("problem:attach:off"));
    CHECK(!r.has("problem:attach:on"));
    CHECK(!r.anyItemWith("problem:diag-line:"));
    if (r.rect("problem:kind:bug") != nullptr) { bug = *r.rect("problem:kind:bug"); }
    if (r.rect("problem:kind:dislike") != nullptr) { dislike = *r.rect("problem:kind:dislike"); }
    std::printf("    kind keys at %.0f,%.0f and %.0f,%.0f\n", bug.cx(), bug.cy(), dislike.cx(), dislike.cy());
}

void testBug(const Rect& bug) {
    std::printf("  bug (with reports on the machine)\n");
    Run run;
    run.tag = "bug";
    run.seed = true;
    run.script = click(20, bug);
    const Result r = once(run);
    CHECK(r.ok);
    if (!r.ok) { return; }
    CHECK(r.has("problem:kind-is:bug"));
    // A bug follows the kind's own default: the box is ticked, and the attachment is the bundle.
    CHECK(r.has("problem:attach:on"));
    CHECK(r.has("problem:send:disabled:text"));
    // THE FAULT MARKER STATE, as the bundle has always said it.
    CHECK(r.anyItemWith("problem:diag-line:last-run-unclean: "));
    // THE SIGNATURE IDS of the reports on this machine (the owner's addition, 2026-10-27): ids and kinds only, newest first, both reports of the seed named.
    const std::string faults = r.allItemsWith("problem:diag-line:recent-faults: ");
    std::printf("    %s\n", faults.c_str());
    CHECK(faults.find(kSigStall) != std::string::npos);
    CHECK(faults.find(kSigHang) != std::string::npos);
    CHECK(faults.find("stall") != std::string::npos);
    CHECK(faults.find("hang") != std::string::npos);
    // ...and the words of what the program recovered from. Usage reporting is off in this run, so nothing is counted and the line says so rather than "none".
    const std::string recovered = r.allItemsWith("problem:diag-line:recovered: ");
    std::printf("    %s\n", recovered.c_str());
    CHECK(!recovered.empty());
    CHECK(recovered.find("not counted") != std::string::npos);
}

void testNoSeed(const Rect& bug) {
    std::printf("  bug (nothing on the machine)\n");
    Run run;
    run.tag = "bugnoseed";
    run.script = click(20, bug);
    const Result r = once(run);
    CHECK(r.ok);
    if (!r.ok) { return; }
    // A folder with no report says "none", not nothing: the line is there to be read.
    CHECK(r.has("problem:diag-line:recent-faults: none"));
}

void testDislike(const Rect& dislike) {
    std::printf("  dislike\n");
    Run run;
    run.tag = "dislike";
    run.seed = true;
    run.script = click(20, dislike);
    const Result r = once(run);
    CHECK(r.ok);
    if (!r.ok) { return; }
    CHECK(r.has("problem:kind-is:dislike"));
    CHECK(r.has("problem:attach:off"));
    CHECK(!r.has("problem:attach:on"));
    // Unticked, so nothing is built and nothing would travel.
    CHECK(!r.anyItemWith("problem:diag-line:"));
}

void testNoTextRefused(const Rect& bug) {
    std::printf("  no text: SEND refused\n");
    Run first;
    first.tag = "notext-aim";
    first.script = click(20, bug);
    const Result aim = once(first);
    CHECK(aim.ok);
    if (!aim.ok || need(aim, "problem:send") == nullptr) { return; }
    const Rect send = *aim.rect("problem:send");
    CHECK(aim.has("problem:send:disabled:text"));

    Run run;
    run.tag = "notext";
    run.script = click(20, bug) + click(50, send);
    const Result r = once(run);
    CHECK(r.ok);
    if (!r.ok) { return; }
    CHECK(r.has("problem:send:disabled:text"));
    CHECK(!r.has("problem:send:enabled"));
    // A PRESS ON A DEAD KEY SENDS NOTHING: no payload built, no log line.
    CHECK(!r.has("problem:sent"));
    CHECK(!r.logHas("problem report ("));
}

void testSends(const Rect& bug) {
    std::printf("  sends\n");
    const std::string words = "the waterfall froze when I tuned";  // 32 characters
    Run first;
    first.tag = "sends-aim";
    first.script = click(20, bug);
    const Result probe = once(first);
    CHECK(probe.ok);
    if (!probe.ok || need(probe, "problem:text") == nullptr) { return; }
    const std::string prefix = click(20, bug) + click(30, *probe.rect("problem:text")) + typeText(38, words);

    Run aim;
    aim.tag = "sends-aim2";
    aim.script = prefix;
    const Result typed = once(aim);
    CHECK(typed.ok);
    if (!typed.ok || need(typed, "problem:send") == nullptr) { return; }
    CHECK(typed.has("problem:send:enabled"));
    CHECK(!typed.has("problem:sent"));
    // Typed and not sent: the words are not in the log and no send was logged.
    CHECK(!typed.logHas("problem report ("));

    Run run;
    run.tag = "sends";
    run.script = prefix + click(60, *typed.rect("problem:send")) + waitForTheAnswer(66);
    run.seed = true;
    const Result r = once(run);
    CHECK(r.ok);
    if (!r.ok) { return; }
    CHECK(r.has("problem:sent"));
    CHECK(r.has("problem:sent:kind:bug"));
    // THE DIAGNOSTICS RODE ALONG - the box was ticked when SEND was pressed - and no attachment was recognised in prose.
    CHECK(r.has("problem:sent:with-diagnostics"));
    CHECK(!r.anyItemWith("problem:sent:attachment:"));
    // ONE LOG LINE per send, with the kind and the count of characters, never the words.
    CHECK(r.logHas("problem report (bug): "));
    CHECK(r.logHas(", 32 characters"));
    CHECK(!r.logHas("waterfall froze"));
}

void testNoDiagnosticsWhenUnticked(const Rect& bug) {
    std::printf("  unticked: the log does not travel\n");
    Run first;
    first.tag = "unticked-aim";
    first.script = click(20, bug);
    const Result probe = once(first);
    CHECK(probe.ok);
    if (!probe.ok || need(probe, "problem:text") == nullptr || need(probe, "problem:attach") == nullptr) { return; }
    const std::string prefix = click(20, bug) + click(28, *probe.rect("problem:attach")) + click(36, *probe.rect("problem:text")) + typeText(44, "no log please");
    Run aim;
    aim.tag = "unticked-aim2";
    aim.script = prefix;
    const Result typed = once(aim);
    CHECK(typed.ok);
    if (!typed.ok || need(typed, "problem:send") == nullptr) { return; }
    Run run;
    run.tag = "unticked";
    run.script = prefix + click(70, *typed.rect("problem:send")) + waitForTheAnswer(76);
    const Result r = once(run);
    CHECK(r.ok);
    if (!r.ok) { return; }
    CHECK(r.has("problem:sent"));
    CHECK(!r.has("problem:sent:with-diagnostics"));
    CHECK(r.logHas("problem report (bug): "));
}

void testRecognised(const Rect& bug) {
    std::printf("  a pasted report is recognised\n");
    Run first;
    first.tag = "paste-aim";
    first.script = click(20, bug);
    const Result probe = once(first);
    CHECK(probe.ok);
    if (!probe.ok || need(probe, "problem:text") == nullptr) { return; }
    std::string typed = click(20, bug) + click(30, *probe.rect("problem:text"));
    int frame = 38;
    for (const char* line : kHangReportLines) {
        typed += typeText(frame, line);
        typed += tapEnter(frame + 1);
        frame += 3;
    }
    Run aim;
    aim.tag = "paste-aim2";
    aim.script = typed;
    const Result pasted = once(aim);
    CHECK(pasted.ok);
    if (!pasted.ok || need(pasted, "problem:send") == nullptr) { return; }
    // THE LINE BEFORE SEND says it was understood, as the class it is.
    CHECK(pasted.has("problem:recognised:hang"));
    CHECK(pasted.has("problem:send:enabled"));

    Run run;
    run.tag = "paste";
    run.script = typed + click(frame + 24, *pasted.rect("problem:send")) + waitForTheAnswer(frame + 30);
    const Result r = once(run);
    CHECK(r.ok);
    if (!r.ok) { return; }
    CHECK(r.has("problem:sent"));
    // ...and travels as a structured attachment beside the message.
    CHECK(r.has("problem:sent:attachment:hang"));

    // PROSE THAT QUOTES A `kind:` LINE IS NOT A REPORT.
    Run prose;
    prose.tag = "prose";
    prose.script = click(20, bug) + click(30, *probe.rect("problem:text")) + typeText(38, "the log said kind: crash and then it stopped");
    const Result p = once(prose);
    CHECK(p.ok);
    if (!p.ok) { return; }
    CHECK(!p.anyItemWith("problem:recognised:"));
}

void testPreviewAndLocalOnly(const Rect& bug) {
    std::printf("  show what will be sent, and what upload local-only means\n");
    Run first;
    first.tag = "preview-aim";
    first.seed = true;
    first.script = click(20, bug);
    const Result probe = once(first);
    CHECK(probe.ok);
    if (!probe.ok || need(probe, "problem:show") == nullptr) { return; }
    CHECK(!probe.has("problem:preview"));
    CHECK(!probe.has("problem:local-only"));

    Run run;
    run.tag = "preview";
    run.seed = true;
    run.script = click(20, bug) + click(60, *probe.rect("problem:show"));
    const Result r = once(run);
    CHECK(r.ok);
    if (!r.ok) { return; }
    CHECK(r.has("problem:preview"));
    // THE SEED'S STALL STAYED ON THIS MACHINE, and the page says what that means in words, with the folder the file is in.
    CHECK(r.has("problem:local-only"));
    CHECK(r.has("problem:local-only:folder"));

    // WITH NOTHING THAT STAYED HERE there is nothing to explain.
    Run quiet;
    quiet.tag = "preview-quiet";
    quiet.script = click(20, bug) + click(60, *probe.rect("problem:show"));
    const Result q = once(quiet);
    CHECK(q.ok);
    if (!q.ok) { return; }
    CHECK(q.has("problem:preview"));
    CHECK(!q.has("problem:local-only"));
}

void testNoEmailNote(const Rect& bug) {
    std::printf("  no email address\n");
    Run first;
    first.tag = "email-aim";
    first.script = click(20, bug);
    const Result probe = once(first);
    CHECK(probe.ok);
    if (!probe.ok || need(probe, "problem:contact") == nullptr) { return; }
    // EMPTY: the note is drawn.
    CHECK(probe.has("problem:no-email-note"));
    CHECK(!probe.has("problem:contact-has-email"));

    // A CALLSIGN ALONE cannot be written back to: still said.
    Run call;
    call.tag = "email-call";
    call.script = click(20, bug) + click(30, *probe.rect("problem:contact")) + typeText(38, "G4ABC");
    const Result c = once(call);
    CHECK(c.ok);
    if (c.ok) {
        CHECK(c.has("problem:no-email-note"));
        CHECK(!c.has("problem:contact-has-email"));
    }

    // AN EMAIL ADDRESS: the note is gone, the other branch ran.
    Run mail;
    mail.tag = "email-mail";
    mail.script = click(20, bug) + click(30, *probe.rect("problem:contact")) + typeText(38, "someone@example.com");
    const Result m = once(mail);
    CHECK(m.ok);
    if (m.ok) { CHECK(m.has("problem:contact-has-email")); }
}

void testFeaturePathIsItsOwn() {
    std::printf("  the feature request is its own path\n");
    Run run;
    run.page = "feature";
    run.tag = "feature";
    const Result r = once(run);
    CHECK(r.ok);
    if (!r.ok) { return; }
    CHECK(r.has("feature:page"));
    CHECK(!r.has("problem:page"));
    CHECK(!r.anyItemWith("problem:"));
    // No kind, no diagnostics row: it sends the message and the contact and nothing else.
    CHECK(r.has("feature:send:disabled:text"));
    CHECK(!r.has("feature:send:enabled"));
    CHECK(r.has("feature:no-email-note"));
    CHECK(!r.has("feature:contact-has-email"));
    for (const char* name : {"feature:text", "feature:contact", "feature:send"}) { CHECK(r.rect(name) != nullptr); }
    if (r.rect("feature:text") == nullptr || r.rect("feature:contact") == nullptr || r.rect("feature:send") == nullptr) { return; }

    const std::string words = "a second waterfall please";  // 25 characters
    const std::string prefix = click(20, *r.rect("feature:text")) + typeText(28, words);
    Run aim;
    aim.page = "feature";
    aim.tag = "feature-aim";
    aim.script = prefix;
    const Result typed = once(aim);
    CHECK(typed.ok);
    if (!typed.ok || need(typed, "feature:send") == nullptr) { return; }
    CHECK(typed.has("feature:send:enabled"));
    Run send;
    send.page = "feature";
    send.tag = "feature-send";
    send.script = prefix + click(50, *typed.rect("feature:send")) + waitForTheAnswer(56);
    const Result s = once(send);
    CHECK(s.ok);
    if (!s.ok) { return; }
    // ITS OWN LOG LINE, and nothing of the problem path's: not the line, not the census, not an attachment.
    CHECK(s.logHas("feature request: "));
    CHECK(s.logHas(", 25 characters"));
    CHECK(!s.logHas("problem report ("));
    CHECK(!s.anyItemWith("problem:"));
    CHECK(!s.logHas("a second waterfall"));

    // AN EMAIL ADDRESS in the contact line ends the note here too.
    Run mail;
    mail.page = "feature";
    mail.tag = "feature-mail";
    mail.script = click(20, *r.rect("feature:contact")) + typeText(28, "someone@example.com");
    const Result m = once(mail);
    CHECK(m.ok);
    if (m.ok) { CHECK(m.has("feature:contact-has-email")); }
}

}  // namespace

int main() {
    std::printf("test_problem_report_app\n");
    g_dir = fs::temp_directory_path() / ("cascade-problem-report-" + std::to_string(pid()));
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir / "appdata", ec);
    fs::create_directories(g_dir / "local", ec);
    fs::create_directories(g_dir / "home", ec);
    setEnv("APPDATA", (g_dir / "appdata").string());
    setEnv("LOCALAPPDATA", (g_dir / "local").string());
    setEnv("XDG_CONFIG_HOME", (g_dir / "appdata").string());
    setEnv("XDG_DATA_HOME", (g_dir / "local").string());
    setEnv("XDG_STATE_HOME", (g_dir / "local").string());
    setEnv("USERPROFILE", (g_dir / "home").string());
    setEnv("HOME", (g_dir / "home").string());
    for (const char* url : {"FOXSDR_TELEMETRY_URL", "FOXSDR_CRASH_URL", "FOXSDR_UPDATE_URL", "FOXSDR_REPORTS_URL", "FOXSDR_FEATURE_URL", "FOXSDR_PROBLEM_URL",
                            "FOXSDR_BETA_API_URL", "FOXSDR_TESTER_USAGE_URL"}) {
        setEnv(url, "http://127.0.0.1:9/");
    }
    setEnv("FOXSDR_SINGLE_VIEWPORT", "1");
    setEnv("FOXSDR_WINDOW_SIZE", "1600x1000");
    setEnv("FOXSDR_FAKE_PACKAGE", "none");
    setEnv("FOXSDR_CENSUS_HOLD_BANK", "1");
    setEnv("FOXSDR_PATCH_START", "");
    setEnv("FOXSDR_PATCH_FILE", "");
    setEnv("FOXSDR_OPEN_PLUGIN_STORE", "");
    setEnv("FOXSDR_PRESS_PRESET", "");
    setEnv("FOXSDR_SHOT_DIR", "");
    setEnv("FOXSDR_SHOT_AT_FRAME", "");
    setEnv("FOXSDR_WINDOW_POS", "");
    setEnv("CASCADE_DECODE_TEST", "");
    setEnv("CASCADE_PLUGIN_TEST", "");

    Rect bug;
    Rect dislike;
    testOpens(bug, dislike);
    testBug(bug);
    testNoSeed(bug);
    testDislike(dislike);
    testNoTextRefused(bug);
    testSends(bug);
    testNoDiagnosticsWhenUnticked(bug);
    testRecognised(bug);
    testPreviewAndLocalOnly(bug);
    testNoEmailNote(bug);
    testFeaturePathIsItsOwn();

    const int rc = testSummary("test_problem_report_app");
    if (rc == 0) { fs::remove_all(g_dir, ec); }
    return rc;
}
