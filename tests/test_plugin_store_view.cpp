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

#include <cstdio>

#include "core/i18n.hpp"
#include "core/plugin_abi.h"
#include "gui/fonts.hpp"
#include "gui/plugin_store_view.hpp"
#include "gui/theme.hpp"
#include "test_check.hpp"

using cascade::gui::AddAllPlan;
using cascade::gui::kStoreSortCount;
using cascade::gui::ModulePlate;
using cascade::gui::planAddAll;
using cascade::gui::PluginStoreModel;
using cascade::gui::storeInstallColour;
using cascade::gui::storeInstallState;
using cascade::gui::StoreInstallState;
using cascade::gui::storeInstallWord;
using cascade::gui::StoreModule;
using cascade::gui::storeProsePx;
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
          "may fetch from a server it chose");
    CHECK(moduleReachSummary(fittedRow(CASCADE_CAP_TRACK_INFO)) ==
          "may fetch from a server it chose");

    // THE TUNE GRANT - one of the two permissions this product actually
    // enforces (the other, since host API level 1, is the radio-settings grant
    // on the Fitted modules plate), and the only place a row states a granted
    // fact rather than a declared one.
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

// ---------------------------------------------------------------------------
// 6. storeProsePx - the size the page's sentences are set in
// ---------------------------------------------------------------------------
//
// THE WHOLE OF "make the plugin store larger and easier to read" is behind this
// one number, because every measured height and key width in the view derives
// from it. It is pinned here rather than left to a screenshot for two reasons:
// a regression would be a silent shrink back to the tiny engraving, and the
// figure has to stay a THEME size - a local literal here is how the next
// typeface change leaves one window behind.
void testProseSize() {
    // Bigger than the smallest engraving in the application, which is what
    // every sentence on this page used to be set in.
    CHECK(storeProsePx() > cascade::gui::fonts::kTinySize);
    // ...and bigger than the ordinary prose size too. "Go bigger on the font"
    // was asked for AFTER the first raise, and a step from 14 to 17 is what
    // it was asked about.
    CHECK(storeProsePx() > cascade::gui::fonts::kUiSize);
    // A THEME SIZE, NOT A NUMBER INVENTED IN THE VIEW.
    CHECK(storeProsePx() == cascade::gui::fonts::kPanelSize);
    // The largest of the five the theme publishes: nothing on the ladder is
    // above it, so "the largest face the theme offers" is a statement this
    // check can keep true.
    CHECK(cascade::gui::fonts::kPanelSize > cascade::gui::fonts::kUiSize);
    CHECK(cascade::gui::fonts::kPanelSize > cascade::gui::fonts::kReadingSize);
    CHECK(cascade::gui::fonts::kPanelSize > cascade::gui::fonts::kLegendSize);
}

// ---------------------------------------------------------------------------
// 7. storeInstallState - the catalogue's question, in one word
// ---------------------------------------------------------------------------

StoreModule row() {
    StoreModule sm;
    sm.id = "example";
    sm.plate = catalogueRow();
    sm.installableHere = true;
    return sm;
}

StoreModule fittedStoreRow() {
    StoreModule sm = row();
    sm.plate.fitted = true;
    sm.plate.loaded = true;
    sm.plate.running = true;
    sm.plate.haveCapabilities = true;
    sm.plate.capabilities = CASCADE_CAP_DECODER;
    return sm;
}

std::string instWord(const StoreModule& sm) {
    return std::string(storeInstallWord(storeInstallState(sm)));
}

void testInstallState() {
    // NOT INSTALLED and CANNOT FIT are different answers, and the difference
    // is whether anything the user does could change it.
    CHECK(storeInstallState(row()) == StoreInstallState::NotInstalled);
    CHECK(instWord(row()) == "NOT INSTALLED");
    {
        StoreModule sm = row();
        sm.installableHere = false;
        CHECK(storeInstallState(sm) == StoreInstallState::CannotFit);
        CHECK(instWord(sm) == "CANNOT FIT");
    }
    // A TRANSFER IN FLIGHT MUST NOT MOVE A ROW BETWEEN TWO WORDS. blockedReason
    // carries "a transfer is already in progress"; installableHere is the
    // stable fact, and this state is derived from the stable one.
    {
        StoreModule sm = row();
        sm.blockedReason = "a transfer is already in progress";
        CHECK(storeInstallState(sm) == StoreInstallState::NotInstalled);
    }

    CHECK(storeInstallState(fittedStoreRow()) == StoreInstallState::Installed);
    CHECK(instWord(fittedStoreRow()) == "INSTALLED");
    {
        StoreModule sm = fittedStoreRow();
        sm.updateToVersion = "1.3.0";
        CHECK(storeInstallState(sm) == StoreInstallState::UpdateAvailable);
        CHECK(instWord(sm) == "UPDATE");
    }
    {
        // FITTED AND REFUSED. The file is here and the host would not have it,
        // which outranks an update on offer: the row's key still says UPDATE
        // because that is the ACTION, and the word says what the state IS.
        StoreModule sm = fittedStoreRow();
        sm.plate.loaded = false;
        sm.plate.running = false;
        sm.plate.refusalReason = "ABI mismatch";
        CHECK(storeInstallState(sm) == StoreInstallState::Refused);
        CHECK(instWord(sm) == "REFUSED");
        sm.updateToVersion = "1.3.0";
        CHECK(storeInstallState(sm) == StoreInstallState::Refused);
    }

    // FIVE STATES, FIVE WORDS, FIVE INKS - collapsing any two would report one
    // state as another on a panel whose whole job is to tell them apart.
    const StoreInstallState all[5] = {
        StoreInstallState::NotInstalled, StoreInstallState::CannotFit,
        StoreInstallState::Installed,    StoreInstallState::UpdateAvailable,
        StoreInstallState::Refused,
    };
    std::vector<std::string> words;
    for (StoreInstallState s : all) {
        words.push_back(std::string(storeInstallWord(s)));
        CHECK(!words.back().empty());
    }
    CHECK(allDistinct(words));
    for (int i = 0; i < 5; ++i) {
        for (int j = i + 1; j < 5; ++j) {
            CHECK(storeInstallColour(all[i]) != storeInstallColour(all[j]));
        }
    }

    // NEVER AMBER. Amber in this palette is a READING - something the radio or
    // the machine measured - and an install state is not a measurement. The
    // updates banner letters its own heading in gold for exactly this reason.
    for (StoreInstallState s : all) {
        CHECK(storeInstallColour(s) != theme::kAmber);
        CHECK(storeInstallColour(s) != theme::kAmberDim);
    }
    // ...and only REFUSED takes the alarm ink. Not having something is not a
    // fault, and a panel of red words means nothing.
    for (StoreInstallState s : all) {
        const bool red = storeInstallColour(s) == theme::kAlarm ||
                         storeInstallColour(s) == theme::kAlarmHot;
        CHECK(red == (s == StoreInstallState::Refused));
    }
    CHECK(storeInstallColour(StoreInstallState::Installed) == theme::kPhosphor);
    CHECK(storeInstallColour(StoreInstallState::UpdateAvailable) == theme::kGold);
}

// ---------------------------------------------------------------------------
// 8. planAddAll - what the one big key picks, and what it says it will do
// ---------------------------------------------------------------------------

PluginStoreModel readCatalogue(std::vector<StoreModule> mods) {
    PluginStoreModel m;
    m.modules = std::move(mods);
    m.haveCatalogue = !m.modules.empty();
    m.sourceStatus = "N plugins in the catalogue";
    return m;
}

bool hasNaming(const std::vector<std::string>& v, const char* name) {
    for (const std::string& s : v) {
        if (s.find(name) != std::string::npos) { return true; }
    }
    return false;
}

void testAddAllPlan() {
    {
        // NOTHING INSTALLED, nothing blocked: the key really does add all of
        // them, and only then may it say so.
        std::vector<StoreModule> mods(3, row());
        const AddAllPlan p = planAddAll(readCatalogue(mods), false);
        CHECK(p.install.size() == 3u);
        CHECK(p.update.empty());
        CHECK(p.skipped.empty());
        CHECK(p.blockedReason.empty());
        CHECK(p.label == "ADD ALL PLUGINS");
        // Catalogue order, so the run is the order the user is reading.
        CHECK(p.install[0] == 0 && p.install[1] == 1 && p.install[2] == 2);
    }
    {
        // SOME INSTALLED AND CURRENT. They are not "skipped": already having
        // something is the outcome the key was pressed for, and listing five
        // of them as passed over would bury the one that really could not be
        // fitted.
        std::vector<StoreModule> mods = {row(), fittedStoreRow(), row()};
        const AddAllPlan p = planAddAll(readCatalogue(mods), false);
        CHECK(p.install.size() == 2u);
        CHECK(p.install[0] == 0 && p.install[1] == 2);
        CHECK(p.skipped.empty());
        CHECK(p.label == "ADD ALL PLUGINS");
        CHECK(p.blockedReason.empty());
    }
    {
        // SOME BEHIND. Both counts on the key, because "ADD ALL PLUGINS" over
        // a run that also replaces two fitted modules is not what it says.
        std::vector<StoreModule> mods = {row(), row(), row(), fittedStoreRow(),
                                         fittedStoreRow()};
        mods[3].updateToVersion = "2.0.0";
        mods[4].updateToVersion = "2.0.0";
        const AddAllPlan p = planAddAll(readCatalogue(mods), false);
        CHECK(p.install.size() == 3u);
        CHECK(p.update.size() == 2u);
        CHECK(p.update[0] == 3 && p.update[1] == 4);
        CHECK(p.label == "ADD 3 PLUGINS, UPDATE 2");
        CHECK(p.blockedReason.empty());
    }
    {
        // ONLY UPDATES, and the singular reads as English.
        std::vector<StoreModule> mods = {fittedStoreRow(), fittedStoreRow()};
        mods[0].updateToVersion = "2.0.0";
        mods[1].updateToVersion = "2.0.0";
        CHECK(planAddAll(readCatalogue(mods), false).label == "UPDATE 2 PLUGINS");
        mods[1].updateToVersion.clear();
        CHECK(planAddAll(readCatalogue(mods), false).label == "UPDATE 1 PLUGIN");
    }
    {
        // EVERYTHING ALREADY FITTED AND CURRENT: a dead key, and it says why.
        std::vector<StoreModule> mods(2, fittedStoreRow());
        const AddAllPlan p = planAddAll(readCatalogue(mods), false);
        CHECK(p.install.empty());
        CHECK(p.update.empty());
        CHECK(!p.blockedReason.empty());
        CHECK(has(p.blockedReason, "already fitted"));
    }
    {
        // AN ENTRY THIS BUILD CANNOT TAKE - a retirement floor above the
        // installed build, an ABI that does not match - is SKIPPED AND NAMED,
        // with the reason it gave carried through. "17 installed, 7 skipped"
        // is a number nobody can act on.
        std::vector<StoreModule> mods = {row(), row()};
        mods[1].plate.name = "Inmarsat-C";
        mods[1].plate.retirementFloor = "9.9.9";
        mods[1].installableHere = false;
        mods[1].blockedReason =
            "not compatible with this version (built for plugin ABI 2, this build "
            "requires exactly 3)";
        mods[1].blockedReasonIfAcknowledged = mods[1].blockedReason;
        const AddAllPlan p = planAddAll(readCatalogue(mods), false);
        CHECK(p.install.size() == 1u);
        CHECK(p.install[0] == 0);
        CHECK(p.skipped.size() == 1u);
        CHECK(hasNaming(p.skipped, "Inmarsat-C"));
        CHECK(hasNaming(p.skipped, "plugin ABI 2"));
        // NOT "ALL", because it is not all. A key engraved ADD ALL PLUGINS
        // that quietly passes one over is the copy this window exists to
        // refuse.
        CHECK(p.label == "ADD 1 PLUGIN");
        CHECK(p.blockedReason.empty());
        // ...and it is not counted as held by a notice, because no tick on
        // this panel would change it.
        CHECK(p.heldByNotice == 0);
    }
    {
        // A MAKER'S LEGAL NOTICE is the one skip the user can undo from this
        // panel, so it is counted apart - and the tick is what moves it.
        std::vector<StoreModule> mods = {row(), row()};
        mods[1].plate.name = "406 MHz Distress Beacon Decoder";
        mods[1].plate.legalNotice = "Interception may be an offence where you are.";
        mods[1].blockedReason = "the legal notice must be acknowledged first";
        mods[1].blockedReasonIfAcknowledged.clear();
        const AddAllPlan held = planAddAll(readCatalogue(mods), false);
        CHECK(held.install.size() == 1u);
        CHECK(held.heldByNotice == 1);
        CHECK(hasNaming(held.skipped, "406 MHz"));
        CHECK(held.label == "ADD 1 PLUGIN");

        const AddAllPlan acked = planAddAll(readCatalogue(mods), true);
        CHECK(acked.install.size() == 2u);
        CHECK(acked.skipped.empty());
        CHECK(acked.heldByNotice == 0);
        CHECK(acked.label == "ADD ALL PLUGINS");
    }
    {
        // NOBODY HAS ASKED FOR A CATALOGUE. Not "everything is up to date",
        // which is the clean zero this product has been bitten by: "nothing to
        // add" and "we have not looked" are different statements.
        PluginStoreModel m;
        const AddAllPlan p = planAddAll(m, false);
        CHECK(!p.blockedReason.empty());
        CHECK(has(p.blockedReason, "CHECK NOW"));
        CHECK(!has(p.blockedReason, "already fitted"));
        CHECK(p.label == "ADD ALL PLUGINS");
    }
    {
        // ASKED, AND THE ATTEMPT FAILED. Telling this user to press CHECK NOW
        // is telling them to do again the thing that just did not work.
        PluginStoreModel m;
        m.sourceError = "TLS handshake failed";
        const AddAllPlan p = planAddAll(m, false);
        CHECK(!has(p.blockedReason, "CHECK NOW"));
        CHECK(has(p.blockedReason, "did not return a catalogue"));
    }
    {
        // ASKED, AND IT LISTED NOTHING.
        PluginStoreModel m;
        m.sourceStatus = "0 plugins in the catalogue";
        CHECK(has(planAddAll(m, false).blockedReason, "lists no modules"));
    }
    {
        // ONE TRANSFER AT A TIME is what the downloader actually does.
        PluginStoreModel m = readCatalogue(std::vector<StoreModule>(3, row()));
        m.busy = true;
        const AddAllPlan p = planAddAll(m, false);
        CHECK(has(p.blockedReason, "transfer is already in progress"));
        // The plan is still computed, so the key's word does not flicker while
        // a download runs.
        CHECK(p.install.size() == 3u);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// 34-language review: the store's words in every catalogue, in the real faces
// ---------------------------------------------------------------------------

// Every word the card's action column letters under its key.
const char* const kStatusWords[] = {"NOT FITTED", "REFUSED",       "STOPPED",   "TAKES NO SIGNAL",
                                    "STARTED",    "NOT INSTALLED", "CANNOT FIT", "INSTALLED",
                                    "UPDATE"};

bool useLanguage(const std::string& code) {
    if (!cascade::gui::fonts::canDraw(code)) { return false; }
    cascade::i18n::setLanguage(code);
    cascade::gui::fonts::applyLanguage(code);
    cascade::gui::fonts::applyPending();
    return true;
}

void testEveryLanguageStoreWords() {
    std::printf("  every language: status words never broken mid-word, SHOW keeps two columns\n");
    // THE SHOW COLUMN AT THE STORE'S DESIGN SIZE (1480 px wide, as it opens
    // in a 1600 px main window): the well is 462 px, 442 inside its 10 px
    // padding, two columns of (442 - 12) / 2 = 215 px - measured off the
    // 34-language captures. English fits there at its own size. Every
    // language must keep two columns there too, by drawing its longest label
    // smaller, because a well that drops to one column of six grows three rows
    // taller for the sake of one word (pt-PT, lt).
    //
    // ONE KNOWN EXCEPTION, asserted rather than skipped so it cannot go stale:
    // Polish "NIE MOŻNA ZAMONTOWAĆ" (CANNOT FIT) needs about 239 px even at
    // seven tenths, so the Polish well keeps the one column it always had.
    // A shorter Polish label is a translator's decision, not a layout one.
    useLanguage("en");
    const float englishColW = 215.0f;
    CHECK(cascade::gui::storeShowRockerMinWidth(storeProsePx()) <= englishColW);
    CHECK(cascade::gui::storeShowTwoColumns(englishColW));
    int broken = 0;
    int wrapped = 0;
    int oneColumn = 0;
    int skipped = 0;
    for (const cascade::i18n::Language& l : cascade::i18n::languages()) {
        if (!useLanguage(l.code)) {
            ++skipped;
            continue;
        }
        ImFont* uf = cascade::gui::fonts::ui();
        const float room = cascade::gui::storeStatusWordRoom();
        for (const char* key : kStatusWords) {
            const char* word = cascade::i18n::tr(key);
            const cascade::gui::LineFit fit = cascade::gui::storeStatusWordFit(word);
            if (!fit.wrap) { continue; }
            ++wrapped;
            // Wrapped is allowed; wrapped THROUGH a word is not ("PAIGALDAMAT / A").
            if (cascade::gui::longestUnbreakableWidth(uf, fit.px, word) > room + 0.5f) {
                std::printf("      %s: \"%s\" breaks mid-word in a %.1f px column\n", l.code.c_str(),
                            word, room);
                ++broken;
            }
        }
        // (Only one way round: in the narrower Linux face Polish may fit.)
        const bool knownOne = l.code == "pl";
        if (!cascade::gui::storeShowTwoColumns(englishColW)) {
            std::printf("      %s: SHOW is one column at %.1f px (needs %.1f at the floor)%s\n",
                        l.code.c_str(), englishColW,
                        cascade::gui::storeShowRockerMinWidth(cascade::gui::fitFloorFor(storeProsePx())),
                        knownOne ? " - the known Polish exception" : "");
            if (!knownOne) { ++oneColumn; }
        }
    }
    useLanguage("en");
    std::printf("    %d status words broken mid-word (%d wrapped at a word), %d one-column SHOW wells, "
                "%d languages not drawable here\n",
                broken, wrapped, oneColumn, skipped);
    CHECK(broken == 0);
    CHECK(oneColumn == 0);
}

// A CATALOGUE REPLACED TAKES EVERY CONSENT WITH IT (bug hunt 2026-09-24,
// plugin-store-1). The ADD ALL tick used to survive CHECK NOW for the life of
// the process, so a tick given against one catalogue's notices silently covered
// a module - or a reworded notice - that arrived in the next one, and the key
// read ADD ALL PLUGINS over a notice nobody had read.
void testCatalogueConsentForgotten() {
    // The first catalogue: one plain module, one with a maker's notice. The
    // user reads the notice and ticks the ADD ALL box.
    std::vector<StoreModule> first = {row(), row()};
    first[1].plate.name = "406 MHz Distress Beacon Decoder";
    first[1].plate.legalNotice = "Interception may be an offence where you are.";
    first[1].blockedReason = "the legal notice must be acknowledged first";
    first[1].blockedReasonIfAcknowledged.clear();
    cascade::gui::PluginStoreDeck deck;
    deck.selected = 1;
    deck.legalAck = true;
    deck.addAllAck = true;
    CHECK(planAddAll(readCatalogue(first), deck.addAllAck).label == "ADD ALL PLUGINS");

    // CHECK NOW: the catalogue is replaced.
    cascade::gui::forgetCatalogueConsent(deck);
    CHECK(deck.selected == -1);
    CHECK(!deck.legalAck);
    CHECK(!deck.addAllAck);

    // The next catalogue carries a NEW module with a notice of its own. The
    // plan drawn from the deck as it now stands must hold it back and name it.
    std::vector<StoreModule> second = first;
    second.push_back(row());
    second[2].plate.name = "Pager Decoder";
    second[2].plate.legalNotice = "Reading pager traffic may be unlawful where you are.";
    second[2].blockedReason = "the legal notice must be acknowledged first";
    second[2].blockedReasonIfAcknowledged.clear();
    const AddAllPlan p = planAddAll(readCatalogue(second), deck.addAllAck);
    CHECK(p.heldByNotice == 2);
    CHECK(hasNaming(p.skipped, "Pager Decoder"));
    CHECK(p.label == "ADD 1 PLUGIN");
    if (p.label != "ADD 1 PLUGIN") {
        std::printf("  after a catalogue refresh the key reads \"%s\"\n", p.label.c_str());
    }
}

int main() {
    testKindTag();
    testReachSummary();
    testReachColour();
    testStateWord();
    testStateInkAndLamp();
    testSortLabels();
    testProseSize();
    testInstallState();
    testAddAllPlan();
    testCatalogueConsentForgotten();

    // THE REAL FACES for the measured half, as test_bench_text_fits loads them.
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1600.0f, 1000.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    const bool loaded = cascade::gui::fonts::load();
    CHECK(loaded);
    if (loaded) {
        ImGui::NewFrame();
        ImGui::Render();
        testEveryLanguageStoreWords();
    }
    ImGui::DestroyContext();
    return testSummary("test_plugin_store_view");
}
