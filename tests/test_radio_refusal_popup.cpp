// test_radio_refusal_popup.cpp - A REFUSED RADIO CHOICE SAYS WHY (0.99.113): the data a refusal carries, the popup's words and the state machine both windows drive
// (core/radio_refusal_popup.hpp). No window, no engine: the engine half is tests/test_engine_api_source_refusal.cpp; the classic window's wiring is
// tests/test_refusal_popup_app.cpp; the new window's Source error line is src/slint/tests/test_slint_host_signal.cpp.
//
//   B  THE DATA: a patch-part refusal is recognised from applied[0]/applied[1] and the status, never from words; a receiver-held refusal, a transmitter-held one, an
//      answer that is not BUSY, an answer of another op, and a BUSY that was not a refusal are NOT patch-part refusals.
//   C  THE POPUP: raised once for the answer to a ticket THIS window noted, when that answer is a patch-part refusal - and for nothing else: not for a receiver-held refusal,
//      not for another kind of refusal, not for an answer to a ticket never noted (another session's command, a script, a remote client). "Open the Patch page" opens the
//      page and closes it; "Close" closes it and opens nothing; neither does anything else (the hooks are the whole of what a key can reach). A second refused click shows
//      it again; a gate that says no keeps it away.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstring>
#include <string>

#include "test_check.hpp"
#include "core/radio_refusal_popup.hpp"
#include "foxsdr_api.h"

#define CHECK_EQ(a, b) CHECK((a) == (b))

namespace rf = cascade::core::refusal;

namespace {

FoxCommandResult answer(std::uint32_t op, std::int32_t status, std::uint64_t ticket, int holderKind, int number) {
    FoxCommandResult r{};
    r.structSize = sizeof(r);
    r.op = op;
    r.status = status;
    r.ticket = ticket;
    r.flags = status == FOXAPI_OK ? 0u : FOXAPI_RESULT_REFUSED;
    r.applied[0] = holderKind;
    r.applied[1] = number;
    std::snprintf(r.message, sizeof(r.message), "%s", status == FOXAPI_OK ? "" : "in use by something");
    return r;
}

struct Hooked {
    int shows = 0, hides = 0, patchPages = 0;
    bool gate = true;
    rf::Popup last;
    rf::RefusalPopup popup{rf::RefusalPopup::Hooks{[this] { return gate; }, [this](const rf::Popup& p) { ++shows; last = p; }, [this] { ++hides; }, [this] { ++patchPages; }}};
};

void testThePopupWords() {
    const rf::Popup p = rf::popupFor("B200 (serial ending 4ZDB)", 1);
    CHECK_EQ(p.title, std::string("Radio in use"));
    CHECK_EQ(p.text, std::string("B200 (serial ending 4ZDB) is set as the radio of Radio part 1 on the Patch page. Change or remove that part there, or start the patch to use the radio there."));
    CHECK_EQ(p.goLabel, std::string("Open the Patch page"));
    CHECK_EQ(p.stayLabel, std::string("Close"));
    CHECK(rf::popupFor("", 2).text.rfind("This radio is set as the radio of Radio part 2", 0) == 0);
}

void testTheData() {
    int part = -1;
    CHECK(rf::isPatchPartRefusal(answer(FOXAPI_OP_SELECT_SOURCE, FOXAPI_BUSY, 1, rf::kHolderPatchPart, 4), part) && part == 4);
    CHECK(rf::isPatchPartRefusal(answer(FOXAPI_OP_RADIO_ADD, FOXAPI_BUSY, 1, rf::kHolderPatchPart, 2), part) && part == 2);
    CHECK(!rf::isPatchPartRefusal(answer(FOXAPI_OP_SELECT_SOURCE, FOXAPI_BUSY, 1, rf::kHolderReceiver, 2), part));
    CHECK(!rf::isPatchPartRefusal(answer(FOXAPI_OP_SELECT_SOURCE, FOXAPI_BUSY, 1, rf::kHolderTransmitter, 0), part));
    CHECK(!rf::isPatchPartRefusal(answer(FOXAPI_OP_SELECT_SOURCE, FOXAPI_NOT_FOUND, 1, rf::kHolderPatchPart, 4), part));
    CHECK(!rf::isPatchPartRefusal(answer(FOXAPI_OP_SELECT_SOURCE, FOXAPI_OK, 1, rf::kHolderPatchPart, 4), part));
    CHECK(!rf::isPatchPartRefusal(answer(FOXAPI_OP_SET_GAIN, FOXAPI_BUSY, 1, rf::kHolderPatchPart, 4), part));
    FoxCommandResult notRefused = answer(FOXAPI_OP_SELECT_SOURCE, FOXAPI_BUSY, 1, rf::kHolderPatchPart, 4);
    notRefused.flags = 0;
    CHECK(!rf::isPatchPartRefusal(notRefused, part));
    // never from words: a message that names a patch radio but carries no data is not a patch-part refusal
    FoxCommandResult words = answer(FOXAPI_OP_SELECT_SOURCE, FOXAPI_BUSY, 1, rf::kHolderNone, 0);
    std::snprintf(words.message, sizeof(words.message), "%s", "in use by patch radio 3");
    CHECK(!rf::isPatchPartRefusal(words, part));
}

void testThePopup() {
    {   // appears for the patch-part refusal of this window's own click, once
        Hooked h;
        h.popup.noteSubmitted(7, "B200 EDR04ZDB2");
        CHECK(h.popup.onAnswer(answer(FOXAPI_OP_SELECT_SOURCE, FOXAPI_BUSY, 7, rf::kHolderPatchPart, 1)));
        CHECK(h.popup.open() && h.popup.part() == 1 && h.shows == 1);
        CHECK(h.last.text.find("B200 EDR04ZDB2") == 0 && h.last.text.find("Radio part 1") != std::string::npos);
        // the same answer is not shown again (its ticket was spent)
        CHECK(!h.popup.onAnswer(answer(FOXAPI_OP_SELECT_SOURCE, FOXAPI_BUSY, 7, rf::kHolderPatchPart, 1)));
        CHECK_EQ(h.shows, 1);
        // a second click that is refused again may show it again
        h.popup.dismiss();
        h.popup.noteSubmitted(8, "B200 EDR04ZDB2");
        h.popup.onAnswer(answer(FOXAPI_OP_SELECT_SOURCE, FOXAPI_BUSY, 8, rf::kHolderPatchPart, 1));
        CHECK_EQ(h.shows, 2);
        CHECK_EQ(h.popup.shown(), 2);
    }
    {   // not for a receiver-held refusal, the transmitter, another refusal, an accepted choice
        Hooked h;
        h.popup.noteSubmitted(1, "x");
        h.popup.onAnswer(answer(FOXAPI_OP_SELECT_SOURCE, FOXAPI_BUSY, 1, rf::kHolderReceiver, 2));
        h.popup.noteSubmitted(2, "x");
        h.popup.onAnswer(answer(FOXAPI_OP_SELECT_SOURCE, FOXAPI_BUSY, 2, rf::kHolderTransmitter, 0));
        h.popup.noteSubmitted(3, "x");
        h.popup.onAnswer(answer(FOXAPI_OP_SELECT_SOURCE, FOXAPI_NOT_FOUND, 3, rf::kHolderNone, 0));
        h.popup.noteSubmitted(4, "x");
        h.popup.onAnswer(answer(FOXAPI_OP_SELECT_SOURCE, FOXAPI_OK, 4, rf::kHolderNone, 0));
        CHECK(!h.popup.open() && h.shows == 0 && h.popup.shown() == 0);
    }
    {   // not for an answer to a different session's (or a script's) command: its ticket was never noted here
        Hooked h;
        h.popup.noteSubmitted(5, "x");
        CHECK(!h.popup.onAnswer(answer(FOXAPI_OP_SELECT_SOURCE, FOXAPI_BUSY, 99, rf::kHolderPatchPart, 1)));
        CHECK(!h.popup.open() && h.shows == 0);
        h.popup.noteSubmitted(0, "x");   // a submit that was not taken (ticket 0) notes nothing
        CHECK(!h.popup.onAnswer(answer(FOXAPI_OP_SELECT_SOURCE, FOXAPI_BUSY, 0, rf::kHolderPatchPart, 1)));
        CHECK(h.shows == 0);
    }
    {   // "Open the Patch page": the page opens, the popup closes; nothing else
        Hooked h;
        h.popup.noteSubmitted(1, "x");
        h.popup.onAnswer(answer(FOXAPI_OP_SELECT_SOURCE, FOXAPI_BUSY, 1, rf::kHolderPatchPart, 1));
        h.popup.goToPatchPage();
        CHECK(!h.popup.open() && h.patchPages == 1 && h.hides == 1);
        h.popup.goToPatchPage();   // closed already: a second press does nothing
        CHECK(h.patchPages == 1 && h.hides == 1);
    }
    {   // "Close": closes, opens nothing
        Hooked h;
        h.popup.noteSubmitted(1, "x");
        h.popup.onAnswer(answer(FOXAPI_OP_SELECT_SOURCE, FOXAPI_BUSY, 1, rf::kHolderPatchPart, 1));
        h.popup.dismiss();
        CHECK(!h.popup.open() && h.patchPages == 0 && h.hides == 1);
    }
    {   // the gate: a window that may not show it never does
        Hooked h;
        h.gate = false;
        h.popup.noteSubmitted(1, "x");
        CHECK(h.popup.onAnswer(answer(FOXAPI_OP_SELECT_SOURCE, FOXAPI_BUSY, 1, rf::kHolderPatchPart, 1)));
        CHECK(!h.popup.open() && h.shows == 0);
    }
    {   // a refused add (Radio 2-8) for a patch part's device raises it the same way
        Hooked h;
        h.popup.noteSubmitted(3, "RTL-SDR");
        h.popup.onAnswer(answer(FOXAPI_OP_RADIO_ADD, FOXAPI_BUSY, 3, rf::kHolderPatchPart, 2));
        CHECK(h.popup.open() && h.popup.part() == 2);
    }
}

}  // namespace

int main() {
    std::printf("test_radio_refusal\n");
    testThePopupWords();
    testTheData();
    testThePopup();
    return testSummary("test_radio_refusal_popup");
}
