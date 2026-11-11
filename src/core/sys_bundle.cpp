// sys_bundle.cpp - see sys_bundle.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/sys_bundle.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <set>

#include <nlohmann/json.hpp>

#include "core/sys_hash.hpp"
#include "core/sys_manifest_sig.hpp"   // base64

namespace fs = std::filesystem;
using nlohmann::json;

namespace cascade::core::sys {
namespace {

std::string lowerOf(std::string s) {
    for (auto& c : s) { if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); } }
    return s;
}

bool jsonDepthOk(const std::string& text, int maxDepth) {
    int depth = 0;
    bool inString = false, escape = false;
    for (char c : text) {
        if (inString) {
            if (escape) { escape = false; }
            else if (c == '\\') { escape = true; }
            else if (c == '"') { inString = false; }
            continue;
        }
        if (c == '"') { inString = true; }
        else if (c == '[' || c == '{') { if (++depth > maxDepth) { return false; } }
        else if (c == ']' || c == '}') { --depth; }
    }
    return true;
}

std::string dumpJson(const json& j) { return j.dump(2, ' ', false, json::error_handler_t::replace); }

void stripKeys(json& j, bool identity, const std::string& path, std::vector<std::string>& removed) {
    if (j.is_object()) {
        std::vector<std::string> kill;
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (secretKeyName(it.key()) || (identity && identityKeyName(it.key()))) { kill.push_back(it.key()); }
        }
        for (const auto& k : kill) { removed.push_back(path.empty() ? k : path + "." + k); j.erase(k); }
        for (auto it = j.begin(); it != j.end(); ++it) { stripKeys(it.value(), identity, path.empty() ? it.key() : path + "." + it.key(), removed); }
    } else if (j.is_array()) {
        std::size_t i = 0;
        for (auto& e : j) { stripKeys(e, identity, path + "[" + std::to_string(i++) + "]", removed); }
    }
}

bool readAll(const fs::path& p, std::size_t cap, std::string& out) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) { return false; }
    const auto size = fs::file_size(p, ec);
    if (ec || size > cap) { return false; }
    std::ifstream f(p, std::ios::binary);
    if (!f) { return false; }
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

bool writeAll(const fs::path& p, const std::string& bytes) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) { return false; }
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    f.flush();
    return static_cast<bool>(f);
}

bool validStamp(const std::string& s) {
    if (s.empty() || s.size() > 32) { return false; }
    for (unsigned char c : s) { if (!(std::isalnum(c) || c == '-')) { return false; } }
    return true;
}

bool hex64(const std::string& s) {
    if (s.size() != 64) { return false; }
    for (char c : s) { if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { return false; } }
    return true;
}

}  // namespace

const char* bundleErrorName(BundleError e) {
    switch (e) {
        case BundleError::None: return "none";
        case BundleError::TooLarge: return "too-large";
        case BundleError::NotJson: return "not-json";
        case BundleError::NotABundle: return "not-a-bundle";
        case BundleError::UnknownVersion: return "unknown-version";
        case BundleError::BadManifest: return "bad-manifest";
        case BundleError::TooManyFiles: return "too-many-files";
        case BundleError::EmptyBundle: return "empty-bundle";
        case BundleError::BadName: return "bad-name";
        case BundleError::DuplicateName: return "duplicate-name";
        case BundleError::BadBase64: return "bad-base64";
        case BundleError::SizeMismatch: return "size-mismatch";
        case BundleError::HashMismatch: return "hash-mismatch";
        case BundleError::BadContent: return "bad-content";
    }
    return "?";
}

bool allowedBundleFileName(const std::string& name) { return name == "config.json" || name == "bookmarks.json" || name == "markers.json"; }

bool secretKeyName(const std::string& key) {
    const std::string k = lowerOf(key);
    for (const char* frag : {"password", "passwd", "passphrase", "secret", "token", "apikey", "api_key", "privatekey", "private_key", "credential"}) {
        if (k.find(frag) != std::string::npos) { return true; }
    }
    return false;
}

bool identityKeyName(const std::string& key) {
    const std::string k = lowerOf(key);
    if (k == "installid") { return true; }
    return k.rfind("telemetry", 0) == 0 && k != "telemetryenabled";
}

std::string exportBundle(const std::vector<BundleFile>& files, const ExportOptions& opt, ExportReport& report) {
    report = ExportReport{};
    auto fail = [&](BundleError e, const std::string& d) { report.error = e; report.detail = d; return std::string(); };
    if (files.empty()) { return fail(BundleError::EmptyBundle, "nothing to export"); }
    if (files.size() > kBundleMaxFiles) { return fail(BundleError::TooManyFiles, "too many files"); }
    std::set<std::string> seen;
    std::size_t total = 0;
    json outFiles = json::array();
    for (const auto& f : files) {
        if (!allowedBundleFileName(f.name)) { return fail(BundleError::BadName, "not an exportable file: " + f.name); }
        if (!seen.insert(f.name).second) { return fail(BundleError::DuplicateName, f.name); }
        if (f.bytes.size() > kBundleMaxFileBytes) { return fail(BundleError::TooLarge, f.name); }
        if (!jsonDepthOk(f.bytes, kBundleMaxJsonDepth)) { return fail(BundleError::BadContent, f.name + " is nested too deeply"); }
        json j = json::parse(f.bytes, nullptr, /*allow_exceptions=*/false);
        if (j.is_discarded() || !j.is_object()) { return fail(BundleError::BadContent, f.name + " is not a JSON object"); }
        std::vector<std::string> removed;
        stripKeys(j, opt.stripIdentity, "", removed);
        for (const auto& r : removed) { report.stripped.push_back(f.name + ":" + r); }
        const std::string stored = dumpJson(j);
        total += stored.size();
        if (total > kBundleMaxTotalBytes) { return fail(BundleError::TooLarge, "total"); }
        json e;
        e["name"] = f.name;
        e["size"] = stored.size();
        e["sha256"] = sha256Hex(stored);
        e["data"] = base64Encode(stored);
        outFiles.push_back(std::move(e));
    }
    json b;
    b["format"] = "foxsdr-settings-bundle";
    b["version"] = kBundleVersion;
    if (!opt.appVersion.empty()) { b["app"] = opt.appVersion; }
    if (!opt.createdUtc.empty()) { b["created"] = opt.createdUtc; }
    b["stripped"] = report.stripped;
    b["files"] = std::move(outFiles);
    const std::string text = dumpJson(b);
    if (text.size() > kBundleMaxTextBytes) { return fail(BundleError::TooLarge, "bundle text"); }
    return text;
}

ImportResult parseBundle(const std::string& text) {
    ImportResult r;
    auto fail = [&](BundleError e, const std::string& d) { r.error = e; r.detail = d; r.files.clear(); r.notes.clear(); return r; };
    if (text.size() > kBundleMaxTextBytes) { return fail(BundleError::TooLarge, "the file is too large to be a settings bundle"); }
    if (!jsonDepthOk(text, kBundleMaxJsonDepth)) { return fail(BundleError::NotJson, "nested too deeply"); }
    json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) { return fail(BundleError::NotJson, "not JSON"); }
    if (!j.is_object()) { return fail(BundleError::NotABundle, "not an object"); }
    const auto fmt = j.find("format");
    if (fmt == j.end() || !fmt->is_string() || fmt->get<std::string>() != "foxsdr-settings-bundle") { return fail(BundleError::NotABundle, "not a FoxSDR settings bundle"); }
    const auto ver = j.find("version");
    if (ver == j.end() || !ver->is_number_integer()) { return fail(BundleError::BadManifest, "no version"); }
    if (ver->get<long long>() != kBundleVersion) { return fail(BundleError::UnknownVersion, "version " + std::to_string(ver->get<long long>()) + " is not understood"); }
    const auto app = j.find("app");
    if (app != j.end() && app->is_string()) { r.appVersion = app->get<std::string>().substr(0, 64); }
    const auto files = j.find("files");
    if (files == j.end() || !files->is_array()) { return fail(BundleError::BadManifest, "no file list"); }
    if (files->empty()) { return fail(BundleError::EmptyBundle, "the bundle holds no files"); }
    if (files->size() > kBundleMaxFiles) { return fail(BundleError::TooManyFiles, "too many files"); }

    // First pass: every declared size, name and hash text, BEFORE anything is decoded.
    struct Decl { std::string name, sha, data; std::size_t size; };
    std::vector<Decl> decls;
    std::set<std::string> seen;
    std::size_t declaredTotal = 0;
    for (const auto& e : *files) {
        if (!e.is_object()) { return fail(BundleError::BadManifest, "a file entry is not an object"); }
        const auto n = e.find("name"), s = e.find("size"), h = e.find("sha256"), d = e.find("data");
        if (n == e.end() || !n->is_string() || s == e.end() || !s->is_number_unsigned() || h == e.end() || !h->is_string() || d == e.end() || !d->is_string()) {
            return fail(BundleError::BadManifest, "a file entry is missing a field or has one of the wrong type");
        }
        Decl dl{n->get<std::string>(), h->get<std::string>(), {}, static_cast<std::size_t>(s->get<unsigned long long>())};
        if (!allowedBundleFileName(dl.name)) { return fail(BundleError::BadName, "not a file a bundle may carry"); }
        if (!seen.insert(dl.name).second) { return fail(BundleError::DuplicateName, dl.name); }
        if (!hex64(dl.sha)) { return fail(BundleError::BadManifest, dl.name + ": the hash is not 64 lower-case hex digits"); }
        if (s->get<unsigned long long>() > kBundleMaxFileBytes) { return fail(BundleError::TooLarge, dl.name + " is over the per-file limit"); }
        declaredTotal += dl.size;
        if (declaredTotal > kBundleMaxTotalBytes) { return fail(BundleError::TooLarge, "the declared total is over the limit"); }
        dl.data = d->get_ref<const std::string&>();
        decls.push_back(std::move(dl));
    }
    // The data must be exactly as long as its declaration says, checked for every file before any of it is decoded.
    for (const auto& dl : decls) {
        if (dl.data.size() != (dl.size + 2) / 3 * 4) { return fail(BundleError::SizeMismatch, dl.name + ": the data is not the declared size"); }
    }
    // Second pass: decode, check, validate, sanitise.
    for (auto& dl : decls) {
        std::string bytes;
        if (!base64Decode(dl.data, bytes)) { return fail(BundleError::BadBase64, dl.name); }
        if (bytes.size() != dl.size) { return fail(BundleError::SizeMismatch, dl.name); }
        if (sha256Hex(bytes) != dl.sha) { return fail(BundleError::HashMismatch, dl.name); }
        if (!jsonDepthOk(bytes, kBundleMaxJsonDepth)) { return fail(BundleError::BadContent, dl.name + " is nested too deeply"); }
        json c = json::parse(bytes, nullptr, /*allow_exceptions=*/false);
        if (c.is_discarded() || !c.is_object()) { return fail(BundleError::BadContent, dl.name + " is not a JSON object"); }
        std::vector<std::string> removed;
        stripKeys(c, /*identity=*/true, "", removed);
        if (!removed.empty()) { r.notes.push_back(dl.name + ": " + std::to_string(removed.size()) + " secret or identity value(s) in the bundle were NOT imported"); }
        if (dl.name == "config.json") {
            for (const char* sw : {"webEnabled", "catEnabled", "tciEnabled", "udpAudioEnabled"}) {
                const auto it = c.find(sw);
                if (it != c.end() && it->is_boolean() && it->get<bool>()) {
                    *it = false;
                    r.notes.push_back(std::string("config.json: ") + sw + " was imported as OFF; switch it on again yourself if you want it");
                }
            }
        }
        r.files.push_back(BundleFile{dl.name, dumpJson(c)});
    }
    return r;
}

namespace {

std::string shaOfFile(const fs::path& p, bool& present) {
    std::string bytes;
    present = false;
    std::error_code ec;
    if (!fs::exists(p, ec)) { return std::string(); }
    present = true;
    if (!readAll(p, kBundleMaxFileBytes * 2, bytes)) { return "unreadable"; }
    return sha256Hex(bytes);
}

std::string tokenFor(const ImportPlan& plan) {
    std::string s;
    for (const auto& it : plan.items) {
        s += it.name + "|" + std::to_string(static_cast<int>(it.action)) + "|" + it.newSha256 + "|" + it.presentSha256 + "\n";
    }
    return "IMPORT-" + sha256Hex(s).substr(0, 8);
}

}  // namespace

ImportPlan planImport(const ImportResult& r, const fs::path& destDir) {
    ImportPlan plan;
    for (const auto& f : r.files) {
        ImportPlanItem it;
        it.name = f.name;
        it.newSha256 = sha256Hex(f.bytes);
        bool present = false;
        it.presentSha256 = shaOfFile(destDir / f.name, present);
        it.action = !present ? ImportAction::New : (it.presentSha256 == it.newSha256 ? ImportAction::Identical : ImportAction::Replace);
        plan.items.push_back(std::move(it));
    }
    plan.confirmToken = tokenFor(plan);
    return plan;
}

bool commitImport(const ImportResult& r, const fs::path& destDir, const std::string& token, const std::string& stamp, std::string& error) {
    error.clear();
    if (r.error != BundleError::None || r.files.empty()) { error = "there is nothing valid to import"; return false; }
    if (!validStamp(stamp)) { error = "the stamp for the kept copies is not a plain stamp"; return false; }
    std::error_code ec;
    if (!fs::is_directory(destDir, ec)) {
        fs::create_directories(destDir, ec);
        if (ec) { error = "cannot create the settings folder"; return false; }
    }
    const ImportPlan plan = planImport(r, destDir);
    if (token.empty() || token != plan.confirmToken) { error = "the confirmation does not match: the files here changed since the plan was made, or the token is wrong"; return false; }

    std::vector<const ImportPlanItem*> todo;
    for (const auto& it : plan.items) { if (it.action != ImportAction::Identical) { todo.push_back(&it); } }
    auto bytesOf = [&](const std::string& name) -> const std::string& { for (const auto& f : r.files) { if (f.name == name) { return f.bytes; } } static const std::string none; return none; };

    std::vector<std::pair<fs::path, fs::path>> kept;   // target, kept copy
    std::vector<fs::path> placedNew;                   // targets that did not exist before
    std::vector<fs::path> temps;
    auto rollback = [&] {
        std::error_code e2;
        for (const auto& t : temps) { fs::remove(t, e2); }
        for (const auto& p : placedNew) { fs::remove(p, e2); }
        for (const auto& kv : kept) {
            fs::copy_file(kv.second, kv.first, fs::copy_options::overwrite_existing, e2);
        }
    };
    // 1. keep what will be replaced, 2. stage every new file beside its target.
    for (const auto* it : todo) {
        const fs::path target = destDir / it->name;
        if (it->action == ImportAction::Replace) {
            const fs::path keep = destDir / (it->name + ".before-import-" + stamp);
            fs::copy_file(target, keep, fs::copy_options::overwrite_existing, ec);
            if (ec) { error = "cannot keep a copy of " + it->name; rollback(); return false; }
            kept.emplace_back(target, keep);
        }
        const fs::path tmp = destDir / (it->name + ".import-tmp");
        temps.push_back(tmp);
        if (!writeAll(tmp, bytesOf(it->name))) { error = "cannot write " + it->name; rollback(); return false; }
    }
    // 3. rename into place.
    for (const auto* it : todo) {
        const fs::path target = destDir / it->name;
        const fs::path tmp = destDir / (it->name + ".import-tmp");
        fs::rename(tmp, target, ec);
        if (ec) { error = "cannot put " + it->name + " in place: " + ec.message(); rollback(); return false; }
        if (it->action == ImportAction::New) { placedNew.push_back(target); }
    }
    return true;
}

std::string backupFileName(const std::string& stamp) { return "foxsdr-settings-" + stamp + ".foxsdr-bundle"; }

bool isBackupFileName(const std::string& name) {
    const std::string pre = "foxsdr-settings-", suf = ".foxsdr-bundle";
    if (name.size() != pre.size() + 15 + suf.size() || name.compare(0, pre.size(), pre) != 0 || name.compare(name.size() - suf.size(), suf.size(), suf) != 0) { return false; }
    const std::string stamp = name.substr(pre.size(), 15);
    for (std::size_t i = 0; i < stamp.size(); ++i) {
        if (i == 8) { if (stamp[i] != '-') { return false; } }
        else if (stamp[i] < '0' || stamp[i] > '9') { return false; }
    }
    return true;
}

std::vector<std::string> pruneBackups(const fs::path& dir, std::size_t keepNewest) {
    std::vector<std::string> ours, removed;
    std::error_code ec;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) { continue; }
        const std::string n = it->path().filename().string();
        if (isBackupFileName(n)) { ours.push_back(n); }
    }
    std::sort(ours.begin(), ours.end());   // the stamp sorts oldest first
    if (ours.size() <= keepNewest) { return removed; }
    const std::size_t drop = ours.size() - keepNewest;
    for (std::size_t i = 0; i < drop; ++i) {
        std::error_code e3;
        if (fs::remove(dir / ours[i], e3) && !e3) { removed.push_back(ours[i]); }
    }
    return removed;
}

}  // namespace cascade::core::sys
