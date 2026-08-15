// Tests for core/plugin_repo.{hpp,cpp} - the plugin catalogue client.
//
// TEST STRATEGY, and the honest gap.
//
// This module is a code-execution path: it moves a native DLL from the
// internet into a directory the host will LoadLibrary. Its header states
// seven security rules; this file proves every one of them that can be proven
// without a live HTTPS server, which is all of them except the transport
// itself:
//
//   rule 1 (https only)          -> isHttpsUrl, parseIndex http:// refusal,
//                                   install refusing a http:// entry before
//                                   it creates so much as a directory
//   rule 2 (sha256 mandatory)    -> parseIndex refusals for missing/short/
//                                   non-hex hashes; the CNG wrapper checked
//                                   against published SHA-256 vectors; and
//                                   sha256Matches, the exact comparison
//                                   install() uses to decide
//   rule 3 (no cross-host 3xx)   -> NOT provable offline; see the gap below
//   rule 4 (hard byte cap)       -> the constants are asserted; the enforcing
//                                   read loop is transport-side
//   rule 5 (exact ABI)           -> parseIndex compatibility marking, and
//                                   install refusing an incompatible entry
//   rule 6 (filename sanitising) -> the exhaustive traversal table below
//   rule 7 (no auto-install)     -> a freshly parsed catalogue writes nothing;
//                                   asserted by checking no directory appears
//
// THE GAP, stated plainly: the WinHTTP transport (certificate validation, the
// cross-host redirect refusal, the byte cap's read loop, and install()'s call
// to sha256Matches on real downloaded bytes) needs a server presenting a
// valid certificate, which a unit test cannot stand up. What IS exercised
// offline is that install() reaches the network only after every validation
// gate has passed, that it creates its destination directory and leaves no
// temp debris when the transfer fails, and that the comparison function it
// verifies with is correct. Set CASCADE_TEST_LIVE_CATALOGUE=1 to additionally
// fetch the real index over the internet; unset (the default, and the
// catalogue repository is private) that section is skipped with a note.
//
// Temp policy follows test_plugin_host.cpp / test_recorder.cpp: per-case
// directories named with the process id, under the test's working directory
// (build-<slug>/tests, gitignored), removed on success and left behind on
// failure for autopsy.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/plugin_repo.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#define TEST_GETPID _getpid
#else
#include <unistd.h>
#define TEST_GETPID getpid
#endif

#include "test_check.hpp"

namespace fs = std::filesystem;

using cascade::core::BlockedPlugin;
using cascade::core::CachedPolicy;
using cascade::core::InstalledPlugin;
using cascade::core::PluginBlockReason;
using cascade::core::PluginCatalogEntry;
using cascade::core::PluginInventory;
using cascade::core::PluginPlatform;
using cascade::core::PluginRepo;
using cascade::core::PluginUpdate;

namespace {

const char* kHashA = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
const char* kHashB = "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";

std::string abiText() {
    return std::to_string(static_cast<unsigned>(CASCADE_PLUGIN_ABI_VERSION));
}

fs::path tmpDir(const char* tag) {
    fs::path d = fs::path("plugin_repo_" + std::to_string(TEST_GETPID()) + "_" + tag);
    std::error_code ec;
    fs::remove_all(d, ec);
    return d;  // deliberately NOT created: several cases assert it is absent
}

void writeText(const fs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string readAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

int signOf(int v) { return v < 0 ? -1 : (v > 0 ? 1 : 0); }

std::size_t countEntries(const fs::path& dir) {
    std::size_t n = 0;
    std::error_code ec;
    for (auto it = fs::directory_iterator(dir, ec); it != fs::directory_iterator(); ++it) {
        ++n;
    }
    return n;
}

// One entry with a single platform record for THIS host, so install() gets
// past the platform lookup and lands on whichever gate the case is probing.
PluginCatalogEntry makeEntry(const std::string& file, const std::string& url,
                             const std::string& sha, std::uint32_t abi) {
    PluginCatalogEntry e;
    e.id = "probe";
    e.name = "Probe";
    e.version = "1.0.0";
    e.abiVersion = abi;
    e.compatible = (abi == static_cast<std::uint32_t>(CASCADE_PLUGIN_ABI_VERSION));
    PluginPlatform p;
    p.os = PluginRepo::hostOs();
    p.arch = PluginRepo::hostArch();
    p.file = file;
    p.url = url;
    p.sha256 = sha;
    e.platforms.push_back(p);
    return e;
}

// A catalogue entry for the update/retirement cases: this host's platform,
// a valid https url and a well-formed hash, so only the field under test
// decides the outcome.
PluginCatalogEntry catEntry(const std::string& id, const std::string& version,
                            std::uint32_t abi = CASCADE_PLUGIN_ABI_VERSION,
                            const std::string& file = "probe.dll") {
    PluginCatalogEntry e = makeEntry(file, "https://example.invalid/probe.dll", kHashA, abi);
    e.id = id;
    e.name = id + " decoder";
    e.version = version;
    return e;
}

// A manifest record as it would look after installing `version` of `id`.
InstalledPlugin installedRec(const std::string& id, const std::string& version,
                             std::uint32_t abi = CASCADE_PLUGIN_ABI_VERSION,
                             const std::string& file = "probe.dll") {
    InstalledPlugin r;
    r.id = id;
    r.name = id + " decoder";
    r.version = version;
    r.file = file;
    r.sha256 = kHashA;
    r.abiVersion = abi;
    r.installedAtUnix = 1755200000;
    return r;
}

CachedPolicy policy(const std::string& id, const std::string& floorVersion,
                    const std::string& catalogueVersion) {
    CachedPolicy p;
    p.id = id;
    p.known = true;
    p.minSupportedVersion = floorVersion;
    p.catalogueVersion = catalogueVersion;
    p.abiVersion = CASCADE_PLUGIN_ABI_VERSION;
    return p;
}

// A catalogue that exercises every field, both compatibility outcomes, an
// entry with no platforms at all, an entry with no build for this host, and
// unknown fields at every level (forward compatibility).
std::string goodIndex() {
    return std::string(R"JSON({
  "schemaVersion": 1,
  "generatedAt": "2026-08-15T00:00:00Z",
  "futureTopLevelField": {"anything": [1, 2, 3]},
  "plugins": [
    {
      "id": "pocsag",
      "name": "POCSAG decoder",
      "version": "1.2.0",
      "author": "Example Author",
      "licence": "MIT",
      "summary": "Decodes POCSAG pager traffic.",
      "description": "A longer paragraph about POCSAG.",
      "homepage": "https://example.invalid/pocsag",
      "legalNotice": "Listening may be restricted where you live.",
      "minSupportedVersion": "1.1.0",
      "abiVersion": )JSON") +
           abiText() + R"JSON(,
      "futureEntryField": 42,
      "platforms": [
        {
          "os": "windows",
          "arch": "x64",
          "file": "cascade_pocsag.dll",
          "url": "https://example.invalid/cascade_pocsag.dll",
          "sha256": ")JSON" +
           kHashA + R"JSON(",
          "sizeBytes": 123456,
          "futurePlatformField": "ignored"
        },
        {
          "os": "linux",
          "arch": "x64",
          "file": "libcascade_pocsag.so",
          "url": "https://example.invalid/libcascade_pocsag.so",
          "sha256": ")JSON" +
           kHashB + R"JSON("
        }
      ]
    },
    {
      "id": "oldabi",
      "name": "Built for a different ABI",
      "version": "0.1.0",
      "abiVersion": 999,
      "platforms": [
        {
          "os": "windows",
          "arch": "x64",
          "file": "oldabi.dll",
          "url": "https://example.invalid/oldabi.dll",
          "sha256": ")JSON" +
           kHashB + R"JSON("
        }
      ]
    },
    {
      "id": "announced",
      "name": "Announced, not built yet",
      "version": "0.0.1",
      "abiVersion": )JSON" +
           abiText() + R"JSON(
    },
    {
      "id": "otherhost",
      "name": "No build for this host",
      "version": "2.0.0",
      "abiVersion": )JSON" +
           abiText() + R"JSON(,
      "platforms": [
        {
          "os": "plan9",
          "arch": "sparc",
          "file": "elsewhere.dll",
          "url": "https://example.invalid/elsewhere.dll",
          "sha256": ")JSON" +
           kHashA + R"JSON("
        }
      ]
    }
  ]
}
)JSON";
}

// Convenience: assert a document is refused, and that nothing leaks into
// `out` on the failure path.
void expectRefused(const std::string& doc, const char* tag) {
    std::vector<PluginCatalogEntry> v;
    std::string err = "stale";
    const bool ok = PluginRepo::parseIndex(doc, v, err);
    if (ok) {
        std::printf("FAIL parseIndex accepted a document it must refuse: %s\n", tag);
    }
    CHECK(!ok);
    CHECK(!err.empty());
    CHECK(v.empty());
}

void expectBadName(const std::string& name, const char* tag) {
    std::string out = "stale";
    std::string err;
    const bool ok = PluginRepo::sanitiseFileName(name, out, err);
    if (ok) {
        std::printf("FAIL sanitiseFileName accepted a dangerous name: %s\n", tag);
    }
    CHECK(!ok);
    CHECK(!err.empty());
    CHECK(out.empty());
}

}  // namespace

int main() {
    // ---------------------------------------------------------------------
    // The published origin, and the portable https gate (rule 1)
    // ---------------------------------------------------------------------
    {
        const std::string u = PluginRepo::defaultIndexUrl();
        CHECK(u.rfind("https://", 0) == 0);
        CHECK(u.find("foxsdr-plugins") != std::string::npos);
        CHECK(u.ends_with("index.json"));
        CHECK(PluginRepo::isHttpsUrl(u));

        CHECK(!PluginRepo::isHttpsUrl("http://example.invalid/index.json"));
        CHECK(!PluginRepo::isHttpsUrl("HTTP://example.invalid/index.json"));
        CHECK(!PluginRepo::isHttpsUrl("ftp://example.invalid/index.json"));
        CHECK(!PluginRepo::isHttpsUrl("file:///C:/evil.dll"));
        CHECK(!PluginRepo::isHttpsUrl(""));
        CHECK(!PluginRepo::isHttpsUrl("https://"));            // scheme only
        CHECK(!PluginRepo::isHttpsUrl(" https://example.com"));  // no leading slack
        CHECK(PluginRepo::isHttpsUrl("HTTPS://example.invalid/x"));  // scheme is ci
    }

    // ---------------------------------------------------------------------
    // parseIndex: the good document
    // ---------------------------------------------------------------------
    {
        std::vector<PluginCatalogEntry> v;
        std::string err = "stale";
        CHECK(PluginRepo::parseIndex(goodIndex(), v, err));
        CHECK(err.empty());
        CHECK(v.size() == 4u);

        if (v.size() == 4u) {
            const PluginCatalogEntry& a = v[0];
            CHECK(a.id == "pocsag");
            CHECK(a.name == "POCSAG decoder");
            CHECK(a.version == "1.2.0");
            CHECK(a.author == "Example Author");
            CHECK(a.licence == "MIT");
            CHECK(a.summary == "Decodes POCSAG pager traffic.");
            CHECK(a.description == "A longer paragraph about POCSAG.");
            CHECK(a.homepage == "https://example.invalid/pocsag");
            CHECK(a.legalNotice == "Listening may be restricted where you live.");
            CHECK(a.abiVersion == static_cast<std::uint32_t>(CASCADE_PLUGIN_ABI_VERSION));
            CHECK(a.compatible);  // rule 5, matching side
            // P10: the retirement floor is carried off the wire...
            CHECK(a.minSupportedVersion == "1.1.0");
            CHECK(a.platforms.size() == 2u);

            // thisPlatform picks the record for the host we are running on.
            const PluginPlatform* p = a.thisPlatform();
            CHECK(p != nullptr);
            if (p != nullptr) {
                CHECK(p->os == "windows");
                CHECK(p->arch == "x64");
                CHECK(p->file == "cascade_pocsag.dll");
                CHECK(p->url == "https://example.invalid/cascade_pocsag.dll");
                CHECK(p->sha256 == kHashA);
                CHECK(p->sizeBytes == 123456u);
            }

            // ...and its ABSENCE is the normal case, which must read as "no
            // floor" and never as "retire everything".
            CHECK(v[1].minSupportedVersion.empty());
            CHECK(v[2].minSupportedVersion.empty());

            // rule 5, mismatching side: listed, but marked incompatible.
            CHECK(v[1].id == "oldabi");
            CHECK(v[1].abiVersion == 999u);
            CHECK(!v[1].compatible);

            // "platforms" absent entirely is a legitimate state.
            CHECK(v[2].id == "announced");
            CHECK(v[2].platforms.empty());
            CHECK(v[2].thisPlatform() == nullptr);
            CHECK(v[2].compatible);

            // Platforms present, but none for this host.
            CHECK(v[3].id == "otherhost");
            CHECK(v[3].platforms.size() == 1u);
            CHECK(v[3].thisPlatform() == nullptr);

            // Optional strings default to empty rather than to junk.
            CHECK(v[2].author.empty());
            CHECK(v[2].licence.empty());
            CHECK(v[2].homepage.empty());
        }
    }

    // ---------------------------------------------------------------------
    // The case that actually happens: the ABI moved from 1 to 2, so every
    // catalogue entry still advertising abiVersion 1 must now be listed and
    // marked INCOMPATIBLE, and install() must refuse it before touching the
    // network. Written against the literal 1 rather than
    // "CASCADE_PLUGIN_ABI_VERSION - 1" because the retired version is a fact
    // about the published catalogue, not an expression.
    // ---------------------------------------------------------------------
    {
        static_assert(CASCADE_PLUGIN_ABI_VERSION == 2,
                      "ABI moved past 2: revisit the v1-catalogue-entry test");

        std::vector<PluginCatalogEntry> v;
        std::string err = "stale";
        const std::string doc = std::string(R"JSON({"schemaVersion":1,"plugins":[
            {"id":"v1plugin","name":"Built for ABI 1","version":"1.0.0","abiVersion":1,
             "platforms":[{"os":")JSON") +
                                PluginRepo::hostOs() + R"JSON(","arch":")JSON" +
                                PluginRepo::hostArch() + R"JSON(","file":"v1plugin.dll",
               "url":"https://example.invalid/v1plugin.dll","sha256":")JSON" +
                                kHashA + R"JSON("}]},
            {"id":"v2plugin","name":"Built for ABI 2","version":"1.0.0","abiVersion":)JSON" +
                                abiText() + R"JSON(,
             "platforms":[{"os":")JSON" +
                                PluginRepo::hostOs() + R"JSON(","arch":")JSON" +
                                PluginRepo::hostArch() + R"JSON(","file":"v2plugin.dll",
               "url":"https://example.invalid/v2plugin.dll","sha256":")JSON" +
                                kHashB + R"JSON("}]}]})JSON";

        // A v1 entry is NOT a parse error: it must still be listed, so the UI
        // can explain why it cannot be installed instead of the plugin simply
        // vanishing from the catalogue after a host upgrade.
        CHECK(PluginRepo::parseIndex(doc, v, err));
        CHECK(err.empty());
        CHECK(v.size() == 2u);
        if (v.size() == 2u) {
            CHECK(v[0].id == "v1plugin");
            CHECK(v[0].abiVersion == 1u);
            CHECK(!v[0].compatible);                // the whole point
            CHECK(v[0].thisPlatform() != nullptr);  // it does have a build for us
            CHECK(v[1].id == "v2plugin");
            CHECK(v[1].abiVersion == 2u);
            CHECK(v[1].compatible);

            // install() re-derives the decision from abiVersion, refuses
            // before any filesystem or network activity, and names BOTH
            // versions in the message.
            PluginRepo repo;
            std::string installed = "stale";
            std::string ierr;
            const fs::path dir = tmpDir("v1entry");  // absent by construction
            CHECK(!repo.install(v[0], dir.string(), installed, ierr));
            CHECK(!ierr.empty());
            CHECK(ierr.find("1") != std::string::npos);
            CHECK(ierr.find("requires exactly 2") != std::string::npos);
            CHECK(!fs::exists(dir));  // nothing was created
            std::printf("  v1 catalogue entry refused: %s\n", ierr.c_str());
        }
    }

    // Case-insensitive os/arch matching, and a sha256 uppercased by the
    // catalogue author, are both accepted and normalised.
    {
        std::vector<PluginCatalogEntry> v;
        std::string err;
        const std::string doc = std::string(R"JSON({"schemaVersion":1,"plugins":[
            {"id":"x","name":"X","version":"1","abiVersion":)JSON") +
                                abiText() + R"JSON(,"platforms":[
              {"os":"Windows","arch":"X64","file":"x.dll",
               "url":"https://example.invalid/x.dll","sha256":")JSON" +
                                "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF" +
                                R"JSON("}]}]})JSON";
        CHECK(PluginRepo::parseIndex(doc, v, err));
        CHECK(v.size() == 1u);
        if (v.size() == 1u) {
            CHECK(v[0].thisPlatform() != nullptr);
            if (v[0].thisPlatform() != nullptr) {
                CHECK(v[0].thisPlatform()->sha256 == kHashA);  // folded to lower case
            }
        }
    }

    // An empty catalogue is valid and parses to zero entries.
    {
        std::vector<PluginCatalogEntry> v;
        std::string err = "stale";
        CHECK(PluginRepo::parseIndex("{\"schemaVersion\":1,\"plugins\":[]}", v, err));
        CHECK(err.empty());
        CHECK(v.empty());
    }

    // ---------------------------------------------------------------------
    // parseIndex: everything it must refuse
    // ---------------------------------------------------------------------
    {
        const std::string entryOpen =
            std::string(R"JSON({"schemaVersion":1,"plugins":[{"id":"x","name":"X",)JSON") +
            R"JSON("version":"1","abiVersion":)JSON" + abiText() + R"JSON(,"platforms":[{)JSON";
        const std::string entryClose = R"JSON(}]}]})JSON";

        expectRefused("", "empty document");
        expectRefused("{ this is not json", "malformed JSON");
        expectRefused("[1,2,3]", "array root");
        expectRefused("42", "number root");
        expectRefused("\"a string\"", "string root");
        expectRefused("{}", "no schemaVersion");
        expectRefused("{\"schemaVersion\":2,\"plugins\":[]}", "schemaVersion 2");
        expectRefused("{\"schemaVersion\":0,\"plugins\":[]}", "schemaVersion 0");
        expectRefused("{\"schemaVersion\":\"1\",\"plugins\":[]}", "schemaVersion string");
        expectRefused("{\"schemaVersion\":1}", "no plugins key");
        expectRefused("{\"schemaVersion\":1,\"plugins\":{}}", "plugins is an object");
        expectRefused("{\"schemaVersion\":1,\"plugins\":\"none\"}", "plugins is a string");
        expectRefused("{\"schemaVersion\":1,\"plugins\":[42]}", "entry is not an object");
        expectRefused("{\"schemaVersion\":1,\"plugins\":[{\"name\":\"X\",\"version\":\"1\"}]}",
                      "entry with no id");
        expectRefused("{\"schemaVersion\":1,\"plugins\":[{\"id\":\"\",\"name\":\"X\","
                      "\"version\":\"1\"}]}",
                      "entry with an empty id");
        expectRefused("{\"schemaVersion\":1,\"plugins\":[{\"id\":7,\"name\":\"X\","
                      "\"version\":\"1\"}]}",
                      "entry whose id is a number");
        expectRefused("{\"schemaVersion\":1,\"plugins\":[{\"id\":\"x\",\"name\":\"X\","
                      "\"version\":\"1\",\"platforms\":\"none\"}]}",
                      "platforms is not an array");
        expectRefused("{\"schemaVersion\":1,\"plugins\":[{\"id\":\"x\",\"name\":\"X\","
                      "\"version\":\"1\",\"platforms\":[3]}]}",
                      "platform is not an object");
        expectRefused("{\"schemaVersion\":1,\"plugins\":[{\"id\":\"x\",\"name\":\"X\","
                      "\"version\":\"1\",\"abiVersion\":\"one\"}]}",
                      "abiVersion is a string");
        expectRefused("{\"schemaVersion\":1,\"plugins\":[{\"id\":\"x\",\"name\":\"X\","
                      "\"version\":\"1\",\"abiVersion\":-1}]}",
                      "abiVersion is negative");
        // A floor is a retirement decision; a mistyped one is refused rather
        // than coerced, because both readings of it are wrong in a bad way.
        expectRefused("{\"schemaVersion\":1,\"plugins\":[{\"id\":\"x\",\"name\":\"X\","
                      "\"version\":\"1\",\"minSupportedVersion\":2}]}",
                      "minSupportedVersion is a number");

        // rule 2: the hash is mandatory and must be well formed.
        expectRefused(entryOpen +
                          R"JSON("os":"windows","arch":"x64","file":"x.dll",
                                  "url":"https://example.invalid/x.dll")JSON" +
                          entryClose,
                      "platform with no sha256");
        expectRefused(entryOpen +
                          R"JSON("os":"windows","arch":"x64","file":"x.dll",
                                  "url":"https://example.invalid/x.dll","sha256":"")JSON" +
                          entryClose,
                      "platform with an empty sha256");
        expectRefused(entryOpen +
                          R"JSON("os":"windows","arch":"x64","file":"x.dll",
                                  "url":"https://example.invalid/x.dll","sha256":"abc123")JSON" +
                          entryClose,
                      "sha256 too short");
        expectRefused(entryOpen +
                          R"JSON("os":"windows","arch":"x64","file":"x.dll",
                                  "url":"https://example.invalid/x.dll","sha256":")JSON" +
                          std::string(kHashA) + "0" + R"JSON(")JSON" + entryClose,
                      "sha256 too long (65)");
        expectRefused(entryOpen +
                          R"JSON("os":"windows","arch":"x64","file":"x.dll",
                                  "url":"https://example.invalid/x.dll","sha256":")JSON" +
                          std::string(63, 'a') + "z" + R"JSON(")JSON" + entryClose,
                      "sha256 with a non-hex digit");
        expectRefused(entryOpen +
                          R"JSON("os":"windows","arch":"x64","file":"x.dll",
                                  "url":"https://example.invalid/x.dll","sha256":12345)JSON" +
                          entryClose,
                      "sha256 is a number");

        // rule 1: a plain-text download URL poisons the whole document.
        expectRefused(entryOpen +
                          R"JSON("os":"windows","arch":"x64","file":"x.dll",
                                  "url":"http://example.invalid/x.dll","sha256":")JSON" +
                          std::string(kHashA) + R"JSON(")JSON" + entryClose,
                      "http:// download url");
        expectRefused(entryOpen +
                          R"JSON("os":"windows","arch":"x64","file":"x.dll",
                                  "url":"ftp://example.invalid/x.dll","sha256":")JSON" +
                          std::string(kHashA) + R"JSON(")JSON" + entryClose,
                      "ftp:// download url");
        expectRefused(entryOpen +
                          R"JSON("os":"windows","arch":"x64","file":"x.dll",
                                  "url":"","sha256":")JSON" +
                          std::string(kHashA) + R"JSON(")JSON" + entryClose,
                      "empty download url");

        // Required platform fields.
        expectRefused(entryOpen +
                          R"JSON("os":"windows","arch":"x64",
                                  "url":"https://example.invalid/x.dll","sha256":")JSON" +
                          std::string(kHashA) + R"JSON(")JSON" + entryClose,
                      "platform with no file");
        expectRefused(entryOpen +
                          R"JSON("os":"windows","file":"x.dll",
                                  "url":"https://example.invalid/x.dll","sha256":")JSON" +
                          std::string(kHashA) + R"JSON(")JSON" + entryClose,
                      "platform with no arch");
        expectRefused(entryOpen +
                          R"JSON("arch":"x64","file":"x.dll",
                                  "url":"https://example.invalid/x.dll","sha256":")JSON" +
                          std::string(kHashA) + R"JSON(")JSON" + entryClose,
                      "platform with no os");
        expectRefused(entryOpen +
                          R"JSON("os":"windows","arch":"x64","file":"x.dll",
                                  "url":"https://example.invalid/x.dll","sha256":")JSON" +
                          std::string(kHashA) + R"JSON(","sizeBytes":-5)JSON" + entryClose,
                      "negative sizeBytes");
    }

    // ---------------------------------------------------------------------
    // rule 6: filename sanitising, the path-traversal guard
    // ---------------------------------------------------------------------
    {
        // Accepted: ordinary, well-behaved names.
        const char* good[] = {"good_name-1.0.dll", "a.dll", "cascade_pocsag.dll",
                              "Plugin.DLL", "x1-2_3.4.dll"};
        for (const char* g : good) {
            std::string out;
            std::string err;
            const bool ok = PluginRepo::sanitiseFileName(g, out, err);
            if (!ok) {
                std::printf("FAIL sanitiseFileName rejected a good name %s: %s\n", g,
                            err.c_str());
            }
            CHECK(ok);
            CHECK(out == g);
            CHECK(err.empty());
        }

        // Refused: the traversal table.
        expectBadName("..\\evil.dll", "windows parent traversal");
        expectBadName("../evil.dll", "posix parent traversal");
        expectBadName("..\\..\\..\\windows\\system32\\evil.dll", "deep traversal");
        expectBadName("C:\\evil.dll", "absolute windows path");
        expectBadName("C:/evil.dll", "absolute windows path, forward slashes");
        expectBadName("/etc/evil.dll", "absolute posix path");
        expectBadName("\\\\server\\share\\evil.dll", "UNC path");
        expectBadName("sub/dir.dll", "relative subdirectory");
        expectBadName("sub\\dir.dll", "relative subdirectory, backslash");
        expectBadName("a:b.dll", "NTFS alternate data stream");
        expectBadName("good.dll:hidden.dll", "ADS appended to a good name");
        expectBadName("no-extension", "no extension at all");
        expectBadName("evil.dll.exe", "executable masquerading as a dll");
        expectBadName("evil.exe", "wrong extension");
        expectBadName(".dll", "extension only");
        expectBadName("", "empty");
        expectBadName("....", "only dots");
        expectBadName("...dll", "dots then dll");
        expectBadName("..dll", "two dots then dll");
        expectBadName(".hidden.dll", "leading dot");
        expectBadName("-switch.dll", "leading dash");
        expectBadName(std::string(296, 'a') + ".dll", "300 characters");
        expectBadName("evil dll.dll", "embedded space");
        expectBadName("evil\tdll.dll", "embedded tab");
        expectBadName("%APPDATA%.dll", "environment syntax");
        expectBadName("evil$.dll", "share syntax");
        expectBadName("ev*l.dll", "wildcard");
        expectBadName("\"quoted\".dll", "quotes");
        expectBadName(std::string("\xC3\xA9") + "vil.dll", "non-ASCII byte");
        expectBadName("CON.dll", "reserved device name");
        expectBadName("nul.dll", "reserved device name, lower case");
        expectBadName("COM1.dll", "reserved serial device");
        expectBadName("LPT9.dll", "reserved printer device");
        // The bound itself: 128 characters is accepted, 129 is not.
        {
            std::string out;
            std::string err;
            CHECK(PluginRepo::sanitiseFileName(std::string(124, 'a') + ".dll", out, err));
            CHECK(out.size() == PluginRepo::kMaxFileNameChars);
        }
        expectBadName(std::string(125, 'a') + ".dll", "one over the length bound");
    }

    // ---------------------------------------------------------------------
    // rule 2: SHA-256 against published vectors, then over a file
    // ---------------------------------------------------------------------
#ifdef _WIN32
    {
        // FIPS 180-4 / NIST published vectors. These are the evidence that the
        // integrity check is a real SHA-256 and not a stand-in.
        std::string hex;
        std::string err = "stale";
        CHECK(PluginRepo::sha256Hex(nullptr, 0, hex, err));
        CHECK(err.empty());
        CHECK(hex == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

        CHECK(PluginRepo::sha256Hex("abc", 3, hex, err));
        CHECK(hex == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

        const char* v448 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        CHECK(PluginRepo::sha256Hex(v448, 56, hex, err));
        CHECK(hex == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

        // A block-boundary case (64 bytes) to catch padding mistakes, and a
        // multi-block case that crosses the internal read buffer.
        const std::string sixtyFour(64, 'a');
        CHECK(PluginRepo::sha256Hex(sixtyFour.data(), sixtyFour.size(), hex, err));
        CHECK(hex == "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");

        const std::string million(1000000, 'a');
        CHECK(PluginRepo::sha256Hex(million.data(), million.size(), hex, err));
        CHECK(hex == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

        // The same digests through the file path, including a >64 KiB file so
        // the streaming loop is exercised.
        const fs::path d = tmpDir("hash");
        fs::create_directories(d);
        writeText(d / "abc.bin", "abc");
        CHECK(PluginRepo::sha256File((d / "abc.bin").string(), hex, err));
        CHECK(hex == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

        writeText(d / "empty.bin", "");
        CHECK(PluginRepo::sha256File((d / "empty.bin").string(), hex, err));
        CHECK(hex == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

        writeText(d / "big.bin", million);
        CHECK(PluginRepo::sha256File((d / "big.bin").string(), hex, err));
        CHECK(hex == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

        err.clear();
        CHECK(!PluginRepo::sha256File((d / "does_not_exist.bin").string(), hex, err));
        CHECK(!err.empty());
        CHECK(hex.empty());

        std::error_code ec;
        fs::remove_all(d, ec);
    }
#else
    {
        // No CNG: the wrapper must FAIL rather than pretend to have verified.
        std::string hex;
        std::string err;
        CHECK(!PluginRepo::sha256Hex("abc", 3, hex, err));
        CHECK(!err.empty());
        std::printf("note: SHA-256 vectors skipped - CNG is Windows-only\n");
    }
#endif

    // ---------------------------------------------------------------------
    // rule 2: the comparison install() actually decides on
    // ---------------------------------------------------------------------
    {
        const std::string a = kHashA;
        std::string upper;
        for (char c : a) {
            upper.push_back(c >= 'a' && c <= 'f' ? static_cast<char>(c - 'a' + 'A') : c);
        }
        CHECK(PluginRepo::sha256Matches(a, a));
        CHECK(PluginRepo::sha256Matches(a, upper));  // hex case is not meaning
        CHECK(PluginRepo::sha256Matches(upper, a));

        CHECK(!PluginRepo::sha256Matches(a, kHashB));
        // One digit different, in the first, middle and last position.
        std::string near1 = a;
        near1[0] = '1';
        CHECK(!PluginRepo::sha256Matches(a, near1));
        std::string near2 = a;
        near2[32] = (near2[32] == '0' ? '1' : '0');
        CHECK(!PluginRepo::sha256Matches(a, near2));
        std::string near3 = a;
        near3[63] = '0';
        CHECK(!PluginRepo::sha256Matches(a, near3));

        // Degenerate inputs must never read as a successful verification.
        CHECK(!PluginRepo::sha256Matches("", ""));
        CHECK(!PluginRepo::sha256Matches("unknown", "unknown"));
        CHECK(!PluginRepo::sha256Matches(a.substr(0, 63), a.substr(0, 63)));
        CHECK(!PluginRepo::sha256Matches(a, ""));
        CHECK(!PluginRepo::sha256Matches("", a));
        CHECK(!PluginRepo::sha256Matches(a, a + "0"));
        CHECK(!PluginRepo::sha256Matches(std::string(64, 'z'), std::string(64, 'z')));
    }

    // ---------------------------------------------------------------------
    // rule 4: the caps are what the header promises
    // ---------------------------------------------------------------------
    {
        // Read through runtime copies: comparing two compile-time constants
        // directly is a /W4 C4127 ("conditional expression is constant").
        std::uint64_t pluginCap = PluginRepo::kMaxPluginBytes;
        std::uint64_t indexCap = PluginRepo::kMaxIndexBytes;
        int hops = PluginRepo::kMaxRedirects;
        CHECK(pluginCap == 64ull * 1024ull * 1024ull);
        CHECK(indexCap == 4ull * 1024ull * 1024ull);
        CHECK(indexCap < pluginCap);
        CHECK(hops > 0 && hops <= 10);
    }

    // ---------------------------------------------------------------------
    // rule 7 / install(): every refusal happens before ANYTHING is written
    //
    // Each case points at a destination directory that does not exist. If the
    // refusal came after the network step (or after the directory step) the
    // directory would appear, so "still absent" is the assertion that the
    // gate ran early.
    // ---------------------------------------------------------------------
    {
        PluginRepo repo;
        CHECK(repo.entries().empty());   // rule 7: nothing is known until asked
        CHECK(repo.progress() == 0.0f);

        const std::string url = "https://example.invalid/x.dll";
        std::string installed = "stale";
        std::string err;

        struct Case {
            const char* tag;
            PluginCatalogEntry e;
        };
        std::vector<Case> cases;
        // rule 5: wrong ABI.
        cases.push_back({"abi mismatch", makeEntry("x.dll", url, kHashA, 999u)});
        // rule 6: the traversal guard, before the network.
        cases.push_back({"traversal file name", makeEntry("..\\evil.dll", url, kHashA,
                                                          CASCADE_PLUGIN_ABI_VERSION)});
        cases.push_back({"absolute file name", makeEntry("C:\\evil.dll", url, kHashA,
                                                         CASCADE_PLUGIN_ABI_VERSION)});
        cases.push_back({"wrong extension", makeEntry("evil.dll.exe", url, kHashA,
                                                      CASCADE_PLUGIN_ABI_VERSION)});
        // rule 2: a malformed hash never gets to download anything.
        cases.push_back({"short sha256",
                         makeEntry("x.dll", url, "abc123", CASCADE_PLUGIN_ABI_VERSION)});
        cases.push_back({"empty sha256",
                         makeEntry("x.dll", url, "", CASCADE_PLUGIN_ABI_VERSION)});
        cases.push_back({"non-hex sha256", makeEntry("x.dll", url, std::string(63, 'a') + "z",
                                                     CASCADE_PLUGIN_ABI_VERSION)});
        // rule 1: http:// refused before the network.
        cases.push_back({"http url", makeEntry("x.dll", "http://example.invalid/x.dll", kHashA,
                                               CASCADE_PLUGIN_ABI_VERSION)});
        cases.push_back({"file url", makeEntry("x.dll", "file:///C:/evil.dll", kHashA,
                                               CASCADE_PLUGIN_ABI_VERSION)});

        int caseIndex = 0;
        for (const Case& c : cases) {
            const fs::path dir =
                fs::path("plugin_repo_" + std::to_string(TEST_GETPID()) + "_refuse" +
                         std::to_string(caseIndex++));
            std::error_code ec;
            fs::remove_all(dir, ec);
            installed = "stale";
            err.clear();
            const bool ok = repo.install(c.e, dir.string(), installed, err);
            if (ok) {
                std::printf("FAIL install accepted what it must refuse: %s\n", c.tag);
            }
            CHECK(!ok);
            CHECK(!err.empty());
            CHECK(installed.empty());
            if (fs::exists(dir)) {
                std::printf("FAIL install touched the filesystem before refusing: %s\n", c.tag);
            }
            CHECK(!fs::exists(dir));
            fs::remove_all(dir, ec);
        }

        // No build for this host: same treatment.
        PluginCatalogEntry noHost;
        noHost.id = "x";
        noHost.name = "X";
        noHost.abiVersion = CASCADE_PLUGIN_ABI_VERSION;
        PluginPlatform other;
        other.os = "plan9";
        other.arch = "sparc";
        other.file = "x.dll";
        other.url = url;
        other.sha256 = kHashA;
        noHost.platforms.push_back(other);
        const fs::path dir2 = tmpDir("nohost");
        CHECK(!repo.install(noHost, dir2.string(), installed, err));
        CHECK(!err.empty());
        CHECK(!fs::exists(dir2));

        // An entry with no platforms at all: same again.
        PluginCatalogEntry none;
        none.id = "x";
        none.name = "X";
        none.abiVersion = CASCADE_PLUGIN_ABI_VERSION;
        const fs::path dir3 = tmpDir("noplatforms");
        CHECK(!repo.install(none, dir3.string(), installed, err));
        CHECK(!fs::exists(dir3));

        // install() must NOT trust a hand-set `compatible` flag over the
        // abiVersion it was built from.
        PluginCatalogEntry lying = makeEntry("x.dll", url, kHashA, 999u);
        lying.compatible = true;
        const fs::path dir4 = tmpDir("lying");
        CHECK(!repo.install(lying, dir4.string(), installed, err));
        CHECK(!fs::exists(dir4));
    }

    // ---------------------------------------------------------------------
    // install(): the destination directory IS created once validation passes,
    // and a failed transfer leaves nothing behind.
    //
    // The URL points at 127.0.0.1:1, which nothing listens on, so the
    // connection is refused locally - no external network is used, and no
    // certificate can be presented. That is enough to prove the ordering
    // (validate -> create directory -> network) and the no-debris rule.
    // ---------------------------------------------------------------------
#ifdef _WIN32
    {
        PluginRepo repo;
        const fs::path dir = tmpDir("transport");
        CHECK(!fs::exists(dir));

        // cancel() before the operation must not poison it: install() clears
        // the flag on entry, so the failure below is a transport failure and
        // never "cancelled".
        repo.cancel();

        const PluginCatalogEntry e = makeEntry("probe.dll", "https://127.0.0.1:1/probe.dll",
                                               kHashA, CASCADE_PLUGIN_ABI_VERSION);
        std::string installed = "stale";
        std::string err;
        const bool ok = repo.install(e, dir.string(), installed, err);
        CHECK(!ok);
        CHECK(!err.empty());
        CHECK(err != "cancelled");
        CHECK(installed.empty());
        CHECK(fs::is_directory(dir));       // created, as documented
        CHECK(countEntries(dir) == 0u);     // and the ".part" temp is gone
        CHECK(!fs::exists(dir / "probe.dll"));

        std::error_code ec;
        fs::remove_all(dir, ec);
    }
#endif

    // ---------------------------------------------------------------------
    // fetchIndex(): the https gate, before any socket
    // ---------------------------------------------------------------------
    {
        PluginRepo repo;
        std::string err = "stale";
        CHECK(!repo.fetchIndex("http://example.invalid/index.json", err));
        CHECK(!err.empty());
        CHECK(repo.entries().empty());

        err.clear();
        CHECK(!repo.fetchIndex("", err));
        CHECK(!err.empty());

        err.clear();
        CHECK(!repo.fetchIndex("ftp://example.invalid/index.json", err));
        CHECK(!err.empty());
    }

    // ---------------------------------------------------------------------
    // remove(): the same sanitiser guards deletion
    // ---------------------------------------------------------------------
    {
        PluginRepo repo;
        const fs::path dir = tmpDir("remove");
        fs::create_directories(dir);
        writeText(dir / "keep.dll", "not really a dll");
        writeText(dir / "goaway.dll", "not really a dll either");

        // A neighbour OUTSIDE the plugins directory, which a traversal would
        // reach if the sanitiser were not there.
        const fs::path outside = dir / ".." / ("plugin_repo_" +
                                               std::to_string(TEST_GETPID()) + "_victim.dll");
        writeText(outside, "must survive");
        CHECK(fs::exists(outside));

        std::string err = "stale";
        const std::string victimName =
            "..\\plugin_repo_" + std::to_string(TEST_GETPID()) + "_victim.dll";
        CHECK(!repo.remove(dir.string(), victimName, err));
        CHECK(!err.empty());
        CHECK(fs::exists(outside));  // the traversal did not delete it

        CHECK(!repo.remove(dir.string(), "sub/keep.dll", err));
        CHECK(!repo.remove(dir.string(), "", err));
        CHECK(!repo.remove(dir.string(), "keep", err));
        CHECK(fs::exists(dir / "keep.dll"));

        // Not installed: a clean failure, not a crash and not a success.
        err.clear();
        CHECK(!repo.remove(dir.string(), "never_installed.dll", err));
        CHECK(!err.empty());

        // The ordinary case works.
        err = "stale";
        CHECK(repo.remove(dir.string(), "goaway.dll", err));
        CHECK(err.empty());
        CHECK(!fs::exists(dir / "goaway.dll"));
        CHECK(fs::exists(dir / "keep.dll"));

        // Removing it twice reports the second attempt honestly.
        CHECK(!repo.remove(dir.string(), "goaway.dll", err));

        std::error_code ec;
        fs::remove(outside, ec);
        fs::remove_all(dir, ec);
    }

    // =====================================================================
    // P10: version comparison, the manifest, retirement, update planning
    // =====================================================================

    // ---------------------------------------------------------------------
    // compareVersions. The table IS the specification; every rule in the
    // header has a row, and the whole table is re-run backwards to prove the
    // ordering is antisymmetric (a comparator that is not cannot be trusted
    // to say "never downgrade").
    // ---------------------------------------------------------------------
    {
        struct VCase {
            const char* a;
            const char* b;
            int want;
        };
        const VCase table[] = {
            // Ordinary releases.
            {"1.0.0", "1.0.1", -1},
            {"1.0.1", "1.0.0", 1},
            {"1.0.0", "1.0.0", 0},
            {"1.0.0", "2.0.0", -1},
            {"1.1.0", "1.0.9", 1},
            // THE CLASSIC BUG. Byte-wise ordering puts "1.10.0" BEFORE
            // "1.9.0" because '1' < '9'. If these two rows are green, the
            // comparator is doing arithmetic and not strcmp.
            {"1.10.0", "1.9.0", 1},
            {"1.9.0", "1.10.0", -1},
            {"2.0.0", "10.0.0", -1},
            {"1.0.9", "1.0.10", -1},
            {"9.0.0", "10.0.0", -1},
            // Differing segment counts: the missing tail is zero.
            {"1.2", "1.2.0", 0},
            {"1.2.0", "1.2", 0},
            {"1.2", "1.2.1", -1},
            {"1.2.1", "1.2", 1},
            {"1.2.0.0.0", "1.2", 0},
            {"1", "1.0.0", 0},
            // Empty and degenerate segments.
            {"1.2.", "1.2", 0},
            {"1..2", "1.0.2", 0},
            {"", "", 0},
            {"", "0", 0},
            {"", "0.0.0", 0},
            {"", "1.0.0", -1},
            {"1.0.0", "", 1},
            {".", "0.0", 0},
            // Leading zeros are formatting, not value.
            {"01.02.03", "1.2.3", 0},
            {"1.00", "1.0", 0},
            {"0001", "1", 0},
            {"1.007", "1.7", 0},
            {"1.010", "1.9", 1},  // ten, not "010" < "9"
            // Numeric outranks non-numeric, in both directions.
            {"1.0.0", "1.0.0-rc1", 1},
            {"1.0.0-rc1", "1.0.0", -1},
            {"1.0.0-rc1", "1.0.0-rc2", -1},
            {"1.0.0-rc2", "1.0.0-rc10", 1},  // byte-wise inside a non-numeric segment
            {"garbage", "1.0.0", -1},
            {"1.0.0", "garbage", 1},
            {"garbage", "garbage", 0},
            {"v1.2.0", "1.2.0", -1},
            {"v1.2.0", "v1.2.0", 0},
            {"1.2.0", "1.2.0beta", 1},
            // Numbers far past any integer type. No conversion happens, so
            // there is nothing to overflow.
            {"99999999999999999999999999999999", "99999999999999999999999999999998", 1},
            {"18446744073709551616", "18446744073709551615", 1},  // uint64 max + 1
            {"18446744073709551615", "18446744073709551615", 0},
            {"1.99999999999999999999999999999999", "1.100000000000000000000000000000000", -1},
            {"340282366920938463463374607431768211456", "1.0.0", 1},
        };
        for (const VCase& c : table) {
            const int got = signOf(PluginRepo::compareVersions(c.a, c.b));
            if (got != c.want) {
                std::printf("FAIL compareVersions(\"%s\", \"%s\") = %d, want %d\n", c.a, c.b, got,
                            c.want);
            }
            CHECK(got == c.want);
            // Antisymmetry, for free, on every row.
            const int rev = signOf(PluginRepo::compareVersions(c.b, c.a));
            if (rev != -c.want) {
                std::printf("FAIL compareVersions is not antisymmetric for \"%s\"/\"%s\"\n", c.a,
                            c.b);
            }
            CHECK(rev == -c.want);
        }
        // Reflexivity on a few awkward values.
        CHECK(PluginRepo::compareVersions("1.2.3", "1.2.3") == 0);
        CHECK(PluginRepo::compareVersions("", "") == 0);
        CHECK(PluginRepo::compareVersions("....", "....") == 0);
    }

    // ---------------------------------------------------------------------
    // The manifest: round trip, and every way it can disagree with reality
    // ---------------------------------------------------------------------
    {
        const fs::path d = tmpDir("manifest");
        fs::create_directories(d);
        const std::string dir = d.string();

        // The manifest is a ".json" and the sanitiser only ever accepts
        // ".dll", so no catalogue entry can ever be aimed at it.
        CHECK(std::string(PluginRepo::manifestFileName()).ends_with(".json"));
        {
            std::string out;
            std::string err;
            CHECK(!PluginRepo::sanitiseFileName(PluginRepo::manifestFileName(), out, err));
        }
        CHECK(PluginRepo::manifestPath(dir).ends_with(PluginRepo::manifestFileName()));

        // --- absent manifest: not an error, and nothing is "installed" -----
        {
            PluginInventory inv;
            std::string err = "stale";
            CHECK(PluginRepo::loadInventory(dir, inv, err));  // absence is normal
            CHECK(err.empty());
            CHECK(!inv.manifestPresent);
            CHECK(!inv.manifestUsable);
            CHECK(inv.plugins.empty());
            CHECK(inv.policies.empty());
            CHECK(!inv.notes.empty());  // it says so rather than staying silent
        }

        // --- round trip through the real save/load path --------------------
        std::vector<InstalledPlugin> plugins;
        plugins.push_back(installedRec("pocsag", "1.0.2", CASCADE_PLUGIN_ABI_VERSION,
                                       "cascade_pocsag.dll"));
        plugins.push_back(installedRec("flex", "0.9.0", 7u, "cascade_flex.dll"));
        std::vector<CachedPolicy> policies;
        policies.push_back(policy("pocsag", "1.1.0", "1.2.0"));
        policies.push_back(policy("flex", "", "0.9.0"));

        std::string err = "stale";
        CHECK(PluginRepo::saveManifest(dir, plugins, policies, err));
        CHECK(err.empty());
        CHECK(fs::is_regular_file(PluginRepo::manifestPath(dir)));

        {
            std::vector<InstalledPlugin> back;
            std::vector<CachedPolicy> backPol;
            std::vector<std::string> notes;
            CHECK(PluginRepo::parseManifest(readAll(PluginRepo::manifestPath(dir)), back, backPol,
                                            notes, err));
            CHECK(err.empty());
            CHECK(notes.empty());
            CHECK(back.size() == 2u);
            if (back.size() == 2u) {
                CHECK(back[0].id == "pocsag");
                CHECK(back[0].name == "pocsag decoder");
                CHECK(back[0].version == "1.0.2");
                CHECK(back[0].file == "cascade_pocsag.dll");
                CHECK(back[0].sha256 == kHashA);
                CHECK(back[0].abiVersion == static_cast<std::uint32_t>(CASCADE_PLUGIN_ABI_VERSION));
                CHECK(back[0].installedAtUnix == 1755200000);
                CHECK(!back[0].missingFromDisk);   // reconciliation has not run
                CHECK(!back[0].digestMismatch);
                CHECK(back[1].abiVersion == 7u);
            }
            CHECK(backPol.size() == 2u);
            if (backPol.size() == 2u) {
                CHECK(backPol[0].id == "pocsag");
                CHECK(backPol[0].known);
                CHECK(backPol[0].minSupportedVersion == "1.1.0");
                CHECK(backPol[0].catalogueVersion == "1.2.0");
                CHECK(backPol[1].minSupportedVersion.empty());  // "no floor" survives
            }
        }

        // --- reconciliation: one record's file is missing, one file on disk
        //     belongs to no record, and one file is not the bytes we recorded.
        {
            // cascade_pocsag.dll: present, and NOT what the record claims.
            writeText(d / "cascade_pocsag.dll", "these are not the installed bytes");
            // cascade_flex.dll: deliberately never created -> missing.
            // stranger.dll: on disk, unrecorded -> unmanaged, left alone.
            writeText(d / "stranger.dll", "hand installed by the user");

            PluginInventory inv;
            std::string ierr = "stale";
            CHECK(PluginRepo::loadInventory(dir, inv, ierr));
            CHECK(ierr.empty());
            CHECK(inv.manifestPresent);
            CHECK(inv.manifestUsable);
            CHECK(inv.plugins.size() == 2u);
            if (inv.plugins.size() == 2u) {
                CHECK(!inv.plugins[0].missingFromDisk);
#ifdef _WIN32
                CHECK(inv.plugins[0].digestMismatch);  // re-hashed, not assumed
#endif
                CHECK(inv.plugins[1].missingFromDisk);
                CHECK(!inv.plugins[1].digestMismatch);  // absent != tampered
            }
            CHECK(inv.unmanaged.size() == 1u);
            if (inv.unmanaged.size() == 1u) {
                CHECK(inv.unmanaged[0] == "stranger.dll");
            }
            // The disagreements are SAID, not silently repaired.
            CHECK(inv.notes.size() >= 3u);
            CHECK(fs::exists(d / "stranger.dll"));  // unmanaged means untouched
            // ...and the manifest itself was not rewritten by a mere read.
            std::vector<InstalledPlugin> stillThere;
            std::vector<CachedPolicy> stillPol;
            std::vector<std::string> notes;
            CHECK(PluginRepo::parseManifest(readAll(PluginRepo::manifestPath(dir)), stillThere,
                                            stillPol, notes, err));
            CHECK(stillThere.size() == 2u);
        }

        // --- a record whose bytes DO match reports no mismatch -------------
#ifdef _WIN32
        {
            const fs::path d2 = tmpDir("manifest_ok");
            fs::create_directories(d2);
            writeText(d2 / "cascade_pocsag.dll", "abc");
            std::string hex;
            std::string herr;
            CHECK(PluginRepo::sha256File((d2 / "cascade_pocsag.dll").string(), hex, herr));
            std::vector<InstalledPlugin> recs;
            InstalledPlugin r = installedRec("pocsag", "1.0.2", CASCADE_PLUGIN_ABI_VERSION,
                                             "cascade_pocsag.dll");
            r.sha256 = hex;
            recs.push_back(r);
            CHECK(PluginRepo::saveManifest(d2.string(), recs, {}, err));
            PluginInventory inv;
            CHECK(PluginRepo::loadInventory(d2.string(), inv, err));
            CHECK(inv.plugins.size() == 1u);
            if (inv.plugins.size() == 1u) {
                CHECK(!inv.plugins[0].missingFromDisk);
                CHECK(!inv.plugins[0].digestMismatch);
            }
            CHECK(inv.unmanaged.empty());
            std::error_code ec;
            fs::remove_all(d2, ec);
        }
#endif

        // --- corrupt manifest: reported, and it must not brick anything ----
        {
            const fs::path d3 = tmpDir("manifest_corrupt");
            fs::create_directories(d3);
            writeText(d3 / "cascade_pocsag.dll", "a plugin the user is still using");
            writeText(fs::path(PluginRepo::manifestPath(d3.string())), "{ this is not json");

            PluginInventory inv;
            std::string cerr;
            CHECK(!PluginRepo::loadInventory(d3.string(), inv, cerr));  // says so
            CHECK(!cerr.empty());
            CHECK(inv.manifestPresent);
            CHECK(!inv.manifestUsable);
            CHECK(inv.plugins.empty());   // nothing is trusted from it
            CHECK(inv.policies.empty());
            CHECK(inv.unmanaged.size() == 1u);  // the dll degrades to unmanaged
            // THE POINT: a corrupt manifest FAILS OPEN. No records, no
            // policies, nothing blocked - a broken file must never take a
            // working plugin away from the user.
            CHECK(PluginRepo::blockedCount(inv.plugins, inv.policies) == 0u);

            // The other document-level refusals, all of which mean "treat as
            // absent" rather than "block everything".
            std::vector<InstalledPlugin> p;
            std::vector<CachedPolicy> q;
            std::vector<std::string> notes;
            std::string e2;
            CHECK(!PluginRepo::parseManifest("[1,2,3]", p, q, notes, e2));
            CHECK(!PluginRepo::parseManifest("42", p, q, notes, e2));
            CHECK(!PluginRepo::parseManifest("", p, q, notes, e2));
            CHECK(!PluginRepo::parseManifest("{\"schemaVersion\":2}", p, q, notes, e2));
            CHECK(!PluginRepo::parseManifest("{\"plugins\":{}}", p, q, notes, e2));
            CHECK(!e2.empty());
            // An empty object is a valid, empty manifest.
            CHECK(PluginRepo::parseManifest("{}", p, q, notes, e2));
            CHECK(p.empty());
            CHECK(q.empty());
            std::error_code ec;
            fs::remove_all(d3, ec);
        }

        // --- per-ROW damage drops the row, never the document --------------
        {
            std::vector<InstalledPlugin> p;
            std::vector<CachedPolicy> q;
            std::vector<std::string> notes;
            std::string e2 = "stale";
            const std::string doc = std::string(R"JSON({"schemaVersion":1,"plugins":[
                {"id":"good","version":"1.0.0","file":"good.dll","sha256":")JSON") +
                                    kHashA + R"JSON("},
                {"id":"noversion","file":"x.dll"},
                {"version":"1.0.0","file":"x.dll"},
                {"id":"traversal","version":"1.0.0","file":"..\\evil.dll"},
                {"id":"absolute","version":"1.0.0","file":"C:\\evil.dll"},
                {"id":"notadll","version":"1.0.0","file":"evil.exe"},
                {"id":"badhash","version":"1.0.0","file":"badhash.dll","sha256":"nope"},
                {"id":"good","version":"9.9.9","file":"dupe.dll"},
                42
            ]})JSON";
            CHECK(PluginRepo::parseManifest(doc, p, q, notes, e2));
            CHECK(e2.empty());
            // Only "good" and "badhash" survive; every dangerous or unusable
            // row is dropped WITH a note.
            CHECK(p.size() == 2u);
            if (p.size() == 2u) {
                CHECK(p[0].id == "good");
                CHECK(p[0].version == "1.0.0");  // not the duplicate's 9.9.9
                CHECK(p[1].id == "badhash");
                CHECK(p[1].sha256.empty());  // malformed digest discarded
            }
            CHECK(notes.size() >= 7u);
        }

        std::error_code ec;
        fs::remove_all(d, ec);
    }

#ifdef _WIN32
    // ---------------------------------------------------------------------
    // saveManifest is ATOMIC. Same technique as test_config.cpp: hold the
    // target open WITHOUT FILE_SHARE_DELETE, which blocks a rename but still
    // permits a plain write - so an implementation that wrote the target
    // directly would succeed here and clobber it, turning this red.
    // ---------------------------------------------------------------------
    {
        const fs::path d = tmpDir("manifest_atomic");
        fs::create_directories(d);
        const std::string dir = d.string();
        std::vector<InstalledPlugin> before;
        before.push_back(installedRec("pocsag", "1.0.2"));
        std::string err;
        CHECK(PluginRepo::saveManifest(dir, before, {policy("pocsag", "1.0.0", "1.0.2")}, err));
        const std::string origBytes = readAll(PluginRepo::manifestPath(dir));
        CHECK(!origBytes.empty());

        HANDLE h = CreateFileA(PluginRepo::manifestPath(dir).c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE);
        std::vector<InstalledPlugin> after;
        after.push_back(installedRec("pocsag", "2.0.0"));
        err.clear();
        CHECK(!PluginRepo::saveManifest(dir, after, {}, err));
        CHECK(!err.empty());
        CloseHandle(h);
        CHECK(readAll(PluginRepo::manifestPath(dir)) == origBytes);

        // Fully exclusive open: same outcome.
        HANDLE h2 = CreateFileA(PluginRepo::manifestPath(dir).c_str(), GENERIC_READ, 0, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(h2 != INVALID_HANDLE_VALUE);
        err.clear();
        CHECK(!PluginRepo::saveManifest(dir, after, {}, err));
        CloseHandle(h2);
        CHECK(readAll(PluginRepo::manifestPath(dir)) == origBytes);

        // No temp debris from either failure.
        CHECK(countEntries(d) == 1u);

        // Lock released: the same save now works and the content changes.
        CHECK(PluginRepo::saveManifest(dir, after, {}, err));
        CHECK(readAll(PluginRepo::manifestPath(dir)) != origBytes);

        std::error_code ec;
        fs::remove_all(d, ec);
    }
#endif

    // ---------------------------------------------------------------------
    // Retirement: the one predicate, and the words the user reads
    // ---------------------------------------------------------------------
    {
        const InstalledPlugin old = installedRec("pocsag", "1.0.2");

        // FAIL OPEN, the case most likely to be got wrong. No catalogue has
        // ever been seen for this id - offline user, private plugin, fresh
        // install - so it LOADS.
        {
            CachedPolicy unknown;
            unknown.id = "pocsag";
            CHECK(!unknown.known);
            CHECK(PluginRepo::pluginBlockReason(old, unknown) == PluginBlockReason::None);
            CHECK(PluginRepo::pluginBlockMessage(old, unknown).empty());
            // policyFor() on an empty cache IS that state.
            const CachedPolicy fromEmpty = PluginRepo::policyFor({}, "pocsag");
            CHECK(!fromEmpty.known);
            CHECK(PluginRepo::pluginBlockReason(old, fromEmpty) == PluginBlockReason::None);
        }
        // Known, but the catalogue published no floor: still loads.
        CHECK(PluginRepo::pluginBlockReason(old, policy("pocsag", "", "2.0.0")) ==
              PluginBlockReason::None);
        // A record with no version tells us nothing: fail open.
        {
            InstalledPlugin noVersion = old;
            noVersion.version.clear();
            CHECK(PluginRepo::pluginBlockReason(noVersion, policy("pocsag", "1.1.0", "1.2.0")) ==
                  PluginBlockReason::None);
        }

        // FAIL CLOSED: below the cached floor.
        {
            const CachedPolicy p = policy("pocsag", "1.1.0", "1.2.0");
            CHECK(PluginRepo::pluginBlockReason(old, p) == PluginBlockReason::BelowMinimumVersion);
            const std::string msg = PluginRepo::pluginBlockMessage(old, p);
            if (msg.empty()) {
                std::printf("FAIL a blocked plugin produced no message\n");
            }
            // The copy is what the user actually experiences, so it is
            // asserted like any other output: the plugin, the version they
            // have, the version they need, and what to do about it.
            CHECK(contains(msg, "pocsag decoder"));  // the name, not the id
            CHECK(contains(msg, "1.0.2"));           // what is installed
            CHECK(contains(msg, "1.1.0"));           // what is required
            CHECK(contains(msg, "1.2.0"));           // what is available
            CHECK(contains(msg, "out of date"));
            CHECK(contains(msg, "update it to use it again"));
            // No jargon in front of a radio amateur.
            CHECK(!contains(msg, "ABI"));
            CHECK(!contains(msg, "BelowMinimumVersion"));
            CHECK(!contains(msg, "sha256"));
        }
        // At the floor, and above it: fine.
        CHECK(PluginRepo::pluginBlockReason(installedRec("pocsag", "1.1.0"),
                                            policy("pocsag", "1.1.0", "1.2.0")) ==
              PluginBlockReason::None);
        CHECK(PluginRepo::pluginBlockReason(installedRec("pocsag", "1.5.0"),
                                            policy("pocsag", "1.1.0", "1.2.0")) ==
              PluginBlockReason::None);
        // ...and the floor is compared NUMERICALLY. Under strcmp ordering
        // "1.9.0" would look newer than the "1.10.0" floor and a retired
        // plugin would keep loading.
        CHECK(PluginRepo::pluginBlockReason(installedRec("pocsag", "1.9.0"),
                                            policy("pocsag", "1.10.0", "1.10.0")) ==
              PluginBlockReason::BelowMinimumVersion);
        CHECK(PluginRepo::pluginBlockReason(installedRec("pocsag", "1.10.0"),
                                            policy("pocsag", "1.9.0", "1.10.0")) ==
              PluginBlockReason::None);

        // A floor with no newer build in the last catalogue seen must not
        // promise a fix that does not exist.
        {
            const InstalledPlugin i = installedRec("pocsag", "1.0.2");
            const std::string msg = PluginRepo::pluginBlockMessage(i, policy("pocsag", "1.1.0", ""));
            CHECK(contains(msg, "out of date"));
            CHECK(contains(msg, "1.1.0"));
            CHECK(!contains(msg, "update it to use it again"));
            CHECK(contains(msg, "author"));
        }

        // ABI MISMATCH is a different reason with a different remedy.
        {
            const InstalledPlugin wrongAbi = installedRec("pocsag", "9.9.9", 999u);
            CHECK(PluginRepo::pluginBlockReason(wrongAbi, policy("pocsag", "", "9.9.9")) ==
                  PluginBlockReason::AbiMismatch);
            // ...and it wins over a floor, because telling someone to update
            // a plugin no update can fix is the worse of the two messages.
            const InstalledPlugin bothWrong = installedRec("pocsag", "1.0.2", 999u);
            CHECK(PluginRepo::pluginBlockReason(bothWrong, policy("pocsag", "1.1.0", "1.2.0")) ==
                  PluginBlockReason::AbiMismatch);
            const std::string msg = PluginRepo::pluginBlockMessage(bothWrong, policy("pocsag",
                                                                                     "1.1.0",
                                                                                     "1.2.0"));
            CHECK(contains(msg, "pocsag decoder"));
            CHECK(contains(msg, "1.0.2"));
            CHECK(contains(msg, "different version of FoxSDR"));
            CHECK(contains(msg, "new build"));
            CHECK(contains(msg, "author"));
            // It must NOT send the user to the catalogue's Update button.
            CHECK(!contains(msg, "update it to use it again"));
            CHECK(!contains(msg, "or newer is required"));
            CHECK(!contains(msg, "ABI"));
        }
        // An UNRECORDED abi (0) is unknown, not wrong: fail open.
        {
            InstalledPlugin unrecorded = installedRec("pocsag", "1.2.0", 0u);
            CHECK(PluginRepo::pluginBlockReason(unrecorded, policy("pocsag", "", "1.2.0")) ==
                  PluginBlockReason::None);
            CHECK(PluginRepo::pluginBlockMessage(unrecorded, policy("pocsag", "", "1.2.0")).empty());
        }
        // A matching ABI is never a mismatch.
        CHECK(PluginRepo::pluginBlockReason(installedRec("pocsag", "1.2.0"),
                                            policy("pocsag", "", "1.2.0")) ==
              PluginBlockReason::None);

        // The set/count the GUI asks for.
        {
            std::vector<InstalledPlugin> installed;
            installed.push_back(installedRec("pocsag", "1.0.2"));            // below floor
            installed.push_back(installedRec("flex", "2.0.0"));              // fine
            installed.push_back(installedRec("acars", "1.0.0", 999u));       // wrong ABI
            installed.push_back(installedRec("private", "0.0.1"));           // no policy at all
            std::vector<CachedPolicy> policies;
            policies.push_back(policy("pocsag", "1.1.0", "1.2.0"));
            policies.push_back(policy("flex", "1.0.0", "2.0.0"));
            policies.push_back(policy("acars", "", "1.0.0"));

            CHECK(PluginRepo::blockedCount(installed, policies) == 2u);
            const std::vector<BlockedPlugin> blocked =
                PluginRepo::blockedPlugins(installed, policies);
            CHECK(blocked.size() == 2u);
            if (blocked.size() == 2u) {
                CHECK(blocked[0].installed.id == "pocsag");
                CHECK(blocked[0].reason == PluginBlockReason::BelowMinimumVersion);
                CHECK(!blocked[0].message.empty());
                CHECK(blocked[0].message ==
                      PluginRepo::pluginBlockMessage(blocked[0].installed, blocked[0].policy));
                CHECK(blocked[1].installed.id == "acars");
                CHECK(blocked[1].reason == PluginBlockReason::AbiMismatch);
            }
            // Nothing installed, or nothing known: no badge.
            CHECK(PluginRepo::blockedCount({}, policies) == 0u);
            CHECK(PluginRepo::blockedCount(installed, {}) == 1u);  // only the ABI one survives
        }
    }

    // ---------------------------------------------------------------------
    // The offline guarantee, end to end: a policy is cached from a catalogue
    // ONCE, and enforcement then works from disk alone, forever, with no
    // network in the process at all.
    // ---------------------------------------------------------------------
    {
        const fs::path d = tmpDir("offline");
        fs::create_directories(d);
        const std::string dir = d.string();
        writeText(d / "cascade_pocsag.dll", "the plugin the user installed a year ago");

        // A year ago: version 1.0.2 was installed from a catalogue that had
        // no floor. It loads.
        PluginCatalogEntry oldCat = catEntry("pocsag", "1.0.2", CASCADE_PLUGIN_ABI_VERSION,
                                             "cascade_pocsag.dll");
        std::string err = "stale";
        CHECK(PluginRepo::recordInstall(dir, oldCat, err));
        CHECK(err.empty());
        {
            PluginInventory inv;
            CHECK(PluginRepo::loadInventory(dir, inv, err));
            CHECK(inv.plugins.size() == 1u);
            CHECK(inv.policies.size() == 1u);
            CHECK(PluginRepo::blockedCount(inv.plugins, inv.policies) == 0u);
            if (inv.plugins.size() == 1u) {
                CHECK(inv.plugins[0].version == "1.0.2");
                CHECK(inv.plugins[0].name == "pocsag decoder");
                CHECK(inv.plugins[0].installedAtUnix > 0);
                CHECK(!inv.plugins[0].missingFromDisk);
            }
        }

        // Today: the user opens the browser once. The catalogue now retires
        // everything below 1.1.0. Only the POLICY is cached - no download.
        std::vector<PluginCatalogEntry> catalogue;
        catalogue.push_back(catEntry("pocsag", "1.2.0", CASCADE_PLUGIN_ABI_VERSION,
                                     "cascade_pocsag.dll"));
        catalogue[0].minSupportedVersion = "1.1.0";
        CHECK(PluginRepo::cacheCataloguePolicies(dir, catalogue, err));
        CHECK(err.empty());
        CHECK(fs::exists(d / "cascade_pocsag.dll"));  // nothing was downloaded or replaced

        // Every launch from now on - offline, catalogue unreachable, browser
        // never opened again - reads the cache and blocks.
        for (int launch = 0; launch < 3; ++launch) {
            PluginInventory inv;
            std::string lerr;
            CHECK(PluginRepo::loadInventory(dir, inv, lerr));
            const std::vector<BlockedPlugin> blocked =
                PluginRepo::blockedPlugins(inv.plugins, inv.policies);
            CHECK(blocked.size() == 1u);
            if (blocked.size() == 1u) {
                CHECK(blocked[0].reason == PluginBlockReason::BelowMinimumVersion);
                CHECK(contains(blocked[0].message, "1.0.2"));
                CHECK(contains(blocked[0].message, "1.1.0"));
                CHECK(contains(blocked[0].message, "1.2.0"));
            }
            // And the remedy the UI offers is a real one.
            const std::vector<PluginUpdate> plan =
                PluginRepo::planUpdates(catalogue, inv.plugins);
            CHECK(plan.size() == 1u);
        }

        // An EMPTY catalogue - unreachable origin, truncated document, an
        // attacker serving "{}" - must not lift the floor.
        CHECK(PluginRepo::cacheCataloguePolicies(dir, {}, err));
        {
            PluginInventory inv;
            CHECK(PluginRepo::loadInventory(dir, inv, err));
            CHECK(PluginRepo::blockedCount(inv.plugins, inv.policies) == 1u);
        }
        // The catalogue itself dropping the floor DOES lift it: an id it
        // publishes is its own to describe.
        std::vector<PluginCatalogEntry> relaxed;
        relaxed.push_back(catEntry("pocsag", "1.2.0", CASCADE_PLUGIN_ABI_VERSION,
                                   "cascade_pocsag.dll"));
        CHECK(PluginRepo::cacheCataloguePolicies(dir, relaxed, err));
        {
            PluginInventory inv;
            CHECK(PluginRepo::loadInventory(dir, inv, err));
            CHECK(PluginRepo::blockedCount(inv.plugins, inv.policies) == 0u);
        }

        std::error_code ec;
        fs::remove_all(d, ec);
    }

    // ---------------------------------------------------------------------
    // mergePolicies, in isolation
    // ---------------------------------------------------------------------
    {
        std::vector<CachedPolicy> cached;
        cached.push_back(policy("pocsag", "1.1.0", "1.2.0"));
        cached.push_back(policy("flex", "0.5.0", "0.6.0"));

        std::vector<PluginCatalogEntry> cat;
        cat.push_back(catEntry("pocsag", "1.3.0"));
        cat[0].minSupportedVersion = "1.2.0";
        cat.push_back(catEntry("acars", "0.1.0"));  // new id
        PluginCatalogEntry noId = catEntry("", "1.0.0");
        cat.push_back(noId);  // must be ignored, not stored under ""

        PluginRepo::mergePolicies(cached, cat);
        CHECK(cached.size() == 3u);
        CHECK(PluginRepo::policyFor(cached, "pocsag").minSupportedVersion == "1.2.0");
        CHECK(PluginRepo::policyFor(cached, "pocsag").catalogueVersion == "1.3.0");
        // flex was not in the catalogue: its policy is KEPT, not cleared.
        CHECK(PluginRepo::policyFor(cached, "flex").known);
        CHECK(PluginRepo::policyFor(cached, "flex").minSupportedVersion == "0.5.0");
        CHECK(PluginRepo::policyFor(cached, "acars").known);
        CHECK(PluginRepo::policyFor(cached, "acars").minSupportedVersion.empty());
        CHECK(!PluginRepo::policyFor(cached, "nobody").known);
        CHECK(PluginRepo::policyFor(cached, "nobody").id == "nobody");
    }

    // ---------------------------------------------------------------------
    // planUpdates. Pure, offline, and the no-downgrade proof.
    // ---------------------------------------------------------------------
    {
        // Newer available -> planned, with the fields the UI shows.
        {
            std::vector<PluginCatalogEntry> cat{catEntry("pocsag", "1.2.0")};
            std::vector<InstalledPlugin> have{installedRec("pocsag", "1.0.2")};
            const std::vector<PluginUpdate> plan = PluginRepo::planUpdates(cat, have);
            CHECK(plan.size() == 1u);
            if (plan.size() == 1u) {
                CHECK(plan[0].id == "pocsag");
                CHECK(plan[0].fromVersion == "1.0.2");
                CHECK(plan[0].toVersion == "1.2.0");
                CHECK(!plan[0].reason.empty());
                CHECK(contains(plan[0].reason, "1.2.0"));
                CHECK(contains(plan[0].reason, "1.0.2"));
                CHECK(plan[0].entry == &cat[0]);  // aliases the catalogue
            }
        }
        // Same version, same ABI -> nothing to do.
        {
            std::vector<PluginCatalogEntry> cat{catEntry("pocsag", "1.2.0")};
            std::vector<InstalledPlugin> have{installedRec("pocsag", "1.2.0")};
            CHECK(PluginRepo::planUpdates(cat, have).empty());
        }
        // NEVER A DOWNGRADE. Written as 1.9.0-catalogue vs 1.10.0-installed
        // on purpose: under strcmp ordering the catalogue would look newer
        // and this test would plan a downgrade.
        {
            std::vector<PluginCatalogEntry> cat{catEntry("pocsag", "1.9.0")};
            std::vector<InstalledPlugin> have{installedRec("pocsag", "1.10.0")};
            const std::vector<PluginUpdate> plan = PluginRepo::planUpdates(cat, have);
            if (!plan.empty()) {
                std::printf("FAIL planUpdates planned a DOWNGRADE %s -> %s\n",
                            plan[0].fromVersion.c_str(), plan[0].toVersion.c_str());
            }
            CHECK(plan.empty());
        }
        {
            std::vector<PluginCatalogEntry> cat{catEntry("pocsag", "0.1.0")};
            std::vector<InstalledPlugin> have{installedRec("pocsag", "3.0.0")};
            CHECK(PluginRepo::planUpdates(cat, have).empty());
        }
        // ...not even to rescue an ABI-mismatched install: a catalogue that
        // has gone backwards is a mistake or an attack either way.
        {
            std::vector<PluginCatalogEntry> cat{catEntry("pocsag", "0.1.0")};
            std::vector<InstalledPlugin> have{installedRec("pocsag", "3.0.0", 999u)};
            CHECK(PluginRepo::planUpdates(cat, have).empty());
        }
        // An entry this host cannot load is not an upgrade.
        {
            std::vector<PluginCatalogEntry> cat{catEntry("pocsag", "9.9.9", 999u)};
            std::vector<InstalledPlugin> have{installedRec("pocsag", "1.0.0")};
            CHECK(PluginRepo::planUpdates(cat, have).empty());
        }
        // No build for this os/arch: nothing to install.
        {
            PluginCatalogEntry e = catEntry("pocsag", "9.9.9");
            e.platforms[0].os = "plan9";
            e.platforms[0].arch = "sparc";
            std::vector<PluginCatalogEntry> cat{e};
            std::vector<InstalledPlugin> have{installedRec("pocsag", "1.0.0")};
            CHECK(PluginRepo::planUpdates(cat, have).empty());
            // ...and an entry with no platforms at all.
            PluginCatalogEntry none = catEntry("pocsag", "9.9.9");
            none.platforms.clear();
            CHECK(PluginRepo::planUpdates({none}, have).empty());
        }
        // Installed but not in the catalogue: left completely alone.
        {
            std::vector<PluginCatalogEntry> cat{catEntry("flex", "1.0.0")};
            std::vector<InstalledPlugin> have{installedRec("pocsag", "1.0.0")};
            CHECK(PluginRepo::planUpdates(cat, have).empty());
        }
        // In the catalogue but not installed: this is not an installer.
        {
            std::vector<PluginCatalogEntry> cat{catEntry("pocsag", "1.0.0")};
            CHECK(PluginRepo::planUpdates(cat, {}).empty());
        }
        // The user deleted the file: do not resurrect it.
        {
            std::vector<PluginCatalogEntry> cat{catEntry("pocsag", "2.0.0")};
            InstalledPlugin gone = installedRec("pocsag", "1.0.0");
            gone.missingFromDisk = true;
            CHECK(PluginRepo::planUpdates(cat, {gone}).empty());
        }
        // Same version, but the installed build targets a dead ABI and the
        // catalogue's build targets ours: a rebuild IS an update.
        {
            std::vector<PluginCatalogEntry> cat{catEntry("pocsag", "1.2.0")};
            std::vector<InstalledPlugin> have{installedRec("pocsag", "1.2.0", 999u)};
            const std::vector<PluginUpdate> plan = PluginRepo::planUpdates(cat, have);
            CHECK(plan.size() == 1u);
            if (plan.size() == 1u) {
                CHECK(plan[0].fromVersion == "1.2.0");
                CHECK(plan[0].toVersion == "1.2.0");
                CHECK(contains(plan[0].reason, "rebuilt"));
            }
        }
        // An unrecorded ABI is not a reason to reinstall the same version.
        {
            std::vector<PluginCatalogEntry> cat{catEntry("pocsag", "1.2.0")};
            std::vector<InstalledPlugin> have{installedRec("pocsag", "1.2.0", 0u)};
            CHECK(PluginRepo::planUpdates(cat, have).empty());
        }
        // A mixed catalogue plans exactly the entries that qualify, in
        // catalogue order.
        {
            std::vector<PluginCatalogEntry> cat;
            cat.push_back(catEntry("pocsag", "1.2.0"));            // newer   -> yes
            cat.push_back(catEntry("flex", "1.0.0"));              // same    -> no
            cat.push_back(catEntry("acars", "0.1.0"));             // older   -> no
            cat.push_back(catEntry("ais", "5.0.0", 999u));         // bad ABI -> no
            cat.push_back(catEntry("dmr", "2.0.0"));               // not installed -> no
            std::vector<InstalledPlugin> have;
            have.push_back(installedRec("pocsag", "1.0.2"));
            have.push_back(installedRec("flex", "1.0.0"));
            have.push_back(installedRec("acars", "0.9.0"));
            have.push_back(installedRec("ais", "1.0.0"));
            const std::vector<PluginUpdate> plan = PluginRepo::planUpdates(cat, have);
            CHECK(plan.size() == 1u);
            if (plan.size() == 1u) {
                CHECK(plan[0].id == "pocsag");
            }
            // Planning removes nothing and installs nothing: `have` is intact.
            CHECK(have.size() == 4u);
        }
        // Empty inputs are ordinary.
        CHECK(PluginRepo::planUpdates({}, {}).empty());
    }

    // ---------------------------------------------------------------------
    // applyUpdate: an update IS an install, so it is refused by exactly the
    // same gates, before any network call - and a refused update leaves the
    // manifest byte-identical.
    // ---------------------------------------------------------------------
    {
        const fs::path d = tmpDir("apply");
        fs::create_directories(d);
        const std::string dir = d.string();
        writeText(d / "probe.dll", "the plugin that must survive a failed update");

        // A manifest that must not change.
        std::vector<InstalledPlugin> before{installedRec("evil", "1.0.0")};
        std::string err;
        CHECK(PluginRepo::saveManifest(dir, before, {policy("evil", "", "1.0.0")}, err));
        const std::string manifestBytes = readAll(PluginRepo::manifestPath(dir));
        const std::string dllBytes = readAll(d / "probe.dll");

        // A catalogue entry whose file name is a path traversal. planUpdates
        // plans it (planning is not where safety lives) and applyUpdate
        // refuses it at the one enforcement point.
        std::vector<PluginCatalogEntry> cat;
        cat.push_back(catEntry("evil", "2.0.0", CASCADE_PLUGIN_ABI_VERSION, "..\\evil.dll"));
        const std::vector<PluginUpdate> plan = PluginRepo::planUpdates(cat, before);
        CHECK(plan.size() == 1u);
        if (plan.size() == 1u) {
            PluginRepo repo;
            std::string installed = "stale";
            err.clear();
            const bool ok = repo.applyUpdate(plan[0], dir, installed, err);
            if (ok) {
                std::printf("FAIL applyUpdate accepted a path-traversal file name\n");
            }
            CHECK(!ok);
            CHECK(!err.empty());
            CHECK(installed.empty());
            // Nothing downloaded, nothing installed, nothing recorded.
            CHECK(readAll(PluginRepo::manifestPath(dir)) == manifestBytes);
            CHECK(readAll(d / "probe.dll") == dllBytes);
            CHECK(!fs::exists(d.parent_path() / "evil.dll"));
            CHECK(countEntries(d) == 2u);  // the dll and the manifest, nothing else
        }

        // The other pre-network refusals, each leaving the manifest alone.
        {
            PluginRepo repo;
            std::string installed;
            struct Case {
                const char* tag;
                PluginCatalogEntry e;
            };
            std::vector<Case> cases;
            cases.push_back({"wrong abi", catEntry("evil", "2.0.0", 999u)});
            cases.push_back({"reserved device name",
                             catEntry("evil", "2.0.0", CASCADE_PLUGIN_ABI_VERSION, "CON.dll")});
            PluginCatalogEntry badHash = catEntry("evil", "2.0.0");
            badHash.platforms[0].sha256 = "abc123";
            cases.push_back({"malformed sha256", badHash});
            PluginCatalogEntry httpUrl = catEntry("evil", "2.0.0");
            httpUrl.platforms[0].url = "http://example.invalid/probe.dll";
            cases.push_back({"http url", httpUrl});
            PluginCatalogEntry noBuild = catEntry("evil", "2.0.0");
            noBuild.platforms.clear();
            cases.push_back({"no build for this host", noBuild});

            for (const Case& c : cases) {
                PluginUpdate u;
                u.id = c.e.id;
                u.fromVersion = "1.0.0";
                u.toVersion = c.e.version;
                u.entry = &c.e;
                installed = "stale";
                err.clear();
                const bool ok = repo.applyUpdate(u, dir, installed, err);
                if (ok) {
                    std::printf("FAIL applyUpdate accepted what install() must refuse: %s\n",
                                c.tag);
                }
                CHECK(!ok);
                CHECK(!err.empty());
                CHECK(installed.empty());
                CHECK(readAll(PluginRepo::manifestPath(dir)) == manifestBytes);
                CHECK(readAll(d / "probe.dll") == dllBytes);
            }
        }

        // A plan with no catalogue entry is refused rather than dereferenced.
        {
            PluginRepo repo;
            PluginUpdate empty;
            empty.id = "evil";
            std::string installed = "stale";
            err.clear();
            CHECK(!repo.applyUpdate(empty, dir, installed, err));
            CHECK(!err.empty());
            CHECK(readAll(PluginRepo::manifestPath(dir)) == manifestBytes);
        }

#ifdef _WIN32
        // A failed TRANSFER (nothing listens on 127.0.0.1:1) is the same
        // promise one layer deeper: the installed file and the manifest are
        // both exactly as they were, and no ".part" debris is left.
        {
            PluginRepo repo;
            PluginCatalogEntry e = catEntry("evil", "2.0.0", CASCADE_PLUGIN_ABI_VERSION,
                                            "probe.dll");
            e.platforms[0].url = "https://127.0.0.1:1/probe.dll";
            PluginUpdate u;
            u.id = e.id;
            u.fromVersion = "1.0.0";
            u.toVersion = "2.0.0";
            u.entry = &e;
            std::string installed = "stale";
            err.clear();
            CHECK(!repo.applyUpdate(u, dir, installed, err));
            CHECK(!err.empty());
            CHECK(err != "cancelled");
            CHECK(readAll(d / "probe.dll") == dllBytes);
            CHECK(readAll(PluginRepo::manifestPath(dir)) == manifestBytes);
            CHECK(countEntries(d) == 2u);
        }
#endif

        std::error_code ec;
        fs::remove_all(d, ec);
    }

    // ---------------------------------------------------------------------
    // recordInstall: the manifest row the browser's install path writes
    // ---------------------------------------------------------------------
    {
        const fs::path d = tmpDir("record");
        fs::create_directories(d);
        const std::string dir = d.string();
        writeText(d / "probe.dll", "installed bytes");

        PluginCatalogEntry e = catEntry("pocsag", "1.0.0");
        e.minSupportedVersion = "0.9.0";
        std::string err = "stale";
        CHECK(PluginRepo::recordInstall(dir, e, err));
        CHECK(err.empty());

        PluginInventory inv;
        CHECK(PluginRepo::loadInventory(dir, inv, err));
        CHECK(inv.plugins.size() == 1u);
        CHECK(inv.policies.size() == 1u);
        if (inv.plugins.size() == 1u) {
            CHECK(inv.plugins[0].id == "pocsag");
            CHECK(inv.plugins[0].version == "1.0.0");
            CHECK(inv.plugins[0].file == "probe.dll");
            CHECK(inv.plugins[0].sha256 == kHashA);
            CHECK(inv.plugins[0].abiVersion ==
                  static_cast<std::uint32_t>(CASCADE_PLUGIN_ABI_VERSION));
        }
        CHECK(PluginRepo::policyFor(inv.policies, "pocsag").minSupportedVersion == "0.9.0");

        // Recording the same id again UPDATES the row rather than adding one.
        PluginCatalogEntry newer = catEntry("pocsag", "1.4.0");
        CHECK(PluginRepo::recordInstall(dir, newer, err));
        CHECK(PluginRepo::loadInventory(dir, inv, err));
        CHECK(inv.plugins.size() == 1u);
        if (inv.plugins.size() == 1u) {
            CHECK(inv.plugins[0].version == "1.4.0");
        }
        // ...and the newer catalogue's (absent) floor replaced the old one.
        CHECK(PluginRepo::policyFor(inv.policies, "pocsag").minSupportedVersion.empty());

        // A bad entry is refused without writing anything.
        const std::string bytes = readAll(PluginRepo::manifestPath(dir));
        PluginCatalogEntry bad = catEntry("pocsag", "2.0.0", CASCADE_PLUGIN_ABI_VERSION,
                                          "../evil.dll");
        err.clear();
        CHECK(!PluginRepo::recordInstall(dir, bad, err));
        CHECK(!err.empty());
        CHECK(readAll(PluginRepo::manifestPath(dir)) == bytes);

        std::error_code ec;
        fs::remove_all(d, ec);
    }

    // ---------------------------------------------------------------------
    // Optional live fetch. Off by default: the catalogue repository is
    // private, so a failure here would say nothing about this code.
    // ---------------------------------------------------------------------
    {
        const char* live = std::getenv("CASCADE_TEST_LIVE_CATALOGUE");
        if (live != nullptr && live[0] == '1') {
            PluginRepo repo;
            std::string err;
            const bool ok = repo.fetchIndex(PluginRepo::defaultIndexUrl(), err);
            std::printf("live fetch: %s (%zu entries) %s\n", ok ? "ok" : "failed",
                        repo.entries().size(), err.c_str());
            CHECK(ok);
        } else {
            std::printf(
                "note: live catalogue fetch skipped (set CASCADE_TEST_LIVE_CATALOGUE=1 to "
                "enable)\n");
        }
    }

    return testSummary("test_plugin_repo");
}
