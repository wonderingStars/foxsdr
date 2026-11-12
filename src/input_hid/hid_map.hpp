// input_hid/hid_map.hpp - generic HID button boxes: a device model built from a parsed report descriptor that turns reports into events, and a
// mapping from buttons / axes / encoders (by HID usage) to action names, with a JSON mapping-file format. PURE: no device is opened, no thread,
// no clock (timestamps come in with each report).
//
// THE EVENT. HidEvent below is a BY-VALUE COPY of the knob lane's tiny event struct (branch feat/knob-input, src/input/input_event.hpp:
// device 32-bit id, control 16-bit id, kind {Relative=0, Absolute=1, ButtonDown=2, ButtonUp=3, Disconnected=4}, raw int32, value float, timeUs int64).
// That header is not in this lane's base and is not included. THE JOIN IS A HAND-OFF: when both branches meet, the event type is replaced by the
// knob lane's (identical layout) and the mapper's ActionOut is fed into the knob lane's mapping/consumer. Control ids here are assigned per
// descriptor in order of first appearance, starting at kFirstControl (0x20); they are NOT the knob lane's 0x10-based ids.
// deviceIdFor() copies the knob lane's stated rule: FNV-1a 32 over the stable text ("hid:<vendor>:<product>").
//
// ACTION NAMES: the same twelve the knob lane's mapping uses (tune, tune.fine, tune.shuttle, volume, squelch, zoom, step.up, step.down, mode.next,
// mode.prev, radio.select, command), copied by value. A name, or a params.name, that begins "transmit" or "ptt" (any case) is REFUSED at load time:
// a button box must never key a transmitter, and the mapper has no action that does.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdint>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "hid_descriptor.hpp"

namespace cascade::input_hid {

enum class EventKind : std::uint8_t { Relative = 0, Absolute = 1, ButtonDown = 2, ButtonUp = 3, Disconnected = 4 };

struct HidEvent {
    std::uint32_t device = 0;
    std::uint16_t control = 0;
    EventKind kind = EventKind::Relative;
    std::int32_t raw = 0;      // Relative: signed detents (never 0); Absolute: the device's integer
    float value = 0.0f;        // Absolute: 0..1; ButtonDown: 1
    std::int64_t timeUs = 0;
};

inline constexpr std::uint16_t kFirstControl = 0x20;
inline constexpr std::size_t kMaxControls = 512;

inline std::uint32_t deviceIdFor(const std::string& stableText) {
    std::uint32_t h = 2166136261u;
    for (unsigned char c : stableText) { h ^= c; h *= 16777619u; }
    return h;
}

enum class ControlKind : std::uint8_t { Button, Axis, Relative };

struct ControlInfo {
    std::uint16_t control = 0;
    std::uint32_t usage = 0;           // page << 16 | id
    ControlKind kind = ControlKind::Button;
    std::int32_t logMin = 0, logMax = 1;
};

enum class ParseResult : std::uint8_t { Ok = 0, BadReport = 1 };

class HidDevice {
public:
    HidDevice(const std::string& stableId, const Descriptor& d) : desc_(d), id_(deviceIdFor(stableId)) {
        if (!desc_.ok) { return; }
        for (const Field& f : desc_.fields) {
            if (f.type != ReportType::Input || f.isConstant()) { continue; }
            if (f.isVariable()) {
                for (std::uint32_t e = 0; e < f.count && e < f.usages.size(); ++e) { addControl(f.usages[e], classify(f), f); }
            } else if (f.hasUsageRange) {
                const std::uint64_t n = static_cast<std::uint64_t>(f.usageMax) - f.usageMin + 1;
                for (std::uint64_t k = 0; k < n && k < 256 && f.usageMin + k <= f.usageMax; ++k) { addControl(static_cast<std::uint32_t>(f.usageMin + k), ControlKind::Button, f); }
            } else {
                for (std::uint32_t u : f.usages) { addControl(u, ControlKind::Button, f); }
            }
        }
    }

    bool valid() const { return desc_.ok; }
    std::uint32_t deviceId() const { return id_; }
    const std::vector<ControlInfo>& controls() const { return controls_; }
    // 0 when this device has no control with that usage (page << 16 | id)
    std::uint16_t controlForUsage(std::uint32_t usage) const {
        auto it = byUsage_.find(usage);
        return it == byUsage_.end() ? 0 : controls_[it->second].control;
    }
    void reset() { last_.clear(); arrayDown_.clear(); }

    // One Input report as read from the device. BadReport (and no event, state untouched) when it does not fit the descriptor.
    ParseResult parseReport(const std::uint8_t* data, std::size_t len, std::int64_t timeUs, std::vector<HidEvent>& out) {
        out.clear();
        std::vector<FieldValue> vals;
        if (!desc_.ok || !decodeInputReport(desc_, data, len, vals)) { return ParseResult::BadReport; }
        std::map<std::size_t, std::set<std::uint32_t>> arrays;   // field -> usages closed in this report
        std::set<std::size_t> arrayFields;
        for (const FieldValue& v : vals) {
            const Field& f = desc_.fields[v.field];
            if (f.isArray()) {
                arrayFields.insert(v.field);
                if (v.usage != 0) { arrays[v.field].insert(v.usage); }
                continue;
            }
            if (v.usage == 0) { continue; }
            const std::uint16_t c = controlForUsage(v.usage);
            if (c == 0) { continue; }
            const ControlKind k = classify(f);
            const std::uint64_t key = (static_cast<std::uint64_t>(v.field) << 32) | v.element;
            const auto prev = last_.find(key);
            const bool known = prev != last_.end();
            if (k == ControlKind::Relative) {
                if (v.value != 0) { out.push_back(mk(c, EventKind::Relative, v.value, 0.0f, timeUs)); }
                continue;
            }
            if (known && prev->second == v.value) { continue; }
            const std::int32_t before = known ? prev->second : 0;
            last_[key] = v.value;
            if (k == ControlKind::Button) {
                const bool wasDown = before != 0, isDown = v.value != 0;
                if (isDown && !wasDown) { out.push_back(mk(c, EventKind::ButtonDown, 1, 1.0f, timeUs)); }
                else if (!isDown && wasDown) { out.push_back(mk(c, EventKind::ButtonUp, 0, 0.0f, timeUs)); }
            } else {
                const double span = static_cast<double>(f.logMax) - static_cast<double>(f.logMin);
                double n = span > 0 ? (static_cast<double>(v.value) - f.logMin) / span : 0.0;
                n = n < 0 ? 0 : (n > 1 ? 1 : n);
                out.push_back(mk(c, EventKind::Absolute, v.value, static_cast<float>(n), timeUs));
            }
        }
        for (std::size_t fi : arrayFields) {
            const std::set<std::uint32_t>& now = arrays[fi];
            std::set<std::uint32_t>& was = arrayDown_[fi];
            for (std::uint32_t u : now) {
                if (was.count(u) == 0) { const std::uint16_t c = controlForUsage(u); if (c != 0) { out.push_back(mk(c, EventKind::ButtonDown, 1, 1.0f, timeUs)); } }
            }
            for (std::uint32_t u : was) {
                if (now.count(u) == 0) { const std::uint16_t c = controlForUsage(u); if (c != 0) { out.push_back(mk(c, EventKind::ButtonUp, 0, 0.0f, timeUs)); } }
            }
            was = now;
        }
        return ParseResult::Ok;
    }

private:
    static ControlKind classify(const Field& f) {
        if (f.isRelative()) { return ControlKind::Relative; }
        if (f.size == 1 || (f.logMin == 0 && f.logMax == 1)) { return ControlKind::Button; }
        if ((f.usagePage & 0xFFFFu) == 0x09) { return ControlKind::Button; }   // the Button usage page
        return ControlKind::Axis;
    }
    void addControl(std::uint32_t usage, ControlKind k, const Field& f) {
        if (usage == 0 || (usage & 0xFFFFu) == 0 || byUsage_.count(usage) != 0 || controls_.size() >= kMaxControls) { return; }
        ControlInfo c;
        c.control = static_cast<std::uint16_t>(kFirstControl + controls_.size());
        c.usage = usage;
        c.kind = k;
        c.logMin = f.logMin;
        c.logMax = f.logMax;
        byUsage_[usage] = controls_.size();
        controls_.push_back(c);
    }
    HidEvent mk(std::uint16_t c, EventKind k, std::int32_t raw, float value, std::int64_t t) const {
        HidEvent e;
        e.device = id_;
        e.control = c;
        e.kind = k;
        e.raw = raw;
        e.value = value;
        e.timeUs = t;
        return e;
    }

    Descriptor desc_;
    std::uint32_t id_ = 0;
    std::vector<ControlInfo> controls_;
    std::map<std::uint32_t, std::size_t> byUsage_;
    std::map<std::uint64_t, std::int32_t> last_;
    std::map<std::size_t, std::set<std::uint32_t>> arrayDown_;
};

// ---- the mapping file --------------------------------------------------------------------------------------------------------------------

inline const char* const kActionNames[] = {"tune", "tune.fine", "tune.shuttle", "volume", "squelch", "zoom", "step.up", "step.down", "mode.next", "mode.prev", "radio.select", "command"};

inline bool isKnownAction(const std::string& n) {
    for (const char* a : kActionNames) { if (n == a) { return true; } }
    return false;
}
inline bool isForbiddenName(const std::string& n) {
    std::string l;
    for (unsigned char c : n) { l += static_cast<char>(std::tolower(c)); }
    return l.rfind("transmit", 0) == 0 || l.rfind("ptt", 0) == 0;
}

struct Binding {
    std::uint16_t usagePage = 0;
    std::uint16_t usage = 0;
    std::string action;
    bool invert = false;
    nlohmann::json params = nlohmann::json::object();
};

struct MappingFile {
    int version = 1;
    std::vector<Binding> bindings;
};

struct MapLoadReport {
    int bindingsRead = 0;
    int skippedMalformed = 0;   // not an object, or page / usage / action missing or of the wrong type or out of range
    int refusedForbidden = 0;   // a transmit / ptt action or params.name
    int unknownActions = 0;
    int duplicates = 0;         // a second binding of the same usage: the first one wins
    bool unparseable = false;
    bool tooBig = false;
    bool wrongVersion = false;
};

inline constexpr std::size_t kMaxMappingBytes = 64 * 1024;
inline constexpr std::size_t kMaxBindings = 256;

// Never throws. Bindings that cannot be used are counted and left out; the rest are kept.
inline bool loadMapping(const std::string& text, MappingFile& out, MapLoadReport& rep) {
    out = MappingFile();
    rep = MapLoadReport();
    if (text.size() > kMaxMappingBytes) { rep.tooBig = true; return false; }
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { rep.unparseable = true; return false; }
    if (const auto v = j.find("version"); v != j.end()) {
        if (!v->is_number_integer() || v->get<long long>() != 1) { rep.wrongVersion = true; return false; }
    }
    const auto bs = j.find("bindings");
    if (bs == j.end() || !bs->is_array()) { return true; }
    std::set<std::uint32_t> seen;
    for (const auto& b : *bs) {
        if (out.bindings.size() >= kMaxBindings) { break; }
        if (!b.is_object()) { ++rep.skippedMalformed; continue; }
        const auto pg = b.find("page"), us = b.find("usage"), ac = b.find("action");
        if (pg == b.end() || us == b.end() || ac == b.end() || !pg->is_number_integer() || !us->is_number_integer() || !ac->is_string()) { ++rep.skippedMalformed; continue; }
        const long long page = pg->get<long long>(), usage = us->get<long long>();
        if (page < 1 || page > 0xFFFF || usage < 1 || usage > 0xFFFF) { ++rep.skippedMalformed; continue; }
        Binding bd;
        bd.usagePage = static_cast<std::uint16_t>(page);
        bd.usage = static_cast<std::uint16_t>(usage);
        bd.action = ac->get<std::string>();
        if (const auto iv = b.find("invert"); iv != b.end() && iv->is_boolean()) { bd.invert = iv->get<bool>(); }
        if (const auto pr = b.find("params"); pr != b.end() && pr->is_object()) { bd.params = *pr; }
        if (isForbiddenName(bd.action) || (bd.params.contains("name") && bd.params["name"].is_string() && isForbiddenName(bd.params["name"].get<std::string>()))) { ++rep.refusedForbidden; continue; }
        if (!isKnownAction(bd.action)) { ++rep.unknownActions; continue; }
        const std::uint32_t key = (static_cast<std::uint32_t>(bd.usagePage) << 16) | bd.usage;
        if (!seen.insert(key).second) { ++rep.duplicates; continue; }
        out.bindings.push_back(std::move(bd));
        ++rep.bindingsRead;
    }
    return true;
}

inline std::string saveMapping(const MappingFile& m) {
    nlohmann::json j;
    j["version"] = 1;
    j["bindings"] = nlohmann::json::array();
    for (const Binding& b : m.bindings) {
        nlohmann::json o;
        o["page"] = b.usagePage;
        o["usage"] = b.usage;
        o["action"] = b.action;
        if (b.invert) { o["invert"] = true; }
        if (!b.params.empty()) { o["params"] = b.params; }
        j["bindings"].push_back(o);
    }
    return j.dump(2, 0x20, false, nlohmann::json::error_handler_t::replace);
}

struct ActionOut {
    std::string action;
    int detents = 0;           // relative controls: signed (invert applied)
    float value = 0.0f;        // absolute controls: 0..1 (invert applied)
    bool isButton = false;
    bool pressed = false;
    bool isAbsolute = false;
    nlohmann::json params = nlohmann::json::object();
};

class HidMapper {
public:
    HidMapper(const HidDevice& dev, const MappingFile& file) {
        for (const Binding& b : file.bindings) {
            const std::uint16_t c = dev.controlForUsage((static_cast<std::uint32_t>(b.usagePage) << 16) | b.usage);
            if (c != 0) { byControl_[c] = b; }
            else { ++unbound_; }
        }
    }
    std::size_t boundCount() const { return byControl_.size(); }
    int bindingsWithoutControl() const { return unbound_; }

    // Zero or one ActionOut per event; nothing for a control with no binding and nothing at all for a Disconnected event.
    bool translate(const HidEvent& e, ActionOut& out) const {
        const auto it = byControl_.find(e.control);
        if (it == byControl_.end() || e.kind == EventKind::Disconnected) { return false; }
        const Binding& b = it->second;
        out = ActionOut();
        out.action = b.action;
        out.params = b.params;
        switch (e.kind) {
            case EventKind::Relative: out.detents = b.invert ? (e.raw == std::numeric_limits<std::int32_t>::min() ? std::numeric_limits<std::int32_t>::max() : -e.raw) : e.raw; break;
            case EventKind::Absolute: out.isAbsolute = true; out.value = b.invert ? 1.0f - e.value : e.value; break;
            case EventKind::ButtonDown: out.isButton = true; out.pressed = true; break;
            case EventKind::ButtonUp: out.isButton = true; out.pressed = false; break;
            default: return false;
        }
        return true;
    }

private:
    std::map<std::uint16_t, Binding> byControl_;
    int unbound_ = 0;
};

}  // namespace cascade::input_hid
