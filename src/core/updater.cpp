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

bool wellFormedVersion(const std::string& v) {
    // A STRUCTURAL PARSE, not a charset filter. A filter that only banned odd
    // characters would still accept "..", and ".." is the whole attack: this
    // string names the downloaded installer and that file is then executed.
    if (v.empty() || v.size() > 64) { return false; }

    // Dotted numeric core: at least one part, each 1 to 9 digits and nothing
    // else. An empty part is what "1..2" and a leading dot look like.
    std::size_t i = 0;
    int parts = 0;
    while (true) {
        std::size_t digits = 0;
        while (i < v.size() && v[i] >= '0' && v[i] <= '9') { ++i; ++digits; }
        if (digits == 0 || digits > 9) { return false; }
        if (++parts > 4) { return false; }
        if (i < v.size() && v[i] == '.') { ++i; continue; }
        break;
    }
    if (i == v.size()) { return true; }  // a plain release: 0.58.0

    // Optional pre-release suffix: one hyphen, then dot-separated segments of
    // letters and digits - 0.57.0-nightly.20260819.b97092e. No segment may be
    // empty, which is what keeps ".." out, and nothing outside [0-9A-Za-z.]
    // gets in at all, which is what keeps separators, colons and spaces out.
    if (v[i] != '-') { return false; }
    ++i;
    std::size_t segment = 0;
    for (; i < v.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(v[i]);
        if (c == '.') {
            if (segment == 0) { return false; }
            segment = 0;
            continue;
        }
        const bool alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                           (c >= 'A' && c <= 'Z');
        if (!alnum) { return false; }
        ++segment;
    }
    return segment != 0;
}

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
    if (!wellFormedVersion(out.version)) {
        error = "refusing an update version that does not parse: \"" + out.version + "\"";
        return false;
    }
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

bool downloadUpdate(const UpdateInfo& info, std::string& outPath, std::string& error,
                    std::atomic<float>* progress, std::atomic<bool>* cancel) {
    outPath.clear();
    error.clear();
    // Reset before anything can read it: a second attempt after a failed one
    // would otherwise start its bar wherever the first one stopped.
    if (progress != nullptr) {
        progress->store(0.0f, std::memory_order_relaxed);
    }
    if (!info.newer || info.url.empty()) {
        error = "there is no update to download";
        return false;
    }
    // Re-checked here as well as at parse time: downloadUpdate is public, and
    // a caller that built an UpdateInfo by hand must not be able to skip the
    // host rule - or the version rule, which matters more here, because the
    // version below becomes a path and that path is later executed.
    if (!wellFormedVersion(info.version)) {
        error = "refusing an update version that does not parse: \"" + info.version + "\"";
        return false;
    }
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
                                       error, progress, cancel)) {
        return false;
    }
    // The bar sat at whatever the last chunk made it; the file is verified and
    // named, so say so rather than stopping at 99%.
    if (progress != nullptr) {
        progress->store(1.0f, std::memory_order_relaxed);
    }
    outPath = dest.string();
    return true;
}

}  // namespace cascade::core
