// wire_b_sweep.cpp - the sweep device host (wire_b_sweep.hpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/wire_b_sweep.hpp"

#include <algorithm>
#include <cmath>

#include "source/serial_port_win.hpp"

namespace cascade::engine::wireb {

namespace tsa = cascade::source::tinysa;

SerialLinkFactory defaultSerialLinkFactory() {
    return [](const std::string& port, std::string& why) -> std::unique_ptr<tsa::SerialLink> {
        if (!tsa::isValidComPortName(port)) {
            why = "the port name is not a COM port name (COM1 to COM256)";
            return nullptr;
        }
        auto link = std::make_unique<tsa::WinSerialLink>();
        if (!link->open(port, 100)) {
            why = "the sweep device's port could not be opened (is it plugged in, and free?)";
            return nullptr;
        }
        return link;
    };
}

bool SweepHost::start(const SerialLinkFactory& factory, const std::string& kind, const std::string& port, tsa::Model model, double startHz, double stopHz, std::size_t points, int intervalMs,
                      std::string& why) {
    stop();
    if (!factory) { why = "no way to open a sweep device on this build"; return false; }
    std::unique_ptr<tsa::SerialLink> link = factory(port, why);
    if (link == nullptr) {
        if (why.empty()) { why = "the sweep device could not be opened"; }
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(m_);
        link_ = std::move(link);
        req_ = tsa::SweepRequest{};
        req_.startHz = startHz;
        req_.stopHz = stopHz;
        req_.points = points;
        req_.model = model;
        intervalMs_ = std::max(0, intervalMs);
        kind_ = kind;
        sweeps_ = 0;
        refused_ = 0;
        lastError_.clear();
        haveLast_ = false;
        cascade::core::HeatmapConfig hc;
        hc.startHz = startHz;
        hc.stopHz = stopHz;
        std::string hwhy;
        heatReady_ = heat_.configure(hc, &hwhy);
    }
    stopFlag_.store(false);
    running_.store(true);
    th_ = std::thread([this] { loop(); });
    return true;
}

HackRfFactory defaultHackRfFactory() {
    return [](const std::string& args, std::string& why) -> std::unique_ptr<cascade::source::hackrf_sweep::HackRfSweeper> {
        auto sw = std::make_unique<cascade::source::hackrf_sweep::HackRfSweeper>();
        if (!sw->open(args)) {
            why = sw->lastError().empty() ? std::string("no HackRF could be opened (is it plugged in, with the WinUSB driver, and free?)") : sw->lastError();
            return nullptr;
        }
        return sw;
    };
}

bool SweepHost::startHackRf(const HackRfFactory& factory, const std::string& args, const cascade::source::hackrf_sweep::SweepConfig& cfg, std::string& why) {
    stop();
    if (!factory) { why = "no way to open a HackRF on this build"; return false; }
    std::unique_ptr<cascade::source::hackrf_sweep::HackRfSweeper> sw = factory(args, why);
    if (sw == nullptr) {
        if (why.empty()) { why = "the HackRF could not be opened"; }
        return false;
    }
    if (!sw->configure(cfg)) { why = sw->lastError().empty() ? std::string("the HackRF refused the sweep settings") : sw->lastError(); return false; }
    if (!sw->start()) { why = sw->lastError().empty() ? std::string("the HackRF would not start sweeping") : sw->lastError(); return false; }
    const double a = static_cast<double>(cfg.plan.rangesMHz.front().first) * 1e6, b = static_cast<double>(cfg.plan.rangesMHz.front().second) * 1e6;
    {
        std::lock_guard<std::mutex> lk(m_);
        hackrf_ = std::move(sw);
        link_.reset();
        req_ = cascade::source::tinysa::SweepRequest{};
        req_.startHz = a;
        req_.stopHz = b;
        req_.points = 0;
        kind_ = "hackrf";
        sweeps_ = 0;
        refused_ = 0;
        lastError_.clear();
        haveLast_ = false;
        cascade::core::HeatmapConfig hc;
        hc.startHz = a;
        hc.stopHz = b;
        std::string hwhy;
        heatReady_ = heat_.configure(hc, &hwhy);
    }
    stopFlag_.store(false);
    running_.store(true);
    th_ = std::thread([this] { loopHackRf(); });
    return true;
}

void SweepHost::loopHackRf() {
    std::vector<cascade::core::SweepLine> lines;
    while (!stopFlag_.load()) {
        lines.clear();
        const double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        const bool ok = hackrf_->poll(lines, 200, now);
        std::lock_guard<std::mutex> lk(m_);
        for (cascade::core::SweepLine& l : lines) {
            ++sweeps_;
            if (heatReady_) { std::string w; (void)heat_.addLine(l, &w); }
            if (occReady_) { std::string w; (void)occ_.addSweep(l, &w); }
            last_ = l;
            haveLast_ = true;
        }
        if (!ok) {
            lastError_ = "the HackRF stopped sending";
            running_.store(false);
            return;
        }
    }
}

void SweepHost::stop() {
    stopFlag_.store(true);
    if (th_.joinable()) { th_.join(); }
    running_.store(false);
    std::lock_guard<std::mutex> lk(m_);
    link_.reset();
    if (hackrf_ != nullptr) { hackrf_->stop(); hackrf_->close(); hackrf_.reset(); }
}

void SweepHost::loop() {
    while (!stopFlag_.load()) {
        tsa::SweepRequest req;
        int interval;
        tsa::SerialLink* link;
        {
            std::lock_guard<std::mutex> lk(m_);
            req = req_;
            interval = intervalMs_;
            link = link_.get();
        }
        req.timeS = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        cascade::core::SweepLine line;
        std::string why;
        const tsa::SweepResult r = tsa::sweepOnce(*link, req, line, &why);
        {
            std::lock_guard<std::mutex> lk(m_);
            if (r == tsa::SweepResult::Ok) {
                ++sweeps_;
                if (heatReady_) { std::string w; (void)heat_.addLine(line, &w); }
                if (occReady_) { std::string w; (void)occ_.addSweep(line, &w); }
                last_ = std::move(line);
                haveLast_ = true;
            } else if (r == tsa::SweepResult::Refused || r == tsa::SweepResult::Malformed) {
                ++refused_;
                lastError_ = r == tsa::SweepResult::Refused ? "the sweep device refused a sweep" : "the sweep device sent a damaged sweep";
            } else {
                // a lost device: the worker ends, the sentence says so (never the port)
                lastError_ = r == tsa::SweepResult::TimedOut ? "the sweep device stopped answering" : r == tsa::SweepResult::BadRequest ? "the sweep request was not valid" : "the sweep device's link failed";
                running_.store(false);
                return;
            }
        }
        if (interval > 0) {
            // sleep in small steps so stop() is prompt
            for (int waited = 0; waited < interval && !stopFlag_.load(); waited += 10) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
}

SweepStatus SweepHost::status() const {
    std::lock_guard<std::mutex> lk(m_);
    SweepStatus s;
    s.open = running_.load();
    s.kind = kind_;
    s.sweeps = sweeps_;
    s.refused = refused_;
    s.lastError = lastError_;
    s.points = req_.points;
    s.startHz = req_.startHz;
    s.stopHz = req_.stopHz;
    s.heatRows = heatReady_ ? heat_.rows() : 0;
    s.heatCols = heatReady_ ? heat_.cols() : 0;
    s.occChannels = occReady_ ? occ_.channels() : 0;
    s.occSweeps = occReady_ ? occ_.sweepsAccepted() : 0;
    return s;
}

bool SweepHost::configureHeatmap(const cascade::core::HeatmapConfig& cfg, std::string& why) {
    std::lock_guard<std::mutex> lk(m_);
    cascade::core::HeatmapConfig c = cfg;
    if (c.startHz == 0.0 && c.stopHz == 0.0) { c.startHz = req_.startHz; c.stopHz = req_.stopHz; }
    cascade::core::SweepHeatmap fresh;
    if (!fresh.configure(c, &why)) { return false; }
    heat_ = std::move(fresh);
    heatReady_ = true;
    return true;
}

bool SweepHost::configureOccupancy(const cascade::core::OccupancyConfig& cfg, std::string& why) {
    std::lock_guard<std::mutex> lk(m_);
    cascade::core::OccupancyModel fresh;
    if (!fresh.configure(cfg, &why)) { return false; }
    occ_ = std::move(fresh);
    occReady_ = true;
    return true;
}

void SweepHost::clearOccupancy() {
    std::lock_guard<std::mutex> lk(m_);
    occ_ = cascade::core::OccupancyModel{};
    occReady_ = false;
}

bool SweepHost::lastLine(std::size_t maxPoints, cascade::core::SweepLine& out) const {
    std::lock_guard<std::mutex> lk(m_);
    if (!haveLast_ || last_.db.empty() || maxPoints == 0) { return false; }
    out.startHz = last_.startHz;
    out.stopHz = last_.stopHz;
    out.timeS = last_.timeS;
    const std::size_t n = last_.db.size();
    if (n <= maxPoints) { out.db = last_.db; return true; }
    out.db.assign(maxPoints, std::nanf(""));
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t o = std::min(maxPoints - 1, i * maxPoints / n);
        const float v = last_.db[i];
        if (std::isfinite(v) && (!std::isfinite(out.db[o]) || v > out.db[o])) { out.db[o] = v; }
    }
    return true;
}

std::string SweepHost::occupancyCsv() {
    std::lock_guard<std::mutex> lk(m_);
    if (!occReady_) { return std::string(); }
    occ_.finish();
    return occ_.channelsCsv();
}

bool SweepHost::heatmapFrequencyAt(double x01, double& hz) const {
    std::lock_guard<std::mutex> lk(m_);
    if (!heatReady_) { return false; }
    hz = heat_.frequencyAtFraction(x01);
    return true;
}

}  // namespace cascade::engine::wireb
