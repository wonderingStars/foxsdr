// Recording file names and file rollover, as pure logic (no engine, no recorder).
//
// STANDALONE. Nothing here is wired (docs/feature-records/recording-models.md, Part 3, has the hand-off). The present recorder
// (core/recorder.cpp makeFilename) names a take `<prefix>_<YYYYMMDD>_<HHMMSS>_<rate>Hz.wav` from the LOCAL time and reuses a name that already
// exists within the second; this layer replaces that with a template and never overwrites.
//
// NAME TEMPLATES
//   Tokens in braces: {date} YYYYMMDD, {time} HHMMSS, {datetime} YYYYMMDD_HHMMSS, {year} {month} {day} {hour} {min} {sec} {ms}, {tz} (Z for UTC,
//   else +HHMM), {freq} (whole Hz), {freq_khz} (kHz, up to 3 decimals, trailing zeros trimmed), {freq_mhz} (MHz, up to 6 decimals trimmed),
//   {mode}, {radio}, {bookmark}, {counter} or {counter:N} (zero padded to N digits, 1..9). `{{` and `}}` are literal braces. `/` and `\` separate
//   folders (at most three folders below the base; written back with `/`). The extension is added by the caller and is not part of the template.
//   Time is UTC or local: the caller passes the offset in minutes (looking up the zone is the clock's job), so this layer is pure.
//
// WHAT IS REFUSED (a template is refused as a whole with a message naming the problem, so a mistyped template never silently records somewhere odd)
//   Characters Windows forbids in a file name (< > : " | ? * and control characters 0..31); a leading separator or a drive / UNC form (empty first
//   component, or a colon); `.` and `..` components (path escapes); empty components (`a//b`); a component that ends in a dot or a space (Windows
//   strips them, so the name written is not the name asked for); a reserved device name as a whole component (CON PRN AUX NUL COM1..9 LPT1..9, with
//   or without an extension, any case); an unknown token; an unclosed or stray brace; a bad counter width; too many folders; a total relative
//   length over 200 bytes or a component over 120 bytes.
//   VALUES that come from the user at run time (a bookmark called `BBC R4 (LW)/test`, a radio called `a:b`) are not refused: their forbidden
//   characters become `_`, `.` / `..` become `_`, a reserved device name gets a `_` in front, trailing dots and spaces are trimmed, leading
//   spaces too, the value is cut at 64 bytes without splitting a UTF-8 sequence, and an empty value becomes `unknown`.
//
// ROLLOVER (Rollover): by size, by time, or both, and optionally on a wall-clock boundary (a new file at every :00 of the hour). The splitter works in
// FRAMES (complex or audio sample frames), so a hand-over is sample exact: the frames of one write call are cut into segments, each segment
// belongs to exactly one file, and the concatenation of the segments of all files is the input, no frame lost or repeated (the test proves it).
// For compressed output (FLAC) the size limit applies to the RAW size (bytesPerFrame), an upper bound.
//
// ONE FILE PER TRANSMISSION is not here: core/clip_recorder.hpp (ClipTrigger) already turns squelch open / close into clips with hang time, a
// minimum length and a maximum clip length; the wiring lane can name its clips with expand() below.
//
// THREADS: all functions are pure or touch only their own object; Rollover is for one thread.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "core/civil_time.hpp"

namespace cascade::core::recname {

struct Fields {
    std::int64_t unixMs = 0;        // the moment the file is opened, UTC
    int utcOffsetMinutes = 0;       // used when the template is expanded in local time
    double frequencyHz = 0.0;
    std::string mode, radio, bookmark;
    std::uint64_t counter = 0;
};

struct Expanded {
    bool ok = false;
    std::string relPath;            // forward slashes, no extension
    std::string error;
    bool valueSanitised = false;    // a run-time value had characters replaced
};

inline constexpr int kMaxFolders = 3;
inline constexpr std::size_t kMaxRelBytes = 200;
inline constexpr std::size_t kMaxComponentBytes = 120;
inline constexpr std::size_t kMaxValueBytes = 64;
inline constexpr std::size_t kMaxFullPathBytes = 240;   // MAX_PATH (260) minus a margin for the \\?\ -less limit and a counter suffix

namespace detail {

inline bool isForbiddenChar(unsigned char c) {
    return c < 32 || c == 127 || c == '<' || c == '>' || c == ':' || c == '"' || c == '|' || c == '?' || c == '*';
}

inline std::string upperAscii(std::string s) {
    for (char& c : s) { if (c >= 'a' && c <= 'z') { c = static_cast<char>(c - 'a' + 'A'); } }
    return s;
}

// A reserved DOS device name, with or without an extension ("nul", "NUL.txt", "com3").
inline bool isReservedDeviceName(const std::string& comp) {
    std::string stem = comp.substr(0, comp.find('.'));
    while (!stem.empty() && stem.back() == ' ') { stem.pop_back(); }
    const std::string u = upperAscii(stem);
    if (u == "CON" || u == "PRN" || u == "AUX" || u == "NUL") { return true; }
    if (u.size() == 4 && (u.compare(0, 3, "COM") == 0 || u.compare(0, 3, "LPT") == 0) && u[3] >= '0' && u[3] <= '9') { return true; }
    return false;
}

inline std::string sanitiseValue(const std::string& in, bool* changed) {
    std::string s;
    for (unsigned char c : in) {
        if (isForbiddenChar(c) || c == '/' || c == '\\') { s.push_back('_'); *changed = true; } else { s.push_back(static_cast<char>(c)); }
    }
    std::size_t b = 0;
    while (b < s.size() && s[b] == ' ') { ++b; }
    if (b > 0) { *changed = true; }
    s.erase(0, b);
    while (!s.empty() && (s.back() == '.' || s.back() == ' ')) { s.pop_back(); *changed = true; }
    if (s.size() > kMaxValueBytes) {
        std::size_t cut = kMaxValueBytes;
        while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) { --cut; }   // never inside a UTF-8 sequence
        s.resize(cut);
        *changed = true;
    }
    if (s.empty()) { *changed = true; return "unknown"; }
    if (s == "." || s == "..") { *changed = true; return "_"; }
    if (isReservedDeviceName(s)) { *changed = true; return "_" + s; }
    return s;
}

inline std::string trimmedNumber(double v, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') { s.pop_back(); }
        if (!s.empty() && s.back() == '.') { s.pop_back(); }
    }
    return s;
}

inline bool checkComponent(const std::string& c, std::string* err) {
    if (c.empty()) { *err = "empty folder or file name part (a doubled or leading separator)"; return false; }
    if (c == "." || c == "..") { *err = "'.' and '..' are not allowed (path escape)"; return false; }
    if (c.back() == '.' || c.back() == ' ') { *err = "a name part may not end in a dot or a space"; return false; }
    if (c.size() > kMaxComponentBytes) { *err = "a name part is longer than 120 bytes"; return false; }
    if (isReservedDeviceName(c)) { *err = "'" + c + "' is a reserved Windows device name"; return false; }
    return true;
}

}  // namespace detail

// Expands `tmpl` for `f`. `utc` chooses the clock the date and time tokens read (local = unixMs + utcOffsetMinutes).
inline Expanded expand(const std::string& tmpl, const Fields& f, bool utc = true) {
    Expanded r;
    const int off = utc ? 0 : f.utcOffsetMinutes;
    const civil::DateTime t = civil::fromUnixMs(f.unixMs, off);
    bool changed = false;
    std::string out;
    auto fail = [&](const std::string& m) { r.ok = false; r.error = m; r.relPath.clear(); return r; };
    if (tmpl.empty()) { return fail("the template is empty"); }
    for (std::size_t i = 0; i < tmpl.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(tmpl[i]);
        if (c == '{') {
            if (i + 1 < tmpl.size() && tmpl[i + 1] == '{') { out.push_back('{'); ++i; continue; }
            const std::size_t close = tmpl.find('}', i + 1);
            if (close == std::string::npos) { return fail("unclosed '{' in the template"); }
            const std::string tok = tmpl.substr(i + 1, close - i - 1);
            i = close;
            std::string name = tok, arg;
            const std::size_t colon = tok.find(':');
            if (colon != std::string::npos) { name = tok.substr(0, colon); arg = tok.substr(colon + 1); }
            if (name != "counter" && colon != std::string::npos) { return fail("token {" + name + "} takes no argument"); }
            std::string v;
            if (name == "date") { v = civil::pad(t.year, 4) + civil::pad(t.month, 2) + civil::pad(t.day, 2); }
            else if (name == "time") { v = civil::pad(t.hour, 2) + civil::pad(t.minute, 2) + civil::pad(t.second, 2); }
            else if (name == "datetime") { v = civil::pad(t.year, 4) + civil::pad(t.month, 2) + civil::pad(t.day, 2) + "_" + civil::pad(t.hour, 2) + civil::pad(t.minute, 2) + civil::pad(t.second, 2); }
            else if (name == "year") { v = civil::pad(t.year, 4); }
            else if (name == "month") { v = civil::pad(t.month, 2); }
            else if (name == "day") { v = civil::pad(t.day, 2); }
            else if (name == "hour") { v = civil::pad(t.hour, 2); }
            else if (name == "min") { v = civil::pad(t.minute, 2); }
            else if (name == "sec") { v = civil::pad(t.second, 2); }
            else if (name == "ms") { v = civil::pad(t.millis, 3); }
            else if (name == "tz") { v = civil::offsetText(off, true); }
            else if (name == "freq") { char b[40]; std::snprintf(b, sizeof(b), "%.0f", std::max(0.0, std::floor(f.frequencyHz + 0.5))); v = b; }
            else if (name == "freq_khz") { v = detail::trimmedNumber(std::max(0.0, f.frequencyHz) / 1e3, 3); }
            else if (name == "freq_mhz") { v = detail::trimmedNumber(std::max(0.0, f.frequencyHz) / 1e6, 6); }
            else if (name == "mode") { v = detail::sanitiseValue(f.mode, &changed); }
            else if (name == "radio") { v = detail::sanitiseValue(f.radio, &changed); }
            else if (name == "bookmark") { v = detail::sanitiseValue(f.bookmark, &changed); }
            else if (name == "counter") {
                int width = 1;
                if (colon != std::string::npos) {
                    if (arg.size() != 1 || arg[0] < '1' || arg[0] > '9') { return fail("{counter:N} needs a width of 1 to 9"); }
                    width = arg[0] - '0';
                }
                char b[40];
                std::snprintf(b, sizeof(b), "%0*llu", width, static_cast<unsigned long long>(f.counter));
                v = b;
            } else { return fail("unknown token {" + name + "}"); }
            out += v;
            continue;
        }
        if (c == '}') {
            if (i + 1 < tmpl.size() && tmpl[i + 1] == '}') { out.push_back('}'); ++i; continue; }
            return fail("stray '}' in the template");
        }
        if (c == '/' || c == '\\') { out.push_back('/'); continue; }
        if (detail::isForbiddenChar(c)) {
            if (c < 32 || c == 127) { return fail("a control character in the template"); }
            return fail(std::string("the character '") + static_cast<char>(c) + "' is not allowed in a file name on Windows");
        }
        out.push_back(static_cast<char>(c));
    }
    // split and check every component of the RESULT (a token cannot smuggle a separator: values were sanitised, the rest are digits)
    std::vector<std::string> parts;
    std::size_t from = 0;
    for (;;) {
        const std::size_t p = out.find('/', from);
        parts.push_back(out.substr(from, p == std::string::npos ? std::string::npos : p - from));
        if (p == std::string::npos) { break; }
        from = p + 1;
    }
    if (static_cast<int>(parts.size()) > kMaxFolders + 1) { return fail("more than three folders in the template"); }
    for (const std::string& p : parts) {
        std::string e;
        if (!detail::checkComponent(p, &e)) { return fail(e); }
    }
    if (out.size() > kMaxRelBytes) { return fail("the name is longer than 200 bytes"); }
    r.ok = true;
    r.relPath = out;
    r.valueSanitised = changed;
    return r;
}

// Joins base + relPath + "." + ext and proves the result stays inside `base`. Empty string and *err on refusal.
inline std::string resolvePath(const std::string& baseDir, const std::string& relPath, const std::string& ext, std::string* err) {
    namespace fs = std::filesystem;
    auto fail = [&](const std::string& m) { if (err != nullptr) { *err = m; } return std::string(); };
    if (ext.empty() || ext.size() > 8) { return fail("the extension must be 1 to 8 letters or digits"); }
    for (char c : ext) { if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) { return fail("the extension must be letters and digits only"); } }
    if (baseDir.empty()) { return fail("no recordings folder set"); }
    const fs::path base = fs::path(baseDir).lexically_normal();
    const fs::path full = (base / fs::path(relPath + "." + ext)).lexically_normal();
    // lexical containment: every element of base must be the leading elements of full, and full must be deeper
    auto bi = base.begin();
    auto fi = full.begin();
    for (; bi != base.end(); ++bi, ++fi) {
        if (bi->empty()) { continue; }
        if (fi == full.end() || *fi != *bi) { return fail("the name leaves the recordings folder"); }
    }
    for (const auto& el : full) {
        const std::string s = el.string();
        if (s == "..") { return fail("the name leaves the recordings folder"); }
    }
    const std::string s = full.string();
    if (s.size() > kMaxFullPathBytes) { return fail("the full path is longer than 240 characters"); }
    return s;
}

// A path that does not exist yet: `path` itself, else name-2.ext, name-3.ext ... up to -9999. Empty when all are taken. (The check can race
// with another writer: create the file with createExclusive() and retry on failure.)
inline std::string uniquePath(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(path, ec)) { return path; }
    const fs::path p(path);
    const std::string stem = (p.parent_path() / p.stem()).string();
    const std::string ext = p.extension().string();
    for (int n = 2; n <= 9999; ++n) {
        const std::string cand = stem + "-" + std::to_string(n) + ext;
        if (!fs::exists(cand, ec)) { return cand; }
    }
    return {};
}

// Creates the file only if it does not exist (fails when it does), making every missing folder first. True when this call created it.
inline bool createExclusive(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::FILE* f = nullptr;
#if defined(_WIN32)
    f = _wfopen(fs::path(path).c_str(), L"wbx");
#else
    f = std::fopen(path.c_str(), "wbx");
#endif
    if (f == nullptr) { return false; }
    std::fclose(f);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// Rollover.
// ---------------------------------------------------------------------------------------------------------------------------------------------
struct RolloverPolicy {
    std::uint64_t maxBytes = 0;            // 0 = no size limit; counts headerBytes + frames * bytesPerFrame
    double maxSeconds = 0.0;               // 0 = no time limit; counted in FRAMES at sampleRate
    bool alignToClock = false;             // time limit on a wall-clock boundary: file 1 ends at the next multiple of maxSeconds since the Unix epoch (UTC)
    std::uint64_t headerBytes = 0;         // the container's header (44 for WAV)
    std::uint32_t bytesPerFrame = 4;
    double sampleRate = 48000.0;
    std::int64_t startUnixMs = 0;          // when the first frame is written (for alignToClock)
};

struct Segment {
    std::uint64_t fileIndex = 0;           // 0 for the first file
    std::size_t offset = 0;                // into the frames passed to feed(), counted from the start of THAT call
    std::size_t frames = 0;
    bool startsFile = false;               // the caller opens a new file before writing this segment
    bool closesFile = false;               // the caller closes the file after writing this segment
};

// The plain-WAV ceiling the current recorder works to: the 32-bit RIFF size field with some slack.
inline constexpr std::uint64_t kWavMaxBytes = 0xFFFFFFFFull - 8192;

class Rollover {
public:
    // False with *err when the policy cannot hold even one frame (size limit under header + one frame, or a limit that rounds to 0 frames).
    bool configure(const RolloverPolicy& p, std::string* err) {
        auto fail = [&](const std::string& m) { if (err != nullptr) { *err = m; } return false; };
        if (p.bytesPerFrame == 0) { return fail("bytes per frame is zero"); }
        if (!(p.sampleRate > 0.0) || !std::isfinite(p.sampleRate)) { return fail("sample rate must be positive"); }
        if (!(p.maxSeconds >= 0.0) || !std::isfinite(p.maxSeconds)) { return fail("the time limit must be zero or positive"); }
        if (p.maxBytes != 0 && p.maxBytes < p.headerBytes + p.bytesPerFrame) { return fail("the size limit is smaller than one frame plus the header"); }
        if (p.maxSeconds != 0.0 && std::floor(p.maxSeconds * p.sampleRate) < 1.0) { return fail("the time limit is shorter than one frame"); }
        if (p.alignToClock && p.maxSeconds == 0.0) { return fail("clock alignment needs a time limit"); }
        pol_ = p;
        sizeCap_ = p.maxBytes == 0 ? ~std::uint64_t{0} : (p.maxBytes - p.headerBytes) / p.bytesPerFrame;
        periodFrames_ = p.maxSeconds == 0.0 ? ~std::uint64_t{0} : static_cast<std::uint64_t>(std::floor(p.maxSeconds * p.sampleRate + 0.5));
        index_ = 0; inFile_ = 0; fileFrames_ = firstTimeCap(); started_ = false;
        configured_ = true;
        return true;
    }

    // Cuts `frames` new frames into segments (appended to *out, at most maxSegments of them). Returns the frames consumed (== frames unless the
    // segment cap was reached). A segment of a file that is exactly full has closesFile set; the next call or segment starts the next file.
    std::size_t feed(std::size_t frames, std::vector<Segment>* out, std::size_t maxSegments = 1024) {
        std::size_t used = 0;
        if (!configured_) { return 0; }
        while (used < frames && out->size() < maxSegments) {
            Segment s;
            s.fileIndex = index_;
            s.offset = used;
            s.startsFile = !started_;
            started_ = true;
            const std::uint64_t room = fileFrames_ - inFile_;
            const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(room, frames - used));
            s.frames = take;
            inFile_ += take;
            used += take;
            if (inFile_ >= fileFrames_) {
                s.closesFile = true;
                ++index_; inFile_ = 0; started_ = false;
                fileFrames_ = std::min(sizeCap_, periodFrames_);   // every later file is a full one
            }
            out->push_back(s);
        }
        return used;
    }

    // Ends the current file now (a stop). The next feed() starts a new file with a fresh counter.
    void endFile() { if (started_) { ++index_; } inFile_ = 0; started_ = false; fileFrames_ = std::min(sizeCap_, periodFrames_); }

    std::uint64_t fileIndex() const { return index_; }
    std::uint64_t framesInCurrentFile() const { return inFile_; }
    std::uint64_t currentFileCapacityFrames() const { return fileFrames_; }

private:
    std::uint64_t firstTimeCap() const {
        std::uint64_t cap = std::min(sizeCap_, periodFrames_);
        if (pol_.alignToClock && pol_.maxSeconds > 0.0) {
            const std::int64_t periodMs = static_cast<std::int64_t>(std::floor(pol_.maxSeconds * 1000.0 + 0.5));
            if (periodMs > 0) {
                const std::int64_t intoPeriod = civil::floorMod(pol_.startUnixMs, periodMs);
                const std::int64_t leftMs = periodMs - intoPeriod;   // 1 .. periodMs
                const std::uint64_t frames = static_cast<std::uint64_t>(std::floor(static_cast<double>(leftMs) * pol_.sampleRate / 1000.0 + 0.5));
                cap = std::min(sizeCap_, std::max<std::uint64_t>(1, frames));
            }
        }
        return cap;
    }

    RolloverPolicy pol_;
    std::uint64_t sizeCap_ = ~std::uint64_t{0}, periodFrames_ = ~std::uint64_t{0}, fileFrames_ = ~std::uint64_t{0};
    std::uint64_t index_ = 0, inFile_ = 0;
    bool started_ = false, configured_ = false;
};

}  // namespace cascade::core::recname
