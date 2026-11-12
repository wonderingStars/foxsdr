// sys_keep_awake.cpp - see sys_keep_awake.hpp.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "core/sys_keep_awake.hpp"

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

KeepAwake::Fns KeepAwake::realFns() {
    Fns f;
#ifdef _WIN32
    f.setState = [](unsigned long flags) -> unsigned long { return static_cast<unsigned long>(::SetThreadExecutionState(static_cast<EXECUTION_STATE>(flags))); };
#else
    f.setState = [](unsigned long) -> unsigned long { return 0; };
#endif
    return f;
}

KeepAwake::KeepAwake(Fns fns, bool alsoKeepDisplayOn)
    : fns_(std::move(fns)), flags_(kEsContinuous | kEsSystemRequired | (alsoKeepDisplayOn ? kEsDisplayRequired : 0UL)) {}

KeepAwake::KeepAwake() : KeepAwake(realFns(), false) {}

KeepAwake::~KeepAwake() {
    std::lock_guard<std::mutex> lk(mu_);
    if (holders_ > 0) {
        holders_ = 0;
        stopHolder();
    }
}

KeepAwake::Lease& KeepAwake::Lease::operator=(Lease&& o) noexcept {
    if (this != &o) {
        reset();
        owner_ = o.owner_;
        o.owner_ = nullptr;
    }
    return *this;
}

void KeepAwake::Lease::reset() {
    if (owner_ != nullptr) {
        KeepAwake* o = owner_;
        owner_ = nullptr;
        o->release();
    }
}

KeepAwake::Lease KeepAwake::acquire() {
    std::lock_guard<std::mutex> lk(mu_);
    if (++holders_ == 1) { startHolder(); }
    return Lease(this);
}

void KeepAwake::release() {
    std::lock_guard<std::mutex> lk(mu_);
    if (holders_ <= 0) { return; }
    if (--holders_ == 0) { stopHolder(); }
}

void KeepAwake::startHolder() {
    {
        std::lock_guard<std::mutex> h(hmu_);
        stop_ = false;
        applied_ = false;
        active_ = false;
    }
    holder_ = std::thread([this] {
        const unsigned long prev = fns_.setState ? fns_.setState(flags_) : 0UL;
        {
            std::lock_guard<std::mutex> h(hmu_);
            ++sets_;
            active_ = (prev != 0);
            applied_ = true;
        }
        hcv_.notify_all();
        {
            std::unique_lock<std::mutex> h(hmu_);
            hcv_.wait(h, [this] { return stop_; });
        }
        // Cleared by the thread that set it (the state is the thread's own).
        if (fns_.setState) { fns_.setState(kEsContinuous); }
        {
            std::lock_guard<std::mutex> h(hmu_);
            ++clears_;
            active_ = false;
        }
    });
    std::unique_lock<std::mutex> h(hmu_);
    hcv_.wait(h, [this] { return applied_; });
}

void KeepAwake::stopHolder() {
    {
        std::lock_guard<std::mutex> h(hmu_);
        stop_ = true;
    }
    hcv_.notify_all();
    if (holder_.joinable()) { holder_.join(); }
}

int KeepAwake::holders() const {
    std::lock_guard<std::mutex> lk(mu_);
    return holders_;
}

bool KeepAwake::active() const {
    std::lock_guard<std::mutex> h(hmu_);
    return active_;
}

std::uint64_t KeepAwake::setCalls() const { std::lock_guard<std::mutex> h(hmu_); return sets_; }
std::uint64_t KeepAwake::clearCalls() const { std::lock_guard<std::mutex> h(hmu_); return clears_; }

}  // namespace cascade::core::sys
