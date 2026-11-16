// control_hub.cpp - see control_hub.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "net/control_hub.hpp"

#include <algorithm>
#include <cmath>

#include "core/demod_names.hpp"

namespace cascade::net {

std::string ControlHub::cleanText(const std::string& in, std::size_t maxBytes) {
    std::string out;
    std::size_t i = 0;
    while (i < in.size() && out.size() < maxBytes) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        const std::size_t len = c < 0x80 ? 1 : (c >= 0xC2 && c <= 0xDF) ? 2 : (c >= 0xE0 && c <= 0xEF) ? 3 : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
        bool ok = len != 0 && i + len <= in.size();
        for (std::size_t k = 1; ok && k < len; ++k) {
            if ((static_cast<unsigned char>(in[i + k]) & 0xC0) != 0x80) { ok = false; }
        }
        if (ok && len == 3) {
            const unsigned char c1 = static_cast<unsigned char>(in[i + 1]);
            if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 >= 0xA0)) { ok = false; }
        }
        if (ok && len == 4) {
            const unsigned char c1 = static_cast<unsigned char>(in[i + 1]);
            if ((c == 0xF0 && c1 < 0x90) || (c == 0xF4 && c1 >= 0x90)) { ok = false; }
        }
        if (ok && len == 1 && ((c < 0x20 && c != '\n' && c != '\t') || c == 0x7F)) { ok = false; }
        if (!ok) { out += '?'; ++i; continue; }
        if (out.size() + len > maxBytes) { break; }
        out.append(in, i, len);
        i += len;
    }
    return out;
}

std::string ControlHub::canonicalMode(const std::string& name) {
    for (int i = 1; i <= 8; ++i) {
        const std::string want = cascade::core::kDemodNames[i];
        if (name.size() != want.size()) { continue; }
        bool same = true;
        for (std::size_t k = 0; k < name.size(); ++k) {
            char a = name[k];
            if (a >= 'a' && a <= 'z') { a = static_cast<char>(a - 'a' + 'A'); }
            if (a != want[k]) { same = false; break; }
        }
        if (same) { return want; }
    }
    return std::string();
}

void ControlHub::publishStatus(const HubStatus& s) {
    std::lock_guard<std::mutex> lk(m_);
    status_ = s;
    status_.mode = cleanText(s.mode, 16);
    status_.source = cleanText(s.source, kHubNameBytes);
}

void ControlHub::publishBookmarks(std::vector<HubBookmark> b) {
    if (b.size() > kHubMaxBookmarks) { b.resize(kHubMaxBookmarks); }
    for (HubBookmark& x : b) {
        x.name = cleanText(x.name, kHubNameBytes);
        x.mode = cleanText(x.mode, 16);
        x.group = cleanText(x.group, kHubNameBytes);
    }
    std::lock_guard<std::mutex> lk(m_);
    bookmarks_ = std::move(b);
}

void ControlHub::addDecode(double unixSeconds, double freqHz, const std::string& decoder, const std::string& text) {
    HubDecode d;
    d.unixSeconds = unixSeconds;
    d.freqHz = freqHz;
    d.decoder = cleanText(decoder, 64);
    d.text = cleanText(text, kHubTextBytes);
    std::lock_guard<std::mutex> lk(m_);
    d.seq = nextSeq_++;
    decodes_.push_back(std::move(d));
    while (decodes_.size() > kHubMaxDecodes) { decodes_.pop_front(); }
}

std::optional<double> ControlHub::takeTune() {
    std::lock_guard<std::mutex> lk(m_);
    std::optional<double> r = tune_;
    tune_.reset();
    return r;
}

std::optional<std::string> ControlHub::takeMode() {
    std::lock_guard<std::mutex> lk(m_);
    std::optional<std::string> r = mode_;
    mode_.reset();
    return r;
}

HubStatus ControlHub::status() const {
    std::lock_guard<std::mutex> lk(m_);
    return status_;
}

std::vector<HubBookmark> ControlHub::bookmarks() const {
    std::lock_guard<std::mutex> lk(m_);
    return bookmarks_;
}

std::vector<HubDecode> ControlHub::recentDecodes(std::size_t maxCount) const {
    std::lock_guard<std::mutex> lk(m_);
    maxCount = std::min(maxCount, kHubMaxDecodes);
    const std::size_t n = std::min(maxCount, decodes_.size());
    return std::vector<HubDecode>(decodes_.end() - static_cast<std::ptrdiff_t>(n), decodes_.end());
}

std::vector<HubDecode> ControlHub::decodesSince(std::uint64_t& cursor, std::size_t maxCount) const {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<HubDecode> out;
    for (const HubDecode& d : decodes_) {
        if (d.seq > cursor && out.size() < maxCount) { out.push_back(d); }
    }
    if (!out.empty()) { cursor = out.back().seq; }
    return out;
}

std::uint64_t ControlHub::newestSeq() const {
    std::lock_guard<std::mutex> lk(m_);
    return nextSeq_ - 1;
}

bool ControlHub::takeToken() {
    const Clock::time_point now = now_ != nullptr ? now_() : Clock::now();
    if (!started_) { started_ = true; last_ = now; }
    const double dt = std::chrono::duration<double>(now - last_).count();
    last_ = now;
    tokens_ = std::min(20.0, tokens_ + std::max(0.0, dt) * 10.0);
    if (tokens_ < 1.0) { return false; }
    tokens_ -= 1.0;
    return true;
}

HubResult ControlHub::requestTune(double hz) {
    std::lock_guard<std::mutex> lk(m_);
    if (!std::isfinite(hz) || hz < kHubMinTuneHz || hz > kHubMaxTuneHz) {
        ++counts_.refused;
        return {false, "that is not a frequency this receiver can tune (1 kHz to 7.2 GHz)"};
    }
    if (!takeToken()) {
        ++counts_.rateLimited;
        return {false, "too many tuning requests: at most ten a second"};
    }
    tune_ = hz;
    ++counts_.tunesAccepted;
    return {};
}

HubResult ControlHub::requestMode(const std::string& modeName) {
    std::lock_guard<std::mutex> lk(m_);
    const std::string m = modeName.size() <= 8 ? canonicalMode(modeName) : std::string();
    if (m.empty()) {
        ++counts_.refused;
        return {false, "the mode is one of NFM, WFM, AM, DSB, USB, CW, LSB or RAW"};
    }
    if (!takeToken()) {
        ++counts_.rateLimited;
        return {false, "too many tuning requests: at most ten a second"};
    }
    mode_ = m;
    ++counts_.modesAccepted;
    return {};
}

ControlHub::Counts ControlHub::counts() const {
    std::lock_guard<std::mutex> lk(m_);
    return counts_;
}

}  // namespace cascade::net
