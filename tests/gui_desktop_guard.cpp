// gui_desktop_guard.cpp - the program behind the CTest fixture `gui_desktop` that every gui-labelled test requires (tests/gui_desktop_guard.hpp says why). It opens no window and no
// dialog: it reads the name of the desktop of its own thread (GetUserObjectInformation), prints one line, and exits 0 (allowed) or 1 (refused).
//
// FOXSDR_GUARD_DESKTOP_NAME replaces the name it reads: that is how tests/test_gui_desktop_guard.cpp drives both branches without a second desktop (it names no real desktop and
// changes no behaviour of the guard when unset). The override FOXSDR_ALLOW_VISIBLE_GUI_TESTS=1 is judged by gui_desktop_guard.hpp.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include <cstdio>
#include <cstdlib>
#include <string>

#include "gui_desktop_guard.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

std::string envValue(const char* name, bool& set) {
    set = false;
#ifdef _WIN32
    char* buf = nullptr;
    std::size_t n = 0;
    if (_dupenv_s(&buf, &n, name) != 0 || buf == nullptr) { return std::string(); }
    std::string v(buf);
    std::free(buf);
    set = true;
    return v;
#else
    const char* v = std::getenv(name);
    set = v != nullptr;
    return v != nullptr ? std::string(v) : std::string();
#endif
}

// The name of the desktop this thread is on, "" when it cannot be read.
std::string threadDesktopName() {
#ifdef _WIN32
    HDESK d = ::GetThreadDesktop(::GetCurrentThreadId());
    if (d == nullptr) { return std::string(); }
    char buf[256] = {};
    DWORD needed = 0;
    if (!::GetUserObjectInformationA(d, UOI_NAME, buf, sizeof buf, &needed)) { return std::string(); }
    return std::string(buf);
#else
    return std::string();
#endif
}

}  // namespace

int main() {
    bool nameSet = false, overrideSet = false;
    const std::string forced = envValue("FOXSDR_GUARD_DESKTOP_NAME", nameSet);
    const std::string over = envValue("FOXSDR_ALLOW_VISIBLE_GUI_TESTS", overrideSet);
    const std::string name = nameSet ? forced : threadDesktopName();
    const cascade::testguard::Verdict v = cascade::testguard::judge(name, overrideSet ? over.c_str() : nullptr);
    std::fputs((v.message + "\n").c_str(), v.allowed ? stdout : stderr);
    return v.allowed ? 0 : 1;
}
