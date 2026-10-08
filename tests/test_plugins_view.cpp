// Tests for gui/plugins_view.hpp - the pure half of the FITTED MODULES window:
// the five-state module classifier, the word and the sentence it produces, the
// per-state counter behind the strip, the adapter that turns a host record into
// the shared data plate, and the adapter that turns a LoadedPlugin into this
// window's own record.
//
// WHY THIS HALF IS THE HALF WORTH TESTING. The header says it outright at
// lines 61-66: "PURE FIRST, DRAWN SECOND ... the free functions below, which
// have no ImGui in them and can be exercised without a graphics context". Every
// decision this window makes about what a user is TOLD about a module lives in
// those functions, and none of it is observable once it has gone into an
// ImDrawList. There is no ImGui in this file and none is needed.
//
// WHAT EACH GROUP OF CHECKS CAN ACTUALLY CATCH.
//
//   THE TRUTH TABLE. fittedState() is a chain of five guards whose ORDER is
//   the whole design - a stopped module that was also refused must read
//   REFUSED, because a refused record's descriptor was never copied and every
//   later guard would be asking about fields the host never filled in. So the
//   sweep below runs all 32 combinations of the four booleans against both a
//   decoder mask and an empty one, and compares each against a precedence
//   table transcribed from the header's PROSE rather than from the code. A
//   reordered guard changes at least one row of that sweep.
//
//   THE IMPOSSIBLE INPUTS. A module cannot really be stopped and refused at
//   once, and a loaded module cannot really carry an error string - but a
//   record arriving that way must still produce a DEFINED answer rather than a
//   plausible one, so each is asserted by name with the answer written out.
//
//   THE COUNTER'S CAPTION. countStates() feeds a strip that letters `notFed`
//   as decoders that are NOT DECODING. A basemap declares no decoder and can
//   never decode anything, so it must not be in that total - it was, once, and
//   the header records the correction at lines 249-255. The claim is pinned as
//   a claim: a vector of nothing but signal-less modules must report notFed 0.
//
//   THE VERBATIM STRINGS. The refusal reason and the runner's idle sentence
//   are quoted rather than rewritten, and both carry numbers a paraphrase
//   would throw away ("expected 3, plugin reports 2"). Those two are checked
//   for EQUALITY with the input, not for containment, because containment
//   would pass a sentence that wrapped the host's words in an opinion.
//
//   THE TWO WINDOWS AGREEING. makeModulePlate() feeds the SHARED data plate,
//   whose own state word is the coarser half of this window's. The last group
//   checks they never contradict: STARTED against FED, STOPPED against
//   STOPPED, REFUSED against REFUSED.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "core/plugin_abi.h"
#include "core/plugin_host.hpp"
#include "gui/plugin_store_view.hpp"
#include "gui/plugins_view.hpp"
#include "test_check.hpp"

using cascade::core::LoadedPlugin;
using cascade::gui::countStates;
using cascade::gui::FittedCounts;
using cascade::gui::FittedModule;
using cascade::gui::fittedOrphanSentence;
using cascade::gui::fittedState;
using cascade::gui::FittedState;
using cascade::gui::fittedStateSentence;
using cascade::gui::fittedStateWord;
using cascade::gui::makeFittedModule;
using cascade::gui::makeModulePlate;
using cascade::gui::FittedModulesDeck;
using cascade::gui::fittedDateText;
using cascade::gui::fittedRowNote;
using cascade::gui::FittedRowNote;
using cascade::gui::fittedVerdictLine;
using cascade::gui::fittedVisibleRows;
using cascade::gui::ModulePlate;
using cascade::gui::moduleMachineText;
using cascade::gui::modulePageFacts;

namespace {

// The three bits PluginRunner will create an instance for. Spelled out here
// rather than reused from the view's own file-local constant: if the view ever
// stopped counting one of them as signal-bearing, a shared constant would move
// with it and this suite would follow the mistake.
constexpr std::uint32_t kDecoderBit = CASCADE_CAP_DECODER;
constexpr std::uint32_t kIqBit = CASCADE_CAP_IQ_DECODER;
constexpr std::uint32_t kImageBit = CASCADE_CAP_IMAGE_DECODER;

bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// Whole-struct comparison, so a counter that moved a module from one field to
// another fails HERE rather than passing an assertion on the field that did not
// change. Six named fields, no indexing.
bool sameCounts(const FittedCounts& a, const FittedCounts& b) {
    return a.fed == b.fed && a.notFed == b.notFed && a.noSignal == b.noSignal &&
           a.stopped == b.stopped && a.refused == b.refused && a.total == b.total;
}

FittedCounts counts(int fed, int notFed, int noSignal, int stopped, int refused, int total) {
    FittedCounts c;
    c.fed = fed;
    c.notFed = notFed;
    c.noSignal = noSignal;
    c.stopped = stopped;
    c.refused = refused;
    c.total = total;
    return c;
}

// A record the tests can vary one field of at a time. Every default here is
// the BENIGN one - a loaded, started, decoder-declaring module - so a case
// below reads as exactly the departure it is testing.
FittedModule module(bool loaded, bool stopped, std::uint32_t caps, bool fed) {
    FittedModule m;
    m.file = "thing-1.0.0.dll";
    m.path = "C:/plugins/thing-1.0.0.dll";
    m.name = "Thing";
    m.version = "1.0.0";
    m.author = "A Maker";
    m.licence = "MIT";
    m.capabilities = caps;
    m.loaded = loaded;
    m.stopped = stopped;
    m.fed = fed;
    return m;
}

// THE PRECEDENCE, transcribed from the header's own words (plugins_view.hpp
// lines 31-45) and not from plugins_view.cpp:
//
//   REFUSED  the file was found and rejected            -> !loaded
//   STOPPED  in the stop set; the user's own choice
//   SIGNAL   declares no decoder at all                 -> no signal bit
//   FED      isFeeding AND the receiver is running
//   NOT FED  everything else
FittedState expected(bool loaded, bool stopped, std::uint32_t caps, bool fed,
                     bool receiverRunning) {
    const bool signalBearing = (caps & (kDecoderBit | kIqBit | kImageBit)) != 0u;
    if (!loaded) { return FittedState::Refused; }
    if (stopped) { return FittedState::Stopped; }
    if (!signalBearing) { return FittedState::NoSignal; }
    if (fed && receiverRunning) { return FittedState::Fed; }
    return FittedState::NotFed;
}

// ---------------------------------------------------------------------------
// 1. fittedState - every input combination, including the impossible ones
// ---------------------------------------------------------------------------
void testStateSweep() {
    // Four capability masks: nothing at all, each of the three signal bits on
    // its own paired with a non-signal bit, and a module that declares only
    // things signal is never routed to.
    const std::uint32_t masks[] = {
        0u,
        CASCADE_CAP_BASEMAP,
        CASCADE_CAP_TRACK_SOURCE | CASCADE_CAP_PANEL,
        kDecoderBit,
        kIqBit,
        kImageBit,
        kDecoderBit | CASCADE_CAP_BASEMAP,
        CASCADE_CAP_ALL_KNOWN,
    };
    int rows = 0;
    for (std::uint32_t caps : masks) {
        for (int bits = 0; bits < 16; ++bits) {
            const bool loaded = (bits & 1) != 0;
            const bool stopped = (bits & 2) != 0;
            const bool fed = (bits & 4) != 0;
            const bool running = (bits & 8) != 0;
            FittedModule m = module(loaded, stopped, caps, fed);
            // A refused record carries the host's reason and no descriptor;
            // give it both here so the sweep also proves the later guards are
            // never reached on one.
            if (!loaded) { m.error = "wrong ABI: expected 3, plugin reports 2"; }
            CHECK(fittedState(m, running) == expected(loaded, stopped, caps, fed, running));
            ++rows;
        }
    }
    // The sweep must actually have run the whole cross-product; a loop that
    // silently skipped rows would look identical from the outside.
    CHECK(rows == 8 * 16);
}

void testImpossibleInputs() {
    // BOTH STOPPED AND REFUSED. Refusal wins: a record the host rejected has
    // no descriptor, so the stop set's answer is about a module nobody read.
    FittedModule both = module(false, true, kDecoderBit, false);
    both.error = "no cascade_plugin_query entry point";
    CHECK(fittedState(both, true) == FittedState::Refused);
    CHECK(fittedState(both, false) == FittedState::Refused);

    // REFUSED AND SUPPOSEDLY FED. The runner cannot hold an instance for a
    // module that never loaded, but a record saying so must still read REFUSED
    // rather than FED.
    FittedModule fedGhost = module(false, false, kDecoderBit, true);
    CHECK(fittedState(fedGhost, true) == FittedState::Refused);

    // A LOADED RECORD CARRYING AN ERROR STRING. `error` is documented empty iff
    // loaded, so this pairing cannot arise from the host - and it must not turn
    // a working module into a refusal.
    FittedModule loadedWithError = module(true, false, kDecoderBit, true);
    loadedWithError.error = "a stale string nobody cleared";
    CHECK(fittedState(loadedWithError, true) == FittedState::Fed);

    // A LOADED MODULE WITH NO CAPABILITY BITS AT ALL. The host will not accept
    // a descriptor declaring nothing, so this is another record the caller had
    // to construct; it takes no signal, and claiming it is fed would be a green
    // lamp on a module the runner has no instance for.
    FittedModule blank = module(true, false, 0u, true);
    CHECK(fittedState(blank, true) == FittedState::NoSignal);

    // THE SAME MODULE STOPPED. The user's choice outranks the design fact.
    FittedModule blankStopped = module(true, true, 0u, true);
    CHECK(fittedState(blankStopped, true) == FittedState::Stopped);

    // THE RECEIVER'S RUN STATE IS PART OF THE ANSWER, and it is the one that
    // separates these two rows.
    FittedModule ready = module(true, false, kDecoderBit, true);
    CHECK(fittedState(ready, true) == FittedState::Fed);
    CHECK(fittedState(ready, false) == FittedState::NotFed);

    // A module declaring ONLY things nothing is routed to stays NoSignal
    // whatever the receiver is doing - that is the point of the state.
    FittedModule basemap = module(true, false, CASCADE_CAP_BASEMAP, false);
    CHECK(fittedState(basemap, true) == FittedState::NoSignal);
    CHECK(fittedState(basemap, false) == FittedState::NoSignal);
}

// ---------------------------------------------------------------------------
// 2. fittedStateWord - five states, five distinct words, none of them a claim
//    the window cannot keep
// ---------------------------------------------------------------------------
void testStateWords() {
    const std::string fed = fittedStateWord(FittedState::Fed);
    const std::string notFed = fittedStateWord(FittedState::NotFed);
    const std::string noSignal = fittedStateWord(FittedState::NoSignal);
    const std::string stopped = fittedStateWord(FittedState::Stopped);
    const std::string refused = fittedStateWord(FittedState::Refused);

    CHECK(fed == "FED");
    // The row (and the page's state word) letter this state as the CHIP that counts it does.
    CHECK(notFed == "NOT DECODING");
    CHECK(noSignal == "TAKES NO SIGNAL");
    CHECK(stopped == "STOPPED");  // the chip's word; "you stopped it" is the verdict sentence's
    CHECK(refused == "REFUSED");

    // Five states must letter as five DIFFERENT words, or the row cannot be
    // read back to the state that produced it.
    const std::vector<std::string> words = {fed, notFed, noSignal, stopped, refused};
    std::vector<std::string> sorted = words;
    std::sort(sorted.begin(), sorted.end());
    CHECK(std::unique(sorted.begin(), sorted.end()) == sorted.end());
    for (const std::string& w : words) { CHECK(!w.empty()); }

    // A module that takes no signal must never be lettered as one that is
    // failing to decode - the exact conflation the header records correcting.
    CHECK(!has(noSignal, "NOT DECODING"));
    CHECK(!has(noSignal, "DECOD"));
}

// ---------------------------------------------------------------------------
// 3. fittedStateSentence - what explains each state, and what is quoted
// ---------------------------------------------------------------------------
void testSentences() {
    // REFUSED: the host's own words, VERBATIM. Equality, not containment: a
    // sentence that wrapped the reason in framing would still contain it.
    const std::string reason = "ABI mismatch: expected 3, plugin reports 2";
    FittedModule refused = module(false, false, 0u, false);
    refused.name.clear();
    refused.version.clear();
    refused.author.clear();
    refused.licence.clear();
    refused.error = reason;
    CHECK(fittedStateSentence(refused, true) == reason);
    CHECK(fittedStateSentence(refused, false) == reason);

    // REFUSED WITH NO REASON RECORDED. Absent is not zero and it is not a
    // blank line either: the window says the host recorded nothing.
    FittedModule mute = refused;
    mute.error.clear();
    const std::string muteSentence = fittedStateSentence(mute, true);
    CHECK(!muteSentence.empty());
    CHECK(has(muteSentence, "recorded no reason"));

    // STOPPED: the user's own choice, described as a choice.
    FittedModule stopped = module(true, true, kDecoderBit, true);
    const std::string stoppedSentence = fittedStateSentence(stopped, true);
    CHECK(has(stoppedSentence, "You stopped this module"));

    // NO SIGNAL: says the module declares no decoder, and does not report it
    // as a fault.
    FittedModule basemap = module(true, false, CASCADE_CAP_BASEMAP, false);
    const std::string basemapSentence = fittedStateSentence(basemap, true);
    CHECK(has(basemapSentence, "declares no decoder"));
    CHECK(!has(basemapSentence, "not being fed"));

    // FED.
    FittedModule fed = module(true, false, kDecoderBit, true);
    CHECK(has(fittedStateSentence(fed, true), "being fed"));

    // NOT FED, RECEIVER STOPPED, INSTANCE ALREADY MATCHED. "Starting the
    // receiver is all this needs" is a promise, and it is only true when the
    // runner already holds a matched instance - so it is made HERE and nowhere
    // else.
    FittedModule waiting = module(true, false, kDecoderBit, true);
    const std::string waitingSentence = fittedStateSentence(waiting, false);
    CHECK(has(waitingSentence, "receiver is stopped"));
    CHECK(has(waitingSentence, "starting the receiver is all this needs"));
    CHECK(!has(waitingSentence, "second reason"));

    // NOT FED, RECEIVER STOPPED, AND A SECOND REASON. The promise must NOT be
    // made, and the runner's sentence must be carried through so the user is
    // not sent to start the receiver for nothing.
    FittedModule twoFaults = module(true, false, kDecoderBit, false);
    twoFaults.idleDetail = "no instance: 48000 Hz decoder, pipeline at 24000 Hz";
    const std::string twoSentence = fittedStateSentence(twoFaults, false);
    CHECK(has(twoSentence, "receiver is stopped"));
    CHECK(has(twoSentence, "second reason"));
    CHECK(has(twoSentence, twoFaults.idleDetail.c_str()));
    CHECK(!has(twoSentence, "all this needs"));

    // NOT FED, RECEIVER STOPPED, NOTHING ELSE RECORDED. No invented second
    // reason, and no promise either.
    FittedModule oneFault = module(true, false, kDecoderBit, false);
    const std::string oneSentence = fittedStateSentence(oneFault, false);
    CHECK(has(oneSentence, "receiver is stopped"));
    CHECK(!has(oneSentence, "second reason"));
    CHECK(!has(oneSentence, "all this needs"));

    // NOT FED, RECEIVER RUNNING, RUNNER RECORDED A SENTENCE. Quoted exactly -
    // the Plugins rail prints this same string, and one idle decoder described
    // two ways is worse than one description in the wrong place.
    FittedModule idle = module(true, false, kDecoderBit, false);
    idle.idleDetail = "no instance: 48000 Hz decoder, pipeline at 24000 Hz";
    CHECK(fittedStateSentence(idle, true) == idle.idleDetail);

    // NOT FED, RECEIVER RUNNING, NOTHING RECORDED. Says so rather than
    // inventing a cause.
    FittedModule silent = module(true, false, kDecoderBit, false);
    const std::string silentSentence = fittedStateSentence(silent, true);
    CHECK(!silentSentence.empty());
    CHECK(has(silentSentence, "No reason was recorded"));

    // Five states, five different sentences.
    const std::vector<std::string> all = {fittedStateSentence(fed, true),
                                          silentSentence,
                                          basemapSentence,
                                          stoppedSentence,
                                          fittedStateSentence(refused, true)};
    std::vector<std::string> sorted = all;
    std::sort(sorted.begin(), sorted.end());
    CHECK(std::unique(sorted.begin(), sorted.end()) == sorted.end());
    for (const std::string& s : all) { CHECK(!s.empty()); }
}

// ---------------------------------------------------------------------------
// 4. countStates - the caption's meaning, pinned as a claim
// ---------------------------------------------------------------------------
void testCounts() {
    // AN EMPTY PANEL COUNTS NOTHING. Absent is not zero elsewhere in this
    // product, but here the vector genuinely holds no modules and every field
    // is a true zero.
    CHECK(sameCounts(countStates({}, true), counts(0, 0, 0, 0, 0, 0)));
    CHECK(sameCounts(countStates({}, false), counts(0, 0, 0, 0, 0, 0)));

    // ONE OF EACH STATE, with the receiver running.
    std::vector<FittedModule> mixed;
    mixed.push_back(module(true, false, kDecoderBit, true));            // Fed
    mixed.push_back(module(true, false, kIqBit, false));                // NotFed
    mixed.push_back(module(true, false, CASCADE_CAP_BASEMAP, false));   // NoSignal
    mixed.push_back(module(true, true, kDecoderBit, true));             // Stopped
    mixed.push_back(module(false, false, 0u, false));                   // Refused
    CHECK(sameCounts(countStates(mixed, true), counts(1, 1, 1, 1, 1, 5)));

    // THE SAME PANEL WITH THE RECEIVER STOPPED. Only the fed module moves, and
    // it moves into notFed - nothing else may shift with it.
    CHECK(sameCounts(countStates(mixed, false), counts(0, 2, 1, 1, 1, 5)));

    // THE CAPTION'S CLAIM. The strip letters `notFed` as decoders that are NOT
    // DECODING. A basemap and a track source declare no decoder and can never
    // decode anything, so neither may appear in that total - in either receiver
    // state.
    std::vector<FittedModule> signalless;
    signalless.push_back(module(true, false, CASCADE_CAP_BASEMAP, false));
    signalless.push_back(module(true, false, CASCADE_CAP_TRACK_SOURCE, false));
    signalless.push_back(module(true, false, CASCADE_CAP_PANEL | CASCADE_CAP_PRESET, false));
    CHECK(sameCounts(countStates(signalless, true), counts(0, 0, 3, 0, 0, 3)));
    CHECK(sameCounts(countStates(signalless, false), counts(0, 0, 3, 0, 0, 3)));

    // THE TOTAL IS THE SUM OF THE FIVE AND NOTHING IS ADDED TOGETHER ON THE
    // WAY. A state folded into another would keep this true; a state counted
    // twice would not.
    const FittedCounts c = countStates(mixed, true);
    CHECK(c.fed + c.notFed + c.noSignal + c.stopped + c.refused == c.total);
    CHECK(c.total == static_cast<int>(mixed.size()));

    // The counts must be the rows: counting the same vector one module at a
    // time gives the same six figures.
    FittedCounts byHand;
    for (const FittedModule& m : mixed) {
        const FittedCounts one = countStates({m}, true);
        byHand.fed += one.fed;
        byHand.notFed += one.notFed;
        byHand.noSignal += one.noSignal;
        byHand.stopped += one.stopped;
        byHand.refused += one.refused;
        byHand.total += one.total;
    }
    CHECK(sameCounts(byHand, c));
}

// ---------------------------------------------------------------------------
// 5. makeModulePlate - what the SHARED plate is told, and what it is not
// ---------------------------------------------------------------------------
void testPlateAdapter() {
    // A FILE REFUSED BEFORE ITS DESCRIPTOR WAS READ. All four identity fields
    // empty, so the plate must be told there was no descriptor at all - drawing
    // "not stated" would report the maker's silence where the truth is our own
    // ignorance.
    FittedModule unread;
    unread.file = "mystery-0.0.1.dll";
    unread.path = "C:/plugins/mystery-0.0.1.dll";
    unread.loaded = false;
    unread.error = "no cascade_plugin_query entry point";
    const ModulePlate up = makeModulePlate(unread);
    CHECK(!up.haveDescriptor);
    CHECK(up.name == unread.file);  // the file name, since nothing else is known
    CHECK(up.fileName == unread.file);
    CHECK(up.fitted);               // a refused file IS a file in the directory
    CHECK(!up.loaded);
    CHECK(!up.running);
    CHECK(up.refusalReason == unread.error);
    CHECK(!up.haveCapabilities);
    CHECK(!up.haveTuneGrant);
    CHECK(!up.haveAbi);
    CHECK(!up.haveSizeBytes);

    // THE DUPLICATE RESOLVER'S CASE, which is the one that separates
    // haveDescriptor from `loaded`: the host read this module in full and THEN
    // turned it off because a newer copy won. Its identity is perfectly known
    // and must not be hatched out.
    FittedModule loser = unread;
    loser.name = "ADS-B";
    loser.version = "1.0.0";
    loser.author = "FoxSDR";
    loser.licence = "PolyForm-Noncommercial-1.0.0";
    loser.capabilities = kDecoderBit;
    loser.error = "a newer copy is loaded: adsb-1.2.0.dll";
    const ModulePlate lp = makeModulePlate(loser);
    CHECK(lp.haveDescriptor);
    CHECK(lp.version == "1.0.0");
    CHECK(lp.maker == "FoxSDR");
    CHECK(lp.licence == "PolyForm-Noncommercial-1.0.0");
    CHECK(!lp.loaded);
    // ...and its capability list is still NOT known here, because nothing of it
    // is loaded. The plate says "not known", never "declares nothing".
    CHECK(!lp.haveCapabilities);
    CHECK(!lp.haveTuneGrant);
    // The identity that IS known is not the file name.
    CHECK(lp.name == loser.file);
    CHECK(lp.fileName == loser.file);

    // A LOADED, STARTED MODULE.
    FittedModule live = module(true, false, kDecoderBit | CASCADE_CAP_HOST_CLIENT, true);
    live.tuneAllowed = true;
    const ModulePlate rp = makeModulePlate(live);
    CHECK(rp.haveDescriptor);
    CHECK(rp.name == "Thing");
    CHECK(rp.loaded);
    CHECK(rp.running);
    CHECK(rp.refusalReason.empty());
    CHECK(rp.haveCapabilities);
    CHECK(rp.capabilities == (kDecoderBit | CASCADE_CAP_HOST_CLIENT));
    CHECK(rp.haveTuneGrant);
    CHECK(rp.tuneGranted);

    // STOPPED IS NOT RUNNING, and the plate's `running` is the coarse
    // definition - loaded and not stopped - not this window's finer FED.
    FittedModule halted = module(true, true, kDecoderBit, true);
    const ModulePlate hp = makeModulePlate(halted);
    CHECK(hp.loaded);
    CHECK(!hp.running);

    // A module that IS loaded and NOT fed is still `running` on the plate: the
    // plate is a description of a module, not a meter.
    FittedModule idle = module(true, false, kDecoderBit, false);
    CHECK(makeModulePlate(idle).running);

    // SIZE. 0 means NOT MEASURED and must never be drawn as a clean zero.
    FittedModule sized = module(true, false, kDecoderBit, true);
    CHECK(!makeModulePlate(sized).haveSizeBytes);
    sized.sizeBytes = 191488;
    const ModulePlate sp = makeModulePlate(sized);
    CHECK(sp.haveSizeBytes);
    CHECK(sp.sizeBytes == 191488u);

    // ABI IS NOT IN A HOST RECORD. LoadedPlugin carries no abiVersion, so a record with no install
    // record behind it has haveAbi false - the plate's "not recorded", which must never be read as a
    // mismatch. True for every record, loaded or not.
    CHECK(!makeModulePlate(live).haveAbi);
    CHECK(!makeModulePlate(halted).haveAbi);
    CHECK(!makeModulePlate(unread).haveAbi);
    // ...but the INSTALL RECORD's ABI (0.99.72) is carried when there is one, compared with this build's.
    FittedModule recorded = module(true, false, kDecoderBit, true);
    recorded.abiVersion = 3;
    const ModulePlate ap = makeModulePlate(recorded);
    CHECK(ap.haveAbi);
    CHECK(ap.abiVersion == 3u);
    CHECK(ap.hostAbiVersion == static_cast<std::uint32_t>(CASCADE_PLUGIN_ABI_VERSION));
    CHECK(modulePageFacts(ap)[3].key == "PLUGIN ABI");
    CHECK(modulePageFacts(ap)[3].value == "3, matches this build");

    // NO CATALOGUE FIELDS ARE INVENTED. A host record carries no summary, no
    // homepage, no legal notice, no platform list and no retirement floor, and
    // an empty retirement floor means NO floor rather than "retire everything".
    CHECK(rp.blurb.empty());
    CHECK(rp.homepage.empty());
    CHECK(rp.legalNotice.empty());
    CHECK(rp.platforms.empty());
    CHECK(rp.retirementFloor.empty());
}

// ---------------------------------------------------------------------------
// 6. makeFittedModule - the adapter that cannot pair the wrong predicate with
//    the wrong field
// ---------------------------------------------------------------------------
void testRecordAdapter() {
    static CascadeHostClientApi kHostClient{};

    LoadedPlugin p;
    p.path = "C:/Program Files/FoxSDR/plugins/adsb-1.2.0.dll";
    p.name = "ADS-B";
    p.version = "1.2.0";
    p.author = "FoxSDR";
    p.licence = "PolyForm-Noncommercial-1.0.0";
    p.capabilities = kDecoderBit | CASCADE_CAP_HOST_CLIENT;
    p.loaded = true;
    p.hostClient = &kHostClient;

    const FittedModule m = makeFittedModule(p, /*stopped=*/true, /*fed=*/false,
                                            "no instance: rate mismatch",
                                            /*tuneAllowed=*/true);
    // THE KEY IS THE FILE NAME, never the display name.
    CHECK(m.file == "adsb-1.2.0.dll");
    CHECK(m.file != m.name);
    CHECK(m.path == p.path);
    CHECK(m.name == "ADS-B");
    CHECK(m.version == "1.2.0");
    CHECK(m.author == "FoxSDR");
    CHECK(m.licence == "PolyForm-Noncommercial-1.0.0");
    CHECK(m.capabilities == p.capabilities);
    CHECK(m.loaded);
    CHECK(m.error.empty());
    CHECK(m.stopped);
    CHECK(!m.fed);
    CHECK(m.idleDetail == "no instance: rate mismatch");
    CHECK(m.tuneAllowed);
    // A record the scan never measured says "not measured" - 0 - and the plate
    // reads that as no size at all, never as a clean zero.
    CHECK(m.sizeBytes == 0u);
    CHECK(!makeModulePlate(m).haveSizeBytes);

    // THE SIZE IS THE RECORD'S, NOT THE FILE'S. The window used to stat the file
    // for it on every frame, on the thread that draws; the scan now measures once
    // and the adapter only carries the figure. The path below names no file, and
    // the size still arrives - which a stat could not have given it.
    p.path = "C:/this/folder/does/not/exist/adsb-1.2.0.dll";
    p.fileBytes = 191488;
    const FittedModule sized = makeFittedModule(p, false, false, "", false);
    CHECK(sized.sizeBytes == 191488u);
    CHECK(makeModulePlate(sized).haveSizeBytes);
    CHECK(makeModulePlate(sized).sizeBytes == 191488u);
    p.path = "C:/Program Files/FoxSDR/plugins/adsb-1.2.0.dll";
    p.fileBytes = 0;

    // TUNE CAPABILITY COMES FROM THE TABLE POINTER, NOT THE BIT. The host
    // clears a table it could not accept, so a module that declared the bit and
    // supplied nothing usable must not be offered a grant it cannot use.
    LoadedPlugin liar = p;
    liar.hostClient = nullptr;
    const FittedModule lm = makeFittedModule(liar, false, false, "", true);
    CHECK((lm.capabilities & CASCADE_CAP_HOST_CLIENT) != 0u);
    CHECK(!lm.tuneCapable);
    CHECK(makeFittedModule(p, false, false, "", false).tuneCapable);

    // A REFUSED RECORD. The error travels; the descriptor fields do not exist.
    LoadedPlugin bad;
    bad.path = "C:/plugins/broken-9.9.9.dll";
    bad.loaded = false;
    bad.error = "ABI mismatch: expected 3, plugin reports 2";
    const FittedModule bm = makeFittedModule(bad, false, false, "", false);
    CHECK(bm.file == "broken-9.9.9.dll");
    CHECK(bm.name.empty());
    CHECK(!bm.loaded);
    CHECK(bm.error == bad.error);
    CHECK(!bm.tuneCapable);
    CHECK(fittedState(bm, true) == FittedState::Refused);
    CHECK(fittedStateSentence(bm, true) == bad.error);

    // A RECORD WITH NO PATH produces an empty key, which the stop set is
    // documented never to match.
    LoadedPlugin pathless;
    pathless.loaded = true;
    pathless.name = "Nameless";
    CHECK(makeFittedModule(pathless, false, false, "", false).file.empty());
}

// ---------------------------------------------------------------------------
// 7. The two windows may be coarser than each other, never contradictory
// ---------------------------------------------------------------------------
void testWindowsAgree() {
    // THE PAGE'S ON THIS MACHINE FACT (moduleMachineText) is the coarser half of this window's state.
    struct Case {
        FittedModule m;
        bool running;
        FittedState state;
        const char* machineText;
    };
    std::vector<Case> cases;
    cases.push_back({module(true, false, kDecoderBit, true), true, FittedState::Fed,
                     "fitted and started"});
    cases.push_back({module(true, false, kDecoderBit, false), true, FittedState::NotFed,
                     "fitted and started"});
    cases.push_back({module(true, false, CASCADE_CAP_BASEMAP, false), true,
                     FittedState::NoSignal, "fitted, takes no signal"});
    cases.push_back({module(true, true, kDecoderBit, true), true, FittedState::Stopped,
                     "fitted, stopped"});
    FittedModule refused = module(false, false, 0u, false);
    refused.name.clear();
    refused.error = "refused";
    cases.push_back({refused, true, FittedState::Refused, "fitted, refused"});

    for (const Case& c : cases) {
        CHECK(fittedState(c.m, c.running) == c.state);
        CHECK(moduleMachineText(makeModulePlate(c.m)) == c.machineText);
    }
    // FED and NOT FED are the finer half of "fitted and started": the page must not claim either,
    // since it is handed neither the runner's table nor the receiver.
    const ModulePlate fedPlate = makeModulePlate(cases[0].m);
    CHECK(moduleMachineText(fedPlate).find("FED") == std::string::npos);
    CHECK(moduleMachineText(fedPlate).find("fed") == std::string::npos);
}

// ---------------------------------------------------------------------------
// 8. the row's note: the reach warning, the refusal, the orphan - and silence
// ---------------------------------------------------------------------------
void testRowNote() {
    // "PUBLISHES TO THE HOST ONLY" GETS NOTHING under the name: a warning on every row is no warning.
    FittedModule quiet = module(true, false, kDecoderBit | CASCADE_CAP_PANEL, true);
    CHECK(fittedRowNote(quiet, true).text.empty());
    CHECK(fittedRowNote(module(true, false, CASCADE_CAP_TRACK_SOURCE, false), true).text.empty());
    // A module that reaches outward carries the reach warning, in amber (warning), not as a refusal.
    FittedModule asks = module(true, false, kDecoderBit | CASCADE_CAP_HOST_CLIENT, true);
    FittedRowNote n = fittedRowNote(asks, true);
    CHECK(n.text == "asks to move the receiver");
    CHECK(n.warning);
    CHECK(!n.refusal);
    asks.tuneAllowed = true;
    CHECK(fittedRowNote(asks, true).text == "granted: may move the receiver");
    n = fittedRowNote(module(true, false, CASCADE_CAP_BASEMAP, false), true);
    CHECK(n.text == "may fetch from a server it chose");
    CHECK(n.warning);
    n = fittedRowNote(module(true, false, CASCADE_CAP_TRACK_INFO, false), false);
    CHECK(n.text == "may fetch from a server it chose");

    // A REFUSED MODULE'S LINE IS THE HOST'S REASON, VERBATIM, MUTED (a refusal, not a warning).
    FittedModule refused = module(false, false, 0u, false);
    refused.name.clear();
    refused.error = "ABI mismatch: expected 3, plugin reports 2";
    n = fittedRowNote(refused, true);
    CHECK(n.text == refused.error);
    CHECK(n.refusal);
    CHECK(!n.warning);
    // ...and with no reason recorded, the sentence that says so.
    refused.error.clear();
    CHECK(has(fittedRowNote(refused, true).text, "recorded no reason"));

    // AN ORPHAN'S says first that it was not installed from the store, the host's words after it.
    FittedModule orphan = module(false, false, 0u, false);
    orphan.name.clear();
    orphan.error = "plugin reports ABI version 2, expected 3";
    orphan.orphaned = true;
    n = fittedRowNote(orphan, true);
    CHECK(n.text == fittedOrphanSentence(orphan));
    CHECK(has(n.text, "Not installed from the plugin store"));

    // A stopped or unfed module with no outward reach says nothing under its name: the state word does.
    CHECK(fittedRowNote(module(true, true, kDecoderBit, true), true).text.empty());
    CHECK(fittedRowNote(module(true, false, kDecoderBit, false), false).text.empty());
}

// ---------------------------------------------------------------------------
// 9. the verdict line, the rows the chips leave, and the fitted date
// ---------------------------------------------------------------------------
void testVerdictAndRows() {
    // THE ONE MUTED LINE: the receiver's sentence, a dot, and where the scan looked.
    CHECK(fittedVerdictLine(true, "C:/Users/x/AppData/Local/foxsdr/plugins") ==
          "The receiver is running, so a module with a matched decoder is being fed.  \xc2\xb7  read from "
          "C:/Users/x/AppData/Local/foxsdr/plugins");
    const std::string stopped = fittedVerdictLine(false, "/home/x/plugins");
    CHECK(has(stopped, "The receiver is stopped, so NOTHING is being fed to any module"));
    CHECK(has(stopped, "  \xc2\xb7  read from /home/x/plugins"));
    // A scan that has not happened says so rather than naming an empty folder.
    CHECK(has(fittedVerdictLine(true, ""), "No directory has been scanned yet."));
    CHECK(!has(fittedVerdictLine(true, ""), "read from"));

    // THE ROWS: sorted by name (the file name for a module nobody read), filtered by the five chips and the search.
    std::vector<FittedModule> mods;
    FittedModule b = module(true, false, kDecoderBit, true);
    b.name = "Bravo";
    b.file = "bravo-1.0.0.dll";
    FittedModule a = module(true, false, CASCADE_CAP_BASEMAP, false);
    a.name = "alpha";
    a.file = "alpha-1.0.0.dll";
    FittedModule c = module(true, true, kDecoderBit, true);
    c.name = "Charlie";
    c.file = "charlie-1.0.0.dll";
    FittedModule z = module(false, false, 0u, false);
    z.name.clear();
    z.file = "zeta-0.0.1.dll";
    z.error = "refused";
    mods = {b, a, c, z};
    FittedModulesDeck deck;
    std::vector<int> rows = fittedVisibleRows(mods, true, deck);
    CHECK((rows == std::vector<int>{1, 0, 2, 3}));  // alpha, Bravo, Charlie, zeta
    // Each chip is a toggle: off hides exactly its own state.
    deck.showFed = false;
    CHECK((fittedVisibleRows(mods, true, deck) == std::vector<int>{1, 2, 3}));
    deck.showFed = true;
    deck.showNoSignal = false;
    CHECK((fittedVisibleRows(mods, true, deck) == std::vector<int>{0, 2, 3}));
    deck.showNoSignal = true;
    deck.showStopped = false;
    deck.showRefused = false;
    CHECK((fittedVisibleRows(mods, true, deck) == std::vector<int>{1, 0}));
    deck.showStopped = true;
    deck.showRefused = true;
    // The receiver stopped moves Fed into Not fed, and the "NOT DECODING" chip (showIdle) holds it.
    deck.showIdle = false;
    CHECK((fittedVisibleRows(mods, true, deck) == std::vector<int>{1, 0, 2, 3}));
    CHECK((fittedVisibleRows(mods, false, deck) == std::vector<int>{1, 2, 3}));
    deck.showIdle = true;
    // THE SEARCH: name, file or version, any case.
    std::snprintf(deck.search, sizeof deck.search, "%s", "CHARLIE");
    CHECK((fittedVisibleRows(mods, true, deck) == std::vector<int>{2}));
    std::snprintf(deck.search, sizeof deck.search, "%s", "zeta-0.0.1");
    CHECK((fittedVisibleRows(mods, true, deck) == std::vector<int>{3}));
    std::snprintf(deck.search, sizeof deck.search, "%s", "nothing like this");
    CHECK(fittedVisibleRows(mods, true, deck).empty());

    // FITTED <DATE>, only when the install record says when - never 1970.
    FittedModule dated = module(true, false, kDecoderBit, true);
    CHECK(fittedDateText(dated).empty());
    dated.fittedAtUnix = 1790000000;  // October 2026
    const std::string fd = fittedDateText(dated);
    CHECK(fd.rfind("fitted 20", 0) == 0);
    CHECK(fd.size() == std::string("fitted 2026-10-02").size());
}

// ---------------------------------------------------------------------------
// 10. THE PAGE'S ON THIS MACHINE FACTS, for a module the catalogue does not know
// ---------------------------------------------------------------------------
void testPageFactsForAFittedModule() {
    FittedModule m = module(true, false, kDecoderBit | CASCADE_CAP_BASEMAP, true);
    m.sizeBytes = 191488;
    m.abiVersion = 3;
    const std::vector<cascade::gui::PageFact> f = modulePageFacts(makeModulePlate(m));
    std::vector<std::string> keys;
    for (const auto& x : f) { keys.push_back(x.key); }
    // Fitted: ON THIS MACHINE and FILE follow the catalogue-shaped facts; none is invented.
    CHECK((keys == std::vector<std::string>{"MAKER", "LICENCE", "VERSION", "PLUGIN ABI", "DOWNLOAD",
                                            "BUILDS FOR", "REACHES", "HOMEPAGE", "SHA-256", "PUBLISHED",
                                            "ON THIS MACHINE", "FILE"}));
    CHECK(f[0].value == "A Maker");
    CHECK(f[3].value == "3, matches this build");
    CHECK(f[4].value == "191 kB");
    CHECK(f[5].hatched);   // a host record names no builds
    CHECK(f[6].value == "Audio decoder, Map imagery");
    CHECK(f[7].hatched && f[8].hatched && f[9].hatched);
    CHECK(f[10].value == "fitted and started");
    CHECK(f[11].value == "thing-1.0.0.dll");
}

// ---------------------------------------------------------------------------
// 11. the five chips' captions are the five states' words
// ---------------------------------------------------------------------------
void testChipCounts() {
    // The counts behind the chips ARE countStates, one counter per state: a basemap is not "NOT DECODING".
    std::vector<FittedModule> mods = {module(true, false, kDecoderBit, true), module(true, false, kIqBit, false),
                                      module(true, false, CASCADE_CAP_BASEMAP, false),
                                      module(true, true, kDecoderBit, true), module(false, false, 0u, false)};
    const FittedCounts c = countStates(mods, true);
    CHECK(c.fed == 1 && c.notFed == 1 && c.noSignal == 1 && c.stopped == 1 && c.refused == 1);
    // The window's rows are exactly the sum of what the chips count.
    FittedModulesDeck deck;
    CHECK(fittedVisibleRows(mods, true, deck).size() == static_cast<std::size_t>(c.total));
}

// ORPHANED FILES (0.99.69): the one line a row carries, and what it must not do.
// The classification itself is core's and is tested on real files in
// test_plugin_cleanup; what is pinned here is the window's half of it - the
// host's words come through verbatim after the plugin-index sentence (they carry
// numbers a paraphrase would lose, as the refusal sentence's do), a file that is
// not an orphan has no such line, and the flag changes no state.
void testOrphans() {
    FittedModule refused = module(false, false, 0u, false);
    refused.name.clear();
    refused.error = "plugin reports ABI version 2, expected 3";

    // Not an orphan unless the application says so: a built record, a refused one,
    // a running one.
    CHECK(!refused.orphaned);
    CHECK(!module(true, false, kDecoderBit, true).orphaned);
    CHECK(fittedOrphanSentence(refused).empty());
    CHECK(fittedOrphanSentence(module(true, false, kDecoderBit, true)).empty());
    LoadedPlugin lp;
    lp.path = "/p/thing-1.0.0.dll";
    CHECK(!makeFittedModule(lp, false, false, "", false).orphaned);

    // An orphan: the plugin-store sentence, then the host's words EXACTLY.
    refused.orphaned = true;
    CHECK(fittedOrphanSentence(refused) ==
          "Not installed from the plugin store and not running: plugin reports ABI version 2, "
          "expected 3");
    // ...and with no reason recorded, no colon and nothing invented.
    FittedModule silent = refused;
    silent.error.clear();
    CHECK(fittedOrphanSentence(silent) == "Not installed from the plugin store and not running.");
    // One line: the host's own text is not edited into several.
    CHECK(fittedOrphanSentence(refused).find('\n') == std::string::npos);

    // The flag is about REMOVAL; it is not a sixth state. A refused file stays REFUSED,
    // and the counts do not move.
    CHECK(fittedState(refused, true) == FittedState::Refused);
    std::vector<FittedModule> two = {refused, module(true, false, kDecoderBit, true)};
    CHECK(sameCounts(countStates(two, true), counts(1, 0, 0, 0, 1, 2)));
}

}  // namespace

int main() {
    testStateSweep();
    testImpossibleInputs();
    testStateWords();
    testSentences();
    testCounts();
    testPlateAdapter();
    testRecordAdapter();
    testWindowsAgree();
    testOrphans();
    testRowNote();
    testVerdictAndRows();
    testPageFactsForAFittedModule();
    testChipCounts();
    return testSummary("test_plugins_view");
}
