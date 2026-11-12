// hpsdr1_source.hpp - FoxSDR's client for openHPSDR protocol-1 network radios (Metis, Hermes, Angelia,
// Orion boards and the Red Pitaya SDR receiver / transceiver, which presents itself as a Hermes radio): UDP
// discovery, start/stop, the real frame layout (hpsdr1_frames.hpp), tuning, sample rate and attenuator.
// Receive only: the transmit audio and I/Q fields are always zero, MOX is never set, drive is 0.
//
// It reuses the discovery, start/stop and data-packet envelope of hpsdr_protocol.hpp / hpsdr_source.hpp
// (the earlier lane's Metis work) and adds what that lane could not build, the frame contents. The earlier
// HpsdrSource is untouched (its tests still prove its own contract: no decoder, no start).
//
// UNVERIFIED ON HARDWARE: no HPSDR radio and no Red Pitaya is on the desk; the stand-in in
// tests/test_hpsdr1_source.cpp is written from the same documents. See hpsdr1_frames.hpp for the exact
// list of hypotheses (duplex, refresh interval, sample full scale, attenuator step).
//
// ARGS ("key=value,key=value"):
//   hpsdr1=host[:port]     the radio (port default 1024); without it, broadcast discovery
//   mac=aa:bb:cc:dd:ee:ff  choose among several that answered
//   profile=generic|redpitaya   generic: 48/96/192/384 kHz, an attenuator 0..31 dB, tuning range not
//                          stated by the document; redpitaya: 48/96/192 kHz, 0 .. 61.44 MHz, no gain
//                          control (both numbers are Pavel Demin's page for the Red Pitaya, not a
//                          manufacturer's specification)
//   rate=Hz                48000, 96000, 192000 (or 384000 for generic); default 192000
//   timeout=ms             the discovery window and the bound on silence (default 2000)
//
// THREADS: the base class's one reader thread; it also sends the periodic command refresh.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "source/hl2_protocol.hpp"
#include "source/hpsdr1_frames.hpp"
#include "source/hpsdr_source.hpp"
#include "source/net_sample_source.hpp"

namespace cascade::source {

enum class Hpsdr1Profile { Generic, RedPitaya, HermesLite2 };

class Hpsdr1Source : public NetSampleSource {
public:
    Hpsdr1Source() = default;
    ~Hpsdr1Source() override;

    const char* driverKey() const override { return "hpsdr1"; }
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

    HpsdrDevice device() const;
    Hpsdr1Profile profile() const;
    // Accounting since the last start().
    std::uint64_t packetsLost() const;
    std::uint64_t packetsLate() const;
    std::uint64_t badPackets() const;      // wrong envelope, wrong sync, wrong length
    std::uint64_t overloadFrames() const;  // frames whose status said "A/D overflow"
    std::uint64_t commandPacketsSent() const;
    // Hermes-Lite 2 profile only (profile=hl2): the extended discovery reply of the radio that was opened, and the
    // latest status words its data frames carried (raw counts). Empty / zero for other profiles.
    hl2::Info hl2Info() const;
    hl2::Telemetry hl2Telemetry() const;

private:
    struct Rx;

    mutable std::mutex mutex_;
    std::shared_ptr<Rx> rx_;
    std::shared_ptr<UdpTransport> udp_;
    HpsdrDevice dev_;
    Hpsdr1Profile profile_ = Hpsdr1Profile::Generic;
    bool active_ = false;
    int timeoutMs_ = 2000;
    double freqHz_ = 0.0;
    bool hl2WatchdogOff_ = false;
    hl2::Info hl2Info_;
};

}  // namespace cascade::source
