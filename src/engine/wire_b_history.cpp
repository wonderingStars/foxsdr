// wire_b_history.cpp - the sweep history (wire_b_history.hpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/wire_b_history.hpp"

#include <filesystem>
#include <system_error>

#include "core/file_read.hpp"
#include "core/freq_markers.hpp"

namespace cascade::engine::wireb {

void SweepKeep::add(const cascade::core::SweepLine& line) {
    if (line.db.empty() || line.db.size() > cascade::core::kSweepMaxPointsPerLine) { return; }
    lines_.push_back(line);
    points_ += line.db.size();
    while (!lines_.empty() && (points_ > kSweepKeepPoints || lines_.size() > cascade::core::kHistoryMaxSweeps)) {
        points_ -= lines_.front().db.size();
        lines_.pop_front();
    }
}

void SweepKeep::clear() {
    lines_.clear();
    points_ = 0;
}

bool SweepKeep::replace(const cascade::core::SweepHistory& h, std::string& why) {
    std::size_t total = 0;
    for (const cascade::core::SweepLine& l : h.sweeps) { total += l.db.size(); }
    if (total > kSweepKeepPoints) { why = "the history has more points than the engine keeps (" + std::to_string(kSweepKeepPoints) + ")"; return false; }
    if (h.sweeps.size() > cascade::core::kHistoryMaxSweeps) { why = "the history has too many sweeps"; return false; }
    lines_.assign(h.sweeps.begin(), h.sweeps.end());
    points_ = total;
    return true;
}

cascade::core::SweepHistory SweepKeep::snapshot() const {
    cascade::core::SweepHistory h;
    h.sweeps.assign(lines_.begin(), lines_.end());
    return h;
}

bool historyFullPath(const std::string& path, std::string& why) {
    if (path.empty() || path.size() > 1024) { why = "give the full path of a file"; return false; }
    for (const unsigned char ch : path) {
        if (ch < 0x20) { why = "the path has a control character"; return false; }
    }
    const std::filesystem::path fp(path);
    if (!fp.is_absolute() || !fp.has_filename()) { why = "give the full path of a file"; return false; }
    return true;
}

bool historyReadFile(const std::string& path, std::size_t maxBytes, std::string& text, std::string& why) {
    std::error_code ec;
    const std::filesystem::path fp(path);
    if (!std::filesystem::is_regular_file(fp, ec)) { why = "that is not a file that can be read"; return false; }
    const std::uintmax_t size = std::filesystem::file_size(fp, ec);
    if (ec) { why = "the file could not be read"; return false; }
    if (size > maxBytes) { why = "the file is too big"; return false; }
    if (cascade::core::readTextFile(fp, text) != cascade::core::ReadResult::Ok) { why = "the file could not be read"; return false; }
    return true;
}

bool historyWriteNewFile(const std::string& path, const std::string& text, std::string& why) {
    std::error_code ec;
    if (std::filesystem::exists(std::filesystem::path(path), ec)) { why = "a file with that name exists already"; return false; }
    std::string err;
    if (!cascade::core::FreqMarkers::writeFile(path, text, err)) { why = err.empty() ? std::string("the file could not be written") : err; return false; }
    return true;
}

}  // namespace cascade::engine::wireb
