// plugin_calls.hpp - THE ONE PLACE the host calls into a plugin's non-signal entry points (plugin containment P1, docs/engine-stage4.md 6.28).
//
// Every function here is a call through the guard of core/plugin_fault.hpp: a fault inside the plugin is absorbed and the plugin switched off
// for the session, a throw across the C boundary is the same, a call past its budget is named in a hang report, and a call to a plugin that
// is already switched off is refused without being made. What the caller sees in each case is the answer the ABI already has for "this
// instance is gone", so no call site grows a new branch:
//
//   create            nullptr          (the ABI's "failed to create")
//   destroy           nothing          (an instance of a plugin that faulted is LEAKED, deliberately, and its module is never unmapped -
//                                       a faulted plugin's threads may still be running in it: PluginHost::unloadAll keeps it mapped)
//   poll_*            -1               (the ABI's "failed permanently": the caller already stops polling an instance that says so)
//   get_tile/get_info CASCADE_*_MISSING (-1: "will never exist, do not ask again")
//   columns           0
//   release_*, press, attach, preset get   nothing / 0
//
// THE HOT SIGNAL PATH IS NOT HERE: process(), the audio-out and transmit-audio pull() and active(), retune() are called directly by the
// runner, unguarded, exactly as the ABI and the vendor-call guard's own reasoning require (plugin_fault.hpp, rule 1).
//
// tests/test_plugin_containment.cpp scans src/ for a raw call of any entry point listed above outside this file, so a call site that
// bypasses the guard is a test failure rather than a silent return to the unguarded host.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_PLUGIN_CALLS_HPP
#define CASCADE_CORE_PLUGIN_CALLS_HPP

#include <cstddef>
#include <cstdint>

#include "core/plugin_abi.h"
#include "core/plugin_fault.hpp"

namespace cascade::core::calls {

using fault::Entry;
using fault::Outcome;
using fault::Slot;

// THE ANSWERS THAT ARE MORE THAN THE CALL ALLOWED (plugin containment P2, Misuse::BadReturn): a poll that says it filled more than it was given
// room for, a panel with more columns than the ABI has. The callers have always clamped these; the wrapper now also counts the plugin that said
// it, on its slot, and still hands the answer back unchanged (the clamp stays where it was, the count is new).
inline void checkCount(Slot* s, std::int32_t n, std::uint32_t cap) noexcept {
    if (n > 0 && static_cast<std::uint32_t>(n) > cap) { fault::noteMisuse(s, fault::Misuse::BadReturn); }
}

// ---- the life of an instance -------------------------------------------------------------------------------------------------------

// Any capability's create(): `Api` is the table (CascadeDecoderApi, CascadeTrackSourceApi...) and `args` are its create()'s arguments.
template <class Api, class... Args>
void* create(Slot* s, const Api* api, Args... args) noexcept {
    void* handle = nullptr;
    const Outcome o = fault::call(s, Entry::Create, [&] { handle = api->create(args...); });
    return o == Outcome::Ran ? handle : nullptr;
}

// Any capability's destroy(). False when it was not called (the plugin is switched off) or did not complete.
template <class Api>
bool destroy(Slot* s, const Api* api, void* handle) noexcept {
    return fault::call(s, Entry::Destroy, [&] { api->destroy(handle); }) == Outcome::Ran;
}

// ---- polls -------------------------------------------------------------------------------------------------------------------------

template <class Api>
std::int32_t pollText(Slot* s, const Api* api, void* handle, char* buf, std::size_t cap) noexcept {
    std::int32_t r = -1;
    const Outcome o = fault::call(s, Entry::PollText, [&] { r = api->poll_text(handle, buf, cap); });
    if (o == Outcome::Ran && r > 0 && static_cast<std::size_t>(r) > cap) { fault::noteMisuse(s, fault::Misuse::BadReturn); }
    return o == Outcome::Ran ? r : -1;
}

inline std::int32_t pollImage(Slot* s, const CascadeImageDecoderApi* api, void* handle, CascadeImage* out) noexcept {
    std::int32_t r = -1;
    const Outcome o = fault::call(s, Entry::PollImage, [&] { r = api->poll_image(handle, out); });
    return o == Outcome::Ran ? r : -1;
}

inline void releaseImage(Slot* s, const CascadeImageDecoderApi* api, void* handle, const CascadeImage* img) noexcept {
    (void)fault::call(s, Entry::ReleaseImage, [&] { api->release_image(handle, img); });
}

inline std::int32_t pollTracks(Slot* s, const CascadeTrackSourceApi* api, void* handle, CascadeTrack* out, std::uint32_t cap) noexcept {
    std::int32_t r = -1;
    const Outcome o = fault::call(s, Entry::PollTracks, [&] { r = api->poll_tracks(handle, out, cap); });
    if (o == Outcome::Ran) { checkCount(s, r, cap); }
    return o == Outcome::Ran ? r : -1;
}

inline std::int32_t pollPaths(Slot* s, const CascadeTrackSourceApi* api, void* handle, CascadePath* out, std::uint32_t cap) noexcept {
    std::int32_t r = -1;
    const Outcome o = fault::call(s, Entry::PollPaths, [&] { r = api->poll_paths(handle, out, cap); });
    if (o == Outcome::Ran) { checkCount(s, r, cap); }
    return o == Outcome::Ran ? r : -1;
}

inline std::int32_t pollState(Slot* s, const CascadeInstrumentApi* api, void* handle, CascadeInstrumentState* out) noexcept {
    std::int32_t r = -1;
    const Outcome o = fault::call(s, Entry::PollState, [&] { r = api->poll_state(handle, out); });
    if (o == Outcome::Ran && r > 1) { fault::noteMisuse(s, fault::Misuse::BadReturn); }   // the ABI answers 1, 0 or negative
    return o == Outcome::Ran ? r : -1;
}

// A panel's and an instrument's memory feed share the column and row calls: `Api` is CascadePanelApi or CascadeInstrumentApi.
template <class Api>
std::uint32_t columns(Slot* s, const Api* api, void* handle, char (*headings)[CASCADE_PANEL_CELL_CHARS]) noexcept {
    std::uint32_t r = 0;
    const Outcome o = fault::call(s, Entry::Columns, [&] { r = api->columns(handle, headings); });
    if (o == Outcome::Ran && r > CASCADE_PANEL_MAX_COLUMNS) { fault::noteMisuse(s, fault::Misuse::BadReturn); }
    return o == Outcome::Ran ? r : 0u;
}

template <class Api>
std::int32_t pollRows(Slot* s, const Api* api, void* handle, CascadePanelRow* out, std::uint32_t cap) noexcept {
    std::int32_t r = -1;
    const Outcome o = fault::call(s, Entry::PollRows, [&] { r = api->poll_rows(handle, out, cap); });
    if (o == Outcome::Ran) { checkCount(s, r, cap); }
    return o == Outcome::Ran ? r : -1;
}

// ---- the basemap and track-info clients --------------------------------------------------------------------------------------------

inline std::int32_t getTile(Slot* s, const CascadeBasemapApi* api, void* handle, std::uint32_t z, std::uint32_t x, std::uint32_t y,
                            CascadeTile* out) noexcept {
    std::int32_t r = CASCADE_TILE_MISSING;
    const Outcome o = fault::call(s, Entry::GetTile, [&] { r = api->get_tile(handle, z, x, y, out); });
    return o == Outcome::Ran ? r : CASCADE_TILE_MISSING;
}

inline void releaseTile(Slot* s, const CascadeBasemapApi* api, void* handle, const CascadeTile* tile) noexcept {
    (void)fault::call(s, Entry::ReleaseTile, [&] { api->release_tile(handle, tile); });
}

inline std::int32_t getInfo(Slot* s, const CascadeTrackInfoApi* api, void* handle, const char* id, std::uint32_t kind,
                            CascadeTrackInfo* out) noexcept {
    std::int32_t r = CASCADE_INFO_MISSING;
    const Outcome o = fault::call(s, Entry::GetInfo, [&] { r = api->get_info(handle, id, kind, out); });
    return o == Outcome::Ran ? r : CASCADE_INFO_MISSING;
}

inline void releaseInfo(Slot* s, const CascadeTrackInfoApi* api, void* handle, const CascadeTrackInfo* info) noexcept {
    (void)fault::call(s, Entry::ReleaseInfo, [&] { api->release_info(handle, info); });
}

// ---- the small ones ----------------------------------------------------------------------------------------------------------------

inline void attach(Slot* s, const CascadeHostClientApi* api, const CascadeHostApi* host) noexcept {
    (void)fault::call(s, Entry::Attach, [&] { api->attach(host); });
}

inline void press(Slot* s, const CascadeImageInputApi* api, std::uint32_t x, std::uint32_t y) noexcept {
    (void)fault::call(s, Entry::Press, [&] { api->press(x, y); });
}

// The preset table's count() and get(): 0 and "not fetched" (0) when the plugin faulted or is switched off.
inline std::uint32_t presetCount(Slot* s, const CascadePresetApi* api) noexcept {
    std::uint32_t r = 0;
    const Outcome o = fault::call(s, Entry::PresetCount, [&] { r = api->count(); });
    return o == Outcome::Ran ? r : 0u;
}

inline std::int32_t presetGet(Slot* s, const CascadePresetApi* api, std::uint32_t index, CascadePreset* out) noexcept {
    std::int32_t r = 0;
    const Outcome o = fault::call(s, Entry::PresetGet, [&] { r = api->get(index, out); });
    return o == Outcome::Ran ? r : 0;
}

}  // namespace cascade::core::calls

#endif  // CASCADE_CORE_PLUGIN_CALLS_HPP
