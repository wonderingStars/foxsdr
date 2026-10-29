// plugin_ui_port.hpp (engine) - THE ENGINE'S SIDE OF THE PLUGIN UI PLANE: the PluginUiPort an interface holds (core/plugin_ui_port.hpp), built
// over the Engine, the PluginUi (core/plugin_ui.hpp) and the engine's directive link (shell retirement R4; docs/engine-stage4.md 6.25).
//
// It is today's PluginFeedImpl (the shell's engine_shell.cpp until R4) moved here, where it can keep seeing PluginUi - which the interface's fence
// denies - and given the three calls the window used to make of the shell's HOST class and of the shell itself: setFrontEnd, serveTiles and
// release. It is created by one place only, the table's get_local_extension(FOXAPI_EXT_PLUGIN_UI) (fox_engine_api.cpp), which has checked
// that the caller is a LOCAL interface session; the interface (the window, a test) never names this header. Part of cascade_engine, so the
// headless engine serves the extension too (the headless engine's own dormant PluginUi stays as it is, Engine::headlessUi_).
//
// THREAD AFFINITY. The thread that attaches is the interface's thread: Engine::attachPluginUi records it, the engine applies a directive
// inline only where it runs on it, and the plugin ABI's rule (created, polled and destroyed on the interface's thread) is the rule here.
// Every method except disconnect() and the destructor counts a call from any other thread in offThreadCalls() - a tripwire, not a lock: it
// changes nothing about what the call does, and the tests assert it stays zero (the engine's offPassKeyTouches is the same kind of counter).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_ENGINE_PLUGIN_UI_PORT_HPP
#define CASCADE_ENGINE_PLUGIN_UI_PORT_HPP

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

#include "core/plugin_directive.hpp"
#include "core/plugin_ui.hpp"
#include "core/plugin_ui_port.hpp"

namespace cascade::engine {

class Engine;

class PluginUiPortImpl final : public cascade::core::PluginUiPort {
public:
    // Attaches (Engine::attachPluginUi) - see its comment for what that does and the thread it must be called on. Use attachPluginUiPort:
    // the attach can be refused.
    PluginUiPortImpl(Engine& engine, Steps steps);
    ~PluginUiPortImpl() override;

    const std::vector<cascade::core::HostTrack>& tracks() const override;
    const std::vector<cascade::core::HostTrack>& unplacedTracks() const override;
    const std::vector<cascade::core::HostPath>& paths() const override;
    const std::vector<cascade::core::HostPanel>& panels() const override;
    const std::vector<cascade::core::HostInstrument>& instruments() const override;
    const std::vector<std::string>& trackPluginNames() const override;
    bool altitudeNear(const std::string& plugin, const std::string& id, double latDeg, double lonDeg, double& outAltM) override;
    void poll() override;
    void pollImages(std::vector<cascade::core::HostImage>& out) override;
    void applyNewestDirective() override;
    void setFrontEnd(const cascade::core::PluginUiFrontEnd& facts) override;
    void serveTiles(bool clearFirst, bool serve, std::size_t maxTiles, const cascade::core::WebTileSource& source) override;
    void release() override;
    void disconnect() override;

    // (the tests', through rawPluginUi)
    cascade::core::PluginUi& ui() { return ui_; }
    // Whether the attach took (false: another front end is attached).
    bool attached() const { return connected_; }
    // How many calls arrived from a thread other than the one that attached (see the file header).
    std::uint64_t offThreadCalls() const { return offThread_.load(std::memory_order_relaxed); }
    std::thread::id interfaceThread() const { return guiThread_; }

private:
    // What one directive does: the window's step before, the plugin UI rebuilt or emptied, the window's step after.
    void apply(const cascade::core::PluginDirective& d);
    void touch() const;

    Engine& engine_;
    Steps steps_;
    cascade::core::PluginUi ui_;
    cascade::core::PluginDirectiveLink& directives_;
    bool connected_ = false;
    std::thread::id guiThread_;
    mutable std::atomic<std::uint64_t> offThread_{0};
};

// Builds the port for an interface on the CALLING thread, or null when another port is attached.
std::unique_ptr<cascade::core::PluginUiPort> attachPluginUiPort(Engine& engine, cascade::core::PluginUiPort::Steps steps);

// The plugin UI inside a port, for the tests (they drive grants and injected tracks through it): the port must be the engine's.
cascade::core::PluginUi& rawPluginUi(cascade::core::PluginUiPort& port);
// The engine's port object itself, for the tests that read its tripwire.
PluginUiPortImpl& rawPluginUiPort(cascade::core::PluginUiPort& port);

}  // namespace cascade::engine

#endif  // CASCADE_ENGINE_PLUGIN_UI_PORT_HPP
