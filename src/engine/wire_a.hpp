// wire_a.hpp - WIRING LANE wire-a: the engine side of the tuning, recording and display models (docs/multi-radio-records/wire-a.md).
//
// ONE class, a friend of Engine, so that the lane's state and hooks live in new files and the shared engine files carry only a few bracketed lines:
//   engine.hpp            the friend declaration and the holder (`wireA_`),
//   engine.cpp            one dispatch line in applyCommand (every op 0x8800-0x88FF), one call per control pass (`pass`),
//   engine_api_lists.cpp  the facts and their fingerprint.
// Everything runs on the control thread (applyCommand, the pass, the facts), so nothing here is locked.
//
// PERSISTENCE: each feature keeps one small file in the per-user folder, beside markers.json (so a hermetic engine, which has no such folder, keeps memory only and never
// touches the disk). Writes go to a BackgroundSaver per file (never the control thread).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_ENGINE_WIRE_A_HPP
#define CASCADE_ENGINE_WIRE_A_HPP

#include <array>
#include <atomic>
#include <limits>
#include <utility>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/background_saver.hpp"
#include "core/bookmark.hpp"
#include "core/bookmark_notes.hpp"
#include "core/channel_plans.hpp"
#include "core/disk_job.hpp"
#include "core/rec_daily.hpp"
#include "core/recorder.hpp"
#include "core/favourite_state.hpp"
#include "core/scan_plan.hpp"
#include "core/scan_trigger.hpp"
#include "core/scanner_params.hpp"
#include "core/freq_history.hpp"
#include "foxsdr_api.h"

namespace cascade::engine {

class Engine;

class WireA {
public:
    explicit WireA(Engine& e) : e_(e) {}

    // The lane's state for one engine, made at first use.
    static WireA& of(Engine& e);

    // Applies one op of the block (the caller has checked the range, the grants and the radio). The result is `res` with the status, the applied values and
    // the refusal flag and message set.
    FoxCommandResult apply(const FoxCommand& c, const std::string& text, FoxCommandResult res);

    // Once per control pass, after the plugins and the scanner: records, ticks, saves.
    void pass();

    // Writes what is dirty and waits up to `bound` for the lane's writes (the engine's exit drain). False when a write was abandoned.
    bool drain(std::chrono::milliseconds bound);

    // The lane's facts (LOCAL: they can name a frequency), and the counter that moves when any of them does.
    void facts(const std::function<void(const std::string&, const std::string&)>& fact) const;
    std::uint64_t version() const { return version_; }

private:
    friend class WireATestAccess;

    using Result = FoxCommandResult;
    static Result refused(Result r, std::int32_t status, const std::string& why);

    // ---- persistence (wire_a.cpp) ----
    // The per-user folder (beside markers.json); empty for a hermetic engine.
    std::string dir() const;
    // Writes `text` to <dir>/<name> on a worker; a no-op when there is no folder.
    void saveFile(const std::string& name, const std::string& text);
    // The file's text, or "" when there is none (or no folder).
    std::string readFile(const std::string& name) const;
    void drainSaves();
    void bump() { ++version_; }

    // ---- tuning (wire_a_tuning.cpp) ----
    Result historyStep(const FoxCommand& c, Result res);
    void historyPass();
    void historyFacts(const std::function<void(const std::string&, const std::string&)>& fact) const;

    Result channelOp(const FoxCommand& c, const std::string& text, Result res);
    Result keyActionOp(const FoxCommand& c, const std::string& text, Result res);
    void channelFacts(const std::function<void(const std::string&, const std::string&)>& fact) const;
    // fact.tuned.<n>.hz / .mode / .bandwidthHz: where each radio is (the engine's state in words a screen and a test can read without the state block).
    void tunedFacts(const std::function<void(const std::string&, const std::string&)>& fact) const;

    // ---- files of the user's choosing: markers, packs, notes ... (wire_a_files.cpp) ----
    Result markerIoOp(const FoxCommand& c, const std::string& text, Result res);
    void filesFacts(const std::function<void(const std::string&, const std::string&)>& fact) const;
    std::string markerIoLast_;   // fact.markerio.last: what the last export / import did, in words (no path)
    std::string markerIoError_;

    // ---- bookmarks: notes and tags (wire_a_bookmarks.cpp) ----
    Result bookmarkNoteOp(const FoxCommand& c, const std::string& text, Result res);
    Result bookmarkFindOp(const FoxCommand& c, const std::string& text, Result res);
    void bookmarksPass();
    void bookmarkFacts(const std::function<void(const std::string&, const std::string&)>& fact) const;
    void bookmarksLoadOnce();
    void notesChanged();
    // The bookmark with this id (nullptr when there is none).
    const cascade::core::Bookmark* bookmarkById(std::uint64_t id) const;
    cascade::core::tuning::BookmarkNotes notes_;
    bool bookmarksLoaded_ = false;
    bool notesDirty_ = false;
    double notesDueS_ = 0.0;
    unsigned bookmarksSeenVersion_ = 0;
    std::map<std::uint64_t, cascade::core::tuning::BookmarkNotes::Key> bookmarkKeys_;   // id -> the key its note is under, as of the last look
    std::string noteLast_, noteLastId_, findLast_;
    std::string notesError_;

    // ---- packs (wire_a_bookmarks.cpp) ----
    Result packOp(const FoxCommand& c, const std::string& text, Result res);
    std::string packLast_;

    // ---- favourites (wire_a_bookmarks.cpp) ----
    Result favouriteOp(const FoxCommand& c, const std::string& text, Result res);
    void favouritesChanged();
    cascade::core::tuning::FavouriteStore favourites_;
    bool favouritesDirty_ = false;
    double favouritesDueS_ = 0.0;
    std::string favouriteRecalled_, favouritesError_;

    // ---- the one background job (wire_a_jobs.cpp) ----
public:
    struct JobResult {
        bool ok = false;
        bool cancelled = false;
        std::string message;   // words, never a path
        std::string json;      // the job's own numbers
    };
    struct JobCtl {
        std::atomic<bool> cancel{false};
        std::atomic<double> progress{0.0};
    };
    ~WireA();

private:
    Result jobOp(const FoxCommand& c, const std::string& text, Result res);
    // Starts `work` on the worker; BUSY while a job is out. `name` is a fixed English word.
    Result startJob(Result res, const char* name, std::function<JobResult(JobCtl&)> work);
    void jobPass();
    void jobFacts(const std::function<void(const std::string&, const std::string&)>& fact) const;
    cascade::core::DiskJob<JobResult> job_{"wire-a job"};
    std::shared_ptr<JobCtl> jobCtl_;
    std::string jobName_, jobState_ = "idle", jobMessage_, jobJson_;

    // ---- records: analyser project, attachments, SigMF annotations (wire_a_records.cpp) ----
    Result recordsOp(const FoxCommand& c, const std::string& text, Result res);

    // ---- recording: daily folders (wire_a_recording.cpp) ----
    Result dailyOp(const FoxCommand& c, const std::string& text, Result res);
    void recordingFacts(const std::function<void(const std::string&, const std::string&)>& fact) const;
    void dailyLoadOnce();
    std::string dailyJson() const;
    cascade::core::recdaily::Options daily_ = [] { cascade::core::recdaily::Options o; o.style = cascade::core::recdaily::Style::None; return o; }();
    bool dailyLoaded_ = false;

public:
    // The take's place: its folder is the day's folder below the recordings folder (engine.cpp's applyRecordName calls it first).
    void applyDaily(cascade::core::Recorder::OpenRequest& req);

private:
    // ---- the scanner (wire_a_scan.cpp) ----
    Result scanOp(const FoxCommand& c, const std::string& text, Result res);
    Result scanPlanOp(const FoxCommand& c, const std::string& text, Result res);
    Result scanExtrasOp(const FoxCommand& c, const std::string& text, Result res);
    void scanPass();
    void scanFacts(const std::function<void(const std::string&, const std::string&)>& fact) const;
    void scanLoadOnce();
    void scanSave();
    bool scanLoaded_ = false;
    bool scanDirty_ = false;
    double scanDueS_ = 0.0;
    cascade::core::tuning::ScanPlan plan_;
    cascade::core::tuning::ScanTrigger trigger_;
    cascade::core::tuning::ScanCursor cursor_;
    bool planOn_ = false;           // the running (or last started) scan is the plan's
    bool triggerOn_ = false;
    double rotateToHz_ = 0.0;       // SCAN_BACK: the next configure starts the list at this point
    std::vector<double> priorityHz_, lockedHz_;
    double priorityIntervalMs_ = 0.0;
    bool muteWhileScanning_ = false;
    std::vector<std::pair<double, double>> thresholds_;   // (Hz, dB)
    std::string scanError_;

public:
    // The engine's hooks into the scanner (each a bracketed line in engine.cpp / engine_airband.cpp):
    // the parameters the scanner is configured with, adjusted for the plan and the extras;
    void adjustScannerParams(cascade::core::ScannerParams& p);
    // whether the signal counts as present: the squelch's answer, or the trigger's when it is on;
    bool scanOpen(bool squelchOpen, long long slot, double levelDb);
    // the scanner's own retune (a plan scan tunes the mode and bandwidth of the range it is in);
    void scanTune(double hz);
    // another kind of scan was started: plan mode ends;
    void planOff() { planOn_ = false; }
    // an I/Q file was installed as radio `slot`'s source (0-based); whether its centre came from its own header.
    void iqFileOpened(std::size_t slot, bool centreFromHeader) { if (slot < fileHeaderCentre_.size()) { fileHeaderCentre_[slot] = centreFromHeader; bump(); } }
    // the trigger's floors belong to the scan that was configured: a new configuration starts them again.
    void scanConfigured() { trigger_.reset(); }

private:
    // ---- shared helpers (wire_a.cpp) ----
    // Applies an engine op to the selected radio as a sub-command (the clamping and the bookkeeping of the op itself); the caller is inside the radio's RadioWork.
    Result subCommand(std::uint32_t op, double n0, long long i0, long long i1 = 0, double n1 = 0.0);
    // A full path the user typed (absolute, no control characters, under 1024 bytes). False with `why` when it is not.
    static bool fullPath(const std::string& p, std::string& why);
    // Reads a user's file up to maxBytes (a bigger one, a directory, a device, a missing one: false with `why`; never throws).
    static bool readUserFile(const std::string& path, std::size_t maxBytes, std::string& text, std::string& why);
    // Writes a new file (temp + rename, directory made); a file that exists is refused.
    static bool writeNewFile(const std::string& path, const std::string& text, std::string& why);
    // Tunes the selected radio (the caller is inside its RadioWork): the mode by name (""/unknown = keep), then the absolute frequency, then the bandwidth (0 = keep).
    void tuneTo(double hz, const std::string& mode, double bandwidthHz);

    Engine& e_;
    std::uint64_t version_ = 1;
    std::map<std::string, std::unique_ptr<cascade::gui::BackgroundSaver>> savers_;

    struct History {
        cascade::core::tuning::FreqHistory list;
        double seenHz = 0.0;   // the frequency the pass last looked at: a change from it is a retune
        bool seen = false;
    };
    std::array<History, 8> history_;
    std::array<bool, 8> fileHeaderCentre_{};
    std::array<cascade::core::tuning::ChannelCursor, 8> channel_;
};

}  // namespace cascade::engine

#endif  // CASCADE_ENGINE_WIRE_A_HPP
