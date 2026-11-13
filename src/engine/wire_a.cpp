// wire_a.cpp - WIRING LANE wire-a: the dispatcher, the persistence helpers and the facts (see wire_a.hpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/wire_a.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include "core/app_commands.hpp"
#include "core/file_read.hpp"
#include "core/freq_markers.hpp"
#include "core/receiver_tables.hpp"
#include "engine/engine.hpp"
#include "source/iq_file_source.hpp"

namespace cascade::engine {

WireA& WireA::of(Engine& e) {
    if (!e.wireA_) { e.wireA_ = std::make_shared<WireA>(e); }
    return *static_cast<WireA*>(e.wireA_.get());
}

WireA::Result WireA::refused(Result r, std::int32_t status, const std::string& why) {
    r.status = status;
    r.flags |= FOXAPI_RESULT_REFUSED;
    std::snprintf(r.message, sizeof(r.message), "%s", why.c_str());
    return r;
}

FoxCommandResult WireA::apply(const FoxCommand& c, const std::string& text, FoxCommandResult res) {
    (void)text;
    switch (c.op) {
        case FOXAPP_OP_HISTORY_STEP:
            return historyStep(c, res);
        case FOXAPP_OP_CHANNEL:
            return channelOp(c, text, res);
        case FOXAPP_OP_KEY_ACTION:
            return keyActionOp(c, text, res);
        case FOXAPP_OP_SET_RECORD_DAILY:
            return dailyOp(c, text, res);
        case FOXAPP_OP_RECORD_LOCK:
            return lockOp(c, text, res);
        case FOXAPP_OP_RECORD_CONTAINER:
            return containerOp(c, res);
        case FOXAPP_OP_RECORD_IQ_CONTAINER:
            return iqContainerOp(c, res);
        case FOXAPP_OP_PATCH_UNDO:
        case FOXAPP_OP_PATCH_REDO:
        case FOXAPP_OP_PATCH_SELECT:
        case FOXAPP_OP_PATCH_COPY:
        case FOXAPP_OP_PATCH_PASTE:
        case FOXAPP_OP_PATCH_BYPASS:
            return patchEditOp(c, text, res);
        case FOXAPP_OP_BAND_MEMORY:
        case FOXAPP_OP_BAND_MEMORY_NOTE:
            return bandOp(c, text, res);
        case FOXAPP_OP_RECORDINGS_SCAN:
        case FOXAPP_OP_RECORDINGS_LIST:
        case FOXAPP_OP_RECORDINGS_OPEN:
            return recordingsOp(c, text, res);
        case FOXAPP_OP_OVERLAY_LOAD:
        case FOXAPP_OP_OVERLAY_LOOKUP:
        case FOXAPP_OP_GRADIENT_SET:
            return displayOp(c, text, res);
        case FOXAPP_OP_DISPLAY_AVERAGE:
        case FOXAPP_OP_WF_HISTORY:
        case FOXAPP_OP_FFT_SCAN:
            return framesOp(c, text, res);
        case FOXAPP_OP_FILE_TRANSPORT:
            return transportOp(c, res);
        case FOXAPP_OP_RIT:
            return ritOp(c, res);
        case FOXAPP_OP_CW_SURROUND:
            return surroundOp(c, text, res);
        case FOXAPP_OP_TRACE_MODE:
            return traceModeOp(text, res);
        case FOXAPP_OP_MARKER_EXPORT:
        case FOXAPP_OP_MARKER_IMPORT:
            return markerIoOp(c, text, res);
        case FOXAPP_OP_BOOKMARK_NOTE:
            return bookmarkNoteOp(c, text, res);
        case FOXAPP_OP_BOOKMARK_FIND:
            return bookmarkFindOp(c, text, res);
        case FOXAPP_OP_BOOKMARK_FIELDS:
            return bookmarkFieldsOp(c, text, res);
        case FOXAPP_OP_FAVOURITE_SAVE:
        case FOXAPP_OP_FAVOURITE_RECALL:
        case FOXAPP_OP_FAVOURITE_EDIT:
            return favouriteOp(c, text, res);
        case FOXAPP_OP_PACK_EXPORT:
        case FOXAPP_OP_PACK_IMPORT:
            return packOp(c, text, res);
        case FOXAPP_OP_JOB_CANCEL:
        case FOXAPP_OP_EXPORT_BOX:
            return jobOp(c, text, res);
        case FOXAPP_OP_PROJECT:
        case FOXAPP_OP_ATTACH:
        case FOXAPP_OP_SIGMF_ANNOTATE:
            return recordsOp(c, text, res);
        case FOXAPP_OP_SCAN_PLAN:
        case FOXAPP_OP_SCAN_BACK:
        case FOXAPP_OP_SCAN_SAVE_HIT:
        case FOXAPP_OP_SCAN_TRIGGER:
        case FOXAPP_OP_SCANNER_EXTRAS:
            return scanOp(c, text, res);
        default:
            return refused(res, FOXAPI_UNSUPPORTED, "this operation is not in this build");
    }
}

bool WireA::drain(std::chrono::milliseconds bound) {
    if (scanDirty_) {
        scanDirty_ = false;
        scanSave();
    }
    if (bandDirty_) { bandDirty_ = false; bandSave(); }
    notesDueS_ = 0.0;   // anything dirty goes out now
    favouritesDueS_ = 0.0;
    bookmarksPass();
    const auto end = std::chrono::steady_clock::now() + bound;
    bool ok = true;
    for (auto& kv : savers_) {
        const auto now = std::chrono::steady_clock::now();
        const auto left = now < end ? std::chrono::duration_cast<std::chrono::milliseconds>(end - now) : std::chrono::milliseconds(0);
        if (!kv.second->finishOrAbandon(left)) { ok = false; }
    }
    return ok;
}

void WireA::pass() {
    bandPass();
    recordingsPass();
    containerPass();
    iqContainerPass();
    patchHistoryPass();
    framesLoadOnce();
    gradientLoadOnce();
    for (std::size_t slot = 0; slot < 8; ++slot) {
        if (e_.receiverLive(static_cast<std::uint32_t>(slot) + 1u)) { lockSync(slot); }
    }
    dailyLoadOnce();
    lockLoadOnce();
    jobPass();
    scanPass();
    bookmarksPass();
    historyPass();
    ritPass();
    drainSaves();
}

void WireA::facts(const std::function<void(const std::string&, const std::string&)>& fact) const {
    historyFacts(fact);
    channelFacts(fact);
    tunedFacts(fact);
    filesFacts(fact);
    bookmarkFacts(fact);
    scanFacts(fact);
    jobFacts(fact);
    recordingFacts(fact);
    recordingsFacts(fact);
    bandFacts(fact);
    displayFacts(fact);
    framesFacts(fact);
    transportFacts(fact);
    ritFacts(fact);
    surroundFacts(fact);
    patchEditFacts(fact);
    fact("record.iqcontainer", iqContainer_ == 2 ? "sigmf" : (iqContainer_ == 1 ? "rf64" : "wav"));
}

void WireA::tunedFacts(const std::function<void(const std::string&, const std::string&)>& fact) const {
    for (std::size_t slot = 0; slot < history_.size(); ++slot) {
        if (!e_.receiverLive(static_cast<std::uint32_t>(slot) + 1u)) { continue; }
        // fact.iqfile.<n>.centreFromHeader: "1" when the radio plays an I/Q file whose own header said where it was recorded, "0" for any other source
        fact("iqfile." + std::to_string(slot + 1) + ".centreFromHeader", fileHeaderCentre_[slot] && e_.receivers_[slot]->sourceKind_ == "file" ? "1" : "0");
    }
}

// ---- shared helpers -------------------------------------------------------------------------------------------------------------------------

WireA::Result WireA::subCommand(std::uint32_t op, double n0, long long i0, long long i1, double n1) {
    FoxCommand c{};
    c.structSize = sizeof(c);
    c.op = op;
    c.num[0] = n0;
    c.num[1] = n1;
    c.ival[0] = i0;
    c.ival[1] = i1;
    return e_.applyCommand(c, std::string());
}

void WireA::tuneTo(double hz, const std::string& mode, double bandwidthHz) {
    for (int m = 0; m < 8; ++m) {
        if (mode == cascade::gui::kModeNames[m]) {
            if (m != e_.rx().modeIndex_) { e_.setModeIndex(m); }
            break;
        }
    }
    e_.tuneAbsoluteHz(hz);
    if (bandwidthHz > 0.0) {
        // The same clamp as SET_BANDWIDTH: no narrower than the engine's floor, no wider than the channel can carry.
        const double hi = cascade::gui::kVfoBwMaxChanFrac * e_.rx().pipeline_.channelRateHz();
        e_.rx().vfoBandwidthHz_ = std::max(cascade::gui::kVfoBwMinHz, std::min(bandwidthHz, hi));
        e_.applyChannelBandwidth();
        e_.rx().bandwidthIndex_ = cascade::gui::bandwidthStepIndex(e_.rx().vfoBandwidthHz_);
    }
}

bool WireA::fullPath(const std::string& p, std::string& why) {
    if (p.empty() || p.size() > 1024) { why = "give the full path of a file"; return false; }
    for (const unsigned char ch : p) {
        if (ch < 0x20) { why = "the path has a control character"; return false; }
    }
    const std::filesystem::path fp(p);
    if (!fp.is_absolute() || !fp.has_filename()) { why = "give the full path of a file"; return false; }
    return true;
}

bool WireA::readUserFile(const std::string& path, std::size_t maxBytes, std::string& text, std::string& why) {
    std::error_code ec;
    const std::filesystem::path fp(path);
    if (!std::filesystem::is_regular_file(fp, ec)) { why = "that is not a file that can be read"; return false; }
    const std::uintmax_t size = std::filesystem::file_size(fp, ec);
    if (ec) { why = "the file could not be read"; return false; }
    if (size > maxBytes) { why = "the file is too big"; return false; }
    if (cascade::core::readTextFile(fp, text) != cascade::core::ReadResult::Ok) { why = "the file could not be read"; return false; }
    return true;
}

bool WireA::writeNewFile(const std::string& path, const std::string& text, std::string& why) {
    std::error_code ec;
    if (std::filesystem::exists(std::filesystem::path(path), ec)) { why = "a file with that name exists already"; return false; }
    std::string err;
    if (!cascade::core::FreqMarkers::writeFile(path, text, err)) { why = err.empty() ? "the file could not be written" : err; return false; }
    return true;
}

// ---- persistence ----------------------------------------------------------------------------------------------------------------------------

std::string WireA::dir() const {
    if (e_.markerPath_.empty()) { return std::string(); }   // a hermetic engine: never touch the disk
    return std::filesystem::path(e_.markerPath_).parent_path().string();
}

void WireA::saveFile(const std::string& name, const std::string& text) {
    const std::string d = dir();
    if (d.empty()) { return; }
    auto& s = savers_[name];
    if (!s) { s = std::make_unique<cascade::gui::BackgroundSaver>("wire-a", &cascade::core::FreqMarkers::writeFile); }
    s->request((std::filesystem::path(d) / name).string(), text);
}

std::string WireA::readFile(const std::string& name) const {
    const std::string d = dir();
    if (d.empty()) { return std::string(); }
    std::string text;
    if (cascade::core::readTextFile(std::filesystem::path(d) / name, text) != cascade::core::ReadResult::Ok) { return std::string(); }
    return text;
}

void WireA::drainSaves() {
    bool ok = true;
    std::string err;
    for (auto& kv : savers_) { (void)kv.second->poll(ok, err); }
}

}  // namespace cascade::engine
