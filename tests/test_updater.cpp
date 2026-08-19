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
    testRubbishDoesNotCrashIt();
    testNotesSurviveAndMalformedOnesAreDropped();
    testDownloadRefusesWithoutAnUpdate();
    testEndpointIsHttpsAndOnTheProjectsDomain();
    return testSummary("test_updater");
}
