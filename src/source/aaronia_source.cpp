// aaronia_source.cpp - see aaronia_source.hpp and aaronia_protocol.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/aaronia_source.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>

#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace cascade::source {

namespace aa = aaronia;

namespace {

constexpr int kPollMs = 200;
constexpr std::size_t kMaxControlBody = 1024 * 1024;

#if defined(_WIN32)
using SockT = SOCKET;
const SockT kBad = INVALID_SOCKET;
void closeS(SockT s) { ::closesocket(s); }
void shutS(SockT s) { ::shutdown(s, SD_BOTH); }
bool netUp() {
    static const bool ok = [] {
        WSADATA d{};
        return ::WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    return ok;
}
using SockLen = int;
void setBlocking(SockT s, bool b) {
    u_long m = b ? 0 : 1;
    ::ioctlsocket(s, FIONBIO, &m);
}
#else
using SockT = int;
const SockT kBad = -1;
void closeS(SockT s) { ::close(s); }
void shutS(SockT s) { ::shutdown(s, SHUT_RDWR); }
bool netUp() { return true; }
using SockLen = socklen_t;
void setBlocking(SockT s, bool b) {
    const int f = ::fcntl(s, F_GETFL, 0);
    ::fcntl(s, F_SETFL, b ? (f & ~O_NONBLOCK) : (f | O_NONBLOCK));
}
#endif

bool readable(SockT s, int ms) {
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(s, &rf);
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
#if defined(_WIN32)
    return ::select(0, &rf, nullptr, nullptr, &tv) > 0;
#else
    return ::select(s + 1, &rf, nullptr, nullptr, &tv) > 0;
#endif
}

// A connected TCP socket, or kBad with `err` set. IPv4 only.
SockT connectTcp(const std::string& host, std::uint16_t port, int timeoutMs, std::string& err) {
    if (!netUp()) { err = "the network could not be started"; return kBad; }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || res == nullptr) {
        err = "could not find the host " + host + " on the network (only IPv4 addresses are supported; is the address right?)";
        return kBad;
    }
    sockaddr_in peer = *reinterpret_cast<sockaddr_in*>(res->ai_addr);
    ::freeaddrinfo(res);
    peer.sin_port = ::htons(port);
    const SockT s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kBad) { err = "could not open a TCP socket"; return kBad; }
    setBlocking(s, false);
    ::connect(s, reinterpret_cast<sockaddr*>(&peer), sizeof(peer));
    fd_set wf, ef;
    FD_ZERO(&wf);
    FD_ZERO(&ef);
    FD_SET(s, &wf);
    FD_SET(s, &ef);
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
#if defined(_WIN32)
    const int sr = ::select(0, nullptr, &wf, &ef, &tv);
#else
    const int sr = ::select(s + 1, nullptr, &wf, &ef, &tv);
#endif
    int soerr = 0;
    SockLen sl = sizeof(soerr);
    if (sr > 0) { ::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soerr), &sl); }
    if (sr <= 0 || soerr != 0 || FD_ISSET(s, &ef)) {
        closeS(s);
        err = "could not reach the Aaronia server at " + host + ":" + std::to_string(port) +
              (sr <= 0 ? " (no answer in time)" : " (connection refused)") + " - is the address right, and is the HTTP server block running?";
        return kBad;
    }
    setBlocking(s, true);
    int nodelay = 1;
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
    return s;
}

bool sendAllBytes(SockT s, const std::string& d) {
    std::size_t sent = 0;
    while (sent < d.size()) {
        const int r = ::send(s, d.data() + sent, static_cast<int>(d.size() - sent), 0);
        if (r <= 0) { return false; }
        sent += static_cast<std::size_t>(r);
    }
    return true;
}

// One short request on a fresh connection ("Connection: close"). Reads the answer within `timeoutMs` in
// all. False with `err` set when the exchange itself failed (no connection, no answer, not HTTP); a
// non-200 status is a successful exchange (the caller decides).
bool httpRoundTrip(const std::string& host, std::uint16_t port, const std::string& method, const std::string& path,
                   const std::string& body, int timeoutMs, int& status, std::string& respBody, std::string& err) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    const SockT s = connectTcp(host, port, timeoutMs, err);
    if (s == kBad) { return false; }
    struct Guard { SockT s; ~Guard() { closeS(s); } } g{s};
    std::string req = method + " " + path + " HTTP/1.1\r\nHost: " + host + ":" + std::to_string(port) + "\r\nConnection: close\r\nAccept: application/json\r\n";
    if (!body.empty() || method == "PUT" || method == "POST") {
        req += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    }
    req += "\r\n" + body;
    if (!sendAllBytes(s, req)) { err = "could not send the request to the Aaronia server"; return false; }
    std::string head;
    std::vector<std::uint8_t> rest;
    char buf[8192];
    auto msLeft = [&] { return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count()); };
    std::size_t headEnd = std::string::npos;
    while (headEnd == std::string::npos) {
        if (msLeft() <= 0 || !readable(s, std::min(msLeft(), kPollMs))) {
            if (msLeft() <= 0) { err = "the Aaronia server did not answer (receive timed out)"; return false; }
            continue;
        }
        const int n = ::recv(s, buf, sizeof(buf), 0);
        if (n <= 0) { err = "the Aaronia server closed the connection before it answered"; return false; }
        head.append(buf, static_cast<std::size_t>(n));
        headEnd = head.find("\r\n\r\n");
        if (headEnd == std::string::npos && head.size() > aa::kMaxHeadBytes) { err = "the Aaronia server's reply header is too long"; return false; }
    }
    aa::HttpHead hh;
    if (!aa::parseHttpHead(head.substr(0, headEnd), hh, err)) { return false; }
    status = hh.status;
    std::string bodyBytes = head.substr(headEnd + 4);
    aa::ChunkDecoder cd;
    std::vector<std::uint8_t> decoded;
    if (hh.chunked) { cd.feed(reinterpret_cast<const std::uint8_t*>(bodyBytes.data()), bodyBytes.size(), decoded); }
    auto done = [&] {
        if (hh.chunked) { return cd.finished() || !cd.error().empty(); }
        if (hh.contentLength >= 0) { return static_cast<long long>(bodyBytes.size()) >= hh.contentLength; }
        return false;  // until the server closes
    };
    while (!done()) {
        if (msLeft() <= 0) { err = "the Aaronia server did not finish its answer (receive timed out)"; return false; }
        if (!readable(s, std::min(msLeft(), kPollMs))) { continue; }
        const int n = ::recv(s, buf, sizeof(buf), 0);
        if (n <= 0) { break; }
        if (hh.chunked) { cd.feed(reinterpret_cast<const std::uint8_t*>(buf), static_cast<std::size_t>(n), decoded); }
        else { bodyBytes.append(buf, static_cast<std::size_t>(n)); }
        if (bodyBytes.size() > kMaxControlBody || decoded.size() > kMaxControlBody) { err = "the Aaronia server's answer is larger than this client accepts"; return false; }
    }
    if (hh.chunked) {
        if (!cd.error().empty()) { err = "the Aaronia server's chunked answer is malformed: " + cd.error(); return false; }
        respBody.assign(decoded.begin(), decoded.end());
    } else {
        if (hh.contentLength >= 0 && static_cast<long long>(bodyBytes.size()) > hh.contentLength) { bodyBytes.resize(static_cast<std::size_t>(hh.contentLength)); }
        respBody = bodyBytes;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------------------
// the stream connection, shared with the reader thread
// ---------------------------------------------------------------------------------------

struct AaroniaSource::Stream {
    explicit Stream(aa::SampleFormat f) : parser(f) {}
    SockT sock = kBad;
    std::string head;
    bool headDone = false, chunked = false;
    aa::ChunkDecoder chunks;
    aa::RecordParser parser;
    std::vector<std::uint8_t> decoded;
    std::vector<aa::IqRecord> recs;
    std::vector<std::uint8_t> buf = std::vector<std::uint8_t>(65536);
    std::atomic<double> rate{0.0}, center{0.0};
    std::atomic<std::uint64_t> gaps{0}, others{0}, nonFinite{0};  // mirrors of the parser's counters, safe to read from other threads
    double lastEnd = -1.0;
    int silentTicks = 0, silentLimit = 15;

    ~Stream() {
        if (sock != kBad) { closeS(sock); }
    }

    bool pump(NetLink& link) {
        if (!readable(sock, kPollMs)) {
            if (!link.run.load(std::memory_order_relaxed)) { return false; }
            if (!link.streaming.load(std::memory_order_relaxed)) { silentTicks = 0; }
            else if (++silentTicks >= silentLimit) {
                link.fault("the Aaronia server stopped sending", "no I/Q data arrived");
                return false;
            }
            return true;
        }
        const int n = ::recv(sock, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0);
        if (n <= 0) {
            if (!link.run.load(std::memory_order_relaxed)) { return false; }
            link.fault("the Aaronia server closed the stream", n == 0 ? "end of stream" : "receive error");
            return false;
        }
        const std::uint8_t* p = buf.data();
        std::size_t len = static_cast<std::size_t>(n);
        if (!headDone) {
            head.append(reinterpret_cast<const char*>(p), len);
            const std::size_t e = head.find("\r\n\r\n");
            if (e == std::string::npos) {
                if (head.size() > aa::kMaxHeadBytes) { link.fault("the Aaronia server's stream reply is malformed", "the header is too long"); return false; }
                return true;
            }
            aa::HttpHead hh;
            std::string err;
            if (!aa::parseHttpHead(head.substr(0, e), hh, err)) { link.fault("the Aaronia server's stream reply is malformed", err); return false; }
            if (hh.status == 401 || hh.status == 403) {
                link.fault("the Aaronia server wants a user name and password for the stream", "this build does not send credentials");
                return false;
            }
            if (hh.status != 200) { link.fault("the Aaronia server refused the stream", "HTTP status " + std::to_string(hh.status)); return false; }
            headDone = true;
            chunked = hh.chunked;
            const std::string rest = head.substr(e + 4);
            head.clear();
            decoded.clear();
            if (chunked) { chunks.feed(reinterpret_cast<const std::uint8_t*>(rest.data()), rest.size(), decoded); }
            else { decoded.assign(rest.begin(), rest.end()); }
        } else {
            decoded.clear();
            if (chunked) { chunks.feed(p, len, decoded); }
            else { decoded.assign(p, p + len); }
        }
        if (chunked && !chunks.error().empty()) { link.fault("the Aaronia server's stream is malformed", chunks.error()); return false; }
        recs.clear();
        if (!decoded.empty()) { parser.feed(decoded.data(), decoded.size(), recs); }
        others.store(parser.otherPackets(), std::memory_order_relaxed);
        nonFinite.store(parser.nonFiniteSamples(), std::memory_order_relaxed);
        if (!parser.error().empty()) { link.fault("the Aaronia server's stream is malformed", parser.error()); return false; }
        for (const aa::IqRecord& r : recs) {
            if (r.samples.empty()) { continue; }
            const double rt = r.rateHz();
            if (rt > 0.0) {
                const double old = rate.load();
                rate.store(old > 0.0 ? old * 0.9 + rt * 0.1 : rt);
            }
            if (r.hasFrequency) { center.store(0.5 * (r.startFrequency + r.endFrequency)); }
            if (lastEnd >= 0.0 && r.endTime > r.startTime) {
                const double dur = r.endTime - r.startTime;
                if (r.startTime - lastEnd > 0.5 * dur + 0.001) { gaps.fetch_add(1, std::memory_order_relaxed); }
            }
            lastEnd = r.endTime;
            link.pushSamples(r.samples.data(), r.samples.size());
        }
        if (chunked && chunks.finished()) {
            link.fault("the Aaronia server ended the stream", "the last chunk arrived");
            return false;
        }
        silentTicks = 0;
        return true;
    }
};

// ---------------------------------------------------------------------------------------
// the source
// ---------------------------------------------------------------------------------------

AaroniaSource::~AaroniaSource() { closeDevice(); }

bool AaroniaSource::failOpen(const std::string& why) {
    setError(why);
    return false;
}

bool AaroniaSource::open(const std::string& args) {
    closeDevice();
    std::lock_guard<std::mutex> lk(ctlMutex_);
    std::string host = "127.0.0.1", err;
    std::uint16_t port = aa::kDefaultPort;
    const std::string addr = argValue(args, "aaronia");
    if (!addr.empty() && !parseHostPort(addr, aa::kDefaultPort, host, port, err)) { return failOpen(err); }
    timeoutMs_ = 3000;
    const std::string to = argValue(args, "timeout");
    if (!to.empty()) {
        const long v = std::strtol(to.c_str(), nullptr, 10);
        if (v < 100 || v > 60000) { return failOpen("the timeout must be 100 to 60000 ms"); }
        timeoutMs_ = static_cast<int>(v);
    }
    const std::string fmt = argValue(args, "format");
    format_ = aa::SampleFormat::Json;
    if (fmt == "float32") { format_ = aa::SampleFormat::Float32; }
    else if (!fmt.empty() && fmt != "json") { return failOpen("the format must be json or float32"); }
    spanHz_ = 0.0;
    captureDirty_ = false;
    const std::string rt = argValue(args, "rate");
    if (!rt.empty()) {
        char* e = nullptr;
        const double v = std::strtod(rt.c_str(), &e);
        if (e == rt.c_str() || *e != '\0' || !(v >= 1000.0) || v > 1e9) { return failOpen("the rate must be 1000 to 1000000000 samples per second"); }
        spanHz_ = v;
        captureDirty_ = true;
    }
    int status = 0;
    std::string body;
    if (!httpRoundTrip(host, port, "GET", "/info", "", timeoutMs_, status, body, err)) { return failOpen(err); }
    if (status == 401 || status == 403) {
        return failOpen("the Aaronia server wants a user name and password, which this build does not send (the document describes Basic and RToken authorisation)");
    }
    if (status != 200) { return failOpen("the server at " + host + ":" + std::to_string(port) + " answered /info with HTTP status " + std::to_string(status) + ": it does not look like an RTSA HTTP server block"); }
    const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("name") || !j["name"].is_string()) {
        return failOpen("the server at " + host + ":" + std::to_string(port) + " did not answer /info as an RTSA HTTP server block does");
    }
    std::string nm = j["name"].get<std::string>();
    std::string clean;
    for (char c : nm) { if (clean.size() >= 40) { break; } clean += (c >= 32 && c < 127) ? c : '?'; }
    serverName_ = clean;
    host_ = host;
    port_ = port;
    setError("");
    markOpen(true);
    setName("Aaronia RTSA: " + serverName_ + " (" + host_ + ")");
    return true;
}

void AaroniaSource::closeStream() {
    endReader();  // also ends keeping samples
    stream_.reset();
}

void AaroniaSource::closeDevice() {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    if (!isOpen() && !stream_) { endReader(); return; }
    const std::string keep = lastError();
    const bool wasActive = active_;
    closeStream();
    if (wasActive) { control(aa::streamingRequest(false), "stopping the stream"); }
    active_ = false;
    markOpen(false);
    if (!keep.empty()) { setError(keep); }
    setName("network radio: (not connected)");
}

bool AaroniaSource::control(const std::string& json, const char* what) {
    int status = 0;
    std::string body, err;
    if (!httpRoundTrip(host_, port_, "PUT", "/control", json, timeoutMs_, status, body, err)) {
        setError(std::string(what) + ": " + err);
        return false;
    }
    if (status == 401 || status == 403) { setError(std::string(what) + ": the server wants credentials, which this build does not send"); return false; }
    if (status < 200 || status >= 300) { setError(std::string(what) + ": the server answered HTTP status " + std::to_string(status)); return false; }
    return true;
}

bool AaroniaSource::sendCapture() {
    if (!(centerHz_ > 0.0) && !(spanHz_ > 0.0)) { return true; }
    if (!control(aa::captureRequest(centerHz_, spanHz_ > 0.0 ? spanHz_ : 0.0), "setting the capture")) { return false; }
    captureDirty_ = false;
    return true;
}

bool AaroniaSource::start() {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    if (!isOpen()) { setError("start() called with no Aaronia server open"); return false; }
    if (faulted()) { return false; }
    if (active_) { return true; }
    if (captureDirty_ && !sendCapture()) { return false; }
    if (!control(aa::streamingRequest(true), "starting the stream")) { return false; }
    std::string err;
    const SockT s = connectTcp(host_, port_, timeoutMs_, err);
    if (s == kBad) { setError(err); return false; }
    auto st = std::make_shared<Stream>(format_);
    st->sock = s;
    st->silentLimit = std::max(3, timeoutMs_ / kPollMs);
    std::string path = "/stream?format=";
    path += format_ == aa::SampleFormat::Json ? "json" : "float32";
    path += "&rate_adaption=0";
    const std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host_ + ":" + std::to_string(port_) + "\r\nAccept: */*\r\n\r\n";
    if (!sendAllBytes(s, req)) { setError("could not send the stream request"); return false; }
    stream_ = st;
    if (!beginReader([st](NetLink& l) { return st->pump(l); }, [st]() { shutS(st->sock); }, st)) {
        stream_.reset();
        return false;
    }
    active_ = true;
    return startKeeping();
}

void AaroniaSource::stop() {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    const bool was = active_;
    closeStream();
    active_ = false;
    if (was) { control(aa::streamingRequest(false), "stopping the stream"); }
}

double AaroniaSource::sampleRateHz() const {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    if (stream_) {
        const double r = stream_->rate.load();
        if (r > 0.0) { return r; }
    }
    return spanHz_;
}

std::vector<double> AaroniaSource::supportedSampleRatesHz() const {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    return spanHz_ > 0.0 ? std::vector<double>{spanHz_} : std::vector<double>{};
}

bool AaroniaSource::setSampleRateHz(double hz) {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    if (!isOpen()) { setError("setSampleRateHz() called with no Aaronia server open"); return false; }
    if (!std::isfinite(hz) || hz < 1000.0 || hz > 1e9) { setError("the rate must be 1000 to 1000000000 samples per second"); return false; }
    spanHz_ = hz;
    captureDirty_ = true;
    if (active_) { return sendCapture(); }
    return true;
}

double AaroniaSource::centerFrequencyHz() const {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    if (stream_) {
        const double c = stream_->center.load();
        if (c > 0.0) { return c; }
    }
    return centerHz_;
}

bool AaroniaSource::setCenterFrequencyHz(double hz) {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    if (!isOpen()) { setError("setCenterFrequencyHz() called with no Aaronia server open"); return false; }
    if (!(hz > 0.0) || !std::isfinite(hz) || hz > 1e12) { setError("setCenterFrequencyHz() requires a positive frequency"); return false; }
    centerHz_ = hz;
    captureDirty_ = true;
    if (active_) { return sendCapture(); }
    return true;
}

std::string AaroniaSource::serverName() const {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    return serverName_;
}
std::uint64_t AaroniaSource::gapCount() const {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    return stream_ ? stream_->gaps.load() : 0;
}
std::uint64_t AaroniaSource::otherPackets() const {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    return stream_ ? stream_->others.load() : 0;
}
std::uint64_t AaroniaSource::nonFiniteSamples() const {
    std::lock_guard<std::mutex> lk(ctlMutex_);
    return stream_ ? stream_->nonFinite.load() : 0;
}

}  // namespace cascade::source
