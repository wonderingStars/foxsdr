// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/upnp_igd.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace cascade::net {
namespace {

// ===== text helpers =====

std::string lower(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

std::string trim(const std::string& s) {
    const std::size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) { return std::string(); }
    const std::size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool hasControl(const std::string& s) {
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7F) { return true; }
    }
    return false;
}

bool parseIpv4(const std::string& s, unsigned out[4]) {
    int parts = 0;
    std::size_t i = 0;
    while (parts < 4) {
        std::size_t dot = s.find('.', i);
        // exactly four segments: the last one must run to the end of the text (no fifth segment, no trailing dot)
        if (parts == 3 ? dot != std::string::npos : dot == std::string::npos) { return false; }
        if (dot == std::string::npos) { dot = s.size(); }
        const std::string seg = s.substr(i, dot - i);
        if (seg.empty() || seg.size() > 3) { return false; }
        unsigned v = 0;
        for (const char c : seg) {
            if (c < '0' || c > '9') { return false; }
            v = v * 10 + static_cast<unsigned>(c - '0');
        }
        if (v > 255 || (seg.size() > 1 && seg[0] == '0')) { return false; }
        out[parts++] = v;
        i = dot + 1;
    }
    return true;
}

// ===== a small, strict XML reader (rule 6) =====

struct XmlNode {
    std::string name;  // local name (the prefix is dropped)
    std::string text;  // character data directly inside, concatenated
    std::vector<XmlNode> children;
};

class XmlReader {
public:
    explicit XmlReader(const std::string& s) : s_(s) {}

    bool parse(XmlNode& root) {
        if (s_.size() > kMaxIgdHttpReply) { return false; }
        if (s_.compare(0, 3, "\xEF\xBB\xBF") == 0) { pos_ = 3; }
        for (;;) {
            skipWs();
            if (pos_ >= s_.size()) { return false; }
            if (startsWith("<?")) { if (!skipUntil("?>")) { return false; } continue; }
            if (startsWith("<!--")) { if (!skipUntil("-->")) { return false; } continue; }
            if (startsWith("<!")) { return false; }  // DOCTYPE / ENTITY: refused (rule 6)
            break;
        }
        if (s_[pos_] != '<') { return false; }
        if (!element(root, 0)) { return false; }
        for (;;) {  // only whitespace, comments and processing instructions may follow
            skipWs();
            if (pos_ >= s_.size()) { return true; }
            if (startsWith("<!--")) { if (!skipUntil("-->")) { return false; } continue; }
            if (startsWith("<?")) { if (!skipUntil("?>")) { return false; } continue; }
            return false;
        }
    }

private:
    bool startsWith(const char* p) const { return s_.compare(pos_, std::strlen(p), p) == 0; }
    void skipWs() { while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) { ++pos_; } }
    bool skipUntil(const char* end) {
        const std::size_t e = s_.find(end, pos_);
        if (e == std::string::npos) { return false; }
        pos_ = e + std::strlen(end);
        return true;
    }
    static bool nameChar(char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.' || c == ':';
    }

    bool decodeEntity(std::string& out) {  // at '&'
        const std::size_t semi = s_.find(';', pos_);
        if (semi == std::string::npos || semi - pos_ > 10) { return false; }
        const std::string ent = s_.substr(pos_ + 1, semi - pos_ - 1);
        pos_ = semi + 1;
        if (ent == "amp") { out.push_back('&'); return true; }
        if (ent == "lt") { out.push_back('<'); return true; }
        if (ent == "gt") { out.push_back('>'); return true; }
        if (ent == "quot") { out.push_back('"'); return true; }
        if (ent == "apos") { out.push_back('\''); return true; }
        if (ent.size() >= 2 && ent[0] == '#') {
            unsigned long v = 0;
            const bool hex = ent[1] == 'x';
            const std::size_t from = hex ? 2 : 1;
            if (from >= ent.size()) { return false; }
            for (std::size_t i = from; i < ent.size(); ++i) {
                const char c = ent[i];
                int d = -1;
                if (c >= '0' && c <= '9') { d = c - '0'; }
                else if (hex && c >= 'a' && c <= 'f') { d = c - 'a' + 10; }
                else if (hex && c >= 'A' && c <= 'F') { d = c - 'A' + 10; }
                if (d < 0) { return false; }
                v = v * (hex ? 16 : 10) + static_cast<unsigned long>(d);
                if (v > 0x10FFFF) { return false; }
            }
            if (v == 0 || (v < 0x20 && v != 9 && v != 10 && v != 13)) { return false; }
            if (v < 0x80) { out.push_back(static_cast<char>(v)); return true; }
            // Not needed for the fields this client reads; a non-ASCII reference is kept as '?' rather than building UTF-8 here.
            out.push_back('?');
            return true;
        }
        return false;  // an entity we did not define (and cannot, there is no DTD)
    }

    bool element(XmlNode& node, std::size_t depth) {
        if (depth >= kMaxIgdXmlDepth || ++nodes_ > kMaxIgdXmlNodes) { return false; }
        ++pos_;  // '<'
        const std::size_t ns = pos_;
        while (pos_ < s_.size() && nameChar(s_[pos_])) { ++pos_; }
        if (pos_ == ns) { return false; }
        std::string name = s_.substr(ns, pos_ - ns);
        const std::size_t colon = name.rfind(':');
        node.name = colon == std::string::npos ? name : name.substr(colon + 1);
        // attributes: skipped, but their quoting is checked so the tag ends where it should
        for (;;) {
            skipWs();
            if (pos_ >= s_.size()) { return false; }
            if (s_[pos_] == '/') {
                if (pos_ + 1 < s_.size() && s_[pos_ + 1] == '>') { pos_ += 2; return true; }
                return false;
            }
            if (s_[pos_] == '>') { ++pos_; break; }
            const std::size_t an = pos_;
            while (pos_ < s_.size() && nameChar(s_[pos_])) { ++pos_; }
            if (pos_ == an) { return false; }
            skipWs();
            if (pos_ >= s_.size() || s_[pos_] != '=') { return false; }
            ++pos_;
            skipWs();
            if (pos_ >= s_.size() || (s_[pos_] != '"' && s_[pos_] != '\'')) { return false; }
            const char q = s_[pos_++];
            const std::size_t e = s_.find(q, pos_);
            if (e == std::string::npos) { return false; }
            if (s_.find('<', pos_) < e) { return false; }
            pos_ = e + 1;
        }
        // content
        for (;;) {
            if (pos_ >= s_.size()) { return false; }
            const char c = s_[pos_];
            if (c == '<') {
                if (startsWith("</")) {
                    pos_ += 2;
                    const std::size_t cn = pos_;
                    while (pos_ < s_.size() && nameChar(s_[pos_])) { ++pos_; }
                    if (s_.compare(cn, pos_ - cn, name) != 0 || pos_ - cn != name.size()) { return false; }
                    skipWs();
                    if (pos_ >= s_.size() || s_[pos_] != '>') { return false; }
                    ++pos_;
                    return true;
                }
                if (startsWith("<!--")) { if (!skipUntil("-->")) { return false; } continue; }
                if (startsWith("<![CDATA[")) {
                    const std::size_t from = pos_ + 9;
                    const std::size_t e = s_.find("]]>", from);
                    if (e == std::string::npos) { return false; }
                    node.text += s_.substr(from, e - from);
                    pos_ = e + 3;
                    continue;
                }
                if (startsWith("<!") || startsWith("<?")) { return false; }
                node.children.emplace_back();
                if (!element(node.children.back(), depth + 1)) { return false; }
                continue;
            }
            if (c == '&') {
                if (!decodeEntity(node.text)) { return false; }
                continue;
            }
            node.text.push_back(c);
            ++pos_;
            if (node.text.size() > kMaxIgdHttpReply) { return false; }
        }
    }

    const std::string& s_;
    std::size_t pos_ = 0;
    std::size_t nodes_ = 0;
};

const XmlNode* child(const XmlNode& n, const char* name) {
    for (const XmlNode& c : n.children) {
        if (c.name == name) { return &c; }
    }
    return nullptr;
}

void findServices(const XmlNode& n, std::vector<const XmlNode*>& out) {
    if (n.name == "service") { out.push_back(&n); }
    for (const XmlNode& c : n.children) { findServices(c, out); }
}

// ===== sockets =====

#if defined(_WIN32)
using Sock = std::intptr_t;
constexpr Sock kInvalid = static_cast<Sock>(INVALID_SOCKET);
SOCKET raw(Sock s) { return static_cast<SOCKET>(s); }
void closeSock(Sock s) { if (s != kInvalid) { ::closesocket(raw(s)); } }
bool ensureNet() {
    static const bool ok = [] {
        WSADATA d{};
        return ::WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    return ok;
}
void setNonBlocking(Sock s) { u_long on = 1; ::ioctlsocket(raw(s), FIONBIO, &on); }
bool wouldBlock() { const int e = ::WSAGetLastError(); return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
#else
using Sock = std::intptr_t;
constexpr Sock kInvalid = -1;
int raw(Sock s) { return static_cast<int>(s); }
void closeSock(Sock s) { if (s != kInvalid) { ::close(raw(s)); } }
bool ensureNet() { return true; }
void setNonBlocking(Sock s) { const int fl = ::fcntl(raw(s), F_GETFL, 0); ::fcntl(raw(s), F_SETFL, fl | O_NONBLOCK); }
bool wouldBlock() { return errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EAGAIN; }
#endif

struct Guard {
    Sock s;
    ~Guard() { closeSock(s); }
};

bool makeAddr(const std::string& host, int port, sockaddr_in& a) {
    std::memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<unsigned short>(port));
    return ::inet_pton(AF_INET, host.c_str(), &a.sin_addr) == 1;  // literals only: no name lookup, ever
}

bool waitFor(Sock s, bool write, std::chrono::steady_clock::time_point deadline) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    if (left <= 0) { return false; }
    fd_set set;
    FD_ZERO(&set);
    FD_SET(raw(s), &set);
    timeval tv{};
    tv.tv_sec = static_cast<long>(left / 1000);
    tv.tv_usec = static_cast<long>((left % 1000) * 1000);
    const int r = ::select(static_cast<int>(raw(s)) + 1, write ? nullptr : &set, write ? &set : nullptr, nullptr, &tv);
    return r > 0;
}

class SocketIgdTransport final : public IgdTransport {
public:
    bool ssdpSearch(const std::string& host, int port, const std::string& request, int timeoutMs, std::string& reply,
                    std::string& fromAddress) override {
        reply.clear();
        fromAddress.clear();
        if (!ensureNet() || port < 1 || port > 65535) { return false; }
        sockaddr_in to{};
        if (!makeAddr(host, port, to)) { return false; }
        unsigned oct[4] = {0, 0, 0, 0};
        parseIpv4(host, oct);
        const bool loop = oct[0] == 127;
        const Sock s = static_cast<Sock>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
        if (s == kInvalid) { return false; }
        Guard g{s};
        // A loopback target is answered on loopback, so the reply socket binds loopback only (no firewall prompt for a test); anything
        // else needs the wildcard to hear a reply.
        sockaddr_in me{};
        makeAddr(loop ? "127.0.0.1" : "0.0.0.0", 0, me);
        if (::bind(raw(s), reinterpret_cast<sockaddr*>(&me), sizeof(me)) != 0) { return false; }
        if (!loop) {
            const int ttl = 2;
            ::setsockopt(raw(s), IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof(ttl));
        }
        if (::sendto(raw(s), request.data(), static_cast<int>(request.size()), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to)) < 0) {
            return false;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(timeoutMs, 1));
        char buf[kMaxSsdpDatagram + 1];
        while (waitFor(s, false, deadline)) {
            sockaddr_in from{};
#if defined(_WIN32)
            int fl = sizeof(from);
#else
            socklen_t fl = sizeof(from);
#endif
            const int n = static_cast<int>(::recvfrom(raw(s), buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fl));
            if (n <= 0) { continue; }
            if (static_cast<std::size_t>(n) > kMaxSsdpDatagram) { continue; }  // an oversized datagram is not an SSDP reply
            char ip[INET_ADDRSTRLEN] = {0};
            ::inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
            reply.assign(buf, static_cast<std::size_t>(n));
            fromAddress = ip;
            return true;
        }
        return false;
    }

    bool httpExchange(const HttpRequest& req, int timeoutMs, std::size_t maxBytes, std::string& rawReply) override {
        rawReply.clear();
        if (!ensureNet() || req.port < 1 || req.port > 65535) { return false; }
        sockaddr_in to{};
        if (!makeAddr(req.host, req.port, to)) { return false; }
        const Sock s = static_cast<Sock>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        if (s == kInvalid) { return false; }
        Guard g{s};
        setNonBlocking(s);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(timeoutMs, 1));
        if (::connect(raw(s), reinterpret_cast<sockaddr*>(&to), sizeof(to)) != 0) {
            if (!wouldBlock() || !waitFor(s, true, deadline)) { return false; }
            int err = 0;
#if defined(_WIN32)
            int el = sizeof(err);
#else
            socklen_t el = sizeof(err);
#endif
            if (::getsockopt(raw(s), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &el) != 0 || err != 0) { return false; }
        }
        std::string wire = req.method + " " + req.path + " HTTP/1.1\r\nHOST: " + req.host + ":" + std::to_string(req.port) + "\r\n";
        for (const auto& h : req.headers) { wire += h.first + ": " + h.second + "\r\n"; }
        wire += "CONTENT-LENGTH: " + std::to_string(req.body.size()) + "\r\nCONNECTION: close\r\n\r\n" + req.body;
        std::size_t sent = 0;
        while (sent < wire.size()) {
            if (!waitFor(s, true, deadline)) { return false; }
            const int n = static_cast<int>(::send(raw(s), wire.data() + sent, static_cast<int>(wire.size() - sent), 0));
            if (n <= 0) {
                if (n < 0 && wouldBlock()) { continue; }
                return false;
            }
            sent += static_cast<std::size_t>(n);
        }
        char buf[4096];
        while (rawReply.size() <= maxBytes) {
            if (!waitFor(s, false, deadline)) { return false; }
            const int n = static_cast<int>(::recv(raw(s), buf, sizeof(buf), 0));
            if (n == 0) { return true; }  // closed: the reply is complete
            if (n < 0) {
                if (wouldBlock()) { continue; }
                return false;
            }
            rawReply.append(buf, static_cast<std::size_t>(n));
        }
        return false;  // over the cap
    }
};

}  // namespace

// ===== public helpers =====

bool isLocalNetworkAddress(const std::string& ipv4) {
    unsigned o[4];
    if (!parseIpv4(ipv4, o)) { return false; }
    if (o[0] == 127) { return true; }
    if (o[0] == 169 && o[1] == 254) { return true; }
    return isPrivateLanAddress(ipv4);
}

bool isPrivateLanAddress(const std::string& ipv4) {
    unsigned o[4];
    if (!parseIpv4(ipv4, o)) { return false; }
    if (o[0] == 10) { return true; }
    if (o[0] == 172 && o[1] >= 16 && o[1] <= 31) { return true; }
    if (o[0] == 192 && o[1] == 168) { return true; }
    return false;
}

bool parseSsdpReply(const std::string& datagram, SsdpReply& out) {
    out = SsdpReply{};
    if (datagram.empty() || datagram.size() > kMaxSsdpDatagram) { return false; }
    std::size_t lineEnd = datagram.find("\r\n");
    if (lineEnd == std::string::npos) { return false; }
    const std::string status = datagram.substr(0, lineEnd);
    if (status.compare(0, 9, "HTTP/1.1 ") != 0 || status.compare(9, 3, "200") != 0) { return false; }
    std::size_t pos = lineEnd + 2;
    bool haveLoc = false;
    while (pos < datagram.size()) {
        std::size_t e = datagram.find("\r\n", pos);
        if (e == std::string::npos) { e = datagram.size(); }
        const std::string line = datagram.substr(pos, e - pos);
        pos = e + 2;
        if (line.empty()) { break; }
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) { continue; }
        const std::string key = lower(trim(line.substr(0, colon)));
        const std::string val = trim(line.substr(colon + 1));
        if (hasControl(val)) { return false; }
        if (key == "location") { out.location = val; haveLoc = true; }
        else if (key == "st") { out.st = val; }
        else if (key == "usn") { out.usn = val; }
    }
    return haveLoc && !out.location.empty();
}

std::string buildMSearch(const std::string& host, int port, const std::string& st, int mx) {
    mx = std::min(std::max(mx, 1), 5);
    return "M-SEARCH * HTTP/1.1\r\nHOST: " + host + ":" + std::to_string(port) + "\r\nMAN: \"ssdp:discover\"\r\nMX: " + std::to_string(mx) +
           "\r\nST: " + st + "\r\n\r\n";
}

bool parseHttpUrl(const std::string& url, IgdUrl& out) {
    out = IgdUrl{};
    if (url.size() > 512 || hasControl(url) || url.find(' ') != std::string::npos) { return false; }
    if (lower(url.substr(0, 7)) != "http://") { return false; }
    std::string rest = url.substr(7);
    if (rest.find('#') != std::string::npos || rest.find('@') != std::string::npos || rest.find('\\') != std::string::npos) { return false; }
    std::string hostport = rest;
    std::string path = "/";
    const std::size_t slash = rest.find('/');
    if (slash != std::string::npos) {
        hostport = rest.substr(0, slash);
        path = rest.substr(slash);
    } else if (rest.find('?') != std::string::npos) {
        return false;
    }
    std::string host = hostport;
    int port = 80;
    const std::size_t colon = hostport.find(':');
    if (colon != std::string::npos) {
        host = hostport.substr(0, colon);
        const std::string ps = hostport.substr(colon + 1);
        if (ps.empty() || ps.size() > 5) { return false; }
        int p = 0;
        for (const char c : ps) {
            if (c < '0' || c > '9') { return false; }
            p = p * 10 + (c - '0');
        }
        if (p < 1 || p > 65535) { return false; }
        port = p;
    }
    unsigned o[4];
    if (!parseIpv4(host, o)) { return false; }  // an IPv4 literal only: a name could be pointed anywhere
    out.host = host;
    out.port = port;
    out.path = path;
    return true;
}

bool parseDeviceDescription(const std::string& xml, const IgdUrl& base, IgdService& out, std::string& error) {
    out = IgdService{};
    error.clear();
    XmlNode root;
    XmlReader r(xml);
    if (!r.parse(root)) { error = "the router's description is not valid XML"; return false; }
    std::vector<const XmlNode*> services;
    findServices(root, services);
    static const char* const kTypes[] = {"urn:schemas-upnp-org:service:WANIPConnection:1", "urn:schemas-upnp-org:service:WANIPConnection:2",
                                         "urn:schemas-upnp-org:service:WANPPPConnection:1"};
    for (const char* want : kTypes) {
        for (const XmlNode* s : services) {
            const XmlNode* t = child(*s, "serviceType");
            const XmlNode* c = child(*s, "controlURL");
            if (t == nullptr || c == nullptr || trim(t->text) != want) { continue; }
            const std::string cu = trim(c->text);
            if (cu.empty() || cu.size() > 512 || hasControl(cu) || cu.find(' ') != std::string::npos || cu.find('#') != std::string::npos) { continue; }
            std::string path;
            if (lower(cu.substr(0, 7)) == "http://") {
                IgdUrl u;
                if (!parseHttpUrl(cu, u) || u.host != base.host || u.port != base.port) { continue; }  // rule 4: the same host and port only
                path = u.path;
            } else if (cu[0] == '/') {
                if (cu.size() > 1 && cu[1] == '/') { continue; }  // "//host/..." is a network-path reference, not a path
                path = cu;
            } else {
                if (cu.find("://") != std::string::npos) { continue; }
                const std::size_t q = base.path.rfind('/');
                path = (q == std::string::npos ? std::string("/") : base.path.substr(0, q + 1)) + cu;
            }
            if (path.compare(0, 2, "//") == 0) { continue; }  // a path that starts with two slashes reads as a network-path reference downstream
            out.serviceType = want;
            out.controlPath = path;
            return true;
        }
    }
    error = "the router offers no port-mapping service";
    return false;
}

std::string xmlEscape(const std::string& s) {
    std::string o;
    for (const char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            case '\'': o += "&apos;"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20 && c != '\t' && c != '\n' && c != '\r') { o.push_back(' '); } else { o.push_back(c); }
        }
    }
    return o;
}

std::string buildSoapBody(const std::string& serviceType, const std::string& action, const std::vector<std::pair<std::string, std::string>>& args) {
    std::string b = "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                    "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" +
                    action + " xmlns:u=\"" + serviceType + "\">";
    for (const auto& a : args) { b += "<" + a.first + ">" + xmlEscape(a.second) + "</" + a.first + ">"; }
    b += "</u:" + action + "></s:Body></s:Envelope>";
    return b;
}

bool parseSoapReply(int httpStatus, const std::string& body, const std::string& action, SoapReply& out) {
    out = SoapReply{};
    XmlNode root;
    XmlReader r(body);
    if (!r.parse(root) || root.name != "Envelope") { return false; }
    const XmlNode* b = child(root, "Body");
    if (b == nullptr) { return false; }
    if (httpStatus == 200) {
        const XmlNode* resp = child(*b, (action + "Response").c_str());
        if (resp == nullptr) { return false; }
        for (const XmlNode& c : resp->children) { out.out[c.name] = trim(c.text); }
        out.ok = true;
        return true;
    }
    const XmlNode* fault = child(*b, "Fault");
    if (fault == nullptr) { return false; }
    const XmlNode* detail = child(*fault, "detail");
    const XmlNode* err = detail != nullptr ? child(*detail, "UPnPError") : nullptr;
    if (err == nullptr) { return false; }
    const XmlNode* code = child(*err, "errorCode");
    const XmlNode* desc = child(*err, "errorDescription");
    int c = 0;
    if (code != nullptr) {
        const std::string t = trim(code->text);
        if (t.empty() || t.size() > 5) { return false; }
        for (const char ch : t) {
            if (ch < '0' || ch > '9') { return false; }
            c = c * 10 + (ch - '0');
        }
    } else {
        return false;
    }
    out.ok = false;
    out.errorCode = c;
    if (desc != nullptr) { out.errorDescription = trim(desc->text).substr(0, 128); }
    return true;
}

bool parseHttpResponse(const std::string& rawIn, int& status, std::string& body) {
    status = 0;
    body.clear();
    if (rawIn.size() > kMaxIgdHttpReply + 8192) { return false; }
    const std::size_t he = rawIn.find("\r\n\r\n");
    if (he == std::string::npos) { return false; }
    const std::string head = rawIn.substr(0, he);
    if (head.compare(0, 5, "HTTP/") != 0) { return false; }
    const std::size_t sp = head.find(' ');
    if (sp == std::string::npos || sp + 4 > head.size()) { return false; }
    int st = 0;
    for (std::size_t i = sp + 1; i < sp + 4; ++i) {
        if (head[i] < '0' || head[i] > '9') { return false; }
        st = st * 10 + (head[i] - '0');
    }
    bool chunked = false;
    long long contentLength = -1;
    std::size_t pos = head.find("\r\n");
    while (pos != std::string::npos && pos < head.size()) {
        pos += 2;
        std::size_t e = head.find("\r\n", pos);
        if (e == std::string::npos) { e = head.size(); }
        const std::string line = head.substr(pos, e - pos);
        pos = e;
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) { continue; }
        const std::string key = lower(trim(line.substr(0, colon)));
        const std::string val = lower(trim(line.substr(colon + 1)));
        if (key == "transfer-encoding" && val.find("chunked") != std::string::npos) { chunked = true; }
        if (key == "content-length") {
            if (val.empty() || val.size() > 9) { return false; }
            long long v = 0;
            for (const char c : val) {
                if (c < '0' || c > '9') { return false; }
                v = v * 10 + (c - '0');
            }
            contentLength = v;
        }
    }
    const std::string rest = rawIn.substr(he + 4);
    if (chunked) {
        std::size_t p = 0;
        std::string out;
        for (;;) {
            const std::size_t le = rest.find("\r\n", p);
            if (le == std::string::npos) { return false; }
            std::string szs = trim(rest.substr(p, le - p));
            const std::size_t semi = szs.find(';');
            if (semi != std::string::npos) { szs = szs.substr(0, semi); }
            if (szs.empty() || szs.size() > 6) { return false; }
            std::size_t sz = 0;
            for (const char c : szs) {
                int d = -1;
                if (c >= '0' && c <= '9') { d = c - '0'; }
                else if (c >= 'a' && c <= 'f') { d = c - 'a' + 10; }
                else if (c >= 'A' && c <= 'F') { d = c - 'A' + 10; }
                if (d < 0) { return false; }
                sz = sz * 16 + static_cast<std::size_t>(d);
            }
            p = le + 2;
            if (sz == 0) { break; }
            if (p + sz > rest.size() || out.size() + sz > kMaxIgdHttpReply) { return false; }
            out.append(rest, p, sz);
            p += sz;
            if (rest.compare(p, 2, "\r\n") != 0) { return false; }
            p += 2;
        }
        body = std::move(out);
    } else if (contentLength >= 0) {
        if (static_cast<std::size_t>(contentLength) > rest.size() || static_cast<std::size_t>(contentLength) > kMaxIgdHttpReply) { return false; }
        body = rest.substr(0, static_cast<std::size_t>(contentLength));
    } else {
        if (rest.size() > kMaxIgdHttpReply) { return false; }
        body = rest;
    }
    status = st;
    return true;
}

std::shared_ptr<IgdTransport> makeSocketIgdTransport() { return std::make_shared<SocketIgdTransport>(); }

// ===== the mapper =====

const char* upnpResultText(UpnpResult r) {
    switch (r) {
        case UpnpResult::Ok: return "ok";
        case UpnpResult::Disabled: return "router port mapping is switched off";
        case UpnpResult::BadConfig: return "the port mapping settings are not valid";
        case UpnpResult::NotDiscovered: return "no router has been found yet";
        case UpnpResult::NoDevice: return "no router on this network answered";
        case UpnpResult::BadReply: return "the router's answer could not be understood";
        case UpnpResult::Network: return "could not talk to the router";
        case UpnpResult::PortNotAllowed: return "that port is not one you chose to open";
        case UpnpResult::BadInternalClient: return "this computer's network address is not a private one";
        case UpnpResult::DeviceRefused: return "the router refused";
    }
    return "unknown";
}

UpnpMapper::UpnpMapper(const UpnpConfig& cfg, std::shared_ptr<IgdTransport> transport) : cfg_(cfg), transport_(std::move(transport)) {
    bool ok = transport_ != nullptr && cfg_.allowedPorts.size() <= kMaxIgdAllowedPorts && cfg_.leaseSeconds >= 60 && cfg_.leaseSeconds <= 86400 &&
              cfg_.timeoutMs >= 100 && cfg_.timeoutMs <= 30000 && cfg_.ssdpPort >= 1 && cfg_.ssdpPort <= 65535 && !cfg_.description.empty() &&
              cfg_.description.size() <= 64 && !hasControl(cfg_.description);
    unsigned o[4];
    // The search goes to the standard SSDP group or to a local-network literal (a stand-in router in a test); nothing else.
    ok = ok && parseIpv4(cfg_.ssdpHost, o) && (cfg_.ssdpHost == "239.255.255.250" || isLocalNetworkAddress(cfg_.ssdpHost));
    for (const int p : cfg_.allowedPorts) { ok = ok && p >= 1024 && p <= 65535; }
    configValid_ = ok;
}

UpnpMapper::~UpnpMapper() { removeAll(); }

UpnpResult UpnpMapper::discover() {
    if (!cfg_.enabled) { return UpnpResult::Disabled; }
    if (!configValid_) { return UpnpResult::BadConfig; }
    std::lock_guard<std::mutex> lk(mu_);
    discovered_ = false;
    std::string dg, from;
    const std::string req = buildMSearch(cfg_.ssdpHost, cfg_.ssdpPort, "urn:schemas-upnp-org:device:InternetGatewayDevice:1", 2);
    if (!transport_->ssdpSearch(cfg_.ssdpHost, cfg_.ssdpPort, req, cfg_.timeoutMs, dg, from)) { return UpnpResult::NoDevice; }
    SsdpReply rep;
    if (!parseSsdpReply(dg, rep)) { return UpnpResult::NoDevice; }
    IgdUrl loc;
    // rule 4: a local IPv4 literal that is also the one that answered
    if (!parseHttpUrl(rep.location, loc) || !isLocalNetworkAddress(loc.host) || loc.host != from || !isLocalNetworkAddress(from)) {
        return UpnpResult::NoDevice;
    }
    HttpRequest g;
    g.method = "GET";
    g.host = loc.host;
    g.port = loc.port;
    g.path = loc.path;
    std::string rawReply;
    if (!transport_->httpExchange(g, cfg_.timeoutMs, kMaxIgdHttpReply + 8192, rawReply)) { return UpnpResult::Network; }
    int status = 0;
    std::string body;
    if (!parseHttpResponse(rawReply, status, body) || status != 200) { return UpnpResult::BadReply; }  // a redirect is a failure (rule 4)
    IgdService svc;
    std::string err;
    if (!parseDeviceDescription(body, loc, svc, err)) { return UpnpResult::NoDevice; }
    router_ = loc;
    service_ = svc;
    discovered_ = true;
    return UpnpResult::Ok;
}

UpnpResult UpnpMapper::soap(const std::string& action, const std::vector<std::pair<std::string, std::string>>& args, SoapReply& reply) {
    HttpRequest p;
    p.method = "POST";
    p.host = router_.host;
    p.port = router_.port;
    p.path = service_.controlPath;
    p.headers.emplace_back("CONTENT-TYPE", "text/xml; charset=\"utf-8\"");
    p.headers.emplace_back("SOAPACTION", "\"" + service_.serviceType + "#" + action + "\"");
    p.body = buildSoapBody(service_.serviceType, action, args);
    std::string rawReply;
    if (!transport_->httpExchange(p, cfg_.timeoutMs, kMaxIgdHttpReply + 8192, rawReply)) { return UpnpResult::Network; }
    int status = 0;
    std::string body;
    if (!parseHttpResponse(rawReply, status, body)) { return UpnpResult::BadReply; }
    if (!parseSoapReply(status, body, action, reply)) { return UpnpResult::BadReply; }
    if (!reply.ok) {
        lastError_ = reply.errorCode;
        return UpnpResult::DeviceRefused;
    }
    return UpnpResult::Ok;
}

UpnpResult UpnpMapper::addMapping(int port) {
    if (!cfg_.enabled) { return UpnpResult::Disabled; }
    if (!configValid_) { return UpnpResult::BadConfig; }
    if (std::find(cfg_.allowedPorts.begin(), cfg_.allowedPorts.end(), port) == cfg_.allowedPorts.end()) { return UpnpResult::PortNotAllowed; }
    if (!isPrivateLanAddress(cfg_.internalClient)) { return UpnpResult::BadInternalClient; }
    std::lock_guard<std::mutex> lk(mu_);
    if (!discovered_) { return UpnpResult::NotDiscovered; }
    SoapReply rep;
    const std::string ps = std::to_string(port);
    const UpnpResult r = soap("AddPortMapping",
                              {{"NewRemoteHost", ""},
                               {"NewExternalPort", ps},
                               {"NewProtocol", "TCP"},
                               {"NewInternalPort", ps},
                               {"NewInternalClient", cfg_.internalClient},
                               {"NewEnabled", "1"},
                               {"NewPortMappingDescription", cfg_.description},
                               {"NewLeaseDuration", std::to_string(cfg_.leaseSeconds)}},
                              rep);
    if (r == UpnpResult::Ok && std::find(active_.begin(), active_.end(), port) == active_.end()) { active_.push_back(port); }
    return r;
}

UpnpResult UpnpMapper::removeMapping(int port) {
    if (!cfg_.enabled) { return UpnpResult::Disabled; }
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = std::find(active_.begin(), active_.end(), port);
    if (it == active_.end() || !discovered_) { return UpnpResult::PortNotAllowed; }  // only what this object added is ever removed
    SoapReply rep;
    const UpnpResult r = soap("DeletePortMapping", {{"NewRemoteHost", ""}, {"NewExternalPort", std::to_string(port)}, {"NewProtocol", "TCP"}}, rep);
    // 714 (no such entry) means it is already gone, which is the state we wanted.
    if (r == UpnpResult::Ok || (r == UpnpResult::DeviceRefused && lastError_ == 714)) {
        active_.erase(std::find(active_.begin(), active_.end(), port));
        return UpnpResult::Ok;
    }
    return r;
}

UpnpResult UpnpMapper::removeAll() {
    if (!cfg_.enabled) { return UpnpResult::Disabled; }
    std::vector<int> todo;
    {
        std::lock_guard<std::mutex> lk(mu_);
        todo = active_;
    }
    UpnpResult worst = UpnpResult::Ok;
    for (const int p : todo) {
        const UpnpResult r = removeMapping(p);
        if (r != UpnpResult::Ok) { worst = r; }
    }
    return worst;
}

UpnpResult UpnpMapper::getExternalAddress(std::string& out) {
    out.clear();
    if (!cfg_.enabled) { return UpnpResult::Disabled; }
    std::lock_guard<std::mutex> lk(mu_);
    if (!discovered_) { return UpnpResult::NotDiscovered; }
    SoapReply rep;
    const UpnpResult r = soap("GetExternalIPAddress", {}, rep);
    if (r != UpnpResult::Ok) { return r; }
    const auto it = rep.out.find("NewExternalIPAddress");
    unsigned o[4];
    if (it == rep.out.end() || !parseIpv4(it->second, o)) { return UpnpResult::BadReply; }
    out = it->second;
    return UpnpResult::Ok;
}

std::vector<int> UpnpMapper::activePorts() const {
    std::lock_guard<std::mutex> lk(mu_);
    return active_;
}

int UpnpMapper::lastErrorCode() const {
    std::lock_guard<std::mutex> lk(mu_);
    return lastError_;
}

}  // namespace cascade::net
