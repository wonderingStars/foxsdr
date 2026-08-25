// See report_reader.hpp for why symbolisation happens here and not on the
// server, and what this tool refuses to pretend.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/report_reader.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#if defined(_WIN32)
#include <windows.h>

#include <dbghelp.h>
#include <winhttp.h>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winhttp.lib")
#endif

namespace fs = std::filesystem;

namespace cascade::core {

namespace {

std::string jstr(const nlohmann::json& j, const char* key) {
    if (!j.contains(key) || !j[key].is_string()) { return std::string(); }
    return j[key].get<std::string>();
}

std::uint64_t jnum(const nlohmann::json& j, const char* key) {
    if (!j.contains(key)) { return 0; }
    if (j[key].is_number_unsigned()) { return j[key].get<std::uint64_t>(); }
    if (j[key].is_number_integer()) {
        const std::int64_t v = j[key].get<std::int64_t>();
        return (v < 0) ? 0 : static_cast<std::uint64_t>(v);
    }
    if (j[key].is_number_float()) {
        const double d = j[key].get<double>();
        return (d < 0.0) ? 0 : static_cast<std::uint64_t>(d);
    }
    // Accepted as a courtesy to an export that stringified its numbers; the
    // contract says a JSON number, and this is the one place a disagreement
    // between two independently written parsers would silently zero every
    // offset in an archive.
    if (j[key].is_string()) {
        const std::string s = j[key].get<std::string>();
        const int b = (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0) ? 16 : 10;
        return static_cast<std::uint64_t>(std::strtoull(s.c_str(), nullptr, b));
    }
    return 0;
}

std::string moduleStem(const std::string& fileName) {
    const std::size_t dot = fileName.find_last_of('.');
    return (dot == std::string::npos) ? fileName : fileName.substr(0, dot);
}

std::vector<std::string> sortedUnique(const std::set<std::string>& s) {
    std::vector<std::string> out(s.begin(), s.end());
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// The feed
// ---------------------------------------------------------------------------
std::vector<ReaderReport> parseReportFeed(const std::string& jsonText, std::string& err,
                                          int* skipped) {
    err.clear();
    if (skipped != nullptr) { *skipped = 0; }
    std::vector<ReaderReport> out;

    const nlohmann::json root = nlohmann::json::parse(jsonText, nullptr, false);
    if (root.is_discarded()) {
        err = "the feed is not JSON";
        return out;
    }
    // {"reports":[...]} is what the endpoint answers; a bare array is what an
    // export saved by hand usually is, and a year-old export has to stay
    // readable without a service in the loop.
    const nlohmann::json* arr = nullptr;
    if (root.is_object() && root.contains("reports") && root["reports"].is_array()) {
        arr = &root["reports"];
    } else if (root.is_array()) {
        arr = &root;
    } else {
        err = "the feed carries no reports array";
        return out;
    }

    for (const nlohmann::json& e : *arr) {
        // A report with no signature cannot be grouped, and a group of one
        // ungroupable thing is a row nobody can act on. Skipped and counted -
        // one bad entry must not cost the rest of the export.
        if (!e.is_object() || jstr(e, "signature").empty()) {
            if (skipped != nullptr) { ++(*skipped); }
            continue;
        }
        ReaderReport r;
        r.schema = e.contains("schema") && e["schema"].is_number_integer()
                       ? e["schema"].get<int>()
                       : 0;
        r.kind = jstr(e, "kind");
        r.version = jstr(e, "version");
        r.commit = jstr(e, "commit");
        r.buildId = jstr(e, "buildId");
        r.module = jstr(e, "module");
        r.offset = jnum(e, "offset");
        r.signature = jstr(e, "signature");
        r.os = jstr(e, "os");
        r.arch = jstr(e, "arch");
        r.installId = jstr(e, "installId");
        r.receivedAt = jstr(e, "receivedAt");

        if (e.contains("context") && e["context"].is_object()) {
            const nlohmann::json& c = e["context"];
            r.mode = jstr(c, "mode");
            r.source = jstr(c, "source");
            r.sdrModel = jstr(c, "sdrModel");
            if (c.contains("sampleRate") && c["sampleRate"].is_number()) {
                r.sampleRateHz = c["sampleRate"].get<double>();
            }
            if (c.contains("deviceOpen") && c["deviceOpen"].is_boolean()) {
                r.deviceOpen = c["deviceOpen"].get<bool>();
            }
        }
        if (e.contains("plugins") && e["plugins"].is_array()) {
            for (const nlohmann::json& p : e["plugins"]) {
                if (!p.is_object()) { continue; }
                ReaderPlugin rp;
                rp.name = jstr(p, "name");
                rp.version = jstr(p, "version");
                rp.buildId = jstr(p, "buildId");
                r.plugins.push_back(rp);
            }
        }
        if (e.contains("log") && e["log"].is_array()) {
            for (const nlohmann::json& l : e["log"]) {
                if (l.is_string()) { r.log.push_back(l.get<std::string>()); }
            }
        }
        if (e.contains("threads") && e["threads"].is_array()) {
            for (const nlohmann::json& t : e["threads"]) {
                if (!t.is_object()) { continue; }
                ReaderThread rt;
                rt.id = jnum(t, "id");
                if (t.contains("frames") && t["frames"].is_array()) {
                    for (const nlohmann::json& f : t["frames"]) {
                        if (!f.is_object()) { continue; }
                        ReaderFrame rf;
                        rf.module = jstr(f, "module");
                        rf.buildId = jstr(f, "buildId");
                        rf.offset = jnum(f, "offset");
                        rt.frames.push_back(rf);
                    }
                }
                r.threads.push_back(rt);
            }
        }
        out.push_back(std::move(r));
    }
    return out;
}

// ---------------------------------------------------------------------------
// The archive
// ---------------------------------------------------------------------------
namespace {

std::atomic<unsigned> g_symSeq{0};

}  // namespace

SymbolArchive::SymbolArchive(std::string root) : root_(std::move(root)) {
#if defined(_WIN32)
    // dbghelp keys its state by a "process handle" which, for a symbol-only
    // session (fInvadeProcess FALSE), is nothing but a key. A unique fake one
    // keeps this from colliding with anything else in the process that uses
    // dbghelp - including a future second SymbolArchive.
    symHandle_ = reinterpret_cast<void*>(
        static_cast<std::uintptr_t>(0xF05D0000u + (g_symSeq.fetch_add(1) & 0xFFFFu)));
    // UNDNAME so C++ names read as C++; LOAD_LINES because a function without
    // a line number is half an answer and the option is the only thing that
    // decides whether the line table is read at all.
    ::SymSetOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS |
                    SYMOPT_NO_PROMPTS);
    symReady_ = ::SymInitialize(static_cast<HANDLE>(symHandle_), nullptr, FALSE) != 0;
#endif
}

SymbolArchive::~SymbolArchive() {
#if defined(_WIN32)
    if (symReady_) { ::SymCleanup(static_cast<HANDLE>(symHandle_)); }
#endif
}

bool SymbolArchive::exists() const {
    if (root_.empty()) { return false; }
    std::error_code ec;
    return fs::is_directory(fs::path(root_), ec);
}

std::string SymbolArchive::pdbPath(const std::string& moduleName,
                                   const std::string& buildId) const {
    if (buildId.empty() || !exists()) { return std::string(); }
    std::error_code ec;

    // The conventional name first: MSVC names the PDB after the binary, and
    // tools/archive-symbols.ps1 files it under the name the linker recorded.
    if (!moduleName.empty()) {
        const std::string pdbName = moduleStem(moduleName) + ".pdb";
        const fs::path direct = fs::path(root_) / pdbName / buildId / pdbName;
        if (fs::is_regular_file(direct, ec)) { return direct.string(); }
    }

    // ...then the whole archive, because a PDB with an unconventional name is
    // still the right PDB, and the build id directory is the actual key.
    for (const fs::directory_entry& e : fs::directory_iterator(fs::path(root_), ec)) {
        if (ec) { break; }
        if (!e.is_directory(ec)) { continue; }
        const fs::path byId = e.path() / buildId;
        if (!fs::is_directory(byId, ec)) { continue; }
        for (const fs::directory_entry& f : fs::directory_iterator(byId, ec)) {
            if (ec) { break; }
            if (f.is_regular_file(ec)) { return f.path().string(); }
        }
    }
    return std::string();
}

SymbolResult SymbolArchive::resolve(const std::string& moduleName, const std::string& buildId,
                                    std::uint64_t offset) const {
    SymbolResult out;
    const std::string mod = moduleName.empty() ? std::string("(unknown module)") : moduleName;

    if (buildId.empty()) {
        // Not the same sentence as "not archived": this module never carried a
        // CodeView record, so there is no id to go looking for and no PDB that
        // could ever exist. Saying "not archived" would send somebody to the
        // NAS for a file that was never made.
        out.note = mod +
                   " carries no build id, so nothing identifies its symbols - it was built "
                   "without a PDB, or it is a system module we do not archive";
        return out;
    }
    if (!exists()) {
        out.note = "no symbol archive at " + (root_.empty() ? std::string("(none given)") : root_) +
                   ", so build id " + buildId + " (" + mod + ") cannot be resolved";
        return out;
    }

#if defined(_WIN32)
    if (!symReady_) {
        out.note = "the symbol engine did not start; build id " + buildId + " (" + mod +
                   ") was not resolved";
        return out;
    }

    const auto cached = missing_.find(buildId);
    if (cached != missing_.end()) {
        out.note = cached->second;
        return out;
    }

    std::uint64_t base = 0;
    const auto it = loaded_.find(buildId);
    if (it != loaded_.end()) {
        base = it->second;
    } else {
        const std::string pdb = pdbPath(mod, buildId);
        if (pdb.empty()) {
            // THE SENTENCE THAT MATTERS. It names the id, so the answer to
            // "why is this unreadable" is something somebody can act on.
            out.note = "no symbols archived for build id " + buildId + " (" + mod +
                       ") - look in " + root_ + " or nas:/volume1/foxsdr-symbols";
            missing_[buildId] = out.note;
            return out;
        }
        // A synthetic base per module, spaced far enough apart that two
        // modules loaded in one session cannot overlap. The real base is a
        // property of a process that no longer exists and is not in the
        // payload; only the OFFSET is, which is the whole point of recording
        // module+offset in the first place.
        constexpr std::uint64_t kSpan = 0x08000000ull;
        base = 0x10000000ull + static_cast<std::uint64_t>(loaded_.size()) * kSpan;
        const DWORD64 got = ::SymLoadModuleEx(static_cast<HANDLE>(symHandle_), nullptr,
                                              pdb.c_str(), mod.c_str(), base,
                                              static_cast<DWORD>(kSpan), nullptr, 0);
        if (got == 0) {
            out.note = "the archived PDB for build id " + buildId + " (" + mod +
                       ") could not be loaded: " + pdb;
            missing_[buildId] = out.note;
            return out;
        }
        base = got;
        loaded_[buildId] = base;
    }

    char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(char)] = {};
    auto* si = reinterpret_cast<PSYMBOL_INFO>(buffer);
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = MAX_SYM_NAME;
    DWORD64 disp = 0;
    if (::SymFromAddr(static_cast<HANDLE>(symHandle_), base + offset, &disp, si) == 0) {
        out.note = "build id " + buildId + " (" + mod +
                   ") is archived, but the offset falls outside every function in it - the "
                   "report and the PDB may not describe the same binary";
        return out;
    }
    out.resolved = true;
    out.function = si->Name;

    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD lineDisp = 0;
    if (::SymGetLineFromAddr64(static_cast<HANDLE>(symHandle_), base + offset, &lineDisp,
                               &line) != 0) {
        out.file = (line.FileName != nullptr) ? line.FileName : "";
        out.line = static_cast<int>(line.LineNumber);
    }
    return out;
#else
    (void)offset;
    out.note = "symbolisation needs Windows and dbghelp; build id " + buildId + " (" + mod +
               ") was not resolved";
    return out;
#endif
}

// ---------------------------------------------------------------------------
// Grouping
// ---------------------------------------------------------------------------
std::vector<ReportGroup> groupReports(const std::vector<ReaderReport>& reports,
                                      const SymbolArchive& archive) {
    struct Acc {
        ReportGroup g;
        std::set<std::string> versions;
        std::set<std::string> oses;
        std::set<std::string> plugins;
        std::set<std::string> missing;
        const ReaderReport* rep = nullptr;
    };
    std::vector<Acc> accs;
    std::map<std::string, std::size_t> index;

    for (const ReaderReport& r : reports) {
        auto it = index.find(r.signature);
        if (it == index.end()) {
            Acc a;
            a.g.signature = r.signature;
            a.g.kind = r.kind;
            a.g.firstSeen = r.receivedAt;
            a.g.lastSeen = r.receivedAt;
            index[r.signature] = accs.size();
            accs.push_back(std::move(a));
            it = index.find(r.signature);
        }
        Acc& a = accs[it->second];
        ++a.g.count;
        if (!r.version.empty()) { a.versions.insert(r.version); }
        if (!r.os.empty()) { a.oses.insert(r.os); }
        for (const ReaderPlugin& p : r.plugins) {
            if (!p.name.empty()) { a.plugins.insert(p.name + " " + p.version); }
        }
        // ISO8601 in UTC sorts lexicographically, which is why the contract
        // asks for it rather than for a local timestamp.
        if (!r.receivedAt.empty()) {
            if (a.g.firstSeen.empty() || r.receivedAt < a.g.firstSeen) {
                a.g.firstSeen = r.receivedAt;
            }
            if (a.g.lastSeen.empty() || r.receivedAt > a.g.lastSeen) {
                a.g.lastSeen = r.receivedAt;
            }
        }
        // THE REPRESENTATIVE IS THE MOST RECENT ONE, because it is the report
        // whose build is most likely to still be in the archive.
        if (a.rep == nullptr || r.receivedAt >= a.rep->receivedAt) { a.rep = &r; }
    }

    std::vector<ReportGroup> out;
    out.reserve(accs.size());
    for (Acc& a : accs) {
        a.g.versions = sortedUnique(a.versions);
        a.g.osVersions = sortedUnique(a.oses);
        a.g.plugins = sortedUnique(a.plugins);
        if (a.rep != nullptr) {
            const ReaderReport& r = *a.rep;
            a.g.module = r.module;
            a.g.buildId = r.buildId;
            a.g.offset = r.offset;
            a.g.commit = r.commit;
            a.g.log = r.log;
            a.g.symbol = archive.resolve(r.module, r.buildId, r.offset);
            if (!a.g.symbol.resolved && !r.buildId.empty()) { a.missing.insert(r.buildId); }
            // The FAULTING thread's stack. For a crash that is the only one
            // captured; for a freeze it is the stalled one, which is the thread
            // the signature was built from.
            if (!r.threads.empty()) {
                for (const ReaderFrame& f : r.threads.front().frames) {
                    SymbolisedFrame sf;
                    sf.frame = f;
                    sf.symbol = archive.resolve(f.module, f.buildId, f.offset);
                    if (!sf.symbol.resolved && !f.buildId.empty()) {
                        a.missing.insert(f.buildId);
                    }
                    a.g.frames.push_back(std::move(sf));
                }
            }
        }
        a.g.missingBuildIds = sortedUnique(a.missing);
        out.push_back(std::move(a.g));
    }

    // Newest first, then biggest: what is happening now, then what used to.
    std::sort(out.begin(), out.end(), [](const ReportGroup& x, const ReportGroup& y) {
        if (x.lastSeen != y.lastSeen) { return x.lastSeen > y.lastSeen; }
        if (x.count != y.count) { return x.count > y.count; }
        return x.signature < y.signature;
    });
    return out;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
namespace {

std::string joinList(const std::vector<std::string>& v) {
    std::string out;
    for (const std::string& s : v) {
        if (!out.empty()) { out += ", "; }
        out += s;
    }
    return out.empty() ? std::string("(none)") : out;
}

// A frame, for a human.
//
// NOTE WHAT IS NOT HERE: the raw offset. An offset without the matching PDB is
// not information, it is a trap - the same number in a different build is a
// different function, and printing it invites somebody to look it up in
// whatever build they have to hand and believe the answer. Every offset is in
// the JSON output, where a machine can use it and nobody can misread it.
std::string frameLine(const SymbolisedFrame& f) {
    std::string out = "    ";
    out += f.frame.module.empty() ? std::string("(unknown module)") : f.frame.module;
    if (f.symbol.resolved) {
        out += "!";
        out += f.symbol.function;
        if (!f.symbol.file.empty()) {
            out += "  ";
            out += f.symbol.file;
            if (f.symbol.line > 0) { out += ":" + std::to_string(f.symbol.line); }
        }
    } else {
        out += "  -- ";
        out += f.symbol.note;
    }
    out += "\n";
    return out;
}

}  // namespace

std::string renderGroupsText(const std::vector<ReportGroup>& groups, int totalReports,
                             int skipped, const SymbolArchive& archive) {
    std::string out;
    out += "FoxSDR fault reports\n";
    out += "archive: " + (archive.root().empty() ? std::string("(none)") : archive.root());
    out += archive.exists() ? "\n" : "   *** NOT FOUND - nothing can be symbolised ***\n";
    out += "reports: " + std::to_string(totalReports) + " in " +
           std::to_string(groups.size()) + " group(s)";
    if (skipped > 0) { out += ", " + std::to_string(skipped) + " unreadable and skipped"; }
    out += "\n";

    int n = 0;
    for (const ReportGroup& g : groups) {
        ++n;
        out += "\n";
        out += "[" + std::to_string(n) + "] " + g.signature + "   " +
               (g.kind.empty() ? std::string("?") : g.kind) + " x" +
               std::to_string(g.count) + "\n";
        out += "    first seen: " + (g.firstSeen.empty() ? std::string("(unknown)") : g.firstSeen) +
               "    last seen: " +
               (g.lastSeen.empty() ? std::string("(unknown)") : g.lastSeen) + "\n";
        out += "    versions:   " + joinList(g.versions) + "\n";
        out += "    systems:    " + joinList(g.osVersions) + "\n";
        out += "    plugins:    " + joinList(g.plugins) + "\n";
        if (!g.commit.empty()) { out += "    commit:     " + g.commit + "\n"; }
        out += "    fault:      ";
        out += g.module.empty() ? std::string("(unknown module)") : g.module;
        if (g.symbol.resolved) {
            out += "!" + g.symbol.function;
            if (!g.symbol.file.empty()) {
                out += "  " + g.symbol.file;
                if (g.symbol.line > 0) { out += ":" + std::to_string(g.symbol.line); }
            }
            out += "\n";
        } else {
            out += "\n                NOT SYMBOLISED: " + g.symbol.note + "\n";
        }
        if (!g.frames.empty()) {
            out += "    stack:\n";
            for (const SymbolisedFrame& f : g.frames) { out += frameLine(f); }
        }
        if (!g.missingBuildIds.empty()) {
            out += "    missing symbols for build id(s): " + joinList(g.missingBuildIds) + "\n";
        }
        if (!g.log.empty()) {
            out += "    log (last " + std::to_string(g.log.size()) + " lines):\n";
            for (const std::string& l : g.log) { out += "      " + l + "\n"; }
        }
    }
    if (groups.empty()) { out += "\nNo reports.\n"; }
    return out;
}

std::string renderGroupsJson(const std::vector<ReportGroup>& groups, int totalReports,
                             int skipped) {
    nlohmann::json j;
    j["totalReports"] = totalReports;
    j["skipped"] = skipped;
    nlohmann::json arr = nlohmann::json::array();
    for (const ReportGroup& g : groups) {
        nlohmann::json e;
        e["signature"] = g.signature;
        e["kind"] = g.kind;
        e["count"] = g.count;
        e["firstSeen"] = g.firstSeen;
        e["lastSeen"] = g.lastSeen;
        e["versions"] = g.versions;
        e["osVersions"] = g.osVersions;
        e["plugins"] = g.plugins;
        e["commit"] = g.commit;
        e["module"] = g.module;
        e["buildId"] = g.buildId;
        e["offset"] = g.offset;
        e["symbolised"] = g.symbol.resolved;
        e["function"] = g.symbol.function;
        e["file"] = g.symbol.file;
        e["line"] = g.symbol.line;
        e["note"] = g.symbol.note;
        e["missingBuildIds"] = g.missingBuildIds;
        nlohmann::json frames = nlohmann::json::array();
        for (const SymbolisedFrame& f : g.frames) {
            nlohmann::json fe;
            fe["module"] = f.frame.module;
            fe["buildId"] = f.frame.buildId;
            fe["offset"] = f.frame.offset;
            fe["resolved"] = f.symbol.resolved;
            fe["function"] = f.symbol.function;
            fe["file"] = f.symbol.file;
            fe["line"] = f.symbol.line;
            fe["note"] = f.symbol.note;
            frames.push_back(std::move(fe));
        }
        e["frames"] = std::move(frames);
        e["log"] = g.log;
        // installId is DELIBERATELY absent, here and in the text rendering. It
        // exists so the receiving end can rate-limit one machine; a reader that
        // emitted it would turn an anonymous counter into a way to follow one
        // person's crashes around.
        arr.push_back(std::move(e));
    }
    j["groups"] = std::move(arr);
    return j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
}

// ---------------------------------------------------------------------------
// Fetching
// ---------------------------------------------------------------------------
std::string reportsEndpoint() {
#if defined(_WIN32)
    char buf[512] = {0};
    const DWORD n = ::GetEnvironmentVariableA("FOXSDR_REPORTS_URL", buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) { return std::string(buf, n); }
#endif
    return std::string("https://foxsdr.com/api/crash/reports");
}

bool fetchReports(const std::string& url, const std::string& token, std::string& bodyOut,
                  std::string& err) {
    bodyOut.clear();
    err.clear();
    if (url.empty()) {
        err = "no reports URL";
        return false;
    }
    if (token.empty()) {
        // REFUSED, not attempted. An unauthenticated GET to this endpoint would
        // either fail or - worse, if the endpoint were ever misconfigured -
        // succeed, and nobody would find out until it was public.
        err = "no token: set FOXSDR_REPORTS_TOKEN (it is never compiled in)";
        return false;
    }
#if defined(_WIN32)
    URL_COMPONENTSW uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {0};
    wchar_t path[1024] = {0};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 1023;
    const std::wstring wurl(url.begin(), url.end());
    if (::WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc) == 0) {
        err = "the reports URL could not be parsed";
        return false;
    }
    const bool secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    const bool loopback =
        (std::wcscmp(host, L"127.0.0.1") == 0 || std::wcscmp(host, L"localhost") == 0);
    if (!secure && !loopback) {
        // The token is a real secret; putting it on the wire in clear is not a
        // thing this tool will do off the loopback.
        err = "refusing to send the token over plain http";
        return false;
    }

    HINTERNET ses = ::WinHttpOpen(L"FoxSDR-reports/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (ses == nullptr) {
        err = "could not open an HTTP session";
        return false;
    }
    ::WinHttpSetTimeouts(ses, 10000, 10000, 30000, 30000);
    bool ok = false;
    HINTERNET con = ::WinHttpConnect(ses, host, uc.nPort, 0);
    if (con != nullptr) {
        HINTERNET req =
            ::WinHttpOpenRequest(con, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
        if (req != nullptr) {
            // The token goes in a HEADER and nowhere else. A query string is
            // logged by every proxy between here and there, and by the server.
            std::wstring auth = L"Authorization: Bearer ";
            auth.append(token.begin(), token.end());
            auth += L"\r\n";
            ::WinHttpAddRequestHeaders(req, auth.c_str(), static_cast<DWORD>(-1),
                                       WINHTTP_ADDREQ_FLAG_ADD);
            if (::WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                     WINHTTP_NO_REQUEST_DATA, 0, 0, 0) != 0 &&
                ::WinHttpReceiveResponse(req, nullptr) != 0) {
                DWORD status = 0;
                DWORD len = sizeof(status);
                ::WinHttpQueryHeaders(req,
                                      WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                      WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                                      WINHTTP_NO_HEADER_INDEX);
                if (status >= 200 && status < 300) {
                    DWORD avail = 0;
                    while (::WinHttpQueryDataAvailable(req, &avail) != 0 && avail > 0) {
                        std::string chunk;
                        chunk.resize(avail);
                        DWORD read = 0;
                        if (::WinHttpReadData(req, &chunk[0], avail, &read) == 0) { break; }
                        chunk.resize(read);
                        bodyOut += chunk;
                        if (bodyOut.size() > 64u * 1024u * 1024u) { break; }
                    }
                    ok = true;
                } else {
                    err = "the reports endpoint answered " + std::to_string(status);
                }
            } else {
                err = "the reports endpoint could not be reached";
            }
            ::WinHttpCloseHandle(req);
        }
        ::WinHttpCloseHandle(con);
    } else {
        err = "could not connect to the reports endpoint";
    }
    ::WinHttpCloseHandle(ses);
    return ok;
#else
    err = "fetching needs Windows";
    return false;
#endif
}

}  // namespace cascade::core
