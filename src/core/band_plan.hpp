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
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
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

// One plan file's identity, without its bands — what the picker needs to
// build a menu without parsing every band in every installed plan.
struct PlanInfo {
    std::string id;    // "id" key, else the filename stem. Unique per directory.
    std::string name;  // display name ("United Kingdom")
    std::string base;  // id of the plan this REFINES; empty when it is a baseline
    std::string path;  // the file it came from
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

    // Every plan installed in `dir`, sorted by display name, WITHOUT loading
    // any bands — this is what fills the picker.
    //
    // Deliberately failure-TOLERANT, unlike loadDirectory's all-or-nothing
    // merge: a file that will not parse is skipped and the rest are listed.
    // A menu that vanishes entirely because one hand-edited file lost a brace
    // would hide every working plan the user has, which is a worse failure
    // than one absent entry. A missing directory yields an empty vector.
    static std::vector<PlanInfo> available(const std::string& dir);

    // Loads ONE plan and the chain of plans it refines, instead of merging
    // everything in the directory.
    //
    // WHY THIS EXISTS. loadDirectory merges every file, which is right only
    // when the files agree. "United Kingdom" refines "ITU Region 1" and the
    // two are consistent, so merging them is exactly what is wanted. But
    // Region 1 and Region 2 genuinely CONTRADICT each other — 40 m is
    // 7.0-7.2 MHz in Region 1 and 7.0-7.3 in Region 2, mediumwave has
    // different edges, FM broadcast starts at 87.5 rather than 88.0 — and
    // merging those does not produce a superset, it produces an overlay that
    // confidently labels the wrong answer. at()'s narrowest-wins rule then
    // PREFERS the contradicting entry, because the narrower of two disagreeing
    // bands wins. Shipping plans for the whole world therefore requires
    // choosing one, and this is that choice.
    //
    // `id` names a plan from available(). The chain is walked through `base`
    // and loaded BASELINE FIRST, so a refinement's narrower bands sort on top
    // of the region's wider ones. name() becomes the chain's display names
    // joined with " + ", e.g. "ITU Region 1 + United Kingdom".
    //
    // An EMPTY id restores the legacy behaviour and merges the whole
    // directory, which is what an unset configuration means.
    //
    // Errors, all leaving the previous contents intact: `dir` unreadable, no
    // plan with that id, a file in the chain that will not parse, or a `base`
    // cycle (self-reference or a loop) — a cycle is reported rather than
    // silently truncated, because it means the data is wrong and a truncated
    // chain would quietly drop bands.
    bool loadSelection(const std::string& dir, const std::string& id,
                       std::string& error);

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
    // APPENDS the file's bands to `out` (unsorted) and reports its identity;
    // shared by loadFile, the merging loop in loadDirectory, and the chain
    // walk in loadSelection. On false, `out` may hold partial results — every
    // caller discards it in that case.
    static bool parseInto(const std::string& path, std::vector<BandEntry>& out,
                          PlanInfo& info, std::string& error);

    std::vector<BandEntry> entries_;
    std::string name_;
};

}  // namespace cascade::core
