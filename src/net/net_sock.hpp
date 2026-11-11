// net_sock.hpp - the few BSD-socket calls the network OUTPUT parts (tcp_fanout, udp_sink, aprs_is) share, written once for Winsock and POSIX.
//
// Nothing here owns anything: a socket is a plain std::intptr_t and every function is a thin call. The parts keep their own descriptors
// and close them on their own thread. IPv4 only, deliberately: every address the parts accept is a dotted quad, so no name lookup can
// stall a thread that must stop on time. The APRS-IS client is the one place that resolves a host name, on its own thread, never on the
// caller's.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_NET_NET_SOCK_HPP
#define CASCADE_NET_NET_SOCK_HPP

#include <cstddef>
#include <cstdint>
#include <string>

namespace cascade::net::sock {

using Handle = std::intptr_t;
inline constexpr Handle kInvalid = -1;

// Starts Winsock once per process (no-op elsewhere). False when it cannot start.
bool ensureStarted();

void closeHandle(Handle s);
bool setNonBlocking(Handle s);
void setNoDelay(Handle s);
// Receive and send buffer sizes (bytes) asked of the OS; the OS may round them.
void setBuffers(Handle s, int recvBytes, int sendBytes);

// True when the last socket call failed only because it would block (or was interrupted).
bool wouldBlock();

// Parses a dotted-quad IPv4 address; false for anything else (names, IPv6, "1..2.3", octal-looking fields).
bool parseIpv4(const std::string& text, std::uint32_t& hostOrder);
std::string formatIpv4(std::uint32_t hostOrder);

// A TCP listening socket on `ip` (host order), `port` (0 = ephemeral); non-blocking, exclusive on Windows. kInvalid and `error` on failure.
Handle listenTcp(std::uint32_t ip, std::uint16_t port, int backlog, std::string& error);
// The port a bound socket really has.
std::uint16_t boundPort(Handle s);
// Accepts one pending connection (non-blocking listener): kInvalid when none is waiting. The accepted socket is non-blocking.
Handle acceptOne(Handle listener);
// A non-blocking connect, finished with waitConnect.
Handle beginConnectTcp(std::uint32_t ip, std::uint16_t port);
// Waits up to `ms` for a connect to finish; true when connected.
bool waitConnect(Handle s, int ms);

// One recv / send on a non-blocking socket. Bytes moved, 0 = peer closed (recv), -1 = nothing now (see wouldBlock()) or an error.
long recvSome(Handle s, void* buf, std::size_t n);
long sendSome(Handle s, const void* buf, std::size_t n);

enum PollFlag : unsigned { kReadable = 1, kWritable = 2, kError = 4 };
struct PollItem {
    Handle s = kInvalid;
    unsigned want = 0;  // kReadable / kWritable
    unsigned got = 0;   // kReadable / kWritable / kError (error or hang-up)
};
// Polls `n` sockets (at most 256) for up to `ms` milliseconds; returns the count ready, -1 on failure. With n == 0 it just sleeps `ms`.
int pollMany(PollItem* items, std::size_t n, int ms);

// A UDP socket (non-blocking) and one datagram out.
Handle openUdp(bool allowBroadcast, std::string& error);
long sendTo(Handle s, const void* buf, std::size_t n, std::uint32_t ip, std::uint16_t port);
// A bound, non-blocking UDP socket (a listener; the stand-in receivers of the tests use it). recvSome() reads one datagram from it.
Handle bindUdp(std::uint32_t ip, std::uint16_t port, std::string& error);

}  // namespace cascade::net::sock

#endif
