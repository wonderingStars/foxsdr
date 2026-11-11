// Antenna rotator control: a client for the rotctld network protocol, the Yaesu GS-232A/B serial command set, and a tracker that turns a stream of
// target azimuth/elevation (a satellite or an aircraft) into sparse, safe rotator commands.
//
// SOURCES (documentation only; no Hamlib source and no other program's source was read):
//  - rotctld(1), Debian testing, libhamlib-utils 4.7.2, page dated 2020-09-09, https://manpages.debian.org/testing/libhamlib-utils/rotctld.1.en.html
//    (read 2026-11-11): TCP daemon, default port 4533; default protocol = one command per line, set commands answer "RPRT x" (0 = success, a negative
//    Hamlib code otherwise), get commands answer one value per line; P (set_pos, azimuth then elevation, floating point; "most rotators don't support
//    elevation, pass 0.0"), p (get_pos: azimuth line, elevation line), S (stop), K (park).
//  - Yaesu GS-232A "Computer Control Interface for Antenna Rotators" operation manual (copy at https://www.qsl.net/on7eq/pdf/GS232A.pdf, text
//    extracted from the PDF 2026-11-11): commands end with CR (0Dh), upper or lower case; the interface answers CR after every command and LF as well
//    when data is returned; an invalid command answers "?>"; angles sent are three digits, zero padded; Maaa (azimuth), Waaa eee (azimuth and
//    elevation), S (all stop), C (azimuth, returned "+0aaa"), C2 (azimuth and elevation, returned "+0aaa+0eee"), B (elevation, "+0eee"), R L A U D E
//    (turn and stop), Xn (speed 1..4); azimuth 000-360 or 450 "vary according to controller type", elevation 000-180. THE GS-232B is NOT covered by that
//    manual: the same command letters are used; its C2 answer is parsed in the "AZ=aaa EL=eee" form as well as the A form (the B form is UNVERIFIED).
//    The GS-232 has no park command: parking is a move to the configured park position.
//
// NO ROTATOR WAS AVAILABLE: everything here is UNVERIFIED ON HARDWARE and is tested against stand-in transports and a simulated rotator.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "core/power_log.hpp"  // the shared socket helpers (detail::dial and friends) for the rotctld TCP transport

namespace cascade::core::rotator {

struct Position {
    double az = std::nan("");
    double el = std::nan("");
    bool valid() const { return std::isfinite(az) && std::isfinite(el); }
};

namespace detail {
inline std::string fixed(double v, int decimals) {
    char b[48];
    const auto r = std::to_chars(b, b + sizeof b, v, std::chars_format::fixed, decimals);  // locale-independent
    return std::string(b, r.ptr);
}
// Strict number: the whole string, finite.
inline bool parseNumber(std::string_view s, double* out) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) { s.remove_prefix(1); }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) { s.remove_suffix(1); }
    if (s.empty()) { return false; }
    if (s.front() == '+') { s.remove_prefix(1); }
    double v = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc() || r.ptr != s.data() + s.size() || !std::isfinite(v)) { return false; }
    *out = v;
    return true;
}
inline double wrap360(double a) {
    a = std::fmod(a, 360.0);
    return a < 0 ? a + 360.0 : a;
}
}  // namespace detail

// ---- rotctld protocol -------------------------------------------------------------------------------------------------------------------

namespace rotctld {
constexpr int kDefaultPort = 4533;
inline std::string setPos(double az, double el) { return "P " + detail::fixed(az, 2) + " " + detail::fixed(el, 2) + "\n"; }
inline std::string getPos() { return "p\n"; }
inline std::string stop() { return "S\n"; }
inline std::string park() { return "K\n"; }
// "RPRT n" -> true with *code = n; anything else false.
inline bool parseRprt(std::string_view line, int* code) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) { line.remove_suffix(1); }
    if (line.rfind("RPRT ", 0) != 0) { return false; }
    line.remove_prefix(5);
    int v = 0;
    const auto r = std::from_chars(line.data(), line.data() + line.size(), v);
    if (r.ec != std::errc() || r.ptr != line.data() + line.size()) { return false; }
    *code = v;
    return true;
}
}  // namespace rotctld

// ---- GS-232 command set -----------------------------------------------------------------------------------------------------------------

namespace gs232 {
// Whole degrees, clamped, three digits. Empty string for a non-finite angle.
inline std::string deg3(double v, int maxDeg) {
    if (!std::isfinite(v)) { return std::string(); }
    int d = static_cast<int>(std::lround(v));
    d = std::clamp(d, 0, maxDeg);
    char b[8];
    std::snprintf(b, sizeof b, "%03d", d);
    return b;
}
inline std::string setPos(double az, double el, int azMaxDeg = 450) {
    const std::string a = deg3(az, azMaxDeg), e = deg3(el, 180);
    return (a.empty() || e.empty()) ? std::string() : "W" + a + " " + e + "\r";
}
inline std::string setAz(double az, int azMaxDeg = 450) {
    const std::string a = deg3(az, azMaxDeg);
    return a.empty() ? std::string() : "M" + a + "\r";
}
inline std::string stop() { return "S\r"; }
inline std::string readAzEl() { return "C2\r"; }
inline std::string readAz() { return "C\r"; }
inline std::string readEl() { return "B\r"; }
inline std::string speed(int n) { return (n >= 1 && n <= 4) ? "X" + std::to_string(n) + "\r" : std::string(); }
inline bool isErrorReply(std::string_view s) { return s.find('?') != std::string_view::npos; }

namespace detail2 {
inline bool intAt(std::string_view s, std::size_t* i, int* out) {  // up to 4 digits
    std::size_t n = 0;
    int v = 0;
    while (*i < s.size() && s[*i] >= '0' && s[*i] <= '9' && n < 4) { v = v * 10 + (s[*i] - '0'); ++*i; ++n; }
    if (n == 0) { return false; }
    *out = v;
    return true;
}
}  // namespace detail2

// Reply to C2: "+0aaa+0eee" (A) or "AZ=aaa EL=eee" (B form, unverified). False for anything else.
inline bool parseAzEl(std::string_view s, Position* out) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\r' || s.front() == '\n')) { s.remove_prefix(1); }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n')) { s.remove_suffix(1); }
    int a = 0, e = 0;
    std::size_t i = 0;
    if (s.rfind("AZ=", 0) == 0) {
        i = 3;
        if (!detail2::intAt(s, &i, &a)) { return false; }
        while (i < s.size() && s[i] == ' ') { ++i; }
        if (s.substr(i, 3) != "EL=") { return false; }
        i += 3;
        if (!detail2::intAt(s, &i, &e) || i != s.size()) { return false; }
    } else {
        if (s.size() < 1 || s[0] != '+') { return false; }
        i = 1;
        if (!detail2::intAt(s, &i, &a)) { return false; }
        if (i >= s.size() || s[i] != '+') { return false; }
        ++i;
        if (!detail2::intAt(s, &i, &e) || i != s.size()) { return false; }
    }
    if (a > 450 || e > 180) { return false; }
    out->az = a;
    out->el = e;
    return true;
}
// Reply to C or B: "+0nnn".
inline bool parseAngle(std::string_view s, double* out) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\r' || s.front() == '\n')) { s.remove_prefix(1); }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n')) { s.remove_suffix(1); }
    if (s.empty() || s[0] != '+') { return false; }
    std::size_t i = 1;
    int v = 0;
    if (!detail2::intAt(s, &i, &v) || i != s.size() || v > 450) { return false; }
    *out = v;
    return true;
}
}  // namespace gs232

// ---- transports and links ---------------------------------------------------------------------------------------------------------------

// A byte stream with a delimiter: a TCP socket to rotctld, a serial port, or a stand-in in a test.
class Transport {
public:
    virtual ~Transport() = default;
    virtual bool write(std::string_view bytes) = 0;
    // Reads up to and including `delim` (which is not stored); false on timeout, close, error or a line longer than the transport's cap.
    virtual bool readUntil(char delim, std::string* out, int timeoutMs) = 0;
};

class RotatorLink {
public:
    virtual ~RotatorLink() = default;
    virtual bool setPosition(double az, double el) = 0;
    virtual bool getPosition(Position* out) = 0;
    virtual bool stop() = 0;
    virtual bool park() = 0;
    virtual std::string lastError() const { return err_; }

protected:
    bool fail(const std::string& e) { err_ = e; return false; }
    std::string err_;
};

class RotctldLink : public RotatorLink {
public:
    explicit RotctldLink(Transport& t, int timeoutMs = 1000) : t_(t), timeoutMs_(timeoutMs) {}
    bool setPosition(double az, double el) override {
        if (!std::isfinite(az) || !std::isfinite(el)) { return fail("bad position"); }
        return simple(rotctld::setPos(az, el));
    }
    bool stop() override { return simple(rotctld::stop()); }
    bool park() override { return simple(rotctld::park()); }
    bool getPosition(Position* out) override {
        if (!t_.write(rotctld::getPos())) { return fail("write failed"); }
        std::string l1, l2;
        if (!t_.readUntil('\n', &l1, timeoutMs_)) { return fail("no answer"); }
        int code = 0;
        if (rotctld::parseRprt(l1, &code)) { return fail("rotctld error " + std::to_string(code)); }
        if (!t_.readUntil('\n', &l2, timeoutMs_)) { return fail("no elevation line"); }
        Position p;
        if (!detail::parseNumber(l1, &p.az) || !detail::parseNumber(l2, &p.el)) { return fail("malformed position"); }
        *out = p;
        return true;
    }

private:
    bool simple(const std::string& cmd) {
        if (!t_.write(cmd)) { return fail("write failed"); }
        std::string line;
        if (!t_.readUntil('\n', &line, timeoutMs_)) { return fail("no answer"); }
        int code = 0;
        if (!rotctld::parseRprt(line, &code)) { return fail("malformed answer"); }
        if (code != 0) { return fail("rotctld error " + std::to_string(code)); }
        return true;
    }
    Transport& t_;
    int timeoutMs_;
};

struct Gs232Config {
    int azMaxDeg = 450;       // 360 or 450 by controller type
    double parkAz = 0.0, parkEl = 0.0;
    bool azimuthOnly = false; // an azimuth-only rotator: M instead of W
    int timeoutMs = 1000;
};

class Gs232Link : public RotatorLink {
public:
    Gs232Link(Transport& t, const Gs232Config& c = Gs232Config()) : t_(t), c_(c) {}
    bool setPosition(double az, double el) override {
        const std::string cmd = c_.azimuthOnly ? gs232::setAz(az, c_.azMaxDeg) : gs232::setPos(az, el, c_.azMaxDeg);
        if (cmd.empty()) { return fail("bad position"); }
        return expectAck(cmd);
    }
    bool stop() override { return expectAck(gs232::stop()); }
    bool park() override { return setPosition(c_.parkAz, c_.parkEl); }  // the GS-232 has no park command
    bool getPosition(Position* out) override {
        if (!t_.write(c_.azimuthOnly ? gs232::readAz() : gs232::readAzEl())) { return fail("write failed"); }
        std::string r;
        if (!readData(&r)) { return fail("no answer"); }
        if (gs232::isErrorReply(r)) { return fail("controller refused the command"); }
        Position p;
        if (c_.azimuthOnly) {
            double a = 0;
            if (!gs232::parseAngle(r, &a)) { return fail("malformed position"); }
            p.az = a;
            p.el = 0;
        } else if (!gs232::parseAzEl(r, &p)) {
            return fail("malformed position");
        }
        *out = p;
        return true;
    }

private:
    // A command that returns no data is answered by a bare CR; "?>" means refused.
    bool expectAck(const std::string& cmd) {
        if (!t_.write(cmd)) { return fail("write failed"); }
        std::string r;
        if (!t_.readUntil('\r', &r, c_.timeoutMs)) { return fail("no answer"); }
        if (gs232::isErrorReply(r)) { return fail("controller refused the command"); }
        return true;
    }
    // Data answers end CR LF; a leading empty line (a stray CR or LF) is skipped once.
    bool readData(std::string* r) {
        for (int i = 0; i < 2; ++i) {
            if (!t_.readUntil('\r', r, c_.timeoutMs)) { return false; }
            while (!r->empty() && (r->front() == '\n' || r->front() == ' ')) { r->erase(0, 1); }
            if (!r->empty()) { return true; }
        }
        return false;
    }
    Transport& t_;
    Gs232Config c_;
};

// rotctld over TCP (plain, no authentication: rotctld has none). Blocking with timeouts; line cap 512 bytes.
class TcpTransport : public Transport {
public:
    ~TcpTransport() override { close(); }
    bool connect(const std::string& host, int port, int timeoutMs = 2000) {
        close();
        if (host.empty() || port < 1 || port > 65535 || host.find_first_of("\r\n ") != std::string::npos) { return false; }
        powerlog::InfluxTarget t;  // only its host / port / timeout are used by the shared dial helper
        t.enabled = true;
        t.host = host;
        t.port = port;
        t.timeoutMs = timeoutMs;
        std::string err;
        sock_ = powerlog::detail::dial(t, false, &err);
        open_ = !powerlog::detail::badSock(sock_);
        buf_.clear();
        return open_;
    }
    void close() {
        if (open_) { powerlog::detail::closeSock(sock_); }
        open_ = false;
        buf_.clear();
    }
    bool isOpen() const { return open_; }
    bool write(std::string_view b) override {
        if (!open_) { return false; }
        std::size_t off = 0;
        while (off < b.size()) {
            const int n = ::send(sock_, b.data() + off, static_cast<int>(b.size() - off), 0);
            if (n <= 0) { close(); return false; }
            off += static_cast<std::size_t>(n);
        }
        return true;
    }
    bool readUntil(char delim, std::string* out, int timeoutMs) override {
        if (!open_) { return false; }
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            const std::size_t p = buf_.find(delim);
            if (p != std::string::npos) {
                out->assign(buf_, 0, p);
                buf_.erase(0, p + 1);
                return true;
            }
            if (buf_.size() > 512) { close(); return false; }  // a line this long is not rotctld
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count();
            if (left <= 0) { return false; }
            fd_set r;
            FD_ZERO(&r);
            FD_SET(sock_, &r);
            timeval tv{static_cast<long>(left / 1000), static_cast<long>((left % 1000) * 1000)};
#if defined(_WIN32)
            const int s = ::select(0, &r, nullptr, nullptr, &tv);
#else
            const int s = ::select(sock_ + 1, &r, nullptr, nullptr, &tv);
#endif
            if (s <= 0) { if (s < 0) { close(); } return false; }
            char tmp[256];
            const int n = ::recv(sock_, tmp, sizeof tmp, 0);
            if (n <= 0) { close(); return false; }
            buf_.append(tmp, static_cast<std::size_t>(n));
        }
    }

private:
    powerlog::detail::sock_t sock_{};
    bool open_ = false;
    std::string buf_;
};

// ---- tracker ----------------------------------------------------------------------------------------------------------------------------

inline double angleDiff(double a, double b) {  // a - b folded into (-180, 180]
    double d = std::fmod(a - b, 360.0);
    if (d > 180.0) { d -= 360.0; }
    if (d <= -180.0) { d += 360.0; }
    return d;
}

// The azimuth the rotator can reach (within [azMin, azMax]) that is the same compass direction as theta and nearest to `from`.
// A 0..360 rotator has one choice (two at the ends, where 0 and 360 are the same direction); a 0..450 rotator has two for 0..90.
inline double nearestAzimuth(double theta, double from, double azMin, double azMax) {
    const double t = detail::wrap360(theta);
    double best = std::nan("");
    double bestD = 1e18;
    for (int k = -2; k <= 3; ++k) {
        const double c = t + 360.0 * k;
        if (c < azMin - 1e-9 || c > azMax + 1e-9) { continue; }
        const double d = std::fabs(c - from);
        if (d < bestD) { bestD = d; best = c; }
    }
    if (!std::isfinite(best)) { best = std::clamp(t, azMin, azMax); }  // a range narrower than 360: the nearest end
    return best;
}

struct TrackerConfig {
    double azMin = 0.0, azMax = 360.0;       // the rotator's azimuth range (360, 450 or more degrees wide)
    double elMin = 0.0, elMax = 90.0;        // 90 for a normal elevation rotator, 180 for one that can flip over the top
    double deadBandDeg = 2.0;                // no command until the target is this far from the last commanded position (azimuth or elevation)
    double minIntervalSec = 2.0;             // never two commands closer than this
    double minTargetElevationDeg = 0.0;      // a target below this is not tracked
    bool allowFlip = true;                   // used only when elMax >= 180
    double azSpeedDegPerSec = 6.0;           // the rotator's speeds (a G-5500 is about 6 deg/s in azimuth and 2.7 in elevation); they weigh the
    double elSpeedDegPerSec = 6.0;           //   cost of a representation: seconds of travel from where the last command put the rotator
    double flipHysteresisSec = 3.0;          // the other representation must be this many seconds cheaper before the tracker switches to it
    double parkAz = 0.0, parkEl = 0.0;
};

struct Command {
    enum class Kind { None, Move, Park, Stop } kind = Kind::None;
    double az = 0.0, el = 0.0;
    bool flipped = false;
    const char* why = "";  // for None: the reason; for Move: "move"
};

class Tracker {
public:
    explicit Tracker(const TrackerConfig& c = TrackerConfig()) : c_(c) {
        c_.elMax = std::clamp(c_.elMax, 1.0, 180.0);
        c_.elMin = std::clamp(c_.elMin, 0.0, c_.elMax);
        if (!(c_.azSpeedDegPerSec > 0.0)) { c_.azSpeedDegPerSec = 6.0; }
        if (!(c_.elSpeedDegPerSec > 0.0)) { c_.elSpeedDegPerSec = 6.0; }
        if (c_.azMax < c_.azMin) { std::swap(c_.azMin, c_.azMax); }
    }
    const TrackerConfig& config() const { return c_; }
    bool flipCapable() const { return c_.allowFlip && c_.elMax >= 179.0; }
    bool active() const { return active_; }
    bool flipped() const { return flipped_; }
    bool haveLast() const { return haveLast_; }

    // Starts (or restarts) a track.
    void begin() {
        active_ = true;
        haveLast_ = false;
        flipped_ = false;
        lastTime_ = -1e18;
    }
    // After a command that could not be delivered: the next update may command at once (the last position stays as it was).
    void retryNow() { lastTime_ = -1e18; haveLast_ = haveBefore_; lastAz_ = beforeAz_; lastEl_ = beforeEl_; flipped_ = beforeFlip_; }

    // One target sample at monotonic time nowSec. `measured` (optional) is where the rotator actually is: it is the reference for the first command.
    Command update(double nowSec, double targetAz, double targetEl, const Position* measured = nullptr) {
        Command none;
        if (!active_) { none.why = "not tracking"; return none; }
        if (!std::isfinite(nowSec) || !std::isfinite(targetAz) || !std::isfinite(targetEl)) { none.why = "bad input"; return none; }
        if (targetEl < c_.minTargetElevationDeg) { none.why = "below the elevation limit"; return none; }
        const double el = std::clamp(targetEl, 0.0, 90.0);
        // the two ways to point at the target
        const double refAz = haveLast_ ? lastAz_ : (measured != nullptr && measured->valid() ? measured->az : detail::wrap360(targetAz));
        const double refEl = haveLast_ ? lastEl_ : (measured != nullptr && measured->valid() ? measured->el : el);
        Cand normal{nearestAzimuth(targetAz, refAz, c_.azMin, c_.azMax), std::clamp(el, c_.elMin, std::min(c_.elMax, 90.0))};
        Cand flip{nearestAzimuth(targetAz + 180.0, refAz, c_.azMin, c_.azMax), 180.0 - el};
        bool useFlip = flipped_;
        if (flipCapable() && flip.el >= c_.elMin && flip.el <= c_.elMax) {
            const double cn = std::fabs(normal.az - refAz) / c_.azSpeedDegPerSec + std::fabs(normal.el - refEl) / c_.elSpeedDegPerSec;
            const double cf = std::fabs(flip.az - refAz) / c_.azSpeedDegPerSec + std::fabs(flip.el - refEl) / c_.elSpeedDegPerSec;
            if (haveLast_ || (measured != nullptr && measured->valid())) {
                if (!flipped_ && cf + c_.flipHysteresisSec < cn) { useFlip = true; }
                else if (flipped_ && cn + c_.flipHysteresisSec < cf) { useFlip = false; }
            }
        } else {
            useFlip = false;
        }
        const Cand pick = useFlip ? flip : normal;
        if (haveLast_) {
            if (std::fabs(pick.az - lastAz_) < c_.deadBandDeg && std::fabs(pick.el - lastEl_) < c_.deadBandDeg && useFlip == flipped_) {
                none.why = "inside the dead band";
                return none;
            }
            if (nowSec - lastTime_ < c_.minIntervalSec) { none.why = "too soon after the last command"; return none; }
        }
        haveBefore_ = haveLast_; beforeAz_ = lastAz_; beforeEl_ = lastEl_; beforeFlip_ = flipped_;
        haveLast_ = true;
        lastAz_ = pick.az;
        lastEl_ = pick.el;
        lastTime_ = nowSec;
        flipped_ = useFlip;
        Command m;
        m.kind = Command::Kind::Move;
        m.az = pick.az;
        m.el = pick.el;
        m.flipped = useFlip;
        m.why = "move";
        return m;
    }

    // End of the track: park (once).
    Command finish() {
        Command c;
        if (!active_) { c.why = "not tracking"; return c; }
        active_ = false;
        c.kind = Command::Kind::Park;
        c.az = std::clamp(c_.parkAz, c_.azMin, c_.azMax);
        c.el = std::clamp(c_.parkEl, c_.elMin, c_.elMax);
        c.why = "park";
        return c;
    }
    // Abort: stop the rotator, no further commands until begin().
    Command abort() {
        Command c;
        if (!active_) { c.why = "not tracking"; return c; }
        active_ = false;
        c.kind = Command::Kind::Stop;
        c.why = "stop";
        return c;
    }

private:
    struct Cand { double az, el; };
    TrackerConfig c_;
    bool active_ = true;
    bool haveLast_ = false, flipped_ = false;
    bool haveBefore_ = false, beforeFlip_ = false;
    double lastAz_ = 0, lastEl_ = 0, lastTime_ = -1e18, beforeAz_ = 0, beforeEl_ = 0;
};

struct DriverStats {
    int moves = 0, parks = 0, stops = 0, failures = 0, suppressed = 0;
};

// Carries out what the tracker decides on a link; a failed command is counted and re-tried at the next update.
class TrackerDriver {
public:
    TrackerDriver(Tracker& t, RotatorLink& l) : t_(t), l_(l) {}
    Command step(double nowSec, double az, double el, const Position* measured = nullptr) { return run(t_.update(nowSec, az, el, measured)); }
    Command finish() { return run(t_.finish()); }
    Command abort() { return run(t_.abort()); }
    const DriverStats& stats() const { return s_; }

private:
    Command run(const Command& c) {
        bool ok = true;
        switch (c.kind) {
            case Command::Kind::None: ++s_.suppressed; return c;
            case Command::Kind::Move: ok = l_.setPosition(c.az, c.el); if (ok) { ++s_.moves; } else { t_.retryNow(); } break;
            case Command::Kind::Park: ok = l_.park(); if (ok) { ++s_.parks; } break;
            case Command::Kind::Stop: ok = l_.stop(); if (ok) { ++s_.stops; } break;
        }
        if (!ok) { ++s_.failures; }
        return c;
    }
    Tracker& t_;
    RotatorLink& l_;
    DriverStats s_;
};

}  // namespace cascade::core::rotator
