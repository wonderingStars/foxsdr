// manifest_sign.cpp - the standalone tool that signs an update manifest (and checks one), for the owner's release machine.
//
//   foxsdr_manifest_sign sign   --seed-file <file> --in <manifest.json> --out <envelope.json>
//   foxsdr_manifest_sign verify --pubkey-hex <64 hex> --in <envelope.json> [--out <manifest.json>]
//   foxsdr_manifest_sign pubkey --seed-file <file>
//
// THE SEED FILE holds 64 hex characters (32 bytes; whitespace around them is ignored), kept by the owner OFFLINE. THIS TOOL NEVER GENERATES A KEY, never
// reads a Windows key store or certificate store, never prompts, and never touches the network: it reads the files it is named and writes the one it is told
// to. The public key it prints goes into core/sys_manifest_sig.cpp (kOwnerKeyHex) when the owner decides to enforce signatures. Exit 0 on success, 2 on
// any refusal (the reason is on stderr; a seed or key is never printed except the PUBLIC key by `pubkey`).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "core/sys_hash.hpp"
#include "core/sys_manifest_sig.hpp"

using namespace cascade::core::sys;

namespace {

bool readFile(const std::string& path, std::size_t cap, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { return false; }
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return out.size() <= cap;
}

bool writeFile(const std::string& path, const std::string& bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) { return false; }
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(f);
}

bool loadSeed(const std::string& path, Ed25519Seed& seed) {
    std::string text;
    if (!readFile(path, 4096, text)) { return false; }
    std::string hex;
    for (char c : text) { if (c != ' ' && c != '\t' && c != '\r' && c != '\n') { hex.push_back(c); } }
    std::string bytes;
    if (hex.size() != 64 || !fromHex(hex, bytes) || bytes.size() != 32) { return false; }
    for (int i = 0; i < 32; ++i) { seed[i] = static_cast<std::uint8_t>(bytes[i]); }
    return true;
}

int refuse(const char* why) { std::fprintf(stderr, "manifest_sign: %s\n", why); return 2; }

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { return refuse("usage: sign|verify|pubkey (see the header of tools/manifest_sign.cpp)"); }
    const std::string cmd = argv[1];
    std::string seedFile, inFile, outFile, pubHex;
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string k = argv[i];
        if (k == "--seed-file") { seedFile = argv[i + 1]; }
        else if (k == "--in") { inFile = argv[i + 1]; }
        else if (k == "--out") { outFile = argv[i + 1]; }
        else if (k == "--pubkey-hex") { pubHex = argv[i + 1]; }
        else { return refuse("unknown option"); }
    }
    if (cmd == "pubkey") {
        Ed25519Seed seed{};
        if (seedFile.empty() || !loadSeed(seedFile, seed)) { return refuse("the seed file is missing or is not 64 hex characters"); }
        const auto pub = ed25519PublicKey(seed);
        std::printf("%s\n", toHex(pub.data(), pub.size()).c_str());
        return 0;
    }
    if (cmd == "sign") {
        Ed25519Seed seed{};
        if (seedFile.empty() || !loadSeed(seedFile, seed)) { return refuse("the seed file is missing or is not 64 hex characters"); }
        std::string manifest;
        if (inFile.empty() || outFile.empty() || !readFile(inFile, kMaxManifestPayloadBytes, manifest)) { return refuse("cannot read the manifest (or it is over the size limit)"); }
        if (manifest.empty()) { return refuse("the manifest is empty"); }
        if (!writeFile(outFile, makeEnvelope(seed, manifest))) { return refuse("cannot write the envelope"); }
        return 0;
    }
    if (cmd == "verify") {
        std::string bytes, env;
        if (pubHex.size() != 64 || !fromHex(pubHex, bytes) || bytes.size() != 32) { return refuse("--pubkey-hex must be 64 hex characters"); }
        Ed25519PublicKey pub{};
        for (int i = 0; i < 32; ++i) { pub[i] = static_cast<std::uint8_t>(bytes[i]); }
        if (inFile.empty() || !readFile(inFile, kMaxEnvelopeBytes, env)) { return refuse("cannot read the envelope (or it is over the size limit)"); }
        std::string payload;
        const SigResult r = verifyEnvelope(env, {pub}, payload);
        if (r != SigResult::Ok) { std::fprintf(stderr, "manifest_sign: not valid: %s\n", sigResultName(r)); return 2; }
        if (!outFile.empty() && !writeFile(outFile, payload)) { return refuse("cannot write the manifest"); }
        std::printf("valid\n");
        return 0;
    }
    return refuse("unknown command");
}
