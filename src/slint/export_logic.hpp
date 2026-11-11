// export_logic.hpp - THE WATERFALL AS A FILE (docs/multi-radio-records/screen-s1.md, lane S1 item 3; design section 13): the picture as a PNG with a frequency and a time scale, and the data as a CSV
// or a compact "FoxSDR spectrum" file, from a host-side store of the levels the window has been given.
//
// THE STORE (SpectrumStore, one per radio) keeps one row of levels every 250 ms (the frame rate is 30 a second; a row a quarter second is what a picture or a table of a minutes-long capture needs), for
// up to an hour, as ONE BYTE per column: dBFS in steps of 0.6 dB from -150 dBFS (255 steps: -150 .. +3 dBFS), so a column of 1024 is a kilobyte and a radio's hour is 14.4 MB. A row has at most 1024 columns:
// a frame with more bins is reduced by taking the strongest bin of each group (the same rule the 3D surface uses). A retune, a zoom of the engine's span or a change of the number of columns starts a
// new SEGMENT (its own centre, span, column count and start time): rows of different windows are never mixed in one table. Only the host's memory is used; nothing here touches the engine.
//
// THE FILES. CSV: `#` header lines (what, radio, how to read the columns), then per segment `# segment` lines with the centre, span and column count, then `utc,db0,db1,...` rows (UTC as
// ISO-8601 with milliseconds, levels in dBFS to one decimal). Column i of a segment is at centre - span/2 + (i + 0.5) * span / columns. FOXSDR SPECTRUM (.fsw): little-endian, magic "FXSP", u32 version 1, u32
// segment count, then per segment u32 columns, f64 centreHz, f64 spanHz, f32 floorDb, f32 stepDb, i64 startUtcMs, u32 rows, then per row u32 milliseconds since the segment's start and `columns` bytes (level
// = floorDb + stepDb * byte). The picture: the waterfall as it is drawn (the colours the window shows), a frequency scale above it, a time scale at its side, a caption with the centre, the span and the UTC
// time, and the same facts in a PNG text chunk ("FoxSDR").
//
// WRITING FILES IS NOT HERE: this file builds the bytes (a sink the caller gives, so a worker thread can stream a large table); the host writes them from a core::DiskJob, off the frame.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace foxsdr::slintexport {

constexpr int kMaxColumns = 1024;
constexpr std::int64_t kRowGapMs = 250;
constexpr std::int64_t kKeepMs = 60LL * 60 * 1000;
constexpr float kFloorDb = -150.0f;
constexpr float kStepDb = 0.6f;

std::uint8_t quantise(float db);
float dequantise(std::uint8_t q);

// One stretch of rows of the same window.
struct Segment {
    double centreHz = 0, spanHz = 0;
    std::uint32_t columns = 0;
    std::int64_t startMs = 0;               // UTC, milliseconds
    std::vector<std::uint32_t> dtMs;        // per row: milliseconds since startMs
    std::vector<std::uint8_t> levels;       // rows x columns
    std::size_t rows() const { return dtMs.size(); }
};

// A copy of the newest rows, for the worker to own.
struct Snapshot {
    std::vector<Segment> segments;
    std::size_t rows() const { std::size_t n = 0; for (const Segment& s : segments) { n += s.rows(); } return n; }
    bool empty() const { return rows() == 0; }
};

class SpectrumStore {
public:
    // Takes one frame (dBFS per bin). Returns true when a row was stored (the rate is one in kRowGapMs). `utcMs` is the wall clock, `n` the bin count (> 0).
    bool push(std::int64_t utcMs, double centreHz, double spanHz, const float* bins, std::size_t n);
    // The rows of the last `minutes` (1 .. 60) before `nowMs`, oldest first.
    Snapshot snapshot(int minutes, std::int64_t nowMs) const;
    std::size_t rows() const;
    std::size_t segments() const { return segments_.size(); }
    std::size_t bytes() const;
    void clear() { segments_.clear(); lastRowMs_ = -1; }

private:
    std::vector<Segment> segments_;
    std::int64_t lastRowMs_ = -1;
};

// ---- the files ----
using Sink = std::function<void(const void*, std::size_t)>;
struct FileFacts {
    int radio = 1;                 // the radio's number
    std::string note;              // a free line for the header (the host writes the program version here)
};
void writeCsv(const Snapshot& s, const FileFacts& facts, const Sink& sink);
void writeBinary(const Snapshot& s, const Sink& sink);
std::optional<Snapshot> readBinary(const std::vector<std::uint8_t>& bytes);
// The UTC time of a millisecond count as "2026-11-11T12:34:56.250Z", and "2026-11-11 12:34:56" for a caption.
std::string isoUtc(std::int64_t ms);
std::string captionUtc(std::int64_t ms);

// ---- the picture ----
struct Pixel { std::uint8_t r = 0, g = 0, b = 0, a = 255; };
struct Image {
    std::uint32_t w = 0, h = 0;
    std::vector<Pixel> px;
};
struct PictureSpec {
    const Pixel* picture = nullptr;   // the waterfall's canonical picture, row 0 = the newest line
    std::uint32_t w = 0, h = 0;       // its size
    double centreHz = 0, spanHz = 0;  // the window it shows
    double secondsPerRow = 0;         // 0 = unknown (no time scale numbers)
    std::int64_t newestUtcMs = 0;
    int radio = 1;
    double dbMin = -110, dbMax = 0;
};
// The picture with a margin: a caption and the frequency scale above, the time scale at the left. The picture's own pixels are copied unchanged.
Image renderPicture(const PictureSpec& spec);
constexpr std::uint32_t kMarginLeft = 72, kMarginTop = 54;
// "0 S", "-2 S", "-1 M" at the lines of the time scale, and the lines' rows (row, text): the rule the picture uses, public so a test can hold it.
std::vector<std::pair<std::uint32_t, std::string>> timeLabels(std::uint32_t h, double secondsPerRow);
// The frequency scale: (x in 0..w, text in MHz) at a round step.
std::vector<std::pair<double, std::string>> frequencyLabels(double centreHz, double spanHz, std::uint32_t w);

// PNG bytes of an image (RGB, stored deflate blocks: no compression, nothing to install), with a text chunk "FoxSDR" holding `text` (Latin-1).
std::vector<std::uint8_t> encodePng(const Image& img, const std::string& text);
// The text chunk's value read back from PNG bytes ("" when none); for the tests and for a check of an exported file.
std::string pngText(const std::vector<std::uint8_t>& png, const std::string& keyword);
// A path with a missing extension completed ("" stays ""), and one that does not collide: "a.png" -> "a (2).png" when `exists(path)`.
std::string withExtension(const std::string& path, const std::string& ext);
std::string freePath(const std::string& path, const std::function<bool(const std::string&)>& exists);

// ---- writing (what the worker does; the host runs these from a core::DiskJob, never on a frame) ----
struct Outcome {
    bool ok = false;
    std::string path;           // the file that was written (the name may have been made free: "a (2).png")
    std::string error;          // English, one short sentence, when !ok (the host puts it in its own words around the path)
    int why = 0;                // 0 = ok, 1 = no file name, 2 = nothing to write, 3 = the disk or the folder refused (the host words each in the interface language)
    std::uint64_t bytes = 0;
    std::size_t rows = 0;       // data files: the rows written
};
// The path the user typed made usable: quotes and spaces off the ends, a relative one put under `baseFolder`, the extension added when it has none. "" when nothing usable was typed.
std::string resolvePath(const std::string& typed, const std::string& baseFolder, const std::string& ext);
// The picture: the waterfall as drawn with its scales, as a PNG with the FoxSDR text chunk. The file is written under a ".part" name and renamed, so a half file never has the final name; an existing
// file is never overwritten (freePath).
Outcome savePicture(const std::string& typedPath, const std::string& baseFolder, std::vector<Pixel> picture, PictureSpec spec);
// The data: format 0 CSV (.csv), 1 FoxSDR spectrum (.fsw). Streamed, so a long table never sits in memory twice.
Outcome saveData(const std::string& typedPath, const std::string& baseFolder, Snapshot snapshot, int format, FileFacts facts);
// The text of the picture's PNG chunk: radio, centre, span, the newest line's UTC time, the display range, seconds a line.
std::string pictureFacts(const PictureSpec& spec);

}  // namespace foxsdr::slintexport
