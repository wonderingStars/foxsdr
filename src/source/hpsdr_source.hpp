// hpsdr_source.hpp - FoxSDR's client for the openHPSDR protocol-1 family of network radios (Metis, Hermes,
// ANAN, Hermes-Lite ...; survey row R7): discovery, start and stop, and the receive stream's UDP envelope,
// from the published "Metis - How it works" guide (hpsdr_protocol.hpp has the provenance and the full list
// of what the guide does not say).
//
// STAND-ALONE, called by nothing yet. Receive works from the frame layer in hpsdr_frame.hpp (the openHPSDR
// "USB Data Protocol" document): the 24-bit I/Q unpacking, the sample-rate, frequency and input-attenuator
// commands (carried to the radio as command-and-control bytes in end-point-2 frames with silent audio and
// MOX always 0: nothing is transmitted). A HpsdrFrameDecoder can still be injected with setFrameDecoder()
// (the plumbing tests use that, with a layout of their own invention); with none, the built-in protocol-1
// decoder and command sender are used. NOTHING HERE HAS BEEN RUN AGAINST A REAL HPSDR RADIO: UNVERIFIED ON
// HARDWARE (none on the desk).
//
// ARGS ("key=value,key=value"):
//   hpsdr=host[:port]   the device (port default 1024). Without it, discovery is broadcast to
//                       255.255.255.255:1024 and the first device that answers (or the one whose MAC
//                       matches mac=) is used.
//   mac=aa:bb:cc:dd:ee:ff   choose among several that answered
//   board=hermes       refuse a radio that answers as another board (metis, hermes, griffin, angelia,
//                       orion, hermes_lite)
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

#include "source/hpsdr_frame.hpp"
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

// What turns the 1024 bytes of a data packet (two 512-byte frames) into samples. Optional: with none set the
// source uses the built-in protocol-1 decoder (hpsdr_frame.hpp) and sends its own control frames; a decoder
// set here replaces both (the source then sends nothing but discovery, start and stop - the plumbing tests).
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

    // Optional (see above); null restores the built-in protocol-1 decoder. Used on the reader thread.
    void setFrameDecoder(std::shared_ptr<HpsdrFrameDecoder> d);

    const char* driverKey() const override { return "hpsdr"; }
    bool open(const std::string& args) override;
    void closeDevice() override;
    bool start() override;
    void stop() override;

    double sampleRateHz() const override;
    // 48, 96, 192 or 384 kHz (the speed field); anything else is refused with a sentence. Takes effect at
    // once while streaming; the samples already queued are at the old rate.
    bool setSampleRateHz(double hz) override;
    double centerFrequencyHz() const override;
    // The receiver's NCO frequency in whole Hz (0 to 4294967295); the radio's own range applies beyond that
    // and the document names none.
    bool setCenterFrequencyHz(double hz) override;

    // One control, "Attenuator" 0 to 31 dB of input attenuation (document: Hermes and Angelia only; none is
    // offered for another board, because the document names none).
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

    // What the device said in discovery.
    HpsdrDevice device() const;
    // Receive accounting since the last start().
    std::uint64_t packetsLost() const;
    std::uint64_t packetsLate() const;
    std::uint64_t badPackets() const;
    // Built-in decoder tally since the last start(): frames, refused frames, samples, per-ADC overflow
    // frames, the software serial number the radio reports.
    hpsdr::StreamTally frameTally() const;
    // Packets of control frames sent to end point 2 since open (0 with an injected decoder).
    std::uint64_t controlPacketsSent() const;

private:
    struct Rx;
    bool applySettingsLocked(const hpsdr::ControlSettings& next);

    mutable std::mutex mutex_;
    hpsdr::ControlSettings settings_;  // what start() and the setters send; 48 kHz and 0 Hz until set
    std::shared_ptr<HpsdrFrameDecoder> decoder_;
    std::shared_ptr<Rx> rx_;
    std::shared_ptr<UdpTransport> udp_;
    HpsdrDevice dev_;
    bool active_ = false;
    int timeoutMs_ = 2000;
};

}  // namespace cascade::source
