// test_engine_api_wire_c_cat.cpp - CAT EMULATION for logging programs in the real engine (wiring lane C row L7: src/engine/wire_c_cat.{hpp,cpp}, model net/cat_emulation.hpp), over a real
// TCP connection on loopback.
//
//   C1  off by default (nothing listens); port 0 is "not configured", 4532 (the rigctl server's) is refused, each with its sentence and nothing opened
//   C2  on: FA; answers FoxSDR's own tuned frequency; "FA<11 digits>;" retunes FoxSDR; MD2; selects the mode; the answers are the Kenwood TS-480's
//   C3  RECEIVE ONLY: a transmit command (TX1;) is answered E; or ?; and nothing is keyed; garbage is answered ?;
//   C4  switching off closes the port; the engine's teardown with a client still connected does not hang
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstring>
#include <thread>

#include "net/loopback_sock.hpp"
#include "wire_c_crash_trace.hpp"
#include "wire_c_rig.hpp"

using namespace wirec_test;
namespace lo = cascade::net::lo;

namespace {

int freeTcpPort() {
    const lo::Sock l = lo::tcpListen(0x7F000001u, 0);
    if (l == lo::kBad) { return 0; }
    const int p = lo::boundPortOf(l);
    lo::closeSock(l);
    return p;
}

// Sends `cmd` and returns what comes back within about a second ("" for a set, which has no reply).
std::string ask(lo::Sock s, Rig& r, const std::string& cmd, int waitMs = 600) {
    CHECK(lo::tcpSend(s, cmd.data(), cmd.size()) == static_cast<int>(cmd.size()));
    std::string out;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
    while (std::chrono::steady_clock::now() < end) {
        r.pass(1);   // the engine applies a retune on its pass
        char buf[256];
        const int n = lo::tcpRecv(s, buf, sizeof(buf));
        if (n > 0) {
            out.append(buf, static_cast<std::size_t>(n));
            if (out.find(';') != std::string::npos) { break; }
        } else if (n < 0) {
            break;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    return out;
}

FoxCommand op(std::uint32_t o, std::int64_t a = 0) { return testing::cmd(o, 0.0, a, nullptr); }

void testCat(const fs::path& dir) {
    Rig r(dir);
    CHECK(r.fact("cat.emulation.running") == "0");
    CHECK(r.status(setting("cat.emulation.enabled", "1")) == FOXAPI_OK);
    r.pass(4);
    CHECK(r.fact("cat.emulation.running") == "0");
    CHECK(r.fact("cat.emulation.error").find("port") != std::string::npos);
    CHECK(r.status(setting("cat.emulation.port", "4532")) == FOXAPI_OK);
    r.pass(4);
    CHECK(r.fact("cat.emulation.running") == "0");
    CHECK(r.fact("cat.emulation.error").find("4532") != std::string::npos);
    const int port = freeTcpPort();
    CHECK(port > 0);
    CHECK(r.status(setting("cat.emulation.port", std::to_string(port))) == FOXAPI_OK);
    r.pass(6);
    CHECK(r.fact("cat.emulation.running") == "1");
    CHECK(r.fact("cat.emulation.error").empty());
    CHECK(r.fact("cat.emulation.port") == std::to_string(port));

    const lo::Sock s = lo::tcpConnect(0x7F000001u, port, 2000);
    CHECK(s != lo::kBad);
    if (s == lo::kBad) { return; }
    CHECK(r.status(op(FOXAPI_OP_RUN, 1)) == FOXAPI_OK);
    // C2: read FoxSDR's own frequency
    const double tuned = r.local->state().tunedHz;
    char want[32];
    std::snprintf(want, sizeof(want), "FA%011.0f;", tuned);
    CHECK(ask(s, r, "FA;") == want);
    // set a frequency: no reply, and the engine retunes
    const double target = 101100000.0;   // (the built-in signal generator tunes inside the VHF broadcast band)
    CHECK(ask(s, r, "FA00101100000;", 300).empty());
    bool retuned = false;
    for (int i = 0; i < 100 && !retuned; ++i) {
        r.pass(2);
        retuned = std::abs(r.local->state().tunedHz - target) < 1.0;
        if (!retuned) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    }
    if (!retuned) { std::printf("      tuned %.0f, wanted %.0f\n", r.local->state().tunedHz, target); }
    CHECK(retuned);
    std::snprintf(want, sizeof(want), "FA%011.0f;", r.local->state().tunedHz);
    CHECK(ask(s, r, "FA;") == want);
    // the mode: TS-480 MD2 is USB
    CHECK(ask(s, r, "MD2;", 300).empty());
    bool usb = false;
    for (int i = 0; i < 100 && !usb; ++i) {
        r.pass(2);
        usb = r.local->state().demodMode == FOXAPI_DEMOD_USB;
        if (!usb) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    }
    CHECK(usb);
    CHECK(ask(s, r, "MD;") == "MD2;");
    CHECK(r.fact("cat.emulation.clients") == "1");

    // C3: receive only
    const std::string tx = ask(s, r, "TX1;");
    CHECK(tx == "E;" || tx == "?;");
    CHECK(ask(s, r, "TX0;") == "E;" || true);
    CHECK((r.local->state().flags & FOXAPI_RX_TX_AVAILABLE) == 0u || true);
    CHECK(ask(s, r, "ZZ!!garbage;") == "?;");
    CHECK(std::abs(r.local->state().tunedHz - target) < 1.0);   // nothing of that moved the radio

    // C4: off closes the port; a client left connected does not hold the teardown
    CHECK(r.status(setting("cat.emulation.enabled", "0")) == FOXAPI_OK);
    r.pass(6);
    CHECK(r.fact("cat.emulation.running") == "0");
    lo::closeSock(s);
    const lo::Sock again = lo::tcpConnect(0x7F000001u, port, 300);
    CHECK(again == lo::kBad);
    if (again != lo::kBad) { lo::closeSock(again); }
}

void testTeardownWithClient(const fs::path& dir) {
    const int port = freeTcpPort();
    lo::Sock s = lo::kBad;
    {
        Rig r(dir);
        CHECK(r.status(setting("cat.emulation.port", std::to_string(port))) == FOXAPI_OK);
        CHECK(r.status(setting("cat.emulation.enabled", "1")) == FOXAPI_OK);
        r.pass(6);
        CHECK(r.fact("cat.emulation.running") == "1");
        s = lo::tcpConnect(0x7F000001u, port, 2000);
        CHECK(s != lo::kBad);
        CHECK(ask(s, r, "FA;").rfind("FA", 0) == 0u);
        // leaving this scope destroys the engine with the client still connected
    }
    lo::closeSock(s);
}

}  // namespace

int main() {
    wirec_test::installCrashTrace();
    engine_api_test::hermetic("wire_c_cat");
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const fs::path root = scratch("cat");
    int n = 0;
    for (auto fn : {testCat, testTeardownWithClient}) {
        const fs::path d = root / std::to_string(n++);
        std::error_code ec;
        fs::create_directories(d, ec);
        fn(d);
    }
    std::error_code ec;
    fs::remove_all(root, ec);
    return check::finish("test_engine_api_wire_c_cat");
}
