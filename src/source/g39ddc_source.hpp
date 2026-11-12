// g39ddc_source.hpp - WiNRADiO G39DDC receivers through the maker's own library, G39DDCAPI.dll, which the USER has installed (run-time bridge).
//
// FoxSDR ships no WiNRADiO file. open() looks for G39DDCAPI.dll in the folder or file named by `lib=` or, when none is given, in the PATH
// directories; checks that the file is a library this process can load (vendor_lib.hpp: a 32-bit file in this 64-bit program is named as such and
// left alone); loads it silently; resolves the thirteen calls it needs (g39ddc_api.hpp, declared from the maker's published document); and says in
// one sentence which of those steps failed. OFF until the person picks the source; nothing here runs at start-up.
//
// WHAT IT STREAMS. DDC1 of one channel (0 or 1): 16- or 32-bit signed I/Q at the DDC type's sample rate, delivered by the library on a thread of its
// own to a callback that converts it and writes it to a ring; read() drains the ring on the pipeline's source thread. Tuning sets the analog
// front end to the nearest multiple of its tuning step and puts the rest on DDC1's relative frequency, so the centre the person asked for is the
// centre of the stream (the document's formula: absolute DDC1 frequency = front end + relative DDC1).
//
// THE ONE-THREAD RULE. The document says a receiver "can be controlled from a single user thread only" and that the handle returned by OpenDevice
// "can only be used in the same thread". FoxSDR calls come from the GUI thread, the control thread and the pipeline, so every library call is made
// on ONE thread owned by this object (the "apartment"): the caller hands it a task and waits a bounded time for the answer. A library call that
// does not return in time marks the receiver dead (faulted()), the apartment is abandoned and the library is deliberately never unloaded.
// Calls into the library are also run under callGuardingVendorFaults, so an access violation inside the vendor's code is a refusal, not a crash.
//
// ARGS ("key=value,key=value"):
//   lib=<folder or file>   where G39DDCAPI.dll is (default: the PATH directories)
//   serial=<9 chars>       which receiver (default: the first the library lists)
//   channel=0|1            which DDC channel (default 0)
//   rate=<Hz>              the DDC type nearest this rate (default: nearest 192000)
//   timeout=<ms>           the bound on any one library call (default 5000)
//   silence=<ms>           a streaming receiver silent this long is a fault (default 5000)
//
// UNVERIFIED ON HARDWARE (no WiNRADiO on the desk): every behaviour of the real library. The stand-in library in tests/fixtures was written from the
// same document, so it cannot show the reading was right.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_SOURCE_G39DDC_SOURCE_HPP
#define CASCADE_SOURCE_G39DDC_SOURCE_HPP

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "source/device_source.hpp"
#include "source/vendor_lib.hpp"

namespace cascade::source {

struct G39DdcStats {
    std::uint64_t callbacks = 0;       // stream callbacks received
    std::uint64_t samplePairs = 0;     // pairs written to the ring
    std::uint64_t pairsDropped = 0;    // dropped because the ring was full
    std::uint64_t badCallbacks = 0;    // callbacks with an impossible argument (dropped, counted)
};

// Pure helpers, tested alone.
// The front-end frequency and DDC1 offset that make `centerHz` the centre of the stream, or false with a sentence.
struct G39Tune {
    std::uint64_t frontEndHz = 0;
    std::int32_t ddc1Hz = 0;
};
bool g39PlanTune(double centerHz, std::uint64_t minHz, std::uint64_t maxHz, std::uint32_t stepHz, std::uint32_t windowHz, std::uint32_t ddcBandwidthHz,
                 G39Tune& out, std::string& why);

class G39DdcSource : public DeviceSource {
public:
    G39DdcSource();
    ~G39DdcSource() override;
    G39DdcSource(const G39DdcSource&) = delete;
    G39DdcSource& operator=(const G39DdcSource&) = delete;

    const char* driverKey() const override { return "g39ddc"; }
    bool open(const std::string& args) override;
    void closeDevice() override;
    bool isOpen() const override;

    bool start() override;
    void stop() override;
    bool running() const override;
    bool selfPaced() const override { return true; }
    std::size_t read(std::complex<float>* dst, std::size_t n) override;
    bool faulted() const override;
    const char* name() const override { return "WiNRADiO G39DDC"; }
    const char* lastError() const override;

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
    std::vector<std::string> antennas() const override { return {"RF"}; }
    bool setAntenna(const std::string& name) override { return name == "RF"; }
    std::string antenna() const override { return "RF"; }
    std::vector<double> supportedSampleRatesHz() const override;
    bool frequencyRangeHz(double& loHz, double& hiHz) const override;
    bool deviceDead() const override;
    std::string faultedWhile() const override;

    // Why the last open() failed, as a word (vendorlib::verdictWord) or "" when it did not.
    std::string openVerdictWord() const;
    G39DdcStats stats() const;
    // The serial number of the open receiver, masked to its last four characters.
    std::string serialMasked() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace cascade::source

#endif  // CASCADE_SOURCE_G39DDC_SOURCE_HPP
