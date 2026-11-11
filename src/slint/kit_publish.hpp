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
#include <future>
#include <optional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <slint.h>

#include "main.h"
#include "globe_publish.hpp"
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
    // THE DISPLAY SETTINGS THE MAPS APPLY (SYSTEM > Display: Aircraft icons, Trail width, the trail style), already bounded (slintmap::mapLookOf) from the interface
    // half's aircraftIconPx, mapTrailWidthPx and mapTrailStyle: handed in every tick, applied to every map page's compositor and targets at the next publish.
    void setMapLook(const slintmap::MapLook& look) { look_ = look; }
    // The radios that are open (bit n-1 = radio n), handed in every tick: the DECODE bank's entries offer the Runs-on picker when more than one is (screen stage S6, design 3.7).
    void setRadios(std::uint32_t openMask) { radioMask_ = openMask; }
    // slint-gaps.md item 5: the map pages' Line / Ribbon keys ask the host to save mapTrailStyle (0 | 1) in the interface half; the look follows from the next setMapLook
    void setTrailStyleSink(std::function<void(int)> f) { trailStyleSink_ = std::move(f); }
    // slint-gaps.md item 7: a bounded run's answer for the BROWSE FILES key (FOXSDR_FILE_PICK): no dialog, the answer is this path (empty = a cancel)
    void setFilePickSeam(const std::string& path) { filePickSeam_ = path; }
    // A bounded run's self-capture (FOXSDR_SLINT_DECODE_DRAWER=1): the DECODE bank's "Turn on and off plugins" drawer open, whatever language the row's label is in.
    void openPluginDrawerForCapture() { decodeOpen_["plugins"] = true; }
    const slintmap::MapLook& mapLook() const { return look_; }
    // THE 3D VIEWS (globe_publish.hpp, docs 6.65): the state the drive scripts read ("globe.<propagation|ft8|goes|solar|satellites>", "globe.status").
    std::string globeState(const std::string& what) const;
    // The two ends of the framework's view3d setting, for what the globe views remember (main.cpp gives them the interface half).
    void setViewPrefsHooks(std::function<std::string()> load, std::function<void(const std::string&)> save);
    // The count of open pages (for the bounded run's evidence).
    std::size_t pageCount() const { return pages_.size(); }
    std::size_t openMapPages() const;
    std::string pagesLine() const;   // "pages: output, APT image(torn)" for the bounded run
    // A bounded run's self-capture (FOXSDR_SLINT_OPEN="output,fitted,panel:<name>,instrument:<name>|<title>,image:<name>,map:<name>,module:<file>"):
    // the pages named are opened on the first publish that knows the plugins, the last one named is shown. Never set in a release.
    void openFromEnv(const std::string& spec);

    // THE PLUGIN STORE PAGE (kind 9, id "store"; docs 6.42). The KIT OWNS THE PAGE - its PageTab, its place in the tab row, its tear-off, the DECODE bank's row that opens
    // it - and the EXTEND publisher (extend_publish.*) owns what the page SHOWS: it computes the whole ::StoreModel from extend_logic's read and hands it over here
    // (setStoreModel), reads whether the page is up (storeView) so it reads the store's lists only while someone can see them, and hands over the DECODE row's chip
    // (setStoreRail: BUSY / IDLE / "N UPD" / OK, extend_logic.hpp readStoreChip). The kit never reads the store's facts and the EXTEND publisher never touches a Page.
    enum class StoreView { Closed, Docked, Shown };   // not open; open as a tab that is not the one showing; showing (the current tab or torn off)
    StoreView storeView() const;
    void setStoreModel(const ::StoreModel& model);
    void setStoreRail(const std::string& word, ::ChipTone tone, bool lit);
    // Opens the store page and shows it (the DECODE row's key, a bounded run's FOXSDR_SLINT_OPEN=store).
    void openStorePage();

    // THE PATCH PRESETS PAGE (kind 10, id "patch-presets"; docs 6.44), the same seam as the store's. The KIT owns the page - its PageTab, its tab, its tear-off; the PATCH publisher
    // (patch_publish.*) computes the whole ::PatchPresetsModel and hands it over (setPatchPresetsModel), reads whether the page is up (patchPresetsView) and opens and closes it
    // from the band's PRESETS key. The ImGui window keeps this page a floating window (440 x 460, app_window_patch_presets.cpp), so it OPENS TORN OFF: the patch view stays the one
    // showing (a docked page would take the centre, and the engine stops a running patch the moment the patch view is not showing). Dock it from its chrome to make it a tab.
    enum class PatchPresetsView { Closed, Docked, Shown };
    PatchPresetsView patchPresetsView() const;
    void setPatchPresetsModel(const ::PatchPresetsModel& model);
    void openPatchPresetsPage();
    void closePatchPresetsPage();

    // THE TRANSMIT PAGE (kind 11, id "transmit"; docs 6.58), the same seam. The KIT owns the page - its PageTab, its tab, its tear-off; the TRANSMIT publisher (transmit_publish.*)
    // computes the whole ::TransmitPageModel and hands it over (setTransmitModel), and reads whether the page is up. THE PAGE IS LIVE (its controls are on screen, so the key may be
    // held) only while it is Shown and not rolled up (transmitLive): a page that is closed, a tab that is not the one showing, or a torn page rolled up holds nothing. The ImGui
    // window keeps this page a floating 560 x 640 window (app_window.cpp drawTransmitPage), so it OPENS TORN OFF at that size and leaves the view showing as it was.
    enum class TransmitView { Closed, Docked, Shown };
    TransmitView transmitView() const;
    bool transmitLive() const;
    void setTransmitModel(const ::TransmitPageModel& model, const std::string& chip, ::ChipTone tone);
    void openTransmitPage();
    void closeTransmitPage();

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
    void fillStore(PageTab& t, std::string& sig);
    void fillPatchPresets(PageTab& t, std::string& sig);
    void fillTransmit(PageTab& t, std::string& sig);
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
    std::uint32_t radioMask_ = 1u;                    // the open radios (setRadios)
    slintmap::MapLook look_;                          // what the maps are drawn with: the classic window's standard until the first setMapLook
    std::function<void(int)> trailStyleSink_;
    std::optional<std::string> filePickSeam_;
    std::future<std::string> saveJob_;                // item 4: the picture being written on a worker (one at a time); its sentence is the page's note when it is back
    std::string savePlugin_;
    void pollSave();
    void browseSetting(const std::string& file, int id);
    foxsdr::slinthost::Signatures sigs_;
    std::unique_ptr<GlobePublisher> globe_;               // the 3D views of the plugin pages that offer one (docs 6.65)
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
    ::StoreModel storeModel_{};                       // what the EXTEND publisher last handed over for the page of kind 9
    ::PatchPresetsModel patchPresetsModel_{};         // what the PATCH publisher last handed over for the page of kind 10
    std::uint64_t patchPresetsRevision_ = 0;
    ::TransmitPageModel transmitModel_{};             // what the TRANSMIT publisher last handed over for the page of kind 11
    std::uint64_t transmitRevision_ = 0;
    std::string transmitChip_;
    ::ChipTone transmitChipTone_ = ::ChipTone::Neutral;
    std::uint64_t storeRevision_ = 0;                 // moves with each handed-over model: the page's signature
    std::string storeChip_;                           // the DECODE row's chip ("" until the EXTEND publisher's first read: the row says IDLE, the word nothing has been asked)
    ::ChipTone storeChipTone_ = ::ChipTone::Neutral;
    bool storeLit_ = false;
    FoxReceiverState state_{};
    std::int64_t nowMs_ = 0;
    std::string envSpec_;
    bool envApplied_ = false;
};

}  // namespace foxsdr::slintkit
