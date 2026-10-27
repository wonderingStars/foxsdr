// See diag_evidence.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/diag_evidence.hpp"

#include <cctype>
#include <cstddef>
#include <cstdint>

namespace cascade::core {

namespace {

// A signature as the reports carry it: EXACTLY sixteen hex digits (core::crashSignature, core::freezeSignature), upper-cased; anything else is not an id and is
// not carried. diag_history.cpp already reads a signature this way, but a prefix of one is accepted there; this line is pasted into public reports, so what it carries
// is decided by the code that writes it and not by every reader upstream.
std::string signatureId(const std::string& raw) {
    if (raw.size() != 16) { return std::string(); }
    std::string out;
    for (const char c : raw) {
        if (std::isxdigit(static_cast<unsigned char>(c)) == 0) { return std::string(); }
        out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

// One of the three kinds a report can be, or "unknown": the closed list, whatever a file said.
std::string kindWord(const std::string& raw) {
    if (raw == "crash" || raw == "hang" || raw == "stall") { return raw; }
    return "unknown";
}

}  // namespace

const std::vector<std::string>& evidenceBundleFieldNames() {
    static const std::vector<std::string> names = {"recent-faults", "recovered"};
    return names;
}

std::string recentFaultsText(const DiagHistory& h) {
    if (!h.included) { return "(not read - diagnostics are off)"; }
    if (h.pending) { return "(still being read)"; }
    if (!h.reports.readable) { return "(the reports folder could not be read)"; }
    if (h.reports.newest.empty()) { return "none"; }
    std::string out;
    for (const ReportSummary& r : h.reports.newest) {
        if (!out.empty()) { out += ", "; }
        const std::string id = signatureId(r.signature);
        out += id.empty() ? std::string("unsigned") : id;
        out += " ";
        out += kindWord(r.kind);
    }
    if (h.reports.total > h.reports.newest.size()) {
        out += ", +" + std::to_string(h.reports.total - h.reports.newest.size()) + (h.reports.totalCapped ? "+" : "") + " more";
    }
    return out;
}

std::string recoveredText(const health::Counts& all, const health::Counts& prior, bool armed, bool allowed) {
    if (!armed) { return "(not counted - usage reporting is off)"; }
    if (!allowed) { return "(not counted - diagnostics are off)"; }
    std::string out;
    for (std::size_t i = 0; i < health::kRecoveredCount; ++i) {
        const health::Recovered r = static_cast<health::Recovered>(i);
        const std::string token = health::tokenRecovered(r);
        const auto now = all.find(token);
        if (now == all.end()) { continue; }
        const auto before = prior.find(token);
        const std::uint32_t was = before == prior.end() ? 0u : before->second;
        if (now->second <= was) { continue; }
        if (!out.empty()) { out += ", "; }
        out += health::recoveredWord(r);
        out += " ";
        out += std::to_string(now->second - was);
    }
    return out.empty() ? std::string("none") : out;
}

std::string withEvidenceFields(std::string bundle, const std::string& recentFaults, const std::string& recovered) {
    const std::string lines = "recent-faults: " + (recentFaults.empty() ? std::string("none") : recentFaults) + "\n" +
                              "recovered: " + (recovered.empty() ? std::string("none") : recovered) + "\n";
    const std::size_t at = bundle.find("\n--- log ---\n");
    if (at == std::string::npos) {
        bundle += lines;
    } else {
        bundle.insert(at, lines);
    }
    return bundle;
}

}  // namespace cascade::core
