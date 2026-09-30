// Tests for core/tester_link.hpp - the beta-tester PORTAL LINK: URL parsing,
// the one-shot link-request file, the single-instance guard, and the two
// network calls (confirm-by-name, migration exchange) against a local
// server, exactly test_tester_usage.cpp's discipline applied to this second,
// independent flow.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/tester_link.hpp"
#include "core/tester_usage.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <winsock2.h>

#include <windows.h>

#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
// Must match every other TU that includes httplib.h on non-Windows (see
// test_telemetry.cpp's own comment on this exact requirement): without it
// this file's httplib::ClientImpl has a different layout from crash_upload.cpp's,
// and the first real request died with std::bad_alloc inside
// create_client_socket.
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include <httplib.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

using cascade::core::BetaLinkNameSender;
using cascade::core::BetaLinkOutcome;
using cascade::core::BetaMigrationOutcome;
using cascade::core::BetaMigrationSender;
using cascade::core::UploadCancel;

namespace {

// ---------------------------------------------------------------------------
// parseBetaLinkUrl - table-driven, pure function, no network
// ---------------------------------------------------------------------------

void testParseBetaLinkUrlExactShapeOnly() {
    const std::string good = "foxsdr://beta?t=" + std::string(40, 'a');
    CHECK(cascade::core::parseBetaLinkUrl(good) == std::string(40, 'a'));

    // Tolerates exactly one layer of surrounding matching quotes - a
    // shell/shortcut quoting artefact around argv[1].
    CHECK(cascade::core::parseBetaLinkUrl("\"" + good + "\"") == std::string(40, 'a'));
    CHECK(cascade::core::parseBetaLinkUrl("'" + good + "'") == std::string(40, 'a'));
    // Mismatched quotes are not "one layer" - left as part of the content and
    // so rejected.
    CHECK(cascade::core::parseBetaLinkUrl("\"" + good + "'").empty());

    // Wrong scheme.
    CHECK(cascade::core::parseBetaLinkUrl("http://beta?t=" + std::string(40, 'a')).empty());
    CHECK(cascade::core::parseBetaLinkUrl("foxsdr2://beta?t=" + std::string(40, 'a')).empty());

    // THE SHAPE WINDOWS ACTUALLY DELIVERS. The portal navigates to
    // foxsdr://beta?t=<code>, but ShellExecute - and so every browser's
    // hand-off on Windows - normalises the empty path after the "beta"
    // authority to "/" and starts the handler with foxsdr://beta/?t=<code>
    // (seen on the owner's own 0.99.50 install, 2026-09-29: the link was
    // dropped as "unrecognised" and no prompt ever appeared). Both spellings
    // are the same URL and both must be accepted.
    CHECK(cascade::core::parseBetaLinkUrl("foxsdr://beta/?t=" + std::string(40, 'a')) ==
          std::string(40, 'a'));
    CHECK(cascade::core::parseBetaLinkUrl("\"foxsdr://beta/?t=" + std::string(40, 'a') + "\"") ==
          std::string(40, 'a'));
    // ...and ONLY that one slash: anything else there is still a path.
    CHECK(cascade::core::parseBetaLinkUrl("foxsdr://beta//?t=" + std::string(40, 'a')).empty());
    CHECK(cascade::core::parseBetaLinkUrl("foxsdr://beta/?t=" + std::string(40, 'a') + "/")
              .empty());

    // A path segment.
    CHECK(cascade::core::parseBetaLinkUrl("foxsdr://beta/x?t=" + std::string(40, 'a')).empty());

    // An extra query parameter, either side of the token.
    CHECK(cascade::core::parseBetaLinkUrl("foxsdr://beta?t=" + std::string(40, 'a') + "&x=1")
              .empty());
    CHECK(cascade::core::parseBetaLinkUrl("foxsdr://beta?x=1&t=" + std::string(40, 'a')).empty());

    // Uppercase hex - hex.EncodeToString never emits it, so it is refused
    // rather than normalised (a corrupted copy must fail loudly).
    CHECK(cascade::core::parseBetaLinkUrl("foxsdr://beta?t=" + std::string(40, 'A')).empty());

    // Wrong length (32 hex - a PORTAL token, not an app token) - the two
    // shapes must never be confused.
    CHECK(cascade::core::parseBetaLinkUrl("foxsdr://beta?t=" + std::string(32, 'a')).empty());

    // Empty token.
    CHECK(cascade::core::parseBetaLinkUrl("foxsdr://beta?t=").empty());

    // Embedded control byte / literal %00 (three ASCII chars, not a real
    // NUL - argv cannot carry an actual NUL) both fail: the control-byte scan
    // catches a raw one, and "%00" simply is not valid hex so the length or
    // hex check refuses it either way.
    {
        std::string withNul = "foxsdr://beta?t=" + std::string(39, 'a');
        withNul.push_back('\0');  // 40th "digit" is a NUL, not hex
        CHECK(cascade::core::parseBetaLinkUrl(withNul).empty());
    }
    CHECK(cascade::core::parseBetaLinkUrl("foxsdr://beta?t=%00" + std::string(37, 'a')).empty());

    // Oversized.
    CHECK(cascade::core::parseBetaLinkUrl(std::string(200, 'x')).empty());

    // Empty input.
    CHECK(cascade::core::parseBetaLinkUrl("").empty());
}

// ---------------------------------------------------------------------------
// validAppToken / extractAppToken
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// sanitizeTesterName - the tester's own free-text sign-up name (finding 6):
// control characters/newlines stripped, length capped, and (at the app_window
// layer, tests/test_tester_link_app.cpp) an empty result shown distinctly.
// ---------------------------------------------------------------------------

void testSanitizeTesterNameStripsControlCharactersAndNewlines() {
    CHECK(cascade::core::sanitizeTesterName("Ada Lovelace") == "Ada Lovelace");
    // A newline is exactly how a free-text name could inject a fake second
    // line into a single-line prompt ("Link this FoxSDR to beta tester ...").
    CHECK(cascade::core::sanitizeTesterName("Ada\nLovelace") == "AdaLovelace");
    CHECK(cascade::core::sanitizeTesterName("Ada\r\nLovelace\t!") == "AdaLovelace!");
    // Every C0 control byte and DEL, not only the common ones.
    std::string withControls = "A";
    for (unsigned char c = 0; c < 0x20; ++c) { withControls.push_back(static_cast<char>(c)); }
    withControls.push_back(static_cast<char>(0x7f));
    withControls += "B";
    CHECK(cascade::core::sanitizeTesterName(withControls) == "AB");
}

void testSanitizeTesterNameCapsLengthWithoutSplittingUtf8() {
    const std::string exact(cascade::core::kMaxTesterNameBytes, 'x');
    CHECK(cascade::core::sanitizeTesterName(exact) == exact);
    const std::string over(cascade::core::kMaxTesterNameBytes + 10, 'y');
    const std::string cut = cascade::core::sanitizeTesterName(over);
    CHECK(cut.size() <= cascade::core::kMaxTesterNameBytes);
    CHECK(cut == std::string(cascade::core::kMaxTesterNameBytes, 'y'));
    // A cut that would split a multi-byte UTF-8 character drops the whole
    // character rather than emitting a malformed tail byte: the 2-byte
    // character straddling byte 64 (its lead byte at 63, its continuation
    // byte at 64) must come off entirely, leaving 63 bytes, not 64 ending in
    // a bare continuation byte.
    std::string utf8Over(cascade::core::kMaxTesterNameBytes - 1, 'z');
    utf8Over += "\xC3\xA9";  // e-acute, straddling the cut point
    const std::string utf8Cut = cascade::core::sanitizeTesterName(utf8Over);
    CHECK(utf8Cut.size() == cascade::core::kMaxTesterNameBytes - 1);
    CHECK(utf8Cut == std::string(cascade::core::kMaxTesterNameBytes - 1, 'z'));
}

void testSanitizeTesterNameEmptyOrAllControlIsEmpty() {
    // The distinct "no name set" display (drawTesterLinkPrompt,
    // drawTesterUsageSection) is keyed on exactly this: an empty result,
    // whether from an empty sign-up name or one that was nothing but control
    // bytes.
    CHECK(cascade::core::sanitizeTesterName("").empty());
    CHECK(cascade::core::sanitizeTesterName("\n\r\t").empty());
    CHECK(cascade::core::sanitizeTesterName("   ").empty());  // trimmed trailing spaces, nothing left
}

void testValidAppTokenIsFortyHexNeverThirtyTwo() {
    CHECK(cascade::core::validAppToken(std::string(40, 'a')));
    CHECK(cascade::core::validAppToken("0123456789abcdef0123456789abcdef01234567"));
    CHECK(!cascade::core::validAppToken(std::string(32, 'a')));  // a PORTAL token's shape
    CHECK(!cascade::core::validAppToken(std::string(40, 'A')));
    CHECK(!cascade::core::validAppToken(std::string(39, 'a')));
    CHECK(!cascade::core::validAppToken(""));
}

void testExtractAppTokenTrimsAndValidatesOnly() {
    CHECK(cascade::core::extractAppToken("  " + std::string(40, 'a') + "\n") ==
          std::string(40, 'a'));
    CHECK(cascade::core::extractAppToken(std::string(32, 'a')).empty());  // portal shape refused
    CHECK(cascade::core::extractAppToken("https://foxsdr.com/#t=" + std::string(40, 'a'))
              .empty());  // no fragment parsing for app tokens
}

// ---------------------------------------------------------------------------
// TesterUsageQueue::rewriteToken
// ---------------------------------------------------------------------------

std::string reportWithToken(const std::string& tok) {
    nlohmann::json j;
    j["token"] = tok;
    j["version"] = "1.0";
    return j.dump();
}

void testRewriteTokenRelabelsEveryItem() {
    cascade::core::TesterUsageQueue q;
    q.push(reportWithToken(std::string(32, 'a')));
    q.push(reportWithToken(std::string(32, 'a')));
    CHECK(q.size() == 2);
    q.rewriteToken(std::string(40, 'b'));
    CHECK(q.size() == 2);  // nothing dropped - relabelled, not filtered
    for (const std::string& item : q.items()) {
        CHECK(cascade::core::tokenOfReport(item) == std::string(40, 'b'));
    }
}

void testRewriteTokenLeavesUnparsableItemsAlone() {
    cascade::core::TesterUsageQueue q;
    q.push("not json");
    q.rewriteToken(std::string(40, 'c'));
    CHECK(q.size() == 1);
    CHECK(q.items()[0] == "not json");  // untouched, not dropped
}

// ---------------------------------------------------------------------------
// The one-shot link-request file
// ---------------------------------------------------------------------------

fs::path uniqueDir(const char* tag) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
#if defined(_WIN32)
    const auto pid = ::GetCurrentProcessId();
#else
    const auto pid = ::getpid();
#endif
    return fs::temp_directory_path() /
           (std::string("cascade-tester-link-") + tag + "-" + std::to_string(pid) + "-" +
            std::to_string(stamp));
}

void testWriteThenClaimRoundTrips() {
    const fs::path dir = uniqueDir("roundtrip");
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string tok = std::string(40, 'd');

    CHECK(cascade::core::writeLinkRequestFile(dir.string(), tok));
    CHECK(fs::exists(cascade::core::linkRequestPath(dir.string())));

    const std::string claimed = cascade::core::claimLinkRequestFile(dir.string());
    CHECK(claimed == tok);
    // Claimed exactly once: the file is gone, and a second claim finds
    // nothing.
    CHECK(!fs::exists(cascade::core::linkRequestPath(dir.string())));
    CHECK(cascade::core::claimLinkRequestFile(dir.string()).empty());

    fs::remove_all(dir, ec);
}

void testWriteRefusesAnInvalidToken() {
    const fs::path dir = uniqueDir("badtoken");
    std::error_code ec;
    fs::create_directories(dir, ec);
    CHECK(!cascade::core::writeLinkRequestFile(dir.string(), std::string(32, 'a')));  // portal shape
    CHECK(!fs::exists(cascade::core::linkRequestPath(dir.string())));
    fs::remove_all(dir, ec);
}

void testClaimOfAStaleFileDeletesItUnread() {
    const fs::path dir = uniqueDir("stale");
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string tok = std::string(40, 'e');
    CHECK(cascade::core::writeLinkRequestFile(dir.string(), tok));

    // `now` well past the file's real write time, past the 24h bound -
    // claimed and discarded, never returned.
    const std::time_t farFuture = std::time(nullptr) + cascade::core::kLinkRequestMaxAgeSec + 3600;
    const std::string claimed = cascade::core::claimLinkRequestFile(dir.string(), farFuture);
    CHECK(claimed.empty());
    CHECK(!fs::exists(cascade::core::linkRequestPath(dir.string())));  // claimed either way

    fs::remove_all(dir, ec);
}

void testClaimWithNothingThereIsEmpty() {
    const fs::path dir = uniqueDir("empty");
    std::error_code ec;
    fs::create_directories(dir, ec);
    CHECK(cascade::core::claimLinkRequestFile(dir.string()).empty());
    fs::remove_all(dir, ec);
}

void testClaimOfGarbageContentIsEmptyButStillConsumesTheFile() {
    const fs::path dir = uniqueDir("garbage");
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::create_directories(dir, ec);
    {
        std::ofstream f(cascade::core::linkRequestPath(dir.string()), std::ios::binary);
        f << "not a token";
    }
    CHECK(cascade::core::claimLinkRequestFile(dir.string()).empty());
    CHECK(!fs::exists(cascade::core::linkRequestPath(dir.string())));
    fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// claimPrimaryInstanceAt - the two-call-in-one-process trick: the mutex/lock
// identity is what is being tested, not cross-process behaviour, so calling
// it twice for the SAME identity from one test binary is exactly what
// exercises "a second claimant sees it is already held" (a real second
// process would see the identical ERROR_ALREADY_EXISTS/EWOULDBLOCK either
// way - CreateMutex and flock() do not distinguish same-process from
// cross-process callers).
// ---------------------------------------------------------------------------

void testClaimPrimaryInstanceFirstWinsSecondDoesNot() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
#if defined(_WIN32)
    const std::string identity = "Local\\FoxSDR-instance-test-" + std::to_string(stamp);
#else
    const fs::path dir = uniqueDir("instance");
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string identity = (dir / "instance.lock").string();
#endif
    CHECK(cascade::core::claimPrimaryInstanceAt(identity));
    CHECK(!cascade::core::claimPrimaryInstanceAt(identity));
#if !defined(_WIN32)
    fs::remove_all(dir, ec);
#endif
}

void testClaimPrimaryInstanceDifferentIdentitiesAreIndependent() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
#if defined(_WIN32)
    const std::string a = "Local\\FoxSDR-instance-test-a-" + std::to_string(stamp);
    const std::string b = "Local\\FoxSDR-instance-test-b-" + std::to_string(stamp);
#else
    const fs::path dir = uniqueDir("instance2");
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string a = (dir / "a.lock").string();
    const std::string b = (dir / "b.lock").string();
#endif
    CHECK(cascade::core::claimPrimaryInstanceAt(a));
    CHECK(cascade::core::claimPrimaryInstanceAt(b));  // unrelated identity, unaffected
#if !defined(_WIN32)
    fs::remove_all(dir, ec);
#endif
}

// ---------------------------------------------------------------------------
// primaryInstanceIdentity - the test-only instance scope. PRODUCT BEHAVIOUR
// PINNED FIRST: with no CASCADE_CONFIG_TEST hook the identity is byte-for-byte
// the fixed name every shipped copy of FoxSDR claims, whatever
// FOXSDR_INSTANCE_SCOPE says - a stray env var must never split one user's
// copies into ones that cannot hand a link to each other.
// ---------------------------------------------------------------------------

#if defined(_WIN32)
const std::string kFixedIdentity = "Local\\FoxSDR-instance";
std::string scopedIdentity(const std::string& /*configDir*/, const std::string& scope) {
    return "Local\\FoxSDR-instance-" + scope;
}
#else
const std::string kFixedIdentity = "/home/u/.config/foxsdr/instance.lock";
std::string scopedIdentity(const std::string& configDir, const std::string& scope) {
    return configDir + "/instance-" + scope + ".lock";
}
#endif

void testPrimaryInstanceIdentityIsTheFixedNameWithoutTheTestHook() {
    using cascade::core::primaryInstanceIdentity;
    const std::string cfg = "/home/u/.config/foxsdr";
    CHECK(primaryInstanceIdentity(cfg, "", "") == kFixedIdentity);
    CHECK(primaryInstanceIdentity(cfg, "", "scope1") == kFixedIdentity);  // no hook: ignored
#if defined(_WIN32)
    // The literal, not a constant shared with the product: this IS the name.
    CHECK(primaryInstanceIdentity("", "", "") == std::string("Local\\FoxSDR-instance"));
#else
    CHECK(primaryInstanceIdentity("", "", "") == "/tmp/foxsdr-instance.lock");
    CHECK(primaryInstanceIdentity("", "", "scope1") == "/tmp/foxsdr-instance.lock");
#endif
}

void testPrimaryInstanceIdentityTakesAValidScopeOnlyWithTheTestHook() {
    using cascade::core::primaryInstanceIdentity;
    const std::string cfg = "/home/u/.config/foxsdr";
    const std::string hook = "/tmp/x/config.json";
    CHECK(primaryInstanceIdentity(cfg, hook, "tl-123_ab") == scopedIdentity(cfg, "tl-123_ab"));
    // A hook with no scope is an ordinary config-test run: the fixed name.
    CHECK(primaryInstanceIdentity(cfg, hook, "") == kFixedIdentity);
#if !defined(_WIN32)
    CHECK(primaryInstanceIdentity("", hook, "s") == "/tmp/foxsdr-instance-s.lock");
#endif
    // The boundary: exactly kMaxInstanceScopeChars is taken, one more is not.
    const std::string longest(cascade::core::kMaxInstanceScopeChars, 'a');
    CHECK(primaryInstanceIdentity(cfg, hook, longest) == scopedIdentity(cfg, longest));
    CHECK(primaryInstanceIdentity(cfg, hook, longest + "a") == kFixedIdentity);
    // Anything that could change which kernel object / file is named, or
    // escape the directory, is ignored outright - never sanitised into
    // something else.
    for (const std::string bad : {"a\\b", "a/b", "..", "a.b", "a b", " a", "a\n", "Global\\x"}) {
        CHECK(primaryInstanceIdentity(cfg, hook, bad) == kFixedIdentity);
    }
}

// The wiring, not just the name: a scoped claimPrimaryInstance() really holds
// the scoped identity (a second claimant of that exact name is refused).
void testClaimPrimaryInstanceHoldsTheScopedIdentity() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string scope = "unit-" + std::to_string(stamp);
    const fs::path dir = uniqueDir("scoped");
    std::error_code ec;
    fs::create_directories(dir, ec);
    CHECK(cascade::core::claimPrimaryInstance(dir.string(), "hook", scope));
    CHECK(!cascade::core::claimPrimaryInstanceAt(scopedIdentity(dir.string(), scope)));
    fs::remove_all(dir, ec);
}

// ---------------------------------------------------------------------------
// betaApiBaseUrl override
// ---------------------------------------------------------------------------

void testBetaApiBaseUrlOverride() {
#if defined(_WIN32)
    ::SetEnvironmentVariableA("FOXSDR_BETA_API_URL", "http://127.0.0.1:9");
    CHECK(cascade::core::betaApiBaseUrl() == "http://127.0.0.1:9");
    ::SetEnvironmentVariableA("FOXSDR_BETA_API_URL", nullptr);
#else
    ::setenv("FOXSDR_BETA_API_URL", "http://127.0.0.1:9", 1);
    CHECK(cascade::core::betaApiBaseUrl() == "http://127.0.0.1:9");
    ::unsetenv("FOXSDR_BETA_API_URL");
#endif
    CHECK(cascade::core::betaApiBaseUrl() == "https://foxsdr.com");
}

// ---------------------------------------------------------------------------
// THE REAL MAIN PATH: a link clicked with FoxSDR closed, through the actual
// argv/main.cpp handling - not core::parseBetaLinkUrl in isolation.
//
// THE BUG this exists for: argv[1] was consumed as a link (the file written,
// diagLogf'd by length only), but the flag-parsing loop a few lines later
// started at argv[1] again - so on a fresh launch (no other instance running,
// which is exactly claimPrimaryInstance()'s "continue as normal launch" path)
// argv[1] reached the unknown-argument branch, which printed the WHOLE
// "foxsdr://beta?t=<token>" URL - the credential itself - to stderr and
// returned 1. A tester clicking their own portal link with FoxSDR not already
// open got an instant crash-looking exit instead of the start-up prompt
// PRIVACY.md promises.
// ---------------------------------------------------------------------------

#if defined(_WIN32)

// EVERY SPAWN BELOW RUNS IN THIS PROCESS'S OWN INSTANCE SCOPE. The single-
// instance mutex is per logon session, so without this any other cascade.exe
// on the desktop - the owner's own FoxSDR, or another checkout's ctest running
// app smoke tests at the same moment - was "the running instance": the spawned
// copy handed the link to it and exited without rendering, failing the
// no-instance case at its frame and file checks. Proven 2026-09-30 with one
// isolated --frames copy running beside this test. CASCADE_CONFIG_TEST is set
// by every spawn too, which is what lets the app honour the scope at all.
const std::string& spawnInstanceScope() {
    static const std::string scope =
        "tl-" + std::to_string(::GetCurrentProcessId()) + "-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    return scope;
}

void testLinkActivationWithNoInstanceRunningLaunchesNormallyAndNeverLeaksTheToken() {
    const fs::path dir = uniqueDir("mainpath");
    std::error_code ec;
    fs::create_directories(dir, ec);
    // APPDATA drives core::ConfigStore::defaultPath(), which is what
    // main.cpp's link handling ITSELF uses (never CASCADE_CONFIG_TEST) to
    // decide the link-request file's directory - see main.cpp's own comment
    // on claimPrimaryInstance(). CASCADE_CONFIG_TEST is set to the SAME
    // resolved config.json path so AppWindow's own configPath_ (and so
    // testerLinkPoll's ~1 Hz file poll) looks in the identical directory,
    // rather than staying hermetic the way a bare --frames run otherwise
    // would (main.cpp: bounded runs are hermetic unless this hook is set).
    const fs::path foxsdrDir = dir / "foxsdr";
    const fs::path cfgPath = foxsdrDir / "config.json";
    const std::string tok = std::string(40, 'b');
    const std::string url = "foxsdr://beta?t=" + tok;

    ::SetEnvironmentVariableA("APPDATA", dir.string().c_str());
    ::SetEnvironmentVariableA("LOCALAPPDATA", dir.string().c_str());
    ::SetEnvironmentVariableA("CASCADE_CONFIG_TEST", cfgPath.string().c_str());
    ::SetEnvironmentVariableA("FOXSDR_INSTANCE_SCOPE", spawnInstanceScope().c_str());
    // Every network-facing endpoint this run could reach, pointed at a port
    // nothing listens on - this test proves argv handling and the file, not
    // any network exchange, and a fresh isolated profile must never reach
    // foxsdr.com for real.
    for (const char* v : {"FOXSDR_BETA_API_URL", "FOXSDR_TESTER_USAGE_URL", "FOXSDR_TELEMETRY_URL",
                          "FOXSDR_UPDATE_URL", "FOXSDR_CRASH_URL", "FOXSDR_FEATURE_URL",
                          "FOXSDR_PROBLEM_URL", "FOXSDR_REPORTS_URL"}) {
        ::SetEnvironmentVariableA(v, "http://127.0.0.1:9");
    }

    CHECK(!fs::exists(foxsdrDir));  // nothing pre-exists to seed a false pass

    const std::string exe = std::string(CASCADE_APP_BINDIR) + "/cascade.exe";
    const std::string cmd = "\"\"" + exe + "\" \"" + url + "\" --frames 20 2>&1\"";
    std::string out;
    FILE* p = _popen(cmd.c_str(), "r");
    CHECK(p != nullptr);
    char buf[512];
    while (p != nullptr && std::fgets(buf, sizeof(buf), p) != nullptr) { out += buf; }
    const int exitCode = (p != nullptr) ? _pclose(p) : -1;

    for (const char* v : {"APPDATA", "LOCALAPPDATA", "CASCADE_CONFIG_TEST", "FOXSDR_BETA_API_URL",
                          "FOXSDR_TESTER_USAGE_URL", "FOXSDR_TELEMETRY_URL", "FOXSDR_UPDATE_URL",
                          "FOXSDR_CRASH_URL", "FOXSDR_FEATURE_URL", "FOXSDR_PROBLEM_URL",
                          "FOXSDR_REPORTS_URL", "FOXSDR_INSTANCE_SCOPE"}) {
        ::SetEnvironmentVariableA(v, nullptr);
    }

    std::printf("link activation (no instance running) exit=%d, output:\n%s\n", exitCode,
               out.c_str());

    // THE HEADLINE FIX: a normal launch, not the "unknown argument" exit 1.
    CHECK(exitCode == 0);
    CHECK(out.find("rendered 20 frames") != std::string::npos);
    CHECK(out.find("unknown argument") == std::string::npos);

    // NEVER THE TOKEN, and never the raw URL that carries it, anywhere in
    // stdout or stderr - the whole point of diagLogf'ing only its length.
    CHECK(out.find(tok) == std::string::npos);
    CHECK(out.find("foxsdr://beta") == std::string::npos);

    // THE FILE WAS ACTUALLY CONSUMED: main.cpp wrote it, and this run's own
    // AppWindow (configPath_ pointed at the same directory) claimed it on its
    // very first poll - testerLinkPollLast_ starts far enough in the past
    // that frame 1 already checks. Gone means claimed, whatever the (network-
    // isolated, doomed-to-fail) confirm-by-name lookup went on to do with it.
    CHECK(!fs::exists(cascade::core::linkRequestPath(foxsdrDir.string())));
    // And the directory itself must exist - writeLinkRequestFile creates it,
    // so its absence would mean the write path was never reached at all.
    CHECK(fs::exists(foxsdrDir));

    if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
}

// A foxsdr: link in ANY other shape - upper-casing the scheme, appending a
// parameter, no "//beta?t=" at all - is not a link FoxSDR accepts, but it
// still carries the token. It used to fall through to the flag loop's
// unknown-argument branch, which printed the whole URL, token included, to
// stderr and exited 1. It must instead be dropped unechoed and the launch
// carry on; nothing is written, since it is not a link we trust.
// (foxsdr://beta/?t= used to be listed here. It is the shape Windows actually
// delivers - see testLinkShapesWindowsDeliversReachTheRunningInstance.)
void testMalformedLinkIsDroppedWithoutEchoingTheToken() {
    const std::string tok = std::string(40, 'c');
    for (const std::string url : {"FOXSDR://beta?t=" + tok, "foxsdr://beta?t=" + tok + "&x=1",
                                  "foxsdr:" + tok}) {
        const fs::path dir = uniqueDir("malformed");
        std::error_code ec;
        fs::create_directories(dir, ec);
        const fs::path foxsdrDir = dir / "foxsdr";
        const fs::path cfgPath = foxsdrDir / "config.json";
        ::SetEnvironmentVariableA("APPDATA", dir.string().c_str());
        ::SetEnvironmentVariableA("LOCALAPPDATA", dir.string().c_str());
        ::SetEnvironmentVariableA("CASCADE_CONFIG_TEST", cfgPath.string().c_str());
        ::SetEnvironmentVariableA("FOXSDR_INSTANCE_SCOPE", spawnInstanceScope().c_str());
        for (const char* v : {"FOXSDR_BETA_API_URL", "FOXSDR_TESTER_USAGE_URL", "FOXSDR_TELEMETRY_URL",
                              "FOXSDR_UPDATE_URL", "FOXSDR_CRASH_URL", "FOXSDR_FEATURE_URL",
                              "FOXSDR_PROBLEM_URL", "FOXSDR_REPORTS_URL"}) {
            ::SetEnvironmentVariableA(v, "http://127.0.0.1:9");
        }

        const std::string exe = std::string(CASCADE_APP_BINDIR) + "/cascade.exe";
        const std::string cmd = "\"\"" + exe + "\" \"" + url + "\" --frames 20 2>&1\"";
        std::string out;
        FILE* p = _popen(cmd.c_str(), "r");
        CHECK(p != nullptr);
        char buf[512];
        while (p != nullptr && std::fgets(buf, sizeof(buf), p) != nullptr) { out += buf; }
        const int exitCode = (p != nullptr) ? _pclose(p) : -1;

        for (const char* v : {"APPDATA", "LOCALAPPDATA", "CASCADE_CONFIG_TEST", "FOXSDR_BETA_API_URL",
                              "FOXSDR_TESTER_USAGE_URL", "FOXSDR_TELEMETRY_URL", "FOXSDR_UPDATE_URL",
                              "FOXSDR_CRASH_URL", "FOXSDR_FEATURE_URL", "FOXSDR_PROBLEM_URL",
                              "FOXSDR_REPORTS_URL", "FOXSDR_INSTANCE_SCOPE"}) {
            ::SetEnvironmentVariableA(v, nullptr);
        }

        std::printf("malformed link, exit=%d\n", exitCode);
        CHECK(exitCode == 0);
        CHECK(out.find("rendered 20 frames") != std::string::npos);
        CHECK(out.find("unknown argument") == std::string::npos);
        CHECK(out.find(tok) == std::string::npos);
        CHECK(!fs::exists(cascade::core::linkRequestPath(foxsdrDir.string())));

        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }
}

// THE CLICK A TESTER ACTUALLY MAKES, with FoxSDR already open. The tests above
// cannot tell an accepted link from a dropped one - both end with no link file,
// one because the new instance claimed it, the other because none was written -
// which is how a Windows-only shape went unnoticed from 0.99.45 to 0.99.51.
// Holding the single-instance mutex here makes the spawned copy a SECOND
// instance: an accepted link must then write the file for the running one and
// return at once, leaving the file behind with the token in it, and a refused
// shape must write nothing and carry on as a normal launch.
void testLinkShapesWindowsDeliversReachTheRunningInstance() {
    // Opened and CLOSED here, not claimPrimaryInstance()'s deliberate leak:
    // the spawn tests after this one need to be the primary instance again.
    // The SCOPED name each spawn below looks for (spawnInstanceScope()), spelled
    // out here rather than asked of primaryInstanceIdentity(), so a change to
    // the app's derivation shows up as this fake instance not being found.
    const std::string runningName = "Local\\FoxSDR-instance-" + spawnInstanceScope();
    const HANDLE running = ::CreateMutexA(nullptr, FALSE, runningName.c_str());
    CHECK(running != nullptr);
    CHECK(::GetLastError() != ERROR_ALREADY_EXISTS);  // ours alone, nobody else's

    const std::string tok = std::string(40, 'd');
    struct Shape {
        std::string url;
        bool accepted;
    };
    const Shape shapes[] = {
        {"foxsdr://beta?t=" + tok, true},    // what the portal navigates to
        {"foxsdr://beta/?t=" + tok, true},   // what Windows hands the handler
        {"foxsdr://beta//?t=" + tok, false}, // any other path is still refused
        {"foxsdr://beta/x?t=" + tok, false},
    };
    for (const Shape& shape : shapes) {
        const fs::path dir = uniqueDir("running");
        std::error_code ec;
        fs::create_directories(dir, ec);
        const fs::path foxsdrDir = dir / "foxsdr";
        const fs::path cfgPath = foxsdrDir / "config.json";
        ::SetEnvironmentVariableA("APPDATA", dir.string().c_str());
        ::SetEnvironmentVariableA("LOCALAPPDATA", dir.string().c_str());
        ::SetEnvironmentVariableA("CASCADE_CONFIG_TEST", cfgPath.string().c_str());
        ::SetEnvironmentVariableA("FOXSDR_INSTANCE_SCOPE", spawnInstanceScope().c_str());
        for (const char* v : {"FOXSDR_BETA_API_URL", "FOXSDR_TESTER_USAGE_URL", "FOXSDR_TELEMETRY_URL",
                              "FOXSDR_UPDATE_URL", "FOXSDR_CRASH_URL", "FOXSDR_FEATURE_URL",
                              "FOXSDR_PROBLEM_URL", "FOXSDR_REPORTS_URL"}) {
            ::SetEnvironmentVariableA(v, "http://127.0.0.1:9");
        }

        const std::string exe = std::string(CASCADE_APP_BINDIR) + "/cascade.exe";
        const std::string cmd = "\"\"" + exe + "\" \"" + shape.url + "\" --frames 20 2>&1\"";
        std::string out;
        FILE* p = _popen(cmd.c_str(), "r");
        CHECK(p != nullptr);
        char buf[512];
        while (p != nullptr && std::fgets(buf, sizeof(buf), p) != nullptr) { out += buf; }
        const int exitCode = (p != nullptr) ? _pclose(p) : -1;

        for (const char* v : {"APPDATA", "LOCALAPPDATA", "CASCADE_CONFIG_TEST", "FOXSDR_BETA_API_URL",
                              "FOXSDR_TESTER_USAGE_URL", "FOXSDR_TELEMETRY_URL", "FOXSDR_UPDATE_URL",
                              "FOXSDR_CRASH_URL", "FOXSDR_FEATURE_URL", "FOXSDR_PROBLEM_URL",
                              "FOXSDR_REPORTS_URL", "FOXSDR_INSTANCE_SCOPE"}) {
            ::SetEnvironmentVariableA(v, nullptr);
        }

        std::printf("link shape %s with an instance running, exit=%d\n",
                    shape.accepted ? "accepted" : "refused", exitCode);
        CHECK(exitCode == 0);
        CHECK(out.find("unknown argument") == std::string::npos);
        CHECK(out.find(tok) == std::string::npos);
        const fs::path linkFile(cascade::core::linkRequestPath(foxsdrDir.string()));
        if (shape.accepted) {
            // Handed over and gone: no window, no frames of its own.
            CHECK(out.find("rendered 20 frames") == std::string::npos);
            CHECK(fs::exists(linkFile));
            std::ifstream f(linkFile, std::ios::binary);
            const std::string written((std::istreambuf_iterator<char>(f)),
                                      std::istreambuf_iterator<char>());
            CHECK(written == tok);
        } else {
            CHECK(out.find("rendered 20 frames") != std::string::npos);
            CHECK(!fs::exists(linkFile));
        }

        if (g_checksFailed == 0) { fs::remove_all(dir, ec); }
    }
    if (running != nullptr) { ::CloseHandle(running); }
}

#endif  // _WIN32

// ---------------------------------------------------------------------------
// The real transport against a local server - confirm-by-name and migration
// exchange, both response shapes plus the invalid/network-error split.
// ---------------------------------------------------------------------------

#if defined(_WIN32)

// A local HTTP stub that answers ANY request with a configured status/body,
// and records the last request's method, path and Authorization header - the
// same raw-socket shape as test_tester_usage.cpp's own StubServer,
// independent of it (and of test_crash_upload.cpp's) so a change to one
// transport's fixture cannot silently break another's coverage.
class StubServer {
public:
    bool start(int status, const std::string& body) {
        status_ = status;
        body_ = body;
        WSADATA wsa{};
        ::WSAStartup(MAKEWORD(2, 2), &wsa);
        listen_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_ == INVALID_SOCKET) { return false; }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            return false;
        }
        int len = sizeof(addr);
        ::getsockname(listen_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ::ntohs(addr.sin_port);
        if (::listen(listen_, 8) != 0) { return false; }
        run_ = true;
        thread_ = std::thread([this] { loop(); });
        return true;
    }
    // Nothing listens at all - as close to "the server is down" as a
    // deterministic test gets.
    bool startRefused() {
        WSADATA wsa{};
        ::WSAStartup(MAKEWORD(2, 2), &wsa);
        SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (s == INVALID_SOCKET || ::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            return false;
        }
        int len = sizeof(addr);
        ::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ::ntohs(addr.sin_port);
        ::closesocket(s);
        return true;
    }

    void stop() {
        run_ = false;
        if (listen_ != INVALID_SOCKET) {
            ::closesocket(listen_);
            listen_ = INVALID_SOCKET;
        }
        if (thread_.joinable()) { thread_.join(); }
    }
    ~StubServer() { stop(); }

    std::string baseUrl() const { return "http://127.0.0.1:" + std::to_string(port_); }
    std::string lastAuth() {
        std::lock_guard<std::mutex> lk(mu_);
        return lastAuth_;
    }
    std::string lastMethod() {
        std::lock_guard<std::mutex> lk(mu_);
        return lastMethod_;
    }

private:
    void loop() {
        while (run_) {
            fd_set rd;
            FD_ZERO(&rd);
            if (listen_ == INVALID_SOCKET) { break; }
            FD_SET(listen_, &rd);
            timeval tv{0, 100 * 1000};
            const int n = ::select(0, &rd, nullptr, nullptr, &tv);
            if (n <= 0) { continue; }
            SOCKET c = ::accept(listen_, nullptr, nullptr);
            if (c == INVALID_SOCKET) { continue; }
            std::string req;
            char buf[4096];
            std::size_t contentLength = 0;
            std::size_t headerEnd = std::string::npos;
            while (run_) {
                const int got = ::recv(c, buf, sizeof(buf), 0);
                if (got <= 0) { break; }
                req.append(buf, buf + got);
                if (headerEnd == std::string::npos) {
                    headerEnd = req.find("\r\n\r\n");
                    if (headerEnd != std::string::npos) {
                        const std::size_t at = lowerFind(req, "content-length:");
                        if (at != std::string::npos) {
                            contentLength = static_cast<std::size_t>(
                                std::strtoull(req.c_str() + at + 15, nullptr, 10));
                        }
                    }
                }
                if (headerEnd != std::string::npos &&
                    req.size() >= headerEnd + 4 + contentLength) {
                    break;
                }
            }
            {
                std::lock_guard<std::mutex> lk(mu_);
                const std::size_t sp = req.find(' ');
                lastMethod_ = (sp == std::string::npos) ? std::string() : req.substr(0, sp);
                const std::size_t at = lowerFind(req, "authorization:");
                lastAuth_.clear();
                if (at != std::string::npos) {
                    const std::size_t lineEnd = req.find("\r\n", at);
                    std::size_t start = at + 14;
                    while (start < req.size() && req[start] == ' ') { ++start; }
                    lastAuth_ = req.substr(start, (lineEnd == std::string::npos ? req.size() : lineEnd) - start);
                }
            }
            const std::string status = std::to_string(status_);
            const std::string statusLine = "HTTP/1.1 " + status + " x\r\nContent-Length: " +
                                           std::to_string(body_.size()) + "\r\n\r\n" + body_;
            ::send(c, statusLine.c_str(), static_cast<int>(statusLine.size()), 0);
            ::shutdown(c, SD_BOTH);
            ::closesocket(c);
        }
    }
    static std::size_t lowerFind(const std::string& hay, const std::string& needle) {
        std::string l;
        l.reserve(hay.size());
        for (char ch : hay) {
            l.push_back((ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch);
        }
        return l.find(needle);
    }

    int status_ = 200;
    std::string body_;
    SOCKET listen_ = INVALID_SOCKET;
    int port_ = 0;
    std::atomic<bool> run_{false};
    std::thread thread_;
    std::mutex mu_;
    std::string lastAuth_;
    std::string lastMethod_;
};

#else  // !_WIN32

// The POSIX mirror: a local httplib::Server serving plain http, which
// authRequestBounded's loopback exception allows without a certificate.
struct StubServer {
    httplib::Server srv;
    std::thread th;
    int port = 0;
    int status = 200;
    std::string body;
    std::mutex mu;
    std::string lastAuth_;
    std::string lastMethod_;

    bool start(int st, const std::string& b) {
        status = st;
        body = b;
        auto handler = [this](const httplib::Request& req, httplib::Response& res) {
            std::lock_guard<std::mutex> lk(mu);
            lastMethod_ = req.method;
            lastAuth_ = req.get_header_value("Authorization");
            res.status = status;
            res.set_content(body, "application/json");
        };
        srv.Get(".*", handler);
        srv.Post(".*", handler);
        port = srv.bind_to_any_port("127.0.0.1");
        th = std::thread([this] { srv.listen_after_bind(); });
        srv.wait_until_ready();
        return true;
    }
    bool startRefused() {
        httplib::Server probe;
        port = probe.bind_to_any_port("127.0.0.1");
        probe.stop();
        return true;
    }
    ~StubServer() {
        srv.stop();
        if (th.joinable()) { th.join(); }
    }
    std::string baseUrl() const { return "http://127.0.0.1:" + std::to_string(port); }
    std::string lastAuth() {
        std::lock_guard<std::mutex> lk(mu);
        return lastAuth_;
    }
    std::string lastMethod() {
        std::lock_guard<std::mutex> lk(mu);
        return lastMethod_;
    }
};

#endif  // _WIN32

void testResolveAppTokenNameOk() {
    StubServer srv;
    CHECK(srv.start(200, "{\"name\":\"Ada\"}"));
    auto cancel = std::make_shared<UploadCancel>();
    const auto r = cascade::core::resolveAppTokenName(srv.baseUrl(), std::string(40, 'a'), cancel);
    CHECK(r.outcome == BetaLinkOutcome::Ok);
    CHECK(r.name == "Ada");
    CHECK(srv.lastMethod() == "GET");
    CHECK(srv.lastAuth() == "Bearer " + std::string(40, 'a'));
}

void testResolveAppTokenNameInvalidOn404() {
    StubServer srv;
    CHECK(srv.start(404, ""));
    auto cancel = std::make_shared<UploadCancel>();
    const auto r = cascade::core::resolveAppTokenName(srv.baseUrl(), std::string(40, 'a'), cancel);
    CHECK(r.outcome == BetaLinkOutcome::Invalid);
    CHECK(r.name.empty());
}

void testResolveAppTokenNameNetworkErrorOnRefusal() {
    StubServer srv;
    CHECK(srv.startRefused());
    auto cancel = std::make_shared<UploadCancel>();
    const auto r = cascade::core::resolveAppTokenName(srv.baseUrl(), std::string(40, 'a'), cancel);
    CHECK(r.outcome == BetaLinkOutcome::NetworkError);
}

void testResolveAppTokenNameNetworkErrorOnMalformedBody() {
    StubServer srv;
    CHECK(srv.start(200, "not json"));
    auto cancel = std::make_shared<UploadCancel>();
    const auto r = cascade::core::resolveAppTokenName(srv.baseUrl(), std::string(40, 'a'), cancel);
    CHECK(r.outcome == BetaLinkOutcome::NetworkError);
}

void testExchangePortalTokenOk() {
    StubServer srv;
    const std::string appTok = std::string(40, 'f');
    CHECK(srv.start(200, "{\"appToken\":\"" + appTok + "\",\"name\":\"Grace\"}"));
    auto cancel = std::make_shared<UploadCancel>();
    const auto r = cascade::core::exchangePortalToken(srv.baseUrl(), std::string(32, 'p'), cancel);
    CHECK(r.outcome == BetaMigrationOutcome::Ok);
    CHECK(r.appToken == appTok);
    CHECK(r.name == "Grace");
    CHECK(srv.lastMethod() == "POST");
    CHECK(srv.lastAuth() == "Bearer " + std::string(32, 'p'));
}

void testExchangePortalTokenInvalidOn404And401() {
    {
        StubServer srv;
        CHECK(srv.start(404, ""));
        auto cancel = std::make_shared<UploadCancel>();
        const auto r =
            cascade::core::exchangePortalToken(srv.baseUrl(), std::string(32, 'p'), cancel);
        CHECK(r.outcome == BetaMigrationOutcome::Invalid);
    }
    {
        StubServer srv;
        CHECK(srv.start(401, ""));
        auto cancel = std::make_shared<UploadCancel>();
        const auto r =
            cascade::core::exchangePortalToken(srv.baseUrl(), std::string(32, 'p'), cancel);
        CHECK(r.outcome == BetaMigrationOutcome::Invalid);
    }
}

void testExchangePortalTokenNetworkErrorOnRefusal() {
    StubServer srv;
    CHECK(srv.startRefused());
    auto cancel = std::make_shared<UploadCancel>();
    const auto r = cascade::core::exchangePortalToken(srv.baseUrl(), std::string(32, 'p'), cancel);
    CHECK(r.outcome == BetaMigrationOutcome::NetworkError);
}

void testExchangePortalTokenRejectsAMintedTokenOfTheWrongShape() {
    // A site bug (or a stub server for this exact test) returning a 32-hex
    // value in "appToken" must not be trusted as an app token - the two
    // shapes are how this codebase tells them apart at all.
    StubServer srv;
    CHECK(srv.start(200, "{\"appToken\":\"" + std::string(32, 'z') + "\",\"name\":\"X\"}"));
    auto cancel = std::make_shared<UploadCancel>();
    const auto r = cascade::core::exchangePortalToken(srv.baseUrl(), std::string(32, 'p'), cancel);
    CHECK(r.outcome == BetaMigrationOutcome::NetworkError);
}

// ---------------------------------------------------------------------------
// The fire-and-forget senders
// ---------------------------------------------------------------------------

void testBetaLinkNameSenderRunsAndReports() {
    StubServer srv;
    CHECK(srv.start(200, "{\"name\":\"Marie\"}"));
    BetaLinkNameSender s;
    s.send(srv.baseUrl(), std::string(40, 'a'));
    std::optional<cascade::core::BetaLinkResolved> got;
    for (int i = 0; i < 1000 && !got.has_value(); ++i) {
        got = s.takeResult();
        if (!got.has_value()) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    }
    CHECK(got.has_value());
    if (got) {
        CHECK(got->outcome == BetaLinkOutcome::Ok);
        CHECK(got->name == "Marie");
    }
}

void testBetaMigrationSenderRunsAndReports() {
    StubServer srv;
    const std::string appTok = std::string(40, 'b');
    CHECK(srv.start(200, "{\"appToken\":\"" + appTok + "\",\"name\":\"Rosalind\"}"));
    BetaMigrationSender s;
    s.send(srv.baseUrl(), std::string(32, 'p'));
    std::optional<cascade::core::BetaMigrationResult> got;
    for (int i = 0; i < 1000 && !got.has_value(); ++i) {
        got = s.takeResult();
        if (!got.has_value()) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    }
    CHECK(got.has_value());
    if (got) {
        CHECK(got->outcome == BetaMigrationOutcome::Ok);
        CHECK(got->appToken == appTok);
    }
}

void testSendersRefuseASecondCallWhileBusy() {
    // A hanging server would be needed to observe busy() mid-flight reliably;
    // the cheaper, deterministic half of this property is that a sender with
    // no url/token at all never starts a thread, which busy() reflects
    // immediately.
    BetaLinkNameSender s;
    CHECK(!s.busy());
    s.send("", "");
    CHECK(!s.busy());
}

// ---------------------------------------------------------------------------
// Nothing here logs the token - same discipline as test_tester_usage.cpp's
// own scan, applied to the two new source files this feature adds.
// ---------------------------------------------------------------------------

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) { return {}; }
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

fs::path findRepoRoot(const std::string& givenRoot) {
    if (!givenRoot.empty()) { return fs::path(givenRoot); }
    fs::path dir = fs::current_path();
    std::error_code ec;
    for (int level = 0; !ec && level < 10; ++level) {
        if (fs::is_regular_file(dir / "PRIVACY.md", ec) && fs::is_directory(dir / "src", ec)) {
            return dir;
        }
        if (!dir.has_parent_path() || dir.parent_path() == dir) { break; }
        dir = dir.parent_path();
    }
    return {};
}

void testTokenNeverAppearsNearALogCall(const std::string& givenRoot) {
    const fs::path repoRoot = findRepoRoot(givenRoot);
    CHECK(!repoRoot.empty());
    if (repoRoot.empty()) { return; }

    // core/tester_link.cpp: no log-emitting call at all, same rule as
    // tester_usage.cpp.
    const std::string linkSrc = readFile(repoRoot / "src" / "core" / "tester_link.cpp");
    CHECK(!linkSrc.empty());
    CHECK(linkSrc.find("diagLogf") == std::string::npos);
    CHECK(linkSrc.find("diagWarnf") == std::string::npos);
    CHECK(linkSrc.find("std::printf") == std::string::npos);
    CHECK(linkSrc.find("fprintf") == std::string::npos);

    // main.cpp: the one diagLogf call this feature adds logs only a length,
    // never the token/URL variable - scan every logging line for the names
    // that WOULD leak it.
    const std::string mainSrc = readFile(repoRoot / "src" / "main.cpp");
    CHECK(!mainSrc.empty());
    std::istringstream lines(mainSrc);
    std::string line;
    bool clean = true;
    int lineNo = 0;
    while (std::getline(lines, line)) {
        ++lineNo;
        const bool logs = line.find("diagLogf(") != std::string::npos ||
                          line.find("diagWarnf(") != std::string::npos;
        if (!logs) { continue; }
        // linkToken.size() is fine (that is exactly what the one permitted
        // log line reports) - what must never appear is the variable used
        // any other way on a logging line.
        const bool mentionsToken = line.find("linkToken") != std::string::npos;
        const bool onlyItsSize = line.find("linkToken.size()") != std::string::npos &&
                                 line.find("linkToken") == line.rfind("linkToken");
        const bool leaks = (mentionsToken && !onlyItsSize) ||
                           line.find("argv[1]") != std::string::npos;
        if (leaks) {
            clean = false;
            std::printf("FAIL main.cpp:%d logs the tester link token: %s\n", lineNo, line.c_str());
        }
    }
    CHECK(clean);

    // gui/app_window.cpp already has a line-scan for this in
    // test_tester_usage.cpp; extend the same property here to the new field
    // names this feature adds, independent of that file's own list.
    const std::string appSrc = readFile(repoRoot / "src" / "gui" / "app_window.cpp");
    CHECK(!appSrc.empty());
    std::istringstream appLines(appSrc);
    bool appClean = true;
    int appLineNo = 0;
    while (std::getline(appLines, line)) {
        ++appLineNo;
        const bool logs = line.find("diagLogf(") != std::string::npos ||
                          line.find("diagWarnf(") != std::string::npos;
        if (!logs) { continue; }
        const bool leaks = line.find("testerAppToken_") != std::string::npos ||
                           line.find("testerLinkPending_") != std::string::npos ||
                           line.find("testerLinkResolvingToken_") != std::string::npos;
        if (leaks) {
            appClean = false;
            std::printf("FAIL app_window.cpp:%d logs the app token: %s\n", appLineNo, line.c_str());
        }
    }
    CHECK(appClean);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string givenRoot = argc > 1 ? argv[1] : std::string();

    testParseBetaLinkUrlExactShapeOnly();
    testSanitizeTesterNameStripsControlCharactersAndNewlines();
    testSanitizeTesterNameCapsLengthWithoutSplittingUtf8();
    testSanitizeTesterNameEmptyOrAllControlIsEmpty();
    testValidAppTokenIsFortyHexNeverThirtyTwo();
    testExtractAppTokenTrimsAndValidatesOnly();
    testRewriteTokenRelabelsEveryItem();
    testRewriteTokenLeavesUnparsableItemsAlone();
    testWriteThenClaimRoundTrips();
    testWriteRefusesAnInvalidToken();
    testClaimOfAStaleFileDeletesItUnread();
    testClaimWithNothingThereIsEmpty();
    testClaimOfGarbageContentIsEmptyButStillConsumesTheFile();
    testClaimPrimaryInstanceFirstWinsSecondDoesNot();
    testClaimPrimaryInstanceDifferentIdentitiesAreIndependent();
    testPrimaryInstanceIdentityIsTheFixedNameWithoutTheTestHook();
    testPrimaryInstanceIdentityTakesAValidScopeOnlyWithTheTestHook();
    testClaimPrimaryInstanceHoldsTheScopedIdentity();
    testBetaApiBaseUrlOverride();
#if defined(_WIN32)
    testLinkActivationWithNoInstanceRunningLaunchesNormallyAndNeverLeaksTheToken();
    testMalformedLinkIsDroppedWithoutEchoingTheToken();
    testLinkShapesWindowsDeliversReachTheRunningInstance();
#endif
    testResolveAppTokenNameOk();
    testResolveAppTokenNameInvalidOn404();
    testResolveAppTokenNameNetworkErrorOnRefusal();
    testResolveAppTokenNameNetworkErrorOnMalformedBody();
    testExchangePortalTokenOk();
    testExchangePortalTokenInvalidOn404And401();
    testExchangePortalTokenNetworkErrorOnRefusal();
    testExchangePortalTokenRejectsAMintedTokenOfTheWrongShape();
    testBetaLinkNameSenderRunsAndReports();
    testBetaMigrationSenderRunsAndReports();
    testSendersRefuseASecondCallWhileBusy();
    testTokenNeverAppearsNearALogCall(givenRoot);

    return testSummary("test_tester_link");
}
