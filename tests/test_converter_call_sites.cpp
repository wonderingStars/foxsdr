// EVERY PLACE A FREQUENCY CROSSES TO THE RADIO GOES THROUGH THE CONVERTER.
//
// With an up- or down-converter in front of the radio there are two numbers
// for every tune: the AIR frequency the user means and the RADIO frequency
// the radio must be told (core/freq_converter.hpp). The application keeps
// them apart with ONE translation layer - Pipeline::activeSource() speaks air
// and tells the radio the converted figure (source/converter_view.hpp), and
// PatchRadio does the same for the patch page - so a call that reaches a
// radio's centre frequency any other way is a place where a 16.4 kHz preset
// would tune the radio to 16.4 kHz instead of 125.0164 MHz.
//
// This test reads the source tree and holds that line: every call of
// setCenterFrequencyHz / centerFrequencyHz outside the translation layer
// itself must either go through activeSource(), or hand the radio a figure
// already converted (radioHzForSource / radioFromAir), or be one of the few
// sites listed below with the reason it is right. A new direct call fails
// here until somebody decides which it is. rawSource() - the radio's own
// figure - is allowed only where the converter's own status lines print it.
//
// The whole list of sites is printed, classified, on every run: it is the
// call-site audit the converter was built from, kept current by the tree.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "test_check.hpp"

namespace fs = std::filesystem;

namespace {

std::string readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// Comments and string literals blanked to spaces (newlines kept, so line
// numbers survive): a mention in a comment or a log line is not a call.
std::string code(const std::string& s) {
    std::string out = s;
    enum { Code, Line, Block, Str, Chr } st = Code;
    for (std::size_t i = 0; i < out.size(); ++i) {
        const char c = s[i];
        const char n = i + 1 < s.size() ? s[i + 1] : '\0';
        switch (st) {
            case Code:
                if (c == '/' && n == '/') { st = Line; out[i] = ' '; }
                else if (c == '/' && n == '*') { st = Block; out[i] = ' '; }
                else if (c == '"') { st = Str; }
                else if (c == '\'') { st = Chr; }
                break;
            case Line:
                if (c == '\n') { st = Code; } else { out[i] = ' '; }
                break;
            case Block:
                if (c == '*' && n == '/') { out[i] = ' '; out[i + 1] = ' '; ++i; st = Code; }
                else if (c != '\n') { out[i] = ' '; }
                break;
            case Str:
                if (c == '\\') { out[i] = ' '; if (i + 1 < out.size()) { out[++i] = ' '; } }
                else if (c == '"') { st = Code; }
                else if (c != '\n') { out[i] = ' '; }
                break;
            case Chr:
                if (c == '\\') { out[i] = ' '; if (i + 1 < out.size()) { out[++i] = ' '; } }
                else if (c == '\'') { st = Code; }
                else { out[i] = ' '; }
                break;
        }
    }
    return out;
}

// The statement around position `at`: back to the previous ; { or }, forward
// to the next ; - enough to see the object a call is made on and its argument.
std::string statementAt(const std::string& c, std::size_t at) {
    std::size_t b = at;
    while (b > 0 && c[b - 1] != ';' && c[b - 1] != '{' && c[b - 1] != '}') { --b; }
    std::size_t e = c.find(';', at);
    if (e == std::string::npos) { e = c.size(); }
    std::string s = c.substr(b, e - b);
    std::string flat;
    bool space = false;
    for (const char ch : s) {
        const bool ws = ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t';
        if (ws) {
            if (!space && !flat.empty()) { flat.push_back(' '); }
            space = true;
        } else {
            flat.push_back(ch);
            space = false;
        }
    }
    return flat;
}

int lineOf(const std::string& c, std::size_t at) {
    int line = 1;
    for (std::size_t i = 0; i < at && i < c.size(); ++i) {
        if (c[i] == '\n') { ++line; }
    }
    return line;
}

// Sites that are neither through activeSource() nor explicitly converted, and
// are right anyway. Matched on the file name and a piece of the statement.
struct Allowed {
    const char* file;
    const char* snippet;
    const char* why;
};
const Allowed kAllowed[] = {
    {"app_window_patch_radios.cpp", "g->setCenterFrequencyHz(centreHz)",
     "makePatchGenerator: every caller passes radioFromAir(...) of the node's air frequency"},
    {"app_window_patch_radios.cpp", "dev->setCenterFrequencyHz(centre)",
     "patch open worker: `centre` is the lambda's copy of radioCentre, converted on the GUI thread"},
};

// Files that ARE the translation layer, or never speak to the receiving radio.
bool exempt(const fs::path& p) {
    const std::string s = p.generic_string();
    return s.find("/src/source/") != std::string::npos ||      // the drivers and the view
           s.find("/core/pipeline.cpp") != std::string::npos ||  // owns the view
           s.find("/core/patch_radio.cpp") != std::string::npos ||  // owns the view
           s.find("/core/transmitter.cpp") != std::string::npos ||  // TX sink: no converter on TX
           s.find("/src/main.cpp") != std::string::npos;  // CLI checks, never the GUI receiver
}

std::string findRoot(int argc, char** argv) {
    if (argc > 1) { return argv[1]; }
    fs::path at = fs::current_path();
    for (int i = 0; i < 8; ++i) {
        if (fs::exists(at / "src" / "core" / "freq_converter.hpp")) { return at.string(); }
        if (!at.has_parent_path() || at.parent_path() == at) { break; }
        at = at.parent_path();
    }
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    std::printf("test_converter_call_sites\n");
    const std::string root = findRoot(argc, argv);
    CHECK(!root.empty());  // a test that cannot find the tree checks nothing: fail
    if (root.empty()) { return testSummary("test_converter_call_sites"); }

    int viaAir = 0;
    int converted = 0;
    int allowed = 0;
    int bad = 0;
    int raw = 0;
    std::vector<fs::path> files;
    for (const auto& e : fs::recursive_directory_iterator(fs::path(root) / "src")) {
        if (!e.is_regular_file()) { continue; }
        const std::string ext = e.path().extension().string();
        if (ext != ".cpp" && ext != ".hpp" && ext != ".h") { continue; }
        files.push_back(e.path());
    }
    CHECK(files.size() > 100);  // the real tree, not an empty directory
    for (const fs::path& path : files) {
        if (exempt(path)) { continue; }
        const std::string c = code(readFile(path));
        const std::string name = path.filename().string();
        // LOCAL NAMES FOR THE VIEW: "IqSource& src = pipeline_.activeSource();"
        // makes src.centerFrequencyHz() an air read. A name is trusted only if
        // EVERY such declaration in the file binds it to activeSource() - one
        // bound to anything else (rawSource(), a device) and the name proves
        // nothing, so its calls are judged like any other.
        std::vector<std::string> airNames;
        std::vector<std::string> otherNames;
        {
            const std::string decl = "IqSource& ";
            std::size_t d = 0;
            while ((d = c.find(decl, d)) != std::string::npos) {
                d += decl.size();
                std::size_t e = d;
                while (e < c.size() && (std::isalnum(static_cast<unsigned char>(c[e])) || c[e] == '_')) {
                    ++e;
                }
                const std::string var = c.substr(d, e - d);
                const std::size_t semi = c.find(';', e);
                const std::string init = c.substr(e, semi == std::string::npos ? 0 : semi - e);
                if (var.empty() || init.find('=') == std::string::npos) { continue; }
                (init.find("activeSource()") != std::string::npos ? airNames : otherNames)
                    .push_back(var);
            }
        }
        const auto isAirName = [&](const std::string& stmt) {
            for (const std::string& v : airNames) {
                bool clash = false;
                for (const std::string& o : otherNames) { clash = clash || o == v; }
                if (clash) { continue; }
                if (stmt.find(v + ".centerFrequencyHz(") != std::string::npos ||
                    stmt.find(v + ".setCenterFrequencyHz(") != std::string::npos) {
                    return true;
                }
            }
            return false;
        };
        for (const char* needle : {"enterFrequencyHz(", "rawSource("}) {
            std::size_t at = 0;
            while ((at = c.find(needle, at)) != std::string::npos) {
                const std::size_t hit = at;
                at += 1;
                // Declarations and definitions are not calls.
                const std::string stmt = statementAt(c, hit);
                if (stmt.rfind("virtual", 0) == 0 || stmt.find("override") != std::string::npos ||
                    stmt.find("IqSource& rawSource()") != std::string::npos ||
                    stmt.find("IqSource& Pipeline::rawSource()") != std::string::npos) {
                    continue;
                }
                const int line = lineOf(c, hit);
                const char* verdict = nullptr;
                if (stmt.find("rawSource()") != std::string::npos) {
                    // The radio's own figure: only the converter's status
                    // lines may print it. Counted once, on the rawSource hit.
                    if (std::string(needle) != "rawSource(") { continue; }
                    ++raw;
                    if (name == "app_window_converter.cpp") {
                        verdict = "RADIO FIGURE (the converter's status lines)";
                        ++allowed;
                    }
                } else if (std::string(needle) == "rawSource(") {
                    continue;  // unreachable: a rawSource( hit has rawSource() in it
                } else if (stmt.find("activeSource()") != std::string::npos) {
                    verdict = "AIR via activeSource()";
                    ++viaAir;
                } else if (isAirName(stmt)) {
                    verdict = "AIR via a local name for activeSource()";
                    ++viaAir;
                } else if (stmt.find("radioHzForSource(") != std::string::npos ||
                           stmt.find("radioFromAir(") != std::string::npos ||
                           stmt.find("fileRadioHz") != std::string::npos ||
                           stmt.find("setCenterFrequencyHz(radioHz)") != std::string::npos) {
                    verdict = "CONVERTED before install";
                    ++converted;
                } else {
                    for (const Allowed& a : kAllowed) {
                        if (name == a.file && stmt.find(a.snippet) != std::string::npos) {
                            verdict = a.why;
                            ++allowed;
                            break;
                        }
                    }
                }
                if (verdict == nullptr) {
                    ++bad;
                    std::printf("FAIL  %s:%d  bypasses the converter: %s\n", name.c_str(), line,
                                stmt.c_str());
                } else {
                    std::printf("      %s:%d  %s\n", name.c_str(), line, verdict);
                }
            }
        }
    }
    std::printf("  %d through activeSource(), %d converted before install, %d allowed, %d raw "
                "reads; %d bypass the converter\n",
                viaAir, converted, allowed, raw, bad);
    CHECK(bad == 0);
    // The counts that prove the scan saw the real tree: the tune path, the
    // saved centre and the carry-across all read through the view.
    CHECK(viaAir >= 20);
    CHECK(converted >= 3);
    return testSummary("test_converter_call_sites");
}
