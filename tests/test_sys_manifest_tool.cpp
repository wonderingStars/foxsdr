// Test for tools/manifest_sign.cpp (the standalone manifest signing tool): the built tool is run on files in a scratch folder, with the RFC 8032 test seed. Also a
// source scan: no file of this lane may reach a key provider, a certificate store or a random source (the no-prompt rule and "no key is generated").
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#ifdef _WIN32
#include <process.h>
#define TEST_GETPID _getpid
#else
#include <unistd.h>
#define TEST_GETPID getpid
#endif

#include "core/sys_hash.hpp"
#include "core/sys_manifest_sig.hpp"
#include "sys_ed25519_vectors.hpp"
#include "test_check.hpp"

namespace fs = std::filesystem;
using namespace cascade::core::sys;

namespace {

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}
void spit(const fs::path& p, const std::string& s) { std::ofstream f(p, std::ios::binary | std::ios::trunc); f << s; }

fs::path g_tool;

int run(const std::string& args, const fs::path& stdoutFile = {}) {
    std::string cmd = "\"\"" + g_tool.string() + "\" " + args;
    if (!stdoutFile.empty()) { cmd += " > \"" + stdoutFile.string() + "\""; }
    cmd += " 2>NUL\"";
#ifndef _WIN32
    cmd = "\"" + g_tool.string() + "\" " + args + (stdoutFile.empty() ? "" : " > \"" + stdoutFile.string() + "\"") + " 2>/dev/null";
#endif
    return std::system(cmd.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    const fs::path self = fs::absolute(argv[0]);
    g_tool = self.parent_path() / "foxsdr_manifest_sign";
#ifdef _WIN32
    g_tool += ".exe";
#endif
    CHECK(fs::exists(g_tool));

    const fs::path dir = fs::temp_directory_path() / ("sysmodels-tool-" + std::to_string(TEST_GETPID()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    const auto& v = kEd25519Vectors[0];
    spit(dir / "seed.txt", std::string("  ") + v.secret + "\r\n");
    const std::string manifest = "{\"version\":\"0.99.109\",\"url\":\"https://example.invalid/a.exe\",\"sha256\":\"00\"}";
    spit(dir / "m.json", manifest);

    // pubkey prints the RFC public key.
    CHECK(run("pubkey --seed-file \"" + (dir / "seed.txt").string() + "\"", dir / "pub.txt") == 0);
    CHECK(slurp(dir / "pub.txt").substr(0, 64) == v.pub);

    // sign, then verify with the right key: valid, and the manifest comes back byte for byte.
    CHECK(run("sign --seed-file \"" + (dir / "seed.txt").string() + "\" --in \"" + (dir / "m.json").string() + "\" --out \"" + (dir / "env.json").string() + "\"") == 0);
    CHECK(fs::exists(dir / "env.json"));
    CHECK(run(std::string("verify --pubkey-hex ") + v.pub + " --in \"" + (dir / "env.json").string() + "\" --out \"" + (dir / "back.json").string() + "\"", dir / "ok.txt") == 0);
    CHECK(slurp(dir / "back.json") == manifest);
    CHECK(slurp(dir / "ok.txt").rfind("valid", 0) == 0);

    // The library agrees with the tool (the same envelope verifies in-process).
    {
        std::string key, payload;
        CHECK(fromHex(v.pub, key));
        Ed25519PublicKey pub{};
        for (int i = 0; i < 32; ++i) { pub[i] = static_cast<std::uint8_t>(key[i]); }
        CHECK(verifyEnvelope(slurp(dir / "env.json"), {pub}, payload) == SigResult::Ok && payload == manifest);
    }

    // Refusals: a different key, a tampered envelope, a bad seed file, a missing input, an empty manifest, an unknown command, no arguments.
    CHECK(run(std::string("verify --pubkey-hex ") + kEd25519Vectors[1].pub + " --in \"" + (dir / "env.json").string() + "\"") == 2);
    {
        std::string env = slurp(dir / "env.json");
        const auto at = env.find("\"sig\":\"") + 7;
        env[at] = env[at] == 'A' ? 'B' : 'A';
        spit(dir / "bad.json", env);
        CHECK(run(std::string("verify --pubkey-hex ") + v.pub + " --in \"" + (dir / "bad.json").string() + "\"") == 2);
    }
    spit(dir / "short.txt", "abcd");
    CHECK(run("pubkey --seed-file \"" + (dir / "short.txt").string() + "\"") == 2);
    CHECK(run("sign --seed-file \"" + (dir / "short.txt").string() + "\" --in \"" + (dir / "m.json").string() + "\" --out \"" + (dir / "x.json").string() + "\"") == 2);
    CHECK(!fs::exists(dir / "x.json"));
    CHECK(run("sign --seed-file \"" + (dir / "seed.txt").string() + "\" --in \"" + (dir / "nope.json").string() + "\" --out \"" + (dir / "x.json").string() + "\"") == 2);
    spit(dir / "empty.json", "");
    CHECK(run("sign --seed-file \"" + (dir / "seed.txt").string() + "\" --in \"" + (dir / "empty.json").string() + "\" --out \"" + (dir / "x.json").string() + "\"") == 2);
    CHECK(run("frobnicate") == 2);
    CHECK(run("") == 2);
    CHECK(run(std::string("verify --pubkey-hex 00 --in \"") + (dir / "env.json").string() + "\"") == 2);

    // SOURCE SCAN: the lane's files never name a key provider, a certificate store, a signing utility or a random source.
    const fs::path root = fs::path(__FILE__).parent_path().parent_path();
    const char* files[] = {"src/core/sys_hash.cpp", "src/core/sys_ed25519.cpp", "src/core/sys_manifest_sig.cpp", "tools/manifest_sign.cpp"};
    const char* banned[] = {"NCrypt", "BCrypt", "CertOpen", "CertFind", "CryptAcquire", "CryptGen", "random_device", "rand(", "srand", "signtool", "certutil", "CryptoAPI", "wincrypt"};
    for (const char* f : files) {
        const std::string text = slurp(root / f);
        CHECK(!text.empty());
        // The tool's own header comment says it never reads a key store: strip comment lines before scanning.
        std::string code;
        std::size_t pos = 0;
        while (pos < text.size()) {
            std::size_t e = text.find('\n', pos);
            if (e == std::string::npos) { e = text.size(); }
            const std::string line = text.substr(pos, e - pos);
            const auto first = line.find_first_not_of(" \t");
            if (first == std::string::npos || line.compare(first, 2, "//") != 0) { code += line + "\n"; }
            pos = e + 1;
        }
        for (const char* b : banned) {
            const bool hit = code.find(b) != std::string::npos;
            if (hit) { std::printf("scan: %s names %s\n", f, b); }
            CHECK(!hit);
        }
    }

    // THE DORMANT SIGNING SCRIPT: it refuses without a named certificate BEFORE it looks for a tool, it takes no password on a command line, and nothing
    // references it (not the installer script, not the release scripts).
    {
        const std::string ps = slurp(root / "installer/sign-setup.ps1");
        CHECK(!ps.empty());
        const auto refuse = ps.find("no certificate is named");
        const auto look = ps.find("Get-Command signtool.exe");
        CHECK(refuse != std::string::npos && look != std::string::npos && refuse < look);
        CHECK(ps.find("FOXSDR_SIGN_TIMESTAMP_URL must be an https URL") != std::string::npos);
        CHECK(ps.find("exit 2") != std::string::npos);
        CHECK(ps.find("New-SelfSignedCertificate") == std::string::npos && ps.find("certreq") == std::string::npos && ps.find("Import-PfxCertificate") == std::string::npos);
        for (const char* other : {"installer/cascade.iss", "tools/release.ps1", "tools/release-common.ps1", "tools/build-nightly.ps1"}) {
            const std::string t = slurp(root / other);
            CHECK(!t.empty());
            CHECK(t.find("sign-setup") == std::string::npos);
        }
    }

    fs::remove_all(dir);
    return testSummary("test_sys_manifest_tool");
}
