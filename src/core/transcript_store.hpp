// transcript_store.hpp - a bounded, in-memory store of transcribed received speech: timestamped segments of words, each word with a confidence, plus the
// confidence class a screen would colour by. It is only the RECORD MODEL for a speech-to-text feature. The recogniser itself (whisper.cpp, SDR-Control's
// and AetherSDR's "Copy Assist") needs a downloaded engine and model: NOT built, an owner decision (see docs/feature-records/hamops.md).
//
// PRIVACY: what people say on the air is message content. This store lives in memory only and has NO file or network function (a test scans for that); the
// caller decides what, if anything, a user-requested export does with the text it returns. Nothing is printed.
//
// BOUNDS: at most maxSegments segments, maxWords words and maxBytes bytes of word text in all; adding past a bound removes the OLDEST segments first. A
// segment has at most 256 words, a word at most 48 bytes of printable text. Words are sanitised (control characters become spaces and are trimmed), a
// confidence outside 0..1 is clamped and NaN becomes 0.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace cascade::core::speech {

struct Word {
    std::string text;
    float confidence = 0.0f;   // 0..1
};

struct Segment {
    std::int64_t startMs = 0;      // UTC milliseconds since 1970
    std::int64_t endMs = 0;
    int radio = 0;                 // 1..8, 0 = unknown
    double freqHz = 0.0;
    std::string language;          // short code the recogniser reported ("en"), at most 8 characters
    bool translated = false;       // the words are a translation of the speech
    std::vector<Word> words;
};

enum class ConfClass { High, Medium, Low };

struct StoreConfig {
    std::size_t maxSegments = 500;
    std::size_t maxWords = 20000;
    std::size_t maxBytes = 256 * 1024;
    float highAt = 0.80f;          // confidence >= highAt is High
    float mediumAt = 0.50f;        // >= mediumAt is Medium, below is Low
};

enum class AddResult { Added, Refused };

class TranscriptStore {
public:
    explicit TranscriptStore(StoreConfig c = StoreConfig()) : cfg_(c) {
        cfg_.maxSegments = std::max<std::size_t>(1, std::min<std::size_t>(cfg_.maxSegments, 100000));
        cfg_.maxWords = std::max<std::size_t>(1, std::min<std::size_t>(cfg_.maxWords, 1000000));
        cfg_.maxBytes = std::max<std::size_t>(64, std::min<std::size_t>(cfg_.maxBytes, 64u * 1024u * 1024u));
        if (!(cfg_.highAt >= 0.0f && cfg_.highAt <= 1.0f)) { cfg_.highAt = 0.80f; }
        if (!(cfg_.mediumAt >= 0.0f && cfg_.mediumAt <= cfg_.highAt)) { cfg_.mediumAt = std::min(0.50f, cfg_.highAt); }
    }

    // Sanitises and stores a copy. Refused: no usable word, a negative or backwards time, a radio outside 0..8.
    AddResult add(const Segment& in) {
        if (in.startMs < 0 || in.endMs < in.startMs || in.radio < 0 || in.radio > 8) { ++refused_; return AddResult::Refused; }
        Segment s;
        s.startMs = in.startMs;
        s.endMs = in.endMs;
        s.radio = in.radio;
        s.freqHz = (std::isfinite(in.freqHz) && in.freqHz > 0 && in.freqHz < 1e12) ? in.freqHz : 0.0;
        s.language = clean(in.language, 8);
        s.translated = in.translated;
        for (const Word& w : in.words) {
            if (s.words.size() >= 256) { break; }
            Word c;
            c.text = clean(w.text, 48);
            if (c.text.empty()) { continue; }
            const float cf = w.confidence;
            c.confidence = std::isnan(cf) ? 0.0f : (cf < 0.0f ? 0.0f : (cf > 1.0f ? 1.0f : cf));
            s.words.push_back(std::move(c));
        }
        if (s.words.empty()) { ++refused_; return AddResult::Refused; }
        std::size_t bytes = 0;
        for (const Word& w : s.words) { bytes += w.text.size(); }
        if (s.words.size() > cfg_.maxWords || bytes > cfg_.maxBytes) { ++refused_; return AddResult::Refused; }   // a single segment bigger than the whole store
        words_ += s.words.size();
        bytes_ += bytes;
        segs_.push_back(std::move(s));
        while (segs_.size() > cfg_.maxSegments || words_ > cfg_.maxWords || bytes_ > cfg_.maxBytes) { dropOldest(); }
        return AddResult::Added;
    }

    std::size_t segmentCount() const { return segs_.size(); }
    std::size_t wordCount() const { return words_; }
    std::size_t byteCount() const { return bytes_; }
    std::uint64_t evicted() const { return evicted_; }
    std::uint64_t refused() const { return refused_; }
    const std::deque<Segment>& segments() const { return segs_; }
    void clear() { segs_.clear(); words_ = bytes_ = 0; }

    ConfClass classify(float confidence) const {
        if (confidence >= cfg_.highAt) { return ConfClass::High; }
        if (confidence >= cfg_.mediumAt) { return ConfClass::Medium; }
        return ConfClass::Low;
    }

    // The words of a segment joined by single spaces.
    static std::string plain(const Segment& s) {
        std::string t;
        for (const Word& w : s.words) { if (!t.empty()) { t.push_back(' '); } t += w.text; }
        return t;
    }

    // Segments whose text contains `needle` (case-insensitive, ASCII), newest first, at most `limit`.
    std::vector<const Segment*> search(const std::string& needle, std::size_t limit = 50) const {
        std::vector<const Segment*> out;
        if (needle.empty() || needle.size() > 64) { return out; }
        const std::string n = lower(needle);
        for (auto it = segs_.rbegin(); it != segs_.rend() && out.size() < limit; ++it) {
            if (lower(plain(*it)).find(n) != std::string::npos) { out.push_back(&*it); }
        }
        return out;
    }

    // The whole store as text, one segment per line: "<start ms> <freq Hz> <text>". For a user-requested export; the caller owns what happens to it.
    std::string exportText() const {
        std::string t;
        for (const Segment& s : segs_) {
            t += std::to_string(s.startMs) + " " + std::to_string(static_cast<long long>(s.freqHz)) + " " + plain(s) + static_cast<char>(10);
        }
        return t;
    }

private:
    static std::string clean(const std::string& in, std::size_t maxLen) {
        std::string r;
        for (char c : in) {
            const unsigned char u = static_cast<unsigned char>(c);
            r.push_back((u < 32 || u == 127) ? ' ' : c);
            if (r.size() >= maxLen) { break; }
        }
        const std::size_t a = r.find_first_not_of(' ');
        if (a == std::string::npos) { return std::string(); }
        const std::size_t b = r.find_last_not_of(' ');
        return r.substr(a, b - a + 1);
    }
    static std::string lower(std::string s) {
        for (char& c : s) { if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); } }
        return s;
    }
    void dropOldest() {
        if (segs_.empty()) { return; }
        const Segment& s = segs_.front();
        std::size_t b = 0;
        for (const Word& w : s.words) { b += w.text.size(); }
        words_ -= s.words.size();
        bytes_ -= b;
        segs_.pop_front();
        ++evicted_;
    }

    StoreConfig cfg_;
    std::deque<Segment> segs_;
    std::size_t words_ = 0, bytes_ = 0;
    std::uint64_t evicted_ = 0, refused_ = 0;
};

}  // namespace cascade::core::speech
