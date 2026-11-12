// wire_b_sigmeas.hpp - the signal-measurement blocks of dsp/sigmeas_*.hpp as READ OPERATIONS: samples in, one JSON object out (wire-b; docs/multi-radio-records/wire-b.md).
//
// PURE: no engine, no thread, no file. The engine glue (engine_wire_b.cpp) takes a span from a radio's channel taps (the channel I/Q after the VFO, at the
// channel rate, and the finished audio at 48 kHz), calls sigmeasRun and keeps the text in a LOCAL fact. Every frequency in the answers is Hz from the CHANNEL CENTRE
// (the tuned frequency: the channel is mixed to DC before the tap); the glue adds the tuned frequency and the rate to the object.
//
// PARAMETERS arrive as one JSON object (the op's text); every key is optional, an unknown key is an error (a typo must not read as a default), a key of the wrong
// type or outside its range is an error with the key named. The answers carry `valid` and, when a block could not measure, its one-phrase `reason`
// (the model's own: "span too short", "no signal found", "no on/off structure" ...).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "dsp/sigmeas_audio.hpp"
#include "dsp/sigmeas_bandwidth.hpp"
#include "dsp/sigmeas_bursts.hpp"
#include "dsp/sigmeas_histogram.hpp"
#include "dsp/sigmeas_hopchirp.hpp"
#include "dsp/sigmeas_modulation.hpp"
#include "dsp/sigmeas_symbols.hpp"
#include "dsp/sigmeas_vstime.hpp"

namespace cascade::engine::wireb {

namespace sm = cascade::sigmeas;
using Json = nlohmann::json;

enum class Block { Bandwidth, Modulation, Symbols, VsTime, Bursts, HopChirp, Audio, Drift, Histogram };

// Which input a block reads: the channel I/Q, or the finished audio.
inline bool blockWantsAudio(Block b) { return b == Block::Audio; }

struct SpanSpec {
    double endAgoS = 0.0;     // the span ENDS this long before the newest sample
    double durationS = 0.25;  // and lasts this long
};

// The answer of a call: the JSON object, or the sentence of a refusal (a bad parameter). `ok` false = refused (nothing to publish).
struct RunResult {
    bool ok = false;
    std::string why;   // refusal sentence when !ok
    Json out;          // the answer when ok
    bool valid = false;           // the block measured something
    std::string reason;           // when !valid
};

namespace detail {

inline double fin(double v) { return std::isfinite(v) ? v : 0.0; }

// Reads the parameters with strict key checking. `allowed` lists every key the block takes (plus the span keys, which the glue has taken already).
struct Params {
    const Json& j;
    std::set<std::string> seen;
    std::string error;
    explicit Params(const Json& obj) : j(obj) {}
    bool number(const char* key, double& v, double lo, double hi) {
        seen.insert(key);
        if (!j.contains(key)) { return true; }
        const Json& x = j[key];
        if (!x.is_number()) { error = std::string("\"") + key + "\" must be a number"; return false; }
        const double d = x.get<double>();
        if (!std::isfinite(d) || d < lo || d > hi) { error = std::string("\"") + key + "\" is outside its range"; return false; }
        v = d;
        return true;
    }
    bool count(const char* key, std::size_t& v, std::size_t lo, std::size_t hi) {
        double d = static_cast<double>(v);
        if (!number(key, d, static_cast<double>(lo), static_cast<double>(hi))) { return false; }
        v = static_cast<std::size_t>(d);
        return true;
    }
    bool flag(const char* key, bool& v) {
        seen.insert(key);
        if (!j.contains(key)) { return true; }
        if (!j[key].is_boolean()) { error = std::string("\"") + key + "\" must be true or false"; return false; }
        v = j[key].get<bool>();
        return true;
    }
    bool word(const char* key, std::string& v) {
        seen.insert(key);
        if (!j.contains(key)) { return true; }
        if (!j[key].is_string()) { error = std::string("\"") + key + "\" must be text"; return false; }
        v = j[key].get<std::string>();
        return true;
    }
    // After every key was read: any other key is a refusal.
    bool finish() {
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (seen.count(it.key()) == 0) { error = "unknown key \"" + it.key() + "\""; return false; }
        }
        return true;
    }
};

inline Json numberOrNull(double v) { return std::isfinite(v) ? Json(v) : Json(nullptr); }

inline std::string bitText(const std::vector<std::uint8_t>& bits, std::size_t cap) {
    std::string s;
    const std::size_t n = std::min(bits.size(), cap);
    s.reserve(n);
    for (std::size_t i = 0; i < n; ++i) { s.push_back(bits[i] != 0 ? '1' : '0'); }
    return s;
}

}  // namespace detail

// The span keys every block accepts (the glue reads them with spanOf; the block's own reader must not call them unknown).
inline bool spanOf(const Json& p, SpanSpec& s, std::string& why) {
    detail::Params q(p);
    if (!q.number("end_ago_s", s.endAgoS, 0.0, 60.0) || !q.number("duration_s", s.durationS, 0.001, 10.0)) { why = q.error; return false; }
    return true;
}

// THE BLOCKS. `iq` / `audio` is the span already cut; rates in Hz. `p` still holds the span keys.
inline RunResult sigmeasRun(Block block, const sm::Cf* iq, std::size_t nIq, double iqRate, const float* audio, std::size_t nAudio, double audioRate, const Json& p) {
    using detail::numberOrNull;
    RunResult rr;
    detail::Params q(p);
    double endAgo = 0.0, dur = 0.0;   // read only to mark the keys as known
    q.number("end_ago_s", endAgo, 0.0, 60.0);
    q.number("duration_s", dur, 0.001, 10.0);
    Json out = Json::object();
    const auto refuse = [&rr, &q]() {
        rr.ok = false;
        rr.why = q.error;
        return rr;
    };
    switch (block) {
        case Block::Bandwidth: {
            sm::BandwidthParams bp;
            double pct = 99.0, centre = 0.0, width = 0.0;
            double floorPower = 0.0, margin = bp.detectMarginDb;
            std::size_t nfft = 0;
            std::string xs;
            bool haveBand = false;
            q.seen.insert("band_low_hz"); q.seen.insert("band_high_hz");
            if (!q.number("obw_percent", pct, 50.1, 99.9) || !q.number("centre_hz", centre, -1e12, 1e12) || !q.number("width_hz", width, 0.0, 1e12) ||
                !q.number("floor_power", floorPower, 0.0, 1e30) || !q.number("detect_margin_db", margin, 0.1, 60.0) || !q.count("nfft", nfft, 0, 1u << 22)) { return refuse(); }
            if (p.contains("band_low_hz") || p.contains("band_high_hz")) {
                double lo = 0.0, hi = 0.0;
                if (!p.contains("band_low_hz") || !p.contains("band_high_hz")) { q.error = "band_low_hz and band_high_hz go together"; return refuse(); }
                if (!q.number("band_low_hz", lo, -1e12, 1e12) || !q.number("band_high_hz", hi, -1e12, 1e12)) { return refuse(); }
                bp.bandLowHz = lo; bp.bandHighHz = hi; haveBand = true;
            }
            if (width > 0.0) {
                if (haveBand) { q.error = "give either band_low_hz / band_high_hz or centre_hz / width_hz, not both"; return refuse(); }
                bp.bandLowHz = centre - width / 2.0; bp.bandHighHz = centre + width / 2.0;
            }
            if (p.contains("x_db")) {
                const Json& x = p["x_db"];
                if (!x.is_array() || x.empty() || x.size() > 8) { q.error = "\"x_db\" must be a list of one to eight numbers"; return refuse(); }
                bp.xDb.clear();
                for (const Json& e : x) {
                    if (!e.is_number() || !(e.get<double>() >= 1.0) || !(e.get<double>() <= 60.0)) { q.error = "\"x_db\" holds a number outside 1 to 60"; return refuse(); }
                    bp.xDb.push_back(e.get<double>());
                }
            }
            q.seen.insert("x_db");
            if (!q.finish()) { return refuse(); }
            bp.obwFraction = pct / 100.0; bp.floorPower = floorPower; bp.detectMarginDb = margin; bp.nfft = nfft;
            const sm::BandwidthResult r = sm::measureBandwidth(iq, nIq, iqRate, bp);
            rr.valid = r.valid; rr.reason = r.valid ? "" : r.reason;
            out["centre_hz"] = numberOrNull(r.centreHz); out["peak_hz"] = numberOrNull(r.peakHz); out["obw_hz"] = numberOrNull(r.obwHz);
            out["obw_low_hz"] = numberOrNull(r.obwLowHz); out["obw_high_hz"] = numberOrNull(r.obwHighHz);
            out["signal_power_dbfs"] = numberOrNull(r.signalPowerDbfs); out["floor_db_per_bin"] = numberOrNull(r.floorDbPerBin);
            out["snr_db"] = numberOrNull(r.snrDb); out["bin_hz"] = r.binHz; out["occupancy"] = r.occupancy; out["floor_reliable"] = r.floorReliable;
            Json xd = Json::array();
            for (const sm::XdbEntry& e : r.xdb) {
                xd.push_back({{"db", e.db}, {"valid", e.valid}, {"width_hz", numberOrNull(e.widthHz)}, {"low_hz", numberOrNull(e.lowHz)}, {"high_hz", numberOrNull(e.highHz)}});
            }
            out["xdb"] = xd;
            break;
        }
        case Block::Modulation: {
            sm::ModParams mp;
            double bw = 0.0;
            if (!q.number("audio_bandwidth_hz", bw, 0.0, 1e9) || !q.finish()) { return refuse(); }
            mp.audioBandwidthHz = bw;
            const sm::FmResult f = sm::measureFmDeviation(iq, nIq, iqRate, mp);
            const sm::AmResult a = sm::measureAmDepth(iq, nIq, iqRate, mp);
            rr.valid = f.valid || a.valid;
            rr.reason = rr.valid ? "" : std::string(f.reason);
            out["fm"] = {{"valid", f.valid}, {"reason", f.valid ? "" : f.reason}, {"carrier_offset_hz", numberOrNull(f.carrierOffsetHz)},
                         {"peak_deviation_hz", numberOrNull(f.peakDeviationHz)}, {"peak_estimate_hz", numberOrNull(f.peakEstimateHz)},
                         {"positive_peak_hz", numberOrNull(f.positivePeakHz)}, {"negative_peak_hz", numberOrNull(f.negativePeakHz)},
                         {"rms_deviation_hz", numberOrNull(f.rmsDeviationHz)}, {"tone_hz", numberOrNull(f.toneHz)},
                         {"tone_deviation_hz", numberOrNull(f.toneDeviationHz)}, {"tone_share", f.toneShare}, {"is_tone", f.isTone}};
            out["am"] = {{"valid", a.valid}, {"reason", a.valid ? "" : a.reason}, {"carrier_level", numberOrNull(a.carrierLevel)}, {"depth", numberOrNull(a.depth)},
                         {"peak_depth", numberOrNull(a.peakDepth)}, {"positive_depth", numberOrNull(a.positiveDepth)}, {"negative_depth", numberOrNull(a.negativeDepth)},
                         {"tone_depth", numberOrNull(a.toneDepth)}, {"tone_hz", numberOrNull(a.toneHz)}, {"tone_share", a.toneShare}, {"is_tone", a.isTone},
                         {"over_modulated", a.overModulated}};
            break;
        }
        case Block::Symbols: {
            std::string mode = "fsk";
            sm::SymbolParams sp;
            std::size_t maxBits = sp.maxBits, maxPulses = sp.maxPulses;
            double t0 = 0.0, t1 = 0.0, count = 0.0;
            if (!q.word("mode", mode) || !q.number("min_baud_hz", sp.minBaudHz, 0.0, 1e9) || !q.number("max_baud_hz", sp.maxBaudHz, 0.0, 1e9) ||
                !q.number("tolerance", sp.tolerance, 0.01, 0.5) || !q.count("max_bits", maxBits, 1, 65536) || !q.count("max_pulses", maxPulses, 1, 4096) ||
                !q.number("t0_s", t0, -1e9, 1e9) || !q.number("t1_s", t1, -1e9, 1e9) || !q.number("symbol_count", count, 0.0, 1e9)) { return refuse(); }
            if (!q.finish()) { return refuse(); }
            if (mode != "fsk" && mode != "ook" && mode != "cursor") { q.error = "\"mode\" is fsk, ook or cursor"; return refuse(); }
            sp.maxBits = maxBits; sp.maxPulses = maxPulses;
            if (mode == "cursor") {
                const sm::CursorPeriod c = sm::periodFromCursors(t0, t1, count);
                rr.valid = c.valid; rr.reason = c.valid ? "" : c.reason;
                out["period_s"] = numberOrNull(c.periodS); out["rate_hz"] = numberOrNull(c.rateHz);
            } else if (mode == "fsk") {
                const sm::FskResult f = sm::analyseFsk(iq, nIq, iqRate, sp);
                rr.valid = f.valid; rr.reason = f.valid ? "" : f.reason;
                out["mark_hz"] = numberOrNull(f.markHz); out["space_hz"] = numberOrNull(f.spaceHz); out["shift_hz"] = numberOrNull(f.shiftHz);
                out["centre_hz"] = numberOrNull(f.centreHz); out["baud_hz"] = numberOrNull(f.baudHz); out["period_samples"] = numberOrNull(f.periodSamples);
                out["jitter_ui"] = numberOrNull(f.jitterUi); out["separation"] = f.separation; out["edges"] = f.edges; out["ambiguous"] = f.ambiguous;
                out["bit_count"] = f.bits.size(); out["bits"] = detail::bitText(f.bits, maxBits);
            } else {
                const sm::OokResult o = sm::sliceOok(iq, nIq, iqRate, sp);
                rr.valid = o.valid; rr.reason = o.valid ? "" : o.reason;
                out["on_level"] = numberOrNull(o.onLevel); out["off_level"] = numberOrNull(o.offLevel); out["threshold"] = numberOrNull(o.threshold);
                out["baud_hz"] = numberOrNull(o.baudHz); out["period_s"] = numberOrNull(o.periodS); out["jitter_ui"] = numberOrNull(o.jitterUi);
                out["separation"] = o.separation; out["pulse_count"] = o.pulseCount; out["pulse_width_keyed"] = o.pulseWidthKeyed;
                out["short_width_s"] = numberOrNull(o.shortWidthS); out["long_width_s"] = numberOrNull(o.longWidthS); out["repeat_period_s"] = numberOrNull(o.repeatPeriodS);
                out["ambiguous"] = o.ambiguous; out["bit_count"] = o.bits.size(); out["bits"] = detail::bitText(o.bits, maxBits);
                Json pulses = Json::array();
                for (const sm::Pulse& pl : o.pulses) { pulses.push_back({{"start_s", pl.startS}, {"width_s", pl.widthS}, {"gap_after_s", pl.gapAfterS}}); }
                out["pulses"] = pulses;
            }
            break;
        }
        case Block::VsTime: {
            sm::VsTimeParams vp;
            std::size_t maxPoints = 512;
            if (!q.number("centre_hz", vp.centreHz, -1e12, 1e12) || !q.number("bandwidth_hz", vp.bandwidthHz, 0.0, 1e12) || !q.count("max_points", maxPoints, 2, 16384) ||
                !q.flag("remove_linear_phase", vp.removeLinearPhase) || !q.finish()) { return refuse(); }
            // The model decimates to max_points only for a signal it is told the width of (bandwidth_hz): decimating a wider signal would alias it. With no width given the
            // whole span is worked out at the full rate and the answer is THINNED to max_points (every k-th point, no filtering, so nothing aliases into the numbers).
            const bool thin = vp.bandwidthHz == 0.0;
            vp.maxPoints = thin ? 0 : maxPoints;
            const sm::VsTimeResult v = sm::computeVsTime(iq, nIq, iqRate, vp);
            rr.valid = v.valid; rr.reason = v.valid ? "" : v.reason;
            const std::size_t have = v.amplitudeDb.size();
            const std::size_t stride = (thin && have > maxPoints) ? (have + maxPoints - 1) / maxPoints : 1;
            out["rate_out_hz"] = v.rateOut / static_cast<double>(stride); out["decimation"] = v.decimation * stride; out["start_s"] = v.startS; out["step_s"] = v.stepS * static_cast<double>(stride);
            out["mean_freq_hz"] = numberOrNull(v.meanFreqHz); out["phase_slope_rad_per_s"] = numberOrNull(v.phaseSlopeRadPerS);
            Json a = Json::array(), f = Json::array(), ph = Json::array();
            for (std::size_t i = 0; i < v.amplitudeDb.size(); i += stride) { a.push_back(detail::fin(v.amplitudeDb[i])); }
            for (std::size_t i = 0; i < v.freqHz.size(); i += stride) { f.push_back(detail::fin(v.freqHz[i])); }
            for (std::size_t i = 0; i < v.phaseRad.size(); i += stride) { ph.push_back(detail::fin(v.phaseRad[i])); }
            out["points"] = a.size();
            out["amplitude_dbfs"] = a; out["freq_hz"] = f; out["phase_rad"] = ph;
            break;
        }
        case Block::Bursts: {
            sm::BurstParams bp;
            std::size_t block = 0, gap = bp.minGapBlocks, minBurst = bp.minBurstBlocks, maxBursts = bp.maxBursts;
            if (!q.count("block_samples", block, 0, 1u << 20) || !q.number("min_contrast_db", bp.minContrastDb, 0.5, 120.0) || !q.count("bridge_blocks", gap, 0, 1000) ||
                !q.count("min_burst_blocks", minBurst, 1, 100000) || !q.count("max_bursts", maxBursts, 1, 10000) || !q.finish()) { return refuse(); }
            bp.blockSamples = block; bp.minGapBlocks = gap; bp.minBurstBlocks = minBurst; bp.maxBursts = maxBursts;
            const sm::BurstReport b = sm::detectBursts(iq, nIq, iqRate, bp);
            rr.valid = b.valid; rr.reason = b.valid ? "" : b.reason;
            out["continuous"] = b.continuous; out["block_samples"] = b.blockSamples; out["noise_dbfs"] = numberOrNull(b.noiseDbfs); out["level_dbfs"] = numberOrNull(b.levelDbfs);
            out["threshold_dbfs"] = numberOrNull(b.thresholdDbfs); out["total_bursts"] = b.totalBursts; out["duty_cycle"] = numberOrNull(b.dutyCycle);
            out["repeat_period_s"] = numberOrNull(b.repeatPeriodS); out["repeat_jitter"] = numberOrNull(b.repeatJitter);
            Json list = Json::array();
            for (const sm::Burst& x : b.bursts) {
                list.push_back({{"start_s", x.startS}, {"length_s", x.lengthS}, {"power_dbfs", x.powerDbfs}, {"peak_dbfs", x.peakDbfs}, {"freq_hz", x.freqHz}});
            }
            out["bursts"] = list;
            break;
        }
        case Block::HopChirp: {
            sm::TrackParams tp;
            sm::HopParams hp;
            std::size_t frame = 0, hop = 0;
            if (!q.count("frame_samples", frame, 0, 1u << 16) || !q.count("hop_samples", hop, 0, 1u << 16) || !q.number("min_snr_db", tp.minSnrDb, 0.0, 100.0) ||
                !q.number("dwell_tolerance_hz", hp.tolHz, 0.0, 1e12) || !q.finish()) { return refuse(); }
            if (frame != 0 && !sm::isPow2(frame)) { q.error = "\"frame_samples\" must be a power of two"; return refuse(); }
            tp.frameSamples = frame; tp.hopSamples = hop;
            const sm::FreqTrack t = sm::trackPeak(iq, nIq, iqRate, tp);
            rr.valid = t.valid; rr.reason = t.valid ? "" : t.reason;
            if (t.valid) {
                const sm::HopReport h = sm::analyseHops(t, hp);
                const sm::ChirpReport c = sm::analyseChirps(t);
                out["track_points"] = t.timeS.size(); out["track_ok"] = t.okCount; out["step_s"] = t.stepS; out["bin_hz"] = t.binHz;
                out["hop"] = {{"valid", h.valid}, {"reason", h.valid ? "" : h.reason}, {"hop_count", h.hopCount}, {"hop_rate_hz", numberOrNull(h.hopRateHz)},
                              {"dwell_s", numberOrNull(h.dwellS)}, {"channel_spacing_hz", numberOrNull(h.channelSpacingHz)}, {"hop_set_hz", h.distinctFreqs}};
                Json dw = Json::array();
                for (const sm::Dwell& d : h.dwells) { dw.push_back({{"start_s", d.startS}, {"end_s", d.endS}, {"freq_hz", d.freqHz}, {"frames", d.frames}}); }
                out["hop"]["dwells"] = dw;
                out["chirp"] = {{"valid", c.valid}, {"reason", c.valid ? "" : c.reason}, {"slope_hz_per_s", numberOrNull(c.slopeHzPerS)}, {"width_hz", numberOrNull(c.widthHz)},
                                {"direction", c.direction}, {"period_s", numberOrNull(c.periodS)}, {"sweeps", c.sweeps.size()}};
            }
            break;
        }
        case Block::Audio: {
            sm::AudioParams ap;
            std::size_t harm = ap.maxHarmonic, nfft = 0;
            if (!q.number("tone_hz", ap.toneHz, 0.0, 1e9) || !q.count("max_harmonic", harm, 2, 64) || !q.number("band_low_hz", ap.bandLowHz, 0.0, 1e9) ||
                !q.number("band_high_hz", ap.bandHighHz, 0.0, 1e9) || !q.number("full_scale", ap.fullScaleAmplitude, 1e-9, 1e9) || !q.count("nfft", nfft, 0, 1u << 20) ||
                !q.finish()) { return refuse(); }
            ap.maxHarmonic = harm; ap.nfft = nfft;
            const sm::AudioResult a = sm::measureAudio(audio, nAudio, audioRate, ap);
            rr.valid = a.valid; rr.reason = a.valid ? "" : a.reason;
            out["tone_hz"] = numberOrNull(a.toneHz); out["tone_amplitude"] = numberOrNull(a.toneAmplitude); out["tone_dbfs"] = numberOrNull(a.toneDbfs);
            out["sinad_db"] = numberOrNull(a.sinadDb); out["thd_db"] = numberOrNull(a.thdDb); out["thd_percent"] = numberOrNull(a.thdPercent);
            out["thdn_db"] = numberOrNull(a.thdnDb); out["thdn_percent"] = numberOrNull(a.thdnPercent); out["sfdr_dbc"] = numberOrNull(a.sfdrDbc);
            out["spur_hz"] = numberOrNull(a.spurHz); out["enob"] = numberOrNull(a.enob); out["noise_floor_dbfs"] = numberOrNull(a.noiseFloorDbfs);
            Json h = Json::array();
            for (double d : a.harmonicsDbc) { h.push_back(detail::fin(d)); }
            out["harmonics_dbc"] = h;
            break;
        }
        case Block::Drift: {
            sm::DriftParams dp;
            std::size_t steps = dp.gridSteps, decim = 0;
            if (!q.number("min_hz_per_s", dp.minDriftHzPerS, -1e6, 1e6) || !q.number("max_hz_per_s", dp.maxDriftHzPerS, -1e6, 1e6) || !q.count("steps", steps, 8, 4096) ||
                !q.count("decimate", decim, 0, 4096) || !q.finish()) { return refuse(); }
            if (!(dp.maxDriftHzPerS > dp.minDriftHzPerS)) { q.error = "\"max_hz_per_s\" must be above \"min_hz_per_s\""; return refuse(); }
            dp.gridSteps = steps;
            // The search cuts the span to 2^16 samples, which at a 200 kHz channel is a third of a second: too short to see a slow drift. The span is therefore averaged down
            // first (block means: a low-pass with its first null at the new rate), automatically to about 12.5 kHz ("decimate" 0), or by the factor asked for (1 = none).
            std::size_t dec = decim != 0 ? decim : std::max<std::size_t>(1, static_cast<std::size_t>(iqRate / 12500.0));
            if (dec > nIq) { dec = 1; }
            std::vector<sm::Cf> reduced;
            const sm::Cf* sig = iq;
            std::size_t nSig = nIq;
            double sigRate = iqRate;
            if (dec > 1 && iq != nullptr) {
                nSig = nIq / dec;
                reduced.resize(nSig);
                for (std::size_t i = 0; i < nSig; ++i) {
                    std::complex<double> acc(0.0, 0.0);
                    for (std::size_t k = 0; k < dec; ++k) { acc += std::complex<double>(iq[i * dec + k]); }
                    reduced[i] = sm::Cf(static_cast<float>(acc.real() / static_cast<double>(dec)), static_cast<float>(acc.imag() / static_cast<double>(dec)));
                }
                sig = reduced.data();
                sigRate = iqRate / static_cast<double>(dec);
            }
            out["decimation"] = dec;
            const sm::DriftResult d = sm::searchDrift(sig, nSig, sigRate, dp);
            rr.valid = d.valid; rr.reason = d.valid ? "" : d.reason;
            out["drift_hz_per_s"] = numberOrNull(d.driftHzPerS); out["start_freq_hz"] = numberOrNull(d.startFreqHz); out["peak_dbfs"] = numberOrNull(d.peakDbfs);
            out["gain_db"] = numberOrNull(d.gainDb);
            break;
        }
        case Block::Histogram: {
            sm::HistParams hp;
            std::size_t bins = hp.bins;
            std::string source = "iq";
            bool ccdf = false;
            double ccdfRange = 20.0, ccdfStep = 0.5, lo = 0.0, hi = 0.0;
            if (!q.word("source", source) || !q.count("bins", bins, 1, 65536) || !q.flag("ccdf", ccdf) || !q.number("ccdf_range_db", ccdfRange, 1.0, 100.0) ||
                !q.number("ccdf_step_db", ccdfStep, 0.05, 10.0)) { return refuse(); }
            const bool haveLo = p.contains("range_low"), haveHi = p.contains("range_high");
            if (!q.number("range_low", lo, -1e30, 1e30) || !q.number("range_high", hi, -1e30, 1e30) || !q.finish()) { return refuse(); }
            if (source != "iq" && source != "audio") { q.error = "\"source\" is iq or audio"; return refuse(); }
            if (haveLo != haveHi) { q.error = "range_low and range_high go together"; return refuse(); }
            hp.bins = bins;
            if (haveLo) { hp.lo = lo; hp.hi = hi; }
            const sm::HistResult h = source == "iq" ? sm::magnitudeHistogram(iq, nIq, hp) : sm::amplitudeHistogram(audio, nAudio, hp);
            rr.valid = h.valid; rr.reason = h.valid ? "" : h.reason;
            out["source"] = source; out["lo"] = numberOrNull(h.lo); out["hi"] = numberOrNull(h.hi); out["bin_width"] = numberOrNull(h.binWidth);
            out["total"] = h.total; out["below_range"] = h.belowRange; out["above_range"] = h.aboveRange; out["non_finite"] = h.nonFinite;
            out["mean"] = numberOrNull(h.mean); out["rms"] = numberOrNull(h.rms); out["min"] = numberOrNull(h.minValue); out["max"] = numberOrNull(h.maxValue);
            out["peak_abs"] = numberOrNull(h.peakAbs); out["crest_factor_db"] = numberOrNull(h.crestFactorDb);
            out["counts"] = h.counts; out["probability"] = h.probability;
            if (ccdf) {
                const sm::CcdfResult c = sm::powerCcdf(iq, nIq, ccdfRange, ccdfStep);
                out["ccdf"] = {{"valid", c.valid}, {"reason", c.valid ? "" : c.reason}, {"step_db", c.stepDb}, {"db_above_mean", c.dbAboveMean}, {"probability", c.probability},
                               {"mean_power_dbfs", numberOrNull(c.meanPowerDbfs)}, {"peak_power_dbfs", numberOrNull(c.peakPowerDbfs)}, {"papr_db", numberOrNull(c.paprDb)}};
            }
            break;
        }
    }
    rr.ok = true;
    rr.out = std::move(out);
    return rr;
}

}  // namespace cascade::engine::wireb
