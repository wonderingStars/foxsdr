// Headless provisioning, the parts that need no process (engine extraction stage 7e, src/engine/provision.{hpp,cpp}).
//
// WHAT IS ASSERTED
//   R. THE PASSWORD'S READ (readPasswordFrom / readPassword): one trailing line end (LF or CR LF) is not part of the
//      password and nothing else is trimmed (a space at either end is a character); an empty file, an interior line end, a NUL
//      and bytes that are not UTF-8 are refused - each with a sentence that names the flag and the file and holds no byte of
//      the content; a file larger than a password can be is refused unread; a file that is not there is refused.
//   J. THE REQUEST'S TEXT (serverConfigText): each flag combination builds the JSON the engine's own parser accepts
//      (parseServerConfigRequest) and reads back as the flags said; a password with every character JSON has to escape - a
//      quote, a backslash, a tab, accents, an emoji - reads back as the very same bytes.
//   W. THE WIPE (Secret::wipe, which ~Secret calls): the buffer's bytes are zero afterwards, not just its length.
//   P. THE ENGINE'S ANSWER, in process (provisionServers against an Engine that hosts its servers): a loopback web server
//      with a password is up, the record the engine stored VERIFIES the password and is the engine's own PBKDF2 record; an
//      off-machine bind with no password is refused with THE BIND POLICY'S SENTENCE (taken from net::evaluateBind, not copied
//      here) and nothing listens; a password under eight characters is refused with the engine's sentence, which holds no
//      password; a port that is taken is refused with the listener's own; NO_CHANGE (the same request said twice) is not a
//      refusal.
//
// Break-its (docs/engine-stage4.md 6.19): the wipe loop removed -> W RED; the escape of a quote removed from jsonQuoteInto -> J
// RED (the engine's parser refuses the text); the policy's refusal not read back (the listener's state ignored) -> P RED (the
// wildcard bind is reported as provisioned).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <winsock2.h>

#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using SockT = SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SockT = int;
constexpr SockT INVALID_SOCKET = -1;
#endif

#include "core/config.hpp"
#include "engine/engine.hpp"
#include "engine/provision.hpp"
#include "engine/server_host.hpp"
#include "engine_api_test_env.hpp"
#include "net/web_auth.hpp"
#include "net/web_policy.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using cascade::engine::Engine;
using cascade::engine::ProvisionRequest;
using cascade::engine::Secret;

namespace {

// A temporary file with exactly `bytes` in it, read by readPassword by name.
struct TempFile {
    fs::path path;
    explicit TempFile(const std::string& bytes, const char* tag) {
        path = fs::temp_directory_path() / (std::string("foxsdr-prov-") + tag + "-" + std::to_string(std::rand()) + ".txt");
        std::ofstream out(path, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    ~TempFile() {
        std::error_code ec;
        fs::remove(path, ec);
    }
};

struct ReadOutcome {
    bool ok = false;
    std::string value;
    std::string error;
};

ReadOutcome readFrom(const std::string& bytes, const char* tag) {
    TempFile f(bytes, tag);
    ReadOutcome r;
    Secret s;
    r.ok = cascade::engine::readPassword(f.path.string(), s, r.error);
    r.value = s.value;
    return r;
}

bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

// ---------------------------------------------------------------------------------------------------------------------------------
// R
// ---------------------------------------------------------------------------------------------------------------------------------
void testRead() {
    // One trailing line end goes, whichever it is; nothing else does.
    CHECK(readFrom("correct-horse-9\n", "lf").value == "correct-horse-9");
    CHECK(readFrom("correct-horse-9\r\n", "crlf").value == "correct-horse-9");
    CHECK(readFrom("correct-horse-9", "bare").value == "correct-horse-9");
    CHECK(readFrom(" a pass phrase ", "spaces").value == " a pass phrase ");  // spaces are characters
    CHECK(readFrom("tab\tinside-9\n", "tab").value == "tab\tinside-9");
    CHECK(readFrom("na\xC3\xAFve-\xE2\x82\xAC-9\n", "utf8").value == "na\xC3\xAFve-\xE2\x82\xAC-9");
    const ReadOutcome ok = readFrom("fine-password-1\n", "ok");
    CHECK(ok.ok && ok.error.empty());

    // Refused, each in its own sentence, none of them holding what was in the file.
    const std::string secret = "Sup3r-Secret-";
    struct Case { std::string bytes; const char* mentions; };
    const Case refusals[] = {
        {"", "holds no password"},
        {"\n", "holds no password"},
        {"\r\n", "holds no password"},
        {secret + "\nsecond line\n", "on one line"},
        {secret + "\n\n", "on one line"},  // the first line end is the line's, the second makes it two lines
        {secret + std::string(1, '\0') + "x", "on one line"},
        {secret + "\xC3\x28", "not valid UTF-8"},           // a lead byte with a bad continuation
        {secret + "\xC0\xAF", "not valid UTF-8"},           // an overlong '/'
        {secret + "\xED\xA0\x80", "not valid UTF-8"},       // a surrogate
        {secret + "\xF4\x90\x80\x80", "not valid UTF-8"},   // past U+10FFFF
        {std::string(5000, 'a'), "more than a password can be"},
    };
    for (const Case& c : refusals) {
        const ReadOutcome r = readFrom(c.bytes, "refuse");
        CHECK(!r.ok);
        CHECK(has(r.error, c.mentions));
        CHECK(has(r.error, "--web-password-file"));
        CHECK(!has(r.error, secret));  // the sentence never holds a byte of the content
        CHECK(r.value.empty());        // and a refused read leaves no password behind
    }
    // A file that is not there: named by the flag, and by its name.
    {
        Secret s;
        std::string error;
        CHECK(!cascade::engine::readPassword((fs::temp_directory_path() / "foxsdr-prov-not-there.txt").string(), s, error));
        CHECK(has(error, "--web-password-file") && has(error, "cannot open") && has(error, "foxsdr-prov-not-there.txt"));
        CHECK(s.empty());
    }
    std::printf("R: the password's read: one line end dropped, nothing else; 11 refusals in sentences that hold none of it\n");
}

// ---------------------------------------------------------------------------------------------------------------------------------
// J
// ---------------------------------------------------------------------------------------------------------------------------------
bool parses(const std::string& text, cascade::engine::ServerConfigRequest& out, std::string& why) {
    return cascade::engine::parseServerConfigRequest(text, out, why);
}

void testText() {
    cascade::engine::ServerConfigRequest q;
    std::string why;
    {
        ProvisionRequest r;
        r.webEnable = true;
        const std::string t = cascade::engine::serverConfigText(r, nullptr);
        CHECK(parses(t, q, why));
        CHECK(q.webEnabled.has_value() && *q.webEnabled);
        CHECK(!q.webBindAddress && !q.webPort && !q.webUsername && !q.webPassword && !q.catEnabled);  // leave the rest alone
    }
    {
        ProvisionRequest r;
        r.webEnable = true;
        r.webBind = "::1";
        r.webPort = 9123;
        r.webUser = "operator \"one\"";
        r.catEnable = true;
        r.catBindAll = true;
        Secret pw;
        pw.value = "pa55word-ok";
        std::string t = cascade::engine::serverConfigText(r, &pw);
        CHECK(parses(t, q, why));
        CHECK(*q.webBindAddress == "::1" && *q.webPort == 9123 && *q.webUsername == "operator \"one\"");
        CHECK(*q.webPassword == "pa55word-ok");
        CHECK(*q.catEnabled && *q.catBindAll);
        Secret::wipe(t);
    }
    {
        ProvisionRequest r;
        r.catEnable = true;
        const std::string t = cascade::engine::serverConfigText(r, nullptr);
        CHECK(parses(t, q, why));
        CHECK(!q.webEnabled && *q.catEnabled && !q.catBindAll);
    }
    // Every character JSON must escape, and some it need not, come back as the same bytes.
    for (const std::string& pw : {std::string("quo\"te-and\\back-1"), std::string("tab\there-12345"),
                                  std::string("caf\xC3\xA9 cr\xC3\xA8me-9"), std::string("emoji-\xF0\x9F\xA6\x8A-fox"),
                                  std::string("{\"web\":{}}-looks-like-json"), std::string("\x01\x1f-control-chars-9")}) {
        ProvisionRequest r;
        r.webEnable = true;
        Secret s;
        s.value = pw;
        std::string t = cascade::engine::serverConfigText(r, &s);
        CHECK(parses(t, q, why));
        CHECK(q.webPassword.has_value() && *q.webPassword == pw);
        Secret::wipe(t);
    }
    std::printf("J: the request's text parses in the engine's own parser and reads back as the flags said; awkward passwords round-trip\n");
}

// ---------------------------------------------------------------------------------------------------------------------------------
// W
// ---------------------------------------------------------------------------------------------------------------------------------
void testWipe() {
    // A heap buffer (longer than any small-string buffer): after the wipe its bytes - still owned by the string - are zero.
    std::string s(64, 'x');
    const char* p = s.data();
    Secret::wipe(s);
    CHECK(s.empty());
    bool allZero = true;
    for (int i = 0; i < 64; ++i) { allZero = allZero && p[i] == '\0'; }
    CHECK(allZero);
    std::printf("W: Secret::wipe overwrites the bytes, not only the length\n");
}

// ---------------------------------------------------------------------------------------------------------------------------------
// P
// ---------------------------------------------------------------------------------------------------------------------------------
int freePort() {
#if defined(_WIN32)
    WSADATA d;
    (void)WSAStartup(MAKEWORD(2, 2), &d);
#endif
    SockT s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    int port = 0;
    if (s != INVALID_SOCKET && ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
        socklen_t len = sizeof(a);
        if (::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) == 0) { port = ntohs(a.sin_port); }
    }
#if defined(_WIN32)
    ::closesocket(s);
#else
    ::close(s);
#endif
    return port;
}

// A listener the test holds, so that port is taken.
struct PortHolder {
    SockT s = INVALID_SOCKET;
    bool held = false;
    explicit PortHolder(int port) {
        s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = ::htons(static_cast<unsigned short>(port));
        a.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
#if defined(_WIN32)
        const int excl = 1;  // (the Windows default lets a second socket share a port; a real server's is exclusive)
        ::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof(excl));
#endif
        held = s != INVALID_SOCKET && ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0 && ::listen(s, 4) == 0;
    }
    ~PortHolder() {
#if defined(_WIN32)
        ::closesocket(s);
#else
        ::close(s);
#endif
    }
};

struct Rig {
    Engine e;
    Rig() {
        e.configureApi("testHooks=1;servers=1");  // hermetic, but it hosts its servers: a listener really starts
        e.initialise();
    }
    ~Rig() { e.shutdown(); }
};

constexpr std::chrono::milliseconds kBound{60000};

void testEngine() {
    // P1. A loopback web server with a password: up, and the stored record is the engine's own and verifies the password.
    {
        Rig r;
        const int port = freePort();
        ProvisionRequest req;
        req.webEnable = true;
        req.webPort = port;
        req.webUser = "operator";
        Secret pw;
        pw.value = "Zq7-fox-lantern-82";
        const auto out = cascade::engine::provisionServers(r.e, req, &pw, kBound);
        CHECK(out.ok);
        if (!out.ok) { std::printf("      refused: %s\n", out.error.c_str()); }
        CHECK(has(out.summary, "web on 127.0.0.1:" + std::to_string(port)) && has(out.summary, "sign-in required"));
        CHECK(!has(out.summary, pw.value) && !has(out.error, pw.value));
        const cascade::engine::ServerConfig cfg = r.e.servers().config();
        CHECK(cfg.web.enabled && cfg.web.port == port && cfg.web.username == "operator");
        cascade::net::PasswordRecord rec;
        std::string err;
        CHECK(cascade::net::PasswordRecord::parse(cfg.web.passwordRecord, rec, err));
        std::string verr;
        CHECK(cascade::net::verifyPassword(pw.value, rec, verr) && verr.empty());
        CHECK(!cascade::net::verifyPassword("not-the-password-1", rec, verr));
        CHECK(r.e.servers().webServer().running());
    }

    // P2. An off-machine bind with no password: the POLICY'S sentence, nothing listening, no listener refused-but-running.
    {
        Rig r;
        const int port = freePort();
        ProvisionRequest req;
        req.webEnable = true;
        req.webBind = "0.0.0.0";
        req.webPort = port;
        const auto out = cascade::engine::provisionServers(r.e, req, nullptr, kBound);
        CHECK(!out.ok);
        cascade::net::WebServerConfig probe;
        probe.enabled = true;
        probe.bindAddress = "0.0.0.0";
        probe.port = port;
        const cascade::net::BindDecision d = cascade::net::evaluateBind(probe);
        CHECK(!d.allowed() && !d.reason.empty());
        // The sentence is the policy's own, whole (the list's detail is cut at 160 characters; the policy fact is not).
        CHECK(has(out.error, d.reason));
        std::printf("P2: the refusal for a wildcard bind with no password: '%s'\n", out.error.c_str());
        CHECK(!r.e.servers().webServer().running());
    }

    // P3. A password under eight characters: the engine's refusal, without the password in it.
    {
        Rig r;
        ProvisionRequest req;
        req.webEnable = true;
        req.webPort = freePort();
        Secret pw;
        pw.value = "short7!";
        const auto out = cascade::engine::provisionServers(r.e, req, &pw, kBound);
        CHECK(!out.ok);
        CHECK(has(out.error, "at least"));
        CHECK(!has(out.error, pw.value));
        CHECK(!r.e.servers().webServer().running());
    }

    // P4. A port that is taken: the listener's own refusal, and it is a refusal, not a provisioned server.
    {
        Rig r;
        const int port = freePort();
        PortHolder held(port);
        CHECK(held.held);
        ProvisionRequest req;
        req.webEnable = true;
        req.webPort = port;
        const auto out = cascade::engine::provisionServers(r.e, req, nullptr, kBound);
        CHECK(!out.ok);
        CHECK(has(out.error, "web server was not started"));
        std::printf("P4: the refusal for a port that is taken: '%s'\n", out.error.c_str());
    }

    // P5. The same request twice is NO_CHANGE, which is not a refusal: the second says the listener is still up.
    {
        Rig r;
        ProvisionRequest req;
        req.webEnable = true;
        req.webPort = freePort();
        CHECK(cascade::engine::provisionServers(r.e, req, nullptr, kBound).ok);
        const auto again = cascade::engine::provisionServers(r.e, req, nullptr, kBound);
        CHECK(again.ok);
        CHECK(has(again.summary, "no password: loopback only"));
    }
    std::printf("P: provisionServers against a real engine: accepted, refused by the policy, refused for the password, refused by the machine\n");
}

}  // namespace

int main() {
    engine_api_test::hermetic("provision");
    std::printf("test_engine_provision\n");
    testRead();
    testText();
    testWipe();
    testEngine();
    return testSummary("test_engine_provision");
}
