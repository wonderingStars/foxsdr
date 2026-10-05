// Fuzz target: the plugin catalogue's index.json, as a stranger's server sends it.
//
// This text is fetched over HTTPS from a fixed origin, but "the origin said so"
// is exactly what plugin_repo.hpp refuses to rely on: the document decides
// which native module is downloaded, so parseIndex is the first of the seven
// rules. The target also runs what consumes a parsed catalogue (the regional
// merge, the update planner) over the entries it produced.
//
// Properties checked beyond "does not fault": a refused document leaves no
// entries behind, and every platform of an accepted one has an https URL and a
// 64-digit lowercase hash - the two rules parseIndex enforces at parse time.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/plugin_repo.hpp"
#include "fuzz_common.hpp"

using cascade::core::InstalledPlugin;
using cascade::core::PluginCatalogEntry;
using cascade::core::PluginRepo;

namespace {

bool isLowerHex64(const std::string& s) {
    if (s.size() != 64) { return false; }
    for (const char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { return false; }
    }
    return true;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string text = fuzz::asString(data, size);

    std::vector<PluginCatalogEntry> entries;
    std::string error;
    const bool ok = PluginRepo::parseIndex(text, entries, error);
    if (!ok) {
        FUZZ_REQUIRE(entries.empty());
        FUZZ_REQUIRE(!error.empty());
        return 0;
    }

    for (const PluginCatalogEntry& e : entries) {
        for (const auto& p : e.platforms) {
            FUZZ_REQUIRE(PluginRepo::isHttpsUrl(p.url));
            FUZZ_REQUIRE(isLowerHex64(p.sha256));
        }
        (void)e.thisPlatform();
    }

    // What a parsed catalogue is then used for.
    const PluginRepo::RegionalMergeResult merged =
        PluginRepo::mergeRegional(entries, entries, "https://example.invalid/regional/");
    FUZZ_REQUIRE(merged.merged.size() >= entries.size());

    std::vector<InstalledPlugin> installed;
    for (const PluginCatalogEntry& e : entries) {
        InstalledPlugin ip;
        ip.id = e.id;
        ip.name = e.name;
        ip.version = e.minSupportedVersion.empty() ? "0.0.1" : e.minSupportedVersion;
        ip.file = "x";
        ip.abiVersion = e.abiVersion;
        installed.push_back(std::move(ip));
    }
    const std::vector<cascade::core::PluginUpdate> updates =
        PluginRepo::planUpdates(entries, installed);
    for (const auto& u : updates) { FUZZ_REQUIRE(u.entry != nullptr); }

    std::vector<cascade::core::CachedPolicy> policies;
    PluginRepo::mergePolicies(policies, entries);
    (void)PluginRepo::blockedPlugins(installed, policies);
    return 0;
}
