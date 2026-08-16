// image_write.hpp - write a decoded plugin image to a file.
//
// Lives in core rather than gui, and is a free function rather than a private
// method, for one reason: it needs to be testable. As a member of AppWindow it
// could only be exercised by clicking a button, so it shipped unverified - and
// the three things BMP gets wrong (bottom-up rows, B/G/R order, four-byte row
// padding) all produce a file that OPENS and looks wrong rather than failing.
// Those are precisely the bugs a test catches and a glance does not.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#ifndef CASCADE_CORE_IMAGE_WRITE_HPP
#define CASCADE_CORE_IMAGE_WRITE_HPP

#include <string>

#include "core/host_image.hpp"

namespace cascade::core {

// Writes `img` as an uncompressed 24-bit BMP.
//
// BMP because it needs no encoder and no dependency, and opens on any Windows
// machine by double-clicking: a decoded satellite pass the user cannot open is
// not saved. PNG would require a deflate implementation this tree does not
// have.
//
// GRAY8 is expanded to grey RGB rather than written as a palettised 8-bit BMP,
// because an 8-bit BMP needs a 256-entry colour table and more decoders
// mishandle that than mishandle 24-bit.
//
// Returns false with `error` set on any failure; never throws.
bool writeBmp24(const HostImage& img, const std::string& path, std::string& error);

}  // namespace cascade::core

#endif  // CASCADE_CORE_IMAGE_WRITE_HPP
