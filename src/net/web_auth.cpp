// Implementation of the web-server authentication primitives. See
// net/web_auth.hpp for the seven rules this file exists to enforce.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/web_auth.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")
#endif  // _WIN32

namespace cascade::net {
namespace {

constexpr char kB64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int b64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// base64url without padding, for values that travel in a cookie or a URL.
// Derived from the standard encoding rather than reimplemented so the two can
// never disagree about the bits.
std::string toBase64Url(const std::vector<std::uint8_t>& data) {
    std::string s = base64Encode(data);
    while (!s.empty() && s.back() == '=') {
        s.pop_back();
    }
    for (char& c : s) {
        if (c == '+') {
            c = '-';
        } else if (c == '/') {
            c = '_';
        }
    }
    return s;
}

#if defined(_WIN32)
std::string ntStatusText(const char* what, NTSTATUS st) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(st));
    return std::string(what) + " failed (NTSTATUS " + buf + ")";
}
#else
// One place to produce the refusal, so every entry point in this file reports
// the same thing on a platform where the product does not yet ship a server.
bool unsupported(std::string& error) {
    error = "cryptographic services are only implemented on Windows in this build";
    return false;
}
#endif

}  // namespace

// --- Encoding and comparison helpers ----------------------------------------

std::string base64Encode(const std::uint8_t* data, std::size_t n) {
    std::string out;
    out.reserve(((n + 2) / 3) * 4);
    std::size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        const std::uint32_t t = (static_cast<std::uint32_t>(data[i]) << 16) |
                                (static_cast<std::uint32_t>(data[i + 1]) << 8) |
                                static_cast<std::uint32_t>(data[i + 2]);
        out.push_back(kB64Alphabet[(t >> 18) & 0x3F]);
        out.push_back(kB64Alphabet[(t >> 12) & 0x3F]);
        out.push_back(kB64Alphabet[(t >> 6) & 0x3F]);
        out.push_back(kB64Alphabet[t & 0x3F]);
    }
    const std::size_t rem = n - i;
    if (rem == 1) {
        const std::uint32_t t = static_cast<std::uint32_t>(data[i]) << 16;
        out.push_back(kB64Alphabet[(t >> 18) & 0x3F]);
        out.push_back(kB64Alphabet[(t >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2) {
        const std::uint32_t t = (static_cast<std::uint32_t>(data[i]) << 16) |
                                (static_cast<std::uint32_t>(data[i + 1]) << 8);
        out.push_back(kB64Alphabet[(t >> 18) & 0x3F]);
        out.push_back(kB64Alphabet[(t >> 12) & 0x3F]);
        out.push_back(kB64Alphabet[(t >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

std::string base64Encode(const std::vector<std::uint8_t>& data) {
    return base64Encode(data.data(), data.size());
}

bool base64Decode(const std::string& in, std::vector<std::uint8_t>& out) {
    out.clear();
    if (in.size() % 4 != 0) {
        return false;
    }
    if (in.empty()) {
        return true;
    }
    const std::size_t groups = in.size() / 4;
    out.reserve(groups * 3);
    for (std::size_t g = 0; g < groups; ++g) {
        const char* p = in.data() + g * 4;
        const bool last = (g + 1 == groups);
        int v[4] = {0, 0, 0, 0};
        std::size_t pad = 0;
        for (int i = 0; i < 4; ++i) {
            if (p[i] == '=') {
                // Padding is legal only in the final group, and only as the
                // last one or two characters of it — "=AAA" and "AB=C" are
                // both malformed, not "mostly fine".
                if (!last || i < 2) {
                    out.clear();
                    return false;
                }
                for (int j = i; j < 4; ++j) {
                    if (p[j] != '=') {
                        out.clear();
                        return false;
                    }
                }
                pad = 4 - static_cast<std::size_t>(i);
                break;
            }
            v[i] = b64Value(p[i]);
            if (v[i] < 0) {
                out.clear();
                return false;
            }
        }
        // The bits that padding discards must actually be zero. Without this a
        // record has multiple valid encodings, and "does this string equal the
        // stored one" stops being the same question as "do these bytes match".
        if (pad == 2 && (v[1] & 0x0F) != 0) {
            out.clear();
            return false;
        }
        if (pad == 1 && (v[2] & 0x03) != 0) {
            out.clear();
            return false;
        }
        const std::uint32_t t = (static_cast<std::uint32_t>(v[0]) << 18) |
                                (static_cast<std::uint32_t>(v[1]) << 12) |
                                (static_cast<std::uint32_t>(v[2]) << 6) |
                                static_cast<std::uint32_t>(v[3]);
        out.push_back(static_cast<std::uint8_t>((t >> 16) & 0xFF));
        if (pad < 2) {
            out.push_back(static_cast<std::uint8_t>((t >> 8) & 0xFF));
        }
        if (pad < 1) {
            out.push_back(static_cast<std::uint8_t>(t & 0xFF));
        }
    }
    return true;
}

bool constantTimeEquals(const void* a, const void* b, std::size_t n) {
    const auto* pa = static_cast<const unsigned char*>(a);
    const auto* pb = static_cast<const unsigned char*>(b);
    unsigned char diff = 0;
    for (std::size_t i = 0; i < n; ++i) {
        diff = static_cast<unsigned char>(diff | (pa[i] ^ pb[i]));
    }
    return diff == 0;
}

bool constantTimeEquals(const std::vector<std::uint8_t>& a,
                        const std::vector<std::uint8_t>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    if (a.empty()) {
        return true;
    }
    return constantTimeEquals(a.data(), b.data(), a.size());
}

// --- Randomness and digests --------------------------------------------------

bool randomBytes(std::uint8_t* out, std::size_t n, std::string& error) {
    error.clear();
    if (n == 0) {
        return true;
    }
    if (out == nullptr) {
        error = "randomBytes: null destination";
        return false;
    }
#if defined(_WIN32)
    const NTSTATUS st = ::BCryptGenRandom(nullptr, out, static_cast<ULONG>(n),
                                          BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (!BCRYPT_SUCCESS(st)) {
        error = ntStatusText("BCryptGenRandom", st);
        return false;
    }
    return true;
#else
    return unsupported(error);
#endif
}

bool sha256(const std::uint8_t* data, std::size_t n, std::vector<std::uint8_t>& out,
            std::string& error) {
    error.clear();
    out.clear();
#if defined(_WIN32)
    BCRYPT_ALG_HANDLE alg = nullptr;
    NTSTATUS st = ::BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(st)) {
        error = ntStatusText("BCryptOpenAlgorithmProvider(SHA256)", st);
        return false;
    }
    out.resize(kHashBytes);
    st = ::BCryptHash(alg, nullptr, 0, const_cast<PUCHAR>(data), static_cast<ULONG>(n),
                      out.data(), static_cast<ULONG>(out.size()));
    ::BCryptCloseAlgorithmProvider(alg, 0);
    if (!BCRYPT_SUCCESS(st)) {
        out.clear();
        error = ntStatusText("BCryptHash(SHA256)", st);
        return false;
    }
    return true;
#else
    (void)data;
    (void)n;
    return unsupported(error);
#endif
}

// --- Password hashing --------------------------------------------------------

namespace {

#if defined(_WIN32)
bool pbkdf2Sha256(const std::string& password, const std::uint8_t* salt,
                  std::size_t saltLen, std::uint32_t iterations, std::uint8_t* out,
                  std::size_t outLen, std::string& error) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    // BCRYPT_ALG_HANDLE_HMAC_FLAG is what makes this handle usable as the PRF;
    // without it BCryptDeriveKeyPBKDF2 fails with STATUS_INVALID_HANDLE.
    NTSTATUS st = ::BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr,
                                                BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (!BCRYPT_SUCCESS(st)) {
        error = ntStatusText("BCryptOpenAlgorithmProvider(SHA256,HMAC)", st);
        return false;
    }
    st = ::BCryptDeriveKeyPBKDF2(
        alg, reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())),
        static_cast<ULONG>(password.size()), const_cast<PUCHAR>(salt),
        static_cast<ULONG>(saltLen), static_cast<ULONGLONG>(iterations), out,
        static_cast<ULONG>(outLen), 0);
    ::BCryptCloseAlgorithmProvider(alg, 0);
    if (!BCRYPT_SUCCESS(st)) {
        error = ntStatusText("BCryptDeriveKeyPBKDF2", st);
        return false;
    }
    return true;
}
#endif  // _WIN32

}  // namespace

std::string PasswordRecord::serialize() const {
    if (!valid()) {
        return std::string();
    }
    return "pbkdf2-sha256$" + std::to_string(iterations) + "$" + base64Encode(salt) +
           "$" + base64Encode(hash);
}

bool PasswordRecord::parse(const std::string& s, PasswordRecord& out,
                           std::string& error) {
    error.clear();
    out = PasswordRecord{};

    const std::string prefix = "pbkdf2-sha256$";
    if (s.size() <= prefix.size() || s.compare(0, prefix.size(), prefix) != 0) {
        error = "password record: unrecognised algorithm label";
        return false;
    }
    const std::size_t iterStart = prefix.size();
    const std::size_t saltSep = s.find('$', iterStart);
    if (saltSep == std::string::npos) {
        error = "password record: missing salt field";
        return false;
    }
    const std::size_t hashSep = s.find('$', saltSep + 1);
    if (hashSep == std::string::npos) {
        error = "password record: missing key field";
        return false;
    }
    // No fourth field is permitted: a trailing '$' section would mean this
    // string was written by something that is not this code, and guessing at
    // its meaning is exactly how a credential check ends up parsing an
    // attacker's shape.
    if (s.find('$', hashSep + 1) != std::string::npos) {
        error = "password record: unexpected trailing field";
        return false;
    }

    const std::string iterText = s.substr(iterStart, saltSep - iterStart);
    if (iterText.empty() ||
        iterText.find_first_not_of("0123456789") != std::string::npos) {
        error = "password record: iteration count is not a decimal number";
        return false;
    }
    unsigned long long iters = 0;
    try {
        iters = std::stoull(iterText);
    } catch (const std::exception&) {
        error = "password record: iteration count out of range";
        return false;
    }
    if (iters < kMinIterations || iters > kMaxIterations) {
        error = "password record: iteration count outside the accepted range";
        return false;
    }

    PasswordRecord rec;
    rec.iterations = static_cast<std::uint32_t>(iters);
    if (!base64Decode(s.substr(saltSep + 1, hashSep - saltSep - 1), rec.salt)) {
        error = "password record: salt is not valid base64";
        return false;
    }
    if (!base64Decode(s.substr(hashSep + 1), rec.hash)) {
        error = "password record: key is not valid base64";
        return false;
    }
    if (rec.salt.size() != kSaltBytes) {
        error = "password record: wrong salt length";
        return false;
    }
    if (rec.hash.size() != kHashBytes) {
        error = "password record: wrong key length";
        return false;
    }
    out = rec;
    return true;
}

bool hashPassword(const std::string& password, PasswordRecord& out,
                  std::string& error) {
    error.clear();
    out = PasswordRecord{};
    if (password.size() < kMinPasswordLength) {
        error = "password must be at least " + std::to_string(kMinPasswordLength) +
                " characters";
        return false;
    }
#if defined(_WIN32)
    PasswordRecord rec;
    rec.iterations = kDefaultIterations;
    rec.salt.resize(kSaltBytes);
    if (!randomBytes(rec.salt.data(), rec.salt.size(), error)) {
        return false;
    }
    rec.hash.resize(kHashBytes);
    if (!pbkdf2Sha256(password, rec.salt.data(), rec.salt.size(), rec.iterations,
                      rec.hash.data(), rec.hash.size(), error)) {
        return false;
    }
    out = rec;
    return true;
#else
    return unsupported(error);
#endif
}

bool verifyPassword(const std::string& password, const PasswordRecord& rec,
                    std::string& error) {
    error.clear();
    if (!rec.valid()) {
        error = "password record is not usable";
        return false;
    }
#if defined(_WIN32)
    std::vector<std::uint8_t> derived(kHashBytes);
    if (!pbkdf2Sha256(password, rec.salt.data(), rec.salt.size(), rec.iterations,
                      derived.data(), derived.size(), error)) {
        return false;
    }
    // error stays empty here: a mismatch is a wrong password, not a fault, and
    // the caller distinguishes the two by exactly that (see the header).
    return constantTimeEquals(derived, rec.hash);
#else
    (void)password;
    return unsupported(error);
#endif
}

// --- Sessions ----------------------------------------------------------------

bool SessionStore::issue(std::int64_t nowSeconds, std::string& tokenOut,
                         std::string& error) {
    error.clear();
    tokenOut.clear();

    std::vector<std::uint8_t> token(kTokenBytes);
    if (!randomBytes(token.data(), token.size(), error)) {
        return false;
    }
    const std::string encoded = toBase64Url(token);

    std::vector<std::uint8_t> digest;
    if (!sha256(reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size(),
                digest, error)) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Drop anything already expired before applying the cap, so a long-idle
        // server does not evict a live session to make room while holding a
        // deque full of dead ones.
        while (!entries_.empty() && entries_.front().expiresAt <= nowSeconds) {
            entries_.pop_front();
        }
        while (entries_.size() >= kMaxSessions) {
            entries_.pop_front();
        }
        Entry e;
        e.tokenHash = std::move(digest);
        e.expiresAt = nowSeconds + kSessionTtlSeconds;
        entries_.push_back(std::move(e));
    }

    tokenOut = encoded;
    return true;
}

bool SessionStore::validate(const std::string& token, std::int64_t nowSeconds) {
    if (token.empty()) {
        return false;
    }
    std::vector<std::uint8_t> digest;
    std::string ignored;
    if (!sha256(reinterpret_cast<const std::uint8_t*>(token.data()), token.size(),
                digest, ignored)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    bool found = false;
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->expiresAt <= nowSeconds) {
            it = entries_.erase(it);
            continue;
        }
        // No early exit: the scan visits every live entry whether or not it has
        // already matched, so the time this takes does not reveal WHERE in the
        // store a token sits, or whether it is present at all.
        if (constantTimeEquals(it->tokenHash, digest)) {
            found = true;
        }
        ++it;
    }
    return found;
}

bool SessionStore::revoke(const std::string& token) {
    if (token.empty()) {
        return false;
    }
    std::vector<std::uint8_t> digest;
    std::string ignored;
    if (!sha256(reinterpret_cast<const std::uint8_t*>(token.data()), token.size(),
                digest, ignored)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (constantTimeEquals(it->tokenHash, digest)) {
            entries_.erase(it);
            return true;
        }
    }
    return false;
}

void SessionStore::revokeAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
}

std::size_t SessionStore::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

}  // namespace cascade::net
