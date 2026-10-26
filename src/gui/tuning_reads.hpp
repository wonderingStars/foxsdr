// tuning_reads.hpp - the pure parts of what the tuning surface reads from the
// engine's table (window port, group 6, docs/engine-stage4.md 6.5; API 0.3 section
// 8.5): the top bar, the frequency readout, the keys and the centre panels. No
// ImGui, no engine object: arithmetic over rows and numbers, so a test can hold
// each rule against the table it restates.
//
//   - the lamps: when the MUTE lamp is lit, and what the volume meter treats as
//     muted (the pipeline's own audioMuted), from the state's flags and
//     audioMutedBy
//   - the bookmark rows as the window keeps them (BOOKMARKS' entries: the markers' and, since
//     group 7, the Bookmarks section's) and the
//     binary search for the ones in view - core::FreqManager::range's rule
//   - what a typed frequency has to be before the table will take it
//
// What the surface sends is the engine's own limits applied for the frame (the VFO
// reach, the bandwidth bounds): those are the engine's functions, called as the
// engine calls them (engine/soundcard_panel.hpp, engine/receiver_tables.hpp), not
// restated here.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "foxsdr_api.h"

namespace cascade::gui {

// THE MUTE LAMP on the deck: the user's own mute (FOXAPI_RX_MUTED) or a plugin
// muting the audio (audioMutedBy names it). Both lit one lamp: a lamp that only
// knew about plugins would sit dark over silent speakers.
inline bool masterMuteLamp(const FoxReceiverState& st) {
    return (st.flags & FOXAPI_RX_MUTED) != 0u || st.audioMutedBy[0] != '\0';
}

// WHAT THE VOLUME METER TAKES AS MUTED: the pipeline's hard mute, which is the lamp's
// two causes and a third - a keyed transmitter that is not monitored (the receiver on
// the frequency it is transmitting on is a howl). Engine::updateAudioMute ORs the same
// three, and sets Pipeline::audioMuted from them.
inline bool audioHardMuted(const FoxReceiverState& st) {
    return masterMuteLamp(st) ||
           ((st.flags & FOXAPI_RX_TX_KEYED) != 0u && (st.txFlags & FOXAPI_TX_MONITOR) == 0u);
}

// THE MOST A TYPED FREQUENCY MAY BE, and the least it may not be: FOXAPI_OP_SET_FREQUENCY
// is refused outside (0, 6 GHz] (ApiHostImpl::validate, OUT_OF_RANGE). The readout checks
// the same range before it sends, and says so; tests/test_tuning_surface.cpp holds this
// against what the table answers at the edges.
inline constexpr double kTunableMaxHz = 6.0e9;
inline bool typedFrequencyTunable(double hz) { return hz > 0.0 && hz <= kTunableMaxHz; }  // NaN: no

// THE DECODERS THE WATERFALL'S CAPTION NAMES: how many DECODERS entries are FED (a
// decoder is fed only with the receiver running, so a stopped receiver counts none), and
// the first of them. "ADS-B - DECODING" for one, "ADS-B +1 - DECODING" for two.
struct FedDecoders {
    int count = 0;
    std::string first;
    void clear() {
        count = 0;
        first.clear();
    }
    // One DECODERS entry, as read: its item flags and its name.
    void add(std::uint32_t itemFlags, const char* name) {
        if ((itemFlags & FOXAPI_ITEM_FED) == 0u) { return; }
        if (count == 0) { first = name; }
        ++count;
    }
};

// One bookmark as the window keeps it: BOOKMARKS' entry, copied when the store's version moves
// (fact.bookmarks.version). The spectrum's markers read where, whether starred and the name
// (cut by the table at the list item's 95 bytes); since window port group 7 the Bookmarks
// section reads the rest from the same copy - the id its commands name, the group (the item's
// detail, cut at 159 bytes), the mode code and the bandwidth - so there is ONE copy of a list
// that can hold tens of thousands, made once per change of the store.
struct BookmarkRow {
    double hz = 0.0;
    bool favourite = false;
    std::string name;
    std::uint64_t id = 0;
    std::string group;
    std::uint32_t mode = 0;  // FOXAPI_DEMOD_*; 0 for a mode no code names
    double bandwidthHz = 0.0;
    bool ticked = false;     // FOXAPI_ITEM_TICKED (stub campaign B6): the scanner's list mode scans it, the Airband monitor plays it (AM or NFM)
};
using MarkerRow = BookmarkRow;  // (the markers' name for it, group 6)

// The rows [first, last) with loHz <= hz <= hiHz, of rows sorted by hz ascending (the order
// BOOKMARKS is in, and the model's): core::FreqManager::range's rule - lower_bound on
// "hz < lo", upper_bound on "hi < hz" - so a list of 33 000 costs what the dozen in view cost.
inline std::pair<std::size_t, std::size_t> markerRange(const std::vector<BookmarkRow>& rows, double loHz,
                                                       double hiHz) {
    const auto lo = std::lower_bound(rows.begin(), rows.end(), loHz,
                                     [](const BookmarkRow& x, double f) { return x.hz < f; });
    const auto hi = std::upper_bound(lo, rows.end(), hiHz,
                                     [](double f, const BookmarkRow& x) { return f < x.hz; });
    return {static_cast<std::size_t>(lo - rows.begin()), static_cast<std::size_t>(hi - rows.begin())};
}

}  // namespace cascade::gui
