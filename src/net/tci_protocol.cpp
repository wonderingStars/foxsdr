// See tci_protocol.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/tci_protocol.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

#include "net/cat_protocol.hpp"

namespace cascade::net {
namespace {

std::string lowered(std::string_view s) {
    std::string r(s);
    for (char& c : r) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return r;
}

bool nameChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

FoxCommand mk(std::uint32_t op, std::uint32_t target) {
    FoxCommand c{};
    c.structSize = sizeof(c);
    c.op = op;
    c.target = target;
    return c;
}

// The default number of samples per packet for a rate (3.4 of the specification's AUDIO_STREAM_SAMPLES table).
int defaultSamples(int rate) {
    switch (rate) {
        case 24000: return 1024;
        case 12000: return 512;
        case 8000: return 256;
        default: return 2048;
    }
}

struct ModeName { const char* name; std::uint32_t demod; };
// The names the specification itself uses are AM, LSB, USB, CW and NFM (and "FM" in an example list). WFM and DSB are FoxSDR's two
// other demodulators: the specification names no wide-FM or double-sideband mode, so these two words are FoxSDR's own choice (UNVERIFIED
// against any client: a client that does not know them ignores them).
const ModeName kModes[] = {{"AM", FOXAPI_DEMOD_AM}, {"LSB", FOXAPI_DEMOD_LSB}, {"USB", FOXAPI_DEMOD_USB}, {"CW", FOXAPI_DEMOD_CW},
                           {"NFM", FOXAPI_DEMOD_NFM}, {"WFM", FOXAPI_DEMOD_WFM}, {"DSB", FOXAPI_DEMOD_DSB}};

void putLe32(std::string& s, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) { s.push_back(static_cast<char>((v >> (8 * i)) & 0xFFu)); }
}

}  // namespace

TciParse parseTciMessage(std::string_view text) {
    TciParse out;
    if (text.size() > kMaxTciTextBytes) {
        out.oversize = true;
        return out;
    }
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t semi = text.find(';', pos);
        const bool terminated = semi != std::string_view::npos;
        if (!terminated) { semi = text.size(); }
        std::string_view body = text.substr(pos, semi - pos);
        pos = semi + 1;
        // Trim the whitespace some clients put between commands.
        while (!body.empty() && std::isspace(static_cast<unsigned char>(body.front())) != 0) { body.remove_prefix(1); }
        while (!body.empty() && std::isspace(static_cast<unsigned char>(body.back())) != 0) { body.remove_suffix(1); }
        if (body.empty()) { continue; }
        if (!terminated) { ++out.dropped; break; }   // a command without its ';' is not a command (3.1)
        if (out.commands.size() >= kMaxTciCommandsPerMessage) { ++out.dropped; continue; }
        const std::size_t colon = body.find(':');
        std::string_view name = body.substr(0, colon);
        while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back())) != 0) { name.remove_suffix(1); }   // "vfo : 0;" is VFO
        bool ok = !name.empty() && name.size() <= kMaxTciNameBytes;
        for (char c : name) { ok = ok && nameChar(c); }
        if (!ok) { ++out.dropped; continue; }
        TciCommand cmd;
        cmd.name = lowered(name);
        if (colon != std::string_view::npos) {
            std::string_view rest = body.substr(colon + 1);
            std::size_t p = 0;
            while (p <= rest.size()) {
                std::size_t comma = rest.find(',', p);
                if (comma == std::string_view::npos) { comma = rest.size(); }
                std::string_view a = rest.substr(p, comma - p);
                while (!a.empty() && std::isspace(static_cast<unsigned char>(a.front())) != 0) { a.remove_prefix(1); }
                while (!a.empty() && std::isspace(static_cast<unsigned char>(a.back())) != 0) { a.remove_suffix(1); }
                if (!a.empty()) {   // empty arguments are discarded (4.7 note 1)
                    if (a.size() > kMaxTciArgBytes || cmd.args.size() >= kMaxTciArgs) { ok = false; break; }
                    cmd.args.emplace_back(a);
                }
                p = comma + 1;
            }
        }
        if (!ok) { ++out.dropped; continue; }
        out.commands.push_back(std::move(cmd));
    }
    return out;
}

bool tciParseInteger(const std::string& s, std::int64_t& out) {
    std::size_t i = 0;
    bool neg = false;
    if (i < s.size() && s[i] == '-') { neg = true; ++i; }
    if (i >= s.size() || s.size() - i > 15) { return false; }
    std::int64_t v = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') { return false; }
        v = v * 10 + (s[i] - '0');
    }
    out = neg ? -v : v;
    return true;
}

bool tciParseBool(const std::string& s) {
    const std::string l = lowered(s);
    return l == "true" || l == "1";
}

const char* tciModeName(std::uint32_t foxDemod) {
    for (const ModeName& m : kModes) {
        if (m.demod == foxDemod) { return m.name; }
    }
    return "";
}

bool tciModeFromName(const std::string& name, std::uint32_t& foxDemod) {
    std::string up = name;
    for (char& c : up) { c = static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }
    if (up == "FM") { up = "NFM"; }   // the specification's example list says FM
    for (const ModeName& m : kModes) {
        if (up == m.name) { foxDemod = m.demod; return true; }
    }
    return false;
}

TciDecimator::TciDecimator(int factor) : factor_(factor < 1 ? 1 : factor) {
    if (factor_ > 1) {
        const int n = 32 * factor_ + 1;
        const double fc = 0.45 / factor_;   // cycles per input sample: 0.9 of the new Nyquist
        const double pi = 3.14159265358979323846;
        double sum = 0.0;
        taps_.resize(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            const double x = i - (n - 1) / 2.0;
            const double sinc = x == 0.0 ? 2.0 * fc : std::sin(2.0 * pi * fc * x) / (pi * x);
            const double w = 0.54 - 0.46 * std::cos(2.0 * pi * i / (n - 1));
            taps_[static_cast<std::size_t>(i)] = static_cast<float>(sinc * w);
            sum += sinc * w;
        }
        for (float& t : taps_) { t = static_cast<float>(t / sum); }
        hist_.assign(taps_.size() - 1, 0.0f);
    }
}

void TciDecimator::reset() {
    std::fill(hist_.begin(), hist_.end(), 0.0f);
    phase_ = 0;
}

void TciDecimator::process(const float* in, std::size_t n, std::vector<float>& out) {
    if (factor_ == 1) {
        out.insert(out.end(), in, in + n);
        return;
    }
    std::vector<float> buf;
    buf.reserve(hist_.size() + n);
    buf.insert(buf.end(), hist_.begin(), hist_.end());
    buf.insert(buf.end(), in, in + n);
    const std::size_t taps = taps_.size();
    for (std::size_t i = 0; i < n; ++i) {
        if (phase_++ % factor_ != 0) { continue; }
        double acc = 0.0;
        for (std::size_t k = 0; k < taps; ++k) { acc += static_cast<double>(taps_[k]) * buf[i + taps - 1 - k]; }
        out.push_back(static_cast<float>(acc));
    }
    phase_ %= factor_;
    hist_.assign(buf.end() - static_cast<std::ptrdiff_t>(hist_.size()), buf.end());
}

std::string tciPackAudio(std::uint32_t receiver, std::uint32_t sampleRate, TciSampleType type, std::uint32_t channels,
                         std::uint32_t count, const float* interleaved, std::size_t frames) {
    const std::size_t values = frames * channels;
    std::string p;
    p.reserve(static_cast<std::size_t>(kTciHeaderBytes) + values * 4);
    putLe32(p, receiver);
    putLe32(p, sampleRate);
    putLe32(p, static_cast<std::uint32_t>(type));
    putLe32(p, 0);                                          // codec: not implemented, always 0
    putLe32(p, 0);                                          // crc: not implemented, always 0
    putLe32(p, static_cast<std::uint32_t>(values));         // length: the number of samples in data[] (see the header comment of tci_protocol)
    putLe32(p, kTciRxAudioStream);
    putLe32(p, channels);
    putLe32(p, count);
    for (int i = 0; i < 7; ++i) { putLe32(p, 0); }          // reserv[7]
    for (std::size_t i = 0; i < values; ++i) {
        float v = interleaved[i];
        if (!(v == v)) { v = 0.0f; }
        switch (type) {
            case TciSampleType::Int16: {
                const double c = std::max(-1.0, std::min(1.0, static_cast<double>(v)));
                const auto s = static_cast<std::int16_t>(std::lround(c * 32767.0));
                p.push_back(static_cast<char>(s & 0xFF));
                p.push_back(static_cast<char>((s >> 8) & 0xFF));
                break;
            }
            case TciSampleType::Int24: {
                const double c = std::max(-1.0, std::min(1.0, static_cast<double>(v)));
                const auto s = static_cast<std::int32_t>(std::lround(c * 8388607.0));
                p.push_back(static_cast<char>(s & 0xFF));
                p.push_back(static_cast<char>((s >> 8) & 0xFF));
                p.push_back(static_cast<char>((s >> 16) & 0xFF));
                break;
            }
            case TciSampleType::Int32: {
                const double c = std::max(-1.0, std::min(1.0, static_cast<double>(v)));
                const auto s = static_cast<std::int32_t>(std::llround(c * 2147483647.0));
                putLe32(p, static_cast<std::uint32_t>(s));
                break;
            }
            case TciSampleType::Float32: {
                std::uint32_t bits;
                std::memcpy(&bits, &v, 4);
                putLe32(p, bits);
                break;
            }
        }
    }
    return p;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// the connection

std::string TciConnection::num(double v) { return std::to_string(static_cast<long long>(std::llround(v))); }

int TciConnection::audioSamples() const { return samples_ != 0 ? samples_ : defaultSamples(rate_); }

void TciConnection::refreshRadios() { radios_ = be_.radios(); }

int TciConnection::radioOf(const std::string& arg) const {
    std::int64_t i = 0;
    if (!tciParseInteger(arg, i) || i < 0 || i >= static_cast<std::int64_t>(radios_.size())) { return 0; }
    return radios_[static_cast<std::size_t>(i)];
}

int TciConnection::indexOf(int radio) const {
    for (std::size_t i = 0; i < radios_.size(); ++i) {
        if (radios_[i] == radio) { return static_cast<int>(i); }
    }
    return -1;
}

void TciConnection::emitGroup(int radio, const TciRadioView& v, unsigned groups, std::vector<std::string>& out) const {
    const int i = indexOf(radio);
    if (i < 0) { return; }
    const std::string rx = std::to_string(i);
    if ((groups & kDds) != 0) { out.push_back("DDS:" + rx + "," + num(v.centreHz) + ";"); }
    if ((groups & kIf) != 0) { out.push_back("IF:" + rx + ",0," + num(v.vfoOffsetHz) + ";"); }
    if ((groups & kVfo) != 0) { out.push_back("VFO:" + rx + ",0," + num(v.tunedHz) + ";"); }
    if ((groups & kMode) != 0) {
        const char* m = tciModeName(v.demod);
        if (m[0] != '\0') {
            out.push_back("MODULATION:" + rx + "," + m + ";");
            double lo = -v.bandwidthHz / 2.0, hi = v.bandwidthHz / 2.0;
            if (v.demod == FOXAPI_DEMOD_USB) { lo = 0.0; hi = v.bandwidthHz; }
            if (v.demod == FOXAPI_DEMOD_LSB) { lo = -v.bandwidthHz; hi = 0.0; }
            out.push_back("RX_FILTER_BAND:" + rx + "," + num(lo) + "," + num(hi) + ";");
        }
    }
    if ((groups & kRun) != 0) { out.push_back("RX_ENABLE:" + rx + "," + (v.running ? "true" : "false") + ";"); }
    if ((groups & kSql) != 0) {
        out.push_back("SQL_ENABLE:" + rx + "," + (v.squelchDb > -199.5 ? "true" : "false") + ";");
        out.push_back("SQL_LEVEL:" + rx + "," + num(v.squelchDb > -199.5 ? v.squelchDb : -140.0) + ";");
    }
}

std::vector<std::string> TciConnection::begin() {
    std::vector<std::string> out;
    refreshRadios();
    double lo = 0.0, hi = 0.0, rate = 0.0;
    bool anyRun = false;
    std::vector<std::pair<int, TciRadioView>> views;
    for (const int r : radios_) {
        TciRadioView v;
        if (!be_.readRadio(r, v)) { continue; }
        views.emplace_back(r, v);
        if (v.limitHiHz > v.limitLoHz) {
            lo = lo == 0.0 ? v.limitLoHz : std::min(lo, v.limitLoHz);
            hi = std::max(hi, v.limitHiHz);
        }
        anyRun = anyRun || v.running;
    }
    if (!views.empty()) { rate = views.front().second.sampleRateHz; }
    if (hi <= lo) { lo = 1.0; hi = 6.0e9; }   // no radio reports a range: what the engine accepts
    out.push_back("VFO_LIMITS:" + num(lo) + "," + num(hi) + ";");
    out.push_back("IF_LIMITS:" + num(-rate / 2.0) + "," + num(rate / 2.0) + ";");
    ifLimitSent_ = std::llround(rate);
    out.push_back("TRX_COUNT:" + std::to_string(radios_.size()) + ";");
    out.push_back("CHANNELS_COUNT:1;");
    out.push_back("DEVICE:FoxSDR;");
    out.push_back("RECEIVE_ONLY:true;");
    std::string mods = "MODULATIONS_LIST:";
    bool first = true;
    for (const ModeName& m : kModes) {
        mods += (first ? "" : ",") + std::string(m.name);
        first = false;
    }
    out.push_back(mods + ";");
    out.push_back("PROTOCOL:FoxSDR,2.1;");
    out.push_back("READY;");
    out.push_back(anyRun ? "START;" : "STOP;");
    anyRunningSent_ = anyRun;
    anyRunningKnown_ = true;
    for (const auto& [r, v] : views) {
        emitGroup(r, v, kAll, out);
        const std::string rx = std::to_string(indexOf(r));
        out.push_back("TRX:" + rx + ",false;");
        out.push_back("TX_ENABLE:" + rx + ",false;");
        Sent s;
        s.centre = std::llround(v.centreHz); s.offset = std::llround(v.vfoOffsetHz); s.tuned = std::llround(v.tunedHz);
        s.bandwidth = std::llround(v.bandwidthHz); s.squelch = std::llround(v.squelchDb); s.rate = std::llround(v.sampleRateHz);
        s.demod = v.demod; s.running = v.running;
        last_[r] = s;
    }
    return out;
}

std::vector<std::string> TciConnection::onText(std::string_view text) {
    std::vector<std::string> replies;
    std::vector<FoxCommand> cmds;
    const TciParse p = parseTciMessage(text);
    if (p.oversize) { ++c_.oversize; return replies; }
    c_.dropped += static_cast<std::uint64_t>(p.dropped);
    refreshRadios();
    for (const TciCommand& cmd : p.commands) {
        ++c_.commands;
        dispatch(cmd, replies, cmds);
    }
    if (!cmds.empty()) {
        const int n = be_.submit(cmds);
        c_.submitted += cmds.size();
        if (n > 0) { outstanding_ += static_cast<std::size_t>(n); }
        lastSubmitMs_ = now_;
    }
    return replies;
}

void TciConnection::dispatch(const TciCommand& cmd, std::vector<std::string>& replies, std::vector<FoxCommand>& cmds) {
    const std::string& n = cmd.name;
    const std::vector<std::string>& a = cmd.args;
    const auto ignore = [&] { ++c_.ignored; };
    // The receiver named by the first argument; 0 when it is missing or out of range (4.7 note 5: ignored).
    const auto rxArg = [&]() -> int { return a.empty() ? 0 : radioOf(a[0]); };

    if (n == "start" || n == "stop") {
        FoxCommand c = mk(FOXAPI_OP_RUN, FOXAPI_TARGET_ALL_RADIOS);
        c.ival[0] = n == "start" ? 1 : 0;
        cmds.push_back(c);
        for (const int r : radios_) { force(r, kRun); }
        anyRunningKnown_ = false;
        return;
    }
    if (n == "trx" || n == "tune" || n == "tx_enable") {
        // RECEIVE-ONLY. Whatever is asked, the answer is "false": no transmit request ever becomes an engine command.
        const int r = rxArg();
        if (r == 0) { ignore(); return; }
        if (a.size() >= 2 && tciParseBool(a[1])) { ++c_.txRefused; }
        const std::string name = n == "tx_enable" ? "TX_ENABLE" : n == "tune" ? "TUNE" : "TRX";
        replies.push_back(name + ":" + std::to_string(indexOf(r)) + ",false;");
        return;
    }
    if (n == "vfo") {
        const int r = rxArg();
        std::int64_t ch = 0;
        if (r == 0 || a.size() < 2 || !tciParseInteger(a[1], ch) || ch != 0) { ignore(); return; }
        force(r, kVfo);
        std::int64_t hz = 0;
        if (a.size() >= 3 && tciParseInteger(a[2], hz) && hz > 0) {
            TciRadioView v;
            if (!be_.readRadio(r, v)) { return; }
            const TunePlan plan = planTune(static_cast<double>(hz), v.centreHz, v.sampleRateHz);
            if (plan.retune) {
                FoxCommand c = mk(FOXAPI_OP_SET_CENTRE, static_cast<std::uint32_t>(r));
                c.num[0] = plan.centerHz;
                cmds.push_back(c);
            }
            FoxCommand o = mk(FOXAPI_OP_SET_VFO_OFFSET, static_cast<std::uint32_t>(r));
            o.num[0] = plan.vfoOffsetHz;
            cmds.push_back(o);
        }
        return;
    }
    if (n == "dds") {
        const int r = rxArg();
        if (r == 0) { ignore(); return; }
        force(r, kDds);
        std::int64_t hz = 0;
        if (a.size() >= 2 && tciParseInteger(a[1], hz) && hz > 0) {
            FoxCommand c = mk(FOXAPI_OP_SET_CENTRE, static_cast<std::uint32_t>(r));
            c.num[0] = static_cast<double>(hz);
            cmds.push_back(c);
        }
        return;
    }
    if (n == "if") {
        const int r = rxArg();
        std::int64_t ch = 0;
        if (r == 0 || a.size() < 2 || !tciParseInteger(a[1], ch) || ch != 0) { ignore(); return; }
        force(r, kIf);
        std::int64_t hz = 0;
        if (a.size() >= 3 && tciParseInteger(a[2], hz)) {
            FoxCommand c = mk(FOXAPI_OP_SET_VFO_OFFSET, static_cast<std::uint32_t>(r));
            c.num[0] = static_cast<double>(hz);
            cmds.push_back(c);
        }
        return;
    }
    if (n == "modulation") {
        const int r = rxArg();
        if (r == 0) { ignore(); return; }
        force(r, kMode);
        std::uint32_t demod = 0;
        if (a.size() >= 2 && tciModeFromName(a[1], demod)) {
            FoxCommand c = mk(FOXAPI_OP_SET_MODE, static_cast<std::uint32_t>(r));
            c.ival[0] = demod;
            cmds.push_back(c);
        }
        return;
    }
    if (n == "rx_enable") {
        const int r = rxArg();
        if (r == 0) { ignore(); return; }
        force(r, kRun);
        if (a.size() >= 2) {
            FoxCommand c = mk(FOXAPI_OP_RUN, static_cast<std::uint32_t>(r));
            c.ival[0] = tciParseBool(a[1]) ? 1 : 0;
            cmds.push_back(c);
        }
        return;
    }
    if (n == "rx_filter_band") {
        const int r = rxArg();
        if (r == 0) { ignore(); return; }
        force(r, kMode);
        std::int64_t lo = 0, hi = 0;
        if (a.size() >= 3 && tciParseInteger(a[1], lo) && tciParseInteger(a[2], hi) && hi > lo) {
            FoxCommand c = mk(FOXAPI_OP_SET_BANDWIDTH, static_cast<std::uint32_t>(r));
            c.num[0] = static_cast<double>(hi - lo);
            cmds.push_back(c);
        }
        return;
    }
    if (n == "sql_enable" || n == "sql_level") {
        const int r = rxArg();
        if (r == 0) { ignore(); return; }
        force(r, kSql);
        if (a.size() >= 2) {
            double level = -200.0;
            if (n == "sql_level") {
                std::int64_t db = 0;
                if (!tciParseInteger(a[1], db)) { return; }
                level = static_cast<double>(std::max<std::int64_t>(-140, std::min<std::int64_t>(0, db)));
                sqlLevel_[r] = level;
            } else if (tciParseBool(a[1])) {
                const auto it = sqlLevel_.find(r);
                level = it != sqlLevel_.end() ? it->second : -90.0;
            }
            FoxCommand c = mk(FOXAPI_OP_SET_SQUELCH, static_cast<std::uint32_t>(r));
            c.num[0] = level;
            cmds.push_back(c);
        }
        return;
    }
    if (n == "audio_start" || n == "audio_stop") {
        const int r = rxArg();
        if (r == 0) { ignore(); return; }
        AudioStream& s = audio_[r];
        s.on = n == "audio_start";
        s.pend.clear();
        s.count = 0;
        s.dec = TciDecimator(48000 / rate_);
        return;
    }
    if (n == "audio_samplerate") {
        std::int64_t v = 0;
        if (!a.empty() && tciParseInteger(a[0], v) && (v == 8000 || v == 12000 || v == 24000 || v == 48000)) {
            rate_ = static_cast<int>(v);
            for (auto& [r, s] : audio_) { s.dec = TciDecimator(48000 / rate_); s.pend.clear(); }
        }
        replies.push_back("AUDIO_SAMPLERATE:" + std::to_string(rate_) + ";");
        return;
    }
    if (n == "audio_stream_sample_type") {
        if (!a.empty()) {
            const std::string t = lowered(a[0]);
            if (t == "int16") { type_ = TciSampleType::Int16; }
            else if (t == "int24") { type_ = TciSampleType::Int24; }
            else if (t == "int32") { type_ = TciSampleType::Int32; }
            else if (t == "float32") { type_ = TciSampleType::Float32; }
        }
        const char* nm = type_ == TciSampleType::Int16 ? "int16" : type_ == TciSampleType::Int24 ? "int24" : type_ == TciSampleType::Int32 ? "int32" : "float32";
        replies.push_back(std::string("AUDIO_STREAM_SAMPLE_TYPE:") + nm + ";");
        return;
    }
    if (n == "audio_stream_channels") {
        std::int64_t v = 0;
        if (!a.empty() && tciParseInteger(a[0], v)) { channels_ = static_cast<int>(std::max<std::int64_t>(1, std::min<std::int64_t>(2, v))); }
        replies.push_back("AUDIO_STREAM_CHANNELS:" + std::to_string(channels_) + ";");
        return;
    }
    if (n == "audio_stream_samples") {
        std::int64_t v = 0;
        if (!a.empty() && tciParseInteger(a[0], v)) {
            samples_ = static_cast<int>(std::max<std::int64_t>(100, std::min<std::int64_t>(2048, v)));
            for (auto& [r, s] : audio_) { s.pend.clear(); }
        }
        replies.push_back("AUDIO_STREAM_SAMPLES:" + std::to_string(audioSamples()) + ";");
        return;
    }
    ignore();   // anything else, including IQ_START, the CW commands, DRIVE, SPOT and every sensor command
}

std::vector<std::string> TciConnection::tick(std::int64_t nowMs) {
    std::vector<std::string> out;
    now_ = nowMs;
    const int got = be_.pollResults();
    if (got > 0) { outstanding_ -= std::min(outstanding_, static_cast<std::size_t>(got)); }
    const std::vector<int> before = radios_;
    refreshRadios();
    if (radios_ != before) {
        // A radio came or went: the numbering shifted, so say how many there are and everything again (3.1).
        out.push_back("TRX_COUNT:" + std::to_string(radios_.size()) + ";");
        last_.clear();
        forced_.clear();
        ifLimitSent_ = -1;
        anyRunningKnown_ = false;
    }
    const bool settled = outstanding_ == 0 || nowMs - lastSubmitMs_ > 2000;
    bool anyRun = false;
    for (const int r : radios_) {
        TciRadioView v;
        if (!be_.readRadio(r, v)) { continue; }
        anyRun = anyRun || v.running;
        Sent s;
        s.centre = std::llround(v.centreHz); s.offset = std::llround(v.vfoOffsetHz); s.tuned = std::llround(v.tunedHz);
        s.bandwidth = std::llround(v.bandwidthHz); s.squelch = std::llround(v.squelchDb); s.rate = std::llround(v.sampleRateHz);
        s.demod = v.demod; s.running = v.running;
        unsigned g = 0;
        const auto it = last_.find(r);
        if (it == last_.end()) {
            g = kAll;
        } else {
            const Sent& o = it->second;
            if (o.centre != s.centre) { g |= kDds; }
            if (o.offset != s.offset) { g |= kIf; }
            if (o.tuned != s.tuned) { g |= kVfo; }
            if (o.demod != s.demod || o.bandwidth != s.bandwidth) { g |= kMode; }
            if (o.running != s.running) { g |= kRun; }
            if (o.squelch != s.squelch) { g |= kSql; }
        }
        if (settled) {
            const auto f = forced_.find(r);
            if (f != forced_.end()) { g |= f->second; forced_.erase(f); }
        }
        if (g != 0) { emitGroup(r, v, g, out); }
        last_[r] = s;
        if (indexOf(r) == 0 && ifLimitSent_ != s.rate) {
            ifLimitSent_ = s.rate;
            out.push_back("IF_LIMITS:" + num(-v.sampleRateHz / 2.0) + "," + num(v.sampleRateHz / 2.0) + ";");
        }
    }
    if (!anyRunningKnown_ || anyRun != anyRunningSent_) {
        out.push_back(anyRun ? "START;" : "STOP;");
        anyRunningSent_ = anyRun;
        anyRunningKnown_ = true;
    }
    return out;
}

std::vector<int> TciConnection::audioRadios() const {
    std::vector<int> r;
    for (const auto& [radio, s] : audio_) {
        if (s.on) { r.push_back(radio); }
    }
    return r;
}

std::vector<std::string> TciConnection::audioPackets(int radio, const float* mono48k, std::size_t frames) {
    std::vector<std::string> out;
    const auto it = audio_.find(radio);
    if (it == audio_.end() || !it->second.on) { return out; }
    AudioStream& s = it->second;
    s.dec.process(mono48k, frames, s.pend);
    // AUDIO_STREAM_SAMPLES is the number of samples in Stream.length; with two channels they are shared between them.
    const std::size_t perPacketValues = static_cast<std::size_t>(audioSamples());
    const std::size_t ch = static_cast<std::size_t>(channels_);
    const std::size_t perPacketFrames = std::max<std::size_t>(1, perPacketValues / ch);
    std::vector<float> inter;
    while (s.pend.size() >= perPacketFrames) {
        inter.assign(perPacketFrames * ch, 0.0f);
        for (std::size_t i = 0; i < perPacketFrames; ++i) {
            for (std::size_t c = 0; c < ch; ++c) { inter[i * ch + c] = s.pend[i]; }
        }
        s.pend.erase(s.pend.begin(), s.pend.begin() + static_cast<std::ptrdiff_t>(perPacketFrames));
        out.push_back(tciPackAudio(static_cast<std::uint32_t>(std::max(0, indexOf(radio))), static_cast<std::uint32_t>(rate_), type_,
                                   static_cast<std::uint32_t>(ch), s.count++, inter.data(), perPacketFrames));
    }
    return out;
}

}  // namespace cascade::net
