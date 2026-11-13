// test_wire_c_updater.cpp - the signature on the update manifest, wired into the updater (wiring lane C row S6: src/core/updater.cpp, model core/sys_manifest_sig.hpp).
// The manifest arrives through the updater's own canned-answer seam (FOXSDR_FAKE_UPDATE_MANIFEST: a file, no network) and checkForUpdate decides.
//
//   U1  no owner key (today's state): an unsigned manifest is accepted exactly as before
//   U2  an owner key: a manifest signed with it is accepted, and what is parsed is the PAYLOAD (its version, url and checksum come out)
//   U3  an owner key: an unsigned manifest, a manifest signed by another key, an envelope with one byte of its payload altered, garbage and an empty file are each REFUSED with
//       "the update information is not signed by FoxSDR", and nothing of the manifest is returned
//   U4  both ways the body can arrive (the canned answer and the fetched answer) go through the same check (from the source text)
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "core/sys_ed25519.hpp"
#include "core/sys_manifest_sig.hpp"
#include "core/updater.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
namespace sys = cascade::core::sys;
using cascade::core::UpdateInfo;

namespace {

const char* kSha = "393e5fd91b7b2292611c52af7e9f2db1e2c730d78f1639adefceaf0fb5cca0ea";

std::string manifest(const std::string& version) {
    return std::string("{\"ok\":true,\"version\":\"") + version + "\",\"url\":\"https://foxsdr.com/download/foxsdr-setup-" + version + ".exe\",\"sha256\":\"" + kSha +
           "\",\"size\":3274221,\"newer\":true,\"notes\":[]}";
}

void setEnv(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

fs::path g_dir;

// Puts `body` in the canned-answer file and asks the updater about it.
bool ask(const std::string& body, UpdateInfo& info, std::string& error) {
    const fs::path f = g_dir / "canned.json";
    {
        std::ofstream o(f, std::ios::binary | std::ios::trunc);
        o << body;
    }
    setEnv("FOXSDR_FAKE_UPDATE_MANIFEST", f.string());
    return cascade::core::checkForUpdate("https://foxsdr.com/api/update", "0.50.0", "", info, error);
}

sys::Ed25519Seed seedOf(std::uint8_t v) {
    sys::Ed25519Seed s{};
    for (std::size_t i = 0; i < s.size(); ++i) { s[i] = static_cast<std::uint8_t>(v + i); }
    return s;
}

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("test_wire_c_updater\n");
#if defined(_WIN32)
    g_dir = fs::temp_directory_path() / ("foxsdr_wc_updater_" + std::to_string(_getpid()));
#else
    g_dir = fs::temp_directory_path() / ("foxsdr_wc_updater_" + std::to_string(getpid()));
#endif
    std::error_code ec;
    fs::create_directories(g_dir, ec);

    std::printf("U1: no owner key: signatures are not enforced, an unsigned manifest is accepted as before\n");
    cascade::core::setManifestTrustedKeysForTest(nullptr);
    CHECK(sys::ownerTrustedKeys().empty());   // (the owner has not supplied a key: if this fails the test's premise changed)
    {
        UpdateInfo info;
        std::string err;
        CHECK(ask(manifest("0.56.0"), info, err));
        CHECK(info.version == "0.56.0");
    }

    const sys::Ed25519Seed owner = seedOf(1);
    const sys::Ed25519Seed other = seedOf(100);
    const std::vector<sys::Ed25519PublicKey> keys{sys::ed25519PublicKey(owner)};
    cascade::core::setManifestTrustedKeysForTest(&keys);

    std::printf("U2: an owner key: a signed manifest is accepted and the payload is what is parsed\n");
    {
        UpdateInfo info;
        std::string err;
        CHECK(ask(sys::makeEnvelope(owner, manifest("0.57.1")), info, err));
        CHECK(info.version == "0.57.1");
        CHECK(info.url == "https://foxsdr.com/download/foxsdr-setup-0.57.1.exe");
        CHECK(info.sha256 == kSha);
        CHECK(err.empty());
    }

    std::printf("U3: an owner key: everything else is refused\n");
    {
        std::string tampered = sys::makeEnvelope(owner, manifest("0.57.1"));
        // alter one character inside the payload field's base64 (the first '"payload"' value), keeping the text a valid string
        const std::size_t at = tampered.find("\"payload\"");
        CHECK(at != std::string::npos);
        if (at != std::string::npos) {
            const std::size_t q = tampered.find('"', tampered.find(':', at) + 1);
            std::size_t pos = q + 5;
            tampered[pos] = tampered[pos] == 'A' ? 'B' : 'A';
        }
        const std::vector<std::pair<const char*, std::string>> bad{
            {"unsigned manifest", manifest("0.57.1")},
            {"signed by another key", sys::makeEnvelope(other, manifest("0.57.1"))},
            {"payload altered", tampered},
            {"garbage", "this is not json"},
            {"empty", ""},
            {"envelope of nothing", "{\"format\":\"foxsdr-manifest\"}"},
        };
        for (const auto& [name, body] : bad) {
            UpdateInfo info;
            std::string err;
            const bool accepted = ask(body, info, err);
            if (accepted) { std::printf("      '%s' was ACCEPTED\n", name); }
            CHECK(!accepted);
            CHECK(err == "the update information is not signed by FoxSDR");
            CHECK(info.version.empty());
        }
    }

    std::printf("U4: both ways the body can arrive go through the same check\n");
    {
        std::string root;
#ifdef WIRE_C_SOURCE_DIR
        root = WIRE_C_SOURCE_DIR;
#endif
        if (!root.empty()) {
            const std::string text = readFile(fs::path(root) / "src" / "core" / "updater.cpp");
            std::size_t n = 0, at = 0;
            while ((at = text.find("unwrapSignedManifest(", at)) != std::string::npos) { ++n; at += 10; }
            CHECK(n >= 3);   // the definition and the two call sites
            CHECK(text.find("unwrapSignedManifest(canned, error) || !parseUpdateManifest(canned") != std::string::npos);
            CHECK(text.find("unwrapSignedManifest(body, error) || !parseUpdateManifest(body") != std::string::npos);
        }
    }

    cascade::core::setManifestTrustedKeysForTest(nullptr);
    setEnv("FOXSDR_FAKE_UPDATE_MANIFEST", "");
    fs::remove_all(g_dir, ec);
    return testSummary("test_wire_c_updater");
}
