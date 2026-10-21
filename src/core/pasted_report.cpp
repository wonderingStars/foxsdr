// See pasted_report.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/pasted_report.hpp"

#include <cctype>
#include <cstring>

#include "core/sentinel.hpp"

namespace cascade::core {

namespace {

std::string trimmed(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])) != 0) { ++b; }
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])) != 0) { --e; }
    return s.substr(b, e - b);
}

// A person's paste can carry CRLF (a Windows editor), a lone CR (an old buffer) or
// LF. The report that travels is LF, so the site holds one format whichever way it
// was copied.
std::string normaliseLineEndings(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\r') {
            out += '\n';
            if (i + 1 < s.size() && s[i + 1] == '\n') { ++i; }
        } else {
            out += s[i];
        }
    }
    return out;
}

std::vector<std::string> splitLf(const std::string& s) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (at < s.size()) {
        const std::size_t nl = s.find('\n', at);
        if (nl == std::string::npos) {
            out.push_back(s.substr(at));
            break;
        }
        out.push_back(s.substr(at, nl - at));
        at = nl + 1;
    }
    return out;
}

bool startsWith(const std::string& s, const char* prefix) {
    return s.compare(0, std::strlen(prefix), prefix) == 0;
}

// `name: value` -> value, when the line's name (left of the first colon, trimmed) is
// `name`.
bool fieldValue(const std::string& line, const char* name, std::string& value) {
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos) { return false; }
    if (trimmed(line.substr(0, colon)) != name) { return false; }
    value = trimmed(line.substr(colon + 1));
    return true;
}

// Only the characters a version is made of, and all of the value: a field that
// carries anything else is not a version, and a report whose version is not one is
// not recognised on the strength of it.
bool versionOk(const std::string& v) {
    if (v.empty() || v.size() > 48) { return false; }
    for (const char c : v) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '-' ||
                        c == '+' || c == '_';
        if (!ok) { return false; }
    }
    return true;
}

// The signature is sixteen hex digits, written by both writers and by the sentinel.
bool signatureOk(const std::string& v) {
    if (v.size() != 16) { return false; }
    for (const char c : v) {
        if (std::isxdigit(static_cast<unsigned char>(c)) == 0) { return false; }
    }
    return true;
}

// A header line as the writers make one: a name of lower-case letters, digits and
// hyphens ("kind", "address-source", "stalled-ms"), a colon, and a value.
bool headerLine(const std::string& line) {
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos) { return false; }
    const std::string name = trimmed(line.substr(0, colon));
    if (name.empty()) { return false; }
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) { return false; }
    }
    return true;
}

bool kindOk(const std::string& v) { return v == "crash" || v == "hang" || v == "stall"; }

constexpr SentinelClass kSentinelClasses[] = {SentinelClass::Crash, SentinelClass::Frozen,
                                              SentinelClass::Startup, SentinelClass::Outside,
                                              SentinelClass::Session};

struct Candidate {
    std::string kind;
    std::string reason;
    std::string version;
};

// Is the report that starts at lines[first] (its `kind:` line) a whole one? The
// header, up to the first `--- ` marker, has the signature; the marker is the
// context block's; the block has the version.
bool readCandidate(const std::vector<std::string>& lines, std::size_t first, Candidate& out) {
    out = Candidate();
    std::string v;
    if (!fieldValue(lines[first], "kind", v) || !kindOk(v)) { return false; }
    out.kind = v;

    bool haveSignature = false;
    std::size_t i = first + 1;
    for (; i < lines.size(); ++i) {
        if (startsWith(lines[i], "--- ")) { break; }
        // Every header line is `name: value` with a plain name, one `kind:` only:
        // a blank line, a sentence or a second kind line means this was somebody
        // quoting a header and carrying on, and the report - if there is one - is
        // further down.
        if (!headerLine(lines[i]) || fieldValue(lines[i], "kind", v)) { return false; }
        if (fieldValue(lines[i], "signature", v)) {
            haveSignature = signatureOk(v);
        } else if (out.reason.empty() && fieldValue(lines[i], "reason", v)) {
            out.reason = v;
        }
    }
    if (!haveSignature || i >= lines.size() || !startsWith(lines[i], "--- context ---")) { return false; }

    for (++i; i < lines.size(); ++i) {
        if (startsWith(lines[i], "--- ")) { break; }
        if (fieldValue(lines[i], "version", v)) {
            if (!versionOk(v)) { return false; }
            out.version = v;
            break;
        }
    }
    return !out.version.empty();
}

std::string classOf(const Candidate& c) {
    if (c.kind != "crash") { return c.kind; }
    for (const SentinelClass sc : kSentinelClasses) {
        if (startsWith(c.reason, sentinelClassReason(sc))) {
            return std::string("sentinel:") + sentinelClassId(sc);
        }
    }
    return "crash";
}

constexpr const char* kCutNote = "(cut here: the rest of the report is in the message)\n";

}  // namespace

std::optional<PastedReport> detectPastedReport(const std::string& pasted) {
    if (pasted.empty()) { return std::nullopt; }
    const std::string text = normaliseLineEndings(pasted);
    const std::vector<std::string> lines = splitLf(text);

    // Byte offset of each line, to slice the text from the report's own first line.
    std::vector<std::size_t> offset(lines.size());
    {
        std::size_t at = 0;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            offset[i] = at;
            at += lines[i].size() + 1;
        }
    }

    for (std::size_t i = 0; i < lines.size(); ++i) {
        Candidate c;
        if (!readCandidate(lines, i, c)) { continue; }
        PastedReport out;
        out.reportClass = classOf(c);
        out.version = c.version;
        out.text = text.substr(offset[i]);
        if (out.text.size() > kPastedReportMaxBytes) {
            // Cut at a line, and say so: room for the note is taken from the cap, and
            // the cut falls on a newline so no character is split.
            const std::size_t room = kPastedReportMaxBytes - std::strlen(kCutNote);
            std::size_t cut = out.text.rfind('\n', room - 1);
            cut = (cut == std::string::npos) ? 0 : cut + 1;
            out.text.resize(cut);
            out.text += kCutNote;
        }
        return out;
    }
    return std::nullopt;
}

const std::vector<std::string>& pastedReportClassIds() {
    static const std::vector<std::string> ids = [] {
        std::vector<std::string> v = {"crash", "hang", "stall"};
        for (const SentinelClass sc : kSentinelClasses) {
            v.push_back(std::string("sentinel:") + sentinelClassId(sc));
        }
        return v;
    }();
    return ids;
}

}  // namespace cascade::core
