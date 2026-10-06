// freq_import - frequency lists in and out: SDR#'s frequencies.xml and CSV.
//
// Asked for by a user who keeps 10 000 to 33 000 entries in Excel and merges
// them into SDR#'s memory format by hand. Two things they said shaped this:
//
//   - their merged files are not always well-formed: one entry per line with
//     the closing tag typed as "/MemoryEntry>" (no '<'). So this is NOT an XML
//     parser. It finds each "<MemoryEntry" and reads the fields up to the next
//     one, which is exactly as tolerant as the file needs and no more - a
//     field is still only read from a proper <Tag>value</Tag> pair;
//   - "it slows down the program itself" in SDR#. Import is one pass over the
//     text (33 000 entries in well under a second) and the result goes to
//     FreqManager::addMany, one sort, never an insert per entry.
//
// The SDR# fields used: Name, GroupName, Frequency (Hz), DetectorType,
// FilterBandwidth (Hz), IsFavourite. Shift is a converter offset; it is not
// applied - the frequency is taken as written - and the result counts how many
// entries carried one so the user is told rather than silently mistuned.
// CenterFrequency is SDR#'s own tuning choice and is ignored.
//
// CSV is the other door, because an Excel user can "Save as CSV" directly:
// comma, semicolon or tab separated (whichever the header or first data line
// uses most), quoted fields allowed, an optional header naming the columns in
// any order (frequency/freq, name, group, mode, bandwidth, favourite, and since
// 0.99.66 ticked - or scan - which is Bookmark::scan: 1, true, yes or y). Without
// a header the columns are frequency, name, group, mode, bandwidth. A frequency
// is in Hz unless the header says MHz or kHz, or it is under 100 000 - which no
// receivable frequency in Hz is, and every one written in MHz is. A bandwidth
// is in Hz unless its header says MHz or kHz. Lines before the list starts -
// a "# exported ..." comment, an Excel title row: anything that neither starts
// with a number nor names a frequency column - are skipped and counted in
// `skipped`, rather than taken as a header that names nothing.
//
// CSV OUT (0.99.66, exportCsv): the AIRBAND section's presets - a preset is a
// group of the frequency list - are written as the CSV this reads, one file per
// preset, header frequency_mhz,name,group,mode,bandwidth_hz,favourite,ticked.
// Unlike the SDR# XML it keeps the tick, so a preset that goes out and comes
// back is the same preset. The file is what the importer reads, in a
// spreadsheet or an editor, and nothing here is a new format: it is the CSV
// above with every column named.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "core/freq_manager.hpp"

namespace cascade::core {

struct ImportResult {
    std::vector<Bookmark> items;
    std::size_t entries = 0;   // entries seen in the file
    std::size_t skipped = 0;   // seen but unusable (no frequency, not a number)
    std::size_t shifted = 0;   // carried a non-zero SDR# Shift, not applied
    std::string format;        // "SDR# XML" or "CSV"
    std::string error;         // set when nothing could be read at all
};

// SDR# frequencies.xml text.
ImportResult importSdrSharpXml(std::string_view text);

// CSV text.
ImportResult importCsv(std::string_view text);

// Either, decided from the content: anything containing "<MemoryEntry" is
// SDR#'s format, everything else is tried as CSV.
ImportResult importFrequencyList(std::string_view text);

// Reads a file and imports it. A file that cannot be read sets error.
ImportResult importFrequencyFile(const std::string& path);

// SDR#'s frequencies.xml for `list`, so a list built here can go back.
std::string exportSdrSharpXml(const std::vector<Bookmark>& list);

// A CSV for `list` (0.99.66): the header frequency_mhz,name,group,mode,
// bandwidth_hz,favourite,ticked, then one row per bookmark - the frequency in
// MHz to six places (the hertz), the favourite and the tick as 1 or 0 (a
// spreadsheet in any language keeps a number), and a field quoted, its quotes
// doubled, when it holds a comma or a quote. A line break in a name, group or
// mode is written as a space (importCsv cuts the text into lines before it reads
// a quoted field, so a break kept inside quotes would lose the row its columns).
// CRLF line ends, as the XML export has, and the text starts with a UTF-8 byte
// order mark, which Excel needs to read a name outside ASCII and importCsv drops.
// importCsv reads it back whole.
std::string exportCsv(const std::vector<Bookmark>& list);

// WHERE AN IMPORT'S ROWS GO (0.99.66). The AIRBAND section imports a file INTO a
// preset: every row joins the group `group` - whatever group the file gave it -
// and, with `tick`, the rows the AIRBAND monitor can play (AM and NFM) are ticked
// and the others (WFM, SSB, CW...) are added unticked: a tick on a row the
// section does not list could not be undone there, and the Scanner's list mode
// would take it. An empty `group` is the Bookmarks section's and a dropped
// file's import: the rows are left as the file wrote them.
struct ImportInto {
    std::string group;
    bool tick = false;
};

// Puts `into` on every row of `items` (nothing when into.group is empty). The
// window calls it on its own thread, after the worker has read the file and
// before the rows are added to the list.
void applyImportInto(std::vector<Bookmark>& items, const ImportInto& into);

// The bandwidth a mode gets when a list does not give one - an NFM channel
// imported at a 150 kHz default would swallow its neighbours.
double defaultBandwidthForMode(const std::string& mode);

}  // namespace cascade::core
