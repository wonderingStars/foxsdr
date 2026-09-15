// The IIOD client itself. See iiod_client.hpp for the provenance of the
// protocol and for which of the daemon's own handlers each reply format came
// from; this file is the socket, the line discipline and the context scanner.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/iiod_client.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
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

namespace cascade::source::iiod {
namespace {

// The socket helpers, in the same shape src/net/cat_server.cpp uses: an
// intptr_t handle so one type covers both platforms, and a startup that
// happens exactly once without any ordering requirement on the caller. They
// are duplicated rather than shared because cat_server's are file-local to a
// server and this is a client - two small copies that can be read in one
// sitting beat a header that has to be general enough for both.
#if defined(_WIN32)
constexpr std::intptr_t kInvalidSock = static_cast<std::intptr_t>(INVALID_SOCKET);

void closeSock(std::intptr_t s) {
    if (s != kInvalidSock) { ::closesocket(static_cast<SOCKET>(s)); }
}

bool ensureWinsock() {
    static const bool ok = [] {
        WSADATA d{};
        return ::WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    return ok;
}

int lastSocketError() { return ::WSAGetLastError(); }
bool wouldBlock(int err) { return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS; }
bool isTimeout(int err) { return err == WSAETIMEDOUT || err == WSAEWOULDBLOCK; }

void setNonBlocking(std::intptr_t s, bool on) {
    u_long mode = on ? 1u : 0u;
    ::ioctlsocket(static_cast<SOCKET>(s), FIONBIO, &mode);
}

void setIoTimeouts(std::intptr_t s, std::chrono::milliseconds w) {
    DWORD ms = static_cast<DWORD>(w.count());
    ::setsockopt(static_cast<SOCKET>(s), SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&ms), sizeof(ms));
    ::setsockopt(static_cast<SOCKET>(s), SOL_SOCKET, SO_SNDTIMEO,
                 reinterpret_cast<const char*>(&ms), sizeof(ms));
}
#else
constexpr std::intptr_t kInvalidSock = -1;

void closeSock(std::intptr_t s) {
    if (s != kInvalidSock) { ::close(static_cast<int>(s)); }
}

bool ensureWinsock() { return true; }

int lastSocketError() { return errno; }
bool wouldBlock(int err) { return err == EINPROGRESS || err == EAGAIN || err == EWOULDBLOCK; }
bool isTimeout(int err) { return err == EAGAIN || err == EWOULDBLOCK || err == EINTR; }

void setNonBlocking(std::intptr_t s, bool on) {
    const int fd = static_cast<int>(s);
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) { return; }
    flags = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    ::fcntl(fd, F_SETFL, flags);
}

void setIoTimeouts(std::intptr_t s, std::chrono::milliseconds w) {
    timeval tv{};
    tv.tv_sec = static_cast<long>(w.count() / 1000);
    tv.tv_usec = static_cast<long>((w.count() % 1000) * 1000);
    ::setsockopt(static_cast<int>(s), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(static_cast<int>(s), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}
#endif

std::string socketErrorText(const char* what, int err) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s failed (network error %d)", what, err);
    return buf;
}

// The real transport. Everything it can do is send, receive and close, and
// every one of those is bounded by the socket's own timeouts - see
// setIoTimeouts above.
class TcpTransport : public Transport {
public:
    TcpTransport(std::intptr_t sock, std::chrono::milliseconds ioWait)
        : sock_(sock), ioWait_(ioWait) {
        (void)ioWait_;
    }
    ~TcpTransport() override { close(); }

    bool sendAll(const void* data, std::size_t n) override {
        const char* p = static_cast<const char*>(data);
        std::size_t sent = 0;
        while (sent < n) {
            if (sock_ == kInvalidSock) {
                error_ = "the connection is closed";
                return false;
            }
#if defined(_WIN32)
            const int got = ::send(static_cast<SOCKET>(sock_), p + sent,
                                   static_cast<int>(n - sent), 0);
#else
            const int got = static_cast<int>(
                ::send(static_cast<int>(sock_), p + sent, n - sent, MSG_NOSIGNAL));
#endif
            if (got <= 0) {
                const int err = lastSocketError();
                error_ = isTimeout(err)
                             ? "the Pluto stopped accepting commands (send timed out)"
                             : socketErrorText("send", err);
                return false;
            }
            sent += static_cast<std::size_t>(got);
        }
        return true;
    }

    bool recvAll(void* data, std::size_t n) override {
        char* p = static_cast<char*>(data);
        std::size_t got = 0;
        while (got < n) {
            if (sock_ == kInvalidSock) {
                error_ = "the connection is closed";
                return false;
            }
#if defined(_WIN32)
            const int r =
                ::recv(static_cast<SOCKET>(sock_), p + got, static_cast<int>(n - got), 0);
#else
            const int r = static_cast<int>(::recv(static_cast<int>(sock_), p + got, n - got, 0));
#endif
            if (r == 0) {
                // AN ORDERLY CLOSE IS STILL A LOST REPLY. The daemon hangs up
                // when its own parser gives up on us, and a client that read
                // that as "no more data for now" would wait for a sample
                // buffer that can never arrive.
                error_ = "the Pluto closed the connection";
                return false;
            }
            if (r < 0) {
                const int err = lastSocketError();
                error_ = isTimeout(err) ? "the Pluto stopped answering (receive timed out)"
                                        : socketErrorText("receive", err);
                return false;
            }
            got += static_cast<std::size_t>(r);
        }
        return true;
    }

    void close() override {
        if (sock_ != kInvalidSock) {
            closeSock(sock_);
            sock_ = kInvalidSock;
        }
    }

    const char* lastError() const override { return error_.c_str(); }

private:
    std::intptr_t sock_ = kInvalidSock;
    std::chrono::milliseconds ioWait_;
    std::string error_;
};

bool identChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-' || c == ':' ||
           c == '.';
}

// XML entity expansion, for the five the generator can emit. The format
// string of a scan element arrives as "le:S12/16&gt;&gt;0", so this is not
// decoration: without it every capture channel's format fails to parse.
std::string unescape(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '&') {
            out += in[i];
            continue;
        }
        const std::size_t semi = in.find(';', i);
        if (semi == std::string::npos || semi - i > 6) {
            out += in[i];
            continue;
        }
        const std::string ent = in.substr(i + 1, semi - i - 1);
        if (ent == "lt") {
            out += '<';
        } else if (ent == "gt") {
            out += '>';
        } else if (ent == "amp") {
            out += '&';
        } else if (ent == "quot") {
            out += '"';
        } else if (ent == "apos") {
            out += '\'';
        } else {
            out += in.substr(i, semi - i + 1);
        }
        i = semi;
    }
    return out;
}

}  // namespace

std::unique_ptr<Transport> connectTcp(const std::string& host, std::uint16_t port,
                                      std::chrono::milliseconds connectWait,
                                      std::chrono::milliseconds ioWait, std::string& error) {
    error.clear();
    if (!ensureWinsock()) {
        error = "could not initialise the network stack";
        return nullptr;
    }

    char portText[16];
    std::snprintf(portText, sizeof(portText), "%u", static_cast<unsigned>(port));

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;  // a Pluto is IPv4, but pluto.local may not be
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* results = nullptr;
    const int rc = ::getaddrinfo(host.c_str(), portText, &hints, &results);
    if (rc != 0 || results == nullptr) {
        error = "could not find \"" + host +
                "\" on the network (is the Pluto plugged in, and is this the right address?)";
        return nullptr;
    }

    std::intptr_t sock = kInvalidSock;
    std::string lastFailure;
    for (addrinfo* a = results; a != nullptr && sock == kInvalidSock; a = a->ai_next) {
        const std::intptr_t s =
            static_cast<std::intptr_t>(::socket(a->ai_family, a->ai_socktype, a->ai_protocol));
        if (s == kInvalidSock) {
            lastFailure = socketErrorText("socket", lastSocketError());
            continue;
        }

        // A NON-BLOCKING CONNECT, then a select with our own bound. The
        // socket's SO_SNDTIMEO does not bound connect() on either platform,
        // so a board that is powered but not yet answering would otherwise
        // park the GUI thread for the operating system's own retry schedule -
        // tens of seconds on Windows.
        setNonBlocking(s, true);
        int ret =
#if defined(_WIN32)
            ::connect(static_cast<SOCKET>(s), a->ai_addr, static_cast<int>(a->ai_addrlen));
#else
            ::connect(static_cast<int>(s), a->ai_addr, static_cast<socklen_t>(a->ai_addrlen));
#endif
        if (ret != 0) {
            const int err = lastSocketError();
            if (!wouldBlock(err)) {
                lastFailure = socketErrorText("connect", err);
                closeSock(s);
                continue;
            }
            fd_set wfds;
            fd_set efds;
            FD_ZERO(&wfds);
            FD_ZERO(&efds);
#if defined(_WIN32)
            FD_SET(static_cast<SOCKET>(s), &wfds);
            FD_SET(static_cast<SOCKET>(s), &efds);
            const int nfds = 0;
#else
            FD_SET(static_cast<int>(s), &wfds);
            FD_SET(static_cast<int>(s), &efds);
            const int nfds = static_cast<int>(s) + 1;
#endif
            timeval tv{};
            tv.tv_sec = static_cast<long>(connectWait.count() / 1000);
            tv.tv_usec = static_cast<long>((connectWait.count() % 1000) * 1000);
            const int sel = ::select(nfds, nullptr, &wfds, &efds, &tv);
            if (sel <= 0) {
                lastFailure = "the Pluto did not answer in time (is it at this address?)";
                closeSock(s);
                continue;
            }
            int soErr = 0;
#if defined(_WIN32)
            int len = sizeof(soErr);
            ::getsockopt(static_cast<SOCKET>(s), SOL_SOCKET, SO_ERROR,
                         reinterpret_cast<char*>(&soErr), &len);
#else
            socklen_t len = sizeof(soErr);
            ::getsockopt(static_cast<int>(s), SOL_SOCKET, SO_ERROR, &soErr, &len);
#endif
            if (soErr != 0) {
                lastFailure = socketErrorText("connect", soErr);
                closeSock(s);
                continue;
            }
        }
        setNonBlocking(s, false);
        setIoTimeouts(s, ioWait);
        // Nagle off: every command here is one short line whose answer we
        // then wait for, which is precisely the traffic Nagle delays.
        int yes = 1;
#if defined(_WIN32)
        ::setsockopt(static_cast<SOCKET>(s), IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&yes), sizeof(yes));
#else
        ::setsockopt(static_cast<int>(s), IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
#endif
        sock = s;
    }
    ::freeaddrinfo(results);

    if (sock == kInvalidSock) {
        error = "could not reach the Pluto at " + host + ":" + portText +
                (lastFailure.empty() ? std::string() : (" - " + lastFailure));
        return nullptr;
    }
    return std::unique_ptr<Transport>(new TcpTransport(sock, ioWait));
}

// --- the context ----------------------------------------------------------

bool Channel::hasAttr(const std::string& a) const {
    return std::find(attrs.begin(), attrs.end(), a) != attrs.end();
}

bool Device::hasAttr(const std::string& a) const {
    return std::find(attrs.begin(), attrs.end(), a) != attrs.end();
}

const Channel* Device::findChannel(const std::string& idOrName, bool output) const {
    for (const Channel& c : channels) {
        if (c.output == output && c.id == idOrName) { return &c; }
    }
    for (const Channel& c : channels) {
        if (c.output == output && !c.name.empty() && c.name == idOrName) { return &c; }
    }
    return nullptr;
}

const Device* Context::findDevice(const std::string& name) const {
    for (const Device& d : devices) {
        if (d.name == name) { return &d; }
    }
    for (const Device& d : devices) {
        if (d.id == name) { return &d; }
    }
    return nullptr;
}

std::string Context::attr(const std::string& name) const {
    for (const auto& kv : attrs) {
        if (kv.first == name) { return kv.second; }
    }
    return std::string();
}

bool parseContext(const std::string& xml, Context& out, std::string& error) {
    out = Context{};
    error.clear();

    Device* device = nullptr;
    Channel* channel = nullptr;
    bool sawContext = false;

    std::size_t i = 0;
    while (i < xml.size()) {
        const std::size_t lt = xml.find('<', i);
        if (lt == std::string::npos) { break; }
        i = lt + 1;
        if (i >= xml.size()) { break; }

        // The prologue, the doctype and comments. The DOCTYPE the daemon
        // emits carries an internal subset in brackets whose element
        // declarations contain '>' characters of their own, so it cannot be
        // skipped by looking for the next '>' - that lands in the middle of
        // it and the rest of the scan reads element declarations as elements.
        if (xml[i] == '?') {
            const std::size_t end = xml.find("?>", i);
            if (end == std::string::npos) { break; }
            i = end + 2;
            continue;
        }
        if (xml.compare(i, 3, "!--") == 0) {
            const std::size_t end = xml.find("-->", i);
            if (end == std::string::npos) { break; }
            i = end + 3;
            continue;
        }
        if (xml[i] == '!') {
            const std::size_t gt = xml.find('>', i);
            const std::size_t br = xml.find('[', i);
            if (br != std::string::npos && (gt == std::string::npos || br < gt)) {
                const std::size_t end = xml.find("]>", br);
                if (end == std::string::npos) { break; }
                i = end + 2;
            } else {
                if (gt == std::string::npos) { break; }
                i = gt + 1;
            }
            continue;
        }

        const bool closing = xml[i] == '/';
        if (closing) { ++i; }
        std::size_t nameEnd = i;
        while (nameEnd < xml.size() && identChar(xml[nameEnd])) { ++nameEnd; }
        const std::string tag = xml.substr(i, nameEnd - i);
        i = nameEnd;

        if (closing) {
            const std::size_t gt = xml.find('>', i);
            if (gt == std::string::npos) { break; }
            i = gt + 1;
            if (tag == "channel") { channel = nullptr; }
            if (tag == "device") {
                channel = nullptr;
                device = nullptr;
            }
            continue;
        }

        // The attributes of this element, verbatim.
        std::vector<std::pair<std::string, std::string>> fields;
        bool selfClosing = false;
        while (i < xml.size()) {
            while (i < xml.size() && std::isspace(static_cast<unsigned char>(xml[i]))) { ++i; }
            if (i >= xml.size()) { break; }
            if (xml[i] == '/') {
                selfClosing = true;
                ++i;
                continue;
            }
            if (xml[i] == '>') {
                ++i;
                break;
            }
            std::size_t keyEnd = i;
            while (keyEnd < xml.size() && identChar(xml[keyEnd])) { ++keyEnd; }
            if (keyEnd == i) {
                ++i;  // something we do not understand: step over it
                continue;
            }
            const std::string key = xml.substr(i, keyEnd - i);
            i = keyEnd;
            while (i < xml.size() && std::isspace(static_cast<unsigned char>(xml[i]))) { ++i; }
            if (i >= xml.size() || xml[i] != '=') { continue; }
            ++i;
            while (i < xml.size() && std::isspace(static_cast<unsigned char>(xml[i]))) { ++i; }
            if (i >= xml.size() || (xml[i] != '"' && xml[i] != '\'')) { continue; }
            const char quote = xml[i++];
            const std::size_t valEnd = xml.find(quote, i);
            if (valEnd == std::string::npos) {
                error = "the context XML has an unterminated attribute value";
                return false;
            }
            fields.emplace_back(key, unescape(xml.substr(i, valEnd - i)));
            i = valEnd + 1;
        }

        const auto field = [&fields](const char* key) -> std::string {
            for (const auto& kv : fields) {
                if (kv.first == key) { return kv.second; }
            }
            return std::string();
        };

        if (tag == "context") {
            sawContext = true;
        } else if (tag == "context-attribute") {
            out.attrs.emplace_back(field("name"), field("value"));
        } else if (tag == "device") {
            Device d;
            d.id = field("id");
            d.name = field("name");
            out.devices.push_back(std::move(d));
            device = &out.devices.back();
            channel = nullptr;
            if (selfClosing) { device = nullptr; }
        } else if (tag == "channel" && device != nullptr) {
            Channel c;
            c.id = field("id");
            c.name = field("name");
            c.output = field("type") == "output";
            device->channels.push_back(std::move(c));
            channel = &device->channels.back();
            if (selfClosing) { channel = nullptr; }
        } else if (tag == "attribute") {
            const std::string name = field("name");
            if (channel != nullptr) {
                channel->attrs.push_back(name);
            } else if (device != nullptr) {
                device->attrs.push_back(name);
            }
        } else if (tag == "scan-element" && channel != nullptr) {
            channel->hasScanElement = true;
            channel->scanFormat = field("format");
            const std::string idx = field("index");
            channel->scanIndex = idx.empty() ? -1 : std::atoi(idx.c_str());
        }
        // Anything else - debug-attribute, buffer-attribute, an element a
        // newer daemon invents - is skipped rather than rejected: a radio
        // must not become unopenable because its firmware grew a feature.
    }

    if (!sawContext) {
        error = "what answered is not an IIO context (no <context> element)";
        return false;
    }
    if (out.devices.empty()) {
        error = "the IIO context has no devices in it";
        return false;
    }
    return true;
}

// --- the sample layout ----------------------------------------------------

bool parseSampleFormat(const std::string& text, SampleFormat& out) {
    out = SampleFormat{};
    std::size_t i = 0;
    if (text.compare(0, 3, "le:") == 0) {
        out.littleEndian = true;
        i = 3;
    } else if (text.compare(0, 3, "be:") == 0) {
        out.littleEndian = false;
        i = 3;
    }
    if (i >= text.size()) { return false; }
    if (text[i] == 'S' || text[i] == 's') {
        out.isSigned = true;
    } else if (text[i] == 'U' || text[i] == 'u') {
        out.isSigned = false;
    } else {
        return false;
    }
    ++i;
    const std::size_t bitsStart = i;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) { ++i; }
    if (i == bitsStart) { return false; }
    out.bits = std::atoi(text.substr(bitsStart, i - bitsStart).c_str());
    if (i >= text.size() || text[i] != '/') { return false; }
    ++i;
    const std::size_t storeStart = i;
    while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) { ++i; }
    if (i == storeStart) { return false; }
    out.storageBits = std::atoi(text.substr(storeStart, i - storeStart).c_str());
    out.shift = 0;
    if (i + 1 < text.size() && text[i] == '>' && text[i + 1] == '>') {
        i += 2;
        const std::size_t shiftStart = i;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) { ++i; }
        if (i == shiftStart) { return false; }
        out.shift = std::atoi(text.substr(shiftStart, i - shiftStart).c_str());
    }
    if (out.bits <= 0 || out.bits > 64 || out.storageBits < out.bits || out.storageBits > 64) {
        return false;
    }
    return true;
}

float convertSample(const std::uint8_t* word, const SampleFormat& fmt) {
    const std::size_t bytes = fmt.storageBytes();
    std::uint64_t raw = 0;
    if (fmt.littleEndian) {
        for (std::size_t b = 0; b < bytes; ++b) {
            raw |= static_cast<std::uint64_t>(word[b]) << (8 * b);
        }
    } else {
        for (std::size_t b = 0; b < bytes; ++b) {
            raw = (raw << 8) | static_cast<std::uint64_t>(word[b]);
        }
    }
    raw >>= fmt.shift;
    const int bits = fmt.bits;
    const std::uint64_t mask =
        (bits >= 64) ? ~0ULL : ((1ULL << bits) - 1ULL);
    raw &= mask;
    const double full = static_cast<double>(1ULL << (bits - 1));
    if (fmt.isSigned) {
        // SIGN-EXTEND FROM THE SIGNIFICANT WIDTH, not from the storage width.
        // A Pluto's le:S12/16 sample arrives already extended into all 16
        // bits by the hardware, so on a real board this changes nothing; on
        // a device that reports 12 significant bits and fills only 12, it is
        // the difference between -1 and +4095.
        const std::uint64_t sign = 1ULL << (bits - 1);
        std::int64_t value = static_cast<std::int64_t>(raw);
        if ((raw & sign) != 0) { value = static_cast<std::int64_t>(raw) - static_cast<std::int64_t>(mask) - 1; }
        return static_cast<float>(static_cast<double>(value) / full);
    }
    return static_cast<float>((static_cast<double>(raw) - full) / full);
}

void packSample(float value, const SampleFormat& fmt, std::uint8_t* word) {
    const std::size_t bytes = fmt.storageBytes();
    const int bits = fmt.bits;
    const std::uint64_t mask = (bits >= 64) ? ~0ULL : ((1ULL << bits) - 1ULL);
    const double full = static_cast<double>(1ULL << (bits - 1));

    // NaN first, and with a negated test so it cannot fall through: every
    // ordering comparison against NaN is false, so `if (v > hi)` and
    // `if (v < lo)` would BOTH decline to clamp it and a cast would then be
    // undefined. Silence is the only honest transmission of a non-number.
    double v = static_cast<double>(value);
    if (!(v == v)) { v = 0.0; }

    std::uint64_t raw = 0;
    if (fmt.isSigned) {
        // The asymmetry is the format's, not a rounding choice: a two's
        // complement field of `bits` holds -full through full-1, so +1.0
        // saturates one step short of where -1.0 does. Clamping to `full`
        // instead would write the most negative value.
        double scaled = v * full;
        if (scaled > full - 1.0) { scaled = full - 1.0; }
        if (scaled < -full) { scaled = -full; }
        const std::int64_t q = static_cast<std::int64_t>(std::llround(scaled));
        raw = static_cast<std::uint64_t>(q) & mask;
    } else {
        double scaled = v * full + full;
        const double top = static_cast<double>(mask);
        if (scaled > top) { scaled = top; }
        if (scaled < 0.0) { scaled = 0.0; }
        raw = static_cast<std::uint64_t>(std::llround(scaled)) & mask;
    }
    raw <<= fmt.shift;

    if (fmt.littleEndian) {
        for (std::size_t b = 0; b < bytes; ++b) {
            word[b] = static_cast<std::uint8_t>((raw >> (8 * b)) & 0xFFu);
        }
    } else {
        for (std::size_t b = 0; b < bytes; ++b) {
            word[b] = static_cast<std::uint8_t>((raw >> (8 * (bytes - 1 - b))) & 0xFFu);
        }
    }
}

// --- attribute value shapes ----------------------------------------------

bool parseRange(const std::string& text, Range& out) {
    out = Range{};
    const std::size_t open = text.find('[');
    const std::size_t close = text.find(']', open == std::string::npos ? 0 : open);
    if (open == std::string::npos || close == std::string::npos || close < open) { return false; }
    const std::vector<std::string> words = splitWords(text.substr(open + 1, close - open - 1));
    if (words.size() != 3) { return false; }
    char* end = nullptr;
    double v[3];
    for (std::size_t i = 0; i < 3; ++i) {
        v[i] = std::strtod(words[i].c_str(), &end);
        if (end == words[i].c_str() || *end != '\0') { return false; }
    }
    out.min = v[0];
    out.step = v[1];
    out.max = v[2];
    // A range whose ends are the wrong way round is a range we do not
    // understand, and reporting it would put a tuning limit on screen that
    // refuses everything.
    return out.max >= out.min;
}

std::vector<std::string> splitWords(const std::string& text) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) { ++i; }
        const std::size_t start = i;
        while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))) { ++i; }
        if (i > start) { out.push_back(text.substr(start, i - start)); }
    }
    return out;
}

std::string channelMask(const std::vector<std::size_t>& enabled, std::size_t channelCount) {
    const std::size_t words = (channelCount + 31) / 32;
    std::vector<std::uint32_t> bits(words == 0 ? 1 : words, 0);
    for (const std::size_t ch : enabled) {
        if (ch < channelCount) { bits[ch / 32] |= (1u << (ch % 32)); }
    }
    std::string out;
    char buf[16];
    for (std::size_t w = bits.size(); w > 0; --w) {
        std::snprintf(buf, sizeof(buf), "%08x", bits[w - 1]);
        out += buf;
    }
    return out;
}

// --- errno names ----------------------------------------------------------

std::string errnoName(long negativeStatus) {
    // The LINUX numbers, because the daemon is Linux and it is the daemon's
    // errno on the wire - not this machine's, which for several of these
    // disagrees.
    switch (-negativeStatus) {
        case 1: return "EPERM - not permitted";
        case 2: return "ENOENT - no such attribute";
        case 5: return "EIO - the board reported an I/O error";
        case 6: return "ENXIO - no such channel";
        case 9: return "EBADF - that device is not open";
        case 11: return "EAGAIN - the board was not ready";
        case 12: return "ENOMEM - the board ran out of memory";
        case 16: return "EBUSY - something else is using that device";
        case 19: return "ENODEV - no such device";
        case 22: return "EINVAL - the board rejected that value";
        case 25: return "ENOTTY - the board does not support that";
        case 28: return "ENOSPC - out of space";
        case 75: return "EOVERFLOW - the value did not fit";
        case 110: return "ETIMEDOUT - the board did not answer";
        default: return std::string();
    }
}

// --- the client -----------------------------------------------------------

Client::Client(std::unique_ptr<Transport> transport) : transport_(std::move(transport)) {}

Client::~Client() { close(); }

void Client::close() {
    if (transport_ != nullptr) {
        transport_->close();
        transport_.reset();
    }
}

void Client::fail(std::string message) { error_ = std::move(message); }

bool Client::sendLine(const std::string& line) {
    if (transport_ == nullptr) {
        fail("there is no connection to the Pluto");
        return false;
    }
    const std::string wire = line + "\n";
    if (!transport_->sendAll(wire.data(), wire.size())) {
        fail(transport_->lastError());
        return false;
    }
    return true;
}

bool Client::recvLine(std::string& out) {
    out.clear();
    if (transport_ == nullptr) {
        fail("there is no connection to the Pluto");
        return false;
    }
    // ONE BYTE AT A TIME, and deliberately. Every reply line is followed
    // immediately by binary sample data or by nothing, and a buffered read
    // that over-ran the newline would swallow the first bytes of a sample
    // buffer - a fault that would show as a picture shifted by a few samples
    // rather than as an error. The lines are at most a few dozen bytes.
    constexpr std::size_t kMaxLine = 512;
    char c = 0;
    while (out.size() < kMaxLine) {
        if (!transport_->recvAll(&c, 1)) {
            fail(transport_->lastError());
            return false;
        }
        if (c == '\n') { return true; }
        if (c != '\r') { out += c; }
    }
    fail("the Pluto answered a line with no end to it");
    return false;
}

bool Client::recvStatus(long& value, const char* what) {
    status_ = 0;
    std::string line;
    if (!recvLine(line)) { return false; }
    char* end = nullptr;
    const long n = std::strtol(line.c_str(), &end, 10);
    if (end == line.c_str() || (end != nullptr && *end != '\0')) {
        fail(std::string("the Pluto answered something unexpected while ") + what + ": \"" +
             line + "\"");
        return false;
    }
    if (n < 0) {
        status_ = n;
        const std::string name = errnoName(n);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%ld", n);
        fail(std::string("the Pluto refused to ") + what + " (" +
             (name.empty() ? std::string(buf) : name) + ")");
        return false;
    }
    value = n;
    return true;
}

bool Client::version(std::string& out) {
    out.clear();
    if (!sendLine("VERSION")) { return false; }
    // No length prefix on this one: parser.y's VERSION rule writes a single
    // formatted line and nothing else.
    if (!recvLine(out)) { return false; }
    // IT HAS TO LOOK LIKE A VERSION, and this is the only sanity check the
    // protocol offers. parser.y's VERSION rule writes "%u.%u.%-7.7s", so a
    // digit and at least one dot is what an IIO daemon always answers - and
    // something else on port 30431 (an ssh banner, a web server, or an iiod
    // that did not understand the line and answered "-22" through yyerror)
    // would otherwise be accepted here and only discovered halfway through
    // parsing its next reply as XML. "That is not an IIO daemon" is a far
    // better message than "the context is malformed".
    const bool looksLikeVersion =
        !out.empty() && std::isdigit(static_cast<unsigned char>(out[0])) != 0 &&
        out.find('.') != std::string::npos;
    if (!looksLikeVersion) {
        status_ = 0;
        fail("what answered did not identify itself as an IIO daemon (it said \"" + out + "\")");
        out.clear();
        return false;
    }
    return true;
}

bool Client::print(std::string& xml) {
    xml.clear();
    if (!sendLine("PRINT")) { return false; }
    long len = 0;
    if (!recvStatus(len, "describe itself")) { return false; }
    if (len <= 0 || len > 4 * 1024 * 1024) {
        fail("the Pluto described itself in a way this driver cannot use");
        return false;
    }
    xml.resize(static_cast<std::size_t>(len));
    if (!transport_->recvAll(&xml[0], xml.size())) {
        fail(transport_->lastError());
        return false;
    }
    // The trailing newline the length does not count (parser.y's PRINT rule
    // writes it after the document). Consumed here, or it becomes the first
    // byte of the next reply.
    char nl = 0;
    if (!transport_->recvAll(&nl, 1)) {
        fail(transport_->lastError());
        return false;
    }
    return true;
}

bool Client::setTimeoutMs(long ms) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "TIMEOUT %ld", ms);
    if (!sendLine(buf)) { return false; }
    long ignored = 0;
    return recvStatus(ignored, "set its own timeout");
}

bool Client::readAttrReply(std::string& value, const char* what) {
    value.clear();
    long len = 0;
    if (!recvStatus(len, what)) { return false; }
    if (len > 64 * 1024) {
        fail("the Pluto answered an implausibly long attribute value");
        return false;
    }
    std::string buf(static_cast<std::size_t>(len), '\0');
    if (len > 0 && !transport_->recvAll(&buf[0], buf.size())) {
        fail(transport_->lastError());
        return false;
    }
    char nl = 0;
    if (!transport_->recvAll(&nl, 1)) {
        fail(transport_->lastError());
        return false;
    }
    // sysfs hands back a trailing newline, and the daemon passes on whatever
    // it was given plus - depending on the attribute - a NUL. Both are noise
    // to every caller, and a value that compares unequal to itself because of
    // an invisible byte is the kind of defect that costs an afternoon.
    while (!buf.empty() &&
           (buf.back() == '\0' || buf.back() == '\n' || buf.back() == '\r' || buf.back() == ' ')) {
        buf.pop_back();
    }
    value = buf;
    return true;
}

bool Client::readDeviceAttr(const std::string& device, const std::string& attr,
                            std::string& value) {
    if (!sendLine("READ " + device + " " + attr)) { return false; }
    return readAttrReply(value, ("read " + attr).c_str());
}

bool Client::readChannelAttr(const std::string& device, bool output, const std::string& channel,
                             const std::string& attr, std::string& value) {
    if (!sendLine("READ " + device + (output ? " OUTPUT " : " INPUT ") + channel + " " + attr)) {
        return false;
    }
    return readAttrReply(value, ("read " + attr).c_str());
}

bool Client::writeAttrPayload(const std::string& value, const char* what) {
    // The count stated on the command line INCLUDES the terminating NUL, and
    // the bytes sent here match it exactly: ops.c read_all()s precisely that
    // many and hands them to the attribute, so a count that disagreed with
    // what follows would leave the daemon reading the next command as data.
    const std::string payload = value + '\0';
    if (!transport_->sendAll(payload.data(), payload.size())) {
        fail(transport_->lastError());
        return false;
    }
    long written = 0;
    return recvStatus(written, what);
}

bool Client::writeDeviceAttr(const std::string& device, const std::string& attr,
                             const std::string& value) {
    char count[32];
    std::snprintf(count, sizeof(count), "%zu", value.size() + 1);
    if (!sendLine("WRITE " + device + " " + attr + " " + count)) { return false; }
    return writeAttrPayload(value, ("set " + attr).c_str());
}

bool Client::writeChannelAttr(const std::string& device, bool output, const std::string& channel,
                              const std::string& attr, const std::string& value) {
    char count[32];
    std::snprintf(count, sizeof(count), "%zu", value.size() + 1);
    if (!sendLine("WRITE " + device + (output ? " OUTPUT " : " INPUT ") + channel + " " + attr +
                  " " + count)) {
        return false;
    }
    return writeAttrPayload(value, ("set " + attr).c_str());
}

bool Client::setBuffersCount(const std::string& device, long count) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "SET %s BUFFERS_COUNT %ld", device.c_str(), count);
    if (!sendLine(buf)) { return false; }
    long ignored = 0;
    return recvStatus(ignored, "set the number of buffers");
}

bool Client::openBuffer(const std::string& device, std::size_t samplesCount,
                        const std::string& mask) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "OPEN %s %zu %s", device.c_str(), samplesCount, mask.c_str());
    if (!sendLine(buf)) { return false; }
    long ignored = 0;
    return recvStatus(ignored, "open the capture device");
}

bool Client::closeBuffer(const std::string& device) {
    if (!sendLine("CLOSE " + device)) { return false; }
    long ignored = 0;
    return recvStatus(ignored, "close the capture device");
}

bool Client::readBuf(const std::string& device, std::size_t bytes, std::vector<std::uint8_t>& out,
                     std::string* mask) {
    out.clear();
    char buf[96];
    std::snprintf(buf, sizeof(buf), "READBUF %s %zu", device.c_str(), bytes);
    if (!sendLine(buf)) { return false; }

    long len = 0;
    if (!recvStatus(len, "hand over the samples")) { return false; }
    if (len == 0) {
        fail("the Pluto answered that it had no samples to give");
        return false;
    }
    if (static_cast<std::size_t>(len) > bytes) {
        // More than was asked for cannot happen through send_data (it clamps
        // to the requested count), so it is not a long buffer - it is a reply
        // this client is no longer in step with, and reading it would make
        // things worse rather than better.
        fail("the Pluto offered more samples than were asked for");
        return false;
    }

    // The mask line, on EVERY readbuf (see the header). Read before the data
    // and not after: it is the daemon's own framing, and skipping it here
    // would put its nine characters at the front of the sample buffer.
    std::string maskLine;
    if (!recvLine(maskLine)) { return false; }
    if (mask != nullptr) { *mask = maskLine; }

    out.resize(static_cast<std::size_t>(len));
    if (!transport_->recvAll(out.data(), out.size())) {
        fail(transport_->lastError());
        out.clear();
        return false;
    }
    if (static_cast<std::size_t>(len) != bytes) {
        // Read first, THEN fail: the bytes had to come off the socket either
        // way, and leaving them there would desynchronise a connection the
        // caller may still be closing politely.
        out.clear();
        fail("the Pluto handed over a short sample buffer");
        return false;
    }
    return true;
}

bool Client::writeBuf(const std::string& device, const std::uint8_t* data, std::size_t bytes) {
    if (transport_ == nullptr) {
        fail("there is no connection to the Pluto");
        return false;
    }
    if (bytes == 0) {
        // Nothing to say and nothing to send. Refused rather than sent,
        // because a WRITEBUF of zero would still cost the ack round trip and
        // the daemon's own answer to it is not worth discovering on the air.
        fail("nothing was offered to the Pluto to transmit");
        return false;
    }
    char buf[96];
    std::snprintf(buf, sizeof(buf), "WRITEBUF %s %zu", device.c_str(), bytes);
    if (!sendLine(buf)) { return false; }

    // THE ACK, BEFORE THE BYTES. Sending the payload without waiting for it
    // would work right up until the daemon refused the command - at which
    // point the refusal line and our sample bytes would cross, and the next
    // reply this client read would be the middle of its own modulation.
    long ack = 0;
    if (!recvStatus(ack, "take a transmit buffer")) { return false; }

    if (!transport_->sendAll(data, bytes)) {
        fail(transport_->lastError());
        return false;
    }

    long took = 0;
    if (!recvStatus(took, "finish taking a transmit buffer")) { return false; }
    if (static_cast<std::size_t>(took) != bytes) {
        char msg[128];
        std::snprintf(msg, sizeof(msg),
                      "the Pluto took %ld of %zu bytes it was offered to transmit", took, bytes);
        fail(msg);
        return false;
    }
    return true;
}

}  // namespace cascade::source::iiod
