// Tests for the alert rules (core/alert_rules.hpp): the matcher (keywords, wildcards, target lists, filters), rate limiting, quiet hours,
// the rule file (round trip and hostile input) and the notification sink.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/alert_rules.hpp"

#include "test_check.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <thread>

namespace {

namespace al = cascade::core::alerts;
namespace fs = std::filesystem;
using al::Alert;
using al::DecodeEvent;
using al::Rule;

constexpr std::int64_t kBase = 1700000000000LL;   // 2023-11-14 22:13:20 UTC, a Tuesday

DecodeEvent ev(const std::string& text, const std::string& decoder = "ADS-B", double freq = 1090e6, std::int64_t t = kBase, std::uint16_t radio = 0) {
    DecodeEvent e;
    e.timeMs = t;
    e.radio = radio;
    e.freqHz = freq;
    e.decoder = decoder;
    e.kind = "message";
    e.text = text;
    return e;
}

Rule rule(const std::string& name) {
    Rule r;
    r.name = name;
    return r;
}

std::size_t fires(al::AlertEngine& en, const DecodeEvent& e) { return en.process(e).size(); }

// ---- criteria ---------------------------------------------------------------------------------------------------------------------

void testKeywords() {
    al::AlertEngine en;
    Rule r = rule("k");
    r.keywords = {"MAYDAY", "pan pan"};
    std::string err;
    CHECK(en.setRules({r}, &err));
    auto a = en.process(ev("this is a mayday call"));
    CHECK(a.size() == 1);
    if (!a.empty()) {
        CHECK(a[0].rule == "k" && a[0].matchedBy == "keyword: mayday" && a[0].severity == al::Severity::Notice);
        CHECK(a[0].title == "k: ADS-B 1090.0000 MHz" && a[0].body == "this is a mayday call" && a[0].timeMs == kBase);
    }
    CHECK(fires(en, ev("PAN PAN PAN")) == 1);
    CHECK(fires(en, ev("routine traffic")) == 0);
    CHECK(fires(en, ev("")) == 0);
    DecodeEvent f = ev("nothing here");
    f.fields.emplace_back("note", "Emergency MAYDAY declared");
    CHECK(fires(en, f) == 1);                               // a field value counts
    DecodeEvent k = ev("nothing");
    k.kind = "MAYDAY-relay";
    CHECK(fires(en, k) == 1);                               // so does the kind
    DecodeEvent keyOnly = ev("nothing");
    keyOnly.fields.emplace_back("mayday", "x");
    CHECK(fires(en, keyOnly) == 0);                         // a field NAME does not
    // an event with an empty text and no criteria is not matched by a keyword rule
    CHECK(fires(en, ev("x", "", 0)) == 0);
}

void testWildcards() {
    al::AlertEngine en;
    Rule r = rule("w");
    r.wildcards = {"*mayday*", "cq ??? de *", "exact text"};
    CHECK(en.setRules({r}));
    CHECK(fires(en, ev("xx MAYDAY yy")) == 1);
    CHECK(fires(en, ev("mayday")) == 1);                    // '*' matches the empty run
    CHECK(fires(en, ev("CQ ABC DE G4XYZ")) == 1);
    CHECK(fires(en, ev("CQ AB DE G4XYZ")) == 0);            // '?' is exactly one character
    CHECK(fires(en, ev("cq abcd de g4xyz")) == 0);
    CHECK(fires(en, ev("Exact Text")) == 1);
    CHECK(fires(en, ev("exact text and more")) == 0);       // anchored over the whole text
    CHECK(fires(en, ev("more exact text")) == 0);
    // never a regular expression: regex syntax is literal
    Rule lit = rule("lit");
    lit.wildcards = {"a.c", "[a-z]+", "(x|y)", "a+b", "^abc$", "ab*c\\d"};
    CHECK(en.setRules({lit}));
    CHECK(fires(en, ev("abc")) == 0);
    CHECK(fires(en, ev("a.c")) == 1);
    CHECK(fires(en, ev("hello")) == 0);
    CHECK(fires(en, ev("[a-z]+")) == 1);
    CHECK(fires(en, ev("x")) == 0);
    CHECK(fires(en, ev("(x|y)")) == 1);
    CHECK(fires(en, ev("aab")) == 0);
    CHECK(fires(en, ev("^abc$")) == 1);
    CHECK(fires(en, ev("abXXc\\d")) == 1);
    // a lone '*' matches everything, and a pathological pattern on a long text finishes
    Rule all = rule("all");
    all.wildcards = {"*"};
    CHECK(en.setRules({all}));
    CHECK(fires(en, ev("")) == 1 && fires(en, ev("anything")) == 1);
    Rule path = rule("path");
    path.wildcards = {std::string(100, 'a') + "*" + std::string(60, '*') + "b"};
    CHECK(en.setRules({path}));
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(fires(en, ev(std::string(16000, 'a'))) == 0);
    CHECK(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 1.0);
}

void testTargets() {
    al::AlertEngine en;
    Rule r = rule("t");
    r.callsigns = {"baw123", "KJ4XYZ-9"};
    r.icaoAddresses = {"4ca2b1", "406B3F"};
    r.mmsis = {"235012345"};
    r.squawks = {"7700", "7600"};
    std::string err;
    CHECK(en.setRules({r}, &err));
    auto field = [](const std::string& k, const std::string& v, const std::string& text = "x") { DecodeEvent e = ev(text); e.fields.emplace_back(k, v); return e; };
    // callsigns: a named field (any of the names, any case, padded) or a whole token of the text
    CHECK(fires(en, field("callsign", "BAW123")) == 1);
    CHECK(fires(en, field("Flight", "  baw123  ")) == 1);
    CHECK(fires(en, field("ident", "BAW123")) == 1);
    CHECK(fires(en, field("callsign", "BAW1234")) == 0);     // not a prefix match
    CHECK(fires(en, field("callsign", "XBAW123")) == 0);
    CHECK(fires(en, field("other", "BAW123")) == 0);         // an unrelated field name
    CHECK(fires(en, ev("departing BAW123 now")) == 1);
    CHECK(fires(en, ev("departing baw123, now")) == 1);
    CHECK(fires(en, ev("BAW1234 departing")) == 0);
    CHECK(fires(en, ev("departing BAW12")) == 0);
    CHECK(fires(en, ev("kj4xyz-9>APRS,WIDE1-1:position")) == 1);   // '>' ends the token: an APRS packet header names its source first
    CHECK(fires(en, ev("from KJ4XYZ-9>APRS")) == 1);
    CHECK(fires(en, ev("from KJ4XYZ-90")) == 0);
    CHECK(fires(en, ev("from KJ4XYZ")) == 0);                // the SSID is part of the callsign
    // ICAO: hex in any case
    CHECK(fires(en, field("icao", "4CA2B1")) == 1);
    CHECK(fires(en, field("hex", "406b3f")) == 1);
    CHECK(fires(en, field("address", "406B3F")) == 1);
    CHECK(fires(en, ev("aircraft 406b3f climbing")) == 1);
    CHECK(fires(en, field("icao", "4CA2B2")) == 0);
    // MMSI
    CHECK(fires(en, field("mmsi", "235012345")) == 1);
    CHECK(fires(en, ev("vessel 235012345 under way")) == 1);
    CHECK(fires(en, ev("vessel 2350123456")) == 0);
    // squawk: fields only
    CHECK(fires(en, field("squawk", "7700")) == 1);
    CHECK(fires(en, field("code", "7600")) == 1);
    CHECK(fires(en, field("squawk", "7500")) == 0);
    CHECK(fires(en, ev("squawk 7700 seen")) == 0);           // free text is not a squawk
    // what matched is named
    auto a = en.process(field("icao", "4ca2b1"));
    CHECK(a.size() == 1 && a[0].matchedBy == "icao: 4CA2B1");
    a = en.process(field("squawk", "7700"));
    CHECK(a.size() == 1 && a[0].matchedBy == "squawk: 7700");
}

void testFilters() {
    al::AlertEngine en;
    Rule filt = rule("filters");                             // no criteria: the filters alone decide
    filt.decoders = {"pocsag", "APRS"};
    filt.hasFreq = true;
    filt.freqMinHz = 144e6;
    filt.freqMaxHz = 146e6;
    filt.radio = 2;
    CHECK(en.setRules({filt}));
    CHECK(fires(en, ev("x", "POCSAG", 145e6, kBase, 2)) == 1);
    {
        const auto lowEdge = en.process(ev("x", "aprs", 144e6, kBase, 2));   // inclusive low edge
        CHECK(lowEdge.size() == 1 && lowEdge[0].matchedBy == "filters");
    }
    CHECK(fires(en, ev("x", "APRS", 146e6, kBase, 2)) == 1);                          // inclusive high edge
    CHECK(fires(en, ev("x", "APRS", 146e6 + 1, kBase, 2)) == 0);
    CHECK(fires(en, ev("x", "APRS", 144e6 - 1, kBase, 2)) == 0);
    CHECK(fires(en, ev("x", "AIS", 145e6, kBase, 2)) == 0);                           // decoder not listed
    CHECK(fires(en, ev("x", "APRS", 145e6, kBase, 1)) == 0);                          // other radio
    CHECK(fires(en, ev("x", "AP", 145e6, kBase, 2)) == 0);                            // exact, not a prefix
    // filters AND criteria
    Rule both = rule("both");
    both.keywords = {"mayday"};
    both.decoders = {"ais"};
    both.hasFreq = true;
    both.freqMinHz = 156e6;
    both.freqMaxHz = 163e6;
    CHECK(en.setRules({both}));
    CHECK(fires(en, ev("MAYDAY", "AIS", 161.975e6)) == 1);
    CHECK(fires(en, ev("MAYDAY", "ADS-B", 161.975e6)) == 0);
    CHECK(fires(en, ev("MAYDAY", "AIS", 100e6)) == 0);
    CHECK(fires(en, ev("calm", "AIS", 161.975e6)) == 0);
    // groups are alternatives
    Rule alt = rule("alt");
    alt.keywords = {"zzz"};
    alt.wildcards = {"hello*"};
    alt.callsigns = {"ABC1"};
    CHECK(en.setRules({alt}));
    CHECK(fires(en, ev("zzz")) == 1 && fires(en, ev("hello there")) == 1 && fires(en, ev("ABC1 here")) == 1);
    CHECK(fires(en, ev("nothing")) == 0);
    // disabled rules are silent and not counted
    Rule off = rule("off");
    off.enabled = false;
    CHECK(en.setRules({off}));
    CHECK(fires(en, ev("x")) == 0 && en.stats()[0].matched == 0);
}

// ---- rate limits -------------------------------------------------------------------------------------------------------------------

void testRateLimits() {
    al::AlertEngine en;
    Rule r = rule("r");
    r.keywords = {"x"};
    r.minIntervalMs = 10000;
    CHECK(en.setRules({r}));
    CHECK(fires(en, ev("x", "A", 1, kBase)) == 1);
    CHECK(fires(en, ev("x", "A", 1, kBase + 9999)) == 0);
    CHECK(fires(en, ev("x", "A", 1, kBase + 10000)) == 1);   // exactly the interval later
    CHECK(fires(en, ev("x", "A", 1, kBase + 10001)) == 0);   // measured from the last ALERT, not the last match
    CHECK(fires(en, ev("x", "A", 1, kBase + 20000)) == 1);
    CHECK(fires(en, ev("x", "A", 1, kBase - 50000)) == 1);   // an older event (a replay out of order) is not held back by a later one
    auto st = en.stats();
    CHECK(st[0].matched == 6 && st[0].fired == 4 && st[0].rateLimited == 2);

    // per target
    Rule k = rule("k");
    k.callsigns = {"AAA1", "BBB2"};
    k.perKeyIntervalMs = 60000;
    CHECK(en.setRules({k}));
    CHECK(fires(en, ev("AAA1 up", "A", 1, kBase)) == 1);
    CHECK(fires(en, ev("BBB2 up", "A", 1, kBase + 1000)) == 1);     // another target is its own key
    CHECK(fires(en, ev("AAA1 again", "A", 1, kBase + 30000)) == 0);
    CHECK(fires(en, ev("BBB2 again", "A", 1, kBase + 30000)) == 0);
    CHECK(fires(en, ev("AAA1 later", "A", 1, kBase + 60000)) == 1);
    // a suppressed repeat does not extend the quiet period
    CHECK(fires(en, ev("AAA1 x", "A", 1, kBase + 119999)) == 0);
    CHECK(fires(en, ev("AAA1 y", "A", 1, kBase + 120000)) == 1);

    // hourly cap, sliding
    Rule h = rule("h");
    h.keywords = {"x"};
    h.maxPerHour = 3;
    CHECK(en.setRules({h}));
    int fired = 0;
    for (int i = 0; i < 10; ++i) { fired += static_cast<int>(fires(en, ev("x", "A", 1, kBase + i * 1000))); }
    CHECK(fired == 3);
    CHECK(fires(en, ev("x", "A", 1, kBase + 3600000 - 1)) == 0);       // the first of the three is still inside the hour
    CHECK(fires(en, ev("x", "A", 1, kBase + 3600000)) == 1);           // ... and has just left it
    CHECK(fires(en, ev("x", "A", 1, kBase + 3601000)) == 1);
    CHECK(fires(en, ev("x", "A", 1, kBase + 3602000)) == 1);
    CHECK(fires(en, ev("x", "A", 1, kBase + 3603000)) == 0);

    // the per-target memory is bounded (4096 keys, least recently used forgotten): 50000 distinct keys, then the oldest key is forgotten
    // (it fires again) while a recent one is still remembered (it is held back)
    Rule many = rule("many");
    many.wildcards = {"*"};
    many.perKeyIntervalMs = 1000000;
    CHECK(en.setRules({many}));
    for (int i = 0; i < 50000; ++i) { en.process(ev("t" + std::to_string(i), "A", 1, kBase)); }
    CHECK(en.stats()[0].fired == 50000);
    CHECK(fires(en, ev("t0", "A", 1, kBase)) == 1);        // forgotten
    CHECK(fires(en, ev("t49999", "A", 1, kBase)) == 0);    // remembered
}

// ---- quiet hours ------------------------------------------------------------------------------------------------------------------

std::int64_t at(int y, int mo, int d, int h, int mi) {
    // days from civil (Hinnant)
    y -= mo <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * static_cast<unsigned>(mo + (mo > 2 ? -3 : 9)) + 2) / 5 + static_cast<unsigned>(d) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const std::int64_t days = static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
    return (days * 86400 + h * 3600 + mi * 60) * 1000;
}

void testQuietHours() {
    al::AlertEngine en;
    Rule r = rule("q");
    r.keywords = {"x"};
    r.quiet.enabled = true;
    r.quiet.startMinute = 22 * 60;
    r.quiet.endMinute = 6 * 60;                          // 22:00 to 06:00, wraps midnight
    CHECK(en.setRules({r}));
    auto f = [&](int h, int mi) { return fires(en, ev("x", "A", 1, at(2023, 11, 14, h, mi))); };
    CHECK(f(21, 59) == 1);
    CHECK(f(22, 0) == 0);                                // starts at 22:00 inclusive
    CHECK(f(23, 59) == 0);
    CHECK(f(0, 0) == 0);
    CHECK(f(5, 59) == 0);
    CHECK(f(6, 0) == 1);                                 // ends at 06:00 exclusive
    CHECK(f(12, 0) == 1);
    CHECK(en.stats()[0].quiet == 4 && en.stats()[0].fired == 3);
    // a time zone: 22:00-06:00 at UTC+01:00 means 21:00-05:00 UTC
    r.quiet.utcOffsetMinutes = 60;
    CHECK(en.setRules({r}));
    CHECK(f(20, 59) == 1 && f(21, 0) == 0 && f(4, 59) == 0 && f(5, 0) == 1);
    r.quiet.utcOffsetMinutes = -330;                     // UTC-05:30: 22:00 local is 03:30 UTC next day
    CHECK(en.setRules({r}));
    CHECK(f(3, 29) == 1 && f(3, 30) == 0 && f(11, 29) == 0 && f(11, 30) == 1);
    r.quiet.utcOffsetMinutes = 0;
    // a daytime window that does not wrap
    r.quiet.startMinute = 9 * 60;
    r.quiet.endMinute = 17 * 60 + 30;
    CHECK(en.setRules({r}));
    CHECK(f(8, 59) == 1 && f(9, 0) == 0 && f(17, 29) == 0 && f(17, 30) == 1);
    // start == end: the whole day
    r.quiet.startMinute = r.quiet.endMinute = 0;
    CHECK(en.setRules({r}));
    CHECK(f(0, 0) == 0 && f(12, 0) == 0 && f(23, 59) == 0);
    // days: quiet only on days the period STARTS: Tuesday (2023-11-14) only, 22:00-06:00
    r.quiet.startMinute = 22 * 60;
    r.quiet.endMinute = 6 * 60;
    r.quiet.days = 1u << 2;
    CHECK(en.setRules({r}));
    CHECK(f(23, 0) == 0);                                             // Tuesday evening: quiet
    CHECK(fires(en, ev("x", "A", 1, at(2023, 11, 15, 3, 0))) == 0);   // Wednesday 03:00 belongs to Tuesday's night: quiet
    CHECK(fires(en, ev("x", "A", 1, at(2023, 11, 15, 23, 0))) == 1);  // Wednesday evening: not a quiet day
    CHECK(fires(en, ev("x", "A", 1, at(2023, 11, 14, 3, 0))) == 1);   // Tuesday 03:00 belongs to Monday's night: not quiet
    CHECK(fires(en, ev("x", "A", 1, at(2023, 11, 14, 12, 0))) == 1);
    // bypass
    r.quiet.days = 0x7F;
    r.bypassQuietHours = true;
    r.severity = al::Severity::Urgent;
    CHECK(en.setRules({r}));
    CHECK(f(23, 0) == 1);
    {
        const auto night = en.process(ev("x", "A", 1, at(2023, 11, 14, 23, 0)));
        CHECK(night.size() == 1 && night[0].severity == al::Severity::Urgent);
    }
    // the weekday arithmetic itself: 1970-01-01 was a Thursday, 2023-11-14 a Tuesday, 2024-02-29 a Thursday
    al::QuietHours q;
    q.enabled = true;
    q.startMinute = q.endMinute = 0;
    for (int dayMask : {1 << 4}) {
        q.days = static_cast<std::uint8_t>(dayMask);
        CHECK(al::detail::inQuiet(q, 0));
        CHECK(al::detail::inQuiet(q, at(2024, 2, 29, 12, 0)));
        CHECK(!al::detail::inQuiet(q, at(2023, 11, 14, 12, 0)));
        CHECK(!al::detail::inQuiet(q, -86400000LL));       // 1969-12-31, a Wednesday
    }
    q.days = 1u << 3;
    CHECK(al::detail::inQuiet(q, -86400000LL + 1000));
}

// ---- engine management --------------------------------------------------------------------------------------------------------------

void testEngineManagement() {
    al::AlertEngine en;
    std::string err;
    Rule a = rule("a");
    a.keywords = {"x"};
    a.minIntervalMs = 5000;
    Rule b = rule("b");
    b.keywords = {"y"};
    CHECK(en.setRules({a, b}, &err));
    CHECK(fires(en, ev("xy", "A", 1, kBase)) == 2);
    // two rules, both fire on one event
    auto out = en.process(ev("xy", "A", 1, kBase + 6000));
    CHECK(out.size() == 2 && out[0].rule == "a" && out[1].rule == "b");
    // setRules keeps the rate-limit state of a rule that keeps its name
    CHECK(en.setRules({a}, &err));
    CHECK(fires(en, ev("x", "A", 1, kBase + 7000)) == 0);
    // ... and a refused set changes nothing
    Rule bad = rule("");
    CHECK(!en.setRules({b, bad}, &err) && !err.empty());
    CHECK(fires(en, ev("x", "A", 1, kBase + 20000)) == 1);
    Rule dup = a;
    CHECK(!en.setRules({a, dup}, &err) && err.find("duplicate") != std::string::npos);
    Rule badTarget = rule("bt");
    badTarget.icaoAddresses = {"12345"};
    CHECK(!en.setRules({badTarget}, &err) && err.find("ICAO") != std::string::npos);
    badTarget.icaoAddresses = {"12345G"};
    CHECK(!en.setRules({badTarget}));
    badTarget.icaoAddresses.clear();
    badTarget.squawks = {"7800"};                        // 8 is not an octal digit
    CHECK(!en.setRules({badTarget}));
    badTarget.squawks.clear();
    badTarget.mmsis = {"12345678"};
    CHECK(!en.setRules({badTarget}));
    badTarget.mmsis.clear();
    badTarget.callsigns = {"has space"};
    CHECK(!en.setRules({badTarget}));
    std::vector<Rule> many;
    for (int i = 0; i < 501; ++i) { many.push_back(rule("r" + std::to_string(i))); }
    CHECK(!en.setRules(many, &err) && err.find("500") != std::string::npos);
    // an empty rule set is valid and silent
    CHECK(en.setRules({}));
    CHECK(fires(en, ev("x")) == 0 && en.stats().empty());
    // the body is cut on a character boundary
    Rule c = rule("c");
    c.wildcards = {"*"};
    CHECK(en.setRules({c}));
    std::string longText;
    for (int i = 0; i < 300; ++i) { longText += "\xE2\x82\xAC"; }
    const auto o = en.process(ev(longText));
    CHECK(o.size() == 1 && o[0].body.size() <= 203 && o[0].body.size() >= 201 && o[0].body.substr(o[0].body.size() - 3) == "...");
    CHECK((o[0].body.size() - 3) % 3 == 0);
}

// ---- rule files ------------------------------------------------------------------------------------------------------------------------

Rule fullRule() {
    Rule r = rule("Everything \"quoted\" \xC3\xA9");
    r.enabled = false;
    r.severity = al::Severity::Urgent;
    r.keywords = {"mayday", "pan pan"};
    r.wildcards = {"cq ??? de *"};
    r.callsigns = {"BAW123"};
    r.icaoAddresses = {"406B3F"};
    r.mmsis = {"235012345"};
    r.squawks = {"7700"};
    r.decoders = {"ADS-B", "AIS"};
    r.hasFreq = true;
    r.freqMinHz = 1.0e6;
    r.freqMaxHz = 1.2e9;
    r.radio = 3;
    r.minIntervalMs = 1000;
    r.perKeyIntervalMs = 60000;
    r.maxPerHour = 12;
    r.quiet.enabled = true;
    r.quiet.startMinute = 22 * 60 + 15;
    r.quiet.endMinute = 6 * 60;
    r.quiet.utcOffsetMinutes = -330;
    r.quiet.days = 0x41;
    r.bypassQuietHours = true;
    return r;
}

bool sameRule(const Rule& a, const Rule& b) {
    return a.name == b.name && a.enabled == b.enabled && a.severity == b.severity && a.keywords == b.keywords && a.wildcards == b.wildcards && a.callsigns == b.callsigns &&
           a.icaoAddresses == b.icaoAddresses && a.mmsis == b.mmsis && a.squawks == b.squawks && a.decoders == b.decoders && a.hasFreq == b.hasFreq && a.freqMinHz == b.freqMinHz &&
           a.freqMaxHz == b.freqMaxHz && a.radio == b.radio && a.minIntervalMs == b.minIntervalMs && a.perKeyIntervalMs == b.perKeyIntervalMs && a.maxPerHour == b.maxPerHour &&
           a.quiet.enabled == b.quiet.enabled && a.quiet.startMinute == b.quiet.startMinute && a.quiet.endMinute == b.quiet.endMinute &&
           a.quiet.utcOffsetMinutes == b.quiet.utcOffsetMinutes && a.quiet.days == b.quiet.days && a.bypassQuietHours == b.bypassQuietHours;
}

void testRuleFileRoundTrip() {
    const Rule full = fullRule();
    Rule plain = rule("plain");
    plain.keywords = {"x"};
    const std::string text = al::rulesToJson({full, plain});
    const auto r = al::parseRules(text);
    CHECK(r.error.empty());
    CHECK(r.rules.size() == 2);
    if (r.rules.size() == 2) { CHECK(sameRule(r.rules[0], full) && sameRule(r.rules[1], plain)); }
    CHECK(text.find("\"22:15\"") != std::string::npos && text.find("\"version\": 1") != std::string::npos);
    // a hand-written minimal file with defaults
    const auto m = al::parseRules("{\"version\":1,\"rules\":[{\"name\":\"min\"}]}");
    CHECK(m.error.empty() && m.rules.size() == 1 && m.rules[0].enabled && m.rules[0].radio == -1 && m.rules[0].severity == al::Severity::Notice);
    CHECK(al::parseRules("{\"version\":1,\"rules\":[]}").error.empty());
    // files
    const fs::path d = fs::temp_directory_path() / "coref-alerts";
    fs::create_directories(d);
    const std::string p = (d / "rules.json").string();
    std::string err;
    CHECK(al::saveRulesFile(p, {full, plain}, &err));
    CHECK(!fs::exists(p + ".tmp"));
    auto l = al::loadRulesFile(p);
    CHECK(l.error.empty() && l.rules.size() == 2 && sameRule(l.rules[0], full));
    // an invalid set is refused and the file on disk is untouched
    Rule bad = rule("bad");
    bad.mmsis = {"1"};
    const auto before = fs::file_size(p);
    CHECK(!al::saveRulesFile(p, {bad}, &err) && err.find("MMSI") != std::string::npos);
    CHECK(fs::file_size(p) == before && al::loadRulesFile(p).rules.size() == 2);
    // a BOM is accepted
    {
        std::ofstream f(d / "bom.json", std::ios::binary);
        f << "\xEF\xBB\xBF" << al::rulesToJson({plain});
    }
    CHECK(al::loadRulesFile((d / "bom.json").string()).rules.size() == 1);
    CHECK(!al::loadRulesFile((d / "missing.json").string()).error.empty());
    CHECK(!al::loadRulesFile(d.string()).error.empty());                // a directory
    {
        std::ofstream f(d / "big.json", std::ios::binary);
        std::string chunk(1 << 20, ' ');
        f << chunk << chunk;
    }
    CHECK(al::loadRulesFile((d / "big.json").string()).error.find("1 MiB") != std::string::npos);
    CHECK(!al::saveRulesFile((d / "nodir" / "x" / "r.json").string(), {plain}, &err));
    std::error_code ec;
    fs::remove_all(d, ec);
}

void testRuleFileHostile() {
    auto refused = [](const std::string& text, const char* mustMention = nullptr) {
        const auto r = al::parseRules(text);
        const bool ok = !r.error.empty() && r.rules.empty() && (mustMention == nullptr || r.error.find(mustMention) != std::string::npos);
        if (!ok) { std::printf("  NOT REFUSED / wrong message for: %.100s -> '%s'\n", text.c_str(), r.error.c_str()); }
        return ok;
    };
    const std::string pre = "{\"version\":1,\"rules\":[";
    CHECK(refused(""));
    CHECK(refused("not json"));
    CHECK(refused("[]"));
    CHECK(refused("{\"rules\":[]}", "version"));
    CHECK(refused("{\"version\":2,\"rules\":[]}", "version"));
    CHECK(refused("{\"version\":\"1\",\"rules\":[]}", "version"));
    CHECK(refused("{\"version\":1}", "rules"));
    CHECK(refused("{\"version\":1,\"rules\":{}}", "rules"));
    CHECK(refused(pre + "5]}", "rule 1"));
    CHECK(refused(pre + "{}]}", "name"));
    CHECK(refused(pre + "{\"name\":5}]}", "name"));
    CHECK(refused(pre + "{\"name\":\"\"}]}", "name"));
    CHECK(refused(pre + "{\"name\":\"a\\u0001b\"}]}", "control"));
    CHECK(refused(pre + "{\"name\":\"" + std::string(65, 'n') + "\"}]}", "64"));
    CHECK(refused(pre + "{\"name\":\"a\"},{\"name\":\"a\"}]}", "twice"));
    CHECK(refused(pre + "{\"name\":\"a\",\"enabled\":\"yes\"}]}", "enabled"));
    CHECK(refused(pre + "{\"name\":\"a\",\"severity\":\"loud\"}]}", "severity"));
    CHECK(refused(pre + "{\"name\":\"a\",\"severity\":3}]}", "severity"));
    CHECK(refused(pre + "{\"name\":\"a\",\"keywords\":\"x\"}]}", "keywords"));
    CHECK(refused(pre + "{\"name\":\"a\",\"keywords\":[1]}]}", "keywords"));
    CHECK(refused(pre + "{\"name\":\"a\",\"keywords\":[\"\"]}]}", "empty"));
    CHECK(refused(pre + "{\"name\":\"a\",\"keywords\":[\"" + std::string(201, 'k') + "\"]}]}", "200"));
    CHECK(refused(pre + "{\"name\":\"a\",\"wildcards\":[\"x\\ny\"]}]}", "control"));
    CHECK(refused(pre + "{\"name\":\"a\",\"targets\":[]}]}", "targets"));
    CHECK(refused(pre + "{\"name\":\"a\",\"targets\":{\"icao\":[\"12345\"]}}]}", "ICAO"));
    CHECK(refused(pre + "{\"name\":\"a\",\"targets\":{\"mmsi\":[\"12345678\"]}}]}", "MMSI"));
    CHECK(refused(pre + "{\"name\":\"a\",\"targets\":{\"squawks\":[\"8888\"]}}]}", "squawk"));
    CHECK(refused(pre + "{\"name\":\"a\",\"targets\":{\"callsigns\":[\"bad call\"]}}]}", "callsign"));
    CHECK(refused(pre + "{\"name\":\"a\",\"frequency\":{\"minHz\":5}}]}", "frequency"));
    CHECK(refused(pre + "{\"name\":\"a\",\"frequency\":{\"minHz\":5,\"maxHz\":1}}]}", "upside down"));
    CHECK(refused(pre + "{\"name\":\"a\",\"frequency\":{\"minHz\":-5,\"maxHz\":1}}]}", "negative"));
    CHECK(refused(pre + "{\"name\":\"a\",\"frequency\":{\"minHz\":\"1\",\"maxHz\":2}}]}", "frequency"));
    CHECK(refused(pre + "{\"name\":\"a\",\"radio\":-2}]}", "radio"));
    CHECK(refused(pre + "{\"name\":\"a\",\"radio\":1.5}]}", "radio"));
    CHECK(refused(pre + "{\"name\":\"a\",\"radio\":70000}]}", "radio"));
    CHECK(refused(pre + "{\"name\":\"a\",\"rate\":{\"minIntervalMs\":-1}}]}", "interval"));
    CHECK(refused(pre + "{\"name\":\"a\",\"rate\":{\"maxPerHour\":-1}}]}", "maxPerHour"));
    CHECK(refused(pre + "{\"name\":\"a\",\"rate\":{\"maxPerHour\":2000000}}]}", "maxPerHour"));
    CHECK(refused(pre + "{\"name\":\"a\",\"rate\":{\"minIntervalMs\":\"5\"}}]}", "minIntervalMs"));
    CHECK(refused(pre + "{\"name\":\"a\",\"quietHours\":{\"start\":\"24:00\",\"end\":\"06:00\"}}]}", "HH:MM"));
    CHECK(refused(pre + "{\"name\":\"a\",\"quietHours\":{\"start\":\"22:60\",\"end\":\"06:00\"}}]}", "HH:MM"));
    CHECK(refused(pre + "{\"name\":\"a\",\"quietHours\":{\"start\":\"2200\",\"end\":\"06:00\"}}]}", "HH:MM"));
    CHECK(refused(pre + "{\"name\":\"a\",\"quietHours\":{\"start\":\"22:00\"}}]}", "HH:MM"));
    CHECK(refused(pre + "{\"name\":\"a\",\"quietHours\":{\"start\":\"22:00\",\"end\":\"06:00\",\"utcOffsetMinutes\":900}}]}", "14 hours"));
    CHECK(refused(pre + "{\"name\":\"a\",\"quietHours\":{\"start\":\"22:00\",\"end\":\"06:00\",\"days\":[7]}}]}", "0..6"));
    CHECK(refused(pre + "{\"name\":\"a\",\"quietHours\":{\"start\":\"22:00\",\"end\":\"06:00\",\"days\":[0,1,2,3,4,5,6,0]}}]}", "0..6"));
    CHECK(refused(pre + "{\"name\":\"a\",\"quietHours\":[]}]}", "quietHours"));
    // the FIRST bad rule of several is named, and nothing is loaded
    {
        const auto r = al::parseRules(pre + "{\"name\":\"good\",\"keywords\":[\"x\"]},{\"name\":\"bad\",\"targets\":{\"mmsi\":[\"1\"]}}]}");
        CHECK(r.rules.empty() && r.error.find("rule 2 ('bad')") != std::string::npos);
    }
    // size, NUL, nesting
    CHECK(refused(std::string(2 << 20, ' '), "1 MiB"));
    CHECK(refused(std::string("{\"version\":1,\"rules\":[]}\0", 25), "NUL"));
    CHECK(refused(std::string(200000, '['), "nested"));
    {
        std::string deep = "{\"version\":1,\"rules\":[{\"name\":\"a\",\"x\":";
        for (int i = 0; i < 50; ++i) { deep += "["; }
        for (int i = 0; i < 50; ++i) { deep += "]"; }
        deep += "}]}";
        CHECK(refused(deep, "nested"));
    }
    // brackets inside strings are not nesting
    CHECK(al::parseRules(pre + "{\"name\":\"[[[[[[[[[[[[[\",\"keywords\":[\"{{{{{{{{{{{{\\\"[[[\"]}]}").error.empty());
    // 501 rules
    {
        std::string t = pre;
        for (int i = 0; i < 501; ++i) { t += (i ? "," : "") + std::string("{\"name\":\"r") + std::to_string(i) + "\"}"; }
        t += "]}";
        CHECK(refused(t, "500"));
    }
    // unknown keys are ignored
    CHECK(al::parseRules(pre + "{\"name\":\"a\",\"future\":{\"x\":1}}],\"extra\":5}").rules.size() == 1);
    // random bytes never crash
    {
        unsigned s = 99;
        int crashes = 0;
        for (int n = 0; n < 200; ++n) {
            std::string t;
            for (int i = 0; i < 300; ++i) { s = s * 1103515245u + 12345u; t.push_back(static_cast<char>((s >> 16) & 0xFF)); }
            const auto r = al::parseRules(t);
            if (!r.error.empty() && !r.rules.empty()) { ++crashes; }
        }
        CHECK(crashes == 0);
    }
}

// ---- sinks ---------------------------------------------------------------------------------------------------------------------------

struct FailingSink : al::NotificationSink {
    int calls = 0;
    const char* name() const override { return "failing"; }
    bool notify(const al::Notification&, std::string* err) override { ++calls; if (err != nullptr) { *err = "no desktop session"; } return false; }
};
struct ThrowingSink : al::NotificationSink {
    const char* name() const override { return "throwing"; }
    bool notify(const al::Notification&, std::string*) override { throw 5; }
};

void testSinks() {
    al::AlertEngine en;
    Rule r = rule("sink");
    r.keywords = {"mayday"};
    r.severity = al::Severity::Urgent;
    CHECK(en.setRules({r}));
    const auto alerts = en.process(ev("MAYDAY MAYDAY from G-ABCD"));
    CHECK(alerts.size() == 1);
    al::RecordingSink rec;
    std::string err;
    CHECK(al::deliver(alerts, rec, &err) == 1 && err.empty());
    const auto items = rec.items();
    CHECK(items.size() == 1);
    if (!items.empty()) {
        CHECK(items[0].title == "sink: ADS-B 1090.0000 MHz" && items[0].severity == al::Severity::Urgent && items[0].rule == "sink");
        CHECK(items[0].body == "MAYDAY MAYDAY from G-ABCD\n(keyword: mayday)" && items[0].timeMs == kBase);
    }
    al::NullSink none;
    CHECK(al::deliver(alerts, none) == 1);
    FailingSink fail;
    CHECK(al::deliver(alerts, fail, &err) == 0 && err == "no desktop session" && fail.calls == 1);
    ThrowingSink thrower;
    err.clear();
    CHECK(al::deliver(alerts, thrower, &err) == 0 && err.find("threw") != std::string::npos);
    CHECK(al::deliver({}, rec) == 0);
    // the recording sink is bounded
    for (int i = 0; i < 1500; ++i) { rec.notify(al::Notification{}, nullptr); }
    CHECK(rec.items().size() == 1000);
}

// ---- threads and speed -------------------------------------------------------------------------------------------------------------------

void testConcurrencyAndSpeed() {
    al::AlertEngine en;
    Rule a = rule("a");
    a.keywords = {"hello"};
    Rule b = rule("b");
    b.keywords = {"hello"};
    b.perKeyIntervalMs = 1;
    CHECK(en.setRules({a, b}));
    std::atomic<bool> stop{false};
    std::atomic<long> processed{0}, alertsSeen{0};
    std::thread worker([&] {
        std::int64_t t = kBase;
        while (!stop.load()) {
            alertsSeen += static_cast<long>(en.process(ev("hello world", "A", 1, t += 10)).size());
            ++processed;
        }
    });
    std::thread swapper([&] {
        for (int i = 0; i < 2000 && !stop.load(); ++i) {
            std::string err;
            if (!en.setRules(i % 2 ? std::vector<Rule>{a, b} : std::vector<Rule>{b, a}, &err)) { CHECK(false); }
            en.stats();
        }
    });
    swapper.join();
    stop.store(true);
    worker.join();
    CHECK(processed.load() > 100 && alertsSeen.load() > 100);
    // speed: 100 rules of mixed kinds over 200000 events
    std::vector<Rule> rules;
    for (int i = 0; i < 100; ++i) {
        Rule r = rule("r" + std::to_string(i));
        switch (i % 4) {
            case 0: r.keywords = {"neverappears" + std::to_string(i)}; break;
            case 1: r.wildcards = {"*never" + std::to_string(i) + "*"}; break;
            case 2: r.callsigns = {"ZZ" + std::to_string(1000 + i)}; break;
            default: r.icaoAddresses = {"A0000" + std::to_string(i % 10)}; r.decoders = {"ADS-B"}; break;
        }
        rules.push_back(r);
    }
    al::AlertEngine fast;
    CHECK(fast.setRules(rules));
    DecodeEvent e = ev("msg 12345 from KJ4XYZ-33 hello world and some more text to scan through for the benchmark");
    e.fields.emplace_back("callsign", "KJ4XYZ-33");
    e.fields.emplace_back("icao", "406B3F");
    const auto t0 = std::chrono::steady_clock::now();
    const int kN = std::getenv("COREF_SKIP_BIG") != nullptr ? 2000 : 200000;
    std::size_t total = 0;
    for (int i = 0; i < kN; ++i) { e.timeMs = kBase + i; total += fast.process(e).size(); }
    const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("  100 rules x %d events: %.2f s, %.0f events/s (%.1f M rule evaluations/s), %zu alerts\n", kN, sec, kN / sec, kN * 100.0 / sec / 1e6, total);
    CHECK(total == 0);
}

}  // namespace

int main() {
    std::printf("alert rules\n");
    testKeywords();
    testWildcards();
    testTargets();
    testFilters();
    testRateLimits();
    testQuietHours();
    testEngineManagement();
    testRuleFileRoundTrip();
    testRuleFileHostile();
    testSinks();
    testConcurrencyAndSpeed();
    return testSummary("test_alert_rules");
}
