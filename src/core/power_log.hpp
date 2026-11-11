// Power and SNR logging: a CSV logger that writes on a worker thread, a pure formatter for the InfluxDB line
// protocol, and a minimal sender (UDP datagrams, HTTP POST) for that line protocol.
//
// STANDALONE BLOCK. Nothing here is wired into the engine, the settings or the screen
// (docs/feature-records/signal-blocks-2.md, "WIRING HAND-OFF", says where it goes, and how the noise floor and
// the SNR should be taken from what the pipeline already measures).
//
// SOURCES
//  * InfluxDB line protocol: "Line protocol" reference, https://docs.influxdata.com/influxdb/v2/reference/syntax/line-protocol/
//    (read 2026-11-11): `measurement[,tag=value...] field=value[,field=value...] [timestamp]`; the first unescaped space
//    ends the measurement+tags, the second ends the fields; escape comma and space in a measurement, comma / equals /
//    space in tag keys, tag values and field keys, double quote and backslash in string field values; two backslashes read as
//    one; empty tag values are not allowed (omit the tag); names may not start with an underscore; floats have no suffix,
//    integers end in `i`, strings are double-quoted; the timestamp is an integer in nanoseconds by default; one point per
//    line, no newline inside a value.
//  * InfluxDB 1.x UDP input: https://docs.influxdata.com/influxdb/v1/supported_protocols/udp/ (read 2026-11-11): the input
//    splits received data by newline, one point per line, up to 64 KB per read; UDP gives the sender no error signal.
//  * InfluxDB v2 HTTP write: `POST /api/v2/write?org=..&bucket=..&precision=ns`, header `Authorization: Token <token>`,
//    success 204. UNVERIFIED from the reference (the page fetched did not carry the endpoint details; written from memory).
//  * ISO 8601 UTC time stamps from a count of days: the civil-from-days algorithm (H. Hinnant, "chrono-Compatible Low-Level
//    Date Algorithms", from memory; checked in the test against known dates including 2000-02-29 and 2100-03-01).
//
// THREADS
//  * Record formatting functions are pure and may run anywhere.
//  * CsvLogger::offer() is the ONE producer call: it is wait-free (an interval test and a single-producer ring buffer), never
//    allocates, never locks, never touches the disk, so it is safe on the pass thread. It must be called from one thread only
//    (the engine pass handles every radio, so that is one thread). Never call anything else of the logger from the DSP thread.
//  * The worker thread (started by start()) does ALL file I/O. start()/stop() are control-thread calls.
//  * The senders are BLOCKING (connect, send, receive with a timeout): call them from a worker thread, never from the pass or
//    DSP thread. They are off by default: a target with enabled == false never touches the network.
//
// SECRETS. The sender takes the token in the InfluxTarget handed to each call and never stores, logs or returns it. This block
// stores nothing on disk. (What the settings already keep in plain text is stated in the record.)
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace cascade::core::powerlog {

// One logged observation. Trivially copyable so it can cross the ring buffer without allocation.
struct Record {
    std::int64_t unixNs = 0;           // UTC, nanoseconds since 1970-01-01T00:00:00Z
    int radio = 0;                     // radio index (0-based)
    double frequencyHz = 0.0;          // tuned frequency (absolute)
    double bandwidthHz = 0.0;          // channel bandwidth
    double signalDbfs = NAN;           // channel power, dBFS
    double noiseDbfs = NAN;            // noise floor, dBFS
    double snrDb = NAN;                // SNR, dB
    bool hasDbm = false;               // a calibrated level is available
    double signalDbm = NAN;
};

// The SNR for a channel-power reading that INCLUDES the noise (what Pipeline::signalPowerDb() is): subtract the noise
// power in the linear domain. NaN when the reading is not above the floor by a measurable margin (< 0.1 dB).
inline double snrFromTotalAndFloor(double totalDb, double floorDb) {
    if (!std::isfinite(totalDb) || !std::isfinite(floorDb)) { return NAN; }
    const double t = std::pow(10.0, totalDb / 10.0);
    const double n = std::pow(10.0, floorDb / 10.0);
    if (t <= n * 1.0233) { return NAN; }          // under 0.1 dB above the floor: no signal to speak of
    return 10.0 * std::log10((t - n) / n);
}

// ---- formatting -------------------------------------------------------------------------------------------------------

// "2026-11-11T15:04:05.123Z": UTC, millisecond precision. Times before 1970 are clamped to 1970-01-01T00:00:00.000Z.
inline std::string isoUtc(std::int64_t unixNs) {
    if (unixNs < 0) { unixNs = 0; }
    const std::int64_t ms = unixNs / 1000000;
    std::int64_t days = ms / 86400000;
    const std::int64_t rem = ms % 86400000;
    // civil_from_days (Hinnant): days since 1970-01-01 -> y/m/d
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const std::int64_t doe = days - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t y = yoe + era * 400;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const std::int64_t mp = (5 * doy + 2) / 153;
    const std::int64_t d = doy - (153 * mp + 2) / 5 + 1;
    const std::int64_t m = mp < 10 ? mp + 3 : mp - 9;
    const std::int64_t year = y + (m <= 2 ? 1 : 0);
    char b[40];
    std::snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", static_cast<int>(year), static_cast<int>(m), static_cast<int>(d),
                  static_cast<int>(rem / 3600000), static_cast<int>((rem / 60000) % 60), static_cast<int>((rem / 1000) % 60), static_cast<int>(rem % 1000));
    return b;
}

// "20261111" for the file name of a record's UTC day.
inline std::string dayStamp(std::int64_t unixNs) {
    const std::string s = isoUtc(unixNs);
    return s.substr(0, 4) + s.substr(5, 2) + s.substr(8, 2);
}

// Locale-independent fixed-point text; "" for a non-finite value.
inline std::string fixedText(double v, int decimals) {
    if (!std::isfinite(v)) { return std::string(); }
    char b[64];
    const auto r = std::to_chars(b, b + sizeof b, v, std::chars_format::fixed, decimals);
    return r.ec == std::errc() ? std::string(b, r.ptr) : std::string();
}

// The CSV columns, in this order, never reordered (add new columns at the END and bump nothing else).
inline const char* csvHeader() { return "time_utc,radio,freq_hz,bandwidth_hz,signal_dbfs,noise_dbfs,snr_db,signal_dbm"; }

// One CSV line without the newline. Empty cell for a value that is not available. No field ever contains a comma or a quote.
inline std::string csvLine(const Record& r) {
    std::string s = isoUtc(r.unixNs);
    s += ',';
    s += std::to_string(r.radio);
    s += ',';
    s += fixedText(r.frequencyHz, 1);
    s += ',';
    s += fixedText(r.bandwidthHz, 1);
    s += ',';
    s += fixedText(r.signalDbfs, 2);
    s += ',';
    s += fixedText(r.noiseDbfs, 2);
    s += ',';
    s += fixedText(r.snrDb, 2);
    s += ',';
    if (r.hasDbm) { s += fixedText(r.signalDbm, 2); }
    return s;
}

// ---- InfluxDB line protocol ---------------------------------------------------------------------------------------------

struct InfluxOptions {
    std::string measurement = "foxsdr_signal";
    std::vector<std::pair<std::string, std::string>> tags;          // extra tags (host, band ...); radio is always a tag
    std::vector<std::pair<std::string, std::string>> stringFields;  // extra string fields (mode, label ...)
};

namespace detail {

// Escapes `s` for the contexts of the reference: `chars` get a backslash, and a backslash itself is doubled so that a value
// ending in a backslash cannot swallow the delimiter after it. Newlines and carriage returns cannot be represented and become '_'.
inline std::string esc(const std::string& s, const char* chars) {
    std::string o;
    o.reserve(s.size() + 4);
    for (char c : s) {
        if (c == '\n' || c == '\r') { o += '_'; continue; }
        if (c == '\\' || std::strchr(chars, c) != nullptr) { o += '\\'; }
        o += c;
    }
    return o;
}
inline std::string stripUnderscore(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size() && s[i] == '_') { ++i; }
    return s.substr(i);
}
inline std::string floatText(double v) {
    char b[64];
    const auto r = std::to_chars(b, b + sizeof b, v);     // shortest round-trip, may use an exponent: both are valid line protocol floats
    return std::string(b, r.ptr);
}
inline void addField(std::string& fields, const std::string& key, const std::string& value, bool keyIsEscaped = false) {
    if (!fields.empty()) { fields += ','; }
    fields += keyIsEscaped ? key : esc(key, ",= ");
    fields += '=';
    fields += value;
}

}  // namespace detail

// One line-protocol point, without the newline; "" when no field is available (a point needs at least one field).
// Tags: radio, then the options' tags (empty values and names that are empty after stripping leading underscores are
// dropped). Fields: freq_hz and bw_hz as INTEGERS (suffix i), signal_dbfs / noise_dbfs / snr_db / signal_dbm as floats (a
// non-finite one is omitted: InfluxDB has no NaN), the options' string fields quoted. Timestamp: integer nanoseconds,
// omitted when unixNs <= 0.
inline std::string influxLine(const Record& r, const InfluxOptions& o = InfluxOptions()) {
    std::string meas = detail::stripUnderscore(detail::esc(o.measurement, ", "));
    if (meas.empty()) { meas = "foxsdr_signal"; }
    std::string line = meas;
    line += ",radio=" + std::to_string(r.radio);
    for (const auto& t : o.tags) {
        const std::string k = detail::stripUnderscore(detail::esc(t.first, ",= "));
        const std::string v = detail::esc(t.second, ",= ");
        if (k.empty() || v.empty()) { continue; }
        line += ',' + k + '=' + v;
    }
    std::string f;
    if (std::isfinite(r.frequencyHz)) { detail::addField(f, "freq_hz", std::to_string(static_cast<long long>(std::llround(r.frequencyHz))) + "i"); }
    if (std::isfinite(r.bandwidthHz)) { detail::addField(f, "bw_hz", std::to_string(static_cast<long long>(std::llround(r.bandwidthHz))) + "i"); }
    if (std::isfinite(r.signalDbfs)) { detail::addField(f, "signal_dbfs", detail::floatText(r.signalDbfs)); }
    if (std::isfinite(r.noiseDbfs)) { detail::addField(f, "noise_dbfs", detail::floatText(r.noiseDbfs)); }
    if (std::isfinite(r.snrDb)) { detail::addField(f, "snr_db", detail::floatText(r.snrDb)); }
    if (r.hasDbm && std::isfinite(r.signalDbm)) { detail::addField(f, "signal_dbm", detail::floatText(r.signalDbm)); }
    for (const auto& sf : o.stringFields) {
        const std::string k = detail::stripUnderscore(detail::esc(sf.first, ",= "));
        if (k.empty()) { continue; }
        std::string v = "\"";
        for (char c : sf.second) {
            if (c == '\n' || c == '\r') { v += ' '; continue; }
            if (c == '"' || c == '\\') { v += '\\'; }
            v += c;
        }
        v += '"';
        detail::addField(f, k, v, true);
    }
    if (f.empty()) { return std::string(); }
    line += ' ';
    line += f;
    if (r.unixNs > 0) { line += ' ' + std::to_string(r.unixNs); }
    return line;
}

// ---- CSV logger -------------------------------------------------------------------------------------------------------

// The file seam: the default writes real files; a test substitutes one that fails on demand.
class FileSink {
public:
    virtual ~FileSink() = default;
    // Opens `path` for appending (creating the directory); returns the size the file already has in *existingBytes.
    virtual bool open(const std::string& path, std::uint64_t* existingBytes) = 0;
    virtual bool write(const char* data, std::size_t n) = 0;
    virtual bool flush() = 0;
    virtual void close() = 0;
};

class StdioSink final : public FileSink {
public:
    ~StdioSink() override { close(); }
    bool open(const std::string& path, std::uint64_t* existingBytes) override {
        close();
        std::error_code ec;
        const std::filesystem::path p(path);
        if (p.has_parent_path()) { std::filesystem::create_directories(p.parent_path(), ec); }
        std::uint64_t size = 0;
        if (std::filesystem::exists(p, ec)) { size = std::filesystem::file_size(p, ec); if (ec) { size = 0; } }
        f_ = std::fopen(path.c_str(), "ab");
        if (f_ == nullptr) { return false; }
        if (existingBytes != nullptr) { *existingBytes = size; }
        return true;
    }
    bool write(const char* data, std::size_t n) override { return f_ != nullptr && std::fwrite(data, 1, n, f_) == n; }
    bool flush() override { return f_ != nullptr && std::fflush(f_) == 0; }
    void close() override {
        if (f_ != nullptr) { std::fclose(f_); f_ = nullptr; }
    }

private:
    std::FILE* f_ = nullptr;
};

struct CsvOptions {
    std::string directory;                       // created on first write
    std::string baseName = "foxsdr-power";
    std::uint64_t rotateBytes = 0;               // start a new file before a line would take the file past this (0 = never)
    bool rotateDaily = true;                     // a new file when the UTC day of a record changes
    double intervalS = 1.0;                      // minimum spacing of accepted records PER RADIO (0 = every record)
    std::size_t queueCapacity = 4096;            // records; rounded up to a power of two
    int retryMs = 1000;                          // after a write or open failure, try the disk again no sooner than this
};

class CsvLogger {
public:
    static constexpr int kMaxRadios = 16;

    explicit CsvLogger(CsvOptions opt, std::unique_ptr<FileSink> sink = nullptr) : opt_(std::move(opt)), sink_(std::move(sink)) {
        if (!sink_) { sink_ = std::make_unique<StdioSink>(); }
        std::size_t cap = 16;
        while (cap < opt_.queueCapacity) { cap <<= 1; }
        ring_.assign(cap, Record());
        mask_ = cap - 1;
        lastAcceptedNs_.fill(INT64_MIN);
    }
    ~CsvLogger() { stop(); }
    CsvLogger(const CsvLogger&) = delete;
    CsvLogger& operator=(const CsvLogger&) = delete;

    bool start() {
        if (running_.exchange(true)) { return false; }
        worker_ = std::thread([this] { run(); });
        return true;
    }
    // Drains what is queued, flushes and closes, joins. Idempotent.
    void stop() {
        if (!running_.exchange(false)) { return; }
        if (worker_.joinable()) { worker_.join(); }
    }

    // PRODUCER (one thread only). Applies the per-radio interval, then queues. False if skipped by the interval or dropped
    // because the queue was full (dropped() counts the latter). Wait-free, allocation-free.
    bool offer(const Record& r) {
        const int idx = (r.radio >= 0 && r.radio < kMaxRadios) ? r.radio : 0;
        if (opt_.intervalS > 0.0) {
            const std::int64_t last = lastAcceptedNs_[static_cast<std::size_t>(idx)];
            if (last != INT64_MIN && r.unixNs - last < static_cast<std::int64_t>(opt_.intervalS * 1e9) && r.unixNs >= last) {
                skipped_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }
        const std::uint64_t h = head_.load(std::memory_order_relaxed);
        if (h - tail_.load(std::memory_order_acquire) >= ring_.size()) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        lastAcceptedNs_[static_cast<std::size_t>(idx)] = r.unixNs;
        ring_[static_cast<std::size_t>(h) & mask_] = r;
        head_.store(h + 1, std::memory_order_release);
        queued_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    std::uint64_t queued() const { return queued_.load(std::memory_order_relaxed); }
    std::uint64_t skippedByInterval() const { return skipped_.load(std::memory_order_relaxed); }
    std::uint64_t droppedQueueFull() const { return dropped_.load(std::memory_order_relaxed); }
    std::uint64_t written() const { return written_.load(std::memory_order_relaxed); }
    std::uint64_t droppedDiskFailing() const { return droppedDisk_.load(std::memory_order_relaxed); }
    std::uint64_t writeFailures() const { return failures_.load(std::memory_order_relaxed); }
    std::uint64_t filesOpened() const { return files_.load(std::memory_order_relaxed); }
    bool diskHealthy() const { return healthy_.load(std::memory_order_relaxed); }
    std::string lastError() const { std::lock_guard<std::mutex> lk(errMu_); return err_; }
    std::string currentPath() const { std::lock_guard<std::mutex> lk(errMu_); return path_; }

private:
    void run() {
        try {
            while (true) {
                const bool stopping = !running_.load(std::memory_order_acquire);
                bool any = false;
                std::uint64_t t = tail_.load(std::memory_order_relaxed);
                while (t != head_.load(std::memory_order_acquire)) {
                    const Record r = ring_[static_cast<std::size_t>(t) & mask_];
                    tail_.store(++t, std::memory_order_release);
                    handle(r);
                    any = true;
                }
                if (any && open_) { flushNow(); }
                if (stopping && t == head_.load(std::memory_order_acquire)) { break; }
                if (!any) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
            }
        } catch (...) {
            setError("logger worker stopped by an exception");
            healthy_.store(false, std::memory_order_relaxed);
        }
        if (open_) { sink_->flush(); sink_->close(); open_ = false; }
    }

    void setError(const std::string& e) {
        std::lock_guard<std::mutex> lk(errMu_);
        err_ = e;
    }

    void fail(const std::string& why) {
        failures_.fetch_add(1, std::memory_order_relaxed);
        healthy_.store(false, std::memory_order_relaxed);
        setError(why);
        if (open_) { sink_->close(); open_ = false; }
        retryAt_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(opt_.retryMs);
        failing_ = true;
    }

    std::string pathFor(const std::string& day, int part) const {
        std::string n = opt_.baseName + "-" + day;
        if (part > 0) {
            char b[16];
            std::snprintf(b, sizeof b, "-%03d", part);
            n += b;
        }
        n += ".csv";
        return opt_.directory.empty() ? n : (std::filesystem::path(opt_.directory) / n).string();
    }

    // Opens the file for (day, part), skipping parts that are already full; writes the header into an empty file.
    bool openFile(const std::string& day, int part) {
        for (int guard = 0; guard < 1000; ++guard, ++part) {
            const std::string p = pathFor(day, part);
            std::uint64_t existing = 0;
            if (!sink_->open(p, &existing)) { fail("cannot open " + p); return false; }
            if (opt_.rotateBytes > 0 && existing >= opt_.rotateBytes) { sink_->close(); continue; }
            open_ = true;
            curDay_ = day;
            curPart_ = part;
            curBytes_ = existing;
            files_.fetch_add(1, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> lk(errMu_);
                path_ = p;
            }
            if (existing == 0) {
                const std::string h = std::string(csvHeader()) + "\n";
                if (!sink_->write(h.data(), h.size())) { fail("cannot write the header of " + p); return false; }
                curBytes_ = h.size();
            }
            return true;
        }
        fail("no part of " + opt_.baseName + " for " + day + " has room");
        return false;
    }

    void flushNow() {
        if (!sink_->flush()) { fail("flush failed"); }
    }

    void handle(const Record& r) {
        if (failing_) {
            if (std::chrono::steady_clock::now() < retryAt_) { droppedDisk_.fetch_add(1, std::memory_order_relaxed); return; }
        }
        const std::string day = dayStamp(r.unixNs);
        std::string line = csvLine(r);
        line += '\n';
        bool needOpen = !open_;
        int part = curPart_;
        std::string useDay = curDay_;
        if (open_) {
            if (opt_.rotateDaily && day != curDay_) { needOpen = true; part = 0; useDay = day; }
            else if (opt_.rotateBytes > 0 && curBytes_ + line.size() > opt_.rotateBytes && curBytes_ > 0) { needOpen = true; part = curPart_ + 1; }
        } else {
            useDay = opt_.rotateDaily || curDay_.empty() ? day : curDay_;
            part = (useDay == curDay_) ? curPart_ : 0;
        }
        if (needOpen) {
            if (open_) { sink_->flush(); sink_->close(); open_ = false; }
            if (!openFile(useDay, part)) { droppedDisk_.fetch_add(1, std::memory_order_relaxed); return; }
        }
        if (!sink_->write(line.data(), line.size())) {
            fail("write failed (disk full or removed?)");
            droppedDisk_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        curBytes_ += line.size();
        written_.fetch_add(1, std::memory_order_relaxed);
        if (failing_) { failing_ = false; healthy_.store(true, std::memory_order_relaxed); }
    }

    CsvOptions opt_;
    std::unique_ptr<FileSink> sink_;
    std::vector<Record> ring_;
    std::size_t mask_ = 0;
    std::atomic<std::uint64_t> head_{0}, tail_{0};
    std::array<std::int64_t, kMaxRadios> lastAcceptedNs_{};
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::atomic<std::uint64_t> queued_{0}, skipped_{0}, dropped_{0}, written_{0}, droppedDisk_{0}, failures_{0}, files_{0};
    std::atomic<bool> healthy_{true};
    mutable std::mutex errMu_;
    std::string err_, path_;
    // worker-only
    bool open_ = false;
    bool failing_ = false;
    std::string curDay_;
    int curPart_ = 0;
    std::uint64_t curBytes_ = 0;
    std::chrono::steady_clock::time_point retryAt_{};
};

// ---- Influx senders ---------------------------------------------------------------------------------------------------

struct InfluxTarget {
    bool enabled = false;                // OFF by default: a disabled target never touches the network
    bool http = false;                   // false = UDP datagrams, true = HTTP POST
    std::string host;                    // named by the user
    int port = 0;
    std::string path = "/api/v2/write";
    std::string org, bucket;
    std::string token;                   // supplied per call by the caller; never stored or logged here
    int timeoutMs = 2000;
};

struct SendResult {
    bool ok = false;
    int httpStatus = 0;                  // HTTP only
    std::string error;
};

namespace detail {

inline std::string pctEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == '~') { o += static_cast<char>(c); }
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

inline bool hasCtl(const std::string& s) { return s.find_first_of("\r\n") != std::string::npos; }

#if defined(_WIN32)
using sock_t = SOCKET;
inline bool badSock(sock_t s) { return s == INVALID_SOCKET; }
inline void closeSock(sock_t s) { ::closesocket(s); }
inline void ensureNet() {
    static const bool once = [] { WSADATA d; return ::WSAStartup(MAKEWORD(2, 2), &d) == 0; }();
    (void)once;
}
#else
using sock_t = int;
inline bool badSock(sock_t s) { return s < 0; }
inline void closeSock(sock_t s) { ::close(s); }
inline void ensureNet() {}
#endif

// Resolves and connects (TCP with a bounded connect, or UDP "connect" that only fixes the peer). Returns an invalid socket and
// an error text on failure.
inline sock_t dial(const InfluxTarget& t, bool udp, std::string* err) {
    ensureNet();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = udp ? SOCK_DGRAM : SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string port = std::to_string(t.port);
    if (::getaddrinfo(t.host.c_str(), port.c_str(), &hints, &res) != 0 || res == nullptr) {
        *err = "cannot resolve " + t.host;
#if defined(_WIN32)
        return INVALID_SOCKET;
#else
        return -1;
#endif
    }
    sock_t s = static_cast<sock_t>(-1);
#if defined(_WIN32)
    s = INVALID_SOCKET;
#endif
    for (addrinfo* a = res; a != nullptr; a = a->ai_next) {
        sock_t c = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (badSock(c)) { continue; }
        if (udp) {
            if (::connect(c, a->ai_addr, static_cast<int>(a->ai_addrlen)) == 0) { s = c; break; }
            closeSock(c);
            continue;
        }
#if defined(_WIN32)
        u_long nb = 1;
        ::ioctlsocket(c, FIONBIO, &nb);
        const int rc = ::connect(c, a->ai_addr, static_cast<int>(a->ai_addrlen));
        bool ok = rc == 0;
        if (!ok && ::WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set w, e;
            FD_ZERO(&w); FD_ZERO(&e); FD_SET(c, &w); FD_SET(c, &e);
            timeval tv{t.timeoutMs / 1000, (t.timeoutMs % 1000) * 1000};
            ok = ::select(0, nullptr, &w, &e, &tv) > 0 && FD_ISSET(c, &w);
        }
        nb = 0;
        ::ioctlsocket(c, FIONBIO, &nb);
        DWORD ms = static_cast<DWORD>(t.timeoutMs);
        ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
        ::setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
#else
        bool ok = ::connect(c, a->ai_addr, a->ai_addrlen) == 0;   // (POSIX: the blocking connect is bounded by the kernel; UNVERIFIED here)
        timeval tv{t.timeoutMs / 1000, (t.timeoutMs % 1000) * 1000};
        ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        ::setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
        if (ok) { s = c; break; }
        closeSock(c);
    }
    ::freeaddrinfo(res);
    if (badSock(s)) { *err = "cannot connect to " + t.host + ":" + port; }
    return s;
}

inline bool targetOk(const InfluxTarget& t, SendResult* r) {
    if (!t.enabled) { r->error = "disabled"; return false; }
    if (t.host.empty() || t.port < 1 || t.port > 65535) { r->error = "no host or port"; return false; }
    if (hasCtl(t.host) || hasCtl(t.token) || hasCtl(t.path) || hasCtl(t.org) || hasCtl(t.bucket)) { r->error = "control characters in the target"; return false; }
    return true;
}

}  // namespace detail

// The HTTP request text for a write (pure; what sendHttp sends): POST path?org&bucket&precision=ns, Host, Authorization when a
// token is given, Content-Type text/plain, Content-Length, Connection: close, then the body.
inline std::string buildHttpPost(const InfluxTarget& t, const std::string& body) {
    std::string q = t.path.empty() ? "/api/v2/write" : t.path;
    q += "?precision=ns";
    if (!t.org.empty()) { q += "&org=" + detail::pctEncode(t.org); }
    if (!t.bucket.empty()) { q += "&bucket=" + detail::pctEncode(t.bucket); }
    std::string h = "POST " + q + " HTTP/1.1\r\nHost: " + t.host;
    if (t.port != 80) { h += ":" + std::to_string(t.port); }
    h += "\r\n";
    if (!t.token.empty()) { h += "Authorization: Token " + t.token + "\r\n"; }
    h += "Content-Type: text/plain; charset=utf-8\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
    return h + body;
}

// Groups lines (newline-joined) into datagrams of at most maxBytes; a single line longer than that goes alone.
inline std::vector<std::string> splitDatagrams(const std::vector<std::string>& lines, std::size_t maxBytes = 1400) {
    std::vector<std::string> out;
    std::string cur;
    for (const std::string& l : lines) {
        if (l.empty()) { continue; }
        if (!cur.empty() && cur.size() + 1 + l.size() > maxBytes) { out.push_back(cur); cur.clear(); }
        if (!cur.empty()) { cur += '\n'; }
        cur += l;
    }
    if (!cur.empty()) { out.push_back(cur); }
    return out;
}

// BLOCKING. One datagram per group of lines. UDP gives no delivery signal: ok means "handed to the network stack".
inline SendResult sendUdp(const InfluxTarget& t, const std::vector<std::string>& lines) {
    SendResult r;
    if (!detail::targetOk(t, &r)) { return r; }
    std::string err;
    detail::sock_t s = detail::dial(t, true, &err);
    if (detail::badSock(s)) { r.error = err; return r; }
    bool all = true;
    for (const std::string& d : splitDatagrams(lines)) {
        if (::send(s, d.data(), static_cast<int>(d.size()), 0) != static_cast<int>(d.size())) { all = false; r.error = "send failed"; break; }
    }
    detail::closeSock(s);
    r.ok = all;
    return r;
}

// BLOCKING. POSTs the lines (newline-joined) and reads the status line of the answer; ok for 2xx. No TLS: plain HTTP to the
// host the user named (put a TLS-terminating proxy or Telegraf in front if the network is not trusted).
inline SendResult sendHttp(const InfluxTarget& t, const std::vector<std::string>& lines) {
    SendResult r;
    if (!detail::targetOk(t, &r)) { return r; }
    std::string body;
    for (const std::string& l : lines) { if (!l.empty()) { body += l; body += '\n'; } }
    if (body.empty()) { r.error = "nothing to send"; return r; }
    std::string err;
    detail::sock_t s = detail::dial(t, false, &err);
    if (detail::badSock(s)) { r.error = err; return r; }
    const std::string req = buildHttpPost(t, body);
    std::size_t sent = 0;
    while (sent < req.size()) {
        const int n = ::send(s, req.data() + sent, static_cast<int>(req.size() - sent), 0);
        if (n <= 0) { r.error = "send failed"; detail::closeSock(s); return r; }
        sent += static_cast<std::size_t>(n);
    }
    char buf[512];
    std::string head;
    while (head.size() < 4096 && head.find("\r\n") == std::string::npos) {
        const int n = ::recv(s, buf, sizeof buf, 0);
        if (n <= 0) { break; }
        head.append(buf, static_cast<std::size_t>(n));
    }
    detail::closeSock(s);
    if (head.size() < 12 || head.compare(0, 5, "HTTP/") != 0) { r.error = "no HTTP answer"; return r; }
    const std::size_t sp = head.find(' ');
    r.httpStatus = sp == std::string::npos ? 0 : std::atoi(head.c_str() + sp + 1);
    r.ok = r.httpStatus >= 200 && r.httpStatus < 300;
    if (!r.ok) { r.error = "HTTP " + std::to_string(r.httpStatus); }
    return r;
}

}  // namespace cascade::core::powerlog
