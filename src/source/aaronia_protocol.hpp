// aaronia_protocol.hpp - the HTTP stream-server interface of Aaronia's RTSA-Suite (the way a SPECTRAN V6
// delivers I/Q to other programs): a minimal HTTP/1.1 response reader, a chunked-transfer decoder, and the
// parser of the stream's records (a JSON header per packet, then either a JSON sample array or raw binary
// samples). No sockets here.
//
// PROVENANCE. Aaronia AG, "RTSA HTTP Stream Server Endpoints" (12 pages, German-language PDF metadata, English
// text), https://v6-forum.aaronia.de/wp-content/uploads/asgarosforum/3637/RTSA-http-Stream-Server-Endpoints-11.pdf ,
// fetched and read in full on 2026-11-11 (text extracted with pdftotext). It is the manufacturer's published
// description of the interface for third-party programs; it states no licence and no copyright terms. Read:
// the stream format (JSON packets with startTime, endTime, unit, payload, startFrequency, endFrequency,
// sampleDepth, sampleSize, samples, scale), "IQ Data" ("IQ Samples are transmitted as a flat array of
// alternating I and Q values", payload "iq"), the endpoints /info, /sample, /samples, /stream (chunked, "line
// limited JSON ... separated by a line feed and a record separator"), /control (PUT, frequencyCenter /
// frequencySpan / type capture; start/stop type streaming), the raw formats float16, float32 and int16 with
// "the binary part starts behind the record separator character", the 8 MByte drop rule, the Basic and
// RToken authorisation. SDRangel's Aaronia plug-in (GPL) was NOT read.
//
// WHAT THE DOCUMENT DOES NOT SAY, so is a HYPOTHESIS (UNVERIFIED on hardware, built as the stand-in's own
// reading): (1) the exact order and delimiters around a raw record (taken as: JSON object, an optional LF, one
// RS byte (0x1E), then samples*2*width bytes, the next record following at once); (2) the I/Q sample rate
// (taken as the number of pairs divided by endTime - startTime of a packet; the document gives no field for
// it); (3) that frequencySpan sets the I/Q rate; (4) that an int16 value divided by the packet's scale is the
// float value (int16 is not offered); (5) the default port (54664 appears in the document's examples; the
// server block's port is configurable) and the byte order of float32 (little-endian assumed).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace cascade::source::aaronia {

constexpr std::uint16_t kDefaultPort = 54664;
constexpr std::size_t kMaxHeadBytes = 16 * 1024;
constexpr std::size_t kMaxJsonBytes = 8 * 1024 * 1024;
constexpr std::size_t kMaxChunkBytes = 16 * 1024 * 1024;
constexpr std::size_t kMaxPairs = 4 * 1024 * 1024;  // per packet

struct HttpHead {
    int status = 0;
    std::string reason;
    std::map<std::string, std::string> headers;  // lower-case names, trimmed values
    bool chunked = false;
    long long contentLength = -1;  // -1: not given
};

// `head` is the text up to and excluding the blank line. False with a sentence for anything that is not an
// HTTP/1.x status line with well-formed headers, a Content-Length that is not a plain non-negative number, or a
// transfer encoding other than identity/chunked.
inline bool parseHttpHead(const std::string& head, HttpHead& out, std::string& err) {
    out = HttpHead{};
    const std::size_t eol = head.find("\r\n");
    const std::string line = head.substr(0, eol);
    if (line.size() < 12 || line.compare(0, 7, "HTTP/1.") != 0 || (line[7] != '0' && line[7] != '1') || line[8] != ' ') {
        err = "the reply is not an HTTP/1.x response";
        return false;
    }
    if (!std::isdigit(static_cast<unsigned char>(line[9])) || !std::isdigit(static_cast<unsigned char>(line[10])) || !std::isdigit(static_cast<unsigned char>(line[11]))) {
        err = "the HTTP status code is not three digits";
        return false;
    }
    out.status = (line[9] - '0') * 100 + (line[10] - '0') * 10 + (line[11] - '0');
    out.reason = line.size() > 13 ? line.substr(13) : std::string();
    std::size_t pos = eol == std::string::npos ? head.size() : eol + 2;
    while (pos < head.size()) {
        std::size_t e = head.find("\r\n", pos);
        if (e == std::string::npos) { e = head.size(); }
        const std::string h = head.substr(pos, e - pos);
        pos = e + 2;
        if (h.empty()) { continue; }
        const std::size_t c = h.find(':');
        if (c == std::string::npos || c == 0) { err = "an HTTP header line has no name or no colon"; return false; }
        std::string name = h.substr(0, c), val = h.substr(c + 1);
        for (char& ch : name) { ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); }
        if (name.find_first_of(" \t") != std::string::npos) { err = "an HTTP header name holds a space"; return false; }
        const std::size_t a = val.find_first_not_of(" \t");
        const std::size_t b = val.find_last_not_of(" \t");
        val = a == std::string::npos ? std::string() : val.substr(a, b - a + 1);
        out.headers[name] = val;
    }
    const auto te = out.headers.find("transfer-encoding");
    if (te != out.headers.end()) {
        std::string v = te->second;
        for (char& ch : v) { ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); }
        if (v == "chunked") { out.chunked = true; }
        else if (v != "identity") { err = "the reply uses a transfer encoding this client does not read (" + v.substr(0, 20) + ")"; return false; }
    }
    const auto cl = out.headers.find("content-length");
    if (cl != out.headers.end() && !out.chunked) {
        const std::string& v = cl->second;
        if (v.empty() || v.size() > 12 || !std::all_of(v.begin(), v.end(), [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)) != 0; })) {
            err = "the Content-Length is not a number";
            return false;
        }
        out.contentLength = std::stoll(v);
    }
    return true;
}

// Removes HTTP/1.1 chunk framing. feed() appends the decoded bytes; finished() is true after the last chunk.
class ChunkDecoder {
public:
    void feed(const std::uint8_t* d, std::size_t n, std::vector<std::uint8_t>& out) {
        for (std::size_t i = 0; i < n && err_.empty() && !finished_;) {
            switch (st_) {
                case St::Size: {
                    const char c = static_cast<char>(d[i++]);
                    if (c == '\n') {
                        std::string s = line_;
                        line_.clear();
                        if (!s.empty() && s.back() == '\r') { s.pop_back(); }
                        const std::size_t semi = s.find(';');  // chunk extensions are ignored
                        if (semi != std::string::npos) { s.resize(semi); }
                        if (s.empty() || s.size() > 8 || !std::all_of(s.begin(), s.end(), [](char ch) { return std::isxdigit(static_cast<unsigned char>(ch)) != 0; })) {
                            err_ = "a chunk size line is not hexadecimal";
                            return;
                        }
                        left_ = std::stoull(s, nullptr, 16);
                        if (left_ > kMaxChunkBytes) { err_ = "a chunk of " + std::to_string(left_) + " bytes is larger than this client accepts"; return; }
                        st_ = left_ == 0 ? St::Trailer : St::Data;
                    } else {
                        line_ += c;
                        if (line_.size() > 64) { err_ = "a chunk size line is too long"; return; }
                    }
                    break;
                }
                case St::Data: {
                    const std::size_t take = std::min<std::size_t>(left_, n - i);
                    out.insert(out.end(), d + i, d + i + take);
                    i += take;
                    left_ -= take;
                    if (left_ == 0) { st_ = St::DataEnd; crlf_ = 0; }
                    break;
                }
                case St::DataEnd: {
                    const char c = static_cast<char>(d[i++]);
                    if ((crlf_ == 0 && c == '\r') || (crlf_ == 1 && c == '\n')) {
                        if (++crlf_ == 2) { st_ = St::Size; }
                    } else {
                        err_ = "a chunk is not followed by CR LF";
                        return;
                    }
                    break;
                }
                case St::Trailer: {  // after the zero chunk: trailer lines until an empty line
                    const char c = static_cast<char>(d[i++]);
                    if (c == '\n') {
                        if (line_.empty() || line_ == "\r") { finished_ = true; }
                        line_.clear();
                    } else {
                        line_ += c;
                        if (line_.size() > 1024) { err_ = "a chunked trailer is too long"; return; }
                    }
                    break;
                }
            }
        }
    }
    bool finished() const { return finished_; }
    const std::string& error() const { return err_; }

private:
    enum class St { Size, Data, DataEnd, Trailer };
    St st_ = St::Size;
    std::string line_, err_;
    std::size_t left_ = 0;
    int crlf_ = 0;
    bool finished_ = false;
};

enum class SampleFormat { Json, Float32 };

struct IqRecord {
    double startTime = 0, endTime = 0;
    bool hasFrequency = false;
    double startFrequency = 0, endFrequency = 0;
    std::vector<std::complex<float>> samples;
    double rateHz() const {
        const double dt = endTime - startTime;
        return dt > 0 && !samples.empty() ? static_cast<double>(samples.size()) / dt : 0.0;
    }
};

// Cuts the decoded stream into records. Packets whose payload is not "iq" are consumed and counted but not
// returned. A malformed record makes error() non-empty and stops the parser.
class RecordParser {
public:
    explicit RecordParser(SampleFormat f) : fmt_(f) {}

    void feed(const std::uint8_t* d, std::size_t n, std::vector<IqRecord>& out) {
        if (!err_.empty()) { return; }
        buf_.insert(buf_.end(), d, d + n);
        for (;;) {
            if (!err_.empty()) { return; }
            if (!haveHeader_) {
                // skip separators between records, then find the end of the next JSON object
                std::size_t i = 0;
                while (i < buf_.size() && (buf_[i] == '\n' || buf_[i] == '\r' || buf_[i] == 0x1E || buf_[i] == ' ' || buf_[i] == '\t')) { ++i; }
                buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(i));
                if (buf_.empty()) { return; }
                if (buf_[0] != '{') { err_ = "a stream record does not start with a JSON object"; return; }
                const std::size_t end = objectEnd();
                if (end == 0) {
                    if (buf_.size() > kMaxJsonBytes) { err_ = "a record header is larger than this client accepts"; }
                    return;
                }
                nlohmann::json j = nlohmann::json::parse(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(end), nullptr, false);
                if (j.is_discarded() || !j.is_object()) { err_ = "a record header is not valid JSON"; return; }
                buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(end));
                if (!takeHeader(j)) { return; }
                haveHeader_ = true;
                binaryDelimiterDone_ = false;
            }
            if (haveHeader_) {
                if (rawPairs_ >= 0) {
                    // raw: an optional LF, one RS, then the samples
                    if (!binaryDelimiterDone_) {
                        std::size_t i = 0;
                        if (i < buf_.size() && buf_[i] == '\n') { ++i; }
                        if (i >= buf_.size()) { return; }
                        if (buf_[i] != 0x1E) { err_ = "the record separator before the binary samples is missing"; return; }
                        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(i + 1));
                        binaryDelimiterDone_ = true;
                    }
                    const std::size_t need = static_cast<std::size_t>(rawPairs_) * 8;
                    if (buf_.size() < need) { return; }
                    cur_.samples.reserve(static_cast<std::size_t>(rawPairs_));
                    for (std::size_t k = 0; k < static_cast<std::size_t>(rawPairs_); ++k) {
                        float re, im;
                        std::memcpy(&re, &buf_[8 * k], 4);
                        std::memcpy(&im, &buf_[8 * k + 4], 4);
                        if (!std::isfinite(re) || !std::isfinite(im)) { re = 0; im = 0; ++nonFinite_; }
                        cur_.samples.emplace_back(re, im);
                    }
                    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(need));
                }
                finishRecord(out);
            }
        }
    }
    const std::string& error() const { return err_; }
    std::uint64_t otherPackets() const { return other_; }
    std::uint64_t nonFiniteSamples() const { return nonFinite_; }

private:
    // Index one past the closing brace of the object at buf_[0], or 0 when it is not complete yet. Strings
    // (with escapes) are skipped.
    std::size_t objectEnd() {
        for (std::size_t i = scan_; i < buf_.size(); ++i) {
            const char c = static_cast<char>(buf_[i]);
            if (inStr_) {
                if (esc_) { esc_ = false; }
                else if (c == '\\') { esc_ = true; }
                else if (c == '"') { inStr_ = false; }
            } else if (c == '"') {
                inStr_ = true;
            } else if (c == '{' || c == '[') {
                ++depth_;
            } else if (c == '}' || c == ']') {
                if (--depth_ == 0) { scan_ = 0; inStr_ = false; esc_ = false; return i + 1; }
                if (depth_ < 0) { depth_ = 0; }
            }
        }
        scan_ = buf_.size();  // resume here when more bytes arrive: the scan is linear overall
        return 0;
    }

    bool takeHeader(const nlohmann::json& j) {
        cur_ = IqRecord{};
        rawPairs_ = -1;
        isIq_ = j.contains("payload") && j["payload"].is_string() && j["payload"].get<std::string>() == "iq";
        auto num = [&](const char* k, double& dst) {
            if (j.contains(k) && j[k].is_number()) { dst = j[k].get<double>(); return true; }
            return false;
        };
        num("startTime", cur_.startTime);
        num("endTime", cur_.endTime);
        const bool a = num("startFrequency", cur_.startFrequency), b = num("endFrequency", cur_.endFrequency);
        cur_.hasFrequency = a && b;
        if (!isIq_) {
            if (j.contains("samples") && j["samples"].is_number() && fmt_ == SampleFormat::Float32) {
                // a non-I/Q raw packet still carries binary data of a size we cannot know: stop rather than guess
                err_ = "a non-I/Q packet arrived in the raw format, whose size this client cannot know";
                return false;
            }
            return true;
        }
        if (!j.contains("samples")) { err_ = "an I/Q packet has no samples"; return false; }
        const auto& s = j["samples"];
        if (fmt_ == SampleFormat::Json) {
            if (!s.is_array()) { err_ = "an I/Q packet's samples are not an array"; return false; }
            if (s.size() % 2 != 0 || s.size() / 2 > kMaxPairs) { err_ = "an I/Q packet's sample array is odd-sized or too large"; return false; }
            cur_.samples.reserve(s.size() / 2);
            for (std::size_t i = 0; i + 1 < s.size(); i += 2) {
                if (!s[i].is_number() || !s[i + 1].is_number()) { err_ = "an I/Q sample is not a number"; return false; }
                cur_.samples.emplace_back(s[i].get<float>(), s[i + 1].get<float>());
            }
        } else {
            if (!s.is_number_integer() || s.get<long long>() < 0 || static_cast<unsigned long long>(s.get<long long>()) > kMaxPairs) {
                err_ = "an I/Q packet's sample count is not a plain number in range";
                return false;
            }
            rawPairs_ = static_cast<long long>(s.get<long long>());
        }
        return true;
    }

    void finishRecord(std::vector<IqRecord>& out) {
        haveHeader_ = false;
        if (isIq_) { out.push_back(std::move(cur_)); } else { ++other_; }
        cur_ = IqRecord{};
        rawPairs_ = -1;
    }

    SampleFormat fmt_;
    std::vector<std::uint8_t> buf_;
    std::size_t scan_ = 0;
    int depth_ = 0;
    bool inStr_ = false, esc_ = false;
    bool haveHeader_ = false, isIq_ = false, binaryDelimiterDone_ = false;
    long long rawPairs_ = -1;
    IqRecord cur_;
    std::string err_;
    std::uint64_t other_ = 0, nonFinite_ = 0;
};

// The JSON text of the control request that sets the capture, from the document's example
// {"frequencyCenter":..,"frequencySpan":..,"type":"capture"}.
inline std::string captureRequest(double centerHz, double spanHz) {
    nlohmann::json j;
    j["frequencyCenter"] = centerHz;
    j["frequencySpan"] = spanHz;
    j["type"] = "capture";
    return j.dump();
}
inline std::string streamingRequest(bool start) {
    nlohmann::json j;
    j["start"] = start;
    j["type"] = "streaming";
    return j.dump();
}

}  // namespace cascade::source::aaronia
