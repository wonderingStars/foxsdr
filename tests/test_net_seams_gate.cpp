// Tests for the product-run gate on the three network test seams (security review R1, finding F4, 0.99.114).
//
//   FOXSDR_UPDATE_URL               the update service's address (core/updater.hpp updateEndpoint)
//   FOXSDR_FAKE_UPDATE_MANIFEST     a file that answers the update check instead of the network (updateManifestFakeSet, checkForUpdate)
//   FOXSDR_REGIONAL_CATALOGUE_URL   the regional plugin list's address or local path (PluginRepo::regionalIndexUrl / regionalOverrideSet)
//
// They exist for the bounded runs the application tests and the drive scripts make (`--frames`, `--seconds`). A PRODUCT run calls
// lockFakePackageSeams() at start (engine/process_boot.cpp, the L3 gate the fake-package seam already uses) and must from then on ignore them: a variable a person or a
// program left in the environment must not choose who answers the update check or the plugin list. LOCKING IS ONE WAY, so this file is its own process: first it proves the
// seams still work while open (what every bounded run and the existing tests rely on), then it locks and proves they are gone.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#define TEST_GETPID _getpid
#else
#include <unistd.h>
#define TEST_GETPID getpid
#endif

#include "core/package_identity.hpp"
#include "core/plugin_repo.hpp"
#include "core/updater.hpp"
#include "test_check.hpp"

namespace {

namespace fs = std::filesystem;
using cascade::core::PluginRepo;
using cascade::core::UpdateInfo;

void setEnvVar(const char* name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    if (value.empty()) { ::unsetenv(name); } else { ::setenv(name, value.c_str(), 1); }
#endif
}

const char kAttackerUpdate[] = "https://attacker.example.invalid/api/update";
const char kAttackerRegional[] = "https://attacker.example.invalid/regional";
const char kRealUpdate[] = "https://foxsdr.com/api/update";
const char kRealRegional[] = "https://foxsdr.com/api/plugins/regional";

}  // namespace

int main() {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path() / ("foxsdr-test-net-seams-" + std::to_string(TEST_GETPID()));
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path canned = dir / "manifest.json";
    {
        std::ofstream f(canned, std::ios::binary | std::ios::trunc);
        f << "{\"version\":\"9.9.9\",\"url\":\"https://foxsdr.com/download/u/foxsdr-setup-9.9.9.exe\","
             "\"sha256\":\"393e5fd91b7b2292611c52af7e9f2db1e2c730d78f1639adefceaf0fb5cca0ea\"}";
    }
    setEnvVar("FOXSDR_UPDATE_URL", kAttackerUpdate);
    setEnvVar("FOXSDR_FAKE_UPDATE_MANIFEST", canned.string());
    setEnvVar("FOXSDR_REGIONAL_CATALOGUE_URL", kAttackerRegional);

    // OPEN (a bounded run, and every existing test): the three variables are honoured.
    CHECK(cascade::core::fakePackageSeamsOpen());
    CHECK(cascade::core::updateEndpoint() == kAttackerUpdate);
    CHECK(cascade::core::updateManifestFakeSet());
    CHECK(PluginRepo::regionalIndexUrl() == kAttackerRegional);
    CHECK(PluginRepo::regionalOverrideSet());
    {
        UpdateInfo info;
        std::string err;
        CHECK(cascade::core::checkForUpdate("", "0.99.1", "", info, err));   // answered by the file, no endpoint needed
        CHECK(info.version == "9.9.9" && info.newer);
    }

    // LOCKED (a product run): all three are ignored, whatever the environment says.
    cascade::core::lockFakePackageSeams();
    CHECK(!cascade::core::fakePackageSeamsOpen());
    CHECK(cascade::core::updateEndpoint() == kRealUpdate);
    CHECK(!cascade::core::updateManifestFakeSet());
    CHECK(PluginRepo::regionalIndexUrl() == kRealRegional);
    CHECK(!PluginRepo::regionalOverrideSet());
    {
        // the canned file is not read: with no endpoint the ordinary refusal comes back
        UpdateInfo info;
        std::string err;
        CHECK(!cascade::core::checkForUpdate("", "0.99.1", "", info, err));
        CHECK(err == "no update endpoint configured");
        CHECK(info.version.empty());
    }
    // with the override ignored the regional list is only wanted for the default catalogue, as in a normal install
    CHECK(PluginRepo::regionalWanted(PluginRepo::defaultIndexUrl(), PluginRepo::regionalOverrideSet()));
    CHECK(!PluginRepo::regionalWanted(kAttackerRegional, PluginRepo::regionalOverrideSet()));

    setEnvVar("FOXSDR_UPDATE_URL", "");
    setEnvVar("FOXSDR_FAKE_UPDATE_MANIFEST", "");
    setEnvVar("FOXSDR_REGIONAL_CATALOGUE_URL", "");
    fs::remove_all(dir, ec);
    return testSummary("test_net_seams_gate");
}
