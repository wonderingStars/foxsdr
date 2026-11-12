// sys_hash.hpp - SHA-256 and SHA-512, written from FIPS 180-4, with no operating-system provider behind them.
//
// WHY OWN CODE. The Ed25519 verifier (sys_ed25519.hpp) needs SHA-512 and the settings bundle (sys_bundle.hpp) needs SHA-256 per file. Windows has both in
// CNG, but every call into a provider is a place a prompt or a missing provider can show up, and this lane's rule is that nothing here may touch a
// key provider or a certificate store at all. These are pure functions of their input bytes; the constants come from tools/gen_sha_constants.py
// (the definition in FIPS 180-4, computed with exact integers), and the tests pin the published digests of "", "abc" and the two-block message.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_SYS_HASH_HPP
#define CASCADE_CORE_SYS_HASH_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace cascade::core::sys {

using Digest256 = std::array<std::uint8_t, 32>;
using Digest512 = std::array<std::uint8_t, 64>;

class Sha256 {
public:
    Sha256();
    void update(const void* data, std::size_t n);
    Digest256 finish();   // once; the object is spent afterwards

private:
    void block(const std::uint8_t* p);
    std::uint32_t h_[8];
    std::uint8_t buf_[64];
    std::size_t fill_ = 0;
    std::uint64_t total_ = 0;
};

class Sha512 {
public:
    Sha512();
    void update(const void* data, std::size_t n);
    Digest512 finish();

private:
    void block(const std::uint8_t* p);
    std::uint64_t h_[8];
    std::uint8_t buf_[128];
    std::size_t fill_ = 0;
    std::uint64_t total_ = 0;   // bytes; a message over 2^61 bytes is not a thing a process can hold
};

Digest256 sha256(const void* data, std::size_t n);
Digest512 sha512(const void* data, std::size_t n);

// Lower-case hex of any byte run, and its inverse (false on odd length or a non-hex digit; `out` is then empty).
std::string toHex(const std::uint8_t* p, std::size_t n);
bool fromHex(const std::string& hex, std::string& out);
std::string sha256Hex(const std::string& bytes);

}  // namespace cascade::core::sys

#endif  // CASCADE_CORE_SYS_HASH_HPP
