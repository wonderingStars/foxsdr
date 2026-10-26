// plugin_window_ids.hpp - THE IDENTITIES OF A PLUGIN'S WINDOWS, built from what the MODULE DECLARES (its display name and the titles its own panel
// and instrument tables give) and not from an instance (stub campaign B5; master 0.99.73).
//
// A plugin set to AUTO that nothing is using is DORMANT and has no instance, yet its windows must be listed on the rail and be recognised as shown,
// because opening one is how it is woken (the Fitted modules window's double-click does the same). The draw loops in AppWindow::drawPluginWindows
// build the same strings from the instance's own copies of the same declared names, so the two cannot disagree: ImGui hashes only what follows
// "###", so the plugin name is in every id, and an instrument's carries its title as well (two instruments of one module must not fold into one).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_PLUGIN_WINDOW_IDS_HPP
#define CASCADE_GUI_PLUGIN_WINDOW_IDS_HPP

#include <string>

namespace cascade::gui {

// A picture decoder's window.
inline std::string imageWindowId(const std::string& plugin) { return plugin + " image###image_" + plugin; }

// A plugin's own panel; `title` is the table's own, or the plugin's name when it gave none.
inline std::string panelWindowId(const std::string& plugin, const std::string& title) { return title + "###panel_" + plugin; }

// A plugin's instrument.
inline std::string instrumentWindowIdFor(const std::string& plugin, const std::string& title) {
    return title + "###instrument_" + plugin + "|" + title;
}

}  // namespace cascade::gui

#endif  // CASCADE_GUI_PLUGIN_WINDOW_IDS_HPP
