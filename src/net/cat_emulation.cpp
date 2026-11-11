// cat_emulation.cpp - see cat_emulation.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/cat_emulation.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/serial_port.hpp"

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
#endif

namespace cascade::net {

const char* catDialectWord(CatDialect d) { return d == CatDialect::KenwoodTS480 ? "kenwood-ts480" : "yaesu-ft891"; }

namespace {

bool allDigits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}
double toNumber(const std::string& s) {
    double v = 0;
    for (char c : s) { v = v * 10.0 + (c - '0'); }
    return v;
}
std::string digits(double v, int width, double maxValue) {
    if (!std::isfinite(v) || v < 0) { v = 0; }
    if (v > maxValue) { v = maxValue; }
    char buf[40];
    std::snprintf(buf, sizeof buf, "%0*.0f", width, std::floor(v + 0.5));
    return buf;
}

}  // namespace

CatEmulator::CatEmulator(CatDialect dialect, RigBackend backend, std::shared_ptr<RigVfos> vfos)
    : dialect_(dialect), rig_(std::move(backend)), vfos_(vfos ? std::move(vfos) : std::make_shared<RigVfos>()) {}

double CatEmulator::activeFrequency() {
    if (rig_.frequencyHz) { return rig_.frequencyHz(); }
    std::lock_guard<std::mutex> lock(vfos_->m);
    return vfos_->freq[vfos_->rx];
}
bool CatEmulator::tuneActive(double hz) {
    if (!rig_.setFrequencyHz || !rig_.setFrequencyHz(hz)) { return false; }
    std::lock_guard<std::mutex> lock(vfos_->m);
    vfos_->freq[vfos_->rx] = hz;
    return true;
}
char CatEmulator::kenwoodModeDigit() {
    const std::string m = rig_.mode ? rig_.mode() : std::string();
    if (m == "LSB") { return '1'; }
    if (m == "USB" || m == "RAW") { return '2'; }
    if (m == "CW") { return '3'; }
    if (m == "NFM" || m == "WFM") { return '4'; }
    if (m == "AM" || m == "DSB") { return '5'; }
    return '2';
}
char CatEmulator::yaesuModeChar() {
    const std::string m = rig_.mode ? rig_.mode() : std::string();
    if (m == "LSB") { return '1'; }
    if (m == "USB" || m == "RAW") { return '2'; }
    if (m == "CW") { return '3'; }
    if (m == "NFM" || m == "WFM") { return '4'; }
    if (m == "AM" || m == "DSB") { return '5'; }
    return '2';
}

std::string CatEmulator::feed(const char* data, std::size_t n) {
    std::string out;
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(data[i]);
        if (dropping_) {
            if (c == ';') { dropping_ = false; }
            continue;
        }
        if (c == ';') {
            std::string cmd;
            cmd.swap(partial_);
            out += handle(cmd);
            continue;
        }
        if (c < 0x20 || c == 0x7F) { continue; }   // control characters (the CR LF some programs add) are not part of a command
        if (partial_.size() >= kMaxCommandBytes) {
            partial_.clear();
            dropping_ = true;
            ++stats_.commands;
            ++stats_.notUnderstood;
            ++stats_.answered;
            out += "?;";
            continue;
        }
        partial_ += static_cast<char>(c);
    }
    return out;
}

std::string CatEmulator::handle(const std::string& raw) {
    std::string cmd;
    for (char c : raw) {
        if (c == ' ' && cmd.empty()) { continue; }
        cmd += static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
    }
    if (cmd.empty()) { return std::string(); }   // dummy data (a radio's wake-up "; ; ;"): not a command
    ++stats_.commands;
    const std::string reply = dialect_ == CatDialect::KenwoodTS480 ? kenwood(cmd) : yaesu(cmd);
    if (!reply.empty()) { ++stats_.answered; }
    if (reply == "?;") { ++stats_.notUnderstood; }
    return reply;
}

// ---- Kenwood TS-480 ----------------------------------------------------------------------------------------------------------------------
std::string CatEmulator::kenwood(const std::string& cmd) {
    const std::string name = cmd.substr(0, 2);
    const std::string p = cmd.size() > 2 ? cmd.substr(2) : std::string();
    const std::string bad = "?;";
    const std::string notDone = "E;";
    if (cmd.size() < 2) { return bad; }

    if (name == "ID") { return p.empty() ? "ID020;" : bad; }

    if (name == "FA" || name == "FB") {
        const int idx = name == "FA" ? 0 : 1;
        if (p.empty()) {
            double hz;
            int rx;
            { std::lock_guard<std::mutex> lock(vfos_->m); rx = vfos_->rx; hz = vfos_->freq[idx]; }
            if (idx == rx || hz <= 0.0) { hz = activeFrequency(); }   // the receive VFO is the receiver itself; a VFO never set reads as the receiver
            return name + digits(hz, 11, 99999999999.0) + ";";
        }
        if (p.size() != 11 || !allDigits(p)) { return bad; }
        const double hz = toNumber(p);
        int rx;
        { std::lock_guard<std::mutex> lock(vfos_->m); rx = vfos_->rx; }
        if (idx == rx) {
            if (!tuneActive(hz)) { ++stats_.notExecuted; return notDone; }
        } else {
            std::lock_guard<std::mutex> lock(vfos_->m);
            vfos_->freq[idx] = hz;
        }
        ++stats_.setsApplied;
        return std::string();
    }

    if (name == "FR" || name == "FT") {
        int rx;
        { std::lock_guard<std::mutex> lock(vfos_->m); rx = vfos_->rx; }
        if (p.empty()) { return name + std::to_string(rx) + ";"; }
        if (p.size() != 1 || !allDigits(p)) { return bad; }
        if (name == "FT") { ++stats_.transmitRefused; return std::string(); }   // the transmit VFO: accepted and ignored, there is no transmitter
        const int want = p[0] - '0';
        if (want > 1) { ++stats_.notExecuted; return notDone; }   // memory channels: none
        if (want != rx) {
            const double current = activeFrequency();
            double target;
            { std::lock_guard<std::mutex> lock(vfos_->m); vfos_->freq[rx] = current; target = vfos_->freq[want]; }
            if (target <= 0.0) { target = current; }
            if (!rig_.setFrequencyHz || !rig_.setFrequencyHz(target)) { ++stats_.notExecuted; return notDone; }
            std::lock_guard<std::mutex> lock(vfos_->m);
            vfos_->rx = want;
            vfos_->freq[want] = target;
        }
        ++stats_.setsApplied;
        return std::string();
    }

    if (name == "MD") {
        if (p.empty()) { return std::string("MD") + kenwoodModeDigit() + ";"; }
        if (p.size() != 1 || !allDigits(p)) { return bad; }
        const char* names = nullptr;
        switch (p[0]) {
            case '1': names = "LSB"; break;
            case '2': names = "USB"; break;
            case '3': names = "CW"; break;
            case '4': names = "NFM"; break;
            case '5': names = "AM"; break;
            default: break;   // 0 none, 6 FSK, 7 CWR, 8 tune, 9 FSR: the receiver has no such mode
        }
        if (names == nullptr || !rig_.setMode || !rig_.setMode(names)) { ++stats_.notExecuted; return notDone; }
        ++stats_.setsApplied;
        return std::string();
    }

    if (name == "SM") {
        if (p != "0") { return bad; }
        const double f = rig_.signalFraction ? rig_.signalFraction() : 0.0;
        return "SM0" + digits(std::clamp(std::isfinite(f) ? f : 0.0, 0.0, 1.0) * 20.0, 4, 20.0) + ";";
    }

    if (name == "IF") {
        if (!p.empty()) { return bad; }
        int rx;
        { std::lock_guard<std::mutex> lock(vfos_->m); rx = vfos_->rx; }
        // IF + P1 frequency (11) + P2 five spaces + P3 RIT/XIT offset (5) + P4 RIT + P5 XIT + P6 + P7 memory channel (2) + P8 RX/TX + P9 mode +
        // P10 VFO + P11 scan + P12 simplex/split + P13 tone + P14 tone number (2) + P15 a space + ";"
        return "IF" + digits(activeFrequency(), 11, 99999999999.0) + "     " + "+0000" + "0" + "0" + "0" + "00" + "0" + kenwoodModeDigit() +
               static_cast<char>('0' + rx) + "0" + "0" + "0" + "00" + " " + ";";
    }

    if (name == "PS") {
        if (p.empty()) { return (rig_.running && rig_.running()) ? "PS1;" : "PS0;"; }
        if (p == "1") { return std::string(); }
        if (p == "0" || p == "9") { ++stats_.notExecuted; return notDone; }   // power off: the receiver is not stopped from outside
        return bad;
    }

    if (name == "AI") {
        if (p.empty()) { return "AI0;"; }
        if (p.size() == 1 && p[0] >= '0' && p[0] <= '3') { return std::string(); }   // accepted; auto-information is never sent
        return bad;
    }

    if (name == "RX") { return (p.empty() || p == "0") ? std::string() : bad; }   // already receiving

    if (name == "TX" || name == "KY" || name == "AC") {   // transmit, CW keying, antenna-tuner tune: all transmit
        ++stats_.transmitRefused;
        return notDone;
    }
    if (name == "SR") { ++stats_.notExecuted; return notDone; }

    return bad;
}

// ---- Yaesu FT-891 ------------------------------------------------------------------------------------------------------------------------
std::string CatEmulator::yaesu(const std::string& cmd) {
    const std::string name = cmd.substr(0, 2);
    const std::string p = cmd.size() > 2 ? cmd.substr(2) : std::string();
    const std::string bad = "?;";
    if (cmd.size() < 2) { return bad; }

    if (name == "ID") { return p.empty() ? "ID0650;" : bad; }

    if (name == "FA" || name == "FB") {
        const bool a = name == "FA";
        if (p.empty()) {
            double hz;
            { std::lock_guard<std::mutex> lock(vfos_->m); hz = vfos_->freq[1]; }
            if (a || hz <= 0.0) { hz = activeFrequency(); }
            return name + digits(hz, 9, 999999999.0) + ";";
        }
        if (p.size() != 9 || !allDigits(p)) { return bad; }
        const double hz = toNumber(p);
        if (hz < 30000.0) { return bad; }
        if (a) {
            if (!tuneActive(hz)) { return bad; }
        } else {
            std::lock_guard<std::mutex> lock(vfos_->m);
            vfos_->freq[1] = hz;
        }
        ++stats_.setsApplied;
        return std::string();
    }

    if (name == "SV" || name == "AB" || name == "BA") {
        if (!p.empty()) { return bad; }
        const double current = activeFrequency();
        double other;
        { std::lock_guard<std::mutex> lock(vfos_->m); other = vfos_->freq[1]; }
        if (other <= 0.0) { other = current; }
        if (name == "AB") {
            std::lock_guard<std::mutex> lock(vfos_->m);
            vfos_->freq[1] = current;
        } else {   // SV swaps, BA copies B to A: either way the receiver goes to B's frequency
            if (!tuneActive(other)) { return bad; }
            if (name == "SV") {
                std::lock_guard<std::mutex> lock(vfos_->m);
                vfos_->freq[1] = current;
            }
        }
        ++stats_.setsApplied;
        return std::string();
    }

    if (name == "MD") {
        if (p == "0") { return std::string("MD0") + yaesuModeChar() + ";"; }
        if (p.size() != 2 || p[0] != '0') { return bad; }
        const char* m = nullptr;
        switch (p[1]) {
            case '1': case '8': m = "LSB"; break;
            case '2': case 'C': m = "USB"; break;
            case '3': m = "CW"; break;
            case '4': case 'B': m = "NFM"; break;
            case '5': case 'D': m = "AM"; break;
            default: break;   // RTTY, reversed CW, C4FM and the rest: no such mode here
        }
        if (m == nullptr || !rig_.setMode || !rig_.setMode(m)) { return bad; }
        ++stats_.setsApplied;
        return std::string();
    }

    if (name == "SM") {
        if (p != "0") { return bad; }
        const double f = rig_.signalFraction ? rig_.signalFraction() : 0.0;
        return "SM0" + digits(std::clamp(std::isfinite(f) ? f : 0.0, 0.0, 1.0) * 255.0, 3, 255.0) + ";";
    }

    if (name == "IF") {
        if (!p.empty()) { return bad; }
        // IF + P1 memory channel (3) + P2 frequency (9) + P3 clarifier sign and offset (5) + P4 clarifier + P5 + P6 mode + P7 VFO + P8 CTCSS +
        // P9 (2) + P10 shift + ";"
        return "IF001" + digits(activeFrequency(), 9, 999999999.0) + "+0000" + "0" + "0" + yaesuModeChar() + "0" + "0" + "00" + "0" + ";";
    }

    if (name == "PS") {
        if (p.empty()) { return (rig_.running && rig_.running()) ? "PS1;" : "PS0;"; }
        if (p == "1") { return std::string(); }
        if (p == "0") { ++stats_.notExecuted; return bad; }
        return bad;
    }

    if (name == "AI") {
        if (p.empty()) { return "AI0;"; }
        if (p.size() == 1 && (p[0] == '0' || p[0] == '1')) { return std::string(); }
        return bad;
    }

    if (name == "TX") {
        if (p.empty()) { return "TX0;"; }
        if (p == "0") { return std::string(); }   // "transmit off": nothing to do
        ++stats_.transmitRefused;
        return bad;
    }
    if (name == "ST") {   // split: a transmit arrangement
        if (p.empty()) { return "ST0;"; }
        if (p == "0") { return std::string(); }
        ++stats_.transmitRefused;
        return bad;
    }
    if (name == "KY" || name == "MX") {   // CW keying, MOX
        ++stats_.transmitRefused;
        return bad;
    }

    return bad;
}

// ---- transports ----------------------------------------------------------------------------------------------------------------------------
std::uint64_t serveCatStream(CatStream& stream, CatEmulator& emu, const std::atomic<bool>& stop) {
    char buf[512];
    while (!stop.load()) {
        const int n = stream.read(buf, sizeof buf);
        if (n < 0) { break; }
        if (n == 0) { continue; }
        const std::string reply = emu.feed(buf, static_cast<std::size_t>(n));
        std::size_t at = 0;
        while (at < reply.size()) {
            const int w = stream.write(reply.data() + at, reply.size() - at);
            if (w <= 0) { return emu.stats().commands; }
            at += static_cast<std::size_t>(w);
        }
    }
    return emu.stats().commands;
}

namespace {
class SerialStream final : public CatStream {
public:
    explicit SerialStream(core::SerialPort& p) : port_(p) {}
    int read(char* buf, std::size_t cap) override { return port_.read(buf, cap); }
    int write(const char* d, std::size_t n) override { return port_.write(d, n); }

private:
    core::SerialPort& port_;
};
}  // namespace

CatSerialService::CatSerialService(CatDialect dialect, RigBackend backend) : dialect_(dialect), backend_(std::move(backend)), vfos_(std::make_shared<RigVfos>()) {}
CatSerialService::~CatSerialService() { stop(); }

bool CatSerialService::start(const std::string& portName, int baud, CatEmuFact& fact) {
    if (running_.load()) { fact = CatEmuFact{"cat-already-running", "the serial CAT service is already running"}; return false; }
    if (portName.empty()) { fact = CatEmuFact{"cat-port-refused", "no serial port was named"}; return false; }
    if (!core::serialBaudSupported(baud)) { fact = CatEmuFact{"cat-port-refused", "that baud rate is not one this program offers"}; return false; }
    auto port = std::make_unique<core::SerialPort>();
    std::string err;
    if (!port->open(portName, baud, err, true)) {
        fact = CatEmuFact{"cat-port-refused", "the serial port " + core::loggableSerialPortName(portName) + " could not be opened"};
        return false;
    }
    port_ = std::move(port);
    stop_ = false;
    running_ = true;
    thread_ = std::thread([this] {
        CatEmulator emu(dialect_, backend_, vfos_);
        SerialStream stream(*port_);
        serveCatStream(stream, emu, stop_);
        std::lock_guard<std::mutex> lock(statsMu_);
        last_ = emu.stats();
        running_ = false;
    });
    return true;
}

void CatSerialService::stop() {
    stop_ = true;
    if (thread_.joinable()) { thread_.join(); }
    if (port_) { port_->close(); port_.reset(); }
    running_ = false;
}

CatEmuStats CatSerialService::stats() const {
    CatSerialService* self = const_cast<CatSerialService*>(this);
    std::lock_guard<std::mutex> lock(self->statsMu_);
    return last_;
}

#if defined(_WIN32)

namespace {
bool wsaOnce() {
    static const bool ok = [] {
        WSADATA d;
        return ::WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    return ok;
}

class SocketStream final : public CatStream {
public:
    explicit SocketStream(SOCKET s) : s_(s) {}
    int read(char* buf, std::size_t cap) override {
        const int n = ::recv(s_, buf, static_cast<int>(cap), 0);
        if (n > 0) { return n; }
        if (n == 0) { return -1; }   // the peer closed
        const int e = ::WSAGetLastError();
        return (e == WSAETIMEDOUT || e == WSAEWOULDBLOCK) ? 0 : -1;
    }
    int write(const char* d, std::size_t n) override {
        const int w = ::send(s_, d, static_cast<int>(n), 0);
        return w > 0 ? w : -1;
    }

private:
    SOCKET s_;
};
}  // namespace

struct CatTcpService::ClientRec {
    std::thread t;
    std::atomic<bool> done{false};
    SOCKET s = INVALID_SOCKET;
};

CatTcpService::CatTcpService(CatDialect dialect, RigBackend backend) : dialect_(dialect), backend_(std::move(backend)), vfos_(std::make_shared<RigVfos>()) {}
CatTcpService::~CatTcpService() { stop(); }

bool CatTcpService::start(std::uint16_t port, bool bindAll, CatEmuFact& fact) {
    if (running_.load()) { fact = CatEmuFact{"cat-already-running", "the TCP CAT service is already running"}; return false; }
    if (!wsaOnce()) { fact = CatEmuFact{"cat-tcp-refused", "the network layer could not be started"}; return false; }
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { fact = CatEmuFact{"cat-tcp-refused", "a socket could not be created"}; return false; }
    // exclusive: a second listener on the same port must not share it (and a stale one must not steal connections)
    BOOL excl = TRUE;
    ::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof excl);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(bindAll ? INADDR_ANY : INADDR_LOOPBACK);
    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(s, 4) != 0) {
        ::closesocket(s);
        fact = CatEmuFact{"cat-tcp-refused", "the TCP port could not be opened (is another program using it?)"};
        return false;
    }
    int len = sizeof a;
    ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
    port_ = ntohs(a.sin_port);
    listener_ = static_cast<std::intptr_t>(s);
    stop_ = false;
    running_ = true;
    acceptor_ = std::thread([this] { acceptLoop(); });
    return true;
}

void CatTcpService::acceptLoop() {
    const SOCKET ls = static_cast<SOCKET>(listener_);
    while (!stop_.load()) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(ls, &rd);
        timeval tv{0, 100000};
        if (::select(0, &rd, nullptr, nullptr, &tv) <= 0) {
            std::lock_guard<std::mutex> lock(mu_);   // prune finished clients
            for (auto it = recs_.begin(); it != recs_.end();) {
                if ((*it)->done.load()) { if ((*it)->t.joinable()) { (*it)->t.join(); } it = recs_.erase(it); } else { ++it; }
            }
            continue;
        }
        SOCKET c = ::accept(ls, nullptr, nullptr);
        if (c == INVALID_SOCKET) { continue; }
        if (clients_.load() >= kMaxClients) { ::closesocket(c); continue; }
        DWORD to = 100;
        ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof to);
        BOOL nodelay = TRUE;
        ::setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof nodelay);
        auto rec = std::make_unique<ClientRec>();
        rec->s = c;
        ClientRec* r = rec.get();
        ++clients_;
        {
            std::lock_guard<std::mutex> lock(mu_);
            recs_.push_back(std::move(rec));
        }
        r->t = std::thread([this, r, c] {
            CatEmulator emu(dialect_, backend_, vfos_);
            SocketStream stream(c);
            const std::uint64_t n = serveCatStream(stream, emu, stop_);
            commands_ += n;
            ::closesocket(c);
            --clients_;
            r->done = true;
        });
    }
}

void CatTcpService::stop() {
    stop_ = true;
    if (acceptor_.joinable()) { acceptor_.join(); }
    std::vector<std::unique_ptr<ClientRec>> recs;
    {
        std::lock_guard<std::mutex> lock(mu_);
        recs.swap(recs_);
    }
    for (auto& r : recs) {
        if (r->t.joinable()) { r->t.join(); }   // each client thread sees stop_ within its 100 ms receive timeout
    }
    if (listener_ != -1) { ::closesocket(static_cast<SOCKET>(listener_)); listener_ = -1; }
    port_ = 0;
    running_ = false;
}

#else  // not Windows: the TCP service is not built in this lane

struct CatTcpService::ClientRec {};
CatTcpService::CatTcpService(CatDialect d, RigBackend b) : dialect_(d), backend_(std::move(b)), vfos_(std::make_shared<RigVfos>()) {}
CatTcpService::~CatTcpService() = default;
bool CatTcpService::start(std::uint16_t, bool, CatEmuFact& fact) {
    fact = CatEmuFact{"cat-tcp-refused", "the TCP CAT service is built for Windows only so far"};
    return false;
}
void CatTcpService::acceptLoop() {}
void CatTcpService::stop() {}

#endif

}  // namespace cascade::net
