// aor_control.hpp - AOR receiver control over the FTDI serial port: the
// command text, the identification of a VR reply, the documented start-up
// sequence, tuning, and finding WHICH serial port is the receiver.
//
// THE SOURCE. AOR's "Digital I/Q USB Interface Developer Information"
// Rev 1.1, "Receiver control": FTDI 0403:6001, 115200 baud, 8N1, no flow
// control, raw mode, commands terminated by CR. The tested AR5700D start-up
// is EX, VR, @21, VFA; the example tune is RF0081.300000 for 81.3 MHz. Only
// the AR5700D's behaviour is documented, and only that behaviour is claimed
// here; everything about the other models is in ONE place (identifyVr) and
// labelled unverified.
//
// WHY A PORT IS NEVER CHOSEN BY VID/PID ALONE. 0403:6001 is the stock FTDI
// FT232R identity, used by thousands of unrelated products - GPS pucks,
// programmers, other radios' CAT cables. VID/PID narrows the candidates to
// FTDI ports (so FoxSDR never sends a byte to a port that is not even an FTDI
// chip); the receiver's own answer to VR decides. The COM number and the
// USB serial number are never written down anywhere: the AOR document says
// both are unit- and machine-specific.
//
// Receiver control has been tested against a scripted fake only. NOT TESTED
// on hardware.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cascade::source::aor {

// --- the pure half ------------------------------------------------------------

constexpr int kControlBaud = 115200;

// How long a reply line may take to arrive after its command was sent. Bounds
// every wait in this file; a receiver that says nothing costs this, never a
// hang.
constexpr std::chrono::milliseconds kReplyWait{500};

// The largest frequency the RF command's four-digit MHz field can carry.
constexpr double kMaxFormattableHz = 9999999999.0;  // 9999.999999 MHz

// "RF0081.300000" for 81.3 MHz: RF, the frequency in MHz as four integer
// digits, a point, and six decimals - exactly the AOR document's example,
// which is 1 Hz resolution. Built from the frequency ROUNDED TO WHOLE HERTZ
// in integer arithmetic, so no binary-fraction error can turn 81.3 MHz into
// 81.299999. No CR here; the session adds it. False (and `out` untouched)
// for a negative, non-finite or out-of-field frequency.
bool formatTuneCommand(double hz, std::string& out);

// Which AOR receiver a VR reply names.
enum class Model { Unknown, AR5700D, AR2300, AR5001D, AR6000 };

struct Identity {
    bool isAor = false;   // the reply names an AOR receiver this driver knows
    Model model = Model::Unknown;
    bool verified = false;  // true only for behaviour the AOR document states
    // Whether the start-up sends @21 (AR2300-compatible command mode). The
    // AOR document gives it for the AR5700D only.
    bool sendCompatMode = false;
    std::string reply;    // the VR line as received, trimmed
};

// THE ONE PLACE MODELS ARE DETECTED.
//   AR5700D: the reply contains "C5700_" - DOCUMENTED (AOR document: "the
//            AR5700D replied with a version containing C5700_").
//   AR2300 / AR5001D / AR6000 (IQ5001): "C2300_", "C5001_", "C6000_" by
//            analogy with the AR5700D's form - UNVERIFIED, a guess at the
//            reply format that AOR has not confirmed; @21 is not sent to them.
// Anything else is not an AOR receiver as far as this driver knows.
Identity identifyVr(const std::string& reply);

const char* modelName(Model m);

// --- the link, and the session over it ------------------------------------------

// What the session needs from a serial port - write a command, read with a
// short bounded wait - so a test can be the receiver.
class ControlLink {
public:
    virtual ~ControlLink() = default;
    // Bytes written (== len on success), or negative.
    virtual int write(const char* data, std::size_t len) = 0;
    // 1..cap bytes, 0 when the (short) wait expired, negative when gone.
    virtual int read(char* buf, std::size_t cap) = 0;
    // The port's name, for messages ("COM5", "/dev/ttyUSB0").
    virtual std::string name() const = 0;
};

// Opens a port by name at 115200 8N1, read-write (core::SerialPort). Null
// with `error` set when it cannot be opened.
std::unique_ptr<ControlLink> openSerialControlLink(const std::string& port, std::string& error);

class ControlSession {
public:
    explicit ControlSession(ControlLink& link) : link_(link) {}

    // Sends `command` followed by CR.
    bool send(const std::string& command, std::string& error);

    // The next non-empty line (terminated by CR or LF), trimmed, or empty when
    // none arrived within `wait`. `gone` is set when the port failed.
    std::string readLine(std::chrono::milliseconds wait, bool& gone);

    // EX then VR, and the VR reply identified. Does NOT go on to @21/VFA - a
    // port is probed with this, and nothing more is sent to a port that has
    // not identified itself as an AOR receiver. False only when the port
    // itself failed; `id.isAor` says whether it was a receiver.
    bool probe(Identity& id, std::string& error);

    // The documented start-up, in the documented order: EX, VR, (@21 for an
    // AR5700D), VFA. Refuses - sending nothing after VR - a port whose VR
    // reply does not identify an AOR receiver.
    bool initialise(Identity& id, std::string& error);

    // RFnnnn.nnnnnn<CR>.
    bool tune(double hz, std::string& error);

private:
    ControlLink& link_;
    std::string pending_;  // bytes read past the end of the last line
};

// --- which serial port --------------------------------------------------------

// The serial ports that are FTDI 0403:6001 chips - CANDIDATES only, never an
// answer on their own (see the file header). Windows: the Ports device class
// through SetupAPI, matching the hardware id and reading PortName. Linux:
// /sys/class/tty/ttyUSB* whose USB device carries idVendor 0403 / idProduct
// 6001. Nothing is opened to produce the list.
std::vector<std::string> ftdiControlPortCandidates();

// The Linux walk behind the above, over a sysfs root and a /dev directory so a
// test can hand it a fixture tree: every <ttyClassDir>/<name> whose
// device/../idVendor and device/../idProduct read 0403 / 6001 becomes
// "<devDir>/<name>".
std::vector<std::string> ftdiPortsFromSysfs(const std::string& ttyClassDir,
                                            const std::string& devDir);

// The outcome of pairing: exactly one candidate answered VR as an AOR.
struct Pairing {
    bool ok = false;
    std::string port;
    Identity identity;
    std::string error;  // user-facing, in the language in force, when !ok
    // Every port tried and what it said, for the log.
    std::vector<std::string> tried;
};

using LinkOpener = std::function<std::unique_ptr<ControlLink>(const std::string& port,
                                                              std::string& error)>;

// Probes each candidate with EX/VR and keeps the ones that identify as AOR.
// One: paired. None: refused with a sentence saying what to connect. More
// than one: refused - with only VR to go on, FoxSDR cannot tell which control
// port belongs to which I/Q interface, and guessing would tune the wrong
// receiver. When `requested` names a port (args "control=COM5"), only that
// port is probed and a non-AOR answer is refused with the answer quoted.
Pairing pairControlPort(const std::vector<std::string>& candidates, const std::string& requested,
                        const LinkOpener& open);

}  // namespace cascade::source::aor
