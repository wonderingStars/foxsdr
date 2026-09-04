// Tests for gui/plugin_store_view.hpp - the pure half of the PLUGIN STORE, and
// of the DATA PLATE it shares with the FITTED MODULES window: the kind tag on a
// row, the one-line reach summary and the ink it is drawn in, the ON THIS
// MACHINE state word with its colour and its lamp, and the engraved sort
// labels.
//
// WHY THESE ARE THE FUNCTIONS THAT MATTER. Every one of them turns a record
// into words a user will believe, and the header's whole argument is about the
// difference between three things that all look like an empty list:
//
//   NOT DECLARED   a catalogue row. Nobody has read this module's declaration,
//                  because the catalogue index carries no capability field.
//   NOT KNOWN      a file that IS here and that the host refused. It was read
//                  and rejected; it reaches nothing because none of it is
//                  loaded, which is not the same as asking for nothing.
//   publishes to   a module that was read, declares only inward capabilities,
//   the host only  and genuinely reaches nothing outward.
//
// Collapsing any two of those reports our own ignorance as the maker's silence,
// or a refusal as harmlessness. The checks below pin all three as three
// DIFFERENT strings rather than asserting each in isolation, because "each is
// non-empty" would survive the collapse.
//
// THE IMPOSSIBLE COMBINATIONS ARE HERE ON PURPOSE. A plate that is not fitted
// but claims to be loaded, a refused record that claims to be running, a loaded
// module whose capability word was never recorded - none can arise from the
// wiring as it stands, and each must still produce a DEFINED answer rather than
// whatever the last guard happened to fall through to. The one that is not
// hypothetical is the last: `haveCapabilities` false on a started module must
// read STARTED and never TAKES NO SIGNAL, because "we did not record what it
// declares" and "it declares no decoder" are opposite claims.
//
// WHAT IS NOT REACHABLE FROM HERE, and it is a real gap rather than an
// omission. The catalogue-state function (never asked / read-empty / failed /
// read), the SHOW-well filter predicates (stateGroup, kindGroup, passesShow),
// the search match and the sort comparator all have INTERNAL LINKAGE in
// plugin_store_view.cpp - they live in its anonymous namespace, or inside the
// lambda in PluginStoreView::draw - so no test binary can call them. The two
// things the store's own public surface does expose about that machinery are
// checked here: kStoreSortCount against storeSortLabel, and the out-of-range
// index rule. The rest needs those functions declared in the header before it
// can be pinned, which is a change to the view and not to this file.
//
// There is no ImGui in this file. ImU32 is a plain integer typedef and the
// theme's colours are compile-time constants, so the ink checks need no
// graphics context - unlike moduleDataPlateHeight/drawModuleDataPlate, which
// measure text through the loaded typefaces and are deliberately not called.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <climits>
#include <cstdint>
#include <string>
#include <vector>

#include "core/plugin_abi.h"
#include "gui/plugin_store_view.hpp"
#include "gui/theme.hpp"
#include "test_check.hpp"

using cascade::gui::kStoreSortCount;
using cascade::gui::ModulePlate;
using cascade::gui::moduleKindTag;
using cascade::gui::moduleReachColour;
using cascade::gui::moduleReachSummary;
using cascade::gui::moduleStateColour;
using cascade::gui::moduleStateLampLit;
using cascade::gui::moduleStateWord;
using cascade::gui::storeSortLabel;
namespace theme = cascade::gui::theme;

namespace {

std::string tag(const ModulePlate& m) { return std::string(moduleKindTag(m)); }
std::string word(const ModulePlate& m) { return std::string(moduleStateWord(m)); }

bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

bool allDistinct(std::vector<std::string> v) {
    std::sort(v.begin(), v.end());
    return std::unique(v.begin(), v.end()) == v.end();
}

// A CATALOGUE ROW: known to exist, never read. haveCapabilities is false for
// every one of them because PluginCatalogEntry carries no capability field.
ModulePlate catalogueRow() {
    ModulePlate m;
    m.name = "ADS-B";
    m.version = "1.2.0";
    m.maker = "FoxSDR";
    m.licence = "PolyForm-Noncommercial-1.0.0";
    m.fitted = false;
    m.loaded = false;
    m.running = false;
    m.haveCapabilities = false;
    return m;
}

// A FITTED, LOADED, STARTED module whose declaration WAS read.
ModulePlate fittedRow(std::uint32_t caps) {
    ModulePlate m = catalogueRow();
    m.fileName = "adsb-1.2.0.dll";
    m.fitted = true;
    m.loaded = true;
    m.running = true;
    m.haveCapabilities = true;
    m.capabilities = caps;
    m.haveTuneGrant = true;
    m.tuneGranted = false;
    return m;
}

// A FILE THE HOST WOULD NOT HAVE. Fitted (it is in the plugins directory) and
// not loaded, with no capability list.
ModulePlate refusedRow() {
    ModulePlate m;
    m.fileName = "broken-9.9.9.dll";
    m.fitted = true;
    m.loaded = false;
    m.running = false;
    m.haveDescriptor = false;
    m.haveCapabilities = false;
    m.refusalReason = "ABI mismatch: expected 3, plugin reports 2";
    return m;
}

// ---------------------------------------------------------------------------
// 1. moduleKindTag - every branch, and the precedence between them
// ---------------------------------------------------------------------------
void testKindTag() {
    // THE TWO SILENCES. A catalogue row has not told us yet; a refused file
    // was read and rejected, and tagging it NOT DECLARED would put the silence
    // on the module rather than on the refusal.
    CHECK(tag(catalogueRow()) == "NOT DECLARED");
    CHECK(tag(refusedRow()) == "NOT KNOWN");
    CHECK(tag(catalogueRow()) != tag(refusedRow()));

    // NOT FITTED AND SOMEHOW LOADED - an impossible pairing that must still
    // answer. It is not a file on this machine, so it reads as a catalogue row.
    ModulePlate ghost = catalogueRow();
    ghost.loaded = true;
    CHECK(tag(ghost) == "NOT DECLARED");

    // FITTED, LOADED, AND NO CAPABILITY WORD RECORDED. Also impossible from the
    // wiring, and also not a refusal: NOT KNOWN is reserved for a file the host
    // did not accept.
    ModulePlate unrecorded = fittedRow(0u);
    unrecorded.haveCapabilities = false;
    CHECK(tag(unrecorded) == "NOT DECLARED");

    // EACH DECODER BIT ON ITS OWN.
    CHECK(tag(fittedRow(CASCADE_CAP_DECODER)) == "DECODER");
    CHECK(tag(fittedRow(CASCADE_CAP_IQ_DECODER)) == "DECODER");
    CHECK(tag(fittedRow(CASCADE_CAP_IMAGE_DECODER)) == "DECODER");

    // EACH MAP BIT ON ITS OWN.
    CHECK(tag(fittedRow(CASCADE_CAP_TRACK_SOURCE)) == "MAP");
    CHECK(tag(fittedRow(CASCADE_CAP_BASEMAP)) == "MAP");
    CHECK(tag(fittedRow(CASCADE_CAP_TRACK_INFO)) == "MAP");

    CHECK(tag(fittedRow(CASCADE_CAP_PANEL)) == "PANEL");
    CHECK(tag(fittedRow(CASCADE_CAP_HOST_CLIENT)) == "CONTROL");
    CHECK(tag(fittedRow(CASCADE_CAP_PRESET)) == "CONTROL");

    // THE PRECEDENCE, which is what makes a row's tag stable as a module gains
    // capabilities: decoder over map, map over panel, panel over control.
    CHECK(tag(fittedRow(CASCADE_CAP_DECODER | CASCADE_CAP_BASEMAP)) == "DECODER");
    CHECK(tag(fittedRow(CASCADE_CAP_DECODER | CASCADE_CAP_PANEL |
                        CASCADE_CAP_HOST_CLIENT)) == "DECODER");
    CHECK(tag(fittedRow(CASCADE_CAP_TRACK_SOURCE | CASCADE_CAP_PANEL)) == "MAP");
    CHECK(tag(fittedRow(CASCADE_CAP_PANEL | CASCADE_CAP_PRESET)) == "PANEL");
    CHECK(tag(fittedRow(CASCADE_CAP_ALL_KNOWN)) == "DECODER");

    // A DECLARATION THIS BUILD CANNOT CLASSIFY. Bits were read - so this is not
    // "not declared" - but none of them is a bit this host knows. A word rather
    // than an empty tag.
    CHECK(tag(fittedRow(0u)) == "MODULE");
    CHECK(tag(fittedRow(0x80000000u)) == "MODULE");

    // Never empty, whatever it was handed.
    CHECK(!tag(catalogueRow()).empty());
    CHECK(!tag(refusedRow()).empty());
    CHECK(!tag(fittedRow(0u)).empty());
}

// ---------------------------------------------------------------------------
// 2. moduleReachSummary - the three silences kept apart, and the outward reach
// ---------------------------------------------------------------------------
void testReachSummary() {
    const std::string never = moduleReachSummary(catalogueRow());
    const std::string refused = moduleReachSummary(refusedRow());
    const std::string inward = moduleReachSummary(fittedRow(CASCADE_CAP_DECODER));

    // THE THREE MUST BE THREE. A refused module whose declaration nobody could
    // read must be distinguishable BOTH from a catalogue row nobody has fitted
    // and from a module that was read and genuinely reaches nothing outward.
    CHECK(allDistinct({never, refused, inward}));
    CHECK(never == "not declared until it is fitted");
    CHECK(refused == "not known: the host did not accept this file");
    CHECK(inward == "publishes to the host only");

    // ...and the refused one must not claim the catalogue row's excuse: it IS
    // fitted, so "until it is fitted" would be false of it.
    CHECK(!has(refused, "until it is fitted"));

    // NEVER "REACHES NOTHING". Every plugin here is native code mapped into
    // this process; there is no data-only module type, so no row may say one
    // reaches nothing.
    const std::vector<std::string> everySummary = {
        never,
        refused,
        inward,
        moduleReachSummary(fittedRow(0u)),
        moduleReachSummary(fittedRow(CASCADE_CAP_PANEL)),
        moduleReachSummary(fittedRow(CASCADE_CAP_BASEMAP)),
        moduleReachSummary(fittedRow(CASCADE_CAP_TRACK_INFO)),
        moduleReachSummary(fittedRow(CASCADE_CAP_HOST_CLIENT)),
        moduleReachSummary(fittedRow(CASCADE_CAP_ALL_KNOWN)),
    };
    for (const std::string& s : everySummary) {
        CHECK(!s.empty());
        CHECK(!has(s, "nothing"));
    }

    // A MODULE THAT WAS READ AND DECLARES NO BIT AT ALL is still a module in
    // this process, and reads the same as any other inward-only one.
    CHECK(moduleReachSummary(fittedRow(0u)) == "publishes to the host only");
    CHECK(moduleReachSummary(fittedRow(CASCADE_CAP_PANEL)) == "publishes to the host only");
    CHECK(moduleReachSummary(fittedRow(CASCADE_CAP_PRESET)) == "publishes to the host only");
    CHECK(moduleReachSummary(fittedRow(CASCADE_CAP_TRACK_SOURCE)) ==
          "publishes to the host only");

    // FETCHES FROM A SERVER - the two capabilities that go out over the wire.
    CHECK(moduleReachSummary(fittedRow(CASCADE_CAP_BASEMAP)) ==
          "fetches from a server you choose");
    CHECK(moduleReachSummary(fittedRow(CASCADE_CAP_TRACK_INFO)) ==
          "fetches from a server you choose");

    // THE TUNE GRANT - the one permission this product actually enforces, and
    // the only place a row states a granted fact rather than a declared one.
    ModulePlate asks = fittedRow(CASCADE_CAP_HOST_CLIENT);
    CHECK(asks.haveTuneGrant);
    CHECK(!asks.tuneGranted);
    CHECK(moduleReachSummary(asks) == "asks to move the receiver");

    ModulePlate granted = asks;
    granted.tuneGranted = true;
    CHECK(moduleReachSummary(granted) == "granted: may move the receiver");

    // GRANT NOT LOOKED UP is not the same as granted, and must never read as
    // one. haveTuneGrant false with tuneGranted true is an impossible pairing
    // and still has to answer safely.
    ModulePlate unasked = granted;
    unasked.haveTuneGrant = false;
    CHECK(moduleReachSummary(unasked) == "asks to move the receiver");
    CHECK(!has(moduleReachSummary(unasked), "granted:"));

    // THE FURTHEST REACH WINS. A module that both fetches tiles and asks to
    // move the receiver is summarised by the receiver, which is the stronger
    // claim on the user's radio.
    CHECK(moduleReachSummary(fittedRow(CASCADE_CAP_HOST_CLIENT | CASCADE_CAP_BASEMAP)) ==
          "asks to move the receiver");
    CHECK(moduleReachSummary(fittedRow(CASCADE_CAP_ALL_KNOWN)) == "asks to move the receiver");
}

// ---------------------------------------------------------------------------
// 3. moduleReachColour - by the furthest thing declared, and never rust
// ---------------------------------------------------------------------------
void testReachColour() {
    // UNKNOWN reads faint, for both silences: the colour is about how much is
    // known, and neither of them told us anything.
    CHECK(moduleReachColour(catalogueRow()) == theme::kInkFaint);
    CHECK(moduleReachColour(refusedRow()) == theme::kInkFaint);

    // OUTWARD reads gold - each of the three bits on its own.
    CHECK(moduleReachColour(fittedRow(CASCADE_CAP_HOST_CLIENT)) == theme::kGold);
    CHECK(moduleReachColour(fittedRow(CASCADE_CAP_BASEMAP)) == theme::kGold);
    CHECK(moduleReachColour(fittedRow(CASCADE_CAP_TRACK_INFO)) == theme::kGold);
    CHECK(moduleReachColour(fittedRow(CASCADE_CAP_DECODER | CASCADE_CAP_BASEMAP)) ==
          theme::kGold);

    // INWARD reads muted.
    CHECK(moduleReachColour(fittedRow(CASCADE_CAP_DECODER)) == theme::kInkMuted);
    CHECK(moduleReachColour(fittedRow(CASCADE_CAP_PANEL | CASCADE_CAP_PRESET)) ==
          theme::kInkMuted);
    CHECK(moduleReachColour(fittedRow(CASCADE_CAP_TRACK_SOURCE)) == theme::kInkMuted);
    CHECK(moduleReachColour(fittedRow(0u)) == theme::kInkMuted);

    // NEVER RUST. A declared capability is not a fault, and rust in this
    // palette means trouble. Nor phosphor, which means something is working.
    const std::vector<ModulePlate> all = {
        catalogueRow(),
        refusedRow(),
        fittedRow(0u),
        fittedRow(CASCADE_CAP_DECODER),
        fittedRow(CASCADE_CAP_HOST_CLIENT),
        fittedRow(CASCADE_CAP_BASEMAP),
        fittedRow(CASCADE_CAP_ALL_KNOWN),
    };
    for (const ModulePlate& m : all) {
        CHECK(moduleReachColour(m) != theme::kAlarm);
        CHECK(moduleReachColour(m) != theme::kAlarmHot);
        CHECK(moduleReachColour(m) != theme::kPhosphor);
    }
}

// ---------------------------------------------------------------------------
// 4. moduleStateWord / moduleStateColour / moduleStateLampLit
// ---------------------------------------------------------------------------
void testStateWord() {
    // NOT FITTED. A catalogue row, with no file for it here.
    CHECK(word(catalogueRow()) == "NOT FITTED");

    // NOT FITTED WINS OVER EVERYTHING, including a record claiming to be loaded
    // and running - which cannot happen, and must not read STARTED if it does.
    ModulePlate impossible = catalogueRow();
    impossible.loaded = true;
    impossible.running = true;
    impossible.haveCapabilities = true;
    impossible.capabilities = CASCADE_CAP_DECODER;
    CHECK(word(impossible) == "NOT FITTED");

    // REFUSED, and it outranks a record that also claims to be running.
    CHECK(word(refusedRow()) == "REFUSED");
    ModulePlate refusedButRunning = refusedRow();
    refusedButRunning.running = true;
    CHECK(word(refusedButRunning) == "REFUSED");

    // STOPPED: loaded, and the user stopped it.
    ModulePlate stopped = fittedRow(CASCADE_CAP_DECODER);
    stopped.running = false;
    CHECK(word(stopped) == "STOPPED");

    // ...and a stopped module that also declares no decoder still reads
    // STOPPED. The user's choice is the nearer fact.
    ModulePlate stoppedBasemap = fittedRow(CASCADE_CAP_BASEMAP);
    stoppedBasemap.running = false;
    CHECK(word(stoppedBasemap) == "STOPPED");

    // TAKES NO SIGNAL: read, started, and declares no decoder.
    CHECK(word(fittedRow(CASCADE_CAP_BASEMAP)) == "TAKES NO SIGNAL");
    CHECK(word(fittedRow(CASCADE_CAP_PANEL | CASCADE_CAP_PRESET)) == "TAKES NO SIGNAL");
    CHECK(word(fittedRow(0u)) == "TAKES NO SIGNAL");

    // ...but ONLY when the declaration was actually read. A started module
    // whose capability word was never recorded is a module that is started and
    // nothing more; calling it TAKES NO SIGNAL would report our own ignorance
    // as a fact about the module.
    ModulePlate unrecorded = fittedRow(0u);
    unrecorded.haveCapabilities = false;
    CHECK(word(unrecorded) == "STARTED");
    CHECK(word(unrecorded) != word(fittedRow(0u)));

    // STARTED for anything that can be fed. Deliberately NOT "running": this
    // side is handed neither the runner's table nor the receiver's run state.
    CHECK(word(fittedRow(CASCADE_CAP_DECODER)) == "STARTED");
    CHECK(word(fittedRow(CASCADE_CAP_IQ_DECODER)) == "STARTED");
    CHECK(word(fittedRow(CASCADE_CAP_IMAGE_DECODER)) == "STARTED");
    CHECK(word(fittedRow(CASCADE_CAP_ALL_KNOWN)) == "STARTED");
    CHECK(!has(word(fittedRow(CASCADE_CAP_DECODER)), "RUNNING"));

    // Five states, five distinct words.
    CHECK(allDistinct({word(catalogueRow()), word(refusedRow()), word(stopped),
                       word(fittedRow(CASCADE_CAP_BASEMAP)),
                       word(fittedRow(CASCADE_CAP_DECODER))}));
}

void testStateInkAndLamp() {
    ModulePlate stopped = fittedRow(CASCADE_CAP_DECODER);
    stopped.running = false;

    CHECK(moduleStateColour(catalogueRow()) == theme::kInkFaint);
    CHECK(moduleStateColour(refusedRow()) == theme::kAlarm);
    CHECK(moduleStateColour(stopped) == theme::kCream);
    CHECK(moduleStateColour(fittedRow(CASCADE_CAP_BASEMAP)) == theme::kInkMuted);
    CHECK(moduleStateColour(fittedRow(CASCADE_CAP_DECODER)) == theme::kIvory);

    // A STOP IS A CHOICE, NOT TROUBLE. It must not letter in either alarm tone.
    CHECK(moduleStateColour(stopped) != theme::kAlarm);
    CHECK(moduleStateColour(stopped) != theme::kAlarmHot);

    // STARTED IS NOT PHOSPHOR. Phosphor means something is known to be working,
    // which is a claim this side cannot make.
    CHECK(moduleStateColour(fittedRow(CASCADE_CAP_DECODER)) != theme::kPhosphor);
    CHECK(moduleStateColour(fittedRow(CASCADE_CAP_DECODER)) != theme::kPhosphorDim);

    // ONE LAMP, AND ONLY ONE STATE LIGHTS IT. A panel of lit lamps means
    // nothing, and this side cannot see the one state - being fed - that would
    // earn a green light.
    const std::vector<ModulePlate> all = {
        catalogueRow(), refusedRow(), stopped, fittedRow(CASCADE_CAP_BASEMAP),
        fittedRow(CASCADE_CAP_DECODER), fittedRow(CASCADE_CAP_ALL_KNOWN),
    };
    int lit = 0;
    for (const ModulePlate& m : all) {
        // The lamp and the word are two renderings of one decision and can
        // never disagree.
        CHECK(moduleStateLampLit(m) == (word(m) == "REFUSED"));
        if (moduleStateLampLit(m)) { ++lit; }
    }
    CHECK(lit == 1);
}

// ---------------------------------------------------------------------------
// 5. storeSortLabel - the boundary the header states
// ---------------------------------------------------------------------------
void testSortLabels() {
    // Through a variable, not the constant itself: MSVC warns C4127 on a
    // compile-time-constant condition, and a warning in a test is noise that
    // makes the next real one easier to skip past.
    const int sortCount = kStoreSortCount;
    CHECK(sortCount == 3);
    CHECK(std::string(storeSortLabel(0)) == "NAME");
    CHECK(std::string(storeSortLabel(1)) == "MAKER");
    CHECK(std::string(storeSortLabel(2)) == "VERSION");

    // "An index outside the range answers with the first key rather than with
    // whatever the last case happened to be."
    CHECK(std::string(storeSortLabel(-1)) == "NAME");
    CHECK(std::string(storeSortLabel(3)) == "NAME");
    CHECK(std::string(storeSortLabel(kStoreSortCount)) == "NAME");
    CHECK(std::string(storeSortLabel(INT_MIN)) == "NAME");
    CHECK(std::string(storeSortLabel(INT_MAX)) == "NAME");

    // Every key in range is engraved with a different word, and none is empty -
    // the segmented control draws one per index and two the same would make a
    // segment unpressable in effect.
    std::vector<std::string> labels;
    for (int i = 0; i < kStoreSortCount; ++i) {
        labels.push_back(std::string(storeSortLabel(i)));
        CHECK(!labels.back().empty());
    }
    CHECK(allDistinct(labels));
}

}  // namespace

int main() {
    testKindTag();
    testReachSummary();
    testReachColour();
    testStateWord();
    testStateInkAndLamp();
    testSortLabels();
    return testSummary("test_plugin_store_view");
}
