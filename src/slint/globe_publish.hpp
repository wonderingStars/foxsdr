// globe_publish.hpp - THE GLOBE VIEWS' PUBLISHER (docs/engine-stage4.md 6.65): the GlobeModel of every plugin page that offers a 3D view, built from what the kit holds, and the
// gestures of ui/globe_views.slint turned into the view's state. The KIT PUBLISHER (kit_publish.*) owns the pages; this class owns what the globe pages SHOW: it is asked
// once per page build for the model of a page (fill), once per publish for the FT8 decoder lines (tick), and once for the wiring (wire).
//
// WHICH PAGE OFFERS WHICH VIEW (globe_logic.hpp viewKindFor, the kit's page kinds): Propagation Tools, GOES HRIT / LRIT and Solar System View on their picture page (kind 2);
// FT8 on its panel page (kind 3: its "FT8 - PSK Reporter" table is the LIST); Satellites on its map page (kind 5). Everything the page had stays: the VIEW bar sits over the
// page's own body, and the page's own view is the default.
//
// THE STATE of every view (the VIEW chosen, the camera, the band, the window, the scale, the date) is kept per plugin for the life of the window, as the sibling map pages' toggles
// are; nothing of it is written to the settings file.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <slint.h>

#include "globe_compositor.hpp"
#include "globe_draw.hpp"
#include "globe_logic.hpp"
#include "main.h"
#include "plugin_kit.hpp"

namespace foxsdr::slintkit {

class GlobePublisher {
public:
    explicit GlobePublisher(PluginKit& kit);
    ~GlobePublisher();
    GlobePublisher(const GlobePublisher&) = delete;
    GlobePublisher& operator=(const GlobePublisher&) = delete;

    // Whether a page of `pageKind` of this plugin carries the VIEW bar.
    bool offers(const std::string& plugin, int pageKind) const;
    // Once per publish, whatever page shows: the FT8 decoder lines into the spot store (so a window that was closed while stations were heard still has them).
    void tick(const FoxReceiverState& state);
    // The model of one page. `sig` gains everything the model shows (the page is set again only when it changed).
    void fill(const std::string& plugin, int pageKind, const FoxReceiverState& state, ::GlobeModel& out, std::string& sig);
    // Wires every callback of GlobeEvents; `pluginOfPage` answers the plugin of a page index ("" when none).
    void wire(::MainWindow& ui, std::function<std::string(int)> pluginOfPage);
    // A plate key of the plugin (its label as the module page draws it): ROTATE LEFT / ROTATE RIGHT / TILT / ZOOM IN / ZOOM OUT of the Solar System View drive the 3D camera
    // while the 3D view is on. True when taken (the plugin is then not sent the command); false: send it.
    bool takeCommand(const std::string& plugin, const std::string& label);
    bool globeOn(const std::string& plugin) const;
    // For the bounded run's evidence: "globe: Solar System View 3D, ..." ("" when none is on).
    std::string statusLine() const;
    // The state the drive scripts read (main.cpp names it): the view, camera and controls of a plugin as one line.
    std::string stateText(const std::string& plugin) const;

private:
    struct Surface {
        globe::ViewState view;
        globe::ViewKind kind = globe::ViewKind::None;
        std::array<slint::SharedPixelBuffer<slint::Rgba8Pixel>, 2> buffers;
        int which = 0;
        int w = 0, h = 0;
        std::string composeKey;
        std::vector<globe::ScreenLabel> labels;
        bool composed = false;
        globe::GlobeCamera shown;                 // the camera the picture was drawn with (a drag starts from it)
        globe::GlobeCamera dragBase;
        double spinBase = 0.0, tiltBase = 0.0;
        bool dragging = false;
        globe::Texture earth;                     // the baked surface
        globe::Texture goes;                      // the draped picture
        std::uint64_t goesRevision = ~std::uint64_t{0};
        int goesSat = -1;
        std::int64_t goesAtMs = -1000000;
        std::vector<std::string> chipIds;         // the i-th chip's meaning (satellite ids)
        std::string note;                         // the last line stateText reports
    };
    Surface& surfaceFor(const std::string& plugin);
    const Surface* findSurface(const std::string& plugin) const;
    std::vector<globe::PanelRowText> panelRows(const std::string& plugin) const;
    const globe::LandMask& land();
    // the views
    void fillPropagation(Surface& s, const FoxReceiverState& st, ::GlobeModel& m, std::string& sig);
    void fillFt8(Surface& s, const FoxReceiverState& st, ::GlobeModel& m, std::string& sig);
    void fillGoes(Surface& s, const FoxReceiverState& st, ::GlobeModel& m, std::string& sig);
    void fillSolar(Surface& s, const FoxReceiverState& st, ::GlobeModel& m, std::string& sig);
    void fillSatellites(Surface& s, const std::string& plugin, const FoxReceiverState& st, ::GlobeModel& m, std::string& sig);
    // the picture: composes `list` when `key` is new, hands the picture and labels to the model
    void finish(Surface& s, const globe::GlobeDrawList& list, const std::string& key, ::GlobeModel& m, std::string& sig);
    void gesture(int page, const std::function<void(Surface&)>& f);

    PluginKit& kit_;
    std::map<std::string, std::unique_ptr<Surface>> surfaces_;
    std::unique_ptr<globe::LandMask> land_;
    globe::Ft8SpotStore ft8_;
    std::uint64_t ft8LastId_ = 0;
    globe::GlobeCompositor compositor_;
    std::function<std::string(int)> pluginOfPage_;
    std::string satelliteSelected_;
};

}  // namespace foxsdr::slintkit
