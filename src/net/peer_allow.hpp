// net/peer_allow.hpp - the list of client addresses a network server accepts (security lane S4b-sec, finding R7).
//
// WHY. The rtl_tcp server and the audio-over-TCP server have no place for a password in their protocols. Made reachable beyond this computer they would hand a radio's samples (and, for
// rtl_tcp, its tuning) to anyone on the network. So beyond this computer they are widened only to ADDRESSES THE PERSON TYPES: a connection from any other address is closed before a byte is
// read or sent. This is the list.
//
// THE TEXT. IPv4 addresses and networks separated by commas, semicolons, spaces or line breaks: "192.168.1.20, 192.168.1.0/24". A network has a prefix of 8 to 32 bits (a /0 to /7 would be
// "every address" by another name and is refused). Refused: an address that is not a dotted quad, 0.x.x.x, 255.255.255.255, a multicast address (224 and up), more than 16 entries, more than 400
// characters, an empty list. Host bits of a network are ignored (192.168.1.77/24 is 192.168.1.0/24).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_NET_PEER_ALLOW_HPP
#define CASCADE_NET_PEER_ALLOW_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "net/net_sock.hpp"

namespace cascade::net {

inline constexpr std::size_t kPeerAllowMaxEntries = 16;
inline constexpr std::size_t kPeerAllowMaxChars = 400;

class PeerAllow {
public:
    // True with the list read; false with the sentence why not in `why` (and `out` empty).
    static bool parse(const std::string& text, PeerAllow& out, std::string& why) {
        out.entries_.clear();
        why.clear();
        if (text.size() > kPeerAllowMaxChars) { why = "the list of addresses is too long"; return false; }
        std::size_t i = 0;
        while (i <= text.size()) {
            while (i < text.size() && isSep(text[i])) { ++i; }
            if (i >= text.size()) { break; }
            std::size_t j = i;
            while (j < text.size() && !isSep(text[j])) { ++j; }
            const std::string item = text.substr(i, j - i);
            i = j;
            std::string ip = item;
            int prefix = 32;
            const std::size_t slash = item.find('/');
            if (slash != std::string::npos) {
                ip = item.substr(0, slash);
                const std::string p = item.substr(slash + 1);
                if (p.empty() || p.size() > 2) { why = "\"" + item + "\" is not an address like 192.168.1.20 or a network like 192.168.1.0/24"; out.entries_.clear(); return false; }
                prefix = 0;
                for (const char ch : p) {
                    if (ch < '0' || ch > '9') { why = "\"" + item + "\" is not an address like 192.168.1.20 or a network like 192.168.1.0/24"; out.entries_.clear(); return false; }
                    prefix = prefix * 10 + (ch - '0');
                }
                if (prefix < 8 || prefix > 32) { why = "\"" + item + "\": a network is 8 to 32 bits wide (a wider one would be every address)"; out.entries_.clear(); return false; }
            }
            std::uint32_t v = 0;
            if (!sock::parseIpv4(ip, v)) { why = "\"" + item + "\" is not an address like 192.168.1.20 or a network like 192.168.1.0/24"; out.entries_.clear(); return false; }
            if ((v >> 24) == 0 || v == 0xFFFFFFFFu || (v >> 24) >= 224) { why = "\"" + item + "\" is not the address of a computer"; out.entries_.clear(); return false; }
            if (out.entries_.size() >= kPeerAllowMaxEntries) { why = "at most 16 addresses or networks"; out.entries_.clear(); return false; }
            const std::uint32_t mask = prefix == 32 ? 0xFFFFFFFFu : ~((1u << (32 - prefix)) - 1u);
            out.entries_.push_back({v & mask, mask});
        }
        if (out.entries_.empty()) { why = "type the address of each computer that may connect"; return false; }
        return true;
    }

    bool empty() const { return entries_.empty(); }
    std::size_t size() const { return entries_.size(); }
    // `ip` in host order.
    bool allows(std::uint32_t ip) const {
        for (const Entry& e : entries_) {
            if ((ip & e.mask) == e.net) { return true; }
        }
        return false;
    }
    // `dotted` as sock::peerOf gives it ("" = the address could not be read: never allowed).
    bool allowsText(const std::string& dotted) const {
        std::uint32_t v = 0;
        return !dotted.empty() && sock::parseIpv4(dotted, v) && allows(v);
    }

private:
    struct Entry { std::uint32_t net = 0, mask = 0; };
    static bool isSep(char c) { return c == ',' || c == ';' || c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
    std::vector<Entry> entries_;
};

}  // namespace cascade::net

#endif  // CASCADE_NET_PEER_ALLOW_HPP
