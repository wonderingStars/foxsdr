// fobos_source.hpp - Fobos SDR (RigExpert) through the maker's own library, which the USER has installed (run-time bridge).
//
// FoxSDR ships no Fobos file and fetches none. open() looks for the library in the folder or file named by `lib=` or, when none is given, in the PATH
// directories under each name in fobos_api.hpp (kLibraryNames: the maker's header names no file, so these are guesses and the page says so), checks that
// the file is a library this process can load (vendor_lib.hpp), loads it silently, resolves only the calls it makes (fobos_api.hpp) and says in one
// sentence which step failed. OFF until the person picks the source; nothing here runs at start-up.
//
// RECEIVE ONLY. The calls that write firmware, read it out, reset the board or drive the output pins are never resolved (fobos_api.hpp kForbidden).
//
// THREADS. Every control call goes through ONE thread owned by this object (vendor_apartment.hpp), with a bounded wait; a call that does not return marks
// the radio dead and the library is deliberately never unloaded. The maker's header does not say whether the streaming call returns at once or blocks
// until it is cancelled, so it is made on a thread of its own and both are handled: a return with an error at start is a refused start, a return after
// the stream was running is an ended stream (the silence watchdog and the return code mark the radio dead). The library's callback runs on its own thread
// and only writes the ring through a gate that is closed whenever the stream is not wanted.
//
// SAMPLE COUNT. The header does not say whether the length handed to the callback counts floats or complex samples. Reading too many would read past
// the library's buffer, so the default is the safe one (floats: never more than the length), and `count=pairs` reads two floats per unit of the length.
// A stream whose rate is about half the one asked for says so in streamNote() so the person can set it. UNVERIFIED on hardware.
//
// ARGS ("key=value,key=value"):
//   lib=<folder or file>   where the library is (default: the PATH directories)
//   serial=<text>          which board (default: the first listed)      index=<n>  by position in the list
//   rate=<Hz>              the listed rate nearest this (default nearest 2000000)
//   count=floats|pairs     what the stream buffer length counts (default floats)
//   direct=on|off          direct sampling, only sent when given        clock=internal|external   only sent when given
//   timeout=<ms>           the bound on any one library call (default 5000)    silence=<ms>  a stream silent this long is a fault (default 5000)
//
// UNVERIFIED ON HARDWARE (no Fobos on the desk): every behaviour of the real library. The stand-in library in tests/fixtures was written from the same
// header, so it cannot show that the reading was right.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_SOURCE_FOBOS_SOURCE_HPP
#define CASCADE_SOURCE_FOBOS_SOURCE_HPP

#include <memory>
#include <string>
#include <vector>

#include "source/device_source.hpp"
#include "source/vendor_lib.hpp"

namespace cascade::source {

struct FobosStats {
    std::uint64_t callbacks = 0;
    std::uint64_t samplePairs = 0;
    std::uint64_t pairsDropped = 0;
    std::uint64_t badCallbacks = 0;
};

class FobosSource : public DeviceSource {
public:
    FobosSource();
    ~FobosSource() override;
    FobosSource(const FobosSource&) = delete;
    FobosSource& operator=(const FobosSource&) = delete;

    const char* driverKey() const override { return "fobos"; }
    bool open(const std::string& args) override;
    void closeDevice() override;
    bool isOpen() const override;

    bool start() override;
    void stop() override;
    bool running() const override;
    bool selfPaced() const override { return true; }
    std::size_t read(std::complex<float>* dst, std::size_t n) override;
    bool faulted() const override;
    const char* name() const override { return "Fobos SDR"; }
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
    bool frequencyRangeHz(double&, double&) const override { return false; }
    bool deviceDead() const override;
    std::string faultedWhile() const override;

    // Why the last open() failed, as a word (vendorlib::verdictWord) or "" when it did not.
    std::string openVerdictWord() const;
    FobosStats stats() const;
    // The serial of the open board, masked to its last four characters.
    std::string serialMasked() const;
    // "" or one sentence: the stream's rate looked like half what was asked for (see SAMPLE COUNT).
    std::string streamNote() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace cascade::source

#endif  // CASCADE_SOURCE_FOBOS_SOURCE_HPP
