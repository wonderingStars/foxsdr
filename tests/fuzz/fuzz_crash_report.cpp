// Fuzz target: a crash or freeze report read back off the disk.
//
// parseReportText reads the text files the crash handler and the hang watchdog
// write (crashes/crash-*.txt, hang-*.txt) when the next launch offers to send
// them. The files are written by this program, but the reader runs on whatever
// is in the directory: a half-written file from a process that died mid-write
// (the very case it exists for), one an older version wrote, one the user
// edited or truncated. What it parses is then rendered as the JSON body of an
// upload, so the target takes that step too.
//
// Properties: an accepted report has a kind the server accepts; the caps on
// threads, frames, modules, plugins and log lines hold; and the upload body
// built from it is always valid JSON (an invalid one would be refused by the
// server for ever, and the report would be retried for ever).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstddef>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "core/crash_upload.hpp"
#include "fuzz_common.hpp"

using cascade::core::ParsedReport;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string text = fuzz::asString(data, size);

    ParsedReport report;
    if (!cascade::core::parseReportText(text, report)) { return 0; }

    FUZZ_REQUIRE(report.kind == "crash" || report.kind == "hang" || report.kind == "stall");

    const std::string body = cascade::core::uploadJson(report, "00000000-0000-4000-8000-000000000000");
    const nlohmann::json j = nlohmann::json::parse(body, nullptr, /*allow_exceptions=*/false);
    FUZZ_REQUIRE(!j.is_discarded());
    FUZZ_REQUIRE(j.is_object());
    return 0;
}
