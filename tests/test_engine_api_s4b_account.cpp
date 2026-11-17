// test_engine_api_s4b_account.cpp - open row M9-api-account-e2e: changing or removing the web page's password ends the program interface's open streams, proved against the REAL server
// configuration of a REAL engine (not a stand-in account): the account is set, changed and removed with the engine's own SERVER_CONFIG op (the password is hashed by the engine's own worker), the
// interface is switched on through the engine's own settings with "every address" ticked, and the engine's own control pass does the rest.
//
//   E1  no password set: "every address" does not start the interface and says why (no socket made)
//   E2  a password set: the interface starts; no credentials = 401; the right login = 200; a WebSocket and an event stream are open under it
//   E3  the password CHANGED: the old login is refused on the very next request, the new one is let in, and both streams opened under the old password end on the engine's passes
//   E4  the password REMOVED: both streams opened under the new one end, nobody is let in (the stored login is not honoured), and the interface says it needs the account
//   E5  the account hook the interface reads is the engine's (a user name change alone also ends the streams)
//
// The interface binds 127.0.0.1 in every case (ServerSet::setApiBindForTest, a test-only seam that is not a setting), so "every address" is proved without a firewall prompt. Nothing leaves this computer;
// no radio is opened.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdlib>
#include <thread>

#include "net/ws_frame.hpp"
#include "net_test_support.hpp"
#include "wire_c_rig.hpp"

using namespace wirec_test;
using nettest::Client;
using nettest::msSince;

namespace {

std::string basic(const std::string& user, const std::string& pass) { return "Authorization: Basic " + cascade::net::ws::base64Encode(user + ":" + pass) + "\r\n"; }
std::string statusReq(const std::string& extra) { return "GET /status HTTP/1.1\r\nHost: 127.0.0.1\r\n" + extra + "\r\n"; }
const char* kWsKey = "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n";

int ask(int port, const std::string& req) {
    Client c;
    if (!c.connect(static_cast<std::uint16_t>(port))) { return -1; }
    c.sendBytes(req.data(), req.size());
    c.waitEof(6000);
    const std::string all(c.got.begin(), c.got.end());
    return all.size() > 12 ? std::atoi(all.substr(9, 3).c_str()) : 0;
}

bool has(Client& c, const std::string& s) { return std::string(c.got.begin(), c.got.end()).find(s) != std::string::npos; }

bool waitHas(Rig& r, Client& c, const std::string& s, int ms = 6000) {
    const auto t0 = nettest::Clock::now();
    while (msSince(t0) < ms) {
        if (has(c, s)) { return true; }
        if (c.eof) { return false; }
        r.pass(1);
        c.pump(30);
    }
    return has(c, s);
}

// Runs the engine's control pass until the client's connection ends (or `ms` pass).
bool endsWithPasses(Rig& r, Client& c, int ms = 5000) {
    const auto t0 = nettest::Clock::now();
    while (msSince(t0) < ms) {
        r.pass(1);
        c.pump(30);
        if (c.eof) { return true; }
    }
    return c.eof;
}

// A free port on this computer's loopback address (bound and closed again).
int freePort() {
    namespace sock = cascade::net::sock;
    std::string err;
    const sock::Handle h = sock::listenTcp(0x7F000001u, 0, 1, err);
    if (h == sock::kInvalid) { return 38074; }
    const int p = sock::boundPort(h);
    sock::closeHandle(h);
    return p;
}

template <class F>
bool until(Rig& r, F f, int ms = 8000) {
    const auto t0 = nettest::Clock::now();
    while (msSince(t0) < ms) {
        r.pass(1);
        if (f()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    return f();
}

// The engine's own SERVER_CONFIG op; the password is hashed on the engine's worker, which the pass polls: wait for the account to be in place.
std::int32_t serverConfig(Rig& r, const std::string& json, std::string* msg = nullptr) { return r.status(textOp(FOXAPI_OP_SERVER_CONFIG, json), msg); }

bool accountIs(Rig& r, const std::string& user, bool hasPassword) {
    return until(r, [&] {
        const cascade::engine::ServerConfig c = r.eng.servers().config();
        return c.web.username == user && (c.web.passwordRecord.empty() != hasPassword);
    });
}

void testAccountEndToEnd(const fs::path& dir) {
    Rig rig(dir);
    Access::wirec(rig.eng).servers.setApiBindForTest("127.0.0.1");
    rig.pass(4);
    // E1: "every address", no password: the interface does not start and says why
    CHECK(rig.status(setting("srv.api.port", std::to_string(freePort()))) == FOXAPI_OK);
    CHECK(rig.status(setting("srv.api.all", "1")) == FOXAPI_OK);
    CHECK(rig.status(setting("srv.api.enabled", "1")) == FOXAPI_OK);
    CHECK(until(rig, [&] { return rig.fact("srv.api.problem").find("password") != std::string::npos; }));
    CHECK(rig.fact("srv.api.running") == "0");

    // E2: a password set through the engine's own configuration
    CHECK(serverConfig(rig, "{\"web\":{\"username\":\"admin\",\"password\":\"first password here\"}}") == FOXAPI_OK);
    CHECK(accountIs(rig, "admin", true));
    // the interface retries on a configuration change of its own: switch it off and on so it looks at the account now in place
    CHECK(rig.status(setting("srv.api.enabled", "0")) == FOXAPI_OK);
    CHECK(rig.status(setting("srv.api.enabled", "1")) == FOXAPI_OK);
    CHECK(until(rig, [&] { return rig.fact("srv.api.running") == "1"; }));
    const int port = std::atoi(rig.fact("srv.api.port").c_str());
    CHECK(port > 0);
    CHECK(ask(port, statusReq("")) == 401);
    CHECK(ask(port, statusReq(basic("admin", "first password here"))) == 200);
    CHECK(ask(port, statusReq(basic("admin", "wrong"))) == 401);
    Client ws, sse;
    CHECK(ws.connect(static_cast<std::uint16_t>(port)) && sse.connect(static_cast<std::uint16_t>(port)));
    const std::string wsReq = "GET /ws HTTP/1.1\r\nHost: 127.0.0.1\r\n" + std::string(kWsKey) + basic("admin", "first password here") + "\r\n";
    const std::string sseReq = "GET /events HTTP/1.1\r\nHost: 127.0.0.1\r\n" + basic("admin", "first password here") + "\r\n";
    ws.sendBytes(wsReq.data(), wsReq.size());
    sse.sendBytes(sseReq.data(), sseReq.size());
    CHECK(waitHas(rig, ws, "101") && waitHas(rig, sse, "text/event-stream"));
    rig.pass(3);   // the pass notes the account as it is

    // E3: the password changed
    CHECK(serverConfig(rig, "{\"web\":{\"password\":\"second password here\"}}") == FOXAPI_OK);
    CHECK(until(rig, [&] { return ask(port, statusReq(basic("admin", "second password here"))) == 200; }));
    CHECK(ask(port, statusReq(basic("admin", "first password here"))) == 401);   // the remembered login of the old password is not honoured
    CHECK(endsWithPasses(rig, ws) && endsWithPasses(rig, sse));                     // the streams opened under the old password ended
    Client ws2, sse2;
    CHECK(ws2.connect(static_cast<std::uint16_t>(port)) && sse2.connect(static_cast<std::uint16_t>(port)));
    const std::string ws2Req = "GET /ws HTTP/1.1\r\nHost: 127.0.0.1\r\n" + std::string(kWsKey) + basic("admin", "second password here") + "\r\n";
    const std::string sse2Req = "GET /events HTTP/1.1\r\nHost: 127.0.0.1\r\n" + basic("admin", "second password here") + "\r\n";
    ws2.sendBytes(ws2Req.data(), ws2Req.size());
    sse2.sendBytes(sse2Req.data(), sse2Req.size());
    CHECK(waitHas(rig, ws2, "101") && waitHas(rig, sse2, "text/event-stream"));
    rig.pass(3);

    // E4: the password removed: the new streams end and nobody gets in
    CHECK(serverConfig(rig, "{\"web\":{\"clearPassword\":true}}") == FOXAPI_OK);
    CHECK(accountIs(rig, "admin", false));
    CHECK(endsWithPasses(rig, ws2) && endsWithPasses(rig, sse2));
    CHECK(ask(port, statusReq(basic("admin", "second password here"))) == 401);   // the stored login is not honoured: with no password nobody can be told from anybody
    CHECK(ask(port, statusReq(basic("admin", "first password here"))) == 401);
    CHECK(ask(port, statusReq("")) == 401);

    // E5: a password back, a stream open, then only the user name changes
    CHECK(serverConfig(rig, "{\"web\":{\"password\":\"third password here\"}}") == FOXAPI_OK);
    CHECK(accountIs(rig, "admin", true));
    CHECK(until(rig, [&] { return ask(port, statusReq(basic("admin", "third password here"))) == 200; }));
    Client ws3;
    CHECK(ws3.connect(static_cast<std::uint16_t>(port)));
    const std::string ws3Req = "GET /ws HTTP/1.1\r\nHost: 127.0.0.1\r\n" + std::string(kWsKey) + basic("admin", "third password here") + "\r\n";
    ws3.sendBytes(ws3Req.data(), ws3Req.size());
    CHECK(waitHas(rig, ws3, "101"));
    rig.pass(3);
    CHECK(serverConfig(rig, "{\"web\":{\"username\":\"operator\"}}") == FOXAPI_OK);
    CHECK(accountIs(rig, "operator", true));
    CHECK(endsWithPasses(rig, ws3));
    CHECK(ask(port, statusReq(basic("admin", "third password here"))) == 401);
}

// M9-api-host-rule through the real engine: the interface (this computer only, no password) answers the DNS names of the web page's own Host rule - the computer's name, name.local and the hosts of the
// "Allowed web origins" setting - and no other name, no other literal, and nothing with an Origin.
void testHostNamesFromTheEngine(const fs::path& dir) {
    Rig rig(dir);
    rig.pass(4);
    CHECK(rig.status(setting("srv.api.port", std::to_string(freePort()))) == FOXAPI_OK);
    CHECK(rig.status(setting("srv.api.enabled", "1")) == FOXAPI_OK);   // this computer only: Host is the guard
    CHECK(until(rig, [&] { return rig.fact("srv.api.running") == "1"; }));
    const int port = std::atoi(rig.fact("srv.api.port").c_str());
    CHECK(port > 0);
    const auto get = [&](const std::string& host, const std::string& extra = std::string()) { return ask(port, "GET /status HTTP/1.1\r\nHost: " + host + "\r\n" + extra + "\r\n"); };
    CHECK(get("127.0.0.1") == 200 && get("localhost:" + std::to_string(port)) == 200);
    CHECK(get("sdr.example.com") == 403);   // not listed yet
    const std::string computer = cascade::net::sock::computerName();
    if (!computer.empty()) { CHECK(get(computer) == 200 && get(computer + ".local") == 200); }   // the computer's own name, as the page takes it
    CHECK(rig.status(setting("web.cors_origins", "https://sdr.example.com")) == FOXAPI_OK);
    CHECK(until(rig, [&] { return get("sdr.example.com") == 200; }, 10000));   // the interface takes the owner's list within a second
    CHECK(get("SDR.example.com:8443") == 200);
    CHECK(get("evil.example") == 403 && get("sdr.example.com.evil.example") == 403 && get("192.168.1.20") == 403 && get("sdr.example.com", "Origin: https://sdr.example.com\r\n") == 403);
    CHECK(rig.status(setting("web.cors_origins", "")) == FOXAPI_OK);
    CHECK(until(rig, [&] { return get("sdr.example.com") == 403; }, 10000));   // taking a name off the list takes the permission away
}

}  // namespace

int main() {
    engine_api_test::hermetic("s4b_account");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const fs::path root = scratch("s4bacct");
    std::error_code ec;
    fs::create_directories(root / "0", ec);
    testAccountEndToEnd(root / "0");
    fs::create_directories(root / "1", ec);
    testHostNamesFromTheEngine(root / "1");
    fs::remove_all(root, ec);
    return check::finish("test_engine_api_s4b_account");
}
