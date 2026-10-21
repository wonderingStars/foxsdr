// Fuzz target: the installed-plugins manifest (plugins/installed.json).
//
// A file on disk that an installer, a user or a bad shutdown can have written,
// read at start-up before any plugin is loaded. Its file names are joined to
// the plugins directory afterwards, so what parseManifest lets through is what
// the host will later pass to LoadLibrary.
//
// Properties: every record that survives names a file sanitiseFileName
// accepts; ids are unique; a document that is serialised and parsed again keeps
// the same records.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "core/plugin_repo.hpp"
#include "fuzz_common.hpp"

using cascade::core::CachedPolicy;
using cascade::core::InstalledPlugin;
using cascade::core::PluginRepo;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string text = fuzz::asString(data, size);

    std::vector<InstalledPlugin> plugins;
    std::vector<CachedPolicy> policies;
    std::vector<std::string> notes;
    std::string error;
    if (!PluginRepo::parseManifest(text, plugins, policies, notes, error)) {
        FUZZ_REQUIRE(!error.empty());
        return 0;
    }

    std::set<std::string> ids;
    for (const InstalledPlugin& p : plugins) {
        std::string safe;
        std::string why;
        FUZZ_REQUIRE(PluginRepo::sanitiseFileName(p.file, safe, why));
        FUZZ_REQUIRE(ids.insert(p.id).second);
    }

    const std::string again = PluginRepo::serialiseManifest(plugins, policies);
    std::vector<InstalledPlugin> plugins2;
    std::vector<CachedPolicy> policies2;
    std::vector<std::string> notes2;
    std::string error2;
    FUZZ_REQUIRE(PluginRepo::parseManifest(again, plugins2, policies2, notes2, error2));
    FUZZ_REQUIRE(plugins2.size() == plugins.size());
    FUZZ_REQUIRE(policies2.size() == policies.size());
    for (std::size_t i = 0; i < plugins.size(); ++i) {
        FUZZ_REQUIRE(plugins2[i].id == plugins[i].id);
        FUZZ_REQUIRE(plugins2[i].file == plugins[i].file);
        FUZZ_REQUIRE(plugins2[i].version == plugins[i].version);
    }

    for (const CachedPolicy& pol : policies) {
        for (const InstalledPlugin& p : plugins) {
            (void)PluginRepo::pluginBlockReason(p, pol);
            (void)PluginRepo::pluginBlockMessage(p, pol);
        }
    }
    (void)PluginRepo::blockedPlugins(plugins, policies);
    return 0;
}
