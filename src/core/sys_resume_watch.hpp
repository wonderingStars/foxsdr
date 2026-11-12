// sys_resume_watch.hpp - "the PC woke up": a settle-delayed, rate-limited trigger for re-opening what a sleep may have broken (the radio, the audio device).
//
// WHY A GATE AND NOT A DIRECT CALL. Windows announces a resume more than once (PBT_APMRESUMEAUTOMATIC, then PBT_APMRESUMESUSPEND when a person touches the
// machine), a laptop can bounce suspend/resume several times in a minute, and a USB radio is not back on the bus the instant the resume event arrives. A direct
// "re-open now" on the first event reopens a device that is not there yet, and again on the second. The gate (pure, clock injected) collapses a burst into ONE action:
//   * a resume event schedules the action `settle` after it (the first schedule stands; later resumes in the burst add nothing);
//   * a suspend event cancels a pending action (the machine is going back to sleep) and holds the gate shut until the next resume;
//   * after the action fires, the next may not fire before `minInterval` has passed (an event inside it defers the action to the end of the interval, never drops it).
// ResumeWatcher is the operating-system wrapper: it registers for power-setting notifications by CALLBACK (RegisterSuspendResumeNotification, DEVICE_NOTIFY_CALLBACK:
// no window, so no message loop and no top-level-only broadcast to miss), feeds the gate, and runs the action on its own thread. Tests feed it the same events
// through inject(); the real registration is checked to succeed and be released, and no power state is ever changed.
//
// NOT WIRED: the engine is another lane's file (hand-off in the record). The action it would run is "close and re-open the current radio exactly as a Rescan + saved
// radio open does today"; this file never opens a radio.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_SYS_RESUME_WATCH_HPP
#define CASCADE_CORE_SYS_RESUME_WATCH_HPP

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace cascade::core::sys {

enum class PowerEvent { Suspend, ResumeAutomatic, ResumeSuspend, Other };

// The PBT_* value the OS reports -> PowerEvent (PBT_APMSUSPEND 4, PBT_APMRESUMESUSPEND 7, PBT_APMRESUMEAUTOMATIC 18).
PowerEvent powerEventFromPbt(unsigned long pbt);

struct ResumeGateConfig {
    std::int64_t settleMs = 4000;        // wait after a resume before acting (USB re-enumeration)
    std::int64_t minIntervalMs = 30000;  // at least this long between two actions
};

class ResumeGate {
public:
    explicit ResumeGate(ResumeGateConfig cfg = {}) : cfg_(cfg) {}
    void onEvent(PowerEvent e, std::int64_t nowMs);
    // True once when the action is due at `nowMs`; the gate then rate-limits itself. Not thread-safe: ResumeWatcher locks around it.
    bool due(std::int64_t nowMs);
    // When the action will next be due (ms on the injected clock), or -1 if nothing is pending.
    std::int64_t nextDueMs() const { return pending_ ? dueAt_ : -1; }
    bool suspended() const { return suspended_; }

private:
    ResumeGateConfig cfg_;
    bool pending_ = false;
    bool suspended_ = false;
    std::int64_t dueAt_ = 0;
    bool everFired_ = false;
    std::int64_t lastFire_ = 0;
};

class ResumeWatcher {
public:
    using Clock = std::function<std::int64_t()>;   // milliseconds, steady
    ResumeWatcher(ResumeGateConfig cfg, std::function<void()> action, Clock clock = {});
    ~ResumeWatcher();
    ResumeWatcher(const ResumeWatcher&) = delete;
    ResumeWatcher& operator=(const ResumeWatcher&) = delete;

    // Starts the thread; with registerWithOs the OS notification is registered too (false = events only through inject()).
    // Returns false when asked to register and the OS refused (the thread still runs, for inject()).
    bool start(bool registerWithOs);
    void stop();                            // unregisters, ends the thread; idempotent
    bool osRegistered() const { return registered_.load(); }
    void inject(PowerEvent e);              // the same path the OS callback takes
    std::uint64_t actionsRun() const { return actions_.load(); }
    std::uint64_t eventsSeen() const { return events_.load(); }

private:
    void loop();
    static unsigned long osCallback(void* context, unsigned long type, void* setting);

    ResumeGate gate_;
    std::function<void()> action_;
    Clock clock_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool stopping_ = false;
    bool started_ = false;
    std::thread thread_;
    std::atomic<bool> registered_{false};
    std::atomic<std::uint64_t> actions_{0}, events_{0};
    void* osHandle_ = nullptr;
    void* osParams_ = nullptr;
    friend struct ResumeWatcherAccess;
};

// (the OS callback trampoline's way in; not for callers)
struct ResumeWatcherAccess {
    static unsigned long callback(void* context, unsigned long type, void* setting);
};

}  // namespace cascade::core::sys

#endif  // CASCADE_CORE_SYS_RESUME_WATCH_HPP
