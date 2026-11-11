// sys_manifest_sig.cpp - see sys_manifest_sig.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/sys_manifest_sig.hpp"

#include <nlohmann/json.hpp>

#include "core/sys_hash.hpp"

namespace cascade::core::sys {

// The owner's public key (64 hex characters), supplied later. Empty = no key: verifyEnvelope then answers NoTrustedKey.
const char* const kOwnerKeyHex = "";

const char* sigResultName(SigResult r) {
    switch (r) {
        case SigResult::Ok: return "ok";
        case SigResult::NoTrustedKey: return "no-trusted-key";
        case SigResult::TooLarge: return "too-large";
        case SigResult::Malformed: return "malformed";
        case SigResult::UnknownFormat: return "unknown-format";
        case SigResult::UnknownVersion: return "unknown-version";
        case SigResult::UnknownAlgorithm: return "unknown-algorithm";
        case SigResult::UnknownKey: return "unknown-key";
        case SigResult::BadSignature: return "bad-signature";
    }
    return "?";
}

std::string base64Encode(const std::string& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const unsigned v = (unsigned(std::uint8_t(in[i])) << 16) | (unsigned(std::uint8_t(in[i + 1])) << 8) | std::uint8_t(in[i + 2]);
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += t[v & 63];
    }
    if (i + 1 == in.size()) {
        const unsigned v = unsigned(std::uint8_t(in[i])) << 16;
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        const unsigned v = (unsigned(std::uint8_t(in[i])) << 16) | (unsigned(std::uint8_t(in[i + 1])) << 8);
        out += t[v >> 18]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += '=';
    }
    return out;
}

bool base64Decode(const std::string& text, std::string& out) {
    out.clear();
    if (text.size() % 4 != 0) { return false; }
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') { return c - 'A'; }
        if (c >= 'a' && c <= 'z') { return c - 'a' + 26; }
        if (c >= '0' && c <= '9') { return c - '0' + 52; }
        if (c == '+') { return 62; }
        if (c == '/') { return 63; }
        return -1;
    };
    std::string r;
    r.reserve(text.size() / 4 * 3);
    for (std::size_t i = 0; i < text.size(); i += 4) {
        const bool last = (i + 4 == text.size());
        int pad = 0;
        int v[4];
        for (int k = 0; k < 4; ++k) {
            const char c = text[i + k];
            if (c == '=') {
                if (!last || k < 2) { return false; }
                ++pad;
                v[k] = 0;
            } else {
                if (pad != 0) { return false; }   // data after padding
                v[k] = val(c);
                if (v[k] < 0) { return false; }
            }
        }
        const unsigned w = (unsigned(v[0]) << 18) | (unsigned(v[1]) << 12) | (unsigned(v[2]) << 6) | unsigned(v[3]);
        // Canonical padding bits must be zero, so one byte string has exactly one encoding.
        if (pad == 2 && (w & 0xffff) != 0) { return false; }
        if (pad == 1 && (w & 0xff) != 0) { return false; }
        r.push_back(static_cast<char>(w >> 16));
        if (pad < 2) { r.push_back(static_cast<char>((w >> 8) & 255)); }
        if (pad < 1) { r.push_back(static_cast<char>(w & 255)); }
    }
    out = std::move(r);
    return true;
}

std::string keyIdOf(const Ed25519PublicKey& key) {
    const Digest256 d = sha256(key.data(), key.size());
    return toHex(d.data(), 8);
}

std::string signedBytes(const std::string& payload) { return std::string("FoxSDR update manifest v1\n") + payload; }

SigResult verifyEnvelope(const std::string& envelopeText, const std::vector<Ed25519PublicKey>& trusted, std::string& payloadOut) {
    payloadOut.clear();
    if (trusted.empty()) { return SigResult::NoTrustedKey; }
    if (envelopeText.size() > kMaxEnvelopeBytes) { return SigResult::TooLarge; }
    using nlohmann::json;
    json j = json::parse(envelopeText, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) { return SigResult::Malformed; }
    auto str = [&](const char* k, std::string& dst) {
        const auto it = j.find(k);
        if (it == j.end() || !it->is_string()) { return false; }
        dst = it->get<std::string>();
        return true;
    };
    std::string format, alg, keyId, payloadB64, sigB64;
    if (!str("format", format) || !str("alg", alg) || !str("keyId", keyId) || !str("payload", payloadB64) || !str("sig", sigB64)) { return SigResult::Malformed; }
    const auto vit = j.find("version");
    if (vit == j.end() || !vit->is_number_integer()) { return SigResult::Malformed; }
    if (format != "foxsdr-update-manifest-sig") { return SigResult::UnknownFormat; }
    if (vit->get<long long>() != 1) { return SigResult::UnknownVersion; }
    if (alg != "ed25519") { return SigResult::UnknownAlgorithm; }
    if (payloadB64.size() > kMaxManifestPayloadBytes / 3 * 4 + 8) { return SigResult::TooLarge; }
    std::string payload, sigBytes;
    if (!base64Decode(payloadB64, payload) || !base64Decode(sigB64, sigBytes) || sigBytes.size() != 64) { return SigResult::Malformed; }
    if (payload.size() > kMaxManifestPayloadBytes) { return SigResult::TooLarge; }
    Ed25519Signature sig{};
    for (int i = 0; i < 64; ++i) { sig[i] = static_cast<std::uint8_t>(sigBytes[i]); }
    const std::string msg = signedBytes(payload);
    bool known = false;
    for (const auto& k : trusted) {
        if (keyIdOf(k) != keyId) { continue; }
        known = true;
        if (ed25519Verify(k, reinterpret_cast<const std::uint8_t*>(msg.data()), msg.size(), sig)) {
            payloadOut = std::move(payload);
            return SigResult::Ok;
        }
    }
    return known ? SigResult::BadSignature : SigResult::UnknownKey;
}

std::string makeEnvelope(const Ed25519Seed& seed, const std::string& payload) {
    const std::string msg = signedBytes(payload);
    const Ed25519Signature sig = ed25519Sign(seed, reinterpret_cast<const std::uint8_t*>(msg.data()), msg.size());
    nlohmann::json j;
    j["format"] = "foxsdr-update-manifest-sig";
    j["version"] = 1;
    j["alg"] = "ed25519";
    j["keyId"] = keyIdOf(ed25519PublicKey(seed));
    j["payload"] = base64Encode(payload);
    j["sig"] = base64Encode(std::string(reinterpret_cast<const char*>(sig.data()), sig.size()));
    return j.dump();
}

std::vector<Ed25519PublicKey> ownerTrustedKeys() {
    std::vector<Ed25519PublicKey> out;
    const std::string hex = kOwnerKeyHex;
    std::string bytes;
    if (hex.size() == 64 && fromHex(hex, bytes) && bytes.size() == 32) {
        Ed25519PublicKey k{};
        for (int i = 0; i < 32; ++i) { k[i] = static_cast<std::uint8_t>(bytes[i]); }
        out.push_back(k);
    }
    return out;
}

}  // namespace cascade::core::sys
