// plugin_repo_helper.cpp - see plugin_repo_helper.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/plugin_repo_helper.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <system_error>

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#else
#include <process.h>
#endif

#include <nlohmann/json.hpp>

#include "core/plugin_repo.hpp"
#include "core/safe_file_io.hpp"

namespace cascade::core::helperstore {

namespace fs = std::filesystem;
using nlohmann::json;

const char* const kConsentRefusal =
    "this plugin runs a separate program; confirm that you want it installed before it is fetched";

namespace {

bool isHex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

bool wellFormedSha(const std::string& s) {
    if (s.size() != 64) { return false; }
    for (char c : s) {
        if (!isHex(c)) { return false; }
    }
    return true;
}

std::string lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }
    }
    return s;
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && lower(s.substr(s.size() - suffix.size())) == lower(suffix);
}

std::string stemOf(const std::string& name) {
    const std::size_t dot = name.rfind('.');
    return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

bool getString(const json& o, const char* key, std::string& out) {
    const auto it = o.find(key);
    if (it == o.end() || !it->is_string()) { return false; }
    out = it->get<std::string>();
    return !out.empty();
}

bool licenceIsCopyleft(const std::string& licence) {
    const std::string l = lower(licence);
    return l.find("gpl") != std::string::npos;   // GPL, LGPL, AGPL families (an SPDX id)
}

std::string pidText() {
#if defined(_WIN32)
    return std::to_string(_getpid());
#else
    return std::to_string(static_cast<int>(getpid()));
#endif
}

}  // namespace

std::string validInstallAs(const std::string& name) {
    if (name.empty()) { return "the file name is empty"; }
    if (name.size() > 96) { return "the file name is too long"; }
    for (unsigned char c : name) {
        if (c < 0x20 || c == 0x7F) { return "the file name holds a control character"; }
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') { return "the file name holds a character a bare name cannot"; }
    }
    if (name == "." || name == ".." || name.find("..") != std::string::npos) { return "the file name holds \"..\""; }
    if (name.front() == '.' || name.back() == '.' || name.back() == ' ') { return "the file name starts or ends with a dot or a space"; }
    const std::string l = lower(name);
    if (endsWith(l, ".dll") || endsWith(l, ".so") || endsWith(l, ".dylib")) { return "a companion is never a plugin module"; }
    if (endsWith(l, ".part") || endsWith(l, ".tmp") || l.find(".wcbak") != std::string::npos) { return "that name is the store's own"; }
    if (l == "installed.json" || l == "helpers.json") { return "that name is the store's own"; }
    // Windows reserved device names (the stem)
    const std::string stem = lower(stemOf(name));
    static const char* const kReserved[] = {"con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
    for (const char* r : kReserved) {
        if (stem == r) { return "a reserved device name"; }
    }
    return std::string();
}

bool parseHelperFields(const json& entryJson, PluginCatalogEntry& e, const std::string& where, std::string& error) {
    const auto hit = entryJson.find("helper");
    if (hit == entryJson.end() || hit->is_null()) { return true; }
    if (!hit->is_object()) {
        error = where + ": \"helper\" is not an object";
        return false;
    }
    HelperInfo h;
    std::string kind;
    if (!getString(*hit, "kind", kind) || kind != "gpl-helper") {
        error = where + ": helper.kind must be \"gpl-helper\"";
        return false;
    }
    if (!getString(*hit, "name", h.name) || !getString(*hit, "version", h.version) || !getString(*hit, "licence", h.licence) || !getString(*hit, "sourceUrl", h.sourceUrl) ||
        !getString(*hit, "sourceCommit", h.sourceCommit)) {
        error = where + ": a helper needs name, version, licence, sourceUrl and sourceCommit";
        return false;
    }
    if (!licenceIsCopyleft(h.licence)) {
        error = where + ": helper.licence must be a GPL-family licence (an SPDX id)";
        return false;
    }
    if (!pluginIsHttpsUrl(h.sourceUrl)) {
        error = where + ": helper.sourceUrl must be https";
        return false;
    }
    if (h.sourceCommit.size() != 40) {
        error = where + ": helper.sourceCommit must be 40 hexadecimal digits";
        return false;
    }
    for (char c : h.sourceCommit) {
        if (!isHex(c)) {
            error = where + ": helper.sourceCommit must be 40 hexadecimal digits";
            return false;
        }
    }
    for (const std::string* s : {&h.name, &h.version, &h.licence}) {
        if (s->size() > 120) {
            error = where + ": a helper field is too long";
            return false;
        }
    }
    if (h.sourceUrl.size() > 400) {
        error = where + ": helper.sourceUrl is too long";
        return false;
    }
    h.present = true;

    // every platform row carries its companions (a row without them is refused: it would install the bridge alone, with no program to run)
    const auto platIt = entryJson.find("platforms");
    std::size_t row = 0;
    if (platIt != entryJson.end() && platIt->is_array()) {
        for (const json& plj : *platIt) {
            const std::string pwhere = where + " platform " + std::to_string(row);
            PluginPlatform& p = e.platforms[row];
            ++row;
            const auto cit = plj.find("companions");
            if (cit == plj.end() || !cit->is_array() || cit->empty()) {
                error = pwhere + ": a helper plugin's platform row must list its companion files";
                return false;
            }
            if (cit->size() > kMaxCompanionsPerRow) {
                error = pwhere + ": too many companion files";
                return false;
            }
            std::set<std::string> names;
            int helpers = 0, manifests = 0, licences = 0;
            for (const json& cj : *cit) {
                HelperCompanion c;
                if (!cj.is_object() || !getString(cj, "role", c.role) || !getString(cj, "file", c.file) || !getString(cj, "installAs", c.installAs) || !getString(cj, "url", c.url) ||
                    !getString(cj, "sha256", c.sha256)) {
                    error = pwhere + ": a companion needs role, file, installAs, url and sha256";
                    return false;
                }
                if (c.role != "helper" && c.role != "manifest" && c.role != "licence" && c.role != "other") {
                    error = pwhere + ": unknown companion role \"" + c.role + "\"";
                    return false;
                }
                const std::string why = validInstallAs(c.installAs);
                if (!why.empty()) {
                    error = pwhere + ": installAs \"" + c.installAs + "\": " + why;
                    return false;
                }
                if (!names.insert(lower(c.installAs)).second) {
                    error = pwhere + ": two companions install as the same name";
                    return false;
                }
                if (!pluginIsHttpsUrl(c.url)) {
                    error = pwhere + ": a companion's url is not https";
                    return false;
                }
                if (!wellFormedSha(c.sha256)) {
                    error = pwhere + ": a companion's sha256 must be 64 hexadecimal digits";
                    return false;
                }
                c.sha256 = lower(c.sha256);
                const auto sz = cj.find("sizeBytes");
                if (sz != cj.end() && sz->is_number_unsigned()) { c.sizeBytes = sz->get<std::uint64_t>(); }
                const auto ex = cj.find("executable");
                c.executable = ex != cj.end() && ex->is_boolean() && ex->get<bool>();
                if (c.executable != (c.role == "helper")) {
                    error = pwhere + ": only the helper program is executable";
                    return false;
                }
                if (c.role == "helper") { ++helpers; }
                if (c.role == "manifest") { ++manifests; }
                if (c.role == "licence") { ++licences; }
                p.companions.push_back(std::move(c));
            }
            if (helpers != 1 || manifests != 1 || licences != 1) {
                error = pwhere + ": exactly one helper, one manifest and one licence companion are required";
                return false;
            }
            // the names follow from the helper program's: <stem>.helper.json and <stem>.LICENSE.txt, where the bridge looks for them
            std::string program;
            for (const HelperCompanion& c : p.companions) {
                if (c.role == "helper") { program = c.installAs; }
            }
            const std::string stem = stemOf(program);
            for (const HelperCompanion& c : p.companions) {
                if (c.role == "manifest" && lower(c.installAs) != lower(stem + ".helper.json")) {
                    error = pwhere + ": the manifest must install as \"" + stem + ".helper.json\"";
                    return false;
                }
                if (c.role == "licence" && lower(c.installAs) != lower(stem + ".LICENSE.txt")) {
                    error = pwhere + ": the licence must install as \"" + stem + ".LICENSE.txt\"";
                    return false;
                }
            }
        }
    }
    e.helper = std::move(h);
    return true;
}

bool looksLikeProgram(const fs::path& file, const std::string& os, const std::string& arch, std::string& error) {
    std::ifstream f(file, std::ios::binary);
    if (!f) {
        error = "the helper program could not be read";
        return false;
    }
    unsigned char head[4096] = {};
    f.read(reinterpret_cast<char*>(head), sizeof(head));
    const std::size_t n = static_cast<std::size_t>(f.gcount());
    if (os == "windows") {
        if (n < 0x40 || head[0] != 'M' || head[1] != 'Z') {
            error = "the helper program is not a Windows program";
            return false;
        }
        const std::uint32_t pe = static_cast<std::uint32_t>(head[0x3C]) | (static_cast<std::uint32_t>(head[0x3D]) << 8) | (static_cast<std::uint32_t>(head[0x3E]) << 16) |
                                 (static_cast<std::uint32_t>(head[0x3F]) << 24);
        if (pe < 0x40 || pe + 8 > n || std::memcmp(head + pe, "PE\0\0", 4) != 0) {
            error = "the helper program is not a Windows program (no PE header)";
            return false;
        }
        const unsigned machine = static_cast<unsigned>(head[pe + 4]) | (static_cast<unsigned>(head[pe + 5]) << 8);
        const unsigned want = arch == "x64" ? 0x8664u : arch == "arm64" ? 0xAA64u : arch == "x86" ? 0x014Cu : 0u;
        if (want == 0u || machine != want) {
            error = "the helper program is built for another kind of machine";
            return false;
        }
        return true;
    }
    if (os == "linux") {
        if (n < 20 || head[0] != 0x7F || head[1] != 'E' || head[2] != 'L' || head[3] != 'F') {
            error = "the helper program is not a Linux program";
            return false;
        }
        const unsigned cls = head[4];
        const unsigned machine = static_cast<unsigned>(head[18]) | (static_cast<unsigned>(head[19]) << 8);
        const bool ok = (arch == "x64" && cls == 2 && machine == 62) || (arch == "arm64" && cls == 2 && machine == 183) || (arch == "x86" && cls == 1 && machine == 3);
        if (!ok) {
            error = "the helper program is built for another kind of machine";
            return false;
        }
        return true;
    }
    error = "helper programs are not supported on this system";
    return false;
}

Txn::~Txn() {
    if (!committed_) {
        std::error_code ec;
        if (!stageDir_.empty()) { fs::remove_all(stageDir_, ec); }
    }
}

bool Txn::stage(const PluginCatalogEntry& e, const PluginPlatform& p, std::atomic<float>* progress, std::atomic<bool>* cancel, std::string& error) {
    error.clear();
    if (!e.helper.present) { return true; }
    if (!e.source.empty()) {
        error = "\"" + e.name + "\" comes from a plugin site and runs a separate program; the store does not install programs from sites";
        return false;
    }
    if (p.companions.empty()) {
        error = "\"" + e.name + "\" lists no files for its helper program";
        return false;
    }
    std::error_code ec;
    stageDir_ = dir_ / (".helper-stage-" + pidText());
    fs::remove_all(stageDir_, ec);
    fs::create_directories(stageDir_, ec);
    if (!fs::is_directory(stageDir_, ec)) {
        error = "the staging folder for the helper program could not be made";
        return false;
    }
    std::string programName, manifestName;
    for (const HelperCompanion& c : p.companions) {
        const std::string why = validInstallAs(c.installAs);
        if (!why.empty()) {
            error = "\"" + c.installAs + "\": " + why;
            return false;
        }
        Staged s;
        s.name = c.installAs;
        s.staged = stageDir_ / c.installAs;
        s.target = dir_ / c.installAs;
        s.executable = c.executable;
        const std::uint64_t cap = c.role == "helper" ? kMaxHelperBytes : kMaxCompanionTextBytes;
        PluginRepo::FetchFailure failure = PluginRepo::FetchFailure::None;
        std::string fetchError;
        if (!PluginRepo::fetchVerifiedFile(c.url, c.sha256, s.staged.string(), cap, fetchError, progress, cancel, &failure)) {
            error = failure == PluginRepo::FetchFailure::Verify ? "\"" + c.installAs + "\" failed its integrity check and was discarded" : "\"" + c.installAs + "\": " + fetchError;
            return false;
        }
        if (c.role == "helper") {
            programName = c.installAs;
            std::string imageError;
            if (!looksLikeProgram(s.staged, lower(p.os), lower(p.arch), imageError)) {
                error = "\"" + c.installAs + "\": " + imageError;
                return false;
            }
        }
        if (c.role == "manifest") {
            manifestName = c.installAs;
            std::string text;
            if (cascade::core::safeio::readCapped(s.staged, kMaxCompanionTextBytes, text) != cascade::core::safeio::ReadResult::Ok) {
                error = "the helper's manifest could not be read";
                return false;
            }
            const json j = json::parse(text, nullptr, false);
            std::string exe = j.is_object() && j.contains("executable") && j["executable"].is_string() ? j["executable"].get<std::string>() : std::string();
            std::string prog;
            for (const HelperCompanion& o : p.companions) {
                if (o.role == "helper") { prog = o.installAs; }
            }
            if (exe.empty() || lower(exe) != lower(prog)) {
                error = "the helper's manifest does not name the helper program that is installed with it";
                return false;
            }
        }
        if (c.role == "licence") {
            std::string text;
            if (cascade::core::safeio::readCapped(s.staged, kMaxCompanionTextBytes, text) != cascade::core::safeio::ReadResult::Ok || text.empty()) {
                error = "the helper's licence text is empty";
                return false;
            }
        }
        names_.push_back(c.installAs);
        files_.push_back(std::move(s));
    }
    (void)programName;
    (void)manifestName;
    return true;
}

bool Txn::commit(std::string& error) {
    error.clear();
    std::error_code ec;
    for (Staged& f : files_) {
        if (fs::exists(f.target, ec)) {
            f.backup = f.target;
            f.backup += ".wcbak-" + pidText();
            fs::remove(f.backup, ec);
            fs::rename(f.target, f.backup, ec);
            if (ec) {
                error = "\"" + f.name + "\" is in use and cannot be replaced";
                f.backup.clear();
                rollback();
                return false;
            }
        }
        fs::rename(f.staged, f.target, ec);
        if (ec) {
            error = "\"" + f.name + "\" could not be moved into place: " + ec.message();
            rollback();
            return false;
        }
        f.movedIn = true;
#if !defined(_WIN32)
        if (f.executable) { ::chmod(f.target.string().c_str(), 0755); }
#endif
    }
    committed_ = true;
    return true;
}

void Txn::rollback() {
    std::error_code ec;
    for (auto it = files_.rbegin(); it != files_.rend(); ++it) {
        if (it->movedIn) {
            fs::remove(it->target, ec);
            it->movedIn = false;
        }
        if (!it->backup.empty() && fs::exists(it->backup, ec)) {
            fs::rename(it->backup, it->target, ec);
            it->backup.clear();
        }
    }
    committed_ = false;
    if (!stageDir_.empty()) { fs::remove_all(stageDir_, ec); }
}

void Txn::finish() {
    std::error_code ec;
    for (Staged& f : files_) {
        if (!f.backup.empty()) { fs::remove(f.backup, ec); }
        f.backup.clear();
    }
    if (!stageDir_.empty()) { fs::remove_all(stageDir_, ec); }
    committed_ = true;
}

namespace {

fs::path registryPath(const std::string& pluginsDir) { return fs::path(pluginsDir) / "helpers.json"; }

json loadRegistry(const std::string& pluginsDir) {
    std::string text;
    if (cascade::core::safeio::readCapped(registryPath(pluginsDir), 1024u * 1024u, text) != cascade::core::safeio::ReadResult::Ok) { return json::object(); }
    json j = json::parse(text, nullptr, false);
    return j.is_object() ? j : json::object();
}

}  // namespace

std::vector<std::string> companionsOf(const std::string& pluginsDir, const std::string& pluginFile) {
    std::vector<std::string> out;
    const json j = loadRegistry(pluginsDir);
    const auto it = j.find(pluginFile);
    if (it != j.end() && it->is_array()) {
        for (const json& n : *it) {
            if (n.is_string() && validInstallAs(n.get<std::string>()).empty()) { out.push_back(n.get<std::string>()); }
        }
    }
    return out;
}

bool recordCompanions(const std::string& pluginsDir, const std::string& pluginFile, const std::vector<std::string>& names, std::string& error) {
    error.clear();
    json j = loadRegistry(pluginsDir);
    // companions of an earlier version that this version no longer has are removed with it
    std::error_code ec;
    for (const std::string& old : companionsOf(pluginsDir, pluginFile)) {
        if (std::find(names.begin(), names.end(), old) == names.end()) { fs::remove(fs::path(pluginsDir) / old, ec); }
    }
    if (names.empty()) {
        j.erase(pluginFile);
    } else {
        j[pluginFile] = names;
    }
    if (j.empty()) {
        fs::remove(registryPath(pluginsDir), ec);
        return true;
    }
    if (!cascade::core::safeio::writeAtomic(registryPath(pluginsDir), j.dump(1, ' ', false, json::error_handler_t::replace) + "\n")) {
        error = "the helper program list could not be saved";
        return false;
    }
    return true;
}

bool removeCompanions(const std::string& pluginsDir, const std::string& pluginFile, std::string& error) {
    error.clear();
    bool clean = true;
    std::error_code ec;
    for (const std::string& n : companionsOf(pluginsDir, pluginFile)) {
        const fs::path p = fs::path(pluginsDir) / n;
        fs::remove(p, ec);
        if (fs::exists(p, ec)) {
            clean = false;
            error = "\"" + n + "\" could not be deleted (it is probably in use)";
        }
    }
    if (!clean) { return false; }
    return recordCompanions(pluginsDir, pluginFile, {}, error);
}

std::string rowLine(const PluginCatalogEntry& e) {
    if (!e.helper.present) { return std::string(); }
    return "GPL helper - source: " + e.helper.sourceUrl;
}

std::string consentSentence(const PluginCatalogEntry& e) {
    return e.helper.present ? std::string(kConsentRefusal) : std::string();
}

std::vector<std::string> detailLines(const PluginCatalogEntry& e) {
    std::vector<std::string> out;
    if (!e.helper.present) { return out; }
    out.push_back("This plugin uses a separate program, " + e.helper.name + " " + e.helper.version + " (" + e.helper.licence +
                  "). FoxSDR starts it and sends it audio; it is not part of FoxSDR and cannot see FoxSDR's data.");
    out.push_back("Source code: " + e.helper.sourceUrl);
    const PluginPlatform* p = e.thisPlatform();
    if (p != nullptr && !p->companions.empty()) {
        out.push_back("Files that will be installed beside the plugin:");
        for (const HelperCompanion& c : p->companions) {
            char size[48];
            const double kb = static_cast<double>(c.sizeBytes) / 1024.0;
            if (c.sizeBytes == 0) { size[0] = '\0'; }
            else if (kb >= 1024.0) { std::snprintf(size, sizeof(size), " (%.1f MB)", kb / 1024.0); }
            else { std::snprintf(size, sizeof(size), " (%.0f KB)", kb < 1.0 ? 1.0 : kb); }
            out.push_back(c.installAs + size);
        }
    }
    return out;
}

}  // namespace cascade::core::helperstore
