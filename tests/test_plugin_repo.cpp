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
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#define TEST_GETPID _getpid
#else
#include <unistd.h>
#define TEST_GETPID getpid
#endif

#include "test_check.hpp"

namespace fs = std::filesystem;

using cascade::core::PluginCatalogEntry;
using cascade::core::PluginPlatform;
using cascade::core::PluginRepo;

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
    // rule 5, the case that actually happens: the ABI moved from 1 to 2, so
    // every catalogue entry still advertising abiVersion 1 must now be listed
    // and marked INCOMPATIBLE, and install() must refuse it before touching
    // the network. Written against the literal 1 rather than
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
            CHECK(!v[0].compatible);          // the whole point
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
            CHECK(!fs::exists(dir));  // rule 7: nothing was created
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
