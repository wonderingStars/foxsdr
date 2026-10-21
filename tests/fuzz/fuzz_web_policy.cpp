// Fuzz target: the web remote's configuration strings - the bind address a user
// types, the stored password record - and the base64 they travel in.
//
// These are the text fields of config.json that decide whether the web server
// listens, on which interface, and whether it demands a password: the rule that
// refuses an off-machine bind without a usable password is only as good as its
// reading of what the user wrote. A password record is also a place a hand
// edit can set an absurd iteration count or a truncated salt.
//
// Properties: a record that parses is valid() and serialises back to a record
// that parses to the same bytes; base64 decodes what it encodes; an address the
// policy calls acceptable is not empty; and no configuration that reaches
// "Allowed" off the machine does so without authentication (rule 2).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "fuzz_common.hpp"
#include "net/web_auth.hpp"
#include "net/web_policy.hpp"

using namespace cascade::net;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    fuzz::Bytes in(data, size);
    const std::size_t cut = in.u8();
    const std::string a = in.str(cut);
    const std::string b = in.rest();

    // The password record.
    for (const std::string* s : {&a, &b}) {
        PasswordRecord rec;
        std::string error;
        if (PasswordRecord::parse(*s, rec, error)) {
            FUZZ_REQUIRE(rec.valid());
            PasswordRecord again;
            FUZZ_REQUIRE(PasswordRecord::parse(rec.serialize(), again, error));
            FUZZ_REQUIRE(again.iterations == rec.iterations);
            FUZZ_REQUIRE(again.salt == rec.salt);
            FUZZ_REQUIRE(again.hash == rec.hash);
        } else {
            FUZZ_REQUIRE(!error.empty());
        }

        std::vector<std::uint8_t> raw;
        if (base64Decode(*s, raw)) {
            std::vector<std::uint8_t> back;
            FUZZ_REQUIRE(base64Decode(base64Encode(raw), back));
            FUZZ_REQUIRE(back == raw);
        }
    }

    // The bind policy, over text the user typed.
    (void)isLoopbackAddress(a);
    (void)isWildcardAddress(a);
    const bool acceptable = isAcceptableBindAddress(a);
    if (acceptable) { FUZZ_REQUIRE(!normaliseBindAddress(a).empty()); }

    WebServerConfig cfg;
    cfg.enabled = true;
    cfg.bindAddress = a;
    cfg.port = static_cast<int>(in.u32());
    cfg.username = "admin";
    cfg.passwordRecord = b;
    const BindDecision d = evaluateBind(cfg);
    if (d.allowed() && d.reachableOffMachine) { FUZZ_REQUIRE(d.authRequired); }
    return 0;
}
