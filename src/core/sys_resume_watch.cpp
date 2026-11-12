// sys_resume_watch.cpp - see sys_resume_watch.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/sys_resume_watch.hpp"

#include <chrono>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace cascade::core::sys {

PowerEvent powerEventFromPbt(unsigned long pbt) {
    switch (pbt) {
        case 4: return PowerEvent::Suspend;           // PBT_APMSUSPEND
        case 7: return PowerEvent::ResumeSuspend;     // PBT_APMRESUMESUSPEND
        case 18: return PowerEvent::ResumeAutomatic;  // PBT_APMRESUMEAUTOMATIC
        default: return PowerEvent::Other;
    }
}

void ResumeGate::onEvent(PowerEvent e, std::int64_t nowMs) {
    switch (e) {
        case PowerEvent::Suspend:
            pending_ = false;
            suspended_ = true;
            break;
        case PowerEvent::ResumeAutomatic:
        case PowerEvent::ResumeSuspend: {
            suspended_ = false;
            if (pending_) { break; }   // the first resume of a burst set the time; later ones add nothing
            std::int64_t at = nowMs + cfg_.settleMs;
            if (everFired_ && at < lastFire_ + cfg_.minIntervalMs) { at = lastFire_ + cfg_.minIntervalMs; }
            pending_ = true;
            dueAt_ = at;
            break;
        }
        case PowerEvent::Other:
            break;
    }
}

bool ResumeGate::due(std::int64_t nowMs) {
    if (!pending_ || suspended_ || nowMs < dueAt_) { return false; }
    pending_ = false;
    everFired_ = true;
    lastFire_ = nowMs;
    return true;
}

ResumeWatcher::ResumeWatcher(ResumeGateConfig cfg, std::function<void()> action, Clock clock)
    : gate_(cfg), action_(std::move(action)), clock_(std::move(clock)) {
    if (!clock_) {
        clock_ = [] {
            return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        };
    }
}

ResumeWatcher::~ResumeWatcher() { stop(); }

#ifdef _WIN32
namespace {
// winuser.h declares these only for a Windows 8 target; the values and layout are the documented ones (DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS, DEVICE_NOTIFY_CALLBACK = 2).
struct SubscribeParams {
    ULONG(CALLBACK* callback)(PVOID context, ULONG type, PVOID setting);
    PVOID context;
};
constexpr DWORD kNotifyCallback = 2;
using RegisterFn = HANDLE(WINAPI*)(HANDLE, DWORD);
using UnregisterFn = BOOL(WINAPI*)(HANDLE);
ULONG CALLBACK trampoline(PVOID ctx, ULONG type, PVOID setting) { return static_cast<ULONG>(cascade::core::sys::ResumeWatcherAccess::callback(ctx, type, setting)); }
}  // namespace
#endif

unsigned long ResumeWatcher::osCallback(void* context, unsigned long type, void*) {
    static_cast<ResumeWatcher*>(context)->inject(powerEventFromPbt(type));
    return 0;   // ERROR_SUCCESS
}

unsigned long ResumeWatcherAccess::callback(void* context, unsigned long type, void* setting) { return ResumeWatcher::osCallback(context, type, setting); }

bool ResumeWatcher::start(bool registerWithOs) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (started_) { return registered_.load() || !registerWithOs; }
        started_ = true;
        stopping_ = false;
    }
    thread_ = std::thread([this] { loop(); });
    if (!registerWithOs) { return true; }
#ifdef _WIN32
    // user32.dll, by name: the call exists from Windows 8 on and a machine without it just reports false.
    const HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    const auto reg = user32 ? reinterpret_cast<RegisterFn>(::GetProcAddress(user32, "RegisterSuspendResumeNotification")) : nullptr;
    if (reg != nullptr) {
        // The parameter block lives as long as the registration (the OS may read it again), and is freed after the unregister in stop().
        auto* params = new SubscribeParams{&trampoline, this};
        const HANDLE h = reg(params, kNotifyCallback);
        if (h != nullptr) {
            osHandle_ = h;
            osParams_ = params;
            registered_.store(true);
            return true;
        }
        delete params;
    }
#endif
    return false;
}

void ResumeWatcher::stop() {
#ifdef _WIN32
    if (osHandle_ != nullptr) {
        const HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
        const auto unreg = user32 ? reinterpret_cast<UnregisterFn>(::GetProcAddress(user32, "UnregisterSuspendResumeNotification")) : nullptr;
        if (unreg != nullptr) { unreg(static_cast<HANDLE>(osHandle_)); }
        osHandle_ = nullptr;
        delete static_cast<SubscribeParams*>(osParams_);
        osParams_ = nullptr;
    }
#endif
    registered_.store(false);
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!started_) { return; }
        stopping_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) { thread_.join(); }
    std::lock_guard<std::mutex> lk(mu_);
    started_ = false;
}

void ResumeWatcher::inject(PowerEvent e) {
    ++events_;
    {
        std::lock_guard<std::mutex> lk(mu_);
        gate_.onEvent(e, clock_());
    }
    cv_.notify_all();
}

void ResumeWatcher::loop() {
    std::unique_lock<std::mutex> lk(mu_);
    while (!stopping_) {
        const std::int64_t now = clock_();
        if (gate_.due(now)) {
            lk.unlock();
            if (action_) { action_(); }
            ++actions_;
            lk.lock();
            continue;
        }
        const std::int64_t next = gate_.nextDueMs();
        std::int64_t waitMs = (next < 0) ? 1000 : next - now;
        if (waitMs < 1) { waitMs = 1; }
        if (waitMs > 1000) { waitMs = 1000; }   // a clock a test steps by hand is re-read at least once a second
        cv_.wait_for(lk, std::chrono::milliseconds(waitMs));
    }
}

}  // namespace cascade::core::sys
