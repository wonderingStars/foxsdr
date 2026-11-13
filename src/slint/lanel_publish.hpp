// lanel_publish.hpp - SCREEN LANE L's PUBLISHER (docs/multi-radio-records/screen-l.md): the LaneL global of the Slint window filled and its act(id, text) answered, over the pure rules of lanel_logic.hpp.
//
// Eleven sections after lane K's in the SYSTEM bank: Tuning and memories, Scanning, Recording and files, Measurement, Sweep, Network radios, Outputs and tracks, Needed libraries, System,
// Web and CAT, Ham tools. Every row is a lane K row (KRow / KPanel); every control is OFF or at the engine's default until the person acts; the rows READ the facts the engine names and keep
// no copy of the engine's state (what the person has typed into a field that is not sent yet is kept as a draft and is never saved). An operation the engine refuses is shown as refused with the
// engine's own sentence. Nothing here opens a web page or a folder, fetches or installs anything, or keys a transmitter.
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

namespace foxsdr::slintlanel {

// What one submit came to.
struct SubmitOutcome {
    std::uint64_t ticket = 0;      // 0 = the engine never took it
    int status = FOXAPI_OK;        // what submit answered
    std::string message;           // the host's own words when the call refused it
};

class LaneLPublisher {
public:
    struct Env {
        MainWindow* ui = nullptr;
        const FoxEngineApi* api = nullptr;
        FoxSession* session = nullptr;
        bool bounded = false;
        // One command; `radio` is the radio's number (1-based) it is aimed at, 0 = the selected radio. `longText` is the command's text when it is longer than the struct's slot.
        std::function<SubmitOutcome(const FoxCommand&, std::uint32_t radio, const std::string* longText)> submit;
        std::function<int()> selectedRadio;       // 1-based
        std::function<int()> radioCount;          // how many radios are open
    };
    explicit LaneLPublisher(Env env);
    ~LaneLPublisher();
    LaneLPublisher(const LaneLPublisher&) = delete;
    LaneLPublisher& operator=(const LaneLPublisher&) = delete;

    void wire();                                  // registers LaneL.act (once, after the window is created)
    void tick(long tickNo);                       // refreshes the rows whose facts moved (a few times a second)
    void onAnswer(const FoxCommandResult& r);     // the engine's answer to an op this publisher sent (others are ignored)
    // `state ll.<what>` for the drive scripts
    std::string readState(const std::string& what, std::string& out) const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace foxsdr::slintlanel
