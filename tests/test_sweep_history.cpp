// test_sweep_history.cpp - sweep/trace history as CSV, JSON and XML
// (core/sweep_history.hpp): round trips that are bit-exact, refusal of malformed
// input WITH the line number, the size caps, the import of a "frequency, level"
// export, and a fuzz run over every parser.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "core/sweep_history.hpp"
#include "test_check.hpp"

using namespace cascade::core;

namespace {

SweepHistory sample(std::size_t sweeps, std::size_t points, unsigned seed, bool withNan) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> u(-120.0, -10.0);
    SweepHistory h;
    for (std::size_t s = 0; s < sweeps; ++s) {
        SweepLine l;
        l.startHz = 88.0e6 + 1000.0 * static_cast<double>(s);
        l.stopHz = l.startHz + 20.0e6 / 3.0;   // not a round number on purpose
        l.timeS = 1.7e9 + 0.123456789 * static_cast<double>(s);
        for (std::size_t i = 0; i < points; ++i) {
            l.db.push_back(withNan && rng() % 7 == 0 ? std::nanf("") : static_cast<float>(u(rng)));
        }
        h.sweeps.push_back(std::move(l));
    }
    return h;
}

constexpr HistoryFormat kFormats[] = {HistoryFormat::Csv, HistoryFormat::Json, HistoryFormat::Xml};
const char* nameOf(HistoryFormat f) { return f == HistoryFormat::Csv ? "csv" : f == HistoryFormat::Json ? "json" : "xml"; }

HistoryLoad load(HistoryFormat f, const std::string& t) {
    return f == HistoryFormat::Csv ? historyFromCsv(t) : f == HistoryFormat::Json ? historyFromJson(t) : historyFromXml(t);
}

std::string replaceOnce(std::string s, const std::string& a, const std::string& b) {
    const std::size_t p = s.find(a);
    if (p != std::string::npos) { s.replace(p, a.size(), b); }
    return s;
}

}  // namespace

int main() {
    // ---- round trips: bit-exact, NaN gaps kept, every format ----
    for (HistoryFormat f : kFormats) {
        for (bool nan : {false, true}) {
            const SweepHistory h = sample(5, 301, 7u + (nan ? 1u : 0u), nan);
            std::string why;
            const std::string text = historyToText(h, f, &why);
            CHECK(!text.empty());
            const HistoryLoad r = load(f, text);
            if (!r.ok) { std::printf("%s round trip: line %zu %s\n", nameOf(f), r.line, r.error.c_str()); }
            CHECK(r.ok);
            CHECK(r.history == h);
            // the automatic reader finds the format by itself
            const HistoryLoad a = historyFromText(f == HistoryFormat::Csv ? text : "  \n" + text);
            CHECK(a.ok && a.history == h);
        }
        // one sweep of one point, and an extreme but valid line
        SweepHistory one;
        one.sweeps.push_back(SweepLine{145.8e6, 145.8e6, {-97.5f}, 0.0});
        const HistoryLoad r1 = load(f, historyToText(one, f));
        CHECK(r1.ok && r1.history == one);
        SweepHistory edge;
        edge.sweeps.push_back(SweepLine{0.0, kSweepMaxFrequencyHz, {-400.0f, 400.0f, std::nanf("")}, -1.0e9});
        const HistoryLoad r2 = load(f, historyToText(edge, f));
        CHECK(r2.ok && r2.history == edge);
    }

    // ---- the documented layout, literally ----
    {
        SweepHistory h;
        h.sweeps.push_back(SweepLine{1000.0, 3000.0, {-80.5f, std::nanf(""), -70.0f}, 5.0});
        CHECK(historyToText(h, HistoryFormat::Csv) ==
              "#foxsdr-sweep-history,csv,1\ntime_s,start_hz,stop_hz,points,db\n5,1000,3000,3,-80.5,,-70\n");
        CHECK(historyToText(h, HistoryFormat::Json) ==
              "{\"format\":\"foxsdr-sweep-history\",\"version\":1,\"sweeps\":[\n"
              "{\"time_s\":5,\"start_hz\":1000,\"stop_hz\":3000,\"db\":[-80.5,null,-70]}\n]}\n");
        CHECK(historyToText(h, HistoryFormat::Xml) ==
              "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<sweep-history format=\"foxsdr-sweep-history\" version=\"1\">\n"
              "<sweep time_s=\"5\" start_hz=\"1000\" stop_hz=\"3000\" points=\"3\">-80.5 nan -70</sweep>\n</sweep-history>\n");
        // CRLF and trailing blank lines are read
        const HistoryLoad crlf = historyFromCsv("#foxsdr-sweep-history,csv,1\r\ntime_s,start_hz,stop_hz,points,db\r\n5,1000,3000,3,-80.5,,-70\r\n\r\n");
        CHECK(crlf.ok && crlf.history == h);
    }

    // ---- writers refuse ----
    {
        std::string why;
        SweepHistory empty;
        CHECK(historyToText(empty, HistoryFormat::Csv, &why).empty() && why == "no sweeps");
        SweepHistory bad = sample(1, 5, 1, false);
        bad.sweeps[0].db[2] = std::numeric_limits<float>::infinity();
        CHECK(historyToText(bad, HistoryFormat::Json, &why).empty() && why == "level out of range");
        bad = sample(1, 5, 1, false);
        bad.sweeps[0].stopHz = bad.sweeps[0].startHz - 1.0;
        CHECK(historyToText(bad, HistoryFormat::Xml, &why).empty() && why == "stop below start");
        bad = sample(1, 5, 1, false);
        bad.sweeps[0].db.clear();
        CHECK(historyToText(bad, HistoryFormat::Csv, &why).empty() && why == "no points");
        SweepHistory many;
        many.sweeps.assign(kHistoryMaxSweeps + 1, SweepLine{1.0, 2.0, {-1.0f, -2.0f}, 0.0});
        CHECK(historyToText(many, HistoryFormat::Csv, &why).empty() && why == "too many sweeps");
    }

    // ---- readers refuse, naming the line ----
    {
        const SweepHistory h = sample(3, 4, 3, false);
        const std::string csv = historyToText(h, HistoryFormat::Csv);
        // corrupt the level of the 2nd sweep (line 4)
        std::string c = csv;
        std::size_t line4 = 0;
        for (int i = 0; i < 3; ++i) { line4 = c.find('\n', line4) + 1; }   // start of line 4
        c.replace(c.find(',', c.find(',', c.find(',', c.find(',', line4) + 1) + 1) + 1) + 1, 3, "zzz");
        HistoryLoad r = historyFromCsv(c);
        CHECK(!r.ok && r.line == 4);
        r = historyFromCsv(replaceOnce(csv, "csv,1", "csv,2"));
        CHECK(!r.ok && r.line == 1 && r.error == "unsupported version");
        r = historyFromCsv("hello\n");
        CHECK(!r.ok && r.line == 1);
        r = historyFromCsv("#foxsdr-sweep-history,csv,1\ntime,start,stop,points,db\n");
        CHECK(!r.ok && r.line == 2 && r.error == "bad column header");
        r = historyFromCsv("#foxsdr-sweep-history,csv,1\ntime_s,start_hz,stop_hz,points,db\n0,1,2,3,-1,-2\n");
        CHECK(!r.ok && r.line == 3 && r.error == "field count does not match points");
        r = historyFromCsv("#foxsdr-sweep-history,csv,1\ntime_s,start_hz,stop_hz,points,db\n\n0,1,2,2,-1,-2\n0,5,2,2,-1,-2\n");
        CHECK(!r.ok && r.line == 5 && r.error == "stop below start");
        r = historyFromCsv("#foxsdr-sweep-history,csv,1\ntime_s,start_hz,stop_hz,points,db\n0,1,2,2,nan,-2\n");
        CHECK(!r.ok && r.line == 3);   // "nan" text is not accepted
        r = historyFromCsv("#foxsdr-sweep-history,csv,1\ntime_s,start_hz,stop_hz,points,db\n0,1,2,2,-1,-1000\n");
        CHECK(!r.ok && r.error == "level out of range");
        r = historyFromCsv("#foxsdr-sweep-history,csv,1\n");
        CHECK(!r.ok);
        r = historyFromCsv("");
        CHECK(!r.ok);
        r = historyFromCsv("#foxsdr-sweep-history,csv,1\ntime_s,start_hz,stop_hz,points,db\n");
        CHECK(r.ok && r.history.sweeps.empty());   // a CSV with no rows is an empty history, not an error

        const std::string js = historyToText(h, HistoryFormat::Json);
        r = historyFromJson(replaceOnce(js, "\"version\":1", "\"version\":2"));
        CHECK(!r.ok && r.error == "unsupported version");
        // a syntax error on line 3 (the second sweep): break its closing bracket
        std::string j = js;
        const std::size_t second = j.find("\n{", j.find('\n') + 1) + 1;
        j.insert(j.find(']', second), ",,");
        r = historyFromJson(j);
        CHECK(!r.ok && r.line == 3 && r.error == "JSON syntax error");
        // a schema error: a text where a level belongs, in the third sweep (line 4)
        j = js;
        const std::size_t third = j.find("\n{", j.find("\n{", j.find('\n') + 1) + 1) + 1;
        j.replace(j.find("db\":[", third) + 5, 1, "\"x\",");
        r = historyFromJson(j);
        CHECK(!r.ok && r.line == 4);
        r = historyFromJson(replaceOnce(js, "\"time_s\"", "\"bogus\""));
        CHECK(!r.ok && r.line == 2);
        r = historyFromJson("[1,2]");
        CHECK(!r.ok && r.line == 1);
        r = historyFromJson("{\"format\":\"foxsdr-sweep-history\",\"version\":1,\"sweeps\":[]}");
        CHECK(!r.ok && r.error == "no sweeps");

        const std::string xm = historyToText(h, HistoryFormat::Xml);
        r = historyFromXml(replaceOnce(xm, "version=\"1\"", "version=\"9\""));
        CHECK(!r.ok && r.line == 2 && r.error == "unsupported version");
        r = historyFromXml(replaceOnce(xm, "points=\"4\"", "points=\"5\""));
        CHECK(!r.ok && r.line == 3 && r.error == "fewer levels than points");
        r = historyFromXml(replaceOnce(xm, "points=\"4\"", "points=\"3\""));
        CHECK(!r.ok && r.line == 3 && r.error == "more levels than points");
        r = historyFromXml("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE x [<!ENTITY e \"y\">]>\n<sweep-history/>");
        CHECK(!r.ok && r.line == 2);
        r = historyFromXml(replaceOnce(xm, "-1", "&amp;"));
        CHECK(!r.ok);
        r = historyFromXml(xm + "<junk/>");
        CHECK(!r.ok);
        r = historyFromXml(xm.substr(0, xm.size() - 20));
        CHECK(!r.ok);
        r = historyFromXml(replaceOnce(xm, "<sweep time_s", "<sweep extra=\"1\" time_s"));
        CHECK(!r.ok && r.line == 3);
        r = historyFromXml(replaceOnce(xm, "start_hz=\"", "time_s=\""));   // duplicate attribute
        CHECK(!r.ok);
        r = historyFromText("   ");
        CHECK(!r.ok);
        r = historyFromText("what");
        CHECK(!r.ok && r.line == 1);
    }

    // ---- caps ----
    {
        HistoryLoad r = historyFromCsv(std::string(kHistoryMaxBytes + 1, 'a'));
        CHECK(!r.ok && r.error == "file too large" && r.line == 0);
        r = historyFromJson(std::string(kHistoryMaxBytes + 1, ' '));
        CHECK(!r.ok && r.error == "file too large");
        r = historyFromXml(std::string(kHistoryMaxBytes + 1, ' '));
        CHECK(!r.ok && r.error == "file too large");
        // a points field that asks for more than a line may hold
        r = historyFromCsv("#foxsdr-sweep-history,csv,1\ntime_s,start_hz,stop_hz,points,db\n0,1,2,99999999,-1\n");
        CHECK(!r.ok && r.error == "bad points");
        r = historyFromXml("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<sweep-history format=\"foxsdr-sweep-history\" version=\"1\">\n"
                           "<sweep time_s=\"0\" start_hz=\"1\" stop_hz=\"2\" points=\"99999999\">-1</sweep>\n</sweep-history>");
        CHECK(!r.ok && r.error == "bad points" && r.line == 3);
        // too many sweeps in a file
        std::string many = "#foxsdr-sweep-history,csv,1\ntime_s,start_hz,stop_hz,points,db\n";
        for (std::size_t i = 0; i <= kHistoryMaxSweeps; ++i) { many += "0,1,2,2,-1,-2\n"; }
        r = historyFromCsv(many);
        CHECK(!r.ok && r.error == "too many sweeps" && r.line == kHistoryMaxSweeps + 3);
    }

    // ---- the import of a "frequency (MHz), amplitude (dBm)" export ----
    {
        std::string t = "Frequency(MHz), Amplitude(dBm)\r\n";
        for (int i = 0; i < 112; ++i) {
            char b[64];
            std::snprintf(b, sizeof(b), "%.4f, %.1f\r\n", 433.0 + 0.0125 * i, -105.0 + 0.5 * (i % 9));
            t += b;
        }
        HistoryLoad r = importFreqLevelPairs(t);
        CHECK(r.ok && r.history.sweeps.size() == 1);
        if (r.ok) {
            const SweepLine& l = r.history.sweeps[0];
            CHECK(l.db.size() == 112);
            CHECK_NEAR(l.startHz, 433.0e6, 1.0);
            CHECK_NEAR(l.stopHz, 433.0e6 + 111 * 12500.0, 2.0);
            CHECK_NEAR(l.db[10], -105.0 + 0.5, 1e-6);
            CHECK(sweepNearestPoint(l, 433.0e6 + 12500.0 * 50.2) == 50);
            CHECK(sweepNearestPoint(l, 400e6) == -1);
        }
        // tab and semicolon separated, kHz unit, no header
        r = importFreqLevelPairs("100;-50\n101\t-51\n102 -52\n", PairsOptions{1000.0, 0.01});
        CHECK(r.ok && r.history.sweeps[0].db.size() == 3 && r.history.sweeps[0].startHz == 100000.0);
        // refusals with the file's line numbers
        r = importFreqLevelPairs("Freq,dBm\n100,-50\n101,-51\n103,-52\n");
        CHECK(!r.ok && r.line == 3 && r.error == "frequencies are not evenly spaced");   // the first spacing that departs from the mean step
        r = importFreqLevelPairs("Freq,dBm\n100,-50\n100,-51\n");
        CHECK(!r.ok && r.line == 3 && r.error == "frequencies are not strictly increasing");
        r = importFreqLevelPairs("100,-50\n101,abc\n");
        CHECK(!r.ok && r.line == 2 && r.error == "bad level");
        r = importFreqLevelPairs("100,-50,3\n");
        CHECK(!r.ok && r.line == 1);
        r = importFreqLevelPairs("100,-50\n");
        CHECK(!r.ok && r.error == "fewer than two pairs");
        r = importFreqLevelPairs("");
        CHECK(!r.ok);
        r = importFreqLevelPairs("100,-50\n101,-900\n");
        CHECK(!r.ok && r.line == 2);
        r = importFreqLevelPairs("100,-50\n101,-51\n", PairsOptions{0.0, 0.01});
        CHECK(!r.ok);
    }

    // ---- fuzz: every parser, mutated valid files and random bytes. A load that says ok holds
    //      only valid lines, and writing it again and reading that back gives it back. ----
    {
        std::mt19937 rng(0xFEEDu);
        const SweepHistory base = sample(3, 12, 11, true);
        std::size_t okCount = 0, bad = 0;
        for (HistoryFormat f : kFormats) {
            const std::string good = historyToText(base, f);
            for (int it = 0; it < 6000; ++it) {
                std::string t = good;
                const int kind = static_cast<int>(rng() % 4);
                if (kind == 0) {
                    t.assign(rng() % 200, '\0');
                    for (auto& ch : t) { ch = static_cast<char>(rng()); }
                } else {
                    const int edits = 1 + static_cast<int>(rng() % 3);
                    for (int e = 0; e < edits && !t.empty(); ++e) {
                        const std::size_t p = rng() % t.size();
                        switch (rng() % 4) {
                            case 0: t[p] = static_cast<char>(rng()); break;
                            case 1: t.erase(p, 1 + rng() % 8); break;
                            case 2: t.insert(p, 1, "<>\",{}[]\n-.e0&"[rng() % 14]); break;
                            default: t.resize(p); break;
                        }
                    }
                }
                const HistoryLoad r = load(f, t);
                if (r.ok) {
                    ++okCount;
                    for (const SweepLine& l : r.history.sweeps) { CHECK(sweepLineProblem(l).empty()); }
                    if (!r.history.sweeps.empty()) {
                        const std::string again = historyToText(r.history, f);
                        CHECK(!again.empty());
                        CHECK(load(f, again).history == r.history);
                    }
                } else {
                    ++bad;
                    CHECK(!r.error.empty());
                }
                const HistoryLoad p = importFreqLevelPairs(t);
                if (p.ok) { CHECK(p.history.sweeps.size() == 1 && sweepLineProblem(p.history.sweeps[0]).empty()); }
            }
        }
        CHECK(bad > 5000);
        std::printf("fuzz: %zu loads ok, %zu refused of 18000\n", okCount, bad);
    }

    return testSummary("test_sweep_history");
}
