// metrics.hpp - a metrics registry and an HTTP handler in the Prometheus text exposition format, version 0.0.4 (row N21 of the gaps
// survey). Header-only, no HTTP library, no thread of its own: the wiring lane mounts MetricsHandler::handle() on the existing web
// server behind its existing sign-in (see the record's WIRING HAND-OFF) and calls StandardMetrics::publish() from a timer.
//
// FORMAT (https://prometheus.io/docs/instrumenting/exposition_formats/, read 2026-11-11, text format 0.0.4):
//  * served as text/plain; version=0.0.4; charset=utf-8; line oriented, the last line ends with a line feed;
//  * "# HELP name text" with backslash and line feed escaped as \\ and \n, at most one HELP and one TYPE line per metric name, TYPE
//    (counter, gauge, ...) before the first sample, all lines of a metric in one group;
//  * "name{label="value",...} value" with backslash, double quote and line feed escaped in label values as \\ \" \n; any UTF-8 is allowed;
//  * values are floats; NaN, +Inf and -Inf are valid; no timestamps are written (the scraper's time is used);
//  * a metric name and label set must be unique.
//  Names: metric names [a-zA-Z_:][a-zA-Z0-9_:]*, label names [a-zA-Z_][a-zA-Z0-9_]* and not starting with "__" (the page leaves the
//  exact expression to "the usual Prometheus restrictions"; these are the long-standing ones).
//
// BOUNDS. Label cardinality is the way a metrics endpoint grows without limit, so it is bounded: a family holds at most
// maxSeriesPerFamily (default 64) label sets, the registry at most maxFamilies (64) families and maxTotalSeries (1024) series; a label
// value is cut at maxLabelValueBytes (128, on a UTF-8 boundary), a family may have at most 4 labels. A sample that would exceed a bound is
// DROPPED and counted, and the count is exported as foxsdr_metrics_dropped_series_total so the loss is visible to the scraper.
//
// TEXT SAFETY. Any invalid UTF-8 or control byte in a label value or HELP text is replaced by U+FFFD, so the output is always valid UTF-8
// and no value can start a new line or a new sample.
//
// THREADS. Every registry call takes one mutex; render() copies under it and formats without blocking writers for long.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace cascade::net::metrics {

using Labels = std::vector<std::pair<std::string, std::string>>;

enum class MetricType { Counter, Gauge };
enum class Result { Ok, UnknownFamily, WrongType, BadLabels, CardinalityLimit, BadValue };

struct Limits {
    std::size_t maxFamilies = 64;
    std::size_t maxSeriesPerFamily = 64;
    std::size_t maxTotalSeries = 1024;
    std::size_t maxLabelValueBytes = 128;
    std::size_t maxLabelsPerSeries = 4;
    std::size_t maxHelpBytes = 200;
};

namespace detail {

inline bool metricNameOk(const std::string& n) {
    if (n.empty() || n.size() > 100) return false;
    for (std::size_t i = 0; i < n.size(); ++i) {
        const char c = n[i];
        const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':';
        const bool digit = c >= '0' && c <= '9';
        if (!(alpha || (i > 0 && digit))) return false;
    }
    return true;
}

inline bool labelNameOk(const std::string& n) {
    if (n.empty() || n.size() > 50) return false;
    if (n.size() >= 2 && n[0] == '_' && n[1] == '_') return false;
    for (std::size_t i = 0; i < n.size(); ++i) {
        const char c = n[i];
        const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        const bool digit = c >= '0' && c <= '9';
        if (!(alpha || (i > 0 && digit))) return false;
    }
    return true;
}

// Returns valid UTF-8: each invalid byte or sequence, overlong form, surrogate, value above U+10FFFF or control byte (other than
// newline, which is kept for the escaper) becomes U+FFFD. Cut to maxBytes on a character boundary.
inline std::string sanitiseUtf8(const std::string& in, std::size_t maxBytes) {
    std::string out;
    std::size_t i = 0;
    auto bad = [&](std::size_t consumed) {
        if (out.size() + 3 > maxBytes) return false;
        out += "\xEF\xBF\xBD";
        i += consumed;
        return true;
    };
    while (i < in.size()) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        std::size_t len = 0;
        if (c < 0x80) {
            if ((c < 0x20 && c != '\n') || c == 0x7F) { if (!bad(1)) break; continue; }
            len = 1;
        } else if (c >= 0xC2 && c <= 0xDF) len = 2;
        else if (c >= 0xE0 && c <= 0xEF) len = 3;
        else if (c >= 0xF0 && c <= 0xF4) len = 4;
        else { if (!bad(1)) break; continue; }
        if (i + len > in.size()) { if (!bad(1)) break; continue; }
        bool ok = true;
        for (std::size_t k = 1; k < len; ++k)
            if ((static_cast<unsigned char>(in[i + k]) & 0xC0) != 0x80) ok = false;
        if (ok && len == 3) {
            const unsigned char c1 = static_cast<unsigned char>(in[i + 1]);
            if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 >= 0xA0)) ok = false;   // overlong, surrogate
        }
        if (ok && len == 4) {
            const unsigned char c1 = static_cast<unsigned char>(in[i + 1]);
            if ((c == 0xF0 && c1 < 0x90) || (c == 0xF4 && c1 >= 0x90)) ok = false;   // overlong, above U+10FFFF
        }
        if (!ok) { if (!bad(1)) break; continue; }
        if (out.size() + len > maxBytes) break;
        out.append(in, i, len);
        i += len;
    }
    return out;
}

inline std::string escapeHelp(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '\\') o += "\\\\";
        else if (c == '\n') o += "\\n";
        else o += c;
    }
    return o;
}

inline std::string escapeLabelValue(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '\\') o += "\\\\";
        else if (c == '"') o += "\\\"";
        else if (c == '\n') o += "\\n";
        else o += c;
    }
    return o;
}

inline std::string formatValue(double v) {
    if (std::isnan(v)) return "NaN";
    if (std::isinf(v)) return v > 0 ? "+Inf" : "-Inf";
    char buf[48];
    if (v == std::floor(v) && std::fabs(v) < 1e15) std::snprintf(buf, sizeof buf, "%.0f", v);
    else std::snprintf(buf, sizeof buf, "%.17g", v);
    std::string s = buf;
    if (s == "-0") s = "0";
    return s;
}

}  // namespace detail

class MetricsRegistry {
public:
    explicit MetricsRegistry(Limits l = Limits{}) : lim_(l) {}

    // Defines a family. False if the name is invalid, already defined, or the family limit is reached.
    bool define(const std::string& name, MetricType type, const std::string& help) {
        std::lock_guard<std::mutex> lk(m_);
        if (!detail::metricNameOk(name) || families_.count(name) || families_.size() >= lim_.maxFamilies) return false;
        Family f;
        f.type = type;
        f.help = detail::sanitiseUtf8(help, lim_.maxHelpBytes);
        families_.emplace(name, std::move(f));
        return true;
    }

    // Counter: adds delta (>= 0, finite).
    Result inc(const std::string& name, const Labels& labels = {}, double delta = 1.0) {
        std::lock_guard<std::mutex> lk(m_);
        Family* f = find(name, MetricType::Counter);
        if (!f) return missing(name, MetricType::Counter);
        if (!std::isfinite(delta) || delta < 0) return Result::BadValue;
        Series* s = series(*f, labels);
        if (!s) return lastError_;
        s->value += delta;
        return Result::Ok;
    }

    // Counter set to an absolute total kept elsewhere (the engine's own counters). A lower value is a reset and is accepted.
    Result setCounter(const std::string& name, const Labels& labels, double total) {
        std::lock_guard<std::mutex> lk(m_);
        Family* f = find(name, MetricType::Counter);
        if (!f) return missing(name, MetricType::Counter);
        if (!std::isfinite(total) || total < 0) return Result::BadValue;
        Series* s = series(*f, labels);
        if (!s) return lastError_;
        s->value = total;
        return Result::Ok;
    }

    Result set(const std::string& name, const Labels& labels, double value) {
        std::lock_guard<std::mutex> lk(m_);
        Family* f = find(name, MetricType::Gauge);
        if (!f) return missing(name, MetricType::Gauge);
        Series* s = series(*f, labels);
        if (!s) return lastError_;
        s->value = value;   // NaN and infinities are legal gauge values in the format
        return Result::Ok;
    }

    Result add(const std::string& name, const Labels& labels, double delta) {
        std::lock_guard<std::mutex> lk(m_);
        Family* f = find(name, MetricType::Gauge);
        if (!f) return missing(name, MetricType::Gauge);
        if (!std::isfinite(delta)) return Result::BadValue;
        Series* s = series(*f, labels);
        if (!s) return lastError_;
        s->value += delta;
        return Result::Ok;
    }

    std::uint64_t droppedSeries() const {
        std::lock_guard<std::mutex> lk(m_);
        return dropped_;
    }
    std::size_t seriesCount() const {
        std::lock_guard<std::mutex> lk(m_);
        return total_;
    }
    std::size_t familyCount() const {
        std::lock_guard<std::mutex> lk(m_);
        return families_.size();
    }

    // The exposition text. Families sorted by name, series by label set; always ends with a line feed (an empty registry still has the
    // dropped-series metric).
    std::string render() const {
        std::string out;
        std::lock_guard<std::mutex> lk(m_);
        for (const auto& kv : families_) {
            const Family& f = kv.second;
            if (!f.help.empty()) out += "# HELP " + kv.first + " " + detail::escapeHelp(f.help) + "\n";
            out += "# TYPE " + kv.first + (f.type == MetricType::Counter ? " counter\n" : " gauge\n");
            for (const auto& sv : f.series) {
                out += kv.first;
                if (!sv.second.labels.empty()) {
                    out += '{';
                    bool first = true;
                    for (const auto& l : sv.second.labels) {
                        if (!first) out += ',';
                        first = false;
                        out += l.first + "=\"" + detail::escapeLabelValue(l.second) + "\"";
                    }
                    out += '}';
                }
                out += ' ' + detail::formatValue(sv.second.value) + "\n";
            }
        }
        out += "# HELP foxsdr_metrics_dropped_series_total Samples dropped because a label set exceeded a cardinality bound.\n";
        out += "# TYPE foxsdr_metrics_dropped_series_total counter\n";
        out += "foxsdr_metrics_dropped_series_total " + detail::formatValue(static_cast<double>(dropped_)) + "\n";
        return out;
    }

private:
    struct Series {
        Labels labels;
        double value = 0;
    };
    struct Family {
        MetricType type = MetricType::Gauge;
        std::string help;
        std::map<std::string, Series> series;
    };

    Family* find(const std::string& name, MetricType t) {
        auto it = families_.find(name);
        if (it == families_.end() || it->second.type != t) return nullptr;
        return &it->second;
    }
    Result missing(const std::string& name, MetricType) {
        return families_.count(name) ? Result::WrongType : Result::UnknownFamily;
    }

    // Finds or creates the series for `labels`, enforcing every bound; on refusal sets lastError_ and returns null.
    Series* series(Family& f, const Labels& labels) {
        if (labels.size() > lim_.maxLabelsPerSeries) { lastError_ = Result::BadLabels; return nullptr; }
        Labels clean;
        for (const auto& l : labels) {
            if (!detail::labelNameOk(l.first)) { lastError_ = Result::BadLabels; return nullptr; }
            clean.push_back({l.first, detail::sanitiseUtf8(l.second, lim_.maxLabelValueBytes)});
        }
        std::sort(clean.begin(), clean.end());
        for (std::size_t i = 1; i < clean.size(); ++i)
            if (clean[i].first == clean[i - 1].first) { lastError_ = Result::BadLabels; return nullptr; }
        std::string key;
        for (const auto& l : clean) key += l.first + '\x1f' + l.second + '\x1e';
        auto it = f.series.find(key);
        if (it != f.series.end()) return &it->second;
        if (f.series.size() >= lim_.maxSeriesPerFamily || total_ >= lim_.maxTotalSeries) {
            ++dropped_;
            lastError_ = Result::CardinalityLimit;
            return nullptr;
        }
        ++total_;
        Series s;
        s.labels = std::move(clean);
        return &f.series.emplace(std::move(key), std::move(s)).first->second;
    }

    Limits lim_;
    mutable std::mutex m_;
    std::map<std::string, Family> families_;
    std::size_t total_ = 0;
    std::uint64_t dropped_ = 0;
    Result lastError_ = Result::Ok;
};

struct HttpReply {
    int status = 200;
    std::string contentType;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
};

namespace detail {

// Compares without stopping at the first difference (the length itself is not secret here).
inline bool constantTimeEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char d = 0;
    for (std::size_t i = 0; i < a.size(); ++i) d |= static_cast<unsigned char>(a[i] ^ b[i]);
    return d == 0;
}

}  // namespace detail

struct HandlerConfig {
    std::string path = "/metrics";
    // Optional scrape token. Empty: this handler adds NO authentication of its own (the web server's sign-in gate in front of it is the
    // control). Set: the request must carry "Authorization: Bearer <token>" (a scraper cannot use the cookie sign-in).
    std::string bearerToken;
};

class MetricsHandler {
public:
    MetricsHandler(const MetricsRegistry& reg, HandlerConfig cfg = HandlerConfig{}) : reg_(reg), cfg_(std::move(cfg)) {}

    HttpReply handle(const std::string& method, const std::string& target, const std::string& authorization = std::string()) const {
        HttpReply r;
        std::string path = target.substr(0, target.find('?'));
        if (path != cfg_.path) {
            r.status = 404;
            r.contentType = "text/plain; charset=utf-8";
            r.body = "not found\n";
            return r;
        }
        if (method != "GET" && method != "HEAD") {
            r.status = 405;
            r.contentType = "text/plain; charset=utf-8";
            r.body = "method not allowed\n";
            r.headers.push_back({"Allow", "GET, HEAD"});
            return r;
        }
        if (!cfg_.bearerToken.empty()) {
            static const std::string scheme = "bearer ";
            bool ok = false;
            if (authorization.size() > scheme.size() && authorization.size() < 4096) {
                std::string head = authorization.substr(0, scheme.size());
                for (char& c : head)
                    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                ok = head == scheme && detail::constantTimeEquals(authorization.substr(scheme.size()), cfg_.bearerToken);
            }
            if (!ok) {
                r.status = 401;
                r.contentType = "text/plain; charset=utf-8";
                r.body = "unauthorized\n";
                r.headers.push_back({"WWW-Authenticate", "Bearer"});
                return r;
            }
        }
        r.status = 200;
        r.contentType = "text/plain; version=0.0.4; charset=utf-8";
        if (method == "GET") r.body = reg_.render();
        r.headers.push_back({"Cache-Control", "no-store"});
        return r;
    }

private:
    const MetricsRegistry& reg_;
    HandlerConfig cfg_;
};

// What the engine knows; the wiring lane fills this from its own counters (decode counts per decoder, the pipeline's drop counter, the
// S-meter level, the DSP thread's measured share, the web server's client count).
struct EngineSnapshot {
    std::map<std::string, std::uint64_t> decodesByDecoder;
    std::uint64_t droppedSamples = 0;
    double signalLevelDbfs = std::nan("");
    double dspCpuShare = std::nan("");     // 0..1 of one core
    int connectedClients = 0;
};

class StandardMetrics {
public:
    explicit StandardMetrics(MetricsRegistry& reg) : reg_(reg) {
        reg_.define("foxsdr_decodes_total", MetricType::Counter, "Decoded messages per decoder since the application started.");
        reg_.define("foxsdr_dropped_samples_total", MetricType::Counter, "Samples dropped between the radio and the DSP thread since start.");
        reg_.define("foxsdr_signal_level_dbfs", MetricType::Gauge, "Signal level of the tuned channel in dB relative to full scale.");
        reg_.define("foxsdr_dsp_thread_cpu_ratio", MetricType::Gauge, "Share of one CPU core used by the DSP thread, 0 to 1.");
        reg_.define("foxsdr_connected_clients", MetricType::Gauge, "Web clients currently connected.");
    }

    void publish(const EngineSnapshot& s) {
        for (const auto& kv : s.decodesByDecoder) reg_.setCounter("foxsdr_decodes_total", {{"decoder", kv.first}}, static_cast<double>(kv.second));
        reg_.setCounter("foxsdr_dropped_samples_total", {}, static_cast<double>(s.droppedSamples));
        if (std::isfinite(s.signalLevelDbfs)) reg_.set("foxsdr_signal_level_dbfs", {}, s.signalLevelDbfs);
        if (std::isfinite(s.dspCpuShare)) reg_.set("foxsdr_dsp_thread_cpu_ratio", {}, std::min(1.0, std::max(0.0, s.dspCpuShare)));
        reg_.set("foxsdr_connected_clients", {}, static_cast<double>(std::max(0, s.connectedClients)));
    }

private:
    MetricsRegistry& reg_;
};

}  // namespace cascade::net::metrics
