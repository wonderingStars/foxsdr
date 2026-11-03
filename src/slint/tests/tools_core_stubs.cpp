// tools_core_stubs.cpp - the one core symbol test_slint_host_tools.cpp would otherwise have to link the whole plugin host for.
//
// tools_logic.cpp counts the track sources with gui/module_census.cpp (the rule that says WHY the radar scope would be empty), and that unit's censusModules()
// names a module by core::pluginKey(), which lives in plugin_host.cpp beside the dlopen of every plugin. The test counts rows through countModule() and never
// calls censusModules(); it needs this one symbol to LINK, copied from plugin_host.cpp (a module's key is its file name). The product links the real one
// (cascade_engine). The file is not named test_slint_*.cpp so the directory's glob does not register it as a test.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <filesystem>
#include <string>

#include "core/plugin_host_data.hpp"

namespace cascade::core {

std::string pluginKey(const LoadedPlugin& p) { return std::filesystem::path(p.path).filename().string(); }

}  // namespace cascade::core
