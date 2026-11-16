// s4b_rotator.cpp - see s4b_rotator.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/s4b_rotator.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

#include "core/serial_port.hpp"
#include "source/serial_port_win.hpp"

namespace cascade::engine::s4b {

namespace rot = cascade::core::rotator;

namespace {

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// A GS-232 controller on a serial port: lines ended by a CR, read in the port's own bounded turns.
class SerialTransport final : public rot::Transport {
public:
    bool open(const std::string& name, int baud, std::string& error) { return port_.open(name, baud, error, true); }
    bool write(std::string_view bytes) override {
        const int n = port_.write(bytes.data(), bytes.size());
        return n == static_cast<int>(bytes.size());
    }
    bool readUntil(char delim, std::string* out, int timeoutMs) override {
        const std::int64_t end = nowMs() + timeoutMs;
        for (;;) {
            const std::size_t p = buf_.find(delim);
            if (p != std::string::npos) {
                out->assign(buf_, 0, p);
                buf_.erase(0, p + 1);
                return true;
            }
            if (buf_.size() > 512) { return false; }   // a line this long is not a rotator
            if (nowMs() >= end) { return false; }
            char tmp[128];
            const int n = port_.read(tmp, sizeof tmp);
            if (n < 0) { return false; }
            if (n > 0) { buf_.append(tmp, static_cast<std::size_t>(n)); }
        }
    }

private:
    cascade::core::SerialPort port_;
    std::string buf_;
};

}  // namespace

std::unique_ptr<rot::Transport> openRealRotatorTransport(const RotatorConfig& c, std::string& error) {
    error.clear();
    if (!c.serial) {
        auto t = std::make_unique<rot::TcpTransport>();
        if (!t->connect(c.host, c.port, 2000)) {
            error = "cannot reach the rotator controller (rotctld) at that address and port";
            return nullptr;
        }
        return t;
    }
    if (!cascade::source::tinysa::isValidComPortName(c.comPort)) {
        error = "the serial port is a name like COM3";   // a typed path or a pipe is never opened
        return nullptr;
    }
    auto t = std::make_unique<SerialTransport>();
    std::string why;
    if (!t->open(c.comPort, c.baud, why)) {
        error = "cannot open the serial port of the rotator";
        return nullptr;
    }
    return t;
}

std::string RotatorControl::checkBearing(double az, double el, int azMaxDeg) {
    if (!std::isfinite(az) || !std::isfinite(el)) { return "the bearing is two numbers"; }
    if (azMaxDeg != 360 && azMaxDeg != 450) { azMaxDeg = 450; }
    if (az < 0.0 || az > static_cast<double>(azMaxDeg)) { return azMaxDeg == 360 ? "the azimuth is 0 to 360 degrees" : "the azimuth is 0 to 450 degrees"; }
    if (el < 0.0 || el > 180.0) { return "the elevation is 0 to 180 degrees"; }
    return std::string();
}

RotatorControl::~RotatorControl() { shutdown(); }

void RotatorControl::setFactory(RotatorTransportFactory f) {
    std::lock_guard<std::mutex> lk(m_);
    factory_ = std::move(f);
}

void RotatorControl::ensureWorker() {
    if (worker_.joinable()) { return; }
    stopWorker_ = false;
    worker_ = std::thread([this] { run(); });
}

void RotatorControl::configure(const RotatorConfig& c) {
    std::lock_guard<std::mutex> lk(m_);
    if (c == cfg_ && (c.enabled == status_.enabled)) { return; }
    cfg_ = c;
    status_.enabled = c.enabled;
    if (!c.enabled) { status_.problem.clear(); status_.connected = false; status_.haveMeasured = false; }
    dirty_ = true;
    if (c.enabled) { ensureWorker(); }
    cv_.notify_all();
}

RotatorResult RotatorControl::turn(double az, double el) {
    std::lock_guard<std::mutex> lk(m_);
    if (!cfg_.enabled) { return {false, "the rotator is off: turn it on first"}; }
    const std::string why = checkBearing(az, el, cfg_.azMaxDeg);
    if (!why.empty()) { return {false, why}; }
    pending_ = Request{Request::Turn, az, el};
    cv_.notify_all();
    return {};
}

RotatorResult RotatorControl::stopMotion() {
    std::lock_guard<std::mutex> lk(m_);
    if (!cfg_.enabled) { return {false, "the rotator is off: turn it on first"}; }
    pending_ = Request{Request::Stop, 0.0, 0.0};
    cv_.notify_all();
    return {};
}

RotatorResult RotatorControl::park() {
    std::lock_guard<std::mutex> lk(m_);
    if (!cfg_.enabled) { return {false, "the rotator is off: turn it on first"}; }
    pending_ = Request{Request::Park, 0.0, 0.0};
    cv_.notify_all();
    return {};
}

RotatorStatus RotatorControl::status() const {
    std::lock_guard<std::mutex> lk(m_);
    return status_;
}

void RotatorControl::shutdown() {
    {
        std::lock_guard<std::mutex> lk(m_);
        stopWorker_ = true;
        cfg_.enabled = false;
        status_.enabled = false;
        cv_.notify_all();
    }
    if (worker_.joinable()) { worker_.join(); }
}

void RotatorControl::run() {
    std::unique_ptr<rot::Transport> transport;
    std::unique_ptr<rot::RotatorLink> link;
    std::int64_t nextTry = 0, nextPoll = 0;
    RotatorConfig live;
    for (;;) {
        std::optional<Request> req;
        RotatorConfig cfg;
        RotatorTransportFactory factory;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait_for(lk, std::chrono::milliseconds(300), [&] { return stopWorker_ || dirty_ || pending_.has_value(); });
            if (stopWorker_) { break; }
            cfg = cfg_;
            factory = factory_ ? factory_ : RotatorTransportFactory(&openRealRotatorTransport);
            if (dirty_) { dirty_ = false; transport.reset(); link.reset(); nextTry = 0; nextPoll = 0; status_.connected = false; }
            req = pending_;
            pending_.reset();
        }
        if (!cfg.enabled) { transport.reset(); link.reset(); continue; }
        const std::int64_t now = nowMs();
        if (!link) {
            if (now < nextTry) {
                if (req) {
                    std::lock_guard<std::mutex> lk(m_);
                    ++status_.failures;
                    status_.problem = "the rotator is not connected";
                }
                continue;
            }
            std::string why;
            transport = factory(cfg, why);
            if (!transport) {
                std::lock_guard<std::mutex> lk(m_);
                status_.connected = false;
                status_.problem = why.empty() ? std::string("cannot reach the rotator") : why;
                if (req) { ++status_.failures; }
                nextTry = now + 5000;
                continue;
            }
            if (cfg.serial) {
                rot::Gs232Config gc;
                gc.azMaxDeg = cfg.azMaxDeg;
                gc.parkAz = cfg.parkAz;
                gc.parkEl = cfg.parkEl;
                link = std::make_unique<rot::Gs232Link>(*transport, gc);
            } else {
                link = std::make_unique<rot::RotctldLink>(*transport);
            }
            std::lock_guard<std::mutex> lk(m_);
            status_.connected = true;
            status_.problem.clear();
            nextPoll = 0;
        }
        bool ok = true;
        std::string err;
        if (req) {
            switch (req->kind) {
                case Request::Turn: ok = link->setPosition(req->az, req->el); break;
                case Request::Stop: ok = link->stop(); break;
                case Request::Park:
                    ok = link->park();   // rotctld has a park command; a GS-232 has none, and its link turns to the configured park bearing
                    break;
            }
            err = link->lastError();
            std::lock_guard<std::mutex> lk(m_);
            if (ok) {
                if (req->kind == Request::Turn) { ++status_.turns; } else if (req->kind == Request::Stop) { ++status_.stops; } else { ++status_.parks; }
                status_.problem.clear();
            } else {
                ++status_.failures;
                status_.problem = err.empty() ? std::string("the rotator did not answer") : "the rotator said: " + err;
            }
            nextPoll = nowMs() + 500;   // look where it is soon after a command
        } else if (now >= nextPoll) {
            rot::Position p;
            ok = link->getPosition(&p);
            err = link->lastError();
            std::lock_guard<std::mutex> lk(m_);
            if (ok && p.valid()) { status_.haveMeasured = true; status_.az = p.az; status_.el = p.el; status_.problem.clear(); }
            else if (!ok) { status_.problem = err.empty() ? std::string("the rotator did not answer") : "the rotator said: " + err; }
            nextPoll = nowMs() + 2000;
        }
        if (!ok) {   // a link that failed is made again at the next try
            transport.reset();
            link.reset();
            nextTry = nowMs() + 2000;
            std::lock_guard<std::mutex> lk(m_);
            status_.connected = false;
        }
    }
    transport.reset();
    link.reset();
}

}  // namespace cascade::engine::s4b
