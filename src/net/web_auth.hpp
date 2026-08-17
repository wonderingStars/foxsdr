// Authentication primitives for the web server (P11): password hashing,
// session tokens, and the small encoding/compare helpers they need.
//
// WHY THIS IS A SEPARATE MODULE FROM THE SERVER. Everything here is pure
// computation over bytes — no sockets, no HTTP, no global state — so it can be
// tested exhaustively without a network, and the server can be replaced or
// rewritten without touching the part that decides who gets in. It is also the
// part that must be right first: a web server with a sound transport and a
// broken credential check is simply an open receiver.
//
// THE RULES THIS FILE ENFORCES, each covered by a test in
// tests/test_web_auth.cpp:
//
//  1. A PASSWORD IS NEVER STORED, in the config file or anywhere else. What is
//     persisted is a PBKDF2-HMAC-SHA256 derivation of it: a random per-password
//     salt, an iteration count, and the derived key. `AppConfig` carries the
//     serialized record and has no field a plaintext password could live in.
//
//  2. THE RECORD IS SELF-DESCRIBING: "pbkdf2-sha256$<iterations>$<salt>$<key>",
//     salt and key base64. The iteration count travels WITH the record rather
//     than being a compile-time constant applied at verify time, so raising the
//     cost later re-hashes on next password change instead of locking every
//     existing user out. The algorithm label is checked on parse, which is what
//     lets a future scheme be added without ambiguity about what a stored
//     string means.
//
//  3. kDefaultIterations IS 600,000, the OWASP Password Storage Cheat Sheet
//     figure for PBKDF2-HMAC-SHA256.
//
//     MEASURED COST, because the round number invites a false sense of it: on
//     the development machine one derivation takes about 86 ms, not the "few
//     hundred milliseconds" the figure is usually assumed to buy. Windows CNG
//     and OpenSSL agree on this within a few per cent (0.58 s and 0.516 s
//     respectively for six derivations), so it is the hardware talking, not an
//     implementation quirk — SHA-NI puts this CPU at roughly 7 million
//     iterations a second per core. An attacker with a stolen config file and
//     a comparable machine therefore gets about 12 guesses a second per core.
//     That is still a wall in front of any real password and nothing at all in
//     front of a weak one, which is why kMinPasswordLength is enforced here
//     rather than left to the UI.
//
//     The iteration count is deliberately NOT a constant applied at verify
//     time (see rule 2): it travels inside each record, so raising this value
//     re-hashes on the next password change and every existing record keeps
//     verifying at the cost it was written with.
//
//  4. EVERY SECRET COMPARISON IS CONSTANT-TIME. Password verification and
//     session-token lookup both run through constantTimeEquals, which reads
//     every byte of both inputs whatever it finds. A `memcmp` here leaks the
//     length of the matching prefix to anyone who can time the response, and
//     over a LAN that is a practical attack, not a theoretical one.
//
//  5. SALTS AND TOKENS COME FROM THE SYSTEM CSPRNG (BCryptGenRandom with
//     BCRYPT_USE_SYSTEM_PREFERRED_RNG), never from rand(), std::mt19937, or
//     anything seeded from a clock. randomBytes reports failure rather than
//     falling back to a weaker source: a session token that is merely hard to
//     guess is a session token an attacker eventually guesses.
//
//  6. SESSIONS LIVE IN MEMORY ONLY and are never written to disk, so stopping
//     the application ends every session it had issued. The store holds the
//     SHA-256 of each token, not the token, so a memory dump of a running
//     process does not hand over live credentials.
//
//  7. THE SESSION STORE IS CAPPED (kMaxSessions). An unbounded map keyed by
//     something an unauthenticated caller can cause to be created is a memory
//     -exhaustion path; when the cap is reached the oldest session is evicted.
//
// PLATFORM. The cryptographic entry points (randomBytes, hashPassword,
// verifyPassword, and the token issuance that depends on them) are implemented
// on Windows via CNG — the same bcrypt.lib that core/plugin_repo.cpp already
// links for its SHA-256. On other platforms they return false with a clear
// error rather than a weaker implementation, mirroring how plugin_repo.cpp
// treats its HTTPS transport. base64 and constantTimeEquals are portable and
// are compiled everywhere.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace cascade::net {

// --- Encoding and comparison helpers (portable) ------------------------------

// Standard base64 (RFC 4648 alphabet, '=' padded).
std::string base64Encode(const std::uint8_t* data, std::size_t n);
std::string base64Encode(const std::vector<std::uint8_t>& data);

// Strict base64 decode: rejects any character outside the alphabet (including
// whitespace and newlines), a length that is not a multiple of 4, padding
// anywhere but the tail, and non-zero bits in a padded final group. Strict
// because this parses a config file that may have been hand-edited, and a
// lenient decoder that silently skips junk turns a corrupted record into a
// DIFFERENT valid-looking record rather than into an error.
bool base64Decode(const std::string& in, std::vector<std::uint8_t>& out);

// Equality that always reads all n bytes of both inputs. Returns true iff every
// byte matches. See rule 4 above for why this is not memcmp.
bool constantTimeEquals(const void* a, const void* b, std::size_t n);

// Equality for two byte strings of possibly different length. The length
// comparison itself is not secret (both lengths are structural, not derived
// from the secret), but the contents are compared in constant time.
bool constantTimeEquals(const std::vector<std::uint8_t>& a,
                        const std::vector<std::uint8_t>& b);

// --- Randomness --------------------------------------------------------------

// Fills out[0..n) from the system CSPRNG. Returns false with `error` set on any
// failure; `out` must then be treated as containing nothing usable.
bool randomBytes(std::uint8_t* out, std::size_t n, std::string& error);

// --- Password hashing --------------------------------------------------------

inline constexpr std::uint32_t kDefaultIterations = 600000;  // OWASP, PBKDF2-HMAC-SHA256
inline constexpr std::size_t kSaltBytes = 16;
inline constexpr std::size_t kHashBytes = 32;  // full SHA-256 output

// Refuse absurd iteration counts when PARSING a record. The floor stops a
// hand-edited config from downgrading an account to a trivially crackable
// hash; the ceiling stops one from turning every login into a denial of
// service against the machine the receiver runs on.
inline constexpr std::uint32_t kMinIterations = 10000;
inline constexpr std::uint32_t kMaxIterations = 10000000;

// Shortest password the server will accept when one is SET. Deliberately a
// policy of this module rather than of the UI, so no caller can bypass it.
inline constexpr std::size_t kMinPasswordLength = 8;

struct PasswordRecord {
    std::uint32_t iterations = kDefaultIterations;
    std::vector<std::uint8_t> salt;
    std::vector<std::uint8_t> hash;

    bool valid() const {
        return iterations >= kMinIterations && iterations <= kMaxIterations &&
               salt.size() == kSaltBytes && hash.size() == kHashBytes;
    }

    // "pbkdf2-sha256$<iterations>$<base64 salt>$<base64 hash>". Returns an
    // empty string if !valid(), so an unset record can never serialize to
    // something that parses.
    std::string serialize() const;

    // Parses the form above. Returns false with `error` set on a wrong or
    // missing algorithm label, a malformed iteration count, an out-of-range
    // iteration count, bad base64, or a wrong salt/key length.
    static bool parse(const std::string& s, PasswordRecord& out, std::string& error);
};

// Derives a new record for `password` with a fresh random salt at
// kDefaultIterations. Fails if the password is shorter than
// kMinPasswordLength, or if the platform has no CSPRNG/PBKDF2 available.
bool hashPassword(const std::string& password, PasswordRecord& out, std::string& error);

// Recomputes the derivation for `password` under `rec`'s salt and iteration
// count and compares it to rec.hash in constant time.
//
// The return value distinguishes "the check ran and the password is wrong"
// (returns false, `error` empty) from "the check could not be performed"
// (returns false, `error` set). Callers must treat both as a failed login —
// the distinction exists so the second can be logged as a fault rather than
// as a bad password, never so a caller can fall back to letting someone in.
bool verifyPassword(const std::string& password, const PasswordRecord& rec,
                    std::string& error);

// --- Sessions ----------------------------------------------------------------

inline constexpr std::size_t kTokenBytes = 32;      // 256 bits
inline constexpr std::size_t kMaxSessions = 32;     // see rule 7
inline constexpr std::int64_t kSessionTtlSeconds = 12 * 60 * 60;

// Issued session tokens, keyed by the SHA-256 of the token (rule 6).
//
// Threading: every method takes the store's own mutex, so the HTTP server may
// call it from any request thread. It is the only mutable state the
// authentication layer owns.
//
// Time is injected rather than read from the clock inside, because a session
// store whose expiry cannot be driven deterministically cannot be tested for
// the one behaviour that matters most about it.
class SessionStore {
public:
    // Creates a session and returns its token in `tokenOut` (base64url, safe
    // to put in a cookie without escaping). Returns false with `error` set if
    // the CSPRNG is unavailable — no session is created in that case.
    bool issue(std::int64_t nowSeconds, std::string& tokenOut, std::string& error);

    // True iff `token` names a live, unexpired session. Expired entries
    // encountered during the scan are dropped. A malformed or unknown token is
    // simply false — never an error, because an unauthenticated caller
    // controls this input entirely.
    bool validate(const std::string& token, std::int64_t nowSeconds);

    // Ends one session (logout). Returns true if a session was removed.
    bool revoke(const std::string& token);

    // Ends every session. Called when the password changes or the server's
    // binding changes: both are moments where anyone already holding a token
    // should have to prove themselves again.
    void revokeAll();

    std::size_t size() const;

private:
    struct Entry {
        std::vector<std::uint8_t> tokenHash;  // SHA-256 of the issued token
        std::int64_t expiresAt = 0;
    };

    mutable std::mutex mutex_;
    std::deque<Entry> entries_;  // oldest first, so eviction is pop_front
};

// SHA-256 over a byte range, exposed because SessionStore needs it and tests
// need to check the store really is keyed by a digest. Returns false on a
// platform or CNG failure.
bool sha256(const std::uint8_t* data, std::size_t n, std::vector<std::uint8_t>& out,
            std::string& error);

// --- Login throttling --------------------------------------------------------

inline constexpr std::size_t kMaxLoginAttempts = 5;
inline constexpr std::int64_t kLoginWindowSeconds = 300;  // 5 minutes
inline constexpr std::size_t kMaxThrottleClients = 1024;

// Per-client failed-login counter.
//
// WHY THIS IS NEEDED AT ALL, given the password is already expensive to test:
// rule 3 in this header measures one PBKDF2 derivation at about 86 ms on this
// hardware, so an unthrottled login endpoint hands an attacker roughly twelve
// guesses a second per core — days, not centuries, against a weak password.
// The hash cost raises the price of an offline attack on a stolen config file;
// only a throttle raises the price of an ONLINE one.
//
// It counts FAILURES, not attempts: a working client logging in repeatedly is
// never locked out, and a successful login clears the client's counter, so the
// only way to reach the limit is to keep getting it wrong.
//
// Keyed by whatever the caller considers a client identity (an IP string, in
// practice). The map is capped at kMaxThrottleClients because an
// unauthenticated caller can cause entries to be created — see the same
// reasoning behind SessionStore's cap. When full, the oldest entry is evicted;
// that is a deliberate trade, since the alternative (refusing new entries)
// would let an attacker fill the table and then attack unthrottled.
//
// Time is injected, for the same reason SessionStore injects it: a throttle
// whose window cannot be driven deterministically cannot be tested.
class LoginThrottle {
public:
    // True if this client may attempt a login now.
    bool allow(const std::string& client, std::int64_t nowSeconds) const;

    // Records a failed attempt. Returns the number of failures now held for
    // this client within the window.
    std::size_t recordFailure(const std::string& client, std::int64_t nowSeconds);

    // Clears a client's failures after a successful login.
    void recordSuccess(const std::string& client);

    // Seconds until this client may try again; 0 when it may try now.
    std::int64_t retryAfterSeconds(const std::string& client,
                                   std::int64_t nowSeconds) const;

    void clear();
    std::size_t size() const;

private:
    struct Entry {
        std::string client;
        std::size_t failures = 0;
        std::int64_t windowStart = 0;  // first failure of the current window
    };

    // Returns nullptr when absent. Caller holds mutex_.
    const Entry* find(const std::string& client) const;

    mutable std::mutex mutex_;
    std::deque<Entry> entries_;  // oldest first, so eviction is pop_front
};

}  // namespace cascade::net
