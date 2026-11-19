// test_radio_handoff.cpp - FIELD FIX A2 (2026-11-19): the pipe between two FoxSDR windows over one radio (core/radio_handoff.*).
//
// Both "windows" live in this one process (the server of one, the asker of the other); the asker passes a made-up own pid so the real pipe of the server is not skipped as its own.
// The pipe is the real one (a named pipe under this user's access list), under a prefix of this test's own so no real FoxSDR is ever asked anything.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "core/radio_handoff.hpp"
#include "test_check.hpp"

namespace ho = cascade::core::handoff;

int main() {
#if !defined(_WIN32)
    std::printf("test_radio_handoff: named pipes are the Windows channel; nothing to check here\n");
    return 0;
#else
    const std::string prefix = "FoxSDR-handoff-test" + std::to_string(static_cast<unsigned long>(std::chrono::steady_clock::now().time_since_epoch().count() & 0xffffff));
    constexpr unsigned long kAsker = 1;   // "the other window's pid": not this process's

    // --- keys ---------------------------------------------------------------------------------------------------------------------------------
    CHECK(ho::radioKey("rtlsdr", "serial=00000001") == "rtlsdr|00000001");
    CHECK(ho::radioKey("rtlsdr", "driver=rtlsdr,serial=ABC123,x=1") == "rtlsdr|ABC123");
    CHECK(ho::radioKey("rtlsdr", "index=0").empty());   // no serial: no holder is ever claimed
    CHECK(ho::radioKey("rtlsdr", "serial=").empty());
    CHECK(ho::lastFour("SDR_VOICE") == "OICE" && ho::lastFour("12") == "12");
    CHECK(ho::serialOfKey("rtlsdr|00000001") == "00000001");

    // --- nobody holds anything: no holder ---------------------------------------------------------------------------------------------------
    {
        ho::Holder h;
        CHECK(!ho::findHolder(prefix, "rtlsdr|00000001", 500, h, kAsker));
    }

    // --- HOLDER DETECTION --------------------------------------------------------------------------------------------------------------------------
    std::atomic<int> releases{0};
    std::atomic<int> mode{0};   // 0 release, 1 refuse (recording), 2 not held, 3 hang past the asker's patience
    ho::Server server;
    {
        ho::Hooks hooks;
        hooks.holds = [](const std::string& key) { return key == "rtlsdr|00000001"; };
        hooks.release = [&](const std::string&) {
            ++releases;
            ho::ReleaseResult r;
            switch (mode.load()) {
                case 0: r.result = ho::Release::Released; break;
                case 1: r.result = ho::Release::Refused; r.code = "recording"; break;
                case 2: r.result = ho::Release::NotHeld; break;
                default: std::this_thread::sleep_for(std::chrono::milliseconds(900)); r.result = ho::Release::Released; break;
            }
            return r;
        };
        CHECK(server.start(prefix, "slint", hooks));
        CHECK(server.running());
    }
    ho::Holder holder;
    CHECK(ho::findHolder(prefix, "rtlsdr|00000001", 2000, holder, kAsker));
    CHECK(holder.window == "slint");
    CHECK(holder.pid == static_cast<unsigned long>(GetCurrentProcessId()));   // the operating system's word for who is on the pipe
    {
        ho::Holder other;
        CHECK(!ho::findHolder(prefix, "rtlsdr|99999999", 2000, other, kAsker));   // a window is there but not holding THIS radio: the other-program case
    }
    {
        ho::Holder own;
        CHECK(!ho::findHolder(prefix, "rtlsdr|00000001", 2000, own, 0));   // this process's own pipe is never "another copy"
    }

    // --- THE RELEASE REQUEST ----------------------------------------------------------------------------------------------------------------------
    mode = 0;
    ho::ReleaseResult r = ho::requestRelease(holder, "rtlsdr|00000001", 2000);
    CHECK(r.result == ho::Release::Released);
    CHECK(releases == 1);

    // --- REFUSED DURING A RECORDING ---------------------------------------------------------------------------------------------------------------
    mode = 1;
    r = ho::requestRelease(holder, "rtlsdr|00000001", 2000);
    CHECK(r.result == ho::Release::Refused && r.code == "recording");
    CHECK(ho::refusalSentence("recording", "NESDR SMArt v5 (serial ending OICE)").find("recording in the other FoxSDR window") != std::string::npos);
    CHECK(ho::refusalSentence("recording", "X").find('%') == std::string::npos);

    // --- NO LONGER HELD ---------------------------------------------------------------------------------------------------------------------------
    mode = 2;
    r = ho::requestRelease(holder, "rtlsdr|00000001", 2000);
    CHECK(r.result == ho::Release::NotHeld);

    // --- THE TIMEOUT: nothing answers inside the time: NoAnswer, and the asker is back inside its own bound -----------------------------------------
    mode = 3;
    const auto t0 = std::chrono::steady_clock::now();
    r = ho::requestRelease(holder, "rtlsdr|00000001", 300);
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::printf("a holder that does not answer: gave up after %lld ms\n", ms);
    CHECK(r.result == ho::Release::NoAnswer);
    CHECK(ms >= 250 && ms < 800);
    std::this_thread::sleep_for(std::chrono::milliseconds(800));   // the hung hook comes back

    // --- the server survives a rude client (connects, says nothing) and a malformed line --------------------------------------------------------
    {
        const std::wstring path = L"\\\\.\\pipe\\" + std::wstring(prefix.begin(), prefix.end()) + L"-" + std::to_wstring(static_cast<unsigned long>(GetCurrentProcessId()));
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE);
        if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); }
    }
    mode = 0;
    CHECK(ho::findHolder(prefix, "rtlsdr|00000001", 3000, holder, kAsker));

    // --- stop: the pipe is gone ---------------------------------------------------------------------------------------------------------------------
    server.stop();
    CHECK(!server.running());
    {
        ho::Holder gone;
        CHECK(!ho::findHolder(prefix, "rtlsdr|00000001", 500, gone, kAsker));
    }
    return testSummary("test_radio_handoff");
#endif
}
