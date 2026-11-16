// s5_publish.hpp - SCREEN LANE S5's PUBLISHER (wave 8; docs/multi-radio-records/screen-s5-sources.md): the LaneS5 global of the Slint window filled and its act(id, text) answered, over the pure rules of s5_logic.hpp.
//
// One list of rows, drawn by a KPanel appended to the body of the SIGNAL bank's Source section (main.slint): a raw I/Q stream over TCP or UDP, automatic ppm, tuner pooling, the RTL-SDR's direct-sampling
// choice. Everything is folded behind one key until asked for, and every control is OFF or at the engine's own value until the person acts. The rows READ the facts the engine names and keep no copy of the engine's
// state; what the person has typed and not sent is a draft and is never saved. An operation the engine refuses is shown with the engine's own sentence. Nothing here keys a transmitter; nothing opens a
// socket until the person presses Open.
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
#include "s5_rows.hpp"

namespace foxsdr::slints5 {

class S5Publisher {
public:
    struct Env {
        MainWindow* ui = nullptr;
        const FoxEngineApi* api = nullptr;
        FoxSession* session = nullptr;
        bool bounded = false;
        // One command aimed at radio `radio` (1-based; 0 = the selected radio). `longText` is the command's text when it is longer than the struct's slot.
        std::function<SubmitOutcome(const FoxCommand&, std::uint32_t radio, const std::string* longText)> submit;
        std::function<int()> selectedRadio;       // 1-based
    };
    explicit S5Publisher(Env env);
    ~S5Publisher();
    S5Publisher(const S5Publisher&) = delete;
    S5Publisher& operator=(const S5Publisher&) = delete;

    void wire();                                  // registers LaneS5.act (once, after the window is created)
    void tick(long tickNo);                       // refreshes the rows whose facts moved (a few times a second)
    void onAnswer(const FoxCommandResult& r);     // the engine's answer to an op this publisher sent (others are ignored)
    // `state s5.<what>` for the drive scripts
    std::string readState(const std::string& what, std::string& out) const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace foxsdr::slints5
