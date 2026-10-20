// test_command_path_guard.cpp - only the Engine changes the receiver; the
// window submits commands (engine extraction stages 1 and 3a,
// docs/engine-stage1.md section 4 and docs/engine-stage3.md).
//
// THE RULE IT HOLDS. Since stage 1 every desktop control that changes the
// receiver submits a FoxCommand, and applyCommand - with the engine machinery
// it calls - is the only code that changes it. Since stage 3a that machinery
// is the Engine's (src/engine), and the window (gui::AppWindow) holds the
// Engine and asks it. A control written the old way would quietly reopen the
// second control path stage 1 closed, and in stage 3b - when the engine runs
// on its own control thread - it would race it. So the source is read, the
// way tests/test_stop_ends_recordings reads it for pipeline_.stop():
//
// WHAT IS SCANNED. Every .cpp in src/gui (an AppWindow member can be defined
// in any file), cut into DEFINITIONS at every column-0 function head; an
// AppWindow member is a definition named AppWindow::x, and a free function
// counts when its file also defines AppWindow members. And the ENGINE's
// surface, read from src/engine: every Engine::x it defines, which of those
// its header declares `const` (a query), and every data member its header
// declares. Comments and string literals are ignored. The window reaches the
// engine through `engine_.`; the rules read "engine_." as if it were not
// there, so `engine_.pipeline_.stop()` is judged as `pipeline_.stop()` was.
//
// THREE KINDS OF WINDOW DEFINITION.
//   WINDOW MACHINERY (kWindowMachinery): construction, teardown, the config
//     restore - the few window members that still drive
//     the engine directly - and the EngineHost overrides (kHostHooks): the
//     window's side of calls the ENGINE makes. Allowed to reach the engine;
//     rule 5 keeps them free of ImGui input.
//   LINE-ALLOWED MEMBERS (kLineAllowed): members that draw AND carry a few
//     reviewed engine lines (drawUi's per-frame pump steps and transmit key;
//     run's startup, teardown and verification seams). Only the listed lines are
//     exempt. (The patch runtime's steps were drawPatchPage's and drawPatchView's
//     until window port group 13 moved them into the engine's pass.)
//   CONTROLS: everything else - every widget, key handler, gesture and new
//     helper. In a control:
//     1. no call to a state-changing helper (kHelpers), a window-machinery
//        member, or an Engine method that is not a const query - except the
//        command path itself (kControlMayCall);
//     2. no call on an engine object (kEngineObjects) except the reviewed
//        read-only ones (kReadOnly); a bare accessor that hands out the
//        object (pipeline_.activeSource()) only into a `const` binding;
//     3. no write to the Engine's state: any receiver field (kFields) and
//        every data member the Engine declares - plain and compound
//        assignment, ++/--, taking its address, a subscripted write, or a
//        mutating container call - with or without `this->`. The Engine
//        fields the window still edits IN PLACE are listed
//        (kWindowMayWriteScoped, each paired with the ONE member allowed to
//        write it - a write from any other member is rule 3's violation
//        exactly as if the field were not listed at all; kWindowMayWriteUnscoped
//        for a field written from many members), each an OPEN item for stage
//        3b in docs/engine-stage3.md. BOTH LISTS ARE EMPTY since OPEN 6's
//        graph half closed: the last entry, patchGraph_, is now written only
//        through FOXAPP_OP_PATCH_SET_GRAPH. A mutating call includes the
//        patch graph's own (addNode, mutableNode, removeNode, connect ...);
//     4. no engine object passed as an argument (a free helper taking
//        Pipeline& would otherwise change state on a control's behalf).
//   5. window machinery contains no ImGui input (Button, Checkbox, Slider,
//      IsKeyPressed ...); src/engine contains no ImGui at all.
//   6. every name on every allow-list exists in the tree (an AppWindow member
//      in src/gui, or an Engine member in src/engine), so a stale entry
//      cannot quietly allow a future member of the same name.
//
// A new helper that must change state belongs in the Engine. The per-op test
// (test_apply_command), the headless engine test (test_engine_headless) and
// review are the other nets; an indirection none of the lists names (a
// stored lambda called later, a pointer to an engine field kept in a window
// member) is not seen here.
//
// Proven red (docs/engine-stage1.md; docs/engine-stage3.md for stage 3a): a
// setter in drawRadioSection; volume_ assigned in drawToolbar; a setter in a
// helper outside the list; the stage-1 reviewer's probes; and, since 3a, a
// window control calling a non-const Engine method, and a window control
// writing an Engine field that is not a receiver field.
//
// argv[1], optional: a root to scan instead of the source tree (used to run
// the probes against a copy).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "test_check.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// --- rule 1: helpers that change the receiver --------------------------------
const char* const kHelpers[] = {
    "startReceiver(", "stopReceiver(", "retuneSourceHz(", "tuneAbsoluteHz(", "setVfoToAbsoluteHz(",
    "applyRetuneNow(", "setModeIndex(", "requestAudioOpen(", "applyPluginPreset(", "loadBandPlan(",
    "selectSource(", "scanNative(", "scanSoapy(", "openPlutoFromBox(",
    "openPlutoAt(", "openIqFile(", "launchSoundCardOpen(", "switchBiasTee(", "changeConverter(",
    "installSource(", "withRfNotch(", "withDabNotch(", "withHdrMode(", "withAdcSwitches(",
    "startIqRecording(", "startAudioRecording(", "stopIqRecording(", "stopAudioRecording(",
    "importBookmarkFile(", "addBookmarkHere(", "tuneToBookmark(", "setPluginStopped(",
    "recordPluginStopped(", "setPluginMutes(", "setPluginTuneAllowed(", "setPluginSettingsAllowed(",
    "rescanPlugins(", "startCatalogFetch(", "startInstall(", "startUpdate(", "startAddAll(",
    "removeInstalledPlugin(", "removeBlockedPlugin(", "maybeAutoPreset", "stopMutingPlugins(",
    "applyUserPresetEdit(", "patchPressStart(", "patchAllOff(", "openTransmitRadio(",
    "closeTransmitRadio(", "applyReceiverPosition(", "selectSourceById(", "followInputRate(",
    "applyConverterForSource(", "endTakes(",
};

// --- rule 2: engine objects, and what a control may ask of them ---------------
const char* const kEngineObjects[] = {
    "pipeline_",    "device_",      "soapyView_",   "transmitter_",  "freqMgr_",
    "scanner_",     "iqRecorder_",  "audioRecorder_", "gpsReader_",  "pluginRepo_",
    "pluginHost_",  "pluginRunner_", "pluginUi_",   "micOpen_",      "audioOpen_",
    "retuneCoalescer_",
};

// Calls a control may make: "object.method", or "object.accessor().method". Reads of an engine object that survived the
// port: only what the three residue classes (the shell's seams, the host accessors, stage 5's web feeds) still need. EVERY
// ENTRY MUST BE MATCHED by a call in a control (the check in main(): a stale entry fails), so the list is exactly the set of
// engine-object calls the window still makes - and it is the fence's list of what the interface may NOT name.
// (RETIRED with window port group 14 part e, their last caller gone - 63 entries the earlier groups left behind, found by the
// guard itself now and confirmed by a spelling-agnostic search of src/gui (`.` and `->`, an accessor between): audioOpen_ and
// micOpen_ .inFlight / micOpen_.result().ok, audioRecorder_ and iqRecorder_ .sizeLimitReached / .writeFailed, device_
// .deviceDead / .frequencyRangeHz / .name / .lastError / .gainDb / .antenna / .autoGain, freqMgr_ .list().empty / .range /
// .nearestSubset / .indexOfId, eighteen pipeline_ reads (activeSource().sampleRateHz / .faulted, activeSourceName, audio()
// .everOpened / .openedDeviceName / .streamAlive / .underruns, audioMuted, autoNotchEngaged / FrequencyHz, converter,
// getLatestFrame, pilotLevel / Locked, rdsSnapshot, running, stereoActive, rawSource().centerFrequencyHz), pluginHost_
// .loadedCount, pluginRunner_ .activeCount / .audioFramesFed / .audioGapFrames / .audioGaps / .status / .iqFramesFed, pluginUi_
// .panels / .paths / .tracks().size and .tuneRequesters().size, soapyView_ .deadReason / .deviceDead and fourteen transmitter_
// reads. Thirteen of them (the pipeline_, pluginHost_, pluginRunner_ and pluginUi_ ones) had their last caller in the two
// bounded-run diagnostics that part e moved into the engine; the others are leftovers of the groups that ported the sections they
// were read for. Earlier retirements are in the notes of their groups, in docs/engine-stage4.md.)
const char* const kReadOnly[] = {
    "pipeline_.activeSource().centerFrequencyHz", "pipeline_.inputRateHz", "pipeline_.audioSamplesProduced",
    "pipeline_.audioTap", "pluginHost_.plugins", "pluginRunner_.pollImages", "pluginUi_.instruments",
    "pluginUi_.panels", "pluginUi_.paths", "pluginUi_.trackPluginNames", "pluginUi_.tracks", "pluginUi_.altitudeNear",
    "pluginUi_.poll",
};

// Accessors that hand the object out; a control may bind them only `const`.
const char* const kBareAccessors[] = {"pipeline_.activeSource", "pipeline_.audio",
                                      "pipeline_.rawSource"};

// --- rule 3: the receiver's fields ----------------------------------------------
const char* const kFields[] = {
    "vfoOffsetKhz_", "vfoBandwidthHz_", "bandwidthIndex_", "modeIndex_", "squelchDb_", "volume_",
    "userMuted_", "deemphIndex_", "stereoEnabled_", "nrEnabled_", "nrStrength_", "notchEnabled_",
    "notchFreqHz_", "notchQ_", "autoNotch_", "dbMin_", "dbMax_", "bandPlanSelection_", "deviceAgc_",
    "deviceAntenna_", "deviceRateIndex_", "deviceGainsDb_", "deviceRfNotch_", "deviceDabNotch_",
    "deviceHdr_", "deviceDither_", "deviceRandomiser_", "lookForNetworkUsrps_", "soundCardLive_",
    "transmitModeIndex_", "transmitSplit_", "transmitSplitHz_", "transmitPowerDb_",
    "transmitInputIndex_", "transmitToneHz_", "transmitMonitor_", "rxLat_", "rxLon_", "rxSet_",
    "pluginsStopped_", "pluginTuneAllowed_", "pluginSettingsAllowed_", "pluginMuteOverride_",
    "userPresets_", "patchRunning_", "converters_", "device_", "sourceKind_",
};

// --- the window's machinery ---------------------------------------------------------
// The window members that still drive the engine directly: construction,
// teardown and the config restore. (The web/CAT drain left with the pump move's step P1: the ENGINE pulls the two servers'
// queues in its own pass, Engine::pumpRemoteControls, through the source the constructor installs.)
const char* const kWindowMachinery[] = {
    "AppWindow",
    "~AppWindow",
    "applyConfig",
    "currentConfig",
    // hands the engine what only the window knows for the publish - its web
    // listener, the counter's face, the basemap's figures (engine/stage3b-pre
    // OPEN 4: these were EngineHost hooks the engine called while publishing)
    "handFrontEndFacts",
};

// The EngineHost overrides (engine/engine_host.hpp): the window's half of a
// call the ENGINE makes, on the engine's behalf - it pauses the watchdog, answers a
// question about the clock or the plugin UI. Judged as machinery: rule 5 applies, rules
// 1-4 do not.
//
// THE PUMP MOVE'S STEP P2 TOOK EIGHT OUT (18 -> 10), each a call the engine made from its pass that WROTE or READ window state:
//   onDisplayRange        the spectrum's range follows the state (refreshUiState);
//   onBookmarksChanged    dead - its body was empty since group 7;
//   onConverterChanged    the LO field re-seeds from the radio and stored setting it reads (gui/converter_lo_seed.hpp);
//   onCatalogueResult     the window's override read its frame counter from the pass: the report switch and the frame are handed
//                         over in FrontEndFacts and the ENGINE prints (the base class keeps the virtual for a headless front end);
//   openPluginWindowsFor  an EVENT, FOXAPI_EVENT_PRESET_APPLIED (API 0.3 8.14), answered in pumpUiEvents;
//   drainTrackInfoText    the window polls its cache's text in its own frame and sends FOXAPP_OP_TRACK_INFO_LINE;
//   enrichWebTrack and fillWebImages   handed over in FrontEndFacts::webExtras (webExtrasNow).
// What is left: the clocks and the watchdog (thread-safe by construction, engine/stage3b-pre OPEN 4), and the five that belong to
// the plugin UI - the unload handshake and the instances the GUI polls - which is step P3's (docs/engine-stage4.md).
const char* const kHostHooks[] = {
    "frameClockRunning",  "frameTimeS",          "wallTimeS",
    "pauseWatchdog",      "resumeWatchdog",
    // (onReceiverPositionApplied left with window port group 14 part a: the window follows the position in the state.)
    // (onPatchPicture left with window port group 13 part c: the pictures are the engine's, read with read_image.)
    // (onGpsFixApplied left with part b: the GPS list's entry going to Fixed is the cue.)
    // (onCatalogueFetchStarting and onAddAllFinished left with window port group 10: the window watches the table instead.)
    "beforePluginRescan",
    "onPluginsUnloading", "attachBasemap",       "attachTrackInfo",
    "showDemonstrationInstrument",
    // webListening, tunerDisplayStyle, basemapFacts: no longer hooks
    // (engine/stage3b-pre OPEN 4) - handed over by handFrontEndFacts.
};

// Calls a control MAY make to a state-changing name: the command path itself,
// and the few non-const Engine methods that change no receiver state.
const char* const kControlMayCall[] = {
    "applyCommand", "submitCommand", "AppWindow", "~AppWindow",
    // The engine's API handle (engine stage 4): the window opens its LOCAL
    // session on it (openUiSession), and what it changes through that
    // session goes by the table's submit - the command path itself.
    "apiHandle",
    // (telemetryNotePanel was allowed here until window port group 3: the
    // window names a panel through AppWindow::uiNotePanel now, as
    // FOXAPP_OP_NOTE_PANEL, so a direct call is rule 1's again.)
    // the config snapshot the save compares and writes (window machinery; it accrues the usage report)
    "currentConfig",
    // (currentAbsoluteHz, "a query: the tuned frequency", was allowed here until window
    // port group 6: its last caller in src/gui, the frequency readout, reads the
    // state's tunedHz now, so a direct call is rule 1's again.)
    // (RETIRED with window port group 13 part b, their last caller gone: carriedAirCentre - the seed reads
    // fact.patch.receiverCentre and the state's centreHz - patchListRecordings - the Radio part sends
    // FOXAPP_OP_PATCH_LIST_RECORDINGS - and prunePatchSinkLines - the engine pruned in its own pass, the window's Text
    // outs keep their own lines; in part c the map it pruned, patchSinkLines_, went too: nothing read it.)
    // a pure function of its argument (static)
    "muteNameList",
    // (refreshDiagContext RETIRED with window port group 14 part c: the engine's pass renders the context the crash handler
    // prints and the diagnostics bundle carries; the window never calls it.)
    // scanSoundCards CLOSED engine/stage3b-pre OPEN 2: the panel sends
    // FOXAPP_OP_SOUND_CARDS_WANTED.
    // The config's receiver half, handed over NOW for a save that must be
    // current (saveConfigNow: shutdown, a language switch) rather than as the
    // last frame left it (engine/stage3b-pre OPEN 2). It changes no receiver
    // state - it refreshes the copy currentConfig() reads; once the engine
    // runs on its own thread this is a request answered by that thread.
    "publishConfig",
    // asAirspyDevice/chooseAirspyDecimation/GainMode/Agc CLOSED
    // engine/stage3b-pre Airspy round (OPEN 2/3): FOXAPP_OP_AIRSPY_DECIMATION/
    // GAIN_MODE/AGC, and the published state (PublishedState::app's airspy*
    // fields) in place of the raw device pointer. tests/test_airspy_app.cpp
    // is unaffected - it calls the Engine methods directly (a friend
    // accessor), which still exist, unchanged, for the commands to call.
    "airspyRememberOpen",
};

// ENGINE FIELDS THE WINDOW STILL EDITS IN PLACE. Each is a form or a page
// state the engine reads when it acts (docs/engine-stage3.md, OPEN: stage 3b
// must turn each into a command or a form the command carries, because a
// write from the GUI thread races the control thread). Rules 1-4 still
// judge every other engine field.
//
// SCOPED (engine/stage3b-pre 2d, docs/engine-stage3.md OPEN item 1): each
// entry names the ONE AppWindow member allowed to write it - not "any
// control", which is what a flat list (the form this used to be) actually
// allowed. A control OTHER than the named owner writing a listed field is a
// rule-3 violation exactly like writing a field not on this list at all.
// Found this way, before it was fixed: a probe writing transmitOpen_ AND
// soundCard_ from drawToolbar (neither of which drawToolbar owns) passed the
// old flat-list guard silently.
struct ScopedWrite {
    const char* field;
    const char* member;
};
const std::vector<ScopedWrite> kWindowMayWriteScoped = {
    // CLOSED, engine/stage3b-pre fields-to-commands rounds 2-3: the 7
    // scanner-form fields (the existing FOXAPI_OP_SCANNER_CONFIG for the four
    // timings + the existing FOXAPP_OP_SCANNER_RANGE for the range),
    // soundCard_ (FOXAPP_OP_SOUND_CARD_FORM), plutoUri_/transmitArgs_ (a
    // window-local draft, never the engine field - FOXAPP_OP_SET_PLUTO_URI/
    // FOXAPP_OP_SET_TRANSMIT_ARGS as typed, FOXAPI_OP_SELECT_SOURCE/
    // FOXAPI_OP_TX_OPEN on Open), pluginCatalogueUrl_
    // (FOXAPP_OP_SET_CATALOGUE_URL), telemetryEnabled_/telemetryInstallId_
    // (FOXAPP_OP_TELEMETRY_CONSENT, the desktop's own op; the API's
    // FOXAPI_OP_TELEMETRY_ENABLE is refused from everyone), patchListsWanted_
    // (FOXAPP_OP_PATCH_LOOK_FOR_RADIOS) and patchSinkLines_
    // (Engine::prunePatchSinkLines(), kControlMayCall below), and
    // muteKeptRunning_/mutePopup_ (FOXAPP_OP_MUTE_KEEP_RUNNING, both fields
    // together - one user decision, not two independent clears).
    //
    // EMPTY NOW - every field that was ever on this list has a command or a
    // reviewed direct call; so is kWindowMayWriteUnscoped below (OPEN item
    // 6's graph half closed). A std::vector, not a C array, so an empty list compiles
    // (an empty-initialised array's bound cannot be deduced) - the moment
    // this is non-empty again, `= {...}` list-initialises it exactly the
    // same way.
};
// UNSCOPED: a field written from many controls, not paired with one owner.
// EMPTY NOW. Its last entry was patchGraph_, written from a dozen-odd members
// of the patch canvas, faces and inspector until OPEN item 6's graph half
// closed (engine/stage3b-pre, Design A): the page edits a draft of its own
// (AppWindow::patchDraft_) and the Engine takes the whole graph through
// FOXAPP_OP_PATCH_SET_GRAPH. A std::vector for the same reason as above.
const std::vector<const char*> kWindowMayWriteUnscoped = {
};

struct LineAllow {
    const char* member;
    const char* code;  // the exact code the line must contain
};
const LineAllow kLineAllowed[] = {
    // drawUi IS the frame loop's body: the command drains, the remote and
    // plugin applies, the scanner, and the per-frame machinery polls.
    {"drawUi", "drainLocalCommands();"},
    {"drawUi", "pumpRemoteControls();"},
    {"drawUi", "(void)applyCommand(q.c, q.longText);"},
    // ...and the transmit key rebuilt every frame - the dead-man's handle (OPEN 1).
    // (drawPatchPage and drawPatchView's runtime steps - the receiver pipeline's patch runner retired and cleared, the
    // close rule that stops the patch, the radios reconciled, the transport applied, the catalogue rebuilt, the one
    // compile, the sets published - LEFT with window port group 13: the engine's pass runs them, Engine::pumpPatch,
    // and the window tells it whether the view is showing. Sixteen entries.)
    // drawTransmitPage's followTransmitFrequency CLOSED (engine stage 4 window
    // port): the engine's own pass follows the dial (Engine::pumpPublish).
    // run: interactive start, the GPS test seam, the catalogue test hook, the
    // device seam's scan, the preset seam for a plugin with none (OPEN 5),
    // FOXSDR_PATCH_START (OPEN 5), the measurement hook, and the teardown.
    {"run", "startReceiver();"},
    // (The FOXSDR_GPS_PORT seam's gpsReader_.start line LEFT with window port group 14 part e: the seam sends the key's own
    // FOXAPI_OP_GPS through the session now.)
    // the bounded-run diagnostic, the engine's since part e: printed once after the teardown's first phase, when the run asked
    {"run", "reportPluginStatus(pluginImages_);"},
    {"run", "startCatalogFetch();"},
    {"run", "scanNative();"},
    {"run", "recordPluginStopped(cascade::core::pluginKey(lp), false);"},
    {"run", "patchRunning_ = true;"},
    {"run", "installSource(std::move(src));"},
    {"run", "followInputRate();"},
    {"run", "if (!measure_->tick(pipeline_, hooks)) {"},
    {"run", "pollGpsReader();"},
    // the engine's teardown, in its three ordered phases between the
    // window's own steps (engine/stage3b-pre OPEN 9 - they were eight lines
    // here: the transmitter, the GPS reader, the recordings, the bookmark
    // flush, the pipeline, the patch radios, the plugins)
    {"run", "shutdownQuiesce();"},
    {"run", "shutdownStop();"},
    {"run", "shutdownRelease();"},
    // --- engine stage 3a: window lines that still drive the engine directly ---
    // the census seam: a stand-in bias tee for a bounded run
    {"run", "biasStandIn_ = cascade::gui::biasStandInFor("},
    // the GPS test seam starts a read
    // the catalogue test hook points the store at its catalogue
    {"run", "pluginCatalogueUrl_ = pluginTestHook_;"},
    // the teardown marks the clean exit
    {"run", "telemetryCleanExit_ = true;"},
    // the crash handler's context, once a second and at start
    // the engine's frame begins: the snapshot's retry, then the first drain
    {"drawUi", "pumpFrameBegin();"},
    // endTakesOnFault, then pumpDecoderOutput
    {"drawUi", "pumpInputs();"},
    // applyPluginApi, then scannerFrame
    {"drawUi", "pumpPlugins();"},
    // the patch runtime's step of the pass (window port group 13): the catalogue, the transport, the radios, the plan and
    // the sets - or the close rule - which drawPatchPage and drawPatchView used to run from the frame
    {"drawUi", "pumpPatch();"},
    // the bookmark save and the one publish; the window copies the spectrum frame beside it
    // the front end's facts for that publish, handed over first (OPEN 4)
    {"drawUi", "handFrontEndFacts();"},
    {"drawUi", "publishWebSpectrum(pumpPublish());"},
    // where the receiver is decides the mute, before anything draws
    {"drawUi", "pumpAudioMute();"},
    // the page's key submitted on the window's session, for the engine's next pass (stage 4)
    {"drawUi", "pumpTransmitKey();"},
    // the device, sound card, recovery, retune and plugin workers collected
    {"drawUi", "pumpWorkers();"},
    // the audio and microphone gates, the sink health, the heartbeat
    {"drawUi", "pumpAudio();"},
};

// --- rule 5: ImGui input ---------------------------------------------------------------
const char* const kImGuiInput[] = {
    "Button(", "SmallButton(", "InvisibleButton(", "ArrowButton(", "Checkbox(", "RadioButton(",
    "SliderFloat(", "SliderInt(", "SliderDouble(", "DragFloat(", "DragInt(", "InputText(",
    "InputTextWithHint(", "InputDouble(", "InputFloat(", "InputInt(", "Combo(", "BeginCombo(",
    "Selectable(", "MenuItem(", "IsItemClicked(", "IsItemActive(", "IsKeyPressed(", "IsKeyDown(",
    "IsMouseClicked(", "IsMouseDown(", "IsMouseDoubleClicked(",
};

template <std::size_t N>
bool inList(const std::string& s, const char* const (&list)[N]) {
    for (const char* e : list) {
        if (s == e) { return true; }
    }
    return false;
}

bool inList(const std::string& s, const std::vector<const char*>& list) {
    for (const char* e : list) {
        if (s == e) { return true; }
    }
    return false;
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The code of each line: comments and string literals blanked, CR dropped.
// keepEngine: leave "engine_." in place (OPEN 3's read count needs to know a
// name was reached THROUGH the engine; every other rule wants it stripped).
std::vector<std::string> codeLines(const std::string& text, bool keepEngine = false) {
    std::vector<std::string> out;
    bool inBlock = false;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        std::string code;
        bool inString = false;
        bool inChar = false;
        for (std::size_t i = 0; i < line.size(); ++i) {
            const char c = line[i];
            const char n = (i + 1 < line.size()) ? line[i + 1] : '\0';
            if (inBlock) {
                if (c == '*' && n == '/') {
                    inBlock = false;
                    ++i;
                }
                continue;
            }
            if (inString || inChar) {
                if (c == '\\') {
                    ++i;
                    continue;
                }
                if ((inString && c == '"') || (inChar && c == '\'')) {
                    inString = inChar = false;
                    code += c;
                }
                continue;
            }
            if (c == '/' && n == '/') { break; }
            if (c == '/' && n == '*') {
                inBlock = true;
                ++i;
                continue;
            }
            if (c == '"') { inString = true; }
            if (c == '\'') { inChar = true; }
            code += c;
        }
        // ENGINE STAGE 3a: the receiver's state moved into the Engine the
        // window holds (engine_), so what was `pipeline_.stop()` is now
        // `engine_.pipeline_.stop()`. The rules below read "engine_." as if it
        // were not there, so a control reaching the engine's state through
        // the window's reference is judged exactly as it was before the move.
        for (std::size_t at = keepEngine ? std::string::npos : code.find("engine_."); at != std::string::npos;
             at = code.find("engine_.", at)) {
            const unsigned char p = at > 0 ? static_cast<unsigned char>(code[at - 1]) : ' ';
            if (at > 0 && (std::isalnum(p) != 0 || p == '_' || p == '.')) {
                at += 8;
                continue;
            }
            code.erase(at, 8);
        }
        out.push_back(code);
    }
    return out;
}

bool isIdent(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

// A column-0 function head: "AppWindow::x", "free:x", "other:Class::x", or ""
// when the line starts no definition.
std::string definitionAt(const std::string& l) {
    if (l.empty() || l[0] == ' ' || l[0] == '\t' || l[0] == '}' || l[0] == '{' || l[0] == '#') {
        return {};
    }
    const std::size_t p = l.find('(');
    if (p == std::string::npos) { return {}; }
    const std::string head = l.substr(0, p);
    if (head.find('=') != std::string::npos || head.find(';') != std::string::npos) { return {}; }
    std::size_t w = 0;
    while (w < l.size() && isIdent(l[w])) { ++w; }
    static const std::set<std::string> kKeywords = {
        "namespace", "struct", "class", "using", "typedef", "template", "static_assert", "return",
        "if", "for", "while", "switch", "case", "default", "else", "enum", "union", "extern"};
    if (kKeywords.count(l.substr(0, w)) != 0) { return {}; }
    std::size_t e = head.size();
    while (e > 0 && head[e - 1] == ' ') { --e; }
    std::size_t b = e;
    while (b > 0 && (isIdent(head[b - 1]) || head[b - 1] == ':' || head[b - 1] == '~')) { --b; }
    const std::string q = head.substr(b, e - b);
    if (q.empty()) { return {}; }
    const std::size_t aw = q.find("AppWindow::");
    if (aw != std::string::npos) { return "AppWindow::" + q.substr(q.rfind("::") + 2); }
    if (q.find("::") != std::string::npos) { return "other:" + q; }
    return "free:" + q;
}

// Occurrences of `name` as a whole identifier that is not a member of some
// OTHER object (x.name, x->name) - `this->name` counts.
std::vector<std::size_t> identAt(const std::string& l, const std::string& name) {
    std::vector<std::size_t> out;
    for (std::size_t at = l.find(name); at != std::string::npos; at = l.find(name, at + 1)) {
        if (at > 0 && isIdent(l[at - 1])) { continue; }
        const std::size_t end = at + name.size();
        if (end < l.size() && isIdent(l[end])) { continue; }
        if (at > 0 && l[at - 1] == '.') { continue; }
        if (at > 1 && l[at - 1] == '>' && l[at - 2] == '-') {
            if (!(at >= 6 && l.compare(at - 6, 6, "this->") == 0)) { continue; }
        }
        out.push_back(at);
    }
    return out;
}

std::size_t skipSpace(const std::string& l, std::size_t i) {
    while (i < l.size() && (l[i] == ' ' || l[i] == '\t')) { ++i; }
    return i;
}

// Reads ".x" or "->x" at i; returns the identifier and moves i past it.
bool memberAccess(const std::string& l, std::size_t& i, std::string& ident) {
    i = skipSpace(l, i);
    if (i < l.size() && l[i] == '.') {
        ++i;
    } else if (i + 1 < l.size() && l[i] == '-' && l[i + 1] == '>') {
        i += 2;
    } else {
        return false;
    }
    i = skipSpace(l, i);
    const std::size_t b = i;
    while (i < l.size() && isIdent(l[i])) { ++i; }
    ident = l.substr(b, i - b);
    return !ident.empty();
}

struct FieldPatterns {
    std::string name;
    std::vector<std::regex> writes;
};

std::vector<FieldPatterns> fieldPatterns(const std::vector<std::string>& names) {
    std::vector<FieldPatterns> out;
    const std::string pre = "(^|[^A-Za-z0-9_.>]|this->)";
    const std::string assignOp = "\\s*([-+*/%&|^]|<<|>>)?=(?!=)";
    for (const std::string& f : names) {
        FieldPatterns p;
        p.name = f;
        p.writes.emplace_back(pre + f + assignOp);
        p.writes.emplace_back(std::string("(\\+\\+|--)\\s*(this->)?") + f + "(?![A-Za-z0-9_])");
        p.writes.emplace_back(pre + f + "\\s*(\\+\\+|--)");
        p.writes.emplace_back(std::string("(^|[^&])&\\s*(this->)?") + f + "(?![A-Za-z0-9_])");
        p.writes.emplace_back(pre + f + "\\s*\\[[^\\]]*\\]" + assignOp);
        p.writes.emplace_back(pre + f +
                              "\\s*(\\.|->)\\s*(assign|append|clear|push_back|emplace_back|emplace|"
                              "erase|insert|swap|resize|pop_back|replace|reset)\\s*\\(");
        // The patch graph's own mutators (core::patch::Graph): patchGraph_
        // left kWindowMayWriteUnscoped with OPEN 6, and none of these is a
        // container call the line above would see.
        p.writes.emplace_back(pre + f +
                              "\\s*(\\.|->)\\s*(addNode|addNodeAs|removeNode|mutableNode|connect|"
                              "disconnect|reserveIds)\\s*\\(");
        out.push_back(std::move(p));
    }
    return out;
}

// SUB-FIELD assignment through a struct-typed field (soundCard_.cardRateHz =
// ..., one or more member accesses deep) - used ONLY for kWindowMayWriteScoped
// (rule 3's general check has never needed this, because every kFields entry
// is a scalar; a struct-typed field newly added to kFields would want it
// too, but none is today). Found needing this: a probe writing
// soundCard_.cardRateHz from drawToolbar passed the plain fieldPatterns()
// check above silently (it only matches a write to the WHOLE field or a
// mutating container call on it, neither of which a sub-field assignment is).
std::vector<FieldPatterns> scopedFieldPatterns(const std::vector<std::string>& names) {
    std::vector<FieldPatterns> out = fieldPatterns(names);
    const std::string pre = "(^|[^A-Za-z0-9_.>]|this->)";
    const std::string assignOp = "\\s*([-+*/%&|^]|<<|>>)?=(?!=)";
    for (FieldPatterns& p : out) {
        p.writes.emplace_back(pre + p.name + "(\\.[A-Za-z_][A-Za-z0-9_]*)+" + assignOp);
        p.writes.emplace_back(pre + p.name +
                              "(\\.[A-Za-z_][A-Za-z0-9_]*)*\\s*(\\.|->)\\s*(assign|append|clear|push_back|"
                              "emplace_back|emplace|erase|insert|swap|resize|pop_back|replace|reset)\\s*\\(");
    }
    return out;
}

// WHOLE-OBJECT FIELDS (review of the OPEN 6 graph round, finding 6). Rule 3's
// patterns see a write THROUGH the field's own name - an assignment, a
// mutating call on it, its address taken. A struct-typed field can also be
// written WITHOUT its name at the write: bound to a non-const reference
// (`Graph& g = engine_.patchGraph_;` then `g.removeNode(1);`) or handed to a
// function that takes it by non-const reference (`seedDefaultPatch`,
// `drawPatchCanvas` - the canvas's old write path - or `std::swap`). So for
// these fields a control may bind them only to a CONST reference, and pass
// them only to the reviewed helpers below, each of which takes
// `const core::patch::Graph&` (checked when listed: core/patch_draft.hpp,
// patch_io.hpp, patch_plan.hpp). A copy (`patchDraft_ = engine_.patchGraph_;`)
// is a read, and fine.
const char* const kWholeObjectFields[] = {"patchGraph_"};
const char* const kConstGraphHelpers[] = {
    "graphCommandText", "graphsEqual", "rebaseDraft", "serialise", "compile", "mapSources", "channelFeeding", "radioOf",
};

// The findings for one control line `l`, whose two lines before are `before`
// (a call's argument list may start on an earlier line).
void judgeWholeObjectUses(const std::string& before, const std::string& l, std::vector<std::string>& found) {
    const std::string c = before + "\n" + l;
    const std::size_t lineStart = before.size() + 1;
    for (const char* f : kWholeObjectFields) {
        const std::string name = f;
        for (std::size_t at = c.find(name, lineStart); at != std::string::npos; at = c.find(name, at + 1)) {
            if (at > 0 && isIdent(c[at - 1])) { continue; }
            if (at + name.size() < c.size() && isIdent(c[at + name.size()])) { continue; }
            // Step back over the object it is reached through: engine_. / this->
            std::size_t head = at;
            for (const char* via : {"engine_.", "this->", "engine_->"}) {
                const std::size_t n = std::strlen(via);
                if (head >= n && c.compare(head - n, n, via) == 0) { head -= n; }
            }
            if (head > 0 && isIdent(c[head - 1])) { continue; }   // x.patchGraph_: another object's
            std::size_t after = skipSpace(c, at + name.size());
            if (after < c.size() && (c[after] == '.' || c[after] == '[' || c[after] == '-')) { continue; }
            if (after < c.size() && c[after] == '=' && (after + 1 >= c.size() || c[after + 1] != '=')) {
                continue;   // an assignment TO it: rule 3's own pattern
            }
            std::size_t b = head;
            while (b > 0 && (c[b - 1] == ' ' || c[b - 1] == '\t' || c[b - 1] == '\n')) { --b; }
            const char prev = b > 0 ? c[b - 1] : '\0';
            if (prev == '&' && !(b > 1 && c[b - 2] == '&')) { continue; }   // its address: rule 3's own
            if (prev == '=' && !(b > 1 && std::strchr("=!<>", c[b - 2]) != nullptr)) {
                // Initialising or assigning something FROM it. A reference
                // declared here must be const; a copy is a read.
                std::size_t s0 = b - 1;
                while (s0 > 0 && std::strchr(";{}", c[s0 - 1]) == nullptr) { --s0; }
                const std::string decl = c.substr(s0, (b - 1) - s0);
                if (decl.find('&') != std::string::npos && decl.find("const") == std::string::npos) {
                    found.push_back("binds " + name + " to a non-const reference");
                }
                continue;
            }
            if (prev == '(' || prev == ',') {
                // An argument: whose? Back to the '(' that opens this list.
                int depth = 0;
                std::size_t k = b - 1;
                bool opened = false;
                while (true) {
                    const char ch = c[k];
                    if (ch == ')' || ch == ']' || ch == '}') { ++depth; }
                    if (ch == '(' || ch == '[' || ch == '{') {
                        if (depth == 0) {
                            opened = ch == '(';
                            break;
                        }
                        --depth;
                    }
                    if (k == 0) { break; }
                    --k;
                }
                std::string callee;
                if (opened) {
                    std::size_t e = k;
                    while (e > 0 && (c[e - 1] == ' ' || c[e - 1] == '\t' || c[e - 1] == '\n')) { --e; }
                    std::size_t s1 = e;
                    while (s1 > 0 && isIdent(c[s1 - 1])) { --s1; }
                    callee = c.substr(s1, e - s1);
                }
                if (callee.empty() || !inList(callee, kConstGraphHelpers)) {
                    found.push_back("passes " + name + " to " + (callee.empty() ? std::string("?") : callee) +
                                    ", not a reviewed const helper (kConstGraphHelpers)");
                }
                continue;
            }
        }
    }
}

// Rules 1-4 on one line of a control. Appends "rule: token" for each finding.
void judgeControlLine(const std::string& l, const std::vector<std::string>& helpers,
                      const std::vector<FieldPatterns>& fields, bool query,
                      std::vector<std::string>& found, std::set<std::string>* readOnlyUsed = nullptr) {
    // 1. helpers and engine members, called (never as x.name / x->name)
    for (const std::string& name : helpers) {
        const bool call = name.back() == '(';
        std::string ident = call ? name.substr(0, name.size() - 1) : name;
        const std::size_t colon = ident.rfind("->");
        if (colon != std::string::npos) {
            // "spectrum_->setRange": an object's member, matched literally.
            if (l.find(name) != std::string::npos) { found.push_back("helper " + name); }
            continue;
        }
        for (std::size_t at = l.find(ident); at != std::string::npos; at = l.find(ident, at + 1)) {
            if (at > 0 && (isIdent(l[at - 1]) || l[at - 1] == '.')) { continue; }
            if (at > 1 && l[at - 1] == '>' && l[at - 2] == '-') { continue; }
            if (call && l[skipSpace(l, at + ident.size())] != '(') { continue; }
            if (!call && at + ident.size() < l.size() && !isIdent(l[at + ident.size()]) &&
                l[skipSpace(l, at + ident.size())] != '(') {
                continue;
            }
            found.push_back("helper " + name);
            break;
        }
    }
    // 2 and 4. engine objects
    for (const char* o : kEngineObjects) {
        const std::string obj(o);
        for (const std::size_t at : identAt(l, obj)) {
            std::size_t i = at + obj.size();
            std::string m1;
            if (memberAccess(l, i, m1)) {
                std::string chain = obj + "." + m1;
                std::size_t j = skipSpace(l, i);
                bool bare = false;
                if (j + 1 < l.size() && l[j] == '(' && l[skipSpace(l, j + 1)] == ')') {
                    std::size_t k = skipSpace(l, j + 1) + 1;
                    std::string m2;
                    if (memberAccess(l, k, m2)) {
                        chain += "()." + m2;
                    } else {
                        bare = true;
                    }
                }
                bool ok = inList(chain, kReadOnly);
                if (ok && readOnlyUsed != nullptr) { readOnlyUsed->insert(chain); }
                if (!ok && bare && inList(chain, kBareAccessors)) {
                    const std::size_t s = skipSpace(l, 0);
                    ok = l.compare(s, 6, "const ") == 0;
                }
                if (!ok) { found.push_back("engine call " + chain); }
                continue;
            }
            // Passed as an argument: [(,] [*&]? (this->)? obj [,)]
            std::size_t b = at;
            if (b >= 6 && l.compare(b - 6, 6, "this->") == 0) { b -= 6; }
            while (b > 0 && (l[b - 1] == ' ' || l[b - 1] == '\t')) { --b; }
            if (b > 0 && (l[b - 1] == '&' || l[b - 1] == '*')) { --b; }
            while (b > 0 && (l[b - 1] == ' ' || l[b - 1] == '\t')) { --b; }
            const std::size_t a = skipSpace(l, at + obj.size());
            if (!query && b > 0 && (l[b - 1] == '(' || l[b - 1] == ',') && a < l.size() &&
                (l[a] == ',' || l[a] == ')')) {
                found.push_back("engine object passed " + obj);
            }
        }
    }
    // 3. field writes
    for (const FieldPatterns& f : fields) {
        if (l.find(f.name) == std::string::npos) { continue; }
        for (const std::regex& re : f.writes) {
            if (std::regex_search(l, re)) {
                found.push_back("field write " + f.name);
                break;
            }
        }
    }
}

// --- THE ENGINE'S SURFACE, read from src/engine ------------------------------------------
struct EngineSurface {
    std::set<std::string> methods;       // every Engine::x defined, and every method the header declares
    std::set<std::string> constMethods;  // the ones the header declares `const` - queries
    std::set<std::string> fields;        // every data member the header declares
    int imguiUses = 0;                   // "ImGui::" anywhere in src/engine (must be none)
};

std::string trimmed(const std::string& s) {
    std::size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) { ++b; }
    std::size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) { --e; }
    return s.substr(b, e - b);
}

// The statements of `class Engine`'s body, one string each (a nested struct
// or an inline body is part of the statement that opens it).
std::vector<std::string> engineClassStatements(const std::vector<std::string>& lines) {
    std::vector<std::string> out;
    bool in = false;
    int depth = 0;
    std::string cur;
    for (const std::string& l : lines) {
        if (!in) {
            if (l.rfind("class Engine {", 0) == 0) {
                in = true;
                depth = 1;
            }
            continue;
        }
        const int before = depth;
        for (const char c : l) {
            if (c == '{') { ++depth; }
            if (c == '}') { --depth; }
        }
        if (depth <= 0) { break; }
        const std::string t = trimmed(l);
        if (before == 1 && cur.empty() && (t.empty() || t.back() == ':')) { continue; }
        cur += " " + t;
        if (depth == 1 && !t.empty() && (t.back() == ';' || t.back() == '}')) {
            out.push_back(cur);
            cur.clear();
        }
    }
    return out;
}

EngineSurface readEngine(const fs::path& root) {
    EngineSurface s;
    const fs::path dir = root / "src" / "engine";
    std::error_code ec;
    CHECK(fs::is_directory(dir, ec));
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        const std::string ext = e.path().extension().string();
        if (ext != ".cpp" && ext != ".hpp") { continue; }
        const std::vector<std::string> lines = codeLines(readFile(e.path()));
        for (const std::string& l : lines) {
            if (l.find("ImGui::") != std::string::npos) { ++s.imguiUses; }
            const std::string d = definitionAt(l);
            if (d.rfind("other:Engine::", 0) == 0) { s.methods.insert(d.substr(std::strlen("other:Engine::"))); }
        }
        if (e.path().filename() != "engine.hpp") { continue; }
        static const std::regex fieldRx("([A-Za-z][A-Za-z0-9]*_)\\s*(=|\\{|;|\\[)");
        for (const std::string& st0 : engineClassStatements(lines)) {
            const std::string st = trimmed(st0);
            if (st.rfind("friend ", 0) == 0 || st.rfind("using ", 0) == 0 || st.rfind("struct ", 0) == 0 ||
                st.rfind("enum ", 0) == 0 || st.rfind("class ", 0) == 0) {
                continue;
            }
            const std::size_t paren = st.find('(');
            const std::size_t eq = st.find('=');
            if (paren != std::string::npos && (eq == std::string::npos || paren < eq)) {
                std::size_t e2 = paren;
                while (e2 > 0 && st[e2 - 1] == ' ') { --e2; }
                std::size_t b2 = e2;
                while (b2 > 0 && (isIdent(st[b2 - 1]) || st[b2 - 1] == '~')) { --b2; }
                const std::string name = st.substr(b2, e2 - b2);
                if (name.empty() || name == "operator") { continue; }
                s.methods.insert(name);
                int depth = 0;
                std::size_t close = std::string::npos;
                for (std::size_t i = paren; i < st.size(); ++i) {
                    if (st[i] == '(') { ++depth; }
                    if (st[i] == ')' && --depth == 0) {
                        close = i;
                        break;
                    }
                }
                if (close != std::string::npos) {
                    const std::size_t stop = st.find_first_of(";{=", close);
                    const std::string tail = st.substr(close, stop == std::string::npos ? std::string::npos : stop - close);
                    if (tail.find("const") != std::string::npos) { s.constMethods.insert(name); }
                }
                continue;
            }
            std::smatch m;
            std::string rest = st;
            while (std::regex_search(rest, m, fieldRx)) {
                s.fields.insert(m[1].str());
                rest = m.suffix().str();
            }
        }
    }
    return s;
}

// PUBLISHED-ONLY FIELDS (engine/stage3b-pre, docs/engine-stage3.md OPEN 3):
// the engine hands these to the window as a copy (Engine::statusText, once a
// frame), and the window may not read them any other way - not from a
// control, not from machinery. A read of the live field is the cross-thread
// read OPEN 3 exists to remove.
const char* const kPublishedOnly[] = {
    "sourceError_", "gpsRefusal_", "catalogError_", "bandPlanError_", "tuneMismatchNote_",
    "transmitError_", "soundCardMissing_", "sdrPlayApiDetail_", "sdrPlayAdvice_", "recordNotice_",
    "recordError_", "presetNote_", "pluginEnforceError_", "restoreKeepLabel_",
    // The Sinks section's watchdog note (API 0.3 8.4: the STATUS_LINES key
    // audioHealth, and the fifteenth line of EngineStatusText).
    "audioHealthNote_",
    // The Bookmarks section's note and red line (API 0.3 8.6: the STATUS_LINES keys bookmarkNote
    // and bookmarkError, the sixteenth and seventeenth lines of EngineStatusText).
    "bookmarkImportNote_", "bookmarkError_",
    // The plugin store's lines (API 0.3 8.8 and 8.9: STATUS_LINES installReport, installError, catalogStatus and
    // addAllSummary, with addAllFailed its level): the report and the error of an install or a removal, the
    // catalogue's "N plugins" line, and what the last ADD ALL run did - all of them EngineStatusText's.
    "installReport_", "installError_", "catalogStatus_", "addAllSummary_", "addAllFailed_",
};

// THE RATCHET ON EVERY OTHER DIRECT READ (OPEN 3), AND ITS END STATE (window port group 14, part e - the last window group).
// The window reads engine fields directly as a friend - safe only while both run on one thread. Counted over every AppWindow
// member as `engine_.<field>` for any field the Engine declares; each round that moved a group behind a handed copy lowered
// these, and nothing may raise them. THE END OF THE ROAD IS HERE: every window member that still names the engine is either
// PORTED (kPortedMembers below: not at all - a direct `engine_.` in one is a violation) or belongs to exactly ONE of the three
// classes that follow, a read in any OTHER member fails the guard ("unclassified"), and each class has a read budget of its own
// beside the total. These three are the residue the fence step (plan step 5: the interface becomes its own target and sees only
// foxsdr_api.h and app_commands.hpp) inherits, and each says what the fence does with it:
//
//   1. SHELL SEAMS (kShellMembers: run, applyConfig, the constructor). The host process's composition root: it creates the
//      Engine, opens the session, runs the frame loop and the teardown, restores the config and owns the bounded-run seams
//      (the FOXSDR_/CASCADE_ environment hooks that exist for tests and captures: a stand-in bias tee, the device and preset
//      seams, FOXSDR_PATCH_START, the catalogue test hook, the measurement hook, the scripted basemap) beside the start-up and
//      teardown steps no interface has (the usage heartbeat armed for an interactive run, the clean-exit marker, the native
//      device scan the device seam reads, the catalogue URL the store's box is seeded from, the config announce a test reads).
//      THE FENCE KEEPS THEM IN THE HOST TARGET: the class AppWindow splits into the shell, which sees the engine, and the
//      interface, which draws from the table; the env-driven seams are a candidate for a translation unit of their own, because
//      they exist for tests and captures only. The interface must never call shutdownQuiesce/Stop/Release, installSource or
//      scanNative.
//   2. WEB / CAT (kStage5Members: publishWebAudio, publishWebSpectrum, webStatusNow, catStatusNow). The browser's and the CAT
//      server's feeds: the audio tap and the spectrum's rate are read from the GUI thread, and the two status providers run on
//      the SERVERS' threads (they read the engine's published receiver snapshot, which is built for it). STAGE 5 makes the web
//      server and the CAT server sessions of the engine's API like any other front end; after it they are gone from the window,
//      not moved. If the fence lands first they stay a named list. The pump move depends on stage 5, not the other way round
//      (docs/engine-stage4.md, OPEN FOR THE PUMP MOVE: F3).
//   3. HOST ACCESSORS (kHostAccessors: hostTracks, hostPaths, hostPanels, hostInstruments, hostAltitudeNear,
//      hostTrackPluginNames, hostPollPlugins, hostPollImages). The plugin windows, the radar scope and the Patch page's Map part
//      draw what the plugins' own code hands the HOST PROCESS each frame (the plugin ABI polls a track source from the GUI
//      thread at frame rate; the plugin API is deliberately not part of the engine API: 0.3 section 10). Eight narrow
//      accessors, one counted read each, none of them ported by design, so a ninth cannot appear unseen. THE FENCE TURNS THEM
//      INTO THE METHODS OF ONE HostPluginFeed INTERFACE the host target implements over PluginUi and PluginRunner and installs
//      into the window (the data the views draw already lives in core/plugin_ui_data.hpp, which the interface may include); the
//      thread ownership of PluginUi - rebuilt by the pass while the GUI thread polls and draws it - is fixed behind that
//      interface, and it is the one class the fence must DESIGN rather than rename.
//
// What is NOT in the ratchet, and the fence sees: calls (the engine's methods, not its fields), the EngineHost overrides
// (kHostHooks, below: the engine calls them from its thread), direct non-engine reaches in draw members (files, the BMP save,
// std::filesystem) and the engine helper headers the window includes. docs/engine-stage4.md, group 14, has the ledger.
//
// Fixed by name: a member listed in a residue class must exist (a stale name cannot quietly allow a future member), and none may
// also be PORTED.
const char* const kShellMembers[] = {"run", "applyConfig", "AppWindow"};
const char* const kStage5Members[] = {"publishWebAudio", "publishWebSpectrum", "webStatusNow", "catStatusNow"};
const char* const kHostAccessors[] = {"hostTracks",      "hostPaths",      "hostPanels",       "hostInstruments",
                                      "hostAltitudeNear", "hostTrackPluginNames", "hostPollPlugins", "hostPollImages"};

// THE WINDOW'S PORT (engine-stage4.md plan step 4), group by group: a member
// listed here draws from its engine API session ONLY - its state read, its
// lists and facts, its submits - and may not name `engine_.` at all, field
// or call. The list only grows; step 5's fence makes it the whole window.
const char* const kPortedMembers[] = {
    // the Transmit page and its rail key
    "drawTransmitPage", "drawTransmitSection", "setTransmitPageOpen",
    // the session's own helpers
    "uiSubmit", "uiFact", "refreshUiState", "pumpTransmitKey",
    // GROUP 2, the STATUS column and the list reads it keeps
    "drawStatusColumn", "refreshStatusLists",
    // GROUP 3, the panel notes: the seven pages whose only engine reach was
    // telemetryNotePanel, and the once-per-name helper they now call.
    // (The same call at 14 more sites in members not ported yet went through
    // it too, so no window member calls the engine for it any more.)
    "drawRadarSection", "drawCatSection", "drawWebSection", "drawUpdatesSection",
    "drawKeyBindingsSection", "drawDiagnosticsSection", "drawDiagnosticsOffer",
    "uiNotePanel",
    // GROUP 4, the radio-specific panels: the Airspy controls, the deck's bias
    // tee key (its press, its answer, its confirmation dialog), the Converter
    // rows and the sentences they letter - and the three readers that keep
    // what those read of the lists when listSeq moves. The one `engine_.` that
    // stays is run()'s (kLineAllowed: the census stand-in, set once at start).
    "drawAirspyControls", "airspyLists",
    "biasKeyPanel", "biasKeyPressed", "biasKeyAnswered", "drawBiasKeyConfirm",
    "converterAliasNote", "converterStatusLine", "converterModesOffered", "converterUnusableNote",
    "drawConverterControls", "converterLists",
    // GROUP 5, the receiver rail and the display: the footer of the menu column,
    // the Radio section with its WFM stereo and RDS controls, the audio filters,
    // the Sinks section, the Display section, the Language section's country
    // picker (its band plan) and the band plan overlay on the spectrum - and the
    // two readers they share: the rail's cache of the lists and facts, and the
    // fact reader that fills its strings.
    "drawMenuColumn", "drawRadioSection", "drawStereoRdsControls", "drawAudioFilterSection",
    "drawSinksSection", "drawDisplaySection", "drawLanguageSection", "drawBandPlanOverlay",
    "railLists", "readFactText",
    // GROUP 6, the tuning surface: the top bar (the transport, the master lamps, the
    // volume dial and its meters), the frequency readout, the keyboard's actions and
    // the centre panels (spectrum, waterfall, VFO drags) with the bookmark markers
    // they draw - and the window's own helpers: the cache of the lists and facts they
    // read, the one read_spectrum a frame, the VFO drag's two steps and the frequency
    // entry's commit. (The two
    // polls of the pipeline's frame in drawUi went with the spectrum's single read;
    // drawUi itself is a line-allowed member and stays so.)
    "drawToolbar", "drawFrequencyReadout", "applyKeyAction", "drawCenterPanels",
    "drawBookmarkMarkers", "tuningLists", "readSpectrum", "dragVfoCentre", "dragVfoEdge",
    "commitTypedFrequency",
    // GROUP 7, the recorder, the bookmarks and the scanner: the three sections, the bookmark view
    // they filter (over the window's one copy of BOOKMARKS), the scanner form's commit and the
    // flush of a pending edit, and the one reader of a recorder's take (shared with the status
    // column's RECORDER card).
    "drawRecorderSection", "drawBookmarksSection", "rebuildBookmarkView", "drawScannerSection",
    "commitScannerDraft", "flushScannerDraft", "readTake",
    // GROUP 8, the decoders, the presets, the marks, the plugin-mute cluster and the tune grants: the Decoders
    // section and the rows it draws (the decoder status rows, the plugin presets and the preset keys), the Decoder
    // output window with the preset bars above its log, the marks on the spectrum, the Receiver control rows, the
    // tune-away dialog, the banner's subject and its key, and the lookup of a plugin's presets - with the
    // window's own helpers: the cache of the plugin lists, the copy of the decoder lines and its Clear, the
    // session's event drain, the dialog's capture, and the one preset bar (a plugin's own window) that calls
    // the lookup. The engine's mutePopupQueued_, muteKeptRunning_, mutePopup_, mutedByKeys_, pluginMarkers_ and
    // decoderLinesTotal_ are no longer read by the window at all.
    "drawDecodersSection", "drawDecoderStatusRows", "drawDecoderWindow", "drawPluginPresets", "drawPresetKeys",
    "drawDecoderPresetBars", "drawPluginTuneControls", "drawPluginMarkers", "drawMutePopup", "muteBannerSubject",
    "drawMuteBanner", "muteStateForDisplayName", "drawPluginPresetBar",
    "pluginLists", "decoderLines", "clearDecoderLines", "pumpUiEvents", "askAboutMutedPlugins",
    // GROUP 9, the fitted-modules window: the whole window (its model, its seven commands) and the cache of the
    // table it draws from. The window's own rectangle, its open flag and its deck stay the window's. The engine's
    // pluginNotices_ is no longer read by the window at all.
    "drawFittedModulesWindow", "fittedLists",
    // GROUP 10, the plugin store: the rail's key with its chip and the catalogue URL field, the plugins key beside
    // it, the retired rows under them, the store window itself (its model, its nine commands) - and the cache of
    // the table they draw from, which also watches it for the two moments the deck's consents must go. The
    // window's own deck, its open flag and the first-open latch stay the window's; the engine's pluginRepo_ and
    // its ten fields of the store's state are no longer read by the window at all (the two bounded-run
    // diagnostics keep catalog_ and pluginBlocked_).
    "drawPluginStoreSection", "drawPluginsSection", "drawBlockedPluginRows", "drawPluginStoreWindow", "storeLists",
    // GROUP 11, the Source section and the sound card panel: the section (its combo, the scan keys and notes, the
    // dongles that cannot be opened, the IQ file, Pluto and sound card panels, the open radio's rate, antenna, gain
    // and option controls), the lamp on its header, the sound card panel and its "Receives" sentence - and the cache
    // of the table they draw from. The window's own edit buffers (the IQ path, the Pluto address and the sound card's
    // centre, with their drafts) and the combo's edge latch stay the window's; the engine's thirty-four fields of
    // the section's state are no longer read by the window at all (sourceIdForRow left the window: the bounded run's
    // seam asks the engine's own).
    "drawSourceSection", "radioNotOpenLit", "drawSoundCardControls", "soundCardReceivesText", "sourceLists",
    // GROUP 12, the two scopes. The RADAR scope's bar, face, gauges, GAIN knob and POWER key (drawScopeMode), the
    // rail's switch with its position block and its census line (drawScopeModeControl): from the state, the GAINS
    // and PLUGINS caches and uiSubmit, the aircraft apart - they are the HOST PROCESS's (the plugin windows' own
    // poll fills them from the frame), reached through the one accessor hostTracks(), which is NOT listed: it names
    // engine_.pluginUi_ and is the radar scope's only reach into the engine. The DEMOD scope's gather
    // (gatherDemodScope: the three taps through read_scope, the rate, the WFM gate and whose sound from the state),
    // the WFM question and the page, the page's rail key and the signal the page is on - the last three had no
    // engine read before and are listed so that none can grow one.
    "drawScopeMode", "drawScopeModeControl", "gatherDemodScope", "pipelineIsWfm", "scopeSignalNow",
    "drawDemodScopePage", "drawDemodScopeSection",
    // GROUP 13, part a, the Patch page's draft flow and the engine's patch runtime (API 0.3 8.12). The runtime left the
    // window's frame for the engine's pass (Engine::pumpPatch: no drawPatchPage, no sixteen kLineAllowed entries); what
    // is ported here is the page's EDIT of the graph: the look at the graph, its version and its epoch in one publish
    // (patchLook), the draft's follow (syncPatchDraft, adoptPatchLook), the commit through the session with the version
    // it was edited from and its answer (commitPatchDraft, onPatchGraphResult, settlePatchSend), a document
    // (loadPatchDocument), the interaction state a new patch drops (patchInteracting, forgetPatchInteraction), the page-open
    // flag (setMainViewPatch, syncPatchPageOpen), the rail's chip (drawPatchSection), the Radio part's two passes
    // (patchAddRadioPart, finishPatchRadioParts) - and the ticketed submit they share (uiSubmitTicket).
    "drawPatchSection", "setMainViewPatch", "syncPatchPageOpen", "uiSubmitTicket", "patchLook", "syncPatchDraft",
    "commitPatchDraft", "loadPatchDocument", "settlePatchSend", "onPatchGraphResult", "adoptPatchLook",
    "forgetPatchInteraction", "patchInteracting", "finishPatchRadioParts", "patchAddRadioPart",
    // GROUP 13, part b, the rest of the Patch page (API 0.3 8.12): the page itself and its faces (drawPatchView,
    // drawPatchFaces), the seed (seedPatchIfNeeded), the transport, the Radio switch and the three inspectors' bodies
    // (drawPatchTransport, drawPatchRadioSwitch, drawPatchRadioInspector, drawPatchSinkInspector, drawPatchSquelch,
    // drawPatchMapInspector), a starter device and a typed centre (patchDefaultDeviceKey, setPatchRadioCentre,
    // drawPatchCentreNote) and the Map part's targets (patchCollectMapTargets) - and the readers they share: the cache
    // of the lists and facts the page draws from (patchLists, patchRadio), the frame's PATCH_LIVE (readPatchLive,
    // patchLiveRow), a radio's spectrum (patchSpectrum) and the routing of decoder lines to the Text outs
    // (routePatchSinkLines). The Map part's tracks and paths are the HOST PROCESS's, reached through hostTracks() and
    // hostPaths(), which are NOT listed: they name engine_.pluginUi_, and are the only reach into the engine the page has
    // left.
    "drawPatchView", "drawPatchFaces", "seedPatchIfNeeded", "drawPatchTransport", "drawPatchRadioSwitch",
    "drawPatchRadioInspector", "drawPatchSinkInspector", "drawPatchSquelch", "drawPatchMapInspector",
    "patchDefaultDeviceKey", "setPatchRadioCentre", "drawPatchCentreNote", "patchCollectMapTargets", "patchLists",
    "patchRadio", "readPatchLive", "patchLiveRow", "patchSpectrum", "routePatchSinkLines",
    // GROUP 13, part c, the decoded pictures (API 0.3 8.12): the picture a face shows is read from the session
    // (takePatchPicture: PATCH_LIVE's picture row names it, read_image copies it) and uploaded on this thread; the
    // onPatchPicture hook, the only write the engine's thread made into the window's picture state, is gone.
    "takePatchPicture",
    // GROUP 14, part a, the receiver's position and the map (API 0.3 8.11.1; no new API): the map pages (the page a
    // plugin's tracks get, its target register, the details of one target, the rail's rows for pages and windows), the
    // receiver-position offers and the typed entry, the satellite map's click, and the window's follow of the position the
    // state carries (followReceiverPosition: it replaced the engine's onReceiverPositionApplied hook) with the one door
    // its own presses send a position by (submitReceiverPosition). The plugin UI's tracks, paths, panels, instruments and
    // altitudes are the HOST PROCESS's, reached through hostTracks(), hostPaths(), hostPanels(), hostInstruments() and
    // hostAltitudeNear(), which are NOT listed: they name engine_.pluginUi_ and the guard counts each as one read.
    "ensureMapPage", "drawReceiverPositionOffers", "drawRxPositionEntry", "drawSatelliteMapBody", "drawMapPageSections",
    "drawPluginWindowRows", "drawTrackList", "findVisibleTrack", "drawTargetDetailsSection", "drawTargetDetailsWindow",
    "followReceiverPosition", "submitReceiverPosition",
    // GROUP 14, part b, the GPS row (API 0.3 8.13: the GPS list, FOXAPP_STATUS_GPS_RESULT) and the machine's serial ports (the
    // SERIAL_PORTS list, the scan the engine makes on its pass): the row itself, the SYSTEM > Serial ports section, the
    // once-a-look read of the entry, the two clears of an edit, and the ports' cache with its ask. The reader (the engine's
    // gpsReader_) is named by no window member at all since part e: run()'s FOXSDR_GPS_PORT seam sends the key's op too.
    "drawGpsPositionControl", "drawSerialPortsSection", "readGpsRow", "clearGpsResult", "serialPorts", "askForSerialPorts",
    "serialPortsWaiting",
    // GROUP 14, part c, the usage switch and the diagnostics bundle (API 0.3 8.13: fact.telemetry.enabled, .installId,
    // fact.diag.launches and .crashes; the bundle takes the engine's rendered context block). crashUploadStart joined in part e:
    // run() only ARMS the sweep, and the first frame's look - the first that can read a fact - starts it (a fact answers nothing
    // before the engine's first publish, which would have started it with an EMPTY install id: "reporting is off").
    "drawUsageReportingSection", "copyDiagnosticsBundle", "crashUploadStart",
    // GROUP 14, part d, the plugin windows (no API: the plugin UI is the HOST PROCESS's, polled from the frame as the plugin ABI
    // says): the windows' whole frame (the store and the fitted window first, then the poll, the coverage feed, the map pages,
    // the details, the picture, panel and instrument windows) and the hook the engine calls on a preset press. Both name the
    // engine nowhere now: the plugin UI comes through hostPollPlugins(), hostTracks(), hostPaths(), hostTrackPluginNames(),
    // hostPanels() and hostInstruments(), the receiver plugins' pictures through hostPollImages(), the recorder's folder from
    // fact.recorder.dir at the click and the receiver's position from the state. THE EIGHT ACCESSORS ARE NOT LISTED (hostTracks,
    // hostPaths, hostPanels, hostInstruments, hostAltitudeNear, hostTrackPluginNames, hostPollPlugins, hostPollImages): they name
    // engine_.pluginUi_ and engine_.pluginRunner_ by design, the guard counts each as one read, and a ninth cannot appear
    // unseen. openPluginWindowsFor stays in kHostHooks too (the engine calls it; it is judged as machinery) - the event that
    // replaces the call is the pump move's work (docs/engine-stage4.md, group 14 part d).
    "drawPluginWindows", "openPluginWindowsFor",
};

// Window port group 13, part a: 56 fields / 269 reads -> 49 / 230 (patchDspSig_, patchGraphEpoch_, patchGraph_, patchRadioFailedAs_,
// patchWasOpen_, patchWasRunning_ and soundCardOpenPending_ lost their last reader). Part b, the rest of the page's reads:
// 49 / 230 -> 33 / 141 (the sixteen fields that lost their last reader: patchCatalogue_, patchPlan_, patchRadios_,
// patchRadioError_, patchDestError_, patchSpectra_, patchRefused_, patchMainKeep_, patchRadioPending_, patchDests_,
// patchSinkLines_, patchDecoderFaces_, patchRecordings_, devices_, deviceArgs_ and soapyScanPending_; the one read that
// came in is hostPaths()'s, beside hostTracks()'s). The survey predicted 34 / 144 from 56 / 269; part a had already moved the
// three reads of patchGraph_ in run and applyConfig the survey counted as staying (the scripted run's printout now serialises
// the draft), which is the 34 -> 33 and the 144 -> 141. Part c, the decoded pictures: 33 / 141 -> 33 / 141, because the window's
// patchPictures_ was never an engine field (the engine's picture hook WROTE it from the engine's side: no read for the ratchet
// to count) - what part c removes is that write, the onPatchPicture entry of kHostHooks above. The engine-side field patchSinkLines_
// (write-only since part b) is retired with its pruner; the engine's new picture fields (patchImages_ and its five companions) are
// read by no window member, and the guard counts none of them.

// Window port group 14, part a, the receiver's position and the map: 33 / 141 -> 33 / 115 (the 29 reads of the eleven members, three of
// them new reads - hostAltitudeNear, hostPanels and hostInstruments, the host-process accessors; no field lost its last
// reader: rxLat_, rxLon_ and rxSet_ are still read by drawPluginWindows and applyConfig, pluginUi_ by the accessors).
// Part b, the GPS row and the serial ports: 33 / 115 -> 33 / 109 (the five reads of drawGpsPositionControl and the one of
// drawSerialPortsSection; no field lost its last reader - gpsReader_ is still read by run()'s FOXSDR_GPS_PORT seam).
// Part c, the usage switch and the diagnostics bundle: 33 / 109 -> 29 / 96 (the five reads of drawUsageReportingSection and
// the eight of copyDiagnosticsBundle; device_, deviceModel_, telemetryLaunches_ and telemetryCrashes_ lost their last reader -
// telemetryEnabled_ and telemetryInstallId_ are still read by crashUploadStart and run(), modeIndex_, sourceKind_, pipeline_ and
// pluginHost_ by other members). The two refreshDiagContext lines of run() left kLineAllowed with the calls (31 -> 29).
// Part d, the plugin windows: 29 / 96 -> 28 / 77 (the seventeen reads of drawPluginWindows and the five of openPluginWindowsFor,
// three of them new as accessor reads - hostTrackPluginNames, hostPollPlugins and hostPollImages, the host-process accessors; the
// survey predicted 28 / 77 and the guard printed it; recordDir_ lost its last reader - rxLat_, rxLon_ and rxSet_ are still read
// by applyConfig and run's seam, pluginUi_ and pluginRunner_ by the accessors).
// Part e, the last of the window groups - the test machinery, the crash sweep and the shell's own reads: 28 / 77 -> 14 / 33 (the
// survey predicted 18 / 39 for the first three moves; two more came out of the shell, so the printed pair is lower). The bounded-run
// diagnostics left the window: reportPluginStatus (33 reads) and reportPluginTestResult (3) are the engine's now
// (src/engine/engine_diagnostics.cpp), which took catalog_, decoderLog_, muteStates_, pluginBlocked_, pluginDir_, pluginInventory_,
// pluginTuneAllowed_, pluginsStopped_ and sourceKind_ out of the window's reach for good. crashUploadStart (2 reads) reads the
// engine's facts from the first look (telemetryEnabled_ lost its last reader). applyConfig's five reads of the position (redundant
// since part a's follow: rxLat_, rxLon_, rxSet_ lost their last reader) and run()'s FOXSDR_GPS_PORT seam (it sends the key's own
// FOXAPI_OP_GPS through the session: gpsReader_ lost its last reader) are deleted. WHAT IS LEFT IS EXACTLY THE THREE CLASSES ABOVE:
// 19 shell reads (run 15, applyConfig 1, the constructor 3), 6 web/CAT reads (stage 5) and 8 host accessor reads - 33 reads on
// 14 fields - and every one of them is classified: a read in any other member fails the guard.
constexpr int kWindowFieldsReadBudget = 14;
constexpr int kWindowFieldReadsBudget = 33;
// THE RESIDUE'S OWN BUDGETS, one per class (the totals above are their sum). A new read in a listed member raises its class, and
// the class budget fails even where another class has slack.
constexpr int kShellReadsBudget = 19;
constexpr int kStage5ReadsBudget = 6;
constexpr int kHostAccessorReadsBudget = 8;

struct Report {
    int violations = 0;
    int fieldReads = 0;
    std::set<std::string> fieldsRead;
    int controls = 0;
    int submitsInDraw = 0;
    int machineryLines = 0;
    int allowedLinesUsed = 0;
    std::set<std::string> membersSeen;
    std::set<std::string> allowedLinesSeen;
    std::set<std::string> readOnlyUsed;  // the kReadOnly entries a control line actually matched
    int residueReads[3] = {0, 0, 0};     // the reads of the three residue classes: shell, stage 5, host accessors
    std::map<std::string, int> unclassifiedReads;  // reads in a member that is none of them (and not ported)
};

// 0 shell seams, 1 web/CAT (stage 5), 2 host accessors, -1 none of them.
int residueClassOf(const std::string& member) {
    if (inList(member, kShellMembers)) { return 0; }
    if (inList(member, kStage5Members)) { return 1; }
    if (inList(member, kHostAccessors)) { return 2; }
    return -1;
}

void scan(const fs::path& root, const EngineSurface& eng, Report& r) {
    const fs::path gui = root / "src" / "gui";
    std::error_code ec;
    CHECK(fs::is_directory(gui, ec));
    // Rule 3's fields: the receiver's, and every field the Engine declares,
    // but those the window still edits in place (each an OPEN item) - the
    // SCOPED ones are judged separately, below, against their one owner.
    std::vector<std::string> fieldNames(std::begin(kFields), std::end(kFields));
    std::vector<std::string> scopedFieldNames;
    for (const ScopedWrite& sw : kWindowMayWriteScoped) { scopedFieldNames.push_back(sw.field); }
    const auto exemptFromRule3 = [&](const std::string& f) {
        return inList(f, kWindowMayWriteUnscoped) ||
               std::find(scopedFieldNames.begin(), scopedFieldNames.end(), f) != scopedFieldNames.end();
    };
    for (const std::string& f : eng.fields) {
        if (exemptFromRule3(f)) { continue; }
        if (std::find(fieldNames.begin(), fieldNames.end(), f) == fieldNames.end()) { fieldNames.push_back(f); }
    }
    const std::vector<FieldPatterns> fields = fieldPatterns(fieldNames);
    const std::vector<FieldPatterns> scopedFields = scopedFieldPatterns(scopedFieldNames);
    // Rule 1's tokens: the curated helpers, the window's own machinery, and
    // every Engine method that is not a const query - but the command path.
    std::vector<std::string> helpers(std::begin(kHelpers), std::end(kHelpers));
    const auto addCall = [&helpers](const std::string& m) {
        if (inList(m, kControlMayCall)) { return; }
        const std::string call = m + "(";
        if (std::find(helpers.begin(), helpers.end(), call) == helpers.end()) { helpers.push_back(call); }
    };
    for (const char* m : kWindowMachinery) { addCall(m); }
    for (const std::string& m : eng.methods) {
        if (eng.constMethods.count(m) == 0) { addCall(m); }
    }
    std::set<std::string> controls;
    for (const auto& e : fs::directory_iterator(gui, ec)) {
        if (e.path().extension() != ".cpp") { continue; }
        const std::string file = e.path().filename().string();
        const std::vector<std::string> lines = codeLines(readFile(e.path()));
        const std::vector<std::string> rawLines = codeLines(readFile(e.path()), true);
        bool definesAppWindow = false;
        for (const std::string& l : lines) {
            if (definitionAt(l).rfind("AppWindow::", 0) == 0) { definesAppWindow = true; }
        }
        std::string def;
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const std::string& l = lines[i];
            const std::string d = definitionAt(l);
            if (!d.empty()) { def = d; }
            std::string member;
            if (def.rfind("AppWindow::", 0) == 0) {
                member = def.substr(std::strlen("AppWindow::"));
            } else if (def.rfind("free:", 0) == 0 && definesAppWindow) {
                member = def.substr(std::strlen("free:"));
            } else {
                continue;  // file scope, another class, or a file with no AppWindow code
            }
            r.membersSeen.insert(member);
            if (inList(member, kPortedMembers)) {
                const std::string& rawP = i < rawLines.size() ? rawLines[i] : l;
                const std::size_t at = rawP.find("engine_.");
                if (at != std::string::npos && (at == 0 || (!isIdent(rawP[at - 1]) && rawP[at - 1] != '.'))) {
                    ++r.violations;
                    std::printf("FAIL: AppWindow::%s is PORTED (draws from its engine API session only) but "
                                "reaches the engine directly at %s:%zu\n",
                                member.c_str(), file.c_str(), i + 1);
                }
            }
            // OPEN 3: published-only fields, and the ratchet's count - over
            // every AppWindow member, machinery included.
            const std::string& raw = i < rawLines.size() ? rawLines[i] : l;
            for (std::size_t at = raw.find("engine_."); at != std::string::npos; at = raw.find("engine_.", at + 1)) {
                if (at > 0 && (isIdent(raw[at - 1]) || raw[at - 1] == '.')) { continue; }
                std::size_t e = at + std::strlen("engine_.");
                const std::size_t b = e;
                while (e < raw.size() && isIdent(raw[e])) { ++e; }
                const std::string field = raw.substr(b, e - b);
                if (eng.fields.count(field) == 0) { continue; }
                ++r.fieldReads;
                r.fieldsRead.insert(field);
                {
                    const int cls = residueClassOf(member);
                    if (cls >= 0) {
                        ++r.residueReads[cls];
                    } else if (!inList(member, kPortedMembers)) {  // (a ported member's read is already a violation, above)
                        ++r.unclassifiedReads[member];
                        ++r.violations;
                        std::printf("FAIL: AppWindow::%s reads engine_.%s at %s:%zu but is in none of the residue classes "
                                    "(kShellMembers, kStage5Members, kHostAccessors) and is not ported\n      -> port it "
                                    "(read the table), or - only for a shell seam, a stage-5 feed or a host accessor - list it\n",
                                    member.c_str(), field.c_str(), file.c_str(), i + 1);
                    }
                }
                if (inList(field, kPublishedOnly)) {
                    ++r.violations;
                    std::printf("FAIL: AppWindow::%s reads engine_.%s at %s:%zu\n      -> read it from the "
                                "copy the engine hands over (Engine::statusText, docs/engine-stage3.md OPEN 3)\n",
                                member.c_str(), field.c_str(), file.c_str(), i + 1);
                }
            }
            if (inList(member, kWindowMachinery) || inList(member, kHostHooks)) {
                // 5. no ImGui input in machinery.
                for (const char* in : kImGuiInput) {
                    if (l.find(std::string("ImGui::") + in) != std::string::npos) {
                        ++r.violations;
                        std::printf("FAIL: machinery AppWindow::%s takes ImGui input (%s) at %s:%zu"
                                    " - a control does not belong here\n",
                                    member.c_str(), in, file.c_str(), i + 1);
                    }
                }
                ++r.machineryLines;
                continue;
            }
            bool allowedLine = false;
            for (const LineAllow& a : kLineAllowed) {
                if (member == a.member && l.find(a.code) != std::string::npos) {
                    allowedLine = true;
                    r.allowedLinesSeen.insert(std::string(a.member) + "|" + a.code);
                }
            }
            if (allowedLine) {
                ++r.allowedLinesUsed;
                continue;
            }
            controls.insert(member);
            if (member.rfind("draw", 0) == 0 && l.find("submitCommand(") != std::string::npos) {
                ++r.submitsInDraw;
            }
            std::vector<std::string> found;
            judgeControlLine(l, helpers, fields, false, found, &r.readOnlyUsed);
            {
                std::string before;
                for (std::size_t j = (i >= 2 ? i - 2 : 0); j < i; ++j) { before += lines[j] + "\n"; }
                judgeWholeObjectUses(before, l, found);
            }
            for (const std::string& f : found) {
                ++r.violations;
                std::printf("FAIL: AppWindow::%s changes the receiver outside the command path (%s) at "
                            "%s:%zu\n      -> submit a command (docs/engine-stage1.md, docs/engine-stage3.md)\n",
                            member.c_str(), f.c_str(), file.c_str(), i + 1);
            }
            // SCOPED FIELDS (engine/stage3b-pre 2d): a write to one of these
            // is fine from its ONE named owner and a violation from anything
            // else - a control not on this list at all is already caught
            // above by judgeControlLine (these fields were excluded from
            // `fields` precisely so they land here instead).
            for (const FieldPatterns& f : scopedFields) {
                if (l.find(f.name) == std::string::npos) { continue; }
                bool written = false;
                for (const std::regex& re : f.writes) {
                    if (std::regex_search(l, re)) {
                        written = true;
                        break;
                    }
                }
                if (!written) { continue; }
                const char* owner = nullptr;
                for (const ScopedWrite& sw : kWindowMayWriteScoped) {
                    if (f.name == sw.field) {
                        owner = sw.member;
                        break;
                    }
                }
                if (owner != nullptr && member != owner) {
                    ++r.violations;
                    std::printf("FAIL: AppWindow::%s writes %s, which only AppWindow::%s may write, at "
                                "%s:%zu\n      -> submit a command, or move the write to its owner "
                                "(docs/engine-stage3.md OPEN item 1)\n",
                                member.c_str(), f.name.c_str(), owner, file.c_str(), i + 1);
                }
            }
        }
    }
    r.controls = static_cast<int>(controls.size());
}

// THE PUMP MOVE'S RULE FOR THE COMMAND PATH (step P1, the review's F3): once the engine's pass runs on a thread of its own, a
// window thread that calls Engine::applyCommand or Engine::applyControlRequest runs the whole command surface - and the key rules,
// which are single-threaded by contract - beside the pass. So the window makes NO such direct call, with the ONE exception that
// is still listed and goes in the move itself: drawUi's dropped-file import. The three that stood here (the web/CAT drain's
// applyControlRequest, its per-frame WEB_CONTROL_STOPPED and applyWebSettings' own) are the engine's pass now
// (Engine::pumpRemoteControls). Every other command goes through uiSubmit, the session's submit or Engine::submitCommand.
constexpr int kDirectApplyCommandCalls = 1;      // drawUi: the file drop (P4/P5 sends it through uiSubmit like the Import key)
constexpr int kDirectApplyControlRequestCalls = 0;

void checkNoDirectApply(const fs::path& root) {
    const fs::path gui = root / "src" / "gui";
    int applyCommandCalls = 0;
    int controlRequestCalls = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(gui, ec)) {
        if (e.path().extension() != ".cpp") { continue; }
        for (const std::string& l : codeLines(readFile(e.path()), true)) {
            for (std::size_t at = l.find("engine_.applyCommand("); at != std::string::npos;
                 at = l.find("engine_.applyCommand(", at + 1)) {
                ++applyCommandCalls;
                std::printf("  direct engine_.applyCommand in %s: %s\n", e.path().filename().string().c_str(), l.c_str());
            }
            for (std::size_t at = l.find("engine_.applyControlRequest("); at != std::string::npos;
                 at = l.find("engine_.applyControlRequest(", at + 1)) {
                ++controlRequestCalls;
                std::printf("  direct engine_.applyControlRequest in %s: %s\n", e.path().filename().string().c_str(), l.c_str());
            }
        }
    }
    std::printf("  the window's direct calls into the command path: applyCommand %d (allowed %d), applyControlRequest %d "
                "(allowed %d)\n",
                applyCommandCalls, kDirectApplyCommandCalls, controlRequestCalls, kDirectApplyControlRequestCalls);
    if (applyCommandCalls > kDirectApplyCommandCalls || controlRequestCalls > kDirectApplyControlRequestCalls) {
        std::printf("FAIL: a window member calls the engine's command path directly - the pump move (F3) made the engine's "
                    "pass the only thing that applies a remote request; send the command through uiSubmit or let the "
                    "engine pull it (Engine::setRemoteControlSource)\n");
    }
    CHECK(applyCommandCalls <= kDirectApplyCommandCalls);
    CHECK(controlRequestCalls <= kDirectApplyControlRequestCalls);
}

// THE HOOKS THE PUMP MOVE'S STEP P2 REMOVED STAY REMOVED. Each was a call the engine made from its pass into the window, writing or
// reading window state (see the note above kHostHooks): the engine's host class must not declare them, no engine source may call them
// through host_, and the window must not override them. (onCatalogueResult stays in the BASE class for a front end that wants to be
// told - the headless tests do - but the standard window no longer overrides it: the engine prints the bounded run's catalogue
// report itself, from the report switch and the frame the window hands over in FrontEndFacts.)
const char* const kRetiredHooks[] = {"onDisplayRange",     "onBookmarksChanged", "onConverterChanged", "openPluginWindowsFor",
                                     "drainTrackInfoText", "enrichWebTrack",     "fillWebImages"};

void checkRetiredHooks(const fs::path& root) {
    int found = 0;
    const auto codeOf = [&](const fs::path& rel) { return codeLines(readFile(root / rel)); };
    const auto flag = [&](const std::string& where, const std::string& line, const char* hook) {
        ++found;
        std::printf("FAIL: retired host hook %s is back in %s: %s\n", hook, where.c_str(), line.c_str());
    };
    // the engine's host class: no declaration, no default
    for (const char* rel : {"src/engine/engine_host.hpp", "src/engine/engine_host.cpp"}) {
        for (const std::string& l : codeOf(rel)) {
            for (const char* h : kRetiredHooks) {
                if (l.find(std::string(h) + "(") != std::string::npos) { flag(rel, l, h); }
            }
        }
    }
    // the engine's sources: no call through the host
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(root / "src" / "engine", ec)) {
        const std::string ext = e.path().extension().string();
        if (ext != ".cpp" && ext != ".hpp") { continue; }
        for (const std::string& l : codeLines(readFile(e.path()))) {
            for (const char* h : kRetiredHooks) {
                if (l.find(std::string("host_->") + h + "(") != std::string::npos) { flag(e.path().filename().string(), l, h); }
            }
        }
    }
    // the window: no override of any of them, and no override of onCatalogueResult
    for (const std::string& l : codeOf("src/gui/app_window.hpp")) {
        if (l.find("override") == std::string::npos) { continue; }
        for (const char* h : kRetiredHooks) {
            if (l.find(std::string(h) + "(") != std::string::npos) { flag("app_window.hpp", l, h); }
        }
        if (l.find("onCatalogueResult(") != std::string::npos) { flag("app_window.hpp", l, "onCatalogueResult"); }
    }
    std::printf("  the retired host hooks (%zu + the window's onCatalogueResult) are still gone: %d reappeared\n",
                sizeof(kRetiredHooks) / sizeof(kRetiredHooks[0]), found);
    CHECK(found == 0);
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const fs::path root = (argc > 1) ? fs::path(argv[1]) : fs::path(CASCADE_SOURCE_DIR);
    std::printf("scanning %s\n", root.string().c_str());
    const EngineSurface eng = readEngine(root);
    std::printf("  the engine: %zu methods (%zu const queries), %zu fields, %d ImGui uses\n",
                eng.methods.size(), eng.constMethods.size(), eng.fields.size(), eng.imguiUses);
    Report r;
    scan(root, eng, r);
    std::printf("  %d controls judged, %d submitCommand calls in draw members, %d window-machinery lines, "
                "%d allowed engine lines in drawing members, %d violations\n",
                r.controls, r.submitsInDraw, r.machineryLines, r.allowedLinesUsed, r.violations);
    // The scan saw the real files.
    CHECK(r.controls >= 60);
    // NO DRAW MEMBER SENDS THROUGH THE LEGACY QUEUE ANY MORE. This was a sanity floor ("the scan saw the real files") that
    // counted down as the groups ported - 30 until group 11 ported the Source section's seventeen, 13 until group 12 ported
    // the radar scope's two, 11 until group 13 part b ported the patch transport's two and the Radio inspector's one, 8
    // until group 14 part a ported the position helpers' six, 2 until part b ported the GPS row's two - and at zero it is a
    // rule: a draw member that sends with engine_.submitCommand again fails here (every command goes through uiSubmit). The
    // scan seeing the real files is the controls check above.
    CHECK(r.submitsInDraw == 0);
    CHECK(eng.methods.size() >= 100);
    CHECK(eng.fields.size() >= 200);
    CHECK(r.violations == 0);
    // Every PORTED member exists (a stale name cannot quietly exempt nothing).
    for (const char* m : kPortedMembers) {
        if (r.membersSeen.count(m) == 0) { std::printf("FAIL: kPortedMembers names %s, which is no member\n", m); }
        CHECK(r.membersSeen.count(m) != 0);
    }
    // OPEN 3's ratchet.
    std::printf("  the window reads %zu engine fields directly, %d times (budget %d fields, %d reads)\n",
                r.fieldsRead.size(), r.fieldReads, kWindowFieldsReadBudget, kWindowFieldReadsBudget);
    if (static_cast<int>(r.fieldsRead.size()) > kWindowFieldsReadBudget || r.fieldReads > kWindowFieldReadsBudget) {
        std::printf("FAIL: more direct reads of the engine than the budget - read the new ones from a copy "
                    "the engine hands over (docs/engine-stage3.md OPEN 3)\n");
    }
    CHECK(static_cast<int>(r.fieldsRead.size()) <= kWindowFieldsReadBudget);
    CHECK(r.fieldReads <= kWindowFieldReadsBudget);
    // ...and the residue, by class: nothing is read outside the three, and each stays inside its own budget.
    {
        std::string names;
        for (const std::string& f : r.fieldsRead) { names += (names.empty() ? "" : " ") + f; }
        std::printf("  the residue: shell seams %d reads (budget %d), web/CAT for stage 5 %d (budget %d), host accessors %d "
                    "(budget %d), unclassified %zu members\n  the fields: %s\n",
                    r.residueReads[0], kShellReadsBudget, r.residueReads[1], kStage5ReadsBudget, r.residueReads[2],
                    kHostAccessorReadsBudget, r.unclassifiedReads.size(), names.c_str());
        CHECK(r.unclassifiedReads.empty());
        CHECK(r.residueReads[0] <= kShellReadsBudget);
        CHECK(r.residueReads[1] <= kStage5ReadsBudget);
        CHECK(r.residueReads[2] <= kHostAccessorReadsBudget);
        // The classes are named members that exist, and none is also ported (a ported member names the engine nowhere).
        const auto namedAndNotPorted = [&](const char* what, const char* const* b, const char* const* e) {
            for (const char* const* m = b; m != e; ++m) {
                if (r.membersSeen.count(*m) == 0) {
                    std::printf("FAIL: %s names %s, which the window does not define - remove it\n", what, *m);
                }
                CHECK(r.membersSeen.count(*m) != 0);
                if (inList(*m, kPortedMembers)) {
                    std::printf("FAIL: %s names %s, which is also in kPortedMembers\n", what, *m);
                }
                CHECK(!inList(*m, kPortedMembers));
            }
        };
        namedAndNotPorted("kShellMembers", std::begin(kShellMembers), std::end(kShellMembers));
        namedAndNotPorted("kStage5Members", std::begin(kStage5Members), std::end(kStage5Members));
        namedAndNotPorted("kHostAccessors", std::begin(kHostAccessors), std::end(kHostAccessors));
    }
    // THE READ-ONLY LIST HOLDS NOTHING STALE (rule 6, for kReadOnly): every entry is matched by at least one call in a control,
    // so an entry whose last caller went - a group ported it, a spelling changed - is found here, whatever the spelling the call
    // used (`.` or `->`, an accessor between: the guard's own parse of the line, not a text search of the tree). The tree
    // before part e held 47 such entries that nothing had noticed.
    for (const char* e : kReadOnly) {
        if (r.readOnlyUsed.count(e) == 0) {
            std::printf("FAIL: kReadOnly entry %s matches no call in a control - retire it (its last caller is gone)\n", e);
        }
        CHECK(r.readOnlyUsed.count(e) != 0);
    }
    // THE DATA HEADER SPLIT (window port group 14, part d; the fence ledger): the views draw plugin data - a track, a path, a
    // panel's rows, an instrument's state, a preset - and need none of the machinery that fills it. core/plugin_ui_data.hpp
    // carries the data; core/plugin_ui.hpp (PluginUi, the host table, the mute policy) includes it and is the HOST PROCESS's.
    // The gui headers the interface draws from include the data header and NEVER plugin_ui.hpp, so the fence can forbid
    // plugin_ui.hpp to the interface target while the views keep their types. (tests/test_plugin_ui_data_split.cpp is the
    // compile proof for the five that do not include the engine; app_window.hpp still reaches plugin_ui.hpp through
    // engine/engine.hpp, so only the text of its include is held here.)
    {
        const char* const kDataOnlyHeaders[] = {"app_window.hpp",   "decoder_reads.hpp",      "instrument_face.hpp",
                                                "map_view.hpp",     "scope_view.hpp",         "track_detail_view.hpp"};
        for (const char* h : kDataOnlyHeaders) {
            const std::string text = readFile(root / "src" / "gui" / h);
            const bool names = text.find("#include \"core/plugin_ui.hpp\"") != std::string::npos;
            const bool uses = text.find("#include \"core/plugin_ui_data.hpp\"") != std::string::npos;
            if (names) { std::printf("FAIL: src/gui/%s includes core/plugin_ui.hpp - the views take core/plugin_ui_data.hpp\n", h); }
            if (!uses) { std::printf("FAIL: src/gui/%s does not include core/plugin_ui_data.hpp\n", h); }
            CHECK(!names);
            CHECK(uses);
        }
        // ...and the data header carries no host machinery: it includes the standard library and the plugin ABI, nothing else.
        const std::string data = readFile(root / "src" / "core" / "plugin_ui_data.hpp");
        CHECK(!data.empty());
        std::size_t at = 0;
        int includes = 0;
        while ((at = data.find("#include", at)) != std::string::npos) {
            const std::size_t eol = data.find('\n', at);
            const std::string line = data.substr(at, eol == std::string::npos ? std::string::npos : eol - at);
            const bool std_ = line.find('<') != std::string::npos;
            const bool abi = line.find("\"core/plugin_abi.h\"") != std::string::npos;
            if (!std_ && !abi) { std::printf("FAIL: core/plugin_ui_data.hpp includes %s - it may take only the ABI\n", line.c_str()); }
            CHECK(std_ || abi);
            ++includes;
            at += 8;
        }
        CHECK(includes >= 5);
    }
    // 5. The engine is built without ImGui (the configure-time check enforces
    // the includes; this is the belt to that brace).
    CHECK(eng.imguiUses == 0);
    // 6. Every allow-list entry names something that exists.
    // ...and every whole-object helper really takes the graph by CONST
    // reference: its first parameter, as declared in src/core.
    {
        std::string core;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(root / "src" / "core", ec)) {
            if (e.path().extension() == ".hpp") { core += readFile(e.path()); }
        }
        for (const char* h : kConstGraphHelpers) {
            const std::regex decl(std::string("\\b") + h + "\\(\\s*const\\s+Graph\\s*&");
            const bool ok = std::regex_search(core, decl);
            if (!ok) {
                std::printf("FAIL: kConstGraphHelpers names %s, which src/core does not declare taking "
                            "const Graph& - remove it\n", h);
            }
            CHECK(ok);
        }
    }
    for (const char* m : kWindowMachinery) {
        if (r.membersSeen.count(m) == 0) {
            std::printf("FAIL: kWindowMachinery names %s, which the window does not define - remove it\n", m);
        }
        CHECK(r.membersSeen.count(m) != 0);
    }
    for (const char* m : kHostHooks) {
        if (r.membersSeen.count(m) == 0) {
            std::printf("FAIL: kHostHooks names %s, which the window does not define - remove it\n", m);
        }
        CHECK(r.membersSeen.count(m) != 0);
    }
    for (const char* m : kControlMayCall) {
        const bool ok = eng.methods.count(m) != 0 || r.membersSeen.count(m) != 0;
        if (!ok) { std::printf("FAIL: kControlMayCall names %s, which nothing defines - remove it\n", m); }
        CHECK(ok);
    }
    for (const ScopedWrite& sw : kWindowMayWriteScoped) {
        if (eng.fields.count(sw.field) == 0) {
            std::printf("FAIL: kWindowMayWriteScoped names %s, which the engine does not declare - remove it\n",
                        sw.field);
        }
        CHECK(eng.fields.count(sw.field) != 0);
        if (r.membersSeen.count(sw.member) == 0) {
            std::printf("FAIL: kWindowMayWriteScoped names %s as %s's owner, but the window does not define "
                        "%s - fix or remove it\n", sw.field, sw.member, sw.member);
        }
        CHECK(r.membersSeen.count(sw.member) != 0);
    }
    for (const char* f : kWindowMayWriteUnscoped) {
        if (eng.fields.count(f) == 0) {
            std::printf("FAIL: kWindowMayWriteUnscoped names %s, which the engine does not declare - remove it\n", f);
        }
        CHECK(eng.fields.count(f) != 0);
    }
    for (const LineAllow& a : kLineAllowed) {
        const std::string key = std::string(a.member) + "|" + a.code;
        if (r.allowedLinesSeen.count(key) == 0) {
            std::printf("FAIL: kLineAllowed entry {%s, %s} matches no line - remove it\n", a.member, a.code);
        }
        CHECK(r.allowedLinesSeen.count(key) != 0);
    }
    checkNoDirectApply(root);
    checkRetiredHooks(root);
    return testSummary("test_command_path_guard");
}
