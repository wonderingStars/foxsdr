// LANE X (docs/multi-radio-records/lane-x.md): the pure rules of the in-window help (lanex_help.cpp) and of the DSP speed test's job (lanex_bench.cpp).
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "lanex_bench.hpp"
#include "lanex_help.hpp"
#include "test_check.hpp"

#include <atomic>
#include <chrono>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>

using namespace foxsdr::slintlanex;

namespace {

std::string lowerCase(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

bool waitFor(const std::function<bool()>& p, int ms = 5000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (p()) { return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return p();
}

BenchItem okItem(std::size_t i) {
    BenchItem it;
    it.group = "FFT";
    it.name = "item " + std::to_string(i);
    it.ok = true;
    it.samplesPerSec = 24.1e6 + static_cast<double>(i);
    it.referenceRateHz = 2.4e6;
    it.realTimeMultiple = it.samplesPerSec / it.referenceRateHz;
    return it;
}

void helpTable() {
    const auto& t = helpTopics();
    CHECK(t.size() >= 20);
    std::set<std::string> ids;
    std::set<std::pair<int, int>> where;
    // words the window never says about the product: nothing of what is not built, no forum or chat, no "beta"
    const char* const banned[] = {"forum", "discord", "beta", "coming", "not yet", "phase", "later", "soon", "planned"};
    for (const HelpTopic& h : t) {
        CHECK(h.id != nullptr && *h.id != '\0' && ids.insert(h.id).second);       // every id is there once
        CHECK(h.title != nullptr && *h.title != '\0');
        CHECK(!h.body.empty());
        if (h.bank >= 0) { CHECK(where.insert({h.bank, h.row}).second); CHECK(h.row >= 0 && h.bank <= 4); }   // a section has one topic
        std::string all = lowerCase(h.title);
        for (const char* b : h.body) { CHECK(b != nullptr && *b != '\0'); all += " " + lowerCase(b); }
        for (const char* w : banned) { CHECK(all.find(w) == std::string::npos); }
    }
    // every section of the banks that has a chip in the rail has its place: the five the window's rail draws for SIGNAL's first rows, DECODE, TOOLS, EXTEND and SYSTEM's Display are covered
    CHECK(topicForSection(0, 1) >= 0 && topicIndex("sig-source") == topicForSection(0, 1));
    CHECK(topicForSection(2, 3) >= 0 && topicIndex("tools-scanner") == topicForSection(2, 3));
    CHECK(topicForSection(3, 0) >= 0 && topicForSection(4, 0) >= 0 && topicForSection(1, 5) >= 0);
    CHECK(topicForSection(4, 11) == topicIndex("sys-bench"));    // the Performance section holds the speed test
    CHECK(topicForSection(0, 5) == -1);                          // the Transmit row has no topic: nothing there is described
    CHECK(topicForSection(-1, -1) == -1 && topicForSection(9, 0) == -1);
    CHECK(topicIndex("nope") == -1 && topicIndex("") == -1);
}

void sentences() {
    CHECK(sentence("one\ntwo\nthree") == "one two three");   // the tooltip's own line breaks do not break the help's lines
    CHECK(sentence("plain") == "plain");
    CHECK(titleOf(helpTopics()[0]) == helpTopics()[0].title);   // English is in force in the test
}

void search() {
    const auto all = searchTopics("");
    CHECK(all.size() == helpTopics().size());
    CHECK(all.front() == 0 && all.back() == static_cast<int>(helpTopics().size()) - 1);   // the table's own order
    CHECK(searchTopics("   ").size() == all.size());                                       // only blanks is no query
    // a title hit comes before a body hit
    const auto sc = searchTopics("scanner");
    CHECK(!sc.empty() && sc.front() == topicIndex("tools-scanner"));
    // case-insensitive
    CHECK(searchTopics("SCANNER") == sc);
    // every word must be found, in any order
    const auto ab = searchTopics("airport lookup");
    CHECK(ab.empty());                                                                      // "lookup" is not a word the airband topic uses ("Look up" is two)
    const auto ab2 = searchTopics("look up airport");
    CHECK(!ab2.empty() && ab2.front() == topicIndex("tools-airband"));
    // key words that are not in the text are found too
    const auto kw = searchTopics("ppm");
    CHECK(!kw.empty() && kw.front() == topicIndex("sig-ppm"));
    // something not there
    CHECK(searchTopics("zzzzqqq").empty());
    // a word found only in a sentence (the body)
    const auto body = searchTopics("pilot");
    CHECK(!body.empty() && body.front() == topicIndex("sig-radio"));
    // the speed test is found by what a person would type
    CHECK(!searchTopics("benchmark").empty() && searchTopics("benchmark").front() == topicIndex("sys-bench"));
}

void benchFormat() {
    const NumberWriter plain = plainNumbers();
    CHECK(plain(24.123, 1) == "24.1" && plain(10.0, 1) == "10.0" && plain(7.5, 0) == "8" && plain(0.0, 2) == "0.00");
    BenchItem a = okItem(0);
    CHECK(itemText(a, plain) == "24.1 million samples a second, 10.0 times real time");
    BenchItem bad;
    bad.ok = false;
    bad.error = "illegal size";
    CHECK(itemText(bad, plain) == "failed: illegal size");
    bad.error.clear();
    CHECK(itemText(bad, plain) == "failed: ?");
    CHECK(progressText(5, 21) == "5 of 21");
    CHECK(summaryText({}, plain).empty());
    std::vector<BenchItem> v = {okItem(0), okItem(1), bad};
    v[1].realTimeMultiple = 3.25;
    v[1].name = "slow";
    CHECK(summaryText(v, plain) == "3 items timed, 1 failed. The slowest is FFT slow at 3.2 times real time.");
    // a writer that uses the Windows comma is honoured
    const NumberWriter comma = [](double x, int d) { std::string s = plainNumbers()(x, d); for (char& c : s) { if (c == '.') { c = ','; } } return s; };
    CHECK(itemText(a, comma) == "24,1 million samples a second, 10,0 times real time");
}

void benchRuns() {
    // a full run: every item timed, in order, progress reaches the total
    {
        std::atomic<int> calls{0};
        BenchJob j([&](std::size_t i) { ++calls; return okItem(i); }, 5);
        CHECK(j.state() == BenchState::Idle && j.done() == 0 && j.total() == 5 && j.results().empty());
        CHECK(j.start());
        CHECK(waitFor([&] { return j.state() == BenchState::Done; }));
        CHECK(j.done() == 5 && calls == 5);
        const auto r = j.results();
        CHECK(r.size() == 5 && r[0].name == "item 0" && r[4].name == "item 4");
    }
    // cancel between items: the run ends Cancelled, the items done stay, the rest are never run
    {
        std::atomic<int> calls{0};
        std::atomic<bool> release{false};
        BenchJob j([&](std::size_t i) {
            ++calls;
            if (i == 1) { while (!release.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); } }
            return okItem(i);
        }, 6);
        CHECK(j.start());
        CHECK(waitFor([&] { return calls.load() == 2; }));      // item 1 is being timed
        j.cancel();
        release = true;
        CHECK(waitFor([&] { return j.state() == BenchState::Cancelled; }));
        CHECK(calls == 2 && j.done() == 2 && j.results().size() == 2);
    }
    // a second start while running is refused; after the end it begins again from nothing
    {
        std::atomic<bool> release{false};
        BenchJob j([&](std::size_t i) { while (!release.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); } return okItem(i); }, 2);
        CHECK(j.start());
        CHECK(!j.start());
        release = true;
        CHECK(waitFor([&] { return j.state() == BenchState::Done; }));
        CHECK(j.start());
        CHECK(waitFor([&] { return j.state() == BenchState::Done && j.done() == 2; }));
        CHECK(j.results().size() == 2);
    }
    // an item that throws is a failed item and the run goes on
    {
        BenchJob j([&](std::size_t i) -> BenchItem {
            if (i == 1) { throw std::runtime_error("boom"); }
            return okItem(i);
        }, 3);
        CHECK(j.start());
        CHECK(waitFor([&] { return j.state() == BenchState::Done; }));
        const auto r = j.results();
        CHECK(r.size() == 3 && r[0].ok && !r[1].ok && r[1].error == "boom" && r[2].ok);
    }
    // an item that throws a thing that is not an exception
    {
        BenchJob j([&](std::size_t i) -> BenchItem { if (i == 0) { throw 7; } return okItem(i); }, 2);
        CHECK(j.start());
        CHECK(waitFor([&] { return j.state() == BenchState::Done; }));
        CHECK(!j.results()[0].ok && !j.results()[0].error.empty());
    }
    // no items at all: done at once, nothing timed
    {
        BenchJob j([&](std::size_t i) { return okItem(i); }, 0);
        CHECK(j.start());
        CHECK(waitFor([&] { return j.state() == BenchState::Done; }));
        CHECK(j.results().empty());
    }
    // cancelled before the first item runs
    {
        std::atomic<int> calls{0};
        BenchJob j([&](std::size_t i) { ++calls; return okItem(i); }, 3);
        j.cancel();                                              // nothing to cancel: the next start clears the flag
        CHECK(j.start());
        CHECK(waitFor([&] { return j.state() == BenchState::Done; }));
        CHECK(calls == 3);
    }
    // destroying a job in the middle of a run does not wait for the rest of the items and does not hang
    {
        std::atomic<int> calls{0};
        {
            BenchJob j([&](std::size_t i) { ++calls; std::this_thread::sleep_for(std::chrono::milliseconds(20)); return okItem(i); }, 1000);
            CHECK(j.start());
            CHECK(waitFor([&] { return calls.load() >= 1; }));
        }
        CHECK(calls < 1000);
    }
}

}  // namespace

int main() {
    helpTable();
    sentences();
    search();
    benchFormat();
    benchRuns();
    return testSummary("test_slint_lanex_host");
}
