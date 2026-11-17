// test_s4b_sec_servers.cpp - security lane S4b-sec, finding R8 (the credential cache and the sign-in thread) through the engine's ServerSet (engine/s4b_servers.hpp) with a stand-in web account:
//   A  a login the API remembered ends at once when the web page's password changes: the old password is refused on the very next request, the new one is let in; a stream (WebSocket, SSE)
//      opened under the old password is closed on the next control passes
//   B  the API is open beyond this computer only with the web page's account: when the password is REMOVED while it is on, nobody is let in (the stored login is not honoured, the streams close)
//   D  beyond this computer the rtl_tcp server and the TCP audio server start only with a list of client addresses the person typed (finding R7); with none, or a bad one, they do not start and
//      say why, before any socket is made
//   E  what a network client can make the servers do, over every command each accepts: rtl_tcp (all 256 command bytes) reaches the engine's tuning for the frequency command alone; the API's
//      operations are the five and no others (a long list of operations that would transmit, key, read or write a file, change a setting or run a program is refused), and a tune carries nothing but its frequency
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
#include <vector>

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

std::string wsFrame(const std::string& payload) {
    std::string f;
    f.push_back(static_cast<char>(0x81));
    f.push_back(static_cast<char>(0x80 | payload.size()));   // (every message here is under 126 bytes)
    const std::uint8_t key[4] = {1, 2, 3, 4};
    f.append(reinterpret_cast<const char*>(key), 4);
    for (std::size_t i = 0; i < payload.size(); ++i) { f.push_back(static_cast<char>(static_cast<std::uint8_t>(payload[i]) ^ key[i % 4])); }
    return f;
}

std::string cmdBytes(std::uint8_t c, std::uint32_t p) {
    std::string r;
    r.push_back(static_cast<char>(c));
    r.push_back(static_cast<char>(p >> 24));
    r.push_back(static_cast<char>(p >> 16));
    r.push_back(static_cast<char>(p >> 8));
    r.push_back(static_cast<char>(p));
    return r;
}

std::string factOf(const ServerSet& set, const std::string& key) {
    std::string out;
    set.facts([&](const std::string& k, const std::string& v) { if (k == key) { out = v; } });
    return out;
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

    // ---- D: beyond this computer the two servers without a password need a list of client addresses
    {
        ServerSet two;
        Hooks th;
        th.audioReader = [](int) { return cascade::net::UdpAudioSender::Reader([](std::uint64_t&, float*, std::size_t) { return 0; }); };
        th.attachIq = [](int, cascade::core::Pipeline::IqSink*) { return std::string(); };
        th.radioIdentity = [](int) { return std::uintptr_t(1); };
        two.setHooks(th);
        for (const char* list : {"", "   ", "everyone", "0.0.0.0/0", "10.0.0.0/7", "192.168.1.20, junk"}) {
            Config c;
            c.rtl = true;
            c.rtlAll = true;
            c.rtlAllow = list;
            c.rtlPort = 0;
            c.tcpAudio = true;
            c.tcpAudioAll = true;
            c.tcpAudioAllow = list;
            c.tcpAudioPort = 0;
            two.configure(c);                                  // (no socket is made: a refusal comes before it, so no firewall prompt)
            CHECK(factOf(two, "srv.rtltcp.running") == "0" && factOf(two, "srv.rtltcp.problem").find("list the computers that may") != std::string::npos);
            CHECK(factOf(two, "srv.tcpaudio.running") == "0" && factOf(two, "srv.tcpaudio.problem").find("list the computers that may") != std::string::npos);
            CHECK(!two.active());
        }
        // this computer only: no list is needed, and a list that is there is not looked at
        Config c;
        c.rtl = true;
        c.rtlPort = 0;
        c.rtlAllow = "garbage";
        two.configure(c);
        CHECK(factOf(two, "srv.rtltcp.running") == "1" && factOf(two, "srv.rtltcp.problem").empty() && factOf(two, "srv.rtltcp.scope") == "this computer only");
        two.stop();
    }

    // ---- E: what a client can make the servers do
    {
        std::atomic<int> tunes{0}, modes{0};
        std::mutex tm;
        std::vector<double> tuned;
        std::vector<std::string> modeSeen;
        ServerSet svc;
        Hooks eh;
        eh.attachIq = [](int, cascade::core::Pipeline::IqSink*) { return std::string(); };
        eh.radioIdentity = [](int) { return std::uintptr_t(1); };
        eh.tune = [&](int, double hz) { std::lock_guard<std::mutex> lk(tm); tuned.push_back(hz); ++tunes; return std::string(); };
        eh.setMode = [&](int, const std::string& m) { std::lock_guard<std::mutex> lk(tm); modeSeen.push_back(m); ++modes; return std::string(); };
        eh.readStatus = [](int, cascade::net::HubStatus& st) { st.running = true; return true; };
        svc.setHooks(eh);
        Config c;
        c.rtl = true;
        c.rtlPort = 0;
        c.api = true;
        c.apiPort = 0;
        svc.configure(c);
        const int rport = std::atoi(factOf(svc, "srv.rtltcp.port").c_str());
        const int aport = portOf(svc);
        CHECK(rport > 0 && aport > 0);
        // rtl_tcp: an honest retune first, then every one of the 256 command bytes with a parameter that is a valid frequency (999 MHz): only the frequency command may tune, and only to what it carried
        Client rc;
        CHECK(rc.connect(static_cast<std::uint16_t>(rport)));
        for (int i = 0; i < 100 && factOf(svc, "srv.rtltcp.clients") != "1"; ++i) { svc.pass(); rc.pump(20); }
        std::string all = cmdBytes(0x01, 145000000u);
        for (int b = 0; b < 256; ++b) { all += cmdBytes(static_cast<std::uint8_t>(b), b == 1 ? 145000000u : 999000000u); }
        rc.sendBytes(all.data(), all.size());
        for (int i = 0; i < 150 && std::atoi(factOf(svc, "srv.rtltcp.ignored").c_str()) < 255 - 1; ++i) { svc.pass(); rc.pump(20); }
        for (int i = 0; i < 20; ++i) { svc.pass(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        {
            std::lock_guard<std::mutex> lk(tm);
            CHECK(!tuned.empty());
            for (const double hz : tuned) { CHECK(hz == 145000000.0); }   // never 999 MHz: no other command number tunes
            CHECK(modeSeen.empty());                                        // rtl_tcp never sets a mode, a gain, a rate or anything else the engine has
        }
        CHECK(std::atoi(factOf(svc, "srv.rtltcp.ignored").c_str()) >= 255 - 1);   // 0x02..0x0E counted and ignored, 0x0F..0xFF and 0x00 unknown, all ignored
        rc.close();
        // the API: operations that would transmit, key, read or write a file, change a setting or run a program are not operations
        Client ws;
        CHECK(ws.connect(static_cast<std::uint16_t>(aport)));
        const std::string req = "GET /ws HTTP/1.1\r\nHost: 127.0.0.1\r\n" + std::string(kWsKey) + "\r\n";
        ws.sendBytes(req.data(), req.size());
        CHECK(waitHas(ws, "101"));
        const int before = tunes.load();
        const char* evil[] = {"transmit", "tx", "ptt", "key", "unkey", "set_frequency", "set_gain", "set_rate", "set_sample_rate", "record", "start_recording", "open", "open_file", "read_file", "write_file",
                              "delete", "set_setting", "setting", "config", "start", "stop", "run", "exec", "shell", "system", "connect", "install_plugin", "load_plugin", "add_bookmark", "export", "import", "login",
                              "set_password", "restart", "quit", "TUNE ", "Tune", "tune\n", ""};
        int refused = 0;
        for (const char* op : evil) {
            const std::string f = wsFrame(std::string("{\"op\":\"") + op + "\",\"hz\":999000000,\"path\":\"C:/x\",\"cmd\":\"calc\"}");
            if (f.size() > 130) { continue; }
            ws.sendBytes(f.data(), f.size());
            ++refused;
        }
        for (int i = 0; i < 100 && !has(ws, "unknown operation"); ++i) { svc.pass(); ws.pump(30); }
        for (int i = 0; i < 40; ++i) { svc.pass(); ws.pump(20); }
        CHECK(refused > 30);
        CHECK(tunes.load() == before && modes.load() == 0);               // not one of them reached the engine
        // the one that is an operation carries nothing but its frequency, whatever else the message holds
        const std::string t = wsFrame("{\"op\":\"tune\",\"hz\":146000000,\"path\":\"C:/x\",\"cmd\":\"calc\"}");
        ws.sendBytes(t.data(), t.size());
        for (int i = 0; i < 100 && tunes.load() == before; ++i) { svc.pass(); ws.pump(30); }
        CHECK(tunes.load() == before + 1);
        {
            std::lock_guard<std::mutex> lk(tm);
            CHECK(tuned.back() == 146000000.0);
        }
        ws.close();
        svc.stop();
    }

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
