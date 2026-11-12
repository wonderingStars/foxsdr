// wire_a_frames.cpp - WIRING LANE wire-a, the published spectrum frame: the display chain (DISPLAY_AVERAGE) and the waterfall history (WF_HISTORY). The ops are documented in core/wire_a_ops.hpp.
//
// THE TAP. The one place every screen's spectrum is made is ApiHostImpl::endPass (fox_engine_api.cpp): the pipeline's newest frame is copied into a ring that the window, the web page and every other client
// read. The lane's one call there (displayFrame, for the first radio and for each other open radio) works on the engine's OWN copy of the frame, in place, before it is published: the average, the smoothing and
// the de-comb of dsp/trace_average.hpp, then the line kept for the history. The pipeline (the DSP thread) is not touched, and the engine's own looks at the spectrum (centre_signal, a scan) read the raw frame.
// All off (the default) is one test of a flag. The averager's axis key is the (centre, span, bins) of the frame, so a retune or a change of FFT size starts the average again.
//
// THE HISTORY (core/waterfall_history.hpp, row 23): a ring of the lines that were published, one byte a bin, kept per radio and OFF by default (it is up to 192 MiB a radio): WF_HISTORY switches it on for a
// radio and sizes it. The control ops move the view; the lines are read by the host through waterfallCopy (the API has no call for lines; a same-process host calls it). Nothing here bumps the fact block on a frame:
// a frame is not a change of the block's inputs (see the retune rule in wire_a_tuning.cpp).
//
// PERSISTENCE: display-average.json beside markers.json (the chain's setting; the history is never kept across runs: it is a record of what was on the screen).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/app_commands.hpp"
#include "core/waterfall_history.hpp"
#include "dsp/trace_average.hpp"
#include "engine/engine.hpp"
#include "engine/wire_a.hpp"

namespace cascade::engine {

namespace ta = cascade::dsp;
namespace wh = cascade::core::wfhistory;
using nlohmann::json;

struct WireA::FrameChain {
    ta::TraceAverager avg;
    ta::SavitzkyGolay sg;
    ta::DeComb comb;
    std::vector<float> a, b;
    std::uint64_t key = 0;
};

struct WireA::Wf {
    std::unique_ptr<wh::WaterfallHistory> hist;
    bool on = false;
    std::size_t mb = 192;
    std::size_t bins = 0;
};

namespace {
const char* modeWord(int m) { return ta::avgModeName(static_cast<ta::AvgMode>(m)); }
const char* smoothWord(int s) { return s == 1 ? "moving" : (s == 2 ? "savgol" : "none"); }
}  // namespace

std::string WireA::averageJson() const {
    return json{{"average", modeWord(avgCfg_.mode)}, {"frames", avgCfg_.frames}, {"timeConstantS", avgCfg_.tauS}, {"smoothing", smoothWord(avgCfg_.smoothing)}, {"smoothWidth", avgCfg_.width},
                {"smoothOrder", avgCfg_.order}, {"comb", avgCfg_.comb}, {"combPeriod", avgCfg_.period}, {"combStrength", avgCfg_.strength}, {"dcBins", avgCfg_.dcBins}, {"combMemory", avgCfg_.memory}}
        .dump();
}

namespace {
bool parseAverage(const std::string& text, int& mode, long long& frames, double& tau, int& smoothing, long long& width, long long& order, bool& comb, long long& period, double& strength, long long& dcBins,
                  double& memory, std::string& err) {
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { err = "the text is not a JSON object"; return false; }
    auto isNum = [&](const char* k) { return j.contains(k) && j[k].is_number() && std::isfinite(j[k].get<double>()); };
    auto bad = [&](const char* k) { err = std::string("the field ") + k + " is not valid"; return false; };
    if (j.contains("average")) {
        if (!j["average"].is_string()) { return bad("average"); }
        const std::string w = j["average"].get<std::string>();
        if (w == "off") { mode = 0; }
        else if (w == "linear") { mode = 1; }
        else if (w == "rms") { mode = 2; }
        else if (w == "log") { mode = 3; }
        else if (w == "exponential") { mode = 4; }
        else { return bad("average"); }
    }
    if (j.contains("frames")) { if (!isNum("frames")) { return bad("frames"); } frames = static_cast<long long>(j["frames"].get<double>()); if (frames < 1 || frames > 128) { return bad("frames"); } }
    if (j.contains("timeConstantS")) { if (!isNum("timeConstantS")) { return bad("timeConstantS"); } tau = j["timeConstantS"].get<double>(); if (tau < 0.005 || tau > 60.0) { return bad("timeConstantS"); } }
    if (j.contains("smoothing")) {
        if (!j["smoothing"].is_string()) { return bad("smoothing"); }
        const std::string w = j["smoothing"].get<std::string>();
        if (w == "none") { smoothing = 0; } else if (w == "moving") { smoothing = 1; } else if (w == "savgol") { smoothing = 2; } else { return bad("smoothing"); }
    }
    if (j.contains("smoothWidth")) { if (!isNum("smoothWidth")) { return bad("smoothWidth"); } width = static_cast<long long>(j["smoothWidth"].get<double>()); if (width < 3 || width > 101 || width % 2 == 0) { return bad("smoothWidth"); } }
    if (j.contains("smoothOrder")) { if (!isNum("smoothOrder")) { return bad("smoothOrder"); } order = static_cast<long long>(j["smoothOrder"].get<double>()); if (order < 0 || order > 8) { return bad("smoothOrder"); } }
    if (smoothing == 2 && order >= width) { return bad("smoothOrder"); }
    if (j.contains("comb")) { if (!j["comb"].is_boolean()) { return bad("comb"); } comb = j["comb"].get<bool>(); }
    if (j.contains("combPeriod")) { if (!isNum("combPeriod")) { return bad("combPeriod"); } period = static_cast<long long>(j["combPeriod"].get<double>()); if (period != 0 && period < 4) { return bad("combPeriod"); } }
    if (j.contains("combStrength")) { if (!isNum("combStrength")) { return bad("combStrength"); } strength = j["combStrength"].get<double>(); if (strength < 0.0 || strength > 1.0) { return bad("combStrength"); } }
    if (j.contains("dcBins")) { if (!isNum("dcBins")) { return bad("dcBins"); } dcBins = static_cast<long long>(j["dcBins"].get<double>()); if (dcBins < 0 || dcBins > 9) { return bad("dcBins"); } }
    if (j.contains("combMemory")) { if (!isNum("combMemory")) { return bad("combMemory"); } memory = j["combMemory"].get<double>(); if (memory < 0.0 || memory > 0.99) { return bad("combMemory"); } }
    return true;
}
}  // namespace

namespace {
bool traceModeWord(const std::string& w) {
    static const char* const ok[] = {"normal", "peak_hold", "min_hold", "average", "persistence_fast", "persistence_medium", "persistence_slow"};
    for (const char* o : ok) { if (w == o) { return true; } }
    return false;
}
}  // namespace

WireA::Result WireA::traceModeOp(const std::string& text, Result res) {
    framesLoadOnce();
    if (!traceModeWord(text)) { return refused(res, FOXAPI_OUT_OF_RANGE, "the mode is normal, peak_hold, min_hold, average, persistence_fast, persistence_medium or persistence_slow"); }
    if (text == traceMode_) { return refused(res, FOXAPI_NO_CHANGE, "that mode is in force already"); }
    traceMode_ = text;
    traceError_.clear();
    saveFile("trace-mode.json", json{{"version", 1}, {"mode", traceMode_}}.dump() + "\n");
    bump();
    return res;
}

void WireA::framesLoadOnce() {
    if (framesLoaded_ || dir().empty()) { return; }
    framesLoaded_ = true;
    {
        const std::string t = readFile("trace-mode.json");
        if (!t.empty()) {
            const json j = json::parse(t, nullptr, false);
            if (!j.is_discarded() && j.is_object() && j.contains("mode") && j["mode"].is_string() && traceModeWord(j["mode"].get<std::string>())) {
                traceMode_ = j["mode"].get<std::string>();
            } else {
                traceError_ = "the trace mode file is not valid";
            }
            bump();
        }
    }
    const std::string text = readFile("display-average.json");
    if (text.empty()) { return; }
    AverageCfg c;
    std::string err;
    if (parseAverage(text, c.mode, c.frames, c.tauS, c.smoothing, c.width, c.order, c.comb, c.period, c.strength, c.dcBins, c.memory, err)) {
        avgCfg_ = c;
        for (auto& ch : chains_) { ch.reset(); }
        bump();
    }
}

WireA::Result WireA::framesOp(const FoxCommand& c, const std::string& text, Result res) {
    framesLoadOnce();
    if (c.op == FOXAPP_OP_WF_HISTORY) { return wfOp(c, res); }
    AverageCfg n;
    std::string err;
    if (!parseAverage(text, n.mode, n.frames, n.tauS, n.smoothing, n.width, n.order, n.comb, n.period, n.strength, n.dcBins, n.memory, err)) { return refused(res, FOXAPI_BAD_ARGUMENT, err); }
    const AverageCfg before = avgCfg_;
    avgCfg_ = n;
    const std::string now = averageJson();
    avgCfg_ = before;
    if (now == averageJson()) { return refused(res, FOXAPI_NO_CHANGE, "that is in force already"); }
    avgCfg_ = n;
    for (auto& ch : chains_) { ch.reset(); }   // each chain is rebuilt from the setting at its next frame
    saveFile("display-average.json", now + "\n");
    bump();
    return res;
}

void WireA::displayFrame(std::size_t slot, std::vector<float>& bins, double centreHz, double spanHz, double nowS) {
    if (slot >= chains_.size() || bins.empty()) { return; }
    if (avgCfg_.active()) {
        std::shared_ptr<FrameChain>& ch = chains_[slot];
        if (!ch) {
            ch = std::make_shared<FrameChain>();
            ch->avg.setMode(static_cast<ta::AvgMode>(avgCfg_.mode));
            ch->avg.setFrameCount(static_cast<std::size_t>(avgCfg_.frames));
            ch->avg.setTimeConstantS(avgCfg_.tauS);
            ch->sg.configure(static_cast<std::size_t>(avgCfg_.width), static_cast<int>(avgCfg_.order));
            ta::DeCombSettings s;
            s.period = static_cast<std::size_t>(avgCfg_.period);
            s.strength = avgCfg_.strength;
            s.dcBins = static_cast<int>(avgCfg_.dcBins);
            s.profileMemory = avgCfg_.memory;
            ch->comb.setSettings(s);
        }
        const std::size_t n = bins.size();
        std::uint64_t key = static_cast<std::uint64_t>(std::llround(centreHz)) * 1000003ull + static_cast<std::uint64_t>(std::llround(spanHz)) * 7919ull + n;
        ch->key = key;
        const float* cur = bins.data();
        if (avgCfg_.mode != 0) {
            ch->avg.push(bins.data(), static_cast<int>(n), nowS, key);
            if (ch->avg.trace().size() == n) { cur = ch->avg.trace().data(); }
        }
        ch->a.assign(cur, cur + n);
        if (avgCfg_.smoothing == 1) {
            ch->b.resize(n);
            ta::smoothMovingAverage(ch->a.data(), ch->b.data(), n, static_cast<std::size_t>(avgCfg_.width));
            ch->a.swap(ch->b);
        } else if (avgCfg_.smoothing == 2) {
            ch->b.resize(n);
            ch->sg.apply(ch->a.data(), ch->b.data(), n);
            ch->a.swap(ch->b);
        }
        if (avgCfg_.comb) {
            ch->b.resize(n);
            ch->comb.process(ch->a.data(), ch->b.data(), n);
            ch->a.swap(ch->b);
        }
        std::memcpy(bins.data(), ch->a.data(), n * sizeof(float));
    }
    // the history keeps what is drawn
    if (std::shared_ptr<Wf> w = wf_[slot]; w && w->on) {
        if (!w->hist || w->bins != bins.size()) {
            wh::Config cfg;
            cfg.bins = bins.size();
            cfg.maxBytes = w->mb * 1024u * 1024u;
            if (!w->hist) {
                w->hist = std::make_unique<wh::WaterfallHistory>(cfg);
            } else {
                w->hist->reset(cfg);
            }
            w->bins = bins.size();
        }
        if (w->hist && w->hist->valid()) {
            const std::int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            (void)w->hist->push(bins.data(), ms, centreHz, spanHz);
        }
    }
}

WireA::Result WireA::wfOp(const FoxCommand& c, Result res) {
    const std::size_t slot = e_.scopedSlot();
    if (slot >= wf_.size()) { return refused(res, FOXAPI_NO_DEVICE, "there is no radio with that number"); }
    std::shared_ptr<Wf>& w = wf_[slot];
    if (c.ival[0] == 5) {
        if (c.ival[1] != 0 && c.ival[1] != 1) { return refused(res, FOXAPI_OUT_OF_RANGE, "ival[1] is 1 on or 0 off"); }
        const bool on = c.ival[1] == 1;
        const double mbIn = c.num[0] == 0.0 ? 192.0 : c.num[0];
        if (on && (!(mbIn >= 16.0) || mbIn > 1024.0)) { return refused(res, FOXAPI_OUT_OF_RANGE, "the size is 16 to 1024 megabytes (0 = 192)"); }
        if (on) {
            if (!w) { w = std::make_shared<Wf>(); }
            const std::size_t mb = static_cast<std::size_t>(mbIn);
            if (w->on && w->mb == mb) { return refused(res, FOXAPI_NO_CHANGE, "the history is on already"); }
            w->on = true;
            w->mb = mb;
            w->hist.reset();   // allocated at the next frame, at this size
            w->bins = 0;
        } else {
            if (!w || !w->on) { return refused(res, FOXAPI_NO_CHANGE, "the history is off already"); }
            w.reset();
        }
        bump();
        return res;
    }
    if (!w || !w->on || !w->hist) { return refused(res, FOXAPI_NO_CHANGE, "the history is off or has no line yet"); }
    const std::size_t rows = c.num[1] >= 1.0 && c.num[1] <= 100000.0 ? static_cast<std::size_t>(c.num[1]) : 256u;
    const wh::State before = w->hist->state();
    switch (c.ival[0]) {
        case 0: w->hist->pause(); break;
        case 1: w->hist->jumpToLive(); break;
        case 2:
        case 3: {
            if (!(c.num[0] >= 1.0) || c.num[0] > 1.0e9) { return refused(res, FOXAPI_OUT_OF_RANGE, "give a number of lines"); }
            if (c.ival[0] == 2) { w->hist->scrollBack(static_cast<std::size_t>(c.num[0]), rows); } else { w->hist->scrollForward(static_cast<std::size_t>(c.num[0])); }
            break;
        }
        case 4:
            if (!(c.num[0] >= 0.0) || c.num[0] > 1.0e15) { return refused(res, FOXAPI_OUT_OF_RANGE, "give a time in unix milliseconds"); }
            (void)w->hist->seekToTime(static_cast<std::int64_t>(c.num[0]), rows);
            break;
        default:
            return refused(res, FOXAPI_OUT_OF_RANGE, "ival[0] is 0 pause, 1 live, 2 back, 3 forward, 4 seek or 5 on / off");
    }
    const wh::State after = w->hist->state();
    res.applied[0] = static_cast<double>(after.behindLines);
    res.applied[1] = after.held ? 1.0 : 0.0;
    if (after.held == before.held && after.viewNewest == before.viewNewest) { return refused(res, FOXAPI_NO_CHANGE, "the view is there already"); }
    bump();
    return res;
}

std::size_t WireA::waterfallCopy(std::size_t slot, std::size_t rows, std::vector<std::uint8_t>& out, std::size_t& bins, std::vector<wh::LineMeta>* meta) {
    bins = 0;
    out.clear();
    if (slot >= wf_.size() || !wf_[slot] || !wf_[slot]->hist) { return 0; }
    wh::WaterfallHistory& h = *wf_[slot]->hist;
    bins = h.bins();
    if (bins == 0 || rows == 0) { return 0; }
    out.resize(rows * bins);
    if (meta != nullptr) { meta->assign(rows, wh::LineMeta()); }
    const std::size_t got = h.copyViewU8(rows, out.data(), meta != nullptr ? meta->data() : nullptr);
    out.resize(got * bins);
    if (meta != nullptr) { meta->resize(got); }
    return got;
}

void WireA::framesFacts(const std::function<void(const std::string&, const std::string&)>& fact) const {
    fact("display.average", averageJson());
    fact("display.trace", traceMode_);
    fact("display.trace.error", traceError_);
    for (std::size_t slot = 0; slot < wf_.size(); ++slot) {
        if (!e_.receiverLive(static_cast<std::uint32_t>(slot) + 1u)) { continue; }
        const std::string k = "wf." + std::to_string(slot + 1) + ".";
        const std::shared_ptr<Wf>& w = wf_[slot];
        fact(k + "on", w && w->on ? "1" : "0");
        if (!w || !w->on) { continue; }
        fact(k + "megabytes", std::to_string(w->mb));
        if (w->hist) {
            const wh::State s = w->hist->state();
            fact(k + "lines", std::to_string(s.stored));
            fact(k + "behind", std::to_string(s.behindLines));
            fact(k + "held", s.held ? "1" : "0");
            fact(k + "slid", s.viewSlid ? "1" : "0");
            fact(k + "epoch", std::to_string(s.epoch));
            fact(k + "capacity", std::to_string(w->hist->capacityLines()));
        }
    }
}

}  // namespace cascade::engine
