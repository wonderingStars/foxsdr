// Tests for core/linux_update.{hpp,cpp}, core/linux_update_host.{hpp,cpp} and
// core/linux_update_fetch.hpp: the Linux in-app updater's decisions and its file replacement.
//
// Three kinds of host stand in for a Linux machine:
//   FakeHost   an in-memory file system and process, with a knob for every way a step can fail;
//   RealFsHost the real FsUpdateHost on a temp directory (real rename, real files) with a scripted
//              fetch and program run - the swap and the rollback happen on actual files;
//   LoopHost   the real FsUpdateHost plus rangeFetch against a stand-in HTTP server on 127.0.0.1,
//              so resume / restart / refusal are proven against bytes on a socket.
// What none of them can prove is the Linux system host (SystemHost): see the record.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/linux_update.hpp"

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "core/linux_update_fetch.hpp"
#include "core/linux_update_host.hpp"
#include "core/plugin_repo.hpp"
#include "test_check.hpp"

using namespace cascade::core;
using namespace cascade::core::linuxupdate;
namespace fs = std::filesystem;

namespace {

std::string sha(const std::string& data) {
    std::string hex, err;
    PluginRepo::sha256Hex(data.data(), data.size(), hex, err);
    return hex;
}

const std::string kElf = std::string("\x7f" "ELF", 4) + "new-program-bytes-0123456789";
constexpr const char* kUrl = "https://foxsdr.com/download/FoxSDR-0.99.110-x86_64.AppImage";

Asset appImageAsset(const std::string& body = kElf) {
    Asset a;
    a.kind = PackageKind::AppImage;
    a.arch = Arch::X86_64;
    a.url = kUrl;
    a.sha256 = sha(body);
    a.sizeBytes = body.size();
    return a;
}

Decision installDecision(const Asset& a, InstallKind k = InstallKind::AppImage,
                         const std::string& ver = "0.99.110") {
    Decision d;
    d.verdict = Verdict::Install;
    d.install = k;
    d.asset = a;
    d.version = ver;
    return d;
}

// ---------------------------------------------------------------------------------------------
// FakeHost
// ---------------------------------------------------------------------------------------------
class FakeHost : public UpdateHost {
public:
    std::map<std::string, std::string> files;
    std::set<std::string> dirs;
    std::map<std::string, std::string> served;       // url -> bytes
    std::map<std::string, std::vector<std::string>> archives;  // archive path -> listing
    std::vector<std::uint64_t> fetchFrom;            // resumeFrom of every fetch
    std::vector<std::string> renames;
    std::vector<std::string> runs;                   // "program arg arg"
    std::set<std::string> failRenameTo;
    std::function<int(const std::string&, const std::vector<std::string>&, std::string&)> run;
    bool rangeOk = true;          // false: the server ignores Range and sends it all
    bool corruptAppended = false; // the resumed bytes are wrong
    bool fetchFails = false;
    bool dirWritable = true;
    bool extractFails = false;
    bool listFails = false;
    std::uint64_t space = 1ull << 40;
    std::vector<std::string> executable;
    std::string lastTarballTop = "foxsdr-0.99.110-linux-x64";

    bool exists(const std::string& p) override { return files.count(p) != 0 || dirs.count(p) != 0 || hasChild(p); }
    bool hasChild(const std::string& p) const {
        const std::string pre = p + "/";
        for (const auto& f : files) { if (f.first.rfind(pre, 0) == 0) { return true; } }
        return false;
    }
    std::int64_t fileSize(const std::string& p) override {
        auto it = files.find(p);
        return it == files.end() ? -1 : static_cast<std::int64_t>(it->second.size());
    }
    bool makeDir(const std::string& p) override { dirs.insert(p); return true; }
    bool writableDir(const std::string&) override { return dirWritable; }
    std::uint64_t freeSpace(const std::string&) override { return space; }
    bool createExclusive(const std::string& p) override {
        if (files.count(p)) { return false; }
        files[p] = "";
        return true;
    }
    bool removeAll(const std::string& p) override {
        files.erase(p);
        dirs.erase(p);
        const std::string pre = p + "/";
        for (auto it = files.begin(); it != files.end();) {
            it = it->first.rfind(pre, 0) == 0 ? files.erase(it) : std::next(it);
        }
        for (auto it = dirs.begin(); it != dirs.end();) {
            it = it->rfind(pre, 0) == 0 ? dirs.erase(it) : std::next(it);
        }
        return true;
    }
    bool rename(const std::string& from, const std::string& to) override {
        renames.push_back(from + " -> " + to);
        if (failRenameTo.count(to)) { return false; }
        if (!exists(from)) { return false; }
        std::map<std::string, std::string> moved;
        auto f = files.find(from);
        if (f != files.end()) { moved[to] = f->second; files.erase(f); }
        const std::string pre = from + "/";
        for (auto it = files.begin(); it != files.end();) {
            if (it->first.rfind(pre, 0) == 0) { moved[to + "/" + it->first.substr(pre.size())] = it->second; it = files.erase(it); }
            else { ++it; }
        }
        if (dirs.erase(from)) { dirs.insert(to); }
        for (auto& m : moved) { files[m.first] = m.second; }
        return true;
    }
    bool writeText(const std::string& p, const std::string& t) override { files[p] = t; return true; }
    bool readText(const std::string& p, std::string& t) override {
        auto it = files.find(p);
        if (it == files.end()) { return false; }
        t = it->second;
        return true;
    }
    bool readHead(const std::string& p, std::size_t n, std::string& b) override {
        auto it = files.find(p);
        if (it == files.end()) { return false; }
        b = it->second.substr(0, n);
        return true;
    }
    bool sha256(const std::string& p, std::string& h) override {
        auto it = files.find(p);
        if (it == files.end()) { return false; }
        h = sha(it->second);
        return true;
    }
    bool makeExecutable(const std::string& p) override { executable.push_back(p); return files.count(p) != 0; }
    FetchResult fetch(const std::string& url, const std::string& partial, std::uint64_t from,
                      std::uint64_t maxBytes, std::atomic<bool>* cancel) override {
        FetchResult fr;
        fetchFrom.push_back(from);
        if (cancel != nullptr && cancel->load()) { fr.status = FetchResult::Status::Cancelled; return fr; }
        if (fetchFails || !served.count(url)) { fr.error = "network down"; return fr; }
        const std::string& body = served[url];
        if (body.size() > maxBytes) { fr.error = "too big"; return fr; }
        if (from > 0 && rangeOk) {
            std::string tail = body.substr(from);
            if (corruptAppended) { for (char& c : tail) { c = static_cast<char>(c ^ 0x55); } }
            files[partial] += tail;
            fr.resumed = true;
        } else {
            files[partial] = body;
            fr.resumed = false;
        }
        fr.totalBytes = files[partial].size();
        fr.status = FetchResult::Status::Ok;
        return fr;
    }
    bool listArchive(const std::string& a, std::vector<std::string>& names) override {
        if (listFails || !archives.count(a)) { return false; }
        names = archives[a];
        return true;
    }
    bool extractArchive(const std::string& a, const std::string& dest) override {
        if (extractFails) { return false; }
        for (const std::string& n : archives[a]) {
            if (!n.empty() && n.back() != '/') { files[dest + "/" + n] = "x"; }
        }
        return true;
    }
    int runProgram(const std::string& prog, const std::vector<std::string>& args, int, std::string& out) override {
        std::string line = prog;
        for (const auto& a : args) { line += " " + a; }
        runs.push_back(line);
        out.clear();
        if (run) { return run(prog, args, out); }
        out = "FoxSDR 0.99.110\n";
        return 0;
    }
};

FakeHost appImageWorld() {
    FakeHost h;
    h.files["/home/u/FoxSDR.AppImage"] = "OLD-COPY";
    h.served[kUrl] = kElf;
    return h;
}
const ApplyPaths kAppPaths{"/home/u/FoxSDR.AppImage", "0.99.110"};

// ---------------------------------------------------------------------------------------------
// manifest
// ---------------------------------------------------------------------------------------------
std::string manifestJson(const std::string& version = "0.99.110", const std::string& url = kUrl,
                         const std::string& shaHex = sha(kElf), const std::string& extra = "") {
    return "{\"version\":\"0.99.109\",\"linux\":{\"version\":\"" + version + "\"" + extra +
           ",\"assets\":[{\"kind\":\"appimage\",\"arch\":\"x86_64\",\"url\":\"" + url +
           "\",\"sha256\":\"" + shaHex + "\",\"size\":" + std::to_string(kElf.size()) + "}]}}";
}

void testManifest() {
    Manifest m;
    std::string err;
    CHECK(parseLinuxManifest(manifestJson(), m, err));
    CHECK(m.version == "0.99.110" && m.channel == "stable" && m.assets.size() == 1);
    CHECK(m.assets[0].kind == PackageKind::AppImage && m.assets[0].arch == Arch::X86_64);
    CHECK(m.assets[0].sizeBytes == kElf.size());
    CHECK(!parseLinuxManifest("not json", m, err) && !err.empty());
    CHECK(!parseLinuxManifest("[]", m, err));
    CHECK(!parseLinuxManifest("{\"version\":\"1.0.0\"}", m, err));  // no linux object
    CHECK(!parseLinuxManifest("{\"linux\":5}", m, err));
    // The version names a path later on: a structural parse, not a charset filter.
    CHECK(!parseLinuxManifest(manifestJson(".."), m, err));
    CHECK(!parseLinuxManifest(manifestJson("1..2"), m, err));
    CHECK(!parseLinuxManifest(manifestJson("../../etc"), m, err));
    CHECK(!parseLinuxManifest("{\"linux\":{\"version\":5,\"assets\":[]}}", m, err));  // a number is a refusal, not a throw
    // The three fields that make a download safe.
    CHECK(!parseLinuxManifest(manifestJson("0.99.110", kUrl, "abc"), m, err));
    CHECK(!parseLinuxManifest(manifestJson("0.99.110", kUrl, std::string(64, 'g')), m, err));
    CHECK(!parseLinuxManifest(manifestJson("0.99.110", "http://foxsdr.com/a.AppImage"), m, err));
    CHECK(!parseLinuxManifest(manifestJson("0.99.110", "https://evil.example/a.AppImage"), m, err));
    CHECK(!parseLinuxManifest(manifestJson("0.99.110", "https://evilfoxsdr.com/a.AppImage"), m, err));
    CHECK(!parseLinuxManifest(manifestJson("0.99.110", "https://foxsdr.com.evil.example/a"), m, err));
    CHECK(parseLinuxManifest(manifestJson("0.99.110", "https://dl.foxsdr.com/a.AppImage"), m, err));
    // Upper-case hex is accepted and stored lower-case: the comparison is against lower-case.
    std::string up = sha(kElf);
    for (char& c : up) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }
    CHECK(parseLinuxManifest(manifestJson("0.99.110", kUrl, up), m, err) && m.assets[0].sha256 == sha(kElf));
    // Channel.
    CHECK(parseLinuxManifest(manifestJson("0.99.110", kUrl, sha(kElf), ",\"channel\":\"beta\""), m, err) && m.channel == "beta");
    CHECK(!parseLinuxManifest(manifestJson("0.99.110", kUrl, sha(kElf), ",\"channel\":\"nightly\""), m, err));
    // An asset that fails is dropped ALONE.
    const std::string good = "{\"kind\":\"appimage\",\"arch\":\"aarch64\",\"url\":\"" + std::string(kUrl) + "\",\"sha256\":\"" + sha(kElf) + "\"}";
    const std::string badUrl = "{\"kind\":\"appimage\",\"arch\":\"x86_64\",\"url\":\"http://foxsdr.com/x\",\"sha256\":\"" + sha(kElf) + "\"}";
    const std::string badKind = "{\"kind\":\"exe\",\"arch\":\"x86_64\",\"url\":\"" + std::string(kUrl) + "\",\"sha256\":\"" + sha(kElf) + "\"}";
    const std::string huge = "{\"kind\":\"tarball\",\"arch\":\"x86_64\",\"url\":\"" + std::string(kUrl) + "\",\"sha256\":\"" + sha(kElf) + "\",\"size\":99999999999}";
    CHECK(parseLinuxManifest("{\"linux\":{\"version\":\"1.2.3\",\"assets\":[" + badUrl + "," + badKind + "," + huge + ",7," + good + "]}}", m, err));
    CHECK(m.assets.size() == 1 && m.assets[0].arch == Arch::Aarch64);
    CHECK(!parseLinuxManifest("{\"linux\":{\"version\":\"1.2.3\",\"assets\":[" + badUrl + "]}}", m, err));  // nothing usable
    // The asset list is capped.
    std::string many;
    for (int i = 0; i < 40; ++i) { many += (i ? "," : "") + good; }
    CHECK(parseLinuxManifest("{\"linux\":{\"version\":\"1.2.3\",\"assets\":[" + many + "]}}", m, err) && m.assets.size() == kMaxAssets);
    // Withdrawn list: well-formed entries only, from either place.
    CHECK(parseLinuxManifest("{\"withdrawn\":[\"0.99.108\",\"..\"],\"linux\":{\"version\":\"1.2.3\",\"withdrawn\":[\"1.2.3\"],\"assets\":[" + good + "]}}", m, err));
    CHECK(m.withdrawn.size() == 2);
    // Request URL.
    CHECK(updateRequestUrl("https://foxsdr.com/api/update", "0.99.109", "beta", Arch::Aarch64) ==
          "https://foxsdr.com/api/update?v=0.99.109&platform=linux&arch=aarch64&channel=beta");
    CHECK(updateRequestUrl("https://foxsdr.com/api/update", "0.99.109", "stable", Arch::X86_64) ==
          "https://foxsdr.com/api/update?v=0.99.109&platform=linux&arch=x86_64");
    CHECK(parseArch("amd64") == Arch::X86_64 && parseArch("arm64") == Arch::Aarch64 && parseArch("riscv") == Arch::Unknown);
}

// A deterministic mutation fuzz: whatever bytes arrive, the parsers return and never throw.
void testFuzz() {
    const std::string seed = manifestJson();
    unsigned long long x = 0x9E3779B97F4A7C15ull;
    auto next = [&x]() { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; };
    int parsed = 0;
    for (int i = 0; i < 4000; ++i) {
        std::string s = seed;
        const int edits = 1 + static_cast<int>(next() % 4);
        for (int e = 0; e < edits; ++e) {
            const std::size_t pos = static_cast<std::size_t>(next() % s.size());
            switch (next() % 4) {
                case 0: s[pos] = static_cast<char>(next() & 0xff); break;
                case 1: s.erase(pos, 1 + next() % 8); break;
                case 2: s.insert(pos, 1, "{}[]\":,\\"[next() % 9 > 7 ? 7 : next() % 8]); break;
                default: s.resize(pos); break;
            }
            if (s.empty()) { s = "{"; }
        }
        Manifest m;
        std::string err;
        try { if (parseLinuxManifest(s, m, err)) { ++parsed; } } catch (...) { CHECK(false); }
        std::vector<std::string> names{s, s.substr(0, s.size() / 2)};
        try { (void)archiveListingIsSafe(names, err); } catch (...) { CHECK(false); }
    }
    CHECK(parsed > 0);  // the fuzz does reach the accepting path
}

// ---------------------------------------------------------------------------------------------
// install kind and the decision
// ---------------------------------------------------------------------------------------------
InstallProbe appImageProbe() {
    InstallProbe p;
    p.exePath = "/tmp/.mount_x/usr/bin/cascade";
    p.appImageEnv = "/home/u/FoxSDR.AppImage";
    p.appImageFileExists = true;
    p.dirWritable = true;
    return p;
}

void testInstallKind() {
    CHECK(detectInstallKind(appImageProbe()) == InstallKind::AppImage);
    InstallProbe p = appImageProbe();
    p.appImageFileExists = false;  // $APPIMAGE names a file that is not there: do not trust it
    p.underSystemPrefix = false;
    CHECK(detectInstallKind(p) == InstallKind::Tree);
    p = InstallProbe{};
    p.exePath = "/home/u/foxsdr/cascade";
    CHECK(detectInstallKind(p) == InstallKind::Tree);
    p.ownedByPackage = true;
    CHECK(detectInstallKind(p) == InstallKind::PackageManager);
    p = InstallProbe{};
    p.exePath = "/usr/bin/cascade";
    p.underSystemPrefix = true;  // no package proved, but a system location is a package manager's
    CHECK(detectInstallKind(p) == InstallKind::PackageManager);
    p = InstallProbe{};
    p.exePath = "/app/bin/cascade";
    p.flatpakInfo = true;
    CHECK(detectInstallKind(p) == InstallKind::Flatpak);
    p.snapEnv = true;
    CHECK(detectInstallKind(p) == InstallKind::Snap);  // snap wins
    p = InstallProbe{};
    p.exePath = "/opt/foxsdr/cascade";
    p.containerEnv = true;
    CHECK(detectInstallKind(p) == InstallKind::Container);
    CHECK(detectInstallKind(InstallProbe{}) == InstallKind::Unknown);
}

Manifest manifestWith(const std::string& version, std::vector<Asset> assets, const std::string& channel = "stable") {
    Manifest m;
    m.version = version;
    m.channel = channel;
    m.assets = std::move(assets);
    return m;
}

void testDecide() {
    Config cfg{"0.99.109", "stable", Arch::X86_64};
    Asset ai = appImageAsset();
    Asset tar = ai;
    tar.kind = PackageKind::Tarball;
    const InstallProbe app = appImageProbe();

    Decision d = decide(cfg, manifestWith("0.99.110", {ai}), app);
    CHECK(d.verdict == Verdict::Install && d.install == InstallKind::AppImage && d.asset.url == ai.url);
    CHECK(decide(cfg, manifestWith("0.99.109", {ai}), app).verdict == Verdict::UpToDate);
    d = decide(cfg, manifestWith("0.99.108", {ai}), app);
    CHECK(d.verdict == Verdict::DowngradeRefused && d.message.find("older") != std::string::npos);
    // A pre-release of the running number is OLDER than the release: refused, not offered.
    CHECK(decide(Config{"0.99.110", "stable", Arch::X86_64}, manifestWith("0.99.110-beta.1", {ai}), app).verdict == Verdict::DowngradeRefused);
    CHECK(decide(Config{"0.10.0", "stable", Arch::X86_64}, manifestWith("0.9.0", {ai}), app).verdict == Verdict::DowngradeRefused);  // numeric, not text, order
    // Withdrawn.
    Manifest w = manifestWith("0.99.110", {ai});
    w.withdrawn = {"0.99.110"};
    CHECK(decide(cfg, w, app).verdict == Verdict::Withdrawn);
    // Channel.
    CHECK(decide(cfg, manifestWith("0.99.110", {ai}, "beta"), app).verdict == Verdict::ChannelMismatch);
    CHECK(decide(Config{"0.99.109", "beta", Arch::X86_64}, manifestWith("0.99.110", {ai}), app).verdict == Verdict::ChannelMismatch);
    CHECK(decide(Config{"0.99.109", "beta", Arch::X86_64}, manifestWith("0.99.110", {ai}, "beta"), app).verdict == Verdict::Install);
    CHECK(decide(Config{"0.99.109", "weird", Arch::X86_64}, manifestWith("0.99.110", {ai}), app).verdict == Verdict::Install);  // unknown setting = stable
    // Architecture and kind.
    CHECK(decide(Config{"0.99.109", "stable", Arch::Aarch64}, manifestWith("0.99.110", {ai}), app).verdict == Verdict::NoAsset);
    CHECK(decide(cfg, manifestWith("0.99.110", {tar}), app).verdict == Verdict::NoAsset);  // an AppImage install is not handed a tarball
    InstallProbe tree;
    tree.exePath = "/home/u/foxsdr/cascade";
    tree.dirWritable = true;
    d = decide(cfg, manifestWith("0.99.110", {ai, tar}), tree);
    CHECK(d.verdict == Verdict::Install && d.install == InstallKind::Tree && d.asset.kind == PackageKind::Tarball);
    CHECK(decide(cfg, manifestWith("0.99.110", {ai}), tree).verdict == Verdict::NoAsset);
    // Not writable: tell, never replace.
    InstallProbe ro = app;
    ro.dirWritable = false;
    d = decide(cfg, manifestWith("0.99.110", {ai}), ro);
    CHECK(d.verdict == Verdict::TellOnly && d.message.find("not writable") != std::string::npos);
    // Everything a package manager or a sandbox owns: tell, never replace - even with a matching asset.
    InstallProbe pm;
    pm.exePath = "/usr/bin/cascade";
    pm.ownedByPackage = true;
    pm.dirWritable = true;  // even a writable one (running as root)
    d = decide(cfg, manifestWith("0.99.110", {ai, tar}), pm);
    CHECK(d.verdict == Verdict::TellOnly && d.install == InstallKind::PackageManager && d.message.find("package manager") != std::string::npos);
    InstallProbe fp = tree; fp.flatpakInfo = true;
    InstallProbe sn = tree; sn.snapEnv = true;
    InstallProbe ct = tree; ct.containerEnv = true;
    InstallProbe un; un.dirWritable = true;
    CHECK(decide(cfg, manifestWith("0.99.110", {tar}), fp).verdict == Verdict::TellOnly);
    CHECK(decide(cfg, manifestWith("0.99.110", {tar}), sn).verdict == Verdict::TellOnly);
    CHECK(decide(cfg, manifestWith("0.99.110", {tar}), ct).verdict == Verdict::TellOnly);
    CHECK(decide(cfg, manifestWith("0.99.110", {tar}), un).verdict == Verdict::TellOnly);
    // The decision is never Install for a downgrade or the same version, whatever the install.
    CHECK(decide(cfg, manifestWith("0.99.109", {tar}), pm).verdict == Verdict::UpToDate);
}

// ---------------------------------------------------------------------------------------------
// applyUpdate against the fake
// ---------------------------------------------------------------------------------------------
void testApplyAppImage() {
    FakeHost h = appImageWorld();
    ApplyResult r = applyUpdate(h, installDecision(appImageAsset()), kAppPaths);
    CHECK(r.outcome == Outcome::Applied);
    CHECK(h.files["/home/u/FoxSDR.AppImage"] == kElf);                       // replaced
    CHECK(h.files["/home/u/FoxSDR.AppImage.old"] == "OLD-COPY");             // the old copy is kept
    CHECK(h.files["/home/u/FoxSDR.AppImage.update-pending"] == "0.99.110");  // and the marker says why
    CHECK(!h.exists("/home/u/FoxSDR.AppImage.update-lock"));                 // lock released
    CHECK(!h.exists("/home/u/FoxSDR.AppImage.new"));
    CHECK(h.executable.size() == 1);
    CHECK(h.runs.size() == 1 && h.runs[0] == "/home/u/FoxSDR.AppImage --version");
    // The old copy goes only when the new one has started for real.
    CHECK(!confirmStartup(h, kAppPaths.target, "0.99.110").empty());
    CHECK(!h.exists("/home/u/FoxSDR.AppImage.old") && !h.exists("/home/u/FoxSDR.AppImage.update-pending"));
    CHECK(h.files["/home/u/FoxSDR.AppImage"] == kElf);
}

void testApplyRefusals() {
    {  // BREAK-IT TARGET: bytes that do not hash to the published digest are refused and deleted.
        FakeHost h = appImageWorld();
        Asset a = appImageAsset();
        a.sha256 = sha("something else");
        ApplyResult r = applyUpdate(h, installDecision(a), kAppPaths);
        CHECK(r.outcome == Outcome::HashMismatch);
        CHECK(h.files["/home/u/FoxSDR.AppImage"] == "OLD-COPY");
        CHECK(!h.exists("/home/u/FoxSDR.AppImage.old") && !h.exists("/home/u/FoxSDR.AppImage.update-pending"));
        CHECK(h.fileSize(partialPathFor(kAppPaths.target, a)) == -1);  // the bad bytes are not kept
        CHECK(h.runs.empty());                                         // and nothing was run
        CHECK(!h.exists("/home/u/FoxSDR.AppImage.update-lock"));
    }
    {  // Right hash, but not a program.
        FakeHost h = appImageWorld();
        const std::string text = "#!/bin/sh\nrm -rf ~\n";
        h.served[kUrl] = text;
        ApplyResult r = applyUpdate(h, installDecision(appImageAsset(text)), kAppPaths);
        CHECK(r.outcome == Outcome::BadPayload);
        CHECK(h.files["/home/u/FoxSDR.AppImage"] == "OLD-COPY" && h.runs.empty());
    }
    {  // Not writable / no space / busy.
        FakeHost h = appImageWorld();
        h.dirWritable = false;
        CHECK(applyUpdate(h, installDecision(appImageAsset()), kAppPaths).outcome == Outcome::NotWritable);
        CHECK(h.fetchFrom.empty() && h.files["/home/u/FoxSDR.AppImage"] == "OLD-COPY");
        h.dirWritable = true;
        h.space = 10;
        CHECK(applyUpdate(h, installDecision(appImageAsset()), kAppPaths).outcome == Outcome::NoSpace);
        CHECK(h.fetchFrom.empty());
        h.space = 1ull << 40;
        h.files["/home/u/FoxSDR.AppImage.update-lock"] = "";
        CHECK(applyUpdate(h, installDecision(appImageAsset()), kAppPaths).outcome == Outcome::Busy);
        CHECK(h.exists("/home/u/FoxSDR.AppImage.update-lock"));  // somebody else's lock is not ours to delete
        CHECK(h.fetchFrom.empty());
    }
    {  // Network failure and cancel.
        FakeHost h = appImageWorld();
        h.fetchFails = true;
        CHECK(applyUpdate(h, installDecision(appImageAsset()), kAppPaths).outcome == Outcome::DownloadFailed);
        CHECK(h.files["/home/u/FoxSDR.AppImage"] == "OLD-COPY" && !h.exists("/home/u/FoxSDR.AppImage.update-lock"));
        h.fetchFails = false;
        std::atomic<bool> cancel{true};
        CHECK(applyUpdate(h, installDecision(appImageAsset()), kAppPaths, &cancel).outcome == Outcome::Cancelled);
        CHECK(h.files["/home/u/FoxSDR.AppImage"] == "OLD-COPY" && !h.exists("/home/u/FoxSDR.AppImage.update-lock"));
    }
    {  // A hand-built decision cannot skip the manifest's rules.
        FakeHost h = appImageWorld();
        Asset a = appImageAsset();
        a.url = "http://foxsdr.com/x";
        CHECK(applyUpdate(h, installDecision(a), kAppPaths).outcome == Outcome::NothingToDo);
        a = appImageAsset();
        a.url = "https://evil.example/x";
        CHECK(applyUpdate(h, installDecision(a), kAppPaths).outcome == Outcome::NothingToDo);
        a = appImageAsset();
        a.sha256 = "zz";
        CHECK(applyUpdate(h, installDecision(a), kAppPaths).outcome == Outcome::NothingToDo);
        CHECK(applyUpdate(h, installDecision(appImageAsset(), InstallKind::AppImage, "../x"), ApplyPaths{kAppPaths.target, "../x"}).outcome == Outcome::NothingToDo);
        CHECK(applyUpdate(h, installDecision(appImageAsset(), InstallKind::AppImage, "0.99.110"), ApplyPaths{kAppPaths.target, "0.99.111"}).outcome == Outcome::NothingToDo);
        Decision notInstall = installDecision(appImageAsset());
        notInstall.verdict = Verdict::TellOnly;
        CHECK(applyUpdate(h, notInstall, kAppPaths).outcome == Outcome::NothingToDo);
        notInstall = installDecision(appImageAsset(), InstallKind::PackageManager);
        CHECK(applyUpdate(h, notInstall, kAppPaths).outcome == Outcome::NothingToDo);  // never replaces a package's files
        CHECK(h.fetchFrom.empty() && h.files["/home/u/FoxSDR.AppImage"] == "OLD-COPY");
    }
}

void testApplyResume() {
    const Asset a = appImageAsset();
    const std::string partial = partialPathFor(kAppPaths.target, a);
    {  // A good partial is resumed from its own size.
        FakeHost h = appImageWorld();
        h.files[partial] = kElf.substr(0, 10);
        ApplyResult r = applyUpdate(h, installDecision(a), kAppPaths);
        CHECK(r.outcome == Outcome::Applied && h.fetchFrom.size() == 1 && h.fetchFrom[0] == 10);
        CHECK(h.files["/home/u/FoxSDR.AppImage"] == kElf);
    }
    {  // The server ignores Range and sends the whole file: still correct.
        FakeHost h = appImageWorld();
        h.rangeOk = false;
        h.files[partial] = kElf.substr(0, 10);
        CHECK(applyUpdate(h, installDecision(a), kAppPaths).outcome == Outcome::Applied);
        CHECK(h.files["/home/u/FoxSDR.AppImage"] == kElf);
    }
    {  // A partial that resumes into wrong bytes: discarded, ONE clean retry from zero, then it installs.
        FakeHost h = appImageWorld();
        h.corruptAppended = true;
        h.files[partial] = kElf.substr(0, 10);
        // The fake corrupts only ranged appends; the retry starts at zero and is clean.
        ApplyResult r = applyUpdate(h, installDecision(a), kAppPaths);
        CHECK(r.outcome == Outcome::Applied && h.fetchFrom.size() == 2 && h.fetchFrom[0] == 10 && h.fetchFrom[1] == 0);
    }
    {  // Wrong even from zero: refused after the single retry, nothing installed, nothing kept.
        FakeHost h = appImageWorld();
        Asset bad = a;
        bad.sha256 = sha("other");
        const std::string pbad = partialPathFor(kAppPaths.target, bad);
        h.files[pbad] = kElf.substr(0, 10);
        ApplyResult r = applyUpdate(h, installDecision(bad), kAppPaths);
        CHECK(r.outcome == Outcome::HashMismatch && h.fetchFrom.size() == 2);
        CHECK(h.fileSize(pbad) == -1 && h.files["/home/u/FoxSDR.AppImage"] == "OLD-COPY");
    }
    {  // A partial larger than the asset can be is discarded, never resumed.
        FakeHost h = appImageWorld();
        h.files[partial] = kElf + "extra";
        ApplyResult r = applyUpdate(h, installDecision(a), kAppPaths);
        CHECK(r.outcome == Outcome::Applied && h.fetchFrom.size() == 1 && h.fetchFrom[0] == 0);
    }
    {  // A partial that is already complete is verified without a download.
        FakeHost h = appImageWorld();
        h.files[partial] = kElf;
        CHECK(applyUpdate(h, installDecision(a), kAppPaths).outcome == Outcome::Applied);
        CHECK(h.fetchFrom.empty());
    }
    {  // A complete-size partial with wrong content is not trusted: hash mismatch, retried from zero once.
        FakeHost h = appImageWorld();
        std::string wrong = kElf;
        wrong[8] ^= 1;
        h.files[partial] = wrong;
        ApplyResult r = applyUpdate(h, installDecision(a), kAppPaths);
        CHECK(r.outcome == Outcome::Applied && h.fetchFrom.size() == 1 && h.fetchFrom[0] == 0);
    }
    {  // A partial that belongs to ANOTHER digest has another name and is simply not touched.
        FakeHost h = appImageWorld();
        Asset other = a;
        other.sha256 = sha("another build");
        h.files[partialPathFor(kAppPaths.target, other)] = "stale";
        CHECK(applyUpdate(h, installDecision(a), kAppPaths).outcome == Outcome::Applied);
        CHECK(h.fetchFrom.size() == 1 && h.fetchFrom[0] == 0);
    }
}

void testApplySwapAndSmoke() {
    {  // The second rename fails: the old copy goes back, and the caller is told.
        // A knob that fails only the FIRST rename onto the target:
        class Once : public FakeHost {
        public:
            bool failed = false;
            bool rename(const std::string& f, const std::string& t) override {
                if (t == "/home/u/FoxSDR.AppImage" && f == "/home/u/FoxSDR.AppImage.new" && !failed) { failed = true; renames.push_back(f + " -> " + t + " (failed)"); return false; }
                return FakeHost::rename(f, t);
            }
        } o;
        o.files["/home/u/FoxSDR.AppImage"] = "OLD-COPY";
        o.served[kUrl] = kElf;
        ApplyResult r = applyUpdate(o, installDecision(appImageAsset()), kAppPaths);
        CHECK(r.outcome == Outcome::SwapFailed && r.restored);
        CHECK(o.files["/home/u/FoxSDR.AppImage"] == "OLD-COPY");
        CHECK(!o.exists("/home/u/FoxSDR.AppImage.update-pending") && !o.exists("/home/u/FoxSDR.AppImage.new"));
        CHECK(o.runs.empty());
    }
    {  // The first rename fails: nothing moved.
        FakeHost h = appImageWorld();
        h.failRenameTo.insert("/home/u/FoxSDR.AppImage.old");
        ApplyResult r = applyUpdate(h, installDecision(appImageAsset()), kAppPaths);
        CHECK(r.outcome == Outcome::SwapFailed && h.files["/home/u/FoxSDR.AppImage"] == "OLD-COPY");
        CHECK(!h.exists("/home/u/FoxSDR.AppImage.new") && !h.exists("/home/u/FoxSDR.AppImage.update-pending"));
    }
    {  // The new copy does not start: the old copy is put back.
        FakeHost h = appImageWorld();
        h.run = [](const std::string&, const std::vector<std::string>&, std::string& out) { out = "Segmentation fault"; return 139; };
        ApplyResult r = applyUpdate(h, installDecision(appImageAsset()), kAppPaths);
        CHECK(r.outcome == Outcome::SmokeFailed && r.restored);
        CHECK(h.files["/home/u/FoxSDR.AppImage"] == "OLD-COPY");
        CHECK(!h.exists("/home/u/FoxSDR.AppImage.old") && !h.exists("/home/u/FoxSDR.AppImage.update-pending"));
        CHECK(h.runs.size() == 2 && h.runs[1].find("--appimage-extract-and-run") != std::string::npos);  // the FUSE-less retry was tried
    }
    {  // It exits 0 but is not the version we installed: also a failure.
        FakeHost h = appImageWorld();
        h.run = [](const std::string&, const std::vector<std::string>&, std::string& out) { out = "FoxSDR 0.99.109"; return 0; };
        CHECK(applyUpdate(h, installDecision(appImageAsset()), kAppPaths).outcome == Outcome::SmokeFailed);
        CHECK(h.files["/home/u/FoxSDR.AppImage"] == "OLD-COPY");
    }
    {  // A machine without FUSE: the plain start fails, the extract-and-run start works.
        FakeHost h = appImageWorld();
        h.run = [](const std::string&, const std::vector<std::string>& a, std::string& out) {
            if (a.size() == 1) { out = "fuse: failed to exec fusermount"; return 127; }
            out = "FoxSDR 0.99.110"; return 0; };
        CHECK(applyUpdate(h, installDecision(appImageAsset()), kAppPaths).outcome == Outcome::Applied);
        CHECK(h.files["/home/u/FoxSDR.AppImage"] == kElf);
    }
    {  // A stale .old from an earlier update does not block a new one.
        FakeHost h = appImageWorld();
        h.files["/home/u/FoxSDR.AppImage.old"] = "TWO-VERSIONS-AGO";
        CHECK(applyUpdate(h, installDecision(appImageAsset()), kAppPaths).outcome == Outcome::Applied);
        CHECK(h.files["/home/u/FoxSDR.AppImage.old"] == "OLD-COPY");
    }
}

void testApplyTree() {
    const std::string tarUrl = "https://foxsdr.com/download/foxsdr-0.99.110-linux-x64.tar.gz";
    const std::string tarBody = std::string("\x1f\x8b", 2) + "tarball-bytes";
    Asset a;
    a.kind = PackageKind::Tarball;
    a.arch = Arch::X86_64;
    a.url = tarUrl;
    a.sha256 = sha(tarBody);
    a.sizeBytes = tarBody.size();
    const ApplyPaths paths{"/home/u/foxsdr", "0.99.110"};
    auto world = [&]() {
        FakeHost h;
        h.files["/home/u/foxsdr/cascade"] = "OLD-BINARY";
        h.served[tarUrl] = tarBody;
        h.archives[partialPathFor(paths.target, a)] = {"foxsdr-0.99.110-linux-x64/", "foxsdr-0.99.110-linux-x64/cascade", "foxsdr-0.99.110-linux-x64/foxsdr-engine"};
        return h;
    };
    {
        FakeHost h = world();
        ApplyResult r = applyUpdate(h, installDecision(a, InstallKind::Tree), paths);
        CHECK(r.outcome == Outcome::Applied);
        CHECK(h.files["/home/u/foxsdr/cascade"] == "x" && h.files["/home/u/foxsdr/foxsdr-engine"] == "x");
        CHECK(h.files["/home/u/foxsdr.old/cascade"] == "OLD-BINARY");
        CHECK(h.runs.size() == 1 && h.runs[0] == "/home/u/foxsdr/cascade --version");
        CHECK(!h.exists("/home/u/foxsdr.new"));
        CHECK(!h.exists(partialPathFor(paths.target, a)));
        CHECK(!confirmStartup(h, paths.target, "0.99.110").empty() && !h.exists("/home/u/foxsdr.old"));
    }
    {  // Unsafe listings are refused before anything is extracted.
        for (const std::vector<std::string>& bad :
             {std::vector<std::string>{"../evil"}, {"/etc/passwd"}, {"foxsdr/ok", "foxsdr/../../x"},
              {"a/x", "b/y"}, {"foxsdr\\evil"}, {}}) {
            FakeHost h = world();
            h.archives[partialPathFor(paths.target, a)] = bad;
            ApplyResult r = applyUpdate(h, installDecision(a, InstallKind::Tree), paths);
            CHECK(r.outcome == Outcome::BadPayload);
            CHECK(h.files["/home/u/foxsdr/cascade"] == "OLD-BINARY" && !h.exists("/home/u/foxsdr.new"));
        }
    }
    {  // Archive cannot be read / cannot be unpacked: old tree untouched, staging gone.
        FakeHost h = world();
        h.listFails = true;
        CHECK(applyUpdate(h, installDecision(a, InstallKind::Tree), paths).outcome == Outcome::ExtractFailed);
        h = world();
        h.extractFails = true;
        CHECK(applyUpdate(h, installDecision(a, InstallKind::Tree), paths).outcome == Outcome::ExtractFailed);
        CHECK(h.files["/home/u/foxsdr/cascade"] == "OLD-BINARY" && !h.exists("/home/u/foxsdr.new"));
    }
    {  // The new tree does not start: the old tree is put back whole.
        FakeHost h = world();
        h.run = [](const std::string&, const std::vector<std::string>&, std::string&) { return 1; };
        ApplyResult r = applyUpdate(h, installDecision(a, InstallKind::Tree), paths);
        CHECK(r.outcome == Outcome::SmokeFailed && r.restored && h.files["/home/u/foxsdr/cascade"] == "OLD-BINARY");
        CHECK(!h.exists("/home/u/foxsdr/foxsdr-engine"));
    }
}

void testConfirmStartup() {
    const std::string t = "/home/u/FoxSDR.AppImage";
    {  // Nothing pending: nothing to say.
        FakeHost h;
        h.files[t] = "X";
        CHECK(confirmStartup(h, t, "0.99.110").empty());
    }
    {  // An interrupted swap (target gone, old kept): the old copy comes back.
        FakeHost h;
        h.files[t + ".old"] = "OLD";
        h.files[t + ".update-pending"] = "0.99.110";
        CHECK(!confirmStartup(h, t, "0.99.109").empty());
        CHECK(h.files[t] == "OLD" && !h.exists(t + ".update-pending"));
    }
    {  // The marker names another version than the one running: stale, the .old copy stays.
        FakeHost h;
        h.files[t] = "X";
        h.files[t + ".old"] = "OLD";
        h.files[t + ".update-pending"] = "0.99.110\n";
        CHECK(confirmStartup(h, t, "0.99.109").empty());
        CHECK(h.exists(t + ".old") && !h.exists(t + ".update-pending"));
    }
    {  // The marker names the running version (trailing newline tolerated): confirmed.
        FakeHost h;
        h.files[t] = "X";
        h.files[t + ".old"] = "OLD";
        h.files[t + ".update-pending"] = "0.99.110\n";
        CHECK(!confirmStartup(h, t, "0.99.110").empty());
        CHECK(!h.exists(t + ".old") && h.files[t] == "X");
    }
}

void testArchiveListing() {
    std::string e;
    CHECK(archiveListingIsSafe({"top/", "top/a", "top/b/c"}, e));
    CHECK(archiveListingIsSafe({"./top/", "./top/a"}, e));
    CHECK(archiveListingIsSafe({"top"}, e));
    CHECK(!archiveListingIsSafe({}, e) && !e.empty());
    CHECK(!archiveListingIsSafe({"./"}, e));
    CHECK(!archiveListingIsSafe({"top/a", "other/b"}, e));
    CHECK(!archiveListingIsSafe({"top/../x"}, e));
    CHECK(!archiveListingIsSafe({".."}, e));
    CHECK(!archiveListingIsSafe({"/abs"}, e));
    CHECK(!archiveListingIsSafe({"top\\x"}, e));
    CHECK(!archiveListingIsSafe({std::string("top/a\0b", 7)}, e));
    CHECK(splitLines("a\r\nb\n\nc").size() == 4 && splitLines("").empty() && splitLines("x\n").size() == 1);
}

// ---------------------------------------------------------------------------------------------
// Real files: the swap, the kept copy and the rollback on an actual directory
// ---------------------------------------------------------------------------------------------
class RealFsHost : public FsUpdateHost {
public:
    std::string body;
    bool smokeOk = true;
    int runs = 0;
    FetchResult fetch(const std::string&, const std::string& partial, std::uint64_t from, std::uint64_t, std::atomic<bool>*) override {
        FetchResult fr;
        std::ofstream out(partial, std::ios::binary | (from > 0 ? std::ios::app : std::ios::trunc));
        out.write(body.data() + from, static_cast<std::streamsize>(body.size() - from));
        out.close();
        fr.status = FetchResult::Status::Ok;
        return fr;
    }
    bool listArchive(const std::string&, std::vector<std::string>&) override { return false; }
    bool extractArchive(const std::string&, const std::string&) override { return false; }
    int runProgram(const std::string&, const std::vector<std::string>&, int, std::string& out) override {
        ++runs;
        out = "FoxSDR 0.99.110";
        return smokeOk ? 0 : 1;
    }
};

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void testRealFiles() {
    const fs::path root = fs::temp_directory_path() / ("foxsdr-linux-update-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    const std::string target = (root / "FoxSDR.AppImage").generic_string();
    auto put = [](const std::string& p, const std::string& s) { std::ofstream(p, std::ios::binary) << s; };
    {
        RealFsHost h;
        h.body = kElf;
        put(target, "OLD-COPY");
        ApplyResult r = applyUpdate(h, installDecision(appImageAsset()), ApplyPaths{target, "0.99.110"});
        CHECK(r.outcome == Outcome::Applied);
        CHECK(slurp(target) == kElf && slurp(target + ".old") == "OLD-COPY");
        CHECK(slurp(target + ".update-pending") == "0.99.110");
        CHECK(!fs::exists(target + ".update-lock") && !fs::exists(target + ".new"));
        CHECK(!confirmStartup(h, target, "0.99.110").empty());
        CHECK(!fs::exists(target + ".old") && !fs::exists(target + ".update-pending"));
    }
    {  // A new copy that does not start: the old FILE is back, the failed one gone.
        RealFsHost h;
        h.body = kElf;
        h.smokeOk = false;
        put(target, "OLD-COPY");
        ApplyResult r = applyUpdate(h, installDecision(appImageAsset()), ApplyPaths{target, "0.99.110"});
        CHECK(r.outcome == Outcome::SmokeFailed && r.restored && slurp(target) == "OLD-COPY");
        CHECK(!fs::exists(target + ".old") && !fs::exists(target + ".update-pending"));
    }
    {  // A real partial on disk is resumed from its real size.
        RealFsHost h;
        h.body = kElf;
        put(target, "OLD-COPY");
        put(partialPathFor(target, appImageAsset()), kElf.substr(0, 7));
        CHECK(applyUpdate(h, installDecision(appImageAsset()), ApplyPaths{target, "0.99.110"}).outcome == Outcome::Applied);
        CHECK(slurp(target) == kElf);
    }
    {  // The host's own primitives.
        RealFsHost f;
        CHECK(f.writableDir(root.generic_string()));
        CHECK(!f.writableDir((root / "no-such-dir").generic_string()));
        CHECK(f.createExclusive((root / "lock").generic_string()));
        CHECK(!f.createExclusive((root / "lock").generic_string()));  // exclusive means exclusive
        CHECK(f.freeSpace(root.generic_string()) > 0);
        CHECK(f.fileSize((root / "lock").generic_string()) == 0 && f.fileSize((root / "nope").generic_string()) == -1);
        std::string head;
        CHECK(f.readHead(target, 4, head) && head.size() == 4);
        CHECK(f.makeDir((root / "d" / "e").generic_string()) && f.exists((root / "d" / "e").generic_string()));
        CHECK(f.removeAll((root / "d").generic_string()) && !f.exists((root / "d").generic_string()));
        std::string h1;
        CHECK(f.sha256(target, h1) && h1 == sha(kElf));
    }
    std::error_code ec;
    fs::remove_all(root, ec);
}

// ---------------------------------------------------------------------------------------------
// Stand-in HTTP server on 127.0.0.1: the ranged fetch against real bytes on a socket
// ---------------------------------------------------------------------------------------------
struct StandIn {
    httplib::Server svr;
    std::thread th;
    int port = 0;
    std::string body;
    bool honourRange = true;
    bool wrongContentRange = false;
    int forceStatus = 0;
    std::atomic<int> hits{0};
    std::string lastRange;

    explicit StandIn(std::string b) : body(std::move(b)) {
        svr.Get("/file", [this](const httplib::Request& req, httplib::Response& res) {
            ++hits;
            lastRange = req.get_header_value("Range");
            if (forceStatus != 0) { res.status = forceStatus; return; }
            if (honourRange && req.has_header("Range")) {
                unsigned long long from = std::strtoull(lastRange.c_str() + 6, nullptr, 10);  // "bytes=N-"
                if (from >= body.size()) { res.status = 416; return; }
                // The vendored server slices a 206 itself (apply_ranges) and writes Content-Range;
                // wrongContentRange lies about where the slice starts, after it has done so.
                res.status = 206;
                res.set_content(body, "application/octet-stream");
            } else {
                res.status = 200;  // explicit: the vendored server turns an unset status into 206 for a ranged request
                res.set_content(body, "application/octet-stream");
            }
        });
        svr.set_post_routing_handler([this](const httplib::Request&, httplib::Response& res) {
            if (wrongContentRange && res.status == 206) {
                res.headers.erase("Content-Range");
                res.set_header("Content-Range", "bytes 1-" + std::to_string(body.size() - 1) + "/" + std::to_string(body.size()));
            }
        });
        port = svr.bind_to_any_port("127.0.0.1");
        th = std::thread([this] { svr.listen_after_bind(); });
        svr.wait_until_ready();
    }
    ~StandIn() { svr.stop(); if (th.joinable()) { th.join(); } }
};

class LoopHost : public FsUpdateHost {
public:
    int port = 0;
    FetchResult fetch(const std::string&, const std::string& partial, std::uint64_t from, std::uint64_t maxBytes, std::atomic<bool>* cancel) override {
        httplib::Client cli("127.0.0.1", port);
        cli.set_follow_location(false);
        return rangeFetch(cli, "/file", partial, from, maxBytes, cancel);
    }
    bool listArchive(const std::string&, std::vector<std::string>&) override { return false; }
    bool extractArchive(const std::string&, const std::string&) override { return false; }
    int runProgram(const std::string&, const std::vector<std::string>&, int, std::string& out) override { out = "FoxSDR 0.99.110"; return 0; }
};

void testLoopback() {
    std::string big(300000, '\0');
    for (std::size_t i = 0; i < big.size(); ++i) { big[i] = static_cast<char>((i * 31 + 7) & 0xff); }
    big.replace(0, 4, std::string("\x7f" "ELF", 4));
    const fs::path root = fs::temp_directory_path() / ("foxsdr-linux-loop-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    const std::string part = (root / "p").generic_string();
    auto fileBytes = [&]() { return slurp(part); };

    StandIn srv(big);
    httplib::Client cli("127.0.0.1", srv.port);
    FetchResult fr = rangeFetch(cli, "/file", part, 0, 1u << 30, nullptr);
    CHECK(fr.status == FetchResult::Status::Ok && !fr.resumed && fr.totalBytes == big.size() && fileBytes() == big);
    CHECK(srv.lastRange.empty());  // a fresh download sends no Range

    // Resume: only the missing tail is sent, and it is appended.
    { std::ofstream(part, std::ios::binary | std::ios::trunc) << big.substr(0, 1000); }
    fr = rangeFetch(cli, "/file", part, 1000, 1u << 30, nullptr);
    CHECK(fr.status == FetchResult::Status::Ok && fr.resumed && srv.lastRange == "bytes=1000-");
    CHECK(fileBytes() == big);

    // The server ignores Range: the file restarts from zero (resumed=false), never a corrupt splice.
    srv.honourRange = false;
    { std::ofstream(part, std::ios::binary | std::ios::trunc) << big.substr(0, 1000); }
    fr = rangeFetch(cli, "/file", part, 1000, 1u << 30, nullptr);
    CHECK(fr.status == FetchResult::Status::Ok && !fr.resumed && fileBytes() == big);
    srv.honourRange = true;

    // A Content-Range that does not begin where we asked is refused.
    srv.wrongContentRange = true;
    { std::ofstream(part, std::ios::binary | std::ios::trunc) << big.substr(0, 1000); }
    fr = rangeFetch(cli, "/file", part, 1000, 1u << 30, nullptr);
    CHECK(fr.status == FetchResult::Status::Failed);
    CHECK(fileBytes().size() == 1000);  // the partial was not touched
    srv.wrongContentRange = false;

    // 416: the partial cannot be right; it is deleted so the next attempt starts clean.
    { std::ofstream(part, std::ios::binary | std::ios::trunc) << big; }
    fr = rangeFetch(cli, "/file", part, big.size(), 1u << 30, nullptr);
    CHECK(fr.status == FetchResult::Status::Failed && !fs::exists(part));

    // Other statuses fail; a redirect is not followed.
    srv.forceStatus = 404;
    CHECK(rangeFetch(cli, "/file", part, 0, 1u << 30, nullptr).status == FetchResult::Status::Failed);
    srv.forceStatus = 302;
    CHECK(rangeFetch(cli, "/file", part, 0, 1u << 30, nullptr).status == FetchResult::Status::Failed);
    srv.forceStatus = 0;

    // The byte cap: declared too large, and (below) the file stays unwritten.
    fs::remove(part);
    fr = rangeFetch(cli, "/file", part, 0, 1000, nullptr);
    CHECK(fr.status == FetchResult::Status::Failed && fr.error.find("larger") != std::string::npos);

    // Cancel before and during.
    std::atomic<bool> cancel{true};
    fr = rangeFetch(cli, "/file", part, 0, 1u << 30, &cancel);
    CHECK(fr.status == FetchResult::Status::Cancelled);

    // Nobody listening: a failure, not a hang or a throw.
    {
        httplib::Client dead("127.0.0.1", 1);
        dead.set_connection_timeout(1, 0);
        CHECK(rangeFetch(dead, "/file", part, 0, 1u << 30, nullptr).status == FetchResult::Status::Failed);
    }

    // End to end through applyUpdate over the socket, resuming a partial.
    {
        const std::string target = (root / "FoxSDR.AppImage").generic_string();
        { std::ofstream(target, std::ios::binary) << "OLD-COPY"; }
        const Asset a = appImageAsset(big);
        { std::ofstream(partialPathFor(target, a), std::ios::binary) << big.substr(0, 4096); }
        LoopHost h;
        h.port = srv.port;
        srv.honourRange = true;
        ApplyResult r = applyUpdate(h, installDecision(a), ApplyPaths{target, "0.99.110"});
        CHECK(r.outcome == Outcome::Applied && srv.lastRange == "bytes=4096-");
        CHECK(slurp(target) == big && slurp(target + ".old") == "OLD-COPY");
        // And a server that hands over different bytes than the manifest promised is refused.
        { std::ofstream(target, std::ios::binary) << "OLD-COPY"; }
        fs::remove(target + ".old");
        fs::remove(target + ".update-pending");
        srv.body = big;
        srv.body[100] ^= 1;
        r = applyUpdate(h, installDecision(a), ApplyPaths{target, "0.99.110"});
        CHECK(r.outcome == Outcome::HashMismatch && slurp(target) == "OLD-COPY");
    }
    std::error_code ec;
    fs::remove_all(root, ec);
}

}  // namespace

int main() {
    testManifest();
    testFuzz();
    testInstallKind();
    testDecide();
    testApplyAppImage();
    testApplyRefusals();
    testApplyResume();
    testApplySwapAndSmoke();
    testApplyTree();
    testConfirmStartup();
    testArchiveListing();
    testRealFiles();
    testLoopback();
    return testSummary("test_linux_update");
}
