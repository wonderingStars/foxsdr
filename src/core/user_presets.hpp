// The user's OWN presets for a decoder plugin (0.99.4).
//
// WHY THESE EXIST. A plugin's presets are the plugin's to publish, and some
// plugins deliberately publish none for a region: the POCSAG decoder offers
// DAPNET and four US 900 MHz channels and NO UK commercial paging frequency,
// because UK paging is scattered across a dozen bands and a button that tunes
// somewhere plausible and wrong is worse than no button (its own comment has
// the argument). That left a UK listener with no way to keep 153.xxx MHz: the
// host auto-applies a decoder's first preset when its window is opened
// (gui/tune_control.hpp, autoPresetIndexOnStart), so opening POCSAG while
// tuned to 153.050 MHz moved the radio to DAPNET's 439.9875 MHz - reported
// from the field as "I have to change the frequency manually every time".
//
// A user preset is a frequency the USER saved against one plugin, from the
// receiver's tuning at the moment they pressed "Save". It is offered beside
// the plugin's own presets everywhere those are offered, and it comes FIRST
// when the host decides what to auto-apply, so a saved 153.050 is what
// opening POCSAG tunes to from then on.
//
// This file is pure: no ImGui, no plugin ABI, no receiver. The GUI's half
// (turning one of these into a CascadePreset, the index space a preset-bar
// key records) is in gui/tune_control.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cascade::core {

struct UserPreset {
    // Which plugin this belongs to: userPresetKey() of its module file name,
    // e.g. "pocsag-decoder" - NEVER the file name itself, which carries the
    // version ("pocsag-decoder-1.0.2-abi3-win-x64.dll") and would orphan every
    // saved preset the moment the plugin updated. Not the display name
    // either: that is the plugin's own to choose.
    std::string plugin;
    // What the key says. Bounded and single-line (see sanitiseUserPresets).
    std::string label;
    // Where to listen: the ABSOLUTE tuned frequency (device centre plus VFO
    // offset) at the moment the preset was saved.
    double frequencyHz = 0.0;
    // CASCADE_DEMOD_* (0 = leave the receiver's mode alone, 1..8 a mode). Kept
    // as the ABI's number rather than a mode name so a preset replays through
    // exactly the path a plugin's own preset takes.
    std::uint32_t demodMode = 0;
    // Channel bandwidth in Hz, or 0 to leave the receiver's alone.
    double bandwidthHz = 0.0;

    bool operator==(const UserPreset&) const = default;
};

// Per plugin. A handful of channels is what a preset row is for; a list long
// enough to scroll belongs in Bookmarks, which already exists.
inline constexpr std::size_t kMaxUserPresetsPerPlugin = 8;
// Across every plugin - the bound a corrupt or hand-edited config cannot
// exceed, sized so every installed decoder can hold its full allowance.
inline constexpr std::size_t kMaxUserPresets = 256;
// Bytes, excluding the terminator: CASCADE_PRESET_LABEL_CHARS (48) minus the
// NUL, so a user label always fits the same buffer a plugin's does.
inline constexpr std::size_t kMaxUserPresetLabelBytes = 47;
// Longest plugin key accepted from a config file.
inline constexpr std::size_t kMaxUserPresetKeyBytes = 128;
// Two presets for the same plugin this close together are the same channel.
// Well inside the 5 kHz "already tuned" tolerance the preset bars use, so two
// keys that would light together can never both exist.
inline constexpr double kUserPresetSameChannelHz = 100.0;
// No receiver this application drives reaches past 100 GHz, and a number
// above it is a corrupt file, not a channel.
inline constexpr double kMaxUserPresetHz = 100.0e9;

// The version-stable identity of a plugin module: its file name with the
// extension removed and, when present, the "-<major>.<minor>.<patch>..."
// suffix the catalogue's naming scheme appends (see foxsdr-plugins
// SCHEMA.md: <id>-<version>-abi<N>-<os>-<arch>.<ext>).
//   "pocsag-decoder-1.0.2-abi3-win-x64.dll" -> "pocsag-decoder"
//   "my-decoder.dll"                         -> "my-decoder"
//   ""                                       -> ""
// A path is accepted too; only its final component is used.
std::string userPresetKey(const std::string& moduleFileName);

// A frequency that can be tuned: finite, positive, and below kMaxUserPresetHz.
// Written as a positive test so NaN is refused.
bool userPresetFrequencyValid(double hz);

// The label a new preset gets when the user does not type one:
// "153.0500 MHz" - the same four decimal places the preset tooltips use.
std::string defaultUserPresetLabel(double frequencyHz);

// What a config file is allowed to put in the list. Every rule is applied
// per entry, so one bad entry is dropped and the good ones survive:
//   - an empty or over-long plugin key, or an unusable frequency: dropped;
//   - a label: control characters removed, cut to kMaxUserPresetLabelBytes on
//     a UTF-8 boundary, and replaced by defaultUserPresetLabel if empty;
//   - a demodMode outside 0..8: 0 (leave the mode alone);
//   - a bandwidth that is not finite or not in 0..1 GHz: 0;
//   - a second preset for the same plugin within kUserPresetSameChannelHz of
//     an earlier one: dropped (first wins);
//   - more than kMaxUserPresetsPerPlugin for one plugin, or more than
//     kMaxUserPresets in all: the excess dropped.
std::vector<UserPreset> sanitiseUserPresets(const std::vector<UserPreset>& in);

// Why an add did or did not happen - the Save key reports it in words.
enum class UserPresetAdd {
    Added,
    AlreadySaved,   // this plugin already has a preset on this channel
    PluginFull,     // kMaxUserPresetsPerPlugin reached for this plugin
    ListFull,       // kMaxUserPresets reached overall
    Invalid,        // empty key or unusable frequency
};

// Appends `p` (after the same cleaning sanitiseUserPresets applies) unless
// one of the refusals above applies; `list` is unchanged on any refusal.
UserPresetAdd addUserPreset(std::vector<UserPreset>& list, const UserPreset& p);

// Removes the `ordinal`-th preset belonging to `plugin` (0-based, in list
// order - the order userPresetsFor returns). Answers whether one was removed.
bool removeUserPreset(std::vector<UserPreset>& list, const std::string& plugin,
                      std::size_t ordinal);

// The presets belonging to `plugin`, in saved order.
std::vector<UserPreset> userPresetsFor(const std::vector<UserPreset>& list,
                                       const std::string& plugin);

}  // namespace cascade::core
