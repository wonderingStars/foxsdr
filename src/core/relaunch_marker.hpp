// relaunch_marker.hpp - the mark a software-renderer relaunch leaves on its second process.
//
// 0.99.90 merge, engine extraction (docs/engine-merge-0.99.90.md). On master this lived in
// gui/software_gl.{hpp,cpp} and main() read it. Here the claim of the single-instance name is a
// step of process boot (engine/process_boot.cpp, claimInstance), which is the engine library's,
// and the engine library may not reach into gui/: so the one function boot needs - take the mark,
// once, and clear it - lives in core, and gui/software_gl.hpp forwards to it under master's names.
// Everything else about the software renderer (loading Mesa, starting the second process, telling
// the person) stays in gui/software_gl.*: the window application's, and nothing the headless
// engine can reach.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_RELAUNCH_MARKER_HPP
#define CASCADE_CORE_RELAUNCH_MARKER_HPP

#include <string>

namespace cascade::core {

// The variable that marks the second process, holding the first one's process id: the first one
// keeps the primary-instance claim (core/tester_link.hpp) while it waits, so the second is the
// primary instance in all but name.
inline constexpr const char* kSoftwareGlRelaunchEnv = "FOXSDR_SOFTGL_RELAUNCH";

// In the second process, once, in the instance-claim step of process boot: whether this process is
// one, and the first one's id in `parent`. The variable is cleared, so that nothing this process
// starts inherits it. Always false off Windows (there is no relaunch there).
bool takeSoftwareGlRelaunchMarker(std::string& parent);

}  // namespace cascade::core

#endif  // CASCADE_CORE_RELAUNCH_MARKER_HPP
