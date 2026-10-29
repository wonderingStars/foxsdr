// pass_part.hpp - THE THREE PARTS OF THE ENGINE'S PASS, the one word a bounded run's host and its window share (shell retirement R9, 2026-10-28).
// A free-running pass runs them in this order as one; a STEPPED one (a bounded run) runs each when the front end asks, which is how the window keeps
// the order the pump had in its frame - the part up to and including the publish, then the audio mute (which the frame ran after its look, before it
// drew), then the workers and audio:
//   Front  the GPS fix, the command queues, the inputs, the remote controls, the plugins and the scanner, the patch, the publish
//   Mute   the audio mute (a decision about the receiver the state it just published describes)
//   Back   the workers (device scans and opens, the retune coalescer, downloads) and the audio sink's health, the config copy
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

namespace cascade::core {
enum class PassPart { Front, Mute, Back };
}  // namespace cascade::core
