// web_guard.hpp - THE HOST AND ORIGIN CHECK of the built-in web server (security review 2, R1).
//
// WHY. A web page the owner visits can talk to this server in two ways that need no password: it can make the browser send a request to
// http://127.0.0.1:<port>/ from another site (a "simple" cross-site POST), and it can make its own DNS name re-resolve to 127.0.0.1 (DNS
// rebinding), after which the browser treats the server as that page's own origin. In both cases the request carries a `Host` or an `Origin`
// that is not one of ours. Checking the two on EVERY route closes both; it needs no sign-in and no state.
//
// THE RULES (each held by tests/test_web_guard.cpp):
//   HOST. The Host header must name this server. Allowed: an IPv4 literal, a bracketed IPv6 literal (a literal cannot be re-resolved by an
//   attacker), `localhost`, and the names the owner or the computer gave: `policy.hostNames` (the computer's own name and the hosts of the
//   owner's "Allowed web origins" list). Anything else, or two Host headers, is refused (421). A request with NO Host header (an HTTP/1.0 tool)
//   is left alone: a browser always sends one.
//   ORIGIN. No Origin header = a same-origin GET or a tool that is not a browser: allowed, as it always was. An Origin header, if present,
//   must be ONE value and parse as a serialised origin; it is allowed when it is the server's own origin (its host and port equal the Host
//   header's), or - for GET, HEAD and OPTIONS only - when the owner listed it (or "*") in "Allowed web origins". A POST, PUT or DELETE from any
//   other origin, `null` included, is refused (403).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <string>
#include <vector>

namespace cascade::net {

struct HostOriginPolicy {
    std::vector<std::string> machineNames;   // lower-case names of this computer (its host name, host name.local), set by the server
    std::vector<std::string> hostNames;      // lower-case DNS names, no port, from the owner's "Allowed web origins" list
    std::vector<std::string> origins;     // canonical origins (scheme://host[:port]) the owner listed
    bool anyOrigin = false;               // the owner listed "*"
};

struct GuardRequest {
    std::vector<std::string> hostValues;     // every Host header, in order
    std::vector<std::string> originValues;   // every Origin header, in order
    std::string method;                      // GET, POST ...
};

enum class GuardVerdict { Allowed, ForeignHost, ForeignOrigin };

// Splits a Host header value into a lower-case host (an IPv6 literal keeps its brackets) and its port text ("" when none). False when the value
// is malformed (empty, whitespace, a bad bracket, a non-numeric port).
bool splitHostHeader(const std::string& value, std::string& host, std::string& port);

// True for a dotted IPv4 literal or a bracketed IPv6 literal.
bool isIpLiteralHost(const std::string& host);

// The names a Host header may carry besides literals and `localhost`: this computer's own name and `name.local` (lower-case; "" gives none).
std::vector<std::string> guardMachineNames(const std::string& computerName);
// Every DNS name the web page's Host rule accepts: this computer's names and the hosts of the owner's "Allowed web origins" list (entries that do not parse, and IP literals, give none).
// The program interface (net/api_server.hpp) takes the same names, so that a request the page answers by one of them is not refused there.
std::vector<std::string> guardHostNames(const std::string& computerName, const std::vector<std::string>& listedOrigins);

GuardVerdict checkHostOrigin(const HostOriginPolicy& policy, const GuardRequest& req);

// Builds the policy's origin lists from the owner's list (as given to the CORS model). Entries that do not parse are skipped; the CORS model
// has already refused them.
void guardSetOrigins(HostOriginPolicy& policy, const std::vector<std::string>& listed);

}  // namespace cascade::net
