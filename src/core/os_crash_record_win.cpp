// The Windows half of os_crash_record.hpp: the Application event log, read through the
// documented Event Log API - EvtQuery, EvtNext, EvtRender - as the user the program
// runs as, with no privilege. Measured unelevated on Windows 11 22631 (the Application
// log is readable by ordinary users; it is the Security log that is not).
//
// WHAT THIS DOES, and nothing else: runs the one XPath it is given (this provider, this
// event id, a time window) against the "Application" channel, newest first, one event
// at a time, and hands each event's XML to the caller. It decides nothing about which
// event is the right one and keeps nothing: the caller's parser reads five named fields
// and the XML is dropped.
//
// EVERY FAILURE IS SILENCE. A query that cannot be made (no access, the log disabled or
// missing), an event that cannot be rendered, a buffer that would be too large: the
// function returns, and the caller finds nothing. Each EvtNext waits at most 100 ms, so
// the loop notices `cancel` promptly; the caller bounds the whole call by running it on
// a thread of its own.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/os_crash_record.hpp"

#if defined(_WIN32)

#include <windows.h>
#include <winevt.h>

#include <vector>

#pragma comment(lib, "wevtapi.lib")

namespace cascade::core {

namespace {

// An XPath is ASCII here (the text osCrashEventXPath builds), so widening is a copy.
std::wstring widenAscii(const std::string& s) {
    std::wstring w;
    w.reserve(s.size());
    for (const char c : s) { w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c))); }
    return w;
}

// The largest rendered event taken: a real Application Error event is about 2 KB.
constexpr DWORD kMaxEventBytes = 256 * 1024;

bool renderEventXml(EVT_HANDLE event, std::string& out) {
    std::vector<wchar_t> buf(4096);
    DWORD used = 0;
    DWORD props = 0;
    if (!::EvtRender(nullptr, event, EvtRenderEventXml,
                     static_cast<DWORD>(buf.size() * sizeof(wchar_t)), buf.data(), &used, &props)) {
        if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER || used == 0 || used > kMaxEventBytes) {
            return false;
        }
        buf.resize(used / sizeof(wchar_t) + 1);
        if (!::EvtRender(nullptr, event, EvtRenderEventXml,
                         static_cast<DWORD>(buf.size() * sizeof(wchar_t)), buf.data(), &used,
                         &props)) {
            return false;
        }
    }
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, buf.data(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) { return false; }
    out.assign(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, buf.data(), -1, out.data(), n, nullptr, nullptr);
    out.resize(static_cast<std::size_t>(n) - 1);  // the terminator
    return true;
}

}  // namespace

OsEventSource windowsApplicationLogSource() {
    return [](const std::string& xpath, const std::function<bool(const std::string&)>& each,
              const std::atomic<bool>& cancel) {
        const std::wstring query = widenAscii(xpath);
        EVT_HANDLE q = ::EvtQuery(nullptr, L"Application", query.c_str(),
                                  EvtQueryChannelPath | EvtQueryReverseDirection);
        if (q == nullptr) { return; }  // no access, no such log: not known
        // At most about ten seconds of 100 ms waits, whatever the caller does.
        for (int idle = 0; idle < 100 && !cancel.load();) {
            EVT_HANDLE ev = nullptr;
            DWORD got = 0;
            if (!::EvtNext(q, 1, &ev, 100, 0, &got)) {
                if (::GetLastError() == ERROR_TIMEOUT) {
                    ++idle;
                    continue;
                }
                break;  // ERROR_NO_MORE_ITEMS or any other: the end
            }
            if (got == 0 || ev == nullptr) { break; }
            idle = 0;
            std::string xml;
            const bool rendered = renderEventXml(ev, xml);
            ::EvtClose(ev);
            if (rendered && each(xml)) { break; }
        }
        ::EvtClose(q);
    };
}

}  // namespace cascade::core

#endif  // _WIN32
