// REAL PLUGIN MODULES BUILT FOR ANOTHER PLUGIN ABI, for tests/test_health_paths.cpp:
// the plugin host loads each as it loads any plugin and refuses it for the one
// reason the anonymous failure count has a word for ("abi", core/health_events.hpp).
// Built as a MODULE exactly as a third-party plugin is - plugin_abi.h only, never
// linked against cascade_lib.
//
// TWO WAYS A PLUGIN FOR ANOTHER HOST LOOKS FROM HERE, one per build of this file:
//
//   (default)            its descriptor declares an ABI one newer than this host's:
//                        a plugin built for a later FoxSDR, answered whatever host
//                        asks.
//   ABI_FIXTURE_DECLINES its query returns null for a host whose ABI it does not
//                        know, which is the pattern plugin_abi.h teaches and what
//                        every plugin built for an OLDER host does when this one
//                        asks it: the common case in the field.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstddef>
#include <cstdint>

#include "plugin_abi.h"

namespace {

const CascadePluginDesc kDesc = {
    static_cast<uint32_t>(sizeof(CascadePluginDesc)),
    CASCADE_PLUGIN_ABI_VERSION + 1u,
    "ABI Mismatch Probe",
    "1.0.0",
    "FoxSDR tests",
    "PolyForm-Noncommercial-1.0.0",
    0u,
    0u,
    nullptr,
};

}  // namespace

extern "C" CASCADE_PLUGIN_EXPORT const CascadePluginDesc* cascade_plugin_query(uint32_t) {
#if defined(ABI_FIXTURE_DECLINES)
    return nullptr;
#else
    return &kDesc;
#endif
}
