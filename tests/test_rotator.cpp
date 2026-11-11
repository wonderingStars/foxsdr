// Tests for rotator control (core/rotator.hpp): the rotctld and GS-232 command formatters and reply parsers, the links over stand-in transports (and
// a real loopback socket for the rotctld TCP transport), and the tracker, including a simulated overhead satellite pass against a rate-limited
// simulated rotator. NO REAL ROTATOR WAS AVAILABLE: nothing here has touched hardware.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/rotator.hpp"

#include "test_check.hpp"

#include <cstdio>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <functional>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

using namespace cascade::core::rotator;
namespace pld = cascade::core::powerlog::detail;

namespace {

constexpr double kPi = 3.14159265358979323846;

// ---- stand-in transports -------------------------------------------------------------------------------------------------------------------

// A transport whose peer is a function: every write is handed to it and what it returns is queued for reading.
class FakePeer : public Transport {
public:
    std::function<std::string(const std::string&)> handler;
    std::vector<std::string> sent;
    bool failWrites = false;
    bool write(std::string_view b) override {
        if (failWrites) { return false; }
        sent.emplace_back(b);
        if (handler) { rx_ += handler(std::string(b)); }
        return true;
    }
    bool readUntil(char delim, std::string* out, int) override {
        const std::size_t p = rx_.find(delim);
        if (p == std::string::npos) { return false; }  // nothing more is coming: a timeout
        out->assign(rx_, 0, p);
        rx_.erase(0, p + 1);
        return true;
    }
    void feed(const std::string& s) { rx_ += s; }
    std::string rx_;
};

// A rotctld stand-in: P / p / S / K on a position.
struct FakeRotctld {
    double az = 10, el = 20;
    int errorCode = 0;  // answer "RPRT n" to everything when non-zero
    std::string operator()(const std::string& cmd) {
        if (errorCode != 0) { return "RPRT " + std::to_string(errorCode) + "\n"; }
        if (cmd.rfind("P ", 0) == 0) {
            double a, e;
            if (std::sscanf(cmd.c_str(), "P %lf %lf", &a, &e) != 2 || a < 0 || a > 360 || e < 0 || e > 180) { return "RPRT -1\n"; }
            az = a;
            el = e;
            return "RPRT 0\n";
        }
        if (cmd == "p\n") { return cascade::core::rotator::detail::fixed(az, 6) + "\n" + cascade::core::rotator::detail::fixed(el, 6) + "\n"; }
        if (cmd == "S\n") { return "RPRT 0\n"; }
        if (cmd == "K\n") { az = 0; el = 0; return "RPRT 0\n"; }
        return "RPRT -11\n";
    }
};

// A GS-232 stand-in.
struct FakeGs232 {
    int az = 90, el = 45;
    bool variantB = false;
    int azMax = 450;
    std::string operator()(const std::string& cmd) {
        if (cmd.empty() || cmd.back() != '\r') { return ""; }
        if (cmd.size() == 9 && (cmd[0] == 'W' || cmd[0] == 'w') && cmd[4] == ' ') {
            const int a = std::atoi(cmd.substr(1, 3).c_str()), e = std::atoi(cmd.substr(5, 3).c_str());
            if (a > azMax || e > 180) { return "?>\r"; }
            az = a;
            el = e;
            return "\r";
        }
        if (cmd.size() == 5 && cmd[0] == 'M') {
            const int a = std::atoi(cmd.substr(1, 3).c_str());
            if (a > azMax) { return "?>\r"; }
            az = a;
            return "\r";
        }
        if (cmd == "S\r") { return "\r"; }
        char b[40];
        if (cmd == "C2\r") {
            if (variantB) { std::snprintf(b, sizeof b, "AZ=%03d EL=%03d\r\n", az, el); }
            else { std::snprintf(b, sizeof b, "+0%03d+0%03d\r\n", az, el); }
            return b;
        }
        if (cmd == "C\r") { std::snprintf(b, sizeof b, "+0%03d\r\n", az); return b; }
        return "?>\r";
    }
};

// ---- a simulated rotator -------------------------------------------------------------------------------------------------------------------

class SimRotator : public RotatorLink {
public:
    double az = 0, el = 0, tAz = 0, tEl = 0;
    double azMin = 0, azMax = 360, elMax = 90, rate = 6.0, elRate = 2.7;
    int commands = 0, refused = 0;
    std::vector<double> commandTimes;
    double now = 0;
    bool setPosition(double a, double e) override {
        if (a < azMin - 1e-9 || a > azMax + 1e-9 || e < -1e-9 || e > elMax + 1e-9) { ++refused; return fail("out of range"); }
        tAz = a;
        tEl = e;
        ++commands;
        commandTimes.push_back(now);
        return true;
    }
    bool getPosition(Position* p) override { p->az = az; p->el = el; return true; }
    bool stop() override { tAz = az; tEl = el; return true; }
    bool park() override { return setPosition(parkAz, parkEl); }
    double parkAz = 0, parkEl = 0;
    void advance(double dt) {
        now += dt;
        const double da = std::clamp(tAz - az, -rate * dt, rate * dt), de = std::clamp(tEl - el, -elRate * dt, elRate * dt);
        az += da;
        el += de;
    }
};

// Pointing direction as a unit vector (valid for a flipped pair too).
void dir(double az, double el, double v[3]) {
    const double a = az * kPi / 180, e = el * kPi / 180;
    v[0] = std::cos(e) * std::sin(a);
    v[1] = std::cos(e) * std::cos(a);
    v[2] = std::sin(e);
}
double separationDeg(double az1, double el1, double az2, double el2) {
    double a[3], b[3];
    dir(az1, el1, a);
    dir(az2, el2, b);
    const double d = std::clamp(a[0] * b[0] + a[1] * b[1] + a[2] * b[2], -1.0, 1.0);
    return std::acos(d) * 180 / kPi;
}

// A satellite pass: a circular orbit of altitude 800 km seen from an observer; the ground track heads toward `headingDeg` (compass) and passes
// `crossTrackDeg` of Earth-central angle to the side of the observer (0 = exactly overhead). One sample per second.
struct PassSample { double t, az, el; };
std::vector<PassSample> makePass(double crossTrackDeg, double headingDeg) {
    const double Re = 6371.0, R = Re + 800.0;
    const double omega = std::sqrt(398600.4418 / (R * R * R));  // rad/s
    // observer at +x (up); local axes: y east, z north. Orbit plane spanned by u (to the closest-approach point) and w (the direction of travel).
    const double h = headingDeg * kPi / 180.0;
    const double wy = std::sin(h), wz = std::cos(h);          // travel direction (east, north components)
    const double py = -wz, pz = wy;                           // perpendicular, horizontal
    const double d = crossTrackDeg * kPi / 180.0;
    const double ux = std::cos(d), uy = std::sin(d) * py, uz = std::sin(d) * pz;
    std::vector<PassSample> out;
    for (int t = -900; t <= 900; ++t) {
        const double phi = omega * t;
        const double sx = R * (std::cos(phi) * ux), sy = R * (std::cos(phi) * uy + std::sin(phi) * wy), sz = R * (std::cos(phi) * uz + std::sin(phi) * wz);
        const double rx = sx - Re, ry = sy, rz = sz;
        const double E = ry, N = rz, U = rx;
        const double el = std::atan2(U, std::hypot(E, N)) * 180 / kPi;
        if (el < 0) { continue; }
        double az = std::atan2(E, N) * 180 / kPi;
        if (az < 0) { az += 360; }
        out.push_back({static_cast<double>(t), az, el});
    }
    return out;
}

struct PassResult {
    double maxErr = 0, meanErr = 0, maxEl = 0;
    int commands = 0, refused = 0, flips = 0;
    double minGap = 1e9;
};
PassResult flyPass(const std::vector<PassSample>& pass, const TrackerConfig& cfg, double azStart, bool printTrace = false) {
    Tracker trk(cfg);
    SimRotator rot;
    rot.azMin = cfg.azMin;
    rot.azMax = cfg.azMax;
    rot.elMax = cfg.elMax;
    rot.az = rot.tAz = azStart;
    TrackerDriver drv(trk, rot);
    PassResult r;
    double sum = 0;
    int n = 0;
    bool lastFlip = false;
    for (const PassSample& s : pass) {
        // the rotator moves for one second (in 0.1 s steps), then the tracker sees the next target sample
        Position m;
        rot.getPosition(&m);
        const Command c = drv.step(s.t, s.az, s.el, &m);
        if (c.kind == Command::Kind::Move && c.flipped != lastFlip) { ++r.flips; lastFlip = c.flipped; }
        for (int k = 0; k < 10; ++k) { rot.advance(0.1); }
        const double err = separationDeg(rot.az, rot.el, s.az, s.el);
        r.maxErr = std::max(r.maxErr, err);
        sum += err;
        ++n;
        r.maxEl = std::max(r.maxEl, s.el);
        if (printTrace && (n % 60) == 0) { std::printf("   t=%5.0f target az %6.1f el %5.1f  rotator az %6.1f el %5.1f  error %5.1f\n", s.t, s.az, s.el, rot.az, rot.el, err); }
    }
    r.meanErr = n ? sum / n : 0;
    r.commands = rot.commands;
    r.refused = rot.refused;
    for (std::size_t i = 1; i < rot.commandTimes.size(); ++i) { r.minGap = std::min(r.minGap, rot.commandTimes[i] - rot.commandTimes[i - 1]); }
    return r;
}

// ---- tests ----------------------------------------------------------------------------------------------------------------------------------

void testRotctldCodec() {
    CHECK(rotctld::setPos(123.456, 7.0) == "P 123.46 7.00\n");
    CHECK(rotctld::setPos(0, 0) == "P 0.00 0.00\n" && rotctld::setPos(359.999, 89.994) == "P 360.00 89.99\n");
    CHECK(rotctld::setPos(-1.5, 0.004) == "P -1.50 0.00\n");
    CHECK(rotctld::getPos() == "p\n" && rotctld::stop() == "S\n" && rotctld::park() == "K\n");
    int code = 99;
    CHECK(rotctld::parseRprt("RPRT 0", &code) && code == 0);
    CHECK(rotctld::parseRprt("RPRT -1\r", &code) && code == -1);
    CHECK(rotctld::parseRprt("RPRT -11 ", &code) && code == -11);
    for (const char* bad : {"", "RPRT", "RPRT ", "RPRT x", "RPRT 0 0", "rprt 0", "ok", "12.5", "RPRT 99999999999"}) { CHECK(!rotctld::parseRprt(bad, &code)); }
}

void testRotctldLink() {
    FakePeer peer;
    FakeRotctld daemon;
    peer.handler = [&](const std::string& c) { return daemon(c); };
    RotctldLink link(peer, 50);
    CHECK(link.setPosition(135.0, 10.0));
    CHECK(peer.sent.back() == "P 135.00 10.00\n" && daemon.az == 135 && daemon.el == 10);
    Position p;
    CHECK(link.getPosition(&p) && p.az == 135 && p.el == 10);
    CHECK(link.stop() && peer.sent.back() == "S\n");
    CHECK(link.park() && peer.sent.back() == "K\n" && daemon.az == 0);
    // the daemon refuses (RPRT -1) or reports another error
    CHECK(!link.setPosition(400.0, 10.0) && link.lastError() == "rotctld error -1");
    daemon.errorCode = -11;
    CHECK(!link.stop() && link.lastError() == "rotctld error -11");
    CHECK(!link.getPosition(&p) && link.lastError() == "rotctld error -11");
    daemon.errorCode = 0;
    // refusals before sending
    const std::size_t before = peer.sent.size();
    CHECK(!link.setPosition(std::nan(""), 1.0) && !link.setPosition(1.0, INFINITY) && peer.sent.size() == before);
    // silence, a closed channel, garbage, a half position
    peer.handler = nullptr;
    peer.rx_.clear();
    CHECK(!link.setPosition(1, 2) && link.lastError() == "no answer");
    CHECK(!link.getPosition(&p) && link.lastError() == "no answer");
    peer.failWrites = true;
    CHECK(!link.stop() && link.lastError() == "write failed");
    peer.failWrites = false;
    peer.feed("hello\n");
    CHECK(!link.stop() && link.lastError() == "malformed answer");
    peer.rx_.clear();
    peer.feed("12.5\n");  // an azimuth and then nothing
    CHECK(!link.getPosition(&p) && link.lastError() == "no elevation line");
    for (const char* bad : {"abc\n5\n", "1.0\nxyz\n", "nan\n5\n", "5\ninf\n", "1e999\n2\n", "\n\n", "1 2\n3\n"}) {
        peer.rx_.clear();
        peer.feed(bad);
        CHECK(!link.getPosition(&p) && link.lastError() == "malformed position");
    }
    // a position with CRLF and a plus sign is accepted
    peer.rx_.clear();
    peer.feed("+12.50\r\n 3.25 \r\n");
    CHECK(link.getPosition(&p) && p.az == 12.5 && p.el == 3.25);
}

// A real socket: a rotctld stand-in on 127.0.0.1.
class FakeRotctldServer {
public:
    ~FakeRotctldServer() { stop(); }
    bool start(const std::function<std::string(const std::string&)>& h) {
        handler_ = h;
        pld::ensureNet();
        listen_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (pld::badSock(listen_)) { return false; }
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listen_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { return false; }
#if defined(_WIN32)
        int len = sizeof a;
#else
        socklen_t len = sizeof a;
#endif
        ::getsockname(listen_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        ::listen(listen_, 2);
        run_ = true;
        th_ = std::thread([this] { loop(); });
        return true;
    }
    void stop() {
        run_ = false;
        if (th_.joinable()) { th_.join(); }
        if (!pld::badSock(listen_)) { pld::closeSock(listen_); listen_ = static_cast<pld::sock_t>(-1); }
    }
    int port() const { return port_; }
    std::string rawReply;       // if set: sent for every line instead of calling the handler
    bool closeAfterFirst = false;
    int lines() const { return lines_.load(); }

private:
    void loop() {
        while (run_) {
            fd_set r;
            FD_ZERO(&r);
            FD_SET(listen_, &r);
            timeval tv{0, 50000};
#if defined(_WIN32)
            if (::select(0, &r, nullptr, nullptr, &tv) <= 0) { continue; }
#else
            if (::select(listen_ + 1, &r, nullptr, nullptr, &tv) <= 0) { continue; }
#endif
            pld::sock_t c = ::accept(listen_, nullptr, nullptr);
            if (pld::badSock(c)) { continue; }
            std::string in;
            while (run_) {
                fd_set rr;
                FD_ZERO(&rr);
                FD_SET(c, &rr);
                timeval t2{0, 50000};
#if defined(_WIN32)
                if (::select(0, &rr, nullptr, nullptr, &t2) <= 0) { continue; }
#else
                if (::select(c + 1, &rr, nullptr, nullptr, &t2) <= 0) { continue; }
#endif
                char buf[512];
                const int n = ::recv(c, buf, sizeof buf, 0);
                if (n <= 0) { break; }
                in.append(buf, static_cast<std::size_t>(n));
                std::size_t p;
                while ((p = in.find('\n')) != std::string::npos) {
                    const std::string line = in.substr(0, p + 1);
                    in.erase(0, p + 1);
                    lines_.fetch_add(1);
                    const std::string reply = rawReply.empty() ? handler_(line) : rawReply;
                    ::send(c, reply.data(), static_cast<int>(reply.size()), 0);
                    if (closeAfterFirst) { goto done; }
                }
            }
        done:
            pld::closeSock(c);
        }
    }
    std::function<std::string(const std::string&)> handler_;
    pld::sock_t listen_ = static_cast<pld::sock_t>(-1);
    int port_ = 0;
    std::atomic<bool> run_{false};
    std::atomic<int> lines_{0};
    std::thread th_;
};

void testTcpTransport() {
    FakeRotctld daemon;
    FakeRotctldServer srv;
    CHECK(srv.start([&](const std::string& c) { return daemon(c); }));
    TcpTransport tcp;
    CHECK(tcp.connect("127.0.0.1", srv.port(), 1000));
    RotctldLink link(tcp, 1000);
    CHECK(link.setPosition(200.0, 33.5));
    Position p;
    CHECK(link.getPosition(&p) && p.az == 200 && p.el == 33.5);
    CHECK(link.park() && link.getPosition(&p) && p.az == 0 && p.el == 0);
    CHECK(srv.lines() == 4);
    tcp.close();
    CHECK(!tcp.isOpen() && !tcp.write("p\n"));
    // refused port and bad host names
    TcpTransport t2;
    CHECK(!t2.connect("127.0.0.1", 1, 300) && !t2.isOpen());
    CHECK(!t2.connect("", 4533) && !t2.connect("bad host", 4533) && !t2.connect("127.0.0.1", 0) && !t2.connect("127.0.0.1", 70000));
    // a daemon that closes after the first line: the second command fails, the link reports it, nothing hangs
    FakeRotctldServer srv2;
    srv2.closeAfterFirst = true;
    CHECK(srv2.start([&](const std::string& c) { return daemon(c); }));
    TcpTransport t3;
    CHECK(t3.connect("127.0.0.1", srv2.port(), 1000));
    RotctldLink l3(t3, 300);
    CHECK(l3.setPosition(1, 2));
    CHECK(!l3.setPosition(3, 4));
    // a daemon that never ends a line: the 512-byte cap closes the connection instead of growing a buffer
    FakeRotctldServer srv3;
    srv3.rawReply = std::string(2000, 'x');
    CHECK(srv3.start([&](const std::string&) { return std::string(); }));
    TcpTransport t4;
    CHECK(t4.connect("127.0.0.1", srv3.port(), 1000));
    RotctldLink l4(t4, 500);
    CHECK(!l4.stop());
    CHECK(!t4.isOpen());
    // a daemon that does not answer: the read times out at the link's timeout
    FakeRotctldServer srv4;
    CHECK(srv4.start([&](const std::string&) { return std::string(); }));
    TcpTransport t5;
    CHECK(t5.connect("127.0.0.1", srv4.port(), 1000));
    RotctldLink l5(t5, 200);
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(!l5.stop() && l5.lastError() == "no answer");
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ms > 150 && ms < 1500);
}

void testGs232Codec() {
    CHECK(gs232::setPos(123.4, 45.6) == "W123 046\r");
    CHECK(gs232::setPos(0, 0) == "W000 000\r" && gs232::setPos(450, 180) == "W450 180\r");
    CHECK(gs232::setPos(500, 200) == "W450 180\r" && gs232::setPos(-5, -1) == "W000 000\r");  // clamped to the manual's ranges
    CHECK(gs232::setPos(500, 20, 360) == "W360 020\r");                                      // a 360-degree controller
    CHECK(gs232::setPos(std::nan(""), 1).empty() && gs232::setPos(1, INFINITY).empty());
    CHECK(gs232::setAz(7.5) == "M008\r" && gs232::setAz(std::nan("")).empty());
    CHECK(gs232::stop() == "S\r" && gs232::readAzEl() == "C2\r" && gs232::readAz() == "C\r" && gs232::readEl() == "B\r");
    CHECK(gs232::speed(1) == "X1\r" && gs232::speed(4) == "X4\r" && gs232::speed(0).empty() && gs232::speed(5).empty());
    Position p;
    CHECK(gs232::parseAzEl("+0123+0045", &p) && p.az == 123 && p.el == 45);
    CHECK(gs232::parseAzEl("+0000+0180\r\n", &p) && p.az == 0 && p.el == 180);
    CHECK(gs232::parseAzEl("+0450+0090", &p) && p.az == 450);
    CHECK(gs232::parseAzEl("AZ=123 EL=045", &p) && p.az == 123 && p.el == 45);
    CHECK(gs232::parseAzEl("AZ=360EL=000", &p) && p.az == 360);
    for (const char* bad : {"", "+0123", "+0123+0045x", "++", "?>", "+0999+0999", "+0123+0181", "AZ=1", "AZ=123 EL=", "+01 23+0045", "0123+0045", "+0123 +0045", "+012345+0045"}) {
        CHECK(!gs232::parseAzEl(bad, &p));
    }
    double a = 0;
    CHECK(gs232::parseAngle("+0123", &a) && a == 123 && gs232::parseAngle("+0000\r\n", &a) && a == 0);
    CHECK(!gs232::parseAngle("0123", &a) && !gs232::parseAngle("+0123x", &a) && !gs232::parseAngle("+0999", &a) && !gs232::parseAngle("", &a));
    CHECK(gs232::isErrorReply("?>") && !gs232::isErrorReply("+0123+0045"));
}

void testGs232Link() {
    FakePeer peer;
    FakeGs232 dev;
    peer.handler = [&](const std::string& c) { return dev(c); };
    Gs232Config cfg;
    cfg.timeoutMs = 50;
    cfg.parkAz = 180;
    cfg.parkEl = 5;
    Gs232Link link(peer, cfg);
    CHECK(link.setPosition(123.4, 45.6) && peer.sent.back() == "W123 046\r" && dev.az == 123 && dev.el == 46);
    Position p;
    CHECK(link.getPosition(&p) && p.az == 123 && p.el == 46);
    CHECK(link.stop() && peer.sent.back() == "S\r");
    CHECK(link.park() && peer.sent.back() == "W180 005\r" && dev.az == 180);  // no park command: a move to the park position
    dev.variantB = true;
    CHECK(link.getPosition(&p) && p.az == 180 && p.el == 5);                  // the B form of the answer
    // the controller refuses (an angle beyond its range): "?>"
    dev.azMax = 360;
    CHECK(!link.setPosition(400, 10) && link.lastError() == "controller refused the command");
    // a leading stray LF before the data
    peer.handler = nullptr;
    peer.rx_.clear();
    peer.feed("\n+0010+0020\r\n");
    CHECK(link.getPosition(&p) && p.az == 10 && p.el == 20);
    // silence, a refused data command, garbage
    peer.rx_.clear();
    CHECK(!link.getPosition(&p) && link.lastError() == "no answer");
    peer.feed("?>\r");
    CHECK(!link.getPosition(&p) && link.lastError() == "controller refused the command");
    peer.rx_.clear();
    peer.feed("hello\r");
    CHECK(!link.getPosition(&p) && link.lastError() == "malformed position");
    CHECK(!link.setPosition(std::nan(""), 1) && link.lastError() == "bad position");
    // azimuth-only rotator: M and C
    Gs232Config az;
    az.azimuthOnly = true;
    az.timeoutMs = 50;
    FakePeer p2;
    p2.handler = [&](const std::string& c) { return dev(c); };
    dev.azMax = 450;
    dev.variantB = false;
    Gs232Link l2(p2, az);
    CHECK(l2.setPosition(200.2, 99) && p2.sent.back() == "M200\r" && dev.az == 200);
    CHECK(l2.getPosition(&p) && p.az == 200 && p.el == 0 && p2.sent.back() == "C\r");
}

void testTrackerRules() {
    TrackerConfig c;
    c.deadBandDeg = 2.0;
    c.minIntervalSec = 2.0;
    Tracker t(c);
    Command m = t.update(0, 100, 30);
    CHECK(m.kind == Command::Kind::Move && m.az == 100 && m.el == 30 && !m.flipped);
    // inside the dead band: nothing
    m = t.update(10, 101, 31);
    CHECK(m.kind == Command::Kind::None && std::string(m.why) == "inside the dead band");
    m = t.update(10, 101.9, 30);
    CHECK(m.kind == Command::Kind::None);
    // outside the dead band but too soon
    m = t.update(1.0, 105, 30);
    CHECK(m.kind == Command::Kind::None && std::string(m.why) == "too soon after the last command");
    // outside and in time: a move, and the interval restarts from it
    m = t.update(2.0, 105, 30);
    CHECK(m.kind == Command::Kind::Move && m.az == 105);
    CHECK(t.update(3.0, 110, 30).kind == Command::Kind::None);
    CHECK(t.update(4.0, 110, 30).kind == Command::Kind::Move);
    // elevation alone also leaves the dead band
    CHECK(t.update(10.0, 110, 36).kind == Command::Kind::Move);
    // a target below the limit is not tracked and does not disturb the state
    m = t.update(20.0, 200, -3);
    CHECK(m.kind == Command::Kind::None && std::string(m.why) == "below the elevation limit");
    CHECK(t.update(21.0, 111, 36).kind == Command::Kind::None);  // still inside the dead band of the last command (110, 36)
    // a raised limit
    TrackerConfig hi = c;
    hi.minTargetElevationDeg = 10.0;
    Tracker th(hi);
    CHECK(th.update(0, 10, 9.9).kind == Command::Kind::None && th.update(1, 10, 10.0).kind == Command::Kind::Move);
    // bad input
    Tracker tb(c);
    CHECK(tb.update(0, std::nan(""), 10).kind == Command::Kind::None && tb.update(0, 10, INFINITY).kind == Command::Kind::None &&
          tb.update(std::nan(""), 10, 10).kind == Command::Kind::None);
    CHECK(!tb.haveLast());
    // elevation above 90 from a sloppy source is clamped for a normal rotator
    Tracker tc(c);
    m = tc.update(0, 10, 95);
    CHECK(m.kind == Command::Kind::Move && m.el == 90);
    // the end of the track parks, once; then nothing until begin()
    TrackerConfig pk = c;
    pk.parkAz = 270;
    pk.parkEl = 15;
    Tracker tp(pk);
    tp.update(0, 10, 10);
    m = tp.finish();
    CHECK(m.kind == Command::Kind::Park && m.az == 270 && m.el == 15);
    CHECK(tp.finish().kind == Command::Kind::None && tp.update(100, 50, 50).kind == Command::Kind::None && !tp.active());
    tp.begin();
    CHECK(tp.update(200, 50, 50).kind == Command::Kind::Move && tp.finish().kind == Command::Kind::Park);
    // park position outside the limits is clamped
    pk.parkAz = 500;
    pk.parkEl = -9;
    Tracker tq(pk);
    m = tq.finish();
    CHECK(m.az == 360 && m.el == 0);
    // abort stops once
    Tracker ta(c);
    ta.update(0, 10, 10);
    CHECK(ta.abort().kind == Command::Kind::Stop && ta.abort().kind == Command::Kind::None && ta.update(5, 80, 20).kind == Command::Kind::None);
}

void testShortestWay() {
    // nearestAzimuth: single choice on 0..360, two on 0..450
    CHECK(nearestAzimuth(10, 350, 0, 360) == 10);       // a 360 rotator cannot pass north: it must go back
    CHECK(nearestAzimuth(0, 359, 0, 360) == 0 || nearestAzimuth(0, 359, 0, 360) == 360);
    CHECK(nearestAzimuth(0, 359, 0, 360) == 360);       // 0 and 360 are the same direction: the nearer
    CHECK(nearestAzimuth(0, 1, 0, 360) == 0);
    CHECK(nearestAzimuth(10, 350, 0, 450) == 370);      // 450: continue through north
    CHECK(nearestAzimuth(10, 20, 0, 450) == 10);
    CHECK(nearestAzimuth(370, 20, 0, 450) == 10);       // an input beyond 360 is the same direction
    CHECK(nearestAzimuth(-10, 5, 0, 450) == 350 && nearestAzimuth(-10, 355, 0, 450) == 350);
    CHECK(nearestAzimuth(100, 400, 0, 450) == 100);     // 460 would be out of range
    CHECK(nearestAzimuth(90, 400, 0, 450) == 450);      // 450 is the last reachable
    CHECK(nearestAzimuth(95, 440, 0, 450) == 95);       // 455 is not reachable
    CHECK(nearestAzimuth(200, 10, -90, 270) == 200 && nearestAzimuth(300, 10, -90, 270) == -60);  // a south-centred rotator
    CHECK(nearestAzimuth(45, 10, 100, 200) == 100);     // a range narrower than 360: the nearest end
    // angleDiff
    CHECK(angleDiff(10, 350) == 20 && angleDiff(350, 10) == -20 && angleDiff(180, 0) == 180 && angleDiff(0, 180) == 180);
    // tracking across north on a 450 rotator never unwinds; on a 360 rotator it must
    for (int range : {360, 450}) {
        TrackerConfig c;
        c.azMax = range;
        c.deadBandDeg = 0.5;
        c.minIntervalSec = 0.0;
        Tracker t(c);
        Position at{350, 10};
        Command m = t.update(0, 352, 10, &at);
        CHECK(m.az == 352);
        double maxJump = 0, last = m.az;
        for (int i = 1; i <= 40; ++i) {
            m = t.update(i, 352 + i, 10);  // 353 ... 392: the compass wraps at 360
            if (m.kind == Command::Kind::Move) { maxJump = std::max(maxJump, std::fabs(m.az - last)); last = m.az; }
        }
        std::printf("rotator range 0..%d, target crossing north from 352 to 32 degrees: largest single move %.1f degrees, ends at %.1f\n", range, maxJump, last);
        if (range == 450) { CHECK(maxJump <= 1.0 + 1e-9 && last == 392); }
        else { CHECK(maxJump > 300); }  // it has to swing back the long way (and it says so)
    }
    // the first command takes the SHORT way from where the rotator is (measured), not from the target
    TrackerConfig c450;
    c450.azMax = 450;
    Tracker t(c450);
    Position at{400, 5};
    CHECK(t.update(0, 50, 5, &at).az == 410);  // 400 -> 410, not 400 -> 50
    Tracker u(c450);
    Position at2{20, 5};
    CHECK(u.update(0, 355, 5, &at2).az == 355 - 0);  // 355 is 335 away but 355-360 < 0 is out of range: the only choice
    Tracker w(c450);
    Position at3{5, 5};
    CHECK(w.update(0, 355, 5, &at3).az == 355);
}

void testElevationLimits() {
    TrackerConfig c;
    c.elMin = 5.0;      // the rotator cannot go below 5 degrees
    c.elMax = 85.0;     // or above 85
    c.minTargetElevationDeg = 0.0;
    Tracker t(c);
    Command m = t.update(0, 10, 2.0);  // above the horizon but below the rotator's limit: pointed as low as it can go
    CHECK(m.kind == Command::Kind::Move && m.el == 5.0);
    m = t.update(10, 40, 89.0);
    CHECK(m.kind == Command::Kind::Move && m.el == 85.0);
    // a 0..90 rotator never flips even when asked to
    TrackerConfig f = c;
    f.elMin = 0;
    f.elMax = 90;
    f.allowFlip = true;
    Tracker nf(f);
    CHECK(!nf.flipCapable());
    CHECK(Tracker(TrackerConfig{.elMax = 180}).flipCapable() && !Tracker(TrackerConfig{.elMax = 180, .allowFlip = false}).flipCapable());
}

// A start azimuth for the rotator before a pass: the unwrapped path of the satellite shifted by whole turns so it fits the rotator's range if it can
// (what an operator, or a scheduler, does when it pre-positions a 450-degree rotator).
double startFor(const std::vector<PassSample>& pass, double azMin, double azMax) {
    std::vector<double> u;
    double prev = pass.front().az, off = 0;
    for (const PassSample& s : pass) {
        const double d = angleDiff(s.az, prev);
        off += d;
        prev = s.az;
        u.push_back(pass.front().az + off);
    }
    const double lo = *std::min_element(u.begin(), u.end()), hi = *std::max_element(u.begin(), u.end());
    for (int k = -2; k <= 2; ++k) {
        if (lo + 360.0 * k >= azMin && hi + 360.0 * k <= azMax) { return u.front() + 360.0 * k; }
    }
    return nearestAzimuth(pass.front().az, 180.0, azMin, azMax);
}

PassResult flyConfig(const std::vector<PassSample>& pass, double azMax, double elMax, bool flip) {
    TrackerConfig c;
    c.azMax = azMax;
    c.elMax = elMax;
    c.allowFlip = flip;
    c.azSpeedDegPerSec = 6.0;
    c.elSpeedDegPerSec = 2.7;
    return flyPass(pass, c, startFor(pass, 0, azMax));
}

void testOverheadPass() {
    struct Row { const char* name; double cross; double heading; };
    const Row rows[] = {{"overhead (cross-track 0.0 deg)", 0.0, 90.0}, {"near overhead (0.2 deg)", 0.2, 90.0}, {"near overhead (0.5 deg)", 0.5, 90.0},
                        {"high pass (3 deg)", 3.0, 45.0}, {"medium pass (9 deg)", 9.0, 20.0}, {"low pass (17 deg)", 17.0, 135.0},
                        {"low pass through north (14 deg)", 14.0, 350.0}};
    std::printf("simulated passes (800 km orbit; rotator 6 deg/s azimuth, 2.7 deg/s elevation; dead band 2 deg, 2 s between commands)\n");
    std::printf("pointing error between rotator and satellite, max / mean in degrees (number of flips)\n");
    std::printf("%-33s | max el | 360/90: A       | 360/180 flip: B      | 450/90: C       | 450/180 flip: D      | commands A/B\n", "pass");
    for (const Row& r : rows) {
        const std::vector<PassSample> pass = makePass(r.cross, r.heading);
        CHECK(pass.size() > 300);
        const PassResult a = flyConfig(pass, 360, 90, false), b = flyConfig(pass, 360, 180, true), c = flyConfig(pass, 450, 90, false), d = flyConfig(pass, 450, 180, true);
        std::printf("%-33s | %5.1f  | %6.1f / %5.1f | %6.1f / %5.1f (%d) | %6.1f / %5.1f | %6.1f / %5.1f (%d) | %d / %d\n", r.name, a.maxEl, a.maxErr, a.meanErr, b.maxErr, b.meanErr,
                    b.flips, c.maxErr, c.meanErr, d.maxErr, d.meanErr, d.flips, a.commands, b.commands);
        for (const PassResult* x : {&a, &b, &c, &d}) {
            CHECK(x->refused == 0);                   // never a command outside the rotator's limits
            CHECK(x->minGap >= 2.0 - 1e-9);           // never two commands closer than the interval
        }
        if (r.cross <= 0.5) {
            CHECK(b.maxErr < 0.6 * a.maxErr || b.maxErr < 6.0);   // the flipping 360-degree rotator keeps a near-overhead satellite
            CHECK(b.flips >= 1);
        }
        if (r.cross > 8.0) { CHECK(b.flips == 0 && d.flips == 0); }  // a low pass never flips
    }
    // the first command of a pass is never flipped, and park at the end puts the rotator at the park position
    TrackerConfig flip;
    flip.azMax = 450;
    flip.elMax = 180;
    flip.parkAz = 180;
    flip.parkEl = 0;
    Tracker t(flip);
    SimRotator rot;
    rot.azMax = 450;
    rot.elMax = 180;
    rot.parkAz = flip.parkAz;
    rot.parkEl = flip.parkEl;
    TrackerDriver drv(t, rot);
    const std::vector<PassSample> pass = makePass(0.0, 90.0);
    Command first = drv.step(pass.front().t, pass.front().az, pass.front().el);
    CHECK(first.kind == Command::Kind::Move && !first.flipped);
    drv.finish();
    for (int i = 0; i < 400; ++i) { rot.advance(0.5); }
    CHECK(std::fabs(rot.az - 180) < 1e-6 && std::fabs(rot.el) < 1e-6 && drv.stats().parks == 1);
}

void testDriverFailures() {
    // a link that fails the first two commands: the tracker retries at the next update instead of waiting out the interval
    struct Flaky : RotatorLink {
        int calls = 0;
        std::vector<std::pair<double, double>> got;
        bool setPosition(double a, double e) override { ++calls; if (calls <= 2) { return fail("down"); } got.push_back({a, e}); return true; }
        bool getPosition(Position*) override { return false; }
        bool stop() override { return false; }
        bool park() override { return false; }
    } link;
    Tracker t;
    TrackerDriver d(t, link);
    d.step(0, 100, 20);
    CHECK(d.stats().failures == 1 && d.stats().moves == 0);
    d.step(0.1, 100, 20);  // well inside the 2 s interval, but the last command did not go through
    CHECK(d.stats().failures == 2);
    d.step(0.2, 100, 20);
    CHECK(d.stats().moves == 1 && link.got.size() == 1 && link.got[0].first == 100);
    d.step(0.3, 100.5, 20);  // now inside the dead band of what really went out
    CHECK(d.stats().moves == 1 && d.stats().suppressed >= 1);
    d.finish();  // park fails on this link: counted
    CHECK(d.stats().failures == 3 && d.stats().parks == 0);
    // a move that fails after an earlier one went through: the tracker goes back to where the rotator really is, so the same target is sent again
    {
        struct SecondFails : RotatorLink {
            int calls = 0;
            std::vector<double> azs;
            bool setPosition(double a, double) override { ++calls; if (calls == 2) { return fail("down"); } azs.push_back(a); return true; }
            bool getPosition(Position*) override { return false; }
            bool stop() override { return true; }
            bool park() override { return true; }
        } l;
        Tracker t3;
        TrackerDriver d3(t3, l);
        d3.step(0, 100, 20);
        d3.step(10, 150, 20);  // fails
        d3.step(10.1, 150, 20);  // must be sent again: the rotator is still at 100
        CHECK(l.azs.size() == 2 && l.azs[0] == 100 && l.azs[1] == 150 && d3.stats().failures == 1 && d3.stats().moves == 2);
    }
    // a link that recovers: the stop reaches it
    Tracker t2;
    SimRotator rot;
    TrackerDriver d2(t2, rot);
    d2.step(0, 10, 10);
    d2.abort();
    CHECK(d2.stats().stops == 1 && d2.stats().moves == 1);
}

}  // namespace

int main() {
    testRotctldCodec();
    testRotctldLink();
    testTcpTransport();
    testGs232Codec();
    testGs232Link();
    testTrackerRules();
    testShortestWay();
    testElevationLimits();
    testOverheadPass();
    testDriverFailures();
    return testSummary("test_rotator");
}
