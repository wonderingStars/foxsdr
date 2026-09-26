// test_command_path_guard.cpp - a widget may not change the receiver except by
// submitting a command (engine extraction stage 1, docs/engine-stage1.md).
//
// THE RULE IT HOLDS. Since stage 1 every desktop control that changes the
// receiver submits a FoxCommand, and AppWindow::applyCommand is the one place
// state changes. A new widget written the old way - calling a pipeline setter,
// a tuning helper or a recorder, or binding an ImGui control straight to a
// receiver field - would quietly reopen the second control path that stage 1
// closed, and nothing would notice until the engine moves to its own thread
// (stage 3) and the widget races it. So the source of src/gui is read, the way
// tests/test_stop_ends_recordings reads it for pipeline_.stop():
//
//   A. IN A WIDGET, NO STATE CHANGE. A "widget member" is every AppWindow
//      member whose name starts with "draw" (drawUi, the frame loop, is not
//      one), and the key and gesture handlers named in kGestureMembers. None
//      may contain any token in kForbidden: the receiver's setters, its
//      state-changing helpers, an assignment to a receiver field, or a
//      pointer to one (the address an ImGui widget writes through).
//
//   B. THE LOWEST-LEVEL SETTERS HAVE AN ALLOW-LIST. A pipeline, device or
//      transmitter setter may appear only in the members of kEngineMembers -
//      applyCommand, the helpers it calls, the restore, the teardown and the
//      engine's own machinery. A new helper that a widget could call to get
//      round rule A has to be added there, in review, with a reason.
//
// WHAT IT CANNOT SEE. A state change through an indirection this list does
// not name (a new helper outside rule B's tokens, a lambda stored and called
// later) is not caught; the per-op test (test_apply_command) and code review
// are the other two nets. Comments and string literals are ignored.
//
// Proven to go red: a direct pipeline_.setSquelchDb added to
// drawRadioSection, and a helper outside the allow-list calling a setter.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "test_check.hpp"

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

// Rule A. A token beginning with '=' is an ASSIGNMENT to that field (=, +=,
// -=, never ==); one beginning with '&' is its address.
const char* const kForbidden[] = {
    // The receiver's own setters and the calls that stop and start it.
    "pipeline_.set", "pipeline_.audio().set", "pipeline_.start(", "pipeline_.stop(",
    "device_->set", "transmitter_.set", "transmitter_.keyRemote", "transmitter_.releaseRemote",
    // The helpers that change the receiver.
    "startReceiver(", "stopReceiver(", "retuneSourceHz(", "tuneAbsoluteHz(", "setVfoToAbsoluteHz(",
    "applyRetuneNow(", "setModeIndex(", "requestAudioOpen(", "applyPluginPreset(",
    // Receiver fields, written.
    "=vfoOffsetKhz_", "=vfoBandwidthHz_", "=bandwidthIndex_", "=modeIndex_", "=squelchDb_",
    "=volume_", "=userMuted_", "=deemphIndex_", "=stereoEnabled_", "=nrEnabled_", "=nrStrength_",
    "=notchEnabled_", "=notchFreqHz_", "=notchQ_", "=autoNotch_",
    // Receiver fields, handed to an ImGui control to write.
    "&vfoOffsetKhz_", "&squelchDb_", "&volume_", "&deemphIndex_", "&stereoEnabled_", "&nrEnabled_",
    "&nrStrength_", "&notchEnabled_", "&notchFreqHz_", "&notchQ_", "&autoNotch_", "&userMuted_",
    // The display range and the band plan (engine state in the API).
    "=dbMin_", "=dbMax_", "&dbMin_", "&dbMax_", "=bandPlanSelection_", "loadBandPlan(",
    "spectrum_->setRange(",
    // The source and the radio.
    "selectSource(", "scanNative(", "scanSoapy(", "openPlutoFromBox(", "openPlutoAt(", "openIqFile(",
    "launchSoundCardOpen(", "switchBiasTee(", "changeConverter(", "installSource(",
    "withRfNotch(", "withDabNotch(", "withHdrMode(", "withAdcSwitches(",
    "=deviceAgc_", "=deviceAntenna_", "=deviceRateIndex_", "=lookForNetworkUsrps_",
    "&deviceAgc_", "&deviceGainsDb_", "&deviceRfNotch_", "&deviceDabNotch_", "&deviceHdr_",
    "&deviceDither_", "&deviceRandomiser_", "&lookForNetworkUsrps_", "=soundCardLive_",
    // The recorder.
    "startIqRecording(", "startAudioRecording(", "stopIqRecording(", "stopAudioRecording(",
    // Bookmarks and the scanner.
    "freqMgr_.add", "freqMgr_.removeAt(", "freqMgr_.updateAt(", "freqMgr_.removeGroup(",
    "importBookmarkFile(", "addBookmarkHere(", "tuneToBookmark(", "scanner_.start(",
    "scanner_.stop(", "scanner_.skip(", "scanner_.configure(",
    // Plugins and the store.
    "setPluginStopped(", "recordPluginStopped(", "setPluginMutes(", "setPluginTuneAllowed(",
    "setPluginSettingsAllowed(", "rescanPlugins(", "startCatalogFetch(", "startInstall(",
    "startUpdate(", "startAddAll(", "removeInstalledPlugin(", "removeBlockedPlugin(",
    "pluginRepo_.cancel(", "maybeAutoPreset", "stopMutingPlugins(", "pressCommand(",
    "applyUserPresetEdit(",
    // The patch page's run state.
    "patchPressStart(", "patchAllOff(",
    // The transmitter's settings (the key itself is drawUi's - OPEN).
    "openTransmitRadio(", "closeTransmitRadio(", "=transmitModeIndex_", "=transmitSplit_",
    "=transmitSplitHz_", "=transmitPowerDb_", "=transmitInputIndex_", "=transmitToneHz_",
    "=transmitMonitor_", "&transmitMonitor_", "micOpen_.request(",
    // Position and GPS.
    "applyReceiverPosition(", "gpsReader_.start(", "gpsReader_.stop(",
};

// Rule B's tokens: the setters themselves.
const char* const kLowLevel[] = {
    "pipeline_.set", "pipeline_.audio().set", "pipeline_.start(", "pipeline_.stop(",
    "device_->set",  "transmitter_.set",      "transmitter_.keyRemote", "transmitter_.releaseRemote",
};

// Rule B's allow-list: where a setter may be called. Every entry is engine
// machinery that stage 3 moves to the control thread with applyCommand.
const char* const kEngineMembers[] = {
    "AppWindow",               // the constructor: the startup state
    "applyCommand",            // THE one place
    "applyConfig",             // the restore: the saved state, not a control
    "run",                     // the frame loop's teardown
    "drawUi",                  // the frame loop: the TX key's per-frame rebuild (OPEN)
    "applyWebControls",        // the remote key's standing conditions
    "startReceiver", "stopReceiver", "installSource", "followInputRate", "setModeIndex",
    "setVfoToAbsoluteHz", "tuneToBookmark", "startIqRecording", "stopIqRecording",
    "startAudioRecording", "stopAudioRecording", "applyPluginPreset", "applyConverterForSource",
    "changeConverter", "detachAndUnloadPlugins", "refreshPluginRunner", "updateAudioMute",
    "pollSoundCard", "openTransmitRadio", "closeTransmitRadio", "followTransmitFrequency",
};

// Key and gesture handlers that are widgets although their names do not say
// "draw".
const char* const kGestureMembers[] = {"applyKeyAction", "dispatchKeyBindings", "pollKeyCapture",
                                       "biasKeyPressed", "biasKeyAnswered"};

// STAGE 1 WAS CONVERTED AREA BY AREA, one commit each, and the widgets of an
// area not yet converted were listed here and skipped by both rules. The
// list is EMPTY now that every area is converted; a name added back here is
// a widget that bypasses the command path, and says so in review. (The one
// entry is a placeholder no member can match.)
const char* const kNotYetConverted[] = {"(none)"};

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

// The AppWindow member a line belongs to: the nearest preceding definition
// that starts in column 0 and names AppWindow::<fn>(.
std::string enclosingMember(const std::vector<std::string>& lines, std::size_t at) {
    for (std::size_t i = at + 1; i-- > 0;) {
        const std::string& l = lines[i];
        if (l.empty() || l[0] == ' ' || l[0] == '\t' || l[0] == '}' || l[0] == '#') { continue; }
        const std::size_t p = l.find("AppWindow::");
        if (p == std::string::npos) { continue; }
        const std::size_t b = p + std::strlen("AppWindow::");
        const std::size_t e = l.find('(', b);
        if (e == std::string::npos) { continue; }
        const std::string name = l.substr(b, e - b);
        if (name.find_first_of(" <>:") != std::string::npos) { continue; }
        return name;
    }
    return "?";
}

bool inList(const std::string& m, const char* const* list, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        if (m == list[i]) { return true; }
    }
    return false;
}

template <std::size_t N>
bool inList(const std::string& m, const char* const (&list)[N]) {
    return inList(m, list, N);
}

bool isWidget(const std::string& m) {
    if (m == "drawUi") { return false; }
    return m.rfind("draw", 0) == 0 || inList(m, kGestureMembers);
}

struct Matcher {
    std::string token;
    bool regex = false;
    std::regex re;
};

std::vector<Matcher> matchers(const char* const* tokens, std::size_t n) {
    std::vector<Matcher> out;
    for (std::size_t i = 0; i < n; ++i) {
        Matcher m;
        m.token = tokens[i];
        if (m.token[0] == '=') {
            // name, then optional space, then =, += or -= that is not ==.
            m.regex = true;
            m.re = std::regex("(^|[^A-Za-z0-9_.>])" + m.token.substr(1) + "\\s*(\\+|-)?=([^=]|$)");
        }
        out.push_back(std::move(m));
    }
    return out;
}

bool hits(const Matcher& m, const std::string& code) {
    if (m.regex) { return std::regex_search(code, m.re); }
    return code.find(m.token) != std::string::npos;
}

void guard(int& violations, int& widgetsSeen, int& submits, int& engineUses) {
    const fs::path gui = fs::path(CASCADE_SOURCE_DIR) / "src" / "gui";
    std::error_code ec;
    CHECK(fs::is_directory(gui, ec));
    const std::vector<Matcher> forbidden =
        matchers(kForbidden, sizeof(kForbidden) / sizeof(kForbidden[0]));
    const std::vector<Matcher> lowLevel = matchers(kLowLevel, sizeof(kLowLevel) / sizeof(kLowLevel[0]));
    std::set<std::string> widgets;
    for (const auto& e : fs::directory_iterator(gui, ec)) {
        const std::string name = e.path().filename().string();
        if (name.rfind("app_window", 0) != 0 || e.path().extension() != ".cpp") { continue; }
        const std::vector<std::string> lines = codeLines(readFile(e.path()));
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const std::string& l = lines[i];
            const std::string member = enclosingMember(lines, i);
            const bool pending = inList(member, kNotYetConverted);
            if (isWidget(member)) {
                widgets.insert(member);
                if (l.find("submitCommand(") != std::string::npos) { ++submits; }
                for (const Matcher& m : forbidden) {
                    if (!hits(m, l) || pending) { continue; }
                    ++violations;
                    std::printf("FAIL: widget AppWindow::%s changes the receiver directly (%s) at %s:%zu\n"
                                "      -> submit a command instead (docs/engine-stage1.md)\n",
                                member.c_str(), m.token.c_str(), name.c_str(), i + 1);
                }
            }
            for (const Matcher& m : lowLevel) {
                if (!hits(m, l)) { continue; }
                if (inList(member, kEngineMembers)) {
                    ++engineUses;
                    continue;
                }
                if (pending) { continue; }
                ++violations;
                std::printf("FAIL: %s in AppWindow::%s at %s:%zu - not a member allowed to call a "
                            "setter (kEngineMembers)\n",
                            m.token.c_str(), member.c_str(), name.c_str(), i + 1);
            }
        }
    }
    widgetsSeen = static_cast<int>(widgets.size());
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    int violations = 0;
    int widgets = 0;
    int submits = 0;
    int engineUses = 0;
    guard(violations, widgets, submits, engineUses);
    std::printf("  %d widget members scanned, %d submitCommand calls in them, %d setter uses in "
                "allowed members, %d violations\n",
                widgets, submits, engineUses, violations);
    // The scan saw the real files: widgets exist, they submit commands, and
    // applyCommand and its helpers do call setters.
    CHECK(widgets >= 50);
    CHECK(submits >= 20);
    CHECK(engineUses >= 40);
    CHECK(violations == 0);
    // Areas still waiting to be converted: none.
    std::size_t pending = 0;
    for (const char* m : kNotYetConverted) { pending += std::strcmp(m, "(none)") != 0 ? 1u : 0u; }
    std::printf("  %zu widget members not yet converted\n", pending);
    CHECK(pending == 0u);
    return testSummary("test_command_path_guard");
}
