// input_script.hpp - a scripted pointer and keyboard for bounded test runs.
// No ImGui.
//
// WHY THIS EXISTS. The patch page's nodes are operated directly - a close key,
// a resize grip, a frequency box on a node's face - and until now the only way
// to exercise that was to drive the operating system's cursor from a harness.
// On this desk that is the owner's own mouse, and a harness has already landed
// clicks on the wrong window more than once. This feeds events straight into
// ImGui's input queue inside the process instead: nothing outside the app is
// touched, and a script can aim at a node by its position ON THE PATCH rather
// than on the screen, so it does not care where the page happened to open.
//
// ONLY IN A BOUNDED --frames RUN (see AppWindow::run), off unless
// FOXSDR_INPUT_SCRIPT names a file, like every other test hook here.
//
// The format, one step per line; '#' starts a comment, except in a text
// step, which types the whole rest of its line - '#' included:
//
//   <frame> world <x> <y>     pointer to patch-canvas (world) coordinates
//   <frame> screen <x> <y>    pointer to ImGui screen coordinates
//   <frame> down | up         left button
//   <frame> key <name>        tap a key: enter, delete, backspace, escape,
//                             tab, ctrl+a
//   <frame> text <chars...>   type the rest of the line
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_GUI_INPUT_SCRIPT_HPP
#define CASCADE_GUI_INPUT_SCRIPT_HPP

#include <sstream>
#include <string>
#include <vector>

namespace cascade::gui {

struct ScriptStep {
    enum class Verb { World, Screen, Down, Up, Key, Text, Wheel };
    long frame = 0;
    Verb verb = Verb::World;
    float x = 0.0f;
    float y = 0.0f;
    std::string arg;   // the key name, or the text to type
};

struct ScriptParse {
    std::vector<ScriptStep> steps;
    int bad = 0;       // lines that were not blank or comments and did not parse
};

inline const char* const kScriptKeys[] = {"enter", "delete", "backspace", "escape", "tab",
                                          "ctrl+a"};

inline bool knownScriptKey(const std::string& k) {
    for (const char* n : kScriptKeys) {
        if (k == n) { return true; }
    }
    return false;
}

// Parses a script. Steps come back in file order; a script whose frames go
// BACKWARDS is refused line by line (counted in `bad`), because a step that
// can never run is a test that silently checks less than it says.
inline ScriptParse parseInputScript(const std::string& text) {
    ScriptParse out;
    std::istringstream in(text);
    std::string line;
    long lastFrame = -1;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        const auto hash = line.find('#');
        const std::string body = (hash == std::string::npos) ? line : line.substr(0, hash);
        std::istringstream s(body);
        ScriptStep st;
        std::string verb;
        if (!(s >> st.frame)) {
            // Blank or comment-only: not an error.
            std::string rest;
            if (std::istringstream(body) >> rest) { ++out.bad; }
            continue;
        }
        if (!(s >> verb) || st.frame < 0 || st.frame < lastFrame) {
            ++out.bad;
            continue;
        }
        bool ok = true;
        if (verb == "world" || verb == "screen") {
            st.verb = (verb == "world") ? ScriptStep::Verb::World : ScriptStep::Verb::Screen;
            ok = static_cast<bool>(s >> st.x >> st.y);
        } else if (verb == "down") {
            st.verb = ScriptStep::Verb::Down;
        } else if (verb == "up") {
            st.verb = ScriptStep::Verb::Up;
        } else if (verb == "key") {
            st.verb = ScriptStep::Verb::Key;
            ok = static_cast<bool>(s >> st.arg) && knownScriptKey(st.arg);
        } else if (verb == "wheel") {
            // "wheel -3": three notches towards the user, as a mouse wheel
            // scrolling down a list gives (0.99.19).
            st.verb = ScriptStep::Verb::Wheel;
            ok = static_cast<bool>(s >> st.y) && st.y != 0.0f;
        } else if (verb == "text") {
            // THE REST OF THE RAW LINE, not of the comment-stripped body: a
            // '#' is something to type here ("Channel #12"), and cutting the
            // payload at it typed "Channel " as a perfectly good step with
            // nothing counted in `bad`. The body is a prefix of the line, so
            // the stream's position in it is the same position in the line.
            st.verb = ScriptStep::Verb::Text;
            const std::streamoff at = s.tellg();
            const std::size_t from =
                (at < 0) ? body.size() : static_cast<std::size_t>(at);
            // What follows the verb is whitespace, or the '#' that ended the
            // body. "5 text#12" stays what it always was, a text step with
            // nothing to type; otherwise one separating space is dropped,
            // exactly as before.
            if (from < line.size() && line[from] != '#') {
                st.arg = line.substr(from);
                if (!st.arg.empty() && st.arg.front() == ' ') { st.arg.erase(0, 1); }
            }
            ok = !st.arg.empty();
        } else {
            ok = false;
        }
        if (!ok) {
            ++out.bad;
            continue;
        }
        lastFrame = st.frame;
        out.steps.push_back(st);
    }
    return out;
}

}  // namespace cascade::gui

#endif  // CASCADE_GUI_INPUT_SCRIPT_HPP
