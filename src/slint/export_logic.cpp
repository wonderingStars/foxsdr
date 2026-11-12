// export_logic.cpp - see export_logic.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "export_logic.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace foxsdr::slintexport {

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- the levels
std::uint8_t quantise(float db) {
    if (!(db == db)) { return 0; }
    const float q = std::floor((db - kFloorDb) / kStepDb + 0.5f);
    return static_cast<std::uint8_t>(std::clamp(q, 0.0f, 255.0f));
}
float dequantise(std::uint8_t q) { return kFloorDb + kStepDb * static_cast<float>(q); }

bool SpectrumStore::push(std::int64_t utcMs, double centreHz, double spanHz, const float* bins, std::size_t n) {
    if (bins == nullptr || n == 0) { return false; }
    if (lastRowMs_ >= 0 && utcMs >= lastRowMs_ && utcMs - lastRowMs_ < kRowGapMs) { return false; }
    const std::uint32_t columns = static_cast<std::uint32_t>(std::min<std::size_t>(n, kMaxColumns));
    Segment* seg = segments_.empty() ? nullptr : &segments_.back();
    const bool sameWindow = seg != nullptr && seg->columns == columns && std::fabs(seg->centreHz - centreHz) < 0.5 && std::fabs(seg->spanHz - spanHz) < 0.5 &&
                            utcMs >= seg->startMs && utcMs - seg->startMs < 0x7fffffffLL;
    if (!sameWindow) {
        segments_.emplace_back();
        seg = &segments_.back();
        seg->centreHz = centreHz;
        seg->spanHz = spanHz;
        seg->columns = columns;
        seg->startMs = utcMs;
    }
    seg->dtMs.push_back(static_cast<std::uint32_t>(utcMs - seg->startMs));
    const std::size_t at = seg->levels.size();
    seg->levels.resize(at + columns);
    for (std::uint32_t c = 0; c < columns; ++c) {   // the strongest bin of each group
        const std::size_t b0 = static_cast<std::size_t>(c) * n / columns;
        const std::size_t b1 = std::max(b0 + 1, static_cast<std::size_t>(c + 1) * n / columns);
        float peak = bins[b0];
        for (std::size_t b = b0 + 1; b < b1 && b < n; ++b) { peak = std::max(peak, bins[b]); }
        seg->levels[at + c] = quantise(peak);
    }
    lastRowMs_ = utcMs;
    // forget what is older than the hour: whole segments, and the front rows of the oldest one (in chunks of a minute's worth so the move is rare)
    const std::int64_t cutoff = utcMs - kKeepMs;
    while (segments_.size() > 1) {
        const Segment& s = segments_.front();
        if (s.rows() > 0 && s.startMs + static_cast<std::int64_t>(s.dtMs.back()) < cutoff) { segments_.erase(segments_.begin()); } else { break; }
    }
    Segment& old = segments_.front();
    if (old.rows() > 240 && old.startMs + static_cast<std::int64_t>(old.dtMs.front()) < cutoff - 60000) {
        std::size_t drop = 0;
        while (drop < old.rows() && old.startMs + static_cast<std::int64_t>(old.dtMs[drop]) < cutoff) { ++drop; }
        if (drop > 0 && drop < old.rows()) {
            old.dtMs.erase(old.dtMs.begin(), old.dtMs.begin() + static_cast<std::ptrdiff_t>(drop));
            old.levels.erase(old.levels.begin(), old.levels.begin() + static_cast<std::ptrdiff_t>(drop * old.columns));
        }
    }
    return true;
}

Snapshot SpectrumStore::snapshot(int minutes, std::int64_t nowMs) const {
    Snapshot out;
    const std::int64_t from = nowMs - static_cast<std::int64_t>(std::clamp(minutes, 1, 60)) * 60000;
    for (const Segment& s : segments_) {
        std::size_t first = 0;
        while (first < s.rows() && s.startMs + static_cast<std::int64_t>(s.dtMs[first]) < from) { ++first; }
        if (first >= s.rows()) { continue; }
        Segment c;
        c.centreHz = s.centreHz;
        c.spanHz = s.spanHz;
        c.columns = s.columns;
        c.startMs = s.startMs + static_cast<std::int64_t>(s.dtMs[first]);
        const std::uint32_t base = s.dtMs[first];
        for (std::size_t r = first; r < s.rows(); ++r) { c.dtMs.push_back(s.dtMs[r] - base); }
        c.levels.assign(s.levels.begin() + static_cast<std::ptrdiff_t>(first * s.columns), s.levels.end());
        out.segments.push_back(std::move(c));
    }
    return out;
}

std::size_t SpectrumStore::rows() const { std::size_t n = 0; for (const Segment& s : segments_) { n += s.rows(); } return n; }
std::size_t SpectrumStore::bytes() const { std::size_t n = 0; for (const Segment& s : segments_) { n += s.levels.size() + s.dtMs.size() * 4; } return n; }

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- time text
namespace {

// days since 1970-01-01 -> civil date (Howard Hinnant's algorithm)
void civil(std::int64_t days, int& y, int& m, int& d) {
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const std::int64_t doe = days - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<int>(yoe + era * 400);
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    if (m <= 2) { ++y; }
}

std::string clock(std::int64_t ms, bool iso) {
    std::int64_t secs = ms / 1000;
    std::int64_t frac = ms % 1000;
    if (frac < 0) { frac += 1000; --secs; }
    std::int64_t days = secs / 86400;
    std::int64_t rem = secs % 86400;
    if (rem < 0) { rem += 86400; --days; }
    int y = 0, mo = 0, d = 0;
    civil(days, y, mo, d);
    char b[64];
    if (iso) { std::snprintf(b, sizeof(b), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", y, mo, d, static_cast<int>(rem / 3600), static_cast<int>(rem % 3600 / 60), static_cast<int>(rem % 60), static_cast<int>(frac)); }
    else { std::snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d:%02d", y, mo, d, static_cast<int>(rem / 3600), static_cast<int>(rem % 3600 / 60), static_cast<int>(rem % 60)); }
    return b;
}

}  // namespace

std::string isoUtc(std::int64_t ms) { return clock(ms, true); }
std::string captionUtc(std::int64_t ms) { return clock(ms, false); }

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- the files
void writeCsv(const Snapshot& s, const FileFacts& facts, const Sink& sink) {
    const auto put = [&](const std::string& t) { sink(t.data(), t.size()); };
    char b[256];
    put("# FoxSDR waterfall data\n");
    std::snprintf(b, sizeof(b), "# radio,%d\n", facts.radio);
    put(b);
    if (!facts.note.empty()) { put("# " + facts.note + "\n"); }
    put("# rows are UTC time then one level in dBFS per column; the level steps are 0.6 dB (a row is stored in one byte a column), at most 1024 columns\n");
    put("# column i of a segment is at centre_hz - span_hz / 2 + (i + 0.5) * span_hz / columns\n");
    int n = 0;
    for (const Segment& seg : s.segments) {
        if (seg.rows() == 0) { continue; }
        std::snprintf(b, sizeof(b), "# segment,%d,start_utc,%s,centre_hz,%.1f,span_hz,%.1f,columns,%u,rows,%zu\n", ++n, isoUtc(seg.startMs).c_str(), seg.centreHz, seg.spanHz, seg.columns, seg.rows());
        put(b);
        std::string line;
        for (std::size_t r = 0; r < seg.rows(); ++r) {
            line = isoUtc(seg.startMs + static_cast<std::int64_t>(seg.dtMs[r]));
            for (std::uint32_t c = 0; c < seg.columns; ++c) {
                std::snprintf(b, sizeof(b), ",%.1f", static_cast<double>(dequantise(seg.levels[r * seg.columns + c])));
                line += b;
            }
            line += '\n';
            put(line);
        }
    }
}

namespace {
template <class T> void putLe(std::vector<std::uint8_t>& v, T x) {
    std::uint8_t raw[sizeof(T)];
    std::memcpy(raw, &x, sizeof(T));   // little-endian hosts only (Windows, Linux x86-64 and arm64): the file is little-endian
    v.insert(v.end(), raw, raw + sizeof(T));
}
template <class T> bool getLe(const std::vector<std::uint8_t>& v, std::size_t& at, T& x) {
    if (at + sizeof(T) > v.size()) { return false; }
    std::memcpy(&x, v.data() + at, sizeof(T));
    at += sizeof(T);
    return true;
}
}  // namespace

void writeBinary(const Snapshot& s, const Sink& sink) {
    std::vector<std::uint8_t> head{'F', 'X', 'S', 'P'};
    putLe<std::uint32_t>(head, 1);
    std::uint32_t count = 0;
    for (const Segment& seg : s.segments) { if (seg.rows() > 0) { ++count; } }
    putLe<std::uint32_t>(head, count);
    sink(head.data(), head.size());
    for (const Segment& seg : s.segments) {
        if (seg.rows() == 0) { continue; }
        std::vector<std::uint8_t> h;
        putLe<std::uint32_t>(h, seg.columns);
        putLe<double>(h, seg.centreHz);
        putLe<double>(h, seg.spanHz);
        putLe<float>(h, kFloorDb);
        putLe<float>(h, kStepDb);
        putLe<std::int64_t>(h, seg.startMs);
        putLe<std::uint32_t>(h, static_cast<std::uint32_t>(seg.rows()));
        sink(h.data(), h.size());
        std::vector<std::uint8_t> row;
        for (std::size_t r = 0; r < seg.rows(); ++r) {
            row.clear();
            putLe<std::uint32_t>(row, seg.dtMs[r]);
            row.insert(row.end(), seg.levels.begin() + static_cast<std::ptrdiff_t>(r * seg.columns), seg.levels.begin() + static_cast<std::ptrdiff_t>((r + 1) * seg.columns));
            sink(row.data(), row.size());
        }
    }
}

std::optional<Snapshot> readBinary(const std::vector<std::uint8_t>& v) {
    if (v.size() < 12 || std::memcmp(v.data(), "FXSP", 4) != 0) { return std::nullopt; }
    std::size_t at = 4;
    std::uint32_t version = 0, count = 0;
    if (!getLe(v, at, version) || version != 1 || !getLe(v, at, count)) { return std::nullopt; }
    Snapshot out;
    for (std::uint32_t i = 0; i < count; ++i) {
        Segment seg;
        float floorDb = 0, stepDb = 0;
        std::uint32_t rows = 0;
        if (!getLe(v, at, seg.columns) || !getLe(v, at, seg.centreHz) || !getLe(v, at, seg.spanHz) || !getLe(v, at, floorDb) || !getLe(v, at, stepDb) || !getLe(v, at, seg.startMs) || !getLe(v, at, rows)) { return std::nullopt; }
        if (seg.columns == 0 || seg.columns > kMaxColumns || floorDb != kFloorDb || stepDb != kStepDb) { return std::nullopt; }
        for (std::uint32_t r = 0; r < rows; ++r) {
            std::uint32_t dt = 0;
            if (!getLe(v, at, dt) || at + seg.columns > v.size()) { return std::nullopt; }
            seg.dtMs.push_back(dt);
            seg.levels.insert(seg.levels.end(), v.begin() + static_cast<std::ptrdiff_t>(at), v.begin() + static_cast<std::ptrdiff_t>(at + seg.columns));
            at += seg.columns;
        }
        out.segments.push_back(std::move(seg));
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- the picture
namespace {

struct Glyph { char c; const char* rows[7]; };
// a 5 x 7 face for the picture's scales and caption: digits, a few signs and the capitals the words need (no localisation: the picture says UTC, MHZ, SPAN and the numbers)
const Glyph kFont[] = {
    {'0', {".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."}}, {'1', {"..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."}},
    {'2', {".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"}}, {'3', {".###.", "#...#", "....#", "..##.", "....#", "#...#", ".###."}},
    {'4', {"...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."}}, {'5', {"#####", "#....", "####.", "....#", "....#", "#...#", ".###."}},
    {'6', {"..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."}}, {'7', {"#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."}},
    {'8', {".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."}}, {'9', {".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.."}},
    {'.', {".....", ".....", ".....", ".....", ".....", ".##..", ".##.."}}, {':', {".....", ".##..", ".##..", ".....", ".##..", ".##..", "....."}},
    {'-', {".....", ".....", ".....", "#####", ".....", ".....", "....."}}, {'+', {".....", "..#..", "..#..", "#####", "..#..", "..#..", "....."}},
    {'/', {"....#", "....#", "...#.", "..#..", ".#...", "#....", "#...."}}, {' ', {".....", ".....", ".....", ".....", ".....", ".....", "....."}},
    {'A', {".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}}, {'B', {"####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."}},
    {'C', {".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."}}, {'D', {"####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."}},
    {'E', {"#####", "#....", "#....", "####.", "#....", "#....", "#####"}}, {'F', {"#####", "#....", "#....", "####.", "#....", "#....", "#...."}},
    {'G', {".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".###."}}, {'H', {"#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"}},
    {'I', {".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."}}, {'K', {"#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"}},
    {'L', {"#....", "#....", "#....", "#....", "#....", "#....", "#####"}}, {'M', {"#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"}},
    {'N', {"#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#"}}, {'O', {".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}},
    {'P', {"####.", "#...#", "#...#", "####.", "#....", "#....", "#...."}}, {'R', {"####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"}},
    {'S', {".####", "#....", "#....", ".###.", "....#", "....#", "####."}}, {'T', {"#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."}},
    {'U', {"#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."}}, {'W', {"#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#"}},
    {'Y', {"#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."}}, {'V', {"#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."}},
    {'J', {"..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."}}, {'Q', {".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"}},
    {'X', {"#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"}}, {'Z', {"#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"}},
};

const Glyph* glyphOf(char c) {
    if (c >= 'a' && c <= 'z') { c = static_cast<char>(c - 'a' + 'A'); }
    for (const Glyph& g : kFont) { if (g.c == c) { return &g; } }
    return nullptr;
}

constexpr int kScale = 2, kAdvance = 6 * kScale;
int textWidth(const std::string& t) { return static_cast<int>(t.size()) * kAdvance; }

void put(Image& img, int x, int y, Pixel p) {
    if (x < 0 || y < 0 || x >= static_cast<int>(img.w) || y >= static_cast<int>(img.h)) { return; }
    img.px[static_cast<std::size_t>(y) * img.w + static_cast<std::size_t>(x)] = p;
}

void drawText(Image& img, int x, int y, const std::string& t, Pixel ink) {
    for (char ch : t) {
        if (const Glyph* g = glyphOf(ch)) {
            for (int r = 0; r < 7; ++r) {
                for (int c = 0; c < 5; ++c) {
                    if (g->rows[r][c] != '#') { continue; }
                    for (int dy = 0; dy < kScale; ++dy) { for (int dx = 0; dx < kScale; ++dx) { put(img, x + c * kScale + dx, y + r * kScale + dy, ink); } }
                }
            }
        }
        x += kAdvance;
    }
}

std::string num(double v, int decimals) {
    char b[48];
    std::snprintf(b, sizeof(b), "%.*f", decimals, v);
    return b;
}

}  // namespace

void drawLabelRgba(std::uint8_t* rgba, std::uint32_t w, std::uint32_t h, int x, int y, const std::string& text, std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a, int scale) {
    if (rgba == nullptr || scale < 1) { return; }
    for (char ch : text) {
        if (const Glyph* gl = glyphOf(ch)) {
            for (int row = 0; row < 7; ++row) {
                for (int col = 0; col < 5; ++col) {
                    if (gl->rows[row][col] != '#') { continue; }
                    for (int dy = 0; dy < scale; ++dy) {
                        for (int dx = 0; dx < scale; ++dx) {
                            const int px = x + col * scale + dx, py = y + row * scale + dy;
                            if (px < 0 || py < 0 || px >= static_cast<int>(w) || py >= static_cast<int>(h)) { continue; }
                            std::uint8_t* p = rgba + (static_cast<std::size_t>(py) * w + static_cast<std::size_t>(px)) * 4;
                            p[0] = r; p[1] = g; p[2] = b; p[3] = a;
                        }
                    }
                }
            }
        }
        x += 6 * scale;
    }
}

int labelWidthPx(const std::string& text, int scale) { return static_cast<int>(text.size()) * 6 * std::max(1, scale); }

std::vector<std::pair<double, std::string>> frequencyLabels(double centreHz, double spanHz, std::uint32_t w) {
    std::vector<std::pair<double, std::string>> out;
    if (!(spanHz > 0) || w < 100) { return out; }
    const double maxLabels = std::max(1.0, std::floor(static_cast<double>(w) / 110.0));
    double step = 1.0;
    bool found = false;
    for (double decade = 1.0; decade <= 1e11 && !found; decade *= 10.0) {
        for (const double m : {1.0, 2.0, 5.0}) {
            if (spanHz / (decade * m) <= maxLabels) { step = decade * m; found = true; break; }
        }
    }
    const int decimals = step >= 1e6 ? 0 : step >= 1e5 ? 1 : step >= 1e4 ? 2 : step >= 1e3 ? 3 : step >= 100 ? 4 : 5;
    const double lo = centreHz - spanHz / 2.0;
    for (double f = std::ceil(lo / step) * step; f <= lo + spanHz + 1e-6; f += step) {
        out.emplace_back((f - lo) / spanHz * static_cast<double>(w), num(f / 1e6, decimals));
    }
    return out;
}

std::vector<std::pair<std::uint32_t, std::string>> timeLabels(std::uint32_t h, double secondsPerRow) {
    std::vector<std::pair<std::uint32_t, std::string>> out;
    if (!(secondsPerRow > 0) || h < 30) { return out; }
    static const double kSteps[] = {1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600};
    double step = kSteps[sizeof(kSteps) / sizeof(kSteps[0]) - 1];
    for (const double s : kSteps) { if (s / secondsPerRow >= 30.0) { step = s; break; } }
    for (double t = 0; t / secondsPerRow < static_cast<double>(h) - 8; t += step) {
        const std::uint32_t row = static_cast<std::uint32_t>(std::lround(t / secondsPerRow));
        std::string text = t == 0 ? "0 S" : t >= 60 ? "-" + num(t / 60.0, 0) + " M" : "-" + num(t, 0) + " S";
        out.emplace_back(row, text);
    }
    return out;
}

Image renderPicture(const PictureSpec& spec) {
    Image img;
    img.w = kMarginLeft + spec.w + 8;
    img.h = kMarginTop + spec.h + 8;
    const Pixel bg{0x10, 0x12, 0x16, 255}, ink{0xe8, 0xe4, 0xd8, 255}, faint{0x70, 0x72, 0x78, 255};
    img.px.assign(static_cast<std::size_t>(img.w) * img.h, bg);
    if (spec.picture != nullptr) {
        for (std::uint32_t y = 0; y < spec.h; ++y) {
            std::memcpy(&img.px[static_cast<std::size_t>(y + kMarginTop) * img.w + kMarginLeft], spec.picture + static_cast<std::size_t>(y) * spec.w, sizeof(Pixel) * spec.w);
        }
    }
    drawText(img, 8, 6, "RADIO " + std::to_string(spec.radio) + "  CENTRE " + num(spec.centreHz / 1e6, 6) + " MHZ  SPAN " + num(spec.spanHz / 1e6, 6) + " MHZ  " + captionUtc(spec.newestUtcMs) + " UTC", ink);
    for (const auto& [x, text] : frequencyLabels(spec.centreHz, spec.spanHz, spec.w)) {
        const int px = static_cast<int>(kMarginLeft) + static_cast<int>(std::lround(x));
        for (int y = static_cast<int>(kMarginTop) - 8; y < static_cast<int>(kMarginTop); ++y) { put(img, px, y, faint); }
        // (the label is centred on its tick and kept inside the picture: the last tick of a window is at the picture's right edge)
        drawText(img, std::clamp(px - textWidth(text) / 2, 0, std::max(0, static_cast<int>(img.w) - textWidth(text))), 26, text, ink);
    }
    for (const auto& [row, text] : timeLabels(spec.h, spec.secondsPerRow)) {
        const int py = static_cast<int>(kMarginTop) + static_cast<int>(row);
        for (int x = static_cast<int>(kMarginLeft) - 6; x < static_cast<int>(kMarginLeft); ++x) { put(img, x, py, faint); }
        drawText(img, static_cast<int>(kMarginLeft) - 8 - textWidth(text), py - 7, text, ink);
    }
    return img;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- PNG
namespace {

std::uint32_t crc32(const std::uint8_t* p, std::size_t n, std::uint32_t crc = 0) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) { c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1; }
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (std::size_t i = 0; i < n; ++i) { crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8); }
    return ~crc;
}

void be32(std::vector<std::uint8_t>& v, std::uint32_t x) { for (int s = 24; s >= 0; s -= 8) { v.push_back(static_cast<std::uint8_t>(x >> s)); } }

void chunk(std::vector<std::uint8_t>& png, const char* type, const std::vector<std::uint8_t>& data) {
    be32(png, static_cast<std::uint32_t>(data.size()));
    std::vector<std::uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    png.insert(png.end(), body.begin(), body.end());
    be32(png, crc32(body.data(), body.size()));
}

}  // namespace

std::vector<std::uint8_t> encodePng(const Image& img, const std::string& text) {
    std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    std::vector<std::uint8_t> ihdr;
    be32(ihdr, img.w);
    be32(ihdr, img.h);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});   // 8 bit, RGB, deflate, no filter method, no interlace
    chunk(png, "IHDR", ihdr);
    if (!text.empty()) {
        std::vector<std::uint8_t> t{'F', 'o', 'x', 'S', 'D', 'R', 0};
        t.insert(t.end(), text.begin(), text.end());
        chunk(png, "tEXt", t);
    }
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(img.h) * (img.w * 3 + 1));
    for (std::uint32_t y = 0; y < img.h; ++y) {
        raw.push_back(0);   // filter: none
        for (std::uint32_t x = 0; x < img.w; ++x) {
            const Pixel& p = img.px[static_cast<std::size_t>(y) * img.w + x];
            raw.push_back(p.r); raw.push_back(p.g); raw.push_back(p.b);
        }
    }
    std::vector<std::uint8_t> z{0x78, 0x01};
    std::uint32_t a = 1, b = 0;
    for (const std::uint8_t c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
    for (std::size_t at = 0; at < raw.size() || at == 0; ) {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - at);
        const bool last = at + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<std::uint8_t>(n & 0xff)); z.push_back(static_cast<std::uint8_t>(n >> 8));
        z.push_back(static_cast<std::uint8_t>(~n & 0xff)); z.push_back(static_cast<std::uint8_t>((~n >> 8) & 0xff));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(at), raw.begin() + static_cast<std::ptrdiff_t>(at + n));
        at += n;
        if (last) { break; }
    }
    be32(z, (b << 16) | a);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    return png;
}

std::string pngText(const std::vector<std::uint8_t>& png, const std::string& keyword) {
    std::size_t at = 8;
    while (at + 12 <= png.size()) {
        const std::uint32_t len = (static_cast<std::uint32_t>(png[at]) << 24) | (static_cast<std::uint32_t>(png[at + 1]) << 16) | (static_cast<std::uint32_t>(png[at + 2]) << 8) | png[at + 3];
        if (at + 12 + len > png.size()) { break; }
        if (std::memcmp(&png[at + 4], "tEXt", 4) == 0) {
            const std::string body(reinterpret_cast<const char*>(&png[at + 8]), len);
            const std::size_t nul = body.find('\0');
            if (nul != std::string::npos && body.substr(0, nul) == keyword) { return body.substr(nul + 1); }
        }
        at += 12 + len;
    }
    return std::string();
}

std::string withExtension(const std::string& path, const std::string& ext) {
    if (path.empty()) { return path; }
    const std::size_t slash = path.find_last_of("/\\");
    const std::size_t dot = path.find_last_of('.');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash) && dot + 1 < path.size()) { return path; }
    return path + (path.back() == '.' ? "" : ".") + ext;
}

std::string freePath(const std::string& path, const std::function<bool(const std::string&)>& exists) {
    if (!exists(path)) { return path; }
    const std::size_t slash = path.find_last_of("/\\");
    const std::size_t dot = path.find_last_of('.');
    const bool hasExt = dot != std::string::npos && (slash == std::string::npos || dot > slash);
    const std::string stem = hasExt ? path.substr(0, dot) : path, ext = hasExt ? path.substr(dot) : std::string();
    for (int i = 2; i < 1000; ++i) {
        const std::string candidate = stem + " (" + std::to_string(i) + ")" + ext;
        if (!exists(candidate)) { return candidate; }
    }
    return path;
}

namespace {

std::filesystem::path pathOf(const std::string& u8) { return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(u8.data()), u8.size())); }
std::string u8Of(const std::filesystem::path& p) { const std::u8string s = p.u8string(); return std::string(reinterpret_cast<const char*>(s.data()), s.size()); }
std::FILE* openForWrite(const std::filesystem::path& p) {
#ifdef _WIN32
    return _wfopen(p.c_str(), L"wb");
#else
    return std::fopen(p.c_str(), "wb");
#endif
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- writing
std::string resolvePath(const std::string& typed, const std::string& baseFolder, const std::string& ext) {
    std::string p = typed;
    const auto trim = [](std::string& s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) { s.erase(s.begin()); }
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) { s.pop_back(); }
    };
    trim(p);
    if (p.size() >= 2 && p.front() == '"' && p.back() == '"') { p = p.substr(1, p.size() - 2); trim(p); }   // Explorer's "Copy as path"
    if (p.empty()) { return p; }
    namespace fs = std::filesystem;
    fs::path path = pathOf(p);
    if (path.is_relative() && !baseFolder.empty()) { path = pathOf(baseFolder) / path; }
    return withExtension(u8Of(path), ext);
}

std::string pictureFacts(const PictureSpec& spec) {
    char b[256];
    std::snprintf(b, sizeof(b), "radio=%d;centre_hz=%.1f;span_hz=%.1f;newest_utc=%s;db_min=%.1f;db_max=%.1f;seconds_per_line=%.4f", spec.radio, spec.centreHz, spec.spanHz, isoUtc(spec.newestUtcMs).c_str(), spec.dbMin, spec.dbMax, spec.secondsPerRow);
    return b;
}

namespace {

Outcome writeAtomically(const std::string& wanted, const std::function<bool(std::FILE*, std::uint64_t&)>& fill) {
    namespace fs = std::filesystem;
    Outcome out;
    std::error_code ec;
    const fs::path want = pathOf(wanted);
    if (want.has_parent_path()) { fs::create_directories(want.parent_path(), ec); }
    const std::string path = freePath(wanted, [](const std::string& p) { std::error_code e; return fs::exists(pathOf(p), e); });
    const std::string part = path + ".part";
    std::FILE* f = openForWrite(pathOf(part));
    if (f == nullptr) { out.error = "the file could not be created"; out.why = 3; return out; }
    const bool wrote = fill(f, out.bytes);
    const bool closed = std::fclose(f) == 0;
    if (!wrote || !closed) { fs::remove(pathOf(part), ec); out.error = "the disk refused the data"; out.why = 3; out.bytes = 0; return out; }
    fs::rename(pathOf(part), pathOf(path), ec);
    if (ec) { fs::remove(pathOf(part), ec); out.error = "the file could not be put in place"; out.why = 3; out.bytes = 0; return out; }
    out.ok = true;
    out.path = path;
    return out;
}

}  // namespace

Outcome savePicture(const std::string& typedPath, const std::string& baseFolder, std::vector<Pixel> picture, PictureSpec spec) {
    const std::string path = resolvePath(typedPath, baseFolder, "png");
    if (path.empty()) { Outcome o; o.error = "no file name"; o.why = 1; return o; }
    spec.picture = picture.empty() ? nullptr : picture.data();
    if (picture.size() < static_cast<std::size_t>(spec.w) * spec.h) { Outcome o; o.error = "there is no picture"; o.why = 2; return o; }
    const std::vector<std::uint8_t> png = encodePng(renderPicture(spec), pictureFacts(spec));
    return writeAtomically(path, [&](std::FILE* f, std::uint64_t& bytes) { bytes = png.size(); return std::fwrite(png.data(), 1, png.size(), f) == png.size(); });
}

Outcome saveData(const std::string& typedPath, const std::string& baseFolder, Snapshot snapshot, int format, FileFacts facts) {
    const std::string path = resolvePath(typedPath, baseFolder, format == 1 ? "fsw" : "csv");
    if (path.empty()) { Outcome o; o.error = "no file name"; o.why = 1; return o; }
    if (snapshot.empty()) { Outcome o; o.error = "there is nothing to write"; o.why = 2; return o; }
    const std::size_t rows = snapshot.rows();
    Outcome o = writeAtomically(path, [&](std::FILE* f, std::uint64_t& bytes) {
        bool good = true;
        const Sink sink = [&](const void* p, std::size_t n) { if (good && std::fwrite(p, 1, n, f) != n) { good = false; } bytes += n; };
        if (format == 1) { writeBinary(snapshot, sink); } else { writeCsv(snapshot, facts, sink); }
        return good;
    });
    o.rows = o.ok ? rows : 0;
    return o;
}

Outcome saveText(const std::string& typedPath, const std::string& baseFolder, const std::string& text, const std::string& ext) {
    const std::string path = resolvePath(typedPath, baseFolder, ext);
    if (path.empty()) { Outcome o; o.error = "no file name"; o.why = 1; return o; }
    if (text.empty()) { Outcome o; o.error = "there is nothing to write"; o.why = 2; return o; }
    Outcome o = writeAtomically(path, [&](std::FILE* f, std::uint64_t& bytes) { bytes = text.size(); return std::fwrite(text.data(), 1, text.size(), f) == text.size(); });
    return o;
}

}  // namespace foxsdr::slintexport
