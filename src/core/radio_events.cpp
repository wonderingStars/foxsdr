// radio_events.cpp - see radio_events.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/radio_events.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <map>
#include <mutex>
#include <vector>

#include "core/diag_log.hpp"
#include "core/health_events.hpp"
#include "core/radio_reporter.hpp"

namespace cascade::core::radioreport {

namespace health = cascade::core::health;

namespace {

struct Registered {
    RadioIdentity radio;
    std::chrono::steady_clock::time_point openedAt;
};

struct Throttle {
    std::chrono::steady_clock::time_point last{};
    bool seen = false;
};

struct Deferred {
    const void* source = nullptr;
    Event ev;
    std::chrono::steady_clock::time_point at;
};

struct Registry {
    std::mutex mu;
    std::vector<Deferred> deferred;               // events a driver raised while its radio was still opening: said as soon as the engine says which radio it is
    std::map<std::string, Throttle> throttle;    // radio key | word -> when it last went out
    std::map<const void*, Registered> bySource;
    std::map<std::string, bool> closeReported;   // radio key -> a driver already said this radio's ending
};

Registry& registry() {
    static Registry r;
    return r;
}

thread_local const char* t_why = "user";
thread_local std::uint64_t t_refusals = 0;   // noteControlRefused calls made on this thread (RefusalWatch)

std::string lower(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

bool contains(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

// THE RADIO a call is about: the registered source object, else the one registered radio of the driver. Nothing else: no radio is made up.
struct Who {
    RadioIdentity radio;
    long long openSec = 0;
    bool registered = false;
};

Who whoIs(const void* source, const char* driverKey) {
    Who w;
    const std::string driver = driverKey != nullptr ? driverKey : "";
    const auto now = std::chrono::steady_clock::now();
    {
        Registry& r = registry();
        std::lock_guard<std::mutex> lk(r.mu);
        auto it = r.bySource.find(source);
        if (it == r.bySource.end()) {
            // A source the engine has not announced is a radio still OPENING (its events are held until it is); only a caller with no source object at all (a service, a
            // helper class with no back pointer) is taken to mean the one open radio of that driver.
            const Registered* only = nullptr;
            int n = 0;
            for (const auto& kv : r.bySource) {
                if (source == nullptr && kv.second.radio.driver == health::driverWord(driver)) { only = &kv.second; ++n; }
            }
            if (n == 1) { w.radio = only->radio; w.openSec = std::chrono::duration_cast<std::chrono::seconds>(now - only->openedAt).count(); w.registered = true; return w; }
        } else {
            w.radio = it->second.radio;
            w.openSec = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.openedAt).count();
            w.registered = true;
            return w;
        }
    }
    return w;   // not registered: no open radio is known to be this one
}

std::string pointerKey(const void* p) { char b[32]; std::snprintf(b, sizeof(b), "ptr:%p", p); return b; }

std::atomic<std::uint64_t> g_unattributed{0};   // driver events with no open radio to be about (dropped, counted)
std::atomic<std::uint64_t> g_lost{0};           // calls that threw inside the helper (a driver on a noexcept path must never be taken down by a report)

// FOLDING: a slider dragged against a refusal, a device that answers every read with the same fault. One call in `minGapMs` goes out; the ones between are counted
// in the reporter's own `dropped` for that radio (the field every sent event of the radio carries, contract 3.1), so the number is said and never lost.
bool admitCall(const std::string& radioKey, const std::string& what, long long minGapMs) {
    const std::string key = radioKey + "|" + what;
    bool fold = false;
    {
        Registry& r = registry();
        std::lock_guard<std::mutex> lk(r.mu);
        Throttle& t = r.throttle[key];
        const auto now = std::chrono::steady_clock::now();
        if (t.seen && std::chrono::duration_cast<std::chrono::milliseconds>(now - t.last).count() < minGapMs) {
            fold = true;
        } else {
            t.seen = true;
            t.last = now;
        }
    }
    if (fold) { global().countDropped(radioKey, 1); }   // outside the registry's lock: the reporter takes its own
    return !fold;
}

long long clampLl(long long v, long long lo, long long hi) { return std::min(std::max(v, lo), hi); }

// Said now when the radio is known; held (at most 16, none older than two minutes) when the driver raised it while the radio was still opening and is said when
// the engine registers the source; dropped and counted when it can never be attributed.
void emitOrDefer(const void* source, bool registered, Event& ev) {
    if (registered) { global().event(ev); return; }
    if (source == nullptr) { ++g_unattributed; return; }
    Registry& r = registry();
    std::lock_guard<std::mutex> lk(r.mu);
    const auto now = std::chrono::steady_clock::now();
    for (auto it = r.deferred.begin(); it != r.deferred.end();) {
        if (now - it->at > std::chrono::minutes(2)) { ++g_unattributed; it = r.deferred.erase(it); } else { ++it; }
    }
    if (r.deferred.size() >= 16) { ++g_unattributed; r.deferred.erase(r.deferred.begin()); }
    r.deferred.push_back(Deferred{source, ev, now});
}

}  // namespace

// ---------------------------------------------------------------------------

void registerSource(const void* source, const RadioIdentity& radio) {
    if (source == nullptr || radio.key.empty()) { return; }
    std::vector<Event> now;
    {
        Registry& r = registry();
        std::lock_guard<std::mutex> lk(r.mu);
        r.bySource[source] = Registered{radio, std::chrono::steady_clock::now()};
        r.closeReported.erase(radio.key);   // a new open of the radio: its next ending is a new one
        for (auto it = r.deferred.begin(); it != r.deferred.end();) {
            if (it->source == source) { now.push_back(std::move(it->ev)); it = r.deferred.erase(it); } else { ++it; }
        }
    }
    for (Event& ev : now) {
        ev.radio = radio;   // what the driver said while the radio was opening (a setting it refused and carried on without), now about the right radio
        try { global().event(ev); } catch (...) { ++g_lost; }
    }
}

void forgetRadio(const std::string& radioKey) {
    Registry& r = registry();
    std::lock_guard<std::mutex> lk(r.mu);
    for (auto it = r.bySource.begin(); it != r.bySource.end();) {
        if (it->second.radio.key == radioKey) { it = r.bySource.erase(it); } else { ++it; }
    }
}

bool takeCloseReported(const std::string& radioKey) {
    Registry& r = registry();
    std::lock_guard<std::mutex> lk(r.mu);
    const auto it = r.closeReported.find(radioKey);
    if (it == r.closeReported.end()) { return false; }
    r.closeReported.erase(it);
    return true;
}

std::string closeCauseFor(bool deviceDead, const std::string& text) {
    const std::string t = lower(text);
    // gone from the bus: the radio was taken away
    static const char* const kGone[] = {"unplug",        "disconnect",  "device removed", "removed",       "no such device", "no device",
                                        "device not found", "not found", "not present",    "no longer",     "went away",      "gone",
                                        "libusb_error_no_device", "device is not connected"};
    for (const char* g : kGone) {
        if (contains(t, g)) { return "unplugged"; }
    }
    static const char* const kApp[] = {"exception", "threw", "bad_alloc", "access violation", "internal error", "crashed"};
    for (const char* a : kApp) {
        if (contains(t, a)) { return "error"; }
    }
    if (deviceDead) { return "fault"; }
    return std::string();
}

const char* controlWord(Control c) {
    switch (c) {
        case Control::Frequency: return "frequency";
        case Control::Rate: return "rate";
        case Control::Bandwidth: return "bandwidth";
        case Control::Gain: return "gain";
        case Control::AutoGain: return "gain";
        case Control::Ppm: return "ppm";
        case Control::BiasTee: return "bias";
        case Control::Antenna: return "antenna";
        case Control::DirectSampling: return "direct";
        case Control::Decimation: return "decimation";
        case Control::Other: break;
    }
    return "other";
}

const char* controlRefusedWord(Control c) {
    switch (c) {
        case Control::Frequency: return "";
        case Control::Other: return "";
        default: return controlWord(c);
    }
}

Control controlFromText(const std::string& what) {
    const std::string t = lower(what);
    if (contains(t, "agc") || contains(t, "gain mode")) { return Control::AutoGain; }
    if (contains(t, "gain") || contains(t, "lna") || contains(t, "attenuat") || contains(t, "preamp")) { return Control::Gain; }
    if (contains(t, "sample rate") || contains(t, "samplerate") || contains(t, " rate")) { return Control::Rate; }
    if (contains(t, "bandwidth")) { return Control::Bandwidth; }
    if (contains(t, "retune") || contains(t, "frequency") || contains(t, "centre") || contains(t, "center")) { return Control::Frequency; }
    if (contains(t, "antenna") || contains(t, "port")) { return Control::Antenna; }
    if (contains(t, "bias")) { return Control::BiasTee; }
    if (contains(t, "direct")) { return Control::DirectSampling; }
    if (contains(t, "decimat")) { return Control::Decimation; }
    if (contains(t, "ppm") || contains(t, "crystal") || contains(t, "correction")) { return Control::Ppm; }
    return Control::Other;
}

ControlWhyScope::ControlWhyScope(const char* why) : previous_(t_why) { t_why = why != nullptr ? why : "user"; }
ControlWhyScope::~ControlWhyScope() { t_why = previous_; }

std::uint64_t refusalSequence() { return t_refusals; }

RefusalWatch::RefusalWatch() : start_(t_refusals) {}

void noteRefusalIfSilentSince(std::uint64_t sequenceBefore, const void* source, const char* driverKey, Control control, const std::string& text, bool userRange) {
    if (t_refusals != sequenceBefore) { return; }   // a driver said it, at the place it noticed, on this thread: that is the one report of this refusal
    noteControlRefused(source, driverKey, control, text, userRange);
}

void RefusalWatch::noteRefusalIf(bool refused, const void* source, const char* driverKey, Control control, const std::string& text, bool userRange) {
    if (refused) { noteRefusalIfSilentSince(start_, source, driverKey, control, text, userRange); }
}

void RefusalWatch::noteRefusalIfSilent(const void* source, const char* driverKey, Control control, const std::string& text, bool userRange) {
    noteRefusalIfSilentSince(start_, source, driverKey, control, text, userRange);
}

void noteControlRefused(const void* source, const char* driverKey, Control control, const std::string& reason, bool userRange, const char* why) {
    ++t_refusals;
    try {
        const Who w = whoIs(source, driverKey);
        if (!admitCall(w.registered ? w.radio.key : pointerKey(source), std::string("refused-") + controlWord(control), 5000)) { return; }
        Event ev;
        ev.kind = "radio_settings";
        ev.radio = w.radio;
        const std::string whyWord = why != nullptr ? why : (t_why != nullptr ? t_why : "user");
        ev.word = std::string("refused-") + controlWord(control);   // the daily bound's (radio, kind, word): one first-and-last per control
        ev.set("why", whyWord);
        const std::string refused = controlRefusedWord(control);
        if (!refused.empty()) { ev.set("refused", refused); }
        // the control's NAME, then (for a value the user typed out of range) the class, then the driver's sentence: the site sees the control even where the contract
        // has no `refused` word for it
        std::string text = std::string(userRange ? "out of range: " : "refused: ") + controlWord(control);
        if (!reason.empty()) { text += ": " + reason; }
        ev.text("reason", text, 200);
        emitOrDefer(source, w.registered, ev);
    } catch (...) {
        ++g_lost;
    }
}

std::string noteControlRefusedText(const void* source, const char* driverKey, Control control, std::string text, bool userRange, const char* why) {
    noteControlRefused(source, driverKey, control, text, userRange, why);
    return text;
}

void noteDeviceFault(const void* source, const char* driverKey, const std::string& what, const std::string& detail) {
    try {
        const Who w = whoIs(source, driverKey);
        if (!w.registered) { ++g_unattributed; return; }
        const std::string sentence = what + (detail.empty() ? std::string() : (": " + detail));
        std::string cause = closeCauseFor(true, sentence);
        if (cause.empty()) { cause = "fault"; }
        {
            // ONCE PER OPEN of a radio: a dead device fails every later transfer too, and the driver says so every time (the first cause is the one that counts)
            Registry& r = registry();
            std::lock_guard<std::mutex> lk(r.mu);
            if (r.closeReported[w.radio.key]) { return; }
            r.closeReported[w.radio.key] = true;
        }
        Event ev;
        ev.kind = "radio_close";
        ev.radio = w.radio;
        ev.word = cause;
        ev.set("cause", cause).set("openSec", clampLl(w.openSec, 0, 2592000));
        ev.text("reason", sentence, 400);
        ev.urgent = true;
        global().event(ev);
    } catch (...) {
        ++g_lost;
    }
}

void noteStreamTrouble(const void* source, const char* driverKey, const StreamTrouble& t) {
    try {
        const Who w = whoIs(source, driverKey);
        if (!w.registered) { ++g_unattributed; return; }
        const long long windowSec = clampLl(static_cast<long long>(t.windowSec + 0.5), 1, 3600);
        Event ev;
        ev.kind = "radio_stream";
        ev.radio = w.radio;
        ev.word = "trouble";
        ev.set("windowSec", windowSec);
        ev.set("samplesPerSec", clampLl(static_cast<long long>(t.samples / static_cast<std::uint64_t>(windowSec)), 0, 1000000000LL));
        ev.set("drops", clampLl(static_cast<long long>(t.overflows), 0, 1000000000LL));
        ev.set("usbErrors", clampLl(static_cast<long long>(t.errors), 0, 1000000000LL));
        ev.set("stalls", static_cast<long long>(t.longestGapMs >= 1000 ? 1 : 0));
        ev.set("openSec", clampLl(w.openSec, 0, 2592000));
        global().event(ev);
    } catch (...) {
        ++g_lost;
    }
}

void noteVendorCallFault(const void* source, const char* driverKey, const char* what, const std::string& reason, bool absorbed) {
    try {
        const Who w = whoIs(source, driverKey);
        const std::string word = what != nullptr ? what : "vendorcall";
        if (!admitCall(w.registered ? w.radio.key : pointerKey(source), "vendor-" + word, 10000)) { return; }
        Event ev;
        ev.kind = "radio_recovery";
        ev.radio = w.radio;
        const char* phase = absorbed ? "ok" : "gave-up";
        ev.word = std::string(phase) + "-" + word;
        ev.set("phase", phase).set("what", word);
        ev.set("reasonClass", "vendor");
        if (!reason.empty()) { ev.text("reason", reason, 400); }
        ev.urgent = !absorbed;
        emitOrDefer(source, w.registered, ev);
    } catch (...) {
        ++g_lost;
    }
}

void noteServiceFault(const char* driverKey, const char* what, const std::string& reason, bool absorbed) {
    // A service that is not running while NO radio of that driver is open is information (nobody's radio failed; a person without the vendor's software has the
    // same state): with no radio to be about the call is dropped and counted, and no radio is made up.
    noteVendorCallFault(nullptr, driverKey, what, reason, absorbed);
}

void fileSideRadioFailure(const char* driverKey, const std::string& args, const std::string& reason, const char* trigger) {
    try {
        const std::string kind = driverKey != nullptr ? driverKey : "other";
        FailureInput in;
        in.driverKind = kind;
        in.args = args;
        in.reason = reason;
        in.reasonClass = health::radioReasonWord(health::classifyRadioOpen(kind, reason));
        const std::string t = trigger != nullptr ? trigger : "other";
        in.trigger = Trigger::Other;
        for (const Trigger c : {Trigger::StartupRestore, Trigger::UserPick, Trigger::SavedRetry, Trigger::PatchRadio, Trigger::AutoReopen}) {
            if (t == triggerWord(c)) { in.trigger = c; }
        }
        global().fileFailure(in, DiagLog::instance().ringSnapshot());
    } catch (...) {
        ++g_lost;
    }
}

void noteSideRadioStopped(const char* driverKey, const std::string& args, const std::string& what, const std::string& detail) {
    try {
        const std::string kind = driverKey != nullptr ? driverKey : "other";
        const std::string sentence = what + (detail.empty() ? std::string() : (": " + detail));
        std::string cause = closeCauseFor(true, sentence);
        if (cause.empty()) { cause = "fault"; }
        Event ev;
        ev.kind = "radio_close";
        ev.radio = identify(kind, args);
        ev.word = cause;
        ev.set("cause", cause).set("openSec", 0);
        ev.text("reason", sentence, 400);
        ev.urgent = true;
        global().event(ev);
    } catch (...) {
        ++g_lost;
    }
}

std::uint64_t unattributedCount() { return g_unattributed.load(); }
std::uint64_t lostCount() { return g_lost.load(); }

}  // namespace cascade::core::radioreport
