// radio_events.cpp - see radio_events.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/radio_events.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <map>
#include <mutex>
#include <vector>

#include "core/health_events.hpp"
#include "core/radio_reporter.hpp"

namespace cascade::core::radioreport {

namespace health = cascade::core::health;

namespace {

struct Registered {
    RadioIdentity radio;
    std::chrono::steady_clock::time_point openedAt;
};

struct Registry {
    std::mutex mu;
    std::map<const void*, Registered> bySource;
    std::map<std::string, bool> closeReported;   // radio key -> a driver already said this radio's ending
};

Registry& registry() {
    static Registry r;
    return r;
}

thread_local const char* t_why = "user";

std::string lower(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

bool contains(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

// THE RADIO a call is about: the registered source object, else the one registered radio of the driver, else the driver's family name.
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
            const Registered* only = nullptr;
            int n = 0;
            for (const auto& kv : r.bySource) {
                if (kv.second.radio.driver == health::driverWord(driver)) { only = &kv.second; ++n; }
            }
            if (n == 1) { w.radio = only->radio; w.openSec = std::chrono::duration_cast<std::chrono::seconds>(now - only->openedAt).count(); w.registered = true; return w; }
        } else {
            w.radio = it->second.radio;
            w.openSec = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.openedAt).count();
            w.registered = true;
            return w;
        }
    }
    w.radio = identify(driver, std::string());
    return w;
}

long long clampLl(long long v, long long lo, long long hi) { return std::min(std::max(v, lo), hi); }

}  // namespace

// ---------------------------------------------------------------------------

void registerSource(const void* source, const RadioIdentity& radio) {
    if (source == nullptr || radio.key.empty()) { return; }
    Registry& r = registry();
    std::lock_guard<std::mutex> lk(r.mu);
    r.bySource[source] = Registered{radio, std::chrono::steady_clock::now()};
    r.closeReported.erase(radio.key);   // a new open of the radio: its next ending is a new one
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

ControlWhyScope::ControlWhyScope(const char* why) : previous_(t_why) { t_why = why != nullptr ? why : "user"; }
ControlWhyScope::~ControlWhyScope() { t_why = previous_; }

void noteControlRefused(const void* source, const char* driverKey, Control control, const std::string& reason, bool userRange) {
    const Who w = whoIs(source, driverKey);
    Event ev;
    ev.kind = "radio_settings";
    ev.radio = w.radio;
    const std::string why = t_why != nullptr ? t_why : "user";
    ev.word = std::string("refused-") + controlWord(control);   // the daily bound's (radio, kind, word): one first-and-last per control
    ev.set("why", why);
    const std::string refused = controlRefusedWord(control);
    if (!refused.empty()) { ev.set("refused", refused); }
    // the control's NAME, then (for a value the user typed out of range) the class, then the driver's sentence: "ppm" is what the site sees even where the contract
    // has no `refused` word for the control
    std::string text = std::string(userRange ? "out of range: " : "refused: ") + controlWord(control);
    if (!reason.empty()) { text += ": " + reason; }
    ev.text("reason", text, 200);
    global().event(ev);
}

std::string noteControlRefusedText(const void* source, const char* driverKey, Control control, std::string text, bool userRange) {
    noteControlRefused(source, driverKey, control, text, userRange);
    return text;
}

void noteDeviceFault(const void* source, const char* driverKey, const std::string& what, const std::string& detail) {
    const Who w = whoIs(source, driverKey);
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
}

void noteStreamTrouble(const void* source, const char* driverKey, const StreamTrouble& t) {
    const Who w = whoIs(source, driverKey);
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
    if (w.registered) { ev.set("openSec", clampLl(w.openSec, 0, 2592000)); }
    global().event(ev);
}

void noteVendorCallFault(const void* source, const char* driverKey, const char* what, const std::string& reason, bool absorbed) {
    const Who w = whoIs(source, driverKey);
    Event ev;
    ev.kind = "radio_recovery";
    ev.radio = w.radio;
    const char* phase = absorbed ? "ok" : "gave-up";
    ev.word = std::string(phase) + "-" + (what != nullptr ? what : "vendorcall");
    ev.set("phase", phase).set("what", what != nullptr ? what : "vendorcall");
    ev.set("reasonClass", "vendor");
    if (!reason.empty()) { ev.text("reason", reason, 400); }
    ev.urgent = !absorbed;
    global().event(ev);
}

void noteServiceFault(const char* driverKey, const char* what, const std::string& reason, bool absorbed) {
    noteVendorCallFault(nullptr, driverKey, what, reason, absorbed);
}

}  // namespace cascade::core::radioreport
