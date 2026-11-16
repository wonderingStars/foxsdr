// wire_c.cpp - the settings registry of wiring lane C (see wire_c.hpp).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "engine/wire_c.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <nlohmann/json.hpp>

#include "core/diag_log.hpp"
#include "core/rec_naming.hpp"
#include "core/safe_file_io.hpp"
#include "core/secret_vault.hpp"
#include "engine/s4_outputs.hpp"
#include "net/web_cors.hpp"

namespace cascade::engine::wirec {

namespace {

using nlohmann::json;

// The registry. APPEND ONLY (a key is never renamed or removed: a saved file names it). Each row is documented in docs/multi-radio-records/wire-c.md.
// A feature that opens a network port, a device or a file is OFF ("0") here.
constexpr Spec kSpecs[] = {
    // --- system (sysmodels) ---
    {"sys.keep_awake", Kind::Bool, 0, 0, "1", ""},            // keep the PC awake (never the display) while a recording or scheduled capture runs
    {"sys.resume_reopen", Kind::Bool, 0, 0, "1", ""},         // look for the radios again when the PC wakes from sleep
    {"sys.backup_on_close", Kind::Bool, 0, 0, "0", ""},       // back the settings up when FoxSDR closes
    {"sys.backup_keep", Kind::Int, 1, 50, "10", ""},          // how many backups to keep
    {"sys.sync_folder", Kind::Text, 0, 260, "", ""},          // a shared folder for syncing profiles (empty = off)
    {"perf.mode", Kind::Choice, 0, 0, "auto", "auto|on|off"}, // reduced effects: automatic, always, never
    // --- accessibility (access) ---
    {"access.units", Kind::Choice, 0, 0, "automatic", "automatic|metric|imperial|aviation|nautical"},
    {"access.number_format", Kind::Choice, 0, 0, "dot", "dot|windows"},
    {"access.digits", Kind::Choice, 0, 0, "latin", "latin|native|language"},
    {"access.palette", Kind::Choice, 0, 0, "normal", "normal|deuteranopia|protanopia|tritanopia|highcontrast"},
    // --- the Plugin store (webmodels, store sort) ---
    {"store.sort", Kind::Choice, 0, 0, "catalogue", "catalogue|name|updated"},   // the order of the store's list; "popular" and "rating" wait for the server's data (none exists)
    // --- ham operations (hamops): programs followed, receive only ---
    {"hamops.radioinfo.enabled", Kind::Bool, 0, 0, "0", ""},    // follow the radio information a logger (DXLab Commander, N1MM+) broadcasts; shown only, never retunes FoxSDR
    {"hamops.radioinfo.address", Kind::Text, 0, 15, "127.0.0.1", ""},   // the address to listen on (dotted IPv4; the local machine unless allow_other)
    {"hamops.radioinfo.port", Kind::Int, 0, 65535, "0", ""},    // the port the logger sends to (0 = not configured: nothing is opened)
    {"hamops.radioinfo.allow_other", Kind::Bool, 0, 0, "0", ""},   // accept an address other than this computer
    // --- CAT emulation for logging programs (receive only) ---
    {"cat.emulation.enabled", Kind::Bool, 0, 0, "0", ""},       // answer like a rig on a TCP port: read and set the frequency and mode; every transmit command is refused
    {"cat.emulation.dialect", Kind::Choice, 0, 0, "kenwood-ts480", "kenwood-ts480|yaesu-ft891"},
    {"cat.emulation.port", Kind::Int, 0, 65535, "0", ""},       // the port the logging program connects to (0 = not configured; 4532 is the rigctl server's)
    {"cat.emulation.bind_all", Kind::Bool, 0, 0, "0", ""},      // listen on every address instead of this computer only
    // --- web (webmodels) ---
    {"web.cors_origins", Kind::Text, 0, 2000, "", ""},        // allowed origins, one per line (empty = no cross-origin page may read)
    {"web.metrics", Kind::Bool, 0, 0, "0", ""},               // the /metrics page for a signed-in browser (Prometheus text format); off = as if it did not exist
    // --- the RDS log (engine lane E1, FEATURE-GAPS G4-53; core/rds_log.hpp): text only, written beside nothing but the folder the user chose, OFF ---
    {"rds.log", Kind::Bool, 0, 0, "0", ""},                   // write the station name, programme type and radio text of an FM radio's RDS to a file when they change
    {"rds.log.folder", Kind::Text, 0, 260, "", ""},           // the folder (a full path) the log is written in; nothing is written without one
    {"rds.log.name", Kind::Text, 0, 120, "rds_{date}", ""},   // the file name, by the recorder's template rules ({date} {time} {freq_mhz} {radio} ...); ".csv" is added
    {"sig.symbol_rate", Kind::Real, 0, 1000000, "0", ""},         // the symbol rate in baud the user sets for plugins that can use it (0 = none); plugins read it with the host call symbol_rate
    {"web.cors_credentials", Kind::Choice, 0, 0, "off", "off"},   // credentialed cross-origin requests: never (the session cookie is SameSite=Strict; an owner decision with HTTPS)
    // --- screen lane S4: where the decoded-text feed is SENT, and keyword alerts (engine/s4_outputs.hpp). Every switch is OFF; every address defaults to this computer. The keys named in secretKeys()
    // (the webhook address, which may carry a token, and the MQTT password) live in the Windows per-user protected store, never in wirec.json, and are never published as a fact. ---
    {"out.mqtt.enabled", Kind::Bool, 0, 0, "0", ""},            // publish every decoded line (or only the alerts) to an MQTT broker; the broker's address is below
    {"out.mqtt.host", Kind::Text, 0, 253, "127.0.0.1", ""},     // the broker: this computer unless the user types another host
    {"out.mqtt.port", Kind::Int, 1, 65535, "1883", ""},
    {"out.mqtt.topic", Kind::Text, 0, 200, "foxsdr/decodes", ""},
    {"out.mqtt.user", Kind::Text, 0, 128, "", ""},
    {"out.mqtt.password", Kind::Text, 0, 128, "", ""},          // SECRET (protected store)
    {"out.mqtt.send", Kind::Choice, 0, 0, "decodes", "decodes|alerts"},
    {"out.webhook.enabled", Kind::Bool, 0, 0, "0", ""},         // post every decoded line (or only the alerts) to a web address: https, or http on this computer only
    {"out.webhook.url", Kind::Text, 0, 2048, "", ""},           // SECRET (protected store): the address may carry a token
    {"out.webhook.send", Kind::Choice, 0, 0, "decodes", "decodes|alerts"},
    {"out.influx.enabled", Kind::Bool, 0, 0, "0", ""},          // write the selected radio's signal level to an InfluxDB (line protocol), every out.influx.every_s seconds
    {"out.influx.host", Kind::Text, 0, 253, "127.0.0.1", ""},   // this computer unless the user types another host (no encryption on the way: the token crosses the network as it is)
    {"out.influx.port", Kind::Int, 1, 65535, "8086", ""},
    {"out.influx.mode", Kind::Choice, 0, 0, "http", "http|udp"},
    {"out.influx.org", Kind::Text, 0, 64, "", ""},
    {"out.influx.bucket", Kind::Text, 0, 64, "foxsdr", ""},
    {"out.influx.token", Kind::Text, 0, 256, "", ""},           // SECRET (protected store)
    {"out.influx.every_s", Kind::Int, 1, 3600, "10", ""},
    {"alerts.enabled", Kind::Bool, 0, 0, "0", ""},              // raise an alert when a decoded line holds one of the keywords
    {"alerts.keywords", Kind::Text, 0, 4000, "", ""},           // one keyword or phrase per line, at most 64
    {"alerts.gap_s", Kind::Int, 0, 3600, "30", ""},             // the same keyword again within this many seconds is not raised again
    {"alerts.notify", Kind::Bool, 0, 0, "1", ""},               // hand a raised alert to the desktop notification (shown only where the system can)
    // --- screen lane S4b: the servers other programs connect to (engine/s4b_servers.hpp). Every one is OFF; every one listens on this computer only unless its own "all" key is turned on. ---
    {"srv.tcpaudio.enabled", Kind::Bool, 0, 0, "0", ""},        // serve one radio's audio over TCP (raw 16-bit mono, 48000 a second: the format of the UDP audio)
    {"srv.tcpaudio.all", Kind::Bool, 0, 0, "0", ""},            // listen on every address instead of this computer only (no password: anyone on the network can listen)
    {"srv.tcpaudio.port", Kind::Int, 1024, 65535, "7356", ""},
    {"srv.tcpaudio.radio", Kind::Int, 1, 8, "1", ""},
    {"srv.tcpaudio.packet", Kind::Choice, 0, 0, "480", "480|960"},   // samples per write: 10 ms or 20 ms
    {"srv.control.radio", Kind::Int, 1, 8, "1", ""},            // the radio the network streams and controls below act on
    {"srv.rtltcp.enabled", Kind::Bool, 0, 0, "0", ""},          // serve that radio's I/Q to programs that read an RTL-SDR over the network (rtl_tcp); only retuning is accepted from them
    {"srv.rtltcp.all", Kind::Bool, 0, 0, "0", ""},              // listen on every address instead of this computer only (no password)
    {"srv.rtltcp.port", Kind::Int, 1024, 65535, "1234", ""},
    {"srv.api.enabled", Kind::Bool, 0, 0, "0", ""},             // the WebSocket and SSE interface: other programs read the receiver's status, bookmarks and decoded lines and retune it
    {"srv.api.all", Kind::Bool, 0, 0, "0", ""},                 // listen on every address (programs must then sign in with the web page's account; refused while the web page has no password)
    {"srv.api.port", Kind::Int, 1024, 65535, "8074", ""},
    {"srv.mcp.enabled", Kind::Bool, 0, 0, "0", ""},             // the MCP server (five tools: status, bookmarks, recent decodes, tune, set mode); this computer only
    {"srv.mcp.port", Kind::Int, 1024, 65535, "8075", ""},
    {"srv.mcp.token", Kind::Text, 0, 128, "", ""},              // SECRET (protected store): when set, a client must send it as a bearer token
    {"rot.enabled", Kind::Bool, 0, 0, "0", ""},                 // keep a link to an antenna rotator (turned only on the person's request; never at start-up)
    {"rot.kind", Kind::Choice, 0, 0, "rotctld", "rotctld|gs232"},   // rotctld over TCP, or a Yaesu GS-232 controller on a serial port
    {"rot.host", Kind::Text, 1, 253, "127.0.0.1", ""},          // rotctld: this computer unless the person types another host (no password, no encryption)
    {"rot.port", Kind::Int, 1, 65535, "4533", ""},
    {"rot.serial", Kind::Text, 0, 12, "", ""},                  // GS-232: a COM port name like COM3
    {"rot.baud", Kind::Choice, 0, 0, "9600", "1200|2400|4800|9600|19200"},
    {"rot.az_max", Kind::Choice, 0, 0, "450", "360|450"},       // the controller's azimuth range
    {"rot.park_az", Kind::Real, 0, 450, "0", ""},
    {"rot.park_el", Kind::Real, 0, 180, "0", ""},
    {"swl.operator", Kind::Text, 0, 16, "", ""},                // the listener's identifier written into the listening log's ADIF export
    {"swl.grid", Kind::Text, 0, 8, "", ""},                     // the listener's Maidenhead square (2 to 8 characters), optional
};
constexpr std::size_t kSpecCount = sizeof(kSpecs) / sizeof(kSpecs[0]);

bool validText(const std::string& s, std::size_t maxBytes) {
    if (s.size() > maxBytes) { return false; }
    for (unsigned char ch : s) {
        if ((ch < 0x20u && ch != '\n') || ch == 0x7Fu) { return false; }   // no control character except the line break of a list
    }
    return true;
}

std::string numberText(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9g", v);
    return buf;
}

// Rules a key has beyond its kind (empty = fine). Called with the normalised value.
std::string extraRule(const std::string& key, const std::string& value) {
    if (key == "web.cors_origins") {
        std::size_t count = 0, from = 0;
        while (from <= value.size()) {
            std::size_t to = value.find('\n', from);
            if (to == std::string::npos) { to = value.size(); }
            std::string line = value.substr(from, to - from);
            if (!line.empty() && line.back() == '\r') { line.pop_back(); }
            if (!line.empty()) {
                std::string canonical;
                if (!cascade::net::parseCorsOrigin(line, true, canonical)) { return "\"" + line + "\" is not an origin (scheme://host[:port], no path)"; }
                if (++count > cascade::net::kMaxCorsOrigins) { return "too many origins"; }
            }
            from = to + 1;
        }
    }
    if (key == "rds.log.name") {
        // the recorder's own file-name rules (core/rec_naming.hpp): a template they refuse is refused here, with their words
        cascade::core::recname::Fields f;
        f.unixMs = 1'790'000'000'000LL;
        f.frequencyHz = 100.0e6;
        f.radio = "1";
        const cascade::core::recname::Expanded e = cascade::core::recname::expand(value.empty() ? std::string("rds_{date}") : value, f);
        if (!e.ok) { return e.error; }
    }
    if (key == "out.mqtt.host" && !value.empty()) { return cascade::engine::s4::checkMqttHost(value); }
    if (key == "out.mqtt.topic") { return cascade::engine::s4::checkMqttTopic(value); }
    if (key == "out.influx.host" && !value.empty()) { return cascade::engine::s4::checkMqttHost(value); }
    if (key == "out.influx.bucket") { return cascade::engine::s4::checkInfluxText(value, false); }
    if (key == "out.influx.org") { return cascade::engine::s4::checkInfluxText(value, true); }
    if (key == "out.influx.token" && value.find_first_of("\r\n") != std::string::npos) { return "the token has a line break"; }
    if (key == "out.webhook.url") { return cascade::engine::s4::checkWebhookUrl(value); }
    if (key == "srv.mcp.token" && value.find_first_of("\r\n\t ") != std::string::npos) { return "the token has a space or a line break"; }
    if (key == "rot.host" && !value.empty()) { return cascade::engine::s4::checkMqttHost(value); }
    if (key == "rot.serial" && !value.empty()) {
        bool com = value.size() >= 4 && value.size() <= 6 && (value.compare(0, 3, "COM") == 0);
        for (std::size_t i = 3; com && i < value.size(); ++i) { com = value[i] >= '0' && value[i] <= '9'; }
        if (!com) { return "the serial port is a name like COM3"; }
    }
    if (key == "swl.operator" && value.find_first_of("\r\n\t") != std::string::npos) { return "the identifier has a control character"; }
    if (key == "swl.grid" && !value.empty()) {
        bool ok = value.size() == 2 || value.size() == 4 || value.size() == 6 || value.size() == 8;
        for (std::size_t i = 0; ok && i < value.size(); ++i) {
            const char ch = value[i];
            const bool letter = (ch >= 'A' && ch <= 'R') || (ch >= 'a' && ch <= 'r') || (i >= 4 && ((ch >= 'A' && ch <= 'X') || (ch >= 'a' && ch <= 'x')));
            const bool digit = ch >= '0' && ch <= '9';
            ok = (i % 4 < 2) ? (i % 2 == 0 ? letter : letter) : digit;
        }
        if (!ok) { return "the square is 2, 4, 6 or 8 characters like IO91 or IO91wm"; }
    }
    if (key == "alerts.keywords") { return cascade::engine::s4::parseKeywords(value).problem; }
    if (key == "rds.log.folder" && !value.empty() && !std::filesystem::path(value).is_absolute()) { return "the folder must be a full path"; }
    return std::string();
}

}  // namespace

bool isSecretKey(const std::string& key) {
    for (const std::string& k : cascade::core::vault::secretKeys()) {
        if (k == key) { return true; }
    }
    return false;
}

const Spec* specs(std::size_t& count) {
    count = kSpecCount;
    return kSpecs;
}

const Spec* findSpec(const std::string& key) {
    for (const Spec& s : kSpecs) {
        if (key == s.key) { return &s; }
    }
    return nullptr;
}

bool normalise(const Spec& s, const std::string& in, std::string& out, SetResult& bad, std::string& why) {
    switch (s.kind) {
        case Kind::Bool:
            if (in == "1" || in == "true" || in == "on" || in == "yes") { out = "1"; return true; }
            if (in == "0" || in == "false" || in == "off" || in == "no") { out = "0"; return true; }
            bad = SetResult::BadValue;
            why = "a switch is 1 or 0";
            return false;
        case Kind::Int:
        case Kind::Real: {
            if (in.empty() || in.size() > 32) { bad = SetResult::BadValue; why = "that is not a number"; return false; }
            char* end = nullptr;
            const double v = std::strtod(in.c_str(), &end);
            if (end == in.c_str() || *end != '\0' || !std::isfinite(v)) { bad = SetResult::BadValue; why = "that is not a number"; return false; }
            if (s.kind == Kind::Int && v != std::floor(v)) { bad = SetResult::BadValue; why = "a whole number is wanted"; return false; }
            if (v < s.lo || v > s.hi) {
                bad = SetResult::OutOfRange;
                why = "the value must be from " + numberText(s.lo) + " to " + numberText(s.hi);
                return false;
            }
            out = numberText(v);
            return true;
        }
        case Kind::Choice: {
            const std::string all = std::string("|") + s.choices + "|";
            if (in.find('|') == std::string::npos && all.find("|" + in + "|") != std::string::npos) { out = in; return true; }
            bad = SetResult::OutOfRange;
            why = std::string("the choices are ") + s.choices;
            return false;
        }
        case Kind::Text:
            if (!validText(in, static_cast<std::size_t>(s.hi))) {
                bad = SetResult::BadValue;
                why = "that text is too long or holds a control character";
                return false;
            }
            out = in;
            return true;
    }
    bad = SetResult::BadValue;
    return false;
}

Settings::Settings(std::filesystem::path path) : path_(std::move(path)) { reload(); }

void Settings::reload() {
    std::lock_guard<std::mutex> lk(m_);
    values_.clear();
    error_.clear();
    std::string text;
    const auto r = cascade::core::safeio::readCapped(path_, 256u * 1024u, text);
    if (r == cascade::core::safeio::ReadResult::Missing) { ++version_; return; }
    if (r != cascade::core::safeio::ReadResult::Ok) {
        error_ = "the wirec settings file cannot be read";
        ++version_;
        return;
    }
    {
        // the secrets come back from the protected store (and a value an older build left in the file is moved there)
        bool migrated = false;
        std::string note;
        text = cascade::core::vault::mergeAfterRead(path_.string(), text, migrated, note);
        if (!note.empty()) { secretNote_ = note; }
    }
    const json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        error_ = "the wirec settings file is damaged; the defaults are in force";
        cascade::core::diagWarnf("wirec: %s", error_.c_str());
        ++version_;
        return;
    }
    for (auto it = j.begin(); it != j.end(); ++it) {
        const Spec* s = findSpec(it.key());
        if (s == nullptr) { continue; }   // a key a newer build wrote: ignored (and dropped at the next save)
        std::string raw = it.value().is_string() ? it.value().get<std::string>()
                          : it.value().is_boolean() ? std::string(it.value().get<bool>() ? "1" : "0")
                          : it.value().is_number() ? numberText(it.value().get<double>()) : std::string();
        std::string norm;
        SetResult bad = SetResult::BadValue;
        std::string why;
        if (normalise(*s, raw, norm, bad, why) && norm != s->def) { values_[it.key()] = norm; }
    }
    ++version_;
}

std::string Settings::get(const std::string& key) const {
    const Spec* s = findSpec(key);
    if (s == nullptr) { return std::string(); }
    std::lock_guard<std::mutex> lk(m_);
    const auto it = values_.find(key);
    return it == values_.end() ? std::string(s->def) : it->second;
}

std::int64_t Settings::getInt(const std::string& key) const { return static_cast<std::int64_t>(std::strtoll(get(key).c_str(), nullptr, 10)); }
double Settings::getReal(const std::string& key) const { return std::strtod(get(key).c_str(), nullptr); }

SetResult Settings::set(const std::string& key, const std::string& value, std::string& why) {
    const Spec* s = findSpec(key);
    if (s == nullptr) { why = "no such setting"; return SetResult::NotFound; }
    std::string norm;
    SetResult bad = SetResult::BadValue;
    if (!normalise(*s, value, norm, bad, why)) { return bad; }
    why = extraRule(key, norm);
    if (!why.empty()) { return SetResult::BadValue; }
    std::lock_guard<std::mutex> lk(m_);
    const auto it = values_.find(key);
    const std::string current = it == values_.end() ? std::string(s->def) : it->second;
    if (current == norm) { why = "already so"; return SetResult::NoChange; }
    const std::map<std::string, std::string> before = values_;
    if (norm == s->def) { values_.erase(key); } else { values_[key] = norm; }
    if (!save()) {
        values_ = before;   // not kept: what a restart brings back is what was reported
        why = "the setting could not be saved";
        return SetResult::SaveFailed;
    }
    ++version_;
    return SetResult::Changed;
}

// (m_ is held by the caller)
bool Settings::save() {
    if (values_.empty()) {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
        cascade::core::vault::forget(path_.string());
        return !ec;
    }
    json j = json::object();
    for (const auto& [k, v] : values_) { j[k] = v; }
    // a secret key leaves the text for the protected store (a cleared one is removed from it)
    for (const std::string& k : cascade::core::vault::secretKeys()) {
        if (findSpec(k) != nullptr && values_.find(k) == values_.end()) { j[k] = ""; }
    }
    std::string out = j.dump(1, ' ', false, json::error_handler_t::replace) + "\n";
    std::string why;
    bool kept = false;
    out = cascade::core::vault::splitForWrite(path_.string(), out, why, kept);
    if (kept) { return false; }   // the protected store would not take the value: nothing is written with it in the clear
    return cascade::core::safeio::writeAtomic(path_, out);
}

void Settings::each(const std::function<void(const std::string&, const std::string&)>& fn) const {
    for (const Spec& s : kSpecs) {
        std::string v = get(s.key);
        if (isSecretKey(s.key)) { v = v.empty() ? std::string() : std::string("set"); }   // a secret is never published: only whether there is one
        fn(s.key, v);
    }
}

}  // namespace cascade::engine::wirec
