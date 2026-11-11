// SigMF annotations on a recording: labelled time-and-frequency boxes read from, and written into, the `annotations` array of a `.sigmf-meta` file.
//
// STANDALONE. Nothing here is wired (docs/feature-records/recplay.md). feat/core-formats' SigmfWriter can add an annotation WHILE it records (it owns the
// file then); this header edits the annotations of a FINISHED recording's metadata and converts an analyser box to one. The join: the analyser screen calls
// addToFile() for a tag; the recordings browser reads annotations with readAnnotations().
//
// THE SPEC (SigMF v1.2.6, https://sigmf.org, "Annotations Object", read 2026-11-11; the document is CC-BY-SA 4.0): `annotations` is an array of objects
// that MUST be sorted by `core:sample_start` ascending (no order between equal starts). Each MUST have `core:sample_start` (integer >= 0). Optional:
// `core:sample_count` (integer >= 0; when absent the annotation applies from sample_start to the end of its capture), `core:freq_lower_edge` and
// `core:freq_upper_edge` (numbers in Hz, magnitude at most 1e12; BOTH or NEITHER), `core:label` (string, a SHORT form, RECOMMENDED at most 20 characters),
// `core:comment` (string), `core:generator` (string, the entity that made it), `core:uuid` (RFC 4122 string). Sample indices count from the start of the
// dataset, across captures. This header writes those keys only; any other key an annotation already carries (an extension namespace, say) is KEPT unchanged.
//
// WHAT IS DONE TO THE FILE: it is read whole (at most 16 MiB), parsed (refused untouched when it is not a JSON object, has no `global` object or its
// `annotations` is not an array of objects), the annotations are replaced and the document is written with its keys in the order it had (ordered JSON; a
// new `annotations` key goes last), 4-space indent, UTF-8, to a temporary file in the same folder and then renamed over the original. `global` and
// `captures` are never touched. A crash mid-write leaves the old file or the new one, not half of each.
//
// LIMITS (mine, stated): label 128 bytes (the spec's 20 is a recommendation; a longer one is accepted up to 128), comment 4000, generator 128, at most 65536
// annotations, text without control characters other than tab and newline.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace cascade::source::sigmfann {

using ojson = nlohmann::ordered_json;

constexpr std::size_t kMaxFileBytes = 16u * 1024u * 1024u;
constexpr std::size_t kMaxAnnotations = 65536;
constexpr std::size_t kMaxLabel = 128, kMaxComment = 4000, kMaxGenerator = 128;
constexpr double kMaxEdgeHz = 1.0e12;

struct Annotation {
    std::uint64_t sampleStart = 0;
    bool hasCount = false;
    std::uint64_t sampleCount = 0;
    bool hasFreq = false;
    double lowHz = 0.0, highHz = 0.0;
    std::string label, comment, generator, uuid;
    ojson extra = ojson::object();       // keys this header does not model, kept as found
};

namespace detail {

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

inline bool cleanText(const std::string& s, std::size_t maxBytes) {
    if (s.size() > maxBytes || !validUtf8(s)) { return false; }
    for (char ch : s) {
        const unsigned char u = static_cast<unsigned char>(ch);
        if (u == 0x7F || (u < 0x20 && u != '\n' && u != '\t' && u != '\r')) { return false; }
    }
    return true;
}

inline bool isUuid(const std::string& s) {
    if (s.size() != 36) { return false; }
    for (std::size_t i = 0; i < 36; ++i) {
        const char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') { return false; } }
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) { return false; }
    }
    return true;
}

inline const char* const kModelled[] = {"core:sample_start", "core:sample_count", "core:freq_lower_edge", "core:freq_upper_edge", "core:label", "core:comment", "core:generator", "core:uuid"};

inline bool isModelled(const std::string& k) { for (const char* m : kModelled) { if (k == m) { return true; } } return false; }

}  // namespace detail

// All the rules for ONE annotation. `totalSamples` 0 = unknown (no bound). `err` names the rule.
inline bool validate(const Annotation& a, std::uint64_t totalSamples, std::string* err) {
    auto fail = [&](const char* m) { if (err != nullptr) { *err = m; } return false; };
    if (totalSamples != 0) {
        if (a.sampleStart > totalSamples) { return fail("core:sample_start is past the end of the recording"); }
        if (a.hasCount && (a.sampleCount > totalSamples || a.sampleStart > totalSamples - a.sampleCount)) { return fail("the annotation runs past the end of the recording"); }
    }
    if (a.hasCount && a.sampleStart > UINT64_MAX - a.sampleCount) { return fail("core:sample_start + core:sample_count overflows"); }
    if (a.hasFreq) {
        if (!std::isfinite(a.lowHz) || !std::isfinite(a.highHz) || std::fabs(a.lowHz) > kMaxEdgeHz || std::fabs(a.highHz) > kMaxEdgeHz) { return fail("a frequency edge is not a number within +-1e12 Hz"); }
        if (a.lowHz > a.highHz) { return fail("core:freq_lower_edge is above core:freq_upper_edge"); }
    }
    if (!detail::cleanText(a.label, kMaxLabel)) { return fail("core:label is too long or has invalid characters"); }
    if (!detail::cleanText(a.comment, kMaxComment)) { return fail("core:comment is too long or has invalid characters"); }
    if (!detail::cleanText(a.generator, kMaxGenerator)) { return fail("core:generator is too long or has invalid characters"); }
    if (!a.uuid.empty() && !detail::isUuid(a.uuid)) { return fail("core:uuid is not an RFC 4122 identifier"); }
    if (!a.extra.is_object()) { return fail("extra keys are not an object"); }
    for (auto it = a.extra.begin(); it != a.extra.end(); ++it) { if (detail::isModelled(it.key())) { return fail("an extra key shadows a core key"); } }
    return true;
}

// An RFC 4122 version-4 identifier from 16 random bytes (the caller supplies the randomness).
inline std::string uuid4FromBytes(const unsigned char b[16]) {
    unsigned char x[16];
    for (int i = 0; i < 16; ++i) { x[i] = b[i]; }
    x[6] = static_cast<unsigned char>((x[6] & 0x0F) | 0x40);
    x[8] = static_cast<unsigned char>((x[8] & 0x3F) | 0x80);
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) { s += '-'; }
        s += hex[x[i] >> 4]; s += hex[x[i] & 15];
    }
    return s;
}

// A box from the analyser (seconds from the start of the dataset, absolute Hz) as an annotation. False when the box is empty, negative or not finite.
inline bool fromBox(double startSec, double endSec, double lowHz, double highHz, double sampleRateHz, const std::string& label, const std::string& comment,
                    const std::string& generator, Annotation* out) {
    if (!(sampleRateHz > 0.0) || !std::isfinite(sampleRateHz) || !std::isfinite(startSec) || !std::isfinite(endSec) || startSec < 0.0 || endSec < startSec) { return false; }
    if (!std::isfinite(lowHz) || !std::isfinite(highHz) || lowHz > highHz) { return false; }
    const double s = std::floor(startSec * sampleRateHz + 0.5), e = std::floor(endSec * sampleRateHz + 0.5);
    if (s > 9.0e15 || e > 9.0e15) { return false; }
    Annotation a;
    a.sampleStart = static_cast<std::uint64_t>(s);
    a.hasCount = true;
    a.sampleCount = static_cast<std::uint64_t>(e - s);
    a.hasFreq = true; a.lowHz = lowHz; a.highHz = highHz;
    a.label = label; a.comment = comment; a.generator = generator;
    *out = std::move(a);
    return true;
}

namespace detail {

inline ojson toJson(const Annotation& a) {
    ojson o = ojson::object();
    o["core:sample_start"] = a.sampleStart;
    if (a.hasCount) { o["core:sample_count"] = a.sampleCount; }
    if (a.hasFreq) { o["core:freq_lower_edge"] = a.lowHz; o["core:freq_upper_edge"] = a.highHz; }
    if (!a.label.empty()) { o["core:label"] = a.label; }
    if (!a.comment.empty()) { o["core:comment"] = a.comment; }
    if (!a.generator.empty()) { o["core:generator"] = a.generator; }
    if (!a.uuid.empty()) { o["core:uuid"] = a.uuid; }
    for (auto it = a.extra.begin(); it != a.extra.end(); ++it) { o[it.key()] = it.value(); }
    return o;
}

inline bool fromJson(const ojson& o, Annotation* a, std::string* err) {
    auto fail = [&](const char* m) { if (err != nullptr) { *err = m; } return false; };
    if (!o.is_object()) { return fail("an annotation is not an object"); }
    Annotation r;
    const auto s = o.find("core:sample_start");
    if (s == o.end() || !s->is_number_unsigned()) { return fail("core:sample_start is missing or not a non-negative integer"); }
    r.sampleStart = s->get<std::uint64_t>();
    if (const auto c = o.find("core:sample_count"); c != o.end()) {
        if (!c->is_number_unsigned()) { return fail("core:sample_count is not a non-negative integer"); }
        r.hasCount = true; r.sampleCount = c->get<std::uint64_t>();
    }
    const auto lo = o.find("core:freq_lower_edge"), hi = o.find("core:freq_upper_edge");
    if ((lo == o.end()) != (hi == o.end())) { return fail("only one of core:freq_lower_edge and core:freq_upper_edge is present"); }
    if (lo != o.end()) {
        if (!lo->is_number() || !hi->is_number()) { return fail("a frequency edge is not a number"); }
        r.hasFreq = true; r.lowHz = lo->get<double>(); r.highHz = hi->get<double>();
    }
    auto text = [&](const char* k, std::string* dst) {
        const auto it = o.find(k);
        if (it == o.end()) { return true; }
        if (!it->is_string()) { return false; }
        *dst = it->get<std::string>();
        return true;
    };
    if (!text("core:label", &r.label) || !text("core:comment", &r.comment) || !text("core:generator", &r.generator) || !text("core:uuid", &r.uuid)) { return fail("a text key is not a string"); }
    for (auto it = o.begin(); it != o.end(); ++it) { if (!isModelled(it.key())) { r.extra[it.key()] = it.value(); } }
    if (!validate(r, 0, err)) { return false; }
    *a = std::move(r);
    return true;
}

inline bool parseMeta(const std::string& text, ojson* doc, std::string* err) {
    auto fail = [&](const char* m) { if (err != nullptr) { *err = m; } return false; };
    if (text.size() > kMaxFileBytes) { return fail("the metadata file is larger than 16 MiB"); }
    ojson j = ojson::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { return fail("the metadata is not a JSON object"); }
    const auto g = j.find("global");
    if (g == j.end() || !g->is_object()) { return fail("the metadata has no global object"); }
    if (const auto a = j.find("annotations"); a != j.end() && !a->is_array()) { return fail("annotations is not an array"); }
    *doc = std::move(j);
    return true;
}

inline void sortStable(std::vector<Annotation>* v) {
    std::stable_sort(v->begin(), v->end(), [](const Annotation& a, const Annotation& b) { return a.sampleStart < b.sampleStart; });
}

}  // namespace detail

// The annotations of a metadata text, in file order (a file whose array is not sorted is read as it is). False and `err` when the text is not a SigMF
// metadata object or an entry breaks the rules.
inline bool readAnnotations(const std::string& metaText, std::vector<Annotation>* out, std::string* err) {
    ojson doc;
    if (!detail::parseMeta(metaText, &doc, err)) { return false; }
    std::vector<Annotation> v;
    if (const auto it = doc.find("annotations"); it != doc.end()) {
        if (it->size() > kMaxAnnotations) { if (err != nullptr) { *err = "more than 65536 annotations"; } return false; }
        for (const ojson& o : *it) {
            Annotation a;
            if (!detail::fromJson(o, &a, err)) { return false; }
            v.push_back(std::move(a));
        }
    }
    if (out != nullptr) { *out = std::move(v); }
    return true;
}

// Replaces the annotations of `metaText` with `list` (sorted by sample_start, equal starts keep their order), leaving every other key and its order alone.
// `totalSamples` (0 = unknown) bounds every annotation. On failure `metaText` is unchanged.
inline bool setAnnotations(std::string* metaText, std::vector<Annotation> list, std::uint64_t totalSamples, std::string* err) {
    ojson doc;
    if (!detail::parseMeta(*metaText, &doc, err)) { return false; }
    if (list.size() > kMaxAnnotations) { if (err != nullptr) { *err = "more than 65536 annotations"; } return false; }
    for (const Annotation& a : list) { if (!validate(a, totalSamples, err)) { return false; } }
    detail::sortStable(&list);
    ojson arr = ojson::array();
    for (const Annotation& a : list) { arr.push_back(detail::toJson(a)); }
    doc["annotations"] = std::move(arr);
    *metaText = doc.dump(4, ' ', false, ojson::error_handler_t::replace);
    return true;
}

// Reads `path`, applies `edit` to the annotation list, writes the result back atomically. `edit` returns false (with the reason in *err) to cancel.
template <typename Edit>
inline bool editFile(const std::filesystem::path& path, std::uint64_t totalSamples, Edit edit, std::string* err) {
    std::error_code ec;
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) { if (err != nullptr) { *err = "cannot read the metadata file"; } return false; }
    if (size > kMaxFileBytes) { if (err != nullptr) { *err = "the metadata file is larger than 16 MiB"; } return false; }
    std::string text(static_cast<std::size_t>(size), '\0');
    {
        std::ifstream f(path, std::ios::binary);
        if (!f) { if (err != nullptr) { *err = "cannot read the metadata file"; } return false; }
        f.read(text.data(), static_cast<std::streamsize>(size));
        if (static_cast<std::uintmax_t>(f.gcount()) != size) { if (err != nullptr) { *err = "cannot read the metadata file"; } return false; }
    }
    std::vector<Annotation> list;
    if (!readAnnotations(text, &list, err)) { return false; }
    if (!edit(&list, err)) { return false; }
    if (!setAnnotations(&text, std::move(list), totalSamples, err)) { return false; }
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { if (err != nullptr) { *err = "cannot write the metadata file"; } return false; }
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        f.flush();
        if (!f) { std::filesystem::remove(tmp, ec); if (err != nullptr) { *err = "cannot write the metadata file"; } return false; }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) { std::filesystem::remove(tmp, ec); if (err != nullptr) { *err = "cannot replace the metadata file"; } return false; }
    return true;
}

inline bool addToFile(const std::filesystem::path& path, const Annotation& a, std::uint64_t totalSamples, std::string* err) {
    return editFile(path, totalSamples, [&](std::vector<Annotation>* v, std::string*) { v->push_back(a); return true; }, err);
}

inline bool removeFromFile(const std::filesystem::path& path, std::size_t index, std::string* err) {
    return editFile(path, 0, [&](std::vector<Annotation>* v, std::string* e) {
        if (index >= v->size()) { if (e != nullptr) { *e = "no such annotation"; } return false; }
        v->erase(v->begin() + static_cast<std::ptrdiff_t>(index));
        return true;
    }, err);
}

}  // namespace cascade::source::sigmfann
