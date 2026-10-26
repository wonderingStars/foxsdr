// engine_markers.cpp - THE WATERFALL'S MARKERS, worked through the table (stub campaign B10, MARKERS-A; master 0.99.57).
//
// THE REQUEST (a user, 2026-09-30): "I don't always want to tune to an interesting frequency, just take note of it for later examination ... a right click
// menu on the waterfall display that would give you an option to drop a marker on the current cursor position ... clear all markers, list all markers and
// maybe copy all markers to the clipboard."
//
// WHAT WAS THE ENGINE'S ALREADY. The list (core/freq_markers.hpp: numbers, the merge rule, the 200 cap, the file's format), its load at start-up and its
// save on a worker half a second after a change (Engine::flushMarkerSave, markers.json beside bookmarks.json) were the engine's since the extraction - in
// master the window kept them, and the engine kept them for it, so the file's persistence is one thing with the bookmarks'. What nothing could do was
// REACH them: the window worked its own list in master (`freqMarkers_.add(...)` from a menu item) and the port has no engine member to name.
//
// WHAT IS HERE: the four commands (FOXAPP_OP_MARKER_DROP, _REMOVE, _CLEAR, _NOTE; docs/api-0.3-additions.md 9.8), each a one-line call on that list from the
// pass, so a drop made in the window and one made from a browser or a plugin are the same thing and one numbering. The list is read as FREQ_MARKERS (53) and
// the facts fact.markers.* (engine_api_lists.cpp). All four are LOCAL-only, like every application op: a REMOTE session has the web remote's power, and a
// marker is a note the local operator made for themselves.
//
// THE NUMBERS ARE NAMES. M1, M2, ... are handed out in the order markers are dropped and never again while the list lives, so "M3" written on a scrap of
// paper keeps meaning the same frequency after M2 is removed. FreqMarkers keeps the count (and writes it in the file: "next"), this file only never hands
// a number out of its own - a drop answers with the number the LIST gave. Only CLEAR starts the count at M1 again.
//
// ABSOLUTE HERTZ. A marker is dropped at the frequency under the pointer, on the receiver's waterfall or on a patch Display part's; the window works the part's
// radio's centre in before it sends, so the engine only ever sees the absolute frequency (never the part's offset from its radio).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstdint>
#include <ctime>
#include <limits>
#include <string>

#include "core/app_commands.hpp"
#include "core/freq_markers.hpp"
#include "engine/engine.hpp"
#include "foxsdr_api.h"

namespace cascade::engine {

std::int32_t Engine::applyMarkerOp(const FoxCommand& c, const std::string& text, FoxCommandResult& res, std::string& why) {
    // A marker is named by its number: an int the list could hold. A 64-bit figure outside that range names none.
    const auto numberOf = [&c]() -> int {
        const long long v = static_cast<long long>(c.ival[0]);
        return (v >= 1 && v <= std::numeric_limits<int>::max()) ? static_cast<int>(v) : 0;
    };
    switch (c.op) {
        case FOXAPP_OP_MARKER_DROP: {
            const double hz = c.num[0];
            if (!std::isfinite(hz) || !(hz > 0.0)) {
                why = "a marker needs a frequency above zero";
                return FOXAPI_OUT_OF_RANGE;
            }
            // The reach: "the same signal" is within it. Nothing negative or odd widens it past what was asked (0 is none: only the same frequency).
            const double reach = (std::isfinite(c.num[1]) && c.num[1] > 0.0) ? c.num[1] : 0.0;
            const unsigned before = freqMarkers_.version();
            const int number = freqMarkers_.add(hz, static_cast<std::int64_t>(std::time(nullptr)), reach);
            if (number == 0) {
                why = "the list is full - it holds 200 markers";
                return FOXAPI_LIMIT;
            }
            res.applied[0] = static_cast<double>(number);
            res.applied[1] = freqMarkers_.version() != before ? 1.0 : 0.0;
            return FOXAPI_OK;
        }
        case FOXAPP_OP_MARKER_REMOVE: {
            const int number = numberOf();
            if (number == 0 || !freqMarkers_.remove(number)) {
                why = "no marker with that number";
                return FOXAPI_NOT_FOUND;
            }
            return FOXAPI_OK;
        }
        case FOXAPP_OP_MARKER_CLEAR:
            if (freqMarkers_.empty()) {
                why = "there are no markers";
                return FOXAPI_NO_CHANGE;
            }
            freqMarkers_.clear();
            return FOXAPI_OK;
        case FOXAPP_OP_MARKER_NOTE: {
            const int number = numberOf();
            // setNote is true for a marker that is there (and moves the counter only when the note differs): the same note again writes nothing.
            if (number == 0 || !freqMarkers_.setNote(number, text)) {
                why = "no marker with that number";
                return FOXAPI_NOT_FOUND;
            }
            return FOXAPI_OK;
        }
        default:
            why = "not a marker op";
            return FOXAPI_UNSUPPORTED;
    }
}

}  // namespace cascade::engine
