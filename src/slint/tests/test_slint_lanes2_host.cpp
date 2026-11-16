// Screen lane S2's pure rules (src/slint/lanes2_logic.hpp; docs/multi-radio-records/screen-s2.md): the spectrum's FFT size, window and frame rate, the IF zoom (fact, picture), the waterfall autosave
// (when a picture is due, the folder, the recorder's file-name rule), the power and SNR log's readings, the band table and the VFO read-outs.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cmath>
#include <string>
#include <vector>

#include "foxsdr_api.h"
#include "lanes2_logic.hpp"
#include "test_check.hpp"

using namespace foxsdr::slints2;

int main() {
    double d = 0; std::string e;

    // ---------------------------------------------------------------- FFT size, window, frame rate (G2-01, G2-02, G2-03)
    CHECK(fftSizeAt(0) == 1024 && fftSizeAt(7) == 131072 && fftSizeAt(-5) == 1024 && fftSizeAt(99) == 131072);   // the engine's own range 1024..131072, clamped
    CHECK(fftIndexOf(1024) == 0 && fftIndexOf(131072) == 7 && fftIndexOf(512) == -1 && fftIndexOf(3000) == -1 && fftIndexOf(262144) == -1);
    CHECK(fftSizeLabels().size() == 8 && fftSizeLabels()[0] == "1024" && fftSizeLabels()[7] == "131072");
    CHECK(std::string(windowName(0)) == "Rectangular" && std::string(windowName(3)) == "Blackman-Harris" && std::string(windowName(4)) == "Flat top" && std::string(windowName(99)) == "Flat top");
    CHECK(parseFrameRate("0", d, e) && d == 0.0);
    CHECK(parseFrameRate("auto", d, e) && d == 0.0 && parseFrameRate(" Automatic ", d, e) && d == 0.0);
    CHECK(parseFrameRate("1", d, e) && d == 1.0 && parseFrameRate("120", d, e) && d == 120.0 && parseFrameRate("29.97", d, e) && d == 29.97);
    CHECK(!parseFrameRate("121", d, e) && !e.empty());
    CHECK(!parseFrameRate("0.5", d, e) && !e.empty());                 // between automatic and 1 a second is nothing the engine accepts
    CHECK(!parseFrameRate("-1", d, e) && !parseFrameRate("", d, e) && !parseFrameRate("fast", d, e));
    CHECK(frameRateText(0.0) == "0" && frameRateText(30.0) == "30" && frameRateText(-3.0) == "0");
    CHECK(spectrumFigure({2048, 3, 0.0}, "auto", "a second") == "2048 / auto" && spectrumFigure({1024, 3, 50.0}, "auto", "a second") == "1024 / 50 a second");

    // ---------------------------------------------------------------- IF zoom (G2-12)
    CHECK(zoomSizeAt(0) == 0 && zoomSizeAt(1) == 256 && zoomSizeAt(2) == 512 && zoomSizeAt(7) == 16384 && zoomSizeAt(99) == 16384);
    CHECK(zoomIndexOf(0) == 0 && zoomIndexOf(256) == 1 && zoomIndexOf(16384) == 7 && zoomIndexOf(300) == -1 && zoomIndexOf(32768) == -1);
    CHECK(zoomSizeLabels("Off").size() == 8 && zoomSizeLabels("Off")[0] == "Off" && zoomSizeLabels("Off")[1] == "256");
    CHECK(parseZoomSpan("100", d, e) && d == 100.0 && parseZoomSpan("12000", d, e) && d == 12000.0);
    CHECK(!parseZoomSpan("99", d, e) && !parseZoomSpan("", d, e) && !parseZoomSpan("1e8", d, e) && !parseZoomSpan("wide", d, e));
    ZoomFact z;
    CHECK(parseZoomFact("off", z) && !z.on);
    CHECK(parseZoomFact("4096 bins of 2.930 Hz over 12000 Hz", z) && z.on && z.bins == 4096 && std::fabs(z.binHz - 2.93) < 1e-9 && z.spanHz == 12000.0);
    CHECK(!parseZoomFact("", z) && !z.on && !parseZoomFact("a few bins", z) && !z.on);
    {   // the picture: a carrier in the 1000th of 4096 bins stands in its own column and is the tallest
        const std::uint32_t W = 320, H = 96;
        std::vector<float> bins(4096, -110.0f);
        bins[1000] = -20.0f;
        const auto px = paintZoom(W, H, bins.data(), bins.size(), -120.0, 0.0, 0.8);
        CHECK(px.size() == static_cast<std::size_t>(W) * H * 4);
        const std::uint32_t col = paintedPeakColumn(px, W, H);
        CHECK(col == static_cast<std::uint32_t>(1000.0 * W / 4096.0));   // bin 1000 of 4096 falls in column 78
        const auto flat = paintZoom(W, H, nullptr, 0, -120.0, 0.0, 0.8);   // no frame: the ground only, every pixel opaque
        CHECK(flat.size() == px.size() && flat[3] == 255 && flat[flat.size() - 1] == 255);
        // a narrow carrier is not lost to a picture narrower than the bins: the strongest bin of the group wins
        std::vector<float> wide(16384, -100.0f);
        wide[8191] = -10.0f;
        const auto px2 = paintZoom(W, H, wide.data(), wide.size(), -120.0, 0.0, 0.8);
        CHECK(paintedPeakColumn(px2, W, H) == 159);
        // the edge strips are dimmed: the same level at column 2 is drawn darker than at the middle
        std::vector<float> edge(320, -10.0f);
        const auto px3 = paintZoom(W, H, edge.data(), edge.size(), -120.0, 0.0, 0.8);
        const auto at = [&](std::uint32_t x, std::uint32_t y) { return px3[(static_cast<std::size_t>(y) * W + x) * 4]; };
        CHECK(at(2, 4) < at(160, 4));
        // a bad range, a tiny picture: no crash, the ground only
        CHECK(paintZoom(2, 2, bins.data(), bins.size(), 0.0, -120.0, 0.8).size() == 16);
        CHECK(paintZoom(W, H, bins.data(), bins.size(), 0.0, 0.0, 0.8).size() == px.size());
    }

    // ---------------------------------------------------------------- waterfall autosave (G2-09)
    CHECK(autosaveMinutes(0) == 1 && autosaveMinutes(2) == 15 && autosaveMinutes(4) == 60 && autosaveMinutes(99) == 60 && autosaveIndexOf(30) == 3 && autosaveIndexOf(7) == -1);
    CHECK(autosaveDue(0, 1000, 5));                                    // never saved: the first picture is at once
    CHECK(!autosaveDue(1000, 1000 + 4 * 60000 + 59999, 5) && autosaveDue(1000, 1000 + 5 * 60000, 5));
    CHECK(!autosaveDue(1000, 99999999, 0) && !autosaveDue(0, 5, -1));
    CHECK(cleanFolder("C:/Users/steve/Pictures/") == "C:/Users/steve/Pictures" && cleanFolder("  \"C:\\Shots\\wf\\\"  ") == "C:\\Shots\\wf");
    CHECK(cleanFolder("C:\\") == "C:\\" && cleanFolder("D:/") == "D:/");
    CHECK(cleanFolder("").empty() && cleanFolder("   ").empty() && cleanFolder("pictures").empty() && cleanFolder("..\\x").empty() && cleanFolder("C:").empty());   // a relative path has no base to be relative to
    CHECK(cleanFolder("\\\\nas\\share\\wf") == "\\\\nas\\share\\wf" && cleanFolder("/home/me/wf/") == "/home/me/wf");
    {   // the recorder's file-name rule (core/rec_naming.hpp): the default, the user's template, a template the rule refuses
        const auto norm = [](std::string s) { for (char& ch : s) { if (ch == '\\') { ch = '/'; } } return s; };   // Windows gives back backslashes
        AutosaveFields f; f.unixMs = 1700000000000LL; f.frequencyHz = 7074000.0; f.mode = "USB"; f.radio = 2;   // 2023-11-14 22:13:20 UTC
        const std::string a = norm(autosavePath("C:/Shots", "", f, 1));
        CHECK(a == "C:/Shots/waterfall_20231114_221320");
        const std::string b = norm(autosavePath("C:/Shots", "{freq_mhz}/{mode}_{datetime}", f, 1));
        CHECK(b.find("7.074/USB_20231114_221320") != std::string::npos);
        const std::string c = autosavePath("C:/Shots", "{nonsense}", f, 1);     // refused by the rule: the default name takes over, never a silent path
        CHECK(c.find("waterfall_20231114_221320") != std::string::npos);
        const std::string dpath = autosavePath("C:/Shots", "../escape/{date}", f, 1);
        CHECK(dpath.find("escape") == std::string::npos && dpath.find("waterfall_") != std::string::npos);
        CHECK(autosavePath("", "", f, 1).empty());
        const std::string cnt = autosavePath("C:/Shots", "wf_{radio}_{counter:3}", f, 7);
        CHECK(cnt.find("wf_2_007") != std::string::npos);
        CHECK(a.size() >= 4 && a.substr(a.size() - 4) != ".png");           // the extension is the writer's
    }

    // ---------------------------------------------------------------- the power and SNR log (G2-16)
    CHECK(parseLogInterval("0.1", d, e) && d == 0.1 && parseLogInterval("3600", d, e) && d == 3600.0 && parseLogInterval("1", d, e) && d == 1.0);
    CHECK(!parseLogInterval("0.09", d, e) && !parseLogInterval("3601", d, e) && !parseLogInterval("0", d, e) && !parseLogInterval("", d, e) && !parseLogInterval("often", d, e));
    {
        std::vector<float> bins(1000, -100.0f);
        for (int i = 450; i < 550; ++i) { bins[static_cast<std::size_t>(i)] = -40.0f; }
        bins[500] = -30.0f;
        const PowerReading r = readPower(bins.data(), bins.size(), 450, 550);
        CHECK(r.valid);
        CHECK(std::fabs(r.noiseDbfs - (-100.0)) < 1e-6);                    // the median outside the passband
        CHECK(std::fabs(r.snrDb - 70.0) < 1e-6);                            // the strongest bin over that floor
        const double want = 10.0 * std::log10(99.0 * 1e-4 + 1e-3);           // 99 bins at -40 dB and one at -30 dB, summed as power
        CHECK(std::fabs(r.signalDbfs - want) < 1e-6);
        CHECK(!readPower(nullptr, 0, 0, 1).valid && !readPower(bins.data(), bins.size(), 600, 500).valid && !readPower(bins.data(), bins.size(), 2000, 2100).valid);
        std::vector<float> nan(8, std::nanf(""));
        CHECK(!readPower(nan.data(), nan.size(), 2, 4).valid);
        const PowerReading all = readPower(bins.data(), bins.size(), 0, 1000);   // the passband is the whole frame: the floor is taken from the whole frame
        CHECK(all.valid && std::isfinite(all.noiseDbfs));
    }

    // ---------------------------------------------------------------- band stacking (G2-32)
    CHECK(bandCount() >= 20);
    for (int i = 0; i < bandCount(); ++i) {
        const BandInfo& b = bandAt(i);
        CHECK(b.lowHz < b.highHz && b.centreHz >= b.lowHz && b.centreHz <= b.highHz && b.bandwidthHz > 0.0 && std::string(b.name).size() > 0 && std::string(b.mode).size() >= 2);
    }
    CHECK(std::string(bandAt(3).name) == "40 m" && std::string(bandAt(3).mode) == "LSB" && std::string(bandAt(5).name) == "20 m" && std::string(bandAt(5).mode) == "USB");
    CHECK(&bandAt(-4) == &bandAt(0) && &bandAt(999) == &bandAt(bandCount() - 1));
    CHECK(registerText(7074000.0, "USB", 2700.0) == "7.074000 MHz USB 2.7 kHz" && registerText(145500000.0, "NFM", 800.0) == "145.500000 MHz NFM 800 Hz" && registerText(1e6, "", 0.0) == "1.000000 MHz");

    // ---------------------------------------------------------------- VFO A / B (G2-33)
    CHECK(std::string(vfoLetter(0)) == "A" && std::string(vfoLetter(1)) == "B" && std::string(vfoLetter(7)) == "B");
    CHECK(vfoText(7074000.0, "USB", 2700.0) == "7.074000 MHz  USB  2.7 kHz" && vfoText(14200000.0, "CW", 0.0) == "14.200000 MHz  CW");

    return 0;
}
