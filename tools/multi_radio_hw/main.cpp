// multi_radio_hw - STAGE 7 OF THE MULTI-RADIO ENGINE, REAL HARDWARE (docs/multi-radio-engine-design.md section 10; docs/multi-radio-records/engine-stage-7.md).
//
// Runs the REAL engine (control thread, API host, the app's own device scan) on whatever radios this machine enumerates, in a scratch profile (never the real config.json):
//   1  scans the devices (SCAN_DEVICES) and prints every row;
//   2  puts a USRP B200 (a SoapySDR row) on radio 1 and an RTL-SDR (a native row) on radio 2 - or, if only one of them enumerates, that one on radio 1 and says which was missing;
//   3  sweeps the FM broadcast band on each radio and tunes each to a real station (the strongest peak it can see), WFM, both radios at VOLUME ZERO (the audio path runs, nothing plays);
//   4  runs for the time asked and every minute prints, per radio: samples read, ring drops, audio underruns, faults, device restarts, signal level, the stereo pilot and the RDS name,
//      and the process's CPU and memory;
//   5  then the no-hands fault checks: stop radio 2 while radio 1 runs; remove radio 2 and add it again; close the engine with both running (how long the close takes) and look for a
//      crash report in the scratch profile.
//
// Usage: multi_radio_hw [seconds of the steady run, default 1800] [report every n seconds, default 60]
// Exit 0 = every radio read samples, none dropped, none faulted, no restarts; 1 = something to read in the output; 2 = no radio at all.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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

namespace cascade::gui {
struct AppWindowTestAccess {
    using Engine = cascade::engine::Engine;
    static std::uint64_t dropped(Engine& e, std::size_t slot) { return e.receivers_[slot] ? e.receivers_[slot]->pipeline_.ringDroppedSamples() : 0; }
    static std::uint64_t read(Engine& e, std::size_t slot) { return e.receivers_[slot] ? e.receivers_[slot]->pipeline_.sourceSamplesRead() : 0; }
    static std::uint64_t underruns(Engine& e, std::size_t slot) { return e.receivers_[slot] ? e.receivers_[slot]->pipeline_.audio().underruns() : 0; }
    static bool faulted(Engine& e, std::size_t slot) { return e.receivers_[slot] && e.receivers_[slot]->pipeline_.faulted(); }
    static std::uint64_t sourceGen(Engine& e, std::size_t slot) { return e.receivers_[slot] ? e.receivers_[slot]->sourceGen_.load() : 0; }
    static int recoveries(Engine& e, std::size_t slot) { return e.receivers_[slot] ? e.receivers_[slot]->audioRecoveries_ : -1; }
    static bool running(Engine& e, std::size_t slot) { return e.receivers_[slot] && e.receivers_[slot]->pipeline_.running(); }
    static std::string deviceRange(Engine& e, std::size_t slot) {
        if (!e.receivers_[slot] || e.receivers_[slot]->device_ == nullptr) { return "(no device)"; }
        double lo = 0.0, hi = -1.0;
        const bool ok = e.receivers_[slot]->device_->frequencyRangeHz(lo, hi);
        char b[96];
        std::snprintf(b, sizeof(b), "%s %.0f,%.0f", ok ? "reported" : "NOT reported", lo, hi);
        return b;
    }
    static std::string fault(Engine& e, std::size_t slot) { return e.receivers_[slot] ? e.receivers_[slot]->pipeline_.faultMessage() : std::string(); }
};
}  // namespace cascade::gui

namespace {

using Access = cascade::gui::AppWindowTestAccess;
using Engine = cascade::engine::Engine;
namespace fs = std::filesystem;

const FoxEngineApi* g_api = nullptr;
FoxSession* g_session = nullptr;

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
double workingSetMiB() {
    PROCESS_MEMORY_COUNTERS pmc{};
    ::GetProcessMemoryInfo(::GetCurrentProcess(), &pmc, sizeof(pmc));
    return static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
}
double privateMiB() {
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    ::GetProcessMemoryInfo(::GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
    return static_cast<double>(pmc.PrivateUsage) / (1024.0 * 1024.0);
}

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

FoxCommand makeCmd(std::uint32_t op, double n0, long long i0, std::uint32_t target, const char* text = nullptr) {
    FoxCommand c{};
    c.structSize = sizeof(c);
    c.op = op;
    c.num[0] = n0;
    c.ival[0] = i0;
    c.target = target;
    if (text != nullptr) { std::snprintf(c.text, sizeof(c.text), "%s", text); }
    return c;
}
int send(FoxCommand c) {
    FoxSubmitResult r{};
    r.structSize = sizeof(r);
    const int rc = g_api->submit(g_session, &c, 1, &r);
    return rc < 0 ? rc : r.status;
}
// Sends and waits for the engine's answer.
int sendWait(FoxCommand c, std::string* message = nullptr) {
    FoxSubmitResult r{};
    r.structSize = sizeof(r);
    const int rc = g_api->submit(g_session, &c, 1, &r);
    if (rc < 0 || r.status != FOXAPI_OK) { return rc < 0 ? rc : r.status; }
    for (int i = 0; i < 400; ++i) {
        FoxCommandResult res[8];
        for (auto& x : res) { x.structSize = sizeof(x); }
        const int n = g_api->poll_results(g_session, res, 8);
        for (int k = 0; k < n; ++k) {
            if (res[k].ticket == r.ticket) {
                if (message != nullptr) { *message = res[k].message; }
                return res[k].status;
            }
        }
        sleepMs(10);
    }
    return FOXAPI_FAILED;
}
FoxReceiverState state(std::uint32_t radio) {
    FoxReceiverState st{};
    st.structSize = sizeof(st);
    (void)g_api->read_radio_state(g_session, radio, &st);
    return st;
}
std::string fact(const std::string& key) {
    char buf[8192] = {};
    const int n = g_api->get_setting(g_session, ("fact." + key).c_str(), buf, sizeof(buf));
    return n >= 0 ? std::string(buf) : std::string("<absent>");
}

struct Row {
    std::string name, id;
};
std::vector<Row> deviceRows() {
    std::vector<Row> out;
    const int n = g_api->read_list(g_session, FOXAPI_LIST_DEVICES, 0, nullptr);
    for (int i = 0; i < n; ++i) {
        FoxListItem it{};
        it.structSize = sizeof(it);
        if (g_api->read_list(g_session, FOXAPI_LIST_DEVICES, static_cast<std::uint32_t>(i), &it) <= i) { break; }
        out.push_back({it.name, it.detail});
    }
    return out;
}
std::string lower(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}

struct Peak {
    double hz = 0.0;
    double standoutDb = 0.0;
};
// The strongest narrow peak of a spectrum: its frequency and how far it stands above the median.
Peak peakOf(std::uint32_t radio) {
    std::vector<float> bins(4096);
    FoxSpectrumInfo info{};
    info.structSize = sizeof(info);
    if (g_api->read_radio_spectrum(g_session, radio, 0, &info, bins.data(), static_cast<std::uint32_t>(bins.size())) != FOXAPI_OK) { return {}; }
    bins.resize(info.binCount);
    std::vector<float> sorted = bins;
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2), sorted.end());
    const float median = sorted[sorted.size() / 2];
    // ignore the DC spike: the few bins around the centre
    const std::size_t mid = bins.size() / 2;
    std::size_t best = 0;
    float bestv = -1.0e9f;
    for (std::size_t i = 0; i < bins.size(); ++i) {
        if (i + 8 > mid && i < mid + 8) { continue; }
        if (bins[i] > bestv) {
            bestv = bins[i];
            best = i;
        }
    }
    Peak p;
    p.hz = info.centreHz + (static_cast<double>(best) / static_cast<double>(bins.size()) - 0.5) * info.spanHz;
    p.standoutDb = static_cast<double>(bestv - median);
    return p;
}

// Sweeps 87.5 to 108 MHz with the radio's own width and returns the peaks, strongest first.
std::vector<Peak> sweepFm(std::uint32_t radio) {
    std::vector<Peak> found;
    const double rate = std::max(1.0e6, state(radio).sampleRateHz);
    const double step = rate * 0.7;
    for (double centre = 87.5e6 + rate * 0.35; centre < 108.0e6 + rate * 0.35; centre += step) {
        send(makeCmd(FOXAPI_OP_SET_CENTRE, centre, 0, radio));
        sleepMs(900);
        for (int k = 0; k < 2; ++k) {
            const Peak p = peakOf(radio);
            if (p.standoutDb > 15.0 && p.hz > 87.4e6 && p.hz < 108.1e6) {
                bool dup = false;
                for (Peak& q : found) {
                    if (std::fabs(q.hz - p.hz) < 150e3) {
                        q.standoutDb = std::max(q.standoutDb, p.standoutDb);
                        dup = true;
                    }
                }
                if (!dup) { found.push_back(p); }
            }
            sleepMs(200);
        }
    }
    std::sort(found.begin(), found.end(), [](const Peak& a, const Peak& b) { return a.standoutDb > b.standoutDb; });
    return found;
}

void tuneStation(std::uint32_t radio, double stationHz) {
    // The centre 250 kHz below the station so that the carrier is not on the DC spike, the channel on the station.
    send(makeCmd(FOXAPI_OP_SET_CENTRE, stationHz - 250.0e3, 0, radio));
    send(makeCmd(FOXAPI_OP_SET_MODE, 0.0, FOXAPI_DEMOD_WFM, radio));
    send(makeCmd(FOXAPI_OP_SET_VFO_OFFSET, 250.0e3, 0, radio));
    send(makeCmd(FOXAPI_OP_SET_VOLUME, 0.0, 0, radio));
}

struct Snap {
    std::uint64_t read = 0, dropped = 0, under = 0, gen = 0;
    int recov = 0;
};
Snap snap(Engine& e, std::size_t slot) {
    return {Access::read(e, slot), Access::dropped(e, slot), Access::underruns(e, slot), Access::sourceGen(e, slot), Access::recoveries(e, slot)};
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const int seconds = argc > 1 ? std::max(10, std::atoi(argv[1])) : 1800;
    const int every = argc > 2 ? std::max(5, std::atoi(argv[2])) : 60;
    unsigned cores = std::thread::hardware_concurrency();
    if (cores == 0) { cores = 1; }

    // The SoapySDR vendor modules of the user's radioconda are looked for under the USER'S folders; a redirected profile loses them, so the root is named BEFORE the redirect.
    if (std::getenv("SOAPY_SDR_ROOT") == nullptr) {
        for (const char* base : {"USERPROFILE", "LOCALAPPDATA"}) {
            const char* v = std::getenv(base);
            if (v == nullptr || *v == '\0') { continue; }
            const fs::path root = fs::path(v) / "radioconda" / "Library";
            std::error_code ec;
            if (fs::is_directory(root / "lib" / "SoapySDR", ec)) {
                _putenv_s("SOAPY_SDR_ROOT", root.string().c_str());
                std::printf("SOAPY_SDR_ROOT=%s\n", root.string().c_str());
                break;
            }
        }
    }
    const char* uhd = std::getenv("UHD_IMAGES_DIR");
    std::printf("UHD_IMAGES_DIR=%s\n", uhd != nullptr ? uhd : "(not set)");
    const fs::path scratch = fs::temp_directory_path() / ("foxsdr_multi_radio_hw_" + std::to_string(::GetCurrentProcessId()));
    std::error_code ec;
    fs::create_directories(scratch, ec);
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME"}) { ::SetEnvironmentVariableA(v, scratch.string().c_str()); _putenv_s(v, scratch.string().c_str()); }
    for (const char* v : {"FOXSDR_TELEMETRY_URL", "FOXSDR_CRASH_URL", "FOXSDR_UPDATE_URL"}) { _putenv_s(v, "http://127.0.0.1:9"); }
    std::printf("scratch profile: %s\n", scratch.string().c_str());

    g_api = cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR);
    auto eng = std::make_unique<Engine>();
    eng->configureApi("token=hw");
    eng->initialise();
    eng->startControlThread();
    FoxSessionParams sp{};
    sp.structSize = sizeof(sp);
    sp.flags = FOXAPI_SESSION_LOCAL;
    sp.grants = FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS | FOXAPI_GRANT_AUDIO;
    sp.clientName = "multi_radio_hw";
    sp.clientVersion = "1";
    if (g_api == nullptr || g_api->open_session(eng->apiHandle(), &sp, &g_session) != FOXAPI_OK || g_session == nullptr) {
        std::printf("could not open an API session\n");
        return 2;
    }

    // 1 THE SCAN
    send(makeCmd(FOXAPI_OP_SCAN_DEVICES, 0.0, 0, 0));
    std::vector<Row> rows;
    for (int i = 0; i < 120; ++i) {
        sleepMs(500);
        const FoxReceiverState st = state(1);
        rows = deviceRows();
        if (i > 4 && (st.sourceFlags & (FOXAPI_SRC_SCAN_PENDING | FOXAPI_SRC_CARD_SCANNING)) == 0 && rows.size() > 3) { break; }
        if (i > 40 && (st.sourceFlags & FOXAPI_SRC_SCAN_PENDING) == 0) { break; }
    }
    std::printf("devices the app's own scan lists:\n");
    std::string b200, rtl;
    for (const Row& r : rows) {
        std::printf("  %-40s %s\n", r.name.c_str(), r.id.c_str());
        const std::string n = lower(r.name + " " + r.id);
        if (b200.empty() && (n.find("b200") != std::string::npos || n.find("b210") != std::string::npos || (n.find("uhd") != std::string::npos && r.id.rfind("soapy:", 0) == 0))) { b200 = r.id; }
        if (rtl.empty() && r.id.rfind("rtlsdr:", 0) == 0) { rtl = r.id; }
    }
    std::printf("USRP B200: %s\nRTL-SDR: %s\n", b200.empty() ? "NOT ENUMERATED" : b200.c_str(), rtl.empty() ? "NOT ENUMERATED" : rtl.c_str());
    std::vector<std::string> ids;
    if (!b200.empty()) { ids.push_back(b200); }
    // HW_ONLY=b200 or HW_ONLY=rtl runs the control: one radio alone.
    const char* only = std::getenv("HW_ONLY");
    if (only != nullptr && std::string(only) == "b200") { rtl.clear(); }
    if (!rtl.empty()) { ids.push_back(rtl); }
    if (ids.empty()) {
        std::printf("RESULT NO RADIO: neither device enumerates\n");
        g_api->close_session(g_session);
        eng->shutdown();
        return 2;
    }

    // 2 THE RADIOS
    {
        std::string why;
        int rc = sendWait(makeCmd(FOXAPI_OP_SELECT_SOURCE, 0.0, 0, 1, ids[0].c_str()), &why);
        std::printf("radio 1 <- %s: status %d %s\n", ids[0].c_str(), rc, why.c_str());
    }
    for (int i = 0; i < 300 && fact("radio.1.source") == "siggen"; ++i) { sleepMs(100); }
    std::printf("radio 1 source '%s', rate %.0f S/s, device '%s'\n", fact("radio.1.source").c_str(), state(1).sampleRateHz, state(1).deviceName);
    const bool two = ids.size() > 1;
    if (two) {
        FoxCommand add = makeCmd(FOXAPI_OP_RADIO_ADD, 0.0, 2, 0, ids[1].c_str());
        add.ival[1] = 1;
        std::string why;
        const int rc = sendWait(add, &why);
        std::printf("radio 2 <- %s: status %d %s\n", ids[1].c_str(), rc, why.c_str());
        for (int i = 0; i < 300 && fact("radio.2.source") == "siggen"; ++i) { sleepMs(100); }
        std::printf("radio 2 source '%s', rate %.0f S/s, device '%s', refusal '%s'\n", fact("radio.2.source").c_str(), state(2).sampleRateHz, state(2).deviceName, fact("radio.2.refusal").c_str());
    }
    const int radios = two ? 2 : 1;
    for (int n = 1; n <= radios; ++n) {
        send(makeCmd(FOXAPI_OP_SET_VOLUME, 0.0, 0, static_cast<std::uint32_t>(n)));
        send(makeCmd(FOXAPI_OP_SET_MODE, 0.0, FOXAPI_DEMOD_WFM, static_cast<std::uint32_t>(n)));
        send(makeCmd(FOXAPI_OP_RUN, 0.0, 1, static_cast<std::uint32_t>(n)));
    }
    sleepMs(3000);
    for (int n = 1; n <= radios; ++n) {
        const FoxReceiverState st = state(static_cast<std::uint32_t>(n));
        std::printf("radio %d tunable range (fact.radio.%d.range): %s\n", n, n, fact("radio." + std::to_string(n) + ".range").c_str());
        std::printf("radio %d: %s, rate %.0f S/s, running %d, fault '%s'\n", n, st.deviceName, st.sampleRateHz, (st.flags & FOXAPI_RX_RUNNING) != 0u ? 1 : 0, st.faultMessage);
    }

    // 3 THE STATIONS
    std::vector<double> station(3, 0.0);
    std::vector<std::vector<Peak>> peaks(3);
    for (int n = 1; n <= radios; ++n) {
        peaks[static_cast<std::size_t>(n)] = sweepFm(static_cast<std::uint32_t>(n));
        std::printf("radio %d sweep of 87.5 to 108 MHz: %zu peaks", n, peaks[static_cast<std::size_t>(n)].size());
        for (std::size_t i = 0; i < peaks[static_cast<std::size_t>(n)].size() && i < 5; ++i) {
            std::printf("  %.2f MHz (+%.0f dB)", peaks[static_cast<std::size_t>(n)][i].hz / 1e6, peaks[static_cast<std::size_t>(n)][i].standoutDb);
        }
        std::printf("\n");
    }
    // Each radio tries its peaks in order of strength - radio 2 first the ones that are not radio 1's station, so that the two radios hear two stations where there are two - and keeps the
    // first one that decodes RDS within twelve seconds (a real, steady, stereo broadcast); when none does, the strongest.
    for (int n = 1; n <= radios; ++n) {
        const auto& pk = peaks[static_cast<std::size_t>(n)];
        if (pk.empty()) { continue; }
        std::vector<double> order;
        for (const Peak& p : pk) {
            if (n == 2 && station[1] > 0.0 && std::fabs(p.hz - station[1]) < 400e3) { continue; }
            order.push_back(std::round(p.hz / 100e3) * 100e3);
        }
        if (n == 2 && station[1] > 0.0) { order.push_back(station[1]); }
        if (order.empty()) { order.push_back(std::round(pk[0].hz / 100e3) * 100e3); }
        station[static_cast<std::size_t>(n)] = order[0];
        for (std::size_t k = 0; k < order.size() && k < 6; ++k) {
            tuneStation(static_cast<std::uint32_t>(n), order[k]);
            bool rds = false;
            for (int w = 0; w < 24 && !rds; ++w) {
                sleepMs(500);
                rds = state(static_cast<std::uint32_t>(n)).rdsGroups > 3;
            }
            std::printf("radio %d tried %.1f MHz: signal %.1f dB, pilot %.2f, RDS groups %llu\n", n, order[k] / 1e6, state(static_cast<std::uint32_t>(n)).signalDb, state(static_cast<std::uint32_t>(n)).pilotLevel,
                        static_cast<unsigned long long>(state(static_cast<std::uint32_t>(n)).rdsGroups));
            if (rds) {
                station[static_cast<std::size_t>(n)] = order[k];
                break;
            }
        }
    }
    for (int n = 1; n <= radios; ++n) {
        if (station[static_cast<std::size_t>(n)] > 0.0) {
            tuneStation(static_cast<std::uint32_t>(n), station[static_cast<std::size_t>(n)]);
            std::printf("radio %d tuned to %.1f MHz WFM, volume 0\n", n, station[static_cast<std::size_t>(n)] / 1e6);
        } else {
            std::printf("radio %d: NO STATION FOUND in the FM band\n", n);
        }
    }
    sleepMs(4000);

    // 4 THE STEADY RUN
    std::vector<Snap> s0, last;
    for (int n = 0; n < radios; ++n) { s0.push_back(snap(*eng, static_cast<std::size_t>(n))); }
    last = s0;
    const double cpu0 = cpuSeconds();
    double lastCpu = cpu0;
    auto t0 = std::chrono::steady_clock::now();
    auto lastT = t0;
    double peakCpu = 0.0;
    std::printf("steady run: %d s, report every %d s, %u logical processors\n", seconds, every, cores);
    std::vector<double> sigSum(3, 0.0), pilotMax(3, 0.0), pilotSum(3, 0.0);
    std::vector<int> sigN(3, 0);
    std::vector<std::string> ps(3);
    std::vector<std::uint64_t> rdsGroups(3, 0);
    std::vector<int> faultSeen(3, 0);
    while (true) {
        for (int k = 0; k < every; ++k) {
            sleepMs(1000);
            for (int n = 1; n <= radios; ++n) {
                const FoxReceiverState st = state(static_cast<std::uint32_t>(n));
                sigSum[static_cast<std::size_t>(n)] += st.signalDb;
                ++sigN[static_cast<std::size_t>(n)];
                pilotMax[static_cast<std::size_t>(n)] = std::max(pilotMax[static_cast<std::size_t>(n)], st.pilotLevel);
                pilotSum[static_cast<std::size_t>(n)] += st.pilotLevel;
                if (st.rdsPs[0] != '\0') { ps[static_cast<std::size_t>(n)] = st.rdsPs; }
                rdsGroups[static_cast<std::size_t>(n)] = std::max<std::uint64_t>(rdsGroups[static_cast<std::size_t>(n)], st.rdsGroups);
                if (Access::faulted(*eng, static_cast<std::size_t>(n) - 1u)) { ++faultSeen[static_cast<std::size_t>(n)]; }
            }
            if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() >= seconds) { break; }
        }
        const auto now = std::chrono::steady_clock::now();
        const double wall = std::chrono::duration<double>(now - lastT).count();
        const double cpu = cpuSeconds();
        const double used = (cpu - lastCpu) / wall;
        peakCpu = std::max(peakCpu, used);
        std::printf("t=%4.0fs cpu %.2f core (%.2f%%), working set %.0f MiB, private %.0f MiB, %zu threads\n", std::chrono::duration<double>(now - t0).count(), used, 100.0 * used / cores,
                    workingSetMiB(), privateMiB(), threads());
        for (int n = 0; n < radios; ++n) {
            const Snap c = snap(*eng, static_cast<std::size_t>(n));
            const Snap& p = last[static_cast<std::size_t>(n)];
            const FoxReceiverState st = state(static_cast<std::uint32_t>(n) + 1u);
            std::printf("   radio %d: +%.2f MS read (%.3f MS/s), drops +%llu, underruns +%llu, source installs +%llu, audio recoveries +%d, signal %.1f dB, pilot %.2f, RDS '%s' (%llu groups)%s\n", n + 1,
                        static_cast<double>(c.read - p.read) / 1e6, static_cast<double>(c.read - p.read) / wall / 1e6, static_cast<unsigned long long>(c.dropped - p.dropped),
                        static_cast<unsigned long long>(c.under - p.under), static_cast<unsigned long long>(c.gen - p.gen), c.recov - p.recov, st.signalDb, st.pilotLevel,
                        ps[static_cast<std::size_t>(n) + 1].c_str(), static_cast<unsigned long long>(rdsGroups[static_cast<std::size_t>(n) + 1]),
                        Access::faulted(*eng, static_cast<std::size_t>(n)) ? "  FAULTED" : "");
            last[static_cast<std::size_t>(n)] = c;
        }
        lastCpu = cpu;
        lastT = now;
        if (std::chrono::duration<double>(now - t0).count() >= seconds) { break; }
    }
    const double wallTotal = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double avgCpu = (cpuSeconds() - cpu0) / wallTotal;
    int bad = 0;
    std::printf("---- steady run over %.0f s ----\n", wallTotal);
    for (int n = 0; n < radios; ++n) {
        const Snap c = snap(*eng, static_cast<std::size_t>(n));
        const Snap& b = s0[static_cast<std::size_t>(n)];
        const std::size_t i = static_cast<std::size_t>(n) + 1;
        const FoxReceiverState st = state(static_cast<std::uint32_t>(n) + 1u);
        const bool faulted = Access::faulted(*eng, static_cast<std::size_t>(n)) || faultSeen[i] != 0;
        std::printf("radio %d (%s): read %.1f MS (%.3f MS/s), drops %llu, underruns %llu, device faults (seconds seen faulted) %d, source installs (restarts) %llu, audio recoveries %d\n", n + 1,
                    st.deviceName, static_cast<double>(c.read - b.read) / 1e6, static_cast<double>(c.read - b.read) / wallTotal / 1e6, static_cast<unsigned long long>(c.dropped - b.dropped),
                    static_cast<unsigned long long>(c.under - b.under), faultSeen[i], static_cast<unsigned long long>(c.gen - b.gen), c.recov - b.recov);
        std::printf("          station %.1f MHz: mean signal %.1f dB, stereo pilot mean %.2f (max %.2f), RDS name '%s' (%llu groups decoded)\n", station[i] / 1e6, sigN[i] != 0 ? sigSum[i] / sigN[i] : 0.0,
                    sigN[i] != 0 ? pilotSum[i] / sigN[i] : 0.0, pilotMax[i], ps[i].c_str(), static_cast<unsigned long long>(rdsGroups[i]));
        if (c.dropped != b.dropped || faulted || c.read == b.read || c.gen != b.gen) { ++bad; }
    }
    for (int n = 1; n <= radios; ++n) { std::printf("radio %d device->frequencyRangeHz(): %s\n", n, Access::deviceRange(*eng, static_cast<std::size_t>(n) - 1u).c_str()); }
    for (int n = 1; n <= radios; ++n) { std::printf("radio %d tunable range (fact.radio.%d.range) after the run: %s\n", n, n, fact("radio." + std::to_string(n) + ".range").c_str()); }
    std::printf("process: average %.2f core(s) = %.2f%% of the machine, peak ten-second-class window %.2f core(s), working set %.0f MiB, private %.0f MiB, %zu threads\n", avgCpu,
                100.0 * avgCpu / cores, peakCpu, workingSetMiB(), privateMiB(), threads());

    // 5 THE FAULT CHECKS
    if (two) {
        std::printf("---- fault checks ----\n");
        const Snap r1a = snap(*eng, 0);
        const Snap r2a = snap(*eng, 1);
        send(makeCmd(FOXAPI_OP_RUN, 0.0, 0, 2));
        sleepMs(20000);
        const Snap r1b = snap(*eng, 0);
        const Snap r2b = snap(*eng, 1);
        std::printf("radio 2 STOPPED for 20 s: radio 1 read +%.2f MS (drops +%llu, underruns +%llu, installs +%llu), radio 2 read +%llu samples\n", static_cast<double>(r1b.read - r1a.read) / 1e6,
                    static_cast<unsigned long long>(r1b.dropped - r1a.dropped), static_cast<unsigned long long>(r1b.under - r1a.under), static_cast<unsigned long long>(r1b.gen - r1a.gen),
                    static_cast<unsigned long long>(r2b.read - r2a.read));
        if (r1b.read == r1a.read || r1b.dropped != r1a.dropped) { ++bad; }
        send(makeCmd(FOXAPI_OP_RUN, 0.0, 1, 2));
        sleepMs(10000);
        const Snap r2c = snap(*eng, 1);
        // ...and the other way round: the B200 stopped while the RTL-SDR runs
        const Snap r1s = snap(*eng, 0);
        const Snap r2s = snap(*eng, 1);
        send(makeCmd(FOXAPI_OP_RUN, 0.0, 0, 1));
        sleepMs(20000);
        const Snap r1t = snap(*eng, 0);
        const Snap r2t = snap(*eng, 1);
        std::printf("radio 1 STOPPED for 20 s: radio 2 read +%.2f MS (drops +%llu, underruns +%llu, installs +%llu), radio 1 read +%llu samples\n", static_cast<double>(r2t.read - r2s.read) / 1e6,
                    static_cast<unsigned long long>(r2t.dropped - r2s.dropped), static_cast<unsigned long long>(r2t.under - r2s.under), static_cast<unsigned long long>(r2t.gen - r2s.gen),
                    static_cast<unsigned long long>(r1t.read - r1s.read));
        if (r2t.read == r2s.read || r2t.dropped != r2s.dropped) { ++bad; }
        send(makeCmd(FOXAPI_OP_RUN, 0.0, 1, 1));
        sleepMs(10000);
        const Snap r1u = snap(*eng, 0);
        std::printf("radio 1 STARTED again: read +%.2f MS in 10 s (running %d, fault '%s'), signal %.1f dB\n", static_cast<double>(r1u.read - r1t.read) / 1e6, Access::running(*eng, 0) ? 1 : 0,
                    Access::fault(*eng, 0).c_str(), state(1).signalDb);
        if (r1u.read == r1t.read) { ++bad; }
        std::printf("radio 2 STARTED again: read +%.2f MS in 10 s (running %d)\n", static_cast<double>(r2c.read - r2b.read) / 1e6, Access::running(*eng, 1) ? 1 : 0);
        if (r2c.read == r2b.read) { ++bad; }
        // remove and add again
        const auto tr = std::chrono::steady_clock::now();
        std::string why;
        int rc = sendWait(makeCmd(FOXAPI_OP_RADIO_REMOVE, 0.0, 2, 0), &why);
        std::printf("RADIO_REMOVE 2: status %d %s; radio.count %s\n", rc, why.c_str(), fact("radio.count").c_str());
        sleepMs(3000);
        const Snap r1c = snap(*eng, 0);
        FoxCommand add = makeCmd(FOXAPI_OP_RADIO_ADD, 0.0, 2, 0, ids[1].c_str());
        add.ival[1] = 1;
        rc = sendWait(add, &why);
        for (int i = 0; i < 300 && fact("radio.2.source") == "siggen"; ++i) { sleepMs(100); }
        const double reopenS = std::chrono::duration<double>(std::chrono::steady_clock::now() - tr).count();
        std::printf("RADIO_ADD 2 again: status %d %s; source '%s' after %.1f s since the remove; refusal '%s'\n", rc, why.c_str(), fact("radio.2.source").c_str(), reopenS, fact("radio.2.refusal").c_str());
        if (station[2] > 0.0) { tuneStation(2, station[2]); }
        send(makeCmd(FOXAPI_OP_SET_VOLUME, 0.0, 0, 2));
        sleepMs(10000);
        const Snap r2d = snap(*eng, 1);
        const Snap r1d = snap(*eng, 0);
        std::printf("after the re-add: radio 2 read %.2f MS in 10 s, signal %.1f dB; radio 1 read +%.2f MS since the remove, drops +%llu\n", static_cast<double>(r2d.read) / 1e6, state(2).signalDb,
                    static_cast<double>(r1d.read - r1c.read) / 1e6, static_cast<unsigned long long>(r1d.dropped - r1c.dropped));
        if (r2d.read == 0 || fact("radio.2.source") == "siggen" || r1d.dropped != r1c.dropped) { ++bad; }
    }
    // CLOSE WITH EVERY RADIO RUNNING
    std::printf("closing the engine with %d radio(s) running\n", radios);
    const auto tc = std::chrono::steady_clock::now();
    g_api->close_session(g_session);
    eng->shutdown();
    eng.reset();
    const double closeS = std::chrono::duration<double>(std::chrono::steady_clock::now() - tc).count();
    std::printf("close took %.2f s\n", closeS);
    int reports = 0;
    for (const auto& e : fs::recursive_directory_iterator(scratch, fs::directory_options::skip_permission_denied, ec)) {
        const std::string n = lower(e.path().filename().string());
        const std::string p = lower(e.path().string());
        if (e.is_regular_file(ec) && (n.find("crash") != std::string::npos || n.find(".dmp") != std::string::npos || p.find("\\reports\\") != std::string::npos || p.find("\\crash") != std::string::npos)) {
            std::printf("  report file: %s\n", e.path().string().c_str());
            ++reports;
        }
    }
    std::printf("crash/report files in the scratch profile: %d\n", reports);
    if (reports != 0) { ++bad; }
    std::printf("RESULT %s: %d radio(s), %d finding(s)\n", bad == 0 ? "OK" : "NOT OK", radios, bad);
    return bad == 0 ? 0 : 1;
}
