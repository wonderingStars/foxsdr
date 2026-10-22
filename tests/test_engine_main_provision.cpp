// Headless provisioning of `foxsdr-engine` (engine extraction stage 7e): the web server and the CAT listener set up from the
// command line, on the real executable, with a real HTTP client and real sockets on the loopback.
//
// WHAT IS ASSERTED
//   A. A HERMETIC RUN, PROVISIONED (`--web-enable --web-port N --web-password-file F`, no settings file): before the login the
//      page answers 401; a wrong password is 401; the password in the file - read with its trailing line end dropped - signs in
//      (200, a cookie that then opens /api/status); the run says on stderr that the setting is for this run only; asked to
//      stop it exits 0 - and it wrote NOTHING to the disk (the hermetic rule).
//   B. THE SAME FROM STANDARD INPUT (`--web-password-file -`).
//   C. PERSISTENCE WITH A SETTINGS FILE: a run with `--config PATH` and the flags keeps them - the file then holds the web
//      settings and a PBKDF2 record (the engine's own, the panel's: it parses, carries the default iteration count and VERIFIES
//      the password) and NOT the password - and a second run with that file and NO provisioning flags restores the server: the
//      page is closed (401) and the same password signs in. (Flags apply for the run; persistence needs --config.)
//   D. THE REFUSALS, each with exit 1, the sentence on stderr, no "running" line, nothing listening, and - with a settings file -
//      the file byte-for-byte what it was:
//        * a wildcard bind with NO password: the BIND POLICY'S sentence, whole (taken from net::evaluateBind in this test, not
//          copied), hermetic and with a settings file;
//        * a port that something else holds (web, and CAT's 4532);
//        * a password file that is missing, empty, or not there on standard input; a password under eight characters (the
//          engine's sentence, which holds no password).
//   E. CAT: `--cat-enable` listens on 127.0.0.1:4532 and answers a `f` request (when this machine's 4532 is free; otherwise the
//      case says it did not run).
//
// Break-it (docs/engine-stage4.md 6.19): the policy's refusal not surfaced (provisionServers ignoring the listener's state) -> D's
// wildcard cases are RED (exit 0, a running engine with no web server); the settings file written after a refusal (the writer not
// forbidden) -> D's byte comparison is RED.
//
// HERMETIC: every per-user folder is a scratch tree, the settings file a scratch path named on the command line, the volume
// seeded 0 (nothing plays), the telemetry endpoint a closed port. The real %APPDATA% config is never named. The passwords are
// generated for the run (never a fixture in the tree).
//
// UNVERIFIED on this machine: Linux and arm64 (the POSIX halves of the spawn helper), and the CAT `--cat-bind-all` listener, which
// would open a wildcard socket on a developer's machine (a firewall prompt) - its refusal is exercised, its success is not.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

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

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "core/config.hpp"
#include "engine_main_proc.hpp"
#include "net/web_auth.hpp"
#include "net/web_policy.hpp"
#include "test_check.hpp"

namespace emt = engine_main_test;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

// ---- sockets ---------------------------------------------------------------------------------------------------------------------
void sockInit() {
#if defined(_WIN32)
    static bool done = false;
    if (!done) {
        WSADATA d;
        (void)WSAStartup(MAKEWORD(2, 2), &d);
        done = true;
    }
#endif
}

void closeSock(SockT s) {
    if (s == INVALID_SOCKET) { return; }
#if defined(_WIN32)
    ::closesocket(s);
#else
    ::close(s);
#endif
}

// A port nobody is listening on right now: bound, read back, closed.
int freePort() {
    sockInit();
    SockT s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    int port = 0;
    if (s != INVALID_SOCKET && ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
        socklen_t len = sizeof(a);
        if (::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) == 0) { port = ntohs(a.sin_port); }
    }
    closeSock(s);
    return port;
}

// A listener the test holds, so that port is taken (the "something else has it" of a real rigctld or web server).
struct PortHolder {
    SockT s = INVALID_SOCKET;
    bool held = false;
    explicit PortHolder(int port) {
        sockInit();
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
    ~PortHolder() { closeSock(s); }
    PortHolder(const PortHolder&) = delete;
    PortHolder& operator=(const PortHolder&) = delete;
};

// One request to a CAT listener on the loopback: the line back, or empty.
std::string catAsk(int port, const std::string& line) {
    sockInit();
    SockT s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { return std::string(); }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = ::htons(static_cast<unsigned short>(port));
    a.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    std::string reply;
    if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
#if defined(_WIN32)
        const DWORD tmo = 5000;
        ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof(tmo));
#else
        timeval tv{5, 0};
        ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
        (void)::send(s, line.data(), static_cast<int>(line.size()), 0);
        char buf[256];
        const int n = static_cast<int>(::recv(s, buf, sizeof(buf), 0));
        if (n > 0) { reply.assign(buf, static_cast<std::size_t>(n)); }
    }
    closeSock(s);
    return reply;
}

// ---- the web page ----------------------------------------------------------------------------------------------------------------
struct Page {
    int port = 0;
    int status(const std::string& cookie = std::string()) const {
        httplib::Client cli("127.0.0.1", port);
        cli.set_connection_timeout(2, 0);
        cli.set_read_timeout(5, 0);
        httplib::Headers h;
        if (!cookie.empty()) { h.emplace("Cookie", cookie); }
        const auto r = cli.Get("/api/status", h);
        return r ? r->status : -1;
    }
    // The login's status; the cookie ("name=value") in `cookie` when it is 200.
    int login(const std::string& user, const std::string& password, std::string* cookie = nullptr) const {
        httplib::Client cli("127.0.0.1", port);
        cli.set_connection_timeout(2, 0);
        cli.set_read_timeout(5, 0);
        json body;
        body["username"] = user;
        body["password"] = password;
        const auto r = cli.Post("/api/login", body.dump(), "application/json");
        if (!r) { return -1; }
        if (r->status == 200 && cookie != nullptr) {
            const std::string c = r->get_header_value("Set-Cookie");
            *cookie = c.substr(0, c.find(';'));
        }
        return r->status;
    }
    bool listening() const { return status() != -1; }
};

// ---- helpers ---------------------------------------------------------------------------------------------------------------------
// A password for this run: generated, never a fixture (and never the same twice).
std::string generatedPassword() {
    std::random_device rd;
    std::mt19937_64 g((static_cast<std::uint64_t>(rd()) << 32) ^ rd() ^
                      static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    static const char* alphabet = "abcdefghjkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    std::string p = "t7-";
    for (int i = 0; i < 20; ++i) { p += alphabet[g() % 54u]; }
    return p;
}

void writeText(const fs::path& p, const std::string& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void seed(const fs::path& cfg) {
    cascade::core::AppConfig s;  // a predecessor that ended cleanly (the default), silent
    s.volume = 0.0f;
    writeText(cfg, cascade::core::ConfigStore::serialize(s));
}

bool waitErr(const fs::path& errFile, const std::string& needle, int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (emt::contains(emt::readText(errFile), needle)) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
}

json readConfig(const fs::path& p) {
    json j = json::parse(emt::readFile(p), nullptr, false);
    return j.is_object() ? j : json();
}

// The sentence the bind policy gives a wildcard bind with no password, from the policy itself.
std::string policySentenceFor(const std::string& bind, int port) {
    cascade::net::WebServerConfig probe;
    probe.enabled = true;
    probe.bindAddress = bind;
    probe.port = port;
    const cascade::net::BindDecision d = cascade::net::evaluateBind(probe);
    return d.allowed() ? std::string() : d.reason;
}

const char* kUp = "(until ended from outside)";

struct Env {
    fs::path root, home, outDir, engineExe;
    int n = 0;
    std::string tag(const char* what) { return std::string(what) + "-" + std::to_string(n++); }
};

// A run that goes on until it is asked to stop: started, and waited until it says it is running (or has ended).
struct Live {
    emt::Child child;
    fs::path errFile, outFile;
    bool up = false;
};

bool startLive(Env& env, Live& run, const std::vector<std::string>& args, const std::string& tag,
               const fs::path& stdinFile = fs::path()) {
    run.errFile = env.outDir / (tag + ".err");
    run.outFile = env.outDir / (tag + ".out");
    run.child.setConsoleGroup(true);
    if (!stdinFile.empty()) { run.child.setStdinFile(stdinFile); }
    if (!run.child.start(env.engineExe, args, env.home, run.outFile, run.errFile)) { return false; }
    run.up = waitErr(run.errFile, kUp, 60000);
    return run.up;
}

// Asked to stop as an operator asks; true when it exited 0 in time.
bool stopLive(Live& run) {
    if (!run.child.requestStop()) { return false; }
    return run.child.waitExit(60000) && run.child.code() == 0;
}

bool login(const Page& page, const std::string& user, const std::string& password) {
    std::string cookie;
    if (page.login(user, password, &cookie) != 200 || cookie.empty()) { return false; }
    return page.status(cookie) == 200;
}

}  // namespace

int main() {
    std::printf("test_engine_main_provision\n");
    Env env;
    env.root = emt::scratch("engine-provision");
    env.home = env.root / "home";
    env.outDir = env.root / "out";
    fs::create_directories(env.home / "tmp");
    fs::create_directories(env.outDir);
    emt::isolate(env.home);
    CHECK(cascade::core::ConfigStore::defaultPath().rfind(env.home.string(), 0) == 0);
    env.engineExe = emt::appExe("foxsdr-engine");
    CHECK(fs::exists(env.engineExe));

    const std::string password = generatedPassword();
    const fs::path pwFile = env.root / "secrets" / "web.pw";
    writeText(pwFile, password + "\n");

    // --- A. hermetic, from a file ---------------------------------------------------------------------------------------------
    {
        const int port = freePort();
        Live run;
        const bool up = startLive(env, run, {"--web-enable", "--web-port", std::to_string(port), "--web-password-file", pwFile.string()},
                                  env.tag("a"));
        CHECK(up);
        const std::string err = emt::readText(run.errFile);
        std::printf("  A stderr: %s\n", err.c_str());
        CHECK(emt::contains(err, "web on 127.0.0.1:" + std::to_string(port) + " (sign-in required)"));
        CHECK(emt::contains(err, "for this run only (no settings file: nothing is stored)"));
        CHECK(emt::contains(err, "signal generator, muted, no config"));
        const Page page{port};
        CHECK(page.status() == 401);  // closed until the login
        CHECK(page.login("admin", password + "x") == 401);
        CHECK(page.login("someone-else", password) == 401);
        CHECK(login(page, "admin", password));
        CHECK(stopLive(run));
        CHECK(!emt::contains(emt::readText(run.errFile), password));
        // NOTHING written: the scratch profile holds only the "tmp" directory the test made.
        const std::vector<std::string> left = emt::tree(env.home);
        CHECK(left.size() == 1);
        if (left.size() != 1) { for (const std::string& l : left) { std::printf("      left behind: %s\n", l.c_str()); } }
    }

    // --- B. the same from standard input ---------------------------------------------------------------------------------------
    {
        const int port = freePort();
        const std::string pw2 = generatedPassword();
        const fs::path stdinFile = env.root / "secrets" / "stdin.txt";
        writeText(stdinFile, pw2 + "\r\n");  // as a Windows editor leaves it
        Live run;
        CHECK(startLive(env, run, {"--web-enable", "--web-port", std::to_string(port), "--web-user", "operator", "--web-password-file", "-"},
                        env.tag("b"), stdinFile));
        const Page page{port};
        CHECK(page.status() == 401);
        CHECK(page.login("admin", pw2) == 401);  // the user name was asked for
        CHECK(login(page, "operator", pw2));
        CHECK(stopLive(run));
    }

    // --- C. with a settings file: kept, restored by a run with no flags ------------------------------------------------------
    {
        const int port = freePort();
        const fs::path cfg = env.home / "cx" / "config.json";
        seed(cfg);
        std::string recordAfterFirst;
        {
            Live run;
            CHECK(startLive(env, run, {"--config", cfg.string(), "--web-enable", "--web-port", std::to_string(port), "--web-password-file", pwFile.string()},
                            env.tag("c1")));
            const std::string err = emt::readText(run.errFile);
            CHECK(emt::contains(err, "kept in the settings file"));
            const Page page{port};
            CHECK(page.status() == 401);
            CHECK(login(page, "admin", password));
            CHECK(stopLive(run));
        }
        {
            const json j = readConfig(cfg);
            CHECK(j.is_object());
            CHECK(j.value("webEnabled", false));
            CHECK(j.value("webPort", 0) == port);
            recordAfterFirst = j.value("webPasswordRecord", std::string());
            CHECK(!recordAfterFirst.empty());
            // The panel's record, by construction (the same hash, on the same worker): it parses, has the default iteration count
            // and verifies the password.
            cascade::net::PasswordRecord rec;
            std::string perr;
            CHECK(cascade::net::PasswordRecord::parse(recordAfterFirst, rec, perr));
            CHECK(recordAfterFirst.rfind("pbkdf2-sha256$", 0) == 0);
            CHECK(rec.iterations == cascade::net::kDefaultIterations);
            std::string verr;
            CHECK(cascade::net::verifyPassword(password, rec, verr) && verr.empty());
            // ...and the file holds NOT the password.
            CHECK(!emt::contains(emt::readFile(cfg), password));
        }
        {
            // THE RESTART WITH NO PROVISIONING FLAGS: the settings come back from the file.
            Live run;
            CHECK(startLive(env, run, {"--config", cfg.string()}, env.tag("c2")));
            const std::string err = emt::readText(run.errFile);
            CHECK(!emt::contains(err, "web on "));  // nothing was provisioned by this run: the file said it
            const Page page{port};
            CHECK(page.status() == 401);
            CHECK(login(page, "admin", password));
            CHECK(page.login("admin", password + "x") == 401);
            CHECK(stopLive(run));
            const json j = readConfig(cfg);
            CHECK(j.value("webEnabled", false) && j.value("webPasswordRecord", std::string()) == recordAfterFirst);
        }
        std::printf("C: kept in the file (record %zu bytes, no plaintext), restored by a run with no flags\n", recordAfterFirst.size());
    }

    // --- D. the refusals ---------------------------------------------------------------------------------------------------------
    {
        const auto refused = [&](const std::vector<std::string>& args, const char* what, const std::string& mentions,
                                 int expectPort = 0) {
            // (`--seconds 15` is a safety net, not part of the case: a refusal is over before the receiver starts, so it is never
            // reached - a run that is wrongly ACCEPTED ends by itself in 15 s with exit 0, and the exit-code check below says so,
            // instead of the test waiting out its timeout.)
            std::vector<std::string> withNet = args;
            withNet.push_back("--seconds");
            withNet.push_back("15");
            const emt::RunResult x = emt::run(env.engineExe, withNet, env.home, env.outDir, env.tag(what), 120000);
            std::printf("  D %s: exit %d, stderr: %s\n", what, x.exitCode, x.err.c_str());
            CHECK(x.started && !x.timedOut);
            CHECK(x.exitCode == 1);
            CHECK(emt::contains(x.err, mentions));
            CHECK(!emt::contains(x.err, "running,"));  // it never started
            CHECK(x.out.empty());
            if (expectPort != 0) { CHECK(!Page{expectPort}.listening()); }
            return x;
        };

        // The policy: an off-machine bind with no password. Its sentence, whole; hermetic and with a settings file.
        {
            const int port = freePort();
            const std::string sentence = policySentenceFor("0.0.0.0", port);
            CHECK(!sentence.empty());
            const std::vector<std::string> before0 = emt::tree(env.home);
            refused({"--web-enable", "--web-bind", "0.0.0.0", "--web-port", std::to_string(port)}, "wildcard-hermetic", sentence, port);
            CHECK(emt::tree(env.home) == before0);  // a hermetic refusal writes nothing at all
            const fs::path cfg = env.home / "cy" / "config.json";
            seed(cfg);
            const std::string before = emt::readFile(cfg);
            refused({"--config", cfg.string(), "--web-enable", "--web-bind", "0.0.0.0", "--web-port", std::to_string(port)},
                    "wildcard-config", sentence, port);
            CHECK(emt::readFile(cfg) == before);  // the file is what it was, byte for byte (the marker included)
            // A refused flag is not a setting: the next run with this file and no flags has no web server.
            Live run;
            CHECK(startLive(env, run, {"--config", cfg.string()}, env.tag("after-refusal")));
            CHECK(!Page{port}.listening());
            CHECK(stopLive(run));
            CHECK(!readConfig(cfg).value("webEnabled", true));
            // A bind to a specific off-machine-looking address is the same rule (the policy decides, not a list of strings here).
            const std::string sentence2 = policySentenceFor("::", port);
            refused({"--web-enable", "--web-bind", "::", "--web-port", std::to_string(port)}, "wildcard6", sentence2, port);
        }

        // The machine: a port something else holds.
        {
            const int port = freePort();
            PortHolder holder(port);
            CHECK(holder.held);
            refused({"--web-enable", "--web-port", std::to_string(port)}, "web-port-taken", "the web server was not started");
            // ...and a settings file stays as it was.
            const fs::path cfg = env.home / "cz" / "config.json";
            seed(cfg);
            const std::string before = emt::readFile(cfg);
            refused({"--config", cfg.string(), "--web-enable", "--web-port", std::to_string(port)}, "web-port-taken-config",
                    "the web server was not started");
            CHECK(emt::readFile(cfg) == before);
        }
        {
            PortHolder holder(4532);  // CAT's port; if something else of this machine's has it, the refusal is the same
            if (!holder.held) { std::printf("  D: 4532 is held by another program on this machine - CAT's refusal needs no holder of ours\n"); }
            refused({"--cat-enable"}, "cat-port-taken", "the CAT listener was not started");
        }

        // The password file.
        refused({"--web-enable", "--web-password-file", (env.root / "secrets" / "not-there.pw").string()}, "pw-missing",
                "--web-password-file: cannot open \"" + (env.root / "secrets" / "not-there.pw").string() + "\"");
        {
            const fs::path empty = env.root / "secrets" / "empty.pw";
            writeText(empty, "\n");
            refused({"--web-enable", "--web-password-file", empty.string()}, "pw-empty", "holds no password");
            // `-` with nothing on standard input (the helper's stdin is NUL): the same sentence, naming the stream.
            refused({"--web-enable", "--web-password-file", "-"}, "pw-stdin-empty", "--web-password-file: standard input holds no password");
            // A password under eight characters: the ENGINE's sentence (the same one a panel's "set password" is given), no password in it.
            const fs::path shortPw = env.root / "secrets" / "short.pw";
            writeText(shortPw, "sh0rt!\n");
            const emt::RunResult x = refused({"--web-enable", "--web-password-file", shortPw.string()}, "pw-short", "at least 8 characters");
            CHECK(!emt::contains(x.err, "sh0rt!"));
        }
    }

    // --- E. CAT -------------------------------------------------------------------------------------------------------------------
    {
        bool free4532 = false;
        {
            PortHolder probe(4532);
            free4532 = probe.held;
        }
        if (!free4532) {
            std::printf("  E: port 4532 is in use on this machine - the CAT success case did not run (its refusal, in D, did)\n");
        } else {
            Live run;
            CHECK(startLive(env, run, {"--cat-enable"}, env.tag("e")));
            const std::string err = emt::readText(run.errFile);
            std::printf("  E stderr: %s\n", err.c_str());
            CHECK(emt::contains(err, "CAT on 127.0.0.1:4532"));
            CHECK(emt::contains(err, "for this run only"));
            const std::string reply = catAsk(4532, "f\n");
            std::printf("  E: 'f' answered '%s'\n", reply.c_str());
            CHECK(!reply.empty() && reply.find_first_of("0123456789") != std::string::npos);
            CHECK(stopLive(run));
        }
    }

    std::error_code ec;
    fs::remove_all(env.root, ec);
    return testSummary("test_engine_main_provision");
}
