// aaronia_source.hpp - FoxSDR's client for the HTTP stream server of Aaronia's RTSA-Suite, the way a
// SPECTRAN V6 (and other Aaronia receivers driven by the suite on this or another machine) hands I/Q to
// other programs (survey row R7, "Aaronia"). Plain HTTP over TCP; no Aaronia library or driver.
//
// STAND-ALONE, called by nothing yet. The interface and its provenance, and the list of what Aaronia's
// document does not say, are in aaronia_protocol.hpp. UNVERIFIED ON HARDWARE AND AGAINST THE REAL SERVER: no
// SPECTRAN is on the desk, and the stand-in in tests/test_aaronia_source.cpp is written from the same
// document. Authorisation is NOT built: a server that asks for Basic or RToken credentials is refused with a
// sentence saying so (a password or token is a secret; where FoxSDR keeps one is a decision for the wiring
// lane, and none is ever logged here).
//
// ARGS ("key=value,key=value"):
//   aaronia=host[:port]    the RTSA HTTP server block (port default 54664, the document's example)
//   format=json|float32|int16   the sample format asked for: json (default; the layout is fully described),
//                          float32 or int16 (binary: the record delimiting and the int16 scale are those of the
//                          MIT-licensed reference client, see aaronia_protocol.hpp)
//   control=capture|remoteconfig   how the centre and rate are set: "capture" (default) PUTs /control as the
//                          manufacturer's document describes; "remoteconfig" PUTs /remoteconfig to the named blocks
//                          of the user's mission as the reference client does, and sends no start/stop of its own
//   demod=NAME             the IQ demodulator block (remoteconfig only; default Block_IQDemodulator_1)
//   spectran=NAME          the SPECTRAN block whose centre follows (default Block_Spectran_V6B_1; empty: none)
//   spectranoffset=Hz      added to the centre sent to the SPECTRAN block (default 0)
//   rate=Hz                the capture span / I/Q rate to ask for at start (the document gives no rate
//                          control for I/Q other than the span: that frequencySpan sets the rate is a
//                          hypothesis)
//   timeout=ms             the bound on a request and its answer, and on silence (default 3000)
//
// open(): GET /info (the proof it is an RTSA server: a JSON object with a "name"). start(): PUT /control
// {capture: centre, span} when either was set, PUT /control {streaming: start}, then GET /stream (chunked),
// whose I/Q packets are decoded on the reader thread. stop(): the reader is ended, the stream closed, PUT
// /control {streaming: stop} (best effort). The reader is started per start() and ended per stop().
// The reported sample rate is measured (pairs / (endTime - startTime)) once a packet has arrived, else the
// span asked for. The server drops data when its 8 MByte send buffer fills: gaps between packet timestamps
// are counted (gapCount()).
//
// THREADS: the base class's one reader thread, per start().
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "source/aaronia_protocol.hpp"
#include "source/net_sample_source.hpp"

namespace cascade::source {

class AaroniaSource : public NetSampleSource {
public:
    AaroniaSource() = default;
    ~AaroniaSource() override;

    const char* driverKey() const override { return "aaronia"; }
    bool open(const std::string& args) override;
    void closeDevice() override;
    bool start() override;
    void stop() override;

    double sampleRateHz() const override;
    bool setSampleRateHz(double hz) override;
    double centerFrequencyHz() const override;
    bool setCenterFrequencyHz(double hz) override;

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

    std::string serverName() const;
    std::uint64_t gapCount() const;        // packets whose start time is later than the previous end time
    std::uint64_t otherPackets() const;    // non-I/Q packets seen and skipped
    std::uint64_t nonFiniteSamples() const;

private:
    struct Stream;

    bool failOpen(const std::string& why);
    bool control(const std::string& json, const char* what, const char* path = "/control");
    bool sendCapture();
    void closeStream();

    mutable std::mutex ctlMutex_;
    std::string host_;
    std::uint16_t port_ = 0;
    int timeoutMs_ = 3000;
    aaronia::SampleFormat format_ = aaronia::SampleFormat::Json;
    bool remoteConfig_ = false;  // control=remoteconfig: the reference client's way of setting centre and rate
    std::string demodBlock_, spectranBlock_;
    double spectranOffsetHz_ = 0.0;
    long long requestCounter_ = 0;
    std::string serverName_;
    double centerHz_ = 0.0, spanHz_ = 0.0;
    bool captureDirty_ = false;
    bool active_ = false;
    std::shared_ptr<Stream> stream_;
};

}  // namespace cascade::source
