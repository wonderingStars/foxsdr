// THE GATE'S OWN BREAK-IT: does tests/test_error_paths.cpp actually bite?
//
// test_error_paths is only worth having if it goes RED when someone adds an error path that nothing
// reports. This program runs the SAME scanner (tests/errpath_scan.hpp) over small fixture trees under
// tests/errpath_fixtures/<case>/ (src/ plus allow.tsv: scanned as text, never compiled) and checks that
// every way of passing passes and every way of failing fails, each as its own check with a plain message.
//
//   errpath_gate_selftest [repo-root]
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "errpath_scan.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;

namespace {

fs::path gFixtures;
std::vector<std::string> gReporters;

void expect(bool ok, const std::string& msg) {
    ++g_checksRun;
    if (!ok) {
        ++g_checksFailed;
        std::printf("FAIL %s\n", msg.c_str());
    }
}

struct Run {
    errpath::Scan scan;
    errpath::Result result;
};

// scan fixture `scanCase` and judge it against the allow list of `allowCase` (default: its own), with an optional pin
Run run(const std::string& scanCase, const std::string& allowCase = "", int pin = -1) {
    errpath::Options opt;
    opt.root = (gFixtures / scanCase).string();
    opt.profile = "engine";
    opt.reporters = gReporters;
    opt.pin = pin;
    Run r;
    r.scan = errpath::scanTree(opt);
    std::vector<std::string> problems;
    const std::string allowText = errpath::readFile(gFixtures / (allowCase.empty() ? scanCase : allowCase) / "allow.tsv");
    const auto allow = errpath::parseAllow(allowText, &problems);
    r.result = errpath::check(r.scan.sites, allow, opt);
    for (auto& p : problems) { r.result.problems.push_back(p); }
    return r;
}

bool mentions(const std::vector<std::string>& v, const std::string& s) {
    for (const auto& m : v) { if (m.find(s) != std::string::npos) { return true; } }
    return false;
}

const errpath::Site* find(const Run& r, const std::string& kind, const std::string& fn) {
    for (const auto& s : r.scan.sites) { if (s.kind == kind && s.function == fn) { return &s; } }
    return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
    fs::path root = argc > 1 ? fs::path(argv[1]) : fs::current_path();
    for (int i = 0; i < 8 && !fs::exists(root / "tests" / "errpath_fixtures"); ++i) { root = root.parent_path(); }
    gFixtures = root / "tests" / "errpath_fixtures";
    gReporters = errpath::readReporters((root / "tests" / "errpath_reporters.txt").string());
    if (!fs::exists(gFixtures) || gReporters.empty()) {
        std::printf("FAIL cannot find tests/errpath_fixtures and tests/errpath_reporters.txt under %s\n", root.string().c_str());
        return 1;
    }

    // 1. a bare warning is RED, and the message names the file, the function, the kind, the text and what to do
    {
        const Run r = run("bare_warning");
        expect(r.scan.sites.size() == 1, "bare_warning: the scanner should find exactly one site (the diagWarnf), found " + std::to_string(r.scan.sites.size()));
        expect(r.result.unresolved.size() == 1, "bare_warning: a bare diagWarnf with no reporter and no allow line must be RED");
        const std::string m = r.result.unresolved.empty() ? "" : r.result.unresolved[0];
        expect(m.find("src/a.cpp") != std::string::npos, "bare_warning: the message must name the file src/a.cpp");
        expect(m.find("openIt") != std::string::npos, "bare_warning: the message must name the function openIt");
        expect(m.find("kind warn") != std::string::npos, "bare_warning: the message must name the kind (warn)");
        expect(m.find("radio %d would not open") != std::string::npos, "bare_warning: the message must show the text of the site");
        expect(m.find("noteRecovered") != std::string::npos && m.find("errpath_allow.tsv") != std::string::npos,
               "bare_warning: the message must say what the author does (report, or add an allow line)");
        expect(m.find("src/a.cpp::openIt::warn::radio % would not open") != std::string::npos, "bare_warning: the message must carry the key to paste");
        expect(!r.result.ok(), "bare_warning: the gate as a whole must fail");
    }
    // 2. the same with a reporter call in the same block is GREEN
    {
        const Run r = run("with_reporter");
        expect(r.result.unresolved.empty() && r.result.ok(), "with_reporter: a warning followed by noteRecovered in the same block must be GREEN");
        expect(r.result.reported == 1, "with_reporter: the site must be counted as passing by a reporter");
    }
    // 3. the same with an allow line is GREEN
    {
        const Run r = run("with_allow");
        expect(r.result.ok() && r.result.allowed == 1, "with_allow: a warning with an ALLOW line and a reason must be GREEN");
    }
    // 4. an allow line whose site does not exist is RED (the list cannot rot)
    {
        const Run r = run("stale_allow");
        expect(r.scan.sites.empty(), "stale_allow: the fixture has no site");
        expect(r.result.stale.size() == 1 && !r.result.ok(), "stale_allow: an allow line with no site must be RED");
    }
    // 5. a new error assignment is RED
    {
        const Run r = run("new_errassign");
        expect(find(r, "errassign", "failIt") != nullptr, "new_errassign: an assignment to lastError_ must be found as a site (kind errassign)");
        expect(r.result.unresolved.size() == 1, "new_errassign: a new error assignment with no reporter and no allow line must be RED");
    }
    // 6. an empty catch is RED
    {
        const Run r = run("empty_catch");
        expect(find(r, "swallow", "tryIt") != nullptr, "empty_catch: an empty catch (...) must be found as a site (kind swallow)");
        expect(r.result.unresolved.size() == 1, "empty_catch: an empty catch with no allow line must be RED");
    }
    // 7. a function moved to another file: the old line is stale, the new site is unresolved, and the message says it moved
    {
        const Run r = run("moved");
        expect(r.result.unresolved.size() == 1, "moved: the function now in src/b.cpp has no allow line under its new file: RED");
        expect(r.result.stale.size() == 1, "moved: the allow line still names src/a.cpp where the site no longer is: RED (stale)");
        expect(!r.result.unresolved.empty() && r.result.unresolved[0].find("HINT") != std::string::npos && r.result.unresolved[0].find("src/a.cpp") != std::string::npos,
               "moved: the message must say the function moved from src/a.cpp and to edit the old entry's file part");
    }
    // 8. a pure line shift is GREEN against the SAME allow list
    {
        const Run r = run("line_shift", "with_allow");
        expect(r.scan.sites.size() == 1 && r.result.ok(), "line_shift: 50 lines added above the function, the same allow list: must stay GREEN (no line numbers in keys)");
        const Run base = run("with_allow");
        expect(!r.scan.sites.empty() && !base.scan.sites.empty() && r.scan.sites[0].line != base.scan.sites[0].line && r.scan.sites[0].key == base.scan.sites[0].key,
               "line_shift: the line number must differ while the key is identical");
    }
    // 9. the reporter must be in a block that ENCLOSES the site
    {
        const Run r = run("sibling_branch");
        expect(r.result.unresolved.size() == 1, "sibling_branch: a reporter in the else branch does not cover a warning in the if branch: RED");
        const Run e = run("enclosing_block");
        expect(e.result.ok() && e.result.reported == 1, "enclosing_block: a reporter in the function body covers a warning in a nested if block: GREEN");
    }
    // 10. what is not a site
    {
        const Run r = run("not_sites");
        expect(r.scan.sites.empty(), "not_sites: comments, strings, raw strings, informational diagLogf, clears of an error field, comparisons, a class's default member, "
                                     "a catch that does something and an assigned Pa_ result are NOT sites; found " + std::to_string(r.scan.sites.size()) +
                                     (r.scan.sites.empty() ? std::string() : " first: " + r.scan.sites[0].kind + " " + r.scan.sites[0].snippet));
        expect(run("real_log_failure").scan.sites.size() == 1, "real_log_failure: a diagLogf whose text says 'failed' IS a site (kind log)");
    }
    // 11. vendor calls whose result is dropped
    {
        const Run r = run("vendor_ignored");
        int n = 0;
        for (const auto& s : r.scan.sites) { if (s.kind == "ignored") { ++n; } }
        expect(n == 4, "vendor_ignored: Pa_CloseStream; (void)Pa_AbortStream; api_->hackrf_close; if (x) rtlsdr_close are four ignored results, the assigned Pa_StopStream is not; found " + std::to_string(n));
    }
    // 12. function names: in-class, constructor with a brace initialiser, out-of-line
    {
        const Run r = run("class_names");
        expect(find(r, "warn", "Foo::inlineFail") != nullptr, "class_names: an in-class function is named Class::function");
        expect(find(r, "warn", "Foo::Foo") != nullptr, "class_names: a constructor with a member brace-initialiser is named Foo::Foo");
        expect(find(r, "warn", "Foo::outOfLine") != nullptr, "class_names: an out-of-line definition keeps its own qualifier");
    }
    // 13. two identical sites in one function get distinct keys
    {
        const Run r = run("duplicates");
        expect(r.scan.sites.size() == 2 && r.scan.sites[0].key != r.scan.sites[1].key && r.scan.sites[1].key.find("#2") != std::string::npos,
               "duplicates: the second identical site must carry #2 in its key");
    }
    // 14. PENDING: counted, pinned, ratchets down only
    {
        expect(run("pending", "", 1).result.ok(), "pending: one PENDING line and a pin of 1 is GREEN");
        const Run up = run("pending", "", 0);
        expect(!up.result.ok() && mentions(up.result.problems, "rose"), "pending: one PENDING line against a pin of 0 (the count rose) must be RED");
        const Run down = run("pending", "", 2);
        expect(!down.result.ok() && mentions(down.result.problems, "lower the pin"), "pending: one PENDING line against a pin of 2 (it fell, the pin was not lowered) must be RED");
        expect(run("pending", "", 1).result.pendingByGroup["G1-test"] == 1, "pending: the group G1-test must be counted");
        const Run now = run("pending_now_reports");
        expect(mentions(now.result.problems, "STALE PENDING"), "pending_now_reports: a PENDING line whose site now reports must be RED (remove the line, lower the pin)");
    }
    // 15. allow-list hygiene
    {
        expect(mentions(run("bad_reason").result.problems, "reason must be"), "bad_reason: an ALLOW reason of one character must be RED");
        expect(mentions(run("detail_dangling").result.problems, "DETAIL-OF"), "detail_dangling: DETAIL-OF a site that does not exist must be RED");
        expect(run("detail_ok").result.ok(), "detail_ok: DETAIL-OF a site that itself reports is GREEN");
        expect(mentions(run("duplicate_key").result.problems, "twice"), "duplicate_key: the same key twice in the allow list must be RED");
    }
    // 16. tests are not product code
    {
        expect(run("tests_excluded").scan.sites.empty(), "tests_excluded: test_*.cpp files and anything under /tests/ are not scanned");
    }
    return testSummary("errpath_gate_selftest");
}
