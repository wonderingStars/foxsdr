// s4b_swl.cpp - see s4b_swl.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/s4b_swl.hpp"

#include <cmath>

#include <nlohmann/json.hpp>

#include "foxsdr_api.h"

#include "core/safe_file_io.hpp"

namespace cascade::engine::s4b {

namespace {
using nlohmann::json;
namespace ad = cascade::core::adif;

constexpr std::size_t kMaxText = 200;

std::string cleanLine(const std::string& in, std::size_t maxBytes) {
    std::string out;
    for (const char ch : in) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (out.size() >= maxBytes) { break; }
        if (c == '\n' || c == '\t') { out += ' '; }
        else if (c < 0x20 || c == 0x7F) { continue; }
        else { out += ch; }
    }
    // never end inside a multi-byte character
    while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80) { out.pop_back(); }
    if (!out.empty() && static_cast<unsigned char>(out.back()) >= 0xC0) { out.pop_back(); }
    return out;
}

bool strField(const json& j, const char* key, std::string& out, std::string& why) {
    if (!j.contains(key)) { return true; }
    if (!j[key].is_string()) { why = std::string(key) + " is a text"; return false; }
    out = cleanLine(j[key].get<std::string>(), kMaxText);
    return true;
}
}  // namespace

void SwlBook::load() {
    entries_.clear();
    error_.clear();
    std::error_code ec;
    if (!std::filesystem::exists(file_, ec)) { return; }
    std::string text;
    {
        std::FILE* f = std::fopen(file_.string().c_str(), "rb");
        if (f == nullptr) { error_ = "the listening log could not be read"; return; }
        char buf[8192];
        std::size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && text.size() < (64u << 20)) { text.append(buf, n); }
        std::fclose(f);
    }
    const json j = json::parse(text, nullptr, false);
    // A field of the wrong type (a text where a number belongs) is a damaged file, never an exception at start-up: nlohmann's value() throws on a type mismatch.
    bool damaged = j.is_discarded() || !j.is_object() || !j.contains("entries") || !j["entries"].is_array();
    if (!damaged) {
        const auto isStr = [](const json& e, const char* k) { const auto it = e.find(k); return it == e.end() || it->is_string(); };
        const auto isNum = [](const json& e, const char* k) { const auto it = e.find(k); return it == e.end() || it->is_number(); };
        const auto isInt = [](const json& e, const char* k) { const auto it = e.find(k); return it == e.end() || it->is_number_integer(); };
        const auto isSmall = [](const json& e, const char* k) { const auto it = e.find(k); return it == e.end() || (it->is_number_integer() && it->get<double>() > -2.0e9 && it->get<double>() < 2.0e9); };
        for (const json& e : j["entries"]) {
            if (!e.is_object() || !isInt(e, "t") || !isNum(e, "d") || !isNum(e, "f") || !isStr(e, "m") || !isStr(e, "c") || !isStr(e, "s") || !isStr(e, "n") || !isSmall(e, "r") || !isNum(e, "p")) { damaged = true; break; }
        }
    }
    if (damaged) {
        error_ = "the listening log is damaged and was not used";
        std::error_code mv;   // the damaged file is kept beside the log, so the next save does not destroy what the person may still repair
        std::filesystem::path aside = file_;
        aside += ".damaged";
        std::filesystem::rename(file_, aside, mv);
        return;
    }
    for (const json& e : j["entries"]) {
        if (entries_.size() >= kMaxSwlEntries) { break; }
        ad::ListenEntry le;
        le.startUnix = e.value("t", static_cast<std::int64_t>(0));
        le.durationS = e.value("d", 0.0);
        le.frequencyHz = e.value("f", 0.0);
        le.mode = cleanLine(e.value("m", std::string()), 8);
        le.call = cleanLine(e.value("c", std::string()), kMaxText);
        le.station = cleanLine(e.value("s", std::string()), kMaxText);
        le.notes = cleanLine(e.value("n", std::string()), kMaxText);
        le.radio = e.value("r", 0);
        if (e.contains("p")) { le.peakDbfs = e["p"].get<double>(); }
        if (!std::isfinite(le.frequencyHz) || le.frequencyHz <= 0.0 || le.startUnix <= 0) { continue; }
        entries_.push_back(std::move(le));
    }
}

bool SwlBook::save() {
    json arr = json::array();
    for (const ad::ListenEntry& e : entries_) {
        json o = {{"t", e.startUnix}, {"d", e.durationS}, {"f", e.frequencyHz}, {"m", e.mode}, {"c", e.call}, {"s", e.station}, {"n", e.notes}, {"r", e.radio}};
        if (std::isfinite(e.peakDbfs)) { o["p"] = e.peakDbfs; }
        arr.push_back(std::move(o));
    }
    json j = {{"version", 1}, {"entries", std::move(arr)}};
    return cascade::core::safeio::writeAtomic(file_, j.dump(1, ' ', false, json::error_handler_t::replace) + "\n");
}

bool SwlBook::add(const std::string& jsonText, std::int64_t nowUnix, double frequencyHz, const std::string& mode, double peakDbfs, int radio, std::string& why) {
    if (entries_.size() >= kMaxSwlEntries) { why = "the log is full"; return false; }
    if (!std::isfinite(frequencyHz) || frequencyHz <= 0.0) { why = "the radio has no frequency to log"; return false; }
    json j = json::object();
    if (!jsonText.empty()) {
        j = json::parse(jsonText, nullptr, false);
        if (j.is_discarded() || !j.is_object()) { why = "the entry is a JSON object"; return false; }
    }
    ad::ListenEntry e;
    for (auto it = j.begin(); it != j.end(); ++it) {
        const std::string& k = it.key();
        if (k != "call" && k != "station" && k != "notes") { why = "an unknown key in the entry"; return false; }
    }
    if (!strField(j, "call", e.call, why) || !strField(j, "station", e.station, why) || !strField(j, "notes", e.notes, why)) { return false; }
    e.startUnix = nowUnix;
    e.frequencyHz = frequencyHz;
    e.mode = cleanLine(mode, 8);
    e.peakDbfs = peakDbfs;
    e.radio = radio;
    entries_.push_back(std::move(e));
    if (!save()) {
        entries_.pop_back();
        why = "the log could not be saved";
        return false;
    }
    return true;
}

bool SwlBook::remove(std::size_t oneBasedIndex, std::string& why) {
    if (oneBasedIndex < 1 || oneBasedIndex > entries_.size()) { why = "there is no such entry"; return false; }
    ad::ListenEntry keep = entries_[oneBasedIndex - 1];
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(oneBasedIndex - 1));
    if (!save()) {
        entries_.insert(entries_.begin() + static_cast<std::ptrdiff_t>(oneBasedIndex - 1), std::move(keep));
        why = "the log could not be saved";
        return false;
    }
    return true;
}

bool SwlBook::clear() {
    if (entries_.empty()) { return false; }
    std::vector<ad::ListenEntry> keep;
    keep.swap(entries_);
    if (!save()) { entries_.swap(keep); return false; }
    return true;
}

bool SwlBook::exportAdif(const std::string& path, const std::string& operatorCall, const std::string& grid, std::int64_t nowUnix, int& written, int& skipped, std::string& why) const {
    written = 0;
    skipped = 0;
    if (path.empty() || path.size() > 520) { why = "no file named"; return false; }
    const std::filesystem::path p(std::u8string(path.begin(), path.end()));
    if (!p.is_absolute()) { why = "the file must be named with its full path"; return false; }
    std::string ext = p.extension().string();
    for (char& c : ext) { if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); } }
    if (ext != ".adi" && ext != ".adif") { why = "the file name ends in .adi or .adif"; return false; }
    ad::Options o;
    o.operatorCall = operatorCall;
    o.myGrid = grid;
    o.requireCall = false;   // a listener's note of a station heard, named or not
    ad::Stats st;
    const std::string text = ad::adifFromEntries(entries_, o, nowUnix, &st);
    written = st.written;
    skipped = static_cast<int>(entries_.size()) - st.written;
    if (st.written == 0) { why = "no entry of the log can be written as ADIF"; return false; }
    if (!cascade::core::safeio::writeAtomic(p, text)) { why = "the file could not be written"; return false; }
    return true;
}

std::int32_t swlOp(SwlBook& book, const std::string& text, const RadioFigures& rf, std::int64_t nowUnix, const std::string& operatorCall, const std::string& grid, std::string& why, std::string& exported) {
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("action") || !j["action"].is_string()) { why = "the text is JSON with an action: add, remove, clear or export"; return FOXAPI_BAD_ARGUMENT; }
    const std::string action = j["action"].get<std::string>();
    if (action == "add") {
        json rest = j;
        rest.erase("action");
        if (!rf.valid) { why = "the radio is not open: there is nothing to log"; return FOXAPI_FAILED; }
        if (!book.add(rest.dump(), nowUnix, rf.hz, rf.mode, rf.peakDbfs, rf.radio, why)) { return why == "the log is full" ? FOXAPI_LIMIT : FOXAPI_BAD_ARGUMENT; }
        return FOXAPI_OK;
    }
    if (action == "remove") {
        if (j.size() != 2 || !j.contains("index") || !j["index"].is_number_integer()) { why = "remove takes an index"; return FOXAPI_BAD_ARGUMENT; }
        const long long i = j["index"].get<long long>();
        if (!book.remove(i > 0 ? static_cast<std::size_t>(i) : 0u, why)) { return FOXAPI_NOT_FOUND; }
        return FOXAPI_OK;
    }
    if (action == "clear") {
        if (j.size() != 1) { why = "clear takes nothing else"; return FOXAPI_BAD_ARGUMENT; }
        if (!book.clear()) { why = "the log is empty"; return FOXAPI_NO_CHANGE; }
        return FOXAPI_OK;
    }
    if (action == "export") {
        if (j.size() != 2 || !j.contains("path") || !j["path"].is_string()) { why = "export takes a path"; return FOXAPI_BAD_ARGUMENT; }
        int written = 0, skipped = 0;
        if (!book.exportAdif(j["path"].get<std::string>(), operatorCall, grid, nowUnix, written, skipped, why)) { return FOXAPI_BAD_ARGUMENT; }
        exported = "ADIF: " + std::to_string(written) + " entr" + (written == 1 ? "y" : "ies") + " written, " + std::to_string(skipped) + " skipped";
        return FOXAPI_OK;
    }
    why = "the action is add, remove, clear or export";
    return FOXAPI_BAD_ARGUMENT;
}

}  // namespace cascade::engine::s4b
