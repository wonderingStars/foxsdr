// Frequency markers: notes dropped on the waterfall at a frequency of
// interest, WITHOUT tuning to it. A user's request (2026-09-30): "I don't
// always want to tune to an interesting frequency, just take note of it for
// later examination." Right-click the waterfall - the receiver's or a patch
// Display part's - and drop one; list them, copy them, clear them.
//
// A MARKER IS NOT A BOOKMARK. A bookmark is a station the user has named and
// will come back to, with a mode and a bandwidth; a marker is a pencil tick
// made while looking at something else - a frequency, a number to call it by
// and the moment it was made, with an optional note added later. The list
// window can turn one into a bookmark; nothing here depends on FreqManager.
//
// NUMBERS ARE NAMES. M1, M2, ... are handed out in the order markers are
// dropped and are never reused while the list lives, so a "M3" written on a
// scrap of paper keeps meaning the same frequency after M2 is deleted. Only
// clear() starts the count again at 1.
//
// File format (schema 1), beside bookmarks.json:
//   { "schemaVersion": 1, "next": 4,
//     "markers": [ { "n": 1, "freqHz": 145500000.0, "noted": 1790000000,
//                    "note": "..." }, ... ] }
// Load mirrors FreqManager: a missing file is an empty list and true; a file
// that is not JSON, not an object, or of another schema is an empty list and
// false with the reason; a damaged entry (no usable frequency, no positive
// number, a duplicated number) is skipped and the rest load. Save is atomic
// (temp file + rename in the target's own directory).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cascade::core {

struct FreqMarker {
    int number = 0;            // M<number>; unique within the list
    double freqHz = 0.0;       // absolute RF frequency
    std::int64_t notedUnix = 0;  // when it was dropped, seconds since 1970 UTC
    std::string note;          // the user's own words; empty by default
};

class FreqMarkers {
public:
    // Enough for an evening's notes; a list longer than this is a bookmark
    // list and belongs in the Bookmarks section. add() refuses past it.
    static constexpr std::size_t kMaxMarkers = 200;

    // %APPDATA%/foxsdr/markers.json (Windows), XDG/~/.config elsewhere - the
    // directory FreqManager::defaultPath() uses.
    static std::string defaultPath();

    // Drops a marker and returns its number. A frequency within `mergeHz` of
    // an existing marker returns THAT marker's number and adds nothing: a
    // second right-click on the same signal must not stack two flags on one
    // pixel. Returns 0, changing nothing, for a non-finite or non-positive
    // frequency or a full list.
    int add(double freqHz, std::int64_t notedUnix, double mergeHz = 0.0);

    // Removes marker M<number>; false when there is none.
    bool remove(int number);

    // Sets marker M<number>'s note; false when there is none.
    bool setNote(int number, const std::string& note);

    // Empties the list and starts the numbering again at M1.
    void clear();

    // In the order they were dropped (ascending number).
    const std::vector<FreqMarker>& list() const { return list_; }
    bool empty() const { return list_.empty(); }
    std::size_t size() const { return list_.size(); }

    // The number of the marker nearest `freqHz` within `tolHz`, or 0.
    int nearest(double freqHz, double tolHz) const;

    // Bumped on every change, so callers can save only when something moved.
    unsigned version() const { return version_; }

    // The text Copy puts on the clipboard: a heading line, then one marker a
    // line in frequency order, tab-separated so it pastes into a spreadsheet
    // as columns:
    //   FoxSDR markers (2)
    //   M2<TAB>144.800000 MHz<TAB>2026-09-30 14:03:22
    //   M1<TAB>145.500000 MHz<TAB>2026-09-30 14:01:07<TAB>the note
    // Times are local unless `utc` (the tests' deterministic form). An empty
    // list is an empty string.
    std::string clipboardText(bool utc = false) const;

    bool load(const std::string& path, std::string& error);
    // serialize() then writeFile(), as FreqManager's: the GUI takes the text on
    // its own thread and gui::BackgroundSaver runs the blocking half (0.99.64).
    bool save(const std::string& path, std::string& error) const;
    // The file's text, exactly as save() writes it. No disk.
    std::string serialize() const;
    // THE BLOCKING HALF: create the directory, temp file, rename. Static and
    // stateless, so a worker can run it with copies.
    static bool writeFile(const std::string& path, const std::string& text,
                          std::string& error);

private:
    std::vector<FreqMarker> list_;
    int next_ = 1;
    unsigned version_ = 0;
};

// --- where a right-click lands, as a frequency -------------------------------
//
// A pixel on a 2.4 MHz-wide waterfall 1200 px across is 2 kHz; writing the
// cursor's frequency down to the hertz would claim a precision the gesture
// never had. So a dropped marker is rounded to the coarsest 1-2-5 step that
// is no wider than one pixel, and printed with the decimals that step needs.

// The 1-2-5 step (1, 2, 5, 10, 20, 50 ... Hz) that is the largest not
// exceeding `hzPerPixel`; 1 Hz for anything at or below 1 Hz, NaN or
// non-positive.
double markerStepHz(double hzPerPixel);

// `hz` rounded to markerStepHz(hzPerPixel).
double roundMarkerHz(double hz, double hzPerPixel);

// MHz decimals that show a `stepHz` step and no more: 1 kHz -> 3, 100 Hz ->
// 4, 50 Hz -> 5, 1 Hz -> 6; never fewer than 3 or more than 6.
int markerMhzDecimals(double stepHz);

// "145.500" style, with markerMhzDecimals(stepHz) decimals and no unit.
std::string formatMarkerMhz(double hz, double stepHz);

// A stored marker as MHz text, in the fewest decimals (3 to 6) that state it
// to the hertz: 145500000 -> "145.500", 7074050 -> "7.07405". Used where the
// view it was dropped from - and so its step - is no longer known.
std::string markerMhzText(double hz);

// "2026-09-30 14:03:22", local time unless `utc`; empty when the platform
// cannot convert the stamp.
std::string formatMarkerTime(std::int64_t unixSeconds, bool utc = false);

}  // namespace cascade::core
