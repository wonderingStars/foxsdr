// Implementation of control-request parsing. See net/web_control.hpp for the
// four rules this file enforces.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/web_control.hpp"

#include <cmath>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace cascade::net {
namespace {

// Every key this version understands. Rule 1 rejects anything else, so this
// list is the API surface rather than a convenience.
const char* const kKnownKeys[] = {"running",     "centerHz",  "vfoOffsetHz",
                                  "mode",        "bandwidthHz", "squelchDb",
                                  "volume"};

bool isKnownKey(const std::string& key) {
    for (const char* k : kKnownKeys) {
        if (key == k) {
            return true;
        }
    }
    return false;
}

// Reads one numeric field, rejecting a wrong type, a non-finite value, and
// anything outside [lo, hi]. Written so NaN fails the range test rather than
// slipping through a `v < lo || v > hi` that NaN answers false to twice.
bool readNumber(const nlohmann::json& j, const char* key, double lo, double hi,
                std::optional<double>& dst, std::string& error) {
    const auto it = j.find(key);
    if (it == j.end()) {
        return true;  // absent means "leave this alone"
    }
    if (!it->is_number()) {
        error = std::string(key) + " must be a number";
        return false;
    }
    const double v = it->get<double>();
    if (!std::isfinite(v)) {
        error = std::string(key) + " must be a finite number";
        return false;
    }
    if (!(v >= lo && v <= hi)) {
        error = std::string(key) + " is outside the accepted range";
        return false;
    }
    dst = v;
    return true;
}

}  // namespace

bool parseControlRequest(const std::string& body, ControlRequest& out,
                         std::string& error) {
    out = ControlRequest{};
    error.clear();

    const nlohmann::json j = nlohmann::json::parse(body, nullptr,
                                                   /*allow_exceptions=*/false);
    if (j.is_discarded()) {
        error = "body is not valid JSON";
        return false;
    }
    if (!j.is_object()) {
        error = "body must be a JSON object";
        return false;
    }

    for (const auto& item : j.items()) {
        if (!isKnownKey(item.key())) {
            error = "unknown field \"" + item.key() + "\"";
            return false;
        }
    }

    ControlRequest req;

    if (const auto it = j.find("running"); it != j.end()) {
        if (!it->is_boolean()) {
            error = "running must be true or false";
            return false;
        }
        req.running = it->get<bool>();
    }

    if (const auto it = j.find("mode"); it != j.end()) {
        if (!it->is_string()) {
            error = "mode must be a string";
            return false;
        }
        cascade::dsp::DemodMode m{};
        if (!cascade::dsp::modeFromName(it->get<std::string>(), m)) {
            error = "unknown mode \"" + it->get<std::string>() + "\"";
            return false;
        }
        req.mode = m;
    }

    if (!readNumber(j, "centerHz", kMinCenterHz, kMaxCenterHz, req.centerHz, error) ||
        !readNumber(j, "vfoOffsetHz", -kMaxVfoOffsetHz, kMaxVfoOffsetHz,
                    req.vfoOffsetHz, error) ||
        !readNumber(j, "bandwidthHz", kMinBandwidthHz, kMaxBandwidthHz,
                    req.bandwidthHz, error) ||
        !readNumber(j, "squelchDb", kMinSquelchDb, kMaxSquelchDb, req.squelchDb,
                    error) ||
        !readNumber(j, "volume", 0.0, 1.0, req.volume, error)) {
        return false;
    }

    if (req.empty()) {
        error = "no changes requested";
        return false;
    }

    out = req;
    return true;
}

}  // namespace cascade::net
