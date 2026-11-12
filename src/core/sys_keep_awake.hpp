// sys_keep_awake.hpp - keep the PC awake while something that must not be interrupted runs (a recording, a scheduled capture): a reference-counted request.
//
// THE OS FACT THIS IS BUILT AROUND (docs: SetThreadExecutionState, learn.microsoft.com/windows/win32/api/winbase/nf-winbase-setthreadexecutionstate): the execution
// state belongs to the CALLING THREAD, ES_CONTINUOUS keeps it until the thread says otherwise or ends, and a call with ES_CONTINUOUS alone clears it. A guard that
// several threads share cannot therefore set it on one thread and clear it on another. This one owns a small HOLDER THREAD for as long as at least one lease is held:
// the first lease starts it, it sets the state, the last lease ends it, it clears the state and exits. Every set and clear for one period is made by that one thread,
// and the lease's destructor releases on every exit path (an exception, an early return); the guard's own destructor clears whatever is still held.
//
// INJECTABLE: the one OS call is a std::function (`Fns::setState`, returning the previous state, 0 for failure, as the OS call does), so the tests drive the whole
// counting and thread behaviour with a stand-in and never change a real power setting. realFns() is the real call; it is used on this machine only for the single
// read-back test (a few milliseconds, system-required only, never the display, released before the test ends; no power plan or setting is changed).
//
// NOT WIRED: the recorder and the schedule runner (core recorder, engine) are other lanes' files; the hand-off is in docs/feature-records/sysmodels.md.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_SYS_KEEP_AWAKE_HPP
#define CASCADE_CORE_SYS_KEEP_AWAKE_HPP

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace cascade::core::sys {

// The OS flag values (winbase.h), repeated so no Windows header is needed here.
inline constexpr unsigned long kEsSystemRequired = 0x00000001UL;
inline constexpr unsigned long kEsDisplayRequired = 0x00000002UL;
inline constexpr unsigned long kEsContinuous = 0x80000000UL;

class KeepAwake {
public:
    struct Fns {
        // SetThreadExecutionState: returns the previous state, or 0 when the call fails.
        std::function<unsigned long(unsigned long)> setState;
    };
    static Fns realFns();   // the real call (a no-op returning 0 where there is none)

    explicit KeepAwake(Fns fns, bool alsoKeepDisplayOn = false);
    KeepAwake();            // realFns()
    ~KeepAwake();
    KeepAwake(const KeepAwake&) = delete;
    KeepAwake& operator=(const KeepAwake&) = delete;

    // One hold. Movable, not copyable; releases in its destructor (or reset()), and a second release does nothing.
    class Lease {
    public:
        Lease() = default;
        Lease(Lease&& o) noexcept : owner_(o.owner_) { o.owner_ = nullptr; }
        Lease& operator=(Lease&& o) noexcept;
        ~Lease() { reset(); }
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        void reset();
        explicit operator bool() const { return owner_ != nullptr; }

    private:
        friend class KeepAwake;
        explicit Lease(KeepAwake* o) : owner_(o) {}
        KeepAwake* owner_ = nullptr;
    };

    // Takes a hold; when it is the first, the state is requested before this returns (so active() already tells whether the OS accepted it).
    Lease acquire();

    int holders() const;
    bool active() const;                     // the OS accepted the request and no clear has been made since
    std::uint64_t setCalls() const;          // requests made (ES_CONTINUOUS | ...)
    std::uint64_t clearCalls() const;        // clears made (ES_CONTINUOUS alone)

private:
    void release();
    void startHolder();                      // under mu_
    void stopHolder();                       // under mu_

    Fns fns_;
    unsigned long flags_;
    mutable std::mutex mu_;                  // serialises acquire / release (held across the holder thread's start and join)
    int holders_ = 0;
    std::thread holder_;

    mutable std::mutex hmu_;                         // the holder thread's own hand-shake
    std::condition_variable hcv_;
    bool stop_ = false;
    bool applied_ = false;                   // the holder has made its request (accepted or not)
    bool active_ = false;
    std::uint64_t sets_ = 0, clears_ = 0;
};

}  // namespace cascade::core::sys

#endif  // CASCADE_CORE_SYS_KEEP_AWAKE_HPP
