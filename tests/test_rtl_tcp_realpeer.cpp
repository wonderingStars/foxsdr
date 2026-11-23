// test_rtl_tcp_realpeer.cpp - RtlTcpSource against a REAL rtl_tcp server process and a real dongle.
//
// SKIPS (passes, saying so) unless FOXSDR_RTLTCP_REAL_EXE names an rtl_tcp executable. It is never part of
// an unattended run: it needs a dongle that nothing else holds, and it takes minutes. The server is bound
// to 127.0.0.1 only, on an ephemeral-looking scratch port, and is killed at the end of every phase.
// FOXSDR_RTLTCP_SECONDS (default 20) is how long each rate is measured. Receive only: nothing here
// transmits; the radio is tuned to 98 MHz (FM broadcast).
//
// Phases: A) 2.0 MS/s, B) 2.048 MS/s, C) rate change on a running stream, D) server killed mid-stream
// (what the source reports, how fast), E) server started AFTER the first open attempt (the refused text,
// then a good open), F) cold accept-to-header time of the real server (what the 2 s header wait faces).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include "source/rtl_tcp_source.hpp"

using cascade::source::RtlTcpSource;
using Clock = std::chrono::steady_clock;

namespace {
int g_failures = 0;
#define CHECK(c) do { if (!(c)) { ++g_failures; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

struct Server {
    PROCESS_INFORMATION pi{};
    bool start(const std::string& exe, int port) {
        std::string cmd = "\"" + exe + "\" -a 127.0.0.1 -p " + std::to_string(port) + " -f 98000000";
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        std::vector<char> buf(cmd.begin(), cmd.end());
        buf.push_back('\0');
        return CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) != 0;
    }
    void kill() {
        if (pi.hProcess != nullptr) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            pi = {};
        }
    }
};

// open with retries for the server's own start-up (the process needs a moment to bind)
bool openWhenUp(RtlTcpSource& s, const std::string& args, int seconds, double& openMs) {
    const auto t0 = Clock::now();
    while (std::chrono::duration<double>(Clock::now() - t0).count() < seconds) {
        const auto a = Clock::now();
        if (s.open(args)) {
            openMs = std::chrono::duration<double, std::milli>(Clock::now() - a).count();
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return false;
}

// samples per second delivered over `sec` seconds
double measure(RtlTcpSource& s, int sec) {
    std::vector<std::complex<float>> buf(65536);
    std::size_t total = 0;
    const auto t0 = Clock::now();
    while (std::chrono::duration<double>(Clock::now() - t0).count() < sec) {
        total += s.read(buf.data(), buf.size());
        if (s.faulted()) { break; }
    }
    const double el = std::chrono::duration<double>(Clock::now() - t0).count();
    return static_cast<double>(total) / el;
}
}  // namespace

int main() {
    const char* exe = std::getenv("FOXSDR_RTLTCP_REAL_EXE");
    if (exe == nullptr || *exe == '\0') {
        std::printf("test_rtl_tcp_realpeer: SKIPPED (FOXSDR_RTLTCP_REAL_EXE not set)\n");
        return 0;
    }
    const char* secEnv = std::getenv("FOXSDR_RTLTCP_SECONDS");
    const int sec = secEnv != nullptr ? std::atoi(secEnv) : 20;
    const int port = 41234;
    const std::string args = "rtltcp=127.0.0.1:" + std::to_string(port);

    // A and B: each rate, one server, one open
    for (const double rate : {2.0e6, 2048000.0}) {
        Server srv;
        CHECK(srv.start(exe, port));
        RtlTcpSource s;
        double openMs = 0;
        CHECK(openWhenUp(s, args, 15, openMs));
        std::printf("[rate %.0f] open ok in %.0f ms; tuner %s, %u gain steps\n", rate, openMs, s.tunerName().c_str(), s.tunerGainCount());
        CHECK(s.setSampleRateHz(rate));
        CHECK(s.setCenterFrequencyHz(98.0e6));
        CHECK(s.start());
        const double sps = measure(s, sec);
        std::printf("[rate %.0f] %.0f samples/s over %d s (asked %.0f, dropped %llu), faulted=%d\n", rate, sps, sec, s.sampleRateHz(),
                    static_cast<unsigned long long>(s.droppedBuffers()), s.faulted() ? 1 : 0);
        CHECK(sps > rate * 0.97 && sps < rate * 1.03);
        CHECK(!s.faulted());

        if (rate == 2.0e6) {
            // C: rate change on the running stream
            for (const double r2 : {2048000.0, 1024000.0, 2.0e6}) {
                CHECK(s.setSampleRateHz(r2));
                measure(s, 1);  // drain what the ring held at the old rate
                const double m = measure(s, 5);
                std::printf("[rate change] -> %.0f: %.0f samples/s\n", r2, m);
                CHECK(m > r2 * 0.95 && m < r2 * 1.05);
            }
            // D: server killed mid-stream
            const auto k0 = Clock::now();
            srv.kill();
            while (!s.faulted() && std::chrono::duration<double>(Clock::now() - k0).count() < 15) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - k0).count();
            std::printf("[server killed] faulted=%d after %.0f ms; text: %s\n", s.faulted() ? 1 : 0, ms, s.lastError());
            CHECK(s.faulted());
        }
        s.closeDevice();
        srv.kill();
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));  // let the dongle go
    }

    // E: FoxSDR first, server after
    {
        RtlTcpSource early;
        const auto a = Clock::now();
        const bool ok = early.open(args);
        std::printf("[server absent] open=%d after %.0f ms; text: %s\n", ok ? 1 : 0, std::chrono::duration<double, std::milli>(Clock::now() - a).count(), early.lastError());
        CHECK(!ok);
        Server srv;
        CHECK(srv.start(exe, port));
        RtlTcpSource late;
        double openMs = 0;
        CHECK(openWhenUp(late, args, 15, openMs));
        std::printf("[server started after] second open ok\n");
        late.closeDevice();
        srv.kill();
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    }

    // G: close and open again at once (a user re-picking the radio, a rate restart, a retry): the real
    // server is still tearing down the previous connection and may not send its header for seconds.
    {
        Server srv;
        CHECK(srv.start(exe, port));
        RtlTcpSource first;
        double openMs = 0;
        CHECK(openWhenUp(first, args, 15, openMs));
        CHECK(first.start());
        measure(first, 2);
        first.closeDevice();
        for (int i = 0; i < 3; ++i) {
            RtlTcpSource again;
            const auto a = Clock::now();
            const bool ok = again.open(args);
            std::printf("[reopen %d right after close] open=%d after %.0f ms%s%s\n", i, ok ? 1 : 0, std::chrono::duration<double, std::milli>(Clock::now() - a).count(),
                        ok ? "" : "; text: ", ok ? "" : again.lastError());
            CHECK(ok);
            if (ok) { CHECK(again.start()); measure(again, 1); }
            again.closeDevice();
        }
        srv.kill();
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    }

    // F: cold accept-to-header, measured on a raw socket
    {
        Server srv;
        CHECK(srv.start(exe, port));
        WSADATA d{};
        WSAStartup(MAKEWORD(2, 2), &d);
        for (int i = 0; i < 3; ++i) {
            SOCKET so = INVALID_SOCKET;
            const auto t0 = Clock::now();
            for (int tries = 0; tries < 100; ++tries) {
                so = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                sockaddr_in a{};
                a.sin_family = AF_INET;
                a.sin_port = htons(static_cast<u_short>(port));
                inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
                if (connect(so, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) { break; }
                closesocket(so);
                so = INVALID_SOCKET;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (so == INVALID_SOCKET) { CHECK(false); break; }
            const auto conn = Clock::now();
            char hdr[12];
            int got = 0;
            while (got < 12) {
                const int r = recv(so, hdr + got, 12 - got, 0);
                if (r <= 0) { break; }
                got += r;
            }
            std::printf("[header] connection %d: server up->connected %.0f ms, connected->12-byte header %.0f ms (got %d bytes)\n", i,
                        std::chrono::duration<double, std::milli>(conn - t0).count(), std::chrono::duration<double, std::milli>(Clock::now() - conn).count(), got);
            CHECK(got == 12);
            closesocket(so);
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));  // the server serves one client at a time and re-accepts
        }
        srv.kill();
    }
    std::printf("test_rtl_tcp_realpeer: %d failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
