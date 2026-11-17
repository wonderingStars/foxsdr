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

const std::vector<std::string>& importLeftOutKeys() {
    static const std::vector<std::string> k = {"pluginCatalogueUrl", "pluginSites", "webBindAddress", "catBindAll", "catAllow", "tciBindAll", "tciAllow", "udpAudioHost",
                                               "plutoUri", "rtlTcpAddr", "iqFilePath"};
    return k;
}

const std::vector<std::string>& importFilteredKeys() {
    static const std::vector<std::string> k = {"sourceKind", "soapyArgs", "nativeArgs", "radios"};
    return k;
}

namespace {

bool plainValue(const std::string& v) {
    if (v.empty() || v.size() > 64) { return false; }
    for (const char c : v) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ':';
        if (!ok) { return false; }
    }
    return true;
}

bool localDriverName(const std::string& d) {
    static const std::set<std::string> kLocal = {"rtlsdr", "hackrf", "airspy", "airspyhf", "sdrplay", "mirisdr", "miri", "bladerf", "lime", "limesdr", "uhd", "hydrasdr", "rx888", "aor"};
    return kLocal.count(lowerOf(d)) != 0;
}

}  // namespace

bool localSourceKind(const std::string& kind) {
    static const std::set<std::string> kLocal = {"siggen", "soapy", "rtlsdr", "hackrf", "airspy", "airspyhf", "sdrplay", "mirisdr", "rx888", "aor", "hydrasdr", "soundcard", "fobos",
                                                       // the USB radios of the wire-b source table (engine/wire_b_sources.cpp, the rows whose `needsAddress` is false: they name a USB device or a vendor
                                                       // library, never a host). tests/test_wire_b_saved_kinds.cpp holds this list equal to that table in both directions.
                                                       "icr8600", "funcube", "g39ddc", "g33ddc", "sdriq"};
    return kLocal.count(kind) != 0;
}

bool classifyDeviceArgs(const std::string& args, std::string& cleaned, bool& dropped) {
    cleaned.clear();
    dropped = false;
    if (args.empty()) { return true; }
    if (args.size() > 512) { return false; }
    std::size_t pos = 0;
    int fields = 0;
    for (;;) {
        std::size_t end = args.find(',', pos);
        if (end == std::string::npos) { end = args.size(); }
        std::string field = args.substr(pos, end - pos);
        while (!field.empty() && field.front() == ' ') { field.erase(field.begin()); }
        while (!field.empty() && field.back() == ' ') { field.pop_back(); }
        const std::size_t eq = field.find('=');
        if (eq == std::string::npos || eq == 0 || ++fields > 16) { return false; }   // not key=value: cannot be classified
        const std::string key = lowerOf(field.substr(0, eq));
        const std::string val = field.substr(eq + 1);
        // The options the USB radios of the wire-b table save with their device (a radio opened by "open-icr8600:" comes back with "timeout=1500", a Fobos with "timeout=5000"):
        // whole numbers (timeout, rate, silence), the SDR-IQ's serial, and the words of the Fobos' three switches. A library or firmware PATH (lib, firmware) is still not one.
        const bool numberKey = key == "timeout" || key == "rate" || key == "silence";
        const bool wordKey = (key == "count" && (val == "floats" || val == "pairs")) || (key == "direct" && (val == "on" || val == "off")) ||
                             (key == "clock" && (val == "internal" || val == "external"));
        const bool keyOk = key == "driver" || key == "serial" || key == "index" || key == "channel" || key == "deviceid" || key == "device_id" || key == "sdriq" || numberKey || wordKey;
        bool valOk = keyOk && plainValue(val);
        if (valOk && numberKey) { for (const char c : val) { if (c < '0' || c > '9') { valOk = false; } } }
        if (valOk && key == "driver") { if (!localDriverName(val)) { return false; } }   // a remote driver: the whole string is another computer's
        if (valOk && (key == "index" || key == "channel")) { for (const char c : val) { if (c < '0' || c > '9') { valOk = false; } } }
        if (valOk && (key == "serial" || key == "sdriq") && val.find(':') != std::string::npos) { valOk = false; }
        if (valOk) { cleaned += (cleaned.empty() ? "" : ",") + key + "=" + val; } else { dropped = true; }
        if (end == args.size()) { break; }
        pos = end + 1;
    }
    return true;
}

// A saved radio object of config.json's `radios`: true when it names a local device and carries no address, path or remote driver.
static bool localSavedRadio(const json& radio) {
    if (!radio.is_object()) { return false; }
    const auto k = radio.find("sourceKind");
    if (k == radio.end() || !k->is_string() || !localSourceKind(k->get<std::string>())) { return false; }
    const auto a = radio.find("args");
    if (a == radio.end()) { return true; }
    if (!a->is_string()) { return false; }
    std::string cleaned;
    bool dropped = false;
    return classifyDeviceArgs(a->get<std::string>(), cleaned, dropped) && !dropped;
}

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
            std::string left;
            for (const std::string& k : importLeftOutKeys()) {
                const auto it = c.find(k);
                if (it == c.end()) { continue; }
                c.erase(it);
                left += (left.empty() ? "" : ", ") + k;
            }
            if (!left.empty()) {
                r.notes.push_back("config.json: these settings decide where plugins come from, what this program listens on, or make it connect to another computer or open a file at start, so they were NOT imported (this computer keeps its own): " + left);
            }
            // THE SOURCE CHOICE AND THE SAVED RADIOS: what names a local device comes back; what names another computer or a file, or cannot be classified, does not.
            {
                int radiosOut = 0, argsOut = 0;
                bool kindOut = false;
                const auto sk = c.find("sourceKind");
                if (sk != c.end() && !(sk->is_string() && localSourceKind(sk->get<std::string>()))) { c.erase(sk); kindOut = true; }
                for (const char* key : {"soapyArgs", "nativeArgs"}) {
                    const auto it = c.find(key);
                    if (it == c.end()) { continue; }
                    std::string cleaned;
                    bool dropped = false;
                    if (kindOut || !it->is_string() || !classifyDeviceArgs(it->get<std::string>(), cleaned, dropped)) { c.erase(it); ++argsOut; continue; }
                    if (dropped) { ++argsOut; }
                    *it = cleaned;
                }
                const auto rd = c.find("radios");
                if (rd != c.end()) {
                    if (!rd->is_array()) { c.erase(rd); ++radiosOut; }
                    else {
                        json keep = json::array();
                        for (const json& one : *rd) { if (localSavedRadio(one)) { keep.push_back(one); } else { ++radiosOut; } }
                        *rd = keep;
                    }
                }
                if (kindOut) { r.notes.push_back("config.json: the source this backup was using connects to another computer or opens a file, so it was NOT imported (this computer keeps its own choice)"); }
                if (argsOut > 0) { r.notes.push_back("config.json: some device arguments carried a host, an address, a path or a remote driver, or could not be checked, so they were NOT imported"); }
                if (radiosOut > 0) { r.notes.push_back("config.json: " + std::to_string(radiosOut) + " saved radio(s) connect to another computer or open a file, or could not be checked, so they were NOT imported (this computer keeps its own)"); }
            }
            // REPORTING: an import may only make it more private (commitImport keeps a switch that is off here off).
            for (const char* sw : {"telemetryEnabled", "diagnosticsEnabled"}) {
                const auto it = c.find(sw);
                if (it == c.end()) { continue; }
                if (!it->is_boolean()) { c.erase(it); continue; }
                if (it->get<bool>()) { r.notes.push_back(std::string("config.json: ") + sw + " is on in this backup; an import can switch reporting off but never on, so a switch that is off here stays off"); }
            }
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
        std::string bytes = bytesOf(it->name);
        if (it->name == "config.json" && it->action == ImportAction::Replace) {
            // This computer's own values of the settings a bundle may never change (importLeftOutKeys) stay as they are.
            std::string own;
            if (readAll(target, kBundleMaxFileBytes * 2, own)) {
                json mine = json::parse(own, nullptr, /*allow_exceptions=*/false);
                json theirs = json::parse(bytes, nullptr, /*allow_exceptions=*/false);
                if (mine.is_object() && theirs.is_object()) {
                    for (const std::string& k : importLeftOutKeys()) {
                        const auto m = mine.find(k);
                        if (m != mine.end()) { theirs[k] = *m; }
                    }
                    // the source choice: the backup's local choice wins; with none in the backup this computer's own trio stays
                    if (theirs.find("sourceKind") == theirs.end()) {
                        for (const char* k : {"sourceKind", "soapyArgs", "nativeArgs"}) {
                            const auto m = mine.find(k);
                            if (m != mine.end()) { theirs[k] = *m; }
                        }
                    }
                    // the saved radios: the backup's local radios, plus this computer's own entries that are not local ones (a network radio) unless the number is taken
                    {
                        json merged = json::array();
                        std::set<long long> used;
                        const auto tr = theirs.find("radios");
                        if (tr != theirs.end() && tr->is_array()) {
                            for (const json& one : *tr) {
                                merged.push_back(one);
                                const auto n = one.find("number");
                                if (n != one.end() && n->is_number_integer()) { used.insert(n->get<long long>()); }
                            }
                        }
                        const auto mr = mine.find("radios");
                        if (mr != mine.end() && mr->is_array()) {
                            for (const json& one : *mr) {
                                if (localSavedRadio(one)) { continue; }
                                const auto n = one.find("number");
                                if (n != one.end() && n->is_number_integer() && used.count(n->get<long long>()) != 0) { continue; }
                                merged.push_back(one);
                            }
                        }
                        if (!merged.empty()) { theirs["radios"] = merged; } else { theirs.erase("radios"); }
                    }
                    // reporting: only ever MORE private (a switch that is off here stays off)
                    for (const char* sw : {"telemetryEnabled", "diagnosticsEnabled"}) {
                        const auto m = mine.find(sw);
                        if (m != mine.end() && m->is_boolean() && !m->get<bool>()) { theirs[sw] = false; }
                    }
                    bytes = dumpJson(theirs);
                }
            }
        }
        if (!writeAll(tmp, bytes)) { error = "cannot write " + it->name; rollback(); return false; }
    }
    // 3. rename into place.
    for (const auto* it : todo) {
        const fs::path target = destDir / it->name;
        const fs::path tmp = destDir / (it->name + ".import-tmp");
        fs::rename(tmp, target, ec);
        if (ec) { error = "cannot put " + it->name + " in place: " + ec.message(); rollback(); return false; }
        if (it->action == ImportAction::New) { placedNew.push_back(target); }
        // An imported settings file carries no secrets (export leaves them out): the protected copy of the file it replaced goes too, so the
        // secrets of the old file are not read back into the new one.
        if (it->name == "config.json") { fs::remove(fs::path(target.string() + ".secrets"), ec); }   // core/secret_vault.hpp sidecarPath(): "<settings file>.secrets" (not linked here: three test programs build this file alone)
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
