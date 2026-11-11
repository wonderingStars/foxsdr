// lanec_publish.hpp - SCREEN LANE C's PUBLISHER (docs/multi-radio-records/screen-c.md): the LaneC global of ui/tools_bank.slint filled once a tick and its act(id, text) answered, over the pure rules of lanec_logic.hpp.
//
// What it keeps: the saved record (lanec.json beside the settings file, written a moment after the last change), the bookmark list as the engine publishes it (for the lockouts and the memory of the frequency
// the receiver is on), the scan's watcher and mute follower, the bandwidth memory, the clock and the two sheets (shortcuts, recordings). What it sends: ordinary commands through the window's submit.
//
// A BOUNDED RUN opens no folder and no file and starts no program (the constructor's `bounded`).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <slint.h>

#include "foxsdr_api.h"
#include "lanec_logic.hpp"
#include "main.h"
#include "plugin_kit.hpp"

namespace foxsdr::slintlanec {

// The delta lock's two readings for deltaLockApi (the mask is the publisher's, set when it loads and when the user toggles).
std::uint32_t deltaMaskNow();
int deltaRadioOf(const FoxCommand& c);

class LaneCPublisher {
public:
    struct Env {
        MainWindow* ui = nullptr;
        const FoxEngineApi* api = nullptr;
        FoxSession* session = nullptr;
        std::string storePath;                                   // lanec.json; "" = nothing is saved
        std::string scanLogPath;                                 // scan-log.csv; "" = no log
        bool bounded = false;
        std::function<void(const FoxCommand&, std::uint32_t radio)> submit;   // the window's submit (radio 0 = the selected one)
        std::function<double()> nowMs;                           // a steady clock
        std::function<std::int64_t()> unixNow;
        std::function<void(const std::string& path)> openIq;     // a recording to play as the I/Q file
        std::function<std::string()> recorderDir;                // fact.recorder.dir
        std::function<int()> selectedRadio;                      // 1-based
    };
    explicit LaneCPublisher(Env env);
    ~LaneCPublisher();
    LaneCPublisher(const LaneCPublisher&) = delete;
    LaneCPublisher& operator=(const LaneCPublisher&) = delete;

    // Registers LaneC.act (once, after the window is created).
    void wire();
    // Once a frame, after the engine's state was read.
    void tick(const FoxReceiverState& state, long tickNo);
    // Writes what changed now (the end of the run).
    void flush();

    // the counter's right-click menu and the App menu (main.cpp)
    bool stepLocked() const { return store_.stepLock; }
    bool toggleStepLock();
    bool deltaOn(int radio) const { return radio >= 1 && radio <= 8 && ((store_.deltaMask >> (radio - 1)) & 1u) != 0u; }
    void toggleDelta(int radio);
    bool bwMemoryOn() const { return store_.bwMemory; }
    void toggleBwMemory();
    void openShortcuts(const foxsdr::slintsystem::KeyTable& table);
    void openRecordings();
    bool overlayOpen() const;
    void closeOverlays();

    // `state lanec.<what>` for the drive scripts
    std::string readState(const std::string& what, std::string& out) const;
    // a bounded run's seam: the scan log's path, and what `rec-open` last did
    const std::string& lastOpened() const { return lastOpened_; }

private:
    struct Bookmark { int id = 0; double hz = 0.0; std::string name; bool ticked = false; std::uint32_t mode = 0; double bw = 0.0; };
    void act(const std::string& id, const std::string& text);
    void refreshBookmarks();
    const Bookmark* bookmarkAt(double hz) const;
    void publishMemory(const FoxReceiverState& state);
    void publishScan(const FoxReceiverState& state);
    void publishQuick();
    void save();
    void markDirty() { dirty_ = true; dirtySince_ = tickNo_; }
    void send(FoxCommand c, std::uint32_t radio = 0) const { if (env_.submit) { c.structSize = sizeof(c); env_.submit(c, radio); } }
    std::string factOf(const char* key);

    Env env_;
    Store store_;
    foxsdr::slintkit::LiveTable table_;
    bool dirty_ = false;
    long dirtySince_ = 0;
    long tickNo_ = 0;

    // the bookmark list
    std::vector<Bookmark> bookmarks_;
    std::string bookmarksVersion_;
    long bookmarksAt_ = -1000;
    int memoryBookmark_ = -1;          // index into bookmarks_ of the one the receiver is tuned to, -1 none
    int memoryId_ = 0;                 // its id when we last applied its recall (so a recall is applied once)
    double lastTunedHz_ = 0.0;
    double tunedHz_ = 0.0, bandwidthHz_ = 0.0, scanHz_ = 0.0;   // the last state read
    std::uint32_t mode_ = 0, scanState_ = 0;

    // C3
    ListLockouts lockouts_;
    // C4, C5, F7
    ScanWatcher watcher_;
    ScanMuteFollower mute_;
    BandwidthMemory bandwidth_;
    std::uint32_t lastScanState_ = 0;
    int mutedRadio_ = 1;               // the radio the mute follower silenced
    bool quickStore_ = false;
    // the sheets
    bool shortcutsOpen_ = false;
    std::vector<RecordingFile> recordings_;
    std::string recordingsSig_;
    long recordingsAt_ = -1000;
    std::string lastOpened_;
    std::int64_t lastClockSecond_ = -1;
    std::string clockShown_;
    int logLines_ = 0;
    int fieldsEpoch_ = 0;

    // what was last set (a property is set only when it differs)
    struct Shown;
    std::unique_ptr<Shown> shown_;
    std::shared_ptr<slint::VectorModel<LCQuick>> quickModel_;
    std::shared_ptr<slint::VectorModel<LCShortcut>> shortcutModel_;
    std::shared_ptr<slint::VectorModel<LCRecording>> recordingModel_;
};

}  // namespace foxsdr::slintlanec
