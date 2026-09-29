// patch_presets.hpp - the patch page's named presets: whole patches the user
// saved under a name, to load back later.
//
// WHAT A PRESET IS. A name and a text, and the text is EXACTLY what
// core/patch_io.hpp's serialise() writes - the same document AppConfig::patch
// holds. Nothing here understands a graph; a preset is loaded by handing its
// text to the one place the application replaces the whole patch
// (AppWindow::replacePatch), which parses it through patch_io like the
// start-up restore does.
//
// WHERE THEY LIVE. In config.json, as the "patchPresets" array, loaded and
// saved with the rest of AppConfig - so a preset inherits the config's
// crash-safe write and its off-thread save, as the patch itself does. They
// never leave the machine (PRIVACY.md).
//
// THE RULES, each enforced here and nowhere else:
//   - a name is cleaned (control characters and broken UTF-8 out, trimmed),
//     cut to kMaxPatchPresetNameBytes on a character boundary, and must not
//     be empty or the reserved slot's name;
//   - names match case-insensitively, so "Airband" and "AIRBAND" are one
//     preset, and saving over an existing one happens only when the caller
//     says the user confirmed it;
//   - at most kMaxPatchPresets, each text at most kMaxPatchPresetTextBytes;
//   - a text that patch_io::parse() does not recognise is refused on save and
//     dropped on load.
//
// THE PREVIOUS-PATCH SLOT. Loading a preset replaces the patch on the canvas,
// and one mis-click must not lose an evening's wiring. The patch being
// replaced is kept in a single reserved slot beside the list - not in it, and
// not counted against kMaxPatchPresets - which the next load overwrites. It is
// saved with the presets, as "patchPresetPrevious", so a restart does not
// lose it either.
//
// This file is pure: no ImGui, no AppWindow. Its refusals are an enum; the
// sentence that explains each one is the page's, in words it translates.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace cascade::core {

struct PatchPreset {
    std::string name;
    // What core::patch::serialise() wrote. Opaque here but for the parse
    // check at load and save.
    std::string text;

    bool operator==(const PatchPreset&) const = default;
};

// How many named presets the list holds. A list that long wants a search
// box, and a list longer than that is a config file someone else wrote.
inline constexpr std::size_t kMaxPatchPresets = 100;
// A name's length in BYTES. Sixty-four is a long title in any alphabet and
// still fits the list's row.
inline constexpr std::size_t kMaxPatchPresetNameBytes = 64;
// A preset's text in bytes. A five-radio patch with every part wired is a few
// kilobytes; 256 KB is room for any patch the canvas can make and a bound on
// what one hand-edited entry can cost every config save.
inline constexpr std::size_t kMaxPatchPresetTextBytes = 256u * 1024u;
// The reserved slot's name - its identity, not its label (the page shows a
// translation). A user's preset may not take it, whatever its case.
inline constexpr const char* kPreviousPatchSlotName = "(previous patch)";

// A name as it would be stored: control characters (C0, DEL and C1) and any
// byte that does not form a whole UTF-8 character removed, leading and
// trailing spaces trimmed, then cut to kMaxPatchPresetNameBytes without
// splitting a character (and trimmed again, so the cut leaves no trailing
// space). Empty when nothing usable was typed.
std::string cleanPatchPresetName(std::string_view in);

// Whether two names name the same preset: equal once each is cleaned and
// case-folded (ASCII and the alphabets core/utf8_text.hpp capitalises).
bool patchPresetNamesMatch(std::string_view a, std::string_view b);

// Whether a text is a patch document patch_io::parse() reads (its header).
bool patchPresetTextParses(const std::string& text);

enum class PatchPresetStatus {
    Saved,         // a new preset was added
    Overwritten,   // an existing one was replaced (the caller said so)
    Renamed,
    Deleted,
    Exists,        // the name is taken; nothing changed
    EmptyName,     // nothing usable was typed
    ReservedName,  // the previous-patch slot's name
    ListFull,      // kMaxPatchPresets reached
    TooLarge,      // the text is over kMaxPatchPresetTextBytes
    NotAPatch,     // the text does not parse
    NotFound,      // no preset by that name
};

class PatchPresetStore {
public:
    // Adopts a list and a previous-slot text as the config loaded them. The
    // config loader has already applied sanitisePatchPresets and
    // sanitisePatchPresetPrevious; this only sorts.
    void assign(std::vector<PatchPreset> list, std::string previous);

    // Sorted by name, case-insensitively.
    const std::vector<PatchPreset>& list() const { return list_; }
    // The patch the last load replaced; empty when there is none.
    const std::string& previous() const { return previous_; }

    // The index of the preset `name` matches, or -1.
    int find(std::string_view name) const;

    // Saves `text` under `name`. A name already taken is overwritten only
    // when `overwrite` is true (the user confirmed); otherwise Exists and the
    // list is unchanged. Every refusal leaves the list unchanged.
    PatchPresetStatus save(std::string_view name, const std::string& text, bool overwrite);
    // Renames `from` to `to`. A change of case alone is a rename; a name
    // another preset has is Exists, and never an overwrite.
    PatchPresetStatus rename(std::string_view from, std::string_view to);
    PatchPresetStatus remove(std::string_view name);

    // Keeps `text` in the previous-patch slot. False, and the slot unchanged,
    // when the text is over kMaxPatchPresetTextBytes or does not parse.
    bool keepPrevious(const std::string& text);

private:
    void sort();
    std::vector<PatchPreset> list_;
    std::string previous_;
};

// What a config file is allowed to put in the list, entry by entry, so one bad
// entry is dropped and the rest survive. Each drop is one diagLogf line:
//   - a name that cleans to empty, or is the reserved slot's: dropped;
//   - a text over kMaxPatchPresetTextBytes, or one parse() refuses: dropped;
//   - a name matching an earlier entry's: dropped (first wins);
//   - entries past kMaxPatchPresets: dropped.
// Names are stored cleaned. `positions`, when given, is where each entry sat
// in the file's array, so a log line names the entry a reader can find there
// even after the loader has dropped malformed ones before it; without it an
// entry is numbered by its place in `in`.
std::vector<PatchPreset> sanitisePatchPresets(std::vector<PatchPreset> in,
                                              const std::vector<std::size_t>& positions = {});

// The previous-patch slot under the same text rules; empty (and logged) when
// it breaks one.
std::string sanitisePatchPresetPrevious(std::string text);

}  // namespace cascade::core
