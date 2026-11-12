// wire_a_recordings.cpp - WIRING LANE wire-a, the recordings browser (RECORDINGS_SCAN, RECORDINGS_LIST, RECORDINGS_OPEN; fact.recordings.*). The ops are documented in core/wire_a_ops.hpp.
//
// THE MODEL (core/rec_browser.hpp, row 47) scans a folder on its own worker and publishes snapshots; this file starts it, polls it once a pass (a change of state moves the fact block once; the entries
// counted while it runs do not), and answers a list with sort and filter as JSON. A file is described by its NAME only: a path never reaches a fact, a message or a log. Opening a recording is the engine's
// existing I/Q file open (SELECT_SOURCE "file:"), which plays 16-bit integer and 32-bit float two-channel WAV: any other kind is refused with the reason (widening it needs the readers of
// source/iq_readers.hpp in IqFileSource: not done).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "core/app_commands.hpp"
#include "core/rec_browser.hpp"
#include "engine/engine.hpp"
#include "engine/wire_a.hpp"

namespace cascade::engine {

namespace rb = cascade::core::recbrowse;
using nlohmann::json;

struct WireA::Browser {
    rb::RecordingsBrowser model;
    rb::Snapshot last;
};

namespace {
const char* startWord(rb::StartSource s) {
    switch (s) {
        case rb::StartSource::SigmfDatetime: return "sigmf";
        case rb::StartSource::FileNameUtc: return "name (UTC)";
        case rb::StartSource::FileNameLocal: return "name (local)";
        case rb::StartSource::ModifiedMinusDuration: return "file time";
        case rb::StartSource::IfHeader: return "if header";
        default: return "";
    }
}
}  // namespace

void WireA::recordingsPass() {
    if (!browser_) { return; }
    const rb::Snapshot s = browser_->model.snapshot();
    browser_->last = s;
    recordingsCount_ = s.entries ? s.entries->size() : 0;
    recordingsProbed_ = s.probed;
    recordingsBroken_ = s.broken;
    recordingsTruncated_ = s.listTruncated;
    recordingsScanId_ = s.scanId;
    recordingsError_ = s.folderError.empty() ? std::string() : "the folder could not be opened";
    const std::string state = s.cancelled ? "cancelled" : (s.done ? "done" : "scanning");
    if (state != recordingsState_) {
        recordingsState_ = state;
        bump();   // once per change of state (not per entry)
    }
}

WireA::Result WireA::recordingsOp(const FoxCommand& c, const std::string& text, Result res) {
    if (c.op == FOXAPP_OP_RECORDINGS_SCAN) {
        const json j = text.empty() ? json::object() : json::parse(text, nullptr, false);
        if (j.is_discarded() || !j.is_object()) { return refused(res, FOXAPI_BAD_ARGUMENT, "the text is not a JSON object"); }
        std::string folder = j.contains("folder") && j["folder"].is_string() ? j["folder"].get<std::string>() : std::string();
        if (folder.empty()) { folder = e_.recordDir_; }
        std::string why;
        if (!fullPath(folder, why)) { return refused(res, FOXAPI_BAD_ARGUMENT, why); }
        rb::ScanOptions opt;
        if (j.contains("maxDepth")) {
            if (!j["maxDepth"].is_number_integer() || j["maxDepth"].get<long long>() < 0 || j["maxDepth"].get<long long>() > 6) { return refused(res, FOXAPI_BAD_ARGUMENT, "maxDepth is 0 to 6"); }
            opt.maxDepth = static_cast<int>(j["maxDepth"].get<long long>());
        }
        if (j.contains("maxEntries")) {
            if (!j["maxEntries"].is_number_integer() || j["maxEntries"].get<long long>() < 1 || j["maxEntries"].get<long long>() > 20000) { return refused(res, FOXAPI_BAD_ARGUMENT, "maxEntries is 1 to 20000"); }
            opt.maxEntries = static_cast<std::size_t>(j["maxEntries"].get<long long>());
        }
        if (!browser_) { browser_ = std::make_shared<Browser>(); }
        recordingsList_.clear();
        recordingsState_ = "scanning";
        browser_->model.start(folder, opt);
        bump();
        return res;
    }
    if (!browser_ || recordingsScanId_ == 0) { return refused(res, FOXAPI_NO_CHANGE, "no scan has been made"); }
    recordingsPass();   // the newest snapshot
    const std::shared_ptr<const std::vector<rb::Entry>> all = browser_->last.entries;
    if (c.op == FOXAPP_OP_RECORDINGS_OPEN) {
        if (static_cast<std::uint64_t>(c.ival[1]) != recordingsScanId_ || c.ival[0] < 0 || !all || static_cast<std::size_t>(c.ival[0]) >= all->size()) { return refused(res, FOXAPI_NOT_FOUND, "no such recording in that scan"); }
        const rb::Entry& e = (*all)[static_cast<std::size_t>(c.ival[0])];
        if (e.broken) { return refused(res, FOXAPI_UNSUPPORTED, "that file could not be read"); }
        const bool playable = e.kind == rb::Kind::Iq && e.container == "wav" && e.channels == 2 && ((e.format == "pcm16") || (e.format == "float32"));
        if (!playable) { return refused(res, FOXAPI_UNSUPPORTED, "only 16-bit and 32-bit float two-channel WAV I/Q files can be played"); }
        if (e_.rx().tuneLocked_) { return refused(res, FOXAPI_DENIED, "this radio's frequency is locked"); }
        e_.openIqFile(e.path);
        bump();
        return res;
    }
    // RECORDINGS_LIST
    const json j = text.empty() ? json::object() : json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { return refused(res, FOXAPI_BAD_ARGUMENT, "the text is not a JSON object"); }
    rb::Filter f;
    f.text = j.contains("text") && j["text"].is_string() ? j["text"].get<std::string>() : std::string();
    f.iq = j.value("iq", true);
    f.audio = j.value("audio", true);
    f.hideBroken = j.value("hideBroken", false);
    const std::string sort = j.value("sort", std::string("name"));
    static const char* const names[8] = {"name", "modified", "start", "duration", "size", "rate", "center", "kind"};
    int key = -1;
    for (int i = 0; i < 8; ++i) { if (sort == names[i]) { key = i; } }
    if (key < 0) { return refused(res, FOXAPI_BAD_ARGUMENT, "the sort is name, modified, start, duration, size, rate, center or kind"); }
    const long long offset = j.value("offset", 0LL), limit = j.value("limit", 50LL);
    if (offset < 0 || limit < 1 || limit > 200) { return refused(res, FOXAPI_BAD_ARGUMENT, "offset is 0 or more and limit is 1 to 200"); }
    const std::vector<std::size_t> idx = all ? rb::sortAndFilter(*all, f, static_cast<rb::SortKey>(key), j.value("ascending", true)) : std::vector<std::size_t>();
    json out = json::array();
    for (std::size_t n = static_cast<std::size_t>(offset); n < idx.size() && n < static_cast<std::size_t>(offset + limit); ++n) {
        const rb::Entry& e = (*all)[idx[n]];
        out.push_back({{"id", idx[n]}, {"scanId", recordingsScanId_}, {"name", e.fileName}, {"kind", e.kind == rb::Kind::Iq ? "iq" : "audio"}, {"kindGuessed", e.kindGuessed}, {"container", e.container},
                       {"format", e.format}, {"rateHz", e.sampleRateKnown ? e.sampleRateHz : 0.0}, {"centerHz", e.centerKnown ? e.centerHz : 0.0}, {"durationS", e.durationKnown ? e.durationS : 0.0},
                       {"startUnixMs", e.startUnixMs}, {"startSource", startWord(e.startSource)}, {"sizeBytes", e.sizeBytes}, {"channels", e.channels}, {"bits", e.bitsPerSample}, {"broken", e.broken},
                       {"error", e.error}, {"truncated", e.truncated}, {"ifRecording", e.ifRecording}, {"mode", e.mode}, {"bandwidthHz", e.bandwidthHz}});
    }
    recordingsList_ = out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    res.applied[0] = static_cast<double>(idx.size());
    bump();
    return res;
}

void WireA::recordingsFacts(const std::function<void(const std::string&, const std::string&)>& fact) const {
    fact("recordings.state", recordingsState_);
    fact("recordings.count", std::to_string(recordingsCount_));
    fact("recordings.probed", std::to_string(recordingsProbed_));
    fact("recordings.broken", std::to_string(recordingsBroken_));
    fact("recordings.truncated", recordingsTruncated_ ? "1" : "0");
    fact("recordings.folderError", recordingsError_);
    fact("recordings.scanId", std::to_string(recordingsScanId_));
    fact("recordings.list", recordingsList_);
}

}  // namespace cascade::engine
