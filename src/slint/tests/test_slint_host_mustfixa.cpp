// Must-fix lane A (0.99.109, docs/multi-radio-records/mustfix-a.md): the rules this lane made, held by reading the window's own sources and catalogues as text.
//  - the drop-down's focus grab owns no Timer (MG6-popup-timer);
//  - the window says nothing about what is not there: no "not built", "phase 3", "coming", "Stellarium link", no forum or Discord, no "units ... none yet";
//  - a view or control whose data nothing publishes (EVM, symbol rate, lock, survey, radiosonde landing, propagation cities, FT8 reporters, GOES band, satellite orbit path) is not drawn, and
//    the rule that keeps it out is a test: a caption below must not be drawn until the code that draws it is gated on the data being published;
//  - decision 61: ONE sentence ("Not yet proved on a real radio.") through ONE component on every radio family proved only against stand-ins;
//  - Bench Classic XL and the three map layers with no approved source are named only where their rule lives.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "test_check.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path repoRoot() {
    fs::path p = fs::path(__FILE__).parent_path();   // src/slint/tests
    return p.parent_path().parent_path().parent_path();
}

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

std::string lower(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

bool has(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

// every file of the window's own sources (not its tests, not the generated assets), as {relative path, text}
struct Src { std::string rel; std::string text; };
std::vector<Src> windowSources() {
    std::vector<Src> out;
    const fs::path base = repoRoot() / "src" / "slint";
    for (const auto& e : fs::recursive_directory_iterator(base)) {
        if (!e.is_regular_file()) { continue; }
        const std::string ext = e.path().extension().string();
        if (ext != ".cpp" && ext != ".hpp" && ext != ".slint") { continue; }
        const std::string rel = fs::relative(e.path(), base).generic_string();
        if (rel.rfind("tests/", 0) == 0 || rel.find("/specimens/") != std::string::npos) { continue; }
        out.push_back({rel, slurp(e.path())});
    }
    return out;
}

// the words the window shows: every string literal handed to tr(...), @tr(...), FOX_TR_NOOP(...) or T(...) (the SYSTEM and TOOLS logic's own wrapper)
std::vector<std::pair<std::string, std::string>> shownWords(const std::vector<Src>& srcs) {
    std::vector<std::pair<std::string, std::string>> out;
    // a hand scanner (std::regex overflows the stack on a file this size): a call name, "(", blanks, then one string literal
    const auto isId = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; };
    const char* const names[] = {"@tr", "tr", "FOX_TR_NOOP", "T"};
    for (const Src& s : srcs) {
        const std::string& t = s.text;
        for (std::size_t i = 0; i + 2 < t.size(); ++i) {
            for (const char* nm : names) {
                const std::size_t n = std::strlen(nm);
                if (t.compare(i, n, nm) != 0) { continue; }
                if (nm[0] != '@' && i > 0 && (isId(t[i - 1]) || t[i - 1] == '@')) { continue; }
                std::size_t j = i + n;
                if (j >= t.size() || t[j] != '(') { continue; }
                ++j;
                while (j < t.size() && std::isspace(static_cast<unsigned char>(t[j])) != 0) { ++j; }
                if (j >= t.size() || t[j] != '"') { continue; }
                std::string lit;
                for (++j; j < t.size() && t[j] != '"'; ++j) {
                    if (t[j] == '\\' && j + 1 < t.size()) { lit += t[j]; ++j; }
                    lit += t[j];
                }
                out.push_back({s.rel, lit});
            }
        }
    }
    return out;
}

void theDropDownGrabsFocusWithoutATimer() {
    const std::string sb = slurp(repoRoot() / "src" / "slint" / "ui" / "system_bank.slint");
    const std::size_t at = sb.find("component LineInput inherits Rectangle");
    CHECK(at != std::string::npos);
    // the component's body runs to the next top-level "component " or "export component "
    std::size_t end = sb.find("\ncomponent ", at + 10);
    const std::size_t end2 = sb.find("\nexport component ", at + 10);
    if (end == std::string::npos || (end2 != std::string::npos && end2 < end)) { end = end2; }
    CHECK(end != std::string::npos);
    const std::string body = sb.substr(at, end - at);
    for (std::size_t p = body.find("Timer"); p != std::string::npos; p = body.find("Timer", p + 1)) {   // the tooltip's TipTimer is not a focus timer: a bare Timer element is
        const bool tip = p >= 3 && body.compare(p - 3, 3, "Tip") == 0;
        const bool element = body.compare(p, 6, "Timer ") == 0 || body.compare(p, 6, "Timer{") == 0;
        CHECK(tip || !element);
    }                       // MG6-popup-timer: nothing is left pending on a popup that is gone
    CHECK(has(body, "changed grab-token"));                                     // the field takes the keyboard when the drop-down says the popup exists
    // the drop-down bumps the token AFTER it shows the popup (never before: the field would not see a change)
    const std::size_t show = sb.find("popup.show(); root.grab-n += 1;");
    CHECK(show != std::string::npos);
}

void theWindowSaysNothingOfWhatIsNotThere() {
    const auto srcs = windowSources();
    const auto words = shownWords(srcs);
    CHECK(words.size() > 2000);   // the pattern found the window's words (it did not quietly match nothing)
    const std::vector<std::string> banned = {"not built", "phase 3", "coming soon", "stellarium link", "this window has none", "later phase", "forum", "discord", "bench classic xl",
                                              "does not run this part"};
    for (const auto& [rel, w] : words) {
        const std::string l = lower(w);
        for (const std::string& b : banned) {
            if (!has(l, b)) { continue; }
            // Bench Classic XL is the one look the Theme list does not offer: its name lives only in the presets table (system_logic.cpp), never in a drawn list
            if (b == "bench classic xl" && rel == "system_logic.cpp") { continue; }
            std::printf("  the window says \"%s\" in %s: %s\n", b.c_str(), rel.c_str(), w.c_str());
            CHECK(false);
        }
    }
    // LOOK REVIEW M2/M3 (window follow-ups): no developer words and no "yet" in anything the window shows; the one "yet" is the owner's label of decision 61
    for (const auto& [rel, w] : words) {
        const std::string l = lower(w);
        for (const char* dev : {"unverified", "link registry", "this lane", "by the lane", "todo", "stub"}) {
            if (has(l, dev)) { std::printf("  developer word \"%s\" in %s: %s\n", dev, rel.c_str(), w.c_str()); CHECK(false); }
        }
        std::size_t y = l.find("yet");
        while (y != std::string::npos) {
            const bool word = (y == 0 || !std::isalnum(static_cast<unsigned char>(l[y - 1]))) && (y + 3 >= l.size() || !std::isalnum(static_cast<unsigned char>(l[y + 3])));
            if (word && l != "not yet proved on a real radio.") { std::printf("  \"yet\" in %s: %s\n", rel.c_str(), w.c_str()); CHECK(false); }
            y = l.find("yet", y + 1);
        }
    }
    // the placeholder components of phase 1 are gone: nothing draws a "phase 3" plate
    CHECK(!fs::exists(repoRoot() / "src" / "slint" / "ui" / "stubs.slint"));
    // the one place that may say "yet" about a radio is the decision 61 label, once
    int proved = 0;
    for (const auto& [rel, w] : words) { if (lower(w) == "not yet proved on a real radio.") { ++proved; } }
    CHECK(proved == 1);
}

// LOOK REVIEW M2: the Needed libraries rows are sentences a person can act on. The tables that hold them name no lane, registry or verification state.
void theNeededLibrariesTextIsForPeople() {
    int files = 0;
    for (const char* rel : {"src/source/needed_library.cpp", "src/source/sdriq_d2xx.cpp", "src/engine/wire_b_needed_libs.cpp"}) {
        const std::string t = slurp(repoRoot() / rel);
        CHECK(!t.empty());
        ++files;
        // only the text between double quotes on lines that are not comments: the sentences the rows carry
        std::istringstream in(t);
        std::string line;
        while (std::getline(in, line)) {
            const std::size_t c = line.find("//");
            const std::size_t q = line.find('"');
            if (q == std::string::npos || (c != std::string::npos && c < q)) { continue; }
            const std::string l = lower(line.substr(q));
            for (const char* dev : {"this lane", "the lane", "link registry", "unverified", "(how:", "where from:", "not read by"}) {
                if (has(l, dev)) { std::printf("  developer words \"%s\" in %s: %s\n", dev, rel, line.c_str()); CHECK(false); }
            }
        }
    }
    CHECK(files == 3);
}

// LOOK REVIEW B1: the smallest window the drag edges allow is the one where the strip, the spectrum toolbar and the rail all fit (measured: 1480 x 560).
void theSmallestWindowIsTheMeasuredOne() {
    const std::string h = slurp(repoRoot() / "src" / "slint" / "host_logic.hpp");
    CHECK(has(h, "constexpr float kMinWindowW = 1480.0f;") && has(h, "constexpr float kMinWindowH = 560.0f;"));
    const std::string m = slurp(repoRoot() / "src" / "slint" / "main.cpp");
    CHECK(has(m, "std::max(kMinWindowW * scale()") && has(m, "std::max(kMinWindowH * scale()"));
    CHECK(!has(m, "std::max(624.0f * scale()"));
}

// OA-75 follow-up: the Community row of the SYSTEM list is not drawn (it keeps its logical place so the rows after it keep their numbers).
void theEmptyCommunityRowIsNotDrawn() {
    const std::string m = slurp(repoRoot() / "src" / "slint" / "main.cpp");
    CHECK(has(m, "cascade::i18n::tr(\"Accessibility\"), \"\", cascade::i18n::tr(\"Spectrum tools\")"));
    const std::string s = slurp(repoRoot() / "src" / "slint" / "ui" / "sections.slint");
    CHECK(has(s, "height: root.hidden ? 0px") && has(s, "visible: !root.hidden;"));
    const std::string mw = slurp(repoRoot() / "src" / "slint" / "ui" / "main.slint");
    CHECK(has(mw, "hidden: s.name == \"\";"));
    for (const Src& x : windowSources()) { CHECK(!has(x.text, "No community links")); }
}

void noForumAndNoDiscordAnywhere() {
    // the 33 catalogues: no key and no value names either (OA-75, owner decision 75: there is no forum and no Discord)
    int files = 0;
    for (const auto& e : fs::directory_iterator(repoRoot() / "resources" / "lang")) {
        if (e.path().extension() != ".json") { continue; }
        ++files;
        const std::string l = lower(slurp(e.path()));
        if (has(l, "forum") || has(l, "discord")) { std::printf("  %s names a forum or Discord\n", e.path().filename().string().c_str()); CHECK(false); }
    }
    CHECK(files == 33);
    // the window's sources and the in-app help
    for (const Src& s : windowSources()) {
        const std::string l = lower(s.text);
        if (has(l, "discord") || has(l, "community.forum") || has(l, "forumurl")) { std::printf("  %s names Discord or a forum address\n", s.rel.c_str()); CHECK(false); }
    }
}

// The rows of the window's views that need data NOTHING publishes today are not drawn (must-fix lane A, ledger rows NB-3054 to NB-4090). A caption below must not appear in a drawn string until the code that
// draws it is gated on the data being published (and this list is then edited with that change).
void viewsWithNoDataAreNotDrawn() {
    const std::vector<std::string> notDrawn = {"EVM", "SYMBOL RATE", "PERCENTILE FLOOR", "MERGE WIDTH", "SWEEP BAND", "PREDICTED LANDING", "3D ORBIT PATH", "PASS TIMELINE", "LAUNCH",
                                               "COLOUR BY Symbol", "SEGMENTS", "BAND (ABI"};
    const auto words = shownWords(windowSources());
    for (const auto& [rel, w] : words) {
        for (const std::string& n : notDrawn) {
            if (w == n || w.rfind(n + " ", 0) == 0) { std::printf("  %s draws \"%s\" with no data behind it\n", rel.c_str(), w.c_str()); CHECK(false); }
        }
    }
}

void everyUnprovenRadioRowCarriesTheOneLabel() {
    const fs::path base = repoRoot() / "src" / "slint";
    const std::string s5b = slurp(base / "s5b_rows.cpp"), s5 = slurp(base / "s5_rows.cpp"), l = slurp(base / "lanel_publish.cpp");
    const auto count = [](const std::string& hay, const std::string& needle) { int n = 0; for (std::size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) { ++n; } return n; };
    CHECK(count(s5b, "slk::rUnproven()") == 4);   // RTL-SDR IF bandwidth, SoapySDR bandwidth and settings, CAT / OmniRig, SDRplay low-IF
    CHECK(count(s5, "slk::rUnproven()") == 1);    // automatic ppm
    CHECK(count(l, "slk::rUnproven()") == 1);     // Fobos
    // one component, one sentence: no other file composes the words
    for (const Src& s : windowSources()) { if (s.rel != "lanek_logic.cpp") { CHECK(!has(s.text, "Not yet proved")); } }
}

}  // namespace

// OWNER DECISION 19 (window follow-ups, item 11): the CAT "Reachable from other machines" tick is offered by neither window. The constant is false, and each window draws the
// tick only under it.
void theCatReachableTickIsNotOffered() {
    const std::string h = slurp(repoRoot() / "src" / "core" / "cat_scope.hpp");
    CHECK(has(h, "kCatReachableOffered = false;"));
    const std::string legacy = slurp(repoRoot() / "src" / "gui" / "app_window.cpp");
    const std::size_t box = legacy.find("ImGui::Checkbox(trId(" + std::string(1, 34) + "Reachable from other machines" + std::string(1, 34) + ")");
    CHECK(box != std::string::npos);
    const std::size_t guard = box == std::string::npos ? std::string::npos : legacy.rfind("if (cascade::core::kCatReachableOffered)", box);
    CHECK(guard != std::string::npos && box - guard < 400);
    const std::string ext = slurp(repoRoot() / "src" / "slint" / "extend_logic.cpp");
    CHECK(has(ext, "m.reachable_offered = cc::kCatReachableOffered;"));
    const std::string sl = slurp(repoRoot() / "src" / "slint" / "ui" / "extend_bank.slint");
    const std::size_t tick = sl.find("label: @tr(" + std::string(1, 34) + "Reachable from other machines" + std::string(1, 34) + ")");
    CHECK(tick != std::string::npos && sl.rfind("if root.model.reachable-offered", tick) != std::string::npos);
}

int main() {
    theCatReachableTickIsNotOffered();
    theNeededLibrariesTextIsForPeople();
    theSmallestWindowIsTheMeasuredOne();
    theEmptyCommunityRowIsNotDrawn();
    theDropDownGrabsFocusWithoutATimer();
    theWindowSaysNothingOfWhatIsNotThere();
    noForumAndNoDiscordAnywhere();
    viewsWithNoDataAreNotDrawn();
    everyUnprovenRadioRowCarriesTheOneLabel();
    return testSummary("test_slint_host_mustfixa");
}
