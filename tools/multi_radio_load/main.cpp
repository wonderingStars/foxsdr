// multi_radio_load - FoxSDR multi-radio engine, stage 0: can one process run
// four, then eight, receivers at once?  (docs/multi-radio-engine-design.md,
// sections 8 and 10.)
//
// WHAT THIS IS. N independent cascade::core::Pipeline objects in one process.
// Each is the engine's own receiver chain, unmodified: its own SPSC ring, its
// own source thread (Pipeline::sourceThreadBody paces the built-in generator
// at its sample rate), its own DSP thread (spectrum estimator + VFO + demod +
// AGC + resampler + post chain), and its own real AudioOut stream opened on
// the default output device by the Pipeline constructor.
//
// WHAT IS THE ENGINE'S OWN CODE AND WHAT IS REPRODUCED HERE.
//   Engine's own, used as-is: Pipeline (ring, source thread loop and pacing,
//   DSP thread, spectrum, audio chain, drop/underrun/fault counters),
//   SigGenSource (the generator), AudioOut (PortAudio/WASAPI), RtlSdrSource,
//   SoapySource, enumerateRtlSdr, enumerateIsolated (device scan),
//   Pipeline::setSource / setInputRateHz (how Engine installs a device).
//   Reproduced here, because it lives inside the Engine class (src/engine/):
//   the construction of N pipelines with the Engine's Pipeline::Config
//   (FFT 1024, alpha 0.5, audio on), the spectrum poll the window makes
//   (Pipeline::getLatestFrame, ~30 Hz here), mode/offset/bandwidth set-up, and
//   the device open/retune sequence of --device-mix (open, rate, centre, one
//   gain, then setInputRateHz + setSource). NOT exercised: Engine's control
//   thread, API host, plugins, recorders, scanner, the Slint window, the patch
//   runner. So a figure from here is the receivers' cost, not the whole
//   application's.
//
// NOTHING ABOUT CONFIG: this tool never reads or writes config.json, and never
// opens the diagnostic log. Audio is held at VOLUME ZERO (AudioOut::setVolume
// before start(); the gain is applied per sample in the callback).
//
// Exit codes: 0 all OK; 1 a ring drop or an audio/pipeline fault on any radio;
// 2 usage or set-up failure; 3 --device-mix and a required device is absent or
// would not open; 4 a radio's input fell more than 2 percent short of real time
// (no drop was counted but the source did not keep pace).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

#include "core/pipeline.hpp"
#include "core/spectrum_frame.hpp"
#include "dsp/demod.hpp"
#include "sink/audio_out.hpp"
#include "source/rtlsdr_source.hpp"
#include "source/siggen.hpp"
#include "source/soapy_enum_proc.hpp"
#include "source/soapy_source.hpp"

namespace {

using cascade::core::Pipeline;
using cascade::core::SpectrumFrame;
using cascade::dsp::DemodMode;
using Clock = std::chrono::steady_clock;

constexpr std::size_t kFftSize = 1024;   // gui/receiver_tables.hpp kFftSize
constexpr float kAveragingAlpha = 0.5f;  // gui/receiver_tables.hpp kAveragingAlpha

std::uint64_t ft(const FILETIME& f) {
    return (static_cast<std::uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
}

struct ThreadInfo {
    DWORD tid = 0;
    std::uint64_t created = 0;  // FILETIME of creation: orders source before DSP
};

// Every thread of this process (Toolhelp) with its creation time.
std::map<DWORD, ThreadInfo> snapshotThreads() {
    std::map<DWORD, ThreadInfo> out;
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) { return out; }
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    const DWORD pid = ::GetCurrentProcessId();
    if (::Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) { continue; }
            ThreadInfo ti;
            ti.tid = te.th32ThreadID;
            HANDLE h = ::OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, ti.tid);
            if (h != nullptr) {
                FILETIME c, e, k, u;
                if (::GetThreadTimes(h, &c, &e, &k, &u)) { ti.created = ft(c); }
                ::CloseHandle(h);
            }
            out[ti.tid] = ti;
        } while (::Thread32Next(snap, &te));
    }
    ::CloseHandle(snap);
    return out;
}

double threadCpuSec(DWORD tid) {
    HANDLE h = ::OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    if (h == nullptr) { return 0.0; }
    FILETIME c, e, k, u;
    double r = 0.0;
    if (::GetThreadTimes(h, &c, &e, &k, &u)) { r = static_cast<double>(ft(k) + ft(u)) / 1.0e7; }
    ::CloseHandle(h);
    return r;
}

double processCpuSec() {
    FILETIME c, e, k, u;
    ::GetProcessTimes(::GetCurrentProcess(), &c, &e, &k, &u);
    return static_cast<double>(ft(k) + ft(u)) / 1.0e7;
}

struct SysTimes {
    std::uint64_t idle = 0;
    std::uint64_t total = 0;
};
SysTimes sysTimes() {
    FILETIME i, k, u;
    ::GetSystemTimes(&i, &k, &u);
    return {ft(i), ft(k) + ft(u)};  // kernel time includes idle
}

double peakWorkingSetMiB() {
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof(pmc);
    ::GetProcessMemoryInfo(::GetCurrentProcess(), &pmc, sizeof(pmc));
    return static_cast<double>(pmc.PeakWorkingSetSize) / (1024.0 * 1024.0);
}

std::size_t threadCount() { return snapshotThreads().size(); }

// Default render endpoint and its shared-mode MIX format, straight from WASAPI.
std::string describeDefaultEndpoint() {
    std::string out = "(unavailable)";
    const HRESULT co = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    if (SUCCEEDED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                     __uuidof(IMMDeviceEnumerator),
                                     reinterpret_cast<void**>(&en)))) {
        IMMDevice* dev = nullptr;
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) {
            std::string name = "?";
            IPropertyStore* ps = nullptr;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps))) {
                PROPVARIANT pv;
                PropVariantInit(&pv);
                if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.pwszVal) {
                    char buf[256];
                    ::WideCharToMultiByte(CP_UTF8, 0, pv.pwszVal, -1, buf, sizeof(buf), nullptr, nullptr);
                    name = buf;
                }
                PropVariantClear(&pv);
                ps->Release();
            }
            IAudioClient* ac = nullptr;
            if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                        reinterpret_cast<void**>(&ac)))) {
                WAVEFORMATEX* wf = nullptr;
                if (SUCCEEDED(ac->GetMixFormat(&wf)) && wf != nullptr) {
                    char buf[512];
                    std::snprintf(buf, sizeof(buf),
                                  "%s - shared-mode mix format %lu Hz, %u channels, %u bit (format tag 0x%04x)",
                                  name.c_str(), static_cast<unsigned long>(wf->nSamplesPerSec),
                                  static_cast<unsigned>(wf->nChannels), static_cast<unsigned>(wf->wBitsPerSample),
                                  static_cast<unsigned>(wf->wFormatTag));
                    out = buf;
                    ::CoTaskMemFree(wf);
                }
                ac->Release();
            }
            dev->Release();
        }
        en->Release();
    }
    if (SUCCEEDED(co)) { ::CoUninitialize(); }
    return out;
}

struct Radio {
    int index = 0;
    std::string kind;  // "gen NFM", "gen WFM", "B200", "RTL-SDR"
    DemodMode mode = DemodMode::NFM;
    bool hardware = false;
    std::unique_ptr<Pipeline> pipe;
    Clock::time_point startedAt;
    DWORD dspTid = 0;
    DWORD srcTid = 0;
    // interval baselines
    std::uint64_t lastIn = 0;
    double lastDspCpu = 0.0;
    double lastSrcCpu = 0.0;
    std::uint64_t frames = 0;
    std::uint64_t lastSeq = 0;
    bool everFaulted = false;
    std::string faultText;
};

struct Args {
    int radios = 2;
    int seconds = 20;
    double rate = 2.4e6;
    bool deviceMix = false;
    bool listDevices = false;
    double centreHz = 98.0e6;
};

bool parse(int argc, char** argv, Args& a) {
    for (int i = 1; i < argc; ++i) {
        const std::string s = argv[i];
        const auto num = [&](double& v) {
            if (i + 1 >= argc) { return false; }
            v = std::atof(argv[++i]);
            return true;
        };
        double v = 0;
        if (s == "--radios") {
            if (!num(v)) { return false; }
            a.radios = static_cast<int>(v);
        } else if (s == "--seconds") {
            if (!num(v)) { return false; }
            a.seconds = static_cast<int>(v);
        } else if (s == "--rate") {
            if (!num(v)) { return false; }
            a.rate = v;
        } else if (s == "--centre") {
            if (!num(v)) { return false; }
            a.centreHz = v;
        } else if (s == "--device-mix") {
            a.deviceMix = true;
        } else if (s == "--list-devices") {
            a.listDevices = true;
        } else {
            return false;
        }
    }
    return a.radios >= 1 && a.radios <= 16 && a.seconds >= 1 && a.rate >= 100000.0;
}

void usage() {
    std::fprintf(stderr,
                 "usage: multi_radio_load --radios N --seconds S --rate HZ [--device-mix] [--centre HZ]\n"
                 "       multi_radio_load --list-devices\n"
                 "  --device-mix  radio 1 = a USRP B200 (SoapySDR/UHD), radio 2 = an RTL-SDR (native),\n"
                 "                the rest generators; exit 3 if either device is absent.\n");
}

}  // namespace

int main(int argc, char** argv) {
    Args args;
    if (!parse(argc, argv, args)) {
        usage();
        return 2;
    }

    // The enumeration child is foxsdr-engine.exe beside this exe when present (the engine's own
    // `--enumerate-json` child); enumerateIsolated otherwise looks for cascade.exe.
    std::string helper;
    {
        char path[MAX_PATH];
        ::GetModuleFileNameA(nullptr, path, MAX_PATH);
        std::string p = path;
        p = p.substr(0, p.find_last_of("\\/") + 1) + "foxsdr-engine.exe";
        if (::GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES) { helper = p; }
    }

    std::vector<cascade::source::SoapyDeviceInfo> soapy;
    std::vector<cascade::source::NativeDeviceInfo> rtl;
    if (args.listDevices || args.deviceMix) {
        const char* uhdDir = std::getenv("UHD_IMAGES_DIR");
        std::printf("UHD_IMAGES_DIR=%s\n", uhdDir ? uhdDir : "(unset)");
        cascade::source::EnumOptions eo;
        eo.helperPath = helper;
        const auto er = cascade::source::enumerateIsolated(eo);
        soapy = er.devices;
        rtl = cascade::source::enumerateRtlSdr();
        std::printf("soapy enumeration (child %s): outcome %d, %zu device(s)\n",
                    helper.empty() ? "default" : helper.c_str(), static_cast<int>(er.outcome), soapy.size());
        for (const auto& d : soapy) { std::printf("  soapy: %s | %s\n", d.label.c_str(), d.args.c_str()); }
        std::printf("native rtl-sdr enumeration: %zu device(s)\n", rtl.size());
        for (const auto& d : rtl) {
            std::printf("  rtl: %s | %s | %s\n", d.driver.c_str(), d.label.c_str(), d.args.c_str());
        }
        if (args.listDevices) { return 0; }
    }

    std::string b200Args;
    std::string rtlArgs;
    if (args.deviceMix) {
        for (const auto& d : soapy) {
            std::string low = d.args;
            std::transform(low.begin(), low.end(), low.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (b200Args.empty() && low.find("driver=uhd") != std::string::npos) { b200Args = d.args; }
        }
        if (!rtl.empty()) { rtlArgs = rtl.front().args; }
        std::printf("device-mix: B200 %s, RTL-SDR %s\n", b200Args.empty() ? "NOT FOUND" : b200Args.c_str(),
                    rtlArgs.empty() ? "NOT FOUND" : rtlArgs.c_str());
        if (b200Args.empty() || rtlArgs.empty()) { return 3; }
        if (args.radios < 2) { args.radios = 2; }
    }

    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    std::printf("multi_radio_load: %d radio(s), %d s, %.0f S/s per generator, FFT %zu, logical cores %u\n",
                args.radios, args.seconds, args.rate, kFftSize, cores);
    std::printf("default audio endpoint: %s\n", describeDefaultEndpoint().c_str());

    std::vector<Radio> radios(static_cast<std::size_t>(args.radios));
    // ---- construct: each Pipeline opens its own AudioOut on the default device ----
    for (int i = 0; i < args.radios; ++i) {
        Radio& r = radios[static_cast<std::size_t>(i)];
        r.index = i + 1;
        const bool b200 = args.deviceMix && i == 0;
        const bool rtlr = args.deviceMix && i == 1;
        r.mode = (i % 2 == 0) ? DemodMode::NFM : DemodMode::WFM;
        if (b200 || rtlr) {
            r.mode = DemodMode::WFM;
            r.hardware = true;
        }
        r.kind = b200 ? "B200" : rtlr ? "RTL-SDR" : (r.mode == DemodMode::NFM ? "gen NFM" : "gen WFM");
        Pipeline::Config cfg{args.rate, kFftSize, kAveragingAlpha, /*audioEnabled=*/true};
        r.pipe = std::make_unique<Pipeline>(cfg);
        // VOLUME ZERO before anything can be pushed to the stream.
        r.pipe->audio().setVolume(0.0f);
        r.pipe->setDemodMode(r.mode);
        r.pipe->setVfoOffsetHz(0.0);
        r.pipe->setVfoBandwidthHz(r.mode == DemodMode::WFM ? 200000.0 : 12500.0);
        if (r.mode == DemodMode::WFM) { r.pipe->setStereoEnabled(true); }
        if (!r.hardware) {
            auto& g = r.pipe->sigGen();
            g.setTone(0, 3000.0 + 250.0 * i, -20.0f);
            g.setTone(1, -310000.0 - 1000.0 * i, -45.0f);
            g.setNoiseFloorDb(-60.0f);
        }
        std::printf("radio %d (%s): audio opened=%d stream alive=%d host api '%s' device '%s' channels %d\n",
                    r.index, r.kind.c_str(), r.pipe->audio().everOpened() ? 1 : 0,
                    r.pipe->audio().streamAlive() ? 1 : 0, r.pipe->audio().openedHostApi().c_str(),
                    r.pipe->audio().openedDeviceName().c_str(), r.pipe->audio().channels());
    }

    // ---- hardware: open the way the Source panel's path does, then hand it to the pipeline ----
    if (args.deviceMix) {
        for (int i = 0; i < 2; ++i) {
            Radio& r = radios[static_cast<std::size_t>(i)];
            std::unique_ptr<cascade::source::DeviceSource> dev;
            if (i == 0) {
                dev = std::make_unique<cascade::source::SoapySource>();
            } else {
                dev = std::make_unique<cascade::source::RtlSdrSource>();
            }
            if (!dev->open(i == 0 ? b200Args : rtlArgs)) {
                std::fprintf(stderr, "open %s failed: %s\n", r.kind.c_str(), dev->lastError());
                return 3;
            }
            if (!dev->setSampleRateHz(2.4e6)) {
                const auto rates = dev->supportedSampleRatesHz();
                double best = 0.0;
                for (double x : rates) {
                    if (std::abs(x - 2.4e6) < std::abs(best - 2.4e6)) { best = x; }
                }
                std::printf("%s: 2.4 MS/s refused, nearest offered %.0f\n", r.kind.c_str(), best);
                if (best <= 0.0 || !dev->setSampleRateHz(best)) {
                    std::fprintf(stderr, "no usable rate on %s\n", r.kind.c_str());
                    return 3;
                }
            }
            dev->setCenterFrequencyHz(args.centreHz);
            const auto gains = dev->gains();
            if (!gains.empty()) {
                const auto& gi = gains.front();
                dev->setGainDb(gi.name, gi.minDb + 0.6 * (gi.maxDb - gi.minDb));
            }
            const double got = dev->sampleRateHz();
            std::printf("%s opened: %s, rate %.0f, centre %.3f MHz\n", r.kind.c_str(), dev->name(), got,
                        dev->centerFrequencyHz() / 1e6);
            r.pipe->setInputRateHz(got);
            r.pipe->setSource(std::move(dev));
        }
    }

    // ---- start sequentially; the new threads of each pipeline are told apart by creation time ----
    const SysTimes sys0 = sysTimes();
    const double proc0 = processCpuSec();
    const auto t0 = Clock::now();
    for (Radio& r : radios) {
        const auto before = snapshotThreads();
        r.startedAt = Clock::now();
        r.pipe->start();
        const auto after = snapshotThreads();
        std::vector<ThreadInfo> fresh;
        for (const auto& kv : after) {
            if (before.count(kv.first) == 0) { fresh.push_back(kv.second); }
        }
        std::sort(fresh.begin(), fresh.end(),
                  [](const ThreadInfo& a, const ThreadInfo& b) { return a.created < b.created; });
        // Pipeline::start spawns the source thread, then the DSP thread: the last two created.
        if (fresh.size() >= 2) {
            r.srcTid = fresh[fresh.size() - 2].tid;
            r.dspTid = fresh.back().tid;
        }
        std::printf("radio %d started: %zu new thread(s)\n", r.index, fresh.size());
    }

    std::printf("%-3s %-8s %12s %8s %10s %7s %6s %8s %9s %7s %7s %8s %6s\n", "R", "kind", "samples_in", "rate%",
                "ring_drops", "underr", "fault", "frames", "dsp_cpu_s", "dsp_1c%", "src_1c%", "dspmax_ms", "ringhw%");

    std::vector<SpectrumFrame> frame(radios.size());
    auto tPrev = t0;
    double procPrev = proc0;
    SysTimes sysPrev = sys0;

    const auto printRows = [&](bool final) {
        const auto now = Clock::now();
        const double wallTotal = std::chrono::duration<double>(now - t0).count();
        const double wallInt = std::chrono::duration<double>(now - tPrev).count();
        for (Radio& r : radios) {
            const std::uint64_t in = r.pipe->sourceSamplesRead();
            const std::uint64_t drop = r.pipe->ringDroppedSamples();
            const std::uint64_t under = r.pipe->audio().underruns();
            const double dspCpu = r.dspTid ? threadCpuSec(r.dspTid) : 0.0;
            const double srcCpu = r.srcTid ? threadCpuSec(r.srcTid) : 0.0;
            const double sinceStart = std::chrono::duration<double>(now - r.startedAt).count();
            const double wall = final ? sinceStart : wallInt;
            const double dspPct = (final ? dspCpu : dspCpu - r.lastDspCpu) / wall * 100.0;
            const double srcPct = (final ? srcCpu : srcCpu - r.lastSrcCpu) / wall * 100.0;
            const double ratePct =
                static_cast<double>(final ? in : in - r.lastIn) / (wall * r.pipe->inputRateHz()) * 100.0;
            const bool fault = r.pipe->faulted() || r.pipe->dspThreadExceptions() > 0 ||
                               !r.pipe->audio().everOpened() || !r.pipe->audio().streamAlive();
            if (fault && !r.everFaulted) {
                r.everFaulted = true;
                r.faultText = r.pipe->faulted()                  ? r.pipe->faultMessage()
                              : r.pipe->dspThreadExceptions() > 0 ? "dsp thread exception"
                                                                   : "audio stream not open/alive";
            }
            const std::size_t cap = std::max<std::size_t>(1, r.pipe->ringCapacity());
            std::printf("%-3d %-8s %12llu %7.2f%% %10llu %7llu %6s %8llu %9.2f %6.1f%% %6.1f%% %8.2f %5.0f%%\n",
                        r.index, r.kind.c_str(), static_cast<unsigned long long>(in), ratePct,
                        static_cast<unsigned long long>(drop), static_cast<unsigned long long>(under),
                        r.everFaulted ? "FAULT" : "ok", static_cast<unsigned long long>(r.frames), dspCpu, dspPct,
                        srcPct, static_cast<double>(r.pipe->dspBlockLongestNs()) / 1.0e6,
                        100.0 * static_cast<double>(r.pipe->ringHighWaterSamples()) / static_cast<double>(cap));
            r.lastIn = in;
            r.lastDspCpu = dspCpu;
            r.lastSrcCpu = srcCpu;
        }
        const double proc = processCpuSec();
        const SysTimes st = sysTimes();
        const double procWall = final ? wallTotal : wallInt;
        const double procCpu = proc - (final ? proc0 : procPrev);
        const SysTimes& sb = final ? sys0 : sysPrev;
        const double sysBusy =
            (st.total > sb.total) ? 100.0 * (1.0 - static_cast<double>(st.idle - sb.idle) /
                                                       static_cast<double>(st.total - sb.total))
                                  : 0.0;
        std::printf("    process: %.1f%% of whole machine (%.2f cores of %u), machine-wide busy %.1f%%, "
                    "peak working set %.0f MiB, threads %zu, t=%.0fs\n",
                    procCpu / procWall / cores * 100.0, procCpu / procWall, cores, sysBusy, peakWorkingSetMiB(),
                    threadCount(), wallTotal);
        std::fflush(stdout);
        tPrev = now;
        procPrev = proc;
        sysPrev = st;
    };

    const auto end = t0 + std::chrono::seconds(args.seconds);
    auto nextPrint = t0 + std::chrono::seconds(10);
    while (Clock::now() < end) {
        // The window's spectrum poll: newest frame per radio.
        for (std::size_t i = 0; i < radios.size(); ++i) {
            if (radios[i].pipe->getLatestFrame(frame[i])) { ++radios[i].frames; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
        if (Clock::now() >= nextPrint && Clock::now() < end - std::chrono::seconds(2)) {
            printRows(false);
            nextPrint += std::chrono::seconds(10);
        }
    }
    std::printf("---- final (whole run, before stop) ----\n");
    printRows(true);

    // ---- verdict, read before stop() so shutdown cannot colour it ----
    int exitCode = 0;
    for (Radio& r : radios) {
        const std::uint64_t drop = r.pipe->ringDroppedSamples();
        const double wall = std::chrono::duration<double>(Clock::now() - r.startedAt).count();
        const double ratePct =
            100.0 * static_cast<double>(r.pipe->sourceSamplesRead()) / (wall * r.pipe->inputRateHz());
        if (drop != 0 || r.everFaulted) {
            exitCode = 1;
            std::printf("radio %d FAIL: ring drops %llu%s%s\n", r.index, static_cast<unsigned long long>(drop),
                        r.everFaulted ? ", fault: " : "", r.faultText.c_str());
        } else if (ratePct < 98.0 && exitCode == 0) {
            exitCode = 4;
            std::printf("radio %d: input at %.2f%% of real time\n", r.index, ratePct);
        }
    }
    std::printf("underruns (reported, not part of the exit code):");
    for (Radio& r : radios) { std::printf(" R%d=%llu", r.index, static_cast<unsigned long long>(r.pipe->audio().underruns())); }
    std::printf("\nVERDICT: %s (exit %d)\n", exitCode == 0 ? "all OK" : "NOT OK", exitCode);

    for (Radio& r : radios) { r.pipe->stop(); }
    radios.clear();
    return exitCode;
}
