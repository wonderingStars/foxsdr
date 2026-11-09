// tools_publish.hpp - THE TOOLS BANK'S PUBLISHER (docs/engine-stage4.md, the Slint port, phase 3 bank 3): the UI family's models (ui/tools_bank.slint: RadarModel,
// BookmarksModel, ScannerModel, AirbandModel, MarkersModel) filled once a tick from the host's pure logic (tools_logic.hpp: readTools over the C API table, the heard
// aircraft of the plugin kit, the GPS row of the SYSTEM bank) and set only when a model differs from the one last set, and every callback of the ToolsEvents global
// turned into the Effect the logic names for it and EXECUTED here: the commands through the plugin kit's one submit (the long ADD_ROWS text through its long-text
// channel), the local state this object owns (the drafts, the filters, the open menu, the Markers sheet, the airband lists and notes), the two disk workers (the
// lookup of a bookmark import path with no root, the read of a preset's frequency list), the answers the airband section waits for (airbandOnAnswer), the paced
// Tick all run and the clipboard.
//
// THE PATTERN IS signal_publish.hpp / system_publish.hpp's: the constructor takes the window and the kit, wire() registers the callbacks once, publish(state)
// runs after the kit's tick (and after the SYSTEM publisher's, whose GPS row it is handed), onAnswer(result) is called for each answer main.cpp polls. Lists are
// persistent VectorModels updated IN PLACE (a replaced list model recreates every row behind it and closes an open dropdown); a struct is set only when it differs
// from the one last set (the generated structs carry operator==).
//
// THE RADAR FOLD'S GPS ROW: gps-port, gps-baud and gps-toggle are the SystemEvents callbacks (RadarBody forwards them there) and the SystemPublisher already wires
// them; this object only READS the row (SystemPublisher::models().serial.gps, handed to publish()) - the three effects are not duplicated.
//
// A BOUNDED RUN NEVER SENDS OR OPENS: the clipboard is not written and a folder is not opened (the constructor's `bounded` flag).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <slint.h>

#include "main.h"
#include "plugin_kit.hpp"
#include "tools_logic.hpp"

namespace foxsdr::slinttools {

class ToolsPublisher {
public:
    struct Hooks {
        std::function<void()> openImportDialog;   // the SIGNAL bank's "Import a frequency list" dialog (the Bookmarks Import key with an empty box)
        // THE TWO SPECTRUM OPTIONS OF THE BOOKMARKS PANEL (docs 6.52, tick rows 1629-1630): "On the spectrum" and "Stack close names" are the interface
        // half's bookmarkMarkers and bookmarkStackNames, which the classic window saves and reads back at start. Until 6.52 this publisher kept them in
        // its panel options alone, so both reset to on at every launch and the spectrum never read them. The host hands their saved values in here
        // and saves each change through this hook (HostDuties::saveInterfaceHalf); main.cpp reads the half when it builds the spectrum's marks.
        bool onSpectrum = true;
        bool stackNames = true;
        std::function<void(bool onSpectrum, bool stackNames)> saveBookmarkOptions;
    };
    ToolsPublisher(MainWindow& ui, foxsdr::slintkit::PluginKit& kit, const FoxEngineApi* api, FoxSession* session, bool bounded, Hooks hooks);
    ~ToolsPublisher();
    ToolsPublisher(const ToolsPublisher&) = delete;
    ToolsPublisher& operator=(const ToolsPublisher&) = delete;

    // Registers every ToolsEvents callback (once, after the window is created).
    void wire();
    // Reads the models and sets the ones that changed (after kit.tick() and the SYSTEM publisher's publish; `gps` is that publisher's serial.gps).
    void publish(const FoxReceiverState& state, const foxsdr::slintsystem::GpsRow& gps);
    // An answer main.cpp polled: the ones this object waits for (an airband ask, a chunk of a preset import) are settled, the rest are not its business.
    void onAnswer(const FoxCommandResult& r);

    // The chip a TOOLS row of bank 2 shows (rows 0 Demod scope, 1 Radar, 2 Bookmarks, 3 Scanner, 4 Airband); `valid` is false before the first publish.
    struct RowFace {
        bool valid = false;
        std::string chip;
        ::ChipTone tone = ::ChipTone::Neutral;
    };
    RowFace face(int row) const;

    // The Markers sheet: opened by the spectrum toolbar's Markers key, the marker menu's "List markers..." and the bounded run's capture. The sheet's own Close
    // key tells the host through markers-closed.
    void openMarkers();
    // The Radar fold was opened: nothing here needs asking (the serial scan is the SYSTEM publisher's, started by the caller).

    // A bounded run's evidence: the bookmark count and its count line, the scanner status, the airband hearing line and row count, the marker count.
    std::string summaryLine() const;
    const ToolsModels& models() const { return last_; }
    std::size_t commandsSent() const { return sent_; }

private:
    struct Lists;
    struct Jobs;
    struct Pending;   // an answer this object waits for

    // ---- commands and effects ----
    std::uint64_t send(const Command& c);
    void exec(const Effect& e, const char* what);
    void applyLocal(const Effect& e);
    std::string fact(const std::string& key);
    double nowS() const;
    void say(const char* line) const;

    // ---- the disk workers, the answers, the paced runs ----
    void startBookmarkLookup(const std::string& path);
    void startPresetImport(const std::string& path, const std::string& preset);
    void pollJobs();
    void sendChunks();
    void sendTicks();
    void pollExport();
    void airbandFrame();
    void airbandShowGroup(const std::string& group);
    void finishPresetImport();
    void setClipboard(const std::string& text);
    void openFolder(const std::string& path);
    void flushSquelch();

    // ---- the reads ----
    ReadOptions buildOptions(const FoxReceiverState& state, const foxsdr::slintsystem::GpsRow& gps);

    // ---- the models ----
    ::RadarModel mapRadar(const RadarModel& m, const foxsdr::slintsystem::GpsRow& gps);
    ::BookmarksModel mapBookmarks(const BookmarksModel& m);
    ::ScannerModel mapScanner(const ScannerModel& m);
    ::AirbandModel mapAirband(const AirbandModel& m);
    ::MarkersModel mapMarkers(const MarkersModel& m);

    MainWindow& ui_;
    foxsdr::slintkit::PluginKit& kit_;
    const FoxEngineApi* api_;
    FoxSession* session_;
    bool bounded_;
    Hooks hooks_;
    foxsdr::slintkit::LiveTable table_;

    ReadOptions opts_;
    ToolsCache cache_;
    ToolsModels last_;
    PositionSeen seen_;
    bool haveModels_ = false;
    FoxReceiverState state_{};
    foxsdr::slintsystem::GpsRow gps_;
    std::size_t sent_ = 0;
    std::int64_t lastReadMs_ = 0;
    bool forceRead_ = true;

    // what a hand is on or an answer is waited for (the ImGui window's own members of the same names)
    double renameWantAt_ = -1.0;                  // when rename_want was set: it is given up on after a few seconds
    double scanDraftAt_[kScannerFieldCount] = {};
    std::string airNoteSeen_;
    std::uint64_t exportSeqSeen_ = 0;
    bool exportSeqKnown_ = false;
    double importSince_ = -1.0, presetImportSince_ = -1.0;   // the clocks of the two "Waiting for the disk" lines
    std::string importNote_;                      // a bookmark import's own failure sentence (the worker found no file)
    std::string againPath_;                       // an Import pressed while a lookup was out: the last one asked for
    bool again_ = false;
    std::string againPresetPath_, againPresetName_;
    bool againPreset_ = false;
    bool airWasWanting_ = false;
    bool squelchPending_ = false;
    Effect squelchEffect_;
    std::int64_t squelchSentMs_ = 0;

    std::unique_ptr<Lists> lists_;
    std::unique_ptr<Jobs> jobs_;
    std::unique_ptr<Pending> pending_;
    // the structs last set (equality is the guard)
    struct Last;
    std::unique_ptr<Last> lastSet_;
};

}  // namespace foxsdr::slinttools
