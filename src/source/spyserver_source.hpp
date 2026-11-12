// spyserver_source.hpp - FoxSDR's client for Airspy's SpyServer: an Airspy One/Mini/R2, Airspy HF+ or RTL-SDR on another
// machine, streamed here as I/Q over one TCP connection. A DeviceSource with no Airspy library, driver or SoapySDR module.
//
// STAND-ALONE, called by nothing yet (hand-off in docs/feature-records/oss-net.md). OFF until the user types a server
// address: open() with no address is refused with a sentence, and there is no default host. The wire format and where it
// came from (two permissively licensed open clients) are in spyserver_protocol.hpp, with the list of what they leave open.
// UNVERIFIED AGAINST A REAL SPYSERVER; AIRSPY'S TERMS FOR THIRD-PARTY CLIENTS ARE UNKNOWN AND ARE THE OWNER'S DECISION
// BEFORE THIS SHIPS. No credential is sent, stored or logged (the protocol, as the two clients read it, has none).
//
// ARGS ("key=value,key=value"):
//   spyserver=host[:port]   REQUIRED (port default 5555)
//   format=int16|uint8|float   the I/Q format asked for (default int16); a server that forces 8-bit sends 8-bit whatever
//                              was asked, and each message names its own type, so the data is read as it comes
//   rate=Hz                 the sample rate to ask for at open (the nearest the device offers); default: the highest
//   timeout=ms              the bound on connecting and on the server's description (default 3000)
//
// open(): connect, HELLO, wait for the DEVICE_INFO and CLIENT_SYNC messages, refuse a server that sends neither (full, or no
// radio: it closes the connection), one with no device, of another protocol version, or one that did not give this client
// control; then set the stream mode (I/Q only), the format and the decimation. The reader thread runs from open() to
// closeDevice() (it must read the server's CLIENT_SYNC messages); start()/stop() send STREAMING_ENABLED 1/0 and flip the
// keep-or-discard gate. Frequency and gain are sent at once when set. The server NEVER acknowledges a setting except
// through the next CLIENT_SYNC, which the reader folds into the readbacks.
//
// THE GAIN is a stage index 0..MaximumGainIndex (GainUnit::Steps), not decibels.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "source/net_sample_source.hpp"
#include "source/spyserver_protocol.hpp"

namespace cascade::source {

class SpyServerSource : public NetSampleSource {
public:
    SpyServerSource() = default;
    ~SpyServerSource() override;

    const char* driverKey() const override { return "spyserver"; }
    bool open(const std::string& args) override;
    void closeDevice() override;
    bool start() override;
    void stop() override;

    double sampleRateHz() const override;
    bool setSampleRateHz(double hz) override;
    double centerFrequencyHz() const override;
    bool setCenterFrequencyHz(double hz) override;

    std::vector<GainInfo> gains() const override;
    bool setGainDb(const std::string& name, double v) override;
    double gainDb(const std::string& name) const override;
    bool autoGainSupported() const override { return false; }
    bool setAutoGain(bool) override { return false; }
    bool autoGain() const override { return false; }
    std::vector<std::string> antennas() const override { return {}; }
    bool setAntenna(const std::string&) override { return false; }
    std::string antenna() const override { return std::string(); }
    std::vector<double> supportedSampleRatesHz() const override;
    bool frequencyRangeHz(double& lo, double& hi) const override;

    std::string deviceTypeName() const;
    std::uint64_t otherMessages() const;  // audio, FFT, 24-bit and unknown messages seen and skipped
    std::uint64_t sequenceGaps() const;   // I/Q messages the server numbered but this client did not receive

private:
    struct Session;

    bool failOpen(const std::string& why);
    bool sendSet(std::uint32_t setting, std::uint32_t value, const char* what);

    mutable std::mutex ctlMutex_;
    std::shared_ptr<Session> session_;
    spyserver::DeviceInfo info_;
    int timeoutMs_ = 3000;
    std::uint32_t format_ = spyserver::kFmtInt16;
    std::uint32_t stage_ = 0;
    double rateHz_ = 0.0;
    bool active_ = false;
    std::string host_;
};

}  // namespace cascade::source
