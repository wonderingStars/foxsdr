// antenna_genius.hpp - a client for the 4O3A Antenna Genius (antenna switch) Ethernet API: the line protocol, the message parsers, and a small client
// over an abstract byte transport. READ operations (port state, antenna list, band list, status subscriptions) plus ONE write, selecting the antenna on a
// radio port, which exists only as a user action.
//
// SOURCE (maker's own protocol documents, opened 2026-11-11): the 4o3a/genius-api-docs GitHub wiki (Marko Tomasevic, 4O3A), pages "Antenna Genius TCPIP API",
// "...TCPIP port", "...TCPIP band", "...TCPIP antenna", "...TCPIP sub", "Antenna Genius Status Messages": TCP port 9007; the device sends a prologue
// "V<a.b.c> AG[ AUTH]" (firmware version, device id, AUTH when the client came from a WAN and must authenticate); a client sends "C<seq>|command" ended by
// 0x0D with seq 1..255; the device answers "R<seq>|<hex code>|<message>" (code 0 = success); asynchronous status is "S0|<message>"; decimal values use a
// period; "port get <1|2>", "port set <1|2> [auto=] [source=] [band=] [rxant=] [txant=]", "antenna list" (one line per antenna: "antenna <n> name= tx= rx=
// inband=", masks in hex, "followed by an OK status"), "band list" ("band <n> name= freq_start= freq_stop=", MHz), "sub port <n|all>", "sub relay";
// statuses "port <n> auto= source= band= rxant= txant= inband= tx= inhibit=", "relay tx= rx= state=", "antenna reload", "output reload". The pages state no
// licence. UNVERIFIED against a real device: the line terminator the device uses on its replies (the reader accepts CR, LF or CR LF), whether "port get"
// is also followed by an empty OK line, the "sub" forms for antenna and output statuses, and "ping" / "info" / "unsub" syntax (those pages were not opened).
//
// NOT BUILT, ON PURPOSE: reboot, stack, network, conf (factory reset), auth (credentials), flex, group and output (relay outputs), band set / band init, antenna
// set (configuration). The other Genius products (Power Genius, Tuner Genius, Station Genius) are NOT here: a power amplifier or tuner command could reach a
// transmitter. Nothing in this file keys anything.
//
// SAFETY: the one write, userSetPortAntenna(), is only ever called by an explicit user action (there is no timer, no band-follow, no automatic path in this
// file: a test scans for exactly one place that builds "port set"), and it is refused while the port's last known state says it is transmitting or inhibited.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace cascade::core::genius {

inline constexpr int kMaxAntennas = 16;     // the API says "1 to xx"; a bound for this client
inline constexpr int kMaxBandId = 15;       // "band: 0-15"
inline constexpr std::size_t kMaxLine = 1024;
inline constexpr std::size_t kMaxPending = 16;
inline constexpr std::size_t kMaxStatusQueue = 64;

struct Greeting {
    bool valid = false;
    std::string version;     // "4.0.1"
    bool needsAuth = false;
};
struct PortState {
    bool valid = false;
    int id = 0;
    bool autoDetect = false;
    std::string source;
    int band = 0, rxant = 0, txant = 0, inband = 0;
    bool tx = false, inhibit = false;
};
struct AntennaInfo {
    int id = 0;
    std::string name;
    unsigned txMask = 0, rxMask = 0, inbandMask = 0;
};
struct BandInfo {
    int id = 0;
    std::string name;
    double freqStartMHz = 0, freqStopMHz = 0;
};
struct RelayState { unsigned tx = 0, rx = 0, state = 0; };

enum class LineKind { Unknown, Prologue, Response, Status };
struct Line {
    LineKind kind = LineKind::Unknown;
    int seq = 0;           // responses
    unsigned code = 0;     // responses: the hex code
    std::string body;      // responses and status: the message
    Greeting greeting;     // prologue
};

namespace detail {

inline bool isHex(const std::string& s, std::size_t maxLen, unsigned& out) {
    if (s.empty() || s.size() > maxLen) { return false; }
    unsigned v = 0;
    for (char c : s) {
        int d;
        if (c >= '0' && c <= '9') { d = c - '0'; }
        else if (c >= 'a' && c <= 'f') { d = c - 'a' + 10; }
        else if (c >= 'A' && c <= 'F') { d = c - 'A' + 10; }
        else { return false; }
        v = (v << 4) | static_cast<unsigned>(d);
    }
    out = v;
    return true;
}
inline bool isDec(const std::string& s, int maxVal, int& out) {
    if (s.empty() || s.size() > 9) { return false; }
    long long v = 0;
    for (char c : s) { if (c < '0' || c > '9') { return false; } v = v * 10 + (c - '0'); }
    if (v > maxVal) { return false; }
    out = static_cast<int>(v);
    return true;
}
inline bool isDouble(const std::string& s, double& out) {
    if (s.empty() || s.size() > 16) { return false; }
    bool dot = false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '.') { if (dot) { return false; } dot = true; continue; }
        if (c < '0' || c > '9') { return false; }
    }
    out = std::stod(s);
    return std::isfinite(out);
}
inline std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> v;
    std::string cur;
    for (char c : s) { if (c == sep) { v.push_back(cur); cur.clear(); } else { cur.push_back(c); } }
    v.push_back(cur);
    return v;
}
// "k=v k=v ..." after the leading words; returns false on a token without '=' or an empty key, or on a repeated key
inline bool keyValues(const std::vector<std::string>& toks, std::size_t from, std::map<std::string, std::string>& kv) {
    kv.clear();
    for (std::size_t i = from; i < toks.size(); ++i) {
        if (toks[i].empty()) { continue; }
        const std::size_t eq = toks[i].find('=');
        if (eq == std::string::npos || eq == 0) { return false; }
        const std::string k = toks[i].substr(0, eq);
        for (char c : k) { if (!(std::islower(static_cast<unsigned char>(c)) || c == '_')) { return false; } }
        if (!kv.emplace(k, toks[i].substr(eq + 1)).second) { return false; }
    }
    return true;
}
inline bool plainWord(const std::string& s, std::size_t maxLen) {
    if (s.empty() || s.size() > maxLen) { return false; }
    for (char c : s) { if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.')) { return false; } }
    return true;
}

}  // namespace detail

// "V4.0.1 AG" or "V4.0.1 AG AUTH". Anything else is not a prologue.
inline bool parseGreeting(const std::string& line, Greeting& g) {
    g = Greeting();
    const auto t = detail::split(line, ' ');
    if (t.size() < 2 || t.size() > 3 || t[0].size() < 2 || t[0][0] != 'V' || t[1] != "AG") { return false; }
    if (t.size() == 3 && t[2] != "AUTH") { return false; }
    const std::string ver = t[0].substr(1);
    if (!detail::plainWord(ver, 16) || ver.find_first_not_of("0123456789.") != std::string::npos) { return false; }
    g.valid = true;
    g.version = ver;
    g.needsAuth = t.size() == 3;
    return true;
}

// One line, without its terminator. Kind Unknown when it is none of the three shapes or is malformed.
inline Line parseLine(const std::string& line) {
    Line l;
    if (line.empty() || line.size() > kMaxLine) { return l; }
    if (line[0] == 'V') {
        if (parseGreeting(line, l.greeting)) { l.kind = LineKind::Prologue; }
        return l;
    }
    if (line[0] != 'R' && line[0] != 'S') { return l; }
    const std::size_t bar1 = line.find('|');
    if (bar1 == std::string::npos) { return l; }
    int seq = 0;
    if (!detail::isDec(line.substr(1, bar1 - 1), 255, seq)) { return l; }
    if (line[0] == 'S') {
        if (seq != 0) { return l; }   // "S0|": the number is reserved and 0
        l.kind = LineKind::Status;
        l.body = line.substr(bar1 + 1);
        return l;
    }
    if (seq < 1) { return l; }
    const std::size_t bar2 = line.find('|', bar1 + 1);
    if (bar2 == std::string::npos) { return l; }
    unsigned code = 0;
    if (!detail::isHex(line.substr(bar1 + 1, bar2 - bar1 - 1), 8, code)) { return l; }
    l.kind = LineKind::Response;
    l.seq = seq;
    l.code = code;
    l.body = line.substr(bar2 + 1);
    return l;
}

// "port <n> auto=<0|1> source=<s> band=<b> rxant=<a> txant=<a> [inband=<m>] tx=<0|1> inhibit=<0|1>" (the get reply has no inband in the page's example)
inline bool parsePort(const std::string& body, PortState& p) {
    using namespace detail;
    p = PortState();
    const auto t = split(body, ' ');
    if (t.size() < 3 || t[0] != "port") { return false; }
    int id = 0;
    if (!isDec(t[1], 2, id) || id < 1) { return false; }
    std::map<std::string, std::string> kv;
    if (!keyValues(t, 2, kv)) { return false; }
    int v = 0;
    auto num = [&](const char* k, int maxV, int& out) { auto it = kv.find(k); return it != kv.end() && isDec(it->second, maxV, out); };
    int a = 0, bnd = 0, rx = 0, txa = 0, inb = 0, tx = 0, inh = 0;
    if (!num("auto", 1, a) || !num("band", kMaxBandId, bnd) || !num("rxant", kMaxAntennas, rx) || !num("txant", kMaxAntennas, txa) || !num("tx", 1, tx) || !num("inhibit", 1, inh)) { return false; }
    if (kv.count("inband") != 0 && !num("inband", 65535, inb)) { return false; }
    (void)v;
    auto s = kv.find("source");
    if (s == kv.end() || !plainWord(s->second, 16)) { return false; }
    p.valid = true;
    p.id = id;
    p.autoDetect = a == 1;
    p.source = s->second;
    p.band = bnd; p.rxant = rx; p.txant = txa; p.inband = inb; p.tx = tx == 1; p.inhibit = inh == 1;
    return true;
}

inline bool parseAntenna(const std::string& body, AntennaInfo& a) {
    using namespace detail;
    a = AntennaInfo();
    const auto t = split(body, ' ');
    if (t.size() < 3 || t[0] != "antenna") { return false; }
    int id = 0;
    if (!isDec(t[1], kMaxAntennas, id) || id < 1) { return false; }
    std::map<std::string, std::string> kv;
    if (!keyValues(t, 2, kv)) { return false; }
    unsigned tx = 0, rx = 0, inb = 0;
    auto nm = kv.find("name");
    if (nm == kv.end() || !plainWord(nm->second, 32)) { return false; }
    if (kv.count("tx") == 0 || !isHex(kv["tx"], 4, tx) || kv.count("rx") == 0 || !isHex(kv["rx"], 4, rx) || kv.count("inband") == 0 || !isHex(kv["inband"], 4, inb)) { return false; }
    a.id = id; a.name = nm->second; a.txMask = tx; a.rxMask = rx; a.inbandMask = inb;
    return true;
}

inline bool parseBand(const std::string& body, BandInfo& b) {
    using namespace detail;
    b = BandInfo();
    const auto t = split(body, ' ');
    if (t.size() < 3 || t[0] != "band") { return false; }
    int id = 0;
    if (!isDec(t[1], kMaxBandId, id)) { return false; }
    std::map<std::string, std::string> kv;
    if (!keyValues(t, 2, kv)) { return false; }
    auto nm = kv.find("name");
    double lo = 0, hi = 0;
    if (nm == kv.end() || !plainWord(nm->second, 32) || kv.count("freq_start") == 0 || !isDouble(kv["freq_start"], lo) || kv.count("freq_stop") == 0 || !isDouble(kv["freq_stop"], hi)) { return false; }
    if (hi < lo) { return false; }
    b.id = id; b.name = nm->second; b.freqStartMHz = lo; b.freqStopMHz = hi;
    return true;
}

inline bool parseRelay(const std::string& body, RelayState& r) {
    using namespace detail;
    r = RelayState();
    const auto t = split(body, ' ');
    if (t.size() != 4 || t[0] != "relay") { return false; }
    std::map<std::string, std::string> kv;
    if (!keyValues(t, 1, kv)) { return false; }
    unsigned tx = 0, rx = 0, st = 0;
    if (kv.count("tx") == 0 || !isHex(kv["tx"], 4, tx) || kv.count("rx") == 0 || !isHex(kv["rx"], 4, rx) || kv.count("state") == 0 || !isHex(kv["state"], 4, st)) { return false; }
    r.tx = tx; r.rx = rx; r.state = st;
    return true;
}

// ---- command builders (the text after "C<seq>|"; the client adds the framing) -----------------------------------------------------------------

inline std::string frameCommand(int seq, const std::string& cmd) {
    return "C" + std::to_string(seq) + "|" + cmd + "\r";
}

class Transport {
public:
    virtual ~Transport() = default;
    virtual int send(const char* data, std::size_t len) = 0;   // bytes sent, -1 on failure
    virtual int recv(char* buf, std::size_t cap) = 0;          // bytes read, 0 nothing waiting, -1 closed
};

struct StatusMessage {
    enum class Kind { Port, Relay, AntennaReload, OutputReload, Other } kind = Kind::Other;
    PortState port;
    RelayState relay;
};

struct Result {
    bool done = false;
    unsigned code = 0;
    std::vector<std::string> lines;   // data lines (messages) that came with the response
};

class AntennaGeniusClient {
public:
    explicit AntennaGeniusClient(Transport* t) : t_(t) {}

    // Reads whatever the transport has and handles every complete line. Cheap; call it from the owner's worker.
    void poll() {
        if (t_ == nullptr) { return; }
        char buf[512];
        for (int rounds = 0; rounds < 16; ++rounds) {
            const int n = t_->recv(buf, sizeof(buf));
            if (n < 0) { closed_ = true; return; }
            if (n == 0) { return; }
            for (int i = 0; i < n; ++i) {
                const char c = buf[i];
                if (c == '\r' || c == '\n') {
                    if (overflow_) { overflow_ = false; ++droppedLines_; }
                    else if (!line_.empty()) { handle(line_); }
                    line_.clear();
                    continue;
                }
                if (overflow_) { continue; }
                if (line_.size() >= kMaxLine) { overflow_ = true; line_.clear(); continue; }
                line_.push_back(c);
            }
        }
    }

    bool closed() const { return closed_; }
    const Greeting& greeting() const { return greeting_; }
    std::uint64_t droppedLines() const { return droppedLines_; }
    std::uint64_t malformed() const { return malformed_; }
    std::uint64_t unmatched() const { return unmatched_; }

    // -- reads (each returns the sequence number, 0 when it could not be sent) --
    int requestPortGet(int portId) { return portId >= 1 && portId <= 2 ? send("port get " + std::to_string(portId), Kind::PortGet, portId) : 0; }
    int requestAntennaList() { return send("antenna list", Kind::AntennaList, 0); }
    int requestBandList() { return send("band list", Kind::BandList, 0); }
    int subscribePort(int portIdOrZeroForAll) {
        if (portIdOrZeroForAll < 0 || portIdOrZeroForAll > 2) { return 0; }
        return send(portIdOrZeroForAll == 0 ? std::string("sub port all") : "sub port " + std::to_string(portIdOrZeroForAll), Kind::Sub, 0);
    }
    int subscribeRelay() { return send("sub relay", Kind::Sub, 0); }

    // -- THE ONE WRITE: an antenna selection on a radio port, called only by a user action. Refused (0 and a sentence in `error`) for a bad port or antenna
    // number, while the port's last known state says it transmits or is inhibited, or when nothing is known about the port yet (get it first).
    int userSetPortAntenna(int portId, int rxant, int txant, std::string& error) {
        if (portId < 1 || portId > 2) { error = "the port must be 1 or 2"; return 0; }
        if (rxant < 0 || rxant > kMaxAntennas || txant < 0 || txant > kMaxAntennas) { error = "an antenna number is out of range"; return 0; }
        const PortState& p = ports_[portId - 1];
        if (!p.valid) { error = "the port state is not known yet: read it first"; return 0; }
        if (p.tx) { error = "the port is transmitting"; return 0; }
        if (p.inhibit) { error = "the port is inhibited"; return 0; }
        error.clear();
        return send("port set " + std::to_string(portId) + " rxant=" + std::to_string(rxant) + " txant=" + std::to_string(txant), Kind::PortSet, portId);
    }

    // -- state --
    PortState port(int id) const { return id >= 1 && id <= 2 ? ports_[id - 1] : PortState(); }
    const std::vector<AntennaInfo>& antennas() const { return antennas_; }
    const std::vector<BandInfo>& bands() const { return bands_; }
    RelayState relay() const { return relay_; }
    bool takeStatus(StatusMessage& m) {
        if (statusQ_.empty()) { return false; }
        m = statusQ_.front();
        statusQ_.pop_front();
        return true;
    }
    Result result(int seq) const {
        auto it = pending_.find(seq);
        if (it != pending_.end()) { return it->second.res; }
        auto d = finished_.find(seq);
        return d != finished_.end() ? d->second : Result();
    }
    std::size_t pendingCount() const { return pending_.size(); }

private:
    enum class Kind { PortGet, PortSet, AntennaList, BandList, Sub };
    struct Pending { Kind kind; int arg; Result res; };

    int send(const std::string& cmd, Kind k, int arg) {
        if (t_ == nullptr || closed_ || pending_.size() >= kMaxPending) { return 0; }
        int seq = 0;
        for (int tries = 0; tries < 255; ++tries) {
            nextSeq_ = nextSeq_ % 255 + 1;
            if (pending_.count(nextSeq_) == 0) { seq = nextSeq_; break; }
        }
        if (seq == 0) { return 0; }
        const std::string wire = frameCommand(seq, cmd);
        if (t_->send(wire.data(), wire.size()) != static_cast<int>(wire.size())) { return 0; }
        pending_[seq] = Pending{k, arg, Result()};
        finished_.erase(seq);
        return seq;
    }

    void handle(const std::string& text) {
        const Line l = parseLine(text);
        switch (l.kind) {
            case LineKind::Prologue: greeting_ = l.greeting; return;
            case LineKind::Status: handleStatus(l.body); return;
            case LineKind::Response: handleResponse(l); return;
            default: ++malformed_; return;
        }
    }

    void handleResponse(const Line& l) {
        auto it = pending_.find(l.seq);
        if (it == pending_.end()) { ++unmatched_; return; }
        Pending& p = it->second;
        p.res.code = l.code;
        const bool listKind = p.kind == Kind::AntennaList || p.kind == Kind::BandList;
        if (l.code != 0) { finish(it); return; }          // a failure code ends the command
        if (!l.body.empty()) {
            if (p.res.lines.size() < 64) { p.res.lines.push_back(l.body); }
            apply(p, l.body);
            if (!listKind) { finish(it); }                  // a single-line reply (get, set) is complete; a list ends on the empty OK line
            return;
        }
        finish(it);                                        // an empty OK line
    }

    void finish(std::map<int, Pending>::iterator it) {
        it->second.res.done = true;
        if (finished_.size() >= 32) { finished_.erase(finished_.begin()); }
        finished_[it->first] = it->second.res;
        pending_.erase(it);
    }

    void apply(const Pending& p, const std::string& body) {
        if (p.kind == Kind::PortGet) {
            PortState ps;
            if (parsePort(body, ps) && ps.id == p.arg) { ports_[ps.id - 1] = ps; } else { ++malformed_; }
        } else if (p.kind == Kind::AntennaList) {
            AntennaInfo a;
            if (parseAntenna(body, a)) { upsert(a); } else { ++malformed_; }
        } else if (p.kind == Kind::BandList) {
            BandInfo b;
            if (parseBand(body, b)) { upsert(b); } else { ++malformed_; }
        }
    }
    void upsert(const AntennaInfo& a) {
        for (AntennaInfo& x : antennas_) { if (x.id == a.id) { x = a; return; } }
        if (antennas_.size() < static_cast<std::size_t>(kMaxAntennas)) { antennas_.push_back(a); }
    }
    void upsert(const BandInfo& b) {
        for (BandInfo& x : bands_) { if (x.id == b.id) { x = b; return; } }
        if (bands_.size() <= static_cast<std::size_t>(kMaxBandId)) { bands_.push_back(b); }
    }

    void handleStatus(const std::string& body) {
        StatusMessage m;
        if (body == "antenna reload") { m.kind = StatusMessage::Kind::AntennaReload; antennas_.clear(); }
        else if (body == "output reload") { m.kind = StatusMessage::Kind::OutputReload; }
        else if (body.rfind("port ", 0) == 0) {
            if (!parsePort(body, m.port)) { ++malformed_; return; }
            m.kind = StatusMessage::Kind::Port;
            ports_[m.port.id - 1] = m.port;
        } else if (body.rfind("relay ", 0) == 0) {
            if (!parseRelay(body, m.relay)) { ++malformed_; return; }
            m.kind = StatusMessage::Kind::Relay;
            relay_ = m.relay;
        } else {
            ++malformed_;   // a status this client does not know: counted, not queued
            return;
        }
        if (statusQ_.size() >= kMaxStatusQueue) { statusQ_.pop_front(); }
        statusQ_.push_back(m);
    }

    Transport* t_;
    bool closed_ = false;
    Greeting greeting_;
    std::string line_;
    bool overflow_ = false;
    int nextSeq_ = 0;
    std::map<int, Pending> pending_;
    std::map<int, Result> finished_;
    PortState ports_[2];
    std::vector<AntennaInfo> antennas_;
    std::vector<BandInfo> bands_;
    RelayState relay_;
    std::deque<StatusMessage> statusQ_;
    std::uint64_t droppedLines_ = 0, malformed_ = 0, unmatched_ = 0;
};

}  // namespace cascade::core::genius
