// airband_channel.hpp - PER-CHANNEL SETTINGS for the airband monitor as a MODEL: its own squelch (a level threshold or an SNR threshold), a high-pass,
// a low-pass and up to three notches on the audio, and where the channel's audio goes (nowhere, a file, a loopback UDP stream). Not wired: the monitor
// (core/airband_monitor.hpp) is a forbidden file for this lane - it gives every channel the single value squelchDb today (buildMonitorSet's
// `squelchDb` argument); the hand-off in docs/feature-records/dspmodels.md says exactly where a per-channel value goes.
//
// THE RIVAL (RTLSDR-Airband) gives each frequency its own squelch and its own outputs; this is that, as a model with a text form for the config.
//
// WHAT IS HERE.
//   - AirbandChannelConfig: frequency, bandwidth, mode (0 AM, 1 NFM: the MonitorMode values), squelch kind and numbers, filter, output.
//     validate() names the first thing wrong; sanitise() clamps everything into range; serialise()/parse() are a strict one-line text form
//     (FOXAIRCH1|...) of INTEGERS only (hertz, tenths of dB, milliseconds), so a round trip is exact and the parser has no locale.
//     OUTPUT IS OFF BY DEFAULT: output = None; a file or a stream is something the user switches on for that channel.
//   - ChannelAudioChain: the high-pass, low-pass and notches (second-order sections from dsp/audio_biquad.hpp, the W3C Audio EQ Cookbook; the
//     high- and low-pass are 4th-order Butterworth by default (two sections, Q 0.5412 and 1.3066), order 2 when asked; a notch is one section of Q
//     3 .. 100). With everything off process() returns at once and the samples are untouched (bit-exact).
//   - ChannelSquelch: the per-channel gate. Level = dsp::Squelch (the existing one, with THIS channel's threshold and hold); Snr = dsp::SnrSquelch
//     (this lane's). One interface: process(baseband, n, audio) gates the audio in place and says whether the gate is open.
//   - ChannelProcessor: the two in the order the monitor needs them (filter the audio, then gate it).
// No allocation in process(); set-up allocates. One thread (the DSP thread).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "core/airband_monitor.hpp"
#include "dsp/audio_biquad.hpp"
#include "dsp/snr_squelch.hpp"
#include "dsp/squelch.hpp"

namespace cascade::core::airband {

enum class SquelchKind : int { Level = 0, Snr = 1 };
enum class OutputKind : int { None = 0, WavFile = 1, Udp = 2 };

inline constexpr int kMaxNotches = 3;
inline constexpr std::size_t kMaxTextLen = 600;

struct Notch {
    int hz = 0;        // 0 = unused
    int qTenths = 300; // Q x 10
    bool operator==(const Notch&) const = default;
};

struct AirbandChannelConfig {
    std::int64_t freqHz = 0;
    int bandwidthHz = 10000;
    int mode = 0;                       // 0 AM, 1 NFM (MonitorMode)
    SquelchKind squelchKind = SquelchKind::Level;
    int squelchTenthsDb = -500;         // level threshold, dB of channel power re full scale x 10 (-120 .. 0 dB)
    int snrOpenTenthsDb = 100;          // SNR threshold x 10 (1 .. 60 dB)
    int snrHysteresisTenthsDb = 30;     // SNR hysteresis x 10 (0 .. 30 dB)
    int holdMs = 400;                   // level squelch: hold; SNR squelch: hang (0 .. 10000)
    int highPassHz = 0;                 // 0 = off, else 20 .. 3000
    int lowPassHz = 0;                  // 0 = off, else 300 .. 20000
    int filterOrder = 4;                // 2 or 4
    std::array<Notch, kMaxNotches> notches{};
    OutputKind output = OutputKind::None;
    bool dailyFolder = true;
    int maxFileMb = 100;                // 1 .. 2000
    int udpPort = 0;                    // 0 = none, else 1024 .. 65535 (the stream goes to 127.0.0.1 only)
    bool operator==(const AirbandChannelConfig&) const = default;
};

enum class Problem : int {
    None = 0,
    BadFrequency,
    BadBandwidth,
    BadMode,
    BadSquelch,
    BadFilter,
    FilterOrder,
    BadNotch,
    BadOutput,
    BadFileSize,
    BadPort,
    PortMissing,
};

inline const char* problemText(Problem p) {
    switch (p) {
        case Problem::None: return "";
        case Problem::BadFrequency: return "The channel frequency is not usable.";
        case Problem::BadBandwidth: return "The channel width must be between 1 kHz and 200 kHz.";
        case Problem::BadMode: return "The channel mode must be AM or NFM.";
        case Problem::BadSquelch: return "A squelch value is out of range.";
        case Problem::BadFilter: return "The high-pass must be below the low-pass, each within its range.";
        case Problem::FilterOrder: return "The filter order must be 2 or 4.";
        case Problem::BadNotch: return "A notch frequency or Q is out of range.";
        case Problem::BadOutput: return "The output kind is not known.";
        case Problem::BadFileSize: return "The file size limit must be between 1 and 2000 MB.";
        case Problem::BadPort: return "The stream port must be between 1024 and 65535.";
        case Problem::PortMissing: return "A stream output needs a port.";
    }
    return "";
}

inline Problem validate(const AirbandChannelConfig& c) {
    if (c.freqHz < 100000 || c.freqHz > 6000000000ll) { return Problem::BadFrequency; }
    if (c.bandwidthHz < 1000 || c.bandwidthHz > 200000) { return Problem::BadBandwidth; }
    if (c.mode != 0 && c.mode != 1) { return Problem::BadMode; }
    if (c.squelchTenthsDb < -1200 || c.squelchTenthsDb > 0 || c.snrOpenTenthsDb < 10 || c.snrOpenTenthsDb > 600 || c.snrHysteresisTenthsDb < 0 ||
        c.snrHysteresisTenthsDb > 300 || c.holdMs < 0 || c.holdMs > 10000) {
        return Problem::BadSquelch;
    }
    if ((c.highPassHz != 0 && (c.highPassHz < 20 || c.highPassHz > 3000)) || (c.lowPassHz != 0 && (c.lowPassHz < 300 || c.lowPassHz > 20000)) ||
        (c.highPassHz != 0 && c.lowPassHz != 0 && c.highPassHz >= c.lowPassHz)) {
        return Problem::BadFilter;
    }
    if (c.filterOrder != 2 && c.filterOrder != 4) { return Problem::FilterOrder; }
    for (const Notch& n : c.notches) {
        if (n.hz != 0 && (n.hz < 50 || n.hz > 20000 || n.qTenths < 30 || n.qTenths > 1000)) { return Problem::BadNotch; }
    }
    if (c.output != OutputKind::None && c.output != OutputKind::WavFile && c.output != OutputKind::Udp) { return Problem::BadOutput; }
    if (c.maxFileMb < 1 || c.maxFileMb > 2000) { return Problem::BadFileSize; }
    if (c.udpPort != 0 && (c.udpPort < 1024 || c.udpPort > 65535)) { return Problem::BadPort; }
    if (c.output == OutputKind::Udp && c.udpPort == 0) { return Problem::PortMissing; }
    return Problem::None;
}

// Everything into range (a bad frequency stays as the nearest legal one; a stream output with no port falls back to no output).
inline AirbandChannelConfig sanitise(AirbandChannelConfig c) {
    c.freqHz = std::min<std::int64_t>(std::max<std::int64_t>(c.freqHz, 100000), 6000000000ll);
    c.bandwidthHz = std::min(std::max(c.bandwidthHz, 1000), 200000);
    if (c.mode != 1) { c.mode = 0; }
    c.squelchTenthsDb = std::min(std::max(c.squelchTenthsDb, -1200), 0);
    c.snrOpenTenthsDb = std::min(std::max(c.snrOpenTenthsDb, 10), 600);
    c.snrHysteresisTenthsDb = std::min(std::max(c.snrHysteresisTenthsDb, 0), 300);
    c.holdMs = std::min(std::max(c.holdMs, 0), 10000);
    if (c.highPassHz != 0) { c.highPassHz = std::min(std::max(c.highPassHz, 20), 3000); }
    if (c.lowPassHz != 0) { c.lowPassHz = std::min(std::max(c.lowPassHz, 300), 20000); }
    if (c.highPassHz != 0 && c.lowPassHz != 0 && c.highPassHz >= c.lowPassHz) { c.highPassHz = 0; }  // the low-pass wins: it is the one that shapes speech
    c.filterOrder = c.filterOrder == 2 ? 2 : 4;
    for (Notch& n : c.notches) {
        if (n.hz != 0) {
            n.hz = std::min(std::max(n.hz, 50), 20000);
            n.qTenths = std::min(std::max(n.qTenths, 30), 1000);
        } else {
            n.qTenths = 300;
        }
    }
    if (c.output != OutputKind::WavFile && c.output != OutputKind::Udp) { c.output = OutputKind::None; }
    c.maxFileMb = std::min(std::max(c.maxFileMb, 1), 2000);
    if (c.udpPort != 0) { c.udpPort = std::min(std::max(c.udpPort, 1024), 65535); }
    if (c.output == OutputKind::Udp && c.udpPort == 0) { c.output = OutputKind::None; }
    return c;
}

inline MonitorChannel toMonitorChannel(const AirbandChannelConfig& c) {
    MonitorChannel m;
    m.freqHz = static_cast<double>(c.freqHz);
    m.bandwidthHz = static_cast<double>(c.bandwidthHz);
    m.mode = c.mode == 1 ? MonitorMode::Nfm : MonitorMode::Am;
    return m;
}

// ----------------------------------------------------------------- text form

inline std::string serialise(const AirbandChannelConfig& c) {
    std::string s = "FOXAIRCH1";
    auto add = [&](std::int64_t v) { s += '|'; s += std::to_string(v); };
    add(c.freqHz);
    add(c.bandwidthHz);
    add(c.mode);
    add(static_cast<int>(c.squelchKind));
    add(c.squelchTenthsDb);
    add(c.snrOpenTenthsDb);
    add(c.snrHysteresisTenthsDb);
    add(c.holdMs);
    add(c.highPassHz);
    add(c.lowPassHz);
    add(c.filterOrder);
    for (const Notch& n : c.notches) { add(n.hz); add(n.qTenths); }
    add(static_cast<int>(c.output));
    add(c.dailyFolder ? 1 : 0);
    add(c.maxFileMb);
    add(c.udpPort);
    return s;
}

struct ParseResult {
    bool ok = false;
    const char* why = "";
    Problem problem = Problem::None;
    AirbandChannelConfig config;
};

inline ParseResult parse(std::string_view text) {
    ParseResult r;
    auto fail = [&](const char* why) {
        r.ok = false;
        r.why = why;
        r.config = AirbandChannelConfig{};
        return r;
    };
    if (text.size() > kMaxTextLen) { return fail("too long"); }
    constexpr std::string_view head = "FOXAIRCH1";
    if (text.substr(0, head.size()) != head) { return fail("not a channel"); }
    text.remove_prefix(head.size());
    constexpr int kFields = 11 + kMaxNotches * 2 + 4;
    std::int64_t v[kFields];
    for (int i = 0; i < kFields; ++i) {
        if (text.empty() || text[0] != '|') { return fail("missing field"); }
        text.remove_prefix(1);
        const std::size_t bar = text.find('|');
        const std::string_view f = text.substr(0, bar);
        if (f.empty() || f.size() > 14) { return fail("bad number"); }
        const char* b = f.data();
        const char* e = f.data() + f.size();
        std::int64_t x = 0;
        const auto fc = std::from_chars(b, e, x);
        if (fc.ec != std::errc() || fc.ptr != e) { return fail("bad number"); }
        v[i] = x;
        if (bar == std::string_view::npos) {
            text = std::string_view();
            if (i != kFields - 1) { return fail("missing field"); }
        } else {
            text.remove_prefix(bar);
        }
    }
    if (!text.empty()) { return fail("extra text"); }
    auto narrow = [&](int i, int lo, int hi, int* out) {
        if (v[i] < lo || v[i] > hi) { return false; }
        *out = static_cast<int>(v[i]);
        return true;
    };
    AirbandChannelConfig c;
    c.freqHz = v[0];
    int tmp = 0;
    int k = 1;
    if (!narrow(k++, -2000000000, 2000000000, &c.bandwidthHz)) { return fail("bad number"); }
    if (!narrow(k++, -10, 10, &c.mode)) { return fail("bad number"); }
    if (!narrow(k++, 0, 1, &tmp)) { return fail("bad squelch kind"); }
    c.squelchKind = tmp == 1 ? SquelchKind::Snr : SquelchKind::Level;
    for (int* p : {&c.squelchTenthsDb, &c.snrOpenTenthsDb, &c.snrHysteresisTenthsDb, &c.holdMs, &c.highPassHz, &c.lowPassHz, &c.filterOrder}) {
        if (!narrow(k++, -2000000000, 2000000000, p)) { return fail("bad number"); }
    }
    for (Notch& n : c.notches) {
        if (!narrow(k++, -2000000000, 2000000000, &n.hz) || !narrow(k++, -2000000000, 2000000000, &n.qTenths)) { return fail("bad number"); }
    }
    if (!narrow(k++, 0, 2, &tmp)) { return fail("bad output kind"); }
    c.output = static_cast<OutputKind>(tmp);
    if (!narrow(k++, 0, 1, &tmp)) { return fail("bad flag"); }
    c.dailyFolder = tmp == 1;
    if (!narrow(k++, -2000000000, 2000000000, &c.maxFileMb) || !narrow(k++, -2000000000, 2000000000, &c.udpPort)) { return fail("bad number"); }
    const Problem p = validate(c);
    if (p != Problem::None) {
        r.ok = false;
        r.problem = p;
        return r;
    }
    r.ok = true;
    r.config = c;
    return r;
}

// ---------------------------------------------------------------- audio chain

class ChannelAudioChain {
public:
    explicit ChannelAudioChain(double sampleRateHz = 48000.0) : fs_(sampleRateHz > 1000.0 ? sampleRateHz : 48000.0) {}

    // Takes the filter part of a config (the rest is ignored). Section states are kept, so a change in mid-stream moves the response and keeps the
    // signal continuous (a retune of a filter with state is not click-free to the bit, but the states carry on).
    void configure(const AirbandChannelConfig& cfg) {
        const AirbandChannelConfig c = sanitise(cfg);
        nHp_ = c.highPassHz != 0 ? (c.filterOrder == 2 ? 1 : 2) : 0;
        nLp_ = c.lowPassHz != 0 ? (c.filterOrder == 2 ? 1 : 2) : 0;
        static constexpr double q4[2] = {0.5411961001461970, 1.3065629648763766};
        for (int i = 0; i < nHp_; ++i) { hp_[static_cast<std::size_t>(i)] = dsp::abq::highpass(fs_, c.highPassHz, nHp_ == 1 ? 0.70710678118654752 : q4[i]); }
        for (int i = 0; i < nLp_; ++i) { lp_[static_cast<std::size_t>(i)] = dsp::abq::lowpass(fs_, c.lowPassHz, nLp_ == 1 ? 0.70710678118654752 : q4[i]); }
        nNotch_ = 0;
        for (const Notch& n : c.notches) {
            if (n.hz != 0) { notch_[static_cast<std::size_t>(nNotch_++)] = dsp::abq::notch(fs_, n.hz, n.qTenths / 10.0); }
        }
    }
    bool isBypassed() const { return nHp_ == 0 && nLp_ == 0 && nNotch_ == 0; }

    void process(float* x, std::size_t n) {
        if (x == nullptr || isBypassed()) { return; }
        for (std::size_t i = 0; i < n; ++i) {
            double v = std::isfinite(x[i]) ? static_cast<double>(x[i]) : 0.0;
            for (int k = 0; k < nHp_; ++k) { v = dsp::abq::run(hp_[static_cast<std::size_t>(k)], shp_[static_cast<std::size_t>(k)], v); }
            for (int k = 0; k < nLp_; ++k) { v = dsp::abq::run(lp_[static_cast<std::size_t>(k)], slp_[static_cast<std::size_t>(k)], v); }
            for (int k = 0; k < nNotch_; ++k) { v = dsp::abq::run(notch_[static_cast<std::size_t>(k)], sn_[static_cast<std::size_t>(k)], v); }
            x[i] = static_cast<float>(v);
        }
    }

    // The chain's magnitude response in dB at `hz` (for a drawing and for the tests).
    double responseDb(double hz) const {
        double db = 0.0;
        for (int k = 0; k < nHp_; ++k) { db += dsp::abq::magnitudeDb(hp_[static_cast<std::size_t>(k)], fs_, hz); }
        for (int k = 0; k < nLp_; ++k) { db += dsp::abq::magnitudeDb(lp_[static_cast<std::size_t>(k)], fs_, hz); }
        for (int k = 0; k < nNotch_; ++k) { db += dsp::abq::magnitudeDb(notch_[static_cast<std::size_t>(k)], fs_, hz); }
        return db;
    }

private:
    double fs_;
    int nHp_ = 0, nLp_ = 0, nNotch_ = 0;
    std::array<dsp::abq::Coef, 2> hp_{}, lp_{};
    std::array<dsp::abq::State, 2> shp_{}, slp_{};
    std::array<dsp::abq::Coef, kMaxNotches> notch_{};
    std::array<dsp::abq::State, kMaxNotches> sn_{};
};

// ------------------------------------------------------------------ squelch

class ChannelSquelch {
public:
    explicit ChannelSquelch(double sampleRateHz = 48000.0) : fs_(sampleRateHz), level_(sampleRateHz), snr_(sampleRateHz) {}

    void configure(const AirbandChannelConfig& cfg) {
        const AirbandChannelConfig c = sanitise(cfg);
        kind_ = c.squelchKind;
        level_.setThresholdDb(static_cast<float>(c.squelchTenthsDb / 10.0));
        level_.setHoldMs(static_cast<float>(c.holdMs));
        dsp::SnrSquelchSettings s = snr_.settings();
        s.enabled = true;
        s.openDb = c.snrOpenTenthsDb / 10.0;
        s.hysteresisDb = c.snrHysteresisTenthsDb / 10.0;
        s.hangMs = c.holdMs;
        snr_.setSettings(s);
    }
    SquelchKind kind() const { return kind_; }
    bool isOpen() const { return kind_ == SquelchKind::Snr ? snr_.isOpen() : level_.isOpen(); }
    // The number the channel's meter shows: channel power in dB (Level) or SNR in dB (Snr).
    double meterDb() const { return kind_ == SquelchKind::Snr ? snr_.snrDb() : static_cast<double>(level_.levelDb()); }

    // Judges the baseband and gates `audio` (n samples, in place). Returns true when the gate is open at the end of the block.
    bool process(const std::complex<float>* baseband, std::size_t n, float* audio) {
        if (kind_ == SquelchKind::Snr) { snr_.process(baseband, n, audio); }
        else { level_.process(baseband, n, audio); }
        return isOpen();
    }

private:
    double fs_;
    SquelchKind kind_ = SquelchKind::Level;
    dsp::Squelch level_;
    dsp::SnrSquelch snr_;
};

// The two in the monitor's order: the audio is filtered, then gated.
class ChannelProcessor {
public:
    explicit ChannelProcessor(double sampleRateHz = 48000.0) : chain_(sampleRateHz), squelch_(sampleRateHz) {}
    void configure(const AirbandChannelConfig& c) {
        chain_.configure(c);
        squelch_.configure(c);
    }
    // `preGate`, when given (n floats), receives a copy of the audio AFTER the filters and BEFORE the gate: that is what a file output wants (the
    // pre-roll and the tail are real audio there, not the gate's zeros). Returns whether the gate is open at the end of the block.
    bool process(const std::complex<float>* baseband, float* audio, std::size_t n, float* preGate = nullptr) {
        chain_.process(audio, n);
        if (preGate != nullptr && audio != nullptr) { std::copy(audio, audio + n, preGate); }
        return squelch_.process(baseband, n, audio);
    }
    ChannelAudioChain& chain() { return chain_; }
    ChannelSquelch& squelch() { return squelch_; }

private:
    ChannelAudioChain chain_;
    ChannelSquelch squelch_;
};

}  // namespace cascade::core::airband
