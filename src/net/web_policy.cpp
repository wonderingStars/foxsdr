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

// (parseIPv4, isLoopbackAddress and isWildcardAddress are core/server_ports.hpp's since stage 5b - the settings panels need them and the
// interface includes nothing of net/. Unchanged; this file uses them as it always did.)

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

    // --- HTTPS ---------------------------------------------------------------------------------------------------------------------
    if (cfg.tlsEnabled) {
#ifndef _WIN32
        d.verdict = BindVerdict::RefusedTlsUnavailable;
        d.reason = "HTTPS for the web page uses Windows' own TLS (Schannel) and is not built on this platform; put a TLS proxy in front of "
                   "the page instead (UNPROVEN on this platform)";
        return d;
#endif
        if (!cfg.tlsCertThumbprint.empty()) {
            bool hex = cfg.tlsCertThumbprint.size() == 40;
            for (char c : cfg.tlsCertThumbprint) {
                hex = hex && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'));
            }
            if (!hex) {
                d.verdict = BindVerdict::RefusedTlsCertificate;
                d.reason = "the certificate thumbprint must be the 40 hexadecimal digits Windows shows for it (no spaces); leave it empty to "
                           "use the self-signed certificate this program makes";
                return d;
            }
        }
    }

    // --- Listeners -----------------------------------------------------------------------------------------------------------------
    if (cfg.listenersEnabled) {
        if (!recordUsable) {
            d.verdict = BindVerdict::RefusedListenerPassword;
            d.reason = "listeners are signed in by the owner's say-so: set the owner password before allowing listeners";
            return d;
        }
        PasswordRecord lrec;
        std::string lerr;
        if (cfg.listenerPasswordRecord.empty() || !PasswordRecord::parse(cfg.listenerPasswordRecord, lrec, lerr)) {
            d.verdict = BindVerdict::RefusedListenerPassword;
            d.reason = cfg.listenerPasswordRecord.empty()
                           ? "set the listener password before allowing listeners"
                           : "the stored listener password cannot be read (" + lerr + "); set it again before allowing listeners";
            return d;
        }
        if (cfg.maxListeners < 1 || cfg.maxListeners > 32 || cfg.listenerIdleSeconds < 0 || cfg.listenerSessionSeconds < 0) {
            d.verdict = BindVerdict::RefusedListenerLimits;
            d.reason = "the listener limit must be 1 to 32, and the idle and session time limits cannot be negative";
            return d;
        }
    }

    d.verdict = BindVerdict::Allowed;
    d.effectiveAddress = effective;
    d.reachableOffMachine = !loopback;
    // Rule 3: always on for an off-machine bind, and honoured on loopback too
    // whenever a usable credential exists. Listeners always need the sign-in.
    d.authRequired = !loopback || recordUsable || cfg.listenersEnabled;
    return d;
}

}  // namespace cascade::net
