// test_torn_off_ledger.cpp - the ledger of pages in their own windows
// (gui/torn_off_ledger.hpp, 0.99.103): a page's own window is logged once when it
// appears, with its page, size, place, monitor and the session's count; once when
// it goes; never while it just stays; the id in the line is the viewport's, the one
// the slow-frame line names; and the pop-up windows ImGui makes over such pages
// are counted and summarised, never listed one by one.
//
// THE CLASS, NOT THE WINDOW: the frame loop's collector (AppWindow::noteTornOffWindows)
// needs ImGui's viewport list and a display, and what it hands over is this struct.
//
// BREAK-IT CHECKS, run while writing this: with the `find(w.id) >= 0` test deleted
// from update() the "a window that stays is said once" block goes red at the second
// update (a second page line); with the gone-first order reversed the "two at once"
// block's `outside` counts go red; the line-length block was red before the names
// were capped and the wording cut (230 characters against the log's 192); and with
// the pop-up gap ignored the "pop-ups are counted" block goes red on its third blink.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <string>
#include <vector>

#include "gui/torn_off_ledger.hpp"
#include "test_check.hpp"

using cascade::gui::TornOffLedger;
using cascade::gui::TornOffWindow;

namespace {

TornOffWindow page(std::uint32_t id, const char* name, float x, float y, float w, float h,
                   int monitor = 0, int monitors = 1, const char* monitorName = "Laptop panel",
                   float dpi = 1.25f) {
    TornOffWindow t;
    t.id = id;
    t.name = name;
    t.nameLen = static_cast<int>(std::char_traits<char>::length(name));
    t.x = x;
    t.y = y;
    t.w = w;
    t.h = h;
    t.monitor = monitor;
    t.monitors = monitors;
    t.monitorName = monitorName;
    t.dpiScale = dpi;
    return t;
}

TornOffWindow popup(std::uint32_t id) {
    TornOffWindow t = page(id, "##Tooltip_00", 0, 0, 200, 80);
    t.popup = true;
    return t;
}

bool has(const std::vector<std::string>& lines, const std::string& what) {
    for (const std::string& l : lines) {
        if (l.find(what) != std::string::npos) { return true; }
    }
    return false;
}

int count(const std::vector<std::string>& lines, const std::string& what) {
    int n = 0;
    for (const std::string& l : lines) {
        if (l.find(what) != std::string::npos) { ++n; }
    }
    return n;
}

void print(const std::vector<std::string>& lines) {
    for (const std::string& l : lines) { std::printf("    %s\n", l.c_str()); }
}

}  // namespace

int main() {
    std::printf("test_torn_off_ledger\n");

    // --- nothing outside: nothing said, however often it is asked ---------------
    {
        TornOffLedger ledger;
        std::vector<std::string> lines;
        CHECK(ledger.update(nullptr, 0, 1.0, lines) == 0);
        CHECK(ledger.update(nullptr, 0, 2.0, lines) == 0);
        CHECK(lines.empty());
        CHECK(ledger.outside() == 0);
        CHECK(ledger.ownWindows() == 0);
        CHECK(ledger.popupsMade() == 0);
    }

    // --- a page in its own window: one line, with everything the tester's log lacked
    {
        TornOffLedger ledger;
        std::vector<std::string> lines;
        const TornOffWindow w = page(0x8a3f2b1cu, "ADS-B Map", 588.0f, 202.0f, 1026.0f, 699.0f, 1, 2,
                                     "DELL U2720Q", 1.0f);
        CHECK(ledger.update(&w, 1, 10.0, lines) == 1);
        print(lines);
        CHECK(lines.size() == 1);
        if (!lines.empty()) {
            CHECK(lines[0] ==
                  "viewports: page \"ADS-B Map\" in its own window 0x8a3f2b1c - 1026x699 at 588,202 "
                  "on monitor 2 of 2 (DELL U2720Q, 100%); window 1 this session, 1 outside");
        }
        CHECK(ledger.outside() == 1);
        CHECK(ledger.ownWindows() == 1);

        // A window that stays is said once: the next hundred frames add nothing.
        for (int i = 0; i < 100; ++i) { CHECK(ledger.update(&w, 1, 10.0 + i, lines) == 0); }
        CHECK(lines.size() == 1);

        // Gone: the page is back inside, by name and id, and the count follows.
        CHECK(ledger.update(nullptr, 0, 120.0, lines) == 1);
        print(lines);
        CHECK(lines.size() == 2);
        if (lines.size() == 2) {
            CHECK(lines[1] ==
                  "viewports: page \"ADS-B Map\" back in the main window (window 0x8a3f2b1c gone); 0 "
                  "outside");
        }
        CHECK(ledger.outside() == 0);
        CHECK(ledger.ownWindows() == 1);  // a merge-back is not a new window

        // Given a window again: counted again, as the second of the session.
        CHECK(ledger.update(&w, 1, 130.0, lines) == 1);
        CHECK(has(lines, "window 2 this session, 1 outside"));
        CHECK(ledger.ownWindows() == 2);
    }

    // --- the title's visible half only, and the dpi as a percentage ---------------
    {
        TornOffLedger ledger;
        std::vector<std::string> lines;
        TornOffWindow w = page(7u, "Radar Sweep###radar-sweep-page", -84.0f, -108.0f, 520.0f, 300.0f, 0,
                               2, "Laptop panel", 1.25f);
        w.nameLen = 11;  // the caller cuts at "###"; the ledger copies exactly nameLen
        ledger.update(&w, 1, 1.0, lines);
        print(lines);
        CHECK(has(lines, "page \"Radar Sweep\" in its own window"));
        CHECK(!has(lines, "###"));
        CHECK(has(lines, "520x300 at -84,-108 on monitor 1 of 2 (Laptop panel, 125%)"));
    }

    // --- a monitor ImGui could not place it on, and one with no name ---------------
    {
        TornOffLedger ledger;
        std::vector<std::string> lines;
        TornOffWindow w = page(9u, "Store", 10.0f, 20.0f, 300.0f, 200.0f, -1, 2, nullptr, 1.0f);
        ledger.update(&w, 1, 1.0, lines);
        print(lines);
        CHECK(has(lines, "at 10,20 on no monitor the interface could place (2 listed)"));
        // An index past the list is the same as none: never read off the end.
        TornOffLedger other;
        lines.clear();
        w.monitor = 2;
        other.update(&w, 1, 1.0, lines);
        CHECK(has(lines, "on no monitor the interface could place (2 listed)"));
        // A monitor in the list with no name is still placed.
        TornOffLedger third;
        lines.clear();
        w.monitor = 1;
        third.update(&w, 1, 1.0, lines);
        CHECK(has(lines, "on monitor 2 of 2 (unnamed, 100%)"));
    }

    // --- two at once, then one gone: the gone one is said first, counts right ------
    {
        TornOffLedger ledger;
        std::vector<std::string> lines;
        const TornOffWindow both[2] = {page(1u, "A", 0, 0, 100, 100), page(2u, "B", 0, 0, 100, 100)};
        CHECK(ledger.update(both, 2, 1.0, lines) == 2);
        CHECK(has(lines, "page \"A\" in its own window") && has(lines, "page \"B\" in its own window"));
        CHECK(has(lines, "window 1 this session, 1 outside"));
        CHECK(has(lines, "window 2 this session, 2 outside"));
        CHECK(ledger.outside() == 2);
        lines.clear();
        // A goes, C arrives in the same frame.
        const TornOffWindow next[2] = {page(2u, "B", 0, 0, 100, 100), page(3u, "C", 0, 0, 100, 100)};
        CHECK(ledger.update(next, 2, 2.0, lines) == 2);
        print(lines);
        CHECK(lines.size() == 2);
        if (lines.size() == 2) {
            CHECK(lines[0].find("page \"A\" back in the main window (window 0x00000001 gone); 1 outside") !=
                  std::string::npos);
            CHECK(lines[1].find("page \"C\" in its own window") != std::string::npos);
            CHECK(lines[1].find("window 3 this session, 2 outside") != std::string::npos);
        }
        CHECK(ledger.outside() == 2);
        CHECK(ledger.ownWindows() == 3);
    }

    // --- POP-UPS ARE COUNTED, NOT LISTED: one summary line, then one per 30 s -----
    {
        TornOffLedger ledger;
        std::vector<std::string> lines;
        const TornOffWindow map = page(0x8a3f2b1cu, "ADS-B Map", 0, 0, 1026, 699);
        ledger.update(&map, 1, 100.0, lines);
        lines.clear();
        // A tooltip appears past the map's window: said at once, as a pop-up, not
        // as a page.
        TornOffWindow frame[2] = {map, popup(0x5001u)};
        CHECK(ledger.update(frame, 2, 100.5, lines) == 1);
        print(lines);
        CHECK(has(lines, "viewports: 1 pop-up window (a tooltip or menu past a page's own window"));
        CHECK(has(lines, "since the last line; 1 this session"));
        CHECK(!has(lines, "page \"##Tooltip_00\""));
        CHECK(ledger.popupsMade() == 1);
        CHECK(ledger.outside() == 1);  // the page, not the pop-up
        // It stays: nothing more. It goes: nothing (a pop-up's end is not news).
        CHECK(ledger.update(frame, 2, 101.0, lines) == 0);
        CHECK(ledger.update(&map, 1, 101.5, lines) == 0);
        // It blinks forty times in the next 20 s - the field cadence - and the ledger
        // says nothing until 30 s have passed since its line.
        lines.clear();
        double t = 102.0;
        for (int i = 0; i < 40; ++i) {
            TornOffWindow blink[2] = {map, popup(0x5001u)};  // the same id each time: ImGui reuses it
            CHECK(ledger.update(blink, 2, t, lines) == 0);
            t += 0.25;
            CHECK(ledger.update(&map, 1, t, lines) == 0);
            t += 0.25;
        }
        CHECK(lines.empty());
        CHECK(ledger.popupsMade() == 41);
        // At 30 s the next blink brings the summary with everything since.
        TornOffWindow blink[2] = {map, popup(0x5001u)};
        CHECK(ledger.update(blink, 2, 131.0, lines) == 1);
        print(lines);
        CHECK(has(lines, "viewports: 41 pop-up windows (a tooltip or menu past a page's own window"));
        CHECK(has(lines, "since the last line; 42 this session"));
        // Two different pop-ups at once are two.
        lines.clear();
        TornOffWindow two[3] = {map, popup(0x5002u), popup(0x5003u)};
        ledger.update(two, 3, 170.0, lines);
        CHECK(has(lines, "viewports: 2 pop-up windows"));
        CHECK(ledger.popupsMade() == 44);
        // A pop-up with the id of a page is still a pop-up, and a page with the id of
        // a pop-up is still a page: the two tables do not share.
        lines.clear();
        TornOffWindow mixed[2] = {map, popup(0x8a3f2b1cu)};
        ledger.update(mixed, 2, 210.0, lines);
        CHECK(ledger.outside() == 1 && ledger.popupsMade() == 45);
    }

    // --- a window with no id is not a window; a name longer than the ledger keeps
    //     is cut, not overrun; more windows than it follows are left out ----------
    {
        TornOffLedger ledger;
        std::vector<std::string> lines;
        const TornOffWindow none = page(0u, "Ghost", 0, 0, 1, 1);
        CHECK(ledger.update(&none, 1, 1.0, lines) == 0);
        CHECK(ledger.outside() == 0);

        std::string longName(200, 'n');
        const TornOffWindow big = page(5u, longName.c_str(), 0, 0, 1, 1);
        CHECK(ledger.update(&big, 1, 1.0, lines) == 1);
        CHECK(lines.size() == 1);
        if (!lines.empty()) {
            CHECK(lines[0].find(std::string(TornOffLedger::kNameChars - 1, 'n') + "\"") !=
                  std::string::npos);
            CHECK(lines[0].find(std::string(TornOffLedger::kNameChars, 'n')) == std::string::npos);
        }

        TornOffLedger full;
        lines.clear();
        std::vector<TornOffWindow> many;
        for (int i = 0; i < TornOffLedger::kMax + 4; ++i) {
            many.push_back(page(static_cast<std::uint32_t>(100 + i), "P", 0, 0, 1, 1));
        }
        CHECK(full.update(many.data(), static_cast<int>(many.size()), 1.0, lines) == TornOffLedger::kMax);
        CHECK(full.outside() == TornOffLedger::kMax);
        // ...and when they all go, exactly the followed ones are said to be back.
        lines.clear();
        CHECK(full.update(nullptr, 0, 2.0, lines) == TornOffLedger::kMax);
        CHECK(full.outside() == 0);
        // Pop-ups past the table are still counted - the count is the point.
        TornOffLedger pops;
        lines.clear();
        std::vector<TornOffWindow> manyPops;
        for (int i = 0; i < TornOffLedger::kMax + 4; ++i) {
            manyPops.push_back(popup(static_cast<std::uint32_t>(900 + i)));
        }
        pops.update(manyPops.data(), static_cast<int>(manyPops.size()), 1.0, lines);
        CHECK(pops.popupsMade() == TornOffLedger::kMax + 4);
    }

    // --- every line fits the log's line, at its widest ------------------------------
    // A 4-digit size and place, a 3-digit scale, 4-digit counts, and both names
    // longer than the ledger keeps: the first run of this test found the line at
    // 230 characters, past the log's 192, with its counts the part cut off.
    {
        TornOffLedger ledger;
        std::vector<std::string> lines;
        TornOffWindow w = page(0xffffffffu, "A page with a long enough name to matter", -2000.0f,
                               -2000.0f, 4096.0f, 2160.0f, 3, 4, "Some monitor with a long name", 3.0f);
        std::vector<TornOffWindow> many;
        for (int i = 0; i < TornOffLedger::kMax - 1; ++i) {
            many.push_back(page(static_cast<std::uint32_t>(1000 + i), "P", 0, 0, 1, 1));
        }
        for (int round = 0; round < 99; ++round) {
            ledger.update(many.data(), static_cast<int>(many.size()), 1.0, lines);
            ledger.update(nullptr, 0, 1.0, lines);
        }
        lines.clear();
        many.push_back(w);
        ledger.update(many.data(), static_cast<int>(many.size()), 1.0, lines);
        ledger.update(nullptr, 0, 1.0, lines);
        // ...and the pop-up summary at its widest: 4-digit counts.
        std::vector<TornOffWindow> pops;
        for (int i = 0; i < 1500; ++i) { pops.push_back(popup(static_cast<std::uint32_t>(10000 + i))); }
        ledger.update(pops.data(), 1500, 1.0, lines);
        std::size_t longest = 0;
        for (const std::string& l : lines) {
            longest = l.size() > longest ? l.size() : longest;
            CHECK(l.size() < 192);
            CHECK(l.rfind("viewports: ", 0) == 0);
        }
        std::printf("    longest line: %zu characters\n", longest);
        CHECK(has(lines, "(Some monitor with a , 300%)"));  // 20 characters of the monitor's name
        CHECK(has(lines, "page \"A page with a long enough n\" in its own window"));  // 27 of the page's
        CHECK(has(lines, "this session, 16 outside"));
        CHECK(has(lines, "viewports: 1500 pop-up windows"));
        CHECK(count(lines, "pop-up window") == 1);
    }

    return testSummary("test_torn_off_ledger");
}
