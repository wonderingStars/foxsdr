// A fake SDRplay API: the seam that makes an RSP driver provable on a machine
// with no RSP, no SDRplay API and no SDRplay service.
//
// WHY THIS IS THE PROOF AND NOT A CONVENIENCE. There is no SDRplay hardware on
// the bench this driver was written on and the vendor API is not installed -
// installing a third party's Windows service is the owner's decision, not an
// agent's. So nothing in src/source/sdrplay_source.cpp has ever met the real
// service. What CAN be held to account is every call the driver makes and
// every byte it writes into the parameter block, because the driver reaches
// the API through one table of function pointers
// (cascade::source::sdrplay_abi::Api) and this file fills that table in.
//
// WHAT THE FAKE IS FAITHFUL TO. The vendor's own header, restated in
// src/source/sdrplay_api_decl.hpp and pinned there by static_asserts against
// sizes measured from the real 3.07, 3.11 and 3.15 headers; and the call
// sequence SoapySDRPlay3 (MIT) performs, which is what a working host is known
// to do. It answers as the service does: GetDevices fills an array, SelectDevice
// hands back a device handle, GetDeviceParams hands back pointers into
// storage IT owns, Init starts calling our callbacks, Update takes effect and
// is acknowledged through the next stream callback's changed flags.
//
// WHAT IT CANNOT BE FAITHFUL TO, and this is stated here so nobody mistakes a
// green suite for a tested radio: the service's real timing, its real
// threading, its refusals, and whatever it does that its header does not say.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "source/sdrplay_api_decl.hpp"

namespace fakesdrplay {

namespace abi = cascade::source::sdrplay_abi;

// A snapshot of everything that matters at the moment Init was called - the
// state the radio is actually started in, as opposed to whatever the
// parameter block holds by the time a test gets round to looking.
struct InitSnapshot {
    bool taken = false;
    double fsHz = 0.0;
    double rfHz = 0.0;
    int gRdB = 0;
    int LNAstate = 0;
    int bwType = 0;
    int ifType = 0;
    int agcEnable = 0;
    int agcSetPoint = 0;
    int decEnable = 0;
    int decFactor = 0;
    int dcEnable = 0;
    int iqEnable = 0;
};

class FakeSdrPlayApi {
public:
    FakeSdrPlayApi() {
        // One at a time: the API's entry points are plain C functions with no
        // context argument, so the thunks below have to find their object
        // through a file-scope pointer. Two live fakes would be two tests
        // quietly sharing one.
        instance() = this;
        std::memset(&devParams, 0, sizeof(devParams));
        std::memset(&chA, 0, sizeof(chA));
        std::memset(&chB, 0, sizeof(chB));
        params.devParams = &devParams;
        params.rxChannelA = &chA;
        params.rxChannelB = &chB;

        table.resolved = true;
        table.loadDetail = "fake";
        table.Open = &thunkOpen;
        table.Close = &thunkClose;
        table.ApiVersion = &thunkApiVersion;
        table.LockDeviceApi = &thunkLock;
        table.UnlockDeviceApi = &thunkUnlock;
        table.GetDevices = &thunkGetDevices;
        table.SelectDevice = &thunkSelectDevice;
        table.ReleaseDevice = &thunkReleaseDevice;
        table.GetErrorString = &thunkGetErrorString;
        table.GetLastError = nullptr;
        table.DebugEnable = &thunkDebugEnable;
        table.GetDeviceParams = &thunkGetDeviceParams;
        table.Init = &thunkInit;
        table.Uninit = &thunkUninit;
        table.Update = &thunkUpdate;
        table.SwapRspDuoActiveTuner = &thunkSwap;
    }

    ~FakeSdrPlayApi() { instance() = nullptr; }

    FakeSdrPlayApi(const FakeSdrPlayApi&) = delete;
    FakeSdrPlayApi& operator=(const FakeSdrPlayApi&) = delete;

    // --- what the tests configure ----------------------------------------

    float version = 3.15f;
    abi::ErrT openResult = abi::Success;
    abi::ErrT selectResult = abi::Success;
    abi::ErrT getParamsResult = abi::Success;
    abi::ErrT initResult = abi::Success;
    abi::ErrT updateResult = abi::Success;
    abi::ErrT swapResult = abi::Success;
    // The service acknowledges a queued Update through the changed flags in
    // the next stream callback. With this on, the fake fires an empty callback
    // carrying the right flag the instant the Update returns - which is what
    // lets a test prove the driver asked for the acknowledgement without
    // waiting the full kUpdateWait for one that never comes.
    bool autoAck = true;

    // THE WEDGED SERVICE. With hangInGetDevices set, GetDevices goes in and
    // does not come out until releaseHang is set - which is how the enumeration
    // bound (kEnumerateWait) and the hold-off after it are proved. The two
    // observation flags let a test see the worker go in and, afterwards, come
    // back out, so an abandoned thread is never left inside this object while
    // it is being destroyed.
    std::atomic<bool> hangInGetDevices{false};
    std::atomic<bool> releaseHang{false};
    std::atomic<bool> insideGetDevices{false};
    std::atomic<bool> leftGetDevices{false};

    // THE SAME SERVICE, WEDGED WITH THE RADIO ALREADY OPEN. GetDevices is
    // what a SCAN goes into; sdrplay_api_Update is what every LIVE CONTROL
    // goes into - retune, LNA state, IF gain, AGC, set point, antenna, bias
    // tee, notches, HDR, sample rate - and the 0.96.2 report is a retune that
    // did not come back inside the hang watchdog's five seconds. Same shape,
    // same polling loop, and for the same reason: an ABANDONED worker has to
    // be able to leave when the test releases it rather than hold a vendor
    // call open past the end of the process.
    //
    // The hang is taken BEFORE any acknowledgement is fired, and the call
    // returns straight afterwards without firing one: by the time a test
    // releases this, the driver gave up on it long ago and the Link the
    // callback would be delivered into is no longer the fake's business.
    std::atomic<bool> hangInUpdate{false};
    std::atomic<bool> releaseUpdateHang{false};
    std::atomic<bool> insideUpdate{false};
    std::atomic<bool> leftUpdate{false};

    // ...AND THE WEDGE HOLDS THE WHOLE DEVICE, NOT JUST THE ONE CALL.
    //
    // This is what the 0.96.4 hang report added to the two above, and it is
    // the only part of a wedged service the fake could not express. The GUI
    // thread's stack there is
    //
    //   ntdll -> KERNELBASE -> sdrplay_api.dll -> sdrplay_api.dll
    //         -> SdrPlaySource::stopStreamingLocked -> stop
    //         -> Pipeline::quiesceSourceThreadLocked -> Pipeline::start
    //
    // i.e. the TEARDOWN's sdrplay_api_Uninit parked on a synchronisation
    // object inside the vendor DLL, while the log two lines above says a
    // control had already been abandoned and "a worker is still inside
    // sdrplay_api_Update". One call per device at a time is what the API's own
    // device lock means, so a call that never returns is a device nothing else
    // can enter - and Uninit and ReleaseDevice are exactly what a teardown
    // enters it with.
    //
    // Modelled the same way as the hangs above: polled, so a caller released
    // later can still leave, and observed through flags so a test can say WHO
    // went in rather than only how long it took.
    std::atomic<int> updateHangDepth{0};
    std::atomic<bool> wedgedDeviceBlocksTeardown{true};
    std::atomic<bool> uninitEnteredWhileWedged{false};
    std::atomic<bool> releaseEnteredWhileWedged{false};

    // --- what the tests observe ------------------------------------------

    std::vector<std::string> calls;
    InitSnapshot atInit;
    int openCount = 0;
    int closeCount = 0;
    int selectCount = 0;
    int releaseCount = 0;
    bool initialised = false;

    // The storage the "service" owns. The driver writes straight into these,
    // exactly as it writes into the service's own memory.
    abi::DevParamsT devParams{};
    abi::RxChannelParamsT chA{};
    abi::RxChannelParamsT chB{};
    abi::DeviceParamsT params{};

    abi::Api table;

    // --- building a device list ------------------------------------------

    void addDevice(const char* serial, unsigned char hwVer,
                   abi::RspDuoModeT duoModes = abi::RspDuoMode_Unknown,
                   abi::TunerSelectT tuners = abi::Tuner_Neither) {
        abi::DeviceT d{};
        std::snprintf(d.SerNo, sizeof(d.SerNo), "%s", serial);
        d.hwVer = hwVer;
        d.valid = 1;
        d.rspDuoMode = duoModes;
        d.tuner = tuners;
        d.rspDuoSampleFreq = 0.0;
        d.dev = nullptr;
        devices.push_back(d);
    }

    void addRspDuo(const char* serial) {
        // A free RSPduo offers every mode and both tuners; the driver is
        // expected to narrow that to single-tuner at SelectDevice.
        addDevice(serial, abi::kRspDuo,
                  static_cast<abi::RspDuoModeT>(abi::RspDuoMode_Single_Tuner |
                                                abi::RspDuoMode_Dual_Tuner |
                                                abi::RspDuoMode_Master),
                  abi::Tuner_Both);
    }

    // --- driving the callbacks -------------------------------------------

    // One block of signal, as the service delivers it: separate I and Q arrays
    // of int16.
    void pushSamples(const short* xi, const short* xq, unsigned int n) {
        if (streamA == nullptr) { return; }
        abi::StreamCbParamsT p{};
        p.numSamples = n;
        streamA(const_cast<short*>(xi), const_cast<short*>(xq), &p, n, 0, cbContext);
    }

    void fireEvent(abi::EventT id, const abi::EventParamsT& p) {
        if (eventCb == nullptr) { return; }
        abi::EventParamsT copy = p;
        eventCb(id, abi::Tuner_A, &copy, cbContext);
    }

    void fireDeviceRemoved() {
        abi::EventParamsT p{};
        fireEvent(abi::DeviceRemoved, p);
    }

    void fireOverload(bool detected) {
        abi::EventParamsT p{};
        p.powerOverloadParams.powerOverloadChangeType =
            detected ? abi::Overload_Detected : abi::Overload_Corrected;
        fireEvent(abi::PowerOverloadChange, p);
    }

    void fireGainChange(double currGain) {
        abi::EventParamsT p{};
        p.gainParams.currGain = currGain;
        fireEvent(abi::GainChange, p);
    }

    // --- helpers for the assertions --------------------------------------

    bool called(const std::string& what) const {
        for (const std::string& c : calls) {
            if (c == what) { return true; }
        }
        return false;
    }

    // The index of the first call equal to `what`, or -1.
    int indexOf(const std::string& what) const {
        for (std::size_t i = 0; i < calls.size(); ++i) {
            if (calls[i] == what) { return static_cast<int>(i); }
        }
        return -1;
    }

    // The index of the first call that starts with `prefix`, or -1. The
    // parameterised calls are logged as "SelectDevice(tuner=1,mode=1)" and
    // "Update(0x00020000,0x00000000)", so a test can ask either exactly or by
    // shape.
    int indexStarting(const std::string& prefix) const {
        for (std::size_t i = 0; i < calls.size(); ++i) {
            if (calls[i].size() >= prefix.size() &&
                calls[i].compare(0, prefix.size(), prefix) == 0) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    int countStarting(const std::string& prefix) const {
        int n = 0;
        for (const std::string& c : calls) {
            if (c.size() >= prefix.size() && c.compare(0, prefix.size(), prefix) == 0) { ++n; }
        }
        return n;
    }

    std::string joined() const {
        std::string s;
        for (const std::string& c : calls) {
            if (!s.empty()) { s += " "; }
            s += c;
        }
        return s;
    }

    static std::string updateCall(unsigned int reason, unsigned int ext1) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Update(0x%08x,0x%08x)", reason, ext1);
        return std::string(buf);
    }

private:
    static FakeSdrPlayApi*& instance() {
        static FakeSdrPlayApi* p = nullptr;
        return p;
    }

    void note(std::string s) { calls.push_back(std::move(s)); }

    // A call that has to queue behind a worker parked inside Update, because
    // the vendor DLL lets one call at a time near a device. `entered` records
    // that this call went in while the device was wedged, which is the fact a
    // test wants: elapsed time alone cannot tell "we did not call it" from "we
    // called it and it happened to be quick".
    static void queueBehindWedgedDevice(FakeSdrPlayApi* f, std::atomic<bool>& entered) {
        if (!f->wedgedDeviceBlocksTeardown.load() || f->updateHangDepth.load() <= 0) { return; }
        entered.store(true);
        while (f->updateHangDepth.load() > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    std::vector<abi::DeviceT> devices;
    abi::StreamCallbackT streamA = nullptr;
    abi::StreamCallbackT streamB = nullptr;
    abi::EventCallbackT eventCb = nullptr;
    void* cbContext = nullptr;
    // Any non-null value; the driver only ever passes it back to us.
    void* handle = reinterpret_cast<void*>(static_cast<std::uintptr_t>(0xD1A1));

    // --- the thunks -------------------------------------------------------

    static abi::ErrT thunkOpen() {
        FakeSdrPlayApi* f = instance();
        if (f == nullptr) { return abi::Fail; }
        f->note("Open");
        ++f->openCount;
        return f->openResult;
    }

    static abi::ErrT thunkClose() {
        FakeSdrPlayApi* f = instance();
        if (f == nullptr) { return abi::Fail; }
        f->note("Close");
        ++f->closeCount;
        return abi::Success;
    }

    static abi::ErrT thunkApiVersion(float* v) {
        FakeSdrPlayApi* f = instance();
        if (f == nullptr || v == nullptr) { return abi::Fail; }
        f->note("ApiVersion");
        *v = f->version;
        return abi::Success;
    }

    static abi::ErrT thunkLock() {
        FakeSdrPlayApi* f = instance();
        if (f == nullptr) { return abi::Fail; }
        f->note("LockDeviceApi");
        return abi::Success;
    }

    static abi::ErrT thunkUnlock() {
        FakeSdrPlayApi* f = instance();
        if (f == nullptr) { return abi::Fail; }
        f->note("UnlockDeviceApi");
        return abi::Success;
    }

    static abi::ErrT thunkGetDevices(abi::DeviceT* out, unsigned int* n, unsigned int maxDevs) {
        FakeSdrPlayApi* f = instance();
        if (f == nullptr || out == nullptr || n == nullptr) { return abi::Fail; }
        f->note("GetDevices");
        // A SERVICE THAT NEVER ANSWERS - the one behaviour this fake could not
        // express before, and the one that produced a hang report from the
        // field (0.96.1). The real API takes no timeout and cannot be
        // cancelled, so the only faithful imitation is to not return. Polled
        // rather than parked on a condition variable so the ABANDONED worker
        // can still leave when the test releases it, instead of holding a
        // vendor call open past the end of the process.
        if (f->hangInGetDevices.load()) {
            f->insideGetDevices.store(true);
            while (!f->releaseHang.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            f->leftGetDevices.store(true);
        }
        unsigned int count = 0;
        for (const abi::DeviceT& d : f->devices) {
            if (count >= maxDevs) { break; }
            out[count++] = d;
        }
        *n = count;
        return abi::Success;
    }

    static abi::ErrT thunkSelectDevice(abi::DeviceT* d) {
        FakeSdrPlayApi* f = instance();
        if (f == nullptr || d == nullptr) { return abi::Fail; }
        char buf[64];
        std::snprintf(buf, sizeof(buf), "SelectDevice(tuner=%d,mode=%d)",
                      static_cast<int>(d->tuner), static_cast<int>(d->rspDuoMode));
        f->note(buf);
        ++f->selectCount;
        if (f->selectResult != abi::Success) { return f->selectResult; }
        d->dev = f->handle;
        return abi::Success;
    }

    static abi::ErrT thunkReleaseDevice(abi::DeviceT* d) {
        FakeSdrPlayApi* f = instance();
        if (f == nullptr || d == nullptr) { return abi::Fail; }
        f->note("ReleaseDevice");
        queueBehindWedgedDevice(f, f->releaseEnteredWhileWedged);
        ++f->releaseCount;
        d->dev = nullptr;
        return abi::Success;
    }

    static const char* thunkGetErrorString(abi::ErrT err) {
        switch (err) {
            case abi::Success: return "Success";
            case abi::Fail: return "Fail";
            case abi::HwVerError: return "HwVerError";
            case abi::ServiceNotResponding: return "ServiceNotResponding";
            default: break;
        }
        return "Error";
    }

    static abi::ErrT thunkDebugEnable(void*, abi::DbgLvlT) {
        FakeSdrPlayApi* f = instance();
        if (f != nullptr) { f->note("DebugEnable"); }
        return abi::Success;
    }

    static abi::ErrT thunkGetDeviceParams(void* dev, abi::DeviceParamsT** out) {
        FakeSdrPlayApi* f = instance();
        if (f == nullptr || out == nullptr) { return abi::Fail; }
        f->note("GetDeviceParams");
        if (f->getParamsResult != abi::Success) { return f->getParamsResult; }
        if (dev != f->handle) { return abi::InvalidParam; }
        *out = &f->params;
        return abi::Success;
    }

    static abi::ErrT thunkInit(void* dev, abi::CallbackFnsT* cbs, void* ctx) {
        FakeSdrPlayApi* f = instance();
        if (f == nullptr || cbs == nullptr) { return abi::Fail; }
        f->note("Init");
        if (f->initResult != abi::Success) { return f->initResult; }
        if (dev != f->handle) { return abi::InvalidParam; }
        f->streamA = cbs->StreamACbFn;
        f->streamB = cbs->StreamBCbFn;
        f->eventCb = cbs->EventCbFn;
        f->cbContext = ctx;
        f->initialised = true;

        InitSnapshot s;
        s.taken = true;
        s.fsHz = f->devParams.fsFreq.fsHz;
        s.rfHz = f->chA.tunerParams.rfFreq.rfHz;
        s.gRdB = f->chA.tunerParams.gain.gRdB;
        s.LNAstate = f->chA.tunerParams.gain.LNAstate;
        s.bwType = static_cast<int>(f->chA.tunerParams.bwType);
        s.ifType = static_cast<int>(f->chA.tunerParams.ifType);
        s.agcEnable = static_cast<int>(f->chA.ctrlParams.agc.enable);
        s.agcSetPoint = f->chA.ctrlParams.agc.setPoint_dBfs;
        s.decEnable = f->chA.ctrlParams.decimation.enable;
        s.decFactor = f->chA.ctrlParams.decimation.decimationFactor;
        s.dcEnable = f->chA.ctrlParams.dcOffset.DCenable;
        s.iqEnable = f->chA.ctrlParams.dcOffset.IQenable;
        f->atInit = s;
        return abi::Success;
    }

    static abi::ErrT thunkUninit(void*) {
        FakeSdrPlayApi* f = instance();
        if (f == nullptr) { return abi::Fail; }
        f->note("Uninit");
        // Noted first, then queued behind a wedged device - so the call is on
        // the record even when it never comes back out. See updateHangDepth.
        queueBehindWedgedDevice(f, f->uninitEnteredWhileWedged);
        // The API's contract: Uninit returns with the callbacks stopped.
        f->streamA = nullptr;
        f->streamB = nullptr;
        f->eventCb = nullptr;
        f->cbContext = nullptr;
        f->initialised = false;
        return abi::Success;
    }

    static abi::ErrT thunkUpdate(void* dev, abi::TunerSelectT tuner, abi::ReasonForUpdateT reason,
                                 abi::ReasonForUpdateExt1T ext1) {
        FakeSdrPlayApi* f = instance();
        if (f == nullptr) { return abi::Fail; }
        (void) dev;
        (void) tuner;
        f->note(updateCall(static_cast<unsigned int>(reason), static_cast<unsigned int>(ext1)));
        // A SERVICE THAT NEVER ANSWERS A CONTROL. See hangInUpdate: noted
        // first, so the call is on the record before it disappears, and
        // returned without an acknowledgement once released.
        if (f->hangInUpdate.load()) {
            f->insideUpdate.store(true);
            f->updateHangDepth.fetch_add(1);
            while (!f->releaseUpdateHang.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            f->updateHangDepth.fetch_sub(1);
            f->leftUpdate.store(true);
            return f->updateResult;
        }
        if (f->updateResult != abi::Success) { return f->updateResult; }

        if (f->autoAck && f->streamA != nullptr) {
            abi::StreamCbParamsT p{};
            p.numSamples = 0;
            if ((reason & abi::Update_Tuner_Gr) != 0) { p.grChanged = 1; }
            if ((reason & abi::Update_Tuner_Frf) != 0) { p.rfChanged = 1; }
            if ((reason & abi::Update_Dev_Fs) != 0) { p.fsChanged = 1; }
            if (p.grChanged != 0 || p.rfChanged != 0 || p.fsChanged != 0) {
                f->streamA(nullptr, nullptr, &p, 0, 0, f->cbContext);
            }
        }
        return abi::Success;
    }

    static abi::ErrT thunkSwap(void* dev, abi::TunerSelectT* tuner,
                               abi::RspDuoAmPortSelectT amPort) {
        FakeSdrPlayApi* f = instance();
        if (f == nullptr || tuner == nullptr) { return abi::Fail; }
        (void) dev;
        (void) amPort;
        f->note("SwapRspDuoActiveTuner");
        if (f->swapResult != abi::Success) { return f->swapResult; }
        *tuner = (*tuner == abi::Tuner_A) ? abi::Tuner_B : abi::Tuner_A;
        return abi::Success;
    }
};

}  // namespace fakesdrplay
