// server_ports.hpp - the web server's and the CAT listener's port and address vocabulary, as data (engine extraction stage 5b, the fence's
// hoist pattern: core/spectrum_frame.hpp, scanner_params.hpp, tx_input.hpp).
//
// Moved out of net/web_policy.hpp, net/web_policy.cpp and net/cat_server.hpp, unchanged: the settings panels (the window's Web access and
// CAT sections) draw a port field clamped to the range the policy accepts and a "Reachable from" choice classified by the address in force,
// and net/ is the engine's now (the servers are its own; docs/engine-stage4.md 6.10). The interface takes these few constants and two
// pure functions from here and includes nothing of net/. net/web_policy.hpp includes this header and names them back, so every net/ user
// (the policy, the server, the host, the tests) is unchanged.
//
// Nothing here takes anything but the standard library.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_SERVER_PORTS_HPP
#define CASCADE_CORE_SERVER_PORTS_HPP

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace cascade::net {

// Floor for the web server's listening port (web_policy.hpp, rule 6) and the ceiling any TCP port has.
inline constexpr int kMinPort = 1024;
inline constexpr int kMaxPort = 65535;

// The default the application ships with: loopback, so a fresh install is reachable only from the machine it runs on and turning the
// feature on cannot by itself expose anything.
inline constexpr int kDefaultWebPort = 8073;

// The port Hamlib's rigctld uses, and therefore the one every CAT client already expects. Changing it means configuring every client, so it
// is the default.
inline constexpr std::uint16_t kDefaultCatPort = 4532;

namespace addr_detail {

// Splits on '.' WITHOUT collapsing empty fields, so "1..2.3" and "1.2.3." are seen as the malformed things they are rather than silently
// repaired.
inline std::vector<std::string> splitDots(const std::string& s) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : s) {
        if (c == '.') {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    parts.push_back(cur);
    return parts;
}

// One IPv4 octet: 1-3 decimal digits, value <= 255, and no leading zero on a multi-digit field (rule 5 - "010" is octal to some parsers).
// Returns -1 if the text is not such an octet.
inline int parseOctet(const std::string& text) {
    if (text.empty() || text.size() > 3) { return -1; }
    for (char c : text) {
        if (c < '0' || c > '9') { return -1; }
    }
    if (text.size() > 1 && text[0] == '0') { return -1; }
    const int value = std::atoi(text.c_str());
    return (value >= 0 && value <= 255) ? value : -1;
}

}  // namespace addr_detail

// True iff `addr` is a well-formed dotted quad. `firstOctet` receives octet 0 so the caller can test for 127.0.0.0/8 without parsing twice.
inline bool parseIPv4(const std::string& addr, int& firstOctet) {
    const std::vector<std::string> parts = addr_detail::splitDots(addr);
    if (parts.size() != 4) { return false; }
    int octets[4];
    for (std::size_t i = 0; i < 4; ++i) {
        octets[i] = addr_detail::parseOctet(parts[i]);
        if (octets[i] < 0) { return false; }
    }
    firstOctet = octets[0];
    return true;
}

// True for 127.0.0.0/8, ::1, and the literal "localhost" (rule 4).
inline bool isLoopbackAddress(const std::string& addr) {
    if (addr == "localhost" || addr == "::1") { return true; }
    int first = 0;
    if (parseIPv4(addr, first)) {
        // The whole 127.0.0.0/8 block, not just 127.0.0.1 - 127.0.0.2 is every bit as local, and treating it as remote would demand a
        // password for a binding that no other machine can reach.
        return first == 127;
    }
    return false;
}

// True for 0.0.0.0, ::, and the empty string - "every interface". These are valid binds, but they are the MOST exposed ones, so they are
// classified explicitly rather than falling out of "not loopback".
inline bool isWildcardAddress(const std::string& addr) { return addr.empty() || addr == "0.0.0.0" || addr == "::"; }

}  // namespace cascade::net

#endif  // CASCADE_CORE_SERVER_PORTS_HPP
