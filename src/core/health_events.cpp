// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0

#include "core/health_events.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <system_error>
#include <thread>
#include <tuple>

#include <nlohmann/json.hpp>

#include "core/frame_timing.hpp"
#include "core/i18n.hpp"
#include "core/telemetry.hpp"

namespace cascade::core::health {

namespace {

// WHAT THE PROGRAM RECOVERED FROM: the words and, for each, whether it is counted
// once a session (it can repeat by itself) or once an occurrence. The order is the
// enum's and the written order.
struct RecoveredInfo {
    const char* word;
    bool oncePerSession;
};
constexpr RecoveredInfo kRecovered[kRecoveredCount] = {
    {"audio", true},        // the watchdog retries every second
    {"reopen", true},       // at most once a minute, by itself
    {"srcthread", false},   // each is a leaked thread: how many is the point
    {"vendorcall", false},  // likewise
    {"ringdrop", true},     // a ring that overflows overflows again
    {"dspexc", false},      // the receiver stops; a restart is a new occurrence
    {"cfgsave", true},      // the debounce retries it by itself
    {"enumchild", true},    // every rescan can meet it again
    {"pluginapi", true},    // a plugin that throws once throws every frame
    {"webroute", true},     // a client that polls a failing route
    {"patchload", false},   // start-up, once by nature
    {"sdrenum", true},      // the service stays wedged
    {"sdrlost", true},      // the session stays lost
};

// The frame timer's own names for the scopes, but for `user-wait`.
const std::vector<std::string>& scopeWordList() {
    static const std::vector<std::string> w = [] {
        std::vector<std::string> v;
        for (int i = 0; i < kFrameScopeCount; ++i) {
            if (i != static_cast<int>(FrameScope::UserWait)) { v.push_back(kFrameScopeNames[i]); }
        }
        return v;
    }();
    return w;
}

// The widths of the tiers: 250ms, 1s, 5s.
const std::vector<std::string>& tierWordList() {
    static const std::vector<std::string> w = [] {
        std::vector<std::string> v;
        for (int t = 0; t < kSlowFrameTiers; ++t) {
            const std::int64_t ns = kSlowFrameTierNs[t];
            v.push_back(ns % 1'000'000'000LL == 0 ? std::to_string(ns / 1'000'000'000LL) + "s"
                                                  : std::to_string(ns / 1'000'000LL) + "ms");
        }
        return v;
    }();
    return w;
}

const std::vector<std::string>& recoveredWordList() {
    static const std::vector<std::string> w = [] {
        std::vector<std::string> v;
        for (const RecoveredInfo& r : kRecovered) { v.push_back(r.word); }
        return v;
    }();
    return w;
}

// ---------------------------------------------------------------------------
// The tables. THESE ARE THE VOCABULARY: a word that is not here cannot be made
// into a token, written to the ledger, sent, or stored by the Worker (whose
// copy is checked against these by tests/test_health_events.cpp).
// ---------------------------------------------------------------------------

using Words = std::vector<std::string>;

const Words& driverWords() {
    static const Words w = {"rtlsdr", "hackrf", "airspy", "airspyhf", "sdrplay", "mirisdr",
                            "rx888",  "pluto",  "aor",    "hydrasdr", "rtltcp",   "soapy",
                            "soundcard", "other"};
    return w;
}
const Words& radioReasonWords() {
    static const Words w = {"busy", "driver", "bind", "absent", "timeout", "vendor", "rate", "other"};
    return w;
}
const Words& apiWords() {
    static const Words w = {"mme", "dsound", "wasapi", "wdmks", "asio", "alsa",
                            "jack", "oss",   "coreaudio", "none", "other"};
    return w;
}
const Words& soundReasonWords() {
    static const Words w = {"nodevice", "busy", "format", "host", "other"};
    return w;
}
const Words& installWords() {
    static const Words w = {"net", "hash", "write", "other"};
    return w;
}
const Words& loadWords() {
    static const Words w = {"abi", "retired", "load"};
    return w;
}

}  // namespace

const std::vector<VocabularyEntry>& vocabulary() {
    // The written order of tokens in a record is this order, then the order of
    // each qualifier list.
    static const std::vector<VocabularyEntry> v = {
        {"scan_none", {}, true},
        {"radio_open", {driverWords()}, false},
        {"radio_data", {driverWords()}, false},
        {"radio_fail", {driverWords(), radioReasonWords()}, false},
        {"sound_ok", {apiWords()}, true},
        {"sound_fail", {apiWords(), soundReasonWords()}, true},
        {"upd_check", {}, false},
        {"upd_dl", {}, false},
        {"upd_verify", {}, false},
        {"upd_run", {}, false},
        {"plug_cat", {}, false},
        {"plug_inst", {installWords()}, false},
        {"plug_load", {loadWords()}, true},
        {"rec_fail", {}, false},
        // 0.99.65: LAST in the written order, so a record an older
        // Worker reads has every earlier token where it always was.
        {"slow", {scopeWordList(), tierWordList()}, false},
        {"recovered", {recoveredWordList()}, false},
    };
    return v;
}

namespace {

// A token taken apart: where its event and each qualifier sit in the tables.
enum class Family : std::uint8_t { Base, Slow, Recovered };

struct Parsed {
    int event = -1;
    int q1 = -1;
    int q2 = -1;
    bool once = false;
    bool radioFail = false;
    Family family = Family::Base;
};

int indexOf(const Words& w, std::string_view s) {
    for (std::size_t i = 0; i < w.size(); ++i) {
        if (w[i] == s) { return static_cast<int>(i); }
    }
    return -1;
}

bool parseToken(std::string_view token, Parsed& out) {
    out = Parsed{};
    if (token.empty() || token.size() > 40) { return false; }
    // name[.q1[.q2]]
    std::string_view parts[3];
    std::size_t n = 0;
    std::size_t at = 0;
    while (true) {
        const std::size_t dot = token.find('.', at);
        if (n >= 3) { return false; }
        parts[n++] = token.substr(at, dot == std::string_view::npos ? std::string_view::npos : dot - at);
        if (dot == std::string_view::npos) { break; }
        at = dot + 1;
    }
    const std::vector<VocabularyEntry>& v = vocabulary();
    for (std::size_t e = 0; e < v.size(); ++e) {
        if (v[e].name != parts[0]) { continue; }
        if (v[e].qualifiers.size() + 1 != n) { return false; }
        out.event = static_cast<int>(e);
        out.once = v[e].oncePerSession;
        out.radioFail = v[e].name == "radio_fail";
        out.family = v[e].name == "slow"        ? Family::Slow
                     : v[e].name == "recovered" ? Family::Recovered
                                                : Family::Base;
        if (n >= 2) {
            out.q1 = indexOf(v[e].qualifiers[0], parts[1]);
            if (out.q1 < 0) { return false; }
        }
        if (n >= 3) {
            out.q2 = indexOf(v[e].qualifiers[1], parts[2]);
            if (out.q2 < 0) { return false; }
        }
        // Whether a recovery is once a session is the word's, not the family's.
        if (out.family == Family::Recovered) {
            out.once = kRecovered[static_cast<std::size_t>(out.q1)].oncePerSession;
        }
        return true;
    }
    return false;
}

std::tuple<int, int, int> orderKey(const Parsed& p) { return {p.event, p.q1, p.q2}; }

std::string build(const char* name, const std::string& q1 = std::string(),
                  const std::string& q2 = std::string()) {
    std::string s = name;
    if (!q1.empty()) { s += "." + q1; }
    if (!q2.empty()) { s += "." + q2; }
    return s;
}

std::string lowerCopy(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }
    }
    return out;
}

std::filesystem::path utf8Path(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

}  // namespace

std::vector<std::string> allTokens() {
    std::vector<std::string> out;
    for (const VocabularyEntry& e : vocabulary()) {
        if (e.qualifiers.empty()) {
            out.push_back(e.name);
        } else if (e.qualifiers.size() == 1) {
            for (const std::string& a : e.qualifiers[0]) { out.push_back(e.name + "." + a); }
        } else {
            for (const std::string& a : e.qualifiers[0]) {
                for (const std::string& b : e.qualifiers[1]) { out.push_back(e.name + "." + a + "." + b); }
            }
        }
    }
    return out;
}

bool validToken(std::string_view token) {
    Parsed p;
    return parseToken(token, p);
}

// ---------------------------------------------------------------------------
// Reduction to the vocabulary
// ---------------------------------------------------------------------------

std::string driverWord(std::string_view driverKind) {
    const std::string k = lowerCopy(driverKind);
    return indexOf(driverWords(), k) >= 0 ? k : std::string("other");
}

AudioApi audioApiFromName(std::string_view hostApiName) {
    const std::string n = lowerCopy(hostApiName);
    auto has = [&n](const char* s) { return n.find(s) != std::string::npos; };
    if (n.empty()) { return AudioApi::None; }
    if (has("wasapi")) { return AudioApi::Wasapi; }
    if (has("directsound")) { return AudioApi::DirectSound; }
    if (has("wdm")) { return AudioApi::Wdmks; }
    if (has("asio")) { return AudioApi::Asio; }
    if (has("mme")) { return AudioApi::Mme; }
    if (has("alsa")) { return AudioApi::Alsa; }
    if (has("jack")) { return AudioApi::Jack; }
    if (has("oss")) { return AudioApi::Oss; }
    if (has("core audio") || has("coreaudio")) { return AudioApi::CoreAudio; }
    return AudioApi::Other;
}

const char* audioApiWord(AudioApi api) {
    switch (api) {
        case AudioApi::Mme: return "mme";
        case AudioApi::DirectSound: return "dsound";
        case AudioApi::Wasapi: return "wasapi";
        case AudioApi::Wdmks: return "wdmks";
        case AudioApi::Asio: return "asio";
        case AudioApi::Alsa: return "alsa";
        case AudioApi::Jack: return "jack";
        case AudioApi::Oss: return "oss";
        case AudioApi::CoreAudio: return "coreaudio";
        case AudioApi::None: return "none";
        case AudioApi::Other: break;
    }
    return "other";
}

const char* radioReasonWord(RadioReason r) {
    switch (r) {
        case RadioReason::Busy: return "busy";
        case RadioReason::Driver: return "driver";
        case RadioReason::Bind: return "bind";
        case RadioReason::Absent: return "absent";
        case RadioReason::Timeout: return "timeout";
        case RadioReason::Vendor: return "vendor";
        case RadioReason::Rate: return "rate";
        case RadioReason::Other: break;
    }
    return "other";
}

const char* soundReasonWord(SoundReason r) {
    switch (r) {
        case SoundReason::NoDevice: return "nodevice";
        case SoundReason::Busy: return "busy";
        case SoundReason::Format: return "format";
        case SoundReason::Host: return "host";
        case SoundReason::Other: break;
    }
    return "other";
}

namespace {

// THE SENTENCES THE DRIVERS TRANSLATE. Most driver errors are English text, but
// a few go through tr() (the SDRplay API advice, AOR's cable and driver
// advice), and a Spanish or Japanese window would hand the classifier those in
// Spanish or Japanese. Each is matched in the language in force as well as in
// English: the error was produced in this process, in this language. The keys
// are the drivers' own strings, held to them by tests/test_health_events.cpp
// (each must be a key in the translation catalogues).
struct TranslatedSentence {
    const char* english;
    RadioReason why;
};
const TranslatedSentence kTranslated[] = {
    {"SDRplay radios need the SDRplay API from sdrplay.com, version 3.x - install it and "
     "restart FoxSDR.",
     RadioReason::Driver},
    {"The AOR receiver's I/Q interface (USB 08D0:A001) is plugged in but is not bound to "
     "WinUSB - AOR's own driver (AorAlpha) and WinUSB cannot both own it. Run Zadig, select the "
     "AOR I/Q interface, choose WinUSB and click Replace Driver, then try again.",
     RadioReason::Bind},
    {"No AOR I/Q interface (USB 08D0:A001) was found. Check that the receiver's I/Q USB cable "
     "is connected and the receiver is switched on.",
     RadioReason::Absent},
};

}  // namespace

RadioReason classifyRadioOpen(std::string_view driverKind, std::string_view error) {
    for (const TranslatedSentence& t : kTranslated) {
        if (error.find(t.english) != std::string_view::npos) { return t.why; }
        const std::string local = cascade::i18n::tr(t.english);
        if (!local.empty() && error.find(local) != std::string_view::npos) { return t.why; }
    }
    const std::string e = lowerCopy(error);
    auto has = [&e](const char* s) { return e.find(s) != std::string::npos; };

    // THE ORDER IS THE PRECEDENCE, and it is deliberate: the more specific the
    // remedy a class implies, the earlier it is tried.
    //
    // A service or API that is missing or not running - the SDRplay service's own
    // sentence, a build with no USB support.
    if (has("service") || has("not installed") || has("could not be loaded") ||
        has("failed to load") || has("cannot load") || has("support needs") ||
        has("no module") || has("no such driver") || has("api is not")) {
        return RadioReason::Driver;
    }
    if (has("timed out") || has("timeout") || has("did not answer") || has("not answering") ||
        has("no answer") || has("did not respond") || has("never answered")) {
        return RadioReason::Timeout;
    }
    // Held by somebody else: the transports' shared sentence (usb_device.hpp
    // kInUseError), the RSPduo's, libusb's "claim interface", the sound APIs'.
    if (has("in use") || has("busy") || has("claim") || has("another program") ||
        has("another application") || has("sharing violation") || has("exclusive")) {
        return RadioReason::Busy;
    }
    // Present but not reachable: not bound to WinUSB (Zadig), no permission on
    // the device node (udev).
    if (has("winusb") || has("usbfs") || has("permission") || has("access denied") ||
        has("zadig") || has("udev") || has("not authorized")) {
        return RadioReason::Bind;
    }
    if (has("sample rate") || has("samplerate")) { return RadioReason::Rate; }
    // Not there (any more): unplugged since the scan, the serial is not on the
    // bus, nothing at the Pluto's address.
    if (has("not connected") || has("not present") || has("no longer present") ||
        has("is present") || has("there is no") || has("no sdrplay device") ||
        has("nothing at") || has("no match") || has("not found") || has("no such") ||
        has("not in the list") || has("was not found") || has("not plugged")) {
        return RadioReason::Absent;
    }
    // What a vendor stack said that nothing above recognised: SoapySDR's own
    // exceptions, the SDRplay API's error codes, libusb's.
    const std::string kind = lowerCopy(driverKind);
    if (kind == "soapy" || has("libusb") || has("failed:") || has("uhd") || has("vendor")) {
        return RadioReason::Vendor;
    }
    return RadioReason::Other;
}

std::string tokenScanNone() { return "scan_none"; }
std::string tokenRadioOpen(std::string_view k) { return build("radio_open", driverWord(k)); }
std::string tokenRadioData(std::string_view k) { return build("radio_data", driverWord(k)); }
std::string tokenRadioFail(std::string_view k, RadioReason why) {
    return build("radio_fail", driverWord(k), radioReasonWord(why));
}
std::string tokenSoundOk(AudioApi api) { return build("sound_ok", audioApiWord(api)); }
std::string tokenSoundFail(AudioApi api, SoundReason why) {
    return build("sound_fail", audioApiWord(api), soundReasonWord(why));
}
std::string tokenUpdateCheck() { return "upd_check"; }
std::string tokenUpdateDownload() { return "upd_dl"; }
std::string tokenUpdateVerify() { return "upd_verify"; }
std::string tokenUpdateRun() { return "upd_run"; }
std::string tokenPluginCatalogue() { return "plug_cat"; }
std::string tokenPluginInstall(InstallClass c) {
    return build("plug_inst", installWords()[static_cast<std::size_t>(c)]);
}
std::string tokenPluginLoad(LoadClass c) {
    return build("plug_load", loadWords()[static_cast<std::size_t>(c)]);
}
std::string tokenRecordFail() { return "rec_fail"; }

// ---------------------------------------------------------------------------
// Counts and their encoding
// ---------------------------------------------------------------------------

namespace {

bool isSlowOrRecovered(const std::string& token) {
    return token.rfind("slow.", 0) == 0 || token.rfind("recovered.", 0) == 0;
}

// Adds `n` to `token`'s count in `into`, keeping every bound OF A LEDGER: a new
// failure token is refused once kMaxBaseTokens failure tokens are held, or once
// kMaxRadioFailTokens of them are radio failures; a count stops at kMaxCount.
// `slow` and `recovered` tokens are never refused for how many are held - there
// are only 54 and 13 of them, the vocabulary bounds them - because the cap that
// applies to them is the RECORD's (selectForRecord keeps the worst), and a ledger
// that dropped the rest on arrival could not know which were the worst.
// True when anything was added.
bool addCapped(Counts& into, const std::string& token, std::uint64_t n) {
    Parsed p;
    if (n == 0 || !parseToken(token, p)) { return false; }
    auto it = into.find(token);
    if (it == into.end()) {
        if (p.family == Family::Base) {
            std::size_t base = 0;
            std::size_t fails = 0;
            for (const auto& kv : into) {
                if (isSlowOrRecovered(kv.first)) { continue; }
                ++base;
                if (kv.first.rfind("radio_fail.", 0) == 0) { ++fails; }
            }
            if (base >= kMaxBaseTokens) { return false; }
            if (p.radioFail && fails >= kMaxRadioFailTokens) { return false; }
        }
        it = into.emplace(token, 0u).first;
    }
    const std::uint64_t sum = static_cast<std::uint64_t>(it->second) + n;
    it->second = static_cast<std::uint32_t>(std::min<std::uint64_t>(sum, kMaxCount));
    return true;
}

std::vector<std::pair<std::string, std::uint32_t>> inWrittenOrder(const Counts& counts) {
    std::vector<std::pair<std::tuple<int, int, int>, std::pair<std::string, std::uint32_t>>> keyed;
    for (const auto& kv : counts) {
        Parsed p;
        if (kv.second == 0 || !parseToken(kv.first, p)) { continue; }
        keyed.push_back({orderKey(p), {kv.first, std::min(kv.second, kMaxCount)}});
    }
    std::sort(keyed.begin(), keyed.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<std::pair<std::string, std::uint32_t>> out;
    for (auto& k : keyed) { out.push_back(std::move(k.second)); }
    return out;
}

}  // namespace

Counts selectForRecord(const Counts& counts) {
    struct Item {
        std::string token;
        std::uint32_t n;
        Parsed p;
    };
    std::vector<Item> base, slow, recovered;
    for (const auto& kv : counts) {
        Parsed p;
        if (kv.second == 0 || !parseToken(kv.first, p)) { continue; }
        Item it{kv.first, std::min(kv.second, kMaxCount), p};
        (p.family == Family::Slow ? slow : p.family == Family::Recovered ? recovered : base)
            .push_back(std::move(it));
    }
    auto writtenFirst = [](const Item& a, const Item& b) { return orderKey(a.p) < orderKey(b.p); };

    // The failure families: the earlier in the written order, within their caps.
    std::sort(base.begin(), base.end(), writtenFirst);
    std::vector<Item> chosen;
    std::size_t fails = 0;
    for (Item& it : base) {
        if (chosen.size() >= kMaxBaseTokens) { break; }
        if (it.p.radioFail) {
            if (fails >= kMaxRadioFailTokens) { continue; }
            ++fails;
        }
        chosen.push_back(std::move(it));
    }
    // `slow`, over its cap: the worst - the higher tier, then the higher count, then
    // the earlier in the written order.
    if (slow.size() > kMaxSlowTokens) {
        std::sort(slow.begin(), slow.end(), [&](const Item& a, const Item& b) {
            if (a.p.q2 != b.p.q2) { return a.p.q2 > b.p.q2; }
            if (a.n != b.n) { return a.n > b.n; }
            return writtenFirst(a, b);
        });
        slow.resize(kMaxSlowTokens);
    }
    // `recovered`, over its cap: the higher count, then the earlier in the written order.
    if (recovered.size() > kMaxRecoveredTokens) {
        std::sort(recovered.begin(), recovered.end(), [&](const Item& a, const Item& b) {
            if (a.n != b.n) { return a.n > b.n; }
            return writtenFirst(a, b);
        });
        recovered.resize(kMaxRecoveredTokens);
    }
    for (Item& it : slow) { chosen.push_back(std::move(it)); }
    for (Item& it : recovered) { chosen.push_back(std::move(it)); }
    std::sort(chosen.begin(), chosen.end(), writtenFirst);

    // The text: tokens are added in the written order until the next would not fit.
    Counts out;
    std::size_t bytes = 0;
    for (const Item& it : chosen) {
        const std::size_t piece = it.token.size() + 1 + std::to_string(it.n).size();
        const std::size_t add = piece + (out.empty() ? 0 : 1);
        if (bytes + add > kMaxEncodedBytes) { break; }
        bytes += add;
        out[it.token] = it.n;
    }
    return out;
}

std::string encode(const Counts& counts) {
    std::string out;
    for (const auto& kv : inWrittenOrder(selectForRecord(counts))) {
        if (!out.empty()) { out += ','; }
        out += kv.first + "=" + std::to_string(kv.second);
    }
    return out;
}

bool decode(std::string_view text, Counts& out) {
    out.clear();
    if (text.empty()) { return true; }
    if (text.size() > kMaxEncodedBytes) { return false; }
    std::size_t at = 0;
    // Each family has its own cap: the failures, `slow`, `recovered`.
    std::size_t pairs[3] = {0, 0, 0};
    const std::size_t caps[3] = {kMaxBaseTokens, kMaxSlowTokens, kMaxRecoveredTokens};
    while (at <= text.size()) {
        const std::size_t comma = text.find(',', at);
        const std::string_view piece =
            text.substr(at, comma == std::string_view::npos ? std::string_view::npos : comma - at);
        const std::size_t eq = piece.find('=');
        if (eq == std::string_view::npos) { out.clear(); return false; }
        const std::string token(piece.substr(0, eq));
        const std::string_view digits = piece.substr(eq + 1);
        Parsed p;
        if (!parseToken(token, p) || digits.empty() || digits.size() > 3) { out.clear(); return false; }
        std::uint32_t n = 0;
        for (char c : digits) {
            if (c < '0' || c > '9') { out.clear(); return false; }
            n = n * 10 + static_cast<std::uint32_t>(c - '0');
        }
        const std::size_t f = static_cast<std::size_t>(p.family);
        if (n < 1 || n > kMaxCount || out.count(token) != 0 || ++pairs[f] > caps[f]) {
            out.clear();
            return false;
        }
        out[token] = n;
        if (comma == std::string_view::npos) { break; }
        at = comma + 1;
    }
    return true;
}

std::string sanitise(std::string_view text) {
    Counts kept;
    std::size_t at = 0;
    // Bounded input: a megabyte of rubbish is not walked.
    if (text.size() > 4096) { text = text.substr(0, 4096); }
    while (at <= text.size()) {
        const std::size_t comma = text.find(',', at);
        const std::string_view piece =
            text.substr(at, comma == std::string_view::npos ? std::string_view::npos : comma - at);
        const std::size_t eq = piece.find('=');
        if (eq != std::string_view::npos) {
            const std::string token(piece.substr(0, eq));
            const std::string_view digits = piece.substr(eq + 1);
            if (!digits.empty() && digits.size() <= 4 &&
                std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) {
                std::uint64_t n = 0;
                for (char c : digits) { n = n * 10 + static_cast<std::uint64_t>(c - '0'); }
                addCapped(kept, token, n);
            }
        }
        if (comma == std::string_view::npos) { break; }
        at = comma + 1;
    }
    return encode(kept);
}

std::uint64_t sumOf(const Counts& counts, std::string_view eventName) {
    std::uint64_t total = 0;
    for (const auto& kv : counts) {
        const std::string_view t = kv.first;
        if (t == eventName ||
            (t.size() > eventName.size() && t.compare(0, eventName.size(), eventName) == 0 &&
             t[eventName.size()] == '.')) {
            total += kv.second;
        }
    }
    return total;
}

// ---------------------------------------------------------------------------
// The ledger
// ---------------------------------------------------------------------------

struct HealthLedger::Shared {
    mutable std::mutex m;                 // guards everything below but `io`
    mutable std::condition_variable cv;   // the writer going idle
    std::mutex io;                        // serialises writes and removals of the file
    std::string path;
    std::string installId;
    Counts counts;                        // earlier sessions' plus this one's
    Counts prior;                         // what the file held at arm(): earlier sessions only
    bool decided = false;
    bool armed = false;
    bool allowed = true;
    bool dirty = false;
    bool writing = false;
    std::uint64_t generation = 0;         // moves when the target changes
    std::set<std::string> once;           // events already counted this run
};

namespace {

using SharedPtr = std::shared_ptr<HealthLedger::Shared>;

}  // namespace

std::string HealthLedger::fileText(const std::string& installId, const Counts& counts) {
    std::string text = installId + "\n";
    for (const auto& kv : inWrittenOrder(counts)) {
        text += kv.first + "=" + std::to_string(kv.second) + "\n";
    }
    return text;
}

bool HealthLedger::parseFileText(const std::string& text, const std::string& installId,
                                 Counts& out) {
    out.clear();
    if (!validInstallId(installId)) { return false; }
    std::size_t at = 0;
    bool first = true;
    while (at < text.size()) {
        std::size_t nl = text.find('\n', at);
        if (nl == std::string::npos) { nl = text.size(); }
        std::string line = text.substr(at, nl - at);
        while (!line.empty() && line.back() == '\r') { line.pop_back(); }
        at = nl + 1;
        if (first) {
            if (line != installId) { out.clear(); return false; }
            first = false;
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) { continue; }
        const std::string token = line.substr(0, eq);
        const std::string digits = line.substr(eq + 1);
        if (digits.empty() || digits.size() > 9 ||
            !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            continue;
        }
        std::uint64_t n = 0;
        for (char c : digits) { n = n * 10 + static_cast<std::uint64_t>(c - '0'); }
        addCapped(out, token, std::min<std::uint64_t>(n, kMaxCount));
    }
    return !first;
}

std::string HealthLedger::pathIn(const std::string& configDir) {
    if (configDir.empty()) { return std::string(); }
    return configDir + "/" + kFileName;
}

void HealthLedger::removeFile(const std::string& path) {
    if (path.empty()) { return; }
    std::error_code ec;
    std::filesystem::remove(utf8Path(path), ec);
}

namespace {

// Writes `text` over `path` through a temporary file and a rename, so a process
// ended mid-write leaves the old file or the new one and never half of one.
void writeAtomically(const std::string& path, const std::string& text) {
    const std::filesystem::path file = utf8Path(path);
    std::filesystem::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) { return; }
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out) {
            out.close();
            std::error_code ec;
            std::filesystem::remove(tmp, ec);
            return;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, file, ec);
    if (ec) { std::filesystem::remove(tmp, ec); }
}

}  // namespace

static std::atomic<void (*)()> g_writeHook{nullptr};

// The writer: one short-lived thread at a time, started by the first change and
// ending when nothing is left to write. It owns a share of the state, so the
// ledger may be gone before it finishes, and the caller never waits on a disk.
static void healthWriterLoop(SharedPtr s) {
    for (;;) {
        std::uint64_t gen = 0;
        std::string path;
        {
            std::lock_guard<std::mutex> lk(s->m);
            if (!s->dirty || !s->armed || s->path.empty()) {
                s->dirty = false;
                s->writing = false;
                s->cv.notify_all();
                return;
            }
            s->dirty = false;
            gen = s->generation;
            path = s->path;
        }
        std::lock_guard<std::mutex> io(s->io);
        std::string text;
        bool remove = false;
        {
            std::lock_guard<std::mutex> lk(s->m);
            // Switched off or retargeted while this waited: the new state has
            // scheduled its own write (or removal), and writing now could bring
            // back a file that was just deleted.
            if (!s->armed || s->generation != gen) { continue; }
            if (s->counts.empty()) {
                remove = true;
            } else {
                text = HealthLedger::fileText(s->installId, s->counts);
            }
        }
        // A test's slow disk: runs on THIS thread, with the ledger's lock released,
        // so it can only ever delay the file and never a caller of note().
        if (void (*hook)() = g_writeHook.load()) { hook(); }
        if (remove) {
            std::error_code ec;
            std::filesystem::remove(utf8Path(path), ec);
        } else {
            writeAtomically(path, text);
        }
    }
}

// Marks the state changed and makes sure a writer is running. `m` is held.
static void scheduleWriteLocked(const SharedPtr& s) {
    s->dirty = true;
    if (s->writing || !s->armed || s->path.empty()) { return; }
    s->writing = true;
    try {
        std::thread([s]() { healthWriterLoop(s); }).detach();
    } catch (...) {
        s->writing = false;  // no thread to be had: the next change tries again
    }
}

HealthLedger::HealthLedger() : s_(std::make_shared<Shared>()) {}

HealthLedger::~HealthLedger() = default;

void HealthLedger::arm(const std::string& path, const std::string& installId, bool loadExisting) {
    if (!validInstallId(installId)) {
        disarm();
        return;
    }
    Counts loaded;
    if (loadExisting && !path.empty()) {
        std::ifstream in(utf8Path(path), std::ios::binary);
        if (in) {
            // Bounded: 24 tokens of 40 bytes and an id is under 1.1 KB.
            char buf[4096];
            in.read(buf, sizeof buf);
            HealthLedger::parseFileText(std::string(buf, static_cast<std::size_t>(in.gcount())),
                                        installId, loaded);
        }
    }
    std::lock_guard<std::mutex> lk(s_->m);
    Counts mine;
    if (!s_->decided && s_->allowed) { mine = std::move(s_->counts); }
    s_->counts = loaded;
    s_->prior = loaded;
    bool added = false;
    for (const auto& kv : mine) { added = addCapped(s_->counts, kv.first, kv.second) || added; }
    s_->path = path;
    s_->installId = installId;
    s_->armed = true;
    s_->decided = true;
    ++s_->generation;
    if (added) { scheduleWriteLocked(s_); }
}

void HealthLedger::disarm() {
    std::string old;
    {
        std::lock_guard<std::mutex> lk(s_->m);
        old = s_->path;
        s_->counts.clear();
        s_->prior.clear();
        s_->once.clear();
        s_->path.clear();
        s_->installId.clear();
        s_->armed = false;
        s_->decided = true;
        s_->dirty = false;
        ++s_->generation;
    }
    if (!old.empty()) {
        // Off the caller's thread, for the reason the writer is: this is the
        // Settings switch on the GUI thread.
        SharedPtr s = s_;
        try {
            std::thread([s, old]() {
                std::lock_guard<std::mutex> io(s->io);
                removeFile(old);
            }).detach();
        } catch (...) {
            removeFile(old);
        }
    }
}

void HealthLedger::setAllowed(bool allowed) {
    std::lock_guard<std::mutex> lk(s_->m);
    s_->allowed = allowed;
    // Not yet decided and now not allowed: what was held is dropped, not kept
    // for a decision that would refuse it.
    if (!allowed && !s_->decided) { s_->counts.clear(); }
}

bool HealthLedger::decided() const {
    std::lock_guard<std::mutex> lk(s_->m);
    return s_->decided;
}

bool HealthLedger::armed() const {
    std::lock_guard<std::mutex> lk(s_->m);
    return s_->armed;
}

void HealthLedger::note(const std::string& token) { note(token, 1); }

void HealthLedger::note(const std::string& token, std::uint32_t times) {
    Parsed p;
    if (times == 0 || !parseToken(token, p)) { return; }
    std::lock_guard<std::mutex> lk(s_->m);
    if (s_->decided && (!s_->armed || !s_->allowed)) { return; }
    if (!s_->decided && !s_->allowed) { return; }
    if (p.once) {
        if (!s_->once.insert(token).second) { return; }
        times = 1;
    }
    if (addCapped(s_->counts, token, times) && s_->armed) { scheduleWriteLocked(s_); }
}

Counts HealthLedger::counts() const {
    std::lock_guard<std::mutex> lk(s_->m);
    return s_->counts;
}

Counts HealthLedger::priorCounts() const {
    std::lock_guard<std::mutex> lk(s_->m);
    return s_->armed ? s_->prior : Counts();
}

std::string HealthLedger::encoded() const {
    std::lock_guard<std::mutex> lk(s_->m);
    return s_->armed ? encode(s_->counts) : std::string();
}

void HealthLedger::settle(const Counts& carried) {
    if (carried.empty()) { return; }
    std::lock_guard<std::mutex> lk(s_->m);
    if (!s_->armed) { return; }
    auto subtract = [](Counts& c, const std::string& token, std::uint32_t n) {
        auto it = c.find(token);
        if (it == c.end()) { return; }
        if (it->second <= n) { c.erase(it); } else { it->second -= n; }
    };
    for (const auto& kv : carried) {
        subtract(s_->counts, kv.first, kv.second);
        subtract(s_->prior, kv.first, kv.second);
    }
    scheduleWriteLocked(s_);
}

bool HealthLedger::flush(unsigned timeoutMs) const {
    std::unique_lock<std::mutex> lk(s_->m);
    return s_->cv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                           [this] { return !s_->dirty && !s_->writing; });
}

void HealthLedger::reset() {
    std::lock_guard<std::mutex> lk(s_->m);
    s_->counts.clear();
    s_->prior.clear();
    s_->once.clear();
    s_->path.clear();
    s_->installId.clear();
    s_->armed = false;
    s_->decided = false;
    s_->allowed = true;
    s_->dirty = false;
    ++s_->generation;
}

std::shared_ptr<HealthLedger> globalLedger() {
    // Never destroyed: see the header.
    static std::shared_ptr<HealthLedger>* const g =
        new std::shared_ptr<HealthLedger>(std::make_shared<HealthLedger>());
    return *g;
}

// ---------------------------------------------------------------------------
// Call-site helpers
// ---------------------------------------------------------------------------

void note(const std::string& token) { globalLedger()->note(token); }
void noteScanNone() { note(tokenScanNone()); }
void noteRadioOpen(std::string_view k) { note(tokenRadioOpen(k)); }
void noteRadioData(std::string_view k) { note(tokenRadioData(k)); }
void noteRadioFail(std::string_view k, std::string_view error) {
    note(tokenRadioFail(k, classifyRadioOpen(k, error)));
}
void noteSoundOk(std::string_view hostApiName) {
    note(tokenSoundOk(audioApiFromName(hostApiName)));
}
void noteSoundFail(AudioApi api, SoundReason why) { note(tokenSoundFail(api, why)); }
void noteUpdateCheckFailed() { note(tokenUpdateCheck()); }
void noteUpdateDownloadFailed() { note(tokenUpdateDownload()); }
void noteUpdateVerifyFailed() { note(tokenUpdateVerify()); }
void noteUpdateRunFailed() { note(tokenUpdateRun()); }
void notePluginCatalogueFailed() { note(tokenPluginCatalogue()); }
void notePluginInstallFailed(InstallClass c) { note(tokenPluginInstall(c)); }
void notePluginLoadRefused(LoadClass c) { note(tokenPluginLoad(c)); }
void noteRecordFailed() { note(tokenRecordFail()); }

// ---------------------------------------------------------------------------
// Slow frames, and what the program recovered from (0.99.65)
// ---------------------------------------------------------------------------

const std::vector<std::string>& slowScopeWords() { return scopeWordList(); }
const std::vector<std::string>& slowTierWords() { return tierWordList(); }
const std::vector<std::string>& recoveredWords() { return recoveredWordList(); }

std::string tokenSlowFrame(int frameScope, int tier) {
    if (frameScope < 0 || frameScope >= kFrameScopeCount ||
        frameScope == static_cast<int>(FrameScope::UserWait) || tier < 0 || tier >= kSlowFrameTiers) {
        return std::string();
    }
    return build("slow", kFrameScopeNames[frameScope], tierWordList()[static_cast<std::size_t>(tier)]);
}

const char* recoveredWord(Recovered r) {
    const std::size_t i = static_cast<std::size_t>(r);
    return i < kRecoveredCount ? kRecovered[i].word : "";
}
std::string tokenRecovered(Recovered r) {
    const std::string w = recoveredWord(r);
    return w.empty() ? std::string() : build("recovered", w);
}
bool recoveredOncePerSession(Recovered r) {
    const std::size_t i = static_cast<std::size_t>(r);
    return i < kRecoveredCount && kRecovered[i].oncePerSession;
}

void HealthLedger::setWriteHookForTest(void (*hook)()) { g_writeHook.store(hook); }

void noteSlowFrame(int frameScope, int tier) {
    const std::string token = tokenSlowFrame(frameScope, tier);
    if (!token.empty()) { note(token); }
}

void noteRecovered(Recovered r) {
    const std::string token = tokenRecovered(r);
    if (!token.empty()) { note(token); }
}

namespace {
// The words raised and not yet counted: one bit each. Lock-free, so a thread that
// must not take a lock can still say "this happened".
std::atomic<std::uint32_t> g_raisedRecovered{0};
}  // namespace

void raiseRecovered(Recovered r) {
    const std::size_t i = static_cast<std::size_t>(r);
    if (i < kRecoveredCount) { g_raisedRecovered.fetch_or(1u << i, std::memory_order_relaxed); }
}

void drainRecovered() {
    const std::uint32_t bits = g_raisedRecovered.exchange(0u, std::memory_order_relaxed);
    if (bits == 0) { return; }
    for (std::size_t i = 0; i < kRecoveredCount; ++i) {
        if ((bits & (1u << i)) != 0) { noteRecovered(static_cast<Recovered>(i)); }
    }
}

void RecoveryWatch::poll(const RecoveryReadings& now) {
    if (!started_) {
        // The first look is the baseline: what the process had before it is not news.
        started_ = true;
        last_ = now;
        return;
    }
    auto moved = [](unsigned long long a, unsigned long long b) {
        return a > b ? static_cast<std::uint32_t>(std::min<unsigned long long>(a - b, kMaxCount)) : 0u;
    };
    auto g = globalLedger();
    if (const std::uint32_t n = moved(now.sourceThreadsAbandoned, last_.sourceThreadsAbandoned)) {
        g->note(tokenRecovered(Recovered::SrcThread), n);
    }
    if (const std::uint32_t n = moved(now.vendorCallsAbandoned, last_.vendorCallsAbandoned)) {
        g->note(tokenRecovered(Recovered::VendorCall), n);
    }
    if (const std::uint32_t n = moved(now.ringDroppedSamples, last_.ringDroppedSamples)) {
        g->note(tokenRecovered(Recovered::RingDrop), n);
    }
    if (const std::uint32_t n = moved(now.dspThreadExceptions, last_.dspThreadExceptions)) {
        g->note(tokenRecovered(Recovered::DspExc), n);
    }
    last_ = now;
}

std::string withHealth(const std::string& recordJson, const std::string& encoded) {
    nlohmann::json j = nlohmann::json::parse(recordJson, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) { return recordJson; }
    j["health"] = sanitise(encoded);
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

}  // namespace cascade::core::health
