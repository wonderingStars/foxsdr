// Band plan: a sorted list of named frequency allocations, loaded from JSON,
// used to draw labelled service bands behind the spectrum and to answer
// "what am I tuned to?".
//
// This module is pure model — no GUI types, no drawing. A later pass renders
// it; everything here is headless-testable.
//
// DATA PROVENANCE (important, this product ships commercially): the JSON files
// under resources/bandplans/ were authored from PUBLIC REGULATORY ALLOCATIONS
// and common knowledge — ITU Region definitions, Ofcom UK allocations, and the
// standard broadcast / amateur / airband / marine ranges every radio reference
// lists. No band plan file from any other project was consulted or copied. The
// data is INFORMATIONAL, not authoritative: it is a display aid, never a
// licensing reference.
//
// File format (one plan per file):
//   {
//       "name": "United Kingdom",
//       "note": "free text, ignored by the loader",
//       "bands": [
//           { "start": 87500000, "end": 108000000,
//             "name": "FM Broadcast", "service": "broadcast" },
//           ...
//       ]
//   }
// Unknown top-level and per-band keys are ignored (forward compatibility).
// Colour is NOT in the file: it is derived from `service` by the loader, so a
// hand-written plan cannot produce an unreadable overlay and so the palette
// stays consistent across every plan the user has installed.
//
// SERVICE CLASSES and PALETTE. `service` is a coarse class string, one of
// broadcast | amateur | aviation | marine | mobile | satellite | iss | other.
// colorRgba is packed 0xRRGGBBAA (red in the most significant byte, alpha in
// the least). The hues are the Okabe-Ito colour-blind-safe set, which stays
// distinguishable under deuteranopia, protanopia and tritanopia, and every one
// of them is light enough to read against the dark theme's near-black panel:
//
//   broadcast   0xE69F00  orange
//   amateur     0x56B4E9  sky blue
//   aviation    0x009E73  bluish green
//   marine      0x0072B2  blue
//   mobile      0xD55E00  vermillion
//   satellite   0xCC79A7  reddish purple
//   iss         0xF0E442  yellow
//   other       0x9E9E9E  neutral grey  <- FALLBACK for a missing, empty,
//                                          wrong-typed or unrecognised
//                                          service; the entry still loads,
//                                          it just draws as "unclassified"
//
// Alpha is 0x60 (96/255) on every class: these are background fills the
// spectrum trace has to remain readable through, so the alpha is a property
// of the ROLE, not of the class, and must not vary between services.
//
// Load semantics:
//   - loadFile REPLACES the plan's contents on success.
//   - On ANY failure the previous contents and name are left UNCHANGED. This
//     deliberately differs from ConfigStore (which resets to defaults on a bad
//     file): a band plan is display data the user may have just re-picked from
//     a file dialog, and blanking a working overlay because the new pick was
//     malformed is worse than keeping what is on screen and reporting the
//     error.
//   - Missing "bands" key -> empty plan, returns true (a valid, empty plan).
//   - "bands" present but not an array -> false (structural damage).
//   - Missing / non-string "name" -> the file's stem is used as the plan name.
//   - PER-ENTRY TOLERANCE, mirroring FreqManager: a damaged band is skipped
//     and the rest load. A band is skipped when it is not a JSON object, when
//     "start" or "end" is absent or non-numeric or non-finite, or when
//     end <= start (a zero-width or inverted band has nothing to draw and
//     would break the narrowest-wins rule in at()).
//
// ORDER. entries() is always sorted by startHz ascending, ties broken by
// endHz DESCENDING — the widest band first. That makes the vector a valid
// back-to-front draw order all by itself: a containing band (Airband
// 108-137) always precedes the bands nested inside it (VOR/ILS 108-117.975),
// so a renderer that simply walks the vector paints the wide backdrop before
// the narrow detail on top of it.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cascade::core {

struct BandEntry {
    double startHz = 0, endHz = 0;
    std::string name;             // "FM Broadcast", "2m Amateur", ...
    std::string service;          // coarse class, see header comment
    std::uint32_t colorRgba = 0;  // 0xRRGGBBAA, derived from `service`
};

class BandPlan {
public:
    // "<directory of the running executable>/resources/bandplans".
    //
    // Deliberately executable-relative, not CWD-relative and not
    // %APPDATA%-relative: the shipped plans are read-only program DATA
    // installed alongside cascade.exe by the installer, so they must be
    // found no matter what directory the user launched from. If the
    // executable path cannot be determined (a stripped or unusual
    // environment), the path degrades to the relative "resources/bandplans",
    // which at least resolves when run from an install root.
    static std::string defaultDir();

    // Replaces the plan with the file's content. See the load semantics in
    // the header comment; on false the previous contents survive intact.
    bool loadFile(const std::string& path, std::string& error);

    // Merges every *.json (case-insensitive extension) directly inside `dir`
    // into ONE plan, sorted as a whole. Files are visited in lexicographic
    // filename order so the merge is deterministic across filesystems, and
    // name() becomes the individual plan names joined with " + ".
    //
    // ALL-OR-NOTHING: if any file fails to parse, nothing is committed, the
    // error names that file, and the previous contents survive — a half
    // merged plan would silently hide bands the user expects to see.
    // A `dir` that is missing or is not a directory is an error; a directory
    // containing no *.json is not (it yields an empty plan and true).
    bool loadDirectory(const std::string& dir, std::string& error);

    // Sorted by startHz ascending, widest-first on ties (see header).
    const std::vector<BandEntry>& entries() const { return entries_; }

    // Display name of the loaded plan; "" when empty/cleared.
    const std::string& name() const { return name_; }

    // Entries overlapping the window [lowHz, highHz], in entries() order, so
    // a renderer draws only what is on screen. Overlap is STRICT at both
    // edges (entry.start < highHz && entry.end > lowHz): a band that merely
    // touches the window edge — FM Broadcast ending exactly where the view
    // starts at 108 MHz — covers zero pixels and is not returned.
    //
    // An empty or inverted window (highHz <= lowHz), or a non-finite one,
    // returns an empty vector. The pointers alias the internal vector and are
    // invalidated by loadFile / loadDirectory / clear().
    std::vector<const BandEntry*> visible(double lowHz, double highHz) const;

    // The most specific (NARROWEST) entry containing hz, or nullptr when hz
    // falls in a gap — this is the "you are tuned to: Airband Voice" readout,
    // and the narrowest match is the informative one (145.800 MHz should read
    // "ISS Downlink", not the 2 m amateur band that contains it).
    //
    // Containment is INCLUSIVE at both ends, so tuning exactly to a band edge
    // still names a band. Two entries of identical width containing hz —
    // which happens at the shared edge of two adjacent bands — resolve to the
    // first in entries() order, keeping the readout deterministic. Non-finite
    // hz returns nullptr.
    const BandEntry* at(double hz) const;

    // Empties the entries and the name.
    void clear();

    // The palette in the header comment, exposed so a later GUI pass can
    // colour a service legend identically without duplicating the table.
    // Any unrecognised string maps to the "other" grey.
    static std::uint32_t colorForService(const std::string& service);

private:
    // APPENDS the file's bands to `out` (unsorted) and reports its plan name;
    // shared by loadFile and the merging loop in loadDirectory. On false,
    // `out` may hold partial results — both callers discard it in that case.
    static bool parseInto(const std::string& path, std::vector<BandEntry>& out,
                          std::string& planName, std::string& error);

    std::vector<BandEntry> entries_;
    std::string name_;
};

}  // namespace cascade::core
