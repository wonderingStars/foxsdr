// app_window_plugin_use.cpp - what the window shows of each plugin, told to the engine (stub campaign B2, PLUGIN-RUN; master 0.99.73).
//
// A plugin set to AUTO runs while something is using it (core/plugin_run.hpp). Most of the signals are the engine's own (a patch node, the
// speakers, a text sink, a browser session); four are only a window's to know: one of the plugin's windows is shown, its map page is open,
// the radar scope is showing (every track source), the Decoder output window is open (a text decoder). Master read them in the same
// function that woke the plugin; the engine does that now, once a pass, and this is the window's half: it computes the four from its own
// state each frame and sends the WHOLE table with FOXAPP_OP_PLUGIN_USE when it differs from the last one it sent - so a quiet window sends
// nothing, and the engine's lifecycle step (one pass later, by design: the command is applied at the head of the next pass) wakes the plugin.
//
// A plugin is named by its module file (the PLUGINS row's `file`); a window by the identity ImGui keeps it under, which carries the
// plugin's display name ("<name> image###image_<name>", "<title>###panel_<name>", "<title>###instrument_<name>|<title>").
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "gui/app_window.hpp"

#include <cstdint>
#include <string>

#include "core/app_commands.hpp"
#include "core/plugin_abi.h"

namespace cascade::gui {

namespace {

// Whether the window identity `id` is one of the plugin called `name`'s.
bool windowIdBelongsTo(const std::string& id, const std::string& name) {
    struct Tag {
        const char* text;
        bool endsAtBar;  // "###instrument_<plugin>|<title>": the plugin's name ends at the bar
    };
    static constexpr Tag kTags[] = {{"###image_", false}, {"###panel_", false}, {"###instrument_", true}};
    for (const Tag& tag : kTags) {
        const std::size_t at = id.find(tag.text);
        if (at == std::string::npos) { continue; }
        const std::size_t from = at + std::char_traits<char>::length(tag.text);
        std::size_t to = id.size();
        if (tag.endsAtBar) {
            const std::size_t bar = id.find('|', from);
            if (bar != std::string::npos) { to = bar; }
        }
        return to - from == name.size() && id.compare(from, to - from, name) == 0;
    }
    return false;
}

}  // namespace

void AppWindow::reportPluginUse() {
    pluginUseText_.clear();
    const PluginListCache& pl = pluginLists();
    for (const PluginRow& r : pl.plugins) {
        if (!r.loaded || r.file.empty()) { continue; }
        std::uint32_t bits = 0u;
        for (const std::string& id : pluginWindows_.ids()) {
            if (windowIdBelongsTo(id, r.name)) {
                bits |= FOXAPP_PLUGIN_USE_WINDOW;
                break;
            }
        }
        if ((r.caps & CASCADE_CAP_TRACK_SOURCE) != 0u) {
            for (const MapPage& pg : mapPages_) {
                if (pg.open && pg.plugin == r.name) {
                    bits |= FOXAPP_PLUGIN_USE_MAP_PAGE;
                    break;
                }
            }
            if (scopeMode_) { bits |= FOXAPP_PLUGIN_USE_RADAR_SCOPE; }
        }
        if (decoderWindowOpen_ && (r.caps & (CASCADE_CAP_DECODER | CASCADE_CAP_IQ_DECODER)) != 0u) {
            bits |= FOXAPP_PLUGIN_USE_DECODER_OUTPUT;
        }
        if (bits == 0u) { continue; }
        pluginUseText_ += r.file;
        pluginUseText_ += '\t';
        pluginUseText_ += std::to_string(bits);
        pluginUseText_ += '\n';
    }
    if (pluginUseText_ == pluginUseSent_) { return; }
    pluginUseSent_ = pluginUseText_;
    uiSubmit(cascade::core::cmd::makeText(FOXAPP_OP_PLUGIN_USE, pluginUseText_, 0));
}

}  // namespace cascade::gui
