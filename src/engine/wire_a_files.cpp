// wire_a_files.cpp - WIRING LANE wire-a, the files of the user's choosing: marker export / import (MARKER_EXPORT, MARKER_IMPORT; fact.markerio.*).
//
// MARKERS (core/marker_export.hpp, row 6): the engine's own marker list (freqMarkers_) written to a CSV or JSON file the user names, and read back strictly: a file with one bad row
// is refused whole and changes nothing. The engine's marker save follows the list's version, so an import reaches markers.json with no code of its own. A path is a full path typed or
// picked by the user: nothing is written over an existing file, nothing larger than 1 MiB is read, and the facts carry words and counts, never a path.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/app_commands.hpp"
#include "core/marker_export.hpp"
#include "engine/engine.hpp"
#include "engine/wire_a.hpp"

namespace cascade::engine {

namespace {
constexpr std::size_t kMaxImportBytes = 1u << 20;

bool hasJsonExtension(const std::string& path) {
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) { return false; }
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == "json";
}
}  // namespace

WireA::Result WireA::markerIoOp(const FoxCommand& c, const std::string& text, Result res) {
    namespace t = cascade::core::tuning;
    std::string why;
    if (!fullPath(text, why)) { return refused(res, FOXAPI_BAD_ARGUMENT, why); }
    if (c.op == FOXAPP_OP_MARKER_EXPORT) {
        if (c.ival[0] != 0 && c.ival[0] != 1) { return refused(res, FOXAPI_OUT_OF_RANGE, "0 is CSV and 1 is JSON"); }
        const std::vector<cascade::core::FreqMarker>& list = e_.freqMarkers_.list();
        if (list.empty()) { return refused(res, FOXAPI_NO_CHANGE, "there are no markers to export"); }
        const std::string body = c.ival[0] == 0 ? t::markersToCsv(list) : t::markersToJson(list).dump(2) + "\n";
        if (!writeNewFile(text, body, why)) {
            markerIoError_ = why;
            bump();
            return refused(res, FOXAPI_FAILED, why);
        }
        markerIoError_.clear();
        markerIoLast_ = "exported " + std::to_string(list.size());
        res.applied[0] = static_cast<double>(list.size());
        bump();
        return res;
    }
    // MARKER_IMPORT
    if (!(c.num[0] >= 0.0) || c.num[0] > 1.0e6) { return refused(res, FOXAPI_OUT_OF_RANGE, "the merge reach is 0 to 1000000 Hz"); }
    std::string body;
    if (!readUserFile(text, kMaxImportBytes, body, why)) {
        markerIoError_ = why;
        bump();
        return refused(res, FOXAPI_FAILED, why);
    }
    std::vector<cascade::core::FreqMarker> in;
    std::string err;
    bool parsed = false;
    if (hasJsonExtension(text)) {
        const nlohmann::json j = nlohmann::json::parse(body, nullptr, false, false);
        parsed = !j.is_discarded() && t::parseMarkersJson(j, in, err);
        if (j.is_discarded()) { err = "the file is not JSON"; }
    } else {
        parsed = t::parseMarkersCsv(body, in, err);
    }
    if (!parsed) {
        markerIoError_ = err;
        bump();
        return refused(res, FOXAPI_BAD_ARGUMENT, "the file was refused: " + err);
    }
    const t::MarkerImportResult r = t::importInto(e_.freqMarkers_, in, c.num[0]);
    markerIoError_.clear();
    markerIoLast_ = "added " + std::to_string(r.added) + ", merged " + std::to_string(r.merged);
    res.applied[0] = r.added;
    res.applied[1] = r.merged;
    bump();
    if (r.listFull) { return refused(res, FOXAPI_LIMIT, "the marker list is full; what fitted was kept"); }
    return res;
}

void WireA::filesFacts(const std::function<void(const std::string&, const std::string&)>& fact) const {
    fact("markerio.last", markerIoLast_);
    fact("markerio.error", markerIoError_);
}

}  // namespace cascade::engine
