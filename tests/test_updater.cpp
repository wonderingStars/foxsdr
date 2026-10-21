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
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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

using cascade::core::TeamMessage;
using cascade::core::UpdateInfo;

namespace {

namespace fs = std::filesystem;

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

// The checksum every well-formed manifest here carries.
constexpr const char* kSha = "393e5fd91b7b2292611c52af7e9f2db1e2c730d78f1639adefceaf0fb5cca0ea";

// A manifest at `version` whose "messages" value is `messagesJson`, verbatim. The download
// fields are the ordinary, well-formed ones: a manifest whose url or checksum is bad is
// refused whole, and these tests are about what becomes of the MESSAGES of one that is not.
std::string withMessages(const std::string& messagesJson, const std::string& version = "0.56.0") {
    return std::string("{\"ok\":true,\"version\":\"") + version + "\"," +
           "\"url\":\"https://foxsdr.com/download/foxsdr-setup-" + version + ".exe\"," +
           "\"sha256\":\"" + kSha + "\",\"size\":3274221,\"notes\":[]," +
           "\"messages\":" + messagesJson + "}";
}

// One message object, as the site writes it.
std::string msg(const std::string& id, const std::vector<std::string>& lines,
                bool important = false, const std::string& date = "2026-10-14") {
    std::string out = "{\"id\":\"" + id + "\",\"date\":\"" + date + "\",\"lines\":[";
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i != 0) { out += ","; }
        out += "\"" + jsonEscaped(lines[i]) + "\"";
    }
    out += std::string("],\"important\":") + (important ? "true" : "false") + "}";
    return out;
}

// The ids of what was kept, in order.
std::vector<std::string> idsOf(const UpdateInfo& info) {
    std::vector<std::string> ids;
    for (const TeamMessage& m : info.messages) { ids.push_back(m.id); }
    return ids;
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

// A FIELD OF THE WRONG TYPE is a refusal, never an exception.
//
// parseUpdateManifest runs on a worker thread (UpdateCheckTask) and the GUI
// thread collects the result with future::get(), which rethrows whatever left
// the worker - on the one thread that must not be taken down by a web server's
// answer. nlohmann's value(key, default) does not return the default for a
// present-but-wrong-typed field, it THROWS (type_error 302), so a service that
// answered {"version": 5} or {"url": null} used to end the process. Found by
// the update_manifest fuzz target (tests/fuzz); the inputs below are in its
// regression corpus too.
void testWrongTypedFieldsAreRefusedNotThrown() {
    const char* wrong[] = {
        "{\"version\":5}",
        "{\"version\":null}",
        "{\"version\":[\"0.56.0\"]}",
        "{\"version\":\"0.56.0\",\"url\":null,\"sha256\":null}",
        "{\"version\":\"0.56.0\",\"url\":7,\"sha256\":\"x\"}",
        "{\"version\":\"0.56.0\",\"url\":\"https://foxsdr.com/download/x.exe\",\"sha256\":{}}",
    };
    for (const char* s : wrong) {
        UpdateInfo info;
        std::string err;
        bool threw = false;
        bool ok = true;
        try {
            ok = cascade::core::parseUpdateManifest(s, "0.50.0", info, err);
        } catch (...) {
            threw = true;
        }
        if (threw) { std::printf("FAIL parseUpdateManifest threw on: %s\n", s); }
        CHECK(!threw);
        CHECK(!ok);
        CHECK(!err.empty());
    }

    // In the notes, a wrong-typed field is as good as an absent one: that note
    // loses what it cannot say and the manifest - whose version and checksum are
    // still actionable - is kept, as a malformed note already was.
    const std::string m =
        "{\"version\":\"0.56.0\","
        "\"url\":\"https://foxsdr.com/download/x.exe\","
        "\"sha256\":\"393e5fd91b7b2292611c52af7e9f2db1e2c730d78f1639adefceaf0fb5cca0ea\","
        "\"notes\":[{\"version\":1,\"date\":2,\"critical\":\"yes\",\"notes\":[\"no version\"]},"
        "{\"version\":\"0.56.0\",\"date\":null,\"critical\":null,\"notes\":[\"kept\"]}]}";
    UpdateInfo info;
    std::string err;
    bool threw = false;
    bool ok = false;
    try {
        ok = cascade::core::parseUpdateManifest(m, "0.50.0", info, err);
    } catch (...) {
        threw = true;
    }
    CHECK(!threw);
    CHECK(ok);
    CHECK(info.notes.size() == 1);
    if (info.notes.size() == 1) {
        CHECK(info.notes[0].version == "0.56.0");
        CHECK(info.notes[0].date.empty());
        CHECK(!info.notes[0].critical);
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

// A STALLED CHECK, as a fake: the work blocks on a gate the test holds, which
// is what a GET stuck inside WinHttpReceiveResponse looks like from outside.
// The gate also opens by itself after kFailsafe, so the UNFIXED code - whose
// owner waits for the worker - finishes the test red instead of hanging it.
struct CheckGate {
    std::mutex m;
    std::condition_variable cv;
    bool open = false;
    std::atomic<bool> returned{false};
};
constexpr auto kFailsafe = std::chrono::milliseconds(3000);

cascade::core::UpdateCheckTask::Work gatedCheck(const std::shared_ptr<CheckGate>& g) {
    return [g]() {
        {
            std::unique_lock<std::mutex> lk(g->m);
            g->cv.wait_for(lk, kFailsafe, [&g] { return g->open; });
        }
        cascade::core::UpdateCheckOutcome o;
        o.ok = true;
        o.info.newer = true;
        o.info.version = "9.9.9";
        g->returned.store(true);
        return o;
    };
}

void openGate(CheckGate& g) {
    {
        std::lock_guard<std::mutex> lk(g.m);
        g.open = true;
    }
    g.cv.notify_all();
}

void testAStalledCheckDoesNotHoldTeardown() {
    // THE FAILURE THIS GUARDS (bug hunt 2026-09-24, updater-installer-1). The
    // once-per-launch check ran in a std::async future that was an AppWindow
    // member; that future's destructor waits for the worker, and nothing could
    // make a GET stalled in WinHTTP return early. Quitting while foxsdr.com was
    // slow therefore sat in ~AppWindow for WinHTTP's own timeouts.
    //
    // DETERMINISTIC: the worker cannot return before the test opens the gate,
    // and the test opens it only after the owner is gone - so a destructor that
    // waits for the worker is caught every time, by the failsafe's full length.
    using clock = std::chrono::steady_clock;
    const auto gate = std::make_shared<CheckGate>();
    auto t0 = clock::now();
    {
        cascade::core::UpdateCheckTask task;
        task.start(gatedCheck(gate));
        CHECK(task.running());
        t0 = clock::now();
    }  // the application quits here, with the check still stalled
    const auto heldMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - t0).count();
    const bool stillStalled = !gate->returned.load();
    std::printf("  teardown with a stalled check took %lld ms (grace %lld ms)%s\n",
                static_cast<long long>(heldMs),
                static_cast<long long>(cascade::core::UpdateCheckTask::kQuitGrace.count()),
                stillStalled ? "" : " - it waited for the worker");
    CHECK(stillStalled);
    CHECK(heldMs < 1500);
    CHECK(heldMs >= cascade::core::UpdateCheckTask::kQuitGrace.count() - 20);

    // THE ABANDONED WORKER STILL FINISHES, on its own, into its own result -
    // nothing it touches went with the owner.
    openGate(*gate);
    const auto deadline = clock::now() + std::chrono::seconds(5);
    while (!gate->returned.load() && clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(gate->returned.load());
}

void testACheckThatFinishesIsHandedOver() {
    // The ordinary path: poll() hands the outcome over exactly once and never
    // blocks while the work is still running.
    const auto gate = std::make_shared<CheckGate>();
    cascade::core::UpdateCheckTask task;
    cascade::core::UpdateCheckOutcome out;
    CHECK(!task.poll(out));  // nothing started
    task.start(gatedCheck(gate));
    CHECK(!task.poll(out));  // started, still stalled: no answer, no wait
    // A second start while one is in flight is ignored, not a second request.
    std::atomic<bool> secondRan{false};
    task.start([&secondRan] {
        secondRan.store(true);
        return cascade::core::UpdateCheckOutcome{};
    });
    openGate(*gate);
    bool got = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!got && std::chrono::steady_clock::now() < deadline) {
        got = task.poll(out);
        if (!got) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    }
    CHECK(got);
    CHECK(out.ok);
    CHECK(out.info.version == "9.9.9");
    CHECK(!task.running());
    CHECK(!task.poll(out));  // once
    CHECK(!secondRan.load());
    // Nothing running: reap is immediate and says so.
    CHECK(task.reap());
}

// --- TEAM MESSAGES (0.99.79) --------------------------------------------------------
//
// The owner (2026-10-14): "the broadcast system for the software should send the broadcast
// out even if there isn't an update; it should post to the user, then they click Seen and
// it doesn't show up again". Until 0.99.79 a broadcast rode inside the release notes and
// was shown only by the banner of an update that exists. The site now sends it as its own
// `messages` array, and these are the parser's half of that: every field, the case that
// motivated it (no update), and the answers that must be dropped without costing the
// manifest or the messages beside them.

void testTeamMessagesAreParsed() {
    const std::string m = withMessages(
        "[" + msg("k3f9x2ab", {"Server maintenance tonight.", "Back by 9."}, true, "2026-10-14") +
        "," + msg("q7w1z0cd", {"On holiday until the 20th."}, false, "2026-10-12") + "]");
    UpdateInfo info;
    std::string err;
    CHECK(cascade::core::parseUpdateManifest(m, "0.99.79", info, err));
    CHECK(err.empty());
    CHECK(info.messages.size() == 2u);
    if (info.messages.size() == 2u) {
        const TeamMessage& a = info.messages[0];
        const std::vector<std::string> aLines = {"Server maintenance tonight.", "Back by 9."};
        CHECK(a.id == "k3f9x2ab");
        CHECK(a.date == "2026-10-14");
        CHECK(a.lines == aLines);
        CHECK(a.important);
        // Order is the server's (newest first) and is kept.
        const TeamMessage& b = info.messages[1];
        const std::vector<std::string> bLines = {"On holiday until the 20th."};
        CHECK(b.id == "q7w1z0cd");
        CHECK(b.date == "2026-10-12");
        CHECK(b.lines == bLines);
        CHECK(!b.important);
    }

    // A service older than site 2.77.0 sends no such field, and an empty array is none:
    // both are an ordinary manifest.
    CHECK(cascade::core::parseUpdateManifest(manifest(), "0.50.0", info, err));
    CHECK(info.messages.empty());
    CHECK(cascade::core::parseUpdateManifest(withMessages("[]"), "0.99.79", info, err));
    CHECK(info.messages.empty());
}

void testAMessageIsParsedWhenThereIsNoUpdate() {
    // THE MOTIVATING BOUNDARY. A copy on the newest version is told "no update", and the
    // message must come through anyway - that is the whole request. Older than the running
    // build, the same build, and newer: the messages are the same in all three.
    const std::string one = "[" + msg("k3f9x2ab", {"Away until the 20th."}) + "]";
    struct Case {
        const char* offered;
        bool newer;
    };
    const Case cases[] = {{"0.56.0", false}, {"0.99.79", false}, {"99.0.0", true}};
    for (const Case& c : cases) {
        UpdateInfo info;
        std::string err;
        CHECK(cascade::core::parseUpdateManifest(withMessages(one, c.offered), "0.99.79", info,
                                                 err));
        if (info.newer != c.newer) {
            std::printf("FAIL offered %s against 0.99.79: newer=%d\n", c.offered,
                        info.newer ? 1 : 0);
        }
        CHECK(info.newer == c.newer);
        CHECK(info.messages.size() == 1u);
        // An "important" message is not a "critical" update: the two are separate, and a
        // message must not make an up-to-date copy read as one that needs replacing.
        CHECK(!info.critical);
    }

    // ...and an important one on a copy with nothing to update does not make it critical.
    UpdateInfo info;
    std::string err;
    CHECK(cascade::core::parseUpdateManifest(
        withMessages("[" + msg("imp1", {"Do not update yet."}, true) + "]", "0.99.79"),
        "0.99.79", info, err));
    CHECK(info.messages.size() == 1u && info.messages[0].important);
    CHECK(!info.newer);
    CHECK(!info.critical);
}

void testMalformedMessagesAreDroppedAndTheirNeighboursKept() {
    // Each of these is dropped ALONE - the manifest and the good messages around them
    // survive, the policy malformed notes already follow.
    const std::string longId(41, 'a');
    const std::string m = withMessages(
        "[" + msg("ok1", {"first"}) +                                  //
        ",{\"date\":\"2026-10-14\",\"lines\":[\"no id\"]}" +           // no id
        ",{\"id\":\"emptylines\",\"lines\":[]}" +                      // empty lines
        ",{\"id\":\"nolines\"}" +                                      // no lines at all
        ",{\"id\":\"blank\",\"lines\":[\"\",\"\"]}" +                  // only blank lines
        ",{\"id\":\"nonstr\",\"lines\":[1,null,{},[]]}" +              // no string among them
        ",{\"id\":\"a/b\",\"lines\":[\"slash\"]}" +                    // id with a slash
        ",{\"id\":\"a.b\",\"lines\":[\"dot\"]}" +                      // id with a dot
        ",{\"id\":\"..\",\"lines\":[\"dots\"]}" +                      // the parent directory
        ",{\"id\":\"a b\",\"lines\":[\"space\"]}" +                    // id with a space
        ",{\"id\":\"\",\"lines\":[\"empty id\"]}" +                    // empty id
        ",{\"id\":\"" + longId + "\",\"lines\":[\"long id\"]}" +       // 41 characters
        ",\"not an object\",7,null,[]," +                              // not objects
        "{\"id\":\"mixed\",\"lines\":[\"one\",5,null,\"\",\"two\"]}," + // strings kept
        msg("ok2", {"second"}) + "]");
    UpdateInfo info;
    std::string err;
    bool threw = false;
    bool ok = false;
    try {
        ok = cascade::core::parseUpdateManifest(m, "0.99.79", info, err);
    } catch (...) {
        threw = true;
    }
    CHECK(!threw);
    CHECK(ok);
    const std::vector<std::string> wantIds = {"ok1", "mixed", "ok2"};
    if (idsOf(info) != wantIds) {
        std::printf("FAIL kept %zu messages, ids:", info.messages.size());
        for (const std::string& id : idsOf(info)) { std::printf(" %s", id.c_str()); }
        std::printf("\n");
    }
    CHECK(idsOf(info) == wantIds);
    if (info.messages.size() == 3u) {
        // Only the strings of a mixed list, and not the blank one.
        const std::vector<std::string> mixed = {"one", "two"};
        CHECK(info.messages[1].lines == mixed);
    }
}

void testTeamMessageBounds() {
    using cascade::core::kMaxTeamMessageDateBytes;
    using cascade::core::kMaxTeamMessageLineBytes;
    using cascade::core::kMaxTeamMessageLines;
    using cascade::core::kMaxTeamMessages;
    CHECK(kMaxTeamMessages == 10u);
    CHECK(kMaxTeamMessageLines == 20u);
    CHECK(kMaxTeamMessageLineBytes == 1000u);
    CHECK(kMaxTeamMessageDateBytes == 32u);

    UpdateInfo info;
    std::string err;

    // ELEVEN messages keep TEN, the first ten.
    {
        std::string arr = "[";
        for (int i = 0; i < 11; ++i) {
            if (i != 0) { arr += ","; }
            arr += msg("m" + std::to_string(i), {"line"});
        }
        arr += "]";
        CHECK(cascade::core::parseUpdateManifest(withMessages(arr), "0.99.79", info, err));
        CHECK(info.messages.size() == kMaxTeamMessages);
        if (info.messages.size() == kMaxTeamMessages) {
            CHECK(info.messages.front().id == "m0");
            CHECK(info.messages.back().id == "m9");
        }
    }
    // The ten are the first ten USABLE ones: a malformed entry does not use up a place.
    {
        std::string arr = "[{\"id\":\"bad/1\",\"lines\":[\"x\"]},{\"id\":\"bad2\",\"lines\":[]}";
        for (int i = 0; i < 10; ++i) { arr += "," + msg("g" + std::to_string(i), {"line"}); }
        arr += "]";
        CHECK(cascade::core::parseUpdateManifest(withMessages(arr), "0.99.79", info, err));
        CHECK(info.messages.size() == kMaxTeamMessages);
        if (info.messages.size() == kMaxTeamMessages) { CHECK(info.messages.back().id == "g9"); }
    }

    // TWENTY-FIVE lines keep TWENTY, the first twenty.
    {
        std::vector<std::string> lines;
        for (int i = 0; i < 25; ++i) { lines.push_back("line " + std::to_string(i)); }
        CHECK(cascade::core::parseUpdateManifest(
            withMessages("[" + msg("many", lines) + "]"), "0.99.79", info, err));
        CHECK(info.messages.size() == 1u);
        if (info.messages.size() == 1u) {
            CHECK(info.messages[0].lines.size() == kMaxTeamMessageLines);
            if (info.messages[0].lines.size() == kMaxTeamMessageLines) {
                CHECK(info.messages[0].lines.front() == "line 0");
                CHECK(info.messages[0].lines.back() == "line 19");
            }
        }
    }

    // A LINE IS CLIPPED TO 1000 BYTES, on a UTF-8 boundary.
    {
        const std::string exact(1000, 'x');
        const std::string over(3000, 'y');
        // 998 ASCII bytes + a two-byte letter: exactly 1000, nothing to clip.
        const std::string twoByteFits = std::string(998, 'a') + "\xC3\xA9";
        // 999 ASCII bytes + a three-byte euro sign: 1002 bytes, and byte 1000 is in the
        // middle of the euro, so the whole character goes and 999 bytes remain.
        const std::string euroStraddles = std::string(999, 'b') + "\xE2\x82\xAC";
        // A line of two-byte letters, 1200 bytes: clipped to 1000 (500 letters) and not 999.
        std::string accents;
        for (int i = 0; i < 600; ++i) { accents += "\xC3\xA9"; }
        CHECK(cascade::core::parseUpdateManifest(
            withMessages("[" + msg("clip", {exact, over, twoByteFits, euroStraddles, accents}) +
                         "]"),
            "0.99.79", info, err));
        CHECK(info.messages.size() == 1u);
        if (info.messages.size() == 1u && info.messages[0].lines.size() == 5u) {
            const std::vector<std::string>& l = info.messages[0].lines;
            CHECK(l[0] == exact);
            CHECK(l[1].size() == kMaxTeamMessageLineBytes);
            CHECK(l[1] == std::string(1000, 'y'));
            CHECK(l[2] == twoByteFits);
            CHECK(l[3] == std::string(999, 'b'));
            CHECK(l[4].size() == 1000u);
            // No lead byte without its continuation: the clip never leaves half a letter.
            CHECK(l[4].size() % 2 == 0 && l[4].compare(l[4].size() - 2, 2, "\xC3\xA9") == 0);
        } else {
            std::printf("FAIL the clipped message did not come back with five lines\n");
            CHECK(false);
        }
    }

    // THE DATE is informational and clipped to 32 bytes.
    {
        const std::string longDate(60, '2');
        CHECK(cascade::core::parseUpdateManifest(
            withMessages("[" + msg("dated", {"line"}, false, longDate) + "]"), "0.99.79", info,
            err));
        CHECK(info.messages.size() == 1u);
        if (info.messages.size() == 1u) {
            CHECK(info.messages[0].date == std::string(kMaxTeamMessageDateBytes, '2'));
        }
    }
}

void testWrongTypedMessageFieldsAreNotAnError() {
    // The manifest is parsed on the check's worker thread and collected with future::get(),
    // which rethrows: nothing in "messages" may throw, whatever type it is.
    const char* const shapes[] = {
        "5", "\"text\"", "null", "true", "{\"id\":\"a\",\"lines\":[\"b\"]}",
        "[{\"id\":5,\"lines\":[\"x\"]}]",
        "[{\"id\":null,\"lines\":[\"x\"]}]",
        "[{\"id\":[\"a\"],\"lines\":[\"x\"]}]",
        "[{\"id\":\"a\",\"lines\":\"x\"}]",
        "[{\"id\":\"a\",\"lines\":{\"0\":\"x\"}}]",
        "[{\"id\":\"a\",\"lines\":null}]",
    };
    for (const char* s : shapes) {
        UpdateInfo info;
        std::string err;
        bool threw = false;
        bool ok = false;
        try {
            ok = cascade::core::parseUpdateManifest(withMessages(s), "0.99.79", info, err);
        } catch (...) {
            threw = true;
        }
        if (threw) { std::printf("FAIL parseUpdateManifest threw on messages: %s\n", s); }
        CHECK(!threw);
        // The manifest's download fields are good, so the manifest stands...
        CHECK(ok);
        // ...and none of those is a message.
        if (!info.messages.empty()) { std::printf("FAIL kept a message out of: %s\n", s); }
        CHECK(info.messages.empty());
    }

    // A wrong-typed OPTIONAL field costs only itself: the message is kept without it.
    UpdateInfo info;
    std::string err;
    bool threw = false;
    bool ok = false;
    try {
        ok = cascade::core::parseUpdateManifest(
            withMessages("[{\"id\":\"a\",\"date\":5,\"important\":\"yes\",\"lines\":[\"kept\"]},"
                         "{\"id\":\"b\",\"date\":null,\"important\":null,\"lines\":[\"kept\"]}]"),
            "0.99.79", info, err);
    } catch (...) {
        threw = true;
    }
    CHECK(!threw);
    CHECK(ok);
    CHECK(info.messages.size() == 2u);
    for (const TeamMessage& t : info.messages) {
        CHECK(t.date.empty());
        CHECK(!t.important);
    }
}

void testMessageIdGrammar() {
    using cascade::core::wellFormedMessageId;
    CHECK(wellFormedMessageId("k3f9x2ab"));  // what the server's randomID(8) makes
    CHECK(wellFormedMessageId("a"));
    CHECK(wellFormedMessageId("A-Z_0-9"));
    CHECK(wellFormedMessageId(std::string(40, 'a')));  // exactly at the bound

    const std::string hostile[] = {
        "",                       // nothing at all
        "..",                     // the parent directory
        ".",                      //
        "a/b",                    // a separator
        "a\\b",                   //
        "a.b",                    // a dot
        "a b",                    // a space
        "a\"b",                   // a quote, which would break the JSON it is written into
        "a\nb",                   // a control character
        "caf\xC3\xA9",            // a letter outside ASCII
        std::string(41, 'a'),     // one over the bound
        std::string("a\0b", 3),   // an embedded NUL
    };
    for (const std::string& id : hostile) {
        if (wellFormedMessageId(id)) {
            std::printf("FAIL wellFormedMessageId accepted \"%s\" (%zu bytes)\n", id.c_str(),
                        id.size());
        }
        CHECK(!wellFormedMessageId(id));
    }
}

void setEnvVar(const char* name, const std::string& value) {
#ifdef _WIN32
    ::SetEnvironmentVariableA(name, value.empty() ? nullptr : value.c_str());
    _putenv_s(name, value.c_str());
#else
    if (value.empty()) {
        ::unsetenv(name);
    } else {
        ::setenv(name, value.c_str(), 1);
    }
#endif
}

void testTheCannedManifestSeam() {
    // FOXSDR_FAKE_UPDATE_MANIFEST makes checkForUpdate read a file instead of the network
    // (the application's --frames runs cannot reach foxsdr.com: fetchText refuses anything
    // that is not https). The endpoint below is a `.invalid` name that can never resolve, and
    // the empty one is refused before any request: an answer that comes back is, by
    // construction, the file's.
    const fs::path dir = fs::temp_directory_path() / ("cascade-updater-fake-" +
                                                      std::to_string(TEST_GETPID()));
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path file = dir / "manifest.json";
    {
        std::ofstream f(file, std::ios::binary | std::ios::trunc);
        f << withMessages("[" + msg("k3f9x2ab", {"Canned."}, true) + "]", "0.56.0");
    }

    // Unset or empty: off.
    setEnvVar("FOXSDR_FAKE_UPDATE_MANIFEST", "");
    CHECK(!cascade::core::updateManifestFakeSet());
    UpdateInfo info;
    std::string err;
    // Off, an empty endpoint is the ordinary refusal - nothing was read from anywhere.
    CHECK(!cascade::core::checkForUpdate("", "0.99.79", "", info, err));
    CHECK(err == "no update endpoint configured");

    // Set: on, and the file's manifest is the answer.
    setEnvVar("FOXSDR_FAKE_UPDATE_MANIFEST", file.string());
    CHECK(cascade::core::updateManifestFakeSet());
    CHECK(cascade::core::checkForUpdate("https://foxsdr.invalid/api/update", "0.99.79", "", info,
                                        err));
    CHECK(err.empty());
    CHECK(!info.newer);  // 0.56.0 against 0.99.79
    CHECK(info.messages.size() == 1u);
    if (info.messages.size() == 1u) {
        CHECK(info.messages[0].id == "k3f9x2ab");
        CHECK(info.messages[0].important);
    }
    // No URL is built at all, so not even an empty endpoint matters.
    info = UpdateInfo{};
    CHECK(cascade::core::checkForUpdate("", "0.99.79", "", info, err));
    CHECK(info.messages.size() == 1u);

    // A file that is not there is an ordinary FAILED check, with its own words.
    setEnvVar("FOXSDR_FAKE_UPDATE_MANIFEST", (dir / "absent.json").string());
    CHECK(cascade::core::updateManifestFakeSet());
    CHECK(!cascade::core::checkForUpdate("https://foxsdr.invalid/api/update", "0.99.79", "",
                                         info, err));
    CHECK(err == "the canned update manifest could not be read");
    CHECK(info.messages.empty());

    // A file that is not a manifest fails the way the real answer would.
    const fs::path junk = dir / "junk.json";
    {
        std::ofstream f(junk, std::ios::binary | std::ios::trunc);
        f << "this is not json";
    }
    setEnvVar("FOXSDR_FAKE_UPDATE_MANIFEST", junk.string());
    CHECK(!cascade::core::checkForUpdate("https://foxsdr.invalid/api/update", "0.99.79", "",
                                         info, err));
    CHECK(err.find("not JSON") != std::string::npos);

    setEnvVar("FOXSDR_FAKE_UPDATE_MANIFEST", "");
    CHECK(!cascade::core::updateManifestFakeSet());
    fs::remove_all(dir, ec);
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
    testWrongTypedFieldsAreRefusedNotThrown();
    testNotesSurviveAndMalformedOnesAreDropped();
    testDownloadRefusesWithoutAnUpdate();
    testDownloadHonoursACancelFlag();
    testACheckThatFinishesIsHandedOver();
    testAStalledCheckDoesNotHoldTeardown();
    testTeamMessagesAreParsed();
    testAMessageIsParsedWhenThereIsNoUpdate();
    testMalformedMessagesAreDroppedAndTheirNeighboursKept();
    testTeamMessageBounds();
    testWrongTypedMessageFieldsAreNotAnError();
    testMessageIdGrammar();
    testTheCannedManifestSeam();
    testEndpointIsHttpsAndOnTheProjectsDomain();
    return testSummary("test_updater");
}
