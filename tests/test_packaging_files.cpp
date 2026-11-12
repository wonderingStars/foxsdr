// Structure checks on the Linux / macOS packaging recipes (packaging/, cmake/linux_package.cmake and the
// two manual-dispatch workflows). None of them has ever been built; these tests hold the properties that
// can be held without a Linux: the new workflows cannot start from a push, the Linux install rules cannot
// run on Windows, the recipe paths agree with the code (udev helper, policy, launchers), and no recipe
// bakes in a secret.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <filesystem>
#include <fstream>
#include <string>

#include "core/udev_rules.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;

namespace {

fs::path root() {
#ifdef CASCADE_SOURCE_DIR
    return CASCADE_SOURCE_DIR;
#else
    return {};
#endif
}

std::string slurp(const std::string& rel) {
    std::ifstream in(root() / rel, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string out;
    for (char c : s) { if (c != '\r') { out += c; } }
    return out;
}

bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

// A top-level `on:` that names nothing but workflow_dispatch: the trigger block is the text between
// "\non:\n" and the next unindented line.
std::string triggerBlock(const std::string& yml) {
    const std::size_t a = yml.find("\non:\n");
    if (a == std::string::npos) { return std::string(); }
    std::size_t b = yml.find("\n\n", a + 1);
    return yml.substr(a, b == std::string::npos ? std::string::npos : b - a);
}

}  // namespace

int main() {
    CHECK(!root().empty());
    for (const char* wf : {".github/workflows/packaging.yml", ".github/workflows/macos-headless.yml"}) {
        const std::string y = slurp(wf);
        CHECK(!y.empty());
        const std::string trig = triggerBlock(y);
        CHECK(has(trig, "workflow_dispatch:"));
        // Nothing but a manual dispatch may start them: existing runs must not change.
        CHECK(!has(trig, "push") && !has(trig, "pull_request") && !has(trig, "schedule") && !has(trig, "workflow_run"));
        CHECK(has(y, "NEVER RUN") || has(y, "NEVER RUN YET"));
    }
    // The existing workflows are untouched by this lane (their trigger block is whatever it was).
    CHECK(has(slurp(".github/workflows/build.yml"), "push:"));

    const std::string pk = slurp("cmake/linux_package.cmake");
    CHECK(has(pk, "if(NOT ((UNIX AND NOT APPLE) OR FOXSDR_FORCE_LINUX_PACKAGING))\n    return()"));
    CHECK(has(pk, "libexec/foxsdr"));  // the helper's directory agrees with core/udev_rules.hpp
    CHECK(std::string(cascade::core::udev::kHelperPath) == "/usr/libexec/foxsdr/foxsdr-udev-helper");
    CHECK(has(pk, "CPACK_PACKAGING_INSTALL_PREFIX \"/usr\""));
    CHECK(has(pk, std::string(cascade::core::udev::kPolicyActionId) + ".policy"));
    CHECK(has(slurp("CMakeLists.txt"), "include(\"${CMAKE_CURRENT_SOURCE_DIR}/cmake/linux_package.cmake\")"));
    // Windows install layout: no install() rule is outside the Linux-only file.
    CHECK(!has(slurp("CMakeLists.txt"), "\ninstall("));

    // The launchers exec the programs by their real path (resources are executable-relative).
    for (const char* l : {"packaging/linux/foxsdr-launcher", "packaging/linux/foxsdr-engine-launcher"}) {
        const std::string s = slurp(l);
        CHECK(s.rfind("#!/bin/sh\n", 0) == 0 && has(s, "readlink -f") && has(s, "exec "));
    }
    CHECK(has(slurp("packaging/linux/foxsdr.desktop"), "Exec=foxsdr\n"));

    // The Docker recipe carries no secret and no default password; the Flatpak names the app id of its file.
    const std::string d = slurp("packaging/docker/Dockerfile");
    CHECK(has(d, "--web-password-file") && !has(d, "--web-password=") && !has(d, "ENV "));
    CHECK(has(d, "CASCADE_BUILD_INTERFACE=OFF") && has(d, "USER foxsdr"));
    CHECK(has(d, "NEVER BUILT") && has(slurp("packaging/flatpak/com.foxsdr.FoxSDR.yaml"), "NEVER BUILT"));
    CHECK(has(slurp("packaging/flatpak/com.foxsdr.FoxSDR.yaml"), "app-id: com.foxsdr.FoxSDR"));
    CHECK(has(slurp("cmake/linux_package.cmake"), "NEVER RUN ON A REAL LINUX BUILD"));
    return testSummary("test_packaging_files");
}
