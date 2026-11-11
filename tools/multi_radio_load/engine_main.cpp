// multi_radio_load_engine - the SUCCESSOR of multi_radio_load for the multi-radio engine's stage 4: the same measurement (eight radios on paced generators at
// 2 MS/s, each demodulating, for as long as asked) through the REAL ENGINE instead of bare Pipelines - its control thread, API host, per-radio publish and
// housekeeping, recorders idle, audio streams opened by the engine on the default output device at VOLUME ZERO. It is what the stage-0 tool said it did not
// cover ("NOT exercised: Engine's control thread, API host, ...").
//
// Usage: multi_radio_load_engine [radios (2..8, default 8)] [seconds (default 60)] [reverse|-] [radio probe module]
//
// With a radio probe module (tests/fixtures/radio_probe_plugin.cpp, built as radio_probe_fixture) the tool copies it under four names into a plugin folder of its own, puts one
// copy on each of radios 1 to 4 (FOXAPI_OP_PLUGIN_RADIO; multi-radio stage 6) and reports, per copy, how many reports its decoder wrote - so that the load is the load WITH a
// decoder fed on each of four radios, and the figures prove the decoders were fed.
//
// Prints, every 10 s and at the end: whole-process CPU as a share of the machine and in cores, the working set, the threads, and for each radio the samples
// its source read, the samples its ring DROPPED, the audio underruns and whether it faulted. Exit 0 = every radio read, none dropped, none faulted; 1 = a
// drop or a fault. Never reads or writes config.json: the engine runs in a scratch profile (APPDATA and friends are pointed at a temp folder first).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h>
#include <psapi.h>
#include <tlhelp32.h>

#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine/receiver.hpp"
#include "foxsdr_api.h"

namespace {
std::string g_pluginDir;
std::string pluginDirText() { return g_pluginDir; }
}  // namespace

namespace cascade::gui {
struct AppWindowTestAccess {
    using Engine = cascade::engine::Engine;
    static void installPluginDir() { Engine::testHooks_.pluginDir = &pluginDirText; }
    static bool runnerHas(Engine& e, std::size_t slot, const std::string& file) { return e.runnerOf(slot).hasInstances(file); }
    static std::uint64_t dropped(Engine& e, std::size_t slot) { return e.receivers_[slot] ? e.receivers_[slot]->pipeline_.ringDroppedSamples() : 0; }
    static std::uint64_t read(Engine& e, std::size_t slot) { return e.receivers_[slot] ? e.receivers_[slot]->pipeline_.sourceSamplesRead() : 0; }
    static std::uint64_t underruns(Engine& e, std::size_t slot) { return e.receivers_[slot] ? e.receivers_[slot]->pipeline_.audio().underruns() : 0; }
    static bool faulted(Engine& e, std::size_t slot) { return e.receivers_[slot] && e.receivers_[slot]->pipeline_.faulted(); }
    static std::uint64_t produced(Engine& e, std::size_t slot) { return e.receivers_[slot] ? e.receivers_[slot]->pipeline_.audioSamplesProduced() : 0; }
    static int recoveries(Engine& e, std::size_t slot) { return e.receivers_[slot] ? e.receivers_[slot]->audioRecoveries_ : -1; }
    static std::string note(Engine& e, std::size_t slot) { return e.receivers_[slot] ? e.receivers_[slot]->audioHealthNote_ : std::string(); }
    static bool running(Engine& e, std::size_t slot) { return e.receivers_[slot] && e.receivers_[slot]->pipeline_.running(); }
    // The radio's sink as it stands: open and playing, ever opened, its lead in frames, the frames queued, the priming callbacks it has served.
    static std::string sink(Engine& e, std::size_t slot) {
        if (!e.receivers_[slot]) { return "no radio"; }
        cascade::sink::AudioOut& a = e.receivers_[slot]->pipeline_.audio();
        char b[400];
        std::snprintf(b, sizeof(b), "sink %s, alive %d, device index %d, ever opened %d, channels %d, lead %zu frames, queued %zu frames, priming callbacks %llu",
                      a.running() ? "playing" : "NOT playing", a.streamAlive() ? 1 : 0, a.openedDeviceRequested(), a.everOpened() ? 1 : 0, a.channels(), a.leadFrames(),
                      a.ringFrames(), static_cast<unsigned long long>(a.primingCallbacks()));
        return b;
    }
};
}  // namespace cascade::gui

namespace {

using Access = cascade::gui::AppWindowTestAccess;
using Engine = cascade::engine::Engine;

double cpuSeconds() {
    FILETIME c, e, k, u;
    ::GetProcessTimes(::GetCurrentProcess(), &c, &e, &k, &u);
    const auto ticks = [](const FILETIME& f) { return static_cast<double>((static_cast<std::uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime) * 1e-7; };
    return ticks(k) + ticks(u);
}

std::size_t threads() {
    std::size_t n = 0;
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) { return 0; }
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    const DWORD pid = ::GetCurrentProcessId();
    if (::Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID == pid) { ++n; }
        } while (::Thread32Next(snap, &te));
    }
    ::CloseHandle(snap);
    return n;
}

// PER-THREAD CPU (stage 7, B3): the CPU seconds of every thread of this process, by thread id, with the name the engine gave it (SetThreadDescription: "dsp.2", "source.3", ...).
// Sampled every ten seconds; the busiest threads of a window that went over one core are printed, so that a spike has a name.
struct ThreadCpu {
    std::string name;
    double seconds = 0.0;
};
std::map<DWORD, ThreadCpu> threadCpu() {
    std::map<DWORD, ThreadCpu> out;
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) { return out; }
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    const DWORD pid = ::GetCurrentProcessId();
    if (::Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) { continue; }
            HANDLE h = ::OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
            if (h == nullptr) { continue; }
            FILETIME c, e, k, u;
            ThreadCpu t;
            if (::GetThreadTimes(h, &c, &e, &k, &u)) {
                const auto ticks = [](const FILETIME& f) { return static_cast<double>((static_cast<std::uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime) * 1e-7; };
                t.seconds = ticks(k) + ticks(u);
            }
            PWSTR desc = nullptr;
            if (SUCCEEDED(::GetThreadDescription(h, &desc)) && desc != nullptr) {
                for (const wchar_t* w = desc; *w != 0; ++w) { t.name.push_back(*w < 128 ? static_cast<char>(*w) : '?'); }
                ::LocalFree(desc);
            }
            ::CloseHandle(h);
            out[te.th32ThreadID] = t;
        } while (::Thread32Next(snap, &te));
    }
    ::CloseHandle(snap);
    return out;
}

double workingSetMiB() {
    PROCESS_MEMORY_COUNTERS pmc{};
    ::GetProcessMemoryInfo(::GetCurrentProcess(), &pmc, sizeof(pmc));
    return static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
}

FoxCommand makeCmd(std::uint32_t op, double n0, long long i0, std::uint32_t target = 0) {
    FoxCommand c{};
    c.structSize = sizeof(c);
    c.op = op;
    c.num[0] = n0;
    c.ival[0] = i0;
    c.target = target;
    return c;
}

}  // namespace

int main(int argc, char** argv) {
    const int radios = argc > 1 ? std::max(2, std::min(static_cast<int>(cascade::engine::kMaxReceivers), std::atoi(argv[1])))
                                : static_cast<int>(cascade::engine::kMaxReceivers);
    const int seconds = argc > 2 ? std::max(5, std::atoi(argv[2])) : 60;
    // A scratch profile: this tool never touches the real config.
    const std::filesystem::path scratch = std::filesystem::temp_directory_path() / ("foxsdr_multi_radio_load_engine_" + std::to_string(::GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::create_directories(scratch, ec);
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME"}) { ::SetEnvironmentVariableA(v, scratch.string().c_str()); }
    ::SetEnvironmentVariableA("FOXSDR_TELEMETRY_URL", "http://127.0.0.1:9");
    ::SetEnvironmentVariableA("FOXSDR_CRASH_URL", "http://127.0.0.1:9");
    ::SetEnvironmentVariableA("FOXSDR_UPDATE_URL", "http://127.0.0.1:9");

    // THE PLUGINS (optional): four copies of the radio probe, in a folder of their own, found by the engine's first scan.
    const std::string probeModule = argc > 4 ? argv[4] : std::string();
    std::vector<std::string> probeFiles;
    const std::filesystem::path probeLogs = scratch / "probe-logs";
    if (!probeModule.empty()) {
        const std::filesystem::path dir = scratch / "plugins";
        std::filesystem::create_directories(dir, ec);
        std::filesystem::create_directories(probeLogs, ec);
        ::SetEnvironmentVariableA("RADIO_PROBE_DIR", probeLogs.string().c_str());
        _putenv_s("RADIO_PROBE_DIR", probeLogs.string().c_str());   // (the module reads it with getenv, which has its own copy of the environment)
        for (int k = 1; k <= 4; ++k) {
            const std::string leaf = "rp-load" + std::to_string(k) + "-1.2.3-abi3-win-x64.dll";
            std::filesystem::copy_file(probeModule, dir / leaf, std::filesystem::copy_options::overwrite_existing, ec);
            probeFiles.push_back(leaf);
        }
        g_pluginDir = dir.string();
        Access::installPluginDir();
    }
    const FoxEngineApi* api = cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR);
    Engine eng;
    eng.configureApi("token=load");
    eng.initialise();
    eng.startControlThread();
    FoxSession* session = nullptr;
    FoxSessionParams sp{};
    sp.structSize = sizeof(sp);
    sp.flags = FOXAPI_SESSION_LOCAL;
    sp.grants = FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS | FOXAPI_GRANT_AUDIO;
    sp.clientName = "multi_radio_load_engine";
    sp.clientVersion = "1";
    if (api == nullptr || api->open_session(eng.apiHandle(), &sp, &session) != FOXAPI_OK || session == nullptr) {
        std::printf("could not open an API session\n");
        return 2;
    }
    const auto send = [&](FoxCommand c) {
        FoxSubmitResult r{};
        r.structSize = sizeof(r);
        (void)api->submit(session, &c, 1, &r);
    };
    for (int n = 2; n <= radios; ++n) { send(makeCmd(FOXAPI_OP_RADIO_ADD, 20000.0 * n, n)); }
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    // one probe on each of radios 1 to 4 (radio 1 is where a plugin is until it is put elsewhere)
    for (std::size_t k = 1; k < probeFiles.size() && static_cast<int>(k) < radios; ++k) {
        FoxCommand c{};
        c.structSize = sizeof(c);
        c.op = FOXAPI_OP_PLUGIN_RADIO;
        c.ival[0] = static_cast<long long>(k) + 1;
        std::snprintf(c.text, sizeof(c.text), "%s", probeFiles[k].c_str());
        FoxSubmitResult r{};
        r.structSize = sizeof(r);
        (void)api->submit(session, &c, 1, &r);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    // Every radio at volume zero, in WFM like the stage-0 mix, then all running.
    for (int n = 1; n <= radios; ++n) {
        send(makeCmd(FOXAPI_OP_SET_VOLUME, 0.0, 0, static_cast<std::uint32_t>(n)));
        send(makeCmd(FOXAPI_OP_SET_MODE, 0.0, FOXAPI_DEMOD_WFM, static_cast<std::uint32_t>(n)));
    }
    if (argc > 3 && std::string(argv[3]) == "reverse") {
        // One at a time, the last radio first (a diagnostic: does the radio that starts first keep the audio that the others lose?).
        for (int n = radios; n >= 1; --n) {
            send(makeCmd(FOXAPI_OP_RUN, 0.0, 1, static_cast<std::uint32_t>(n)));
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
    } else {
        send(makeCmd(FOXAPI_OP_RUN, 0.0, 1, FOXAPI_TARGET_ALL_RADIOS));
    }
    std::this_thread::sleep_for(std::chrono::seconds(3));   // settle: the first samples, the audio priming

    std::vector<std::uint64_t> drop0, read0, under0, prod0;
    for (int n = 0; n < radios; ++n) {
        drop0.push_back(Access::dropped(eng, static_cast<std::size_t>(n)));
        read0.push_back(Access::read(eng, static_cast<std::size_t>(n)));
        under0.push_back(Access::underruns(eng, static_cast<std::size_t>(n)));
        prod0.push_back(Access::produced(eng, static_cast<std::size_t>(n)));
    }
    const double cpu0 = cpuSeconds();
    const auto t0 = std::chrono::steady_clock::now();
    unsigned cores = std::thread::hardware_concurrency();
    if (cores == 0) { cores = 1; }
    std::printf("multi_radio_load_engine: %d radios through the engine, %d s, %u logical processors\n", radios, seconds, cores);
    double peak = 0.0;
    std::map<DWORD, ThreadCpu> prevThreads = threadCpu();
    double lastCpu = cpu0;
    auto lastT = t0;
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        const auto now = std::chrono::steady_clock::now();
        const double wall = std::chrono::duration<double>(now - lastT).count();
        const double cpu = cpuSeconds();
        const double used = (cpu - lastCpu) / wall;
        peak = std::max(peak, used);
        lastCpu = cpu;
        lastT = now;
        {
            // The busiest threads of this window (always the top four; the whole table when the window went over one core).
            std::map<DWORD, ThreadCpu> cur = threadCpu();
            std::vector<std::pair<double, std::string>> rows;
            for (const auto& [id, t] : cur) {
                const auto was = prevThreads.find(id);
                const double d = (t.seconds - (was == prevThreads.end() ? 0.0 : was->second.seconds)) / wall;
                char b[96];
                std::snprintf(b, sizeof(b), "%s#%lu %.2f", t.name.empty() ? "(unnamed)" : t.name.c_str(), static_cast<unsigned long>(id), d);
                rows.emplace_back(d, b);
            }
            std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            const std::size_t show = used > 1.0 ? std::min<std::size_t>(rows.size(), 12) : std::min<std::size_t>(rows.size(), 4);
            std::printf("      threads:");
            for (std::size_t i = 0; i < show; ++i) { std::printf("  %s", rows[i].second.c_str()); }
            std::printf("\n");
            prevThreads = std::move(cur);
        }
        std::printf("  t=%3.0fs cpu %.2f core(s) = %.2f%% of the machine, working set %.0f MiB, %zu threads\n", std::chrono::duration<double>(now - t0).count(), used,
                    100.0 * used / cores, workingSetMiB(), threads());
        std::fflush(stdout);
        if (std::chrono::duration<double>(now - t0).count() >= seconds) { break; }
    }
    const double wallTotal = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double avg = (cpuSeconds() - cpu0) / wallTotal;
    int bad = 0;
    for (int n = 0; n < radios; ++n) {
        const std::size_t s = static_cast<std::size_t>(n);
        const std::uint64_t dropped = Access::dropped(eng, s) - drop0[s];
        const std::uint64_t read = Access::read(eng, s) - read0[s];
        const std::uint64_t under = Access::underruns(eng, s) - under0[s];
        const bool faulted = Access::faulted(eng, s);
        const bool running = Access::running(eng, s);
        std::printf("  radio %d: read %llu samples (%.3f MS/s), dropped %llu, audio underruns %llu, %s%s\n", n + 1, static_cast<unsigned long long>(read),
                    static_cast<double>(read) / wallTotal / 1e6, static_cast<unsigned long long>(dropped), static_cast<unsigned long long>(under),
                    running ? "running" : "NOT RUNNING", faulted ? ", FAULTED" : "");
        std::printf("           audio produced %.0f frames/s; %s\n", static_cast<double>(Access::produced(eng, s) - prod0[s]) / wallTotal, Access::sink(eng, s).c_str());
        std::printf("           audio recoveries %d, note: %s\n", Access::recoveries(eng, s), Access::note(eng, s).c_str());
        if (dropped != 0 || faulted || !running || read == 0) { ++bad; }
    }
    for (std::size_t k = 0; k < probeFiles.size(); ++k) {
        std::size_t lines = 0;
        {
            std::FILE* f = std::fopen((probeLogs / (probeFiles[k] + ".log")).string().c_str(), "rb");
            if (f != nullptr) {
                int ch;
                while ((ch = std::fgetc(f)) != EOF) { lines += ch == '\n' ? 1u : 0u; }
                std::fclose(f);
            }
        }
        const bool here = k < static_cast<std::size_t>(radios) && Access::runnerHas(eng, k, probeFiles[k]);
        std::printf("  decoder %zu (%s) on radio %zu: %s, %zu reports written\n", k + 1, probeFiles[k].c_str(), k + 1, here ? "instance live" : "NO INSTANCE", lines);
        if (!here || lines == 0) { ++bad; }
    }
    std::printf("RESULT %s: %d radios, average %.2f core(s) = %.2f%% of the machine, peak %.2f core(s), %zu threads\n", bad == 0 ? "OK" : "NOT OK", radios, avg,
                100.0 * avg / cores, peak, threads());
    api->close_session(session);
    eng.shutdown();
    return bad == 0 ? 0 : 1;
}
