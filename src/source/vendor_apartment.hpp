// vendor_apartment.hpp - what every run-time bridge to a maker's library shares: ONE thread that makes every call into the library, and the ring
// the library's own callback thread writes samples to.
//
// THE APARTMENT. Several makers' documents say a receiver "can be controlled from a single user thread only" and that its handle "can only be used
// in the same thread" (WiNRADiO's G3xDDC documents). FoxSDR's calls come from the GUI, the control thread and the pipeline, so none of them may
// call the library directly: each hands a task to the apartment and waits a BOUNDED time for it. A task that does not finish in time marks the
// apartment dead (every later task is refused at once), the thread is abandoned past its join bound (Recovered::ReaderLeak) and the owner must
// leak the library's module, because the vendor's thread may still be inside it. Every task runs under callGuardingVendorFaults, so an access
// violation inside the vendor's code is a refusal ("Faulted"), not a crash of the application; the apartment is dead afterwards because the
// library's state is no longer guaranteed.
//
// TASKS CAPTURE BY VALUE (or a shared_ptr), never by reference to the caller's stack: a task that finishes after its caller gave up must not write
// to a frame that no longer exists.
//
// THE STREAM LINK. The library calls our callback on a thread of its own with a buffer of 16- or 32-bit signed I/Q; StreamLink::deliver() checks
// the arguments (a count or width that is impossible is dropped and counted, never read), converts to float and writes the ring. read() on the
// pipeline's thread drains it. The gate is closed whenever the stream is not wanted, so a late callback after a stop writes nothing.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#ifndef CASCADE_SOURCE_VENDOR_APARTMENT_HPP
#define CASCADE_SOURCE_VENDOR_APARTMENT_HPP

#include <atomic>
#include <complex>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "dsp/spsc_ring.hpp"

namespace cascade::source::vendorapt {

std::int64_t nowMs();  // steady clock, milliseconds

class Apartment {
public:
    Apartment();
    ~Apartment();
    Apartment(const Apartment&) = delete;
    Apartment& operator=(const Apartment&) = delete;

    enum class Result { Done, TimedOut, Faulted, Dead };

    // Runs `fn` on the apartment's thread and waits up to timeoutMs. `fn` must capture by value.
    Result run(std::function<void()> fn, int timeoutMs);
    bool dead() const { return dead_.load(); }
    // Ends the thread. True when it was joined; false when it was abandoned (a call never returned).
    bool shutdown();

private:
    struct Shared;
    static void loop(std::shared_ptr<Shared> st);
    std::shared_ptr<Shared> st_;
    std::thread thread_;
    std::atomic<bool> dead_{false};
    bool abandoned_ = false;
};

// Shared with the library's callback thread. Heap-allocated by the owner and, when the apartment is abandoned, never freed.
struct StreamLink {
    explicit StreamLink(std::size_t ringPairs = std::size_t{1} << 20) : ring(ringPairs) {}
    std::atomic<bool> gate{false};
    dsp::SpscRing<std::complex<float>> ring;
    std::mutex m;
    std::condition_variable cv;
    std::atomic<std::int64_t> lastDataMs{0};
    std::atomic<std::uint64_t> callbacks{0}, pairs{0}, dropped{0}, bad{0};

    // The library's callback body: `n` I/Q pairs of `bits` (16 or 32) signed integers, interleaved I,Q. Counts the call; drops and counts an
    // impossible argument (null buffer, no samples, more than 1M pairs, a width other than 16 or 32); writes nothing while the gate is closed.
    void deliver(const void* buffer, std::uint32_t n, std::uint32_t bits);
    // A callback the owner refused before it got here (the wrong channel): counted as a call and as bad.
    void countBad();
};

}  // namespace cascade::source::vendorapt

#endif  // CASCADE_SOURCE_VENDOR_APARTMENT_HPP
