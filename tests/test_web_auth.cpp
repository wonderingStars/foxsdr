// Tests for the web-server authentication primitives (net/web_auth.hpp).
//
// GROUND TRUTH. The PBKDF2 and SHA-256 expectations below were produced by
// Python 3.14.6's hashlib (OpenSSL-backed), an implementation that shares no
// code with the Windows CNG path under test, and the SHA-256 value is the
// published NIST vector for "abc". That matters here for the same reason it
// matters in the plugin catalogue: a hash function checked only against itself
// proves the code is self-consistent, not that it computes the standard. The
// base64 expectations are the RFC 4648 section 10 test vectors.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdint>
#include <string>
#include <vector>

#include "net/web_auth.hpp"
#include "test_check.hpp"

using namespace cascade::net;

namespace {

std::string toHex(const std::vector<std::uint8_t>& v) {
    static const char* kDigits = "0123456789abcdef";
    std::string s;
    s.reserve(v.size() * 2);
    for (std::uint8_t b : v) {
        s.push_back(kDigits[(b >> 4) & 0x0F]);
        s.push_back(kDigits[b & 0x0F]);
    }
    return s;
}

std::vector<std::uint8_t> bytesOf(const std::string& s) {
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

// A record built from known values, for the parse/serialize and known-answer
// tests that must not pay for a 600,000-iteration derivation.
PasswordRecord knownRecord() {
    PasswordRecord r;
    r.iterations = 10000;
    r.salt.resize(kSaltBytes);
    for (std::size_t i = 0; i < kSaltBytes; ++i) {
        r.salt[i] = static_cast<std::uint8_t>(i);
    }
    r.hash.assign(kHashBytes, 0);
    return r;
}

void testBase64Vectors() {
    // RFC 4648 section 10.
    const std::pair<std::string, std::string> kVectors[] = {
        {"", ""},          {"f", "Zg=="},         {"fo", "Zm8="},
        {"foo", "Zm9v"},   {"foob", "Zm9vYg=="},  {"fooba", "Zm9vYmE="},
        {"foobar", "Zm9vYmFy"},
    };
    for (const auto& v : kVectors) {
        const std::vector<std::uint8_t> raw = bytesOf(v.first);
        CHECK(base64Encode(raw) == v.second);
        std::vector<std::uint8_t> decoded;
        CHECK(base64Decode(v.second, decoded));
        CHECK(decoded == raw);
    }
}

void testBase64RoundTrip() {
    // Every length through 64 bytes, so all three padding cases are exercised
    // repeatedly rather than only at the boundaries the vectors happen to hit.
    for (std::size_t n = 0; n <= 64; ++n) {
        std::vector<std::uint8_t> raw(n);
        for (std::size_t i = 0; i < n; ++i) {
            raw[i] = static_cast<std::uint8_t>((i * 37 + n) & 0xFF);
        }
        const std::string encoded = base64Encode(raw);
        CHECK(encoded.size() % 4 == 0);
        std::vector<std::uint8_t> decoded;
        CHECK(base64Decode(encoded, decoded));
        CHECK(decoded == raw);
    }
}

void testBase64Strictness() {
    std::vector<std::uint8_t> out;
    // Length not a multiple of four.
    CHECK(!base64Decode("Zm9vY", out));
    CHECK(!base64Decode("A", out));
    // Characters outside the alphabet, including the whitespace a lenient
    // decoder would skip.
    CHECK(!base64Decode("Zm9 v", out));
    CHECK(!base64Decode("Zm9v\n", out));
    CHECK(!base64Decode("Zm9*", out));
    CHECK(!base64Decode("-_==", out));  // base64url is not accepted here
    // Padding in the wrong place.
    CHECK(!base64Decode("=m9v", out));
    CHECK(!base64Decode("Z=9v", out));
    CHECK(!base64Decode("Zm=vZm9v", out));  // padded group that is not the last
    CHECK(!base64Decode("Zg==Zg==", out));
    CHECK(!base64Decode("Zm=v", out));      // '=' followed by a non-'='
    // Discarded bits must be zero: "Zh==" and "Zg==" would otherwise both
    // decode to the single byte 'f'.
    CHECK(base64Decode("Zg==", out));
    CHECK(!base64Decode("Zh==", out));
    CHECK(base64Decode("Zm8=", out));
    CHECK(!base64Decode("Zm9=", out));
    // A failed decode must leave nothing behind for a careless caller to use.
    CHECK(!base64Decode("Zm9*", out));
    CHECK(out.empty());
}

void testConstantTimeEquals() {
    const std::vector<std::uint8_t> a = bytesOf("hello world");
    const std::vector<std::uint8_t> b = bytesOf("hello world");
    const std::vector<std::uint8_t> c = bytesOf("hello worlD");
    const std::vector<std::uint8_t> shortOne = bytesOf("hello");
    CHECK(constantTimeEquals(a, b));
    CHECK(!constantTimeEquals(a, c));
    CHECK(!constantTimeEquals(a, shortOne));
    CHECK(constantTimeEquals(std::vector<std::uint8_t>{}, std::vector<std::uint8_t>{}));
    // Differences in the FIRST byte and the LAST byte must both be caught; an
    // implementation that returned early would still pass the first of these.
    std::vector<std::uint8_t> firstDiff = a;
    firstDiff.front() ^= 0x01;
    std::vector<std::uint8_t> lastDiff = a;
    lastDiff.back() ^= 0x01;
    CHECK(!constantTimeEquals(a, firstDiff));
    CHECK(!constantTimeEquals(a, lastDiff));
}

void testSha256KnownAnswer() {
    const std::vector<std::uint8_t> input = bytesOf("abc");
    std::vector<std::uint8_t> digest;
    std::string error;
    CHECK(sha256(input.data(), input.size(), digest, error));
    CHECK(error.empty());
    CHECK(digest.size() == kHashBytes);
    CHECK(toHex(digest) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

void testRandomBytes() {
    std::string error;
    std::vector<std::uint8_t> a(32, 0);
    std::vector<std::uint8_t> b(32, 0);
    CHECK(randomBytes(a.data(), a.size(), error));
    CHECK(error.empty());
    CHECK(randomBytes(b.data(), b.size(), error));
    CHECK(a != b);  // 2^-256 chance of a false failure
    // All-zero output would mean the buffer was never written.
    CHECK(a != std::vector<std::uint8_t>(32, 0));
    // A zero-length request is a no-op success, not an error.
    CHECK(randomBytes(nullptr, 0, error));
}

void testPbkdf2KnownAnswer() {
    // Computed independently by Python 3.14.6:
    //   hashlib.pbkdf2_hmac('sha256', b'correct horse battery staple',
    //                       bytes(range(16)), 10000, 32).hex()
    PasswordRecord rec = knownRecord();
    std::vector<std::uint8_t> expected;
    CHECK(base64Decode("2flfZcLfnShdJogjAMpb4p4+1QBVZmODXExi4nBRUCI=", expected));
    rec.hash = expected;
    CHECK(rec.valid());
    CHECK(toHex(rec.hash) ==
          "d9f95f65c2df9d285d26882300ca5be29e3ed500556663835c4c62e270515022");

    std::string error;
    CHECK(verifyPassword("correct horse battery staple", rec, error));
    CHECK(error.empty());
    // A one-character difference must fail, and must fail as a wrong password
    // rather than as a fault.
    CHECK(!verifyPassword("correct horse battery stapl", rec, error));
    CHECK(error.empty());
}

void testPasswordHashRoundTrip() {
    std::string error;
    PasswordRecord rec;
    CHECK(hashPassword("a-good-long-password", rec, error));
    CHECK(error.empty());
    CHECK(rec.valid());
    CHECK(rec.iterations == kDefaultIterations);
    CHECK(rec.salt.size() == kSaltBytes);
    CHECK(rec.hash.size() == kHashBytes);

    CHECK(verifyPassword("a-good-long-password", rec, error));
    CHECK(!verifyPassword("a-good-long-passwore", rec, error));
    CHECK(error.empty());

    // A second derivation of the SAME password must differ, or the salt is not
    // random and the whole record is a rainbow-table lookup.
    PasswordRecord rec2;
    CHECK(hashPassword("a-good-long-password", rec2, error));
    CHECK(rec2.salt != rec.salt);
    CHECK(rec2.hash != rec.hash);
    // ...and must still verify.
    CHECK(verifyPassword("a-good-long-password", rec2, error));
}

// A WRONG USER NAME MUST COST WHAT A WRONG PASSWORD COSTS.
//
// What leaks here is time, but a wall-clock assertion flakes on a loaded
// machine, so this counts derivations instead: the answer is exactly one
// PBKDF2 either way, and a count is not a measurement. Short-circuiting the
// name check makes it 0 against 1, which is the ~86 ms gap that enumerates
// account names.
void testLoginCostsTheSameForAWrongUserAsForAWrongPassword() {
    std::string error;
    PasswordRecord rec;
    CHECK(hashPassword("a-good-long-password", rec, error));
    CHECK(error.empty());

    const std::uint64_t beforeUser = pbkdf2CallCount();
    CHECK(!verifyLogin("mallory", "a-good-long-password", "admin", rec, error));
    const std::uint64_t wrongUserCost = pbkdf2CallCount() - beforeUser;
    CHECK(error.empty());  // wrong credentials, not a fault

    const std::uint64_t beforePass = pbkdf2CallCount();
    CHECK(!verifyLogin("admin", "a-good-long-passwore", "admin", rec, error));
    const std::uint64_t wrongPassCost = pbkdf2CallCount() - beforePass;
    CHECK(error.empty());

    CHECK(wrongUserCost == wrongPassCost);
    CHECK(wrongUserCost == 1u);

    // ...and the real pair still gets in, while a right password under any
    // other name still does not.
    CHECK(verifyLogin("admin", "a-good-long-password", "admin", rec, error));
    CHECK(error.empty());
    CHECK(!verifyLogin("Admin", "a-good-long-password", "admin", rec, error));
    CHECK(!verifyLogin("", "a-good-long-password", "admin", rec, error));
}

void testPasswordPolicy() {
    std::string error;
    PasswordRecord rec;
    CHECK(!hashPassword("", rec, error));
    CHECK(!error.empty());
    CHECK(!rec.valid());
    CHECK(!hashPassword("short12", rec, error));  // 7 characters
    CHECK(!rec.valid());
    // Exactly kMinPasswordLength is accepted.
    CHECK(hashPassword("short123", rec, error));
    CHECK(rec.valid());
}

void testRecordSerializeParse() {
    PasswordRecord rec = knownRecord();
    for (std::size_t i = 0; i < kHashBytes; ++i) {
        rec.hash[i] = static_cast<std::uint8_t>(255 - i);
    }
    const std::string text = rec.serialize();
    CHECK(text.rfind("pbkdf2-sha256$10000$", 0) == 0);

    PasswordRecord parsed;
    std::string error;
    CHECK(PasswordRecord::parse(text, parsed, error));
    CHECK(error.empty());
    CHECK(parsed.iterations == rec.iterations);
    CHECK(parsed.salt == rec.salt);
    CHECK(parsed.hash == rec.hash);
    CHECK(parsed.serialize() == text);

    // An unset record must not serialize to something that parses.
    const PasswordRecord empty;
    CHECK(empty.serialize().empty());
    CHECK(!PasswordRecord::parse("", parsed, error));
}

void testRecordParseRejects() {
    const PasswordRecord good = []() {
        PasswordRecord r = knownRecord();
        for (std::size_t i = 0; i < kHashBytes; ++i) {
            r.hash[i] = static_cast<std::uint8_t>(i);
        }
        return r;
    }();
    const std::string saltB64 = base64Encode(good.salt);
    const std::string hashB64 = base64Encode(good.hash);

    PasswordRecord out;
    std::string error;

    const std::string kBad[] = {
        "",
        "pbkdf2-sha256$",
        "bcrypt$10000$" + saltB64 + "$" + hashB64,        // wrong algorithm
        "pbkdf2-sha1$10000$" + saltB64 + "$" + hashB64,   // near-miss algorithm
        "pbkdf2-sha256$10000$" + saltB64,                 // missing key field
        "pbkdf2-sha256$10000$" + saltB64 + "$" + hashB64 + "$extra",
        "pbkdf2-sha256$$" + saltB64 + "$" + hashB64,      // empty iterations
        "pbkdf2-sha256$ten$" + saltB64 + "$" + hashB64,   // non-numeric
        "pbkdf2-sha256$-5$" + saltB64 + "$" + hashB64,    // negative
        "pbkdf2-sha256$1000$" + saltB64 + "$" + hashB64,  // below kMinIterations
        "pbkdf2-sha256$99999999999$" + saltB64 + "$" + hashB64,  // above kMax
        "pbkdf2-sha256$10000$!!!!$" + hashB64,            // salt not base64
        "pbkdf2-sha256$10000$" + saltB64 + "$!!!!",       // key not base64
        "pbkdf2-sha256$10000$Zg==$" + hashB64,            // salt too short
        "pbkdf2-sha256$10000$" + saltB64 + "$Zg==",       // key too short
    };
    for (const std::string& bad : kBad) {
        const bool accepted = PasswordRecord::parse(bad, out, error);
        CHECK(!accepted);
        if (!accepted) {
            CHECK(!error.empty());
            CHECK(!out.valid());
        }
    }

    // The same string WITHOUT any of those defects must parse, so the loop
    // above is rejecting the defect and not the shape.
    CHECK(PasswordRecord::parse("pbkdf2-sha256$10000$" + saltB64 + "$" + hashB64,
                                out, error));
}

void testSessionIssueAndValidate() {
    SessionStore store;
    std::string token;
    std::string error;
    CHECK(store.issue(1000, token, error));
    CHECK(error.empty());
    CHECK(!token.empty());
    CHECK(store.size() == 1);

    CHECK(store.validate(token, 1000));
    CHECK(store.validate(token, 1000 + kSessionTtlSeconds - 1));

    // Unknown, empty and near-miss tokens are all rejected.
    CHECK(!store.validate("", 1000));
    CHECK(!store.validate("not-a-real-token", 1000));
    std::string mangled = token;
    mangled.back() = (mangled.back() == 'A') ? 'B' : 'A';
    CHECK(!store.validate(mangled, 1000));

    // Two issued tokens must differ.
    std::string token2;
    CHECK(store.issue(1000, token2, error));
    CHECK(token2 != token);
    CHECK(store.validate(token, 1000));
    CHECK(store.validate(token2, 1000));
}

void testSessionExpiry() {
    SessionStore store;
    std::string token;
    std::string error;
    CHECK(store.issue(0, token, error));
    CHECK(store.validate(token, kSessionTtlSeconds - 1));
    // At exactly the TTL the session is over.
    CHECK(!store.validate(token, kSessionTtlSeconds));
    // ...and the expired entry was dropped rather than accumulating.
    CHECK(store.size() == 0);
}

void testSessionRevoke() {
    SessionStore store;
    std::string a;
    std::string b;
    std::string error;
    CHECK(store.issue(0, a, error));
    CHECK(store.issue(0, b, error));
    CHECK(store.revoke(a));
    CHECK(!store.validate(a, 0));
    CHECK(store.validate(b, 0));
    CHECK(!store.revoke(a));  // already gone
    CHECK(store.size() == 1);

    store.revokeAll();
    CHECK(store.size() == 0);
    CHECK(!store.validate(b, 0));
}

void testSessionCap() {
    SessionStore store;
    std::string error;
    std::vector<std::string> tokens;
    for (std::size_t i = 0; i < kMaxSessions + 5; ++i) {
        std::string t;
        CHECK(store.issue(0, t, error));
        tokens.push_back(t);
    }
    CHECK(store.size() == kMaxSessions);
    // The five oldest were evicted; the newest kMaxSessions survive.
    std::size_t alive = 0;
    std::size_t dead = 0;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (store.validate(tokens[i], 0)) {
            ++alive;
        } else {
            ++dead;
        }
    }
    CHECK(alive == kMaxSessions);
    CHECK(dead == 5);
}

}  // namespace

int main() {
    testBase64Vectors();
    testBase64RoundTrip();
    testBase64Strictness();
    testConstantTimeEquals();
    testSha256KnownAnswer();
    testRandomBytes();
    testPbkdf2KnownAnswer();
    testPasswordHashRoundTrip();
    testLoginCostsTheSameForAWrongUserAsForAWrongPassword();
    testPasswordPolicy();
    testRecordSerializeParse();
    testRecordParseRejects();
    testSessionIssueAndValidate();
    testSessionExpiry();
    testSessionRevoke();
    testSessionCap();
    return testSummary("test_web_auth");
}
