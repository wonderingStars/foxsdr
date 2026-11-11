// SigMF writer: one .sigmf-data file of raw interleaved I/Q and one .sigmf-meta JSON file beside it, read back through
// source/iq_readers.hpp (openSigmf).
//
// STANDALONE LAYER. Nothing here is wired (docs/feature-records/core-formats.md has the hand-off).
//
// SOURCES (clean room: the specification only)
//  * The SigMF specification v1.2.6 as served at https://sigmf.org (read 2026-11-11, recorded in docs/feature-records/signal-blocks-2.md):
//    a recording is a metadata file `<name>.sigmf-meta` (JSON object with "global", "captures", "annotations") and a dataset file
//    `<name>.sigmf-data`; global keys `core:datatype`, `core:sample_rate`, `core:version`, `core:description`, `core:author`,
//    `core:recorder`, `core:hw`, `core:license`; capture segment keys `core:sample_start`, `core:frequency`, `core:datetime`
//    (ISO 8601, UTC, "Z"); annotation keys `core:sample_start`, `core:sample_count`, `core:freq_lower_edge`, `core:freq_upper_edge`,
//    `core:label`, `core:comment`; datatypes cf32_le, ci16_le, ci8, cu8 (and the _be / cu16 forms, not written here).
//    UNVERIFIED: that "1.2.6" is the string a strict validator expects in core:version (the spec text was read, a validator was not run).
//  * JSON escaping is nlohmann::json's (RFC 8259): quotes, backslash and control characters are escaped; invalid UTF-8 in a string is
//    REPLACED with U+FFFD (never written raw), so the file always parses.
//
// DESIGN. The metadata file is valid from the moment open() returns and is rewritten atomically (temp file, then rename) by
// flushMetadata() and close(): the sample count is the data file's length (the format has no count field), so a crash leaves a
// complete, readable recording with the metadata of the last flush.
//
// THREADS: one writer, one thread.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace cascade::source::iqfile {

enum class SigmfType : int { Cf32Le, Ci16Le, Ci8, Cu8 };

struct SigmfOptions {
    SigmfType type = SigmfType::Cf32Le;
    double sampleRateHz = 0.0;       // required, finite, > 0
    bool hasCentre = false;
    double centreHz = 0.0;
    std::int64_t startUnixNs = -1;   // capture start (UTC); < 0 = not recorded
    std::string description, author, recorder, hardware, license;
};

struct SigmfAnnotation {
    std::uint64_t sampleStart = 0;
    bool hasCount = false;
    std::uint64_t sampleCount = 0;
    bool hasEdges = false;
    double lowerHz = 0.0, upperHz = 0.0;
    std::string label, comment;
};

namespace sigmfdetail {

inline const char* typeName(SigmfType t) {
    switch (t) { case SigmfType::Cf32Le: return "cf32_le"; case SigmfType::Ci16Le: return "ci16_le"; case SigmfType::Ci8: return "ci8"; default: return "cu8"; }
}
inline std::size_t bytesPerComponent(SigmfType t) {
    switch (t) { case SigmfType::Cf32Le: return 4; case SigmfType::Ci16Le: return 2; default: return 1; }
}

// "2026-11-11T16:36:45.123Z" from nanoseconds since the Unix epoch (negative = clamped to the epoch).
inline std::string isoUtc(std::int64_t ns) {
    if (ns < 0) { ns = 0; }
    const std::int64_t secs = ns / 1000000000LL;
    const int ms = static_cast<int>((ns % 1000000000LL) / 1000000LL);
    std::int64_t days = secs / 86400;
    const std::int64_t rem = secs % 86400;
    days += 719468;                                  // civil-from-days (Hinnant), era 0 = 0000-03-01
    const std::int64_t era = days / 146097;
    const std::int64_t doe = days - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    std::int64_t y = yoe + era * 400;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    const int d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    const int m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    if (m <= 2) { ++y; }
    char b[40];
    std::snprintf(b, sizeof b, "%04lld-%02d-%02dT%02d:%02d:%02d.%03dZ", static_cast<long long>(y), m, d, static_cast<int>(rem / 3600),
                  static_cast<int>((rem % 3600) / 60), static_cast<int>(rem % 60), ms);
    return b;
}

inline std::filesystem::path withExt(std::filesystem::path base, const char* ext) {
    base += ext;
    return base;
}

// A path the caller gave, with a .sigmf-meta / .sigmf-data / .sigmf suffix removed.
inline std::filesystem::path stripExt(const std::string& path) {
    std::filesystem::path p(path);
    const std::string e = p.extension().string();
    if (e == ".sigmf-meta" || e == ".sigmf-data" || e == ".sigmf") { p.replace_extension(); }
    return p;
}

}  // namespace sigmfdetail

class SigmfWriter {
public:
    SigmfWriter() = default;
    SigmfWriter(const SigmfWriter&) = delete;
    SigmfWriter& operator=(const SigmfWriter&) = delete;
    ~SigmfWriter() { std::string ignore; close(&ignore); }

    // `path` is the name with or without a .sigmf-meta / .sigmf-data suffix.
    bool open(const std::string& path, const SigmfOptions& o, std::string* err) {
        std::string ignore;
        close(&ignore);
        if (!(o.sampleRateHz > 0.0) || !std::isfinite(o.sampleRateHz)) { return fail(err, "sample rate must be a finite number above zero"); }
        if (o.hasCentre && !std::isfinite(o.centreHz)) { return fail(err, "centre frequency must be finite"); }
        if (path.empty()) { return fail(err, "empty path"); }
        opt_ = o;
        const std::filesystem::path base = sigmfdetail::stripExt(path);
        dataPath_ = sigmfdetail::withExt(base, ".sigmf-data");
        metaPath_ = sigmfdetail::withExt(base, ".sigmf-meta");
        data_.open(dataPath_, std::ios::binary | std::ios::trunc);
        if (!data_) { return fail(err, "cannot create " + dataPath_.string()); }
        samples_ = 0;
        captures_.clear();
        annotations_.clear();
        Capture c;
        c.sampleStart = 0;
        c.hasFreq = o.hasCentre;
        c.freqHz = o.centreHz;
        if (o.startUnixNs >= 0) { c.datetime = sigmfdetail::isoUtc(o.startUnixNs); }
        captures_.push_back(c);
        failed_ = false;
        if (!writeMeta(err)) { data_.close(); return false; }
        return true;
    }

    bool isOpen() const { return data_.is_open(); }
    std::uint64_t samplesWritten() const { return samples_; }
    std::string dataPath() const { return dataPath_.string(); }
    std::string metaPath() const { return metaPath_.string(); }

    // Appends complex samples, converted to the datatype (clipped; non-finite as 0).
    bool write(const std::complex<float>* iq, std::size_t n, std::string* err = nullptr) {
        if (!data_.is_open()) { return fail(err, "writer is not open"); }
        if (failed_) { return fail(err, "an earlier write failed"); }
        constexpr std::size_t kBlock = 16384;
        const std::size_t cb = sigmfdetail::bytesPerComponent(opt_.type);
        buf_.resize(kBlock * 2 * cb);
        std::size_t done = 0;
        while (done < n) {
            const std::size_t m = std::min(kBlock, n - done);
            const float* x = reinterpret_cast<const float*>(iq + done);
            for (std::size_t i = 0; i < 2 * m; ++i) {
                const double v = std::isfinite(x[i]) ? std::clamp(static_cast<double>(x[i]), -1.0, 1.0) : 0.0;
                switch (opt_.type) {
                    case SigmfType::Cf32Le: {
                        const float f = static_cast<float>(v);
                        std::memcpy(buf_.data() + 4 * i, &f, 4);   // little-endian host (every supported platform)
                        break;
                    }
                    case SigmfType::Ci16Le: {
                        const std::int16_t q = static_cast<std::int16_t>(std::lround(v * 32767.0));
                        buf_[2 * i] = static_cast<unsigned char>(static_cast<std::uint16_t>(q));
                        buf_[2 * i + 1] = static_cast<unsigned char>(static_cast<std::uint16_t>(q) >> 8);
                        break;
                    }
                    case SigmfType::Ci8:
                        buf_[i] = static_cast<unsigned char>(static_cast<std::int8_t>(std::lround(v * 127.0)));
                        break;
                    case SigmfType::Cu8:
                        buf_[i] = static_cast<unsigned char>(std::clamp(std::lround(v * 127.5 + 127.5), 0L, 255L));
                        break;
                }
            }
            data_.write(reinterpret_cast<const char*>(buf_.data()), static_cast<std::streamsize>(m * 2 * cb));
            if (!data_) { failed_ = true; return fail(err, "disk error while writing " + dataPath_.string()); }
            samples_ += m;
            done += m;
        }
        return true;
    }

    // A new capture segment from the CURRENT sample on (the radio was retuned). Takes effect in the next metadata write.
    bool addCapture(double centreHz, std::int64_t unixNs, std::string* err = nullptr) {
        if (!data_.is_open()) { return fail(err, "writer is not open"); }
        if (!std::isfinite(centreHz)) { return fail(err, "centre frequency must be finite"); }
        Capture c;
        c.sampleStart = samples_;
        c.hasFreq = true;
        c.freqHz = centreHz;
        if (unixNs >= 0) { c.datetime = sigmfdetail::isoUtc(unixNs); }
        // two segments may not start at the same sample: the later one replaces the earlier
        if (!captures_.empty() && captures_.back().sampleStart == c.sampleStart) { captures_.back() = c; } else { captures_.push_back(c); }
        return true;
    }

    // An annotation. Edges must be finite and ordered (lower <= upper); a start past the samples written so far is refused.
    bool addAnnotation(const SigmfAnnotation& a, std::string* err = nullptr) {
        if (!data_.is_open()) { return fail(err, "writer is not open"); }
        if (a.sampleStart > samples_) { return fail(err, "annotation starts after the last sample written"); }
        if (a.hasEdges && (!std::isfinite(a.lowerHz) || !std::isfinite(a.upperHz) || a.lowerHz > a.upperHz)) { return fail(err, "annotation frequency edges are not finite and ordered"); }
        if (annotations_.size() >= 65536) { return fail(err, "too many annotations (the reader accepts 65536)"); }
        annotations_.push_back(a);
        return true;
    }

    // Flushes the data and rewrites the metadata file atomically: after this the recording on disk is complete up to here.
    bool flushMetadata(std::string* err = nullptr) {
        if (!data_.is_open()) { return fail(err, "writer is not open"); }
        data_.flush();
        if (!data_) { failed_ = true; return fail(err, "disk error while flushing " + dataPath_.string()); }
        return writeMeta(err);
    }

    bool close(std::string* err = nullptr) {
        if (!data_.is_open()) { return true; }
        bool ok = true;
        data_.flush();
        if (!data_) { failed_ = true; }
        ok = writeMeta(err) && !failed_;
        data_.close();
        if (failed_ && err != nullptr && err->empty()) { *err = "a write failed: the data file is incomplete"; }
        return ok;
    }

    // The metadata as it would be written now (tests, previews).
    nlohmann::json metadata() const {
        nlohmann::json g = nlohmann::json::object();
        g["core:datatype"] = sigmfdetail::typeName(opt_.type);
        g["core:sample_rate"] = opt_.sampleRateHz;
        g["core:version"] = "1.2.6";
        if (!opt_.description.empty()) { g["core:description"] = opt_.description; }
        if (!opt_.author.empty()) { g["core:author"] = opt_.author; }
        if (!opt_.recorder.empty()) { g["core:recorder"] = opt_.recorder; }
        if (!opt_.hardware.empty()) { g["core:hw"] = opt_.hardware; }
        if (!opt_.license.empty()) { g["core:license"] = opt_.license; }
        nlohmann::json caps = nlohmann::json::array();
        for (const Capture& c : captures_) {
            nlohmann::json j = nlohmann::json::object();
            j["core:sample_start"] = c.sampleStart;
            if (c.hasFreq) { j["core:frequency"] = c.freqHz; }
            if (!c.datetime.empty()) { j["core:datetime"] = c.datetime; }
            caps.push_back(j);
        }
        nlohmann::json anns = nlohmann::json::array();
        for (const SigmfAnnotation& a : annotations_) {
            nlohmann::json j = nlohmann::json::object();
            j["core:sample_start"] = a.sampleStart;
            if (a.hasCount) { j["core:sample_count"] = a.sampleCount; }
            if (a.hasEdges) { j["core:freq_lower_edge"] = a.lowerHz; j["core:freq_upper_edge"] = a.upperHz; }
            if (!a.label.empty()) { j["core:label"] = a.label; }
            if (!a.comment.empty()) { j["core:comment"] = a.comment; }
            anns.push_back(j);
        }
        nlohmann::json root = nlohmann::json::object();
        root["global"] = g;
        root["captures"] = caps;
        root["annotations"] = anns;
        return root;
    }

private:
    struct Capture {
        std::uint64_t sampleStart = 0;
        bool hasFreq = false;
        double freqHz = 0.0;
        std::string datetime;
    };

    static bool fail(std::string* err, const std::string& m) { if (err != nullptr) { *err = m; } return false; }

    bool writeMeta(std::string* err) {
        const std::string text = metadata().dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
        std::filesystem::path tmp = metaPath_;
        tmp += ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) { return fail(err, "cannot create " + tmp.string()); }
            f.write(text.data(), static_cast<std::streamsize>(text.size()));
            f.flush();
            if (!f) { std::error_code ec; std::filesystem::remove(tmp, ec); return fail(err, "disk error while writing the metadata"); }
        }
        std::error_code ec;
        std::filesystem::rename(tmp, metaPath_, ec);
        if (ec) { std::filesystem::remove(tmp, ec); return fail(err, "cannot replace " + metaPath_.string()); }
        return true;
    }

    SigmfOptions opt_;
    std::filesystem::path dataPath_, metaPath_;
    std::ofstream data_;
    std::vector<unsigned char> buf_;
    std::vector<Capture> captures_;
    std::vector<SigmfAnnotation> annotations_;
    std::uint64_t samples_ = 0;
    bool failed_ = false;
};

}  // namespace cascade::source::iqfile
