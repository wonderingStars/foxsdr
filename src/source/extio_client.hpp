// source/extio_client.hpp - FoxSDR's side of the ExtIO helper process (row R8): starts the helper for the DLL the user chose, talks to it over a
// named pipe (source/extio_protocol.hpp) and turns what comes back into samples and refusal FACTS. The application never loads the DLL.
//
// OFF UNTIL CHOSEN. With an empty DLL path nothing starts and open() answers kFactNotChosen. The screen that lets a person pick a DLL must show
// kExtioWarning (the driver runs with the user's rights).
//
// WHAT IT SURVIVES (each a test with a stand-in driver written for it, tests/fixtures/extio_standin.cpp): a driver that crashes (the helper dies:
// kFactHelperExited with the exit code), hangs (a command that does not answer: kFactTimeout, the helper is killed), floods (samples far faster
// than its own sample rate: kFactFlood), returns nonsense sizes from StartHW (kFactBadSize), hands the callback a bad pointer (counted, not fatal),
// goes silent after starting (kFactSilent), lacks entry points or refuses InitHW (kFactDllRefused), returns samples through the sound card
// (kFactUnsupportedType), or is built for the other bitness (picks the helper by the DLL's PE header; kFactHelperMissing when that helper is not
// installed). The helper is in a job object that kills it with this process.
//
// PRIVACY. Facts carry no path and no file name. The driver's own name and model (which may be a serial number) are kept in memory for the
// screen and never written anywhere by this class.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_SOURCE_EXTIO_CLIENT_HPP
#define CASCADE_SOURCE_EXTIO_CLIENT_HPP

#include <atomic>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "source/extio_protocol.hpp"
#include "source/iq_source.hpp"

namespace cascade::source {

struct ExtioFact {
    std::string code;  // one of the extio::kFact* words
    std::string text;  // one sentence, no path
};

struct ExtioOptions {
    std::string dllPath;        // UTF-8; EMPTY = off
    std::string helperPath64;   // UTF-8 path of the 64-bit helper
    std::string helperPath32;   // UTF-8 path of the 32-bit helper ("" = none installed)
    int initTimeoutMs = 20000;  // load + InitHW + OpenHW (a driver may open a USB device or show a window)
    int commandTimeoutMs = 5000;
    int silenceMs = 5000;       // a started driver that delivers nothing for this long is a fault
    std::size_t ringPairs = 1u << 20;
    double maxRateFactor = 4.0; // incoming pairs per second above this times the driver's own sample rate is a flood
    double absoluteMaxRateHz = 60.0e6;   // the flood limit when the driver reports no sample rate
};

struct ExtioStats {
    std::uint64_t pairsReceived = 0;
    std::uint64_t pairsRead = 0;
    std::uint64_t pairsOverrun = 0;       // dropped here because the ring was full
    std::uint64_t framesBad = 0;          // Data frames whose count disagreed with their size
    std::uint32_t helperDropped = 0;      // callbacks the helper dropped (its queue was full)
    std::uint32_t helperBadCallbacks = 0; // callbacks with a null or unreadable buffer
};

class ExtioClient {
public:
    explicit ExtioClient(ExtioOptions options);
    ~ExtioClient();
    ExtioClient(const ExtioClient&) = delete;
    ExtioClient& operator=(const ExtioClient&) = delete;

    // Starts the helper, loads the driver, InitHW, OpenHW. False with fact() set. Idempotent when open.
    bool open();
    // StartHW(centre). False with fact() set (a refused size, an error, a timeout). The sample rate is known afterwards (sampleRateHz()).
    bool start(double centerHz);
    // SetHWLO. 0: done; negative: below the radio's minimum (its absolute value); positive: above its maximum; INT32_MIN: no answer / fault.
    std::int32_t setCenterHz(double hz);
    void stop();    // StopHW; keeps the driver open
    void close();   // CloseHW, the helper ends; idempotent

    // Pairs waiting, copied out (never blocks; 0 when none). Samples are finite and in [-1, 1].
    std::size_t read(std::complex<float>* dst, std::size_t n);

    bool isOpen() const;
    bool running() const;
    bool faulted() const;                 // latched: a fact was raised that ends the session
    ExtioFact fact() const;               // the first (root) fact, empty code when none
    std::vector<ExtioFact> facts() const; // every fact, in order
    double sampleRateHz() const;          // 0 until the driver says
    std::string driverName() const;       // in memory only
    std::string driverModel() const;
    std::uint32_t hwType() const;
    ExtioStats stats() const;
    std::vector<extio::StatusEvent> takeStatusEvents();   // the driver's own status callbacks (100 sample-rate change, 101 LO change ...)
    bool helperAlive() const;
    std::uint32_t helperExitCode() const;                 // valid once the helper has ended

    // The Machine field of a PE file (0x14C 32-bit, 0x8664 64-bit, 0xAA64 arm64), 0 when `path` is not a PE image. Reads 4 KiB.
    static std::uint32_t peMachine(const std::string& utf8Path);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The engine-facing source. start() opens the driver on first use. The sample rate is the driver's; setSampleRateHz() is refused ("set it in the
// driver's own window", which the screen opens with showDriverWindow()).
class ExtioSource : public IqSource {
public:
    explicit ExtioSource(ExtioOptions options);
    bool start() override;
    void stop() override;
    bool running() const override;
    bool selfPaced() const override { return true; }
    double sampleRateHz() const override;
    bool setSampleRateHz(double hz) override;
    double centerFrequencyHz() const override { return centerHz_; }
    bool setCenterFrequencyHz(double hz) override;
    std::size_t read(std::complex<float>* dst, std::size_t n) override;
    bool faulted() const override;
    const char* name() const override { return name_.c_str(); }
    const char* lastError() const override { return lastError_.c_str(); }
    ExtioClient& client() { return client_; }

private:
    ExtioClient client_;
    double centerHz_ = 100.0e6;
    std::string name_ = "ExtIO driver";
    std::string lastError_;
};

}  // namespace cascade::source

#endif  // CASCADE_SOURCE_EXTIO_CLIENT_HPP
