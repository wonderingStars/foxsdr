// Tests for core/updater.{hpp,cpp}.
//
// An updater decides whether to download and run an executable, so the tests
// that matter are the ones about REFUSING. The happy path is one case; the
// rest of this file is the manifest fields that make a download safe, each
// checked to fail closed.
//
// All of it is driven through parseUpdateManifest, which is pure - no network
// is needed to prove that a manifest pointing somewhere else is rejected.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/updater.hpp"

#include <atomic>
#include <cstdio>
#include <string>

#include "test_check.hpp"

using cascade::core::UpdateInfo;

namespace {

// A well-formed manifest, which each test then breaks in exactly one way.
std::string manifest(const std::string& version = "0.56.0",
                     const std::string& url = "https://foxsdr.com/download/foxsdr-setup-0.56.0.exe",
                     const std::string& sha =
                         "393e5fd91b7b2292611c52af7e9f2db1e2c730d78f1639adefceaf0fb5cca0ea") {
    return std::string("{") +
           "\"ok\":true,\"version\":\"" + version + "\"," +
           "\"url\":\"" + url + "\"," +
           "\"sha256\":\"" + sha + "\"," +
           "\"size\":3274221,\"newer\":true,\"critical\":true," +
           "\"notes\":[{\"version\":\"0.56.0\",\"date\":\"2026-08-19\",\"critical\":true," +
           "\"notes\":[\"Radios are detected again.\",\"RTL-SDR needs Zadig.\"]}]}";
}

// Backslashes and quotes have to survive into the JSON as themselves, or a
// hostile version would be testing the escaping and not the parser.
std::string jsonEscaped(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\' || c == '"') { out.push_back('\\'); }
        out.push_back(c);
    }
    return out;
}

// The version names the installer file on disk, and that file is then run, so
// every one of these has to be refused BEFORE any path is built from it.
const char* const kHostileVersions[] = {
    "..\\..\\evil",             // traversal with Windows separators
    "1.2.3/../x",               // traversal with posix ones
    "C:\\x",                    // an absolute path, drive colon and all
    "1.2.3 ",                   // trailing whitespace
    "..",                       // the parent directory itself
    "",                         // nothing at all
    "0.58.0-nightly\xC3\xA9",   // a letter outside the grammar (UTF-8 e-acute)
};

void testVersionOrdering() {
    using cascade::core::compareVersions;
    CHECK(compareVersions("0.55.0", "0.54.0") == 1);
    CHECK(compareVersions("0.54.0", "0.55.0") == -1);
    CHECK(compareVersions("0.55.0", "0.55.0") == 0);

    // THE CLASSIC. Compared as text, "0.9.0" sorts above "0.10.0" and every
    // user is told they are up to date for ever.
    CHECK(compareVersions("0.10.0", "0.9.0") == 1);
    CHECK(compareVersions("1.0.0", "0.99.99") == 1);

    // Missing parts are zero, so these name the same build.
    CHECK(compareVersions("0.55", "0.55.0") == 0);

    // A pre-release is OLDER than the release of the same number. Without
    // this, someone on a nightly would be offered the stable that superseded
    // it - and someone on that stable would be offered the nightly back.
    CHECK(compareVersions("0.56.0-nightly.20260819.abc1234", "0.56.0") == -1);
    CHECK(compareVersions("0.56.0", "0.56.0-nightly.20260819.abc1234") == 1);
    CHECK(compareVersions("0.56.0-nightly.20260819.abc1234", "0.55.0") == 1);
    // Two nightlies of the same version order by suffix, which starts with the
    // date.
    CHECK(compareVersions("0.56.0-nightly.20260820.aaa", "0.56.0-nightly.20260819.zzz") == 1);

    // Junk does not crash and does not invent an ordering.
    CHECK(compareVersions("", "0.55.0") == -1);
    CHECK(compareVersions("not-a-version", "0.55.0") == -1);
}

void testAGoodManifestIsAccepted() {
    UpdateInfo info;
    std::string err;
    CHECK(cascade::core::parseUpdateManifest(manifest(), "0.50.0", info, err));
    CHECK(err.empty());
    CHECK(info.version == "0.56.0");
    CHECK(info.newer);
    CHECK(info.critical);
    CHECK(info.sizeBytes == 3274221u);
    CHECK(info.notes.size() == 1);
    if (info.notes.size() == 1) {
        CHECK(info.notes[0].notes.size() == 2);
        CHECK(info.notes[0].critical);
    }
}

void testTheVersionDecisionIsMadeHereNotByTheServer() {
    // The manifest claims newer:true, but this build IS 0.56.0. A server that
    // got it wrong - or a stale cache, or someone able to answer for it - must
    // not be able to talk a current build into "updating" to itself, nor an
    // newer one into going backwards.
    UpdateInfo info;
    std::string err;
    CHECK(cascade::core::parseUpdateManifest(manifest("0.56.0"), "0.56.0", info, err));
    CHECK(!info.newer);
    CHECK(!info.critical);  // an update that is not newer is never critical

    CHECK(cascade::core::parseUpdateManifest(manifest("0.54.0"), "0.56.0", info, err));
    CHECK(!info.newer);
}

void testAManifestWithoutAChecksumIsRefused() {
    // The digest is the thing that finally decides whether downloaded bytes
    // may become an executable. Without one there is nothing to decide with.
    UpdateInfo info;
    std::string err;
    const char* bad[] = {
        "",                                    // absent
        "393e5fd9",                            // too short
        "393e5fd91b7b2292611c52af7e9f2db1e2c730d78f1639adefceaf0fb5cca0eaZZ",  // too long
        "393e5fd91b7b2292611c52af7e9f2db1e2c730d78f1639adefceaf0fb5cca0eg",    // not hex
    };
    for (const char* s : bad) {
        const bool ok = cascade::core::parseUpdateManifest(
            manifest("0.56.0", "https://foxsdr.com/download/x.exe", s), "0.50.0", info, err);
        if (ok) {
            std::printf("FAIL accepted a manifest with checksum \"%s\"\n", s);
        }
        CHECK(!ok);
        CHECK(!err.empty());
    }
}

void testAManifestPointingSomewhereElseIsRefused() {
    // A manifest is exactly what an attacker able to answer for the endpoint
    // would control, so the URL in it is checked rather than trusted. The
    // sha256 would still have to match, but an update must never even ATTEMPT
    // a download from somewhere unrelated.
    UpdateInfo info;
    std::string err;
    const char* bad[] = {
        "http://foxsdr.com/download/x.exe",       // not https
        "https://example.com/x.exe",              // another host
        "https://evilfoxsdr.com/x.exe",           // suffix trick a naive check allows
        "https://foxsdr.com.attacker.net/x.exe",  // prefix trick
        "",                                       // absent
    };
    for (const char* u : bad) {
        const bool ok = cascade::core::parseUpdateManifest(manifest("0.56.0", u), "0.50.0", info,
                                                           err);
        if (ok) {
            std::printf("FAIL accepted an update URL of \"%s\"\n", u);
        }
        CHECK(!ok);
    }
    // A subdomain of the real host is fine - downloads may move to one.
    CHECK(cascade::core::parseUpdateManifest(
        manifest("0.56.0", "https://dl.foxsdr.com/download/x.exe"), "0.50.0", info, err));
}

void testVersionGrammar() {
    using cascade::core::wellFormedVersion;
    // The two shapes the project actually publishes.
    CHECK(wellFormedVersion("0.58.0"));
    CHECK(wellFormedVersion("0.57.0-nightly.20260819.b97092e"));
    CHECK(wellFormedVersion("1"));
    CHECK(wellFormedVersion("1.2"));
    CHECK(wellFormedVersion("10.0.4.2"));

    for (const char* v : kHostileVersions) {
        if (wellFormedVersion(v)) {
            std::printf("FAIL wellFormedVersion accepted \"%s\"\n", v);
        }
        CHECK(!wellFormedVersion(v));
    }

    // Structurally unparseable, which a bare charset filter would wave through
    // because every character in them is in [0-9A-Za-z.-].
    const char* const malformed[] = {
        "..", "...", ".", "-", "1..2", ".1.2", "1.2.", "1.2.3-", "1.2.3-a..b",
        "1.2.3-.a", "1.2.3-a.", "v1.2.3", "1.2.3-nightly-x", "1.2.3+build7",
        "not-a-version", "1.2.3.4.5", "1234567890.0.0",
    };
    for (const char* v : malformed) {
        if (wellFormedVersion(v)) {
            std::printf("FAIL wellFormedVersion accepted \"%s\"\n", v);
        }
        CHECK(!wellFormedVersion(v));
    }

    // Long enough to be a path all by itself.
    CHECK(!wellFormedVersion(std::string(100, '1')));

    // THE 64-CHARACTER CAP ON ITS OWN. Every other long string here is already
    // refused by an earlier rule - 100 digits fails "1 to 9 digits per part"
    // before length is ever consulted - so the cap has been shadowed and could
    // be deleted without turning anything red. A pre-release suffix is
    // grammar-valid at ANY length, which makes it the one shape where nothing
    // but the cap can object: these two strings differ by a single 'a'.
    const std::string prefix = "1.2.3-";  // 6 characters, then alnum segment
    const std::string atCap = prefix + std::string(58, 'a');
    const std::string overCap = prefix + std::string(59, 'a');
    CHECK(atCap.size() == 64u);
    CHECK(overCap.size() == 65u);
    CHECK(wellFormedVersion(atCap));    // exactly at the cap: accepted
    CHECK(!wellFormedVersion(overCap));  // one over: only the cap refuses it
}

void testAManifestWithAHostileVersionIsRefused() {
    // The version is the one manifest field that becomes a FILESYSTEM PATH -
    // the installer is named after it and then executed - so a manifest that
    // smuggles a traversal into it must be refused whole, not sanitised.
    UpdateInfo info;
    std::string err;
    for (const char* v : kHostileVersions) {
        const bool ok = cascade::core::parseUpdateManifest(manifest(jsonEscaped(v)), "0.50.0",
                                                           info, err);
        if (ok) {
            std::printf("FAIL accepted a manifest naming version \"%s\"\n", v);
        }
        CHECK(!ok);
        CHECK(!err.empty());
    }
}

void testLegitimateVersionsStillGetThrough() {
    // Both real shapes: a release, and the nightly form the site publishes.
    UpdateInfo info;
    std::string err;
    CHECK(cascade::core::parseUpdateManifest(manifest("0.58.0"), "0.50.0", info, err));
    CHECK(err.empty());
    CHECK(info.version == "0.58.0");
    CHECK(info.newer);

    const std::string nightly = "0.57.0-nightly.20260819.b97092e";
    CHECK(cascade::core::parseUpdateManifest(manifest(nightly), "0.50.0", info, err));
    CHECK(err.empty());
    CHECK(info.version == nightly);
    CHECK(info.newer);
}

void testDownloadRefusesAHostileVersion() {
    // downloadUpdate is public, so it re-checks the version the same way it
    // re-checks the host, and does it before the path exists rather than after.
    for (const char* v : kHostileVersions) {
        UpdateInfo info;
        std::string path;
        std::string err;
        info.newer = true;
        info.version = v;
        info.url = "https://foxsdr.com/download/x.exe";
        info.sha256 = "393e5fd91b7b2292611c52af7e9f2db1e2c730d78f1639adefceaf0fb5cca0ea";
        const bool ok = cascade::core::downloadUpdate(info, path, err);
        if (ok) {
            std::printf("FAIL downloaded an update named \"%s\"\n", v);
        }
        CHECK(!ok);
        CHECK(path.empty());
        // Refused for BEING an unusable version, not incidentally by the
        // network failing after the path was already built.
        if (err.find("does not parse") == std::string::npos) {
            std::printf("FAIL version \"%s\" was not refused as a version: %s\n", v, err.c_str());
        }
        CHECK(err.find("does not parse") != std::string::npos);
    }
}

void testRubbishDoesNotCrashIt() {
    UpdateInfo info;
    std::string err;
    const char* bad[] = {
        "", "not json at all", "[]", "null", "{}", "{\"version\":\"\"}",
        "{\"version\":\"0.56.0\"}",  // no url, no sha
    };
    for (const char* s : bad) {
        CHECK(!cascade::core::parseUpdateManifest(s, "0.50.0", info, err));
        CHECK(!err.empty());
    }
}

void testNotesSurviveAndMalformedOnesAreDropped() {
    // A note with no text is not shown as an empty bullet; a whole manifest is
    // not thrown away because one note was malformed, because the version and
    // checksum are still actionable.
    const std::string m =
        "{\"version\":\"0.56.0\","
        "\"url\":\"https://foxsdr.com/download/x.exe\","
        "\"sha256\":\"393e5fd91b7b2292611c52af7e9f2db1e2c730d78f1639adefceaf0fb5cca0ea\","
        "\"notes\":[{\"version\":\"0.56.0\",\"notes\":[\"a real one\"]},"
        "{\"version\":\"\",\"notes\":[\"no version\"]},"
        "{\"version\":\"0.55.0\",\"notes\":[]},"
        "\"not an object\"]}";
    UpdateInfo info;
    std::string err;
    CHECK(cascade::core::parseUpdateManifest(m, "0.50.0", info, err));
    CHECK(info.notes.size() == 1);
    if (info.notes.size() == 1) {
        CHECK(info.notes[0].notes.size() == 1);
    }
}

void testDownloadRefusesWithoutAnUpdate() {
    // downloadUpdate is public, so it re-checks rather than trusting that its
    // caller went through parseUpdateManifest.
    UpdateInfo info;
    std::string path;
    std::string err;
    CHECK(!cascade::core::downloadUpdate(info, path, err));  // newer == false
    CHECK(path.empty());

    info.newer = true;
    info.version = "0.56.0";
    info.url = "https://example.com/x.exe";
    info.sha256 = "393e5fd91b7b2292611c52af7e9f2db1e2c730d78f1639adefceaf0fb5cca0ea";
    CHECK(!cascade::core::downloadUpdate(info, path, err));  // wrong host
    CHECK(path.empty());
}

void testDownloadHonoursACancelFlag() {
    // THE FAILURE THIS GUARDS. downloadUpdate went through the STATIC
    // PluginRepo::fetchVerifiedFile, which passed no cancel flag at all, so
    // nothing could reach the transfer once it started. The download future is
    // an AppWindow member and its destructor blocks until the worker returns,
    // so closing the application mid-update parked the GUI thread in
    // ~AppWindow for the rest of the download — the window gone, the process
    // still there, indistinguishable from a hang.
    //
    // A flag that is already set is refused BEFORE the connection is made,
    // which is both the honest behaviour (a cancel that arrived while this
    // thread was still setting up is still a cancel) and what makes this test
    // deterministic: no network is touched, on any machine, ever.
    UpdateInfo info;
    info.newer = true;
    info.version = "0.59.0";
    info.url = "https://foxsdr.com/download/foxsdr-setup-0.59.0.exe";
    info.sha256 = "393e5fd91b7b2292611c52af7e9f2db1e2c730d78f1639adefceaf0fb5cca0ea";

    std::atomic<float> progress{0.5f};  // a stale reading from an earlier try
    std::atomic<bool> cancel{true};
    std::string path = "stale";
    std::string err;
    const bool ok = cascade::core::downloadUpdate(info, path, err, &progress, &cancel);
    if (ok || err != "cancelled") {
        std::printf("FAIL a pre-set cancel gave ok=%d err=\"%s\"\n", ok ? 1 : 0, err.c_str());
    }
    CHECK(!ok);
    CHECK(err == "cancelled");
    CHECK(path.empty());
    // And the bar is reset rather than left showing the previous attempt's
    // half-full state, which is what a re-press would otherwise start from.
    CHECK(progress.load() == 0.0f);

    // The pair is optional, and omitting it must behave exactly as before:
    // this refusal is the version gate, unchanged.
    UpdateInfo none;
    std::string p2;
    std::string e2;
    CHECK(!cascade::core::downloadUpdate(none, p2, e2));
    CHECK(p2.empty());
}

void testEndpointIsHttpsAndOnTheProjectsDomain() {
    const std::string url = cascade::core::updateEndpoint();
    CHECK(url.rfind("https://", 0) == 0);
    CHECK(url.find("foxsdr.com") != std::string::npos);
    std::printf("  update endpoint: %s\n", url.c_str());
}

}  // namespace

int main() {
    testVersionOrdering();
    testAGoodManifestIsAccepted();
    testTheVersionDecisionIsMadeHereNotByTheServer();
    testAManifestWithoutAChecksumIsRefused();
    testAManifestPointingSomewhereElseIsRefused();
    testVersionGrammar();
    testAManifestWithAHostileVersionIsRefused();
    testLegitimateVersionsStillGetThrough();
    testDownloadRefusesAHostileVersion();
    testRubbishDoesNotCrashIt();
    testNotesSurviveAndMalformedOnesAreDropped();
    testDownloadRefusesWithoutAnUpdate();
    testDownloadHonoursACancelFlag();
    testEndpointIsHttpsAndOnTheProjectsDomain();
    return testSummary("test_updater");
}
