// sys_manifest_sig.hpp - a signature envelope for the update manifest, and the check an updater would make on it.
//
// THE QUESTION THIS ANSWERS (brief row "a signature on the update manifest itself, beyond its SHA-256 and HTTPS"): the update service returns a JSON
// manifest whose url must be on the allowed host and whose sha256 must match the download (core/updater.cpp). If the service, its CDN or a proxy that
// terminates TLS is hostile, it can name a url on the allowed host with the hash of a file it also controls. A signature by a key the owner holds
// offline, checked against a public key compiled into the program, closes that. WHETHER HTTPS ALONE IS ENOUGH IS THE OWNER'S DECISION and is listed
// in the record; this file only makes the stronger check possible, and nothing calls it yet (updater.cpp is another lane's file).
//
// THE ENVELOPE (one JSON object, at most kMaxEnvelopeBytes):
//   {"format":"foxsdr-update-manifest-sig","version":1,"alg":"ed25519","keyId":"<16 hex>","payload":"<base64 of the manifest bytes>","sig":"<base64, 64 bytes>"}
// The signature is over   "FoxSDR update manifest v1\n" + payload bytes   (a domain-separation prefix, so a signature made for some other purpose with the
// same key cannot be replayed as a manifest). keyId is the first 8 bytes of SHA-256 of the public key, so a key can be rotated: the program carries a
// LIST of trusted keys and the envelope says which one signed. The payload is the exact bytes that were signed; the caller parses THEM as it parses
// the manifest today, never any copy of the fields outside the envelope.
//
// NO KEY IS GENERATED OR STORED HERE. The owner's public key is a constant (ownerTrustedKeys) that is EMPTY until the owner supplies one: with an
// empty list verifyEnvelope answers NoTrustedKey, which an updater must treat as "signatures are not enforced yet" or "refuse", whichever the owner
// decides - not as a pass. Tests use a key made from the RFC 8032 test seed.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_SYS_MANIFEST_SIG_HPP
#define CASCADE_CORE_SYS_MANIFEST_SIG_HPP

#include <cstddef>
#include <string>
#include <vector>

#include "core/sys_ed25519.hpp"

namespace cascade::core::sys {

inline constexpr std::size_t kMaxEnvelopeBytes = 256 * 1024;
inline constexpr std::size_t kMaxManifestPayloadBytes = 128 * 1024;

enum class SigResult {
    Ok,
    NoTrustedKey,     // the program carries no key yet (the owner has not supplied one)
    TooLarge,
    Malformed,        // not JSON, not an object, a field missing or of the wrong type, bad base64
    UnknownFormat,
    UnknownVersion,
    UnknownAlgorithm,
    UnknownKey,       // keyId names none of the trusted keys
    BadSignature,
};

const char* sigResultName(SigResult r);

// Standard base64 (RFC 4648 section 4), padded, no whitespace: anything else is refused on decode.
std::string base64Encode(const std::string& bytes);
bool base64Decode(const std::string& text, std::string& out);

// 16 hex characters naming a key.
std::string keyIdOf(const Ed25519PublicKey& key);

// The bytes that are actually signed for a payload.
std::string signedBytes(const std::string& payload);

// Verifies `envelopeText` against `trusted`. On Ok, `payloadOut` is the manifest bytes; on any other result it is empty.
SigResult verifyEnvelope(const std::string& envelopeText, const std::vector<Ed25519PublicKey>& trusted, std::string& payloadOut);

// Builds an envelope (the signing tool and the tests; the application never signs).
std::string makeEnvelope(const Ed25519Seed& seed, const std::string& payload);

// The keys this program trusts. EMPTY until the owner supplies one: see kOwnerKeyHex below.
std::vector<Ed25519PublicKey> ownerTrustedKeys();

// The owner's public key as 64 hex characters, or "" while none is supplied. THE ONE LINE THE OWNER EDITS (in sys_manifest_sig.cpp).
extern const char* const kOwnerKeyHex;

}  // namespace cascade::core::sys

#endif  // CASCADE_CORE_SYS_MANIFEST_SIG_HPP
