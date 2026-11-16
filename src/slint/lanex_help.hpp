// lanex_help.hpp - LANE X, row G5-14: the help that opens INSIDE the window (docs/multi-radio-records/lane-x.md). Pure: no Slint, no engine, no network.
//
// The topics are written here, in the program, so help works with no network and no file beside the program. Each sentence is either the text of a control's own tooltip (the product already
// shows it, so it is verified by being there) or a short sentence checked against the code that is named in the record's table. A topic that could not be checked is not here: the record lists it.
//
// WHERE THE USER IS: a section of a bank has at most one topic (topicForSection); the window remembers the section last opened and the help opens at its topic.
// SEARCH: over the title, the sentences and a few English key words of every topic, in the language in force, case-insensitive, every word of the query must be found; title hits first.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <string>
#include <vector>

namespace foxsdr::slintlanex {

struct HelpTopic {
    const char* id;                      // stable, ASCII
    const char* title;                   // English; the window shows it through the translation call
    std::vector<const char*> body;       // English sentences, each one its own translated string
    int bank;                            // -1 = not tied to a section; 0 SIGNAL, 1 DECODE, 2 TOOLS, 3 EXTEND, 4 SYSTEM
    int row;                             // the logical row of the section in that bank (the window's own numbering); -1 = none
    const char* keywords;                // English key words, space separated (not shown)
};

const std::vector<HelpTopic>& helpTopics();

// The index into helpTopics() of the topic for a section, -1 when the section has none.
int topicForSection(int bank, int row);
// The index of a topic by id, -1 when there is none.
int topicIndex(const std::string& id);

// The translated sentence with its line breaks turned into spaces (a tooltip's own breaks are for the tooltip's width).
std::string sentence(const char* english);
std::string titleOf(const HelpTopic& t);

// The topics that answer a query, best first. An empty query answers every topic in the table's order. Every word of the query must be found in the topic's title, sentences or key words.
std::vector<int> searchTopics(const std::string& query);

}  // namespace foxsdr::slintlanex
