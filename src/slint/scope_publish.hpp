// scope_publish.hpp - THE SCOPE MODES' PUBLISHER (docs/engine-stage4.md 6.57): scope_logic's DemodScope read from the C API table's scope taps, its glass painted by scope_compositor into an
// image, and the models of ui/scope_modes.slint set when they changed; every ScopeEvents callback turned into the call on the state object the ImGui page's own control makes. The pattern is
// extend_publish.hpp's: the constructor takes the window, wire() registers the callbacks once, publish(state) runs once a tick.
//
// THE SETTINGS are the interface half's (demodScopeSignal, Timebase, Gain, AutoGain, Display; the window's open flag is saved and never restored): read at start, saved on each key of the
// instrument (the ranging's own detents are saved with the next key or the close, so a save is not written thirty times a second). A bounded run sends and writes nothing it does not say.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <slint.h>

#include "host_duties.hpp"
#include "main.h"
#include "plugin_kit.hpp"
#include "radar3d_logic.hpp"
#include "radar_logic.hpp"
#include "scope_logic.hpp"
#include "tunnel3d_logic.hpp"

namespace foxsdr::slintscope {

class ScopePublisher {
public:
    ScopePublisher(MainWindow& ui, foxsdr::slintkit::PluginKit& kit, const FoxEngineApi* api, FoxSession* session, foxsdr::slinthost::HostDuties& duties, bool bounded);
    ~ScopePublisher();
    ScopePublisher(const ScopePublisher&) = delete;
    ScopePublisher& operator=(const ScopePublisher&) = delete;

    // Registers every ScopeEvents callback (once, after the window is created).
    void wire();
    // The tick: gather, range, paint and set the models that changed. `nowS` is a steady clock in seconds.
    void publish(const FoxReceiverState& state, double nowS);

    // THE RADAR SCOPE is the SCOPE tab (view-index 2): the Radar section's key puts the window there and remembers the view it came from; EXIT SCOPE, the key again and Esc go back.
    void toggleRadar();
    void leaveRadar();
    bool radarOn() const;

    // The TOOLS row's press (and the bounded run's FOXSDR_SLINT_DEMOD_SCOPE): opens or closes the window.
    void toggleDemod();
    void setDemodOpen(bool open);
    // a bounded run's capture: open on input `signal` (0-4; -1 keeps the saved one)
    void openDemodForCapture(int signal);
    bool demodOpen() const;
    int demodSignal() const;       // for the rail row's chip
    bool demodLive() const;

    // THE 3D AIRSPACE'S FEED (docs 6.64): the aircraft the scope plots (positions from the antenna in nautical miles, altitude in feet, course, trail), its range and TRAILS option and the tube's
    // corner words, as of the last publish; `shown` is false unless the SCOPE tab is up with a position. Read each tick by View3DPublisher.
    foxsdr::slintview3d::Radar3dFeed radar3dFeed() const;
    // THE 3D TUNNEL'S FEED (docs 6.64): the newest baseband I/Q samples of the demod scope's tap with their rate and the scope's own sentences; `shown` is false unless the demod window is open
    // on a baseband input (I/Q or VECTOR). Read each tick by View3DPublisher.
    foxsdr::slintview3d::TunnelFeed tunnel3dFeed() const;

    // The drive script's read-back (`state scope.<what>`): fills `out` and returns "" or says what it does not know.
    std::string readState(const std::string& what, std::string& out) const;

    // A bounded run's evidence: the window, the signal, the sweep, the bins, the segments painted, the frames, the saves.
    std::string summaryLine() const;

private:
    void radarPublish(const FoxReceiverState& state, double nowS);
    struct Impl;
    std::unique_ptr<Impl> impl_;
    MainWindow& ui_;
};

}  // namespace foxsdr::slintscope
