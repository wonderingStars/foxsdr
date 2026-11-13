// wire_b_framelog.hpp - the frame log (dsp/sigmeas_framelog.hpp) as engine operations (wire-b).
//
// PURE like wire_b_sigmeas.hpp: the engine glue owns one FrameLog; this file turns it into JSON.
// (Engine merge 3: this file was wire_b_cursors.hpp. Its eight-cursor and mark functions were dropped on the orchestrator's decision D2, "no engine copies of the screen lane K traces";
// screen lane K draws its own eight-cursor tool in the host against dsp/sigmeas_cursors.hpp.)
//
// THE FRAME LOG keeps decoded frames IN MEMORY ONLY, with the CRC flag a decoder gave them, so that a frame that FAILED its check can be shown on request (a person is the judge of whether the
// bytes are worth anything). No plugin call fills it yet (that is an ABI request); frames never go to disk and never into an upload or a log; they are in a LOCAL fact only when asked for.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstdint>
#include <string>

#include "dsp/sigmeas_framelog.hpp"
#include "engine/wire_b_sigmeas.hpp"

namespace cascade::engine::wireb {

inline Json frameLogJson(const sm::FrameLog& log, bool includeBad) {
    static const char* hexd = "0123456789abcdef";
    Json frames = Json::array();
    for (const sm::FrameRecord& r : log.snapshot(includeBad)) {
        std::string hex;
        for (const std::uint8_t b : r.bytes) { hex.push_back(hexd[b >> 4]); hex.push_back(hexd[b & 15u]); }
        frames.push_back({{"seq", r.seq}, {"time_s", r.timeS}, {"freq_hz", r.freqHz}, {"length", r.bytes.size()}, {"hex", hex}, {"has_crc", r.hasCrc}, {"crc_ok", r.crcOk},
                          {"crc_received", r.crcReceived}, {"crc_computed", r.crcComputed}, {"confidence", detail::numberOrNull(r.confidence)}});
    }
    const sm::FrameLog::Counts c = log.counts();
    Json out = Json::object();
    out["keep_bad"] = log.keepBad(); out["include_bad"] = includeBad; out["good_total"] = c.goodTotal; out["bad_total"] = c.badTotal; out["held"] = c.held; out["held_bad"] = c.heldBad;
    out["frames"] = frames;
    return out;
}

}  // namespace cascade::engine::wireb
