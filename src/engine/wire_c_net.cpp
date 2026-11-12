// wire_c_net.cpp - see wire_c_net.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/wire_c_net.hpp"

#include <cstdio>

namespace cascade::engine::wirec {

bool RadioInfoFollow::configure(bool enabled, const std::string& address, int port, bool allowOther) {
    const bool was = running();
    if (listener_) {
        acceptedBefore_ = listener_->accepted();
        refusedBefore_ = listener_->refused();
    }
    listener_.reset();
    table_ = cascade::core::rig::RadioInfoTable{30};
    lastRender_.clear();
    error_.clear();
    if (!enabled || port <= 0) {
        if (enabled && port <= 0) { error_ = "give the port the logger sends its radio information to"; }
        return was;
    }
    auto l = std::make_unique<cascade::net::RadioInfoListener>();
    cascade::net::RadioInfoListenerConfig cfg;
    cfg.enabled = true;
    cfg.host = address;
    cfg.port = port;
    cfg.allowNonLoopback = allowOther;
    if (!l->start(cfg, error_)) { return was; }
    listener_ = std::move(l);
    return true;
}

bool RadioInfoFollow::poll(std::int64_t nowUnix) {
    nowUnix_ = nowUnix;
    if (!listener_) { return false; }
    listener_->poll(table_, nowUnix);
    const std::string now = render(nowUnix);
    if (now == lastRender_) { return false; }
    lastRender_ = now;
    return true;
}

std::string RadioInfoFollow::render(std::int64_t nowUnix) const {
    std::string out;
    for (int n = 1; n <= 4; ++n) {
        cascade::core::rig::RadioInfo r;
        if (!table_.get(n, nowUnix, r)) { continue; }
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%d|%.0f|%.0f|%d|%d|", n, r.freqHz, cascade::core::rig::RadioInfoTable::effectiveTxHz(r), r.split ? 1 : 0, r.transmitting ? 1 : 0);
        out += buf + r.mode + "|" + r.app + "\n";
    }
    return out;
}

std::string RadioInfoFollow::value(int radio, const std::string& what) const {
    cascade::core::rig::RadioInfo r;
    if (!table_.get(radio, nowUnix_, r)) { return std::string(); }
    char buf[48];
    if (what == "freq_hz") { std::snprintf(buf, sizeof(buf), "%.0f", r.freqHz); return buf; }
    if (what == "tx_hz") { std::snprintf(buf, sizeof(buf), "%.0f", cascade::core::rig::RadioInfoTable::effectiveTxHz(r)); return buf; }
    if (what == "mode") { return r.mode; }
    if (what == "split") { return r.split ? "1" : "0"; }
    if (what == "transmitting") { return r.transmitting ? "1" : "0"; }
    if (what == "app") { return r.app; }
    return std::string();
}

}  // namespace cascade::engine::wirec
