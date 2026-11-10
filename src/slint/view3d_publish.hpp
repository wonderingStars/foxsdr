// view3d_publish.hpp - THE 3D VIEWS' PUBLISHER (docs/engine-stage4.md 6.64): the host's side of the View3D global of ui/scene3d_views.slint for the views of lane A. Each view gets its picture
// painted by the shared CPU renderer (scene3d.hpp) into two alternating buffers, its labels projected, its camera and choices read back from the window and remembered (view3d_prefs.hpp,
// the interface half's `view3d` setting), and every callback of its controls answered. The pattern is scope_publish.hpp's: the constructor takes the window, wire() registers the
// callbacks once, publish() runs once a tick.
//
// RENDER ONLY WHILE VISIBLE, ONLY WHEN SOMETHING MOVED. A view that is not the one on screen costs nothing but (for the waterfall) one row of levels a frame in its history; a view that is
// on screen is repainted when its signature changes: the camera, a choice, the well's size, the data. The 2D paths are not touched.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <slint.h>

#include "host_duties.hpp"
#include "main.h"
#include "radar3d_logic.hpp"
#include "tunnel3d_logic.hpp"
#include "waterfall3d_compositor.hpp"

namespace foxsdr::slintview3d {

// What the waterfall surface needs of the receiver each tick (the host's numbers, read once).
struct Wf3dContext {
    bool visible = false;                 // the RECEIVER tab is the page on screen
    bool running = false;                 // the receiver is running
    double dbMin = -110.0, dbMax = 0.0;   // the display range
    double centreHz = 0.0, spanHz = 0.0;  // the zoomed window the 2D waterfall shows
    double tunedHz = 0.0;
    std::vector<Wf3dMarker> markers;      // the user markers, as the 2D tabs say them
    std::int64_t nowMs = 0;               // a steady clock
};

class View3DPublisher {
public:
    View3DPublisher(MainWindow& ui, foxsdr::slinthost::HostDuties& duties, bool bounded);
    ~View3DPublisher();
    View3DPublisher(const View3DPublisher&) = delete;
    View3DPublisher& operator=(const View3DPublisher&) = delete;

    // Registers the callbacks of the global and puts the remembered views on the window (once, after it is created).
    void wire();
    // The waterfall's history: one published frame of levels (dB) of the window `windowKey`.
    void pushWaterfallFrame(std::int64_t tMs, const float* bins, std::size_t n, std::uint64_t windowKey);
    // The tick: read the cameras and choices back, repaint what is on screen and changed, save what was changed and has settled.
    void publish(const Wf3dContext& wf);
    // The radar scope's 3D AIRSPACE (the SCOPE tab): what the scope plots this tick (ScopePublisher::radar3dFeed). Repainted only while the scope is up with a position and the 3D view is chosen.
    void publishRadar(const Radar3dFeed& feed, std::int64_t nowMs);
    // The demod scope's 3D TUNNEL (the DEMOD SCOPE window on a baseband input): the newest I/Q samples (ScopePublisher::tunnel3dFeed). Repainted only while the window is open on I/Q or VECTOR
    // with the tunnel chosen and something moved.
    void publishTunnel(const TunnelFeed& feed, std::int64_t nowMs);

    // The drive script's read-back (`state view3d.<what>`): fills `out` and returns "" or says what it does not know.
    std::string readState(const std::string& what, std::string& out) const;
    // A bounded run's evidence: renders, the last one's cost, triangles, saves, buffer detaches.
    std::string summaryLine() const;
    int detaches() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    MainWindow& ui_;
};

}  // namespace foxsdr::slintview3d
