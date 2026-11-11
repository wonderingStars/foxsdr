// workspace_publish.hpp - THE RADIO WORKSPACE'S PUBLISHER (docs/multi-radio-records/screen-s3.md): the layout, the page, the radio count and the selected radio into the WorkspaceModel global
// of ui/workspace.slint, and each visible radio's pane figures (a RadioView's PaneFigures, its two pictures, the words of workspace_logic's PaneTexts) into one row of its `cells` model.
// The pattern is publishPicture's of main.cpp: a value is set only when it changed (a Slint model set replaces every repeater item behind it), so a quiet pane costs nothing.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <array>
#include <memory>
#include <string>

#include <slint.h>

#include "main.h"
#include "radio_view.hpp"
#include "workspace_logic.hpp"

namespace foxsdr::slintworkspace {

// What a pane needs besides its RadioView and its words: the host's shared figures and the pointer.
struct PaneContext {
    foxsdr::slinthost::PaneTexts texts;
    std::string decoder;            // the chip: what runs on this radio ("" draws none)
    int traceMode = 1;              // shared: 0 normal, 1 peak hold, 2 average
    float hoverX = -1;              // the pointer over this pane's spectrum or waterfall, fraction of the plot; -1 = not over it
    std::string cursorText;         // "145.212 MHz . -78.4 dB" for hoverX
};

class WorkspacePublisher {
public:
    explicit WorkspacePublisher(MainWindow& ui);

    // The layout side: what is shown, which page, how many radios, which one is selected, the page keys, the notice, the band-plan ribbon size, and which cells hold a radio.
    // Sets only what changed. `notice` is the small-window sentence or "".
    void publishLayout(const foxsdr::slinthost::Workspace& ws, int winW, int winH, int ribbonSize);

    // One visible radio's pane: its words and figures into its row. `rv.fig` is what the tick's paneTick just derived. Sets only what changed.
    void publishPane(foxsdr::slinthost::RadioView& rv, const PaneContext& ctx);

    // How many rows were set (not skipped as unchanged) since the last call: the tests of the host read it to see that a quiet pane costs nothing.
    int takeRowsSet() { const int n = rowsSet_; rowsSet_ = 0; return n; }

private:
    void setRow(int slot, bool occupied);

    MainWindow& ui_;
    std::shared_ptr<slint::VectorModel<WorkspaceCell>> cells_;
    std::array<WorkspaceCell, foxsdr::slinthost::kMaxRadios> rows_;
    std::array<bool, foxsdr::slinthost::kMaxRadios> published_{};
    // the last layout values sent
    int shown_ = -1, layout_ = -1, page_ = -1, count_ = -1, selected_ = -1, ribbon_ = -1;
    std::string notice_, pagesSig_;
    int rowsSet_ = 0;
};

}  // namespace foxsdr::slintworkspace
