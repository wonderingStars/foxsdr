// process_resources.hpp - what this process holds of the things the operating
// system hands out in limited numbers, as one log line (0.99.103).
//
// WHY. On 2026-10-29 a tester's Intel UHD 630 laptop was refused a second OpenGL
// context twice (GLFW 65543, Win32 0xc00705aa - ERROR_NO_SYSTEM_RESOURCES, 1450:
// "insufficient system resources exist to complete the requested service") -
// and the same machine's earlier sessions show pages torn off successfully
// before a later request was refused. A driver that never allows it is one
// thing; one that stops allowing it part-way through a session says something
// ran out, and the log could not say whether it was FoxSDR that used it up: a
// GDI or USER object leak (Windows allows 10,000 of each a process by default
// and a window needs some of both), a handle leak, or this process's video
// memory. These numbers, written at start-up, at every tear-off and merge-back,
// at a refusal and every ten minutes, are what tells a leak from a driver.
//
// WHAT IS READ, and what each costs. Four Win32 calls that answer from the
// process's own bookkeeping (GetGuiResources twice, GetProcessHandleCount,
// GetProcessMemoryInfo), and one trip through DXGI
// (IDXGIAdapter3::QueryVideoMemoryInfo, which answers for THIS process, every
// API it draws with - the graphics kernel accounts OpenGL's allocations the same
// as Direct3D's) for the adapter this process uses the most memory on. DXGI is
// loaded by name and never linked, so a machine without it (Windows 7, a server
// core) just reads `video ?`. A read is a few milliseconds at most and is made
// on the window's thread only at the moments above, never every frame.
//
// A FIELD THAT COULD NOT BE READ IS A QUESTION MARK, not left out: the line's
// shape is the same on every machine, and a reader can see what was not known.
// Every number is this process's own; nothing about any other process, user or
// file is in it (PRIVACY.md, the log).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_PROCESS_RESOURCES_HPP
#define CASCADE_CORE_PROCESS_RESOURCES_HPP

#include <cstdint>
#include <string>

namespace cascade::core {

struct ProcessResources {
    // -1 for anything that could not be read.
    long gdiObjects = -1;      // Windows: GDI objects (GetGuiResources, GR_GDIOBJECTS)
    long userObjects = -1;     // Windows: USER objects (GR_USEROBJECTS)
    long handles = -1;         // Windows: kernel handles; Linux: open file descriptors
    std::int64_t memoryBytes = -1;       // Windows: private commit; Linux: resident set
    std::int64_t videoUsedBytes = -1;    // this process's video memory in use (DXGI)
    std::int64_t videoBudgetBytes = -1;  // what the system would let it use
};

// Reads them now. Never throws; a field that cannot be read stays -1.
ProcessResources readProcessResources();

// `resources: gdi 412, user 97, handles 1234, memory 812 MB, video 356 of 3900 MB
// - at start-up`. `when` is the fixed phrase after the dash, or null for none.
std::string processResourcesLine(const ProcessResources& r, const char* when);

}  // namespace cascade::core

#endif  // CASCADE_CORE_PROCESS_RESOURCES_HPP
