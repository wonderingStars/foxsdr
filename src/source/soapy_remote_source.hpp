// soapy_remote_source.hpp - FoxSDR's own client for a SoapyRemote server (survey row R4): a
// radio on another machine that runs SoapySDR's `SoapySDRServer`, reached by address, with no
// SoapySDR, no SoapyRemote module and no vendor driver on this machine.
//
// STAND-ALONE, called by nothing yet. Discovery (the mDNS/SSDP search for servers) is left
// out: the user types an address. The wire format and its provenance are in
// soapy_remote_protocol.hpp (SoapyRemote is Boost-licensed, its LICENSE_1_0.txt was opened
// first).
//
// UNPROVEN AGAINST A REAL SoapyRemote SERVER: the suite's stand-in is written from the same
// description (tests/test_soapy_remote_source.cpp) and cannot show that a real server
// accepts these bytes. It was not tried: no server was reachable and no outside service may
// be contacted from this lane.
//
// WHAT IT DOES. open(): connect (TCP, default port 55132), MAKE the device with the args the
// user gave, read the device's native RX stream format and full-scale, its sample rates,
// frequency range, gains and antennas, then set up an RX stream on channel 0 and start the
// reader. start(): ACTIVATE_STREAM. stop(): DEACTIVATE_STREAM. closeDevice(): CLOSE_STREAM,
// UNMAKE, HANGUP, then the reader is woken and joined with a bound.
//
// ARGS (the DeviceSource args string, "key=value,key=value"):
//   soapyremote=host[:port]   the server (required; "localhost" and 127.0.0.1 work)
//   prot=tcp|udp              the stream transport. DEFAULT tcp, deliberately: the reference
//                             client defaults to udp, but a udp stream needs the server to
//                             reach a port on this machine, which a firewall refuses by
//                             default. udp is built and tested against the stand-in only.
//   device=<key>:<value>;...  device args sent to MAKE, e.g. device=driver:rtlsdr
//                             (';' separates pairs because ',' separates the args)
//   timeout=<ms>              the bound on each call (default 2000)
//
// THREADS: the base class's one reader thread (net_sample_source.hpp). Control calls run on
// the caller's thread under one mutex.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "source/net_sample_source.hpp"
#include "source/soapy_remote_protocol.hpp"

namespace cascade::source {

// "host", "host:port", "[v6]:port"; default port 55132. False with `error` for a bad port.
bool parseSoapyRemoteAddress(const std::string& text, std::string& host, std::uint16_t& port,
                             std::string& error);

class SoapyRemoteSource : public NetSampleSource {
public:
    SoapyRemoteSource() = default;
    ~SoapyRemoteSource() override;

    const char* driverKey() const override { return "soapyremote"; }
    bool open(const std::string& args) override;
    // TEST SEAM: open() with the control transport supplied.
    bool openWithTransportForTest(const std::string& args, std::shared_ptr<iiod::Transport> control);
    void closeDevice() override;

    bool start() override;
    void stop() override;

    double sampleRateHz() const override;
    bool setSampleRateHz(double hz) override;
    double centerFrequencyHz() const override;
    bool setCenterFrequencyHz(double hz) override;
    bool hasFrequencyCorrection() const override;
    bool setFrequencyCorrectionPpm(double ppm) override;

    std::vector<GainInfo> gains() const override;
    bool setGainDb(const std::string& name, double db) override;
    double gainDb(const std::string& name) const override;
    bool autoGainSupported() const override;
    bool setAutoGain(bool on) override;
    bool autoGain() const override;
    std::vector<std::string> antennas() const override;
    bool setAntenna(const std::string& name) override;
    std::string antenna() const override;
    std::vector<double> supportedSampleRatesHz() const override;
    bool frequencyRangeHz(double& loHz, double& hiHz) const override;

    // What the server said about itself.
    std::string remoteFormat() const;   // "CS8", "CU8", "CS16" or "CF32"
    double remoteFullScale() const;
    std::string streamProtocol() const;

private:
    struct Stream;  // the stream half: sockets, id, flow control (shared with the reader)

    bool openImpl(const std::string& args, std::shared_ptr<iiod::Transport> injected);
    // One request/reply on the control connection (mutex held). Throws std::runtime_error.
    soapyremote::Unpacker callLocked(const soapyremote::Packer& request);
    void queryDeviceLocked();
    bool setupStreamLocked(const std::string& prot);
    void teardownLocked();
    bool failOpen(const std::string& why);

    mutable std::mutex mutex_;
    std::shared_ptr<iiod::Transport> control_;
    std::shared_ptr<Stream> stream_;
    int streamId_ = -1;
    bool made_ = false;
    bool active_ = false;
    int ioWaitMs_ = 2000;
    std::string host_;
    std::string prot_ = "tcp";

    std::string remoteFormat_ = "CS16";
    double fullScale_ = 32768.0;
    std::vector<double> rates_;
    std::vector<soapyremote::Range> freqRanges_;
    std::vector<GainInfo> gains_;
    std::vector<std::string> antennas_;
    bool hasGainMode_ = false;
    bool hasCorrection_ = false;

    double rateHz_ = 0.0;
    double centerHz_ = 0.0;
    std::string antenna_;
    bool autoGain_ = false;
    std::map<std::string, double> gainValues_;
};

}  // namespace cascade::source
