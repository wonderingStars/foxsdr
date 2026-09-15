// device_source.hpp - what the Source section needs from ANY radio, native or
// SoapySDR, so the panel can be written once.
//
// IqSource is what the pipeline needs: start, stop, read, the rate and the
// centre frequency. The Source section needs more - the gains a radio has and
// their ranges, whether it can set its own gain, its antennas, the rates it
// supports, its tuning range, and a way to say the device is dead rather
// than merely quiet. SoapySource has grown all of that over the months as
// separate methods the GUI calls directly; a native driver implements the
// same set through this interface, and the Source section will move to it so
// an RTL-SDR opened natively and one opened through Soapy look identical on
// screen.
//
// Every method here is safe to call from the GUI thread while the reader
// thread streams: the driver serialises against its own device lock, and no
// call blocks for longer than the driver's own bounded USB timeouts.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cctype>
#include <string>
#include <vector>

#include "source/iq_source.hpp"

namespace cascade::source {

// ONE PARSER FOR BOTH FAMILIES' ARGS STRINGS, because they have the same
// shape and are compared against each other. A SoapySDR kwargs markup string
// ("driver=rtlsdr, serial=00000001") and a native driver's args
// ("serial=00000001", "index=0") are both comma-separated key=value, and the
// Source section has to ask one about the other: "is the radio this config
// saved through SoapySDR the same physical dongle as this native row" is a
// comparison of a serial read out of each. Written here rather than in either
// driver so there is exactly one answer to what "serial=" means.
//
// The key is matched case-insensitively and the value is returned verbatim
// with surrounding spaces trimmed; an absent key gives an empty string, which
// is also what an empty value gives - the callers treat "not stated" and
// "stated as nothing" the same way, deliberately.
inline std::string argValue(const std::string& args, const std::string& key) {
    std::size_t pos = 0;
    while (pos <= args.size()) {
        std::size_t end = args.find(',', pos);
        if (end == std::string::npos) { end = args.size(); }
        std::string field = args.substr(pos, end - pos);
        const std::size_t eq = field.find('=');
        if (eq != std::string::npos) {
            std::string k = field.substr(0, eq);
            std::string v = field.substr(eq + 1);
            const auto trim = [](std::string& s) {
                std::size_t a = 0;
                while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) { ++a; }
                std::size_t b = s.size();
                while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) { --b; }
                s = s.substr(a, b - a);
            };
            trim(k);
            trim(v);
            bool match = k.size() == key.size();
            for (std::size_t i = 0; match && i < k.size(); ++i) {
                match = std::tolower(static_cast<unsigned char>(k[i])) ==
                        std::tolower(static_cast<unsigned char>(key[i]));
            }
            if (match) { return v; }
        }
        if (end == args.size()) { break; }
        pos = end + 1;
    }
    return {};
}

// One gain the radio exposes, with the range the driver will accept.
struct GainInfo {
    std::string name;   // as shown to the user: "LNA", "VGA", "TUNER"
    double minDb = 0.0;
    double maxDb = 0.0;
    double stepDb = 1.0;
};

// One device a native driver can open. `driver` is the driver key ("rtlsdr",
// "hackrf"), `args` is what open() takes back, `label` is what the combo
// shows ("RTL-SDR Blog V4 (serial 00000001)").
struct NativeDeviceInfo {
    std::string driver;
    std::string label;
    std::string args;
};

class DeviceSource : public IqSource {
public:
    // Driver key, the same string NativeDeviceInfo::driver carries.
    virtual const char* driverKey() const = 0;

    // Lifecycle beyond IqSource. open() takes NativeDeviceInfo::args; it may
    // be called once per object. closeDevice() releases the hardware and is
    // idempotent; the destructor calls it.
    virtual bool open(const std::string& args) = 0;
    virtual void closeDevice() = 0;
    virtual bool isOpen() const = 0;

    // Gains. setGainDb returns false and sets lastError() for an unknown name
    // or a value the driver refuses (out of range is CLAMPED, not refused,
    // and gainDb() reports what was actually set).
    virtual std::vector<GainInfo> gains() const = 0;
    virtual bool setGainDb(const std::string& name, double db) = 0;
    virtual double gainDb(const std::string& name) const = 0;
    virtual bool autoGainSupported() const = 0;
    virtual bool setAutoGain(bool on) = 0;
    virtual bool autoGain() const = 0;

    // Antennas. A radio with one antenna reports one name and accepts it.
    virtual std::vector<std::string> antennas() const = 0;
    virtual bool setAntenna(const std::string& name) = 0;
    virtual std::string antenna() const = 0;

    // The rates the hardware supports, ascending, in Hz. setSampleRateHz
    // (IqSource) coerces to the nearest supported rate and sampleRateHz()
    // reports the readback, exactly as the Soapy path does.
    virtual std::vector<double> supportedSampleRatesHz() const = 0;

    // The tuning range the driver will accept; false when unknown.
    virtual bool frequencyRangeHz(double& loHz, double& hiHz) const = 0;

    // Dead means the hardware is gone or the driver has given up on it:
    // faulted() is true, nothing further is sent to the device, and the
    // owner's remedy is to reopen (or replug). faultedWhile() names the
    // operation that failed, for the log and the report.
    virtual bool deviceDead() const = 0;
    virtual std::string faultedWhile() const = 0;
};

}  // namespace cascade::source
