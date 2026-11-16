// THE ERROR-PATH GATE.
//
// The owner's rule: no radio, sound device, plugin, update or output may fail in any install without him
// being told. This test reads src/ as TEXT (it needs no build configuration, no generated file and does not
// link the engine) and finds every place the code notices a failure - a warning, a log line with failure
// wording, a setError call, an assignment to an error field, a catch that swallows, a vendor call whose
// result is dropped, a line on stderr (tests/errpath_scan.hpp has the exact patterns). Each site must pass
// ONE of three ways:
//
//   1. a reporting call (tests/errpath_reporters.txt: noteRecovered, raiseRecovered, noteRadioFail,
//      noteSoundFail, the note*Failed calls, fileIssueReport) in a brace block that encloses it;
//   2. a line in tests/errpath_allow.tsv with a reason (pure information, the user's own action, a test
//      seam, the detail of a failure reported one level up);
//   3. a PENDING:<group> line in the same file: a known unreported site that a later lane will fix. The
//      NUMBER of PENDING lines is pinned below; adding one fails, removing one means lowering the pin.
//
// A site that passes none of them fails the build and says which file, which function and which text, and
// what the author must do. The allow list is keyed WITHOUT line numbers (file::function::kind::stem), so
// moving code inside a file breaks nothing, and a line whose site no longer exists is a failure too, so
// the list cannot rot.
//
//   test_error_paths [repo-root] [--allow file] [--dump out.tsv]
//     --dump writes every site (key, file, function, kind, reported, reporter, snippet, line) for
//     tools/errpath_seed.py and exits 0 without judging.
//
// To seed or re-seed after a merge:   python tools/errpath_seed.py   (docs/multi-radio-records/errpath-gate.md)
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "errpath_scan.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;

// THE PIN: how many sites are parked as PENDING in tests/errpath_allow.tsv. It may only go DOWN.
// (tools/errpath_seed.py --write-pin keeps this line in step after a re-seed.)
constexpr int kPendingPin = 1126;   // errpath-pin

namespace {

void expect(bool ok, const std::string& msg) {
    ++g_checksRun;
    if (!ok) {
        ++g_checksFailed;
        std::printf("FAIL %s\n", msg.c_str());
    }
}

fs::path findRoot() {
    fs::path p = fs::current_path();
    for (int i = 0; i < 8; ++i) {
        if (fs::exists(p / "src") && fs::exists(p / "tests" / "errpath_allow.tsv")) { return p; }
        if (!p.has_parent_path() || p.parent_path() == p) { break; }
        p = p.parent_path();
    }
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    fs::path root;
    std::string allowPath;
    std::string dumpPath;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--allow" && i + 1 < argc) { allowPath = argv[++i]; }
        else if (a == "--dump" && i + 1 < argc) { dumpPath = argv[++i]; }
        else { root = a; }
    }
    if (root.empty()) { root = findRoot(); }
    if (root.empty() || !fs::exists(root / "src")) {
        std::printf("FAIL cannot find the repository root (give it as the first argument)\n");
        return 1;
    }
    if (allowPath.empty()) { allowPath = (root / "tests" / "errpath_allow.tsv").string(); }

    errpath::Options opt;
    opt.root = root.string();
    opt.profile = "engine";
    opt.reporters = errpath::readReporters((root / "tests" / "errpath_reporters.txt").string());

    const errpath::Scan scan = errpath::scanTree(opt);
    if (!dumpPath.empty()) {
        std::FILE* f = std::fopen(dumpPath.c_str(), "wb");
        if (!f) { std::printf("FAIL cannot write %s\n", dumpPath.c_str()); return 1; }
        errpath::dumpSites(scan.sites, f);
        std::fclose(f);
        std::printf("dumped %zu sites from %d files (reporters: %zu)\n", scan.sites.size(), scan.files, opt.reporters.size());
        for (const auto& u : scan.unbalancedFiles) { std::printf("NOTE unbalanced braces in %s\n", u.c_str()); }
        return 0;
    }

    // 0. the list of reporters is real: each name is declared in the health or issue-report header
    {
        const std::string h = errpath::readFile(root / "src" / "core" / "health_events.hpp") + errpath::readFile(root / "src" / "core" / "issue_report.hpp");
        expect(!opt.reporters.empty(), "tests/errpath_reporters.txt lists no reporter");
        for (const std::string& name : opt.reporters) {
            bool declared = false;
            std::size_t p = 0;
            while ((p = h.find(name, p)) != std::string::npos) {
                const std::size_t e = p + name.size();
                if ((p == 0 || !errpath::isIdentChar(h[p - 1])) && e < h.size() && (h[e] == '(' )) { declared = true; break; }
                p = e;
            }
            expect(declared, "reporter '" + name + "' in tests/errpath_reporters.txt is not declared in core/health_events.hpp or core/issue_report.hpp (renamed? update the list)");
        }
    }
    // 1. the plugins repository's copy of the scanner is the same file
    {
        const fs::path copy = root / "tools" / "errpath-for-plugins" / "errpath_scan.hpp";
        if (fs::exists(copy)) {
            expect(errpath::readFile(copy) == errpath::readFile(root / "tests" / "errpath_scan.hpp"),
                   "tools/errpath-for-plugins/errpath_scan.hpp differs from tests/errpath_scan.hpp: copy the scanner over (the plugins gate must scan the same way)");
        }
        const fs::path seedCopy = root / "tools" / "errpath-for-plugins" / "errpath_seed.py";
        if (fs::exists(seedCopy)) {
            expect(errpath::readFile(seedCopy) == errpath::readFile(root / "tools" / "errpath_seed.py"),
                   "tools/errpath-for-plugins/errpath_seed.py differs from tools/errpath_seed.py: copy the seed tool over");
        }
    }
    // 2. the scan found something (an empty scan would pass everything)
    expect(scan.files > 500, "the scan read only " + std::to_string(scan.files) + " files of src/: the walk is broken");
    expect(scan.sites.size() > 3000, "the scan found only " + std::to_string(scan.sites.size()) + " sites: the patterns are broken");
    for (const auto& u : scan.unbalancedFiles) { std::printf("NOTE unbalanced braces in %s (functions after the fault may be misnamed)\n", u.c_str()); }

    // 3. the gate
    std::vector<std::string> problems;
    const std::string allowText = errpath::readFile(allowPath);
    expect(!allowText.empty(), "cannot read the allow list " + allowPath);
    const std::vector<errpath::AllowEntry> allow = errpath::parseAllow(allowText, &problems);
    opt.pin = kPendingPin;
    errpath::Result r = errpath::check(scan.sites, allow, opt);
    for (auto& p : problems) { r.problems.push_back(p); }
    errpath::printResult(r, stdout);
    expect(r.unresolved.empty(), std::to_string(r.unresolved.size()) + " error path(s) are neither reported nor in the allow list (messages above)");
    expect(r.stale.empty(), std::to_string(r.stale.size()) + " allow-list line(s) name a site that no longer exists (messages above)");
    expect(r.problems.empty(), std::to_string(r.problems.size()) + " allow-list or pin problem(s) (messages above)");
    return testSummary("test_error_paths");
}
