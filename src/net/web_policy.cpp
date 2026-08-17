// Implementation of the web server bind policy. See net/web_policy.hpp for the
// six rules this file exists to enforce.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/web_policy.hpp"

#include <cstdlib>
#include <string>
#include <vector>

#include "net/web_auth.hpp"

namespace cascade::net {
namespace {

// Splits on '.' WITHOUT collapsing empty fields, so "1..2.3" and "1.2.3." are
// seen as the malformed things they are rather than silently repaired.
std::vector<std::string> splitDots(const std::string& s) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : s) {
        if (c == '.') {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    parts.push_back(cur);
    return parts;
}

// One IPv4 octet: 1-3 decimal digits, value <= 255, and no leading zero on a
// multi-digit field (rule 5 — "010" is octal to some parsers). Returns -1 if
// the text is not such an octet.
int parseOctet(const std::string& text) {
    if (text.empty() || text.size() > 3) {
        return -1;
    }
    for (char c : text) {
        if (c < '0' || c > '9') {
            return -1;
        }
    }
    if (text.size() > 1 && text[0] == '0') {
        return -1;
    }
    const int value = std::atoi(text.c_str());
    return (value >= 0 && value <= 255) ? value : -1;
}

// True iff `addr` is a well-formed dotted quad. `firstOctet` receives octet 0
// so the caller can test for 127.0.0.0/8 without parsing twice.
bool parseIPv4(const std::string& addr, int& firstOctet) {
    const std::vector<std::string> parts = splitDots(addr);
    if (parts.size() != 4) {
        return false;
    }
    int octets[4];
    for (std::size_t i = 0; i < 4; ++i) {
        octets[i] = parseOctet(parts[i]);
        if (octets[i] < 0) {
            return false;
        }
    }
    firstOctet = octets[0];
    return true;
}

}  // namespace

bool isLoopbackAddress(const std::string& addr) {
    if (addr == "localhost" || addr == "::1") {
        return true;
    }
    int first = 0;
    if (parseIPv4(addr, first)) {
        // The whole 127.0.0.0/8 block, not just 127.0.0.1 — 127.0.0.2 is every
        // bit as local, and treating it as remote would demand a password for
        // a binding that no other machine can reach.
        return first == 127;
    }
    return false;
}

bool isWildcardAddress(const std::string& addr) {
    return addr.empty() || addr == "0.0.0.0" || addr == "::";
}

bool isAcceptableBindAddress(const std::string& addr) {
    if (isWildcardAddress(addr) || addr == "localhost" || addr == "::1") {
        return true;
    }
    int ignored = 0;
    return parseIPv4(addr, ignored);
}

std::string normaliseBindAddress(const std::string& addr) {
    if (!isAcceptableBindAddress(addr)) {
        return std::string();
    }
    if (addr == "localhost") {
        // Normalised, never resolved — see rule 4. What the policy classified
        // and what the socket binds must be the same string.
        return "127.0.0.1";
    }
    if (addr.empty()) {
        return "0.0.0.0";
    }
    return addr;
}

BindDecision evaluateBind(const WebServerConfig& cfg) {
    BindDecision d;

    if (!cfg.enabled) {
        d.verdict = BindVerdict::RefusedDisabled;
        d.reason = "web access is switched off";
        return d;
    }

    if (!isAcceptableBindAddress(cfg.bindAddress)) {
        d.verdict = BindVerdict::RefusedInvalidAddress;
        d.reason = "\"" + cfg.bindAddress +
                   "\" is not an address this can bind: use 127.0.0.1 for this "
                   "machine only, 0.0.0.0 for every network interface, or the "
                   "literal address of one interface";
        return d;
    }

    if (cfg.port < kMinPort || cfg.port > kMaxPort) {
        d.verdict = BindVerdict::RefusedInvalidPort;
        d.reason = "port " + std::to_string(cfg.port) + " is outside the allowed range " +
                   std::to_string(kMinPort) + "-" + std::to_string(kMaxPort);
        return d;
    }

    const std::string effective = normaliseBindAddress(cfg.bindAddress);
    const bool loopback = isLoopbackAddress(cfg.bindAddress);

    // Parse the record BEFORE branching on exposure, so "corrupt" and "absent"
    // stay distinguishable in both branches (rule 2).
    PasswordRecord record;
    std::string parseError;
    const bool hasRecordText = !cfg.passwordRecord.empty();
    const bool recordUsable =
        hasRecordText && PasswordRecord::parse(cfg.passwordRecord, record, parseError);

    if (hasRecordText && cfg.username.empty()) {
        d.verdict = BindVerdict::RefusedEmptyUsername;
        d.reason = "a password is set but the user name is empty";
        return d;
    }

    // An unreadable record is refused on EVERY binding, loopback included. The
    // user's intent was "this is protected"; starting anyway with the
    // protection quietly dropped is a downgrade they never asked for and would
    // have no way to notice. Refusing is loud, and the fix is one dialog away.
    if (hasRecordText && !recordUsable) {
        d.verdict = BindVerdict::RefusedPasswordUnusable;
        d.reason = "the stored password cannot be read (" + parseError +
                   "); set it again before turning web access on";
        return d;
    }

    if (!loopback && !hasRecordText) {
        d.verdict = BindVerdict::RefusedPasswordRequired;
        d.reason = "set a password before allowing access from other machines — "
                   "binding " + effective +
                   " would let anyone on the network tune and listen through this "
                   "receiver";
        return d;
    }

    d.verdict = BindVerdict::Allowed;
    d.effectiveAddress = effective;
    d.reachableOffMachine = !loopback;
    // Rule 3: always on for an off-machine bind, and honoured on loopback too
    // whenever a usable credential exists.
    d.authRequired = !loopback || recordUsable;
    return d;
}

}  // namespace cascade::net
