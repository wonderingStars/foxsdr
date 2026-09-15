// Tests for core/telemetry.hpp.
//
// The point of most of these is NEGATIVE: proving that things which must
// never be transmitted are not in the payload. A privacy promise in a README
// is worth exactly as much as the test that holds the payload to it, so the
// serial-number stripping and the field inventory are asserted explicitly
// rather than left to inspection.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/telemetry.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>

// Must match every other TU in the program that includes httplib.h on
// non-Windows (plugin_repo.cpp, crash_upload.cpp, telemetry.cpp,
// web_server.cpp), or a client/server layout mismatch reproduces a SEGV
// inside ClientImpl::create_client_socket - see the comment in
// web_server.cpp for the ASan trace this was found from.
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif
#include <httplib.h>
#endif

using namespace cascade::core;

namespace {

// This machine's host name, so the OS-description test can assert that the
// name is absent rather than assuming the implementation left it out. uname()
// carries it in an adjacent field, which is exactly the kind of thing that
// gets picked up by a later edit without anyone noticing.
std::string hostNameForTest() {
    char buf[256] = {0};
#if defined(_WIN32)
    DWORD n = static_cast<DWORD>(sizeof(buf));
    if (::GetComputerNameA(buf, &n) == 0) { return std::string(); }
#else
    if (::gethostname(buf, sizeof(buf) - 1) != 0) { return std::string(); }
#endif
    return std::string(buf);
}

void testDeviceSerialIsStripped() {
    // THE ONE THAT MATTERS. This is the real argument string for the B200 on
    // the development machine, and it contains the serial twice - once on its
    // own and once embedded in the label.
    const std::string real =
        "driver=uhd, label=B200 EDR04ZDB2, name=, product=B200, "
        "serial=EDR04ZDB2, type=b200";
    const std::string got = sanitiseDevice(real);
    CHECK(got.find("EDR04ZDB2") == std::string::npos);
    CHECK(got.find("edr04zdb2") == std::string::npos);
    // ...while still saying which radio it is, which is the whole point.
    CHECK(got.find("uhd") != std::string::npos);
    CHECK(got.find("b200") != std::string::npos);

    // A driver inventing its own keys must not leak them: the allow list is
    // positive, so anything unrecognised is dropped rather than passed on.
    const std::string nosy =
        "driver=rtlsdr, serial=00000001, addr=192.168.1.40, "
        "uri=usb://1-2, hostname=steve-pc, product=RTL2838";
    const std::string s2 = sanitiseDevice(nosy);
    CHECK(s2.find("192.168.1.40") == std::string::npos);
    CHECK(s2.find("steve-pc") == std::string::npos);
    CHECK(s2.find("usb://") == std::string::npos);
    CHECK(s2.find("00000001") == std::string::npos);
    CHECK(s2.find("rtlsdr") != std::string::npos);

    CHECK(sanitiseDevice("").empty());
    CHECK(sanitiseDevice("garbage with no equals").empty());
}

void testInstallIdIsRandomAndValidated() {
    const std::string a = newInstallId();
    const std::string b = newInstallId();
    CHECK(validInstallId(a));
    CHECK(validInstallId(b));
    // Two ids in a row must differ; a constant would make every install look
    // like one user, and a counter would be guessable.
    CHECK(a != b);
    CHECK(a.size() == 32);

    CHECK(!validInstallId(""));
    CHECK(!validInstallId("short"));
    CHECK(!validInstallId(std::string(32, 'z')));       // not hex
    CHECK(!validInstallId(std::string(33, 'a')));       // too long
    CHECK(!validInstallId("steve@example.com"));        // hand-edited config
    CHECK(!validInstallId("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"));  // uppercase
}

void testPayloadContainsOnlyTheAgreedFields() {
    TelemetryReport r;
    r.installId = newInstallId();
    r.appVersion = "0.48.0";
    r.os = osDescription();
    r.arch = archDescription();
    r.launches = 12;
    r.crashes = 1;
    r.session.seconds = 3600;
    r.session.modeSeconds["WFM"] = 3000;
    r.session.modeSeconds["RAW"] = 600;
    r.session.panels = {"map", "decoded"};
    r.session.plugins = {"ADS-B 1.0.0", "Aircraft Info 1.0.0"};
    r.session.sdrModel = sanitiseDevice("driver=uhd, product=B200, serial=EDR04ZDB2");

    const nlohmann::json j = nlohmann::json::parse(r.toJson());

    // THE FIELD INVENTORY. If someone adds a field to the payload, this fails
    // and they have to come and change the privacy notice too - which is
    // exactly the conversation that should happen.
    const std::set<std::string> allowed = {
        "id", "v", "os", "arch", "launches", "crashes",
        "sessionSec", "sdr", "modes", "panels", "plugins"};
    std::set<std::string> actual;
    for (auto it = j.begin(); it != j.end(); ++it) { actual.insert(it.key()); }
    CHECK(actual == allowed);

    CHECK(j["v"] == "0.48.0");
    CHECK(j["launches"] == 12);
    CHECK(j["crashes"] == 1);
    CHECK(j["modes"]["WFM"] == 3000);
    CHECK(j["panels"].size() == 2);

    // And the serial has not crept back in through the whole-document route.
    CHECK(r.toJson().find("EDR04ZDB2") == std::string::npos);
}

void testPluginNamesCannotBreakTheReport() {
    // Plugin names are third-party text. Invalid UTF-8 in one must not make
    // the report unserialisable - the same failure that took the browser
    // interface down when it was left to throw.
    TelemetryReport r;
    r.installId = newInstallId();
    r.appVersion = "0.48.0";
    r.session.plugins = {std::string("bad\xFF\xFE name 1.0.0")};
    const std::string out = r.toJson();
    CHECK(!out.empty());
    // Parses cleanly despite the rubbish going in.
    const nlohmann::json j = nlohmann::json::parse(out);
    CHECK(j["plugins"].size() == 1);
}

void testModeSecondsAccrueAcrossFrames() {
    // THE ONE THAT MATTERS FOR MODE SECONDS. Accrual is sampled once per
    // rendered frame, so every delta is a fraction of a second. Truncating
    // each delta on its own banks nothing at all, which is how "modes used"
    // arrived at the endpoint empty for a whole release while the session
    // seconds beside it were correct.
    SecondAccrual a;
    a.reset(1000.0);
    std::uint64_t banked = 0;
    for (int k = 1; k <= 60; ++k) { banked += a.advance(1000.0 + 0.0167 * k); }
    CHECK(banked == 1);  // 60 frames at 16.7 ms is one second of wall clock
    for (int k = 61; k <= 120; ++k) { banked += a.advance(1000.0 + 0.0167 * k); }
    CHECK(banked == 2);

    // A long step banks its whole seconds at once and carries the rest, so
    // the second call sees 0.75 s of credit it did not have to earn again.
    SecondAccrual b;
    b.reset(0.0);
    CHECK(b.advance(2.75) == 2);
    CHECK(b.advance(3.30) == 1);

    // Nothing is banked before a reset, and a clock that steps backwards
    // re-marks rather than producing a huge count from a negative delta.
    SecondAccrual c;
    CHECK(c.advance(5.0) == 0);
    SecondAccrual d;
    d.reset(10.0);
    CHECK(d.advance(4.0) == 0);
    CHECK(d.advance(5.5) == 1);
}

void testPanelsReachThePayload() {
    // "Panels opened" was empty in every report ever sent, because nothing
    // called the recorder. Whether the GUI calls it cannot be asserted from
    // here; what can is that a noted panel survives the trip into the
    // payload, in order and without being folded away.
    TelemetryReport r;
    r.installId = newInstallId();
    r.appVersion = "0.48.0";
    r.session.panels = {"map", "decoded", "scanner"};
    const nlohmann::json j = nlohmann::json::parse(r.toJson());
    const std::vector<std::string> got = j["panels"].get<std::vector<std::string>>();
    CHECK(got == std::vector<std::string>({"map", "decoded", "scanner"}));
}

void testOsAndArchSayNothingIdentifying() {
    const std::string os = osDescription();
    CHECK(!os.empty());
    // A version string, not a machine name, a path or a user name. The prefix
    // is the kernel/OS family the build targets; what the property really
    // asserts is on the three lines below, which hold on every platform.
#if defined(_WIN32)
    CHECK(os.rfind("Windows", 0) == 0 || os == "unknown");
#else
    // uname's sysname: "Linux", "Darwin", "FreeBSD"...
    CHECK(!os.empty() && (std::isalpha(static_cast<unsigned char>(os[0])) != 0));
#endif
    CHECK(os.find('@') == std::string::npos);
    // The host name is the identifying thing uname could leak (nodename), so
    // assert it is absent by name rather than trusting the field selection.
    CHECK(os.find(hostNameForTest()) == std::string::npos || hostNameForTest().empty());
    CHECK(os.size() < 40);
    const std::string arch = archDescription();
    CHECK(arch == "x64" || arch == "arm64" || arch == "x86" || arch == "unknown");
}

void testBeatPayloadContainsOnlyTheAgreedFields() {
    // THE FIELD INVENTORY FOR BEATS. A heartbeat exists to say "running right
    // now" and nothing else - if a session field ever creeps in, this fails
    // and the privacy notice has to change in the same edit.
    const std::string id = newInstallId();
    const std::string out = HeartbeatSender::beatJson(id, "0.65.0");
    const nlohmann::json j = nlohmann::json::parse(out);

    const std::set<std::string> allowed = {"id", "v", "beat"};
    std::set<std::string> actual;
    for (auto it = j.begin(); it != j.end(); ++it) { actual.insert(it.key()); }
    CHECK(actual == allowed);

    CHECK(j["id"] == id);
    CHECK(j["v"] == "0.65.0");
    CHECK(j["beat"] == 1);
}

void testBeatScheduleFiresImmediatelyThenAtInterval() {
    // The url is http:// on purpose: postJson refuses non-https before any
    // network I/O, so the schedule can be driven for real - threads and all -
    // without a request ever leaving the machine.
    HeartbeatSender h;
    h.configure("http://beat.invalid/", newInstallId(), "0.65.0", 300);

    // First beat is due immediately, so a session shorter than the interval
    // still counts as running.
    CHECK(h.due(1000.0));
    h.poll(1000.0);
    // ...and not again until the interval has passed.
    CHECK(!h.due(1000.5));
    CHECK(!h.due(1299.0));
    CHECK(h.due(1300.0));
    h.poll(1300.0);
    CHECK(!h.due(1301.0));

    // Re-arming resets the schedule: the first beat after configure() is
    // immediate again.
    h.configure("http://beat.invalid/", newInstallId(), "0.65.0", 300);
    CHECK(h.due(1301.0));
}

void testBeatRefusesToArmWithoutARealId() {
    // Opt-out clears the install id; configure() refusing anything that is
    // not exactly newInstallId()'s shape is what makes "off means off" hold
    // for beats. An unconfigured sender is never due and poll() is a no-op.
    HeartbeatSender h;
    h.configure("http://beat.invalid/", "", "0.65.0", 300);
    CHECK(!h.due(0.0));
    h.poll(0.0);
    CHECK(!h.due(1e9));

    HeartbeatSender h2;
    h2.configure("http://beat.invalid/", "not-an-id", "0.65.0", 300);
    CHECK(!h2.due(0.0));

    // And no url means nowhere to send, which also refuses to arm.
    HeartbeatSender h3;
    h3.configure("", newInstallId(), "0.65.0", 300);
    CHECK(!h3.due(0.0));
}

// The in-app privacy link must name the page the site serves and the store
// listing links to, over https, on the project's own domain. A relative path,
// a plain-http address or a workers.dev host would each pass a reviewer's eye
// and fail the moment the page moved or the transport was tampered with.
void testPrivacyPolicyUrlIsTheSitePageOverHttps() {
    const std::string url = cascade::core::kPrivacyPolicyUrl;
    CHECK(url == "https://foxsdr.com/privacy.html");
    CHECK(url.rfind("https://foxsdr.com/", 0) == 0);
    CHECK(url.find("workers.dev") == std::string::npos);
}

#if !defined(_WIN32)
namespace fs = std::filesystem;

// --- The POSIX transport: telemetryEndpoint() actually reads the seam ------
//
// Before this change the POSIX branch of telemetryEndpoint() did not exist:
// the override block was compiled only under _WIN32, so FOXSDR_TELEMETRY_URL
// was silently ignored here and every interactive run on Linux would have
// posted real usage to the live endpoint regardless of what the environment
// said - exactly the hazard installer/msix/README.md's black-hole recipe
// exists to prevent, and it never worked on this platform until now.
void testTelemetryEndpointOverridePosix() {
    ::unsetenv("FOXSDR_TELEMETRY_URL");
    CHECK(telemetryEndpoint() == "https://telemetry.foxsdr.com");

    ::setenv("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9", 1);
    CHECK(telemetryEndpoint() == "http://127.0.0.1:9");
    ::unsetenv("FOXSDR_TELEMETRY_URL");
    CHECK(telemetryEndpoint() == "https://telemetry.foxsdr.com");
}

// --- The documented black hole: a plain http override never opens a socket -
//
// Unlike the crash uploader, telemetry's transport has no loopback exception
// (see the comment in telemetry.cpp): ANY non-https URL is refused before a
// socket is even created. That is what makes
// FOXSDR_TELEMETRY_URL=http://127.0.0.1:9 (installer/msix/README.md) a black
// hole - not that port 9 refuses the connection, but that the scheme check
// stops it before a connection is attempted at all. Bounded well under the
// 4 s connect timeout because no I/O happens; a looser bound would not
// distinguish "refused instantly" from "connected, then failed".
void testTelemetryHttpUrlNeverConnects() {
    const auto t0 = std::chrono::steady_clock::now();
    {
        TelemetryReporter r;
        r.send("http://127.0.0.1:9/", "{\"probe\":1}");
    }  // the destructor joins the send thread
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
    std::printf("telemetry http:// black hole: %.0f ms\n", ms);
    CHECK(ms < 500.0);
}

// --- A refused https connection fails closed, promptly ---------------------
//
// The complement to the test above: an https URL DOES attempt a real
// connection (proving the scheme check above is not simply refusing
// everything), and a refused port on loopback still returns well within the
// 4 s connect timeout rather than hanging or retrying.
void testTelemetryHttpsConnectionRefusedFailsFast() {
    int port = 0;
    {
        httplib::Server probe;
        port = probe.bind_to_any_port("127.0.0.1");
        // httplib::Server's destructor is `= default` and does not close the
        // listening socket - without this the port stays open and a client
        // connecting to it hangs until the READ timeout instead of being
        // refused, which very nearly hid this exact scenario in
        // test_crash_upload.cpp's own Refuse mode. See that file's comment.
        probe.stop();
    }
    CHECK(port > 0);

    const auto t0 = std::chrono::steady_clock::now();
    {
        TelemetryReporter r;
        r.send("https://127.0.0.1:" + std::to_string(port) + "/", "{\"probe\":1}");
    }
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
    std::printf("telemetry https connection-refused: %.0f ms\n", ms);
    CHECK(ms < 4500.0);
}

// A short-lived self-signed certificate, generated with the system openssl
// binary rather than hand-rolled OpenSSL API calls - this test needs one
// disposable keypair, not a library feature, and shelling out to the same
// tool a developer would use keeps the fixture readable.
struct TempSelfSignedCert {
    fs::path dir;
    fs::path certPath;
    fs::path keyPath;
    bool ok = false;

    TempSelfSignedCert() {
        dir = fs::temp_directory_path() /
              ("cascade-telemetry-cert-" + std::to_string(static_cast<long>(::getpid())));
        std::error_code ec;
        fs::create_directories(dir, ec);
        certPath = dir / "cert.pem";
        keyPath = dir / "key.pem";
        const std::string cmd = "openssl req -x509 -newkey rsa:2048 -nodes -keyout '" +
                                keyPath.string() + "' -out '" + certPath.string() +
                                "' -days 1 -subj /CN=127.0.0.1 >/dev/null 2>&1";
        ok = (std::system(cmd.c_str()) == 0) && fs::exists(certPath) && fs::exists(keyPath);
    }
    ~TempSelfSignedCert() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

// --- Certificate verification is actually ON, not merely documented as on -
//
// The real transport (TelemetryReporter -> postJson) must refuse a server
// presenting a certificate nobody signed, exactly as plugin_repo.cpp's
// catalogue client does. Proved two ways in one fixture: the REAL code path
// against a self-signed server never lets the request through, and a
// deliberately-weakened CONTROL client (verification off, built here in the
// test rather than in product code) reaches the same server fine - which is
// what proves the first result is about the certificate and not a harness
// that cannot connect to anything.
void testCertificateVerificationIsOnPosix() {
    TempSelfSignedCert cert;
    CHECK(cert.ok);
    if (!cert.ok) {
        std::printf("skipping certificate-verification test: no local openssl to mint a "
                    "test certificate\n");
        return;
    }

    httplib::SSLServer srv(cert.certPath.string().c_str(), cert.keyPath.string().c_str());
    CHECK(srv.is_valid());
    bool sawRequest = false;
    std::string sawBody;
    srv.Post("/", [&](const httplib::Request& req, httplib::Response& res) {
        sawRequest = true;
        sawBody = req.body;
        res.status = 204;
    });
    const int port = srv.bind_to_any_port("127.0.0.1");
    CHECK(port > 0);
    std::thread th([&] { srv.listen_after_bind(); });
    srv.wait_until_ready();

    // THE REAL PATH. Verification is on in production code, so the TLS
    // handshake against this self-signed certificate must fail and the
    // server's handler must never run.
    {
        TelemetryReporter r;
        r.send("https://127.0.0.1:" + std::to_string(port) + "/", "{\"probe\":1}");
    }
    CHECK(!sawRequest);
    CHECK(sawBody.empty());

    // THE CONTROL. Same server, same port, verification explicitly disabled -
    // proving the rejection above is the certificate check doing its job,
    // not a fixture that cannot reach its own server.
    httplib::SSLClient control("127.0.0.1", port);
    control.enable_server_certificate_verification(false);
    control.set_connection_timeout(4, 0);
    const httplib::Result res = control.Post("/", "{\"probe\":1}", "application/json");
    CHECK(res.operator bool());
    if (res) { CHECK(res->status == 204); }
    CHECK(sawRequest);
    CHECK(sawBody == "{\"probe\":1}");

    srv.stop();
    th.join();
}
#endif  // !_WIN32

}  // namespace

int main() {
    testPrivacyPolicyUrlIsTheSitePageOverHttps();
    testDeviceSerialIsStripped();
    testInstallIdIsRandomAndValidated();
    testPayloadContainsOnlyTheAgreedFields();
    testPluginNamesCannotBreakTheReport();
    testModeSecondsAccrueAcrossFrames();
    testPanelsReachThePayload();
    testOsAndArchSayNothingIdentifying();
    testBeatPayloadContainsOnlyTheAgreedFields();
    testBeatScheduleFiresImmediatelyThenAtInterval();
    testBeatRefusesToArmWithoutARealId();
#if !defined(_WIN32)
    testTelemetryEndpointOverridePosix();
    testTelemetryHttpUrlNeverConnects();
    testTelemetryHttpsConnectionRefusedFailsFast();
    testCertificateVerificationIsOnPosix();
#endif
    return testSummary("test_telemetry");
}
