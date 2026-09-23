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
// comma, semicolon or tab separated (whichever the first line uses most),
// quoted fields allowed, an optional header naming the columns in any order
// (frequency/freq, name, group, mode, bandwidth, favourite). Without a header
// the columns are frequency, name, group, mode, bandwidth. A frequency is in
// Hz unless the header says MHz or kHz, or it is under 100 000 - which no
// receivable frequency in Hz is, and every one written in MHz is.
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

// The bandwidth a mode gets when a list does not give one - an NFM channel
// imported at a 150 kHz default would swallow its neighbours.
double defaultBandwidthForMode(const std::string& mode);

}  // namespace cascade::core
