// wire_b_needed_libs.hpp - the NEEDED LIBRARIES as the engine shows them: JSON of the ONE table (source/needed_library.hpp: needed::Info rows, needed::Check answers).
// The owner's needed-library rule (2026-11-12; docs/multi-radio-records/wire-b.md "NEEDED LIBRARIES", engine-fixes-4.md) wants one row type, so the table and the checks
// live in source/needed_library.{hpp,cpp} (nine rows) and this file only writes them out for the fact `needed.libs` and the answer of the operation
// NEEDED_LIB_CHECK 0x8930 ("Check again").
//
// A CHECK never loads vendor code except for the one row whose bridge has a silent load check (ftdi-d2xx); every other file row reads headers only (Check::depth).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include "engine/wire_b_sigmeas.hpp"   // wireb::Json
#include "source/needed_library.hpp"

namespace cascade::engine::wireb {

// One row as the fact shows it: every field of needed::Info that a screen draws, plus "check" (the last answer, or null when never checked).
Json neededToJson(const cascade::source::needed::Info& row, const Json& lastCheck);

// One answer: {state (one of the seven words), sentence, looked_in, found_in (a folder, never a file path), file, version, missing, depth}.
Json checkToJson(const cascade::source::needed::Check& c);

}  // namespace cascade::engine::wireb
