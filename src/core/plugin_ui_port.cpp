// plugin_ui_port.cpp - the port that does nothing (core/plugin_ui_port.hpp; shell retirement R4).
//
// What a window holds when the engine cannot serve FOXAPI_EXT_PLUGIN_UI (an engine without FOXAPI_CAP_LOCAL_EXT, a refused attach): every
// accessor answers an empty list, every call does nothing, so the window runs without a plugin UI instead of without a check at each of its
// call sites.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/plugin_ui_port.hpp"

namespace cascade::core {

namespace {

class NullPluginUiPort final : public PluginUiPort {
public:
    const std::vector<HostTrack>& tracks() const override { return tracks_; }
    const std::vector<HostTrack>& unplacedTracks() const override { return tracks_; }
    const std::vector<HostPath>& paths() const override { return paths_; }
    const std::vector<HostPanel>& panels() const override { return panels_; }
    const std::vector<HostInstrument>& instruments() const override { return instruments_; }
    const std::vector<std::string>& trackPluginNames() const override { return names_; }
    bool altitudeNear(const std::string&, const std::string&, double, double, double&) override { return false; }
    void poll() override {}
    void pollImages(std::vector<HostImage>& out) override { out.clear(); }
    void applyNewestDirective() override {}
    void setFrontEnd(const PluginUiFrontEnd&) override {}
    void serveTiles(bool, bool, std::size_t, const WebTileSource&) override {}
    void release() override {}
    void disconnect() override {}

private:
    std::vector<HostTrack> tracks_;
    std::vector<HostPath> paths_;
    std::vector<HostPanel> panels_;
    std::vector<HostInstrument> instruments_;
    std::vector<std::string> names_;
};

}  // namespace

std::unique_ptr<PluginUiPort> makeNullPluginUiPort() { return std::make_unique<NullPluginUiPort>(); }

}  // namespace cascade::core
