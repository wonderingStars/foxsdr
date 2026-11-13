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
        const std::string body = c.ival[0] == 0 ? t::markersToCsv(list) : t::markersToJson(list).dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
        // THE FILE IS WRITTEN ON THE JOB WORKER (second pass: the pass does no disk work); the text is made here, from the list as it is now. The answer is OK for "started"; how it ended is fact.job.* and fact.markerio.*.
        const std::size_t count = list.size();
        const std::string path = text;
        Result started = startJob(res, "marker-export", [this, path, body, count](JobCtl&) {
            JobResult j;
            std::string err;
            j.ok = writeNewFile(path, body, err);
            j.message = j.ok ? "exported " + std::to_string(count) : err;
            j.onFinish = [this, ok = j.ok, msg = j.message]() {
                markerIoError_ = ok ? std::string() : msg;
                if (ok) { markerIoLast_ = msg; }
            };
            return j;
        });
        if (started.status == FOXAPI_OK) { started.applied[0] = static_cast<double>(count); }
        return started;
    }
    // MARKER_IMPORT
    if (!(c.num[0] >= 0.0) || c.num[0] > 1.0e6) { return refused(res, FOXAPI_OUT_OF_RANGE, "the merge reach is 0 to 1000000 Hz"); }
    // THE FILE IS READ ON THE JOB WORKER (second pass); what it held is parsed and merged on the control thread when the job is collected (onDone). The answer is OK for "started"; how it ended is fact.job.state / .message
    // ("done", or "failed" with the words: the file could not be read, was refused, the list is full) and fact.markerio.*.
    const std::string path = text;
    const double reach = c.num[0];
    return startJob(res, "marker-import", [this, path, reach](JobCtl&) {
        JobResult j;
        auto body = std::make_shared<std::string>();
        std::string err;
        j.ok = readUserFile(path, kMaxImportBytes, *body, err);
        j.message = j.ok ? std::string() : err;
        j.onFinish = [this, ok = j.ok, msg = j.message]() { if (!ok) { markerIoError_ = msg; } };
        j.onDone = [this, body, path, reach]() {
            namespace t = cascade::core::tuning;
            std::vector<cascade::core::FreqMarker> in;
            std::string why;
            bool parsed = false;
            if (hasJsonExtension(path)) {
                const nlohmann::json js = nlohmann::json::parse(*body, nullptr, false, false);
                parsed = !js.is_discarded() && t::parseMarkersJson(js, in, why);
                if (js.is_discarded()) { why = "the file is not JSON"; }
            } else {
                parsed = t::parseMarkersCsv(*body, in, why);
            }
            if (!parsed) {
                markerIoError_ = why;
                jobState_ = "failed";
                jobMessage_ = "the file was refused: " + why;
                return;
            }
            const t::MarkerImportResult r = t::importInto(e_.freqMarkers_, in, reach);
            markerIoError_.clear();
            markerIoLast_ = "added " + std::to_string(r.added) + ", merged " + std::to_string(r.merged);
            jobMessage_ = markerIoLast_;
            jobJson_ = nlohmann::json{{"added", r.added}, {"merged", r.merged}}.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
            if (r.listFull) { jobState_ = "failed"; jobMessage_ += "; the marker list is full, what fitted was kept"; }
        };
        return j;
    });
}

void WireA::filesFacts(const std::function<void(const std::string&, const std::string&)>& fact) const {
    fact("markerio.last", markerIoLast_);
    fact("markerio.error", markerIoError_);
}

}  // namespace cascade::engine
