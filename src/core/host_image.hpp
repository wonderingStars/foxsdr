// host_image.hpp - the host's own copy of an image a plugin produced.
//
// Its own header because two subsystems need it and neither should have to
// include the other: PluginRunner PRODUCES these (it owns the image decoder
// instances and drives them from the DSP thread) and the GUI plus the BMP
// writer CONSUME them. It lived in plugin_ui.hpp while image decoders were
// created there and never fed; moving the decoders to the runner - where the
// samples are - left this struct with two unrelated users.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#ifndef CASCADE_CORE_HOST_IMAGE_HPP
#define CASCADE_CORE_HOST_IMAGE_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "core/plugin_abi.h"

namespace cascade::core {

// The newest image a plugin has produced, COPIED out of it.
//
// The ABI lends the pixels only until release_image, and the host wants to
// keep showing the picture across frames, upload it to a texture on its own
// schedule and let the user save it minutes later. So it is copied once on
// arrival and released immediately - the borrow is held for microseconds
// rather than for as long as the window is open.
struct HostImage {
    std::string plugin;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t format = 0;   // CASCADE_IMAGE_GRAY8 / RGB24
    bool complete = false;      // false while still being received
    std::uint64_t sequence = 0;
    std::vector<std::uint8_t> pixels;  // tightly packed, no row padding
    std::uint64_t revision = 0;        // bumped whenever pixels change
};

}  // namespace cascade::core

#endif  // CASCADE_CORE_HOST_IMAGE_HPP
