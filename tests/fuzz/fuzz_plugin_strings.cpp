// Fuzz target: the small string checks that guard a plugin download.
//
// sanitiseFileName is rule 6 of the catalogue's seven (nothing but a bare file
// name may reach the filesystem), isHttpsUrl is rule 1, compareVersions decides
// whether a catalogue's build replaces an installed one, and the regional-URL
// check decides whether an entry from the second catalogue is believed. All of
// them take text a stranger wrote.
//
// Properties: a name sanitiseFileName accepts is returned unchanged, holds no
// path separator, no NUL, no "..", is within the length cap, and is not a
// reserved device name; compareVersions is antisymmetric and reflexive.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstddef>
#include <cstdint>
#include <string>

#include "core/plugin_repo.hpp"
#include "core/updater.hpp"
#include "fuzz_common.hpp"

using cascade::core::PluginRepo;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    fuzz::Bytes in(data, size);
    // The first byte picks how the input is split, so one input exercises two
    // strings at once (the comparison functions take a pair).
    const std::size_t cut = in.u8();
    const std::string a = in.str(cut);
    const std::string b = in.rest();

    for (const std::string* s : {&a, &b}) {
        std::string out;
        std::string error;
        if (PluginRepo::sanitiseFileName(*s, out, error)) {
            FUZZ_REQUIRE(out == *s);
            FUZZ_REQUIRE(!out.empty());
            FUZZ_REQUIRE(out.size() <= PluginRepo::kMaxFileNameChars);
            FUZZ_REQUIRE(out.find('/') == std::string::npos);
            FUZZ_REQUIRE(out.find('\\') == std::string::npos);
            FUZZ_REQUIRE(out.find('\0') == std::string::npos);
            FUZZ_REQUIRE(out.find("..") == std::string::npos);
        } else {
            FUZZ_REQUIRE(out.empty());
            FUZZ_REQUIRE(!error.empty());
        }
        (void)PluginRepo::isHttpsUrl(*s);
        (void)PluginRepo::isRegionalDownloadUrl(*s, a);
        (void)PluginRepo::regionalWanted(*s, cut & 1);
        (void)cascade::core::wellFormedVersion(*s);
    }

    const int ab = PluginRepo::compareVersions(a, b);
    const int ba = PluginRepo::compareVersions(b, a);
    FUZZ_REQUIRE(PluginRepo::compareVersions(a, a) == 0);
    FUZZ_REQUIRE((ab < 0) == (ba > 0));
    FUZZ_REQUIRE((ab == 0) == (ba == 0));

    const int uab = cascade::core::compareVersions(a, b);
    const int uba = cascade::core::compareVersions(b, a);
    FUZZ_REQUIRE((uab < 0) == (uba > 0));

    (void)PluginRepo::sha256Matches(a, b);
    return 0;
}
