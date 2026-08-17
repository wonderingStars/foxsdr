// Tests for the web server bind policy (net/web_policy.hpp).
//
// The interesting half of this module is the REFUSALS, so most of what follows
// asserts that a configuration is rejected and for the right reason. A policy
// that returns Allowed too readily fails open, and failing open here means an
// SDR receiver anyone on the network can retune and listen through.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <string>

#include "net/web_auth.hpp"
#include "net/web_policy.hpp"
#include "test_check.hpp"

using namespace cascade::net;

namespace {

// A real, parseable password record. Built through hashPassword rather than
// hand-assembled so the policy is tested against the same shape the
// application will actually store.
std::string realRecord() {
    PasswordRecord rec;
    std::string error;
    if (!hashPassword("a-good-long-password", rec, error)) {
        return std::string();
    }
    return rec.serialize();
}

WebServerConfig baseConfig() {
    WebServerConfig cfg;
    cfg.enabled = true;
    cfg.bindAddress = "127.0.0.1";
    cfg.port = kDefaultWebPort;
    cfg.username = "admin";
    return cfg;
}

void testLoopbackClassification() {
    CHECK(isLoopbackAddress("127.0.0.1"));
    CHECK(isLoopbackAddress("127.0.0.2"));      // the whole /8, not just .1
    CHECK(isLoopbackAddress("127.255.255.254"));
    CHECK(isLoopbackAddress("::1"));
    CHECK(isLoopbackAddress("localhost"));

    CHECK(!isLoopbackAddress("0.0.0.0"));
    CHECK(!isLoopbackAddress("::"));
    CHECK(!isLoopbackAddress(""));
    CHECK(!isLoopbackAddress("192.168.1.20"));
    CHECK(!isLoopbackAddress("128.0.0.1"));
    // 126/8 and 128/8 bracket the loopback block; an off-by-one in the octet
    // test would let one of these through.
    CHECK(!isLoopbackAddress("126.0.0.1"));
    // Not loopback because it is not a valid address at all.
    CHECK(!isLoopbackAddress("127.0.0"));
    CHECK(!isLoopbackAddress("localhost.evil.example"));
}

void testWildcardClassification() {
    CHECK(isWildcardAddress(""));
    CHECK(isWildcardAddress("0.0.0.0"));
    CHECK(isWildcardAddress("::"));
    CHECK(!isWildcardAddress("127.0.0.1"));
    CHECK(!isWildcardAddress("192.168.1.20"));
}

void testAddressAcceptance() {
    CHECK(isAcceptableBindAddress("127.0.0.1"));
    CHECK(isAcceptableBindAddress("192.168.1.20"));
    CHECK(isAcceptableBindAddress("0.0.0.0"));
    CHECK(isAcceptableBindAddress("255.255.255.255"));
    CHECK(isAcceptableBindAddress("0.0.0.1"));
    CHECK(isAcceptableBindAddress("::1"));
    CHECK(isAcceptableBindAddress("::"));
    CHECK(isAcceptableBindAddress("localhost"));
    CHECK(isAcceptableBindAddress(""));

    // Hostnames are refused outright: a name resolves wherever something else
    // decides, so classifying it as loopback or not is not a decision this
    // policy can make.
    CHECK(!isAcceptableBindAddress("example.com"));
    CHECK(!isAcceptableBindAddress("nas"));
    CHECK(!isAcceptableBindAddress("http://127.0.0.1"));
    // Malformed quads.
    CHECK(!isAcceptableBindAddress("192.168.1"));
    CHECK(!isAcceptableBindAddress("192.168.1.1.1"));
    CHECK(!isAcceptableBindAddress("192.168.1."));
    CHECK(!isAcceptableBindAddress(".168.1.1"));
    CHECK(!isAcceptableBindAddress("192..1.1"));
    CHECK(!isAcceptableBindAddress("1.2.3.256"));
    CHECK(!isAcceptableBindAddress("1.2.3.999"));
    CHECK(!isAcceptableBindAddress("1.2.3.-1"));
    CHECK(!isAcceptableBindAddress("1.2.3.a"));
    // An address with a port glued on is a different thing entirely.
    CHECK(!isAcceptableBindAddress("127.0.0.1:8073"));
    // Surrounding whitespace is not silently trimmed — the string that is
    // classified must be the string that is bound.
    CHECK(!isAcceptableBindAddress(" 127.0.0.1"));
    CHECK(!isAcceptableBindAddress("127.0.0.1 "));
    // Leading zeros: octal to some parsers, decimal to others (rule 5).
    CHECK(!isAcceptableBindAddress("010.0.0.1"));
    CHECK(!isAcceptableBindAddress("127.0.0.01"));
}

void testNormalisation() {
    CHECK(normaliseBindAddress("localhost") == "127.0.0.1");
    CHECK(normaliseBindAddress("") == "0.0.0.0");
    CHECK(normaliseBindAddress("127.0.0.1") == "127.0.0.1");
    CHECK(normaliseBindAddress("192.168.1.20") == "192.168.1.20");
    CHECK(normaliseBindAddress("::1") == "::1");
    CHECK(normaliseBindAddress("example.com").empty());
    // Normalising "localhost" must not change what the policy thinks of it.
    CHECK(isLoopbackAddress(normaliseBindAddress("localhost")));
}

void testDisabledIsRefused() {
    WebServerConfig cfg = baseConfig();
    cfg.enabled = false;
    const BindDecision d = evaluateBind(cfg);
    CHECK(d.verdict == BindVerdict::RefusedDisabled);
    CHECK(!d.allowed());
    CHECK(!d.reason.empty());
    CHECK(!d.reachableOffMachine);
}

void testLoopbackWithoutPasswordIsAllowed() {
    WebServerConfig cfg = baseConfig();
    const BindDecision d = evaluateBind(cfg);
    CHECK(d.allowed());
    CHECK(d.effectiveAddress == "127.0.0.1");
    CHECK(!d.reachableOffMachine);
    CHECK(!d.authRequired);   // rule 3
    CHECK(d.reason.empty());
}

void testLoopbackWithPasswordStillRequiresAuth() {
    WebServerConfig cfg = baseConfig();
    cfg.passwordRecord = realRecord();
    CHECK(!cfg.passwordRecord.empty());
    const BindDecision d = evaluateBind(cfg);
    CHECK(d.allowed());
    CHECK(!d.reachableOffMachine);
    // A configured credential is never silently ignored, even on loopback.
    CHECK(d.authRequired);
}

void testLocalhostNormalisesAndStaysLocal() {
    WebServerConfig cfg = baseConfig();
    cfg.bindAddress = "localhost";
    const BindDecision d = evaluateBind(cfg);
    CHECK(d.allowed());
    CHECK(d.effectiveAddress == "127.0.0.1");
    CHECK(!d.reachableOffMachine);
}

void testOffMachineWithoutPasswordIsRefused() {
    // Every off-machine form must be refused, not just the wildcard.
    const char* kAddresses[] = {"0.0.0.0", "::", "192.168.1.20", "10.0.0.5"};
    for (const char* addr : kAddresses) {
        WebServerConfig cfg = baseConfig();
        cfg.bindAddress = addr;
        const BindDecision d = evaluateBind(cfg);
        CHECK(d.verdict == BindVerdict::RefusedPasswordRequired);
        CHECK(!d.allowed());
        CHECK(!d.reason.empty());
    }
    // The empty string means "every interface" and must be refused too — it is
    // the easiest value to arrive at by accident.
    WebServerConfig cfg = baseConfig();
    cfg.bindAddress = "";
    CHECK(evaluateBind(cfg).verdict == BindVerdict::RefusedPasswordRequired);
}

void testOffMachineWithPasswordIsAllowed() {
    WebServerConfig cfg = baseConfig();
    cfg.bindAddress = "0.0.0.0";
    cfg.passwordRecord = realRecord();
    const BindDecision d = evaluateBind(cfg);
    CHECK(d.allowed());
    CHECK(d.effectiveAddress == "0.0.0.0");
    CHECK(d.reachableOffMachine);
    CHECK(d.authRequired);    // rule 1: never optional off-machine
    CHECK(d.reason.empty());
}

void testCorruptPasswordIsRefusedEverywhere() {
    const std::string good = realRecord();
    // Truncated, wrong label, and a plausible-looking hand edit.
    const std::string kCorrupt[] = {
        "pbkdf2-sha256$",
        "not-a-record",
        "pbkdf2-sha256$10000$short$alsoshort",
        good.substr(0, good.size() - 4),
        "bcrypt$10000$AAECAwQFBgcICQoLDA0ODw==$AAECAwQFBgcICQoLDA0ODw==",
    };
    for (const std::string& bad : kCorrupt) {
        // Off-machine.
        WebServerConfig remote = baseConfig();
        remote.bindAddress = "0.0.0.0";
        remote.passwordRecord = bad;
        CHECK(evaluateBind(remote).verdict == BindVerdict::RefusedPasswordUnusable);

        // ...and loopback, where the danger is a SILENT downgrade to no
        // password rather than exposure.
        WebServerConfig local = baseConfig();
        local.passwordRecord = bad;
        const BindDecision d = evaluateBind(local);
        CHECK(d.verdict == BindVerdict::RefusedPasswordUnusable);
        CHECK(!d.allowed());
        CHECK(!d.authRequired);
    }
}

void testEmptyUsernameWithPasswordIsRefused() {
    WebServerConfig cfg = baseConfig();
    cfg.passwordRecord = realRecord();
    cfg.username = "";
    CHECK(evaluateBind(cfg).verdict == BindVerdict::RefusedEmptyUsername);

    // With no password set at all, the user name is not yet meaningful, so a
    // loopback bind is still fine.
    WebServerConfig noPassword = baseConfig();
    noPassword.username = "";
    CHECK(evaluateBind(noPassword).allowed());
}

void testPortRange() {
    const int kBadPorts[] = {0, -1, 1, 80, 443, 1023, 65536, 70000};
    for (int p : kBadPorts) {
        WebServerConfig cfg = baseConfig();
        cfg.port = p;
        const BindDecision d = evaluateBind(cfg);
        CHECK(d.verdict == BindVerdict::RefusedInvalidPort);
        CHECK(!d.allowed());
    }
    const int kGoodPorts[] = {kMinPort, 8073, 8080, kMaxPort};
    for (int p : kGoodPorts) {
        WebServerConfig cfg = baseConfig();
        cfg.port = p;
        CHECK(evaluateBind(cfg).allowed());
    }
}

void testInvalidAddressIsRefused() {
    const char* kBad[] = {"example.com", "192.168.1", "1.2.3.256", "010.0.0.1",
                          "127.0.0.1:8073", " 127.0.0.1"};
    for (const char* addr : kBad) {
        WebServerConfig cfg = baseConfig();
        cfg.bindAddress = addr;
        // A password is set, so a refusal here can only be about the address.
        cfg.passwordRecord = realRecord();
        const BindDecision d = evaluateBind(cfg);
        CHECK(d.verdict == BindVerdict::RefusedInvalidAddress);
        CHECK(!d.allowed());
        CHECK(d.effectiveAddress.empty());
    }
}

// The ordering matters for what the user is told first: being switched off
// outranks everything, and an unusable address outranks a missing password,
// because there is no point demanding a credential for a binding that could
// never happen.
void testRefusalPrecedence() {
    WebServerConfig cfg;
    cfg.enabled = false;
    cfg.bindAddress = "nonsense";
    cfg.port = 1;
    CHECK(evaluateBind(cfg).verdict == BindVerdict::RefusedDisabled);

    cfg.enabled = true;
    CHECK(evaluateBind(cfg).verdict == BindVerdict::RefusedInvalidAddress);

    cfg.bindAddress = "0.0.0.0";
    CHECK(evaluateBind(cfg).verdict == BindVerdict::RefusedInvalidPort);

    cfg.port = kDefaultWebPort;
    CHECK(evaluateBind(cfg).verdict == BindVerdict::RefusedPasswordRequired);
}

}  // namespace

int main() {
    testLoopbackClassification();
    testWildcardClassification();
    testAddressAcceptance();
    testNormalisation();
    testDisabledIsRefused();
    testLoopbackWithoutPasswordIsAllowed();
    testLoopbackWithPasswordStillRequiresAuth();
    testLocalhostNormalisesAndStaysLocal();
    testOffMachineWithoutPasswordIsRefused();
    testOffMachineWithPasswordIsAllowed();
    testCorruptPasswordIsRefusedEverywhere();
    testEmptyUsernameWithPasswordIsRefused();
    testPortRange();
    testInvalidAddressIsRefused();
    testRefusalPrecedence();
    return testSummary("test_web_policy");
}
