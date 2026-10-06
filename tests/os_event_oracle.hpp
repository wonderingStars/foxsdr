// An INDEPENDENT witness for the tests of core/os_crash_record.hpp (Windows only).
//
// WHY IT EXISTS. The real-process tests need to tell two situations apart that look
// the same from the reader's side: "this machine wrote no Application Error event
// for the child I just killed" (Windows Error Reporting disabled by a policy, the
// Application log disabled or full - a SKIP, with the reason) and "Windows DID write
// it and the reader missed it" (a FAIL: the one thing these tests are for). So the
// witness looks for the event with the plainest possible code - the Event Log API,
// the provider and the event id, then a case-folded substring search of the rendered
// text for the process id - and shares nothing with the reader's query, window,
// matching or parsing.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_TESTS_OS_EVENT_ORACLE_HPP
#define CASCADE_TESTS_OS_EVENT_ORACLE_HPP

#if defined(_WIN32)

#include <windows.h>
#include <winevt.h>

#include <chrono>
#include <cstdio>
#include <cwctype>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "wevtapi.lib")

namespace oracle {

// Is there an Application Error event (id 1000) whose ProcessId is `pid`?
inline bool scanOnce(unsigned long pid) {
    wchar_t needle[96] = {};
    std::swprintf(needle, sizeof(needle) / sizeof(needle[0]), L"name='processid'>0x%lx<", pid);
    EVT_HANDLE q = ::EvtQuery(nullptr, L"Application",
                              L"*[System[Provider[@Name='Application Error'] and EventID=1000]]",
                              EvtQueryChannelPath | EvtQueryReverseDirection);
    if (q == nullptr) { return false; }
    bool found = false;
    for (int guard = 0; guard < 400 && !found; ++guard) {
        EVT_HANDLE ev[8] = {};
        DWORD got = 0;
        if (!::EvtNext(q, 8, ev, 1000, 0, &got) || got == 0) { break; }
        for (DWORD i = 0; i < got; ++i) {
            DWORD used = 0, props = 0;
            std::vector<wchar_t> buf(8192);
            if (!::EvtRender(nullptr, ev[i], EvtRenderEventXml,
                             static_cast<DWORD>(buf.size() * sizeof(wchar_t)), buf.data(), &used,
                             &props) &&
                ::GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
                buf.resize(used / sizeof(wchar_t) + 1);
                ::EvtRender(nullptr, ev[i], EvtRenderEventXml,
                            static_cast<DWORD>(buf.size() * sizeof(wchar_t)), buf.data(), &used,
                            &props);
            }
            std::wstring xml(buf.data());
            for (wchar_t& c : xml) { c = static_cast<wchar_t>(std::towlower(c)); }
            if (xml.find(needle) != std::wstring::npos) { found = true; }
            ::EvtClose(ev[i]);
        }
    }
    ::EvtClose(q);
    return found;
}

// Waits up to `timeoutMs` for the event to appear.
inline bool applicationErrorEventFor(unsigned long pid, unsigned timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        if (scanOnce(pid)) { return true; }
        if (std::chrono::steady_clock::now() >= deadline) { return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

}  // namespace oracle

#endif  // _WIN32

#endif  // CASCADE_TESTS_OS_EVENT_ORACLE_HPP
