// sys_ed25519.hpp - Ed25519 signatures (pure Ed25519, RFC 8032 section 5.1), key generation NOT included.
//
// WHAT THIS IS FOR. The update manifest (core/updater.cpp) is protected today by its SHA-256 field and HTTPS. A signature over the manifest itself
// would also protect against a compromised host or a hostile proxy that terminates TLS. This file is the verifier (and, for the standalone signing
// tool and the tests, the signer); sys_manifest_sig.hpp is the envelope around it. Nothing here wires into the updater (updater.cpp is another
// lane's file): the hand-off is in docs/feature-records/sysmodels.md.
//
// WRITTEN FROM THE OPENED RFC 8032 (https://www.rfc-editor.org/rfc/rfc8032.txt, IETF Trust, text of the standard): the group law of section 5.1.4
// (extended coordinates), the point encoding of 5.1.2, the verification equation of 5.1.7 and the rule that S must be below the group order L. The
// field arithmetic is a 16 x 16-bit limb layout (the shape public-domain TweetNaCl uses; no source of it was copied: the curve constants are COMPUTED
// by the one-off script in the commit message from their definitions d = -121665/121666, By = 4/5, sqrt(-1) = 2^((p-1)/4), L = 2^252 + 27742...). The
// tests run all five vectors of RFC 8032 section 7.1 through sign and verify (tests/sys_ed25519_vectors.hpp is generated from the RFC text by a script, not
// typed), then flip every bit of a signature and a message and expect rejection.
//
// WHAT IS REJECTED, beyond a wrong signature: S >= L (malleability, RFC 8032 5.1.7), a public key or R whose y is not below p, a point not on the curve,
// and an x = 0 point carrying the sign bit. The verifier is cofactorless (the equation [S]B = R + [k]A is checked by recomputing R and comparing its
// encoding), the form RFC 8032 permits.
//
// NO KEY PROVIDER, NO STORE. Seeds come in as bytes the caller holds; nothing here generates a key, reads a key store, or calls the operating system.
// Signing is not constant-time (it is for a build-time tool, never for the application).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_SYS_ED25519_HPP
#define CASCADE_CORE_SYS_ED25519_HPP

#include <array>
#include <cstddef>
#include <cstdint>

namespace cascade::core::sys {

using Ed25519Seed = std::array<std::uint8_t, 32>;
using Ed25519PublicKey = std::array<std::uint8_t, 32>;
using Ed25519Signature = std::array<std::uint8_t, 64>;

// The public key for a 32-byte seed (RFC 8032 5.1.5).
Ed25519PublicKey ed25519PublicKey(const Ed25519Seed& seed);

// Signature of `msg` (RFC 8032 5.1.6).
Ed25519Signature ed25519Sign(const Ed25519Seed& seed, const std::uint8_t* msg, std::size_t n);

// True only for a valid signature by `pub` over `msg` (RFC 8032 5.1.7, with the extra rejections listed above).
bool ed25519Verify(const Ed25519PublicKey& pub, const std::uint8_t* msg, std::size_t n, const Ed25519Signature& sig);

}  // namespace cascade::core::sys

#endif  // CASCADE_CORE_SYS_ED25519_HPP
