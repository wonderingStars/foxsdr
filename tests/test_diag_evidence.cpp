// The bundle's two evidence lines (stub campaign B14, owner-approved 2026-10-27): core/diag_evidence.hpp.
//
// WHAT IS PINNED HERE
//   - recent-faults: the signature id and kind of each report on the machine, newest first as listed, "+N more" past the listing, "none" for an empty folder, and the
//     three honest states when the reports cannot be read (diagnostics off, still being read, the folder unreadable) - never "none" for those;
//   - ONLY IDS AND KINDS: a signature is exactly sixteen hex digits or the report is "unsigned" (a path, a prefix and a word are all refused), a kind is one of the
//     three or "unknown", and nothing else of a report - its reason, its code, its age, its version - reaches the line;
//   - recovered: this session's recoveries as the vocabulary's fixed words and counts in the vocabulary's order, the ledger's earlier-session counts taken off, tokens
//     that are not recoveries ignored; "none" when the ledger is counting and nothing was recovered; "(not counted ...)" - never "none" - when usage reporting is off
//     or the Diagnostics switch is, because not counting is not the same as nothing recovered;
//   - against the REAL ledger: a recovery noted into an armed HealthLedger comes out as its word, and an unarmed one says it did not count;
//   - the field names, and where the lines go: before the log, after what is already in the header, in order, "none" for an empty value, appended when there is no log.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <string>
#include <vector>

#include "core/diag_evidence.hpp"
#include "core/health_events.hpp"
#include "core/telemetry.hpp"
#include "test_check.hpp"

using namespace cascade::core;

namespace {

ReportSummary report(const char* kind, const char* signature) {
    ReportSummary r;
    r.kind = kind;
    r.signature = signature;
    r.ageSec = 90;
    r.version = "0.99.90";
    r.uptimeSec = 600;
    r.reason = "C:\\Users\\steve\\secret.dll could not be loaded";
    r.code = "0xC0000005";
    r.upload = "sent";
    return r;
}

DiagHistory readable(std::vector<ReportSummary> newest, std::size_t total, bool capped = false) {
    DiagHistory h;
    h.included = true;
    h.reports.readable = true;
    h.reports.newest = std::move(newest);
    h.reports.total = total;
    h.reports.totalCapped = capped;
    return h;
}

bool onlyIdsAndKinds(const std::string& line) {
    // hex digits, the three kind words, "unknown", "unsigned", spaces, commas, a plus, digits and "more": no slash, colon, dot, quote or bracket.
    for (const char c : line) {
        if (c == '\\' || c == '/' || c == ':' || c == '.' || c == '"' || c == '(' || c == '<' || c == '\n') { return false; }
    }
    return true;
}

}  // namespace

int main() {
    std::printf("test_diag_evidence\n");

    // --- the names ------------------------------------------------------------------------------------------------------------------------------------
    CHECK(evidenceBundleFieldNames().size() == 2);
    CHECK(evidenceBundleFieldNames()[0] == "recent-faults");
    CHECK(evidenceBundleFieldNames()[1] == "recovered");

    // --- recent-faults: the ids -----------------------------------------------------------------------------------------------------------------------
    {
        const DiagHistory h = readable({report("hang", "FEDCBA9876543210"), report("stall", "0123456789ABCDEF")}, 2);
        const std::string t = recentFaultsText(h);
        CHECK(t == "FEDCBA9876543210 hang, 0123456789ABCDEF stall");
        CHECK(onlyIdsAndKinds(t));
        // The reason, the code, the age and the version of a report never reach it.
        CHECK(t.find("secret") == std::string::npos && t.find("0xC") == std::string::npos && t.find("0.99") == std::string::npos);
        // Lower case is the same id, upper-cased.
        CHECK(recentFaultsText(readable({report("crash", "abcdef0123456789")}, 1)) == "ABCDEF0123456789 crash");
    }
    // THE LISTING IS THE NEWEST TEN: the rest are counted, not named.
    {
        std::vector<ReportSummary> ten;
        for (int i = 0; i < 10; ++i) { ten.push_back(report("hang", "0123456789ABCDEF")); }
        const std::string t = recentFaultsText(readable(ten, 14));
        CHECK(t.find(", +4 more") != std::string::npos);
        CHECK(recentFaultsText(readable(ten, 10)).find("more") == std::string::npos);
        CHECK(recentFaultsText(readable(ten, 40, true)).find(", +30+ more") != std::string::npos);
    }
    // WHAT IS NOT AN ID IS NOT CARRIED.
    {
        for (const char* bad : {"", "C:\\Users\\steve\\x", "ABC", "0123456789ABCDEF0", "0123456789ABCDEG", "0123456789ABCDE", "not a signature", "../../etc/passwd"}) {
            const std::string t = recentFaultsText(readable({report("crash", bad)}, 1));
            CHECK(t == "unsigned crash");
        }
        // A kind that is not one of the three is "unknown", whatever the file said.
        CHECK(recentFaultsText(readable({report("C:\\evil", "0123456789ABCDEF")}, 1)) == "0123456789ABCDEF unknown");
        CHECK(recentFaultsText(readable({report("", "0123456789ABCDEF")}, 1)) == "0123456789ABCDEF unknown");
    }
    // AN EMPTY FOLDER IS "none", AND THE STATES WHERE NOBODY LOOKED ARE NEVER "none".
    {
        CHECK(recentFaultsText(readable({}, 0)) == "none");
        CHECK(recentFaultsText(DiagHistory{}) == "(not read - diagnostics are off)");
        DiagHistory pending;
        pending.included = true;
        pending.pending = true;
        CHECK(recentFaultsText(pending) == "(still being read)");
        DiagHistory unreadable;
        unreadable.included = true;
        CHECK(recentFaultsText(unreadable) == "(the reports folder could not be read)");
        // Still being read wins over an empty listing that has not been filled yet.
        pending.reports.readable = true;
        CHECK(recentFaultsText(pending) == "(still being read)");
    }

    // --- recovered: the words -------------------------------------------------------------------------------------------------------------------------
    {
        health::Counts all;
        health::Counts prior;
        all[health::tokenRecovered(health::Recovered::RingDrop)] = 3;
        all[health::tokenRecovered(health::Recovered::Audio)] = 1;
        // In the vocabulary's order (audio before ringdrop), whatever order the map holds them in.
        CHECK(recoveredText(all, prior, true, true) == "audio 1, ringdrop 3");
        CHECK(onlyIdsAndKinds(recoveredText(all, prior, true, true)));
        // Earlier sessions' unsettled counts are the ledger's, not this session's.
        prior[health::tokenRecovered(health::Recovered::Audio)] = 1;
        prior[health::tokenRecovered(health::Recovered::RingDrop)] = 1;
        CHECK(recoveredText(all, prior, true, true) == "ringdrop 2");
        prior[health::tokenRecovered(health::Recovered::RingDrop)] = 3;
        CHECK(recoveredText(all, prior, true, true) == "none");
        // A token that is not a recovery is not one: a failure count and a slow frame never appear.
        health::Counts mixed;
        mixed["radio_fail.rtlsdr.nodevice"] = 5;
        mixed["slow.render.1s"] = 2;
        mixed["scan_none"] = 1;
        CHECK(recoveredText(mixed, health::Counts{}, true, true) == "none");
        mixed[health::tokenRecovered(health::Recovered::CfgSave)] = 2;
        CHECK(recoveredText(mixed, health::Counts{}, true, true) == "cfgsave 2");
        // EVERY WORD of the vocabulary can come out, once, as its own word.
        health::Counts every;
        for (std::size_t i = 0; i < health::kRecoveredCount; ++i) { every[health::tokenRecovered(static_cast<health::Recovered>(i))] = 1; }
        const std::string t = recoveredText(every, health::Counts{}, true, true);
        for (const std::string& w : health::recoveredWords()) { CHECK(t.find(w + " 1") != std::string::npos); }
    }
    // NOT COUNTING IS NOT "NONE".
    {
        health::Counts all;
        all[health::tokenRecovered(health::Recovered::Audio)] = 1;
        CHECK(recoveredText(all, health::Counts{}, false, true) == "(not counted - usage reporting is off)");
        CHECK(recoveredText(health::Counts{}, health::Counts{}, false, true) == "(not counted - usage reporting is off)");
        CHECK(recoveredText(all, health::Counts{}, true, false) == "(not counted - diagnostics are off)");
        CHECK(recoveredText(health::Counts{}, health::Counts{}, true, true) == "none");
    }
    // THE REAL LEDGER: what is noted into an armed one comes out as its word; an unarmed one counts nothing and says so.
    {
        health::HealthLedger armed;
        armed.arm("", newInstallId(), false);
        armed.note(health::tokenRecovered(health::Recovered::Audio));
        armed.note(health::tokenRecovered(health::Recovered::SrcThread), 2);
        CHECK(armed.armed() && armed.allowed());
        CHECK(recoveredText(armed.counts(), armed.priorCounts(), armed.armed(), armed.allowed()) == "audio 1, srcthread 2");
        armed.setAllowed(false);
        CHECK(recoveredText(armed.counts(), armed.priorCounts(), armed.armed(), armed.allowed()) == "(not counted - diagnostics are off)");
        armed.setAllowed(true);
        armed.disarm();
        CHECK(!armed.armed());
        CHECK(recoveredText(armed.counts(), armed.priorCounts(), armed.armed(), armed.allowed()) == "(not counted - usage reporting is off)");
        health::HealthLedger never;
        never.note(health::tokenRecovered(health::Recovered::Audio));
        CHECK(!never.armed());
        CHECK(recoveredText(never.counts(), never.priorCounts(), never.armed(), never.allowed()) == "(not counted - usage reporting is off)");
    }

    // --- where the lines go ---------------------------------------------------------------------------------------------------------------------------
    {
        const std::string base = "FoxSDR diagnostics bundle\nversion: 1\nslow-frames: none\n\n--- log ---\nline one\n";
        const std::string b = withEvidenceFields(base, "FEDCBA9876543210 hang", "audio 1");
        CHECK(b == "FoxSDR diagnostics bundle\nversion: 1\nslow-frames: none\nrecent-faults: FEDCBA9876543210 hang\nrecovered: audio 1\n\n--- log ---\nline one\n");
        // An empty value says none, a bundle with no log gets the lines at its end, and the log itself is untouched.
        CHECK(withEvidenceFields(base, "", "").find("\nrecent-faults: none\nrecovered: none\n\n--- log ---\n") != std::string::npos);
        CHECK(withEvidenceFields("head\n", "x", "y") == "head\nrecent-faults: x\nrecovered: y\n");
        CHECK(b.substr(b.find("--- log ---")) == "--- log ---\nline one\n");
        // Called after withSlowFramesField's own insert, as the window does, the order is kept.
        const std::string twice = withEvidenceFields(withEvidenceFields(base, "a", "b"), "c", "d");
        CHECK(twice.find("recent-faults: a\nrecovered: b\nrecent-faults: c") != std::string::npos);
    }

    return testSummary("test_diag_evidence");
}
