// lanem_publish.hpp - SCREEN LANE M's PUBLISHER (docs/multi-radio-records/screen-m.md): the LaneM global of the Slint window filled and its act(id, text) answered, over the pure rules of lanem_logic.hpp.
//
// Ten row lists, each drawn by a KPanel appended to the body of the section the person looks in (main.slint): the I/Q file's transport (SIGNAL, Source), the receive clarifier and the symbol rate (SIGNAL, Radio),
// CW surround (SIGNAL, Audio filters), the I/Q take's file kind (SIGNAL, Recorder), the patch edit (SIGNAL, Patch), the airband channel settings (TOOLS, Airband), the level store (TOOLS, Export), the
// stitched sweep and the sweep history (SYSTEM, Sweep), the spectrum's GPU counters (SYSTEM, Diagnostics), a plugin's requests to open a page or a folder (SYSTEM, Needed libraries).
// Every control is OFF or at the engine's default until the person acts; the rows READ the facts the engine names and keep no copy of the engine's state (what the person has typed into a field that is not
// sent yet is kept as a draft and is never saved). An operation the engine refuses is shown as refused with the engine's own sentence. Nothing here keys a transmitter; nothing opens a page or a folder until
// the person presses that request's own key.
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

namespace foxsdr::slintlanem {

// What one submit came to.
struct SubmitOutcome {
    std::uint64_t ticket = 0;      // 0 = the engine never took it
    int status = FOXAPI_OK;        // what submit answered
    std::string message;           // the host's own words when the call refused it
};

class LaneMPublisher {
public:
    struct Env {
        MainWindow* ui = nullptr;
        const FoxEngineApi* api = nullptr;
        FoxSession* session = nullptr;
        bool bounded = false;
        // One command; `radio` is the radio's number (1-based) it is aimed at, 0 = the selected radio. `longText` is the command's text when it is longer than the struct's slot.
        std::function<SubmitOutcome(const FoxCommand&, std::uint32_t radio, const std::string* longText)> submit;
        std::function<int()> selectedRadio;       // 1-based
        std::function<double()> filePositionSec;  // the selected radio's position in the file it plays (its state's filePositionSec)
    };
    explicit LaneMPublisher(Env env);
    ~LaneMPublisher();
    LaneMPublisher(const LaneMPublisher&) = delete;
    LaneMPublisher& operator=(const LaneMPublisher&) = delete;

    void wire();                                  // registers LaneM.act (once, after the window is created)
    void tick(long tickNo);                       // refreshes the rows whose facts moved (a few times a second)
    void onAnswer(const FoxCommandResult& r);     // the engine's answer to an op this publisher sent (others are ignored)
    // `state lm.<what>` for the drive scripts
    std::string readState(const std::string& what, std::string& out) const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace foxsdr::slintlanem
