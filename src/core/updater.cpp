// Implementation of core/updater.hpp. See that header for what this
// deliberately does not do.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/updater.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "core/plugin_repo.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace cascade::core {

namespace {

// A manifest is a few kilobytes of text. The cap is generous for that and
// still small enough that a hostile server cannot make us allocate.
constexpr std::uint64_t kMaxManifestBytes = 256 * 1024;

// An installer is a few megabytes. Well above what we ship, far below anything
// that would fill a disk.
constexpr std::uint64_t kMaxInstallerBytes = 256ull * 1024ull * 1024ull;

// The only host an update may come from. The manifest supplies a URL, and a
// manifest is exactly the thing an attacker who could impersonate the endpoint
// would control - so the URL is checked against this rather than trusted. It
// is not sufficient on its own (the sha256 is what finally decides), but it
// keeps a redirect to somewhere unrelated from ever being attempted.
constexpr char kAllowedHost[] = "foxsdr.com";

// 64 lower- or upper-case hex digits and nothing else. Written here rather
// than borrowed because PluginRepo's copy is file-local to that translation
// unit; the rule is four lines and duplicating it is better than widening that
// class's surface for one predicate.
bool wellFormedSha256(const std::string& s) {
    if (s.size() != 64) { return false; }
    for (char c : s) {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                         (c >= 'A' && c <= 'F');
        if (!hex) { return false; }
    }
    return true;
}

bool urlIsOnAllowedHost(const std::string& url) {
    const std::string prefix = "https://";
    if (url.rfind(prefix, 0) != 0) { return false; }
    const std::size_t start = prefix.size();
    const std::size_t slash = url.find('/', start);
    const std::string host = url.substr(start, slash == std::string::npos ? std::string::npos
                                                                          : slash - start);
    // Exactly the host, or a subdomain of it. "evilfoxsdr.com" must not pass,
    // which a plain suffix test would allow.
    if (host == kAllowedHost) { return true; }
    const std::string dotted = std::string(".") + kAllowedHost;
    return host.size() > dotted.size() &&
           host.compare(host.size() - dotted.size(), dotted.size(), dotted) == 0;
}

std::vector<int> numericParts(const std::string& v, std::string& pre) {
    pre.clear();
    std::string num = v;
    const std::size_t cut = num.find_first_of("-+");
    if (cut != std::string::npos) {
        pre = num.substr(cut + 1);
        num = num.substr(0, cut);
    }
    std::vector<int> out;
    std::size_t i = 0;
    while (i <= num.size()) {
        const std::size_t dot = num.find('.', i);
        const std::string part =
            num.substr(i, dot == std::string::npos ? std::string::npos : dot - i);
        if (part.empty()) { break; }
        int value = 0;
        bool ok = true;
        for (char c : part) {
            if (c < '0' || c > '9') { ok = false; break; }
            value = value * 10 + (c - '0');
            if (value > 1000000) { ok = false; break; }
        }
        if (!ok) { break; }  // junk: stop rather than invent an ordering
        out.push_back(value);
        if (dot == std::string::npos) { break; }
        i = dot + 1;
    }
    return out;
}

}  // namespace

int compareVersions(const std::string& a, const std::string& b) {
    std::string aPre, bPre;
    const std::vector<int> an = numericParts(a, aPre);
    const std::vector<int> bn = numericParts(b, bPre);

    for (std::size_t i = 0; i < an.size() || i < bn.size(); ++i) {
        const int x = (i < an.size()) ? an[i] : 0;  // missing parts are zero
        const int y = (i < bn.size()) ? bn[i] : 0;
        if (x != y) { return x < y ? -1 : 1; }
    }
    if (aPre.empty() && bPre.empty()) { return 0; }
    // A release beats a pre-release of the same number.
    if (aPre.empty()) { return 1; }
    if (bPre.empty()) { return -1; }
    if (aPre < bPre) { return -1; }
    if (aPre > bPre) { return 1; }
    return 0;
}

bool parseUpdateManifest(const std::string& text, const std::string& currentVersion,
                         UpdateInfo& out, std::string& error) {
    out = UpdateInfo{};
    error.clear();

    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        error = "the update service returned something that is not JSON";
        return false;
    }

    out.version = j.value("version", std::string());
    if (out.version.empty()) {
        error = "the update service named no version";
        return false;
    }
    out.url = j.value("url", std::string());
    out.sha256 = j.value("sha256", std::string());

    // THE THREE FIELDS THAT MAKE A DOWNLOAD SAFE. Each is refused rather than
    // warned about, because a manifest missing any of them cannot be acted on
    // and pretending otherwise is how an updater becomes an exploit.
    if (!wellFormedSha256(out.sha256)) {
        error = "the update service gave no usable checksum for " + out.version;
        return false;
    }
    if (!urlIsOnAllowedHost(out.url)) {
        error = "refusing an update URL that is not on " + std::string(kAllowedHost) + ": \"" +
                out.url + "\"";
        return false;
    }

    if (j.contains("size") && j["size"].is_number_unsigned()) {
        out.sizeBytes = j["size"].get<std::uint64_t>();
    }

    // Recomputed here rather than believed: the server says "newer", but this
    // build knows its own version, and a server that got it wrong (or a stale
    // cache) must not be able to talk anyone into a downgrade.
    out.newer = compareVersions(out.version, currentVersion) > 0;

    if (j.contains("notes") && j["notes"].is_array()) {
        for (const json& n : j["notes"]) {
            if (!n.is_object()) { continue; }
            ReleaseNote note;
            note.version = n.value("version", std::string());
            note.date = n.value("date", std::string());
            note.critical = n.value("critical", false);
            if (n.contains("notes") && n["notes"].is_array()) {
                for (const json& line : n["notes"]) {
                    if (line.is_string()) { note.notes.push_back(line.get<std::string>()); }
                }
            }
            if (!note.version.empty() && !note.notes.empty()) {
                out.notes.push_back(std::move(note));
            }
        }
    }

    // Also recomputed, for the same reason.
    out.critical = false;
    if (out.newer) {
        for (const ReleaseNote& n : out.notes) {
            if (n.critical) { out.critical = true; }
        }
    }
    return true;
}

std::string updateEndpoint() {
#if defined(_WIN32)
    char buf[512] = {0};
    const DWORD n = ::GetEnvironmentVariableA("FOXSDR_UPDATE_URL", buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) { return std::string(buf, n); }
#else
    const char* v = std::getenv("FOXSDR_UPDATE_URL");
    if (v != nullptr && *v != '\0') { return std::string(v); }
#endif
    return "https://foxsdr.com/api/update";
}

bool checkForUpdate(const std::string& baseUrl, const std::string& currentVersion,
                    const std::string& channel, UpdateInfo& out, std::string& error) {
    out = UpdateInfo{};
    error.clear();
    if (baseUrl.empty()) {
        error = "no update endpoint configured";
        return false;
    }
    // The running version is the only thing sent. It is a query parameter so
    // the server can say what changed since; nothing else identifies anyone.
    std::string url = baseUrl + "?v=" + currentVersion;
    if (!channel.empty()) { url += "&channel=" + channel; }

    std::string body;
    if (!PluginRepo::fetchText(url, kMaxManifestBytes, body, error)) {
        return false;
    }
    return parseUpdateManifest(body, currentVersion, out, error);
}

bool downloadUpdate(const UpdateInfo& info, std::string& outPath, std::string& error) {
    outPath.clear();
    error.clear();
    if (!info.newer || info.url.empty()) {
        error = "there is no update to download";
        return false;
    }
    // Re-checked here as well as at parse time: downloadUpdate is public, and
    // a caller that built an UpdateInfo by hand must not be able to skip the
    // host rule.
    if (!urlIsOnAllowedHost(info.url)) {
        error = "refusing an update URL that is not on " + std::string(kAllowedHost);
        return false;
    }

    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec);
    if (ec) {
        error = "no temporary directory available";
        return false;
    }
    dir /= "foxsdr-update";
    fs::create_directories(dir, ec);

    // Named after the version so a second attempt overwrites rather than
    // accumulating installers in the user's temp directory.
    const fs::path dest = dir / ("foxsdr-setup-" + info.version + ".exe");

    if (!PluginRepo::fetchVerifiedFile(info.url, info.sha256, dest.string(), kMaxInstallerBytes,
                                       error)) {
        return false;
    }
    outPath = dest.string();
    return true;
}

}  // namespace cascade::core
