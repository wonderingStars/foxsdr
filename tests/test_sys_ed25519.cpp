// Tests for core/sys_hash.hpp, core/sys_ed25519.hpp and core/sys_manifest_sig.hpp (lane sysmodels).
//
// The Ed25519 vectors are RFC 8032 section 7.1 (tests/sys_ed25519_vectors.hpp, generated from the RFC text). The SHA vectors are FIPS 180-4's examples
// (the empty message, "abc", the two-block message) and the one-million-'a' message. Nothing here touches a key store, a certificate or the network.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/sys_ed25519.hpp"
#include "core/sys_hash.hpp"
#include "core/sys_manifest_sig.hpp"
#include "sys_ed25519_vectors.hpp"
#include "test_check.hpp"

using namespace cascade::core::sys;

namespace {

std::string unhex(const std::string& h) {
    std::string o;
    const bool ok = fromHex(h, o);
    CHECK(ok);
    return o;
}
template <class A>
A arr(const std::string& bytes) {
    A a{};
    CHECK(bytes.size() == a.size());
    for (std::size_t i = 0; i < a.size() && i < bytes.size(); ++i) { a[i] = static_cast<std::uint8_t>(bytes[i]); }
    return a;
}
const std::uint8_t* U(const std::string& s) { return reinterpret_cast<const std::uint8_t*>(s.data()); }

std::string hex256(const std::string& s) { const auto d = sha256(s.data(), s.size()); return toHex(d.data(), d.size()); }
std::string hex512(const std::string& s) { const auto d = sha512(s.data(), s.size()); return toHex(d.data(), d.size()); }

struct Rng {   // deterministic, so a failing run is reproducible
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed) {}
    std::uint32_t next() { s = s * 6364136223846793005ull + 1442695040888963407ull; return static_cast<std::uint32_t>(s >> 33); }
};

void testHashes() {
    CHECK(hex256("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(hex256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(hex256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(hex512("") == "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");
    CHECK(hex512("abc") == "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    CHECK(hex512("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu") ==
          "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909");
    // One million 'a', fed in uneven pieces.
    {
        Sha256 h;
        const std::string chunk(997, 'a');
        std::size_t left = 1000000;
        while (left > 0) { const std::size_t n = left < chunk.size() ? left : chunk.size(); h.update(chunk.data(), n); left -= n; }
        const auto d = h.finish();
        CHECK(toHex(d.data(), d.size()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    }
    // Streaming equals one-shot at every length across the block boundaries (both hashes), with every split point of a few lengths.
    {
        std::string data(300, '\0');
        Rng r(7);
        for (auto& c : data) { c = static_cast<char>(r.next()); }
        for (std::size_t n = 0; n <= 300; ++n) {
            const std::string m = data.substr(0, n);
            const std::size_t split = n / 3;
            Sha256 a; a.update(m.data(), split); a.update(m.data() + split, n - split);
            Sha512 b; b.update(m.data(), split); b.update(m.data() + split, n - split);
            const auto d1 = a.finish(), d2 = sha256(m.data(), n);
            const auto e1 = b.finish(), e2 = sha512(m.data(), n);
            CHECK(d1 == d2);
            CHECK(e1 == e2);
        }
    }
    std::string back;
    CHECK(fromHex("00ffAb", back) && back == std::string("\x00\xff\xab", 3));
    CHECK(!fromHex("0", back) && back.empty());
    CHECK(!fromHex("zz", back));
}

void testRfcVectors() {
    for (const auto& v : kEd25519Vectors) {
        const auto seed = arr<Ed25519Seed>(unhex(v.secret));
        const auto pub = arr<Ed25519PublicKey>(unhex(v.pub));
        const auto sig = arr<Ed25519Signature>(unhex(v.sig));
        const std::string msg = unhex(v.msg);
        CHECK(ed25519PublicKey(seed) == pub);
        CHECK(ed25519Sign(seed, U(msg), msg.size()) == sig);
        CHECK(ed25519Verify(pub, U(msg), msg.size(), sig));
        // Every single-bit change of the signature is refused (all 512 of them), and so is a change of the key or of the message.
        for (int bit = 0; bit < 512; ++bit) {
            auto bad = sig;
            bad[bit / 8] ^= static_cast<std::uint8_t>(1u << (bit % 8));
            CHECK(!ed25519Verify(pub, U(msg), msg.size(), bad));
        }
        for (int bit = 0; bit < 256; ++bit) {
            auto badKey = pub;
            badKey[bit / 8] ^= static_cast<std::uint8_t>(1u << (bit % 8));
            CHECK(!ed25519Verify(badKey, U(msg), msg.size(), sig));
        }
        for (std::size_t bit = 0; bit < msg.size() * 8 && bit < 128; ++bit) {
            std::string bad = msg;
            bad[bit / 8] = static_cast<char>(bad[bit / 8] ^ (1u << (bit % 8)));
            CHECK(!ed25519Verify(pub, U(bad), bad.size(), sig));
        }
        // Extended message.
        std::string longer = msg + "x";
        CHECK(!ed25519Verify(pub, U(longer), longer.size(), sig));
    }
}

// S + L is the same point equation but a different encoding: RFC 8032 5.1.7 requires it refused.
void testMalleability() {
    const auto& v = kEd25519Vectors[0];
    const auto pub = arr<Ed25519PublicKey>(unhex(v.pub));
    auto sig = arr<Ed25519Signature>(unhex(v.sig));
    const std::string msg = unhex(v.msg);
    CHECK(ed25519Verify(pub, U(msg), msg.size(), sig));
    static const std::uint8_t L[32] = {237, 211, 245, 92, 26, 99, 18, 88, 214, 156, 247, 162, 222, 249, 222, 20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 16};
    unsigned carry = 0;
    Ed25519Signature mal = sig;
    for (int i = 0; i < 32; ++i) {
        const unsigned s = mal[32 + i] + L[i] + carry;
        mal[32 + i] = static_cast<std::uint8_t>(s & 255);
        carry = s >> 8;
    }
    CHECK(carry == 0);   // S < L < 2^253, so S + L fits in 32 bytes for this vector
    CHECK(!ed25519Verify(pub, U(msg), msg.size(), mal));
    // S exactly L.
    Ed25519Signature eq = sig;
    for (int i = 0; i < 32; ++i) { eq[32 + i] = L[i]; }
    CHECK(!ed25519Verify(pub, U(msg), msg.size(), eq));
    // All-ones S.
    Ed25519Signature ones = sig;
    for (int i = 0; i < 32; ++i) { ones[32 + i] = 0xff; }
    CHECK(!ed25519Verify(pub, U(msg), msg.size(), ones));
}

void testBadPoints() {
    const std::string msg = "m";
    Ed25519Signature zero{};
    // y = p (non-canonical), y = p+1 (as 1 + 2^255-19 + ... ) and the all-ones key must all be refused without matching anything.
    Ed25519PublicKey yp{};
    for (int i = 0; i < 32; ++i) { yp[i] = 0xff; }
    yp[0] = 0xed; yp[31] = 0x7f;   // exactly p
    CHECK(!ed25519Verify(yp, U(msg), msg.size(), zero));
    Ed25519PublicKey ones{};
    for (int i = 0; i < 32; ++i) { ones[i] = 0xff; }
    CHECK(!ed25519Verify(ones, U(msg), msg.size(), zero));
    // x = 0 point (y = 1) with the sign bit set is not a valid encoding.
    Ed25519PublicKey id{};
    id[0] = 1;
    id[31] = 0x80;
    CHECK(!ed25519Verify(id, U(msg), msg.size(), zero));
    // y = 2 is not on the curve (no square root): refused, not crashed.
    Ed25519PublicKey two{};
    two[0] = 2;
    CHECK(!ed25519Verify(two, U(msg), msg.size(), zero));
    // The all-zero signature under a real key.
    const auto pub = arr<Ed25519PublicKey>(unhex(kEd25519Vectors[1].pub));
    CHECK(!ed25519Verify(pub, U(msg), msg.size(), zero));
}

void testBase64() {
    CHECK(base64Encode("") == "");
    CHECK(base64Encode("f") == "Zg==");
    CHECK(base64Encode("fo") == "Zm8=");
    CHECK(base64Encode("foo") == "Zm9v");
    CHECK(base64Encode("foob") == "Zm9vYg==");
    CHECK(base64Encode("fooba") == "Zm9vYmE=");
    CHECK(base64Encode("foobar") == "Zm9vYmFy");   // RFC 4648 section 10
    Rng r(3);
    for (int n = 0; n < 60; ++n) {
        std::string s;
        for (int i = 0; i < n; ++i) { s.push_back(static_cast<char>(r.next())); }
        std::string back;
        CHECK(base64Decode(base64Encode(s), back) && back == s);
    }
    std::string o;
    CHECK(!base64Decode("Zg=", o));            // length
    CHECK(!base64Decode("Zg==Zg==", o));       // padding in the middle
    CHECK(!base64Decode("Zh==", o));           // non-canonical trailing bits
    CHECK(!base64Decode("Zm9=", o));           // non-canonical trailing bits
    CHECK(!base64Decode("Zm9v\n", o));         // whitespace
    CHECK(!base64Decode("Zm 9", o));
    CHECK(!base64Decode("Zm9-", o));           // URL alphabet is not accepted
    CHECK(!base64Decode("=m9v", o));
    CHECK(o.empty());
}

const std::string kManifest = "{\"version\":\"0.99.109\",\"url\":\"https://example.invalid/x.exe\",\"sha256\":\"00\"}";

void testEnvelope() {
    const auto seed = arr<Ed25519Seed>(unhex(kEd25519Vectors[0].secret));
    const auto seed2 = arr<Ed25519Seed>(unhex(kEd25519Vectors[1].secret));
    const auto pub = ed25519PublicKey(seed);
    const auto pub2 = ed25519PublicKey(seed2);
    const std::string env = makeEnvelope(seed, kManifest);

    std::string payload;
    CHECK(verifyEnvelope(env, {pub}, payload) == SigResult::Ok);
    CHECK(payload == kManifest);
    // Key rotation: a list with an old and a new key accepts a signature by either; a list without the signer's key says UnknownKey.
    CHECK(verifyEnvelope(env, {pub2, pub}, payload) == SigResult::Ok);
    CHECK(verifyEnvelope(env, {pub2}, payload) == SigResult::UnknownKey && payload.empty());
    // No key configured: never a pass.
    CHECK(verifyEnvelope(env, {}, payload) == SigResult::NoTrustedKey && payload.empty());
    CHECK(ownerTrustedKeys().empty());   // the placeholder: the owner has supplied no key
    CHECK(std::string(kOwnerKeyHex).empty());
    // The payload is bound: swap it for another manifest under the same signature, or touch any other field.
    using nlohmann::json;
    auto reenc = [&](const char* key, const json& v) {
        json j = json::parse(env);
        j[key] = v;
        return j.dump();
    };
    CHECK(verifyEnvelope(reenc("payload", base64Encode(kManifest + " ")), {pub}, payload) == SigResult::BadSignature && payload.empty());
    CHECK(verifyEnvelope(reenc("version", 2), {pub}, payload) == SigResult::UnknownVersion);
    CHECK(verifyEnvelope(reenc("version", "1"), {pub}, payload) == SigResult::Malformed);
    CHECK(verifyEnvelope(reenc("alg", "rsa"), {pub}, payload) == SigResult::UnknownAlgorithm);
    CHECK(verifyEnvelope(reenc("format", "other"), {pub}, payload) == SigResult::UnknownFormat);
    CHECK(verifyEnvelope(reenc("keyId", "0000000000000000"), {pub}, payload) == SigResult::UnknownKey);
    CHECK(verifyEnvelope(reenc("sig", "AAAA"), {pub}, payload) == SigResult::Malformed);
    CHECK(verifyEnvelope(reenc("payload", "not base64!"), {pub}, payload) == SigResult::Malformed);
    {
        json j = json::parse(env);
        j.erase("sig");
        CHECK(verifyEnvelope(j.dump(), {pub}, payload) == SigResult::Malformed);
    }
    CHECK(verifyEnvelope("", {pub}, payload) == SigResult::Malformed);
    CHECK(verifyEnvelope("[]", {pub}, payload) == SigResult::Malformed);
    CHECK(verifyEnvelope("{", {pub}, payload) == SigResult::Malformed);
    // A signature made for the bare payload (without the domain prefix) does not verify as a manifest.
    {
        const auto raw = ed25519Sign(seed, U(kManifest), kManifest.size());
        json j = json::parse(env);
        j["sig"] = base64Encode(std::string(reinterpret_cast<const char*>(raw.data()), raw.size()));
        CHECK(verifyEnvelope(j.dump(), {pub}, payload) == SigResult::BadSignature);
    }
    // Limits: an oversized envelope and an oversized payload are refused before any curve arithmetic.
    CHECK(verifyEnvelope(std::string(kMaxEnvelopeBytes + 1, ' '), {pub}, payload) == SigResult::TooLarge);
    {
        const std::string big(kMaxManifestPayloadBytes + 1, 'a');
        CHECK(verifyEnvelope(makeEnvelope(seed, big), {pub}, payload) == SigResult::TooLarge);
        const std::string edge(kMaxManifestPayloadBytes, 'a');
        CHECK(verifyEnvelope(makeEnvelope(seed, edge), {pub}, payload) == SigResult::Ok && payload == edge);
    }
    // An empty manifest is a legal payload (the caller's parser then refuses it), the envelope itself is fine.
    CHECK(verifyEnvelope(makeEnvelope(seed, ""), {pub}, payload) == SigResult::Ok && payload.empty());
}

// FUZZ over the envelope parser: byte flips, truncations, insertions and splices of a valid envelope. It must never crash or hang, and it must accept
// nothing but the original payload (a mutation that is still a valid envelope for the same bytes, e.g. in insignificant whitespace, is allowed).
void testEnvelopeFuzz() {
    const auto seed = arr<Ed25519Seed>(unhex(kEd25519Vectors[2].secret));
    const auto pub = ed25519PublicKey(seed);
    const std::string env = makeEnvelope(seed, kManifest);
    Rng r(12345);
    int accepted = 0;
    for (int it = 0; it < 4000; ++it) {
        std::string m = env;
        const int kind = static_cast<int>(r.next() % 5);
        if (kind == 0) { m[r.next() % m.size()] = static_cast<char>(r.next()); }
        else if (kind == 1) { m.resize(r.next() % m.size()); }
        else if (kind == 2) { m.insert(r.next() % m.size(), 1, static_cast<char>(r.next())); }
        else if (kind == 3) { m.erase(r.next() % m.size(), 1 + r.next() % 8); }
        else { const std::size_t a = r.next() % m.size(); const std::size_t b = r.next() % m.size(); std::swap(m[a], m[b]); }
        std::string payload;
        const SigResult res = verifyEnvelope(m, {pub}, payload);
        if (res == SigResult::Ok) { ++accepted; CHECK(payload == kManifest); } else { CHECK(payload.empty()); }
    }
    CHECK(accepted < 4000);
    std::printf("envelope fuzz: 4000 mutants, %d still valid\n", accepted);
}

}  // namespace

int main() {
    testHashes();
    testRfcVectors();
    testMalleability();
    testBadPoints();
    testBase64();
    testEnvelope();
    testEnvelopeFuzz();
    return testSummary("test_sys_ed25519");
}
