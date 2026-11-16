// fobos_source.cpp - see fobos_source.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/fobos_source.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <mutex>
#include <sstream>
#include <thread>

#include "source/fobos_api.hpp"
#include "source/vendor_apartment.hpp"
#include "source/vendor_guard.hpp"
#include "source/vendor_lib.hpp"

namespace cascade::source {

namespace {

using vendorapt::Apartment;
using vendorapt::nowMs;

constexpr int kDefaultCallMs = 5000;
constexpr int kDefaultSilenceMs = 5000;
constexpr std::uint32_t kStreamBuffers = 8;
constexpr std::uint32_t kStreamLength = 16384;  // what is asked for; a callback longer than this is refused whichever way the length is counted
constexpr std::uint32_t kMaxDevices = 16;
constexpr std::size_t kListSlack = 4096;
constexpr std::size_t kInfoSlot = 256;          // each text field of the board info gets this many bytes; the library documents 64
constexpr std::size_t kMaxRates = 256;
constexpr double kPreferredRateHz = 2000000.0;
constexpr unsigned kLnaMax = 3;
constexpr unsigned kVgaMax = 31;

struct Link : vendorapt::StreamLink {
    std::atomic<bool> lengthIsPairs{false};
    std::uint32_t requested = kStreamLength;
};

void FOX_FOBOS_CALL streamCallback(float* buffer, std::uint32_t length, void* context) {
    Link* l = static_cast<Link*>(context);
    if (l == nullptr) { return; }
    if (buffer == nullptr || length == 0 || length > l->requested) {
        l->countBad();
        return;
    }
    const std::uint32_t floats = l->lengthIsPairs.load(std::memory_order_relaxed) ? length * 2u : length;
    l->deliverFloats(buffer, floats);
}

// What the thread that makes the streaming call tells the owner.
struct StreamState {
    std::atomic<bool> done{false};
    std::atomic<int> rc{0};
    std::atomic<bool> faulted{false};   // the library crashed inside the streaming call (caught: an access violation is a refusal, not a crash of FoxSDR)
    std::atomic<std::int64_t> endedMs{0};
};

void trampoline(void* ctx) noexcept {
    auto* fn = static_cast<std::function<void()>*>(ctx);
    try {
        (*fn)();
    } catch (...) {
    }
}

struct Out {
    bool ok = true;
    std::string e;
    std::string serial;
    fobos::Device* dev = nullptr;
    std::vector<double> rates;
    double rate = 0.0;
};

long long parseInt(const std::string& v, long long dflt, long long lo, long long hi) {
    if (v.empty()) { return dflt; }
    char* end = nullptr;
    const long long x = std::strtoll(v.c_str(), &end, 10);
    if (end == v.c_str() || *end != '\0' || x < lo || x > hi) { return -1; }
    return x;
}

std::string cleanField(const char* s, std::size_t max) {
    std::string r;
    for (std::size_t i = 0; i < max && s[i] != '\0'; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x20 || c >= 0x7F) { break; }
        r += static_cast<char>(c);
    }
    return r;
}

std::string lower(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

}  // namespace

struct FobosSource::Impl {
    mutable std::mutex m;
    vendorlib::Library lib;
    fobos::Api api;
    std::unique_ptr<Apartment> apt;
    Link* link = nullptr;
    fobos::Device* dev = nullptr;
    std::shared_ptr<StreamState> ss;
    std::thread streamThread;
    bool streamStuck = false;
    bool opened = false, streaming = false, dead = false;
    std::string error, deadWhile, verdict, serial, note;
    int callMs = kDefaultCallMs, silenceMs = kDefaultSilenceMs;
    std::vector<double> rates;
    double rateHz = 0.0, centerHz = 0.0;
    unsigned lna = 0, vga = 0;
    std::int64_t streamStartMs = 0;

    void setError(const std::string& e) {
        std::lock_guard<std::mutex> lk(m);
        error = e;
    }
    void markDead(const char* during, const std::string& why) {
        std::lock_guard<std::mutex> lk(m);
        dead = true;
        deadWhile = during;
        error = why;
    }
    bool call(const char* during, const std::function<void()>& fn) {
        if (!apt) {
            setError("the Fobos SDR is not open");
            return false;
        }
        switch (apt->run(fn, callMs)) {
            case Apartment::Result::Done: return true;
            case Apartment::Result::TimedOut:
                markDead(during, std::string("the Fobos library did not answer while ") + during + "; FoxSDR must be restarted to use this radio again");
                break;
            case Apartment::Result::Faulted:
                markDead(during, std::string("the Fobos library crashed while ") + during + " (FoxSDR caught it and carried on)");
                break;
            case Apartment::Result::Dead:
                setError("the Fobos SDR has stopped answering; FoxSDR must be restarted to use it again");
                break;
        }
        return false;
    }
    // The library's own words for a code, bounded and printable; the number when it offers none.
    static std::string codeText(const fobos::Api& a, int rc) {
        std::string t;
        if (a.ErrorName != nullptr) {
            const char* s = a.ErrorName(rc);
            if (s != nullptr) { t = cleanField(s, 80); }
        }
        if (t.empty()) { t = "error " + std::to_string(rc); }
        return t;
    }
    static std::string refused(const fobos::Api& a, const char* what, int rc) { return std::string(what) + " (the library said: " + codeText(a, rc) + ")"; }
    // Joins or abandons the thread that made the streaming call. True when it is gone.
    bool reapStreamThread(int waitMs) {
        if (!streamThread.joinable()) { return true; }
        const std::int64_t end = nowMs() + waitMs;
        while (ss && !ss->done.load() && nowMs() < end) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        if (ss && !ss->done.load()) { return false; }
        streamThread.join();
        return true;
    }
};

FobosSource::FobosSource() : p_(std::make_unique<Impl>()) {}
FobosSource::~FobosSource() { closeDevice(); }

const char* FobosSource::lastError() const {
    thread_local std::string copy;
    {
        std::lock_guard<std::mutex> lk(p_->m);
        copy = p_->error;
    }
    return copy.c_str();
}
bool FobosSource::isOpen() const {
    std::lock_guard<std::mutex> lk(p_->m);
    return p_->opened;
}
bool FobosSource::running() const {
    std::lock_guard<std::mutex> lk(p_->m);
    return p_->streaming;
}
bool FobosSource::deviceDead() const {
    std::lock_guard<std::mutex> lk(p_->m);
    return p_->dead;
}
bool FobosSource::faulted() const { return deviceDead(); }
std::string FobosSource::faultedWhile() const {
    std::lock_guard<std::mutex> lk(p_->m);
    return p_->deadWhile;
}
std::string FobosSource::openVerdictWord() const {
    std::lock_guard<std::mutex> lk(p_->m);
    return p_->verdict;
}
std::string FobosSource::streamNote() const {
    std::lock_guard<std::mutex> lk(p_->m);
    return p_->note;
}
std::string FobosSource::serialMasked() const {
    std::lock_guard<std::mutex> lk(p_->m);
    if (p_->serial.size() <= 4) { return p_->serial.empty() ? std::string() : std::string("****"); }
    return std::string(p_->serial.size() - 4, '*') + p_->serial.substr(p_->serial.size() - 4);
}
FobosStats FobosSource::stats() const {
    FobosStats s;
    std::lock_guard<std::mutex> lk(p_->m);
    if (p_->link != nullptr) {
        s.callbacks = p_->link->callbacks.load();
        s.samplePairs = p_->link->pairs.load();
        s.pairsDropped = p_->link->dropped.load();
        s.badCallbacks = p_->link->bad.load();
    }
    return s;
}

bool FobosSource::open(const std::string& args) {
    Impl& I = *p_;
    if (isOpen()) { return true; }
    {
        std::lock_guard<std::mutex> lk(I.m);
        I.error.clear();
        I.verdict.clear();
        I.note.clear();
        I.dead = false;
        I.deadWhile.clear();
    }
    const std::string libWhere = argValue(args, "lib");
    const std::string wantSerial = argValue(args, "serial");
    const long long wantIndex = parseInt(argValue(args, "index"), -2, 0, 1000);
    const long long rate = parseInt(argValue(args, "rate"), static_cast<long long>(kPreferredRateHz), 1000, 1000000000);
    const long long to = parseInt(argValue(args, "timeout"), kDefaultCallMs, 100, 120000);
    const long long sil = parseInt(argValue(args, "silence"), kDefaultSilenceMs, 200, 600000);
    const std::string countWord = lower(argValue(args, "count"));
    const std::string directWord = lower(argValue(args, "direct"));
    const std::string clockWord = lower(argValue(args, "clock"));
    const bool badWord = (!countWord.empty() && countWord != "floats" && countWord != "pairs") || (!directWord.empty() && directWord != "on" && directWord != "off") ||
                         (!clockWord.empty() && clockWord != "internal" && clockWord != "external");
    if (rate < 0 || to < 0 || sil < 0 || wantIndex == -1 || badWord) {
        I.setError("the Fobos settings are not valid (rate, index, timeout and silence are whole numbers in range; count is floats or pairs; direct is on or off; clock is internal or external)");
        return false;
    }
    I.callMs = static_cast<int>(to);
    I.silenceMs = static_cast<int>(sil);

    // Find the library: the person's file or folder, else each name in turn on the PATH.
    std::string path, why;
    for (const char* nm : fobos::kLibraryNames) {
        std::string w;
        path = vendorlib::findLibrary(libWhere, nm, w);
        if (!path.empty()) { break; }
        if (why.empty()) { why = w; }
    }
    if (path.empty()) {
        std::string names;
        for (const char* nm : fobos::kLibraryNames) { names += (names.empty() ? "" : " or ") + std::string(nm); }
        std::lock_guard<std::mutex> lk(I.m);
        I.verdict = vendorlib::verdictWord(vendorlib::Verdict::NotFound);
        I.error = "The Fobos SDR library was not found (looked for " + names + "). Fobos SDR needs the maker's own library, which you install yourself: FoxSDR does not include or download it. Put it in a folder of your own and give that folder or file as lib=.";
        return false;
    }
    const vendorlib::Check c = I.lib.open(path);
    if (!c.ok()) {
        std::lock_guard<std::mutex> lk(I.m);
        I.verdict = vendorlib::verdictWord(c.verdict);
        I.error = c.sentence;
        return false;
    }
    std::vector<const char*> names;
    for (const char* n : fobos::kRequired) { names.push_back(n); }
    for (const char* n : fobos::kOptional) { names.push_back(n); }
    std::vector<void*> fn;
    vendorlib::resolveAll(I.lib, names, fn);
    std::string missing;
    for (std::size_t i = 0; i < std::size(fobos::kRequired); ++i) {
        if (fn[i] == nullptr) { missing += (missing.empty() ? "" : ", ") + std::string(fobos::kRequired[i]); }
    }
    if (!missing.empty()) {
        I.lib.close();
        std::lock_guard<std::mutex> lk(I.m);
        I.verdict = vendorlib::verdictWord(vendorlib::Verdict::SymbolMissing);
        I.error = "This library does not offer what FoxSDR needs for a Fobos SDR (" + missing + "): it is not a Fobos library this build knows, or it is too old";
        return false;
    }
    {
        std::size_t i = 0;
        I.api.GetDeviceCount = reinterpret_cast<fobos::GetDeviceCountFn>(fn[i++]);
        I.api.ListDevices = reinterpret_cast<fobos::ListDevicesFn>(fn[i++]);
        I.api.Open = reinterpret_cast<fobos::OpenFn>(fn[i++]);
        I.api.Close = reinterpret_cast<fobos::CloseFn>(fn[i++]);
        I.api.GetBoardInfo = reinterpret_cast<fobos::GetBoardInfoFn>(fn[i++]);
        I.api.SetFrequency = reinterpret_cast<fobos::SetFrequencyFn>(fn[i++]);
        I.api.GetSampleRates = reinterpret_cast<fobos::GetSampleRatesFn>(fn[i++]);
        I.api.SetSampleRate = reinterpret_cast<fobos::SetSampleRateFn>(fn[i++]);
        I.api.ReadAsync = reinterpret_cast<fobos::ReadAsyncFn>(fn[i++]);
        I.api.CancelAsync = reinterpret_cast<fobos::CancelAsyncFn>(fn[i++]);
        I.api.SetLnaGain = reinterpret_cast<fobos::SetLnaGainFn>(fn[i++]);
        I.api.SetVgaGain = reinterpret_cast<fobos::SetVgaGainFn>(fn[i++]);
        I.api.SetDirectSampling = reinterpret_cast<fobos::SetDirectSamplingFn>(fn[i++]);
        I.api.SetClockSource = reinterpret_cast<fobos::SetClockSourceFn>(fn[i++]);
        I.api.ErrorName = reinterpret_cast<fobos::ErrorNameFn>(fn[i++]);
    }
    I.link = new Link();
    I.link->lengthIsPairs.store(countWord == "pairs");
    I.apt = std::make_unique<Apartment>();

    const auto fail = [&](const std::string& e, const char* verdict) {
        {
            std::lock_guard<std::mutex> lk(I.m);
            I.error = e;
            I.verdict = verdict;
        }
        closeDevice();
        return false;
    };

    // 1. Count, list, choose, open, board info, rates, first rate: all on the apartment's thread.
    auto o = std::make_shared<Out>();
    const fobos::Api api = I.api;
    const bool wantDirect = !directWord.empty(), directOn = directWord == "on";
    const bool wantClock = !clockWord.empty(), clockExt = clockWord == "external";
    if (!I.call("opening the radio", [o, api, wantSerial, wantIndex, rate, wantDirect, directOn, wantClock, clockExt] {
            const int count = api.GetDeviceCount();
            if (count < 0) {
                o->e = Impl::refused(api, "the library could not count the radios", count);
                o->ok = false;
                return;
            }
            if (count == 0) {
                o->e = "no Fobos SDR is connected";
                o->ok = false;
                return;
            }
            if (static_cast<std::uint32_t>(count) > kMaxDevices) {
                o->e = "the library reports an impossible number of radios; FoxSDR will not read its list";
                o->ok = false;
                return;
            }
            const std::size_t listBytes = static_cast<std::size_t>(count) * (fobos::kInfoLen + 1);
            std::vector<char> raw(listBytes + kListSlack, '\xA5');
            std::memset(raw.data(), 0, listBytes);
            const int lr = api.ListDevices(raw.data());
            bool intact = true;
            for (std::size_t i = listBytes; i < raw.size(); ++i) { intact = intact && raw[i] == '\xA5'; }
            if (!intact) {
                o->e = "the library wrote past the end of the radio list FoxSDR gave it: it is a different version of the library, and FoxSDR will not use it";
                o->ok = false;
                return;
            }
            if (lr < 0) {
                o->e = Impl::refused(api, "the library could not list the radios", lr);
                o->ok = false;
                return;
            }
            std::vector<std::string> serials;
            {
                std::istringstream is(cleanField(raw.data(), listBytes));
                std::string tok;
                while (is >> tok && serials.size() < kMaxDevices) { serials.push_back(tok.substr(0, 64)); }
            }
            if (serials.empty()) {
                o->e = "the library listed no serial number for the radio it counted";
                o->ok = false;
                return;
            }
            std::size_t pick = 0;
            if (!wantSerial.empty()) {
                const auto it = std::find(serials.begin(), serials.end(), wantSerial);
                if (it == serials.end()) {
                    o->e = "no connected Fobos SDR has the serial number you gave";
                    o->ok = false;
                    return;
                }
                pick = static_cast<std::size_t>(it - serials.begin());
            } else if (wantIndex >= 0) {
                if (static_cast<std::size_t>(wantIndex) >= serials.size()) {
                    o->e = "there is no Fobos SDR at that position in the list";
                    o->ok = false;
                    return;
                }
                pick = static_cast<std::size_t>(wantIndex);
            }
            o->serial = serials[pick];
            fobos::Device* d = nullptr;
            const int orc = api.Open(&d, static_cast<std::uint32_t>(pick));
            if (orc != 0 || d == nullptr) {
                o->e = Impl::refused(api, "the radio could not be opened (is another program using it?)", orc != 0 ? orc : fobos::kErrNotOpen);
                o->ok = false;
                return;
            }
            o->dev = d;
            // Board info: five text fields, each given a slot far larger than the library documents, with the part past the documented size checked.
            std::vector<char> info(5 * kInfoSlot, '\xA5');
            for (int f = 0; f < 5; ++f) { std::memset(info.data() + static_cast<std::size_t>(f) * kInfoSlot, 0, fobos::kInfoLen); }
            char* b = info.data();
            const int irc = api.GetBoardInfo(d, b, b + kInfoSlot, b + 2 * kInfoSlot, b + 3 * kInfoSlot, b + 4 * kInfoSlot);
            bool infoIntact = true;
            for (int f = 0; f < 5; ++f) {
                for (std::size_t i = fobos::kInfoLen; i < kInfoSlot; ++i) { infoIntact = infoIntact && info[static_cast<std::size_t>(f) * kInfoSlot + i] == '\xA5'; }
            }
            if (!infoIntact) {
                o->e = "the library wrote more than its documented size into the board information FoxSDR gave it: it is a different version of the library, and FoxSDR will not use it";
                o->ok = false;
                return;
            }
            if (irc == 0) {
                const std::string s = cleanField(b + 4 * kInfoSlot, fobos::kInfoLen);
                if (!s.empty()) { o->serial = s; }
            }
            // The rates, in a buffer twice the size read, with the second half checked.
            std::vector<double> rv(kMaxRates * 2, -12345.0);
            unsigned int n = static_cast<unsigned int>(kMaxRates);
            const int rrc = api.GetSampleRates(d, rv.data(), &n);
            bool ratesIntact = true;
            for (std::size_t i = kMaxRates; i < rv.size(); ++i) { ratesIntact = ratesIntact && rv[i] == -12345.0; }
            if (!ratesIntact) {
                o->e = "the library wrote past the end of the sample-rate list FoxSDR gave it: it is a different version of the library, and FoxSDR will not use it";
                o->ok = false;
                return;
            }
            if (rrc != 0) {
                o->e = Impl::refused(api, "the radio would not list its sample rates", rrc);
                o->ok = false;
                return;
            }
            for (unsigned int i = 0; i < n && i < kMaxRates; ++i) {
                if (std::isfinite(rv[i]) && rv[i] >= 1000.0 && rv[i] <= 1.0e9) { o->rates.push_back(rv[i]); }
            }
            std::sort(o->rates.begin(), o->rates.end());
            o->rates.erase(std::unique(o->rates.begin(), o->rates.end()), o->rates.end());
            if (o->rates.empty()) {
                o->e = "the radio offered no sample rate this build can use";
                o->ok = false;
                return;
            }
            double best = o->rates[0];
            for (const double r : o->rates) {
                if (std::fabs(r - static_cast<double>(rate)) < std::fabs(best - static_cast<double>(rate))) { best = r; }
            }
            double actual = 0.0;
            const int src = api.SetSampleRate(d, best, &actual);
            if (src != 0) {
                o->e = Impl::refused(api, "the radio refused the sample rate", src);
                o->ok = false;
                return;
            }
            o->rate = (std::isfinite(actual) && actual >= 1000.0 && actual <= 1.0e9) ? actual : best;
            if (wantDirect) {
                if (api.SetDirectSampling == nullptr) {
                    o->e = "this library has no direct-sampling setting";
                    o->ok = false;
                    return;
                }
                const int drc = api.SetDirectSampling(d, directOn ? 1u : 0u);
                if (drc != 0) {
                    o->e = Impl::refused(api, "the radio refused the direct-sampling setting", drc);
                    o->ok = false;
                    return;
                }
            }
            if (wantClock) {
                if (api.SetClockSource == nullptr) {
                    o->e = "this library has no clock-source setting";
                    o->ok = false;
                    return;
                }
                const int crc = api.SetClockSource(d, clockExt ? 1 : 0);
                if (crc != 0) {
                    o->e = Impl::refused(api, "the radio refused the clock-source setting", crc);
                    o->ok = false;
                    return;
                }
            }
        })) {
        // A call that never returned or crashed: the radio's handle may exist but nothing may be called now.
        return fail(I.error, "call-failed");
    }
    if (o->dev != nullptr) { I.dev = o->dev; }
    if (!o->ok) { return fail(o->e, "no-device"); }
    {
        std::lock_guard<std::mutex> lk(I.m);
        I.serial = o->serial;
        I.rates = o->rates;
        I.rateHz = o->rate;
        I.opened = true;
    }
    // A first tuning, best effort: the header gives no range, so a refusal here is not an open failure.
    if (!setCenterFrequencyHz(100.0e6)) {
        if (deviceDead()) { return fail(I.error, "tune-failed"); }
        I.setError("");
    }
    return true;
}

void FobosSource::closeDevice() {
    Impl& I = *p_;
    bool abandoned = false;
    if (I.apt) {
        if (running()) { stop(); }
        if (I.dev != nullptr && !I.apt->dead()) {
            const fobos::Api api = I.api;
            fobos::Device* d = I.dev;
            I.link->gate.store(false);
            I.call("closing the radio", [api, d] { api.Close(d); });
        }
        abandoned = !I.apt->shutdown() || I.apt->dead() || I.streamStuck;
    }
    if (abandoned) {
        // The library may still be inside a call or about to call back: its module, the link and the stream thread's data are leaked for the life of the process.
        if (I.streamThread.joinable()) { I.streamThread.detach(); }
        const bool wasDead = I.dead;
        const std::string keptError = I.error, keptWhile = I.deadWhile;
        Impl* leaked = p_.release();
        (void)leaked;
        p_ = std::make_unique<Impl>();
        p_->dead = true;
        p_->deadWhile = wasDead && !keptWhile.empty() ? keptWhile : "closing the radio";   // the first thing that went wrong stays the one reported
        p_->error = wasDead && !keptError.empty() ? keptError : "the Fobos library did not answer; FoxSDR must be restarted to use this radio again";
        return;
    }
    I.apt.reset();
    {
        std::lock_guard<std::mutex> lk(I.m);
        I.opened = false;
        I.streaming = false;
        I.rates.clear();
    }
    I.dev = nullptr;
    I.lib.close();
    delete I.link;
    I.link = nullptr;
    I.ss.reset();
}

bool FobosSource::start() {
    Impl& I = *p_;
    if (!isOpen()) {
        I.setError("the Fobos SDR is not open");
        return false;
    }
    if (deviceDead()) { return false; }
    if (running()) { return true; }
    std::complex<float> junk[512];
    while (I.link->ring.read(junk, 512) > 0) {}
    I.link->callbacks.store(0);
    I.link->gate.store(true, std::memory_order_release);
    I.ss = std::make_shared<StreamState>();
    {
        const fobos::Api api = I.api;
        fobos::Device* d = I.dev;
        Link* l = I.link;
        std::shared_ptr<StreamState> st = I.ss;
        I.streamThread = std::thread([api, d, l, st] {
            int rc = 0;
            std::function<void()> body = [&] { rc = api.ReadAsync(d, &streamCallback, l, kStreamBuffers, kStreamLength); };
            if (!callGuardingVendorFaults(&trampoline, &body)) {
                st->faulted.store(true);
                rc = fobos::kErrControl;
            }
            st->rc.store(rc);
            st->endedMs.store(nowMs());
            st->done.store(true);
        });
    }
    // The call either returns at once (a refusal, or a library that streams on its own threads) or stays inside until cancelled: wait a moment for either.
    const std::int64_t until = nowMs() + 400;
    while (nowMs() < until && !I.ss->done.load() && I.link->callbacks.load() == 0) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
    if (I.ss->done.load() && I.ss->rc.load() != 0) {
        const int rc = I.ss->rc.load();
        I.link->gate.store(false);
        I.streamThread.join();
        if (I.ss->faulted.load()) {
            I.markDead("starting the stream", "the Fobos library crashed while starting the stream (FoxSDR caught it and carried on)");
        } else {
            I.setError(Impl::refused(I.api, "the radio would not start streaming", rc));
        }
        return false;
    }
    std::lock_guard<std::mutex> lk(I.m);
    I.streaming = true;
    I.streamStartMs = nowMs();
    I.note.clear();
    I.link->lastDataMs.store(nowMs());
    return true;
}

void FobosSource::stop() {
    Impl& I = *p_;
    if (!running()) { return; }
    I.link->gate.store(false, std::memory_order_release);
    const fobos::Api api = I.api;
    fobos::Device* d = I.dev;
    I.call("stopping the stream", [api, d] { api.CancelAsync(d); });
    if (!I.reapStreamThread(I.callMs)) {
        I.streamStuck = true;
        if (!deviceDead()) { I.markDead("stopping the stream", "the Fobos library did not end its stream; FoxSDR must be restarted to use this radio again"); }
    }
    std::lock_guard<std::mutex> lk(I.m);
    I.streaming = false;
}

std::size_t FobosSource::read(std::complex<float>* dst, std::size_t n) {
    Impl& I = *p_;
    if (dst == nullptr || n == 0) { return 0; }
    Link* l = nullptr;
    bool streaming = false;
    {
        std::lock_guard<std::mutex> lk(I.m);
        l = I.link;
        streaming = I.streaming && !I.dead;
    }
    if (l == nullptr || !streaming) { return 0; }
    std::size_t got = l->ring.read(dst, n);
    if (got == 0) {
        std::unique_lock<std::mutex> lk(l->m);
        l->cv.wait_for(lk, std::chrono::milliseconds(20), [&] { return l->ring.size() > 0; });
        lk.unlock();
        got = l->ring.read(dst, n);
    }
    if (got == 0) {
        const std::int64_t silent = nowMs() - l->lastDataMs.load(std::memory_order_acquire);
        if (I.ss && I.ss->faulted.load()) {
            I.markDead("streaming", "the Fobos library crashed while streaming (FoxSDR caught it and carried on)");
        } else if (I.ss && I.ss->done.load() && I.ss->rc.load() != 0) {
            I.markDead("streaming", Impl::refused(I.api, "the Fobos SDR's stream ended", I.ss->rc.load()));
        } else if (silent > I.silenceMs) {
            I.markDead("streaming", "the Fobos SDR stopped delivering samples");
        }
    } else {
        // A stream whose measured rate is about half the one asked for suggests the buffer length counts complex samples (see the header).
        const std::int64_t since = nowMs() - I.streamStartMs;
        if (since > 2500 && !l->lengthIsPairs.load()) {
            const double seen = static_cast<double>(l->pairs.load() + l->dropped.load()) * 1000.0 / static_cast<double>(since);
            std::lock_guard<std::mutex> lk(I.m);
            if (I.note.empty() && I.rateHz > 0.0 && seen < 0.65 * I.rateHz && seen > 0.3 * I.rateHz) {
                I.note = "The samples arrive at about half the rate asked for: if the library counts complex samples in its buffer length, open the radio again with count=pairs.";
            }
        }
    }
    return got;
}

double FobosSource::sampleRateHz() const {
    std::lock_guard<std::mutex> lk(p_->m);
    return p_->rateHz;
}

std::vector<double> FobosSource::supportedSampleRatesHz() const {
    std::lock_guard<std::mutex> lk(p_->m);
    return p_->rates;
}

bool FobosSource::setSampleRateHz(double hz) {
    Impl& I = *p_;
    if (!isOpen() || deviceDead()) { return false; }
    if (!(hz > 0.0) || !std::isfinite(hz)) {
        I.setError("setSampleRateHz() requires a positive rate");
        return false;
    }
    double want = 0.0;
    {
        std::lock_guard<std::mutex> lk(I.m);
        want = I.rates.empty() ? hz : I.rates[0];
        for (const double r : I.rates) {
            if (std::fabs(r - hz) < std::fabs(want - hz)) { want = r; }
        }
        if (want == I.rateHz) { return true; }
    }
    const bool wasRunning = running();
    if (wasRunning) { stop(); }
    auto o = std::make_shared<Out>();
    const fobos::Api api = I.api;
    fobos::Device* d = I.dev;
    if (!I.call("changing the sample rate", [o, api, d, want] {
            double actual = 0.0;
            const int rc = api.SetSampleRate(d, want, &actual);
            if (rc != 0) {
                o->ok = false;
                o->e = Impl::refused(api, "the radio refused that sample rate", rc);
                return;
            }
            o->rate = (std::isfinite(actual) && actual >= 1000.0 && actual <= 1.0e9) ? actual : want;
        })) {
        return false;
    }
    if (!o->ok) {
        I.setError(o->e);
        if (wasRunning) { start(); }
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(I.m);
        I.rateHz = o->rate;
    }
    return wasRunning ? start() : true;
}

double FobosSource::centerFrequencyHz() const {
    std::lock_guard<std::mutex> lk(p_->m);
    return p_->centerHz;
}

bool FobosSource::setCenterFrequencyHz(double hz) {
    Impl& I = *p_;
    if (!isOpen() || deviceDead()) { return false; }
    if (!std::isfinite(hz) || hz <= 0.0 || hz > 1.0e11) {
        I.setError("that is not a frequency the radio can use");
        return false;
    }
    auto o = std::make_shared<Out>();
    const fobos::Api api = I.api;
    fobos::Device* d = I.dev;
    if (!I.call("tuning", [o, api, d, hz] {
            double actual = 0.0;
            const int rc = api.SetFrequency(d, hz, &actual);
            if (rc != 0) {
                o->ok = false;
                o->e = Impl::refused(api, "the radio refused that frequency", rc);
                return;
            }
            o->rate = (std::isfinite(actual) && actual > 0.0) ? actual : hz;   // (the tuned frequency, carried in the same field)
        })) {
        return false;
    }
    if (!o->ok) {
        I.setError(o->e);
        return false;
    }
    std::lock_guard<std::mutex> lk(I.m);
    I.centerHz = o->rate;
    return true;
}

std::vector<GainInfo> FobosSource::gains() const {
    std::vector<GainInfo> g;
    std::lock_guard<std::mutex> lk(p_->m);
    if (p_->api.SetLnaGain != nullptr) { g.push_back({"LNA", 0.0, static_cast<double>(kLnaMax), 1.0, GainUnit::Steps}); }
    if (p_->api.SetVgaGain != nullptr) { g.push_back({"VGA", 0.0, static_cast<double>(kVgaMax), 1.0, GainUnit::Steps}); }
    return g;
}

bool FobosSource::setGainDb(const std::string& name, double db) {
    Impl& I = *p_;
    const bool lna = name == "LNA", vga = name == "VGA";
    if ((!lna && !vga) || (lna && I.api.SetLnaGain == nullptr) || (vga && I.api.SetVgaGain == nullptr)) {
        I.setError("this radio has no gain called \"" + name + "\" here");
        return false;
    }
    if (!isOpen() || deviceDead()) { return false; }
    if (!std::isfinite(db)) {
        I.setError("that is not a gain setting");
        return false;
    }
    const unsigned maxV = lna ? kLnaMax : kVgaMax;
    const unsigned v = static_cast<unsigned>(std::clamp(std::llround(db), 0LL, static_cast<long long>(maxV)));
    auto o = std::make_shared<Out>();
    const fobos::Api api = I.api;
    fobos::Device* d = I.dev;
    if (!I.call("setting a gain", [o, api, d, lna, v] {
            const int rc = lna ? api.SetLnaGain(d, v) : api.SetVgaGain(d, v);
            if (rc != 0) {
                o->ok = false;
                o->e = Impl::refused(api, "the radio refused that gain setting", rc);
            }
        })) {
        return false;
    }
    if (!o->ok) {
        I.setError(o->e);
        return false;
    }
    std::lock_guard<std::mutex> lk(I.m);
    (lna ? I.lna : I.vga) = v;
    return true;
}

double FobosSource::gainDb(const std::string& name) const {
    std::lock_guard<std::mutex> lk(p_->m);
    return name == "LNA" ? static_cast<double>(p_->lna) : name == "VGA" ? static_cast<double>(p_->vga) : 0.0;
}

}  // namespace cascade::source
