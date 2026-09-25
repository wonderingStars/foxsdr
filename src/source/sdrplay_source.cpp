// SdrPlaySource - see sdrplay_source.hpp for the argument.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/sdrplay_source.hpp"

#include "core/diag_log.hpp"
#include "core/i18n.hpp"
#include "core/utf8_text.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <future>
#include <limits>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace cascade::source {
namespace abi = sdrplay_abi;

namespace {

std::atomic<unsigned long long> g_linksStranded{0};

// A stranded Link is kept alive here for the life of the process rather than
// freed under a service thread that is still inside it. Deliberately never
// emptied: the whole point is that nothing here is safe to destroy, and a
// handful of rings is a price worth paying once per wedged teardown.
std::mutex g_graveyardMutex;
std::vector<std::shared_ptr<void>> g_graveyard;

void strandLink(std::shared_ptr<void> link) {
    {
        std::lock_guard<std::mutex> lk(g_graveyardMutex);
        g_graveyard.push_back(std::move(link));
    }
    g_linksStranded.fetch_add(1, std::memory_order_relaxed);
}

std::string lowerCopy(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

// WHAT A SETTER WROTE INTO THE PARAMETER BLOCK, SO A REFUSED UPDATE CAN PUT IT
// BACK (0.99.36, the review of 0c59853). Every setter writes its fields into
// the service's block and THEN sends the Update; on a refusal the radio stayed
// where it was but the block claimed the new value - which the unchanged-
// frequency check then trusted, and which Init programs on the next start.
// Each field is saved as it is about to be written; undo() restores them in
// reverse order. Used under devMutex_ like the block itself.
class BlockRollback {
public:
    template <typename T>
    void save(T& field) {
        T* const at = &field;
        const T was = field;
        undo_.push_back([at, was]() { *at = was; });
    }
    void undo() {
        for (auto it = undo_.rbegin(); it != undo_.rend(); ++it) { (*it)(); }
        undo_.clear();
    }

private:
    std::vector<std::function<void()>> undo_;
};

// ...EXCEPT after an abandoned control: a worker of ours is still inside the
// vendor's Update and may be reading the block, the device is dead for good,
// and nothing will ever be sent for it again - so the block is left alone
// rather than written under that thread.
void undoRefused(BlockRollback& rb, bool controlAbandoned) {
    if (!controlAbandoned) { rb.undo(); }
}

// The API answers an error code; this is what goes in the log and in
// lastError(). GetErrorString is optional in the table (a fake need not have
// it), so the number is always printed and the string appended when there is
// one.
std::string errText(const abi::Api& api, abi::ErrT err) {
    char buf[128];
    const char* s = (api.GetErrorString != nullptr) ? api.GetErrorString(err) : nullptr;
    if (s != nullptr) {
        std::snprintf(buf, sizeof(buf), "%s (%d)", s, static_cast<int>(err));
    } else {
        std::snprintf(buf, sizeof(buf), "error %d", static_cast<int>(err));
    }
    return std::string(buf);
}

}  // namespace

// --- loading the API ------------------------------------------------------

const char* sdrPlayApiDllPath() {
    // SDRplay's own documented location for the 64-bit library. Their
    // installer writes it here and does NOT put it on PATH.
    return "C:\\Program Files\\SDRplay\\API\\x64\\sdrplay_api.dll";
}

#if !defined(_WIN32)
const char* sdrPlayApiSoName() {
    // SDRplay's Linux .run installer (SDRplay_RSP_API-Linux-<ver>.run) drops
    // libsdrplay_api.so.<major>.<minor> into /usr/local/lib (or /usr/local/lib64
    // on Fedora-family systems) and runs ldconfig, which is what registers
    // this SONAME - confirmed against SoapySDRPlay3, the reference open-source
    // consumer of this API, and against SDRplay's own community documentation
    // for the Linux .run installer. The "3" is the API's own major version,
    // unrelated to any FoxSDR or Ubuntu version. Unlike the Windows install,
    // this location IS on the loader's ordinary search path once ldconfig has
    // run, so - unlike sdrPlayApiDllPath() above - there is no separate
    // hardcoded directory to try first: dlopen() with the bare SONAME already
    // searches it.
    return "libsdrplay_api.so.3";
}
#endif

namespace {

#if defined(_WIN32)
template <typename Fn>
bool resolve(HMODULE mod, const char* name, Fn& out, std::string& missing) {
    FARPROC p = ::GetProcAddress(mod, name);
    if (p == nullptr) {
        if (!missing.empty()) { missing += ", "; }
        missing += name;
        return false;
    }
    out = reinterpret_cast<Fn>(reinterpret_cast<void*>(p));
    return true;
}
#else
template <typename Fn>
bool resolve(void* mod, const char* name, Fn& out, std::string& missing) {
    // dlsym's result is a void*: not implicitly convertible to a function
    // pointer under ISO C (the standard the compiler is entitled to reject
    // this on), which is why POSIX itself pushes the cast through a same-size
    // object representation rather than a direct reinterpret_cast<Fn>(void*).
    void* p = ::dlsym(mod, name);
    if (p == nullptr) {
        if (!missing.empty()) { missing += ", "; }
        missing += name;
        return false;
    }
    std::memcpy(&out, &p, sizeof(out));
    return true;
}
#endif

void loadInto(abi::Api& api) {
#if defined(_WIN32)
    HMODULE mod = ::LoadLibraryA(sdrPlayApiDllPath());
    std::string from = sdrPlayApiDllPath();
    if (mod == nullptr) {
        // Second, and only second: a user who has arranged their own copy.
        // Putting the bare name first would let a stray sdrplay_api.dll beside
        // some other application pre-empt the real install.
        mod = ::LoadLibraryA("sdrplay_api.dll");
        from = "sdrplay_api.dll (loader search path)";
    }
    if (mod == nullptr) {
        api.resolved = false;
        api.loadDetail = "sdrplay_api.dll not found";
        return;
    }

    std::string missing;
    bool ok = true;
    ok &= resolve(mod, "sdrplay_api_Open", api.Open, missing);
    ok &= resolve(mod, "sdrplay_api_Close", api.Close, missing);
    ok &= resolve(mod, "sdrplay_api_ApiVersion", api.ApiVersion, missing);
    ok &= resolve(mod, "sdrplay_api_LockDeviceApi", api.LockDeviceApi, missing);
    ok &= resolve(mod, "sdrplay_api_UnlockDeviceApi", api.UnlockDeviceApi, missing);
    ok &= resolve(mod, "sdrplay_api_GetDevices", api.GetDevices, missing);
    ok &= resolve(mod, "sdrplay_api_SelectDevice", api.SelectDevice, missing);
    ok &= resolve(mod, "sdrplay_api_ReleaseDevice", api.ReleaseDevice, missing);
    ok &= resolve(mod, "sdrplay_api_GetErrorString", api.GetErrorString, missing);
    ok &= resolve(mod, "sdrplay_api_GetLastError", api.GetLastError, missing);
    ok &= resolve(mod, "sdrplay_api_DebugEnable", api.DebugEnable, missing);
    ok &= resolve(mod, "sdrplay_api_GetDeviceParams", api.GetDeviceParams, missing);
    ok &= resolve(mod, "sdrplay_api_Init", api.Init, missing);
    ok &= resolve(mod, "sdrplay_api_Uninit", api.Uninit, missing);
    ok &= resolve(mod, "sdrplay_api_Update", api.Update, missing);
    // Only an RSPduo needs this one, so a library without it is still usable
    // for every other model - resolved, but not required.
    std::string swapMissing;
    resolve(mod, "sdrplay_api_SwapRspDuoActiveTuner", api.SwapRspDuoActiveTuner, swapMissing);

    api.resolved = ok;
    api.loadDetail = ok ? from : ("entry points missing from " + from + ": " + missing);
#else
    // RTLD_NOW rather than RTLD_LAZY: a symbol missing from an installed-but-
    // mismatched API build should surface HERE, as a load failure with a
    // reason, not later as a segfault the first time some rarely-called entry
    // point (SwapRspDuoActiveTuner, say) is finally reached.
    void* mod = ::dlopen(sdrPlayApiSoName(), RTLD_NOW);
    std::string from = sdrPlayApiSoName();
    if (mod == nullptr) {
        // Second, and only second: a dev machine with just the unversioned
        // symlink (no ldconfig run, or a hand-built API), mirroring the
        // Windows branch's own fallback order above.
        mod = ::dlopen("libsdrplay_api.so", RTLD_NOW);
        from = "libsdrplay_api.so (loader search path)";
    }
    if (mod == nullptr) {
        api.resolved = false;
        const char* err = ::dlerror();
        api.loadDetail = std::string("libsdrplay_api.so.3 not found") +
                         (err != nullptr ? (std::string(": ") + err) : std::string());
        return;
    }

    std::string missing;
    bool ok = true;
    ok &= resolve(mod, "sdrplay_api_Open", api.Open, missing);
    ok &= resolve(mod, "sdrplay_api_Close", api.Close, missing);
    ok &= resolve(mod, "sdrplay_api_ApiVersion", api.ApiVersion, missing);
    ok &= resolve(mod, "sdrplay_api_LockDeviceApi", api.LockDeviceApi, missing);
    ok &= resolve(mod, "sdrplay_api_UnlockDeviceApi", api.UnlockDeviceApi, missing);
    ok &= resolve(mod, "sdrplay_api_GetDevices", api.GetDevices, missing);
    ok &= resolve(mod, "sdrplay_api_SelectDevice", api.SelectDevice, missing);
    ok &= resolve(mod, "sdrplay_api_ReleaseDevice", api.ReleaseDevice, missing);
    ok &= resolve(mod, "sdrplay_api_GetErrorString", api.GetErrorString, missing);
    ok &= resolve(mod, "sdrplay_api_GetLastError", api.GetLastError, missing);
    ok &= resolve(mod, "sdrplay_api_DebugEnable", api.DebugEnable, missing);
    ok &= resolve(mod, "sdrplay_api_GetDeviceParams", api.GetDeviceParams, missing);
    ok &= resolve(mod, "sdrplay_api_Init", api.Init, missing);
    ok &= resolve(mod, "sdrplay_api_Uninit", api.Uninit, missing);
    ok &= resolve(mod, "sdrplay_api_Update", api.Update, missing);
    // Only an RSPduo needs this one, so a library without it is still usable
    // for every other model - resolved, but not required.
    std::string swapMissing;
    resolve(mod, "sdrplay_api_SwapRspDuoActiveTuner", api.SwapRspDuoActiveTuner, swapMissing);

    api.resolved = ok;
    api.loadDetail = ok ? from : ("entry points missing from " + from + ": " + missing);
#endif
}

}  // namespace

const sdrplay_abi::Api& processSdrPlayApi() {
    // Function-local statics: resolved on first use, never unloaded, and
    // thread-safe initialisation is the language's problem rather than ours.
    // Two of them rather than one initialised from a lambda's return value,
    // because Api carries the session mutex and is deliberately non-copyable.
    static abi::Api api;
    static const bool once = [] {
        loadInto(api);
        core::diagLogf("source: SDRplay API - %s", api.loadDetail.c_str());
        return true;
    }();
    (void) once;
    return api;
}

// USER COPY, in the user's language: it is drawn under the Source row and
// given as the reason a device would not open. With English in force tr()
// answers the English these sentences always were.
std::string sdrPlayApiAdvice(bool resolved, float version) {
    if (!resolved) {
        return cascade::i18n::tr(
            "SDRplay radios need the SDRplay API from sdrplay.com, version 3.x - install it "
            "and restart FoxSDR.");
    }
    if (version > 0.0f && !abi::versionAtLeast(version, abi::kMinApiVersion)) {
        return cascade::core::formatText(
            cascade::i18n::tr(
                "The installed SDRplay API is version %.2f; FoxSDR needs %.2f or newer - "
                "update it from sdrplay.com and restart FoxSDR."),
            static_cast<double>(version), static_cast<double>(abi::kMinApiVersion));
    }
    return std::string();
}

namespace {

// The last enumeration's skip reason. A mutex rather than an atomic because
// it is a std::string written on whatever thread enumerated and read on the
// GUI's - the same rule the driver's own error slot follows.
std::mutex& enumSkipMutex() {
    static std::mutex m;
    return m;
}
std::string& enumSkipSlot() {
    static std::string s;
    return s;
}

void setEnumerationSkip(std::string reason) {
    std::lock_guard<std::mutex> lk(enumSkipMutex());
    enumSkipSlot() = std::move(reason);
}

}  // namespace

std::string sdrPlayLastEnumerationSkip() {
    std::lock_guard<std::mutex> lk(enumSkipMutex());
    return enumSkipSlot();
}

std::string sdrPlayPanelAdvice(bool resolved, float version, const std::string& enumerationSkip) {
    // THE ENUMERATION'S REASON WINS, because it is the only one of the two
    // that met the API. It was opened, asked its version and closed again, so
    // it knows the number; the load result on its own only knows the DLL was
    // there. When there is no reason - nothing has enumerated yet, or the
    // last one reached the device list - fall back to what the load alone can
    // say, which is what this panel showed before and is empty on a healthy
    // install.
    if (!enumerationSkip.empty()) { return enumerationSkip; }
    return sdrPlayApiAdvice(resolved, version);
}

// --- the session ----------------------------------------------------------

namespace {

// Open the API once per process per table, check the version, and hand out a
// reference count. See sdrplay_abi::Api for why the count lives in the table.
bool sessionAcquire(const abi::Api& api, std::string& error) {
    if (!api.resolved) {
        error = sdrPlayApiAdvice(false, 0.0f);
        return false;
    }
    std::lock_guard<std::mutex> lk(api.sessionMutex);
    // A LOST SESSION IS NOT HANDED OUT AGAIN - see markSessionLost. Checked
    // before the count, because the count is exactly what keeps the stale
    // session "open": an orphaned reference is never released.
    if (api.sessionLost) {
        error = sdrPlaySessionLostSentence();
        return false;
    }
    if (api.sessions > 0) {
        ++api.sessions;
        return true;
    }
    const abi::ErrT err = api.Open();
    if (err != abi::Success) {
        error = "the SDRplay service did not answer: " + errText(api, err) +
                ". Check that the SDRplay API service is running.";
        return false;
    }
    float ver = 0.0f;
    const abi::ErrT verr = api.ApiVersion(&ver);
    if (verr != abi::Success) {
        api.Close();
        error = "the SDRplay API would not report its version: " + errText(api, verr);
        return false;
    }
    if (!abi::versionAtLeast(ver, abi::kMinApiVersion)) {
        api.Close();
        error = sdrPlayApiAdvice(true, ver);
        return false;
    }
    api.version = ver;
    api.sessions = 1;
    return true;
}

void sessionRelease(const abi::Api& api) {
    std::lock_guard<std::mutex> lk(api.sessionMutex);
    if (api.sessions <= 0) { return; }
    --api.sessions;
    if (api.sessions == 0 && api.Close != nullptr) {
        api.Close();
        api.version = 0.0f;
    }
}

// THE PROCESS'S SESSION IS FINISHED, NOT JUST THIS DEVICE'S (0.99.28).
//
// The file header has always said that once a worker is abandoned inside the
// vendor DLL, or the service declares itself gone, "this process's SDRplay
// session is finished until FoxSDR is restarted" - but only the device that saw
// it obeyed. Its session is ORPHANED rather than released (closing it would be
// another unbounded call into a DLL still holding our thread), so the process
// table's count never returns to zero and every later sessionAcquire handed
// the SAME stale session to the next scan and the next open(). The 0.99.27
// crash report is that scan: an access violation inside the vendor's own
// GetDevices copy (sdrplay_api.dll +7098 -> VCRUNTIME140), minutes after the
// log said a retune had been abandoned and the radio closed without
// ReleaseDevice. Latched on the table, so it is process-wide in the field and
// per-fake in the tests, and logged once.
void markSessionLost(const abi::Api& api, const char* why) {
    bool first = false;
    {
        std::lock_guard<std::mutex> lk(api.sessionMutex);
        first = !api.sessionLost;
        api.sessionLost = true;
    }
    if (first) {
        core::diagWarnf("source: SDRplay API session lost - %s; no further SDRplay API calls are "
                        "made until FoxSDR is restarted",
                        why);
    }
}

bool sessionIsLost(const abi::Api& api) {
    std::lock_guard<std::mutex> lk(api.sessionMutex);
    return api.sessionLost;
}

}  // namespace

// --- the pure halves ------------------------------------------------------

std::string sdrPlayModelName(unsigned char hwVer) {
    switch (hwVer) {
        case abi::kRsp1: return "RSP1";
        case abi::kRsp1A: return "RSP1A";
        case abi::kRsp1B: return "RSP1B";
        case abi::kRsp2: return "RSP2";
        case abi::kRspDuo: return "RSPduo";
        case abi::kRspDx: return "RSPdx";
        case abi::kRspDxR2: return "RSPdx-R2";
        default: break;
    }
    // A number the user can quote is worth more than the word UNKNOWN: it is
    // what tells us which model SDRplay shipped after this was written.
    char buf[32];
    std::snprintf(buf, sizeof(buf), "RSP (hw %u)", static_cast<unsigned>(hwVer));
    return std::string(buf);
}

std::string sdrPlayLabel(const abi::DeviceT& dev) {
    // SerNo is a fixed-size char array the service fills; it is not promised
    // to be terminated if it were ever full, so bound the read.
    const std::size_t maxLen = sizeof(dev.SerNo);
    std::size_t len = 0;
    while (len < maxLen && dev.SerNo[len] != '\0') { ++len; }
    const std::string serial(dev.SerNo, len);
    std::string label = "SDRplay " + sdrPlayModelName(dev.hwVer);
    if (!serial.empty()) { label += " (serial " + serial + ")"; }
    return label;
}

std::vector<std::string> sdrPlayAntennas(unsigned char hwVer, abi::RspDuoModeT duoMode) {
    switch (hwVer) {
        case abi::kRsp2:
            return {"Antenna A", "Antenna B", "Hi-Z"};
        case abi::kRspDx:
        case abi::kRspDxR2:
            return {"Antenna A", "Antenna B", "Antenna C"};
        case abi::kRspDuo:
            // Single-tuner is the mode this stage selects; the dual-tuner,
            // master and slave modes are a second receiver's worth of work and
            // the Source section has one column for one radio.
            if (duoMode == abi::RspDuoMode_Single_Tuner || duoMode == abi::RspDuoMode_Unknown) {
                return {"Tuner 1", "Tuner 2"};
            }
            return {"Tuner 1"};
        default: break;
    }
    return {"RX"};
}

int sdrPlayLnaStateCount(unsigned char hwVer) {
    // SoapySDRPlay3 Settings.cpp getGainRange states the MAXIMUM LNAstate per
    // model; the count is that plus one. These are the broadest band's; the
    // hardware has fewer in some bands and the API clamps, which is why
    // setGainDb reports back what was programmed rather than what was asked.
    switch (hwVer) {
        case abi::kRsp1: return 4;    // 0..3
        case abi::kRsp2: return 9;    // 0..8
        case abi::kRspDuo: return 10; // 0..9
        case abi::kRsp1A: return 10;  // 0..9
        case abi::kRsp1B: return 10;  // 0..9
        case abi::kRspDx: return 28;  // 0..27
        case abi::kRspDxR2: return 28;
        default: break;
    }
    return 10;
}

std::vector<double> sdrPlaySupportedRatesHz() {
    return {62500.0,  96000.0,  125000.0, 192000.0,  250000.0,  384000.0,  500000.0,
            768000.0, 1000000.0, 2000000.0, 2048000.0, 3000000.0, 4000000.0, 5000000.0,
            6000000.0, 7000000.0, 8000000.0, 9000000.0, 10000000.0};
}

abi::BwMHzT sdrPlayBwForRate(double r) {
    if (r < 300000.0) { return abi::BW_0_200; }
    if (r < 600000.0) { return abi::BW_0_300; }
    if (r < 1536000.0) { return abi::BW_0_600; }
    if (r < 5000000.0) { return abi::BW_1_536; }
    if (r < 6000000.0) { return abi::BW_5_000; }
    if (r < 7000000.0) { return abi::BW_6_000; }
    if (r < 8000000.0) { return abi::BW_7_000; }
    return abi::BW_8_000;
}

bool sdrPlayRatePlan(double outputRateHz, SdrPlayRatePlan& out) {
    // THE FRONT END DOES NOT RUN BELOW 2 MS/s, so every rate below that is a
    // decimation of something faster, and WHICH something depends on the rate.
    // The reference's own table (SoapySDRPlay3
    // getInputSampleRateAndDecimation): the binary fractions of 2 MS/s come
    // from a 6 MHz LOW-IF front end decimated by powers of two, and the audio
    // rates (96k, 192k, 384k, 768k) come from a ZERO-IF front end running at
    // the rate times the decimation. Mixing the two up is how a receiver ends
    // up 1.62 MHz off frequency.
    const long long r = static_cast<long long>(std::llround(outputRateHz));
    out = SdrPlayRatePlan{};
    out.bwType = sdrPlayBwForRate(static_cast<double>(r));

    switch (r) {
        case 62500: out.ifType = abi::IF_1_620; out.decM = 32; out.decEnable = 1; out.fsHz = 6000000.0; return true;
        case 125000: out.ifType = abi::IF_1_620; out.decM = 16; out.decEnable = 1; out.fsHz = 6000000.0; return true;
        case 250000: out.ifType = abi::IF_1_620; out.decM = 8; out.decEnable = 1; out.fsHz = 6000000.0; return true;
        case 500000: out.ifType = abi::IF_1_620; out.decM = 4; out.decEnable = 1; out.fsHz = 6000000.0; return true;
        case 1000000: out.ifType = abi::IF_1_620; out.decM = 2; out.decEnable = 1; out.fsHz = 6000000.0; return true;
        case 2000000: out.ifType = abi::IF_1_620; out.decM = 1; out.decEnable = 0; out.fsHz = 6000000.0; return true;
        case 96000:
        case 192000:
        case 384000:
        case 768000: {
            out.ifType = abi::IF_Zero;
            out.decM = static_cast<unsigned int>(3072000 / r);  // 32, 16, 8, 4
            out.decEnable = 1;
            out.wideBandSignal = 1;
            out.fsHz = static_cast<double>(r) * out.decM;
            return true;
        }
        default: break;
    }

    // Above 2 MS/s the ADC runs at the output rate and nothing is decimated.
    for (double supported : sdrPlaySupportedRatesHz()) {
        if (std::llround(supported) == r && r > 2000000) {
            out.ifType = abi::IF_Zero;
            out.decM = 1;
            out.decEnable = 0;
            out.fsHz = static_cast<double>(r);
            return true;
        }
    }
    return false;
}

// --- enumeration ----------------------------------------------------------

namespace {

// THE HOLD-OFF, which is process-scope for the same reason the skip sentence
// is: there is one SDRplay API per process and one Source section looking at
// it. Zero means "not held off", which is the state every process starts in -
// so clearing it in a test restores the initial state rather than destroying
// something init() built.
std::mutex& holdOffMutex() {
    static std::mutex m;
    return m;
}
std::chrono::steady_clock::time_point& holdOffUntil() {
    static std::chrono::steady_clock::time_point t{};
    return t;
}

bool enumerationHeldOffAt(std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lk(holdOffMutex());
    return holdOffUntil() != std::chrono::steady_clock::time_point{} && now < holdOffUntil();
}

void armEnumerationHoldOff(std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lk(holdOffMutex());
    holdOffUntil() = now + kEnumerateHoldOff;
}

// The vendor half: everything that can block forever. Runs on the caller's
// thread when the service is healthy and on an abandoned worker when it is
// not, which is why it takes nothing by reference except the table itself -
// the table is process-scope and outlives any worker left inside it.
std::vector<NativeDeviceInfo> enumerateSdrPlayVendor(const abi::Api& api, std::string& skip) {
    std::vector<NativeDeviceInfo> out;
    skip.clear();
    std::string error;
    if (!sessionAcquire(api, error)) {
        core::diagLogf("source: SDRplay enumeration skipped - %s", error.c_str());
        // ...AND THE SCREEN GETS THE SAME SENTENCE THE LOG JUST GOT. Kept
        // verbatim rather than re-derived in the panel: the too-old case
        // knows a version number that only this call learned, and nothing
        // above this line can find it out again without opening the API a
        // second time. See sdrPlayLastEnumerationSkip.
        skip = error;
        return out;
    }

    abi::DeviceT devs[abi::kMaxDevices];
    std::memset(devs, 0, sizeof(devs));
    unsigned int n = 0;

    const abi::ErrT lerr = api.LockDeviceApi();
    if (lerr != abi::Success) {
        core::diagWarnf("source: SDRplay LockDeviceApi failed - %s", errText(api, lerr).c_str());
        sessionRelease(api);
        return out;
    }
    const abi::ErrT gerr = api.GetDevices(devs, &n, abi::kMaxDevices);
    api.UnlockDeviceApi();

    if (gerr != abi::Success) {
        core::diagWarnf("source: SDRplay GetDevices failed - %s", errText(api, gerr).c_str());
        sessionRelease(api);
        return out;
    }

    float ver = 0.0f;
    {
        std::lock_guard<std::mutex> lk(api.sessionMutex);
        ver = api.version;
    }
    // `valid` is padding before 3.08, so believing it on an older API would
    // hide every device behind a byte that means nothing.
    const bool trustValid = abi::versionAtLeast(ver, abi::kValidFieldSinceVersion);

    if (n > abi::kMaxDevices) { n = abi::kMaxDevices; }
    // The index counts only the devices that made it into the list, because
    // that is what open()'s "index=N" counts. Counting raw slots here and
    // valid ones there would make "index=1" mean two different radios on a bus
    // where one device is busy.
    unsigned int listed = 0;
    for (unsigned int i = 0; i < n; ++i) {
        if (trustValid && devs[i].valid == 0) { continue; }
        NativeDeviceInfo info;
        info.driver = "sdrplay";
        info.label = sdrPlayLabel(devs[i]);
        const std::size_t maxLen = sizeof(devs[i].SerNo);
        std::size_t len = 0;
        while (len < maxLen && devs[i].SerNo[len] != '\0') { ++len; }
        if (len > 0) {
            info.args = "serial=" + std::string(devs[i].SerNo, len);
        } else {
            char buf[24];
            std::snprintf(buf, sizeof(buf), "index=%u", listed);
            info.args = buf;
        }
        ++listed;
        out.push_back(std::move(info));
    }

    sessionRelease(api);
    return out;
}

// What the worker hands back. Held through a shared_ptr so that abandoning the
// worker cannot leave it writing into the caller's stack frame.
struct EnumerateResult {
    std::vector<NativeDeviceInfo> devices;
    std::string skip;
};

}  // namespace

const char* sdrPlayServiceHungSentence() {
    // The number in the sentence is kEnumerateWait, spelled out rather than
    // formatted, because this string is pinned by a test and a formatted one
    // would drift out of step with the constant silently either way.
    static_assert(kEnumerateWait == std::chrono::milliseconds(3000),
                  "the sentence below quotes three seconds");
    return "the SDRplay service did not answer within 3 s - restart the SDRplay API service";
}

const char* sdrPlayControlHungSentence() {
    // As above: the number is kControlWait, spelled out and pinned, because a
    // formatted sentence and a changed constant drift apart in silence.
    static_assert(SdrPlaySource::kControlWait == std::chrono::milliseconds(1000),
                  "the sentence below quotes one second");
    // "...THEN RESTART FoxSDR", not "then open the radio again" (0.99.36):
    // an abandoned control marks the process's session lost (0.99.28), and a
    // lost session refuses every open until FoxSDR restarts - so the old
    // instruction closed the dead radio, failed to open it, and left the user
    // on the signal generator for having followed it.
    return "the SDRplay service did not answer within 1 s - restart the SDRplay API service, "
           "then restart FoxSDR";
}

const char* sdrPlayStreamStalledSentence() {
    // The number is kStreamStallLimit, spelled out and pinned like the two
    // above.
    static_assert(SdrPlaySource::kStreamStallLimit == std::chrono::milliseconds(5000),
                  "the sentence below quotes five seconds");
    return "the SDRplay service stopped delivering samples (nothing for 5 s) - restart the "
           "SDRplay API service, then restart FoxSDR";
}

const char* sdrPlaySessionLostSentence() {
    return "the SDRplay service stopped answering while FoxSDR was using it, so FoxSDR will not "
           "call the SDRplay API again until it is restarted - restart the SDRplay API service, "
           "then restart FoxSDR";
}

bool sdrPlayEnumerationHeldOff() {
    return enumerationHeldOffAt(std::chrono::steady_clock::now());
}

void sdrPlayClearEnumerationHoldOffForTest() {
    std::lock_guard<std::mutex> lk(holdOffMutex());
    holdOffUntil() = std::chrono::steady_clock::time_point{};
}

std::vector<NativeDeviceInfo> enumerateSdrPlayWith(const abi::Api& api) {
    // A LOST SESSION BEFORE ANYTHING, and no worker either: nothing is going
    // to be asked, so there is nothing to bound. The 0.99.27 crash was this
    // scan's GetDevices running through a session the driver had already
    // orphaned - see markSessionLost. Unlike the hold-off this never expires.
    if (sessionIsLost(api)) {
        setEnumerationSkip(sdrPlaySessionLostSentence());
        return {};
    }

    // THE HOLD-OFF NEXT, and it touches the API not at all. A service that
    // wedged a moment ago is still wedged, and the source combo scans every
    // time it opens - so without this, each of those would spend another
    // kEnumerateWait of GUI thread and abandon another worker inside it.
    const auto now = std::chrono::steady_clock::now();
    if (enumerationHeldOffAt(now)) {
        // The sentence is re-asserted rather than left standing, because a
        // Soapy scan or another driver's enumeration may have written over it
        // in between and the panel must still say what to do.
        setEnumerationSkip(sdrPlayServiceHungSentence());
        return {};
    }

    // THE VENDOR HALF ON A WORKER, because none of the calls it makes can be
    // cancelled or given a timeout (see the file header). What is bounded is
    // our WAIT on it; the worker itself is either joined, or abandoned and
    // never spoken to again.
    //
    // The promise is a shared_ptr, and that is load-bearing rather than tidy:
    // an abandoned worker outlives this frame, this function and possibly this
    // Source section, so anything it writes into must be owned by the worker
    // too. `api` is safe to capture because the table is process-scope and
    // never unloaded.
    auto result = std::make_shared<std::promise<EnumerateResult>>();
    std::future<EnumerateResult> done = result->get_future();
    std::thread worker([&api, result]() {
        EnumerateResult r;
        r.devices = enumerateSdrPlayVendor(api, r.skip);
        result->set_value(std::move(r));
    });

    if (done.wait_for(kEnumerateWait) != std::future_status::ready) {
        // ABANDONED. Not joined, not killed, not signalled: the thread is
        // parked inside the vendor DLL and there is no handle we can close to
        // bring it back. It holds everything it needs through the shared
        // promise, so it can finish (or not) harmlessly. What this process
        // must never do is call into that API again from here, which is what
        // the hold-off below arranges.
        worker.detach();
        armEnumerationHoldOff(now);
        core::diagWarnf("source: SDRplay enumeration abandoned - %s",
                        sdrPlayServiceHungSentence());
        core::diagLogf("source: SDRplay scans are held off for %lld s",
                       static_cast<long long>(
                           std::chrono::duration_cast<std::chrono::seconds>(kEnumerateHoldOff)
                               .count()));
        setEnumerationSkip(sdrPlayServiceHungSentence());
        return {};
    }

    worker.join();
    EnumerateResult r = done.get();
    // The API answered - whatever it last refused for is over, including a
    // hold-off that has since expired.
    setEnumerationSkip(r.skip);
    return std::move(r.devices);
}

std::vector<NativeDeviceInfo> enumerateSdrPlay() {
    return enumerateSdrPlayWith(processSdrPlayApi());
}

// --- the driver -----------------------------------------------------------

SdrPlaySource::~SdrPlaySource() { closeDevice(); }

void SdrPlaySource::setApiForTest(const abi::Api* api) {
    std::lock_guard<std::mutex> lk(devMutex_);
    api_ = api;
}

const abi::Api& SdrPlaySource::api() const {
    return (api_ != nullptr) ? *api_ : processSdrPlayApi();
}

unsigned long long SdrPlaySource::linksStranded() {
    return g_linksStranded.load(std::memory_order_relaxed);
}

// --- errors ---------------------------------------------------------------

void SdrPlaySource::setErrorOn(Link& link, std::string msg) {
    std::lock_guard<std::mutex> lk(link.errorMutex);
    link.lastError = std::move(msg);
}

void SdrPlaySource::noteFaultOn(Link& link, const char* what, const std::string& detail) {
    {
        std::lock_guard<std::mutex> lk(link.errorMutex);
        link.lastError = std::string(what) + ": " + detail;
        link.faulted = true;
        link.deviceDead = true;
        link.deadWhat = what;
    }
    // Wake a read() that is parked: a dead device must not cost the pipeline
    // a whole kReadWait before it hears about it.
    {
        std::lock_guard<std::mutex> lk(link.waitMutex);
    }
    link.waitCv.notify_all();
}

bool SdrPlaySource::noteIfServiceDead(abi::ErrT err, const char* what) {
    if (err != abi::ServiceNotResponding) { return false; }
    // THE SERVICE HAS DECLARED ITSELF GONE, so nothing of ours enters the
    // vendor DLL for this device again - including the teardown's Uninit,
    // ReleaseDevice and Close. See vendorUnreachableLocked(). Called from
    // updateLocked for its own Update, and from stopStreamingLocked for its
    // own Uninit (0.97.1) - every caller holds devMutex_, which every
    // *Locked helper requires.
    serviceGone_ = true;
    // ...and so does nothing else in the process: this device's session is
    // about to be orphaned, and it is the process's one session.
    markSessionLost(api(), "the service answered sdrplay_api_ServiceNotResponding");
    // The same sentence the enumeration skip uses, because it is the same
    // problem and the same remedy: the service, not the radio, is what has to
    // be restarted. Said once here rather than left to the caller's generic
    // "<what> failed: ServiceNotResponding (14)", which reads as a refused
    // request rather than as a receiver that is gone. "THEN RESTART FoxSDR"
    // since 0.99.36: markSessionLost above means opening the radio again is
    // refused until then, which the old "then open the radio again" sent the
    // user straight into.
    noteFaultOn(*link_, what,
                "the SDRplay service stopped answering - restart the SDRplay API service, "
                "then restart FoxSDR");
    core::diagWarnf("source: SDRplay %s - the service stopped answering; the radio is released",
                    what);
    return true;
}

void SdrPlaySource::setError(std::string msg) { setErrorOn(*link_, std::move(msg)); }

void SdrPlaySource::clearError() {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    link_->lastError.clear();
    link_->faulted = false;
    link_->deviceDead = false;
    link_->deadWhat.clear();
}

const char* SdrPlaySource::lastError() const {
    // Returned as a pointer into the Link's own string, which outlives this
    // object; the lock makes the read safe against the service's thread.
    static thread_local std::string copy;
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    copy = link_->lastError;
    return copy.c_str();
}

bool SdrPlaySource::faulted() const {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    return link_->faulted;
}

bool SdrPlaySource::deviceDead() const {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    return link_->deviceDead;
}

std::string SdrPlaySource::faultedWhile() const {
    std::lock_guard<std::mutex> lk(link_->errorMutex);
    return link_->deadWhat;
}

void SdrPlaySource::setName(std::string n) {
    std::lock_guard<std::mutex> lk(nameMutex_);
    name_ = std::move(n);
}

const char* SdrPlaySource::name() const {
    static thread_local std::string copy;
    std::lock_guard<std::mutex> lk(nameMutex_);
    copy = name_;
    return copy.c_str();
}

std::string SdrPlaySource::serialNo() const {
    std::lock_guard<std::mutex> lk(nameMutex_);
    return serial_;
}

// --- the callbacks --------------------------------------------------------

namespace {

std::int64_t steadyNowNs() {
    return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
}

}  // namespace

void SdrPlaySource::noteBlock(Link& link, std::size_t samples, bool dropped) {
    // THE SERVICE'S THREAD: atomics and nothing else. See Link::cbReads for
    // why this is not the StreamHealth-under-a-mutex it used to be.
    const std::int64_t now = steadyNowNs();
    std::int64_t closed = 0;
    if (link.cbWindowStartNs.compare_exchange_strong(closed, now, std::memory_order_acq_rel)) {
        // This block opens the window, so no gap is measured into it.
        link.cbLastSamplesNs.store(now, std::memory_order_relaxed);
    }
    link.cbReads.fetch_add(1, std::memory_order_relaxed);
    if (samples > 0) {
        link.cbWithSamples.fetch_add(1, std::memory_order_relaxed);
        link.cbSamples.fetch_add(samples, std::memory_order_relaxed);
        const std::int64_t prev = link.cbLastSamplesNs.exchange(now, std::memory_order_relaxed);
        if (prev != 0 && now > prev) {
            const std::int64_t gapMs = (now - prev) / 1000000;
            // The only writer apart from the reader's reset to zero, so a
            // compare-exchange loop is only ever retried against that reset.
            std::int64_t seen = link.cbLongestGapMs.load(std::memory_order_relaxed);
            while (gapMs > seen && !link.cbLongestGapMs.compare_exchange_weak(
                                       seen, gapMs, std::memory_order_relaxed)) {
            }
        }
    }
    if (dropped) { link.cbOverflows.fetch_add(1, std::memory_order_relaxed); }
}

std::string SdrPlaySource::healthLineLocked(Link& link) {
    // CLOSED FIRST, then emptied: a block the callback lands in between opens
    // the next window and is counted in one of the two, never lost and never
    // counted twice.
    const std::int64_t start = link.cbWindowStartNs.exchange(0, std::memory_order_acq_rel);
    const std::int64_t last = link.cbLastSamplesNs.exchange(0, std::memory_order_relaxed);
    const std::uint64_t reads = link.cbReads.exchange(0, std::memory_order_relaxed);
    const std::uint64_t withSamples = link.cbWithSamples.exchange(0, std::memory_order_relaxed);
    const std::uint64_t samples = link.cbSamples.exchange(0, std::memory_order_relaxed);
    const std::uint64_t overflows = link.cbOverflows.exchange(0, std::memory_order_relaxed);
    std::int64_t longestGapMs = link.cbLongestGapMs.exchange(0, std::memory_order_relaxed);
    StreamHealth& h = link.health;
    const StreamHealth mine = h;
    h = StreamHealth{};
    if (start == 0 || reads == 0) { return std::string(); }

    const std::int64_t now = steadyNowNs();
    if (last != 0 && now > last) {
        const std::int64_t openGap = (now - last) / 1000000;
        if (openGap > longestGapMs) { longestGapMs = openGap; }
    }
    const std::int64_t windowMs = (now > start) ? (now - start) / 1000000 : 0;
    char buf[192];
    // THE SAME FORMAT SoapySource::streamHealthLine WRITES, word for word, so
    // a reader of the diagnostic log does not have to know which driver was
    // open to parse the line.
    std::snprintf(buf, sizeof(buf),
                  "source: stream health - reads %llu, with samples %llu, timeouts %llu, "
                  "overflows %llu, errors %llu, longest gap %lld ms, %llu samples in %lld s",
                  static_cast<unsigned long long>(reads),
                  static_cast<unsigned long long>(withSamples),
                  static_cast<unsigned long long>(mine.timeouts),
                  static_cast<unsigned long long>(overflows),
                  static_cast<unsigned long long>(mine.errors),
                  static_cast<long long>(longestGapMs),
                  static_cast<unsigned long long>(samples),
                  static_cast<long long>((windowMs + 500) / 1000));
    return std::string(buf);
}

void SdrPlaySource::maybeWriteHealth(Link& link) {
    // THE PIPELINE'S SOURCE THREAD, from read(). Writing the log here can wait
    // behind a slow disk or another thread's line, and that is fine: this
    // thread's lateness is absorbed by the ring (a quarter of a second at the
    // fastest rate), where the SERVICE's lateness was a wedged service.
    const std::int64_t start = link.cbWindowStartNs.load(std::memory_order_acquire);
    if (start == 0) { return; }
    std::string line;
    bool warn = false;
    {
        std::lock_guard<std::mutex> lk(link.healthMutex);
        const std::int64_t windowNs =
            std::chrono::duration_cast<std::chrono::nanoseconds>(link.healthWindow).count();
        if (steadyNowNs() - start < windowNs) { return; }
        warn = link.cbOverflows.load(std::memory_order_relaxed) > 0 || link.health.errors > 0;
        const bool first = !link.healthEverWritten;
        line = healthLineLocked(link);
        if (line.empty()) { return; }
        // A line ALWAYS for the first window after a start, so a healthy radio
        // leaves one proving it; after that only for a window with something
        // to report.
        if (!(warn || first)) { return; }
        link.healthEverWritten = true;
    }
    if (warn) {
        core::diagWarnf("%s", line.c_str());
    } else {
        core::diagLogf("%s", line.c_str());
    }
}

void SdrPlaySource::drainEventLogs(Link& link) {
    const unsigned int bits = link.pendingEventLogs.exchange(0, std::memory_order_acq_rel);
    if (bits == 0) { return; }
    if ((bits & kEventLogOverload) != 0) {
        core::diagWarnf("source: SDRplay ADC OVERLOAD - reduce the gain or add attenuation");
    }
    if ((bits & kEventLogOverloadCorrected) != 0) {
        core::diagWarnf("source: SDRplay ADC overload corrected");
    }
    if ((bits & kEventLogRemoved) != 0) {
        core::diagWarnf("source: SDRplay device removed - stopping");
    }
    if ((bits & kEventLogFailure) != 0) {
        core::diagWarnf("source: SDRplay device failure - stopping");
    }
    if ((bits & kEventLogMasterLost) != 0) {
        core::diagWarnf("source: SDRplay RSPduo master disappeared - stopping");
    }
}

void SdrPlaySource::streamCallbackA(short* xi, short* xq, abi::StreamCbParamsT* params,
                                    unsigned int numSamples, unsigned int reset, void* ctx) {
    (void) reset;
    Link* link = static_cast<Link*>(ctx);
    if (link == nullptr) { return; }
    link->inCallback.fetch_add(1, std::memory_order_acq_rel);
    // ANY callback proves the service is still calling us - an acknowledgement
    // with no samples included. One atomic store; see kStreamStallLimit.
    link->lastCallbackNs.store(steadyNowNs(), std::memory_order_relaxed);

    if (params != nullptr) {
        // The service's acknowledgement that a queued Update has taken effect.
        // Latched rather than assigned so a waiting setter cannot miss one
        // that landed in a block it did not look at.
        if (params->grChanged != 0) { link->grChanged.store(1, std::memory_order_relaxed); }
        if (params->rfChanged != 0) { link->rfChanged.store(1, std::memory_order_relaxed); }
        if (params->fsChanged != 0) { link->fsChanged.store(1, std::memory_order_relaxed); }
    }

    if (!link->accepting.load(std::memory_order_relaxed) || xi == nullptr || xq == nullptr ||
        numSamples == 0) {
        link->inCallback.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }

    // Converted HERE, on the service's thread, exactly as every other driver
    // converts on its reader thread: the ring carries finished samples so the
    // pipeline's source thread does nothing but copy.
    //
    // /32768 rather than /32767 - the reference's own scaling
    // (SoapySDRPlay3 Streaming.cpp) and the one that puts the int16 range in
    // [-1, 1) with no value able to reach exactly 1.0.
    constexpr float kScale = 1.0f / 32768.0f;
    // Eight kilobytes on the SERVICE's stack, whose size is not ours to
    // choose; a block bigger than the API ever delivers at once would be a
    // gamble taken on somebody else's thread.
    std::complex<float> block[1024];
    std::size_t written = 0;
    bool dropped = false;
    unsigned int i = 0;
    while (i < numSamples) {
        const std::size_t chunk =
            std::min<std::size_t>(sizeof(block) / sizeof(block[0]), numSamples - i);
        for (std::size_t k = 0; k < chunk; ++k) {
            block[k] = std::complex<float>(static_cast<float>(xi[i + k]) * kScale,
                                           static_cast<float>(xq[i + k]) * kScale);
        }
        const std::size_t took = link->ring.write(block, chunk);
        written += took;
        if (took < chunk) {
            dropped = true;
            link->dropped.fetch_add(chunk - took, std::memory_order_relaxed);
            break;
        }
        i += static_cast<unsigned int>(chunk);
    }

    // THIS THREAD IS THE SERVICE'S, AND IT WAITS ON NOTHING OF OURS (0.99.32).
    // No lock another thread can hold, no log line, no file - the health
    // window is atomics (noteBlock) and read() writes its line. Until 0.99.32
    // the block that closed a window wrote the line from here, through the
    // diagnostic log's lock and an fflush, and two field reports (0.97.0
    // RSPdx, 0.99.27 RSP2) each show exactly ten more blocks after that write
    // and then a service that never delivered again. See
    // tests/test_sdrplay_stall.cpp.
    noteBlock(*link, written, dropped);

    if (written > 0) {
        // Notified WITHOUT waitMutex: see Link::waitCv. Taking it here was the
        // other lock this thread could be made to wait on.
        link->waitCv.notify_one();
    }
    link->inCallback.fetch_sub(1, std::memory_order_acq_rel);
}

void SdrPlaySource::streamCallbackB(short* xi, short* xq, abi::StreamCbParamsT* params,
                                    unsigned int numSamples, unsigned int reset, void* ctx) {
    // Channel B only exists in the RSPduo's dual-tuner mode, which this stage
    // does not select. Accepting its samples into the same ring would
    // interleave two receivers' signal into one stream, so it is dropped on
    // purpose rather than by omission.
    (void) xi;
    (void) xq;
    (void) params;
    (void) numSamples;
    (void) reset;
    (void) ctx;
}

void SdrPlaySource::eventCallback(abi::EventT eventId, abi::TunerSelectT tuner,
                                  abi::EventParamsT* params, void* ctx) {
    Link* linkPtr = static_cast<Link*>(ctx);
    if (linkPtr == nullptr) { return; }
    Link& link = *linkPtr;
    link.inCallback.fetch_add(1, std::memory_order_acq_rel);

    switch (eventId) {
        case abi::GainChange:
            if (params != nullptr) {
                // currGain is the service's own CALIBRATED figure, which with
                // the AGC running is the only honest answer to "what gain is
                // the radio at".
                link.currGainDb.store(params->gainParams.currGain, std::memory_order_relaxed);
                link.haveGainDb.store(true, std::memory_order_relaxed);
            }
            break;

        case abi::PowerOverloadChange: {
            const bool detected =
                params != nullptr &&
                params->powerOverloadParams.powerOverloadChangeType == abi::Overload_Detected;
            if (detected) { link.overloads.fetch_add(1, std::memory_order_relaxed); }
            // Logged by the next read(), not here: the same rule as the stream
            // callback (0.99.32) - this thread is the service's, and the log's
            // lock and file are not ours to make it wait on.
            link.pendingEventLogs.fetch_or(detected ? kEventLogOverload : kEventLogOverloadCorrected,
                                           std::memory_order_acq_rel);
            // THE ACKNOWLEDGEMENT IS NOT OPTIONAL. The service keeps
            // re-reporting an overload until it is acknowledged, so an
            // application that only logs the event gets a log full of it and
            // a service that never moves on. Every other argument comes out of
            // the Link, so this is safe even on a stranded one.
            //
            // ADDRESSED TO THE TUNER THE SERVICE NAMED, which is the tuner the
            // overload is about. link.tuner is the one active at Init, and an
            // RSPduo's tuner can be swapped on a live stream
            // (SwapRspDuoActiveTuner) without the Link being touched - through
            // 0.99.34 an overload on Tuner 2 after such a swap was acknowledged
            // for Tuner 1, so the real one was never cleared. link.tuner is
            // only the fallback for a service that names neither tuner.
            const abi::TunerSelectT ackTuner =
                (tuner == abi::Tuner_A || tuner == abi::Tuner_B) ? tuner : link.tuner;
            if (link.api != nullptr && link.api->Update != nullptr && link.dev != nullptr) {
                link.api->Update(link.dev, ackTuner, abi::Update_Ctrl_OverloadMsgAck,
                                 abi::Update_Ext1_None);
            }
            break;
        }

        case abi::DeviceRemoved:
            noteFaultOn(link, "device removed", "the RSP was unplugged or the service released it");
            link.pendingEventLogs.fetch_or(kEventLogRemoved, std::memory_order_acq_rel);
            break;

        case abi::DeviceFailure:
            noteFaultOn(link, "device failure", "the SDRplay service reported a device failure");
            link.pendingEventLogs.fetch_or(kEventLogFailure, std::memory_order_acq_rel);
            break;

        case abi::RspDuoModeChange:
            if (params != nullptr &&
                params->rspDuoModeParams.modeChangeType == abi::MasterDllDisappeared) {
                noteFaultOn(link, "master stream lost",
                            "the RSPduo master application closed");
                link.pendingEventLogs.fetch_or(kEventLogMasterLost, std::memory_order_acq_rel);
            }
            break;

        default: break;
    }

    link.inCallback.fetch_sub(1, std::memory_order_acq_rel);
}

// --- open / close ---------------------------------------------------------

abi::RxChannelParamsT* SdrPlaySource::chParamsLocked() const {
    if (deviceParams_ == nullptr) { return nullptr; }
    return (device_.tuner == abi::Tuner_B) ? deviceParams_->rxChannelB : deviceParams_->rxChannelA;
}

bool SdrPlaySource::acquireSessionLocked(std::string& error) {
    if (sessionHeld_) { return true; }
    if (!sessionAcquire(api(), error)) { return false; }
    sessionHeld_ = true;
    float ver = 0.0f;
    {
        std::lock_guard<std::mutex> lk(api().sessionMutex);
        ver = api().version;
    }
    apiVersion_.store(ver, std::memory_order_relaxed);
    return true;
}

void SdrPlaySource::releaseSessionLocked() {
    if (!sessionHeld_) { return; }
    sessionRelease(api());
    sessionHeld_ = false;
    apiVersion_.store(0.0f, std::memory_order_relaxed);
}

bool SdrPlaySource::selectByArgsLocked(const std::string& args) {
    const abi::Api& a = api();
    abi::DeviceT devs[abi::kMaxDevices];
    std::memset(devs, 0, sizeof(devs));
    unsigned int n = 0;

    const abi::ErrT lerr = a.LockDeviceApi();
    if (lerr != abi::Success) {
        setError("SDRplay LockDeviceApi failed: " + errText(a, lerr));
        return false;
    }
    const abi::ErrT gerr = a.GetDevices(devs, &n, abi::kMaxDevices);
    if (gerr != abi::Success) {
        a.UnlockDeviceApi();
        setError("SDRplay GetDevices failed: " + errText(a, gerr));
        return false;
    }
    if (n > abi::kMaxDevices) { n = abi::kMaxDevices; }

    const bool trustValid =
        abi::versionAtLeast(apiVersion_.load(std::memory_order_relaxed), abi::kValidFieldSinceVersion);

    const std::string wantSerial = lowerCopy(argValue(args, "serial"));
    const std::string wantIndex = argValue(args, "index");
    const std::string wantTuner = argValue(args, "tuner");

    int chosen = -1;
    int seen = 0;
    for (unsigned int i = 0; i < n; ++i) {
        if (trustValid && devs[i].valid == 0) { continue; }
        const std::size_t maxLen = sizeof(devs[i].SerNo);
        std::size_t len = 0;
        while (len < maxLen && devs[i].SerNo[len] != '\0') { ++len; }
        const std::string serial = lowerCopy(std::string(devs[i].SerNo, len));
        if (!wantSerial.empty()) {
            // A SUFFIX match, not an equality: the short form a user reads off
            // another tool's listing still has to find the radio.
            const bool hit = serial.size() >= wantSerial.size() &&
                             serial.compare(serial.size() - wantSerial.size(), wantSerial.size(),
                                            wantSerial) == 0;
            if (!hit) { continue; }
            chosen = static_cast<int>(i);
            break;
        }
        if (!wantIndex.empty()) {
            if (std::to_string(seen) == wantIndex) {
                chosen = static_cast<int>(i);
                break;
            }
            ++seen;
            continue;
        }
        chosen = static_cast<int>(i);
        break;
    }

    if (chosen < 0) {
        a.UnlockDeviceApi();
        setError(n == 0 ? "no SDRplay device is connected"
                        : ("no SDRplay device matches '" + args + "'"));
        return false;
    }

    device_ = devs[chosen];

    if (device_.hwVer == abi::kRspDuo) {
        // SINGLE-TUNER IS WHAT THIS STAGE SELECTS, and it is chosen HERE
        // because SelectDevice is where the mode and the tuner are fixed -
        // there is no later call that can change the mode.
        if ((device_.rspDuoMode & abi::RspDuoMode_Single_Tuner) == 0) {
            a.UnlockDeviceApi();
            setError("this RSPduo is already in use by another application");
            return false;
        }
        device_.rspDuoMode = abi::RspDuoMode_Single_Tuner;
        device_.tuner = (wantTuner == "2") ? abi::Tuner_B : abi::Tuner_A;
        device_.rspDuoSampleFreq = 0.0;
    } else {
        device_.rspDuoMode = abi::RspDuoMode_Unknown;
        device_.tuner = abi::Tuner_Neither;
    }

    const abi::ErrT serr = a.SelectDevice(&device_);
    a.UnlockDeviceApi();
    if (serr != abi::Success) {
        setError("SDRplay SelectDevice failed: " + errText(a, serr));
        return false;
    }
    selected_ = true;
    return true;
}

bool SdrPlaySource::getParamsLocked() {
    const abi::Api& a = api();
    if (a.DebugEnable != nullptr) {
        // Off, always. The API's own tracing is per-call and costs more than
        // it tells us; our diagnostics are in this file.
        a.DebugEnable(device_.dev, abi::DbgLvl_Disable);
    }
    deviceParams_ = nullptr;
    const abi::ErrT err = a.GetDeviceParams(device_.dev, &deviceParams_);
    if (err != abi::Success || deviceParams_ == nullptr) {
        setError("SDRplay GetDeviceParams failed: " + errText(a, err));
        return false;
    }
    if (chParamsLocked() == nullptr) {
        setError("the SDRplay API returned no channel parameters for the selected tuner");
        return false;
    }
    return true;
}

void SdrPlaySource::applyKnownStateLocked() {
    abi::RxChannelParamsT* ch = chParamsLocked();
    if (ch == nullptr) { return; }

    SdrPlayRatePlan plan;
    sdrPlayRatePlan(2000000.0, plan);

    if (deviceParams_->devParams != nullptr) {
        deviceParams_->devParams->fsFreq.fsHz = plan.fsHz;
        deviceParams_->devParams->ppm = 0.0;
    }
    ch->tunerParams.ifType = plan.ifType;
    ch->tunerParams.bwType = plan.bwType;
    ch->tunerParams.loMode = abi::LO_Auto;
    ch->tunerParams.rfFreq.rfHz = 100000000.0;
    ch->tunerParams.gain.gRdB = 40;
    ch->tunerParams.gain.LNAstate = 0;
    ch->tunerParams.gain.minGr = abi::NORMAL_MIN_GR;
    ch->ctrlParams.decimation.enable = static_cast<unsigned char>(plan.decEnable);
    ch->ctrlParams.decimation.decimationFactor = static_cast<unsigned char>(plan.decM);
    ch->ctrlParams.decimation.wideBandSignal = static_cast<unsigned char>(plan.wideBandSignal);
    ch->ctrlParams.agc.enable = abi::AGC_DISABLE;
    ch->ctrlParams.agc.setPoint_dBfs = -60;
    // Both corrections ON: a zero-IF front end without DC and IQ correction
    // puts a carrier in the middle of the display and its own image beside
    // every signal. The API does them in the service; there is nothing to gain
    // by doing them again here.
    ch->ctrlParams.dcOffset.DCenable = 1;
    ch->ctrlParams.dcOffset.IQenable = 1;
    // The tuner's own DC tracking, at the settings the reference uses while
    // streaming (SoapySDRPlay3 activateStream).
    ch->tunerParams.dcOffsetTuner.dcCal = 4;
    ch->tunerParams.dcOffsetTuner.speedUp = 0;
    ch->tunerParams.dcOffsetTuner.trackTime = 63;

    ch->rsp1aTunerParams.biasTEnable = 0;
    ch->rsp2TunerParams.biasTEnable = 0;
    ch->rsp2TunerParams.amPortSel = abi::Rsp2_AMPORT_2;
    ch->rsp2TunerParams.antennaSel = abi::Rsp2_ANTENNA_A;
    ch->rsp2TunerParams.rfNotchEnable = 0;
    ch->rspDuoTunerParams.biasTEnable = 0;
    ch->rspDuoTunerParams.tuner1AmPortSel = abi::RspDuo_AMPORT_2;
    ch->rspDuoTunerParams.tuner1AmNotchEnable = 0;
    ch->rspDuoTunerParams.rfNotchEnable = 0;
    ch->rspDuoTunerParams.rfDabNotchEnable = 0;

    if (deviceParams_->devParams != nullptr) {
        deviceParams_->devParams->rsp1aParams.rfNotchEnable = 0;
        deviceParams_->devParams->rsp1aParams.rfDabNotchEnable = 0;
        deviceParams_->devParams->rspDxParams.hdrEnable = 0;
        deviceParams_->devParams->rspDxParams.biasTEnable = 0;
        deviceParams_->devParams->rspDxParams.antennaSel = abi::RspDx_ANTENNA_A;
        deviceParams_->devParams->rspDxParams.rfNotchEnable = 0;
        deviceParams_->devParams->rspDxParams.rfDabNotchEnable = 0;
    }

    sampleRateHz_.store(2000000.0, std::memory_order_relaxed);
    centerFrequencyHz_.store(100000000.0, std::memory_order_relaxed);
    ifReductionDb_.store(40, std::memory_order_relaxed);
    lnaState_.store(0, std::memory_order_relaxed);
    autoGain_.store(false, std::memory_order_relaxed);
    agcSetPoint_.store(-60, std::memory_order_relaxed);
    biasT_.store(false, std::memory_order_relaxed);
    rfNotch_.store(false, std::memory_order_relaxed);
    dabNotch_.store(false, std::memory_order_relaxed);
    hdrMode_.store(false, std::memory_order_relaxed);
}

bool SdrPlaySource::open(const std::string& args) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (openMirror_.load(std::memory_order_relaxed)) {
        setError("this SDRplay source is already open");
        return false;
    }
    clearError();
    // A fresh session is a fresh device: the same judgement clearError() makes
    // about deviceDead, applied to the control seam. Whatever we abandoned
    // belongs to the device that was open before this one.
    controlAbandoned_ = false;
    serviceGone_ = false;
    streamStalled_.store(false, std::memory_order_release);

    std::string error;
    if (!acquireSessionLocked(error)) {
        setError(error);
        core::diagWarnf("source: SDRplay open failed - %s", error.c_str());
        return false;
    }

    if (!selectByArgsLocked(args)) {
        releaseSessionLocked();
        core::diagWarnf("source: SDRplay open failed - %s", lastError());
        return false;
    }
    if (!getParamsLocked()) {
        api().ReleaseDevice(&device_);
        selected_ = false;
        releaseSessionLocked();
        core::diagWarnf("source: SDRplay open failed - %s", lastError());
        return false;
    }

    applyKnownStateLocked();

    hwVer_.store(device_.hwVer, std::memory_order_relaxed);
    {
        const std::size_t maxLen = sizeof(device_.SerNo);
        std::size_t len = 0;
        while (len < maxLen && device_.SerNo[len] != '\0') { ++len; }
        std::lock_guard<std::mutex> nlk(nameMutex_);
        serial_ = std::string(device_.SerNo, len);
        name_ = "SDRplay " + sdrPlayModelName(device_.hwVer);
        antenna_ = sdrPlayAntennas(device_.hwVer, device_.rspDuoMode).front();
        if (device_.hwVer == abi::kRspDuo) {
            antenna_ = (device_.tuner == abi::Tuner_B) ? "Tuner 2" : "Tuner 1";
        }
    }

    openMirror_.store(true, std::memory_order_relaxed);
    core::diagLogf("source: SDRplay opened %s, API %.2f, tuner %d",
                   sdrPlayModelName(device_.hwVer).c_str(),
                   static_cast<double>(apiVersion_.load(std::memory_order_relaxed)),
                   static_cast<int>(device_.tuner));
    return true;
}

void SdrPlaySource::closeDevice() {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (initialised_) { stopStreamingLocked(); }
    // THE SAME RULE AS THE TEARDOWN ABOVE, AND FOR THE SAME REASON:
    // sdrplay_api_ReleaseDevice and sdrplay_api_Close are two more calls into
    // a DLL that is holding a thread of ours, and the 0.96.4 stack is what one
    // such call looks like from the outside.
    const bool unreachable = vendorUnreachableLocked();
    if (selected_) {
        const abi::Api& a = api();
        if (unreachable) {
            core::diagWarnf(
                "source: SDRplay closed without ReleaseDevice - the radio stays selected in the "
                "service until it is restarted");
        } else if (a.ReleaseDevice != nullptr) {
            const abi::ErrT err = a.ReleaseDevice(&device_);
            if (err != abi::Success) {
                core::diagWarnf("source: SDRplay ReleaseDevice failed - %s",
                                errText(a, err).c_str());
            }
        }
        selected_ = false;
    }
    deviceParams_ = nullptr;
    device_ = abi::DeviceT{};
    if (unreachable) {
        // THE SESSION IS ORPHANED, NOT RELEASED. Letting go of the last
        // reference is what calls sdrplay_api_Close, which is the third
        // unbounded call into the wedged DLL; the reference is therefore
        // dropped from THIS object and left standing in the process's table.
        // The cost is that the process never closes its connection to the
        // service - which is exactly what the abandoned worker means anyway,
        // and the reason the log above says this process cannot use the radio
        // again. The TABLE is told so as well (normally already, by the
        // abandonment or the refusal that set `unreachable`): an orphaned
        // reference means the count never reaches zero, and without the latch
        // the next scan or open() would be handed this same dead session.
        markSessionLost(api(), "the radio was closed with its session orphaned");
        sessionHeld_ = false;
        apiVersion_.store(0.0f, std::memory_order_relaxed);
    } else {
        releaseSessionLocked();
    }
    openMirror_.store(false, std::memory_order_relaxed);
    hwVer_.store(0, std::memory_order_relaxed);
}

// --- streaming ------------------------------------------------------------

bool SdrPlaySource::startStreamingLocked() {
    const abi::Api& a = api();
    link_->accepting.store(true, std::memory_order_relaxed);
    link_->grChanged.store(0, std::memory_order_relaxed);
    link_->rfChanged.store(0, std::memory_order_relaxed);
    link_->fsChanged.store(0, std::memory_order_relaxed);

    // What the event callback will need to acknowledge an overload, copied in
    // BEFORE Init so nothing writes it while the service is calling.
    link_->api = &a;
    link_->dev = device_.dev;
    link_->tuner = device_.tuner;
    // The stall clock is cleared here and STARTED when Init returns (below):
    // Init is unbounded and nothing can be delivered before it returns, so
    // time spent inside it is not silence (the 0c59853 review).
    link_->lastCallbackNs.store(0, std::memory_order_relaxed);
    link_->streamStartNs.store(0, std::memory_order_relaxed);
    link_->emptySinceNs.store(0, std::memory_order_relaxed);
    link_->lastEmptyReadNs.store(0, std::memory_order_relaxed);

    abi::CallbackFnsT cbs{};
    cbs.StreamACbFn = &SdrPlaySource::streamCallbackA;
    cbs.StreamBCbFn = &SdrPlaySource::streamCallbackB;
    cbs.EventCbFn = &SdrPlaySource::eventCallback;

    // The one cbContext the API takes is the LINK - see the header. `cbs`
    // itself is a stack local because the API copies the table; the reference
    // does the same.
    const abi::ErrT err = a.Init(device_.dev, &cbs, link_.get());
    if (err != abi::Success) {
        link_->accepting.store(false, std::memory_order_relaxed);
        link_->dev = nullptr;
        setError("SDRplay Init failed: " + errText(a, err));
        core::diagWarnf("source: SDRplay start failed - %s", lastError());
        return false;
    }
    // A service that never delivers a first block is caught from here.
    link_->streamStartNs.store(steadyNowNs(), std::memory_order_relaxed);
    initialised_ = true;
    running_.store(true, std::memory_order_relaxed);
    return true;
}

void SdrPlaySource::stopStreamingLocked() {
    // Whatever the event callback left unlogged goes in before the stop's own
    // lines, in the order it happened.
    drainEventLogs(*link_);
    if (!initialised_) {
        link_->accepting.store(false, std::memory_order_relaxed);
        running_.store(false, std::memory_order_relaxed);
        return;
    }
    const abi::Api& a = api();
    link_->accepting.store(false, std::memory_order_relaxed);

    // A TEARDOWN DOES NOT ENTER A VENDOR DLL WE HAVE ALREADY LOST A THREAD IN.
    //
    // THE 0.96.4 REPORT, IN ONE BRANCH. The abandonment of a control raises
    // the fault; the fault stops the pipeline; stopping the pipeline calls
    // stop() - on the GUI thread, in the same frame, straight into the Uninit
    // below, which the abandoned worker's device is already holding. The
    // captured stack is ntdll <- KERNELBASE <- sdrplay_api.dll <- this
    // function, and the user's window never came back.
    //
    // There is nothing to wait for and nothing to wait ON. Not the worker: it
    // is detached by definition and joining it is the freeze we are removing.
    // Not a callback drain either: we have not called Uninit, so the service
    // has not been told to stop calling us and no bound could make freeing the
    // Link safe. So the accounting is honest instead - accepting is cleared so
    // a callback that arrives drops its block, the Link is STRANDED, the
    // device handle stays in it for the thread that may still be inside, and
    // this returns. See the file header for what that costs.
    if (vendorUnreachableLocked()) {
        initialised_ = false;
        running_.store(false, std::memory_order_relaxed);
        core::diagWarnf(
            "source: SDRplay stopped without Uninit - %s, so the radio is left to the worker "
            "still inside the API; this process cannot use it again",
            controlAbandoned_ ? "a control was abandoned inside the vendor DLL"
            : streamStalled_.load(std::memory_order_acquire)
                ? "the stream stopped delivering samples"
                : "the service stopped answering");
        strandLink(link_);
        std::string line;
        {
            std::lock_guard<std::mutex> hl(link_->healthMutex);
            line = healthLineLocked(*link_);
        }
        if (!line.empty()) { core::diagLogf("%s", line.c_str()); }
        return;
    }

    // UNBOUNDED BY CONSTRUCTION. sdrplay_api_Uninit takes no timeout and
    // offers no cancellation; the API's contract is that it returns with the
    // callbacks stopped. There is no argument we can pass to shorten it and no
    // handle we can close to interrupt it.
    const abi::ErrT err = a.Uninit(device_.dev);
    if (err != abi::Success && err != abi::NotInitialised) {
        core::diagWarnf("source: SDRplay Uninit failed - %s", errText(a, err).c_str());
        // THE SAME RULE updateLocked ALREADY FOLLOWS FOR ITS OWN FAILURES
        // (0.96.1), applied to this call too (0.97.1's hang report).
        // sdrplay_api_ServiceNotResponding means the thing holding the USB
        // handle is gone NO MATTER WHICH CALL SAYS SO, and until this line
        // only updateLocked's own failures ever reached noteIfServiceDead -
        // stop()'s own Uninit answering it here left serviceGone_ false, so
        // vendorUnreachableLocked() stayed false and the very next thing that
        // touched this device (closeDevice()'s ReleaseDevice below, called
        // moments later when the user picked a different source) walked
        // straight into the vendor DLL with no guard at all. The report's log
        // is exactly that order: "SDRplay Uninit failed -
        // sdrplay_api_ServiceNotResponding (14)", then two refused/abandoned
        // calls, then "closing SDRplay RSPdx before opening another device" -
        // and the GUI thread never came back from that ReleaseDevice.
        noteIfServiceDead(err, "stop");

        // A REFUSED UNINIT HAS STOPPED NOTHING (the 0.97.1 crash report,
        // 2026-09-17). The drain below asks "is a callback inside us right
        // now", which is the right question only when Uninit SUCCEEDED and the
        // library has promised no more are coming. When it refused, the stream
        // is still running on the service's side - the same report's next line
        // is the library saying so itself, "Init failed:
        // sdrplay_api_AlreadyInitialised" - and it still holds our callback
        // and the address of this Link. With no callback inside at that
        // instant the Link was not stranded, so it died with this source when
        // the device was closed, and the next block the service delivered was
        // copied into a freed ring: an access violation in memcpy, on the
        // vendor's own thread, where nothing of ours can catch it.
        //
        // So a refusal is treated exactly as the unreachable branch above
        // treats it: accepting is cleared so a late block is dropped, the Link
        // is stranded for the life of the process, and it KEEPS its device
        // handle for whatever is still inside. A leaked ring is survivable; a
        // use-after-free on somebody else's thread is not.
        link_->accepting.store(false, std::memory_order_relaxed);
        strandLink(link_);
        initialised_ = false;
        running_.store(false, std::memory_order_relaxed);
        std::string refusedLine;
        {
            std::lock_guard<std::mutex> hl(link_->healthMutex);
            refusedLine = healthLineLocked(*link_);
        }
        if (!refusedLine.empty()) { core::diagLogf("%s", refusedLine.c_str()); }
        return;
    }
    initialised_ = false;
    running_.store(false, std::memory_order_relaxed);

    // The bounded half: if a callback is somehow still inside us, wait a
    // little, then strand the Link rather than free it under the service's
    // thread.
    const auto deadline = std::chrono::steady_clock::now() + kCallbackDrainWait;
    while (link_->inCallback.load(std::memory_order_acquire) > 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::unique_lock<std::mutex> wl(link_->waitMutex);
        link_->waitCv.wait_for(wl, std::chrono::milliseconds(1));
    }
    if (link_->inCallback.load(std::memory_order_acquire) > 0) {
        core::diagWarnf(
            "source: SDRplay callback still running %lld ms after Uninit - stranding the link",
            static_cast<long long>(kCallbackDrainWait.count()));
        strandLink(link_);
    } else {
        // Nothing is inside the callbacks any more, so the device handle can
        // be forgotten. A stranded Link deliberately KEEPS its copy: the
        // thread still inside it needs somewhere valid to finish.
        link_->dev = nullptr;
    }

    // The stream-health line for whatever the window holds, so a session that
    // was too short to trip the window still leaves its numbers.
    std::string line;
    {
        std::lock_guard<std::mutex> hl(link_->healthMutex);
        line = healthLineLocked(*link_);
    }
    if (!line.empty()) { core::diagLogf("%s", line.c_str()); }
}

bool SdrPlaySource::start() {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (!openMirror_.load(std::memory_order_relaxed)) {
        setError("no SDRplay device is open");
        return false;
    }
    if (initialised_) { return true; }
    // A START AFTER THE SERVICE STOPPED ANSWERING NEVER ENTERS Init (0.99.32).
    //
    // 0.99.28's latch refuses open() and the scan once the session is lost,
    // but a radio that is still OPEN is started with neither: the user's STOP
    // after a dead stream is stop()'s Uninit answering
    // sdrplay_api_ServiceNotResponding, and every START after it went
    // straight into sdrplay_api_Init on the same dead service - an unbounded
    // call like every other into that DLL - and was told
    // sdrplay_api_AlreadyInitialised. That is the tail of the 0.97.0 RSPdx log
    // and of the 0.99.27 RSP2 log, and the user read "Init failed:
    // AlreadyInitialised (9)" where the one thing that helps is restarting the
    // service. The radio stays dead (faulted, so the pipeline says so) and
    // the sentence it gives is the one that says what to do.
    if (vendorUnreachableLocked() || sessionIsLost(api())) {
        {
            std::lock_guard<std::mutex> ek(link_->errorMutex);
            link_->lastError = sdrPlaySessionLostSentence();
            link_->faulted = true;
            link_->deviceDead = true;
            if (link_->deadWhat.empty()) { link_->deadWhat = "start"; }
        }
        // Shorter than the sentence: a log line is cut at 191 bytes.
        core::diagWarnf("source: SDRplay start refused - the service stopped answering; restart "
                        "the SDRplay API service, then FoxSDR");
        return false;
    }
    return startStreamingLocked();
}

void SdrPlaySource::stop() {
    std::lock_guard<std::mutex> lk(devMutex_);
    stopStreamingLocked();
}

std::size_t SdrPlaySource::read(std::complex<float>* dst, std::size_t n) {
    if (dst == nullptr || n == 0) { return 0; }
    // What the service's thread may not do itself (0.99.32): the log lines
    // its callbacks left, and closing a health window that has run out -
    // here, on the pipeline's source thread, whose lateness the ring absorbs.
    drainEventLogs(*link_);
    maybeWriteHealth(*link_);
    std::size_t got = link_->ring.read(dst, n);
    if (got > 0) {
        link_->emptySinceNs.store(0, std::memory_order_relaxed);  // the empty run is over
        return got;
    }
    if (faulted()) { return 0; }
    {
        // Bounded and short: the pipeline's self-paced loop treats a zero as
        // "nothing yet" and backs off a millisecond of its own.
        std::unique_lock<std::mutex> wl(link_->waitMutex);
        link_->waitCv.wait_for(wl, kReadWait);
    }
    got = link_->ring.read(dst, n);
    if (got == 0) {
        {
            std::lock_guard<std::mutex> hl(link_->healthMutex);
            if (link_->cbWindowStartNs.load(std::memory_order_acquire) != 0) {
                ++link_->health.timeouts;
            }
        }
        checkForStallFromRead();
    } else {
        link_->emptySinceNs.store(0, std::memory_order_relaxed);
    }
    return got;
}

// --- updates --------------------------------------------------------------

bool SdrPlaySource::updateLocked(abi::ReasonForUpdateT reason, abi::ReasonForUpdateExt1T ext1,
                                 const char* what) {
    if (reason == abi::Update_None && ext1 == abi::Update_Ext1_None) { return true; }
    if (!initialised_) {
        // Not streaming: the parameter block IS the device's state and the
        // service reads it at Init. Nothing to send and nothing to wait for.
        return true;
    }

    // A DEVICE WE HAVE ALREADY ABANDONED A THREAD INSIDE gets no further
    // calls, ever. See controlAbandoned_: the first one is parked in the
    // vendor DLL with no way to recall it, and a panel whose sliders keep
    // working would park one per click.
    if (controlAbandoned_) {
        setError(std::string(what) + " refused: " + sdrPlayControlHungSentence());
        return false;
    }
    // ...AND NEITHER DOES A STREAM THAT HAS BEEN DECLARED STALLED (0.99.36):
    // the service stopped calling us without a word, which is the same
    // service a control would now walk into for kControlWait and abandon a
    // worker inside.
    if (streamStalled_.load(std::memory_order_acquire)) {
        setError(std::string(what) + " refused: " + sdrPlayStreamStalledSentence());
        return false;
    }

    const abi::Api& a = api();

    const bool wantGr = (reason & abi::Update_Tuner_Gr) != 0;
    const bool wantRf = (reason & abi::Update_Tuner_Frf) != 0;
    const bool wantFs = (reason & abi::Update_Dev_Fs) != 0;
    if (wantGr) { link_->grChanged.store(0, std::memory_order_relaxed); }
    if (wantRf) { link_->rfChanged.store(0, std::memory_order_relaxed); }
    if (wantFs) { link_->fsChanged.store(0, std::memory_order_relaxed); }

    // THE VENDOR CALL ON A WORKER, and the caller's WAIT is what is bounded -
    // sdrplay_api_Update takes no timeout and cannot be cancelled, exactly
    // like the enumeration calls above. The 0.96.2 report is this call taking
    // five seconds to answer ServiceNotResponding on the GUI thread, which is
    // the hang watchdog's entire frame threshold. See kControlWait.
    //
    // The promise is a shared_ptr and the device handle and tuner are copied
    // in, and that is load-bearing rather than tidy: an abandoned worker
    // outlives this frame and this function, so everything it writes into
    // must be owned by the worker - INCLUDING the table, which is captured as
    // a POINTER BY VALUE rather than as this frame's reference to it: the
    // table itself is process-scope and never unloaded (and a test's fake
    // outlives the worker it abandons deliberately), but the local reference
    // naming it dies with this frame and an abandoned worker does not.
    auto result = std::make_shared<std::promise<abi::ErrT>>();
    std::future<abi::ErrT> done = result->get_future();
    const abi::Api* const table = &a;
    void* const dev = device_.dev;
    const abi::TunerSelectT tuner = device_.tuner;
    std::thread worker([table, result, dev, tuner, reason, ext1]() {
        result->set_value(table->Update(dev, tuner, reason, ext1));
    });

    if (done.wait_for(kControlWait) != std::future_status::ready) {
        // ABANDONED. Not joined, not killed, not signalled: the thread is
        // inside the vendor DLL and there is no handle we can close to bring
        // it back. It holds everything it needs, so it can finish - or not -
        // harmlessly.
        worker.detach();
        controlAbandoned_ = true;
        // A thread of ours is inside the vendor DLL for good, so the process's
        // session is finished too - not only this device's controls.
        markSessionLost(a, "a control was abandoned inside sdrplay_api_Update");
        // A DEAD RECEIVER, through the same path a returned
        // ServiceNotResponding takes, so Pipeline's source thread latches the
        // fault and stops rather than reading a service that is gone.
        noteFaultOn(*link_, what, sdrPlayControlHungSentence());
        core::diagWarnf(
            "source: SDRplay %s abandoned - the service did not answer within %lld ms; the radio "
            "is released",
            what, static_cast<long long>(kControlWait.count()));
        core::diagLogf(
            "source: SDRplay controls are refused for this device from here - a worker is still "
            "inside sdrplay_api_Update");
        return false;
    }

    worker.join();
    const abi::ErrT err = done.get();
    if (err != abi::Success) {
        setError(std::string(what) + " failed: " + errText(a, err));
        core::diagWarnf("source: SDRplay %s failed - %s", what, errText(a, err).c_str());
        // ONE REFUSAL IS A REFUSAL; ServiceNotResponding IS A DEAD RECEIVER.
        // Every live control the panel offers - retune, LNA state, IF gain,
        // AGC, rate - comes through here, so this is the one place that has to
        // tell the two apart. The 0.95.0 report is what happens when it does
        // not: four of those in a row answered 14 and the pipeline kept
        // reading a service that was gone, 1910 timeouts deep.
        noteIfServiceDead(err, what);
        return false;
    }

    if (!(wantGr || wantRf || wantFs)) { return true; }

    // The acknowledgement, bounded. The parameter is already programmed; this
    // is the service telling us it has taken effect, and a missing one is a
    // warning rather than a failure - see kUpdateWait.
    const auto deadline = std::chrono::steady_clock::now() + kUpdateWait;
    for (;;) {
        const bool done = (!wantGr || link_->grChanged.load(std::memory_order_relaxed) != 0) &&
                          (!wantRf || link_->rfChanged.load(std::memory_order_relaxed) != 0) &&
                          (!wantFs || link_->fsChanged.load(std::memory_order_relaxed) != 0);
        if (done) { return true; }
        if (std::chrono::steady_clock::now() >= deadline) { break; }
        if (faulted()) { return false; }
        std::unique_lock<std::mutex> wl(link_->waitMutex);
        link_->waitCv.wait_for(wl, std::chrono::milliseconds(1));
    }
    core::diagWarnf("source: SDRplay %s - no acknowledgement within %lld ms", what,
                    static_cast<long long>(kUpdateWait.count()));
    return true;
}

// --- frequency ------------------------------------------------------------

bool SdrPlaySource::frequencyRangeHz(double& loHz, double& hiHz) const {
    loHz = 1000.0;
    hiHz = 2000000000.0;
    return true;
}

bool SdrPlaySource::setCenterFrequencyHz(double hz) {
    std::lock_guard<std::mutex> lk(devMutex_);
    double lo = 0.0;
    double hi = 0.0;
    frequencyRangeHz(lo, hi);
    if (!(hz >= lo && hz <= hi)) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%.0f Hz is outside the RSP's %.0f Hz to %.0f Hz range", hz,
                      lo, hi);
        setError(buf);
        return false;
    }
    abi::RxChannelParamsT* ch = chParamsLocked();
    if (ch == nullptr) {
        setError("no SDRplay device is open");
        return false;
    }
    // NOTHING TO SEND WHEN NOTHING CHANGES (0.99.36). The radio is already
    // programmed with this frequency - by Init, if it was written before
    // start(), or by an earlier Update - so a live Update would only ask the
    // service to redo work. The one that mattered is the carry-across tune
    // AppWindow makes the instant a radio is installed: three field logs
    // (0.95.0, 0.96.2, 0.99.27) show the service never answering the first
    // Update sent that soon after Init. The reference makes the same check
    // (SoapySDRPlay3 setFrequency compares rfHz before updating).
    //
    // THAT CHECK IS ONLY AS TRUE AS THE BLOCK, so a refused retune puts the
    // old frequency back (BlockRollback): the 0c59853 review's probe was a
    // refused retune followed by the same request, answered true with nothing
    // sent and a readback the radio had never reached.
    if (ch->tunerParams.rfFreq.rfHz == hz) {
        centerFrequencyHz_.store(hz, std::memory_order_relaxed);
        return true;
    }
    BlockRollback rb;
    rb.save(ch->tunerParams.rfFreq.rfHz);
    ch->tunerParams.rfFreq.rfHz = hz;
    if (!updateLocked(abi::Update_Tuner_Frf, abi::Update_Ext1_None, "retune")) {
        undoRefused(rb, controlAbandoned_);
        return false;
    }
    centerFrequencyHz_.store(hz, std::memory_order_relaxed);
    return true;
}

// --- rate -----------------------------------------------------------------

std::vector<double> SdrPlaySource::supportedSampleRatesHz() const {
    return sdrPlaySupportedRatesHz();
}

bool SdrPlaySource::setSampleRateHz(double hz) {
    std::lock_guard<std::mutex> lk(devMutex_);
    abi::RxChannelParamsT* ch = chParamsLocked();
    if (ch == nullptr || deviceParams_ == nullptr) {
        setError("no SDRplay device is open");
        return false;
    }

    // Coerced to the nearest supported rate, as the Soapy path does, so a
    // config that asked for something plausible still opens.
    const std::vector<double> rates = sdrPlaySupportedRatesHz();
    double best = rates.front();
    double bestErr = std::numeric_limits<double>::max();
    for (double r : rates) {
        const double e = std::fabs(r - hz);
        if (e < bestErr) {
            bestErr = e;
            best = r;
        }
    }

    SdrPlayRatePlan plan;
    if (!sdrPlayRatePlan(best, plan)) {
        setError("no SDRplay front-end plan for that rate");
        return false;
    }

    abi::ReasonForUpdateT reason = abi::Update_None;
    BlockRollback rb;  // see the class: a refused Update puts every field back
    if (deviceParams_->devParams != nullptr &&
        deviceParams_->devParams->fsFreq.fsHz != plan.fsHz) {
        rb.save(deviceParams_->devParams->fsFreq.fsHz);
        deviceParams_->devParams->fsFreq.fsHz = plan.fsHz;
        reason = static_cast<abi::ReasonForUpdateT>(reason | abi::Update_Dev_Fs);
    }
    if (ch->tunerParams.ifType != plan.ifType) {
        rb.save(ch->tunerParams.ifType);
        ch->tunerParams.ifType = plan.ifType;
        reason = static_cast<abi::ReasonForUpdateT>(reason | abi::Update_Tuner_IfType);
    }
    if (ch->ctrlParams.decimation.decimationFactor != static_cast<unsigned char>(plan.decM) ||
        ch->ctrlParams.decimation.enable != static_cast<unsigned char>(plan.decEnable)) {
        rb.save(ch->ctrlParams.decimation.enable);
        rb.save(ch->ctrlParams.decimation.decimationFactor);
        rb.save(ch->ctrlParams.decimation.wideBandSignal);
        ch->ctrlParams.decimation.enable = static_cast<unsigned char>(plan.decEnable);
        ch->ctrlParams.decimation.decimationFactor = static_cast<unsigned char>(plan.decM);
        ch->ctrlParams.decimation.wideBandSignal = static_cast<unsigned char>(plan.wideBandSignal);
        reason = static_cast<abi::ReasonForUpdateT>(reason | abi::Update_Ctrl_Decimation);
    }
    if (ch->tunerParams.bwType != plan.bwType) {
        rb.save(ch->tunerParams.bwType);
        ch->tunerParams.bwType = plan.bwType;
        reason = static_cast<abi::ReasonForUpdateT>(reason | abi::Update_Tuner_BwType);
    }

    // ONE Update carrying every reason that changed. Three separate ones would
    // be three separate disturbances to a live stream for what the API is
    // perfectly happy to do at once.
    if (!updateLocked(reason, abi::Update_Ext1_None, "sample rate change")) {
        undoRefused(rb, controlAbandoned_);
        return false;
    }
    sampleRateHz_.store(best, std::memory_order_relaxed);
    return true;
}

// --- gains ----------------------------------------------------------------

std::vector<GainInfo> SdrPlaySource::gains() const {
    std::vector<GainInfo> g;
    // The sign is turned over here and nowhere else - see the header.
    g.push_back(GainInfo{"IF", -59.0, -20.0, 1.0, GainUnit::Decibels});
    const int states = sdrPlayLnaStateCount(hwVer_.load(std::memory_order_relaxed));
    g.push_back(GainInfo{"LNA", 0.0, static_cast<double>(states - 1), 1.0, GainUnit::Steps});
    return g;
}

double SdrPlaySource::gainDb(const std::string& name) const {
    if (name == "IF") {
        return -static_cast<double>(ifReductionDb_.load(std::memory_order_relaxed));
    }
    if (name == "LNA") { return static_cast<double>(lnaState_.load(std::memory_order_relaxed)); }
    return 0.0;
}

bool SdrPlaySource::setGainDb(const std::string& name, double db) {
    std::lock_guard<std::mutex> lk(devMutex_);
    abi::RxChannelParamsT* ch = chParamsLocked();
    if (ch == nullptr) {
        setError("no SDRplay device is open");
        return false;
    }

    if (name == "IF") {
        if (autoGain_.load(std::memory_order_relaxed)) {
            // The API refuses a gRdB write while its AGC owns the field, and
            // pretending otherwise would leave the panel showing a number the
            // radio is not at.
            setError("the IF gain is under the RSP's AGC - turn the AGC off to set it by hand");
            return false;
        }
        // CLAMPED, not refused: the DeviceSource contract, and gainDb reports
        // what was actually programmed.
        const int reduction = std::clamp(static_cast<int>(std::llround(-db)), 20, 59);
        if (ch->tunerParams.gain.gRdB != reduction) {
            BlockRollback rb;
            rb.save(ch->tunerParams.gain.gRdB);
            ch->tunerParams.gain.gRdB = reduction;
            if (!updateLocked(abi::Update_Tuner_Gr, abi::Update_Ext1_None, "IF gain change")) {
                undoRefused(rb, controlAbandoned_);
                return false;
            }
        }
        ifReductionDb_.store(reduction, std::memory_order_relaxed);
        return true;
    }

    if (name == "LNA") {
        const int states = sdrPlayLnaStateCount(hwVer_.load(std::memory_order_relaxed));
        const int state = std::clamp(static_cast<int>(std::llround(db)), 0, states - 1);
        if (ch->tunerParams.gain.LNAstate != static_cast<unsigned char>(state)) {
            BlockRollback rb;
            rb.save(ch->tunerParams.gain.LNAstate);
            ch->tunerParams.gain.LNAstate = static_cast<unsigned char>(state);
            if (!updateLocked(abi::Update_Tuner_Gr, abi::Update_Ext1_None, "LNA state change")) {
                undoRefused(rb, controlAbandoned_);
                return false;
            }
        }
        lnaState_.store(state, std::memory_order_relaxed);
        return true;
    }

    setError("the RSP has no gain called '" + name + "'");
    return false;
}

bool SdrPlaySource::setAutoGain(bool on) {
    std::lock_guard<std::mutex> lk(devMutex_);
    abi::RxChannelParamsT* ch = chParamsLocked();
    if (ch == nullptr) {
        setError("no SDRplay device is open");
        return false;
    }
    const abi::AgcControlT want = on ? abi::AGC_CTRL_EN : abi::AGC_DISABLE;
    if (ch->ctrlParams.agc.enable != want) {
        BlockRollback rb;
        rb.save(ch->ctrlParams.agc.enable);
        rb.save(ch->ctrlParams.agc.setPoint_dBfs);
        ch->ctrlParams.agc.enable = want;
        ch->ctrlParams.agc.setPoint_dBfs = agcSetPoint_.load(std::memory_order_relaxed);
        if (!updateLocked(abi::Update_Ctrl_Agc, abi::Update_Ext1_None, "AGC change")) {
            undoRefused(rb, controlAbandoned_);
            return false;
        }
    }
    autoGain_.store(on, std::memory_order_relaxed);
    if (!on) {
        // Coming off the AGC, the field holds whatever the loop last wrote.
        // Put our own number back so the panel and the radio agree - and if
        // the service refuses that, leave the field saying what the radio IS
        // at, so the next IF change is not taken for "nothing to send".
        BlockRollback rb;
        rb.save(ch->tunerParams.gain.gRdB);
        ch->tunerParams.gain.gRdB = ifReductionDb_.load(std::memory_order_relaxed);
        if (!updateLocked(abi::Update_Tuner_Gr, abi::Update_Ext1_None, "IF gain restore")) {
            undoRefused(rb, controlAbandoned_);
        }
    }
    return true;
}

bool SdrPlaySource::setAgcSetPointDbfs(int dbfs) {
    std::lock_guard<std::mutex> lk(devMutex_);
    abi::RxChannelParamsT* ch = chParamsLocked();
    if (ch == nullptr) {
        setError("no SDRplay device is open");
        return false;
    }
    const int clamped = std::clamp(dbfs, -72, 0);
    BlockRollback rb;
    rb.save(ch->ctrlParams.agc.setPoint_dBfs);
    ch->ctrlParams.agc.setPoint_dBfs = clamped;
    if (autoGain_.load(std::memory_order_relaxed) &&
        !updateLocked(abi::Update_Ctrl_Agc, abi::Update_Ext1_None, "AGC set point change")) {
        // The mirror is written only once the radio took it (0.99.36).
        undoRefused(rb, controlAbandoned_);
        return false;
    }
    agcSetPoint_.store(clamped, std::memory_order_relaxed);
    return true;
}

// --- antennas -------------------------------------------------------------

std::vector<std::string> SdrPlaySource::antennas() const {
    const unsigned char hw = hwVer_.load(std::memory_order_relaxed);
    abi::RspDuoModeT mode = abi::RspDuoMode_Unknown;
    {
        std::lock_guard<std::mutex> lk(devMutex_);
        mode = device_.rspDuoMode;
    }
    return sdrPlayAntennas(hw, mode);
}

std::string SdrPlaySource::antenna() const {
    std::lock_guard<std::mutex> lk(nameMutex_);
    return antenna_;
}

bool SdrPlaySource::reselectTunerLocked(abi::TunerSelectT tuner) {
    const abi::Api& a = api();
    // Everything we have programmed lives in memory the service owns and will
    // free at ReleaseDevice, so it has to be COPIED OUT before the release and
    // written back after the new select - the reference does exactly this
    // (SoapySDRPlay3 selectDevice's save/restore).
    abi::DevParamsT savedDev{};
    abi::RxChannelParamsT savedCh{};
    bool haveDev = false;
    if (deviceParams_ != nullptr) {
        if (deviceParams_->devParams != nullptr) {
            savedDev = *deviceParams_->devParams;
            haveDev = true;
        }
        abi::RxChannelParamsT* ch = chParamsLocked();
        if (ch != nullptr) { savedCh = *ch; }
    }

    const abi::ErrT rerr = a.ReleaseDevice(&device_);
    selected_ = false;
    deviceParams_ = nullptr;
    if (rerr != abi::Success) {
        setError("SDRplay ReleaseDevice failed: " + errText(a, rerr));
        return false;
    }

    device_.tuner = tuner;
    device_.rspDuoMode = abi::RspDuoMode_Single_Tuner;
    const abi::ErrT serr = a.SelectDevice(&device_);
    if (serr != abi::Success) {
        setError("SDRplay SelectDevice failed: " + errText(a, serr));
        return false;
    }
    selected_ = true;
    if (!getParamsLocked()) { return false; }

    if (haveDev && deviceParams_->devParams != nullptr) { *deviceParams_->devParams = savedDev; }
    abi::RxChannelParamsT* ch = chParamsLocked();
    if (ch != nullptr) { *ch = savedCh; }
    return true;
}

bool SdrPlaySource::setAntenna(const std::string& name) {
    std::lock_guard<std::mutex> lk(devMutex_);
    const unsigned char hw = hwVer_.load(std::memory_order_relaxed);
    const std::vector<std::string> list = sdrPlayAntennas(hw, device_.rspDuoMode);
    if (std::find(list.begin(), list.end(), name) == list.end()) {
        setError("this RSP has no antenna called '" + name + "'");
        return false;
    }
    abi::RxChannelParamsT* ch = chParamsLocked();
    if (ch == nullptr) {
        setError("no SDRplay device is open");
        return false;
    }

    if (hw == abi::kRspDx || hw == abi::kRspDxR2) {
        if (deviceParams_->devParams == nullptr) {
            setError("no SDRplay device parameters");
            return false;
        }
        // Each field put back if its own Update is refused (BlockRollback).
        BlockRollback rb;
        rb.save(deviceParams_->devParams->rspDxParams.antennaSel);
        deviceParams_->devParams->rspDxParams.antennaSel =
            (name == "Antenna B")   ? abi::RspDx_ANTENNA_B
            : (name == "Antenna C") ? abi::RspDx_ANTENNA_C
                                    : abi::RspDx_ANTENNA_A;
        // The RSPdx's controls are ALL in the extension-1 word, which is why
        // this is the one Update in the driver whose first reason is None.
        if (!updateLocked(abi::Update_None, abi::Update_RspDx_AntennaControl, "antenna change")) {
            undoRefused(rb, controlAbandoned_);
            return false;
        }
    } else if (hw == abi::kRsp2) {
        if (name == "Hi-Z") {
            BlockRollback rb;
            rb.save(ch->rsp2TunerParams.amPortSel);
            ch->rsp2TunerParams.amPortSel = abi::Rsp2_AMPORT_1;
            if (!updateLocked(abi::Update_Rsp2_AmPortSelect, abi::Update_Ext1_None,
                              "antenna change")) {
                undoRefused(rb, controlAbandoned_);
                return false;
            }
        } else {
            if (ch->rsp2TunerParams.amPortSel == abi::Rsp2_AMPORT_1) {
                // Come off Hi-Z FIRST: the antenna switch means nothing while
                // the AM port owns the input.
                BlockRollback rbPort;
                rbPort.save(ch->rsp2TunerParams.amPortSel);
                ch->rsp2TunerParams.amPortSel = abi::Rsp2_AMPORT_2;
                if (!updateLocked(abi::Update_Rsp2_AmPortSelect, abi::Update_Ext1_None,
                                  "antenna change")) {
                    undoRefused(rbPort, controlAbandoned_);
                    return false;
                }
            }
            // The port change above was TAKEN if we got here, so only the
            // antenna switch is put back on a refusal of this one.
            BlockRollback rb;
            rb.save(ch->rsp2TunerParams.antennaSel);
            ch->rsp2TunerParams.antennaSel =
                (name == "Antenna B") ? abi::Rsp2_ANTENNA_B : abi::Rsp2_ANTENNA_A;
            if (!updateLocked(abi::Update_Rsp2_AntennaControl, abi::Update_Ext1_None,
                              "antenna change")) {
                undoRefused(rb, controlAbandoned_);
                return false;
            }
        }
    } else if (hw == abi::kRspDuo) {
        const abi::TunerSelectT want = (name == "Tuner 2") ? abi::Tuner_B : abi::Tuner_A;
        if (want != device_.tuner) {
            if (initialised_) {
                // Live: the API has a call for exactly this and it is the only
                // way to move the tuner without dropping the stream.
                const abi::Api& a = api();
                if (a.SwapRspDuoActiveTuner == nullptr) {
                    setError("this SDRplay API has no SwapRspDuoActiveTuner");
                    return false;
                }
                const abi::ErrT err = a.SwapRspDuoActiveTuner(
                    device_.dev, &device_.tuner, ch->rspDuoTunerParams.tuner1AmPortSel);
                if (err != abi::Success) {
                    setError("SDRplay SwapRspDuoActiveTuner failed: " + errText(a, err));
                    return false;
                }
                device_.tuner = want;
            } else {
                // Stopped: the tuner is fixed at SelectDevice, so there is
                // nothing for it but a release and a re-select.
                if (!reselectTunerLocked(want)) { return false; }
            }
        }
    }
    // Everything else has one antenna and accepting its own name is the
    // DeviceSource contract.

    {
        std::lock_guard<std::mutex> nlk(nameMutex_);
        antenna_ = name;
    }
    return true;
}

// --- the switches ---------------------------------------------------------

bool SdrPlaySource::biasTeeSupported() const {
    switch (hwVer_.load(std::memory_order_relaxed)) {
        case abi::kRsp1A:
        case abi::kRsp1B:
        case abi::kRsp2:
        case abi::kRspDuo:
        case abi::kRspDx:
        case abi::kRspDxR2: return true;
        default: break;
    }
    // The original RSP1 has no bias tee at all.
    return false;
}

bool SdrPlaySource::setBiasT(bool on) {
    std::lock_guard<std::mutex> lk(devMutex_);
    abi::RxChannelParamsT* ch = chParamsLocked();
    if (ch == nullptr) {
        setError("no SDRplay device is open");
        return false;
    }
    const unsigned char hw = hwVer_.load(std::memory_order_relaxed);
    const unsigned char v = on ? 1 : 0;
    abi::ReasonForUpdateT reason = abi::Update_None;
    abi::ReasonForUpdateExt1T ext1 = abi::Update_Ext1_None;
    BlockRollback rb;  // a refused switch is put back (see the class)

    switch (hw) {
        case abi::kRsp1A:
        case abi::kRsp1B:
            rb.save(ch->rsp1aTunerParams.biasTEnable);
            ch->rsp1aTunerParams.biasTEnable = v;
            reason = abi::Update_Rsp1a_BiasTControl;
            break;
        case abi::kRsp2:
            rb.save(ch->rsp2TunerParams.biasTEnable);
            ch->rsp2TunerParams.biasTEnable = v;
            reason = abi::Update_Rsp2_BiasTControl;
            break;
        case abi::kRspDuo:
            rb.save(ch->rspDuoTunerParams.biasTEnable);
            ch->rspDuoTunerParams.biasTEnable = v;
            reason = abi::Update_RspDuo_BiasTControl;
            break;
        case abi::kRspDx:
        case abi::kRspDxR2:
            if (deviceParams_->devParams == nullptr) {
                setError("no SDRplay device parameters");
                return false;
            }
            rb.save(deviceParams_->devParams->rspDxParams.biasTEnable);
            deviceParams_->devParams->rspDxParams.biasTEnable = v;
            ext1 = abi::Update_RspDx_BiasTControl;
            break;
        default:
            setError("this RSP has no bias tee");
            return false;
    }
    if (!updateLocked(reason, ext1, "bias tee change")) {
        undoRefused(rb, controlAbandoned_);
        return false;
    }
    biasT_.store(on, std::memory_order_relaxed);
    core::diagLogf("source: SDRplay bias tee %s", on ? "ON" : "off");
    return true;
}

bool SdrPlaySource::rfNotchSupported() const {
    switch (hwVer_.load(std::memory_order_relaxed)) {
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

bool SdrPlaySource::setRfNotch(bool on) {
    std::lock_guard<std::mutex> lk(devMutex_);
    abi::RxChannelParamsT* ch = chParamsLocked();
    if (ch == nullptr) {
        setError("no SDRplay device is open");
        return false;
    }
    const unsigned char hw = hwVer_.load(std::memory_order_relaxed);
    const unsigned char v = on ? 1 : 0;
    abi::ReasonForUpdateT reason = abi::Update_None;
    abi::ReasonForUpdateExt1T ext1 = abi::Update_Ext1_None;
    BlockRollback rb;  // a refused switch is put back (see the class)

    switch (hw) {
        case abi::kRsp1A:
        case abi::kRsp1B:
            if (deviceParams_->devParams == nullptr) {
                setError("no SDRplay device parameters");
                return false;
            }
            // NOT in the channel block on an RSP1A/1B: the notch is in front
            // of the whole front end, so the API puts it in DevParams.
            rb.save(deviceParams_->devParams->rsp1aParams.rfNotchEnable);
            deviceParams_->devParams->rsp1aParams.rfNotchEnable = v;
            reason = abi::Update_Rsp1a_RfNotchControl;
            break;
        case abi::kRsp2:
            rb.save(ch->rsp2TunerParams.rfNotchEnable);
            ch->rsp2TunerParams.rfNotchEnable = v;
            reason = abi::Update_Rsp2_RfNotchControl;
            break;
        case abi::kRspDuo:
            rb.save(ch->rspDuoTunerParams.rfNotchEnable);
            ch->rspDuoTunerParams.rfNotchEnable = v;
            reason = abi::Update_RspDuo_RfNotchControl;
            break;
        case abi::kRspDx:
        case abi::kRspDxR2:
            if (deviceParams_->devParams == nullptr) {
                setError("no SDRplay device parameters");
                return false;
            }
            rb.save(deviceParams_->devParams->rspDxParams.rfNotchEnable);
            deviceParams_->devParams->rspDxParams.rfNotchEnable = v;
            ext1 = abi::Update_RspDx_RfNotchControl;
            break;
        default:
            setError("this RSP has no broadcast FM notch");
            return false;
    }
    if (!updateLocked(reason, ext1, "FM notch change")) {
        undoRefused(rb, controlAbandoned_);
        return false;
    }
    rfNotch_.store(on, std::memory_order_relaxed);
    return true;
}

bool SdrPlaySource::dabNotchSupported() const {
    switch (hwVer_.load(std::memory_order_relaxed)) {
        case abi::kRsp1A:
        case abi::kRsp1B:
        case abi::kRspDuo:
        case abi::kRspDx:
        case abi::kRspDxR2: return true;
        default: break;
    }
    // The RSP2 has an FM notch and no DAB one.
    return false;
}

bool SdrPlaySource::setDabNotch(bool on) {
    std::lock_guard<std::mutex> lk(devMutex_);
    abi::RxChannelParamsT* ch = chParamsLocked();
    if (ch == nullptr) {
        setError("no SDRplay device is open");
        return false;
    }
    const unsigned char hw = hwVer_.load(std::memory_order_relaxed);
    const unsigned char v = on ? 1 : 0;
    abi::ReasonForUpdateT reason = abi::Update_None;
    abi::ReasonForUpdateExt1T ext1 = abi::Update_Ext1_None;
    BlockRollback rb;  // a refused switch is put back (see the class)

    switch (hw) {
        case abi::kRsp1A:
        case abi::kRsp1B:
            if (deviceParams_->devParams == nullptr) {
                setError("no SDRplay device parameters");
                return false;
            }
            rb.save(deviceParams_->devParams->rsp1aParams.rfDabNotchEnable);
            deviceParams_->devParams->rsp1aParams.rfDabNotchEnable = v;
            reason = abi::Update_Rsp1a_RfDabNotchControl;
            break;
        case abi::kRspDuo:
            rb.save(ch->rspDuoTunerParams.rfDabNotchEnable);
            ch->rspDuoTunerParams.rfDabNotchEnable = v;
            reason = abi::Update_RspDuo_RfDabNotchControl;
            break;
        case abi::kRspDx:
        case abi::kRspDxR2:
            if (deviceParams_->devParams == nullptr) {
                setError("no SDRplay device parameters");
                return false;
            }
            rb.save(deviceParams_->devParams->rspDxParams.rfDabNotchEnable);
            deviceParams_->devParams->rspDxParams.rfDabNotchEnable = v;
            ext1 = abi::Update_RspDx_RfDabNotchControl;
            break;
        default:
            setError("this RSP has no DAB notch");
            return false;
    }
    if (!updateLocked(reason, ext1, "DAB notch change")) {
        undoRefused(rb, controlAbandoned_);
        return false;
    }
    dabNotch_.store(on, std::memory_order_relaxed);
    return true;
}

bool SdrPlaySource::hdrModeSupported() const {
    const unsigned char hw = hwVer_.load(std::memory_order_relaxed);
    return hw == abi::kRspDx || hw == abi::kRspDxR2;
}

bool SdrPlaySource::setHdrMode(bool on) {
    std::lock_guard<std::mutex> lk(devMutex_);
    if (!hdrModeSupported()) {
        setError("HDR mode is an RSPdx feature");
        return false;
    }
    if (deviceParams_ == nullptr || deviceParams_->devParams == nullptr) {
        setError("no SDRplay device is open");
        return false;
    }
    BlockRollback rb;
    rb.save(deviceParams_->devParams->rspDxParams.hdrEnable);
    deviceParams_->devParams->rspDxParams.hdrEnable = on ? 1 : 0;
    if (!updateLocked(abi::Update_None, abi::Update_RspDx_HdrEnable, "HDR mode change")) {
        undoRefused(rb, controlAbandoned_);
        return false;
    }

    // THE HDR BANDWIDTH IS THE ONE FIELD WHOSE OFFSET MOVED BETWEEN API
    // VERSIONS (sdrplay_api_decl.hpp, kRspDxTunerLayoutVersion): at 3.07 and
    // 3.11 it sits four bytes lower inside the channel block. Writing it
    // against an older service would land on padding at best and on the
    // neighbouring member at worst, so below 3.15 we leave the API's own
    // default (1.7 MHz) alone and say so once.
    const float ver = apiVersion_.load(std::memory_order_relaxed);
    if (on && !abi::versionAtLeast(ver, abi::kRspDxTunerLayoutVersion)) {
        core::diagLogf(
            "source: SDRplay HDR bandwidth left at the API default - it moved in the parameter "
            "block after 3.11 and this service reports %.2f",
            static_cast<double>(ver));
    } else if (on) {
        abi::RxChannelParamsT* ch = chParamsLocked();
        if (ch != nullptr) {
            ch->rspDxTunerParams.hdrBw = abi::RspDx_HDRMODE_BW_1_700;
            updateLocked(abi::Update_None, abi::Update_RspDx_HdrBw, "HDR bandwidth change");
        }
    }

    hdrMode_.store(on, std::memory_order_relaxed);
    return true;
}

// --- health ---------------------------------------------------------------

std::string SdrPlaySource::streamHealthLine() {
    std::lock_guard<std::mutex> lk(link_->healthMutex);
    return healthLineLocked(*link_);
}

void SdrPlaySource::setStreamHealthWindowForTest(std::chrono::milliseconds w) {
    std::lock_guard<std::mutex> lk(link_->healthMutex);
    link_->healthWindow = w;
}

void SdrPlaySource::setStreamStallLimitForTest(std::chrono::milliseconds w) {
    link_->stallLimitNs.store(std::chrono::duration_cast<std::chrono::nanoseconds>(w).count(),
                              std::memory_order_relaxed);
}

void SdrPlaySource::checkForStallFromRead() {
    // THE PIPELINE'S SOURCE THREAD, on a read that came back empty. No
    // devMutex_: a GUI-thread control can hold it for up to kControlWait +
    // kUpdateWait, and the reader must not queue behind that. Everything read
    // here is atomic, and the one thing raised (streamStalled_) is read by
    // vendorUnreachableLocked() as an atomic too.
    if (!link_->accepting.load(std::memory_order_relaxed)) { return; }
    if (streamStalled_.load(std::memory_order_acquire)) { return; }
    const std::int64_t start = link_->streamStartNs.load(std::memory_order_relaxed);
    if (start == 0) { return; }
    const std::int64_t now = steadyNowNs();
    const std::int64_t limit = link_->stallLimitNs.load(std::memory_order_relaxed);

    // THE READER MUST HAVE BEEN SEEING NOTHING, READ AFTER READ, FOR THE WHOLE
    // LIMIT (the 0c59853 review). A healthy pipeline reads every ~20 ms
    // (kReadWait plus its own 1 ms back-off); a gap far longer than that
    // between two empty reads means THIS process was not running - laptop
    // sleep, a paused VM, a debugger - and then the service's callback thread,
    // which lives in this process too, was not running either. Such a gap
    // starts a new run instead of counting as silence.
    constexpr std::int64_t kReaderGapNs = 250LL * 1000000LL;
    const std::int64_t prevEmpty = link_->lastEmptyReadNs.exchange(now, std::memory_order_relaxed);
    std::int64_t emptySince = link_->emptySinceNs.load(std::memory_order_relaxed);
    if (prevEmpty == 0 || now - prevEmpty > kReaderGapNs || emptySince == 0) {
        link_->emptySinceNs.store(now, std::memory_order_relaxed);
        return;
    }
    if (now - emptySince < limit) { return; }

    // ...AND THE SERVICE MUST NOT HAVE CALLED US FOR IT EITHER.
    const std::int64_t last = link_->lastCallbackNs.load(std::memory_order_relaxed);
    const std::int64_t since = (last > start) ? last : start;
    if (now - since < limit) { return; }
    if (streamStalled_.exchange(true, std::memory_order_acq_rel)) { return; }

    // THE SAME TREATMENT AS A SERVICE THAT ANSWERED (14). Nothing of ours
    // enters the vendor DLL for this device again (vendorUnreachableLocked),
    // and the process's session is finished (markSessionLost): the 0.97.0
    // RSPdx hang report is a teardown walking into exactly this service.
    // link_->api is the table Init was called through, written before Init.
    if (link_->api != nullptr) {
        markSessionLost(*link_->api, "the stream stopped delivering samples");
    }
    noteFaultOn(*link_, "stream", sdrPlayStreamStalledSentence());
    core::diagWarnf("source: SDRplay stream stalled - no callback for %lld ms; the service is "
                    "treated as gone and the radio is released",
                    static_cast<long long>((now - since) / 1000000));
}

std::uint64_t SdrPlaySource::droppedSamples() const {
    return link_->dropped.load(std::memory_order_relaxed);
}

std::uint64_t SdrPlaySource::overloadEvents() const {
    return link_->overloads.load(std::memory_order_relaxed);
}

double SdrPlaySource::currentGainDb() const {
    return link_->currGainDb.load(std::memory_order_relaxed);
}

bool SdrPlaySource::haveCurrentGainDb() const {
    return link_->haveGainDb.load(std::memory_order_relaxed);
}

}  // namespace cascade::source
