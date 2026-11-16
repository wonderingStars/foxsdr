// raw_file_id.hpp - the engine's source id for an I/Q file that has no header (screen lane S3, docs/multi-radio-records/screen-s3-recplay.md):
//     "file-raw:<format>,<rate Hz>,<centre Hz>|<path>"      format cu8, cs16 or cf32 (little-endian); the path is everything after the first '|'.
// A raw file carries nothing about itself, so the person states the format and the rate. Pure; no file is touched.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cmath>
#include <cstdio>
#include <string>

namespace cascade::core {

struct RawFileId {
    std::string format, path;
    double rateHz = 0.0, centreHz = 0.0;
    bool hasCentre = false;
};

// `args` is the id after "file-raw:". False for anything that is not exactly that shape, a rate outside 1 kS/s .. 1 GS/s or a centre outside 0 .. 10 THz.
inline bool parseRawFileId(const std::string& args, RawFileId& out) {
    const std::size_t bar = args.find('|');
    if (bar == std::string::npos || bar + 1 >= args.size()) { return false; }
    char fmt[16] = {};
    double rate = 0.0, centre = 0.0;
    const std::string head = args.substr(0, bar);
    if (std::sscanf(head.c_str(), "%15[a-z0-9],%lf,%lf", fmt, &rate, &centre) != 3) { return false; }
    const std::string f = fmt;
    if (f != "cu8" && f != "cs16" && f != "cf32") { return false; }
    if (!std::isfinite(rate) || !std::isfinite(centre) || !(rate >= 1000.0 && rate <= 1e9) || !(centre >= 0.0 && centre <= 1e13)) { return false; }
    out.format = f;
    out.rateHz = rate;
    out.centreHz = centre;
    out.hasCentre = centre > 0.0;
    out.path = args.substr(bar + 1);
    return true;
}

}  // namespace cascade::core
