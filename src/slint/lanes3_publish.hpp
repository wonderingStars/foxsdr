// lanes3_publish.hpp - SCREEN LANE S3's PUBLISHER (docs/multi-radio-records/screen-s3-recplay.md): the LaneS3 global of the Slint window filled and its act(id, text) answered, over the pure rules of lanes3_logic.hpp.
//
// Three row lists, each drawn by a KPanel appended to the body of the section the person looks in (main.slint): SIGNAL > Recorder (file names, sample formats, the passband take, rollover, triggered
// clips, the replay buffer), SIGNAL > Recorder again (the timed recordings) and SIGNAL > Source (where in the recording, the loop, a raw file's stated format).
// Every control shows what the ENGINE says now (a fact, or the radio's state) and keeps only what the person has typed and not sent (a draft, never saved). An operation the engine refuses is shown as
// refused with the engine's own sentence. Nothing here opens a radio or keys a transmitter.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <slint.h>

#include "foxsdr_api.h"
#include "main.h"

namespace foxsdr::slintlanes3 {

struct SubmitOutcome {
    std::uint64_t ticket = 0;      // 0 = the engine never took it
    int status = FOXAPI_OK;
    std::string message;           // the host's own words when the call refused it
};

// What the selected radio's state says that the rows need (read from FoxRadioState once a tick).
struct PlayState {
    bool filePlays = false;
    double positionS = 0.0, durationS = 0.0, loopStartS = 0.0, loopEndS = 0.0, startUnixMs = 0.0;
    double sampleRateHz = 0.0;
    int recIq = 0, recAudio = 0;
};

class LaneS3Publisher {
public:
    struct Env {
        MainWindow* ui = nullptr;
        const FoxEngineApi* api = nullptr;
        FoxSession* session = nullptr;
        bool bounded = false;
        std::function<SubmitOutcome(const FoxCommand&, std::uint32_t radio, const std::string* longText)> submit;
        std::function<int()> selectedRadio;            // 1-based
        std::function<PlayState()> playState;          // the selected radio's
        std::function<std::int64_t()> nowUnix;         // seconds; the clock (a test sets it)
        std::function<int(std::int64_t unixMs)> offsetAt;   // the local zone's offset east of UTC in minutes at an instant (a test fixes it)
    };
    explicit LaneS3Publisher(Env env);
    ~LaneS3Publisher();
    LaneS3Publisher(const LaneS3Publisher&) = delete;
    LaneS3Publisher& operator=(const LaneS3Publisher&) = delete;

    void wire();                                   // registers LaneS3.act (once, after the window is created)
    void tick(long tickNo);                        // refreshes the rows (a few times a second)
    void onAnswer(const FoxCommandResult& r);      // the engine's answer to an op this publisher sent (others are ignored)
    // `state s3.<what>` for the drive scripts
    std::string readState(const std::string& what, std::string& out) const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace foxsdr::slintlanes3
