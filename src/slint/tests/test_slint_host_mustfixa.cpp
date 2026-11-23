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
    // DRIVE FIXES 5, ITEM 7: "later" is refused where it PROMISES something that is not there; the ordinary uses (a choice that can be made at any time, a later end than start)
    // are listed whole, so a new one has to be looked at.
    {
        const std::vector<std::string> ordinary = {
            "a few quick questions to set foxsdr up for your radio. you can skip them and change everything later.",
            "record the frequency, mode, bandwidth, squelch, volume and mute you set, save them as a macro, and play it back later. nothing else is recorded.",
            "the box needs a later end than start and a higher top than bottom"};
        for (const auto& [rel, w] : words) {
            const std::string l = lower(w);
            std::size_t y = l.find("later");
            while (y != std::string::npos) {
                const bool word = (y == 0 || !std::isalnum(static_cast<unsigned char>(l[y - 1]))) && (y + 5 >= l.size() || !std::isalnum(static_cast<unsigned char>(l[y + 5])));
                bool ok = false;
                for (const auto& o : ordinary) { if (l == o) { ok = true; } }
                if (word && !ok) { std::printf("  \"later\" in %s: %s\n", rel.c_str(), w.c_str()); CHECK(false); }
                y = l.find("later", y + 1);
            }
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

// USER BUG B (2026-11-23): while a basemap plugin was attached the host switched the built-in coastline OFF for good, so tiles that never came (no network, a refused request) left a blank map and nothing
// said why. The compositor already draws the coastline only while no tile is painted, so the host must leave the layer on; the note is a property the map page binds, with its words.
void theMapKeepsItsCoastlineAndSaysWhyTilesAreMissing() {
    const std::string kp = slurp(repoRoot() / "src" / "slint" / "kit_publish.cpp");
    CHECK(has(kp, "s.comp.setLayers(true, true);"));
    CHECK(!has(kp, "s.comp.setLayers(!tiles, true);"));
    CHECK(has(kp, "mapTilesNote(tiles, tileRep.drawn, tileRep.missing, tileRep.pending, bm.lastFailure())"));
    const std::string pk = slurp(repoRoot() / "src" / "slint" / "plugin_kit.cpp");
    CHECK(has(pk, "failureLog_.allow(nowMs)") && has(pk, "map: tile fetches are failing (%s)"));
    const std::string ms = slurp(repoRoot() / "src" / "slint" / "ui" / "map.slint");
    CHECK(has(ms, "in property <string> tiles-note;") && has(ms, "if root.tiles-note != \"\" : HaloText"));
    const std::string pg = slurp(repoRoot() / "src" / "slint" / "ui" / "pages.slint");
    CHECK(has(pg, "tiles-note: root.page.map.tiles-note;"));
}

// USER BUG A (2026-11-23): under automatic gain the gain slider is disabled and said nothing. It is still disabled (the radio sets its own gain), and the hint beside it now says why.
void theGainSliderSaysWhyItIsOffUnderAutomaticGain() {
    const std::string m = slurp(repoRoot() / "src" / "slint" / "main.cpp");
    CHECK(has(m, "ui->set_gain_enabled(lists.haveGain && !agc);"));
    CHECK(has(m, "ui->set_gain_hint(ss(gainHintText(agc, lists.gainStage, state.deviceName)));"));
    const std::string w = slurp(repoRoot() / "src" / "gui" / "app_window.cpp");   // the classic window: the same sentence under the sliders it greys out
    CHECK(has(w, "if (agc && !airspyPanel && !c.gains.empty())") && has(w, "Automatic gain is on. Switch it off to set the gain by hand."));
}

// WINDOW STRIP LANE (look review B1): the receiver strip and the spectrum toolbar FOLD to a second row instead of being cut off, so the smallest window the drag edges allow
// is 1024 x 640. OLD expectation: kMinWindowW = 1480.0f, kMinWindowH = 560.0f (a window cannot be dragged smaller than a screen it does not fit). NEW: 1024.0f and 640.0f.
void theSmallestWindowIsTheMeasuredOne() {
    const std::string h = slurp(repoRoot() / "src" / "slint" / "host_logic.hpp");
    CHECK(has(h, "constexpr float kMinWindowW = 1024.0f;") && has(h, "constexpr float kMinWindowH = 640.0f;"));
    CHECK(!has(h, "kMinWindowW = 1480.0f"));
    const std::string m = slurp(repoRoot() / "src" / "slint" / "main.cpp");
    // Drive fixes 7: the edge drag's size is EdgeDrag::at (look_logic.cpp), which main.cpp hands the same two minimums at the scale in force; the clamp that used to be written in main.cpp
    // as std::max(kMinWindowW * scale() ...) is the `lo` of EdgeDrag::at's axis (the minimum, at the scale, rounded). Same rule, in its new place; the two sides are still checked.
    // User bug C: the minimum handed to the drag is the session's (winMinW / winMinH), which starts as kMinWindowW / kMinWindowH and is lowered only when the work area is smaller than that.
    CHECK(has(m, "edgeDrag.at(px, py, scale(), winMinW, winMinH") && has(m, "float winMinW = kMinWindowW, winMinH = kMinWindowH;"));
    const std::string ll = slurp(repoRoot() / "src" / "slint" / "look_logic.cpp");
    CHECK(has(ll, "std::llround(static_cast<double>(minLogical) * sf)") && has(ll, "std::max(lo, v)"));
    CHECK(!has(m, "std::max(624.0f * scale()"));
    // the folding is in the window's own file: the strip's second row and the toolbar's groups (a revert of either to one fixed row would bring the cut-off back)
    const std::string w = slurp(repoRoot() / "src" / "slint" / "ui" / "main.slint");
    CHECK(has(w, "out property <bool> two-rows: self.width < self.need-one;") && has(w, "out property <length> row2-h: self.two-rows"));
    CHECK(has(w, "height: Theme.strip-h + sb.row2-h;") && has(w, "height: Theme.tab-h + tbr.row2-h;"));
    CHECK(has(w, "g5b := VerticalLayout"));
    // the drives that hold it: nothing cut at 1024, 1280, 1366, 1440, 1480 and 1920 wide (768 and 800 high at 1366 and 1280) and at the minimum itself
    for (const char* d : {"window-min-strip", "window-strip-1024", "window-strip-1280", "window-strip-1366", "window-strip-1440", "window-strip-1480", "window-strip-1920"}) {
        const std::string t = slurp(repoRoot() / "src" / "slint" / "tests" / "drive" / (std::string(d) + ".txt"));
        CHECK(!t.empty() && has(t, "\"Full span\" right <=") && has(t, "\"Bias tee off - asks before switching on\" right <="));
    }
    CHECK(has(slurp(repoRoot() / "src" / "slint" / "tests" / "drive" / "window-min-strip.txt"), "size=1024x640"));
    CHECK(has(slurp(repoRoot() / "src" / "slint" / "tests" / "drive" / "window-strip-1366.txt"), "size=1366x768"));
    CHECK(has(slurp(repoRoot() / "src" / "slint" / "tests" / "drive" / "window-strip-1280.txt"), "size=1280x800"));
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
    theMapKeepsItsCoastlineAndSaysWhyTilesAreMissing();
    theGainSliderSaysWhyItIsOffUnderAutomaticGain();
    theEmptyCommunityRowIsNotDrawn();
    theDropDownGrabsFocusWithoutATimer();
    theWindowSaysNothingOfWhatIsNotThere();
    noForumAndNoDiscordAnywhere();
    viewsWithNoDataAreNotDrawn();
    everyUnprovenRadioRowCarriesTheOneLabel();
    return testSummary("test_slint_host_mustfixa");
}
