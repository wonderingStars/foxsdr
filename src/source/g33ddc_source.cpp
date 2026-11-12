// g33ddc_source.cpp - see g33ddc_source.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/g33ddc_source.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <mutex>

#include "source/g33ddc_api.hpp"
#include "source/vendor_apartment.hpp"
#include "source/vendor_lib.hpp"

namespace cascade::source {

namespace {

using vendorapt::Apartment;
using vendorapt::nowMs;

constexpr int kDefaultCallMs = 5000;
constexpr int kDefaultSilenceMs = 5000;
constexpr std::uint32_t kSamplesPerBuffer = 4096;  // a multiple of 64, as the document requires
constexpr std::uint32_t kMaxDdcTypes = 64;
constexpr std::uint32_t kMaxDevices = 32;
constexpr std::size_t kListSlack = 4096;  // bytes after the list buffer that the library must leave alone
constexpr double kPreferredRateHz = 192000.0;

struct Link : vendorapt::StreamLink {};

void FOX_G33_CALL ddc1Callback(const void* buffer, std::uint32_t n, std::uint32_t bits, std::uintptr_t user) {
    Link* l = reinterpret_cast<Link*>(user);
    if (l == nullptr) { return; }
    l->deliver(buffer, n, bits);
}

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

long long parseInt(const std::string& v, long long dflt, long long lo, long long hi) {
    if (v.empty()) { return dflt; }
    char* end = nullptr;
    const long long x = std::strtoll(v.c_str(), &end, 10);
    if (end == v.c_str() || *end != '\0' || x < lo || x > hi) { return -1; }
    return x;
}

// A printable, bounded copy of a fixed-size, possibly unterminated field.
std::string cleanField(const char* s, std::size_t max) {
    std::string r;
    for (std::size_t i = 0; i < max && s[i] != '\0'; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x20 || c >= 0x7F) { break; }
        r += static_cast<char>(c);
    }
    return r;
}

// The document says these strings are null-terminated and the counts are small; anything else means the record was not written in the layout read.
bool plausibleRecord(const g33ddc::DeviceInfo& d) {
    if (std::memchr(d.devicePath, 0, sizeof d.devicePath) == nullptr) { return false; }
    if (std::memchr(d.serialNumber, 0, sizeof d.serialNumber) == nullptr) { return false; }
    if (cleanField(d.serialNumber, sizeof d.serialNumber).empty()) { return false; }
    if (d.channelCount == 0 || d.channelCount > 8) { return false; }
    if (d.ddcTypeCount == 0 || d.ddcTypeCount > kMaxDdcTypes) { return false; }
    return true;
}

}  // namespace

struct G33DdcSource::Impl {
    mutable std::mutex m;
    vendorlib::Library lib;
    g33ddc::Api api;
    std::unique_ptr<Apartment> apt;
    Link* link = nullptr;
    g33ddc::Handle h = -1;
    bool opened = false, streaming = false, dead = false;
    std::string error, deadWhile, verdict, serial;
    int callMs = kDefaultCallMs, silenceMs = kDefaultSilenceMs;
    std::vector<DdcType> types;
    std::size_t typeAt = 0;
    double centerHz = 0.0;
    bool preamp = false, hasPreamp = false;
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
    bool call(const char* during, const std::function<void()>& fn) {
        if (!apt) {
            setError("the WiNRADiO receiver is not open");
            return false;
        }
        switch (apt->run(fn, callMs)) {
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
    static std::string osErr(const char* what) {
        char b[160];
        std::snprintf(b, sizeof b, "%s (Windows error %lu)", what, static_cast<unsigned long>(vendorlib::lastOsError()));
        return b;
    }
};

G33DdcSource::G33DdcSource() : p_(std::make_unique<Impl>()) {}
G33DdcSource::~G33DdcSource() { closeDevice(); }

const char* G33DdcSource::lastError() const {
    thread_local std::string copy;
    {
        std::lock_guard<std::mutex> lk(p_->m);
        copy = p_->error;
    }
    return copy.c_str();
}
bool G33DdcSource::isOpen() const {
    std::lock_guard<std::mutex> lk(p_->m);
    return p_->opened;
}
bool G33DdcSource::running() const {
    std::lock_guard<std::mutex> lk(p_->m);
    return p_->streaming;
}
bool G33DdcSource::deviceDead() const {
    std::lock_guard<std::mutex> lk(p_->m);
    return p_->dead;
}
bool G33DdcSource::faulted() const { return deviceDead(); }
std::string G33DdcSource::faultedWhile() const {
    std::lock_guard<std::mutex> lk(p_->m);
    return p_->deadWhile;
}
std::string G33DdcSource::openVerdictWord() const {
    std::lock_guard<std::mutex> lk(p_->m);
    return p_->verdict;
}
std::string G33DdcSource::serialMasked() const {
    std::lock_guard<std::mutex> lk(p_->m);
    if (p_->serial.size() <= 4) { return p_->serial.empty() ? std::string() : std::string("****"); }
    return std::string(p_->serial.size() - 4, '*') + p_->serial.substr(p_->serial.size() - 4);
}
G33DdcStats G33DdcSource::stats() const {
    G33DdcStats s;
    std::lock_guard<std::mutex> lk(p_->m);
    if (p_->link != nullptr) {
        s.callbacks = p_->link->callbacks.load();
        s.samplePairs = p_->link->pairs.load();
        s.pairsDropped = p_->link->dropped.load();
        s.badCallbacks = p_->link->bad.load();
    }
    return s;
}

bool G33DdcSource::open(const std::string& args) {
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
    const long long rate = parseInt(argValue(args, "rate"), static_cast<long long>(kPreferredRateHz), 1000, 100000000);
    const long long to = parseInt(argValue(args, "timeout"), kDefaultCallMs, 100, 120000);
    const long long sil = parseInt(argValue(args, "silence"), kDefaultSilenceMs, 200, 600000);
    if (rate < 0 || to < 0 || sil < 0) {
        I.setError("the WiNRADiO settings are not valid (rate, timeout and silence are whole numbers in range)");
        return false;
    }
    I.callMs = static_cast<int>(to);
    I.silenceMs = static_cast<int>(sil);

    std::string why;
    const std::string path = vendorlib::findLibrary(libWhere, "G33DDCAPI.dll", why);
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
    for (const char* n : g33ddc::kRequired) { names.push_back(n); }
    for (const char* n : g33ddc::kOptional) { names.push_back(n); }
    std::vector<void*> fn;
    vendorlib::resolveAll(I.lib, names, fn);
    std::string missing;
    for (std::size_t i = 0; i < std::size(g33ddc::kRequired); ++i) {
        if (fn[i] == nullptr) {
            if (!missing.empty()) { missing += ", "; }
            missing += g33ddc::kRequired[i];
        }
    }
    if (!missing.empty()) {
        I.lib.close();
        std::lock_guard<std::mutex> lk(I.m);
        I.verdict = vendorlib::verdictWord(vendorlib::Verdict::SymbolMissing);
        I.error = "G33DDCAPI.dll does not export what FoxSDR needs (" + missing + "): it is not a G33DDC library this build knows";
        return false;
    }
    {
        std::size_t i = 0;
        I.api.GetDeviceList = reinterpret_cast<g33ddc::GetDeviceListFn>(fn[i++]);
        I.api.OpenDevice = reinterpret_cast<g33ddc::OpenDeviceFn>(fn[i++]);
        I.api.CloseDevice = reinterpret_cast<g33ddc::CloseDeviceFn>(fn[i++]);
        I.api.GetDeviceInfo = reinterpret_cast<g33ddc::GetDeviceInfoFn>(fn[i++]);
        I.api.SetPower = reinterpret_cast<g33ddc::SetPowerFn>(fn[i++]);
        I.api.GetDDCInfo = reinterpret_cast<g33ddc::GetDdcInfoFn>(fn[i++]);
        I.api.GetDDC1Count = reinterpret_cast<g33ddc::GetDdc1CountFn>(fn[i++]);
        I.api.SetDDC1 = reinterpret_cast<g33ddc::SetDdc1Fn>(fn[i++]);
        I.api.SetDDC1Frequency = reinterpret_cast<g33ddc::SetDdc1FrequencyFn>(fn[i++]);
        I.api.StartDDC1 = reinterpret_cast<g33ddc::StartDdc1Fn>(fn[i++]);
        I.api.StopDDC1 = reinterpret_cast<g33ddc::StopDdc1Fn>(fn[i++]);
        I.api.SetCallbacks = reinterpret_cast<g33ddc::SetCallbacksFn>(fn[i++]);
        I.api.IsDeviceConnected = reinterpret_cast<g33ddc::IsDeviceConnectedFn>(fn[i++]);
        I.api.SetPreamp = reinterpret_cast<g33ddc::SetPreampFn>(fn[i++]);
    }
    I.hasPreamp = I.api.SetPreamp != nullptr;
    I.link = new Link();
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

    // 1. The list, read into a buffer with slack and a sentinel; every record must be plausible.
    auto o = std::make_shared<Out>();
    Impl* ip = &I;
    const g33ddc::Api api = I.api;
    if (!I.call("listing the receivers", [o, api, wantSerial] {
            const std::int32_t count = api.GetDeviceList(nullptr, 0);
            if (count < 0) {
                o->e = Impl::osErr("the library could not list the receivers");
                o->ok = false;
                return;
            }
            if (count == 0) {
                o->e = "no WiNRADiO G33DDC receiver is connected";
                o->ok = false;
                return;
            }
            if (static_cast<std::uint32_t>(count) > kMaxDevices) {
                o->e = "the library reports an impossible number of receivers; FoxSDR will not read its list";
                o->ok = false;
                return;
            }
            const std::size_t listBytes = static_cast<std::size_t>(count) * sizeof(g33ddc::DeviceInfo);
            std::vector<unsigned char> raw(listBytes + kListSlack, 0xA5);
            const std::int32_t got = api.GetDeviceList(reinterpret_cast<g33ddc::DeviceInfo*>(raw.data()), static_cast<std::uint32_t>(listBytes));
            bool intact = true;
            for (std::size_t i = listBytes; i < raw.size(); ++i) { intact = intact && raw[i] == 0xA5; }
            if (!intact) {
                o->e = "the library wrote past the end of the device list FoxSDR gave it: it is a different version of the library, and FoxSDR will not use it";
                o->ok = false;
                return;
            }
            if (got != count) {
                o->e = Impl::osErr("the library could not list the receivers");
                o->ok = false;
                return;
            }
            bool found = false;
            for (std::int32_t i = 0; i < got; ++i) {
                g33ddc::DeviceInfo d;
                std::memcpy(&d, raw.data() + static_cast<std::size_t>(i) * sizeof d, sizeof d);
                if (!plausibleRecord(d)) {
                    o->e = "the library describes its receivers in a layout FoxSDR does not recognise: it is a different version of the library, and FoxSDR will not read it";
                    o->ok = false;
                    return;
                }
                const std::string s = cleanField(d.serialNumber, sizeof d.serialNumber);
                if (!found && (wantSerial.empty() || s == wantSerial)) {
                    o->serial = s;
                    found = true;
                }
            }
            if (!found) {
                o->e = "no connected G33DDC receiver has the serial number you gave";
                o->ok = false;
            }
        })) {
        return fail(I.error, "call-failed");
    }
    if (!o->ok) { return fail(o->e, "no-device"); }
    const std::string chosenSerial = o->serial;

    // 2. Open, power on, callback, DDC types.
    auto o2 = std::make_shared<Out>();
    Link* linkC = I.link;
    if (!I.call("opening the receiver", [o2, ip, api, chosenSerial, linkC, rate] {
            const g33ddc::Handle h = api.OpenDevice(chosenSerial.c_str());
            if (h < 0) {
                o2->e = Impl::osErr("the receiver could not be opened (is another program using it?)");
                o2->ok = false;
                return;
            }
            ip->h = h;
            if (!api.SetPower(h, 1)) {
                o2->e = Impl::osErr("the receiver could not be powered on");
                o2->ok = false;
                return;
            }
            g33ddc::Callbacks cb;
            cb.ddc1StreamCallback = &ddc1Callback;
            if (!api.SetCallbacks(h, &cb, reinterpret_cast<std::uintptr_t>(linkC))) {
                o2->e = Impl::osErr("the library refused the stream callback");
                o2->ok = false;
                return;
            }
            std::uint32_t count = 0;
            if (!api.GetDDC1Count(h, &count) || count == 0 || count > kMaxDdcTypes) {
                o2->e = "the receiver reported no usable sample-rate choices";
                o2->ok = false;
                return;
            }
            std::vector<DdcType> types;
            for (std::uint32_t i = 0; i < count; ++i) {
                g33ddc::DdcInfo di{};
                if (!api.GetDDCInfo(h, i, &di)) { continue; }
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
            if (!api.SetDDC1(h, types[best].index)) {
                o2->e = Impl::osErr("the receiver refused the sample rate");
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
    // 3. A first tuning, best effort: the document gives no range, so a refusal here is not an open failure.
    if (!setCenterFrequencyHz(10.0e6)) {
        if (deviceDead()) { return fail(I.error, "tune-failed"); }
        I.setError("");
    }
    return true;
}

void G33DdcSource::closeDevice() {
    Impl& I = *p_;
    bool abandoned = false;
    if (I.apt) {
        if (I.h >= 0 && !I.apt->dead()) {
            I.link->gate.store(false);
            const g33ddc::Api api = I.api;
            const g33ddc::Handle h = I.h;
            I.call("closing the receiver", [api, h] {
                api.StopDDC1(h);
                api.SetCallbacks(h, nullptr, 0);
                api.SetPower(h, 0);
                api.CloseDevice(h);
            });
        }
        abandoned = !I.apt->shutdown() || I.apt->dead();
    }
    if (abandoned) {
        // See G39DdcSource::closeDevice: the module, the link and the thread's shared data are leaked for the life of the process.
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

bool G33DdcSource::start() {
    Impl& I = *p_;
    if (!isOpen()) {
        I.setError("the WiNRADiO receiver is not open");
        return false;
    }
    if (deviceDead()) { return false; }
    if (running()) { return true; }
    std::complex<float> junk[512];
    while (I.link->ring.read(junk, 512) > 0) {}
    I.link->gate.store(true, std::memory_order_release);
    auto o = std::make_shared<Out>();
    const g33ddc::Api api = I.api;
    const g33ddc::Handle h = I.h;
    if (!I.call("starting the stream", [o, api, h] {
            if (!api.StartDDC1(h, kSamplesPerBuffer)) {
                o->ok = false;
                o->e = Impl::osErr("the receiver would not start streaming");
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
    I.link->lastDataMs.store(nowMs());
    return true;
}

void G33DdcSource::stop() {
    Impl& I = *p_;
    if (!running()) { return; }
    I.link->gate.store(false, std::memory_order_release);
    const g33ddc::Api api = I.api;
    const g33ddc::Handle h = I.h;
    I.call("stopping the stream", [api, h] { api.StopDDC1(h); });
    std::lock_guard<std::mutex> lk(I.m);
    I.streaming = false;
}

std::size_t G33DdcSource::read(std::complex<float>* dst, std::size_t n) {
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
            const g33ddc::Api api = I.api;
            const g33ddc::Handle h = I.h;
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

double G33DdcSource::sampleRateHz() const {
    std::lock_guard<std::mutex> lk(p_->m);
    return p_->types.empty() ? 0.0 : static_cast<double>(p_->types[p_->typeAt].rate);
}

std::vector<double> G33DdcSource::supportedSampleRatesHz() const {
    std::vector<double> r;
    std::lock_guard<std::mutex> lk(p_->m);
    for (const auto& t : p_->types) { r.push_back(static_cast<double>(t.rate)); }
    std::sort(r.begin(), r.end());
    r.erase(std::unique(r.begin(), r.end()), r.end());
    return r;
}

bool G33DdcSource::setSampleRateHz(double hz) {
    Impl& I = *p_;
    if (!isOpen() || deviceDead()) { return false; }
    if (!(hz > 0.0) || !std::isfinite(hz)) {
        I.setError("setSampleRateHz() requires a positive rate");
        return false;
    }
    std::size_t best = 0;
    std::uint32_t typeIndex = 0;
    {
        std::lock_guard<std::mutex> lk(I.m);
        for (std::size_t i = 1; i < I.types.size(); ++i) {
            if (std::fabs(I.types[i].rate - hz) < std::fabs(I.types[best].rate - hz)) { best = i; }
        }
        if (best == I.typeAt) { return true; }
        typeIndex = I.types[best].index;
    }
    const bool wasRunning = running();
    if (wasRunning) { stop(); }
    auto o = std::make_shared<Out>();
    const g33ddc::Api api = I.api;
    const g33ddc::Handle h = I.h;
    if (!I.call("changing the sample rate", [o, api, h, typeIndex] {
            if (!api.SetDDC1(h, typeIndex)) {
                o->ok = false;
                o->e = Impl::osErr("the receiver refused that sample rate");
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
    return wasRunning ? start() : true;
}

double G33DdcSource::centerFrequencyHz() const {
    std::lock_guard<std::mutex> lk(p_->m);
    return p_->centerHz;
}

bool G33DdcSource::setCenterFrequencyHz(double hz) {
    Impl& I = *p_;
    if (!isOpen() || deviceDead()) { return false; }
    if (!std::isfinite(hz) || hz < 0.0 || hz > 4294967295.0) {
        I.setError("that is not a frequency the receiver can use");
        return false;
    }
    const std::uint32_t f = static_cast<std::uint32_t>(std::llround(hz));
    auto o = std::make_shared<Out>();
    const g33ddc::Api api = I.api;
    const g33ddc::Handle h = I.h;
    if (!I.call("tuning", [o, api, h, f] {
            if (!api.SetDDC1Frequency(h, f)) {
                o->ok = false;
                o->e = Impl::osErr("the receiver refused that frequency");
            }
        })) {
        return false;
    }
    if (!o->ok) {
        I.setError(o->e);
        return false;
    }
    std::lock_guard<std::mutex> lk(I.m);
    I.centerHz = static_cast<double>(f);
    return true;
}

std::vector<GainInfo> G33DdcSource::gains() const {
    std::vector<GainInfo> g;
    std::lock_guard<std::mutex> lk(p_->m);
    // The attenuator's values are defined only by name in the document, so there is no attenuator here; the preamplifier is a plain BOOL.
    if (p_->hasPreamp) { g.push_back({"Preamp", 0.0, 1.0, 1.0, GainUnit::Steps}); }
    return g;
}

bool G33DdcSource::setGainDb(const std::string& name, double db) {
    Impl& I = *p_;
    if (name != "Preamp" || !I.hasPreamp) {
        I.setError("this receiver has no gain called \"" + name + "\" here");
        return false;
    }
    if (!isOpen() || deviceDead()) { return false; }
    const bool on = db >= 0.5;
    auto o = std::make_shared<Out>();
    const g33ddc::Api api = I.api;
    const g33ddc::Handle h = I.h;
    if (!I.call("setting the preamplifier", [o, api, h, on] { o->ok = api.SetPreamp(h, on ? 1 : 0) != 0; })) { return false; }
    if (!o->ok) {
        I.setError(Impl::osErr("the receiver refused the preamplifier setting"));
        return false;
    }
    std::lock_guard<std::mutex> lk(I.m);
    I.preamp = on;
    return true;
}

double G33DdcSource::gainDb(const std::string& name) const {
    std::lock_guard<std::mutex> lk(p_->m);
    return name == "Preamp" && p_->preamp ? 1.0 : 0.0;
}

}  // namespace cascade::source
