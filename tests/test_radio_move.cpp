// test_radio_move.cpp - FIELD FIX A2 (2026-11-19): the window's side of the radio hand-off (slint/radio_move_host.hpp) and the sheet's words (core::handoff::sheetFor). No window, no pipe.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <string>
#include <vector>

#include "core/radio_handoff.hpp"
#include "slint/radio_move_host.hpp"
#include "test_check.hpp"

namespace ho = cascade::core::handoff;

int main() {
    // --- the words ---------------------------------------------------------------------------------------------------------------------------------------
    {
        const ho::Sheet ask = ho::sheetFor("ask", "NESDR SMArt v5", "OICE", "");
        CHECK(ask.title == "Radio in use by another FoxSDR window");
        CHECK(ask.text.find("NESDR SMArt v5 (serial ending OICE) is in use by another FoxSDR window") == 0);
        CHECK(ask.goLabel == "Move it here" && ask.stayLabel == "Leave it there");
        const ho::Sheet other = ho::sheetFor("other", "NESDR SMArt v5", "OICE", "");
        CHECK(other.text.find("is in use by another program") != std::string::npos);
        CHECK(other.goLabel.empty() && other.stayLabel == "OK");   // no move offered for another program
        CHECK(other.text.find("FoxSDR window") == std::string::npos);
        const ho::Sheet refused = ho::sheetFor("refused", "NESDR SMArt v5", "OICE", "recording");
        CHECK(refused.text.find("is recording in the other FoxSDR window") != std::string::npos);
        CHECK(ho::sheetFor("timeout", "X", "1234", "").text.find("did not answer in time") != std::string::npos);
        CHECK(ho::sheetFor("moved", "X", "1234", "").text.find("was moved to another FoxSDR window") != std::string::npos);
        CHECK(!ho::sheetFor("", "X", "1234", "").shown());
        CHECK(ho::sheetFor("ask", "A", "0001", "").text.find('%') == std::string::npos);
    }

    // --- the host: shown once per question, the keys answer ---------------------------------------------------------------------------------------------
    {
        std::vector<std::string> log;
        foxsdr::slinthost::RadioMoveHost host({[&](const ho::Sheet& s) { log.push_back("show:" + s.title); }, [&] { log.push_back("hide"); },
                                               [&](bool move) { log.push_back(move ? "submit:1" : "submit:0"); }});
        const std::string ask = "1\task\tNESDR SMArt v5\tOICE\t";
        host.poll(ask, "");
        CHECK(host.open() && host.shown() == 1);
        for (int i = 0; i < 20; ++i) { host.poll(ask, ""); }   // the fact stays set tick after tick: no second sheet
        CHECK(host.shown() == 1);
        host.confirm();   // "Move it here"
        CHECK(!host.open());
        CHECK(log.back() == "submit:1");
        host.poll(ask, "");   // a stale fact for the same question does not reopen it
        CHECK(host.shown() == 1);
        host.poll("", "");
        // a NEW failed open is a new sequence: shown once more; "Leave it there" sends 0
        host.poll("2\task\tNESDR SMArt v5\tOICE\t", "");
        CHECK(host.open() && host.shown() == 2);
        host.cancel();
        CHECK(log.back() == "submit:0");
        // the other-program sentence: one key, which only puts the question away
        host.poll("3\tother\tNESDR SMArt v5\tOICE\t", "");
        CHECK(host.open() && host.shown() == 3);
        host.confirm();
        CHECK(log.back() == "submit:0");
        // the engine puts the question away itself (the move began): the open sheet goes with it
        host.poll("4\task\tNESDR SMArt v5\tOICE\t", "");
        CHECK(host.open());
        host.poll("", "");
        CHECK(!host.open());
        // the notice on the window that let the radio go: shown once, answers nothing
        const std::size_t before = log.size();
        host.poll("", "1\tmoved\tNESDR SMArt v5\tOICE\t");
        CHECK(host.open());
        host.poll("", "1\tmoved\tNESDR SMArt v5\tOICE\t");
        host.confirm();
        CHECK(!host.open());
        bool submitted = false;
        for (std::size_t i = before; i < log.size(); ++i) { submitted = submitted || log[i].rfind("submit", 0) == 0; }
        CHECK(!submitted);
    }
    return testSummary("test_radio_move");
}
