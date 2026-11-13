// wire_c_open.hpp - carries out what a plugin ASKED for and the user CONFIRMED: open an https page in the browser, or the plugin's own library folder in the file
// manager (plugin_abi.h request_open_url / request_open_library_folder; wiring lane C). Nothing here is reachable from a plugin: only the engine's confirm op calls it.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <filesystem>
#include <string>

namespace cascade::engine::wirec {

// "<base>/plugin-libs/<name made safe>": every character of the plugin's name outside [A-Za-z0-9._-] becomes '_', and a name that is empty or only dots becomes "plugin".
// The folder is a child of `base` by construction (no separator can survive), so a plugin cannot name any other place.
std::filesystem::path libraryFolderFor(const std::filesystem::path& base, const std::string& pluginName);

// Opens `what` (an https address, or a folder that exists) with the system's default handler. Never shows a dialog of its own. false + `why` when it could not.
bool openWithSystem(const std::string& what, bool isFolder, std::string& why);

}  // namespace cascade::engine::wirec
