// ascp_source.hpp - FoxSDR's own client for the network radios that speak RFspace's "ASCP" / SDR-IP
// protocol: RFspace NetSDR, SDR-IP and Cloud-IQ, and the Afedri SDR-Net / AFE822x family (survey row R7).
// TCP control (default port 50000) plus a UDP I/Q stream. No vendor library, no driver.
//
// STAND-ALONE, called by nothing yet. The wire format and its provenance (CuteSDR under the Simplified
// BSD licence, Afedri's own two documents, RFspace's NetSDR guide) are in ascp_protocol.hpp.
//
// UNVERIFIED ON HARDWARE: no NetSDR, SDR-IP, Cloud-IQ or Afedri radio is on the desk. The stand-in server
// in tests/test_ascp_source.cpp is written from the same reading and cannot show the reading right. The
// RFspace interface specification itself (on request only) was NOT obtained. Specifically unproven:
//  * that the radio sends its UDP stream to the source address and port of the one-byte keepalive
//    datagram this client sends (CuteSDR binds its UDP socket to the TCP server port and sends the
//    keepalive from it; this client does the same by default);
//  * the rate tables below (they are the five rates CuteSDR offers per model);
//  * the 24-bit/16-bit switch at 1.5 MS/s and the MTU choice at 100 kS/s (both are CuteSDR's);
//  * the RF-gain steps (0, -10, -20, -30 dB are CuteSDR's choices) and their meaning on an Afedri.
//
// ARGS ("key=value,key=value"):
//   rfspace=host[:port]   the radio (default port 50000)
//   rate=Hz               the sample rate to use at start (default: the model's third table entry)
//   udpport=N             the local UDP port (default: the TCP port; 0 = any, for a host running two radios)
//   timeout=ms            the bound on a request and its answer, and on silence (default 3000)
//
// open() asks the radio for its name (the proof it is one of these radios; an unknown name is refused,
// naming it), serial number, frequency range and current sample rate. start(): the setup items, then
// receiver-on; the sample size and packet size follow the rate. stop(): receiver-idle. closeDevice(): idle
// (if running), then both sockets closed and the reader joined with a bound.
//
// THREADS: the base class's one reader thread; it serves the TCP and UDP sockets together with select().
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "source/ascp_protocol.hpp"
#include "source/net_sample_source.hpp"

namespace cascade::source {

enum class AscpModel { Unknown, NetSdr, SdrIp, CloudIq, Afedri };

// "NetSDR", "SDR-IP", "CloudSDR"/"CloudIQ", "AFEDRI SDR-Net"/"AFE822x SDR-Net" (the names the radios give
// for the interface-name item; the Afedri strings are the ones its own document lists). Unknown for
// anything else, including an empty string.
AscpModel ascpModelFromName(const std::string& name);
const char* ascpModelName(AscpModel m);
// The rates offered for a model (CuteSDR's five per model; for the Afedri family the same list of
// candidates passed through the document's real-rate function with the main clock given).
std::vector<double> ascpRatesFor(AscpModel m, std::uint64_t mainClockHz);

class AscpSource : public NetSampleSource {
public:
    AscpSource() = default;
    ~AscpSource() override;

    const char* driverKey() const override { return "rfspace"; }
    bool open(const std::string& args) override;
    void closeDevice() override;
    bool start() override;
    void stop() override;

    double sampleRateHz() const override;
    bool setSampleRateHz(double hz) override;
    double centerFrequencyHz() const override;
    bool setCenterFrequencyHz(double hz) override;

    std::vector<GainInfo> gains() const override;
    bool setGainDb(const std::string& name, double db) override;
    double gainDb(const std::string& name) const override;
    bool autoGainSupported() const override { return false; }
    bool setAutoGain(bool) override { return false; }
    bool autoGain() const override { return false; }
    std::vector<std::string> antennas() const override { return {}; }
    bool setAntenna(const std::string&) override { return false; }
    std::string antenna() const override { return std::string(); }
    std::vector<double> supportedSampleRatesHz() const override;
    bool frequencyRangeHz(double& lo, double& hi) const override;

    AscpModel model() const;
    std::string interfaceName() const;
    std::string serialNumber() const;
    // The local UDP port the stream is received on (0 when not open).
    std::uint16_t localUdpPort() const;
    // UDP accounting since the last start().
    std::uint64_t packetsLost() const;
    std::uint64_t packetsLate() const;
    std::uint64_t badPackets() const;
    // Radio-sent status 0x20 ("A/D overload") seen since open().
    std::uint64_t overloadReports() const;

private:
    struct Conn;

    bool failOpen(const std::string& why);
    void teardown();
    // Sends one control message and waits for the radio's answer to `item`. `nak` is set when the
    // radio answered with its "not implemented" message; the function then returns true (the caller
    // decides whether that matters). False with lastError() set on a timeout, a send failure or a fault.
    bool transact(ascp::MsgType type, std::uint16_t item, const std::vector<std::uint8_t>& params,
                  std::vector<std::uint8_t>& reply, bool& nak);
    bool transactMandatory(ascp::MsgType type, std::uint16_t item, const std::vector<std::uint8_t>& params,
                           std::vector<std::uint8_t>& reply, const char* what);

    mutable std::mutex ctlMutex_;
    mutable std::mutex connMutex_;
    std::shared_ptr<Conn> conn_;
    int timeoutMs_ = 3000;
    bool active_ = false;
    AscpModel model_ = AscpModel::Unknown;
    std::string name_, serial_;
    std::uint64_t mainClockHz_ = 80000000;
    double rateHz_ = 0.0;
    double wantedRateHz_ = 0.0;
    std::uint64_t freqHz_ = 0;
    ascp::FreqRange range_{0, 0};
    bool haveRange_ = false;
    int rfGainDb_ = 0;
};

}  // namespace cascade::source
