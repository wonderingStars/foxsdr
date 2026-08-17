// Bind policy for web server mode (P11): the decision about whether a given
// configuration is allowed to start listening, and what it must require of a
// caller once it does.
//
// WHY THIS IS A SEPARATE MODULE FROM THE SERVER. It is the single most
// security-critical decision in the feature — "may this configuration accept
// connections from other machines, and with what protection" — and it involves
// no sockets, no threads, and no I/O. Keeping it as a pure function over a
// config struct means every branch can be tested directly, including the ones
// that must REFUSE, which is the half that is hard to exercise through a live
// server and the half that matters.
//
// THE RULES, each covered by a test in tests/test_web_policy.cpp:
//
//  1. A NON-LOOPBACK BIND WITHOUT A USABLE PASSWORD IS REFUSED, and there is
//     no flag anywhere that turns this off. Not "warned about", not "allowed
//     with a banner" — evaluateBind returns a refusal and the server never
//     reaches its listen call. This is the rule the whole feature rests on: an
//     SDR receiver reachable from the LAN with no credential is a device
//     anyone on the network can retune and listen through.
//
//  2. A PASSWORD RECORD THAT DOES NOT PARSE REFUSES THE BIND — on every
//     address, loopback included. A hand-edited or truncated record must never
//     be read as "authentication is configured", but nor may it be quietly
//     downgraded to "no password": the user's intent was that this is
//     protected, and starting without that protection is a change they never
//     asked for and could not notice. The refusal names which case it was,
//     because "you have not set a password" and "your stored password is
//     unreadable" call for different actions.
//
//  3. LOOPBACK MAY RUN WITHOUT A PASSWORD, because reaching it already
//     requires code execution on this machine, and at that point the config
//     file, the audio device and the receiver itself are equally available. But
//     if a password IS set, it is enforced on loopback too — a configured
//     credential is never silently ignored.
//
//  4. THE BIND ADDRESS MUST BE A LITERAL, not a hostname. "localhost" is the
//     one exception and it is NORMALISED to 127.0.0.1 rather than resolved:
//     the hosts file can point "localhost" anywhere, and a policy that decides
//     "this is loopback" from a name whose meaning another file controls is
//     not a policy. Everything else must be a dotted-quad IPv4 or one of the
//     two IPv6 literals below, so what the policy classifies and what the
//     socket binds are the same string.
//
//  5. IPV4 OCTETS WITH LEADING ZEROS ARE REFUSED. "010.0.0.1" is octal to some
//     parsers and decimal to others, and an address that means different things
//     to the classifier and to the socket layer is exactly how a "loopback
//     only" setting ends up listening somewhere else.
//
//  6. THE PORT MUST BE IN [kMinPort, 65535]. The floor keeps this off the
//     well-known range: on Windows nothing stops a user process binding port
//     80 or 445, so there is no operating-system guard to rely on, and no
//     legitimate use of this feature needs one.
//
// WHAT THIS MODULE DOES NOT DECIDE. Whether a particular REQUEST is authorised
// (that is the session check, net/web_auth.hpp), and whether the socket can
// actually be bound (that is the operating system's answer, discovered at
// listen time and reported separately). A policy verdict of Allowed means "this
// configuration is permitted to try", never "this will succeed".
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <string>

namespace cascade::net {

// Floor for the listening port; see rule 6.
inline constexpr int kMinPort = 1024;
inline constexpr int kMaxPort = 65535;

// The default the application ships with: loopback, so a fresh install is
// reachable only from the machine it runs on and turning the feature on cannot
// by itself expose anything.
inline constexpr int kDefaultWebPort = 8073;

struct WebServerConfig {
    bool enabled = false;
    std::string bindAddress = "127.0.0.1";
    int port = kDefaultWebPort;
    std::string username = "admin";
    // Serialized PasswordRecord (see net/web_auth.hpp). Empty means no
    // password has been set. A plaintext password is never stored here or
    // anywhere else.
    std::string passwordRecord;
};

enum class BindVerdict {
    Allowed,
    RefusedDisabled,          // the feature is switched off
    RefusedInvalidAddress,    // not a literal we accept (rules 4 and 5)
    RefusedInvalidPort,       // outside [kMinPort, kMaxPort] (rule 6)
    RefusedEmptyUsername,     // a credential with no account name is unusable
    RefusedPasswordRequired,  // off-machine bind, no password set (rule 1)
    RefusedPasswordUnusable,  // off-machine bind, password record corrupt (rule 2)
};

struct BindDecision {
    BindVerdict verdict = BindVerdict::RefusedDisabled;

    // The literal address the socket should bind, after normalisation (rule 4).
    // Only meaningful when verdict == Allowed.
    std::string effectiveAddress;

    // True when a request must carry a valid session before it is served.
    // Always true for an off-machine bind; also true on loopback when a
    // password is configured (rule 3).
    bool authRequired = false;

    // True when the binding accepts connections from other machines. This is
    // what the UI's "remote access is live" indicator must show, and it is
    // deliberately a property of the DECISION rather than something the GUI
    // re-derives from the address string.
    bool reachableOffMachine = false;

    // Human-readable explanation, always set for a refusal and suitable for
    // showing in the settings panel verbatim. Empty when Allowed.
    std::string reason;

    bool allowed() const { return verdict == BindVerdict::Allowed; }
};

// True for 127.0.0.0/8, ::1, and the literal "localhost" (rule 4).
bool isLoopbackAddress(const std::string& addr);

// True for 0.0.0.0, ::, and the empty string — "every interface". These are
// valid binds, but they are the MOST exposed ones, so they are classified
// explicitly rather than falling out of "not loopback".
bool isWildcardAddress(const std::string& addr);

// Accepts a dotted-quad IPv4 (no leading zeros in any octet), "::1", "::",
// "0.0.0.0", "localhost", or the empty string. Anything else — a hostname, a
// URL, an address with a port glued on, a partial quad — is rejected.
bool isAcceptableBindAddress(const std::string& addr);

// Maps an accepted address to the literal that should actually be bound:
// "localhost" becomes "127.0.0.1" and the empty string becomes "0.0.0.0";
// everything else is returned unchanged. Returns an empty string for an
// address isAcceptableBindAddress rejects.
std::string normaliseBindAddress(const std::string& addr);

// Applies every rule above. Pure: no sockets, no clock, no filesystem.
BindDecision evaluateBind(const WebServerConfig& cfg);

}  // namespace cascade::net
