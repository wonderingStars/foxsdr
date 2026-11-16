// companion_programs.cpp - see companion_programs.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/companion_programs.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "core/diag_log.hpp"
#include "core/package_identity.hpp"
#include "core/plugin_abi.h"
#include "core/plugin_repo.hpp"
#include "core/plugin_repo_helper.hpp"
#include "core/safe_file_io.hpp"
#include "core/sys_manifest_sig.hpp"
#include "core/sys_portable.hpp"

namespace cascade::core::companion {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::string lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }
    }
    return s;
}

bool wellFormedSha(const std::string& s) {
    if (s.size() != 64) { return false; }
    for (const char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) { return false; }
    }
    return true;
}

std::string pidText() {
#if defined(_WIN32)
    return std::to_string(_getpid());
#else
    return std::to_string(static_cast<int>(getpid()));
#endif
}

std::string str(const json& j, const char* key, std::size_t cap) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) { return std::string(); }
    std::string s = it->get<std::string>();
    if (s.size() > cap) { s.resize(cap); }
    return s;
}

bool isHttps(const std::string& u) { return u.size() > 8 && u.compare(0, 8, "https://") == 0 && u.find_first_of(" \t\r\n\"<>") == std::string::npos; }

// a relative path inside the package's companions folder: parts of letters, digits, '.', '_' and '-', '/' between them, no ".."
bool validPackagePath(const std::string& p) {
    if (p.empty() || p.size() > 200 || p.front() == '/' || p.back() == '/') { return false; }
    std::size_t start = 0;
    while (start <= p.size()) {
        std::size_t end = p.find('/', start);
        if (end == std::string::npos) { end = p.size(); }
        const std::string part = p.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") { return false; }
        for (const char c : part) {
            const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
            if (!ok) { return false; }
        }
        if (end >= p.size()) { break; }
        start = end + 1;
    }
    return true;
}

std::mutex g_mu;
int g_forcePackaged = -1;
fs::path g_forcedPackageDir;

std::atomic<std::uint64_t> g_hostFaults[4];

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// the catalogue's row

std::string identifierSentence(const std::string& description) {
    const std::string low = lower(description);
    const std::size_t at = low.find("cannot hide identifiers");
    if (at == std::string::npos) { return std::string(); }
    std::size_t begin = 0;
    for (std::size_t i = at; i > 0; --i) {
        if (description[i - 1] == '.' && i < description.size() && (description[i] == ' ' || description[i] == '\n')) { begin = i; break; }
        if (description[i - 1] == '\n') { begin = i; break; }
    }
    while (begin < description.size() && (description[begin] == ' ' || description[begin] == '\n')) { ++begin; }
    std::size_t end = at;
    while (end < description.size()) {
        if (description[end] == '.' && (end + 1 >= description.size() || description[end + 1] == ' ' || description[end + 1] == '\n')) { ++end; break; }
        if (description[end] == '\n') { break; }
        ++end;
    }
    std::string out = description.substr(begin, end - begin);
    if (out.size() > 600) { out.resize(600); }
    return out;
}

std::string validProgramName(const std::string& name) {
    if (name.empty()) { return "the name is empty"; }
    if (name.size() > 64) { return "the name is longer than 64 characters"; }
    if (name == "." || name == ".." || name.find("..") != std::string::npos) { return "the name holds .."; }
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) { return "the name holds a character that is not a letter, digit, dot, underscore or hyphen"; }
    }
    const std::string l = lower(name);
    if (l.size() >= 4 && (l.compare(l.size() - 4, 4, ".dll") == 0 || l.compare(l.size() - 3, 3, ".so") == 0)) { return "the name is a library, not a program"; }
    if (std::string(PluginRepo::hostOs()) == "windows" && !(l.size() > 4 && l.compare(l.size() - 4, 4, ".exe") == 0)) { return "the name does not end in .exe"; }
    return std::string();
}

void parseCompanionFields(const json& entryJson, PluginCatalogEntry& e) {
    e.companion = CompanionInfo{};
    try {
        const auto it = entryJson.find("companion");
        if (it == entryJson.end() || !it->is_object()) { return; }
        const json& c = *it;
        CompanionInfo info;
        info.present = true;
        info.upstream = str(c, "upstream", 200);
        info.licence = str(c, "licence", 100);
        info.whySeparate = str(c, "whySeparate", 4000);
        const std::string src = str(c, "source", 300);
        info.source = isHttps(src) ? src : std::string();
        info.identifierLine = identifierSentence(e.description);
        const auto pit = c.find("program");
        if (pit != c.end() && pit->is_object()) {
            ProgramInfo& p = info.program;
            p.file = str(*pit, "file", 80);
            p.url = str(*pit, "url", 600);
            p.sha256 = lower(str(*pit, "sha256", 80));
            p.status = str(*pit, "status", 40);
            p.signature = str(*pit, "signature", kMaxSignatureBytes);
            // USABLE: published, a safe name, an https address and a real digest. Anything else is "not available" and nothing is ever fetched for it.
            p.usable = p.status == "published" && validProgramName(p.file).empty() && isHttps(p.url) && wellFormedSha(p.sha256);
        }
        e.companion = std::move(info);
    } catch (...) {
        e.companion = CompanionInfo{};
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// the check

CheckResult checkProgram(const fs::path& file, const std::string& expectedSha256) {
    CheckResult r;
    if (!wellFormedSha(expectedSha256)) {
        r.kind = Check::NoHash;
        return r;
    }
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        r.kind = Check::Missing;
        return r;
    }
    std::string error;
    if (!PluginRepo::sha256File(file.string(), r.actual, error)) {
        r.kind = Check::Unreadable;
        return r;
    }
    r.kind = PluginRepo::sha256Matches(expectedSha256, r.actual) ? Check::Ok : Check::Mismatch;
    return r;
}

std::string refusalSentence(Check c, const std::string& programFile) {
    const std::string n = "\"" + programFile + "\"";
    switch (c) {
        case Check::Ok: return std::string();
        case Check::NoHash: return n + " has no checksum to check it against, so it was not started.";
        case Check::Missing: return n + " is not where it was installed, so it was not started.";
        case Check::Unreadable: return n + " could not be read to check it, so it was not started.";
        case Check::Mismatch: return n + " is not the file that was installed: it has changed on disk, so it was not started.";
        case Check::NotAProgram: return n + " is not a program for this machine, so it was not started.";
    }
    return n + " could not be checked, so it was not started.";
}

std::string signedStatement(const std::string& id, const std::string& file, const std::string& sha256) {
    return "foxsdr-companion-program v1\n" + id + "\n" + file + "\n" + lower(sha256) + "\n";
}

Signature checkRowSignature(const std::string& id, const ProgramInfo& program, const std::vector<sys::Ed25519PublicKey>& trustedKeys) {
    if (trustedKeys.empty()) { return Signature::NotEnforced; }
    if (program.signature.empty()) { return Signature::Missing; }
    std::string payload;
    if (sys::verifyEnvelope(program.signature, trustedKeys, payload) != sys::SigResult::Ok) { return Signature::Refused; }
    if (payload != signedStatement(id, program.file, program.sha256)) { return Signature::Refused; }
    return Signature::Verified;
}

namespace {
std::vector<sys::Ed25519PublicKey> g_testKeys;
bool g_useTestKeys = false;
}  // namespace

void setSignatureKeysForTest(bool use, const std::vector<sys::Ed25519PublicKey>& keys) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_useTestKeys = use;
    g_testKeys = keys;
}

static std::vector<sys::Ed25519PublicKey> trustedKeysNow() {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_useTestKeys) { return g_testKeys; }
    }
    return sys::ownerTrustedKeys();
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// the places

fs::path programFolder(const fs::path& pluginsDir) { return pluginsDir / "helpers"; }

void setPackagedForTest(int state, const fs::path& packageDir) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_forcePackaged = state;
    g_forcedPackageDir = packageDir;
}

Places placesFor(const std::string& pluginsDir) {
    Places p;
    p.pluginsDir = pluginsDir;
    int forced;
    fs::path forcedDir;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        forced = g_forcePackaged;
        forcedDir = g_forcedPackageDir;
    }
    if (forced >= 0) {
        p.packaged = forced != 0;
        p.packageDir = forcedDir;
        return p;
    }
    p.packaged = cascade::core::runningInPackage();
    if (p.packaged) {
        p.packageDir = sys::currentExeDir() / "companions";
        // THE TEST SEAM of a fake package (FOXSDR_FAKE_PACKAGE, core/package_identity.cpp): its companions folder is named by FOXSDR_FAKE_PACKAGE_COMPANIONS. Honoured ONLY
        // while a fake package is in force, so a real package can never be pointed at another folder by the environment.
        const char* fake = std::getenv("FOXSDR_FAKE_PACKAGE");
        const char* dir = std::getenv("FOXSDR_FAKE_PACKAGE_COMPANIONS");
        if (fake != nullptr && *fake != '\0' && dir != nullptr && *dir != '\0') { p.packageDir = dir; }
    }
    return p;
}

bool readPackageList(const fs::path& packageDir, std::vector<PackageProgram>& out, std::string& error) {
    out.clear();
    error.clear();
    std::string text;
    const auto rr = safeio::readCapped(packageDir / "companions.json", 1024u * 1024u, text);
    if (rr != safeio::ReadResult::Ok) {
        error = "this copy of FoxSDR has no list of companion programs";
        return false;
    }
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("programs") || !j["programs"].is_array()) {
        error = "the list of companion programs in this copy of FoxSDR is not readable";
        return false;
    }
    for (const json& p : j["programs"]) {
        if (!p.is_object()) { continue; }
        PackageProgram r;
        r.id = str(p, "id", 80);
        r.file = str(p, "file", 80);
        r.path = str(p, "path", 200);
        r.sha256 = lower(str(p, "sha256", 80));
        const auto sz = p.find("size");
        if (sz != p.end() && sz->is_number_unsigned()) { r.size = sz->get<std::uint64_t>(); }
        // a line the host cannot use whole is left out: a program is "in the package" only when its name, path and digest are all sound
        if (r.id.empty() || !validProgramName(r.file).empty() || !validPackagePath(r.path) || !wellFormedSha(r.sha256)) { continue; }
        out.push_back(std::move(r));
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// the record

namespace {

fs::path recordPath(const std::string& pluginsDir) { return fs::path(pluginsDir) / "companions.json"; }

std::vector<Record> loadRecords(const std::string& pluginsDir) {
    std::vector<Record> out;
    std::string text;
    if (safeio::readCapped(recordPath(pluginsDir), 1024u * 1024u, text) != safeio::ReadResult::Ok) { return out; }
    const json j = json::parse(text, nullptr, false);
    if (!j.is_object() || !j.contains("programs") || !j["programs"].is_array()) { return out; }
    for (const json& p : j["programs"]) {
        if (!p.is_object()) { continue; }
        Record r;
        r.plugin = str(p, "plugin", 200);
        r.id = str(p, "id", 80);
        r.file = str(p, "file", 80);
        r.sha256 = lower(str(p, "sha256", 80));
        r.via = str(p, "via", 20);
        const auto sz = p.find("size");
        if (sz != p.end() && sz->is_number_unsigned()) { r.size = sz->get<std::uint64_t>(); }
        if (r.plugin.empty() || !validProgramName(r.file).empty()) { continue; }
        out.push_back(std::move(r));
    }
    return out;
}

bool saveRecords(const std::string& pluginsDir, const std::vector<Record>& rs, std::string& error) {
    std::error_code ec;
    if (rs.empty()) {
        fs::remove(recordPath(pluginsDir), ec);
        return true;
    }
    json arr = json::array();
    for (const Record& r : rs) {
        arr.push_back({{"plugin", r.plugin}, {"id", r.id}, {"file", r.file}, {"sha256", r.sha256}, {"size", r.size}, {"via", r.via}});
    }
    const json j = {{"version", 1}, {"programs", arr}};
    if (!safeio::writeAtomic(recordPath(pluginsDir), j.dump(1, ' ', false, json::error_handler_t::replace) + "\n")) {
        error = "the list of companion programs could not be saved";
        return false;
    }
    return true;
}

}  // namespace

bool recordProgram(const std::string& pluginsDir, const Record& r, std::string& error) {
    error.clear();
    std::vector<Record> rs = loadRecords(pluginsDir);
    bool replaced = false;
    for (Record& x : rs) {
        if (x.plugin == r.plugin) {
            x = r;
            replaced = true;
        }
    }
    if (!replaced) { rs.push_back(r); }
    return saveRecords(pluginsDir, rs, error);
}

bool readRecord(const std::string& pluginsDir, const std::string& pluginFile, Record& out) {
    for (const Record& r : loadRecords(pluginsDir)) {
        if (r.plugin == pluginFile) {
            out = r;
            return true;
        }
    }
    return false;
}

bool forgetRecord(const std::string& pluginsDir, const std::string& pluginFile) {
    std::vector<Record> rs = loadRecords(pluginsDir);
    rs.erase(std::remove_if(rs.begin(), rs.end(), [&](const Record& r) { return r.plugin == pluginFile; }), rs.end());
    std::string error;
    return saveRecords(pluginsDir, rs, error);
}

bool removeProgram(const std::string& pluginsDir, const std::string& pluginFile, std::string& error) {
    error.clear();
    std::vector<Record> rs = loadRecords(pluginsDir);
    Record mine;
    bool have = false;
    std::vector<Record> rest;
    for (const Record& r : rs) {
        if (r.plugin == pluginFile) {
            mine = r;
            have = true;
        } else {
            rest.push_back(r);
        }
    }
    if (!have) { return true; }
    bool shared = false;
    for (const Record& r : rest) {
        if (r.file == mine.file) { shared = true; }
    }
    std::error_code ec;
    if (!shared) {
        const fs::path p = programFolder(fs::path(pluginsDir)) / mine.file;
        fs::remove(p, ec);
        if (fs::exists(p, ec)) {
            error = "\"" + mine.file + "\" could not be deleted (it is probably in use)";
            return false;
        }
        // the helpers folder goes too when nothing else is in it
        std::error_code ec2;
        if (fs::is_empty(programFolder(fs::path(pluginsDir)), ec2) && !ec2) { fs::remove(programFolder(fs::path(pluginsDir)), ec2); }
    }
    return saveRecords(pluginsDir, rest, error);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// what the shelf says

State stateOf(const PluginCatalogEntry& e, const Places& places, const std::vector<PackageProgram>& packageList, const std::string& pluginFile) {
    if (!e.companion.present) { return State::NotAvailable; }
    Record rec;
    if (!pluginFile.empty() && readRecord(places.pluginsDir.string(), pluginFile, rec)) {
        std::error_code ec;
        const fs::path p = programFolder(places.pluginsDir) / rec.file;
        if (fs::is_regular_file(p, ec) && fs::file_size(p, ec) == rec.size) { return State::Installed; }
        return State::Changed;
    }
    if (places.packaged) {
        for (const PackageProgram& pp : packageList) {
            if (pp.id != e.id || pp.file != e.companion.program.file) { continue; }
            std::error_code ec;
            if (fs::is_regular_file(places.packageDir / pp.path, ec)) { return State::InPackage; }
        }
        return State::NotAvailable;
    }
    return e.companion.program.usable ? State::Download : State::NotAvailable;
}

std::string bridgeFileOf(const PluginCatalogEntry& e) {
    const PluginPlatform* p = e.thisPlatform();
    if (p == nullptr) { return std::string(); }
    std::string want, err;
    return PluginRepo::sanitiseFileName(p->file, want, err) ? want : std::string();
}

State stateForEntry(const PluginCatalogEntry& e, const std::string& pluginsDir) {
    if (!e.companion.present || !e.source.empty()) { return State::NotAvailable; }
    const Places places = placesFor(pluginsDir);
    std::vector<PackageProgram> list;
    if (places.packaged) {
        std::string ignored;
        (void)readPackageList(places.packageDir, list, ignored);   // no list: nothing is "in the package", the state says not available
    }
    return stateOf(e, places, list, bridgeFileOf(e));
}

const char* stateWord(State s) {
    switch (s) {
        case State::NotAvailable: return "unavailable";
        case State::InPackage: return "package";
        case State::Download: return "download";
        case State::Installed: return "installed";
        case State::Changed: return "changed";
    }
    return "unavailable";
}

std::string stateSentence(State s) {
    switch (s) {
        case State::NotAvailable: return "The program for this companion is not available in this copy of FoxSDR.";
        case State::InPackage: return "The program is included in this copy of FoxSDR. Fitting the plugin puts it in place; nothing is downloaded.";
        case State::Download: return "Fitting the plugin downloads the program and checks it against the checksum the catalogue gives before it can run.";
        case State::Installed: return "The program is installed and is checked again every time before it starts.";
        case State::Changed: return "The installed program is missing or is not the file that was installed. It will not start; remove the plugin and fit it again.";
    }
    return std::string();
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// the install transaction

Txn::Txn(fs::path pluginsDir) : dir_(std::move(pluginsDir)) {}

namespace {
// A failed install leaves nothing behind: the helpers folder goes too when this transaction made it and nothing is in it.
void dropEmptyFolder(const fs::path& folder) {
    std::error_code ec;
    if (fs::is_directory(folder, ec) && fs::is_empty(folder, ec) && !ec) { fs::remove(folder, ec); }
}
}  // namespace

Txn::~Txn() {
    if (!committed_) {
        std::error_code ec;
        if (!stageDir_.empty()) { fs::remove_all(stageDir_, ec); }
        if (!dir_.empty()) { dropEmptyFolder(programFolder(dir_)); }
    }
}

bool Txn::stage(const PluginCatalogEntry& e, std::atomic<float>* progress, std::atomic<bool>* cancel, std::string& error) {
    error.clear();
    if (!e.companion.present || !e.source.empty()) { return true; }
    const std::string q = "\"" + e.name + "\": ";
    const Places places = placesFor(dir_.string());
    file_ = e.companion.program.file;
    if (const std::string why = validProgramName(file_); !why.empty()) {
        error = q + "the program has no usable file name in the catalogue (" + why + ")";
        return false;
    }
    std::error_code ec;
    stageDir_ = programFolder(dir_) / (".stage-" + pidText());
    fs::remove_all(stageDir_, ec);
    fs::create_directories(stageDir_, ec);
    if (!fs::is_directory(stageDir_, ec)) {
        error = q + "the staging folder for the program could not be made";
        return false;
    }
    staged_ = stageDir_ / file_;
    target_ = programFolder(dir_) / file_;

    if (places.packaged) {
        // THE STORE COPY: the program is inside the package; no transport is touched on this path.
        std::vector<PackageProgram> list;
        std::string listError;
        if (!readPackageList(places.packageDir, list, listError)) {
            error = q + listError;
            return false;
        }
        const PackageProgram* mine = nullptr;
        for (const PackageProgram& p : list) {
            if (p.id == e.id && p.file == file_) { mine = &p; }
        }
        if (mine == nullptr) {
            error = q + "this copy of FoxSDR does not include the program for it";
            return false;
        }
        const fs::path src = places.packageDir / mine->path;
        const CheckResult before = checkProgram(src, mine->sha256);
        if (before.kind != Check::Ok) {
            error = q + refusalSentence(before.kind, file_);
            return false;
        }
        fs::copy_file(src, staged_, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            error = q + "the program could not be copied out of the package: " + ec.message();
            return false;
        }
        const CheckResult after = checkProgram(staged_, mine->sha256);
        if (after.kind != Check::Ok) {
            error = q + refusalSentence(after.kind, file_);
            return false;
        }
        sha_ = mine->sha256;
        via_ = "package";
    } else {
        const ProgramInfo& pr = e.companion.program;
        if (!pr.usable) {
            error = q + "the program for this companion is not available";
            return false;
        }
        const Signature sig = checkRowSignature(e.id, pr, trustedKeysNow());
        if (sig == Signature::Missing || sig == Signature::Refused) {
            error = q + "the catalogue's signature for the program is " + (sig == Signature::Missing ? "missing" : "not valid") + ", so it was not fetched";
            return false;
        }
        PluginRepo::FetchFailure failure = PluginRepo::FetchFailure::None;
        std::string fetchError;
        if (!PluginRepo::fetchVerifiedFile(pr.url, pr.sha256, staged_.string(), kMaxProgramBytes, fetchError, progress, cancel, &failure)) {
            error = failure == PluginRepo::FetchFailure::Verify ? "\"" + file_ + "\" failed its integrity check and was discarded" : "\"" + file_ + "\": " + fetchError;
            return false;
        }
        sha_ = lower(pr.sha256);
        via_ = "download";
    }
    std::string imageError;
    if (!helperstore::looksLikeProgram(staged_, lower(PluginRepo::hostOs()), lower(PluginRepo::hostArch()), imageError)) {
        error = "\"" + file_ + "\": " + imageError;
        return false;
    }
    size_ = fs::file_size(staged_, ec);
    active_ = true;
    return true;
}

bool Txn::commit(std::string& error) {
    error.clear();
    if (!active_) { return true; }
    std::error_code ec;
    if (fs::exists(target_, ec)) {
        backup_ = target_;
        backup_ += ".bak-" + pidText();
        fs::remove(backup_, ec);
        fs::rename(target_, backup_, ec);
        if (ec) {
            error = "\"" + file_ + "\" is in use and cannot be replaced";
            backup_.clear();
            return false;
        }
    }
    fs::rename(staged_, target_, ec);
    if (ec) {
        error = "\"" + file_ + "\" could not be moved into place: " + ec.message();
        rollback();
        return false;
    }
    movedIn_ = true;
#if !defined(_WIN32)
    ::chmod(target_.string().c_str(), 0755);
#endif
    committed_ = true;
    return true;
}

void Txn::rollback() {
    std::error_code ec;
    if (movedIn_) {
        fs::remove(target_, ec);
        movedIn_ = false;
    }
    if (!backup_.empty() && fs::exists(backup_, ec)) {
        fs::rename(backup_, target_, ec);
        backup_.clear();
    }
    committed_ = false;
    if (!stageDir_.empty()) { fs::remove_all(stageDir_, ec); }
    dropEmptyFolder(programFolder(dir_));
}

void Txn::finish() {
    std::error_code ec;
    if (!backup_.empty()) { fs::remove(backup_, ec); }
    backup_.clear();
    if (!stageDir_.empty()) { fs::remove_all(stageDir_, ec); }
    committed_ = true;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// before every start

namespace {

// The Store copy's helpers copy must BE the package's program. Missing or different: copied again from the package (never downloaded). Returns "" when it is in place and
// matches `expected`, else the plain sentence.
std::string provisionFromPackage(const Places& places, const PackageProgram& pp, const fs::path& program) {
    const CheckResult here = checkProgram(program, pp.sha256);
    if (here.kind == Check::Ok) { return std::string(); }
    const fs::path src = places.packageDir / pp.path;
    const CheckResult inPackage = checkProgram(src, pp.sha256);
    if (inPackage.kind != Check::Ok) { return refusalSentence(inPackage.kind, pp.file); }
    std::error_code ec;
    fs::create_directories(program.parent_path(), ec);
    fs::path tmp = program;
    tmp += ".new-" + pidText();
    fs::copy_file(src, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) { return "\"" + pp.file + "\" could not be copied out of the package, so it was not started."; }
    const CheckResult copied = checkProgram(tmp, pp.sha256);
    if (copied.kind != Check::Ok) {
        fs::remove(tmp, ec);
        return refusalSentence(copied.kind, pp.file);
    }
    fs::remove(program, ec);
    fs::rename(tmp, program, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return "\"" + pp.file + "\" could not be put in place (it is probably in use), so it was not started.";
    }
    return std::string();
}

}  // namespace

StartVerdict checkBeforeStart(const std::string& pluginPath) {
    StartVerdict v;
    try {
        const fs::path p(pluginPath);
        const std::string dir = p.parent_path().string();
        const std::string file = p.filename().string();
        Record rec;
        if (!readRecord(dir, file, rec)) { return v; }   // not a catalogue companion: a program the user chose is the user's
        const Places places = placesFor(dir);
        const fs::path program = programFolder(fs::path(dir)) / rec.file;
        std::string expected = rec.sha256;
        if (places.packaged && rec.via == "package") {
            std::vector<PackageProgram> list;
            std::string listError;
            const PackageProgram* mine = nullptr;
            if (readPackageList(places.packageDir, list, listError)) {
                for (const PackageProgram& pp : list) {
                    if (pp.id == rec.id && pp.file == rec.file) { mine = &pp; }
                }
            }
            if (mine == nullptr) {
                v.allowed = false;
                v.sentence = "\"" + rec.file + "\" is not in this copy of FoxSDR any more, so it was not started.";
            } else {
                const std::string why = provisionFromPackage(places, *mine, program);
                if (!why.empty()) {
                    v.allowed = false;
                    v.sentence = why;
                } else {
                    expected = mine->sha256;
                    if (expected != rec.sha256) {   // the package was updated: the record follows it
                        Record fresh = rec;
                        fresh.sha256 = expected;
                        std::error_code ec;
                        fresh.size = fs::file_size(program, ec);
                        std::string recErr;
                        (void)recordProgram(dir, fresh, recErr);
                    }
                }
            }
        }
        if (v.allowed) {
            const CheckResult r = checkProgram(program, expected);
            if (r.kind != Check::Ok) {
                v.allowed = false;
                v.sentence = refusalSentence(r.kind, rec.file);
            }
        }
        if (!v.allowed) {
            noteHostFault(CASCADE_FAULT_BREAK);
            diagLogf("plugins: a companion program was refused before it started (%s)", rec.id.c_str());
        }
    } catch (...) {
        v.allowed = false;
        v.sentence = "The companion program could not be checked, so it was not started.";
        noteHostFault(CASCADE_FAULT_BREAK);
    }
    return v;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// the fault path

void noteHostFault(std::uint32_t kind) {
    if (kind < CASCADE_FAULT_CRASH || kind > CASCADE_FAULT_GAVE_UP) { return; }
    g_hostFaults[kind - 1u].fetch_add(1, std::memory_order_relaxed);
}

void takeHostFaults(std::array<std::uint64_t, 4>& sums) {
    for (std::size_t i = 0; i < 4; ++i) { sums[i] += g_hostFaults[i].exchange(0, std::memory_order_relaxed); }
}

}  // namespace cascade::core::companion
