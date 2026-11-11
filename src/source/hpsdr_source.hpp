// hpsdr_source.hpp - FoxSDR's client for the openHPSDR protocol-1 family of network radios (Metis, Hermes,
// ANAN, Hermes-Lite ...; survey row R7): discovery, start and stop, and the receive stream's UDP envelope,
// from the published "Metis - How it works" guide (hpsdr_protocol.hpp has the provenance and the full list
// of what the guide does not say).
//
// STAND-ALONE, called by nothing yet, and INCOMPLETE BY NECESSITY: the guide does not contain the layout
// of the 512-byte frames the data packets carry, so the I/Q sample unpacking and the frequency, rate and
// gain commands (all of which live inside those frames) are NOT built. The class takes a
// HpsdrFrameDecoder from its caller; without one start() refuses with a sentence saying so, and the
// setters refuse. The tests install a decoder of their OWN invention (a test layout, labelled as such) to
// prove the plumbing; it says nothing about the real frame format. UNPROVEN on every real device.
// Receive only: no data is ever sent to end point 2.
//
// ARGS ("key=value,key=value"):
//   hpsdr=host[:port]   the device (port default 1024). Without it, discovery is broadcast to
//                       255.255.255.255:1024 and the first device that answers (or the one whose MAC
//                       matches mac=) is used.
//   mac=aa:bb:cc:dd:ee:ff   choose among several that answered
//   timeout=ms          the discovery window and the bound on silence (default 2000)
//
// THREADS: the base class's one reader thread (net_sample_source.hpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <complex>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "source/hpsdr_protocol.hpp"
#include "source/net_sample_source.hpp"

namespace cascade::source {

struct HpsdrDevice {
    std::string ip;
    std::uint16_t port = hpsdr::kDevicePort;
    hpsdr::DiscoveryReply reply;
    std::string name() const;  // "Hermes (aa:bb:..., version 3)"
};

// Sends a discovery request to host:port (255.255.255.255 broadcasts) from an ephemeral UDP port and
// collects every valid reply for `window`. False with `error` only when the socket could not be used; an
// empty list is a successful search that found nothing. Replies that are not discovery replies are
// ignored; one device answering twice is listed once (by MAC).
bool discoverHpsdr(const std::string& host, std::uint16_t port, std::chrono::milliseconds window,
                   std::vector<HpsdrDevice>& found, std::string& error);

// What turns the 1024 bytes of a data packet (two 512-byte frames) into samples. Supplied by the caller
// because the frame layout is not in the document this lane may read.
class HpsdrFrameDecoder {
public:
    virtual ~HpsdrFrameDecoder() = default;
    // Appends the samples of `frames` (1024 bytes) to `out`; false when the frames are not valid (the
    // reader counts it and carries on).
    virtual bool decode(const std::uint8_t* frames1024, std::vector<std::complex<float>>& out) = 0;
    virtual double sampleRateHz() const = 0;
};

class HpsdrSource : public NetSampleSource {
public:
    HpsdrSource() = default;
    ~HpsdrSource() override;

    // The decoder must be set before start(); it is used on the reader thread.
    void setFrameDecoder(std::shared_ptr<HpsdrFrameDecoder> d);

    const char* driverKey() const override { return "hpsdr"; }
    bool open(const std::string& args) override;
    void closeDevice() override;
    bool start() override;
    void stop() override;

    double sampleRateHz() const override;
    bool setSampleRateHz(double hz) override;  // refused: the rate is set inside the frames (not built)
    double centerFrequencyHz() const override { return 0.0; }
    bool setCenterFrequencyHz(double) override;  // refused, same reason

    std::vector<GainInfo> gains() const override { return {}; }
    bool setGainDb(const std::string&, double) override { return false; }
    double gainDb(const std::string&) const override { return 0.0; }
    bool autoGainSupported() const override { return false; }
    bool setAutoGain(bool) override { return false; }
    bool autoGain() const override { return false; }
    std::vector<std::string> antennas() const override { return {}; }
    bool setAntenna(const std::string&) override { return false; }
    std::string antenna() const override { return std::string(); }
    std::vector<double> supportedSampleRatesHz() const override;
    bool frequencyRangeHz(double&, double&) const override { return false; }

    // What the device said in discovery.
    HpsdrDevice device() const;
    // Receive accounting since the last start().
    std::uint64_t packetsLost() const;
    std::uint64_t packetsLate() const;
    std::uint64_t badPackets() const;

private:
    struct Rx;

    mutable std::mutex mutex_;
    std::shared_ptr<HpsdrFrameDecoder> decoder_;
    std::shared_ptr<Rx> rx_;
    std::shared_ptr<UdpTransport> udp_;
    HpsdrDevice dev_;
    bool active_ = false;
    int timeoutMs_ = 2000;
};

}  // namespace cascade::source
