// sdrconnect_source.hpp - FoxSDR's own client for SDRplay's SDRconnect (GUI or Headless) over its
// documented WebSocket API (survey row R3): a radio on this or another machine, reached by
// address, its I/Q stream delivered as 16-bit pairs. No SDRplay API, no vendor library.
//
// STAND-ALONE, called by nothing yet. Provenance and the exact list of what the document does and
// does not say are in sdrconnect_protocol.hpp; the WebSocket layer is RFC 6455 (websocket_client.hpp).
//
// UNPROVEN AGAINST A REAL SDRconnect: the stand-in in tests/test_sdrconnect_source.cpp is written
// from the same document, and the document leaves the TCP port, the WebSocket path and the IQ
// stream's sample rate unstated. The port default (5454) comes from a search snippet and is NOT
// VERIFIED: the address should always be given with its port until it is.
//
// ARGS ("key=value,key=value"):
//   sdrconnect=host[:port]   the server (default 127.0.0.1:5454, the port NOT VERIFIED)
//   path=/...                the WebSocket path (default "/")
//   serial=...               select the device by serial (selected_device_serial)
//   mode=...                 network mode appended to the serial as "serial:mode" ("Full IQ",
//                            "IQ Lite" or "Compact", the three names the document lists)
//   name=...                 select by display name (selected_device_name)
//   index=N                  select by index (selected_device)
//   timeout=ms               the bound on a request and its answer (default 3000)
//   iqrate=Hz                the sample rate the I/Q STREAM is treated as having (default: the device's own
//                            device_sample_rate). The document does not say whether the stream is decimated
//                            ("IQ Lite" suggests it may be), so a person who has measured the real rate can state
//                            it here; with iqrate set the rate is fixed (setSampleRateHz accepts only that value
//                            and supportedSampleRatesHz offers only it) and the device's own rate is left alone.
//   The port is the part of sdrconnect=host:port; it is never assumed beyond the default 5454 (NOT VERIFIED).
// With none of serial/name/index the device SDRconnect already has active is used.
//
// WHAT open() DOES: handshake, start the reader, read api_version (the proof this is SDRconnect),
// optionally select the device, read the device's sample rate, centre frequency, LNA range and
// antennas. start(): iq_stream_enable and device_stream_enable true. stop(): both false.
// closeDevice(): a WebSocket close frame, then the reader is woken and joined with a bound.
//
// THREADS: the base class's one reader thread. Requests are sent from the caller's thread under a
// send mutex; replies arrive on the reader and are matched by property name.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "source/net_sample_source.hpp"

namespace cascade::source {

constexpr std::uint16_t kSdrConnectDefaultPort = 5454;  // NOT VERIFIED: see the header

class SdrConnectSource : public NetSampleSource {
public:
    SdrConnectSource() = default;
    ~SdrConnectSource() override;

    const char* driverKey() const override { return "sdrconnect"; }
    bool open(const std::string& args) override;
    bool openWithTransportForTest(const std::string& args, std::shared_ptr<iiod::Transport> t);
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
    std::vector<std::string> antennas() const override;
    bool setAntenna(const std::string& name) override;
    std::string antenna() const override;
    // The document lists no rates: only the one the server reports is offered.
    std::vector<double> supportedSampleRatesHz() const override;
    bool frequencyRangeHz(double&, double&) const override { return false; }

    // What the server said: api_version and the active device's display name.
    std::string apiVersion() const;
    std::string activeDevice() const;
    std::vector<std::string> validDevices() const;

private:
    struct Conn;

    bool openImpl(const std::string& args, std::shared_ptr<iiod::Transport> injected);
    bool failOpen(const std::string& why);
    void teardown();
    bool sendRequest(const std::string& eventType, const std::string& property,
                     const std::string& value);
    // get_property and wait for the answer; false with lastError set on timeout.
    bool getProperty(const std::string& name, std::string& value);
    bool setProperty(const std::string& name, const std::string& value);
    std::string prop(const std::string& name) const;

    mutable std::mutex ctlMutex_;
    mutable std::mutex connMutex_;  // guards the pointer itself, for the getters on other threads
    std::shared_ptr<Conn> conn_;
    int timeoutMs_ = 3000;
    bool active_ = false;
    std::vector<std::string> antennas_;
    std::vector<std::string> devices_;
    int lnaMin_ = 0, lnaMax_ = 0;
    bool hasLna_ = false;
    double iqRateHz_ = 0.0;  // 0 = use device_sample_rate
};

}  // namespace cascade::source
