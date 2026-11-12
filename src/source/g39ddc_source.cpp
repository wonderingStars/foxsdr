// g39ddc_source.cpp - see g39ddc_source.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/g39ddc_source.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#include "source/g39ddc_api.hpp"
#include "source/vendor_apartment.hpp"
#include "source/vendor_lib.hpp"

namespace cascade::source {

namespace {

constexpr int kDefaultCallMs = 5000;
constexpr int kDefaultSilenceMs = 5000;
constexpr std::uint32_t kSamplesPerBuffer = 4096;  // a multiple of 64, as the document requires
constexpr std::uint32_t kMaxDdcTypes = 64;
constexpr double kPreferredRateHz = 192000.0;

std::string cleanSerial(const char* s, std::size_t max) {
    std::string r;
    for (std::size_t i = 0; i < max && s[i] != '\0'; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x20 || c >= 0x7F) { break; }
        r += static_cast<char>(c);
    }
    return r;
}

long long parseInt(const std::string& v, long long dflt, long long lo, long long hi) {
    if (v.empty()) { return dflt; }
    char* end = nullptr;
    const long long x = std::strtoll(v.c_str(), &end, 10);
    if (end == v.c_str() || *end != '\0' || x < lo || x > hi) { return -1; }
    return x;
}

}  // namespace

bool g39PlanTune(double centerHz, std::uint64_t minHz, std::uint64_t maxHz, std::uint32_t stepHz, std::uint32_t windowHz, std::uint32_t ddcBandwidthHz,
                 G39Tune& out, std::string& why) {
    if (!std::isfinite(centerHz) || centerHz < 0.0 || centerHz > 1.0e12) {
        why = "that is not a frequency the receiver can use";
        return false;
    }
    const std::uint64_t step = stepHz != 0 ? stepHz : 1;
    const std::int64_t c = static_cast<std::int64_t>(std::llround(centerHz));
    const std::int64_t half = windowHz / 2;
    const std::int64_t bwHalf = ddcBandwidthHz / 2;
    const std::int64_t limit = windowHz != 0 ? half - bwHalf : INT32_MAX;
    // The lowest multiple of the step that is allowed, the highest, and the nearest to the wish.
    const auto clampFe = [&](std::int64_t fe) -> std::uint64_t {
        if (fe <= static_cast<std::int64_t>(minHz)) { return minHz; }
        std::uint64_t u = static_cast<std::uint64_t>(fe);
        u = (u / step) * step;
        if (u > maxHz) { u = (maxHz / step) * step; }
        if (u < minHz) { u = minHz; }
        return u;
    };
    const std::int64_t nearest = static_cast<std::int64_t>((static_cast<std::uint64_t>(c) + step / 2) / step * step);
    bool found = false;
    std::uint64_t best = 0;
    std::int64_t bestRel = 0;
    const std::int64_t offsets[] = {0, -static_cast<std::int64_t>(step), static_cast<std::int64_t>(step)};
    for (const std::int64_t off : offsets) {
        const std::uint64_t fe = clampFe(nearest + off);
        const std::int64_t rel = c - static_cast<std::int64_t>(fe);
        if (std::llabs(rel) > limit) { continue; }
        if (!found || std::llabs(rel) < std::llabs(bestRel)) {
            found = true;
            best = fe;
            bestRel = rel;
        }
    }
    if (!found || bestRel < INT32_MIN || bestRel > INT32_MAX) {
        char b[200];
        std::snprintf(b, sizeof b, "the receiver cannot centre on %.6f MHz: its front end tunes %.3f to %.3f MHz", centerHz / 1e6,
                      static_cast<double>(minHz) / 1e6, static_cast<double>(maxHz) / 1e6);
        why = b;
        return false;
    }
    out.frontEndHz = best;
    out.ddc1Hz = static_cast<std::int32_t>(bestRel);
    return true;
}

// ------------------------------------------------------------------------------------------------------------------------------------------
// The apartment (one thread for every library call) and the stream link are shared with the other WiNRADiO bridge: source/vendor_apartment.hpp.

namespace {

using vendorapt::Apartment;
using vendorapt::nowMs;

// Shared with the library's callback thread (see vendor_apartment.hpp).
struct Link : vendorapt::StreamLink {
    std::uint32_t channel = 0;
};

void FOX_G39_CALL ddc1Callback(std::uint32_t channel, const void* buffer, std::uint32_t n, std::uint32_t bits, std::uintptr_t user) {
    Link* l = reinterpret_cast<Link*>(user);
    if (l == nullptr) { return; }
    if (channel != l->channel) {
        l->countBad();
        return;
    }
    l->deliver(buffer, n, bits);
}

// What a task hands back. Held by shared_ptr and captured BY VALUE by every task: a library call that returns after its caller gave up
// (the apartment was abandoned) then writes into memory that still exists.
struct Out {
    bool ok = true;
    std::string e;
    std::string serial;
};

struct DdcType {
    std::uint32_t index = 0;
    std::uint32_t rate = 0;
    std::uint32_t bandwidth = 0;
};

}  // namespace

struct G39DdcSource::Impl {
    mutable std::mutex m;  // guards the fields below that the GUI and the pipeline both read
    vendorlib::Library lib;
    g39ddc::Api api;
    std::unique_ptr<Apartment> apt;
    Link* link = nullptr;
    bool linkLeaked = false;
    g39ddc::Handle h = -1;
    bool opened = false;
    bool streaming = false;
    bool dead = false;
    std::string error, deadWhile, verdict, serial;
    int callMs = kDefaultCallMs, silenceMs = kDefaultSilenceMs;
    g39ddc::DeviceInfo info{};
    std::vector<DdcType> types;
    std::uint32_t channel = 0;
    std::size_t typeAt = 0;
    double centerHz = 10.0e6;
    bool preamp = false, hasPreamp = false;
    std::int64_t startedMs = 0;
    std::int64_t lastLinkCheckMs = 0;

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
    // One library call (or a short run of them) on the apartment. False with the error set when it did not complete.
    bool call(const char* during, const std::function<void()>& fn) {
        if (!apt) {
            setError("the WiNRADiO receiver is not open");
            return false;
        }
        const auto r = apt->run(fn, callMs);
        switch (r) {
            case Apartment::Result::Done: return true;
            case Apartment::Result::TimedOut:
                markDead(during, std::string("the WiNRADiO library did not answer while ") + during + "; FoxSDR must be restarted to use this receiver again");
                break;
            case Apartment::Result::Faulted:
                markDead(during, std::string("the WiNRADiO library crashed while ") + during + " (FoxSDR caught it and carried on)");
                break;
            case Apartment::Result::Dead:
                setError("the WiNRADiO receiver has stopped answering; FoxSDR must be restarted to use it again");
                break;
        }
        return false;
    }
    std::string osErr(const char* what) const {
        char b[120];
        std::snprintf(b, sizeof b, "%s (Windows error %lu)", what, static_cast<unsigned long>(vendorlib::lastOsError()));
        return b;
    }
};

G39DdcSource::G39DdcSource() : p_(std::make_unique<Impl>()) {}
G39DdcSource::~G39DdcSource() { closeDevice(); }

const char* G39DdcSource::lastError() const {
    thread_local std::string copy;
    {
        std::lock_guard<std::mutex> lk(p_->m);
        copy = p_->error;
    }
    return copy.c_str();
}

bool G39DdcSource::isOpen() const {
    std::lock_guard<std::mutex> lk(p_->m);
    return p_->opened;
}
bool G39DdcSource::running() const {
    std::lock_guard<std::mutex> lk(p_->m);
    return p_->streaming;
}
bool G39DdcSource::deviceDead() const {
    std::lock_guard<std::mutex> lk(p_->m);
    return p_->dead;
}
bool G39DdcSource::faulted() const { return deviceDead(); }
std::string G39DdcSource::faultedWhile() const {
    std::lock_guard<std::mutex> lk(p_->m);
    return p_->deadWhile;
}
std::string G39DdcSource::openVerdictWord() const {
    std::lock_guard<std::mutex> lk(p_->m);
    return p_->verdict;
}
std::string G39DdcSource::serialMasked() const {
    std::lock_guard<std::mutex> lk(p_->m);
    if (p_->serial.size() <= 4) { return p_->serial.empty() ? std::string() : std::string("****"); }
    return std::string(p_->serial.size() - 4, '*') + p_->serial.substr(p_->serial.size() - 4);
}
G39DdcStats G39DdcSource::stats() const {
    G39DdcStats s;
    std::lock_guard<std::mutex> lk(p_->m);
    if (p_->link != nullptr) {
        s.callbacks = p_->link->callbacks.load();
        s.samplePairs = p_->link->pairs.load();
        s.pairsDropped = p_->link->dropped.load();
        s.badCallbacks = p_->link->bad.load();
    }
    return s;
}

bool G39DdcSource::open(const std::string& args) {
    Impl& I = *p_;
    if (isOpen()) { return true; }
    {
        std::lock_guard<std::mutex> lk(I.m);
        I.error.clear();
        I.verdict.clear();
        I.dead = false;
        I.deadWhile.clear();
    }
    const std::string libWhere = argValue(args, "lib");
    const std::string wantSerial = argValue(args, "serial");
    const long long ch = parseInt(argValue(args, "channel"), 0, 0, 1);
    const long long rate = parseInt(argValue(args, "rate"), static_cast<long long>(kPreferredRateHz), 1000, 100000000);
    const long long to = parseInt(argValue(args, "timeout"), kDefaultCallMs, 100, 120000);
    const long long sil = parseInt(argValue(args, "silence"), kDefaultSilenceMs, 200, 600000);
    if (ch < 0 || rate < 0 || to < 0 || sil < 0) {
        I.setError("the WiNRADiO settings are not valid (channel is 0 or 1; rate, timeout and silence are whole numbers in range)");
        return false;
    }
    I.callMs = static_cast<int>(to);
    I.silenceMs = static_cast<int>(sil);
    I.channel = static_cast<std::uint32_t>(ch);

    std::string why;
    const std::string path = vendorlib::findLibrary(libWhere, "G39DDCAPI.dll", why);
    if (path.empty()) {
        std::lock_guard<std::mutex> lk(I.m);
        I.verdict = vendorlib::verdictWord(vendorlib::Verdict::NotFound);
        I.error = why;
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
    for (const char* n : g39ddc::kRequired) { names.push_back(n); }
    for (const char* n : g39ddc::kOptional) { names.push_back(n); }
    std::vector<void*> fn;
    const std::string missing = vendorlib::resolveAll(I.lib, names, fn);
    // Only the required ones may be missing for a refusal.
    std::string missingRequired;
    for (std::size_t i = 0; i < std::size(g39ddc::kRequired); ++i) {
        if (fn[i] == nullptr) {
            if (!missingRequired.empty()) { missingRequired += ", "; }
            missingRequired += g39ddc::kRequired[i];
        }
    }
    (void)missing;
    if (!missingRequired.empty()) {
        I.lib.close();
        std::lock_guard<std::mutex> lk(I.m);
        I.verdict = vendorlib::verdictWord(vendorlib::Verdict::SymbolMissing);
        I.error = "G39DDCAPI.dll does not export what FoxSDR needs (" + missingRequired + "): it is not a G39DDC library this build knows";
        return false;
    }
    {
        std::size_t i = 0;
        I.api.GetDeviceList = reinterpret_cast<g39ddc::GetDeviceListFn>(fn[i++]);
        I.api.OpenDevice = reinterpret_cast<g39ddc::OpenDeviceFn>(fn[i++]);
        I.api.CloseDevice = reinterpret_cast<g39ddc::CloseDeviceFn>(fn[i++]);
        I.api.GetDeviceInfo = reinterpret_cast<g39ddc::GetDeviceInfoFn>(fn[i++]);
        I.api.SetPower = reinterpret_cast<g39ddc::SetPowerFn>(fn[i++]);
        I.api.SetFrontEndFrequency = reinterpret_cast<g39ddc::SetFrontEndFrequencyFn>(fn[i++]);
        I.api.GetDDCInfo = reinterpret_cast<g39ddc::GetDdcInfoFn>(fn[i++]);
        I.api.GetDDC1Count = reinterpret_cast<g39ddc::GetDdc1CountFn>(fn[i++]);
        I.api.SetDDC1 = reinterpret_cast<g39ddc::SetDdc1Fn>(fn[i++]);
        I.api.SetDDC1Frequency = reinterpret_cast<g39ddc::SetDdc1FrequencyFn>(fn[i++]);
        I.api.StartDDC1 = reinterpret_cast<g39ddc::StartDdc1Fn>(fn[i++]);
        I.api.StopDDC1 = reinterpret_cast<g39ddc::StopDdc1Fn>(fn[i++]);
        I.api.SetCallbacks = reinterpret_cast<g39ddc::SetCallbacksFn>(fn[i++]);
        I.api.IsDeviceConnected = reinterpret_cast<g39ddc::IsDeviceConnectedFn>(fn[i++]);
        I.api.SetPreamplifier = reinterpret_cast<g39ddc::SetPreamplifierFn>(fn[i++]);
    }
    I.hasPreamp = I.api.SetPreamplifier != nullptr;

    I.link = new Link();
    I.link->channel = I.channel;
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

    // 1. The list of receivers, and the proof the record layout is the one this build reads.
    auto o = std::make_shared<Out>();
    Impl* ip = &I;
    const g39ddc::Api api = I.api;
    if (!I.call("listing the receivers", [o, ip, api, wantSerial] {
            std::uint32_t count = 0, item = 0;
            if (!api.GetDeviceList(nullptr, 0, &count, &item)) {
                o->e = ip->osErr("the library could not list the receivers");
                o->ok = false;
                return;
            }
            if (count == 0) {
                o->e = "no WiNRADiO G39DDC receiver is connected";
                o->ok = false;
                return;
            }
            if (item != g39ddc::kDeviceInfoBytes || count > 64) {
                char b[260];
                std::snprintf(b, sizeof b,
                              "the library describes a receiver in %u bytes where this build of FoxSDR expects %u: it is a different version of the library, and FoxSDR will not read it",
                              static_cast<unsigned>(item), static_cast<unsigned>(g39ddc::kDeviceInfoBytes));
                o->e = b;
                o->ok = false;
                return;
            }
            std::vector<g39ddc::DeviceInfo> list(count);
            std::uint32_t got = 0, item2 = 0;
            if (!api.GetDeviceList(list.data(), static_cast<std::uint32_t>(list.size() * sizeof(g39ddc::DeviceInfo)), &got, &item2) || got == 0 || got > count) {
                o->e = ip->osErr("the library could not list the receivers");
                o->ok = false;
                return;
            }
            bool found = false;
            for (std::uint32_t i = 0; i < got; ++i) {
                const std::string s = cleanSerial(list[i].serialNumber, sizeof list[i].serialNumber);
                if (s.empty()) { continue; }
                if (wantSerial.empty() || s == wantSerial) {
                    o->serial = s;
                    found = true;
                    break;
                }
            }
            if (!found) {
                o->e = wantSerial.empty() ? "the library listed a receiver without a serial number" : "no connected G39DDC receiver has the serial number you gave";
                o->ok = false;
            }
        })) {
        return fail(I.error, "call-failed");
    }
    if (!o->ok) { return fail(o->e, "no-device"); }
    const std::string chosenSerial = o->serial;

    // 2. Open, power on, callbacks, DDC types.
    auto o2 = std::make_shared<Out>();
    const std::uint32_t channelC = I.channel;
    Link* linkC = I.link;
    if (!I.call("opening the receiver", [o2, ip, api, chosenSerial, channelC, linkC, rate] {
            const g39ddc::Handle h = api.OpenDevice(chosenSerial.c_str());
            if (h < 0) {
                o2->e = ip->osErr("the receiver could not be opened (is another program using it?)");
                o2->ok = false;
                return;
            }
            ip->h = h;
            if (!api.GetDeviceInfo(h, &ip->info, static_cast<std::uint32_t>(sizeof ip->info))) {
                o2->e = ip->osErr("the receiver did not describe itself");
                o2->ok = false;
                return;
            }
            if (!api.SetPower(h, 1)) {
                o2->e = ip->osErr("the receiver could not be powered on");
                o2->ok = false;
                return;
            }
            g39ddc::Callbacks cb;
            cb.ddc1StreamCallback = &ddc1Callback;
            if (!api.SetCallbacks(h, &cb, reinterpret_cast<std::uintptr_t>(linkC))) {
                o2->e = ip->osErr("the library refused the stream callback");
                o2->ok = false;
                return;
            }
            std::uint32_t count = 0;
            if (!api.GetDDC1Count(h, channelC, &count) || count == 0 || count > kMaxDdcTypes) {
                o2->e = "the receiver reported no usable sample-rate choices";
                o2->ok = false;
                return;
            }
            std::vector<DdcType> types;
            for (std::uint32_t i = 0; i < count; ++i) {
                g39ddc::DdcInfo di{};
                if (!api.GetDDCInfo(i, &di)) { continue; }
                if (di.sampleRate == 0 || (di.bitsPerSample != 16 && di.bitsPerSample != 32)) { continue; }
                types.push_back({i, di.sampleRate, di.bandwidth});
            }
            if (types.empty()) {
                o2->e = "the receiver offered no sample-rate choice this build can read";
                o2->ok = false;
                return;
            }
            std::size_t best = 0;
            for (std::size_t i = 1; i < types.size(); ++i) {
                if (std::fabs(static_cast<double>(types[i].rate) - static_cast<double>(rate)) <
                    std::fabs(static_cast<double>(types[best].rate) - static_cast<double>(rate))) {
                    best = i;
                }
            }
            if (!api.SetDDC1(h, channelC, types[best].index)) {
                o2->e = ip->osErr("the receiver refused the sample rate");
                o2->ok = false;
                return;
            }
            std::lock_guard<std::mutex> lk(ip->m);
            ip->types = std::move(types);
            ip->typeAt = best;
        })) {
        return fail(I.error, "call-failed");
    }
    if (!o2->ok) { return fail(o2->e, "open-failed"); }
    {
        std::lock_guard<std::mutex> lk(I.m);
        I.serial = chosenSerial;
        I.opened = true;
    }
    // 3. A first tuning inside the range.
    const double lo = static_cast<double>(I.info.frontEndMinFrequency), hi = static_cast<double>(I.info.frontEndMaxFrequency);
    I.centerHz = std::clamp(10.0e6, lo, hi);
    if (!setCenterFrequencyHz(I.centerHz)) { return fail(I.error, "tune-failed"); }
    return true;
}

void G39DdcSource::closeDevice() {
    Impl& I = *p_;
    bool abandoned = false;
    if (I.apt) {
        if (I.h >= 0 && !I.apt->dead()) {
            I.link->gate.store(false);
            const g39ddc::Api api = I.api;
            const g39ddc::Handle h = I.h;
            const std::uint32_t ch = I.channel;
            I.call("closing the receiver", [api, h, ch] {
                api.StopDDC1(h, ch);
                api.SetCallbacks(h, nullptr, 0);
                api.SetPower(h, 0);
                api.CloseDevice(h);
            });
        }
        abandoned = !I.apt->shutdown() || I.apt->dead();
    }
    if (abandoned) {
        // A library call never returned (or crashed). The vendor's thread may still be inside the library or writing to the link, so the whole
        // state - the mapped library, the link, the abandoned thread's shared data - is leaked for the life of the process, and this object
        // starts again from nothing (its next open() will say what happens). The receiver is marked dead first so the owner is told.
        {
            std::lock_guard<std::mutex> lk(I.m);
            I.dead = true;
        }
        Impl* leaked = p_.release();
        (void)leaked;
        p_ = std::make_unique<Impl>();
        p_->dead = true;
        p_->deadWhile = "closing the receiver";
        p_->error = "the WiNRADiO library did not answer; FoxSDR must be restarted to use this receiver again";
        return;
    }
    I.apt.reset();
    {
        std::lock_guard<std::mutex> lk(I.m);
        I.opened = false;
        I.streaming = false;
        I.h = -1;
        I.types.clear();
    }
    I.lib.close();
    delete I.link;
    I.link = nullptr;
}

bool G39DdcSource::start() {
    Impl& I = *p_;
    if (!isOpen()) {
        I.setError("the WiNRADiO receiver is not open");
        return false;
    }
    if (deviceDead()) { return false; }
    if (running()) { return true; }
    // Drop anything left from before, then open the gate before the library can call back.
    std::complex<float> junk[512];
    while (I.link->ring.read(junk, 512) > 0) {}
    I.link->gate.store(true, std::memory_order_release);
    auto o = std::make_shared<Out>();
    Impl* ip = &I;
    const g39ddc::Api api = I.api;
    const g39ddc::Handle h = I.h;
    const std::uint32_t ch = I.channel;
    if (!I.call("starting the stream", [o, ip, api, h, ch] {
            if (!api.StartDDC1(h, ch, kSamplesPerBuffer)) {
                o->ok = false;
                o->e = ip->osErr("the receiver would not start streaming");
            }
        })) {
        I.link->gate.store(false);
        return false;
    }
    if (!o->ok) {
        I.link->gate.store(false);
        I.setError(o->e);
        return false;
    }
    std::lock_guard<std::mutex> lk(I.m);
    I.streaming = true;
    I.startedMs = nowMs();
    I.link->lastDataMs.store(I.startedMs);
    return true;
}

void G39DdcSource::stop() {
    Impl& I = *p_;
    if (!running()) { return; }
    I.link->gate.store(false, std::memory_order_release);
    {
        const g39ddc::Api api = I.api;
        const g39ddc::Handle h = I.h;
        const std::uint32_t ch = I.channel;
        I.call("stopping the stream", [api, h, ch] { api.StopDDC1(h, ch); });
    }
    std::lock_guard<std::mutex> lk(I.m);
    I.streaming = false;
}

std::size_t G39DdcSource::read(std::complex<float>* dst, std::size_t n) {
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
        if (silent > 1000 && I.api.IsDeviceConnected != nullptr && nowMs() - I.lastLinkCheckMs > 1000) {
            I.lastLinkCheckMs = nowMs();
            auto gone = std::make_shared<std::atomic<bool>>(false);
            const g39ddc::Api api = I.api;
            const g39ddc::Handle h = I.h;
            if (I.call("checking the connection", [gone, api, h] {
                    // The document: zero with GetLastError() == ERROR_SUCCESS means the receiver is disconnected.
                    if (!api.IsDeviceConnected(h) && vendorlib::lastOsError() == 0) { gone->store(true); }
                }) &&
                gone->load()) {
                I.markDead("streaming", "the WiNRADiO receiver was disconnected");
            }
        } else if (silent > I.silenceMs) {
            I.markDead("streaming", "the WiNRADiO receiver stopped delivering samples");
        }
    }
    return got;
}

double G39DdcSource::sampleRateHz() const {
    std::lock_guard<std::mutex> lk(p_->m);
    return p_->types.empty() ? 0.0 : static_cast<double>(p_->types[p_->typeAt].rate);
}

std::vector<double> G39DdcSource::supportedSampleRatesHz() const {
    std::vector<double> r;
    std::lock_guard<std::mutex> lk(p_->m);
    for (const auto& t : p_->types) { r.push_back(static_cast<double>(t.rate)); }
    std::sort(r.begin(), r.end());
    r.erase(std::unique(r.begin(), r.end()), r.end());
    return r;
}

bool G39DdcSource::setSampleRateHz(double hz) {
    Impl& I = *p_;
    if (!isOpen() || deviceDead()) { return false; }
    if (!(hz > 0.0) || !std::isfinite(hz)) {
        I.setError("setSampleRateHz() requires a positive rate");
        return false;
    }
    std::size_t best = 0;
    {
        std::lock_guard<std::mutex> lk(I.m);
        for (std::size_t i = 1; i < I.types.size(); ++i) {
            if (std::fabs(I.types[i].rate - hz) < std::fabs(I.types[best].rate - hz)) { best = i; }
        }
        if (best == I.typeAt) { return true; }
    }
    const bool wasRunning = running();
    if (wasRunning) { stop(); }
    auto o = std::make_shared<Out>();
    Impl* ip = &I;
    const g39ddc::Api api = I.api;
    const g39ddc::Handle h = I.h;
    const std::uint32_t ch = I.channel;
    std::uint32_t typeIndex = 0;
    {
        std::lock_guard<std::mutex> lk(I.m);
        typeIndex = I.types[best].index;
    }
    if (!I.call("changing the sample rate", [o, ip, api, h, ch, typeIndex] {
            if (!api.SetDDC1(h, ch, typeIndex)) {
                o->ok = false;
                o->e = ip->osErr("the receiver refused that sample rate");
            }
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
        I.typeAt = best;
    }
    // The DDC's bandwidth changed: re-plan the tuning for the same centre.
    setCenterFrequencyHz(I.centerHz);
    return wasRunning ? start() : true;
}

double G39DdcSource::centerFrequencyHz() const {
    std::lock_guard<std::mutex> lk(p_->m);
    return p_->centerHz;
}

bool G39DdcSource::setCenterFrequencyHz(double hz) {
    Impl& I = *p_;
    if (!isOpen() || deviceDead()) { return false; }
    G39Tune t;
    std::string why;
    std::uint32_t bw = 0;
    {
        std::lock_guard<std::mutex> lk(I.m);
        bw = I.types.empty() ? 0 : I.types[I.typeAt].bandwidth;
    }
    if (!g39PlanTune(hz, I.info.frontEndMinFrequency, I.info.frontEndMaxFrequency, I.info.frontEndFrequencyStep, I.info.frontEndWindowWidth, bw, t, why)) {
        I.setError(why);
        return false;
    }
    auto o = std::make_shared<Out>();
    Impl* ip = &I;
    const g39ddc::Api api = I.api;
    const g39ddc::Handle h = I.h;
    const std::uint32_t ch = I.channel;
    if (!I.call("tuning", [o, ip, api, h, ch, t] {
            if (!api.SetFrontEndFrequency(h, t.frontEndHz)) {
                o->ok = false;
                o->e = ip->osErr("the receiver refused that front-end frequency");
                return;
            }
            if (!api.SetDDC1Frequency(h, ch, t.ddc1Hz)) {
                o->ok = false;
                o->e = ip->osErr("the receiver refused that offset");
            }
        })) {
        return false;
    }
    if (!o->ok) {
        I.setError(o->e);
        return false;
    }
    std::lock_guard<std::mutex> lk(I.m);
    I.centerHz = hz;
    return true;
}

std::vector<GainInfo> G39DdcSource::gains() const {
    std::vector<GainInfo> g;
    std::lock_guard<std::mutex> lk(p_->m);
    // The document defines the attenuator's values only by name (G39DDC_ATTENUATOR_*), their numbers being in the maker's header, so there is no
    // attenuator here. The preamplifier is a plain BOOL.
    if (p_->hasPreamp) { g.push_back({"Preamp", 0.0, 1.0, 1.0, GainUnit::Steps}); }
    return g;
}

bool G39DdcSource::setGainDb(const std::string& name, double db) {
    Impl& I = *p_;
    if (name != "Preamp" || !I.hasPreamp) {
        I.setError("this receiver has no gain called \"" + name + "\" here");
        return false;
    }
    if (!isOpen() || deviceDead()) { return false; }
    const bool on = db >= 0.5;
    auto o = std::make_shared<Out>();
    const g39ddc::Api api = I.api;
    const g39ddc::Handle h = I.h;
    if (!I.call("setting the preamplifier", [o, api, h, on] { o->ok = api.SetPreamplifier(h, on ? 1 : 0) != 0; })) { return false; }
    if (!o->ok) {
        I.setError(I.osErr("the receiver refused the preamplifier setting"));
        return false;
    }
    std::lock_guard<std::mutex> lk(I.m);
    I.preamp = on;
    return true;
}

double G39DdcSource::gainDb(const std::string& name) const {
    std::lock_guard<std::mutex> lk(p_->m);
    return name == "Preamp" && p_->preamp ? 1.0 : 0.0;
}

bool G39DdcSource::frequencyRangeHz(double& loHz, double& hiHz) const {
    std::lock_guard<std::mutex> lk(p_->m);
    if (!p_->opened) { return false; }
    const double w = p_->info.frontEndWindowWidth / 2.0;
    loHz = std::max(0.0, static_cast<double>(p_->info.frontEndMinFrequency) - w);
    hiHz = static_cast<double>(p_->info.frontEndMaxFrequency) + w;
    return true;
}

}  // namespace cascade::source
