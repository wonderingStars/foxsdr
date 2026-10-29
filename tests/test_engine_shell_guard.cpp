// test_engine_shell_guard.cpp - THE FENCE'S GUARD: what the compiler cannot hold about the window's contact with the engine
// (engine extraction stage 4, step 5; shell retirement R1 to R10; the records are docs/engine-stage4.md 6.25 and 6.26).
//
// THE SUCCESSOR OF tests/test_command_path_guard.cpp. That ratchet scanned the whole window for reads and writes of the engine's state
// (163 reads of 31 fields at its first count, 788 calls before the port; 33 of 141 when the pump move began; 13 fields and 26 reads when
// the fence stepped in) and was written to reach zero. It did not: it ended at a CLASSIFIED RESIDUE (the shell's seams, the host
// accessors, stage 5's web/CAT reads), and the fence did what a ratchet could not - the interface is its own target (cascade_interface),
// cmake/interface_fence.cmake puts stubs of every header it may not include first on its include path. Shell retirement (stage 7h, 2026-10-28 to
// 2026-10-29) then took the residue itself to zero and DELETED THE DOOR; this file is what is left of the door's guard: the door's surface check, its
// three budgets and their giant dated comments, its knock table and its holder checks are gone, and the survivors (the frame, the port, the plugin UI,
// the bounded hooks, the test host) and the new fence-to-zero checks are what keep it near its old length. What the compiler CANNOT see, held here:
//
//   0. THE DOOR IS DELETED (shell retirement R10, 2026-10-29): engine/engine_shell.hpp and .cpp do not exist, nothing includes them, the
//      cascade_shell target is not in CMake, `rawEngine`, `EngineShell`, `ShellHooks` and the window's `engine_` / `engineHolder_` are named
//      nowhere in code, and the window calls nothing on the engine by any name (the tombstone rows print their zeros). It was a header of 471 lines
//      at the survey, 274 as a holder with no methods, and a budget table of 41 methods, 3 host methods, 12 feed virtuals and 67 call sites.
//   1. THE PORT. The plugin UI port (core/plugin_ui_port.hpp, R4) is the window's one C++ contact with the engine that is not the table: 14 pure
//      virtuals and one extension id, each implemented by the engine's side and called by the window, in a header of data and pure virtuals
//      that includes nothing the fence denies. WHO MAY KNOCK: which window members reach the plugin UI through it (the host accessors, the
//      tile pump, the front end's hand-over, the shutdown's release).
//   2. THE FENCE REACHES ZERO. src/engine/ and src/net/ are denied WHOLE (no allow list; the guard reads the directories, so a new header is
//      denied too); the fence's stubs exist; no src/gui file includes a denied header; and the whole INCLUDE CLOSURE of src/gui - every header a
//      window file reaches through any other header - contains no denied one, no engine/ header and no net/ header. The twelve pure helpers that
//      were engine/'s (an allow list of 13 until R10) are core/ headers; the machinery whose data the interface was reaching (R11a) is denied.
//   3. THE SURVIVORS of the old guard, which were never about reads: the frame is a look (three stepPass calls in drawUi, in order; the
//      engine's pass started by the service); the window saves nothing; the ops that name a row by position are a reviewed set; the plugin UI
//      is the window's (the port applies the directive before any reference into it is taken); the data header split (the views take
//      core/plugin_ui_data.hpp); no ImGui input in the window's machinery; the bounded run's two hooks (core/bounded_hooks.hpp), which the host
//      installs in a `--frames` run and never in a product run.
//   4. THE TEST HOST (R1, R7, R10): tests reach the Engine behind a window through th::engine and build windows as th::TestWindow or
//      th::BareWindow, both th::HostedWindow (tests/test_host.hpp); `rawEngine` is named nowhere; the window has exactly one TestHostAccess
//      friend, which names no Engine; and no test constructs an AppWindow by that name.
//
// PROVEN RED (docs/engine-stage4.md 6.26): a 15th virtual in the port, a call from a widget, a stub-less deny list, `#include "engine/tune_control.hpp"` in a
// gui file, an engine header put back on an allow list, the door's header recreated; and the compiler's half by tests/CMakeLists.txt's
// interface_fence_* probes, which compile an interface translation unit that includes a denied header and want the build to fail.
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
// BUDGETS. Every number is the surface as shell retirement left it; a change moves the number in the same commit, with the dated comment that says
// why. They only went down: the door's budgets (kShellMethodsBudget 41 at the survey, kHostMethodsBudget 3, kFeedMethodsBudget 12, kCallSitesBudget 67,
// kAllowedEngineHeadersBudget 13) are ZERO and are held as the tombstone rows below, not as constants.
// ----------------------------------------------------------------------------------------------------------------------------------
constexpr int kPortMethodsBudget = 14;    // PluginUiPort's pure virtuals (the destructor apart): the seven host accessors (tracks, unplacedTracks, paths, panels, instruments, trackPluginNames, altitudeNear), poll, pollImages, applyNewestDirective, setFrontEnd, serveTiles, release, disconnect. 14 at R4: HostPluginFeed's 12 minus `report` (the plugin half of the status report rides setFrontEnd once) plus setFrontEnd (was the door's setFrontEndFacts), serveTiles (was the door's serveWebTiles) and release (the window's own shutdown release)
constexpr int kPortCallSitesBudget = 15;  // `ui_->x` in src/gui: 15 at R4 - the 13 sites `pluginFeed_->x` had, minus `report` (run: 1) plus setFrontEnd (handFrontEndFacts: 1), serveTiles (pumpWebTiles: 1) and release (run: 1); the extension's own call, `get_local_extension`, is a table call (attachPluginUi) and is held by checkThePort
constexpr int kExtensionIdsBudget = 1;    // FOXAPI_EXT_* in the vendored header: FOXAPI_EXT_PLUGIN_UI only. A second in-process extension is a decision (it reaches the engine's process by C++ objects), not a thing that happens
// THE DOOR'S ROWS, AT THEIR EPITAPH (shell retirement R10, 2026-10-29): what the window may call on the engine by `engine_.x` is NOTHING and the allow list of engine/ headers is
// EMPTY. They are constants only so that the tombstone rows print them; a number here that is not zero is a door that came back.
constexpr int kDoorCallSitesBudget = 0;
constexpr int kAllowedEngineHeadersBudget = 0;
constexpr int kRawEngineFilesBudget = 0;

// ----------------------------------------------------------------------------------------------------------------------------------
// WHO MAY KNOCK: for each AppWindow member, the port methods it may call. EVERY ENTRY MUST BE MATCHED by a call (a stale entry fails), so this is
// exactly the set of calls the window makes through the port. (There was a table of the DOOR's knocks beside it, kEngineKnocks, empty since R9 and
// deleted with the door in R10: a call `engine_.x` is outside every table now, which is a failure by construction.)
// ----------------------------------------------------------------------------------------------------------------------------------
struct Knock {
    const char* member;
    std::vector<const char*> methods;
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
    {"AppWindow::leavePluginUi", {"disconnect"}},             // the one disconnect: the prelude's and ~AppWindow's net (R8; was ~AppWindow's own)
    {"AppWindow::run", {"release"}},                          // the window's own release of the plugin UI before the engine's - R8's prelude, before run() returns
    {"AppWindow::handFrontEndFacts", {"setFrontEnd"}},        // the face, the basemap's figures, the track feed; and the bounded run's plugin report half (R4)
    {"AppWindow::pumpWebTiles", {"serveTiles"}},              // the browser map's tiles, drained and answered on the window's thread (R4; was serveWebTiles)
};

// The window's machinery: the members that drive the port or answer for the host. Rule: no ImGui input in them.
const char* const kWindowMachinery[] = {
    "AppWindow::AppWindow",       "AppWindow::~AppWindow",       "AppWindow::applyWindowHalf",  "AppWindow::flushInterfaceHalf",
    "AppWindow::handFrontEndFacts", "AppWindow::wallTimeS",      "AppWindow::stepPass",         "AppWindow::endOfFrameHook",
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
                                 // src/engine, denied WHOLE since R10: the vocabulary only the engine speaks, the service, the harness, the process boot
                                 "engine/engine_types.hpp",  "engine/engine_service.hpp", "engine/host_measure.hpp", "engine/process_boot.hpp",
                                 // the machinery whose data the interface was reaching, hoisted by R11a
                                 "core/plugin_host.hpp",     "core/plugin_repo.hpp",   "core/freq_manager.hpp",     "core/mp3_writer.hpp",
                                 "core/recorder.hpp",        "core/transmitter.hpp",   "core/gps_reader.hpp",       "core/scanner.hpp",
                                 // src/net, denied whole by stage 5f (the servers are the engine's)
                                 "net/web_server.hpp",       "net/cat_server.hpp",     "net/web_session.hpp",       "net/status_compose.hpp"};
// The twelve pure helpers that were src/engine's (the allow list of 13, with the door) until shell retirement R10 moved them: they live in core/ and nowhere else.
const char* const kMovedToCore[] = {"tune_control.hpp",     "bias_tee.hpp",       "soundcard_panel.hpp",     "audio_open.hpp",
                                    "device_scan_plan.hpp", "receiver_tables.hpp", "source_fallback.hpp",    "add_all_plan.hpp",
                                    "running_view.hpp",     "rate_follow_status.hpp", "plugin_store_reasons.hpp", "tx_frequency.hpp"};
// The data headers shell retirement made: the vocabulary split out of the door (R10) and the hoists of the ledger (R11a). The interface may include each; none may include a denied header.
const char* const kDataHeaders[] = {"core/engine_vocab.hpp",     "core/status_text.hpp",       "core/pass_part.hpp",       "core/plugin_ui_port.hpp",
                                    "core/bounded_hooks.hpp",    "core/plugin_host_data.hpp",  "core/plugin_repo_data.hpp", "core/bookmark.hpp",
                                    "core/mp3_support.hpp"};

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

std::vector<fs::path> sourcesUnder(const fs::path& dir);   // (defined with the test host's checks below)

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

// --- the plugin UI port's header ------------------------------------------------------------------------------------------------

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
    std::vector<Decl> port;
    std::string portText;  // core/plugin_ui_port.hpp, whole
};

Surface readSurface(const fs::path& root) {
    Surface s;
    // THE PORT: core/plugin_ui_port.hpp's class body. (The door's header, engine/engine_shell.hpp, was read here until R10 deleted it.)
    s.portText = readFile(root / "src" / "core" / "plugin_ui_port.hpp");
    CHECK(!s.portText.empty());
    const std::size_t portAt = s.portText.find("class PluginUiPort {");
    CHECK(portAt != std::string::npos);
    const std::size_t portEnd = s.portText.find("\n};", portAt);
    CHECK(portEnd != std::string::npos);
    s.port = declarationsIn(codeLines(s.portText.substr(portAt + std::strlen("class PluginUiPort {"), portEnd - (portAt + std::strlen("class PluginUiPort {")))));
    return s;
}

// Names of every port method (the destructor apart).
std::set<std::string> namesOf(const std::vector<Decl>& v) {
    std::set<std::string> out;
    for (const Decl& d : v) {
        if (d.name == "~PluginUiPort") { continue; }
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

// The fence's deny list, as the fence file builds it: every `cascade_interface_deny(<header> "<why>")` line, plus EVERY header of src/engine and of src/net (both are
// denied whole, by a directory glob, with no allow list: the guard reads the directories, so a header added there is denied here too).
std::set<std::string> denyListOf(const fs::path& root) {
    std::set<std::string> deny;
    const std::string text = readFile(root / "cmake" / "interface_fence.cmake");
    CHECK(!text.empty());
    // cascade_interface_deny(<header> "<why>")
    const std::regex d("cascade_interface_deny\\(\\s*([A-Za-z0-9_/\\.]+)\\s+\"");
    for (std::sregex_iterator it(text.begin(), text.end(), d), end; it != end; ++it) {
        const std::string h = (*it)[1].str();
        if (h.find("${") == std::string::npos) { deny.insert(h); }
    }
    std::error_code ec;
    for (const char* dir : {"engine", "net"}) {
        for (const auto& e : fs::directory_iterator(root / "src" / dir, ec)) {
            const std::string ext = e.path().extension().string();
            if (ext != ".hpp" && ext != ".h") { continue; }
            deny.insert(std::string(dir) + "/" + e.path().filename().string());
        }
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

// THE INCLUDE CLOSURE of src/gui: every header a window file reaches through quoted includes, followed through every header it reaches (an include is
// resolved against src/ and then against the including file's own directory; <system> includes and generated headers resolve to nothing). The
// compiler fence sees only the headers some interface translation unit happens to compile; this is the whole closure. Returns the set of src-relative paths.
std::set<std::string> guiClosure(const fs::path& root) {
    std::set<std::string> seen;
    std::vector<std::string> todo;
    for (const fs::path& p : guiFiles(root, true)) { todo.push_back("gui/" + p.filename().string()); }
    while (!todo.empty()) {
        const std::string h = todo.back();
        todo.pop_back();
        if (!seen.insert(h).second) { continue; }
        const fs::path file = root / "src" / h;
        std::error_code ec;
        if (!fs::exists(file, ec)) { continue; }
        const std::string dir = h.find('/') == std::string::npos ? std::string() : h.substr(0, h.rfind('/') + 1);
        for (const std::string& inc : includesOf(readFile(file))) {
            if (fs::exists(root / "src" / inc, ec)) {
                todo.push_back(inc);
            } else if (fs::exists(root / "src" / (dir + inc), ec)) {
                todo.push_back(dir + inc);
            }
        }
    }
    return seen;
}

// ---------------------------------------------------------------------------------------------------------------------------------

void checkSurface(const fs::path& root, const Surface& s, const std::vector<Call>& calls) {
    const auto po = namesOf(s.port);
    std::printf("  the port's surface: %zu virtuals (budget %d); the door's: engine_shell.hpp deleted (R10), no host or web/CAT class\n", po.size(), kPortMethodsBudget);
    if (static_cast<int>(po.size()) != kPortMethodsBudget) {
        std::printf("FAIL: core/plugin_ui_port.hpp has %zu virtuals, the budget is %d - a new method on the port is a decision: say why in the header's dated comment and "
                    "move the budget in this file in the same change (or port the read the house way: a list, a fact or an op of the table)\n",
                    po.size(), kPortMethodsBudget);
    }
    CHECK(static_cast<int>(po.size()) <= kPortMethodsBudget);
    CHECK(static_cast<int>(po.size()) >= 8);   // the scan saw the real header
    CHECK(static_cast<int>(po.size()) == kPortMethodsBudget);   // ...and a budget above what is there is a stale budget

    // Every port virtual is PURE and implemented by the engine's side, src/engine/plugin_ui_port.cpp (PluginUiPortImpl::x).
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

    // Every port method is CALLED from src/gui (a method nobody calls is a surface nobody needs: retire it), and the window calls nothing the port does not declare.
    std::set<std::string> portCalled;
    for (const Call& c : calls) {
        if (c.object == "ui_") { portCalled.insert(c.method); }
    }
    for (const std::string& n : po) {
        if (portCalled.count(n) == 0) { std::printf("FAIL: PluginUiPort::%s is declared and the window never calls it - retire it\n", n.c_str()); }
        CHECK(portCalled.count(n) != 0);
    }
    for (const Call& c : calls) {
        if (c.object == "ui_" && po.count(c.method) == 0) {
            std::printf("FAIL: %s:%d calls ui_->%s, which core/plugin_ui_port.hpp does not declare\n", c.file.c_str(), c.line, c.method.c_str());
            CHECK(false);
        }
    }
}

void checkWhoMayKnock(const std::vector<Call>& calls) {
    std::set<std::string> matched;
    int doorSites = 0, portSites = 0, bad = 0;
    std::set<std::string> members;
    for (const Call& c : calls) {
        if (c.object == "engine_") { ++doorSites; } else { ++portSites; }
        bool ok = false;
        if (c.object == "ui_") {
            for (const Knock& k : kPortKnocks) {
                if (c.member != k.member) { continue; }
                for (const char* m : k.methods) {
                    if (c.method == m) {
                        ok = true;
                        matched.insert(k.member + std::string("|") + m);
                    }
                }
            }
        }
        members.insert(c.member);
        if (!ok) {
            ++bad;
            std::printf("FAIL: %s:%d: %s calls %s%s%s - the window calls NOTHING on the engine (the door was deleted, R10); only the plugin UI's accessors, the "
                        "frame's hand-over, the tile pump and the shutdown go through the port (kPortKnocks)\n",
                        c.file.c_str(), c.line, c.member.c_str(), c.object.c_str(), c.object == "engine_" ? "." : "->", c.method.c_str());
        }
    }
    std::printf("  who knocks: %d door call sites (budget %d - the tombstone) and %d port call sites in %zu window members, %d outside the table (budget %d port sites)\n",
                doorSites, kDoorCallSitesBudget, portSites, members.size(), bad, kPortCallSitesBudget);
    CHECK(bad == 0);
    CHECK(doorSites == kDoorCallSitesBudget);
    CHECK(portSites == kPortCallSitesBudget);
    CHECK(portSites >= 9);  // the scan saw the real files
    // Every table entry is matched by a call.
    for (const Knock& k : kPortKnocks) {
        for (const char* m : k.methods) {
            const bool ok = matched.count(k.member + std::string("|") + m) != 0;
            if (!ok) { std::printf("FAIL: kPortKnocks {%s, %s} matches no call - remove it\n", k.member, m); }
            CHECK(ok);
        }
    }
}

void checkTheFence(const fs::path& root, const std::set<std::string>& deny) {
    // the deny list holds what it must
    for (const char* h : kMustDeny) {
        const bool ok = deny.count(h) != 0;
        if (!ok) { std::printf("FAIL: the fence no longer denies <%s> to the interface (cmake/interface_fence.cmake)\n", h); }
        CHECK(ok);
    }
    const std::string fenceText = readFile(root / "cmake" / "interface_fence.cmake");
    // src/engine and src/net are denied WHOLE by the fence file, with no allow list and no way to take a header back out of the deny list (the guard reads the
    // directories for the deny set above; this holds that the file really does the same). The fence file's CODE (its comments tell of the dead list).
    {
        std::string code;
        for (const std::string& l : codeLines(fenceText)) { code += l + "\n"; }
        // (codeLines blanks `#` comments? no: it blanks C++ comments, and a cmake comment is `#`: drop those lines here)
        std::string cmakeCode;
        std::istringstream in(fenceText);
        for (std::string l; std::getline(in, l);) {
            const std::size_t first = l.find_first_not_of(" \t");
            if (first == std::string::npos || l[first] == '#') { continue; }
            cmakeCode += l + "\n";
        }
        const bool netGlob = cmakeCode.find("src/net/*.hpp") != std::string::npos && cmakeCode.find("foreach(_h IN LISTS _cascade_net_headers)") != std::string::npos;
        const bool engineGlob = cmakeCode.find("src/engine/*.hpp") != std::string::npos && cmakeCode.find("foreach(_h IN LISTS _cascade_engine_headers)") != std::string::npos;
        const bool noTakeBack = cmakeCode.find("REMOVE_ITEM CASCADE_INTERFACE_DENIED") == std::string::npos && cmakeCode.find("ALLOWED_NET") == std::string::npos;
        const bool noAllowList = cmakeCode.find("ALLOWED_ENGINE") == std::string::npos && cmakeCode.find("list(FIND CASCADE_INTERFACE_ALLOWED") == std::string::npos;
        if (!netGlob) { std::printf("FAIL: cmake/interface_fence.cmake no longer denies every header of src/net (stage 5f)\n"); }
        if (!engineGlob) { std::printf("FAIL: cmake/interface_fence.cmake no longer denies every header of src/engine (shell retirement R10)\n"); }
        if (!noTakeBack) { std::printf("FAIL: cmake/interface_fence.cmake takes a header back out of the deny list or has a net/ allow list\n"); }
        if (!noAllowList) { std::printf("FAIL: cmake/interface_fence.cmake has an engine/ allow list again - src/engine is denied WHOLE (R10); a pure helper belongs in core/\n"); }
        CHECK(netGlob);
        CHECK(engineGlob);
        CHECK(noTakeBack);
        CHECK(noAllowList);
    }
    // THE ALLOWED ENGINE HEADERS: every header of src/engine is on the deny list, so the allow side is empty (budget 0; it was 13 until R10)
    {
        std::size_t engineHeaders = 0, engineDenied = 0;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(root / "src" / "engine", ec)) {
            const std::string ext = e.path().extension().string();
            if (ext != ".hpp" && ext != ".h") { continue; }
            ++engineHeaders;
            if (deny.count("engine/" + e.path().filename().string()) != 0) { ++engineDenied; }
        }
        const std::size_t allowed = engineHeaders - engineDenied;
        std::printf("  the fence: %zu headers denied; %zu of %zu engine/ headers allowed (budget %d - it was 13 until R10)\n", deny.size(), allowed, engineHeaders,
                    kAllowedEngineHeadersBudget);
        CHECK(engineHeaders >= 10);   // the scan saw the real directory
        CHECK(allowed == static_cast<std::size_t>(kAllowedEngineHeadersBudget));
    }

    // the interface target is built, from src/gui, behind the fence; it links the engine LIBRARY (core/'s objects) and no shell; the headless programs refuse it
    const std::string cmake = readFile(root / "CMakeLists.txt");
    CHECK(!cmake.empty());
    const bool target = cmake.find("add_library(cascade_interface STATIC ${CASCADE_GUI})") != std::string::npos;
    const bool fenced = cmake.find("cascade_apply_interface_fence(cascade_interface)") != std::string::npos;
    const bool links = cmake.find("    OpenGL::GL\n    cascade_engine)") != std::string::npos &&
                       cmake.find("target_link_libraries(cascade_gui INTERFACE cascade_interface cascade_engine)") != std::string::npos;
    const bool refused = cmake.find("^cascade_gui$|^cascade_interface$\")") != std::string::npos;
    const bool headless = cmake.find("set(_todo cascade_engine cascade_engine_link_check foxsdr-reports foxsdr-engine)") != std::string::npos;
    if (!target || !fenced) { std::printf("FAIL: CMakeLists.txt no longer builds the window as cascade_interface behind the fence\n"); }
    if (!links) { std::printf("FAIL: cascade_interface / cascade_gui no longer link the engine library directly (the shell target is gone: they link cascade_engine)\n"); }
    if (!refused || !headless) { std::printf("FAIL: the engine-side link check no longer refuses cascade_interface / cascade_gui for the headless programs\n"); }
    CHECK(target);
    CHECK(fenced);
    CHECK(links);
    CHECK(refused);
    CHECK(headless);
    // the compile probes exist (tests/CMakeLists.txt registers them)
    const std::string tcmake = readFile(root / "tests" / "CMakeLists.txt");
    const bool probes = tcmake.find("interface_fence_denies_") != std::string::npos && tcmake.find("interface_fence_allows") != std::string::npos;
    if (!probes) { std::printf("FAIL: tests/CMakeLists.txt registers no interface_fence_* compile probes\n"); }
    CHECK(probes);

    // NO FILE UNDER src/gui NAMES A DENIED HEADER - the compiler sees only the headers some interface translation unit happens to include.
    int named = 0, engineNamed = 0;
    for (const fs::path& p : guiFiles(root, true)) {
        for (const std::string& inc : includesOf(readFile(p))) {
            if (deny.count(inc) != 0) {
                ++named;
                std::printf("FAIL: src/gui/%s includes <%s>, which the fence denies the interface\n", p.filename().string().c_str(), inc.c_str());
            }
            if (inc.rfind("engine/", 0) == 0) { ++engineNamed; }
        }
    }
    CHECK(named == 0);
    std::printf("  src/gui includes %d engine/ headers (budget 0)\n", engineNamed);
    CHECK(engineNamed == 0);

    // THE INCLUDE CLOSURE OF src/gui. Everything a window file reaches through any header: none denied, none under engine/ or net/. (The sibling-hole check of the
    // allow list - an allowed header that itself includes a denied one - is this check, for every header the interface reaches, allowed by a list or not.)
    {
        const std::set<std::string> closure = guiClosure(root);
        std::map<std::string, int> byDir;
        int denied = 0;
        for (const std::string& h : closure) {
            const std::size_t slash = h.find('/');
            if (slash == std::string::npos) { continue; }
            ++byDir[h.substr(0, slash)];
            if (deny.count(h) != 0 || h.rfind("engine/", 0) == 0 || h.rfind("net/", 0) == 0) {
                ++denied;
                std::printf("FAIL: the interface's include closure reaches <%s>, which the fence denies (through a header the interface includes)\n", h.c_str());
            }
        }
        std::printf("  the interface's include closure: %zu files (", closure.size());
        for (const auto& kv : byDir) { std::printf("%s %d, ", kv.first.c_str(), kv.second); }
        std::printf("engine %d, net %d): %d denied (expect 0)\n", byDir.count("engine") != 0 ? byDir["engine"] : 0, byDir.count("net") != 0 ? byDir["net"] : 0, denied);
        CHECK(denied == 0);
        CHECK(closure.size() > 200);   // the walk saw the real tree
        // every data header shell retirement made is reached or at least exists, is not denied and includes nothing denied
        for (const char* h : kDataHeaders) {
            CHECK(deny.count(h) == 0);
            const std::string t = readFile(root / "src" / h);
            if (t.empty()) { std::printf("FAIL: %s is missing\n", h); }
            CHECK(!t.empty());
            for (const std::string& inc : includesOf(t)) {
                if (deny.count(inc) != 0 || inc.rfind("engine/", 0) == 0) {
                    std::printf("FAIL: %s includes \"%s\" - a data header the interface may include takes nothing denied\n", h, inc.c_str());
                    CHECK(false);
                }
            }
        }
    }

    // THE TWELVE PURE HELPERS live in core/ and nowhere in engine/ (R10's moves; an allow list of 13 held them until then)
    {
        int bad = 0;
        for (const char* n : kMovedToCore) {
            std::error_code ec;
            const bool inCore = fs::exists(root / "src" / "core" / n, ec);
            const bool inEngine = fs::exists(root / "src" / "engine" / n, ec);
            if (!inCore || inEngine) {
                ++bad;
                std::printf("FAIL: %s must be src/core/%s and not src/engine/%s (shell retirement R10)\n", n, n, n);
            }
        }
        CHECK(bad == 0);
    }

    // THE DOOR DOES NOT EXIST. The header, its implementation and the target are gone, and nothing in src/, tests/ or the CMake files includes, links or names them
    // in code (comments tell the history; this file names them in strings to hold them dead).
    {
        std::error_code ec;
        const bool gone = !fs::exists(root / "src" / "engine" / "engine_shell.hpp", ec) && !fs::exists(root / "src" / "engine" / "engine_shell.cpp", ec);
        if (!gone) { std::printf("FAIL: engine/engine_shell.hpp or .cpp exists again - the door was deleted by shell retirement R10\n"); }
        CHECK(gone);
        std::vector<fs::path> all = sourcesUnder(root / "src");
        const std::vector<fs::path> tests = sourcesUnder(root / "tests");
        all.insert(all.end(), tests.begin(), tests.end());
        int doorNames = 0;
        for (const fs::path& p : all) {
            if (p.filename() == "test_engine_shell_guard.cpp") { continue; }
            for (const std::string& l : codeLines(readFile(p))) {
                for (const char* n : {"engine_shell", "EngineShell"}) {
                    if (l.find(n) != std::string::npos) { ++doorNames; }
                }
            }
            for (const std::string& inc : includesOf(readFile(p))) {
                if (inc.find("engine_shell") != std::string::npos) { ++doorNames; }
            }
        }
        std::printf("  the door: engine_shell.{hpp,cpp} %s; engine_shell / EngineShell named in code or includes: %d (expect 0)\n", gone ? "deleted" : "BACK",
                    doorNames);
        CHECK(doorNames == 0);
        // CMake: no cascade_shell target or link, no engine_shell source (comment lines of the files tell the history)
        int cmakeNames = 0;
        for (const fs::path& f : {root / "CMakeLists.txt", root / "tests" / "CMakeLists.txt", root / "cmake" / "interface_fence.cmake"}) {
            std::istringstream in(readFile(f));
            for (std::string l; std::getline(in, l);) {
                const std::size_t first = l.find_first_not_of(" \t");
                if (first == std::string::npos || l[first] == '#') { continue; }
                if (l.find("engine_shell") != std::string::npos) { ++cmakeNames; }
                if (l.find("cascade_shell") != std::string::npos && l.find("^cascade_shell$") == std::string::npos) { ++cmakeNames; }
            }
        }
        std::printf("  the shell target: named in CMake code %d time(s) (expect 0)\n", cmakeNames);
        CHECK(cmakeNames == 0);
    }

    // THE BACK DOORS. rawEngine returned the Engine and engineHolder_ held the shell's view of it; naming either from src/gui is the one way left to hold an engine
    // (they do not exist; this holds it for the window's CODE).
    int back = 0;
    for (const fs::path& p : guiFiles(root, true)) {
        for (const std::string& l : codeLines(readFile(p))) {
            if (l.find("rawEngine") != std::string::npos || l.find("engineHolder_") != std::string::npos) {
                ++back;
                std::printf("FAIL: src/gui/%s names rawEngine or engineHolder_ - the window holds no engine\n", p.filename().string().c_str());
            }
        }
    }
    CHECK(back == 0);
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
//   * stepPass (the window's own, which runs the bounded run's hook: shell retirement R9) appears exactly three times, all in drawUi, as Front, Mute and Back, in that order, and the window does NOT start the engine's
//     pass (shell retirement R8): the service's start makes a free-running run's first publish (controlPass) and starts the thread
//     (startControlThread), main.cpp calls it before run(), and the window's run() names neither;
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
    // THE THREE PARTS OF THE PASS are the window's own stepPass since shell retirement R9 (it runs the bounded run's hook, when the host installed one, under
    // the window's watchdog pause): its calls are counted as text over the CODE of src/gui. The window starts no pass, kicks none and calls nothing on the engine
    // (the door's rows - frameKick, controlPass, startControlThread, engine_ calls - are the tombstone: zero, and the global door-site count says so).
    int stepCalls = 0, controlPassCalls = 0, startCalls = 0, engineCalls = 0;
    for (const Call& c : calls) {
        if (c.object != "engine_") { continue; }
        ++engineCalls;
        if (c.method == "controlPass") { ++controlPassCalls; }
        if (c.method == "startControlThread") { ++startCalls; }
    }
    for (const fs::path& p : guiFiles(root, true)) {
        for (const std::string& l : codeLines(readFile(p))) {
            std::size_t at = 0;
            while ((at = l.find("stepPass(cascade::core::PassPart::", at)) != std::string::npos) {
                ++stepCalls;
                at += 1;
            }
        }
    }
    std::printf("  the frame is a look: stepPass %d (expect 3), controlPass %d (expect 0), startControlThread %d (expect 0), engine_ calls %d (expect 0)\n", stepCalls,
                controlPassCalls, startCalls, engineCalls);
    CHECK(stepCalls == 3);
    CHECK(controlPassCalls == 0);   // THE ENGINE'S PASS IS THE SERVICE'S TO START (shell retirement R8): the window starts nothing - checked below
    CHECK(startCalls == 0);
    CHECK(engineCalls == 0);
    const std::string text = readFile(root / "src" / "gui" / "app_window.cpp");
    const std::size_t draw = text.find("void AppWindow::drawUi() {");
    CHECK(draw != std::string::npos);
    std::size_t drawEnd = text.find("\n}\n", draw);
    CHECK(drawEnd != std::string::npos);
    const std::string body = text.substr(draw, drawEnd - draw);
    const auto at = [&](const char* needle) { return body.find(needle); };
    const std::size_t front = at("stepPass(cascade::core::PassPart::Front);");
    const std::size_t poll = at("pollUiAnswers();");
    const std::size_t status = at("refreshStatusLines();");
    const std::size_t look = at("refreshUiState();");
    const std::size_t act = at("actOnUiAnswers();");
    const std::size_t mute = at("stepPass(cascade::core::PassPart::Mute);");
    const std::size_t key = at("submitTransmitKey();");
    const std::size_t back = at("stepPass(cascade::core::PassPart::Back);");
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
        // (R8: the directory the control thread's watchdog moves to when the half later switches Diagnostics on is named by the SERVICE when it starts
        // the thread - from the options main.cpp built - and the frame loop sends the half once a frame.)
        const std::string service = readFile(root / "src" / "engine" / "engine_service.cpp");
        const std::size_t dirAt = service.find("opt.diagnosticsDir = options_.diagnosticsDir;");
        const std::size_t threadAt = service.find("engine_.startControlThread(opt);");
        const std::size_t runAt = text.find("int AppWindow::run(int frames) {");
        const std::size_t sendAt = runAt == std::string::npos ? std::string::npos : text.find("(void)sendInterfaceHalf(glfwGetTime());", runAt);
        const bool sent = dirAt != std::string::npos && threadAt != std::string::npos && dirAt < threadAt && sendAt != std::string::npos &&
                          text.find("sendInterfaceHalf(glfwGetTime())") == sendAt + 6 && text.find("sendInterfaceHalf(glfwGetTime())", sendAt + 7) == std::string::npos;
        if (!sent) {
            std::printf("FAIL: the service does not name the diagnostics directory before it starts the pass, or run() does not send the interface's half once a frame\n");
        }
        CHECK(sent);
    }
    // run() starts nothing (R8). The first publish, then the thread, then the keeper are the service's start, in that order; main.cpp calls the start
    // after the window's constructor and before run() (tests/test_engine_service_order.cpp holds that order).
    const std::size_t run = text.find("int AppWindow::run(int frames) {");
    CHECK(run != std::string::npos);
    // (0.99.67: the loop also ends when the console the program started on asks for the close - core/console_close.hpp.)
    const std::size_t loop = text.find("while (!glfwWindowShouldClose(window) && !closeRequested_ && !cascade::core::consoleCloseRequested()) {", run);
    {
        const std::string service = readFile(root / "src" / "engine" / "engine_service.cpp");
        const std::size_t startAt = service.find("bool EngineService::start(");
        const std::size_t firstPass = service.find("engine_.controlPass();", startAt);
        const std::size_t startThread = service.find("engine_.startControlThread(opt);", startAt);
        const bool startOk = startAt != std::string::npos && firstPass != std::string::npos && startThread != std::string::npos && loop != std::string::npos &&
                             firstPass < startThread && text.find("engine_.controlPass(") == std::string::npos && text.find("engine_.startControlThread(") == std::string::npos;
        if (!startOk) { std::printf("FAIL: the service's start does not make the first publish and then start the thread, or the window still starts the engine's pass\n"); }
        CHECK(startOk);
    }
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

// THE WINDOW SAVES NOTHING (shell retirement R8; was: the config save is the shell's, not the frame's). The settings file is kept by the engine's service
// - a thread of its own, from the snapshot the engine holds - and the window's only contact with it is the interface's half it hands over (sendInterfaceHalf
// each look, flushInterfaceHalf as the frame loop ends). The window's own copy of the keeper - the writer and its back-off, the saved and pending copies, the
// debounce, the field-by-field compare (configsEqual, 240 lines), currentConfig, saveConfigNow - was the dead copy the survey calls it; R8a left it compiled and
// unused and R8b deleted it. This holds that it stays deleted: none of its names is in the CODE of any file of src/gui, and run() hands the last half over before it
// releases the plugin UI.
void checkTheWindowSavesNothing(const fs::path& root) {
    int found = 0;
    for (const fs::path& p : guiFiles(root, false)) {
        const std::string text = readFile(p);
        for (const char* needle : {"configWriter_", "savedCfg_", "pendingCfg_", "lastRequestedConfig_", "lastChangeTimeS_", "maybeSaveConfig", "saveConfigNow",
                                   "requestConfigSave", "pollConfigWriter", "configsEqual", "currentConfig(", "noteConfigWrite", "configRetryHeld", "ConfigWriter"}) {
            std::size_t at = 0;
            while ((at = text.find(needle, at)) != std::string::npos) {
                const std::size_t lineStart = text.rfind('\n', at) == std::string::npos ? 0 : text.rfind('\n', at) + 1;
                const std::string line = text.substr(lineStart, text.find('\n', at) - lineStart);
                const bool comment = line.find("//") != std::string::npos && line.find("//") < line.find(needle);
                if (!comment) {
                    ++found;
                    std::printf("  the window's keeper is back: %s names %s\n", p.filename().string().c_str(), needle);
                }
                at += 1;
            }
        }
    }
    std::printf("  the window saves nothing: %d name(s) of its deleted keeper in code (expect 0)\n", found);
    CHECK(found == 0);
    const std::string text = readFile(root / "src" / "gui" / "app_window.cpp");
    const std::size_t run = text.find("int AppWindow::run(int frames) {");
    const std::size_t runEnd = run == std::string::npos ? std::string::npos : text.find("\n}\n", run);
    CHECK(run != std::string::npos && runEnd != std::string::npos);
    const std::string body = (run == std::string::npos || runEnd == std::string::npos) ? std::string() : text.substr(run, runEnd - run);
    const std::size_t flush = body.find("(void)flushInterfaceHalf();");
    const std::size_t release = body.find("ui_->release();");
    CHECK(flush != std::string::npos && release != std::string::npos && flush < release);   // the last half goes before the plugin UI is released
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
    for (const char* rel : {"src/engine/engine_host.hpp", "src/engine/engine_host.cpp"}) {
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
    // THE WINDOW OVERRIDES NOTHING (shell retirement R9): ShellHooks - the three questions the engine asked of its front end - is gone with the class
    // that carried it, and so are the shell's adapter and Engine::setHost / resetHost; the names are held out of the CODE of src/ (comments tell of them).
    int overrides = 0;
    for (const std::string& l : codeLines(readFile(root / "src" / "gui" / "app_window.hpp"))) {
        if (l.find(") const override;") != std::string::npos || l.find(") override;") != std::string::npos) { ++overrides; }
    }
    int shellGear = 0;
    for (const fs::path& p : sourcesUnder(root / "src")) {
        for (const std::string& l : codeLines(readFile(p))) {
            // (ShellHooks as a WHOLE WORD: the window's `watchdogShellHooks` and gui::ShellPauseHooks are another thing, the native modal waits' user-paced pause)
            static const std::regex shellHooksWord(R"(\bShellHooks\b)");
            if (std::regex_search(l, shellHooksWord)) {
                ++shellGear;
                std::printf("FAIL: %s names ShellHooks - gone with shell retirement R9: %s\n", p.filename().string().c_str(), l.c_str());
            }
            for (const char* name : {"ShellHostAdapter", "setHost(", "resetHost(", "setFrontEndWaitBeat", "frontEndWaitBeat_", "seamMeasureTick",
                                     "seamMarkPassStats", "seamPassStatsJson", "seamInstallSource"}) {
                if (l.find(name) != std::string::npos) {
                    ++shellGear;
                    std::printf("FAIL: %s names %s - gone with shell retirement R9: %s\n", p.filename().string().c_str(), name, l.c_str());
                }
            }
        }
    }
    std::printf("  the retired host hooks (%zu) are still gone: %d reappeared; the window overrides %d (expect 0); the R9 gear (ShellHooks, the adapter, setHost, the wait "
                "beat, the measurement seams) named in code: %d (expect 0)\n",
                sizeof(kRetiredHooks) / sizeof(kRetiredHooks[0]), found, overrides, shellGear);
    CHECK(found == 0);
    CHECK(overrides == 0);
    CHECK(shellGear == 0);
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
            if (l.find("pluginUi_") != std::string::npos) {
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
        const std::size_t firstEngine = text.find("stepPass(cascade::core::PassPart::Front);", draw);   // (the frame's first contact with the engine; engine_. is gone, R9)
        CHECK(applyInDraw != std::string::npos && firstEngine != std::string::npos);
        const bool drawOk = applyInDraw < firstEngine && applyInDraw - draw < 2500;
        if (!drawOk) { std::printf("FAIL: drawUi does not apply the plugin directive before its first contact with the engine\n"); }
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
    // core/receiver_tables.hpp checks the window's buttons against) stay as light.
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
    // of them (checkTheFence, over the deny list), and no header the interface reaches - the hoisted data headers, the twelve helpers
    // that moved from engine/ to core/ (R10) - includes one either (checkTheFence's include closure). The compiler holds the same by the interface_fence_denies_net_* probes.
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
        // the one header that used to reach net/ (receiver_tables.hpp, for the mode names; core/ since R10) takes them from the data header
        const std::string rt = readFile(root / "src" / "core" / "receiver_tables.hpp");
        CHECK(!rt.empty());
        const bool rtClean = rt.find("#include \"net/") == std::string::npos && rt.find("#include \"core/demod_names.hpp\"") != std::string::npos;
        if (!rtClean) { std::printf("FAIL: core/receiver_tables.hpp must take the mode names from core/demod_names.hpp and include nothing of net/\n"); }
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
        // SINCE R8 the plugin UI's disconnect is ONE function, leavePluginUi, which run()'s prelude calls right after the port's release and ~AppWindow calls
        // as its last act - the net for every path that never reached the prelude (a window that could not be created, a constructor's sibling that threw, the
        // software-GL relaunch, a test window that never ran). The engine's own teardown is the host's service's and ~Engine's (not the window's).
        const std::string dtor = bodyAfter(text, "AppWindow::~AppWindow() {");
        CHECK(!dtor.empty());
        const std::size_t leave = dtor.find("leavePluginUi();");
        const bool last = leave != std::string::npos && dtor.find("engine_.teardown();") == std::string::npos &&
                          dtor.find_first_not_of(" \n", dtor.find(';', leave) + 1) == std::string::npos;
        if (!last) { std::printf("FAIL: ~AppWindow does not disconnect the plugin UI port (leavePluginUi) as its last act, and must not take the engine down (the host's service does)\n"); }
        CHECK(last);
        const std::string leaveFn = bodyAfter(text, "void AppWindow::leavePluginUi() {");
        CHECK(!leaveFn.empty() && leaveFn.find("ui_->disconnect();") != std::string::npos);
        const std::size_t run = text.find("int AppWindow::run(int frames) {");
        CHECK(run != std::string::npos);
        // run()'s prelude: the last half of the settings handed over, THEN the plugin UI released through the port (once) - all before run() returns, that is
        // before the host stops the engine (main.cpp's order, held by tests/test_engine_service_order.cpp). The port is let go of by closeWindow(), AFTER the
        // engine's stop: disconnected earlier, the engine's pass (still running) would be window-less and wake the asleep AUTO plugins.
        const std::size_t flush = text.find("(void)flushInterfaceHalf();", run);
        const std::size_t release = text.find("ui_->release();", run);
        const std::size_t runEnd = text.find("\n}\n", run);
        const std::size_t closeAt = text.find("void AppWindow::closeWindow() {", run);
        const std::size_t leaveAt = closeAt == std::string::npos ? std::string::npos : text.find("leavePluginUi();", closeAt);
        const std::size_t glAt = closeAt == std::string::npos ? std::string::npos : text.find("ImGui_ImplOpenGL3_Shutdown();", closeAt);
        const bool ordered = flush != std::string::npos && release != std::string::npos && flush < release && release < runEnd &&
                             text.find("leavePluginUi();", run) == leaveAt && leaveAt != std::string::npos && glAt != std::string::npos && leaveAt < glAt &&
                             text.find("ui_->release();", release + 1) == std::string::npos;
        if (!ordered) { std::printf("FAIL: run() does not hand over the last half and release the plugin UI through the port once before it returns, or closeWindow() does not let go of the port before the GL teardown\n"); }
        CHECK(ordered);
        // the bounded run's plugin report half is handed over before the plugin UI is released (the port still has its tracks, paths and panels), only when asked
        const std::size_t hand = text.find("if (pluginStatusReport) { handFrontEndFacts(true); }", run);
        const bool handOk = hand != std::string::npos && release != std::string::npos && hand < release;
        if (!handOk) { std::printf("FAIL: run() does not hand the plugin status report's half over before it releases the plugin UI (and so before the engine's first teardown phase)\n"); }
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

    // 1. rawEngine is named in code by NO file of src/ or tests/ (comments and strings do not count: they are blanked). It was named by 72 files before R1, by the shell's
    // two and test_host.hpp (3) between R1 and R9, and the shell's two went with the shell in R10; test_host.hpp finds the Host's Engine without it.
    std::vector<fs::path> all = sourcesUnder(root / "src");
    const std::vector<fs::path> tests = sourcesUnder(testsDir);
    all.insert(all.end(), tests.begin(), tests.end());
    int named = 0;
    for (const fs::path& p : all) {
        if (p.filename() == "test_engine_shell_guard.cpp") { continue; }   // (this file names it in strings and in this comment; codeLines blanks both, but be exact)
        for (const std::string& l : codeLines(readFile(p))) {
            if (l.find("rawEngine") != std::string::npos) {
                ++named;
                std::printf("FAIL: %s names rawEngine - it was deleted with the shell (R10); a test reaches the Engine through th::engine (tests/test_host.hpp)\n",
                            fs::relative(p, root).generic_string().c_str());
                break;
            }
        }
    }
    std::printf("  rawEngine is named in code by %d files (budget %d: it was 72 before R1, 3 until R9)\n", named, kRawEngineFilesBudget);
    CHECK(named == kRawEngineFilesBudget);
    // the host's Engine comes from the Host, through ONE base every test window has
    CHECK(host.find("class HostedWindow : public cascade::gui::AppWindow") != std::string::npos);
    CHECK(host.find("class TestWindow : private HostHolder, public HostedWindow") != std::string::npos);
    CHECK(host.find("class BareWindow : public HostedWindow") != std::string::npos);
    CHECK(host.find("static_cast<HostedWindow&>(a).hostOf().engine()") != std::string::npos);
    // the window's ONE friend, which names no Engine: the window-private seams a test needs (the port, the late attach, the window's half of a state)
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
    {
        const std::size_t at = host.find("struct TestHostAccess {");
        const std::size_t end = at == std::string::npos ? std::string::npos : host.find("\n};", at);
        CHECK(at != std::string::npos && end != std::string::npos);
        int engineWords = 0;
        if (at != std::string::npos && end != std::string::npos) {
            std::string body;
            for (const std::string& l : codeLines(host.substr(at, end - at))) { body += l + "\n"; }
            for (const char* w : {"Engine", "engine::", "rawEngine", "engine_"}) {
                if (body.find(w) != std::string::npos) { ++engineWords; }
            }
        }
        std::printf("  TestHostAccess names %d engine words (expect 0: it is the window's seams, not a way to the Engine)\n", engineWords);
        CHECK(engineWords == 0);
    }

    // 2. no test constructs an AppWindow by its own name: every window a test builds is a th::TestWindow or th::BareWindow (both th::HostedWindow, which owns or
    // knows the Engine's Host); a bare AppWindow made anywhere else would be a window th::engine cannot find an engine for
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


// ----------------------------------------------------------------------------------------------------------------------------------
// THE BOUNDED RUN'S TWO HOOKS (shell retirement R9, 2026-10-28; core/bounded_hooks.hpp, engine/host_measure.hpp). The window's last two contacts with the
// engine that are not the table were the stepping of the pass (stepPass, three calls a frame) and the measurement run (four seams and the wait beat). They
// are the HOST's now: the host owns the engine, so main() - or the test host - builds two std::functions and hands them to the window, IN A `--frames` RUN
// AND NEVER IN A PRODUCT ONE (a free-running pass has nothing to step). The compiler cannot hold that, nor that the window calls them from two places only.
// What this holds, as text:
//   * the hooks' header is two std::functions and a word (PassPart): it includes <functional> and core/pass_part.hpp and nothing else, names no engine
//     type, and is not denied to the interface (the window includes it) - while the harness that builds them (engine/host_measure.hpp) IS denied;
//   * main() builds them once, on the one line that tests boot.frames >= 0, and assigns them nowhere else; the test host builds them for the same
//     condition (frames >= 0) and nowhere else; and no other file of src/ names makeBoundedHooks;
//   * the window calls them from exactly two members - stepPass and endOfFrameHook - each of which does nothing when the hook is empty; stepPass holds a
//     WatchdogPause across the call and gives the watchdog the heartbeat that takes the pause's gap-skip (so the frame's own beat still measures a gap);
//     the end-of-frame hook holds NO pause (it runs every frame; the host brackets the one wait it makes with BoundedClient::blocked, which the window
//     implements as a pause and, on release, that heartbeat) and is called once, in run()'s loop, after the frame is counted;
//   * the measurement is the harness's: the window names no EngineMeasure, no MeasureHooks and no measure_.
// ----------------------------------------------------------------------------------------------------------------------------------
void checkTheBoundedHooks(const fs::path& root, const std::set<std::string>& deny) {
    // 1. the header
    const std::string hdr = readFile(root / "src" / "core" / "bounded_hooks.hpp");
    CHECK(!hdr.empty());
    int badInc = 0;
    for (const std::string& inc : includesOf(hdr)) {
        if (inc != "core/pass_part.hpp") {
            ++badInc;
            std::printf("FAIL: core/bounded_hooks.hpp includes \"%s\" - the hooks name no engine type: <functional> and core/pass_part.hpp only\n", inc.c_str());
        }
    }
    CHECK(badInc == 0);
    CHECK(hdr.find("#include <functional>") != std::string::npos);
    const std::string part = readFile(root / "src" / "core" / "pass_part.hpp");
    CHECK(!part.empty() && part.find("enum class PassPart { Front, Mute, Back };") != std::string::npos && includesOf(part).empty());
    CHECK(deny.count("core/bounded_hooks.hpp") == 0);       // the interface MAY include it: that is what it is for
    CHECK(deny.count("core/pass_part.hpp") == 0);
    CHECK(deny.count("engine/host_measure.hpp") != 0);      // ... and may not include the harness that builds them
    {
        int members = 0;
        std::string flat;
        for (const std::string& l : codeLines(hdr)) { flat += l + "\n"; }
        const std::size_t at = flat.find("struct BoundedHooks {");
        CHECK(at != std::string::npos);
        const std::size_t end = at == std::string::npos ? std::string::npos : flat.find("};", at);
        CHECK(end != std::string::npos);
        if (at != std::string::npos && end != std::string::npos) {
            const std::string body = flat.substr(at, end - at);
            for (std::size_t m = body.find("std::function<"); m != std::string::npos; m = body.find("std::function<", m + 1)) { ++members; }
        }
        std::printf("  the bounded run's hooks: %d members (expect 2: stepPass, onFrame)\n", members);
        CHECK(members == 2);
    }

    // 2. the host installs them in a bounded run only
    {
        int builds = 0, assigns = 0, gated = 0;
        for (const std::string& l : codeLines(readFile(root / "src" / "main.cpp"))) {
            if (l.find("makeBoundedHooks(") != std::string::npos) {
                ++builds;
                const std::size_t gate = l.find("boot.frames >= 0");
                if (gate != std::string::npos && gate < l.find("makeBoundedHooks(")) { ++gated; }
            }
            if (l.find("host.bounded") != std::string::npos) { ++assigns; }
        }
        std::printf("  main.cpp: makeBoundedHooks %d (expect 1, on the line that tests boot.frames >= 0: %d), host.bounded assigned on %d line(s) (expect 1)\n", builds, gated, assigns);
        CHECK(builds == 1);
        CHECK(gated == 1);
        CHECK(assigns == 1);
        int tbuilds = 0, tgated = 0;
        for (const std::string& l : codeLines(readFile(root / "tests" / "test_host.hpp"))) {
            if (l.find("makeBoundedHooks(") != std::string::npos) {
                ++tbuilds;
                const std::size_t gate = l.find("frames_ >= 0");
                if (gate != std::string::npos && gate < l.find("makeBoundedHooks(")) { ++tgated; }
            }
        }
        CHECK(tbuilds == 1);
        CHECK(tgated == 1);
        // no other file of src/ names the builder (its definition and declaration are the harness's)
        int stray = 0;
        for (const fs::path& f : sourcesUnder(root / "src")) {
            const std::string name = f.filename().string();
            if (name == "main.cpp" || name == "host_measure.cpp" || name == "host_measure.hpp") { continue; }
            for (const std::string& l : codeLines(readFile(f))) {
                if (l.find("makeBoundedHooks") != std::string::npos) {
                    ++stray;
                    std::printf("FAIL: src/%s names makeBoundedHooks - the bounded run's hooks are built by main (and the test host) and nowhere else\n", name.c_str());
                }
            }
        }
        CHECK(stray == 0);
    }

    // 3. the window's two calls
    {
        const std::string text = readFile(root / "src" / "gui" / "app_window.cpp");
        const std::string stepFn = bodyAfter(text, "void AppWindow::stepPass(cascade::core::PassPart part) {");
        const std::string frameFn = bodyAfter(text, "bool AppWindow::endOfFrameHook(int nextFrame) {");
        CHECK(!stepFn.empty());
        CHECK(!frameFn.empty());
        const std::size_t sEmpty = stepFn.find("if (!bounded_.stepPass) { return; }");
        const std::size_t sPause = stepFn.find("cascade::core::WatchdogPause");
        const std::size_t sCall = stepFn.find("bounded_.stepPass(part);");
        const std::size_t sBeat = stepFn.find("watchdog_.heartbeat(false);");
        const bool stepOk = sEmpty != std::string::npos && sPause != std::string::npos && sCall != std::string::npos && sBeat != std::string::npos && sEmpty < sPause &&
                            sPause < sCall && sCall < sBeat;
        // The end-of-frame hook runs at the end of EVERY frame of a bounded run, so it holds NO pause of its own (each release would make the next heartbeat drop the
        // frame's gap, and test_diag_hang's "worst frame gap" would read 0.0 ms): the host brackets the one wait it makes, through boundedClient_.blocked, which
        // pauses the window's watchdog and, on release, gives it the heartbeat that takes the skip.
        const bool frameOk = frameFn.find("if (!bounded_.onFrame) { return true; }") != std::string::npos &&
                             frameFn.find("return bounded_.onFrame(nextFrame, boundedClient_);") != std::string::npos &&
                             frameFn.find("WatchdogPause") == std::string::npos && frameFn.find("pause(") == std::string::npos;
        const std::string ctorBody = bodyAfter(text, "AppWindow::AppWindow(const WindowHost& host)");
        const std::size_t bAt = ctorBody.find("boundedClient_.blocked = [this](bool on) {");
        const std::size_t bEnd = bAt == std::string::npos ? std::string::npos : ctorBody.find("};", bAt);
        const std::string blockedFn = (bAt == std::string::npos || bEnd == std::string::npos) ? std::string() : ctorBody.substr(bAt, bEnd - bAt);
        const bool blockedOk = !blockedFn.empty() && blockedFn.find("watchdog_.pause();") != std::string::npos && blockedFn.find("watchdog_.resume();") != std::string::npos &&
                               blockedFn.find("watchdog_.resume();") < blockedFn.find("watchdog_.heartbeat(false);");
        if (!stepOk) { std::printf("FAIL: AppWindow::stepPass must do nothing without the hook, then hold a WatchdogPause across the hook, then give the watchdog heartbeat(false)\n"); }
        if (!frameOk) { std::printf("FAIL: AppWindow::endOfFrameHook must do nothing without the hook and call it with no pause of its own\n"); }
        if (!blockedOk) { std::printf("FAIL: the window's BoundedClient::blocked must pause the watchdog on true, and resume it and give it heartbeat(false) on false\n"); }
        CHECK(stepOk);
        CHECK(frameOk);
        CHECK(blockedOk);
        int calls = 0;
        for (const fs::path& p : guiFiles(root, true)) {
            for (const std::string& l : codeLines(readFile(p))) {
                for (const char* needle : {"bounded_.stepPass(", "bounded_.onFrame("}) {
                    std::size_t at = 0;
                    while ((at = l.find(needle, at)) != std::string::npos) {
                        ++calls;
                        at += 1;
                    }
                }
            }
        }
        std::printf("  the window calls a bounded hook from %d place(s) (expect 2: stepPass, endOfFrameHook)\n", calls);
        CHECK(calls == 2);
        // the end of the frame: once, in run()'s loop, after the frame is counted
        const std::size_t run = text.find("int AppWindow::run(int frames) {");
        const std::size_t counted = text.find("++rendered;", run);
        // (and its answer closes the window: a measurement that has finished and written its result ends the run - `if (!endOfFrameHook(rendered)) { closeRequested_ = true; }`)
        const std::size_t hook = text.find("if (!endOfFrameHook(rendered)) { closeRequested_ = true; }", run);
        const bool once = run != std::string::npos && counted != std::string::npos && hook != std::string::npos && counted < hook && hook - counted < 1500 &&
                          text.find("endOfFrameHook(rendered)", run) == hook + 5 && text.find("endOfFrameHook(rendered)", hook + 6) == std::string::npos;
        if (!once) { std::printf("FAIL: run() does not end each frame with endOfFrameHook(rendered), once, right after the frame is counted\n"); }
        CHECK(once);
        // the measurement is the harness's
        int measure = 0;
        for (const fs::path& p : guiFiles(root, true)) {
            for (const std::string& l : codeLines(readFile(p))) {
                if (l.find("EngineMeasure") != std::string::npos || l.find("MeasureHooks") != std::string::npos || l.find("measure_") != std::string::npos) {
                    ++measure;
                    std::printf("FAIL: src/gui/%s names the measurement run (%s) - it is the host's harness\n", p.filename().string().c_str(), l.c_str());
                }
            }
        }
        CHECK(measure == 0);
    }
    // 4. the harness does the work the shell's seams did
    {
        const std::string h = readFile(root / "src" / "engine" / "host_measure.cpp");
        CHECK(!h.empty());
        const bool ok = h.find("engine_.stepPass(part);") != std::string::npos && h.find("engine_.notePluginTestFrame(nextFrame);") != std::string::npos &&
                        h.find("measure_->tick(engine_.pipeline_, hooks)") != std::string::npos && h.find("engine_.markPassStats();") != std::string::npos &&
                        h.find("engine_.passStatsJson()") != std::string::npos && h.find("e.postToPass(") != std::string::npos &&
                        h.find("e.applyConverterForSource();") != std::string::npos && h.find("e.followInputRate();") != std::string::npos &&
                        h.find("(void)e.runPassNow();") != std::string::npos && h.find("getenv(\"CASCADE_PLUGIN_TEST\")") != std::string::npos &&
                        h.find("client_->blocked(true);") != std::string::npos && h.find("client_->blocked(false);") != std::string::npos &&
                        h.find("client_->blocked(true);") < h.find("(void)e.runPassNow();") && h.find("(void)e.runPassNow();") < h.find("client_->blocked(false);");
        if (!ok) { std::printf("FAIL: engine/host_measure.cpp no longer does what the shell's seams did (step, frame, tick, pass figures, the pass's install of a source, the report's arming)\n"); }
        CHECK(ok);
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const fs::path root = (argc > 1) ? fs::path(argv[1]) : fs::path(CASCADE_SOURCE_DIR);
    std::printf("scanning %s\n", root.string().c_str());
    const Surface surface = readSurface(root);
    const std::vector<Call> calls = callsIn(root);
    {
        std::size_t doorCalls = 0;
        for (const Call& c : calls) { doorCalls += (c.object == "engine_") ? 1u : 0u; }
        std::printf("  %zu calls found in src/gui: %zu through the door (engine_.x - expect 0), %zu through the plugin UI port (ui_->x)\n", calls.size(), doorCalls,
                    calls.size() - doorCalls);
    }
    const std::set<std::string> deny = denyListOf(root);

    checkSurface(root, surface, calls);
    checkWhoMayKnock(calls);
    checkTheFence(root, deny);
    checkMachineryHasNoInput(root);
    checkTheFrameIsALook(root, calls);
    checkTheWindowSavesNothing(root);
    checkIndexOps(root);
    checkRetiredHooks(root);
    checkPluginUiOwnership(root);
    checkDataHeaderSplit(root);
    checkThePort(root, deny, surface);
    checkTheBoundedHooks(root, deny);
    checkTheTestHost(root, argc > 1);
    return testSummary("test_engine_shell_guard");
}
