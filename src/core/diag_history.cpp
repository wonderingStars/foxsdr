// diag_history.cpp - see diag_history.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/diag_history.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include <utility>

namespace cascade::core {

namespace fs = std::filesystem;

namespace {

// The log's own file names (DiagLog::kKeptFiles of them, live file first):
// foxsdr.log, foxsdr.1.log, foxsdr.2.log. Oldest first here, which is the order
// the sessions in them run.
std::vector<std::string> logFileNamesOldestFirst() {
    return {"foxsdr.2.log", "foxsdr.1.log", "foxsdr.log"};
}

// The newest `cap` bytes of a file, cut into lines. A file longer than `cap`
// starts part-way through a line, and that fragment is not a line: it is dropped.
std::vector<std::string> readTailLines(const fs::path& path, std::size_t cap) {
    std::vector<std::string> lines;
    std::ifstream in(path, std::ios::binary);
    if (!in) { return lines; }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size <= 0) { return lines; }
    const std::streamoff want = static_cast<std::streamoff>(cap);
    const std::streamoff start = size > want ? size - want : 0;
    in.seekg(start, std::ios::beg);
    std::string text(static_cast<std::size_t>(size - start), '\0');
    in.read(&text[0], static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(in.gcount()));

    std::size_t pos = 0;
    if (start > 0) {
        const std::size_t nl = text.find('\n');
        pos = nl == std::string::npos ? text.size() : nl + 1;
    }
    while (pos < text.size()) {
        std::size_t nl = text.find('\n', pos);
        const bool last = nl == std::string::npos;
        if (last) { nl = text.size(); }
        std::string line = text.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        if (!line.empty()) { lines.push_back(std::move(line)); }
        pos = nl + 1;
    }
    return lines;
}

std::string trimmed(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) { ++b; }
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) { --e; }
    return s.substr(b, e - b);
}

// Only the characters a version or a commit is made of, and not too many of them:
// a field read out of a file is a field, never a way to carry a sentence.
std::string versionToken(const std::string& v) {
    std::string out;
    for (const char c : v) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '-' ||
                        c == '+' || c == '_';
        if (!ok) { break; }
        out += c;
        if (out.size() >= 48) { break; }
    }
    return out;
}

// A signature is sixteen hex digits; anything else is not one.
std::string signatureToken(const std::string& v) {
    std::string out;
    for (const char c : v) {
        if (std::isxdigit(static_cast<unsigned char>(c)) == 0) { break; }
        out += c;
        if (out.size() >= 16) { break; }
    }
    return out;
}

// "0xC0000005": the prefix and hex digits, nothing after them.
std::string codeToken(const std::string& v) {
    if (v.size() < 3 || v[0] != '0' || (v[1] != 'x' && v[1] != 'X')) { return std::string(); }
    std::string out = "0x";
    for (std::size_t i = 2; i < v.size() && out.size() < 18; ++i) {
        if (std::isxdigit(static_cast<unsigned char>(v[i])) == 0) { break; }
        out += v[i];
    }
    return out.size() > 2 ? out : std::string();
}

// A reason is a sentence the application wrote ("access violation", "fault in a
// third-party SDR module, absorbed...", "SDR device enumeration child process
// died..."). Printable characters only, and capped: the upload sends the same
// line verbatim, and this is the shorter copy of it.
std::string reasonText(const std::string& v) {
    std::string out;
    for (const char c : v) {
        const unsigned char u = static_cast<unsigned char>(c);
        out += (u >= 0x20 && u < 0x7f) ? c : ' ';
        if (out.size() >= 120) { break; }
    }
    return trimmed(out);
}

bool parseInt(const std::string& v, std::int64_t& out) {
    if (v.empty()) { return false; }
    std::int64_t n = 0;
    for (const char c : v) {
        if (c < '0' || c > '9') { break; }
        n = n * 10 + (c - '0');
        if (n > 4000000000000ll) { return false; }
    }
    if (v[0] < '0' || v[0] > '9') { return false; }
    out = n;
    return true;
}

// The seconds since the epoch a file was last written; 0 when unknown.
std::int64_t fileEpoch(const fs::directory_entry& e) {
    std::error_code ec;
    const auto ft = e.last_write_time(ec);
    if (ec) { return 0; }
    const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    const std::time_t t = std::chrono::system_clock::to_time_t(sys);
    return t < 0 ? 0 : static_cast<std::int64_t>(t);
}

std::string readHead(const fs::path& path, std::size_t cap) {
    std::ifstream in(path, std::ios::binary);
    if (!in) { return std::string(); }
    std::string out(cap, '\0');
    in.read(&out[0], static_cast<std::streamsize>(cap));
    out.resize(static_cast<std::size_t>(in.gcount()));
    return out;
}

// What the upload sidecar says became of the report: its `status:` word, held to
// the words the uploader writes (core/crash_upload.cpp). A report nothing has
// swept yet - or from a machine where sending is off - has no sidecar: "none".
std::string uploadStatus(const fs::path& report) {
    const fs::path side = fs::path(report.string() + ".upload");
    std::error_code ec;
    if (!fs::is_regular_file(side, ec)) { return "none"; }
    const std::string text = readHead(side, 512);
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) { nl = text.size(); }
        const std::string line = trimmed(text.substr(pos, nl - pos));
        pos = nl + 1;
        if (line.rfind("status:", 0) != 0) { continue; }
        const std::string word = trimmed(line.substr(7));
        static const char* const kKnown[] = {"sent",      "duplicate", "local-only", "backoff",
                                             "rate-limited", "failed", "abandoned", "too-large",
                                             "expired",   "refused"};
        for (const char* k : kKnown) {
            if (word == k) { return word; }
        }
        return "other";
    }
    return "other";
}

// A report's head: the "name: value" lines before its first section, the
// `version:` line of the context block, and the `uptime-sec:` line of the process
// block. Everything else in the file - the stacks, the module list, the log - is
// left unread, and nothing from it is ever carried.
void parseReport(const std::string& text, ReportSummary& r) {
    enum class Part { Header, Context, Process, Other } part = Part::Header;
    bool sawKind = false;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) { nl = text.size(); }
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == '\r') { line.pop_back(); }
        if (line.rfind("--- ", 0) == 0) {
            if (line.rfind("--- context ---", 0) == 0) {
                part = Part::Context;
            } else if (line.rfind("--- process ---", 0) == 0) {
                part = Part::Process;
            } else {
                part = Part::Other;
            }
            continue;
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) { continue; }
        const std::string key = trimmed(line.substr(0, colon));
        const std::string value = trimmed(line.substr(colon + 1));
        std::int64_t n = 0;
        if (part == Part::Header) {
            if (key == "kind") {
                sawKind = true;
                r.kind = (value == "crash" || value == "hang" || value == "stall")
                             ? value
                             : std::string("unknown");
            } else if (key == "reason") {
                r.reason = reasonText(value);
            } else if (key == "code") {
                r.code = codeToken(value);
            } else if (key == "signature") {
                r.signature = signatureToken(value);
            } else if (key == "stalled-ms" && parseInt(value, n)) {
                r.stalledMs = n;
            }
        } else if (part == Part::Context) {
            if (key == "version" && r.version.empty()) { r.version = versionToken(value); }
        } else if (part == Part::Process) {
            if (key == "uptime-sec" && parseInt(value, n)) { r.uptimeSec = n; }
        }
    }
    if (!sawKind) { r.kind = "unknown"; }
}

std::string sessionBuild(const std::string& startLine) {
    const std::size_t at = startLine.find(" FoxSDR ");
    const std::size_t end = startLine.rfind(" starting");
    if (at == std::string::npos || end == std::string::npos || end <= at) { return std::string(); }
    return startLine.substr(at + 1, end - at - 1);
}

}  // namespace

bool isSessionStartLine(const std::string& line) {
    static const std::string kTail = ") starting";
    if (line.size() < kTail.size() + 12) { return false; }
    if (line.compare(line.size() - kTail.size(), kTail.size(), kTail) != 0) { return false; }
    const std::size_t at = line.find(" FoxSDR ");
    if (at == std::string::npos) { return false; }
    // "FoxSDR <version> (<commit>) starting": the version is one token.
    const std::size_t versionAt = at + 8;
    const std::size_t open = line.find(" (", versionAt);
    if (open == std::string::npos || open == versionAt) { return false; }
    return line.find(' ', versionAt) == open;
}

PreviousSessionLog readPreviousSessionLog(const std::string& logDir, std::size_t maxLines,
                                          std::size_t maxBytes) {
    PreviousSessionLog out;
    if (logDir.empty()) {
        out.reason = "no log folder is known";
        return out;
    }
    std::error_code ec;
    if (!fs::is_directory(fs::path(logDir), ec)) {
        out.reason = "the log folder does not exist, so nothing was written to it";
        return out;
    }

    // Oldest file first, so the lines run in the order they were written.
    std::vector<std::string> all;
    bool readAny = false;
    for (const std::string& name : logFileNamesOldestFirst()) {
        const fs::path p = fs::path(logDir) / name;
        if (!fs::is_regular_file(p, ec)) { continue; }
        readAny = true;
        std::vector<std::string> part = readTailLines(p, kLogFileReadBytes);
        all.insert(all.end(), std::make_move_iterator(part.begin()),
                   std::make_move_iterator(part.end()));
    }
    if (!readAny) {
        out.reason = "the log folder holds no log file";
        return out;
    }

    std::vector<std::size_t> starts;
    for (std::size_t i = 0; i < all.size(); ++i) {
        if (isSessionStartLine(all[i])) { starts.push_back(i); }
    }
    if (starts.empty()) {
        out.reason = "no session start line is in the log files";
        return out;
    }
    if (starts.size() < 2) {
        out.reason = "the log files hold no session before this one";
        return out;
    }

    // The NEWEST start line is this session; the one before it begins the session
    // that ended when this one started.
    const std::size_t from = starts[starts.size() - 2];
    const std::size_t to = starts[starts.size() - 1];
    out.found = true;
    out.build = sessionBuild(all[from]);
    out.sessionLines = to - from;

    // The newest lines that fit both bounds.
    std::size_t bytes = 0;
    std::size_t first = to;
    while (first > from && to - first < maxLines) {
        const std::size_t cost = all[first - 1].size() + 1;
        if (bytes + cost > maxBytes) { break; }
        bytes += cost;
        --first;
    }
    out.lines.assign(all.begin() + static_cast<std::ptrdiff_t>(first),
                     all.begin() + static_cast<std::ptrdiff_t>(to));
    return out;
}

std::string agoText(std::int64_t ageSec) {
    if (ageSec < 0) { return "(age unknown)"; }
    if (ageSec < 60) { return "just now"; }
    if (ageSec < 3600) { return std::to_string(ageSec / 60) + " min ago"; }
    if (ageSec < 86400) { return std::to_string(ageSec / 3600) + " h ago"; }
    return std::to_string(ageSec / 86400) + " d ago";
}

ReportListing listRecentReports(const std::string& crashDir, std::time_t now,
                                std::size_t maxListed) {
    ReportListing out;
    if (crashDir.empty()) { return out; }
    std::error_code ec;
    if (!fs::is_directory(fs::path(crashDir), ec)) { return out; }

    struct Found {
        fs::path path;
        std::int64_t epoch = 0;
    };
    std::vector<Found> found;
    std::size_t examined = 0;
    for (fs::directory_iterator it(fs::path(crashDir), ec), end; !ec && it != end;
         it.increment(ec)) {
        if (++examined > kExaminedReports) {
            out.totalCapped = true;
            break;
        }
        std::error_code fe;
        if (!it->is_regular_file(fe)) { continue; }
        const std::string name = it->path().filename().string();
        // crash-*.txt and hang-*.txt ONLY - the same two prefixes the uploader
        // opens. A .dmp is process memory, the .upload sidecars are read by name,
        // and diagnostics.txt is a bundle the user copied for themselves.
        if (name.size() < 5 || name.compare(name.size() - 4, 4, ".txt") != 0) { continue; }
        if (name.rfind("crash-", 0) != 0 && name.rfind("hang-", 0) != 0) { continue; }
        found.push_back({it->path(), fileEpoch(*it)});
    }
    out.readable = !ec || !found.empty();
    out.total = found.size();
    std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) {
        if (a.epoch != b.epoch) { return a.epoch > b.epoch; }
        return a.path.filename().string() > b.path.filename().string();
    });
    if (found.size() > maxListed) { found.resize(maxListed); }

    for (const Found& f : found) {
        ReportSummary r;
        r.ageSec = (f.epoch > 0 && static_cast<std::int64_t>(now) >= f.epoch)
                       ? static_cast<std::int64_t>(now) - f.epoch
                       : -1;
        parseReport(readHead(f.path, kReportReadBytes), r);
        r.upload = uploadStatus(f.path);
        out.newest.push_back(std::move(r));
    }
    return out;
}

std::string reportSummaryLine(const ReportSummary& r) {
    std::string s = r.kind.empty() ? std::string("unknown") : r.kind;
    s += ", " + agoText(r.ageSec);
    s += ", version " + (r.version.empty() ? std::string("unknown") : r.version);
    s += ", uptime " + (r.uptimeSec >= 0 ? std::to_string(r.uptimeSec) + " s" : std::string("unknown"));
    if (r.kind == "crash") {
        s += ", " + (r.reason.empty() ? std::string("reason not recorded") : r.reason);
        if (!r.code.empty()) { s += " (" + r.code + ")"; }
    } else if (r.kind == "hang" || r.kind == "stall") {
        s += r.stalledMs >= 0 ? ", stalled " + std::to_string(r.stalledMs) + " ms"
                              : std::string(", stalled for an unknown time");
    } else {
        s += ", not a readable report";
    }
    s += ", signature " + (r.signature.empty() ? std::string("none") : r.signature);
    s += ", upload " + (r.upload.empty() ? std::string("none") : r.upload);
    return s;
}

DiagHistory readDiagHistory(const std::string& logDir, const std::string& crashDir,
                            std::time_t now) {
    DiagHistory h;
    h.included = true;
    h.previous = readPreviousSessionLog(logDir);
    h.reports = listRecentReports(crashDir, now);
    return h;
}

// ---------------------------------------------------------------------------
// The cache
// ---------------------------------------------------------------------------

struct DiagHistoryCache::State {
    mutable std::mutex m;
    mutable std::condition_variable cv;
    bool running = false;
    bool have = false;
    DiagHistory result;
    std::int64_t takenAtMs = 0;
    std::size_t started = 0;
};

DiagHistoryCache::DiagHistoryCache() : state_(std::make_shared<State>()) {}

// A read still blocked in the filesystem is abandoned, not joined: the worker
// holds the shared state, so nothing it touches goes away under it.
DiagHistoryCache::~DiagHistoryCache() = default;

void DiagHistoryCache::refresh(const std::string& logDir, const std::string& crashDir,
                               std::int64_t nowMs, std::int64_t maxAgeMs) {
    PreviousSessionLog carried;
    bool haveCarried = false;
    {
        std::lock_guard<std::mutex> lk(state_->m);
        if (state_->running) { return; }
        if (state_->have && nowMs - state_->takenAtMs < maxAgeMs) { return; }
        state_->running = true;
        ++state_->started;
        // The previous session never changes: read once, carried afterwards.
        if (state_->have) {
            carried = state_->result.previous;
            haveCarried = true;
        }
    }
    std::shared_ptr<State> state = state_;
    try {
        std::thread([state, logDir, crashDir, nowMs, carried = std::move(carried),
                     haveCarried]() mutable {
            DiagHistory h;
            h.included = true;
            try {
                h.previous = haveCarried ? std::move(carried) : readPreviousSessionLog(logDir);
                h.reports = listRecentReports(crashDir, std::time(nullptr));
            } catch (...) {
                // A read that throws is a read that found nothing; the bundle says
                // so in words, and the next refresh tries again.
            }
            std::lock_guard<std::mutex> lk(state->m);
            state->result = std::move(h);
            state->have = true;
            state->takenAtMs = nowMs;
            state->running = false;
            state->cv.notify_all();
        }).detach();
    } catch (...) {
        std::lock_guard<std::mutex> lk(state->m);
        state->running = false;
        state->cv.notify_all();
    }
}

DiagHistory DiagHistoryCache::snapshot() const {
    std::lock_guard<std::mutex> lk(state_->m);
    if (state_->have) { return state_->result; }
    DiagHistory pending;
    pending.included = true;
    pending.pending = true;
    return pending;
}

bool DiagHistoryCache::ready() const {
    std::lock_guard<std::mutex> lk(state_->m);
    return state_->have;
}

bool DiagHistoryCache::waitIdle(std::chrono::milliseconds limit) const {
    std::unique_lock<std::mutex> lk(state_->m);
    return state_->cv.wait_for(lk, limit, [this] { return !state_->running; });
}

std::size_t DiagHistoryCache::readsStarted() const {
    std::lock_guard<std::mutex> lk(state_->m);
    return state_->started;
}

}  // namespace cascade::core
