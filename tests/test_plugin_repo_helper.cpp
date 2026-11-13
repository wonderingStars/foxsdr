// test_plugin_repo_helper.cpp - GPL HELPER PROGRAMS in the Plugin store (wiring lane C group 7: src/core/plugin_repo_helper.{hpp,cpp}, hooks in core/plugin_repo.cpp), against a
// LOCAL STAND-IN CATALOGUE and a stand-in transport: no network, no download, no real helper (the program here is eighty bytes of a PE header made in memory).
//
//   H1  parse: a helper entry with its helper object and companions reads back; every malformed document is refused WHOLE (no helper kind, no source commit, a licence that is
//       not GPL-family, a non-https source or companion url, a bad sha256, a companion installAs with a separator / ".." / drive letter / a ".dll" / the store's own name,
//       two companions with one name, the wrong number of helper / manifest / licence companions, a manifest or licence that does not install under the helper's stem,
//       an executable flag on anything but the program, more than eight companions, a platform row without companions); an ordinary plugin still parses exactly as before
//   H2  install: the bridge DLL and the three companions are all in the plugins folder, with the hashes the catalogue gave; recorded in helpers.json; the staging folder is gone
//   H3  all or nothing: a hash mismatch on ANY companion (the first, the second, the last), a helper that is not a program (a text file, a PE for another machine), a manifest that
//       names another program, an empty licence, a transfer that fails half way, and a bridge that cannot be moved in - each leaves the plugins folder as it was (no new file,
//       no .part, no staging folder, an earlier version's files restored)
//   H4  update: a newer version replaces all four; a failed update leaves the old four; a companion the new version no longer has is removed
//   H5  remove: the plugin's file and its companions are deleted, helpers.json forgets it; a companion that cannot be deleted is reported and the entry is kept
//   H6  a helper entry from a plugin SITE is refused before any socket
//   H7  the store's words: the badge line, the detail lines, the consent sentence
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "core/plugin_repo.hpp"
#include "core/plugin_repo_helper.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using cascade::core::PluginCatalogEntry;
using cascade::core::PluginRepo;
namespace hs = cascade::core::helperstore;

namespace {

fs::path g_dir;

std::string sha(const std::string& bytes) {
    std::string hex, err;
    CHECK(PluginRepo::sha256Hex(bytes.data(), bytes.size(), hex, err));
    return hex;
}

// A Windows program image, made in memory: MZ, the PE signature at 0x80 and the machine word. Never run.
std::string peImage(unsigned machine) {
    std::string b(512, '\0');
    b[0] = 'M';
    b[1] = 'Z';
    b[0x3C] = static_cast<char>(0x80);
    std::memcpy(&b[0x80], "PE\0\0", 4);
    b[0x84] = static_cast<char>(machine & 0xFF);
    b[0x85] = static_cast<char>((machine >> 8) & 0xFF);
    return b;
}

std::string elfImage(unsigned machine) {
    std::string b(64, '\0');
    b[0] = 0x7F;
    b[1] = 'E';
    b[2] = 'L';
    b[3] = 'F';
    b[4] = 2;
    b[18] = static_cast<char>(machine & 0xFF);
    b[19] = static_cast<char>((machine >> 8) & 0xFF);
    return b;
}

// The program of THIS machine's kind.
std::string myProgram() {
    const std::string os = PluginRepo::hostOs();
    const std::string arch = PluginRepo::hostArch();
    if (os == "windows") { return peImage(arch == "arm64" ? 0xAA64u : arch == "x86" ? 0x014Cu : 0x8664u); }
    return elfImage(arch == "arm64" ? 183u : 62u);
}
std::string otherProgram() {
    const std::string os = PluginRepo::hostOs();
    if (os == "windows") { return peImage(0x014Cu == 0x8664u ? 0x8664u : (std::string(PluginRepo::hostArch()) == "x86" ? 0x8664u : 0x014Cu)); }
    return elfImage(40u);
}

struct World {
    std::map<std::string, std::string> files;   // url -> bytes
    std::vector<std::string> asked;
    std::string failUrl;                          // a transfer that fails
    std::string failAfterUrlHalf;                 // a transfer that ends after half its bytes
    void install() {
        PluginRepo::setTransportForTest([this](const std::string& url, std::uint64_t maxBytes, const std::function<bool(const void*, std::size_t)>& sink, std::string& error) {
            asked.push_back(url);
            if (url == failUrl) {
                error = "connection reset";
                return false;
            }
            const auto it = files.find(url);
            if (it == files.end()) {
                error = "HTTP 404";
                return false;
            }
            if (it->second.size() > maxBytes) {
                error = "too large";
                return false;
            }
            std::size_t n = it->second.size();
            if (url == failAfterUrlHalf) {
                sink(it->second.data(), n / 2);
                error = "connection reset";
                return false;
            }
            return sink(it->second.data(), n);
        });
    }
};

struct Version {
    std::string version = "1.0.0";
    std::string program;    // bytes of the helper program
    std::string manifest;   // bytes
    std::string licence;
    std::string dll;
    std::string programName = "multimon-ng.exe";
};

std::string manifestFor(const std::string& exe) { return "{\"protocol\":1,\"name\":\"multimon-ng\",\"executable\":\"" + exe + "\"}"; }

// The index JSON of one helper entry, and the files a World serves for it. `mutate` lets a test break one field.
std::string helperIndex(World& w, const Version& v, const std::string& extraHelper = "", const std::string& source = "") {
    const std::string base = "https://dl.example.invalid/" + v.version + "/";
    const std::string stem = v.programName.substr(0, v.programName.rfind('.'));
    const std::string dllName = "helper-bridge-" + v.version + "-abi3.dll";
    w.files[base + dllName] = v.dll;
    w.files[base + v.programName] = v.program;
    w.files[base + stem + ".helper.json"] = v.manifest;
    w.files[base + stem + ".LICENSE.txt"] = v.licence;
    (void)source;
    std::string j = std::string("{\"schemaVersion\":1,\"plugins\":[{\"id\":\"helper-bridge-multimon\",\"name\":\"Pager decoders\",\"version\":\"") + v.version +
                    "\",\"author\":\"x\",\"licence\":\"MIT\",\"abiVersion\":" + std::to_string(CASCADE_PLUGIN_ABI_VERSION) +
                    ",\"helper\":{\"kind\":\"gpl-helper\",\"name\":\"multimon-ng\",\"version\":\"1.4.1\",\"licence\":\"GPL-2.0-only\",\"sourceUrl\":\"https://example.invalid/mm/tree/" +
                    std::string(40, 'a') + "\",\"sourceCommit\":\"" + std::string(40, 'a') + "\"" + extraHelper + "},\"platforms\":[{\"os\":\"" + PluginRepo::hostOs() + "\",\"arch\":\"" +
                    PluginRepo::hostArch() + "\",\"file\":\"" + dllName + "\",\"url\":\"" + base + dllName + "\",\"sha256\":\"" + sha(v.dll) +
                    "\",\"sizeBytes\":10,\"companions\":[" + "{\"role\":\"helper\",\"file\":\"" + v.programName + "\",\"installAs\":\"" + v.programName + "\",\"url\":\"" + base + v.programName +
                    "\",\"sha256\":\"" + sha(v.program) + "\",\"sizeBytes\":512,\"executable\":true}," + "{\"role\":\"manifest\",\"file\":\"" + stem + ".helper.json\",\"installAs\":\"" + stem +
                    ".helper.json\",\"url\":\"" + base + stem + ".helper.json\",\"sha256\":\"" + sha(v.manifest) + "\",\"sizeBytes\":80,\"executable\":false}," + "{\"role\":\"licence\",\"file\":\"" +
                    stem + ".LICENSE.txt\",\"installAs\":\"" + stem + ".LICENSE.txt\",\"url\":\"" + base + stem + ".LICENSE.txt\",\"sha256\":\"" + sha(v.licence) +
                    "\",\"sizeBytes\":18000,\"executable\":false}]}]}]}";
    return j;
}

Version goodVersion(const std::string& ver = "1.0.0") {
    Version v;
    v.version = ver;
    v.program = myProgram();
    v.manifest = manifestFor("multimon-ng.exe");
    v.licence = "GNU GENERAL PUBLIC LICENSE Version 2 (stand-in text)";
    v.dll = "stand-in bridge " + ver;
    return v;
}

bool parse(const std::string& json, std::vector<PluginCatalogEntry>& out, std::string& err) {
    out.clear();
    return PluginRepo::parseIndex(json, out, err);
}

// every file under the folder, relative, sorted: what is "left behind"
std::vector<std::string> tree(const fs::path& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(dir, ec)) {
        if (e.is_regular_file()) { out.push_back(fs::relative(e.path(), dir).generic_string()); }
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool installOne(World& w, const std::string& json, const fs::path& dir, std::string& err, std::string& path) {
    std::vector<PluginCatalogEntry> es;
    std::string perr;
    if (!parse(json, es, perr) || es.size() != 1u) {
        err = "parse: " + perr;
        return false;
    }
    w.install();
    PluginRepo repo;
    return repo.install(es[0], dir.string(), path, err);
}

// ---- H1 ----------------------------------------------------------------------------------------------------------------------------------
void testParse() {
    std::printf("[H1] parse\n");
    World w;
    const Version v = goodVersion();
    std::vector<PluginCatalogEntry> es;
    std::string err;
    CHECK(parse(helperIndex(w, v), es, err));
    CHECK(es.size() == 1u);
    if (es.size() != 1u) { return; }
    const PluginCatalogEntry& e = es[0];
    CHECK(e.helper.present && e.helper.name == "multimon-ng" && e.helper.version == "1.4.1" && e.helper.licence == "GPL-2.0-only");
    CHECK(e.helper.sourceCommit == std::string(40, 'a'));
    const cascade::core::PluginPlatform* p = e.thisPlatform();
    CHECK(p != nullptr);
    if (p == nullptr) { return; }
    CHECK(p->companions.size() == 3u);
    if (p->companions.size() == 3u) {
        CHECK(p->companions[0].role == "helper" && p->companions[0].executable && p->companions[0].installAs == "multimon-ng.exe");
        CHECK(p->companions[1].role == "manifest" && !p->companions[1].executable);
        CHECK(p->companions[2].role == "licence" && p->companions[2].installAs == "multimon-ng.LICENSE.txt");
    }

    // every malformed document is refused whole. Each variant replaces one piece of the good index.
    struct Bad { const char* why; std::string from; std::string to; };
    const std::string base = helperIndex(w, v);
    const std::vector<Bad> bad = {
        {"kind is not gpl-helper", "\"kind\":\"gpl-helper\"", "\"kind\":\"plain\""},
        {"no kind", "\"kind\":\"gpl-helper\",", ""},
        {"commit too short", "\"sourceCommit\":\"" + std::string(40, 'a') + "\"", "\"sourceCommit\":\"abc\""},
        {"commit not hex", "\"sourceCommit\":\"" + std::string(40, 'a') + "\"", "\"sourceCommit\":\"" + std::string(40, 'z') + "\""},
        {"licence not GPL family", "\"licence\":\"GPL-2.0-only\"", "\"licence\":\"MIT\""},
        {"source not https", "\"sourceUrl\":\"https://", "\"sourceUrl\":\"http://"},
        {"companion url not https", "\"url\":\"https://dl.example.invalid/1.0.0/multimon-ng.exe\"", "\"url\":\"http://dl.example.invalid/1.0.0/multimon-ng.exe\""},
        {"companion sha256 short", "\"sha256\":\"" + sha(v.program) + "\"", "\"sha256\":\"abc\""},
        {"installAs with a separator", "\"installAs\":\"multimon-ng.exe\"", "\"installAs\":\"sub/multimon-ng.exe\""},
        {"installAs with a backslash", "\"installAs\":\"multimon-ng.exe\"", "\"installAs\":\"sub\\\\multimon-ng.exe\""},
        {"installAs with ..", "\"installAs\":\"multimon-ng.exe\"", "\"installAs\":\"..mm.exe\""},
        {"installAs a drive", "\"installAs\":\"multimon-ng.exe\"", "\"installAs\":\"C:mm.exe\""},
        {"installAs a dll", "\"installAs\":\"multimon-ng.exe\"", "\"installAs\":\"multimon-ng.dll\""},
        {"installAs the store's own", "\"installAs\":\"multimon-ng.LICENSE.txt\"", "\"installAs\":\"installed.json\""},
        {"manifest under another stem", "\"installAs\":\"multimon-ng.helper.json\"", "\"installAs\":\"other.helper.json\""},
        {"licence under another stem", "\"installAs\":\"multimon-ng.LICENSE.txt\"", "\"installAs\":\"other.LICENSE.txt\""},
        {"executable on the manifest", "\"sizeBytes\":80,\"executable\":false", "\"sizeBytes\":80,\"executable\":true"},
        {"program not executable", "\"sizeBytes\":512,\"executable\":true", "\"sizeBytes\":512,\"executable\":false"},
        {"unknown role", "\"role\":\"licence\"", "\"role\":\"mystery\""},
        {"no companions", ",\"companions\":[", ",\"companionsX\":["},
    };
    for (const Bad& b : bad) {
        std::string doc = base;
        const std::size_t at = doc.find(b.from);
        if (at == std::string::npos) {
            std::printf("      variant '%s' does not apply to the base text\n", b.why);
            CHECK(false);
            continue;
        }
        doc.replace(at, b.from.size(), b.to);
        std::vector<PluginCatalogEntry> out;
        std::string e2;
        const bool accepted = parse(doc, out, e2);
        if (accepted) { std::printf("      ACCEPTED: %s\n", b.why); }
        CHECK(!accepted);
    }
    {   // two helper programs; and nine companions
        std::string doc = base;
        const std::size_t at = doc.find("{\"role\":\"manifest\"");
        std::string twice = doc;
        twice.replace(at, 0, "{\"role\":\"helper\",\"file\":\"b.exe\",\"installAs\":\"b.exe\",\"url\":\"https://dl.example.invalid/b.exe\",\"sha256\":\"" + std::string(64, 'b') + "\",\"executable\":true},");
        std::vector<PluginCatalogEntry> out;
        std::string e2;
        CHECK(!parse(twice, out, e2));
        std::string many = doc;
        std::string extra;
        for (int i = 0; i < 6; ++i) { extra += "{\"role\":\"other\",\"file\":\"x" + std::to_string(i) + ".txt\",\"installAs\":\"x" + std::to_string(i) + ".txt\",\"url\":\"https://dl.example.invalid/x\",\"sha256\":\"" + std::string(64, 'c') + "\",\"executable\":false},"; }
        many.replace(many.find("{\"role\":\"manifest\""), 0, extra);
        CHECK(!parse(many, out, e2));
    }
    {   // an ordinary plugin entry parses exactly as before, with no helper
        const std::string plain = std::string("{\"schemaVersion\":1,\"plugins\":[{\"id\":\"p\",\"name\":\"P\",\"version\":\"1.0.0\",\"licence\":\"MIT\",\"abiVersion\":") + std::to_string(CASCADE_PLUGIN_ABI_VERSION) +
                                  ",\"platforms\":[{\"os\":\"" + PluginRepo::hostOs() + "\",\"arch\":\"" + PluginRepo::hostArch() + "\",\"file\":\"p.dll\",\"url\":\"https://x.invalid/p.dll\",\"sha256\":\"" +
                                  std::string(64, 'd') + "\"}]}]}";
        std::vector<PluginCatalogEntry> out;
        std::string e2;
        CHECK(parse(plain, out, e2));
        CHECK(out.size() == 1u && !out[0].helper.present && out[0].platforms.size() == 1u && out[0].platforms[0].companions.empty());
        // companions on a row of an entry that has no helper object are ignored (a half-understood helper is refused, an absent one is not a helper)
        std::string with = plain;
        with.replace(with.find("\"sha256\":\"" + std::string(64, 'd') + "\""), 0, "\"companions\":[{\"role\":\"helper\"}],");
        CHECK(parse(with, out, e2));
        CHECK(out.size() == 1u && !out[0].helper.present && out[0].platforms[0].companions.empty());
    }
}

// ---- H2 ----------------------------------------------------------------------------------------------------------------------------------
void testInstall() {
    std::printf("[H2] install\n");
    const fs::path dir = g_dir / "h2";
    fs::create_directories(dir);
    World w;
    const Version v = goodVersion();
    std::string err, path;
    CHECK(installOne(w, helperIndex(w, v), dir, err, path));
    if (!err.empty()) { std::printf("      %s\n", err.c_str()); }
    CHECK(fs::exists(dir / "helper-bridge-1.0.0-abi3.dll"));
    CHECK(slurp(dir / "multimon-ng.exe") == v.program);
    CHECK(slurp(dir / "multimon-ng.helper.json") == v.manifest);
    CHECK(slurp(dir / "multimon-ng.LICENSE.txt") == v.licence);
    CHECK(tree(dir) == (std::vector<std::string>{"helper-bridge-1.0.0-abi3.dll", "multimon-ng.LICENSE.txt", "multimon-ng.exe", "multimon-ng.helper.json"}));   // no .part, no staging
    // recorded (what the manifest step does) and listed beside installed.json
    std::vector<PluginCatalogEntry> es;
    std::string perr;
    CHECK(parse(helperIndex(w, v), es, perr));
    CHECK(PluginRepo::recordInstall(dir.string(), es[0], err));
    CHECK(hs::companionsOf(dir.string(), "helper-bridge-1.0.0-abi3.dll") == (std::vector<std::string>{"multimon-ng.exe", "multimon-ng.helper.json", "multimon-ng.LICENSE.txt"}));
    fs::remove_all(dir);
}

// ---- H3 ----------------------------------------------------------------------------------------------------------------------------------
void testAllOrNothing() {
    std::printf("[H3] all or nothing\n");
    struct Case { const char* name; std::function<void(World&, Version&)> shape; std::string failUrlSuffix; std::string halfUrlSuffix; };
    const std::vector<Case> cases = {
        {"hash of the helper program is wrong", [](World& w, Version& v) { (void)w; v.program += "tampered-after-hashing"; }, "", ""},   // (the index is hashed AFTER shape: see below)
    };
    (void)cases;
    const std::string base = "https://dl.example.invalid/1.0.0/";
    struct Run { const char* name; int mode; };
    for (const Run& run : {Run{"tampered helper", 0}, Run{"tampered manifest", 1}, Run{"tampered licence", 2}, Run{"helper is text", 3}, Run{"helper is for another machine", 4}, Run{"manifest names another program", 5},
                           Run{"empty licence", 6}, Run{"transfer fails", 7}, Run{"transfer ends half way", 8}}) {
        const fs::path dir = g_dir / "h3";
        fs::remove_all(dir);
        fs::create_directories(dir);
        World w;
        Version v = goodVersion();
        if (run.mode == 3) { v.program = "this is not a program at all"; }
        if (run.mode == 4) { v.program = otherProgram(); }
        if (run.mode == 5) { v.manifest = manifestFor("something-else.exe"); }
        if (run.mode == 6) { v.licence = ""; }
        const std::string index = helperIndex(w, v);   // hashed from the Version as it is NOW
        if (run.mode == 0) { w.files[base + "multimon-ng.exe"] += "x"; }
        if (run.mode == 1) { w.files[base + "multimon-ng.helper.json"] += " "; }
        if (run.mode == 2) { w.files[base + "multimon-ng.LICENSE.txt"] += "x"; }
        if (run.mode == 7) { w.failUrl = base + "multimon-ng.LICENSE.txt"; }
        if (run.mode == 8) { w.failAfterUrlHalf = base + "multimon-ng.exe"; }
        std::string err, path;
        const bool ok = installOne(w, index, dir, err, path);
        if (ok) { std::printf("      '%s' was INSTALLED\n", run.name); }
        CHECK(!ok);
        CHECK(!err.empty());
        const std::vector<std::string> left = tree(dir);
        if (!left.empty()) { std::printf("      '%s' left behind: %s ...\n", run.name, left[0].c_str()); }
        CHECK(left.empty());
        std::error_code ec;
        bool stagingGone = true;
        for (const auto& e : fs::directory_iterator(dir, ec)) { stagingGone = false; (void)e; }
        CHECK(stagingGone);   // not even an empty staging folder
        fs::remove_all(dir);
    }
    {   // a bridge that cannot be moved in (its name is a folder): the companions go back as they were, an earlier version's included
        const fs::path dir = g_dir / "h3b";
        fs::remove_all(dir);
        fs::create_directories(dir);
        World w;
        const Version old = goodVersion("1.0.0");
        std::string err, path;
        CHECK(installOne(w, helperIndex(w, old), dir, err, path));
        // the new version's bridge cannot replace a directory of that name
        fs::remove(dir / "helper-bridge-1.1.0-abi3.dll");
        fs::create_directories(dir / "helper-bridge-1.1.0-abi3.dll");
        Version nv = goodVersion("1.1.0");
        nv.program = myProgram() + std::string(16, '\1');
        CHECK(!installOne(w, helperIndex(w, nv), dir, err, path));
        CHECK(slurp(dir / "multimon-ng.exe") == old.program);   // the old program is back
        CHECK(slurp(dir / "multimon-ng.helper.json") == old.manifest);
        bool backups = false;
        for (const auto& e : fs::directory_iterator(dir)) {
            if (e.path().filename().string().find(".wcbak") != std::string::npos || e.path().filename().string().find(".helper-stage") != std::string::npos) { backups = true; }
        }
        CHECK(!backups);
        fs::remove_all(dir);
    }
}

// ---- H4 / H5 -----------------------------------------------------------------------------------------------------------------------------
void testUpdateAndRemove() {
    std::printf("[H4] update, [H5] remove\n");
    const fs::path dir = g_dir / "h4";
    fs::remove_all(dir);
    fs::create_directories(dir);
    World w;
    const Version v1 = goodVersion("1.0.0");
    std::string err, path;
    std::vector<PluginCatalogEntry> es;
    std::string perr;
    CHECK(installOne(w, helperIndex(w, v1), dir, err, path));
    CHECK(parse(helperIndex(w, v1), es, perr) && PluginRepo::recordInstall(dir.string(), es[0], err));
    // update: new bridge name, new program bytes; all four replaced; the old bridge is the manifest's business (not this lane's)
    Version v2 = goodVersion("1.1.0");
    v2.program = myProgram() + std::string(32, '\2');
    CHECK(installOne(w, helperIndex(w, v2), dir, err, path));
    CHECK(slurp(dir / "multimon-ng.exe") == v2.program);
    CHECK(slurp(dir / "helper-bridge-1.1.0-abi3.dll") == v2.dll);
    {
        bool leftovers = false;
        for (const auto& e : fs::directory_iterator(dir)) {
            const std::string n = e.path().filename().string();
            if (n.find(".wcbak") != std::string::npos || n.find(".part") != std::string::npos || n.find(".helper-stage") != std::string::npos) { leftovers = true; }
        }
        CHECK(!leftovers);
    }
    // a failed update (the licence of 1.2.0 is tampered) leaves the 1.1.0 four
    Version v3 = goodVersion("1.2.0");
    v3.program = myProgram() + std::string(48, '\3');
    const std::string idx3 = helperIndex(w, v3);
    w.files["https://dl.example.invalid/1.2.0/multimon-ng.LICENSE.txt"] += "tampered";
    CHECK(!installOne(w, idx3, dir, err, path));
    CHECK(slurp(dir / "multimon-ng.exe") == v2.program);
    CHECK(!fs::exists(dir / "helper-bridge-1.2.0-abi3.dll"));
    // remove: the plugin file and the three companions go, helpers.json forgets it
    CHECK(parse(helperIndex(w, v2), es, perr) && PluginRepo::recordInstall(dir.string(), es[0], err));
    CHECK(hs::companionsOf(dir.string(), "helper-bridge-1.1.0-abi3.dll").size() == 3u);
    PluginRepo repo;
    CHECK(repo.remove(dir.string(), "helper-bridge-1.1.0-abi3.dll", err));
    CHECK(!fs::exists(dir / "multimon-ng.exe") && !fs::exists(dir / "multimon-ng.helper.json") && !fs::exists(dir / "multimon-ng.LICENSE.txt"));
    CHECK(hs::companionsOf(dir.string(), "helper-bridge-1.1.0-abi3.dll").empty());
    CHECK(hs::companionsOf(dir.string(), "helper-bridge-1.0.0-abi3.dll").size() == 3u);   // the first install's entry is another plugin file's: untouched
    // the bridge of 1.0.0 is still there from the first install (an update does not remove the old file: that is the manifest's business): not touched by removing 1.1.0
    CHECK(fs::exists(dir / "helper-bridge-1.0.0-abi3.dll"));
    fs::remove_all(dir);
}

// ---- H6 / H7 -----------------------------------------------------------------------------------------------------------------------------
void testSiteAndWords() {
    std::printf("[H6] a site's helper, [H7] the words\n");
    const fs::path dir = g_dir / "h6";
    fs::remove_all(dir);
    fs::create_directories(dir);
    World w;
    const Version v = goodVersion();
    std::vector<PluginCatalogEntry> es;
    std::string perr;
    CHECK(parse(helperIndex(w, v), es, perr) && es.size() == 1u);
    if (es.size() != 1u) { return; }
    es[0].source = "https://dl.example.invalid/plugins.json";   // what mergeSite sets
    w.install();
    PluginRepo repo;
    std::string err, path;
    CHECK(!repo.install(es[0], dir.string(), path, err));
    CHECK(err.find("separate program") != std::string::npos);
    CHECK(w.asked.empty());   // not one socket was asked
    CHECK(tree(dir).empty());
    es[0].source.clear();
    CHECK(hs::rowLine(es[0]) == "GPL helper - source: https://example.invalid/mm/tree/" + std::string(40, 'a'));
    const std::vector<std::string> lines = hs::detailLines(es[0]);
    CHECK(lines.size() == 6u);
    if (lines.size() == 6u) {
        CHECK(lines[0].find("separate program, multimon-ng 1.4.1 (GPL-2.0-only)") != std::string::npos);
        CHECK(lines[1] == "Source code: https://example.invalid/mm/tree/" + std::string(40, 'a'));
        CHECK(lines[2] == "Files that will be installed beside the plugin:");
        CHECK(lines[3].find("multimon-ng.exe (1 KB)") == 0u || lines[3].find("multimon-ng.exe (") == 0u);
    }
    CHECK(!hs::consentSentence(es[0]).empty());
    PluginCatalogEntry plain;
    CHECK(hs::rowLine(plain).empty() && hs::detailLines(plain).empty() && hs::consentSentence(plain).empty());
    fs::remove_all(dir);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_plugin_repo_helper\n");
#if defined(_WIN32)
    g_dir = fs::temp_directory_path() / ("foxsdr_wc_helper_" + std::to_string(_getpid()));
#else
    g_dir = fs::temp_directory_path() / ("foxsdr_wc_helper_" + std::to_string(getpid()));
#endif
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir, ec);
    testParse();
    testInstall();
    testAllOrNothing();
    testUpdateAndRemove();
    testSiteAndWords();
    PluginRepo::setTransportForTest(nullptr);
    fs::remove_all(g_dir, ec);
    return testSummary("test_plugin_repo_helper");
}
