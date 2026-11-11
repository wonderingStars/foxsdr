// Analyser project: a saved analysis of ONE recording - a crop (time range and frequency range), tags (labelled time-and-frequency boxes with a note),
// notes, and the linked cursors (one time and one frequency shared by the waterfall, the spectrum and the timeline, so a click in one moves the others).
//
// STANDALONE. Nothing here is wired (docs/feature-records/recplay.md). It holds the data and the click-to-sync rules; the screens read state() and push it
// to their views.
//
// FILE FORMAT (versioned JSON, UTF-8): {"format":"foxsdr-analyser-project","version":1,"recording":{"path","sha256","sizeBytes","sampleRateHz","centerHz",
// "durationSec"},"crop":{"startSec","endSec","lowHz","highHz"},"cursors":{"timeSec","freqHz"},"tags":[{"id","startSec","endSec","lowHz","highHz","label",
// "note"}],"nextTagId","notes"}. The recording is referenced by path AND sha256 (and size): verifyReference() says whether a file found at the path is the
// recording the project was made on. A reader REFUSES a file whose version is not 1 (a newer program's file is never half-read), whose "format" differs, or
// that breaks any rule in validate(): finite numbers, time and frequency pairs ordered, boxes inside the recording, text limits, at most 4096 tags,
// unique tag ids. Unknown keys inside a version-1 file are ignored. Input is capped at 4 MiB and 64 levels of nesting before parsing.
//
// PRIVACY: tags, notes and labels are user text that lives only in this file. This header never prints or logs; its error messages name the field and
// never repeat the user's text or the path.
//
// SHA-256 is implemented here (FIPS 180-4) so the header needs nothing else; the test checks it against the published vectors and the Windows CNG.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace cascade::core::analyser {

constexpr int kVersion = 1;
constexpr const char* kFormat = "foxsdr-analyser-project";
constexpr std::size_t kMaxFileBytes = 4u * 1024u * 1024u;
constexpr std::size_t kMaxTags = 4096;
constexpr std::size_t kMaxLabelBytes = 64;
constexpr std::size_t kMaxNoteBytes = 2000;
constexpr std::size_t kMaxNotesBytes = 20000;
constexpr std::size_t kMaxPathBytes = 1024;
constexpr double kMaxFreqHz = 1.0e12;
constexpr double kMaxSeconds = 1.0e9;

// ---- SHA-256 -------------------------------------------------------------------------------------------------------------------------------

class Sha256 {
public:
    Sha256() { reset(); }
    void reset() {
        static const std::uint32_t init[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
        std::memcpy(h_, init, sizeof h_);
        bits_ = 0; fill_ = 0;
    }
    void update(const void* data, std::size_t n) {
        const unsigned char* p = static_cast<const unsigned char*>(data);
        bits_ += static_cast<std::uint64_t>(n) * 8u;
        while (n > 0) {
            const std::size_t take = std::min<std::size_t>(n, 64 - fill_);
            std::memcpy(buf_ + fill_, p, take);
            fill_ += take; p += take; n -= take;
            if (fill_ == 64) { block(buf_); fill_ = 0; }
        }
    }
    std::string finishHex() {
        const std::uint64_t bits = bits_;
        unsigned char pad[72];
        std::size_t padLen = (fill_ < 56) ? (56 - fill_) : (120 - fill_);
        std::memset(pad, 0, sizeof pad);
        pad[0] = 0x80;
        for (int i = 0; i < 8; ++i) { pad[padLen + static_cast<std::size_t>(i)] = static_cast<unsigned char>(bits >> (56 - 8 * i)); }
        update(pad, padLen + 8);
        static const char* hex = "0123456789abcdef";
        std::string out;
        for (int i = 0; i < 8; ++i) {
            for (int s = 28; s >= 0; s -= 4) { out += hex[(h_[i] >> s) & 0xF]; }
        }
        reset();
        return out;
    }

private:
    static std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const unsigned char* p) {
        static const std::uint32_t k[64] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u,
            0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du,
            0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu,
            0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
            0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(p[4 * i]) << 24) | (static_cast<std::uint32_t>(p[4 * i + 1]) << 16) | (static_cast<std::uint32_t>(p[4 * i + 2]) << 8) | p[4 * i + 3];
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], hh = h_[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t t1 = hh + S1 + ch + k[i] + w[i];
            const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = S0 + mj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d; h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += hh;
    }
    std::uint32_t h_[8];
    unsigned char buf_[64];
    std::size_t fill_ = 0;
    std::uint64_t bits_ = 0;
};

inline std::string sha256Hex(const void* data, std::size_t n) { Sha256 s; s.update(data, n); return s.finishHex(); }

// Hashes a file in 1 MiB reads. `cancel` (optional) is checked between reads; `progress` (optional) gets bytes done. False with `err` on failure or cancel.
inline bool sha256File(const std::filesystem::path& path, std::string* hex, std::uint64_t* bytes, std::string* err,
                       const std::atomic<bool>* cancel = nullptr, const std::function<void(std::uint64_t)>& progress = nullptr) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { if (err != nullptr) { *err = "cannot open the recording"; } return false; }
    Sha256 s;
    std::vector<char> buf(1u << 20);
    std::uint64_t total = 0;
    while (f) {
        if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) { if (err != nullptr) { *err = "cancelled"; } return false; }
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize got = f.gcount();
        if (got <= 0) { break; }
        s.update(buf.data(), static_cast<std::size_t>(got));
        total += static_cast<std::uint64_t>(got);
        if (progress) { progress(total); }
    }
    if (f.bad()) { if (err != nullptr) { *err = "read error"; } return false; }
    if (hex != nullptr) { *hex = s.finishHex(); }
    if (bytes != nullptr) { *bytes = total; }
    return true;
}

// ---- the model -----------------------------------------------------------------------------------------------------------------------------

struct Reference {
    std::string path;                    // as the user's disk had it when the project was saved
    std::string sha256;                  // 64 lower-case hex characters; empty = not yet hashed
    std::uint64_t sizeBytes = 0;
    double sampleRateHz = 0.0;
    double centerHz = 0.0;
    double durationSec = 0.0;
};

struct Crop {
    bool hasTime = false;
    double startSec = 0.0, endSec = 0.0;
    bool hasFreq = false;
    double lowHz = 0.0, highHz = 0.0;
};

struct Tag {
    std::uint32_t id = 0;
    double startSec = 0.0, endSec = 0.0;
    double lowHz = 0.0, highHz = 0.0;
    std::string label;
    std::string note;
};

struct Cursors {
    bool hasTime = false;
    double timeSec = 0.0;
    bool hasFreq = false;
    double freqHz = 0.0;
};

struct Project {
    int version = kVersion;
    Reference recording;
    Crop crop;
    Cursors cursors;
    std::vector<Tag> tags;
    std::uint32_t nextTagId = 1;
    std::string notes;
};

namespace detail {

inline bool fin(double v) { return std::isfinite(v); }

inline bool validUtf8(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t n = 0;
        std::uint32_t cp = 0;
        if (c < 0x80) { ++i; continue; }
        if (c >= 0xC2 && c <= 0xDF) { n = 1; cp = c & 0x1Fu; }
        else if (c >= 0xE0 && c <= 0xEF) { n = 2; cp = c & 0x0Fu; }
        else if (c >= 0xF0 && c <= 0xF4) { n = 3; cp = c & 0x07u; }
        else { return false; }
        if (i + n >= s.size()) { return false; }
        for (std::size_t k = 1; k <= n; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) { return false; }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if ((n == 2 && cp < 0x800) || (n == 3 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF)) { return false; }
        i += n + 1;
    }
    return true;
}

// control characters other than tab / newline / carriage return are refused in user text
inline bool cleanText(const std::string& s, std::size_t maxBytes, bool allowNewlines) {
    if (s.size() > maxBytes || !validUtf8(s)) { return false; }
    for (char ch : s) {
        const unsigned char u = static_cast<unsigned char>(ch);
        if (u == 0x7F) { return false; }
        if (u < 0x20 && !(allowNewlines && (u == '\n' || u == '\r' || u == '\t'))) { return false; }
    }
    return true;
}

inline bool isHex64(const std::string& s) {
    if (s.size() != 64) { return false; }
    for (char c : s) { if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { return false; } }
    return true;
}

}  // namespace detail

// All rules of a valid project, in one place. `err` names the field (never user text).
inline bool validate(const Project& p, std::string* err) {
    auto fail = [&](const char* m) { if (err != nullptr) { *err = m; } return false; };
    if (p.version != kVersion) { return fail("version"); }
    const Reference& r = p.recording;
    if (r.path.empty() || !detail::cleanText(r.path, kMaxPathBytes, false)) { return fail("recording.path"); }
    if (!r.sha256.empty() && !detail::isHex64(r.sha256)) { return fail("recording.sha256"); }
    if (!detail::fin(r.sampleRateHz) || r.sampleRateHz < 0.0 || r.sampleRateHz > 1.0e10) { return fail("recording.sampleRateHz"); }
    if (!detail::fin(r.centerHz) || std::fabs(r.centerHz) > kMaxFreqHz) { return fail("recording.centerHz"); }
    if (!detail::fin(r.durationSec) || r.durationSec < 0.0 || r.durationSec > kMaxSeconds) { return fail("recording.durationSec"); }
    const bool knownDuration = r.durationSec > 0.0;
    const bool knownBand = r.sampleRateHz > 0.0;
    const double bandLo = r.centerHz - r.sampleRateHz / 2.0, bandHi = r.centerHz + r.sampleRateHz / 2.0;
    auto timeOk = [&](double t) { return detail::fin(t) && t >= 0.0 && t <= kMaxSeconds && (!knownDuration || t <= r.durationSec); };
    auto freqOk = [&](double f) { return detail::fin(f) && std::fabs(f) <= kMaxFreqHz && (!knownBand || (f >= bandLo && f <= bandHi)); };
    if (p.crop.hasTime && !(timeOk(p.crop.startSec) && timeOk(p.crop.endSec) && p.crop.startSec < p.crop.endSec)) { return fail("crop.time"); }
    if (p.crop.hasFreq && !(freqOk(p.crop.lowHz) && freqOk(p.crop.highHz) && p.crop.lowHz < p.crop.highHz)) { return fail("crop.freq"); }
    if (p.cursors.hasTime && !timeOk(p.cursors.timeSec)) { return fail("cursors.timeSec"); }
    if (p.cursors.hasFreq && !freqOk(p.cursors.freqHz)) { return fail("cursors.freqHz"); }
    if (p.tags.size() > kMaxTags) { return fail("tags (too many)"); }
    std::set<std::uint32_t> ids;
    for (const Tag& t : p.tags) {
        if (t.id == 0 || !ids.insert(t.id).second) { return fail("tags.id"); }
        if (t.id >= p.nextTagId) { return fail("nextTagId"); }
        if (!(timeOk(t.startSec) && timeOk(t.endSec) && t.startSec <= t.endSec)) { return fail("tags.time"); }
        if (!(freqOk(t.lowHz) && freqOk(t.highHz) && t.lowHz <= t.highHz)) { return fail("tags.freq"); }
        if (!detail::cleanText(t.label, kMaxLabelBytes, false)) { return fail("tags.label"); }
        if (!detail::cleanText(t.note, kMaxNoteBytes, true)) { return fail("tags.note"); }
    }
    if (!detail::cleanText(p.notes, kMaxNotesBytes, true)) { return fail("notes"); }
    return true;
}

// ---- JSON ----------------------------------------------------------------------------------------------------------------------------------

inline std::string toJson(const Project& p) {
    using nlohmann::json;
    json j;
    j["format"] = kFormat;
    j["version"] = p.version;
    json rec;
    rec["path"] = p.recording.path; rec["sha256"] = p.recording.sha256; rec["sizeBytes"] = p.recording.sizeBytes;
    rec["sampleRateHz"] = p.recording.sampleRateHz; rec["centerHz"] = p.recording.centerHz; rec["durationSec"] = p.recording.durationSec;
    j["recording"] = rec;
    json crop = json::object();
    if (p.crop.hasTime) { crop["startSec"] = p.crop.startSec; crop["endSec"] = p.crop.endSec; }
    if (p.crop.hasFreq) { crop["lowHz"] = p.crop.lowHz; crop["highHz"] = p.crop.highHz; }
    j["crop"] = crop;
    json cur = json::object();
    if (p.cursors.hasTime) { cur["timeSec"] = p.cursors.timeSec; }
    if (p.cursors.hasFreq) { cur["freqHz"] = p.cursors.freqHz; }
    j["cursors"] = cur;
    json tags = json::array();
    for (const Tag& t : p.tags) {
        json o;
        o["id"] = t.id; o["startSec"] = t.startSec; o["endSec"] = t.endSec; o["lowHz"] = t.lowHz; o["highHz"] = t.highHz; o["label"] = t.label; o["note"] = t.note;
        tags.push_back(o);
    }
    j["tags"] = tags;
    j["nextTagId"] = p.nextTagId;
    j["notes"] = p.notes;
    return j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
}

namespace detail {

inline int maxDepth(const std::string& s) {
    int d = 0, best = 0;
    bool inStr = false, esc = false;
    for (char c : s) {
        if (inStr) { if (esc) { esc = false; } else if (c == '\\') { esc = true; } else if (c == '"') { inStr = false; } continue; }
        if (c == '"') { inStr = true; }
        else if (c == '[' || c == '{') { best = std::max(best, ++d); }
        else if (c == ']' || c == '}') { --d; }
    }
    return best;
}

inline bool num(const nlohmann::json& o, const char* k, double* out) {
    const auto it = o.find(k);
    if (it == o.end() || !it->is_number()) { return false; }
    *out = it->get<double>();
    return std::isfinite(*out);
}
inline bool str(const nlohmann::json& o, const char* k, std::string* out) {
    const auto it = o.find(k);
    if (it == o.end() || !it->is_string()) { return false; }
    *out = it->get<std::string>();
    return true;
}
inline bool uint(const nlohmann::json& o, const char* k, std::uint64_t* out) {
    const auto it = o.find(k);
    if (it == o.end() || !it->is_number_unsigned()) { return false; }
    *out = it->get<std::uint64_t>();
    return true;
}

}  // namespace detail

// Parses and validates. On any failure `*out` is left unchanged and `err` names the problem (never the user's text).
inline bool parse(const std::string& text, Project* out, std::string* err) {
    using nlohmann::json;
    auto fail = [&](const std::string& m) { if (err != nullptr) { *err = m; } return false; };
    if (text.size() > kMaxFileBytes) { return fail("the project file is larger than 4 MiB"); }
    if (detail::maxDepth(text) > 64) { return fail("the project file is nested too deeply"); }
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { return fail("the project file is not valid JSON"); }
    {
        std::string f;
        if (!detail::str(j, "format", &f) || f != kFormat) { return fail("not an analyser project file"); }
    }
    {
        const auto it = j.find("version");
        if (it == j.end() || !it->is_number_integer()) { return fail("version missing"); }
        const long long v = it->get<long long>();
        if (v != kVersion) { return fail("unknown project version " + std::to_string(v) + " (this program reads version 1)"); }
    }
    Project p;
    const auto rit = j.find("recording");
    if (rit == j.end() || !rit->is_object()) { return fail("recording missing"); }
    const json& r = *rit;
    if (!detail::str(r, "path", &p.recording.path)) { return fail("recording.path"); }
    if (r.contains("sha256") && !detail::str(r, "sha256", &p.recording.sha256)) { return fail("recording.sha256"); }
    if (r.contains("sizeBytes") && !detail::uint(r, "sizeBytes", &p.recording.sizeBytes)) { return fail("recording.sizeBytes"); }
    if (r.contains("sampleRateHz") && !detail::num(r, "sampleRateHz", &p.recording.sampleRateHz)) { return fail("recording.sampleRateHz"); }
    if (r.contains("centerHz") && !detail::num(r, "centerHz", &p.recording.centerHz)) { return fail("recording.centerHz"); }
    if (r.contains("durationSec") && !detail::num(r, "durationSec", &p.recording.durationSec)) { return fail("recording.durationSec"); }
    if (const auto it = j.find("crop"); it != j.end()) {
        if (!it->is_object()) { return fail("crop"); }
        const bool a = it->contains("startSec"), b = it->contains("endSec"), c = it->contains("lowHz"), d = it->contains("highHz");
        if (a != b || c != d) { return fail("crop (a range needs both ends)"); }
        if (a) { p.crop.hasTime = true; if (!detail::num(*it, "startSec", &p.crop.startSec) || !detail::num(*it, "endSec", &p.crop.endSec)) { return fail("crop.time"); } }
        if (c) { p.crop.hasFreq = true; if (!detail::num(*it, "lowHz", &p.crop.lowHz) || !detail::num(*it, "highHz", &p.crop.highHz)) { return fail("crop.freq"); } }
    }
    if (const auto it = j.find("cursors"); it != j.end()) {
        if (!it->is_object()) { return fail("cursors"); }
        if (it->contains("timeSec")) { p.cursors.hasTime = true; if (!detail::num(*it, "timeSec", &p.cursors.timeSec)) { return fail("cursors.timeSec"); } }
        if (it->contains("freqHz")) { p.cursors.hasFreq = true; if (!detail::num(*it, "freqHz", &p.cursors.freqHz)) { return fail("cursors.freqHz"); } }
    }
    if (const auto it = j.find("tags"); it != j.end()) {
        if (!it->is_array()) { return fail("tags"); }
        if (it->size() > kMaxTags) { return fail("tags (too many)"); }
        for (const json& o : *it) {
            if (!o.is_object()) { return fail("tags (entry)"); }
            Tag t;
            std::uint64_t id = 0;
            if (!detail::uint(o, "id", &id) || id > 0xFFFFFFFFull) { return fail("tags.id"); }
            t.id = static_cast<std::uint32_t>(id);
            if (!detail::num(o, "startSec", &t.startSec) || !detail::num(o, "endSec", &t.endSec)) { return fail("tags.time"); }
            if (!detail::num(o, "lowHz", &t.lowHz) || !detail::num(o, "highHz", &t.highHz)) { return fail("tags.freq"); }
            if (o.contains("label") && !detail::str(o, "label", &t.label)) { return fail("tags.label"); }
            if (o.contains("note") && !detail::str(o, "note", &t.note)) { return fail("tags.note"); }
            p.tags.push_back(std::move(t));
        }
    }
    {
        std::uint64_t n = 0;
        if (!detail::uint(j, "nextTagId", &n) || n == 0 || n > 0xFFFFFFFFull) { return fail("nextTagId"); }
        p.nextTagId = static_cast<std::uint32_t>(n);
    }
    if (j.contains("notes") && !detail::str(j, "notes", &p.notes)) { return fail("notes"); }
    std::string verr;
    if (!validate(p, &verr)) { return fail("invalid " + verr); }
    if (out != nullptr) { *out = std::move(p); }
    return true;
}

// Atomic save (temp file in the same folder, then rename) and load.
inline bool saveFile(const std::filesystem::path& path, const Project& p, std::string* err) {
    std::string verr;
    if (!validate(p, &verr)) { if (err != nullptr) { *err = "invalid " + verr; } return false; }
    const std::string text = toJson(p);
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { if (err != nullptr) { *err = "cannot write the project file"; } return false; }
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        f.flush();
        if (!f) { std::error_code ec; std::filesystem::remove(tmp, ec); if (err != nullptr) { *err = "cannot write the project file"; } return false; }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) { std::filesystem::remove(tmp, ec); if (err != nullptr) { *err = "cannot replace the project file"; } return false; }
    return true;
}

inline bool loadFile(const std::filesystem::path& path, Project* out, std::string* err) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) { if (err != nullptr) { *err = "cannot read the project file"; } return false; }
    if (size > kMaxFileBytes) { if (err != nullptr) { *err = "the project file is larger than 4 MiB"; } return false; }
    std::ifstream f(path, std::ios::binary);
    if (!f) { if (err != nullptr) { *err = "cannot read the project file"; } return false; }
    std::string text(static_cast<std::size_t>(size), '\0');
    f.read(text.data(), static_cast<std::streamsize>(size));
    if (static_cast<std::uintmax_t>(f.gcount()) != size) { if (err != nullptr) { *err = "cannot read the project file"; } return false; }
    return parse(text, out, err);
}

// ---- the recording the project was made on -------------------------------------------------------------------------------------------------

enum class RefCheck { Match, Missing, SizeDiffers, HashDiffers, NotHashed, Cancelled, ReadError };

// Is the file at `path` the recording of this project? A size check first (cheap), then the hash. `path` may differ from project.recording.path (the
// recording was moved); the project's own path is not used here.
inline RefCheck verifyReference(const Project& p, const std::filesystem::path& path, const std::atomic<bool>* cancel = nullptr) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec) { return RefCheck::Missing; }
    if (p.recording.sha256.empty()) { return RefCheck::NotHashed; }
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) { return RefCheck::ReadError; }
    if (p.recording.sizeBytes != 0 && size != p.recording.sizeBytes) { return RefCheck::SizeDiffers; }
    std::string hex, err;
    if (!sha256File(path, &hex, nullptr, &err, cancel)) { return err == "cancelled" ? RefCheck::Cancelled : RefCheck::ReadError; }
    return hex == p.recording.sha256 ? RefCheck::Match : RefCheck::HashDiffers;
}

// ---- editing -------------------------------------------------------------------------------------------------------------------------------

// Adds a tag (id assigned); 0 and `err` when it breaks the rules (the project is unchanged).
inline std::uint32_t addTag(Project* p, Tag t, std::string* err) {
    t.id = p->nextTagId;
    Project trial = *p;
    trial.tags.push_back(t);
    trial.nextTagId = p->nextTagId + 1;
    if (p->nextTagId == 0xFFFFFFFFu || !validate(trial, err)) { if (err != nullptr && err->empty()) { *err = "no more tag ids"; } return 0; }
    *p = std::move(trial);
    return t.id;
}

inline bool removeTag(Project* p, std::uint32_t id) {
    const auto it = std::find_if(p->tags.begin(), p->tags.end(), [&](const Tag& t) { return t.id == id; });
    if (it == p->tags.end()) { return false; }
    p->tags.erase(it);
    return true;
}

// Edits an existing tag (its id is kept). False and `err` when the result would be invalid.
inline bool editTag(Project* p, std::uint32_t id, const Tag& changed, std::string* err) {
    Project trial = *p;
    for (Tag& t : trial.tags) {
        if (t.id == id) { const std::uint32_t keep = t.id; t = changed; t.id = keep; return validate(trial, err) ? (*p = std::move(trial), true) : false; }
    }
    if (err != nullptr) { *err = "no such tag"; }
    return false;
}

// The tags whose box contains the point, smallest area first (what a click on the waterfall selects).
inline std::vector<std::uint32_t> tagsAt(const Project& p, double timeSec, double freqHz) {
    std::vector<std::pair<double, std::uint32_t>> hit;
    for (const Tag& t : p.tags) {
        if (timeSec >= t.startSec && timeSec <= t.endSec && freqHz >= t.lowHz && freqHz <= t.highHz) {
            hit.emplace_back((t.endSec - t.startSec) * (t.highHz - t.lowHz), t.id);
        }
    }
    std::sort(hit.begin(), hit.end());
    std::vector<std::uint32_t> ids;
    for (const auto& h : hit) { ids.push_back(h.second); }
    return ids;
}

// The crop as source frames [first, end) for a recording of `total` frames at `rate`; the whole recording when no time crop is set.
inline bool cropFrames(const Project& p, double rate, std::uint64_t total, std::uint64_t* first, std::uint64_t* end) {
    if (!(rate > 0.0) || !std::isfinite(rate)) { return false; }
    if (!p.crop.hasTime) { *first = 0; *end = total; return true; }
    const double a = std::floor(p.crop.startSec * rate), b = std::ceil(p.crop.endSec * rate);
    *first = a <= 0.0 ? 0 : std::min<std::uint64_t>(total, static_cast<std::uint64_t>(a));
    *end = b <= 0.0 ? 0 : std::min<std::uint64_t>(total, static_cast<std::uint64_t>(b));
    return *first < *end;
}

// ---- linked cursors (click-to-sync) --------------------------------------------------------------------------------------------------------

// One shared time and one shared frequency. Any view reports a click; the others read state() and move their own cursor lines. A click outside the
// recording's span is clamped to its edge; a NaN or infinite click is refused and changes nothing.
class LinkedCursors {
public:
    enum Changed : unsigned { kNone = 0, kTime = 1, kFreq = 2 };
    struct State { bool hasTime = false; double timeSec = 0.0; bool hasFreq = false; double freqHz = 0.0; std::uint64_t generation = 0; };

    LinkedCursors(double durationSec, double lowHz, double highHz) : dur_(durationSec), lo_(lowHz), hi_(highHz) {}

    unsigned clickWaterfall(double timeSec, double freqHz) {
        if (!std::isfinite(timeSec) || !std::isfinite(freqHz)) { return kNone; }
        return setTime(timeSec) | setFreq(freqHz);
    }
    unsigned clickSpectrum(double freqHz) { return std::isfinite(freqHz) ? setFreq(freqHz) : kNone; }
    unsigned clickTimeline(double timeSec) { return std::isfinite(timeSec) ? setTime(timeSec) : kNone; }
    void clear() { if (s_.hasTime || s_.hasFreq) { s_.hasTime = s_.hasFreq = false; ++s_.generation; } }
    State state() const { return s_; }
    void restore(const Cursors& c) {
        if (c.hasTime) { setTime(c.timeSec); }
        if (c.hasFreq) { setFreq(c.freqHz); }
    }
    Cursors toCursors() const { Cursors c; c.hasTime = s_.hasTime; c.timeSec = s_.timeSec; c.hasFreq = s_.hasFreq; c.freqHz = s_.freqHz; return c; }

    // pixel / bin / sample conversions, so every view maps its own coordinates to the shared ones
    static double sampleToSeconds(std::uint64_t sample, double rate) { return rate > 0.0 ? static_cast<double>(sample) / rate : 0.0; }
    static double secondsToSample(double sec, double rate) { return rate > 0.0 && sec > 0.0 ? sec * rate : 0.0; }
    // FFT bin k of n (fftshifted: bin 0 is the lowest frequency) at centre c and rate r
    static double binToFreq(double k, std::size_t n, double centerHz, double rateHz) { return n == 0 ? centerHz : centerHz - rateHz / 2.0 + (k + 0.5) * rateHz / static_cast<double>(n); }
    static double freqToBin(double f, std::size_t n, double centerHz, double rateHz) { return rateHz > 0.0 ? (f - (centerHz - rateHz / 2.0)) / rateHz * static_cast<double>(n) - 0.5 : 0.0; }

private:
    unsigned setTime(double t) {
        t = std::min(std::max(t, 0.0), dur_ > 0.0 ? dur_ : t);
        if (s_.hasTime && s_.timeSec == t) { return kNone; }
        s_.hasTime = true; s_.timeSec = t; ++s_.generation;
        return kTime;
    }
    unsigned setFreq(double f) {
        if (hi_ > lo_) { f = std::min(std::max(f, lo_), hi_); }
        if (s_.hasFreq && s_.freqHz == f) { return kNone; }
        s_.hasFreq = true; s_.freqHz = f; ++s_.generation;
        return kFreq;
    }
    double dur_, lo_, hi_;
    State s_;
};

}  // namespace cascade::core::analyser
