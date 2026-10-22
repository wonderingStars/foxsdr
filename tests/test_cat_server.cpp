// Tests for net/cat_server.hpp — the CAT server driven over a REAL socket.
//
// cat_protocol is already tested as a pure function, so nothing here re-checks
// what a command means. What this file exists to prove is everything the pure
// tests structurally cannot: that a socket is actually listening, that
// requests split across TCP segments are reassembled, that several commands
// arriving in one segment are all answered, that set requests reach the
// connection's session on the engine, and that the connection cap and the line
// cap hold.
//
// A mock cannot show any of that, and an I/O feature verified only against a
// mock is a hypothesis rather than a result.
//
// STAGE 5c: THE SERVER SPEAKS TO THE ENGINE THROUGH A SESSION PER CONNECTION
// (net::CatSession), so these tests give it a STUB session - the figures from
// g_status, every submitted command recorded - and the socket under it is
// still real. What the real engine does with the commands (the table's
// BUSY at 256, the profile, the pass's order) is tests/test_cat_engine.cpp's
// and tests/test_internal_session_order.cpp's. The old queue's pinned
// overflow case ("drop the oldest at 64") has no subject here any more - the
// server keeps no queue; testResultsAreCollectedAsItGoes and
// testASessionThatTakesNothingIsSurvived replace it, and say what they pin.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/cat_server.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
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

#include "foxsdr_api.h"
#include "test_check.hpp"

using cascade::net::CatServer;
using cascade::net::CatRadio;
using cascade::net::CatSession;

namespace {

CatRadio g_status;

// ---- the stub session ------------------------------------------------------------------------------------------------------------------
//
// One shared ledger for every session the server opens (the server's connection threads and the test's thread both touch it).
struct Ledger {
    std::mutex m;
    std::vector<FoxCommand> submitted;  // every command taken, in order, until takeSubmitted()
    int opened = 0;
    int closed = 0;
    int polls = 0;                  // pollResults calls, in all
    int maxOutstanding = 0;         // the most tickets any one session held un-collected at a submit
    int lostAtClose = 0;            // tickets a session still had un-collected when it was closed
    int busyRefused = 0;            // commands the stub refused (past its capacity)
    // How the stub behaves.
    int capacity = 1 << 30;         // outstanding tickets one session holds before it answers BUSY (the table's is 256)
    int applyAfterMs = 0;           // a ticket's result is ready this long after its submit
    bool refuseOpen = false;        // the engine will not give a session
    bool resultsHeld = false;       // pollResults returns nothing (an engine that is not applying)
    std::string nextLabel;
    std::vector<std::string> labels;
} g_ledger;

class StubSession final : public CatSession {
public:
    ~StubSession() override {
        std::lock_guard<std::mutex> lk(g_ledger.m);
        ++g_ledger.closed;
        g_ledger.lostAtClose += static_cast<int>(tickets_.size());
    }
    bool readState(CatRadio& out) override {
        out = g_status;
        return true;
    }
    int submit(const std::vector<FoxCommand>& commands) override {
        std::lock_guard<std::mutex> lk(g_ledger.m);
        int taken = 0;
        for (const FoxCommand& c : commands) {
            if (static_cast<int>(tickets_.size()) >= g_ledger.capacity) {
                ++g_ledger.busyRefused;
                continue;
            }
            g_ledger.submitted.push_back(c);
            tickets_.push_back(std::chrono::steady_clock::now() + std::chrono::milliseconds(g_ledger.applyAfterMs));
            ++taken;
        }
        g_ledger.maxOutstanding = std::max(g_ledger.maxOutstanding, static_cast<int>(tickets_.size()));
        return taken;
    }
    int pollResults() override {
        std::lock_guard<std::mutex> lk(g_ledger.m);
        ++g_ledger.polls;
        if (g_ledger.resultsHeld) return 0;
        const auto now = std::chrono::steady_clock::now();
        int ready = 0;
        while (!tickets_.empty() && tickets_.front() <= now) {
            tickets_.erase(tickets_.begin());
            ++ready;
        }
        return ready;
    }

private:
    std::vector<std::chrono::steady_clock::time_point> tickets_;
};

void resetLedger() {
    std::lock_guard<std::mutex> lk(g_ledger.m);
    g_ledger.submitted.clear();
    g_ledger.opened = g_ledger.closed = g_ledger.polls = g_ledger.maxOutstanding = g_ledger.lostAtClose = g_ledger.busyRefused = 0;
    g_ledger.capacity = 1 << 30;
    g_ledger.applyAfterMs = 0;
    g_ledger.refuseOpen = false;
    g_ledger.resultsHeld = false;
    g_ledger.labels.clear();
}

cascade::net::CatSessionOpener opener() {
    return [](const std::string& label) -> std::unique_ptr<CatSession> {
        std::lock_guard<std::mutex> lk(g_ledger.m);
        if (g_ledger.refuseOpen) return nullptr;
        ++g_ledger.opened;
        g_ledger.labels.push_back(label);
        return std::make_unique<StubSession>();
    };
}

// What the sessions were handed since the last call (the old takePendingControls' place), as the commands of one op.
std::vector<FoxCommand> takeSubmitted() {
    std::lock_guard<std::mutex> lk(g_ledger.m);
    std::vector<FoxCommand> out;
    out.swap(g_ledger.submitted);
    return out;
}

int closedNow() {
    std::lock_guard<std::mutex> lk(g_ledger.m);
    return g_ledger.closed;
}

bool waitFor(int ms, const std::function<bool()>& pred) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

void closeClient(SockT s) {
#if defined(_WIN32)
    if (s != INVALID_SOCKET) ::closesocket(s);
#else
    if (s != INVALID_SOCKET) ::close(s);
#endif
}

SockT connectTo(int port) {
#if defined(_WIN32)
    WSADATA d{};
    ::WSAStartup(MAKEWORD(2, 2), &d);
#endif
    SockT s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    // A receive timeout, or every "nothing should arrive" assertion below
    // blocks for ever instead of failing: recv() on a healthy but silent
    // socket does not return, so the deadline in readLines would never be
    // reached. This is what makes those cases testable at all.
#if defined(_WIN32)
    DWORD tv = 250;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv),
                 sizeof(tv));
#else
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 250 * 1000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = ::htons(static_cast<unsigned short>(port));
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closeClient(s);
        return INVALID_SOCKET;
    }
    return s;
}

bool sendRaw(SockT s, const std::string& text) {
    std::size_t sent = 0;
    while (sent < text.size()) {
        const int n = static_cast<int>(
            ::send(s, text.data() + sent, static_cast<int>(text.size() - sent), 0));
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

// Reads until `lines` newlines have arrived or the deadline passes. Counting
// lines rather than bytes is what makes this robust to segmentation: the point
// of several tests here is that the reply may not arrive in one piece.
std::string readLines(SockT s, int lines, int timeoutMs = 3000) {
    std::string out;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        int have = 0;
        for (char c : out) {
            if (c == '\n') ++have;
        }
        if (have >= lines) break;
        char buf[256];
        const int n = static_cast<int>(::recv(s, buf, sizeof(buf), 0));
        if (n > 0) {
            out.append(buf, static_cast<std::size_t>(n));
        } else if (n == 0) {
            break;  // peer closed
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    return out;
}

// Asks the OS for a free port rather than walking a fixed range.
//
// The range this used to scan started at a FIXED number, which made two copies
// of this suite running at once collide: on Windows SO_REUSEADDR let the second
// process bind a port the first was already listening on, so each suite's
// clients were served by the other suite's server. That produced failures with
// no relation to the code under test — and, until stop() was made to wait for
// its connection threads, an outright crash when one process tore its server
// down while the other still held a connection to it. An ephemeral port cannot
// collide, so the tests below assert the same things with no shared state.
int startOnFreePort(CatServer& server, std::string& error) {
    if (!server.start(0, /*bindAll=*/false, error)) return -1;
    return static_cast<int>(server.boundPort());
}

void resetStatus() {
    resetLedger();
    g_status = CatRadio{};
    g_status.running = true;
    g_status.centerHz = 145'000'000.0;
    g_status.sampleRateHz = 2'400'000.0;
    g_status.vfoOffsetHz = 0.0;
    g_status.bandwidthHz = 12'500.0;
    g_status.mode = "NFM";
}

// --- the tests -------------------------------------------------------------

void testRefusesToStartWithoutAProvider() {
    CatServer server;
    std::string error = "stale";
    // No engine session: every command would have to invent a frequency, so
    // starting at all is refused rather than serving fiction.
    CHECK(!server.start(18999, false, error));
    CHECK(!error.empty());
    CHECK(!server.running());
}

void testRoundTripOverARealSocket() {
    resetStatus();
    CatServer server;
    server.setSessionOpener(opener());
    std::string error;
    const int port = startOnFreePort(server, error);
    CHECK(port > 0);
    if (port < 0) {
        std::printf("SKIP: no free port for the CAT server (%s)\n", error.c_str());
        return;
    }
    CHECK(server.running());

    SockT c = connectTo(port);
    CHECK(c != INVALID_SOCKET);
    if (c == INVALID_SOCKET) {
        server.stop();
        return;
    }

    // A get returns its value and nothing else.
    CHECK(sendRaw(c, "f\n"));
    CHECK(readLines(c, 1) == "145000000\n");

    // Two lines for mode.
    CHECK(sendRaw(c, "m\n"));
    CHECK(readLines(c, 2) == "FM\n12500\n");

    // A set answers RPRT 0 and hands the connection's session its command:
    // inside the sampled band only the VFO moves, so exactly one SET_VFO_OFFSET
    // (500 kHz above the centre) - never SET_FREQUENCY, which would retune.
    CHECK(sendRaw(c, "F 145500000\n"));
    CHECK(readLines(c, 1) == "RPRT 0\n");

    const std::vector<FoxCommand> queued = takeSubmitted();
    CHECK(queued.size() == 1u);
    if (queued.size() == 1u) {
        CHECK(queued[0].op == FOXAPI_OP_SET_VFO_OFFSET);
        CHECK(queued[0].num[0] == 500'000.0);
        // Taking them again must yield nothing: a control applied twice would
        // fight whatever the user did in between.
        CHECK(takeSubmitted().empty());
    }

    CHECK(server.commandCount() >= 3u);

    closeClient(c);
    server.stop();
    CHECK(!server.running());
}

void testCommandSplitAcrossSegmentsIsReassembled() {
    resetStatus();
    CatServer server;
    server.setSessionOpener(opener());
    std::string error;
    const int port = startOnFreePort(server, error);
    if (port < 0) { std::printf("SKIP: no free port\n"); return; }

    SockT c = connectTo(port);
    CHECK(c != INVALID_SOCKET);
    if (c == INVALID_SOCKET) { server.stop(); return; }

    // TCP is a byte stream, not a message stream: a client is entitled to send
    // one command in three writes, and a server that assumed one read equals
    // one command would answer RPRT -4 to each fragment.
    CHECK(sendRaw(c, "F 14"));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(sendRaw(c, "6000"));
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(sendRaw(c, "000\n"));

    CHECK(readLines(c, 1) == "RPRT 0\n");
    const std::vector<FoxCommand> queued = takeSubmitted();
    CHECK(queued.size() == 1u);
    if (queued.size() == 1u) {
        CHECK(queued[0].op == FOXAPI_OP_SET_VFO_OFFSET);
        // 146.0 MHz from a 145.0 MHz centre: 1 MHz, inside the usable band.
        CHECK(queued[0].num[0] > 999'000.0);
        CHECK(queued[0].num[0] < 1'001'000.0);
    }

    closeClient(c);
    server.stop();
}

void testSeveralCommandsInOneSegment() {
    resetStatus();
    CatServer server;
    server.setSessionOpener(opener());
    std::string error;
    const int port = startOnFreePort(server, error);
    if (port < 0) { std::printf("SKIP: no free port\n"); return; }

    SockT c = connectTo(port);
    CHECK(c != INVALID_SOCKET);
    if (c == INVALID_SOCKET) { server.stop(); return; }

    // The mirror case: three commands in one write must produce three replies
    // in order, not just the first. Clients pipeline exactly like this.
    CHECK(sendRaw(c, "f\nt\nv\n"));
    const std::string reply = readLines(c, 3);
    CHECK(reply == "145000000\n0\nVFOA\n");

    closeClient(c);
    server.stop();
}

void testQuitClosesTheConnection() {
    resetStatus();
    CatServer server;
    server.setSessionOpener(opener());
    std::string error;
    const int port = startOnFreePort(server, error);
    if (port < 0) { std::printf("SKIP: no free port\n"); return; }

    SockT c = connectTo(port);
    CHECK(c != INVALID_SOCKET);
    if (c == INVALID_SOCKET) { server.stop(); return; }

    CHECK(sendRaw(c, "q\n"));
    // The server hangs up rather than replying, so a read returns nothing.
    char buf[64];
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    int n = -1;
    while (std::chrono::steady_clock::now() < deadline) {
        n = static_cast<int>(::recv(c, buf, sizeof(buf), 0));
        if (n <= 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(n == 0);  // orderly close, not data

    closeClient(c);
    server.stop();
}

void testOverlongLineIsRefusedRatherThanBuffered() {
    resetStatus();
    CatServer server;
    server.setSessionOpener(opener());
    std::string error;
    const int port = startOnFreePort(server, error);
    if (port < 0) { std::printf("SKIP: no free port\n"); return; }

    SockT c = connectTo(port);
    CHECK(c != INVALID_SOCKET);
    if (c == INVALID_SOCKET) { server.stop(); return; }

    // A client that never sends a newline must not be able to grow the
    // server's buffer without bound — that is a memory leak driven from the
    // network. The server answers once and hangs up.
    const std::string flood(cascade::net::kMaxCatLineBytes + 256, 'x');
    sendRaw(c, flood);
    const std::string reply = readLines(c, 1);
    CHECK(reply == "RPRT -1\n");

    closeClient(c);
    server.stop();
}

void testIdleConnectionSurvives() {
    resetStatus();
    CatServer server;
    server.setSessionOpener(opener());
    std::string error;
    const int port = startOnFreePort(server, error);
    if (port < 0) { std::printf("SKIP: no free port\n"); return; }

    SockT c = connectTo(port);
    CHECK(c != INVALID_SOCKET);
    if (c == INVALID_SOCKET) { server.stop(); return; }

    CHECK(sendRaw(c, "f\n"));
    CHECK(readLines(c, 1) == "145000000\n");

    // THE CASE EVERY OTHER TEST HERE MISSES. All of them send their next
    // command immediately, so they collectively prove the server works for a
    // BUSY client and say nothing about an idle one — and a CAT client is idle
    // almost all of the time, polling a frequency every second or two.
    //
    // The listener carries a short receive timeout so accept() can notice a
    // stop request, and on Linux an accepted socket INHERITS it. A timeout
    // surfaces as recv() returning -1, which reads identically to a closed
    // connection, so the server hung up on healthy clients the moment they
    // paused. This waits several times that timeout and then asks again.
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));

    CHECK(sendRaw(c, "f\n"));
    const std::string afterIdle = readLines(c, 1);
    CHECK(afterIdle == "145000000\n");
    // And the server must still count it as one connected client, not have
    // silently reaped it.
    CHECK(server.clientCount() == 1);

    closeClient(c);
    server.stop();
}

void testConnectionCap() {
    resetStatus();
    CatServer server;
    server.setSessionOpener(opener());
    std::string error;
    const int port = startOnFreePort(server, error);
    if (port < 0) { std::printf("SKIP: no free port\n"); return; }

    std::vector<SockT> held;
    for (int i = 0; i < cascade::net::kMaxCatClients; ++i) {
        SockT s = connectTo(port);
        CHECK(s != INVALID_SOCKET);
        if (s != INVALID_SOCKET) {
            // Force the server to actually service the connection so its count
            // is up to date before the next one is attempted.
            sendRaw(s, "f\n");
            readLines(s, 1);
            held.push_back(s);
        }
    }
    CHECK(server.clientCount() == cascade::net::kMaxCatClients);

    // One more is accepted by the OS and then dropped by the server, so the
    // client sees an immediate close rather than a hang.
    SockT extra = connectTo(port);
    if (extra != INVALID_SOCKET) {
        sendRaw(extra, "f\n");
        const std::string reply = readLines(extra, 1, 1500);
        CHECK(reply.empty());
        closeClient(extra);
    }

    for (SockT s : held) closeClient(s);
    server.stop();
}

// Reads one submitted offset without ever indexing out of range: a CHECK on
// the size records and continues, so guarding the read with one would still
// read past the end in exactly the run that has something to report.
double offsetAt(const std::vector<FoxCommand>& v, std::size_t i) {
    if (i >= v.size() || v[i].op != FOXAPI_OP_SET_VFO_OFFSET) { return -1.0; }
    return v[i].num[0];
}

// REPLACES testControlQueueIsCapped (stage 5c). That test pinned the server's
// OWN queue: nothing drained it (the stalled-GUI case), 200 sets were sent, and
// the newest 64 had to be the ones left - the oldest dropped. The server has no
// queue now, so the bound is the engine's (the session answers the NEWEST BUSY
// at 256 outstanding commands: tests/test_internal_session_order.cpp C and
// tests/test_cat_engine.cpp pin it against the real table). What is left for
// the SERVER to be held to, and what a flood from the network can still do to
// it, is this: it answers every line, hands the session every command in
// order, keeps nothing back, and COLLECTS THE RESULTS as it goes - without
// that a session fills to its cap and starts refusing even though the engine
// is applying everything.
void testResultsAreCollectedAsItGoes() {
    resetStatus();
    CatServer server;
    server.setSessionOpener(opener());
    std::string error;
    const int port = startOnFreePort(server, error);
    if (port < 0) { std::printf("SKIP: no free port\n"); return; }

    SockT c = connectTo(port);
    CHECK(c != INVALID_SOCKET);
    if (c == INVALID_SOCKET) {
        server.stop();
        return;
    }

    // Every frequency stays inside the sampled band, so each command is one VFO
    // move and the offsets identify which commands arrived, and in what order.
    const int kFlood = 200;
    std::string all;
    for (int i = 0; i < kFlood; ++i) {
        all += "F " + std::to_string(145'000'000 + i * 1000) + "\n";
    }
    CHECK(sendRaw(c, all));
    const std::string replies = readLines(c, kFlood, 8000);
    std::size_t lines = 0;
    for (char ch : replies) {
        if (ch == '\n') ++lines;
    }
    CHECK(lines == static_cast<std::size_t>(kFlood));  // all of them were answered

    const std::vector<FoxCommand> queued = takeSubmitted();
    CHECK(queued.size() == static_cast<std::size_t>(kFlood));  // none dropped: the stub never refuses, and the server holds nothing
    CHECK(offsetAt(queued, 0) == 0.0);
    CHECK(offsetAt(queued, static_cast<std::size_t>(kFlood) - 1) == static_cast<double>(kFlood - 1) * 1000.0);
    bool inOrder = queued.size() == static_cast<std::size_t>(kFlood);
    for (std::size_t i = 0; inOrder && i < queued.size(); ++i) { inOrder = offsetAt(queued, i) == static_cast<double>(i) * 1000.0; }
    CHECK(inOrder);

    {
        std::lock_guard<std::mutex> lk(g_ledger.m);
        // The results were collected after every request: the session never held more than the one command it had just been given.
        CHECK(g_ledger.polls >= kFlood);
        CHECK(g_ledger.maxOutstanding <= 1);
        if (g_ledger.maxOutstanding > 1) { std::printf("  the session held %d outstanding commands at once\n", g_ledger.maxOutstanding); }
    }

    closeClient(c);
    server.stop();
}

// An engine that is not applying (its pass is stalled): the session fills to its
// capacity and refuses the rest. The server must survive that unchanged on the
// wire - every line answered, the connection alive, nothing queued by the
// server itself - and serve the client again once the engine applies.
void testASessionThatTakesNothingIsSurvived() {
    resetStatus();
    CatServer server;
    server.setSessionOpener(opener());
    std::string error;
    const int port = startOnFreePort(server, error);
    if (port < 0) { std::printf("SKIP: no free port\n"); return; }
    {
        std::lock_guard<std::mutex> lk(g_ledger.m);
        g_ledger.capacity = 5;
        g_ledger.resultsHeld = true;
    }

    SockT c = connectTo(port);
    CHECK(c != INVALID_SOCKET);
    if (c == INVALID_SOCKET) {
        server.stop();
        return;
    }
    const int kFlood = 20;
    std::string all;
    for (int i = 0; i < kFlood; ++i) {
        all += "F " + std::to_string(145'000'000 + i * 1000) + "\n";
    }
    CHECK(sendRaw(c, all));
    const std::string replies = readLines(c, kFlood, 8000);
    std::size_t lines = 0;
    for (char ch : replies) {
        if (ch == '\n') ++lines;
    }
    CHECK(lines == static_cast<std::size_t>(kFlood));  // the wire is what it was: every set is answered
    CHECK(replies.find("RPRT 0") != std::string::npos && replies.find("RPRT -") == std::string::npos);

    // The session took the FIRST five and refused the newest (the table's rule; the old queue kept the newest and dropped the oldest).
    const std::vector<FoxCommand> queued = takeSubmitted();
    CHECK(queued.size() == 5u);
    CHECK(offsetAt(queued, 0) == 0.0);
    CHECK(offsetAt(queued, 4) == 4000.0);
    int pollsBefore = 0;
    {
        std::lock_guard<std::mutex> lk(g_ledger.m);
        CHECK(g_ledger.busyRefused == kFlood - 5);
        g_ledger.resultsHeld = false;  // the engine applies again
        pollsBefore = g_ledger.polls;
    }
    // ...and the connection is alive and serves the client: the idle tick collects the held results, and a set is taken again.
    CHECK(waitFor(3000, [&] {
        std::lock_guard<std::mutex> lk(g_ledger.m);
        return g_ledger.polls > pollsBefore;  // the idle tick polled
    }));
    CHECK(sendRaw(c, "F 145100000\n"));
    CHECK(readLines(c, 1) == "RPRT 0\n");
    const std::vector<FoxCommand> again = takeSubmitted();
    CHECK(again.size() == 1u);
    CHECK(offsetAt(again, 0) == 100'000.0);

    closeClient(c);
    server.stop();
}

// A CONNECTION IS A SESSION (stage 5c): opened at accept (after the cap), named
// for the connection, and closed by the connection's own thread BEFORE the
// client count drops - so a stop() that sees no client left has no session
// left either. A refused session (the engine's limits) refuses the
// connection, as the cap does.
void testEveryConnectionHoldsASessionAndClosesIt() {
    resetStatus();
    CatServer server;
    server.setSessionOpener(opener());
    std::string error;
    const int port = startOnFreePort(server, error);
    if (port < 0) { std::printf("SKIP: no free port\n"); return; }

    SockT a = connectTo(port);
    SockT b = connectTo(port);
    CHECK(a != INVALID_SOCKET && b != INVALID_SOCKET);
    if (a == INVALID_SOCKET || b == INVALID_SOCKET) {
        closeClient(a);
        closeClient(b);
        server.stop();
        return;
    }
    CHECK(sendRaw(a, "f\n") && readLines(a, 1) == "145000000\n");
    CHECK(sendRaw(b, "f\n") && readLines(b, 1) == "145000000\n");
    {
        std::lock_guard<std::mutex> lk(g_ledger.m);
        CHECK(g_ledger.opened == 2);
        CHECK(g_ledger.closed == 0);
        CHECK(g_ledger.labels.size() == 2u);
        if (g_ledger.labels.size() == 2u) {
            CHECK(g_ledger.labels[0] == "cat:1");  // named for the connection
            CHECK(g_ledger.labels[1] == "cat:2");
        }
    }
    CHECK(server.clientCount() == 2);

    // One hangs up. The moment the server stops COUNTING it, its session is already closed: the order the thread keeps.
    closeClient(a);
    CHECK(waitFor(3000, [&] { return server.clientCount() == 1; }));
    CHECK(closedNow() == 1);

    // The engine runs out of sessions: the next connection is refused (closed at once), not served, and is not counted.
    {
        std::lock_guard<std::mutex> lk(g_ledger.m);
        g_ledger.refuseOpen = true;
    }
    SockT refused = connectTo(port);
    CHECK(refused != INVALID_SOCKET);
    if (refused != INVALID_SOCKET) {
        sendRaw(refused, "f\n");
        CHECK(readLines(refused, 1, 1500).empty());
        closeClient(refused);
    }
    CHECK(server.clientCount() == 1);
    {
        std::lock_guard<std::mutex> lk(g_ledger.m);
        g_ledger.refuseOpen = false;
        CHECK(g_ledger.opened == 2);  // no session was made for the refused one
    }

    // stop() ends the other, and when it returns nothing is open.
    server.stop();
    CHECK(server.clientCount() == 0);
    CHECK(closedNow() == 2);
    closeClient(b);
}

// A ONE-SHOT CLIENT STILL RETUNES (stage 5c). `rigctl F ...` sends the set,
// reads "RPRT 0" and hangs up. The old queue applied the request whoever had
// sent it; a session that closes with commands queued has them refused
// (DETACHED), so the connection must wait - bounded - for what it handed over
// to be applied before it closes the session. Here the engine applies 150 ms
// after the submit (a few passes); the client is gone long before.
void testAConnectionDrainsBeforeItsSessionCloses() {
    resetStatus();
    CatServer server;
    server.setSessionOpener(opener());
    std::string error;
    const int port = startOnFreePort(server, error);
    if (port < 0) { std::printf("SKIP: no free port\n"); return; }
    {
        std::lock_guard<std::mutex> lk(g_ledger.m);
        g_ledger.applyAfterMs = 150;
    }

    SockT c = connectTo(port);
    CHECK(c != INVALID_SOCKET);
    if (c == INVALID_SOCKET) {
        server.stop();
        return;
    }
    CHECK(sendRaw(c, "F 145250000\nM USB 2400\n"));
    CHECK(readLines(c, 2) == "RPRT 0\nRPRT 0\n");
    closeClient(c);  // gone before the engine has applied anything
    CHECK(waitFor(5000, [&] { return server.clientCount() == 0; }));
    {
        std::lock_guard<std::mutex> lk(g_ledger.m);
        CHECK(g_ledger.closed == 1);
        CHECK(g_ledger.lostAtClose == 0);  // every ticket was collected before the session closed
        if (g_ledger.lostAtClose != 0) { std::printf("  the session closed with %d commands un-applied\n", g_ledger.lostAtClose); }
    }
    server.stop();
}

void testStopClosesLiveConnections() {
    resetStatus();
    CatServer server;
    server.setSessionOpener(opener());
    std::string error;
    const int port = startOnFreePort(server, error);
    if (port < 0) { std::printf("SKIP: no free port\n"); return; }

    SockT c = connectTo(port);
    CHECK(c != INVALID_SOCKET);
    if (c == INVALID_SOCKET) { server.stop(); return; }

    CHECK(sendRaw(c, "f\n"));
    CHECK(readLines(c, 1) == "145000000\n");
    CHECK(server.clientCount() == 1);

    // THE CRASH. Connection threads are detached and capture the server, and
    // stop() used to close only the listening socket: a client that simply sat
    // there — which is what a CAT client does nearly all the time — left a
    // thread parked in recv() holding a pointer to an object the caller was
    // about to destroy. The next byte that peer sent, or its hang-up, then ran
    // serveClient against freed memory. A peer can pick that moment, so it is
    // a remote use-after-free rather than a shutdown-ordering nuisance.
    //
    // stop() must therefore be a hard stop: when it returns, no connection
    // thread is left to touch the object.
    //
    // AND IT MUST BE PROMPT. This runs on the GUI thread — unticking "Accept
    // CAT connections", changing the port, closing the application — so the
    // wait for those threads has to be bounded by something this process
    // controls. It cannot be bounded by the peer hanging up: the first hard
    // stop leaned on shutdown() to wake recv(), which on Windows does not
    // interrupt a recv() already in progress, so with one idle client
    // connected stop() blocked until the FIN_WAIT_2 timer expired — measured
    // at 120.0 s, on the GUI thread, roughly half the time. The sleep below
    // makes that deterministic rather than a race: it parks the connection
    // thread inside recv() before stop() is called.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto stopBegan = std::chrono::steady_clock::now();
    server.stop();
    const auto stopMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - stopBegan)
                            .count();
    // One accept poll plus one receive poll is the expected cost, a few
    // hundred milliseconds; this bound is an order of magnitude above that and
    // still far below the two-minute stall it exists to catch.
    CHECK(stopMs < 4000);
    if (stopMs >= 4000) {
        std::printf("  stop() took %lld ms with one idle client connected\n",
                    static_cast<long long>(stopMs));
    }
    CHECK(server.clientCount() == 0);
    CHECK(!server.running());
    // ...and with the last client gone, its session is gone too (stage 5c).
    CHECK(closedNow() == 1);

    // ...and the peer is told, rather than being left holding a socket that
    // will never answer again.
    char buf[64];
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(4000);
    int n = -1;
    while (std::chrono::steady_clock::now() < deadline) {
        n = static_cast<int>(::recv(c, buf, sizeof(buf), 0));
        if (n >= 0) break;  // 0 = closed; a negative is this socket's timeout
    }
    CHECK(n == 0);

    closeClient(c);
}

void testSecondBindOfTheSamePortIsRefused() {
    resetStatus();
    CatServer first;
    first.setSessionOpener(opener());
    std::string error;
    const int port = startOnFreePort(first, error);
    if (port < 0) { std::printf("SKIP: no free port\n"); return; }

    // A control port that retunes the radio must not be quietly takeable by
    // another local program. On Windows SO_REUSEADDR would let this second
    // bind SUCCEED and start stealing connections; SO_EXCLUSIVEADDRUSE makes
    // it fail here instead, through the ordinary bind-error path.
    CatServer second;
    second.setSessionOpener(opener());
    std::string secondError = "stale";
    CHECK(!second.start(static_cast<std::uint16_t>(port), /*bindAll=*/false,
                        secondError));
    CHECK(!second.running());
    // The message a user sees has to name the port and say what to look for,
    // not print a bare error number.
    CHECK(secondError.find(std::to_string(port)) != std::string::npos);
    CHECK(secondError.find("rigctld") != std::string::npos);

    // The original server is untouched by the failed attempt.
    CHECK(first.running());
    SockT c = connectTo(port);
    CHECK(c != INVALID_SOCKET);
    if (c != INVALID_SOCKET) {
        CHECK(sendRaw(c, "f\n"));
        CHECK(readLines(c, 1) == "145000000\n");
        closeClient(c);
    }
    first.stop();
}

void testRoguePeerDoesNotDisturbAnotherSession() {
    resetStatus();
    CatServer server;
    server.setSessionOpener(opener());
    std::string error;
    const int port = startOnFreePort(server, error);
    if (port < 0) { std::printf("SKIP: no free port\n"); return; }

    SockT good = connectTo(port);
    CHECK(good != INVALID_SOCKET);
    if (good == INVALID_SOCKET) { server.stop(); return; }
    CHECK(sendRaw(good, "f\n"));
    CHECK(readLines(good, 1) == "145000000\n");

    // A second peer arrives mid-session and behaves like nothing this protocol
    // expects: binary rubbish, an out-of-range set, then an abrupt hang-up in
    // the middle of an unterminated line. Anything on a network port has to
    // survive this, and it must not leak into the healthy session.
    SockT rogue = connectTo(port);
    CHECK(rogue != INVALID_SOCKET);
    if (rogue != INVALID_SOCKET) {
        std::string garbage(std::string("\x01\x02\xfe\xff", 4) + "\r\n");
        garbage += "F 999999999999999\n";       // far outside any tuning range
        garbage += std::string("\x03\x04", 2) + " unterminated";
        CHECK(sendRaw(rogue, garbage));
        const std::string rogueReply = readLines(rogue, 2);
        // Both complete lines are refused; neither is obeyed.
        CHECK(rogueReply.find("RPRT 0") == std::string::npos);
        CHECK(rogueReply.find("RPRT -") != std::string::npos);
        closeClient(rogue);  // hang up mid-fragment
    }

    // The healthy session carries on, and nothing the rogue sent was handed
    // to the engine to apply.
    CHECK(sendRaw(good, "m\n"));
    CHECK(readLines(good, 2) == "FM\n12500\n");
    CHECK(takeSubmitted().empty());
    CHECK(server.running());

    closeClient(good);
    server.stop();
}

void testStopIsIdempotentAndSafeWithNoClients() {
    resetStatus();
    CatServer server;
    server.setSessionOpener(opener());
    std::string error;
    const int port = startOnFreePort(server, error);
    if (port < 0) { std::printf("SKIP: no free port\n"); return; }
    CHECK(server.running());
    server.stop();
    CHECK(!server.running());
    server.stop();  // a second stop must not deadlock or crash
    CHECK(!server.running());
}

}  // namespace

int main() {
    testRefusesToStartWithoutAProvider();
    testRoundTripOverARealSocket();
    testCommandSplitAcrossSegmentsIsReassembled();
    testSeveralCommandsInOneSegment();
    testQuitClosesTheConnection();
    testOverlongLineIsRefusedRatherThanBuffered();
    testIdleConnectionSurvives();
    testConnectionCap();
    testResultsAreCollectedAsItGoes();
    testASessionThatTakesNothingIsSurvived();
    testEveryConnectionHoldsASessionAndClosesIt();
    testAConnectionDrainsBeforeItsSessionCloses();
    testStopClosesLiveConnections();
    testSecondBindOfTheSamePortIsRefused();
    testRoguePeerDoesNotDisturbAnotherSession();
    testStopIsIdempotentAndSafeWithNoClients();
    return testSummary("test_cat_server");
}
