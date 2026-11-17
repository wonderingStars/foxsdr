// test_s4b_sec_api.cpp - security lane S4b-sec, findings R8 (the WebSocket / SSE API server, net/api_server.hpp) on a REAL loopback socket with stand-in clients:
//   H  the Host rule: with no password (this computer only) a request whose Host is not this computer's own name or address is refused 403 on every route (DNS rebinding: a page of another
//      site that made its own name point here); localhost, 127.x, [::1], with or without a port, a request with no Host (not a browser) and Origin-less programs still work; a request that
//      carries an Origin is still refused; beyond this computer (requireAuth) the Host is not what guards the interface, the sign-in is
//   T  a password check never runs on the IO thread: while one sign-in is slow, another connection is served, the periodic push to an open stream goes on, a flood of sign-ins is
//      bounded (the next is answered 429), and stop() returns while a sign-in is stuck
//   P  one address cannot hold every connection slot with requests it never finishes
//   C  closeAllClients ends the open streams (WebSocket and SSE) and leaves the server serving
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "net/api_server.hpp"
#include "net/ws_frame.hpp"
#include "net_test_support.hpp"
#include "test_check.hpp"

using namespace cascade::net;
using nettest::Client;
using nettest::msSince;

namespace {

ApiConfig cfg0() {
    ApiConfig c;
    c.port = 0;
    c.statusIntervalMs = 100;
    return c;
}

int statusOf(const std::string& all) { return all.size() > 12 ? std::atoi(all.substr(9, 3).c_str()) : 0; }

// One request on its own connection: the whole response (until the server closes).
std::string ask(std::uint16_t port, const std::string& request, int ms = 4000) {
    Client c;
    if (!c.connect(port)) { return "<no connection>"; }
    c.sendBytes(request.data(), request.size());
    c.waitEof(ms);
    return std::string(c.got.begin(), c.got.end());
}

std::string get(const std::string& path, const std::string& host, const std::string& extra = std::string()) {
    return "GET " + path + " HTTP/1.1\r\n" + (host.empty() ? std::string() : "Host: " + host + "\r\n") + extra + "\r\n";
}

const char* kWsKey = "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n";

bool has(Client& c, const std::string& s) { return std::string(c.got.begin(), c.got.end()).find(s) != std::string::npos; }

bool waitHas(Client& c, const std::string& s, int ms = 3000) {
    const auto t0 = nettest::Clock::now();
    while (msSince(t0) < ms) {
        if (has(c, s)) { return true; }
        if (c.eof) { return false; }
        c.pump(20);
    }
    return has(c, s);
}

void testHost() {
    ControlHub hub;
    std::string err;
    ApiServer s(cfg0(), hub, nullptr);
    CHECK(s.start(err));
    const std::uint16_t port = static_cast<std::uint16_t>(s.port());
    const std::string p = ":" + std::to_string(port);
    // this computer's own names and addresses: served, on every route
    for (const std::string& h : {std::string("127.0.0.1"), "127.0.0.1" + p, std::string("localhost"), "LocalHost" + p, std::string("127.9.8.7"), "[::1]" + p, std::string("[::1]")}) {
        CHECK(statusOf(ask(port, get("/status", h))) == 200);
    }
    // no Host at all (HTTP/1.0, a plain program): served; a browser never omits it
    CHECK(statusOf(ask(port, "GET /status HTTP/1.0\r\n\r\n")) == 200);
    // somebody else's name or address: refused, on every route, before anything is read or any stream is opened
    for (const std::string& h : {std::string("evil.example"), "evil.example" + p, std::string("127.0.0.1.evil.example"), std::string("localhost.evil.example"), std::string("192.168.1.20"), std::string("10.0.0.1:80"),
                                 std::string("[2001:db8::1]"), std::string("127.0.0.1:"), std::string("127.0.0.1:80a"), std::string("0.0.0.0"), std::string("a")}) {
        CHECK(statusOf(ask(port, get("/status", h))) == 403);
        CHECK(statusOf(ask(port, get("/events", h))) == 403);
        CHECK(statusOf(ask(port, get("/ws", h, kWsKey))) == 403);
    }
    CHECK(s.stats().sseOpened == 0 && s.stats().wsOpened == 0);
    // the body of the refusal is the sentence, and it says nothing of the status
    const std::string r = ask(port, get("/status", "evil.example"));
    CHECK(r.find("own address only") != std::string::npos && r.find("tuned_hz") == std::string::npos);
    // an Origin (a page) is refused whatever the Host says
    CHECK(statusOf(ask(port, get("/status", "127.0.0.1", "Origin: http://127.0.0.1\r\n"))) == 403);
    CHECK(statusOf(ask(port, get("/status", "evil.example", "Origin: http://evil.example\r\n"))) == 403);
    s.stop();

    // beyond this computer (requireAuth) every request signs in: the Host is not the guard there, the sign-in is
    ApiConfig wide = cfg0();
    wide.requireAuth = true;
    ApiServer w(wide, hub, [](const std::string& a, const std::string&) { return a == "Basic QQ==" ? ApiAuth::Allowed : ApiAuth::Denied; });
    CHECK(w.start(err));
    const std::uint16_t wp = static_cast<std::uint16_t>(w.port());
    CHECK(statusOf(ask(wp, get("/status", "myradio.lan"))) == 401);
    CHECK(statusOf(ask(wp, get("/status", "myradio.lan", "Authorization: Basic QQ==\r\n"))) == 200);
    w.stop();

    // the switch the tests of other lanes rely on: checkHost = false serves any Host
    ApiConfig off = cfg0();
    off.checkHost = false;
    ApiServer o(off, hub, nullptr);
    CHECK(o.start(err));
    CHECK(statusOf(ask(static_cast<std::uint16_t>(o.port()), get("/status", "evil.example"))) == 200);
    o.stop();
}

// M9-api-host-rule: the interface takes the same DNS NAMES as the web page's Host rule (the computer's name, name.local, the hosts of "Allowed web origins"), given by setHostNames; it is not
// loosened for any other name, for an IP literal that is not its own, or for a request that carries an Origin. Without names set it behaves as it always did.
void testHostNamesOfThePage() {
    ControlHub hub;
    std::string err;
    ApiServer s(cfg0(), hub, nullptr);
    CHECK(s.start(err));
    const std::uint16_t port = static_cast<std::uint16_t>(s.port());
    const std::string p = ":" + std::to_string(port);
    for (const std::string& h : {std::string("mypc"), std::string("mypc.local"), std::string("sdr.example.com") + p}) { CHECK(statusOf(ask(port, get("/status", h))) == 403); }   // before: refused
    s.setHostNames({"mypc", "mypc.local", "SDR.Example.com"});
    for (const std::string& h : {std::string("mypc"), std::string("MYPC.local") + p, std::string("sdr.example.com"), std::string("sdr.example.com.") + p}) {
        if (statusOf(ask(port, get("/status", h))) != 200) { std::printf("      %s was refused\n", h.c_str()); }
        CHECK(statusOf(ask(port, get("/status", h))) == 200);
    }
    // never loosened: any other name, a literal that is not its own, a name that merely contains a listed one, a page's Origin
    for (const std::string& h : {std::string("evil.example"), std::string("mypc.local.evil.example"), std::string("xmypc"), std::string("sdr.example.com.evil.example"), std::string("192.168.1.20"),
                                 std::string("[2001:db8::1]"), std::string("10.0.0.1:80"), std::string("localhost.evil.example")}) {
        if (statusOf(ask(port, get("/status", h))) != 403) { std::printf("      %s was served\n", h.c_str()); }
        CHECK(statusOf(ask(port, get("/status", h))) == 403);
    }
    CHECK(statusOf(ask(port, get("/status", "mypc", "Origin: http://mypc\r\n"))) == 403);
    CHECK(statusOf(ask(port, get("/status", "127.0.0.1"))) == 200);   // the old names still work
    s.setHostNames({});                                               // taking the names away takes the permission away
    CHECK(statusOf(ask(port, get("/status", "mypc"))) == 403);
    s.stop();
}

// A sign-in callback that is slow for the credential "slow" and can be held shut by a test.
struct SlowAuth {
    std::atomic<int> calls{0};
    std::atomic<bool> hold{false};
    std::atomic<std::thread::id> where{};
    ApiAuth operator()(const std::string& a, const std::string&) {
        ++calls;
        where.store(std::this_thread::get_id());
        if (a.find("slow") != std::string::npos) { std::this_thread::sleep_for(std::chrono::milliseconds(700)); }
        while (hold.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
        return ApiAuth::Allowed;
    }
};

void testAuthIsOffTheIoThread() {
    ControlHub hub;
    HubStatus st;
    st.running = true;
    hub.publishStatus(st);
    std::string err;
    SlowAuth auth;
    ApiConfig c = cfg0();
    c.maxPendingAuth = 2;
    ApiServer s(c, hub, [&](const std::string& a, const std::string& p) { return auth(a, p); });
    CHECK(s.start(err));
    const std::uint16_t port = static_cast<std::uint16_t>(s.port());
    // an open stream first (fast sign-in): its periodic status push must not stop while another client's sign-in is slow
    Client ws;
    CHECK(ws.connect(port));
    const std::string wsReq = "GET /ws HTTP/1.1\r\nHost: 127.0.0.1\r\n" + std::string(kWsKey) + "Authorization: Basic fast\r\n\r\n";
    ws.sendBytes(wsReq.data(), wsReq.size());
    CHECK(waitHas(ws, "101"));
    // a slow sign-in begins ...
    Client slow;
    CHECK(slow.connect(port));
    const std::string slowReq = get("/status", "127.0.0.1", "Authorization: Basic slow\r\n");
    slow.sendBytes(slowReq.data(), slowReq.size());
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    // ... and meanwhile the open stream still gets its pushes and a second connection is answered quickly (the worker is busy, so it queues: but the IO thread is free to keep
    // answering what needs no sign-in, and to push)
    const std::size_t before = ws.got.size();
    const auto t0 = nettest::Clock::now();
    ws.pump(450);
    CHECK(ws.got.size() > before);                     // a status frame arrived while the sign-in was in progress
    CHECK(msSince(t0) < 1500);
    // a request that needs no sign-in (a bad request line) is answered at once, while the slow sign-in is still running
    const auto t1 = nettest::Clock::now();
    const std::string bad = ask(port, "GET nonsense\r\n\r\n", 1500);
    CHECK(statusOf(bad) == 400 && msSince(t1) < 500);
    // the slow client gets its answer in the end
    slow.waitEof(4000);
    CHECK(statusOf(std::string(slow.got.begin(), slow.got.end())) == 200);
    CHECK(auth.calls.load() >= 2);
    // the sign-in did not run on the thread that serves the sockets: the test thread is not it either, and it is one thread for all calls
    CHECK(auth.where.load() != std::this_thread::get_id());
    ws.close();
    s.stop();
}

void testAuthFloodAndStop() {
    ControlHub hub;
    std::string err;
    SlowAuth auth;
    auth.hold.store(true);
    ApiConfig c = cfg0();
    c.maxPendingAuth = 2;
    c.maxPerAddressHead = 0;
    c.maxConnections = 16;
    c.headMs = 20000;
    ApiServer s(c, hub, [&](const std::string& a, const std::string& p) { return auth(a, p); });
    CHECK(s.start(err));
    const std::uint16_t port = static_cast<std::uint16_t>(s.port());
    std::vector<std::unique_ptr<Client>> cs;
    for (int i = 0; i < 5; ++i) {
        cs.push_back(std::make_unique<Client>());
        CHECK(cs.back()->connect(port));
        const std::string req = get("/status", "127.0.0.1", "Authorization: Basic w" + std::to_string(i) + "\r\n");
        cs.back()->sendBytes(req.data(), req.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }
    // the worker holds one, two wait: the queue is two deep, so at least two of the five are refused 429 at once (and the IO thread answered them while the worker was stuck)
    int refused = 0;
    for (auto& cl : cs) {
        cl->pump(100);
        if (has(*cl, "429")) { ++refused; }
    }
    CHECK(refused >= 2);
    CHECK(s.stats().authDenied >= 2);
    // stop() while a sign-in is stuck must not hang: release the callback a moment after stop begins (the worker joins when it comes back)
    std::thread release([&] { std::this_thread::sleep_for(std::chrono::milliseconds(200)); auth.hold.store(false); });
    const auto t0 = nettest::Clock::now();
    s.stop();
    CHECK(msSince(t0) < 3000);
    release.join();
}

void testPerAddressHeadCap() {
    ControlHub hub;
    std::string err;
    ApiConfig c = cfg0();
    c.maxConnections = 16;
    c.maxPerAddressHead = 4;
    ApiServer s(c, hub, nullptr);
    CHECK(s.start(err));
    const std::uint16_t port = static_cast<std::uint16_t>(s.port());
    std::vector<std::unique_ptr<Client>> idle;
    for (int i = 0; i < 4; ++i) {
        idle.push_back(std::make_unique<Client>());
        CHECK(idle.back()->connect(port));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // a fifth that never finishes its request from the same address is closed at once; so is any request from it while the four hold their places
    Client fifth;
    CHECK(fifth.connect(port));
    CHECK(fifth.waitEof(1500));
    CHECK(s.stats().refusedFull >= 1);
    // when one of the four goes, the place is free
    idle.front()->close();
    idle.erase(idle.begin());
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(statusOf(ask(port, get("/status", "127.0.0.1"))) == 200);
    // a request that has finished its head (a stream) no longer counts against the cap
    Client ws;
    CHECK(ws.connect(port));
    const std::string wsReq = "GET /ws HTTP/1.1\r\nHost: 127.0.0.1\r\n" + std::string(kWsKey) + "\r\n";
    ws.sendBytes(wsReq.data(), wsReq.size());
    CHECK(waitHas(ws, "101"));
    s.stop();
}

void testCloseAllClients() {
    ControlHub hub;
    std::string err;
    ApiServer s(cfg0(), hub, nullptr);
    CHECK(s.start(err));
    const std::uint16_t port = static_cast<std::uint16_t>(s.port());
    Client ws, sse;
    CHECK(ws.connect(port) && sse.connect(port));
    const std::string wsReq = "GET /ws HTTP/1.1\r\nHost: 127.0.0.1\r\n" + std::string(kWsKey) + "\r\n";
    const std::string sseReq = get("/events", "127.0.0.1");
    ws.sendBytes(wsReq.data(), wsReq.size());
    sse.sendBytes(sseReq.data(), sseReq.size());
    CHECK(waitHas(ws, "101") && waitHas(sse, "text/event-stream"));
    s.closeAllClients();
    CHECK(ws.waitEof(2000) && sse.waitEof(2000));
    CHECK(s.running());
    CHECK(statusOf(ask(port, get("/status", "127.0.0.1"))) == 200);   // and a client that comes back is served afresh
    s.stop();
}

}  // namespace

int main() {
    testHost();
    testHostNamesOfThePage();
    testAuthIsOffTheIoThread();
    testAuthFloodAndStop();
    testPerAddressHeadCap();
    testCloseAllClients();
    return testSummary("test_s4b_sec_api");
}
