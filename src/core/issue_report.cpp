// issue_report.cpp - see the header.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/issue_report.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "core/crash_handler.hpp"
#include "core/diag_log.hpp"
#include "core/diag_report.hpp"

namespace cascade::core::issue {

namespace {

using health::Recovered;

// THE SENTENCES, in the enum's order. Each says what the person met and what the program
// did, in the words PRIVACY.md's "Recovered from" table uses, so a reader of the feed and
// a reader of the privacy page meet the same description. No word of any sentence comes
// from the machine: no device, no file, no frequency, no message from a driver.
constexpr const char* kSentences[health::kRecoveredCount] = {
    // Audio
    "the sound output stopped and the watchdog restarted it",
    // Reopen
    "a SoapySDR radio's driver faulted and the radio was reopened by itself",
    // SrcThread
    "a radio driver's read never returned when the receiver was stopped, and the thread "
    "was let go rather than freeze the window",
    // VendorCall
    "a call into a SoapySDR vendor module did not return in time and was let go",
    // RingDrop
    "the radio delivered samples faster than they were taken off and some were dropped",
    // DspExc
    "the signal-processing thread threw an error that was caught, and the receiver stopped "
    "rather than the program",
    // CfgSave
    "the settings file could not be written",
    // EnumChild
    "the helper that lists SoapySDR radios died and was contained",
    // PluginApi
    "a plugin's call into FoxSDR threw an error and was answered with a default",
    // WebRoute
    "a web-interface handler threw an error and the server answered with a failure",
    // PatchLoad
    "a saved patch loaded with connections left out",
    // SdrEnum
    "the SDRplay service did not answer a scan, and the scan was let go",
    // SdrLost
    "the SDRplay API session was lost and SDRplay is shut for the rest of the session",
    // AudioLead
    "the audio buffer was made deeper because this computer fell behind",
    // Viewport
    "the graphics driver refused a second window for a page torn off the main one, and every "
    "page is kept inside the main window for the rest of the session",
};

// One bit per word: set when its report has been filed this process. Atomic, so two
// threads meeting the same word at once file one report between them.
std::atomic<std::uint32_t> g_filed{0};
// The sequence field of the file name, so two words filed in one second are two files.
std::atomic<unsigned> g_seq{0};

std::string stamp() {
    const std::time_t t = std::time(nullptr);
#if defined(_WIN32)
    std::tm tmv{};
    localtime_s(&tmv, &t);
    char buf[32] = {};
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d%02d%02d", tmv.tm_year + 1900, tmv.tm_mon + 1,
                  tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return buf;
#else
    // The Linux crash handler's own naming: epoch seconds.
    return std::to_string(static_cast<long long>(t));
#endif
}

unsigned long long processId() {
#if defined(_WIN32)
    return ::GetCurrentProcessId();
#else
    return static_cast<unsigned long long>(::getpid());
#endif
}

}  // namespace

const char* issueSentence(Recovered word) {
    const std::size_t i = static_cast<std::size_t>(word);
    return i < health::kRecoveredCount ? kSentences[i] : "";
}

std::string issueReason(Recovered word) {
    const char* w = health::recoveredWord(word);
    if (w == nullptr || w[0] == '\0') { return std::string(); }
    return std::string(kIssueReasonPrefix) + w + " - " + issueSentence(word);
}

std::string issueSignature(Recovered word) {
    // FNV-1a, 64 bits, over "issue|<word>": a fixed function of a fixed word. Sixteen
    // upper-case hex digits, the width the crash and freeze signatures have.
    const std::string key = std::string("issue|") + health::recoveredWord(word);
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : key) {
        h ^= c;
        h *= 1099511628211ull;
    }
    static const char kHex[] = "0123456789ABCDEF";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<std::size_t>(i)] = kHex[h & 0xFu];
        h >>= 4;
    }
    return out;
}

std::string renderIssueReport(Recovered word) {
    std::string out;
    out.reserve(16384);
    // THE HEADER: the fields issueReportFieldNames() declares and PRIVACY.md lists, and
    // no others. No `code` (nothing faulted), no `address`, no `thread`.
    out += "kind: issue\n";
    out += "reason: " + issueReason(word) + "\n";
    out += "signature: " + issueSignature(word) + "\n";
    // THE CONTEXT, the same bytes a crash or freeze report carries, and the window's
    // last-known state beside it, as the freeze report has it.
    out += "--- context ---\n";
    out += diagContextBlock();
    {
        char stateLine[192];
        const std::size_t n = renderWindowStateLine(stateLine, sizeof(stateLine));
        out.append(stateLine, n);
    }
    out += "--- process ---\n";
    out += "uptime-sec: " + std::to_string(static_cast<unsigned long long>(processUptimeSec())) + "\n";
    // THE LOG, which is what a report of a handled fault is for: the lines before it say
    // what the driver said.
    const std::vector<std::string> ring = DiagLog::instance().ringSnapshot();
    out += "--- log (last " + std::to_string(ring.size()) + " of " +
           std::to_string(static_cast<unsigned long long>(DiagLog::instance().linesWritten())) +
           " lines) ---\n";
    for (const std::string& line : ring) {
        out += line;
        out += '\n';
    }
    return out;
}

std::string fileIssueReport(Recovered word) {
    const std::size_t i = static_cast<std::size_t>(word);
    if (i >= health::kRecoveredCount) { return std::string(); }
    // Consent and a directory, both, or nothing is written - the crash handler's own
    // answer, so the Settings switch means the same thing for every report.
    const std::string dir = activeCrashDir();
    if (dir.empty()) { return std::string(); }
    const std::uint32_t bit = 1u << i;
    if ((g_filed.fetch_or(bit, std::memory_order_acq_rel) & bit) != 0) { return std::string(); }

    const std::string path = dir + "/issue-" + stamp() + "-" + std::to_string(processId()) + "-" +
                             std::to_string(g_seq.fetch_add(1u) + 1u) + ".txt";
    const std::string text = renderIssueReport(word);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) { return std::string(); }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
    if (!out) { return std::string(); }
    diagLogf("issue report written - %s (recovered.%s)", path.c_str(), health::recoveredWord(word));
    return path;
}

void resetIssueReportsForTest() { g_filed.store(0u, std::memory_order_release); }

}  // namespace cascade::core::issue
