// test_engine_api_s4b_servers.cpp - screen lane S4b's servers through the REAL engine: the wirec settings op, the engine's pass starting and stopping them, the facts they publish.
// Every server binds 127.0.0.1 only (a wider bind raises the Windows firewall prompt, so no test turns "every address" on: the refusal of 0.0.0.0 without that key is proved in
// the model's own test). No radio is opened, nothing leaves this computer.
//
//   T1  everything off: no server runs, the facts say so
//   T2  audio over TCP: off by default; turned on it listens on this computer only and says so, a client can connect, a bad port or radio is refused and changes nothing,
//       turned off again the port is released; the setting is back after a restart
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <thread>

#include "net_test_support.hpp"
#include "wire_c_rig.hpp"

using namespace wirec_test;

namespace {

template <class F>
bool until(Rig& r, F f, int tries = 80) {
    for (int i = 0; i < tries; ++i) {
        r.pass(2);
        if (f()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return f();
}

void testOff(const fs::path& dir) {
    Rig rig(dir);
    rig.pass(4);
    CHECK(rig.fact("srv.tcpaudio.running") == "0" && rig.fact("srv.tcpaudio.port").empty() && rig.fact("srv.tcpaudio.scope").empty());
    CHECK(rig.fact("wirec.srv.tcpaudio.enabled") == "0" && rig.fact("wirec.srv.tcpaudio.all") == "0");
    CHECK(rig.fact("wirec.srv.tcpaudio.port") == "7356" && rig.fact("wirec.srv.tcpaudio.radio") == "1" && rig.fact("wirec.srv.tcpaudio.packet") == "480");
}

void testTcpAudio(const fs::path& dir) {
    int port = 0;
    {
        Rig rig(dir);
        // refused, and nothing changes
        CHECK(rig.status(setting("srv.tcpaudio.port", "80")) != FOXAPI_OK);
        CHECK(rig.status(setting("srv.tcpaudio.port", "70000")) != FOXAPI_OK);
        CHECK(rig.status(setting("srv.tcpaudio.radio", "9")) != FOXAPI_OK);
        CHECK(rig.status(setting("srv.tcpaudio.packet", "100")) != FOXAPI_OK);
        CHECK(rig.fact("wirec.srv.tcpaudio.port") == "7356" && rig.fact("wirec.srv.tcpaudio.radio") == "1");
        // an OS-chosen port is not a setting: use a free one found here
        {
            std::string err;
            std::uint32_t ip = 0;
            cascade::net::sock::parseIpv4("127.0.0.1", ip);
            const cascade::net::sock::Handle h = cascade::net::sock::listenTcp(ip, 0, 1, err);
            CHECK(h != cascade::net::sock::kInvalid);
            port = cascade::net::sock::boundPort(h);
            cascade::net::sock::closeHandle(h);
        }
        CHECK(port >= 1024);
        CHECK(rig.status(setting("srv.tcpaudio.port", std::to_string(port))) == FOXAPI_OK);
        rig.pass(4);
        CHECK(rig.fact("srv.tcpaudio.running") == "0");   // a port alone starts nothing
        CHECK(rig.status(setting("srv.tcpaudio.enabled", "1")) == FOXAPI_OK);
        CHECK(until(rig, [&] { return rig.fact("srv.tcpaudio.running") == "1"; }));
        CHECK(rig.fact("srv.tcpaudio.port") == std::to_string(port));
        CHECK(rig.fact("srv.tcpaudio.scope") == "this computer only");
        CHECK(rig.fact("srv.tcpaudio.problem").empty());
        // a program connects (the counters move)
        {
            nettest::Client c;
            CHECK(c.connect(static_cast<std::uint16_t>(port)));
            CHECK(until(rig, [&] { return rig.fact("srv.tcpaudio.accepted") == "1"; }));
            CHECK(until(rig, [&] { return rig.fact("srv.tcpaudio.clients") == "1"; }));
        }
        // the same port is held: a second engine-side start on it would say why; here: turning it off releases it
        CHECK(rig.status(setting("srv.tcpaudio.enabled", "0")) == FOXAPI_OK);
        CHECK(until(rig, [&] { return rig.fact("srv.tcpaudio.running") == "0"; }));
        nettest::Client gone;
        CHECK(!gone.connect(static_cast<std::uint16_t>(port), 300));   // nobody is listening any more
        CHECK(rig.fact("srv.tcpaudio.scope").empty());
        // a port something else holds: the sentence says so and nothing runs
        std::string err;
        std::uint32_t ip = 0;
        cascade::net::sock::parseIpv4("127.0.0.1", ip);
        const cascade::net::sock::Handle holder = cascade::net::sock::listenTcp(ip, static_cast<std::uint16_t>(port), 1, err);
        CHECK(holder != cascade::net::sock::kInvalid);
        CHECK(rig.status(setting("srv.tcpaudio.enabled", "1")) == FOXAPI_OK);
        CHECK(until(rig, [&] { return !rig.fact("srv.tcpaudio.problem").empty(); }));
        CHECK(rig.fact("srv.tcpaudio.running") == "0");
        cascade::net::sock::closeHandle(holder);
        CHECK(rig.status(setting("srv.tcpaudio.enabled", "0")) == FOXAPI_OK);
        CHECK(until(rig, [&] { return rig.fact("srv.tcpaudio.problem").empty(); }));
        CHECK(rig.status(setting("srv.tcpaudio.enabled", "1")) == FOXAPI_OK);   // saved on: the next engine starts it
    }
    {
        Rig again(dir);   // a restart: the choice is back and the server is up again
        CHECK(again.fact("wirec.srv.tcpaudio.enabled") == "1" && again.fact("wirec.srv.tcpaudio.port") == std::to_string(port));
        CHECK(until(again, [&] { return again.fact("srv.tcpaudio.running") == "1"; }));
    }
}

}  // namespace

int main() {
    engine_api_test::hermetic("s4b_servers");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const fs::path root = scratch("s4bsrv");
    int n = 0;
    for (auto fn : {testOff, testTcpAudio}) {
        const fs::path d = root / std::to_string(n++);
        std::error_code ec;
        fs::create_directories(d, ec);
        fn(d);
    }
    std::error_code ec;
    fs::remove_all(root, ec);
    return check::finish("test_engine_api_s4b_servers");
}
