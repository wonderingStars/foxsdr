// Structural policy test for android/app/src/main/res/xml/network_security_config.xml:
// the platform's half of "this application sends nothing in clear and trusts
// nothing the person holding the device installed" - with one exception, in
// one place, that only a debuggable build gets.
//
// WHY A TEST AND NOT JUST THE FILE. Proving an Android upload really arrives
// at a real server needs a certificate for 10.0.2.2 (the address an emulator
// reaches the host on), which no public CA can issue, so the device has to be
// told to trust a locally made CA. Android offers exactly one honest way to do
// that - a <debug-overrides> block, which the platform consults ONLY for a
// package carrying android:debuggable - and exactly one dishonest way that
// looks identical in a diff: the same <trust-anchors> at the top level, or
// inside <base-config>, where it applies to the SHIPPED application too and
// quietly makes every install trust whatever CA anyone can talk a user into
// adding. One indent's difference between a test arrangement and a
// man-in-the-middle hole is the reason this is pinned by a test rather than by
// a comment.
//
// WHAT IS CHECKED, on the file with its XML comments removed (the comments
// discuss these very attributes, and a scan that read them would credit the
// file for text that configures nothing):
//
//   - the base configuration refuses cleartext, and never permits it;
//   - the one cleartext exception names loopback and only loopback;
//   - there is exactly one <debug-overrides> block;
//   - every `src="user"` trust anchor lies INSIDE it - the load-bearing one;
//   - that block also lists the system anchors, because a <trust-anchors>
//     block REPLACES the default set rather than adding to it, and a debuggable
//     build that had dropped the public CAs would fail against every real
//     endpoint for a reason nothing would point at;
//   - the manifest still points its android:networkSecurityConfig at this
//     file, so the whole thing cannot be left configuring nothing.
//
// Deliberately a textual scan rather than an XML parse: this codebase links no
// XML parser, the file is 40 lines of hand-written configuration, and the one
// property that matters (is this anchor inside the debug block?) is a span
// comparison either way.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "test_check.hpp"

namespace fs = std::filesystem;

namespace {

std::string readWholeFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) { return std::string(); }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Replaces every <!-- ... --> with an equal number of spaces, so offsets into
// the result still line up with the original file and an unterminated comment
// cannot swallow the rest of the document silently (it is reported instead).
std::string stripComments(const std::string& text, bool& ok) {
    ok = true;
    std::string out = text;
    std::size_t at = 0;
    while (true) {
        const std::size_t open = out.find("<!--", at);
        if (open == std::string::npos) { break; }
        const std::size_t close = out.find("-->", open + 4);
        if (close == std::string::npos) {
            ok = false;
            break;
        }
        for (std::size_t i = open; i < close + 3; ++i) { out[i] = ' '; }
        at = close + 3;
    }
    return out;
}

std::size_t countOf(const std::string& hay, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t at = hay.find(needle); at != std::string::npos;
         at = hay.find(needle, at + needle.size())) {
        ++n;
    }
    return n;
}

// Every offset at which `needle` occurs.
std::vector<std::size_t> offsetsOf(const std::string& hay, const std::string& needle) {
    std::vector<std::size_t> out;
    for (std::size_t at = hay.find(needle); at != std::string::npos;
         at = hay.find(needle, at + needle.size())) {
        out.push_back(at);
    }
    return out;
}

// The offset of the next <domain ...> element at or after `at`, skipping
// <domain-config>, which shares the first seven characters and whose own tag
// would otherwise be read as a domain element whose value is the whole first
// entry (which is exactly what the first draft of this test did).
std::size_t findDomainTag(const std::string& text, std::size_t at) {
    while (true) {
        const std::size_t open = text.find("<domain", at);
        if (open == std::string::npos) { return std::string::npos; }
        const char next = (open + 7 < text.size()) ? text[open + 7] : '\0';
        if (next != '-') { return open; }
        at = open + 7;
    }
}

// The text of every <domain ...>VALUE</domain> inside the region [from, to).
std::vector<std::string> domainsIn(const std::string& text, std::size_t from, std::size_t to) {
    std::vector<std::string> out;
    std::size_t at = from;
    while (at < to) {
        const std::size_t open = findDomainTag(text, at);
        if (open == std::string::npos || open >= to) { break; }
        const std::size_t gt = text.find('>', open);
        if (gt == std::string::npos || gt >= to) { break; }
        const std::size_t end = text.find("</domain>", gt);
        if (end == std::string::npos || end > to) { break; }
        std::string value = text.substr(gt + 1, end - gt - 1);
        const std::size_t b = value.find_first_not_of(" \t\r\n");
        const std::size_t e = value.find_last_not_of(" \t\r\n");
        out.push_back(b == std::string::npos ? std::string() : value.substr(b, e - b + 1));
        at = end + 9;
    }
    return out;
}

bool isLoopbackName(const std::string& s) {
    return s == "127.0.0.1" || s == "::1" || s == "localhost";
}

}  // namespace

int main() {
    const fs::path root = fs::path(CASCADE_SOURCE_DIR);
    const fs::path cfgPath =
        root / "android" / "app" / "src" / "main" / "res" / "xml" / "network_security_config.xml";
    const fs::path manifestPath =
        root / "android" / "app" / "src" / "main" / "AndroidManifest.xml";

    const std::string raw = readWholeFile(cfgPath);
    CHECK(!raw.empty());
    if (raw.empty()) {
        std::printf("could not read %s\n", cfgPath.string().c_str());
        return testSummary("test_android_network_config");
    }

    bool commentsWellFormed = false;
    const std::string text = stripComments(raw, commentsWellFormed);
    CHECK(commentsWellFormed);

    // --- cleartext: refused by default, permitted for loopback only --------
    CHECK(text.find("<base-config cleartextTrafficPermitted=\"false\"") != std::string::npos);
    CHECK(countOf(text, "cleartextTrafficPermitted=\"true\"") == 1);

    const std::size_t dcOpen = text.find("<domain-config cleartextTrafficPermitted=\"true\"");
    CHECK(dcOpen != std::string::npos);
    const std::size_t dcClose = text.find("</domain-config>", dcOpen);
    CHECK(dcClose != std::string::npos);
    const std::vector<std::string> domains =
        (dcOpen == std::string::npos || dcClose == std::string::npos)
            ? std::vector<std::string>()
            : domainsIn(text, dcOpen, dcClose);
    CHECK(domains.size() == 3);
    for (const std::string& d : domains) {
        if (!isLoopbackName(d)) {
            std::printf("cleartext permitted to a NON-loopback destination: %s\n", d.c_str());
        }
        CHECK(isLoopbackName(d));
    }
    // Every <domain> in the file is inside that one block: a domain element
    // anywhere else would be a second configuration this scan says nothing
    // about.
    std::size_t domainTags = 0;
    for (std::size_t at = findDomainTag(text, 0); at != std::string::npos;
         at = findDomainTag(text, at + 7)) {
        ++domainTags;
    }
    CHECK(domainTags == domains.size());

    // --- the debug block, and the trust anchors that may only live in it ---
    CHECK(countOf(text, "<debug-overrides>") == 1);
    CHECK(countOf(text, "</debug-overrides>") == 1);
    const std::size_t dbgOpen = text.find("<debug-overrides>");
    const std::size_t dbgClose = text.find("</debug-overrides>");
    CHECK(dbgOpen != std::string::npos && dbgClose != std::string::npos && dbgOpen < dbgClose);

    const std::vector<std::size_t> userAnchors = offsetsOf(text, "src=\"user\"");
    // Non-vacuous on purpose: this arrangement is what makes an end-to-end
    // upload test against a real server possible at all (see the file's own
    // comment and tools/android_tls_front.py), so its ABSENCE is a change
    // someone should have to come here and make deliberately.
    CHECK(userAnchors.size() >= 1);
    for (const std::size_t at : userAnchors) {
        const bool inside =
            dbgOpen != std::string::npos && dbgClose != std::string::npos &&
            at > dbgOpen && at < dbgClose;
        if (!inside) {
            std::printf("a user trust anchor at offset %zu is OUTSIDE <debug-overrides> - "
                        "a shipped build would trust user-installed CAs\n",
                        at);
        }
        CHECK(inside);
    }

    // The system anchors have to be in the same block, or a debuggable build
    // verifies nothing public.
    const std::vector<std::size_t> systemAnchors = offsetsOf(text, "src=\"system\"");
    CHECK(systemAnchors.size() >= 1);
    for (const std::size_t at : systemAnchors) {
        CHECK(dbgOpen != std::string::npos && dbgClose != std::string::npos && at > dbgOpen &&
              at < dbgClose);
    }

    // --- and the manifest still uses this file ----------------------------
    const std::string manifestRaw = readWholeFile(manifestPath);
    CHECK(!manifestRaw.empty());
    bool manifestComments = false;
    const std::string manifest = stripComments(manifestRaw, manifestComments);
    CHECK(manifestComments);
    CHECK(manifest.find("android:networkSecurityConfig=\"@xml/network_security_config\"") !=
          std::string::npos);

    std::printf("test_android_network_config: %zu cleartext domain(s), %zu user anchor(s), "
                "%zu system anchor(s)\n",
                domains.size(), userAnchors.size(), systemAnchors.size());
    return testSummary("test_android_network_config");
}
