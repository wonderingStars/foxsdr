// s4_publish.hpp - SCREEN LANE S4's PUBLISHER (docs/multi-radio-records/screen-s4-wave8.md): the LaneS4 global of the Slint window filled and its act(id, text) answered, over the pure rules of s4_logic.hpp.
//
// Two row lists, each drawn by a KPanel appended to the section the person looks in (main.slint): the Scanner (TOOLS) gets "save each signal the scan stops on as a bookmark"; the Outputs
// section (SYSTEM) gets the TCI and UDP-audio switches, MQTT, the webhook and the keyword alerts. Every control is OFF or at the engine's default until the person acts; the rows READ the facts
// the engine publishes and keep no copy of its state. A password or an address that may hold a token is typed into a masked field, sent once, never shown again and never kept here; the engine
// keeps it in the Windows protected store. Nothing here keys a transmitter and nothing raises a desktop notification.
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

namespace foxsdr::slints4 {

struct SubmitOutcome {
    std::uint64_t ticket = 0;      // 0 = the engine never took it
    int status = FOXAPI_OK;
    std::string message;
};

class S4Publisher {
public:
    struct Env {
        MainWindow* ui = nullptr;
        const FoxEngineApi* api = nullptr;
        FoxSession* session = nullptr;
        bool bounded = false;
        std::function<SubmitOutcome(const FoxCommand&, std::uint32_t radio, const std::string* longText)> submit;
        std::function<std::pair<std::uint32_t, double>()> scanner;   // the selected radio's scanner state (FOXAPI_SCANNER_*) and the frequency it is on
    };
    explicit S4Publisher(Env env);
    ~S4Publisher();
    S4Publisher(const S4Publisher&) = delete;
    S4Publisher& operator=(const S4Publisher&) = delete;

    void wire();                                  // registers LaneS4.act (once, after the window is created)
    void tick(long tickNo);                       // refreshes the rows whose facts moved; saves a scan's hit when asked
    void onAnswer(const FoxCommandResult& r);     // the engine's answer to an op this publisher sent (others are ignored)
    // `state s4.<what>` for the drive scripts
    std::string readState(const std::string& what, std::string& out) const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace foxsdr::slints4
