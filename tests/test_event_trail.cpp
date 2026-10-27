// core/event_trail.hpp - the bounded ring of recent state transitions that rides in crash and
// freeze reports (field-early-warning).
//
// WHAT IS HELD: the ring keeps the LAST thirty entries and reads them back oldest first; every
// word on the wire is from a closed table that is itself held to its source (the driver words to
// the health vocabulary, the reload steps to core/plugin_reload_phases.hpp, the modes to the
// application's own list); nothing but those words can be written, and the uploader's
// sanitise() refuses anything else with the same tables - so a hand-edited report, a path, a
// serial number or a frequency cannot ride in the field; the formatter never overruns a
// buffer; and writers racing a reader never make it print a torn entry.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "core/event_trail.hpp"
#include "core/health_events.hpp"
#include "core/plugin_reload_phases.hpp"
#include "test_check.hpp"

namespace trail = cascade::core::trail;
using trail::Event;

namespace {

std::string fmt(std::uint64_t nowMs) {
    char buf[trail::kMaxFormatted];
    const std::size_t n = trail::format(buf, sizeof(buf), nowMs);
    CHECK(std::strlen(buf) == n);
    return std::string(buf, n);
}

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> v;
    std::size_t at = 0;
    while (at <= s.size()) {
        std::size_t end = s.find(';', at);
        if (end == std::string::npos) { end = s.size(); }
        if (!s.empty()) { v.push_back(s.substr(at, end - at)); }
        at = end + 1;
    }
    return v;
}

std::string readSource(const char* rel) {
    std::ifstream f(std::string(CASCADE_SOURCE_DIR) + "/" + rel, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void testTheTablesAreHeldToTheirSources() {
    // The events and their words.
    CHECK(trail::kEventCount == 8);
    const char* events[] = {"src-open", "src-close", "mode", "rescan", "rec-start", "rec-stop", "plug-fit", "plug-unfit"};
    for (unsigned i = 0; i < 8; ++i) { CHECK(std::string(trail::eventWord(static_cast<Event>(i + 1))) == events[i]); }
    CHECK(std::string(trail::eventWord(Event::None)).empty());
    // Every word is the wire's grammar: lower case, digits, hyphen, within the site's bound.
    for (unsigned e = 1; e <= trail::kEventCount; ++e) {
        const Event ev = static_cast<Event>(e);
        for (unsigned q = 0; q <= trail::qualifierCount(ev); ++q) {
            std::string w = trail::eventWord(ev);
            if (q > 0) { w += std::string(".") + trail::qualifierWord(ev, q); }
            CHECK(w.size() <= trail::kMaxEventChars);
            CHECK(w[0] >= 'a' && w[0] <= 'z');
            for (char c : w) { CHECK((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.'); }
        }
    }
    // The driver words ARE the health vocabulary's, in its order.
    namespace health = cascade::core::health;
    std::vector<std::string> drivers;
    for (const auto& v : health::vocabulary()) {
        if (v.name == "radio_open") { drivers = v.qualifiers.at(0); }
    }
    CHECK(!drivers.empty());
    CHECK(trail::qualifierCount(Event::SrcOpen) == drivers.size());
    CHECK(trail::qualifierCount(Event::SrcClose) == drivers.size());
    for (std::size_t i = 0; i < drivers.size(); ++i) {
        CHECK(drivers[i] == trail::qualifierWord(Event::SrcOpen, static_cast<unsigned>(i) + 1));
        CHECK(drivers[i] == trail::qualifierWord(Event::SrcClose, static_cast<unsigned>(i) + 1));
    }
    // The reload steps ARE core/plugin_reload_phases.hpp's, with the spaces made hyphens.
    CHECK(trail::qualifierCount(Event::Rescan) == static_cast<unsigned>(cascade::core::kPluginReloadPhaseCount));
    for (int i = 0; i < cascade::core::kPluginReloadPhaseCount; ++i) {
        std::string name = cascade::core::kPluginReloadPhaseNames[i];
        for (char& c : name) { if (c == ' ') { c = '-'; } }
        CHECK(name == trail::qualifierWord(Event::Rescan, static_cast<unsigned>(i) + 1));
    }
    // The modes ARE the application's list (read from its source: the list is file-local there).
    const std::string src = readSource("src/core/receiver_tables.hpp");
    CHECK(!src.empty());
    const std::size_t at = src.find("kModeNames[8] = {");
    CHECK(at != std::string::npos);
    if (at != std::string::npos) {
        const std::string tail = src.substr(at, 200);
        std::vector<std::string> names;
        for (std::size_t p = tail.find('"'); p != std::string::npos && names.size() < 8;) {
            const std::size_t e = tail.find('"', p + 1);
            names.push_back(tail.substr(p + 1, e - p - 1));
            p = tail.find('"', e + 1);
        }
        CHECK(names.size() == 8);
        CHECK(trail::qualifierCount(Event::Mode) == 8);
        for (std::size_t i = 0; i < names.size(); ++i) {
            std::string lower = names[i];
            for (char& c : lower) { c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c); }
            CHECK(lower == trail::qualifierWord(Event::Mode, static_cast<unsigned>(i) + 1));
        }
    }
    // The other events take no qualifier at all.
    for (Event e : {Event::RecStart, Event::RecStop, Event::PlugFit, Event::PlugUnfit}) {
        CHECK(trail::qualifierCount(e) == 0);
    }
}

void testTheRingReadsBackOldestFirstWithSecondsBeforeNow() {
    trail::resetForTest();
    CHECK(fmt(1000000).empty());
    const unsigned rtl = trail::qualifierIndex(Event::SrcOpen, "rtlsdr");
    CHECK(rtl != 0);
    trail::noteAt(Event::SrcOpen, rtl, 100000);
    trail::noteAt(Event::Mode, trail::qualifierIndex(Event::Mode, "wfm"), 107000);
    trail::noteAt(Event::RecStart, 0, 109500);
    trail::noteAt(Event::Rescan, trail::qualifierIndex(Event::Rescan, "unload"), 112000);
    // 112 s after the first, reported at 112.9 s: whole seconds, rounded down.
    CHECK(fmt(112900) == "12:src-open.rtlsdr;5:mode.wfm;3:rec-start;0:rescan.unload");
    // The same moment read twice is the same text (reading changes nothing).
    CHECK(fmt(112900) == fmt(112900));
    // A clock that reads EARLIER than the entry (a report written by another clock) is 0, not a wrap.
    CHECK(fmt(50000) == "0:src-open.rtlsdr;0:mode.wfm;0:rec-start;0:rescan.unload");
    // And the figure is bounded.
    CHECK(fmt(100000 + 5000000000ull) == "999999:src-open.rtlsdr;999999:mode.wfm;999999:rec-start;999999:rescan.unload");
}

void testTheRingKeepsTheLastThirty() {
    trail::resetForTest();
    for (int i = 0; i < 45; ++i) {
        // 45 entries, one a second; the mode alternates so each is recognisable.
        trail::noteAt(Event::Mode, static_cast<unsigned>(i % 8) + 1, 1000u * static_cast<unsigned>(i + 1));
    }
    const std::vector<std::string> v = split(fmt(46000));
    CHECK(v.size() == trail::kEntries);
    // Entries 16..44 (0-based 15..44) survive: the oldest kept was written at 16 s, 30 s before now.
    CHECK(v.front() == "30:mode." + std::string(trail::qualifierWord(Event::Mode, 15 % 8 + 1)));
    CHECK(v.back() == "1:mode." + std::string(trail::qualifierWord(Event::Mode, 44 % 8 + 1)));
    // The longest text the ring can write fits the documented buffer.
    trail::resetForTest();
    for (int i = 0; i < 40; ++i) {
        trail::noteAt(Event::Rescan, trail::qualifierIndex(Event::Rescan, "panels-and-map"), 1);
    }
    CHECK(fmt(1ull + 999999000ull).size() < trail::kMaxFormatted);
}

void testOnlyTheVocabularyCanBeWritten() {
    trail::resetForTest();
    // An event with qualifiers is written only with a real one; one without, only without.
    trail::noteAt(Event::SrcOpen, 0, 1);
    trail::noteAt(Event::SrcOpen, 99, 1);
    trail::noteAt(Event::Mode, 9, 1);
    trail::noteAt(Event::RecStart, 1, 1);
    trail::noteAt(Event::None, 0, 1);
    trail::noteAt(static_cast<Event>(200), 0, 1);
    CHECK(fmt(1000).empty());
    // A driver this build does not know is "other"; a name that merely LOOKS like one is too.
    trail::noteSource(true, "rtlsdr");
    trail::noteSource(false, "C:\\Users\\steve\\secret serial 0000000123");
    trail::noteSource(true, "");
    const std::vector<std::string> v = split(fmt(trail::kMaxSecs));
    CHECK(v.size() == 3);
    CHECK(v.size() == 3 && v[1].find(":src-close.other") != std::string::npos);
    CHECK(v.size() == 3 && v[2].find(":src-open.other") != std::string::npos);
    CHECK(fmt(0).find("secret") == std::string::npos);
    CHECK(fmt(0).find("Users") == std::string::npos);
    // Modes: the application's index, and nothing out of range.
    trail::resetForTest();
    trail::noteMode(-1);
    trail::noteMode(8);
    trail::noteMode(1000);
    CHECK(fmt(10).empty());
    trail::noteMode(1);
    CHECK(fmt(10 + 3000).find(":mode.wfm") != std::string::npos);
    // The PhaseClock's own spelling is mapped, anything else is not recorded.
    trail::resetForTest();
    trail::noteRescanStep("panels and map");
    trail::noteRescanStep("unload");
    trail::noteRescanStep("some plugin name");
    trail::noteRescanStep("panels-and-map");   // the wire spelling is not the clock's
    trail::noteRescanStep("");
    trail::noteRescanStep(nullptr);
    const std::vector<std::string> r = split(fmt(trail::kMaxSecs));
    CHECK(r.size() == 2);
    CHECK(r.size() == 2 && r[0].find(":rescan.panels-and-map") != std::string::npos);
    CHECK(r.size() == 2 && r[1].find(":rescan.unload") != std::string::npos);
    trail::resetForTest();
}

void testTheFormatterNeverOverrunsABuffer() {
    trail::resetForTest();
    for (int i = 0; i < 30; ++i) {
        trail::noteAt(Event::SrcOpen, trail::qualifierIndex(Event::SrcOpen, "airspyhf"), 1000u + static_cast<unsigned>(i));
    }
    const std::string whole = fmt(5000);
    for (std::size_t cap = 0; cap < 64; ++cap) {
        // A guard after the buffer: nothing may be written past `cap`.
        std::vector<char> mem(cap + 16, '\x7e');
        const std::size_t n = trail::format(mem.data(), cap, 5000);
        CHECK(n < (cap == 0 ? 1 : cap));
        for (std::size_t i = cap; i < mem.size(); ++i) { CHECK(mem[i] == '\x7e'); }
        if (cap > 0) {
            CHECK(mem[n] == '\0');
            // What it wrote is a prefix of the whole text, ending on an entry boundary.
            CHECK(whole.compare(0, n, std::string(mem.data(), n)) == 0);
            CHECK(n == 0 || n == whole.size() || whole[n] == ';');
            // And is itself valid.
            CHECK(trail::sanitise(std::string(mem.data(), n)) == std::string(mem.data(), n));
        }
    }
    CHECK(trail::format(nullptr, 10, 0) == 0);
    trail::resetForTest();
}

void testTheUploadersCheckRefusesEverythingElse() {
    // Valid.
    for (const char* ok : {"0:rec-start", "412:src-open.rtlsdr", "999999:rescan.panels-and-map",
                           "3:mode.cw", "7:plug-unfit", "1:src-close.other"}) {
        if (!trail::validEntry(ok)) { std::printf("FAIL %s was refused\n", ok); }
        CHECK(trail::validEntry(ok));
    }
    // Hostile or malformed.
    for (const char* bad : {"", ":", "5:", ":rec-start", "x:rec-start", "-1:rec-start", "1000000:rec-start",
                            "0000000:rec-start", "5:src-open", "5:src-open.", "5:src-open.RTLSDR",
                            "5:src-open.rtlsdr.extra", "5:rec-start.x", "5:mode.wfm2", "5:mode",
                            "5:rescan.panels and map", "5:rescan.some-plugin", "5:rec_start", "5:REC-START",
                            "5 :rec-start", " 5:rec-start", "5:rec-start ", "5:rec-start\n",
                            "5:C:\\Users\\steve\\file.wav", "5:/home/steve/rec", "5:src-open.192.168.1.7",
                            "5:src-open.0000000123", "5:mode.100.1e6", "5;rec-start", "5:rec-start;6:rec-stop",
                            "\x01:rec-start", "5:\xE2\x82\xAC", "５:rec-start"}) {
        if (trail::validEntry(bad)) { std::printf("FAIL \"%s\" was accepted\n", bad); }
        CHECK(!trail::validEntry(bad));
    }
    // sanitise keeps the valid entries in order, drops the rest, and the newest thirty.
    CHECK(trail::sanitise("").empty());
    CHECK(trail::sanitise("garbage").empty());
    CHECK(trail::sanitise("9:rec-start;C:\\secret;8:rec-stop;;x;7:mode.am") == "9:rec-start;8:rec-stop;7:mode.am");
    std::string many;
    for (int i = 0; i < 40; ++i) { many += (many.empty() ? "" : ";") + std::to_string(100 - i) + ":rec-start"; }
    const std::vector<std::string> kept = split(trail::sanitise(many));
    CHECK(kept.size() == trail::kEntries);
    CHECK(kept.front() == "90:rec-start");     // entries 11..40 of 40: the newest thirty
    CHECK(kept.back() == "61:rec-start");
    // Everything format() writes survives sanitise() unchanged.
    trail::resetForTest();
    trail::noteSource(true, "hackrf");
    trail::noteMode(4);
    trail::note(Event::PlugFit);
    trail::noteRescanStep("restart");
    const std::string text = trail::formatNow();
    CHECK(!text.empty());
    CHECK(trail::sanitise(text) == text);
    trail::resetForTest();
}

// EVERY WRITER IS WHERE THE DOCUMENTATION SAYS, and only there: each kind of entry is written at
// one place in the application (so a second, unreviewed writer cannot appear), each fault writer
// puts the trail in its report, and the uploader checks it. The window-level behaviour of the
// mode, source and stop writers is exercised by tests/test_health_app.cpp; the reload step by
// tests/test_diag_hang.cpp; this is the part a behavioural test cannot see - that the places that
// write are the places that were reviewed.
int occurrences(const std::string& text, const std::string& needle) {
    int n = 0;
    for (std::size_t at = 0; (at = text.find(needle, at)) != std::string::npos; at += needle.size()) { ++n; }
    return n;
}

void testEveryWriterIsWhereTheDocumentationSays() {
    struct Site {
        const char* file;
        const char* needle;
        int count;
    };
    const Site sites[] = {
        // a radio opened: the one function every open is counted through (health_events.cpp)
        {"src/core/health_events.cpp", "trail::noteSource(true", 1},
        // the receiver stopped, the demodulator changed
        {"src/engine/engine.cpp", "trail::noteSource(false", 1},
        {"src/engine/engine.cpp", "trail::noteMode(index)", 1},
        // a take began (the file arrived and the recorder armed) and a take ended
        {"src/engine/engine.cpp", "trail::Event::RecStart", 1},
        {"src/engine/engine.cpp", "trail::Event::RecStop", 1},
        // a plugin installed or updated, a plugin removed - by the person
        {"src/engine/engine_store.cpp", "trail::Event::PlugFit", 1},
        {"src/engine/engine.cpp", "trail::Event::PlugUnfit", 1},
        // a reload step entered: the publishing clock, the one place steps are announced
        {"src/core/phase_clock.hpp", "trail::noteRescanStep(name)", 1},
        // the four fault writers put the trail in the process block of their report
        {"src/core/crash_handler.cpp", "trail::format(", 1},
        {"src/core/crash_handler_posix.cpp", "trail::format(", 1},
        {"src/core/hang_watchdog.cpp", "trail::format(", 1},
        {"src/core/hang_watchdog.cpp", "writeTrailLine(out);", 2},
        // and the uploader reads it back through the closed tables
        {"src/core/crash_upload.cpp", "trail::sanitise(", 2},
    };
    for (const Site& s : sites) {
        const std::string code = readSource(s.file);
        CHECK(!code.empty());
        const int got = occurrences(code, s.needle);
        if (got != s.count) {
            std::printf("FAIL %s: \"%s\" appears %d times, expected %d\n", s.file, s.needle, got, s.count);
        }
        CHECK(got == s.count);
    }
    // Nothing else in the application writes one: every use of the writing API outside the
    // trail's own files is one of the sites above (the hot-path clock hook included).
    int total = 0;
    for (const char* dir : {"src/gui", "src/engine", "src/core", "src/source", "src/sink", "src/net", "src/dsp", "src/usb"}) {
        namespace fs = std::filesystem;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(fs::path(CASCADE_SOURCE_DIR) / dir, ec)) {
            if (!e.is_regular_file()) { continue; }
            const std::string name = e.path().filename().string();
            if (name == "event_trail.cpp" || name == "event_trail.hpp") { continue; }
            const std::string code = readSource((std::string(dir) + "/" + name).c_str());
            total += occurrences(code, "trail::note(") + occurrences(code, "trail::noteSource(") +
                     occurrences(code, "trail::noteMode(") + occurrences(code, "trail::noteRescanStep(");
        }
    }
    // noteSource x2 (open, close), noteMode, noteRescanStep, and note() x4 (rec start/stop, plug
    // fit/unfit; spelled `trail::note(cascade::core::trail::Event::...)` is not matched by the
    // `trail::note(` prefix twice - each is one call).
    std::printf("event trail: %d writer calls outside the trail's own files\n", total);
    CHECK(total == 8);
}

void testWritersRacingAReaderNeverProduceATornEntry() {
    trail::resetForTest();
    std::atomic<bool> stop{false};
    std::vector<std::thread> writers;
    for (int t = 0; t < 4; ++t) {
        writers.emplace_back([&stop, t] {
            unsigned i = 0;
            while (!stop.load()) {
                trail::note(Event::Mode, (i++ + static_cast<unsigned>(t)) % 8 + 1);
                trail::note(Event::SrcOpen, (i + static_cast<unsigned>(t)) % 14 + 1);
                trail::note(Event::RecStart);
            }
        });
    }
    int reads = 0;
    for (int i = 0; i < 3000; ++i) {
        const std::string text = trail::formatNow();
        // Every entry that came back is a whole, valid one; none is cut or mixed.
        CHECK(trail::sanitise(text) == text);
        CHECK(text.size() < trail::kMaxFormatted);
        ++reads;
    }
    stop.store(true);
    for (auto& w : writers) { w.join(); }
    CHECK(reads == 3000);
    trail::resetForTest();
}

}  // namespace

int main() {
    testTheTablesAreHeldToTheirSources();
    testTheRingReadsBackOldestFirstWithSecondsBeforeNow();
    testTheRingKeepsTheLastThirty();
    testOnlyTheVocabularyCanBeWritten();
    testTheFormatterNeverOverrunsABuffer();
    testTheUploadersCheckRefusesEverythingElse();
    testEveryWriterIsWhereTheDocumentationSays();
    testWritersRacingAReaderNeverProduceATornEntry();
    return testSummary("test_event_trail");
}
