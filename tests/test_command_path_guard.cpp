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
//     restore, the web/CAT drain - the few window members that still drive
//     the engine directly - and the EngineHost overrides (kHostHooks): the
//     window's side of calls the ENGINE makes. Allowed to reach the engine;
//     rule 5 keeps them free of ImGui input.
//   LINE-ALLOWED MEMBERS (kLineAllowed): members that draw AND carry a few
//     reviewed engine lines (drawUi's per-frame pump steps and transmit key;
//     run's startup, teardown and verification seams; drawPatchPage's patch
//     runtime steps). Only the listed lines are exempt.
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
    "spectrum_->setRange(", "selectSource(", "scanNative(", "scanSoapy(", "openPlutoFromBox(",
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

// Calls a control may make: "object.method", or "object.accessor().method".
// Reads, and the stream reads a panel consumes (a spectrum frame, images, a
// peak) - never a change to the receiver. gpsReader_.clearResult() clears the
// finished read's status line when the form's port changes (view state).
const char* const kReadOnly[] = {
    "audioOpen_.inFlight",
    "audioRecorder_.bytesWritten", "audioRecorder_.recording", "audioRecorder_.samplesWritten",
    "audioRecorder_.sizeLimitReached", "audioRecorder_.writeFailed",
    "iqRecorder_.bytesWritten", "iqRecorder_.recording", "iqRecorder_.samplesWritten",
    "iqRecorder_.sizeLimitReached", "iqRecorder_.writeFailed",
    "device_.deviceDead", "device_.frequencyRangeHz", "device_.name", "device_.sampleRateHz",
    "device_.lastError", "device_.gainDb", "device_.antenna", "device_.autoGain",
    "freqMgr_.list", "freqMgr_.list().size", "freqMgr_.list().empty", "freqMgr_.version",
    "freqMgr_.range", "freqMgr_.nearestSubset", "freqMgr_.indexOfId",
    "gpsReader_.listening", "gpsReader_.status", "gpsReader_.clearResult",
    "micOpen_.inFlight", "micOpen_.result().ok",
    "pipeline_.activeSource().centerFrequencyHz", "pipeline_.activeSource().name",
    "pipeline_.activeSource().sampleRateHz", "pipeline_.activeSource().faulted",
    "pipeline_.activeSourceName", "pipeline_.audio().everOpened", "pipeline_.audio().openedDeviceName",
    "pipeline_.audio().streamAlive", "pipeline_.audio().underruns", "pipeline_.audioMuted",
    "pipeline_.autoNotchEngaged", "pipeline_.autoNotchFrequencyHz", "pipeline_.channelRateHz",
    "pipeline_.converter", "pipeline_.demodMode", "pipeline_.faultMessage", "pipeline_.faulted",
    "pipeline_.getLatestFrame", "pipeline_.inputRateHz", "pipeline_.pilotLevel", "pipeline_.pilotLocked",
    "pipeline_.rdsSnapshot", "pipeline_.running", "pipeline_.signalPowerDb", "pipeline_.stereoActive",
    "pipeline_.vfoOffsetHz", "pipeline_.rawSource().centerFrequencyHz", "pipeline_.audioSamplesProduced",
    "pipeline_.audioTap",
    "pipeline_.scopeAudio().capacity", "pipeline_.scopeAudio().snapshot", "pipeline_.scopeAudio().written",
    "pipeline_.scopeIq().capacity", "pipeline_.scopeIq().snapshot", "pipeline_.scopeIq().written",
    "pipeline_.scopeMpx().capacity", "pipeline_.scopeMpx().snapshot", "pipeline_.scopeMpx().written",
    "pluginHost_.plugins", "pluginHost_.loadedCount",
    "pluginRunner_.activeCount", "pluginRunner_.audioFramesFed", "pluginRunner_.audioGapFrames",
    "pluginRunner_.audioGaps", "pluginRunner_.isFeeding", "pluginRunner_.playingPlugin",
    "pluginRunner_.status", "pluginRunner_.iqFramesFed", "pluginRunner_.pollImages",
    "pluginUi_.api().commands", "pluginUi_.api().settingsRequesters", "pluginUi_.instruments",
    "pluginUi_.panels", "pluginUi_.panels().size", "pluginUi_.paths", "pluginUi_.paths().size",
    "pluginUi_.lastDeniedPlugin", "pluginUi_.trackPluginNames", "pluginUi_.trackPluginNames().empty",
    "pluginUi_.tracks", "pluginUi_.tracks().size", "pluginUi_.tuneAllowed", "pluginUi_.settingsAllowed",
    "pluginUi_.altitudeNear", "pluginUi_.tuneRequesters().size", "pluginUi_.poll",
    "pluginRepo_.progress",
    "scanner_.active", "scanner_.currentHz", "scanner_.state",
    "soapyView_.deadReason", "soapyView_.deviceDead",
    "transmitter_.haveSink", "transmitter_.lastAutoUnkeyReason", "transmitter_.lastError",
    "transmitter_.latched", "transmitter_.remoteHoldRemainingMs", "transmitter_.sink",
    "transmitter_.sink().faulted", "transmitter_.sink().gainDb", "transmitter_.sink().name",
    "transmitter_.takeInputPeak", "transmitter_.toneHz", "transmitter_.transmitting",
    "transmitter_.frequencyHz", "transmitter_.audioIn().running",
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
// teardown, the config restore and the web/CAT drain.
const char* const kWindowMachinery[] = {
    "AppWindow",
    "~AppWindow",
    "applyConfig",
    "currentConfig",
    "applyWebControls",
};

// The EngineHost overrides (engine/engine_host.hpp): the window's half of a
// call the ENGINE makes, on the engine's behalf - it redraws, re-seeds a
// field, pauses the watchdog, answers a question about the view. Judged as
// machinery: rule 5 applies, rules 1-4 do not (the one rule-1 helper they
// use is spectrum_->setRange, in onDisplayRange: the engine's new range
// applied to this window's spectrum).
const char* const kHostHooks[] = {
    "frameClockRunning",  "frameTimeS",          "wallTimeS",
    "pauseWatchdog",      "resumeWatchdog",      "onDisplayRange",
    "onBookmarksChanged", "openPluginWindowsFor", "onReceiverPositionApplied",
    "onConverterChanged", "onPatchGraphChanged", "onPatchPicture",
    "onGpsFixApplied",    "onCatalogueFetchStarting", "onCatalogueResult",
    "onAddAllFinished",   "planAddAll",          "beforePluginRescan",
    "onPluginsUnloading", "attachBasemap",       "attachTrackInfo",
    "showDemonstrationInstrument", "drainTrackInfoText", "patchPageOpen",
    "webListening",       "tunerDisplayStyle",   "basemapFacts",
    "enrichWebTrack",     "fillWebImages",
};

// Calls a control MAY make to a state-changing name: the command path itself,
// and the few non-const Engine methods that change no receiver state.
const char* const kControlMayCall[] = {
    "applyCommand", "submitCommand", "AppWindow", "~AppWindow",
    // Usage telemetry: the window reports which panel was opened (a counter
    // in the report, not receiver state).
    "telemetryNotePanel",
    // the config snapshot the save compares and writes (window machinery; it accrues the usage report)
    "currentConfig",
    // a query: the tuned frequency (not declared const: activeSource() is not)
    "currentAbsoluteHz",
    // a query: the air centre a switch would carry
    "carriedAirCentre",
    // a pure function of its argument (static)
    "muteNameList",
    // the crash handler's description of the receiver, refreshed for a diagnostics bundle
    "refreshDiagContext",
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
    // the patch Radio inspector lists local I/Q recordings (0.99.40), the
    // same reviewed direct-call pattern.
    "patchListRecordings",
    // drawPatchFaces prunes a gone node's cached decoder text (engine/stage3b-pre
    // fields-to-commands round 2), the same reviewed direct-call pattern -
    // patchSinkLines_ CLOSED off kWindowMayWriteScoped as a result.
    "prunePatchSinkLines",
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
    {"drawUi", "applyWebControls();"},
    {"drawUi", "(void)applyCommand(q.c, q.longText);"},
    // ...and the transmit key rebuilt every frame - the dead-man's handle (OPEN 1).
    // drawPatchPage: closing the page stops the patch (OPEN 2), and the
    // patch's own radios are reconciled with the graph every frame (the patch
    // page's runtime, API gap 7).
    {"drawPatchPage", "pipeline_.patchRunner().reap();"},
    {"drawPatchPage", "patchRunning_ = false;"},
    {"drawPatchPage", "pipeline_.patchRunner().clear();"},
    {"drawPatchPage", "patchStopAll(true);"},
    {"drawPatchPage", "refreshPluginRunner();"},
    {"drawPatchView", "patchReconcile();"},
    {"drawPatchView", "patchApplyRunning();"},
    // drawTransmitPage: a transmitter follows the receiver's dial, per frame.
    {"drawTransmitPage", "followTransmitFrequency();"},
    // run: interactive start, the GPS test seam, the catalogue test hook, the
    // device seam's scan, the preset seam for a plugin with none (OPEN 5),
    // FOXSDR_PATCH_START (OPEN 5), the measurement hook, and the teardown.
    {"run", "startReceiver();"},
    {"run", "gpsReader_.start({hookPort, baud, cascade::core::GpsReader::kDefaultTimeoutS});"},
    {"run", "startCatalogFetch();"},
    {"run", "scanNative();"},
    {"run", "recordPluginStopped(cascade::core::pluginKey(lp), false);"},
    {"run", "patchRunning_ = true;"},
    {"run", "installSource(std::move(src));"},
    {"run", "followInputRate();"},
    {"run", "if (!measure_->tick(pipeline_, hooks)) {"},
    {"run", "pollGpsReader();"},
    {"run", "transmitter_.stop();"},
    {"run", "gpsReader_.stop();"},
    {"run", "flushBookmarkSave(true);"},
    {"run", "stopIqRecording();"},
    {"run", "stopAudioRecording();"},
    {"run", "pipeline_.stop();"},
    {"run", "patchStopAll(false);"},
    {"run", "detachAndUnloadPlugins();"},
    // --- engine stage 3a: window lines that still drive the engine directly ---
    // the census seam: a stand-in bias tee for a bounded run
    {"run", "biasStandIn_ = cascade::gui::biasStandInFor("},
    // the GPS test seam starts a read
    // the catalogue test hook points the store at its catalogue
    {"run", "pluginCatalogueUrl_ = pluginTestHook_;"},
    // the teardown marks the clean exit
    {"run", "telemetryCleanExit_ = true;"},
    // the crash handler's context, once a second and at start
    {"run", "refreshDiagContext();"},
    // the patch runtime's per-frame steps (OPEN 2/10). Since 0.99.40 the
    // patch is the main window's other face (drawPatchView), not a page of
    // its own; drawPatchPage is left with only the per-frame retire and the
    // close-stops-the-patch rule below.
    {"drawPatchView", "rebuildPatchCatalogue();"},
    // closing the page stops the patch (OPEN 2)
    {"drawPatchPage", "patchWasOpen_ = false;"},
    // the same
    {"drawPatchPage", "patchWasRunning_ = false;"},
    // the same
    {"drawPatchPage", "patchDspSig_.clear();"},
    // the same
    {"drawPatchPage", "patchRefused_.clear();"},
    // the one compile a frame
    {"drawPatchView", "patchPlan_ = cascade::core::patch::compile("},
    // the same (a const pointer handed to compile)
    {"drawPatchView", "&patchCatalogue_);"},
    // each radio publishes its set
    {"drawPatchView", "patchPublishSets();"},
    // the page is open this frame
    {"drawPatchView", "patchWasOpen_ = true;"},
    // the engine's frame begins: the snapshot's retry, then the first drain
    {"drawUi", "pumpFrameBegin();"},
    // endTakesOnFault, then pumpDecoderOutput
    {"drawUi", "pumpInputs();"},
    // applyPluginApi, then scannerFrame
    {"drawUi", "pumpPlugins();"},
    // the bookmark save and the one publish; the window copies the spectrum frame beside it
    {"drawUi", "publishWebSpectrum(pumpPublish());"},
    // where the receiver is decides the mute, before anything draws
    {"drawUi", "pumpAudioMute();"},
    // the transmit key rebuilt every frame, then the tick - the dead-man's handle (OPEN 1)
    {"drawUi", "submitTransmitPageKey(keyRequest);"},
    {"drawUi", "pumpTransmitter();"},
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
                      std::vector<std::string>& found) {
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
};

// THE RATCHET ON EVERY OTHER DIRECT READ (OPEN 3). The window still reads the
// rest of the engine's fields directly as a friend - safe only while both run
// on one thread. Each round that moves a group behind a handed copy lowers
// these; nothing may raise them. Counted over every AppWindow member, as
// `engine_.<field>` for any field the Engine declares.
constexpr int kWindowFieldsReadBudget = 163;
constexpr int kWindowFieldReadsBudget = 793;

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
};

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
            judgeControlLine(l, helpers, fields, false, found);
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
    CHECK(r.submitsInDraw >= 40);
    CHECK(eng.methods.size() >= 100);
    CHECK(eng.fields.size() >= 200);
    CHECK(r.violations == 0);
    // OPEN 3's ratchet.
    std::printf("  the window reads %zu engine fields directly, %d times (budget %d fields, %d reads)\n",
                r.fieldsRead.size(), r.fieldReads, kWindowFieldsReadBudget, kWindowFieldReadsBudget);
    if (static_cast<int>(r.fieldsRead.size()) > kWindowFieldsReadBudget || r.fieldReads > kWindowFieldReadsBudget) {
        std::printf("FAIL: more direct reads of the engine than the budget - read the new ones from a copy "
                    "the engine hands over (docs/engine-stage3.md OPEN 3)\n");
    }
    CHECK(static_cast<int>(r.fieldsRead.size()) <= kWindowFieldsReadBudget);
    CHECK(r.fieldReads <= kWindowFieldReadsBudget);
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
    return testSummary("test_command_path_guard");
}
