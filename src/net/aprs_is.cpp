// aprs_is.cpp - see aprs_is.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/aprs_is.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <vector>

namespace cascade::net {
namespace {

constexpr std::size_t kMaxFilterChars = 200;
constexpr std::size_t kMaxGatedLine = kAprsMaxLineBytes - 2;  // leaves room for CR LF
constexpr int kReadSliceMs = 50;                              // the worker's wake interval: bounds stop() and idle checks
constexpr std::size_t kMaxIncomingLine = 4096;                // a server line longer than this is hostile (the page says 512)

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool isAlnumUpper(char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }
bool isHex(char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'); }

bool hasControl(const std::string& s) {
    for (char c : s) {
        if (c == '\r' || c == '\n' || c == '\0') { return true; }
    }
    return false;
}

std::string upper(std::string s) {
    for (char& c : s) {
        if (c >= 'a' && c <= 'z') { c = static_cast<char>(c - 'a' + 'A'); }
    }
    return s;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

// "SRC>DEST,P1,P2" -> source and the path elements (destination first). False when the header is not that shape.
bool parseHeader(const std::string& header, std::string& src, std::vector<std::string>& rest) {
    const std::size_t gt = header.find('>');
    if (gt == std::string::npos || gt == 0) { return false; }
    src = header.substr(0, gt);
    rest = split(header.substr(gt + 1), ',');
    if (rest.empty() || rest.size() > 9) { return false; }  // destination + at most 8 path elements
    auto okElem = [](const std::string& e, bool allowStar) {
        if (e.empty() || e.size() > 9) { return false; }
        for (char c : e) {
            const bool good = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || (allowStar && c == '*');
            if (!good) { return false; }
        }
        return true;
    };
    if (!okElem(src, false)) { return false; }
    if (!okElem(rest[0], false)) { return false; }
    for (std::size_t i = 1; i < rest.size(); ++i) {
        if (!okElem(rest[i], true)) { return false; }
    }
    return true;
}

std::string bare(const std::string& e) {  // an element without the trailing '*' (the "has been used" mark), upper-cased
    std::string t = e;
    if (!t.empty() && t.back() == '*') { t.pop_back(); }
    return upper(t);
}

bool isForbiddenElement(const std::string& e, bool inclThirdPartyOnly) {
    const std::string b = bare(e);
    if (b == "TCPIP" || b == "TCPXX") { return true; }
    if (!inclThirdPartyOnly && (b == "NOGATE" || b == "RFONLY")) { return true; }
    return false;
}

bool isQOrI(const std::string& e) {
    const std::string b = bare(e);
    if (b == "I") { return true; }
    return b.size() == 3 && b[0] == 'Q' && b[1] == 'A';
}

}  // namespace

bool aprsNormaliseCallsign(std::string& call) {
    std::string c = upper(call);
    if (c.size() < 3 || c.size() > 9) { return false; }
    const std::size_t dash = c.find('-');
    std::string base = c, ssid;
    if (dash != std::string::npos) {
        if (c.find('-', dash + 1) != std::string::npos) { return false; }  // at most one hyphen
        base = c.substr(0, dash);
        ssid = c.substr(dash + 1);
        if (ssid.empty() || ssid.size() > 2 || ssid == "0" || ssid == "00") { return false; }  // never -0
    }
    if (base.size() < 3) { return false; }
    for (char ch : base) {
        if (!isAlnumUpper(ch)) { return false; }
    }
    for (char ch : ssid) {
        if (!isAlnumUpper(ch)) { return false; }
    }
    if (c.size() == 8 && std::all_of(c.begin(), c.end(), isHex)) { return false; }  // would look like a server's IP in a q construct
    call = c;
    return true;
}

bool aprsPasscodeWellFormed(const std::string& pass) {
    if (pass == "-1") { return true; }
    if (pass.empty() || pass.size() > 5) { return false; }
    int v = 0;
    for (char c : pass) {
        if (c < '0' || c > '9') { return false; }
        v = v * 10 + (c - '0');
    }
    return v <= 32767;
}

bool aprsBuildLogin(const AprsIsConfig& cfg, std::string& line, std::string& error) {
    std::string call = cfg.callsign;
    if (!aprsNormaliseCallsign(call)) {
        error = "the callsign is not valid for APRS-IS (3 to 9 characters, letters and digits, an optional -SSID of one or two characters, not -0)";
        return false;
    }
    auto oneWord = [](const std::string& w) {
        if (w.empty() || w.size() > 32) { return false; }
        for (char c : w) {
            if (c <= ' ' || c >= 127) { return false; }
        }
        return true;
    };
    if (!oneWord(cfg.softwareName) || !oneWord(cfg.softwareVersion)) {
        error = "the software name and version must each be one printable word with no spaces";
        return false;
    }
    if (cfg.filter.size() > kMaxFilterChars || hasControl(cfg.filter)) {
        error = "the filter is too long or contains a line break";
        return false;
    }
    for (char c : cfg.filter) {
        if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) > 126) {
            error = "the filter may hold only printable ASCII";
            return false;
        }
    }
    std::string pass = "-1";  // RECEIVE-ONLY unless gating is switched on
    if (cfg.gateway) {
        if (!aprsPasscodeWellFormed(cfg.passcode) || cfg.passcode == "-1") {
            error = "gateway mode needs the passcode you were issued for this callsign (digits only); none was supplied";
            return false;
        }
        pass = cfg.passcode;
    }
    line = "user " + call + " pass " + pass + " vers " + cfg.softwareName + " " + cfg.softwareVersion;
    if (!cfg.filter.empty()) { line += " filter " + cfg.filter; }
    line += "\r\n";
    if (line.size() > kAprsMaxLineBytes) {
        error = "the login line would exceed 512 bytes";
        return false;
    }
    return true;
}

std::string aprsRedactedLogin(const std::string& loginLine) {
    const std::size_t p = loginLine.find(" pass ");
    if (p == std::string::npos) { return loginLine; }
    const std::size_t start = p + 6;
    const std::size_t end = loginLine.find(' ', start);
    return loginLine.substr(0, start) + "***" + (end == std::string::npos ? std::string() : loginLine.substr(end));
}

AprsLine aprsClassify(const std::string& line) {
    if (line.empty()) { return AprsLine::Empty; }
    return line[0] == '#' ? AprsLine::Comment : AprsLine::Packet;
}

AprsLogresp aprsParseLogresp(const std::string& commentLine) {
    AprsLogresp r;
    if (commentLine.empty() || commentLine[0] != '#') { return r; }
    const std::string t = upper(commentLine);
    const std::size_t lr = t.find("LOGRESP");
    if (lr == std::string::npos) { return r; }
    r.isLogresp = true;
    const std::string rest = t.substr(lr + 7);
    // whole words only: "UNVERIFIED" must not count as "VERIFIED"
    for (const std::string& w : split(rest, ' ')) {
        std::string word = w;
        while (!word.empty() && (word.back() == ',' || word.back() == ';')) { word.pop_back(); }
        if (word == "VERIFIED") { r.verified = true; }
    }
    return r;
}

int aprsNextBackoffMs(int attempt, int initialMs, int maxMs) {
    if (initialMs < 1) { initialMs = 1; }
    if (maxMs < initialMs) { maxMs = initialMs; }
    long long d = initialMs;
    for (int i = 0; i < attempt && d < maxMs; ++i) { d *= 2; }
    return static_cast<int>(std::min<long long>(d, maxMs));
}

const char* aprsGateReasonName(GateReason r) {
    switch (r) {
        case GateReason::Gated: return "gated";
        case GateReason::NotUiFrame: return "not a UI frame (control 0x03, PID 0xF0)";
        case GateReason::PassAll: return "TNC has PASSALL on";
        case GateReason::Malformed: return "malformed packet";
        case GateReason::ForbiddenPathElement: return "TCPIP, TCPXX, NOGATE or RFONLY in the header";
        case GateReason::QConstructOnRf: return "q construct or I in an RF header";
        case GateReason::GenericQuery: return "generic query";
        case GateReason::ThirdPartyInternet: return "third-party packet that came from the internet";
        case GateReason::NestedThirdParty: return "nested third-party packet";
        case GateReason::TooLong: return "line too long";
        case GateReason::GatewayOff: return "gateway is off";
        case GateReason::NotOnline: return "not logged in";
        case GateReason::NotVerified: return "login not verified";
        case GateReason::QueueFull: return "queue full";
    }
    return "?";
}

GateDecision aprsGate(const GateInput& in, const std::string& igateCall) {
    GateDecision d;
    if (!in.controlPidOk) { d.reason = GateReason::NotUiFrame; return d; }
    if (in.tncPassAll) { d.reason = GateReason::PassAll; return d; }
    if (in.tnc2.empty() || hasControl(in.tnc2)) { d.reason = GateReason::Malformed; return d; }
    const std::size_t colon = in.tnc2.find(':');
    if (colon == std::string::npos || colon + 1 >= in.tnc2.size()) { d.reason = GateReason::Malformed; return d; }
    std::string src;
    std::vector<std::string> rest;
    if (!parseHeader(in.tnc2.substr(0, colon), src, rest)) { d.reason = GateReason::Malformed; return d; }
    for (const std::string& e : rest) {
        if (isQOrI(e)) { d.reason = GateReason::QConstructOnRf; return d; }
        if (isForbiddenElement(e, false)) { d.reason = GateReason::ForbiddenPathElement; return d; }
    }
    // the source and destination fields are names too: TCPIP as a destination is as forbidden as in the path
    if (isForbiddenElement(src, false)) { d.reason = GateReason::ForbiddenPathElement; return d; }
    const char type = in.tnc2[colon + 1];
    if (type == '?') { d.reason = GateReason::GenericQuery; return d; }

    std::string out = in.tnc2;
    if (type == '}') {
        // THIRD PARTY: the inner packet is what is gated, without the RF header and without the '}'.
        const std::string inner = in.tnc2.substr(colon + 2);
        const std::size_t c2 = inner.find(':');
        if (c2 == std::string::npos || c2 + 1 >= inner.size()) { d.reason = GateReason::Malformed; return d; }
        std::string isrc;
        std::vector<std::string> irest;
        if (!parseHeader(inner.substr(0, c2), isrc, irest)) { d.reason = GateReason::Malformed; return d; }
        for (const std::string& e : irest) {
            if (isForbiddenElement(e, false) && (bare(e) == "TCPIP" || bare(e) == "TCPXX")) { d.reason = GateReason::ThirdPartyInternet; return d; }
            if (isQOrI(e)) { d.reason = GateReason::QConstructOnRf; return d; }
            if (isForbiddenElement(e, false)) { d.reason = GateReason::ForbiddenPathElement; return d; }
        }
        if (isForbiddenElement(isrc, false)) { d.reason = GateReason::ForbiddenPathElement; return d; }
        if (inner[c2 + 1] == '}') { d.reason = GateReason::NestedThirdParty; return d; }
        if (inner[c2 + 1] == '?') { d.reason = GateReason::GenericQuery; return d; }
        out = inner;
    }
    const std::size_t c3 = out.find(':');
    out = out.substr(0, c3) + ",qAO," + igateCall + out.substr(c3);
    if (out.size() > kMaxGatedLine) { d.reason = GateReason::TooLong; return d; }
    d.reason = GateReason::Gated;
    d.line = out;
    return d;
}

const char* aprsStateName(AprsState s) {
    switch (s) {
        case AprsState::Stopped: return "stopped";
        case AprsState::Connecting: return "connecting";
        case AprsState::WaitingForServer: return "waiting for the server";
        case AprsState::LoggedIn: return "logged in";
        case AprsState::Online: return "online";
        case AprsState::BackingOff: return "waiting to reconnect";
    }
    return "?";
}

// ---- the client --------------------------------------------------------------------------------------------------------------------

AprsIsClient::AprsIsClient(const AprsIsConfig& cfg) : cfg_(cfg), out_(std::max<std::size_t>(cfg.outQueueBytes, 4096)) {}

AprsIsClient::~AprsIsClient() { stop(); }

bool AprsIsClient::start(std::string& error) {
    if (thread_.joinable()) {
        error = "already running";
        return false;
    }
    if (cfg_.host.empty()) {
        error = "no APRS-IS server has been chosen";
        return false;
    }
    if (cfg_.port == 0) {
        error = "the port must be 1..65535";
        return false;
    }
    if (cfg_.backoffInitialMs < 1 || cfg_.backoffMaxMs < cfg_.backoffInitialMs || cfg_.idleTimeoutMs < 100 || cfg_.connectTimeoutMs < 10) {
        error = "a timing value is out of range";
        return false;
    }
    if (!aprsBuildLogin(cfg_, login_, error)) { return false; }
    std::string c = cfg_.callsign;
    aprsNormaliseCallsign(c);
    cfg_.callsign = c;
    std::vector<std::uint8_t> sink(out_.maxMessage());
    while (out_.pop(sink.data(), sink.size()) != 0) {}
    verified_.store(false);
    unverified_.store(false);
    stopFlag_.store(false);
    state_.store(AprsState::Connecting);
    thread_ = std::thread([this] { run(); });
    return true;
}

void AprsIsClient::stop() {
    if (!thread_.joinable()) { return; }
    stopFlag_.store(true);
    thread_.join();
    state_.store(AprsState::Stopped);
}

AprsStats AprsIsClient::stats() const {
    AprsStats s;
    s.connects = connects_.load();
    s.failures = failures_.load();
    s.linesReceived = lines_.load();
    s.commentLines = comments_.load();
    s.packetsReceived = packets_.load();
    s.idleReconnects = idleReconnects_.load();
    s.hostileDrops = hostile_.load();
    s.longLinesSkipped = longSkipped_.load();
    s.packetsForwarded = forwarded_.load();
    s.gateRefused = refused_.load();
    s.bytesWritten = written_.load();
    return s;
}

std::string AprsIsClient::lastError() const {
    std::lock_guard<std::mutex> g(errMutex_);
    return lastError_;
}

void AprsIsClient::setError(const std::string& e) {
    std::lock_guard<std::mutex> g(errMutex_);
    lastError_ = e;
}

GateReason AprsIsClient::forward(const GateInput& in) {
    auto refuse = [&](GateReason r) {
        refused_.fetch_add(1, std::memory_order_relaxed);
        return r;
    };
    if (!cfg_.gateway) { return refuse(GateReason::GatewayOff); }
    if (state_.load() != AprsState::Online) { return refuse(GateReason::NotOnline); }
    if (!verified_.load() || unverified_.load()) { return refuse(GateReason::NotVerified); }
    const GateDecision d = aprsGate(in, cfg_.callsign);
    if (d.reason != GateReason::Gated) { return refuse(d.reason); }
    if (!out_.push(d.line.data(), d.line.size())) { return refuse(GateReason::QueueFull); }
    return GateReason::Gated;
}

void AprsIsClient::run() {
    int attempt = 0;
    while (!stopFlag_.load()) {
        state_.store(AprsState::Connecting);
        std::uint32_t ip = 0;
        bool ok = sock::resolveIpv4(cfg_.host, ip);
        if (!ok) { setError("could not resolve the server name"); }
        sock::Handle s = sock::kInvalid;
        if (ok) {
            s = sock::beginConnectTcp(ip, cfg_.port);
            ok = s != sock::kInvalid;
            if (!ok) { setError("could not start a connection"); }
        }
        if (ok) {
            // wait for the connect in slices so stop() is never held for the whole timeout
            const std::int64_t t0 = nowMs();
            bool connected = false;
            while (!stopFlag_.load() && nowMs() - t0 < cfg_.connectTimeoutMs) {
                const int st = sock::connectState(s, kReadSliceMs);
                if (st == 1) { connected = true; }
                if (st != 0) { break; }  // connected, or refused: no reason to wait out the timeout
            }
            ok = connected;
            if (!ok && !stopFlag_.load()) { setError("the server did not accept the connection"); }
        }
        bool reachedOnline = false;
        std::int64_t onlineSince = 0;
        if (ok) {
            connects_.fetch_add(1);
            sock::setNoDelay(s);
            onlineSince = nowMs();
            reachedOnline = session(s);
            if (reachedOnline && nowMs() - onlineSince >= cfg_.stableMs) { attempt = 0; }
        }
        sock::closeHandle(s);
        if (stopFlag_.load()) { break; }
        failures_.fetch_add(1);
        verified_.store(false);
        unverified_.store(false);
        state_.store(AprsState::BackingOff);
        const int wait = aprsNextBackoffMs(attempt, cfg_.backoffInitialMs, cfg_.backoffMaxMs);
        ++attempt;
        const std::int64_t until = nowMs() + wait;
        while (!stopFlag_.load() && nowMs() < until) { sock::pollMany(nullptr, 0, kReadSliceMs); }
    }
    state_.store(AprsState::Stopped);
}

// One connected session. Returns true when the server answered the login (the session was "online"), false when it never did.
bool AprsIsClient::session(sock::Handle s) {
    std::string inbuf;
    std::vector<std::uint8_t> msg(out_.maxMessage());
    std::string pending;  // outgoing bytes not yet accepted by the socket
    bool loginSent = false;
    bool answered = false;
    const std::int64_t t0 = nowMs();
    std::int64_t lastRx = t0;
    state_.store(AprsState::WaitingForServer);
    std::uint8_t buf[2048];

    auto sendLogin = [&] {
        pending += login_;
        loginSent = true;
        state_.store(AprsState::LoggedIn);
    };

    while (!stopFlag_.load()) {
        const std::int64_t now = nowMs();
        // The page: the server sends an identification line and the client logs in after it appears. Wait for it, but not for ever.
        if (!loginSent && now - t0 >= cfg_.loginWaitMs) { sendLogin(); }
        // Packets to gate go out only after the login was answered, and only with gating on.
        if (answered && cfg_.gateway) {
            for (;;) {
                if (pending.size() >= kAprsMaxLineBytes * 8) { break; }
                const std::size_t n = out_.pop(msg.data(), msg.size());
                if (n == 0) { break; }
                pending.append(reinterpret_cast<const char*>(msg.data()), n);
                pending += "\r\n";
                forwarded_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        sock::PollItem it;
        it.s = s;
        it.want = sock::kReadable | (pending.empty() ? 0u : sock::kWritable);
        if (sock::pollMany(&it, 1, kReadSliceMs) < 0) { return answered; }
        if (it.got & sock::kReadable) {
            for (int k = 0; k < 16; ++k) {
                const long r = sock::recvSome(s, buf, sizeof(buf));
                if (r > 0) {
                    lastRx = nowMs();
                    inbuf.append(reinterpret_cast<const char*>(buf), static_cast<std::size_t>(r));
                    for (;;) {
                        const std::size_t nl = inbuf.find('\n');
                        if (nl == std::string::npos) { break; }
                        std::string line = inbuf.substr(0, nl);
                        inbuf.erase(0, nl + 1);
                        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
                        lines_.fetch_add(1);
                        if (line.size() + 2 > kAprsMaxLineBytes) {
                            longSkipped_.fetch_add(1);  // the protocol allows 512 bytes a line: anything longer is not handed on
                            continue;
                        }
                        switch (aprsClassify(line)) {
                            case AprsLine::Empty: break;
                            case AprsLine::Comment: {
                                comments_.fetch_add(1);
                                if (!loginSent) { sendLogin(); }  // the identification line has appeared
                                const AprsLogresp lr = aprsParseLogresp(line);
                                if (lr.isLogresp) {
                                    answered = true;
                                    verified_.store(lr.verified);
                                    unverified_.store(!lr.verified);
                                    state_.store(AprsState::Online);
                                }
                                break;
                            }
                            case AprsLine::Packet:
                                packets_.fetch_add(1);
                                if (!loginSent) { sendLogin(); }
                                if (handler_) { handler_(line); }
                                break;
                        }
                    }
                    if (inbuf.size() > kMaxIncomingLine) {
                        hostile_.fetch_add(1);
                        setError("the server sent a line far longer than the 512 bytes the protocol allows; the connection was dropped");
                        return answered;
                    }
                    continue;
                }
                if (r == 0) {
                    setError("the server closed the connection");
                    return answered;
                }
                if (!sock::wouldBlock()) {
                    setError("the connection failed");
                    return answered;
                }
                break;
            }
        } else if (it.got & sock::kError) {
            setError("the connection failed");
            return answered;
        }
        if (!pending.empty() && (it.got & sock::kWritable)) {
            const long w = sock::sendSome(s, pending.data(), pending.size());
            if (w > 0) {
                written_.fetch_add(static_cast<std::uint64_t>(w));
                pending.erase(0, static_cast<std::size_t>(w));
            } else if (!(w < 0 && sock::wouldBlock())) {
                setError("the connection failed while writing");
                return answered;
            }
        }
        if (nowMs() - lastRx >= cfg_.idleTimeoutMs) {
            idleReconnects_.fetch_add(1);
            setError("nothing was heard from the server (not even a # keep-alive line) for too long; reconnecting");
            return answered;
        }
    }
    return answered;
}

}  // namespace cascade::net
