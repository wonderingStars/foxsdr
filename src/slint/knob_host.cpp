// knob_host.cpp - see knob_host.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "knob_host.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <utility>

namespace foxsdr::slintknobs {

namespace ci = cascade::input;

bool applyAction(const ci::ActionOut& a, const Callbacks& cb) {
    // a knob must never key a transmitter, whatever the mapping file said (the layer refuses these names at load: this is the second lock)
    if (ci::isForbiddenName(a.action)) { return false; }
    const int detents = static_cast<int>(std::lround(a.amount));
    if (a.action == "tune" || a.action == "tune.shuttle") {
        if (detents == 0 || !cb.tune) { return false; }
        cb.tune(detents);
        return true;
    }
    if (a.action == "tune.fine") {
        if (detents == 0 || !cb.tuneFine) { return false; }
        const int divisor = a.params.is_object() && a.params.value("divisor", 10) > 0 ? a.params.value("divisor", 10) : 10;
        cb.tuneFine(detents, divisor);
        return true;
    }
    if (a.action == "volume") { if (!cb.volume || a.amount == 0.0) { return false; } cb.volume(a.amount); return true; }
    if (a.action == "squelch") { if (!cb.squelch || a.amount == 0.0) { return false; } cb.squelch(a.amount); return true; }
    if (a.action == "zoom") {
        if (detents == 0 || !cb.zoom) { return false; }
        cb.zoom(std::pow(kZoomPerDetent, static_cast<double>(detents)));
        return true;
    }
    if (a.action == "step.up" || a.action == "step.down") {
        if (!cb.stepSize) { return false; }
        cb.stepSize(a.action == "step.up" ? 1 : -1);
        return true;
    }
    if (a.action == "mode.next" || a.action == "mode.prev") {
        if (!cb.mode) { return false; }
        cb.mode(a.action == "mode.next" ? 1 : -1);
        return true;
    }
    if (a.action == "radio.select") {
        const int n = a.params.is_object() ? a.params.value("n", 0) : 0;
        if (n < 1 || n > 8 || !cb.radioSelect) { return false; }
        cb.radioSelect(n);
        return true;
    }
    if (a.action == "command") {
        const std::string name = a.params.is_object() ? a.params.value("name", std::string()) : std::string();
        if (name.empty() || ci::isForbiddenName(name) || !cb.command) { return false; }
        cb.command(name);
        return true;
    }
    return false;
}

struct KnobHost::Impl {
    Env env;
    bool enabled = false;
    std::vector<FoundDevice> found;
    std::set<std::string> use;                       // ids the person marked
    struct Open { std::shared_ptr<void> handle; std::string error; };
    std::map<std::string, Open> open;                // id -> what is open (or why not)
    ci::EventQueue queue{1024};
    ci::Mapper mapper;
    std::uint64_t seen = 0;
    mutable std::mutex mu;

    std::int64_t now() const { return env.nowUs ? env.nowUs() : ci::nowMicros(); }

    void rebuildMapping() {
        ci::Settings s;
        for (const auto& kv : open) {
            if (!kv.second.handle) { continue; }
            const auto it = std::find_if(found.begin(), found.end(), [&](const FoundDevice& d) { return d.id == kv.first; });
            if (it == found.end()) { continue; }
            const ci::Settings p = it->preset == 1 ? ci::powerMatePreset(it->id) : ci::shuttleXpressPreset(it->id);
            s.bindings.insert(s.bindings.end(), p.bindings.begin(), p.bindings.end());
        }
        mapper.setSettings(std::move(s));
    }

    void tryOpen(const FoundDevice& d) {
        if (!enabled || use.count(d.id) == 0 || !env.open) { return; }
        Open& o = open[d.id];
        if (o.handle) { return; }
        std::string err;
        o.handle = env.open(d, queue, err);
        o.error = o.handle ? std::string() : (err.empty() ? std::string("could not be opened") : err);
    }

    void closeAll() {
        open.clear();
        rebuildMapping();
    }
};

KnobHost::KnobHost(Env env) : p_(std::make_unique<Impl>()) { p_->env = std::move(env); }
KnobHost::~KnobHost() = default;

void KnobHost::setEnabled(bool on) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->enabled == on) { return; }
    p_->enabled = on;
    if (!on) { p_->closeAll(); return; }
    if (p_->found.empty() && p_->env.list) { p_->found = p_->env.list(); }
    for (const FoundDevice& d : p_->found) { p_->tryOpen(d); }
    p_->rebuildMapping();
}

bool KnobHost::enabled() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->enabled; }

void KnobHost::scan() {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->env.list) { p_->found = p_->env.list(); }
    // a device that is no longer listed is closed
    for (auto it = p_->open.begin(); it != p_->open.end();) {
        const bool listed = std::any_of(p_->found.begin(), p_->found.end(), [&](const FoundDevice& d) { return d.id == it->first; });
        it = listed ? std::next(it) : p_->open.erase(it);
    }
    for (const FoundDevice& d : p_->found) { p_->tryOpen(d); }
    p_->rebuildMapping();
}

bool KnobHost::setUse(const std::string& id, bool use) {
    std::lock_guard<std::mutex> lk(p_->mu);
    const auto it = std::find_if(p_->found.begin(), p_->found.end(), [&](const FoundDevice& d) { return d.id == id; });
    if (it == p_->found.end()) { return false; }
    if (use) { p_->use.insert(id); p_->tryOpen(*it); }
    else { p_->use.erase(id); p_->open.erase(id); }
    p_->rebuildMapping();
    return true;
}

void KnobHost::setUsedIds(const std::vector<std::string>& ids) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->use = std::set<std::string>(ids.begin(), ids.end());
}

std::vector<std::string> KnobHost::usedIds() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return std::vector<std::string>(p_->use.begin(), p_->use.end());
}

std::vector<Status> KnobHost::status() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    std::vector<Status> out;
    for (const FoundDevice& d : p_->found) {
        Status s;
        s.id = d.id;
        s.name = d.name;
        s.use = p_->use.count(d.id) > 0;
        const auto it = p_->open.find(d.id);
        if (it != p_->open.end()) {
            s.open = it->second.handle != nullptr;
            s.error = it->second.error;
            if (s.open && p_->env.reports) { s.reports = p_->env.reports(it->second.handle); }
        }
        out.push_back(std::move(s));
    }
    return out;
}

int KnobHost::tick() {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->enabled) { return 0; }
    std::vector<ci::InputEvent> ev;
    p_->queue.drain(ev);
    int ran = 0;
    for (const ci::InputEvent& e : ev) {
        ++p_->seen;
        if (e.kind == ci::EventKind::Disconnected) {
            // the device was unplugged: close it and say so; it opens again at the next look while it is marked
            for (auto& kv : p_->open) {
                if (ci::deviceIdFor(kv.first) == e.device) { kv.second.handle.reset(); kv.second.error = "The device was unplugged."; }
            }
            p_->rebuildMapping();
            continue;
        }
        for (const ci::ActionOut& a : p_->mapper.process(e)) { if (applyAction(a, p_->env.act)) { ++ran; } }
    }
    for (const ci::ActionOut& a : p_->mapper.tick(p_->now())) { if (applyAction(a, p_->env.act)) { ++ran; } }
    return ran;
}

cascade::input::EventQueue& KnobHost::queue() { return p_->queue; }

std::uint64_t KnobHost::eventsSeen() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->seen; }

}  // namespace foxsdr::slintknobs
