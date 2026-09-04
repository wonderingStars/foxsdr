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
//   STOPPED BY YOU, REFUSED against REFUSED.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cstdint>
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
using cascade::gui::fittedState;
using cascade::gui::FittedState;
using cascade::gui::fittedStateSentence;
using cascade::gui::fittedStateWord;
using cascade::gui::makeFittedModule;
using cascade::gui::makeModulePlate;
using cascade::gui::ModulePlate;
using cascade::gui::moduleStateWord;

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
    CHECK(notFed == "NOT FED");
    CHECK(noSignal == "TAKES NO SIGNAL");
    CHECK(stopped == "STOPPED BY YOU");
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
    CHECK(!has(noSignal, "NOT FED"));
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

    // ABI IS NEVER RECORDED FROM A HOST RECORD. LoadedPlugin carries no
    // abiVersion, and haveAbi false is the plate's "not recorded" - which must
    // never be read as a mismatch. True for every record, loaded or not.
    CHECK(!makeModulePlate(live).haveAbi);
    CHECK(!makeModulePlate(halted).haveAbi);
    CHECK(!makeModulePlate(unread).haveAbi);

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
    CHECK(m.sizeBytes == 0u);  // never measured by this adapter

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
    struct Case {
        FittedModule m;
        bool running;
        FittedState state;
        const char* plateWord;
    };
    std::vector<Case> cases;
    cases.push_back({module(true, false, kDecoderBit, true), true, FittedState::Fed,
                     "STARTED"});
    cases.push_back({module(true, false, kDecoderBit, false), true, FittedState::NotFed,
                     "STARTED"});
    cases.push_back({module(true, false, CASCADE_CAP_BASEMAP, false), true,
                     FittedState::NoSignal, "TAKES NO SIGNAL"});
    cases.push_back({module(true, true, kDecoderBit, true), true, FittedState::Stopped,
                     "STOPPED"});
    FittedModule refused = module(false, false, 0u, false);
    refused.name.clear();
    refused.error = "refused";
    cases.push_back({refused, true, FittedState::Refused, "REFUSED"});

    for (const Case& c : cases) {
        CHECK(fittedState(c.m, c.running) == c.state);
        CHECK(std::string(moduleStateWord(makeModulePlate(c.m))) == c.plateWord);
    }
    // FED and NOT FED are the finer half of STARTED: the plate must not claim
    // either, since it is handed neither the runner's table nor the receiver.
    const ModulePlate fedPlate = makeModulePlate(cases[0].m);
    CHECK(std::string(moduleStateWord(fedPlate)) != "FED");
    CHECK(std::string(moduleStateWord(fedPlate)) != "NOT FED");
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
    return testSummary("test_plugins_view");
}
