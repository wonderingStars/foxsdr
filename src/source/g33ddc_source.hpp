// g33ddc_source.hpp - WiNRADiO G33DDC receivers through the maker's own library, G33DDCAPI.dll, which the USER has installed (run-time bridge).
//
// The same design as the G39DDC bridge (g39ddc_source.hpp) and the same shared parts (vendor_lib.hpp: find, judge and load the file silently;
// vendor_apartment.hpp: one thread for every library call, bounded waits, the vendor-fault guard, the stream ring). Read those headers for the
// reasons; this one lists only what is different, all of it from the two maker's documents (g33ddc_api.hpp).
//
// DIFFERENCES FROM THE G39DDC. A single DDC1 (no channel number) whose centre frequency IS the receiver's tuning: tuning is one call,
// SetDDC1Frequency(absolute Hz), and the receiver's own range is not stated by the document, so none is claimed - the library refuses a frequency
// it cannot tune and the sentence says so. The device list does not report its record size, so the list is read into a buffer with slack and a
// sentinel and every record is checked for plausibility; a library that fails either check is refused. The DDC1 stream callback carries no channel.
//
// G31DDC: NOT BUILT. WiNRADiO's G31DDC page says its library is a 32-bit dynamic library usable by 32-bit applications (the G33DDC and G39DDC pages
// add that 64-bit applications can use theirs; the G31DDC page does not), and a 32-bit file cannot be loaded into this 64-bit program. The G31DDC
// document's calls are the same as the G33DDC's, so a 64-bit G31DDCAPI.dll, if the maker ever ships one, is a table entry; until then the loader
// would refuse the file with its "32-bit library" sentence.
//
// ARGS ("key=value,key=value"): lib=<folder or file> (default: the PATH directories), serial=<9 chars>, rate=<Hz> (nearest DDC type, default nearest
// 192000), timeout=<ms> (default 5000), silence=<ms> (default 5000).
//
// UNVERIFIED ON HARDWARE (no WiNRADiO on the desk): every behaviour of the real library.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_SOURCE_G33DDC_SOURCE_HPP
#define CASCADE_SOURCE_G33DDC_SOURCE_HPP

#include <memory>
#include <string>
#include <vector>

#include "source/device_source.hpp"
#include "source/vendor_lib.hpp"

namespace cascade::source {

struct G33DdcStats {
    std::uint64_t callbacks = 0;
    std::uint64_t samplePairs = 0;
    std::uint64_t pairsDropped = 0;
    std::uint64_t badCallbacks = 0;
};

class G33DdcSource : public DeviceSource {
public:
    G33DdcSource();
    ~G33DdcSource() override;
    G33DdcSource(const G33DdcSource&) = delete;
    G33DdcSource& operator=(const G33DdcSource&) = delete;

    const char* driverKey() const override { return "g33ddc"; }
    bool open(const std::string& args) override;
    void closeDevice() override;
    bool isOpen() const override;

    bool start() override;
    void stop() override;
    bool running() const override;
    bool selfPaced() const override { return true; }
    std::size_t read(std::complex<float>* dst, std::size_t n) override;
    bool faulted() const override;
    const char* name() const override { return "WiNRADiO G33DDC"; }
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
    bool frequencyRangeHz(double&, double&) const override { return false; }  // the document gives no range
    bool deviceDead() const override;
    std::string faultedWhile() const override;

    std::string openVerdictWord() const;
    G33DdcStats stats() const;
    std::string serialMasked() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace cascade::source

#endif  // CASCADE_SOURCE_G33DDC_SOURCE_HPP
