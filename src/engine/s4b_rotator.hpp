// s4b_rotator.hpp - screen lane S4b, G3-20: the antenna rotator, turned from the Outputs section (docs/multi-radio-records/screen-s4b.md).
//
// WHAT IT DOES. It keeps a link to a rotator controller, either a rotctld daemon over TCP (default 127.0.0.1 port 4533) or a Yaesu GS-232 controller on a serial port (COM1..COM256 only), and does
// three things on the person's request: turn to a bearing (azimuth 0..360 or 0..450 by the controller, elevation 0..180), stop, and go to the park bearing. It also asks the controller where the
// antenna is every two seconds and says so. The protocols are core/rotator.hpp's (a client of rotctld and of the GS-232 command set), tested there against stand-ins; they are UNVERIFIED on a
// real rotator.
//
// A ROTATOR IS TURNED; A TRANSMITTER IS NEVER KEYED. The link carries only position commands and the two stop commands of those protocols. Nothing here reaches a radio, a transmit path or a plug-in.
// The person asks for each turn: nothing moves the antenna at start-up or on its own (following an aircraft or a satellite is NOT built in this lane).
//
// OFF until the person turns it on; no socket or port is opened while it is off. The rotctld daemon has no password and sends its commands unencrypted: an address other than this computer is a
// deliberate setting and the window says so. A serial port name is checked to be a COM name before the system is asked to open it (a typed path or pipe is refused).
//
// THREADS. One worker owns the link (connecting and a command may take up to two seconds); configure() and the requests only record, so the engine's pass never waits. stop() joins the worker.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "core/rotator.hpp"

namespace cascade::engine::s4b {

struct RotatorConfig {
    bool enabled = false;
    bool serial = false;             // false: rotctld over TCP; true: a Yaesu GS-232 on a serial port
    std::string host = "127.0.0.1";  // rotctld
    int port = 4533;
    std::string comPort;             // GS-232: "COM3"
    int baud = 9600;
    int azMaxDeg = 450;              // 360 or 450 (the controller type)
    double parkAz = 0.0, parkEl = 0.0;
    bool operator==(const RotatorConfig& o) const {
        return enabled == o.enabled && serial == o.serial && host == o.host && port == o.port && comPort == o.comPort && baud == o.baud && azMaxDeg == o.azMaxDeg && parkAz == o.parkAz && parkEl == o.parkEl;
    }
};

struct RotatorStatus {
    bool enabled = false;
    bool connected = false;
    bool haveMeasured = false;
    double az = 0.0, el = 0.0;       // where the controller says the antenna is
    std::string problem;             // one sentence, "" = none
    std::uint64_t turns = 0, stops = 0, parks = 0, failures = 0;
};

// The result of a request: accepted for the worker, or a sentence.
struct RotatorResult {
    bool ok = true;
    std::string error;
};

// Opens the byte stream for a config: a stand-in in a test. `error` gets a sentence when it returns null.
using RotatorTransportFactory = std::function<std::unique_ptr<cascade::core::rotator::Transport>(const RotatorConfig&, std::string& error)>;

// The real factory: a TCP connection to rotctld, or a serial port. A test gives its own and never calls this.
std::unique_ptr<cascade::core::rotator::Transport> openRealRotatorTransport(const RotatorConfig& c, std::string& error);

class RotatorControl {
public:
    RotatorControl() = default;
    ~RotatorControl();
    RotatorControl(const RotatorControl&) = delete;
    RotatorControl& operator=(const RotatorControl&) = delete;

    void setFactory(RotatorTransportFactory f);
    void configure(const RotatorConfig& c);            // records the wish; the worker acts on it
    RotatorResult turn(double az, double el);          // validated against the controller type, queued
    RotatorResult stopMotion();
    RotatorResult park();
    RotatorStatus status() const;
    void shutdown();                                   // the engine's teardown: the link is let go and the worker joined

    // A validation any caller may use first: the sentence when this bearing is not one the controller takes, "" when it is.
    static std::string checkBearing(double az, double el, int azMaxDeg);

private:
    void run();
    void ensureWorker();

    mutable std::mutex m_;
    std::condition_variable cv_;
    std::thread worker_;
    bool stopWorker_ = false;
    RotatorTransportFactory factory_;
    RotatorConfig cfg_, appliedCfg_;
    bool dirty_ = false;
    struct Request { enum Kind { Turn, Stop, Park } kind = Turn; double az = 0.0, el = 0.0; };
    std::optional<Request> pending_;
    RotatorStatus status_;
};

}  // namespace cascade::engine::s4b
