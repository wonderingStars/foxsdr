// wire_c_ham.cpp - see wire_c_ham.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/wire_c_ham.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>

#include <nlohmann/json.hpp>

#include "core/adif_qso.hpp"
#include "core/safe_file_io.hpp"

namespace cascade::engine::wirec {

namespace fs = std::filesystem;
namespace ham = cascade::core::ham;
using nlohmann::json;

namespace {

std::string strOf(const json& j, const char* key, std::size_t maxLen = 200) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) { return std::string(); }
    std::string s = it->get<std::string>();
    for (char& c : s) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) { c = ' '; }
    }
    if (s.size() > maxLen) { s.resize(maxLen); }
    return s;
}

std::vector<std::string> listOf(const json& j, const char* key, std::size_t maxItems, std::size_t maxLen) {
    std::vector<std::string> out;
    const auto it = j.find(key);
    if (it == j.end() || !it->is_array()) { return out; }
    for (const json& v : *it) {
        if (out.size() >= maxItems) { break; }
        if (v.is_string()) {
            std::string s = v.get<std::string>();
            for (char& c : s) {
                if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) { c = ' '; }
            }
            if (s.size() > maxLen) { s.resize(maxLen); }
            out.push_back(std::move(s));
        }
    }
    return out;
}

bool exchangeOk(const std::vector<std::string>& v) {
    if (v.size() > 8) { return false; }
    for (const std::string& t : v) {
        if (t.empty() || t.size() > 12) { return false; }
        for (char c : t) {
            if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) { return false; }
        }
    }
    return true;
}

json qsoJson(const ham::Qso& q) {
    json j;
    j["t"] = q.unixTime;
    j["f"] = q.freqKHz;
    j["m"] = q.mode;
    j["c"] = q.call;
    j["me"] = q.myCall;
    j["s"] = q.sent;
    j["r"] = q.rcvd;
    return j;
}

}  // namespace

void QsoBook::load() {
    qsos_.clear();
    header_ = ham::CabrilloHeader{};
    error_.clear();
    std::string text;
    const auto r = cascade::core::safeio::readCapped(file_, 16u * 1024u * 1024u, text);
    if (r == cascade::core::safeio::ReadResult::Missing) { return; }
    const json j = r == cascade::core::safeio::ReadResult::Ok ? json::parse(text, nullptr, false) : json();
    if (r != cascade::core::safeio::ReadResult::Ok || j.is_discarded() || !j.is_object()) {
        error_ = "the QSO log file cannot be read; it was left as it is";
        return;
    }
    if (j.contains("header") && j["header"].is_object()) {
        std::string ignored;
        const json h = j["header"];
        header_.contest = strOf(h, "contest", 32);
        header_.callsign = strOf(h, "callsign", 32);
        header_.location = strOf(h, "location", 20);
        header_.categoryAssisted = strOf(h, "assisted", 20);
        header_.categoryBand = strOf(h, "band", 20);
        header_.categoryMode = strOf(h, "mode", 20);
        header_.categoryOperator = strOf(h, "operator", 20);
        header_.categoryOverlay = strOf(h, "overlay", 20);
        header_.categoryPower = strOf(h, "power", 20);
        header_.categoryStation = strOf(h, "station", 20);
        header_.categoryTime = strOf(h, "time", 20);
        header_.categoryTransmitter = strOf(h, "transmitter", 20);
        header_.club = strOf(h, "club", 75);
        header_.email = strOf(h, "email", 75);
        header_.gridLocator = strOf(h, "grid", 8);
        header_.name = strOf(h, "name", 75);
        header_.claimedScore = h.contains("score") && h["score"].is_number_integer() ? h["score"].get<long long>() : -1;
        header_.address = listOf(h, "address", 6, 45);
        header_.operators = listOf(h, "operators", 32, 32);
        header_.soapbox = listOf(h, "soapbox", 20, 66);
    }
    if (j.contains("qsos") && j["qsos"].is_array()) {
        for (const json& e : j["qsos"]) {
            if (!e.is_object() || qsos_.size() >= kMaxQsos) { continue; }
            ham::Qso q;
            q.unixTime = e.contains("t") && e["t"].is_number_integer() ? e["t"].get<std::int64_t>() : 0;
            q.freqKHz = e.contains("f") && e["f"].is_number() ? e["f"].get<double>() : 0.0;
            q.mode = strOf(e, "m", 20);
            q.call = strOf(e, "c", 32);
            q.myCall = strOf(e, "me", 32);
            q.sent = listOf(e, "s", 8, 12);
            q.rcvd = listOf(e, "r", 8, 12);
            qsos_.push_back(std::move(q));
        }
    }
}

bool QsoBook::save() {
    json j;
    j["version"] = 1;
    json h = json::object();
    h["contest"] = header_.contest;
    h["callsign"] = header_.callsign;
    h["location"] = header_.location;
    h["assisted"] = header_.categoryAssisted;
    h["band"] = header_.categoryBand;
    h["mode"] = header_.categoryMode;
    h["operator"] = header_.categoryOperator;
    h["overlay"] = header_.categoryOverlay;
    h["power"] = header_.categoryPower;
    h["station"] = header_.categoryStation;
    h["time"] = header_.categoryTime;
    h["transmitter"] = header_.categoryTransmitter;
    h["club"] = header_.club;
    h["email"] = header_.email;
    h["grid"] = header_.gridLocator;
    h["name"] = header_.name;
    if (header_.claimedScore >= 0) { h["score"] = header_.claimedScore; }
    h["address"] = header_.address;
    h["operators"] = header_.operators;
    h["soapbox"] = header_.soapbox;
    j["header"] = h;
    json arr = json::array();
    for (const ham::Qso& q : qsos_) { arr.push_back(qsoJson(q)); }
    j["qsos"] = std::move(arr);
    return cascade::core::safeio::writeAtomic(file_, j.dump(1, ' ', false, json::error_handler_t::replace) + "\n");
}

bool QsoBook::add(const std::string& text, std::int64_t nowUnix, std::string& why) {
    if (!error_.empty()) {
        why = error_;
        return false;
    }
    if (qsos_.size() >= kMaxQsos) {
        why = "the log is full";
        return false;
    }
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        why = "a QSO is a JSON object";
        return false;
    }
    ham::Qso q;
    q.unixTime = j.contains("t") && j["t"].is_number_integer() ? j["t"].get<std::int64_t>() : 0;
    if (q.unixTime == 0) { q.unixTime = nowUnix; }
    if (q.unixTime < 0 || q.unixTime > 4102444800LL) {
        why = "the time is not a date";
        return false;
    }
    q.freqKHz = j.contains("f") && j["f"].is_number() ? j["f"].get<double>() : 0.0;
    if (!std::isfinite(q.freqKHz) || !(q.freqKHz > 0.0) || q.freqKHz > 3.0e8) {
        why = "the frequency (kHz) is missing or not a frequency";
        return false;
    }
    q.mode = ham::upperAscii(strOf(j, "m", 20));
    if (q.mode.empty()) {
        why = "the mode is missing";
        return false;
    }
    if (!ham::normaliseCall(strOf(j, "c", 32), q.call)) {
        why = "the call sign has characters other than A-Z, 0-9 and /";
        return false;
    }
    const std::string me = strOf(j, "me", 32);
    if (!me.empty() && !ham::normaliseCall(me, q.myCall)) {
        why = "your call sign has characters other than A-Z, 0-9 and /";
        return false;
    }
    if (q.myCall.empty()) { ham::normaliseCall(header_.callsign, q.myCall); }
    q.sent = listOf(j, "s", 8, 12);
    q.rcvd = listOf(j, "r", 8, 12);
    if (!exchangeOk(q.sent) || !exchangeOk(q.rcvd)) {
        why = "an exchange value is empty, too long or has a character other than letters and digits";
        return false;
    }
    qsos_.push_back(std::move(q));
    if (!save()) {
        qsos_.pop_back();
        why = "the log could not be saved";
        return false;
    }
    return true;
}

bool QsoBook::remove(std::size_t oneBased, std::string& why) {
    if (oneBased == 0 || oneBased > qsos_.size()) {
        why = "no such QSO";
        return false;
    }
    const ham::Qso gone = qsos_[oneBased - 1];
    qsos_.erase(qsos_.begin() + static_cast<std::ptrdiff_t>(oneBased - 1));
    if (!save()) {
        qsos_.insert(qsos_.begin() + static_cast<std::ptrdiff_t>(oneBased - 1), gone);
        why = "the log could not be saved";
        return false;
    }
    return true;
}

bool QsoBook::clear() {
    if (qsos_.empty()) { return false; }
    const std::vector<ham::Qso> before = qsos_;
    qsos_.clear();
    if (!save()) {
        qsos_ = before;
        return false;
    }
    return true;
}

bool QsoBook::setHeader(const std::string& text, std::string& why) {
    if (!error_.empty()) {
        why = error_;
        return false;
    }
    const json h = json::parse(text, nullptr, false);
    if (h.is_discarded() || !h.is_object()) {
        why = "the header is a JSON object";
        return false;
    }
    const ham::CabrilloHeader before = header_;
    ham::CabrilloHeader n;
    n.contest = strOf(h, "contest", 32);
    n.callsign = strOf(h, "callsign", 32);
    n.location = strOf(h, "location", 20);
    n.categoryAssisted = strOf(h, "assisted", 20);
    n.categoryBand = strOf(h, "band", 20);
    n.categoryMode = strOf(h, "mode", 20);
    n.categoryOperator = strOf(h, "operator", 20);
    n.categoryOverlay = strOf(h, "overlay", 20);
    n.categoryPower = strOf(h, "power", 20);
    n.categoryStation = strOf(h, "station", 20);
    n.categoryTime = strOf(h, "time", 20);
    n.categoryTransmitter = strOf(h, "transmitter", 20);
    n.club = strOf(h, "club", 75);
    n.email = strOf(h, "email", 75);
    n.gridLocator = strOf(h, "grid", 8);
    n.name = strOf(h, "name", 75);
    n.claimedScore = h.contains("score") && h["score"].is_number_integer() ? h["score"].get<long long>() : -1;
    n.address = listOf(h, "address", 6, 45);
    n.operators = listOf(h, "operators", 32, 32);
    n.soapbox = listOf(h, "soapbox", 20, 66);
    if (!n.callsign.empty()) {
        std::string c;
        if (!ham::normaliseCall(n.callsign, c)) {
            why = "the call sign has characters other than A-Z, 0-9 and /";
            return false;
        }
        n.callsign = c;
    }
    header_ = std::move(n);
    if (!save()) {
        header_ = before;
        why = "the log could not be saved";
        return false;
    }
    return true;
}

bool QsoBook::exportCabrillo(const std::string& path, std::string& problems, int& lines, int& skipped, std::string& why) const {
    problems.clear();
    lines = 0;
    skipped = 0;
    if (path.empty() || path.size() > 520) {
        why = "no file named";
        return false;
    }
    const ham::CabrilloResult r = ham::writeCabrillo(header_, qsos_);
    for (const std::string& p : r.problems) { problems += p + "\n"; }
    lines = r.qsoLines;
    skipped = r.skipped;
    if (!r.ok) {
        why = "the log header was refused (see the problems)";
        return false;
    }
    if (!ham::writeCabrilloFile(path, r.text)) {
        why = "the file could not be written";
        return false;
    }
    return true;
}

bool QsoBook::exportAdif(const std::string& path, int& written, int& skipped, std::string& why) const {
    written = 0;
    skipped = 0;
    if (path.empty() || path.size() > 520) {
        why = "no file named";
        return false;
    }
    ham::AdifQsoOptions opt;
    opt.contest = ham::findCabrilloContest(header_.contest);
    opt.contestId = header_.contest;
    std::string out = ham::adifHeader("FoxSDR", "1");
    for (const ham::Qso& q : qsos_) {
        const std::string rec = ham::adifQsoRecord(q, opt);
        if (rec.empty()) {
            ++skipped;
            continue;
        }
        out += rec;
        ++written;
    }
    if (written == 0) {
        why = "no QSO in the log can be written as ADIF";
        return false;
    }
    if (!cascade::core::safeio::writeAtomic(fs::path(path), out)) {
        why = "the file could not be written";
        return false;
    }
    return true;
}

}  // namespace cascade::engine::wirec
