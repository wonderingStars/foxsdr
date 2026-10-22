// test_cat_engine.cpp - CAT IS AN INTERNAL ENGINE-API SESSION (engine extraction stage 5c; docs/engine-stage4.md 6.11,
// docs/api-0.3-additions.md 8.16.3-8.16.4). The real CAT listener over a real loopback socket, started by FOXAPI_OP_SERVER_CONFIG on a
// headless engine whose pass runs on its own thread: every connection holds a session of its own on the table, reads the receiver through
// its read_state, hands its sets to its submit, and collects their results - and there is no pull for CAT any more, none is installed
// here.
//
// test_cat_server.cpp holds the transport against a stub session (reassembly, the line cap, idle clients, stop()); test_cat_protocol.cpp
// holds the grammar; test_internal_session_order.cpp holds the table's tier (the order, the same-pass release, the flood); this file holds
// the WHOLE LINE from a client's bytes to the engine's state:
//
//   C1  THE WIRE, through the engine: f, F, M, m, t, and T 1 refused (RPRT -11) - the answers a Hamlib client has always had, and the
//       radio actually follows (the pass applied what the session submitted).
//   C2  THE TUNE POLICY (survey 0.6): an F inside the sampled band moves the VFO and NOT the centre (no retune, no gap in the waterfall);
//       an F outside moves the centre and the offset returns to 0. CAT's F is never SET_FREQUENCY ("VFO kept, centre follows" - the
//       hardware would retune on every CAT frequency change, and the offset a client set would stay).
//   C3  THE CAP: a fifth client is refused, and the refusal is BEFORE a session is made for it.
//   C4  THE SESSION COUNT: one internal CAT session per connection - up on accept, back to the baseline on disconnect, and back to the
//       baseline when the listener is stopped with clients connected (stop() returned means no session of the server exists).
//   C5  THE DRAIN: a thousand commands go through with none refused (BUSY at 256 outstanding would show as a command that never lands: the
//       connection collects results after every request and on an idle tick), and a burst far past the cap is all answered on the wire and
//       the connection serves on afterwards.
//   C6  THE PROFILE: a CAT session cannot press the PTT, latch, arm, open or close a transmitter, change a server, fetch from the store or
//       name a file - at the door, and as the result a submit gets; its profile is exactly the four ops F and M send.
//   C7  A ONE-SHOT CLIENT STILL RETUNES: it sends, reads "RPRT 0" and hangs up before the engine's pass has run; the connection waits for
//       its commands to be applied before it closes the session (a closed session has its queued commands refused).
//   C8  THE TEARDOWN with clients still connected: the engine shuts down promptly and nothing hangs.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
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

#include "test_check.hpp"
#include "core/app_commands.hpp"
#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine/server_host.hpp"
#include "engine_api_test_env.hpp"
#include "foxsdr_api.h"

// (test_check.hpp has CHECK only: a comparison that prints what it saw)
template <class T>
std::string showVal(const T& v) {
    if constexpr (std::is_convertible_v<const T&, std::string>) {
        return "\"" + std::string(v) + "\"";
    } else {
        return std::to_string(v);
    }
}
#define CHECK_EQ(a, b)                                                                                   \
    do {                                                                                                 \
        const auto va_ = (a);                                                                            \
        const auto vb_ = (b);                                                                            \
        CHECK(va_ == vb_);                                                                               \
        if (!(va_ == vb_)) {                                                                             \
            std::printf("      %s = %s, %s = %s\n", #a, showVal(va_).c_str(), #b, showVal(vb_).c_str()); \
        }                                                                                                \
    } while (0)

using cascade::engine::Engine;
using cascade::engine::InternalKind;
namespace cmd = cascade::core::cmd;

namespace {

const FoxEngineApi* api() { return cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR); }

constexpr std::uint64_t kAllGrants = FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS | FOXAPI_GRANT_TRANSMIT |
                                     FOXAPI_GRANT_ADMIN | FOXAPI_GRANT_AUDIO | FOXAPI_GRANT_IQ;

bool waitUntil(int ms, const std::function<bool()>& pred) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > end) { return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

// ---- sockets (the shape test_server_host.cpp uses) -----------------------------------------------------------------------------------

void closeSock(SockT s) {
#if defined(_WIN32)
    if (s != INVALID_SOCKET) { ::closesocket(s); }
#else
    if (s != INVALID_SOCKET) { ::close(s); }
#endif
}

void sockInit() {
#if defined(_WIN32)
    static bool done = false;
    if (!done) {
        WSADATA d{};
        ::WSAStartup(MAKEWORD(2, 2), &d);
        done = true;
    }
#endif
}

SockT connectTo(int port, int rcvTimeoutMs = 250) {
    sockInit();
    SockT s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { return INVALID_SOCKET; }
#if defined(_WIN32)
    DWORD tv = static_cast<DWORD>(rcvTimeoutMs);
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    timeval tv{};
    tv.tv_sec = rcvTimeoutMs / 1000;
    tv.tv_usec = (rcvTimeoutMs % 1000) * 1000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(static_cast<unsigned short>(port));
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closeSock(s);
        return INVALID_SOCKET;
    }
    return s;
}

bool sendRaw(SockT s, const std::string& text) {
    std::size_t sent = 0;
    while (sent < text.size()) {
        const int n = static_cast<int>(::send(s, text.data() + sent, static_cast<int>(text.size() - sent), 0));
        if (n <= 0) { return false; }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

std::string readLines(SockT s, int lines, int timeoutMs = 3000) {
    std::string out;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        int have = 0;
        for (char c : out) {
            if (c == '\n') { ++have; }
        }
        if (have >= lines) { break; }
        char buf[4096];
        const int n = static_cast<int>(::recv(s, buf, sizeof(buf), 0));
        if (n > 0) {
            out.append(buf, static_cast<std::size_t>(n));
        } else if (n == 0) {
            break;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    return out;
}

// A port nobody is listening on right now: bound, read back, closed.
int freePort() {
    sockInit();
    SockT s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = 0;
    a.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    int port = 0;
    if (s != INVALID_SOCKET && ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
        socklen_t len = sizeof(a);
        if (::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) == 0) { port = ntohs(a.sin_port); }
    }
    closeSock(s);
    return port;
}

// ---- the engine ------------------------------------------------------------------------------------------------------------------

// A hermetic engine that HOSTS its servers (servers=1), its pass on a thread of its own - and NO pull installed: CAT does not need one.
struct Rig {
    Engine e;
    FoxSession* op = nullptr;
    int catPort = 0;
    explicit Rig(const std::string& options = "testHooks=1;servers=1") {
        e.configureApi(options);
        e.initialise();
        (void)e.applyCommand(cmd::makeInt(FOXAPI_OP_RUN, 1));
        e.controlPass();
        e.startControlThread();
        FoxSessionParams p{};
        p.structSize = sizeof(p);
        p.flags = FOXAPI_SESSION_LOCAL;
        p.grants = kAllGrants;
        p.clientName = "the operator";
        (void)api()->open_session(e.apiHandle(), &p, &op);
    }
    ~Rig() {
        if (op != nullptr) { api()->close_session(op); }
        e.shutdown();
    }
    // SERVER_CONFIG, waiting for the answer and for the host's worker to have acted on it.
    FoxCommandResult config(const std::string& json) {
        FoxCommand c{};
        c.structSize = sizeof(c);
        c.op = FOXAPI_OP_SERVER_CONFIG;
        FoxSubmitResult sr{};
        sr.structSize = sizeof(sr);
        FoxCommandResult out{};
        out.status = FOXAPI_FAILED;
        if (api()->submit_text(op, &c, json.data(), static_cast<uint32_t>(json.size()), &sr) < 0) { return out; }
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < end) {
            FoxCommandResult r[8];
            for (auto& x : r) { x.structSize = sizeof(x); }
            const int32_t got = api()->poll_results(op, r, 8);
            for (int32_t i = 0; i < got; ++i) {
                if (r[i].ticket == sr.ticket) {
                    out = r[i];
                    (void)e.servers().waitIdle(std::chrono::seconds(30));
                    return out;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return out;
    }
    // CAT on a free port.
    bool startCat() {
        catPort = freePort();
        const FoxCommandResult a = config("{\"cat\":{\"enabled\":true,\"bindAll\":false,\"port\":" + std::to_string(catPort) + "}}");
        return a.status == FOXAPI_OK && e.servers().catServer().running();
    }
    FoxReceiverState state() {
        FoxReceiverState st{};
        st.structSize = sizeof(st);
        for (int i = 0; i < 100 && api()->read_state(op, &st) != FOXAPI_OK; ++i) {}
        return st;
    }
    FoxListItem server(std::uint32_t index) {
        FoxListItem it{};
        it.structSize = sizeof(it);
        (void)api()->read_list(op, FOXAPI_LIST_SERVERS, index, &it);
        return it;
    }
    std::size_t sessions() { return e.api().sessionCountForTest(); }
    std::size_t catSessions() { return e.api().sessionCountForTest(InternalKind::Cat); }
};

// One request, one reply line (a get or a set), on an open connection.
std::string ask(SockT s, const std::string& line, int lines = 1) {
    if (!sendRaw(s, line + "\n")) { return std::string(); }
    return readLines(s, lines);
}

long long hz(double v) { return static_cast<long long>(std::llround(v)); }

// ===================================================================================================================================

void testWireThroughTheEngine() {
    Rig r;
    CHECK(r.startCat());
    SockT s = connectTo(r.catPort, 2000);
    CHECK(s != INVALID_SOCKET);
    if (s == INVALID_SOCKET) { return; }
    const FoxReceiverState st = r.state();
    CHECK((st.flags & FOXAPI_RX_RUNNING) != 0u);

    // f: the tuned frequency (centre + offset), from the connection's own read_state.
    CHECK_EQ(ask(s, "f"), std::to_string(hz(st.centreHz + st.vfoOffsetHz)) + "\n");
    // \dump_state is the same table the protocol layer builds from the figures; the connection must at least have served it whole.
    const std::string dump = ask(s, "\\dump_state", 26);
    CHECK(dump.rfind("1\n1\n2\n", 0) == 0);
    CHECK_EQ(static_cast<int>(std::count(dump.begin(), dump.end(), '\n')), 26);

    // F inside the band: RPRT 0, and the pass moves the VFO to the requested frequency.
    const long long target = static_cast<long long>(st.centreHz) + 40000;
    CHECK_EQ(ask(s, "F " + std::to_string(target)), std::string("RPRT 0\n"));
    CHECK(waitUntil(3000, [&] { return r.state().vfoOffsetHz == 40000.0; }));
    CHECK_EQ(ask(s, "f"), std::to_string(hz(st.centreHz + 40000.0)) + "\n");

    // M: the mode and its passband (a USB filter of 4000 Hz; the engine's own floor for a passband is 3 kHz, so Hamlib's usual 2400 is
    // clamped to 3000 by the same SET_BANDWIDTH as ever - which is why the test does not ask for it).
    CHECK_EQ(ask(s, "M USB 4000"), std::string("RPRT 0\n"));
    CHECK(waitUntil(3000, [&] {
        const FoxReceiverState x = r.state();
        return x.demodMode == FOXAPI_DEMOD_USB && x.bandwidthHz == 4000.0;
    }));
    CHECK_EQ(ask(s, "m", 2), std::string("USB\n4000\n"));
    // ...and "FM 0" is Hamlib's "the default passband of the mode": the mode's default, not a zero-width filter.
    CHECK_EQ(ask(s, "M FM 0"), std::string("RPRT 0\n"));
    CHECK(waitUntil(3000, [&] { return r.state().demodMode == FOXAPI_DEMOD_NFM; }));
    const FoxReceiverState nfm = r.state();
    CHECK(nfm.bandwidthHz > 1000.0);
    CHECK_EQ(ask(s, "m", 2), std::string("FM\n") + std::to_string(hz(nfm.bandwidthHz)) + "\n");

    // The receiver is always receiving: t is 0, T 0 is fine, and T 1 is refused with the code a radio with no transmitter answers.
    CHECK_EQ(ask(s, "t"), std::string("0\n"));
    CHECK_EQ(ask(s, "T 0"), std::string("RPRT 0\n"));
    CHECK_EQ(ask(s, "T 1"), std::string("RPRT -11\n"));
    CHECK_EQ(ask(s, "t"), std::string("0\n"));
    // The power state reflects the receiver running (\get_powerstat).
    CHECK_EQ(ask(s, "\\get_powerstat"), std::string("1\n"));
    CHECK(r.server(1).value[3] >= 10.0);  // the list counted the commands served
    closeSock(s);
    std::printf("C1: f/F/M/m/t/T over a real socket against the engine - F moved the offset to 40000 (centre %.0f kept), M USB 4000 and M FM 0 followed, T 1 -> RPRT -11\n",
                st.centreHz);
}

void testTunePolicyIsPlanTune() {
    Rig r;
    CHECK(r.startCat());
    SockT s = connectTo(r.catPort, 2000);
    CHECK(s != INVALID_SOCKET);
    if (s == INVALID_SOCKET) { return; }
    const FoxReceiverState st = r.state();
    CHECK(st.sampleRateHz > 100000.0);

    // INSIDE the band (a quarter of the way to the edge): the centre does NOT move, the VFO does. A SET_FREQUENCY ("VFO kept, centre
    // follows") would leave the offset at 0 and move the centre to the target: both halves are checked.
    const double inside = st.centreHz + 0.25 * st.sampleRateHz;
    CHECK_EQ(ask(s, "F " + std::to_string(hz(inside))), std::string("RPRT 0\n"));
    CHECK(waitUntil(3000, [&] { return std::fabs(r.state().vfoOffsetHz - (hz(inside) - st.centreHz)) < 1.0; }));
    FoxReceiverState a = r.state();
    CHECK_EQ(a.centreHz, st.centreHz);
    CHECK(std::fabs(a.vfoOffsetHz - 0.25 * st.sampleRateHz) < 1.0);

    // OUTSIDE the band (a whole sample rate away): the centre moves to the target and the offset goes back to 0, in the same batch.
    const double outside = st.centreHz + 1.0 * st.sampleRateHz;
    CHECK_EQ(ask(s, "F " + std::to_string(hz(outside))), std::string("RPRT 0\n"));
    CHECK(waitUntil(3000, [&] { return std::fabs(r.state().centreHz - hz(outside)) < 1.0; }));
    const FoxReceiverState b = r.state();
    CHECK(std::fabs(b.centreHz - hz(outside)) < 1.0);
    CHECK_EQ(b.vfoOffsetHz, 0.0);

    // And inside the NEW band again: only the VFO.
    const double inside2 = b.centreHz - 0.1 * st.sampleRateHz;
    CHECK_EQ(ask(s, "F " + std::to_string(hz(inside2))), std::string("RPRT 0\n"));
    CHECK(waitUntil(3000, [&] { return std::fabs(r.state().vfoOffsetHz - (hz(inside2) - b.centreHz)) < 1.0; }));
    CHECK_EQ(r.state().centreHz, b.centreHz);
    closeSock(s);
    std::printf("C2: F inside the band left the centre at %.0f and set the offset (%+.0f); F outside moved the centre to %.0f and returned the offset to 0\n",
                st.centreHz, a.vfoOffsetHz, b.centreHz);
}

void testFifthClientRefusedBeforeASessionIsMade() {
    Rig r;
    CHECK(r.startCat());
    const std::size_t baseline = r.sessions();
    std::vector<SockT> held;
    for (int i = 0; i < cascade::net::kMaxCatClients; ++i) {
        SockT s = connectTo(r.catPort, 2000);
        CHECK(s != INVALID_SOCKET);
        if (s == INVALID_SOCKET) { continue; }
        CHECK(!ask(s, "f").empty());  // served: its connection thread (and so its session) exists
        held.push_back(s);
    }
    CHECK_EQ(r.catSessions(), static_cast<std::size_t>(cascade::net::kMaxCatClients));
    CHECK_EQ(r.sessions(), baseline + static_cast<std::size_t>(cascade::net::kMaxCatClients));
    CHECK_EQ(r.e.servers().catServer().clientCount(), cascade::net::kMaxCatClients);

    // The fifth is accepted by the OS and dropped by the server: an immediate close, and no session was made for it.
    SockT extra = connectTo(r.catPort, 1500);
    CHECK(extra != INVALID_SOCKET);
    if (extra != INVALID_SOCKET) {
        sendRaw(extra, "f\n");
        CHECK(readLines(extra, 1, 1500).empty());
        closeSock(extra);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_EQ(r.catSessions(), static_cast<std::size_t>(cascade::net::kMaxCatClients));
    CHECK_EQ(r.e.servers().catServer().clientCount(), cascade::net::kMaxCatClients);
    for (SockT s : held) { closeSock(s); }
    CHECK(waitUntil(3000, [&] { return r.catSessions() == 0u; }));
    std::printf("C3: four clients hold four CAT sessions, the fifth was closed on accept and made none (sessions %zu -> %zu -> %zu)\n", baseline,
                baseline + 4, r.sessions());
}

void testSessionCountReturnsToBaseline() {
    Rig r;
    CHECK(r.startCat());
    const std::size_t baseline = r.sessions();  // the operator's own session, and nothing else
    CHECK_EQ(r.catSessions(), static_cast<std::size_t>(0));

    SockT a = connectTo(r.catPort, 2000);
    SockT b = connectTo(r.catPort, 2000);
    CHECK(a != INVALID_SOCKET && b != INVALID_SOCKET);
    if (a == INVALID_SOCKET || b == INVALID_SOCKET) {
        closeSock(a);
        closeSock(b);
        return;
    }
    CHECK(!ask(a, "f").empty() && !ask(b, "f").empty());
    CHECK_EQ(r.sessions(), baseline + 2);
    CHECK_EQ(r.catSessions(), static_cast<std::size_t>(2));

    // A disconnect closes that connection's session - and only that one.
    closeSock(a);
    CHECK(waitUntil(3000, [&] { return r.sessions() == baseline + 1; }));
    CHECK_EQ(r.catSessions(), static_cast<std::size_t>(1));
    CHECK(!ask(b, "f").empty());  // the other is untouched
    closeSock(b);
    CHECK(waitUntil(3000, [&] { return r.sessions() == baseline; }));
    CHECK_EQ(r.catSessions(), static_cast<std::size_t>(0));

    // stop() with clients connected: when the listener's stop has returned (SERVER_CONFIG settles on the worker that ran it), no session of
    // the server exists - checked at once, no waiting, because the connection threads close theirs before they stop being counted.
    SockT c = connectTo(r.catPort, 2000);
    SockT d = connectTo(r.catPort, 2000);
    CHECK(c != INVALID_SOCKET && d != INVALID_SOCKET);
    CHECK(!ask(c, "f").empty() && !ask(d, "f").empty());
    CHECK_EQ(r.sessions(), baseline + 2);
    CHECK(r.config("{\"cat\":{\"enabled\":false}}").status == FOXAPI_OK);
    CHECK(!r.e.servers().catServer().running());
    CHECK_EQ(r.e.servers().catServer().clientCount(), 0);
    CHECK_EQ(r.sessions(), baseline);
    CHECK_EQ(r.catSessions(), static_cast<std::size_t>(0));
    closeSock(c);
    closeSock(d);

    // ...and the same listener started again serves again, from the same baseline.
    CHECK(r.startCat());
    SockT e2 = connectTo(r.catPort, 2000);
    CHECK(e2 != INVALID_SOCKET);
    CHECK(!ask(e2, "f").empty());
    CHECK_EQ(r.sessions(), baseline + 1);
    closeSock(e2);
    CHECK(waitUntil(3000, [&] { return r.sessions() == baseline; }));
    std::printf("C4: sessions %zu -> %zu with two clients -> %zu after one left -> %zu after both -> %zu with two more and stop() -> %zu\n", baseline,
                baseline + 2, baseline + 1, baseline, baseline + 2, baseline);
}

void testAThousandCommandsDrain() {
    Rig r;
    CHECK(r.startCat());
    SockT s = connectTo(r.catPort, 2000);
    CHECK(s != INVALID_SOCKET);
    if (s == INVALID_SOCKET) { return; }
    const FoxReceiverState st = r.state();
    CHECK(st.sampleRateHz > 1.0e6);

    // 1000 sets, ten batches of a hundred, each batch waited for until the pass has applied its last: the offsets run 100 Hz .. 100 kHz,
    // all inside the band. Each batch is far below the session's 256, so a connection that collects results as it goes never sees BUSY;
    // one that does not collect them reaches 256 outstanding on the third batch, and from then on every set is refused - the batch's last
    // offset never lands, and the wait below fails.
    int landed = 0;
    for (int batch = 0; batch < 10; ++batch) {
        std::string lines;
        for (int i = 0; i < 100; ++i) {
            const int n = batch * 100 + i + 1;
            lines += "F " + std::to_string(hz(st.centreHz) + 100 * n) + "\n";
        }
        CHECK(sendRaw(s, lines));
        const std::string replies = readLines(s, 100, 8000);
        CHECK_EQ(static_cast<int>(std::count(replies.begin(), replies.end(), '\n')), 100);
        CHECK(replies.find("RPRT -") == std::string::npos);
        const double last = 100.0 * static_cast<double>((batch + 1) * 100);
        const bool ok = waitUntil(4000, [&] { return r.state().vfoOffsetHz == last; });
        CHECK(ok);
        if (!ok) {
            std::printf("   batch %d: the offset reads %.0f, wanted %.0f (the commands were refused)\n", batch, r.state().vfoOffsetHz, last);
            break;
        }
        ++landed;
    }
    CHECK_EQ(landed, 10);
    CHECK_EQ(r.state().vfoOffsetHz, 100000.0);
    CHECK(r.server(1).value[3] >= 1000.0);  // 1000 commands served
    std::printf("C5a: 1000 F commands in ten batches of 100: every batch's last offset landed (final %+.0f Hz), %.0f commands counted\n",
                r.state().vfoOffsetHz, r.server(1).value[3]);

    // A BURST far past the session's cap, in one write. The wire is what it was - every line is answered RPRT 0, the commands the engine
    // could not take are the newest and are simply not applied - and the connection is not left wedged: once the engine has applied what it
    // took and the connection has collected the results, a set lands again.
    std::string burst;
    for (int i = 0; i < 1000; ++i) { burst += "F " + std::to_string(hz(st.centreHz) + 1000 + i) + "\n"; }
    CHECK(sendRaw(s, burst));
    const std::string replies = readLines(s, 1000, 15000);
    CHECK_EQ(static_cast<int>(std::count(replies.begin(), replies.end(), '\n')), 1000);
    CHECK(replies.find("RPRT -") == std::string::npos);
    bool recovered = false;
    for (int attempt = 0; attempt < 40 && !recovered; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (ask(s, "F " + std::to_string(hz(st.centreHz) + 7777)) != "RPRT 0\n") { continue; }
        recovered = waitUntil(500, [&] { return r.state().vfoOffsetHz == 7777.0; });
    }
    CHECK(recovered);
    std::printf("C5b: a 1000-line burst was all answered RPRT 0, and the connection served again afterwards (offset %+.0f)\n", r.state().vfoOffsetHz);
    closeSock(s);
}

FoxCommand benign(std::uint32_t op) {
    FoxCommand c{};
    c.structSize = sizeof(c);
    c.op = op;
    c.num[0] = 1.0;
    c.ival[0] = 1;
    std::strcpy(c.text, "x=y");
    switch (op) {
        case FOXAPI_OP_SET_CENTRE: case FOXAPI_OP_SET_FREQUENCY: c.num[0] = 100.0e6; break;
        case FOXAPI_OP_SET_BANDWIDTH: c.num[0] = 12500.0; break;
        case FOXAPI_OP_SET_VOLUME: c.num[0] = 0.5; break;
        case FOXAPI_OP_SET_MODE: c.ival[0] = FOXAPI_DEMOD_NFM; break;
        case FOXAPI_OP_SELECT_SOURCE: std::strcpy(c.text, "file:C:/secret.iq"); break;
        default: break;
    }
    return c;
}

void testTheProfileCannotKey() {
    Rig r;
    // The profile, exactly: the four ops F and M send, and no transmitter op in it.
    const auto ops = cascade::engine::internalProfileOps(InternalKind::Cat);
    const std::set<std::uint32_t> mine(ops.begin(), ops.end());
    CHECK_EQ(mine.size(), static_cast<std::size_t>(4));
    CHECK_EQ(ops.size(), static_cast<std::size_t>(4));
    for (const std::uint32_t op : {FOXAPI_OP_SET_CENTRE, FOXAPI_OP_SET_VFO_OFFSET, FOXAPI_OP_SET_MODE, FOXAPI_OP_SET_BANDWIDTH}) {
        CHECK(mine.count(op) == 1u);
    }
    for (const std::uint32_t op : mine) { CHECK((op & 0xFF00u) != 0x0A00u); }  // none is in the transmitter's group
    const std::uint64_t g = cascade::engine::internalProfileGrants(InternalKind::Cat);
    CHECK_EQ(g, FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS);

    // A session of the kind the CAT server opens, at the table's door and through a real submit.
    std::int32_t st = 0;
    FoxSession* cat = r.e.api().openInternalSession(InternalKind::Cat, "a CAT client", &st);
    CHECK_EQ(st, FOXAPI_OK);
    CHECK(cat != nullptr);
    if (cat == nullptr) { return; }
    // The operator gives the consent a browser would need to press the PTT: it makes no difference to a CAT session.
    FoxCommand arm = benign(FOXAPI_OP_TX_REMOTE_ARM);
    FoxSubmitResult ar{};
    ar.structSize = sizeof(ar);
    (void)api()->submit(r.op, &arm, 1, &ar);

    const std::uint32_t refused[] = {FOXAPI_OP_TX_PTT,     FOXAPI_OP_TX_LATCH,       FOXAPI_OP_TX_REMOTE_ARM, FOXAPI_OP_TX_OPEN,
                                     FOXAPI_OP_TX_CLOSE,   FOXAPI_OP_TX_SET_MODE,    FOXAPI_OP_TX_SET_POWER,  FOXAPI_OP_SERVER_CONFIG,
                                     FOXAPI_OP_STORE_FETCH, FOXAPI_OP_SET_FREQUENCY, FOXAPI_OP_SET_VOLUME,    FOXAPI_OP_SELECT_SOURCE,
                                     FOXAPI_OP_RUN,        FOXAPI_OP_SETTING_SET};
    int n = 0;
    for (const std::uint32_t op : refused) {
        for (int on = 0; on <= 1; ++on) {
            FoxCommand c = benign(op);
            c.ival[0] = on;
            const std::int32_t v = r.e.api().validateForTest(cat, c);
            if (v != FOXAPI_DENIED) { std::printf("   %s (ival %d) at the door: %d, wanted DENIED\n", cmd::opName(op), on, v); }
            CHECK_EQ(v, FOXAPI_DENIED);
            ++n;
        }
    }
    // The four it may speak are not refused by the profile.
    for (const std::uint32_t op : mine) { CHECK(r.e.api().validateForTest(cat, benign(op)) != FOXAPI_DENIED); }

    // ...and as results: the PTT, the latch and the arm are submitted for real and come back DENIED, REFUSED, naming the profile.
    std::vector<FoxSubmitResult> sub;
    std::vector<std::uint32_t> sent;
    for (const std::uint32_t op : {FOXAPI_OP_TX_PTT, FOXAPI_OP_TX_LATCH, FOXAPI_OP_TX_REMOTE_ARM}) {
        FoxCommand c = benign(op);
        FoxSubmitResult sr{};
        sr.structSize = sizeof(sr);
        (void)api()->submit(cat, &c, 1, &sr);
        sub.push_back(sr);
        sent.push_back(op);
    }
    std::vector<FoxCommandResult> got;
    CHECK(waitUntil(3000, [&] {
        FoxCommandResult res[8];
        for (auto& x : res) { x.structSize = sizeof(x); }
        const int32_t k = api()->poll_results(cat, res, 8);
        for (int32_t i = 0; i < k; ++i) { got.push_back(res[i]); }
        return got.size() >= sub.size();
    }));
    CHECK_EQ(got.size(), sub.size());
    for (const FoxCommandResult& x : got) {
        CHECK_EQ(x.status, FOXAPI_DENIED);
        CHECK((x.flags & FOXAPI_RESULT_REFUSED) != 0u);
        CHECK(std::string(x.message).find("profile") != std::string::npos);
    }
    // Nothing keyed, nothing latched.
    const FoxReceiverState rx = r.state();
    CHECK((rx.flags & FOXAPI_RX_TX_KEY_MINE) == 0u);
    api()->close_session(cat);
    std::printf("C6: the CAT profile is the four ops F and M send; %d transmitter/store/config/source asks refused at the door, the PTT, latch and arm refused as results ('%s')\n",
                n, got.empty() ? "" : got[0].message);
}

void testAOneShotClientStillRetunes() {
    Rig r;
    CHECK(r.startCat());
    const FoxReceiverState st = r.state();
    // `rigctl -m 2 -r localhost:PORT F 145250000`: send, read the reply, hang up - three rounds, each from a fresh connection.
    for (int round = 1; round <= 3; ++round) {
        SockT s = connectTo(r.catPort, 2000);
        CHECK(s != INVALID_SOCKET);
        if (s == INVALID_SOCKET) { return; }
        const long long want = 10000 * round;
        CHECK_EQ(ask(s, "F " + std::to_string(hz(st.centreHz) + want)), std::string("RPRT 0\n"));
        closeSock(s);  // gone before the pass has run
        const bool landed = waitUntil(3000, [&] { return r.state().vfoOffsetHz == static_cast<double>(want); });
        CHECK(landed);
        if (!landed) { std::printf("   round %d: the offset reads %.0f, wanted %lld (the closed session's command was refused)\n", round, r.state().vfoOffsetHz, want); }
    }
    CHECK(waitUntil(3000, [&] { return r.catSessions() == 0u; }));
    std::printf("C7: three one-shot clients each retuned the radio and hung up before the pass (final offset %+.0f)\n", r.state().vfoOffsetHz);
}

void testTeardownWithClientsConnected() {
    const auto t0 = std::chrono::steady_clock::now();
    SockT a = INVALID_SOCKET, b = INVALID_SOCKET;
    {
        Rig r;
        CHECK(r.startCat());
        a = connectTo(r.catPort, 2000);
        b = connectTo(r.catPort, 2000);
        CHECK(a != INVALID_SOCKET && b != INVALID_SOCKET);
        if (a != INVALID_SOCKET) { CHECK(!ask(a, "f").empty()); }
        if (b != INVALID_SOCKET) { CHECK(sendRaw(b, "F 100\nF 200\n")); }  // commands in flight as the engine goes down
        // (b's session is made when the acceptor gets to b's connection - not when connect() returns - so it is waited for)
        CHECK(waitUntil(3000, [&] { return r.catSessions() == 2u; }));
    }  // ~Rig: the operator's session closes, the engine shuts down (the servers first)
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 10000.0);
    // The peers are told: the sockets are closed from the other end.
    char buf[16];
    if (a != INVALID_SOCKET) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        int n = -1;
        while (std::chrono::steady_clock::now() < end) {
            n = static_cast<int>(::recv(a, buf, sizeof(buf), 0));
            if (n >= 0) { break; }
        }
        CHECK_EQ(n, 0);
    }
    closeSock(a);
    closeSock(b);
    std::printf("C8: an engine torn down with two clients connected (one with sets in flight) shut down in %.0f ms and closed their sockets\n", ms);
}

}  // namespace

int main(int argc, char** argv) {
    // (An argument of the form C<n> runs that case alone - the break-it checks use it; the CTest entry passes none.)
    std::string only;
    if (argc > 1 && argv[1][0] == 'C' && std::isdigit(static_cast<unsigned char>(argv[1][1])) != 0) { only = argv[1]; }
    const auto want = [&](const char* id) { return only.empty() || only == id; };
    engine_api_test::hermetic("catengine");
    std::printf("test_cat_engine\n");
    if (want("C1")) { testWireThroughTheEngine(); }
    if (want("C2")) { testTunePolicyIsPlanTune(); }
    if (want("C3")) { testFifthClientRefusedBeforeASessionIsMade(); }
    if (want("C4")) { testSessionCountReturnsToBaseline(); }
    if (want("C5")) { testAThousandCommandsDrain(); }
    if (want("C6")) { testTheProfileCannotKey(); }
    if (want("C7")) { testAOneShotClientStillRetunes(); }
    if (want("C8")) { testTeardownWithClientsConnected(); }
    return testSummary("test_cat_engine");
}
