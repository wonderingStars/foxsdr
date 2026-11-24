// test_handover_sheet.cpp - THE ONE HANDOVER SHEET AND ITS THREE USERS (0.99.113 review fixes M1, S1, S2): the Legacy-window question, the radio hand-off question and the refused-radio
// popup all draw on the new window's one sheet. Plain C++, no window: the REAL RadioMoveHost and RefusalPopup, the real HandoverSheet owner and gate, wired exactly as src/slint/main.cpp
// wires them (the same calls in the same hooks); the Legacy question is a small stand-in with HandoverHost's own state (asking_ set by choose(), a Switch that only counts).
//
//   A  who owns the sheet: a popup then a hand-off question - go/stay reach the hand-off only, the popup is dropped and none of its callbacks fires later, a later Legacy "Switch" does the
//      switch and not the Patch page;
//   B  a hand-off question open, then a refusal answer: nothing is raised, the question stays and its keys are its own;
//   C  the Legacy question open, then a refusal answer: nothing is raised, "Switch" / "Stay here" are the Legacy question's;
//   D  the popup alone: its keys reach it only; a refused ADD raises no popup (an add's ticket is never noted);
//   E  the wiring that cannot run without a window is scanned in the sources (signal_publish.cpp notes the ticket, main.cpp routes through the owner, the add path of main.cpp notes no
//      ticket), so deleting any of it is RED.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "core/radio_refusal_popup.hpp"
#include "foxsdr_api.h"
#include "slint/handover_sheet.hpp"
#include "slint/radio_move_host.hpp"
#include "slint/radio_refusal_gate.hpp"
#include "test_check.hpp"

namespace rf = cascade::core::refusal;
namespace sl = foxsdr::slinthost;
using Who = sl::HandoverSheet::Who;

namespace {

FoxCommandResult refusedAnswer(std::uint32_t op, std::uint64_t ticket) {
    FoxCommandResult r{};
    r.structSize = sizeof(r);
    r.op = op;
    r.status = FOXAPI_BUSY;
    r.ticket = ticket;
    r.flags = FOXAPI_RESULT_REFUSED;
    r.applied[0] = rf::kHolderPatchPart;
    r.applied[1] = 1;
    std::snprintf(r.message, sizeof(r.message), "%s", "in use by a patch part");
    return r;
}

// The window, as main.cpp wires it.
struct Window {
    bool sheetOpen = false;
    std::string title;
    int patchPages = 0, legacySwitches = 0, legacyStays = 0, handoffYes = 0, handoffNo = 0;
    bool legacyAsking = false;   // HandoverHost's asking_
    sl::HandoverSheet sheet;
    sl::RadioMoveHost move{{[this](const cascade::core::handoff::Sheet& s) { sheet.took(Who::Move); title = s.title; sheetOpen = true; },
                            [this] { if (sheet.released(Who::Move)) { sheetOpen = false; } },
                            [this](bool yes) { yes ? ++handoffYes : ++handoffNo; }}};
    rf::RefusalPopup popup{{[this] { return sl::mayShowRefusalPopup(sheet.mayRaiseRefusal() && !move.open()); },
                            [this](const rf::Popup& p) { sheet.took(Who::Refusal); title = p.title; sheetOpen = true; },
                            [this] { if (sheet.released(Who::Refusal)) { sheetOpen = false; } },
                            [this] { ++patchPages; },
                            [](int) { return std::string("Aerial B200"); }}};
    Window() {
        sheet.setUsers({[this] { move.confirm(); }, [this] { move.cancel(); },
                        [this] { if (legacyAsking) { legacyAsking = false; ++legacySwitches; } if (sheet.released(Who::Legacy)) { sheetOpen = false; } },
                        [this] { legacyAsking = false; ++legacyStays; if (sheet.released(Who::Legacy)) { sheetOpen = false; } },
                        [this] { popup.goToPatchPage(); }, [this] { popup.dismiss(); },
                        [this] { popup.superseded(); }});
    }
    void askLegacy() { legacyAsking = true; sheet.took(Who::Legacy); title = "Switch to the Legacy window?"; sheetOpen = true; }   // HandoverHost::choose() with a recording running
    void handoffArrives(unsigned seq) { move.poll(std::to_string(seq) + "\task\tNESDR SMArt v5\tOICE\t", ""); }
    void refusedClick(std::uint64_t ticket) { popup.noteSubmitted(ticket, "B200"); popup.onAnswer(refusedAnswer(FOXAPI_OP_SELECT_SOURCE, ticket)); }
};

std::string slurp(const char* rel) {
    std::string here = __FILE__;
    here = here.substr(0, here.find_last_of("/\\") + 1) + "../" + rel;
    std::ifstream f(here, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void testPopupThenHandoff() {   // A
    Window w;
    w.refusedClick(1);
    CHECK(w.popup.open() && w.sheet.owner() == Who::Refusal && w.sheetOpen && w.popup.shown() == 1);
    w.handoffArrives(7);   // a published fact: the question takes the sheet
    CHECK(w.move.open() && w.sheet.owner() == Who::Move && w.title == "Radio in use by another FoxSDR window");
    CHECK(!w.popup.open());   // the popup is dropped
    w.sheet.go();             // "Move it here"
    CHECK(w.handoffYes == 1 && w.handoffNo == 0 && w.patchPages == 0 && !w.sheetOpen);
    CHECK(!w.popup.open());
    w.popup.goToPatchPage();   // nothing of the dropped popup fires later
    w.popup.dismiss();
    CHECK(w.patchPages == 0);
    // the stale popup used to answer the Legacy question's key: now "Switch" is the Legacy switch
    w.askLegacy();
    w.sheet.go();
    CHECK(w.legacySwitches == 1 && w.patchPages == 0 && !w.legacyAsking && !w.sheetOpen);
    // and "Leave it there" of a hand-off that took the popup's sheet reaches the hand-off only
    Window v;
    v.refusedClick(1);
    v.handoffArrives(8);
    v.sheet.stay();
    CHECK(v.handoffNo == 1 && v.handoffYes == 0 && v.patchPages == 0 && !v.sheetOpen && !v.popup.open());
}

void testHandoffThenRefusal() {   // B
    Window w;
    w.handoffArrives(3);
    CHECK(w.sheet.owner() == Who::Move && w.sheetOpen);
    w.refusedClick(5);   // a late answer to this window's click
    CHECK(!w.popup.open() && w.popup.shown() == 0 && w.sheet.owner() == Who::Move && w.title == "Radio in use by another FoxSDR window");
    w.sheet.go();
    CHECK(w.handoffYes == 1 && w.patchPages == 0 && !w.sheetOpen);
    w.refusedClick(6);   // the question is answered: the popup may be raised again
    CHECK(w.popup.open() && w.popup.shown() == 1);
}

void testLegacyThenRefusal() {   // C
    Window w;
    w.askLegacy();
    w.refusedClick(5);
    CHECK(!w.popup.open() && w.popup.shown() == 0 && w.sheet.owner() == Who::Legacy && w.title == "Switch to the Legacy window?");
    w.sheet.stay();
    CHECK(w.legacyStays == 1 && w.legacySwitches == 0 && w.patchPages == 0 && !w.sheetOpen);
    w.askLegacy();
    w.refusedClick(6);
    w.sheet.go();
    CHECK(w.legacySwitches == 1 && w.patchPages == 0);
}

void testPopupAloneAndAdd() {   // D
    Window w;
    w.refusedClick(1);
    w.sheet.go();   // "Open the Patch page"
    CHECK(w.patchPages == 1 && !w.popup.open() && !w.sheetOpen && w.handoffYes == 0 && w.legacySwitches == 0);
    w.refusedClick(2);
    w.sheet.stay();   // "Close"
    CHECK(w.patchPages == 1 && !w.popup.open() && !w.sheetOpen && w.handoffNo == 0 && w.legacyStays == 0);
    // a refused ADD: main.cpp never notes the add's ticket, so its answer raises nothing
    w.popup.onAnswer(refusedAnswer(FOXAPI_OP_RADIO_ADD, 9));
    CHECK(!w.popup.open() && w.popup.shown() == 2 && !w.sheetOpen);
    // a stray hide of another user does not close the Legacy question's sheet
    w.askLegacy();
    CHECK(!w.sheet.released(Who::Move) && w.sheet.owner() == Who::Legacy);
}

void testSourceWiring() {   // E (needs the window: scanned)
    const std::string main = slurp("src/slint/main.cpp");
    const std::string pub = slurp("src/slint/signal_publish.cpp");
    CHECK(!main.empty() && !pub.empty());
    CHECK(pub.find("if (c.q.c.op == FOXAPI_OP_SELECT_SOURCE && choiceNoted_) { choiceNoted_(ticket, clickedRowLabel_); }") != std::string::npos);
    CHECK(main.find("signalBank.setChoiceNoted([&](std::uint64_t ticket, const std::string& label) { refusalPopup.noteSubmitted(ticket, label); });") != std::string::npos);
    CHECK(main.find("on_go([&] { handoverSheet.go(); })") != std::string::npos && main.find("on_stay([&] { handoverSheet.stay(); })") != std::string::npos);
    CHECK(main.find("handoverSheet.took(foxsdr::slinthost::HandoverSheet::Who::Legacy)") != std::string::npos);
    CHECK(main.find("handoverSheet.took(foxsdr::slinthost::HandoverSheet::Who::Move)") != std::string::npos);
    CHECK(main.find("handoverSheet.took(foxsdr::slinthost::HandoverSheet::Who::Refusal)") != std::string::npos);
    CHECK(main.find("mayShowRefusalPopup(handoverSheet.mayRaiseRefusal()") != std::string::npos);
    // M1: the add path notes no ticket for the popup
    const std::size_t a = main.find("const auto addFromCard = ");
    const std::size_t b = main.find("addSwitchLayout = workspace.radioCount() == 1", a);
    CHECK(a != std::string::npos && b != std::string::npos && b > a);
    CHECK(main.substr(a, b - a).find("refusalPopup.noteSubmitted") == std::string::npos);
}

}  // namespace

int main() {
    std::printf("test_handover_sheet\n");
    testPopupThenHandoff();
    testHandoffThenRefusal();
    testLegacyThenRefusal();
    testPopupAloneAndAdd();
    testSourceWiring();
    return testSummary("test_handover_sheet");
}
