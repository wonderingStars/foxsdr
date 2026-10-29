// mp3_support.hpp - whether this build can write MP3 at all (shell retirement R11a, 2026-10-29).
//
// The patch page's output chooser needs ONE fact about the MP3 writer: whether it can write MP3 on this build (Windows only, through Media Foundation's
// own encoder; elsewhere the page writes WAV). That is decided when the program is COMPILED (core/mp3_writer.cpp has a Windows build and a stub build),
// so it is a constant, and a constant is data: the window includes this header and not core/mp3_writer.hpp, whose class is the engine's file worker.
// Mp3Writer::available() returns it, so the two can never disagree.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

namespace cascade::core {

#ifdef _WIN32
inline constexpr bool kMp3Supported = true;
#else
inline constexpr bool kMp3Supported = false;
#endif

}  // namespace cascade::core
