// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "mapping.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace cascade::input {

using nlohmann::json;

const char* const kActionNames[] = {"tune",      "tune.fine", "tune.shuttle", "volume",    "squelch",   "zoom",    "step.up",
                                    "step.down", "mode.next", "mode.prev",    "radio.select", "command"};
const std::size_t kActionCount = sizeof kActionNames / sizeof kActionNames[0];

bool isKnownAction(const std::string& name) {
    for (std::size_t i = 0; i < kActionCount; ++i) {
        if (name == kActionNames[i]) { return true; }
    }
    return false;
}

bool isForbiddenName(const std::string& name) {
    std::string l;
    for (unsigned char c : name) { l += static_cast<char>(std::tolower(c)); }
    return l.rfind("transmit", 0) == 0 || l.rfind("ptt", 0) == 0;
}

namespace {

double clampD(double v, double lo, double hi, LoadReport* r) {
    if (!(v >= lo)) { if (r) { ++r->clamped; } return lo; }  // also catches NaN
    if (v > hi) { if (r) { ++r->clamped; } return hi; }
    return v;
}

int clampI(long long v, int lo, int hi, LoadReport* r) {
    if (v < lo) { if (r) { ++r->clamped; } return lo; }
    if (v > hi) { if (r) { ++r->clamped; } return hi; }
    return static_cast<int>(v);
}

bool takeNumber(const json& o, const char* key, double& out) {
    const auto it = o.find(key);
    if (it == o.end() || !it->is_number()) { return false; }
    out = it->get<double>();
    return true;
}

const char* const kBindingKeys[] = {"device", "control", "action", "params", "encoding", "invert", "accel", "deadZone", "minRateHz", "maxRateHz"};

json extraOf(const json& o, std::initializer_list<const char*> known) {
    json x = json::object();
    for (auto it = o.begin(); it != o.end(); ++it) {
        bool k = false;
        for (const char* n : known) { if (it.key() == n) { k = true; } }
        if (!k) { x[it.key()] = it.value(); }
    }
    return x;
}

}  // namespace

json settingsToJson(const Settings& s) {
    json j = s.extra.is_object() ? s.extra : json::object();
    j["version"] = s.version;
    if (s.modifier) {
        j["modifier"] = {{"device", s.modifier->device}, {"control", s.modifier->control}, {"factor", s.modifier->factor}};
    } else {
        j.erase("modifier");
    }
    json arr = json::array();
    for (const Binding& b : s.bindings) {
        json o = b.extra.is_object() ? b.extra : json::object();
        o["device"] = b.device;
        o["control"] = b.control;
        o["action"] = b.action;
        o["params"] = b.params.is_object() ? b.params : json::object();
        o["encoding"] = midiEncodingName(b.encoding);
        o["invert"] = b.invert;
        o["accel"] = {{"slowPerSec", b.accel.slowPerSec}, {"fastPerSec", b.accel.fastPerSec}, {"maxFactor", b.accel.maxFactor}};
        o["deadZone"] = b.deadZone;
        o["minRateHz"] = b.minRateHz;
        o["maxRateHz"] = b.maxRateHz;
        arr.push_back(std::move(o));
    }
    j["bindings"] = std::move(arr);
    return j;
}

Settings settingsFromJson(const json& j, LoadReport* rep) {
    Settings s;
    LoadReport local;
    LoadReport* r = rep ? rep : &local;
    if (!j.is_object()) { return s; }
    s.extra = extraOf(j, {"version", "modifier", "bindings"});
    if (const auto v = j.find("version"); v != j.end() && v->is_number_integer()) { s.version = v->get<int>(); }

    if (const auto m = j.find("modifier"); m != j.end() && m->is_object()) {
        Modifier mod;
        double c = -1, f = 10;
        if (m->contains("device") && (*m)["device"].is_string() && takeNumber(*m, "control", c) && c >= 0 && c <= 65535) {
            mod.device = (*m)["device"].get<std::string>();
            mod.control = static_cast<std::uint16_t>(c);
            if (takeNumber(*m, "factor", f)) { mod.factor = clampD(f, 1.0, 100.0, r); }
            s.modifier = mod;
        } else {
            ++r->skippedMalformed;
        }
    }

    const auto arr = j.find("bindings");
    if (arr == j.end() || !arr->is_array()) { return s; }
    std::map<std::pair<std::string, std::uint16_t>, std::size_t> seen;
    for (const json& o : *arr) {
        double ctl = -1;
        if (!o.is_object() || !o.contains("device") || !o["device"].is_string() || !o.contains("action") || !o["action"].is_string() || !takeNumber(o, "control", ctl) ||
            ctl < 0 || ctl > 65535 || ctl != std::floor(ctl)) {
            ++r->skippedMalformed;
            continue;
        }
        Binding b;
        b.device = o["device"].get<std::string>();
        b.control = static_cast<std::uint16_t>(ctl);
        b.action = o["action"].get<std::string>();
        if (b.device.empty() || b.action.empty()) { ++r->skippedMalformed; continue; }
        if (const auto p = o.find("params"); p != o.end() && p->is_object()) { b.params = *p; }
        if (isForbiddenName(b.action) || (b.params.contains("name") && b.params["name"].is_string() && isForbiddenName(b.params["name"].get<std::string>()))) {
            ++r->refusedForbidden;
            continue;
        }
        if (const auto e = o.find("encoding"); e != o.end()) {
            MidiEncoding enc = MidiEncoding::Absolute7;
            if (e->is_string() && midiEncodingFromName(e->get<std::string>(), enc)) { b.encoding = enc; } else { ++r->unknownEncodings; }
        }
        if (const auto i = o.find("invert"); i != o.end() && i->is_boolean()) { b.invert = i->get<bool>(); }
        if (const auto a = o.find("accel"); a != o.end() && a->is_object()) {
            double v;
            if (takeNumber(*a, "slowPerSec", v)) { b.accel.slowPerSec = clampD(v, 0.0, 1000.0, r); }
            if (takeNumber(*a, "fastPerSec", v)) { b.accel.fastPerSec = clampD(v, 0.0, 1000.0, r); }
            if (takeNumber(*a, "maxFactor", v)) { b.accel.maxFactor = clampD(v, 1.0, 20.0, r); }
            if (b.accel.fastPerSec <= b.accel.slowPerSec) { b.accel.fastPerSec = b.accel.slowPerSec + 1.0; }
        }
        double v;
        if (takeNumber(o, "deadZone", v)) { b.deadZone = clampI(static_cast<long long>(std::llround(std::isfinite(v) ? v : 0)), 0, 6, r); }
        if (takeNumber(o, "minRateHz", v)) { b.minRateHz = clampD(v, 0.1, 200.0, r); }
        if (takeNumber(o, "maxRateHz", v)) { b.maxRateHz = clampD(v, 0.1, 200.0, r); }
        if (b.maxRateHz < b.minRateHz) { std::swap(b.minRateHz, b.maxRateHz); }
        b.extra = extraOf(o, {kBindingKeys[0], kBindingKeys[1], kBindingKeys[2], kBindingKeys[3], kBindingKeys[4], kBindingKeys[5], kBindingKeys[6],
                              kBindingKeys[7], kBindingKeys[8], kBindingKeys[9]});
        ++r->bindingsRead;
        const auto key = std::make_pair(b.device, b.control);
        if (const auto it = seen.find(key); it != seen.end()) {
            s.bindings[it->second] = std::move(b);  // a later entry for the same control replaces the earlier one
        } else {
            seen[key] = s.bindings.size();
            s.bindings.push_back(std::move(b));
        }
    }
    return s;
}

Settings settingsFromText(const std::string& text, LoadReport* rep) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded()) {
        if (rep) { rep->unparseable = true; }
        return Settings{};
    }
    return settingsFromJson(j, rep);
}

std::string settingsToText(const Settings& s) { return settingsToJson(s).dump(2); }

namespace {
Binding mkBinding(const std::string& dev, std::uint16_t ctl, const char* action, json params = json::object()) {
    Binding b;
    b.device = dev;
    b.control = ctl;
    b.action = action;
    b.params = std::move(params);
    return b;
}
}  // namespace

Settings shuttleXpressPreset(const std::string& dev) {
    Settings s;
    s.bindings.push_back(mkBinding(dev, 1, "tune"));
    s.bindings.back().accel.maxFactor = 1.0;
    s.bindings.push_back(mkBinding(dev, 2, "tune.shuttle"));
    s.bindings.push_back(mkBinding(dev, 0x10, "step.down"));
    s.bindings.push_back(mkBinding(dev, 0x11, "step.up"));
    s.bindings.push_back(mkBinding(dev, 0x12, "mode.next"));
    s.bindings.push_back(mkBinding(dev, 0x13, "command", {{"name", "mute"}}));
    s.bindings.push_back(mkBinding(dev, 0x14, "command", {{"name", "record"}}));
    return s;
}

Settings powerMatePreset(const std::string& dev) {
    Settings s;
    s.bindings.push_back(mkBinding(dev, 1, "tune"));
    s.bindings.push_back(mkBinding(dev, 0x10, "command", {{"name", "mute"}}));
    return s;
}

// ------------------------------------------------------------------------------------------------ Mapper

void Mapper::setSettings(Settings s) {
    s_ = std::move(s);
    index_.clear();
    lastUs_.clear();
    rings_.clear();
    modDown_ = false;
    for (std::size_t i = 0; i < s_.bindings.size(); ++i) {
        index_[{deviceIdFor(s_.bindings[i].device), s_.bindings[i].control}] = i;
    }
    modDevice_ = s_.modifier ? deviceIdFor(s_.modifier->device) : 0;
}

const Binding* Mapper::find(std::uint32_t dev, std::uint16_t control) const {
    const auto it = index_.find({dev, control});
    return it == index_.end() ? nullptr : &s_.bindings[it->second];
}

std::vector<std::pair<std::uint16_t, MidiControlConfig>> Mapper::midiConfigsFor(const std::string& dev) const {
    std::vector<std::pair<std::uint16_t, MidiControlConfig>> out;
    for (const Binding& b : s_.bindings) {
        if (b.device == dev) { out.push_back({b.control, MidiControlConfig{b.encoding, false}}); }
    }
    return out;
}

namespace {
double scaleOf(const Binding& b) {
    if (const auto it = b.params.find("scale"); it != b.params.end() && it->is_number()) {
        const double v = it->get<double>();
        if (std::isfinite(v)) { return std::clamp(v, -1000.0, 1000.0); }
    }
    return 1.0;
}
bool absoluteCapable(const std::string& a) { return a == "volume" || a == "squelch" || a == "zoom"; }
int sgn(int v) { return v > 0 ? 1 : (v < 0 ? -1 : 0); }
}  // namespace

std::vector<ActionOut> Mapper::process(const InputEvent& e) {
    std::vector<ActionOut> out;
    if (e.kind == EventKind::Disconnected) {
        for (auto it = rings_.begin(); it != rings_.end();) { it = it->first.first == e.device ? rings_.erase(it) : std::next(it); }
        for (auto it = lastUs_.begin(); it != lastUs_.end();) { it = it->first.first == e.device ? lastUs_.erase(it) : std::next(it); }
        if (e.device == modDevice_) { modDown_ = false; }
        return out;
    }
    if (s_.modifier && e.device == modDevice_ && e.control == s_.modifier->control) {
        if (e.kind == EventKind::ButtonDown) { modDown_ = true; }
        if (e.kind == EventKind::ButtonUp) { modDown_ = false; }
    }
    const Binding* b = find(e.device, e.control);
    if (!b) { return out; }
    ActionOut a;
    a.action = b->action;
    a.params = b->params;
    a.device = e.device;
    a.control = e.control;
    a.timeUs = e.timeUs;
    const double mod = (modDown_ && s_.modifier) ? s_.modifier->factor : 1.0;

    switch (e.kind) {
        case EventKind::Relative: {
            if (b->action == "tune.shuttle" || e.raw == 0) { return out; }
            double amt = static_cast<double>(e.raw);
            if (b->invert) { amt = -amt; }
            if (b->accel.maxFactor > 1.0) {
                const auto key = std::make_pair(e.device, e.control);
                double speed = 0.0;
                if (const auto it = lastUs_.find(key); it != lastUs_.end()) {
                    const double dt = std::max(0.001, static_cast<double>(e.timeUs - it->second) / 1e6);
                    speed = std::fabs(static_cast<double>(e.raw)) / dt;
                }
                const double t = std::clamp((speed - b->accel.slowPerSec) / (b->accel.fastPerSec - b->accel.slowPerSec), 0.0, 1.0);
                amt *= 1.0 + (b->accel.maxFactor - 1.0) * t;
            }
            lastUs_[{e.device, e.control}] = e.timeUs;
            a.amount = amt * scaleOf(*b) * mod;
            out.push_back(std::move(a));
            return out;
        }
        case EventKind::Absolute: {
            if (b->action == "tune.shuttle") {
                Ring& rg = rings_[{e.device, e.control}];
                const int defl = b->invert ? -e.raw : e.raw;
                const int was = std::abs(rg.defl) > b->deadZone ? sgn(rg.defl) : 0;
                const int now = std::abs(defl) > b->deadZone ? sgn(defl) : 0;
                rg.defl = defl;
                if (now != was) {
                    rg.acc = 0.0;
                    rg.lastUs = e.timeUs;
                    if (now != 0) { a.amount = now * scaleOf(*b); out.push_back(std::move(a)); }  // the first detent, at once
                }
                return out;
            }
            if (!absoluteCapable(b->action)) { return out; }
            a.absolute = true;
            a.amount = b->invert ? 1.0 - static_cast<double>(e.value) : static_cast<double>(e.value);
            out.push_back(std::move(a));
            return out;
        }
        case EventKind::ButtonDown: {
            if (b->action == "tune.shuttle") { return out; }
            double amt = 1.0;
            if (const auto it = b->params.find("amount"); it != b->params.end() && it->is_number() && std::isfinite(it->get<double>())) { amt = it->get<double>(); }
            a.amount = (b->invert ? -amt : amt) * mod;
            out.push_back(std::move(a));
            return out;
        }
        default: return out;
    }
}

std::vector<ActionOut> Mapper::tick(std::int64_t nowUs) {
    std::vector<ActionOut> out;
    for (auto& [key, rg] : rings_) {
        const Binding* b = find(key.first, key.second);
        if (!b) { continue; }
        const int mag = std::abs(rg.defl);
        if (mag <= b->deadZone) { continue; }
        const double dt = std::clamp(static_cast<double>(nowUs - rg.lastUs) / 1e6, 0.0, 0.25);
        rg.lastUs = nowUs;
        const int span = std::max(1, 7 - b->deadZone - 1);
        const double frac = std::clamp(static_cast<double>(mag - b->deadZone - 1) / span, 0.0, 1.0);
        const double rate = b->minRateHz + (b->maxRateHz - b->minRateHz) * frac;
        rg.acc += rate * dt;
        const double whole = std::floor(rg.acc);
        if (whole >= 1.0) {
            rg.acc -= whole;
            ActionOut a;
            a.action = b->action;
            a.params = b->params;
            a.device = key.first;
            a.control = key.second;
            a.timeUs = nowUs;
            a.amount = sgn(rg.defl) * whole * scaleOf(*b);
            out.push_back(std::move(a));
        }
    }
    return out;
}

}  // namespace cascade::input
