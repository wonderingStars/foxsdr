// test_command_path_guard.cpp - nothing but the command path changes the
// receiver (engine extraction stage 1, docs/engine-stage1.md).
//
// THE RULE IT HOLDS. Since stage 1 every desktop control that changes the
// receiver submits a FoxCommand, and AppWindow::applyCommand - with the engine
// machinery it calls - is the only code that changes it. A control written
// the old way would quietly reopen the second control path stage 1 closed,
// and nothing would notice until the engine moves to its own thread (stage 3)
// and the control races it. So the source is read, the way
// tests/test_stop_ends_recordings reads it for pipeline_.stop():
//
// WHAT IS SCANNED. Every .cpp in src/gui (not only app_window*.cpp: an
// AppWindow member can be defined in any file). The code is cut into
// DEFINITIONS at every column-0 function head; an AppWindow member is a
// definition named AppWindow::x, and a free function counts when its file
// also defines AppWindow members (a helper written beside them). Members of
// other classes are skipped - they cannot reach AppWindow's state, and a call
// that hands them an engine object is caught at the call site (rule 4).
// Comments and string literals are ignored.
//
// THREE KINDS OF DEFINITION.
//   ENGINE MEMBERS (kEngineMembers): applyCommand, the helpers it calls, the
//     restore, the machinery that runs the receiver. Allowed to change state.
//     Rule 5 keeps them free of ImGui input, so no control hides in one.
//   LINE-ALLOWED MEMBERS (kLineAllowed): members that draw AND carry a few
//     reviewed engine lines (drawUi's per-frame transmit key - OPEN 1; run's
//     startup, teardown and verification seams; drawPatchPage's close -
//     OPEN 2). Only the listed lines are exempt; any other line is judged as
//     a control's.
//   CONTROLS: everything else - every widget, key handler, gesture and new
//     helper. In a control:
//     1. no call to a state-changing helper (kHelpers);
//     2. no call on an engine object (kEngineObjects) except the reviewed
//        read-only ones (kReadOnly); a bare accessor that hands out the
//        object (pipeline_.activeSource()) only into a `const` binding;
//     3. no write to a receiver field (kFields): plain and compound
//        assignment, ++/--, taking its address, a subscripted write, or a
//        mutating container call - with or without `this->`;
//     4. no engine object passed as an argument (a free helper taking
//        Pipeline& would otherwise change state on a control's behalf).
//   5. an engine member contains no ImGui input (Button, Checkbox, Slider,
//      IsKeyPressed ...).
//   6. every name on the two allow-lists exists in the tree, so a stale
//      entry cannot quietly allow a future member of the same name.
//
// A new helper that must change state goes on kEngineMembers, in review, with
// a reason. The per-op test (test_apply_command) and review are the other two
// nets; an indirection none of the lists names (a stored lambda called later,
// a pointer to a receiver field kept in a member) is not seen here.
//
// Proven red (docs/engine-stage1.md): a setter in drawRadioSection; volume_
// assigned in drawToolbar; a setter in a helper outside the list; and the
// reviewer's probes - a widget calling a new helper that tunes, a Button in
// drawUi that sets the squelch and tunes, compound/increment/address/
// subscript/container writes to receiver fields, a free helper taking the
// Pipeline, and the same code in a new src/gui file.
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

// --- the allow-lists ---------------------------------------------------------------
const char* const kEngineMembers[] = {
    // Construction, teardown, restore.
    "AppWindow", "~AppWindow", "applyConfig",
    // THE command path and the helpers its ops call.
    "applyCommand", "applyControlRequest", "applyWebControls", "applyPluginApi",
    "startReceiver", "stopReceiver", "endTakes", "endTakesOnFault", "installSource", "followInputRate",
    "setModeIndex", "setVfoToAbsoluteHz", "tuneAbsoluteHz", "retuneSourceHz", "applyRetuneNow",
    "pollPendingRetune", "retuneFixedCentre", "tuneToBookmark", "addBookmarkHere", "importBookmarkFile",
    "flushBookmarkSave", "startIqRecording", "stopIqRecording", "startAudioRecording",
    "stopAudioRecording", "applyReceiverPosition", "loadBandPlan", "changeConverter",
    "applyConverterForSource", "switchBiasTee", "selectSourceById", "selectSource", "openIqFile",
    "openPlutoAt", "openPlutoFromBox", "launchSoundCardOpen", "requestAudioOpen", "openTransmitRadio",
    "closeTransmitRadio", "followTransmitFrequency", "patchPressStart", "patchAllOff",
    "applyUserPresetEdit",
    // The local command queue itself.
    "drainLocalCommands",
    // Devices: open completion, fault recovery, scans, the sound card, the mute.
    "finishDeviceOpen", "adoptDeviceMirrors", "openDeviceSync", "pollSourceAsync",
    "reopenAfterDriverFault", "pollSoapyRecovery", "scanNative", "scanSoapy", "pollSoundCard",
    "updateAudioMute", "pollAudioHealth", "pollAudioOpen", "applyAudioOpenResult", "pollMicOpen",
    "pollGpsReader", "scannerFrame",
    // Plugins and the store.
    "applyPluginPreset", "maybeAutoPreset", "maybeAutoPresetOnShow", "maybeAutoPresetOnStart",
    "setPluginStopped", "recordPluginStopped", "setPluginMutes", "setPluginTuneAllowed",
    "setPluginSettingsAllowed", "applyPluginTuneGrants", "applyPluginSettingsGrants",
    "stopMutingPlugins", "rescanPlugins", "detachAndUnloadPlugins", "refreshPluginRunner",
    "startCatalogFetch", "startInstall", "startUpdate", "startAddAll", "pumpAddAll",
    "pollPluginAsync", "removeInstalledPlugin", "removeBlockedPlugin", "pumpDecoderOutput",
    // The patch page's radio hand-over and run state.
    "patchReconcile", "patchStopAll", "patchApplyRunning",
    // What remote clients and plugins read.
    "publishWebSnapshot", "publishPluginApiState",
    // The RSP / RX888 switch helpers (free functions beside the members).
    "withRfNotch", "withDabNotch", "withHdrMode", "withAdcSwitches",
};

// Calls a control MAY make to an engine member: the command path itself.
// (Every other kEngineMembers name is a forbidden call in a control - rule 1 -
// so a control cannot reach state by calling machinery, and a new helper that
// calls machinery is itself judged a control until it is reviewed onto the
// list.)
const char* const kControlMayCall[] = {"applyCommand", "AppWindow", "~AppWindow"};

// Read-only queries exempt from rule 4 alone: each hands the open radio to a
// pure predicate (gui/bias_tee.hpp) to ask whether it has a bias tee / whether
// an "on" would be remembered. Rules 1-3 still apply to them.
const char* const kQueryMembers[] = {"biasTeeReachable", "biasKeyMayRememberNow"};

struct LineAllow {
    const char* member;
    const char* code;  // the exact code the line must contain
};
const LineAllow kLineAllowed[] = {
    // drawUi IS the frame loop's body: the command drains, the remote and
    // plugin applies, the scanner, and the per-frame machinery polls.
    {"drawUi", "drainLocalCommands();"},
    {"drawUi", "endTakesOnFault();"},
    {"drawUi", "pumpDecoderOutput();"},
    {"drawUi", "applyWebControls();"},
    {"drawUi", "applyPluginApi();"},
    {"drawUi", "scannerFrame();"},
    {"drawUi", "(void)applyCommand(q.c, q.longText);"},
    {"drawUi", "flushBookmarkSave(false);"},
    {"drawUi", "publishWebSnapshot();"},
    {"drawUi", "updateAudioMute();"},
    {"drawUi", "pollSourceAsync();"},
    {"drawUi", "pollSoundCard();"},
    {"drawUi", "pollSoapyRecovery();"},
    {"drawUi", "pollPendingRetune();"},
    {"drawUi", "pollPluginAsync();"},
    {"drawUi", "pumpAddAll();"},
    {"drawUi", "pollAudioOpen();"},
    {"drawUi", "pollAudioHealth();"},
    {"drawUi", "pollMicOpen();"},
    // ...and the transmit key rebuilt every frame - the dead-man's handle (OPEN 1).
    {"drawUi", "transmitter_.setLatched(key.latched);"},
    {"drawUi", "transmitter_.setPttHeld(key.pttHeld);"},
    {"drawUi", "transmitter_.tick();"},
    // drawPatchPage: closing the page stops the patch (OPEN 2), and the
    // patch's own radios are reconciled with the graph every frame (the patch
    // page's runtime, API gap 7).
    {"drawPatchPage", "pipeline_.patchRunner().reap();"},
    {"drawPatchPage", "patchRunning_ = false;"},
    {"drawPatchPage", "pipeline_.patchRunner().clear();"},
    {"drawPatchPage", "patchStopAll(true);"},
    {"drawPatchPage", "refreshPluginRunner();"},
    {"drawPatchPage", "patchReconcile();"},
    {"drawPatchPage", "patchApplyRunning();"},
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

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// The code of each line: comments and string literals blanked, CR dropped.
std::vector<std::string> codeLines(const std::string& text) {
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

std::vector<FieldPatterns> fieldPatterns() {
    std::vector<FieldPatterns> out;
    const std::string pre = "(^|[^A-Za-z0-9_.>]|this->)";
    const std::string assignOp = "\\s*([-+*/%&|^]|<<|>>)?=(?!=)";
    for (const char* f : kFields) {
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
        out.push_back(std::move(p));
    }
    return out;
}

// Rule 1's tokens: the curated helpers and every engine member (as a call)
// but the command path itself.
std::vector<std::string> helperTokens() {
    std::vector<std::string> out(std::begin(kHelpers), std::end(kHelpers));
    for (const char* m : kEngineMembers) {
        if (inList(m, kControlMayCall)) { continue; }
        const std::string call = std::string(m) + "(";
        if (std::find(out.begin(), out.end(), call) == out.end()) { out.push_back(call); }
    }
    return out;
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

struct Report {
    int violations = 0;
    int controls = 0;
    int submitsInDraw = 0;
    int engineLines = 0;
    int allowedLinesUsed = 0;
    std::set<std::string> membersSeen;
    std::set<std::string> allowedLinesSeen;
};

void scan(const fs::path& root, Report& r) {
    const fs::path gui = root / "src" / "gui";
    std::error_code ec;
    CHECK(fs::is_directory(gui, ec));
    const std::vector<FieldPatterns> fields = fieldPatterns();
    const std::vector<std::string> helpers = helperTokens();
    std::set<std::string> controls;
    for (const auto& e : fs::directory_iterator(gui, ec)) {
        if (e.path().extension() != ".cpp") { continue; }
        const std::string file = e.path().filename().string();
        const std::vector<std::string> lines = codeLines(readFile(e.path()));
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
            if (inList(member, kEngineMembers)) {
                // 5. no ImGui input in engine machinery.
                for (const char* in : kImGuiInput) {
                    if (l.find(std::string("ImGui::") + in) != std::string::npos) {
                        ++r.violations;
                        std::printf("FAIL: engine member AppWindow::%s takes ImGui input (%s) at %s:%zu"
                                    " - a control does not belong here\n",
                                    member.c_str(), in, file.c_str(), i + 1);
                    }
                }
                ++r.engineLines;
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
            judgeControlLine(l, helpers, fields, inList(member, kQueryMembers), found);
            for (const std::string& f : found) {
                ++r.violations;
                std::printf("FAIL: AppWindow::%s changes the receiver outside the command path (%s) at "
                            "%s:%zu\n      -> submit a command (docs/engine-stage1.md)\n",
                            member.c_str(), f.c_str(), file.c_str(), i + 1);
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
    Report r;
    scan(root, r);
    std::printf("  %d controls judged, %d submitCommand calls in draw members, %d engine-member lines, "
                "%d allowed engine lines in drawing members, %d violations\n",
                r.controls, r.submitsInDraw, r.engineLines, r.allowedLinesUsed, r.violations);
    // The scan saw the real files.
    CHECK(r.controls >= 60);
    CHECK(r.submitsInDraw >= 40);
    CHECK(r.engineLines >= 2000);
    CHECK(r.violations == 0);
    // 6. Every allow-list entry names something that exists.
    for (const char* m : kEngineMembers) {
        if (r.membersSeen.count(m) == 0) {
            std::printf("FAIL: kEngineMembers names %s, which is not defined - remove it\n", m);
        }
        CHECK(r.membersSeen.count(m) != 0);
    }
    for (const char* m : kQueryMembers) {
        if (r.membersSeen.count(m) == 0) {
            std::printf("FAIL: kQueryMembers names %s, which is not defined - remove it\n", m);
        }
        CHECK(r.membersSeen.count(m) != 0);
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
