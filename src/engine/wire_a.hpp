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
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/background_saver.hpp"
#include "core/channel_plans.hpp"
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
    void channelFacts(const std::function<void(const std::string&, const std::string&)>& fact) const;
    // fact.tuned.<n>.hz / .mode / .bandwidthHz: where each radio is (the engine's state in words a screen and a test can read without the state block).
    void tunedFacts(const std::function<void(const std::string&, const std::string&)>& fact) const;

    // ---- files of the user's choosing: markers, packs, notes ... (wire_a_files.cpp) ----
    Result markerIoOp(const FoxCommand& c, const std::string& text, Result res);
    void filesFacts(const std::function<void(const std::string&, const std::string&)>& fact) const;
    std::string markerIoLast_;   // fact.markerio.last: what the last export / import did, in words (no path)
    std::string markerIoError_;

    // ---- shared helpers (wire_a.cpp) ----
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
    std::array<cascade::core::tuning::ChannelCursor, 8> channel_;
};

}  // namespace cascade::engine

#endif  // CASCADE_ENGINE_WIRE_A_HPP
