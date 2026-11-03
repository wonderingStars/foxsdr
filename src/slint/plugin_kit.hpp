// plugin_kit.hpp - THE PLUGIN KIT'S HOST SIDE (docs/engine-stage4.md 6.37): the Slint window's half of the plugin UI plane. It owns the
// PluginUiPort the engine hands out through get_local_extension(FOXAPI_EXT_PLUGIN_UI), the GL-free basemap client and the track-info
// cache the directive's `after` step attaches, the pictures the engine accumulates, the plugin lists and the decoder-output tail read
// from the table, the map pages' book-keeping and the operator's choices (which windows are shown, which target is followed), and it
// performs the front end's obligations once a tick, in the ImGui frame's order (app_window.cpp drawUi 3173-3619): applyNewestDirective,
// poll, pollImages, the lists when listSeq moved, setFrontEnd (the counter's face, the basemap's figures, the track feed), serveTiles
// for the browser's map, the track-info plugin's status lines as FOXAPP_OP_TRACK_INFO_LINE, the plugin-use table as FOXAPP_OP_PLUGIN_USE
// when it changed, the two events the window subscribes to, and the answers to its tickets. Every command goes through the one table
// (submit / submit_text); nothing of a plugin is called except the basemap and track-info tables the port lent, through their guards.
//
// WHAT IS NOT HERE: Slint. The page models are built from this object's state by kit_publish.cpp; the pure rules are kit_logic.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include "foxsdr_api.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/app_commands.hpp"
#include "core/plugin_fault.hpp"
#include "core/plugin_ui_port.hpp"
#include "gui/track_info_cache.hpp"
#include "gui/track_metrics.hpp"
#include "kit_logic.hpp"

namespace foxsdr::slintkit {

// The basemap plugin's table as the Slint window holds it: BasemapCache (gui/basemap_cache.cpp) without the GL textures - the plugin is
// asked for packed RGB rows through its guard, and the compositor keeps its own CPU cache. Attached from the directive's `after` step,
// detached from a release, on the attaching thread.
class BasemapClient {
public:
    BasemapClient() = default;
    ~BasemapClient();
    BasemapClient(const BasemapClient&) = delete;
    BasemapClient& operator=(const BasemapClient&) = delete;
    void attach(const CascadeBasemapApi* api, const std::string& pluginKey);
    void detach();
    bool active() const { return api_ != nullptr && handle_ != nullptr; }
    const std::string& attribution() const { return attribution_; }
    std::uint32_t minZoom() const { return minZoom_; }
    std::uint32_t maxZoom() const { return maxZoom_; }
    std::uint32_t tileSize() const { return tileSize_; }
    // CASCADE_TILE_READY with `rgb` filled (tileSize * tileSize * 3), PENDING (asking started the fetch), MISSING.
    std::int32_t rawTile(std::uint32_t z, std::uint32_t x, std::uint32_t y, std::vector<std::uint8_t>& rgb);
    std::vector<std::string> drainText();

private:
    const CascadeBasemapApi* api_ = nullptr;
    void* handle_ = nullptr;
    cascade::core::fault::Slot* slot_ = nullptr;
    std::string attribution_;
    std::uint32_t minZoom_ = 0, maxZoom_ = 19, tileSize_ = 256;
    std::vector<char> pollBuf_;
    std::string partial_;
};

// The table, live: the C API through the window's session.
class LiveTable : public TableReader {
public:
    LiveTable(const FoxEngineApi* api, FoxSession* session) : api_(api), session_(session) {}
    std::int32_t readList(std::uint32_t list, std::uint32_t index, FoxListItem* out) override;
    bool fact(const std::string& key, std::string& out) override;

private:
    const FoxEngineApi* api_;
    FoxSession* session_;
};

// A plugin's preset was applied (the engine's event): which windows to open is decided by the publisher, which knows the pages.
struct KitEvents {
    std::vector<std::string> presetApplied;    // module files, in order
    bool muteTuneAway = false;                 // the "Sound is muted" question is to be asked
    std::vector<std::string> notices;          // the basemap's and the track-info plugin's status lines, for the log
};

class PluginKit {
public:
    struct Env {
        const FoxEngineApi* api = nullptr;
        FoxEngine* engine = nullptr;
        FoxSession* session = nullptr;
        std::string configPath;                // where the saved pictures go (beside the settings file)
    };
    explicit PluginKit(const Env& env);
    ~PluginKit();
    PluginKit(const PluginKit&) = delete;
    PluginKit& operator=(const PluginKit&) = delete;

    // get_local_extension on the attaching thread, after restore() and before start(). False: no plugin UI (a null port stands in).
    bool attach();
    bool attached() const { return attached_; }
    // The tick's duties, in the frame's order. `tunerFace` is the counter's face word for the web page ("plain", "neon", "nixie").
    void tick(const FoxReceiverState& state, bool listSeqMoved, const std::string& tunerFace);
    // The interface's own release of everything borrowed from the modules, before the service stops.
    void release();

    // ---- what the publisher reads ----
    const KitLists& lists() const { return lists_; }
    const std::vector<cascade::core::HostTrack>& tracks() const { return port_->tracks(); }
    const std::vector<cascade::core::HostTrack>& unplacedTracks() const { return port_->unplacedTracks(); }
    const std::vector<cascade::core::HostPath>& paths() const { return port_->paths(); }
    const std::vector<cascade::core::HostPanel>& panels() const { return port_->panels(); }
    const std::vector<cascade::core::HostInstrument>& instruments() const { return port_->instruments(); }
    const std::vector<std::string>& trackPluginNames() const { return port_->trackPluginNames(); }
    bool altitudeNear(const std::string& plugin, const std::string& id, double latDeg, double lonDeg, double& outAltM) { return port_->altitudeNear(plugin, id, latDeg, lonDeg, outAltM); }
    const std::vector<cascade::core::HostImage>& images() const { return images_; }
    const cascade::core::HostImage* imageOf(const std::string& plugin) const;
    const cascade::core::HostPanel* panelOf(const std::string& plugin, const std::string& title) const;
    const cascade::core::HostInstrument* instrumentOf(const std::string& plugin, const std::string& title) const;
    const std::deque<DecoderLine>& decoderLines() const { return lines_; }
    std::uint64_t decoderLinesRead() const { return linesSeenTotal_; }
    void clearDecoderLines() { lines_.clear(); }
    std::vector<MapPage>& mapPages() { return pages_; }
    BasemapClient& basemap() { return basemap_; }
    cascade::gui::TrackInfoCache& trackInfo() { return trackInfo_; }
    cascade::gui::CoverageMap& coverage() { return coverage_; }
    OpenWindows& open() { return open_; }
    SettingsEdits& edits() { return edits_; }
    KitEvents takeEvents();                           // drained once a tick by the publisher
    // The answers the kit's own poll took that are not its settings edits' (a graph commit's, the radio scan's): main.cpp polls first each tick, but an answer that lands between
    // that poll and the kit's is taken here, and the patch publisher's draft machine waits for exactly such an answer, so they are kept (the newest 64) and handed on.
    std::vector<FoxCommandResult> takeUnclaimedResults();
    const FoxReceiverState& state() const { return state_; }
    std::uint64_t instrumentSeen(const std::string& key) const;
    void markInstrumentSeen(const std::string& key, std::uint32_t seq);

    // ---- commands, each one submit through the table (the ticket, 0 when the session refused it) ----
    std::uint64_t submit(const cascade::core::cmd::QueuedCommand& q);
    std::uint64_t submit(const FoxCommand& c) { return submit(cascade::core::cmd::QueuedCommand{c, std::string()}); }
    void startStop(const std::string& file, bool stop);
    void stopList(const std::vector<std::string>& files, bool endMute);
    void applyPreset(const std::string& file, const Preset& p);        // FOXAPI_OP_PLUGIN_PRESET or FOXAPP_OP_USER_PRESET_APPLY
    void forgetPreset(const std::string& file, const Preset& p);       // FOXAPP_OP_USER_PRESET_FORGET_AT
    void savePreset(const std::string& file);                          // FOXAPI_OP_USER_PRESET_SAVE
    void setMute(const std::string& file, bool mutes);
    void setGrant(const std::string& file, int kind, bool on);         // 1 tune, 2 settings (FOXAPI_OP_PLUGIN_GRANT), 3 transmit (FOXAPP_OP_PLUGIN_TX_GRANT)
    void pressCommand(const std::string& file, std::uint32_t id);      // FOXAPI_OP_PLUGIN_COMMAND
    void setRunMode(const std::string& file, int mode);                // FOXAPP_OP_PLUGIN_RUN_MODE 0 auto 1 always 2 stopped
    void reenable(const std::string& file);
    void rescan();
    void removeBlocked(const std::string& file);
    void updateBlocked(const std::string& catalogueId);
    void autoPreset(const std::string& file);                          // a window was opened by its row: FOXAPP_OP_PLUGIN_AUTO_PRESET
    void imagePress(const std::string& plugin, int x, int y);
    void keepMuteRunning();                                            // the question's "Keep it running"
    void setCatalogueUrl(const std::string& url);
    // The settings form: the edit sent with its ticket; the answer lands in edits() by that ticket.
    void editSetting(const std::string& file, const SettingSpec& f, const std::string& typed);
    // Saves the plugin's newest picture as a BMP beside the settings file; the sentence for the save note.
    std::string savePicture(const std::string& plugin);

private:
    void directiveBefore(const cascade::core::PluginDirectiveFacts& d);
    void directiveAfter(const cascade::core::PluginDirectiveFacts& d);
    void handFrontEnd(const std::string& tunerFace);
    void serveWebTiles();
    void sendTrackInfoLines();
    void reportPluginUse();
    void pollEvents();
    void pollResults();
    bool fillTrackFeed(cascade::core::TrackFeed& out);

    Env env_;
    LiveTable table_;
    std::unique_ptr<cascade::core::PluginUiPort> port_;
    bool attached_ = false;
    BasemapClient basemap_;
    cascade::gui::TrackInfoCache trackInfo_;
    cascade::gui::CoverageMap coverage_;
    std::vector<cascade::core::HostImage> images_;
    KitLists lists_;
    std::deque<DecoderLine> lines_;
    std::uint64_t linesLastId_ = 0, linesSeenTotal_ = 0;
    std::vector<MapPage> pages_;
    OpenWindows open_;
    SettingsEdits edits_;
    KitEvents events_;
    std::vector<FoxCommandResult> unclaimed_;
    FoxReceiverState state_{};
    cascade::core::PluginUiFrontEnd frontEnd_;
    std::string useSent_;
    bool webTilesActive_ = false;
    std::string webTileAttribution_;
    std::deque<std::string> trackInfoLines_;
    std::size_t trackInfoLinesLost_ = 0;
    std::map<std::string, std::uint32_t> instrumentSeen_;
    int busyDrops_ = 0;
};

}  // namespace foxsdr::slintkit
