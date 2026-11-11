// Tests for the UPnP IGD client (net/upnp_igd.hpp).
//
// NOTHING HERE TOUCHES A REAL ROUTER OR THE REAL NETWORK. Every socket is on 127.0.0.1: the "router" is a stand-in in this file (a UDP
// responder for SSDP and a TCP server for the description and the SOAP control URL), and the transport under test is wrapped in
// LoopbackOnlyTransport, which REFUSES (and records) any target that is not 127.x.x.x, so even a bug could not send a packet off the machine.
// The default multicast group 239.255.255.250 appears only in a test that checks the default config and never reaches a socket.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

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
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "net/upnp_igd.hpp"
#include "test_check.hpp"

#ifndef CASCADE_SOURCE_DIR
#define CASCADE_SOURCE_DIR "."
#endif

using namespace cascade::net;

namespace {

// ===== portable socket bits for the stand-in =====
#if defined(_WIN32)
using Sock = std::intptr_t;
constexpr Sock kBad = static_cast<Sock>(INVALID_SOCKET);
SOCKET rw(Sock s) { return static_cast<SOCKET>(s); }
void closeS(Sock s) { if (s != kBad) { ::closesocket(rw(s)); } }
void netInit() { WSADATA d{}; ::WSAStartup(MAKEWORD(2, 2), &d); }
#else
using Sock = std::intptr_t;
constexpr Sock kBad = -1;
int rw(Sock s) { return static_cast<int>(s); }
void closeS(Sock s) { if (s != kBad) { ::close(rw(s)); } }
void netInit() {}
#endif

sockaddr_in loopback(int port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<unsigned short>(port));
    ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    return a;
}

int boundPort(Sock s) {
    sockaddr_in a{};
#if defined(_WIN32)
    int l = sizeof(a);
#else
    socklen_t l = sizeof(a);
#endif
    ::getsockname(rw(s), reinterpret_cast<sockaddr*>(&a), &l);
    return ntohs(a.sin_port);
}

// ===== the stand-in router =====
struct Behaviour {
    std::string locationHost = "127.0.0.1";   // the host written into the SSDP LOCATION
    bool answerSsdp = true;
    bool oversizedSsdp = false;                // answer with a datagram over the cap
    int descStatus = 200;                      // 302 = a redirect
    std::string descOverride;                  // replace the description body
    bool chunkedDesc = false;
    std::string serviceType = "urn:schemas-upnp-org:service:WANIPConnection:1";
    std::string controlUrl = "/ctl/IPConn";
    int addFault = 0;                          // a UPnPError code for AddPortMapping (0 = succeed)
    int deleteFault = 0;
    bool answerControl = true;                 // false: accept and say nothing (a hung router)
    std::string externalIp = "203.0.113.9";    // a TEST-NET-3 documentation address
};

struct SeenCall {
    std::string action;
    std::map<std::string, std::string> args;
};

class StandInRouter {
public:
    StandInRouter() {
        netInit();
        udp_ = static_cast<Sock>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
        tcp_ = static_cast<Sock>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        sockaddr_in a = loopback(0);
        ::bind(rw(udp_), reinterpret_cast<sockaddr*>(&a), sizeof(a));
        ::bind(rw(tcp_), reinterpret_cast<sockaddr*>(&a), sizeof(a));
        ::listen(rw(tcp_), 8);
        udpPort_ = boundPort(udp_);
        tcpPort_ = boundPort(tcp_);
        thread_ = std::thread([this] { run(); });
    }
    ~StandInRouter() {
        stop_ = true;
        thread_.join();
        closeS(udp_);
        closeS(tcp_);
    }
    int ssdpPort() const { return udpPort_; }
    int httpPort() const { return tcpPort_; }
    void set(const Behaviour& b) { std::lock_guard<std::mutex> lk(mu_); beh_ = b; }
    Behaviour behaviour() const { std::lock_guard<std::mutex> lk(mu_); return beh_; }
    std::vector<SeenCall> calls() const { std::lock_guard<std::mutex> lk(mu_); return calls_; }
    std::size_t count(const std::string& action) const {
        std::lock_guard<std::mutex> lk(mu_);
        std::size_t n = 0;
        for (const SeenCall& c : calls_) { n += c.action == action ? 1 : 0; }
        return n;
    }
    std::vector<std::string> mapped() const { std::lock_guard<std::mutex> lk(mu_); return mapped_; }
    std::size_t descriptionFetches() const { return descFetches_.load(); }
    std::size_t ssdpSeen() const { return ssdpSeen_.load(); }
    std::string lastSsdpRequest() const { std::lock_guard<std::mutex> lk(mu_); return lastSsdp_; }

private:
    static std::string between(const std::string& s, const std::string& a, const std::string& b) {
        const std::size_t i = s.find(a);
        if (i == std::string::npos) { return std::string(); }
        const std::size_t j = s.find(b, i + a.size());
        return j == std::string::npos ? std::string() : s.substr(i + a.size(), j - i - a.size());
    }

    std::string description(const Behaviour& b) const {
        if (!b.descOverride.empty()) { return b.descOverride; }
        return "<?xml version=\"1.0\"?><root xmlns=\"urn:schemas-upnp-org:device-1-0\"><specVersion><major>1</major><minor>0</minor></specVersion>"
               "<URLBase>http://203.0.113.77:1/</URLBase>"
               "<device><deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType><friendlyName>Stand-in</friendlyName>"
               "<deviceList><device><deviceType>urn:schemas-upnp-org:device:WANDevice:1</deviceType><deviceList><device>"
               "<deviceType>urn:schemas-upnp-org:device:WANConnectionDevice:1</deviceType><serviceList>"
               "<service><serviceType>urn:schemas-upnp-org:service:WANCommonInterfaceConfig:1</serviceType><controlURL>/ctl/CmnIfCfg</controlURL></service>"
               "<service><serviceType>" + b.serviceType + "</serviceType><serviceId>urn:upnp-org:serviceId:WANIPConn1</serviceId><controlURL>" +
               b.controlUrl + "</controlURL></service></serviceList></device></deviceList></device></deviceList></device></root>";
    }

    static std::string soapOk(const std::string& action, const std::string& inner) {
        return "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
               "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" + action +
               "Response xmlns:u=\"urn:schemas-upnp-org:service:WANIPConnection:1\">" + inner + "</u:" + action + "Response></s:Body></s:Envelope>";
    }
    static std::string soapFault(int code) {
        return "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
               "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><s:Fault><faultcode>s:Client</faultcode>"
               "<faultstring>UPnPError</faultstring><detail><UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\"><errorCode>" +
               std::to_string(code) + "</errorCode><errorDescription>test fault</errorDescription></UPnPError></detail></s:Fault></s:Body></s:Envelope>";
    }

    static void sendAll(Sock c, const std::string& data) {
        std::size_t sent = 0;
        while (sent < data.size()) {
            const int n = static_cast<int>(::send(rw(c), data.data() + sent, static_cast<int>(data.size() - sent), 0));
            if (n <= 0) { return; }
            sent += static_cast<std::size_t>(n);
        }
    }

    static std::string httpReply(int status, const std::string& ctype, const std::string& body, bool chunked = false, const std::string& extra = "") {
        std::string r = "HTTP/1.1 " + std::to_string(status) + (status == 200 ? " OK" : (status == 500 ? " Internal Server Error" : " Found")) + "\r\n";
        r += "CONTENT-TYPE: " + ctype + "\r\nEXT:\r\nSERVER: stand-in UPnP/1.0 test/1\r\n" + extra;
        if (chunked) {
            r += "TRANSFER-ENCODING: chunked\r\n\r\n";
            std::size_t p = 0;
            while (p < body.size()) {
                const std::size_t n = std::min<std::size_t>(37, body.size() - p);
                char hex[16];
                std::snprintf(hex, sizeof(hex), "%zx", n);
                r += std::string(hex) + "\r\n" + body.substr(p, n) + "\r\n";
                p += n;
            }
            r += "0\r\n\r\n";
        } else {
            r += "CONTENT-LENGTH: " + std::to_string(body.size()) + "\r\n\r\n" + body;
        }
        return r;
    }

    void handleTcp(Sock c) {
        std::string req;
        char buf[4096];
        std::size_t need = std::string::npos;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline && req.size() < 65536) {
            fd_set s;
            FD_ZERO(&s);
            FD_SET(rw(c), &s);
            timeval tv{0, 100000};
            if (::select(static_cast<int>(rw(c)) + 1, &s, nullptr, nullptr, &tv) <= 0) { continue; }
            const int n = static_cast<int>(::recv(rw(c), buf, sizeof(buf), 0));
            if (n <= 0) { break; }
            req.append(buf, static_cast<std::size_t>(n));
            const std::size_t he = req.find("\r\n\r\n");
            if (he != std::string::npos && need == std::string::npos) {
                std::string head = req.substr(0, he);
                for (char& ch : head) { ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); }
                std::size_t cl = 0;
                const std::size_t p = head.find("content-length:");
                if (p != std::string::npos) { cl = static_cast<std::size_t>(std::atoi(head.c_str() + p + 15)); }
                need = he + 4 + cl;
            }
            if (need != std::string::npos && req.size() >= need) { break; }
        }
        const Behaviour b = behaviour();
        if (req.compare(0, 4, "GET ") == 0) {
            ++descFetches_;
            if (b.descStatus != 200) {
                sendAll(c, httpReply(b.descStatus, "text/plain", "", false, "LOCATION: http://203.0.113.77:1/elsewhere\r\n"));
            } else {
                sendAll(c, httpReply(200, "text/xml", description(b), b.chunkedDesc));
            }
            return;
        }
        // POST: the action is named in the SOAPACTION header, the arguments in the body
        const std::string action = between(req, "#", "\"");
        const std::size_t be = req.find("\r\n\r\n");
        const std::string body = be == std::string::npos ? std::string() : req.substr(be + 4);
        SeenCall call;
        call.action = action;
        for (const char* name : {"NewRemoteHost", "NewExternalPort", "NewProtocol", "NewInternalPort", "NewInternalClient", "NewEnabled",
                                 "NewPortMappingDescription", "NewLeaseDuration"}) {
            const std::string open = std::string("<") + name + ">";
            if (body.find(open) != std::string::npos) { call.args[name] = between(body, open, std::string("</") + name + ">"); }
            else if (body.find(std::string("<") + name + "></") != std::string::npos) { call.args[name] = ""; }
        }
        // the path the control request used is checked too
        call.args["_path"] = req.substr(5, req.find(' ', 5) - 5);
        {
            std::lock_guard<std::mutex> lk(mu_);
            calls_.push_back(call);
        }
        if (!b.answerControl) { std::this_thread::sleep_for(std::chrono::milliseconds(2000)); return; }
        if (action == "AddPortMapping") {
            if (b.addFault != 0) { sendAll(c, httpReply(500, "text/xml; charset=\"utf-8\"", soapFault(b.addFault))); return; }
            {
                std::lock_guard<std::mutex> lk(mu_);
                const std::string key = call.args["NewExternalPort"] + "/" + call.args["NewProtocol"];
                if (std::find(mapped_.begin(), mapped_.end(), key) == mapped_.end()) { mapped_.push_back(key); }
            }
            sendAll(c, httpReply(200, "text/xml; charset=\"utf-8\"", soapOk(action, "")));
        } else if (action == "DeletePortMapping") {
            if (b.deleteFault != 0) { sendAll(c, httpReply(500, "text/xml; charset=\"utf-8\"", soapFault(b.deleteFault))); return; }
            bool found = false;
            {
                std::lock_guard<std::mutex> lk(mu_);
                const std::string key = call.args["NewExternalPort"] + "/" + call.args["NewProtocol"];
                const auto it = std::find(mapped_.begin(), mapped_.end(), key);
                if (it != mapped_.end()) { mapped_.erase(it); found = true; }
            }
            if (!found) { sendAll(c, httpReply(500, "text/xml; charset=\"utf-8\"", soapFault(714))); return; }
            sendAll(c, httpReply(200, "text/xml; charset=\"utf-8\"", soapOk(action, "")));
        } else if (action == "GetExternalIPAddress") {
            sendAll(c, httpReply(200, "text/xml; charset=\"utf-8\"", soapOk(action, "<NewExternalIPAddress>" + b.externalIp + "</NewExternalIPAddress>")));
        } else {
            sendAll(c, httpReply(500, "text/xml; charset=\"utf-8\"", soapFault(401)));
        }
    }

    void run() {
        while (!stop_) {
            fd_set s;
            FD_ZERO(&s);
            FD_SET(rw(udp_), &s);
            FD_SET(rw(tcp_), &s);
            timeval tv{0, 50000};
            const int hi = static_cast<int>(std::max(rw(udp_), rw(tcp_))) + 1;
            if (::select(hi, &s, nullptr, nullptr, &tv) <= 0) { continue; }
            if (FD_ISSET(rw(udp_), &s)) {
                char buf[2048];
                sockaddr_in from{};
#if defined(_WIN32)
                int fl = sizeof(from);
#else
                socklen_t fl = sizeof(from);
#endif
                const int n = static_cast<int>(::recvfrom(rw(udp_), buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fl));
                if (n > 0) {
                    ++ssdpSeen_;
                    const Behaviour b = behaviour();
                    { std::lock_guard<std::mutex> lk(mu_); lastSsdp_.assign(buf, static_cast<std::size_t>(n)); }
                    if (b.answerSsdp) {
                        std::string r = "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=120\r\nEXT:\r\nLOCATION: http://" + b.locationHost + ":" +
                                        std::to_string(tcpPort_) + "/rootDesc.xml\r\nSERVER: stand-in UPnP/1.0\r\nST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n"
                                        "USN: uuid:00000000-0000-0000-0000-000000000000::urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n\r\n";
                        if (b.oversizedSsdp) { r += std::string(3000, 'x'); }
                        ::sendto(rw(udp_), r.data(), static_cast<int>(r.size()), 0, reinterpret_cast<sockaddr*>(&from), fl);
                    }
                }
            }
            if (FD_ISSET(rw(tcp_), &s)) {
                const Sock c = static_cast<Sock>(::accept(rw(tcp_), nullptr, nullptr));
                if (c != kBad) {
                    handleTcp(c);
                    closeS(c);
                }
            }
        }
    }

    Sock udp_ = kBad, tcp_ = kBad;
    int udpPort_ = 0, tcpPort_ = 0;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    mutable std::mutex mu_;
    Behaviour beh_;
    std::vector<SeenCall> calls_;
    std::vector<std::string> mapped_;
    std::string lastSsdp_;
    std::atomic<std::size_t> descFetches_{0};
    std::atomic<std::size_t> ssdpSeen_{0};
};

// The transport under test, fenced to the loopback block and counting every call. A target outside 127.0.0.0/8 is refused HERE, before a socket.
class LoopbackOnlyTransport final : public IgdTransport {
public:
    LoopbackOnlyTransport() : inner_(makeSocketIgdTransport()) {}
    bool ssdpSearch(const std::string& host, int port, const std::string& request, int timeoutMs, std::string& reply, std::string& from) override {
        ++ssdpCalls;
        if (host.compare(0, 4, "127.") != 0) { ++refused; reply.clear(); from.clear(); return false; }
        return inner_->ssdpSearch(host, port, request, timeoutMs, reply, from);
    }
    bool httpExchange(const HttpRequest& req, int timeoutMs, std::size_t maxBytes, std::string& raw) override {
        ++httpCalls;
        if (req.host.compare(0, 4, "127.") != 0) { ++refused; raw.clear(); return false; }
        return inner_->httpExchange(req, timeoutMs, maxBytes, raw);
    }
    std::atomic<int> ssdpCalls{0};
    std::atomic<int> httpCalls{0};
    std::atomic<int> refused{0};

private:
    std::shared_ptr<IgdTransport> inner_;
};

UpnpConfig cfgFor(const StandInRouter& r, std::vector<int> ports = {8073}) {
    UpnpConfig c;
    c.enabled = true;
    c.ssdpHost = "127.0.0.1";
    c.ssdpPort = r.ssdpPort();
    c.internalClient = "192.168.1.50";
    c.allowedPorts = std::move(ports);
    c.timeoutMs = 1500;
    return c;
}

// ===== tests =====

void testOffByDefault() {
    const UpnpConfig d;
    CHECK(!d.enabled);                                        // rule 1
    CHECK(d.ssdpHost == "239.255.255.250" && d.ssdpPort == 1900);   // the real group: never reaches a socket in this file
    CHECK(d.allowedPorts.empty());
    auto t = std::make_shared<LoopbackOnlyTransport>();
    {
        StandInRouter r;
        UpnpConfig c = cfgFor(r);
        c.enabled = false;
        UpnpMapper m(c, t);
        CHECK(m.discover() == UpnpResult::Disabled);
        CHECK(m.addMapping(8073) == UpnpResult::Disabled);
        CHECK(m.removeMapping(8073) == UpnpResult::Disabled);
        CHECK(m.removeAll() == UpnpResult::Disabled);
        std::string ip;
        CHECK(m.getExternalAddress(ip) == UpnpResult::Disabled && ip.empty());
        CHECK(r.ssdpSeen() == 0 && r.calls().empty());
    }                                                         // and the destructor with it off
    CHECK(t->ssdpCalls == 0 && t->httpCalls == 0);            // not one call reached the transport
    // a default config in a mapper is off too
    UpnpMapper m2(UpnpConfig{}, t);
    CHECK(m2.discover() == UpnpResult::Disabled && t->ssdpCalls == 0);
}

void testConfigValidation() {
    StandInRouter r;
    auto t = std::make_shared<LoopbackOnlyTransport>();
    auto valid = [&](const UpnpConfig& c) { return UpnpMapper(c, t).configValid(); };
    CHECK(valid(cfgFor(r)));
    UpnpConfig c = cfgFor(r);
    c.allowedPorts = {80};
    CHECK(!valid(c));                                         // below 1024
    c.allowedPorts = {1023};
    CHECK(!valid(c));
    c.allowedPorts = {1024};
    CHECK(valid(c));                                          // the boundary
    c.allowedPorts = {65536};
    CHECK(!valid(c));
    c.allowedPorts = {0};
    CHECK(!valid(c));
    c.allowedPorts = {-1};
    CHECK(!valid(c));
    c.allowedPorts = {8073, 8074, 8075, 8076};
    CHECK(valid(c));
    c.allowedPorts = {8073, 8074, 8075, 8076, 8077};
    CHECK(!valid(c));                                         // at most four
    c = cfgFor(r);
    c.leaseSeconds = 0;
    CHECK(!valid(c));                                         // no permanent mapping
    c.leaseSeconds = 59;
    CHECK(!valid(c));
    c.leaseSeconds = 60;
    CHECK(valid(c));
    c.leaseSeconds = 86400;
    CHECK(valid(c));
    c.leaseSeconds = 86401;
    CHECK(!valid(c));
    c = cfgFor(r);
    c.timeoutMs = 99;
    CHECK(!valid(c));
    c.timeoutMs = 30001;
    CHECK(!valid(c));
    c = cfgFor(r);
    c.ssdpHost = "8.8.8.8";
    CHECK(!valid(c));                                         // search only the group or the local network
    c.ssdpHost = "example.com";
    CHECK(!valid(c));
    c.ssdpHost = "239.255.255.250";
    CHECK(valid(c));
    c.ssdpHost = "192.168.1.1";
    CHECK(valid(c));
    c = cfgFor(r);
    c.description = "";
    CHECK(!valid(c));
    c.description = "line\nbreak";
    CHECK(!valid(c));
    CHECK(!UpnpMapper(cfgFor(r), nullptr).configValid());      // no transport
    // an invalid config refuses discovery and never reaches the transport
    c = cfgFor(r);
    c.allowedPorts = {80};
    UpnpMapper bad(c, t);
    CHECK(bad.discover() == UpnpResult::BadConfig && t->ssdpCalls == 0);
}

void testHappyPathAndShutdownRemoval() {
    StandInRouter r;
    auto t = std::make_shared<LoopbackOnlyTransport>();
    {
        UpnpMapper m(cfgFor(r), t);
        CHECK(m.addMapping(8073) == UpnpResult::NotDiscovered);       // before discover()
        CHECK(m.discover() == UpnpResult::Ok);
        // the M-SEARCH is the one UDA 1.0 section 1.2.2 shows
        const std::string ms = r.lastSsdpRequest();
        CHECK(ms.compare(0, 19, "M-SEARCH * HTTP/1.1") == 0);
        CHECK(ms.find("MAN: \"ssdp:discover\"\r\n") != std::string::npos);
        CHECK(ms.find("ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n") != std::string::npos);
        CHECK(ms.find("MX: ") != std::string::npos && ms.size() >= 4 && ms.compare(ms.size() - 4, 4, "\r\n\r\n") == 0);
        CHECK(m.addMapping(8073) == UpnpResult::Ok);
        CHECK((m.activePorts() == std::vector<int>{8073}));
        const auto calls = r.calls();
        CHECK(calls.size() == 1 && calls[0].action == "AddPortMapping");
        if (!calls.empty()) {
            const auto& a = calls[0].args;
            auto at = [&](const char* k) { const auto it = a.find(k); return it == a.end() ? std::string("<missing>") : it->second; };
            CHECK(at("NewRemoteHost").empty() && a.count("NewRemoteHost") == 1);       // present and empty: anyone
            CHECK(at("NewExternalPort") == "8073" && at("NewInternalPort") == "8073");  // the same number
            CHECK(at("NewProtocol") == "TCP");
            CHECK(at("NewInternalClient") == "192.168.1.50");
            CHECK(at("NewEnabled") == "1");
            CHECK(at("NewLeaseDuration") == "3600");                                    // finite
            CHECK(at("NewPortMappingDescription") == "FoxSDR web");
            CHECK(at("_path") == "ctl/IPConn" || at("_path") == "/ctl/IPConn" || at("_path").find("ctl/IPConn") != std::string::npos);
        }
        CHECK((r.mapped() == std::vector<std::string>{"8073/TCP"}));
        std::string ip;
        CHECK(m.getExternalAddress(ip) == UpnpResult::Ok && ip == "203.0.113.9");
        // adding the same port again is idempotent for the list
        CHECK(m.addMapping(8073) == UpnpResult::Ok);
        CHECK((m.activePorts() == std::vector<int>{8073}));
        // removal by hand
        CHECK(m.removeMapping(8073) == UpnpResult::Ok);
        CHECK(m.activePorts().empty() && r.mapped().empty());
        CHECK(m.addMapping(8073) == UpnpResult::Ok);                                     // and again, to be left for the destructor
        CHECK(!r.mapped().empty());
    }
    // rule 3: the destructor removed it
    CHECK(r.mapped().empty());
    CHECK(r.count("DeletePortMapping") == 2);
    CHECK(t->refused == 0);
}

void testOnlyNamedPorts() {
    StandInRouter r;
    auto t = std::make_shared<LoopbackOnlyTransport>();
    UpnpMapper m(cfgFor(r, {8073, 9000}), t);
    CHECK(m.discover() == UpnpResult::Ok);
    for (const int p : {80, 443, 22, 1023, 8072, 8074, 8999, 9001, 0, -1, 65535, 70000}) {
        CHECK(m.addMapping(p) == UpnpResult::PortNotAllowed);
    }
    CHECK(r.count("AddPortMapping") == 0);                                    // the router was never asked
    CHECK(m.addMapping(9000) == UpnpResult::Ok);
    CHECK(m.addMapping(8073) == UpnpResult::Ok);
    CHECK(r.count("AddPortMapping") == 2);
    CHECK(m.activePorts().size() == 2);
    // removal is only for what this object added
    CHECK(m.removeMapping(443) == UpnpResult::PortNotAllowed);
    CHECK(r.count("DeletePortMapping") == 0);
    // the internal client must be a private address
    for (const char* bad : {"", "8.8.8.8", "127.0.0.1", "0.0.0.0", "192.169.1.1", "172.32.0.1", "172.15.0.1", "11.0.0.1", "192.168.1", "fe80::1",
                            "192.168.1.1; evil", "router.local"}) {
        UpnpConfig c = cfgFor(r);
        c.internalClient = bad;
        UpnpMapper mm(c, t);
        CHECK(mm.discover() == UpnpResult::Ok);
        CHECK(mm.addMapping(8073) == UpnpResult::BadInternalClient);
    }
    for (const char* good : {"10.1.2.3", "172.16.0.9", "172.31.255.1", "192.168.0.7"}) {
        UpnpConfig c = cfgFor(r);
        c.internalClient = good;
        UpnpMapper mm(c, t);
        CHECK(mm.discover() == UpnpResult::Ok);
        CHECK(mm.addMapping(8073) == UpnpResult::Ok);
    }
    CHECK(t->refused == 0);
}

void testRouterFaults() {
    StandInRouter r;
    auto t = std::make_shared<LoopbackOnlyTransport>();
    UpnpMapper m(cfgFor(r), t);
    CHECK(m.discover() == UpnpResult::Ok);
    for (const int code : {718, 501, 402, 725, 724, 726, 727, 715, 716}) {
        Behaviour b = r.behaviour();
        b.addFault = code;
        r.set(b);
        CHECK(m.addMapping(8073) == UpnpResult::DeviceRefused);
        CHECK(m.lastErrorCode() == code);
        CHECK(m.activePorts().empty());                                      // a refused mapping is not recorded as active
    }
    // 725 (permanent leases only) is NOT retried with a permanent lease: the lease rule has no exception
    Behaviour b = r.behaviour();
    b.addFault = 725;
    r.set(b);
    const std::size_t before = r.count("AddPortMapping");
    CHECK(m.addMapping(8073) == UpnpResult::DeviceRefused);
    CHECK(r.count("AddPortMapping") == before + 1);
    for (const SeenCall& c : r.calls()) {
        if (c.action == "AddPortMapping") { CHECK(c.args.at("NewLeaseDuration") == "3600"); }
    }
    // success, then a delete the router has already forgotten (714) counts as removed
    b.addFault = 0;
    r.set(b);
    CHECK(m.addMapping(8073) == UpnpResult::Ok);
    b.deleteFault = 714;
    r.set(b);
    CHECK(m.removeMapping(8073) == UpnpResult::Ok && m.activePorts().empty());
    // a delete the router refuses for another reason leaves the port listed
    b.deleteFault = 0;
    r.set(b);
    CHECK(m.addMapping(8073) == UpnpResult::Ok);
    b.deleteFault = 501;
    r.set(b);
    CHECK(m.removeAll() == UpnpResult::DeviceRefused);
    CHECK((m.activePorts() == std::vector<int>{8073}));                       // rule 3: it stays listed so the caller can see it
    b.deleteFault = 0;
    r.set(b);
    CHECK(m.removeAll() == UpnpResult::Ok && m.activePorts().empty());
    // a router that hangs: Network, nothing recorded
    CHECK(m.addMapping(8073) == UpnpResult::Ok);
    b.answerControl = false;
    r.set(b);
    CHECK(m.removeMapping(8073) == UpnpResult::Network);
    CHECK(m.activePorts().size() == 1);
    b.answerControl = true;
    r.set(b);
    CHECK(m.removeAll() == UpnpResult::Ok);
}

void testDiscoveryRefusals() {
    auto t = std::make_shared<LoopbackOnlyTransport>();
    {   // no answer at all
        StandInRouter r;
        Behaviour b;
        b.answerSsdp = false;
        r.set(b);
        UpnpConfig c = cfgFor(r);
        c.timeoutMs = 300;
        UpnpMapper m(c, t);
        CHECK(m.discover() == UpnpResult::NoDevice);
        CHECK(m.addMapping(8073) == UpnpResult::NotDiscovered);
    }
    int http0 = t->httpCalls;
    // each refusal below must happen BEFORE any HTTP request is made (rule 4)
    for (const char* host : {"8.8.8.8", "203.0.113.5", "127.0.0.2", "router.local", "0.0.0.0", "999.1.1.1", "example.com"}) {
        StandInRouter r;
        Behaviour b;
        b.locationHost = host;
        r.set(b);
        UpnpMapper m(cfgFor(r), t);
        CHECK(m.discover() == UpnpResult::NoDevice);
        CHECK(t->httpCalls == http0);
        CHECK(t->refused == 0);
        CHECK(r.descriptionFetches() == 0);
    }
    {   // an answer over the datagram cap is not an SSDP reply
        StandInRouter r;
        Behaviour b;
        b.oversizedSsdp = true;
        r.set(b);
        UpnpConfig c = cfgFor(r);
        c.timeoutMs = 300;
        UpnpMapper m(c, t);
        CHECK(m.discover() == UpnpResult::NoDevice);
        CHECK(r.descriptionFetches() == 0);
    }
    {   // a redirect is not followed
        StandInRouter r;
        Behaviour b;
        b.descStatus = 302;
        r.set(b);
        UpnpMapper m(cfgFor(r), t);
        CHECK(m.discover() == UpnpResult::BadReply);
        CHECK(r.descriptionFetches() == 1);                                    // exactly one fetch: no second request to the Location
        CHECK(t->refused == 0);
    }
    const std::vector<std::pair<std::string, std::string>> badDesc = {
        {"not xml", "this is not xml"},
        {"truncated", "<root><device><serviceList><service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>"},
        {"doctype", "<?xml version=\"1.0\"?><!DOCTYPE lolz [<!ENTITY lol \"lol\">]><root>&lol;</root>"},
        {"no service", "<root><device><serviceList><service><serviceType>urn:other</serviceType><controlURL>/x</controlURL></service></serviceList></device></root>"},
        {"other host control", "<root><device><serviceList><service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>"
                               "<controlURL>http://203.0.113.5/ctl</controlURL></service></serviceList></device></root>"},
        {"network path control", "<root><device><serviceList><service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>"
                                 "<controlURL>//203.0.113.5/ctl</controlURL></service></serviceList></device></root>"},
        {"https control", "<root><device><serviceList><service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>"
                          "<controlURL>https://127.0.0.1/ctl</controlURL></service></serviceList></device></root>"},
        {"mismatched tags", "<root><device></root></device>"},
    };
    for (const auto& bd : badDesc) {
        StandInRouter r;
        Behaviour b;
        b.descOverride = bd.second;
        r.set(b);
        UpnpMapper m(cfgFor(r), t);
        const UpnpResult res = m.discover();
        CHECK(res == UpnpResult::NoDevice);
        if (res != UpnpResult::NoDevice) { std::printf("  description case '%s' gave %s\n", bd.first.c_str(), upnpResultText(res)); }
        CHECK(m.addMapping(8073) == UpnpResult::NotDiscovered);
        CHECK(r.count("AddPortMapping") == 0);
    }
    {   // a chunked description is understood
        StandInRouter r;
        Behaviour b;
        b.chunkedDesc = true;
        r.set(b);
        UpnpMapper m(cfgFor(r), t);
        CHECK(m.discover() == UpnpResult::Ok);
    }
    {   // WANIPConnection:2 and WANPPPConnection:1 are accepted; a look-alike is not
        for (const char* st : {"urn:schemas-upnp-org:service:WANIPConnection:2", "urn:schemas-upnp-org:service:WANPPPConnection:1"}) {
            StandInRouter r;
            Behaviour b;
            b.serviceType = st;
            r.set(b);
            UpnpMapper m(cfgFor(r), t);
            CHECK(m.discover() == UpnpResult::Ok);
        }
        StandInRouter r;
        Behaviour b;
        b.serviceType = "urn:schemas-upnp-org:service:WANIPConnection:9";
        r.set(b);
        UpnpMapper m(cfgFor(r), t);
        CHECK(m.discover() == UpnpResult::NoDevice);
    }
    {   // the description's URLBase (a public host) is ignored: every request still goes to the router that answered
        StandInRouter r;
        UpnpMapper m(cfgFor(r), t);
        CHECK(m.discover() == UpnpResult::Ok);
        CHECK(m.addMapping(8073) == UpnpResult::Ok);
        CHECK(t->refused == 0);
    }
    {   // a relative control URL resolves against the description's directory
        StandInRouter r;
        Behaviour b;
        b.controlUrl = "ctl/Relative";
        r.set(b);
        UpnpMapper m(cfgFor(r), t);
        CHECK(m.discover() == UpnpResult::Ok);
        CHECK(m.addMapping(8073) == UpnpResult::Ok);
        const auto calls = r.calls();
        CHECK(!calls.empty() && calls[0].args.at("_path").find("ctl/Relative") != std::string::npos);
    }
    CHECK(t->refused == 0);
}

void testParsers() {
    SsdpReply s;
    const std::string good = "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=120\r\nEXT:\r\nLOCATION: http://192.168.1.1:5000/rootDesc.xml\r\nST: x\r\nUSN: y\r\n\r\n";
    CHECK(parseSsdpReply(good, s) && s.location == "http://192.168.1.1:5000/rootDesc.xml" && s.st == "x" && s.usn == "y");
    CHECK(parseSsdpReply("HTTP/1.1 200 OK\r\nlocation:   http://10.0.0.1/d.xml  \r\n\r\n", s) && s.location == "http://10.0.0.1/d.xml");   // case, spacing
    CHECK(!parseSsdpReply("HTTP/1.1 200 OK\r\nST: x\r\n\r\n", s));                               // no LOCATION
    CHECK(!parseSsdpReply("HTTP/1.1 404 Not Found\r\nLOCATION: http://10.0.0.1/\r\n\r\n", s));
    CHECK(!parseSsdpReply("NOTIFY * HTTP/1.1\r\nLOCATION: http://10.0.0.1/\r\n\r\n", s));         // an advertisement is not a reply
    CHECK(!parseSsdpReply("M-SEARCH * HTTP/1.1\r\nLOCATION: http://10.0.0.1/\r\n\r\n", s));
    CHECK(!parseSsdpReply("", s));
    CHECK(!parseSsdpReply("HTTP/1.1 200 OK", s));
    CHECK(!parseSsdpReply("HTTP/1.1 200 OK\r\nLOCATION: http://10.0.0.1/\x01\r\n\r\n", s));      // a control character in a value
    CHECK(!parseSsdpReply(std::string(kMaxSsdpDatagram + 1, 'a'), s));
    CHECK(buildMSearch("239.255.255.250", 1900, "ssdp:all", 2) ==
          "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: ssdp:all\r\n\r\n");
    CHECK(buildMSearch("h", 1, "s", 99).find("MX: 5\r\n") != std::string::npos);                  // MX is bounded
    CHECK(buildMSearch("h", 1, "s", 0).find("MX: 1\r\n") != std::string::npos);

    UrlParts u;
    CHECK(parseHttpUrl("http://192.168.1.1:5000/rootDesc.xml", u) && u.host == "192.168.1.1" && u.port == 5000 && u.path == "/rootDesc.xml");
    CHECK(parseHttpUrl("http://10.0.0.1", u) && u.port == 80 && u.path == "/");
    CHECK(parseHttpUrl("HTTP://10.0.0.1/x?y=1", u) && u.path == "/x?y=1");
    for (const char* bad : {"https://192.168.1.1/", "http://router.local/", "http://192.168.1.1:0/", "http://192.168.1.1:99999/", "http://192.168.1.1:/",
                            "http://user@192.168.1.1/", "http://192.168.1.1/#frag", "http://192.168.1.1\\x", "http://192.168.1.1?x=1", "http:///x",
                            "http://[::1]/", "http://1.2.3/", "http://01.2.3.4/", "http://256.1.1.1/", "ftp://10.0.0.1/", "10.0.0.1", "",
                            "http://10.0.0.1/ space", "http://10.0.0.1/\x01"}) {
        CHECK(!parseHttpUrl(bad, u));
    }
    CHECK(!parseHttpUrl("http://10.0.0.1/" + std::string(600, 'a'), u));

    CHECK(isLocalNetworkAddress("10.0.0.1") && isLocalNetworkAddress("172.16.0.1") && isLocalNetworkAddress("172.31.255.255") &&
          isLocalNetworkAddress("192.168.0.1") && isLocalNetworkAddress("169.254.1.1") && isLocalNetworkAddress("127.0.0.1"));
    for (const char* pub : {"8.8.8.8", "172.15.255.255", "172.32.0.0", "192.167.1.1", "11.0.0.0", "169.253.1.1", "128.0.0.1", "0.0.0.0", "", "1.2.3",
                            "1.2.3.4.5", "a.b.c.d", "10.0.0.256", "10.0.0.1.", "010.0.0.1"}) {
        CHECK(!isLocalNetworkAddress(pub));
    }
    CHECK(isPrivateLanAddress("192.168.1.1") && !isPrivateLanAddress("127.0.0.1") && !isPrivateLanAddress("169.254.1.1"));

    // description
    UrlParts base;
    base.host = "192.168.1.1";
    base.port = 5000;
    base.path = "/desc/root.xml";
    auto desc = [&](const std::string& control) {
        return "<root><device><serviceList><service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType><controlURL>" + control +
               "</controlURL></service></serviceList></device></root>";
    };
    IgdService svc;
    std::string err;
    CHECK(parseDeviceDescription(desc("/ctl/x"), base, svc, err) && svc.controlPath == "/ctl/x");
    CHECK(parseDeviceDescription(desc("ctl/x"), base, svc, err) && svc.controlPath == "/desc/ctl/x");
    CHECK(parseDeviceDescription(desc("http://192.168.1.1:5000/ctl/y"), base, svc, err) && svc.controlPath == "/ctl/y");
    CHECK(!parseDeviceDescription(desc("http://192.168.1.1:5001/ctl/y"), base, svc, err));      // another port
    CHECK(!parseDeviceDescription(desc("http://192.168.1.2:5000/ctl/y"), base, svc, err));
    CHECK(!parseDeviceDescription(desc("//evil/ctl"), base, svc, err));
    CHECK(!parseDeviceDescription(desc("javascript://x"), base, svc, err));
    CHECK(!parseDeviceDescription(desc(""), base, svc, err));
    CHECK(!parseDeviceDescription(desc("/a b"), base, svc, err));
    CHECK(parseDeviceDescription("<?xml version=\"1.0\"?>\n<!-- c --><u:root xmlns:u=\"x\"><u:device><u:serviceList><u:service><u:serviceType>"
                                 "urn:schemas-upnp-org:service:WANIPConnection:1</u:serviceType><u:controlURL>/p&amp;q</u:controlURL></u:service>"
                                 "</u:serviceList></u:device></u:root>", base, svc, err) && svc.controlPath == "/p&q");   // prefixes, comments, entities
    CHECK(parseDeviceDescription("<root><device><serviceList><service><serviceType><![CDATA[urn:schemas-upnp-org:service:WANIPConnection:1]]></serviceType>"
                                 "<controlURL>/c</controlURL></service></serviceList></device></root>", base, svc, err));
    // depth and node caps: a hostile document is refused, not walked
    std::string deep;
    for (int i = 0; i < 40; ++i) { deep += "<a>"; }
    for (int i = 0; i < 40; ++i) { deep += "</a>"; }
    CHECK(!parseDeviceDescription(deep, base, svc, err));
    std::string wide = "<root>";
    for (int i = 0; i < 5000; ++i) { wide += "<n/>"; }
    wide += "</root>";
    CHECK(!parseDeviceDescription(wide, base, svc, err));
    // entity expansion cannot happen: an entity that is not one of the five (or a number) refuses the document
    CHECK(!parseDeviceDescription("<root>&bomb;</root>", base, svc, err));
    CHECK(!parseDeviceDescription("<!DOCTYPE x [<!ENTITY a \"aaaa\">]><root/>", base, svc, err));
    CHECK(!parseDeviceDescription("<root/><root/>", base, svc, err));                        // two roots

    // SOAP
    const std::string body = buildSoapBody("urn:schemas-upnp-org:service:WANIPConnection:1", "AddPortMapping", {{"NewRemoteHost", ""}, {"NewPortMappingDescription", "a<b>&\"c'"}});
    CHECK(body.find("<NewRemoteHost></NewRemoteHost>") != std::string::npos);
    CHECK(body.find("a&lt;b&gt;&amp;&quot;c&apos;") != std::string::npos);
    CHECK(body.find("xmlns:u=\"urn:schemas-upnp-org:service:WANIPConnection:1\"") != std::string::npos);
    CHECK(body.find("s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"") != std::string::npos);
    CHECK(xmlEscape(std::string("a\x01" "b")) == "a b");
    SoapReply sr;
    const std::string okBody = "<s:Envelope xmlns:s=\"x\"><s:Body><u:GetExternalIPAddressResponse xmlns:u=\"y\"><NewExternalIPAddress> 1.2.3.4 </NewExternalIPAddress>"
                               "</u:GetExternalIPAddressResponse></s:Body></s:Envelope>";
    CHECK(parseSoapReply(200, okBody, "GetExternalIPAddress", sr) && sr.ok && sr.out["NewExternalIPAddress"] == "1.2.3.4");
    CHECK(!parseSoapReply(200, okBody, "AddPortMapping", sr));                                // a response to another action
    CHECK(!parseSoapReply(200, "<s:Envelope><s:Body/></s:Envelope>", "AddPortMapping", sr));
    CHECK(!parseSoapReply(200, "junk", "AddPortMapping", sr));
    CHECK(!parseSoapReply(200, "<a><s:Body/></a>", "AddPortMapping", sr));
    const std::string fault = "<s:Envelope xmlns:s=\"x\"><s:Body><s:Fault><faultcode>s:Client</faultcode><detail><UPnPError xmlns=\"y\"><errorCode>718</errorCode>"
                              "<errorDescription>ConflictInMappingEntry</errorDescription></UPnPError></detail></s:Fault></s:Body></s:Envelope>";
    CHECK(parseSoapReply(500, fault, "AddPortMapping", sr) && !sr.ok && sr.errorCode == 718 && sr.errorDescription == "ConflictInMappingEntry");
    CHECK(!parseSoapReply(500, "<s:Envelope><s:Body><s:Fault/></s:Body></s:Envelope>", "AddPortMapping", sr));
    CHECK(parseSoapReply(500, fault, "AddPortMapping", sr));
    std::string badCode = fault;
    badCode.replace(badCode.find("718"), 3, "7x8");
    CHECK(!parseSoapReply(500, badCode, "AddPortMapping", sr));
    badCode = fault;
    badCode.replace(badCode.find("718"), 3, "9999999");
    CHECK(!parseSoapReply(500, badCode, "AddPortMapping", sr));                              // not a five-digit-or-less code
    CHECK(!parseSoapReply(404, okBody, "GetExternalIPAddress", sr));                          // an odd status with no fault

    // HTTP replies
    int st = 0;
    std::string hb;
    CHECK(parseHttpResponse("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhelloEXTRA", st, hb) && st == 200 && hb == "hello");
    CHECK(parseHttpResponse("HTTP/1.0 500 X\r\n\r\nbody", st, hb) && st == 500 && hb == "body");
    CHECK(parseHttpResponse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n6;ext=1\r\n world\r\n0\r\n\r\n", st, hb) && hb == "hello world");
    CHECK(!parseHttpResponse("HTTP/1.1 200 OK\r\nContent-Length: 50\r\n\r\nshort", st, hb));    // truncated
    CHECK(!parseHttpResponse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\nhello\r\n", st, hb));
    CHECK(!parseHttpResponse("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhel", st, hb));
    CHECK(!parseHttpResponse("HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n", st, hb));
    CHECK(!parseHttpResponse("HTTP/1.1 200 OK\r\nContent-Length: 99999999999\r\n\r\n", st, hb));
    CHECK(!parseHttpResponse("HTTP/1.1 2x0 OK\r\n\r\n", st, hb));
    CHECK(!parseHttpResponse("FTP 200\r\n\r\n", st, hb));
    CHECK(!parseHttpResponse("HTTP/1.1 200 OK\r\nno terminator", st, hb));
    CHECK(!parseHttpResponse("", st, hb));
    CHECK(st == 0 && hb.empty());
}

// FUZZ: every parser of router bytes ends in a verdict. Whatever it accepts satisfies the rules the callers rely on.
void testFuzz() {
    std::mt19937_64 rng(31337);
    const std::string ssdp = "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=120\r\nEXT:\r\nLOCATION: http://192.168.1.1:5000/rootDesc.xml\r\nST: a\r\nUSN: b\r\n\r\n";
    const std::string xml = "<?xml version=\"1.0\"?><root xmlns=\"urn:schemas-upnp-org:device-1-0\"><device><deviceList><device><serviceList><service>"
                            "<serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType><controlURL>/ctl/IPConn</controlURL></service>"
                            "</serviceList></device></deviceList></device></root>";
    const std::string soapOk = "<s:Envelope xmlns:s=\"x\"><s:Body><u:AddPortMappingResponse xmlns:u=\"y\"><NewA>1</NewA></u:AddPortMappingResponse></s:Body></s:Envelope>";
    const std::string soapFault = "<s:Envelope xmlns:s=\"x\"><s:Body><s:Fault><detail><UPnPError xmlns=\"y\"><errorCode>718</errorCode>"
                                  "<errorDescription>d</errorDescription></UPnPError></detail></s:Fault></s:Body></s:Envelope>";
    const std::string http = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Length: 11\r\n\r\n5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n";
    const std::string url = "http://192.168.1.1:5000/rootDesc.xml";
    const std::vector<const std::string*> seeds = {&ssdp, &xml, &soapOk, &soapFault, &http, &url};
    UrlParts base;
    base.host = "192.168.1.1";
    base.port = 5000;
    base.path = "/rootDesc.xml";
    int bad = 0;
    for (int i = 0; i < 30000; ++i) {
        std::string s = *seeds[rng() % seeds.size()];
        const int ops = static_cast<int>(rng() % 8);
        for (int k = 0; k < ops; ++k) {
            if (s.empty()) { break; }
            const std::size_t pos = rng() % s.size();
            switch (rng() % 5) {
                case 0: s[pos] = static_cast<char>(rng()); break;
                case 1: s.erase(pos, 1 + rng() % 6); break;
                case 2: s.insert(pos, 1, static_cast<char>(rng())); break;
                case 3: s.resize(pos); break;
                default: s.insert(pos, s.substr(rng() % s.size(), rng() % 40)); break;
            }
        }
        try {
            SsdpReply sr;
            if (parseSsdpReply(s, sr) && sr.location.empty()) { ++bad; }
            UrlParts up;
            if (parseHttpUrl(s, up)) {
                unsigned char dummy = 0;
                (void)dummy;
                if (!isLocalNetworkAddress(up.host) && up.host.empty()) { ++bad; }
                if (up.port < 1 || up.port > 65535 || up.path.empty() || up.path[0] != '/') { ++bad; }
                if (up.host.find_first_not_of("0123456789.") != std::string::npos) { ++bad; }
            }
            IgdService svc;
            std::string err;
            if (parseDeviceDescription(s, base, svc, err)) {
                if (svc.controlPath.empty() || svc.controlPath[0] != '/' || svc.controlPath.compare(0, 2, "//") == 0) { ++bad; }
                if (svc.serviceType.find("urn:schemas-upnp-org:service:WAN") != 0) { ++bad; }
            }
            SoapReply so;
            parseSoapReply(200, s, "AddPortMapping", so);
            parseSoapReply(500, s, "AddPortMapping", so);
            int st = 0;
            std::string body;
            if (parseHttpResponse(s, st, body)) {
                if (st < 0 || st > 999 || body.size() > kMaxIgdHttpReply) { ++bad; }
            }
        } catch (...) {
            ++bad;
        }
    }
    CHECK(bad == 0);
    // pure random bytes
    for (int i = 0; i < 5000; ++i) {
        std::string s(rng() % 300, '\0');
        for (char& c : s) { c = static_cast<char>(rng()); }
        try {
            SsdpReply sr; UrlParts up; IgdService svc; std::string err; SoapReply so; int st; std::string b;
            parseSsdpReply(s, sr); parseHttpUrl(s, up); parseDeviceDescription(s, base, svc, err);
            parseSoapReply(200, s, "X", so); parseSoapReply(500, s, "X", so); parseHttpResponse(s, st, b);
        } catch (...) { ++bad; }
    }
    CHECK(bad == 0);
}

// A router that answers the SOAP call with junk (not XML, huge, wrong status) never crashes the mapper and records nothing.
void testGarbageControlReplies() {
    StandInRouter r;
    auto t = std::make_shared<LoopbackOnlyTransport>();
    UpnpMapper m(cfgFor(r), t);
    CHECK(m.discover() == UpnpResult::Ok);
    // an unknown action path: the stand-in answers 401 for actions it does not know; use GetExternalIPAddress with a port it cannot parse
    Behaviour b = r.behaviour();
    b.externalIp = "not-an-address";
    r.set(b);
    std::string ip = "stale";
    CHECK(m.getExternalAddress(ip) == UpnpResult::BadReply && ip.empty());
    b.externalIp = "999.9.9.9";
    r.set(b);
    CHECK(m.getExternalAddress(ip) == UpnpResult::BadReply);
    b.externalIp = "<script>";
    r.set(b);
    CHECK(m.getExternalAddress(ip) == UpnpResult::BadReply);
}

void testSourceScan() {
    std::ifstream in(std::string(CASCADE_SOURCE_DIR) + "/src/net/upnp_igd.cpp", std::ios::binary);
    CHECK(static_cast<bool>(in));
    const std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(!src.empty());
    for (const char* banned : {"printf", "std::cout", "std::cerr", "fopen", "ofstream", "fstream", "fwrite", "OutputDebugString", "stderr", "stdout",
                               "settings_set", "getenv", "getaddrinfo", "gethostbyname", "WinHttp", "URLDownloadToFile", "InternetOpen"}) {
        CHECK(src.find(banned) == std::string::npos);                  // no logging, no files, and never a name lookup (literals only)
    }
    // the application does not call the model: nothing outside this file, the header and the tests names it
    std::ifstream h(std::string(CASCADE_SOURCE_DIR) + "/src/net/upnp_igd.hpp", std::ios::binary);
    CHECK(static_cast<bool>(h));
}

}  // namespace

int main() {
    netInit();
    testOffByDefault();
    testConfigValidation();
    testHappyPathAndShutdownRemoval();
    testOnlyNamedPorts();
    testRouterFaults();
    testDiscoveryRefusals();
    testParsers();
    testFuzz();
    testGarbageControlReplies();
    testSourceScan();
    return testSummary("test_upnp_igd");
}
