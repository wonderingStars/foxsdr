// patch_publish.hpp - THE PATCH PAGE'S PUBLISHER (docs/engine-stage4.md, the Slint port, phase 3, the patch allowance): the UI family's models (ui/patch_page.slint: PatchModel,
// CanvasModel, InspectorModel, and the PatchPresetsModel the page of kind 10 shows) filled from patch_logic.hpp's PatchPage (which reads the C API table's lists and facts and owns
// the draft, the canvas and the presets) and set only when a model differs from the one last set, and every callback of the PatchEvents global turned into the call on the PatchPage
// that the ImGui window's own control makes. The commands the page produces (the draft's SET_GRAPH, a document, START, ALL OFF, the radio scan, the view-shown offer) go through
// the plugin kit's one submit with their ticket; the picture a decoder node holds is read through the engine's read_image and published as an image.
//
// THE PATTERN IS extend_publish.hpp's: the constructor takes the window, the kit and the kit's publisher, wire() registers the callbacks once, publish(state) runs once a tick (BEFORE
// the kit's publish, so the presets page is built from this tick's model) and onAnswer(result) for each answer main.cpp polls.
//
// THE SEAM WITH THE KIT'S PRESETS PAGE (kit_publish.hpp): the KIT owns the page of kind 10 (its PageTab, its place in the tab row, its tear-off); this object computes the whole
// ::PatchPresetsModel and hands it over (KitPublisher::setPatchPresetsModel), asks whether the page is up (patchPresetsView) and opens and closes it from the band's PRESETS key.
// The settings file's half of the page (the saved patch, the presets, the previous-patch slot) is the Interface half HostDuties holds (PatchConfig, implemented in the .cpp).
//
// A BOUNDED RUN SENDS ONLY WHAT TOUCHES NOTHING BUT THE ENGINE'S GRAPH (the draft's SET_GRAPH and the view-shown flag): START, ALL OFF, the radio scan, the device lists and a preset
// load's STOP are logged by their op and kept in the process, and the session is told it was not taken (ticket 0). The graph an edit sends is what the probe captures.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <slint.h>

#include "host_duties.hpp"
#include "kit_publish.hpp"
#include "main.h"
#include "patch_logic.hpp"
#include "plugin_kit.hpp"

namespace foxsdr::slintpatch {

class PatchPublisher {
public:
    PatchPublisher(MainWindow& ui, foxsdr::slintkit::PluginKit& kit, foxsdr::slintkit::KitPublisher& pages, foxsdr::slinthost::HostDuties& duties, const FoxEngineApi* api,
                   FoxSession* session, bool bounded);
    ~PatchPublisher();
    PatchPublisher(const PatchPublisher&) = delete;
    PatchPublisher& operator=(const PatchPublisher&) = delete;

    // Registers every PatchEvents callback (once, after the window is created).
    void wire();
    // The tick: the page's reads, the draft's sync and commit, then the models that changed (once a tick, BEFORE the kit's publish).
    void publish(const FoxReceiverState& state);
    // An answer main.cpp polled (or the kit's poll took first): the draft's own (SET_GRAPH) and the radio scan's are settled, the rest are not this object's business.
    void onAnswer(const FoxCommandResult& r);
    // main.cpp's on_view_selected: the patch view (view-index 1) is showing or not. The engine is told on the next tick (an offer, repeated until taken). The tick also follows
    // the window's own view-index, because the plugin kit changes it directly when it opens or closes a page.
    void setViewShown(bool shown);

    // A bounded run's self-captures (never set in a release): FOXSDR_SLINT_OPEN=patch-presets is the kit's (openFromEnv); this one is FOXSDR_SLINT_PATCH_PROBE=1 - a scripted
    // pass over the canvas through the same callbacks the window's pointer fires (see runProbe in the .cpp).
    void enableProbe() { probe_ = true; }
    // A bounded run's evidence: the nodes, wires, whether it runs, the verdict, the commands sent and withheld, the answers.
    std::string summaryLine() const;
    std::size_t commandsSent() const { return sent_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    MainWindow& ui_;
    foxsdr::slintkit::PluginKit& kit_;
    foxsdr::slintkit::KitPublisher& pages_;
    foxsdr::slinthost::HostDuties& duties_;
    const FoxEngineApi* api_;
    FoxSession* session_;
    bool bounded_;
    bool probe_ = false;
    std::size_t sent_ = 0, withheld_ = 0, answered_ = 0;
};

}  // namespace foxsdr::slintpatch
