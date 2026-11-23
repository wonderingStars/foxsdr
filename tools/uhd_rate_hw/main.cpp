// uhd_rate_hw - the real engine on a real Ettus B200 through SoapySDR, RECEIVE ONLY, to measure samples per second across live sample-rate changes.
//
// Field fault (22 Nov 2026): after the rate was changed on a live B200 stream (2.4, 1.0, 2.0 MS/s) the samples per second fell to zero and stayed there. This tool
// runs that sequence through the engine's own API (SELECT_SOURCE, SET_SAMPLE_RATE, SET_CENTRE, SET_ANTENNA) and prints the source's sample count per 10 s.
// Only 98.0 MHz, antenna TX/RX (the whip), at most 8 MS/s. Nothing is transmitted. The profile is a scratch folder, never the user's.
//
// Usage: uhd_rate_hw <first-phase seconds> <seconds per later phase> <rate in MS/s> [<rate> ...]      e.g.  uhd_rate_hw 120 300 2.4 1.0 2.0
// Exit 0 = every phase delivered samples in its last window; 1 = a phase ended with zero samples; 2 = no B200 enumerated.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "engine/engine.hpp"
#include "engine/fox_engine_api.hpp"
#include "engine/receiver.hpp"
#include "foxsdr_api.h"

namespace cascade::gui {
struct AppWindowTestAccess {
    using Engine = cascade::engine::Engine;
    static std::uint64_t read(Engine& e, std::size_t slot) { return e.receivers_[slot] ? e.receivers_[slot]->pipeline_.sourceSamplesRead() : 0; }
    static bool faulted(Engine& e, std::size_t slot) { return e.receivers_[slot] && e.receivers_[slot]->pipeline_.faulted(); }
    static std::string fault(Engine& e, std::size_t slot) { return e.receivers_[slot] ? e.receivers_[slot]->pipeline_.faultMessage() : std::string(); }
};
}  // namespace cascade::gui

namespace {
using Access = cascade::gui::AppWindowTestAccess;
namespace fs = std::filesystem;
const FoxEngineApi* g_api = nullptr;
FoxSession* g_session = nullptr;

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
int sendWait(FoxCommand c, std::string* message = nullptr) {
    FoxSubmitResult r{};
    r.structSize = sizeof(r);
    const int rc = g_api->submit(g_session, &c, 1, &r);
    if (rc < 0 || r.status != FOXAPI_OK) { return rc < 0 ? rc : r.status; }
    for (int i = 0; i < 1500; ++i) {
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
std::string lower(std::string s) {
    for (char& c : s) { c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}
}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 4) {
        std::printf("usage: uhd_rate_hw <first-phase seconds> <seconds per later phase> <MS/s> [<MS/s> ...]\n");
        return 64;
    }
    const int firstSec = std::max(10, std::atoi(argv[1]));
    const int laterSec = std::max(10, std::atoi(argv[2]));
    std::vector<double> rates;
    for (int i = 3; i < argc; ++i) {
        const double r = std::atof(argv[i]) * 1.0e6;
        if (!(r > 0.0) || r > 8.0e6) { std::printf("refused: %s MS/s is outside 0..8\n", argv[i]); return 64; }
        rates.push_back(r);
    }
    namespace fs = std::filesystem;
    if (std::getenv("SOAPY_SDR_ROOT") == nullptr) {
        for (const char* base : {"USERPROFILE", "LOCALAPPDATA"}) {
            const char* v = std::getenv(base);
            if (v == nullptr || *v == '\0') { continue; }
            const fs::path root = fs::path(v) / "radioconda" / "Library";
            std::error_code ec;
            if (fs::is_directory(root / "lib" / "SoapySDR", ec)) { _putenv_s("SOAPY_SDR_ROOT", root.string().c_str()); break; }
        }
    }
    const char* scratchEnv = std::getenv("UHD_RATE_HW_SCRATCH");
    const fs::path scratch = scratchEnv != nullptr ? fs::path(scratchEnv) : fs::temp_directory_path() / ("foxsdr_uhd_rate_hw_" + std::to_string(::GetCurrentProcessId()));
    std::error_code ec;
    fs::create_directories(scratch, ec);
    for (const char* v : {"LOCALAPPDATA", "APPDATA", "USERPROFILE", "HOME"}) { ::SetEnvironmentVariableA(v, scratch.string().c_str()); _putenv_s(v, scratch.string().c_str()); }
    for (const char* v : {"FOXSDR_TELEMETRY_URL", "FOXSDR_CRASH_URL", "FOXSDR_UPDATE_URL"}) { _putenv_s(v, "http://127.0.0.1:9"); }
    std::printf("scratch profile: %s\n", scratch.string().c_str());

    g_api = cascade::engine::engineApi(FOXAPI_VERSION_MAJOR, FOXAPI_VERSION_MINOR);
    auto eng = std::make_unique<cascade::engine::Engine>();
    eng->configureApi("token=hw");
    eng->initialise();
    eng->startControlThread();
    FoxSessionParams sp{};
    sp.structSize = sizeof(sp);
    sp.flags = FOXAPI_SESSION_LOCAL;
    sp.grants = FOXAPI_GRANT_VIEW | FOXAPI_GRANT_TUNE | FOXAPI_GRANT_SETTINGS | FOXAPI_GRANT_AUDIO;
    sp.clientName = "uhd_rate_hw";
    sp.clientVersion = "1";
    if (g_api == nullptr || g_api->open_session(eng->apiHandle(), &sp, &g_session) != FOXAPI_OK || g_session == nullptr) {
        std::printf("could not open an API session\n");
        return 2;
    }

    send(makeCmd(FOXAPI_OP_SCAN_DEVICES, 0.0, 0, 0));
    std::string b200;
    for (int i = 0; i < 120; ++i) {
        sleepMs(500);
        const FoxReceiverState st = state(1);
        const int n = g_api->read_list(g_session, FOXAPI_LIST_DEVICES, 0, nullptr);
        for (int k = 0; k < n; ++k) {
            FoxListItem it{};
            it.structSize = sizeof(it);
            if (g_api->read_list(g_session, FOXAPI_LIST_DEVICES, static_cast<std::uint32_t>(k), &it) <= k) { break; }
            const std::string low = lower(std::string(it.name) + " " + it.detail);
            if (i == 0 || b200.empty()) {
                if (std::string(it.detail).rfind("soapy:", 0) == 0 && (low.find("b200") != std::string::npos || low.find("uhd") != std::string::npos)) { b200 = it.detail; }
            }
        }
        if (i > 4 && (st.sourceFlags & FOXAPI_SRC_SCAN_PENDING) == 0) { break; }
    }
    std::printf("ETTUS SEARCH LISTS THE B200: %s\n", b200.empty() ? "NO" : "YES");
    if (!b200.empty()) { std::printf("row: %s\n", "(args withheld: serial)"); }
    if (b200.empty()) {
        g_api->close_session(g_session);
        eng->shutdown();
        return 2;
    }

    std::string why;
    const int rc = sendWait(makeCmd(FOXAPI_OP_SELECT_SOURCE, 0.0, 0, 1, b200.c_str()), &why);
    std::printf("select: status %d %s\n", rc, why.c_str());
    for (int i = 0; i < 300 && fact("radio.1.source") == "siggen"; ++i) { sleepMs(100); }
    send(makeCmd(FOXAPI_OP_SET_ANTENNA, 0.0, 0, 1, "TX/RX"));   // the whip, a RECEIVE port
    send(makeCmd(FOXAPI_OP_SET_MODE, 0.0, FOXAPI_DEMOD_WFM, 1));
    send(makeCmd(FOXAPI_OP_SET_VOLUME, 0.0, 0, 1));            // volume zero: the audio path runs, nothing plays
    send(makeCmd(FOXAPI_OP_SET_SAMPLE_RATE, rates[0], 0, 1));
    send(makeCmd(FOXAPI_OP_SET_CENTRE, 98.0e6, 0, 1));
    send(makeCmd(FOXAPI_OP_RUN, 0.0, 1, 1));
    sleepMs(3000);
    {
        const FoxReceiverState st = state(1);
        std::printf("radio: %s, rate %.0f S/s, running %d, fault '%s', antenna fact '%s'\n", st.deviceName, st.sampleRateHz, (st.flags & FOXAPI_RX_RUNNING) != 0u ? 1 : 0, st.faultMessage, fact("radio.1.antenna").c_str());
    }

    int stalledPhases = 0;
    for (std::size_t ph = 0; ph < rates.size(); ++ph) {
        if (ph > 0) {
            const int r = send(makeCmd(FOXAPI_OP_SET_SAMPLE_RATE, rates[ph], 0, 1));
            std::printf("--- rate change to %.0f S/s: status %d\n", rates[ph], r);
        }
        const int secs = ph == 0 ? firstSec : laterSec;
        std::uint64_t prev = Access::read(*eng, 0);
        double lastWindow = -1.0;
        for (int t = 10; t <= secs; t += 10) {
            sleepMs(10000);
            const std::uint64_t now = Access::read(*eng, 0);
            lastWindow = static_cast<double>(now - prev) / 10.0;
            prev = now;
            const FoxReceiverState st = state(1);
            std::printf("phase %zu rate %.0f t=%3d s  samples/s %.0f  faulted %d '%s'\n", ph, st.sampleRateHz, t, lastWindow, Access::faulted(*eng, 0) ? 1 : 0, Access::fault(*eng, 0).c_str());
        }
        if (lastWindow < 1.0) { ++stalledPhases; std::printf("PHASE %zu ENDED WITH ZERO SAMPLES\n", ph); }
    }
    send(makeCmd(FOXAPI_OP_RUN, 0.0, 0, 1));   // a graceful stop
    sleepMs(1500);
    g_api->close_session(g_session);
    eng->shutdown();
    std::printf("RESULT %s (phases ending at zero: %d)\n", stalledPhases == 0 ? "NO STALL" : "STALL", stalledPhases);
    return stalledPhases == 0 ? 0 : 1;
}
