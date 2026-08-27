// report_reader.hpp - turning uploaded reports back into something an engineer
// can act on. The half that makes the whole feature worth having.
//
// A report arrives as module+offset. An offset is unreadable hex forever unless
// the PDB produced by THAT EXACT LINK can be found again, so everything here
// exists to do one thing: find the right PDB and print a function and a line.
//
// ---------------------------------------------------------------------------
// SYMBOLISATION HAPPENS HERE, NOT ON THE SERVER
// ---------------------------------------------------------------------------
//
// The PDBs live in symbols\ in the working tree and on
// nas:/volume1/foxsdr-symbols. They are tens of megabytes per link, they are
// the only thing that makes any past report readable, and they must not be
// uploaded anywhere - a symbol server would put the complete private debug
// information for every shipped build on a machine reachable from the
// internet, to save a step nobody is short of. So the server stores the raw
// module+offset it was sent, and the resolution happens on a machine that
// already has the archive: this one.
//
// The consequence, stated rather than discovered: this tool only works where
// the archive is. That is the correct trade, and it is why
// tools/archive-symbols.ps1 runs on every build.
//
// ---------------------------------------------------------------------------
// WHAT IT REFUSES TO PRETEND
// ---------------------------------------------------------------------------
//
// A report whose build id is not in the archive prints exactly that, NAMING
// THE ID, and never prints raw hex as though it were an answer. "cascade.exe
// +0x1A2B" looks like information and is not: the same offset in a different
// build is a different function, so an unqualified offset is worse than
// silence - it invites someone to look up the wrong line and believe it. The
// same applies frame by frame: a stack that crosses into a plugin whose
// symbols were never archived shows the application's frames resolved and says
// which build id it would need for the rest.
//
// ---------------------------------------------------------------------------
// THE ENDPOINT THE READER PULLS FROM
// ---------------------------------------------------------------------------
//
//   GET https://foxsdr.com/api/crash/reports?since=<ISO8601>&limit=<n>
//   Authorization: Bearer <token>      (from FOXSDR_REPORTS_TOKEN; NEVER
//                                       compiled in - the upload path is
//                                       anonymous and unauthenticated, this
//                                       one is the opposite and its secret
//                                       belongs in the environment)
//   200 { "reports": [ <the POSTed body, plus "receivedAt": "<ISO8601>"> ] }
//
// The reader also accepts a local file of the same shape (--file), which is
// what the fixtures use and what makes an archived export readable a year
// later with no service involved.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_REPORT_READER_HPP
#define CASCADE_CORE_REPORT_READER_HPP

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace cascade::core {

struct ReaderFrame {
    std::string module;
    std::string buildId;
    std::uint64_t offset = 0;
};

struct ReaderThread {
    std::uint64_t id = 0;
    std::vector<ReaderFrame> frames;
};

struct ReaderPlugin {
    std::string name;
    std::string version;
    std::string buildId;
};

// One uploaded report, as the reader consumes it. Deliberately the same field
// names as the wire contract: two spellings of one payload is how a consumer
// silently stops reading a field the producer still sends.
struct ReaderReport {
    int schema = 0;
    std::string kind;
    std::string version;
    std::string commit;
    std::string buildId;
    std::string module;
    std::uint64_t offset = 0;
    std::string signature;
    std::string os;
    std::string arch;
    // Read, but never printed and never grouped by. It exists so the RECEIVING
    // END can rate-limit one machine; a reader that put it on screen would turn
    // an anonymous counter into a way to follow one person's crashes around.
    std::string installId;
    std::string receivedAt;

    std::string mode;
    std::string source;
    double sampleRateHz = 0.0;
    bool deviceOpen = false;
    std::string sdrModel;

    std::vector<ReaderPlugin> plugins;
    std::vector<std::string> log;
    std::vector<ReaderThread> threads;
};

// Parses the endpoint's answer, or a file of the same shape. Accepts either
// {"reports":[...]} or a bare array, because an export saved by hand is
// usually the array. `err` is set and the result is empty when the text is not
// JSON at all; a single unreadable report inside a good feed is skipped and
// counted rather than failing the whole run.
std::vector<ReaderReport> parseReportFeed(const std::string& jsonText, std::string& err,
                                          int* skipped = nullptr);

// ---------------------------------------------------------------------------
// The archive
// ---------------------------------------------------------------------------
struct SymbolResult {
    bool resolved = false;
    std::string function;
    std::string file;
    int line = 0;
    // Why it did not resolve, in words, naming the build id. Never empty when
    // `resolved` is false.
    std::string note;
};

// TWO BUILD-ID SPELLINGS, ONE ARCHIVE, and the spelling picks the resolver.
// A Windows PDB key is the CodeView GUID plus age - 33 hex digits, rendered
// uppercase. An ELF key is the NT_GNU_BUILD_ID note - 40 (SHA-1) or 32 (MD5)
// hex digits, rendered lowercase, the way readelf prints it. The two cannot
// collide on length-33, so the id itself says whether a frame wants dbghelp
// on a PDB or addr2line on a split .debug file. Exposed as a free function
// because the tests pin the classification directly.
bool isElfBuildId(const std::string& buildId);

// The symbol-server layout the archivers write:
//
//     <root>\<pdb name>\<GUID><AGE>\<pdb name>            (Windows)
//     <root>/<module>.debug/<gnu id>/<module>.debug       (Linux, split DWARF)
//     <root>/<module>/<gnu id>/<module>                   (Linux, the module)
//
// The module name in a report is the BINARY's ("cascade.exe", "adsb.dll"), so
// the PDB name is derived by replacing the extension - which is what MSVC
// names it by default. When that misses, the whole archive is scanned for a
// directory named by the build id, so a PDB with an unconventional name is
// still found. Nothing is guessed: a build id that is not there is reported as
// not there.
//
// ELF frames resolve through addr2line rather than a private DWARF parser: a
// DWARF line-table reader is a project of its own, and addr2line is the tool
// whose answers everyone already trusts. The binary is found in this order -
// the FOXSDR_ADDR2LINE environment variable, `addr2line` on PATH, and (on
// Windows) `wsl addr2line`, with Windows paths translated to /mnt/<drive>
// form for that last case. No candidate working is reported per frame, in
// words, not silently.
class SymbolArchive {
public:
    explicit SymbolArchive(std::string root);
    ~SymbolArchive();

    SymbolArchive(const SymbolArchive&) = delete;
    SymbolArchive& operator=(const SymbolArchive&) = delete;

    const std::string& root() const { return root_; }
    bool exists() const;

    // The archived PDB for this module and build id, or empty.
    std::string pdbPath(const std::string& moduleName, const std::string& buildId) const;

    // The archived ELF symbol file for this module and GNU build id, or
    // empty. Prefers the split .debug (full DWARF); falls back to the
    // archived module itself (symbol table only), then to the by-id scan.
    std::string elfSymbolPath(const std::string& moduleName, const std::string& buildId) const;

    // addr2line's two-line answer (-f -C) parsed into a result: function,
    // then "file:line" - "??" and "??:0" are its spellings for "unknown".
    // Pure and public because the tests pin the parse directly.
    static bool parseAddr2lineOutput(const std::string& text, SymbolResult& out);

    // Resolve one frame. Never throws, never asserts, and never returns a
    // half-answer: either a function (and a line, when the PDB has one) or a
    // note saying what is missing.
    SymbolResult resolve(const std::string& moduleName, const std::string& buildId,
                         std::uint64_t offset) const;

private:
    // The ELF half of resolve(); see the class comment for the tool search.
    SymbolResult resolveElf(const std::string& moduleName, const std::string& buildId,
                            std::uint64_t offset) const;
    // The addr2line invocation prefix ("addr2line", "wsl addr2line", or the
    // FOXSDR_ADDR2LINE override), probed once; empty when nothing works.
    const std::string& addr2lineTool() const;

    std::string root_;
    // dbghelp keeps its state per "process handle", which for a symbol-only
    // session is just a key. A unique fake one per archive keeps this from
    // colliding with anything else in the process that uses dbghelp.
    void* symHandle_ = nullptr;
    bool symReady_ = false;
    mutable std::string addr2line_;
    mutable bool addr2lineProbed_ = false;
    // buildId -> loaded module base, so a stack of forty frames in one module
    // loads its PDB once rather than forty times.
    mutable std::map<std::string, std::uint64_t> loaded_;
    mutable std::map<std::string, std::string> missing_;
};

// ---------------------------------------------------------------------------
// Grouping - the thing that turns 400 reports into "three bugs"
// ---------------------------------------------------------------------------
struct SymbolisedFrame {
    ReaderFrame frame;
    SymbolResult symbol;
};

struct ReportGroup {
    std::string signature;
    std::string kind;
    int count = 0;
    std::string firstSeen;  // ISO8601, from receivedAt; empty when the feed had none
    std::string lastSeen;
    std::vector<std::string> versions;  // sorted, unique
    std::vector<std::string> osVersions;
    std::vector<std::string> plugins;  // "name version", sorted, unique

    // The representative report: the most recent one in the group, because it
    // is the one whose build is most likely still archived.
    std::string module;
    std::string buildId;
    std::uint64_t offset = 0;
    std::string commit;
    SymbolResult symbol;
    std::vector<SymbolisedFrame> frames;  // the faulting thread's stack
    std::vector<std::string> log;

    // Build ids this group needed and the archive did not have. Named, so the
    // answer to "why is this unreadable" is a build id somebody can go and
    // look for on the NAS rather than a shrug.
    std::vector<std::string> missingBuildIds;
};

// Newest group first, which is the order an engineering session wants: what is
// happening now, then what used to.
std::vector<ReportGroup> groupReports(const std::vector<ReaderReport>& reports,
                                      const SymbolArchive& archive);

std::string renderGroupsText(const std::vector<ReportGroup>& groups, int totalReports,
                             int skipped, const SymbolArchive& archive);
std::string renderGroupsJson(const std::vector<ReportGroup>& groups, int totalReports,
                             int skipped);

// ---------------------------------------------------------------------------
// Fetching
// ---------------------------------------------------------------------------
// One authenticated GET. `token` goes in an Authorization header and nowhere
// else - never in the query string, which is logged by every proxy between
// here and there. Empty `token` is refused rather than sent unauthenticated.
bool fetchReports(const std::string& url, const std::string& token, std::string& bodyOut,
                  std::string& err);

// https://foxsdr.com/api/crash/reports, overridable by FOXSDR_REPORTS_URL.
std::string reportsEndpoint();

}  // namespace cascade::core

#endif  // CASCADE_CORE_REPORT_READER_HPP
