// transmit_publish.hpp - THE TRANSMIT PAGE'S PUBLISHER (docs/engine-stage4.md 6.58): the UI family's model (ui/transmit_page.slint: TransmitPageModel) filled from
// transmit_logic.hpp's TransmitPage (which reads the receiver state and the table's facts) and set only when it differs from the one last set, every callback of the
// TransmitEvents global turned into the call on the TransmitPage that the ImGui window's own control makes, and the KEY'S BATCH sent once a tick.
//
// THE PATTERN IS patch_publish.hpp's: the constructor takes the window, the kit's publisher and the engine session; wire() registers the callbacks once; publish(state) runs once a
// tick BEFORE the kit's publish (the Transmit page of kind 11 is built from the model this hands over); onAnswer(result) for each answer main.cpp polls.
//
// THE PAGE'S OPEN FLAG is the KIT's (the page of kind 11 exists or not); this object tells the engine (FOXAPP_OP_SET_TRANSMIT_PAGE_OPEN, which the engine saves and uses to seed the
// address box) and believes it has told it only when the session TOOK the command - the classic's syncTransmitPageOpen rule. togglePage() is the Transmit row's switch.
//
// THE KEY, EVERY TICK, ONE BATCH: the tick asks TransmitPage::tick(live, now) for the key's commands (the classic's txKeyCommands and txPluginArmCommands over what happened) and submits
// them together in one submit, in order, so a LATCH press's "release then press" lands as a pair. The tick runs whether or not the page is open: the first tick after the page
// went away is the one that lets go of what it held.
//
// A BOUNDED RUN SENDS ONLY WHAT CANNOT PUT RF OUT OR OPEN A BOARD (transmit_logic.hpp boundedMaySend): every command passes that filter; what it refuses is counted and said once by its
// op, and the page then does what the window does with a refusal. NO TEST OR CAPTURE EVER KEYS A TRANSMITTER: there is no transmit hardware on the test machine, the engine's own
// no-transmitter answers (NO_DEVICE, the arming refusal) are what the drives read back, and the dummy load is reachable only from C++ tests against the table.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include <slint.h>

#include "host_duties.hpp"
#include "kit_publish.hpp"
#include "main.h"
#include "plugin_kit.hpp"

namespace foxsdr::slinttransmit {

class TransmitPublisher {
public:
    TransmitPublisher(MainWindow& ui, foxsdr::slintkit::KitPublisher& pages, foxsdr::slinthost::HostDuties& duties, const FoxEngineApi* api, FoxSession* session, bool bounded);
    ~TransmitPublisher();
    TransmitPublisher(const TransmitPublisher&) = delete;
    TransmitPublisher& operator=(const TransmitPublisher&) = delete;

    // Registers every TransmitEvents callback (once, after the window is created).
    void wire();
    // The tick: the reads, the key's batch, the open flag's offer, then the model if it changed (once a tick, BEFORE the kit's publish).
    void publish(const FoxReceiverState& state);
    // An answer main.cpp polled (or the kit's poll took first): a refused LATCH or consent is lettered on the page.
    void onAnswer(const FoxCommandResult& r);
    // The Transmit row's switch (bank 0, row 5): opens the page torn off, or closes it.
    void togglePage();
    // The kit's page is open (the row's lit state).
    bool pageOpen() const;
    // A bounded run's evidence: the page, the key's state, the commands sent and withheld, the answers.
    std::string summaryLine() const;
    // The scripted input's read-back (`state transmit.<name>`): page (closed / docked / shown), live, ptt, consent, latchPending, sent, withheld, answers, refusal, keyNote,
    // sent.<op hex> and withheld.<op hex> (e.g. withheld.0a01 for TX_OPEN). "" when served, else the refusal.
    std::string readState(const std::string& name, std::string& out) const;
    std::size_t commandsSent() const { return sent_; }
    std::size_t commandsWithheld() const { return withheld_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    MainWindow& ui_;
    foxsdr::slintkit::KitPublisher& pages_;
    const FoxEngineApi* api_;
    FoxSession* session_;
    bool bounded_;
    std::size_t sent_ = 0, withheld_ = 0, answered_ = 0;
};

}  // namespace foxsdr::slinttransmit
