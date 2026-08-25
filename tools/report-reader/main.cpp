// foxsdr-reports - pull uploaded fault reports and symbolise them locally.
//
// THE PRIMARY CONSUMER IS A FUTURE ENGINEERING SESSION, so this prints two
// things from one run: a human summary grouped by signature, and the same
// answers as JSON. Grouping is the whole point - four hundred reports are
// three bugs, and nothing else in the pipeline can tell you which three.
//
//   foxsdr-reports --archive C:\...\cascade\symbols
//   foxsdr-reports --file exported.json --json --out groups.json
//
// SYMBOLS NEVER LEAVE THIS MACHINE. The reports arrive as module+offset and
// are resolved here, against the PDB archive that tools/archive-symbols.ps1
// writes on every build. See core/report_reader.hpp for why that is a decision
// rather than a limitation.
//
// THE TOKEN COMES FROM THE ENVIRONMENT (FOXSDR_REPORTS_TOKEN) and is never
// compiled in and never put in a query string. The UPLOAD path is anonymous
// and unauthenticated on purpose - a secret shipped in every binary is not a
// secret - but reading the reports back is the opposite question.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/report_reader.hpp"
#include "core/version.hpp"

namespace fs = std::filesystem;
using namespace cascade::core;

namespace {

void usage() {
    std::printf(
        "foxsdr-reports %s - read and symbolise uploaded fault reports\n"
        "\n"
        "  --archive DIR   the PDB archive (default: $FOXSDR_SYMBOLS, then the\n"
        "                  symbols/ directory beside this build)\n"
        "  --file PATH     read a saved feed instead of fetching\n"
        "  --url URL       the reports endpoint (default: $FOXSDR_REPORTS_URL,\n"
        "                  then https://foxsdr.com/api/crash/reports)\n"
        "  --json          print JSON instead of the human summary\n"
        "  --out PATH      write the output to a file as well as the screen\n"
        "\n"
        "The token is read from FOXSDR_REPORTS_TOKEN. It is never compiled in.\n",
        cascade::versionString());
}

std::string envOr(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return (v != nullptr && *v != '\0') ? std::string(v) : fallback;
}

// Where the archive is, if nobody said. Deliberately a SEARCH with the answer
// printed, not a silent default: an empty archive and a wrong path produce the
// same output otherwise, and one of them is a five-minute fix.
std::string defaultArchive(const char* argv0) {
    const std::string fromEnv = envOr("FOXSDR_SYMBOLS", std::string());
    if (!fromEnv.empty()) { return fromEnv; }
    std::error_code ec;
    fs::path here = fs::absolute(fs::path(argv0), ec).parent_path();
    for (int i = 0; i < 4 && !here.empty(); ++i) {
        const fs::path candidate = here / "symbols";
        if (fs::is_directory(candidate, ec)) { return candidate.string(); }
        here = here.parent_path();
    }
    return std::string();
}

}  // namespace

int main(int argc, char** argv) {
    std::string archiveRoot;
    std::string file;
    std::string url;
    std::string out;
    bool asJson = false;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto next = [&](std::string& dst) -> bool {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "foxsdr-reports: %s needs a value\n", a);
                return false;
            }
            dst = argv[++i];
            return true;
        };
        if (std::strcmp(a, "--archive") == 0) {
            if (!next(archiveRoot)) { return 2; }
        } else if (std::strcmp(a, "--file") == 0) {
            if (!next(file)) { return 2; }
        } else if (std::strcmp(a, "--url") == 0) {
            if (!next(url)) { return 2; }
        } else if (std::strcmp(a, "--out") == 0) {
            if (!next(out)) { return 2; }
        } else if (std::strcmp(a, "--json") == 0) {
            asJson = true;
        } else if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
            usage();
            return 0;
        } else {
            std::fprintf(stderr, "foxsdr-reports: unknown argument '%s'\n", a);
            usage();
            return 2;
        }
    }

    if (archiveRoot.empty()) { archiveRoot = defaultArchive(argv[0]); }
    if (archiveRoot.empty()) {
        // Said plainly rather than guessed at. Without an archive every report
        // is unreadable hex, and that is worth one clear sentence up front.
        std::fprintf(stderr,
                     "foxsdr-reports: no symbol archive found. Pass --archive DIR or set\n"
                     "                FOXSDR_SYMBOLS. Without it nothing can be symbolised.\n");
    }

    std::string feed;
    if (!file.empty()) {
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "foxsdr-reports: cannot read %s\n", file.c_str());
            return 1;
        }
        feed.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    } else {
        const std::string endpoint = url.empty() ? reportsEndpoint() : url;
        const std::string token = envOr("FOXSDR_REPORTS_TOKEN", std::string());
        std::string err;
        if (!fetchReports(endpoint, token, feed, err)) {
            std::fprintf(stderr, "foxsdr-reports: %s\n", err.c_str());
            return 1;
        }
    }

    std::string err;
    int skipped = 0;
    const std::vector<ReaderReport> reports = parseReportFeed(feed, err, &skipped);
    if (!err.empty()) {
        std::fprintf(stderr, "foxsdr-reports: %s\n", err.c_str());
        return 1;
    }

    SymbolArchive archive(archiveRoot);
    const std::vector<ReportGroup> groups = groupReports(reports, archive);
    const std::string text =
        asJson ? renderGroupsJson(groups, static_cast<int>(reports.size()), skipped)
               : renderGroupsText(groups, static_cast<int>(reports.size()), skipped, archive);

    std::fwrite(text.data(), 1, text.size(), stdout);
    if (!out.empty()) {
        std::ofstream o(out, std::ios::binary | std::ios::trunc);
        if (!o) {
            std::fprintf(stderr, "foxsdr-reports: cannot write %s\n", out.c_str());
            return 1;
        }
        o << text;
    }
    return 0;
}
