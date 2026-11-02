// test_engine_api_secret_compare.cpp - THE TABLE'S CREDENTIAL CHECKS COMPARE IN CONSTANT TIME (deep review of engine/stage4, batch A; docs/engine-stage4.md 6.31).
//
// The engine's table has a login (user + password -> a session token) and a configured static token (the API's `token=` option, what the web server and a
// future transport open sessions with). Its first version compared the typed credential with `==` / `!=`: an early-exit compare, whose time says how many
// leading characters were right. The web server's own login never did that (net/web_auth.cpp compares through constantTimeEquals and always pays the key
// derivation); the table's now compares through the same family (net::constantTimeEqualsText, which reads every byte of the longer string whatever the
// contents) in every place it holds a secret: login (user and password), tokenValid (the static token) and logout (the static token cannot be revoked).
//
//   A  BEHAVIOUR IS UNCHANGED: near misses of every shape - a prefix, a last character wrong, a longer string, the empty string, another case - are refused
//      exactly as before; the right credential still logs in and opens a session.
//   B  THE COMPARE SITES CALL THE HELPER, and no `==` or `!=` on opt_.user, opt_.password or opt_.token is left in the table's source. (Time cannot be
//      asserted in a test - the host's noise is larger than one byte's compare - so the property is held where it can be: at the call sites.)
//   C  THE HELPER ITSELF: equal, differing at the first / last byte, differing in length either way, empty, embedded NUL (a NUL does not end the compare).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "check.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine_api_serial.hpp"
#include "engine_api_test_env.hpp"
#include "engine_client.hpp"
#include "foxsdr_api.h"
#include "net/web_auth.hpp"

#ifndef CASCADE_SOURCE_DIR
#error "CASCADE_SOURCE_DIR must be defined by the build"
#endif

namespace fs = std::filesystem;
using testing::kAllGrants;

namespace {

const FoxEngineApi* api() { return cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR); }

// ---- A ----------------------------------------------------------------------------------------------------------------------------
void testBehaviourIsUnchanged() {
    std::printf("  A: near misses are refused as ever, the right credential is accepted\n");
    // (the lockout is 5 failures: each engine below stays under it before its success)
    const FoxEngineApi* a = api();
    char tok[80] = {};
    {
        engine_api_test::SerialEngine e(a, "testHooks=1;user=operator;password=correct-horse-battery;loginLockoutMs=10");
        CHECK_EQ(a->login(e.raw(), "operator", "correct-horse-batter", tok, sizeof(tok)), FOXAPI_UNAUTHENTICATED);       // a prefix
        CHECK_EQ(a->login(e.raw(), "operator", "correct-horse-batterx", tok, sizeof(tok)), FOXAPI_UNAUTHENTICATED);      // the last character wrong
        CHECK_EQ(a->login(e.raw(), "operator", "correct-horse-battery!", tok, sizeof(tok)), FOXAPI_UNAUTHENTICATED);     // longer
        CHECK_EQ(a->login(e.raw(), "operator", "Correct-horse-battery", tok, sizeof(tok)), FOXAPI_UNAUTHENTICATED);      // the first character wrong
        CHECK_EQ(a->login(e.raw(), "operator", "correct-horse-battery", tok, sizeof(tok)), 64);                         // and the right pair
        CHECK_EQ(std::strlen(tok), static_cast<std::size_t>(64));
    }
    {
        engine_api_test::SerialEngine e(a, "testHooks=1;user=operator;password=correct-horse-battery;loginLockoutMs=10");
        CHECK_EQ(a->login(e.raw(), "operator", "", tok, sizeof(tok)), FOXAPI_UNAUTHENTICATED);
        CHECK_EQ(a->login(e.raw(), "", "correct-horse-battery", tok, sizeof(tok)), FOXAPI_UNAUTHENTICATED);
        CHECK_EQ(a->login(e.raw(), "OPERATOR", "correct-horse-battery", tok, sizeof(tok)), FOXAPI_UNAUTHENTICATED);
        CHECK_EQ(a->login(e.raw(), "operato", "correct-horse-battery", tok, sizeof(tok)), FOXAPI_UNAUTHENTICATED);   // the user is a prefix
        CHECK_EQ(a->login(e.raw(), "operator", "correct-horse-battery", tok, sizeof(tok)), 64);
    }
    {
        // the configured static token: open_session takes it, nothing near it, and logout cannot revoke it
        engine_api_test::SerialEngine e(a, "testHooks=1;token=static-token-0123456789");
        const char* const nearMisses[] = {"static-token-012345678", "static-token-012345678x", "static-token-0123456789!", "Static-token-0123456789", ""};
        for (const char* miss : nearMisses) {
            testing::Session s(a, e.raw(), FOXAPI_SESSION_REMOTE, kAllGrants, miss);
            if (s.openStatus != FOXAPI_UNAUTHENTICATED) { std::printf("    a session opened with the near miss \"%s\" answered %d\n", miss, s.openStatus); }
            CHECK_EQ(s.openStatus, FOXAPI_UNAUTHENTICATED);
        }
        testing::Session good(a, e.raw(), FOXAPI_SESSION_REMOTE, kAllGrants, "static-token-0123456789");
        CHECK(good.ok());
        CHECK_EQ(a->logout(e.raw(), "static-token-0123456789"), FOXAPI_DENIED);   // the configured token is not a session token: it cannot be revoked
        CHECK_EQ(a->logout(e.raw(), "static-token-012345678"), FOXAPI_NOT_FOUND);
        CHECK_EQ(a->logout(e.raw(), "static-token-0123456789!"), FOXAPI_NOT_FOUND);
        testing::Session still(a, e.raw(), FOXAPI_SESSION_REMOTE, kAllGrants, "static-token-0123456789");
        CHECK(still.ok());
    }
}

// ---- B ----------------------------------------------------------------------------------------------------------------------------
std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());   // (a checkout may hold either line end)
    return s;
}

// The text of the function whose signature line contains `signature`: from that line to the first line that is exactly four spaces and a brace (the end of
// a member function of the table's implementation class).
std::string bodyOf(const std::string& src, const std::string& signature) {
    const std::size_t at = src.find(signature);
    if (at == std::string::npos) { return std::string(); }
    const std::size_t end = src.find("\n    }\n", at);
    return end == std::string::npos ? std::string() : src.substr(at, end - at);
}

std::size_t countOf(const std::string& hay, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + needle.size())) { ++n; }
    return n;
}

void testCompareSites() {
    std::printf("  B: the credential compares go through the helper\n");
    const std::string src = readFile(fs::path(CASCADE_SOURCE_DIR) / "src" / "engine" / "fox_engine_api.cpp");
    CHECK(!src.empty());
    struct Site {
        const char* signature;
        std::size_t minHelperCalls;
    };
    const Site sites[] = {{"int32_t login(const char* user, const char* password", 2},   // the user and the password
                          {"int32_t logout(const char* token)", 1},                        // the configured token cannot be revoked
                          {"bool tokenValid(const char* t)", 1}};                          // the configured token opens a session
    for (const Site& site : sites) {
        const std::string body = bodyOf(src, site.signature);
        if (body.empty()) { std::printf("    could not find %s\n", site.signature); }
        CHECK(!body.empty());
        const std::size_t calls = countOf(body, "constantTimeEqualsText(");
        if (calls < site.minHelperCalls) { std::printf("    %s calls the constant-time compare %zu times (wanted %zu)\n", site.signature, calls, site.minHelperCalls); }
        CHECK(calls >= site.minHelperCalls);
        for (const char* bad : {"opt_.password !=", "opt_.password ==", "opt_.user !=", "opt_.user ==", "opt_.token ==", "opt_.token !=", "== opt_.token", "!= opt_.token",
                                "!= password", "== password"}) {
            if (body.find(bad) != std::string::npos) { std::printf("    %s still compares with \"%s\"\n", site.signature, bad); }
            CHECK(body.find(bad) == std::string::npos);
        }
    }
}

// ---- C ----------------------------------------------------------------------------------------------------------------------------
void testHelper() {
    std::printf("  C: the helper\n");
    using cascade::net::constantTimeEqualsText;
    CHECK(constantTimeEqualsText("", ""));
    CHECK(constantTimeEqualsText("a", "a"));
    CHECK(constantTimeEqualsText("correct-horse-battery", "correct-horse-battery"));
    CHECK(!constantTimeEqualsText("correct-horse-battery", "xorrect-horse-battery"));    // first byte
    CHECK(!constantTimeEqualsText("correct-horse-battery", "correct-horse-batterx"));    // last byte
    CHECK(!constantTimeEqualsText("correct-horse-battery", "correct-horse-batter"));     // shorter
    CHECK(!constantTimeEqualsText("correct-horse-batter", "correct-horse-battery"));     // longer
    CHECK(!constantTimeEqualsText("", "x"));
    CHECK(!constantTimeEqualsText("x", ""));
    // a shorter string that is a prefix of the other, padded with what the compare pads with, must not be equal to it
    CHECK(!constantTimeEqualsText(std::string("ab"), std::string("ab\0", 3)));
    CHECK(!constantTimeEqualsText(std::string("ab\0", 3), std::string("ab")));
    // an embedded NUL does not end the compare
    CHECK(constantTimeEqualsText(std::string("a\0b", 3), std::string("a\0b", 3)));
    CHECK(!constantTimeEqualsText(std::string("a\0b", 3), std::string("a\0c", 3)));
    CHECK(!constantTimeEqualsText(std::string("a\0b", 3), std::string("a", 1)));
}

}  // namespace

int main() {
    engine_api_test::hermetic("secret_compare");
    std::printf("test_engine_api_secret_compare\n");
    testHelper();
    testCompareSites();
    testBehaviourIsUnchanged();
    return check::finish("test_engine_api_secret_compare");
}
