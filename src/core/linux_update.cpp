// Implementation of core/linux_update.hpp. Pure logic plus calls on the UpdateHost interface; it
// touches no file, process or socket itself, so all of it runs (and is tested) on Windows.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/linux_update.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>

#include "core/updater.hpp"

namespace cascade::core::linuxupdate {

using json = nlohmann::json;

namespace {

// The only host an update may come from: the one the Windows updater already allows.
constexpr char kAllowedHost[] = "foxsdr.com";
constexpr int kSmokeTimeoutMs = 20000;

std::string strField(const json& j, const char* key) {
    const auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

bool urlOk(const std::string& url) {
    const std::string prefix = "https://";
    if (url.rfind(prefix, 0) != 0) { return false; }
    const std::size_t slash = url.find('/', prefix.size());
    const std::string host =
        url.substr(prefix.size(), slash == std::string::npos ? std::string::npos
                                                             : slash - prefix.size());
    if (host == kAllowedHost) { return true; }
    const std::string dotted = std::string(".") + kAllowedHost;
    return host.size() > dotted.size() &&
           host.compare(host.size() - dotted.size(), dotted.size(), dotted) == 0;
}

bool hex64(const std::string& s) {
    if (s.size() != 64) { return false; }
    return std::all_of(s.begin(), s.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
}

std::string lower(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

std::string parentOf(const std::string& path) {
    const std::size_t s = path.find_last_of('/');
    return s == std::string::npos ? std::string(".") : (s == 0 ? std::string("/") : path.substr(0, s));
}

// Takes the lock for the scope of applyUpdate and gives it back on every way out.
struct LockGuard {
    UpdateHost& host;
    std::string path;
    ~LockGuard() { host.removeAll(path); }
};

}  // namespace

std::string archName(Arch a) {
    switch (a) {
        case Arch::X86_64: return "x86_64";
        case Arch::Aarch64: return "aarch64";
        default: return "unknown";
    }
}
Arch parseArch(const std::string& s) {
    if (s == "x86_64" || s == "amd64" || s == "x64") { return Arch::X86_64; }
    if (s == "aarch64" || s == "arm64") { return Arch::Aarch64; }
    return Arch::Unknown;
}
std::string kindName(PackageKind k) {
    switch (k) {
        case PackageKind::AppImage: return "appimage";
        case PackageKind::Tarball: return "tarball";
        case PackageKind::Deb: return "deb";
        case PackageKind::Rpm: return "rpm";
        default: return "unknown";
    }
}
PackageKind parseKind(const std::string& s) {
    if (s == "appimage") { return PackageKind::AppImage; }
    if (s == "tarball") { return PackageKind::Tarball; }
    if (s == "deb") { return PackageKind::Deb; }
    if (s == "rpm") { return PackageKind::Rpm; }
    return PackageKind::Unknown;
}
std::string installKindName(InstallKind k) {
    switch (k) {
        case InstallKind::AppImage: return "appimage";
        case InstallKind::Tree: return "tree";
        case InstallKind::PackageManager: return "package-manager";
        case InstallKind::Flatpak: return "flatpak";
        case InstallKind::Snap: return "snap";
        case InstallKind::Container: return "container";
        default: return "unknown";
    }
}
std::string outcomeName(Outcome o) {
    switch (o) {
        case Outcome::Applied: return "applied";
        case Outcome::NothingToDo: return "nothing-to-do";
        case Outcome::Cancelled: return "cancelled";
        case Outcome::NoSpace: return "no-space";
        case Outcome::NotWritable: return "not-writable";
        case Outcome::Busy: return "busy";
        case Outcome::DownloadFailed: return "download-failed";
        case Outcome::HashMismatch: return "hash-mismatch";
        case Outcome::BadPayload: return "bad-payload";
        case Outcome::ExtractFailed: return "extract-failed";
        case Outcome::SwapFailed: return "swap-failed";
        case Outcome::SmokeFailed: return "smoke-failed";
    }
    return "unknown";
}

bool parseLinuxManifest(const std::string& text, Manifest& out, std::string& error) {
    out = Manifest{};
    error.clear();
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        error = "the update service returned something that is not JSON";
        return false;
    }
    const auto lin = j.find("linux");
    if (lin == j.end() || !lin->is_object()) {
        error = "the update service has no Linux build to offer";
        return false;
    }
    out.version = strField(*lin, "version");
    if (!wellFormedVersion(out.version)) {
        error = "refusing a Linux update version that does not parse";
        out.version.clear();
        return false;
    }
    out.channel = strField(*lin, "channel");
    if (out.channel.empty()) { out.channel = "stable"; }
    if (out.channel != "stable" && out.channel != "beta") {
        error = "the update answer names an unknown channel";
        return false;
    }
    for (const json* src : {&*lin, &j}) {
        const auto w = src->find("withdrawn");
        if (w == src->end() || !w->is_array()) { continue; }
        for (const json& v : *w) {
            if (out.withdrawn.size() >= kMaxWithdrawn) { break; }
            if (v.is_string() && wellFormedVersion(v.get<std::string>())) {
                out.withdrawn.push_back(v.get<std::string>());
            }
        }
    }
    const auto as = lin->find("assets");
    if (as != lin->end() && as->is_array()) {
        for (const json& a : *as) {
            if (out.assets.size() >= kMaxAssets) { break; }
            if (!a.is_object()) { continue; }
            Asset asset;
            asset.kind = parseKind(strField(a, "kind"));
            asset.arch = parseArch(strField(a, "arch"));
            asset.url = strField(a, "url");
            asset.sha256 = lower(strField(a, "sha256"));
            if (a.contains("size") && a["size"].is_number_unsigned()) {
                asset.sizeBytes = a["size"].get<std::uint64_t>();
            }
            // Each rule below makes the download safe; an asset that breaks one is dropped ALONE.
            if (asset.kind == PackageKind::Unknown || asset.arch == Arch::Unknown) { continue; }
            if (!hex64(asset.sha256) || !urlOk(asset.url)) { continue; }
            if (asset.sizeBytes > kMaxAssetBytes) { continue; }
            out.assets.push_back(std::move(asset));
        }
    }
    if (out.assets.empty()) {
        error = "the update answer carries no usable Linux file";
        return false;
    }
    return true;
}

std::string updateRequestUrl(const std::string& baseUrl, const std::string& runningVersion,
                             const std::string& channelSetting, Arch arch) {
    std::string url = baseUrl + "?v=" + runningVersion + "&platform=linux&arch=" + archName(arch);
    if (channelSetting == "beta") { url += "&channel=beta"; }
    return url;
}

InstallKind detectInstallKind(const InstallProbe& p) {
    if (p.snapEnv) { return InstallKind::Snap; }
    if (p.flatpakInfo) { return InstallKind::Flatpak; }
    if (!p.appImageEnv.empty() && p.appImageFileExists) { return InstallKind::AppImage; }
    if (p.containerEnv) { return InstallKind::Container; }
    // A system location is a package manager's even when we cannot prove which one owns it.
    if (p.ownedByPackage || p.underSystemPrefix) { return InstallKind::PackageManager; }
    if (p.exePath.empty()) { return InstallKind::Unknown; }
    return InstallKind::Tree;
}

Decision decide(const Config& cfg, const Manifest& m, const InstallProbe& probe) {
    Decision d;
    d.version = m.version;
    d.install = detectInstallKind(probe);
    const std::string wantChannel = cfg.channelSetting == "beta" ? "beta" : "stable";
    if (m.channel != wantChannel) {
        d.verdict = Verdict::ChannelMismatch;
        d.message = "The update answer is for the " + m.channel + " channel, not " + wantChannel + ".";
        return d;
    }
    const int cmp = compareVersions(m.version, cfg.runningVersion);
    if (cmp == 0) {
        d.verdict = Verdict::UpToDate;
        d.message = "FoxSDR " + cfg.runningVersion + " is the newest version.";
        return d;
    }
    if (cmp < 0) {
        d.verdict = Verdict::DowngradeRefused;
        d.message = "The service offered " + m.version + ", which is older than the " +
                    cfg.runningVersion + " that is running. Nothing was changed.";
        return d;
    }
    if (std::find(m.withdrawn.begin(), m.withdrawn.end(), m.version) != m.withdrawn.end()) {
        d.verdict = Verdict::Withdrawn;
        d.message = "Version " + m.version + " has been withdrawn and is not offered.";
        return d;
    }
    // A newer version exists. Whether this program may replace itself depends on how it got here.
    const std::string news = "FoxSDR " + m.version + " is available. ";
    d.verdict = Verdict::TellOnly;
    switch (d.install) {
        case InstallKind::PackageManager:
            d.message = news + "This copy was installed by your system's package manager, so FoxSDR will not "
                               "replace it; update it with the package manager.";
            return d;
        case InstallKind::Flatpak:
            d.message = news + "This is the Flatpak version; update it with Flatpak.";
            return d;
        case InstallKind::Snap:
            d.message = news + "This is the Snap version; Snap updates it.";
            return d;
        case InstallKind::Container:
            d.message = news + "This runs in a container; pull the new image instead.";
            return d;
        case InstallKind::Unknown:
            d.message = news + "FoxSDR cannot tell how it was installed, so it will not replace itself; "
                               "download the new version from foxsdr.com.";
            return d;
        default: break;
    }
    if (!probe.dirWritable) {
        d.message = news + "The folder FoxSDR is in is not writable by you, so it will not replace itself; "
                           "download the new version from foxsdr.com.";
        return d;
    }
    const PackageKind want = d.install == InstallKind::AppImage ? PackageKind::AppImage
                                                                : PackageKind::Tarball;
    for (const Asset& a : m.assets) {
        if (a.kind == want && a.arch == cfg.arch) {
            d.verdict = Verdict::Install;
            d.asset = a;
            d.message = news + "It can be installed now; the current copy is kept until the new one has started.";
            return d;
        }
    }
    d.verdict = Verdict::NoAsset;
    d.message = news + "There is no " + kindName(want) + " build for " + archName(cfg.arch) + " yet.";
    return d;
}

std::string partialPathFor(const std::string& target, const Asset& a) {
    return target + ".update-" + a.sha256.substr(0, 16) + ".part";
}

bool archiveListingIsSafe(const std::vector<std::string>& names, std::string& error) {
    error.clear();
    std::string top;
    bool any = false;
    for (std::string n : names) {
        if (n.rfind("./", 0) == 0) { n.erase(0, 2); }
        if (n.empty() || n == ".") { continue; }
        if (n[0] == '/') { error = "an archive entry has an absolute path"; return false; }
        if (n.find('\\') != std::string::npos || n.find('\0') != std::string::npos) {
            error = "an archive entry has a backslash or a NUL in its name";
            return false;
        }
        std::size_t i = 0;
        bool first = true;
        while (i <= n.size()) {
            const std::size_t slash = n.find('/', i);
            const std::string comp =
                n.substr(i, slash == std::string::npos ? std::string::npos : slash - i);
            if (comp == "..") { error = "an archive entry climbs out with \"..\""; return false; }
            if (first) {
                if (comp.empty()) { error = "an archive entry has an empty name"; return false; }
                if (!any) { top = comp; any = true; }
                else if (comp != top) { error = "the archive has more than one top-level entry"; return false; }
                first = false;
            }
            if (slash == std::string::npos) { break; }
            i = slash + 1;
        }
    }
    if (!any) { error = "the archive is empty"; return false; }
    return true;
}

namespace {

ApplyResult fail(Outcome o, std::string detail, bool restored = false) {
    ApplyResult r;
    r.outcome = o;
    r.detail = std::move(detail);
    r.restored = restored;
    return r;
}

// Does the freshly placed copy run? An AppImage is also tried with the runtime's own
// extract-and-run flag, because a machine without FUSE cannot mount it (UNVERIFIED on Linux).
bool smokeRuns(UpdateHost& host, const Decision& d, const std::string& target) {
    std::string out;
    if (d.install == InstallKind::AppImage) {
        if (host.runProgram(target, {"--version"}, kSmokeTimeoutMs, out) == 0 &&
            out.find(d.version) != std::string::npos) {
            return true;
        }
        out.clear();
        return host.runProgram(target, {"--appimage-extract-and-run", "--version"},
                               kSmokeTimeoutMs, out) == 0 &&
               out.find(d.version) != std::string::npos;
    }
    return host.runProgram(target + "/cascade", {"--version"}, kSmokeTimeoutMs, out) == 0 &&
           out.find(d.version) != std::string::npos;
}

}  // namespace

ApplyResult applyUpdate(UpdateHost& host, const Decision& d, const ApplyPaths& paths,
                        std::atomic<bool>* cancel) {
    if (d.verdict != Verdict::Install ||
        (d.install != InstallKind::AppImage && d.install != InstallKind::Tree)) {
        return fail(Outcome::NothingToDo, "the decision is not an install");
    }
    if (!wellFormedVersion(d.version) || d.version != paths.newVersion || !hex64(d.asset.sha256) ||
        !urlOk(d.asset.url)) {
        // applyUpdate is public: a hand-built Decision must not skip the rules the manifest parse applies.
        return fail(Outcome::NothingToDo, "the decision does not pass the download rules");
    }
    const std::string& target = paths.target;
    const std::string dir = parentOf(target);
    if (!host.writableDir(dir)) { return fail(Outcome::NotWritable, "the folder is not writable"); }
    const std::uint64_t perCopy = d.asset.sizeBytes != 0 ? d.asset.sizeBytes : (256ull << 20);
    const std::uint64_t need = perCopy * (d.install == InstallKind::Tree ? 3 : 2);
    if (host.freeSpace(dir) < need) { return fail(Outcome::NoSpace, "not enough free space beside the program"); }

    if (!host.createExclusive(target + kLockSuffix)) {
        return fail(Outcome::Busy, "another update is in progress (or one was interrupted)");
    }
    LockGuard lock{host, target + kLockSuffix};

    const std::string partial = partialPathFor(target, d.asset);
    const std::uint64_t maxBytes = d.asset.sizeBytes != 0 ? d.asset.sizeBytes : kMaxAssetBytes;

    // --- download, resuming when that is sound ------------------------------------------------
    std::uint64_t resumeFrom = 0;
    {
        const std::int64_t have = host.fileSize(partial);
        if (have > 0) {
            if (d.asset.sizeBytes != 0 && static_cast<std::uint64_t>(have) > d.asset.sizeBytes) {
                host.removeAll(partial);  // larger than the asset can be: discard, never resume
            } else {
                resumeFrom = static_cast<std::uint64_t>(have);
            }
        }
    }
    bool verified = false;
    for (int attempt = 0; attempt < 2 && !verified; ++attempt) {
        const bool complete = d.asset.sizeBytes != 0 && resumeFrom == d.asset.sizeBytes;
        if (!complete) {
            const FetchResult fr = host.fetch(d.asset.url, partial, resumeFrom, maxBytes, cancel);
            if (fr.status == FetchResult::Status::Cancelled) { return fail(Outcome::Cancelled, "cancelled"); }
            if (fr.status != FetchResult::Status::Ok) { return fail(Outcome::DownloadFailed, fr.error); }
        }
        std::string got;
        if (!host.sha256(partial, got)) { return fail(Outcome::DownloadFailed, "the download could not be read back"); }
        if (got != d.asset.sha256) {
            host.removeAll(partial);  // never keep bytes that failed their digest
            if (resumeFrom > 0 && attempt == 0) {
                resumeFrom = 0;  // a resumed partial may have been the bad part: one clean retry
                continue;
            }
            return fail(Outcome::HashMismatch, "the download does not match the published checksum");
        }
        verified = true;
    }
    if (!verified) { return fail(Outcome::HashMismatch, "the download does not match the published checksum"); }

    // --- payload ------------------------------------------------------------------------------
    const std::string staged = target + ".new";
    std::string stagedRoot = staged;
    host.removeAll(staged);
    if (d.install == InstallKind::AppImage) {
        std::string head;
        if (!host.readHead(partial, 4, head) || head != std::string("\x7f" "ELF", 4)) {
            host.removeAll(partial);
            return fail(Outcome::BadPayload, "the file is not an executable program");
        }
        if (!host.makeExecutable(partial) || !host.rename(partial, staged)) {
            host.removeAll(partial);
            return fail(Outcome::SwapFailed, "the new file could not be put in place");
        }
    } else {
        std::vector<std::string> names;
        std::string why;
        if (!host.listArchive(partial, names)) {
            host.removeAll(partial);
            return fail(Outcome::ExtractFailed, "the archive could not be read");
        }
        if (!archiveListingIsSafe(names, why)) {
            host.removeAll(partial);
            return fail(Outcome::BadPayload, why);
        }
        std::string top = names.front();
        if (top.rfind("./", 0) == 0) { top.erase(0, 2); }
        for (const std::string& n : names) {  // the first non-empty, non-"." name's top component
            std::string x = n.rfind("./", 0) == 0 ? n.substr(2) : n;
            if (!x.empty() && x != ".") { top = x; break; }
        }
        top = top.substr(0, top.find('/'));
        if (!host.makeDir(staged) || !host.extractArchive(partial, staged)) {
            host.removeAll(staged);
            host.removeAll(partial);
            return fail(Outcome::ExtractFailed, "the archive could not be unpacked");
        }
        host.removeAll(partial);
        stagedRoot = staged + "/" + top;
        if (!host.exists(stagedRoot)) {
            host.removeAll(staged);
            return fail(Outcome::ExtractFailed, "the unpacked archive is not what its listing said");
        }
    }

    // --- swap, keeping the old copy -----------------------------------------------------------
    const std::string old = target + kOldSuffix;
    const std::string marker = target + kPendingSuffix;
    host.removeAll(old);  // a leftover from an earlier update is not worth keeping
    if (!host.writeText(marker, d.version)) {
        host.removeAll(staged);
        return fail(Outcome::SwapFailed, "the update marker could not be written");
    }
    if (!host.rename(target, old)) {
        host.removeAll(marker);
        host.removeAll(staged);
        return fail(Outcome::SwapFailed, "the current copy could not be moved aside");
    }
    if (!host.rename(stagedRoot, target)) {
        const bool back = host.rename(old, target);
        host.removeAll(marker);
        host.removeAll(staged);
        return fail(Outcome::SwapFailed, "the new copy could not be moved into place", back);
    }
    host.removeAll(staged);  // an empty staging directory for a tarball; absent for an AppImage

    // --- the new copy must run once; otherwise the old one goes back ---------------------------
    if (!smokeRuns(host, d, target)) {
        host.removeAll(target);
        const bool back = host.rename(old, target);
        host.removeAll(marker);
        return fail(Outcome::SmokeFailed, "the new copy did not start; the previous copy was put back", back);
    }
    ApplyResult ok;
    ok.outcome = Outcome::Applied;
    ok.detail = "installed " + d.version + "; the previous copy is kept until the new one has started";
    return ok;
}

std::string confirmStartup(UpdateHost& host, const std::string& target,
                           const std::string& runningVersion) {
    const std::string old = target + kOldSuffix;
    const std::string marker = target + kPendingSuffix;
    if (!host.exists(target) && host.exists(old)) {
        // An interrupted swap: the copy that was running before is still there.
        if (host.rename(old, target)) {
            host.removeAll(marker);
            return "an interrupted update was undone; the previous copy was put back";
        }
        return "";
    }
    std::string pending;
    if (!host.exists(marker) || !host.readText(marker, pending)) { return ""; }
    while (!pending.empty() && std::isspace(static_cast<unsigned char>(pending.back()))) { pending.pop_back(); }
    if (pending == runningVersion) {
        host.removeAll(old);
        host.removeAll(marker);
        return "the update to " + runningVersion + " is confirmed; the previous copy was removed";
    }
    host.removeAll(marker);  // stale: keep the old copy for the user, stop watching
    return "";
}

std::string confirmStartupFor(UpdateHost& host, InstallKind kind, const std::string& target,
                              const std::string& runningVersion) {
    if (kind != InstallKind::AppImage && kind != InstallKind::Tree) { return ""; }
    if (target.empty()) { return ""; }
    return confirmStartup(host, target, runningVersion);
}

}  // namespace cascade::core::linuxupdate
