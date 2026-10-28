// test_engine_shell_guard.cpp - THE FENCE'S GUARD: what the compiler cannot hold about the interface's one door onto the engine
// (engine extraction stage 4, step 5; the record is the closing section of docs/engine-stage4.md).
//
// THE SUCCESSOR OF tests/test_command_path_guard.cpp. That ratchet scanned the whole window for reads and writes of the engine's state
// (163 reads of 31 fields at its first count, 788 calls before the port; 33 of 141 when the pump move began; 13 fields and 26 reads when
// the fence stepped in) and was written to reach zero. It did not: it ended at a CLASSIFIED RESIDUE (the shell's seams, the host
// accessors, stage 5's web/CAT reads - gone in stage 5f), and the fence did what a ratchet could not - the interface is its own target
// (cascade_interface), cmake/interface_fence.cmake puts stubs of every header it may not include first on its include path, and the
// window's engine member is an EngineShell, a class that declares only the residue. A direct read of an engine field, a call to an
// engine method, a write to engine state, an engine object passed as an argument: none of them compiles, so the ratchet's rules 1 to
// 4 are retired with the ratchet. What is left here is what the compiler CANNOT see:
//
//   1. THE SURFACE (engine/engine_shell.hpp) does not grow silently. Its section is dated and says how it ends; its declarations are
//      counted against a budget; every declaration is implemented and is CALLED (a stale door is found here, the way the old guard's
//      allow lists were). THE PLUGIN UI PORT (core/plugin_ui_port.hpp, shell retirement R4) is the same discipline on the other surface the
//      window has: 14 pure virtuals and one extension id, each implemented by the engine's side and called by the window, in a header of data
//      and pure virtuals that includes nothing the fence denies.
//   2. WHO MAY KNOCK. The compiler lets any AppWindow member call any EngineShell method; this holds which members call which (the
//      shell's seams from the shell's members), and which members reach the plugin UI through the port (the host accessors, the tile pump,
//      the front end's hand-over, the shutdown's release).
//      A widget that calls engine_.teardown() compiles. It fails here.
//   3. THE FENCE ITSELF IS STILL BUILT. The interface target exists and is put behind the fence; the deny list still holds the engine,
//      its host, its API table and its pipeline; the engine/ allow list stays small and names no denied header; no file under src/gui -
//      the headers no interface translation unit happens to include included - names a denied header (the compiler sees only what is
//      compiled); the engine's link check refuses the interface and the shell.
//   4. THE BACK DOOR (rawEngine) is named nowhere in src/gui, and the engine is reached by no other name (engineHolder_ is the
//      constructor's alone).
//   5. THE SURVIVORS of the old guard, which were never about reads: the frame is a look (three stepPass calls in drawUi, in order; the
//      engine's pass started in run()); the config save is the shell's (saveConfigNow twice, in run, around the control thread's life);
//      the ops that name a row by position are a reviewed set; the plugin UI is the window's (the host feed applies the directive
//      before any reference into it is taken); the data header split (the views take core/plugin_ui_data.hpp); no ImGui input in the
//      window's machinery.
//   6. THE TEST HOST (shell retirement R1): tests reach the Engine behind a window through th::engine and build windows as
//      th::TestWindow (tests/test_host.hpp); rawEngine is named in code by the shell's two files and that header alone, the window has
//      exactly one TestHostAccess friend, and no test constructs an AppWindow by that name.
//
// PROVEN RED (docs/engine-stage4.md, the closing section): a method added to engine_shell.hpp with no call, a call from a widget, a
// section without its date, a stub-less deny list ...; and the compiler's half by tests/CMakeLists.txt's interface_fence_* probes,
// which compile an interface translation unit that includes engine.hpp (and the others) and want the build to fail.
//
// argv[1], optional: a root to scan instead of the source tree (used to run the probes against a copy).
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

// ----------------------------------------------------------------------------------------------------------------------------------
// BUDGETS. Every number is the surface as the fence left it (2026-10-21); a change moves the number in the same commit, with the dated
// comment in engine_shell.hpp that says why. They only go down on their own: stage 5f deleted the web/CAT section (there is no budget for
// it any more), stage 7 deletes the shell's.
// ----------------------------------------------------------------------------------------------------------------------------------
constexpr int kShellMethodsBudget = 19;   // CLASS SHELL: EngineShell's own declarations (distinct names; view and the special members apart): 19 after shell retirement R7 (2026-10-28: -7, the construction flip - initialise, loadBookmarks, telemetryStartup, applyConfig and configReadback are the host's restore (EngineService::restore: main.cpp, or the test host, builds the engine's service and restores BEFORE the window exists), apiTable and apiHandle are main's to pass (WindowHost::api, handle); the shell's create(hooks) became view(engine, hooks), a non-owning view that installs the window as the engine's host through Engine::setHost; 26 before it); 26 after shell retirement R5 (2026-10-28: -1, setControlWatchdogReportDir - the Diagnostics switch is a field of the interface's half of the settings, which crosses the table as FOXAPP_OP_INTERFACE_CONFIG, and the control thread's hang-report directory is derived from it by the engine; 27 before it); 27 after shell retirement R4 (2026-10-28: -2, the plugin UI plane left the door - setFrontEndFacts (the face, the basemap's figures and the track feed ride the port's setFrontEnd) and reportPluginStatus (the engine prints the bounded run's report at the end of shutdownQuiesce, from the plugin half the window hands over in its last setFrontEnd); 29 before it); 29 after shell retirement R3 (2026-10-28: -4, status, URL and kick as table facts - pluginCatalogueUrl, publishStatusText, statusText, frameKick; 33 before it); 33 after shell retirement R2 (2026-10-28: -8, the seams that became client code - seamBiasStandIn, seamFetchCatalogue, seamSelectNativeDevice, seamDeviceOpenPending, seamPressPreset, seamPatchStart, seamPatchRunning, seamSubmit; 41 before it); 40 before the frame's kick (frameKick, 6.9)
// (kHostMethodsBudget and kFeedMethodsBudget are GONE, shell retirement R4, 2026-10-28: the HOST class - makePluginFeed and serveWebTiles, 2 after R3, 3 with setServerUsageHooks (stub campaign B15, gone in R3), 2 before B15 - and HostPluginFeed's 12 virtuals (12 with unplacedTracks, stub campaign B16; 11 before it) were deleted with their knock rows: makePluginFeed became the table's get_local_extension(FOXAPI_EXT_PLUGIN_UI), serveWebTiles became PluginUiPort::serveTiles, and the 12 virtuals became PluginUiPort's 14 (minus `report`, plus setFrontEnd, serveTiles and release), counted below as the EXTENSION surface, not the door.)
constexpr int kCallSitesBudget = 23;      // `engine_.x` in src/gui, all classes: 23 after shell retirement R7 (2026-10-28: -6, the constructor's initialise, loadBookmarks, telemetryStartup and configReadback (4), applyConfig's applyConfig (1) and openUiSession's apiHandle (1); 29 before it); 29 after shell retirement R5 (2026-10-28: -1, applyDiagnosticsEnabled's setControlWatchdogReportDir; 30 before it); 30 after shell retirement R4 (2026-10-28: -4, the four door methods of the plugin UI plane - makePluginFeed 1, setFrontEndFacts 1, reportPluginStatus 1, serveWebTiles 1 - whose sites are `ui_->` sites of the port now, 15 of them, counted by kPortCallSitesBudget; with them gone `pluginFeed_->x` (13 sites) is not a thing the guard counts any more: 47 before R4 was 34 engine_ + 13 pluginFeed_); 47 after shell retirement R3 (2026-10-28: -7, the five methods' sites - pluginCatalogueUrl 2, publishStatusText 1, statusText 2, frameKick 1, setServerUsageHooks 1; 54 before it); 54 after shell retirement R2 (2026-10-28: -13, the eight seams' sites - seamBiasStandIn 1, seamFetchCatalogue 1, seamSelectNativeDevice 1, seamDeviceOpenPending 1, seamPressPreset 1, seamPatchStart 1, seamPatchRunning 1, seamSubmit 6; 67 before it); 67 with stub campaign B16 (+1: hostUnplacedTracks' unplacedTracks, the one read of the new feed accessor); 66 with stub campaign B15 (+1: installTesterUsageHooks' setServerUsageHooks, the window's one call, from its constructor's helper); 65 after stage 5e and unchanged by 5f (serveWebTiles is still called, once, from pumpWebTiles) (-7: publishWebAudio's audioSamplesProduced, audioTap and pushWebAudio, publishWebImages' setWebImages, publishWebSpectrum's publishWebSpectrum, and pumpWebTiles' clearWebTiles, takeWebTileRequests and setWebTile; +1: pumpWebTiles' serveWebTiles); was `engine_.x` and `pluginFeed_->x` in src/gui, all classes: 72 after stage 5b (the servers are the engine's: -7 call sites - the constructor's setRemoteControlSource and webRemoteBeat, the destructor's setRemoteControlSource, webStatusNow's readFullSnapshot, catStatusNow's two reads, publishWebSpectrum's inputRateHz - and +6 feed calls: pushWebAudio, setWebImages, clearWebTiles, takeWebTileRequests, setWebTile, publishWebSpectrum); 73 with the frame's kick; 71 when the fence landed, 72 after the 0.99.84 merge split shutdownRelease in two (the plugins come down BEFORE the clean-exit marker, the drain of the lists' saves after it: master 0.99.60), 73 with the frame's kick, one call at the top of run()'s frame loop (6.9) (the ratchet it replaces ended at 13 fields and 26 reads)

// THE PLUGIN UI PORT (core/plugin_ui_port.hpp; shell retirement R4, 2026-10-28; docs/api-0.3-additions.md 9.15) - the window's one C++ contact with the engine that is not the door and not the table's lists and facts, reached through ONE table member (get_local_extension) on the window's LOCAL session. Its surface is held like the door's: counted, implemented, called.
constexpr int kPortMethodsBudget = 14;    // PluginUiPort's pure virtuals (the destructor apart): the seven host accessors (tracks, unplacedTracks, paths, panels, instruments, trackPluginNames, altitudeNear), poll, pollImages, applyNewestDirective, setFrontEnd, serveTiles, release, disconnect. 14 at R4: HostPluginFeed's 12 minus `report` (the plugin half of the status report rides setFrontEnd once) plus setFrontEnd (was the door's setFrontEndFacts), serveTiles (was the door's serveWebTiles) and release (the window's own shutdown release)
constexpr int kPortCallSitesBudget = 15;  // `ui_->x` in src/gui: 15 at R4 - the 13 sites `pluginFeed_->x` had, minus `report` (run: 1) plus setFrontEnd (handFrontEndFacts: 1), serveTiles (pumpWebTiles: 1) and release (run: 1); the extension's own call, `get_local_extension`, is a table call (attachPluginUi) and is held by checkThePort
constexpr int kExtensionIdsBudget = 1;    // FOXAPI_EXT_* in the vendored header: FOXAPI_EXT_PLUGIN_UI only. A second in-process extension is a decision (it reaches the engine's process by C++ objects), not a thing that happens

// ----------------------------------------------------------------------------------------------------------------------------------
// WHO MAY KNOCK: for each AppWindow member, the door methods it may call. EVERY ENTRY MUST BE MATCHED by a call (a stale entry fails),
// so this is exactly the set of calls the window makes - and the fence's list of who in the window talks to the engine at all.
// ----------------------------------------------------------------------------------------------------------------------------------
struct Knock {
    const char* member;
    std::vector<const char*> methods;
};
const std::vector<Knock> kEngineKnocks = {
    // CLASS SHELL: construction, the config restore, the run, the teardown
    {"AppWindow::AppWindow", {"publishConfig", "setFrontEndWaitBeat"}},   // (R7: the restore - initialise, loadBookmarks, telemetryStartup, applyConfig, configReadback - is the host's)
    {"AppWindow::~AppWindow", {"setFrontEndWaitBeat", "stopTransfers", "teardown"}},
    {"AppWindow::currentConfig", {"configSnapshot"}},
    {"AppWindow::saveConfigNow", {"publishConfig"}},
    {"AppWindow::run",
     {"startReceiver", "controlPass", "startControlThread", "configureHeartbeat", "markCleanExit", "shutdownQuiesce", "shutdownStop",
      "shutdownRelease", "shutdownDrain", "seamMeasureTick",
      "seamMarkPassStats", "seamPassStatsJson", "seamInstallSource"}},
    // the frame is a look: the three parts of the pass (the status lines the look reads are the table's facts - refreshStatusLines - and the frame's kick is
    // the heartbeat of run()'s loop: neither is a knock on the door any more, shell retirement R3)
    {"AppWindow::drawUi", {"stepPass"}},
    // (The browser map's tiles - pumpWebTiles' serveWebTiles - and the front end's facts - handFrontEndFacts' setFrontEndFacts - are the PORT's now:
    // kPortKnocks below. Shell retirement R4 deleted their rows from this table with the HOST class.)
};
// THE PLUGIN UI PORT (shell retirement R4; was the host feed, CLASS HOST): the host accessors and what drives them, the frame's hand-over, the
// browser map's tiles and the shutdown's release - every `ui_->x` in src/gui.
const std::vector<Knock> kPortKnocks = {
    {"AppWindow::hostTracks", {"tracks"}},
    {"AppWindow::hostUnplacedTracks", {"unplacedTracks"}},   // stub campaign B16
    {"AppWindow::hostPaths", {"paths"}},
    {"AppWindow::hostPanels", {"panels"}},
    {"AppWindow::hostInstruments", {"instruments"}},
    {"AppWindow::hostTrackPluginNames", {"trackPluginNames"}},
    {"AppWindow::hostAltitudeNear", {"altitudeNear"}},
    {"AppWindow::hostPollPlugins", {"poll"}},
    {"AppWindow::hostPollImages", {"pollImages"}},
    {"AppWindow::applyPluginDirective", {"applyNewestDirective"}},
    {"AppWindow::applyDirectiveAfter", {"instruments"}},
    {"AppWindow::~AppWindow", {"disconnect"}},
    {"AppWindow::run", {"release"}},                          // the window's own release of the plugin UI before the engine's (R4; R8's prelude moves it)
    {"AppWindow::handFrontEndFacts", {"setFrontEnd"}},        // the face, the basemap's figures, the track feed; and the bounded run's plugin report half (R4)
    {"AppWindow::pumpWebTiles", {"serveTiles"}},              // the browser map's tiles, drained and answered on the window's thread (R4; was serveWebTiles)
};

// The window's machinery: the members that drive the shell or answer for it. Rule: no ImGui input in them.
const char* const kWindowMachinery[] = {
    "AppWindow::AppWindow",       "AppWindow::~AppWindow",       "AppWindow::applyWindowHalf",  "AppWindow::currentConfig",
    "AppWindow::handFrontEndFacts", "AppWindow::wallTimeS",      "AppWindow::pauseWatchdog",    "AppWindow::resumeWatchdog",
    "AppWindow::applyDirectiveBefore", "AppWindow::applyDirectiveAfter", "AppWindow::attachPluginUi",
};
const char* const kImGuiInput[] = {
    "Button(", "SmallButton(", "InvisibleButton(", "ArrowButton(", "Checkbox(", "RadioButton(",
    "SliderFloat(", "SliderInt(", "SliderDouble(", "DragFloat(", "DragInt(", "InputText(",
    "InputTextWithHint(", "InputDouble(", "InputFloat(", "InputInt(", "Combo(", "BeginCombo(",
    "Selectable(", "MenuItem(", "IsItemClicked(", "IsItemActive(", "IsKeyPressed(", "IsKeyDown(",
    "IsMouseClicked(", "IsMouseDown(", "IsMouseDoubleClicked(",
};

// The headers the fence must always deny (a deny list that quietly lost one of these would be the fence with a hole).
const char* const kMustDeny[] = {"engine/engine.hpp",       "engine/engine_host.hpp", "engine/fox_engine_api.hpp", "engine/pass_stats.hpp",
                                 "core/pipeline.hpp",        "core/plugin_ui.hpp",     "core/plugin_api.hpp",       "core/plugin_runner.hpp",
                                 // the engine's side of the plugin UI port (R4): the window sees core/plugin_ui_port.hpp, never this
                                 "engine/plugin_ui_port.hpp",
                                 "core/recorder.hpp",        "core/transmitter.hpp",   "core/gps_reader.hpp",       "core/scanner.hpp",
                                 // src/net, denied whole by stage 5f (the servers are the engine's)
                                 "net/web_server.hpp",       "net/cat_server.hpp",     "net/web_session.hpp",       "net/status_compose.hpp"};
constexpr std::size_t kAllowedEngineHeadersBudget = 13;

// THE TEST HOST (shell retirement R1, 2026-10-28; docs/engine-stage4.md 6.25). The files that NAME rawEngine in code, in src/ and tests/ together:
// 72 before R1 (69 window tests, pump3_support.hpp's local access, the shell's two files, and the comment-only mentions the code scan blanks),
// 3 after - the shell's declaration and definition (they go with the shell, R10) and tests/test_host.hpp, the one audited place a test reaches
// the Engine behind its window. The budget only goes down: 1 at R10, when rawEngine itself is deleted.
constexpr int kRawEngineFilesBudget = 3;
const char* const kRawEngineAllowed[] = {"src/engine/engine_shell.hpp", "src/engine/engine_shell.cpp", "tests/test_host.hpp"};

// ----------------------------------------------------------------------------------------------------------------------------------

// (CR dropped: a CRLF checkout - this machine's autocrlf - must not move the \n}\n a body's end is found by.)
std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
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

// The lines of a file as they are, CR dropped (the vendored header is C: its comments stay, a #define is found at the start of its line).
std::vector<std::string> codeLinesRaw(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        out.push_back(line);
    }
    return out;
}

bool isIdent(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

// A column-0 function head: "AppWindow::x", "free:x", "other:Class::x", or "" when the line starts no definition.
std::string definitionAt(const std::string& l) {
    if (l.empty() || l[0] == ' ' || l[0] == '\t' || l[0] == '}' || l[0] == '{' || l[0] == '#') { return {}; }
    const std::size_t p = l.find('(');
    if (p == std::string::npos) { return {}; }
    const std::string head = l.substr(0, p);
    if (head.find('=') != std::string::npos || head.find(';') != std::string::npos) { return {}; }
    std::size_t w = 0;
    while (w < l.size() && isIdent(l[w])) { ++w; }
    static const std::set<std::string> kKeywords = {"namespace", "struct", "class", "using",  "typedef", "template", "static_assert",
                                                    "return",    "if",     "for",   "while",  "switch",  "case",     "default",
                                                    "else",      "enum",   "union", "extern"};
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

struct Definition {
    std::string name;
    std::vector<std::string> lines;
};

// The definitions of one file, cut at every column-0 function head (a free function counts as its own).
std::vector<Definition> definitionsOf(const std::string& text) {
    std::vector<Definition> out;
    Definition cur;
    for (const std::string& l : codeLines(text)) {
        const std::string d = definitionAt(l);
        if (!d.empty()) {
            if (!cur.name.empty() || !cur.lines.empty()) { out.push_back(std::move(cur)); }
            cur = Definition();
            cur.name = d;
        }
        cur.lines.push_back(l);
    }
    if (!cur.name.empty() || !cur.lines.empty()) { out.push_back(std::move(cur)); }
    return out;
}

std::vector<fs::path> guiFiles(const fs::path& root, bool headers) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(root / "src" / "gui", ec)) {
        const std::string ext = e.path().extension().string();
        if (ext == ".cpp" || (headers && (ext == ".hpp" || ext == ".h"))) { out.push_back(e.path()); }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// --- engine_shell.hpp -----------------------------------------------------------------------------------------------------------

struct Decl {
    std::string name;
    bool pure = false;  // `= 0`
};

// The declarations of a stretch of class-body code lines: statements ending in ';' at paren depth 0 that declare a function (a name
// followed by '('), minus friends, usings, deleted functions and data members. Constructors, the destructor and statics count.
std::vector<Decl> declarationsIn(const std::vector<std::string>& lines) {
    std::vector<Decl> out;
    std::string stmt;
    int paren = 0, brace = 0;
    for (const std::string& l : lines) {
        for (char c : l) {
            if (c == '(') { ++paren; }
            if (c == ')') { --paren; }
            if (c == '{') { ++brace; }
            if (c == '}') { --brace; }
            stmt += c;
            if ((c == ';' && paren == 0 && brace == 0) || (c == '}' && paren == 0 && brace == 0)) {
                std::string s = stmt;
                stmt.clear();
                // strip leading blanks and access specifiers glued to the statement, then skip what is not a function declaration
                bool skip = false;
                for (;;) {
                    const std::size_t st = s.find_first_not_of(" \t\n");
                    if (st == std::string::npos) {
                        skip = true;
                        break;
                    }
                    s = s.substr(st);
                    if (s.rfind("public:", 0) == 0) {
                        s = s.substr(7);
                    } else if (s.rfind("private:", 0) == 0) {
                        s = s.substr(8);
                    } else {
                        break;
                    }
                }
                if (skip) { continue; }
                if (s.rfind("friend", 0) == 0 || s.rfind("using", 0) == 0 || s.rfind("struct", 0) == 0 || s.rfind("class", 0) == 0) { continue; }
                if (s.find("= delete") != std::string::npos) { continue; }
                const std::size_t p = s.find('(');
                if (p == std::string::npos) { continue; }
                std::size_t e = p;
                while (e > 0 && s[e - 1] == ' ') { --e; }
                std::size_t b = e;
                while (b > 0 && (isIdent(s[b - 1]) || s[b - 1] == '~')) { --b; }
                if (b == e) { continue; }
                Decl d;
                d.name = s.substr(b, e - b);
                d.pure = s.find("= 0") != std::string::npos;
                out.push_back(d);
            }
        }
        stmt += ' ';
    }
    return out;
}

struct Surface {
    std::vector<Decl> shell, port;
    bool sectionsDated = true;
    std::string whyNot;
    std::string portText;  // core/plugin_ui_port.hpp, whole
};

Surface readSurface(const fs::path& root) {
    Surface s;
    const std::string text = readFile(root / "src" / "engine" / "engine_shell.hpp");
    CHECK(!text.empty());
    // The class body, by its opening line.
    const std::size_t shellAt = text.find("class EngineShell {");
    CHECK(shellAt != std::string::npos);
    const std::size_t shellEnd = text.find("\nprivate:", shellAt);
    CHECK(shellEnd != std::string::npos);
    // The marker inside EngineShell (a comment, so found in the raw text).
    const std::size_t mShell = text.find("CLASS SHELL (added", shellAt);
    CHECK(mShell != std::string::npos);
    CHECK(mShell < shellEnd);
    // THE HOST CLASS IS GONE (shell retirement R4): its marker may not come back without this file changing with it, any more than the WEB / CAT
    // class's (stage 5f) may. The words survive in comments (the retired classes are told of), so what is looked for is the MARKER form.
    const bool webClassGone = text.find("CLASS WEB / CAT (added") == std::string::npos;
    if (!webClassGone) { std::printf("FAIL: engine_shell.hpp has a CLASS WEB / CAT marker again - the class was deleted by stage 5f\n"); }
    CHECK(webClassGone);
    const bool hostClassGone = text.find("CLASS HOST (added") == std::string::npos && text.find("class HostPluginFeed") == std::string::npos;
    if (!hostClassGone) {
        std::printf("FAIL: engine_shell.hpp has a CLASS HOST marker or a HostPluginFeed again - the class was deleted by shell retirement R4: the plugin UI is "
                    "the port's (core/plugin_ui_port.hpp)\n");
    }
    CHECK(hostClassGone);
    // the marker carries a DATE and an END
    const std::size_t eol = text.find("\n", text.find("\n", mShell) + 1);
    const std::string head = text.substr(mShell, eol - mShell);
    if (!(std::regex_search(head, std::regex("added 20[0-9][0-9]-[01][0-9]-[0-3][0-9]")) && head.find("END:") != std::string::npos)) {
        s.sectionsDated = false;
        s.whyNot += " SHELL";
    }
    const auto lineStart = [&](std::size_t at) { return text.rfind('\n', at) == std::string::npos ? std::size_t(0) : text.rfind('\n', at) + 1; };
    (void)lineStart;
    s.shell = declarationsIn(codeLines(text.substr(shellAt + std::strlen("class EngineShell {"), shellEnd - (shellAt + std::strlen("class EngineShell {")))));

    // THE PORT: core/plugin_ui_port.hpp's class body.
    s.portText = readFile(root / "src" / "core" / "plugin_ui_port.hpp");
    CHECK(!s.portText.empty());
    const std::size_t portAt = s.portText.find("class PluginUiPort {");
    CHECK(portAt != std::string::npos);
    const std::size_t portEnd = s.portText.find("\n};", portAt);
    CHECK(portEnd != std::string::npos);
    s.port = declarationsIn(codeLines(s.portText.substr(portAt + std::strlen("class PluginUiPort {"), portEnd - (portAt + std::strlen("class PluginUiPort {")))));
    return s;
}

// The parts of engine_shell.hpp's EngineShell section that precede the marker are comments and the "public:"; CLASS SHELL's marker sits in
// the comment before create(), so the shell slice above starts at the class head and includes create(). Names of every door method:
std::set<std::string> namesOf(const std::vector<Decl>& v, bool skipCtors) {
    std::set<std::string> out;
    for (const Decl& d : v) {
        if (skipCtors && (d.name == "EngineShell" || d.name == "~EngineShell" || d.name == "create" || d.name == "view" || d.name == "~PluginUiPort")) { continue; }
        out.insert(d.name);
    }
    return out;
}

// --- the calls --------------------------------------------------------------------------------------------------------------------

struct Call {
    std::string file;
    int line = 0;
    std::string member;  // the definition it sits in
    std::string object;  // engine_ or ui_
    std::string method;
};

std::vector<Call> callsIn(const fs::path& root) {
    std::vector<Call> out;
    for (const fs::path& p : guiFiles(root, true)) {
        const std::string name = p.filename().string();
        const std::string ext = p.extension().string();
        std::string member;
        int n = 0;
        for (const std::string& l : codeLines(readFile(p))) {
            ++n;
            const std::string d = definitionAt(l);
            if (!d.empty()) { member = d; }
            for (const char* obj : {"engine_", "ui_"}) {
                const std::string needle = obj;
                for (std::size_t at = l.find(needle); at != std::string::npos; at = l.find(needle, at + 1)) {
                    if (at > 0 && (isIdent(l[at - 1]) || l[at - 1] == '.')) { continue; }
                    std::size_t i = at + needle.size();
                    if (i >= l.size()) { continue; }
                    if (l[i] == '.') {
                        if (needle == "ui_") { continue; }   // (`ui_.reset()` and the like are the window's own unique_ptr, not a call through the port)
                        ++i;
                    } else if (l[i] == '-' && i + 1 < l.size() && l[i + 1] == '>') {
                        i += 2;
                    } else {
                        continue;
                    }
                    std::size_t e = i;
                    while (e < l.size() && isIdent(l[e])) { ++e; }
                    if (e == i) { continue; }
                    Call c;
                    c.file = name + (ext == ".hpp" ? " (header)" : "");
                    c.line = n;
                    c.member = member.empty() ? std::string("?") : member;
                    c.object = obj;
                    c.method = l.substr(i, e - i);
                    out.push_back(c);
                }
            }
        }
    }
    return out;
}

// --- the fence's configuration ---------------------------------------------------------------------------------------------------

std::set<std::string> denyListOf(const fs::path& root, std::set<std::string>& allowedEngine) {
    std::set<std::string> deny;
    const std::string text = readFile(root / "cmake" / "interface_fence.cmake");
    CHECK(!text.empty());
    // cascade_interface_deny(<header> "<why>")
    const std::regex d("cascade_interface_deny\\(\\s*([A-Za-z0-9_/\\.]+)\\s+\"");
    for (std::sregex_iterator it(text.begin(), text.end(), d), end; it != end; ++it) {
        const std::string h = (*it)[1].str();
        if (h.find("${") == std::string::npos) { deny.insert(h); }
    }
    // the allow list for src/engine
    const std::size_t a = text.find("set(CASCADE_INTERFACE_ALLOWED_ENGINE_HEADERS");
    CHECK(a != std::string::npos);
    const std::size_t b = text.find(")", a);
    std::istringstream in(text.substr(a, b - a));
    std::string tok;
    in >> tok;  // set(CASCADE_INTERFACE_ALLOWED_ENGINE_HEADERS
    while (in >> tok) {
        if (tok.find(".hpp") != std::string::npos || tok.find(".h") != std::string::npos) { allowedEngine.insert(tok); }
    }
    // src/engine is denied by default
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(root / "src" / "engine", ec)) {
        const std::string ext = e.path().extension().string();
        if (ext != ".hpp" && ext != ".h") { continue; }
        const std::string name = e.path().filename().string();
        if (allowedEngine.count(name) == 0) { deny.insert("engine/" + name); }
    }
    // src/net is denied WHOLE (stage 5f: the servers are the engine's), with no allow list
    for (const auto& e : fs::directory_iterator(root / "src" / "net", ec)) {
        const std::string ext = e.path().extension().string();
        if (ext != ".hpp" && ext != ".h") { continue; }
        deny.insert("net/" + e.path().filename().string());
    }
    return deny;
}

std::vector<std::string> includesOf(const std::string& text) {
    std::vector<std::string> out;
    const std::regex inc("^\\s*#\\s*include\\s*\"([^\"]+)\"");
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::smatch m;
        if (std::regex_search(line, m, inc)) { out.push_back(m[1].str()); }
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------------------

void checkSurface(const fs::path& root, const Surface& s, const std::vector<Call>& calls) {
    const auto sh = namesOf(s.shell, true);
    const auto po = namesOf(s.port, true);
    std::printf("  the door's surface: shell %zu methods (budget %d); no host or web/CAT class; the plugin UI port's %zu virtuals (budget %d)\n", sh.size(),
                kShellMethodsBudget, po.size(), kPortMethodsBudget);
    if (static_cast<int>(sh.size()) > kShellMethodsBudget || static_cast<int>(po.size()) > kPortMethodsBudget) {
        std::printf("FAIL: engine_shell.hpp or core/plugin_ui_port.hpp grew past a budget - a new door is a decision: say which class it is, give it its "
                    "dated comment and its end, and move the budget in this file in the same change (or port the read the house way "
                    "instead of widening the door)\n");
    }
    CHECK(static_cast<int>(sh.size()) <= kShellMethodsBudget);
    CHECK(static_cast<int>(po.size()) <= kPortMethodsBudget);
    // The scan saw the real headers.
    CHECK(sh.size() >= 15);
    CHECK(po.size() >= 8);
    // ...and a class that only ever shrinks: a budget above what is there is a stale budget.
    CHECK(static_cast<int>(sh.size()) == kShellMethodsBudget);
    CHECK(static_cast<int>(po.size()) == kPortMethodsBudget);
    if (!s.sectionsDated) {
        std::printf("FAIL: engine_shell.hpp's section marker lacks 'added YYYY-MM-DD' or 'END:' for:%s\n", s.whyNot.c_str());
    }
    CHECK(s.sectionsDated);

    // Every declaration is implemented in engine_shell.cpp (EngineShell::x for the door), and every port virtual is a PURE one implemented by
    // the engine's side, src/engine/plugin_ui_port.cpp (PluginUiPortImpl::x).
    const std::string impl = readFile(root / "src" / "engine" / "engine_shell.cpp");
    CHECK(!impl.empty());
    for (const std::string& n : sh) {
        const bool ok = impl.find("EngineShell::" + n + "(") != std::string::npos;
        if (!ok) { std::printf("FAIL: EngineShell::%s is declared in engine_shell.hpp and defined nowhere in engine_shell.cpp\n", n.c_str()); }
        CHECK(ok);
    }
    const std::string portImpl = readFile(root / "src" / "engine" / "plugin_ui_port.cpp");
    CHECK(!portImpl.empty());
    for (const Decl& d : s.port) {
        if (d.name == "~PluginUiPort") { continue; }
        if (!d.pure) { std::printf("FAIL: PluginUiPort::%s is not pure - the port's header is data and pure virtuals\n", d.name.c_str()); }
        CHECK(d.pure);
        const bool ok = portImpl.find("PluginUiPortImpl::" + d.name + "(") != std::string::npos;
        if (!ok) { std::printf("FAIL: PluginUiPort::%s is declared and not implemented in engine/plugin_ui_port.cpp\n", d.name.c_str()); }
        CHECK(ok);
    }

    // Every declaration is CALLED from src/gui (a method nobody calls is a door nobody needs: retire it). create / apiTable are static
    // (EngineShell::x), the rest go through engine_ / ui_.
    std::set<std::string> engineCalled, portCalled;
    for (const Call& c : calls) { (c.object == "engine_" ? engineCalled : portCalled).insert(c.method); }
    std::string guiText;
    for (const fs::path& p : guiFiles(root, false)) { guiText += readFile(p); }
    for (const fs::path& p : guiFiles(root, true)) {
        if (p.extension() == ".hpp") { guiText += readFile(p); }
    }
    const auto called = [&](const std::string& n) {
        return engineCalled.count(n) != 0 || guiText.find("EngineShell::" + n + "(") != std::string::npos;
    };
    for (const std::string& n : sh) {
        if (!called(n)) {
            std::printf("FAIL: EngineShell::%s is declared and no member of the window calls it - retire the door (its last caller is gone)\n", n.c_str());
        }
        CHECK(called(n));
    }
    for (const std::string& n : po) {
        if (portCalled.count(n) == 0) {
            std::printf("FAIL: PluginUiPort::%s is declared and the window never calls it - retire it\n", n.c_str());
        }
        CHECK(portCalled.count(n) != 0);
    }
    // ...and the window calls nothing the door does not declare (the compiler says so too; this names the class when it is a typo of one).
    for (const Call& c : calls) {
        if (c.object == "engine_" && sh.count(c.method) == 0) {
            std::printf("FAIL: %s:%d calls engine_.%s, which engine_shell.hpp does not declare\n", c.file.c_str(), c.line, c.method.c_str());
            CHECK(false);
        }
        if (c.object == "ui_" && po.count(c.method) == 0) {
            std::printf("FAIL: %s:%d calls ui_->%s, which core/plugin_ui_port.hpp does not declare\n", c.file.c_str(), c.line, c.method.c_str());
            CHECK(false);
        }
    }
    (void)root;
}

void checkWhoMayKnock(const std::vector<Call>& calls) {
    std::set<std::string> matched;
    int sites = 0, portSites = 0, bad = 0;
    std::set<std::string> members;
    for (const Call& c : calls) {
        if (c.object == "engine_") { ++sites; } else { ++portSites; }
        const std::vector<Knock>& table = (c.object == "engine_") ? kEngineKnocks : kPortKnocks;
        bool ok = false;
        for (const Knock& k : table) {
            if (c.member != k.member) { continue; }
            for (const char* m : k.methods) {
                if (c.method == m) {
                    ok = true;
                    matched.insert(c.object + "|" + k.member + "|" + m);
                }
            }
        }
        members.insert(c.member);
        if (!ok) {
            ++bad;
            std::printf("FAIL: %s:%d: %s calls %s%s%s, which the fence does not let it - only the shell's members drive the door, the plugin UI's "
                        "accessors, the frame's hand-over, the tile pump and the shutdown go through the port (kEngineKnocks / kPortKnocks)\n",
                        c.file.c_str(), c.line, c.member.c_str(), c.object.c_str(), c.object == "engine_" ? "." : "->", c.method.c_str());
        }
    }
    std::printf("  who knocks: %d door call sites and %d port call sites in %zu window members, %d outside the table (budgets %d and %d sites)\n", sites,
                portSites, members.size(), bad, kCallSitesBudget, kPortCallSitesBudget);
    CHECK(bad == 0);
    CHECK(sites == kCallSitesBudget);
    CHECK(portSites == kPortCallSitesBudget);
    CHECK(sites >= 20);  // the scan saw the real files
    // Every table entry is matched by a call.
    for (const Knock& k : kEngineKnocks) {
        for (const char* m : k.methods) {
            const bool ok = matched.count("engine_|" + std::string(k.member) + "|" + m) != 0;
            if (!ok) { std::printf("FAIL: kEngineKnocks {%s, %s} matches no call - remove it\n", k.member, m); }
            CHECK(ok);
        }
    }
    for (const Knock& k : kPortKnocks) {
        for (const char* m : k.methods) {
            const bool ok = matched.count("ui_|" + std::string(k.member) + "|" + m) != 0;
            if (!ok) { std::printf("FAIL: kPortKnocks {%s, %s} matches no call - remove it\n", k.member, m); }
            CHECK(ok);
        }
    }
}

void checkTheFence(const fs::path& root, const std::set<std::string>& deny, const std::set<std::string>& allowedEngine) {
    // the deny list holds what it must
    for (const char* h : kMustDeny) {
        const bool ok = deny.count(h) != 0;
        if (!ok) { std::printf("FAIL: the fence no longer denies <%s> to the interface (cmake/interface_fence.cmake)\n", h); }
        CHECK(ok);
    }
    // src/net is denied WHOLE by the fence file, with no allow list and no way to take a header back out of the deny list (the guard reads
    // the directory for the deny set above; this holds that the file really does the same)
    {
        const std::string fenceText = readFile(root / "cmake" / "interface_fence.cmake");
        const bool netGlob = fenceText.find("src/net/*.hpp") != std::string::npos &&
                             fenceText.find("foreach(_h IN LISTS _cascade_net_headers)") != std::string::npos;
        const bool noTakeBack = fenceText.find("REMOVE_ITEM CASCADE_INTERFACE_DENIED") == std::string::npos &&
                                fenceText.find("ALLOWED_NET") == std::string::npos;
        if (!netGlob) { std::printf("FAIL: cmake/interface_fence.cmake no longer denies every header of src/net (stage 5f)\n"); }
        if (!noTakeBack) { std::printf("FAIL: cmake/interface_fence.cmake takes a header back out of the deny list or has a net/ allow list\n"); }
        CHECK(netGlob);
        CHECK(noTakeBack);
    }
    // the engine/ allow list stays small, and never names the Engine
    std::printf("  the fence: %zu headers denied, %zu engine/ headers allowed (budget %zu)\n", deny.size(), allowedEngine.size(),
                kAllowedEngineHeadersBudget);
    CHECK(allowedEngine.size() <= kAllowedEngineHeadersBudget);
    CHECK(allowedEngine.count("engine.hpp") == 0);
    CHECK(allowedEngine.count("engine_host.hpp") == 0);
    CHECK(allowedEngine.count("fox_engine_api.hpp") == 0);
    CHECK(allowedEngine.count("engine_shell.hpp") == 1);

    // the interface target is built, from src/gui, behind the fence, and the headless programs refuse it
    const std::string cmake = readFile(root / "CMakeLists.txt");
    CHECK(!cmake.empty());
    const bool target = cmake.find("add_library(cascade_interface STATIC ${CASCADE_GUI})") != std::string::npos;
    const bool fenced = cmake.find("cascade_apply_interface_fence(cascade_interface)") != std::string::npos;
    const bool shellTarget = cmake.find("add_library(cascade_shell STATIC src/engine/engine_shell.cpp)") != std::string::npos;
    const bool excluded = cmake.find("EXCLUDE REGEX \"engine_shell") != std::string::npos;
    const bool refused = cmake.find("^cascade_interface$|^cascade_shell$") != std::string::npos;
    const bool headless = cmake.find("set(_todo cascade_engine cascade_engine_link_check foxsdr-reports foxsdr-engine)") != std::string::npos;
    if (!target || !fenced) { std::printf("FAIL: CMakeLists.txt no longer builds the window as cascade_interface behind the fence\n"); }
    if (!shellTarget || !excluded) { std::printf("FAIL: engine_shell.cpp is not the cascade_shell target's alone (the headless engine must not carry it)\n"); }
    if (!refused || !headless) { std::printf("FAIL: the engine-side link check no longer refuses cascade_interface / cascade_shell for the headless programs\n"); }
    CHECK(target);
    CHECK(fenced);
    CHECK(shellTarget);
    CHECK(excluded);
    CHECK(refused);
    CHECK(headless);
    // the compile probes exist (tests/CMakeLists.txt registers them)
    const std::string tcmake = readFile(root / "tests" / "CMakeLists.txt");
    const bool probes = tcmake.find("interface_fence_denies_") != std::string::npos && tcmake.find("interface_fence_allows") != std::string::npos;
    if (!probes) { std::printf("FAIL: tests/CMakeLists.txt registers no interface_fence_* compile probes\n"); }
    CHECK(probes);

    // NO FILE UNDER src/gui NAMES A DENIED HEADER - the compiler sees only the headers some interface translation unit happens to include.
    int named = 0;
    for (const fs::path& p : guiFiles(root, true)) {
        for (const std::string& inc : includesOf(readFile(p))) {
            if (deny.count(inc) != 0) {
                ++named;
                std::printf("FAIL: src/gui/%s includes <%s>, which the fence denies the interface\n", p.filename().string().c_str(), inc.c_str());
            }
        }
    }
    CHECK(named == 0);

    // THE SIBLING HOLE. An allowed header of src/engine that itself includes a denied one would be caught by the compiler only through a
    // translation unit that reaches it; this holds it for the allowed ones whether or not one does yet.
    int holes = 0;
    for (const std::string& h : allowedEngine) {
        for (const std::string& inc : includesOf(readFile(root / "src" / "engine" / h))) {
            if (deny.count(inc) != 0) {
                ++holes;
                std::printf("FAIL: src/engine/%s is on the interface's allow list and includes <%s>, which is denied\n", h.c_str(), inc.c_str());
            }
        }
    }
    CHECK(holes == 0);

    // THE BACK DOOR. rawEngine returns the Engine; naming it from src/gui is the one way left to hold an incomplete reference. And the
    // engine is reached by no other name: engineHolder_ is the constructor's.
    int back = 0;
    for (const fs::path& p : guiFiles(root, true)) {
        for (const std::string& l : codeLines(readFile(p))) {
            if (l.find("rawEngine") != std::string::npos) {
                ++back;
                std::printf("FAIL: src/gui/%s names rawEngine - the Engine behind the door is not the interface's\n", p.filename().string().c_str());
            }
        }
    }
    CHECK(back == 0);
    int holders = 0;
    for (const fs::path& p : guiFiles(root, true)) {
        for (const std::string& l : codeLines(readFile(p))) {
            if (l.find("engineHolder_") != std::string::npos) { ++holders; }
        }
    }
    std::printf("  engineHolder_: %d code lines in src/gui (expect 3: the declaration, the initialiser and engine_'s)\n", holders);
    CHECK(holders == 3);
}

// RULE 5, kept from the ratchet: the window's machinery contains no ImGui input.
void checkMachineryHasNoInput(const fs::path& root) {
    std::set<std::string> seen;
    int bad = 0;
    for (const fs::path& p : guiFiles(root, false)) {
        for (const Definition& d : definitionsOf(readFile(p))) {
            if (std::find(std::begin(kWindowMachinery), std::end(kWindowMachinery), d.name) == std::end(kWindowMachinery)) { continue; }
            seen.insert(d.name);
            for (const std::string& l : d.lines) {
                for (const char* in : kImGuiInput) {
                    if (l.find(std::string("ImGui::") + in) != std::string::npos) {
                        ++bad;
                        std::printf("FAIL: %s draws input (%s) - the machinery drives the door, a control is a different member\n", d.name.c_str(), in);
                    }
                }
            }
        }
    }
    for (const char* m : kWindowMachinery) {
        if (seen.count(m) == 0) { std::printf("FAIL: kWindowMachinery names %s, which the window does not define - remove it\n", m); }
        CHECK(seen.count(m) != 0);
    }
    CHECK(bad == 0);
}

// THE FRAME IS A LOOK (the pump move's steps P4 and P5): the engine's pass runs on its own thread, so the window runs no engine phase,
// drains no queue, applies no command and takes no GPS fix; it looks at what the pass published, draws, and submits. What this holds, as
// text over src/gui:
//   * stepPass appears exactly three times, all in drawUi, as Front, Mute and Back, in that order, and the engine's pass is started in
//     run() exactly once (startControlThread) after the one controlPass() that makes a free-running run's first publish;
//   * THE LOOK'S ORDER (survey 1.6 B and C): in drawUi the session's answers are polled (pollUiAnswers), then the status lines
//     are read (refreshStatusLines: the table's fact.status.* since shell retirement R3), then the state is read (refreshUiState), then
//     the answers are acted on (actOnUiAnswers) - and the Front part of the pass comes before all of it, the Mute part after it, and
//     the Back part after everything else the frame does;
//   * THE FRAME'S KICK is the window's heartbeat (shell retirement R3): `engineApi_->heartbeat(uiSession_)` is called exactly once in the
//     window, as the first act of run()'s frame loop, never from drawUi, and the session it beats on is opened with FOXAPI_SESSION_FRAME;
//   * the transmit key is SUBMITTED at the end of the frame (submitTransmitKey), after the page has drawn what it holds.
// (The ratchet also held that no member names a pump phase or the command path: the door does not declare them, so naming one does not
// compile.)
void checkTheFrameIsALook(const fs::path& root, const std::vector<Call>& calls) {
    int stepCalls = 0, controlPassCalls = 0, startCalls = 0, kickCalls = 0;
    for (const Call& c : calls) {
        if (c.object != "engine_") { continue; }
        if (c.method == "frameKick") { ++kickCalls; }   // (the door no longer has it: the frame's kick is the heartbeat of a FRAME session)
        if (c.method == "stepPass") { ++stepCalls; }
        if (c.method == "controlPass") { ++controlPassCalls; }
        if (c.method == "startControlThread") { ++startCalls; }
    }
    std::printf("  the frame is a look: stepPass %d (expect 3), controlPass %d (expect 1), startControlThread %d (expect 1)\n", stepCalls,
                controlPassCalls, startCalls);
    CHECK(stepCalls == 3);
    CHECK(kickCalls == 0);   // THE FRAME KICKS THE PASS through the table now (6.9, R3): the heartbeat in run()'s loop - checked below
    CHECK(controlPassCalls == 1);
    CHECK(startCalls == 1);
    const std::string text = readFile(root / "src" / "gui" / "app_window.cpp");
    const std::size_t draw = text.find("void AppWindow::drawUi() {");
    CHECK(draw != std::string::npos);
    std::size_t drawEnd = text.find("\n}\n", draw);
    CHECK(drawEnd != std::string::npos);
    const std::string body = text.substr(draw, drawEnd - draw);
    const auto at = [&](const char* needle) { return body.find(needle); };
    const std::size_t front = at("engine_.stepPass(cascade::engine::PassPart::Front);");
    const std::size_t poll = at("pollUiAnswers();");
    const std::size_t status = at("refreshStatusLines();");
    const std::size_t look = at("refreshUiState();");
    const std::size_t act = at("actOnUiAnswers();");
    const std::size_t mute = at("engine_.stepPass(cascade::engine::PassPart::Mute);");
    const std::size_t key = at("submitTransmitKey();");
    const std::size_t back = at("engine_.stepPass(cascade::engine::PassPart::Back);");
    const bool found = front != std::string::npos && poll != std::string::npos && status != std::string::npos && look != std::string::npos &&
                       act != std::string::npos && mute != std::string::npos && key != std::string::npos && back != std::string::npos;
    if (!found) { std::printf("FAIL: drawUi lacks one of the look's steps\n"); }
    CHECK(found);
    if (found) {
        const bool order = front < poll && poll < status && status < look && look < act && act < mute && mute < key && key < back;
        if (!order) {
            std::printf("FAIL: drawUi's look is out of order - the pass's front part, then the session's answers polled, the status lines "
                        "read, the state read and the answers acted on, then the mute part, the transmit key, the back part\n");
        }
        CHECK(order);
    }
    // THE DIAGNOSTICS SWITCH MOVES BOTH WATCHDOGS: "off means off" has to reach the engine's own hang watchdog (the control thread's), or
    // a session switched off would keep writing the pass's hang reports. Since shell retirement R5 it reaches it INSIDE THE INTERFACE'S HALF
    // of the settings (FOXAPP_OP_INTERFACE_CONFIG; Engine::applyInterfaceConfig moves the watchdog between "off" and the directory run() named in
    // ControlThreadOptions::diagnosticsDir), not by a call of its own: applyDiagnosticsEnabled moves the window's watchdog and does NOT name the
    // engine, run() names the directory when it starts the thread, and the frame loop sends the half after the settings keeper, every frame.
    {
        const std::size_t diag = text.find("void AppWindow::applyDiagnosticsEnabled(bool on) {");
        const std::size_t diagEnd = diag == std::string::npos ? std::string::npos : text.find("\n}\n", diag);
        const bool window = diag != std::string::npos && diagEnd != std::string::npos && text.find("watchdog_.setReportDir(", diag) < diagEnd;
        const bool noCall = diag != std::string::npos && diagEnd != std::string::npos &&
                            text.substr(diag, diagEnd - diag).find("engine_.") == std::string::npos;
        if (!window) { std::printf("FAIL: applyDiagnosticsEnabled does not move the window's watchdog\n"); }
        if (!noCall) { std::printf("FAIL: applyDiagnosticsEnabled calls the engine - the switch reaches its watchdog inside the interface's half\n"); }
        CHECK(window);
        CHECK(noCall);
        const std::size_t runAt = text.find("int AppWindow::run(int frames) {");
        const std::size_t dirAt = runAt == std::string::npos ? std::string::npos : text.find("opt.diagnosticsDir = diagCrashDir_;", runAt);
        const std::size_t threadAt = runAt == std::string::npos ? std::string::npos : text.find("engine_.startControlThread(opt);", runAt);
        const std::size_t keepAt = runAt == std::string::npos ? std::string::npos : text.find("maybeSaveConfig(glfwGetTime());", runAt);
        const std::size_t sendAt = keepAt == std::string::npos ? std::string::npos : text.find("(void)sendInterfaceHalf(glfwGetTime());", keepAt);
        const bool sent = dirAt != std::string::npos && threadAt != std::string::npos && dirAt < threadAt && sendAt != std::string::npos &&
                          text.find("sendInterfaceHalf(glfwGetTime())") == sendAt + 6 && text.find("sendInterfaceHalf(glfwGetTime())", sendAt + 7) == std::string::npos;
        if (!sent) {
            std::printf("FAIL: run() does not name the diagnostics directory before it starts the pass, or does not send the interface's half once a frame, "
                        "after the settings keeper\n");
        }
        CHECK(sent);
    }
    // run(): the first publish, then the thread - and the thread starts before the frame loop
    const std::size_t run = text.find("int AppWindow::run(int frames) {");
    CHECK(run != std::string::npos);
    const std::size_t firstPass = text.find("engine_.controlPass();", run);
    const std::size_t startThread = text.find("engine_.startControlThread(opt);", run);
    // (0.99.67: the loop also ends when the console the program started on asks for the close - core/console_close.hpp.)
    const std::size_t loop = text.find("while (!glfwWindowShouldClose(window) && !closeRequested_ && !cascade::core::consoleCloseRequested()) {", run);
    const bool startOk = firstPass != std::string::npos && startThread != std::string::npos && loop != std::string::npos &&
                         firstPass < startThread && startThread < loop;
    if (!startOk) { std::printf("FAIL: run() does not start the engine's pass (first publish, then the thread) before the frame loop\n"); }
    CHECK(startOk);
    // THE FRAME KICKS THE PASS as the first act of its loop (6.9): after the frame log's and the cap's start stamps, before the events are polled
    // and the frame is drawn (the pass takes ~0.1 ms to publish, and the look is little later than the top of the frame: a later kick loses
    // that race and the look reads a publish a whole frame old - measured in 6.9). Since shell retirement R3 the kick is the window's HEARTBEAT,
    // on a session opened FOXAPI_SESSION_FRAME: `engineApi_->heartbeat(uiSession_)` appears once in the window, at that place, and not in drawUi;
    // and openUiSession is where the flag is asked for (a heartbeat on a session without it kicks nothing).
    {
        const std::size_t capStart = text.find("frameCap_->frameStart();", loop);
        const std::size_t kick = text.find("engineApi_->heartbeat(uiSession_);", loop);
        const std::size_t poll = text.find("glfwPollEvents();", loop);
        const bool kickOk = capStart != std::string::npos && kick != std::string::npos && poll != std::string::npos && loop < capStart &&
                            capStart < kick && kick < poll && text.find("engineApi_->heartbeat(", kick + 1) == std::string::npos &&
                            text.find("engineApi_->heartbeat(") == kick && body.find("heartbeat(") == std::string::npos;
        if (!kickOk) { std::printf("FAIL: run() does not beat the FRAME session exactly once, as the first act of the frame loop (after the start stamps, before glfwPollEvents), or drawUi beats it\n"); }
        CHECK(kickOk);
        const std::size_t open = text.find("void AppWindow::openUiSession() {");
        const std::size_t openEnd = open == std::string::npos ? std::string::npos : text.find("\n}\n", open);
        const bool flagged = open != std::string::npos && openEnd != std::string::npos &&
                             text.find("p.flags = FOXAPI_SESSION_LOCAL | FOXAPI_SESSION_FRAME;", open) < openEnd;
        if (!flagged) { std::printf("FAIL: openUiSession does not open the window's session with FOXAPI_SESSION_FRAME - its heartbeat would kick nothing\n"); }
        CHECK(flagged);
    }
}

// THE CONFIG SAVE IS THE SHELL'S, NOT THE FRAME'S (the pump move's step P4; the survey's 1.6 D). saveConfigNow asks the engine to hand over
// its half of the config (publishConfig -> fillConfig, which reads receiver fields) - a read of the pass's own state, safe only when no
// pass is running: before the control thread starts and after it is joined. So it is called from run() exactly twice, the first before
// startControlThread and the second after shutdownQuiesce (which joins it), and from nowhere else.
void checkConfigSaveIsTheShells(const fs::path& root) {
    int calls = 0, callsOutsideRun = 0;
    for (const fs::path& p : guiFiles(root, false)) {
        const std::string text = readFile(p);
        std::size_t at = 0;
        while ((at = text.find("saveConfigNow();", at)) != std::string::npos) {
            const std::size_t lineStart = text.rfind('\n', at) == std::string::npos ? 0 : text.rfind('\n', at) + 1;
            const std::string line = text.substr(lineStart, text.find('\n', at) - lineStart);
            const bool comment = line.find("//") != std::string::npos && line.find("//") < line.find("saveConfigNow();");
            const bool definition = line.find("void AppWindow::saveConfigNow") != std::string::npos;
            if (!comment && !definition) {
                ++calls;
                const std::size_t run = text.find("int AppWindow::run(int frames) {");
                const std::size_t runEnd = run == std::string::npos ? std::string::npos : text.find("\n}\n", run);
                if (run == std::string::npos || at < run || at > runEnd) { ++callsOutsideRun; }
            }
            at += 1;
        }
    }
    std::printf("  saveConfigNow: %d call(s) (expect 2, both in run()), %d outside run()\n", calls, callsOutsideRun);
    CHECK(calls == 2);
    CHECK(callsOutsideRun == 0);
    const std::string text = readFile(root / "src" / "gui" / "app_window.cpp");
    const std::size_t run = text.find("int AppWindow::run(int frames) {");
    const std::size_t first = text.find("if (!configPath_.empty()) { saveConfigNow(); }", run);
    const std::size_t start = text.find("engine_.startControlThread(opt);", run);
    const std::size_t quiesce = text.find("engine_.shutdownQuiesce();", run);
    const std::size_t second = text.find("if (!configPath_.empty()) { saveConfigNow(); }", quiesce);
    const bool ordered = first != std::string::npos && start != std::string::npos && quiesce != std::string::npos && second != std::string::npos &&
                         first < start && start < quiesce && quiesce < second;
    if (!ordered) {
        std::printf("FAIL: run()'s two config saves are not around the control thread's life (first save, start thread, ..., shutdownQuiesce, second save)\n");
    }
    CHECK(ordered);
}

// THE OPS THAT NAME A ROW BY ITS POSITION (the pump move's step P4; the survey's 1.6 J). A command that carries an INDEX into a list the
// window cached can land on a different row once the lists move under it: with the pass on a thread of its own a list can move between the
// window's look and the pass that applies the click. The port moved the dangerous ones to ids and keys. What is left names a position, and
// each was audited against the engine's handler:
//   FOXAPP_OP_SET_BANDWIDTH_STEP   an index into kBwHz, a CONSTANT table (not a list): it cannot move.
//   FOXAPI_OP_PLUGIN_PRESET        the raw index of a preset in the module's own static array; changes only when the module file is replaced
//                                  and rescanned (the command also carries the module's name, and the engine refuses an index past the end).
//   FOXAPP_OP_USER_PRESET_APPLY / FOXAPP_OP_USER_PRESET_FORGET_AT   an ordinal among the stored user presets of ONE plugin key (also carried):
//                                  the list changes only by this window's own FORGET and by a save, both user gestures a frame apart.
//   FOXAPP_OP_SOUND_CARD_PICK      the SOUND_CARDS entry's id (index + 1) of the last listing: a re-listing between the click and the pass could
//                                  shift it (the engine refuses an id past the end and says "no such sound card entry"); the same window as before
//                                  the move, a pass wider. RECORDED, NOT CLOSED: closing it would carry the card's name and host API beside the id.
// THE TEST holds the SET: any op whose documented argument is an index or an ordinal must be on the reviewed list above, so a new one is a
// decision somebody makes and not a thing that happens.
void checkIndexOps(const fs::path& root) {
    const std::set<std::string> reviewed = {"FOXAPP_OP_SET_BANDWIDTH_STEP", "FOXAPI_OP_PLUGIN_PRESET", "FOXAPP_OP_USER_PRESET_APPLY",
                                            "FOXAPP_OP_USER_PRESET_FORGET_AT", "FOXAPP_OP_SOUND_CARD_PICK"};
    std::set<std::string> found;
    const std::regex define("^#define\\s+(FOX(?:API|APP)_OP_[A-Z0-9_]+)\\s+0x[0-9A-Fa-f]+u?\\s*(.*)$");
    const std::regex positional("(index|ordinal|\\bid \\(index)", std::regex::icase);
    for (const char* rel : {"third_party/foxsdr_api/foxsdr_api.h", "src/core/app_commands.hpp"}) {
        std::istringstream in(readFile(root / rel));
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') { line.pop_back(); }
            std::smatch m;
            if (!std::regex_match(line, m, define)) { continue; }
            const std::string doc = m[2].str();
            // (SET_BANDWIDTH_DRAG's text says the combo's step INDEX is "left alone": it carries a width, not an index.)
            if (m[1].str() != "FOXAPP_OP_SET_BANDWIDTH_DRAG" && std::regex_search(doc, positional)) { found.insert(m[1].str()); }
        }
    }
    // SOUND_CARD_PICK's index is documented a few lines above its define; the other four on the define's own line.
    found.insert("FOXAPP_OP_SOUND_CARD_PICK");
    std::string unreviewed;
    for (const std::string& f : found) {
        if (reviewed.count(f) == 0) { unreviewed += (unreviewed.empty() ? "" : ", ") + f; }
    }
    std::string stale;
    for (const std::string& r : reviewed) {
        if (found.count(r) == 0) { stale += (stale.empty() ? "" : ", ") + r; }
    }
    std::printf("  ops that name a row by position: %zu found, %zu reviewed (see checkIndexOps)\n", found.size(), reviewed.size());
    if (!unreviewed.empty()) {
        std::printf("FAIL: an op documents an index or an ordinal argument and is not on the reviewed list: %s\n      -> carry the row's id or key instead, or audit it "
                    "against the engine's handler and add it to checkIndexOps (the pump move's survey, 1.6 J)\n",
                    unreviewed.c_str());
    }
    if (!stale.empty()) { std::printf("FAIL: the reviewed list names ops that no longer document an index: %s\n", stale.c_str()); }
    CHECK(unreviewed.empty());
    CHECK(stale.empty());
}

// THE HOOKS THE PUMP MOVE'S STEPS P2 AND P3 REMOVED STAY REMOVED, and the window overrides only the three the fence left it. Each retired
// hook was a call the engine made from its pass into the window, writing or reading window state. The window's base is ShellHooks now
// (three pure virtuals, so the compiler holds that it overrides no more); what the compiler cannot hold is that the ENGINE's host class
// does not grow them back, or call them: engine_host.hpp must not declare them, no engine source may call one through host_.
const char* const kRetiredHooks[] = {"onDisplayRange",     "onBookmarksChanged", "onConverterChanged", "openPluginWindowsFor",
                                     "drainTrackInfoText", "enrichWebTrack",     "fillWebImages",      "beforePluginRescan",
                                     "onPluginsUnloading", "attachBasemap",      "attachTrackInfo",    "showDemonstrationInstrument"};

void checkRetiredHooks(const fs::path& root) {
    int found = 0;
    const auto flag = [&](const std::string& where, const std::string& line, const char* hook) {
        ++found;
        std::printf("FAIL: retired host hook %s is back in %s: %s\n", hook, where.c_str(), line.c_str());
    };
    for (const char* rel : {"src/engine/engine_host.hpp", "src/engine/engine_host.cpp", "src/engine/engine_shell.hpp"}) {
        for (const std::string& l : codeLines(readFile(root / rel))) {
            for (const char* h : kRetiredHooks) {
                if (l.find(std::string(h) + "(") != std::string::npos) { flag(rel, l, h); }
            }
        }
    }
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
    // The window overrides exactly the three of ShellHooks.
    int overrides = 0;
    for (const std::string& l : codeLines(readFile(root / "src" / "gui" / "app_window.hpp"))) {
        if (l.find(") const override;") != std::string::npos || l.find(") override;") != std::string::npos) { ++overrides; }
    }
    std::printf("  the retired host hooks (%zu) are still gone: %d reappeared; the window overrides %d (expect 3: wallTimeS, pauseWatchdog, resumeWatchdog)\n",
                sizeof(kRetiredHooks) / sizeof(kRetiredHooks[0]), found, overrides);
    CHECK(found == 0);
    CHECK(overrides == 3);
}

// THE PLUGIN UI'S OWNERSHIP (pump move step P3, behind the host feed since the fence): the plugin UI - its instances, its basemap cache, its
// track-info cache - is the WINDOW'S and is touched from the window's thread alone; the engine reaches it only by posting a directive. What
// this holds, as text:
//   * no engine source names a PluginUi except through headlessUi_, and that only as the member's declaration and as the one line that
//     applies a directive to the engine's own plugin UI where there is no window (Engine::deliverPluginDirective);
//   * engine/plugin_ui_port.cpp (the port's engine side) is the only other file that names PluginUi, and the window names none (the port holds it);
//   * the basemap cache and the track-info cache are attached and detached in applyDirectiveAfter and, for the bounded run's stand-in, in
//     run() - nowhere else, so every GL delete they cause is on the window's thread;
//   * the newest directive is applied at the head of the frame (drawUi) before the frame touches the engine or draws a thing, and at the head
//     of the plugin poll (hostPollPlugins) before the poll - a reference into the plugin UI is never taken before it.
void checkPluginUiOwnership(const fs::path& root) {
    int bad = 0;
    const auto flag = [&](const std::string& where, const std::string& line, const char* why) {
        ++bad;
        std::printf("FAIL: %s: %s [%s]\n", where.c_str(), line.c_str(), why);
    };
    std::error_code ec;
    int headlessLines = 0;
    for (const auto& e : fs::directory_iterator(root / "src" / "engine", ec)) {
        const std::string ext = e.path().extension().string();
        if (ext != ".cpp" && ext != ".hpp") { continue; }
        for (const std::string& l : codeLines(readFile(e.path()))) {
            if (l.find("pluginUi_") != std::string::npos && e.path().filename() != "engine_shell.cpp") {
                flag(e.path().filename().string(), l, "the engine has no pluginUi_ any more");
            }
            if (l.find("headlessUi_") != std::string::npos) {
                ++headlessLines;
                const bool declaration = l.find("PluginUi headlessUi_{pluginApi_}") != std::string::npos;
                const bool apply = l.find("headlessUi_.apply(x)") != std::string::npos;
                // (shell retirement R4: a front end that attaches late lets go of what the engine's own plugin UI built before it came - the
                // one release directive Engine::attachPluginUi applies to it)
                const bool attachRelease = l.find("headlessUi_.apply(gone)") != std::string::npos;
                if (!declaration && !apply && !attachRelease) { flag(e.path().filename().string(), l, "the engine drives its own plugin UI only through apply()"); }
            }
        }
    }
    CHECK(headlessLines == 3);  // the declaration, the one apply and (R4) the attach's release of what it built: all exist, and nothing else
    int basemapAttach = 0, basemapDetach = 0, infoAttach = 0, infoDetach = 0, uiNamed = 0, linkNamed = 0;
    for (const fs::path& p : guiFiles(root, true)) {
        for (const std::string& l : codeLines(readFile(p))) {
            // (the WORD PluginUi: the machinery the interface may not name. PluginUiPort, PluginUiAttach and PluginUiFrontEnd are the port's own names.)
            static const std::regex pluginUiWord(R"(\bPluginUi\b)");
            if (std::regex_search(l, pluginUiWord)) { ++uiNamed; }
            if (l.find("PluginDirectiveLink") != std::string::npos || l.find("pluginDirectives()") != std::string::npos) { ++linkNamed; }
            if (p.filename() != "app_window.cpp") { continue; }
            if (l.find("basemap_.attach(") != std::string::npos) { ++basemapAttach; }
            if (l.find("basemap_.detach(") != std::string::npos) { ++basemapDetach; }
            if (l.find("trackInfo_.attach(") != std::string::npos) { ++infoAttach; }
            if (l.find("trackInfo_.detach(") != std::string::npos) { ++infoDetach; }
        }
    }
    std::printf("  the plugin UI is the window's, behind the port: basemap attach %d detach %d, track-info attach %d detach %d, PluginUi named %d, "
                "the link named %d (expect 2 1 1 1 0 0)\n",
                basemapAttach, basemapDetach, infoAttach, infoDetach, uiNamed, linkNamed);
    CHECK(basemapAttach == 2);
    CHECK(basemapDetach == 1);
    CHECK(infoAttach == 1);
    CHECK(infoDetach == 1);
    CHECK(uiNamed == 0);
    CHECK(linkNamed == 0);
    {
        const std::string text = readFile(root / "src" / "gui" / "app_window.cpp");
        const std::size_t draw = text.find("void AppWindow::drawUi() {");
        CHECK(draw != std::string::npos);
        const std::size_t applyInDraw = text.find("applyPluginDirective();", draw);
        const std::size_t firstEngine = text.find("engine_.", draw);
        CHECK(applyInDraw != std::string::npos && firstEngine != std::string::npos);
        const bool drawOk = applyInDraw < firstEngine && applyInDraw - draw < 2500;
        if (!drawOk) { std::printf("FAIL: drawUi does not apply the plugin directive before its first engine call\n"); }
        CHECK(drawOk);
        const std::size_t poll = text.find("void AppWindow::hostPollPlugins() {");
        CHECK(poll != std::string::npos);
        const std::size_t applyInPoll = text.find("applyPluginDirective();", poll);
        const std::size_t pollCall = text.find("ui_->poll();", poll);
        const bool pollOk = applyInPoll != std::string::npos && pollCall != std::string::npos && applyInPoll < pollCall && pollCall - poll < 1500;
        if (!pollOk) { std::printf("FAIL: hostPollPlugins does not apply the plugin directive before it polls\n"); }
        CHECK(pollOk);
        // the feed applies the window's step BEFORE the plugin UI's rebuild and its step AFTER it (a release folds the map pages while
        // the instances still stand)
        const std::string impl = readFile(root / "src" / "engine" / "plugin_ui_port.cpp");
        const std::size_t before = impl.find("steps_.before(f);");
        const std::size_t apply = impl.find("ui_.apply(d);");
        const std::size_t after = impl.find("steps_.after(f);");
        const bool order = before != std::string::npos && apply != std::string::npos && after != std::string::npos && before < apply && apply < after;
        if (!order) { std::printf("FAIL: the port does not run the window's step, the plugin UI's apply, the window's second step in that order\n"); }
        CHECK(order);
    }
    CHECK(bad == 0);
}

// THE DATA HEADER SPLIT (window port group 14, part d; the fence ledger): the views draw plugin data - a track, a path, a panel's rows, an
// instrument's state, a preset - and need none of the machinery that fills it. core/plugin_ui_data.hpp carries the data; core/plugin_ui.hpp
// (PluginUi, the host table, the mute policy) includes it and is the HOST PROCESS's. The gui headers the interface draws from include the
// data header and NEVER plugin_ui.hpp (the fence's stub would stop it too; this holds the text of the six that carry the data).
void checkDataHeaderSplit(const fs::path& root) {
    const char* const kDataOnlyHeaders[] = {"app_window.hpp",  "decoder_reads.hpp", "instrument_face.hpp",
                                            "map_view.hpp",    "scope_view.hpp",    "track_detail_view.hpp"};
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
    // The data headers the fence's hoists made (core/spectrum_frame.hpp, scanner_params.hpp, tx_input.hpp, and since stage 5b
    // server_ports.hpp - the settings panels' ports and address classifiers - and since stage 5f demod_names.hpp, the mode names
    // engine/receiver_tables.hpp checks the window's buttons against) stay as light.
    for (const char* h : {"spectrum_frame.hpp", "scanner_params.hpp", "tx_input.hpp", "server_ports.hpp", "demod_names.hpp"}) {
        const std::string t = readFile(root / "src" / "core" / h);
        CHECK(!t.empty());
        for (const std::string& inc : includesOf(t)) {
            std::printf("FAIL: core/%s includes \"%s\" - a hoisted data header takes nothing of the engine\n", h, inc.c_str());
            CHECK(false);
        }
    }
    // STAGES 5b AND 5f: the window's header takes the panels' vocabulary from core/server_ports.hpp, and net/ is fenced whole (5f): the
    // deny list names every header of the directory (read from the directory, so a new one is denied too), no src/gui file includes any
    // of them (checkTheFence, over the deny list), and no header the interface is allowed - the engine/ allow list, the hoisted data
    // headers - includes one either. The compiler holds the same by the interface_fence_denies_net_* probes.
    {
        const std::string w = readFile(root / "src" / "gui" / "app_window.hpp");
        const bool hoisted = w.find("#include \"core/server_ports.hpp\"") != std::string::npos;
        if (!hoisted) { std::printf("FAIL: src/gui/app_window.hpp does not include core/server_ports.hpp\n"); }
        CHECK(hoisted);
        int netNamed = 0;
        for (const fs::path& p : guiFiles(root, true)) {
            for (const std::string& inc : includesOf(readFile(p))) {
                if (inc.rfind("net/", 0) == 0) {
                    ++netNamed;
                    std::printf("FAIL: src/gui/%s includes <%s> - net/ is the engine's and fenced whole (stage 5f)\n",
                                p.filename().string().c_str(), inc.c_str());
                }
            }
        }
        CHECK(netNamed == 0);
        // the one allowed header that used to reach net/ (receiver_tables.hpp, for the mode names) takes them from the data header
        const std::string rt = readFile(root / "src" / "engine" / "receiver_tables.hpp");
        CHECK(!rt.empty());
        const bool rtClean = rt.find("#include \"net/") == std::string::npos && rt.find("#include \"core/demod_names.hpp\"") != std::string::npos;
        if (!rtClean) { std::printf("FAIL: engine/receiver_tables.hpp must take the mode names from core/demod_names.hpp and include nothing of net/\n"); }
        CHECK(rtClean);
    }
}

// ----------------------------------------------------------------------------------------------------------------------------------
// THE PLUGIN UI PORT (shell retirement R4, 2026-10-28; docs/api-0.3-additions.md 9.15). The window's contact with the plugin UI is ONE table member
// (get_local_extension, FOXAPI_EXT_PLUGIN_UI) and the C++ interface it returns. What the compiler cannot hold about it, held as text:
//   * the port's header is data and pure virtuals: it includes the standard library, the plugin ABI, the data headers and the vendored header and
//     nothing the fence denies (the interface includes it; the sibling-hole check above covers the engine/ allow list, this covers the new header);
//   * there is ONE extension id in the vendored header, and the table member and its capability bit exist;
//   * the window asks for the port once, in its constructor, with its own session (opened first) and before the engine's start-up scan - and a
//     failed ask leaves it the null port, not a null pointer;
//   * THE EXIT PATHS. ~AppWindow ends the engine's use of the window's applier as its last act, after the engine's teardown (which releases the
//     plugin UI through that applier, inline, with the control thread joined); run() releases the plugin UI through the port on the window's own
//     thread before the engine's shutdownRelease; and the port's destructor disconnects, so a constructor that throws, a window that could not be
//     created and the software-GL relaunch (the window object is destroyed before the second process starts, main.cpp) all end disconnected;
//   * THE ENGINE'S HALF. attachPluginUi replays the kept state and lets go of the engine's own instances; deliverPluginDirective records the
//     state before it posts; get_local_extension refuses a REMOTE session and an engine-internal one; setFrontEnd COPIES the track feed.
// ----------------------------------------------------------------------------------------------------------------------------------
std::string bodyAfter(const std::string& text, const std::string& head) {
    const std::size_t at = text.find(head);
    if (at == std::string::npos) { return {}; }
    const std::size_t end = text.find("\n}\n", at);
    return end == std::string::npos ? std::string() : text.substr(at, end - at);
}

void checkThePort(const fs::path& root, const std::set<std::string>& deny, const Surface& surface) {
    // 1. the header is data and pure virtuals, and includes nothing denied
    int badInc = 0;
    for (const std::string& inc : includesOf(surface.portText)) {
        const bool ok = inc == "foxsdr_api.h" || inc == "core/host_image.hpp" || inc == "core/plugin_abi.h" || inc == "core/plugin_ui_data.hpp";
        if (!ok || deny.count(inc) != 0) {
            ++badInc;
            std::printf("FAIL: core/plugin_ui_port.hpp includes \"%s\" - the port's header takes the plugin ABI, the data headers and the vendored header only\n",
                        inc.c_str());
        }
    }
    CHECK(badInc == 0);
    CHECK(deny.count("core/plugin_ui_port.hpp") == 0);       // the interface MAY include it: that is what it is for
    CHECK(deny.count("engine/plugin_ui_port.hpp") != 0);     // ...and may not include the engine's side
    for (const Decl& d : surface.port) {
        if (d.name != "~PluginUiPort" && !d.pure) { CHECK(false); }
    }

    // 2. one extension id, the member and its bit
    const std::string api = readFile(root / "third_party" / "foxsdr_api" / "foxsdr_api.h");
    CHECK(!api.empty());
    int ids = 0;
    for (const std::string& l : codeLinesRaw(api)) {
        if (l.rfind("#define FOXAPI_EXT_", 0) == 0) { ++ids; }
    }
    std::printf("  extension ids in the vendored header: %d (budget %d); get_local_extension and bit 29 %s\n", ids, kExtensionIdsBudget,
                api.find("get_local_extension)(") != std::string::npos ? "present" : "ABSENT");
    CHECK(ids == kExtensionIdsBudget);
    CHECK(api.find("#define FOXAPI_CAP_LOCAL_EXT     (1ull << 29)") != std::string::npos);
    CHECK(api.find("int32_t (FOXAPI_CALL *get_local_extension)(FoxEngine *engine, uint32_t extension, const FoxLocalExtensionArgs *args,") != std::string::npos);

    // 3. the window's ask
    const std::string text = readFile(root / "src" / "gui" / "app_window.cpp");
    const std::string ctorRaw = bodyAfter(text, "AppWindow::AppWindow(const WindowHost& host)");
    CHECK(!ctorRaw.empty());
    std::string ctor;   // the constructor's CODE: its comments (which name initialise and the file's load to say they moved) and strings blanked
    for (const std::string& l : codeLines(ctorRaw)) { ctor += l + "\n"; }
    {
        // SINCE R7 THE ENGINE IS RESTORED BEFORE THE WINDOW EXISTS (the host's EngineService::restore, main.cpp / the test host), so the constructor opens
        // its session on the table the host passed, attaches the plugin UI port ONCE - a LATE attach: the scan has run, the engine replays its current
        // directive - and starts nothing up: it names no initialise, no file load, no receiver restore.
        const std::size_t session = ctor.find("openUiSession();");
        const std::size_t attach = ctor.find("attachPluginUi();");
        const bool tableFromHost = ctor.find("engineApi_ = host.api;") != std::string::npos && ctor.find("uiEngine_ = host.handle;") != std::string::npos &&
                                   ctor.find("engineApi_ = host.api;") < session;
        const bool ok = session != std::string::npos && attach != std::string::npos && session < attach && tableFromHost &&
                        ctor.find("attachPluginUi();", attach + 1) == std::string::npos && ctor.find("initialise") == std::string::npos &&
                        ctor.find("ConfigStore::load(") == std::string::npos && ctor.find("telemetryStartup") == std::string::npos &&
                        ctor.find("loadBookmarks") == std::string::npos;
        if (!ok) { std::printf("FAIL: the constructor does not open its session on the host's table, attach the plugin UI port once (a late attach), and leave the engine's start-up and the settings file to the host\n"); }
        CHECK(ok);
    }
    {
        const std::string fn = bodyAfter(text, "void AppWindow::attachPluginUi() {");
        CHECK(!fn.empty());
        const bool ok = fn.find("get_local_extension(") != std::string::npos && fn.find("FOXAPI_EXT_PLUGIN_UI") != std::string::npos &&
                        fn.find("attach.session = uiSession_;") != std::string::npos && fn.find("makeNullPluginUiPort()") != std::string::npos &&
                        text.find("get_local_extension(") == text.find("get_local_extension(", text.find("void AppWindow::attachPluginUi() {"));
        if (!ok) { std::printf("FAIL: attachPluginUi does not ask the table for FOXAPI_EXT_PLUGIN_UI with the window's session (once, in the window), or leaves no null port on a refusal\n"); }
        CHECK(ok);
    }

    // 4. the exit paths
    {
        const std::string dtor = bodyAfter(text, "AppWindow::~AppWindow() {");
        CHECK(!dtor.empty());
        const std::size_t teardown = dtor.find("engine_.teardown();");
        const std::size_t disconnect = dtor.find("ui_->disconnect();");
        const bool last = teardown != std::string::npos && disconnect != std::string::npos && teardown < disconnect &&
                          dtor.find_first_not_of(" \n", dtor.find(';', disconnect) + 1) == std::string::npos;
        if (!last) { std::printf("FAIL: ~AppWindow does not disconnect the plugin UI port as its last act, after the engine's teardown (the exit paths: a window that could not be created, a thrown constructor's sibling, the software-GL relaunch)\n"); }
        CHECK(last);
        const std::size_t run = text.find("int AppWindow::run(int frames) {");
        CHECK(run != std::string::npos);
        const std::size_t stop = text.find("engine_.shutdownStop();", run);
        const std::size_t release = text.find("ui_->release();", run);
        const std::size_t engineRelease = text.find("engine_.shutdownRelease();", run);
        const bool ordered = stop != std::string::npos && release != std::string::npos && engineRelease != std::string::npos && stop < release &&
                             release < engineRelease && text.find("ui_->release();", release + 1) == std::string::npos;
        if (!ordered) { std::printf("FAIL: run() does not release the plugin UI through the port, once, between the pipeline's stop and the engine's own release\n"); }
        CHECK(ordered);
        // the bounded run's plugin report half is handed over before the first teardown phase, and only when it was asked for
        const std::size_t hand = text.find("if (pluginStatusReport) { handFrontEndFacts(true); }", run);
        const std::size_t quiesce = text.find("engine_.shutdownQuiesce();", run);
        const bool handOk = hand != std::string::npos && quiesce != std::string::npos && hand < quiesce;
        if (!handOk) { std::printf("FAIL: run() does not hand the plugin status report's half over before the engine's first teardown phase\n"); }
        CHECK(handOk);
    }

    // 5. the engine's half
    {
        const std::string eng = readFile(root / "src" / "engine" / "engine.cpp");
        const std::string attach = bodyAfter(eng, "bool Engine::attachPluginUi(PluginUiApplier applier) {");
        CHECK(!attach.empty());
        const bool replay = attach.find("pluginUiState_") != std::string::npos && attach.find("headlessUi_.apply(gone)") != std::string::npos &&
                            attach.find("pluginLink_.post(") != std::string::npos && attach.find("replay->release = false;") != std::string::npos;
        if (!replay) { std::printf("FAIL: Engine::attachPluginUi does not release the engine's own instances and post the kept state (the late attach's replay)\n"); }
        CHECK(replay);
        const std::string deliver = bodyAfter(eng, "bool Engine::deliverPluginDirective(cascade::core::PluginDirective d) {");
        CHECK(!deliver.empty());
        const std::size_t record = deliver.find("recordPluginUiStateLocked(d);");
        const std::size_t post = deliver.find("pluginLink_.post(");
        if (record == std::string::npos || post == std::string::npos || record > post) {
            std::printf("FAIL: deliverPluginDirective does not record the plugin UI's state before it posts (a late front end would miss it)\n");
        }
        CHECK(record != std::string::npos && post != std::string::npos && record < post);
        const std::string apiCpp = readFile(root / "src" / "engine" / "fox_engine_api.cpp");
        const std::string ext = bodyAfter(apiCpp, "int32_t localExtension(uint32_t extension, const FoxLocalExtensionArgs* args, void** out) {");
        CHECK(!ext.empty());
        const bool refuses = ext.find("FOXAPI_SESSION_REMOTE") != std::string::npos && ext.find("session->internal != InternalKind::None") != std::string::npos &&
                             ext.find("FOXAPI_DENIED") != std::string::npos && ext.find("FOXAPI_SESSION_LOCAL") != std::string::npos;
        if (!refuses) { std::printf("FAIL: get_local_extension does not refuse a REMOTE or engine-internal session\n"); }
        CHECK(refuses);
        const std::string port = readFile(root / "src" / "engine" / "plugin_ui_port.cpp");
        const std::string front = bodyAfter(port, "void PluginUiPortImpl::setFrontEnd(const cc::PluginUiFrontEnd& f) {");
        CHECK(!front.empty());
        const bool copies = front.find("std::make_shared<const TrackFeed>(f.trackFeed)") != std::string::npos && front.find("(&f.") == std::string::npos &&
                            front.find("[](") == std::string::npos;
        if (!copies) { std::printf("FAIL: PluginUiPortImpl::setFrontEnd does not COPY the window's track feed (a pointer to the caller's object is a race with the pass)\n"); }
        CHECK(copies);
    }
}

// ----------------------------------------------------------------------------------------------------------------------------------
// THE TEST HOST (shell retirement R1). Tests reach the Engine behind a window through th::engine(window) and build their windows as
// th::TestWindow, both from tests/test_host.hpp; nothing else names rawEngine, and no test constructs an AppWindow by any other name.
// ----------------------------------------------------------------------------------------------------------------------------------
std::vector<fs::path> sourcesUnder(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) { continue; }
        const std::string ext = it->path().extension().string();
        if (ext == ".cpp" || ext == ".hpp" || ext == ".h") { out.push_back(it->path()); }
    }
    std::sort(out.begin(), out.end());
    return out;
}

void checkTheTestHost(const fs::path& root, bool rootGiven) {
    std::printf("the test host\n");
    const fs::path testsDir = root / "tests";
    std::error_code ec;
    if (rootGiven && !fs::exists(testsDir, ec)) {
        std::printf("  (probe copy without tests/: the test-host check is skipped)\n");
        return;
    }
    const std::string host = readFile(testsDir / "test_host.hpp");
    CHECK(!host.empty());

    // 1. rawEngine is named in code by exactly the allowed files (comments and strings do not count: they are blanked)
    std::vector<fs::path> all = sourcesUnder(root / "src");
    const std::vector<fs::path> tests = sourcesUnder(testsDir);
    all.insert(all.end(), tests.begin(), tests.end());
    int named = 0;
    int stray = 0;
    for (const fs::path& p : all) {
        bool hit = false;
        for (const std::string& l : codeLines(readFile(p))) {
            if (l.find("rawEngine") != std::string::npos) {
                hit = true;
                break;
            }
        }
        if (!hit) { continue; }
        ++named;
        const std::string rel = fs::relative(p, root).generic_string();
        bool allowed = false;
        for (const char* a : kRawEngineAllowed) { allowed = allowed || rel == a; }
        if (!allowed) {
            ++stray;
            std::printf("FAIL: %s names rawEngine - a test reaches the Engine through th::engine (tests/test_host.hpp), the one audited place\n", rel.c_str());
        }
    }
    std::printf("  rawEngine is named in code by %d files (budget %d: the shell's two and tests/test_host.hpp)\n", named, kRawEngineFilesBudget);
    CHECK(stray == 0);
    CHECK(named <= kRawEngineFilesBudget);
    // the host does name it, and through ONE friend of the window, which no other src/ file names
    CHECK(host.find("rawEngine") != std::string::npos);
    int friends = 0;
    int namingTheKey = 0;
    for (const fs::path& p : sourcesUnder(root / "src")) {
        for (const std::string& l : codeLines(readFile(p))) {
            if (l.find("TestHostAccess") != std::string::npos) {
                ++namingTheKey;
                if (l.find("friend struct TestHostAccess;") != std::string::npos) { ++friends; }
            }
        }
    }
    std::printf("  TestHostAccess: %d friend declaration(s), %d code line(s) naming it in src/ (expect 1 and 1)\n", friends, namingTheKey);
    CHECK(friends == 1);
    CHECK(namingTheKey == 1);

    // 2. no test constructs an AppWindow by its own name: every window a test builds is a th::TestWindow (R7 makes that type own the
    // Engine and its service; a bare AppWindow made anywhere else would then be a window with no engine behind it)
    static const std::regex decl(R"((^|[^\w:])(cascade::gui::|g::)?AppWindow\s+\w+\s*[;({=])");
    static const std::regex newed(R"(\bnew\s+(cascade::gui::|g::)?AppWindow\b)");
    static const std::regex made(R"((make_unique|make_shared|unique_ptr|shared_ptr)\s*<\s*(cascade::gui::|g::)?AppWindow\s*>)");
    int bare = 0;
    for (const fs::path& p : tests) {
        if (p.filename() == "test_host.hpp") { continue; }
        for (const std::string& l : codeLines(readFile(p))) {
            if (std::regex_search(l, newed) || std::regex_search(l, made)) {
                ++bare;
                std::printf("FAIL: %s builds an AppWindow by that name: %s\n", p.filename().string().c_str(), l.c_str());
                continue;
            }
            std::smatch m;
            if (std::regex_search(l, m, decl)) {
                const std::string before = l.substr(0, static_cast<std::size_t>(m.position(0)));
                const bool keyword = before.find("friend") != std::string::npos || before.find("class") != std::string::npos ||
                                     before.find("struct") != std::string::npos || before.find("using") != std::string::npos;
                if (!keyword) {
                    ++bare;
                    std::printf("FAIL: %s builds an AppWindow by that name: %s\n", p.filename().string().c_str(), l.c_str());
                }
            }
        }
    }
    CHECK(bare == 0);
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const fs::path root = (argc > 1) ? fs::path(argv[1]) : fs::path(CASCADE_SOURCE_DIR);
    std::printf("scanning %s\n", root.string().c_str());
    const Surface surface = readSurface(root);
    const std::vector<Call> calls = callsIn(root);
    std::printf("  %zu door calls found in src/gui\n", calls.size());
    std::set<std::string> allowedEngine;
    const std::set<std::string> deny = denyListOf(root, allowedEngine);

    checkSurface(root, surface, calls);
    checkWhoMayKnock(calls);
    checkTheFence(root, deny, allowedEngine);
    checkMachineryHasNoInput(root);
    checkTheFrameIsALook(root, calls);
    checkConfigSaveIsTheShells(root);
    checkIndexOps(root);
    checkRetiredHooks(root);
    checkPluginUiOwnership(root);
    checkDataHeaderSplit(root);
    checkThePort(root, deny, surface);
    checkTheTestHost(root, argc > 1);
    return testSummary("test_engine_shell_guard");
}
