// One-click layouts (src/slint/layouts_prefs.hpp, lane S1 item 2): the text they are saved in, the two stock layouts, save / rename / remove and the rules for names, the step order a restore uses, the
// older settings file (no entry) and a newer one (unknown keys), and that saving them leaves the workspace entry and the 3D views' entries as they were. Pure: no Slint, no engine.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "layouts_prefs.hpp"
#include "view3d_prefs.hpp"
#include "workspace_prefs.hpp"
#include "test_check.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

using namespace foxsdr::slintlayouts;

namespace {

Layout sample(const std::string& name) {
    Layout l;
    l.name = name;
    l.bank = 2; l.sections = 0b10100; l.railFolded = true; l.view = 1; l.splitPct = 33; l.workspace = 2; l.link = true; l.wf3d = true; l.selected = 3; l.tuning = true; l.tuneHz = 7074000; l.mode = 4;
    return l;
}

void stock() {
    const auto& b = builtins();
    CHECK(b.size() == 2);
    CHECK(b[0].name == "Airband" && b[0].builtin && b[0].bank == 2 && b[0].sections == (1u << 4) && b[0].workspace == 1);
    CHECK(b[1].name == "HF DX" && b[1].builtin && b[1].bank == 0 && b[1].workspace == 0 && b[1].splitPct < 45);
    for (const Layout& l : b) { CHECK(!l.tuning && l.selected == 0); }   // a stock layout never retunes and never picks a radio
}

void names() {
    CHECK(cleanName("  My   layout  ") == "My layout");
    CHECK(cleanName("a;b|c,d=e:f~g") == "a b c d e f g");
    CHECK(cleanName(";;;") == "");
    CHECK(cleanName("") == "");
    CHECK(cleanName(std::string("tab\there\nnl")) == "tab here nl");
    CHECK(cleanName(std::string(60, 'x')).size() == kMaxNameBytes);
    // never cut inside a UTF-8 sequence: 12 two-byte characters = 24 bytes, 13 = 26 -> 24
    std::string ae;
    for (int i = 0; i < 13; ++i) { ae += "\xC3\xA4"; }
    const std::string cut = cleanName(ae);
    CHECK(cut.size() == 24 && cut.size() % 2 == 0);
}

void roundTrip() {
    LayoutBook book;
    book.load("");
    CHECK(book.user().empty());
    CHECK(book.save("Mine", sample("")) == SaveResult::Saved);
    CHECK(book.save("Second one", sample("")) == SaveResult::Saved);
    const std::string text = book.text("");
    CHECK(text.rfind("layouts:", 0) == 0);
    LayoutBook again;
    again.load(text);
    CHECK(again.user().size() == 2);
    CHECK(again.user()[0].name == "Mine" && again.user()[1].name == "Second one");
    Layout expect = sample("Mine");
    CHECK(again.user()[0] == expect);
    CHECK(body(book.user()) == body(again.user()));   // the same state always the same text
    // all() lists the stock layouts first
    const auto all = again.all();
    CHECK(all.size() == 4 && all[0].name == "Airband" && all[1].name == "HF DX" && all[2].name == "Mine");
}

void saveRules() {
    LayoutBook book;
    book.load("");
    CHECK(book.save("", sample("")) == SaveResult::BadName);
    CHECK(book.save(" ; ", sample("")) == SaveResult::BadName);
    CHECK(book.save("airband", sample("")) == SaveResult::ReservedName);   // a stock layout's name, any case
    CHECK(book.save("HF dx", sample("")) == SaveResult::ReservedName);
    CHECK(book.user().empty() && !book.dirty());
    CHECK(book.save("Mine", sample("")) == SaveResult::Saved && book.dirty());
    book.markWritten();
    Layout other = sample("");
    other.bank = 1;
    CHECK(book.save("MINE", other) == SaveResult::Replaced);   // the same name replaces, in any case, and keeps one layout
    CHECK(book.user().size() == 1 && book.user()[0].bank == 1 && book.user()[0].name == "MINE" && book.dirty());
    for (std::size_t i = 1; i < kMaxUserLayouts; ++i) { CHECK(book.save("L" + std::to_string(i), sample("")) == SaveResult::Saved); }
    CHECK(book.user().size() == kMaxUserLayouts);
    CHECK(book.save("One more", sample("")) == SaveResult::Full);
    CHECK(book.save("L3", sample("")) == SaveResult::Replaced);   // a full book still replaces
    // what a saved layout holds is clamped
    Layout wild = sample("");
    wild.bank = 99; wild.splitPct = 500; wild.workspace = -4; wild.selected = 40; wild.mode = 9; wild.sections = 0xFFFFFFFFu;
    LayoutBook b2;
    b2.load("");
    CHECK(b2.save("Wild", wild) == SaveResult::Saved);
    const Layout& w = b2.user()[0];
    CHECK(w.bank == 4 && w.splitPct == 90 && w.workspace == 0 && w.selected == 8 && w.mode == 7 && w.sections == 0x3FFu);
}

void renameRemove() {
    LayoutBook book;
    book.load("");
    (void)book.save("A", sample(""));
    (void)book.save("B", sample(""));
    CHECK(book.rename("A", "C") == SaveResult::Saved && book.user()[0].name == "C");
    CHECK(book.rename("C", "b") == SaveResult::ReservedName);       // another layout's name (any case)
    CHECK(book.rename("C", "airband") == SaveResult::ReservedName);  // a stock name
    CHECK(book.rename("C", "") == SaveResult::BadName);
    CHECK(book.rename("nothing", "X") == SaveResult::BadName);
    CHECK(book.rename("Airband", "X") == SaveResult::BadName);       // a stock layout is not renamed (it is not among the user's)
    CHECK(book.user()[0].name == "C" && book.user()[1].name == "B");
    CHECK(book.remove("c") && book.user().size() == 1);
    CHECK(!book.remove("c") && !book.remove("Airband"));              // gone already; a stock layout cannot be removed
    CHECK(book.find("airband") != nullptr && book.find("B") != nullptr && book.find("C") == nullptr);
}

void oldAndNewFiles() {
    // an older settings file: no entry, the 3D views and the workspace as they were
    LayoutBook book;
    book.load("waterfall:on=1,az=-58.00,el=32.00,zoom=1.00;workspace:lay=2,pg=1,sel=3,lk=1,ex=4,sp=45.45.45.45.45.45.45.45");
    CHECK(book.user().empty() && book.all().size() == 2);
    // saving a layout keeps both of them, byte for byte in what they mean
    (void)book.save("Mine", sample(""));
    const std::string text = book.text("waterfall:on=1,az=-58.00,el=32.00,zoom=1.00;workspace:lay=2,pg=1,sel=3,lk=1,ex=4,sp=45.45.45.45.45.45.45.45");
    foxsdr::slintview3d::ViewPrefs p = foxsdr::slintview3d::ViewPrefs::parse(text);
    CHECK(p.get("waterfall", {}).on);
    const foxsdr::slinthost::SavedWorkspace w = foxsdr::slinthost::workspaceFromSettings(text);
    CHECK(w.present && w.layout == 2 && w.page == 1 && w.selected == 3 && w.linkOn);
    // removing the last layout takes the entry out (the settings text does not keep an empty one)
    LayoutBook b2;
    b2.load(text);
    CHECK(b2.user().size() == 1);
    CHECK(b2.remove("Mine"));
    CHECK(b2.text(text).find("layouts:") == std::string::npos);
    // a newer file: unknown keys are skipped, a bad number keeps the default, an entry without a name is dropped, a stock name is dropped
    LayoutBook b3;
    b3.load("layouts:n=Fut,b=1,zz=9,s=notanumber,p=5000|b=2|n=Airband,b=3|n=Ok,w=2");
    CHECK(b3.user().size() == 2 && b3.user()[0].name == "Fut" && b3.user()[0].bank == 1 && b3.user()[0].sections == 0 && b3.user()[0].splitPct == 90);
    CHECK(b3.user()[1].name == "Ok" && b3.user()[1].workspace == 2);
    // more than the limit in the file: the first kMaxUserLayouts
    std::string many = "layouts:";
    for (int i = 0; i < 20; ++i) { many += (i ? "|n=L" : "n=L") + std::to_string(i); }
    LayoutBook b4;
    b4.load(many);
    CHECK(b4.user().size() == kMaxUserLayouts);
    // the entry's size is bounded by the interface half's text (config.cpp reads at most 2048 bytes of it)
    LayoutBook b5;
    b5.load("");
    for (std::size_t i = 0; i < kMaxUserLayouts; ++i) { Layout l = sample(""); l.tuneHz = 9999999999LL; (void)b5.save(std::string(kMaxNameBytes, static_cast<char>('a' + i)), l); }
    const std::string full = b5.text("");
    CHECK(full.size() <= foxsdr::slintview3d::kRawLayoutsLimit + 9 && full.rfind("layouts:", 0) == 0);
    LayoutBook b6;
    b6.load(full);
    CHECK(b6.user().size() == kMaxUserLayouts);
}

void order() {
    const std::vector<Step> s = stepsOf(builtins()[0]);
    const auto at = [&](Step x) { return std::find(s.begin(), s.end(), x) - s.begin(); };
    CHECK(at(Step::Bank) < at(Step::Sections));            // the sections are the bank's
    CHECK(at(Step::View) < at(Step::Split));               // a split is the receiver tab's
    CHECK(at(Step::Workspace) < at(Step::Wf3d));
    CHECK(std::find(s.begin(), s.end(), Step::Tuning) == s.end() && std::find(s.begin(), s.end(), Step::Selection) == s.end());   // stock: no tuning, no radio picked
    const std::vector<Step> t = stepsOf(sample("x"));
    CHECK(std::find(t.begin(), t.end(), Step::Tuning) == t.end() - 1);   // the tuning is last
    CHECK((std::find(t.begin(), t.end(), Step::Workspace) - t.begin()) < (std::find(t.begin(), t.end(), Step::Selection) - t.begin()));   // the layout before the radio picked
}

// A fake window: every action is recorded, in order, as a word. The Actions struct has NO member that starts or stops anything, so the record is the whole of what a layout can do.
struct Fake {
    std::vector<std::string> calls;
    Actions actions() {
        Actions a;
        a.setBank = [this](int b) { calls.push_back("bank" + std::to_string(b)); };
        a.setSections = [this](int b, std::uint32_t m) { calls.push_back("sections" + std::to_string(b) + ":" + std::to_string(m)); };
        a.setRail = [this](bool f) { calls.push_back(f ? "rail-fold" : "rail-open"); };
        a.setView = [this](int v) { calls.push_back("view" + std::to_string(v)); };
        a.setSplitPct = [this](int p) { calls.push_back("split" + std::to_string(p)); };
        a.setWorkspace = [this](int w) { calls.push_back("workspace" + std::to_string(w)); };
        a.setLink = [this](bool l) { calls.push_back(l ? "link-on" : "link-off"); };
        a.selectRadio = [this](int r) { calls.push_back("select" + std::to_string(r)); };
        a.setWf3d = [this](bool o) { calls.push_back(o ? "wf3d-on" : "wf3d-off"); };
        a.tune = [this](std::int64_t hz) { calls.push_back("tune" + std::to_string(hz)); };
        a.setMode = [this](int m) { calls.push_back("mode" + std::to_string(m)); };
        return a;
    }
    long at(const std::string& what) const { const auto it = std::find(calls.begin(), calls.end(), what); return it == calls.end() ? -1 : it - calls.begin(); }
};

Window windowNow() {
    Window w;
    w.bank = 0; w.sections = 0b110; w.railFolded = false; w.view = 0; w.splitPct = 60; w.workspace = 0; w.link = false; w.wf3d = false; w.selected = 1; w.openRadios = 1; w.tunedHz = 100300000; w.mode = 2;
    return w;
}

void applyOrder() {
    Fake f;
    const int held = apply(builtins()[0], windowNow(), f.actions());   // Airband on a one-radio window
    CHECK(held == 0);
    CHECK(f.at("bank2") == 0 && f.at("sections2:16") == 1);   // the bank, then ITS sections (bit 4 = Airband)
    CHECK(f.at("split40") > f.at("sections2:16") && f.at("workspace1") > f.at("split40"));
    CHECK(f.at("view0") < 0 && f.at("rail-open") < 0 && f.at("link-on") < 0);   // already so: not done again
    // the stock layouts never retune, never pick a radio and never touch the 3D waterfall unless it differs
    for (const std::string& c : f.calls) { CHECK(c.rfind("tune", 0) != 0 && c.rfind("mode", 0) != 0 && c.rfind("select", 0) != 0); }
    // a user layout with tuning and a selection, on a window with three radios open
    Window w3 = windowNow();
    w3.openRadios = 0b111;
    Fake g;
    Layout l = sample("x");
    CHECK(apply(l, w3, g.actions()) == 0);
    CHECK(g.at("bank2") == 0 && g.at("sections2:20") == 1 && g.at("rail-fold") > 1 && g.at("view1") > 1 && g.at("split33") > g.at("view1"));
    CHECK(g.at("workspace2") > g.at("split33") && g.at("link-on") > g.at("workspace2") && g.at("select3") > g.at("workspace2") && g.at("wf3d-on") > g.at("select3"));
    CHECK(g.at("tune7074000") > g.at("wf3d-on") && g.at("mode4") == g.at("tune7074000") + 1 && g.calls.back() == "mode4");   // the tuning is last
}

void applyDegrades() {
    // the radio the layout wants selected is not open: the selection is left alone, everything else is done, and the answer says so
    Window w = windowNow();
    w.openRadios = 0b1;
    Fake f;
    Layout l = sample("x");
    l.selected = 3;
    CHECK((apply(l, w, f.actions()) & kHeldSelection) != 0 && f.at("select3") < 0 && f.at("workspace2") >= 0);
    // THE PATCH IS RUNNING: leaving its tab would stop it, so the tab is left alone (the rest is done) - a layout never stops anything
    Window p = windowNow();
    p.view = 1;
    p.patchRunning = true;
    Fake g;
    Layout toReceiver = builtins()[1];
    CHECK((apply(toReceiver, p, g.actions()) & kHeldView) != 0 && g.at("view0") < 0 && g.at("bank0") >= 0);
    // the same layout with no patch running does go to the tab
    p.patchRunning = false;
    Fake h;
    CHECK(apply(toReceiver, p, h.actions()) == 0 && h.at("view0") >= 0);
    // staying on the PATCH tab while it runs is no hold
    Fake k;
    Layout stay = sample("s");
    stay.view = 1;
    Window running = windowNow();
    running.view = 1;
    running.patchRunning = true;
    running.openRadios = 0b111;   // (the layout names radio 3)
    CHECK(apply(stay, running, k.actions()) == 0 && k.at("view1") < 0);
    // an empty Actions does nothing and does not crash
    CHECK(apply(l, w, Actions{}) == kHeldSelection);
}

void captureWindow() {
    Window w = windowNow();
    w.bank = 3; w.sections = 0b11; w.railFolded = true; w.view = 2; w.splitPct = 33; w.workspace = 1; w.link = true; w.wf3d = true; w.selected = 2; w.tunedHz = 7074000; w.mode = 5;
    const Layout plain = capture(w, false);
    CHECK(plain.bank == 3 && plain.sections == 0b11 && plain.railFolded && plain.view == 2 && plain.splitPct == 33 && plain.workspace == 1 && plain.link && plain.wf3d && plain.selected == 2);
    CHECK(!plain.tuning && plain.tuneHz == 0);
    const Layout tuned = capture(w, true);
    CHECK(tuned.tuning && tuned.tuneHz == 7074000 && tuned.mode == 5);
    // a plugin page (view 3 and up) is not part of a layout
    w.view = 5;
    CHECK(capture(w, false).view == 0);
    // what was captured is what is applied: capture, save, load, apply reproduces the window
    Window src = windowNow();
    src.bank = 4; src.sections = 0b101; src.railFolded = true; src.splitPct = 70; src.workspace = 2; src.link = true; src.selected = 2; src.openRadios = 0b11;
    LayoutBook book;
    book.load("");
    CHECK(book.save("Round trip", capture(src, false)) == SaveResult::Saved);
    LayoutBook again;
    again.load(book.text(""));
    Fake f;
    Window dst = windowNow();
    dst.openRadios = 0b11;
    CHECK(apply(*again.find("round trip"), dst, f.actions()) == 0);
    CHECK(f.at("bank4") == 0 && f.at("sections4:5") == 1 && f.at("rail-fold") >= 0 && f.at("split70") >= 0 && f.at("workspace2") >= 0 && f.at("link-on") >= 0);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("stock\n"); stock();
    std::printf("names\n"); names();
    std::printf("roundTrip\n"); roundTrip();
    std::printf("saveRules\n"); saveRules();
    std::printf("renameRemove\n"); renameRemove();
    std::printf("oldAndNewFiles\n"); oldAndNewFiles();
    std::printf("order\n"); order();
    std::printf("apply\n"); applyOrder(); applyDegrades(); captureWindow();
    return testSummary("test_slint_host_layouts");
}
