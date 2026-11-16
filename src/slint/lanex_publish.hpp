// lanex_publish.hpp - LANE X's PUBLISHER (docs/multi-radio-records/lane-x.md): the LaneX global of the Slint window filled and its act(id, text) answered, over the pure rules of lanex_help.hpp and
// lanex_bench.hpp.
//
// HELP: openHelp(bank, row) opens the sheet at the topic of that section (or at the list of topics); the search field, the list and a topic are rows of lane K's panel; Esc and Close shut it.
// SPEED TEST: rows under SYSTEM > Performance. The test runs on one worker thread (lanex_bench.hpp); this class only reads its progress twice a second. It has NO way to send an engine command and
// writes no setting: the Env below holds nothing that could.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

#include <slint.h>

#include "knob_host.hpp"
#include "lanex_bench.hpp"
#include "main.h"

namespace foxsdr::slintlanex {

class LaneXPublisher {
public:
    struct Env {
        MainWindow* ui = nullptr;
        // Times item i of the speed test. Empty = the program's own (core/dsp_benchmark.hpp); a test hands in a stand-in.
        std::function<BenchItem(std::size_t)> benchRun;
        std::size_t benchCount = 0;            // the number of items when benchRun is given; ignored otherwise
        NumberWriter numbers;                  // the program's number format; empty = the plain point
        // The section the person is in: the bank and the logical row, (-1, -1) when none. Asked when the help opens.
        std::function<std::pair<int, int>()> where;
        // The controllers (knob_host.hpp; null = no such rows). The folder the choice is kept in ("" = not kept); `standIn` = the made-up controllers of a bounded run (never a real device).
        foxsdr::slintknobs::KnobHost* knobs = nullptr;
        std::string settingsDir;
        bool standIn = false;
    };
    explicit LaneXPublisher(Env env);
    ~LaneXPublisher();
    LaneXPublisher(const LaneXPublisher&) = delete;
    LaneXPublisher& operator=(const LaneXPublisher&) = delete;

    void wire();                       // registers LaneX.act (once, after the window is created)
    void tick(long tickNo);            // the speed test's progress, a few times a second
    void openHelp();                   // at the topic of the section the person is in, else the list
    void openHelpAt(int bank, int row);
    void closeHelp();
    bool helpOpen() const;
    // The saved choice is applied at start (after the window and the knob host exist).
    void loadKnobs();
    // `state lx.<what>` for the drive scripts
    std::string readState(const std::string& what, std::string& out) const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

}  // namespace foxsdr::slintlanex
