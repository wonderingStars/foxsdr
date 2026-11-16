// session_env.hpp - the machine around the radios, read once a session for the `session_env` journal event (contract 3.8).
//
// WHAT IS READ: the processor's model name (without its clock speed: a hertz number is refused by the contract's text rules), its thread count, the
// installed memory, the graphics adapter's model name and driver version, the product names of the USB host controllers, the number of displays
// and each display's scaling. WHAT IS NEVER READ: a machine or user name, an address, a serial number, a device path, the name of an audio
// device (a person's headphones are named after them), a display's name or its position. The audio API and the output's CLASS are the engine's
// to say (it knows what is playing), not read here.
//
// Everything is read from the operating system's own registry or API and nothing is opened: no radio, no USB device. A value that cannot be
// read is left empty (and left out of the message), never guessed.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_SESSION_ENV_HPP
#define CASCADE_CORE_SESSION_ENV_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace cascade::core {

struct SessionEnv {
    std::string cpu;                 // "Intel(R) Core(TM) i7-8700K CPU" - no "@ 3.70GHz"
    int cpuThreads = 0;              // 0 = unknown
    std::uint64_t ramMb = 0;         // 0 = unknown
    std::string gpu;                 // the primary graphics adapter's model name
    std::string gpuDriver;           // its driver version, "552.44"; empty when the system does not say
    std::vector<std::string> usbHosts;   // product names of the USB host controllers, at most 8
    int displayCount = 0;            // 0 = unknown
    std::vector<long long> displayScalePct;   // each display's scaling, 25 to 1000, at most 8
};

// Reads the machine. May take a few milliseconds (registry and SetupAPI): called once, at start-up, off the frame path's hot loop.
SessionEnv readSessionEnv();

}  // namespace cascade::core

#endif  // CASCADE_CORE_SESSION_ENV_HPP
