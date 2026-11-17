// s4b_publish.hpp - SCREEN LANE S4b's PUBLISHER (docs/multi-radio-records/screen-s4b.md): the LaneS4b global of the Slint window filled and its act(id, text) answered, over the pure rules of s4b_logic.hpp.
//
// Row lists drawn by KPanels appended to the sections S4 already uses (main.slint): the Scanner (TOOLS) gets the lockout list, the Outputs section (SYSTEM) the servers other programs connect to and the
// helpers. Every control is OFF or at the engine's default until the person acts; the rows READ the facts the engine publishes and keep no copy of its state. Nothing here keys a transmitter.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <slint.h>

#include "foxsdr_api.h"
#include "main.h"

namespace foxsdr::slints4b {

struct SubmitOutcome {
    std::uint64_t ticket = 0;      // 0 = the engine never took it
    int status = FOXAPI_OK;
    std::string message;
};

class S4bPublisher {
public:
    struct Env {
        MainWindow* ui = nullptr;
        const FoxEngineApi* api = nullptr;
        FoxSession* session = nullptr;
        bool bounded = false;
        std::function<SubmitOutcome(const FoxCommand&, std::uint32_t radio, const std::string* longText)> submit;
        std::function<std::pair<std::uint32_t, double>()> scanner;   // the selected radio's scanner state (FOXAPI_SCANNER_*) and the frequency it is on
    };
    explicit S4bPublisher(Env env);
    ~S4bPublisher();
    S4bPublisher(const S4bPublisher&) = delete;
    S4bPublisher& operator=(const S4bPublisher&) = delete;

    void wire();                                  // registers LaneS4b.act (once, after the window is created)
    void tick(long tickNo);                       // refreshes the rows whose facts moved; saves a scan's hit when asked
    void onAnswer(const FoxCommandResult& r);     // the engine's answer to an op this publisher sent (others are ignored)
    // `state s4b.<what>` for the drive scripts
    std::string readState(const std::string& what, std::string& out) const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace foxsdr::slints4b
