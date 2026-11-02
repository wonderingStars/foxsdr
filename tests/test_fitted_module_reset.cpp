// test_fitted_module_reset.cpp - EVERY FIELD OF A FITTED MODULE IS RESET WHERE ITS SLOT IS REFILLED (the deep pre-merge review, batch B, finding 1;
// docs/engine-stage4.md 6.32).
//
// WHAT WENT WRONG. The Fitted modules window's model is a vector of FittedModule that fittedLists (src/gui/app_window_fitted.cpp) REFILLS IN PLACE whenever the
// engine's lists move - strings and vectors keep their room, so a refresh allocates almost nothing - which means a slot is a different module after the list shifts.
// fillFittedModule (src/gui/plugins_view.cpp) is the one function that resets a slot, and it reset every field a later step sets conditionally except three: the
// transmit grant, the offer of its key and the plate, which fittedLists sets only for a module that has fact.plugin.<file>.tx. So the module that moved up into a
// granted transmitter's slot showed REVOKE TRANSMIT and an ARMED plate, and nothing ever cleared them. tests/test_fitted_modules_window.cpp (A's tail and G) holds the
// three fields and the scenario; THIS holds the class, because a test that names the fields cannot name the next one:
//
//   * every data member of struct FittedModule (src/gui/plugins_view.hpp) is written by fillFittedModule - assigned, cleared or assigned from - and a member that is
//     deliberately set only elsewhere would have to be named in kSetElsewhere below, with its reason (none is);
//   * fittedLists calls fillFittedModule BEFORE it sets the three transmit fields, not after (the reset would undo the set);
//   * the scan can see what it looks for: the same scan, run on the function as it was before the fix, reports the three fields missing.
//
// It is a source scan because no test can enumerate the members of a C++ struct, and the window needs a GL context to run the real path: the real path is G's.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include "test_check.hpp"

namespace fs = std::filesystem;

namespace {

// Members of FittedModule that fillFittedModule does not write, and why. None: the function is the slot's one reset.
struct SetElsewhere {
    const char* member;
    const char* why;
};
const std::vector<SetElsewhere> kSetElsewhere = {};

std::string readText(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c != '\r') { out += c; }
    }
    return out;
}

// `text` with every // comment removed (a comment says a great deal about the old behaviour and must not be mistaken for code).
std::string withoutLineComments(const std::string& text) {
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t eol = text.find('\n', i);
        const std::size_t end = eol == std::string::npos ? text.size() : eol;
        std::string line = text.substr(i, end - i);
        const std::size_t slashes = line.find("//");
        if (slashes != std::string::npos) { line.erase(slashes); }
        out += line;
        out += '\n';
        i = end + 1;
    }
    return out;
}

// The text between the brace that opens at `open` and its match, exclusive. "" when unbalanced.
std::string braced(const std::string& text, std::size_t open) {
    int depth = 0;
    for (std::size_t i = open; i < text.size(); ++i) {
        if (text[i] == '{') { ++depth; }
        if (text[i] == '}' && --depth == 0) { return text.substr(open + 1, i - open - 1); }
    }
    return std::string();
}

bool identChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

// The data members of `struct FittedModule { ... }`, in declaration order: the statements at brace depth 1 that end in ';' and are not a function, a nested type, a
// using or a static. The name is the last identifier before the '=' (or the ';').
std::vector<std::string> membersOf(const std::string& header) {
    std::vector<std::string> out;
    const std::size_t at = header.find("struct FittedModule {");
    if (at == std::string::npos) { return out; }
    const std::string body = withoutLineComments(braced(header, header.find('{', at)));
    int depth = 0;
    std::string stmt;
    for (const char c : body) {
        if (c == '{') {
            if (depth == 0) { stmt += c; }   // (a brace-initialised member, or a nested type: the statement is judged when its ';' comes)
            ++depth;
            continue;
        }
        if (c == '}') {
            --depth;
            if (depth == 0) { stmt += c; }
            continue;
        }
        if (depth > 0) { continue; }         // (inside a nested type's body, or a member's initialiser)
        if (c != ';') {
            stmt += c;
            continue;
        }
        std::string s = stmt;
        stmt.clear();
        const std::size_t first = s.find_first_not_of(" \t\n");
        if (first == std::string::npos) { continue; }
        s.erase(0, first);
        static const char* const kNotMembers[] = {"struct", "enum", "using", "typedef", "static", "friend", "class", "public", "private"};
        bool skip = s.find('(') != std::string::npos;
        for (const char* k : kNotMembers) {
            if (s.rfind(k, 0) == 0 && !identChar(s[std::char_traits<char>::length(k)])) { skip = true; }
        }
        if (skip) { continue; }
        const std::size_t eq = s.find('=');
        std::string decl = eq == std::string::npos ? s : s.substr(0, eq);
        const std::size_t brace = decl.find('{');
        if (brace != std::string::npos) { decl.erase(brace); }
        std::size_t e = decl.size();
        while (e > 0 && !identChar(decl[e - 1])) { --e; }
        std::size_t b = e;
        while (b > 0 && identChar(decl[b - 1])) { --b; }
        if (b < e) { out.push_back(decl.substr(b, e - b)); }
    }
    return out;
}

// The text of fillFittedModule, from its signature to the brace that closes it.
std::string fillBody(const std::string& source) {
    const std::size_t at = source.find("void fillFittedModule(FittedModule& m, const FittedRow& r) {");
    if (at == std::string::npos) { return std::string(); }
    return braced(source, source.find('{', at));
}

// True when `body` writes member `name` of `m`: `m.name = ...`, `m.name.clear()` or `m.name.assign(...)`.
bool writes(const std::string& body, const std::string& name) {
    const std::regex re("\\bm\\." + name + "\\s*(=[^=]|\\.clear\\s*\\(|\\.assign\\s*\\()");
    return std::regex_search(body, re);
}

// The members `body` leaves unwritten (and that are not on the set-elsewhere list).
std::vector<std::string> unwritten(const std::vector<std::string>& members, const std::string& body) {
    std::vector<std::string> out;
    for (const std::string& m : members) {
        bool elsewhere = false;
        for (const SetElsewhere& e : kSetElsewhere) { elsewhere = elsewhere || m == e.member; }
        if (!elsewhere && !writes(body, m)) { out.push_back(m); }
    }
    return out;
}

}  // namespace

int main() {
    std::printf("test_fitted_module_reset\n");
    const fs::path src = fs::path(CASCADE_SOURCE_DIR) / "src" / "gui";
    const std::string header = readText(src / "plugins_view.hpp");
    const std::string impl = readText(src / "plugins_view.cpp");
    const std::string window = readText(src / "app_window_fitted.cpp");
    CHECK(!header.empty());
    CHECK(!impl.empty());
    CHECK(!window.empty());

    const std::vector<std::string> members = membersOf(header);
    const std::string body = withoutLineComments(fillBody(impl));
    std::printf("  FittedModule: %zu data members; fillFittedModule: %zu bytes of code\n", members.size(), body.size());
    // The scan saw the real struct and the real function (the struct had 37 members when this was written: a parse that found a handful has gone blind).
    CHECK(members.size() >= 30);
    CHECK(body.size() > 600);
    const std::set<std::string> distinct(members.begin(), members.end());
    CHECK(distinct.size() == members.size());
    for (const char* known : {"file", "commands", "catalogueIndex", "ownWindow", "txCapable", "txAllowed", "txPlate", "sizeBytes"}) {
        const bool seen = distinct.count(known) != 0;
        if (!seen) { std::printf("FAIL: the scan did not find the member %s of FittedModule - the parse has gone blind\n", known); }
        CHECK(seen);
    }
    // (nested type Command's own fields are not the module's)
    CHECK(distinct.count("label") == 0);

    // --- THE CONTROL: the scan sees a field that is not reset ----------------------------------------------------------------------------------------------------------
    //
    // The function as it stood before the fix: the row's fields and the conditional ones, without the three transmit fields. A scan that cannot find THESE missing would
    // pass the real function for the wrong reason - a renamed function, a signature that no longer matches - and the test would be green and blind.
    {
        const std::string before = body;
        std::string old = before;
        for (const char* gone : {"m.txCapable = false;", "m.txAllowed = false;", "m.txPlate = 0;"}) {
            const std::size_t at = old.find(gone);
            CHECK(at != std::string::npos);
            if (at != std::string::npos) { old.erase(at, std::char_traits<char>::length(gone)); }
        }
        const std::vector<std::string> missing = unwritten(members, old);
        CHECK(missing.size() == 3u);
        const std::set<std::string> asSet(missing.begin(), missing.end());
        CHECK(asSet.count("txCapable") == 1u && asSet.count("txAllowed") == 1u && asSet.count("txPlate") == 1u);
    }

    // --- THE REAL FUNCTION: no member left unwritten --------------------------------------------------------------------------------------------------------------------
    {
        const std::vector<std::string> missing = unwritten(members, body);
        for (const std::string& m : missing) {
            std::printf("FAIL: fillFittedModule does not write FittedModule::%s - fittedLists refills the model's slots in place, so a module that moves into a slot would "
                        "keep what the one before it left there (a field set elsewhere on purpose goes on kSetElsewhere with its reason)\n", m.c_str());
        }
        CHECK(missing.empty());
    }

    // --- THE ORDER: the reset comes first, the transmit fields are set after it ------------------------------------------------------------------------------------------
    {
        const std::string code = withoutLineComments(window);
        const std::size_t fn = code.find("FittedListCache& AppWindow::fittedLists(");
        CHECK(fn != std::string::npos);
        const std::size_t fill = code.find("fillFittedModule(m, r);", fn);
        const std::size_t tx = code.find("m.txAllowed =", fn);
        CHECK(fill != std::string::npos && tx != std::string::npos);
        CHECK(fill < tx);
    }
    return testSummary("test_fitted_module_reset");
}
