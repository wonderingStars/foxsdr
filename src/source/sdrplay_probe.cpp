// sdrplay_probe.cpp - see sdrplay_probe.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/sdrplay_probe.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include "core/diag_log.hpp"
#include "core/telemetry.hpp"
#include "core/utf8_text.hpp"
#include "core/version.hpp"
#include "source/sdrplay_source.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif !defined(__ANDROID__)
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace cascade::source {
namespace abi = sdrplay_abi;

const char* probeStatusName(ProbeStatus s) {
    switch (s) {
        case ProbeStatus::Pass: return "PASS";
        case ProbeStatus::Slow: return "SLOW";
        case ProbeStatus::Hung: return "HUNG";
        case ProbeStatus::Error: return "ERROR";
        case ProbeStatus::NotRun: return "NOT RUN";
        case ProbeStatus::Skipped: return "SKIPPED";
    }
    return "?";
}

std::string sdrPlayProbeSerialHash(const std::string& serial) {
    std::uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : serial) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return std::string(buf);
}

namespace {

using Clock = std::chrono::steady_clock;

std::int64_t nowNs() {
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
            .count());
}

// "13:10:05.123" in UTC - the wall time of a call, so a report can be laid
// beside the service's own log and the diagnostics log line by line.
std::string wallNow() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             now.time_since_epoch())
                             .count() %
                         1000;
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03lld", tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
    return std::string(buf);
}

std::string dateNowUtc() {
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d UTC", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string(buf);
}

// No fixed buffer: a report line (a parameter-block dump, a long refusal) can
// run past any size picked in advance, and a cut line is a wrong line.
std::string fmt(const char* f, ...) {
    std::string out;
    va_list ap;
    va_start(ap, f);
    core::vformatUtf8(out, f, ap);
    va_end(ap);
    return out;
}

std::string errName(const abi::Api& api, abi::ErrT err) {
    const char* s = (api.GetErrorString != nullptr) ? api.GetErrorString(err) : nullptr;
    return fmt("%s (%d)", (s != nullptr) ? s : "error", static_cast<int>(err));
}

// EVERYTHING THE SERVICE'S THREADS TOUCH, held by shared_ptr so an abandoned
// worker or a late callback can never outlive it. Counters only on the stream
// thread; the event thread formats one short line under a mutex nobody holds
// for longer than a push_back.
struct ProbeLink {
    std::int64_t startNs = 0;
    std::atomic<std::uint64_t> samples{0};
    std::atomic<std::uint64_t> callbacks{0};
    std::atomic<std::uint64_t> resets{0};
    std::atomic<std::int64_t> lastCbNs{0};
    std::atomic<std::int64_t> longestGapNs{0};
    std::atomic<int> grChanged{0};
    std::atomic<int> rfChanged{0};
    std::atomic<int> fsChanged{0};
    std::mutex eventMutex;
    std::vector<std::string> events;
    // The last gain event, for the LNA table.
    std::atomic<int> lastLnaGr{-1};
    std::atomic<int> lastGr{-1};
    std::atomic<double> lastGain{0.0};
};

void probeStreamA(short* xi, short* xq, abi::StreamCbParamsT* params, unsigned int numSamples,
                  unsigned int reset, void* ctx) {
    (void) xi;
    (void) xq;
    ProbeLink* link = static_cast<ProbeLink*>(ctx);
    if (link == nullptr) { return; }
    const std::int64_t now = nowNs();
    const std::int64_t prev = link->lastCbNs.exchange(now, std::memory_order_relaxed);
    if (prev != 0 && now > prev) {
        const std::int64_t gap = now - prev;
        std::int64_t seen = link->longestGapNs.load(std::memory_order_relaxed);
        while (gap > seen &&
               !link->longestGapNs.compare_exchange_weak(seen, gap, std::memory_order_relaxed)) {
        }
    }
    link->callbacks.fetch_add(1, std::memory_order_relaxed);
    link->samples.fetch_add(numSamples, std::memory_order_relaxed);
    if (reset != 0) { link->resets.fetch_add(1, std::memory_order_relaxed); }
    if (params != nullptr) {
        if (params->grChanged != 0) { link->grChanged.store(1, std::memory_order_relaxed); }
        if (params->rfChanged != 0) { link->rfChanged.store(1, std::memory_order_relaxed); }
        if (params->fsChanged != 0) { link->fsChanged.store(1, std::memory_order_relaxed); }
    }
}

void probeStreamB(short*, short*, abi::StreamCbParamsT*, unsigned int, unsigned int, void*) {}

void probeEvent(abi::EventT id, abi::TunerSelectT tuner, abi::EventParamsT* params, void* ctx) {
    ProbeLink* link = static_cast<ProbeLink*>(ctx);
    if (link == nullptr) { return; }
    const double rel = static_cast<double>(nowNs() - link->startNs) / 1e9;
    std::string line;
    switch (id) {
        case abi::GainChange:
            if (params != nullptr) {
                link->lastGr.store(static_cast<int>(params->gainParams.gRdB));
                link->lastLnaGr.store(static_cast<int>(params->gainParams.lnaGRdB));
                link->lastGain.store(params->gainParams.currGain);
                line = fmt("+%8.3f %s GainChange tuner %d gRdB %u lnaGRdB %u currGain %.2f", rel,
                           wallNow().c_str(), static_cast<int>(tuner), params->gainParams.gRdB,
                           params->gainParams.lnaGRdB, params->gainParams.currGain);
            }
            break;
        case abi::PowerOverloadChange:
            line = fmt("+%8.3f %s PowerOverloadChange tuner %d %s", rel, wallNow().c_str(),
                       static_cast<int>(tuner),
                       (params != nullptr && params->powerOverloadParams.powerOverloadChangeType ==
                                                 abi::Overload_Detected)
                           ? "DETECTED"
                           : "corrected");
            break;
        case abi::DeviceRemoved:
            line = fmt("+%8.3f %s DeviceRemoved", rel, wallNow().c_str());
            break;
        case abi::RspDuoModeChange:
            line = fmt("+%8.3f %s RspDuoModeChange %d", rel, wallNow().c_str(),
                       params != nullptr ? static_cast<int>(params->rspDuoModeParams.modeChangeType)
                                         : -1);
            break;
        case abi::DeviceFailure:
            line = fmt("+%8.3f %s DeviceFailure", rel, wallNow().c_str());
            break;
        default: line = fmt("+%8.3f %s event %d", rel, wallNow().c_str(), static_cast<int>(id)); break;
    }
    if (line.empty()) { return; }
    std::lock_guard<std::mutex> lk(link->eventMutex);
    if (link->events.size() < 2000) { link->events.push_back(std::move(line)); }
}

// What one vendor call came to.
struct CallResult {
    bool made = false;      // false when the probe had already stopped calling
    bool finished = false;  // false when it was still inside at the limit
    abi::ErrT err = abi::Fail;
    long long ms = 0;
    bool ok() const { return made && finished && err == abi::Success; }
};

struct Answer {
    abi::ErrT err = abi::Fail;
    std::int64_t retNs = 0;
};

class Probe {
public:
    Probe(const abi::Api& api, const SdrPlayProbeOptions& opt) : api_(api), opt_(opt) {
        link_ = std::make_shared<ProbeLink>();
        link_->startNs = nowNs();
        devParamsOut_ = std::make_shared<abi::DeviceParamsT*>(nullptr);
        dev_ = std::make_shared<abi::DeviceT>();
        devs_ = std::make_shared<std::vector<abi::DeviceT>>(abi::kMaxDevices);
        numDevs_ = std::make_shared<unsigned int>(0);
        ver_ = std::make_shared<float>(0.0f);
        const char* names[] = {"open and start", "sample rates", "frequencies", "LNA states",
                               "antennas and HDR", "bias tee", "IF modes", "uninit and close"};
        for (int i = 0; i < 8; ++i) {
            SdrPlayProbeStep s;
            s.number = i + 1;
            s.name = names[i];
            steps_.push_back(s);
        }
    }

    SdrPlayProbeResult run();

private:
    // --- the timed call -----------------------------------------------------
    CallResult call(const char* name, const std::string& args, std::function<abi::ErrT()> fn);

    // --- step bookkeeping ---------------------------------------------------
    // `teardown` is step 8's: it runs whatever the earlier steps left open,
    // so a stop earlier in the run does not make it NOT RUN by itself.
    void beginStep(int n, bool teardown = false) {
        cur_ = n - 1;
        detail_ += fmt("\n[%d %s]\n", n, steps_[cur_].name.c_str());
        // A step the run never reached says so, rather than PASS.
        if (stopped_ && !teardown) {
            steps_[cur_].status = ProbeStatus::NotRun;
            steps_[cur_].detail = hung_           ? "an earlier call is still inside the SDRplay API"
                                  : serviceGone_   ? "the service stopped answering"
                                  : cancelledFlag_ ? "cancelled"
                                                   : "an earlier step did not complete";
            line("not run - " + steps_[cur_].detail);
            return;
        }
        steps_[cur_].status = ProbeStatus::Pass;
        if (opt_.progress) { opt_.progress(fmt("%d/8 %s", n, steps_[cur_].name.c_str())); }
    }
    static int rank(ProbeStatus s) {
        switch (s) {
            case ProbeStatus::Pass: return 0;
            case ProbeStatus::Skipped: return 0;
            case ProbeStatus::Slow: return 1;
            case ProbeStatus::Error: return 2;
            case ProbeStatus::Hung: return 3;
            case ProbeStatus::NotRun: return -1;
        }
        return 0;
    }
    void mark(ProbeStatus s, const std::string& why) {
        if (cur_ < 0) { return; }
        SdrPlayProbeStep& st = steps_[cur_];
        if (rank(s) > rank(st.status)) {
            st.status = s;
            st.detail = why;
        }
    }
    void line(const std::string& s) { detail_ += "  " + s + "\n"; }
    bool stopped() const { return stopped_; }
    bool cancelled() const {
        return opt_.cancel != nullptr && opt_.cancel->load(std::memory_order_relaxed);
    }

    // --- the radio ----------------------------------------------------------
    abi::RxChannelParamsT* ch() const {
        abi::DeviceParamsT* p = *devParamsOut_;
        if (p == nullptr) { return nullptr; }
        return (dev_->tuner == abi::Tuner_B) ? p->rxChannelB : p->rxChannelA;
    }
    abi::DevParamsT* dp() const {
        abi::DeviceParamsT* p = *devParamsOut_;
        return (p != nullptr) ? p->devParams : nullptr;
    }
    CallResult update(abi::ReasonForUpdateT r, abi::ReasonForUpdateExt1T e, const char* why);
    // Waits up to 500 ms for a changed flag, as the receiver does; returns
    // the milliseconds it took, or -1.
    long long waitFlag(std::atomic<int>& flag);
    // Streams for `d` and reports what arrived; `expectRate` 0 means "no
    // expectation, just say what came".
    void measure(std::chrono::milliseconds d, double expectRate, const std::string& label,
                 bool judgeRate);
    void dumpBlocks(const char* when);
    void knownState();
    bool hasBiasTee() const;
    unsigned char hw() const { return dev_->hwVer; }

    void stepOpen();
    void stepRates();
    void stepFrequencies();
    void stepLna();
    void stepAntennas();
    void stepBias();
    void stepIfModes();
    void stepClose();

    const abi::Api& api_;
    SdrPlayProbeOptions opt_;
    std::shared_ptr<ProbeLink> link_;
    std::shared_ptr<abi::DeviceParamsT*> devParamsOut_;
    std::shared_ptr<abi::DeviceT> dev_;
    std::shared_ptr<std::vector<abi::DeviceT>> devs_;
    std::shared_ptr<unsigned int> numDevs_;
    std::shared_ptr<float> ver_;
    std::vector<SdrPlayProbeStep> steps_;
    int cur_ = -1;
    std::string detail_;
    bool stopped_ = false;
    bool hung_ = false;
    bool cancelledFlag_ = false;
    bool serviceGone_ = false;
    bool opened_ = false;
    bool locked_ = false;
    bool selected_ = false;
    bool initialised_ = false;
    int calls_ = 0;
};

CallResult Probe::call(const char* name, const std::string& args, std::function<abi::ErrT()> fn) {
    CallResult r;
    if (stopped_) { return r; }
    r.made = true;
    ++calls_;
    const std::int64_t entryNs = nowNs();
    const std::string entryWall = wallNow();
    auto promise = std::make_shared<std::promise<Answer>>();
    std::future<Answer> fut = promise->get_future();
    // THE CALL ON A WORKER, AND THE WORKER OWNS EVERYTHING IT TOUCHES: `fn`
    // captures only values and shared_ptrs, so if the probe gives up on it
    // (the limit, or a cancel) it can finish - or not - with nothing freed
    // under it.
    std::thread worker([promise, fn = std::move(fn)]() {
        Answer a;
        a.err = fn();
        a.retNs = nowNs();
        promise->set_value(a);
    });
    const std::int64_t limitNs =
        static_cast<std::int64_t>(opt_.callLimit.count()) * 1000000LL;
    bool finished = false;
    for (;;) {
        if (fut.wait_for(std::chrono::milliseconds(10)) == std::future_status::ready) {
            finished = true;
            break;
        }
        if (nowNs() - entryNs >= limitNs) { break; }
        if (cancelled()) {
            cancelledFlag_ = true;
            break;
        }
    }
    const double rel = static_cast<double>(entryNs - link_->startNs) / 1e9;
    if (!finished) {
        worker.detach();
        stopped_ = true;
        const long long insideMs = (nowNs() - entryNs) / 1000000;
        r.ms = insideMs;
        if (cancelledFlag_) {
            line(fmt("+%8.3f %s -> sdrplay_api_%s(%s) | still inside after %lld ms when the run "
                     "was cancelled",
                     rel, entryWall.c_str(), name, args.c_str(), insideMs));
            mark(ProbeStatus::Error, fmt("cancelled inside %s", name));
        } else {
            hung_ = true;
            line(fmt("+%8.3f %s -> sdrplay_api_%s(%s) | STILL INSIDE after %lld ms - HUNG; no "
                     "further SDRplay API call is made",
                     rel, entryWall.c_str(), name, args.c_str(), insideMs));
            mark(ProbeStatus::Hung, fmt("%s still inside after %lld ms", name, insideMs));
        }
        return r;
    }
    worker.join();
    const Answer a = fut.get();
    r.finished = true;
    r.err = a.err;
    r.ms = (a.retNs - entryNs) / 1000000;
    const bool slow = r.ms > static_cast<long long>(opt_.slowCall.count());
    line(fmt("+%8.3f %s -> sdrplay_api_%s(%s) | %s <- %s in %lld ms%s", rel, entryWall.c_str(),
             name, args.c_str(), wallNow().c_str(), errName(api_, a.err).c_str(), r.ms,
             slow ? "  SLOW" : ""));
    if (a.err != abi::Success) {
        mark(ProbeStatus::Error, fmt("%s(%s) -> %s", name, args.c_str(), errName(api_, a.err).c_str()));
        if (a.err == abi::ServiceNotResponding) {
            // The service has said it is gone; every further call would only
            // take its own time to say the same.
            serviceGone_ = true;
            stopped_ = true;
            line("the service answered sdrplay_api_ServiceNotResponding - no further SDRplay API "
                 "call is made");
        }
    } else if (slow) {
        mark(ProbeStatus::Slow, fmt("%s(%s) took %lld ms", name, args.c_str(), r.ms));
    }
    return r;
}

std::string reasonText(abi::ReasonForUpdateT r, abi::ReasonForUpdateExt1T e) {
    return fmt("0x%08x, 0x%08x", static_cast<unsigned>(r), static_cast<unsigned>(e));
}

CallResult Probe::update(abi::ReasonForUpdateT r, abi::ReasonForUpdateExt1T e, const char* why) {
    if ((r & abi::Update_Tuner_Gr) != 0) { link_->grChanged.store(0); }
    if ((r & abi::Update_Tuner_Frf) != 0) { link_->rfChanged.store(0); }
    if ((r & abi::Update_Dev_Fs) != 0) { link_->fsChanged.store(0); }
    const abi::Api* table = &api_;
    void* dev = dev_->dev;
    const abi::TunerSelectT tuner = dev_->tuner;
    return call("Update", reasonText(r, e) + " " + why,
                [table, dev, tuner, r, e]() { return table->Update(dev, tuner, r, e); });
}

long long Probe::waitFlag(std::atomic<int>& flag) {
    const auto t0 = Clock::now();
    while (Clock::now() - t0 < std::chrono::milliseconds(500)) {
        if (flag.load() != 0) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
        }
        if (cancelled()) { return -1; }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return -1;
}

void Probe::measure(std::chrono::milliseconds d, double expectRate, const std::string& label,
                    bool judgeRate) {
    if (stopped_) { return; }
    link_->samples.store(0);
    link_->callbacks.store(0);
    link_->resets.store(0);
    link_->longestGapNs.store(0);
    const auto t0 = Clock::now();
    std::uint64_t lastCallbacks = 0;
    int silentSlices = 0;
    const auto end = t0 + d;
    while (Clock::now() < end) {
        if (cancelled()) {
            stopped_ = true;
            cancelledFlag_ = true;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const std::uint64_t cb = link_->callbacks.load();
        if (cb == lastCallbacks) { ++silentSlices; }
        lastCallbacks = cb;
    }
    const double secs =
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count() / 1e6;
    const std::uint64_t samples = link_->samples.load();
    const double delivered = secs > 0.0 ? static_cast<double>(samples) / secs : 0.0;
    std::string s = fmt("%s: delivered %.0f S/s over %.2f s", label.c_str(), delivered, secs);
    if (expectRate > 0.0) {
        s += fmt(" (%.1f%% of %.0f)", 100.0 * delivered / expectRate, expectRate);
    }
    s += fmt(", callbacks %llu, longest gap %lld ms, silent 20 ms slices %d, resets %llu",
             static_cast<unsigned long long>(link_->callbacks.load()),
             static_cast<long long>(link_->longestGapNs.load() / 1000000), silentSlices,
             static_cast<unsigned long long>(link_->resets.load()));
    line(s);
    if (samples == 0) {
        mark(ProbeStatus::Error, label + ": no samples at all");
    } else if (judgeRate && expectRate > 0.0 && std::fabs(delivered / expectRate - 1.0) > 0.25) {
        mark(ProbeStatus::Error,
             fmt("%s: delivered %.0f%% of the rate set", label.c_str(), 100.0 * delivered / expectRate));
    }
}

void Probe::dumpBlocks(const char* when) {
    const abi::DevParamsT* d = dp();
    const abi::RxChannelParamsT* c = ch();
    line(fmt("parameter block %s:", when));
    if (d != nullptr) {
        line(fmt("  devParams: ppm %.3f fsHz %.0f syncUpdate %u reCal %u mode %d samplesPerPkt %u",
                 d->ppm, d->fsFreq.fsHz, d->fsFreq.syncUpdate, d->fsFreq.reCal,
                 static_cast<int>(d->mode), d->samplesPerPkt));
        line(fmt("  rsp1aParams: rfNotch %u rfDabNotch %u | rsp2Params: extRefOut %u | "
                 "rspDuoParams: extRefOut %d",
                 d->rsp1aParams.rfNotchEnable, d->rsp1aParams.rfDabNotchEnable,
                 d->rsp2Params.extRefOutputEn, d->rspDuoParams.extRefOutputEn));
        line(fmt("  rspDxParams: hdr %u biasT %u antenna %d rfNotch %u rfDabNotch %u",
                 d->rspDxParams.hdrEnable, d->rspDxParams.biasTEnable,
                 static_cast<int>(d->rspDxParams.antennaSel), d->rspDxParams.rfNotchEnable,
                 d->rspDxParams.rfDabNotchEnable));
    } else {
        line("  devParams: (none - a slave has no device block)");
    }
    if (c != nullptr) {
        const abi::TunerParamsT& t = c->tunerParams;
        line(fmt("  tuner: bwType %d ifType %d loMode %d gRdB %d LNAstate %u minGr %d rfHz %.0f "
                 "dcCal %u speedUp %u trackTime %d refreshRate %d",
                 static_cast<int>(t.bwType), static_cast<int>(t.ifType),
                 static_cast<int>(t.loMode), t.gain.gRdB, t.gain.LNAstate,
                 static_cast<int>(t.gain.minGr), t.rfFreq.rfHz, t.dcOffsetTuner.dcCal,
                 t.dcOffsetTuner.speedUp, t.dcOffsetTuner.trackTime,
                 t.dcOffsetTuner.refreshRateTime));
        const abi::ControlParamsT& k = c->ctrlParams;
        line(fmt("  ctrl: DC %u IQ %u decimation %u factor %u wideBand %u agc %d setPoint %d "
                 "attack %u decay %u adsbMode %d",
                 k.dcOffset.DCenable, k.dcOffset.IQenable, k.decimation.enable,
                 k.decimation.decimationFactor, k.decimation.wideBandSignal,
                 static_cast<int>(k.agc.enable), k.agc.setPoint_dBfs, k.agc.attack_ms,
                 k.agc.decay_ms, static_cast<int>(k.adsbMode)));
        line(fmt("  rsp1aTuner: biasT %u | rsp2Tuner: biasT %u amPort %d antenna %d rfNotch %u | "
                 "rspDuoTuner: biasT %u amPort %d amNotch %u rfNotch %u dabNotch %u | "
                 "rspDxTuner: hdrBw %d",
                 c->rsp1aTunerParams.biasTEnable, c->rsp2TunerParams.biasTEnable,
                 static_cast<int>(c->rsp2TunerParams.amPortSel),
                 static_cast<int>(c->rsp2TunerParams.antennaSel), c->rsp2TunerParams.rfNotchEnable,
                 c->rspDuoTunerParams.biasTEnable,
                 static_cast<int>(c->rspDuoTunerParams.tuner1AmPortSel),
                 c->rspDuoTunerParams.tuner1AmNotchEnable, c->rspDuoTunerParams.rfNotchEnable,
                 c->rspDuoTunerParams.rfDabNotchEnable,
                 abi::versionAtLeast(*ver_, abi::kRspDxTunerLayoutVersion)
                     ? static_cast<int>(c->rspDxTunerParams.hdrBw)
                     : -1));
    }
}

// THE STATE FOXSDR'S RECEIVER STARTS AN RSP IN (SdrPlaySource::
// applyKnownStateLocked), so the probe measures what the receiver meets.
void Probe::knownState() {
    abi::RxChannelParamsT* c = ch();
    abi::DevParamsT* d = dp();
    if (c == nullptr) { return; }
    SdrPlayRatePlan plan;
    sdrPlayRatePlan(2000000.0, plan);
    if (d != nullptr) {
        d->fsFreq.fsHz = plan.fsHz;
        d->ppm = 0.0;
        d->rsp1aParams.rfNotchEnable = 0;
        d->rsp1aParams.rfDabNotchEnable = 0;
        d->rspDxParams.hdrEnable = 0;
        d->rspDxParams.biasTEnable = 0;
        d->rspDxParams.antennaSel = abi::RspDx_ANTENNA_A;
        d->rspDxParams.rfNotchEnable = 0;
        d->rspDxParams.rfDabNotchEnable = 0;
    }
    c->tunerParams.ifType = plan.ifType;
    c->tunerParams.bwType = plan.bwType;
    c->tunerParams.loMode = abi::LO_Auto;
    c->tunerParams.rfFreq.rfHz = 100000000.0;
    c->tunerParams.gain.gRdB = 40;
    c->tunerParams.gain.LNAstate = 0;
    c->tunerParams.gain.minGr = abi::NORMAL_MIN_GR;
    c->ctrlParams.decimation.enable = static_cast<unsigned char>(plan.decEnable);
    c->ctrlParams.decimation.decimationFactor = static_cast<unsigned char>(plan.decM);
    c->ctrlParams.decimation.wideBandSignal = static_cast<unsigned char>(plan.wideBandSignal);
    c->ctrlParams.agc.enable = abi::AGC_DISABLE;
    c->ctrlParams.agc.setPoint_dBfs = -60;
    c->ctrlParams.dcOffset.DCenable = 1;
    c->ctrlParams.dcOffset.IQenable = 1;
    c->tunerParams.dcOffsetTuner.dcCal = 4;
    c->tunerParams.dcOffsetTuner.speedUp = 0;
    c->tunerParams.dcOffsetTuner.trackTime = 63;
    c->rsp1aTunerParams.biasTEnable = 0;
    c->rsp2TunerParams.biasTEnable = 0;
    c->rsp2TunerParams.amPortSel = abi::Rsp2_AMPORT_2;
    c->rsp2TunerParams.antennaSel = abi::Rsp2_ANTENNA_A;
    c->rsp2TunerParams.rfNotchEnable = 0;
    c->rspDuoTunerParams.biasTEnable = 0;
    c->rspDuoTunerParams.tuner1AmPortSel = abi::RspDuo_AMPORT_2;
    c->rspDuoTunerParams.tuner1AmNotchEnable = 0;
    c->rspDuoTunerParams.rfNotchEnable = 0;
    c->rspDuoTunerParams.rfDabNotchEnable = 0;
}

bool Probe::hasBiasTee() const {
    switch (hw()) {
        case abi::kRsp1A:
        case abi::kRsp1B:
        case abi::kRsp2:
        case abi::kRspDuo:
        case abi::kRspDx:
        case abi::kRspDxR2: return true;
        default: break;
    }
    return false;
}

void Probe::stepOpen() {
    beginStep(1);
    if (!api_.resolved) {
        line("the SDRplay API is not installed or did not load: " +
             core::scrubUploadPath(api_.loadDetail));
        mark(ProbeStatus::Error, "SDRplay API not installed");
        stopped_ = true;
        return;
    }
    line("API library: " + core::scrubUploadPath(api_.loadDetail));
    const abi::Api* table = &api_;
    CallResult r = call("Open", "", [table]() { return table->Open(); });
    if (!r.ok()) {
        stopped_ = true;
        return;
    }
    opened_ = true;
    auto ver = ver_;
    r = call("ApiVersion", "", [table, ver]() { return table->ApiVersion(ver.get()); });
    if (!r.ok()) {
        stopped_ = true;
        return;
    }
    line(fmt("API version %.2f (this probe's structures were checked against %.2f; oldest "
             "accepted %.2f)",
             static_cast<double>(*ver_), static_cast<double>(abi::kDeclaredAgainstVersion),
             static_cast<double>(abi::kMinApiVersion)));
    if (!abi::versionAtLeast(*ver_, abi::kMinApiVersion)) {
        mark(ProbeStatus::Error, fmt("API %.2f is older than %.2f", static_cast<double>(*ver_),
                                     static_cast<double>(abi::kMinApiVersion)));
        stopped_ = true;
        return;
    }
    r = call("LockDeviceApi", "", [table]() { return table->LockDeviceApi(); });
    if (!r.ok()) {
        stopped_ = true;
        return;
    }
    locked_ = true;
    auto devs = devs_;
    auto n = numDevs_;
    r = call("GetDevices", "", [table, devs, n]() {
        return table->GetDevices(devs->data(), n.get(), abi::kMaxDevices);
    });
    if (!r.ok()) {
        stopped_ = true;
        return;
    }
    const unsigned int count = std::min<unsigned int>(*numDevs_, abi::kMaxDevices);
    line(fmt("devices: %u", count));
    int chosen = -1;
    for (unsigned int i = 0; i < count; ++i) {
        const abi::DeviceT& d = (*devs_)[i];
        std::size_t len = 0;
        while (len < sizeof(d.SerNo) && d.SerNo[len] != '\0') { ++len; }
        const std::string serial(d.SerNo, len);
        const bool trustValid = abi::versionAtLeast(*ver_, abi::kValidFieldSinceVersion);
        line(fmt("  #%u hwVer %u (%s) serial-hash %s tuner 0x%02x rspDuoMode 0x%02x valid %s "
                 "rspDuoSampleFreq %.0f",
                 i, static_cast<unsigned>(d.hwVer), sdrPlayModelName(d.hwVer).c_str(),
                 sdrPlayProbeSerialHash(serial).c_str(), static_cast<unsigned>(d.tuner),
                 static_cast<unsigned>(d.rspDuoMode),
                 trustValid ? (d.valid != 0 ? "1" : "0") : "(n/a before 3.08)",
                 d.rspDuoSampleFreq));
        if (chosen < 0 && (!trustValid || d.valid != 0)) { chosen = static_cast<int>(i); }
    }
    if (chosen < 0) {
        mark(ProbeStatus::Error, "no usable RSP in the device list");
        stopped_ = true;
        return;
    }
    *dev_ = (*devs_)[static_cast<std::size_t>(chosen)];
    if (dev_->hwVer == abi::kRspDuo) {
        // Single tuner, tuner A: the mode the receiver selects.
        dev_->tuner = abi::Tuner_A;
        dev_->rspDuoMode = abi::RspDuoMode_Single_Tuner;
    }
    line(fmt("testing #%d, %s", chosen, sdrPlayModelName(dev_->hwVer).c_str()));
    auto dev = dev_;
    r = call("SelectDevice", fmt("tuner %d mode %d", static_cast<int>(dev_->tuner),
                                 static_cast<int>(dev_->rspDuoMode)),
             [table, dev]() { return table->SelectDevice(dev.get()); });
    if (!r.ok()) {
        stopped_ = true;
        return;
    }
    selected_ = true;
    r = call("UnlockDeviceApi", "", [table]() { return table->UnlockDeviceApi(); });
    locked_ = false;
    auto out = devParamsOut_;
    void* h = dev_->dev;
    r = call("GetDeviceParams", "", [table, h, out]() { return table->GetDeviceParams(h, out.get()); });
    if (!r.ok() || *devParamsOut_ == nullptr || ch() == nullptr) {
        if (r.ok()) { mark(ProbeStatus::Error, "GetDeviceParams returned no parameter block"); }
        stopped_ = true;
        return;
    }
    dumpBlocks("as the service handed it over");
    knownState();
    dumpBlocks("as FoxSDR's receiver sets it before Init (2 MS/s zero-IF, 100 MHz)");

    abi::CallbackFnsT cbs{};
    cbs.StreamACbFn = &probeStreamA;
    cbs.StreamBCbFn = &probeStreamB;
    cbs.EventCbFn = &probeEvent;
    ProbeLink* ctx = link_.get();
    auto keep = link_;
    r = call("Init", "", [table, h, cbs, ctx, keep]() mutable {
        (void) keep;
        return table->Init(h, &cbs, ctx);
    });
    if (!r.ok()) {
        stopped_ = true;
        return;
    }
    initialised_ = true;
}

void Probe::stepRates() {
    beginStep(2);
    if (stopped_) { return; }
    abi::RxChannelParamsT* c = ch();
    abi::DevParamsT* d = dp();
    for (double rate : sdrPlaySupportedRatesHz()) {
        if (stopped_) { return; }
        SdrPlayRatePlan plan;
        if (!sdrPlayRatePlan(rate, plan)) { continue; }
        abi::ReasonForUpdateT reason = abi::Update_None;
        if (d != nullptr && d->fsFreq.fsHz != plan.fsHz) {
            d->fsFreq.fsHz = plan.fsHz;
            reason = static_cast<abi::ReasonForUpdateT>(reason | abi::Update_Dev_Fs);
        }
        if (c->tunerParams.ifType != plan.ifType) {
            c->tunerParams.ifType = plan.ifType;
            reason = static_cast<abi::ReasonForUpdateT>(reason | abi::Update_Tuner_IfType);
        }
        if (c->ctrlParams.decimation.decimationFactor != plan.decM ||
            c->ctrlParams.decimation.enable != plan.decEnable) {
            c->ctrlParams.decimation.enable = static_cast<unsigned char>(plan.decEnable);
            c->ctrlParams.decimation.decimationFactor = static_cast<unsigned char>(plan.decM);
            c->ctrlParams.decimation.wideBandSignal = static_cast<unsigned char>(plan.wideBandSignal);
            reason = static_cast<abi::ReasonForUpdateT>(reason | abi::Update_Ctrl_Decimation);
        }
        if (c->tunerParams.bwType != plan.bwType) {
            c->tunerParams.bwType = plan.bwType;
            reason = static_cast<abi::ReasonForUpdateT>(reason | abi::Update_Tuner_BwType);
        }
        const std::string label = fmt("rate %.0f (fsHz %.0f, decimation %s%u, IF %d, BW %d)", rate,
                                      plan.fsHz, plan.decEnable ? "" : "off/", plan.decM,
                                      static_cast<int>(plan.ifType), static_cast<int>(plan.bwType));
        long long ack = -1;
        if (reason != abi::Update_None) {
            const CallResult r = update(reason, abi::Update_Ext1_None, "rate");
            if (!r.ok()) {
                line(label + ": not measured");
                continue;
            }
            if ((reason & abi::Update_Dev_Fs) != 0) { ack = waitFlag(link_->fsChanged); }
        }
        if (ack >= 0) { line(fmt("fsChanged seen %lld ms after the Update returned", ack)); }
        measure(opt_.streamPerRate, rate, label, true);
    }
}

void Probe::stepFrequencies() {
    beginStep(3);
    if (stopped_) { return; }
    // Back to the receiver's own rate first, so every step below is measured
    // against the same 2 MS/s.
    abi::RxChannelParamsT* c = ch();
    abi::DevParamsT* d = dp();
    SdrPlayRatePlan plan;
    sdrPlayRatePlan(2000000.0, plan);
    abi::ReasonForUpdateT reason = abi::Update_None;
    if (d != nullptr && d->fsFreq.fsHz != plan.fsHz) {
        d->fsFreq.fsHz = plan.fsHz;
        reason = static_cast<abi::ReasonForUpdateT>(reason | abi::Update_Dev_Fs);
    }
    if (c->ctrlParams.decimation.enable != plan.decEnable ||
        c->ctrlParams.decimation.decimationFactor != plan.decM) {
        c->ctrlParams.decimation.enable = static_cast<unsigned char>(plan.decEnable);
        c->ctrlParams.decimation.decimationFactor = static_cast<unsigned char>(plan.decM);
        reason = static_cast<abi::ReasonForUpdateT>(reason | abi::Update_Ctrl_Decimation);
    }
    if (c->tunerParams.bwType != plan.bwType) {
        c->tunerParams.bwType = plan.bwType;
        reason = static_cast<abi::ReasonForUpdateT>(reason | abi::Update_Tuner_BwType);
    }
    if (reason != abi::Update_None) { update(reason, abi::Update_Ext1_None, "back to 2 MS/s"); }

    const double freqs[] = {500e3, 1.9e6, 7.1e6, 14.2e6, 28.5e6, 50.1e6, 100e6,
                            145.5e6, 225e6, 435e6, 1090e6, 1575.42e6, 1900e6};
    for (double f : freqs) {
        if (stopped_) { return; }
        c->tunerParams.rfFreq.rfHz = f;
        const CallResult r = update(abi::Update_Tuner_Frf, abi::Update_Ext1_None,
                                    fmt("%.3f MHz", f / 1e6).c_str());
        if (!r.ok()) { continue; }
        const long long ack = waitFlag(link_->rfChanged);
        line(ack >= 0 ? fmt("rfChanged seen %lld ms after the Update returned", ack)
                      : std::string("rfChanged not seen within 500 ms"));
        measure(opt_.streamPerStep, 2000000.0, fmt("at %.3f MHz", f / 1e6), false);
    }
}

void Probe::stepLna() {
    beginStep(4);
    if (stopped_) { return; }
    abi::RxChannelParamsT* c = ch();
    // 300 MHz: the band in which every model has its FULL table (SDRplay API
    // Specification 3.15, section 5: RSPdx/RSPdxR2 250-420 MHz reach state
    // 27, the RSP1A/1B/duo 60-420 MHz state 9, the RSP2 0-420 MHz state 8).
    c->tunerParams.rfFreq.rfHz = 300e6;
    update(abi::Update_Tuner_Frf, abi::Update_Ext1_None, "300 MHz for the LNA table");
    const int states = sdrPlayLnaStateCount(hw());
    line(fmt("%d LNA states for %s, IF gain reduction %d dB", states,
             sdrPlayModelName(hw()).c_str(), c->tunerParams.gain.gRdB));
    for (int s = 0; s < states; ++s) {
        if (stopped_) { return; }
        c->tunerParams.gain.LNAstate = static_cast<unsigned char>(s);
        link_->lastLnaGr.store(-1);
        const CallResult r = update(abi::Update_Tuner_Gr, abi::Update_Ext1_None,
                                    fmt("LNAstate %d", s).c_str());
        if (!r.ok()) { continue; }
        const long long ack = waitFlag(link_->grChanged);
        measure(opt_.streamPerStep, 2000000.0, fmt("LNAstate %d", s), false);
        line(fmt("LNAstate %d: grChanged %s, gain event lnaGRdB %d gRdB %d currGain %.2f", s,
                 ack >= 0 ? fmt("after %lld ms", ack).c_str() : "not seen",
                 link_->lastLnaGr.load(), link_->lastGr.load(), link_->lastGain.load()));
    }
    c->tunerParams.gain.LNAstate = 0;
    update(abi::Update_Tuner_Gr, abi::Update_Ext1_None, "LNAstate back to 0");
    c->tunerParams.rfFreq.rfHz = 100e6;
    update(abi::Update_Tuner_Frf, abi::Update_Ext1_None, "back to 100 MHz");
}

void Probe::stepAntennas() {
    beginStep(5);
    if (stopped_) { return; }
    abi::RxChannelParamsT* c = ch();
    abi::DevParamsT* d = dp();
    if ((hw() == abi::kRspDx || hw() == abi::kRspDxR2) && d != nullptr) {
        const abi::RspDxAntennaSelectT ants[] = {abi::RspDx_ANTENNA_A, abi::RspDx_ANTENNA_B,
                                                 abi::RspDx_ANTENNA_C, abi::RspDx_ANTENNA_A};
        const char* names[] = {"A", "B", "C", "A"};
        for (int i = 0; i < 4; ++i) {
            if (stopped_) { return; }
            d->rspDxParams.antennaSel = ants[i];
            const CallResult r = update(abi::Update_None, abi::Update_RspDx_AntennaControl,
                                        fmt("antenna %s", names[i]).c_str());
            if (r.ok()) {
                measure(opt_.streamPerStep, 2000000.0, fmt("antenna %s", names[i]), false);
            }
        }
        // HDR: only below 2 MHz, at one of the frequencies the specification
        // lists for it (section 3.15, p25: 1.9 MHz is one).
        c->tunerParams.rfFreq.rfHz = 1.9e6;
        update(abi::Update_Tuner_Frf, abi::Update_Ext1_None, "1.900 MHz for HDR");
        d->rspDxParams.hdrEnable = 1;
        if (update(abi::Update_None, abi::Update_RspDx_HdrEnable, "HDR on").ok()) {
            measure(opt_.streamPerStep, 2000000.0, "HDR on at 1.9 MHz", false);
        }
        d->rspDxParams.hdrEnable = 0;
        if (update(abi::Update_None, abi::Update_RspDx_HdrEnable, "HDR off").ok()) {
            measure(opt_.streamPerStep, 2000000.0, "HDR off at 1.9 MHz", false);
        }
        c->tunerParams.rfFreq.rfHz = 100e6;
        update(abi::Update_Tuner_Frf, abi::Update_Ext1_None, "back to 100 MHz");
        return;
    }
    if (hw() == abi::kRsp2) {
        c->rsp2TunerParams.antennaSel = abi::Rsp2_ANTENNA_B;
        if (update(abi::Update_Rsp2_AntennaControl, abi::Update_Ext1_None, "antenna B").ok()) {
            measure(opt_.streamPerStep, 2000000.0, "antenna B", false);
        }
        c->rsp2TunerParams.antennaSel = abi::Rsp2_ANTENNA_A;
        if (update(abi::Update_Rsp2_AntennaControl, abi::Update_Ext1_None, "antenna A").ok()) {
            measure(opt_.streamPerStep, 2000000.0, "antenna A", false);
        }
        c->rsp2TunerParams.amPortSel = abi::Rsp2_AMPORT_1;
        if (update(abi::Update_Rsp2_AmPortSelect, abi::Update_Ext1_None, "Hi-Z port").ok()) {
            measure(opt_.streamPerStep, 2000000.0, "Hi-Z port", false);
        }
        c->rsp2TunerParams.amPortSel = abi::Rsp2_AMPORT_2;
        update(abi::Update_Rsp2_AmPortSelect, abi::Update_Ext1_None, "back to antenna A");
        return;
    }
    steps_[cur_].status = ProbeStatus::Skipped;
    steps_[cur_].detail = (hw() == abi::kRspDuo)
                              ? "RSPduo: tuner 1 only in this probe"
                              : "this model has one antenna input";
    line(steps_[cur_].detail);
}

void Probe::stepBias() {
    beginStep(6);
    if (stopped_) { return; }
    if (!hasBiasTee()) {
        steps_[cur_].status = ProbeStatus::Skipped;
        steps_[cur_].detail = "this model has no bias tee";
        line(steps_[cur_].detail);
        return;
    }
    abi::RxChannelParamsT* c = ch();
    abi::DevParamsT* d = dp();
    // One write for both directions, so ON and OFF can never go to two
    // different fields.
    const auto setBias = [&](unsigned char v, const char* why) {
        abi::ReasonForUpdateT r = abi::Update_None;
        abi::ReasonForUpdateExt1T e = abi::Update_Ext1_None;
        switch (hw()) {
            case abi::kRsp1A:
            case abi::kRsp1B:
                c->rsp1aTunerParams.biasTEnable = v;
                r = abi::Update_Rsp1a_BiasTControl;
                break;
            case abi::kRsp2:
                c->rsp2TunerParams.biasTEnable = v;
                r = abi::Update_Rsp2_BiasTControl;
                break;
            case abi::kRspDuo:
                c->rspDuoTunerParams.biasTEnable = v;
                r = abi::Update_RspDuo_BiasTControl;
                break;
            default:
                if (d == nullptr) { return CallResult{}; }
                d->rspDxParams.biasTEnable = v;
                e = abi::Update_RspDx_BiasTControl;
                break;
        }
        return update(r, e, why);
    };
    if (setBias(0, "bias tee OFF").ok()) {
        measure(opt_.streamPerStep, 2000000.0, "bias tee off", false);
    }
    // THE ONLY PLACE THE BIAS TEE IS EVER SWITCHED ON, and only with the
    // caller's separate confirmation. Switched off again straight after,
    // whatever the ON answered.
    if (!opt_.biasTeeOn) {
        line("bias tee ON: not tested - it was not separately confirmed");
        steps_[cur_].detail = "ON not tested (not confirmed)";
        return;
    }
    if (stopped_) { return; }
    const CallResult on = setBias(1, "bias tee ON (confirmed)");
    if (on.ok()) { measure(opt_.streamPerStep, 2000000.0, "bias tee on", false); }
    if (!stopped_) { setBias(0, "bias tee OFF again"); }
}

void Probe::stepIfModes() {
    beginStep(7);
    if (stopped_) { return; }
    abi::RxChannelParamsT* c = ch();
    abi::DevParamsT* d = dp();
    if (d == nullptr) {
        steps_[cur_].status = ProbeStatus::Skipped;
        steps_[cur_].detail = "no device block";
        return;
    }
    // THE LOW-IF COMBINATIONS THE SPECIFICATION LISTS (section 3.15, p25,
    // "Conditions for LIF down-conversion to be enabled for all RSPs in single
    // tuner mode"). What each one DELIVERS is exactly what the specification
    // does not say and what 0.99.44 had to guess from two RSP2 logs, so it is
    // measured and printed, not judged.
    struct Mode {
        double fs;
        abi::BwMHzT bw;
        abi::IfKHzT ifk;
    };
    const Mode modes[] = {
        {2000000.0, abi::BW_0_200, abi::IF_0_450},  {2000000.0, abi::BW_0_600, abi::IF_0_450},
        {6000000.0, abi::BW_1_536, abi::IF_1_620},  {8000000.0, abi::BW_1_536, abi::IF_2_048},
        {8192000.0, abi::BW_1_536, abi::IF_2_048},  {8000000.0, abi::BW_5_000, abi::IF_2_048},
    };
    for (const Mode& m : modes) {
        if (stopped_) { return; }
        d->fsFreq.fsHz = m.fs;
        c->tunerParams.bwType = m.bw;
        c->tunerParams.ifType = m.ifk;
        c->ctrlParams.decimation.enable = 0;
        c->ctrlParams.decimation.decimationFactor = 1;
        const std::string label =
            fmt("IF %d kHz, fsHz %.0f, BW %d", static_cast<int>(m.ifk), m.fs, static_cast<int>(m.bw));
        const CallResult r =
            update(static_cast<abi::ReasonForUpdateT>(abi::Update_Dev_Fs | abi::Update_Tuner_IfType |
                                                      abi::Update_Tuner_BwType |
                                                      abi::Update_Ctrl_Decimation),
                   abi::Update_Ext1_None, label.c_str());
        if (!r.ok()) { continue; }
        waitFlag(link_->fsChanged);
        measure(opt_.streamPerRate, 0.0, label, false);
    }
    SdrPlayRatePlan plan;
    sdrPlayRatePlan(2000000.0, plan);
    d->fsFreq.fsHz = plan.fsHz;
    c->tunerParams.bwType = plan.bwType;
    c->tunerParams.ifType = plan.ifType;
    update(static_cast<abi::ReasonForUpdateT>(abi::Update_Dev_Fs | abi::Update_Tuner_IfType |
                                              abi::Update_Tuner_BwType),
           abi::Update_Ext1_None, "back to 2 MS/s zero-IF");
}

void Probe::stepClose() {
    beginStep(8, true);
    if (hung_ || serviceGone_ || cancelledFlag_) {
        steps_[cur_].status = ProbeStatus::NotRun;
        steps_[cur_].detail = hung_ ? "a call is still inside the SDRplay API - no further calls"
                              : serviceGone_ ? "the service stopped answering - no further calls"
                                             : "cancelled";
        line(steps_[cur_].detail);
        return;
    }
    if (!opened_ && !locked_ && !selected_ && !initialised_) {
        steps_[cur_].status = ProbeStatus::NotRun;
        steps_[cur_].detail = "nothing was opened";
        line(steps_[cur_].detail);
        return;
    }
    // A step that failed early set stopped_ to skip the rest; the teardown of
    // whatever was opened still runs, and is timed like everything else.
    stopped_ = false;
    steps_[cur_].status = ProbeStatus::Pass;
    steps_[cur_].detail.clear();
    const abi::Api* table = &api_;
    void* h = dev_->dev;
    if (initialised_) {
        call("Uninit", "", [table, h]() { return table->Uninit(h); });
        initialised_ = false;
    }
    if (locked_ && !stopped_) {
        call("UnlockDeviceApi", "", [table]() { return table->UnlockDeviceApi(); });
        locked_ = false;
    }
    if (selected_ && !stopped_) {
        auto dev = dev_;
        call("ReleaseDevice", "", [table, dev]() { return table->ReleaseDevice(dev.get()); });
        selected_ = false;
    }
    if (opened_ && !stopped_) {
        call("Close", "", [table]() { return table->Close(); });
        opened_ = false;
    }
}

SdrPlayProbeResult Probe::run() {
    const std::string started = dateNowUtc();
    stepOpen();
    const bool openedOk = !stopped_;
    if (!openedOk) {
        for (int i = 1; i < 7; ++i) { steps_[static_cast<std::size_t>(i)].status = ProbeStatus::NotRun; }
    } else {
        stepRates();
        stepFrequencies();
        stepLna();
        stepAntennas();
        stepBias();
        stepIfModes();
    }
    stepClose();
    // A STREAM NOBODY STOPPED STILL CALLS US. When the run ended with a call
    // still inside the DLL (or was cancelled), Uninit was never made, so the
    // service may go on delivering into this link after the probe is gone:
    // it is kept for the life of the process rather than freed under the
    // service's thread - the receiver's own rule (SdrPlaySource's graveyard).
    if (initialised_) {
        static std::mutex graveyardMutex;
        static auto* graveyard = new std::vector<std::shared_ptr<ProbeLink>>();
        std::lock_guard<std::mutex> lk(graveyardMutex);
        graveyard->push_back(link_);
    }
    if (!openedOk) {
        for (int i = 1; i < 7; ++i) {
            SdrPlayProbeStep& s = steps_[static_cast<std::size_t>(i)];
            s.status = ProbeStatus::NotRun;
            if (s.detail.empty()) { s.detail = "step 1 did not complete"; }
        }
    }

    std::string out;
    out += "FoxSDR SDRplay diagnostic\n";
    out += std::string("probe version: ") + kSdrPlayProbeVersion + "\n";
    out += "FoxSDR version: " + opt_.foxsdrVersion + "\n";
    out += "OS: " + opt_.osDescription + "\n";
    out += "started: " + started + "\n";
    out += fmt("limits: a call is SLOW over %lld ms and HUNG at %lld ms; %lld ms streamed per "
               "rate, %lld ms per step\n",
               static_cast<long long>(opt_.slowCall.count()),
               static_cast<long long>(opt_.callLimit.count()),
               static_cast<long long>(opt_.streamPerRate.count()),
               static_cast<long long>(opt_.streamPerStep.count()));
    out += std::string("bias tee ON test: ") +
           (opt_.biasTeeOn ? "confirmed by the user" : "not confirmed - the bias tee stayed off") +
           "\n";
    out += "\nSUMMARY\n";
    bool anyBad = false;
    for (const SdrPlayProbeStep& s : steps_) {
        std::string name = s.name;
        std::string dots(std::max<int>(2, 22 - static_cast<int>(name.size())), '.');
        out += fmt("  %d %s %s %-7s", s.number, name.c_str(), dots.c_str(), probeStatusName(s.status));
        if (!s.detail.empty()) { out += "  " + s.detail; }
        out += "\n";
        if (s.status == ProbeStatus::Error || s.status == ProbeStatus::Hung) { anyBad = true; }
    }
    out += fmt("  SDRplay API calls made: %d%s%s\n", calls_, hung_ ? "; ended early on a HUNG call" : "",
               cancelledFlag_ ? "; cancelled" : "");
    (void) anyBad;
    out += "\nEVENTS (from the service's event callback)\n";
    {
        std::lock_guard<std::mutex> lk(link_->eventMutex);
        if (link_->events.empty()) { out += "  (none)\n"; }
        for (const std::string& e : link_->events) { out += "  " + e + "\n"; }
    }
    out += "\nDETAIL (every sdrplay_api_* call: +seconds since start, UTC time -> call | UTC time "
           "<- answer in ms)\n";
    out += detail_;

    SdrPlayProbeResult res;
    res.report = core::maskAccountNames(out);
    res.steps = steps_;
    res.hung = hung_;
    res.cancelled = cancelledFlag_;
    res.callsMade = calls_;
    return res;
}

}  // namespace

SdrPlayProbeResult runSdrPlayProbe(const abi::Api& api, const SdrPlayProbeOptions& opt) {
    Probe p(api, opt);
    return p.run();
}

std::string sdrPlayProbeCommandLine(const std::string& exePath, const std::string& outPath,
                                    bool biasTeeOn) {
    std::string cmd = "\"" + exePath + "\" --sdrplay-probe \"" + outPath + "\"";
    if (biasTeeOn) { cmd += " --sdrplay-probe-bias-tee"; }
    return cmd;
}

#if defined(_WIN32)

// THE ANSI CODE PAGE, NOT UTF-8, on purpose: the output path comes from
// core::diagCrashDir(), which is built from getenv("LOCALAPPDATA") - ANSI -
// and the child reads it back through main()'s narrow argv, which the C
// runtime also converts with the ANSI code page. Round-tripping it through
// UTF-8 here would garble exactly the non-ASCII profile names it must keep.
namespace {
std::wstring widenAcp(const std::string& s) {
    if (s.empty()) { return std::wstring(); }
    const int n = ::MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) {
        ::MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    }
    return w;
}
std::string narrowAcp(const std::wstring& w) {
    if (w.empty()) { return std::string(); }
    const int n = ::WideCharToMultiByte(CP_ACP, 0, w.data(), static_cast<int>(w.size()), nullptr,
                                        0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) {
        ::WideCharToMultiByte(CP_ACP, 0, w.data(), static_cast<int>(w.size()), s.data(), n,
                              nullptr, nullptr);
    }
    return s;
}
}  // namespace

SdrPlayProbeChild::~SdrPlayProbeChild() {
    // The handle only: the process itself is left to finish (see the header).
    if (process_ != nullptr) { ::CloseHandle(static_cast<HANDLE>(process_)); }
}

bool SdrPlayProbeChild::start(const std::string& outPath, bool biasTeeOn, std::string& error) {
    if (process_ != nullptr) {
        error = "a diagnostic is already running";
        return false;
    }
    std::wstring exe(32768, L'\0');
    const DWORD len = ::GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    if (len == 0 || len >= exe.size()) {
        error = "could not find FoxSDR's own program file";
        return false;
    }
    exe.resize(len);
    // argv[0] only - the program actually started is `exe` itself, wide.
    std::wstring cmd = widenAcp(sdrPlayProbeCommandLine(narrowAcp(exe), outPath, biasTeeOn));
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // No window, no inherited handles: the child writes its own file and
    // needs nothing of ours.
    if (!::CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                          CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &si,
                          &pi)) {
        error = fmt("Windows would not start it (error %lu)", static_cast<unsigned long>(::GetLastError()));
        return false;
    }
    ::CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    exitCode_ = -1;
    return true;
}

bool SdrPlayProbeChild::running() {
    if (process_ == nullptr) { return false; }
    if (::WaitForSingleObject(static_cast<HANDLE>(process_), 0) == WAIT_TIMEOUT) { return true; }
    DWORD code = 0;
    ::GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
    exitCode_ = static_cast<int>(code);
    ::CloseHandle(static_cast<HANDLE>(process_));
    process_ = nullptr;
    return false;
}

#elif defined(__ANDROID__)

SdrPlayProbeChild::~SdrPlayProbeChild() = default;
bool SdrPlayProbeChild::start(const std::string&, bool, std::string& error) {
    error = "the SDRplay diagnostic is not available on Android";
    return false;
}
bool SdrPlayProbeChild::running() { return false; }

#else

SdrPlayProbeChild::~SdrPlayProbeChild() = default;

bool SdrPlayProbeChild::start(const std::string& outPath, bool biasTeeOn, std::string& error) {
    if (pid_ > 0) {
        error = "a diagnostic is already running";
        return false;
    }
    char exe[4096];
    const ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) {
        error = "could not find FoxSDR's own program file";
        return false;
    }
    exe[n] = '\0';
    const pid_t pid = ::fork();
    if (pid < 0) {
        error = "the system would not start it";
        return false;
    }
    if (pid == 0) {
        // The child: nothing but the exec - no allocation, no locks.
        if (biasTeeOn) {
            ::execl(exe, exe, "--sdrplay-probe", outPath.c_str(), "--sdrplay-probe-bias-tee",
                    static_cast<char*>(nullptr));
        } else {
            ::execl(exe, exe, "--sdrplay-probe", outPath.c_str(), static_cast<char*>(nullptr));
        }
        ::_exit(127);
    }
    pid_ = pid;
    exitCode_ = -1;
    return true;
}

bool SdrPlayProbeChild::running() {
    if (pid_ <= 0) { return false; }
    int status = 0;
    const pid_t r = ::waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
    if (r == 0) { return true; }
    exitCode_ = (r > 0 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    pid_ = -1;
    return false;
}

#endif

int runSdrPlayProbeToFile(const std::string& outPath, bool biasTeeOn) {
    SdrPlayProbeOptions opt;
    opt.biasTeeOn = biasTeeOn;
    opt.foxsdrVersion = std::string(cascade::versionString()) + " (" + cascade::gitCommit() + ")";
    opt.osDescription = core::osDescription();
    opt.progress = [](const std::string& s) {
        std::printf("sdrplay-probe: %s\n", s.c_str());
        std::fflush(stdout);
    };
    const SdrPlayProbeResult res = runSdrPlayProbe(processSdrPlayApi(), opt);
    std::ofstream f(outPath, std::ios::binary | std::ios::trunc);
    if (!f) {
        std::fprintf(stderr, "sdrplay-probe: cannot write the report file\n");
        return 2;
    }
    f << res.report;
    f.close();
    if (!f) {
        std::fprintf(stderr, "sdrplay-probe: writing the report file failed\n");
        return 2;
    }
    bool bad = false;
    for (const SdrPlayProbeStep& s : res.steps) {
        std::printf("sdrplay-probe: %d %s: %s%s%s\n", s.number, s.name.c_str(),
                    probeStatusName(s.status), s.detail.empty() ? "" : " - ", s.detail.c_str());
        if (s.status == ProbeStatus::Error || s.status == ProbeStatus::Hung) { bad = true; }
    }
    std::fflush(stdout);
    return bad ? 1 : 0;
}

}  // namespace cascade::source
