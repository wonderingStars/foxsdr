// engine_recname.cpp - RECORDING FILE NAMES FROM A TEMPLATE in the engine (engine wiring W2; docs/api-0.3-additions.md 9.36; the model is core/rec_naming.hpp).
//
// One template for every take, kept in "record-name.txt" in the per-user folder (beside the configuration and macros.json) and set by FOXAPP_OP_SET_RECORD_NAME:
//   text ""            the template is cleared: takes are named as they always were (<prefix>_<YYYYMMDD>_<HHMMSS>_<rate>Hz.wav, radioN_iq / radioN_audio for several radios)
//   text "{freq_mhz}/{mode}_{datetime}"   a template (core/rec_naming.hpp: {date} {time} {datetime} {year} {month} {day} {hour} {min} {sec} {ms} {tz} {freq} {freq_khz}
//                      {freq_mhz} {mode} {radio} {bookmark} {counter} {counter:N}; '/' separates up to three folders below the recordings folder; the extension is added)
// A template is validated when it is SET, by expanding it for a sample take: a refusal (a forbidden character, a path escape, an unknown token...) is BAD_ARGUMENT with the
// model's own words, and the template in force does not change. At a take's start the template is expanded for that take (the moment, the tuned frequency, the mode, the
// radio's label) and the take goes to <recordings folder>/<expansion>.wav; the name is created exclusively on the disk worker as every take's is, so a name that exists is
// never opened over (it becomes name-2.wav). If an expansion cannot be made (a path too long) the take falls back to the default name and says so in the log.
// The time tokens read the LOCAL clock. {bookmark} is not resolved here (a take does not know which bookmark it is on): it reads "unknown".
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "core/app_commands.hpp"
#include "core/config.hpp"
#include "core/diag_log.hpp"
#include "core/file_read.hpp"
#include "core/rec_naming.hpp"
#include "core/receiver_tables.hpp"
#include "core/recorder.hpp"
#include "core/status_clock.hpp"
#include "engine/engine.hpp"

namespace cascade::engine {

namespace rn = cascade::core::recname;

std::string Engine::recordNameFilePath() const {
    if (!recordNamePathForTest_.empty()) { return recordNamePathForTest_; }
    return (std::filesystem::path(cascade::core::ConfigStore::defaultPath()).parent_path() / "record-name.txt").string();
}

void Engine::loadRecordName() {
    recordNameLoaded_ = true;
    recordNameTemplate_.clear();
    std::string text;
    if (cascade::core::readTextFile(std::filesystem::path(recordNameFilePath()), text) != cascade::core::ReadResult::Ok) { return; }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) { text.pop_back(); }
    // A file that holds a template this build refuses (edited by hand, or from a newer build) is ignored, said once, and takes keep their default names.
    rn::Fields sample;
    sample.unixMs = 1'790'000'000'000LL;
    sample.frequencyHz = 100.0e6;
    sample.mode = "WFM";
    sample.radio = "radio";
    sample.bookmark = "bookmark";
    const rn::Expanded e = rn::expand(text, sample, false);
    if (text.empty()) { return; }
    if (!e.ok) {
        cascade::core::diagWarnf("record: the file-name template in record-name.txt was refused (%s); takes keep their default names", e.error.c_str());
        return;
    }
    recordNameTemplate_ = text;
    ++recordNameVersion_;   // (the fact block's fingerprint: a template read from the file is news to a reader)
}

std::int32_t Engine::applyRecordNameOp(const FoxCommand& c, const std::string& text, std::string& why) {
    if (!recordNameLoaded_) { loadRecordName(); }
    const std::string tmpl = text.empty() ? std::string(c.text) : text;
    if (!tmpl.empty()) {
        rn::Fields sample;
        sample.unixMs = 1'790'000'000'000LL;
        sample.frequencyHz = 100.0e6;
        sample.mode = "WFM";
        sample.radio = "radio";
        sample.bookmark = "bookmark";
        const rn::Expanded e = rn::expand(tmpl, sample, false);
        if (!e.ok) {
            why = e.error;
            return FOXAPI_BAD_ARGUMENT;
        }
    }
    if (tmpl == recordNameTemplate_) {
        why = "that template is in force already";
        return FOXAPI_NO_CHANGE;
    }
    // Written before it is taken into force: a template that cannot be kept is not in force (so what a restart brings back is what was reported).
    const std::filesystem::path path(recordNameFilePath());
    std::error_code ec;
    if (tmpl.empty()) {
        std::filesystem::remove(path, ec);
    } else {
        std::filesystem::create_directories(path.parent_path(), ec);
        const std::filesystem::path tmp = path.string() + ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            f << tmpl << "\n";
            if (!f) {
                why = "the template could not be saved";
                return FOXAPI_FAILED;
            }
        }
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            std::filesystem::remove(tmp, ec);
            why = "the template could not be saved";
            return FOXAPI_FAILED;
        }
    }
    recordNameTemplate_ = tmpl;
    ++recordNameVersion_;
    return FOXAPI_OK;
}

void Engine::applyRecordName(cascade::core::Recorder::OpenRequest& req) {
    if (!recordNameLoaded_) { loadRecordName(); }
    if (recordNameTemplate_.empty()) { return; }
    const std::int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    rn::Fields f;
    f.unixMs = nowMs;
    f.utcOffsetMinutes = cascade::core::statusclock::systemUtcOffsetMinutes(nowMs);
    f.frequencyHz = currentAbsoluteHz();
    f.mode = cascade::gui::kModeNames[rx().modeIndex_ >= 0 && rx().modeIndex_ < 8 ? rx().modeIndex_ : 0];
    f.radio = radioLabel();
    f.bookmark = "unknown";
    const rn::Expanded e = rn::expand(recordNameTemplate_, f, false);
    std::string err;
    const std::string full = e.ok ? rn::resolvePath(req.directory, e.relPath, "wav", &err) : std::string();
    if (full.empty()) {
        cascade::core::diagWarnf("record: the file-name template could not be used for this take (%s); the default name is used", e.ok ? err.c_str() : e.error.c_str());
        return;
    }
    req.path = full;
    req.directory = std::filesystem::path(full).parent_path().string();
}

}  // namespace cascade::engine
