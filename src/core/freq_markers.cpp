// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/freq_markers.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>

#include <nlohmann/json.hpp>

#include "core/freq_manager.hpp"

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace cascade::core {

namespace fs = std::filesystem;
using json = nlohmann::json;

std::string FreqMarkers::defaultPath() {
    // Beside bookmarks.json, so the two lists share one directory and one set
    // of platform rules.
    return (fs::path(FreqManager::defaultPath()).parent_path() / "markers.json").string();
}

int FreqMarkers::add(double freqHz, std::int64_t notedUnix, double mergeHz) {
    if (!std::isfinite(freqHz) || !(freqHz > 0.0)) { return 0; }
    const double tol = (std::isfinite(mergeHz) && mergeHz > 0.0) ? mergeHz : 0.0;
    if (const int same = nearest(freqHz, tol); same != 0) { return same; }
    if (list_.size() >= kMaxMarkers) { return 0; }
    FreqMarker m;
    m.number = next_++;
    m.freqHz = freqHz;
    m.notedUnix = notedUnix;
    list_.push_back(std::move(m));
    ++version_;
    return list_.back().number;
}

bool FreqMarkers::remove(int number) {
    const auto it = std::find_if(list_.begin(), list_.end(),
                                 [number](const FreqMarker& m) { return m.number == number; });
    if (it == list_.end()) { return false; }
    list_.erase(it);
    ++version_;
    return true;
}

bool FreqMarkers::setNote(int number, const std::string& note) {
    for (FreqMarker& m : list_) {
        if (m.number != number) { continue; }
        if (m.note != note) {
            m.note = note;
            ++version_;
        }
        return true;
    }
    return false;
}

void FreqMarkers::clear() {
    list_.clear();
    next_ = 1;
    ++version_;
}

int FreqMarkers::nearest(double freqHz, double tolHz) const {
    if (!std::isfinite(freqHz)) { return 0; }
    int best = 0;
    double bestD = 0.0;
    for (const FreqMarker& m : list_) {
        const double d = std::fabs(m.freqHz - freqHz);
        if (d <= tolHz && (best == 0 || d < bestD)) {
            best = m.number;
            bestD = d;
        }
    }
    return best;
}

std::string formatMarkerTime(std::int64_t unixSeconds, bool utc) {
    const std::time_t t = static_cast<std::time_t>(unixSeconds);
    std::tm tmv{};
#ifdef _WIN32
    const bool ok = (utc ? gmtime_s(&tmv, &t) : localtime_s(&tmv, &t)) == 0;
#else
    const bool ok = (utc ? gmtime_r(&t, &tmv) : localtime_r(&t, &tmv)) != nullptr;
#endif
    if (!ok) { return std::string(); }
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    return buf;
}

std::string FreqMarkers::clipboardText(bool utc) const {
    if (list_.empty()) { return std::string(); }
    std::vector<const FreqMarker*> byFreq;
    byFreq.reserve(list_.size());
    for (const FreqMarker& m : list_) { byFreq.push_back(&m); }
    std::stable_sort(byFreq.begin(), byFreq.end(),
                     [](const FreqMarker* a, const FreqMarker* b) { return a->freqHz < b->freqHz; });
    std::string out = "FoxSDR markers (" + std::to_string(list_.size()) + ")\n";
    for (const FreqMarker* m : byFreq) {
        char line[96];
        std::snprintf(line, sizeof(line), "M%d\t%.6f MHz\t", m->number, m->freqHz / 1.0e6);
        out += line;
        out += formatMarkerTime(m->notedUnix, utc);
        if (!m->note.empty()) {
            // A tab or a newline typed into a note would split its row into
            // columns or lines that are not there; they paste as spaces.
            std::string note = m->note;
            std::replace(note.begin(), note.end(), '\t', ' ');
            std::replace(note.begin(), note.end(), '\n', ' ');
            std::replace(note.begin(), note.end(), '\r', ' ');
            out += '\t';
            out += note;
        }
        out += '\n';
    }
    return out;
}

bool FreqMarkers::load(const std::string& path, std::string& error) {
    list_.clear();
    next_ = 1;
    ++version_;
    error.clear();

    std::error_code ec;
    if (!fs::exists(fs::path(path), ec)) { return true; }
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "markers: cannot open \"" + path + "\" for reading";
        return false;
    }
    const json j = json::parse(f, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        error = "markers: \"" + path + "\" is not a JSON object";
        return false;
    }
    const auto ver = j.find("schemaVersion");
    if (ver == j.end() || !ver->is_number_integer() || ver->get<int>() != 1) {
        error = "markers: \"" + path + "\" has an unknown schemaVersion";
        return false;
    }
    const auto arr = j.find("markers");
    if (arr == j.end()) { return true; }
    if (!arr->is_array()) {
        error = "markers: \"markers\" in \"" + path + "\" is not an array";
        return false;
    }
    std::set<int> seen;
    int highest = 0;
    for (const json& e : *arr) {
        if (!e.is_object()) { continue; }
        const auto hz = e.find("freqHz");
        const auto n = e.find("n");
        if (hz == e.end() || !hz->is_number() || n == e.end() || !n->is_number_integer()) { continue; }
        FreqMarker m;
        m.freqHz = hz->get<double>();
        m.number = n->get<int>();
        if (!std::isfinite(m.freqHz) || !(m.freqHz > 0.0) || m.number <= 0) { continue; }
        if (!seen.insert(m.number).second) { continue; }
        if (list_.size() >= kMaxMarkers) { break; }
        if (const auto t = e.find("noted"); t != e.end() && t->is_number_integer()) {
            m.notedUnix = t->get<std::int64_t>();
        }
        if (const auto s = e.find("note"); s != e.end() && s->is_string()) {
            m.note = s->get<std::string>();
        }
        highest = std::max(highest, m.number);
        list_.push_back(std::move(m));
    }
    std::sort(list_.begin(), list_.end(),
              [](const FreqMarker& a, const FreqMarker& b) { return a.number < b.number; });
    // The next number is past every one in the file, whatever "next" says: a
    // hand-edited or older file must never make add() hand out a number that
    // is already in the list.
    int next = highest + 1;
    if (const auto nx = j.find("next"); nx != j.end() && nx->is_number_integer()) {
        next = std::max(next, nx->get<int>());
    }
    next_ = next;
    return true;
}

bool FreqMarkers::save(const std::string& path, std::string& error) const {
    return writeFile(path, serialize(), error);
}

std::string FreqMarkers::serialize() const {
    json arr = json::array();
    for (const FreqMarker& m : list_) {
        json e;
        e["n"] = m.number;
        e["freqHz"] = m.freqHz;
        e["noted"] = m.notedUnix;
        if (!m.note.empty()) { e["note"] = m.note; }
        arr.push_back(std::move(e));
    }
    json j;
    j["schemaVersion"] = 1;
    j["next"] = next_;
    j["markers"] = std::move(arr);
    // error_handler_t::replace: a note is user text (tests/test_json_dump_policy).
    return j.dump(4, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
}

bool FreqMarkers::writeFile(const std::string& path, const std::string& text,
                            std::string& error) {
    error.clear();
    const fs::path target(path);
    std::error_code ec;
    const fs::path parent = target.parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, ec);
        if (ec || !fs::is_directory(parent)) {
            error = "markers: cannot create directory \"" + parent.string() + "\"";
            return false;
        }
    }

#ifdef _WIN32
    const int pid = _getpid();
#else
    const int pid = static_cast<int>(getpid());
#endif
    fs::path tmp = target;
    tmp += "." + std::to_string(pid) + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            error = "markers: cannot create temp file \"" + tmp.string() + "\"";
            return false;
        }
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        f.flush();
        if (!f) {
            f.close();
            fs::remove(tmp, ec);
            error = "markers: write to temp file \"" + tmp.string() + "\" failed";
            return false;
        }
    }
    fs::rename(tmp, target, ec);
    if (ec) {
        std::error_code ignored;
        fs::remove(tmp, ignored);
        error = "markers: atomic replace of \"" + path + "\" failed: " + ec.message();
        return false;
    }
    return true;
}

double markerStepHz(double hzPerPixel) {
    if (!std::isfinite(hzPerPixel) || !(hzPerPixel > 1.0)) { return 1.0; }
    double decade = std::pow(10.0, std::floor(std::log10(hzPerPixel)));
    // log10 of an exact power of ten can land a hair under the integer; the
    // decade must never exceed the pixel.
    if (decade > hzPerPixel) { decade /= 10.0; }
    if (5.0 * decade <= hzPerPixel) { return 5.0 * decade; }
    if (2.0 * decade <= hzPerPixel) { return 2.0 * decade; }
    return decade;
}

double roundMarkerHz(double hz, double hzPerPixel) {
    const double step = markerStepHz(hzPerPixel);
    return std::round(hz / step) * step;
}

int markerMhzDecimals(double stepHz) {
    if (!std::isfinite(stepHz) || !(stepHz > 1.0)) { return 6; }
    // Decimals of MHz that resolve the step's own decade: 6 - floor(log10).
    // A 1-2-5 step is a whole multiple of its decade, so that is enough.
    const int d = 6 - static_cast<int>(std::floor(std::log10(stepHz) + 1e-9));
    return std::clamp(d, 3, 6);
}

std::string formatMarkerMhz(double hz, double stepHz) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.*f", markerMhzDecimals(stepHz), hz / 1.0e6);
    return buf;
}

std::string markerMhzText(double hz) {
    const double wholeHz = std::round(hz);
    int decimals = 3;
    // Each decimal past the third resolves a tenth of the one before it:
    // 3 -> 1 kHz, 4 -> 100 Hz, 5 -> 10 Hz, 6 -> 1 Hz.
    for (double unit = 1000.0; decimals < 6 && std::fmod(wholeHz, unit) != 0.0; unit /= 10.0) {
        ++decimals;
    }
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, wholeHz / 1.0e6);
    return buf;
}

}  // namespace cascade::core
