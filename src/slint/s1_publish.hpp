// s1_publish.hpp - SCREEN LANE S1's PUBLISHER (wave 8; docs/multi-radio-records/screen-s1-signal.md): the LaneS1 global of the Slint window filled and its act(id, text) answered, over the pure rules of
// s1_logic.hpp.
//
// Three row lists, each drawn by a KPanel appended to the body of the SIGNAL section the person looks in (main.slint): the I/Q correction (Source), the meter's unit, the CW pitch and peak filter, the
// passband's presets and shape, the squelches, AFC and the multiplex record (Radio), the equaliser, the audio high-pass and low-pass, the noise-reduction method, the notch bank and the stereo placement
// (Audio filters). Every control shows what the engine's state of the SELECTED radio says now (read again each tick, so a change made by a bookmark, the old window or the web page shows) and keeps no copy
// of its own; what a person has typed into a field and not sent is a draft that is never saved. An operation the engine refuses is shown with the engine's own sentence. Each group is folded away until it
// is asked for, so the sections stay as short as they were. Nothing here keys a transmitter.
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

namespace foxsdr::slints1 {

// What one submit came to.
struct SubmitOutcome {
    std::uint64_t ticket = 0;      // 0 = the engine never took it
    int status = FOXAPI_OK;        // what submit answered
    std::string message;           // the host's own words when the call refused it
};

class S1Publisher {
public:
    struct Env {
        MainWindow* ui = nullptr;
        const FoxEngineApi* api = nullptr;
        FoxSession* session = nullptr;
        bool bounded = false;
        // One command; `radio` is the radio's number (1-based) it is aimed at, 0 = the selected radio.
        std::function<SubmitOutcome(const FoxCommand&, std::uint32_t radio, const std::string* longText)> submit;
        std::function<int()> selectedRadio;       // 1-based
    };
    explicit S1Publisher(Env env);
    ~S1Publisher();
    S1Publisher(const S1Publisher&) = delete;
    S1Publisher& operator=(const S1Publisher&) = delete;

    void wire();                                                    // registers LaneS1.act (once, after the window is created)
    void tick(const FoxReceiverState& state, long tickNo);          // refreshes the rows whose facts moved
    void onAnswer(const FoxCommandResult& r);                       // the engine's answer to an op this publisher sent (others are ignored)
    // `state s1.<what>` for the drive scripts: sent | refused | answered | note.<section> | rows.<section> | draft.<id> | fact.<name> | state.<engine field>
    std::string readState(const std::string& what, std::string& out) const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace foxsdr::slints1
