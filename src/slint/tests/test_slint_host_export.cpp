// The waterfall as a file (src/slint/export_logic.hpp, lane S1 item 3): the level store (one row a quarter second, segments at a window change, the hour's limit), the quantiser, the CSV and the
// FoxSDR spectrum file (written and read back, the carrier at the expected frequency within one bin), the picture with its frequency and time scale, the PNG that comes out of it (decoded back by
// the repo's own PNG reader), and the path rules. Pure: no Slint, no engine, no window.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/png_decode.hpp"
#include "export_logic.hpp"
#include "test_check.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace foxsdr::slintexport;

namespace {

constexpr std::int64_t kT0 = 1794398400000LL;   // 2026-11-11T12:00:00.000Z

std::vector<float> spectrum(std::size_t n, std::size_t carrier, float noise = -100.0f, float peak = -20.0f) {
    std::vector<float> b(n, noise);
    b[carrier] = peak;
    return b;
}

std::string collect(const Snapshot& s, const FileFacts& f) {
    std::string out;
    writeCsv(s, f, [&](const void* p, std::size_t n) { out.append(static_cast<const char*>(p), n); });
    return out;
}

void quantiser() {
    CHECK(quantise(-150.0f) == 0 && quantise(-200.0f) == 0 && quantise(1000.0f) == 255);
    CHECK(quantise(std::nanf("")) == 0);
    for (float db = -150.0f; db <= 3.0f; db += 0.37f) { CHECK(std::fabs(dequantise(quantise(db)) - db) <= kStepDb / 2 + 1e-3f); }   // within half a step
    CHECK(std::fabs(dequantise(quantise(-20.0f)) - (-20.0f)) <= 0.31f);
    CHECK(quantise(-60.0f) < quantise(-59.0f));
}

void storeRows() {
    SpectrumStore st;
    const auto b = spectrum(2048, 700);
    CHECK(st.push(kT0, 100e6, 2e6, b.data(), b.size()));
    CHECK(!st.push(kT0 + 100, 100e6, 2e6, b.data(), b.size()));       // one row in 250 ms
    CHECK(!st.push(kT0 + 249, 100e6, 2e6, b.data(), b.size()));
    CHECK(st.push(kT0 + 250, 100e6, 2e6, b.data(), b.size()));
    CHECK(st.rows() == 2 && st.segments() == 1);
    CHECK(!st.push(kT0, 100e6, 2e6, nullptr, 5) && !st.push(kT0 + 9999, 100e6, 2e6, b.data(), 0));
    // 2048 bins are reduced to 1024 columns taking the strongest: the carrier in bin 700 is in column 350
    const Snapshot s = st.snapshot(5, kT0 + 1000);
    CHECK(s.segments.size() == 1 && s.segments[0].columns == 1024 && s.segments[0].rows() == 2);
    const auto& lv = s.segments[0].levels;
    CHECK(std::fabs(dequantise(lv[350]) - (-20.0f)) <= 0.31f && std::fabs(dequantise(lv[349]) - (-100.0f)) <= 0.31f);
    // fewer bins than the limit are kept as they are
    SpectrumStore small;
    const auto b2 = spectrum(300, 10);
    CHECK(small.push(kT0, 7.1e6, 48e3, b2.data(), b2.size()));
    CHECK(small.snapshot(1, kT0).segments[0].columns == 300);
    CHECK(st.bytes() > 2000 && st.bytes() < 2 * 1024 * 1024);
}

void segmentsAtWindowChange() {
    SpectrumStore st;
    const auto b = spectrum(1024, 100);
    for (int i = 0; i < 4; ++i) { CHECK(st.push(kT0 + 250 * i, 100e6, 2e6, b.data(), b.size())); }
    for (int i = 4; i < 8; ++i) { CHECK(st.push(kT0 + 250 * i, 101e6, 2e6, b.data(), b.size())); }    // a retune
    for (int i = 8; i < 10; ++i) { CHECK(st.push(kT0 + 250 * i, 101e6, 1e6, b.data(), b.size())); }   // a new span
    const auto b3 = spectrum(512, 10);
    CHECK(st.push(kT0 + 250 * 10, 101e6, 1e6, b3.data(), b3.size()));                                // a different number of columns
    CHECK(st.segments() == 4 && st.rows() == 11);
    const Snapshot s = st.snapshot(5, kT0 + 5000);
    CHECK(s.segments.size() == 4 && s.segments[0].centreHz == 100e6 && s.segments[1].centreHz == 101e6 && s.segments[2].spanHz == 1e6 && s.segments[3].columns == 512);
    CHECK(s.segments[0].rows() == 4 && s.segments[1].rows() == 4 && s.segments[2].rows() == 2 && s.segments[3].rows() == 1);
    // rows of different windows are never mixed: every segment's levels are rows x its own columns
    for (const Segment& g : s.segments) { CHECK(g.levels.size() == g.rows() * g.columns && g.dtMs.size() == g.rows()); }
}

void lengths() {
    SpectrumStore st;
    const auto b = spectrum(64, 5);
    for (int i = 0; i < 20 * 60 * 4; ++i) { (void)st.push(kT0 + 250LL * i, 1e6, 1e5, b.data(), b.size()); }   // 20 minutes
    const std::int64_t now = kT0 + 250LL * (20 * 60 * 4 - 1);
    CHECK(st.snapshot(1, now).rows() >= 239 && st.snapshot(1, now).rows() <= 241);
    CHECK(st.snapshot(5, now).rows() >= 1199 && st.snapshot(5, now).rows() <= 1201);
    CHECK(st.snapshot(15, now).rows() >= 3599 && st.snapshot(15, now).rows() <= 3601);
    CHECK(st.snapshot(60, now).rows() == 4800);
    const Snapshot one = st.snapshot(1, now);
    CHECK(one.segments[0].startMs >= now - 60000 && one.segments[0].dtMs[0] == 0 && one.segments[0].dtMs.back() == static_cast<std::uint32_t>(now - one.segments[0].startMs));
    CHECK(st.snapshot(1, now + 3600LL * 1000 * 5).empty());   // nothing that recent
}

void hourLimit() {
    SpectrumStore st;
    const auto b = spectrum(32, 5);
    const std::int64_t rows = 70LL * 60 * 4;   // 70 minutes
    for (std::int64_t i = 0; i < rows; ++i) { (void)st.push(kT0 + 250 * i, 1e6, 1e5, b.data(), b.size()); }
    CHECK(st.rows() <= 60 * 60 * 4 + 241 + 240 && st.rows() >= 60 * 60 * 4);   // an hour, and at most a minute of slack
    CHECK(st.bytes() < static_cast<std::size_t>(rows) * 40);
    // and an hour of 1024-column rows is the 14.4 MB the header promises (measured, not computed)
    SpectrumStore big;
    const auto wide = spectrum(1024, 5);
    for (std::int64_t i = 0; i < 60LL * 60 * 4; ++i) { (void)big.push(kT0 + 250 * i, 1e6, 1e5, wide.data(), wide.size()); }
    std::printf("store: one hour of 1024 columns = %.1f MB\n", static_cast<double>(big.bytes()) / 1e6);
    CHECK(big.bytes() < 16'000'000);
}

void csv() {
    SpectrumStore st;
    const auto b = spectrum(8, 3, -100.0f, -30.0f);
    (void)st.push(kT0, 100e6, 8e3, b.data(), b.size());
    (void)st.push(kT0 + 250, 100e6, 8e3, b.data(), b.size());
    const auto b2 = spectrum(4, 1);
    (void)st.push(kT0 + 500, 101e6, 4e3, b2.data(), b2.size());
    FileFacts f;
    f.radio = 2;
    f.note = "FoxSDR 0.99.108";
    const std::string text = collect(st.snapshot(5, kT0 + 1000), f);
    CHECK(text.find("# FoxSDR waterfall data\n# radio,2\n# FoxSDR 0.99.108\n") == 0);
    CHECK(text.find("# segment,1,start_utc,2026-11-11T12:00:00.000Z,centre_hz,100000000.0,span_hz,8000.0,columns,8,rows,2\n") != std::string::npos);
    CHECK(text.find("# segment,2,start_utc,2026-11-11T12:00:00.500Z,centre_hz,101000000.0,span_hz,4000.0,columns,4,rows,1\n") != std::string::npos);
    CHECK(text.find("2026-11-11T12:00:00.250Z,-100.2,-100.2,-100.2,-30.0,") != std::string::npos || text.find("2026-11-11T12:00:00.250Z,-100.0,") != std::string::npos);
    // the data rows: time plus one value a column
    std::size_t rowsSeen = 0;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t nl = text.find('\n', at);
        const std::string line = text.substr(at, nl - at);
        at = nl + 1;
        if (line.empty() || line[0] == '#') { continue; }
        ++rowsSeen;
        const std::size_t commas = static_cast<std::size_t>(std::count(line.begin(), line.end(), ','));
        CHECK(commas == 8 || commas == 4);
    }
    CHECK(rowsSeen == 3);
    // the carrier's column is the strongest and is at the expected frequency within one bin
    const Snapshot s = st.snapshot(5, kT0 + 1000);
    const Segment& g = s.segments[0];
    const auto strongest = std::max_element(g.levels.begin(), g.levels.begin() + 8) - g.levels.begin();
    const double hz = g.centreHz - g.spanHz / 2 + (static_cast<double>(strongest) + 0.5) * g.spanHz / g.columns;
    CHECK(std::fabs(hz - 99999500.0) < 1e-6 && std::fabs(hz - 99.9995e6) <= g.spanHz / g.columns);   // column 3 of 8 over 8 kHz, centre 100 MHz
}

void binaryFile() {
    SpectrumStore st;
    const auto b = spectrum(1024, 512, -90.0f, -10.0f);
    for (int i = 0; i < 40; ++i) { (void)st.push(kT0 + 250 * i, 14.1e6, 48e3, b.data(), b.size()); }
    const auto b2 = spectrum(100, 40);
    for (int i = 40; i < 50; ++i) { (void)st.push(kT0 + 250 * i, 7.074e6, 48e3, b2.data(), b2.size()); }
    const Snapshot s = st.snapshot(5, kT0 + 20000);
    std::vector<std::uint8_t> bytes;
    writeBinary(s, [&](const void* p, std::size_t n) { const auto* q = static_cast<const std::uint8_t*>(p); bytes.insert(bytes.end(), q, q + n); });
    CHECK(bytes.size() > 40 * 1024 && std::string(bytes.begin(), bytes.begin() + 4) == "FXSP");
    const auto back = readBinary(bytes);
    CHECK(back.has_value() && back->segments.size() == 2);
    CHECK(back->segments[0].columns == 1024 && back->segments[0].rows() == 40 && back->segments[0].centreHz == 14.1e6 && back->segments[0].spanHz == 48e3 && back->segments[0].startMs == kT0);
    CHECK(back->segments[0].levels == s.segments[0].levels && back->segments[0].dtMs == s.segments[0].dtMs && back->segments[1].levels == s.segments[1].levels);
    const auto& lv = back->segments[0].levels;
    const auto peak = std::max_element(lv.begin(), lv.begin() + 1024) - lv.begin();
    CHECK(peak == 512 && std::fabs(dequantise(lv[512]) - (-10.0f)) <= 0.31f);
    // damaged files are refused, never half read
    CHECK(!readBinary({}).has_value());
    std::vector<std::uint8_t> cut(bytes.begin(), bytes.begin() + 100);
    CHECK(!readBinary(cut).has_value());
    std::vector<std::uint8_t> badMagic = bytes;
    badMagic[0] = 'X';
    CHECK(!readBinary(badMagic).has_value());
    std::vector<std::uint8_t> badVersion = bytes;
    badVersion[4] = 9;
    CHECK(!readBinary(badVersion).has_value());
}

void clockText() {
    CHECK(isoUtc(kT0) == "2026-11-11T12:00:00.000Z");
    CHECK(isoUtc(kT0 + 250) == "2026-11-11T12:00:00.250Z");
    CHECK(isoUtc(0) == "1970-01-01T00:00:00.000Z");
    CHECK(isoUtc(951782400000LL) == "2000-02-29T00:00:00.000Z");   // a leap day
    CHECK(captionUtc(kT0 + 3723000) == "2026-11-11 13:02:03");
}

void scales() {
    const auto f = frequencyLabels(100.3e6, 2e6, 1024);
    CHECK(!f.empty() && f.size() <= 10);
    for (std::size_t i = 1; i < f.size(); ++i) { CHECK(f[i].first - f[i - 1].first >= 100.0); }   // never crowded
    CHECK(f.front().second == "99.5" || f.front().second == "99.4" || f.front().second == "99.50" || f.front().second.rfind("99.", 0) == 0);
    // a label sits where its frequency is: 100.0 MHz in a 2 MHz window centred on 100.3 is at (100.0 - 99.3) / 2 of the width
    bool found = false;
    for (const auto& [x, text] : frequencyLabels(100.3e6, 2e6, 1000)) { if (std::fabs(std::stod(text) - 100.0) < 1e-9) { found = true; CHECK(std::fabs(x - 350.0) < 1e-6); } }
    CHECK(found);
    CHECK(frequencyLabels(100e6, 0, 1024).empty() && frequencyLabels(100e6, 1e6, 50).empty());
    // a narrow window gets finer labels
    const auto n = frequencyLabels(7.074e6, 3000, 1024);
    CHECK(n.size() >= 2 && n[0].second.find('.') != std::string::npos);
    // the time scale: a line every 30+ px, the first at row 0 ("0 S"), minutes past a minute
    const auto t = timeLabels(238, 1.0 / 30.0);
    CHECK(!t.empty() && t[0].first == 0 && t[0].second == "0 S");
    for (std::size_t i = 1; i < t.size(); ++i) { CHECK(t[i].first - t[i - 1].first >= 29); }
    const auto mins = timeLabels(238, 1.0);
    CHECK(mins.size() >= 2 && mins[1].second.back() == 'S');
    const auto slow = timeLabels(238, 2.0);   // 238 rows of 2 s = 476 s: a line every 30 px = 60 s
    CHECK(slow.size() >= 2 && slow[1].second == "-1 M");
    const auto slower = timeLabels(238, 10.0);
    CHECK(slower.size() >= 2 && slower[1].second == "-5 M");
    CHECK(timeLabels(238, 0.0).empty() && timeLabels(10, 1.0).empty());
}

void picture() {
    constexpr std::uint32_t W = 256, H = 120;
    std::vector<Pixel> pic(W * H, Pixel{0, 0, 80, 255});
    for (std::uint32_t y = 0; y < H; ++y) { pic[y * W + 100] = Pixel{255, 200, 0, 255}; }   // a carrier line
    PictureSpec spec;
    spec.picture = pic.data(); spec.w = W; spec.h = H; spec.centreHz = 100e6; spec.spanHz = 1e6; spec.secondsPerRow = 0.5; spec.newestUtcMs = kT0; spec.radio = 3;
    const Image img = renderPicture(spec);
    CHECK(img.w == kMarginLeft + W + 8 && img.h == kMarginTop + H + 8 && img.px.size() == static_cast<std::size_t>(img.w) * img.h);
    // the picture's pixels are copied unchanged into their place
    bool same = true;
    for (std::uint32_t y = 0; y < H && same; ++y) { for (std::uint32_t x = 0; x < W; ++x) { const Pixel& a = img.px[(y + kMarginTop) * img.w + kMarginLeft + x]; const Pixel& b = pic[y * W + x]; if (a.r != b.r || a.g != b.g || a.b != b.b) { same = false; break; } } }
    CHECK(same);
    // the FREQUENCY SCALE is drawn: every label's place (its tick's x) has ink in the band of the labels, 14 px tall at row 26 (a picture without the scale has none)
    for (const auto& [x, text] : frequencyLabels(spec.centreHz, spec.spanHz, spec.w)) {
        const int px = static_cast<int>(kMarginLeft) + static_cast<int>(std::lround(x));
        std::size_t ink = 0;
        for (int y = 26; y < 40; ++y) { for (int dx = -30; dx <= 30; ++dx) { if (px + dx >= 0 && px + dx < static_cast<int>(img.w) && img.px[static_cast<std::size_t>(y) * img.w + static_cast<std::size_t>(px + dx)].r > 0xa0) { ++ink; } } }
        CHECK(ink > 10);
        // and the tick line above the picture
        CHECK(img.px[static_cast<std::size_t>(kMarginTop - 3) * img.w + static_cast<std::size_t>(px)].r > 0x50);
    }
    // the TIME SCALE is drawn: a tick at the left of each label's row
    for (const auto& [row, text] : timeLabels(spec.h, spec.secondsPerRow)) { CHECK(img.px[static_cast<std::size_t>(kMarginTop + row) * img.w + static_cast<std::size_t>(kMarginLeft - 3)].r > 0x50); }
    // the margins carry ink (the caption, the scales): not one flat colour
    std::size_t lit = 0;
    for (std::uint32_t y = 0; y < kMarginTop - 8; ++y) { for (std::uint32_t x = 0; x < img.w; ++x) { if (img.px[y * img.w + x].r > 0xa0) { ++lit; } } }
    CHECK(lit > 400);
    std::size_t litLeft = 0;
    for (std::uint32_t y = kMarginTop; y < kMarginTop + H; ++y) { for (std::uint32_t x = 0; x < kMarginLeft - 4; ++x) { if (img.px[y * img.w + x].r > 0xa0) { ++litLeft; } } }
    CHECK(litLeft > 20);   // the time labels
    // PNG: signature, the text chunk, the repo's decoder reads it back to the same pixels
    const std::string text = "centre_hz=100000000;span_hz=1000000;radio=3";
    const std::vector<std::uint8_t> png = encodePng(img, text);
    CHECK(png.size() > img.w * img.h * 3 && png[0] == 0x89 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G');
    CHECK(pngText(png, "FoxSDR") == text && pngText(png, "Other").empty());
    CHECK(pngText(encodePng(img, ""), "FoxSDR").empty());
    // the repo's own PNG reader (stb) decodes what the encoder wrote, pixel for pixel (RGB; alpha comes back opaque)
    cascade::core::PngImage dec;
    std::string why;
    CHECK(cascade::core::decodePng(png, 4096, dec, why) && dec.width == static_cast<int>(img.w) && dec.height == static_cast<int>(img.h));
    bool equal = dec.rgba.size() == static_cast<std::size_t>(img.w) * img.h * 4;
    for (std::size_t i = 0; equal && i < img.px.size(); ++i) { equal = dec.rgba[i * 4] == img.px[i].r && dec.rgba[i * 4 + 1] == img.px[i].g && dec.rgba[i * 4 + 2] == img.px[i].b && dec.rgba[i * 4 + 3] == 255; }
    CHECK(equal);
    // a file cut short is refused by the reader, not drawn as a half picture
    std::vector<std::uint8_t> shortened(png.begin(), png.begin() + static_cast<std::ptrdiff_t>(png.size() / 2));
    cascade::core::PngImage bad;
    CHECK(!cascade::core::decodePng(shortened, 4096, bad, why));
    // the chunks' checksums are the PNG standard's (CRC-32 over type and data): the IEND chunk of every PNG ends with AE 42 60 82
    CHECK(png.size() > 8 && png[png.size() - 4] == 0xAE && png[png.size() - 3] == 0x42 && png[png.size() - 2] == 0x60 && png[png.size() - 1] == 0x82);
}

void paths() {
    CHECK(withExtension("C:/out/wf", "png") == "C:/out/wf.png");
    CHECK(withExtension("C:/out/wf.png", "png") == "C:/out/wf.png");
    CHECK(withExtension("C:/out.d/wf", "csv") == "C:/out.d/wf.csv");   // a dot in a folder is not an extension
    CHECK(withExtension("wf.", "csv") == "wf.csv");
    CHECK(withExtension("", "png") == "");
    CHECK(freePath("a.png", [](const std::string&) { return false; }) == "a.png");
    CHECK(freePath("a.png", [](const std::string& p) { return p == "a.png"; }) == "a (2).png");
    CHECK(freePath("a.png", [](const std::string& p) { return p == "a.png" || p == "a (2).png"; }) == "a (3).png");
    CHECK(freePath("C:/x.d/a", [](const std::string& p) { return p == "C:/x.d/a"; }) == "C:/x.d/a (2)");
}

std::vector<std::uint8_t> slurp(const std::string& path) {
    std::vector<std::uint8_t> v;
    if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
        std::uint8_t buf[4096];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) { v.insert(v.end(), buf, buf + n); }
        std::fclose(f);
    }
    return v;
}

void files() {
    namespace fs = std::filesystem;
    const fs::path base = fs::temp_directory_path() / "s1-export-test";
    std::error_code ec;
    fs::remove_all(base, ec);
    const std::string folder = base.string();
    // a picture to a path that has no folder yet and no extension: the folder is made, ".png" added
    std::vector<Pixel> pic(64 * 40, Pixel{10, 20, 30, 255});
    PictureSpec spec;
    spec.w = 64; spec.h = 40; spec.centreHz = 7.074e6; spec.spanHz = 48e3; spec.secondsPerRow = 0.25; spec.newestUtcMs = kT0; spec.radio = 2; spec.dbMin = -110; spec.dbMax = 0;
    const Outcome a = savePicture("sub/dir/wf", folder, pic, spec);
    CHECK(a.ok && a.error.empty() && a.bytes > 0 && fs::exists(fs::path(a.path)) && a.path.find("wf.png") != std::string::npos);
    CHECK(slurp(a.path).size() == a.bytes);
    CHECK(pngText(slurp(a.path), "FoxSDR") == pictureFacts(spec));
    CHECK(pictureFacts(spec).find("radio=2;centre_hz=7074000.0;span_hz=48000.0;newest_utc=2026-11-11T12:00:00.000Z") == 0);
    // the same path again is never overwritten: the next file is "wf (2).png"
    const Outcome b = savePicture("sub/dir/wf.png", folder, pic, spec);
    CHECK(b.ok && b.path != a.path && b.path.find("wf (2).png") != std::string::npos && fs::exists(fs::path(a.path)));
    // no half file is left behind under any name
    std::size_t part = 0;
    for (const auto& e : fs::recursive_directory_iterator(base)) { if (e.path().extension() == ".part") { ++part; } }
    CHECK(part == 0);
    // an absolute path with quotes and spaces around it (Explorer's "Copy as path")
    const Outcome q = savePicture("  \"" + (base / "quoted.png").string() + "\"  ", "", pic, spec);
    CHECK(q.ok && fs::exists(base / "quoted.png"));
    // data: CSV and FoxSDR spectrum from a store, read back
    SpectrumStore st;
    const auto bins = spectrum(256, 100, -95.0f, -25.0f);
    for (int i = 0; i < 20; ++i) { (void)st.push(kT0 + 250 * i, 14.1e6, 48e3, bins.data(), bins.size()); }
    FileFacts facts;
    facts.radio = 2;
    const Outcome c = saveData("data", folder, st.snapshot(5, kT0 + 5000), 0, facts);
    CHECK(c.ok && c.rows == 20 && c.path.find("data.csv") != std::string::npos);
    const std::vector<std::uint8_t> csvBytes = slurp(c.path);
    CHECK(std::string(csvBytes.begin(), csvBytes.begin() + 24) == "# FoxSDR waterfall data\n");
    const Outcome d = saveData("data", folder, st.snapshot(5, kT0 + 5000), 1, facts);
    CHECK(d.ok && d.rows == 20 && d.path.find("data.fsw") != std::string::npos && d.path != c.path);
    const auto back = readBinary(slurp(d.path));
    CHECK(back.has_value() && back->rows() == 20 && back->segments[0].centreHz == 14.1e6);
    const auto peak = std::max_element(back->segments[0].levels.begin(), back->segments[0].levels.begin() + 256) - back->segments[0].levels.begin();
    CHECK(peak == 100);   // the carrier's column: at 14.1 MHz - 24 kHz + (100.5 / 256) * 48 kHz = 14.0948 MHz, within one column (187.5 Hz)
    // refusals: no name, nothing to write, a folder that cannot be made (its parent is a file), no picture
    CHECK(!saveData("", folder, st.snapshot(5, kT0 + 5000), 0, facts).ok && !saveData("  ", folder, st.snapshot(5, kT0 + 5000), 0, facts).error.empty());
    CHECK(!saveData("empty", folder, Snapshot{}, 0, facts).ok);
    const Outcome blocked = saveData((base / "quoted.png" / "inside" / "x").string(), "", st.snapshot(5, kT0 + 5000), 0, facts);
    CHECK(!blocked.ok && !blocked.error.empty());
    CHECK(!savePicture("none", folder, {}, spec).ok);
    CHECK(resolvePath("", folder, "png").empty() && resolvePath("a.png", "", "png") == "a.png" && resolvePath("a", "", "png") == "a.png");
    fs::remove_all(base, ec);
}

}  // namespace

int main() {
    files();
    quantiser();
    storeRows();
    segmentsAtWindowChange();
    lengths();
    hourLimit();
    csv();
    binaryFile();
    clockText();
    scales();
    picture();
    paths();
    return testSummary("test_slint_host_export");
}
