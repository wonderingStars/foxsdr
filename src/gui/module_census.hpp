// module_census.hpp - WHY A SURFACE IS EMPTY, counted rather than guessed at.
//
// THE FAULT THIS FILE EXISTS TO PREVENT, which this product has now shipped
// and repaired five times in five different places. A surface asks one of the
// live objects for its list - PluginUi::trackPluginNames(), PluginRunner::
// status() - finds it empty, and prints "nothing is installed". But both of
// those lists are built by a loop that SKIPS a module which did not load and
// SKIPS a module the user stopped, before it writes anything. So an empty list
// is equally true of three machines:
//
//   - one with nothing in the plugin folder;
//   - one whose module the host FOUND and REFUSED;
//   - one whose module loaded perfectly and the user STOPPED.
//
// In the last two the product told a user who already owns the module to go
// and buy it, while the Fitted modules window one key away lettered that same
// file REFUSED or STOPPED BY YOU. ABSENT IS NOT ZERO AND ABSENT IS NOT KNOWN:
// the emptiness of a list is not evidence about the disk.
//
// WHAT IS SHARED HERE AND WHAT IS NOT. The census is shared and general - it
// takes the capability word the surface cares about, so a decoder surface and
// a track-source surface count the same way and cannot come to disagree about
// one module. The NOTES are separate, one per role, because their prose is
// about different things and a single sentence parameterised into meaning both
// would be readable as neither.
//
// THE VOCABULARY IS THE FITTED MODULES WINDOW'S - STOPPED BY YOU and REFUSED
// (gui/plugins_view.hpp) - derived from the same two record fields that window
// derives them from, rather than a second set of words meaning the same thing.
//
// PURE, AND DELIBERATELY OUT OF app_window.cpp. These functions decide what a
// user is TOLD, and while they sat in an anonymous namespace inside a 12,000
// line translation unit no test could reach a single one of them. There is no
// ImGui here and none is needed; tests/test_module_census.cpp exercises every
// state.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_MODULE_CENSUS_HPP
#define CASCADE_GUI_MODULE_CENSUS_HPP

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/plugin_abi.h"
#include "core/plugin_host.hpp"

namespace cascade::gui {

// EVERY CAPABILITY BIT THAT MAKES A MODULE A DECODER. The runner drives three
// tables - text from audio, text from raw I/Q, and pictures - and a module
// carrying any one of them is a decoder for the purposes of every sentence in
// this product. Held here rather than written out at each site so a fourth
// decoder table cannot be added to the ABI and missed by one surface.
inline constexpr std::uint32_t kDecoderCaps =
    CASCADE_CAP_DECODER | CASCADE_CAP_IQ_DECODER | CASCADE_CAP_IMAGE_DECODER;

// The four states a module can be in with respect to ONE capability, counted
// over the host's records. Nothing here is a guess: each field names the
// predicate it counts.
struct ModuleCensus {
    // Loaded, not stopped, and carrying a table for the capability asked
    // about: precisely what the live object would have instantiated. Non-zero
    // while the surface's own list is empty means the list has not been built
    // since this module arrived, which is its own state and not "nothing is
    // installed".
    int live = 0;
    // Loaded, and in the stop set. Fitted, still mapped, given nothing.
    int stopped = 0;
    // Not loaded, and its descriptor declared the capability. The duplicate
    // resolver's outcome: it nulls the borrowed tables but keeps the
    // capability word it read (plugin_host.cpp:829-843), so this file's
    // identity is known and the host recorded why it is not running.
    int refused = 0;
    // Not loaded, and refused before a descriptor was ever accepted - every
    // such path returns at plugin_host.cpp:208-239, ahead of the copies at
    // :245-249 - so its capability word is empty and NOTHING HERE CAN SAY
    // WHAT IT WAS. Counted rather than guessed at: "no decoder is fitted" is a
    // weaker claim when a file in the folder was never opened far enough to
    // know what it held.
    int unread = 0;
    // One name each, for the surfaces with room to say which module they mean.
    // The display name where the host read one, the file name otherwise; first
    // in the host's scan order.
    std::string stoppedName;
    std::string refusedName;
};

// Takes a module FILE NAME - core::pluginKey - which is the identity the stop
// set, the tune grant and the mute override are all keyed on. Never the display
// name, which the module itself chooses and two modules may share.
using ModuleStoppedFn = std::function<bool(const std::string& moduleFile)>;

// Does this LOADED record carry a table behind any bit in `capMask`?
//
// THE TABLE, NOT THE WORD, on a record that loaded, because the table is what
// the live objects test before instantiating anything. validatePluginDesc has
// already proven a declared bit has a usable table behind it, so the two agree
// here - and asking the question the instantiation asks keeps them agreeing.
// False for a record that did not load: every borrowed pointer is nulled on the
// way out.
bool moduleProvides(const cascade::core::LoadedPlugin& p, std::uint32_t capMask);

// Count the host's records against one capability word.
ModuleCensus censusModules(const std::vector<cascade::core::LoadedPlugin>& plugins,
                           std::uint32_t capMask, const ModuleStoppedFn& isStopped);

// --- the notes ---------------------------------------------------------------
//
// Each returns the sentence a surface prints when it is empty BECAUSE the
// application is holding no module of that kind. Every branch says WHICH of the
// four states the machine is in and points at the thing that would change it,
// and only the last one mentions installing anything.
//
// THE ORDER IS STOPPED, REFUSED, LIVE, ABSENT in both, and it is the order
// gui::fittedState uses: the user's own choice is not a fault, and it is the
// one state where the module they need is already on the disk.

// `subject` completes "publishes ..." and names what the calling surface would
// have drawn; `installRemedy` is the one clause that cannot be shared, because
// where the plugin store is from here depends on which surface is asking.
//
// WHAT A TRACK SOURCE PUBLISHES IS NOT KNOWN UNTIL IT PUBLISHES IT. The ABI
// gives a module ONE capability bit for tracks and puts the kind on each TRACK,
// so no sentence here may say a particular module publishes a particular
// subject - only that it is a track source and that nothing is publishing that
// subject, which are the two facts there are.
std::string trackSourceAbsenceNote(const ModuleCensus& c, const char* subject,
                                   const char* installRemedy);

// The decoder equivalent, in the same shape and with the same ordering.
//
// AND UNDER THE SAME PROHIBITION, for the same ABI reason. A module declares
// that it decodes; NOTHING in the descriptor says WHAT. There is no mode, no
// band and no protocol in a capability word, so no sentence here may name one.
// "Your ADS-B decoder is stopped" would be exactly the invention this whole
// family of repairs removes - the module is a decoder, and that is the claim
// the product can keep.
std::string decoderAbsenceNote(const ModuleCensus& c);

}  // namespace cascade::gui

#endif  // CASCADE_GUI_MODULE_CENSUS_HPP
