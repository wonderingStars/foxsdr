// plugin_start_worker.hpp - THE START WORKER: the one thread that runs every plugin create() and destroy() (freeze diagnosis host design 7 to 10, stage two).
//
// WHY. A create() can take as long as a plugin likes (dvb-receiver 0.4.0's waited for cmd.exe with no limit). Stage one (docs/engine-stage4.md 6.x, PluginRunner's
// unlocked build) stopped the window and the DSP from waiting for it, but the CONTROL thread still sat inside it: commands queued, nothing applied, and after 5 s a
// freeze report. Here every create() and every destroy() runs on this thread instead, so the control pass posts a request and goes on.
//
// ONE, AND SERIAL, for the whole process (not one per runner):
//   - a plugin's create() is never concurrent with a destroy() of the same module, on any radio: the ABI's "owned by one host thread at a time" is kept by there being
//     one thread, as it was by there being one control thread;
//   - a destroy posted before a create of the same plugin runs first (a plugin that owns a port or a device is closed before it is reopened).
//
// A JOB THAT NEVER RETURNS. The thread inside it cannot be stopped. After the abandon time the pump (PluginRunner::pumpStarts, on the control thread) calls
// abandonCurrent(): that thread is parked for good (when the call returns it finds itself abandoned and ends without taking another job), its module stays pinned
// by the containment (it is never unloaded), and a REPLACEMENT thread takes the queue, so other plugins still start.
//
// The state of this object is leaked on purpose (core/leak_on_purpose.hpp): a worker still inside a plugin when main() returns must not read a destroyed queue
// (the exit-time family of signatures 929BF825 and 74EEC22D).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_CORE_PLUGIN_START_WORKER_HPP
#define CASCADE_CORE_PLUGIN_START_WORKER_HPP

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace cascade::core {

class StartWorker {
public:
    struct Job {
        std::function<void()> run;     // the create() or destroy() calls; runs on the worker
        std::uint64_t owner = 0;       // whose job (a PluginRunner's id): drained and counted per owner
        std::string key;               // the plugin's key, for the pump's rows and reports
        bool isCreate = true;          // a create (timed, reported) or a destroy
    };

    // What is running right now.
    struct Current {
        bool busy = false;
        std::uint64_t owner = 0;
        std::string key;
        bool isCreate = true;
        std::int64_t sinceMs = 0;      // how long it has been running, on nowMs()'s clock
    };

    static StartWorker& instance();

    // Queues a job (FIFO). Starts the worker thread on first use. Never blocks on the job.
    void post(Job job);

    // The job in flight, if any.
    Current current() const;

    // Jobs of `owner` queued or in flight.
    std::size_t pendingFor(std::uint64_t owner) const;

    // Waits until `owner` has no queued or running job, at most `bound`. True when drained.
    bool waitDrained(std::uint64_t owner, std::chrono::milliseconds bound);

    // Parks the thread now inside a job for good and starts a replacement for the queue. Returns the key of the job abandoned, empty when none was running or `expectKey`
    // (when not empty) is no longer the running job's. At most kMaxAbandoned per process: past that false, and no replacement (a stuck session gets a sentence instead).
    bool abandonCurrent(const std::string& expectKey);
    std::size_t abandoned() const;
    static constexpr std::size_t kMaxAbandoned = 4;

    // The clock the pump reads (steady milliseconds).
    static std::int64_t nowMs();

    // TESTS ONLY: forget abandoned counts and drop everything queued (no job may be running).
    void resetForTest();

private:
    StartWorker() = default;
};

}  // namespace cascade::core

#endif  // CASCADE_CORE_PLUGIN_START_WORKER_HPP
