// sdrplay_scm_restart.cpp - see sdrplay_scm_restart.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "source/sdrplay_scm_restart.hpp"

#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace cascade::source {

namespace {

SdrPlayScmOutcome make(SdrPlayScmResult r, unsigned long e, const std::string& s) {
    SdrPlayScmOutcome o;
    o.result = r;
    o.error = e;
    o.sentence = s;
    return o;
}

SdrPlayScmOutcome fromError(unsigned long e, const char* doing) {
    if (e == kScmErrorAccessDenied) {
        return make(SdrPlayScmResult::AccessDenied, e,
                    "Windows refused: restarting the SDRplay service needs administrator rights.");
    }
    if (e == kScmErrorServiceDoesNotExist) {
        return make(SdrPlayScmResult::NotInstalled, e, "The SDRplay API service is not installed.");
    }
    return make(SdrPlayScmResult::OtherError, e,
                std::string("Windows reported error ") + std::to_string(e) + " while " + doing + ".");
}

// Poll until `want` or the wait is spent. Returns 0 and sets `reached`, or a Windows error.
unsigned long waitFor(ScmInterface& scm, ScmState want, std::chrono::milliseconds limit,
                      std::chrono::milliseconds step, bool& reached) {
    reached = false;
    std::chrono::milliseconds spent{0};
    for (;;) {
        ScmState s = ScmState::Other;
        const unsigned long e = scm.queryState(s);
        if (e != 0) { return e; }
        if (s == want) { reached = true; return 0; }
        if (spent >= limit) { return 0; }
        scm.sleepFor(step);
        spent += step;
    }
}

}  // namespace

SdrPlayScmOutcome restartSdrPlayServiceVia(ScmInterface& scm, const std::string& serviceName,
                                           const SdrPlayScmLimits& limits) {
    try {
        unsigned long e = scm.open(serviceName);
        if (e != 0) { return fromError(e, "opening the service"); }
        ScmState s = ScmState::Other;
        e = scm.queryState(s);
        if (e != 0) { scm.close(); return fromError(e, "reading the service state"); }
        if (s != ScmState::Stopped) {
            if (s != ScmState::StopPending) {
                e = scm.requestStop();
                if (e != 0) { scm.close(); return fromError(e, "stopping the service"); }
            }
            bool stopped = false;
            e = waitFor(scm, ScmState::Stopped, limits.stopWait, limits.step, stopped);
            if (e != 0) { scm.close(); return fromError(e, "waiting for the service to stop"); }
            if (!stopped) {
                scm.close();
                return make(SdrPlayScmResult::StopTimedOut, 0, "The SDRplay service did not stop in time.");
            }
        }
        e = scm.requestStart();
        if (e != 0) { scm.close(); return fromError(e, "starting the service"); }
        bool running = false;
        e = waitFor(scm, ScmState::Running, limits.startWait, limits.step, running);
        scm.close();
        if (e != 0) { return fromError(e, "waiting for the service to start"); }
        if (!running) {
            return make(SdrPlayScmResult::StartTimedOut, 0, "The SDRplay service did not start in time.");
        }
        return make(SdrPlayScmResult::Restarted, 0, "The SDRplay service was restarted.");
    } catch (...) {
        scm.close();
        return make(SdrPlayScmResult::OtherError, 0, "The restart failed unexpectedly.");
    }
}

#if defined(_WIN32)

WindowsScm::WindowsScm() = default;
WindowsScm::~WindowsScm() { close(); }

unsigned long WindowsScm::open(const std::string& serviceName) {
    close();
    manager_ = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (manager_ == nullptr) { return ::GetLastError(); }
    const std::wstring w(serviceName.begin(), serviceName.end());  // service names are ASCII (callers check)
    service_ = ::OpenServiceW(static_cast<SC_HANDLE>(manager_), w.c_str(),
                              SERVICE_QUERY_STATUS | SERVICE_START | SERVICE_STOP);
    if (service_ == nullptr) {
        const unsigned long e = ::GetLastError();
        close();
        return e;
    }
    return 0;
}

unsigned long WindowsScm::queryState(ScmState& state) {
    SERVICE_STATUS_PROCESS p{};
    DWORD needed = 0;
    if (!::QueryServiceStatusEx(static_cast<SC_HANDLE>(service_), SC_STATUS_PROCESS_INFO,
                                reinterpret_cast<LPBYTE>(&p), sizeof(p), &needed)) {
        return ::GetLastError();
    }
    switch (p.dwCurrentState) {
        case SERVICE_STOPPED: state = ScmState::Stopped; break;
        case SERVICE_START_PENDING: state = ScmState::StartPending; break;
        case SERVICE_STOP_PENDING: state = ScmState::StopPending; break;
        case SERVICE_RUNNING: state = ScmState::Running; break;
        default: state = ScmState::Other; break;
    }
    return 0;
}

unsigned long WindowsScm::requestStop() {
    SERVICE_STATUS st{};
    return ::ControlService(static_cast<SC_HANDLE>(service_), SERVICE_CONTROL_STOP, &st)
               ? 0
               : ::GetLastError();
}

unsigned long WindowsScm::requestStart() {
    return ::StartServiceW(static_cast<SC_HANDLE>(service_), 0, nullptr) ? 0 : ::GetLastError();
}

void WindowsScm::close() {
    if (service_ != nullptr) {
        ::CloseServiceHandle(static_cast<SC_HANDLE>(service_));
        service_ = nullptr;
    }
    if (manager_ != nullptr) {
        ::CloseServiceHandle(static_cast<SC_HANDLE>(manager_));
        manager_ = nullptr;
    }
}

#else

WindowsScm::WindowsScm() = default;
WindowsScm::~WindowsScm() = default;
unsigned long WindowsScm::open(const std::string&) { return 50; }
unsigned long WindowsScm::queryState(ScmState&) { return 50; }
unsigned long WindowsScm::requestStop() { return 50; }
unsigned long WindowsScm::requestStart() { return 50; }
void WindowsScm::close() {}

#endif

void WindowsScm::sleepFor(std::chrono::milliseconds ms) { std::this_thread::sleep_for(ms); }

SdrPlayScmOutcome restartSdrPlayServiceNow(const std::string& serviceName) {
    WindowsScm scm;
    return restartSdrPlayServiceVia(scm, serviceName);
}

}  // namespace cascade::source
