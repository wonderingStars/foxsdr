// upnp_igd.hpp - a UPnP Internet Gateway Device client: ask the home router to forward ONE named TCP port to this machine (feat/webmodels).
//
// ===== OWNER DECISION, NOT SWITCHED ON BY THIS FILE =====
// A port mapping opens the home router to the internet. Nothing in the application calls this model: it is built, tested against a stand-in
// router on 127.0.0.1 only, and left unwired. Wiring it (a settings switch, off by default; the page behind it needs the password and the
// HTTPS work of the web-secure lane first) is the owner's decision, listed in docs/feature-records/webmodels.md. It was NEVER run against a real
// router or the real network: every test target is 127.0.0.1.
//
// SOURCES (opened 2026-11-11): UPnP Device Architecture 1.0 (15 October 2008): section 1.2.2 M-SEARCH, 1.2.3 search response, the control
// section (SOAP POST, SOAPACTION, response and UPnPError forms); "WANIPConnection:1 Service Template Version 1.01" (12 November 2001, UPnP
// Forum, standardized DCP): AddPortMapping (8 arguments, in order), DeletePortMapping (3), GetExternalIPAddress, error codes 402, 501, 714,
// 715, 716, 718, 724, 725, 726, 727. Both are published free of charge by the Open Connectivity Foundation (openconnectivity.org). They are
// "provided as is" and say membership terms govern use in compliant devices: the owner decision above includes that.
//
// RULES (each tested in tests/test_upnp_igd.cpp; the parsers are fuzzed):
//   1. OFF BY DEFAULT: a default-constructed UpnpConfig has enabled = false, and then NO call reaches the transport (tests count the calls).
//   2. ONLY PORTS THE USER NAMED: addMapping(port) is refused unless `port` is in UpnpConfig::allowedPorts (at most 4, each 1024..65535). The
//      internal and external port are the same number (no mapping of one port to another), TCP only, remote host "" (anyone), and a FINITE
//      lease (60..86400 s, default 3600): a crash can leave a mapping only until its lease runs out.
//   3. REMOVED ON SHUTDOWN: ~UpnpMapper() deletes every mapping it added (removeAll()); a mapping that could not be removed stays listed in
//      activePorts() so the caller can see it.
//   4. THE ROUTER MUST BE ON THE LOCAL NETWORK: the SSDP reply, the description URL and the control URL must all name the same private,
//      link-local or loopback IPv4 LITERAL (never a name, never a public address, never a different host than the one that answered); an
//      HTTP redirect is never followed; URLBase in the description is ignored.
//   5. THE INTERNAL CLIENT must be a private IPv4 (10/8, 172.16/12, 192.168/16): a mapping to a public or loopback address is refused.
//   6. Replies are bounded (datagram 2 KB, HTTP reply 128 KB, XML depth 16, 2000 elements) and parsed without entity expansion: a DOCTYPE or
//      ENTITY declaration is refused, so no reply can inflate itself.
//   7. Nothing the router says reaches a log: this file prints nothing, writes nothing, and keeps the router's address and the external IP
//      only in memory (getExternalAddress() hands the external address to the caller, never stores it past the call).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace cascade::net {

inline constexpr std::size_t kMaxSsdpDatagram = 2048;
inline constexpr std::size_t kMaxIgdHttpReply = 128 * 1024;
inline constexpr std::size_t kMaxIgdXmlDepth = 16;
inline constexpr std::size_t kMaxIgdXmlNodes = 2000;
inline constexpr std::size_t kMaxIgdAllowedPorts = 4;

// ---- Parsers of outside bytes (all total: they never throw, never read past the buffer) ---------------------------------------------------

struct SsdpReply {
    std::string location;  // the description URL
    std::string st;
    std::string usn;
};
// "HTTP/1.1 200 OK" with headers; header names case-insensitive; LOCATION required. false for anything else (including a NOTIFY or an M-SEARCH).
bool parseSsdpReply(const std::string& datagram, SsdpReply& out);

std::string buildMSearch(const std::string& host, int port, const std::string& searchTarget, int mxSeconds);

struct UrlParts {
    std::string host;  // an IPv4 literal
    int port = 80;
    std::string path = "/";
};
// http://a.b.c.d[:port][/path] only. Hosts that are names, https, userinfo, fragments, control characters: false.
bool parseHttpUrl(const std::string& url, UrlParts& out);

// Private (10/8, 172.16/12, 192.168/16), link-local (169.254/16) or loopback IPv4 literal.
bool isLocalNetworkAddress(const std::string& ipv4);
bool isPrivateLanAddress(const std::string& ipv4);  // the first three only

struct IgdService {
    std::string serviceType;  // urn:schemas-upnp-org:service:WANIPConnection:1 / :2 or WANPPPConnection:1
    std::string controlPath;  // an absolute path on the SAME host and port as the description
};
bool parseDeviceDescription(const std::string& xml, const UrlParts& base, IgdService& out, std::string& error);

std::string buildSoapBody(const std::string& serviceType, const std::string& action, const std::vector<std::pair<std::string, std::string>>& args);
std::string xmlEscape(const std::string& s);

struct SoapReply {
    bool ok = false;
    int errorCode = 0;            // the UPnPError code of a fault, else 0
    std::string errorDescription;
    std::map<std::string, std::string> out;  // the response's out arguments
};
bool parseSoapReply(int httpStatus, const std::string& body, const std::string& action, SoapReply& out);

// Splits an HTTP/1.x reply (status line, headers, body) read to the end of the connection; honours Content-Length and chunked coding.
bool parseHttpResponse(const std::string& raw, int& status, std::string& body);

// ---- The transport (replaceable) ---------------------------------------------------------------------------------------------------

struct HttpRequest {
    std::string method;  // "GET" or "POST"
    std::string host;
    int port = 80;
    std::string path;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

class IgdTransport {
public:
    virtual ~IgdTransport() = default;
    // Sends one M-SEARCH to host:port and returns the first reply datagram and the IPv4 literal it came from, or false after timeoutMs.
    virtual bool ssdpSearch(const std::string& host, int port, const std::string& request, int timeoutMs, std::string& reply,
                            std::string& fromAddress) = 0;
    // One HTTP exchange; the reply is the raw bytes read to the end of the connection (at most maxBytes).
    virtual bool httpExchange(const HttpRequest& req, int timeoutMs, std::size_t maxBytes, std::string& rawReply) = 0;
};

// Plain sockets (Windows and POSIX). Targets are taken as given: the CALLER's rules (above) keep it to the local network.
std::shared_ptr<IgdTransport> makeSocketIgdTransport();

// ---- The mapper --------------------------------------------------------------------------------------------------------------------

struct UpnpConfig {
    bool enabled = false;                      // rule 1
    std::string ssdpHost = "239.255.255.250";  // the standard SSDP group; a test passes 127.0.0.1
    int ssdpPort = 1900;
    std::string internalClient;                // this machine's LAN address, e.g. "192.168.1.20"
    std::vector<int> allowedPorts;             // rule 2
    int leaseSeconds = 3600;
    int timeoutMs = 3000;
    std::string description = "FoxSDR web";
};

enum class UpnpResult {
    Ok,
    Disabled,
    BadConfig,
    NotDiscovered,     // addMapping before a successful discover()
    NoDevice,          // no reply, or the reply failed rule 4
    BadReply,
    Network,
    PortNotAllowed,    // rule 2
    BadInternalClient, // rule 5
    DeviceRefused,     // the router answered with a UPnPError (see lastErrorCode())
};

const char* upnpResultText(UpnpResult r);

class UpnpMapper {
public:
    UpnpMapper(const UpnpConfig& cfg, std::shared_ptr<IgdTransport> transport);
    ~UpnpMapper();
    UpnpMapper(const UpnpMapper&) = delete;
    UpnpMapper& operator=(const UpnpMapper&) = delete;

    UpnpResult discover();
    UpnpResult addMapping(int port);
    UpnpResult removeMapping(int port);
    UpnpResult removeAll();  // rule 3
    // The router's external IPv4 as text, for showing the user the address to share; never stored.
    UpnpResult getExternalAddress(std::string& out);

    std::vector<int> activePorts() const;
    int lastErrorCode() const;
    bool configValid() const { return configValid_; }

private:
    UpnpResult soap(const std::string& action, const std::vector<std::pair<std::string, std::string>>& args, SoapReply& reply);

    UpnpConfig cfg_;
    bool configValid_ = false;
    std::shared_ptr<IgdTransport> transport_;
    mutable std::mutex mu_;
    bool discovered_ = false;
    UrlParts router_;
    IgdService service_;
    std::vector<int> active_;
    int lastError_ = 0;
};

}  // namespace cascade::net
