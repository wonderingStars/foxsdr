// test_cat_emulation.cpp - the application answering as a Kenwood TS-480 and a Yaesu FT-891 (net/cat_emulation.hpp), receive only.
//
//   [A] Kenwood TS-480, command by command against hand-written expected bytes taken from the maker's reference: ID, FA, FB, FR, MD, SM, IF, PS,
//       AI, RX, the transmit commands that are refused, syntax errors, an over-long command, a command split across reads, wake-up dummy data.
//   [B] Yaesu FT-891 the same way: ID, FA, FB, SV, AB, BA, MD, SM, IF, PS, AI, TX, ST, KY.
//   [C] receive only: after every transmit command of both dialects the receiver was not touched and the counters say so.
//   [D] fuzz: random bytes into both dialects; never a crash, every reply is "?;" / "E;" or a well-formed answer.
//   [E] over an abstract byte stream (serveCatStream): chunks, a stream that ends, a failing write, the stop flag.
//   [F] the serial service over a named pipe opened by the same SerialPort class (the test seam core/serial_port.hpp documents), and its REFUSAL
//       path on real ports: an absent COM port, no name, a baud it does not offer, a typed path (never echoed), already running.
//   [G] the TCP service: 127.0.0.1 only unless asked (a connection to this machine's other address is refused), two clients sharing the VFOs, the
//       client limit, a port in use, a hard stop that closes clients.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "net/cat_emulation.hpp"
#include "test_check.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif

using namespace cascade::net;
using Clock = std::chrono::steady_clock;

namespace {

struct FakeRig {
    double freq = 7050000.0;
    std::string mode = "USB";
    double signal = 0.5;
    bool running = true;
    double minHz = 30000.0, maxHz = 2.0e9;
    int freqSets = 0, modeSets = 0;
    RigBackend backend() {
        RigBackend b;
        b.frequencyHz = [this] { return freq; };
        b.setFrequencyHz = [this](double hz) { if (hz < minHz || hz > maxHz) { return false; } freq = hz; ++freqSets; return true; };
        b.mode = [this] { return mode; };
        b.setMode = [this](const std::string& m) { mode = m; ++modeSets; return true; };
        b.signalFraction = [this] { return signal; };
        b.running = [this] { return running; };
        return b;
    }
};

std::string ask(CatEmulator& e, const std::string& s) { return e.feed(s); }

void testKenwood() {
    FakeRig r;
    CatEmulator e(CatDialect::KenwoodTS480, r.backend());
    CHECK(ask(e, "ID;") == "ID020;");
    CHECK(ask(e, "FA;") == "FA00007050000;");
    CHECK(ask(e, "FA00014200000;").empty() && r.freq == 14200000.0 && r.freqSets == 1);
    CHECK(ask(e, "FA;") == "FA00014200000;");
    CHECK(ask(e, "fa;") == "FA00014200000;");                                    // lower case is allowed
    CHECK(ask(e, "FA;MD;SM0;") == "FA00014200000;MD2;SM00010;");                  // three commands in one read
    CHECK(ask(e, "F").empty() && ask(e, "A") .empty() && ask(e, ";") == "FA00014200000;");   // one command in three reads
    CHECK(ask(e, "ID;\r\nID;\r\n") == "ID020;ID020;");                             // line ends between commands are not commands
    CHECK(ask(e, "; ; ; ;ID;") == "ID020;");                                       // wake-up dummy data (the manual's "; ; ; ; PS1;")
    // VFO B and the receive VFO
    CHECK(ask(e, "FB00021000000;").empty() && r.freq == 14200000.0);               // B is not the receive VFO: remembered, nothing tuned
    CHECK(ask(e, "FB;") == "FB00021000000;" && ask(e, "FR;") == "FR0;");
    CHECK(ask(e, "FR1;").empty() && r.freq == 21000000.0 && ask(e, "FR;") == "FR1;");   // select B: the receiver goes to B's frequency
    CHECK(ask(e, "FA;") == "FA00014200000;" && ask(e, "FB;") == "FB00021000000;");   // A kept its frequency, B is the receiver's
    CHECK(ask(e, "FB00021100000;").empty() && r.freq == 21100000.0);                  // setting the receive VFO tunes
    CHECK(ask(e, "FR0;").empty() && r.freq == 14200000.0);
    CHECK(ask(e, "FR2;") == "E;" && ask(e, "FR;") == "FR0;");                          // memory channels: none
    CHECK(ask(e, "FR9;") == "E;" && ask(e, "FR;") == "FR0;");
    CHECK(ask(e, "FT1;").empty() && ask(e, "FT;") == "FT0;");                          // the transmit VFO is accepted and ignored
    // frequency errors
    const int sets = r.freqSets;
    CHECK(ask(e, "FA00000000001;") == "E;" && r.freq == 14200000.0 && r.freqSets == sets);   // the receiver refused: "not executed"
    CHECK(ask(e, "FA1420000;") == "?;" && ask(e, "FA0001420000X;") == "?;" && ask(e, "FA000142000000;") == "?;" && ask(e, "FAabcdefghijk;") == "?;");
    CHECK(ask(e, "FA-0014200000;") == "?;");
    // modes
    r.mode = "USB";
    CHECK(ask(e, "MD1;").empty() && r.mode == "LSB" && ask(e, "MD;") == "MD1;");
    CHECK(ask(e, "MD2;").empty() && r.mode == "USB");
    CHECK(ask(e, "MD3;").empty() && r.mode == "CW" && ask(e, "MD;") == "MD3;");
    CHECK(ask(e, "MD4;").empty() && r.mode == "NFM" && ask(e, "MD;") == "MD4;");
    CHECK(ask(e, "MD5;").empty() && r.mode == "AM" && ask(e, "MD;") == "MD5;");
    const int modeSets = r.modeSets;
    CHECK(ask(e, "MD6;") == "E;" && ask(e, "MD7;") == "E;" && ask(e, "MD9;") == "E;" && ask(e, "MD0;") == "E;" && ask(e, "MD8;") == "E;" && r.mode == "AM" && r.modeSets == modeSets);
    CHECK(ask(e, "MD;") == "MD5;" && ask(e, "MDX;") == "?;" && ask(e, "MD12;") == "?;");
    r.mode = "WFM";
    CHECK(ask(e, "MD;") == "MD4;");
    r.mode = "DSB";
    CHECK(ask(e, "MD;") == "MD5;");
    r.mode = "RAW";
    CHECK(ask(e, "MD;") == "MD2;");
    r.mode = "USB";
    // signal strength
    CHECK(ask(e, "SM0;") == "SM00010;");
    r.signal = 0.0;
    CHECK(ask(e, "SM0;") == "SM00000;");
    r.signal = 1.0;
    CHECK(ask(e, "SM0;") == "SM00020;");
    r.signal = 7.0;
    CHECK(ask(e, "SM0;") == "SM00020;");   // clamped
    r.signal = std::nan("");
    CHECK(ask(e, "SM0;") == "SM00000;");
    CHECK(ask(e, "SM;") == "?;" && ask(e, "SM1;") == "?;");
    // IF: 38 characters, laid out as the manual's table
    r.freq = 7050000.0;
    r.mode = "USB";
    {
        const std::string expect = std::string("IF") + "00007050000" + "     " + "+0000" + "0" + "0" + "0" + "00" + "0" + "2" + "0" + "0" + "0" + "0" + "00" + " " + ";";
        const std::string got = ask(e, "IF;");
        CHECK(got == expect && got.size() == 38);
    }
    CHECK(ask(e, "FR1;").empty());   // VFO B becomes the receive VFO (tuned to B's 21.1 MHz)
    CHECK(ask(e, "IF;").substr(2, 11) == "00021000000" || ask(e, "IF;").substr(2, 11) == "00021100000");
    CHECK(ask(e, "IF;")[30] == '1');   // P10: the VFO
    CHECK(ask(e, "FR0;").empty());
    // power, auto-information, receive
    CHECK(ask(e, "PS;") == "PS1;");
    r.running = false;
    CHECK(ask(e, "PS;") == "PS0;");
    r.running = true;
    CHECK(ask(e, "PS1;").empty() && ask(e, "PS0;") == "E;" && ask(e, "PS9;") == "E;" && r.running && ask(e, "PS;") == "PS1;");
    CHECK(ask(e, "AI;") == "AI0;" && ask(e, "AI2;").empty() && ask(e, "AI;") == "AI0;" && ask(e, "AI9;") == "?;");
    CHECK(ask(e, "RX;").empty() && ask(e, "RX0;").empty() && ask(e, "RX5;") == "?;");
    // not understood
    CHECK(ask(e, "ZZ;") == "?;" && ask(e, "ID1;") == "?;" && ask(e, "X;") == "?;" && ask(e, "\xC3\xA9\xC3\xA9;") == "?;" && ask(e, "AG0;") == "?;");
    // an over-long command: one "?;" and then it is skipped up to its terminator, and the next command works
    const std::string longCmd(200, 'A');
    CHECK(ask(e, longCmd) == "?;");
    CHECK(ask(e, std::string(300, 'B') + ";ID;") == "ID020;");
    CHECK(ask(e, "ID;") == "ID020;");
    const auto st = e.stats();
    CHECK(st.commands > 50 && st.notUnderstood >= 10 && st.setsApplied >= 8);
}

void testYaesu() {
    FakeRig r;
    CatEmulator e(CatDialect::YaesuFT891, r.backend());
    CHECK(ask(e, "ID;") == "ID0650;");
    CHECK(ask(e, "FA;") == "FA007050000;");
    CHECK(ask(e, "FA014250000;").empty() && r.freq == 14250000.0);
    CHECK(ask(e, "FA;") == "FA014250000;" && ask(e, "fa;") == "FA014250000;");
    CHECK(ask(e, "FA14250000;") == "?;" && ask(e, "FA0142500000;") == "?;" && ask(e, "FA000029999;") == "?;" && ask(e, "FA00000000A;") == "?;");
    r.maxHz = 100e6;
    CHECK(ask(e, "FA500000000;") == "?;" && r.freq == 14250000.0);   // the receiver refused: "?;"
    r.maxHz = 2e9;
    CHECK(ask(e, "FB021000000;").empty() && r.freq == 14250000.0 && ask(e, "FB;") == "FB021000000;");
    CHECK(ask(e, "SV;").empty() && r.freq == 21000000.0 && ask(e, "FB;") == "FB014250000;");   // swap: the receiver goes to B, B takes A's
    CHECK(ask(e, "SV;").empty() && r.freq == 14250000.0 && ask(e, "FB;") == "FB021000000;");
    CHECK(ask(e, "AB;").empty() && ask(e, "FB;") == "FB014250000;");                            // A copied to B
    CHECK(ask(e, "FB028000000;").empty() && ask(e, "BA;").empty() && r.freq == 28000000.0);     // B copied to A
    CHECK(ask(e, "SV1;") == "?;");
    // modes
    r.mode = "USB";
    CHECK(ask(e, "MD0;") == "MD02;");
    CHECK(ask(e, "MD01;").empty() && r.mode == "LSB" && ask(e, "MD0;") == "MD01;");
    CHECK(ask(e, "MD0C;").empty() && r.mode == "USB");
    CHECK(ask(e, "MD03;").empty() && r.mode == "CW" && ask(e, "MD0;") == "MD03;");
    CHECK(ask(e, "MD04;").empty() && r.mode == "NFM");
    CHECK(ask(e, "MD0B;").empty() && r.mode == "NFM");
    CHECK(ask(e, "MD05;").empty() && r.mode == "AM" && ask(e, "MD0D;").empty() && r.mode == "AM");
    CHECK(ask(e, "MD08;").empty() && r.mode == "LSB");
    CHECK(ask(e, "md02;").empty() && r.mode == "USB");
    const int modeSets = r.modeSets;
    CHECK(ask(e, "MD06;") == "?;" && ask(e, "MD07;") == "?;" && ask(e, "MD09;") == "?;" && ask(e, "MD0E;") == "?;" && ask(e, "MD1;") == "?;" && ask(e, "MD02X;") == "?;" &&
          r.modeSets == modeSets);
    // signal strength: 000 to 255
    r.signal = 0.5;
    CHECK(ask(e, "SM0;") == "SM0128;");
    r.signal = 0.0;
    CHECK(ask(e, "SM0;") == "SM0000;");
    r.signal = 1.0;
    CHECK(ask(e, "SM0;") == "SM0255;");
    CHECK(ask(e, "SM;") == "?;" && ask(e, "SM1;") == "?;");
    // IF: 28 characters
    r.freq = 7050000.0;
    r.mode = "USB";
    {
        const std::string expect = std::string("IF") + "001" + "007050000" + "+0000" + "0" + "0" + "2" + "0" + "0" + "00" + "0" + ";";
        const std::string got = ask(e, "IF;");
        CHECK(got == expect && got.size() == 28);
    }
    // a frequency above what nine digits can say is clamped, never wrapped
    r.freq = 1.5e9;
    CHECK(ask(e, "FA;") == "FA999999999;");
    r.freq = 7050000.0;
    // power, auto-information
    CHECK(ask(e, "PS;") == "PS1;" && ask(e, "PS1;").empty() && ask(e, "PS0;") == "?;" && r.running);
    CHECK(ask(e, "AI;") == "AI0;" && ask(e, "AI1;").empty() && ask(e, "AI3;") == "?;");
    // transmit
    CHECK(ask(e, "TX;") == "TX0;" && ask(e, "TX0;").empty());
    CHECK(ask(e, "TX1;") == "?;" && ask(e, "TX2;") == "?;");
    CHECK(ask(e, "ST;") == "ST0;" && ask(e, "ST0;").empty() && ask(e, "ST1;") == "?;" && ask(e, "ST2;") == "?;");
    CHECK(ask(e, "KY2ABC;") == "?;" && ask(e, "MX1;") == "?;");
    CHECK(ask(e, "ZZ;") == "?;" && ask(e, "ID1;") == "?;");
    CHECK(ask(e, "ID;\r\n; ; ID;") == "ID0650;ID0650;");
}

void testReceiveOnly() {
    for (CatDialect d : {CatDialect::KenwoodTS480, CatDialect::YaesuFT891}) {
        FakeRig r;
        CatEmulator e(d, r.backend());
        const std::vector<std::string> tx = d == CatDialect::KenwoodTS480
                                                ? std::vector<std::string>{"TX;", "TX0;", "TX1;", "TX2;", "KY ABC;", "KYabc;", "AC111;", "AC;", "SR1;", "SR2;"}
                                                : std::vector<std::string>{"TX1;", "TX2;", "ST1;", "ST2;", "KY2ABC;", "MX1;"};
        for (const auto& c : tx) {
            const std::string reply = e.feed(c);
            CHECK(reply == "E;" || reply == "?;");   // refused, in the radio's own words
        }
        CHECK(r.freqSets == 0 && r.modeSets == 0 && r.freq == 7050000.0 && r.running);   // nothing reached the receiver
        CHECK(e.stats().transmitRefused >= (d == CatDialect::KenwoodTS480 ? 8u : 6u));
        // the same commands in lower case and mixed with reads are still refused
        CHECK(e.feed("tx1;FA;") == (d == CatDialect::KenwoodTS480 ? "E;FA00007050000;" : "?;FA007050000;"));
    }
    // a radio with no callbacks at all: reads answer, sets are refused, nothing is called through a null
    for (CatDialect d : {CatDialect::KenwoodTS480, CatDialect::YaesuFT891}) {
        CatEmulator e(d, RigBackend{});
        CHECK(!e.feed("FA;MD;SM0;IF;PS;ID;").empty());
        const std::string kw = e.feed(d == CatDialect::KenwoodTS480 ? "FA00007000000;MD2;" : "FA007000000;MD02;");
        CHECK(kw == (d == CatDialect::KenwoodTS480 ? "E;E;" : "?;?;"));
    }
}

void testFuzz() {
    std::mt19937 rng(8642);
    for (CatDialect d : {CatDialect::KenwoodTS480, CatDialect::YaesuFT891}) {
        FakeRig r;
        CatEmulator e(d, r.backend());
        const std::vector<std::string> seeds = {"FA00007050000;", "FA007050000;", "MD2;", "MD02;", "SM0;", "IF;", "FR1;", "SV;", "TX1;", "PS0;", "AI2;", "ID;"};
        std::size_t bytesIn = 0, bytesOut = 0;
        for (int i = 0; i < 40000; ++i) {
            std::string in;
            if (i % 3 == 0) {
                const std::size_t n = rng() % 120;
                for (std::size_t k = 0; k < n; ++k) { in += static_cast<char>(rng()); }
            } else {
                in = seeds[rng() % seeds.size()];
                for (int f = 0, m = static_cast<int>(rng() % 4); f < m && !in.empty(); ++f) { in[rng() % in.size()] = static_cast<char>(rng()); }
            }
            bytesIn += in.size();
            const std::string out = e.feed(in);
            bytesOut += out.size();
            // every reply is a sequence of ';'-terminated answers, nothing else
            if (!out.empty() && out.back() != ';') { CHECK(false); }
        }
        CHECK(bytesIn > 0 && bytesOut > 0);
        CHECK(r.freq >= r.minHz && r.freq <= r.maxHz);   // whatever was fuzzed, the receiver only ever saw legal frequencies
        CHECK(r.running);
    }
}

// ---- [E] -----------------------------------------------------------------------------------------------------------------------------------
class Scripted final : public CatStream {
public:
    std::vector<std::string> chunks;
    std::size_t next = 0;
    std::string written;
    int failWriteAfter = -1;
    int read(char* buf, std::size_t cap) override {
        if (next >= chunks.size()) { return -1; }
        const std::string& c = chunks[next++];
        if (c.empty()) { return 0; }   // a timeout
        const std::size_t n = std::min(cap, c.size());
        std::memcpy(buf, c.data(), n);
        return static_cast<int>(n);
    }
    int write(const char* d, std::size_t n) override {
        if (failWriteAfter >= 0 && static_cast<int>(written.size()) >= failWriteAfter) { return -1; }
        written.append(d, n);
        return static_cast<int>(n);
    }
};

void testStream() {
    {
        FakeRig r;
        CatEmulator e(CatDialect::KenwoodTS480, r.backend());
        Scripted s;
        s.chunks = {"I", "", "D;F", "A0001", "4200000;FA;", ""};
        std::atomic<bool> stop{false};
        const auto n = serveCatStream(s, e, stop);
        CHECK(s.written == "ID020;FA00014200000;" && n == 3 && r.freq == 14200000.0);
    }
    {   // a failing write ends the session
        FakeRig r;
        CatEmulator e(CatDialect::KenwoodTS480, r.backend());
        Scripted s;
        s.chunks = {"ID;", "ID;", "ID;"};
        s.failWriteAfter = 1;
        std::atomic<bool> stop{false};
        serveCatStream(s, e, stop);
        CHECK(s.next == 2);
    }
    {   // the stop flag, set before the first read
        FakeRig r;
        CatEmulator e(CatDialect::YaesuFT891, r.backend());
        Scripted s;
        s.chunks = {"ID;"};
        std::atomic<bool> stop{true};
        CHECK(serveCatStream(s, e, stop) == 0 && s.written.empty());
    }
}

#if defined(_WIN32)

// ---- [F] -----------------------------------------------------------------------------------------------------------------------------------
std::string pipeName() { return "\\\\.\\pipe\\foxsdr-test-cat-" + std::to_string(::GetCurrentProcessId()); }

bool pipeRead(HANDLE h, std::string& out, std::size_t want, int ms) {
    const auto end = Clock::now() + std::chrono::milliseconds(ms);
    while (out.size() < want && Clock::now() < end) {
        DWORD avail = 0;
        if (!::PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) { return false; }
        if (avail == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(3)); continue; }
        char buf[256];
        DWORD got = 0;
        if (!::ReadFile(h, buf, std::min<DWORD>(avail, sizeof buf), &got, nullptr) || got == 0) { return false; }
        out.append(buf, got);
    }
    return out.size() >= want;
}

void testSerial() {
    FakeRig r;
    // an absent real COM port, no name, a baud that is not offered, a typed path
    {
        CatSerialService s(CatDialect::KenwoodTS480, r.backend());
        CatEmuFact f;
        CHECK(!s.start("COM98", 9600, f) && f.code == "cat-port-refused" && f.text.find("COM98") != std::string::npos && !s.running());
        CHECK(!s.start("", 9600, f) && f.code == "cat-port-refused");
        CHECK(!s.start("COM98", 1234, f) && f.code == "cat-port-refused");
        CHECK(!s.start("C:\\Users\\steve\\secret-file.txt", 9600, f) && f.code == "cat-port-refused" && f.text.find("secret") == std::string::npos &&
              f.text.find("Users") == std::string::npos);
        s.stop();   // after refusals: harmless
    }
    // a named pipe stands in for the port
    const std::string name = pipeName();
    HANDLE srv = ::CreateNamedPipeW(std::wstring(name.begin(), name.end()).c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                    1, 65536, 65536, 0, nullptr);
    CHECK(srv != INVALID_HANDLE_VALUE);
    if (srv == INVALID_HANDLE_VALUE) { return; }
    {
        CatSerialService s(CatDialect::KenwoodTS480, r.backend());
        CatEmuFact f;
        CHECK(s.start(name, 9600, f));
        CHECK(s.running());
        CatEmuFact f2;
        CHECK(!s.start(name, 9600, f2) && f2.code == "cat-already-running");
        auto xfer = [&](const std::string& send, std::size_t want) {
            DWORD w = 0;
            ::WriteFile(srv, send.data(), static_cast<DWORD>(send.size()), &w, nullptr);
            std::string got;
            pipeRead(srv, got, want, 3000);
            return got;
        };
        CHECK(xfer("ID;", 6) == "ID020;");
        CHECK(xfer("FA00010000000;FA;", 14) == "FA00010000000;" && r.freq == 10000000.0);
        CHECK(xfer("TX1;", 2) == "E;" && r.freq == 10000000.0);
        const auto t0 = Clock::now();
        s.stop();
        CHECK(std::chrono::duration<double>(Clock::now() - t0).count() < 2.0);   // a pipe that never speaks does not hold stop() up
        CHECK(!s.running());
    }
    ::CloseHandle(srv);
}

// ---- [G] -----------------------------------------------------------------------------------------------------------------------------------
SOCKET connectTo(const char* host, std::uint16_t port, int ms = 1500) {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, host, &a.sin_addr);
    u_long nb = 1;
    ::ioctlsocket(s, FIONBIO, &nb);
    ::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
    fd_set wr, ex;
    FD_ZERO(&wr);
    FD_ZERO(&ex);
    FD_SET(s, &wr);
    FD_SET(s, &ex);
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    const int n = ::select(0, nullptr, &wr, &ex, &tv);
    u_long blocking = 0;
    ::ioctlsocket(s, FIONBIO, &blocking);
    if (n <= 0 || FD_ISSET(s, &ex)) { ::closesocket(s); return INVALID_SOCKET; }
    int err = 0, len = sizeof err;
    ::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
    if (err != 0) { ::closesocket(s); return INVALID_SOCKET; }
    DWORD to = 3000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof to);
    return s;
}
std::string tcpAsk(SOCKET s, const std::string& send, std::size_t want) {
    ::send(s, send.data(), static_cast<int>(send.size()), 0);
    std::string got;
    while (got.size() < want) {
        char buf[256];
        const int n = ::recv(s, buf, sizeof buf, 0);
        if (n <= 0) { break; }
        got.append(buf, static_cast<std::size_t>(n));
    }
    return got;
}
// This machine's first non-loopback IPv4 address, "" when it has none.
std::string otherAddress() {
    char host[256];
    if (::gethostname(host, sizeof host) != 0) { return std::string(); }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host, nullptr, &hints, &res) != 0) { return std::string(); }
    std::string found;
    for (addrinfo* p = res; p != nullptr && found.empty(); p = p->ai_next) {
        char txt[64];
        inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(p->ai_addr)->sin_addr, txt, sizeof txt);
        if (std::strncmp(txt, "127.", 4) != 0 && std::strncmp(txt, "0.", 2) != 0) { found = txt; }
    }
    ::freeaddrinfo(res);
    return found;
}

void testTcp() {
    WSADATA d;
    ::WSAStartup(MAKEWORD(2, 2), &d);
    FakeRig r;
    {
        CatTcpService svc(CatDialect::KenwoodTS480, r.backend());
        CatEmuFact f;
        CHECK(svc.start(0, false, f));
        const std::uint16_t port = svc.boundPort();
        CHECK(port != 0 && svc.running());
        SOCKET a = connectTo("127.0.0.1", port);
        CHECK(a != INVALID_SOCKET);
        if (a != INVALID_SOCKET) {
            CHECK(tcpAsk(a, "ID;FA;", 6 + 14) == "ID020;FA00007050000;");
            CHECK(tcpAsk(a, "TX1;", 2) == "E;");
            // a second client shares the VFOs
            SOCKET b = connectTo("127.0.0.1", port);
            CHECK(b != INVALID_SOCKET);
            if (b != INVALID_SOCKET) {
                CHECK(tcpAsk(a, "FB00021000000;FR1;FR;", 4) == "FR1;");
                CHECK(tcpAsk(b, "FR;", 4) == "FR1;" && tcpAsk(b, "FB;", 14) == "FB00021000000;" && r.freq == 21000000.0);
                // the limit: four clients at once; a fifth is let go
                SOCKET c = connectTo("127.0.0.1", port), d2 = connectTo("127.0.0.1", port);
                CHECK(c != INVALID_SOCKET && d2 != INVALID_SOCKET);
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                CHECK(svc.clients() == 4);
                SOCKET e5 = connectTo("127.0.0.1", port);
                if (e5 != INVALID_SOCKET) {
                    DWORD to = 1000;
                    ::setsockopt(e5, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof to);
                    ::send(e5, "ID;", 3, 0);
                    char buf[16];
                    CHECK(::recv(e5, buf, sizeof buf, 0) <= 0);   // closed, not served
                    ::closesocket(e5);
                }
                if (c != INVALID_SOCKET) { ::closesocket(c); }
                if (d2 != INVALID_SOCKET) { ::closesocket(d2); }
                // a client that leaves frees its place
                ::closesocket(b);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                CHECK(svc.clients() == 1);
            }
            // a port in use is refused with a fact
            CatTcpService dup(CatDialect::KenwoodTS480, r.backend());
            CatEmuFact f2;
            CHECK(!dup.start(port, false, f2) && f2.code == "cat-tcp-refused");
            CatEmuFact f3;
            CHECK(!svc.start(0, false, f3) && f3.code == "cat-already-running");
            // 127.0.0.1 only: this machine's other address is refused
            const std::string other = otherAddress();
            if (!other.empty()) {
                SOCKET x = connectTo(other.c_str(), port, 1500);
                CHECK(x == INVALID_SOCKET);
                if (x != INVALID_SOCKET) { ::closesocket(x); }
            } else {
                std::printf("no non-loopback address on this machine: the loopback-only check was SKIPPED\n");
            }
            // a hard stop closes the client and returns promptly
            const auto t0 = Clock::now();
            svc.stop();
            CHECK(std::chrono::duration<double>(Clock::now() - t0).count() < 3.0);
            char buf[16];
            CHECK(::recv(a, buf, sizeof buf, 0) <= 0);
            CHECK(!svc.running() && svc.boundPort() == 0 && svc.clients() == 0);
            ::closesocket(a);
        }
    }
    {   // all interfaces only when asked
        CatTcpService svc(CatDialect::YaesuFT891, r.backend());
        CatEmuFact f;
        CHECK(svc.start(0, true, f));
        const std::string other = otherAddress();
        if (!other.empty()) {
            SOCKET x = connectTo(other.c_str(), svc.boundPort(), 1500);
            CHECK(x != INVALID_SOCKET);
            if (x != INVALID_SOCKET) { CHECK(tcpAsk(x, "ID;", 7) == "ID0650;"); ::closesocket(x); }
        }
    }
    {   // restart after stop
        CatTcpService svc(CatDialect::YaesuFT891, r.backend());
        CatEmuFact f;
        CHECK(svc.start(0, false, f));
        svc.stop();
        CHECK(svc.start(0, false, f) && svc.boundPort() != 0);
        SOCKET x = connectTo("127.0.0.1", svc.boundPort());
        CHECK(x != INVALID_SOCKET && tcpAsk(x, "ID;", 7) == "ID0650;");
        if (x != INVALID_SOCKET) { ::closesocket(x); }
    }
}

#endif  // _WIN32

}  // namespace

int main() {
    testKenwood();
    testYaesu();
    testReceiveOnly();
    testFuzz();
    testStream();
#if defined(_WIN32)
    testSerial();
    testTcp();
#endif
    return testSummary("test_cat_emulation");
}
