// kiwisdr_source.hpp - a KiwiSDR receiver on the network as a RADIO of the engine (engine wiring lane 1).
//
// A KiwiSDR sends DEMODULATED audio (12 kHz, mono, the mode the client asked for), not I/Q: the client in kiwisdr_client.hpp is the connection and this
// is the DeviceSource that lets the engine hold it as a radio like any other. What the pipeline is given is the audio as a real signal: I = the sample,
// Q = 0, at 12 kS/s, and the radio's demodulator is kept in RAW (Re{x}), so the audio goes through the radio's own chain (channel filter, AGC, noise
// reduction, squelch, volume, route, recorders) and out of its own sink. The spectrum of such a radio is the audio spectrum, mirrored about zero: it is
// not an RF display (the screen says "audio only").
//
// LISTEN ONLY: nothing here transmits, and the radio's centre frequency is the receiver's own tuned frequency (the VFO offset stays 0: a KiwiSDR
// tunes itself). The MODE the user picks is sent to the receiver (setServerMode), not applied to the local demodulator.
//
// OPEN IS BLOCKING and runs on the engine's open worker (launchDeviceOpen): it connects, sends the opening commands and a first tune, then waits (at
// most kFirstAudioWait) until the receiver has answered with audio or with a refusal. A refusal (too busy, a password, "down", a redirect) or a
// receiver that is not there makes open() return false with a sentence in lastError(): the engine publishes it as the radio's refusal, as it does for
// every other network source. A connection lost later is faulted() with the client's sentence.
//
// THE ARGS are "kiwisdr=<host>[:<port>]" (port 8073 by default). A password is not part of this release (a receiver that wants one is refused in
// words); the address is the only thing saved, and it never reaches a usage report (the diagnostics log hides "kiwisdr=" like "rtltcp=").
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <chrono>
#include <complex>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "source/device_source.hpp"
#include "source/kiwisdr_client.hpp"

namespace cascade::source {

class KiwiSdrSource : public DeviceSource {
public:
    // The driver key: the radio kind word ("kiwisdr") in the saved settings and the radio's `source` fact.
    static constexpr const char* kDriverKey = "kiwisdr";
    // The longest open() waits for the receiver's first word (audio or a refusal) after the upgrade, on top of the client's own connect bound.
    static constexpr std::chrono::milliseconds kFirstAudioWait{4000};
    // A read() waits at most this long for audio, so the source thread can see a stop.
    static constexpr std::chrono::milliseconds kReadWait{20};
    static constexpr double kDefaultFrequencyHz = 10.0e6;

    struct Address {
        std::string host;
        std::uint16_t port = 8073;
    };
    // "kiwisdr=host[:port]" or the bare "host[:port]". False (and `why` set) for an empty host, a port outside 1..65535 or a character a host name
    // does not hold (control characters, spaces, slashes, '@', '#', '?' and '%': a name, never a URL).
    static bool parseArgs(const std::string& args, Address& out, std::string& why);

    // A timing override for the tests (a stand-in server on this machine answers at once; a stalled one is tested with short bounds).
    struct Timing {
        std::chrono::milliseconds connectWait = KiwiSdrClient::kConnectWait;
        std::chrono::milliseconds firstAudioWait = kFirstAudioWait;
        std::chrono::milliseconds stallWait = KiwiSdrClient::kStallWait;
    };

    KiwiSdrSource();
    explicit KiwiSdrSource(const Timing& timing);
    ~KiwiSdrSource() override;

    // --- IqSource ---
    bool start() override;
    void stop() override;
    bool running() const override;
    bool selfPaced() const override { return true; }
    double sampleRateHz() const override;
    bool setSampleRateHz(double hz) override;
    double centerFrequencyHz() const override;
    bool setCenterFrequencyHz(double hz) override;
    std::size_t read(std::complex<float>* dst, std::size_t n) override;
    bool faulted() const override;
    const char* name() const override;
    const char* lastError() const override;

    // --- DeviceSource ---
    const char* driverKey() const override { return kDriverKey; }
    bool open(const std::string& args) override;
    void closeDevice() override;
    bool isOpen() const override;
    std::vector<GainInfo> gains() const override { return {}; }
    bool setGainDb(const std::string&, double) override { return false; }
    double gainDb(const std::string&) const override { return 0.0; }
    bool autoGainSupported() const override { return false; }
    bool setAutoGain(bool) override { return false; }
    bool autoGain() const override { return false; }
    std::vector<std::string> antennas() const override { return {"network"}; }
    bool setAntenna(const std::string& name) override { return name == "network"; }
    std::string antenna() const override { return "network"; }
    std::vector<double> supportedSampleRatesHz() const override;
    bool frequencyRangeHz(double&, double&) const override { return false; }   // receivers differ (a converter or an offset): unknown
    bool deviceDead() const override { return faulted(); }
    std::string faultedWhile() const override;

    // --- the receiver's mode ---
    // The mode word the receiver is asked for and the passband it is given: usb 300..2700, lsb -2700..-300, am -4900..4900 (the AM figure was
    // confirmed by the live test; the rest are the usual defaults: UNVERIFIED on the air), cw 300..800, nbfm -6000..6000. An empty word (a mode the
    // receiver has no word for: WFM, DSB, RAW) is refused and nothing is sent. `engineMode` is the application's index (0 NFM, 1 WFM, 2 AM, 3 DSB,
    // 4 USB, 5 LSB, 6 CW, 7 RAW, as demod.hpp orders them).
    struct ServerMode {
        const char* word = "";
        int lowCutHz = 0;
        int highCutHz = 0;
    };
    static ServerMode serverModeFor(int engineMode);
    bool setServerMode(int engineMode);
    std::string serverMode() const;

    // --- readouts for the engine's facts ---
    bool haveSmeter() const;
    double rssiDbm() const;
    // The client's sentence for the state, and a short word for the facts: "connecting", "playing", "busy", "password", "down", "redirect",
    // "failed", "dropped", "malformed", "closed", "idle".
    std::string stateSentence() const;
    std::string stateWord() const;
    KiwiSdrClient::Status clientStatus() const;
    std::uint64_t droppedSamples() const;

private:
    bool tuneNow();
    void setError(const std::string& s);

    Timing timing_;
    std::unique_ptr<KiwiSdrClient> client_;
    mutable std::mutex mutex_;          // guards the strings below and the tuning values
    std::string error_;
    std::string name_ = "KiwiSDR: (not connected)";
    ServerMode mode_ = {"am", -4900, 4900};
    int engineMode_ = 2;
    double centerHz_ = kDefaultFrequencyHz;
    std::atomic<bool> open_{false};
    std::atomic<bool> running_{false};
    std::vector<std::int16_t> scratch_;
};

}  // namespace cascade::source
