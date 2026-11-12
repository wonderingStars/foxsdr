// hpsdr2_source.hpp - FoxSDR's receive-only client for the openHPSDR PROTOCOL 2 (Ethernet protocol) network
// radios: discovery, the General / DDC-Specific / High-Priority packets with the run bit, DDC0 I&Q on its
// own UDP port, the periodic command-and-control refresh the document asks for, and the status packet's ADC
// overload and FIFO flags. Packets and provenance are in hpsdr2_protocol.hpp (the project's own document,
// V4.4). Boards 1 to 4 only (the document's Appendix A has tables for those); any other board type is
// refused with a sentence.
//
// STAND-ALONE, called by nothing yet; UNVERIFIED ON HARDWARE (no HPSDR radio is on the desk). RECEIVE ONLY:
// the PTT bit, the drive level and every transmit-side byte stay zero, no DUC I&Q or microphone packet is
// ever sent, the PA and Alex enables stay off.
//
// ONE SOCKET, BECAUSE THE DOCUMENT SAYS SO: the hardware answers to "the Source Port of the Host that
// initiated the Discovery", from several of its own ports (1024 discovery, 1025 status, 1035 DDC0 by
// default). The host therefore keeps ONE unconnected UDP socket, sends discovery from it and receives
// everything on it, telling the packets apart by the device's source port. A loopback target is bound to
// 127.0.0.1 only.
//
// ARGS ("key=value,key=value"):
//   hpsdr2=host[:port]   the device (port default 1024). Without it the discovery is BROADCAST to
//                        255.255.255.255:1024 and the first radio that answers (or the mac= one) is used.
//   mac=aa:bb:...        choose among several that answered
//   takeover=1           use a radio that says it is already connected to another host (refused otherwise)
//   timeout=ms           discovery window and bound on silence (default 2000)
//   ddcport= hpport= statusport= ddc0port=   the UDP ports asked for in the General packet (default: the
//                        document's 1025, 1027, 1025, 1035; they are for stand-ins and unusual setups)
//
// THREADS: the base class's one reader thread (net_sample_source.hpp), which also sends the periodic refresh.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <complex>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "source/hpsdr2_protocol.hpp"
#include "source/hpsdr_protocol.hpp"
#include "source/net_sample_source.hpp"

namespace cascade::source {

struct Hpsdr2Device {
    std::string ip;
    std::uint16_t port = hpsdr2::kDiscoveryPort;
    hpsdr2::DiscoveryReply reply;
    std::string name() const;  // "Hermes (ANAN-10, 100) (aa:bb:..., firmware 32)"
};

// Sends the protocol-2 discovery to host:port (255.255.255.255 broadcasts) and collects every valid reply
// for `window`. False with `error` only when no socket could be used; an empty list means nothing answered.
bool discoverHpsdr2(const std::string& host, std::uint16_t port, std::chrono::milliseconds window,
                    std::vector<Hpsdr2Device>& found, std::string& error);

class Hpsdr2Source : public NetSampleSource {
public:
    Hpsdr2Source() = default;
    ~Hpsdr2Source() override;

    const char* driverKey() const override { return "hpsdr2"; }
    bool open(const std::string& args) override;
    void closeDevice() override;
    bool start() override;
    void stop() override;

    double sampleRateHz() const override;
    // 48, 96, 192, 384, 768 or 1536 kHz; anything else is refused with a sentence.
    bool setSampleRateHz(double hz) override;
    double centerFrequencyHz() const override;
    // As a phase word (2^32 * f / 122.88 MHz) or in Hz, as the radio's reply said; 0 up to (not including)
    // the DSP clock for a phase word.
    bool setCenterFrequencyHz(double hz) override;

    // "Attenuator" 0 to 31 dB, the step attenuator before ADC0.
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
    bool frequencyRangeHz(double&, double&) const override { return false; }

    Hpsdr2Device device() const;
    std::uint64_t packetsLost() const;
    std::uint64_t packetsLate() const;
    std::uint64_t badPackets() const;
    // From the status packets since the last start().
    std::uint64_t statusPackets() const;
    std::uint64_t adcOverloadPackets() const;
    std::uint64_t fifoOverflowPackets() const;
    // Packets sent to the radio since open (all kinds).
    std::uint64_t controlPacketsSent() const;

    struct Net;  // the UDP socket (implementation detail, public so the scan function can use it)

private:
    struct Rx;

    bool applyLocked();  // re-sends what the changed setting needs; caller holds mutex_

    mutable std::mutex mutex_;
    std::shared_ptr<Rx> rx_;
    Hpsdr2Device dev_;
    hpsdr2::PortPlan ports_;
    bool active_ = false;
    int timeoutMs_ = 2000;
    int rateHz_ = 48000;
    double freqHz_ = 0.0;
    int attenDb_ = 0;
};

}  // namespace cascade::source
