// Tests for core/report_reader.hpp - the tool that turns an uploaded
// module+offset back into a function and a line.
//
// THE FIXTURE ARCHIVE IS REAL, and it has to be. A symboliser can be made to
// pass against a hand-written table of fake symbols while being completely
// unable to read a PDB, which is the only thing it will ever be asked to do.
// So this test builds an archive out of THIS TEST BINARY'S OWN PDB, in exactly
// the layout tools/archive-symbols.ps1 writes, computes a real offset of a
// real function in this module, and requires the reader to name that function
// and the line it is on.
//
// The negative half matters as much: a report whose build id is not in the
// archive must SAY SO, naming the id, and must never print raw hex as though
// it were an answer - because the same offset in a different build is a
// different function, and an unqualified offset invites somebody to look up
// the wrong line and believe it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/diag_report.hpp"
#include "core/report_reader.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;
using namespace cascade::core;

namespace {

template <typename T>
T at(const std::vector<T>& v, std::size_t i) {
    return (i < v.size()) ? v[i] : T();
}

// The nlohmann equivalent of at() above, for exactly the same reason: an
// assertion written as `if (arr.size() == 2) { CHECK(arr[0]...); }` VANISHES
// when the guard is false, so the run that has something to report is the one
// that silently stops reporting it. Indexing through this instead keeps every
// CHECK at the top level, where it can fail by name.
nlohmann::json jat(const nlohmann::json& a, std::size_t i) {
    return (i < a.size()) ? a[i] : nlohmann::json::object();
}

nlohmann::json parseOrEmpty(const std::string& s) {
    nlohmann::json j = nlohmann::json::parse(s, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { return nlohmann::json::object(); }
    return j;
}

fs::path scratchDir(const char* tag) {
    const char* tmp = std::getenv("TEMP");
    const fs::path base = (tmp != nullptr && *tmp != '\0') ? fs::path(tmp) : fs::path(".");
#if defined(_WIN32)
    const unsigned long pid = ::GetCurrentProcessId();
#else
    const unsigned long pid = 0;
#endif
    const fs::path dir =
        base / (std::string("cascade-reader-") + tag + "-" + std::to_string(pid));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    return dir;
}

// THE ANCHOR. A function with a body nothing else in this binary shares, so
// /OPT:ICF cannot fold it into an identical one and hand the test somebody
// else's name. Its address is taken below, which is also what keeps /OPT:REF
// from removing it.
#if defined(_MSC_VER)
#pragma optimize("", off)
#endif
int anchorFunctionForSymbols(int x) {
    volatile int acc = 0x5EED;
    acc += x * 7919;
    acc ^= 0x0BADC0DE;
    return static_cast<int>(acc);
}
#if defined(_MSC_VER)
#pragma optimize("", on)
#endif

std::string selfExePath() {
#if defined(_WIN32)
    char buf[MAX_PATH] = {};
    const DWORD n = ::GetModuleFileNameA(nullptr, buf, static_cast<DWORD>(sizeof(buf)));
    return std::string(buf, buf + n);
#else
    return std::string();
#endif
}

// One uploaded report in exactly the shape the wire contract defines, so the
// reader is fed what the endpoint will actually hand it.
nlohmann::json makeReport(const std::string& signature, const std::string& module,
                          const std::string& buildId, std::uint64_t offset,
                          const std::string& version, const std::string& receivedAt) {
    nlohmann::json j;
    j["schema"] = 1;
    j["kind"] = "crash";
    j["version"] = version;
    j["commit"] = "abc123def456";
    j["buildId"] = buildId;
    j["module"] = module;
    j["offset"] = offset;
    j["signature"] = signature;
    j["os"] = "Windows 10.0.22631";
    j["arch"] = "x64";
    j["installId"] = "4f9c1d2e3a4b5c6d7e8f90a1b2c3d4e5";
    j["receivedAt"] = receivedAt;
    j["plugins"] = nlohmann::json::array(
        {nlohmann::json{{"name", "ADS-B"}, {"version", "1.1.0"}, {"buildId", ""}}});
    j["context"] = nlohmann::json{{"mode", "WFM"},
                                  {"source", "soapy"},
                                  {"sampleRate", 2400000.0},
                                  {"deviceOpen", true},
                                  {"sdrModel", "uhd b200"}};
    j["log"] = nlohmann::json::array({"source opened", "plugin started"});
    nlohmann::json frame;
    frame["module"] = module;
    frame["buildId"] = buildId;
    frame["offset"] = offset;
    j["threads"] = nlohmann::json::array(
        {nlohmann::json{{"id", 24180}, {"frames", nlohmann::json::array({frame})}}});
    return j;
}

std::string feedOf(const std::vector<nlohmann::json>& reports) {
    nlohmann::json j;
    j["reports"] = reports;
    return j.dump();
}

}  // namespace

int main() {
    // Keep the anchor referenced from a path the optimiser cannot fold away.
    const int anchorValue = anchorFunctionForSymbols(3);
    CHECK(anchorValue != 0);

    // --- The feed parses into the fields the contract names ----------------
    {
        std::string err;
        int skipped = -1;
        const std::vector<ReaderReport> rs = parseReportFeed(
            feedOf({makeReport("AAAA0000AAAA0000", "cascade.exe", "DEADBEEF1", 0x1A2B, "0.62.0",
                               "2026-08-25T10:00:00Z")}),
            err, &skipped);
        CHECK(err.empty());
        CHECK(skipped == 0);
        CHECK(rs.size() == 1);
        const ReaderReport r = at(rs, 0);
        CHECK(r.schema == 1);
        CHECK(r.kind == "crash");
        CHECK(r.signature == "AAAA0000AAAA0000");
        CHECK(r.module == "cascade.exe");
        CHECK(r.buildId == "DEADBEEF1");
        CHECK(r.offset == 0x1A2Bull);
        CHECK(r.version == "0.62.0");
        CHECK(r.os == "Windows 10.0.22631");
        CHECK(r.receivedAt == "2026-08-25T10:00:00Z");
        CHECK(r.mode == "WFM");
        CHECK(r.sampleRateHz == 2400000.0);
        CHECK(r.deviceOpen);
        CHECK(r.sdrModel == "uhd b200");
        CHECK(r.plugins.size() == 1);
        CHECK(at(r.plugins, 0).name == "ADS-B");
        CHECK(r.log.size() == 2);
        CHECK(r.threads.size() == 1);
        CHECK(at(r.threads, 0).frames.size() == 1);
        CHECK(at(at(r.threads, 0).frames, 0).offset == 0x1A2Bull);
    }

    // --- A bare array is a feed too, and rubbish is refused loudly ---------
    {
        std::string err;
        const std::vector<ReaderReport> rs = parseReportFeed(
            nlohmann::json::array({makeReport("A", "cascade.exe", "B", 1, "0.62.0", "")}).dump(),
            err, nullptr);
        CHECK(err.empty());
        CHECK(rs.size() == 1);

        std::string err2;
        const std::vector<ReaderReport> bad = parseReportFeed("not json at all", err2, nullptr);
        CHECK(!err2.empty());
        CHECK(bad.empty());

        // ONE unreadable report in a good feed is skipped and COUNTED, not
        // allowed to lose the rest of the export.
        nlohmann::json mixed;
        mixed["reports"] = nlohmann::json::array(
            {makeReport("A", "cascade.exe", "B", 1, "0.62.0", ""), nlohmann::json("garbage"),
             makeReport("C", "cascade.exe", "B", 2, "0.62.0", "")});
        std::string err3;
        int skipped = 0;
        const std::vector<ReaderReport> some = parseReportFeed(mixed.dump(), err3, &skipped);
        CHECK(err3.empty());
        CHECK(some.size() == 2);
        CHECK(skipped == 1);
    }

#if defined(_WIN32)
    // --- THE FIXTURE ARCHIVE, built from this binary's own PDB -------------
    const fs::path archiveRoot = scratchDir("archive");
    const std::string exePath = selfExePath();
    std::string realBuildId;
    std::string pdbName;
    CHECK(peBuildId(exePath, realBuildId, pdbName));
    CHECK(!realBuildId.empty());
    CHECK(!pdbName.empty());

    const fs::path pdbBeside = fs::path(exePath).parent_path() / pdbName;
    const bool havePdb = fs::exists(pdbBeside);
    // A Release build of this project MUST produce a PDB - CMakeLists.txt adds
    // /Zi and /DEBUG for exactly that reason. If this ever fails, the symbol
    // archive is worthless and every report ever filed is unreadable hex, so
    // it is an assertion rather than a skip.
    CHECK(havePdb);
    // The copy result is asserted at the TOP LEVEL, not inside `if (havePdb)`:
    // an assertion nested in a guard vanishes instead of failing, and "the PDB
    // was missing so the archive was never populated" is precisely the run that
    // has to say so. copy_file on a source that does not exist sets `copyEc`,
    // so this stays one honest expression either way.
    const fs::path dest = archiveRoot / pdbName / realBuildId / pdbName;
    std::error_code copyEc;
    fs::create_directories(dest.parent_path(), copyEc);
    copyEc.clear();
    fs::copy_file(pdbBeside, dest, fs::copy_options::overwrite_existing, copyEc);
    CHECK(havePdb && !copyEc);

    const std::string exeName = fs::path(exePath).filename().string();
    const auto base = reinterpret_cast<std::uintptr_t>(::GetModuleHandleA(nullptr));
    const auto anchorAddr = reinterpret_cast<std::uintptr_t>(&anchorFunctionForSymbols);
    CHECK(anchorAddr > base);
    const std::uint64_t anchorOffset = static_cast<std::uint64_t>(anchorAddr - base);

    // --- A real offset resolves to a real function and a real line ---------
    {
        SymbolArchive archive(archiveRoot.string());
        CHECK(archive.exists());
        CHECK(!archive.pdbPath(exeName, realBuildId).empty());

        const SymbolResult s = archive.resolve(exeName, realBuildId, anchorOffset);
        std::printf("resolved: %s (%s:%d) note=%s\n", s.function.c_str(), s.file.c_str(),
                    s.line, s.note.c_str());
        CHECK(s.resolved);
        CHECK(s.function.find("anchorFunctionForSymbols") != std::string::npos);
        // The line number is the half that turns "somewhere in this function"
        // into "this statement", and it is the half most easily lost by
        // loading a PDB without SYMOPT_LOAD_LINES.
        CHECK(s.line > 0);
        CHECK(s.file.find("test_report_reader.cpp") != std::string::npos);
        CHECK(s.note.empty());
    }

    // --- AN UNKNOWN BUILD ID SAYS SO, AND NAMES IT -------------------------
    {
        SymbolArchive archive(archiveRoot.string());
        const std::string unknown = "0123456789ABCDEF0123456789ABCDEF1";
        const SymbolResult s = archive.resolve(exeName, unknown, anchorOffset);
        CHECK(!s.resolved);
        CHECK(s.function.empty());
        CHECK(s.line == 0);
        // The id must be IN the note. "symbols not found" without it sends the
        // reader nowhere; with it, they can go and look on the NAS.
        CHECK(s.note.find(unknown) != std::string::npos);

        // A report with NO build id at all - the module carried no CodeView
        // record - is a different sentence, and must not claim an id it does
        // not have.
        const SymbolResult none = archive.resolve("mystery.dll", std::string(), 0x40);
        CHECK(!none.resolved);
        CHECK(!none.note.empty());
        CHECK(none.note.find("mystery.dll") != std::string::npos);
    }

    // --- GROUPING: counts, first/last seen, affected versions --------------
    {
        SymbolArchive archive(archiveRoot.string());
        std::string err;
        const std::vector<ReaderReport> rs = parseReportFeed(
            feedOf({
                makeReport("SIGONE", exeName, realBuildId, anchorOffset, "0.62.0",
                           "2026-08-20T09:00:00Z"),
                makeReport("SIGONE", exeName, realBuildId, anchorOffset, "0.62.1",
                           "2026-08-25T09:00:00Z"),
                makeReport("SIGONE", exeName, realBuildId, anchorOffset, "0.62.0",
                           "2026-08-22T09:00:00Z"),
                makeReport("SIGTWO", exeName, "0123456789ABCDEF0123456789ABCDEF1", 0x40,
                           "0.62.0", "2026-08-21T09:00:00Z"),
            }),
            err, nullptr);
        CHECK(rs.size() == 4);

        const std::vector<ReportGroup> groups = groupReports(rs, archive);
        CHECK(groups.size() == 2);
        // Newest first: what is happening now, then what used to.
        CHECK(at(groups, 0).signature == "SIGONE");
        CHECK(at(groups, 0).count == 3);
        CHECK(at(groups, 0).firstSeen == "2026-08-20T09:00:00Z");
        CHECK(at(groups, 0).lastSeen == "2026-08-25T09:00:00Z");
        CHECK(at(groups, 0).versions.size() == 2);
        CHECK(at(at(groups, 0).versions, 0) == "0.62.0");
        CHECK(at(at(groups, 0).versions, 1) == "0.62.1");
        CHECK(at(groups, 0).symbol.resolved);
        CHECK(at(groups, 0).symbol.function.find("anchorFunctionForSymbols") !=
              std::string::npos);
        CHECK(at(groups, 0).frames.size() == 1);
        CHECK(at(at(groups, 0).frames, 0).symbol.resolved);
        CHECK(at(groups, 0).missingBuildIds.empty());

        CHECK(at(groups, 1).signature == "SIGTWO");
        CHECK(at(groups, 1).count == 1);
        CHECK(!at(groups, 1).symbol.resolved);
        CHECK(at(groups, 1).missingBuildIds.size() == 1);
        CHECK(at(at(groups, 1).missingBuildIds, 0) == "0123456789ABCDEF0123456789ABCDEF1");

        // --- The human rendering says both things, in words ----------------
        const std::string text = renderGroupsText(groups, 4, 0, archive);
        CHECK(text.find("SIGONE") != std::string::npos);
        CHECK(text.find("anchorFunctionForSymbols") != std::string::npos);
        CHECK(text.find("test_report_reader.cpp") != std::string::npos);
        CHECK(text.find("0.62.0") != std::string::npos);
        CHECK(text.find("0.62.1") != std::string::npos);
        CHECK(text.find("2026-08-25T09:00:00Z") != std::string::npos);
        // The unreadable group names the id it needs...
        CHECK(text.find("0123456789ABCDEF0123456789ABCDEF1") != std::string::npos);
        // ...and NEVER offers a raw offset as if it were the answer. "+0x40"
        // in a group with no symbols is the exact thing that gets somebody to
        // look up the wrong line in the wrong build and believe it.
        CHECK(text.find("+0x40") == std::string::npos);
        // The install id is read off the wire and never printed: it is there so
        // the SERVER can rate-limit a machine, not so a reader can follow one.
        CHECK(text.find("4f9c1d2e3a4b5c6d7e8f90a1b2c3d4e5") == std::string::npos);

        // --- ...and the JSON carries the same answers ----------------------
        const std::string js = renderGroupsJson(groups, 4, 0);
        const nlohmann::json j = parseOrEmpty(js);
        CHECK(j.value("totalReports", 0) == 4);
        CHECK(j.contains("groups") && j["groups"].is_array());
        const nlohmann::json arr =
            (j.contains("groups") && j["groups"].is_array()) ? j["groups"]
                                                             : nlohmann::json::array();
        CHECK(arr.size() == 2);
        CHECK(jat(arr, 0).value("signature", std::string()) == "SIGONE");
        CHECK(jat(arr, 0).value("count", 0) == 3);
        CHECK(jat(arr, 0).value("firstSeen", std::string()) == "2026-08-20T09:00:00Z");
        CHECK(jat(arr, 0).value("lastSeen", std::string()) == "2026-08-25T09:00:00Z");
        CHECK(jat(arr, 0).value("symbolised", false));
        CHECK(jat(arr, 1).value("signature", std::string()) == "SIGTWO");
        CHECK(!jat(arr, 1).value("symbolised", true));
        // The JSON must not carry the install id either.
        CHECK(js.find("4f9c1d2e3a4b5c6d7e8f90a1b2c3d4e5") == std::string::npos);
    }

    // --- A FRAME INSIDE AN UNARCHIVED PLUGIN, in an otherwise readable stack
    //
    // The most common real shape: the application's frames resolve, the plugin
    // that actually failed does not, and the reader has to say which build id
    // it would need rather than quietly printing a shorter stack.
    {
        SymbolArchive archive(archiveRoot.string());
        nlohmann::json r = makeReport("MIXED", exeName, realBuildId, anchorOffset, "0.62.0",
                                      "2026-08-25T09:00:00Z");
        nlohmann::json plugin;
        plugin["module"] = "adsb.dll";
        plugin["buildId"] = "AAAABBBBCCCCDDDDEEEEFFFF000011112";
        plugin["offset"] = 0x1234;
        r["threads"][0]["frames"].push_back(plugin);
        std::string err;
        const std::vector<ReaderReport> rs = parseReportFeed(feedOf({r}), err, nullptr);
        CHECK(rs.size() == 1);
        const std::vector<ReportGroup> groups = groupReports(rs, archive);
        CHECK(groups.size() == 1);
        CHECK(at(groups, 0).frames.size() == 2);
        CHECK(at(at(groups, 0).frames, 0).symbol.resolved);
        CHECK(!at(at(groups, 0).frames, 1).symbol.resolved);
        CHECK(at(at(groups, 0).frames, 1).symbol.note.find(
                  "AAAABBBBCCCCDDDDEEEEFFFF000011112") != std::string::npos);
        CHECK(at(groups, 0).missingBuildIds.size() == 1);

        const std::string text = renderGroupsText(groups, 1, 0, archive);
        CHECK(text.find("anchorFunctionForSymbols") != std::string::npos);
        CHECK(text.find("adsb.dll") != std::string::npos);
        CHECK(text.find("AAAABBBBCCCCDDDDEEEEFFFF000011112") != std::string::npos);
    }

    // --- An archive that is not there is said plainly, not crashed on ------
    {
        SymbolArchive missing((archiveRoot / "not-here").string());
        CHECK(!missing.exists());
        const SymbolResult s = missing.resolve(exeName, realBuildId, anchorOffset);
        CHECK(!s.resolved);
        CHECK(!s.note.empty());
    }

    // --- An unauthenticated fetch is refused rather than attempted ---------
    {
        std::string body;
        std::string err;
        CHECK(!fetchReports("https://foxsdr.com/api/crash/reports", std::string(), body, err));
        CHECK(!err.empty());
        CHECK(body.empty());
    }

    {
        std::error_code ec;
        fs::remove_all(archiveRoot, ec);
    }
#endif  // _WIN32

    return testSummary("test_report_reader");
}
