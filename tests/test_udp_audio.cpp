// test_udp_audio.cpp - "AUDIO OVER THE NETWORK" (net/udp_audio.*): raw 48 kHz 16-bit little-endian mono PCM in UDP datagrams, on loopback.
//
//   U1  THE PAYLOAD: little-endian int16, full scale, clipping, NaN, the empty case.
//   U2  THE STREAM on a real UDP socket: every datagram is exactly the packet size (480 or 960 samples), the samples arrive in order with none
//       missing (the generator is a counter, so a lost or reordered sample shows), and a tone in the payload is at its frequency.
//   U3  REFUSALS in words: an empty host, one that cannot be found, a bad port, a bad packet size, no reader, a second start.
//   U4  A radio that goes away (the reader answers negative) sends nothing and counts it; when it returns the stream continues.
//   U5  A destination nobody listens at does not stop, block or crash the sender; stop() returns at once; it can start again.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>

#include <ws2tcpip.h>
using SockT = SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SockT = int;
constexpr SockT INVALID_SOCKET = -1;
#endif

#include "net/udp_audio.hpp"
#include "test_check.hpp"

using namespace cascade::net;
using Clock = std::chrono::steady_clock;

namespace {

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
void closeSock(SockT s) {
#if defined(_WIN32)
    if (s != INVALID_SOCKET) { ::closesocket(s); }
#else
    if (s != INVALID_SOCKET) { ::close(s); }
#endif
}

struct Receiver {
    SockT s = INVALID_SOCKET;
    int port = 0;
    Receiver() {
        sockInit();
        s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
        socklen_t len = sizeof(a);
        ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
        port = ntohs(a.sin_port);
        int big = 1 << 20;
        ::setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&big), sizeof(big));
#if defined(_WIN32)
        DWORD tv = 100;
        ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
        timeval tv{0, 100000};
        ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    }
    ~Receiver() { closeSock(s); }
    // Collects datagrams for `ms`.
    std::vector<std::string> collect(int ms) {
        std::vector<std::string> out;
        const auto end = Clock::now() + std::chrono::milliseconds(ms);
        char buf[4096];
        while (Clock::now() < end) {
            const int n = static_cast<int>(::recv(s, buf, sizeof(buf), 0));
            if (n > 0) { out.emplace_back(buf, static_cast<std::size_t>(n)); }
        }
        return out;
    }
};

// A generator that is a pure function of the sample index, so what arrives can be compared exactly.
float tone(std::uint64_t i) { return 0.5f * std::sin(2.0f * 3.14159265f * 1000.0f * static_cast<float>(i % 48000) / 48000.0f); }

UdpAudioSender::Reader generator(std::atomic<std::uint64_t>* produced, std::atomic<bool>* radioPresent = nullptr, std::size_t perCall = 960) {
    return [produced, radioPresent, perCall](std::uint64_t& cursor, float* out, std::size_t cap) -> int {
        if (radioPresent != nullptr && !radioPresent->load()) { return -3; }   // FOXAPI_NO_DEVICE
        const std::size_t n = std::min(cap, perCall);
        for (std::size_t i = 0; i < n; ++i) { out[i] = tone(cursor + i); }
        cursor += n;
        produced->fetch_add(n);
        return static_cast<int>(n);
    };
}

std::int16_t sampleAt(const std::string& pkt, std::size_t i) {
    return static_cast<std::int16_t>(static_cast<unsigned char>(pkt[2 * i]) | (static_cast<unsigned char>(pkt[2 * i + 1]) << 8));
}

void testPayload() {
    const float v[] = {0.0f, 1.0f, -1.0f, 0.5f, 2.0f, -9.0f, std::nanf("")};
    const std::string p = udpAudioPayload(v, 7);
    CHECK(p.size() == 14);
    CHECK(sampleAt(p, 0) == 0 && sampleAt(p, 1) == 32767 && sampleAt(p, 2) == -32767 && sampleAt(p, 3) == 16384 || sampleAt(p, 3) == 16383);
    CHECK(sampleAt(p, 4) == 32767 && sampleAt(p, 5) == -32767 && sampleAt(p, 6) == 0);
    CHECK(static_cast<unsigned char>(p[2]) == 0xFF && static_cast<unsigned char>(p[3]) == 0x7F);   // 32767 little-endian
    CHECK(udpAudioPayload(v, 0).empty());
}

void testStream(int packetFrames) {
    Receiver rx;
    std::atomic<std::uint64_t> produced{0};
    UdpAudioSender tx;
    std::string err;
    CHECK(tx.start("127.0.0.1", rx.port, packetFrames, generator(&produced), err));
    CHECK(tx.running());
    const std::vector<std::string> got = rx.collect(1200);
    tx.stop();
    CHECK(!tx.running());
    CHECK(got.size() > 20);
    // exactly the packet size, in order, none missing: the n-th packet holds samples [n*P, (n+1)*P)
    std::size_t badSize = 0, badSample = 0;
    std::uint64_t index = 0;
    std::string all;
    for (const std::string& pkt : got) {
        if (pkt.size() != static_cast<std::size_t>(packetFrames) * 2) { ++badSize; continue; }
        for (int i = 0; i < packetFrames; ++i, ++index) {
            const float e = std::max(-1.0f, std::min(1.0f, tone(index)));
            const int want = static_cast<int>(std::lround(e * 32767.0));
            if (std::abs(sampleAt(pkt, static_cast<std::size_t>(i)) - want) > 1) { ++badSample; }
        }
        all += pkt;
    }
    CHECK(badSize == 0);
    CHECK(badSample == 0);
    CHECK(tx.packetsSent() >= got.size());
    CHECK(tx.bytesSent() >= all.size());
    // the tone is where it should be: a Goertzel at 1 kHz against 2 kHz and 5 kHz
    const std::size_t n = all.size() / 2;
    const auto power = [&](double hz) {
        double c = 0, s = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const double x = sampleAt(all, i) / 32768.0, w = 2.0 * 3.14159265358979 * hz * static_cast<double>(i) / 48000.0;
            c += x * std::cos(w);
            s += x * std::sin(w);
        }
        return (c * c + s * s) / (static_cast<double>(n) * static_cast<double>(n));
    };
    const double p1 = power(1000.0), p2 = power(2000.0), p5 = power(5000.0);
    CHECK(p1 > 0.060 && p1 < 0.065);          // an amplitude-0.5 tone has |X|^2 = (0.5/2)^2 = 0.0625
    CHECK(p1 > 100.0 * p2 && p1 > 100.0 * p5);
    std::printf("udp %d-sample packets: %zu datagrams received, 0 wrong sizes, 0 wrong samples; tone power at 1 kHz %.4f, 2 kHz %.6f, 5 kHz %.6f\n",
                packetFrames, got.size(), p1, p2, p5);
}

void testRefusals() {
    std::string err;
    std::atomic<std::uint64_t> produced{0};
    UdpAudioSender a;
    CHECK(!a.start("", 7355, 480, generator(&produced), err) && err.find("empty") != std::string::npos);
    CHECK(!a.start("no-such-host.invalid", 7355, 480, generator(&produced), err) && err.find("cannot find") != std::string::npos);
    CHECK(!a.start("127.0.0.1", 0, 480, generator(&produced), err) && err.find("port") != std::string::npos);
    CHECK(!a.start("127.0.0.1", 70000, 480, generator(&produced), err) && err.find("port") != std::string::npos);
    CHECK(!a.start("127.0.0.1", 7355, 500, generator(&produced), err) && err.find("480 or 960") != std::string::npos);
    CHECK(!a.start("127.0.0.1", 7355, 480, UdpAudioSender::Reader(), err) && err.find("no radio") != std::string::npos);
    CHECK(!a.running());
    Receiver rx;
    CHECK(a.start("127.0.0.1", rx.port, 480, generator(&produced), err));
    CHECK(!a.start("127.0.0.1", rx.port, 480, generator(&produced), err) && err.find("already running") != std::string::npos);
    a.stop();
    a.stop();   // twice is fine
    CHECK(a.start("127.0.0.1", rx.port, 960, generator(&produced), err));   // and it can start again
    a.stop();
}

void testRadioGoesAway() {
    Receiver rx;
    std::atomic<std::uint64_t> produced{0};
    std::atomic<bool> present{true};
    UdpAudioSender tx;
    std::string err;
    CHECK(tx.start("127.0.0.1", rx.port, 480, generator(&produced, &present), err));
    CHECK(rx.collect(300).size() > 3);
    present = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    rx.collect(100);   // drain
    const std::uint64_t before = tx.packetsSent();
    const std::size_t during = rx.collect(400).size();
    CHECK(during == 0);
    CHECK(tx.packetsSent() - before <= 1);
    CHECK(tx.noRadio() > 10);
    present = true;
    CHECK(rx.collect(500).size() > 3);
    tx.stop();
}

void testNobodyListening() {
    // a port nobody listens at
    Receiver gone;
    const int port = gone.port;
    closeSock(gone.s);
    gone.s = INVALID_SOCKET;
    std::atomic<std::uint64_t> produced{0};
    UdpAudioSender tx;
    std::string err;
    CHECK(tx.start("127.0.0.1", port, 480, generator(&produced), err));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(tx.running());
    CHECK(tx.packetsSent() + tx.sendErrors() > 5);     // it went on trying (a closed port may or may not be reported by the stack)
    const auto t0 = Clock::now();
    tx.stop();
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count() < 500);
    CHECK(!tx.running());
    // the reader is called from the worker thread only and not after stop()
    const std::uint64_t at = produced.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(produced.load() == at);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // (a run that hangs still shows how far it got)
    testPayload();
    testStream(480);
    testStream(960);
    testRefusals();
    testRadioGoesAway();
    testNobodyListening();
    return testSummary("test_udp_audio");
}
