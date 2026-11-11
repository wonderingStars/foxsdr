// The Slint host's pure logic (src/slint/host_logic.hpp): what the main screen shows is computed from the
// C API table's state by these functions, and every key the operator presses becomes a submit() through them.
// No Slint, no engine: FoxReceiverState is filled by hand.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "host_logic.hpp"
#include "test_check.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

using namespace foxsdr::slinthost;

namespace {

FoxReceiverState blankState() {
    FoxReceiverState s{};
    s.structSize = sizeof(s);
    std::snprintf(s.deviceName, sizeof(s.deviceName), "%s", "Signal generator");
    std::snprintf(s.sinkName, sizeof(s.sinkName), "%s", "Speakers (Realtek)");
    s.sampleRateHz = 2.4e6;
    s.channelRateHz = 12500;
    s.tunedHz = 145.5e6;
    s.demodMode = FOXAPI_DEMOD_NFM;
    s.bandwidthHz = 12500;
    s.squelchDb = -85;
    s.volume = 0.72;
    s.audioLevelDb = -12.4;
    s.dbMin = -110;
    s.dbMax = 0;
    s.audioRateHz = 48000;
    return s;
}

const StatusCardModel& card(const std::vector<StatusCardModel>& cards, const char* key) {
    static const StatusCardModel none{};
    for (const auto& c : cards) { if (c.key == key) { return c; } }
    return none;
}

}  // namespace

int main() {
    // ---- the counter's ten figures: GHz, MHz x3, kHz x3, Hz x3 ----
    {
        const auto d = digitsOf(145'500'000.0);
        CHECK((d == std::array<int, 10>{0, 1, 4, 5, 5, 0, 0, 0, 0, 0}));
        CHECK(leadingZeros(d) == 1);
        CHECK(leadingZeros(digitsOf(0.0)) == 9);            // a zero frequency keeps its last figure bright
        CHECK(leadingZeros(digitsOf(1.2e9)) == 0);
        CHECK((digitsOf(-5.0) == std::array<int, 10>{0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));  // never negative
        CHECK((digitsOf(9'999'999'999.0) == std::array<int, 10>{9, 9, 9, 9, 9, 9, 9, 9, 9, 9}));
        CHECK((digitsOf(12'345'678'901.0) == std::array<int, 10>{9, 9, 9, 9, 9, 9, 9, 9, 9, 9}));  // ten figures is the plate: clamp
        CHECK_NEAR(stepOfDigit(0), 1e9, 0.0);
        CHECK_NEAR(stepOfDigit(5), 1e4, 0.0);               // the 10 kHz cell, Ctrl+Up's digit
        CHECK_NEAR(stepOfDigit(9), 1.0, 0.0);
        CHECK(std::string(placeName(0)) == "GHz");
        CHECK(std::string(placeName(1)) == "100 MHz");
        CHECK(std::string(placeName(4)) == "100 kHz");
        CHECK(std::string(placeName(9)) == "1 Hz");
        CHECK(std::string(placeName(10)) == "");
    }

    // ---- the bookmark names on the spectrum (docs 6.52): the classic window's stacking rule ----
    {
        // two names a few pixels apart: the second takes the row below with stacking on, and is left off with it off
        std::vector<BookmarkName> close{{0.50, "Repeater", false}, {0.51, "Simplex", false}};
        CHECK((bookmarkNameRows(close, 1000.0, 6.5, 3) == std::vector<int>{0, 1}));
        CHECK((bookmarkNameRows(close, 1000.0, 6.5, 1) == std::vector<int>{0, -1}));
        // far apart: both on the top row
        std::vector<BookmarkName> apart{{0.10, "A", false}, {0.90, "B", false}};
        CHECK((bookmarkNameRows(apart, 1000.0, 6.5, 3) == std::vector<int>{0, 0}));
        // a favourite later in the list is placed first, so it wins the top row and the plain one stacks under it
        std::vector<BookmarkName> fav{{0.50, "Plain", false}, {0.505, "Star", true}};
        CHECK((bookmarkNameRows(fav, 1000.0, 6.5, 3) == std::vector<int>{1, 0}));
        // four names on one spot: three rows at most, the fourth is left off; an empty name takes no row and gets none
        std::vector<BookmarkName> pile{{0.5, "One", false}, {0.5, "", false}, {0.5, "Two", false}, {0.5, "Three", false}, {0.5, "Four", false}};
        CHECK((bookmarkNameRows(pile, 1000.0, 6.5, 3) == std::vector<int>{0, -1, 1, 2, -1}));
        CHECK((bookmarkNameRows(pile, 1000.0, 6.5, 9) == std::vector<int>{0, -1, 1, 2, -1}));   // the budget is capped at three
        // a name that would run off the right edge is not placed; just clear of the gap, two names share a row
        std::vector<BookmarkName> edge{{0.99, "Long name here", false}};
        CHECK((bookmarkNameRows(edge, 1000.0, 6.5, 3) == std::vector<int>{-1}));
        std::vector<BookmarkName> gap{{0.100, "AB", false}, {0.122, "CD", false}};   // 100+3 .. 116, then 122+3 = 125 > 116 + 6
        CHECK((bookmarkNameRows(gap, 1000.0, 6.5, 3) == std::vector<int>{0, 0}));
        CHECK(bookmarkNameRows({}, 1000.0, 6.5, 3).empty());
    }

    // ---- modes: the strip's segment, the table's codes, the tuning step per mode ----
    {
        CHECK(std::string(modeName(FOXAPI_DEMOD_NFM)) == "NFM");
        CHECK(std::string(modeName(FOXAPI_DEMOD_RAW)) == "RAW");
        CHECK(std::string(modeName(99)) == "?");
        CHECK(modeIndex(FOXAPI_DEMOD_NFM) == 0 && modeIndex(FOXAPI_DEMOD_RAW) == 7 && modeIndex(0) == -1);
        for (int i = 0; i < 8; ++i) { CHECK(modeIndex(demodOfIndex(i)) == i); }
        CHECK_NEAR(modeStepHz(FOXAPI_DEMOD_NFM), 12500, 0);
        CHECK_NEAR(modeStepHz(FOXAPI_DEMOD_WFM), 100000, 0);
        CHECK_NEAR(modeStepHz(FOXAPI_DEMOD_AM), 9000, 0);
        CHECK_NEAR(modeStepHz(FOXAPI_DEMOD_USB), 1000, 0);
        CHECK_NEAR(modeStepHz(FOXAPI_DEMOD_CW), 1000, 0);
    }

    // ---- readouts ----
    {
        CHECK(fmtMHz(145.5e6) == "145.500000 MHz");
        CHECK(fmtMHz(144.8e6) == "144.800000 MHz");
        CHECK(fmtRate(2.4e6) == "2.400 MS/s");
        CHECK(fmtRate(0) == "--");
        CHECK(fmtKHz(12500) == "12.5 kHz");
        CHECK(fmtKHz(150000) == "150.0 kHz");
        CHECK(cursorText(145.212e6, -78.4) == "145.212 MHz \xC2\xB7 -78.4 dB");   // the IA's readout, a middle dot
        CHECK(spanText(2.4e6) == "SPAN 2.400 MHz");
        CHECK(spanText(240e3) == "SPAN 240.0 kHz");
        CHECK(spanText(1.2e9) == "SPAN 1.200 GHz");

        // the typed editor's entry
        double hz = 0;
        CHECK(parseFrequencyHz("145.5", hz) && hz == 145.5e6);               // a bare number is MHz
        CHECK(parseFrequencyHz("145.500 MHz", hz) && hz == 145.5e6);
        CHECK(parseFrequencyHz("145500 kHz", hz) && hz == 145.5e6);
        CHECK(parseFrequencyHz("145500000", hz) && hz == 145.5e6);          // ...unless it is too big to be MHz
        CHECK(parseFrequencyHz("7,074", hz) && hz == 7.074e6);               // a decimal comma
        CHECK(parseFrequencyHz("1.2 GHz", hz) && hz == 1.2e9);
        CHECK(parseFrequencyHz(" 14 313 Hz ", hz) && hz == 14313.0);
        CHECK(!parseFrequencyHz("", hz));
        CHECK(!parseFrequencyHz("abc", hz));
        CHECK(!parseFrequencyHz("145.5 furlongs", hz));
        CHECK(!parseFrequencyHz("-7", hz));
        CHECK(!parseFrequencyHz("0", hz));
        CHECK(!parseFrequencyHz("1.2.3", hz));

        // the click-to-tune snap and the zoom window
        CHECK_NEAR(snapToStep(145'506'000, 12500, false), 145'500'000, 0);
        CHECK_NEAR(snapToStep(145'506'300, 12500, false), 145'512'500, 0);
        CHECK_NEAR(snapToStep(145'506'300, 12500, true), 145'506'300, 0);     // Shift = exact
        CHECK_NEAR(snapToStep(145'506'300, 0, false), 145'506'300, 0);
        ViewWindow v{0, 0};
        v = zoomAbout(v, 145.5e6, 2.4e6, 1.3, 0.5, 2400);                     // from the full span, about the centre
        CHECK_NEAR(v.spanHz, 2.4e6 / 1.3, 1e-6);
        CHECK_NEAR(v.centreHz, 145.5e6, 1e-6);
        const double edgeHz = hzAt(0.9, 145.5e6, 2.4e6);
        ViewWindow w = zoomAbout({145.5e6, 2.4e6}, 145.5e6, 2.4e6, 2.0, 0.9, 2400);  // about a point near the right edge: it stays put
        CHECK_NEAR(hzAt(0.9, w.centreHz, w.spanHz), edgeHz, 1e-3);
        CHECK_NEAR(w.spanHz, 1.2e6, 1e-6);
        ViewWindow out = zoomAbout({145.5e6, 1.0e6}, 145.5e6, 2.4e6, 1.0 / 1.3, 0.5, 2400); // zooming out never leaves the full span
        CHECK_NEAR(out.spanHz, 1.3e6, 1e-6);
        ViewWindow full = zoomAbout({145.5e6, 2.3e6}, 145.5e6, 2.4e6, 1.0 / 1.3, 0.5, 2400);
        CHECK_NEAR(full.spanHz, 2.4e6, 1e-6);
        CHECK_NEAR(full.centreHz, 145.5e6, 1e-6);
        ViewWindow floor = zoomAbout({145.5e6, 3000}, 145.5e6, 2.4e6, 2.0, 0.5, 2400);
        CHECK_NEAR(floor.spanHz, 2400, 1e-9);                                 // the zoom floor: sample rate / 1000
        ViewWindow p = panBy({145.5e6, 1.0e6}, 145.5e6, 2.4e6, -0.25);        // pan right by a quarter of the view
        CHECK_NEAR(p.centreHz, 145.75e6, 1e-6);
        ViewWindow clamped = panBy({145.5e6, 1.0e6}, 145.5e6, 2.4e6, -5.0);
        CHECK_NEAR(clamped.centreHz, 145.5e6 + 1.2e6 - 0.5e6, 1e-6);          // stops at the right edge of the full span
    }

    // ---- the strip's dials as sliders ----
    {
        CHECK_NEAR(squelchNorm(-85), 35.0 / 120.0, 1e-9);    // -120..0 dB is the dial's range
        CHECK_NEAR(squelchNorm(0), 1.0, 0);
        CHECK_NEAR(squelchNorm(-200), 0.0, 0);               // auto / always open sits at the floor
        CHECK(squelchAuto(-200) && squelchAuto(-199.5) && !squelchAuto(-120));
        CHECK_NEAR(squelchDbFromNorm(0.5), -60, 1e-9);
        CHECK_NEAR(squelchDbFromNorm(1.5), 0, 0);            // clamped
        CHECK_NEAR(levelNorm(-12.4), 1.0 - 12.4 / 60.0, 1e-9);
        CHECK_NEAR(levelNorm(-90), 0.0, 0);                  // the meter's -60 dB floor
        CHECK_NEAR(levelNorm(3), 1.0, 0);
        CHECK(levelText(-12.4, true) == "-12.4 dB");
        CHECK(levelText(-75, true) == "-60 dB");             // silence reads the floor, as the VOLUME meter does
        CHECK(levelText(-12.4, false) == "--");              // no audio
        CHECK(gainText(false, false, 32, "dB") == "N/A");
        CHECK(gainText(true, true, 32, "dB") == "AUTO");
        CHECK(gainText(true, false, 32, "dB") == "32 dB");
        CHECK(gainText(true, false, 7, "step") == "7 step");
    }

    // ---- the spectrum's axes ----
    {
        // the sheet: 145.5 MHz centre, 2.4 MHz span, a 980 px plot -> 200 kHz ticks
        const auto t = frequencyTicks(145.5e6, 2.4e6, 980);
        CHECK(t.size() >= 10 && t.size() <= 13);
        CHECK(!t.empty() && t.front().label == "144.4" && t.front().frac >= 0.0 && t.front().frac < 0.1);
        CHECK(!t.empty() && t.back().frac <= 1.0);
        bool ascending = true;
        for (std::size_t i = 1; i < t.size(); ++i) { ascending = ascending && t[i].frac > t[i - 1].frac; }
        CHECK(ascending);
        bool has145 = false;
        for (const auto& k : t) { if (k.label == "145.0") { has145 = true; CHECK_NEAR(k.frac, (145.0e6 - 144.3e6) / 2.4e6, 1e-6); } }
        CHECK(has145);
        CHECK(axisUnit(2.4e6) == "MHz");
        // a 20 kHz span zooms to 2 kHz ticks, labelled in MHz with 3 decimals, and a 1.4 GHz span to 200 MHz ticks
        const auto z = frequencyTicks(145.5e6, 20e3, 980);
        CHECK(!z.empty() && z.front().label == "145.490" && z.front().frac == 0.0);   // the left edge IS a tick: the view slides its label inboard
        const auto w = frequencyTicks(1.0e9, 1.4e9, 980);
        CHECK(!w.empty() && w.front().label == "400" && axisUnit(1.4e9) == "MHz");
        // degenerate inputs never loop
        CHECK(frequencyTicks(145.5e6, 0, 980).empty());
        CHECK(frequencyTicks(145.5e6, 2.4e6, 0).empty());
        CHECK(frequencyTicks(145.5e6, -1, 980).empty());

        // dB: a line every 10 dB; labels thin to 20 dB when the pitch is under 40 px (the sheet's 0, -20, ... -100)
        const auto d = dbTicks(-110, 0, 330);
        CHECK(d.size() == 12);
        CHECK(d.front().label == "0" && d.front().frac == 0.0);
        CHECK(d[1].label == "" && d[2].label == "-20");
        CHECK_NEAR(d[11].frac, 1.0, 1e-9);
        const auto tall = dbTicks(-110, 0, 1200);
        CHECK(tall.size() == 12 && tall[1].label == "-10");
        CHECK(dbTicks(0, 0, 330).empty());
        CHECK(dbTicks(-10, -110, 330).empty());

        CHECK_NEAR(fracOf(145.5e6, 145.5e6, 2.4e6), 0.5, 1e-12);
        CHECK_NEAR(fracOf(144.3e6, 145.5e6, 2.4e6), 0.0, 1e-12);
        CHECK_NEAR(hzAt(0.75, 145.5e6, 2.4e6), 146.1e6, 1e-3);
        CHECK_NEAR(hzAt(fracOf(144.8e6, 145.5e6, 2.4e6), 145.5e6, 2.4e6), 144.8e6, 1e-3);

        // the waterfall's age strip: now at the top, then every 10 s of the history the picture holds
        const auto a = ageTicks(42.0, 238);
        CHECK(a.size() == 5 && a[0].label == "now" && a[0].frac == 0.0 && a[1].label == "10 s" && a[4].label == "40 s");
        CHECK_NEAR(a[1].frac, 10.0 / 42.0, 1e-9);
        const auto b = ageTicks(600.0, 238);                 // ten minutes in 238 px: labels 48 px apart or more -> 5 min
        CHECK(b.size() == 3 && b[1].label == "5 min" && b.back().label == "10 min");
        const auto c = ageTicks(600.0, 2400);                // a tall picture affords 15 s steps (60 px apart)
        CHECK(c.size() == 41 && c[1].label == "15 s" && c[4].label == "1 min" && c.back().label == "10 min");
        CHECK(ageTicks(0, 238).size() == 1);                 // just "now"
    }

    // ---- paint only on a new frame ----
    {
        CHECK(isNewFrame(1, 0));
        CHECK(!isNewFrame(5, 5));
        CHECK(!isNewFrame(4, 5));
        CHECK(!isNewFrame(0, 0));                            // the host starts at 0 and the engine publishes from 1
    }

    // ---- the painters work on a caller-owned RGBA buffer: nothing is allocated per frame ----
    {
        const std::uint32_t w = 64, h = 32;
        std::vector<Rgba> px(w * h, Rgba{1, 2, 3, 255});
        TraceState st;
        std::vector<float> bins(16, -100.0f);
        bins[8] = -20.0f;                                    // one carrier in the middle
        paintTrace(px.data(), w, h, bins.data(), bins.size(), -110, 0, st, TraceMode::PeakHold, Rgba{0x7a, 0xd7, 0xff, 255}, Rgba{0xff, 0x9f, 0x43, 255}, Rgba{0xf1, 0xea, 0xd6, 255});
        // the ground is cleared to transparent; in the first frame the held trace equals the live one and, drawn
        // bold over it, covers it - the carrier's column shows the held colour near the top (-20 dB of -110..0 is 18% down)
        CHECK(px[0].a == 0);
        const auto firstRow = [&](std::uint32_t x, std::uint8_t r, std::uint8_t g) {
            for (std::uint32_t y = 0; y < h; ++y) { const auto& p = px[y * w + x]; if (p.a == 255 && p.r == r && p.g == g) { return static_cast<int>(y); } }
            return -1;
        };
        int peakTop = firstRow(34, 0xff, 0x9f);
        CHECK(peakTop >= 0 && peakTop <= 8);
        CHECK(st.peak.size() == w);
        // the second frame, quieter: the peak-hold trace stays near the top while the live trace drops to the floor
        bins[8] = -100.0f;
        paintTrace(px.data(), w, h, bins.data(), bins.size(), -110, 0, st, TraceMode::PeakHold, Rgba{0x7a, 0xd7, 0xff, 255}, Rgba{0xff, 0x9f, 0x43, 255}, Rgba{0xf1, 0xea, 0xd6, 255});
        peakTop = firstRow(34, 0xff, 0x9f);
        CHECK(peakTop >= 0 && peakTop <= 8);
        const int liveTop = firstRow(34, 0x7a, 0xd7);
        CHECK(liveTop >= 20);                                // -100 dB sits 91% down a 32-row buffer
        // Normal mode draws no second trace; a reset forgets the hold
        resetTrace(st);
        CHECK(st.peak.empty() && st.average.empty());
        paintTrace(px.data(), w, h, bins.data(), bins.size(), -110, 0, st, TraceMode::Normal, Rgba{0x7a, 0xd7, 0xff, 255}, Rgba{0xff, 0x9f, 0x43, 255}, Rgba{0xf1, 0xea, 0xd6, 255});
        int peakPixels = 0;
        for (const auto& p : px) { if (p.a == 255 && p.r == 0xff && p.g == 0x9f) { ++peakPixels; } }
        CHECK(peakPixels == 0);

        // the waterfall: a row scrolls down and the new frame lands on row 0 through the strength key's ramp
        std::vector<Rgba> wf(w * h, Rgba{9, 9, 9, 255});
        bins[8] = 0.0f;
        bins[0] = -110.0f;                                   // the floor of the -110..0 range
        scrollWaterfallRow(wf.data(), w, h, bins.data(), bins.size(), -110, 0);
        CHECK(wf[1 * w + 0].r == 9);                         // the old row 0 moved to row 1
        const Rgba hot = wf[0 * w + 34];
        const Rgba cold = wf[0 * w + 0];
        CHECK(hot.r == 0xff && hot.g == 0xff && hot.b == 0xff);   // 0 dB is the ramp's top: white
        CHECK(cold.r == 0x0a && cold.g == 0x14 && cold.b == 0x30); // the floor
        CHECK(heat(0.0f).b == 0x30 && heat(1.0f).r == 0xff && heat(0.55f).b == 0xc1);
        CHECK(heat(-1.0f).b == 0x30 && heat(2.0f).g == 0xff);     // clamped
    }

    // ---- the seven status cards, the IA's order, from the table's state ----
    {
        FoxReceiverState s = blankState();
        HostExtras x;
        x.gainText = "32 dB";
        x.antenna = "RX";
        x.ppm = 0.0;
        x.havePpm = true;
        x.webPort = 8080;
        x.webBind = "0.0.0.0";
        auto cards = statusCards(s, x);
        CHECK(cards.size() == 7);
        CHECK(cards[0].key == "RECEIVER" && cards[1].key == "DECODERS" && cards[2].key == "OUTPUT" && cards[3].key == "SINK" &&
              cards[4].key == "RECORDER" && cards[5].key == "WEB" && cards[6].key == "UNDERRUNS");
        // stopped, nothing fitted, no sink, no web, no underruns
        CHECK(card(cards, "RECEIVER").value == "STOPPED" && card(cards, "RECEIVER").tone == CardTone::Neutral && !card(cards, "RECEIVER").lampLit);
        CHECK(card(cards, "RECEIVER").detail.find("Signal generator") != std::string::npos);
        CHECK(card(cards, "RECEIVER").detail.find("2.400 MS/s") != std::string::npos);
        CHECK(card(cards, "DECODERS").value == "0 ready" && card(cards, "DECODERS").detail == "of 0 fitted - receiver stopped");
        CHECK(card(cards, "OUTPUT").value == "--" && card(cards, "OUTPUT").detail == "the receiver is stopped");
        CHECK(card(cards, "SINK").value == "STOPPED" && card(cards, "SINK").tone == CardTone::Neutral);
        CHECK(card(cards, "RECORDER").value == "off" && card(cards, "RECORDER").detail == "no file open");
        CHECK(card(cards, "WEB").value == "off" && card(cards, "WEB").detail == "not listening");
        CHECK(card(cards, "UNDERRUNS").value == "0" && card(cards, "UNDERRUNS").detail == "no callback has starved yet" && card(cards, "UNDERRUNS").tone == CardTone::Neutral);

        // running, decoders fed, sink open, web up, output measured
        s.flags = FOXAPI_RX_RUNNING | FOXAPI_RX_SINK_OPEN | FOXAPI_RX_WEB_LISTENING | FOXAPI_RX_DECODER_ACTIVE;
        s.audioFlags = FOXAPI_AUDIO_EVER_OPENED | FOXAPI_AUDIO_STREAM_ALIVE;
        s.decodersRunning = 2; s.decodersFitted = 5; s.decodersMatched = 3;
        x.outputRate = 3.2; x.outputMeanS = 2.0; x.outputMeasured = true;
        cards = statusCards(s, x);
        CHECK(card(cards, "RECEIVER").value == "RUNNING" && card(cards, "RECEIVER").tone == CardTone::Good && card(cards, "RECEIVER").lampLit);
        CHECK(card(cards, "RECEIVER").detail == "Signal generator \xC2\xB7 ANT RX \xC2\xB7 GAIN 32 dB \xC2\xB7 PPM 0.0 \xC2\xB7 2.400 MS/s");
        CHECK(card(cards, "RECEIVER").lines.size() == 5);
        // THE HARDWARE'S AUTOMATIC GAIN reads "GAIN AUTO" (it used to read "GAIN AUTO - AGC ON"): AGC now means the audio AGC alone on this screen
        {
            const auto has = [&](const StatusCardModel& c, const char* line) { return std::find(c.lines.begin(), c.lines.end(), std::string(line)) != c.lines.end(); };
            HostExtras a = x;
            a.gainText = "AUTO";
            const auto autoCards = statusCards(s, a);
            CHECK(has(card(autoCards, "RECEIVER"), "GAIN AUTO"));
            CHECK(!has(card(autoCards, "RECEIVER"), "GAIN AUTO - AGC ON"));
            a.gainText = "32 dB";
            CHECK(has(card(statusCards(s, a), "RECEIVER"), "GAIN 32 dB"));
        }
        CHECK(card(cards, "DECODERS").value == "2 running" && card(cards, "DECODERS").detail == "of 5 fitted, 1 idle" && card(cards, "DECODERS").tone == CardTone::Good);
        CHECK(card(cards, "OUTPUT").value == "3.2 /s" && card(cards, "OUTPUT").detail == "output lines, 2.0 s mean" && card(cards, "OUTPUT").tone == CardTone::Warn);
        CHECK(card(cards, "SINK").value == "OPEN" && card(cards, "SINK").tone == CardTone::Good);
        CHECK(card(cards, "SINK").detail == "Speakers (Realtek) \xC2\xB7 playing NFM");
        CHECK(card(cards, "WEB").value == "port 8080" && card(cards, "WEB").detail == "0.0.0.0");

        // measuring: a decoder runs but no rate yet
        x.outputMeasured = false;
        cards = statusCards(s, x);
        CHECK(card(cards, "OUTPUT").value == "--" && card(cards, "OUTPUT").detail == "measuring");
        s.decodersRunning = 0;
        s.flags &= ~FOXAPI_RX_DECODER_ACTIVE;
        cards = statusCards(s, x);
        CHECK(card(cards, "OUTPUT").detail == "no decoder is running");
        CHECK(card(cards, "DECODERS").value == "3 ready" && card(cards, "DECODERS").detail == "of 5 fitted, 3 not fed");   // matched but not fed

        // muted by a plugin, a plugin playing, the sink opening, no device, a fault, underruns, recording, web refused
        s.flags = FOXAPI_RX_RUNNING | FOXAPI_RX_SINK_OPEN | FOXAPI_RX_MUTED | FOXAPI_RX_RECORDING_IQ | FOXAPI_RX_RECORDING_AUDIO;
        std::snprintf(s.audioMutedBy, sizeof(s.audioMutedBy), "%s", "APRS and POCSAG");
        std::snprintf(s.audioPlayingPlugin, sizeof(s.audioPlayingPlugin), "%s", "Airband");
        s.audioUnderruns = 3; s.audioRingFrames = 2400; s.audioRingCapacityFrames = 12000; s.audioPluginGaps = 2; s.audioPluginGapFrames = 480;
        x.recorderValue = "IQ + AUDIO"; x.recorderDetail = "00:01:12 elapsed \xC2\xB7 14.2 MB written";
        x.webRefused = "port 8080 is in use";
        cards = statusCards(s, x);
        CHECK(card(cards, "SINK").value == "MUTED" && card(cards, "SINK").tone == CardTone::Warn);
        CHECK(card(cards, "SINK").lines.size() >= 3 && card(cards, "SINK").lines[1] == "by APRS and POCSAG" && card(cards, "SINK").lines[2] == "for Airband");
        CHECK(card(cards, "UNDERRUNS").value == "3" && card(cards, "UNDERRUNS").tone == CardTone::Err);
        CHECK(card(cards, "UNDERRUNS").lines.size() == 3 && card(cards, "UNDERRUNS").lines[0] == "starved callbacks, since start" &&
              card(cards, "UNDERRUNS").lines[1] == "ring 50 of 250 ms" && card(cards, "UNDERRUNS").lines[2] == "plugin gaps 2, 480 frames");
        CHECK(card(cards, "RECORDER").value == "IQ + AUDIO" && card(cards, "RECORDER").tone == CardTone::Err);
        CHECK(card(cards, "WEB").value == "REFUSED" && card(cards, "WEB").detail == "port 8080 is in use" && card(cards, "WEB").tone == CardTone::Err);
        s.flags = FOXAPI_RX_RUNNING; s.audioFlags = FOXAPI_AUDIO_OPENING;
        cards = statusCards(s, x);
        CHECK(card(cards, "SINK").value == "OPENING" && card(cards, "SINK").detail == "waiting for the output device");
        s.audioFlags = 0;
        cards = statusCards(s, x);
        CHECK(card(cards, "SINK").value == "NO DEVICE" && card(cards, "SINK").tone == CardTone::Err);
        s.flags = FOXAPI_RX_FAULTED;
        std::snprintf(s.faultMessage, sizeof(s.faultMessage), "%s", "the radio went away");
        cards = statusCards(s, x);
        CHECK(card(cards, "RECEIVER").value == "FAULT" && card(cards, "RECEIVER").tone == CardTone::Err && card(cards, "RECEIVER").lampLit);
        CHECK(card(cards, "RECEIVER").lines.back() == "the radio went away");
    }

    // ---- the decoder output rate: lines per second over a 2 s window, "measuring" until the window is full ----
    {
        OutputRate r;
        CHECK(!r.measured());
        r.add(100, 0);
        r.add(104, 1000);
        CHECK(!r.measured());                                 // one second: not yet
        r.add(107, 2500);
        CHECK(r.measured());
        CHECK_NEAR(r.perSecond(), 7.0 / 2.5, 1e-9);
        CHECK_NEAR(r.meanSeconds(), 2.5, 1e-9);
        r.add(107, 60000);                                    // a long silence: the rate decays to zero, still measured
        CHECK(r.measured());
        CHECK_NEAR(r.perSecond(), 0.0, 1e-9);
        r.reset();
        CHECK(!r.measured());
    }

    // ---- the rail footer and strip hints ----
    {
        FoxReceiverState s = blankState();
        CHECK(rateLine(s) == "2.400 MS/s \xC2\xB7 ch 12.5 kHz \xC2\xB7 underruns 0");
        CHECK(sourceLine(s, "2 m amateur") == "Signal generator \xC2\xB7 2 m amateur");
        CHECK(sourceLine(s, "") == "Signal generator");
        CHECK(modeHint(s) == "Passband 12.5 kHz \xC2\xB7 step 12.5 kHz \xC2\xB7 more in Signal \xE2\x80\xBA Radio");
        CHECK(tuneHint(false) == "Scroll a digit to tune \xC2\xB7 click to type (T) \xC2\xB7 no converter");
        CHECK(tuneHint(true) == "Scroll a digit to tune \xC2\xB7 click to type (T)");
        CHECK(biasHint(false, false) == "No bias tee on this radio");
        CHECK(biasHint(true, false) == "Off \xC2\xB7 4.5 V on the cable \xC2\xB7 asks first");
        CHECK(biasHint(true, true) == "On \xC2\xB7 4.5 V on the cable");
        s.flags = FOXAPI_RX_RUNNING;
        CHECK(waterfallFootRight(s, "APRS") == "NFM \xC2\xB7 decoding: APRS");
        CHECK(waterfallFootRight(s, "") == "NFM \xC2\xB7 receiving");
        s.flags = 0;
        FoxReceiverState stopped = s;
        CHECK(waterfallFootRight(stopped, "APRS") == "STOPPED");
    }

    return testSummary("test_slint_host_logic");
}
