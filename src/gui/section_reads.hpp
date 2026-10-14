// section_reads.hpp - the pure parts of what the Recorder, Bookmarks and Scanner sections read
// from the engine's table and send back (window port, group 7, docs/engine-stage4.md 6.5; API
// 0.3 section 8.6). No ImGui, no engine object: arithmetic over rows, numbers and the state, so a
// test can hold each rule against the model it restates.
//
//   - the recorder: how many samples a take's bytes are (a frame's size by the take's kind)
//   - the bookmarks: the group names the section offers, derived from the window's one copy of
//     the list, and the filter that picks the rows it shows (the rule the section always had:
//     case-insensitive substring over name and group, or a frequency in MHz that starts with
//     what was typed, one group, favourites only)
//   - the scanner: the seven values its form shows, from the state, and the two commands its
//     edits send (the timings through FOXAPI_OP_SCANNER_CONFIG, the range through
//     FOXAPP_OP_SCANNER_RANGE), built exactly as the section built them from the engine's fields
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/app_commands.hpp"
#include "foxsdr_api.h"
#include "gui/tuning_reads.hpp"

namespace cascade::gui {

// --- the recorder ------------------------------------------------------------------------

// What a take counts as a sample: Recorder::flushStage adds bytes / bytesPerFrame to its frame
// counter, a frame being two 32-bit floats (I/Q) or one 16-bit sample (audio). RECORDINGS gives
// the bytes (value[1], live), so the section lettering "samples" divides them by the same size.
inline constexpr unsigned kIqFrameBytes = 8u;
inline constexpr unsigned kAudioFrameBytes = 2u;
inline std::uint64_t takeSamples(double bytes, unsigned bytesPerFrame) {
    if (!(bytes > 0.0) || bytesPerFrame == 0u) { return 0u; }
    return static_cast<std::uint64_t>(bytes) / bytesPerFrame;
}

// --- the bookmarks -----------------------------------------------------------------------

// The group names the section's combo offers: the distinct non-empty groups of the copy, sorted
// (by bytes, as the model's std::map and std::sort are). Pointers are sorted and only the
// distinct names copied, so a list of 33 000 builds a few strings and not 33 000.
inline std::vector<std::string> bookmarkGroupNames(const std::vector<BookmarkRow>& rows) {
    std::vector<const std::string*> names;
    for (const BookmarkRow& b : rows) {
        if (!b.group.empty()) { names.push_back(&b.group); }
    }
    std::sort(names.begin(), names.end(), [](const std::string* a, const std::string* b) { return *a < *b; });
    names.erase(std::unique(names.begin(), names.end(),
                            [](const std::string* a, const std::string* b) { return *a == *b; }),
                names.end());
    std::vector<std::string> out;
    out.reserve(names.size());
    for (const std::string* n : names) { out.push_back(*n); }
    return out;
}

// The rows of the copy the section shows, as indices into it, in the copy's order: the filter is
// matched case-insensitively against the name and the group, and a filter that reads as a number
// also matches frequencies that start with it in MHz ("145.5"); `group` ("" for every group) and
// `favouritesOnly` narrow it. `out` is cleared and keeps its room.
inline void bookmarkView(const std::vector<BookmarkRow>& rows, const std::string& group, bool favouritesOnly,
                         const std::string& filter, std::vector<std::uint32_t>& out) {
    std::string needle = filter;
    for (char& c : needle) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    const auto contains = [](const std::string& hay, const std::string& n) {
        if (n.empty()) { return true; }
        const auto it = std::search(hay.begin(), hay.end(), n.begin(), n.end(), [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) == b;
        });
        return it != hay.end();
    };
    out.clear();
    char mhz[32];
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const BookmarkRow& b = rows[i];
        if (favouritesOnly && !b.favourite) { continue; }
        if (!group.empty() && b.group != group) { continue; }
        if (!needle.empty()) {
            std::snprintf(mhz, sizeof(mhz), "%.6f", b.hz / 1e6);
            if (!contains(b.name, needle) && !contains(b.group, needle) &&
                std::strncmp(mhz, needle.c_str(), needle.size()) != 0) {
                continue;
            }
        }
        out.push_back(static_cast<std::uint32_t>(i));
    }
}

// --- the scanner -------------------------------------------------------------------------

// The seven values the form shows, in its order: start MHz, stop MHz, step kHz, dwell, hold,
// resume, listen ms - from the state (the range in Hz, the timings in whole milliseconds).
inline void scannerFormValues(const FoxReceiverState& st, double (&out)[7]) {
    out[0] = st.scanStartHz / 1.0e6;
    out[1] = st.scanStopHz / 1.0e6;
    out[2] = st.scanStepHz / 1.0e3;
    out[3] = static_cast<double>(st.scanDwellMs);
    out[4] = static_cast<double>(st.scanHoldMs);
    out[5] = static_cast<double>(st.scanResumeMs);
    out[6] = static_cast<double>(st.scanListenMs);
}

// The timing command for the edited fields among bits 0x08 dwell, 0x10 hold, 0x20 resume, 0x40
// listen: the draft's values for those, the engine's own (the state's) for the rest, so a field
// nobody touched keeps whatever the engine holds. FOXAPI_OP_SCANNER_CONFIG carries all four and
// reconfigures a running scan by itself.
inline FoxCommand scannerTimingCommand(unsigned bits, const double (&draft)[7], const FoxReceiverState& st) {
    FoxCommand c = cascade::core::cmd::make(FOXAPI_OP_SCANNER_CONFIG);
    c.num[0] = (bits & 0x08u) != 0 ? draft[3] : static_cast<double>(st.scanDwellMs);
    c.num[1] = (bits & 0x10u) != 0 ? draft[4] : static_cast<double>(st.scanHoldMs);
    c.num[2] = (bits & 0x20u) != 0 ? draft[5] : static_cast<double>(st.scanResumeMs);
    c.num[3] = (bits & 0x40u) != 0 ? draft[6] : static_cast<double>(st.scanListenMs);
    return c;
}

// The range command: the edited range fields' bits (1 start, 2 stop, 4 step) and, when a running
// scan is to be reconfigured, bit 8; the three values from the draft in Hz.
inline FoxCommand scannerRangeCommand(unsigned bits, bool reconfigureRange, const double (&draft)[7]) {
    FoxCommand c = cascade::core::cmd::make(FOXAPP_OP_SCANNER_RANGE);
    c.ival[0] = static_cast<std::int64_t>(bits & 0x07u) | (reconfigureRange ? 8 : 0);
    c.num[0] = draft[0] * 1.0e6;
    c.num[1] = draft[1] * 1.0e6;
    c.num[2] = draft[2] * 1.0e3;
    return c;
}

// START: the range the engine stores (the state's), as SCANNER_RUN carries it.
inline FoxCommand scannerStartCommand(const FoxReceiverState& st) {
    FoxCommand c = cascade::core::cmd::makeInt(FOXAPI_OP_SCANNER_RUN, 1);
    c.num[0] = st.scanStartHz;
    c.num[1] = st.scanStopHz;
    c.num[2] = st.scanStepHz;
    return c;
}

}  // namespace cascade::gui
