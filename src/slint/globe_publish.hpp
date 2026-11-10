// globe_publish.hpp - THE GLOBE VIEWS' PUBLISHER (docs/engine-stage4.md 6.65): the GlobeModel of every plugin page that offers a 3D view, built from what the kit holds, and the
// gestures of ui/globe_views.slint turned into the view's state. The KIT PUBLISHER (kit_publish.*) owns the pages; this class owns what the globe pages SHOW: it is asked
// once per page build for the model of a page (fill), once per publish for the FT8 decoder lines and the debounced save of the views' memory (tick), and once for the wiring (wire).
//
// WHICH PAGE OFFERS WHICH VIEW (globe_logic.hpp viewKindFor, the kit's page kinds): Propagation Tools, GOES HRIT / LRIT and Solar System View on their picture page (kind 2);
// FT8 on its panel page (kind 3: its "FT8 - PSK Reporter" table is the LIST); Satellites on its map page (kind 5). Everything the page had stays: the VIEW bar sits over the
// page's own body, and the page's own view is the default.
//
// WHAT A VIEW REMEMBERS - the view chosen, the camera, the band, the window, the scale - is kept per plugin and written through the framework's one setting (AppConfig::view3d,
// view3d_prefs.hpp) by the hooks main.cpp gives (setViewPrefsHooks): read when a view is first used, saved 0.4 s after its last change (a drag is many changes). The date of the solar
// system is not remembered: it opens on today.
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
#include "scene3d_slint.hpp"

namespace foxsdr::slintkit {

class GlobePublisher {
public:
    explicit GlobePublisher(PluginKit& kit);
    ~GlobePublisher();
    GlobePublisher(const GlobePublisher&) = delete;
    GlobePublisher& operator=(const GlobePublisher&) = delete;

    // The two ends of the framework's setting: `load` answers the text of AppConfig::view3d, `save` writes it (the interface half is saved by the host's duties).
    void setViewPrefsHooks(std::function<std::string()> load, std::function<void(const std::string&)> save);

    // Whether a page of `pageKind` of this plugin carries the VIEW bar.
    bool offers(const std::string& plugin, int pageKind) const;
    // Once per publish, whatever page shows: the FT8 decoder lines into the spot store (so a window that was closed while stations were heard still has them), and the views'
    // memory saved when it has been still for 0.4 s.
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
        scene3d::SceneSurface surface;            // the well's two alternating buffers
        slint::Image image;                       // the picture last painted
        int w = 0, h = 0;
        std::string composeKey;
        std::vector<scene3d::Label3D> labels;
        bool composed = false;
        globe::GlobeCamera shown;                 // the camera the picture was drawn with
        globe::GlobeCamera initial;               // what a camera never touched shows (the home, the receiver, the spacecraft, the default tilt)
        int camSeq = 0;                           // moves when the HOST moved the camera: the well takes the host's numbers
        bool touched = false;                     // a gesture or a read setting: this view is written to the setting
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
    void changed(Surface& s);                     // a view changed: it is written to the setting soon
    void flushPrefs();

    PluginKit& kit_;
    std::map<std::string, std::unique_ptr<Surface>> surfaces_;
    std::unique_ptr<globe::LandMask> land_;
    globe::Ft8SpotStore ft8_;
    std::uint64_t ft8LastId_ = 0;
    globe::GlobeCompositor compositor_;
    std::function<std::string(int)> pluginOfPage_;
    std::function<std::string()> loadPrefs_;
    std::function<void(const std::string&)> savePrefs_;
    std::string lastSavedPrefs_;
    bool prefsDirty_ = false;
    std::int64_t prefsChangedAtMs_ = 0;
};

}  // namespace foxsdr::slintkit
