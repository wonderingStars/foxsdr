// kit_publish.hpp - THE PLUGIN KIT'S PUBLISHER (docs/engine-stage4.md 6.37): the pages and the DECODE bank as Slint models, built from
// the kit's state (plugin_kit.hpp) once a tick and set only when they changed (signatures.hpp), and every callback of ui/pages.slint
// turned into one command on the kit. The map pages' pictures are composed here (map_compositor.hpp) into persistent pixel buffers.
//
// WHAT A PAGE IS. The window shows plugin windows as docked tabs after RECEIVER / PATCH / SCOPE (view-index 3 + i), and a torn-off page as
// an in-window TornWindow over the centre (an OS-level window is a later step). Nothing opens by itself (0.79.1): a page opens from its
// DECODE row, a preset (the engine's PRESET_APPLIED event) or a Fitted modules row, and closes from its tab's key or the window's chrome.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <slint.h>

#include "main.h"
#include "map_compositor.hpp"
#include "plugin_kit.hpp"
#include "signatures.hpp"

namespace foxsdr::slintkit {

class KitPublisher {
public:
    KitPublisher(MainWindow& ui, PluginKit& kit);
    ~KitPublisher();
    KitPublisher(const KitPublisher&) = delete;
    KitPublisher& operator=(const KitPublisher&) = delete;

    // Wires every page and DECODE bank callback of the window to the kit (once, after the window is created).
    void wire();
    // Builds and publishes the models from the kit's state (after kit.tick()).
    void publish(const FoxReceiverState& state);
    // The count of open pages (for the bounded run's evidence).
    std::size_t pageCount() const { return pages_.size(); }
    std::size_t openMapPages() const;
    std::string pagesLine() const;   // "pages: output, APT image(torn)" for the bounded run
    // A bounded run's self-capture (FOXSDR_SLINT_OPEN="output,fitted,panel:<name>,instrument:<name>|<title>,image:<name>,map:<name>,module:<file>"):
    // the pages named are opened on the first publish that knows the plugins, the last one named is shown. Never set in a release.
    void openFromEnv(const std::string& spec);

private:
    struct Page {
        std::string id;            // "output", "image:<name>", "panel:<name>", "instrument:<name>|<title>", "map:<name>", "fitted", "module:<file>", "details"
        int kind = 0;
        std::string plugin;        // the display name (the window rows and the map pages carry it)
        std::string file;          // the module file
        std::string title;
        bool torn = false;
        float tx = 80.f, ty = 60.f, tw = 720.f, th = 520.f;
        bool rolled = false;
        bool settingsOpen = false;
        bool follow = true;
        std::string saveNote;
        std::vector<float> widths;
        int obsCourse = 90;
    };
    struct PictureBuffer {
        slint::SharedPixelBuffer<slint::Rgba8Pixel> buffer;
        std::uint64_t revision = ~std::uint64_t{0};
        std::uint32_t w = 0, h = 0;
    };
    struct MapSurface {
        slintmap::TileCompositor comp;
        std::array<slint::SharedPixelBuffer<slint::Rgba8Pixel>, 2> buffers;
        int which = 0;
        int w = 0, h = 0;
        bool coverageOn = false, trailsOn = true, altColours = true, onScreenOnly = false, pickArmed = false;
        int sortKey = 0;
        bool sortAscending = true;
        std::string hoverText;
        std::vector<std::string> targetIds;         // the i-th MapTarget's id
        std::vector<std::string> listIds;           // the i-th list row's id
        bool sourceSet = false;
        std::string sourceAttribution;
    };

    // pages
    Page* findPage(const std::string& id);
    Page& openPage(const std::string& id, int kind, const std::string& plugin, const std::string& file, const std::string& title);
    void closePage(const std::string& id);
    void showPage(const std::string& id);
    void openWindowsFor(const PluginRow& r, bool withOutput);
    void syncOpenSet();
    int currentPage() const;

    // models
    PageTab pageModel(const Page& p, const FoxReceiverState& state, std::string& sig);
    void fillPrelude(const Page& p, PageTab& t, std::string& sig);
    void fillOutput(PageTab& t, std::string& sig);
    void fillPicture(const Page& p, PageTab& t, std::string& sig);
    void fillPanel(const Page& p, PageTab& t, std::string& sig);
    void fillInstrument(const Page& p, PageTab& t, std::string& sig);
    void fillMap(const Page& p, PageTab& t, const FoxReceiverState& state, std::string& sig);
    void fillFitted(PageTab& t, const FoxReceiverState& state, std::string& sig);
    void fillModule(const Page& p, PageTab& t, const FoxReceiverState& state, std::string& sig);
    void fillDetails(PageTab& t, const FoxReceiverState& state, std::string& sig);
    DetailsModel detailsModel(const FoxReceiverState& state, std::string& sig);
    void publishDecodeBank(const FoxReceiverState& state);
    void publishPages(const FoxReceiverState& state);
    void publishMuteQuestion();
    std::shared_ptr<slint::Model<PresetKeyModel>> presetModel(const PluginRow& r, const FoxReceiverState& state, bool mine, std::string& sig);
    std::shared_ptr<slint::Model<SettingField>> fieldsModel(const std::string& file, std::string& sig);
    slint::Image pictureImage(const std::string& plugin);
    MapSurface& surfaceFor(const std::string& plugin);
    const PluginRow* rowOf(const Page& p) const;

    MainWindow& ui_;
    PluginKit& kit_;
    foxsdr::slinthost::Signatures sigs_;
    std::vector<Page> pages_;
    std::shared_ptr<slint::VectorModel<PageTab>> pagesModel_;
    std::string pagesSig_;
    std::map<std::string, PictureBuffer> pictures_;
    std::map<std::string, std::shared_ptr<slint::VectorModel<SettingField>>> fieldModels_;   // one per form, updated IN PLACE: a replaced model recreates the fields and loses what is being typed
    std::map<std::string, std::unique_ptr<MapSurface>> surfaces_;
    std::map<std::string, bool> entrySettingsOpen_;   // the rail entries' settings folds, by file
    std::map<std::string, bool> decodeOpen_{{"plugins", false}, {"details", false}};
    std::set<int> fittedHidden_;                     // the state chips switched off (by fittedStateIndex)
    std::string fittedSearch_;
    std::string armedRemove_;                         // the Fitted row whose REMOVE was pressed once
    std::int64_t armedAtMs_ = 0;
    std::string detailsId_;                           // the target the details show
    std::string presetNote_;                          // the rail's preset result note
    std::vector<std::string> muteNames_, muteKeys_;   // the "Sound is muted" question's subject
    bool muteQuestionOpen_ = false;
    std::string catalogueUrl_;
    FoxReceiverState state_{};
    std::int64_t nowMs_ = 0;
    std::string envSpec_;
    bool envApplied_ = false;
};

}  // namespace foxsdr::slintkit
