// test_s4b_sec_servers.cpp - security lane S4b-sec, finding R8 (the credential cache and the sign-in thread) through the engine's ServerSet (engine/s4b_servers.hpp) with a stand-in web account:
//   A  a login the API remembered ends at once when the web page's password changes: the old password is refused on the very next request, the new one is let in; a stream (WebSocket, SSE)
//      opened under the old password is closed on the next control passes
//   B  the API is open beyond this computer only with the web page's account: when the password is REMOVED while it is on, nobody is let in (the stored login is not honoured, the streams close)
//   C  the sign-in worker never waits on the lock a restart of the API holds: requests and restarts at the same time do not deadlock (a watchdog turns a hang into a failure)
//
// The API binds 127.0.0.1 in every case (Config::apiBindForTest) so that "beyond this computer" can be proved without a firewall prompt. Nothing leaves this computer.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>

#include "engine/s4b_servers.hpp"
#include "net/web_auth.hpp"
#include "net/ws_frame.hpp"
#include "net_test_support.hpp"
#include "test_check.hpp"

using namespace cascade::engine::s4b;
namespace net = cascade::net;
using nettest::Client;
using nettest::msSince;

namespace {

struct Account {
    std::mutex m;
    std::string user = "admin", record;
    Hooks::WebAccount get() {
        std::lock_guard<std::mutex> lk(m);
        return {user, record};
    }
    void set(const std::string& u, const std::string& r) {
        std::lock_guard<std::mutex> lk(m);
        user = u;
        record = r;
    }
};

std::string recordFor(const std::string& password) {
    net::PasswordRecord rec;
    std::string err;
    if (!net::hashPassword(password, rec, err)) { return std::string(); }
    return rec.serialize();
}

std::string basic(const std::string& user, const std::string& pass) { return "Authorization: Basic " + net::ws::base64Encode(user + ":" + pass) + "\r\n"; }

std::string statusReq(const std::string& extra) { return "GET /status HTTP/1.1\r\nHost: 127.0.0.1\r\n" + extra + "\r\n"; }

int ask(int port, const std::string& req) {
    Client c;
    if (!c.connect(static_cast<std::uint16_t>(port))) { return -1; }
    c.sendBytes(req.data(), req.size());
    c.waitEof(6000);
    const std::string all(c.got.begin(), c.got.end());
    return all.size() > 12 ? std::atoi(all.substr(9, 3).c_str()) : 0;
}

bool has(Client& c, const std::string& s) { return std::string(c.got.begin(), c.got.end()).find(s) != std::string::npos; }

bool waitHas(Client& c, const std::string& s, int ms = 6000) {
    const auto t0 = nettest::Clock::now();
    while (msSince(t0) < ms) {
        if (has(c, s)) { return true; }
        if (c.eof) { return false; }
        c.pump(30);
    }
    return has(c, s);
}

// Runs the control pass until the client's connection ends (or `ms` pass).
bool endsWithPasses(ServerSet& set, Client& c, int ms = 4000) {
    const auto t0 = nettest::Clock::now();
    while (msSince(t0) < ms) {
        set.pass();
        c.pump(30);
        if (c.eof) { return true; }
    }
    return c.eof;
}

int portOf(const ServerSet& set) {
    int port = 0;
    set.facts([&](const std::string& k, const std::string& v) { if (k == "srv.api.port" && !v.empty()) { port = std::atoi(v.c_str()); } });
    return port;
}

const char* kWsKey = "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n";

Config apiConfig() {
    Config c;
    c.api = true;
    c.apiAll = true;                   // as if "every address" were ticked: every request must sign in ...
    c.apiPort = 0;
    c.apiBindForTest = "127.0.0.1";    // ... but nothing is exposed beyond this computer by this test
    return c;
}

}  // namespace

int main() {
    // watchdog: a deadlock in the code under test must fail the run, not hang it
    std::atomic<bool> done{false};
    std::thread dog([&] {
        for (int i = 0; i < 1200 && !done.load(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
        if (!done.load()) { std::printf("FAIL watchdog: the test did not finish in 60 s (a deadlock)\n"); std::fflush(stdout); std::_Exit(3); }
    });

    const std::string r1 = recordFor("first password here"), r2 = recordFor("second password here");
    CHECK(!r1.empty() && !r2.empty() && r1 != r2);
    Account acct;
    acct.set("admin", r1);
    Hooks h;
    h.webAccount = [&] { return acct.get(); };
    ServerSet set;
    set.setHooks(h);
    set.configure(apiConfig());
    const int port = portOf(set);
    CHECK(port > 0);

    // ---- A: the password changes
    CHECK(ask(port, statusReq("")) == 401);                                               // beyond this computer: no credentials, no entry
    CHECK(ask(port, statusReq(basic("admin", "first password here"))) == 200);            // the good login is remembered ...
    CHECK(ask(port, statusReq(basic("admin", "first password here"))) == 200);            // ... (and served from the memory the second time)
    Client ws, sse;
    CHECK(ws.connect(static_cast<std::uint16_t>(port)) && sse.connect(static_cast<std::uint16_t>(port)));
    const std::string wsReq = "GET /ws HTTP/1.1\r\nHost: 127.0.0.1\r\n" + std::string(kWsKey) + basic("admin", "first password here") + "\r\n";
    const std::string sseReq = "GET /events HTTP/1.1\r\nHost: 127.0.0.1\r\n" + basic("admin", "first password here") + "\r\n";
    ws.sendBytes(wsReq.data(), wsReq.size());
    sse.sendBytes(sseReq.data(), sseReq.size());
    CHECK(waitHas(ws, "101") && waitHas(sse, "text/event-stream"));
    set.pass();                                                                           // (the pass notes the account as it is)
    acct.set("admin", r2);                                                                // the person changes the web page's password
    CHECK(ask(port, statusReq(basic("admin", "first password here"))) == 401);            // AT ONCE: the remembered login of the old password is not honoured
    CHECK(ask(port, statusReq(basic("admin", "second password here"))) == 200);           // the new one is
    CHECK(endsWithPasses(set, ws) && endsWithPasses(set, sse));                           // the streams opened under the old password end
    CHECK(set.active());
    Client ws2;
    CHECK(ws2.connect(static_cast<std::uint16_t>(port)));
    const std::string ws2Req = "GET /ws HTTP/1.1\r\nHost: 127.0.0.1\r\n" + std::string(kWsKey) + basic("admin", "second password here") + "\r\n";
    ws2.sendBytes(ws2Req.data(), ws2Req.size());
    CHECK(waitHas(ws2, "101"));                                                           // a stream under the new password is kept
    for (int i = 0; i < 12; ++i) { set.pass(); ws2.pump(60); }
    CHECK(!ws2.eof);
    // a changed user name ends the logins as well
    acct.set("owner", r2);
    CHECK(ask(port, statusReq(basic("admin", "second password here"))) == 401);
    CHECK(ask(port, statusReq(basic("owner", "second password here"))) == 200);
    CHECK(endsWithPasses(set, ws2));
    acct.set("admin", r2);

    // ---- B: the password is removed while the API listens beyond this computer
    Client ws3;
    CHECK(ws3.connect(static_cast<std::uint16_t>(port)));
    const std::string ws3Req = "GET /ws HTTP/1.1\r\nHost: 127.0.0.1\r\n" + std::string(kWsKey) + basic("admin", "second password here") + "\r\n";
    CHECK(ask(port, statusReq(basic("admin", "second password here"))) == 200);           // (remembered)
    ws3.sendBytes(ws3Req.data(), ws3Req.size());
    CHECK(waitHas(ws3, "101"));
    set.pass();
    acct.set("admin", std::string());                                                     // no password at all now
    CHECK(ask(port, statusReq(basic("admin", "second password here"))) == 401);           // the stored login is not honoured
    CHECK(ask(port, statusReq(basic("anyone", "anything"))) == 401);                      // and "no password" is not "open" beyond this computer
    CHECK(ask(port, statusReq("")) == 401);
    CHECK(endsWithPasses(set, ws3));
    set.configure(Config());                                                              // everything off

    // ---- this computer only: no password means the interface is open to the local programs, as before (a rule of the design: the Host check guards it)
    Config local;
    local.api = true;
    local.apiPort = 0;
    set.configure(local);
    const int lport = portOf(set);
    CHECK(lport > 0 && ask(lport, statusReq("")) == 200);
    set.configure(Config());

    // ---- C: a sign-in never waits for the lock a settings change holds. configure() holds the set's lock while it calls the engine's hooks: here the audio hook is held shut, so the lock is held
    // for as long as the test wants; a sign-in made meanwhile must still be answered (the API's worker reads the account under its own lock). With the old design (the IO thread asked for the set's
    // lock) it is not, and stop() of the API waiting on that thread would deadlock the whole set.
    acct.set("admin", r1);
    Config cfg = apiConfig();
    set.configure(cfg);
    const int cport = portOf(set);
    CHECK(cport > 0);
    std::atomic<bool> inHook{false}, gate{false};
    Hooks h2 = h;
    h2.audioReader = [&](int) {
        inHook.store(true);
        const auto t0 = nettest::Clock::now();
        while (!gate.load() && msSince(t0) < 20000) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
        return cascade::net::UdpAudioSender::Reader();
    };
    set.setHooks(h2);
    Config withAudio = cfg;
    withAudio.tcpAudio = true;
    withAudio.tcpAudioPort = 0;
    std::thread changing([&] { set.configure(withAudio); });
    for (int i = 0; i < 400 && !inHook.load(); ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    CHECK(inHook.load());                                               // the set's lock is held now
    const auto t0 = nettest::Clock::now();
    CHECK(ask(cport, statusReq("Authorization: Basic QQ==\r\n")) == 401);   // answered while it is held
    CHECK(ask(cport, statusReq("")) == 401);
    CHECK(msSince(t0) < 3000);
    gate.store(true);
    changing.join();
    // and a change that stops the API while sign-ins are arriving returns
    for (int i = 0; i < 6; ++i) {
        Client c;
        if (c.connect(static_cast<std::uint16_t>(cport))) { const std::string req = statusReq("Authorization: Basic QQ==\r\n"); c.sendBytes(req.data(), req.size()); }
        Config off = cfg;
        off.api = (i % 2) == 1;
        set.configure(off);
    }
    set.stop();

    done.store(true);
    dog.join();
    return testSummary("test_s4b_sec_servers");
}
