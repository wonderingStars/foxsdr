// engine_recplay.cpp - SCREEN LANE S3 (docs/multi-radio-records/screen-s3-recplay.md): the engine's side of the recording and playback controls.
//
//   facts   record.schedule.ids (the ids of the timed recordings, so a screen can list them), radio.<n>.record.format ("iq=0;audio=0": the radio's sample formats for the next take) and
//           radio.<n>.file.header ("1" while the radio plays an I/Q file whose centre frequency came from the file's own header). Nothing here names a file or a folder.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <string>

#include "core/schedule.hpp"
#include "engine/engine.hpp"
#include "source/iq_file_source.hpp"

namespace cascade::engine {

void Engine::recplayFacts(const std::function<void(const std::string&, const std::string&)>& put) const {
    std::string ids;
    for (const cascade::core::sched::Entry& e : schedule_.entries()) {
        if (!ids.empty()) { ids += ','; }
        ids += std::to_string(e.id);
    }
    put("record.schedule.ids", ids);
}

void Engine::recplayRadioFacts(std::uint32_t n, const std::function<void(const std::string&, const std::string&)>& put) const {
    if (n < 1u || n > kMaxReceivers) { return; }
    const Receiver& r = *receivers_[n - 1u];
    const std::string pre = "radio." + std::to_string(n) + ".";
    put(pre + "record.format", "iq=" + std::to_string(r.recordIqFormat_) + ";audio=" + std::to_string(r.recordAudioFormat_));
    const auto* file = dynamic_cast<const cascade::source::IqFileSource*>(&const_cast<Receiver&>(r).pipeline_.rawSource());
    if (file != nullptr && file->totalFrames() > 0) { put(pre + "file.header", file->centerFromHeader() ? "1" : "0"); }
}

}  // namespace cascade::engine
