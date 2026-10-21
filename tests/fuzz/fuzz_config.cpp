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
    return 0;
}
