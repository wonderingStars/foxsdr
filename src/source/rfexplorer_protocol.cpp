// rfexplorer_protocol.cpp - see rfexplorer_protocol.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/rfexplorer_protocol.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace cascade::source::rfexplorer {

namespace {

constexpr std::size_t kMaxBuffer = std::size_t{1} << 20;
constexpr std::size_t kMaxBytesPerCall = std::size_t{8} << 20;
constexpr long kMaxPoints = 65536;

std::string frameCommand(const std::string& body) {
    // '#' + one size byte (the whole message, the page: "Total size of the message in bytes") + the command
    std::string s;
    s += '#';
    s += static_cast<char>(body.size() + 2);
    s += body;
    return s;
}

// "-010", "0150", "  12": an optionally signed run of digits with optional surrounding blanks; false for anything else or an overlong run.
bool parseInt(const std::string& in, long long& out) {
    std::size_t b = 0, e = in.size();
    while (b < e && (in[b] == ' ' || in[b] == '\t')) { ++b; }
    while (e > b && (in[e - 1] == ' ' || in[e - 1] == '\t' || in[e - 1] == '\r')) { --e; }
    if (b == e) { return false; }
    bool neg = false;
    if (in[b] == '-' || in[b] == '+') { neg = in[b] == '-'; ++b; }
    if (b == e || e - b > 12) { return false; }
    long long v = 0;
    for (std::size_t i = b; i < e; ++i) {
        if (in[i] < '0' || in[i] > '9') { return false; }
        v = v * 10 + (in[i] - '0');
    }
    out = neg ? -v : v;
    return true;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> r;
    std::size_t at = 0;
    while (true) {
        const std::size_t k = s.find(sep, at);
        if (k == std::string::npos) { r.push_back(s.substr(at)); break; }
        r.push_back(s.substr(at, k - at));
        at = k + 1;
    }
    return r;
}

bool startsWith(const std::vector<std::uint8_t>& b, std::size_t at, const char* prefix) {
    const std::size_t n = std::strlen(prefix);
    return at + n <= b.size() && std::memcmp(b.data() + at, prefix, n) == 0;
}

bool isEolPair(std::uint8_t a, std::uint8_t b) { return (a == 0x0D && b == 0x0A) || (a == 0x0A && b == 0x0D); }

}  // namespace

// ---- commands -------------------------------------------------------------------------------------------------------------------------
std::string requestConfigCommand() { return frameCommand("C0"); }
std::string holdCommand() { return frameCommand("CH"); }
std::string dumpScreenOffCommand() { return frameCommand("D0"); }

std::string analyzerConfigCommand(double startHz, double endHz, int topDbm, int bottomDbm, std::string* whyNot) {
    auto no = [&](const char* why) {
        if (whyNot != nullptr) { *whyNot = why; }
        return std::string();
    };
    if (!(std::isfinite(startHz) && std::isfinite(endHz))) { return no("the start and stop frequencies must be numbers"); }
    const double sk = std::round(startHz / 1000.0);
    const double ek = std::round(endHz / 1000.0);
    if (sk < 1.0 || ek > static_cast<double>(kMaxFrequencyKHz)) { return no("the RF Explorer takes frequencies from 1 kHz to 9999999 kHz"); }
    if (!(sk < ek)) { return no("the stop frequency must be above the start frequency (in whole kHz)"); }
    if (topDbm > 0 || bottomDbm < -500 || !(bottomDbm < topDbm)) { return no("the amplitude range must be -500 dBm up to 0 dBm with the bottom below the top"); }
    char buf[48];
    std::snprintf(buf, sizeof buf, "C2-F:%07ld,%07ld,%04d,%04d", static_cast<long>(sk), static_cast<long>(ek), topDbm, bottomDbm);
    return frameCommand(buf);
}

// ---- parsing --------------------------------------------------------------------------------------------------------------------------
const char* mainModelName(int code) {
    switch (code) {
        case 0: return "433M";
        case 1: return "868M";
        case 2: return "915M";
        case 3: return "WSUB1G";
        case 4: return "2.4G";
        case 5: return "WSUB3G";
        case 6: return "6G";
        case 10: return "WSUB1G+";
        case 60: return "signal generator";
        default: return "unknown";
    }
}
bool isAnalyzerModel(int m) { return m == 0 || m == 1 || m == 2 || m == 3 || m == 4 || m == 5 || m == 6 || m == 10; }

bool parseSetup(const std::string& text, Setup& out, std::string* why) {
    auto no = [&](const char* w) {
        if (why != nullptr) { *why = w; }
        return false;
    };
    const std::vector<std::string> f = split(text, ',');
    if (f.size() != 3) { return no("the setup message does not have three fields"); }
    long long m = 0, x = 0;
    if (!parseInt(f[0], m) || !parseInt(f[1], x) || m < 0 || m > 255 || x < 0 || x > 255) { return no("the setup message has a model that is not a number"); }
    std::string fw;
    for (char c : f[2]) {
        if (c == '\r' || c == '\n') { break; }
        fw += (c >= 0x20 && c < 0x7F) ? c : '?';
        if (fw.size() >= 8) { break; }
    }
    out.mainModel = static_cast<int>(m);
    out.expansionModel = static_cast<int>(x);
    out.firmware = fw;
    return true;
}

bool parseConfig(const std::string& text, Config& out, std::string* why) {
    auto no = [&](const char* w) {
        if (why != nullptr) { *why = w; }
        return false;
    };
    const std::vector<std::string> f = split(text, ',');
    if (f.size() != 10 && f.size() != 11 && f.size() != 13) { return no("the configuration message has a number of fields no firmware of the page sends"); }
    long long v[13] = {};
    for (std::size_t i = 0; i < f.size(); ++i) {
        if (!parseInt(f[i], v[i])) { return no("the configuration message has a field that is not a number"); }
    }
    Config c;
    c.fields = static_cast<int>(f.size());
    c.startKHz = static_cast<long>(v[0]);
    c.stepHz = static_cast<long>(v[1]);
    c.ampTop = static_cast<int>(v[2]);
    c.ampBottom = static_cast<int>(v[3]);
    c.points = static_cast<long>(v[4]);
    c.expansionActive = static_cast<int>(v[5]);
    c.mode = static_cast<int>(v[6]);
    c.minKHz = static_cast<long>(v[7]);
    c.maxKHz = static_cast<long>(v[8]);
    c.maxSpanKHz = static_cast<long>(v[9]);
    c.rbwKHz = f.size() >= 11 ? static_cast<long>(v[10]) : -1;
    if (c.startKHz < 0 || c.startKHz > kMaxFrequencyKHz || c.stepHz < 1 || c.stepHz > 100000000L || c.points < 2 || c.points > kMaxPoints ||
        c.minKHz < 0 || c.maxKHz < c.minKHz || c.maxKHz > kMaxFrequencyKHz || c.maxSpanKHz < 0 || c.ampTop < -1000 || c.ampTop > 1000 ||
        c.ampBottom < -1000 || c.ampBottom > 1000) {
        return no("the configuration message has a value outside any range the page describes");
    }
    out = c;
    return true;
}

// ---- the stream -----------------------------------------------------------------------------------------------------------------------
void Demux::reset() {
    buf_.clear();
    points_ = 0;
    stats_ = DemuxStats{};
}

void Demux::feed(const std::uint8_t* data, std::size_t n, std::vector<Item>& out) {
    if (data == nullptr || n == 0) { return; }
    buf_.insert(buf_.end(), data, data + n);
    std::size_t p = 0;
    const std::size_t size = buf_.size();
    while (p < size) {
        const std::uint8_t c = buf_[p];
        if (c != '#' && c != '$') {
            ++p;
            ++stats_.discardedBytes;
            continue;
        }
        const std::size_t avail = size - p;
        if (c == '#') {
            // Only the message starts of the page are taken as a line ("#C2-F:", "#C2-M:", "#C3-", "#C5-", "#Sn", "#K", "#T:"); any other '#' is noise,
            // so a stray one cannot swallow the next real line.
            int cls = 0;  // 0 = noise, 1 = could still match (too few bytes), 2 = a message start
            for (const char* cand : {"#C2-F:", "#C2-M:", "#C3-", "#C5-", "#Sn", "#K", "#T:"}) {
                const std::size_t len = std::strlen(cand);
                const std::size_t cmp = std::min(avail, len);
                if (std::memcmp(&buf_[p], cand, cmp) == 0) { cls = std::max(cls, cmp == len ? 2 : 1); }
            }
            if (cls == 0) {
                ++p;
                ++stats_.discardedBytes;
                continue;
            }
            if (cls == 1) { break; }
            const std::size_t scan = std::min(avail, kMaxTextLine);
            std::size_t nl = std::string::npos;
            for (std::size_t i = 1; i < scan; ++i) {
                if (buf_[p + i] == '\n') { nl = p + i; break; }
            }
            if (nl == std::string::npos) {
                if (avail >= kMaxTextLine) {  // not a line: a '#' inside noise
                    ++p;
                    ++stats_.discardedBytes;
                    continue;
                }
                break;  // wait for the rest of the line
            }
            std::string line(reinterpret_cast<const char*>(&buf_[p]), nl - p);
            while (!line.empty() && line.back() == '\r') { line.pop_back(); }
            if (line.compare(0, 6, "#C2-M:") == 0) {
                Item it;
                it.kind = ItemKind::Setup;
                if (parseSetup(line.substr(6), it.setup)) { out.push_back(std::move(it)); } else { ++stats_.badConfigs; }
            } else if (line.compare(0, 6, "#C2-F:") == 0) {
                Item it;
                it.kind = ItemKind::Config;
                if (parseConfig(line.substr(6), it.config)) {
                    points_ = it.config.points;
                    out.push_back(std::move(it));
                } else {
                    ++stats_.badConfigs;
                }
            } else if (line.compare(0, 4, "#C3-") == 0 || line.compare(0, 4, "#C5-") == 0) {
                ++stats_.generatorMessages;
            } else {
                ++stats_.ignoredLines;
            }
            p = nl + 1;
            if (p < size && buf_[p] == '\r') { ++p; }  // the page writes the end of line both ways round
            continue;
        }
        // '$'
        if (avail < 2) { break; }
        const std::uint8_t kind = buf_[p + 1];
        if (kind == 'D') {
            if (avail < 2 + kScreenDumpBytes) { break; }
            p += 2 + kScreenDumpBytes;
            for (int k = 0; k < 2 && p < size && (buf_[p] == 0x0D || buf_[p] == 0x0A); ++k) { ++p; }  // its end of line, when it is there already
            ++stats_.screenDumps;
            continue;
        }
        if (kind != 'S' && kind != 's' && kind != 'z') {
            ++p;
            ++stats_.discardedBytes;
            continue;
        }
        if (points_ == 0) {  // no configuration seen: the frame cannot be sized
            ++p;
            ++stats_.discardedBytes;
            continue;
        }
        const std::size_t hdr = kind == 'z' ? 4 : 3;
        if (avail < hdr) { break; }
        bool match = false;
        if (kind == 'S') {
            const long b = buf_[p + 2];
            match = b == points_ || (b + 1) * 16 == points_;  // the 2013 client: the count; the page: (count + 1) * 16
        } else if (kind == 's') {
            match = (static_cast<long>(buf_[p + 2]) + 1) * 16 == points_;
        } else {
            match = ((static_cast<long>(buf_[p + 2]) << 8) | buf_[p + 3]) == points_;
        }
        if (!match) {
            ++stats_.badFrames;
            ++p;
            continue;
        }
        const std::size_t need = hdr + static_cast<std::size_t>(points_) + 2;
        if (avail < need) { break; }
        if (!isEolPair(buf_[p + need - 2], buf_[p + need - 1])) {
            ++stats_.badFrames;
            ++p;
            continue;
        }
        Item it;
        it.kind = ItemKind::Sweep;
        it.frame = static_cast<char>(kind);
        it.dbm.resize(static_cast<std::size_t>(points_));
        for (long i = 0; i < points_; ++i) { it.dbm[static_cast<std::size_t>(i)] = dbmOfByte(buf_[p + hdr + static_cast<std::size_t>(i)]); }
        out.push_back(std::move(it));
        ++stats_.sweeps;
        p += need;
    }
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(p));
    if (buf_.size() > kMaxBuffer) {  // an unfinished frame that never ends
        stats_.discardedBytes += buf_.size();
        buf_.clear();
    }
}

// ---- the sweeper ----------------------------------------------------------------------------------------------------------------------
const char* resultWord(Result r) {
    switch (r) {
        case Result::Ok: return "ok";
        case Result::BadRequest: return "bad-request";
        case Result::WriteFailed: return "write-failed";
        case Result::LinkFailed: return "link-failed";
        case Result::TimedOut: return "timed-out";
        case Result::NotAnalyzer: return "not-analyzer";
        case Result::Refused: return "refused";
    }
    return "unknown";
}

void Sweeper::absorb(const std::vector<Item>& items) {
    for (const Item& it : items) {
        switch (it.kind) {
            case ItemKind::Setup:
                setup_ = it.setup;
                haveSetup_ = true;
                break;
            case ItemKind::Config:
                config_ = it.config;
                haveConfig_ = true;
                ++configsSeen_;
                pending_.clear();  // sweeps taken before this configuration belong to the old span
                break;
            case ItemKind::Sweep:
                if (pending_.size() < 64) { pending_.push_back(it); }
                break;
        }
    }
}

Result Sweeper::pump(int maxIdleReads, bool wantSweep, std::uint64_t configsBefore, std::string* why) {
    std::vector<std::uint8_t> buf(4096);
    std::vector<Item> items;
    int idle = 0;
    std::size_t total = 0;
    while (true) {
        if (wantSweep && !pending_.empty()) { return Result::Ok; }
        if (!wantSweep && configsSeen_ > configsBefore) { return Result::Ok; }
        const long r = link_.read(buf.data(), buf.size());
        if (r < 0) {
            if (why != nullptr) { *why = "the serial port failed (the RF Explorer was unplugged or the port closed)"; }
            return Result::LinkFailed;
        }
        if (r == 0) {
            if (++idle > maxIdleReads) {
                if (why != nullptr) { *why = "the RF Explorer did not answer in time"; }
                return Result::TimedOut;
            }
            continue;
        }
        idle = 0;
        total += static_cast<std::size_t>(r);
        if (total > kMaxBytesPerCall) {
            if (why != nullptr) { *why = "the serial port delivered far more than any RF Explorer message and no usable one"; }
            return Result::TimedOut;
        }
        items.clear();
        demux_.feed(buf.data(), static_cast<std::size_t>(r), items);
        absorb(items);
    }
}

Result Sweeper::connect(int maxIdleReads, std::string* why) {
    demux_.reset();
    pending_.clear();
    haveConfig_ = haveSetup_ = false;
    const std::string dump = dumpScreenOffCommand();
    const std::string ask = requestConfigCommand();
    if (!link_.write(reinterpret_cast<const std::uint8_t*>(dump.data()), dump.size()) ||
        !link_.write(reinterpret_cast<const std::uint8_t*>(ask.data()), ask.size())) {
        if (why != nullptr) { *why = "the serial port would not take the request"; }
        return Result::WriteFailed;
    }
    const Result r = pump(maxIdleReads, false, configsSeen_, why);
    if (r != Result::Ok) {
        if (r == Result::TimedOut) {
            if (demux_.stats().generatorMessages > 0) {
                if (why != nullptr) { *why = "this is an RF Explorer signal generator; FoxSDR uses the spectrum analyser models"; }
                return Result::NotAnalyzer;
            }
            if (why != nullptr) {
                *why = "no RF Explorer answered: check the port, that it is switched on and that its baud rate is the default (500000)";
            }
            return Result::NotAnalyzer;
        }
        return r;
    }
    if ((haveSetup_ && !isAnalyzerModel(setup_.mainModel)) || demux_.stats().generatorMessages > 0) {
        if (why != nullptr) { *why = "this is an RF Explorer signal generator; FoxSDR uses the spectrum analyser models"; }
        return Result::NotAnalyzer;
    }
    if (config_.mode != 0) {
        if (why != nullptr) { *why = "the RF Explorer is not in spectrum analyser mode; switch it to the spectrum analyser on the device"; }
        return Result::NotAnalyzer;
    }
    return Result::Ok;
}

Result Sweeper::configure(const Request& req, std::string* why) {
    if (!haveConfig_) {
        if (why != nullptr) { *why = "the RF Explorer has not been connected"; }
        return Result::BadRequest;
    }
    std::string w;
    const std::string cmd = analyzerConfigCommand(req.startHz, req.stopHz, req.topDbm, req.bottomDbm, &w);
    if (cmd.empty()) {
        if (why != nullptr) { *why = w; }
        return Result::BadRequest;
    }
    const long sk = static_cast<long>(std::lround(req.startHz / 1000.0));
    const long ek = static_cast<long>(std::lround(req.stopHz / 1000.0));
    if ((config_.minKHz > 0 && sk < config_.minKHz) || (config_.maxKHz > 0 && ek > config_.maxKHz)) {
        if (why != nullptr) {
            *why = "that range is outside what this RF Explorer covers (" + std::to_string(config_.minKHz) + " to " + std::to_string(config_.maxKHz) + " kHz)";
        }
        return Result::BadRequest;
    }
    if (config_.maxSpanKHz > 0 && ek - sk > config_.maxSpanKHz) {
        if (why != nullptr) { *why = "that span is wider than this RF Explorer's maximum of " + std::to_string(config_.maxSpanKHz) + " kHz"; }
        return Result::BadRequest;
    }
    if (!link_.write(reinterpret_cast<const std::uint8_t*>(cmd.data()), cmd.size())) {
        if (why != nullptr) { *why = "the serial port would not take the new range"; }
        return Result::WriteFailed;
    }
    // The device answers with its configuration; sweeps still in flight from the old span come first and are dropped when it arrives.
    for (int attempt = 0; attempt < 8; ++attempt) {
        const Result r = pump(req.maxIdleReads, false, configsSeen_, why);
        if (r == Result::TimedOut) {
            if (why != nullptr) { *why = "the RF Explorer did not take that range"; }
            return Result::Refused;
        }
        if (r != Result::Ok) { return r; }
        if (std::labs(config_.startKHz - sk) <= 1) {
            pending_.clear();
            return Result::Ok;
        }
    }
    if (why != nullptr) { *why = "the RF Explorer did not take that range"; }
    return Result::Refused;
}

Result Sweeper::nextLine(core::SweepLine& out, double timeS, int maxIdleReads, std::string* why) {
    if (!haveConfig_) {
        if (why != nullptr) { *why = "the RF Explorer has not been connected"; }
        return Result::BadRequest;
    }
    const Result r = pump(maxIdleReads, true, 0, why);
    if (r != Result::Ok) { return r; }
    Item it = std::move(pending_.front());
    pending_.erase(pending_.begin());
    core::SweepLine line;
    line.startHz = static_cast<double>(config_.startKHz) * 1000.0;
    line.stopHz = line.startHz + static_cast<double>(config_.stepHz) * static_cast<double>(it.dbm.size() - 1);
    line.db = std::move(it.dbm);
    line.timeS = timeS;
    const std::string problem = core::sweepLineProblem(line);
    if (!problem.empty()) {
        if (why != nullptr) { *why = "the sweep is not usable: " + problem; }
        return Result::Refused;
    }
    out = std::move(line);
    return Result::Ok;
}

void Sweeper::hold() {
    const std::string h = holdCommand();
    link_.write(reinterpret_cast<const std::uint8_t*>(h.data()), h.size());
}

}  // namespace cascade::source::rfexplorer
