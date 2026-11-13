// test_rfexplorer_source.cpp - the RF Explorer spectrum analyser as a sweep source (source/rfexplorer_protocol, source/rfexplorer_serial_win) proven
// against a STAND-IN device written here from the maker's published "RF Explorer UART API interface specification" (message syntax, field widths, units)
// and the maker's own 2013 client under the Unlicense (500000 baud, command framing, $S frame). The stand-in plays the device on a fake link and, for the
// serial link's own I/O, behind a LOCAL NAMED PIPE (no real COM port is opened anywhere in this file). No RF Explorer was touched: UNVERIFIED ON HARDWARE.
// Generated sweeps carry noise, a tone off the grid (a frequency offset) and a device step that is not the nominal one (a step-size error).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "source/rfexplorer_protocol.hpp"
#include "source/rfexplorer_serial_win.hpp"
#include "test_check.hpp"

using namespace cascade::source::rfexplorer;
using cascade::core::SweepLine;
using Bytes = std::vector<std::uint8_t>;

namespace {

Bytes bytesOf(const std::string& s) { return Bytes(s.begin(), s.end()); }
bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

// ---- frames written from the page ----------------------------------------------------------------------------------------------------
std::string configText(long startKHz, long stepHz, int top, int bottom, long points, int mode = 0, long minK = 15000, long maxK = 2700000, long spanK = 112000,
                       int fields = 13) {
    char b[200];
    int n = std::snprintf(b, sizeof b, "%07ld,%07ld,%04d,%04d,%04ld,%d,%03d,%07ld,%07ld,%07ld", startKHz, stepHz, top, bottom, points, 0, mode, minK, maxK, spanK);
    if (fields >= 11) { n += std::snprintf(b + n, sizeof b - static_cast<std::size_t>(n), ",%05d", 100); }
    if (fields >= 13) { n += std::snprintf(b + n, sizeof b - static_cast<std::size_t>(n), ",%04d,%03d", 0, 0); }
    return std::string(b, static_cast<std::size_t>(n));
}
Bytes configLine(const std::string& text, bool lfCr = false) { return bytesOf("#C2-F:" + text + (lfCr ? "\n\r" : "\r\n")); }
Bytes setupLine(int model = 5, int exp = 255, const char* fw = "01.12") {
    char b[64];
    std::snprintf(b, sizeof b, "#C2-M:%03d,%03d,%s\r\n", model, exp, fw);
    return bytesOf(b);
}
// A sweep frame. kind 'S' with the count byte read as the count (2013 client) or as (count+1)*16 (the page); 's' as the page; 'z' as a 16-bit word.
Bytes sweepFrame(char kind, const std::vector<std::uint8_t>& amp, bool direct = true, bool lfCr = false) {
    Bytes b{'$', static_cast<std::uint8_t>(kind)};
    const std::size_t n = amp.size();
    if (kind == 'z') {
        b.push_back(static_cast<std::uint8_t>(n >> 8));
        b.push_back(static_cast<std::uint8_t>(n & 0xFF));
    } else if (kind == 'S' && direct) {
        b.push_back(static_cast<std::uint8_t>(n));
    } else {
        b.push_back(static_cast<std::uint8_t>(n / 16 - 1));
    }
    b.insert(b.end(), amp.begin(), amp.end());
    b.push_back(lfCr ? 0x0A : 0x0D);
    b.push_back(lfCr ? 0x0D : 0x0A);
    return b;
}
std::vector<std::uint8_t> ampRamp(std::size_t n) {
    std::vector<std::uint8_t> a(n);
    for (std::size_t i = 0; i < n; ++i) { a[i] = static_cast<std::uint8_t>((i * 3 + 20) & 0xFF); }
    return a;
}

std::vector<Item> feedAll(Demux& d, const Bytes& b, std::size_t chunk = 0) {
    std::vector<Item> out;
    if (chunk == 0) { chunk = b.size(); }
    for (std::size_t i = 0; i < b.size(); i += chunk) { d.feed(b.data() + i, std::min(chunk, b.size() - i), out); }
    return out;
}

// ---- the stand-in device --------------------------------------------------------------------------------------------------------------
struct Device {
    std::mutex mu;
    Bytes out;                  // bytes the device has queued
    long startKHz = 100000, stepHz = 898000, points = 112, minK = 15000, maxK = 2700000, spanK = 112000;
    int top = -10, bottom = -120, mode = 0, model = 5;
    bool analyzer = true, silent = false, ignoreConfig = false, noSetup = false, running = true, hold = false;
    int staleSweeps = 0;        // sweeps of the OLD span sent after a configuration command, before the new configuration
    double toneHz = 150012345.0, noiseDb = 1.5;
    bool eolLfCr = false, directCount = true;
    char frameKind = 'S';
    std::vector<std::string> commands;  // the command bodies the device received
    std::vector<std::string> wrongSize; // commands whose size byte was not their length
    std::mt19937 rng{777};
    std::uint64_t sweepsSent = 0;
    Bytes inbuf;

    std::string currentConfig() { return configText(startKHz, stepHz, top, bottom, points, mode, minK, maxK, spanK); }
    void queue(const Bytes& b) { out.insert(out.end(), b.begin(), b.end()); }
    void announce() {
        if (!analyzer) {
            queue(bytesOf("#C3-M:060,255,01.05\r\n"));
            queue(bytesOf("#C3-*:0100000,0100000,0001,0000000,0,0,0,0,0,0,0,1,00000\r\n"));
            return;
        }
        if (!noSetup) { queue(setupLine(model)); }
        queue(configLine(currentConfig(), eolLfCr));
    }
    void sweep() {
        std::vector<std::uint8_t> a(static_cast<std::size_t>(points));
        std::normal_distribution<double> nd(0.0, noiseDb);
        for (long i = 0; i < points; ++i) {
            const double f = static_cast<double>(startKHz) * 1000.0 + static_cast<double>(stepHz) * static_cast<double>(i);
            double level = -100.0 + nd(rng);
            const double d = (f - toneHz) / static_cast<double>(stepHz);
            level += 60.0 * std::exp(-d * d / 2.0);  // a tone 60 dB above the floor, off the grid
            const double v = std::clamp(-level * 2.0, 0.0, 255.0);
            a[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(std::lround(v));
        }
        queue(sweepFrame(frameKind, a, directCount, eolLfCr));
        ++sweepsSent;
    }
    void onBytes(const std::uint8_t* d, std::size_t n) {
        std::lock_guard<std::mutex> lk(mu);
        inbuf.insert(inbuf.end(), d, d + n);
        while (inbuf.size() >= 2) {
            if (inbuf[0] != '#') { inbuf.erase(inbuf.begin()); continue; }
            const std::size_t size = inbuf[1];
            if (size < 3 || size > 64) { inbuf.erase(inbuf.begin()); continue; }
            if (inbuf.size() < size) { return; }
            const std::string body(reinterpret_cast<const char*>(&inbuf[2]), size - 2);
            inbuf.erase(inbuf.begin(), inbuf.begin() + static_cast<std::ptrdiff_t>(size));
            commands.push_back(body);
            if (silent) { continue; }
            if (body == "C0") {
                announce();
            } else if (body == "CH") {
                hold = true;
                running = false;
            } else if (body.compare(0, 5, "C2-F:") == 0 && analyzer) {
                if (ignoreConfig) { announce(); continue; }
                long s = 0, e = 0;
                int t = 0, b = 0;
                if (std::sscanf(body.c_str() + 5, "%ld,%ld,%d,%d", &s, &e, &t, &b) == 4) {
                    for (int i = 0; i < staleSweeps; ++i) { sweep(); }
                    startKHz = s;
                    stepHz = (e - s) * 1000 / (points - 1);
                    top = t;
                    bottom = b;
                    announce();
                }
            }
        }
    }
    // What the link returns to the host: queued bytes first (in the chunk size asked), then a fresh sweep while the analyser runs.
    Bytes pull(std::size_t cap) {
        std::lock_guard<std::mutex> lk(mu);
        if (out.empty() && running && !silent && analyzer && sweepsSent < 100000) { sweep(); }
        const std::size_t n = std::min(cap, out.size());
        Bytes r(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(n));
        out.erase(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(n));
        return r;
    }
};

struct FakeLink : cascade::source::tinysa::SerialLink {
    Device& dev;
    std::size_t chunk = 97;     // the device's bytes arrive in odd pieces
    bool failWrite = false, failRead = false;
    long readsBeforeFail = -1;
    std::size_t reads = 0, writes = 0;
    explicit FakeLink(Device& d) : dev(d) {}
    bool write(const std::uint8_t* d, std::size_t n) override {
        ++writes;
        if (failWrite) { return false; }
        dev.onBytes(d, n);
        return true;
    }
    long read(std::uint8_t* b, std::size_t cap) override {
        ++reads;
        if (failRead || (readsBeforeFail >= 0 && static_cast<long>(reads) > readsBeforeFail)) { return -1; }
        const Bytes r = dev.pull(std::min(cap, chunk));
        if (r.empty()) { return 0; }
        std::memcpy(b, r.data(), r.size());
        return static_cast<long>(r.size());
    }
};

// ---------------------------------------------------------------------------------------------------------------------------------------
void testCommands() {
    CHECK(requestConfigCommand() == std::string("#\x04" "C0", 4));
    CHECK(holdCommand() == std::string("#\x04" "CH", 4));
    CHECK(dumpScreenOffCommand() == std::string("#\x04" "D0", 4));
    std::string why;
    const std::string c = analyzerConfigCommand(100e6, 200e6, -10, -120, &why);
    CHECK(c == std::string("#\x20" "C2-F:0100000,0200000,-010,-120"));
    CHECK(c.size() == 32 && static_cast<std::uint8_t>(c[1]) == 32);  // the page: <Size>=32 bytes
    CHECK(analyzerConfigCommand(88.0e6, 108.0e6, 0, -100) == std::string("#\x20" "C2-F:0088000,0108000,0000,-100"));
    CHECK(analyzerConfigCommand(100.4e3, 200.6e3, -10, -120) == std::string("#\x20" "C2-F:0000100,0000201,-010,-120"));
    for (const auto& r : std::vector<std::pair<std::string, std::string>>{
             {"start at or above stop", analyzerConfigCommand(200e6, 100e6, -10, -120, &why)},
             {"equal", analyzerConfigCommand(100e6, 100e6, -10, -120, &why)},
             {"zero start", analyzerConfigCommand(0, 100e6, -10, -120, &why)},
             {"negative", analyzerConfigCommand(-5e6, 100e6, -10, -120, &why)},
             {"too high", analyzerConfigCommand(1e6, 1.0e10 + 1e7, -10, -120, &why)},
             {"positive top", analyzerConfigCommand(1e6, 2e6, 5, -120, &why)},
             {"bottom above top", analyzerConfigCommand(1e6, 2e6, -50, -40, &why)},
             {"bottom too low", analyzerConfigCommand(1e6, 2e6, -10, -501, &why)},
             {"nan", analyzerConfigCommand(std::nan(""), 2e6, -10, -120, &why)}}) {
        CHECK(r.second.empty());
    }
    why.clear();
    analyzerConfigCommand(200e6, 100e6, -10, -120, &why);
    CHECK(contains(why, "stop frequency must be above"));
    // every command is at most 64 bytes and its size byte is its length
    for (const std::string& s : {requestConfigCommand(), holdCommand(), dumpScreenOffCommand(), c}) {
        CHECK(s.size() <= kMaxCommandBytes && static_cast<std::uint8_t>(s[1]) == s.size());
    }
}

void testParsing() {
    Config c;
    std::string why;
    CHECK(parseConfig(configText(100000, 898000, -10, -120, 112), c, &why) && c.fields == 13);
    CHECK(c.startKHz == 100000 && c.stepHz == 898000 && c.ampTop == -10 && c.ampBottom == -120 && c.points == 112 && c.mode == 0);
    CHECK(c.minKHz == 15000 && c.maxKHz == 2700000 && c.maxSpanKHz == 112000 && c.rbwKHz == 100);
    CHECK(parseConfig(configText(100000, 898000, -10, -120, 112, 0, 15000, 2700000, 112000, 11), c) && c.fields == 11 && c.rbwKHz == 100);
    CHECK(parseConfig(configText(100000, 898000, -10, -120, 112, 0, 15000, 2700000, 112000, 10), c) && c.fields == 10 && c.rbwKHz == -1);
    CHECK(parseConfig(" 0100000, 0898000 ,-010,-120,0112,0,000,0015000,2700000,0112000", c));  // blanks around fields are tolerated
    for (const char* bad : {"", "1,2,3", "a,b,c,d,e,f,g,h,i,j", "0100000,0000000,-010,-120,0112,0,000,0015000,2700000,0112000",  // step 0
                            "0100000,0898000,-010,-120,0001,0,000,0015000,2700000,0112000",                                       // one point
                            "0100000,0898000,-010,-120,0112,0,000,0900000,0015000,0112000",                                       // max below min
                            "0100000,0898000,-010,-120,0112,0,000,0015000,2700000,0112000,1,2",                                   // 12 fields
                            "0100000,0898000,-010,-120,99999999999999,0,000,0015000,2700000,0112000"}) {
        CHECK(!parseConfig(bad, c, &why) && !why.empty());
    }
    Setup s;
    CHECK(parseSetup("005,255,01.12", s) && s.mainModel == 5 && s.expansionModel == 255 && s.firmware == "01.12");
    CHECK(parseSetup("060,255,01.05", s) && !isAnalyzerModel(s.mainModel) && std::string(mainModelName(60)) == "signal generator");
    CHECK(isAnalyzerModel(0) && isAnalyzerModel(5) && isAnalyzerModel(10) && !isAnalyzerModel(60) && !isAnalyzerModel(99));
    CHECK(!parseSetup("005,255", s) && !parseSetup("x,255,01.12", s) && !parseSetup("300,255,01.12", s));
    CHECK(parseSetup("005,255,\x01\x02xyz", s) && s.firmware.size() <= 8);
    CHECK(dbmOfByte(0x11) == -8.5f && dbmOfByte(0) == 0.0f && dbmOfByte(255) == -127.5f);  // the page's own example
    // fuzz: random text never crashes and never yields a config outside the ranges
    std::mt19937 rng(5);
    int accepted = 0;
    for (int i = 0; i < 20000; ++i) {
        std::string t;
        const int n = static_cast<int>(rng() % 120);
        for (int k = 0; k < n; ++k) { t += static_cast<char>(i % 3 == 0 ? "0123456789,-+ "[rng() % 14] : static_cast<char>(rng() & 0xFF)); }
        Config x;
        if (parseConfig(t, x)) {
            ++accepted;
            CHECK(x.points >= 2 && x.points <= 65536 && x.stepHz >= 1 && x.maxKHz >= x.minKHz);
        }
        Setup y;
        parseSetup(t, y);
    }
    CHECK(accepted >= 0);
}

void testDemux() {
    const std::string cfg = configText(100000, 898000, -10, -120, 112);
    // The sweep of the page's example byte, in all four chunkings: whole, 1, 7, 113.
    for (std::size_t chunk : {std::size_t{0}, std::size_t{1}, std::size_t{7}, std::size_t{113}}) {
        Demux d;
        Bytes all = setupLine();
        const Bytes c = configLine(cfg);
        all.insert(all.end(), c.begin(), c.end());
        std::vector<std::uint8_t> amp(112, 0x11);
        amp[5] = 0;
        amp[6] = 255;
        const Bytes f = sweepFrame('S', amp);
        all.insert(all.end(), f.begin(), f.end());
        const auto items = feedAll(d, all, chunk);
        CHECK(items.size() == 3);
        if (items.size() == 3) {
            CHECK(items[0].kind == ItemKind::Setup && items[1].kind == ItemKind::Config && items[2].kind == ItemKind::Sweep);
            CHECK(items[2].dbm.size() == 112 && items[2].dbm[0] == -8.5f && items[2].dbm[5] == 0.0f && items[2].dbm[6] == -127.5f);
        }
        CHECK(d.stats().sweeps == 1 && d.stats().badFrames == 0 && d.knownPoints() == 112);
    }
    // $S with the count byte as the page reads it ((count+1)*16), $s, $z; both end-of-line orders; the point count comes from the configuration.
    struct Case { char kind; bool direct; bool lfCr; long points; };
    for (const Case& k : {Case{'S', true, false, 112}, Case{'S', false, false, 112}, Case{'S', true, true, 240}, Case{'s', false, false, 112},
                          Case{'s', false, true, 4096}, Case{'z', false, false, 112}, Case{'z', false, true, 5000}}) {
        Demux d;
        Bytes all = configLine(configText(100000, 898000, -10, -120, k.points));
        const Bytes f = sweepFrame(k.kind, ampRamp(static_cast<std::size_t>(k.points)), k.direct, k.lfCr);
        all.insert(all.end(), f.begin(), f.end());
        const auto items = feedAll(d, all, 13);
        CHECK(items.size() == 2 && items.back().kind == ItemKind::Sweep && items.back().frame == k.kind);
        CHECK(!items.empty() && items.back().dbm.size() == static_cast<std::size_t>(k.points));
        CHECK(d.stats().badFrames == 0);
    }
    // A sweep before any configuration is never interpreted (its size is unknown).
    {
        Demux d;
        const auto items = feedAll(d, sweepFrame('S', ampRamp(112)));
        CHECK(items.empty() && d.stats().sweeps == 0);
    }
    // Payload bytes that look like framing ('#', '$', LF, CR, "C2-F:") do not break a frame: it is delimited by its count.
    {
        Demux d;
        std::vector<std::uint8_t> amp(112, 0x23);
        for (std::size_t i = 0; i < 112; i += 5) { amp[i] = static_cast<std::uint8_t>("#$\r\nS"[i % 5]); }
        const std::string tag = "#C2-F:0000000,";
        for (std::size_t i = 0; i < tag.size(); ++i) { amp[40 + i] = static_cast<std::uint8_t>(tag[i]); }
        Bytes all = configLine(configText(100000, 898000, -10, -120, 112));
        const Bytes f = sweepFrame('S', amp);
        all.insert(all.end(), f.begin(), f.end());
        const auto items = feedAll(d, all, 3);
        CHECK(items.size() == 2 && items[1].kind == ItemKind::Sweep && items[1].dbm.size() == 112);
        CHECK(items.size() == 2 && items[1].dbm[0] == -static_cast<float>(amp[0]) / 2.0f);
        CHECK(d.stats().badConfigs == 0);
    }
    // Noise before and between frames, other '#' lines, the generator's lines, a screen dump: skipped and counted, the frames still come out.
    {
        Demux d;
        Bytes all = bytesOf("garbage \x01\x02 and a lone # and $ and $Q ");
        const Bytes c = configLine(configText(100000, 898000, -10, -120, 112));
        all.insert(all.end(), c.begin(), c.end());
        for (const char* line : {"#Sn0123456789012345\r\n", "#K1\r\n", "DSP:0\r\n", "#T:3\r\n", "#C3-M:060,255,01.05\r\n"}) {
            const Bytes l = bytesOf(line);
            all.insert(all.end(), l.begin(), l.end());
        }
        Bytes dump{'$', 'D'};
        dump.insert(dump.end(), kScreenDumpBytes, 0x5A);
        dump.push_back(0x0D);
        dump.push_back(0x0A);
        all.insert(all.end(), dump.begin(), dump.end());
        const Bytes f = sweepFrame('S', ampRamp(112));
        all.insert(all.end(), f.begin(), f.end());
        const auto items = feedAll(d, all, 11);
        CHECK(items.size() == 2 && items[0].kind == ItemKind::Config && items[1].kind == ItemKind::Sweep);
        CHECK(d.stats().screenDumps == 1 && d.stats().generatorMessages == 1 && d.stats().ignoredLines >= 3 && d.stats().discardedBytes > 10);
    }
    // A frame whose count matches neither reading, and one without its end of line, are bad frames: counted, skipped, and the next good frame decodes.
    {
        Demux d;
        Bytes all = configLine(configText(100000, 898000, -10, -120, 112));
        Bytes wrong = sweepFrame('S', ampRamp(110));  // 110 points for a 112 point configuration
        all.insert(all.end(), wrong.begin(), wrong.end());
        Bytes noEol = sweepFrame('S', ampRamp(112));
        noEol.pop_back();
        noEol.pop_back();
        noEol.push_back('x');
        noEol.push_back('y');
        all.insert(all.end(), noEol.begin(), noEol.end());
        Bytes sameEol = sweepFrame('S', ampRamp(112));
        sameEol[sameEol.size() - 1] = 0x0D;  // CR CR
        all.insert(all.end(), sameEol.begin(), sameEol.end());
        const Bytes good = sweepFrame('S', ampRamp(112));
        all.insert(all.end(), good.begin(), good.end());
        const auto items = feedAll(d, all, 17);
        std::size_t sweeps = 0;
        for (const Item& it : items) { sweeps += it.kind == ItemKind::Sweep ? 1 : 0; }
        CHECK(sweeps == 1 && d.stats().badFrames >= 3);
    }
    // Fuzz: 40000 random chunks, a third of them mutated real frames. No crash; every sweep is exactly the configured size; the buffer stays bounded.
    {
        Demux d;
        std::mt19937 rng(99);
        const std::vector<Bytes> parts = {configLine(configText(100000, 898000, -10, -120, 112)), sweepFrame('S', ampRamp(112)), setupLine(), sweepFrame('z', ampRamp(112)),
                                         bytesOf("#C3-*:1,2,3\r\n")};
        std::size_t sweeps = 0;
        for (int i = 0; i < 40000; ++i) {
            Bytes b;
            if (rng() % 3 == 0) {
                b = parts[rng() % parts.size()];
                if (!b.empty()) { b[rng() % b.size()] = static_cast<std::uint8_t>(rng()); }
                if (b.size() > 4 && rng() % 2) { b.resize(1 + rng() % b.size()); }
            } else if (rng() % 2) {
                b = parts[rng() % parts.size()];
            } else {
                b.resize(rng() % 300);
                for (auto& x : b) { x = static_cast<std::uint8_t>(rng()); }
            }
            std::vector<Item> out;
            d.feed(b.data(), b.size(), out);
            for (const Item& it : out) {
                if (it.kind == ItemKind::Sweep) {
                    ++sweeps;
                    CHECK(it.dbm.size() == 112);
                }
            }
        }
        CHECK(sweeps > 100);
    }
}

void testSweeper() {
    // connect + configure + lines: the numbers come from the device's own configuration, the tone is where the device put it.
    {
        Device dev;
        FakeLink link(dev);
        Sweeper sw(link);
        std::string why;
        CHECK(sw.connect(20, &why) == Result::Ok);
        CHECK(sw.haveConfig() && sw.config().points == 112 && sw.setup().mainModel == 5);
        Request req;
        req.startHz = 140e6;
        req.stopHz = 160e6;
        req.topDbm = -20;
        req.bottomDbm = -110;
        req.maxIdleReads = 20;
        CHECK(sw.configure(req, &why) == Result::Ok);
        CHECK(sw.config().startKHz == 140000 && sw.config().ampTop == -20 && sw.config().ampBottom == -110);
        SweepLine line;
        CHECK(sw.nextLine(line, 1234.5, 20, &why) == Result::Ok);
        CHECK(line.points() == 112 && line.startHz == 140e6 && line.timeS == 1234.5);
        const double step = static_cast<double>(sw.config().stepHz);
        CHECK(line.stopHz == 140e6 + step * 111.0);
        CHECK(std::fabs(step - 20e6 / 111.0) < 1.5);  // the device's integer step, not the nominal one
        if (line.db.size() == 112) {  // (a failed CHECK above is already recorded; the indexing below must not run on an empty line)
            const std::size_t peak = static_cast<std::size_t>(std::max_element(line.db.begin(), line.db.end()) - line.db.begin());
            const double peakHz = line.startHz + step * static_cast<double>(peak);
            CHECK(std::fabs(peakHz - dev.toneHz) <= step);
            CHECK(line.db[peak] > -60.0f && line.db[0] < -90.0f);
        }
        for (int i = 0; i < 20; ++i) { CHECK(sw.nextLine(line, 1.0 + i, 20, &why) == Result::Ok); }
        CHECK(sw.stats().badFrames == 0 && sw.stats().sweeps >= 21);
        // only the four allowed commands were ever sent (the page: C0, D0, C2-F:, CH)
        sw.hold();
        CHECK(dev.hold);
        for (const std::string& c : dev.commands) { CHECK(c == "C0" || c == "D0" || c == "CH" || c.compare(0, 5, "C2-F:") == 0); }
        CHECK(dev.wrongSize.empty());
        CHECK(dev.commands.size() == 4);  // D0, C0, C2-F:, CH
    }
    // each frame kind and end of line the page describes, through the whole stack
    for (char kind : {'S', 's', 'z'}) {
        for (bool lfCr : {false, true}) {
            Device dev;
            dev.frameKind = kind;
            dev.directCount = false;
            dev.eolLfCr = lfCr;
            FakeLink link(dev);
            Sweeper sw(link);
            CHECK(sw.connect(20) == Result::Ok);
            SweepLine line;
            CHECK(sw.nextLine(line, 1, 20) == Result::Ok && line.points() == 112);
        }
    }
    // stale sweeps of the old span that arrive after the command are dropped: the first line is on the NEW span
    {
        Device dev;
        dev.staleSweeps = 3;
        FakeLink link(dev);
        Sweeper sw(link);
        CHECK(sw.connect(20) == Result::Ok);
        Request req;
        req.startHz = 300e6;
        req.stopHz = 340e6;
        CHECK(sw.configure(req) == Result::Ok);
        SweepLine line;
        CHECK(sw.nextLine(line, 1, 20) == Result::Ok && line.startHz == 300e6);
    }
    // a setup message is optional; a 1.08 style configuration is accepted
    {
        Device dev;
        dev.noSetup = true;
        FakeLink link(dev);
        Sweeper sw(link);
        CHECK(sw.connect(20) == Result::Ok && sw.setup().mainModel == -1);
    }
    // refusals and failures, each with a sentence
    {   // a silent device
        Device dev;
        dev.silent = true;
        FakeLink link(dev);
        Sweeper sw(link);
        std::string why;
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(sw.connect(10, &why) == Result::NotAnalyzer);
        CHECK(contains(why, "no RF Explorer answered") && contains(why, "500000"));
        CHECK(link.reads <= 12);  // bounded by the idle limit
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5));
    }
    {   // a signal generator
        Device dev;
        dev.analyzer = false;
        FakeLink link(dev);
        Sweeper sw(link);
        std::string why;
        CHECK(sw.connect(10, &why) == Result::NotAnalyzer && contains(why, "signal generator"));
        CHECK(!sw.haveConfig());
        SweepLine line;
        CHECK(sw.nextLine(line, 1, 5) == Result::BadRequest);
    }
    {   // an analyser that is not in spectrum analyser mode, and a model code of the generator
        Device dev;
        dev.mode = 2;
        FakeLink link(dev);
        Sweeper sw(link);
        std::string why;
        CHECK(sw.connect(10, &why) == Result::NotAnalyzer && contains(why, "spectrum analyser mode"));
        Device dev2;
        dev2.model = 60;
        FakeLink link2(dev2);
        Sweeper sw2(link2);
        CHECK(sw2.connect(10, &why) == Result::NotAnalyzer && contains(why, "signal generator"));
    }
    {   // the port fails: on write, on read
        Device dev;
        FakeLink link(dev);
        link.failWrite = true;
        Sweeper sw(link);
        std::string why;
        CHECK(sw.connect(10, &why) == Result::WriteFailed && contains(why, "serial port"));
        link.failWrite = false;
        link.failRead = true;
        CHECK(sw.connect(10, &why) == Result::LinkFailed && contains(why, "unplugged"));
        link.failRead = false;
        link.readsBeforeFail = -1;
        CHECK(sw.connect(10) == Result::Ok);
        link.readsBeforeFail = static_cast<long>(link.reads) + 2;  // the cable is pulled in the middle of the stream
        SweepLine line;
        Result r = Result::Ok;
        for (int i = 0; i < 50 && r == Result::Ok; ++i) { r = sw.nextLine(line, 1, 10); }
        CHECK(r == Result::LinkFailed);
    }
    {   // ranges the device cannot do are refused before anything is sent; a device that ignores the command is Refused
        Device dev;
        FakeLink link(dev);
        Sweeper sw(link);
        CHECK(sw.connect(10) == Result::Ok);
        Request req;
        std::string why;
        req.startHz = 5e6;
        req.stopHz = 20e6;  // below the 15 MHz minimum
        CHECK(sw.configure(req, &why) == Result::BadRequest && contains(why, "outside what this RF Explorer covers"));
        req.startHz = 100e6;
        req.stopHz = 300e6;  // 200 MHz span, the device does 112
        CHECK(sw.configure(req, &why) == Result::BadRequest && contains(why, "112000 kHz"));
        req.startHz = 200e6;
        req.stopHz = 100e6;
        CHECK(sw.configure(req, &why) == Result::BadRequest);
        CHECK(dev.commands.size() == 2);  // only D0 and C0: nothing was written for the refused ranges
        dev.ignoreConfig = true;
        req.startHz = 100e6;
        req.stopHz = 150e6;
        req.maxIdleReads = 10;
        // the device answers every command with its OLD configuration (start 100000): the range 100-150 MHz has the same start, so use another
        req.startHz = 120e6;
        req.stopHz = 160e6;
        CHECK(sw.configure(req, &why) == Result::Refused && contains(why, "did not take that range"));
    }
    {   // configure before connect
        Device dev;
        FakeLink link(dev);
        Sweeper sw(link);
        Request req;
        req.startHz = 100e6;
        req.stopHz = 150e6;
        CHECK(sw.configure(req) == Result::BadRequest && link.writes == 0);
    }
    {   // a flood of bytes that never forms a message ends the call (no endless loop)
        struct Flood : cascade::source::tinysa::SerialLink {
            bool write(const std::uint8_t*, std::size_t) override { return true; }
            long read(std::uint8_t* b, std::size_t cap) override { std::memset(b, 'x', cap); return static_cast<long>(cap); }
        } flood;
        Sweeper sw(flood);
        const auto t0 = std::chrono::steady_clock::now();
        std::string why;
        CHECK(sw.connect(5, &why) == Result::NotAnalyzer);
        CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(20));
    }
    CHECK(std::string(resultWord(Result::Refused)) == "refused" && std::string(resultWord(Result::NotAnalyzer)) == "not-analyzer");
}

#ifdef _WIN32
// The serial link's own I/O over a local named pipe, with the stand-in device behind it.
struct PipeDevice {
    std::string name;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    Device dev;
    std::atomic<bool> stop{false}, closeAfterConnect{false};
    std::atomic<int> closeAfterBytes{-1};
    std::thread th;
    bool start(const std::string& n) {
        name = n;
        pipe = ::CreateNamedPipeA(n.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 65536, 65536, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) { return false; }
        th = std::thread([this] {
            if (!::ConnectNamedPipe(pipe, nullptr) && ::GetLastError() != ERROR_PIPE_CONNECTED) { return; }
            std::uint64_t sent = 0;
            while (!stop.load()) {
                DWORD avail = 0;
                if (!::PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr)) { break; }
                if (avail > 0) {
                    std::uint8_t b[4096];
                    DWORD got = 0;
                    if (!::ReadFile(pipe, b, std::min<DWORD>(avail, sizeof b), &got, nullptr)) { break; }
                    dev.onBytes(b, got);
                }
                const Bytes r = dev.pull(2048);
                if (!r.empty()) {
                    DWORD w = 0;
                    if (!::WriteFile(pipe, r.data(), static_cast<DWORD>(r.size()), &w, nullptr)) { break; }
                    sent += w;
                    if (closeAfterBytes.load() >= 0 && sent >= static_cast<std::uint64_t>(closeAfterBytes.load())) { break; }
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(3));
                }
            }
            ::DisconnectNamedPipe(pipe);
        });
        return true;
    }
    ~PipeDevice() {
        stop = true;
        if (th.joinable()) { th.join(); }
        if (pipe != INVALID_HANDLE_VALUE) { ::CloseHandle(pipe); }
    }
};

void testSerialLink() {
    std::string err;
    // names and bauds are checked before the OS is asked; the pipe seam accepts only its own names
    {
        RfeSerialLink l;
        CHECK(!l.open("COM0", 500000, 100, err) && contains(err, "COM1 to COM256"));
        CHECK(!l.open("COM999", 500000, 100, err) && !l.open("C:\\x", 500000, 100, err) && !l.open("", 500000, 100, err));
        CHECK(!l.open("COM3", 12345, 100, err) && contains(err, "500000"));
        CHECK(!l.openTestPipe("\\\\.\\pipe\\other", 100, err) && contains(err, "only the pipe names of the tests"));
        CHECK(!l.openTestPipe("\\\\.\\C:\\x", 100, err));
        CHECK(!l.isOpen() && l.read(nullptr, 0) == -1);
        std::uint8_t b[4];
        CHECK(l.read(b, 4) == -1 && !l.write(b, 4));
        CHECK(isSupportedBaud(500000) && isSupportedBaud(2400) && !isSupportedBaud(250000) && !isSupportedBaud(0));
        CHECK(!l.openTestPipe("\\\\.\\pipe\\foxsdr-rfe-nobody-" + std::to_string(::GetCurrentProcessId()), 100, err) && contains(err, "no such port"));
    }
    const std::string pn = "\\\\.\\pipe\\foxsdr-rfe-t" + std::to_string(::GetCurrentProcessId());
    {   // the whole exchange over the link
        PipeDevice pd;
        CHECK(pd.start(pn));
        RfeSerialLink link;
        CHECK(link.openTestPipe(pn, 50, err));
        Sweeper sw(link);
        std::string why;
        CHECK(sw.connect(40, &why) == Result::Ok);
        Request req;
        req.startHz = 140e6;
        req.stopHz = 160e6;
        req.maxIdleReads = 40;
        CHECK(sw.configure(req, &why) == Result::Ok);
        SweepLine line;
        for (int i = 0; i < 15; ++i) { CHECK(sw.nextLine(line, 1, 40, &why) == Result::Ok && line.points() == 112); }
        CHECK(sw.stats().badFrames == 0);
        sw.hold();
        const auto t0 = std::chrono::steady_clock::now();
        std::uint8_t sink[8192];
        // after the hold the device is silent: the read times out with 0 (not an error) in about the read timeout
        for (int i = 0; i < 100 && link.read(sink, sizeof sink) > 0; ++i) {}
        const auto t1 = std::chrono::steady_clock::now();
        CHECK(link.read(sink, sizeof sink) == 0);
        const auto waited = std::chrono::steady_clock::now() - t1;
        CHECK(waited >= std::chrono::milliseconds(30) && waited < std::chrono::milliseconds(1500));
        (void)t0;
        CHECK(pd.dev.hold);
        link.close();
        CHECK(!link.isOpen() && link.read(sink, 4) == -1);
    }
    {   // the device end closes in the middle of a sweep: the next read says the link failed, the sweeper says so in a sentence
        PipeDevice pd;
        CHECK(pd.start(pn));
        RfeSerialLink link;
        CHECK(link.openTestPipe(pn, 50, err));
        pd.closeAfterBytes = 400;
        Sweeper sw(link);
        std::string why;
        Result r = sw.connect(40, &why);
        if (r == Result::Ok) {
            SweepLine line;
            for (int i = 0; i < 50 && r == Result::Ok; ++i) { r = sw.nextLine(line, 1, 40, &why); }
        }
        CHECK(r == Result::LinkFailed && contains(why, "serial port failed"));
    }
    {   // a read on a quiet pipe returns 0 at its timeout and a write of nothing is true
        PipeDevice pd;
        pd.dev.silent = true;
        CHECK(pd.start(pn));
        RfeSerialLink link;
        CHECK(link.openTestPipe(pn, 40, err));
        std::uint8_t b[16];
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(link.read(b, sizeof b) == 0);
        CHECK(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(25));
        CHECK(link.write(b, 0));
        const std::string big(100000, 'z');  // more than the pipe buffer takes at once: the write is split and completes
        CHECK(link.write(reinterpret_cast<const std::uint8_t*>(big.data()), big.size()));
    }
}
#endif

std::string readText(const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Receive only, and nothing on the console: the only commands the source can write, and no sink.
void testSourceScan() {
    const std::string root = RB_SRC_DIR;
    const std::string cpp = readText(root + "/src/source/rfexplorer_protocol.cpp");
    CHECK(!cpp.empty());
    std::vector<std::string> bodies;
    std::size_t at = 0;
    while ((at = cpp.find("frameCommand(", at)) != std::string::npos) {
        at += 13;
        if (cpp.compare(at, 1, "\"") == 0) {
            const std::size_t e = cpp.find('"', at + 1);
            bodies.push_back(cpp.substr(at + 1, e - at - 1));
        } else {
            bodies.push_back(cpp.substr(at, cpp.find(')', at) - at));  // frameCommand(buf) / (const std::string& body) in the definition
        }
    }
    // the definition `frameCommand(const std::string& body)`, the three fixed commands and the analyser configuration
    CHECK(bodies.size() == 5);
    std::size_t fixed = 0;
    for (const std::string& b : bodies) {
        if (b == "C0" || b == "CH" || b == "D0") { ++fixed; }
    }
    CHECK(fixed == 3);
    CHECK(contains(cpp, "C2-F:%07ld,%07ld,%04d,%04d"));
    for (const char* f : {"src/source/rfexplorer_protocol.cpp", "src/source/rfexplorer_protocol.hpp", "src/source/rfexplorer_serial_win.cpp", "src/source/rfexplorer_serial_win.hpp"}) {
        const std::string t = readText(root + "/" + f);
        CHECK(!t.empty());
        CHECK(!contains(t, "stderr") && !contains(t, "stdout") && !contains(t, "std::cerr") && !contains(t, "std::cout") && !contains(t, " printf(") &&
              !contains(t, "puts(") && !contains(t, "OutputDebugString") && !contains(t, "settings_set"));
    }
    // no generator, power, baud, shutdown, reboot, wifi or tracking command exists in the writer
    CHECK(!contains(cpp, "\"CP1\"") && !contains(cpp, "\"CP0\"") && !contains(cpp, "\"Cc") && !contains(cpp, "\"S\"") && !contains(cpp, "\"r\"") &&
          !contains(cpp, "\"CW") && !contains(cpp, "\"k") && !contains(cpp, "frameCommand(\"C3") && !contains(cpp, "frameCommand(\"C5"));
}

}  // namespace

int main() {
    testCommands();
    testParsing();
    testDemux();
    testSweeper();
#ifdef _WIN32
    testSerialLink();
#endif
    testSourceScan();
    return testSummary("test_rfexplorer_source");
}
