// errpath_check - the error-path gate as a stand-alone program (for the plugins repository).
//
//   errpath_check <repo-root> [--profile plugins|engine] [--dir src] [--allow tools/errpath_allow.tsv]
//                 [--reporters tools/errpath_reporters.txt] [--pin-file tools/errpath_pin.txt] [--dump out.tsv]
//
// Exit 0: every site is reported, allowed with a reason, or PENDING within the pinned count.
// Exit 1: the gate is RED (the messages say which file, which function, which text and what to do).
// --dump writes every site (for errpath_seed.py) and exits 0.
//
// Same scanner as the engine's tests/test_error_paths.cpp (errpath_scan.hpp is the same file).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "errpath_scan.hpp"

int main(int argc, char** argv) {
    namespace fs = std::filesystem;
    errpath::Options opt;
    opt.profile = "plugins";
    opt.dirs = {};
    std::string allow, reporters, pinFile, dump;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--profile") { opt.profile = next(); }
        else if (a == "--dir") { opt.dirs.push_back(next()); }
        else if (a == "--allow") { allow = next(); }
        else if (a == "--reporters") { reporters = next(); }
        else if (a == "--pin-file") { pinFile = next(); }
        else if (a == "--dump") { dump = next(); }
        else { opt.root = a; }
    }
    if (opt.root.empty()) { opt.root = "."; }
    if (opt.dirs.empty()) { opt.dirs.push_back("src"); }
    if (allow.empty()) { allow = (fs::path(opt.root) / "tools" / "errpath_allow.tsv").string(); }
    if (reporters.empty()) { reporters = (fs::path(opt.root) / "tools" / "errpath_reporters.txt").string(); }
    if (pinFile.empty()) { pinFile = (fs::path(opt.root) / "tools" / "errpath_pin.txt").string(); }
    opt.reporters = errpath::readReporters(reporters);
    opt.allowName = fs::path(allow).generic_string();

    const errpath::Scan scan = errpath::scanTree(opt);
    if (!dump.empty()) {
        std::FILE* f = std::fopen(dump.c_str(), "wb");
        if (!f) { std::printf("cannot write %s\n", dump.c_str()); return 1; }
        errpath::dumpSites(scan.sites, f);
        std::fclose(f);
        std::printf("dumped %zu sites from %d files\n", scan.sites.size(), scan.files);
        return 0;
    }
    if (scan.files == 0) { std::printf("FAIL no source file under %s/%s: wrong root?\n", opt.root.c_str(), opt.dirs[0].c_str()); return 1; }
    for (const auto& u : scan.unbalancedFiles) { std::printf("NOTE unbalanced braces in %s\n", u.c_str()); }
    std::vector<std::string> problems;
    const auto entries = errpath::parseAllow(errpath::readFile(allow), &problems);
    const std::string pinText = errpath::readFile(pinFile);
    opt.pin = pinText.empty() ? -1 : std::atoi(pinText.c_str());
    errpath::Result r = errpath::check(scan.sites, entries, opt);
    for (auto& p : problems) { r.problems.push_back(p); }
    if (pinText.empty()) { r.problems.push_back("no pin file " + pinFile + ": create it (errpath_seed.py --write-pin --pin-file ...)"); }
    errpath::printResult(r, stdout);
    std::printf("%s\n", r.ok() ? "errpath_check: GREEN" : "errpath_check: RED");
    return r.ok() ? 0 : 1;
}
