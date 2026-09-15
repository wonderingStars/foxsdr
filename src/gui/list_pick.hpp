// list_pick.hpp - choosing a row from a list whose own handler REBUILDS that
// list.
//
// WHY THIS EXISTS, AND WHAT IT COST TO LEARN. The REGION picker in the Display
// section was written the way every ImGui combo in every sample is written:
//
//     for (const PlanInfo& p : bandPlanChoices_) {
//         if (ImGui::Selectable(p.name.c_str(), selected) && !selected) {
//             bandPlanSelection_ = p.id;
//             loadBandPlan();          // <- assigns bandPlanChoices_
//         }
//     }
//
// loadBandPlan() does `bandPlanChoices_ = BandPlan::available(dir)`, which
// frees the vector's storage. The range-for is still holding an iterator into
// that storage, so the NEXT p is a reference to freed memory and the next
// Selectable hashes a dangling name pointer. That is the "crash cascade.exe @
// ImHashStr" report: ImHashStr <- ImGuiWindow::GetID <- ImGui::Selectable <-
// drawDisplaySection. Three of them arrived, from two different releases, and
// the loop reads as completely ordinary code - which is the point of moving
// the rule out of the call site and into something that can be held to it.
//
// THE RULE: RECORD THE PICK, DO NOT APPLY IT. pickFromList walks the list by
// index and STOPS at the first row that reports a click. The handler runs
// after the loop - after EndCombo, with nothing iterating - so whatever it
// does to the container it can no longer invalidate anything. The row callback
// is given the item and may draw it; it must not mutate the list, and because
// the loop ends the moment it answers true, it cannot observe one that has
// been mutated either.
//
// THE AUDIT THAT FOLLOWED, so the next reader does not have to repeat it.
// Every BeginCombo / Selectable loop in app_window.cpp was traced to what its
// click handler actually calls, and the REGION picker was the only one whose
// handler rebuilt the list it was iterating. The others are clear for reasons
// worth writing down, because two of them are clear only by accident:
//
//   Bandwidth, GPS baud, track sort - fixed static arrays. Nothing can
//   reallocate them.
//   Audio Device, Source, Rate - indexed loops whose bound re-reads size()
//   every turn, so a shortened list ends the loop instead of running off it.
//   openAudioDevice(), selectSource() and followInputRate() were each read
//   through: none of them touches the list its own combo walks
//   (devices_ is rebuilt only by the once-a-second audio watchdog,
//   deviceRateLabels_ and deviceAntennas_ only by adoptDeviceMirrors on the
//   open path), and selectSource bounds-checks its index anyway.
//   Antenna, GPS ports - range-fors, and safe only because setAntenna() and
//   the port handler happen not to rebuild deviceAntennas_ / gpsPorts_. That
//   is the accident; the day one of them does, they need this.
//   Bookmarks - already defers its deletion past the loop, with a comment
//   saying why. Same rule, arrived at independently.
//
// Hence a general helper rather than a band-plan function.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>

namespace cascade::gui {

// "Nothing was picked this frame", which is what almost every frame answers.
inline constexpr std::size_t kNoPick = static_cast<std::size_t>(-1);

// Draws `items` one row at a time through `row(item, index) -> bool` and
// returns the index of the first row that answered true, or kNoPick.
//
// `items` needs only size() and operator[](size_t) - indices, not iterators,
// because an index survives a reallocation and an iterator does not. Nothing
// here touches the container after `row` has answered true.
template <class Container, class RowFn>
std::size_t pickFromList(const Container& items, RowFn&& row) {
    const std::size_t n = items.size();
    for (std::size_t i = 0; i < n; ++i) {
        // The read and the call, in that order, once per row. If row() decides
        // this is the pick we leave immediately: the caller's handler is what
        // may rebuild the list, and it runs where nothing is iterating.
        if (row(items[i], i)) { return i; }
    }
    return kNoPick;
}

}  // namespace cascade::gui
