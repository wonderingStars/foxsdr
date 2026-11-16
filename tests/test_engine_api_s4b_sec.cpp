// test_engine_api_s4b_sec.cpp - security lane S4b-sec, finding R7 through the REAL engine: the two servers that have no password (rtl_tcp, audio over TCP) take a list of client addresses as a
// setting, and "listen on every address" without a usable list does not start them. No socket is made by any case here (a refusal comes before it, so no firewall prompt); nothing leaves this
// computer and no radio is opened.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <thread>

#include "wire_c_rig.hpp"

using namespace wirec_test;

namespace {

template <class F>
bool until(Rig& r, F f, int tries = 60) {
    for (int i = 0; i < tries; ++i) {
        r.pass(2);
        if (f()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    return f();
}

void testSettings(const fs::path& dir) {
    Rig rig(dir);
    rig.pass(4);
    // the lists are empty by default
    CHECK(rig.fact("wirec.srv.tcpaudio.allow").empty() && rig.fact("wirec.srv.rtltcp.allow").empty());
    // a list that is not one is refused, and nothing changes
    for (const char* key : {"srv.tcpaudio.allow", "srv.rtltcp.allow"}) {
        for (const char* bad : {"everyone", "0.0.0.0/0", "10.0.0.0/7", "224.0.0.1", "192.168.1.20, junk", "192.168.1.300"}) { CHECK(rig.status(setting(key, bad)) != FOXAPI_OK); }
        CHECK(rig.fact(std::string("wirec.") + key).empty());
        CHECK(rig.status(setting(key, "192.168.1.20, 10.0.0.0/8")) == FOXAPI_OK);
        CHECK(rig.fact(std::string("wirec.") + key) == "192.168.1.20, 10.0.0.0/8");
        CHECK(rig.status(setting(key, "")) == FOXAPI_OK);   // and it can be emptied
        CHECK(rig.fact(std::string("wirec.") + key).empty());
    }
}

void testRefusedBeyondThisComputer(const fs::path& dir) {
    Rig rig(dir);
    rig.pass(4);
    // every address, no list: the servers do not start and say what to do; no socket exists
    CHECK(rig.status(setting("srv.rtltcp.all", "1")) == FOXAPI_OK);
    CHECK(rig.status(setting("srv.rtltcp.enabled", "1")) == FOXAPI_OK);
    CHECK(until(rig, [&] { return !rig.fact("srv.rtltcp.problem").empty(); }));
    CHECK(rig.fact("srv.rtltcp.running") == "0" && rig.fact("srv.rtltcp.problem").find("list the computers that may") != std::string::npos && rig.fact("srv.rtltcp.port").empty());
    CHECK(rig.status(setting("srv.tcpaudio.all", "1")) == FOXAPI_OK);
    CHECK(rig.status(setting("srv.tcpaudio.enabled", "1")) == FOXAPI_OK);
    CHECK(until(rig, [&] { return rig.fact("srv.tcpaudio.problem").find("list the computers that may") != std::string::npos; }));
    CHECK(rig.fact("srv.tcpaudio.running") == "0" && rig.fact("srv.tcpaudio.port").empty());
    // turned back to this computer only, the same servers start without a list
    CHECK(rig.status(setting("srv.rtltcp.all", "0")) == FOXAPI_OK);
    CHECK(until(rig, [&] { return rig.fact("srv.rtltcp.running") == "1"; }));
    CHECK(rig.fact("srv.rtltcp.scope") == "this computer only" && rig.fact("srv.rtltcp.problem").find("list the computers") == std::string::npos);
    CHECK(rig.status(setting("srv.rtltcp.enabled", "0")) == FOXAPI_OK);
    CHECK(until(rig, [&] { return rig.fact("srv.rtltcp.running") == "0"; }));
}

}  // namespace

int main() {
    engine_api_test::hermetic("s4b_sec");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const fs::path root = scratch("s4bsec");
    int n = 0;
    for (auto fn : {testSettings, testRefusedBeyondThisComputer}) {
        const fs::path d = root / std::to_string(n++);
        std::error_code ec;
        fs::create_directories(d, ec);
        fn(d);
    }
    std::error_code ec;
    fs::remove_all(root, ec);
    return check::finish("test_engine_api_s4b_sec");
}
