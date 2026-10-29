// fault_probe_names.hpp - the file names the plugin-containment tests give the fault probe when they copy it into a scratch plugins folder.
//
// A module file is found by its extension (".dll", ".so" or ".dylib" - PluginHost::hasPluginExtension), and the plugin's id is the part of the
// name before "-<version>" (core/user_presets.cpp userPresetKey), so the same id is the same plugin on every platform; only the tail differs. The
// tail also declares the plugin ABI the file was built for (the "-abi3-" token: checkModuleFile refuses another without reading the file), and
// CASCADE_PROBE_SUFFIX_ABI2 is the same name with a different one, for the wrong-ABI case.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_TESTS_FAULT_PROBE_NAMES_HPP
#define CASCADE_TESTS_FAULT_PROBE_NAMES_HPP

#if defined(_WIN32)
#define CASCADE_PROBE_SUFFIX "-1.2.3-abi3-win-x64.dll"
#define CASCADE_PROBE_SUFFIX_ABI2 "-1.2.3-abi2-win-x64.dll"
#elif defined(__APPLE__)
#define CASCADE_PROBE_SUFFIX "-1.2.3-abi3-macos-arm64.dylib"
#define CASCADE_PROBE_SUFFIX_ABI2 "-1.2.3-abi2-macos-arm64.dylib"
#else
#define CASCADE_PROBE_SUFFIX "-1.2.3-abi3-linux-x64.so"
#define CASCADE_PROBE_SUFFIX_ABI2 "-1.2.3-abi2-linux-x64.so"
#endif

#endif  // CASCADE_TESTS_FAULT_PROBE_NAMES_HPP
