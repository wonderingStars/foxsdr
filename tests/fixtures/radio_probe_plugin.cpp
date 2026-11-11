// radio_probe_plugin.cpp - A REAL PLUGIN FOR THE PLUGINS-PER-RADIO TESTS (multi-radio engine, stage 6; tests/test_engine_api_plugin_radio.cpp).
//
// An I/Q decoder that says WHERE IT IS RUNNING by what it is fed and by what the host tells it:
//
//   * from the I/Q block it is handed it estimates the dominant carrier's offset from the centre (the mean phase step between neighbouring samples, which for a generator radio
//     with one tone is that tone's offset to a few Hz) and counts the frames;
//   * from create() and retune() it knows the centre the radio was at; from the host's get_state it reads the centre and the running flag of the radio the HOST says it runs on;
//   * it registers three command keys: 1 asks the host to tune to 7.2 MHz (set_frequency, a TUNE-grant request), 2 makes its next poll_text() fault on purpose (an access
//     violation, which the guard absorbs: process() is the one entry point the ABI leaves unguarded), 3 makes its next poll_text() run for 1.5 s (a hang).
//
// Every few frames' worth of poll it appends one line to "<module file name>.log" in the directory RADIO_PROBE_DIR names, and returns the same text as decoder output:
//
//   file=<leaf> tone_hz=<n> centre_create=<n> centre_now=<n> rate=<n> frames=<n> state_centre=<n> state_running=<0|1> asked=<rc>
//
// The test copies this one module under several names (each a plugin of its own) and reads the last line of each log. The module includes ONLY plugin_abi.h, as every third-party
// plugin must.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "plugin_abi.h"

namespace {

const CascadeHostApi* g_host = nullptr;
std::atomic<bool> g_crash{false};
std::atomic<bool> g_spin{false};
std::atomic<int> g_asked{-99};   // the answer to the last set_frequency this plugin made (-99: none yet)

std::string leaf() {
#if defined(_WIN32)
    HMODULE h = nullptr;
    if (!::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCSTR>(&leaf), &h)) { return "?"; }
    char path[1024];
    const DWORD n = ::GetModuleFileNameA(h, path, static_cast<DWORD>(sizeof(path)));
    std::string p(path, n);
#else
    Dl_info info;
    if (dladdr(reinterpret_cast<void*>(&leaf), &info) == 0 || info.dli_fname == nullptr) { return "?"; }
    std::string p(info.dli_fname);
#endif
    const std::size_t cut = p.find_last_of("/\\");
    return cut == std::string::npos ? p : p.substr(cut + 1);
}

std::string logDir() {
    const char* d = std::getenv("RADIO_PROBE_DIR");
    return d != nullptr ? std::string(d) : std::string();
}

void appendLog(const std::string& line) {
    const std::string dir = logDir();
    if (dir.empty()) { return; }
    std::FILE* f = std::fopen((dir + "/" + leaf() + ".log").c_str(), "ab");
    if (f == nullptr) { return; }
    std::fwrite(line.data(), 1, line.size(), f);
    std::fclose(f);
}

struct Instance {
    double rate = 0.0;
    double centreCreate = 0.0;
    double centreNow = 0.0;
    double accRe = 0.0, accIm = 0.0;     // sum of x[n] * conj(x[n-1])
    float prevRe = 0.0f, prevIm = 0.0f;
    bool havePrev = false;
    std::uint64_t frames = 0;
    std::uint64_t reportedAt = 0;
};

void* create(double rateHz, double centreHz) {
    Instance* s = new (std::nothrow) Instance();
    if (s != nullptr) {
        s->rate = rateHz;
        s->centreCreate = centreHz;
        s->centreNow = centreHz;
    }
    return s;
}

void process(void* handle, const float* iq, size_t frames) {
    Instance* s = static_cast<Instance*>(handle);
    if (s == nullptr || iq == nullptr) { return; }
    for (size_t i = 0; i < frames; ++i) {
        const float re = iq[2 * i], im = iq[2 * i + 1];
        if (s->havePrev) {
            s->accRe += static_cast<double>(re) * s->prevRe + static_cast<double>(im) * s->prevIm;
            s->accIm += static_cast<double>(im) * s->prevRe - static_cast<double>(re) * s->prevIm;
        }
        s->prevRe = re;
        s->prevIm = im;
        s->havePrev = true;
    }
    s->frames += frames;
}

void retune(void* handle, double centreHz) {
    if (Instance* s = static_cast<Instance*>(handle)) { s->centreNow = centreHz; }
}

int32_t pollText(void* handle, char* buf, size_t cap) {
    Instance* s = static_cast<Instance*>(handle);
    if (s == nullptr || buf == nullptr) { return 0; }
    // on purpose: a plugin that faults in poll_text(), on its radio's DSP thread (the guard absorbs it and puts the plugin down)
    if (g_crash.load()) { *static_cast<volatile int*>(nullptr) = 1; }
    if (g_spin.exchange(false)) {
        // on purpose: a call that does not return for 1.5 s
        volatile std::uint64_t sink = 0;
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
        while (std::chrono::steady_clock::now() < end) { sink = sink + 1u; }
    }
    // the host's commands: 1 = tune, 2 = fault on purpose, 3 = hang
    if (g_host != nullptr && CASCADE_HOST_HAS(g_host, poll_command)) {
        uint32_t id = 0;
        while (g_host->poll_command(g_host->ctx, &id) == 1) {
            const uint32_t command = id;
            if (command == 1u && CASCADE_HOST_HAS(g_host, set_frequency)) { g_asked.store(g_host->set_frequency(g_host->ctx, 7.2e6)); }
            if (command == 2u) { g_crash.store(true); }
            if (command == 3u) { g_spin.store(true); }
        }
    }
    if (s->frames < s->reportedAt + 50000u) { return 0; }
    s->reportedAt = s->frames;
    const double tone = std::atan2(s->accIm, s->accRe) * s->rate / (2.0 * 3.14159265358979323846);
    s->accRe = s->accIm = 0.0;
    double stateCentre = -1.0;
    int running = -1;
    if (g_host != nullptr && CASCADE_HOST_HAS(g_host, get_state)) {
        CascadeReceiverState st{};
        st.structSize = static_cast<uint32_t>(sizeof(st));
        if (g_host->get_state(g_host->ctx, &st) == CASCADE_API_OK) {
            stateCentre = st.centreHz;
            running = (st.flags & CASCADE_STATE_RUNNING) != 0u ? 1 : 0;
        }
    }
    char line[512];
    const int n = std::snprintf(line, sizeof(line),
                                "file=%s tone_hz=%.0f centre_create=%.0f centre_now=%.0f rate=%.0f frames=%llu state_centre=%.0f state_running=%d asked=%d\n", leaf().c_str(), tone,
                                s->centreCreate, s->centreNow, s->rate, static_cast<unsigned long long>(s->frames), stateCentre, running, g_asked.load());
    if (n <= 0) { return 0; }
    appendLog(std::string(line, static_cast<size_t>(n)));
    const size_t out = static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap;
    std::memcpy(buf, line, out);
    return static_cast<int32_t>(out);
}

void destroy(void* handle) { delete static_cast<Instance*>(handle); }

const CascadeIqDecoderApi kIq = {
    static_cast<uint32_t>(sizeof(CascadeIqDecoderApi)),
    0.0,  // any rate
    0.0,
    &create,
    &process,
    &retune,
    &pollText,
    &destroy,
};

void attach(const CascadeHostApi* host) {
    g_host = host;
    if (host != nullptr && CASCADE_HOST_HAS(host, add_command)) {
        (void)host->add_command(host->ctx, 1u, "Tune 7.2 MHz");
        (void)host->add_command(host->ctx, 2u, "Fault");
        (void)host->add_command(host->ctx, 3u, "Hang");
    }
}

const CascadeHostClientApi kHostClient = {
    static_cast<uint32_t>(sizeof(CascadeHostClientApi)),
    &attach,
};

const CascadeCapabilityEntry kCapabilities[] = {
    {CASCADE_CAP_IQ_DECODER, static_cast<uint32_t>(sizeof(CascadeIqDecoderApi)), &kIq},
    {CASCADE_CAP_HOST_CLIENT, static_cast<uint32_t>(sizeof(CascadeHostClientApi)), &kHostClient},
};

// ITS OWN NAME PER COPY: the host takes two modules with one descriptor name to be an old copy and a new one of the same plugin, so the name carries the file's leaf.
char g_name[96] = "Radio Probe";

CascadePluginDesc kDesc = {
    static_cast<uint32_t>(sizeof(CascadePluginDesc)),
    CASCADE_PLUGIN_ABI_VERSION,
    g_name,
    "1.2.3",
    "FoxSDR tests",
    "MIT",
    CASCADE_CAP_IQ_DECODER | CASCADE_CAP_HOST_CLIENT,
    static_cast<uint32_t>(sizeof(kCapabilities) / sizeof(kCapabilities[0])),
    kCapabilities,
};

}  // namespace

extern "C" CASCADE_PLUGIN_EXPORT const CascadePluginDesc* cascade_plugin_query(uint32_t hostAbiVersion) {
    if (hostAbiVersion != CASCADE_PLUGIN_ABI_VERSION) { return nullptr; }
    std::snprintf(g_name, sizeof(g_name), "Radio Probe %s", leaf().c_str());
    return &kDesc;
}
