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
const char* const kKnownKeys[] = {
    "running",     "centerHz",    "vfoOffsetHz",     "mode",
    "bandwidthHz", "squelchDb",   "volume",          "dbMin",
    "dbMax",       "deemphasisIndex", "nrEnabled",   "nrStrength",
    "notchEnabled", "notchFreqHz", "notchQ",         "autoNotch",
    "stereoEnabled", "sourceKind",  "soapyArgs",     "antenna",
    "sampleRateHz", "gainName",    "gainDb",         "agc",
    "scanDevices"};

// Longest string a control field will accept. Device kwargs and antenna names
// are short by nature; a cap keeps a hostile client from making the
// application hold megabytes of nonsense while it is matched against a list.
constexpr std::size_t kMaxFieldChars = 256;

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

    // Booleans, all read the same strict way: JSON true/false only, since a
    // number or a "looks boolean" string is a client bug.
    struct BoolField {
        const char* key;
        std::optional<bool>* dst;
    };
    const BoolField boolFields[] = {
        {"running", &req.running},
        {"nrEnabled", &req.nrEnabled},
        {"notchEnabled", &req.notchEnabled},
        {"autoNotch", &req.autoNotch},
        {"stereoEnabled", &req.stereoEnabled},
        {"agc", &req.agc},
        {"scanDevices", &req.scanDevices},
    };
    for (const BoolField& f : boolFields) {
        const auto it = j.find(f.key);
        if (it == j.end()) {
            continue;
        }
        if (!it->is_boolean()) {
            error = std::string(f.key) + " must be true or false";
            return false;
        }
        *f.dst = it->get<bool>();
    }

    if (const auto it = j.find("deemphasisIndex"); it != j.end()) {
        // An index into a THREE-entry table; out of range would read past it.
        if (!it->is_number_integer()) {
            error = "deemphasisIndex must be an integer";
            return false;
        }
        const int v = it->get<int>();
        if (v < 0 || v > 2) {
            error = "deemphasisIndex must be 0 (50 us), 1 (75 us) or 2 (off)";
            return false;
        }
        req.deemphasisIndex = v;
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
        !readNumber(j, "volume", 0.0, 1.0, req.volume, error) ||
        !readNumber(j, "dbMin", kMinDisplayDb, kMaxDisplayDb, req.dbMin, error) ||
        !readNumber(j, "dbMax", kMinDisplayDb, kMaxDisplayDb, req.dbMax, error) ||
        !readNumber(j, "nrStrength", 0.0, 1.0, req.nrStrength, error) ||
        !readNumber(j, "notchFreqHz", kMinNotchHz, kMaxNotchHz, req.notchFreqHz, error) ||
        !readNumber(j, "notchQ", kMinNotchQ, kMaxNotchQ, req.notchQ, error)) {
        return false;
    }

    // A display range is only meaningful as a pair, and an inverted or
    // degenerate one divides by zero where dB is mapped to pixels. Checked
    // here when BOTH ends are supplied; when only one is, the application
    // applies the same minimum-span rule the desktop sliders use, against the
    // end it already holds.
    if (req.dbMin && req.dbMax && !(*req.dbMin < *req.dbMax - 10.0)) {
        error = "dbMin must be at least 10 dB below dbMax";
        return false;
    }

    // Bounded strings.
    struct StringField {
        const char* key;
        std::optional<std::string>* dst;
    };
    const StringField stringFields[] = {
        {"sourceKind", &req.sourceKind},
        {"soapyArgs", &req.soapyArgs},
        {"antenna", &req.antenna},
        {"gainName", &req.gainName},
    };
    for (const StringField& f : stringFields) {
        const auto it = j.find(f.key);
        if (it == j.end()) {
            continue;
        }
        if (!it->is_string()) {
            error = std::string(f.key) + " must be a string";
            return false;
        }
        const std::string v = it->get<std::string>();
        if (v.size() > kMaxFieldChars) {
            error = std::string(f.key) + " is too long";
            return false;
        }
        *f.dst = v;
    }

    if (req.sourceKind && *req.sourceKind != "siggen" && *req.sourceKind != "soapy") {
        // "file" is refused ON PURPOSE — see the note in web_control.hpp. A
        // browser naming a path on the host is a file-read primitive, not a
        // source selector.
        error = "sourceKind must be \"siggen\" or \"soapy\"";
        return false;
    }

    if (!readNumber(j, "sampleRateHz", 8000.0, 61.44e6, req.sampleRateHz, error) ||
        !readNumber(j, "gainDb", -30.0, 100.0, req.gainDb, error)) {
        return false;
    }

    // A gain is a NAME and a VALUE; either alone cannot be acted on, and
    // silently ignoring the half that arrived would be a control that
    // reported success and did nothing.
    if (req.gainName.has_value() != req.gainDb.has_value()) {
        error = "gainName and gainDb must be given together";
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
