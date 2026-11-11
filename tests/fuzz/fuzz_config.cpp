// Fuzz target: config.json, the file every launch reads.
//
// The loader is documented as repair-not-reject: a damaged field keeps its
// default and the rest load, whatever a hand edit, an old version, a cloud-sync
// conflict or a half-written file left behind. That makes it the broadest
// parser in the program (well over a hundred fields, most with their own range
// rules), and the one whose failure costs the most: a fault here is a fault at
// start-up, every start-up, until the user finds the file.
//
// Properties: load always leaves a usable config; what it produced serialises
// to valid JSON; and that JSON loads back to something that serialises
// identically (a repaired value is a fixed point, not a second repair).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstddef>
#include <cstdint>
#include <string>

#include "core/config.hpp"
#include "fuzz_common.hpp"

using cascade::core::AppConfig;
using cascade::core::ConfigStore;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string path = fuzz::writeScratch("config.json", data, size);

    AppConfig first;
    std::string error;
    (void)ConfigStore::load(path, first, error);

    const std::string written = ConfigStore::serialize(first);
    FUZZ_REQUIRE(!written.empty());

    const std::string path2 = fuzz::writeScratch(
        "config2.json", reinterpret_cast<const std::uint8_t*>(written.data()), written.size());
    AppConfig second;
    std::string error2;
    FUZZ_REQUIRE(ConfigStore::load(path2, second, error2));
    FUZZ_REQUIRE(ConfigStore::serialize(second) == written);

    // RADIOS 2 TO 8 AND EACH PLUGIN'S RADIO (multi-radio stage 5): whatever the file said, at most seven radios survive, each with its own number from 2 to 8, every value in
    // its range, a kind that can be a second radio; and a plugin's radio is a number a radio can have.
    FUZZ_REQUIRE(first.radios.size() <= static_cast<std::size_t>(cascade::core::kMaxSavedRadios));
    int seen = 0;
    for (const cascade::core::SavedRadio& r : first.radios) {
        FUZZ_REQUIRE(r.number >= 2 && r.number <= 8 && (seen & (1 << r.number)) == 0);
        seen |= 1 << r.number;
        FUZZ_REQUIRE(r.volume >= 0.0 && r.volume <= 1.0 && r.squelchDb >= -200.0 && r.squelchDb <= 20.0);
        FUZZ_REQUIRE(r.agcMode >= 0 && r.agcMode <= 4 && r.agcFixedGainDb >= -20.0 && r.agcFixedGainDb <= 40.0);
        FUZZ_REQUIRE(r.nbThresholdDb >= 6.0 && r.nbThresholdDb <= 30.0 && r.notchFreqHz >= 10.0 && r.notchFreqHz <= 20000.0);
        FUZZ_REQUIRE(r.audioRoute >= 0 && r.audioRoute <= 3 && r.deemphasisIndex >= 0 && r.deemphasisIndex <= 2);
        FUZZ_REQUIRE(r.sampleRateHz >= 0.0 && r.centerHz >= 0.0 && r.bandwidthHz >= 0.0 && r.args.size() <= 512u);
        FUZZ_REQUIRE(r.sourceKind != "soundcard" && r.sourceKind != "row" && r.sourceKind != "open-pluto");   // (file, pluto and rtltcp are kept: stage 7)
    }
    for (const auto& kv : first.pluginRadio) { FUZZ_REQUIRE(kv.second >= 1 && kv.second <= 8 && !kv.first.empty()); }
    return 0;
}
